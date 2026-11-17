// nn::pl (shared system fonts) at the API level.
//
// On the console the pl sysmodule loads the system fonts (system archives 0100000000000810-814) into shared memory
// in BFTTF form: an 8-byte header {magic, size} and the TrueType data, both XORed with a key. The game hands that
// to nn::fontll::ScalableFontEngineHelper::Decode (sdk+3093c0 in SDK 16), which accepts the data already decoded
// when the first word is the plain magic 0x18029A7F, and then only skips the header.
//
// So any TrueType font works. Per type (0 standard, 1 simplified Chinese, 2 its extension, 3 traditional Chinese,
// 4 Korean, 5 Nintendo extension), in <data root>/fonts/: first the console's own font, as the user dumped it from
// their console (the system archive's file, e.g. nintendo_ext_003.bfttf, XOR-encoded, or the same name with .ttf
// once decoded); else shared_<type>.ttf (for type 5, tools/fonts/make_button_font.py draws a stand-in); else
// shared.ttf; else the host's own (NX_SHARED_FONT, or a system font on macOS, Android and Windows). Wonder (both
// editions) composes its message fonts with nintendo_ext_003.bfttf for the controller button icons (Font.*.bfarc,
// fcpx/MsgFont.bfcpx), so type 5 is the one that decides how the buttons look. None of these files is ever
// committed: they are the user's or derived from the host's fonts.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include <nx/runtime.h>

#include "platform.h"

