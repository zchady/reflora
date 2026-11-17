// libvpx VP9 decoding, loaded at run time (movie_vpx.h).
#include "movie_vpx.h"

#include <cstdlib>
#include <cstring>
#include <mutex>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace nx::movie {

namespace {

// ---- the slice of the libvpx C API used here (vpx/vpx_codec.h, vpx_decoder.h, vpx_image.h, vp8dx.h) ----------
// Decoder ABI version = 3 + codec ABI (4 + image ABI 5) = 12.
constexpr int kVpxDecoderAbiVersion = 12;
constexpr int kVpxImgFmtI420 = 0x102;        // VPX_IMG_FMT_PLANAR | 2
constexpr int kVpxImgFmtHighBitDepth = 0x800;

struct VpxImage {
    int fmt, cs, range;
    unsigned w, h, bit_depth, d_w, d_h, r_w, r_h, x_chroma_shift, y_chroma_shift;
    unsigned char* planes[4];
    int stride[4];
    int bps;
    void* user_priv;
    unsigned char* img_data;
    int img_data_owner, self_allocd;
    void* fb_priv;
};

struct VpxCodecCtx {  // vpx_codec_ctx_t
    const char* name;
    void* iface;
    int err;
    const char* err_detail;
    long init_flags;
    const void* config;
    void* priv;
};

struct VpxDecCfg {
    unsigned threads, w, h;
};

struct Api {
    void* (*vp9_dx)() = nullptr;
    int (*dec_init_ver)(VpxCodecCtx*, void*, const VpxDecCfg*, long, int) = nullptr;
    int (*decode)(VpxCodecCtx*, const uint8_t*, unsigned, void*, long) = nullptr;
    VpxImage* (*get_frame)(VpxCodecCtx*, void**) = nullptr;
    int (*destroy)(VpxCodecCtx*) = nullptr;
    const char* (*err_to_string)(int) = nullptr;
    std::string error;
    bool ok = false;
};

void* open_library(std::string& tried) {
    const char* env = std::getenv("NX_VPX_LIB");
#ifdef _WIN32
    const char* names[] = {env, "libvpx-1.dll", "vpx.dll", "C:\\msys64\\ucrt64\\bin\\libvpx-1.dll",
                           "C:\\msys64\\mingw64\\bin\\libvpx-1.dll", "C:\\msys64\\clang64\\bin\\libvpx-1.dll"};
    for (const char* n : names) {
        if (!n || !*n) continue;
        tried += std::string(tried.empty() ? "" : ", ") + n;
        // A full path loads its own dependencies (libwinpthread, ...) from its folder.
        DWORD flags = std::strchr(n, '\\') || std::strchr(n, '/') ? LOAD_WITH_ALTERED_SEARCH_PATH : 0;
        if (HMODULE h = LoadLibraryExA(n, nullptr, flags)) return h;
    }
#else
    const char* names[] = {env, "libvpx.so.9", "libvpx.so.8", "libvpx.so.7", "libvpx.so", "libvpx.dylib"};
    for (const char* n : names) {
        if (!n || !*n) continue;
        tried += std::string(tried.empty() ? "" : ", ") + n;
        if (void* h = dlopen(n, RTLD_NOW)) return h;
    }
#endif
    return nullptr;
}

void* symbol(void* lib, const char* name) {
#ifdef _WIN32
    return reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(lib), name));
#else
    return dlsym(lib, name);
#endif
}

const Api& api() {
    static Api a;
    static std::once_flag once;
    std::call_once(once, [] {
        std::string tried;
        void* lib = open_library(tried);
        if (!lib) {
            a.error = "libvpx not found (tried " + tried + "; set NX_VPX_LIB)";
            return;
        }
        a.vp9_dx = reinterpret_cast<void* (*)()>(symbol(lib, "vpx_codec_vp9_dx"));
        a.dec_init_ver = reinterpret_cast<decltype(a.dec_init_ver)>(symbol(lib, "vpx_codec_dec_init_ver"));
        a.decode = reinterpret_cast<decltype(a.decode)>(symbol(lib, "vpx_codec_decode"));
        a.get_frame = reinterpret_cast<decltype(a.get_frame)>(symbol(lib, "vpx_codec_get_frame"));
        a.destroy = reinterpret_cast<decltype(a.destroy)>(symbol(lib, "vpx_codec_destroy"));
        a.err_to_string = reinterpret_cast<decltype(a.err_to_string)>(symbol(lib, "vpx_codec_err_to_string"));
        a.ok = a.vp9_dx && a.dec_init_ver && a.decode && a.get_frame && a.destroy;
        if (!a.ok) a.error = "libvpx lacks the decoder entry points";
    });
    return a;
}

