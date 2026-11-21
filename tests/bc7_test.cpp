// BC7 encoder test (runtime/src/nvn/nvn_bc7_encode.cpp, NX_ASTC_BC7), decoded with the runtime's own BC7 decoder
// (nvn_bc.cpp, built standalone). Built by CMake with -DNX_TESTS=ON.
//
//   bc7_test --selftest
//       synthetic images (gradients, noise, hard edges, an anti-aliased icon on transparency, alpha ramps, solid
//       blocks): PSNR floors, opaque stays exactly opaque, grey stays grey, sizes that are not multiples of 4, the same
//       output twice, and a digest of the whole output: when it changes, the encoder's output changed, so
//       kBc7EncoderVersion (nvn_bc7_encode.h) must be bumped (it keys the NX_ASTC_BC7 cache) and the digest updated.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

#include "nvn_bc7_encode.h"

namespace nx::nvn {
void bc_decode(uint32_t nvn_format, const uint8_t* blocks, uint32_t width, uint32_t height, uint32_t slices,
               uint8_t* out);
}

using nx::nvn::bc7_encode_image;

namespace {

// The digest of every encoded image below with encoder version 3 (see the header).
constexpr uint64_t kDigest = 0x05a23c098c19a14aull;

int g_failed = 0;
uint64_t g_digest = 0xcbf29ce484222325ull;

void expect(bool ok, const char* what) {
    if (!ok) std::printf("FAIL: %s\n", what), g_failed++;
}

struct Image {
    uint32_t w, h;
    std::vector<uint8_t> rgba;
    Image(uint32_t w_, uint32_t h_) : w(w_), h(h_), rgba(uint64_t(w_) * h_ * 4) {}
    uint8_t* at(uint32_t x, uint32_t y) { return rgba.data() + (uint64_t(y) * w + x) * 4; }
};

struct Result {
    double psnr_rgb = 99, psnr_alpha = 99;
    bool alpha_kept_opaque = true, grey_kept = true;
};

Result round_trip(const char* name, const Image& img) {
    std::vector<uint8_t> blocks(uint64_t((img.w + 3) / 4) * ((img.h + 3) / 4) * 16), again(blocks.size());
    bc7_encode_image(img.rgba.data(), img.w, img.h, blocks.data());
    bc7_encode_image(img.rgba.data(), img.w, img.h, again.data());
    expect(blocks == again, "the same output twice");
    for (uint8_t b : blocks) g_digest = (g_digest ^ b) * 0x100000001b3ull;
    std::vector<uint8_t> back(img.rgba.size(), 0x5A);
    nx::nvn::bc_decode(0x4d, blocks.data(), img.w, img.h, 1, back.data());
    double se = 0, sa = 0;
    bool opaque = true, grey = true;
    Result r;
    for (uint64_t i = 0; i < uint64_t(img.w) * img.h; i++) {
        const uint8_t* a = img.rgba.data() + 4 * i;
        const uint8_t* b = back.data() + 4 * i;
        for (int c = 0; c < 3; c++) se += double(a[c] - b[c]) * (a[c] - b[c]);
        sa += double(a[3] - b[3]) * (a[3] - b[3]);
        opaque &= a[3] == 255;
        grey &= a[0] == a[1] && a[1] == a[2];
        if (a[3] == 255 && b[3] != 255) r.alpha_kept_opaque = false;
        if (grey && (std::abs(b[0] - b[1]) > 2 || std::abs(b[1] - b[2]) > 2)) r.grey_kept = false;
    }
    if (!opaque) r.alpha_kept_opaque = true;  // only asked of opaque images
    if (!grey) r.grey_kept = true;
    const double n = double(img.w) * img.h;
    r.psnr_rgb = se == 0 ? 99 : 10 * std::log10(255.0 * 255.0 * 3 * n / se);
    r.psnr_alpha = sa == 0 ? 99 : 10 * std::log10(255.0 * 255.0 * n / sa);
    std::printf("bc7_test: %-28s %4ux%-4u RGB %5.1f dB, alpha %5.1f dB\n", name, img.w, img.h, r.psnr_rgb, r.psnr_alpha);
    return r;
}

int selftest() {
    std::mt19937 rng(7);
    {  // smooth colour gradient, opaque
        Image img(64, 64);
        for (uint32_t y = 0; y < img.h; y++)
            for (uint32_t x = 0; x < img.w; x++) {
                uint8_t* p = img.at(x, y);
                p[0] = uint8_t(x * 4), p[1] = uint8_t(y * 4), p[2] = uint8_t(255 - x * 2 - y), p[3] = 255;
            }
        Result r = round_trip("gradient", img);
        expect(r.psnr_rgb >= 45, "a smooth gradient keeps 45 dB");
        expect(r.alpha_kept_opaque, "an opaque gradient stays opaque");
    }
    {  // grey ramp with noise
        Image img(37, 23);  // not a multiple of 4: edge blocks
        for (uint32_t y = 0; y < img.h; y++)
            for (uint32_t x = 0; x < img.w; x++) {
                const uint8_t v = uint8_t(std::min<int>(255, int(x * 6 + (rng() % 9))));
                uint8_t* p = img.at(x, y);
                p[0] = p[1] = p[2] = v, p[3] = 255;
            }
        Result r = round_trip("grey ramp, noise, 37x23", img);
        expect(r.psnr_rgb >= 38, "a noisy grey ramp keeps 38 dB");
        expect(r.grey_kept, "grey stays grey");
        expect(r.alpha_kept_opaque, "an opaque grey ramp stays opaque");
    }
    {  // hard edges: two colours per block (partitioned modes)
        Image img(64, 64);
        for (uint32_t y = 0; y < img.h; y++)
            for (uint32_t x = 0; x < img.w; x++) {
                const bool in = (x + y) % 11 < 5;
                uint8_t* p = img.at(x, y);
                p[0] = in ? 230 : 20, p[1] = in ? 40 : 200, p[2] = in ? 30 : 60, p[3] = 255;
            }
        Result r = round_trip("hard two-colour edges", img);
        expect(r.psnr_rgb >= 40, "two-colour edges keep 40 dB");
    }
    {  // an anti-aliased disc on full transparency (a UI icon)
        Image img(48, 48);
        for (uint32_t y = 0; y < img.h; y++)
            for (uint32_t x = 0; x < img.w; x++) {
                const double d = std::hypot(x - 23.5, y - 23.5) - 18;
                const double cover = std::clamp(0.5 - d, 0.0, 1.0);
                uint8_t* p = img.at(x, y);
                p[0] = 250, p[1] = 200, p[2] = 30, p[3] = uint8_t(std::lround(cover * 255));
            }
        Result r = round_trip("anti-aliased icon", img);
        expect(r.psnr_rgb >= 45 && r.psnr_alpha >= 40, "an icon keeps 45 dB RGB and 40 dB alpha");
    }
    {  // alpha ramp under varying colour
        Image img(32, 32);
        for (uint32_t y = 0; y < img.h; y++)
            for (uint32_t x = 0; x < img.w; x++) {
                uint8_t* p = img.at(x, y);
                p[0] = uint8_t(x * 8), p[1] = 128, p[2] = uint8_t(y * 8), p[3] = uint8_t(x * 4 + y * 4);
            }
        Result r = round_trip("alpha ramp", img);
        expect(r.psnr_alpha >= 40, "an alpha ramp keeps 40 dB");
    }
    {  // solid blocks of random colours: at most a level off
        Image img(32, 32);
        for (uint32_t by = 0; by < 8; by++)
            for (uint32_t bx = 0; bx < 8; bx++) {
                const uint8_t c[4] = {uint8_t(rng()), uint8_t(rng()), uint8_t(rng()), uint8_t(bx & 1 ? 255 : rng())};
                for (uint32_t y = 0; y < 4; y++)
                    for (uint32_t x = 0; x < 4; x++) std::memcpy(img.at(bx * 4 + x, by * 4 + y), c, 4);
            }
        Result r = round_trip("solid blocks", img);
        expect(r.psnr_rgb >= 48 && r.psnr_alpha >= 48, "solid blocks are within a level or two");
    }
    {  // random noise: no floor beyond not breaking (worst case for any 4x4 code)
        Image img(16, 16);
        for (uint8_t& v : img.rgba) v = uint8_t(rng());
        round_trip("noise", img);
    }
    {  // 1x1, 2x3: smaller than a block
        Image a(1, 1), b(2, 3);
        a.rgba = {10, 20, 30, 255};
        for (uint8_t& v : b.rgba) v = uint8_t(rng() | 1);
        expect(round_trip("1x1", a).psnr_rgb >= 45, "a single texel survives");
        round_trip("2x3", b);
    }
    std::printf("bc7_test: output digest %016llx (encoder version %u)\n", (unsigned long long)g_digest,
                nx::nvn::kBc7EncoderVersion);
    expect(g_digest == kDigest, "the encoder's output changed: bump kBc7EncoderVersion and update kDigest");
    std::printf("bc7_test: %s\n", g_failed ? "FAILED" : "all passed");
    return g_failed ? 1 : 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::strcmp(argv[1], "--selftest") == 0) return selftest();
    std::printf("usage: bc7_test --selftest\n");
    return 2;
}
