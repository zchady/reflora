// NX_RELEASE_REPLACED=1 (Windows only; diagnostic prototype, off by default): a render target's image gives its device
// memory back once its contents are provably gone on the console and nothing has used it for a while.
//
// Wonder's dynamic resolution re-initializes its targets with a new size over the same guest memory; the struct that
// held the old size keeps it (and our image) until the game reuses that struct (TextureInitialize, nvn_objects.cpp). An
// old image A is released only when ALL of these hold (otherwise nothing changes for it):
// - proof that its bytes were overwritten: after A's last use, another target B at the same guest address and pool
//   generation was cleared whole (no scissor, every channel, all aspects, level 0 of a one-level one-layer image), and
//   that clear covers every byte of A's guest storage. B's block-linear layout depends on its width and on a block
//   height we do not know (at most 32 GOBs: 256 rows), so only B's complete 256-row bands count as written, and only
//   when its rows have no padding (width x bytes per pixel a multiple of 64): A's storage (a prefix of the same
//   memory) must fit in them. A partial overlap, or anything that bound cannot show, keeps A.
// - A not used (any use: sampled, rendered, copied, cleared; transition records it) for 600 frames: targets in
//   alternating use (the course's poison-pool bubbles) are used every frame and never qualify.
// - A a render target the GPU wrote, not evicted (NX_VRAM_EVICT), still with its Vulkan image.
// The release (supersede_release, nvn_frames.cpp) sends its Vulkan objects to the grave with the next submit's serial:
// freed only when the GPU has finished every submit that could use them. If the game uses A again, image_of makes it
// again (a render target begins cleared: on the console those bytes hold B's clear); counted and logged.
#include <algorithm>
#include <cstdlib>
#include <mutex>
#include <unordered_map>

#include "nvn_vk.h"

