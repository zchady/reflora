// The canonical pipeline key (runtime/src/nvn/nvn_pipeline_state.cpp) against an oracle written from the Vulkan
// specification: what a VkGraphicsPipelineCreateInfo's fixed state actually decides once Vulkan's own rules are
// applied (blend factors only with blending on, the depth compare op and writes only with the depth test on and a
// depth attachment, the stencil state only with the stencil test on and a stencil attachment...). No device, no game
// data: synthetic keys and stages.
//
//   1. canonical(key) gives the same effective pipeline as key (nothing that matters is changed);
//   2. no false merge: two keys with the same canonical key have the same effective pipeline;
//   3. canonical is idempotent, and padding or leftovers never make two keys of one pipeline differ;
//   4. named cases for blend, depth/stencil, vertex input, targets, raster and the depth bias.
//
// Standalone (llvm-mingw or MSVC; Vulkan headers on the include path):
//   clang++ -std=c++20 -O2 -Iruntime/include -Iruntime/src -Iruntime/src/nvn -I<Vulkan-Headers>/include \
//       tests/pipeline_key_test.cpp runtime/src/nvn/nvn_pipeline_state.cpp runtime/src/nvn/nvn_enums.cpp -o <out>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "nvn_pipeline_state.h"

using namespace nx::nvn;

// nvn_format.cpp asks the device whether R11G11B10F is native (only colour formats depend on it).
namespace nx::nvn {
bool native_r11g11b10() { return false; }
}  // namespace nx::nvn

