// The kernel's own state, shared by kernel.cpp (handles, threads, waits, svc dispatch) and kernel_watch.cpp (hang
// report, stall sampler, backtraces). Not for the rest of the runtime: kernel.h is the interface.
#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "kernel.h"

namespace nx::kstate {

constexpr uint32_t kHandleWaitMask = 0x40000000;
constexpr uint32_t kPseudoCurrentThread = 0xFFFF8000;
constexpr uint32_t kPseudoCurrentProcess = 0xFFFF8001;

extern std::recursive_mutex g_k;              // the "kernel lock"
extern std::vector<KThread*> g_sync_waiters;  // threads in WaitSynchronization: woken by any change of waitable state
extern std::unordered_map<uint32_t, std::shared_ptr<KObject>> g_handles;
extern uint32_t g_next_handle;
extern std::atomic<uint64_t> g_next_thread_id;
extern uint64_t g_heap_size;

extern std::vector<KThread*> g_mutex_waiters;  // FIFO
extern std::vector<KThread*> g_cv_waiters;
extern std::vector<KThread*> g_addr_waiters;

extern thread_local KThread* t_self;

extern std::mutex g_all_lock;  // every thread ever created, for the hang report
extern std::vector<std::weak_ptr<KThread>> g_all;

// kernel_selftest.cpp's seams in svcWaitProcessWideKeyAtomic: the order before the fix (mutex released, then the
// condvar word set) and a call right after the release. Never set outside the self-test.
extern bool g_cv_release_first;
extern void (*g_cv_after_release)();

}  // namespace nx::kstate

namespace nx {
// The mutex and condvar svcs (kernel.cpp), called by the svc dispatch and by kernel_selftest.cpp (t_self set).
uint32_t svc_arbitrate_lock(uint32_t owner, uint64_t addr, uint32_t tag);
uint32_t svc_arbitrate_unlock(uint64_t addr);
uint32_t svc_wait_process_wide_key(uint64_t mutex, uint64_t key, uint32_t tag, int64_t timeout);
uint32_t svc_signal_process_wide_key(uint64_t key, int32_t count);
}  // namespace nx
