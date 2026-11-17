// A VkPipelineCache kept on disk (<data root>/pipelines_<vendor>_<device>.bin), and the cost of what the replay
// creates on the fly. Pipelines are compiled the first time a draw needs them, inside the replay: the course's
// stutters when new objects come in (16-42 ms replays on the RTX 3050). The driver checks the cache header itself
// (vendor, device, driver build): a stale file is ignored. Saved every 600 presents when it grew.
// NX_PIPELINE_CACHE=0: no cache (bisecting, or to measure first-run stutter).
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#ifdef _WIN32
#include <chrono>
#include <filesystem>
#include <future>
#include <windows.h>
#endif

#include <atomic>

#include <nx/game.h>

#include "collect.h"
#include "hitch.h"
#include "nvn_vk.h"
#include "platform.h"

namespace nx::nvn {

using namespace nx::vk;

namespace {

VkPipelineCache g_cache = VK_NULL_HANDLE;
bool g_tried = false;
size_t g_saved_size = 0;
std::string g_path;
uint64_t g_pipelines = 0, g_pipeline_ns = 0, g_astc = 0, g_astc_ns = 0;
// Images made in the window, and the distinct sizes of the render targets among them (960 wide or more, 16 at most):
// the game's dynamic resolution re-creates its render targets at every change of scale.
uint64_t g_images = 0, g_image_ns = 0;
uint32_t g_target_sizes = 0;
uint32_t g_target_seen[16] = {};

// The file: "NXVC", the data's size and its FNV-1a, then the driver's data. A file whose check fails (a disk error, a
// copy cut short) is not given to the driver: the cache starts empty. A file without the header (written before it
// had one) is the driver's data alone, as before; the driver checks its own header in any case.
constexpr uint32_t kCacheMagic = 0x4356584E;  // "NXVC"
struct CacheHeader {
    uint32_t magic = kCacheMagic, version = 1;
    uint64_t size = 0, check = 0;
};
uint64_t fnv1a(const uint8_t* p, size_t n) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 0x100000001b3ull;
    return h;
}

}  // namespace

size_t pipeline_cache_bytes() { return g_saved_size; }

