// NVN events (NVN 2: Wonder's Switch 2 Edition uses them, the SDK 16 game did not). An event is a 32-bit value in a
// memory pool (nvnEventBuilderSetStorage). The CPU reads it (nvnEventGetValue) and writes it (nvnEventSignal); the
// GPU writes it when a command buffer gets to nvnCommandBufferSignalEvent, and waits on it at nvnCommandBufferWaitEvent.
// The value lives in the pool's own memory, so the game, our replay and any other reader see the same word.
// NX_TRACE_EVENTS=<frame>: the signals and waits from that frame on.
//
// Arguments as Wonder passes them (NX_NVN_REAL traces): nvnEventSignal(event, mode, value) with mode 0 = write;
// nvnCommandBufferSignalEvent(cmd, event, mode, location, flags, value); nvnCommandBufferWaitEvent(cmd, event,
// mode, value). Mode 1 is taken as "add", as the counterpart of write.
#include <cstdlib>

#include "nvn.h"

namespace nx::nvn {

void queue_event_signaled(uint64_t storage);  // nvn_queue.cpp

namespace {

struct Event : Object {
    uint64_t storage = 0;  // host address of the 32-bit value
};

uint64_t storage_of(uint64_t event) {
    auto* e = get<Event>(event);
    return e ? e->storage : 0;
}

// NVNboolean nvnEventInitialize(NVNevent*, const NVNeventBuilder*)
void EventInitialize(CPU& c) {
    uint64_t event = c.x[0], builder = c.x[1];
    uint64_t pool = driver(c, "nvnEventBuilderGetMemoryPool", {builder});
    uint64_t offset = driver(c, "nvnEventBuilderGetMemoryOffset", {builder});
    uint64_t storage = 0;
    if (auto* p = get<MemoryPool>(pool)) storage = pool_host(*p, offset);
    if (!storage) {  // no pool we know: a word of our own keeps CPU-side use working
        storage = kernel_alloc(0x1000);
        log("nvn: event %llx: storage pool %llx + %llx not mapped, using a private word", (unsigned long long)event,
            (unsigned long long)pool, (unsigned long long)offset);
    }
    make<Event>(event).storage = storage;
    c.x[0] = 1;
}

void EventFinalize(CPU& c) { destroy(c.x[0]); }

void EventGetValue(CPU& c) {  // uint32_t (const NVNevent*)
    uint64_t s = storage_of(c.x[0]);
    c.x[0] = s ? __atomic_load_n(reinterpret_cast<uint32_t*>(s), __ATOMIC_ACQUIRE) : 0;
}

bool event_trace() {
    static const uint64_t from = std::getenv("NX_TRACE_EVENTS") ? std::strtoull(std::getenv("NX_TRACE_EVENTS"), nullptr, 10) : 0;
    const bool on = from && g_frame >= from;
    return on;
}
int g_event_lines = 0;

void EventSignal(CPU& c) {  // (NVNevent*, mode, uint32_t value)
    uint64_t s = storage_of(c.x[0]);
    if (event_trace() && g_event_lines < 200)
        g_event_lines++, log("event: CPU signal %llx mode %u value %u (was %u), lr %s", (unsigned long long)s, uint32_t(c.x[1]),
                             uint32_t(c.x[2]), s ? *reinterpret_cast<uint32_t*>(s) : 0u, describe(c.x[30]).c_str());
    if (!s) return;
    auto* p = reinterpret_cast<uint32_t*>(s);
    uint32_t v = uint32_t(c.x[2]);
    if (uint32_t(c.x[1]) == 1)
        __atomic_fetch_add(p, v, __ATOMIC_ACQ_REL);
    else
        __atomic_store_n(p, v, __ATOMIC_RELEASE);
    queue_event_signaled(s);  // a submit waiting on it may go now
}

// (cmd, const NVNevent*, mode, location, flags, uint32_t value): written by the replay, in order.
void CommandBufferSignalEvent(CPU& c) {
    Cmd cmd{Cmd::SignalEvent};
    cmd.a[0] = storage_of(c.x[1]);
    cmd.n = uint32_t(c.x[2]);
    cmd.a[1] = uint32_t(c.x[5]);
    if (event_trace() && g_event_lines < 200)
        g_event_lines++, log("event: GPU signal recorded %llx mode %u value %u", (unsigned long long)cmd.a[0], cmd.n,
                             uint32_t(cmd.a[1]));
    record(c.x[0], cmd);
}

// (cmd, const NVNevent*, mode, uint32_t value): the GPU waits until the event satisfies the mode. Recorded: the queue
// holds the submit until then (nvn_queue.cpp); a wait on work submitted earlier is met by the replay's order.
void CommandBufferWaitEvent(CPU& c) {
    Cmd cmd{Cmd::WaitEvent};
    cmd.a[0] = storage_of(c.x[1]);
    cmd.n = uint32_t(c.x[2]);
    cmd.a[1] = uint32_t(c.x[3]);
    if (event_trace() && g_event_lines < 200)
        g_event_lines++, log("event: GPU wait recorded %llx mode %u value %u (now %u), lr %s", (unsigned long long)cmd.a[0],
                             cmd.n, uint32_t(cmd.a[1]), cmd.a[0] ? *reinterpret_cast<uint32_t*>(cmd.a[0]) : 0u,
                             describe(c.x[30]).c_str());
    record(c.x[0], cmd);
}

}  // namespace

NX_HLE("nvnEventInitialize", EventInitialize);
NX_HLE("nvnEventFinalize", EventFinalize);
NX_HLE("nvnEventGetValue", EventGetValue);
NX_HLE("nvnEventSignal", EventSignal);
NX_HLE("nvnCommandBufferSignalEvent", CommandBufferSignalEvent);
NX_HLE("nvnCommandBufferWaitEvent", CommandBufferWaitEvent);

}  // namespace nx::nvn
