// PS5 native software keyboard for nn::swkbd::ShowKeyboard.
// Uses libSceImeDialog: the system draws the keyboard and takes DualSense input.
#include <SDL3/SDL.h>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

extern "C" {
#include "../ui/menu.h"

int sceSysmoduleLoadModule(std::uint16_t module_id);
int sceCommonDialogInitialize(void);
int sceUserServiceGetInitialUser(std::int32_t *user_id);
int sceImeDialogInit(const void *param, const void *extended);
int sceImeDialogGetStatus(void);
int sceImeDialogGetResult(void *result);
int sceImeDialogTerm(void);
bool bind_capture_event(const SDL_Event *e);
bool game_options_input_hold(void);
}

// ABI used by libSceImeDialog. Keep layout and offsets checked.
struct ImeParam {
    std::int32_t user_id, type;
    std::uint64_t supported_languages;
    std::int32_t enter_label, input_method;
    void *filter;
    std::uint32_t option, max_text_length;
    std::uint16_t *input_text_buffer;
    float pos_x, pos_y;
    std::int32_t horizontal_alignment, vertical_alignment;
    const std::uint16_t *placeholder;
    const std::uint16_t *title;
    std::int8_t reserved[16];
};
struct ImeResult {
    std::int32_t outcome;
    std::int8_t reserved[12];
};
static_assert(sizeof(ImeParam) == 96);
static_assert(offsetof(ImeParam, input_text_buffer) == 40);
static_assert(offsetof(ImeParam, title) == 72);
static_assert(sizeof(ImeResult) == 16);

namespace {
constexpr std::uint16_t kImeDialogModule = 0x0096;
constexpr std::uint32_t kCommonDialogAlreadyInitialized = 0x80b80002u;
constexpr int kStatusNone = 0;
constexpr int kStatusRunning = 1;
constexpr int kStatusFinished = 2;
constexpr int kAlignCenter = 1;
constexpr std::size_t kMaxImeText = 1023;

std::mutex g_ime_mutex;
bool g_ime_ready;

void log_result(const char *what, int result) {
    std::fprintf(stderr, "[swkbd] %s: %#x\n", what, static_cast<unsigned>(result));
}

// Convert UTF-8 to UTF-16, replacing malformed sequences. The IME length is in
// UTF-16 code units, not bytes.
std::vector<std::uint16_t> utf8_to_utf16(const char *s, std::size_t limit) {
    std::vector<std::uint16_t> out(limit + 1, 0);
    const auto *p = reinterpret_cast<const unsigned char *>(s ? s : "");
    std::size_t n = 0;
    while (*p && n < limit) {
        std::uint32_t cp;
        unsigned count;
        if (*p < 0x80) { cp = *p++; count = 0; }
        else if (*p >= 0xc2 && *p <= 0xdf) { cp = *p++ & 0x1f; count = 1; }
        else if (*p >= 0xe0 && *p <= 0xef) { cp = *p++ & 0x0f; count = 2; }
        else if (*p >= 0xf0 && *p <= 0xf4) { cp = *p++ & 0x07; count = 3; }
        else { ++p; cp = 0xfffd; count = 0; }
        bool valid = true;
        for (unsigned i = 0; i < count; ++i) {
            if ((p[i] & 0xc0) != 0x80) { valid = false; break; }
            cp = (cp << 6) | (p[i] & 0x3f);
        }
        if (!valid) { cp = 0xfffd; }
        else p += count;
        if ((count == 1 && cp < 0x80) || (count == 2 && cp < 0x800) ||
            (count == 3 && cp < 0x10000) || cp > 0x10ffff ||
            (cp >= 0xd800 && cp <= 0xdfff)) cp = 0xfffd;
        if (cp < 0x10000) out[n++] = static_cast<std::uint16_t>(cp);
        else {
            if (n + 2 > limit) break;
            cp -= 0x10000;
            out[n++] = static_cast<std::uint16_t>(0xd800 + (cp >> 10));
            out[n++] = static_cast<std::uint16_t>(0xdc00 + (cp & 0x3ff));
        }
    }
    return out;
}

void utf16_to_utf8(const std::uint16_t *s, char *out, std::size_t cap) {
    if (!out || !cap) return;
    std::size_t used = 0;
    for (std::size_t i = 0; s && s[i]; ++i) {
        std::uint32_t cp = s[i];
        if (cp >= 0xd800 && cp <= 0xdbff && s[i + 1] >= 0xdc00 && s[i + 1] <= 0xdfff) {
            cp = 0x10000 + ((cp - 0xd800) << 10) + (s[++i] - 0xdc00);
        } else if (cp >= 0xd800 && cp <= 0xdfff) cp = 0xfffd;

        unsigned char b[4];
        std::size_t count;
        if (cp < 0x80) { b[0] = cp; count = 1; }
        else if (cp < 0x800) {
            b[0] = 0xc0 | (cp >> 6); b[1] = 0x80 | (cp & 0x3f); count = 2;
        } else if (cp < 0x10000) {
            b[0] = 0xe0 | (cp >> 12); b[1] = 0x80 | ((cp >> 6) & 0x3f);
            b[2] = 0x80 | (cp & 0x3f); count = 3;
        } else {
            b[0] = 0xf0 | (cp >> 18); b[1] = 0x80 | ((cp >> 12) & 0x3f);
            b[2] = 0x80 | ((cp >> 6) & 0x3f); b[3] = 0x80 | (cp & 0x3f); count = 4;
        }
        if (used + count >= cap) break;
        std::memcpy(out + used, b, count);
        used += count;
    }
    out[used] = '\0';
}

bool initialize_ime() {
    if (g_ime_ready) return true;
    int r = sceSysmoduleLoadModule(kImeDialogModule);
    if (r < 0) { log_result("loading libSceImeDialog failed", r); return false; }
    r = sceCommonDialogInitialize();
    if (r < 0 && static_cast<std::uint32_t>(r) != kCommonDialogAlreadyInitialized) {
        log_result("sceCommonDialogInitialize failed", r);
        return false;
    }
    g_ime_ready = true;
    return true;
}
} // namespace

