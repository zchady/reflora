// Frame timeline (timeline.cpp): a few seconds of every guest thread's blocking waits and wake-ups (at the SVC level),
// the NVN calls that make a frame (submit, present, acquire, sync waits) and the display vsync, with host
// timestamps, to find the chain of waits that sets a frame's length. A file named `timeline` in the data folder
// (holding a number of seconds, 3 by default) starts it; timeline.txt is written next to it when it ends. Off
// otherwise (one relaxed atomic load per call).
#pragma once

#include <atomic>
#include <cstdint>

namespace nx {
struct CPU;
}

namespace nx::tl {

enum Kind : uint8_t {
    kBlockBegin = 1,  // a blocking SVC begins: code = svc, a = what it waits on, b = its second argument
    kBlockEnd = 2,    // ... and returns: code = svc, a as for the begin, b = the result
    kSignal = 3,      // a waking SVC: code = svc, a = what it signals
    kNvn = 4,         // code = NvnEvent, a/b by event
    kVsync = 5,       // the display vsync (hle_vi.cpp)
};

enum NvnEvent : uint16_t {
    kSubmitBegin = 1, kSubmitEnd, kPresentBegin, kPaceBegin, kPaceEnd, kPresentEnd, kAcquire, kSyncWaitBegin,
    kSyncWaitEnd, kFinishBegin, kFinishEnd, kFrameTime /* a = fw[0x130] ticks, b = interval */,
    kReport /* replayed ReportCounter: a = address, b = type */, kQueueWaitSync /* a = sync */,
};

extern std::atomic<bool> g_on;
inline bool on() { return g_on.load(std::memory_order_relaxed); }

// c (may be null): the guest context, for the guest call chain (lr and three frame records).
void record(uint8_t kind, uint16_t code, uint64_t a, uint64_t b, const CPU* c);

// The watchdog (kernel_watch.cpp), once a second: starts on request and writes the file when it is over.
void poll();

}  // namespace nx::tl
