// Draws (docs/NVN.md, N6): replays the bind, state and draw commands of nvn_state.cpp into Vulkan. Everything a draw
// reads from guest memory (vertices, indices, uniform and storage buffers, textures) is copied at replay time into
// the submit's upload buffer: the replay runs right when the game submits and is waited for, so the memory holds
// what the GPU would have read. Render targets become a dynamic-rendering pass, begun at the first draw and ended
// before any transfer.
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <cstring>
#include <set>

#include "nvn_astc_cache.h"
#include "nvn_draw_state.h"
#include "platform.h"

namespace nx::nvn {

using namespace nx::vk;

// The replay state shared with nvn_draw_trace.cpp (nvn_draw_state.h).
namespace replay {

State g_state;
Stats g_stats;

const uint8_t* guest_bytes(uint64_t gpu) {
    uint64_t off = 0;
    MemoryPool* p = pool_at_gpu(gpu, &off);
    uint64_t host = p ? pool_host(*p, off) : 0;
    return host ? reinterpret_cast<const uint8_t*>(host) : nullptr;
}

}  // namespace replay

using namespace replay;

namespace {

bool g_rendering = false;
bool g_render_pass = false;
bool g_vertex_pending = false;  // a narrow pass-end barrier since the last one that covers vertex stages (G4)
uint64_t g_pass_serial = 0, g_vertex_visible_at = 0;  // pass ends so far; the serial a full barrier covered
uint64_t g_vertex_barriers = 0;

// Off by default (NX_PASS_BARRIER_NARROW=1 turns it on): the first S23 run with it showed a black band over the course
// background in 2 of 28 frames (phone_n6def, 15000 and 16200), a timing-dependent hazard of the DEBUG-2 #71 class.
bool narrow_pass_barrier() {
    static const bool on = std::getenv("NX_PASS_BARRIER_NARROW") != nullptr;
    return on;
}

// A vertex shader sampling a rendered image after a narrow pass-end barrier: the pass ends (it cannot take a barrier
// inside) and the writes so far are made visible to the vertex stages.
void vertex_read_barrier(VkCommandBuffer cb, const ShaderInfo* vs);  // the open pass is a render pass object (nvn_renderpass.cpp), not dynamic rendering
Targets g_pass;           // what the open pass renders to
GpuImage* g_pass_images[9];  // their images (8: depth), resolved once per pass for gather_textures
uint32_t g_pass_written = 0;  // targets the open pass's draws wrote: bit c colour c, bit 8 depth
uint64_t g_pass_ends[8];  // passes ended, by draw_end_rendering reason
uint64_t g_submit_ns = 0;  // when the submit being replayed started
VkExtent2D g_pass_extent{};

// What this submit's command buffer already has bound and set. Vulkan keeps the pipeline, vertex and index buffers
// and dynamic state of a command buffer across its dynamic-rendering passes, and most draws repeated what the draw
// before them set (the vertex streams and the viewport of a whole pass): only what differs is recorded. Forgotten
// at each submit, on another command buffer, and when something else binds a pipeline there (the masked clear's
// fullscreen draw, nvn_mask_clear.cpp). NX_DRAW_SLOW: forgotten before every draw.
struct Bound {
    VkCommandBuffer cb = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    uint32_t vertex_set = 0;  // bit s: binding s holds vertex[s]
    Upload vertex[16];
    bool has_index = false, has_dynamic = false;
    Upload index;
    VkIndexType index_type = VK_INDEX_TYPE_UINT16;
    VkViewport viewport{};
    VkRect2D scissor{};
    float blend[4] = {};
    uint32_t raster[10] = {};  // raster_dynamic's inputs: stencil reference and masks per face, the depth bias
} g_bound;
uint64_t g_state_set = 0, g_state_kept = 0;  // state commands recorded and left out, for the log

template <typename T>
bool same_bytes(const T& a, const T& b) { return std::memcmp(&a, &b, sizeof(T)) == 0; }

void bind_vertex(VkCommandBuffer cb, uint32_t binding, const Upload& u) {
    Upload& now = g_bound.vertex[binding];
    if ((g_bound.vertex_set >> binding & 1) && now.buffer == u.buffer && now.offset == u.offset) {
        g_state_kept++;
        return;
    }
    VkDeviceSize off = u.offset;
    p_vkCmdBindVertexBuffers(cb, binding, 1, &u.buffer, &off);
    now = u, g_bound.vertex_set |= 1u << binding, g_state_set++;
}

// Viewport, scissor, blend constants and the rasterizer's dynamic state (every pipeline declares them dynamic).
void set_dynamic(VkCommandBuffer cb, const VkViewport& vp, const VkRect2D& sc) {
    Bound& b = g_bound;
    const State& s = g_state;
    uint32_t raster[10] = {s.stencil_ref[0],        s.stencil_ref[1],        s.stencil_compare_mask[0],
                          s.stencil_compare_mask[1], s.stencil_write_mask[0], s.stencil_write_mask[1]};
    // the bias values only while the game's offset is enabled (nvn_raster.cpp): turning it on or off sets them again
    static const float none = 0.0f;
    const bool game_bias = game_bias_applies(s.polygon);
    std::memcpy(raster + 6, game_bias ? &s.bias_factor : &none, 4), std::memcpy(raster + 7, game_bias ? &s.bias_units : &none, 4);
    std::memcpy(raster + 8, game_bias ? &s.bias_clamp : &none, 4);
    bool slack = s.depth.depth_test && equal_slack_pass(s.depth);
    raster[9] = slack;
    bool all = !b.has_dynamic;
    b.has_dynamic = true;
    auto differs = [&](bool d) { return (all || d) ? (g_state_set++, true) : (g_state_kept++, false); };
    if (differs(!same_bytes(b.viewport, vp))) p_vkCmdSetViewport(cb, 0, 1, &vp), b.viewport = vp;
    if (differs(!same_bytes(b.scissor, sc))) p_vkCmdSetScissor(cb, 0, 1, &sc), b.scissor = sc;
    if (differs(std::memcmp(b.blend, s.blend_color, sizeof b.blend) != 0))
        p_vkCmdSetBlendConstants(cb, s.blend_color), std::memcpy(b.blend, s.blend_color, sizeof b.blend);
    if (differs(std::memcmp(b.raster, raster, sizeof raster) != 0))  // stencil reference and masks, depth bias
        raster_dynamic(cb, slack), std::memcpy(b.raster, raster, sizeof raster);  // (nvn_raster.cpp)
}

void begin_rendering(VkCommandBuffer cb, const Targets& t) {
    if (clears_pending_other(t)) clears_flush(cb);  // other targets' clears first, in order
    // An image never written (layout UNDEFINED) has nothing to load: loadOp CLEAR to zero, not a LOAD of whatever
    // the memory held (G3, docs/nvn/DEBUG.md #50); a pending clear still wins (clears_apply). Env: bisecting.
    static const bool load_undefined = std::getenv("NX_UNDEFINED_LOAD") != nullptr;
    uint32_t fresh = 0;  // bit c colour c, bit 8 depth: begun cleared to 0 (depth 0, stencil 0)
    VkRenderingAttachmentInfo colors[8]{};
    VkRenderingAttachmentInfo depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    VkExtent2D extent{UINT32_MAX, UINT32_MAX};
    VkFormat formats[8]{};  // the views' (render pass objects)
    for (uint32_t k = 0; k < t.color_count; k++) {
        colors[k].sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        GpuImage* img = t.colors[k] ? target_image(cb, t.colors[k]) : nullptr;  // (NX_VRAM_EVICT: contents first)
        g_pass_images[k] = img;
        if (!img) continue;
        if (img->layout == VK_IMAGE_LAYOUT_UNDEFINED && !load_undefined) fresh |= 1u << k;
        transition(cb, *img, VK_IMAGE_LAYOUT_GENERAL);  // rendered images stay GENERAL (sampled while rendered)
        img->rendered = true;
        uint32_t level = std::min<uint32_t>(t.views.level[k], img->levels - 1);
        formats[k] = view_format(*img, t.views.format[k]);
        colors[k].imageView = attachment_view(*img, level, t.views.layer[k], formats[k]);
        // NX_TRACE_CUBES=<first frame>: writes into cube images from that frame on. With 400 from the start, the log
        // ran out at the first 3D scene (frame ~171) and never reached a course load.
        static const char* trace_cubes = std::getenv("NX_TRACE_CUBES");
        static const uint64_t cube_from = trace_cubes ? std::strtoull(trace_cubes, nullptr, 10) : 0;
        static int cube_logs = 0;
        if (trace_cubes && (img->view_type == VK_IMAGE_VIEW_TYPE_CUBE || img->view_type == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY) &&
            g_frame >= cube_from && cube_logs++ < 2000)
            log("cubes: render into %llx level %u layer %u (%ux%u, %u levels, %u layers) view %s",
                (unsigned long long)t.colors[k], level, t.views.layer[k], img->width, img->height, img->levels,
                img->layers, colors[k].imageView == img->view ? "WHOLE IMAGE (fallback)" : "ok");
        colors[k].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        colors[k].loadOp = fresh >> k & 1 ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
        colors[k].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        extent = {std::min(extent.width, std::max(1u, img->width >> level)),
                  std::min(extent.height, std::max(1u, img->height >> level))};
    }
    GpuImage* dimg = t.depth ? target_image(cb, t.depth) : nullptr;
    g_pass_images[8] = dimg;
    if (dimg && dimg->aspect != VK_IMAGE_ASPECT_COLOR_BIT) {
        if (dimg->layout == VK_IMAGE_LAYOUT_UNDEFINED && !load_undefined) fresh |= 1u << 8;
        transition(cb, *dimg, VK_IMAGE_LAYOUT_GENERAL);
        dimg->rendered = true;
        uint32_t level = std::min<uint32_t>(t.views.level[8], dimg->levels - 1);
        depth.imageView = attachment_view(*dimg, level, t.views.layer[8]);
        depth.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        depth.loadOp = fresh >> 8 & 1 ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
        depth.clearValue.depthStencil = {1.0f, 0};  // far (a fresh depth buffer; the course passes test LESS)
        depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        extent = {std::min(extent.width, std::max(1u, dimg->width >> level)),
                  std::min(extent.height, std::max(1u, dimg->height >> level))};
    } else {
        dimg = nullptr;
    }
    if (extent.width == UINT32_MAX) extent = {1, 1};
    VkRenderingAttachmentInfo stencil = depth;  // same view, its own loadOp
    uint32_t cleared = clears_apply(t, colors, depth, stencil);  // pending clears of these targets: loadOp CLEAR
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, extent};
    ri.layerCount = 1;
    ri.colorAttachmentCount = t.color_count;
    ri.pColorAttachments = colors;
    if (dimg) {
        if (dimg->aspect & VK_IMAGE_ASPECT_DEPTH_BIT) ri.pDepthAttachment = &depth;
        if (dimg->aspect & VK_IMAGE_ASPECT_STENCIL_BIT) ri.pStencilAttachment = &stencil;
    }
    g_render_pass = render_pass_objects();
    if (g_render_pass)
        begin_render_pass(cb, t.color_count, formats, colors, dimg ? dimg->format : VK_FORMAT_UNDEFINED, depth, stencil,
                          extent);
    else
        p_vkCmdBeginRendering(cb, &ri);
    g_rendering = true;
    g_pass_written = cleared | fresh;  // as a clear inside the pass: sampling it needs it stored first
    trace_pass("begin: cleared, fresh", t, cleared, fresh);
    g_pass = t;
    g_pass_extent = extent;
}

uint64_t g_dump_draws = 0, g_dump_request = 0;

// DispatchCompute: the bound program's compute stage over (x, y, z) groups, outside render passes, then a barrier
// so what it wrote is visible to what follows.
void dispatch(VkCommandBuffer cb, const Cmd& c) {
    static const bool off = std::getenv("NX_NO_COMPUTE") != nullptr;  // debug: skip compute dispatches
    if (off) return;
    const auto* shaders = program_shaders(g_state.program);
    const ShaderInfo* cs = nullptr;
    if (shaders)
        for (const ShaderInfo* s : *shaders)
            if (s->stage == ShaderStage::Compute) cs = s;
    if (!cs) {  // the program's compute stage has no translation (or was refused): the dispatch is skipped
        static uint64_t skipped = 0;
        if (++skipped <= 5 || skipped % 1000 == 0)
            log("nvn: dispatch %ux%ux%u skipped: program %llx has no compute stage (%llu skipped)", uint32_t(c.i[0]),
                uint32_t(c.i[1]), uint32_t(c.i[2]), (unsigned long long)g_state.program, (unsigned long long)skipped);
        return;
    }
    uint32_t x = uint32_t(c.i[0]), y = uint32_t(c.i[1]), z = uint32_t(c.i[2]);
    const uint8_t* args = c.n == 1 ? guest_bytes(c.a[0]) : nullptr;  // indirect: {x, y, z}
    if (c.n == 1 && !args) return;
    // Written by an earlier dispatch of this submit: only the GPU knows them (vkCmdDispatchIndirect, below).
    bool gpu_args = args && !written_separate() && written_overlaps(args, 12);
    if (args && !gpu_args) std::memcpy(&x, args, 4), std::memcpy(&y, args + 4, 4), std::memcpy(&z, args + 8, 4);
    if (!gpu_args && (!x || !y || !z)) return;
    trace_compute_once(cs, x, y, z);  // 0, 0, 0 for GPU arguments
    trace_nan_dispatch(cs);
    std::vector<const ShaderInfo*> stages{cs};
    const ProgramLayout& layout = program_layout(stages);
    VkPipeline pipeline = compute_pipeline(cs, layout);
    if (!pipeline) return;
    draw_end_rendering(cb, 2);
    gather_textures(stages);
    build_sets(cb, stages, layout);
    p_vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    bind_sets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, layout);
    if (gpu_profile_on()) gpu_profile_mark(cb, "compute " + cs->hash);
    if (!gpu_args || !written_indirect(cb, args)) p_vkCmdDispatch(cb, x, y, z);
    if (gpu_profile_on()) gpu_profile_mark(cb, "outside passes (clears, uploads, copies)");
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    p_vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0,
                           nullptr, 0, nullptr);
    g_stats.dispatches++;
}

