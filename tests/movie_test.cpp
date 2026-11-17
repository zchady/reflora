// VP9 movie decoding test (runtime/src/movie_vpx.cpp). Standalone:
//   clang++ -std=c++20 -O2 -Iruntime/src tests/movie_test.cpp runtime/src/movie_vpx.cpp -o <out>
//
//   movie_test --selftest
//       I420 -> NV12 packing on a synthetic picture (no libvpx needed).
//   movie_test <in.ivf> <outdir> [every]
//       decodes every frame of an IVF file (tools/webm_to_ivf.py extracts one from the game's WebM movies) with
//       libvpx, packs each picture into an NV12 buffer of width * height * 3 / 2 bytes as hle_movie.cpp does for
//       movie::VideoFrame, and writes every `every`-th (default 120) as <outdir>/frame_NNNNN.ppm (BT.601 limited
//       range, the usual convention for these streams). Fails on any decode error or a picture of another size.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "movie_vpx.h"

using namespace nx::movie;

namespace {

int selftest() {
    // 6x4 picture: Y = x + 10 * y, U = 100 + x, V = 200 + y (chroma 3x2)
    uint8_t y[4][6], u[2][3], v[2][3];
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 6; c++) y[r][c] = uint8_t(c + 10 * r);
    for (int r = 0; r < 2; r++)
        for (int c = 0; c < 3; c++) u[r][c] = uint8_t(100 + c), v[r][c] = uint8_t(200 + r);
    Picture p;
    p.width = 6, p.height = 4;
    p.plane[0] = &y[0][0], p.plane[1] = &u[0][0], p.plane[2] = &v[0][0];
    p.stride[0] = 6, p.stride[1] = 3, p.stride[2] = 3;
    std::vector<uint8_t> out(6 * 4 * 3 / 2, 0xEE);
    int failed = 0;
    auto expect = [&](bool ok, const char* what) {
        if (!ok) std::printf("FAIL: %s\n", what), failed++;
    };
    expect(nv12_y_size(6, 4) == 24 && nv12_uv_size(6, 4) == 12, "plane sizes");
    expect(i420_to_nv12(p, out.data(), out.size()), "fits");
    expect(out[0] == 0 && out[5] == 5 && out[6] == 10 && out[23] == 35, "luma rows copied");
    expect(out[24] == 100 && out[25] == 200 && out[28] == 102 && out[29] == 200, "first chroma row interleaved U, V");
    expect(out[30] == 100 && out[31] == 201 && out[35] == 201, "second chroma row");
    expect(!i420_to_nv12(p, out.data(), out.size() - 1), "refuses a short buffer");
    std::printf("movie_test selftest: %s\n", failed ? "FAILED" : "all passed");
    return failed ? 1 : 0;
}

uint8_t clamp8(int v) { return uint8_t(v < 0 ? 0 : v > 255 ? 255 : v); }

void write_ppm(const std::string& path, const uint8_t* nv12, uint32_t w, uint32_t h) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return;
    std::fprintf(f, "P6\n%u %u\n255\n", w, h);
    const uint8_t* uv = nv12 + uint64_t(w) * h;
    std::vector<uint8_t> row(w * 3);
    for (uint32_t y = 0; y < h; y++) {
        for (uint32_t x = 0; x < w; x++) {
            int Y = nv12[uint64_t(y) * w + x] - 16;
            const uint8_t* c = uv + uint64_t(y / 2) * ((w + 1) / 2) * 2 + (x / 2) * 2;
            int U = c[0] - 128, V = c[1] - 128;
            row[x * 3 + 0] = clamp8((298 * Y + 409 * V + 128) >> 8);
            row[x * 3 + 1] = clamp8((298 * Y - 100 * U - 208 * V + 128) >> 8);
            row[x * 3 + 2] = clamp8((298 * Y + 516 * U + 128) >> 8);
        }
        std::fwrite(row.data(), 1, row.size(), f);
    }
    std::fclose(f);
}

int decode_file(const char* path, const std::string& outdir, int every) {
    FILE* f = std::fopen(path, "rb");
    if (!f) return std::printf("cannot open %s\n", path), 2;
    uint8_t hdr[32];
    if (std::fread(hdr, 1, 32, f) != 32 || std::memcmp(hdr, "DKIF", 4) != 0) return std::printf("not IVF\n"), 2;
    uint32_t w = hdr[12] | hdr[13] << 8, h = hdr[14] | hdr[15] << 8;
    Vp9Decoder dec;
    std::string err;
    if (!dec.open(&err)) return std::printf("open: %s\n", err.c_str()), 1;
    std::vector<uint8_t> nv12(uint64_t(w) * h * 3 / 2), data;
    int frames = 0, pictures = 0, bad = 0;
    double ms = 0;
    uint8_t fh[12];
    while (std::fread(fh, 1, 12, f) == 12) {
        uint32_t size = fh[0] | fh[1] << 8 | fh[2] << 16 | uint32_t(fh[3]) << 24;
        data.resize(size);
        if (std::fread(data.data(), 1, size, f) != size) break;
        auto t0 = std::chrono::steady_clock::now();
        if (!dec.decode(data.data(), size, &err)) {
            std::printf("frame %d: %s\n", frames, err.c_str());
            bad++;
        }
        Picture p;
        while (dec.next_picture(p)) {
            if (p.width != w || p.height != h || !i420_to_nv12(p, nv12.data(), nv12.size())) {
                std::printf("picture %d: %ux%u, expected %ux%u\n", pictures, p.width, p.height, w, h);
                bad++;
            } else if (every > 0 && pictures % every == 0) {
                char name[64];
                std::snprintf(name, sizeof name, "/frame_%05d.ppm", pictures);
                write_ppm(outdir + name, nv12.data(), w, h);
            }
            pictures++;
        }
        ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        frames++;
    }
    std::fclose(f);
    std::printf("%d frames, %d pictures %ux%u, %d errors, %.2f ms per frame (decode + NV12)\n", frames, pictures, w,
                h, bad, frames ? ms / frames : 0.0);
    return bad || !pictures ? 1 : 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::strcmp(argv[1], "--selftest") == 0) return selftest();
    if (argc == 3 || argc == 4) return decode_file(argv[1], argv[2], argc == 4 ? std::atoi(argv[3]) : 120);
    std::printf("usage: movie_test --selftest | <in.ivf> <outdir> [every]\n");
    return 2;
}