std::string vpx_error(const Api& a, VpxCodecCtx* c, int err) {
    std::string s = a.err_to_string ? a.err_to_string(err) : ("error " + std::to_string(err));
    if (c && c->err_detail) s += std::string(": ") + c->err_detail;
    return s;
}

}  // namespace

Vp9Decoder::~Vp9Decoder() { close(); }

bool Vp9Decoder::open(std::string* error) {
    close();
    const Api& a = api();
    if (!a.ok) {
        if (error) *error = a.error;
        return false;
    }
    auto* c = new VpxCodecCtx{};
    VpxDecCfg cfg{2, 0, 0};  // two threads: 960x540 frames decode in a few ms either way
    int err = a.dec_init_ver(c, a.vp9_dx(), &cfg, 0, kVpxDecoderAbiVersion);
    if (err) {
        if (error) *error = "vpx_codec_dec_init: " + vpx_error(a, c, err);
        delete c;
        return false;
    }
    ctx_ = c;
    return true;
}

bool Vp9Decoder::decode(const uint8_t* data, size_t size, std::string* error) {
    const Api& a = api();
    auto* c = static_cast<VpxCodecCtx*>(ctx_);
    iter_ = nullptr;
    if (!c) {
        if (error) *error = "decoder not open";
        return false;
    }
    int err = a.decode(c, data, unsigned(size), nullptr, 0);
    if (err) {
        if (error) *error = vpx_error(a, c, err);
        return false;
    }
    return true;
}

bool Vp9Decoder::next_picture(Picture& out) {
    auto* c = static_cast<VpxCodecCtx*>(ctx_);
    if (!c) return false;
    for (;;) {
        VpxImage* img = api().get_frame(c, &iter_);
        if (!img) return false;
        if (img->fmt != kVpxImgFmtI420 || (img->fmt & kVpxImgFmtHighBitDepth)) continue;  // only 8-bit 4:2:0
        out.width = img->d_w;
        out.height = img->d_h;
        for (int k = 0; k < 3; k++) out.plane[k] = img->planes[k], out.stride[k] = img->stride[k];
        return true;
    }
}

void Vp9Decoder::close() {
    if (auto* c = static_cast<VpxCodecCtx*>(ctx_)) {
        api().destroy(c);
        delete c;
    }
    ctx_ = nullptr;
    iter_ = nullptr;
}

uint64_t nv12_y_size(uint32_t width, uint32_t height) { return uint64_t(width) * height; }

uint64_t nv12_uv_size(uint32_t width, uint32_t height) {
    return uint64_t((width + 1) / 2) * 2 * ((height + 1) / 2);
}

bool i420_to_nv12(const Picture& p, uint8_t* dst, uint64_t dst_size) {
    const uint32_t w = p.width, h = p.height, cw = (w + 1) / 2, ch = (h + 1) / 2;
    if (!dst || nv12_y_size(w, h) + nv12_uv_size(w, h) > dst_size) return false;
    for (uint32_t y = 0; y < h; y++) std::memcpy(dst + uint64_t(y) * w, p.plane[0] + int64_t(y) * p.stride[0], w);
    uint8_t* uv = dst + nv12_y_size(w, h);
    for (uint32_t y = 0; y < ch; y++) {
        const uint8_t* u = p.plane[1] + int64_t(y) * p.stride[1];
        const uint8_t* v = p.plane[2] + int64_t(y) * p.stride[2];
        uint8_t* d = uv + uint64_t(y) * cw * 2;
        for (uint32_t x = 0; x < cw; x++) d[2 * x] = u[x], d[2 * x + 1] = v[x];
    }
    return true;
}

}  // namespace nx::movie
