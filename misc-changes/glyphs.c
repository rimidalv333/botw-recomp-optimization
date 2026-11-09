// Button icons for the device in use.
//
// BOTW draws its button prompts (HUD hints, menus, dialogs) as characters of its own bitmap font External_00.bffnt
// (Font/Font_*.sbfarc): 51 glyphs at U+E040..E091 in 50x50 cells of one BC4 sheet (A B X Y at E040..E043, L R ZL ZR at
// E044..E047, the d-pad E049 and E060..E065, Home E04A, Plus / Minus E04B / E04C, the sticks' directions and presses
// E050..E05B, the sticks E05D..E05F, then the console, arrows and marks). The prompts' text tags choose those characters at run time, so the sheet is
// what changes, not the texts. nn::font copies the sheet into a texture memory pool of its own; the texture is
// recognised by its size and contents, and on upload its texels are replaced by glyphs drawn here for the device in use,
// in the same cells and inside each original glyph's box (the game's widths stay): keycaps and mouse buttons from the
// keyboard bindings, Xbox or PlayStation buttons through the controller bindings, or the game's own glyphs (moved
// between cells when the controller bindings differ from the defaults). Nothing of Nintendo's is shipped: shapes are
// drawn, labels set in Fira Sans (OFL, third_party/fonts).
//
// The texture goes up again whenever what to show changes (glyphs_generation: device, bindings, setting), so prompts
// already on screen change on the next frame.
//
// SAVAGEWIND_GLYPH_DUMP=1 saves each sheet as uploaded, the original beside it (build/glyph_sheet_<n>.pgm, bottom-up).
#include <math.h>
#include <pthread.h>
#include <sched.h>
#ifdef __linux__
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
extern long syscall(long number, ...);
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "glyphs.h"
#include "text_reconstruct.h"
#ifdef __PROSPERO__
#include "player_font.h"
#include "../storage_ps5.h"
#include <mbedtls/sha256.h>
#endif
#include "../framepace.h"
#include "../input_bind.h"
#include "../paths.h"
#include "../rt.h"
#include "../settings.h"

#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#include "../../third_party/imgui/imstb_truetype.h"

#define FMT_BC4 0x49
#define FMT_R8 0x01
#define CELL 50

// The game's sheet: 256x1024 BC4, 5 x 20 cells of 50x50 (51 apart). nn::font copies it into a texture memory pool of
// its own, so it is recognised by its contents (the same in every region and language).
typedef struct { uint32_t cols, rows, sheet_w, sheet_h; } SheetFont;
static const SheetFont k_sheet = {5, 20, 256, 1024};
#define SHEET_BYTES 0x20000ull
#define SHEET_HASH 0x726127a3b63fbd95ull  // FNV-1a of those bytes (External_00.bffnt from 0x2000, 1.8.2)

// Glyph index of U+E040..E091 (External_00.bffnt's character map), 0xffff: none
static uint16_t index_of(int code) {
    if (code >= 0xE040 && code <= 0xE04C) return (uint16_t)(code - 0xE040);
    if (code >= 0xE050 && code <= 0xE05B) return (uint16_t)(13 + code - 0xE050);
    if (code >= 0xE05D && code <= 0xE067) return (uint16_t)(25 + code - 0xE05D);
    if (code >= 0xE083 && code <= 0xE091) return (uint16_t)(36 + code - 0xE083);
    return 0xffff;
}

static uint64_t fnv1a(const uint8_t *p, uint64_t n) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (uint64_t i = 0; i < n; i++) h = (h ^ p[i]) * 0x100000001b3ull;
    return h;
}

// ---- BC4
static void bc4_decode(const uint8_t *lin, uint32_t wb, uint32_t hb, uint8_t *img) {
    uint32_t w = wb * 4;
    for (uint32_t by = 0; by < hb; by++)
        for (uint32_t bx = 0; bx < wb; bx++) {
            const uint8_t *b = lin + ((uint64_t)by * wb + bx) * 8;
            uint8_t r0 = b[0], r1 = b[1], pal[8] = {r0, r1};
            if (r0 > r1) for (int i = 1; i < 7; i++) pal[i + 1] = (uint8_t)(((7 - i) * r0 + i * r1) / 7);
            else { for (int i = 1; i < 5; i++) pal[i + 1] = (uint8_t)(((5 - i) * r0 + i * r1) / 5); pal[6] = 0, pal[7] = 255; }
            uint64_t bits = 0;
            for (int i = 0; i < 6; i++) bits |= (uint64_t)b[2 + i] << (8 * i);
            for (int i = 0; i < 16; i++) img[(uint64_t)(by * 4 + i / 4) * w + bx * 4 + i % 4] = pal[(bits >> (3 * i)) & 7];
        }
}

typedef struct {
    uint64_t hash;
    uint32_t w, h, format;
    uint16_t cell_w, cell_h, cols, rows;
#ifdef __PROSPERO__
    PlayerFontGlyph *glyphs;
    const char *name;
    uint8_t baseline;
#endif
} TextSheet;
static TextSheet text_sheets[128];
static size_t text_sheet_count;

static uint16_t font_u16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t font_u32(const uint8_t *p) { return (uint32_t)font_u16(p) | (uint32_t)font_u16(p + 2) << 16; }
static uint64_t font_u64(const uint8_t *p) { return font_u32(p) | (uint64_t)font_u32(p + 4) << 32; }
#ifdef __PROSPERO__
static const char *text_sheet_name(unsigned cw, unsigned ch) {
    if (cw == 31 && ch == 39) return "Normal_00";
    if (cw == 24 && ch == 30) return "NormalS_00";
    if (cw == 18 && ch == 22) return "Caption_00";
    return "Special_00";
}

// A CMAP index is global across atlas pages; only Latin codes are replaced.
static PlayerFontGlyph *text_codes(const uint8_t *b, uint64_t n, unsigned page, unsigned cells, unsigned cw) {
    PlayerFontGlyph *glyphs = calloc(cells, sizeof *glyphs);
    if (!glyphs) return NULL;
    for (unsigned i = 0; i < cells; i++) glyphs[i].width = cw;
    uint32_t ptr = font_u32(b + 0x30);
    for (unsigned sections = 0; ptr >= 8 && (uint64_t)ptr + 16 <= n && sections < 4096; sections++) {
        uint64_t off = ptr - 8;
        if (memcmp(b + off, "CMAP", 4)) break;
        uint32_t size = font_u32(b + off + 4);
        if (size < 24 || size > n - off) break;
        uint32_t first = font_u32(b + off + 8), last = font_u32(b + off + 12);
        unsigned method = font_u16(b + off + 16);
        const uint8_t *data = b + off + 24;
        uint32_t len = size - 24;
        for (uint32_t code = 0x20; code <= 0x1eff; code++) {
            if ((code > 0x24f && code < 0x1e00) || code < first || code > last) continue;
            uint32_t index = 0xffff;
            if (method == 0 && len >= 2) index = font_u16(data) + code - first;
            if (method == 1 && (uint64_t)(code - first + 1) * 2 <= len)
                index = font_u16(data + (code - first) * 2);
            if (method == 2 && len >= 4) {
                uint32_t entries = font_u32(data);
                if (entries <= (len - 4) / 8)
                    for (uint32_t j = 0; j < entries; j++)
                        if (font_u32(data + 4 + j * 8) == code) {
                            index = font_u32(data + 8 + j * 8);
                            break;
                        }
            }
            if (index >= page * cells && index - page * cells < cells)
                glyphs[index - page * cells].code = code;
        }
        ptr = font_u32(b + off + 20);
    }
    ptr = font_u32(b + 0x2c);
    for (unsigned sections = 0; ptr >= 8 && (uint64_t)ptr + 8 <= n && sections < 4096; sections++) {
        uint64_t off = ptr - 8;
        if (memcmp(b + off, "CWDH", 4)) break;
        uint32_t size = font_u32(b + off + 4);
        if (size < 16 || size > n - off) break;
        uint32_t first = font_u16(b + off + 8), last = font_u16(b + off + 10);
        if (last < first || (uint64_t)(last - first + 1) * 3 > size - 16) break;
        for (uint32_t index = first; index <= last; index++) {
            if (index < page * cells || index - page * cells >= cells) continue;
            const uint8_t *m = b + off + 16 + (index - first) * 3;
            PlayerFontGlyph *g = &glyphs[index - page * cells];
            g->left = (int8_t)m[0];
            g->width = m[1];
            g->advance = m[2];
        }
        ptr = font_u32(b + off + 12);
    }
    return glyphs;
}
#endif

