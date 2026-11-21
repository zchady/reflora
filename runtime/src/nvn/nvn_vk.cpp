// NVN's GPU side on Vulkan (docs/NVN.md, N3): the host device, a VkImage per NVN texture the GPU touches, the
// replay of recorded command lists, and presenting a window texture on the host swapchain.
//
// First version: every submit is recorded into one Vulkan command buffer and waited for (NX_PIPELINED, NX_ASYNC_GPU:
// into a slot's, not waited for, nvn_frames.cpp); image layouts are tracked per image, in submission order. The image
// behind a texture is nvn_image.cpp. On macOS this is MoltenVK: the instance enumerates portability drivers and the
// device enables VK_KHR_portability_subset.
#if !defined(_WIN32)
#include <unistd.h>  // _exit (stdlib.h on Windows)
#endif

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

#include <deque>
#include <condition_variable>
#include <thread>
#include <atomic>
#include "hitch.h"
#include "nvn_astc_bc7.h"
#include "nvn_vk.h"
#include "hooks.h"
#include "platform.h"

#include <nx/game.h>

namespace nx::nvn {

using namespace nx::vk;


namespace {

std::recursive_mutex g_gpu;  // the whole GPU side: one submitting thread at a time
bool g_ready = false, g_failed = false;
VkInstance g_instance = VK_NULL_HANDLE;
VkPhysicalDevice g_physical = VK_NULL_HANDLE;
VkDevice g_device = VK_NULL_HANDLE;
VkQueue g_queue = VK_NULL_HANDLE;
uint32_t g_family = 0;
VkPhysicalDeviceMemoryProperties g_memory{};
VkCommandPool g_pool = VK_NULL_HANDLE;
VkCommandBuffer g_cb = VK_NULL_HANDLE;
VkFence g_fence = VK_NULL_HANDLE;

VkSurfaceKHR g_surface = VK_NULL_HANDLE;
VkSwapchainKHR g_swapchain = VK_NULL_HANDLE;
std::vector<VkImage> g_sc_images;
VkExtent2D g_sc_extent{};
VkExtent2D g_sc_current{};  // the surface's currentExtent when the swapchain was made (SUBOPTIMAL: did it change?)
VkSurfaceTransformFlagBitsKHR g_sc_transform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
VkFormat g_sc_format = VK_FORMAT_B8G8R8A8_UNORM;
VkSemaphore g_acquired = VK_NULL_HANDLE, g_rendered = VK_NULL_HANDLE;
std::mutex g_queue_lock;  // queue_lock() (nvn_vk.h)

// Device memory accounting (vram_log): what this process holds, by heap, from the wrappers below; with
// VK_EXT_memory_budget, what the driver says the process uses of each heap and may use. Allocations are rare (image
// blocks of 256 MB, staging and upload buffers), so a lock and a map cost nothing measurable.
bool g_memory_budget = false;
std::mutex g_mem_lock;
std::unordered_map<VkDeviceMemory, std::pair<VkDeviceSize, uint32_t>> g_mem_live;  // size, heap
VkDeviceSize g_mem_heap[VK_MAX_MEMORY_HEAPS] = {};
uint64_t g_mem_allocs = 0, g_mem_frees = 0, g_mem_failed = 0;
PFN_vkAllocateMemory g_real_allocate = nullptr;
PFN_vkFreeMemory g_real_free = nullptr;

VKAPI_ATTR VkResult VKAPI_CALL counted_allocate(VkDevice d, const VkMemoryAllocateInfo* ai, const VkAllocationCallbacks* cb,
                                                VkDeviceMemory* out) {
    VkResult r = g_real_allocate(d, ai, cb, out);
    std::lock_guard lk(g_mem_lock);
    if (r != VK_SUCCESS || !out || !*out) {
        g_mem_failed++;
        return r;
    }
    uint32_t heap = ai->memoryTypeIndex < g_memory.memoryTypeCount ? g_memory.memoryTypes[ai->memoryTypeIndex].heapIndex : 0;
    g_mem_live[*out] = {ai->allocationSize, heap};
    g_mem_heap[heap] += ai->allocationSize;
    g_mem_allocs++;
    return r;
}

VKAPI_ATTR void VKAPI_CALL counted_free(VkDevice d, VkDeviceMemory m, const VkAllocationCallbacks* cb) {
    if (m) {
        std::lock_guard lk(g_mem_lock);
        if (auto it = g_mem_live.find(m); it != g_mem_live.end()) {
            g_mem_heap[it->second.second] -= it->second.first;
            g_mem_live.erase(it);
            g_mem_frees++;
        }
    }
    g_real_free(d, m, cb);
}

// NX_VRAM_LOG=0|1: the accounting and its log. On by default on Windows (a lock per allocation, two log lines every
// 30 s); off elsewhere, where it would also enable VK_EXT_memory_budget on the device.
// NX_VRAM_LOG=2 adds the census of who holds each image and how long ago it was sampled: a walk of every texture
// object (~37,000 on Wonder's world map, 6-7 ms on the present thread every 30 s), so only when asked for.
int vram_log_level() {
    static const int level = [] {
        const char* e = std::getenv("NX_VRAM_LOG");
        if (e && *e) return std::atoi(e);
#if defined(_WIN32)
        return 1;
#else
        return 0;
#endif
    }();
    return level;
}
bool vram_log_on() { return vram_log_level() > 0; }

void count_device_memory() {  // the eviction's measure too (NX_VRAM_EVICT)
    if (!(vram_log_on() || evict_on()) || g_real_allocate || !p_vkAllocateMemory || !p_vkFreeMemory) return;
    g_real_allocate = p_vkAllocateMemory, g_real_free = p_vkFreeMemory;
    p_vkAllocateMemory = counted_allocate, p_vkFreeMemory = counted_free;
}
const char* g_present_mode_name = "";  // the swapchain's present mode (the overlay)
VkBool32 g_depth_clip_control = VK_FALSE;
VkBool32 g_fragment_interlock = VK_FALSE;
VkPhysicalDeviceFeatures g_features{};  // the core features enabled (vk_features())
VkDescriptorPool g_descriptors = VK_NULL_HANDLE;

// The per-submit upload buffer (nvn_vk.h): one host-visible buffer, bump-allocated, reset at every submit.
VkBuffer g_upload = VK_NULL_HANDLE;
VkDeviceMemory g_upload_memory = VK_NULL_HANDLE;
uint8_t* g_upload_ptr = nullptr;
uint64_t g_upload_size = 0, g_upload_used = 0;
void make_upload_buffer(uint64_t size);

// Commands the Hades bridge intercepts are not in the generated table: ours are loaded here.
PFN_vkEnumerateInstanceExtensionProperties p_enum_instance_ext = nullptr;
PFN_vkCreateInstance p_create_instance = nullptr;
PFN_vkEnumeratePhysicalDevices p_enum_physical = nullptr;
PFN_vkCreateDevice p_create_device = nullptr;

}  // namespace

// A lost device means the GPU hung (a shader looping for ever): the process ends at once. Submitting more work, or
// waiting, keeps the GPU stuck, and on macOS that froze WindowServer and restarted the machine.
[[noreturn]] void gpu_hung(const char* why) {
    log("nvn/vk: GPU hang (%s): stopping", why);
    std::fprintf(stderr, "nvn/vk: GPU hang (%s): stopping\n", why);
    _exit(4);
}

namespace {

bool check(VkResult r, const char* what) {
    if (r >= 0) return true;
    log("nvn/vk: %s failed (%d)", what, int(r));
    if (r == VK_ERROR_DEVICE_LOST) gpu_hung(what);
    return false;
}

bool has_extension(const std::vector<VkExtensionProperties>& list, const char* name) {
    for (auto& e : list)
        if (std::strcmp(e.extensionName, name) == 0) return true;
    return false;
}

void* g_gipa = nullptr;  // vkGetInstanceProcAddr, for surfaces made later (make_surface)
bool make_surface();

bool init() {
    if (g_ready || g_failed) return g_ready;
    g_failed = true;  // until everything below succeeds
    auto gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(plat::vulkan_get_instance_proc_addr());
    if (!gipa) {
        log("nvn/vk: no Vulkan loader");
        return false;
    }
    for (size_t k = 0; k < g_entry_count; k++) *g_entries[k].pfn = gipa(nullptr, g_entries[k].name);
    p_enum_instance_ext = reinterpret_cast<PFN_vkEnumerateInstanceExtensionProperties>(
        gipa(nullptr, "vkEnumerateInstanceExtensionProperties"));
    p_create_instance = reinterpret_cast<PFN_vkCreateInstance>(gipa(nullptr, "vkCreateInstance"));

    uint32_t n = 0;
    p_enum_instance_ext(nullptr, &n, nullptr);
    std::vector<VkExtensionProperties> iext(n);
    p_enum_instance_ext(nullptr, &n, iext.data());
    std::vector<const char*> exts = {VK_KHR_SURFACE_EXTENSION_NAME, plat::vulkan_surface_extension()};
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    if (has_extension(iext, "VK_KHR_portability_enumeration")) {
        exts.push_back("VK_KHR_portability_enumeration");
        ici.flags |= 0x00000001;  // VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR
    }
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = nx::game::kInfo.title;
    app.apiVersion = VK_API_VERSION_1_3;
    ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = uint32_t(exts.size());
    ici.ppEnabledExtensionNames = exts.data();
    if (!check(p_create_instance(&ici, nullptr, &g_instance), "vkCreateInstance")) return false;
    for (size_t k = 0; k < g_entry_count; k++)
        if (auto p = gipa(g_instance, g_entries[k].name)) *g_entries[k].pfn = p;
    p_enum_physical = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(gipa(g_instance, "vkEnumeratePhysicalDevices"));
    p_create_device = reinterpret_cast<PFN_vkCreateDevice>(gipa(g_instance, "vkCreateDevice"));

    n = 0;
    p_enum_physical(g_instance, &n, nullptr);
    std::vector<VkPhysicalDevice> gpus(n);
    p_enum_physical(g_instance, &n, gpus.data());
    gpus.resize(n);
    g_physical = pick_physical_device(gpus);  // NX_GPU, default: the first discrete GPU (nvn_gpu_pick.cpp)
    if (!g_physical) {
        log("nvn/vk: no GPU");
        return false;
    }
    VkPhysicalDeviceProperties props{};
    p_vkGetPhysicalDeviceProperties(g_physical, &props);
    p_vkGetPhysicalDeviceMemoryProperties(g_physical, &g_memory);

    g_gipa = reinterpret_cast<void*>(gipa);
    if (!make_surface()) return false;

    p_vkGetPhysicalDeviceQueueFamilyProperties(g_physical, &n, nullptr);
    std::vector<VkQueueFamilyProperties> families(n);
    p_vkGetPhysicalDeviceQueueFamilyProperties(g_physical, &n, families.data());
    g_family = UINT32_MAX;
    for (uint32_t k = 0; k < n; k++) {
        VkBool32 present = VK_FALSE;
        p_vkGetPhysicalDeviceSurfaceSupportKHR(g_physical, k, g_surface, &present);
        if ((families[k].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present) {
            g_family = k;
            break;
        }
    }
    if (g_family == UINT32_MAX) {
        log("nvn/vk: no graphics + present queue");
        return false;
    }

    p_vkEnumerateDeviceExtensionProperties(g_physical, nullptr, &n, nullptr);
    std::vector<VkExtensionProperties> dext(n);
    p_vkEnumerateDeviceExtensionProperties(g_physical, nullptr, &n, dext.data());
    std::vector<const char*> dexts = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    // Features: everything core the GPU has, dynamic rendering (1.3), 8-bit indices (NVN has them), clip-space
    // depth -1..1 (NVN's default depth mode, what Wonder uses), and on MoltenVK the portability subset's own
    // features, which must be enabled to be used.
    VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    // The Vulkan 1.1 and 1.2 features too (all the GPU has): the translated vertex shaders declare DrawParameters,
    // which needs shaderDrawParameters enabled; AMD's driver accepted them without it, a strict driver need not.
    VkPhysicalDeviceVulkan11Features f11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
    VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    // Every translated fragment shader declares FragmentShaderPixelInterlockEXT (unused: no interlock instruction in
    // any of the 17,245): enabled where the GPU has it, stripped from the modules where it does not (Mali, 5zd16).
    VkPhysicalDeviceFragmentShaderInterlockFeaturesEXT fint{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADER_INTERLOCK_FEATURES_EXT};
    VkPhysicalDeviceIndexTypeUint8FeaturesEXT fu8{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_INDEX_TYPE_UINT8_FEATURES_EXT};
    VkPhysicalDeviceDepthClipControlFeaturesEXT fclip{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_CONTROL_FEATURES_EXT};
    struct {  // VkPhysicalDevicePortabilitySubsetFeaturesKHR, without its header (beta extensions)
        VkStructureType sType = VkStructureType(1000163000);
        void* pNext = nullptr;
        VkBool32 fields[15] = {};
    } fport;
    features.pNext = &f11;
    f11.pNext = &f12;
    f12.pNext = &f13;
    void** tail = &f13.pNext;
    if (has_extension(dext, "VK_EXT_fragment_shader_interlock")) {
        dexts.push_back("VK_EXT_fragment_shader_interlock");
        *tail = &fint, tail = &fint.pNext;
    }
    if (has_extension(dext, "VK_EXT_index_type_uint8")) {
        dexts.push_back("VK_EXT_index_type_uint8");
        *tail = &fu8, tail = &fu8.pNext;
    }
    if (has_extension(dext, "VK_EXT_depth_clip_control")) {
        dexts.push_back("VK_EXT_depth_clip_control");
        *tail = &fclip, tail = &fclip.pNext;
    }
    if (has_extension(dext, "VK_KHR_portability_subset")) {
        dexts.push_back("VK_KHR_portability_subset");
        *tail = &fport, tail = &fport.pNext;
    }
    if ((vram_log_on() || evict_on()) && has_extension(dext, "VK_EXT_memory_budget")) {  // budget and usage per heap
        dexts.push_back("VK_EXT_memory_budget");
        g_memory_budget = true;
    }
    p_vkGetPhysicalDeviceFeatures2(g_physical, &features);
    g_depth_clip_control = fclip.depthClipControl;
    g_fragment_interlock = fint.fragmentShaderPixelInterlock;
    f11.protectedMemory = VK_FALSE;  // only for protected queues, which are not made
    g_features = features.features;
    log("nvn/vk: shaderDrawParameters %u, fragment shader pixel interlock %u (extension %u), shaderClipDistance %u, "
        "shaderCullDistance %u", f11.shaderDrawParameters, g_fragment_interlock,
        has_extension(dext, "VK_EXT_fragment_shader_interlock") ? 1u : 0u, features.features.shaderClipDistance,
        features.features.shaderCullDistance);
    // NX_ASTC_DECODE=1|linear|srgb: the CPU decoder even where the GPU has ASTC (to compare the two on one GPU).
    set_astc_native(g_features.textureCompressionASTC_LDR);
    log("nvn/vk: ASTC %s", astc_native() ? "native" : "decoded to RGBA8 at upload (no textureCompressionASTC_LDR)");
    astc_bc7_init(g_features.textureCompressionBC);  // NX_ASTC_BC7 (nvn_astc_bc7.cpp): off unless asked for
    log("nvn/vk: depthClamp %u, depthBiasClamp %u, depthBounds %u, depth clip control %u (extension %u)",
        g_features.depthClamp, g_features.depthBiasClamp, g_features.depthBounds, g_depth_clip_control,
        has_extension(dext, "VK_EXT_depth_clip_control") ? 1u : 0u);
    log("nvn/vk: portability: format reinterpretation %u, format swizzle %u", fport.fields[2], fport.fields[3]);
    log_device_caps(g_physical, dext);  // nvn_caps.cpp
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = g_family;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = uint32_t(dexts.size());
    dci.ppEnabledExtensionNames = dexts.data();
    dci.pNext = &features;
    if (!check(p_create_device(g_physical, &dci, nullptr, &g_device), "vkCreateDevice")) return false;
    count_device_memory();  // vram_log: every vkAllocateMemory / vkFreeMemory from here on, by heap
    p_vkGetDeviceQueue(g_device, g_family, 0, &g_queue);

    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = g_family;
    p_vkCreateCommandPool(g_device, &pci, nullptr, &g_pool);
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = g_pool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    p_vkAllocateCommandBuffers(g_device, &cai, &g_cb);
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    p_vkCreateFence(g_device, &fci, nullptr, &g_fence);
    VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    p_vkCreateSemaphore(g_device, &sci, nullptr, &g_acquired);
    p_vkCreateSemaphore(g_device, &sci, nullptr, &g_rendered);

    // A busy submit reached 67 MB (uniforms padded to 4 KB, storage to 64 KB). Android: 128 MB, half, for an 8 GB
    // phone (docs/nvn/android-perf/MEMORY.md: the phone never filled it; big uploads have staging buffers, #72).
#if defined(__ANDROID__)
    make_upload_buffer(128ull << 20);
#else
    make_upload_buffer(256ull << 20);
#endif
    if (async_gpu())
        frame_init(g_pool, g_family, g_upload, g_upload_ptr, g_upload_size);  // the upload buffer: the slots' ring
    else
        g_descriptors = make_descriptor_pool();
    log("nvn/vk: %s, Vulkan %u.%u, queue family %u", props.deviceName, VK_API_VERSION_MAJOR(props.apiVersion),
        VK_API_VERSION_MINOR(props.apiVersion), g_family);
    g_failed = false;
    g_ready = true;
    catalog_prewarm_start();  // NX_PIPELINE_PREWARM: the catalog's pipelines, before the game asks for them
    return true;
}

// The window's surface, and the swapchain format from what the surface offers: B8G8R8A8_UNORM (the Mac), else
// R8G8B8A8_UNORM (Android surfaces offer no BGRA: a swapchain made with it showed nothing on a Galaxy S23), else the
// first. A new Android window (the app back from home or the lock screen) needs a new surface (gpu_present).
void* g_window = nullptr;
uint64_t g_window_gen = 0;
bool make_surface() {
    if (g_surface) p_vkDestroySurfaceKHR(g_instance, g_surface, nullptr);
    if (g_window) plat::release_window(g_window);
    g_surface = VK_NULL_HANDLE;
    g_window = plat::create_window();  // waits for a window (Android); the generation is read once it is there
    g_window_gen = plat::window_generation();
    uint64_t surface = 0;
    if (!check(VkResult(plat::vulkan_create_surface(g_instance, g_window, &surface, g_gipa)), "surface")) return false;
    g_surface = reinterpret_cast<VkSurfaceKHR>(surface);
    uint32_t n = 0;
    p_vkGetPhysicalDeviceSurfaceFormatsKHR(g_physical, g_surface, &n, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(n);
    p_vkGetPhysicalDeviceSurfaceFormatsKHR(g_physical, g_surface, &n, formats.data());
    auto offered = [&](VkFormat f) {
        for (const VkSurfaceFormatKHR& x : formats)
            if (x.format == f) return true;
        return false;
    };
    g_sc_format = offered(VK_FORMAT_B8G8R8A8_UNORM)   ? VK_FORMAT_B8G8R8A8_UNORM
                  : offered(VK_FORMAT_R8G8B8A8_UNORM) ? VK_FORMAT_R8G8B8A8_UNORM
                  : n                                  ? formats[0].format
                                                       : VK_FORMAT_B8G8R8A8_UNORM;
    log("nvn/vk: surface: %u formats offered, swapchain format %d", n, int(g_sc_format));
    return true;
}

// ---- presents held until their display time ----------------------------------------------------------------------
//
// The display model (nvn_queue.cpp) gives each present the time the console's display would show it, D(p), on a
// 60 Hz grid; the game runs up to two frames ahead of it (three window textures). Presented at once, a frame reached
// the screen whenever the game happened to present it: at 180 Hz, 10% of the frames stayed 1-2 or 4-5 refreshes
// instead of 3 (§5t). Here the frame is copied to its swapchain image at once (the fence is waited, so the window
// texture is free again, as the game expects), and only vkQueuePresentKHR waits on a presenter thread until D(p), as
// the console's display queue would show it. The present then needs no semaphore (its copy has finished). On for
// Switch 2 games with the synchronous path (the display model's); NX_PRESENT_AT_DISPLAY=0: presented at once, =1 on.
bool present_at_display() {
    static const bool on = [] {
        const char* e = std::getenv("NX_PRESENT_AT_DISPLAY");
        return e ? e[0] == '1' : nx::game::kInfo.tick_hz != 0;
    }();
    return on;
}

struct PendingPresent {
    uint32_t index;
    uint64_t at;
};
std::mutex g_pend_lock;
std::condition_variable g_pend_cv;
std::deque<PendingPresent> g_pending;
std::atomic<int> g_present_result{VK_SUCCESS};  // a deferred present's OUT_OF_DATE / SUBOPTIMAL, for gpu_present

void queue_present_now(uint32_t index, VkSemaphore wait) {  // with g_queue_lock held
    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    if (wait) pi.waitSemaphoreCount = 1, pi.pWaitSemaphores = &wait;
    pi.swapchainCount = 1;
    pi.pSwapchains = &g_swapchain;
    pi.pImageIndices = &index;
    VkResult r = p_vkQueuePresentKHR(g_queue, &pi);
    present_log_host(2);
    plat::frame_presented();  // cores, CPU time, present intervals (frame_stats.cpp)
    if (r != VK_SUCCESS) g_present_result = r;
}

void presenter_thread() {
    plat::set_thread_name("nvn present");
    for (;;) {
        uint64_t at;
        {
            std::unique_lock lk(g_pend_lock);
            g_pend_cv.wait(lk, [] { return !g_pending.empty(); });
            at = g_pending.front().at;
        }
        uint64_t now = plat::monotonic_ns();
        if (at > now) plat::sleep_ns(int64_t(std::min<uint64_t>(at - now, 100'000'000ull)));
        std::lock_guard qk(g_queue_lock);  // the queue, then the list (as flush_presents)
        std::unique_lock lk(g_pend_lock);
        if (g_pending.empty() || g_pending.front().at > plat::monotonic_ns()) continue;  // flushed, or not yet due
        uint32_t index = g_pending.front().index;
        g_pending.pop_front();
        lk.unlock();
        queue_present_now(index, VK_NULL_HANDLE);
    }
}

void defer_present(uint32_t index, uint64_t at) {
    static std::once_flag once;
    std::call_once(once, [] { std::thread(presenter_thread).detach(); });
    {
        std::lock_guard lk(g_pend_lock);
        g_pending.push_back({index, at});
    }
    g_pend_cv.notify_one();
}

// Presents the oldest deferred frames at once: all of them (before the swapchain changes), or one (an image is needed).
void flush_presents(bool all = true) {
    std::lock_guard qk(g_queue_lock);
    std::lock_guard lk(g_pend_lock);
    while (!g_pending.empty()) {
        queue_present_now(g_pending.front().index, VK_NULL_HANDLE);
        g_pending.pop_front();
        if (!all) break;
    }
}

bool presents_pending() {
    std::lock_guard lk(g_pend_lock);
    return !g_pending.empty();
}

// vkAcquireNextImageKHR without blocking under the queue lock: a deferred present may be what frees an image.
VkResult acquire_image(VkSemaphore acquired, uint32_t& index) {
    uint64_t t0 = plat::monotonic_ns();
    for (;;) {
        VkResult r;
        {
            std::lock_guard qk(g_queue_lock);
            r = p_vkAcquireNextImageKHR(g_device, g_swapchain, 0, acquired, VK_NULL_HANDLE, &index);
        }
        if (r != VK_NOT_READY && r != VK_TIMEOUT) return r;
        if (presents_pending())
            flush_presents(false);
        else if (plat::monotonic_ns() - t0 > 2'000'000'000ull)
            return r;
        else
            plat::sleep_ns(200'000);
    }
}

bool make_swapchain() {
    if (g_swapchain) {
        flush_presents();  // the deferred ones still hold images of this swapchain
        if (async_gpu()) frame_wait_all();
        {
            std::lock_guard qk(g_queue_lock);
            p_vkDeviceWaitIdle(g_device);
        }
        present_pass_release();
        p_vkDestroySwapchainKHR(g_device, g_swapchain, nullptr);
        g_swapchain = VK_NULL_HANDLE;
    }
    VkSurfaceCapabilitiesKHR caps{};
    p_vkGetPhysicalDeviceSurfaceCapabilitiesKHR(g_physical, g_surface, &caps);
    VkExtent2D extent = caps.currentExtent;
    if (extent.width == UINT32_MAX) {
        uint32_t w = 0, h = 0;
        plat::window_size(w, h);
        extent = {w ? w : 1280, h ? h : 720};
    }
    if (!extent.width || !extent.height) return false;  // minimized
    VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    ci.surface = g_surface;
    // Deferred presents (present_at_display) hold up to two images until their display time: two more.
    ci.minImageCount = std::max(caps.minImageCount, 3u) + (present_at_display() ? 2u : 0u);
    if (caps.maxImageCount) ci.minImageCount = std::min(ci.minImageCount, caps.maxImageCount);
    ci.imageFormat = g_sc_format;
    ci.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    ci.imageExtent = extent;
    ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.preTransform = caps.currentTransform;
    // The present pass (nvn_present.cpp) draws the picture pre-rotated: the images are in the display's own
    // orientation (portrait on a phone held sideways). Without it the images stay upright (the Mac: identity).
    bool sideways = caps.currentTransform & (VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR | VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR);
    bool pass = present_pass_wanted() && !present_pass_failed();
    g_sc_current = caps.currentExtent;
    if (pass && sideways && extent.width > extent.height) std::swap(extent.width, extent.height);
    if (!pass && (caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR))
        ci.preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    // Some drivers report maxImageExtent in the other orientation: the larger side bounds both.
    uint32_t most = std::max(caps.maxImageExtent.width, caps.maxImageExtent.height);
    if (most) extent.width = std::min(extent.width, most), extent.height = std::min(extent.height, most);
    ci.imageExtent = extent;
    ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    if (!(caps.supportedCompositeAlpha & ci.compositeAlpha))
        for (VkCompositeAlphaFlagBitsKHR a : {VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR, VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
                                              VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR})
            if (caps.supportedCompositeAlpha & a) {
                ci.compositeAlpha = a;
                break;
            }
    log("nvn/vk: surface %ux%u (current %ux%u), transform %u of %#x, composite alpha %#x of %#x, usage %#x",
        extent.width, extent.height, caps.currentExtent.width, caps.currentExtent.height, unsigned(caps.currentTransform),
        caps.supportedTransforms, unsigned(ci.compositeAlpha), caps.supportedCompositeAlpha,
        caps.supportedUsageFlags);
    // The game paces itself on its vsync event (hle_vi.cpp, 60 Hz): a FIFO present also waited for the display's own
    // refresh, two clocks drifting against each other (stutter near 60 fps, docs/nvn/DEBUG.md #33). MAILBOX never
    // blocks and shows the newest frame at each refresh; else IMMEDIATE (MoltenVK: no display sync in the Metal
    // layer, the macOS compositor still shows whole frames at its refresh), FIFO last. NX_FIFO=1: FIFO (DEBUG.md #62).
    ci.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    uint32_t modes_n = 0;
    p_vkGetPhysicalDeviceSurfacePresentModesKHR(g_physical, g_surface, &modes_n, nullptr);
    std::vector<VkPresentModeKHR> modes(modes_n);
    p_vkGetPhysicalDeviceSurfacePresentModesKHR(g_physical, g_surface, &modes_n, modes.data());
    auto has = [&](VkPresentModeKHR m) { return std::find(modes.begin(), modes.end(), m) != modes.end(); };
    if (!std::getenv("NX_FIFO")) {
        if (has(VK_PRESENT_MODE_MAILBOX_KHR))
            ci.presentMode = VK_PRESENT_MODE_MAILBOX_KHR;
        else if (has(VK_PRESENT_MODE_IMMEDIATE_KHR)) {
#if defined(_WIN32)
            // Switch 2's display presenter already schedules the frame. Keep FIFO as the tear-free
            // fallback on Windows when the driver exposes IMMEDIATE but no MAILBOX.
            if (!nx::game::kInfo.tick_hz)
#endif
                ci.presentMode = VK_PRESENT_MODE_IMMEDIATE_KHR;
        }
    }
    static const char* const mode_names[] = {"IMMEDIATE", "MAILBOX", "FIFO", "FIFO_RELAXED"};
    log("nvn/vk: present mode %s (%u modes offered)", ci.presentMode <= 3 ? mode_names[ci.presentMode] : "?", modes_n);
    g_present_mode_name = ci.presentMode <= 3 ? mode_names[ci.presentMode] : "?";
    ci.clipped = VK_TRUE;
    if (!check(p_vkCreateSwapchainKHR(g_device, &ci, nullptr, &g_swapchain), "vkCreateSwapchainKHR")) return false;
    uint32_t n = 0;
    p_vkGetSwapchainImagesKHR(g_device, g_swapchain, &n, nullptr);
    g_sc_images.resize(n);
    p_vkGetSwapchainImagesKHR(g_device, g_swapchain, &n, g_sc_images.data());
    g_sc_extent = extent;
    g_sc_transform = ci.preTransform;
    if (n > 8) log("nvn/vk: %u swapchain images: more than the 8 present semaphores (nvn_frames.cpp)", n);
    bool drawn = pass && present_pass_targets(g_sc_format, g_sc_images, extent, ci.preTransform);
    if (pass && !drawn && present_pass_failed()) {  // the pass is out: an upright swapchain for the blit
        log("nvn/vk: present pass failed: swapchain made again for the blit");
        return make_swapchain();
    }
    log("nvn/vk: swapchain %ux%u, %u images, transform %u, %s", extent.width, extent.height, n,
        unsigned(ci.preTransform), drawn ? "drawn by the present pass" : "blitted");
    return true;
}

}  // namespace

std::recursive_mutex& gpu_lock() { return g_gpu; }

std::mutex& queue_lock() { return g_queue_lock; }
VkResult queue_submit(const VkSubmitInfo* si, VkFence fence) {
    std::lock_guard lk(g_queue_lock);
    return p_vkQueueSubmit(g_queue, 1, si, fence);
}
VkDevice vk_device() { return g_device; }
VkInstance vk_instance() { return g_instance; }
VkPhysicalDevice vk_physical() { return g_physical; }
VkQueue vk_queue() { return g_queue; }
uint32_t vk_queue_family() { return g_family; }
bool vk_depth_clip_control() { return g_depth_clip_control; }
bool vk_fragment_interlock() { return g_fragment_interlock; }
const VkPhysicalDeviceFeatures& vk_features() { return g_features; }

uint32_t memory_type(uint32_t bits, VkMemoryPropertyFlags want) {
    for (uint32_t k = 0; k < g_memory.memoryTypeCount; k++)
        if ((bits >> k) & 1 && (g_memory.memoryTypes[k].propertyFlags & want) == want) return k;
    return 0;
}

Upload upload_alloc(uint64_t size, uint64_t align) {
    if (async_gpu()) return frame_upload_alloc(size, align);
    uint64_t at = (g_upload_used + align - 1) & ~(align - 1);
    if (at + size > g_upload_size) {
        log("nvn/vk: upload buffer full (%llu + %llu bytes)", (unsigned long long)at, (unsigned long long)size);
        at = 0;  // wraps: this submit's earlier data may be overwritten (grow the buffer when this shows)
        replay::dedup_reset();  // shared copies may be among them
    }
    g_upload_used = at + size;
    return {g_upload, at, g_upload_ptr + at};
}

Upload upload(const void* data, uint64_t size, uint64_t align) {
    Upload u = upload_alloc(size, align);
    if (data && size) std::memcpy(u.ptr, data, size);
    return u;
}

VkDescriptorSet descriptor_set(VkDescriptorSetLayout layout) {
    if (async_gpu()) return frame_descriptor_set(layout);
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = g_descriptors;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &layout;
    VkDescriptorSet set = VK_NULL_HANDLE;
    if (p_vkAllocateDescriptorSets(g_device, &ai, &set) != VK_SUCCESS) log("nvn/vk: descriptor pool exhausted");
    return set;
}

VkDescriptorPool make_descriptor_pool() {
    VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 65536}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 16384},
                                    {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 65536}, {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 4096},
                                    {VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 1024}, {VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, 1024},
                                    {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 65536}};
    VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpi.maxSets = 32768;
    dpi.poolSizeCount = uint32_t(sizeof sizes / sizeof sizes[0]);
    dpi.pPoolSizes = sizes;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    p_vkCreateDescriptorPool(g_device, &dpi, nullptr, &pool);
    return pool;
}

namespace {

void make_upload_buffer(uint64_t size) {
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = size;
    bi.usage = kUploadUsage;
    p_vkCreateBuffer(g_device, &bi, nullptr, &g_upload);
    VkMemoryRequirements req{};
    p_vkGetBufferMemoryRequirements(g_device, g_upload, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = upload_memory_type(req.memoryTypeBits);  // nvn_caps.cpp (NX_RING_CACHED)
    p_vkAllocateMemory(g_device, &ai, nullptr, &g_upload_memory);
    p_vkBindBufferMemory(g_device, g_upload, g_upload_memory, 0);
    void* p = nullptr;
    p_vkMapMemory(g_device, g_upload_memory, 0, VK_WHOLE_SIZE, 0, &p);
    g_upload_ptr = static_cast<uint8_t*>(p);
    g_upload_size = size;
}

bool begin(bool replay = false) {
    if (async_gpu()) return (g_cb = frame_begin(replay)) != VK_NULL_HANDLE;
    p_vkResetCommandBuffer(g_cb, 0);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    return check(p_vkBeginCommandBuffer(g_cb, &bi), "vkBeginCommandBuffer");
}

// Where the submitting thread's time goes, logged with the draw stats, per presented frame: the replay (recording
// into Vulkan), vkQueueSubmit (Metal encoding happens inside it), fences waited for, the rest of gpu_submit (object
// drain, image frees, write-backs), and gpu_present as a whole; bytes through the upload buffer.
struct Timing {
    uint64_t submits = 0, replay_ns = 0, encode_ns = 0, fence_ns = 0, submit_ns = 0, present_ns = 0, upload_bytes = 0;
    // The submitting thread's own CPU time over the replay, and over the whole gpu_submit (not on Windows: 0). Next
    // to the wall-clock replay it shows whether the replay is working or waiting (descheduled, page faults, locks).
    uint64_t replay_cpu_ns = 0, submit_cpu_ns = 0;
} g_timing;

uint64_t thread_cpu_now() {
    uint64_t cpu = 0;
    long faults[2], switches[2];
    plat::thread_counters(cpu, faults, switches);
    return cpu;
}

void end_and_wait(VkSemaphore wait, VkSemaphore signal) {
    uint64_t t0 = plat::monotonic_ns();
    p_vkEndCommandBuffer(g_cb);
    VkPipelineStageFlags stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &g_cb;
    if (wait) {
        si.waitSemaphoreCount = 1;
        si.pWaitSemaphores = &wait;
        si.pWaitDstStageMask = &stage;
    }
    if (signal) {
        si.signalSemaphoreCount = 1;
        si.pSignalSemaphores = &signal;
    }
    p_vkResetFences(g_device, 1, &g_fence);
    bool queued = check(queue_submit(&si, g_fence), "vkQueueSubmit");
    uint64_t t1 = plat::monotonic_ns();
    // 2 s at most: a frame's work takes milliseconds; longer is a shader that does not end.
    if (queued) {
        VkResult w = p_vkWaitForFences(g_device, 1, &g_fence, VK_TRUE, 2'000'000'000ull);
        if (w == VK_TIMEOUT) gpu_hung("a submit did not finish in 2 s");
        check(w, "vkWaitForFences");
    }
    frame_times().encode_ns += t1 - t0, frame_times().fence_ns += plat::monotonic_ns() - t1;
}

// The overlay's performance page (hooks.h): the last 60-frame window of log_timing and the last presented image.
std::mutex g_perf_lock;
hooks::PerfSnapshot g_perf;

void log_timing(uint64_t frames) {
    auto ms = [&](uint64_t ns) { return double(ns) / 1e6 / double(frames); };
    static uint64_t last = 0;  // wall clock: the real frame rate
    uint64_t now = plat::monotonic_ns();
    double fps = last ? double(frames) * 1e9 / double(now - last) : 0.0;
    last = now;
    const Timing& t = g_timing;
    uint64_t known = t.replay_ns + t.encode_ns + t.fence_ns;
    log("nvn/vk: per frame over %llu: %.1f fps, %.1f submits, replay %.2f ms, submit (encode) %.2f ms, fence wait "
        "%.2f ms, other %.2f ms, present %.2f ms, upload %.2f MB; on a CPU: replay %.2f ms, whole submits %.2f ms",
        (unsigned long long)frames, fps, double(t.submits) / double(frames), ms(t.replay_ns), ms(t.encode_ns),
        ms(t.fence_ns), ms(t.submit_ns > known ? t.submit_ns - known : 0), ms(t.present_ns),
        double(t.upload_bytes) / 1e6 / double(frames), ms(t.replay_cpu_ns), ms(t.submit_cpu_ns));
    {
        std::lock_guard pk(g_perf_lock);
        g_perf.fps = fps, g_perf.replay_ms = ms(t.replay_ns), g_perf.encode_ms = ms(t.encode_ns);
        g_perf.fence_ms = ms(t.fence_ns), g_perf.present_ms = ms(t.present_ns);
        g_perf.upload_mb = double(t.upload_bytes) / 1e6 / double(frames);
        g_perf.submits = double(t.submits) / double(frames);
    }
    frame_log(frames);
    creation_log(frames);  // pipelines and ASTC decodes of the window; saves the pipeline cache now and then
    g_timing = {};
}

// gpu_present's time, whatever way it returns.
struct PresentTimer {
    uint64_t t0 = plat::monotonic_ns();
    ~PresentTimer() { g_timing.present_ns += plat::monotonic_ns() - t0; }
};

}  // namespace

uint64_t draw_take_dump_request();          // nvn_draw.cpp
void watched_blocks_check(const char* where);  // nvn_draw.cpp (NX_TRACE_SLOT_WIDTH)

namespace {
std::shared_ptr<const DeferredValues> g_deferred;  // the submit being replayed (replay thread only)
}
const DeferredValues* current_deferred() { return g_deferred.get(); }

void gpu_submit(const std::vector<std::shared_ptr<CmdList>>& lists, std::shared_ptr<const DeferredValues> deferred) {
    g_deferred = std::move(deferred);
    std::lock_guard lk(g_gpu);
    uint64_t t_in = plat::monotonic_ns(), cpu_in = thread_cpu_now();
    const FrameTimes f_in = frame_times();
    uint64_t lists_epoch = 0;
    for (auto& l : lists) lists_epoch = std::max(lists_epoch, l->epoch);
    ReplayScope replay(lists_epoch);  // the texture mark (#55, #57), then drain_objects (under the GPU lock, #50)
    if (!init()) return;
    bool async = async_gpu();
    if (!async) {  // the previous submit was waited for: all of it is free
        free_retired_images(UINT64_MAX);
        p_vkResetDescriptorPool(g_device, g_descriptors, 0);
        g_upload_used = 0;
    } else if (pipelined()) {  // what the retired submits were the last to use (they free nothing themselves)
        free_retired_images(frame_completed());
    }
    if (!begin(true)) return;
    draw_begin_submit();
    gpu_profile_begin(g_cb);
    zero_draws_begin_submit(g_cb);
    // Pipelined: the part up to the last dispatch is queued as soon as it is recorded (nvn_frames.cpp). Not with the
    // profile or the zero-draw queries: they span the submit, which is then waited for whole.
    size_t split_after = gpu_profile_on() || zero_draws_on() ? SIZE_MAX : frame_plan_split(lists);
    uint64_t t0 = plat::monotonic_ns(), cpu0 = thread_cpu_now();
    const FrameTimes f0 = frame_times();
    Targets st;
    for (size_t k = 0; k < lists.size(); k++) {
        replay_list(g_cb, st, *lists[k], 0);
        // After the last list with a dispatch, unless nothing follows it (replay_list may have split inside a list).
        g_cb = k == split_after && k + 1 < lists.size() ? frame_split(g_cb) : frame_cb(g_cb);
    }
    draw_end_rendering(g_cb);
    const FrameTimes f1 = frame_times();  // the first part's submit and ring waits are not replay time
    g_timing.submits++;
    g_timing.replay_ns += plat::monotonic_ns() - t0 - (f1.encode_ns - f0.encode_ns) - (f1.fence_ns - f0.fence_ns);
    g_timing.replay_cpu_ns += thread_cpu_now() - cpu0;  // includes the first part's vkQueueSubmit when split
    g_timing.upload_bytes += async ? frame_upload_used() : g_upload_used;
    hitch::add(hitch::kUploadBytes, async ? frame_upload_used() : g_upload_used);
    gpu_profile_mark(g_cb, "end");
    if (async)  // the profile, zero-draw queries and snapshots read this submit's results right below: waited for
        frame_submit(VK_NULL_HANDLE, VK_NULL_HANDLE, draw_take_write_backs(),
                     gpu_profile_on() || zero_draws_on() || debug_snapshots_pending());
    else
        end_and_wait(VK_NULL_HANDLE, VK_NULL_HANDLE);
    draw_end_submit();  // not waited for: snapshots only (the write-backs went with the submit)
    gpu_profile_collect(), zero_draws_collect();
    debug_dump_requested(draw_take_dump_request());  // reads back in a submit of its own, which it waits for
    watched_blocks_check("after a submit");
    const FrameTimes f2 = frame_times();
    g_timing.encode_ns += f2.encode_ns - f_in.encode_ns, g_timing.fence_ns += f2.fence_ns - f_in.fence_ns;
    g_timing.submit_ns += plat::monotonic_ns() - t_in;
    g_timing.submit_cpu_ns += thread_cpu_now() - cpu_in;
}

// Headless: the presented texture is only counted, logged and dumped (no swapchain). Nothing is submitted unless a
// frame dump is due: the empty submit was a Metal command buffer and a fence wait per frame.
void present_headless(uint64_t texture) {
    GpuImage* src = image_of(texture);  // made here if new, as before (the targets rendered into it share it, #44)
    static uint64_t presents = 0;
    presents++;
    if (presents % (presents < 300 ? 60 : 300) == 0) draw_log_stats();
    if (presents % 60 == 0) log_timing(60), gpu_profile_log(60);
    if (presents % 300 == 0) zero_draws_log();
    if (!frame_dump_due(presents)) return;
    if (src && src->layout != VK_IMAGE_LAYOUT_UNDEFINED && begin()) {
        GpuImage* shown = src;  // NX_PRESENT_TEST: what the present pass would put on a display (nvn_present.cpp)
        if (GpuImage* test = present_pass_test_target(); test && present_pass_record(g_cb, *src, nullptr, 0))
            test->layout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, shown = test;
        transition(g_cb, *shown, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        bool dump = frame_dump_record(g_cb, *shown);
        if (async_gpu())
            frame_submit(VK_NULL_HANDLE, VK_NULL_HANDLE, {}, dump);  // a dumped frame is waited for
        else
            end_and_wait(VK_NULL_HANDLE, VK_NULL_HANDLE);
        if (dump) frame_dump_write(*shown, presents);
    }
    debug_dump_textures(presents);
}

// SUBOPTIMAL re-makes the swapchain only when the surface size changed: Android returns it for every present when the
// swapchain's transform is not the display's (a Galaxy S23 did for Hades), a rebuild per frame.
bool present_needs_new_swapchain(VkResult r) {
    if (r == VK_ERROR_OUT_OF_DATE_KHR) return true;
    if (r != VK_SUBOPTIMAL_KHR) return false;
    VkSurfaceCapabilitiesKHR caps{};  // compared with what the swapchain was made from (its extent may be turned)
    p_vkGetPhysicalDeviceSurfaceCapabilitiesKHR(g_physical, g_surface, &caps);
    return caps.currentExtent.width != g_sc_current.width || caps.currentExtent.height != g_sc_current.height ||
           (present_pass_wanted() && !present_pass_failed() && caps.currentTransform != g_sc_transform);
}

bool overlay_pass_record(VkCommandBuffer cb, GpuImage& dst, uint32_t index, const std::vector<VkImage>& images,
                         VkExtent2D extent);  // nvn_present.cpp

void gpu_present(uint64_t texture, const int32_t* crop, uint64_t show_at) {
    watched_blocks_check("at a present");
    plat::perf_hint_thread();  // the thread that presents takes part in the frame (perf_hint.cpp)
    std::lock_guard lk(g_gpu);
    PresentTimer timer;
    present_log_host(0);
    struct LogEnd {  // a frame that does not reach vkQueuePresentKHR (headless, minimized, no swapchain)
        bool queued = false;
        ~LogEnd() { if (!queued) present_log_host(3); }
    } log_end;
    drain_objects();  // under the GPU lock, as in gpu_submit
    host_work_frame();  // the next frame's reports start from no host-only work
    if (!init()) return;
    if (!async_gpu() || pipelined()) free_retired_images(async_gpu() ? frame_completed() : UINT64_MAX);
    if (evict_on()) vram_evict_tick();  // NX_VRAM_EVICT (experimental): between submits, under the GPU lock
    if (release_replaced_on()) supersede_tick();  // NX_RELEASE_REPLACED (nvn_supersede.cpp): the same place
    if (plat::headless()) return present_headless(texture);
    if (plat::window_minimized()) return;
    if (plat::window_generation() != g_window_gen) {  // Android: a new window after home or the lock screen
        flush_presents();
        if (async_gpu()) frame_wait_all();
        {
            std::lock_guard qk(g_queue_lock);
            p_vkDeviceWaitIdle(g_device);
        }
        present_pass_release();
        if (g_swapchain) p_vkDestroySwapchainKHR(g_device, g_swapchain, nullptr);
        g_swapchain = VK_NULL_HANDLE;
        log("nvn/vk: new window: surface and swapchain re-made");
        if (!make_surface()) return;
    }
    if (VkResult last = VkResult(g_present_result.exchange(VK_SUCCESS)); last != VK_SUCCESS && g_swapchain)
        if (present_needs_new_swapchain(last)) make_swapchain();  // a deferred present said so
    if (!g_swapchain && !make_swapchain()) return;
    uint32_t index = 0;
    VkSemaphore acquired = async_gpu() ? frame_acquire_semaphore() : g_acquired;
    VkResult r = acquire_image(acquired, index);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) {
        make_swapchain();
        return;
    }
    if (!check(r, "vkAcquireNextImageKHR") || !begin()) return;
    GpuImage* src = image_of(texture);
    GpuImage dst;
    dst.image = g_sc_images[index];
    dst.format = g_sc_format;
    if (src) {
        if (src->layout == VK_IMAGE_LAYOUT_UNDEFINED) {  // never rendered: dark grey, to tell it from a failure
            transition(g_cb, *src, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            VkClearColorValue v{{0.08f, 0.08f, 0.1f, 1.0f}};
            VkImageSubresourceRange rr{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            p_vkCmdClearColorImage(g_cb, src->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &v, 1, &rr);
        }
    }
    if (src) {
        std::lock_guard pk(g_perf_lock);
        const bool cropped = crop && crop[2] > 0 && crop[3] > 0;
        g_perf.game_w = cropped ? uint32_t(crop[2]) : src->width, g_perf.game_h = cropped ? uint32_t(crop[3]) : src->height;
        g_perf.present_mode = g_present_mode_name;
    }
    bool drawn = src && present_pass_record(g_cb, *src, crop, index);  // nvn_present.cpp: leaves dst PRESENT_SRC
    if (src && !drawn) transition(g_cb, *src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    if (!drawn) transition(g_cb, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    if (src && !drawn) {
        VkImageBlit blit{};
        blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        blit.srcOffsets[1] = {int32_t(src->width), int32_t(src->height), 1};
        if (crop && crop[2] > 0 && crop[3] > 0 && crop[0] + crop[2] <= int32_t(src->width) &&
            crop[1] + crop[3] <= int32_t(src->height))  // nvnWindowSetCrop
            blit.srcOffsets[0] = {crop[0], crop[1], 0}, blit.srcOffsets[1] = {crop[0] + crop[2], crop[1] + crop[3], 1};
        blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        blit.dstOffsets[1] = {int32_t(g_sc_extent.width), int32_t(g_sc_extent.height), 1};
        p_vkCmdBlitImage(g_cb, src->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst.image,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
    }
    // The settings overlay over the blitted image, when it draws something (hooks.h); else as before.
    if (!drawn && !(hooks::overlay_wanted && hooks::overlay_wanted() &&
                    overlay_pass_record(g_cb, dst, index, g_sc_images, g_sc_extent)))
        transition(g_cb, dst, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    static uint64_t presents = 0;
    presents++;
    if (presents % (presents < 300 ? 60 : 300) == 0) draw_log_stats();
    if (presents % 60 == 0) log_timing(60), gpu_profile_log(60);
    if (presents % 300 == 0) zero_draws_log();
    frame_sig_flush(presents);
    const bool due = frame_dump_due(presents);
    bool dump = src && due;
    if (dump) transition(g_cb, *src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);  // the present pass left it sampled
    dump = dump && frame_dump_record(g_cb, *src);
    // Held until its display time (present_at_display): the copy is waited for, the present needs no semaphore.
    const bool defer = show_at && present_at_display() && !async_gpu() && show_at > plat::monotonic_ns() + 500'000;
    // One "rendered" semaphore per swapchain image: a single one, signaled again while the last present may still
    // wait on it, broke VUID-vkQueueSubmit-pSignalSemaphores-00067 (validation, run 130). Image i's is signaled again
    // only once image i was acquired again, so its last present has consumed it.
    VkSemaphore rendered = defer ? VK_NULL_HANDLE : frame_rendered_semaphore(index);
    if (async_gpu())  // the blit's fence is not waited for (vkQueuePresentKHR waits on `rendered`); a dumped frame is
        frame_submit(acquired, rendered, {}, dump);
    else
        end_and_wait(acquired, rendered);
    present_log_host(1);
    if (dump) frame_dump_write(*src, presents);
    if (due) debug_dump_textures(presents);
    log_end.queued = true;  // the present log's row is completed by the present itself
    if (defer) {
        defer_present(index, show_at);
        return;
    }
    flush_presents();  // presents stay in order (a frame shown at once after deferred ones: the path changed)
    {
        std::lock_guard qk(g_queue_lock);
        queue_present_now(index, rendered);
    }
    if (VkResult last = VkResult(g_present_result.exchange(VK_SUCCESS)); last != VK_SUCCESS)
        if (present_needs_new_swapchain(last)) make_swapchain();
}

uint64_t device_memory_held() {
    std::lock_guard lk(g_mem_lock);
    uint64_t sum = 0;
    for (uint32_t h = 0; h < g_memory.memoryHeapCount; h++)
        if (g_memory.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) sum += g_mem_heap[h];
    return sum;
}

uint64_t vram_driver_budget() {
    if (!g_memory_budget || !p_vkGetPhysicalDeviceMemoryProperties2) return 0;
    VkPhysicalDeviceMemoryBudgetPropertiesEXT budget{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT};
    VkPhysicalDeviceMemoryProperties2 props{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2};
    props.pNext = &budget;
    p_vkGetPhysicalDeviceMemoryProperties2(g_physical, &props);
    uint64_t best = 0, size = 0;  // the largest device-local heap (the discrete GPU's VRAM)
    for (uint32_t h = 0; h < props.memoryProperties.memoryHeapCount; h++)
        if ((props.memoryProperties.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) &&
            props.memoryProperties.memoryHeaps[h].size > size)
            size = props.memoryProperties.memoryHeaps[h].size, best = budget.heapBudget[h];
    return best;
}

void vram_log() {
    static uint64_t next = 0;
    const uint64_t now = plat::monotonic_ns();
    if (now < next || !g_real_allocate) return;
    next = now + 30'000'000'000ull;
    constexpr double kMB = 1.0 / (1 << 20);
    std::string held;
    uint64_t live = 0, allocs = 0, frees = 0, failed = 0;
    {
        std::lock_guard lk(g_mem_lock);
        live = g_mem_live.size(), allocs = g_mem_allocs, frees = g_mem_frees, failed = g_mem_failed;
        for (uint32_t h = 0; h < g_memory.memoryHeapCount; h++) {
            char b[96];
            std::snprintf(b, sizeof b, "%sheap %u%s %.0f MB", h ? ", " : "", h,
                          (g_memory.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) ? " (device)" : "",
                          double(g_mem_heap[h]) * kMB);
            held += b;
        }
    }
    // Allocated is not in use: a 256 MB block counts whole as long as one image sits in it.
    const ImageMemoryStats im = image_memory_stats();
    log("nvn/vk: vram: %llu allocations live (%llu made, %llu freed, %llu failed); allocated by heap: %s; image blocks: "
        "%llu allocated (%.0f MB), %.0f MB in use by %llu images, %.0f MB free inside them, %llu idle (empty); %llu images "
        "with their own allocation (%.0f MB)",
        (unsigned long long)live, (unsigned long long)allocs, (unsigned long long)frees, (unsigned long long)failed,
        held.c_str(), (unsigned long long)im.blocks, double(im.block_bytes) * kMB, double(im.used_bytes) * kMB,
        (unsigned long long)im.block_images, double(im.block_bytes - im.used_bytes) * kMB,
        (unsigned long long)im.idle_blocks, (unsigned long long)im.own_images, double(im.own_bytes) * kMB);
    // Who holds the live images: textures the game still has (guest keys), pool snapshots (0xE... keys, nvn_objects.cpp)
    // and kept textures (0xF...). An image only snapshots or kept textures hold is one the game has let go of. Skipped
    // when the object table is busy (a shared try-lock: never waits on the replay).
    //
    // Safe to read: this runs in gpu_present under the GPU lock, which every replay-side write of Texture::image
    // (image_of, in gpu_submit / gpu_present) also holds; game-thread writes (snapshots, destruction) take the object
    // table exclusively. The census holds the table shared only while it walks it; sizes are read after.
    const uint64_t census_t0 = plat::monotonic_ns();
    if (vram_log_level() < 2) {
        // the census is NX_VRAM_LOG=2's
    } else if (objects_lock().try_lock_shared()) {
        struct Seen {
            uint32_t who = 0;  // bit 0 game texture, 1 snapshot, 2 kept texture
            uint64_t sampled = 0;  // the latest draw that sampled it, of any holder
        };
        std::unordered_map<const GpuImage*, Seen> seen;
        uint64_t textures = 0;
        for (auto& [key, obj] : objects()) {
            auto* t = dynamic_cast<Texture*>(obj.get());
            if (!t) continue;
            textures++;
            if (!t->image) continue;
            const uint64_t top = key >> 60;
            Seen& s = seen[t->image.get()];
            s.who |= top == 0xE ? 2u : top == 0xF ? 4u : 1u;
            s.sampled = std::max<uint64_t>(s.sampled, t->last_sampled.load(std::memory_order_relaxed));
        }
        objects_lock().unlock_shared();
        const uint64_t walk_ns = plat::monotonic_ns() - census_t0;
        uint64_t n[8] = {}, bytes[8] = {};
        // By kind and by the time since their last sampling draw (up to 10 s, 10-60 s, 1-5 min, older or never).
        // Rebuildable: only ever uploaded from guest memory. GPU-written (`rendered`) ones hold results nothing else has.
        uint64_t age_n[2][4] = {}, age_bytes[2][4] = {};
        for (auto& [img, s] : seen) {
            const uint64_t size = image_memory_size(img->memory, img->memory_handle);
            n[s.who]++, bytes[s.who] += size;
            const uint64_t age = s.sampled && now > s.sampled ? now - s.sampled : UINT64_MAX;
            const int a = age <= 10'000'000'000ull ? 0 : age <= 60'000'000'000ull ? 1 : age <= 300'000'000'000ull ? 2 : 3;
            const int k = img->rendered ? 1 : 0;
            age_n[k][a]++, age_bytes[k][a] += size;
        }
        for (int k = 0; k < 2; k++)
            log("nvn/vk: vram %s by last sampling: <=10 s %llu (%.0f MB), 10-60 s %llu (%.0f MB), 1-5 min %llu (%.0f MB), "
                "older or never %llu (%.0f MB)",
                k ? "GPU-written images" : "uploaded images", (unsigned long long)age_n[k][0], double(age_bytes[k][0]) * kMB,
                (unsigned long long)age_n[k][1], double(age_bytes[k][1]) * kMB, (unsigned long long)age_n[k][2],
                double(age_bytes[k][2]) * kMB, (unsigned long long)age_n[k][3], double(age_bytes[k][3]) * kMB);
        log("nvn/vk: vram holders: %llu texture objects; images held by game textures only %llu (%.0f MB), snapshots "
            "only %llu (%.0f MB), kept textures only %llu (%.0f MB), game textures and copies %llu (%.0f MB), copies of "
            "both kinds %llu (%.0f MB); census %.2f ms (table held %.2f ms)",
            (unsigned long long)textures, (unsigned long long)n[1], double(bytes[1]) * kMB, (unsigned long long)n[2],
            double(bytes[2]) * kMB, (unsigned long long)n[4], double(bytes[4]) * kMB,
            (unsigned long long)(n[3] + n[5] + n[7]), double(bytes[3] + bytes[5] + bytes[7]) * kMB,
            (unsigned long long)n[6], double(bytes[6]) * kMB, double(plat::monotonic_ns() - census_t0) / 1e6,
            double(walk_ns) / 1e6);
    } else {
        log("nvn/vk: vram holders: skipped (object table busy)");
    }
    supersede_log();  // NX_RELEASE_REPLACED (nvn_supersede.cpp): only when on
    if (!g_memory_budget || !p_vkGetPhysicalDeviceMemoryProperties2) return;
    VkPhysicalDeviceMemoryBudgetPropertiesEXT budget{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT};
    VkPhysicalDeviceMemoryProperties2 props{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2};
    props.pNext = &budget;
    p_vkGetPhysicalDeviceMemoryProperties2(g_physical, &props);
    std::string line;
    for (uint32_t h = 0; h < props.memoryProperties.memoryHeapCount; h++) {
        char b[128];
        std::snprintf(b, sizeof b, "%sheap %u: usage %.0f of budget %.0f MB (size %.0f MB)", h ? "; " : "", h,
                      double(budget.heapUsage[h]) * kMB, double(budget.heapBudget[h]) * kMB,
                      double(props.memoryProperties.memoryHeaps[h].size) * kMB);
        line += b;
    }
    log("nvn/vk: vram budget (driver): %s", line.c_str());
}

}  // namespace nx::nvn

namespace nx::hooks {
PerfSnapshot perf_snapshot() {
    std::lock_guard pk(nvn::g_perf_lock);
    return nvn::g_perf;
}
}  // namespace nx::hooks
