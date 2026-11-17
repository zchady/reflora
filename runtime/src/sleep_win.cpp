// plat::sleep_ns on Windows. The timer ticks every 15.6 ms by default: sleep_for(20 us) took 4.7-16.5 ms (11.7 on
// average). Wonder's Switch 2 job system sleeps 20 us in its wait loops (main+c3e1c/c3f8c/c3fcc), on the main thread
// and the workers, and those sleeps were ~30 ms of each 43 ms course frame (tools/timeline.py,
// WONDER_SWITCH2_PROGRESS.md §5s). The bulk of a sleep now waits on a high-resolution waitable timer (Windows 10
// 1803+; this thread's own handle, no change to the system's timer resolution) and the last stretch yields the core
// until the deadline, so a sleep lasts what it asked for. NX_SLEEP_LEGACY=1: sleep_for, as before.
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <thread>

#include <windows.h>

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002  // Windows 10 1803 SDK
#endif

namespace nx::plat {

namespace {
uint64_t qpc_ns() {
    static const double ns_per_count = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return 1e9 / double(f.QuadPart);
    }();
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return uint64_t(double(t.QuadPart) * ns_per_count);
}
}  // namespace

void sleep_ns(int64_t ns) {
    if (ns <= 0) return;
    static const bool legacy = std::getenv("NX_SLEEP_LEGACY") != nullptr;
    if (legacy) {
        std::this_thread::sleep_for(std::chrono::nanoseconds(ns));
        return;
    }
    constexpr int64_t kYieldNs = 1'000'000;  // the high-resolution timer's own slack; shorter sleeps only yield
    const uint64_t until = qpc_ns() + uint64_t(ns);
    if (ns > kYieldNs) {
        thread_local HANDLE timer =
            CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        LARGE_INTEGER due;
        due.QuadPart = -((ns - kYieldNs) / 100);  // relative, in 100 ns units
        if (timer && SetWaitableTimerEx(timer, &due, 0, nullptr, nullptr, nullptr, 0))
            WaitForSingleObject(timer, INFINITE);
        else
            std::this_thread::sleep_for(std::chrono::nanoseconds(ns - kYieldNs));
    }
    while (qpc_ns() < until) SwitchToThread();
}

}  // namespace nx::plat