void glyphs_register_font(const uint8_t *b, uint64_t n) {
    if (n < 0x54 || memcmp(b, "FFNT", 4) || b[4] != 0xff || b[5] != 0xfe) return;
    uint32_t ptr = font_u32(b + 0x28);
    if (ptr < 8 || (uint64_t)ptr + 24 > n) return;
    const uint8_t *t = b + ptr - 8;
    if (memcmp(t, "TGLP", 4)) return;
    uint32_t cw = t[8], ch = t[9], count = t[10], fmt = font_u16(t + 18);
    uint32_t cols = font_u16(t + 20), rows = font_u16(t + 22);
    uint32_t w = font_u16(t + 24), h = font_u16(t + 26), boff = font_u32(t + 28);
    uint32_t format = fmt == 12 ? FMT_BC4 : fmt == 8 ? FMT_R8 : 0;
    if (!format || !cw || !ch || !count || !cols || !rows || !w || !h ||
        (uint64_t)cols * (cw + 1) > w + 1 || (uint64_t)rows * (ch + 1) > h + 1 ||
        (w & 3) || (h & 3) || boff > n || n - boff < 0x30) return;
    const uint8_t *bt = b + boff;
    uint64_t bn = n - boff;
    if (memcmp(bt, "BNTX", 4) || font_u32(bt + 0x24) != 1) return;
    uint64_t table = font_u64(bt + 0x28);
    if (table > bn || bn - table < 8) return;
    uint64_t brti = font_u64(bt + table);
    if (brti > bn || bn - brti < 0x78 || memcmp(bt + brti, "BRTI", 4)) return;
    const uint8_t *r = bt + brti;
    if (font_u32(r + 0x24) != w || font_u32(r + 0x28) != h || font_u32(r + 0x30) < count) return;
    uint64_t offsets = font_u64(r + 0x70);
    if (offsets > bn || bn - offsets < 8) return;
    uint64_t data = font_u64(bt + offsets);
    uint64_t bytes = (uint64_t)w * h / (format == FMT_BC4 ? 2 : 1);
    if (data > bn || bytes * count > bn - data) return;
    for (uint32_t i = 0; i < count && text_sheet_count < sizeof text_sheets / sizeof *text_sheets; i++) {
        uint64_t hash = fnv1a(bt + data + i * bytes, bytes);
        size_t j;
        for (j = 0; j < text_sheet_count; j++)
            if (text_sheets[j].hash == hash && text_sheets[j].w == w && text_sheets[j].h == h &&
                text_sheets[j].format == format) break;
        if (j == text_sheet_count) {
            TextSheet sheet = {hash, w, h, format, cw, ch, cols, rows
#ifdef __PROSPERO__
                               , text_codes(b, n, i, cols * rows, cw), text_sheet_name(cw, ch), b[0x1f]
#endif
            };
            text_sheets[text_sheet_count++] = sheet;
        }
    }
}

// The render thread owns slot creation; workers publish their immutable results with release/acquire.
// A bounded session cache avoids both repeated work and invalidating a result while an upload uses it.
typedef struct {
    uint64_t hash;
    uint32_t w, h, format;
    uint16_t cell_w, cell_h, cols, rows;
    int scale, state;  // 1 working, 2 ready, 3 failed
    uint8_t *input, *output;
#ifdef __PROSPERO__
    const PlayerFontGlyph *glyphs;
    const char *name;
    uint8_t baseline;
#endif
} TextCache;
static TextCache text_cache[32];
#ifdef __PROSPERO__
typedef struct {
    char magic[8];
    uint64_t bytes;
    unsigned char digest[32];
    PlayerFontFit fit;
} FontCacheHeader;

static bool text_font_digest(FILE *f, unsigned char digest[32]) {
    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    int rc = mbedtls_sha256_starts_ret(&ctx, 0);
    uint8_t block[16384];
    size_t n;
    while (!rc && (n = fread(block, 1, sizeof block, f)) != 0)
        rc = mbedtls_sha256_update_ret(&ctx, block, n);
    if (ferror(f)) rc = -1;
    if (!rc) rc = mbedtls_sha256_finish_ret(&ctx, digest);
    mbedtls_sha256_free(&ctx);
    return rc == 0;
}
// These are the first Open Sans build's fitted axes, retained so the guard fix does not retune the look.
static PlayerFontPreset text_preset(const TextCache *c) {
    if (c->cell_w == 31 && c->cell_h == 39) return (PlayerFontPreset){612.5, 98.75, 1};
    if (c->cell_w == 24 && c->cell_h == 30) return (PlayerFontPreset){612.5, 100, 0};
    if (c->cell_w == 18 && c->cell_h == 22) return (PlayerFontPreset){300, 100, 1};
    return (PlayerFontPreset){581.25, 97.5, 0};
}


static void text_bundled_font(TextCache *c, const uint8_t *src, uint8_t *dst) {
    char path[1024];
    if (!rt_resource("fonts/OpenSans.ttf", NULL, path, sizeof path)) return;
    FILE *f = fopen(path, "rb");
    if (!f) return;
    unsigned char digest[32];
    bool ok = text_font_digest(f, digest);
    fclose(f);
    if (!ok) return;
    char hex[65];
    for (int i = 0; i < 32; i++) snprintf(hex + i * 2, 3, "%02x", digest[i]);
    char cache[180];
    snprintf(cache, sizeof cache, "cache/glyphs/v12-%016llx-%s-%d.bin",
             (unsigned long long)c->hash, hex, c->scale);
    size_t bytes = (size_t)c->w * c->h * c->scale * c->scale;
    FontCacheHeader hdr = {0};
    uint8_t *packed = malloc(sizeof hdr + bytes);
    uint64_t started = SDL_GetTicksNS();
    bool cached = false;
    f = rt_data_fopen(cache, "rb");
    if (f && packed && fread(packed, 1, sizeof hdr + bytes, f) == sizeof hdr + bytes && fgetc(f) == EOF) {
        memcpy(&hdr, packed, sizeof hdr);
        unsigned char check[32];
        if (!memcmp(hdr.magic, "SWFNT12", 8) && hdr.bytes == bytes &&
            !mbedtls_sha256_ret(packed + sizeof hdr, bytes, check, 0) && !memcmp(check, hdr.digest, 32)) {
            memcpy(dst, packed + sizeof hdr, bytes);
            cached = true;
        }
    }
    if (f) fclose(f);
    if (!cached) {
        PlayerFontPreset preset = text_preset(c);
        if (!player_font_fit(path, c->glyphs, (size_t)c->cols * c->rows, src, dst, c->w, c->h,
                             c->cell_w, c->cell_h, c->cols, c->rows, c->scale, c->baseline, &preset, &hdr.fit)) {
            free(packed);
            return;
        }
        if (packed && !mbedtls_sha256_ret(dst, bytes, hdr.digest, 0)) {
            memcpy(hdr.magic, "SWFNT12", 8);
            hdr.bytes = bytes;
            memcpy(packed, &hdr, sizeof hdr);
            memcpy(packed + sizeof hdr, dst, bytes);
            if (!rt_storage_mkdirs("cache/glyphs")) rt_storage_replace(cache, packed, sizeof hdr + bytes);
        }
    }
    free(packed);
}
#endif

static void text_low_priority(void) {
#ifdef __PROSPERO__
    struct sched_param p = { .sched_priority = sched_get_priority_min(SCHED_OTHER) };
    if (p.sched_priority >= 0) pthread_setschedparam(pthread_self(), SCHED_OTHER, &p);
#elif defined(__linux__)
    setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), 10);
#endif
}


static void *text_worker(void *arg) {
    TextCache *c = arg;
    text_low_priority();
    uint64_t started = SDL_GetTicksNS();
    uint32_t w = c->w, h = c->h, scale = c->scale;
    uint8_t *src = malloc((uint64_t)w * h);
    uint8_t *dst = malloc((uint64_t)w * h * scale * scale);
    if (!src || !dst) {
        free(src); free(dst); free(c->input);
        __atomic_store_n(&c->state, 3, __ATOMIC_RELEASE);
        return NULL;
    }
    if (c->format == FMT_BC4) bc4_decode(c->input, w / 4, h / 4, src);
    else memcpy(src, c->input, (uint64_t)w * h);
    free(c->input);
    uint32_t ow = w * scale;
    for (uint32_t y = 0; y < h * scale; y++)
        for (uint32_t x = 0; x < ow; x++)
            dst[(uint64_t)y * ow + x] = src[(uint64_t)(y / scale) * w + x / scale];
    text_reconstruct_sheet(src, dst, w, h, c->cell_w, c->cell_h, c->cols, c->rows, scale);
#ifdef __PROSPERO__
    if (c->glyphs && g_settings.sharp_text_font) text_bundled_font(c, src, dst);
#endif
    free(src);
    c->output = dst;
    __atomic_store_n(&c->state, 2, __ATOMIC_RELEASE);
    return NULL;
}

