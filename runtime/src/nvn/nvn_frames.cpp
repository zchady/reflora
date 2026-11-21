// Submits that are not waited for (the design, and what stays synchronous and why:
// private/wonder/patches/async_submit.md). The synchronous path waits for every submit's fence before returning to
// the game, so the submitting thread stalls for the whole GPU time of each frame. Here a submit or a present takes one
// of a few slots (command buffers, fences, descriptor pool, the stretch of the upload ring its replay filled, the
// compute results to copy back) and returns once queued. A slot is retired (its fences waited, 2 s at most, as the
// synchronous path) when it is reused, when the ring needs its memory back, or when something waits for it.
//
// Replays copy everything the GPU reads from guest memory into the ring at submit time, so the game may change its
// memory while a submit runs. Only compute results flow back (write-backs). Two modes use the slots:
//
// NX_PIPELINED (the default since DEBUG.md #61; docs/nvn/PERF-PLAN.md step 3): "pipelined fences". The
// replay and vkQueueSubmit stay on the submitting thread; the fence of the frame's work is not waited for. A submit
// with dispatches is split after its last DispatchCompute into two command buffers: the first part is queued at the
// split, while the rest is still being replayed, and only its fence is waited for, after the rest is queued. Its
// write-backs are copied before gpu_submit returns, so guest memory holds the compute results exactly when it does
// synchronously, and nothing the game waits on (nvnSyncWait, nvnQueueFinish, pool invalidation and finalization) has
// anything left to wait for: gpu_serial() is 0, as synchronously, and those calls never take the GPU lock. At most
// g_max_replays submits are in flight (the oldest is waited for before another is queued).
// NX_PIPELINED_NO_SPLIT=1: no split (a submit with write-backs is waited for whole).
//
// Chunks (docs/nvn/android-perf/PLAN.md R1): a submit's replay is also queued in parts while it is recorded, at the
// first point with no render pass open after NX_SUBMIT_CHUNK_DRAWS draws (at twice that, a pass is ended for it), so
// the GPU starts on a frame a few milliseconds into its replay instead of at its end (or at the split). At most
// kMaxParts command buffers per submit; each begins with the barrier that orders it after everything queued before.
// Default 150 on Android, 0 (off) elsewhere: MoltenVK encodes Metal inside each vkQueueSubmit.
//
// NX_ASYNC_GPU=<frames in flight> (the older experiment): a submit that has write-backs is retired before the next
// replay begins (it may read them) and before a wait on it returns; the game's waits retire slots under the GPU lock.
//
// Also the images of destroyed textures (freed once the submits that could use them are retired) are handled here, in
// every mode.
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <tuple>
#include <vector>

#include "nvn_astc_cache.h"  // texture_hash (NX_VRAM_EVICT)
#include "nvn_vk.h"
#include "platform.h"

