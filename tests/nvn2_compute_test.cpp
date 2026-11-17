// NVN 2 compute c[0] layout (runtime/src/nvn/nvn_shaders.cpp read_control / nvn2_compute_c0_slot, and
// nvn_spirv_reflect.cpp apply_harness). Synthetic control sections, no game data. Standalone:
//   clang++ -std=c++20 -O2 -DNX_SHADERS_STANDALONE -Iruntime/src/nvn tests/nvn2_compute_test.cpp \
//       runtime/src/nvn/nvn_shaders.cpp runtime/src/nvn/nvn_spirv_reflect.cpp -o <out>
//
// The layout was checked on every compute block of Wonder's Switch 2 Edition (102 blocks: 368 storage-buffer and
// 144 uniform-buffer descriptor reads and 97 texture handles, all on an entry of the tables computed from the counts).
#include <cstdio>
#include <cstring>
#include <vector>

#include "nvn_shaders.h"

using namespace nx::nvn;

namespace {

int g_failed = 0;

void expect(bool ok, const char* what) {
    if (!ok) std::printf("FAIL: %s\n", what), g_failed++;
}

std::vector<uint8_t> control(const uint8_t roots[8], uint8_t ubo, uint8_t ssbo, uint8_t tex, uint8_t img) {
    std::vector<uint8_t> c(0xe0, 0);
    auto put = [&](size_t at, uint32_t v) { std::memcpy(c.data() + at, &v, 4); };
    put(0x00, kShaderControlMagic);
    put(0x04, 0xc9);
    put(0x08, 20);
    put(0x38, 0x3280);
    put(0x40, 0x3300);
    put(0x44, 0x3400);
    for (int k = 0; k < 8; k++) c[0xa9 + k] = roots[k];
    c[0xb1] = ubo, c[0xb2] = ssbo, c[0xb3] = tex, c[0xb6] = img;
    return c;
}

}  // namespace

int main() {
    // The effect compute shader 5c19f80d4b766329: no root tables, 14 uniform buffers, 8 storage buffers, 16 textures
    const uint8_t none[8] = {};
    auto c = control(none, 14, 8, 16, 0);
    ShaderControl ctl = read_control(c.data());
    expect(ctl.valid && ctl.nvn2, "NVN 2 control");
    expect(ctl.c0.ubo == 0x10 && ctl.c0.ssbo == 0xf0 && ctl.c0.tex == 0x170 && ctl.c0.img == 0x1f0, "table offsets");
    int index = -1;
    expect(nvn2_compute_c0_slot(ctl, 0x60, index) == 1 && index == 5, "0x60: uniform buffer 5");
    expect(nvn2_compute_c0_slot(ctl, 0xe0, index) == 1 && index == 13, "0xe0: uniform buffer 13");
    expect(nvn2_compute_c0_slot(ctl, 0xf0, index) == 2 && index == 0, "0xf0: storage buffer 0");
    expect(nvn2_compute_c0_slot(ctl, 0x160, index) == 2 && index == 7, "0x160: storage buffer 7");
    expect(nvn2_compute_c0_slot(ctl, 0x1e8, index) == 3 && index == 15, "0x1e8: texture 15");
    expect(nvn2_compute_c0_slot(ctl, 0x168, index) == 0, "0x168: between entries");
    expect(nvn2_compute_c0_slot(ctl, 0x08, index) == 0, "0x08: the head");

    // Root tables of 3 and 5 words take 16 and 32 bytes: the uniform table moves to 0x40
    const uint8_t roots[8] = {3, 5, 0, 0, 0, 0, 0, 0};
    auto c2 = control(roots, 2, 1, 1, 2);
    ShaderControl r = read_control(c2.data());
    expect(r.c0.ubo == 0x40 && r.c0.ssbo == 0x60 && r.c0.tex == 0x70 && r.c0.img == 0x78, "root tables");
    expect(nvn2_compute_c0_slot(r, 0x80, index) == 4 && index == 1, "image 1");

    // apply_harness: the harness's c[0] words resolve through the program's layout
    ShaderReflection refl;
    refl.stage = ShaderStage::Compute;
    DescriptorBinding sb, ub, tx;
    sb.set = 1, sb.binding = 0, sb.type = DescriptorType::StorageBuffer;
    ub.set = 1, ub.binding = 1, ub.type = DescriptorType::StorageBuffer;
    tx.set = 2, tx.binding = 0, tx.type = DescriptorType::CombinedImageSampler;
    refl.bindings = {sb, ub, tx};
    std::vector<HarnessResource> list;
    expect(parse_harness_resources("sb[c0+0x40]=s1b0 sb[c0+0x18]=s1b1 texTexture2D[c0:122]=s2b0", list),
           "parse resources");
    expect(apply_harness(refl, list, &ctl), "apply");
    expect(refl.bindings[0].nvn.source == NvnSource::StorageBuffer && refl.bindings[0].nvn.index == 1,
           "c0 word 0x40 (0x100): storage buffer 1");
    expect(refl.bindings[1].nvn.source == NvnSource::UniformAsStorage && refl.bindings[1].nvn.index == 5,
           "c0+0x60: uniform buffer 5");
    expect(refl.bindings[2].nvn.source == NvnSource::SamplerBinding && refl.bindings[2].nvn.index == 15,
           "texture handle at c0 word 122: sampler binding 15");

    std::printf("nvn2_compute_test: %s\n", g_failed ? "FAILED" : "all passed");
    return g_failed ? 1 : 0;
}
