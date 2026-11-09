// PS5 game entry: the game on its guest thread, the VideoOut window and renderer on the main thread (ui_run).
// SPDX-License-Identifier: GPL-3.0-or-later
#include "../gamelib.h"
#include "../hle.h"
#include "../kernel.h"
#include "../nvn/hd_ui_generate.h"
#include "../nvn/precompile.h"
#include "../paths.h"
#include "../settings.h"
#include "../storage_ps5.h"
#include "fd_census.h"
#include "home_art_gate.h"
#include "home_art_ps5.h"
#include <errno.h>
#include <ps5platform/heap.h>
#include <ps5platform/klog.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <unistd.h>

int sceSystemServiceLoadExec(const char *path, const char *const *argv);
bool ps5_exefs_init(const char *directory, char *why, size_t why_size);
const char *g_romfs_dir = "/data/savagewind/game/romfs";
uint64_t g_program_id;
static uint64_t startup_ns;
void ps5_startup_mark(const char *step) {
    LOG("[startup] %llu ms: %s\n", (unsigned long long)((plat_now_ns() - startup_ns) / 1000000), step);
}

void ps5_fd_census(const char *reason) {
    struct rlimit limit;
    if (getrlimit(RLIMIT_NOFILE, &limit)) {
        LOG("[fs] fd census %s: getrlimit failed: %s\n", reason, strerror(errno));
        return;
    }
    // Existing descriptors can be above the current soft limit after it is lowered.
    // Bound an infinite or unusually large hard limit, and report the bound explicitly.
    const rlim_t scan_max = 65536;
    int end = (int)(limit.rlim_max < scan_max ? limit.rlim_max : scan_max);
    struct fd_census count = fd_census_scan(end);
    LOG("[fs] open fds: %u (%s; scanned %d)\n",
        count.regular + count.directories + count.sockets + count.other, reason, end);
    LOG("[fs] fd types: regular %u directory %u socket %u other %u\n",
        count.regular, count.directories, count.sockets, count.other);
    LOG("[fs] fd limit at census: soft %llu hard %llu\n",
        (unsigned long long)limit.rlim_cur, (unsigned long long)limit.rlim_max);
}

static void *fd_monitor(void *unused) {
    (void)unused;
    for (;;) {
        sleep(60);
        ps5_fd_census("periodic");
    }
    return NULL;
}

static void fd_monitor_start(void) {
    struct rlimit limit;
    if (getrlimit(RLIMIT_NOFILE, &limit)) {
        LOG("[fs] getrlimit(RLIMIT_NOFILE) failed: %s\n", strerror(errno));
    } else {
        rlim_t old = limit.rlim_cur;
        limit.rlim_cur = limit.rlim_max;
        int rc = setrlimit(RLIMIT_NOFILE, &limit);
        LOG("[fs] fd limit soft %llu hard %llu; raise: %s\n",
            (unsigned long long)old, (unsigned long long)limit.rlim_max, rc ? strerror(errno) : "ok");
    }
    ps5_fd_census("startup");
    pthread_t thread;
    int rc = pthread_create(&thread, NULL, fd_monitor, NULL);
    if (rc) LOG("[fs] fd monitor unavailable: %s\n", strerror(rc));
    else pthread_detach(thread);
}

static void exit_title(void) {
    fflush(stderr);
    sceSystemServiceLoadExec("exit", NULL);
    for (;;) sleep(1);
}
void ps5_exit_title(void) {
    exit_title();
    for (;;) sleep(1);
}
// No in-place reboot (runtime/main.c: back to the start screen, or the game again): the title has no start screen.
// A request (the Options pages: Quit > Start screen, Language > Restart now; settings already saved) closes the title,
// to be started again from the home screen. SAVAGEWIND_REBOOT_AT is a desktop test (a title has no environment).
int rt_reboot_requested(void) { return 0; }
void rt_request_reboot(int kind) {
    fprintf(stderr, "[ps5] %s requested: no in-place restart on the PS5, the title closes (start it again)\n",
            kind == 2 ? "the start screen" : "a restart");
    exit_title();
}
void reboot_test_frame(uint64_t frame) { (void)frame; }