namespace nx::nvn {

using namespace nx::vk;

VkQueue vk_queue();                // nvn_vk.cpp
std::recursive_mutex& gpu_lock();  // nvn_vk.cpp

namespace {

constexpr uint32_t kMaxSlots = 8;
constexpr uint32_t kMaxParts = 8;  // command buffers per submit: chunks, the split, the last part
constexpr uint64_t kFenceTimeout = 2'000'000'000ull;  // a frame's work takes milliseconds; longer is a hung shader

struct Slot {
    VkCommandPool pool = VK_NULL_HANDLE;  // its own (the parts reset together), unless NX_CB_RESET=1
    bool pool_used = false;               // parts were begun since the pool's last reset
    VkCommandBuffer cbs[kMaxParts] = {};  // the parts, queued in order
    VkFence fences[kMaxParts] = {};
    uint32_t part = 0;                    // the part being recorded
    uint32_t queued = 0;                  // bit k: part k was queued (its fence will signal)
    uint32_t waited = 0;                  // parts below this were waited for (the ring may reuse their data)
    int split_part = -1;                  // pipelined: the part that ends with the last dispatch
    bool ok = true;                       // the current part is recording (a failed begin leaves nothing to queue)
    VkSemaphore acquired = VK_NULL_HANDLE;          // a present's swapchain image
    VkDescriptorPool descriptors = VK_NULL_HANDLE;  // made on first use (presents need none)
    bool descriptors_used = false;
    std::atomic<bool> reset_done{true};  // the janitor reset its pools (nvn_janitor.cpp); frame_begin waits for it
    uint64_t serial = 0;    // in flight until retired; 0: free
    uint64_t ring_end = 0;  // the ring position after this submit's data
    std::vector<WriteBack> write_backs;
    bool replay = false;  // a submit's replay (not a present)
    uint64_t binds_at_split = 0;  // replay::written_bind_count() at the split
    uint64_t begin_ns = 0;
};

Slot g_slots[kMaxSlots];
uint32_t g_slot_count = 0, g_next = 0;
Slot* g_current = nullptr;               // the slot being recorded
VkSemaphore g_rendered[kMaxSlots] = {};  // per swapchain image: signaled for its present
std::atomic<uint64_t> g_submitted{0}, g_completed{0};
std::atomic<uint64_t> g_barrier{0};  // NX_ASYNC_GPU: the last submit with write-backs, retired before the next replay
FrameTimes g_times;
uint32_t g_max_replays = 2;                       // pipelined: submits in flight
uint32_t g_plan_dispatches = 0, g_seen_dispatches = 0;  // pipelined: the submit's DispatchCompute commands, replayed
// Pipelined, per log period: submits, split ones, write-backs after the first part's fence / after the whole submit,
// waits for the oldest submit (two in flight), and how far into the replay the split came.
struct Stats {
    uint64_t submits = 0, splits = 0, waited_first = 0, waited_whole = 0, throttled = 0, first_ns = 0, replay_ns = 0;
    uint64_t chunks = 0;
} g_stats;
// A command pool per slot, reset whole when the slot is taken again (PHASE3.md C2): resetting each part's command
// buffer was 6-9% of the replay thread on Adreno (it frees the driver's per-pass objects). NX_CB_RESET=1: as before.
bool g_slot_pools = false;
uint64_t g_chunk_draws = 0;           // NX_SUBMIT_CHUNK_DRAWS (frame_init); 0: no chunks
bool g_chunking = false;              // this submit is chunked (frame_plan_split)
uint64_t g_chunk_from = UINT64_MAX;   // the draw count at the last part's start (UINT64_MAX: the next call's)
std::atomic<uint64_t> g_game_waits{0}, g_game_wait_ns{0};
uint64_t g_staging_count = 0, g_staging_bytes = 0;  // staging_alloc's buffers, per log period (PHASE4.md U1)  // the game's waits that blocked (NX_ASYNC_GPU only)

// The upload ring (the synchronous path's upload buffer). Positions only grow; position p is byte p % size. What
// lies before g_tail belongs to retired submits; g_begin is where the submit being recorded started.
VkBuffer g_ring = VK_NULL_HANDLE;
uint8_t* g_ring_ptr = nullptr;
uint64_t g_ring_size = 0, g_head = 0, g_tail = 0, g_begin = 0;

// Images of destroyed textures, with the serial of the last submit that may still use them.
std::mutex g_grave_lock;
std::vector<std::pair<uint64_t, std::shared_ptr<GpuImage>>> g_grave;
std::vector<std::tuple<uint64_t, VkBuffer, VkDeviceMemory>> g_staging;  // staging_alloc's, freed like the grave's

// The ring's overflow (WONDER_SWITCH2_PROGRESS.md §5zd20). A submit that fills the ring alone (a scene load: up to
// ~190 MB on the Tab S10+, against a 128 MB ring) used to wrap over its own first data before its parts had run: the
// GPU then copied other uploads' bytes into the textures being loaded, and those textures showed blocks of garbage
// colours for as long as they lived (seconds later, in other scenes). The rest of such a submit goes into buffers of
// its own (overflow_alloc), freed when it is retired, as staging buffers. NX_RING_WRAP=1: the old wrap (bisecting).
struct Overflow {
    VkBuffer buffer = VK_NULL_HANDLE;
    uint8_t* ptr = nullptr;
    uint64_t size = 0, used = 0;
} g_overflow;  // the newest of the submit being recorded (frame_begin forgets it; g_staging frees it)
uint64_t g_overflow_count = 0, g_overflow_bytes = 0, g_overflow_buffers = 0;  // per log period
std::atomic<uint64_t> g_stale_reuse{0}, g_stale_share{0}, g_stale_refused{0};  // stale_image (§5zd21)
uint64_t g_reuse_not_rt = 0, g_reuse_not_rt_refused = 0, g_reuse_expired = 0;  // reuse_image, free_retired_images

// GPU-written images of destroyed textures, kept a few submits for a new texture over the same memory with the same
// shape. Wonder re-creates ~180 NVNtexture objects per frame over the same render-target memory: each got a new
// image, and its first draw ended the render pass to set it up (most of the ~300 passes per frame, each reloading
// and storing its targets; docs/nvn/DEBUG.md #34).
struct ReuseKey {
    uint64_t memory = 0;
    uint32_t format = 0, width = 0, height = 0, depth = 0, levels = 0, target = 0, samples = 0, swizzle = 0;
    bool operator<(const ReuseKey& o) const {
        return std::tie(memory, format, width, height, depth, levels, target, samples, swizzle) <
               std::tie(o.memory, o.format, o.width, o.height, o.depth, o.levels, o.target, o.samples, o.swizzle);
    }
};
ReuseKey reuse_key(const Texture& t) {
    uint32_t swizzle = uint32_t(t.swizzle[0]) | uint32_t(t.swizzle[1]) << 8 | uint32_t(t.swizzle[2]) << 16 |
                  uint32_t(t.swizzle[3]) << 24;
    static const bool key_swizzle = std::getenv("NX_KEY_SWIZZLE") != nullptr;  // as image_of's key (#46)
    if (!key_swizzle) swizzle = 0;
    return {t.memory, t.format, t.width, t.height, t.depth, t.levels, t.target, t.samples, swizzle};
}
std::map<ReuseKey, std::pair<uint64_t, std::shared_ptr<GpuImage>>> g_reusable;  // key -> (kept at, image)
uint64_t g_reuse_clock = 0, g_reused = 0;

void wait_fences(const VkFence* fences, uint32_t n) {
    if (!n) return;
    uint64_t t0 = plat::monotonic_ns();
    VkResult r = p_vkWaitForFences(vk_device(), n, fences, VK_TRUE, kFenceTimeout);
    if (r == VK_TIMEOUT) gpu_hung("a submit did not finish in 2 s");
    if (r == VK_ERROR_DEVICE_LOST) gpu_hung("vkWaitForFences");
    if (r != VK_SUCCESS) log("nvn/vk: vkWaitForFences failed (%d)", int(r));
    g_times.fence_ns += plat::monotonic_ns() - t0;
}

// Compute results to guest memory: after the fence of the part that holds the submit's dispatches.
void apply_write_backs(Slot& s) {
    for (const WriteBack& w : s.write_backs) {
        replay::trace_nan_write_back(w.from, w.to, w.size);
        apply_write_back(w);
    }
    s.write_backs.clear();
}

// Waits for the oldest submit in flight and finishes it: compute results to guest memory (if not there yet), its
// descriptor pool and ring stretch free again, images destroyed before it freed. False when nothing is in flight.
// Fences complete in submission order on the one queue, so retiring oldest first keeps write-backs in order.
// The fences of the queued parts below `upto`.
uint32_t queued_fences(const Slot& s, uint32_t upto, VkFence* out) {
    uint32_t n = 0;
    for (uint32_t k = 0; k < upto && k < kMaxParts; k++)
        if (s.queued >> k & 1) out[n++] = s.fences[k];
    return n;
}

bool retire_oldest() {
    Slot* s = nullptr;
    for (uint32_t k = 0; k < g_slot_count; k++)
        if (g_slots[k].serial && (!s || g_slots[k].serial < s->serial)) s = &g_slots[k];
    if (!s) return false;
    VkFence fences[kMaxParts];
    wait_fences(fences, queued_fences(*s, kMaxParts, fences));
    gpu_busy_retired(uint32_t(s - g_slots), s->queued);  // NX_GPU_PROFILE=3
    apply_write_backs(*s);
    // Its command and descriptor pools: reset on the janitor thread (PHASE4.md C2b), or here (NX_JANITOR=0).
    if (janitor_on()) {
        s->reset_done.store(false, std::memory_order_relaxed);
        janitor_run([s] {
            if (g_slot_pools && s->pool_used) p_vkResetCommandPool(vk_device(), s->pool, 0);
            if (s->descriptors_used) p_vkResetDescriptorPool(vk_device(), s->descriptors, 0);
            s->pool_used = s->descriptors_used = false;
            s->reset_done.store(true, std::memory_order_release);
        });
    } else {
        if (s->descriptors_used) p_vkResetDescriptorPool(vk_device(), s->descriptors, 0);
        s->descriptors_used = false;
    }
    g_tail = std::max(g_tail, s->ring_end);
    g_completed = s->serial;
    s->serial = 0;
    // Pipelined: gpu_submit and gpu_present free the images, where the synchronous path does (the reuse clock ticks
    // there, so a destroyed target's image is kept for the same number of submits and presents).
    if (!pipelined()) free_retired_images(g_completed);
    return true;
}

void retire_through(uint64_t serial) {
    while (g_completed < serial && retire_oldest()) {
    }
}

uint32_t replays_in_flight() {
    uint32_t n = 0;
    for (uint32_t k = 0; k < g_slot_count; k++) n += g_slots[k].serial && g_slots[k].replay;
    return n;
}

bool begin_part(VkCommandBuffer cb) {
    if (!g_slot_pools) p_vkResetCommandBuffer(cb, 0);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (p_vkBeginCommandBuffer(cb, &bi) != VK_SUCCESS) {
        log("nvn/vk: vkBeginCommandBuffer failed");
        return false;
    }
    // Earlier submits (and this submit's first part) may still run: their writes come before anything here (image
    // layouts are tracked in submission order, so the images already are where these commands expect them).
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    p_vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0,
                           nullptr, 0, nullptr);
    return true;
}

// Ends `cb` and queues it (Metal encoding happens inside vkQueueSubmit: MoltenVK's synchronous submits). False:
// nothing was queued, `fence` will not signal.
bool submit_part(VkCommandBuffer cb, VkFence fence, VkSemaphore wait, VkSemaphore signal) {
    uint64_t t0 = plat::monotonic_ns();
    // The host reads compute results after the fence.
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    p_vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, nullptr, 0,
                           nullptr);
    p_vkEndCommandBuffer(cb);
    VkPipelineStageFlags stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb;
    if (wait) {
        si.waitSemaphoreCount = 1;
        si.pWaitSemaphores = &wait;
        si.pWaitDstStageMask = &stage;
    }
    if (signal) {
        si.signalSemaphoreCount = 1;
        si.pSignalSemaphores = &signal;
    }
    p_vkResetFences(vk_device(), 1, &fence);
    VkResult r = queue_submit(&si, fence);
    g_times.encode_ns += plat::monotonic_ns() - t0;
    if (r == VK_ERROR_DEVICE_LOST) gpu_hung("vkQueueSubmit");
    if (r != VK_SUCCESS) log("nvn/vk: vkQueueSubmit failed (%d)", int(r));
    return r == VK_SUCCESS;
}

