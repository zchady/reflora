// NX_ASTC_BC7 (nvn_astc_bc7.h): the persistent cache of ASTC -> BC7 conversions and their quality check.
//
// Cache: one file per texture content in NX_ASTC_BC7_CACHE (default <data root>/astc_bc7_cache), named after the key
// and the versions of the ASTC decoder, the BC7 encoder and the check, so a change of any of them starts a new set of
// names (the old files are then unused). "<key>.bc7": an accepted conversion (header, levels, blocks, checksum of the
// blocks); "<key>.rgba8": a rejected one (a text line: why), so it is not tried again. Each file is written under a
// temporary name and renamed when complete, so an interrupted write never leaves a file that passes for a verdict;
// a .bc7 whose header or checksum does not match is not used (the texture stays RGBA8). The cache holds data
// derived from the user's own game files: it stays on this machine and is never published.
//
// Quality check, per level (>= 8 px) and layer, of the BC7 decode (nvn_bc.cpp's decoder) against the RGBA8 decode of
// the ASTC data (what is shown without BC7). A texture is kept RGBA8 unless every check passes:
//   - PSNR of RGB over visible texels (weighted by alpha, as blended) and over all texels (additive effects show RGB
//     under alpha 0), PSNR of alpha;
//   - the worst 4x4 block (RMS of RGB weighted by alpha): one wrong-coloured block on an edge is what an average
//     hides;
//   - alpha-test flips: texels clearly on one side of 128 (16 levels or more away) decoded on the other side;
//   - an opaque source stays exactly opaque, a grey source (R = G = B) stays grey (at most 2 levels of colour).
// UI-like textures (sRGB ASTC 4x4 without mipmaps: UI/ and Layout/) and small ones (64 px or less: icons) get the
// strict limits; the rest the general ones. Mipmaps under 32 px (not level 0) only get the gross checks. Limits: see
// judge(). NX_ASTC_BC7_DB=<dB>: the general PSNR limit (35; measurement only, it is part of the file names).
#include "nvn_astc_bc7.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#include "nvn.h"
#include "nvn_astc.h"
#include "nvn_astc_cache.h"
#include "nvn_bc7_encode.h"
#include "nvn_vk.h"
#include "platform.h"

#ifdef _WIN32
#include <windows.h>
#endif