void vertex_read_barrier(VkCommandBuffer cb, const ShaderInfo* vs) {
    if (!g_vertex_pending || !vs) return;
    bool reads = false;
    for (size_t k = 0; k < g_textures.bindings.size() && !reads; k++)
        if (g_textures.bindings[k].first == vs && k < g_textures.resolved.size() && g_textures.resolved[k].texture)
            if (GpuImage* img = image_of(g_textures.resolved[k].texture))
                reads = img->rendered && img->rendered_at > g_vertex_visible_at;
    if (!reads) return;
    draw_end_rendering(cb, 1);
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                       VK_ACCESS_SHADER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    p_vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT, VK_PIPELINE_STAGE_VERTEX_SHADER_BIT, 0, 1, &mb, 0,
                           nullptr, 0, nullptr);
    g_vertex_pending = false, g_vertex_barriers++, g_vertex_visible_at = g_pass_serial;
}

// The uniform blocks bound for the vertex and fragment stages, as "stage.index@address:size=hash" (diagnostic).
// NX_TRACE_SLOT_WIDTH: the light camera blocks of the shadow pass (0, the casters' c3) and of the receivers (1, the canopy
// VS's c3), hashed when their pass replays, then checked after each submit and at each present: a block the game
// writes again after its pass was replayed (data written after the submit) shows here.
struct WatchedBlock { uint64_t address = 0, size = 0, hash = 0, frame = 0; };
WatchedBlock g_watched[2];
void watch_block(int k, const Range& r) {
    const uint8_t* p = r.address ? reinterpret_cast<const uint8_t*>(guest_bytes(r.address)) : nullptr;
    if (!p) return;
    g_watched[k] = {r.address, std::min<uint64_t>(r.size, 0x2000), texture_hash(p, std::min<uint64_t>(r.size, 0x2000)), g_frame};
    const float* f = reinterpret_cast<const float*>(p) + (k ? 180 : 4);
    log("%s light block %llx replayed in frame %llu: %f %f %f %f", k ? "receiver" : "caster", (unsigned long long)r.address,
        (unsigned long long)g_frame.load(), f[0], f[1], f[2], f[3]);
}
void watched_check(const char* where) {
    static int logged = 0;
    for (int k = 0; k < 2; k++) {
        WatchedBlock& w = g_watched[k];
        if (!w.address || logged >= 80) continue;
        const uint8_t* p = reinterpret_cast<const uint8_t*>(guest_bytes(w.address));
        uint64_t h = p ? texture_hash(p, w.size) : 0;
        if (h != w.hash) {
            const float* f = reinterpret_cast<const float*>(p) + (k ? 180 : 4);
            logged++, log("%s light block %llx (replayed in frame %llu) changed %s, frame %llu: now %f %f %f %f", k ? "receiver" : "caster",
                          (unsigned long long)w.address, (unsigned long long)w.frame, where, (unsigned long long)g_frame.load(),
                          f[0], f[1], f[2], f[3]);
        }
        w.hash = h;
    }
}