// As replay_list walks it. A list's own commands are scanned once (a recorded list does not change): the walk over
// every command of the frame took ~4% of the replay thread on the S23.
uint32_t count_dispatches(const CmdList& list, int depth) {
    auto& l = const_cast<CmdList&>(list);
    if (l.scanned_size != l.cmds.size()) {
        l.own_dispatches = 0, l.calls.clear();
        for (const Cmd& c : l.cmds) {
            if (c.op == Cmd::DispatchCompute) l.own_dispatches++;
            if (c.op == Cmd::CallList) l.calls.push_back(c.a[0]);
        }
        l.scanned_size = l.cmds.size();
    }
    uint32_t n = l.own_dispatches;
    if (depth < 8)
        for (uint64_t h : l.calls)
            if (auto called = submit_command_list(h)) n += count_dispatches(*called, depth + 1);
    return n;
}

int env_int(const char* name) {
    const char* e = std::getenv(name);
    return e ? std::atoi(e) : 0;
}

// Ends the part being recorded (and its render pass; pending clears stay pending, as they are not tied to a command
// buffer: the next pass on their targets takes them as load operations), queues it, and begins the next one.
VkCommandBuffer next_part(Slot& s) {
    pass_end_only(s.cbs[s.part], 0);
    gpu_busy_part_end(uint32_t(&s - g_slots), s.part, s.cbs[s.part]);
    if (s.ok && submit_part(s.cbs[s.part], s.fences[s.part], VK_NULL_HANDLE, VK_NULL_HANDLE)) s.queued |= 1u << s.part;
    s.part++;
    s.ok = begin_part(s.cbs[s.part]);
    if (s.ok) gpu_busy_part_begin(uint32_t(&s - g_slots), s.part, s.cbs[s.part]);
    if (!s.ok) log("nvn/vk: a part of a submit could not begin: it is lost");
    g_chunk_from = UINT64_MAX;
    return s.cbs[s.part];
}

}  // namespace

// On by default on macOS only (course 1-1 headless: ~50 -> ~59 fps there; DEBUG.md #61): MoltenVK encodes Metal inside
// vkQueueSubmit, which is what the overlap hides. With native drivers (Windows, NVIDIA) the synchronous path played
// at 58 fps against 40 and had none of the exploded meshes the pipelined one shows in course 1-1 (DEBUG-2 #76).
// NX_PIPELINED=0: synchronous; 1..4: pipelined, that many submits in flight (2 at least). NX_ASYNC_GPU wins.
bool pipelined() {
    static const bool on = [] {
        if (env_int("NX_ASYNC_GPU") > 0) return false;
        const char* e = std::getenv("NX_PIPELINED");
#if defined(__APPLE__)
        return !e || std::atoi(e) > 0;
#else
        return e && std::atoi(e) > 0;
#endif
    }();
    return on;
}

bool async_gpu() {
    static const bool on = pipelined() || env_int("NX_ASYNC_GPU") > 0;
    return on;
}

FrameTimes& frame_times() { return g_times; }
uint64_t frame_completed() { return g_completed; }

void frame_init(VkCommandPool pool, uint32_t family, VkBuffer ring, uint8_t* ring_ptr, uint64_t ring_size) {
    // NX_ASYNC_GPU=1 (or 2): two frames in flight; 3 or 4: that many. A frame is a submit and a present, a slot each.
    // NX_PIPELINED likewise (at most that many submits in flight, whatever the slots).
    uint32_t frames = uint32_t(std::clamp(env_int(pipelined() ? "NX_PIPELINED" : "NX_ASYNC_GPU"), 2, 4));
    g_max_replays = frames;
    g_slot_count = std::min(kMaxSlots, frames * 2);
    VkDevice d = vk_device();
    g_slot_pools = std::getenv("NX_CB_RESET") == nullptr;
    for (uint32_t k = 0; k < g_slot_count; k++) {
        if (g_slot_pools) {
            VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
            pci.queueFamilyIndex = family;
            g_slot_pools = p_vkCreateCommandPool(d, &pci, nullptr, &g_slots[k].pool) == VK_SUCCESS;
        }
        VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        cai.commandPool = g_slot_pools ? g_slots[k].pool : pool;
        cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cai.commandBufferCount = kMaxParts;
        p_vkAllocateCommandBuffers(d, &cai, g_slots[k].cbs);
        VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        for (VkFence& f : g_slots[k].fences) p_vkCreateFence(d, &fci, nullptr, &f);
        VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        p_vkCreateSemaphore(d, &sci, nullptr, &g_slots[k].acquired);
    }
    g_ring = ring, g_ring_ptr = ring_ptr, g_ring_size = ring_size;
#if defined(__ANDROID__)
    g_chunk_draws = std::getenv("NX_SUBMIT_CHUNK_DRAWS") ? uint64_t(env_int("NX_SUBMIT_CHUNK_DRAWS")) : 150;
#else
    g_chunk_draws = uint64_t(env_int("NX_SUBMIT_CHUNK_DRAWS"));
#endif
    if (g_chunk_draws) log("nvn/vk: submits queued in parts every %llu draws (NX_SUBMIT_CHUNK_DRAWS)",
                           (unsigned long long)g_chunk_draws);
    if (pipelined())
        log("nvn/vk: pipelined fences (NX_PIPELINED): %u submits in flight, %u slots", frames, g_slot_count);
    else
        log("nvn/vk: asynchronous submits (NX_ASYNC_GPU): %u frames in flight, %u slots", frames, g_slot_count);
}