bool glyphs_text_ready(int pending) {
    return pending > 0 && pending <= (int)(sizeof text_cache / sizeof *text_cache) &&
           __atomic_load_n(&text_cache[pending - 1].state, __ATOMIC_ACQUIRE) != 1;
}

bool glyphs_sharp_text(uint64_t addr, uint64_t pool_lo, uint64_t pool_hi, uint32_t format,
                       uint32_t w, uint32_t h, const uint8_t *lin, int scale, bool log_upload,
                       uint8_t **high, int *pending) {
    if ((format != FMT_BC4 && format != FMT_R8) || w < 128 || h < 128) return false;
    bool candidate = false;
    for (size_t j = 0; j < text_sheet_count; j++)
        if (text_sheets[j].w == w && text_sheets[j].h == h && text_sheets[j].format == format) { candidate = true; break; }
    if (!candidate && !log_upload) return false;
    uint64_t bytes = format == FMT_BC4 ? (uint64_t)((w + 3) / 4) * ((h + 3) / 4) * 8 : (uint64_t)w * h;
    if (addr < pool_lo || addr > pool_hi || bytes > pool_hi - addr) return false;
    uint64_t hash = fnv1a((const uint8_t *)(uintptr_t)addr, bytes);
    size_t i = 0;
    for (; i < text_sheet_count; i++)
        if (text_sheets[i].hash == hash && text_sheets[i].w == w &&
            text_sheets[i].h == h && text_sheets[i].format == format) break;
    bool hit = i != text_sheet_count;
    if (!hit) return false;
    if (scale != 2 && scale != 4) return true;
    size_t slot;
    for (slot = 0; slot < sizeof text_cache / sizeof *text_cache; slot++) {
        TextCache *c = &text_cache[slot];
        if (__atomic_load_n(&c->state, __ATOMIC_ACQUIRE) && c->hash == hash && c->scale == scale &&
            c->format == format && c->w == w && c->h == h) break;
    }
    if (slot == sizeof text_cache / sizeof *text_cache) {
        for (slot = 0; slot < sizeof text_cache / sizeof *text_cache && __atomic_load_n(&text_cache[slot].state, __ATOMIC_ACQUIRE); slot++) {}
        if (slot == sizeof text_cache / sizeof *text_cache) return true;
        TextCache *c = &text_cache[slot];
        c->input = malloc(bytes);
        if (!c->input) return true;
        memcpy(c->input, lin, bytes);
        c->hash = hash; c->w = w; c->h = h; c->format = format; c->scale = scale;
        c->cell_w = text_sheets[i].cell_w; c->cell_h = text_sheets[i].cell_h;
        c->cols = text_sheets[i].cols; c->rows = text_sheets[i].rows;
#ifdef __PROSPERO__
        c->glyphs = text_sheets[i].glyphs;
        c->name = text_sheets[i].name;
        c->baseline = text_sheets[i].baseline;
#endif
        __atomic_store_n(&c->state, 1, __ATOMIC_RELEASE);
        pthread_t thread;
        if (pthread_create(&thread, NULL, text_worker, c)) {
            free(c->input);
            __atomic_store_n(&c->state, 3, __ATOMIC_RELEASE);
            return true;
        }
        pthread_detach(thread);
    }
    TextCache *c = &text_cache[slot];
    int state = __atomic_load_n(&c->state, __ATOMIC_ACQUIRE);
    if (state == 2) *high = c->output;
    else if (state == 1) *pending = (int)slot + 1;
    return true;
}

static void bc4_encode(const uint8_t *img, uint32_t wb, uint32_t hb, uint8_t *lin) {
    uint32_t w = wb * 4;
    for (uint32_t by = 0; by < hb; by++)
        for (uint32_t bx = 0; bx < wb; bx++) {
            uint8_t v[16], lo = 255, hi = 0;
            for (int i = 0; i < 16; i++) {
                v[i] = img[(uint64_t)(by * 4 + i / 4) * w + bx * 4 + i % 4];
                if (v[i] < lo) lo = v[i];
                if (v[i] > hi) hi = v[i];
            }
            uint8_t *b = lin + ((uint64_t)by * wb + bx) * 8;
            uint64_t bits = 0;
            if (hi > lo) {  // 8-value mode: r0 = hi > r1 = lo; t = 0 (hi) .. 7 (lo) -> index 0, 2..7, 1
                for (int i = 0; i < 16; i++) {
                    int t = ((hi - v[i]) * 7 + (hi - lo) / 2) / (hi - lo);
                    bits |= (uint64_t)(t == 0 ? 0 : t == 7 ? 1 : t + 1) << (3 * i);
                }
            }
            b[0] = hi, b[1] = lo;
            for (int i = 0; i < 6; i++) b[2 + i] = (uint8_t)(bits >> (8 * i));
        }
}

// ---- the cell canvas: coverage 0..1, cell pixels, y down
#define MAX_GLYPH_SCALE 4
typedef struct { float a[CELL * MAX_GLYPH_SCALE * CELL * MAX_GLYPH_SCALE]; } Canvas;
static int canvas_scale = 1;
#define CANVAS_SIZE (CELL * canvas_scale)
typedef struct { float cx, cy, s, w; } Box;  // the original glyph's centre, height and width

typedef enum { SH_CIRCLE, SH_BOX, SH_SEG, SH_TRI } ShapeType;
typedef struct { ShapeType t; float p[6], r, ring; } Shape;  // ring > 0: only a band that thick on its edge

static float maxf(float a, float b) { return a > b ? a : b; }
static float minf(float a, float b) { return a < b ? a : b; }
static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

static float sd_seg(float px, float py, float ax, float ay, float bx, float by) {
    float dx = bx - ax, dy = by - ay, h = clampf(((px - ax) * dx + (py - ay) * dy) / maxf(dx * dx + dy * dy, 1e-6f), 0, 1);
    return hypotf(px - ax - dx * h, py - ay - dy * h);
}

static float sd(const Shape *s, float x, float y) {
    float d = 0;
    switch (s->t) {
    case SH_CIRCLE: d = hypotf(x - s->p[0], y - s->p[1]) - s->r; break;
    case SH_BOX: {  // centre, half sizes, corner radius r
        float qx = fabsf(x - s->p[0]) - s->p[2] + s->r, qy = fabsf(y - s->p[1]) - s->p[3] + s->r;
        d = hypotf(maxf(qx, 0), maxf(qy, 0)) + minf(maxf(qx, qy), 0) - s->r;
        break;
    }
    case SH_SEG: d = sd_seg(x, y, s->p[0], s->p[1], s->p[2], s->p[3]) - s->r; break;
    case SH_TRI: {  // three corners (any order)
        float e = minf(minf(sd_seg(x, y, s->p[0], s->p[1], s->p[2], s->p[3]), sd_seg(x, y, s->p[2], s->p[3], s->p[4], s->p[5])),
                       sd_seg(x, y, s->p[4], s->p[5], s->p[0], s->p[1]));
        float c1 = (s->p[2] - s->p[0]) * (y - s->p[1]) - (s->p[3] - s->p[1]) * (x - s->p[0]);
        float c2 = (s->p[4] - s->p[2]) * (y - s->p[3]) - (s->p[5] - s->p[3]) * (x - s->p[2]);
        float c3 = (s->p[0] - s->p[4]) * (y - s->p[5]) - (s->p[1] - s->p[5]) * (x - s->p[4]);
        bool in = (c1 >= 0 && c2 >= 0 && c3 >= 0) || (c1 <= 0 && c2 <= 0 && c3 <= 0);
        d = (in ? -e : e) - s->r;
        break;
    }
    }
    return s->ring > 0 ? fabsf(d) - s->ring : d;
}

static float cover(float d) { return clampf(0.5f - d * canvas_scale, 0, 1); }
static void fill(Canvas *c, Shape s) {
    int n = CANVAS_SIZE;
    for (int y = 0; y < n; y++)
        for (int x = 0; x < n; x++)
            c->a[y * n + x] = maxf(c->a[y * n + x], cover(sd(&s, (x + 0.5f) / canvas_scale, (y + 0.5f) / canvas_scale)));
}
static void cut(Canvas *c, Shape s) {
    int n = CANVAS_SIZE;
    for (int y = 0; y < n; y++)
        for (int x = 0; x < n; x++)
            c->a[y * n + x] *= 1 - cover(sd(&s, (x + 0.5f) / canvas_scale, (y + 0.5f) / canvas_scale));
}
static Shape circle(float x, float y, float r) { return (Shape){SH_CIRCLE, {x, y}, r, 0}; }
static Shape box(float x, float y, float hw, float hh, float r) { return (Shape){SH_BOX, {x, y, hw, hh}, r, 0}; }
static Shape seg(float ax, float ay, float bx, float by, float r) { return (Shape){SH_SEG, {ax, ay, bx, by}, r, 0}; }
static Shape tri(float ax, float ay, float bx, float by, float cx, float cy) { return (Shape){SH_TRI, {ax, ay, bx, by, cx, cy}, 0, 0}; }
static Shape ring(Shape s, float t) { s.ring = t; return s; }