namespace nx::nvn {

using namespace nx::vk;

namespace {

constexpr char kMagic[8] = {'R', 'F', 'L', 'B', 'C', '7', 1, 0};
// Bumped whenever the quality check changes: part of the file names, so verdicts of another check are not reused.
constexpr uint32_t kGateVersion = 2;
constexpr double kDefaultDb = 35.0;

double base_db() {
    static const double db = [] {
        const char* e = std::getenv("NX_ASTC_BC7_DB");
        const double v = e ? std::atof(e) : kDefaultDb;
        return v >= 20 && v <= 60 ? v : kDefaultDb;
    }();
    return db;
}

bool g_on = false, g_open = false;
std::string g_dir;
std::mutex g_index_lock;
std::unordered_map<std::string, Bc7Decision> g_index;  // key name -> verdict (files there, and those written now)

struct Stats {
    std::atomic<uint64_t> images_bc7{0}, images_rgba8{0}, hash_ns{0};
    std::atomic<uint64_t> built{0}, accepted{0}, rejected{0}, write_failed{0}, build_ns{0}, build_texels{0};
    std::atomic<uint64_t> encode_ns{0}, check_ns{0};
    std::atomic<uint64_t> loaded{0}, load_ns{0}, load_bytes{0}, bad_files{0};
    std::atomic<uint64_t> up_memory{0}, up_disk{0}, up_bytes{0}, up_ns{0};
    std::atomic<uint64_t> copies{0}, texel_writes{0}, views{0}, changed{0}, unloadable{0}, other{0};
    std::mutex reasons_lock;
    std::unordered_map<std::string, uint64_t> reasons;
} g;

struct FileHeader {
    char magic[8];
    uint32_t encoder, format, width, height, layers, levels, bh0, level_count;
    uint64_t hash, tiled, payload, payload_hash;
    float worst_visible, worst_all, worst_alpha, worst_block;  // the quality measured (dB, dB, dB, RMS levels)
};
struct FileLevel {
    uint64_t offset, layer_size;
    uint32_t width, height;
};

double psnr(double se, double n) { return n == 0 || se == 0 ? 99.0 : 10.0 * std::log10(255.0 * 255.0 * n / se); }

struct Verdict {
    bool ok = true;
    std::string why;
    float worst_visible = 99, worst_all = 99, worst_alpha = 99, worst_block = 0;
};

// One level and layer of the BC7 decode `got` against the source `want`. strict: UI and icons; full: level 0 or a
// mipmap of 32 px or more.
void judge(const uint8_t* want, const uint8_t* got, uint32_t w, uint32_t h, bool strict, bool full, Verdict& v) {
    const double base = base_db();
    const double lim_visible = strict ? base + 5 : base, lim_all = strict ? base + 1 : base - 3;
    const double lim_alpha = strict ? base + 5 : base, lim_block = strict ? 8.0 : 12.0;
    const uint64_t n = uint64_t(w) * h;
    double se_v = 0, se_all = 0, se_a = 0, nv = 0;  // visible: RGB error weighted by alpha (as blended over a background)
    uint64_t flips = 0;
    bool opaque = true, grey = true, got_opaque = true;
    int chroma = 0;
    for (uint64_t i = 0; i < n; i++) {
        const uint8_t* a = want + 4 * i;
        const uint8_t* b = got + 4 * i;
        double s = 0;
        for (int c = 0; c < 3; c++) s += double(int(a[c]) - b[c]) * (int(a[c]) - b[c]);
        se_all += s;
        se_v += s * a[3] / 255.0, nv += a[3] / 255.0;
        se_a += double(int(a[3]) - b[3]) * (int(a[3]) - b[3]);
        if (std::abs(int(a[3]) - 128) >= 16 && (a[3] >= 128) != (b[3] >= 128)) flips++;
        opaque &= a[3] == 255, got_opaque &= b[3] == 255;
        grey &= a[0] == a[1] && a[1] == a[2];
        chroma = std::max({chroma, std::abs(int(b[0]) - b[1]), std::abs(int(b[1]) - b[2])});
    }
    double worst_block = 0;
    for (uint32_t by = 0; by < (h + 3) / 4; by++)
        for (uint32_t bx = 0; bx < (w + 3) / 4; bx++) {
            double s = 0, m = 0;
            for (uint32_t y = by * 4; y < std::min(h, by * 4 + 4); y++)
                for (uint32_t x = bx * 4; x < std::min(w, bx * 4 + 4); x++) {
                    const uint8_t* a = want + 4 * (uint64_t(y) * w + x);
                    const uint8_t* b = got + 4 * (uint64_t(y) * w + x);
                    const double wt = a[3] / 255.0;
                    for (int c = 0; c < 3; c++) s += wt * double(int(a[c]) - b[c]) * (int(a[c]) - b[c]);
                    m += 3 * wt;
                }
            if (m >= 3) worst_block = std::max(worst_block, std::sqrt(s / m));  // at least one opaque texel's worth
        }
    const float pv = float(psnr(se_v, double(nv) * 3)), pall = float(psnr(se_all, double(n) * 3));
    const float pa = float(psnr(se_a, double(n)));
    v.worst_visible = std::min(v.worst_visible, pv), v.worst_all = std::min(v.worst_all, pall);
    v.worst_alpha = std::min(v.worst_alpha, pa), v.worst_block = std::max(v.worst_block, float(worst_block));
    auto fail = [&](const char* why) {
        if (v.ok) v.ok = false, v.why = why;
    };
    if (opaque && !got_opaque) fail("opaque source decoded with alpha");
    if (grey && chroma > 2) fail("grey source decoded with colour");
    if (w < 8 || h < 8) return;  // the smallest mipmaps: only the two checks above
    if (!full) {  // mipmaps under 32 px (not level 0): seen only far away; gross errors only
        if (pall < 28) fail("RGB of a small mipmap below 28 dB");
        if (pa < 28) fail("alpha of a small mipmap below 28 dB");
        return;
    }
    if (pv < lim_visible) fail(strict ? "visible RGB below the strict limit" : "visible RGB below the limit");
    if (pall < lim_all) fail("RGB of all texels below the limit");
    if (pa < lim_alpha) fail("alpha below the limit");
    if (worst_block > lim_block) fail(strict ? "a block above the strict limit" : "a block above the limit");
    if (flips > (strict ? 0 : n / 2000)) fail("alpha-test flips");
}

// `bytes` to `path`, written under a temporary name and renamed into place once complete.
bool write_file(const std::string& path, const void* a, uint64_t na, const void* b, uint64_t nb) {
    static std::atomic<uint64_t> serial{0};
    char suffix[64];
    std::snprintf(suffix, sizeof suffix, ".%llu.%llu.tmp", (unsigned long long)plat::monotonic_ns(),
                  (unsigned long long)serial++);
    const std::string tmp = path + suffix;
    FILE* f = plat::fopen_utf8(tmp, "wbx");
    if (!f) return false;
    bool ok = std::fwrite(a, 1, na, f) == na && (!nb || std::fwrite(b, 1, nb, f) == nb);
    ok &= std::fflush(f) == 0;
    ok &= std::fclose(f) == 0;
    if (ok) ok = plat::rename_utf8(tmp, path);
    if (!ok) {
        std::error_code ec;
        std::filesystem::remove(std::filesystem::u8path(tmp), ec);  // this function's own temporary file
    }
    return ok;
}

std::atomic<uint64_t>& counter_for(const char* what) {
    return !std::strcmp(what, "copy")                    ? g.copies
           : !std::strncmp(what, "texel writes", 12)     ? g.texel_writes
           : !std::strcmp(what, "view")                  ? g.views
           : !std::strcmp(what, "bytes changed")         ? g.changed
           : !std::strcmp(what, "no conversion to load") ? g.unloadable
                                                         : g.other;
}

}  // namespace

bool astc_bc7_on() { return g_on; }
const std::string& astc_bc7_cache_dir() { return g_dir; }

std::string astc_bc7_name(const Bc7Key& k) {
    char b[200];
    int n = std::snprintf(b, sizeof b, "%016llx-%llx-%02x-%ux%ux%u-%u-%u-a%u-e%u-g%u", (unsigned long long)k.hash,
                          (unsigned long long)k.tiled, k.format, k.width, k.height, k.layers, k.levels, k.bh0,
                          kAstcDecoderVersion, kBc7EncoderVersion, kGateVersion);
    if (base_db() != kDefaultDb && n > 0 && n < int(sizeof b))
        std::snprintf(b + n, sizeof b - n, "-db%d", int(std::lround(base_db() * 10)));
    return b;
}

bool astc_bc7_open_cache() {
    if (g_open) return true;
    std::string dir;
    if (!plat::env_utf8("NX_ASTC_BC7_CACHE", dir) || dir.empty()) dir = plat::data_root() + "/astc_bc7_cache";
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::u8path(dir), ec);
    if (!std::filesystem::is_directory(std::filesystem::u8path(dir), ec)) {
        log("nvn/vk: NX_ASTC_BC7: cannot use the cache folder %s", dir.c_str());
        return false;
    }
    g_dir = dir;
    const uint64_t t0 = plat::monotonic_ns();
    uint64_t bc7 = 0, rgba8 = 0;
    {
        std::lock_guard lk(g_index_lock);
        for (auto& f : std::filesystem::directory_iterator(std::filesystem::u8path(g_dir), ec)) {
            const std::string name = f.path().filename().string();  // the cache's names are ASCII
            const auto dot = name.rfind('.');
            if (dot == std::string::npos) continue;
            const std::string ext = name.substr(dot);
            if (ext == ".bc7") g_index[name.substr(0, dot)] = Bc7Decision::kBc7, bc7++;
            if (ext == ".rgba8") g_index[name.substr(0, dot)] = Bc7Decision::kRgba8, rgba8++;
        }
    }
    g_open = true;
    log("nvn/vk: NX_ASTC_BC7: cache %s: %llu files, %llu accepted, %llu kept RGBA8 (listed in %.1f ms); versions: ASTC "
        "decoder %u, BC7 encoder %u, check %u",
        g_dir.c_str(), (unsigned long long)(bc7 + rgba8), (unsigned long long)bc7, (unsigned long long)rgba8,
        double(plat::monotonic_ns() - t0) / 1e6, kAstcDecoderVersion, kBc7EncoderVersion, kGateVersion);
    return true;
}

