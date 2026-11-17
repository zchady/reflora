// Hitch attribution (hitch.h).
#include "hitch.h"

#include <cstdlib>

#include <nx/game.h>
#include <nx/runtime.h>

#include "collect.h"
#include "platform.h"

namespace nx::hitch {

std::atomic<uint64_t> g_counters[kCount];

namespace {

// A collection session (collect.h): every frame of 50 ms or more with where its time went (no cap, unlike the log),
// and a summary each minute of play: frames, slow frames, the worst, and what was compiled, decoded and uploaded.
void collect_frame(uint64_t gap, const uint64_t* v, uint64_t frame_ticks) {
    static uint64_t frames = 0, slow33 = 0, slow50 = 0, worst = 0, minute_start = 0, total = 0;
    static uint64_t sum[kCount] = {};
    const uint64_t now = plat::monotonic_ns();
    if (!minute_start) minute_start = now;
    frames++, total += gap;
    slow33 += gap >= 33'400'000, slow50 += gap >= 50'000'000;
    if (gap > worst) worst = gap;
    for (int k = 0; k < kCount; k++) sum[k] += v[k];
    if (gap >= 50'000'000) {
        const uint32_t hz = game::kInfo.tick_hz ? game::kInfo.tick_hz : 19200000u;
        collect::event("hitch", "%.1f ms\tpipelines %llu (%.1f ms)\tASTC %llu (%.1f ms)\timages %llu (%.1f ms)\tupload %.1f MB\t"
                       "package reads %llu (%.1f MB, %.1f ms)\tsubmits %.1f ms\tsync waits %.1f ms\tgame frame %.1f ms\t"
                       "pipeline-objects lock waits %.1f ms",
                       double(gap) / 1e6, (unsigned long long)v[kPipelines], double(v[kPipelineNs]) / 1e6,
                       (unsigned long long)v[kAstc], double(v[kAstcNs]) / 1e6, (unsigned long long)v[kImages],
                       double(v[kImageNs]) / 1e6, double(v[kUploadBytes]) / 1e6, (unsigned long long)v[kFsReads],
                       double(v[kFsReadBytes]) / 1e6, double(v[kFsReadNs]) / 1e6, double(v[kSubmitNs]) / 1e6,
                       double(v[kSyncWaitNs]) / 1e6, double(frame_ticks) * 1000.0 / double(hz),
                       double(v[kObjectsWaitNs]) / 1e6);
    }
    if (now - minute_start >= 60'000'000'000ull) {
        collect::event("minute", "%llu frames (%.1f fps)\t>= 33 ms %llu\t>= 50 ms %llu\tworst %.1f ms\t"
                       "pipelines %llu (%.1f ms)\tASTC %llu (%.1f ms)\timages %llu\tupload %.1f MB",
                       (unsigned long long)frames, frames * 1e9 / double(total ? total : 1),
                       (unsigned long long)slow33, (unsigned long long)slow50, double(worst) / 1e6,
                       (unsigned long long)sum[kPipelines], double(sum[kPipelineNs]) / 1e6,
                       (unsigned long long)sum[kAstc], double(sum[kAstcNs]) / 1e6, (unsigned long long)sum[kImages],
                       double(sum[kUploadBytes]) / 1e6);
        frames = slow33 = slow50 = worst = total = 0;
        for (auto& x : sum) x = 0;
        minute_start = now;
    }
}

}  // namespace

void present(uint64_t n, uint64_t frame_ticks, int interval) {
    static const uint64_t threshold = [] {
        const char* e = std::getenv("NX_HITCH_MS");
        return uint64_t(e ? std::atoi(e) : 50) * 1'000'000ull;
    }();
    static const uint64_t start = plat::monotonic_ns();
    static uint64_t last = 0;
    static int logged = 0;
    uint64_t now = plat::monotonic_ns();
    uint64_t v[kCount];
    for (int k = 0; k < kCount; k++) v[k] = g_counters[k].exchange(0, std::memory_order_relaxed);
    uint64_t gap = last ? now - last : 0;
    last = now;
    if (collect::on()) collect_frame(gap, v, frame_ticks);
    if (!threshold || gap < threshold || logged >= 2000) return;
    logged++;
    uint32_t hz = game::kInfo.tick_hz ? game::kInfo.tick_hz : 19200000u;
    log("hitch: present %llu at %.3f s: %.1f ms since the last; package reads %llu (%.1f MB, %.1f ms), save writes "
        "%llu (%llu B), pipelines %llu (%.1f ms), ASTC %llu (%.1f ms), images %llu (%.1f ms), submits %llu (%.1f ms), "
        "upload %.1f MB, sync waits %.1f ms, pipeline-objects lock waits %.1f ms; game frame time %.1f ms, interval %d",
        (unsigned long long)n, double(now - start) / 1e9, double(gap) / 1e6, (unsigned long long)v[kFsReads],
        double(v[kFsReadBytes]) / 1e6, double(v[kFsReadNs]) / 1e6, (unsigned long long)v[kSaveWrites],
        (unsigned long long)v[kSaveWriteBytes], (unsigned long long)v[kPipelines], double(v[kPipelineNs]) / 1e6,
        (unsigned long long)v[kAstc], double(v[kAstcNs]) / 1e6, (unsigned long long)v[kImages],
        double(v[kImageNs]) / 1e6, (unsigned long long)v[kSubmits],
        double(v[kSubmitNs]) / 1e6, double(v[kUploadBytes]) / 1e6, double(v[kSyncWaitNs]) / 1e6,
        double(v[kObjectsWaitNs]) / 1e6,
        double(frame_ticks) * 1000.0 / double(hz), interval);
}

}  // namespace nx::hitch