static void module_init(Cpu *c, Module *m) {
    if (m->init) guest_call(c, m->init);
    else for (uint64_t i = 0; i < m->init_array_sz / 8; i++) {
        uint64_t fn = ((uint64_t *)(uintptr_t)m->init_array)[i];
        if (fn) guest_call(c, fn);
    }
}
static void init_sdk(Cpu *c) { module_init(c, loader_module("sdk")); }
static void init_main(Cpu *c) {
    for (int i = g_nmodules - 1; i >= 0; i--)
        if (strcmp(g_modules[i].name, "sdk")) module_init(c, &g_modules[i]);
    extern void rt_title_menu_prepare(void);  // runtime/ui/game_options.mm: tables the constructors filled
    rt_title_menu_prepare();
}
static void fini_main(Cpu *c) {
    for (int i = 0; i < g_nmodules; i++) {
        Module *m = &g_modules[i];
        if (!strcmp(m->name, "sdk")) continue;
        if (m->fini) guest_call(c, m->fini);
        else for (uint64_t j = m->fini_array_sz / 8; j-- > 0;) {
            uint64_t fn = ((uint64_t *)(uintptr_t)m->fini_array)[j];
            if (fn) guest_call(c, fn);
        }
    }
}

// ---- first start: every shader of the game precompiled (docs/PS5-PORT-PLAN.md D6, runtime/nvn/precompile.h), before
// the game runs, while the screen shows what it is doing (a heading, the percentage of the whole, the step in plain
// words with a bar, and that it happens once) and the klog the step, how far it is and the time so far. Runs again
// when the translator or the driver changed (cache/precompiled.txt); off with precompile_shaders = 0.
// (on a thread of its own with an 8 MiB stack: opening the packages needs more than the main thread's 2 MiB,
// runtime/platform/exefs_ps5.c)
static ShaderPrecompile g_pc;
static bool g_generate_hd, g_run_shaders;
static void *precompile_main(void *arg) {
    (void)arg;
    RomfsImage *img = fs_game_romfs();  // (kept open: the game reads it next)
    if (img) {
        if (g_generate_hd && !hd_ui_generate_start()) LOG("[hd-ui-gen] could not start generation\n");
        if (g_run_shaders) shader_precompile_run(&g_pc, img, "exefs/main");
        else atomic_store(&g_pc.state, SP_DONE);
        hd_ui_generate_shaders_done();
    }
    else {
        snprintf(g_pc.why, sizeof g_pc.why, "the game's RomFS can't be read");
        atomic_store(&g_pc.state, SP_FAILED);
    }
    return NULL;
}

static void thousands(char *out, size_t n, int v) {  // 13997 -> "13,997"
    if (v >= 1000) snprintf(out, n, "%d,%03d", v / 1000, v % 1000);
    else snprintf(out, n, "%d", v);
}

