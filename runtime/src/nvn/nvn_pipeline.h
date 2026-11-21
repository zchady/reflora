// Pipelines and the resources draws read (docs/NVN.md, N4-N6): shader modules, descriptor-set and pipeline
// layouts, graphics pipelines keyed by their state, samplers, and sampled textures (untiled and uploaded on first
// use). Used by nvn_draw.cpp, under the GPU lock.
#pragma once

#include <cstddef>
#include <cstring>
#include <memory>
#include <vector>

#include "nvn_pipeline_state.h"
#include "nvn_shaders.h"
#include "nvn_vk.h"

namespace nx::nvn {

bool vk_depth_clip_control();

// Cache keys that are compared byte for byte (a PipelineKey with its padding, a SamplerInfo and a flag): the bytes,
// a 64-bit hash of them for an unordered_map, and the exact comparison that confirms a hit. Four lanes: one multiply
// chain over a 704-byte PipelineKey would cost what the lookup saves.
inline uint64_t hash_bytes(const unsigned char* p, size_t size) {
    constexpr uint64_t kMul = 0x9E3779B97F4A7C15ull;
    uint64_t h[4] = {size, 1, 2, 3}, w[4];
    for (; size >= 32; p += 32, size -= 32) {
        std::memcpy(w, p, 32);
        for (int l = 0; l < 4; l++) h[l] = (h[l] ^ w[l]) * kMul, h[l] ^= h[l] >> 32;
    }
    uint64_t r = h[0];
    for (int l = 1; l < 4; l++) r = (r ^ h[l]) * kMul, r ^= r >> 32;
    while (size) {
        size_t n = size < 8 ? size : 8;
        uint64_t t = 0;
        std::memcpy(&t, p, n);
        r = (r ^ t) * kMul, r ^= r >> 32;
        p += n, size -= n;
    }
    return r;
}
template <size_t N>
struct ByteKey {
    unsigned char b[N];
    bool operator==(const ByteKey& o) const { return std::memcmp(b, o.b, N) == 0; }
};
struct ByteKeyHash {
    template <size_t N>
    size_t operator()(const ByteKey<N>& k) const { return size_t(hash_bytes(k.b, N)); }
};

// The four descriptor sets of the translator's scheme (0 uniform buffers, 1 storage buffers, 2 textures, 3 images)
// for a set of stages, and the pipeline layout over them.
struct ProgramLayout {
    VkDescriptorSetLayout sets[4] = {};
    VkPipelineLayout layout = VK_NULL_HANDLE;
    // Set 0 bindings (< 64) that are UNIFORM_BUFFER_DYNAMIC: their ring offset goes with vkCmdBindDescriptorSets, so
    // the set depends only on which buffers and sizes it names (the set cache, nvn_sets.cpp). NX_DYNAMIC_UBO=0: none.
    uint64_t dynamic = 0;
};
const ProgramLayout& program_layout(const std::vector<const ShaderInfo*>& stages);

// PipelineKey, GraphicsPipelineDesc, fill_fixed_state and canonical: nvn_pipeline_state.h (no device).
VkPipeline graphics_pipeline(const PipelineKey& key, const ProgramLayout& layout);
const FixedStateEnv& fixed_state_env();  // this device's and this run's (nvn_pipeline.cpp)

// describe_ makes the shader modules and fills the create info (under the GPU lock: the module and layout caches are
// not locked); create_ only calls the driver, so it can run on any thread (the VkPipelineCache is internally
// synchronized).
std::unique_ptr<GraphicsPipelineDesc> describe_graphics_pipeline(const PipelineKey& key, const ProgramLayout& layout);
VkPipeline create_graphics_pipeline(const GraphicsPipelineDesc& d);  // null on failure

// The pipeline catalog (nvn_pipeline_catalog.cpp): pipelines described by shader hashes and state only, recorded
// as the game uses them and made in advance (before the game asks) from a catalog file.
bool catalog_note(const PipelineKey& key);   // a pipeline the game made: recorded if NX_PIPELINE_RECORD (true: new)
VkPipeline catalog_take(const PipelineKey& key);  // one made in advance for this key, or null
bool catalog_note_compute(const ShaderInfo* cs);       // the same for compute pipelines (a stage and its shared size)
VkPipeline catalog_take_compute(const ShaderInfo* cs);
// The shared lock of shader modules, set and pipeline layouts (nvn_pipeline.cpp): the catalog's workers mark their
// threads; other threads' waits for it (the replay's) are counted, the longest kept.
struct ObjectsLockWaits { uint64_t count, ns, max_ns; };
void mark_catalog_worker_thread();
uint64_t objects_lock_wait_ns_this_thread();
ObjectsLockWaits objects_lock_waits();
void catalog_prewarm_start();  // after the device exists: NX_PIPELINE_PREWARM=<files>, or the data root's catalogs
VkPipeline create_compute_pipeline(const ShaderInfo* cs, const ProgramLayout& layout);  // null on failure; any thread

bool depth_remap();  // NX_CLIP_Z_PATCH: clip z -w..w mapped in the vertex shaders, not by VK_EXT_depth_clip_control
bool patch_clip_z(const std::vector<uint32_t>& in, std::vector<uint32_t>& out);  // nvn_spirv_clip.cpp
void clip_z_module(const ShaderInfo* s, std::vector<uint32_t>& code);             // patch_clip_z if depth_remap

// Integer vertex inputs (nvn_spirv_patch.cpp): the kind of an NVN attribute format (0 float, 1 unsigned bits,
// 2 signed bits), and a copy of a module with those locations declared as integers and bitcast to float on load.
uint32_t vertex_int_kind(uint32_t nvn_format);
bool patch_int_inputs(const std::vector<uint32_t>& in, const uint8_t kinds[16], std::vector<uint32_t>& out);
// Constant buffer reads with a computed index (c[k][index]) are clamped to the 64 KB a bank can hold, and the
// set 0 bindings read that way are returned: those are bound zero-padded to 64 KB (nvn_spirv_patch.cpp).
bool clamp_uniform_indices(const std::vector<uint32_t>& in, std::vector<uint32_t>& out, uint64_t* bindings);
// Storage buffer bindings the shader writes (nvn_spirv_patch.cpp).
uint64_t written_storage_bindings(const ShaderInfo* s);
uint64_t dynamic_uniform_bindings(const ShaderInfo* s);
// Compute: the shared memory array redeclared with the control section's size (the translator declares 48 KB, past
// Metal's 32 KB). Fragment: the input locations in `locations` (bit mask) that the vertex stage does not write become
// private variables holding (0, 0, 0, 1), Maxwell's value for them (nvn_spirv_patch.cpp).
bool resize_shared_memory(const std::vector<uint32_t>& in, uint32_t bytes, std::vector<uint32_t>& out);
bool default_unwritten_inputs(const std::vector<uint32_t>& in, uint32_t locations, std::vector<uint32_t>& out);
// Vertex: every user output of the entry point starts as (0, 0, 0, 1), what a component the code does not write reads
// as on the console (nvn_spirv_patch.cpp). Applied by patched_spirv when vertex_output_init_on().
bool init_vertex_outputs(const std::vector<uint32_t>& in, std::vector<uint32_t>& out);
bool vertex_output_init_on();
// Every stage: GLSL.std.450 FClamp/FMin/FMax become NClamp/NMin/NMax, the NaN-dropping .SAT/FMNMX of the hardware
// (the translator's spirv-nan.patch, done on the module; nvn_spirv_patch.cpp). Applied when nan_dropping_minmax_on().
bool nan_dropping_minmax(const std::vector<uint32_t>& in, std::vector<uint32_t>& out);
bool nan_dropping_minmax_on();
// The fragment input locations `vs` does not write (0 without both stages).
uint32_t unwritten_inputs(const ShaderInfo* vs, const ShaderInfo* fs);
// The code of a stage's module: its SPIR-V with every patch above that applies (`kinds` as patch_int_inputs, or
// null; `unwritten` as default_unwritten_inputs). `int_inputs` gets whether patch_int_inputs changed it.
std::vector<uint32_t> patched_spirv(const ShaderInfo* s, const uint8_t* kinds, uint32_t unwritten, bool* int_inputs);

VkPipeline compute_pipeline(const ShaderInfo* cs, const ProgramLayout& layout);

VkSampler sampler_for(const SamplerInfo& info, bool shadow);
// A storage-image view of the texture (single level of the pool view), or the dummy storage image. Layout GENERAL.
VkImageView storage_view(VkCommandBuffer cb, uint64_t texture, const ImageShape& shape, const ViewInfo* view);

// A sampled texture ready for the shaders (uploaded from guest memory the first time, unless the GPU renders it),
// and a view of it matching what the shader declares. Transitions the image to SHADER_READ_ONLY on cb.
// `view`: the texture pool entry's view (levels, layers, swizzle), if any. `layout` gets the layout to sample in:
// images the GPU renders stay GENERAL (a bloom chain samples one level while rendering another).
VkImageView sampled_view(VkCommandBuffer cb, uint64_t texture, const ImageShape& shape, const ViewInfo* view,
                         VkImageLayout* layout);
// The image behind a texture with its contents in place (uploaded, CPU writes applied, never-written ones cleared);
// null for an unknown texture. Records transfers: outside render passes.
GpuImage* ensure_contents(VkCommandBuffer cb, uint64_t texture);
// The image of a texture about to be rendered or cleared into (a pass begins, a transfer clear): image_of, except that
// an image made again after the VRAM eviction (NX_VRAM_EVICT) gets its contents first, as the image it replaces had
// them: a partial draw or clear must load them, not start from a cleared image. Records transfers: outside passes.
GpuImage* target_image(VkCommandBuffer cb, uint64_t texture);
// nvn_upload.cpp: the texture's pool memory untiled (or taken row by row, pitch-linear) into the image, every level
// and layer; cleared instead when the copies would read past what was staged. Marks it uploaded.
void upload_texture(VkCommandBuffer cb, const Texture& t, GpuImage& img);
// NX_ASTC_BC7 (nvn_astc_bc7.h): an image made BC7 gets the cache's blocks for the texture's current bytes (from the
// background decoder, else from disk). False, with nothing recorded, when it cannot: bytes other than the ones the
// image was chosen for, or no readable conversion of them; the caller then makes the image again as RGBA8.
bool upload_astc_bc7(VkCommandBuffer cb, const Texture& t, GpuImage& img);
// The block height an upload untiles a block-linear texture with: the Texture's own, fitted to the room before the
// next texture of its pool (packaged textures, see upload_texture). The second form is the same steps from the
// texture's parameters and that room (`gap`), for the NX_ASTC_BC7 preparation that reads the game's files.
uint32_t upload_block_height(const Texture& t, uint32_t width, uint32_t height, uint32_t layers, uint32_t levels);
uint32_t upload_block_height(uint32_t format, uint32_t bh0, bool packaged, uint64_t gap, uint32_t width, uint32_t height,
                             uint32_t layers, uint32_t levels);
// Every level and layer to zero (colour; magenta with `magenta`), depth and stencil to 0. Leaves TRANSFER_DST.
void clear_image(VkCommandBuffer cb, GpuImage& img, bool magenta = false);
// A single level and layer of an image, for rendering to.
void sampled_ready_log();
void check_linear_content(Texture& t);  // nvn_sampler.cpp
VkImageView attachment_view(GpuImage& img, uint32_t level, uint32_t layer, VkFormat format = VK_FORMAT_UNDEFINED);
// The Vulkan format of a view with NVN format `nvn` (0: the image's own) of `img`: the image's when the view's is not
// a same-size colour format it can be viewed as.
VkFormat view_format(const GpuImage& img, uint32_t nvn);
// Destroys the cached views of an image about to be freed.
void forget_image_views(const GpuImage& img);
// True when sampled_view() would need no transfer or layout change (it can then be called inside a render pass).
bool sampled_ready(uint64_t texture);
void pipeline_begin_submit();  // per-submit state (the content checks of pitch-linear textures)
VkImageViewType view_type(const ImageShape& shape);  // the view type a shader's image declaration samples
bool dummy_ready(const ImageShape& shape);
// A 1x1 black texture of the shape, for bindings with nothing behind them.
VkImageView dummy_view(VkCommandBuffer cb, const ImageShape& shape);

}  // namespace nx::nvn