// ---- text (Fira Sans SemiBold)
static stbtt_fontinfo g_font;
static int g_font_state;  // 0 not tried, 1 loaded, -1 missing

static bool font_ready(void) {
    if (g_font_state) return g_font_state > 0;
    g_font_state = -1;
    char path[1100];
    if (!rt_resource("FiraSans-SemiBold.ttf", "third_party/fonts/FiraSans/FiraSans-SemiBold.ttf", path, sizeof path)) {
        LOG("[glyphs] Fira Sans not found: button labels left out\n");
        return false;
    }
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *d = malloc((size_t)n);
    if (fread(d, 1, (size_t)n, f) != (size_t)n || !stbtt_InitFont(&g_font, d, stbtt_GetFontOffsetForIndex(d, 0))) {
        fclose(f);
        free(d);
        return false;
    }
    fclose(f);
    g_font_state = 1;  // (d stays: stbtt reads it)
    return true;
}

// The text's capitals cap_h tall (less if it is wider than max_w), centred on (cx, cy): coverage into t
static void text(Canvas *t, const char *s, float cx, float cy, float cap_h, float max_w) {
    cx *= canvas_scale; cy *= canvas_scale; cap_h *= canvas_scale; max_w *= canvas_scale;
    memset(t, 0, sizeof *t);
    if (!font_ready() || !*s) return;
    int cap_y0, cap_y1, x0, x1;  // the height of "H" gives the scale for capitals
    stbtt_GetCodepointBox(&g_font, 'H', &x0, &cap_y0, &x1, &cap_y1);
    float scale = cap_h / (float)(cap_y1 > 0 ? cap_y1 : 700);
    float width = 0;
    for (const char *p = s; *p; p++) {
        int adv, lsb;
        stbtt_GetCodepointHMetrics(&g_font, (unsigned char)*p, &adv, &lsb);
        width += adv * scale;
        if (p[1]) width += stbtt_GetCodepointKernAdvance(&g_font, (unsigned char)*p, (unsigned char)p[1]) * scale;
    }
    if (width > max_w) scale *= max_w / width, width = max_w;
    float pen = cx - width / 2, base = cy + cap_y1 * scale / 2;
    static unsigned char bm[CELL * 4 * CELL * 4];
    for (const char *p = s; *p; p++) {
        int cp = (unsigned char)*p, bx0, by0, bx1, by1, adv, lsb;
        float sx = pen - floorf(pen);
        stbtt_GetCodepointBitmapBoxSubpixel(&g_font, cp, scale, scale, sx, 0, &bx0, &by0, &bx1, &by1);
        int bw = bx1 - bx0, bh = by1 - by0;
        if (bw > 0 && bh > 0 && bw * bh <= (int)sizeof bm) {
            stbtt_MakeCodepointBitmapSubpixel(&g_font, bm, bw, bh, bw, scale, scale, sx, 0, cp);
            int ox = (int)floorf(pen) + bx0, oy = (int)lroundf(base) + by0;
            for (int y = 0; y < bh; y++)
                for (int x = 0; x < bw; x++) {
                    int X = ox + x, Y = oy + y;
                    if (X >= 0 && X < CANVAS_SIZE && Y >= 0 && Y < CANVAS_SIZE)
                        t->a[Y * CANVAS_SIZE + X] = maxf(t->a[Y * CANVAS_SIZE + X], bm[y * bw + x] / 255.0f);
                }
        }
        stbtt_GetCodepointHMetrics(&g_font, cp, &adv, &lsb);
        pen += adv * scale;
        if (p[1]) pen += stbtt_GetCodepointKernAdvance(&g_font, cp, (unsigned char)p[1]) * scale;
    }
}

uint8_t *glyphs_text_line(const char *s, float cap_h, float max_w, int *w, int *h) {
    if (!font_ready() || !*s) return NULL;
    int cap_y0, cap_y1, x0, x1, asc, desc, gap;
    stbtt_GetCodepointBox(&g_font, 'H', &x0, &cap_y0, &x1, &cap_y1);
    stbtt_GetFontVMetrics(&g_font, &asc, &desc, &gap);
    float scale = cap_h / (float)(cap_y1 > 0 ? cap_y1 : 700), width = 0;
    for (const char *p = s; *p; p++) {
        int adv, lsb;
        stbtt_GetCodepointHMetrics(&g_font, (unsigned char)*p, &adv, &lsb);
        width += adv * scale;
        if (p[1]) width += stbtt_GetCodepointKernAdvance(&g_font, (unsigned char)*p, (unsigned char)p[1]) * scale;
    }
    if (width > max_w) scale *= max_w / width, width = max_w;
    int W = (int)ceilf(width) + 4, H = (int)ceilf((float)(asc - desc) * scale) + 4, base = (int)ceilf((float)asc * scale) + 2;
    uint8_t *out = calloc((size_t)W * (size_t)H, 1), *bm = NULL;
    size_t bm_size = 0;
    float pen = 2;
    for (const char *p = s; out && *p; p++) {
        int cp = (unsigned char)*p, bx0, by0, bx1, by1, adv, lsb;
        float sx = pen - floorf(pen);
        stbtt_GetCodepointBitmapBoxSubpixel(&g_font, cp, scale, scale, sx, 0, &bx0, &by0, &bx1, &by1);
        int bw = bx1 - bx0, bh = by1 - by0;
        if (bw > 0 && bh > 0 && (size_t)bw * (size_t)bh > bm_size) {
            free(bm);
            bm_size = (size_t)bw * (size_t)bh;
            bm = malloc(bm_size);
            if (!bm) bm_size = 0;
        }
        if (bw > 0 && bh > 0 && bm) {
            stbtt_MakeCodepointBitmapSubpixel(&g_font, bm, bw, bh, bw, scale, scale, sx, 0, cp);
            int ox = (int)floorf(pen) + bx0, oy = base + by0;  // (glyphs overlap where kerning pulls them together)
            for (int y = 0; y < bh; y++)
                for (int x = 0; x < bw; x++) {
                    int X = ox + x, Y = oy + y;
                    if (X >= 0 && X < W && Y >= 0 && Y < H && bm[y * bw + x] > out[Y * W + X]) out[Y * W + X] = bm[y * bw + x];
                }
        }
        stbtt_GetCodepointHMetrics(&g_font, cp, &adv, &lsb);
        pen += adv * scale;
        if (p[1]) pen += stbtt_GetCodepointKernAdvance(&g_font, cp, (unsigned char)p[1]) * scale;
    }
    free(bm);
    *w = W;
    *h = H;
    return out;
}
static void erase(Canvas *c, const Canvas *t) { for (int i = 0; i < CANVAS_SIZE * CANVAS_SIZE; i++) c->a[i] *= 1 - t->a[i]; }

// ---- icons
enum { DIR_U = 1, DIR_D = 2, DIR_L = 4, DIR_R = 8 };

static void cut_text(Canvas *c, const char *s, float cx, float cy, float cap, float max_w) {
    Canvas t;
    text(&t, s, cx, cy, cap, max_w);
    erase(c, &t);
}

// A filled round button with its letter cut out (Xbox face buttons, stick presses)
static void round_label(Canvas *c, Box b, const char *s) {
    float r = b.s / 2;
    fill(c, circle(b.cx, b.cy, r));
    cut_text(c, s, b.cx, b.cy, b.s * (strlen(s) > 1 ? 0.36f : 0.5f), b.s * 0.72f);
}

// A filled rounded tab with its label cut out (shoulders, triggers, other buttons)
static void tab_label(Canvas *c, Box b, const char *s) {
    float hw = minf(b.w / 2, b.s * 0.56f), hh = b.s * 0.36f;
    fill(c, box(b.cx, b.cy, hw, hh, b.s * 0.14f));
    cut_text(c, s, b.cx, b.cy, b.s * 0.38f, hw * 2 - b.s * 0.18f);
}

