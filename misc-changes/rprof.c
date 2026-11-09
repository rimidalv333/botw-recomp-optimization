// Render-thread profile (rprof.h).
#include "rprof.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../benchmark.h"
#include "../../frametime.h"
#include "../../rt.h"
#include "../../settings.h"
#include "record.h"

int g_rprof;
uint64_t g_rp[RP_COUNT], g_rp_n[RP_COUNT];

#ifdef __PROSPERO__
// ps5/tools/gen-vk-globals.py: every Vulkan command's trampoline times its driver call on the profiled thread
extern int g_vk_driver_prof;
extern pthread_t g_vk_driver_prof_thread;
extern uint64_t g_vk_driver_ticks[], g_vk_driver_calls[];
extern const char *const g_vk_driver_names[];
extern const int g_vk_driver_count;
#define NDRV g_vk_driver_count
#else
#define NDRV 0
#endif

// what each frame adds to: the 10 s window ([rprof] line) and the benchmark route's two parts
enum { B_WIN, B_TURN, B_STILL, NB };
enum { F_PRESENT, F_ACQUIRE, F_GPU_WAIT, F_TEXTURE, F_BUILD_INLINE, F_BUILD_WAIT, NF };
static const int ft_of[NF] = {FT_PRESENT_NS, FT_ACQUIRE_NS, FT_GPU_WAIT_NS, FT_TEXTURE_NS, FT_BUILD_INLINE_NS, FT_BUILD_WAIT_NS};
typedef struct {
    uint64_t frames, cpu_ns, ft[NF], rp[RP_COUNT], rp_n[RP_COUNT];
    RecStats rec;           // (record.h: what the recorder thread did and the waits on it)
    uint64_t *drv, *drv_n;  // NDRV each
} Bucket;
static Bucket bk[NB];
static uint64_t last_rp[RP_COUNT], last_rp_n[RP_COUNT], last_ft[NF], last_cpu, *last_drv, *last_drv_n;
static RecStats last_rec;
static uint64_t cal_tick0, cal_ns0, win_t0;
static bool started;

double rprof_tick_ns(void) {
    uint64_t t = rp_now(), ns = plat_now_ns();
    if (t <= cal_tick0 || ns <= cal_ns0) return 1.0;
    return (double)(ns - cal_ns0) / (double)(t - cal_tick0);
}

static void bench_lines(char *out, size_t n);

void rprof_init(void) {
    cal_tick0 = rp_now(), cal_ns0 = plat_now_ns();  // (SAVAGEWIND_VK_PROF's draw steps use the ticks too)
    const char *e = getenv("SAVAGEWIND_RENDER_PROFILE");
    g_rprof = e ? atoi(e) != 0 : g_settings.render_profile;
    if (!g_rprof) return;
    cal_tick0 = rp_now(), cal_ns0 = plat_now_ns();
    for (int b = 0; b < NB; b++) {
        bk[b].drv = calloc((size_t)NDRV + 1, sizeof(uint64_t));
        bk[b].drv_n = calloc((size_t)NDRV + 1, sizeof(uint64_t));
    }
    last_drv = calloc((size_t)NDRV + 1, sizeof(uint64_t));
    last_drv_n = calloc((size_t)NDRV + 1, sizeof(uint64_t));
    benchmark_extra(bench_lines);
    LOG("[rprof] render-thread profile on (render_profile = 1): an [rprof] line every 10 s, ms per game frame%s\n",
        NDRV ? "; driver time per Vulkan command" : "");
}

static double ms_of(uint64_t ticks, double tick_ns, double frames) { return (double)ticks * tick_ns / 1e6 / frames; }

