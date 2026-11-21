// NX_KERNEL_SELFTEST=cv: the condvar svcs against nn::os's protocol, run on host threads before the game starts (the
// exe exits with 0 when every case passes). The guest's mutex is a word in memory taken by CAS in user space (svcs
// only when contended); its condvar Signal (InternalConditionVariableImplByHorizon::Signal, Wonder's sdk+171590) reads
// the condvar word and skips svcSignalProcessWideKey when it is 0. Each case runs with the order before the fix
// (mutex released, then the condvar word set) and with the fixed one; the old order must lose a wakeup where marked.
//   1. interleaving: a seam stops the waiter right after the mutex is released; another thread then does what the
//      last reader of a reader-writer lock does (ReleaseReadLock: take the mutex, read the condvar word, signal if it
//      is set, release). Deterministic.
//   2. stress: a waiter and a signaler spinning on the mutex hand a flag back and forth 100,000 times, no seam; a lost
//      wakeup shows as a timed-out wait that finds the flag set.
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>

#include "kernel_state.h"
#include "platform.h"

namespace nx {

using namespace kstate;

namespace {

uint32_t ld32(uint64_t a) { return __atomic_load_n(reinterpret_cast<uint32_t*>(a), __ATOMIC_SEQ_CST); }
void st32(uint64_t a, uint32_t v) { __atomic_store_n(reinterpret_cast<uint32_t*>(a), v, __ATOMIC_SEQ_CST); }
bool cas(uint64_t a, uint32_t expected, uint32_t v) {
    return __atomic_compare_exchange_n(reinterpret_cast<uint32_t*>(a), &expected, v, false, __ATOMIC_SEQ_CST,
                                       __ATOMIC_SEQ_CST);
}

// nn::os's user-space mutex (InternalCriticalSection on Horizon): 0 free, else the owner's tag, | wait bit when
// someone sleeps in the kernel for it.
void lock(uint64_t m, uint32_t tag) {
    for (;;) {
        uint32_t v = ld32(m);
        if (v == 0) {
            if (cas(m, 0, tag)) return;
            continue;
        }
        if (!(v & kHandleWaitMask) && !cas(m, v, v | kHandleWaitMask)) continue;
        svc_arbitrate_lock(v & ~kHandleWaitMask, m, tag);
        if ((ld32(m) & ~kHandleWaitMask) == tag) return;  // handed over by the kernel
    }
}
void unlock(uint64_t m, uint32_t tag) {
    for (;;) {
        uint32_t v = ld32(m);
        if (v & kHandleWaitMask) {
            svc_arbitrate_unlock(m);
            return;
        }
        if (cas(m, v, 0)) return;
    }
    (void)tag;
}
// InternalConditionVariableImplByHorizon::Signal: no svc when the word reads 0.
void signal(uint64_t key) {
    if (ld32(key) != 0) svc_signal_process_wide_key(key, 1);
}

std::shared_ptr<KThread> make_thread(uint32_t handle) {
    auto t = std::make_shared<KThread>();
    t->handle = handle;
    t->id = handle;
    t->started = true;
    return t;
}

// ---- 1. the interleaving ------------------------------------------------------------------------------------------
struct Gate {
    std::mutex m;
    std::condition_variable cv;
    int stage = 0;  // 1: the waiter released the mutex; 2: the reader decided
} * g_gate = nullptr;

void after_release() {  // in the waiter's svc, kernel lock held: let the reader run, wait for its decision
    std::unique_lock lk(g_gate->m);
    g_gate->stage = 1;
    g_gate->cv.notify_all();
    g_gate->cv.wait_for(lk, std::chrono::seconds(2), [] { return g_gate->stage >= 2; });
}

// true when the waiter was woken holding the mutex; *read_zero: the reader found the condvar word 0 (no signal)
bool interleaving(bool release_first, bool* read_zero) {
    alignas(8) static uint32_t words[4];
    std::memset(words, 0, sizeof words);
    const uint64_t m = uint64_t(&words[0]), key = uint64_t(&words[2]);
    constexpr uint32_t kW = 0x7001, kR = 0x7002;
    Gate gate;
    g_gate = &gate;
    g_cv_release_first = release_first;
    g_cv_after_release = after_release;
    auto W = make_thread(kW), R = make_thread(kR);
    uint32_t rc = ~0u, owner_after = 0;
    std::thread waiter([&] {
        t_self = W.get();
        lock(m, kW);  // as AcquireWriteLock: the mutex held while it sees readers and decides to wait
        rc = svc_wait_process_wide_key(m, key, kW, 1'000'000'000);  // 1 s: past it, the wakeup was lost
        owner_after = ld32(m) & ~kHandleWaitMask;
        if (rc == 0) unlock(m, kW);
    });
    std::thread reader([&] {
        t_self = R.get();
        {
            std::unique_lock lk(gate.m);
            gate.cv.wait(lk, [&] { return gate.stage >= 1; });
        }
        g_cv_after_release = nullptr;  // only the waiter's svc stops at the seam
        lock(m, kR);                   // ReleaseReadLock's slow path: under the mutex,
        *read_zero = ld32(key) == 0;   // the last reader decides to signal from the condvar word
        {
            std::lock_guard lk(gate.m);
            gate.stage = 2;
            gate.cv.notify_all();
        }
        signal(key);
        unlock(m, kR);
    });
    reader.join();
    waiter.join();
    g_cv_after_release = nullptr;
    g_cv_release_first = false;
    g_gate = nullptr;
    return rc == 0 && owner_after == kW;
}

// ---- 2. stress ------------------------------------------------------------------------------------------------------
struct StressResult {
    int rounds = 0, lost = 0, woken = 0;  // woken: waits that slept in the kernel and were signalled
    double ms = 0;
};

StressResult stress(bool release_first, int rounds) {
    alignas(8) static uint32_t words[4];
    std::memset(words, 0, sizeof words);
    const uint64_t m = uint64_t(&words[0]), key = uint64_t(&words[2]);
    std::atomic<uint32_t> flag{0}, ready{0};  // ready: the waiter holds the mutex and is about to wait
    constexpr uint32_t kW = 0x7101, kS = 0x7102;
    g_cv_release_first = release_first;
    auto W = make_thread(kW), S = make_thread(kS);
    StressResult r;
    std::atomic<bool> done{false};
    const auto t0 = std::chrono::steady_clock::now();
    std::thread waiter([&] {
        t_self = W.get();
        for (int i = 0; i < rounds && r.lost < 20; i++) {
            lock(m, kW);
            ready.store(1);
            while (flag.load() == 0) {
                uint32_t rc = svc_wait_process_wide_key(m, key, kW, 100'000'000);  // 100 ms
                if (rc == 0) r.woken++;
                if (rc != 0) {  // timed out without the mutex: take it back, as the SDK does
                    lock(m, kW);
                    if (flag.load() != 0) r.lost++;  // the signal came and the wait never saw it
                }
            }
            flag.store(0);
            ready.store(0);
            unlock(m, kW);
            r.rounds++;
        }
        done = true;
    });
    std::thread signaler([&] {
        t_self = S.get();
        while (!done) {
            while (!done && ready.load() == 0) {}  // until the waiter holds the mutex, about to wait
            // then CAS on the mutex without pause: taken the moment the waiter's svc releases it (the window)
            uint32_t v;
            while (!done && ((v = ld32(m)) != 0 || !cas(m, 0, kS))) {}
            if (done) break;
            if (flag.load() == 0 && ready.load() != 0) {
                flag.store(1);
                signal(key);
            }
            unlock(m, kS);
            while (!done && flag.load() != 0) {}  // until the waiter took it
        }
    });
    waiter.join();
    signaler.join();
    g_cv_release_first = false;
    r.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    return r;
}

}  // namespace

int kernel_selftest(const char* which) {
    if (std::strcmp(which, "cv") != 0) {
        log("kernel selftest: unknown case %s (cases: cv)", which);
        return 2;
    }
    int failures = 0;
    for (bool old_order : {true, false}) {
        const char* name = old_order ? "old order (release, then condvar word)" : "fixed order (condvar word first)";
        bool read_zero = false;
        bool woken = interleaving(old_order, &read_zero);
        // the old order must reproduce the lost wakeup here; the fixed one must not
        bool as_expected = old_order ? (!woken && read_zero) : (woken && !read_zero);
        log("kernel selftest cv interleaving, %s: reader read the condvar word as %s, waiter %s -> %s", name,
            read_zero ? "0 (no signal)" : "1 (signalled)", woken ? "woken holding the mutex" : "NOT woken (lost wakeup)",
            as_expected ? "as expected" : "UNEXPECTED");
        failures += !as_expected;
        StressResult s = stress(old_order, 100'000);
        bool ok = old_order || (s.lost == 0 && s.woken == s.rounds);  // fixed: every hand-off slept and was signalled
        log("kernel selftest cv stress, %s: %d hand-offs, %d waits woken by a signal, %d lost wakeups, %.0f ms -> %s", name, s.rounds, s.woken, s.lost, s.ms,
            ok ? "ok" : "FAIL");
        failures += !ok;
    }
    log("kernel selftest cv: %s", failures ? "FAILED" : "passed");
    return failures ? 1 : 0;
}

}  // namespace nx