// PlayStation face buttons: cross, circle, square, triangle cut out of a filled circle
static void ps_face(Canvas *c, Box b, int host) {
    float r = b.s / 2, t = b.s * 0.06f, k = b.s * 0.2f, x = b.cx, y = b.cy;
    fill(c, circle(x, y, r));
    switch (host) {
    case SDL_GAMEPAD_BUTTON_SOUTH: cut(c, seg(x - k, y - k, x + k, y + k, t)); cut(c, seg(x - k, y + k, x + k, y - k, t)); break;
    case SDL_GAMEPAD_BUTTON_EAST: cut(c, ring(circle(x, y, k * 1.05f), t)); break;
    case SDL_GAMEPAD_BUTTON_WEST: cut(c, ring(box(x, y, k * 0.95f, k * 0.95f, 0), t)); break;
    default: {
        Shape tr = tri(x, y - k * 1.15f, x - k * 1.1f, y + k * 0.8f, x + k * 1.1f, y + k * 0.8f);
        cut(c, ring(tr, t));
        break;
    }
    }
}

// Xbox Menu / View, PlayStation Options / Create
static void menu_button(Canvas *c, Box b, bool start, bool ps) {
    float x = b.cx, y = b.cy, s = b.s, t = s * 0.045f;
    if (ps) fill(c, box(x, y, minf(b.w / 2, s * 0.5f), s * 0.3f, s * 0.3f));
    else fill(c, circle(x, y, s / 2));
    if (start) {  // three lines
        float w = ps ? s * 0.2f : s * 0.19f;
        for (int i = -1; i <= 1; i++) cut(c, seg(x - w, y + i * s * (ps ? 0.11f : 0.14f), x + w, y + i * s * (ps ? 0.11f : 0.14f), t));
    } else if (ps) {  // a small square
        cut(c, ring(box(x, y, s * 0.1f, s * 0.1f, s * 0.02f), t));
    } else {  // two overlapping windows
        cut(c, ring(box(x - s * 0.05f, y - s * 0.05f, s * 0.12f, s * 0.1f, 0), t));
        cut(c, ring(box(x + s * 0.06f, y + s * 0.06f, s * 0.12f, s * 0.1f, 0), t));
    }
}

// A d-pad: the arms in mask filled, the rest outlined
static void dpad(Canvas *c, Box b, int mask) {
    float x = b.cx, y = b.cy, L = b.s * 0.48f, w = b.s * 0.17f, t = b.s * 0.045f, r = b.s * 0.05f;
    Shape h = box(x, y, L, w, r), v = box(x, y, w, L, r);
    if (mask == (DIR_U | DIR_D | DIR_L | DIR_R)) {
        fill(c, h);
        fill(c, v);
        cut(c, circle(x, y, w * 0.55f));
        return;
    }
    for (int py = 0; py < CANVAS_SIZE; py++)
        for (int px = 0; px < CANVAS_SIZE; px++) {
            float X = (px + 0.5f) / canvas_scale, Y = (py + 0.5f) / canvas_scale;
            float d = fabsf(minf(sd(&h, X, Y), sd(&v, X, Y))) - t;
            c->a[py * CANVAS_SIZE + px] = maxf(c->a[py * CANVAS_SIZE + px], cover(d));
        }
    if (mask & DIR_U) fill(c, box(x, y - L / 2 - w / 2, w, L / 2 - w / 2 + r, r));
    if (mask & DIR_D) fill(c, box(x, y + L / 2 + w / 2, w, L / 2 - w / 2 + r, r));
    if (mask & DIR_L) fill(c, box(x - L / 2 - w / 2, y, L / 2 - w / 2 + r, w, r));
    if (mask & DIR_R) fill(c, box(x + L / 2 + w / 2, y, L / 2 - w / 2 + r, w, r));
}

static Shape arrow(float x, float y, float k, int dir) {  // a triangle pointing dir, k: its half size
    switch (dir) {
    case DIR_U: return tri(x, y - k, x - k, y + k * 0.7f, x + k, y + k * 0.7f);
    case DIR_D: return tri(x, y + k, x - k, y - k * 0.7f, x + k, y - k * 0.7f);
    case DIR_L: return tri(x - k, y, x + k * 0.7f, y - k, x + k * 0.7f, y + k);
    default: return tri(x + k, y, x - k * 0.7f, y - k, x - k * 0.7f, y + k);
    }
}

// Analog stick with directional indicators. Nintendo mode retains the game's own art.
static void stick(Canvas *c, Box b, int dirs, bool right) {
    float s = minf(b.s, b.w), x = b.cx, y = b.cy, r = s * 0.27f;
    fill(c, circle(x, y, r));
    cut_text(c, right ? "R" : "L", x, y, s * 0.28f, s * 0.36f);
    float k = s * 0.085f, gap = s * 0.39f;
    if (dirs & DIR_U) fill(c, arrow(x, y - gap, k, DIR_U));
    if (dirs & DIR_D) fill(c, arrow(x, y + gap, k, DIR_D));
    if (dirs & DIR_L) fill(c, arrow(x - gap, y, k, DIR_L));
    if (dirs & DIR_R) fill(c, arrow(x + gap, y, k, DIR_R));
}

// A keycap with its label (or arrows: dirs) cut out; wide labels get a wider key
static void keycap(Canvas *c, Box b, const char *label, int dirs) {
    float hh = b.s * 0.42f, hw = (label && strlen(label) > 1) ? minf(b.w / 2, b.s * 0.6f) : hh;
    if (!label && dirs && (dirs & (dirs - 1))) hw = minf(b.w / 2, b.s * 0.6f);
    fill(c, box(b.cx, b.cy, hw, hh, b.s * 0.12f));
    if (label) {
        cut_text(c, label, b.cx, b.cy, b.s * (strlen(label) > 1 ? 0.34f : 0.46f), hw * 2 - b.s * 0.16f);
        return;
    }
    int n = 0, d[4];
    for (int i = 0; i < 4; i++) if (dirs & (1 << i)) d[n++] = 1 << i;
    float k = b.s * (n == 1 ? 0.17f : 0.12f);
    if (n == 4) {  // the arrow cluster
        cut(c, arrow(b.cx, b.cy - k * 1.6f, k * 0.8f, DIR_U));
        cut(c, arrow(b.cx, b.cy + k * 1.6f, k * 0.8f, DIR_D));
        cut(c, arrow(b.cx - k * 1.9f, b.cy, k * 0.8f, DIR_L));
        cut(c, arrow(b.cx + k * 1.9f, b.cy, k * 0.8f, DIR_R));
    } else {
        for (int i = 0; i < n; i++) cut(c, arrow(b.cx + (n > 1 ? (i ? 1 : -1) * k * 1.5f : 0), b.cy, k, d[i]));
    }
}

// A mouse: button 1 left, 2 middle, 3 right, 4 / 5 the side buttons; 0 none. dirs: arrows around it (movement)
static void mouse(Canvas *c, Box b, int button, int dirs) {
    float sc = dirs ? 0.7f : 1, x = b.cx, y = b.cy, hw = b.s * 0.29f * sc, hh = b.s * 0.45f * sc, t = b.s * 0.05f;
    float split = y - hh * 0.15f;
    Shape body = box(x, y, hw, hh, hw);
    fill(c, ring(body, t));
    fill(c, seg(x - hw, split, x + hw, split, t * 0.8f));
    fill(c, seg(x, y - hh, x, split, t * 0.8f));
    for (int py = 0; py < CANVAS_SIZE; py++)
        for (int px = 0; px < CANVAS_SIZE; px++) {
            float X = (px + 0.5f) / canvas_scale, Y = (py + 0.5f) / canvas_scale, d = sd(&body, X, Y);
            if (button == 1) d = maxf(d, maxf(X - x, Y - split));
            else if (button == 3) d = maxf(d, maxf(x - X, Y - split));
            else if (button == 4 || button == 5) d = maxf(d, maxf(X - (x - hw * 0.3f), fabsf(Y - (y + hh * (button == 4 ? 0.2f : 0.55f))) - hh * 0.14f));
            else continue;
            c->a[py * CANVAS_SIZE + px] = maxf(c->a[py * CANVAS_SIZE + px], cover(d));
        }
    if (button == 2) fill(c, seg(x, y - hh * 0.62f, x, y - hh * 0.32f, hw * 0.32f));
    float k = b.s * 0.09f, g = b.s * 0.08f;
    if (dirs & DIR_U) fill(c, arrow(x, y - hh - g, k, DIR_U));
    if (dirs & DIR_D) fill(c, arrow(x, y + hh + g, k, DIR_D));
    if (dirs & DIR_L) fill(c, arrow(x - hw - g * 1.4f, y, k, DIR_L));
    if (dirs & DIR_R) fill(c, arrow(x + hw + g * 1.4f, y, k, DIR_R));
}

// ---- what each glyph shows
enum { G_KEEP, G_BUTTON, G_DPAD, G_LSTICK, G_RSTICK };
typedef struct { uint8_t kind, arg; } Meaning;