static void first_start_precompile(void) {
    int shaders = 0, status = shader_precompile_status(&shaders, NULL, 0);
    g_run_shaders = status != 1 && g_settings.precompile_shaders;
    g_generate_hd = g_settings.hd_ui_generate && hd_ui_generate_needed();
    if (!g_run_shaders && !g_generate_hd) {
        LOG("[precompile] %s\n", status == 1 ? "done for this translator and driver: skipped" : "off (precompile_shaders = 0)");
        return;
    }
    if (g_run_shaders)
        LOG("[precompile] %s: every shader of the game is translated and compiled now, with its pipeline libraries\n",
            status ? "the translator or the driver changed" : "first start");
    uint64_t t0 = plat_now_ns();
    extern void vk_present_progress(float done, const char *const *lines);
    extern void gpu_lock(void), gpu_unlock(void);
    pthread_t t;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 8u << 20);
    int rc = pthread_create(&t, &attr, precompile_main, NULL);
    pthread_attr_destroy(&attr);
    if (rc) { LOG("[precompile] no thread (%d): skipped\n", rc); return; }
    static const char *const steps[] = {"opening the game's packages", "reading the shader archives", "translating and compiling shaders",
                                        "building pipeline libraries", "done", "failed", "stopped"};
    uint64_t next_log = t0, last_window = t0;
    int last_state = -1;
    bool hd_reads_released = false;
    extern uint64_t radv_ps5_allocated_bytes;  // (RADV's PS5 winsys: its buffers' direct memory)
    extern void rt_heap_timing(uint64_t *calls, uint64_t *ns, unsigned *own_arenas);  // (runtime/ps5/heap_arenas.c)
    for (;;) {
        int st = atomic_load(&g_pc.state), hd_done = 0, hd_total = 0;
        if (st >= SP_DONE) break;
        if (g_generate_hd && !hd_reads_released && st >= SP_COMPILING) {
            hd_ui_generate_shader_reads_done();
            hd_reads_released = true;
        }
        bool hd_active = hd_ui_generate_progress(&hd_done, &hd_total);
        float f = shader_precompile_progress(&g_pc);
        char pct[16], step[160], hd_step[100], a[16], b[16];
        snprintf(pct, sizeof pct, "%d %%", (int)(f * 100));  // (99 at most until it is done)
        thousands(a, sizeof a, atomic_load(&g_pc.done));
        thousands(b, sizeof b, atomic_load(&g_pc.total));
        if (st == SP_READING) snprintf(step, sizeof step, "Step 1 of 3: reading the game's shaders");
        else if (st == SP_COMPILING) snprintf(step, sizeof step, "Step 2 of 3: translating shaders, %s of %s", a, b);
        else if (st == SP_PIPELINES) snprintf(step, sizeof step, "Step 3 of 3: compiling them for the GPU, %s of %s", a, b);
        else snprintf(step, sizeof step, "Opening the game's files");
        if (hd_active && hd_total) snprintf(hd_step, sizeof hd_step, "HD interface %d of %d", hd_done, hd_total);
        else if (hd_active && !hd_reads_released) snprintf(hd_step, sizeof hd_step, "HD interface: waiting for shader reads");
        else if (hd_active || (g_generate_hd && st == SP_IDLE)) snprintf(hd_step, sizeof hd_step, "HD interface: reading textures");
        else snprintf(hd_step, sizeof hd_step, "This happens only on the first start and after an update.");
        const char *lines[4] = {"Compiling shaders", pct, step, hd_step};
        gpu_lock();
        vk_present_progress(f, lines);
        gpu_unlock();
        uint64_t now = plat_now_ns();
        bool tick = now >= next_log;
        if (tick || st != last_state) {
            LOG("[precompile] %.0f s: %s, %d of %d (%.0f%% of the whole)\n", (now - t0) / 1e9, steps[st],
                atomic_load(&g_pc.done), atomic_load(&g_pc.total), shader_precompile_progress(&g_pc) * 100);
            next_log = now + 5000000000ull;
            last_state = st;
        }
        if (tick) {  // (and where the workers' time went meanwhile, the heap's share sampled: runtime/ps5/heap_arenas.c)
            struct ps5_heap_stats heap;
            ps5_heap_stats(&heap);
            static uint64_t last_calls, last_ns;
            uint64_t calls, ns;
            unsigned own;
            rt_heap_timing(&calls, &ns, &own);
            char extra[300];
            snprintf(extra, sizeof extra, "; heap %.1fM calls taking ~%.1f s, %u threads on arenas of their own; title heap "
                     "%.0f MB in %u segments, %llu libc fallbacks; RADV buffers %.0f MB", (calls - last_calls) / 1e6,
                     (ns - last_ns) / 1e9, own, heap.mapped_bytes / 1048576.0, heap.segments, heap.libc_fallbacks,
                     __atomic_load_n(&radv_ps5_allocated_bytes, __ATOMIC_RELAXED) / 1048576.0);
            last_calls = calls, last_ns = ns;
            shader_precompile_log_window(&g_pc, (now - last_window) / 1e9, extra);
            last_window = now;
        }
        usleep(100000);
    }
    pthread_join(t, NULL);
    if (hd_ui_generate_progress(NULL, NULL))
        LOG("[hd-ui-gen] continuing in background after shader precompile\n");
    struct ps5_heap_stats heap;
    ps5_heap_stats(&heap);
    int st = atomic_load(&g_pc.state);
    LOG("[precompile] %s in %.1f s: %d shaders, %d translations (%d made, %d were cached, %d failed), %d pipeline "
        "libraries and compute pipelines%s%s; title heap %.0f MB now, %.0f MB at most\n", steps[st], (plat_now_ns() - t0) / 1e9,
        g_pc.shaders, g_pc.keys, g_pc.made, g_pc.cached, g_pc.failed, g_pc.pipelines, st == SP_FAILED ? ": " : "",
        st == SP_FAILED ? g_pc.why : "", heap.mapped_bytes / 1048576.0, heap.peak_bytes / 1048576.0);
}
static const char home_art_marker_path[] = "cache/home-art/marker";
static unsigned char g_home_art_source[128];
static atomic_int g_home_art_finished;
static int g_home_art_result;