std::string uniform_hashes() {
    std::string out;
    for (int stage : {0, 1})
        for (int k = 0; k < 18; k++) {
            const Range& r = g_state.uniforms[stage][k];
            if (!r.address || !r.size) continue;
            const uint8_t* p = reinterpret_cast<const uint8_t*>(guest_bytes(r.address));
            char b[96];
            std::snprintf(b, sizeof b, " %c%d@%llx:%llx=%08llx", stage ? 'f' : 'v', k, (unsigned long long)r.address,
                          (unsigned long long)r.size,
                          (unsigned long long)(p ? texture_hash(p, std::min<uint64_t>(r.size, 0x10000)) & 0xffffffff : 0));
            out += b;
        }
    return out;
}

bool draw(VkCommandBuffer cb, const Targets& t, uint32_t mode, bool indexed, const Cmd& c) {
    g_stats.total++;
    if (g_state.rasterizer_discard || !g_state.render_enable) return false;  // nothing reaches the targets
    // NX_SKIP_DEPTH_ONLY_WIDTH=<width>: drops depth-only draws into depth targets that wide (diagnostic: a shadow map
    // left as cleared, to tell its content from the rest of the lighting).
    static const uint32_t skip_depth_w = std::getenv("NX_SKIP_DEPTH_ONLY_WIDTH") ? uint32_t(std::atoi(std::getenv("NX_SKIP_DEPTH_ONLY_WIDTH"))) : 0;
    if (skip_depth_w && t.color_count == 0 && t.depth)
        if (Texture* d = get<Texture>(t.depth); d && d->width == skip_depth_w) return false;
    // NX_TRACE_SLOT_WIDTH (with NX_TRACE_SLOT_FRAME): the first depth-only draw of each frame into a depth target that
    // wide, with its polygon offset state, and how many such draws the previous frame had.
    {
        static const uint32_t trace_w = std::getenv("NX_TRACE_SLOT_WIDTH") ? uint32_t(std::atoi(std::getenv("NX_TRACE_SLOT_WIDTH"))) : 0;
        static const uint64_t trace_from = std::getenv("NX_TRACE_SLOT_FRAME") ? std::strtoull(std::getenv("NX_TRACE_SLOT_FRAME"), nullptr, 10) : 0;
        static uint64_t last_frame = 0, count = 0;
        static int traced = 0;
        if (trace_w && t.color_count == 0 && t.depth && g_frame >= trace_from && traced < 200)
            if (Texture* d = get<Texture>(t.depth); d && d->width == trace_w) {
                count++;
                if (g_frame != last_frame) {
                    traced++;
                    log("shadow frame %llu: first caster: offset enables %x mode %u cull %u front %u, bias factor %g units %g "
                        "clamp %g, depth test %u write %u func %u; %llu casters in the frame before",
                        (unsigned long long)g_frame.load(), g_state.polygon.offset_enables, g_state.polygon.mode,
                        g_state.polygon.cull, g_state.polygon.front, g_state.bias_factor, g_state.bias_units,
                        g_state.bias_clamp, g_state.depth.depth_test, g_state.depth.depth_write, g_state.depth.depth_func,
                        (unsigned long long)(count - 1));
                    last_frame = g_frame, count = 1;
                    log("  caster uniforms:%s", uniform_hashes().c_str());
                    watch_block(0, g_state.uniforms[0][0]);
                    // The terrain caster VS's light camera: c3 vec4 0..2 (view, 3x4) and 7..10 (projection).
                    const Range& c3 = g_state.uniforms[0][0];
                    if (const float* m = c3.address && c3.size >= 0xb0 ? reinterpret_cast<const float*>(guest_bytes(c3.address)) : nullptr) {
                        log("  caster view: %f %f %f %f | %f %f %f %f | %f %f %f %f", m[0], m[1], m[2], m[3], m[4], m[5], m[6],
                            m[7], m[8], m[9], m[10], m[11]);
                        const float* p = m + 28;
                        log("  caster proj: %f %f %f %f | %f %f %f %f | %f %f %f %f | %f %f %f %f", p[0], p[1], p[2], p[3], p[4],
                            p[5], p[6], p[7], p[8], p[9], p[10], p[11], p[12], p[13], p[14], p[15]);
                    }
                }
            }
        // The first draw of each frame with the course's canopy shader (a shadow receiver), its uniforms.
        static uint64_t recv_frame = 0;
        static int recv_traced = 0;
        const ShaderInfo* rfs = nullptr;
        if (trace_w && g_frame >= trace_from && g_frame != recv_frame && recv_traced < 200)
            if (const auto* sh = program_shaders(g_state.program))
                for (const ShaderInfo* x : *sh)
                    if (x->stage == ShaderStage::Fragment) rfs = x;
        if (rfs && rfs->hash == "731fbb46881631c3") {
            recv_frame = g_frame, recv_traced++;
            log("receiver frame %llu uniforms:%s", (unsigned long long)g_frame.load(), uniform_hashes().c_str());
            watch_block(1, g_state.uniforms[0][0]);
            // The canopy VS's light matrix: c3 vec4 42..45 (bytes 0x2a0..0x2e0), for the vertex slots 0 and 3.
            for (int k : {0, 3}) {
                const Range& r = g_state.uniforms[0][k];
                const float* m = r.address && r.size >= 0x2e0 ? reinterpret_cast<const float*>(guest_bytes(r.address + 0x2a0)) : nullptr;
                if (m)
                    log("  v%d light rows: %.6f %.6f %.6f %.6f | %.6f %.6f %.6f %.6f | %.6f %.6f %.6f %.6f | %.6f %.6f %.6f %.6f", k,
                        m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8], m[9], m[10], m[11], m[12], m[13], m[14], m[15]);
            }
        }
    }
    const auto* shaders = program_shaders(g_state.program);
    if (!shaders || shaders->empty()) {
        g_stats.no_program++;
        if (!g_state.program) g_stats.no_program_unbound++;
        else if (!shaders) g_stats.no_program_unknown++;
        return false;
    }
    const ShaderInfo* vs = nullptr;
    const ShaderInfo* fs = nullptr;
    for (const ShaderInfo* s : *shaders) {
        if (s->stage == ShaderStage::Vertex) vs = s;
        if (s->stage == ShaderStage::Fragment) fs = s;
    }
    if (!vs) {
        g_stats.no_vertex++;
        return false;
    }
    std::vector<const ShaderInfo*> swapped;  // NX_SWAP_FS / NX_SKIP_FS (nvn_draw_trace.cpp)
    if (!debug_shader_overrides(shaders, fs, swapped)) return false;
    for (const ShaderInfo* s : *shaders)  // NX_SWAP_VS may have replaced it
        if (s->stage == ShaderStage::Vertex) vs = s;
    const ProgramLayout& layout = program_layout(*shaders);

    // NX_SNAP_FS_BEFORE=<hash>[@<first draw>]: snapshot colour target 0 before draws with that fragment shader
    // (what a feedback pass like a tonemap reads), 8 at most.
    {
        static const std::string before = std::getenv("NX_SNAP_FS_BEFORE") ? std::getenv("NX_SNAP_FS_BEFORE") : "";
        static const uint64_t from = before.find('@') != std::string::npos
                                         ? std::strtoull(before.c_str() + before.find('@') + 1, nullptr, 10) : 0;
        static int taken = 0;
        bool match = (fs && before.compare(0, 16, fs->hash) == 0) || before.compare(0, 16, vs->hash) == 0;
        if (!before.empty() && match && g_stats.total >= from && taken < 8 &&
            t.color_count && t.colors[0])
            if (GpuImage* img = image_of(t.colors[0])) {
                taken++;
                draw_end_rendering(cb, 6);
                debug_snapshot(cb, *img, 0xB0000000ull + g_stats.total);
            }
    }
    // Textures first: an upload or a layout change cannot happen inside the render pass.
    uint32_t gathered = gather_textures(*shaders);
    // A pass that neither tests nor writes depth but keeps its depth image bound while sampling that same image (the
    // world map's depth-to-colour linearization, fs 0aa56d1b): on Metal the image is then attached and read in one
    // encoder, which is undefined, and the copy came out 0 everywhere; the composite then took the player for
    // occluded (a flat silhouette, issue #50; DEBUG-2.md #67). The depth is unused: the pass runs without it.
    // NX_KEEP_SAMPLED_DEPTH=1: as before.
    Targets detached;
    const Targets& tt = [&]() -> const Targets& {
        static const bool keep = std::getenv("NX_KEEP_SAMPLED_DEPTH") != nullptr;
        const DepthStencilInfo& d = g_state.depth;
        if (keep || !t.depth || d.depth_test || d.depth_write || d.stencil_test) return t;
        GpuImage* dimg = image_of(t.depth);
        if (!dimg) return t;
        for (const ResolvedTexture& r : g_textures.resolved)
            if (r.texture && image_of(r.texture) == dimg) {
                static uint64_t n = 0;
                if (n++ == 0) log("nvn: a pass samples its own unused depth image: drawn without it (#50)");
                detached = t, detached.depth = 0;
                return detached;
            }
        return t;
    }();
    vertex_read_barrier(cb, vs);  // G4
    bool need_break = gathered & kNeedBreak;
    if (gathered & kSampledPending) clears_flush(cb);  // cleared (and stored) before it is sampled
    static const bool bytes = std::getenv("NX_TARGETS_MEMCMP") != nullptr;  // bisecting
    bool targets_changed = !g_rendering || (bytes ? std::memcmp(&g_pass, &tt, sizeof tt) != 0 : !targets_same(g_pass, tt));
    if (g_rendering && (need_break || targets_changed)) {
        (targets_changed ? g_stats.breaks_targets : g_stats.breaks_textures)++;
        draw_end_rendering(cb, 1);
    }

    build_sets(cb, *shaders, layout);
    // Vertex and index data before the pass begins: a range a dispatch of this submit wrote may need a GPU copy
    // (nvn_written.cpp), which cannot be recorded inside a pass.
    DrawInputs in;
    if (!draw_inputs_shared(cb, vs, indexed, c, in)) {
        g_stats.no_indices++;
        return false;
    }
    check_vertex_ranges(vs, fs, indexed, c, in);  // NX_CHECK_VERTS (nvn_written.cpp)
    if (!g_rendering) {
        if (gpu_profile_on()) {
            GpuImage* img = t.color_count && t.colors[0] ? image_of(t.colors[0]) : nullptr;
            char label[128];
            std::snprintf(label, sizeof label, "%ux%u L%u fs %s", img ? img->width : 0, img ? img->height : 0,
                          t.views.level[0], fs ? fs->hash.c_str() : "-");
            gpu_profile_mark(cb, label);
        }
        begin_rendering(cb, tt);
    }

    // Pipeline.
    PipelineKey key;
    std::memset(&key, 0, sizeof key);  // the key is compared byte for byte
    key.vs = vs, key.fs = fs;
    key.topology = mode;
    key.attrib_count = uint32_t(std::min<size_t>(g_state.attribs.size(), 16));
    std::copy_n(g_state.attribs.begin(), key.attrib_count, key.attribs);
    key.stream_count = uint32_t(std::min<size_t>(g_state.streams.size(), 16));
    std::copy_n(g_state.streams.begin(), key.stream_count, key.streams);
    std::copy_n(g_state.blend, 8, key.blend);
    key.color = g_state.color, key.mask = g_state.mask, key.depth = g_state.depth, key.polygon = g_state.polygon;
    key.color_count = t.color_count, key.depth_clamp = g_state.depth_clamp;
    key.alpha_to_coverage = g_state.alpha_to_coverage;
    for (uint32_t k = 0; k < t.color_count; k++) {
        GpuImage* img = t.colors[k] ? image_of(t.colors[k]) : nullptr;
        key.color_formats[k] = img ? view_format(*img, t.views.format[k]) : unused_slot_format();
        if (img && (img->nvn_format == 0x3f || t.views.format[k] == 0x3f)) key.ufloat_colors |= 1u << k;  // G4
    }
    GpuImage* dimg = tt.depth ? image_of(tt.depth) : nullptr;
    key.depth_format = dimg && dimg->aspect != VK_IMAGE_ASPECT_COLOR_BIT ? dimg->format : VK_FORMAT_UNDEFINED;
    VkPipeline pipeline = graphics_pipeline(key, layout);
    if (!pipeline) {
        g_stats.no_pipeline++;
        return false;
    }

    if (g_bound.cb != cb || draw_slow(kSlowState)) g_bound = Bound{}, g_bound.cb = cb;
    if (g_bound.pipeline != pipeline) {
        p_vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        g_bound.pipeline = pipeline, g_state_set++;
    } else {
        g_state_kept++;
    }
    bind_sets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, layout);

    // The vertex buffers resolved above, plus the zero stream (binding 15).
    for (uint32_t s = 0; s < 15; s++)
        if (in.used >> s & 1) bind_vertex(cb, s, in.vertex[s]);
    bind_vertex(cb, 15, zero_stream());

    VkViewport vp{0, 0, float(g_pass_extent.width), float(g_pass_extent.height), 0, 1};
    if (g_state.has_viewport)
        vp = {float(g_state.viewport[0]), float(g_state.viewport[1]), float(g_state.viewport[2]),
              float(g_state.viewport[3]), g_state.depth_near, g_state.depth_far};
    // NVN's upper-left window origin (what Wonder sets) maps NDC +Y to the top of the render target, like D3D;
    // Vulkan maps it to the bottom. A negative-height viewport gives NVN's mapping, and with it NVN's front faces.
    // (Before this, the front face was inverted instead: triangles were culled right, but every render target was
    // upside down, which the post chain undid in some scenes and not in others: the first course showed a
    // mirrored sky.)
    vp.y += vp.height, vp.height = -vp.height;
    VkRect2D sc{{0, 0}, g_pass_extent};
    if (g_state.has_scissor)
        sc = {{std::max(0, g_state.scissor[0]), std::max(0, g_state.scissor[1])},
              {uint32_t(std::max(0, g_state.scissor[2])), uint32_t(std::max(0, g_state.scissor[3]))}};
    set_dynamic(cb, vp, sc);

    trace_skin(indexed, c, vs, fs);
    if (indexed) {
        uint32_t count = uint32_t(c.i[0]);
        if (g_bound.has_index && g_bound.index.buffer == in.index.buffer && g_bound.index.offset == in.index.offset &&
            g_bound.index_type == in.index_type) {
            g_state_kept++;
        } else {
            p_vkCmdBindIndexBuffer(cb, in.index.buffer, in.index.offset, in.index_type);
            g_bound.has_index = true, g_bound.index = in.index, g_bound.index_type = in.index_type, g_state_set++;
        }
        bool q = zero_draws_on() && zero_draws_begin(cb, vs->hash + " " + (fs ? fs->hash : std::string("-")));
        p_vkCmdDrawIndexed(cb, count, uint32_t(std::max(1, c.i[3])), 0, c.i[1], uint32_t(c.i[2]));
        if (q) zero_draws_end(cb);
    } else {
        bool q = zero_draws_on() && zero_draws_begin(cb, vs->hash + " " + (fs ? fs->hash : std::string("-")));
        p_vkCmdDraw(cb, uint32_t(c.i[1]), uint32_t(std::max(1, c.i[3])), uint32_t(c.i[0]), uint32_t(c.i[2]));
        if (q) zero_draws_end(cb);
    }
    g_stats.done++;
    g_pass_written |= ((1u << t.color_count) - 1) | (g_state.depth.depth_write || g_state.depth.stencil_test ? 1u << 8 : 0);
    // NX_SNAP_FS=<hash>,<hash>[@<first draw>]: also snapshot after draws with those fragment shaders (8 at most, or
    // NX_SNAP_FS_MAX).
    static const std::string snap_fs = std::getenv("NX_SNAP_FS") ? std::getenv("NX_SNAP_FS") : "";
    static const uint64_t snap_from = snap_fs.find('@') != std::string::npos
                                          ? std::strtoull(snap_fs.c_str() + snap_fs.find('@') + 1, nullptr, 10) : 0;
    static int snap_count = 0;
    static const int snap_max = std::getenv("NX_SNAP_FS_MAX") ? std::atoi(std::getenv("NX_SNAP_FS_MAX")) : 8;
    bool snap_by_fs = !snap_fs.empty() && g_stats.done >= snap_from && snap_count < snap_max &&
                      ((fs && snap_fs.find(fs->hash) != std::string::npos) ||
                       snap_fs.find(vs->hash) != std::string::npos) &&
                      ++snap_count;  // the vertex shader's hash matches too
    if ((snap_by_fs || snap_follow(t, vs, fs, g_textures.handles) || debug_snap_wanted(g_stats.done)) && t.color_count && t.colors[0])
        if (GpuImage* snap = image_of(t.colors[0])) draw_end_rendering(cb, 6), debug_snapshot(cb, *snap, g_stats.done, t.views.level[0]);
    // NX_DUMP_ON_TARGET=<hex NVNtexture*>: every NX_DUMP_FRAMES-th draw into it asks for NX_DUMP_TEXTURES to be read
    // back right after this submit (before later frames change them).
    static const char* dump_env = std::getenv("NX_DUMP_ON_TARGET");
    static const uint64_t dump_target = dump_env ? std::strtoull(dump_env, nullptr, 16) : 0;
    if (dump_target && t.colors[0] == dump_target) g_dump_request = ++g_dump_draws;
    // NX_LIST_DRAWS_FRAME=<frame> (diagnostic): the draws of 900 frames from that one with blending on target 0, once per distinct
    // vs/fs/textures: shaders, target size, blend factors, and the sampled textures (size, NVN format, swizzle).
    {
        static const uint64_t ld_frame = std::getenv("NX_LIST_DRAWS_FRAME") ? std::strtoull(std::getenv("NX_LIST_DRAWS_FRAME"), nullptr, 10) : 0;
        static std::set<std::string> seen;
        if (ld_frame && g_frame >= ld_frame && g_frame < ld_frame + 900 && t.color_count && g_state.color.blend_enable[0]) {
            std::string tex;
            for (const ResolvedTexture& r : g_textures.resolved) {
                Texture* x = r.texture ? get<Texture>(r.texture) : nullptr;
                char b[96];
                if (x) {
                    std::snprintf(b, sizeof b, " %ux%ux%u/f%x/t%u/sw%u%u%u%u/l%u/w%u%u@%llx", x->width, x->height, x->depth,
                                  x->format, x->target, x->swizzle[0], x->swizzle[1], x->swizzle[2], x->swizzle[3],
                                  x->levels, r.sampler.wrap[0], r.sampler.wrap[1], (unsigned long long)r.texture);
                    tex += b;
                    if (r.view.swizzle) std::snprintf(b, sizeof b, "/view-sw%08x", r.view.swizzle), tex += b;
                } else {
                    tex += " -";
                }
            }
            const BlendTarget& bl = g_state.blend[0];
            std::string key = vs->hash + (fs ? fs->hash : std::string("-")) + tex;
            // NX_LIST_DUMP=1 (with NX_DUMP_TARGET_EVERY=1): also read back the textures of each new draw (level 0,
            // layer 0, as RGBA8 PPM; NX_DUMP_RAW: as stored).
            static const bool ld_dump = std::getenv("NX_LIST_DUMP") != nullptr;
            static std::set<uint64_t> dumped;
            if (ld_dump && seen.count(key) == 0)
                for (const ResolvedTexture& r : g_textures.resolved)
                    if (r.texture && dumped.insert(r.texture).second && dumped.size() < 300)
                        debug_dump_add(r.texture), g_dump_request = g_frame;
            if (seen.insert(key).second)
                log("draw list %llu: vs %s fs %s target %ux%u blend %u/%u %u/%u eq %u textures:%s",
                    (unsigned long long)ld_frame, vs->hash.c_str(), fs ? fs->hash.c_str() : "-", g_pass_extent.width,
                    g_pass_extent.height, bl.src_rgb, bl.dst_rgb, bl.src_a, bl.dst_a, bl.eq_rgb, tex.c_str());
        }
    }
    // NX_DUMP_SHADOW=<width>,<first frame>,<frames> (diagnostic, with NX_DUMP_RAW and NX_DUMP_TARGET_EVERY=1): in
    // those frames, the depth-only target that wide (a shadow map) and the color target 0 of the draws with fragment
    // shader NX_DUMP_SHADOW_FS (default the map's canopies, a shadow receiver) are read back after their submit, as
    // frames/<frame>_<texture>.raw.
    {
        static const char* ds = std::getenv("NX_DUMP_SHADOW");
        static uint32_t ds_w = 0;
        static uint64_t ds_from = 0, ds_count = 0;
        static const bool ds_parsed = ds && std::sscanf(ds, "%u,%llu,%llu", &ds_w, (unsigned long long*)&ds_from,
                                                        (unsigned long long*)&ds_count) == 3;
        static const std::string ds_fs = std::getenv("NX_DUMP_SHADOW_FS") ? std::getenv("NX_DUMP_SHADOW_FS") : "731fbb46881631c3";
        // NX_RECEIVERS_FRAME=<frame> (with NX_DUMP_SHADOW's width): every draw of that frame sampling the shadow map.
        static const uint64_t rl_frame = std::getenv("NX_RECEIVERS_FRAME") ? std::strtoull(std::getenv("NX_RECEIVERS_FRAME"), nullptr, 10) : 0;
        if (ds_parsed && g_frame == rl_frame && t.color_count == 0 && t.depth)
            if (Texture* d = get<Texture>(t.depth); d && d->width == ds_w) {
                log("caster %llu: draw %llu vs %s fs %s cull %u front %u mode %u offset %x factor %g units %g depth test %u write %u func %u",
                    (unsigned long long)rl_frame, (unsigned long long)g_stats.done, vs->hash.c_str(), fs ? fs->hash.c_str() : "-",
                    g_state.polygon.cull, g_state.polygon.front, g_state.polygon.mode, g_state.polygon.offset_enables,
                    g_state.bias_factor, g_state.bias_units, g_state.depth.depth_test, g_state.depth.depth_write,
                    g_state.depth.depth_func);
            }
        if (ds_parsed && g_frame == rl_frame)
            for (size_t k = 0; k < g_textures.resolved.size(); k++)
                if (Texture* s = g_textures.resolved[k].texture ? get<Texture>(g_textures.resolved[k].texture) : nullptr;
                    s && s->width == ds_w && s->height == ds_w) {
                    log("receiver %llu: draw %llu vs %s fs %s handle %x texture %llx target %llx %ux%u uniforms:%s",
                        (unsigned long long)rl_frame, (unsigned long long)g_stats.done, vs->hash.c_str(),
                        fs ? fs->hash.c_str() : "-", k < g_textures.handles.size() ? unsigned(g_textures.handles[k]) : 0u,
                        (unsigned long long)g_textures.resolved[k].texture, (unsigned long long)t.colors[0],
                        g_pass_extent.width, g_pass_extent.height, uniform_hashes().c_str());
                }
        if (ds_parsed && g_frame >= ds_from && g_frame < ds_from + ds_count) {
            if (t.color_count == 0 && t.depth)
                if (Texture* d = get<Texture>(t.depth); d && d->width == ds_w) debug_dump_add(t.depth), g_dump_request = g_frame;
            if (fs && fs->hash == ds_fs && t.color_count && t.colors[0]) debug_dump_add(t.colors[0]), g_dump_request = g_frame;
        }
    }
    // NX_DUMP_ON_FS=<fs hash>[,...]: a draw with one of these fragment shaders dumps its target 0 and what it samples.
    static const std::string dump_fs = [] { const char* e = std::getenv("NX_DUMP_ON_FS"); return std::string(e ? e : ""); }();
    static const uint64_t dump_from = [] { const char* e = std::getenv("NX_DUMP_FROM"); return e ? std::strtoull(e, nullptr, 10) : 0; }();
    if (!dump_fs.empty() && fs && g_frame >= dump_from && dump_fs.find(fs->hash) != std::string::npos && g_dump_draws < 400 &&
        t.colors[0] && image_of(t.colors[0]) && (std::getenv("NX_DUMP_ANY") || image_of(t.colors[0])->view_type == VK_IMAGE_VIEW_TYPE_CUBE ||
                                                           image_of(t.colors[0])->view_type == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY)) {
        debug_dump_add(t.colors[0]);
        if (std::getenv("NX_DUMP_DEPTH")) debug_dump_add(t.depth);  // with NX_DUMP_RAW: the D32 values
        for (const ResolvedTexture& r : g_textures.resolved) debug_dump_add(r.texture);
        g_dump_request = ++g_dump_draws;
    }
    trace_draw(t, mode, indexed, c, vs, fs, g_textures.bindings, g_textures.handles);
    return true;
}

}  // namespace