VkCommandBuffer frame_begin(bool replay) {
    // NX_ASYNC_GPU: this replay may read the results of the last submit that had write-backs. Pipelined: they are
    // in guest memory since that submit returned.
    if (!pipelined() && replay && g_barrier > g_completed) retire_through(g_barrier);
    Slot& s = g_slots[g_next];
    if (s.serial) retire_through(s.serial);
    janitor_wait(s.reset_done);
    if (g_slot_pools && s.pool_used) p_vkResetCommandPool(vk_device(), s.pool, 0);  // none of its parts is in flight
    s.pool_used = true;
    g_next = (g_next + 1) % g_slot_count;
    g_begin = g_head;
    g_overflow = {};  // the last submit's overflow buffers are in g_staging, freed when it retires
    s.replay = replay, s.part = s.queued = s.waited = 0, s.split_part = -1, s.ok = true;
    s.begin_ns = plat::monotonic_ns();
    g_plan_dispatches = g_seen_dispatches = 0;
    g_chunking = false;
    if (!begin_part(s.cbs[0])) {
        g_current = nullptr;
        return VK_NULL_HANDLE;
    }
    g_current = &s;
    gpu_busy_part_begin(uint32_t(&s - g_slots), 0, s.cbs[0]);  // NX_GPU_PROFILE=3
    return s.cbs[0];
}

// ---- pipelined: the split after the last dispatch ----------------------------------------------------------------

size_t frame_plan_split(const std::vector<std::shared_ptr<CmdList>>& lists) {
    static const bool off = std::getenv("NX_PIPELINED_NO_SPLIT") != nullptr;
    g_plan_dispatches = g_seen_dispatches = 0;
    g_chunking = g_chunk_draws && g_current, g_chunk_from = UINT64_MAX;  // not with the profile (not called then)
    // NX_COMPUTE_SEPARATE's write-backs are not counted by written_bind_count(): such a submit is waited for whole.
    if (!pipelined() || off || !g_current || replay::written_separate()) return SIZE_MAX;
    size_t last = SIZE_MAX;
    for (size_t k = 0; k < lists.size(); k++)
        if (uint32_t n = count_dispatches(*lists[k], 0)) g_plan_dispatches += n, last = k;
    return last;
}

VkCommandBuffer frame_cb(VkCommandBuffer cb) { return g_current ? g_current->cbs[g_current->part] : cb; }

VkCommandBuffer frame_split(VkCommandBuffer cb) {
    Slot* s = g_current;
    if (!pipelined() || !s || s->split_part >= 0 || cb != s->cbs[s->part] || s->part + 1 >= kMaxParts)
        return frame_cb(cb);
    s->split_part = int(s->part);
    s->binds_at_split = replay::written_bind_count();
    g_stats.splits++, g_stats.first_ns += plat::monotonic_ns() - s->begin_ns;
    VkCommandBuffer next = next_part(*s);
    if (!(s->queued >> s->split_part & 1))
        log("nvn/vk: the part with a split submit's dispatches was not queued: its compute results are lost");
    return next;
}

VkCommandBuffer frame_chunk(VkCommandBuffer cb, uint64_t draws, bool pass_open) {
    Slot* s = g_current;
    // Two parts are kept for the split and the last one.
    if (!g_chunking || !s || cb != s->cbs[s->part] || s->part + 2 >= kMaxParts) return cb;
    if (g_chunk_from == UINT64_MAX) g_chunk_from = draws;
    uint64_t since = draws - g_chunk_from;
    if (since < g_chunk_draws || (pass_open && since < 2 * g_chunk_draws)) return cb;
    g_stats.chunks++;
    return next_part(*s);
}

VkCommandBuffer frame_after_dispatch(VkCommandBuffer cb) {
    if (!g_plan_dispatches || ++g_seen_dispatches != g_plan_dispatches) return frame_cb(cb);
    return frame_split(cb);
}

VkSemaphore frame_acquire_semaphore() {  // the next slot's (frame_begin takes that slot)
    Slot& s = g_slots[g_next];
    if (s.serial) retire_through(s.serial);  // its last present waited on it
    return s.acquired;
}

VkSemaphore frame_rendered_semaphore(uint32_t image) {
    if (image >= kMaxSlots) image = kMaxSlots - 1;
    if (!g_rendered[image]) {
        VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        p_vkCreateSemaphore(vk_device(), &sci, nullptr, &g_rendered[image]);
    }
    return g_rendered[image];
}

void frame_submit(VkSemaphore wait, VkSemaphore signal, std::vector<WriteBack> write_backs, bool wait_done) {
    if (!g_current) return;
    Slot& s = *g_current;
    g_current = nullptr;
    if (pipelined() && s.replay) {
        g_stats.submits++;
        if (s.split_part >= 0) g_stats.replay_ns += plat::monotonic_ns() - s.begin_ns;
    }
    if (s.ok) gpu_busy_part_end(uint32_t(&s - g_slots), s.part, s.cbs[s.part]);
    const bool has_last = s.ok && submit_part(s.cbs[s.part], s.fences[s.part], wait, signal);
    if (has_last) s.queued |= 1u << s.part;
    if (!s.queued) return;  // nothing ran: no results to copy back, the slot stays free
    s.serial = ++g_submitted;
    s.ring_end = g_head;
    s.write_backs = std::move(write_backs);
    // The dispatches were in a part that never ran: the buffers hold no results, and copying them back would put the
    // inputs (or older data) over guest memory.
    const bool has_first = s.split_part >= 0 && (s.queued >> s.split_part & 1);
    if (s.split_part >= 0 && !has_first) s.write_backs.clear();
    if (pipelined()) {
        // At most g_max_replays submits in flight, this one included: the oldest are waited for once this one is
        // queued (before, the rest waited behind them and the GPU idled between the two parts).
        struct Throttle {
            Slot& s;
            ~Throttle() {
                if (!s.replay) return;
                bool waited = false;
                while (replays_in_flight() > g_max_replays && retire_oldest()) waited = true;
                g_stats.throttled += waited;
            }
        } throttle{s};
        // Debug features read this submit's results right after it: all of it is waited for. Otherwise only compute
        // results are: they are in the first part when the split came after the last dispatch that wrote storage.
        if (wait_done) {
            retire_through(s.serial);
        } else if (!s.write_backs.empty()) {
            if (has_first && has_last && replay::written_bind_count() == s.binds_at_split) {
                VkFence fences[kMaxParts];
                wait_fences(fences, queued_fences(s, uint32_t(s.split_part) + 1, fences));
                apply_write_backs(s);
                g_stats.waited_first++;
            } else {
                retire_through(s.serial);
                g_stats.waited_whole++;
            }
        }
        return;
    }
    if (!s.write_backs.empty()) g_barrier = s.serial;
    // NX_ASYNC_COMPUTE_WAIT=1: submits with compute results are waited for at once, as synchronously (the game then
    // finds them in its memory right after nvnQueueSubmitCommands, without waiting on a sync).
    static const bool compute_wait = std::getenv("NX_ASYNC_COMPUTE_WAIT") != nullptr;
    // NX_ASYNC_WAIT_ALL=1 (diagnostic, WONDER_SWITCH2_PROGRESS.md §5zd20): every submit and present waited for at
    // once. The slots, the ring and the parts stay as they are; only the overlap with the GPU is gone.
    static const bool wait_all = std::getenv("NX_ASYNC_WAIT_ALL") != nullptr;
    if (wait_done || wait_all || (compute_wait && !s.write_backs.empty())) retire_through(s.serial);
}

