// Frame timeline (timeline.h). Events go to one preallocated array (an atomic index, no lock): a full array ends the
// recording early. The file: "T <guest thread id> <host tid> <entry>" per thread, "E <ns> <thread> <kind> <code>
// <a> <b> <lr> <caller> <caller> <caller>" per event (hex; thread 0 is a host thread with no guest context, e.g. the
// vsync), and "S <address> <description>" for every guest address that appears. tools/timeline.py reads it.
#include "timeline.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "kernel_state.h"
#include "platform.h"

namespace nx::tl {

std::atomic<bool> g_on{false};

namespace {

struct Event {
    uint64_t ns;
    uint64_t thread;  // guest thread id (0: not a guest thread)
    uint8_t kind;
    uint16_t code;
    uint64_t a, b;
    uint64_t lr[4];
};

constexpr size_t kCapacity = 1u << 20;  // 1 M events (~80 MB), several seconds of a course
std::unique_ptr<Event[]> g_events;
std::atomic<size_t> g_count{0};
uint64_t g_until = 0, g_started = 0;

void write_file() {
    size_t n = std::min(g_count.load(), kCapacity);
    std::string path = plat::data_root() + "/timeline.txt";
    FILE* f = plat::fopen_utf8(path, "wb");
    if (!f) {
        log("timeline: cannot write %s", path.c_str());
        return;
    }
    {
        using namespace kstate;
        std::lock_guard al(g_all_lock);
        for (auto& w : g_all)
            if (auto t = w.lock())
                std::fprintf(f, "T %llx %lld %s\n", (unsigned long long)t->id, (long long)t->host_tid,
                             describe(t->entry).c_str());
    }
    std::set<uint64_t> addrs;
    for (size_t k = 0; k < n; k++) {
        const Event& e = g_events[k];
        std::fprintf(f, "E %llu %llx %u %x %llx %llx %llx %llx %llx %llx\n", (unsigned long long)(e.ns - g_started),
                     (unsigned long long)e.thread, e.kind, e.code, (unsigned long long)e.a, (unsigned long long)e.b,
                     (unsigned long long)e.lr[0], (unsigned long long)e.lr[1], (unsigned long long)e.lr[2],
                     (unsigned long long)e.lr[3]);
        for (uint64_t lr : e.lr)
            if (lr) addrs.insert(lr);
    }
    for (uint64_t a : addrs) std::fprintf(f, "S %llx %s\n", (unsigned long long)a, describe(a).c_str());
    std::fclose(f);
    log("timeline: %zu events%s written to timeline.txt", n, g_count.load() > kCapacity ? " (array full: cut short)" : "");
}

}  // namespace

void record(uint8_t kind, uint16_t code, uint64_t a, uint64_t b, const CPU* c) {
    size_t i = g_count.fetch_add(1, std::memory_order_relaxed);
    if (i >= kCapacity) return;
    Event& e = g_events[i];
    e.ns = plat::monotonic_ns();
    KThread* t = kstate::t_self;
    e.thread = t ? t->id : 0;
    e.kind = kind, e.code = code, e.a = a, e.b = b;
    e.lr[0] = e.lr[1] = e.lr[2] = e.lr[3] = 0;
    if (!c) return;
    e.lr[0] = c->x[30];
    // The guest's frame records (x29 -> [previous x29, lr]) while they stay on this thread's stack above sp.
    uint64_t sp = c->x[31], fp = c->x[29];
    for (int k = 1; k < 4 && fp >= sp && fp - sp < (8ull << 20) && (fp & 7) == 0; k++) {
        const uint64_t* rec = reinterpret_cast<const uint64_t*>(fp);
        e.lr[k] = rec[1];
        if (rec[0] <= fp) break;
        fp = rec[0];
    }
}

void poll() {
    if (g_on) {
        if (plat::monotonic_ns() < g_until && g_count.load() < kCapacity) return;
        g_on = false;
        plat::sleep_ns(50'000'000);  // calls that read g_on just before finish writing their event
        write_file();
        return;
    }
    std::string req = plat::data_root() + "/timeline";
    FILE* f = plat::fopen_utf8(req, "rb");
    if (!f) return;
    int seconds = 3;
    if (std::fscanf(f, "%d", &seconds) != 1 || seconds <= 0 || seconds > 30) seconds = 3;
    std::fclose(f);
    plat::remove_utf8(req);
    if (!g_events) g_events.reset(new Event[kCapacity]);
    g_count = 0;
    g_started = plat::monotonic_ns();
    g_until = g_started + uint64_t(seconds) * 1'000'000'000ull;
    log("timeline: recording %d s", seconds);
    g_on = true;
}

}  // namespace nx::tl
