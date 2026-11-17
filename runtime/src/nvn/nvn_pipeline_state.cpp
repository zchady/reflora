// A graphics pipeline's fixed-function state and its canonical key (nvn_pipeline_state.h). No device, no logging:
// tests/pipeline_key_test.cpp builds this file alone.
#include "nvn_pipeline_state.h"

#include <cstring>

namespace nx::nvn {

bool equal_slack_rule(const DepthStencilInfo& d, bool on, bool greater) {
    if (d.depth_write || !on) return false;
    return d.depth_func == 3 || (d.depth_func == 5 && greater);
}

bool has_stencil_aspect(VkFormat f) {
    return f == VK_FORMAT_D32_SFLOAT_S8_UINT || f == VK_FORMAT_S8_UINT || f == VK_FORMAT_D24_UNORM_S8_UINT ||
           f == VK_FORMAT_D16_UNORM_S8_UINT;
}

namespace {

// NVNpolygonOffsetEnable has a bit per polygon mode (POINT 1, LINE 2, FILL 4; modes POINT 0, LINE 1, FILL 2): the
// bit of the mode drawn with decides.
uint32_t offset_bit(uint32_t mode) { return mode <= 2 ? 1u << mode : 4u; }

bool depth_bias_enabled(const PipelineKey& k, const FixedStateEnv& env) {
    return (k.polygon.offset_enables & offset_bit(k.polygon.mode)) ||
           equal_slack_rule(k.depth, env.equal_slack, env.greater_slack);
}

}  // namespace

void fill_fixed_state(const PipelineKey& key, const FixedStateEnv& env, GraphicsPipelineDesc& d) {
    std::memcpy(d.color_formats, key.color_formats, sizeof d.color_formats);

    // Vertex input: each shader input location reads the attribute of the same index. Locations with no attribute
    // (or an unknown format) read a zero vec4 from binding 15.
    d.attrs.clear(), d.binds.clear();
    bool streams_used[16] = {}, zero_used = false;
    if (key.vs)
        for (const ShaderIo& in : key.vs->refl.inputs) {
            VkVertexInputAttributeDescription a{in.location, 15, VK_FORMAT_R32G32B32A32_SFLOAT, 0};
            if (in.location < key.attrib_count) {
                const VertexAttrib& va = key.attribs[in.location];
                VkFormat f = vk_vertex_format(va.format);
                if (f != VK_FORMAT_UNDEFINED && va.stream < 15) {
                    a.binding = va.stream, a.format = f, a.offset = va.offset;
                    streams_used[va.stream] = true;
                }
            }
            if (a.binding == 15) zero_used = true;
            d.attrs.push_back(a);
        }
    for (uint32_t s = 0; s < 15; s++)
        if (streams_used[s]) {
            const VertexStream& vs = key.streams[s];
            d.binds.push_back({s, vs.stride, vs.divisor ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX});
        }
    if (zero_used) d.binds.push_back({15, 0, VK_VERTEX_INPUT_RATE_INSTANCE});
    d.vi.vertexBindingDescriptionCount = uint32_t(d.binds.size());
    d.vi.pVertexBindingDescriptions = d.binds.data();
    d.vi.vertexAttributeDescriptionCount = uint32_t(d.attrs.size());
    d.vi.pVertexAttributeDescriptions = d.attrs.data();

    d.ia.topology = vk_topology(key.topology);

    d.clip.negativeOneToOne = VK_TRUE;  // NVN depth mode 0 (Wonder): clip-space z in -1..1
    d.vp.viewportCount = d.vp.scissorCount = 1;
    d.vp.pNext = env.depth_clip_control ? &d.clip : nullptr;

    VkPipelineRasterizationStateCreateInfo& rs = d.rs;
    rs.polygonMode = key.polygon.mode == 0 ? VK_POLYGON_MODE_POINT : key.polygon.mode == 1 ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL;
    rs.cullMode = vk_cull(key.polygon.cull);
    // NVNfrontFace CW 0 / CCW 1, taken as is: the draws use a negative-height viewport (nvn_draw.cpp) that gives NVN's
    // upper-left-origin mapping, so windings match NVN's.
    rs.frontFace = key.polygon.front == 0 ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;
    // SetDepthClamp: clamps instead of clipping at the near and far planes, like depthClampEnable (a device feature).
    rs.depthClampEnable = key.depth_clamp && env.depth_clamp_feature ? VK_TRUE : VK_FALSE;
    // Factor, units and clamp are dynamic (SetDepthBias at each draw, nvn_raster.cpp).
    rs.depthBiasEnable = depth_bias_enabled(key, env) ? VK_TRUE : VK_FALSE;

    d.ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    // BindMultisampleState's alpha to coverage (G7; single-sampled targets: alpha under 0.5 drops the fragment).
    d.ms.alphaToCoverageEnable = key.alpha_to_coverage && env.alpha_to_coverage ? VK_TRUE : VK_FALSE;

    VkPipelineDepthStencilStateCreateInfo& ds = d.ds;
    const bool has_depth = key.depth_format != VK_FORMAT_UNDEFINED;
    ds.depthTestEnable = has_depth && key.depth.depth_test;
    ds.depthWriteEnable = has_depth && key.depth.depth_write;
    // Diagnostic (not a fix): NX_DEBUG_NO_DEPTH_WRITE=<fs hash>[,...] turns depth writes off for those fragment shaders.
    if (!env.no_depth_write_fs.empty() && key.fs && env.no_depth_write_fs.find(key.fs->hash) != std::string::npos)
        ds.depthWriteEnable = VK_FALSE;
    ds.depthCompareOp = vk_compare(key.depth.depth_func);
    // EQUAL after a depth pre-pass (the colour pass does not write depth) needs both passes to compute bit-identical
    // depth; two translated shaders compiled apart do not, and Wonder's whole course colour pass was discarded (only
    // the sky showed). LESS_OR_EQUAL keeps what EQUAL keeps for opaque geometry after a pre-pass.
    if (key.depth.depth_func == 3 && !key.depth.depth_write) ds.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    // Diagnostic: NX_DEBUG_EQUAL_ALWAYS=1 (or =<fs hash>[,...]) lets those colour passes through.
    if (!env.equal_always.empty() && key.depth.depth_func == 3 && !key.depth.depth_write &&
        (env.equal_always == "1" || (key.fs && env.equal_always.find(key.fs->hash) != std::string::npos)))
        ds.depthCompareOp = VK_COMPARE_OP_ALWAYS;
    ds.stencilTestEnable = has_depth && key.depth.stencil_test && env.stencil_test;
    VkStencilOpState* faces[2] = {&ds.front, &ds.back};
    for (int f = 0; f < 2; f++) {
        faces[f]->failOp = vk_stencil_op(key.depth.stencil_fail[f]);
        faces[f]->depthFailOp = vk_stencil_op(key.depth.depth_fail[f]);
        faces[f]->passOp = vk_stencil_op(key.depth.depth_pass[f]);
        faces[f]->compareOp = vk_compare(key.depth.stencil_func[f]);
        faces[f]->compareMask = faces[f]->writeMask = 0xFF;  // dynamic: SetStencilValueMask / SetStencilMask
    }

    for (uint32_t k = 0; k < key.color_count && k < 8; k++) {
        const BlendTarget& b = key.blend[k];
        // An R11G11B10F target's destination alpha is 1 on Maxwell; the RGBA16F image holds what the shader wrote.
        const bool ufloat = (key.ufloat_colors >> k & 1) && env.ufloat_alpha_one;
        auto factor = [ufloat](uint32_t f) {
            VkBlendFactor v = vk_blend_factor(f);
            if (!ufloat) return v;
            return v == VK_BLEND_FACTOR_DST_ALPHA             ? VK_BLEND_FACTOR_ONE
                   : v == VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA ? VK_BLEND_FACTOR_ZERO
                   : v == VK_BLEND_FACTOR_SRC_ALPHA_SATURATE  ? VK_BLEND_FACTOR_ZERO  // min(As, 1 - 1)
                                                              : v;
        };
        VkPipelineColorBlendAttachmentState& a = d.att[k];
        a.blendEnable = key.color.blend_enable[k] ? VK_TRUE : VK_FALSE;
        a.srcColorBlendFactor = factor(b.src_rgb);
        a.dstColorBlendFactor = factor(b.dst_rgb);
        a.colorBlendOp = vk_blend_op(b.eq_rgb);
        a.srcAlphaBlendFactor = factor(b.src_a);
        a.dstAlphaBlendFactor = factor(b.dst_a);
        a.alphaBlendOp = vk_blend_op(b.eq_a);
        a.colorWriteMask = key.mask.mask[k] & 0xF;
    }
    d.cb.attachmentCount = key.color_count;
    d.cb.pAttachments = d.att;

    // Stencil reference and masks, blend constants and depth bias values come from command-buffer calls
    // (nvn_raster.cpp), so they are dynamic: one pipeline for every value the game uses.
    static const VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR,
                                         VK_DYNAMIC_STATE_BLEND_CONSTANTS, VK_DYNAMIC_STATE_STENCIL_REFERENCE,
                                         VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK, VK_DYNAMIC_STATE_STENCIL_WRITE_MASK,
                                         VK_DYNAMIC_STATE_DEPTH_BIAS};
    d.dy.dynamicStateCount = uint32_t(sizeof dyn / sizeof dyn[0]);
    d.dy.pDynamicStates = dyn;

