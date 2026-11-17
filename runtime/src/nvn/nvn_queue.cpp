// NVN queues, command buffers, windows and syncs (docs/NVN.md, N3). Synchronously every command completes at
// submit: fences are signaled right away, the window hands out its textures in turn, and presents are counted. With
// NX_ASYNC_GPU a fence is the submit it follows, and waiting for it waits for that submit (nvn_frames.cpp). With
// NX_PIPELINED the GPU work is in flight when a submit returns but its compute results are in guest memory, so
// syncs and nvnQueueFinish have nothing to wait for, as synchronously (gpu_serial() is 0). Return
// values follow the SDK driver (nvnSyncWait sdk+45a960: 0 once the fence has passed; nvnWindowAcquireTexture
// sdk+45e050: 0 on success).
#include <algorithm>
#include <functional>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <deque>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <array>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <atomic>
#include <vector>

#include <nx/game.h>

#include "hitch.h"
#include "nvn.h"
#include "platform.h"
#include "timeline.h"

namespace nx::nvn {
// The CPU set an event (nvn_event.cpp, its storage): a submit waiting on it may go now.
void queue_event_signaled(uint64_t storage);

// Submits the game made and submits whose replay (and GPU work, write-backs) finished. The game records the next frame
// on one thread while another submits the last one, and our replay runs when the submits are handed over (at the
// submit, unless its queue waits: queue_item below): a fence the game waits on must not pass before the submits it
// follows were replayed, or the game rewrites the next frame's vertex and uniform data while this one is still read
// (the horizon band, exploded meshes, DEBUG.md #59).
// NX_SYNC_LOOSE=1: the old behaviour (fences passed at once).
namespace {
std::mutex g_sub_lock;
std::condition_variable g_sub_cv;
uint64_t g_sub_called = 0, g_sub_done = 0;
std::atomic<uint64_t> g_sub_blocked{0};

bool sync_loose() {
    static const bool on = std::getenv("NX_SYNC_LOOSE") != nullptr;
    return on;
}

void submit_done() {
    {
        std::lock_guard lk(g_sub_lock);
        g_sub_done++;
    }
    g_sub_cv.notify_all();
}

// Where the game waits for the replay (docs/nvn/android-perf/PHASE3.md I1): calls, the ones that blocked and for how
// long, per 600 presents; frame_ns is this main-thread frame's blocked time, for the hitch lines (frame_time_probe).
struct WaitSite {
    const char* name;
    std::atomic<uint64_t> calls{0}, blocked{0}, ns{0}, frame_ns{0};
};
WaitSite g_wait_sites[kWaitSites] = {{"sync wait"}, {"queue finish"}, {"pool invalidate"}, {"pool finalize"}};

// Waits until the submits up to `seq` were replayed (2 s at most: a hung replay is logged, not waited forever).
void wait_submits(uint64_t seq, int site) {
    if (sync_loose() || !seq) return;
    WaitSite& w = g_wait_sites[site];
    w.calls++;
    std::unique_lock lk(g_sub_lock);
    if (g_sub_done >= seq) return;
    uint64_t n = ++g_sub_blocked;
    if (n == 1 || n % 1000 == 0) log("nvn: %llu CPU waits blocked on a submit still being replayed", (unsigned long long)n);
    uint64_t t0 = plat::monotonic_ns();
    if (!g_sub_cv.wait_for(lk, std::chrono::seconds(2), [seq] { return g_sub_done >= seq; }))
        log("nvn: a CPU wait gave up after 2 s on submit %llu (%llu done)", (unsigned long long)seq,
            (unsigned long long)g_sub_done);
    uint64_t dt = plat::monotonic_ns() - t0;
    w.blocked++, w.ns += dt, w.frame_ns += dt;
}

void log_wait_sites() {
    std::string line;
    for (WaitSite& w : g_wait_sites) {
        char b[128];
        std::snprintf(b, sizeof b, "; %s %llu calls, %llu blocked %.1f ms", w.name, (unsigned long long)w.calls.exchange(0),
                      (unsigned long long)w.blocked.exchange(0), double(w.ns.exchange(0)) / 1e6);
        line += b;
    }
    log("nvn: game waits on the replay over 600 presents%s", line.c_str());
}

uint64_t submits_called() {
    std::lock_guard lk(g_sub_lock);
    return g_sub_called;
}
}  // namespace

// MemoryPoolInvalidateMappedRange / Finalize (nvn_objects.cpp): the CPU reads or frees what submits made so far use.
namespace {
void flush_pending();
}
void wait_submits_so_far(int site) {
    flush_pending();
    wait_submits(submits_called(), site);
}

namespace {

// ---- syncs ------------------------------------------------------------------------------------------------------

struct Sync : Object {
    std::atomic<bool> signaled{false};
    std::atomic<uint64_t> serial{0};  // NX_ASYNC_GPU: the submit it waits for (0: none; always 0 otherwise)
    std::atomic<uint64_t> job{0};     // the replay-thread job that fences it (0: none)
    std::atomic<uint64_t> seq{0};     // the submits it follows (wait_submits); 0: none
    // Its last fence's position in the queues' lines (FenceSync), and the last one handed over to the replay.
    std::atomic<uint64_t> fence_queued{0}, fence_handed{0};
    // A window's texture-available sync (nvnWindowAcquireTexture): it passes once the display stops showing the
    // texture, when present number `release` is shown (display_wait). 0: no display wait.
    uint64_t window = 0, release = 0;
};

void SyncInitialize(CPU& c) {  // NVNboolean (NVNsync*, NVNdevice*)
    make<Sync>(c.x[0]);
    c.x[0] = 1;
}

void SyncFinalize(CPU& c) { destroy(c.x[0]); }

void signal_sync(uint64_t guest) {
    if (auto* s = get<Sync>(guest))
        s->serial = 0, s->job = 0, s->seq = 0, s->window = s->release = 0, s->signaled = true,
        s->fence_handed = s->fence_queued.load();
}

// ---- the display ------------------------------------------------------------------------------------------------
//
// On the Switch nvnQueuePresentTexture does not block: the display shows each presented texture for `interval`
// vsyncs, and nvnWindowAcquireTexture hands out a texture with a sync that passes once the display has moved off it.
// Wonder waits on that sync with nvnSyncWait (Switch 2: main+1bbb68, in sead's procFrame) right before it takes the
// next frame's start tick (fw[0xf8]), so its 30/60 governor measures the frame's work, not the wait for the display.
// The runtime used to sleep inside the present instead (pace_present): the render thread then held the next frame
// past the start tick, every frame measured ~33 ms and the governor never left 30 fps (§5s). Here a present only
// schedules when it is shown, D(p) = max(now, D(p-1) + interval vsyncs) (a late frame is shown at once, not
// snapped to the next vsync), and the acquired texture's sync waits for D of the present after the one that last
// showed it. On for Switch 2 games (game::Info::tick_hz); the Switch games keep the pacing in the present they
// were tuned with. NX_PACE_AT_PRESENT=1: the old pacing (acquire syncs pass at once), =0: the display model.
// NX_NO_FRAME_LIMIT=1: no pacing at all.
bool pace_at_present() {
    static const bool on = [] {
        const char* e = std::getenv("NX_PACE_AT_PRESENT");
        return e ? e[0] == '1' : game::kInfo.tick_hz == 0;
    }();
    return on;
}
bool frame_limit_off() {
    static const bool off = std::getenv("NX_NO_FRAME_LIMIT") != nullptr;
    return off;
}
std::mutex g_display_lock;
std::condition_variable g_display_cv;

// The display model's clock is the monitor's (NX_VSYNC_LOCK=0: the host clock's 60.000 Hz, as before). A frame is
// 16.667 ms of the host clock, but the monitor's refresh is its own clock: at 180 Hz the presents slid across the
// refresh at ~0.05 ms per second, and for the ~20 s in which they fell next to a vblank each one landed just before
// or just after it: frames shown for 2 and 4 refreshes in turn (11 and 22 ms) with the game at a perfect 60 fps,
// every ~110 s (run D, §5w). When the monitor's refresh is a multiple of 60 Hz (within 0.5%: 60, 120, 180, 240...),
// a frame is that many refreshes of the monitor's measured period, and its display time is the middle of a refresh:
// as far as it can be from the vblanks on both sides. The game then runs at the monitor's 60 Hz (59.997 here), as
// it does on the console's own display. Other refresh rates (144, 165 Hz) keep the host clock.
bool vsync_lock() {
    static const bool on = [] {
        const char* e = std::getenv("NX_VSYNC_LOCK");
        return !(e && e[0] == '0');
    }();
    return on;
}

// A present of texture `index`: when it is shown. Returns that time (host ns).
uint64_t display_schedule(Window& w, int index, int interval) {
    uint64_t now = plat::monotonic_ns();
    uint64_t vblank = 0, refresh = 0, frame = 16'666'667ull;
    bool locked = false;
    if (vsync_lock() && !frame_limit_off() && plat::monitor_vblank(vblank, refresh) && refresh) {
        uint64_t n = (frame + refresh / 2) / refresh;  // refreshes in a 60 Hz frame
        uint64_t of = n * refresh;
        if (n >= 1 && (of > frame ? of - frame : frame - of) * 200 < frame) frame = of, locked = true;
    }
    static bool was_locked = false;
    if (locked != was_locked) {
        was_locked = locked;
        if (locked)
            log("nvn: display model on the monitor's clock: %.3f Hz, a frame is %llu refreshes (%.4f ms)",
                1e9 / double(refresh), (unsigned long long)(frame / refresh), double(frame) / 1e6);
        else
            log("nvn: display model on the host clock (no monitor vblank, or a refresh that is no multiple of 60 Hz)");
    }
    std::lock_guard lk(g_display_lock);
    uint64_t prev = w.presents ? w.shown_at[w.presents % Window::kShown] : 0;
    uint64_t period = frame_limit_off() ? 0 : uint64_t(std::max(interval, 1)) * frame;
    uint64_t at = std::max(now, prev + period);
    if (locked) {
        // The middle of the refresh nearest to `at`. A late frame (at = now) past that middle is shown at once, unless
        // the next vblank is under 1 ms away: then from the middle of the next refresh (it would not make this one).
        const int64_t T = int64_t(refresh), half = T / 2;
        const int64_t d = int64_t(at) - int64_t(vblank);
        const int64_t k = d >= 0 ? d / T : -((-d + T - 1) / T);  // the refresh [begin, begin + T) that `at` is in
        const int64_t begin = int64_t(vblank) + k * T, mid = begin + half;
        const bool late = prev + period < now;
        if (!late || int64_t(at) <= mid)
            at = uint64_t(mid);  // on time: the middle of its refresh; late, before the middle: from the middle
        else if (begin + T - int64_t(at) < 1'000'000)
            at = uint64_t(mid + T);
    }
    w.presents++;
    w.shown_at[w.presents % Window::kShown] = at;
    if (index >= 0) {
        if (w.last_present.size() <= size_t(index)) w.last_present.resize(size_t(index) + 1, 0);
        w.last_present[size_t(index)] = w.presents;
    }
    g_display_cv.notify_all();
    return at;
}

// nvnSyncWait on a texture-available sync: until present `release` of the window is shown, or the timeout (ns).
// NVNsyncWaitResult: 0 once it has passed, 2 timeout expired.
uint64_t display_wait(uint64_t window, uint64_t release, uint64_t timeout) {
    uint64_t t0 = plat::monotonic_ns();
    uint64_t limit = std::min<uint64_t>(timeout, 2'000'000'000ull);  // a present that never comes: 2 s, logged
    std::unique_lock lk(g_display_lock);
    auto* w = get<Window>(window);
    if (!w) return 0;
    if (w->presents < release) {
        if (timeout == 0) return 2;
        if (!g_display_cv.wait_for(lk, std::chrono::nanoseconds(limit), [&] { return w->presents >= release; })) {
            if (timeout > limit) log("nvn: a texture-available sync waited 2 s for present %llu (%llu so far)",
                                     (unsigned long long)release, (unsigned long long)w->presents);
            return timeout > limit ? 0 : 2;
        }
    }
    uint64_t at = w->presents - release < Window::kShown ? w->shown_at[release % Window::kShown] : 0;
    lk.unlock();
    uint64_t now = plat::monotonic_ns();
    if (at <= now) return 0;
    if (timeout == 0) return 2;
    uint64_t left = timeout - std::min(timeout, now - t0);
    if (tl::on()) tl::record(tl::kNvn, tl::kPaceBegin, at - now, 0, nullptr);
    plat::sleep_ns(int64_t(std::min(at - now, left)));
    if (tl::on()) tl::record(tl::kNvn, tl::kPaceEnd, 0, 0, nullptr);
    return at - now <= left ? 0 : 2;  // passed (0, as for fences: the SDK's own result once it has passed)
}

// NVNsyncWaitResult (const NVNsync*, u64 timeout). Always 0 (already signaled), as before: a fenced submit is waited
// for whatever the timeout, 2 s at most (then the GPU hung and the process ends). Correct results before latency.
bool queue_trace();
extern int g_queue_lines;
void SyncWait(CPU& c) {
    if (queue_trace() && g_queue_lines < 300)
        if (auto* s = get<Sync>(c.x[0]))
            g_queue_lines++, log("queue: CPU waits for sync %llx (fence %llu, handed %llu, window %llu), lr %s",
                                 (unsigned long long)c.x[0], (unsigned long long)s->fence_queued.load(),
                                 (unsigned long long)s->fence_handed.load(), (unsigned long long)s->release,
                                 describe(c.x[30]).c_str());
    if (tl::on()) tl::record(tl::kNvn, tl::kSyncWaitBegin, c.x[0], 0, &c);
    struct TlEnd {
        uint64_t t0 = plat::monotonic_ns();
        ~TlEnd() {
            if (tl::on()) tl::record(tl::kNvn, tl::kSyncWaitEnd, 0, 0, nullptr);
            hitch::add(hitch::kSyncWaitNs, plat::monotonic_ns() - t0);
        }
    } tl_end;
    auto* s = get<Sync>(c.x[0]);
    // Its fence may still wait in a queue's line (queue_item): until it is handed over (another thread's event signal
    // lets it go), 2 s at most.
    if (s && s->fence_handed.load() < s->fence_queued.load()) {
        const uint64_t t0 = plat::monotonic_ns();
        while (s->fence_handed.load() < s->fence_queued.load() && plat::monotonic_ns() - t0 < 2'000'000'000ull) {
            flush_pending();
            if (s->fence_handed.load() < s->fence_queued.load()) plat::sleep_ns(200'000);
        }
    }
    if (s) wait_submits(s->seq.load(), kWaitSync);
    if (s) worker_wait(s->job);
    if (uint64_t serial = s ? s->serial.load() : 0) gpu_wait(serial);
    c.x[0] = s && s->release ? display_wait(s->window, s->release, c.x[1]) : 0;
}

// ---- queues -----------------------------------------------------------------------------------------------------

std::atomic<uint64_t> g_presents{0};

// When the replay takes what the game submitted: at the submit, in order, unless the submit's queue is waiting. Each
// NVN queue runs its commands in order and stops at a nvnCommandBufferWaitEvent until the event holds the value; the
// other queues go on. Wonder (5 queues) starts each frame's main command buffer with a wait for an event the CPU sets
// (nvnEventSignal, one of two alternating events: reset to 0, then 1) once it has written that frame's per-frame data,
// after the submit: on the World 1 map, the shadow pass's light camera in the (double-buffered) uniform memory.
// Replayed at the submit, the shadow map was drawn with the camera of two frames before while the receivers sampled it
// with the current one: offset shadows, the dark angular patches flashing on the map while walking. So a submit that
// waits for a CPU-set event not set yet waits in its queue's line (with what its queue submits after it: submits,
// fences, nvnQueueWaitSync) until the CPU sets it, or a second (logged). Waits for events a command buffer signals
// (GPU to GPU, between queues and in the middle of command buffers) are met by the replay's order, as before.
// NX_EVENT_WAITS=0: not modeled, every submit replayed at once as before.
bool event_waits_on() {
    static const bool on = [] {
        const char* e = std::getenv("NX_EVENT_WAITS");
        return !(e && e[0] == '0');
    }();
    return on;
}
struct EventWait {
    uint64_t storage;  // host address of the event's value
    uint32_t mode, value;
};
// Wonder's events are counters (GPU signals 7104, 7105, ...) or a word reset to 0 then set to 1, and its waits use
// mode 0: met once the value reached the one waited for (wrapping compare). An exact compare would never be met once
// a later signal went past it (the replay checks a wait when the queue gets to it, not as the value changes).
bool event_wait_met(const EventWait& w) {
    uint32_t v = __atomic_load_n(reinterpret_cast<const uint32_t*>(w.storage), __ATOMIC_ACQUIRE);
    return int32_t(v - w.value) >= 0;
}
// Events the CPU has set (nvnEventSignal): the waits the queues' lines model.
std::mutex g_cpu_events_lock;
std::unordered_set<uint64_t> g_cpu_events;
bool cpu_event(uint64_t storage) {
    std::lock_guard lk(g_cpu_events_lock);
    return g_cpu_events.count(storage) != 0;
}
// The waits of a submit's lists (and the lists they call) for CPU-set events, not preceded in the same list by a
// signal of that event.
void collect_waits(const CmdList& l, std::vector<EventWait>& out, int depth) {
    std::vector<uint64_t> signaled;
    for (const Cmd& c : l.cmds) {
        if (c.op == Cmd::SignalEvent) {
            signaled.push_back(c.a[0]);
        } else if (c.op == Cmd::WaitEvent && c.a[0] && cpu_event(c.a[0]) &&
                   std::find(signaled.begin(), signaled.end(), c.a[0]) == signaled.end()) {
            out.push_back({c.a[0], c.n, uint32_t(c.a[1])});
        } else if (c.op == Cmd::CallList && depth < 4) {
            if (auto called = command_list(c.a[0])) collect_waits(*called, out, depth + 1);
        }
    }
}

struct Pending {
    uint64_t at = 0;                       // its position (all queues)
    std::function<void(uint64_t)> job;     // given the time it became ready (its submit, or what it waited for)
    std::vector<EventWait> waits;          // a submit's event waits
    uint64_t wait_sync = 0, wait_fence = 0;  // nvnQueueWaitSync: until that sync's fence `wait_fence` was handed over
    uint64_t queued_ns = 0, blocked_ns = 0;
};
std::mutex g_pending_lock;        // the lines below
std::recursive_mutex g_flushing;  // one hand-over at a time
std::map<uint64_t, std::deque<Pending>> g_lines;  // per NVNqueue*, in its submission order
uint64_t g_pending_next = 1;                      // the next item's position

// A sync's fence (its position, FenceSync) was handed over to the replay; a fence that never was, or a sync a window
// signals, counts as passed.
bool sync_fence_handed(uint64_t guest, uint64_t fence) {
    auto* s = get<Sync>(guest);
    return !s || s->fence_handed.load() >= fence;
}

bool pending_ready(const Pending& p) {
    if (!std::all_of(p.waits.begin(), p.waits.end(), event_wait_met)) return false;
    return !p.wait_sync || sync_fence_handed(p.wait_sync, p.wait_fence);
}

// The replay of a submit handed over late runs as if it had started when it became ready (or when the one before it
// ended, on that timeline): its counter reports (the game's GPU frame time) leave out the hold.
std::atomic<uint64_t> g_replay_delay{0};
uint64_t g_replay_mapped_end = 0;  // replay thread only
void dispatch_submit(std::vector<std::shared_ptr<CmdList>> lists, std::shared_ptr<const DeferredValues> deferred,
                     uint64_t ready_ns) {
    {
        std::lock_guard lk(g_sub_lock);
        g_sub_called++;
    }
    worker_run([lists = std::move(lists), deferred, ready_ns] {
        const uint64_t start = plat::monotonic_ns();
        const uint64_t from = std::max(ready_ns, g_replay_mapped_end);
        const uint64_t delay = ready_ns && start > from ? start - from : 0;
        g_replay_delay.store(delay, std::memory_order_relaxed);
        gpu_submit(lists, deferred);
        g_replay_mapped_end = plat::monotonic_ns() - delay;
        g_replay_delay.store(0, std::memory_order_relaxed);
        submit_done();
    });
}

// Hands over, across the queues, every item whose line is not waiting, oldest first.
void flush_pending() {
    std::lock_guard<std::recursive_mutex> fl(g_flushing);
    bool retried = false;
    for (;;) {
        Pending item;
        bool blocked = false;
        {
            std::lock_guard lk(g_pending_lock);
            std::deque<Pending>* pick = nullptr;
            std::deque<Pending>* oldest_blocked = nullptr;
            const uint64_t now = plat::monotonic_ns();
            for (auto& [queue, line] : g_lines) {
                if (line.empty()) continue;
                Pending& front = line.front();
                if (pending_ready(front)) {
                    if (!pick || front.at < pick->front().at) pick = &line;
                } else {
                    blocked = true;
                    if (!front.blocked_ns) front.blocked_ns = now;
                    if (now - front.blocked_ns >= 1'000'000'000ull &&
                        (!oldest_blocked || front.at < oldest_blocked->front().at))
                        oldest_blocked = &line;
                }
            }
            if (!pick && oldest_blocked) {
                static int given_up = 0;
                if (++given_up <= 10) {
                    std::string what;
                    for (const EventWait& w : oldest_blocked->front().waits) {
                        char b[96];
                        std::snprintf(b, sizeof b, " event %llx for %u (now %u)", (unsigned long long)w.storage, w.value,
                                      *reinterpret_cast<const uint32_t*>(w.storage));
                        what += b;
                    }
                    for (auto& [queue, line] : g_lines)
                        if (&line == oldest_blocked) {
                            char b[64];
                            std::snprintf(b, sizeof b, " on queue %llx", (unsigned long long)queue);
                            what += b;
                        }
                    for (auto& [queue, line] : g_lines) {
                        char b[64];
                        std::snprintf(b, sizeof b, "; queue %llx: %zu waiting", (unsigned long long)queue, line.size());
                        what += b;
                    }
                    log("nvn: a queue waited 1 s for an event or fence that never came: went on (sync %llx%s)",
                        (unsigned long long)oldest_blocked->front().wait_sync, what.c_str());
                }
                pick = oldest_blocked;
            }
            if (pick) {
                item = std::move(pick->front());
                pick->pop_front();
                if (item.blocked_ns) item.queued_ns = now;
            }
        }
        if (!item.job) {
            // Waiting lines: with a replay thread, what was handed over may not have signaled yet.
            if (!blocked || retried) return;
            worker_finish();
            retried = true;
            continue;
        }
        retried = false;
        item.job(item.queued_ns);
    }
}

// Queues `job` in its queue's line and hands over what can go now. `positioned` is told the item's position before
// anything can hand it over.
void queue_item(uint64_t queue, std::function<void(uint64_t)> job, std::vector<EventWait> waits = {},
                uint64_t wait_sync = 0, uint64_t wait_fence = 0, const std::function<void(uint64_t)>& positioned = {}) {
    {
        std::lock_guard lk(g_pending_lock);
        const uint64_t at = g_pending_next++;
        if (positioned) positioned(at);
        Pending p;
        p.at = at, p.job = std::move(job), p.waits = std::move(waits), p.wait_sync = wait_sync, p.wait_fence = wait_fence;
        p.queued_ns = plat::monotonic_ns();
        g_lines[queue].push_back(std::move(p));
    }
    flush_pending();
}

bool queue_trace() {
    static const uint64_t from = std::getenv("NX_TRACE_QUEUES") ? std::strtoull(std::getenv("NX_TRACE_QUEUES"), nullptr, 10) : 0;
    const bool on = from && g_frame >= from;
    return on;
}
int g_queue_lines = 0;
void QueueInitialize(CPU& c) {  // NVNboolean (NVNqueue*, const NVNqueueBuilder*)
    if (queue_trace()) log("queue: initialize %llx, lr %s", (unsigned long long)c.x[0], describe(c.x[30]).c_str());
    c.x[0] = 1;
}
void QueueFinalize(CPU&) {}
void QueueSubmitCommands(CPU& c) {  // (NVNqueue*, int count, const NVNcommandHandle*)
    plat::perf_hint_thread();  // this thread replays: part of every frame (perf_hint.cpp)
    if (tl::on()) tl::record(tl::kNvn, tl::kSubmitBegin, c.x[1], 0, &c);
    struct TlEnd {
        uint64_t t0 = plat::monotonic_ns();
        ~TlEnd() {
            if (tl::on()) tl::record(tl::kNvn, tl::kSubmitEnd, 0, 0, nullptr);
            hitch::add(hitch::kSubmits, 1), hitch::add(hitch::kSubmitNs, plat::monotonic_ns() - t0);
        }
    } tl_end;
    std::vector<std::shared_ptr<CmdList>> lists;
    static uint64_t submitted = 0, missing = 0;
    for (uint32_t k = 0; k < uint32_t(c.x[1]); k++) {
        submitted++;
        if (auto l = command_list(ld<uint64_t>(c.x[2] + 8 * k)))
            lists.push_back(std::move(l));
        else if (++missing <= 5)
            log("nvn: submitted handle %llx is unknown", (unsigned long long)ld<uint64_t>(c.x[2] + 8 * k));
    }
    static const bool trace_lists = std::getenv("NX_NVN_LISTS") != nullptr;
    if (trace_lists && g_presents > 900 && g_presents < 905)
        for (uint32_t k = 0; k < uint32_t(c.x[1]); k++) {
            auto l = command_list(ld<uint64_t>(c.x[2] + 8 * k));
            log("nvn list: submit %llx handle %llu (%zu cmds)", (unsigned long long)c.x[0],
                (unsigned long long)ld<uint64_t>(c.x[2] + 8 * k), l ? l->cmds.size() : 0);
        }
    if ((submitted & 1023) == 0) log("nvn: %llu handles submitted, %llu unknown", (unsigned long long)submitted, (unsigned long long)missing);
    // NX_DEFERRED_SNAPSHOT=1: read the Deferred arrays here; by default they are read when the submit is replayed.
    static const bool snap = std::getenv("NX_DEFERRED_SNAPSHOT") != nullptr;
    auto deferred = snap ? std::make_shared<const DeferredValues>(deferred_snapshot(lists)) : nullptr;
    std::vector<EventWait> waits;
    if (event_waits_on() || queue_trace())
        for (const auto& l : lists) collect_waits(*l, waits, 0);
    if (queue_trace() && g_queue_lines < 300) {
        std::string ev;  // the events its lists signal and wait for, in order (S/W storage low bits = value)
        std::function<void(const CmdList&, int)> scan = [&](const CmdList& l, int depth) {
            for (const Cmd& cmd : l.cmds) {
                char b[48];
                if (cmd.op == Cmd::SignalEvent || cmd.op == Cmd::WaitEvent) {
                    std::snprintf(b, sizeof b, " %c%llx=%u", cmd.op == Cmd::SignalEvent ? 'S' : 'W',
                                  (unsigned long long)(cmd.a[0] & 0xfffff), uint32_t(cmd.a[1]));
                    ev += b;
                } else if (cmd.op == Cmd::CallList && depth < 4) {
                    if (auto called = command_list(cmd.a[0])) scan(*called, depth + 1);
                }
            }
        };
        for (const auto& l : lists) scan(*l, 0);
        if (!ev.empty() || g_queue_lines < 40)
            g_queue_lines++, log("queue: submit on %llx, %zu lists:%s, lr %s", (unsigned long long)c.x[0], lists.size(),
                                 ev.c_str(), describe(c.x[30]).c_str());
    }
    if (!event_waits_on()) waits.clear();
    if (snap) {  // the Deferred arrays were read now: replayed now too
        flush_pending();
        dispatch_submit(std::move(lists), deferred, 0);
        return;
    }
    queue_item(c.x[0], [lists = std::move(lists)](uint64_t ready_ns) mutable {
        dispatch_submit(std::move(lists), nullptr, ready_ns);
    }, std::move(waits));
}
void QueueFlush(CPU&) {}
void QueueFinish(CPU& c) {
    flush_pending();
    if (tl::on()) tl::record(tl::kNvn, tl::kFinishBegin, 0, 0, &c);
    wait_submits(submits_called(), kWaitFinish);
    worker_finish();
    gpu_wait(gpu_serial());
    if (tl::on()) tl::record(tl::kNvn, tl::kFinishEnd, 0, 0, nullptr);
}
// (NVNqueue*, NVNsync*, NVNsyncCondition, int flags): every condition taken as "all previous work complete".
void QueueFenceSync(CPU& c) {
    if (queue_trace() && g_queue_lines < 300)
        g_queue_lines++, log("queue: fence on %llx sync %llx, lr %s", (unsigned long long)c.x[0], (unsigned long long)c.x[1],
                             describe(c.x[30]).c_str());
    uint64_t guest = c.x[1];  // the fence goes after the work queued so far: its serial is taken on the replay thread
    auto* s = get<Sync>(guest);
    if (!s) return;
    s->signaled = true;
    s->window = s->release = 0;
    s->job = 0;
    // Its position (fence_queued): SyncWait and nvnQueueWaitSync wait until it is handed over, after what its queue had
    // before it.
    auto at = std::make_shared<uint64_t>(0);
    queue_item(c.x[0], [guest, at](uint64_t) {
        auto* s = get<Sync>(guest);
        if (!s) return;
        s->seq = submits_called();
        s->job = worker_run([guest] {
            if (auto* s = get<Sync>(guest)) s->serial = gpu_serial();
        });
        s->fence_handed = std::max(s->fence_handed.load(), *at);
    }, {}, 0, 0, [s, at](uint64_t position) { *at = position, s->fence_queued = position; });
}
void QueueWaitSync(CPU& c) {
    if (queue_trace() && g_queue_lines < 300)
        g_queue_lines++, log("queue: %llx waits for sync %llx, lr %s", (unsigned long long)c.x[0], (unsigned long long)c.x[1],
                             describe(c.x[30]).c_str());
    if (tl::on()) tl::record(tl::kNvn, tl::kQueueWaitSync, c.x[1], 0, &c);
    // NVNboolean (NVNqueue*, const NVNsync*): this queue's later work waits until the sync's fence passed (another
    // queue's); a window's texture-available sync is not waited for (its texture is not reused before the display
    // let it go, display_wait).
    auto* s = get<Sync>(c.x[1]);
    if (event_waits_on() && s && !s->release && s->fence_handed.load() < s->fence_queued.load())
        queue_item(c.x[0], [](uint64_t) {}, {}, c.x[1], s->fence_queued.load());
    c.x[0] = 1;
}

// ---- windows ----------------------------------------------------------------------------------------------------

void WindowInitialize(CPU& c) {  // NVNboolean (NVNwindow*, const NVNwindowBuilder*)
    uint64_t guest = c.x[0], builder = c.x[1];
    auto& w = make<Window>(guest);
    int n = int(driver(c, "nvnWindowBuilderGetNumTextures", {builder}));
    for (int k = 0; k < n; k++) w.textures.push_back(driver(c, "nvnWindowBuilderGetTexture", {builder, uint64_t(k)}));
    w.active = int(driver(c, "nvnWindowBuilderGetNumActiveTextures", {builder}));
    if (w.active <= 0 || w.active > n) w.active = n;
    w.interval = int(driver(c, "nvnWindowBuilderGetPresentInterval", {builder}));
    log("nvn: window %llx: %d textures (%d active), present interval %d", (unsigned long long)guest, n, w.active, w.interval);
    c.x[0] = 1;
}

void WindowFinalize(CPU& c) { destroy(c.x[0]); }

// NVNwindowAcquireTextureResult (NVNwindow*, NVNsync* textureAvailableSync, int* textureIndex)
void WindowAcquireTexture(CPU& c) {
    auto* w = get<Window>(c.x[0]);
    int index = 0;
    if (w && w->active > 0) {
        index = w->next;
        w->next = (w->next + 1) % w->active;
    }
    if (c.x[2]) st<int32_t>(c.x[2], index);
    if (tl::on()) tl::record(tl::kNvn, tl::kAcquire, uint64_t(index), c.x[1], &c);
    signal_sync(c.x[1]);
    if (w && !pace_at_present()) {
        std::lock_guard lk(g_display_lock);
        uint64_t last = size_t(index) < w->last_present.size() ? w->last_present[size_t(index)] : 0;
        if (auto* s = last ? get<Sync>(c.x[1]) : nullptr) s->window = c.x[0], s->release = last + 1;
    }
    c.x[0] = 0;
}

void WindowGetNumTextures(CPU& c) {
    auto* w = get<Window>(c.x[0]);
    c.x[0] = w ? w->textures.size() : 0;
}

void WindowGetNumActiveTextures(CPU& c) {
    auto* w = get<Window>(c.x[0]);
    c.x[0] = w ? uint64_t(w->active) : 0;
}

void WindowSetNumActiveTextures(CPU& c) {
    if (auto* w = get<Window>(c.x[0])) w->active = std::max(1, std::min(int(c.x[1]), int(w->textures.size())));
}

void WindowGetPresentInterval(CPU& c) {
    auto* w = get<Window>(c.x[0]);
    c.x[0] = w ? uint64_t(w->interval) : 1;
}

// Our governor over the game's (NX_GOVERNOR; on by default on Android unless NX_MAX_INTERVAL is set). The game's own
// rule (below) locks 30 fps after 3 frames over 15.83 ms; at 30 fps a phone lowers its clocks, every frame then takes
// longer, and it never leaves 30 (a Galaxy S23 sat at 30 fps in course with frames that run at 60 when allowed).
// This one holds 60 (the game's maximum clamped to 1) while it is kept: it watches the main thread's loop period over
// 120-frame windows, and when two windows in a row had over 20% of their frames past 20 ms (the game would run
// visibly slow; one window is a scene load), it hands the choice back to the game (30 fps at the right speed) for a
// while, then tries 60 again, skipping the first 90 frames of a try (the clocks ramp up from the 30-fps level). The
// wait doubles after each failed try (10 s, 20 s, ... 80 s) and resets once a try holds. Returns 1 (clamp) or 0.
uint32_t governor_max(uint64_t now) {
    static const bool on = [] {
        const char* e = std::getenv("NX_GOVERNOR");
#if defined(__ANDROID__)
        return !(e && e[0] == '0');
#else
        return e && e[0] == '1';
#endif
    }();
    if (!on) return 0;
    static uint64_t last = 0, frames = 0, slow = 0, game_until = 0, warm_until = 0, backoff = 600, n = 0;
    static int bad_windows = 0, held_windows = 0;
    uint64_t period = last ? now - last : 0;
    last = now;
    n++;
    if (n < game_until) return 0;
    if (n == game_until) frames = slow = 0, bad_windows = 0, warm_until = n + 90;
    if (n < warm_until) return 1;
    frames++, slow += period > 20'000'000ull;
    if (frames < 120) return 1;
    bool held = slow * 5 <= frames;
    if (!held || held_windows++ % 30 == 0)
        log("nvn: governor: %llu of %llu frames over 20 ms%s", (unsigned long long)slow, (unsigned long long)frames,
            held ? ": 60 fps holds" : bad_windows ? ": back to the game's choice" : " (once: still 60)");
    frames = slow = 0;
    if (held) {
        bad_windows = 0, backoff = 600;
        return 1;
    }
    held_windows = 0;
    if (++bad_windows < 2) return 1;
    game_until = n + backoff;
    backoff = std::min<uint64_t>(backoff * 2, 4800);
    return 0;
}

// Wonder's sead::GameFramework::procFrame_ override (main+e15ec) picks the interval every frame from the main
// thread's last frame time, fw[0x118] (GetSystemTick ticks from the end of the previous present wait to the end of
// this frame's calc and draw recording): 2 when it reaches 15.83 ms, locked after 3 slow frames, back to 1 after 15
// fast ones (docs/nvn/DEBUG.md #43). From that call site (lr main+e16ac, x19 = the framework): a histogram of that
// time every 600 frames, and NX_MAX_INTERVAL=1 clamps the game's own maximum to 1 (fields 0x24c/0x264/0x268).
void frame_time_probe(CPU& c) {
    const LoadedModule* m = module_at(c.x[30]);
    if (!m || c.x[30] - m->base != 0xe16ac || !c.x[19]) return;
    plat::perf_hint_thread();  // the game's main thread makes the frame too (ADPF, PLAN.md R2)
    plat::frame_thread_tick(1);
    static const char* max_env = std::getenv("NX_MAX_INTERVAL");
    static uint32_t game_max = 0;  // the game's own maximum (fw[0x24c]), restored when the governor lets go
    if (!game_max) game_max = ld<uint32_t>(c.x[19] + 0x24c);
    uint32_t max = max_env && std::atoi(max_env) >= 1 ? uint32_t(std::atoi(max_env)) : governor_max(plat::monotonic_ns());
    if (!max && !max_env && game_max) st<uint32_t>(c.x[19] + 0x24c, game_max);
    if (max >= 1) {
        st<uint32_t>(c.x[19] + 0x24c, max);
        if (ld<uint32_t>(c.x[19] + 0x268) > max) st<uint32_t>(c.x[19] + 0x268, max);
        if (ld<uint32_t>(c.x[19] + 0x264) > max) st<uint32_t>(c.x[19] + 0x264, max);
        if (int(c.x[1]) > int(max)) c.x[1] = max;
    }
    static uint64_t bins[5] = {}, frames = 0;
    double ms = double(ld<uint64_t>(c.x[19] + 0x118)) / 19200.0;
    // A main-thread frame over 33 ms: what it waited on (PHASE3.md I1), its CPU time, faults and switches since the
    // last call, and how much of it a memory-guard read covered (PHASE4.md H0). NX_HITCH_LINES lines at most (300).
    static int hitches = 0;
    static const int max_hitches = std::getenv("NX_HITCH_LINES") ? std::atoi(std::getenv("NX_HITCH_LINES")) : 300;
    static uint64_t cpu_prev = 0;
    static long faults_prev[2] = {}, switches_prev[2] = {};
    uint64_t cpu = 0, now = plat::monotonic_ns();
    long faults[2], switches[2];
    plat::thread_counters(cpu, faults, switches);
    if (ms >= 33 && hitches < max_hitches && cpu_prev) {
        hitches++;
        uint64_t span = uint64_t(ms * 1e6);
        log("hitch: frame %llu, main thread %.1f ms (cpu %.1f), blocked: sync %.1f, finish %.1f, invalidate %.1f, "
            "finalize %.1f ms; faults %ld/%ld, switches %ld/%ld; guard read %.1f ms",
            (unsigned long long)g_frame.load(), ms, double(cpu - cpu_prev) / 1e6,
            double(g_wait_sites[0].frame_ns) / 1e6, double(g_wait_sites[1].frame_ns) / 1e6,
            double(g_wait_sites[2].frame_ns) / 1e6, double(g_wait_sites[3].frame_ns) / 1e6, faults[0] - faults_prev[0],
            faults[1] - faults_prev[1], switches[0] - switches_prev[0], switches[1] - switches_prev[1],
            double(plat::memguard_overlap_ns(now > span ? now - span : 0, now)) / 1e6);
    }
    cpu_prev = cpu;
    for (int k = 0; k < 2; k++) faults_prev[k] = faults[k], switches_prev[k] = switches[k];
    for (WaitSite& w : g_wait_sites) w.frame_ns = 0;
    bins[ms < 12 ? 0 : ms < 15.83 ? 1 : ms < 20 ? 2 : ms < 33 ? 3 : 4]++;
    if (++frames % 600 == 0) {
        log("nvn: main-thread frame time over 600 frames: <12 ms %llu, <15.8 %llu, <20 %llu, <33 %llu, more %llu",
            (unsigned long long)bins[0], (unsigned long long)bins[1], (unsigned long long)bins[2],
            (unsigned long long)bins[3], (unsigned long long)bins[4]);
        for (auto& b : bins) b = 0;
    }
}

// Switch 2 Wonder: the governor (main+1bbf..) picks the interval from the framework's last main-thread frame time,
// fw[0x130] (system ticks), times 60 and rounded up, and calls the setter (main+1bc1d0, which reaches us with
// lr main+1bc1f0 and x20 = fw) only when the interval changes. The framework is kept so present_frame_time() can
// read that time every frame (NX_FRAME_TIME_LOG histograms; the Switch 1 probe above has the other address).
uint64_t g_s2_fw = 0;
void s2_frame_time_note() {
    static const bool on = std::getenv("NX_FRAME_TIME_LOG") != nullptr;
    if (!on || !g_s2_fw) return;
    static uint64_t bins[5] = {}, frames = 0, sum = 0;
    uint64_t ticks = ld<uint64_t>(g_s2_fw + 0x130);
    static int detail = 0;
    if (detail++ < 3) {  // what the frame time is made of (main+909d0): max(fw[0x138], now - fw[0xf8]), now from the
        const LoadedModule* m = module_at(g_s2_fw ? 0x4000000000ull : 0);  // proc at main+3b71c20(window)
        uint64_t proc = m ? ld<uint64_t>(m->base + 0x3b71c20) : 0;
        log("nvn: S2 frame time parts: fw[0xf8] %llx fw[0x138] %llx fw[0x140] %llx fw[0x130] %llx; now proc %s",
            (unsigned long long)ld<uint64_t>(g_s2_fw + 0xf8), (unsigned long long)ld<uint64_t>(g_s2_fw + 0x138),
            (unsigned long long)ld<uint64_t>(g_s2_fw + 0x140), (unsigned long long)ticks, describe(proc).c_str());
        if (m) {  // the timestamp reports it reads (main+90a30/909a0) and the frequency the governor divides by
            uint64_t obj = ld<uint64_t>(ld<uint64_t>(m->base + 0x3a1e150)) + 0x30;
            uint32_t idx = ld<uint32_t>(obj + 0x138);
            uint64_t rep = ld<uint64_t>(obj + 0x68 + (idx < 2 ? idx * 8 : 0));
            log("nvn: S2 timing: index %u, reports at %llx: %llx %llx | %llx %llx; governor freq %lld; host ns %llu, "
                "system tick %llu", idx, (unsigned long long)rep, (unsigned long long)ld<uint64_t>(rep),
                (unsigned long long)ld<uint64_t>(rep + 8), (unsigned long long)ld<uint64_t>(rep + 16),
                (unsigned long long)ld<uint64_t>(rep + 24), (long long)ld<int64_t>(m->base + 0x3b73b80),
                (unsigned long long)plat::monotonic_ns(), (unsigned long long)system_tick());
            // The governor's controller (fw+0x258, main+1bbee0): interval min/max, the bias added to frames*60, the
            // frames before going up, the current interval and simulation Hz (60 / interval, -> main+3aac460 = 60/Hz,
            // the game's time step per frame).
            uint64_t g = g_s2_fw + 0x258;
            log("nvn: S2 governor: min %d max %d bias %.3f up-after %d down-reset %d; interval %d, Hz %d, step %.3f",
                ld<int32_t>(g), ld<int32_t>(g + 4), double(ld<float>(g + 8)), ld<int32_t>(g + 0xc),
                ld<int32_t>(g + 0x10), ld<int32_t>(g + 0x40), ld<int32_t>(g + 0x50),
                double(ld<float>(m->base + 0x3aac460)));
        }
    }
    double ms = double(ticks) * 1000.0 / double(tick_hz());
    bins[ms < 12 ? 0 : ms < 16.67 ? 1 : ms < 20 ? 2 : ms < 33.3 ? 3 : 4]++;
    sum += ticks;
    // Game speed: the time step the game applies per frame (main+3aac460 = 60 / simulation Hz, in 1/60 s units; 2.0
    // at 30 Hz) summed over the frames, against the host time between them. 100% is real time; a game that misses its
    // frame rate runs slower, as on a console that drops frames.
    static double game_s = 0, wall_s = 0;
    static uint64_t last_ns = 0;
    uint64_t now_ns = plat::monotonic_ns();
    if (const LoadedModule* mm = module_at(0x4000000000ull); mm && last_ns && now_ns - last_ns < 250'000'000ull) {
        float step = ld<float>(mm->base + 0x3aac460);
        if (step > 0 && step <= 60) game_s += double(step) / 60.0, wall_s += double(now_ns - last_ns) / 1e9;
    }
    last_ns = now_ns;
    if (frames % 300 == 299 && wall_s > 0) {
        log("nvn: S2 game speed over the last 300 frames: %.1f%% (game %.2f s in %.2f s)", 100.0 * game_s / wall_s,
            game_s, wall_s);
        game_s = wall_s = 0;
    }
    if (++frames % 300 == 0) {
        log("nvn: S2 main-thread frame time (fw[0x130]) over 300 frames: mean %.1f ms; <12 %llu, <16.7 %llu, <20 %llu, "
            "<33.3 %llu, more %llu", double(sum) * 1000.0 / double(tick_hz()) / 300.0, (unsigned long long)bins[0],
            (unsigned long long)bins[1], (unsigned long long)bins[2], (unsigned long long)bins[3],
            (unsigned long long)bins[4]);
        for (auto& b : bins) b = 0;
        sum = 0;
    }
}

void WindowSetPresentInterval(CPU& c) {
    frame_time_probe(c);
    if (const LoadedModule* m = module_at(c.x[30]); m && c.x[30] - m->base == 0x1bc1f0) g_s2_fw = c.x[20];
    if (auto* w = get<Window>(c.x[0])) {
        static int logged = 0;
        if (w->interval != int(c.x[1]) && logged++ < 50)
            log("nvn: window present interval %d -> %d (frame %llu, from %s)", w->interval, int(c.x[1]),
                (unsigned long long)g_frame.load(), describe(c.x[30]).c_str());
        static const bool trace = std::getenv("NX_TRACE_INTERVAL") != nullptr;  // every call's site and x19 fields
        static int traced = 0;
        if (trace && traced++ < 40)
            log("interval: %d at frame %llu from %s, x19 %llx", int(c.x[1]), (unsigned long long)g_frame.load(),
                describe(c.x[30]).c_str(), (unsigned long long)c.x[19]);
        w->interval = int(c.x[1]);
    }
}

// (NVNqueue*, NVNwindow*, int textureIndex)
// Frame pacing: a present returns no earlier than the window's present interval (in 60 Hz vsyncs) after the previous
// one. On the Switch the texture-available sync of the next acquire does this; ours is signaled at once, so without
// it the game runs as fast as it can once it is faster than 60 fps. A late frame is not snapped to the next vsync
// (an 18 ms frame stays 18 ms, not 33). NX_NO_FRAME_LIMIT=1: unpaced.
void present_stats(int interval) {
    s2_frame_time_note();
    static uint64_t presents = 0, at2 = 0;  // how often the game asks for 30 fps (interval 2), per 600 presents
    at2 += interval >= 2;
    if (++presents % 600 == 0)
        log("nvn: %llu of the last 600 presents at interval >= 2", (unsigned long long)at2), at2 = 0, log_wait_sites();
}

void pace_present(int interval) {
    static uint64_t next = 0;
    if (frame_limit_off() || interval <= 0) return;
    uint64_t now = plat::monotonic_ns();
    if (tl::on()) tl::record(tl::kNvn, tl::kPaceBegin, next > now ? next - now : 0, uint64_t(interval), nullptr);
    if (next > now) plat::sleep_ns(next - now), now = plat::monotonic_ns();
    if (tl::on()) tl::record(tl::kNvn, tl::kPaceEnd, 0, 0, nullptr);
    uint64_t period = uint64_t(interval) * 16'666'667ull;
    next = (next > now || now - next < period ? next : now) + period;
}

void QueuePresentTexture(CPU& c) {
    // The frame's work, from the end of the last present's pacing to now (perf_hint.cpp: ADPF on Android).
    static uint64_t work_from = 0;
    uint64_t t_in = plat::monotonic_ns();
    if (tl::on()) tl::record(tl::kNvn, tl::kPresentBegin, c.x[2], 0, &c);
    plat::perf_hint_thread();
    if (work_from) plat::perf_hint_frame(int64_t(t_in - work_from));
    if (auto* w = get<Window>(c.x[1])) {
        // At most two presents queued: the game runs at most two frames ahead of the replay.
        static uint64_t queued[2] = {};
        static int next_slot = 0;
        worker_wait(queued[next_slot]);
        // When the display shows it (the display model; 0 on the old path, which paces below instead).
        const uint64_t show_at = pace_at_present() ? 0 : display_schedule(*w, int(c.x[2]), w->interval);
        if (int(c.x[2]) >= 0 && size_t(c.x[2]) < w->textures.size()) {
            uint64_t texture = w->textures[size_t(c.x[2])];
            std::array<int32_t, 4> crop{w->crop[0], w->crop[1], w->crop[2], w->crop[3]};
            if (present_log_on()) {  // before the host present below, which fills the row's host side
                const LoadedModule* m = g_s2_fw ? module_at(0x4000000000ull) : nullptr;
                present_log_game(g_presents + 1, c.x[2], uint64_t(w->interval), t_in, show_at,
                                 g_s2_fw ? ld<uint64_t>(g_s2_fw + 0x130) : 0, m ? ld<float>(m->base + 0x3aac460) : 0.0f);
            }
            queued[next_slot] = worker_run([texture, crop, show_at] { gpu_present(texture, crop.data(), show_at); });
            next_slot ^= 1;
        }
        if (tl::on() && g_s2_fw)
            tl::record(tl::kNvn, tl::kFrameTime, ld<uint64_t>(g_s2_fw + 0x130), uint64_t(w->interval), nullptr);
        present_stats(w->interval);
        if (pace_at_present()) pace_present(w->interval);
    }
    work_from = plat::monotonic_ns();
    if (tl::on()) tl::record(tl::kNvn, tl::kPresentEnd, 0, 0, nullptr);
    uint64_t n = ++g_presents;
    if (auto* w = get<Window>(c.x[1])) hitch::present(n, g_s2_fw ? ld<uint64_t>(g_s2_fw + 0x130) : 0, w->interval);
    if (n == 1 || (n & (n - 1)) == 0) log("nvn: present #%llu (texture %d)", (unsigned long long)n, int(c.x[2]));
    g_frame++;
}

// (NVNwindow*, int x, int y, int width, int height): the part of the window textures shown. In handheld mode
// (NX_HANDHELD) the game renders 1280x720 into its 1920x1080 window textures and crops to it.
void WindowSetCrop(CPU& c) {
    if (auto* w = get<Window>(c.x[0])) {
        int32_t v[4] = {int32_t(c.x[1]), int32_t(c.x[2]), int32_t(c.x[3]), int32_t(c.x[4])};
        if (std::memcmp(v, w->crop, sizeof v) != 0) {
            static int logged = 0;
            if (logged++ < 20) log("nvn: window crop %d,%d %dx%d", v[0], v[1], v[2], v[3]);
            std::memcpy(w->crop, v, sizeof v);
        }
    }
}

}  // namespace

NX_HLE("nvnWindowSetCrop", WindowSetCrop);
NX_HLE("nvnSyncInitialize", SyncInitialize);
NX_HLE("nvnSyncFinalize", SyncFinalize);
NX_HLE("nvnSyncWait", SyncWait);
NX_HLE("nvnQueueInitialize", QueueInitialize);
NX_HLE("nvnQueueFinalize", QueueFinalize);
NX_HLE("nvnQueueSubmitCommands", QueueSubmitCommands);
NX_HLE("nvnQueueFlush", QueueFlush);
NX_HLE("nvnQueueFinish", QueueFinish);
NX_HLE("nvnQueueFenceSync", QueueFenceSync);
NX_HLE("nvnQueueWaitSync", QueueWaitSync);
NX_HLE("nvnQueuePresentTexture", QueuePresentTexture);
NX_HLE("nvnWindowInitialize", WindowInitialize);
NX_HLE("nvnWindowFinalize", WindowFinalize);
NX_HLE("nvnWindowAcquireTexture", WindowAcquireTexture);
NX_HLE("nvnWindowGetNumTextures", WindowGetNumTextures);
NX_HLE("nvnWindowGetNumActiveTextures", WindowGetNumActiveTextures);
NX_HLE("nvnWindowSetNumActiveTextures", WindowSetNumActiveTextures);
NX_HLE("nvnWindowGetPresentInterval", WindowGetPresentInterval);
NX_HLE("nvnWindowSetPresentInterval", WindowSetPresentInterval);

void queue_event_signaled(uint64_t storage) {
    if (!event_waits_on()) return;
    {
        std::lock_guard lk(g_cpu_events_lock);
        g_cpu_events.insert(storage);
    }
    flush_pending();
}

uint64_t replay_delay_ns() { return g_replay_delay.load(std::memory_order_relaxed); }

}  // namespace nx::nvn
