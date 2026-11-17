// VP9 decoding on the CPU for the game's movies (Wonder's Switch 2 Edition ships WebM files, VP9 video + Vorbis
// audio, in romfs/UI/Movie). The console decodes VP9 on its video engine through movie::VideoDecoder (subsdk0);
// hle_movie.cpp replaces that class and decodes with libvpx (BSD-3-Clause) here.
//
// libvpx is loaded at run time from a shared library (no link-time dependency, nothing of it in the repository):
// NX_VPX_LIB=<path>, else libvpx-1.dll / libvpx.so.* on the search path, else MSYS2's
// C:\msys64\ucrt64\bin\libvpx-1.dll. Its C ABI (decoder ABI version 12, libvpx 1.8-1.17) is declared here.
//
// Self-contained on purpose (no runtime headers): tests/movie_test.cpp compiles it on its own.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace nx::movie {

// One decoded picture, 8-bit 4:2:0 (I420): plane 0 Y, 1 U, 2 V; the chroma planes are (w + 1) / 2 x (h + 1) / 2.
struct Picture {
    uint32_t width = 0, height = 0;
    const uint8_t* plane[3] = {};
    int stride[3] = {};
};

class Vp9Decoder {
public:
    Vp9Decoder() = default;
    ~Vp9Decoder();
    Vp9Decoder(const Vp9Decoder&) = delete;
    Vp9Decoder& operator=(const Vp9Decoder&) = delete;

    // Loads libvpx (once per process) and creates a VP9 decoder. False, with the reason in *error, if either fails.
    bool open(std::string* error);
    bool is_open() const { return ctx_ != nullptr; }
    // Decodes one compressed frame (a WebM block: a VP9 frame or superframe). Pictures it outputs are then taken
    // with next_picture() until it returns false; they stay valid until the next decode().
    bool decode(const uint8_t* data, size_t size, std::string* error);
    bool next_picture(Picture& out);
    void close();

private:
    void* ctx_ = nullptr;   // vpx_codec_ctx_t
    void* iter_ = nullptr;  // vpx_codec_iter_t
};

// Bytes of a tightly packed NV12 picture: Y rows of `width` bytes, then (height + 1) / 2 rows of interleaved U/V
// pairs, 2 * ((width + 1) / 2) bytes each.
uint64_t nv12_y_size(uint32_t width, uint32_t height);
uint64_t nv12_uv_size(uint32_t width, uint32_t height);

// I420 -> tightly packed NV12 (the layout of movie::VideoFrame buffers the game allocates: width * height * 3 / 2).
// Writes at most dst_size bytes; returns false if the picture does not fit.
bool i420_to_nv12(const Picture& p, uint8_t* dst, uint64_t dst_size);

}  // namespace nx::movie