    d.ri.colorAttachmentCount = key.color_count;
    d.ri.pColorAttachmentFormats = d.color_formats;
    d.ri.depthAttachmentFormat = key.depth_format == VK_FORMAT_S8_UINT ? VK_FORMAT_UNDEFINED : key.depth_format;
    d.ri.stencilAttachmentFormat = has_stencil_aspect(key.depth_format) ? key.depth_format : VK_FORMAT_UNDEFINED;

    VkGraphicsPipelineCreateInfo& ci = d.ci;
    ci.pNext = &d.ri;
    ci.pVertexInputState = &d.vi;
    ci.pInputAssemblyState = &d.ia;
    ci.pViewportState = &d.vp;
    ci.pRasterizationState = &d.rs;
    ci.pMultisampleState = &d.ms;
    ci.pDepthStencilState = &d.ds;
    ci.pColorBlendState = &d.cb;
    ci.pDynamicState = &d.dy;
}

// The key with only what the pipeline is made from, so two keys whose pipelines draw the same are the same key.
// The draw state carries leftovers of earlier draws, and a struct copy carries a padding byte: kept, the same
// pipeline was made again (and missed the catalog) depending on the order objects came in (7,446 pipelines for 744 on
// the 1-1 route, WONDER_SWITCH2_PROGRESS.md §5zd). Cleared:
//  - what fill_fixed_state never reads: attributes at locations the vertex shader does not read, streams no read
//    attribute uses, stream_count, BlendTarget::target, the colour state's alpha test and logic op, everything of
//    unbound colour targets, the padding byte;
//  - what Vulkan ignores (the specification's rules): an attribute with no Vulkan format (it reads the zero binding,
//    like a missing one); the divisor's value (only "per instance" is used); the blend factors and ops of a target
//    with blending off; the depth state with no depth target; the compare op and writes with the depth test off
//    ("depth writes are always disabled when depthTestEnable is VK_FALSE"); the stencil state with no stencil test
//    or no stencil aspect;
//  - booleans to 0/1 (blend enable, depth clamp, alpha to coverage, front face, the depth/stencil flags).
// Kept exactly: everything else, and the effective depthBiasEnable: a bias changes the fragment's depth, which a
// shader may read (gl_FragCoord.z) even with no depth test, so it is computed from the draw's own key first and
// carried in the polygon-offset bit of the mode.
PipelineKey canonical(const PipelineKey& in, const FixedStateEnv& env) {
    PipelineKey k;
    std::memcpy(&k, &in, sizeof k);
    const bool bias = depth_bias_enabled(in, env);

    // Vertex input: only the locations the vertex shader reads, and the streams those read from.
    std::memset(k.attribs, 0, sizeof k.attribs);
    std::memset(k.streams, 0, sizeof k.streams);
    k.stream_count = 0;
    k.attrib_count = k.vs ? 16 : 0;  // a read location past the old count reads a zero attribute: so does a zeroed one
    if (k.vs)
        for (const ShaderIo& io : k.vs->refl.inputs) {
            if (io.location >= 16 || io.location >= in.attrib_count) continue;
            const VertexAttrib& va = in.attribs[io.location];
            if (vk_vertex_format(va.format) != VK_FORMAT_UNDEFINED && va.stream < 15) {  // else: the zero binding
                k.attribs[io.location] = va;
                k.streams[va.stream].stride = in.streams[va.stream].stride;
                k.streams[va.stream].divisor = in.streams[va.stream].divisor ? 1 : 0;  // only "per instance" counts
            }
        }

    // Colour targets: only the bound ones.
    for (uint32_t t = 0; t < 8; t++) {
        k.blend[t].target = 0;
        if (t < k.color_count) {
            k.mask.mask[t] &= 0xF;
            k.color.blend_enable[t] = k.color.blend_enable[t] ? 1 : 0;
            if (!k.color.blend_enable[t]) k.blend[t] = BlendTarget{};  // factors and ops of a target not blended
            continue;
        }
        k.blend[t] = BlendTarget{};
        k.color.blend_enable[t] = 0;
        k.mask.mask[t] = 0;
        k.color_formats[t] = VK_FORMAT_UNDEFINED;
    }
    k.ufloat_colors &= k.color_count >= 32 ? ~0u : (1u << k.color_count) - 1;
    k.color.alpha_test = k.color.logic_op = 0;

    // Depth and stencil: only what a test that is on uses.
    DepthStencilInfo& d = k.depth;
    const bool has_depth = k.depth_format != VK_FORMAT_UNDEFINED;
    if (!has_depth) {
        d = DepthStencilInfo{};
    } else {
        if (!d.depth_test) d.depth_write = 0, d.depth_func = 0;
        if (!d.stencil_test || !has_stencil_aspect(k.depth_format)) {
            d.stencil_test = 0;
            for (int f = 0; f < 2; f++) d.stencil_func[f] = d.stencil_fail[f] = d.depth_fail[f] = d.depth_pass[f] = 0;
        }
        d.depth_test = d.depth_test ? 1 : 0, d.depth_write = d.depth_write ? 1 : 0, d.stencil_test = d.stencil_test ? 1 : 0;
    }
    std::memset(reinterpret_cast<unsigned char*>(&d) + 3, 0, offsetof(DepthStencilInfo, depth_func) - 3);  // padding

    // Raster: booleans, and the depth bias as the draw's key had it (the depth state above may no longer give it).
    k.polygon.front = k.polygon.front ? 1 : 0;
    k.depth_clamp = k.depth_clamp ? 1 : 0;
    k.alpha_to_coverage = k.alpha_to_coverage ? 1 : 0;
    k.polygon.offset_enables = bias ? offset_bit(k.polygon.mode) : 0;
    return k;
}

}  // namespace nx::nvn