static void format(const Bucket *b, char *out, size_t n) {
    double f = b->frames ? (double)b->frames : 1, k = rprof_tick_ns();
    const uint64_t *r = b->rp;
    uint64_t lists = 0;
    for (int i = RP_OP_DRAW; i <= RP_OP_LIST; i++) lists += r[i];
    uint64_t parts = 0;
    for (int i = RP_D_PRE; i <= RP_D_VTX; i++) parts += r[i];
    double cpu = (double)b->cpu_ns / 1e6 / f;
    double accounted = ms_of(lists + r[RP_EXEC_IN] + r[RP_EXEC_SUBMIT], k, f) + (double)b->ft[F_PRESENT] / 1e6 / f;
    size_t at = (size_t)snprintf(out, n,
        "%llu frames, ms per frame: render cpu %.2f | lists %.2f: draw %.2f (%.0f/f: pre %.2f pass %.2f (%.0f/f) key %.2f "
        "dyn %.2f vset %.2f tex %.2f fset %.2f vtx %.2f other %.2f) dispatch %.2f (%.0f) clear %.2f (%.0f) copy %.2f (%.0f) "
        "uub %.2f (%.0f) targets %.2f (%.0f) state %.2f (%.0f) sync %.2f (%.0f) list %.2f (%.0f) | exec in %.2f submit "
        "%.2f | present %.2f (acquire %.2f) | waits in them: gpu %.2f, builds %.2f; texture %.2f, inline builds %.2f | rest "
        "(cpu outside lists, exec and present, less their waits) %.2f",
        (unsigned long long)b->frames, cpu, ms_of(lists, k, f), ms_of(r[RP_OP_DRAW], k, f), b->rp_n[RP_OP_DRAW] / f,
        ms_of(r[RP_D_PRE], k, f), ms_of(r[RP_D_PASS], k, f), b->rp_n[RP_D_PASS] / f, ms_of(r[RP_D_KEY], k, f),
        ms_of(r[RP_D_DYN], k, f), ms_of(r[RP_D_VSET], k, f), ms_of(r[RP_D_TEX], k, f), ms_of(r[RP_D_FSET], k, f),
        ms_of(r[RP_D_VTX], k, f), ms_of(r[RP_OP_DRAW] > parts ? r[RP_OP_DRAW] - parts : 0, k, f),
        ms_of(r[RP_OP_DISPATCH], k, f), b->rp_n[RP_OP_DISPATCH] / f, ms_of(r[RP_OP_CLEAR], k, f), b->rp_n[RP_OP_CLEAR] / f,
        ms_of(r[RP_OP_COPY], k, f), b->rp_n[RP_OP_COPY] / f, ms_of(r[RP_OP_UUB], k, f), b->rp_n[RP_OP_UUB] / f,
        ms_of(r[RP_OP_TARGETS], k, f), b->rp_n[RP_OP_TARGETS] / f, ms_of(r[RP_OP_STATE], k, f), b->rp_n[RP_OP_STATE] / f,
        ms_of(r[RP_OP_SYNC], k, f), b->rp_n[RP_OP_SYNC] / f, ms_of(r[RP_OP_LIST], k, f), b->rp_n[RP_OP_LIST] / f,
        ms_of(r[RP_EXEC_IN], k, f), ms_of(r[RP_EXEC_SUBMIT], k, f), (double)b->ft[F_PRESENT] / 1e6 / f,
        (double)b->ft[F_ACQUIRE] / 1e6 / f, (double)b->ft[F_GPU_WAIT] / 1e6 / f, (double)b->ft[F_BUILD_WAIT] / 1e6 / f,
        (double)b->ft[F_TEXTURE] / 1e6 / f, (double)b->ft[F_BUILD_INLINE] / 1e6 / f,
        cpu - (accounted - (double)(b->ft[F_GPU_WAIT] + b->ft[F_BUILD_WAIT] + b->ft[F_ACQUIRE]) / 1e6 / f));
    if (at < n)
        at += (size_t)snprintf(out + at, n - at, " | fsr3: drain %.2f dispatch %.2f (%.2f/f)",
                               ms_of(r[RP_FSR3_DRAIN], k, f), ms_of(r[RP_FSR3_DISPATCH], k, f),
                               b->rp_n[RP_FSR3_DISPATCH] / f);
    if (at < n)
        at += (size_t)snprintf(out + at, n - at, " | mvec: record %.2f (%.0f/f, frame %.2f match %.2f copy %.2f %.1f KB/f) variant %.2f; scene post %.2f",
                               ms_of(r[RP_MVEC_RECORD], k, f), b->rp_n[RP_MVEC_RECORD] / f,
                               ms_of(r[RP_MVEC_FRAME], k, f), ms_of(r[RP_MVEC_MATCH], k, f),
                               ms_of(r[RP_MVEC_COPY], k, f), b->rp_n[RP_MVEC_COPY] / f / 1024.0,
                               ms_of(r[RP_MVEC_VARIANT], k, f), ms_of(r[RP_SCENE_POST], k, f));
    if (at < n)
        at += (size_t)snprintf(out + at, n - at, " | pre split: program %.2f debug %.2f feedback %.2f fg %.2f"
                               " snapshot %.2f gw %.2f motion %.2f reactive %.2f",
                               ms_of(r[RP_PRE_PROGRAM], k, f), ms_of(r[RP_PRE_DEBUG], k, f),
                               ms_of(r[RP_PRE_FEEDBACK], k, f), ms_of(r[RP_PRE_FG], k, f),
                               ms_of(r[RP_PRE_SNAPSHOT], k, f), ms_of(r[RP_PRE_GW], k, f),
                               ms_of(r[RP_PRE_MOTION], k, f), ms_of(r[RP_PRE_REACTIVE], k, f));
    if (at < n && rec_on()) {
        const RecStats *s = &b->rec;
        at += (size_t)snprintf(out + at, n - at, " | recorder thread: busy %.2f, %.0f packets/f (%.0f KB), queuing %.2f; render "
                               "thread waits: drains %.1f/f %.2f, submissions %.2f, ring full %.2f",
                               ms_of(s->busy_ticks, k, f), s->packets / f, s->bytes / f / 1024.0, ms_of(s->enqueue_ticks, k, f),
                               s->drains / f, s->drain_ns / 1e6 / f, s->seq_wait_ns / 1e6 / f, s->full_ns / 1e6 / f);
    }
    if (at >= n || !NDRV) return;
    // the driver: in all, then the commands that took most
    uint64_t total = 0, calls = 0;
    for (int i = 0; i < NDRV; i++) total += b->drv[i], calls += b->drv_n[i];
    at += (size_t)snprintf(out + at, n - at, " | driver %.2f (%.0f calls/f):", ms_of(total, k, f), calls / f);
    int used[10];
    for (int t = 0; t < 10 && at < n; t++) {
        int best = -1;
        for (int i = 0; i < NDRV; i++) {
            bool seen = false;
            for (int u = 0; u < t; u++) seen |= used[u] == i;
            if (!seen && b->drv[i] && (best < 0 || b->drv[i] > b->drv[best])) best = i;
        }
        if (best < 0) break;
        used[t] = best;
#ifdef __PROSPERO__
        at += (size_t)snprintf(out + at, n - at, " %s %.2f (%.0f)", g_vk_driver_names[best] + 2, ms_of(b->drv[best], k, f),
                               b->drv_n[best] / f);
#endif
    }
}