static Meaning meaning(int code) {
    switch (code) {
    case 0xE040: case 0xE041: case 0xE042: case 0xE043: return (Meaning){G_BUTTON, (uint8_t)(BIND_A + code - 0xE040)};
    case 0xE044: return (Meaning){G_BUTTON, BIND_L};
    case 0xE045: return (Meaning){G_BUTTON, BIND_R};
    case 0xE046: return (Meaning){G_BUTTON, BIND_ZL};
    case 0xE047: return (Meaning){G_BUTTON, BIND_ZR};
    case 0xE04B: return (Meaning){G_BUTTON, BIND_PLUS};
    case 0xE04C: return (Meaning){G_BUTTON, BIND_MINUS};
    case 0xE056: return (Meaning){G_BUTTON, BIND_LSTICK};
    case 0xE057: return (Meaning){G_BUTTON, BIND_RSTICK};
    case 0xE049: return (Meaning){G_DPAD, DIR_U | DIR_D | DIR_L | DIR_R};
    case 0xE060: return (Meaning){G_DPAD, DIR_U};
    case 0xE061: return (Meaning){G_DPAD, DIR_D};
    case 0xE062: return (Meaning){G_DPAD, DIR_L};
    case 0xE063: return (Meaning){G_DPAD, DIR_R};
    case 0xE064: return (Meaning){G_DPAD, DIR_U | DIR_D};
    case 0xE065: return (Meaning){G_DPAD, DIR_L | DIR_R};
    case 0xE05E: return (Meaning){G_LSTICK, DIR_U | DIR_D | DIR_L | DIR_R};
    case 0xE052: return (Meaning){G_LSTICK, DIR_U};
    case 0xE050: return (Meaning){G_LSTICK, DIR_D};
    case 0xE054: return (Meaning){G_LSTICK, DIR_L | DIR_R};
    case 0xE058: return (Meaning){G_LSTICK, DIR_R};
    case 0xE05A: return (Meaning){G_LSTICK, DIR_L};
    case 0xE05F: return (Meaning){G_RSTICK, DIR_U | DIR_D | DIR_L | DIR_R};
    case 0xE05D: return (Meaning){G_RSTICK, DIR_U | DIR_D};
    case 0xE053: return (Meaning){G_RSTICK, DIR_U};
    case 0xE051: return (Meaning){G_RSTICK, DIR_D};
    case 0xE055: return (Meaning){G_RSTICK, DIR_L | DIR_R};
    case 0xE059: return (Meaning){G_RSTICK, DIR_R};
    case 0xE05B: return (Meaning){G_RSTICK, DIR_L};
    default: return (Meaning){G_KEEP, 0};  // (the power button, Home, the console, arrows and marks)
    }
}

static const int k_dpad_slot[4] = {BIND_UP, BIND_DOWN, BIND_LEFT, BIND_RIGHT};  // (DIR_U, DIR_D, DIR_L, DIR_R order)
static const int k_dpad_host[4] = {SDL_GAMEPAD_BUTTON_DPAD_UP, SDL_GAMEPAD_BUTTON_DPAD_DOWN, SDL_GAMEPAD_BUTTON_DPAD_LEFT,
                                   SDL_GAMEPAD_BUTTON_DPAD_RIGHT};
static int dir_index(int dir) { return dir == DIR_U ? 0 : dir == DIR_D ? 1 : dir == DIR_L ? 2 : 3; }

// The game's own glyph for a controller button by position (Nintendo names), 0: none
static int nintendo_code(int host) {
    switch (host) {
    case SDL_GAMEPAD_BUTTON_EAST: return 0xE040;
    case SDL_GAMEPAD_BUTTON_SOUTH: return 0xE041;
    case SDL_GAMEPAD_BUTTON_NORTH: return 0xE042;
    case SDL_GAMEPAD_BUTTON_WEST: return 0xE043;
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: return 0xE044;
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return 0xE045;
    case PADB_LTRIGGER: return 0xE046;
    case PADB_RTRIGGER: return 0xE047;
    case SDL_GAMEPAD_BUTTON_START: return 0xE04B;
    case SDL_GAMEPAD_BUTTON_BACK: return 0xE04C;
    case SDL_GAMEPAD_BUTTON_LEFT_STICK: return 0xE056;
    case SDL_GAMEPAD_BUTTON_RIGHT_STICK: return 0xE057;
    case SDL_GAMEPAD_BUTTON_DPAD_UP: return 0xE060;
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return 0xE061;
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return 0xE062;
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return 0xE063;
    case SDL_GAMEPAD_BUTTON_GUIDE: return 0xE04A;
    default: return 0;
    }
}

// A controller button as an Xbox / PlayStation icon
static void pad_button(Canvas *c, Box b, int host, bool ps) {
    switch (host) {
    case SDL_GAMEPAD_BUTTON_SOUTH: case SDL_GAMEPAD_BUTTON_EAST: case SDL_GAMEPAD_BUTTON_WEST: case SDL_GAMEPAD_BUTTON_NORTH:
        if (ps) ps_face(c, b, host);
        else round_label(c, b, host == SDL_GAMEPAD_BUTTON_SOUTH ? "A" : host == SDL_GAMEPAD_BUTTON_EAST ? "B" :
                               host == SDL_GAMEPAD_BUTTON_WEST ? "X" : "Y");
        return;
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: tab_label(c, b, ps ? "L1" : "LB"); return;
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: tab_label(c, b, ps ? "R1" : "RB"); return;
    case PADB_LTRIGGER: tab_label(c, b, ps ? "L2" : "LT"); return;
    case PADB_RTRIGGER: tab_label(c, b, ps ? "R2" : "RT"); return;
    case SDL_GAMEPAD_BUTTON_START: menu_button(c, b, true, ps); return;
    case SDL_GAMEPAD_BUTTON_BACK: menu_button(c, b, false, ps); return;
    case SDL_GAMEPAD_BUTTON_TOUCHPAD:
        if (ps) {
            float hw = minf(b.w * 0.46f, b.s * 0.60f);
            fill(c, ring(box(b.cx, b.cy, hw, b.s * 0.28f, b.s * 0.10f), b.s * 0.06f));
        } else {
            tab_label(c, b, bind_pad_label(host, 1));
        }
        return;
    case SDL_GAMEPAD_BUTTON_LEFT_STICK: round_label(c, b, ps ? "L3" : "LS"); return;
    case SDL_GAMEPAD_BUTTON_RIGHT_STICK: round_label(c, b, ps ? "R3" : "RS"); return;
    case SDL_GAMEPAD_BUTTON_DPAD_UP: dpad(c, b, DIR_U); return;
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN: dpad(c, b, DIR_D); return;
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT: dpad(c, b, DIR_L); return;
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: dpad(c, b, DIR_R); return;
    default: tab_label(c, b, bind_pad_label(host, ps ? 2 : 1)); return;
    }
}

// A key's short label ("Ctrl", "Space"), NULL for the arrow keys (*dir set)
static const char *key_short(int sc, int *dir) {
    static char buf[16];
    *dir = 0;
    switch (sc) {
    case SDL_SCANCODE_UP: *dir = DIR_U; return NULL;
    case SDL_SCANCODE_DOWN: *dir = DIR_D; return NULL;
    case SDL_SCANCODE_LEFT: *dir = DIR_L; return NULL;
    case SDL_SCANCODE_RIGHT: *dir = DIR_R; return NULL;
    case SDL_SCANCODE_LSHIFT: case SDL_SCANCODE_RSHIFT: return "Shift";
    case SDL_SCANCODE_LCTRL: case SDL_SCANCODE_RCTRL: return "Ctrl";
    case SDL_SCANCODE_LALT: case SDL_SCANCODE_RALT: return "Alt";
    case SDL_SCANCODE_LGUI: case SDL_SCANCODE_RGUI: return "Win";
    case SDL_SCANCODE_RETURN: case SDL_SCANCODE_KP_ENTER: return "Enter";
    case SDL_SCANCODE_ESCAPE: return "Esc";
    case SDL_SCANCODE_BACKSPACE: return "Bksp";
    case SDL_SCANCODE_CAPSLOCK: return "Caps";
    case SDL_SCANCODE_DELETE: return "Del";
    case SDL_SCANCODE_INSERT: return "Ins";
    case SDL_SCANCODE_PAGEUP: return "PgUp";
    case SDL_SCANCODE_PAGEDOWN: return "PgDn";
    case SDL_SCANCODE_SPACE: return "Space";
    default: break;
    }
    const char *n = sc > 0 && sc < SDL_SCANCODE_COUNT ? SDL_GetScancodeName((SDL_Scancode)sc) : "";
    if (!strncmp(n, "Keypad ", 7)) { snprintf(buf, sizeof buf, "Num%s", n + 7); return buf; }
    snprintf(buf, sizeof buf, "%s", n && *n ? n : "?");
    return buf;
}