void draw_end_submit() {  // after the submit's fence: compute results to guest memory, debug snapshots
    debug_snapshots_write();
    for (const WriteBack& w : draw_take_write_backs()) {
        trace_nan_write_back(w.from, w.to, w.size);
        apply_write_back(w);
    }
}

std::vector<WriteBack> draw_take_write_backs() {  // an asynchronous submit copies them when it is retired
    std::vector<WriteBack> v = written_take();  // one per written range (disjoint), then NX_COMPUTE_SEPARATE's
    v.insert(v.end(), separate_write_backs().begin(), separate_write_backs().end());
    separate_write_backs().clear();
    return v;
}

void draw_begin_submit() {
    g_rendering = false;
    g_vertex_pending = false, g_vertex_visible_at = g_pass_serial;  // each command buffer begins with a full barrier
    g_bound = Bound{};  // a new command buffer
    submit_lists_reset();
    sets_begin_submit();
    clears_begin_submit();
    g_submit_ns = plat::monotonic_ns();
    dedup_reset();
    written_reset();
    pipeline_begin_submit();
}

uint64_t recorded_draws();

void draw_log_stats() {
    if (g_vertex_barriers) log("nvn passes: %llu vertex-read barriers (G4)", (unsigned long long)g_vertex_barriers);
    dedup_log();
    render_pass_log();
    written_log();
    sampled_ready_log();
    sets_log();
    if (g_state_set + g_state_kept)
        log("nvn: draw state: %llu binds and sets recorded, %llu already in place", (unsigned long long)g_state_set,
            (unsigned long long)g_state_kept);
    g_state_set = g_state_kept = 0;
    clears_log();
    log("nvn passes ended: %llu submit end, %llu draw, %llu dispatch, %llu clear of other targets, %llu clear larger than "
        "the pass, %llu copy, %llu snapshot, %llu SetRenderTargets", (unsigned long long)g_pass_ends[0],
        (unsigned long long)g_pass_ends[1], (unsigned long long)g_pass_ends[2], (unsigned long long)g_pass_ends[3],
        (unsigned long long)g_pass_ends[4], (unsigned long long)g_pass_ends[5], (unsigned long long)g_pass_ends[6],
        (unsigned long long)g_pass_ends[7]);
    Stats& t = g_stats;
    log("nvn draws: %llu recorded, %llu replayed:", (unsigned long long)recorded_draws(), (unsigned long long)t.total);
    log("nvn draws: %llu total, %llu done, %llu no program, %llu no vertex shader, %llu no pipeline, %llu no indices",
        (unsigned long long)t.total, (unsigned long long)t.done, (unsigned long long)t.no_program,
        (unsigned long long)t.no_vertex, (unsigned long long)t.no_pipeline, (unsigned long long)t.no_indices);
    log("nvn draws without a program: %llu none bound, %llu unknown object, %llu with no translated stage",
        (unsigned long long)t.no_program_unbound, (unsigned long long)t.no_program_unknown,
        (unsigned long long)(t.no_program - t.no_program_unbound - t.no_program_unknown));
    log("nvn passes: %llu ended by a draw's texture uploads or layout changes, %llu by new targets, %llu dispatches",
        (unsigned long long)t.breaks_textures, (unsigned long long)t.breaks_targets, (unsigned long long)t.dispatches);
}