static size_t home_art_read_marker(void *context, HomeArtMarker *marker) {
    (void)context;
    FILE *file = fopen("/data/savagewind/cache/home-art/marker", "rb");
    if (!file) return 0;
    size_t size = fread(marker, 1, sizeof *marker, file);
    if (fgetc(file) != EOF) size++;
    if (ferror(file)) size = 0;
    fclose(file);
    return size;
}

static int home_art_remove_marker(void *context) {
    (void)context;
    int rc = rt_storage_unlink(home_art_marker_path);
    return rc == -ENOENT ? 0 : rc;
}

static int home_art_install(void *context, int enabled, unsigned char digests[3][32]) {
    (void)context;
    return home_art_generate(enabled, digests);
}

static int home_art_write_marker(void *context, const HomeArtMarker *marker) {
    (void)context;
    int rc = rt_storage_mkdirs("cache/home-art");
    if (rc < 0) return rc;
    return rt_storage_replace(home_art_marker_path, marker, sizeof *marker);
}

static const HomeArtGateOps home_art_ops = {
    home_art_read_marker, home_art_remove_marker, home_art_install, home_art_write_marker
};

static void *home_art_main(void *unused) {
    (void)unused;
    g_home_art_result = home_art_gate_run(&home_art_ops, NULL, g_settings.home_art, g_home_art_source);
    atomic_store(&g_home_art_finished, 1);
    return NULL;
}

static void first_start_home_art(void) {
    if (ps5_exefs_home_art_identity(g_home_art_source) < 0) {
        LOG("[home-art] package identity unavailable; skipped\n");
        return;
    }
    HomeArtMarker marker;
    size_t size = home_art_read_marker(NULL, &marker);
    if (home_art_marker_matches(&marker, size, g_settings.home_art, g_home_art_source)) {
        LOG("[home-art] current; skipped\n");
        return;
    }
    LOG("[home-art] installing %s art\n", g_settings.home_art ? "generated" : "shipped");
    pthread_attr_t attr;
    pthread_t thread;
    int rc = pthread_attr_init(&attr);
    if (rc) {
        LOG("[home-art] thread attributes failed (%d)\n", rc);
        return;
    }
    rc = pthread_attr_setstacksize(&attr, 8u << 20);
    if (!rc) rc = pthread_create(&thread, &attr, home_art_main, NULL);
    pthread_attr_destroy(&attr);
    if (rc) {
        LOG("[home-art] thread creation failed (%d)\n", rc);
        return;
    }
    extern void vk_present_progress(float done, const char *const *lines);
    extern void gpu_lock(void), gpu_unlock(void);
    const char *lines[] = {"Preparing game files", "", "Home screen art", "This happens only when the art changes."};
    while (!atomic_load(&g_home_art_finished)) {
        gpu_lock();
        vk_present_progress(0, lines);
        gpu_unlock();
        usleep(100000);
    }
    pthread_join(thread, NULL);
    if (g_home_art_result < 0) LOG("[home-art] failed (stage %d); continuing\n", g_home_art_result);
    else LOG("[home-art] installed and recorded\n");
}

#ifdef GAMECODE_CONSOLE
bool ps5_gamecode_prepare(unsigned threads, bool overlap,
                          void (*progress)(void *, const char *, size_t, size_t), void *context,
                          char *why, size_t why_size);
void ps5_gamecode_record_start_failure(unsigned threads, bool overlap);
bool ps5_exefs_validate_gamecode(char *why, size_t why_size);
static struct {
    atomic_int stage, done, total, finished;
    bool success;
    char why[512];
} g_gamecode;
static pthread_t g_overlap_thread;
static bool g_overlap_started;

static bool start_overlap_precompile(void) {
    if (!g_settings.gamecode_overlap) return false;
    int status = shader_precompile_status(NULL, NULL, 0);
    g_run_shaders = status != 1 && g_settings.precompile_shaders;
    g_generate_hd = g_settings.hd_ui_generate && hd_ui_generate_needed();
    if (!g_run_shaders && !g_generate_hd) return false;
    pthread_attr_t attr;
    int rc = pthread_attr_init(&attr);
    if (rc) {
        LOG("[gamecode] overlap thread attributes failed (%d); running serially\n", rc);
        return false;
    }
    rc = pthread_attr_setstacksize(&attr, 8u << 20);
    if (!rc) rc = pthread_create(&g_overlap_thread, &attr, precompile_main, NULL);
    pthread_attr_destroy(&attr);
    if (rc) {
        LOG("[gamecode] overlap thread failed (%d); running serially\n", rc);
        return false;
    }
    g_overlap_started = true;
    LOG("[gamecode] overlapping shader/HD first-start work\n");
    return true;
}