void astc_bc7_init(bool bc_feature) {
#ifdef _WIN32
    const char* e = std::getenv("NX_ASTC_BC7");
    if (!e || e[0] != '1') return;
    if (astc_native()) return log("nvn/vk: NX_ASTC_BC7: ASTC is native on this GPU: nothing to convert");
    bool ok = bc_feature;
    for (VkFormat f : {VK_FORMAT_BC7_UNORM_BLOCK, VK_FORMAT_BC7_SRGB_BLOCK}) {
        VkFormatProperties fp{};
        p_vkGetPhysicalDeviceFormatProperties(vk_physical(), f, &fp);
        const VkFormatFeatureFlags need = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
                                          VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT |
                                          VK_FORMAT_FEATURE_TRANSFER_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT;
        ok &= (fp.optimalTilingFeatures & need) == need;
        VkImageFormatProperties ip{};
        for (VkImageCreateFlags flags : {VkImageCreateFlags(0), VkImageCreateFlags(VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT)})
            ok &= p_vkGetPhysicalDeviceImageFormatProperties(
                      vk_physical(), f, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL,
                      VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                      flags, &ip) == VK_SUCCESS;
    }
    if (!ok) return log("nvn/vk: NX_ASTC_BC7: this GPU cannot sample BC7 as needed: ASTC stays RGBA8");
    if (!astc_bc7_open_cache()) return;
    g_on = true;
    log("nvn/vk: NX_ASTC_BC7: ASTC textures as BC7 where the cache holds an accepted conversion");
#else
    (void)bc_feature;
#endif
}