namespace {

int g_failed = 0;
void expect(bool ok, const char* what) {
    if (!ok) std::printf("FAIL: %s\n", what), g_failed++;
}

// ---- the oracle: the fixed state Vulkan uses, from the create info alone --------------------------------------------

struct Writer {
    std::string s;
    template <class T>
    void put(const T& v) { s.append(reinterpret_cast<const char*>(&v), sizeof v); }
};

std::string effective(const GraphicsPipelineDesc& d) {
    Writer w;
    // vertex input: each attribute, and each binding an attribute reads (stride and rate)
    w.put(d.vi.vertexAttributeDescriptionCount);
    for (uint32_t i = 0; i < d.vi.vertexAttributeDescriptionCount; i++) {
        const auto& a = d.vi.pVertexAttributeDescriptions[i];
        w.put(a.location), w.put(a.binding), w.put(a.format), w.put(a.offset);
        bool found = false;
        for (uint32_t b = 0; b < d.vi.vertexBindingDescriptionCount; b++)
            if (d.vi.pVertexBindingDescriptions[b].binding == a.binding) {
                w.put(d.vi.pVertexBindingDescriptions[b].stride), w.put(d.vi.pVertexBindingDescriptions[b].inputRate);
                found = true;
            }
        w.put(found);
    }
    w.put(d.ia.topology);
    w.put(d.vp.pNext != nullptr);  // depth clip control (negativeOneToOne)
    const auto& r = d.rs;
    w.put(r.polygonMode), w.put(r.cullMode), w.put(r.frontFace), w.put(r.depthClampEnable), w.put(r.lineWidth);
    w.put(r.depthBiasEnable);  // kept even with no depth test: a bias changes the depth a shader reads (FragCoord.z)
    w.put(d.ms.alphaToCoverageEnable);
    // depth: only with a depth attachment and the test on; writes only with the test on
    const bool depth_att = d.ri.depthAttachmentFormat != VK_FORMAT_UNDEFINED;
    const bool depth_test = depth_att && d.ds.depthTestEnable;
    w.put(depth_test);
    if (depth_test) w.put(d.ds.depthCompareOp), w.put(VkBool32(d.ds.depthWriteEnable ? 1 : 0));
    // stencil: only with a stencil attachment and the test on (masks and reference are dynamic)
    const bool stencil_test = d.ri.stencilAttachmentFormat != VK_FORMAT_UNDEFINED && d.ds.stencilTestEnable;
    w.put(stencil_test);
    if (stencil_test)
        for (const VkStencilOpState* f : {&d.ds.front, &d.ds.back})
            w.put(f->failOp), w.put(f->depthFailOp), w.put(f->passOp), w.put(f->compareOp);
    // colour targets: formats, write masks; factors and ops only with blending on
    w.put(d.ri.colorAttachmentCount);
    for (uint32_t k = 0; k < d.ri.colorAttachmentCount; k++) w.put(d.ri.pColorAttachmentFormats[k]);
    w.put(d.ri.depthAttachmentFormat), w.put(d.ri.stencilAttachmentFormat);
    w.put(d.cb.attachmentCount);
    for (uint32_t k = 0; k < d.cb.attachmentCount; k++) {
        const auto& a = d.cb.pAttachments[k];
        w.put(a.colorWriteMask), w.put(VkBool32(a.blendEnable ? 1 : 0));
        if (a.blendEnable)
            w.put(a.srcColorBlendFactor), w.put(a.dstColorBlendFactor), w.put(a.colorBlendOp),
                w.put(a.srcAlphaBlendFactor), w.put(a.dstAlphaBlendFactor), w.put(a.alphaBlendOp);
    }
    w.put(d.dy.dynamicStateCount);
    for (uint32_t i = 0; i < d.dy.dynamicStateCount; i++) w.put(d.dy.pDynamicStates[i]);
    return w.s;
}

std::string effective(const PipelineKey& k, const FixedStateEnv& env) {
    GraphicsPipelineDesc d;
    fill_fixed_state(k, env, d);
    return effective(d);
}

std::string bytes(const PipelineKey& k) { return std::string(reinterpret_cast<const char*>(&k), sizeof k); }

// ---- synthetic stages and keys ----------------------------------------------------------------------------------------

std::vector<ShaderInfo> make_stages(std::mt19937& rng) {
    std::vector<ShaderInfo> v(12);
    for (size_t i = 0; i < v.size(); i++) {
        v[i].hash = "stage" + std::to_string(i);
        v[i].stage = i % 2 ? ShaderStage::Fragment : ShaderStage::Vertex;
        v[i].refl.stage = v[i].stage;
        if (v[i].stage == ShaderStage::Vertex) {
            uint32_t mask = rng() & 0xFFFF;
            if (i == 0) mask = 0;  // a vertex shader with no inputs
            for (uint32_t l = 0; l < 16; l++)
                if (mask >> l & 1) v[i].refl.inputs.push_back(ShaderIo{l, 0, ScalarType::Other, 4, 1});
            if (i == 2) v[i].refl.inputs.push_back(ShaderIo{17, 0, ScalarType::Other, 4, 1});  // past 16 attributes
        }
    }
    // stage 10: a vertex shader reading locations 0..3 (the named cases)
    v[10].refl.inputs.clear();
    for (uint32_t l = 0; l < 4; l++) v[10].refl.inputs.push_back(ShaderIo{l, 0, ScalarType::Other, 4, 1});
    return v;
}

const uint32_t kVertexFormats[] = {0, 0x51, 0x55, 0x2b, 0x3c, 0x47, 0x1e, 0x25, 0xfff, 7};
const VkFormat kColorFormats[] = {VK_FORMAT_UNDEFINED, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R16G16B16A16_SFLOAT,
                                  VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_R32_SFLOAT};
const VkFormat kDepthFormats[] = {VK_FORMAT_UNDEFINED, VK_FORMAT_D32_SFLOAT, VK_FORMAT_D32_SFLOAT_S8_UINT,
                                  VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_S8_UINT, VK_FORMAT_D16_UNORM};

uint32_t pick(std::mt19937& rng, uint32_t n) { return uint32_t(rng() % n); }

PipelineKey random_key(std::mt19937& rng, const std::vector<ShaderInfo>& stages) {
    PipelineKey k;
    // garbage everywhere first, padding included: a draw's key is built over whatever was there
    for (size_t i = 0; i < sizeof k; i++) reinterpret_cast<unsigned char*>(&k)[i] = uint8_t(rng());
    k.vs = &stages[2 * pick(rng, uint32_t(stages.size() / 2))];
    k.fs = pick(rng, 8) ? &stages[2 * pick(rng, uint32_t(stages.size() / 2)) + 1] : nullptr;
    k.topology = pick(rng, 8);
    for (auto& a : k.attribs)
        a.format = kVertexFormats[pick(rng, sizeof kVertexFormats / 4)], a.offset = pick(rng, 64), a.stream = pick(rng, 17);
    for (auto& s : k.streams) s.stride = pick(rng, 4) * 12, s.divisor = pick(rng, 4) ? 0 : pick(rng, 3);
    k.attrib_count = pick(rng, 17), k.stream_count = pick(rng, 17);
    for (auto& b : k.blend) {
        uint32_t* f = &b.target;
        for (int j = 0; j < 7; j++) f[j] = pick(rng, 3) ? pick(rng, 12) : 0x61 + pick(rng, 4);
    }
    for (auto& e : k.color.blend_enable) e = uint8_t(pick(rng, 3));
    k.color.alpha_test = rng(), k.color.logic_op = rng();
    for (auto& m : k.mask.mask) m = uint8_t(rng());
    k.depth.depth_test = uint8_t(pick(rng, 3)), k.depth.depth_write = uint8_t(pick(rng, 3));
    k.depth.stencil_test = uint8_t(pick(rng, 3));
    k.depth.depth_func = pick(rng, 9);
    for (int f = 0; f < 2; f++)
        k.depth.stencil_func[f] = pick(rng, 9), k.depth.stencil_fail[f] = pick(rng, 9),
        k.depth.depth_fail[f] = pick(rng, 9), k.depth.depth_pass[f] = pick(rng, 9);
    k.polygon.cull = pick(rng, 4), k.polygon.front = pick(rng, 3), k.polygon.mode = pick(rng, 4);
    k.polygon.offset_enables = pick(rng, 8);
    for (auto& f : k.color_formats) f = kColorFormats[pick(rng, 5)];
    k.color_count = pick(rng, 9);
    k.depth_format = kDepthFormats[pick(rng, 6)];
    k.depth_clamp = pick(rng, 3), k.ufloat_colors = rng(), k.alpha_to_coverage = pick(rng, 3);
    return k;
}

// One field of the key changed at random (as the next draw's state could differ).
void mutate(std::mt19937& rng, PipelineKey& k, const std::vector<ShaderInfo>& stages) {
    PipelineKey other = random_key(rng, stages);
    auto* dst = reinterpret_cast<unsigned char*>(&k);
    const auto* src = reinterpret_cast<const unsigned char*>(&other);
    struct Range {
        size_t at, size;
    };
    const Range fields[] = {
        {offsetof(PipelineKey, topology), 4},          {offsetof(PipelineKey, attribs) + 12 * pick(rng, 16), 12},
        {offsetof(PipelineKey, streams) + 8 * pick(rng, 16), 8}, {offsetof(PipelineKey, attrib_count), 8},
        {offsetof(PipelineKey, blend) + 28 * pick(rng, 8), 28},  {offsetof(PipelineKey, color), sizeof(ColorStateInfo)},
        {offsetof(PipelineKey, mask), 8},              {offsetof(PipelineKey, depth), sizeof(DepthStencilInfo)},
        {offsetof(PipelineKey, depth) + 4 + 4 * pick(rng, 9), 4}, {offsetof(PipelineKey, polygon), 16},
        {offsetof(PipelineKey, color_formats) + 4 * pick(rng, 8), 4}, {offsetof(PipelineKey, color_count), 4},
        {offsetof(PipelineKey, depth_format), 4},      {offsetof(PipelineKey, depth_clamp), 4},
        {offsetof(PipelineKey, ufloat_colors), 4},     {offsetof(PipelineKey, alpha_to_coverage), 4},
        {offsetof(PipelineKey, depth) + 3, 1},  // the padding byte
    };
    const Range& r = fields[pick(rng, sizeof fields / sizeof fields[0])];
    std::memcpy(dst + r.at, src + r.at, r.size);
}

// ---- named cases ----------------------------------------------------------------------------------------------------

PipelineKey base_key(const std::vector<ShaderInfo>& stages) {
    PipelineKey k;
    std::memset(&k, 0, sizeof k);
    k.vs = &stages[10];
    k.fs = &stages[5];
    k.topology = 4;
    for (uint32_t l = 0; l < 16; l++) k.attribs[l] = {0x2b, 4 * l, l % 2};
    k.streams[0] = {32, 0}, k.streams[1] = {16, 0};
    k.attrib_count = 16;
    k.color_count = 2;
    k.color_formats[0] = VK_FORMAT_R8G8B8A8_UNORM, k.color_formats[1] = VK_FORMAT_R16G16B16A16_SFLOAT;
    k.mask.mask[0] = k.mask.mask[1] = 0xF;
    k.blend[0] = {0, 5, 6, 2, 1, 1, 1}, k.blend[1] = {0, 2, 2, 2, 2, 1, 1};
    k.depth_format = VK_FORMAT_D32_SFLOAT_S8_UINT;
    k.depth.depth_test = 1, k.depth.depth_write = 1, k.depth.depth_func = 2;
    k.polygon = {1, 1, 2, 0};
    return k;
}

void named_cases(const std::vector<ShaderInfo>& stages, const FixedStateEnv& env) {
    const PipelineKey base = base_key(stages);
    auto same = [&](const PipelineKey& a, const PipelineKey& b) { return bytes(canonical(a, env)) == bytes(canonical(b, env)); };
    // `applies`: the environment has what the case is about (a switch can make two states the same pipeline: then
    // two canonical keys only cost a duplicate). Everywhere: equal canonical keys must mean the same pipeline.
    auto check = [&](const char* what, bool want_same, auto change, bool applies = true) {
        PipelineKey a = base, b = base;
        change(a, b);
        const bool s = same(a, b);
        const bool eff = effective(a, env) == effective(b, env);
        char line[200];
        std::snprintf(line, sizeof line, "%s: canonical keys %s (expected %s), effective %s", what, s ? "equal" : "differ",
                      want_same ? "equal" : "differ", eff ? "equal" : "differs");
        expect(!s || eff, line);  // never a false merge
        if (applies) expect(s == want_same && s == eff, line);  // and here, the specification's reading exactly
    };
    const bool slack = env.equal_slack, a2c = env.alpha_to_coverage, ufloat = env.ufloat_alpha_one;
    const bool clamp = env.depth_clamp_feature, stencil = env.stencil_test;
    // blend and transparency
    check("blend off, other factors", true, [](PipelineKey& a, PipelineKey& b) { a.color.blend_enable[0] = b.color.blend_enable[0] = 0; b.blend[0].src_rgb = 9; });
    check("blend on, other factor", false, [](PipelineKey& a, PipelineKey& b) { a.color.blend_enable[0] = b.color.blend_enable[0] = 1; b.blend[0].src_rgb = 9; });
    check("blend on, other op", false, [](PipelineKey& a, PipelineKey& b) { a.color.blend_enable[1] = b.color.blend_enable[1] = 1; b.blend[1].eq_a = 4; });
    check("blend enable 1 vs 2", true, [](PipelineKey& a, PipelineKey& b) { a.color.blend_enable[0] = 1, b.color.blend_enable[0] = 2; });
    check("blend on vs off", false, [](PipelineKey&, PipelineKey& b) { b.color.blend_enable[0] = 1; });
    check("write mask", false, [](PipelineKey&, PipelineKey& b) { b.mask.mask[1] = 0x7; });
    check("write mask high bits", true, [](PipelineKey&, PipelineKey& b) { b.mask.mask[1] = 0xFF; });
    check("BlendTarget::target", true, [](PipelineKey&, PipelineKey& b) { b.blend[0].target = 7; });
    check("alpha test / logic op (baked or unused)", true, [](PipelineKey&, PipelineKey& b) { b.color.alpha_test = 5, b.color.logic_op = 3; });
    check("alpha to coverage", false, [](PipelineKey&, PipelineKey& b) { b.alpha_to_coverage = 1; }, a2c);
    check("R11G11B10F target blended with DST_ALPHA", false, [](PipelineKey& a, PipelineKey& b) {
        a.color.blend_enable[0] = b.color.blend_enable[0] = 1, a.blend[0].src_rgb = b.blend[0].src_rgb = 7; b.ufloat_colors = 1; }, ufloat);
    // depth and stencil
    check("depth test on, other func", false, [](PipelineKey&, PipelineKey& b) { b.depth.depth_func = 4; });
    check("depth test off, other func", true, [](PipelineKey& a, PipelineKey& b) { a.depth.depth_test = b.depth.depth_test = 0; b.depth.depth_func = 7; });
    check("depth test off, write on/off", true, [](PipelineKey& a, PipelineKey& b) { a.depth.depth_test = b.depth.depth_test = 0; b.depth.depth_write = 0; });
    check("depth write on/off", false, [](PipelineKey&, PipelineKey& b) { b.depth.depth_write = 0; });
    check("no depth target, other depth state", true, [](PipelineKey& a, PipelineKey& b) {
        a.depth_format = b.depth_format = VK_FORMAT_UNDEFINED; b.depth.depth_func = 6, b.depth.stencil_test = 1; });
    check("stencil on, other op", false, [](PipelineKey& a, PipelineKey& b) { a.depth.stencil_test = b.depth.stencil_test = 1; b.depth.depth_pass[1] = 3; }, stencil);
    check("stencil off, other op", true, [](PipelineKey&, PipelineKey& b) { b.depth.depth_pass[1] = 3, b.depth.stencil_func[0] = 5; });
    check("stencil on but no stencil aspect", true, [](PipelineKey& a, PipelineKey& b) {
        a.depth_format = b.depth_format = VK_FORMAT_D32_SFLOAT; a.depth.stencil_test = b.depth.stencil_test = 1; b.depth.depth_pass[0] = 3; });
    check("stencil on with D24S8", false, [](PipelineKey& a, PipelineKey& b) {
        a.depth_format = b.depth_format = VK_FORMAT_D24_UNORM_S8_UINT; a.depth.stencil_test = b.depth.stencil_test = 1; b.depth.depth_pass[0] = 3; }, stencil);
    check("padding byte", true, [](PipelineKey&, PipelineKey& b) { reinterpret_cast<unsigned char*>(&b.depth)[3] = 0xE3; });
    check("depth format", false, [](PipelineKey&, PipelineKey& b) { b.depth_format = VK_FORMAT_D32_SFLOAT; });
    // depth bias: kept with the depth test off (FragCoord.z)
    check("bias bit on, depth test off", false, [](PipelineKey& a, PipelineKey& b) { a.depth.depth_test = b.depth.depth_test = 0; b.polygon.offset_enables = 4; });
    check("bias bit of another mode", true, [](PipelineKey&, PipelineKey& b) { b.polygon.offset_enables = 1; });
    check("equal slack, depth test off", false, [](PipelineKey& a, PipelineKey& b) {
        a.depth.depth_test = b.depth.depth_test = 0; a.depth.depth_write = b.depth.depth_write = 0; b.depth.depth_func = 3; }, slack);
    check("equal slack on vs bias bit", true, [](PipelineKey& a, PipelineKey& b) {
        a.depth.depth_write = b.depth.depth_write = 0; a.depth.depth_func = b.depth.depth_func = 3; b.polygon.offset_enables = 4; }, slack);
    // vertex input
    check("attribute at an unread location", true, [&](PipelineKey& a, PipelineKey& b) {
        a.vs = b.vs = &stages[0]; b.attribs[3].format = 0x55; });
    check("attribute format at a read location", false, [](PipelineKey&, PipelineKey& b) { b.attribs[0].format = 0x55; });
    check("attribute offset at a read location", false, [](PipelineKey&, PipelineKey& b) { b.attribs[0].offset = 40; });
    check("stride of a read stream", false, [](PipelineKey&, PipelineKey& b) { b.streams[0].stride = 48; });
    check("stride of an unread stream", true, [](PipelineKey&, PipelineKey& b) { b.streams[9].stride = 48; });
    check("divisor 1 vs 3", true, [](PipelineKey& a, PipelineKey& b) { a.streams[0].divisor = 1, b.streams[0].divisor = 3; });
    check("divisor 0 vs 1", false, [](PipelineKey&, PipelineKey& b) { b.streams[0].divisor = 1; });
    check("attribute with no Vulkan format vs none", true, [](PipelineKey& a, PipelineKey& b) {
        a.attribs[0].format = 0xfff; b.attrib_count = 0; a.attrib_count = 0; });
    check("stream_count", true, [](PipelineKey&, PipelineKey& b) { b.stream_count = 9; });
    // render targets
    check("bound target format", false, [](PipelineKey&, PipelineKey& b) { b.color_formats[1] = VK_FORMAT_R8G8B8A8_SRGB; });
    check("unbound target format", true, [](PipelineKey&, PipelineKey& b) { b.color_formats[5] = VK_FORMAT_R8G8B8A8_SRGB; });
    check("unbound target blend", true, [](PipelineKey&, PipelineKey& b) { b.blend[6].src_a = 9, b.color.blend_enable[6] = 1, b.mask.mask[6] = 3; });
    check("target count", false, [](PipelineKey&, PipelineKey& b) { b.color_count = 1; });
    // raster
    check("topology", false, [](PipelineKey&, PipelineKey& b) { b.topology = 5; });
    check("cull", false, [](PipelineKey&, PipelineKey& b) { b.polygon.cull = 2; });
    check("front 1 vs 2", true, [](PipelineKey& a, PipelineKey& b) { a.polygon.front = 1, b.polygon.front = 2; });
    check("depth clamp with no depth target", false, [](PipelineKey& a, PipelineKey& b) {
        a.depth_format = b.depth_format = VK_FORMAT_UNDEFINED; b.depth_clamp = 1; }, clamp);
}

}  // namespace

