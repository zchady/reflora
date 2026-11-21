// Pipelines and sampled resources (nvn_pipeline.h). Everything is cached for the life of the process: layouts by
// their bindings, pipelines by their full state, samplers by their state, views by (image, type).
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <unordered_map>

#include "../collect.h"
#include "../hitch.h"
#include "nvn_astc_bc7.h"
#include "nvn_pipeline.h"
#include "platform.h"

namespace nx::nvn {

using namespace nx::vk;

// NVN views carry a format: the game renders into RGBA8 textures through sRGB views (the GPU encodes on write and
// blends in linear) and samples them the same way. Ignoring it lost one conversion per draw: far too much contrast
// and saturation (docs/nvn/DEBUG.md #29).
VkFormat view_format(const GpuImage& img, uint32_t nvn) {
    if (VkFormat f; bc_view_format(img, nvn, f)) return f;  // decoded BC (nvn_bc.cpp)
    if (img.astc_bc7) {  // NX_ASTC_BC7: a view as an ASTC format keeps its sRGB-ness; any other is not served as BC7
        if (!nvn || nvn == img.nvn_format) return img.format;
        if (is_astc(nvn)) return nvn >= 0x87 ? VK_FORMAT_BC7_SRGB_BLOCK : VK_FORMAT_BC7_UNORM_BLOCK;
        img.bc7_remake_why = "view";  // this use as it is; RGBA8 before the next one (ensure_contents)
        return img.format;
    }
    if (!nvn || nvn == img.nvn_format || img.alpha_from_r || img.aspect != VK_IMAGE_ASPECT_COLOR_BIT) return img.format;
    VkFormat f = vk_format(nvn);
    uint32_t bw = 1, bh = 1, bytes = 0, iw = 1, ih = 1, ibytes = 0;
    format_block(nvn, bw, bh, bytes);
    format_block(img.nvn_format, iw, ih, ibytes);
    // The sizes of the host formats: R11G11B10F is 4 bytes in NVN but RGBA16F here, and a 4-byte view (RGBA8, R32F)
    // of an 8-byte image is no valid view (G4, docs/nvn/DEBUG.md #50). NX_VIEW_NVN_SIZES=1: the NVN sizes (bisecting).
    static const bool nvn_sizes = std::getenv("NX_VIEW_NVN_SIZES") != nullptr;
    if (!nvn_sizes && vk_block_bytes(f) && vk_block_bytes(img.format))
        bytes = vk_block_bytes(f), ibytes = vk_block_bytes(img.format);
    bool ok = f != VK_FORMAT_UNDEFINED && format_aspect(nvn) == 0 && bw == iw && bh == ih && bytes == ibytes;
    if (ok && f != img.format && !img.mutable_format) {  // ubwc_flags: an image made without MUTABLE_FORMAT
        static std::set<std::pair<uint32_t, uint32_t>> not_mutable;
        if (not_mutable.insert({img.nvn_format, nvn}).second)
            log("nvn/vk: texture format %#x viewed as %#x, but the image is not mutable: its own format", img.nvn_format,
                nvn);
        return img.format;
    }
    static std::set<std::pair<uint32_t, uint32_t>> logged;
    if (logged.insert({img.nvn_format, nvn}).second && logged.size() <= 40)
        log("nvn/vk: texture format %#x viewed as %#x%s", img.nvn_format, nvn, ok ? "" : " (not compatible: ignored)");
    return ok ? f : img.format;
}

VkImageViewType view_type(const ImageShape& s) {  // SPIR-V Dim: 0 1D, 1 2D, 2 3D, 3 Cube
    switch (s.dim) {
        case 0: return s.arrayed ? VK_IMAGE_VIEW_TYPE_1D_ARRAY : VK_IMAGE_VIEW_TYPE_1D;
        case 2: return VK_IMAGE_VIEW_TYPE_3D;
        case 3: return s.arrayed ? VK_IMAGE_VIEW_TYPE_CUBE_ARRAY : VK_IMAGE_VIEW_TYPE_CUBE;
        default: return s.arrayed ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
    }
}

namespace {

VkShaderStageFlags stage_bit(ShaderStage s) {
    return s == ShaderStage::Vertex     ? VK_SHADER_STAGE_VERTEX_BIT
           : s == ShaderStage::Fragment ? VK_SHADER_STAGE_FRAGMENT_BIT
                                        : VK_SHADER_STAGE_COMPUTE_BIT;
}

// NX_SPIRV_SALT=<text> (measurement only): every module gets an OpSourceExtension "<text>", a debug instruction with
// no effect on the code, so the driver sees modules it has never compiled: its own shader cache (outside the
// runtime, e.g. the AMD driver's) cannot hide what a first run costs. A new text per run gives a cold driver each
// time, without deleting any cache. Inserted where the debug section begins (after the entry points and modes).
void salt_module(std::vector<uint32_t>& code) {
    static const char* salt = std::getenv("NX_SPIRV_SALT");
    if (!salt || !*salt || code.size() < 5) return;
    size_t at = 5;
    while (at < code.size()) {
        uint32_t op = code[at] & 0xFFFF, len = code[at] >> 16;
        // OpCapability 17, OpExtension 10, OpExtInstImport 11, OpMemoryModel 14, OpEntryPoint 15,
        // OpExecutionMode 16, OpExecutionModeId 331
        if (!len || (op != 17 && op != 10 && op != 11 && op != 14 && op != 15 && op != 16 && op != 331)) break;
        at += len;
    }
    std::string text(salt);
    std::vector<uint32_t> words((text.size() + 4) / 4, 0);  // nul-terminated, padded
    std::memcpy(words.data(), text.data(), text.size());
    std::vector<uint32_t> ins{uint32_t(1 + words.size()) << 16 | 4};  // OpSourceExtension
    ins.insert(ins.end(), words.begin(), words.end());
    code.insert(code.begin() + std::ptrdiff_t(at), ins.begin(), ins.end());
}

// The caches of shader modules, descriptor-set layouts and pipeline layouts (and describe_graphics_pipeline's
// trace), shared by the replay (under the GPU lock) and the pipeline catalog's workers (not under it: they must not
// hold up a submit). Held only while one of those is looked up, or a layout made: shader modules, the slow part, are
// made outside it (module_of, §5zd9).
std::recursive_mutex& pipeline_objects_lock() {
    static std::recursive_mutex m;
    return m;
}

// The lock taken with its waits measured (WONDER_SWITCH2_PROGRESS.md §5zd9): a wait by any other thread than a
// catalog worker (the replay, mostly) is a hitch cause (hitch.h kObjectsWaitNs) and is counted for the catalog's
// summary; a worker's own waits are added up per thread for its breakdown.
thread_local bool t_catalog_worker = false;
thread_local uint64_t t_objects_wait_ns = 0;
std::atomic<uint64_t> g_objects_waits{0}, g_objects_wait_ns{0}, g_objects_wait_max{0};

struct PipelineObjectsLock {
    std::unique_lock<std::recursive_mutex> lk{pipeline_objects_lock(), std::try_to_lock};
    PipelineObjectsLock() {
        if (lk.owns_lock()) return;
        const uint64_t t0 = plat::monotonic_ns();
        lk.lock();
        const uint64_t ns = plat::monotonic_ns() - t0;
        t_objects_wait_ns += ns;
        if (t_catalog_worker) return;
        hitch::add(hitch::kObjectsWaitNs, ns);
        g_objects_waits++;
        g_objects_wait_ns += ns;
        for (uint64_t m = g_objects_wait_max.load(); ns > m && !g_objects_wait_max.compare_exchange_weak(m, ns);) {}
    }
};

// `kinds`: per vertex input location, 0 float or the integer kind of its attribute format (vertex_int_kind).
// `unwritten`: fragment input locations the vertex stage does not write (default_unwritten_inputs).
VkShaderModule module_of(const ShaderInfo* s, const uint8_t* kinds = nullptr, uint32_t unwritten = 0) {
    static std::map<std::tuple<const ShaderInfo*, std::string, uint32_t>, VkShaderModule> cache;  // under the lock
    // The integer-vertex-input patch (skinned models' bone indices; without it Metal refuses every character's
    // pipeline, DEBUG.md #25/#26). NX_INT_ATTRIBS=0 turns it off.
    static const char* int_env = std::getenv("NX_INT_ATTRIBS");
    static const bool off = int_env && int_env[0] == '0';
    std::string k = kinds && !off ? std::string(reinterpret_cast<const char*>(kinds), 16) : std::string(16, '\0');
    {
        PipelineObjectsLock lk;
        if (auto it = cache.find({s, k, unwritten}); it != cache.end()) return it->second;
    }
    // Made outside the lock (§5zd9): patching the SPIR-V and creating the module are the slow part, and with them
    // under the lock the catalog's workers took turns and the replay waited behind them (frames of 25-45 ms while
    // the catalog was being made). Two threads may make the same module at once: the first one kept wins.
    bool is_patched = false;
    const uint8_t* int_kinds = k != std::string(16, '\0') ? reinterpret_cast<const uint8_t*>(k.data()) : nullptr;
    std::vector<uint32_t> code = patched_spirv(s, int_kinds, unwritten, &is_patched);
    if (s->stage == ShaderStage::Vertex) clip_z_module(s, code);  // NX_CLIP_Z_PATCH (nvn_spirv_clip.cpp)
    salt_module(code);  // NX_SPIRV_SALT (measurement)
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = code.size() * 4;
    ci.pCode = code.data();
    VkShaderModule m = VK_NULL_HANDLE;
    if (p_vkCreateShaderModule(vk_device(), &ci, nullptr, &m) != VK_SUCCESS) {
        log("nvn/vk: shader %s rejected", s->hash.c_str());
        m = VK_NULL_HANDLE;
    }
    PipelineObjectsLock lk;
    auto [it, fresh] = cache.emplace(std::make_tuple(s, k, unwritten), m);
    if (!fresh) {  // another thread made it first
        if (m) p_vkDestroyShaderModule(vk_device(), m, nullptr);
        return it->second;
    }
    static int logged = 0;  // under the lock
    if (is_patched && logged++ < 20) log("nvn/vk: shader %s: integer vertex inputs", s->hash.c_str());
    return m;
}

VkDescriptorSetLayout set_layout(const std::vector<VkDescriptorSetLayoutBinding>& bindings) {
    PipelineObjectsLock lk;
    static std::map<std::string, VkDescriptorSetLayout> cache;
    std::string key(reinterpret_cast<const char*>(bindings.data()), bindings.size() * sizeof(bindings[0]));
    auto& l = cache[key];
    if (!l) {
        VkDescriptorSetLayoutCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        ci.bindingCount = uint32_t(bindings.size());
        ci.pBindings = bindings.data();
        p_vkCreateDescriptorSetLayout(vk_device(), &ci, nullptr, &l);
    }
    return l;
}

// ---- images ---------------------------------------------------------------------------------------------------------

// Views made for sampling and for rendering, by image; forget_image_views() drops an image's before it is freed.
using SampledKey = std::tuple<VkImage, VkImageViewType, uint32_t, uint64_t, uint64_t>;
std::map<SampledKey, VkImageView> g_sampled_views;
std::map<std::tuple<VkImage, uint32_t, uint32_t, VkFormat>, VkImageView> g_attachment_views;
// In front of g_sampled_views (a tree walk per sampled texture of every draw): copies of entries it holds, by a hash
// of the key (view null: an empty slot). forget_image_views, the only place entries go, empties the image's slots.
struct ViewHit {
    SampledKey key{};
    VkImageView view = VK_NULL_HANDLE;
};
ViewHit g_view_hits[256];

bool identity(const VkComponentMapping& m) {
    auto id = [](VkComponentSwizzle s, VkComponentSwizzle own) { return s == VK_COMPONENT_SWIZZLE_IDENTITY || s == own; };
    return id(m.r, VK_COMPONENT_SWIZZLE_R) && id(m.g, VK_COMPONENT_SWIZZLE_G) && id(m.b, VK_COMPONENT_SWIZZLE_B) &&
           id(m.a, VK_COMPONENT_SWIZZLE_A);
}


// Views Metal accepts of an image: cubes only of cube images with 6 layers per cube, 3D only of 3D images (a 2D
// view of a 3D image is for rendering a slice), 1D of 1D (only with NX_1D_IMAGES: NVN 1D textures are 2D images of
// height 1, since the translator samples them as 2D, G8). A shader declaring another shape than the texture it gets
// (a samplerCube given a 2D texture) reads the dummy instead: Metal aborts on the view.
bool view_compatible(const GpuImage& img, VkImageViewType type, uint32_t layers) {
    bool cube_img = img.view_type == VK_IMAGE_VIEW_TYPE_CUBE || img.view_type == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
    bool img3d = img.view_type == VK_IMAGE_VIEW_TYPE_3D;
    bool img1d = img.view_type == VK_IMAGE_VIEW_TYPE_1D || img.view_type == VK_IMAGE_VIEW_TYPE_1D_ARRAY;
    switch (type) {
        case VK_IMAGE_VIEW_TYPE_CUBE:
        case VK_IMAGE_VIEW_TYPE_CUBE_ARRAY: return cube_img && layers >= 6;
        case VK_IMAGE_VIEW_TYPE_3D: return img3d;
        case VK_IMAGE_VIEW_TYPE_1D:
        case VK_IMAGE_VIEW_TYPE_1D_ARRAY: return img1d;
        default: return !img3d && !img1d;
    }
}

// A view for sampling: the image's own when it fits, else one of the view type with the texture's swizzle.
VkImageView view_of(GpuImage& img, VkImageViewType type, const ViewInfo& view) {
    uint32_t swizzle = img.alpha_from_r ? 0 : view.swizzle;  // materialized at upload
    VkComponentMapping m = img.swizzle;
    if (swizzle) {
        constexpr VkComponentSwizzle kSwizzle[] = {VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ONE,
                                                   VK_COMPONENT_SWIZZLE_R,    VK_COMPONENT_SWIZZLE_G,
                                                   VK_COMPONENT_SWIZZLE_B,    VK_COMPONENT_SWIZZLE_A};
        m = {kSwizzle[swizzle & 0xFF], kSwizzle[(swizzle >> 8) & 0xFF], kSwizzle[(swizzle >> 16) & 0xFF],
             kSwizzle[swizzle >> 24]};
    }
    uint32_t base = std::min(view.base_level, img.levels - 1);
    uint32_t levels = view.levels ? std::min(view.levels, img.levels - base) : img.levels - base;
    uint32_t min_layer = std::min(view.min_layer, img.layers - 1);
    bool whole = base == 0 && levels == img.levels && min_layer == 0 && (!view.layers || view.layers == img.layers);
    VkFormat format = view_format(img, view.format);
    if (type == img.view_type && identity(m) && whole && format == img.format) return img.view;
    auto& cache = g_sampled_views;
    uint64_t range = uint64_t(base) | uint64_t(levels) << 16 | uint64_t(min_layer) << 32 | uint64_t(view.layers) << 48;
    uint32_t mapping = uint32_t(m.r) | uint32_t(m.g) << 8 | uint32_t(m.b) << 16 | uint32_t(m.a) << 24;
    SampledKey key{img.image, type, mapping, range, uint64_t(format)};
    uint64_t h = uintptr_t(img.image) >> 4 ^ range * 31 ^ mapping ^ uint64_t(type) << 5 ^ uint64_t(format);
    ViewHit& hit = g_view_hits[(h ^ h >> 8) & 255];
    if (hit.view && hit.key == key) return hit.view;
    auto& v = cache[key];
    if (!v) {
        VkImageViewCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        ci.image = img.image;
        ci.viewType = type;
        ci.format = format;
        ci.components = m;
        uint32_t layers = std::min(view.layers ? view.layers : img.layers - min_layer, img.layers - min_layer);
        if (type == VK_IMAGE_VIEW_TYPE_2D || type == VK_IMAGE_VIEW_TYPE_1D) layers = 1;
        if (type == VK_IMAGE_VIEW_TYPE_CUBE) layers = 6;
        if (type == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY) layers -= layers % 6;
        if (!view_compatible(img, type, layers)) return VK_NULL_HANDLE;
        ci.subresourceRange = {img.aspect == (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)
                                   ? VkImageAspectFlags(VK_IMAGE_ASPECT_DEPTH_BIT)
                                   : img.aspect,
                               base, levels, min_layer, layers};
        if (p_vkCreateImageView(vk_device(), &ci, nullptr, &v) != VK_SUCCESS) v = img.view;
    }
    if (v) hit = {key, v};  // never a null one: those are tried again
    return v;
}

// R11G11B10F (0x3f) has no alpha, stored here as RGBA16F: sampling reads alpha 1 and blending takes destination
// alpha as 1, as Maxwell does (G4, docs/nvn/DEBUG.md #50). NX_R11G11B10_RAW=1: the stored alpha (bisecting).
bool ufloat_raw() {
    static const bool raw = std::getenv("NX_R11G11B10_RAW") != nullptr;
    return raw;
}

}  // namespace

// ---- layouts ----------------------------------------------------------------------------------------------------------

const ProgramLayout& program_layout(const std::vector<const ShaderInfo*>& stages) {
    PipelineObjectsLock lk;  // entries are never erased: the reference stays valid after it
    static std::map<std::vector<const ShaderInfo*>, ProgramLayout> cache;
    static const std::pair<const std::vector<const ShaderInfo*>, ProgramLayout>* last = nullptr;  // entries stay
    if (last && last->first == stages) return last->second;  // runs of draws use one program
    auto it = cache.find(stages);
    if (it != cache.end()) return (last = &*it)->second;
    std::map<uint32_t, VkDescriptorSetLayoutBinding> sets[4];
    for (const ShaderInfo* s : stages)
        for (const DescriptorBinding& b : s->refl.bindings) {
            if (b.set >= 4) continue;
            auto& vb = sets[b.set][b.binding];
            vb.binding = b.binding;
            vb.descriptorType = VkDescriptorType(b.type);
            vb.descriptorCount = std::max(1u, b.count);
            vb.stageFlags |= stage_bit(s->stage);
        }
    ProgramLayout pl;
    // On by default on Android only (NX_DYNAMIC_UBO=0/1 overrides): on the Mac (MoltenVK) every frame came out black
    // and the GPU hung a few thousand frames in (2026-10-01), cause not found yet.
    static const uint32_t max_dynamic = [] {
        const char* e = std::getenv("NX_DYNAMIC_UBO");
#if defined(__ANDROID__)
        if (e && e[0] == '0') return 0u;
#else
        if (!e || e[0] != '1') return 0u;
#endif
        VkPhysicalDeviceProperties p{};
        p_vkGetPhysicalDeviceProperties(vk_physical(), &p);
        log("nvn/vk: uniform buffers bound with dynamic offsets (up to %u per layout)",
            p.limits.maxDescriptorSetUniformBuffersDynamic);
        return p.limits.maxDescriptorSetUniformBuffersDynamic;
    }();
    uint64_t dynamic = 0;
    for (auto& [n, b] : sets[0])
        if (b.descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER && b.descriptorCount == 1 && n < 64) dynamic |= 1ull << n;
    if (dynamic && uint32_t(__builtin_popcountll(dynamic)) <= max_dynamic) {
        for (auto& [n, b] : sets[0])
            if (n < 64 && dynamic >> n & 1) b.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        pl.dynamic = dynamic;
    }
    for (int k = 0; k < 4; k++) {
        std::vector<VkDescriptorSetLayoutBinding> v;
        for (auto& [n, b] : sets[k]) v.push_back(b);
        pl.sets[k] = set_layout(v);
    }
    VkPipelineLayoutCreateInfo ci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    ci.setLayoutCount = 4;
    ci.pSetLayouts = pl.sets;
    p_vkCreatePipelineLayout(vk_device(), &ci, nullptr, &pl.layout);
    return cache[stages] = pl;
}

// ---- graphics pipelines ---------------------------------------------------------------------------------------------

// What the pipelines' fixed state depends on outside the key, for this device and this run (nvn_pipeline_state.h).
const FixedStateEnv& fixed_state_env() {
    static const FixedStateEnv env = [] {
        FixedStateEnv e;
        e.depth_clip_control = vk_depth_clip_control() && !depth_remap();
        e.depth_clamp_feature = vk_features().depthClamp;
        e.alpha_to_coverage = std::getenv("NX_NO_ALPHA_TO_COVERAGE") == nullptr;
        e.ufloat_alpha_one = !ufloat_raw();
        e.equal_slack = equal_slack_on();
        e.greater_slack = greater_slack_on();
        e.stencil_test = std::getenv("NX_DEBUG_NO_STENCIL") == nullptr;
        if (const char* v = std::getenv("NX_DEBUG_NO_DEPTH_WRITE")) e.no_depth_write_fs = v;
        if (const char* v = std::getenv("NX_DEBUG_EQUAL_ALWAYS")) e.equal_always = v;
        return e;
    }();
    return env;
}

VkPipeline graphics_pipeline(const PipelineKey& draw_key, const ProgramLayout& layout) {
    // By the key's bytes, as before, but hashed (nvn_pipeline.h) instead of a ~700-byte std::string built and
    // compared down a tree per draw; and the last entry first: consecutive draws mostly share their state.
    // NX_PIPELINE_KEY_RAW=1: the draw's key as it comes (before canonical(), for comparison).
    static const bool raw = std::getenv("NX_PIPELINE_KEY_RAW") != nullptr;
    // The draw's own key first: consecutive draws mostly repeat it exactly, and then nothing is recomputed.
    static PipelineKey last_draw_key;
    static VkPipeline last_draw_pipeline = VK_NULL_HANDLE;
    if (last_draw_pipeline && std::memcmp(&draw_key, &last_draw_key, sizeof draw_key) == 0) return last_draw_pipeline;
    const PipelineKey key = raw ? draw_key : canonical(draw_key, fixed_state_env());
    using Key = ByteKey<sizeof(PipelineKey)>;
    static std::unordered_map<Key, VkPipeline, ByteKeyHash> cache;
    static const std::pair<const Key, VkPipeline>* last = nullptr;  // entries are never erased
    auto remember = [&](VkPipeline p) {
        std::memcpy(&last_draw_key, &draw_key, sizeof draw_key);
        last_draw_pipeline = p;
        return p;
    };
    if (last && std::memcmp(&key, last->first.b, sizeof key) == 0) return remember(last->second);
    Key k;
    std::memcpy(k.b, &key, sizeof key);
    if (auto it = cache.find(k); it != cache.end()) return remember((last = &*it)->second);

    // First use in this run: one the catalog prepared in advance (nvn_pipeline_catalog.cpp), else made now.
    VkPipeline p = catalog_take(key);
    const bool from_catalog = p != VK_NULL_HANDLE;
    const uint64_t t0 = plat::monotonic_ns();
    if (!p) {
        auto desc = describe_graphics_pipeline(key, layout);
        p = create_graphics_pipeline(*desc);
        if (!p)
            log("nvn/vk: pipeline for %s + %s failed", key.vs ? key.vs->hash.c_str() : "-", key.fs ? key.fs->hash.c_str() : "-");
        else if (g_trace)
            log("nvn/vk: pipeline %zu: %s + %s", cache.size() + 1, key.vs ? key.vs->hash.c_str() : "-", key.fs ? key.fs->hash.c_str() : "-");
    }
    const bool recorded = p && catalog_note(key);
    // A collection session (collect.h): every pipeline compiled while playing, its cost and whether the catalogs
    // lacked it (recorded: new to pipelines_collected.bin).
    if (!from_catalog)
        collect::event("pipeline", "graphics\t%.2f ms\t%s\t%s\ttopology %u, %u targets, depth format %d\t%s",
                       double(plat::monotonic_ns() - t0) / 1e6, key.vs ? key.vs->hash.c_str() : "-",
                       key.fs ? key.fs->hash.c_str() : "-", key.topology, key.color_count, int(key.depth_format),
                       !p ? "failed" : recorded ? "recorded" : "known");
    return remember((last = &*cache.emplace(k, p).first)->second);
}

VkPipeline create_graphics_pipeline(const GraphicsPipelineDesc& d) {
    VkPipeline p = VK_NULL_HANDLE;
    const uint64_t t0 = plat::monotonic_ns();
    VkResult made = p_vkCreateGraphicsPipelines(vk_device(), pipeline_cache(), 1, &d.ci, nullptr, &p);
    note_pipeline_created(plat::monotonic_ns() - t0);
    if (made != VK_SUCCESS) {  // the driver's reason for the first few (a bare "failed" said nothing on the Mali)
        static std::atomic<int> logged{0};
        if (logged++ < 8) log("nvn/vk: vkCreateGraphicsPipelines returned %d", int(made));
    }
    return made == VK_SUCCESS ? p : VK_NULL_HANDLE;
}

// Not under the shared lock as a whole (§5zd9): module_of takes it for its cache only, and the trace and the render
// pass objects below take it themselves.
std::unique_ptr<GraphicsPipelineDesc> describe_graphics_pipeline(const PipelineKey& key, const ProgramLayout& layout) {
    auto out = std::make_unique<GraphicsPipelineDesc>();
    GraphicsPipelineDesc& d = *out;
    VkPipelineShaderStageCreateInfo* stages = d.stages;
    uint32_t nstages = 0;
    uint8_t int_kinds[16] = {};
    for (uint32_t a = 0; a < key.attrib_count && a < 16; a++) int_kinds[a] = uint8_t(vertex_int_kind(key.attribs[a].format));
    // NX_TRACE_PIPELINES=1: every new pipeline's shaders, and each vertex shader's inputs once.
    static std::set<const ShaderInfo*> logged_vs;
    static const bool trace = std::getenv("NX_TRACE_PIPELINES") != nullptr;
    if (trace) log("nvn/vk: new pipeline vs %s fs %s", key.vs ? key.vs->hash.c_str() : "-", key.fs ? key.fs->hash.c_str() : "-");
    if (trace) {
        PipelineObjectsLock lk;
        if (key.vs && logged_vs.insert(key.vs).second) {
            std::string line;
            for (const ShaderIo& in : key.vs->refl.inputs) {
                char b[64];
                const VertexAttrib* va = in.location < key.attrib_count ? &key.attribs[in.location] : nullptr;
                std::snprintf(b, sizeof b, " %u:%x/s%u+%u", in.location, va ? va->format : 0, va ? va->stream : 0,
                              va ? va->offset : 0);
                line += b;
            }
            log("nvn/vk: vs %s inputs (location:format/stream+offset)%s", key.vs->hash.c_str(), line.c_str());
        }
    }
    uint32_t unwritten = unwritten_inputs(key.vs, key.fs);
    for (const ShaderInfo* s : {key.vs, key.fs}) {
        if (!s) continue;
        auto& st = stages[nstages++];
        st.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        st.stage = VkShaderStageFlagBits(stage_bit(s->stage));
        st.module = module_of(s, s == key.vs ? int_kinds : nullptr, s == key.fs ? unwritten : 0);
        st.pName = "main";
    }

    fill_fixed_state(key, fixed_state_env(), d);  // nvn_pipeline_state.cpp
    if (key.alpha_to_coverage)
        log("nvn/vk: alpha to coverage%s: fs %s", fixed_state_env().alpha_to_coverage ? "" : " (off)",
            key.fs ? key.fs->hash.c_str() : "-");
    VkGraphicsPipelineCreateInfo& ci = d.ci;
    if (render_pass_objects()) {  // nvn_renderpass.cpp: compatible with every pass of these formats
        PipelineObjectsLock lk;
        ci.pNext = nullptr, ci.renderPass = compatible_render_pass(key.color_count, key.color_formats, key.depth_format);
    }
    ci.stageCount = nstages;
    ci.pStages = stages;
    ci.layout = layout.layout;
    return out;
}

// ---- sampled textures -------------------------------------------------------------------------------------------------

// Pending nvnTextureWriteTexels regions go to the image (a transfer: outside render passes).
void apply_texel_writes(VkCommandBuffer cb, Texture& t, GpuImage& img) {
    // Nothing queued (nearly always): no lock. Every sampled texture of every draw comes here, and taking the
    // registry lock alone also invalidated every thread's get<T> cache each time (docs/nvn/PERF-PLAN.md).
    if (!t.has_writes.load(std::memory_order_acquire)) return;
    std::vector<Texture::TexelWrite> writes;
    {
        std::lock_guard lk(objects_lock());
        writes.swap(t.writes);
        t.has_writes.store(false, std::memory_order_release);
    }
    if (writes.empty()) return;
    if (img.astc_bc7) return astc_bc7_note_problem("texel writes after the check");  // ensure_contents demotes first
    img.texels = true;
    VkImageLayout back = img.layout;
    bool is3d = img.view_type == VK_IMAGE_VIEW_TYPE_3D;
    for (const auto& w : writes) {
        if (w.level >= img.levels) continue;
        // Ordered after the texture's upload just before and after the previous write (the game may write the same
        // texels twice: the later must win; synchronization validation WRITE_AFTER_WRITE, run 162).
        transfer_write_barrier(cb, img);
        Upload u = upload(w.data.data(), w.data.size(), 16);
        VkBufferImageCopy r{};
        r.bufferOffset = u.offset;
        r.imageSubresource = {img.aspect == (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)
                                  ? VkImageAspectFlags(VK_IMAGE_ASPECT_DEPTH_BIT)
                                  : img.aspect,
                              w.level, is3d ? 0 : std::min(w.z, img.layers - 1), is3d ? 1 : std::min(w.d, img.layers)};
        r.imageOffset = {int32_t(w.x), int32_t(w.y), is3d ? int32_t(w.z) : 0};
        r.imageExtent = {w.w, w.h, is3d ? w.d : 1};
        p_vkCmdCopyBufferToImage(cb, u.buffer, img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
    }
    if (back != VK_IMAGE_LAYOUT_UNDEFINED && back != VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) transition(cb, img, back);
}

uint64_t g_submit_serial = 0;

void pipeline_begin_submit() { g_submit_serial++; }

uint64_t g_not_ready[6];  // why sampled_ready said no: no image, dirty, texel writes, rendered, not uploaded, layout

bool sampled_ready(uint64_t texture) {
    Texture* t = get<Texture>(texture);
    // no image yet (missing textures are the caller's), or one the VRAM eviction gave back (made again before the pass)
    if (!t || !t->image || t->image->evicted) return g_not_ready[0]++, false;
    if (t->image->superseded) return g_not_ready[0]++, false;  // NX_RELEASE_REPLACED: made again before the pass
    // NX_ASTC_BC7: a BC7 image demoted through another texture, or one with a reason to be made RGBA8: made again (and
    // uploaded) before the pass, not inside it
    if (t->image->bc7_demoted || t->image->bc7_remake_why) return g_not_ready[0]++, false;
    check_linear_content(*t);
    if (t->dirty && !t->image->rendered) return g_not_ready[1]++, false;
    if (t->has_writes.load(std::memory_order_acquire)) return g_not_ready[2]++, false;  // no lock (nvn.h)
    const GpuImage& img = *t->image;
    bool ok = img.rendered ? img.layout == VK_IMAGE_LAYOUT_GENERAL
                           : img.uploaded && img.layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    if (!ok) g_not_ready[img.rendered ? 3 : !img.uploaded ? 4 : 5]++;
    return ok;
}

void sampled_ready_log() {
    log("nvn passes: textures not ready: %llu no image, %llu dirty, %llu texel writes, %llu rendered not GENERAL, "
        "%llu not uploaded, %llu uploaded in another layout",
        (unsigned long long)g_not_ready[0], (unsigned long long)g_not_ready[1], (unsigned long long)g_not_ready[2],
        (unsigned long long)g_not_ready[3], (unsigned long long)g_not_ready[4], (unsigned long long)g_not_ready[5]);
}

VkImageView attachment_view(GpuImage& img, uint32_t level, uint32_t layer, VkFormat format) {
    if (format == VK_FORMAT_UNDEFINED) format = img.format;
    level = std::min(level, img.levels - 1);
    // A 3D image's slice is a layer of its 2D-array-compatible views, counted at this level.
    layer = std::min(layer, img.view_type == VK_IMAGE_VIEW_TYPE_3D ? std::max(1u, img.depth >> level) - 1 : img.layers - 1);
    if (level == 0 && layer == 0 && img.levels == 1 && img.layers == 1 && img.view_type == VK_IMAGE_VIEW_TYPE_2D &&
        format == img.format)
        return img.view;
    auto& cache = g_attachment_views;
    auto& v = cache[{img.image, level, layer, format}];
    if (!v) {
        VkImageViewCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        ci.image = img.image;
        ci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        ci.format = format;
        ci.subresourceRange = {img.aspect, level, 1, layer, 1};
        if (p_vkCreateImageView(vk_device(), &ci, nullptr, &v) != VK_SUCCESS) v = img.view;
    }
    return v;
}

GpuImage* target_image(VkCommandBuffer cb, uint64_t texture) {
    GpuImage* img = image_of(texture);
    if (img && img->rebuilt && !img->uploaded) img = ensure_contents(cb, texture);
    return img;
}

GpuImage* ensure_contents(VkCommandBuffer cb, uint64_t texture) {
    Texture* t = get<Texture>(texture);
    GpuImage* img = t ? image_of(texture) : nullptr;
    if (!img) return nullptr;
    // NX_ASTC_BC7: CPU texel writes, or a view in a format BC7 cannot be seen as: the image is made again as RGBA8
    if (img->astc_bc7 && (t->has_writes.load(std::memory_order_acquire) || img->bc7_remake_why)) {
        img = image_remake_rgba8(texture, img->bc7_remake_why ? img->bc7_remake_why : "texel writes");
        if (!img) return nullptr;
    }
    bool dirty = t->dirty.exchange(false);
    static const bool reupload = std::getenv("NX_TEXELS_REUPLOAD") != nullptr;  // bisecting: the old way
    bool upload = !img->rendered && (!img->uploaded || (dirty && (!img->texels || reupload)));
    // A render target (COMPRESSIBLE, flag 0x8) the GPU never wrote through this image: its guest memory never holds
    // GPU output here (another target over the same memory left only stale data), so it starts cleared, not uploaded
    // (G3 cause 2, docs/nvn/DEBUG.md #50). NX_RT_UPLOAD=1: uploaded as before; NX_ALIAS_MAGENTA=1: cleared magenta.
    static const bool rt_upload = std::getenv("NX_RT_UPLOAD") != nullptr;
    static const bool magenta = std::getenv("NX_ALIAS_MAGENTA") != nullptr;
    if (upload && !rt_upload && (t->flags & 0x8) && !img->uploaded && !img->texels && !dirty) {
        if (!t->has_writes.load(std::memory_order_acquire)) {
            clear_image(cb, *img, magenta);
            img->uploaded = true;
            upload = false;
        }
    }
    // NX_ASTC_BC7: the cache's blocks for exactly these bytes, else (other bytes, no readable conversion) the image is
    // made again as RGBA8 and uploaded as before
    if (upload && img->astc_bc7 && !upload_astc_bc7(cb, *t, *img)) {
        img = image_remake_rgba8(texture, img->uploaded ? "bytes changed" : "no conversion to load");
        if (!img) return nullptr;
        upload = !img->rendered && !img->uploaded;
    }
    if (upload && img->astc_bc7) upload = false;  // uploaded just now
    if (upload) {
        const uint64_t t0 = img->rebuilt ? plat::monotonic_ns() : 0;
        upload_texture(cb, *t, *img);
        if (img->rebuilt) vram_evict_note_rebuild(*img, plat::monotonic_ns() - t0), img->rebuilt = false;
    }
    apply_texel_writes(cb, *t, *img);
    if (img->layout == VK_IMAGE_LAYOUT_UNDEFINED) {  // never written: black, not garbage
        transition(cb, *img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkImageSubresourceRange r{img->aspect, 0, img->levels, 0, img->layers};
        if (img->aspect == VK_IMAGE_ASPECT_COLOR_BIT) {
            VkClearColorValue v{};
            p_vkCmdClearColorImage(cb, img->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &v, 1, &r);
        }
    }
    return img;
}

VkImageView sampled_view(VkCommandBuffer cb, uint64_t texture, const ImageShape& shape, const ViewInfo* view,
                         VkImageLayout* layout) {
    *layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    GpuImage* img = ensure_contents(cb, texture);
    static uint64_t no_image = 0, incompatible = 0;
    if (!img) {
        if (++no_image % 100000 == 1) log("nvn/vk: dummy sampled: %llu with no image, %llu with an incompatible view",
                                          (unsigned long long)no_image, (unsigned long long)incompatible);
        return dummy_view(cb, shape);
    }
    *layout = img->rendered ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    transition(cb, *img, *layout);
    // The swizzle belongs to the texture (its builder, or its pool view), not to the image: an image may be shared
    // with or inherited from another texture over the same memory (#36, #42), whose swizzle differs. With the
    // image's, the UI's single-channel masks lost their alpha and drew as rectangles (docs/nvn/DEBUG.md #44).
    // The slot's own view decides: pool_swizzle is whichever slot registered the texture last (the world map's player
    // mask read as G G G G: a flat silhouette, DEBUG-2.md #67). NX_POOL_SWIZZLE_ANY=1: as before.
    static const bool pool_any = std::getenv("NX_POOL_SWIZZLE_ANY") != nullptr;
    ViewInfo vi = view ? *view : ViewInfo{};
    if (!vi.swizzle && !img->alpha_from_r)
        if (Texture* t = get<Texture>(texture)) {
            uint32_t s = pool_any || !view ? t->pool_swizzle.load() : 0u;
            if (!s)
                for (int k = 0; k < 4; k++) s |= uint32_t(std::min<uint8_t>(t->swizzle[k], 5)) << (8 * k);
            vi.swizzle = s;
        }
    // R11G11B10F: a channel reading A (NVNtextureSwizzle 5) reads ONE (1), after the texture's swizzle (G4).
    if (!ufloat_raw() && !img->alpha_from_r && (img->nvn_format == 0x3f || vi.format == 0x3f)) {
        uint32_t s = vi.swizzle ? vi.swizzle : 0x05040302u;  // 0: identity (R G B A)
        for (int k = 0; k < 4; k++)
            if ((s >> (8 * k) & 0xFF) == 5) s = (s & ~(0xFFu << (8 * k))) | (1u << (8 * k));
        vi.swizzle = s;
    }
    VkImageView v = view_of(*img, view_type(shape), vi);
    if (!v) {
        *layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        if (++incompatible % 1000 == 1)
            log("nvn/vk: texture %llx (%ux%u x%u, view type %d) sampled as view type %d: dummy", (unsigned long long)texture,
                img->width, img->height, img->layers, int(img->view_type), int(view_type(shape)));
        return dummy_view(cb, shape);
    }
    return v;
}

VkPipeline create_compute_pipeline(const ShaderInfo* cs, const ProgramLayout& layout) {
    VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    ci.stage.module = module_of(cs);
    ci.stage.pName = "main";
    ci.layout = layout.layout;
    VkPipeline p = VK_NULL_HANDLE;
    if (p_vkCreateComputePipelines(vk_device(), pipeline_cache(), 1, &ci, nullptr, &p) != VK_SUCCESS) {
        log("nvn/vk: compute pipeline for %s failed", cs->hash.c_str());
        p = VK_NULL_HANDLE;
    }
    return p;
}

VkPipeline compute_pipeline(const ShaderInfo* cs, const ProgramLayout& layout) {
    static std::map<std::pair<const ShaderInfo*, VkPipelineLayout>, VkPipeline> cache;
    auto& p = cache[{cs, layout.layout}];
    if (!p) {
        // Made in advance from the catalog (WONDER_SWITCH2_PROGRESS.md §5zd2): its layout is another object, made
        // from the same set layouts (set_layout is cached by content), so it is compatible with the sets bound here.
        if ((p = catalog_take_compute(cs))) return p;
        const uint64_t t0 = plat::monotonic_ns();
        p = create_compute_pipeline(cs, layout);
        // Counted with the graphics pipelines (host work, hitches), and listed so the cost of compute is known.
        const uint64_t ns = plat::monotonic_ns() - t0;
        note_pipeline_created(ns);
        static uint32_t made = 0;
        if (++made <= 64) log("nvn/vk: compute pipeline %u: %s (%.1f ms)", made, cs->hash.c_str(), double(ns) / 1e6);
        const bool recorded = p && catalog_note_compute(cs);
        collect::event("pipeline", "compute\t%.2f ms\t%s\t-\tshared %u bytes\t%s", double(ns) / 1e6, cs->hash.c_str(),
                       cs->control.shared_size, !p ? "failed" : recorded ? "recorded" : "known");
    }
    return p;
}

VkImageView storage_view(VkCommandBuffer cb, uint64_t texture, const ImageShape& shape, const ViewInfo* view) {
    GpuImage* img = texture ? ensure_contents(cb, texture) : nullptr;
    if (img && img->storage) {
        transition(cb, *img, VK_IMAGE_LAYOUT_GENERAL);
        img->rendered = true;  // the shader may write it
        ViewInfo v = view ? *view : ViewInfo{};
        v.levels = 1, v.swizzle = 0;  // storage views: one level, identity
        if (VkImageView iv = view_of(*img, view_type(shape), v)) return iv;
    }
    static std::map<VkImageViewType, GpuImage> dummies;
    VkImageViewType type = view_type(shape);
    auto& d = dummies[type];
    if (!d.image) {
        VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ci.imageType = type == VK_IMAGE_VIEW_TYPE_3D ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
        ci.format = VK_FORMAT_R32_UINT;
        ci.extent = {1, 1, 1};
        ci.mipLevels = 1;
        ci.arrayLayers = 1;
        ci.samples = VK_SAMPLE_COUNT_1_BIT;
        ci.usage = VK_IMAGE_USAGE_STORAGE_BIT;
        p_vkCreateImage(vk_device(), &ci, nullptr, &d.image);
        VkMemoryRequirements req{};
        p_vkGetImageMemoryRequirements(vk_device(), d.image, &req);
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        p_vkAllocateMemory(vk_device(), &ai, nullptr, &d.memory);
        p_vkBindImageMemory(vk_device(), d.image, d.memory, 0);
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = d.image;
        vi.viewType = type == VK_IMAGE_VIEW_TYPE_3D ? VK_IMAGE_VIEW_TYPE_3D
                      : (type == VK_IMAGE_VIEW_TYPE_2D_ARRAY ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D);
        vi.format = ci.format;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        p_vkCreateImageView(vk_device(), &vi, nullptr, &d.view);
        d.aspect = VK_IMAGE_ASPECT_COLOR_BIT;
        d.format = ci.format;
    }
    transition(cb, d, VK_IMAGE_LAYOUT_GENERAL);
    return d.view;
}

void forget_image_views(const GpuImage& img) {
    // The caches are ordered by image first: its views are one run from the smallest key with that image (this was
    // a scan of every cached view per destroyed texture, a visible share of the replay time).
    auto drop = [&](auto& cache, auto lowest) {
        for (auto it = cache.lower_bound(lowest); it != cache.end() && std::get<0>(it->first) == img.image;) {
            forget_framebuffers(it->second);
            if (it->second && it->second != img.view) p_vkDestroyImageView(vk_device(), it->second, nullptr);
            it = cache.erase(it);
        }
    };
    forget_framebuffers(img.view);  // nvn_renderpass.cpp (attachments may use the whole-image view)
    drop(g_sampled_views, std::make_tuple(img.image, VkImageViewType(0), 0u, uint64_t(0), uint64_t(0)));
    for (ViewHit& h : g_view_hits)
        if (std::get<0>(h.key) == img.image) h = {};
    drop(g_attachment_views, std::make_tuple(img.image, 0u, 0u, VkFormat(0)));
}

void mark_catalog_worker_thread() { t_catalog_worker = true; }
uint64_t objects_lock_wait_ns_this_thread() { return t_objects_wait_ns; }
ObjectsLockWaits objects_lock_waits() { return {g_objects_waits.load(), g_objects_wait_ns.load(), g_objects_wait_max.load()}; }

}  // namespace nx::nvn
