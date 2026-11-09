// Kernel object types shared by kernel.c and the IPC/service layer.
#pragma once
#include <setjmp.h>

#include "rt.h"

#define STACK_BASE 0x7200000000ull
#define STACK_SIZE 0x0040000000ull

typedef enum { KOBJ_THREAD = 1, KOBJ_EVENT, KOBJ_SESSION, KOBJ_SHMEM, KOBJ_TMEM } KType;
typedef enum { WAIT_NONE = 0, WAIT_MUTEX, WAIT_CONDVAR, WAIT_ARBITER } WaitKind;

typedef struct KObject {
    KType type;
    int refs;
    bool signaled;
} KObject;

typedef KObject KEvent;

// Where a guest thread's time went (runtime/frametime.c reads these every frame): only the thread itself writes them
// (KT_ADD), other threads read them as they are (relaxed loads).
typedef struct {
    uint64_t cpu_ns;    // its CPU time (plat_thread_cpu_ns) as of its latest wait: a thread reads only its own
    uint64_t wait_ns;   // blocked in a kernel wait (WaitSynchronization, mutexes, condition variables, arbiters)
    uint64_t core_ns;   // waiting for its core (another guest thread of that core ran)
    uint64_t sleep_ns;  // SleepThread, and host code pacing on this thread (the present's turn)
    uint64_t host_ns;   // the GPU backend's work and waits on this thread (kernel_host_work_begin..end)
    uint64_t romfs_ns, romfs_bytes, romfs_reads;  // reading the game's RomFS (the packages) for the game
} KTimes;
#define KT_ADD(t, field, v) __atomic_store_n(&(t)->times.field, (t)->times.field + (v), __ATOMIC_RELAXED)
#define KT_CPU(t) __atomic_store_n(&(t)->times.cpu_ns, plat_thread_cpu_ns(), __ATOMIC_RELAXED)

struct KThread {
    KObject obj;  // signaled when the thread exits
    Cpu cpu;
    pthread_t host;
    uint64_t entry, tlr, id;
    uint32_t handle;
    int prio, core;
    int base_prio;  // the priority the game set; prio may be better while a better thread waits for its mutex (g_klock)
    bool cancel, in_sync_wait;
    // wait state (protected by g_klock)
    WaitKind wait_kind;
    uint64_t wait_addr, wait_mutex;
    uint32_t wait_tag, wait_result;
    KThread *wnext;
    KThread *snext;        // in WaitSynchronization (g_syncwaiters)
    pthread_cond_t cond;   // each thread sleeps on its own condition; wakers signal it directly
    // in-place reboot (kernel_stop_threads): thread_main's setjmp point, where the thread unwinds to and ends
    jmp_buf unwind;
    bool started, finished;
    KThread *anext;        // every thread of this boot (g_klock)
    // core scheduling (runtime/kernel.c "cores"): the core it runs on, the one it holds (-1: none) and its turn
    int core_held;
    uint64_t core_ticket;
    bool off_cores;  // runs beside the cores, never holding one (kernel_thread_off_cores)
    KTimes times;
};

typedef struct KSharedMem {
    KObject obj;
    uint64_t size, guest_addr;  // guest_addr: where the guest mapped it (0 if not mapped)
    void (*on_map)(struct KSharedMem *s);  // HLE owner fills initial contents
    void *owner;
} KSharedMem;

KSharedMem *kernel_create_shared_memory(uint64_t size, void (*on_map)(KSharedMem *), uint32_t *handle);

typedef struct {
    KObject obj;
    uint64_t addr, size;
} KTransferMem;

extern pthread_mutex_t g_klock;
void klock(void);  // pthread_mutex_lock(&g_klock), with a short spin first (kernel.c)
extern KThread *g_waitlist;

KObject *handle_get(uint32_t h);
uint32_t kernel_new_handle(KObject *o);
void kernel_signal(KObject *o);  // caller holds g_klock
// (debug) an HLE write of [addr, addr+len) into guest memory: reported when it overlaps SAVAGEWIND_WATCH
void kernel_watch_write(const char *what, uint64_t addr, uint64_t len);
KEvent *kernel_create_event_pair(uint32_t *writable, uint32_t *readable);
KThread *kernel_current_thread(void);
void kernel_thread_off_cores(void);  // the current thread stops taking part in core scheduling (runtime/kernel.c)
void kernel_sleep_until_ns(uint64_t t);  // host code on a guest thread waits until t (plat_now_ns), its core let go
// host work on a guest thread that runs no guest code (the GPU backend: translating command lists, presenting, waiting
// for the GPU), between these two: its core is let go meanwhile, as the console's GPU works beside the CPU cores
void kernel_host_work_begin(void);
void kernel_host_work_end(void);
// (profiling, runtime/sampler.c) the host threads of the guest threads running now: up to max, with their handles
// and entry points; returns how many
int kernel_running_threads(pthread_t *host, uint32_t *handle, uint64_t *entry, int max);
void kernel_sample_host_thread(void);  // (profiling) the calling host thread is sampled too (handle 0xf000 + n)
// A better thread waits for t's core: t lets it have it (dispatch.c, at indirect calls; g_core_best_wait: per core)
typedef struct { volatile int v; char pad[60]; } __attribute__((aligned(64))) CoreBestWait;  // (one cache line per core)
extern CoreBestWait g_core_best_wait[4];
void kernel_preempt(KThread *t);
uint64_t kernel_heap_size(void);  // bytes from HEAP_BASE set by SetHeapSize
// The guest threads' KTimes (runtime/frametime.c): up to max, returns how many
typedef struct { uint32_t handle; int prio, core; uint64_t entry; KTimes times; } KThreadTimes;
int kernel_thread_times(KThreadTimes *out, int max);
void kernel_romfs_read_done(uint64_t ns, uint64_t bytes);  // runtime/services/fs.c: a RomFS read for the calling thread
