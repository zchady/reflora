// Hitch attribution (hitch.cpp): what the runtime did between two presents, from every thread, so that a long gap
// (NX_HITCH_MS, 50 ms by default) is logged with where the time went: package file reads (RomFS, with their
// decompression), save writes, Vulkan pipelines created, ASTC textures decoded, submits (replay + GPU fence), upload
// bytes, the game's sync waits and the
// replay's waits for the pipeline-objects lock that the catalog's workers share. What these do not cover was the game's own work (its threads, its decoding).
#pragma once

#include <atomic>
#include <cstdint>

namespace nx::hitch {

enum Counter : int {
    kFsReads, kFsReadBytes, kFsReadNs, kSaveWrites, kSaveWriteBytes, kPipelines, kPipelineNs, kAstc, kAstcNs,
    kSubmits, kSubmitNs, kUploadBytes, kSyncWaitNs, kImages, kImageNs, kObjectsWaitNs, kCount
};

extern std::atomic<uint64_t> g_counters[kCount];
inline void add(Counter c, uint64_t v) { g_counters[c].fetch_add(v, std::memory_order_relaxed); }

// At each present (nvn_queue.cpp): logs the interval when it was long, then starts the next one.
void present(uint64_t present_number, uint64_t game_frame_ticks, int interval);

}  // namespace nx::hitch
