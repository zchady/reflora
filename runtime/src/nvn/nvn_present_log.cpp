// Present log (NX_PRESENT_LOG=<first present>:<count>): one row per present, from the game's nvnQueuePresentTexture
// to the host's vkQueuePresentKHR, with the display's vblank grid, written to present_log.csv in the data folder once
// <count> presents are in. tools/present_log.py reads it: present cadence, percentiles, and which refresh each frame
// reached the screen at (frames shown for 2/3/4 refreshes, frames replaced before any refresh showed them).
//
// Columns (host ns, monotonic_ns' clock; 0 when that stage did not happen, e.g. headless):
//   n            the game's present number
//   index        the window texture presented
//   interval     the present interval the game asked for (vsyncs)
//   game_ns      nvnQueuePresentTexture entered
//   shown_ns     when the runtime's display model shows it (D(p), nvn_queue.cpp)
//   gpu_ns       gpu_present entered (the host present of that frame)
//   ready_ns     its blit / present pass done on the GPU (fence waited), vkQueuePresentKHR about to be called
//   queued_ns    vkQueuePresentKHR returned
//   vblank_ns    the display's last vblank when queued (DWM), period_ns its refresh period (0: unknown)
//   frame_ticks  the game's measured frame time fw[0x130] (Switch 2 ticks), step_x1000 its time step (60 / Hz) * 1000
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <vector>

#include "nvn.h"
#include "platform.h"

namespace nx::nvn {

namespace {

struct Row {
    uint64_t n = 0, index = 0, interval = 0, game_ns = 0, shown_ns = 0, gpu_ns = 0, ready_ns = 0, queued_ns = 0,
             vblank_ns = 0, period_ns = 0, frame_ticks = 0, step_x1000 = 0;
};

std::mutex g_lock;
std::vector<Row> g_rows;
uint64_t g_first = 0, g_count = 0;
// The next rows the host fills: entered/copied (stages 0, 1) and presented (2, 3). A present held until its display
// time (nvn_vk.cpp) is presented after the next frame entered, so the two advance apart; both go in order.
size_t g_enter = 0, g_out = 0;
uint64_t g_total_in = 0, g_total_out = 0, g_base = 0;  // every host present, logged or not; g_base: the first logged one's
bool g_written = false;

bool parse() {
    static const bool on = [] {
        const char* e = std::getenv("NX_PRESENT_LOG");
        if (!e) return false;
        unsigned long long a = 0, b = 0;
        if (std::sscanf(e, "%llu:%llu", &a, &b) != 2 || !b) return false;
        g_first = a, g_count = b;
        g_rows.reserve(size_t(b));
        log("nvn: present log: presents %llu to %llu -> present_log.csv", a, a + b - 1);
        return true;
    }();
    return on;
}

void write() {
    std::string path = plat::data_root() + "/present_log.csv";
    FILE* f = plat::fopen_utf8(path, "wb");
    if (!f) return;
    std::fprintf(f, "n,index,interval,game_ns,shown_ns,gpu_ns,ready_ns,queued_ns,vblank_ns,period_ns,frame_ticks,step_x1000\n");
    for (const Row& r : g_rows)
        std::fprintf(f, "%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu\n", (unsigned long long)r.n,
                     (unsigned long long)r.index, (unsigned long long)r.interval, (unsigned long long)r.game_ns,
                     (unsigned long long)r.shown_ns, (unsigned long long)r.gpu_ns, (unsigned long long)r.ready_ns,
                     (unsigned long long)r.queued_ns, (unsigned long long)r.vblank_ns, (unsigned long long)r.period_ns,
                     (unsigned long long)r.frame_ticks, (unsigned long long)r.step_x1000);
    std::fclose(f);
    log("nvn: present log: %zu presents written to present_log.csv", g_rows.size());
}

void maybe_write() {
    if (!g_written && g_rows.size() >= g_count && g_out >= g_rows.size()) g_written = true, write();
}

}  // namespace

bool present_log_on() { return parse(); }

void present_log_game(uint64_t n, uint64_t index, uint64_t interval, uint64_t game_ns, uint64_t shown_ns,
                      uint64_t frame_ticks, float step) {
    if (!parse()) return;
    std::lock_guard lk(g_lock);
    if (n < g_first || g_rows.size() >= g_count) return;
    Row r;
    r.n = n, r.index = index, r.interval = interval, r.game_ns = game_ns, r.shown_ns = shown_ns;
    r.frame_ticks = frame_ticks, r.step_x1000 = uint64_t(step * 1000.0f + 0.5f);
    g_rows.push_back(r);
}

void present_log_shown(uint64_t n, uint64_t shown_ns) {
    if (!parse()) return;
    std::lock_guard lk(g_lock);
    for (size_t k = g_rows.size(); k-- > 0 && g_rows[k].n >= n;)
        if (g_rows[k].n == n) g_rows[k].shown_ns = shown_ns;
}

// The host side of the logged frames, in order: stage 0 gpu_present entered, 1 its copy done, 2 vkQueuePresentKHR
// returned, 3 not presented to a display.
void present_log_host(int stage) {
    if (!parse()) return;
    std::lock_guard lk(g_lock);
    uint64_t now = plat::monotonic_ns();
    if (stage == 0) {
        if (g_enter < g_rows.size()) {
            if (g_enter == 0) g_base = g_total_in;
            g_rows[g_enter++].gpu_ns = now;
        }
        g_total_in++;
    }
    if (stage == 1 && g_enter && g_enter <= g_rows.size() && g_total_in - g_base == g_enter) g_rows[g_enter - 1].ready_ns = now;
    if (stage == 2 || stage == 3) {
        uint64_t k = g_total_out++;  // presents leave in the order they entered
        if (g_enter && k >= g_base && k - g_base < g_enter) {
            Row& r = g_rows[size_t(k - g_base)];
            if (stage == 2) r.queued_ns = now, plat::display_vblank(r.vblank_ns, r.period_ns);
            g_out = size_t(k - g_base) + 1;
        }
    }
    maybe_write();
}

}  // namespace nx::nvn
