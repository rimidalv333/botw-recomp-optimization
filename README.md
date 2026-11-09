# PS5 runtime scheduling expr/technical report on PS5 native port of BOTW (SavageWindRecomp)
A runtime-only scheduling experiment was performed on the PS5 build of SavageWindRecomp using:

``` ini
SAVAGEWIND_NO_PREEMPT=1
SAVAGEWIND_CORES=0
```

The tuned run showed a clear improvement in frame-time consistency in the demanding open-world test area. In the most useful pair of 60-second profiler windows, the proportion of frames meeting the target increased from **70.3% to 95.3%**, P95 frame time decreased from **21.25 ms to 18.00 ms**, P99 decreased from **28.25 ms to 19.00 ms**, and reported hitches fell from **52 to 1**. The number of presented frames in those windows increased from 3,369 to 3,532 (approximately **56.15 to 58.87 FPS**, +4.8%).

The logs do not show a corresponding reduction in guest CPU, render-thread CPU, or GPU work. In the selected windows, all three reported per-frame workload figures were higher in the tuned run. This is consistent with a scheduling/latency improvement rather than a reduction in rendering or game simulation workload.

**Important source-level qualification:** the two variables are not equally active in the combined configuration. With `SAVAGEWIND_CORES=0`, the guest-core token mechanism is disabled and guest threads do not acquire a `core_held` token. The dispatch-time preemption check guarded by `core_held >= 0` therefore cannot run. Consequently, in this exact combined configuration, `SAVAGEWIND_NO_PREEMPT=1` is effectively redundant; the observed change is most plausibly attributable to `SAVAGEWIND_CORES=0`. An isolated A/B test is required to quantify each variable independently.

## test setup and controls
### build identity
Both runs used the same EBOOT and the same build settings:

``` bash
PS5_GAMECODE_OPT=-O2 ps5/tools/build-gamecode.sh

PS5_BUILD_JOBS=8 \
PS5_GAMECODE_OPT=-O2 \
PS5_ALLOW_LOCAL_RADV=1 \
bash ps5/tools/build-game.sh
```

The experiment therefore did not compare different game-code optimization levels or different EBOOT builds. The variables were changed at runtime.

### test procedure
The baseline and tuned logs were captured after teleporting to, and traversing, generally the same demanding area and route. This is a practical repeatability measure, but it is not a deterministic replay: exact camera position, NPC activity, simulation state, streaming activity, weather, and background work may still differ.

The route was from Shrine of Resurrection -> Temple of Time

The two source logs used here are:

-   **Baseline:** `botwx86compile.txt`
-   **Tuned:** `botwx86_experimental_env_compile.txt`
[botwx86_experimental_env_compile.txt](https://github.com/user-attachments/files/34595546/botwx86_experimental_env_compile.txt)
[botwx86compile.txt](https://github.com/user-attachments/files/34595548/botwx86compile.txt)

The logs contain `[svcprof]` output produced by the runtime's `SAVAGEWIND_PROF_SVC` instrumentation (yet again revealed by the env implementation)

The behaviour below is taken from `runtime/kernel.c` and `runtime/dispatch.c` in the supplied source archive.

SAVAGEWIND_CORES=0:
The runtime contains a four-entry guest-core token scheduler. Each emulated guest core has a token/owner. A guest thread must hold the token associated with its assigned guest core before executing guest code.

The implementation is intended to approximate Horizon scheduling semantics:

Threads assigned to the same guest core do not execute guest code simultaneously. A thread that blocks or sleeps releases its token. Waiting threads are selected by priority (lower numeric priority is higher priority), then by wait order. A running thread may yield the token when a higher-priority thread is waiting.

Guest-generated code has no independent preemption points, so a thread can retain its token through a long stretch of guest execution until it reaches a runtime safe point.

The source explicitly documents why this emulation exists: BOTW 1.8.2 shares a `sead` heap between threads pinned to one guest core without a lock. The heap free list was observed to break when those threads ran in parallel. The token mechanism prevents that particular same-core parallel execution.

`SAVAGEWIND_CORES=0` makes `cores_on()` return false. As a result, `core_acquire()` and `core_release()` do not implement guest-core ownership, and guest threads are no longer serialized by the four guest-core tokens. They can execute concurrently on host threads.

This removes a potentially significant source of scheduling and hand-off latency, but it also removes a synchronization property that the game may rely on. The source comment is an explicit warning: disabling this mechanism can reintroduce races in the shared `sead` heap.

The value is parsed as an exact string comparison: it must be `0`. For example, `SAVAGEWIND_CORES=1` does not disable the mechanism.

SAVAGEWIND_NO_PREEMPT=1:
When guest-core token scheduling is enabled, the runtime tracks the best (highest-priority) waiting thread for each core in `g_core_best_wait[]`.

At indirect guest dispatch points, `rt_dispatch()` checks:

``` c
if (t && t->core_held >= 0 &&
    g_core_best_wait[t->core_held].v < t->prio)
    kernel_preempt(t);
```

If a higher-priority thread is waiting for the same guest core, `kernel_preempt()` releases the current thread's token and reacquires it according to the wait queue.

When `SAVAGEWIND_NO_PREEMPT` is present, `core_best_update()` keeps the corresponding best-wait priority at `INT_MAX`, rather than publishing the priority of waiting threads. This disables that **dispatch-point, priority-based preemption path**.

It does **not** disable all scheduling or all preemption. The syscall-side `core_preempt_point()` remains in place when guest-core emulation is active. The distinction matters: this variable suppresses the extra preemption opportunity at indirect dispatch points, while syscall scheduling behaviour remains.

This variable is presence-based. `SAVAGEWIND_NO_PREEMPT=0` still activates it; remove the variable entirely to turn it off.

When `SAVAGEWIND_CORES=0` is active:

1.  `core_acquire()` returns without assigning a guest-core token.
2.  Threads consequently have no held guest-core token (`core_held` remains negative).
3.  `rt_dispatch()` only performs the priority-preemption check when `core_held >= 0`.
4.  The dispatch-point preemption path is therefore already unreachable.

Thus, with both variables enabled, `SAVAGEWIND_NO_PREEMPT=1` does not appear to contribute an additional scheduling change. The combined test establishes that the **combined configuration** performs better, but the source indicates that `SAVAGEWIND_CORES=0` is the operative setting in that combination.

A controlled follow-up should compare: - neither variable; - `SAVAGEWIND_NO_PREEMPT=1` only; - `SAVAGEWIND_CORES=0` only; - both variables.

The `CORES=0`-only case is the critical next test.

## frame-pacing results
### 60-second window comparison

The logs each contain two `[perf] last 60 s` summaries. The second summary in each file is the closest comparison of the demanding portion of the route.

  Metric                        Baseline          Tuned                        Change
  --------------------------- ---------- -------------- -----------------------------
  Frames in 60 s                   3,369          3,532                         +4.8%
  Approx. average FPS              56.15          58.87                         +4.8%
  Frames on time                   70.3%      **95.3%**   **+25.0 percentage points**
  P50 frame time                17.00 ms       17.00 ms                     unchanged
  P95 frame time                21.25 ms   **18.00 ms**                        −15.3%
  P99 frame time                28.25 ms   **19.00 ms**                        −32.7%
  Reported hitches                    52          **1**                        −98.1%
  Render-thread CPU / frame     10.99 ms       13.29 ms                        +20.9%
  Guest-thread CPU / frame      49.29 ms       59.11 ms                        +19.9%
  GPU / frame                    5.78 ms        6.94 ms                        +20.1%

Approximate FPS is calculated as the reported frame count divided by 60 seconds. It is not a separate instantaneous FPS measurement.

The key result is not the small change in median frame time: P50 remains 17 ms in both runs. The improvement is in the tail of the frame-time distribution. P95 and especially P99 move substantially closer to the 17 ms target, and the hitch count collapses.

The CPU/GPU figures are accumulated per-frame work measurements from the profiler, not components that should be added together to obtain wall-clock frame duration. They also do not imply that the system became more efficient at executing each unit of game work. In this window, the tuned run reports more work per frame while meeting the presentation deadline more consistently.

### first 60-second windows

The first summaries show the same direction, though the baseline first interval is less representative of the heaviest section:

  Metric      Baseline, first window   Tuned, first window
  --------- ------------------------ ---------------------
  Frames                       3,483                 3,520
  On time                      93.5%                 99.5%
  P50                       17.00 ms              17.00 ms
  P95                       18.50 ms              17.25 ms
  P99                       23.25 ms              17.25 ms
  Hitches                         22                     9

This provides a second observation in the same direction: the tuned run has a tighter frame-time distribution and fewer hitches.

### individual high-load snapshots

Selected Vulkan profiler samples provide a useful cross-check:

  Sample                     Baseline, frame 8783   Tuned, frame 8682
  ------------------------ ---------------------- -------------------
  Reported FPS                               52.4                58.9
  Maximum frame time                      31.1 ms             19.7 ms
  On-time frames                            29.5%               88.1%
  Render CPU / frame                      13.7 ms             14.4 ms
  Guest CPU / frame                       61.7 ms             64.0 ms
  GPU / frame                              7.3 ms              7.5 ms
  Draws in sample window                  515,911             585,995

These are rolling profiler windows, not identical frames or a deterministic replay. Nevertheless, the tuned sample processes a higher draw count and reports slightly higher render, guest, and GPU work while showing materially better frame pacing.

A later tuned sample at frame 9390 reports 59.0 FPS, 17 ms maximum frame time, and 100% on-time frames, with 12.1 ms render CPU, 59.8 ms guest CPU, and 7.1 ms GPU per frame.

In `runtime/kernel.c`, `SAVAGEWIND_PROF_SVC` enables timing around `svc_impl()` for guest SVC calls below `0x80`.

The profiler records:

-   call count;
-   elapsed milliseconds spent inside the SVC implementation;
-   a bucket based on `thread_id & 31`.

It periodically emits records in this form:

``` text
[svcprof] t16 1a:1960/384ms 1b:8540/48ms 1c:552/103ms
```

The hexadecimal number is the SVC ID; the first number after the colon is the call count, and the second is cumulative elapsed milliseconds.

This is elapsed time inside the SVC implementation, **not CPU time**. It can include time blocked on a mutex, condition variable, address wait, or other synchronization primitive. Timings from different guest threads overlap and therefore must not be summed as if they were exclusive CPU execution. The `thread_id & 31` bucketing also means that the printed `tN` is not guaranteed to uniquely identify one guest thread.

### relevant SVC identifiers

The source maps the prominent IDs as follows:

  SVC ID   Operation
  -------- ------------------------
  `0x0B`   `SleepThread`
  `0x1A`   `ArbitrateLock`
  `0x1B`   `ArbitrateUnlock`
  `0x1C`   `WaitProcessWideKey`
  `0x1D`   `SignalProcessWideKey`
  `0x21`   `SendSyncRequest`
  `0x34`   `WaitForAddress`
  `0x35`   `SignalToAddress`

The profiler output shows that synchronization and waiting operations are a substantial part of the guest runtime's activity. In particular, `WaitProcessWideKey` (`0x1C`) has a high average elapsed duration per call in both runs.

### per-call elapsed-time comparison

The following values are calculated from the complete `[svcprof]` records in each supplied log. They are average elapsed milliseconds inside the corresponding SVC implementation per recorded call.

  SVC      Operation                Baseline mean   Tuned mean   Difference
  -------- ---------------------- --------------- ------------ ------------
  `0x0B`   SleepThread                  0.0767 ms    0.0160 ms       −79.2%
  `0x1A`   ArbitrateLock                0.2020 ms    0.0860 ms       −57.4%
  `0x1B`   ArbitrateUnlock             0.00689 ms   0.00651 ms        −5.5%
  `0x1C`   WaitProcessWideKey           6.2475 ms    6.2880 ms        +0.6%
  `0x1D`   SignalProcessWideKey        0.00352 ms   0.00242 ms       −31.3%
  `0x21`   SendSyncRequest              0.1316 ms    0.1130 ms       −14.1%
  `0x34`   WaitForAddress               0.2299 ms    0.1057 ms       −54.0%
  `0x35`   SignalToAddress             0.00868 ms   0.00793 ms        −8.6%

These are useful directional measurements, not a perfectly controlled microbenchmark. The baseline log spans approximately 104.5 seconds of Vulkan timestamps, while the tuned log spans approximately 64.1 seconds; the route and thread activity are not identical. Raw aggregate call counts and aggregate milliseconds are therefore not directly comparable. Per-call means reduce the effect of different recording durations, but they remain sensitive to differences in workload and contention.

The strongest directional changes are in `ArbitrateLock` and `WaitForAddress`: average elapsed time per call is lower by approximately 57% and 54%, respectively. This is consistent with reduced waiting/contending time in those synchronization paths. It does not, by itself, prove that every such wait was caused by the guest-core token scheduler.

`WaitProcessWideKey` is the counterexample: its mean remains approximately 6.25 ms per call in both runs. This indicates that a substantial wait path remains even after the scheduling change. The experiment did not remove all guest synchronization or all blocking.

### observation `SleepThread`

The profiler records substantially more `SleepThread` (`0x0B`) calls in the tuned log, despite the shorter log duration. This should not be interpreted as a regression in CPU usage.

The source implementation of `core_yield()` calls `sched_yield()` when the current thread has no held guest-core token. With `SAVAGEWIND_CORES=0`, that is the normal state. Consequently, guest `SleepThread(0)`/yield behaviour follows a different path than it does when the emulated core-token scheduler is active.

This is a concrete example of why call counts alone are insufficient: the same guest SVC can take a different host-side path depending on the runtime configuration. Its measured duration and frequency need to be interpreted together with the implementation.

## interpretation

The evidence supports the following conclusions:

1.  **Frame pacing improved substantially.** The strongest signal is the reduction in P95/P99 frame time and hitch count, rather than a dramatic change in P50.
2.  **The GPU does not appear to be the primary cause of the observed improvement.** GPU time per frame is similar between the selected high-load samples and slightly higher in the tuned 60-second window.
3.  **The runtime is not simply doing less work.** The tuned samples report equal or higher guest, render, and GPU work per frame while presenting more consistently.
4.  **Synchronization wait behaviour changed.** The SVC profile shows materially lower average elapsed time in `ArbitrateLock` and `WaitForAddress`, while `WaitProcessWideKey` remains expensive.
5.  **The operative setting in the combined configuration is almost certainly `SAVAGEWIND_CORES=0`.** With core-token emulation disabled, the `core_held` guard prevents the dispatch preemption path from running, making `SAVAGEWIND_NO_PREEMPT` effectively inactive in this combination.

The most defensible explanation is that removing the per-guest-core token serialization allows host-side guest threads to make progress concurrently rather than waiting for an emulated core hand-off. The reduced synchronization wait averages are consistent with that interpretation. The logs do not isolate every source of contention, so this remains a source-supported explanation rather than a proof of a single root cause.

## risks

The source's warning about the shared BOTW `sead` heap is significant. Threads pinned to one guest core can access that heap without a lock. The token scheduler exists partly to prevent those threads from executing concurrently. `SAVAGEWIND_CORES=0` removes that protection.

Potential consequences include: - heap free-list corruption; - intermittent crashes; - memory corruption that appears later than the triggering race; - save or gameplay-state corruption.

A stable run over a short route is not sufficient to establish correctness. Test longer sessions, scene transitions, loading and saving, busy NPC/physics areas, and repeated visits to the same locations. Keep a separate backup of the save data.

## source locations consulted and conclusions

The principal implementation references in the supplied source archive are:

-   `runtime/kernel.c`
    -   guest-core token scheduler and its rationale;
    -   `SAVAGEWIND_CORES` parsing;
    -   `SAVAGEWIND_NO_PREEMPT` handling;
    -   syscall-side preemption;
    -   `SAVAGEWIND_PROF_SVC` instrumentation and SVC timing;
    -   SVC dispatch table and operation IDs.
-   `runtime/dispatch.c`
    -   indirect-dispatch preemption check in `rt_dispatch()`.
-   `runtime/kernel.h`
    -   `core_held` and `g_core_best_wait` declarations.

The runtime configuration produced a large and measurable improvement in frame pacing in the tested high-load area, with P99 frame time falling from 28.25 ms to 19.00 ms and the selected 60-second hitch count falling from 52 to 1. The improvement occurred without a corresponding reduction in reported CPU or GPU work.

The source code points to disabling guest-core token scheduling as the operative change. This is a meaningful performance result, but it trades away a scheduling constraint that the source explicitly says protects BOTW's shared `sead` heap. The next step is not further stacking of flags; it is isolating `SAVAGEWIND_CORES=0` from `SAVAGEWIND_NO_PREEMPT=1` and performing longer correctness testing.