static void join_overlap_precompile(void) {
    if (!g_overlap_started) return;
    pthread_join(g_overlap_thread, NULL);
    int state = atomic_load(&g_pc.state);
    if (state == SP_FAILED) LOG("[precompile] failed: %s\n", g_pc.why);
    else if (state == SP_CANCELLED) LOG("[precompile] cancelled\n");
    else LOG("[precompile] overlap finished: %d shaders, %d pipelines\n", g_pc.shaders, g_pc.pipelines);
    if (hd_ui_generate_progress(NULL, NULL))
        LOG("[hd-ui-gen] continuing in background after shader precompile\n");
}

static void gamecode_progress(void *unused, const char *stage, size_t done, size_t total) {
    (void)unused;
    int value = strstr(stage, "generat") ? 1 :
                (strstr(stage, "compil") || !strcmp(stage, "cache")) ? 2 :
                strstr(stage, "link") ? 3 : strstr(stage, "sav") ? 4 : 0;
    int previous = atomic_load(&g_gamecode.stage);
    if (value == 1 && previous >= 2) return;
    atomic_store(&g_gamecode.done, (int)done);
    atomic_store(&g_gamecode.total, (int)total);
    atomic_store(&g_gamecode.stage, value);
}

static void *gamecode_main(void *unused) {
    (void)unused;
    plat_thread_heap_own();
    g_gamecode.success = ps5_gamecode_prepare((unsigned)g_settings.gamecode_threads, g_overlap_started,
                                               gamecode_progress, NULL, g_gamecode.why, sizeof g_gamecode.why);
    atomic_store(&g_gamecode.finished, 1);
    return NULL;
}