// A keyboard binding (key or mouse button) as an icon
static void key_icon(Canvas *c, Box b, int sc) {
    if (sc > BIND_MOUSE && sc <= BIND_MOUSE_LAST) { mouse(c, b, sc - BIND_MOUSE, 0); return; }
    int dir;
    const char *l = key_short(sc, &dir);
    keycap(c, b, l, dir);
}

// Several keys (a stick's directions) on one keycap: arrows, or single-character labels side by side
static void keys_icon(Canvas *c, Box b, const int *slots, int dirs) {
    char label[16] = "";
    int arrows = 0, n = 0;
    bool letters = true;
    for (int i = 0; i < 4; i++) {
        if (!(dirs & (1 << i))) continue;
        int sc = g_bind_key[slots[i]], dir;
        if (sc > BIND_MOUSE && sc <= BIND_MOUSE_LAST) { letters = false; continue; }
        const char *l = key_short(sc, &dir);
        if (!l) arrows |= dir;
        else if (strlen(l) == 1 && n < 6) label[n++] = l[0];
        else letters = false;
    }
    if (arrows == dirs) { keycap(c, b, NULL, dirs); return; }
    if (letters && n && !arrows) {
        if (n == 4) {  // up left down right: "WASD"
            char wasd[5] = {label[0], label[2], label[1], label[3], 0};
            keycap(c, b, wasd, 0);
            return;
        }
        keycap(c, b, label, 0);
        return;
    }
    keycap(c, b, NULL, dirs);  // (a mix: show the directions)
}

static int resolve_kind(void) {
    int k = g_settings.button_icons;
    if (k > ICONS_AUTO && k < ICONS_COUNT) return k;
    if (input_last_device() == DEVICE_KEYBOARD) return ICONS_KEYBOARD;
    int f = input_pad_family();
    return f == 1 ? ICONS_XBOX : f == 2 ? ICONS_PLAYSTATION : ICONS_NINTENDO;
}

uint32_t glyphs_generation(void) {
    int k = resolve_kind();
    uint32_t h = 2166136261u ^ (uint32_t)k;
    if (k == ICONS_KEYBOARD) {
        for (int i = 0; i < BIND_KEY_COUNT; i++) h = (h ^ (uint16_t)g_bind_key[i]) * 16777619u;
        h = (h ^ g_settings.mouse_camera) * 16777619u;
    } else {
        for (int i = 0; i < BIND_PAD_COUNT; i++) h = (h ^ (uint16_t)g_bind_pad[i]) * 16777619u;
    }
    h = (h ^ (uint32_t)g_settings.sharp_text) * 16777619u;
    return h | 1;
}

static const char *const k_kind_names[ICONS_COUNT] = {"auto", "keyboard", "Xbox", "PlayStation", "Nintendo"};

// The icon for one glyph: false = keep the game's (or *copy: the game's glyph for that code instead)
static bool draw_glyph(Canvas *c, Box b, int code, int kind, int *copy) {
    Meaning m = meaning(code);
    *copy = 0;
    if (m.kind == G_KEEP) return false;
    bool ps = kind == ICONS_PLAYSTATION;
    if (kind == ICONS_NINTENDO) {  // the game's glyphs, following the controller bindings
        int host = -1;
        if (m.kind == G_BUTTON) host = g_bind_pad[m.arg];
        else if (m.kind == G_DPAD && !(m.arg & (m.arg - 1))) host = g_bind_pad[k_dpad_slot[dir_index(m.arg)]];
        int want = host >= 0 ? nintendo_code(host) : 0;
        if (want && want != code) *copy = want;
        return false;
    }
    if (kind == ICONS_KEYBOARD) {
        switch (m.kind) {
        case G_BUTTON: key_icon(c, b, g_bind_key[m.arg]); return true;
        case G_DPAD:
            if (!(m.arg & (m.arg - 1))) key_icon(c, b, g_bind_key[k_dpad_slot[dir_index(m.arg)]]);
            else keys_icon(c, b, k_dpad_slot, m.arg);
            return true;
        case G_LSTICK: {
            static const int ls[4] = {BIND_LS_UP, BIND_LS_DOWN, BIND_LS_LEFT, BIND_LS_RIGHT};
            if (!(m.arg & (m.arg - 1))) key_icon(c, b, g_bind_key[ls[dir_index(m.arg)]]);
            else keys_icon(c, b, ls, m.arg);
            return true;
        }
        default: {  // the right stick: the mouse when it turns the camera, else its keys
            static const int rs[4] = {BIND_RS_UP, BIND_RS_DOWN, BIND_RS_LEFT, BIND_RS_RIGHT};
            if (g_settings.mouse_camera) mouse(c, b, 0, m.arg);
            else if (!(m.arg & (m.arg - 1))) key_icon(c, b, g_bind_key[rs[dir_index(m.arg)]]);
            else keys_icon(c, b, rs, m.arg);
            return true;
        }
        }
    }
    // Xbox / PlayStation: buttons through the bindings, sticks drawn at the target resolution.
    switch (m.kind) {
    case G_BUTTON: pad_button(c, b, g_bind_pad[m.arg], ps); return true;
    case G_DPAD:
        if (!(m.arg & (m.arg - 1))) {
            int host = g_bind_pad[k_dpad_slot[dir_index(m.arg)]];
            if (host != k_dpad_host[dir_index(m.arg)]) { pad_button(c, b, host, ps); return true; }
        }
        dpad(c, b, m.arg);
        return true;
    case G_LSTICK: case G_RSTICK: stick(c, b, m.arg, m.kind == G_RSTICK); return true;
    default: return false;
    }
}

// Where glyph index i's cell is in the uploaded sheet: its top-left pixel and whether rows run bottom-up
static void cell_origin(const SheetFont *f, uint32_t i, int *x0, int *y0) {
    *x0 = (int)(i % f->cols) * (CELL + 1);
    *y0 = (int)(i / f->cols) * (CELL + 1);
}
static int sheet_y(const SheetFont *f, int y0, int y) { return (int)f->sheet_h - 1 - (y0 + y); }  // (stored bottom-up)

bool glyphs_apply(uint64_t addr, uint64_t pool_lo, uint64_t pool_hi, uint32_t format, uint32_t w, uint32_t h,
                  uint8_t *lin, uint32_t wb, uint32_t hb, int scale, uint8_t **high) {
    const SheetFont *f = &k_sheet;
    if (format != FMT_BC4 || w != f->sheet_w || h != f->sheet_h || w != wb * 4 || h != hb * 4) return false;
    if (addr < pool_lo || addr + SHEET_BYTES > pool_hi) return false;
    uint64_t hash = fnv1a((const uint8_t *)(uintptr_t)addr, SHEET_BYTES);
    if (hash != SHEET_HASH) {
        static int nlog;
        if (nlog++ < 4) LOG("[glyphs] 256x1024 BC4 texture %llx is not the button glyph sheet (hash %016llx)\n",
                            (unsigned long long)addr, (unsigned long long)hash);
        return false;
    }
    int kind = resolve_kind();
    if (scale != 2 && scale != 4) scale = 1;
    uint8_t *orig = malloc((uint64_t)w * h), *img = malloc((uint64_t)w * h * scale * scale);
    if (!img && scale > 1) { scale = 1; img = malloc((uint64_t)w * h); }
    if (!orig || !img) { free(orig); free(img); return false; }
    bc4_decode(lin, wb, hb, orig);
    uint32_t ow = w * scale, oh = h * scale;
    for (uint32_t y = 0; y < oh; y++)
        for (uint32_t x = 0; x < ow; x++) img[(uint64_t)y * ow + x] = orig[(uint64_t)(y / scale) * w + x / scale];
    canvas_scale = scale;
    int changed = 0;
    for (int code = 0xE040; code < 0xE0A0; code++) {
        uint32_t gi = index_of(code);
        if (gi == 0xffff) continue;
        int x0, y0;
        cell_origin(f, gi, &x0, &y0);
        if (x0 + CELL > (int)w || y0 + CELL > (int)h) continue;
        // the original glyph's box (its lit pixels)
        int bx0 = CELL, by0 = CELL, bx1 = -1, by1 = -1;
        for (int y = 0; y < CELL; y++)
            for (int x = 0; x < CELL; x++)
                if (orig[(uint64_t)sheet_y(f, y0, y) * w + x0 + x] > 96) {
                    if (x < bx0) bx0 = x;
                    if (x > bx1) bx1 = x;
                    if (y < by0) by0 = y;
                    if (y > by1) by1 = y;
                }
        if (bx1 < 0) bx0 = 4, by0 = 6, bx1 = 45, by1 = 45;
        Box b = {(bx0 + bx1 + 1) / 2.0f, (by0 + by1 + 1) / 2.0f, (float)(by1 - by0 + 1), (float)(bx1 - bx0 + 1)};
        static Canvas cv;
        memset(&cv, 0, sizeof cv);
        int copy = 0;
        bool drawn = draw_glyph(&cv, b, code, kind, &copy);
        if (copy && index_of(copy) != 0xffff) {  // the game's glyph from another cell
            int sx0, sy0;
            cell_origin(f, index_of(copy), &sx0, &sy0);
            for (int y = 0; y < CELL * scale; y++)
                for (int x = 0; x < CELL * scale; x++)
                    img[(uint64_t)(oh - 1 - (y0 * scale + y)) * ow + x0 * scale + x] =
                        orig[(uint64_t)sheet_y(f, sy0, y / scale) * w + sx0 + x / scale];
            changed++;
        } else if (drawn) {
            for (int y = 0; y < CELL * scale; y++)
                for (int x = 0; x < CELL * scale; x++)
                    img[(uint64_t)(oh - 1 - (y0 * scale + y)) * ow + x0 * scale + x] =
                        (uint8_t)lroundf(cv.a[y * CANVAS_SIZE + x] * 255);
            changed++;
        }
    }
    canvas_scale = 1;
    if (scale == 1 && changed) bc4_encode(img, wb, hb, lin);
    static int nlog;
    if (nlog++ < 16)
        LOG("[glyphs] button glyph sheet %llx: %s icons, %d glyphs redrawn\n", (unsigned long long)addr,
            k_kind_names[kind], changed);
    if (scale == 1 && getenv("SAVAGEWIND_GLYPH_DUMP")) {
        static int n;
        char path[128];
        snprintf(path, sizeof path, "build/glyph_sheet_%d.pgm", ++n);
        FILE *fp = n <= 16 ? fopen(path, "wb") : NULL;
        if (fp) {  // (as stored: bottom-up) and the original before it
            fprintf(fp, "P5\n%u %u\n255\n", w * 2, h);
            for (uint32_t y = 0; y < h; y++) {
                fwrite(orig + (uint64_t)y * w, 1, w, fp);
                fwrite(img + (uint64_t)y * w, 1, w, fp);
            }
            fclose(fp);
        }
    }
    free(orig);
    if (scale > 1) *high = img; else free(img);
    return true;
}