void frame_wait_all() {
    retire_through(g_submitted);
    for (uint32_t k = 0; k < g_slot_count; k++) janitor_wait(g_slots[k].reset_done);
}

uint64_t frame_upload_used() { return g_head - g_begin; }
uint64_t frame_ring_size() { return async_gpu() ? g_ring_size : 0; }

void frame_log(uint64_t frames) {
    gpu_busy_log(frames);
    if (uint64_t waits = g_game_waits.exchange(0))
        log("nvn/vk: the game's waits (syncs, finish, pool invalidate/finalize): %llu blocked, %.2f ms per frame",
            (unsigned long long)waits, double(g_game_wait_ns.exchange(0)) / 1e6 / double(frames));
    if (g_staging_count)
        log("nvn/vk: staging: %llu buffers, %.1f MB over %llu frames", (unsigned long long)g_staging_count,
            double(g_staging_bytes) / 1e6, (unsigned long long)frames);
    g_staging_count = g_staging_bytes = 0;
    if (g_overflow_count)
        log("nvn/vk: upload ring full: %llu uploads (%.1f MB) in %llu overflow buffers over %llu frames",
            (unsigned long long)g_overflow_count, double(g_overflow_bytes) / 1e6, (unsigned long long)g_overflow_buffers,
            (unsigned long long)frames);
    g_overflow_count = g_overflow_bytes = g_overflow_buffers = 0;
    if (!pipelined() || !g_stats.submits) return;
    const Stats& t = g_stats;
    log("nvn/vk: pipelined: %llu submits, %llu split (first part: %.0f%% of the replay), write-backs after the first "
        "part %llu, after the whole submit %llu, %llu waited for the oldest in flight, %llu chunks queued early",
        (unsigned long long)t.submits, (unsigned long long)t.splits,
        t.replay_ns ? 100.0 * double(t.first_ns) / double(t.replay_ns) : 0.0, (unsigned long long)t.waited_first,
        (unsigned long long)t.waited_whole, (unsigned long long)t.throttled, (unsigned long long)t.chunks);
    g_stats = {};
}

namespace {
// See g_overflow above.
Upload overflow_alloc(uint64_t size, uint64_t align) {
    uint64_t at = (g_overflow.used + align - 1) & ~(align - 1);
    if (!g_overflow.buffer || at + size > g_overflow.size) {
        VkDevice d = vk_device();
        VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bi.size = std::max<uint64_t>(size, 32ull << 20);
        bi.usage = kUploadUsage;
        VkBuffer buffer = VK_NULL_HANDLE;
        if (p_vkCreateBuffer(d, &bi, nullptr, &buffer) != VK_SUCCESS) return {};
        VkMemoryRequirements req{};
        p_vkGetBufferMemoryRequirements(d, buffer, &req);
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = upload_memory_type(req.memoryTypeBits);
        VkDeviceMemory memory = VK_NULL_HANDLE;
        void* p = nullptr;
        if (p_vkAllocateMemory(d, &ai, nullptr, &memory) != VK_SUCCESS || p_vkBindBufferMemory(d, buffer, memory, 0) ||
            p_vkMapMemory(d, memory, 0, VK_WHOLE_SIZE, 0, &p) != VK_SUCCESS) {
            p_vkDestroyBuffer(d, buffer, nullptr);
            if (memory) p_vkFreeMemory(d, memory, nullptr);
            return {};
        }
        {
            std::lock_guard lk(g_grave_lock);
            g_staging.push_back({g_submitted + 1, buffer, memory});  // freed once the submit being recorded retires
        }
        g_overflow = {buffer, static_cast<uint8_t*>(p), bi.size, 0};
        g_overflow_buffers++;
        at = 0;
    }
    g_overflow.used = at + size;
    g_overflow_count++, g_overflow_bytes += size;
    return {g_overflow.buffer, at, g_overflow.ptr + at};
}
}  // namespace

Upload frame_upload_alloc(uint64_t size, uint64_t align) {
    static const bool old_wrap = std::getenv("NX_RING_WRAP") != nullptr;
    uint64_t at = (g_head + align - 1) & ~(align - 1);
    if (at % g_ring_size + size > g_ring_size) at += g_ring_size - at % g_ring_size;  // never split: the next lap
    while (at + size > g_tail + g_ring_size)  // the oldest submits' data is still read: wait for them
        if (!retire_oldest()) {
            // This submit alone filled the ring: its data stays where it is, the rest goes elsewhere.
            if (!old_wrap && g_current)
                if (Upload u = overflow_alloc(size, align); u.buffer) return u;
            // The submit's parts queued so far may still read its data: wait for them first.
            if (g_current && g_current->waited < g_current->part) {
                VkFence fences[kMaxParts];
                wait_fences(fences, queued_fences(*g_current, g_current->part, fences));
                g_current->waited = g_current->part;
            }
            log("nvn/vk: upload buffer full (%llu bytes in this submit)", (unsigned long long)(at + size - g_begin));
            g_tail = at + size - g_ring_size;  // this submit wraps over its own first data, as the synchronous path
            replay::dedup_reset();
            break;
        }
    g_head = at + size;
    uint64_t off = at % g_ring_size;
    return {g_ring, off, g_ring_ptr + off};
}