static void clear(Bucket *b) {
    uint64_t *d = b->drv, *dn = b->drv_n;
    memset(b, 0, sizeof *b);
    b->drv = d, b->drv_n = dn;
    memset(d, 0, sizeof(uint64_t) * ((size_t)NDRV + 1));
    memset(dn, 0, sizeof(uint64_t) * ((size_t)NDRV + 1));
}

static void bench_lines(char *out, size_t n) {
    char a[2048], b[2048];
    format(&bk[B_TURN], a, sizeof a);
    format(&bk[B_STILL], b, sizeof b);
    snprintf(out, n, "render profile, camera turning: %s\nrender profile, still: %s\n", a, b);
}

void rprof_frame(void) {
    if (!g_rprof) return;
    uint64_t cpu = plat_thread_cpu_ns();
    RecStats rec;
    rec_stats(&rec);
    uint64_t ft[NF];
    for (int i = 0; i < NF; i++) ft[i] = __atomic_load_n(&g_ft[ft_of[i]], __ATOMIC_RELAXED);
    if (!started) {  // (the first frame: the render thread is the one presenting)
        started = true;
#ifdef __PROSPERO__
        g_vk_driver_prof_thread = pthread_self();
        __atomic_store_n(&g_vk_driver_prof, 1, __ATOMIC_RELEASE);
#endif
        memcpy(last_rp, g_rp, sizeof last_rp), memcpy(last_rp_n, g_rp_n, sizeof last_rp_n), memcpy(last_ft, ft, sizeof ft);
        last_rec = rec;
        last_cpu = cpu, win_t0 = plat_now_ns();
        return;
    }
    int seg = benchmark_segment();
    Bucket *to[2] = {&bk[B_WIN], seg < 0 ? NULL : &bk[seg ? B_TURN : B_STILL]};
    for (int t = 0; t < 2; t++) {
        Bucket *b = to[t];
        if (!b) continue;
        b->frames++, b->cpu_ns += cpu - last_cpu;
        for (int i = 0; i < NF; i++) b->ft[i] += ft[i] - last_ft[i];
        for (int i = 0; i < RP_COUNT; i++) b->rp[i] += g_rp[i] - last_rp[i], b->rp_n[i] += g_rp_n[i] - last_rp_n[i];
        uint64_t *rs = (uint64_t *)&b->rec;
        const uint64_t *now_r = (const uint64_t *)&rec, *last_r = (const uint64_t *)&last_rec;
        for (size_t i = 0; i < sizeof rec / sizeof(uint64_t); i++) rs[i] += now_r[i] - last_r[i];
#ifdef __PROSPERO__
        for (int i = 0; i < NDRV; i++) b->drv[i] += g_vk_driver_ticks[i] - last_drv[i], b->drv_n[i] += g_vk_driver_calls[i] - last_drv_n[i];
#endif
    }
    memcpy(last_rp, g_rp, sizeof last_rp), memcpy(last_rp_n, g_rp_n, sizeof last_rp_n), memcpy(last_ft, ft, sizeof ft);
    last_rec = rec;
    last_cpu = cpu;
#ifdef __PROSPERO__
    memcpy(last_drv, g_vk_driver_ticks, sizeof(uint64_t) * (size_t)NDRV);
    memcpy(last_drv_n, g_vk_driver_calls, sizeof(uint64_t) * (size_t)NDRV);
#endif
    uint64_t now = plat_now_ns();
    if (now - win_t0 >= 10000000000ull) {
        clear(&bk[B_WIN]);
        win_t0 = now;
    }
}