Bc7Decision astc_bc7_lookup(const Bc7Key& k) {
    if (!g_open) return Bc7Decision::kUnknown;
    const std::string name = astc_bc7_name(k);
    std::lock_guard lk(g_index_lock);
    auto it = g_index.find(name);
    return it == g_index.end() ? Bc7Decision::kUnknown : it->second;
}

bool astc_bc7_load(const Bc7Key& k, std::vector<uint8_t>& blocks, std::vector<LinearLevel>& levels) {
    const uint64_t t0 = plat::monotonic_ns();
    const std::string name = astc_bc7_name(k);
    const std::string path = g_dir + "/" + name + ".bc7";
    FILE* f = plat::fopen_utf8(path, "rb");
    if (!f) return false;
    FileHeader h{};
    bool ok = std::fread(&h, sizeof h, 1, f) == 1 && std::memcmp(h.magic, kMagic, 8) == 0 &&
              h.encoder == kBc7EncoderVersion && h.hash == k.hash && h.tiled == k.tiled && h.format == k.format &&
              h.width == k.width && h.height == k.height && h.layers == k.layers && h.levels == k.levels &&
              h.bh0 == k.bh0 && h.level_count == k.levels && h.payload < (1ull << 32);
    levels.clear();
    for (uint32_t i = 0; ok && i < h.level_count; i++) {
        FileLevel fl{};
        ok = std::fread(&fl, sizeof fl, 1, f) == 1;
        LinearLevel l;
        l.offset = fl.offset, l.layer_size = fl.layer_size, l.width = fl.width, l.height = fl.height;
        l.width_blocks = (fl.width + 3) / 4, l.height_blocks = (fl.height + 3) / 4;
        ok &= l.width == std::max(1u, k.width >> i) && l.height == std::max(1u, k.height >> i) &&
              l.layer_size == uint64_t(l.width_blocks) * l.height_blocks * 16 &&
              l.offset + l.layer_size * k.layers <= h.payload;
        levels.push_back(l);
    }
    if (ok) {
        blocks.resize(h.payload);
        ok = std::fread(blocks.data(), 1, h.payload, f) == h.payload &&
             texture_hash(blocks.data(), h.payload) == h.payload_hash && std::fgetc(f) == EOF;
    }
    std::fclose(f);
    if (!ok) {
        if (g.bad_files++ < 10) log("nvn/vk: NX_ASTC_BC7: %s does not match its key or checksum: not used", path.c_str());
        std::lock_guard lk(g_index_lock);
        g_index[name] = Bc7Decision::kRgba8;  // for this session; the file stays as it is
        blocks.clear(), levels.clear();
        return false;
    }
    g.loaded++, g.load_bytes += h.payload, g.load_ns += plat::monotonic_ns() - t0;
    return true;
}