VkDescriptorSet frame_descriptor_set(VkDescriptorSetLayout layout) {
    if (!g_current) return VK_NULL_HANDLE;
    Slot& s = *g_current;
    if (!s.descriptors) s.descriptors = make_descriptor_pool();
    s.descriptors_used = true;
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = s.descriptors;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &layout;
    VkDescriptorSet set = VK_NULL_HANDLE;
    if (p_vkAllocateDescriptorSets(vk_device(), &ai, &set) != VK_SUCCESS) log("nvn/vk: descriptor pool exhausted");
    return set;
}

// nvn.h: what nvnQueueFenceSync records and nvnSyncWait / nvnQueueFinish wait for. The synchronous path has nothing
// in flight when the game gets control back: 0, and no wait. Pipelined submits are in flight, but nothing the game
// can observe depends on them: compute results are in guest memory when gpu_submit returns, the GPU reads its inputs
// from the ring (never from guest memory), and images, descriptor pools and ring stretches are kept until their
// submit is retired. So it is 0 there too, and those calls neither wait nor take the GPU lock.
uint64_t gpu_serial() { return async_gpu() && !pipelined() ? g_submitted.load() : 0; }
uint64_t gpu_write_back_serial() { return async_gpu() && !pipelined() ? g_barrier.load() : 0; }

bool gpu_wait(uint64_t serial) {
    if (!serial || pipelined() || g_completed >= serial) return false;
    uint64_t t0 = plat::monotonic_ns();
    std::lock_guard lk(gpu_lock());
    if (g_completed < serial) retire_through(serial);
    g_game_waits++, g_game_wait_ns += plat::monotonic_ns() - t0;
    return true;
}

// How long (submits and presents) the image of a destroyed render target waits to be reused: 8 by default.
// NX_REUSE_KEEP=<n> to test longer.
uint64_t reuse_keep() {
    static const uint64_t n = std::getenv("NX_REUSE_KEEP") ? std::strtoull(std::getenv("NX_REUSE_KEEP"), nullptr, 10) : 8;
    return n;
}

void free_retired_images(uint64_t completed) {
    std::vector<std::shared_ptr<GpuImage>> dead;
    {
        std::lock_guard lk(g_grave_lock);
        g_reuse_clock++;  // once per submit and present
        for (auto it = g_reusable.begin(); it != g_reusable.end();)
            if (it->second.first + reuse_keep() < g_reuse_clock) {
                g_reuse_expired++;
                g_grave.push_back({g_submitted + 1, std::move(it->second.second)});
                it = g_reusable.erase(it);
            } else {
                ++it;
            }
        if (g_reuse_clock % 3600 == 0 && (g_reused || g_stale_reuse || g_stale_share || g_reuse_expired))
            log("nvn/vk: %llu images reused, %llu expired unclaimed; for textures not render targets: %llu (%llu refused); "
                "of another pool generation: %llu reuses, %llu shares, %llu of those refused",
                (unsigned long long)g_reused, (unsigned long long)g_reuse_expired, (unsigned long long)g_reuse_not_rt,
                (unsigned long long)g_reuse_not_rt_refused, (unsigned long long)g_stale_reuse.load(),
                (unsigned long long)g_stale_share.load(), (unsigned long long)g_stale_refused.load());
        auto done = std::stable_partition(g_grave.begin(), g_grave.end(),
                                          [&](const auto& e) { return e.first > completed; });
        for (auto it = done; it != g_grave.end(); ++it) dead.push_back(std::move(it->second));
        g_grave.erase(done, g_grave.end());
    }
    std::vector<std::pair<VkBuffer, VkDeviceMemory>> staging;
    {
        std::lock_guard lk(g_grave_lock);
        auto done = std::stable_partition(g_staging.begin(), g_staging.end(),
                                          [&](const auto& e) { return std::get<0>(e) > completed; });
        for (auto it = done; it != g_staging.end(); ++it) staging.push_back({std::get<1>(*it), std::get<2>(*it)});
        g_staging.erase(done, g_staging.end());
    }
    for (auto& [b, m] : staging) p_vkDestroyBuffer(vk_device(), b, nullptr), p_vkFreeMemory(vk_device(), m, nullptr);
    for (auto& img : dead) {
        if (!img || !img->image) continue;
        if (img.use_count() > 1) continue;  // another live texture shares it (image_of): freed with the last one
        forget_image_views(*img);
        if (img->view) p_vkDestroyImageView(vk_device(), img->view, nullptr);
        p_vkDestroyImage(vk_device(), img->image, nullptr);
        image_memory_free(img->memory, img->memory_handle);
        img->image = VK_NULL_HANDLE, img->memory = VK_NULL_HANDLE, img->view = VK_NULL_HANDLE, img->memory_handle = 0;
    }
}

// Images of destroyed textures are freed once no submit can use them: the one being recorded now included (a replay
// on another thread may hold it), hence the serial after the last submitted. Before the graveyard no image was ever
// freed: the opening creates textures continuously and the process reached 85 GB (the machine froze).
Texture::~Texture() {
    if (watched) plat::write_watch_remove(memory);
    if (!image) return;
    std::lock_guard lk(g_grave_lock);
    if (image->rendered && image->image && memory) {
        auto& slot = g_reusable[reuse_key(*this)];
        if (slot.second) g_grave.push_back({g_submitted + 1, std::move(slot.second)});
        slot = {g_reuse_clock, std::move(image)};
        return;
    }
    g_grave.push_back({g_submitted + 1, std::move(image)});
}

void bury_image(std::shared_ptr<GpuImage> img) {
    if (!img) return;
    std::lock_guard lk(g_grave_lock);
    g_grave.push_back({g_submitted + 1, std::move(img)});
}