// Some prompts are not text: the HUD draws them with textures of its own layouts (Layout/Common.sblarc): the action
// button ("Pick up", "Talk"...), the paraglider's button with its diamond of the others, the d-pad of the quick menus.
// R8 coverage masks, a button glyph each, recognised by size and contents (as uploaded, deswizzled). The dim letters
// of that diamond are text: runtime/services/fs.c (hud_letters_patch) makes them icon characters of this sheet.
typedef struct { uint16_t w, h; uint64_t hash; int code; } LayoutIcon;
static const LayoutIcon k_layout_icons[] = {
    {48, 48, 0x3cf281fb77fe6e1full, 0xE040},  // Nt_KeyTexA_00
    {48, 48, 0x5c11641d8572ef3eull, 0xE041},  // Nt_KeyTexB_00
    {48, 48, 0x80ac73dec7998dc2ull, 0xE042},  // Nt_KeyTexX_00
    {48, 48, 0x4d2a26b5991ee454ull, 0xE043},  // Nt_KeyTexY_00
    {48, 48, 0x01ba4a1c31788ef6ull, 0xE044},  // Nt_KeyTexL_00
    {48, 48, 0xc85329ba2d18b0cbull, 0xE046},  // Nt_KeyTexZL_00
    {48, 48, 0xc3719621a38a3959ull, 0xE05E},  // Nt_KeyTexLSt_01: the left stick
    {48, 48, 0x0d90f7505ab43e59ull, 0xE052},  // Nt_KeyTexLSt_00: the left stick, up
    {46, 46, 0xde6c79012595283cull, 0xE049},  // ButtonIcon46_00: the d-pad
    {64, 64, 0xc171fee97aa2e0b1ull, 0xE049},  // ButtonIconL46NN_00: the d-pad (the shortcuts at the top left)
};

static const LayoutIcon *layout_icon(uint32_t format, uint32_t w, uint32_t h, const uint8_t *lin, uint32_t row) {
    if (format != FMT_R8 || w > 2 * CELL || h > 2 * CELL || row < w) return false;
    const LayoutIcon *li = NULL;
    for (size_t i = 0; i < sizeof k_layout_icons / sizeof *k_layout_icons && !li; i++)
        if (k_layout_icons[i].w == w && k_layout_icons[i].h == h) {
            uint64_t hash = 0xcbf29ce484222325ull;
            for (uint32_t y = 0; y < h; y++)
                for (uint32_t x = 0; x < w; x++) hash = (hash ^ lin[(uint64_t)y * row + x]) * 0x100000001b3ull;
            for (size_t k = i; k < sizeof k_layout_icons / sizeof *k_layout_icons; k++)
                if (k_layout_icons[k].w == w && k_layout_icons[k].h == h && k_layout_icons[k].hash == hash) li = &k_layout_icons[k];
            break;
        }
    return li;
}

bool glyphs_layout_match(uint32_t format, uint32_t w, uint32_t h, const uint8_t *lin, uint32_t row) {
    return layout_icon(format, w, h, lin, row) != NULL;
}

bool glyphs_apply_layout(uint32_t format, uint32_t w, uint32_t h, uint8_t *lin, uint32_t row, int scale, uint8_t **high) {
    const LayoutIcon *li = layout_icon(format, w, h, lin, row);
    if (!li) return false;
    if (scale != 2 && scale != 4) scale = 1;
    uint8_t *out = scale > 1 ? malloc((uint64_t)w * h * scale * scale) : lin;
    if (!out) scale = 1, out = lin;
    if (scale > 1)
        for (uint32_t y = 0; y < h * scale; y++)
            for (uint32_t x = 0; x < w * scale; x++)
                out[(uint64_t)y * w * scale + x] = lin[(uint64_t)(y / scale) * row + x / scale];
    // the icon's box (its lit pixels), in a cell canvas with the texture centred in it (scaled down to fit)
    float k = w > CELL || h > CELL ? (float)CELL / (float)(w > h ? w : h) : 1;
    float ox = (CELL - w * k) / 2, oy = (CELL - h * k) / 2;
    int bx0 = (int)w, by0 = (int)h, bx1 = -1, by1 = -1;
    for (int y = 0; y < (int)h; y++)
        for (int x = 0; x < (int)w; x++)
            if (lin[(uint64_t)y * row + x] > 96) {
                if (x < bx0) bx0 = x;
                if (x > bx1) bx1 = x;
                if (y < by0) by0 = y;
                if (y > by1) by1 = y;
            }
    if (bx1 < 0) { if (scale > 1) *high = out; return true; }
    Box b = {ox + (bx0 + bx1 + 1) * k / 2, oy + (by0 + by1 + 1) * k / 2, (by1 - by0 + 1) * k, (bx1 - bx0 + 1) * k};
    static Canvas cv;
    memset(&cv, 0, sizeof cv);
    canvas_scale = scale;
    int kind = resolve_kind(), copy = 0;
    bool drawn = draw_glyph(&cv, b, li->code, kind, &copy);  // (Nintendo: the game's own, even with other bindings)
    if (drawn)
        for (int y = 0; y < (int)h * scale; y++)
            for (int x = 0; x < (int)w * scale; x++) {
                int n = CANVAS_SIZE;
                float fx = clampf(ox * scale + (x + 0.5f) * k - 0.5f, 0, n - 1);
                float fy = clampf(oy * scale + (y + 0.5f) * k - 0.5f, 0, n - 1);
                int x0 = (int)fx, y0 = (int)fy, x1 = x0 + 1 < n ? x0 + 1 : x0, y1 = y0 + 1 < n ? y0 + 1 : y0;
                float tx = fx - x0, ty = fy - y0;
                float a = (cv.a[y0 * n + x0] * (1 - tx) + cv.a[y0 * n + x1] * tx) * (1 - ty) +
                          (cv.a[y1 * n + x0] * (1 - tx) + cv.a[y1 * n + x1] * tx) * ty;
                out[(uint64_t)y * (scale > 1 ? w * scale : row) + x] = (uint8_t)lroundf(a * 255);
            }
    canvas_scale = 1;
    if (scale > 1) *high = out;
    static int nlog;
    if (nlog++ < 16) LOG("[glyphs] layout icon %04X (%ux%u): %s icons%s\n", li->code, w, h, k_kind_names[kind], drawn ? "" : ", kept");
    return true;
}