namespace nx {

namespace {

struct Font {
    uint64_t addr = 0, size = 0;  // header + data, in guest memory
};

std::vector<uint8_t> read_file(const std::string& path) {
    std::vector<uint8_t> v;
    FILE* f = plat::fopen_utf8(path, "rb");
    if (!f) return v;
    plat::fseek64(f, 0, SEEK_END);
    int64_t n = plat::ftell64(f);
    plat::fseek64(f, 0, SEEK_SET);
    if (n > 0) {
        v.resize(size_t(n));
        v.resize(std::fread(v.data(), 1, v.size(), f));
    }
    std::fclose(f);
    return v;
}

// The system archives' file names per type (what the console's pl loads).
const char* const kSystemNames[6] = {"nintendo_udsg-r_std_003", "nintendo_udsg-r_org_zh-cn_003",
                                      "nintendo_udsg-r_ext_zh-cn_003", "nintendo_udjxh-db_zh-tw_003",
                                      "nintendo_udsg-r_ko_003", "nintendo_ext_003"};

// A .bfttf as the system archive holds it: 32-bit words XORed with a key, the first one decoding to the plain magic
// 0x18029A7F and the second to the size of the TrueType data that follows. Decoded in place to that data; the key is
// derived from the magic and the result checked (a TrueType or OpenType header, a size that fits). Either byte order
// of the decoded words is tried, as files from different tools differ.
bool decode_bfttf(std::vector<uint8_t>& v) {
    if (v.size() < 16 || v.size() % 4) return false;
    auto word = [&](size_t i) { uint32_t w; std::memcpy(&w, v.data() + 4 * i, 4); return w; };
    auto bswap = [](uint32_t w) { return (w >> 24) | (w >> 8 & 0xFF00) | (w << 8 & 0xFF0000) | (w << 24); };
    auto ttf_header = [](const uint8_t* p) {
        return (p[0] == 0 && p[1] == 1 && p[2] == 0 && p[3] == 0) || std::memcmp(p, "OTTO", 4) == 0 ||
               std::memcmp(p, "true", 4) == 0 || std::memcmp(p, "ttcf", 4) == 0;
    };
    for (int swapped = 0; swapped < 2; swapped++) {
        const uint32_t key = word(0) ^ (swapped ? bswap(0x18029A7Fu) : 0x18029A7Fu);
        auto dec = [&](uint32_t w) { uint32_t x = w ^ key; return swapped ? bswap(x) : x; };
        // The size word: in the words' own order, or big-endian (the console's nintendo_ext_003.bfttf: 0x0002C00C
        // stored as 0x0CC00200, its 180236 bytes of data).
        uint32_t size = dec(word(1));
        if (size == 0 || size > v.size() - 8) size = bswap(size);
        if (size == 0 || size > v.size() - 8) continue;
        uint32_t head = dec(word(2));
        uint8_t hb[4];
        std::memcpy(hb, &head, 4);
        if (!ttf_header(hb)) continue;
        std::vector<uint8_t> out(size);
        for (size_t i = 0; i < (size_t(size) + 3) / 4; i++) {
            uint32_t x = dec(word(2 + i));
            std::memcpy(out.data() + 4 * i, &x, std::min<size_t>(4, size - 4 * i));
        }
        v.swap(out);
        return true;
    }
    return false;
}

const Font& font(uint32_t type) {
    static std::mutex lock;
    static Font fonts[6];
    std::lock_guard lk(lock);
    Font& f = fonts[type < 6 ? type : 0];
    if (f.addr) return f;
    std::string root = plat::data_root() + "/fonts/";
    std::vector<uint8_t> ttf;
    // The console's own font first (the user's dump): encoded .bfttf, or decoded .ttf.
    const std::string sys = root + kSystemNames[type < 6 ? type : 0];
    if (ttf = read_file(sys + ".bfttf"); !ttf.empty()) {
        const size_t raw = ttf.size();
        if (decode_bfttf(ttf)) {
            log("pl: shared font %u <- %s.bfttf (the console's own, %zu bytes decoded)", type, sys.c_str(), ttf.size());
        } else {
            log("pl: %s.bfttf (%zu bytes) is not an encoded font this runtime knows: not used", sys.c_str(), raw);
            ttf.clear();
        }
    } else if (ttf = read_file(sys + ".ttf"); !ttf.empty()) {
        log("pl: shared font %u <- %s.ttf (the console's own, %zu bytes)", type, sys.c_str(), ttf.size());
    }
    std::vector<std::string> candidates = {root + "shared_" + std::to_string(type) + ".ttf", root + "shared.ttf"};
    if (std::string e; plat::env_utf8("NX_SHARED_FONT", e)) candidates.push_back(e);
#if defined(__APPLE__)
    candidates.push_back("/System/Library/Fonts/Supplemental/Arial Unicode.ttf");
    candidates.push_back("/System/Library/Fonts/Supplemental/Arial.ttf");
#elif defined(__ANDROID__)
    candidates.push_back("/system/fonts/Roboto-Regular.ttf");
#elif defined(_WIN32)
    // Arial Unicode (Office installs it) covers the most; else Arial, as on the Mac.
    candidates.push_back("C:/Windows/Fonts/arialuni.ttf");
    candidates.push_back("C:/Windows/Fonts/arial.ttf");
#endif
    if (!ttf.empty()) candidates.clear();
    for (const auto& path : candidates) {
        ttf = read_file(path);
        if (!ttf.empty()) {
            log("pl: shared font %u <- %s (%zu bytes)", type, path.c_str(), ttf.size());
            break;
        }
    }
    if (ttf.empty()) log("pl: no font for shared font %u (put one in %s)", type, root.c_str());
    f.size = ttf.size() + 8;
    f.addr = kernel_alloc(f.size);
    st<uint32_t>(f.addr, 0x18029A7F);  // plain magic: "already decoded"
    st<uint32_t>(f.addr + 4, uint32_t(ttf.size()));
    if (!ttf.empty()) std::memcpy(reinterpret_cast<void*>(f.addr + 8), ttf.data(), ttf.size());
    return f;
}

void GetSharedFontLoadState(CPU& c) { c.x[0] = 1; }  // SharedFontLoadState_Loaded
void GetSharedFontAddress(CPU& c) { c.x[0] = font(uint32_t(c.x[0])).addr; }
void GetSharedFontSize(CPU& c) { c.x[0] = font(uint32_t(c.x[0])).size; }

// ---- SDK 21 (Wonder, Switch 2 Edition) --------------------------------------------------------------------------
// RequestLoadNxFont(void* buffer, size_t size): the application lends pl a buffer (Wonder: 0x1300000 bytes) for the
// Switch fonts. Ours live in our own memory, so the buffer is not needed.
void RequestLoadNxFont(CPU& c) {
    log("pl: RequestLoadNxFont(%llx, %llx)", (unsigned long long)c.x[0], (unsigned long long)c.x[1]);
    c.x[0] = 0;
}

// bool GetBuiltInFontData(BuiltInFontData{Nx,Ounce}* out, type, bool): out is 0x18 bytes, two flag bytes at +0 and
// +1 (what the reply's first word holds; Wonder does not read them), the font's address at +8, its size at +0x10
// (sdk+0x599710 and sdk+0x599350 in 21.4.1). Wonder asks for types 0-5, the same order as SharedFontType, and retries
// while the answer is false.
void GetBuiltInFontData(CPU& c) {
    uint64_t out = c.x[0];
    uint32_t type = uint32_t(c.x[1]);
    const Font& f = font(type);
    std::memset(reinterpret_cast<void*>(out), 0, 0x18);
    st<uint64_t>(out + 8, f.addr);
    st<uint64_t>(out + 0x10, f.size);
    c.x[0] = 1;
}

}  // namespace

NX_HLE("_ZN2nn2pl22GetSharedFontLoadStateENS0_14SharedFontTypeE", GetSharedFontLoadState);
NX_HLE("_ZN2nn2pl20GetSharedFontAddressENS0_14SharedFontTypeE", GetSharedFontAddress);
NX_HLE("_ZN2nn2pl17GetSharedFontSizeENS0_14SharedFontTypeE", GetSharedFontSize);
NX_HLE("_ZN2nn2pl17RequestLoadNxFontEPvm", RequestLoadNxFont);
NX_HLE("_ZN2nn2pl18GetBuiltInFontDataEPNS0_17BuiltInFontDataNxENS0_17BuiltInFontTypeNxEb", GetBuiltInFontData);
NX_HLE("_ZN2nn2pl18GetBuiltInFontDataEPNS0_20BuiltInFontDataOunceENS0_20BuiltInFontTypeOunceEb", GetBuiltInFontData);

}  // namespace nx