Bc7Decision astc_bc7_build(const Bc7Key& k, const std::vector<uint8_t>& rgba, const std::vector<LinearLevel>& levels,
                           std::vector<uint8_t>& blocks, std::vector<LinearLevel>& out_levels) {
    const uint64_t t0 = plat::monotonic_ns();
    out_levels.clear();
    uint64_t at = 0, texels = 0;
    for (const LinearLevel& l : levels) {
        LinearLevel o;
        o.width = l.width, o.height = l.height, o.width_blocks = (l.width + 3) / 4, o.height_blocks = (l.height + 3) / 4;
        o.layer_size = uint64_t(o.width_blocks) * o.height_blocks * 16, o.offset = at;
        at += o.layer_size * k.layers;
        texels += uint64_t(l.width) * l.height * k.layers;
        out_levels.push_back(o);
    }
    blocks.assign(at, 0);
    const bool strict = (k.format == 0x87 && k.levels == 1) || std::max(k.width, k.height) <= 64;
    Verdict v;
    if (levels.size() != k.levels) v.ok = false, v.why = "decoded levels differ from the key";
    uint64_t encode_ns = 0, check_ns = 0;
    std::vector<uint8_t> back;
    for (size_t li = 0; v.ok && li < levels.size(); li++)
        for (uint32_t layer = 0; v.ok && layer < k.layers; layer++) {
            const LinearLevel& l = levels[li];
            const LinearLevel& o = out_levels[li];
            if (l.offset + l.layer_size * (layer + 1) > rgba.size() || l.layer_size != uint64_t(l.width) * l.height * 4) {
                v.ok = false, v.why = "decoded data shorter than its levels";
                break;
            }
            const uint8_t* src = rgba.data() + l.offset + l.layer_size * layer;
            uint8_t* dst = blocks.data() + o.offset + o.layer_size * layer;
            const uint64_t e0 = plat::monotonic_ns();
            bc7_encode_image(src, l.width, l.height, dst);
            const uint64_t e1 = plat::monotonic_ns();
            encode_ns += e1 - e0;
            back.resize(uint64_t(l.width) * l.height * 4);
            bc_decode(0x4d, dst, l.width, l.height, 1, back.data());  // BC7 (linear: the bytes as stored)
            judge(src, back.data(), l.width, l.height, strict, li == 0 || std::min(l.width, l.height) >= 32, v);
            if (!v.ok)  // the first failing level and layer
                v.why += " (level " + std::to_string(li) + ", " + std::to_string(l.width) + "x" +
                         std::to_string(l.height) + ", layer " + std::to_string(layer) + ")";
            check_ns += plat::monotonic_ns() - e1;
        }
    g.built++, g.build_texels += texels, g.encode_ns += encode_ns, g.check_ns += check_ns;
    g.build_ns += plat::monotonic_ns() - t0;
    const std::string name = astc_bc7_name(k);
    const std::string path = g_dir + "/" + name + (v.ok ? ".bc7" : ".rgba8");
    bool written;
    if (v.ok) {
        std::vector<uint8_t> head(sizeof(FileHeader) + sizeof(FileLevel) * out_levels.size());
        FileHeader h{};
        std::memcpy(h.magic, kMagic, 8);
        h.encoder = kBc7EncoderVersion, h.format = k.format, h.width = k.width, h.height = k.height;
        h.layers = k.layers, h.levels = k.levels, h.bh0 = k.bh0, h.level_count = uint32_t(out_levels.size());
        h.hash = k.hash, h.tiled = k.tiled, h.payload = blocks.size();
        h.payload_hash = texture_hash(blocks.data(), blocks.size());
        h.worst_visible = v.worst_visible, h.worst_all = v.worst_all, h.worst_alpha = v.worst_alpha;
        h.worst_block = v.worst_block;
        std::memcpy(head.data(), &h, sizeof h);
        for (size_t i = 0; i < out_levels.size(); i++) {
            const FileLevel fl{out_levels[i].offset, out_levels[i].layer_size, out_levels[i].width, out_levels[i].height};
            std::memcpy(head.data() + sizeof h + i * sizeof fl, &fl, sizeof fl);
        }
        written = write_file(path, head.data(), head.size(), blocks.data(), blocks.size());
    } else {
        char line[512];
        const int n = std::snprintf(line, sizeof line, "%s; visible %.1f dB, all %.1f dB, alpha %.1f dB, worst block %.1f\n",
                                    v.why.c_str(), v.worst_visible, v.worst_all, v.worst_alpha, v.worst_block);
        written = write_file(path, line, uint64_t(std::clamp(n, 0, int(sizeof line) - 1)), nullptr, 0);
    }
    if (!written) {
        if (g.write_failed++ < 5) log("nvn/vk: NX_ASTC_BC7: cannot write %s", path.c_str());
        blocks.clear(), out_levels.clear();
        return Bc7Decision::kUnknown;
    }
    const Bc7Decision d = v.ok ? Bc7Decision::kBc7 : Bc7Decision::kRgba8;
    (v.ok ? g.accepted : g.rejected)++;
    if (!v.ok) {
        std::lock_guard lk(g.reasons_lock);
        g.reasons[v.why.substr(0, v.why.find(" ("))]++;
    }
    {
        std::lock_guard lk(g_index_lock);
        g_index[name] = d;
    }
    if (!v.ok) blocks.clear(), out_levels.clear();
    return d;
}