void pass_end_only(VkCommandBuffer cb, int reason) {
    if (!g_rendering) return;
    g_pass_ends[reason & 7]++, trace_pass("end: reason", g_pass, uint32_t(reason), 0);
    if (g_render_pass)
        p_vkCmdEndRenderPass(cb);
    else
        p_vkCmdEndRendering(cb);
    g_rendering = false;
    // What the pass wrote is read by what follows (sampled, copied, loaded by the next pass), and what it sampled may
    // be rendered over next. The images stay GENERAL, so transition() records no barrier. Metal (MoltenVK) and the
    // Intel driver ordered passes anyway; NVIDIA overlaps them, and later passes read half-written tiles: the course
    // HUD in blocks (DEBUG-2 #71). NX_NO_PASS_BARRIER=1: none, as before (bisecting).
    // Narrow (PHASE4.md G4; NX_PASS_BARRIER_NARROW=1): the later stages only, so the next pass's vertex work and
    // binning may start while this one finishes. A draw whose vertex shader samples a rendered image gets a barrier
    // of its own first (vertex_read_barrier). By default ALL_COMMANDS, as before.
    static const bool no_barrier = std::getenv("NX_NO_PASS_BARRIER") != nullptr;
    if (!no_barrier) {
        VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        mb.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                           VK_ACCESS_SHADER_WRITE_BIT;
        mb.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        VkPipelineStageFlags dst = narrow_pass_barrier()
                                       ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                                             VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT |
                                             VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT |
                                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT
                                       : VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        p_vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT, dst, 0, 1, &mb, 0, nullptr, 0, nullptr);
        g_vertex_pending |= narrow_pass_barrier();
    }
    g_pass_serial++;
    for (uint32_t k = 0; k < 9; k++)  // this pass's targets only (the other slots may name freed images)
        if ((k < g_pass.color_count || (k == 8 && g_pass.depth)) && g_pass_images[k])
            g_pass_images[k]->rendered_at = g_pass_serial;
    gpu_profile_mark(cb, "outside passes (clears, uploads, copies)");
}
void draw_end_rendering(VkCommandBuffer cb, int reason) {  // and the pending clears (nvn_clears.cpp)
    pass_end_only(cb, reason);
    clears_flush(cb);
}
bool pass_open() { return g_rendering; }
bool pass_open_on(const Targets& t) { return g_rendering && targets_same(g_pass, t); }
VkExtent2D pass_extent() { return g_pass_extent; }
void pass_begin(VkCommandBuffer cb, const Targets& t) { begin_rendering(cb, t); }
// After a clear inside the pass (nvn_clears.cpp). A masked one is a fullscreen draw with its own pipeline, viewport
// and scissor (nvn_mask_clear.cpp): the next draw sets everything again.
void pass_mark_written(uint32_t bits) { g_pass_written |= bits, g_bound = Bound{}; }

bool replay::pass_wrote(const GpuImage* img) {
    if (!g_rendering || !img) return false;
    uint32_t hit = g_pass.depth && g_pass_images[8] == img ? 1u << 8 : 0;
    for (uint32_t c = 0; c < g_pass.color_count; c++)
        if (g_pass.colors[c] && g_pass_images[c] == img) hit |= 1u << c;
    return (hit & g_pass_written) != 0;
}

uint64_t submit_start_ns() { return g_submit_ns; }

// Entry points of the draws and dispatches for draw_command (nvn_draw_state.cpp).
bool draw_call(VkCommandBuffer cb, const Targets& t, uint32_t mode, bool indexed, const Cmd& c) {
    return draw(cb, t, mode, indexed, c);
}
void dispatch_call(VkCommandBuffer cb, const Cmd& c) { dispatch(cb, c); }

void watched_blocks_check(const char* where) { watched_check(where); }

uint64_t draw_take_dump_request() {
    uint64_t r = g_dump_request;
    g_dump_request = 0;
    return r;
}

}  // namespace nx::nvn
