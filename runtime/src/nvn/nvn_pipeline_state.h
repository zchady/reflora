// A graphics pipeline's key and fixed-function state, with no Vulkan device (tests/pipeline_key_test.cpp builds this
// alone): what a draw's state turns into in VkGraphicsPipelineCreateInfo, and the canonical key that identifies a
// pipeline by what it is made from. nvn_pipeline.cpp adds the shader modules, the layout and the driver call.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "nvn.h"
#include "nvn_shaders.h"

namespace nx::nvn {

// Everything a graphics pipeline is built from (NVN enums, decoded state).
struct PipelineKey {
    const ShaderInfo* vs = nullptr;
    const ShaderInfo* fs = nullptr;
    uint32_t topology = 4;
    VertexAttrib attribs[16] = {};
    VertexStream streams[16] = {};
    uint32_t attrib_count = 0, stream_count = 0;
    BlendTarget blend[8] = {};
    ColorStateInfo color;
    ChannelMaskInfo mask;
    DepthStencilInfo depth;
    PolygonInfo polygon;
    VkFormat color_formats[8] = {};
    uint32_t color_count = 0;
    VkFormat depth_format = VK_FORMAT_UNDEFINED;
    uint32_t depth_clamp = 0;  // SetDepthClamp (applied only where the device has the depthClamp feature)
    uint32_t ufloat_colors = 0;  // bit k: colour target k is R11G11B10F (no alpha: destination alpha reads 1)
    uint32_t alpha_to_coverage = 0;  // BindMultisampleState (G7)
};
// The key is compared byte for byte: its only padding is the byte after DepthStencilInfo's three flags, which
// canonical() clears. A new field with padding around it must be cleared there too (the test checks the sizes).
static_assert(sizeof(DepthStencilInfo) == 40 && offsetof(DepthStencilInfo, depth_func) == 4, "one padding byte");
static_assert(sizeof(PipelineKey) == 704, "PipelineKey layout: update canonical(), the catalog version and the test");

// What the pipeline's state also depends on outside the key: the device and the runtime's switches. The runtime's
// values come from fixed_state_env() (nvn_pipeline.cpp); a test passes its own.
struct FixedStateEnv {
    bool depth_clip_control = false;  // VK_EXT_depth_clip_control in use (clip z -1..1), not NX_CLIP_Z_PATCH
    bool depth_clamp_feature = true;  // the device's depthClamp
    bool alpha_to_coverage = true;    // not NX_NO_ALPHA_TO_COVERAGE
    bool ufloat_alpha_one = true;     // not NX_R11G11B10_RAW: an R11G11B10F target's destination alpha reads 1
    bool equal_slack = true;          // NX_EQUAL_SLACK not 0 (nvn_raster.cpp)
    bool greater_slack = false;       // NX_GREATER_SLACK=1
    bool stencil_test = true;         // not NX_DEBUG_NO_STENCIL
    std::string no_depth_write_fs;    // NX_DEBUG_NO_DEPTH_WRITE (diagnostic)
    std::string equal_always;         // NX_DEBUG_EQUAL_ALWAYS (diagnostic)
};

// An EQUAL colour pass after a depth pre-pass (tested, not written) is drawn with a small depth bias toward the camera
// (DEBUG.md #60); GREATER the same way with NX_GREATER_SLACK.
bool equal_slack_rule(const DepthStencilInfo& d, bool on, bool greater);
// Depth formats with a stencil aspect.
bool has_stencil_aspect(VkFormat f);

// A graphics pipeline's create info and everything it points to (heap-allocated: never moved).
struct GraphicsPipelineDesc {
    VkPipelineShaderStageCreateInfo stages[2]{};
    std::vector<VkVertexInputAttributeDescription> attrs;
    std::vector<VkVertexInputBindingDescription> binds;
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    VkPipelineViewportDepthClipControlCreateInfoEXT clip{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_DEPTH_CLIP_CONTROL_CREATE_INFO_EXT};
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkPipelineColorBlendAttachmentState att[8]{};
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    VkFormat color_formats[8] = {};
    VkPipelineRenderingCreateInfo ri{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    VkGraphicsPipelineCreateInfo ci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
};

// Fills everything but the shader stages, the layout and a render pass object: vertex input, input assembly,
// viewport, rasterization, multisample, depth/stencil, blend, dynamic states and the dynamic-rendering formats, with
// ci pointing at them (pNext = ri). Pure: the same key and env give the same bytes.
void fill_fixed_state(const PipelineKey& key, const FixedStateEnv& env, GraphicsPipelineDesc& d);

// The key with only what the pipeline is made from: equal create infos (as Vulkan uses them), equal keys. See the
// definition for the rules; tests/pipeline_key_test.cpp checks them against the specification's.
PipelineKey canonical(const PipelineKey& key, const FixedStateEnv& env);

}  // namespace nx::nvn
