// Device memory for the textures' images, in blocks (image_memory_alloc / image_memory_free).
//
// Each image used to get a vkAllocateMemory of its own. On Windows that call costs 0.3-1 ms for a render target, and
// Wonder's Switch 2 build changes its dynamic resolution in small steps, re-creating its render targets at every
// step: 5-8 images per frame, 4-7 ms of allocations per frame on the replay thread. That pushed the frame over the
// game's 15.8 ms limit, its governor doubled the frame and its resolution control answered with more changes of scale:
// stretches of 47-57 fps while running through a course (WONDER_SWITCH2_PROGRESS.md 5w). Here images take ranges of
// 256 MB blocks (first fit, free ranges merged); an image of 128 MB or more keeps an allocation of its own. A block
// left empty is freed when another empty one of its memory type already exists.
// NX_IMAGE_BLOCKS=0: one allocation per image, as before (comparison).
#include <cstdlib>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "nvn_vk.h"

namespace nx::nvn {

using namespace nx::vk;

namespace {

constexpr VkDeviceSize kBlockSize = 256ull << 20, kDedicatedFrom = 128ull << 20;

struct Block {
    VkDeviceMemory memory = VK_NULL_HANDLE;
    uint32_t type = 0;
    VkDeviceSize used = 0;
    std::map<VkDeviceSize, VkDeviceSize> free;  // offset -> size, merged with its neighbours
};
struct Range {
    Block* block;
    VkDeviceSize offset, size;
};

std::mutex g_lock;
std::vector<std::unique_ptr<Block>> g_blocks;
std::unordered_map<uint64_t, Range> g_ranges;  // by handle
uint64_t g_next_handle = 0;
uint64_t g_block_allocs = 0, g_dedicated = 0;

bool blocks_on() {
    static const bool on = [] {
        const char* e = std::getenv("NX_IMAGE_BLOCKS");
        return !(e && e[0] == '0');
    }();
    return on;
}

bool take(Block& b, VkDeviceSize size, VkDeviceSize align, VkDeviceSize& offset) {
    for (auto it = b.free.begin(); it != b.free.end(); ++it) {
        VkDeviceSize at = (it->first + align - 1) / align * align;
        VkDeviceSize pad = at - it->first;
        if (it->second < pad + size) continue;
        VkDeviceSize begin = it->first, len = it->second;
        b.free.erase(it);
        if (pad) b.free[begin] = pad;
        if (len > pad + size) b.free[at + size] = len - pad - size;
        b.used += size;
        offset = at;
        return true;
    }
    return false;
}

void give(Block& b, VkDeviceSize offset, VkDeviceSize size) {
    auto next = b.free.lower_bound(offset);
    if (next != b.free.begin()) {
        auto prev = std::prev(next);
        if (prev->first + prev->second == offset) offset = prev->first, size += prev->second, b.free.erase(prev);
    }
    if (next != b.free.end() && offset + size == next->first) size += next->second, b.free.erase(next);
    b.free[offset] = size;
}

}  // namespace

bool image_memory_alloc(const VkMemoryRequirements& req, ImageMemory& out) {
    out = {};
    uint32_t type = memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (blocks_on() && req.size < kDedicatedFrom) {
        std::lock_guard lk(g_lock);
        VkDeviceSize align = req.alignment ? req.alignment : 1, offset = 0;
        Block* found = nullptr;
        for (auto& b : g_blocks)
            if (b->type == type && take(*b, req.size, align, offset)) {
                found = b.get();
                break;
            }
        if (!found) {
            auto b = std::make_unique<Block>();
            VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
            ai.allocationSize = kBlockSize;
            ai.memoryTypeIndex = type;
            if (p_vkAllocateMemory(vk_device(), &ai, nullptr, &b->memory) == VK_SUCCESS) {
                b->type = type;
                b->free[0] = kBlockSize;
                take(*b, req.size, align, offset);
                found = b.get();
                g_blocks.push_back(std::move(b));
                if (++g_block_allocs % 8 == 1)
                    log("nvn/vk: image memory: %zu blocks of %llu MB (%llu images with an allocation of their own)",
                        g_blocks.size(), (unsigned long long)(kBlockSize >> 20), (unsigned long long)g_dedicated);
            }
        }
        if (found) {
            out.memory = found->memory, out.offset = offset, out.handle = ++g_next_handle;
            g_ranges[out.handle] = {found, offset, req.size};
            return true;
        }
    }
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = type;
    g_dedicated++;
    VkResult r = p_vkAllocateMemory(vk_device(), &ai, nullptr, &out.memory);
    if (r != VK_SUCCESS) log("nvn/vk: vkAllocateMemory of %llu MB for an image failed (%d)", (unsigned long long)(req.size >> 20), int(r));
    return r == VK_SUCCESS;
}

void image_memory_free(VkDeviceMemory memory, uint64_t handle) {
    if (!handle) {
        if (memory) p_vkFreeMemory(vk_device(), memory, nullptr);
        return;
    }
    std::lock_guard lk(g_lock);
    auto it = g_ranges.find(handle);
    if (it == g_ranges.end()) return;
    Block* b = it->second.block;
    give(*b, it->second.offset, it->second.size);
    b->used -= it->second.size;
    g_ranges.erase(it);
    if (b->used) return;
    // An empty block goes when another empty one of its type remains (one is kept for the next change of scale).
    for (auto& o : g_blocks)
        if (o.get() != b && o->type == b->type && !o->used) {
            p_vkFreeMemory(vk_device(), b->memory, nullptr);
            for (auto k = g_blocks.begin(); k != g_blocks.end(); ++k)
                if (k->get() == b) {
                    g_blocks.erase(k);
                    break;
                }
            return;
        }
}

}  // namespace nx::nvn