namespace nx::nvn {

using namespace nx::vk;

namespace {

struct FullClear {
    uint64_t frame = 0, image = 0, covered = 0, pool_serial = 0;
    uint32_t width = 0, height = 0;
};
std::mutex g_sup_lock;
std::unordered_map<uint64_t, FullClear> g_clears;  // guest address -> the latest full clear that covers something
struct SupStats {
    uint64_t clears = 0, clears_covering = 0, checks = 0, released = 0, released_bytes = 0, remade = 0;
    uint64_t kept_no_clear = 0, kept_partial = 0, kept_recent = 0, kept_used_after = 0, kept_small = 0, kept_remade = 0;
} g_ss;
constexpr uint64_t kIdleFrames = 600;
// Below 1 MB a release saves nothing worth a risk: the menus' 64x36 targets (run ron1-3) were released and used again.
constexpr uint64_t kMinBytes = 1ull << 20;
// A target (guest address, size) the game used again after its release is never released again.
struct SizeKey {
    uint64_t memory;
    uint32_t width, height;
    bool operator==(const SizeKey& o) const { return memory == o.memory && width == o.width && height == o.height; }
};
struct SizeKeyHash {
    size_t operator()(const SizeKey& k) const { return std::hash<uint64_t>()(k.memory ^ (uint64_t(k.width) << 40) ^ k.height); }
};
std::unordered_map<SizeKey, int, SizeKeyHash> g_remade;

}  // namespace

bool release_replaced_on() {
#ifdef _WIN32
    static const bool on = [] {
        const char* e = std::getenv("NX_RELEASE_REPLACED");
        return e && e[0] == '1';
    }();
    return on;
#else
    return false;
#endif
}

void supersede_note_full_clear(uint64_t guest, const GpuImage& img, uint32_t level, VkImageAspectFlags aspects) {
    (void)guest;
    if (!release_replaced_on() || !img.guest_memory) return;
    g_ss.clears++;
    if (level != 0 || img.levels != 1 || img.layers != 1 || aspects != img.aspect) return;
    uint32_t bw = 1, bh = 1, bytes = 0;
    format_block(img.nvn_format, bw, bh, bytes);
    if (bw != 1 || bh != 1 || !bytes) return;
    const uint64_t row = uint64_t(img.width) * bytes;
    if (row % 64) return;  // padding inside each row: some bytes of the storage are not pixels the clear writes
    const uint64_t covered = std::min<uint64_t>(uint64_t(img.height / 256) * 256 * row, img.guest_bytes);
    if (!covered) return;
    g_ss.clears_covering++;
    std::lock_guard lk(g_sup_lock);
    g_clears[img.guest_memory] = {g_frame.load(), img.id, covered, img.pool_serial, img.width, img.height};
}

void supersede_tick() {
    static uint64_t presents = 0;
    if (++presents % 60) return;
    g_ss.checks++;
    const uint64_t now = g_frame.load();
    std::vector<std::shared_ptr<GpuImage>> release;
    {
        std::lock_guard lk(g_sup_lock);
        for_each_live_image([&](const std::shared_ptr<GpuImage>& img) {
            if (!img->image || img->superseded || img->evicted || !img->target_flag || !img->rendered ||
                !img->guest_memory)
                return;
            auto it = g_clears.find(img->guest_memory);
            if (it == g_clears.end() || it->second.image == img->id || it->second.pool_serial != img->pool_serial)
                return void(g_ss.kept_no_clear++);
            const FullClear& c = it->second;
            if (c.frame <= img->touched) return void(g_ss.kept_used_after++);  // used since that clear
            if (img->guest_bytes > c.covered) return void(g_ss.kept_partial++);  // not all its bytes shown written
            if (img->guest_bytes < kMinBytes) return void(g_ss.kept_small++);
            if (g_remade.count({img->guest_memory, img->width, img->height})) return void(g_ss.kept_remade++);
            if (now - img->touched < kIdleFrames) return void(g_ss.kept_recent++);
            release.push_back(img);
            static int logged = 0;
            if (logged++ < 30)
                log("nvn/vk: release replaced: image %llu (%ux%u, format %#x, %llu guest bytes, last used %llu frames ago) "
                    "at %llx: overwritten by the full clear of image %llu (%ux%u, %llu bytes shown written) at frame %llu",
                    (unsigned long long)img->id, img->width, img->height, img->nvn_format,
                    (unsigned long long)img->guest_bytes, (unsigned long long)(now - img->touched),
                    (unsigned long long)img->guest_memory, (unsigned long long)c.image, c.width, c.height,
                    (unsigned long long)c.covered, (unsigned long long)c.frame);
        });
    }
    for (auto& img : release) {
        g_ss.released++, g_ss.released_bytes += image_memory_size(img->memory, img->memory_handle);
        supersede_release(*img);
    }
}

void supersede_note_remade(const GpuImage& img) {
    g_ss.remade++;
    {
        std::lock_guard lk(g_sup_lock);
        g_remade[{img.guest_memory, img.width, img.height}]++;
    }
    static int logged = 0;
    if (logged++ < 20)
        log("nvn/vk: release replaced: image %llu (%ux%u) used again after its release: made again (begins cleared)",
            (unsigned long long)img.id, img.width, img.height);
}

void supersede_log() {
    if (!release_replaced_on()) return;
    log("nvn/vk: release replaced: %llu images released (%.0f MB), %llu made again; full clears %llu (%llu covering); "
        "kept at the last checks: %llu no covering clear, %llu used after it, %llu only partly covered, %llu under 1 MB, "
        "%llu used again after an earlier release, %llu used in the last %llu frames",
        (unsigned long long)g_ss.released, double(g_ss.released_bytes) / (1 << 20), (unsigned long long)g_ss.remade,
        (unsigned long long)g_ss.clears, (unsigned long long)g_ss.clears_covering, (unsigned long long)g_ss.kept_no_clear,
        (unsigned long long)g_ss.kept_used_after, (unsigned long long)g_ss.kept_partial,
        (unsigned long long)g_ss.kept_small, (unsigned long long)g_ss.kept_remade, (unsigned long long)g_ss.kept_recent,
        (unsigned long long)kIdleFrames);
    g_ss.kept_no_clear = g_ss.kept_used_after = g_ss.kept_partial = g_ss.kept_recent = g_ss.kept_small =
        g_ss.kept_remade = 0;
}

}  // namespace nx::nvn