// A host-visible buffer of its own for one big texture upload: the ring is 256 MB, and a scene load with ASTC decoded
// to RGBA8 staged 312 MB in one submit, wrapping over its own data before the GPU had copied it (DEBUG-2 #72). Freed
// once the submit being recorded is retired (as images in the grave). Falls back to the ring if allocation fails.
Upload staging_alloc(uint64_t size) {
    g_staging_count++, g_staging_bytes += size;
    VkDevice d = vk_device();
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = size;
    bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    VkBuffer buffer = VK_NULL_HANDLE;
    if (p_vkCreateBuffer(d, &bi, nullptr, &buffer) != VK_SUCCESS) return upload_alloc(size, 16);
    VkMemoryRequirements req{};
    p_vkGetBufferMemoryRequirements(d, buffer, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex =
        memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkDeviceMemory memory = VK_NULL_HANDLE;
    void* p = nullptr;
    if (p_vkAllocateMemory(d, &ai, nullptr, &memory) != VK_SUCCESS || p_vkBindBufferMemory(d, buffer, memory, 0) ||
        p_vkMapMemory(d, memory, 0, VK_WHOLE_SIZE, 0, &p) != VK_SUCCESS) {
        p_vkDestroyBuffer(d, buffer, nullptr);
        if (memory) p_vkFreeMemory(d, memory, nullptr);
        return upload_alloc(size, 16);
    }
    std::lock_guard lk(g_grave_lock);
    g_staging.push_back({g_submitted + 1, buffer, memory});
    return {buffer, 0, static_cast<uint8_t*>(p)};
}

// Images are found by the host address of their storage (and its shape). A memory pool finalized and another
// initialized over the same storage (a scene change: the plaza after the Bowser cutscene) made a new texture there take
// an image of the old pool's: a render of the old scene for a texture the CPU filled (rendered images are never
// uploaded), or a target with the old scene in it (WONDER_SWITCH2_PROGRESS.md §5zd21). The pool generation tells them
// apart. Measured on the Tab S10+: 25-77 such shares per session, the same in both modes, and the synchronous one
// clean with them, so they are only counted; NX_POOL_GENERATIONS=1 refuses them (bisecting).
bool pool_generations() {
    static const bool on = [] {
        const char* e = std::getenv("NX_POOL_GENERATIONS");
        return e && e[0] == '1';
    }();
    return on;
}

// A destroyed render target's image is kept for a new texture with the same storage and shape (#39: the course actors
// sample AO and ambient targets of a texture object re-made since). Only a render target (COMPRESSIBLE, 0x8, as
// ensure_contents tells them) may take one: a texture the CPU fills (UI, sky, decals) took a render of another frame,
// which as a rendered image is never uploaded (the hypothesis of §5zd21). Measured on the Tab S10+: no reuse at all
// in either mode (live textures share first, image_of), so only counted; NX_REUSE_RT_ONLY=1 refuses them.
bool reuse_rt_only() {
    static const bool on = [] {
        const char* e = std::getenv("NX_REUSE_RT_ONLY");
        return e && e[0] == '1';
    }();
    return on;
}

bool stale_image(const char* what, const GpuImage& img, const Texture& t, uint64_t age) {
    const bool refuse = pool_generations();
    (what[0] == 'r' ? g_stale_reuse : g_stale_share)++;
    if (refuse) g_stale_refused++;
    static std::atomic<int> logged{0};
    if (logged++ < 30)
        log("nvn/vk: stale image (%s): image %llu of pool generation %llu, %ux%u format %#x%s, for a texture of "
            "generation %llu at memory %llx (%ux%u format %#x), kept %llu ticks: %s",
            what, (unsigned long long)img.id, (unsigned long long)img.pool_serial, img.width, img.height, img.nvn_format,
            img.rendered ? " rendered" : "", (unsigned long long)t.pool_serial, (unsigned long long)t.memory, t.width,
            t.height, t.format, (unsigned long long)age, refuse ? "refused (NX_POOL_GENERATIONS=1)" : "used");
    return refuse;
}

std::shared_ptr<GpuImage> reuse_image(const Texture& t) {
    // Off unless NX_IMAGE_REUSE=1: a CPU-written texture placed over a freed render target's memory would sample the
    // old render, and swizzle/alpha_from_r come from the old texture (review of #34). The pass breaks it avoided came
    // mostly from destroyed pool textures (fixed separately).
    // On by default (NX_IMAGE_REUSE=0 turns it off): the course's player samples AO/ambient targets of a destroyed
    // texture object two frames later (DEBUG.md #39); without the reused image they were black (a dark Mario).
    static const bool off = [] {
        const char* e = std::getenv("NX_IMAGE_REUSE");
        return e && e[0] == '0';
    }();
    if (!t.memory || off) return nullptr;
    std::lock_guard lk(g_grave_lock);
    auto it = g_reusable.find(reuse_key(t));
    if (it == g_reusable.end()) return nullptr;
    const uint64_t age = g_reuse_clock - it->second.first;
    if (!(t.flags & 0x8)) {  // not a render target
        g_reuse_not_rt++;
        const bool refuse = reuse_rt_only();
        g_reuse_not_rt_refused += refuse;
        static int logged = 0;
        if (logged++ < 20)
            log("nvn/vk: reuse for a texture that is not a render target (flags %#x, %ux%u format %#x, memory %llx): "
                "image %llu kept %llu ticks: %s", t.flags, t.width, t.height, t.format, (unsigned long long)t.memory,
                (unsigned long long)(it->second.second ? it->second.second->id : 0), (unsigned long long)age,
                refuse ? "refused (NX_REUSE_RT_ONLY=1; it stays for a target)" : "given");
        if (refuse) return nullptr;
    }
    std::shared_ptr<GpuImage> img = std::move(it->second.second);
    g_reusable.erase(it);
    if (img && img->pool_serial != t.pool_serial && stale_image("reuse", *img, t, age)) {
        g_grave.push_back({g_submitted + 1, std::move(img)});  // freed as an expired one
        return nullptr;
    }
    g_reused++;
    return img;
}

// ---- VRAM eviction (experimental, NX_VRAM_EVICT=1; off by default) ------------------------------------------------
// The device memory of a long session grows with the images of textures the game keeps alive (~37,000 objects on the
// world map), many of them not sampled for minutes (NX_VRAM_LOG=2's census). An image can give its memory back when it
// can be made again exactly as it is: uploaded from guest memory, never written by the GPU (rendered: draws, clears,
// copies, storage bindings), no nvnTextureWriteTexels in it, its texture not a render target (those are rebuilt as
// cleared, nvn_pipeline.cpp), its pool physical and still of the same generation, and the bytes its upload read
// unchanged (a hash taken at the upload). It is then marked evicted: its Vulkan image, views and memory go to the grave
// with the next submit's serial (freed when the GPU is done, as a destroyed texture's), and image_of makes a new one at
// the next use, which sampled_ready treats as not ready (the pass ends first, as for a first use).
// It acts only above a budget: NX_VRAM_EVICT_BUDGET_MB (an artificial one, for tests), else the driver's
// (VK_EXT_memory_budget); measured as device memory held minus the free space inside image blocks. From 90% of it,
// images idle for NX_VRAM_EVICT_AGE_S (default 60) go, oldest first, until 80%, at most NX_VRAM_EVICT_MB (default 64)
// per check; a check every 30 presents. NX_VRAM_EVICT_ASTC=0 keeps ASTC images (decoded on the CPU when rebuilt).
namespace {

struct EvictStats {
    uint64_t checks = 0, over = 0, evicted = 0, evicted_bytes = 0, changed = 0, pool_gone = 0, rebuilt = 0,
             rebuilt_bytes = 0, rebuild_ns = 0, rebuild_max_ns = 0, check_ns_max = 0, last_log = 0;
} g_evict;

uint64_t env_u64(const char* name, uint64_t def) {
    const char* e = std::getenv(name);
    return e && *e ? std::strtoull(e, nullptr, 10) : def;
}

// Under objects_lock (shared): the source of an upload is still what it was.
bool evict_source_same(const GpuImage& img) {
    auto it = objects().find(img.src_pool);
    auto* pool = it == objects().end() ? nullptr : dynamic_cast<MemoryPool*>(it->second.get());
    if (!pool || !pool->memory || pool->serial != img.pool_serial || img.src_offset + img.src_bytes > pool->size ||
        pool->memory + img.src_offset != img.src_memory) {
        g_evict.pool_gone++;
        return false;
    }
    if (texture_hash(reinterpret_cast<const uint8_t*>(img.src_memory), img.src_bytes) != img.src_hash) {
        g_evict.changed++;
        return false;
    }
    return true;
}

}  // namespace

bool evict_on() {
    static const bool on = [] {
        const char* e = std::getenv("NX_VRAM_EVICT");
        return e && e[0] == '1';
    }();
    return on;
}

void vram_evict_note_upload(const Texture& t, GpuImage& img) {
    MemoryPool* pool = get<MemoryPool>(t.pool);  // under the object table's shared lock (nvn.h)
    img.src_bytes = 0;
    if (!pool || !pool->memory || t.memory != pool->memory + t.offset) return;  // virtual or unknown: kept
    img.src_pool = t.pool, img.src_offset = t.offset, img.src_memory = t.memory, img.src_bytes = t.readable;
    img.src_hash = texture_hash(reinterpret_cast<const uint8_t*>(t.memory), t.readable);
}

void vram_evict_note_rebuild(const GpuImage& img, uint64_t ns) {
    g_evict.rebuilt++, g_evict.rebuild_ns += ns, g_evict.rebuild_max_ns = std::max(g_evict.rebuild_max_ns, ns);
    g_evict.rebuilt_bytes += image_memory_size(img.memory, img.memory_handle);
}

// NX_RELEASE_REPLACED (nvn_supersede.cpp): the image's Vulkan objects go to the grave with the next submit's serial
// (freed when the GPU is done with every submit that could use them, as a destroyed texture's image); the GpuImage
// stays, marked superseded, and image_of makes a new one at its next use. Under the GPU lock, between submits.
void supersede_release(GpuImage& img) {
    auto husk = std::make_shared<GpuImage>();
    husk->image = img.image, husk->memory = img.memory, husk->memory_handle = img.memory_handle, husk->view = img.view;
    img.image = VK_NULL_HANDLE, img.memory = VK_NULL_HANDLE, img.memory_handle = 0, img.view = VK_NULL_HANDLE;
    img.superseded = true, img.layout = VK_IMAGE_LAYOUT_UNDEFINED;
    std::lock_guard gl(g_grave_lock);
    g_grave.push_back({g_submitted + 1, std::move(husk)});
}

void vram_evict_tick() {
    static uint64_t presents = 0;
    if (++presents % 30) return;
    const uint64_t t0 = plat::monotonic_ns();
    g_evict.checks++;
    static const uint64_t artificial = env_u64("NX_VRAM_EVICT_BUDGET_MB", 0) << 20;
    static const uint64_t min_age = env_u64("NX_VRAM_EVICT_AGE_S", 60) * 1'000'000'000ull;
    static const uint64_t per_check = env_u64("NX_VRAM_EVICT_MB", 64) << 20;
    static const bool astc = env_u64("NX_VRAM_EVICT_ASTC", 1) != 0;
    const uint64_t budget = artificial ? artificial : vram_driver_budget();
    const ImageMemoryStats im = image_memory_stats();
    const uint64_t held = device_memory_held(), idle_inside = im.block_bytes - im.used_bytes;
    const uint64_t used = held > idle_inside ? held - idle_inside : 0;
    if (budget && used > budget / 10 * 9) {
        g_evict.over++;
        const uint64_t target = budget / 10 * 8;
        std::vector<std::shared_ptr<GpuImage>> candidates;
        for_each_live_image([&](const std::shared_ptr<GpuImage>& img) {
            if (img->evicted || !img->image || !img->uploaded || img->rendered || img->texels || img->target_flag ||
                !img->src_bytes || (!astc && astc_decoded(img->nvn_format)) || t0 < img->last_used + min_age)
                return;
            candidates.push_back(img);
        });
        std::sort(candidates.begin(), candidates.end(),
                  [](const auto& a, const auto& b) { return a->last_used < b->last_used; });
        uint64_t freed = 0, need = used - target;
        std::shared_lock lk(objects_lock());  // pools stay as they are while their bytes are hashed
        for (const auto& img : candidates) {
            if (freed >= need || freed >= per_check) break;
            if (!evict_source_same(*img)) continue;
            const uint64_t size = image_memory_size(img->memory, img->memory_handle);
            auto husk = std::make_shared<GpuImage>();  // the Vulkan objects, freed by the grave
            husk->image = img->image, husk->memory = img->memory, husk->memory_handle = img->memory_handle;
            husk->view = img->view;
            img->image = VK_NULL_HANDLE, img->memory = VK_NULL_HANDLE, img->memory_handle = 0, img->view = VK_NULL_HANDLE;
            img->evicted = true, img->uploaded = false, img->layout = VK_IMAGE_LAYOUT_UNDEFINED;
            {
                std::lock_guard gl(g_grave_lock);
                g_grave.push_back({g_submitted + 1, std::move(husk)});
            }
            freed += size, g_evict.evicted++, g_evict.evicted_bytes += size;
        }
    }
    const uint64_t ns = plat::monotonic_ns() - t0;
    g_evict.check_ns_max = std::max(g_evict.check_ns_max, ns);
    if (t0 > g_evict.last_log + 30'000'000'000ull) {
        g_evict.last_log = t0;
        log("nvn/vk: vram evict: used %.0f MB of budget %.0f MB%s; %llu checks (%llu over 90%%, the slowest %.2f ms); "
            "evicted %llu (%.0f MB); kept: %llu changed since upload, %llu pool gone; rebuilt %llu (%.0f MB, %.1f ms, the "
            "slowest %.2f ms)",
            double(used) / (1 << 20), double(budget) / (1 << 20), artificial ? " (artificial)" : "",
            (unsigned long long)g_evict.checks, (unsigned long long)g_evict.over, double(g_evict.check_ns_max) / 1e6,
            (unsigned long long)g_evict.evicted, double(g_evict.evicted_bytes) / (1 << 20),
            (unsigned long long)g_evict.changed, (unsigned long long)g_evict.pool_gone, (unsigned long long)g_evict.rebuilt,
            double(g_evict.rebuilt_bytes) / (1 << 20), double(g_evict.rebuild_ns) / 1e6,
            double(g_evict.rebuild_max_ns) / 1e6);
    }
}

}  // namespace nx::nvn