VkPipelineCache pipeline_cache() {
    if (g_tried) return g_cache;
    g_tried = true;
    if (const char* e = std::getenv("NX_PIPELINE_CACHE"); e && e[0] == '0') return VK_NULL_HANDLE;
    VkPhysicalDeviceProperties p{};
    p_vkGetPhysicalDeviceProperties(vk_physical(), &p);
    char name[64];
    std::snprintf(name, sizeof name, "/pipelines_%04x_%04x.bin", p.vendorID, p.deviceID);
    g_path = plat::data_root() + name;
    std::vector<uint8_t> data;
    if (FILE* f = plat::fopen_utf8(g_path, "rb")) {
        plat::fseek64(f, 0, SEEK_END);
        int64_t n = plat::ftell64(f);
        plat::fseek64(f, 0, SEEK_SET);
        if (n > 0) data.resize(size_t(n)), data.resize(std::fread(data.data(), 1, data.size(), f));
        std::fclose(f);
    }
    if (data.size() >= sizeof(CacheHeader)) {
        CacheHeader h;
        std::memcpy(&h, data.data(), sizeof h);
        if (h.magic == kCacheMagic) {
            const bool ok = h.size == data.size() - sizeof h && fnv1a(data.data() + sizeof h, size_t(h.size)) == h.check;
            if (!ok) log("nvn/vk: pipeline cache %s is damaged: starting empty", g_path.c_str());
            data = ok ? std::vector<uint8_t>(data.begin() + sizeof h, data.end()) : std::vector<uint8_t>();
        }
    }
    // The driver's own header (VkPipelineCacheHeaderVersionOne): a cache made for another GPU or driver is ignored by
    // the driver anyway; dropping it here also keeps its size from counting as "already saved" (a preparation's stamp
    // needs a cache that holds this driver's pipelines, and the next save must not be skipped as "nothing new").
    if (data.size() >= 16 + VK_UUID_SIZE) {
        uint32_t w[4];
        std::memcpy(w, data.data(), sizeof w);
        const bool same = w[0] >= 16 + VK_UUID_SIZE && w[1] == VK_PIPELINE_CACHE_HEADER_VERSION_ONE &&
                          w[2] == p.vendorID && w[3] == p.deviceID &&
                          std::memcmp(data.data() + 16, p.pipelineCacheUUID, VK_UUID_SIZE) == 0;
        if (!same) {
            log("nvn/vk: pipeline cache %s was made for another GPU or driver: starting empty", g_path.c_str());
            data.clear();
        }
    } else if (!data.empty()) {
        log("nvn/vk: pipeline cache %s is too short: starting empty", g_path.c_str());
        data.clear();
    }
    VkPipelineCacheCreateInfo ci{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
    ci.initialDataSize = data.size();
    ci.pInitialData = data.empty() ? nullptr : data.data();
    if (p_vkCreatePipelineCache(vk_device(), &ci, nullptr, &g_cache) != VK_SUCCESS) {
        ci.initialDataSize = 0, ci.pInitialData = nullptr;  // a cache the driver refuses: start empty
        p_vkCreatePipelineCache(vk_device(), &ci, nullptr, &g_cache);
    }
    g_saved_size = data.size();
    log("nvn/vk: pipeline cache %s (%zu bytes loaded)", g_path.c_str(), data.size());
    return g_cache;
}

bool pipeline_cache_save() {
    if (!g_cache) return false;
    size_t n = 0;
    if (p_vkGetPipelineCacheData(vk_device(), g_cache, &n, nullptr) != VK_SUCCESS) return false;
    if (n <= g_saved_size) return true;  // nothing new since the file was read or written
    std::vector<uint8_t> data(n);
    if (p_vkGetPipelineCacheData(vk_device(), g_cache, &n, data.data()) != VK_SUCCESS) return false;
#ifdef _WIN32
    std::string tmp = g_path + "." + std::to_string(GetCurrentProcessId()) + ".tmp";
#else
    std::string tmp = g_path + ".tmp";
#endif
    if (FILE* f = plat::fopen_utf8(tmp, "wb")) {
        CacheHeader h;
        h.size = n, h.check = fnv1a(data.data(), n);
        bool ok = std::fwrite(&h, sizeof h, 1, f) == 1;
        ok = std::fwrite(data.data(), 1, n, f) == n && ok;
        ok = std::fclose(f) == 0 && ok;
#ifdef _WIN32
        const auto temp_path = std::filesystem::u8path(tmp);
        if (ok && MoveFileExW(temp_path.c_str(), std::filesystem::u8path(g_path).c_str(), MOVEFILE_REPLACE_EXISTING))
            g_saved_size = n;
        else
            DeleteFileW(temp_path.c_str());
#else
        std::remove(g_path.c_str());
        if (ok && std::rename(tmp.c_str(), g_path.c_str()) == 0) g_saved_size = n;
#endif
    }
    return g_saved_size == n;
}

// ---- host-only work and the game's GPU timestamps ----------------------------------------------------------------
//
// Wonder's Switch 2 build takes each frame's length as (the timestamp of the GPU report at the end of the frame's
// commands) - (the frame's start tick) (main+909d0, fw[0x130]): 15.83 ms or more and its governor shows the next frame
// for two vsyncs with a doubled time step, and its dynamic resolution lowers the scale. The replay writes those
// reports when it reaches them, so everything it did on the way counted as GPU time, including work the console's GPU
// never does: ASTC decoded on the CPU, textures copied to the GPU (the console's GPU reads them in place), Vulkan
// pipelines compiled, images created. A course streams textures in while the player runs: each batch (5-15 ms) made
// a doubled frame, and each change of scale re-created the render targets, which cost again (§5w). That time is kept
// out of the reports: a report carries (now - the host-only work of the frame being replayed). The frame itself is
// not faster; a present has about one display interval of slack before its display time, so work up to that long
// costs no displayed frame. NX_REPORT_HOST_WORK=1: reports carry the plain time, as before (comparison).
std::atomic<uint64_t> g_host_work{0}, g_host_work_total{0}, g_host_work_frames{0}, g_host_work_max{0};
bool host_work_hidden() {
    static const bool on = [] {
        const char* e = std::getenv("NX_REPORT_HOST_WORK");
        return !(e && e[0] == '1') && game::kInfo.tick_hz != 0;
    }();
    return on;
}
void note_host_work(uint64_t ns) { g_host_work.fetch_add(ns, std::memory_order_relaxed); }
uint64_t host_work_ns() { return host_work_hidden() ? g_host_work.load(std::memory_order_relaxed) : 0; }
void host_work_frame() {
    uint64_t ns = g_host_work.exchange(0, std::memory_order_relaxed);
    g_host_work_total += ns, g_host_work_frames += ns >= 1'000'000;
    if (ns > g_host_work_max) g_host_work_max = ns;
}

void note_pipeline_created(uint64_t ns) { note_host_work(ns), g_pipelines++, g_pipeline_ns += ns, hitch::add(hitch::kPipelines, 1), hitch::add(hitch::kPipelineNs, ns); }
void note_astc_decoded(uint64_t ns) {
    g_astc++, g_astc_ns += ns, hitch::add(hitch::kAstc, 1), hitch::add(hitch::kAstcNs, ns);
    if (ns >= 10'000'000) collect::event("texture", "ASTC decoded\t%.1f ms", double(ns) / 1e6);  // a costly one
}

void note_image_created(uint64_t ns, uint32_t width, uint32_t height, bool render_target) {
    note_host_work(ns), g_images++, g_image_ns += ns, hitch::add(hitch::kImages, 1), hitch::add(hitch::kImageNs, ns);
    if (ns >= 10'000'000)
        collect::event("texture", "image created\t%.1f ms\t%ux%u%s", double(ns) / 1e6, width, height,
                       render_target ? " render target" : "");
    if (!render_target || width < 960) return;  // the scene's targets, not its reduced copies (bloom, blur chains)
    for (uint32_t k = 0; k < g_target_sizes; k++)
        if (g_target_seen[k] == (width << 16 | height)) return;
    if (g_target_sizes < 16) g_target_seen[g_target_sizes++] = width << 16 | height;
}

// One line per timing window (nvn_vk.cpp log_timing): what the replay created, and the time it took.
void creation_log(uint64_t frames) {
    if (g_pipelines || g_astc || g_images) {
        char sizes[16 * 12 + 1] = "";
        for (uint32_t k = 0, at = 0; k < g_target_sizes; k++)
            at += uint32_t(std::snprintf(sizes + at, sizeof sizes - at, " %ux%u", g_target_seen[k] >> 16, g_target_seen[k] & 0xffff));
        log("nvn/vk: created over %llu frames: %llu pipelines (%.1f ms), %llu ASTC textures decoded (%.1f ms), %llu "
            "images (%.1f ms); render target sizes:%s",
            (unsigned long long)frames, (unsigned long long)g_pipelines, double(g_pipeline_ns) / 1e6,
            (unsigned long long)g_astc, double(g_astc_ns) / 1e6, (unsigned long long)g_images,
            double(g_image_ns) / 1e6, g_target_sizes ? sizes : " none");
    }
    if (uint64_t total = g_host_work_total.exchange(0))
        log("nvn/vk: host-only work over %llu frames: %.1f ms (%llu frames with 1 ms or more, the most %.1f ms)%s",
            (unsigned long long)frames, double(total) / 1e6, (unsigned long long)g_host_work_frames.exchange(0),
            double(g_host_work_max.exchange(0)) / 1e6, host_work_hidden() ? ", kept out of the GPU reports" : "");
    g_pipelines = g_pipeline_ns = g_astc = g_astc_ns = g_images = g_image_ns = 0;
    g_target_sizes = 0;
    static uint64_t windows = 0;
    if (++windows % 10 != 0) return;
#ifdef _WIN32
    // The device and its internally synchronized pipeline cache live for the whole process.
    // Reading the driver blob and replacing the cache file can take several display intervals.
    static std::future<bool> pending;
    if (g_cache && (!pending.valid() || pending.wait_for(std::chrono::seconds(0)) == std::future_status::ready)) {
        try {
            pending = std::async(std::launch::async, pipeline_cache_save);
        } catch (const std::system_error&) {
            // Retry at the next checkpoint if Windows cannot start a worker.
        }
    }
#else
    pipeline_cache_save();
#endif
}

}  // namespace nx::nvn