namespace {

struct Job {
    Bc7Key key;
    std::vector<uint8_t> rgba;
    std::vector<LinearLevel> levels;
};
std::mutex g_job_lock;
std::condition_variable g_job_cv;
std::deque<Job> g_jobs;
std::unordered_set<std::string> g_queued;  // key names queued or being converted
uint64_t g_job_bytes = 0;
constexpr uint64_t kMaxJobBytes = 512ull << 20;
std::atomic<uint64_t> g_dropped{0};

void converter() {
    plat::set_thread_name("astc bc7");
#ifdef _WIN32
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_IDLE);
#endif
    for (;;) {
        Job j;
        {
            std::unique_lock lk(g_job_lock);
            g_job_cv.wait(lk, [] { return !g_jobs.empty(); });
            j = std::move(g_jobs.front());
            g_jobs.pop_front();
            g_job_bytes -= j.rgba.size();
        }
        std::vector<uint8_t> blocks;
        std::vector<LinearLevel> levels;
        astc_bc7_build(j.key, j.rgba, j.levels, blocks, levels);
        std::lock_guard lk(g_job_lock);
        g_queued.erase(astc_bc7_name(j.key));
    }
}

}  // namespace

void astc_bc7_enqueue(const Bc7Key& k, std::vector<uint8_t>&& rgba, std::vector<LinearLevel>&& levels) {
    if (!g_on || rgba.empty() || astc_bc7_lookup(k) != Bc7Decision::kUnknown) return;
    static std::once_flag once;
    std::call_once(once, [] { std::thread(converter).detach(); });
    const std::string name = astc_bc7_name(k);
    std::lock_guard lk(g_job_lock);
    if (g_queued.count(name)) return;
    if (g_job_bytes + rgba.size() > kMaxJobBytes) return void(g_dropped++);
    g_queued.insert(name);
    g_job_bytes += rgba.size();
    g_jobs.push_back(Job{k, std::move(rgba), std::move(levels)});
    g_job_cv.notify_one();
}