int main() {
    std::mt19937 rng(20261112);
    const std::vector<ShaderInfo> stages = make_stages(rng);
    std::vector<FixedStateEnv> envs(5);
    envs[1].greater_slack = true;
    envs[2].equal_slack = false;
    envs[3].depth_clamp_feature = false, envs[3].alpha_to_coverage = false, envs[3].ufloat_alpha_one = false;
    envs[4].depth_clip_control = true, envs[4].stencil_test = false;
    uint64_t keys = 0, merges = 0, false_merges = 0, missed = 0, changed = 0, not_idempotent = 0;
    for (const FixedStateEnv& env : envs) {
        named_cases(stages, env);
        for (int i = 0; i < 20000; i++) {
            PipelineKey a = random_key(rng, stages);
            const PipelineKey ca = canonical(a, env);
            keys++;
            if (effective(a, env) != effective(ca, env)) changed++;                    // 1. same pipeline
            if (bytes(canonical(ca, env)) != bytes(ca)) not_idempotent++;               // 3. idempotent
            PipelineKey b = a;
            mutate(rng, b, stages);
            const bool same_key = bytes(canonical(b, env)) == bytes(ca);
            const bool same_pipeline = effective(a, env) == effective(b, env);
            if (same_key) merges++;
            if (same_key && !same_pipeline) false_merges++;                             // 2. no false merge
            if (!same_key && same_pipeline) missed++;                                  // allowed: only a duplicate
        }
    }
    std::printf("pipeline keys: %llu random keys over %zu environments; %llu one-field changes merged by the canonical "
                "key, %llu of them false; %llu keys whose canonical form changed the pipeline; %llu not idempotent; "
                "%llu equal pipelines left apart (harmless duplicates)\n",
                (unsigned long long)keys, envs.size(), (unsigned long long)merges, (unsigned long long)false_merges,
                (unsigned long long)changed, (unsigned long long)not_idempotent, (unsigned long long)missed);
    expect(false_merges == 0, "no false merge");
    expect(changed == 0, "canonical keeps the pipeline");
    expect(not_idempotent == 0, "canonical is idempotent");
    expect(merges > 1000, "the random changes do exercise merges");
    std::printf(g_failed ? "%d FAILED\n" : "all passed\n", g_failed);
    return g_failed ? 1 : 0;
}