static bool first_start_gamecode(char *why, size_t why_size) {
    extern void vk_present_progress(float done, const char *const *lines);
    extern void gpu_lock(void), gpu_unlock(void);
    start_overlap_precompile();
    pthread_attr_t attr;
    pthread_t thread;
    int rc = pthread_attr_init(&attr);
    if (rc) {
        snprintf(why, why_size, "game code thread attributes failed: %d", rc);
        ps5_gamecode_record_start_failure((unsigned)g_settings.gamecode_threads, g_overlap_started);
        if (g_overlap_started) {
            atomic_store(&g_pc.cancel, true);
            join_overlap_precompile();
        }
        return false;
    }
    rc = pthread_attr_setstacksize(&attr, 8u << 20);
    if (!rc) rc = pthread_create(&thread, &attr, gamecode_main, NULL);
    pthread_attr_destroy(&attr);
    if (rc) {
        snprintf(why, why_size, "game code thread creation failed: %d", rc);
        ps5_gamecode_record_start_failure((unsigned)g_settings.gamecode_threads, g_overlap_started);
        if (g_overlap_started) {
            atomic_store(&g_pc.cancel, true);
            join_overlap_precompile();
        }
        return false;
    }
    bool hd_reads_released = false;
    for (;;) {
        bool game_done = atomic_load(&g_gamecode.finished);
        int shader_state = atomic_load(&g_pc.state);
        if (g_overlap_started && g_generate_hd && !hd_reads_released && shader_state >= SP_COMPILING) {
            hd_ui_generate_shader_reads_done();
            hd_reads_released = true;
        }
        if (game_done && !g_gamecode.success && g_overlap_started) atomic_store(&g_pc.cancel, true);
        if (game_done && (!g_overlap_started || shader_state >= SP_DONE)) break;
        int stage = atomic_load(&g_gamecode.stage);
        int done = atomic_load(&g_gamecode.done);
        int total = atomic_load(&g_gamecode.total);
        if (stage == 2 && !total) total = 996;
        float part = total > 0 ? (float)done / (float)total : 0;
        if (part > 1) part = 1;
        float fraction = game_done && g_gamecode.success ? 1 :
            stage == 1 ? part * 0.13f : stage == 2 ? 0.13f + part * 0.82f :
            stage == 3 ? 0.95f : stage == 4 ? 0.96f + part * 0.02f : 0;
        char step[160], shader[160], hd[160];
        int percent = (int)(fraction * 100);
        if (game_done) snprintf(step, sizeof step, "Game code: %s (%d%%)",
                                g_gamecode.success ? "ready" : "failed", percent);
        else if (stage == 1) snprintf(step, sizeof step, "Generating (%d%%)", percent);
        else if (stage == 2) snprintf(step, sizeof step, "Compiling %d of %d (%d%%)", done, total, percent);
        else if (stage == 3) snprintf(step, sizeof step, "Linking (%d%%)", percent);
        else if (stage == 4) snprintf(step, sizeof step, "Saving (%d%%)", percent);
        else snprintf(step, sizeof step, "Checking game code cache (%d%%)", percent);
        if (!g_overlap_started) snprintf(shader, sizeof shader, "Shaders: after game code");
        else if (!g_run_shaders) snprintf(shader, sizeof shader, "Shaders: up to date or disabled");
        else if (shader_state == SP_DONE) snprintf(shader, sizeof shader, "Shaders: complete");
        else if (shader_state == SP_FAILED) snprintf(shader, sizeof shader, "Shaders: failed (see log)");
        else if (shader_state == SP_CANCELLED) snprintf(shader, sizeof shader, "Shaders: cancelled");
        else snprintf(shader, sizeof shader, "Shaders: %d%%", (int)(shader_precompile_progress(&g_pc) * 100));
        int hd_done = 0, hd_total = 0;
        bool hd_active = g_overlap_started && hd_ui_generate_progress(&hd_done, &hd_total);
        if (!g_overlap_started) snprintf(hd, sizeof hd, "HD interface: after game code");
        else if (!g_generate_hd) snprintf(hd, sizeof hd, "HD interface: up to date or disabled");
        else if (hd_active && hd_total) snprintf(hd, sizeof hd, "HD interface: %d of %d", hd_done, hd_total);
        else if (hd_active || shader_state == SP_IDLE) snprintf(hd, sizeof hd, "HD interface: preparing");
        else snprintf(hd, sizeof hd, "HD interface: complete or unavailable");
        // A valid linked game-code cache has no progress callbacks. Keep the screen black while it loads;
        // only show this UI for actual game-code work or first-start shader/HD precompilation.
        if (stage > 0 || g_overlap_started) {
            const char *lines[] = {"Building game code (first start only)", step, shader, hd};
            gpu_lock();
            vk_present_progress(fraction, lines);
            gpu_unlock();
        }
        usleep(100000);
    }
    pthread_join(thread, NULL);
    join_overlap_precompile();
    if (!g_gamecode.success) snprintf(why, why_size, "%s", g_gamecode.why);
    return g_gamecode.success;
}
#endif