void astc_bc7_demote(const Bc7Key& k, const char* why) {
    {
        std::lock_guard lk(g_index_lock);
        g_index[astc_bc7_name(k)] = Bc7Decision::kRgba8;  // this session only: the cache keeps its verdict
    }
    astc_bc7_note_problem(why);
}

void astc_bc7_note_image(bool bc7, uint64_t hash_ns) { (bc7 ? g.images_bc7 : g.images_rgba8)++, g.hash_ns += hash_ns; }

void astc_bc7_note_upload(bool from_memory, uint64_t bytes, uint64_t ns) {
    (from_memory ? g.up_memory : g.up_disk)++;
    g.up_bytes += bytes, g.up_ns += ns;
}

void astc_bc7_note_problem(const char* what) {
    std::atomic<uint64_t>& c = counter_for(what);
    if (c++ < 5) log("nvn/vk: NX_ASTC_BC7: %s: a BC7 image made again as RGBA8", what);
}

void astc_bc7_log() {
    if (!g_on && !g_open) return;
    size_t waiting = 0;
    uint64_t waiting_bytes = 0;
    {
        std::lock_guard lk(g_job_lock);
        waiting = g_jobs.size(), waiting_bytes = g_job_bytes;
    }
    std::string why;
    {
        std::lock_guard lk(g.reasons_lock);
        for (auto& [r, n] : g.reasons) why += "; " + r + " " + std::to_string(n);
    }
    log("nvn/vk: NX_ASTC_BC7: images made BC7 %llu, RGBA8 %llu (key hashing %.1f ms); converted in play %llu (%.1f "
        "Mtexels: encode %.0f ms, check %.0f ms, in all %.0f ms of idle-priority CPU): accepted %llu, kept RGBA8 %llu, "
        "not written %llu%s; loaded from the cache %llu (%.1f MB in %.0f ms), bad files %llu; uploads from memory %llu, "
        "from disk %llu (%.1f MB, %.0f ms); made again as RGBA8: copies %llu, texel writes %llu, views %llu, bytes "
        "changed %llu, no conversion to load %llu, other %llu; conversions waiting %zu (%.0f MB), dropped (memory cap) "
        "%llu",
        (unsigned long long)g.images_bc7.load(), (unsigned long long)g.images_rgba8.load(), double(g.hash_ns.load()) / 1e6,
        (unsigned long long)g.built.load(), double(g.build_texels.load()) / 1e6, double(g.encode_ns.load()) / 1e6,
        double(g.check_ns.load()) / 1e6, double(g.build_ns.load()) / 1e6, (unsigned long long)g.accepted.load(),
        (unsigned long long)g.rejected.load(), (unsigned long long)g.write_failed.load(), why.c_str(),
        (unsigned long long)g.loaded.load(), double(g.load_bytes.load()) / (1 << 20), double(g.load_ns.load()) / 1e6,
        (unsigned long long)g.bad_files.load(), (unsigned long long)g.up_memory.load(), (unsigned long long)g.up_disk.load(),
        double(g.up_bytes.load()) / (1 << 20), double(g.up_ns.load()) / 1e6, (unsigned long long)g.copies.load(),
        (unsigned long long)g.texel_writes.load(), (unsigned long long)g.views.load(), (unsigned long long)g.changed.load(),
        (unsigned long long)g.unloadable.load(), (unsigned long long)g.other.load(), waiting,
        double(waiting_bytes) / (1 << 20), (unsigned long long)g_dropped.load());
}

}  // namespace nx::nvn