extern "C" int menu_keyboard_run(const char *initial, int max_len, char *out, int cap) {
    const std::lock_guard<std::mutex> lock(g_ime_mutex);
    if (!initialize_ime()) return 0;

    std::int32_t user_id = 0;
    int r = sceUserServiceGetInitialUser(&user_id);
    if (r < 0) { log_result("sceUserServiceGetInitialUser failed", r); return 0; }

    const std::size_t limit = std::clamp<std::size_t>(
        max_len > 0 ? static_cast<std::size_t>(max_len) : 9, 1, kMaxImeText);
    std::vector<std::uint16_t> text = utf8_to_utf16(initial, limit);
    static const std::uint16_t title[] = {'E','n','t','e','r',' ','y','o','u','r',' ','h','o','r','s','e','\'','s',' ','n','a','m','e',0};
    static const std::uint16_t placeholder[] = {0};

    ImeParam param{};
    param.user_id = user_id;
    param.max_text_length = static_cast<std::uint32_t>(limit);
    param.input_text_buffer = text.data();
    param.horizontal_alignment = param.vertical_alignment = kAlignCenter;
    param.placeholder = placeholder;
    param.title = title;

    r = sceImeDialogInit(&param, nullptr);
    if (r != 0) { log_result("sceImeDialogInit failed", r); return 0; }
    std::fprintf(stderr, "[swkbd] native PS5 IME opened (user=%d, max=%zu)\n", user_id, limit);

    bool accepted = false;
    const auto opened_at = std::chrono::steady_clock::now();
    for (;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        const int status = sceImeDialogGetStatus();
        if (status == kStatusRunning ||
            (status == kStatusNone && std::chrono::steady_clock::now() - opened_at < std::chrono::seconds(1)))
            continue;

        if (status == kStatusFinished) {
            ImeResult result{};
            r = sceImeDialogGetResult(&result);
            if (r >= 0) accepted = result.outcome == 0;
            else log_result("sceImeDialogGetResult failed", r);
        } else {
            log_result("IME closed without a finished result", status);
        }
        break;
    }

    for (int tries = 0; tries < 100; ++tries) {
        r = sceImeDialogTerm();
        if (r >= 0) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    if (accepted && out && cap > 0) utf16_to_utf8(text.data(), out, static_cast<std::size_t>(cap));
    std::fprintf(stderr, "[swkbd] native PS5 IME %s\n", accepted ? "accepted" : "cancelled");
    return accepted ? 1 : 0;
}

// The system IME owns the screen and DualSense while open; no in-game overlay is drawn.
extern "C" bool menu_handle_event(const SDL_Event *e) {
    return bind_capture_event(e);
}
extern "C" bool menu_input_captured(void) {
    return game_options_input_hold();
}
extern "C" bool menu_render_needed(void) { return false; }
extern "C" void menu_render(void *, std::uint32_t, std::uint32_t) {}