int main(void) {
    startup_ns = plat_now_ns();
    sceKernelDebugOutText(0, "savagewind: PPSA90212 M3 1.9.0 startup\n");
    if (ps5_klog_capture_stderr("savagewind: ") != 0) {
        sceKernelDebugOutText(0, "savagewind: cannot capture stderr\n");
        exit_title();
    }
    ps5_startup_mark("main start");
    if (!plat_reserve_guest(GUEST_LO, GUEST_HI)) {
        fprintf(stderr, "cannot reserve guest address space\n");
        exit_title();
    }
    fd_monitor_start();
    // RADV's own shader cache is off (by default a database in /app0/radv-shader-cache, radv_ps5_platform.c): the
    // title keeps the driver's compiled shaders itself (cache/vk_pipelines.bin, vk_precompiled.bin, through the
    // broker), and the database wrote every new shader again with direct title writes (two flushed writes each,
    // under locks the lookups of all compiling threads share). On the host the precompilation's libraries took 10%
    // longer with it, on a fast disk (65.3 against 59.1 s; 7 threads, RADV's null NAVI21 device, Mesa 25.2).
    setenv("MESA_SHADER_CACHE_DISABLE", "true", 1);
    plat_init();
    rt_paths_init();
    ps5_startup_mark("paths and permission repair done");
    char why[512] = "";
#ifdef GAMECODE_CONSOLE
    extern void ui_window_init(void);
    ui_window_init();
    bool packages_ok = ps5_exefs_init("/data/savagewind/import", why, sizeof why);
    bool startup_ok = packages_ok && first_start_gamecode(why, sizeof why) && gamelib_load(NULL, why, sizeof why) &&
        ps5_exefs_validate_gamecode(why, sizeof why);
#else
    bool startup_ok = gamelib_load(NULL, why, sizeof why) && ps5_exefs_init("/data/savagewind/import", why, sizeof why);
#endif
    if (!startup_ok) {
#ifdef GAMECODE_CONSOLE
        extern void vk_present_progress(float done, const char *const *lines);
        extern void gpu_lock(void), gpu_unlock(void);
        // Players read this on the TV without a log: for missing or unusable game files, name the folder and what
        // goes in it (line 1 is the large one, so it stays short), and leave time to read
        const char *files[] = {"Savage Wind can't start", "Check your game files", why,
                               "Put the base NSP or XCI, the 1.9.0 update NSP and prod.keys (and title.keys if needed) "
                               "in /data/savagewind/import/, then restart."};
        // Key and ticket errors (package_core.cpp codes): the keys file is the likely culprit, so say which one works
        const char *keys[] = {"Savage Wind can't start", "Check your keys", why,
                              "Use prod.keys dumped from a Switch on firmware 19.0.0 (recommended), in "
                              "/data/savagewind/import/, then restart."};
        const char *other[] = {"Savage Wind can't start", "", why, "Restart the game. If this repeats, report it."};
        bool key_error = !strncmp(why, "keys_", 5) || !strncmp(why, "ticket_missing", 14);
        gpu_lock();
        vk_present_progress(0, packages_ok ? other : key_error ? keys : files);
        gpu_unlock();
        sleep(30);
#endif
        fprintf(stderr, "startup failed: %s\n", why);
        exit_title();
    }
    ps5_startup_mark("packages opened");
    loader_load("exefs");
    ps5_exefs_read_summary();
    ps5_startup_mark("loader_load exefs finished");
    extern void fs_title_version_patch(void);
    extern void settings_load(void);
    extern void language_booted(void);
    fs_title_version_patch();
    ps5_startup_mark("fs_title_version_patch finished");
    settings_load();
    ps5_startup_mark("settings loaded");
    ps5_log_init(g_settings.log_sync);
    language_booted();
    kernel_init();
    dispatch_init();
    size_t size = 0;
    uint8_t *npdm = loader_read_file("exefs/main.npdm", &size, 0x10000);
    if (!npdm || size < 0x80 || memcmp(npdm, "META", 4)) {
        fprintf(stderr, "package main.npdm is missing or invalid\n");
        free(npdm);
        exit_title();
    }
    uint32_t stack_size, aci;
    memcpy(&stack_size, npdm + 0x1c, 4);
    int prio = npdm[0xe];
    memcpy(&aci, npdm + 0x70, 4);
    if ((uint64_t)aci + 0x18 <= size && !memcmp(npdm + aci, "ACI0", 4)) memcpy(&g_program_id, npdm + aci + 0x10, 8);
    free(npdm);
    uint64_t start = loader_find_symbol("_ZN2nn4init5StartEmmPFvvES2_S2_");
    bool has_fini = start != 0;
    if (!start) start = loader_find_symbol("_ZN2nn4init5StartEmmPFvvES2_");
    if (!start) { fprintf(stderr, "nn::init::Start is missing\n"); exit_title(); }
    uint64_t sdk = hle_register("<rtld_init_sdk>", init_sdk);
    uint64_t game = hle_register("<rtld_init_main>", init_main);
    uint64_t fini = has_fini ? hle_register("<rtld_fini_main>", fini_main) : 0;
    KThread *thread = kernel_create_main_thread(start, stack_size, prio);
    extern void ui_window_init(void);  // runtime/nvn/vulkan/device.c: the display plane and device
    ui_window_init();
    ps5_startup_mark("window up");
    if (!ps5_startup_romfs_wait(why, sizeof why)) {
        fprintf(stderr, "startup failed: %s\n", why);
        exit_title();
    }
    extern void ps5_s7_run(void);
    ps5_s7_run();
#ifdef GAMECODE_CONSOLE
    if (!g_overlap_started) first_start_precompile();
#else
    first_start_precompile();
#endif
    first_start_home_art();
    ps5_startup_mark("precompile done or skipped");
    extern void ps5_startup_splash_start(void);
    if (g_settings.startup_splash) ps5_startup_splash_start();
    fprintf(stderr, "[ps5] entering linked game code at %llx\n", (unsigned long long)start);
    ps5_startup_mark("entering game code");
    kernel_run_main(thread, thread->handle, 0, sdk, game, fini);
    extern int ui_run(void);  // (its window loop; the window is up)
    ui_run();
    exit_title();
}
