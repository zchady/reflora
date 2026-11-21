// NX_ASTC_BC7_PREPARE=1 (nvn_astc_bc7.h): the NX_ASTC_BC7 cache filled ahead of play, so the first session does not
// convert while the game runs. Reads the game's package the way the game's files reach the GPU: every file of the
// RomFS of every package (zstd files inflated, SARC archives opened, BNTX found wherever they are embedded: BFRES,
// effect files, packs), every block-linear 2D or 2D-array ASTC texture of every BNTX, keyed as the runtime will key
// the NVNtexture the game makes of it (format, size, layers, levels, the packaged block height and its fit to the
// room before the next texture, the hash of its tiled bytes), then untiled, decoded and converted with the same code
// as during play (nvn_astc_bc7.cpp: quality check, verdict files). Textures already in the cache are skipped, so an
// interrupted preparation continues where it stopped. Run by the preparer's `textures` step (tools/prepare_switch2.py)
// on the installed exe, with the game's modules loaded (block sizes come from the SDK's format table) and before the
// game starts; the process then exits. Nothing of the package is written anywhere but the cache.
//
//   NX_ASTC_BC7_PREPARE=1                   prepare, then exit (code: 0 done, 3 no package or cache, 5 not all written)
//   NX_ASTC_BC7_PREPARE_THREADS=<n>         conversion threads (default: logical cores - 2, 1..16), below normal priority
//
// Log lines (for the preparer): "astc bc7 prepare: <files> files ..." at the start, "astc bc7 prepare: progress d/n"
// per file done (every 1 %), "astc bc7 prepare: done ..." at the end.
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#include "../../third_party/zstd/zstd.h"
#include "../content.h"
#include "../nup_archive.h"
#include "nvn.h"
#include "nvn_astc_bc7.h"
#include "nvn_astc_cache.h"
#include "nvn_pipeline.h"
#include "nvn_tiling.h"
#include "platform.h"

#ifdef _WIN32
#include <windows.h>
#endif

namespace nx::nvn {

namespace {

using Buffer = std::shared_ptr<const std::vector<uint8_t>>;

constexpr const char* kSkip[] = {".bwav", ".bars", ".bfstm", ".mp4", ".webm", ".bfsar", ".bfwav", ".mov", ".ogg"};
constexpr uint64_t kMaxBuffered = 1536ull << 20;  // inflated files waiting for their textures' conversion

struct FileRef {
    uint32_t index;
    std::string path;
    uint64_t size;
};

struct Counts {
    std::atomic<uint64_t> bntx{0}, textures{0}, astc{0}, not_2d{0}, linear{0}, bad{0}, other_gob{0};
    std::atomic<uint64_t> already{0}, duplicate{0}, converted{0}, accepted{0}, rejected{0}, unwritten{0};
    std::atomic<uint64_t> untile_failed{0}, texels{0}, unread{0};
} g_n;

struct Job {
    Buffer buf;
    uint64_t start = 0, readable = 0;
    Bc7Key key;
    uint32_t bw = 4, bh = 4;
    std::shared_ptr<std::atomic<int>> file_left;  // jobs of its file still to finish
};

template <class T>
bool rd(const std::vector<uint8_t>& d, uint64_t at, T& v) {
    if (at > d.size() || d.size() - at < sizeof(T)) return false;
    std::memcpy(&v, d.data() + at, sizeof(T));
    return true;
}

bool ends_with(const std::string& s, const char* suffix) {
    const size_t n = std::strlen(suffix);
    return s.size() >= n && std::equal(s.end() - n, s.end(), suffix, [](char a, char b) {
               return std::tolower((unsigned char)a) == std::tolower((unsigned char)b);
           });
}

// A zstd frame (a whole file, or a SARC member) inflated; empty if it is not one.
std::vector<uint8_t> inflate(const uint8_t* p, uint64_t n) {
    std::vector<uint8_t> out;
    const unsigned long long size = ZSTD_getFrameContentSize(p, n);
    if (size != ZSTD_CONTENTSIZE_UNKNOWN && size != ZSTD_CONTENTSIZE_ERROR) {
        if (size > (2ull << 30)) return out;
        out.resize(size);
        const size_t got = ZSTD_decompress(out.data(), out.size(), p, n);
        if (ZSTD_isError(got) || got != size) out.clear();
        return out;
    }
    ZSTD_DStream* ds = ZSTD_createDStream();  // no size in the frame header: streamed
    ZSTD_initDStream(ds);
    ZSTD_inBuffer in{p, size_t(n), 0};
    std::vector<uint8_t> chunk(1 << 20);
    for (;;) {
        ZSTD_outBuffer o{chunk.data(), chunk.size(), 0};
        const size_t r = ZSTD_decompressStream(ds, &o, &in);
        if (ZSTD_isError(r) || out.size() + o.pos > (2ull << 30)) {
            out.clear();
            break;
        }
        out.insert(out.end(), chunk.data(), chunk.data() + o.pos);
        if (r == 0 && in.pos == in.size) break;
        if (o.pos == 0 && in.pos == in.size) {
            out.clear();
            break;
        }
    }
    ZSTD_freeDStream(ds);
    return out;
}

// The ASTC textures of the BNTX at `b`, keyed as image_of / upload_texture will key the game's NVNtextures.
void parse_bntx(const Buffer& buf, uint64_t b, std::vector<Job>& out) {
    const std::vector<uint8_t>& d = *buf;
    uint32_t count = 0;
    uint64_t infos = 0, brtd = 0, brtd_size = 0;
    char kind[4] = {};
    if (!rd(d, b + 0x20, kind) || !rd(d, b + 0x24, count) || !rd(d, b + 0x28, infos) || !rd(d, b + 0x30, brtd))
        return void(g_n.bad++);
    if (std::memcmp(kind, "NX  ", 4) && std::memcmp(kind, "Ounc", 4)) return void(g_n.bad++);
    // Both kinds are untiled as the runtime does every texture: with the GOB layout the game asked the driver for
    // (NVN 2, nvn_device.cpp), whatever the file says. "NX  " ones are counted.
    if (std::memcmp(kind, "NX  ", 4) == 0) g_n.other_gob += count;
    brtd += b, infos += b;
    char tag[4] = {};
    if (count > 100000 || !rd(d, brtd, tag) || std::memcmp(tag, "BRTD", 4) || !rd(d, brtd + 8, brtd_size))
        return void(g_n.bad++);
    // The pool the game makes of the data: from the data's start to the block's end. A texture's room is to the next
    // texture's start (texture_gap), the last one's to the pool's end.
    const uint64_t data_end = std::min<uint64_t>(d.size(), brtd + brtd_size);
    g_n.bntx++;
    struct Info {
        uint64_t start = 0;
        uint32_t format = 0, width = 0, height = 0, depth = 0, layers = 0, levels = 0, image_dim = 0, tile = 0;
    };
    std::vector<Info> all;
    for (uint32_t k = 0; k < count; k++) {
        uint64_t info = 0, mips = 0, mip0 = 0;
        Info t;
        uint16_t tile = 0, levels = 0;
        int32_t w = 0, h = 0, depth = 0, layers = 0;
        uint8_t dim = 0;
        if (!rd(d, infos + 8ull * k, info) || !rd(d, b + info, tag) || std::memcmp(tag, "BRTI", 4)) return void(g_n.bad++);
        info += b;
        if (!rd(d, info + 0x12, tile) || !rd(d, info + 0x16, levels) || !rd(d, info + 0x1C, t.format) ||
            !rd(d, info + 0x24, w) || !rd(d, info + 0x28, h) || !rd(d, info + 0x2C, depth) || !rd(d, info + 0x30, layers) ||
            !rd(d, info + 0x5C, dim) || !rd(d, info + 0x70, mips) || !rd(d, b + mips, mip0))
            return void(g_n.bad++);
        t.start = b + mip0, t.tile = tile, t.levels = levels, t.image_dim = dim;
        t.width = uint32_t(std::max(w, 1)), t.height = uint32_t(std::max(h, 1));
        t.depth = uint32_t(std::max(depth, 1)), t.layers = uint32_t(std::max(layers, 1));
        all.push_back(t);
    }
    g_n.textures += all.size();
    for (const Info& t : all) {
        const uint32_t ch = t.format >> 8, type = t.format & 0xFF;
        if (ch < 0x2D || ch > 0x3A || (type != 1 && type != 6)) continue;  // not ASTC LDR (UNORM or sRGB)
        g_n.astc++;
        if (t.tile != 0) {
            g_n.linear++;
            continue;
        }
        // nn::gfx's image dimension: 1 2D, 5 2D array; NVN targets 1 and 4 (image_of's BC7 candidates)
        if (t.image_dim != 1 && t.image_dim != 5) {
            g_n.not_2d++;
            continue;
        }
        if (t.start >= data_end) {
            g_n.bad++;
            continue;
        }
        const uint32_t nvn = 0x79 + (ch - 0x2D) + (type == 6 ? 14 : 0);
        const uint32_t target = t.image_dim == 5 ? 4 : 1, depth = target == 4 ? t.layers : 1;
        // The NVNtexture: storage as the builder computes it, readable to the pool's end; the room to the next texture
        uint64_t room = data_end - t.start;
        const uint64_t storage = texture_storage_size(nvn, t.width, t.height, depth, std::max(1u, t.levels));
        const uint64_t readable = std::min(storage, room);
        uint64_t gap = readable;
        for (const Info& o : all)
            if (o.start > t.start) gap = std::min(gap, o.start - t.start);
        // The image: image_of's shape (levels no more than the size allows)
        const uint32_t layers = target == 4 ? depth : 1;
        uint32_t largest = std::max(t.width, t.height), fit = 1;
        while (largest >>= 1) fit++;
        const uint32_t levels = std::min(std::max(1u, t.levels), fit);
        const uint32_t packaged = packaged_block_height(nvn, t.height, target, depth);
        const uint32_t bh0 = upload_block_height(nvn, packaged, true, gap, t.width, t.height, layers, levels);
        uint32_t bw = 1, bh = 1, bytes = 16;
        format_block(nvn, bw, bh, bytes);
        if (bw < 4 || bytes != 16) {
            g_n.bad++;
            continue;
        }
        const uint64_t tiled = block_linear_layout(t.width, t.height, layers, levels, bw, bh, bytes, bh0).tiled_size;
        Job j;
        j.buf = buf, j.start = t.start, j.readable = readable, j.bw = bw, j.bh = bh;
        j.key = Bc7Key{texture_hash(d.data() + t.start, std::min(readable, tiled)), tiled, nvn, t.width, t.height,
                       layers, levels, bh0};
        out.push_back(std::move(j));
    }
}

void scan(const Buffer& buf, uint64_t base, uint64_t size, int nesting, std::vector<Job>& out) {
    const std::vector<uint8_t>& d = *buf;
    if (nesting > 8 || base > d.size() || size > d.size() - base) return;
    const uint8_t* p = d.data() + base;
    if (size >= 0x20 && std::memcmp(p, "SARC", 4) == 0) {
        uint32_t data = 0;
        uint16_t nodes = 0;
        if (!rd(d, base + 0xC, data) || !rd(d, base + 0x1A, nodes)) return;
        for (uint32_t k = 0; k < nodes; k++) {
            uint32_t start = 0, end = 0;
            if (!rd(d, base + 0x20 + k * 0x10 + 8, start) || !rd(d, base + 0x20 + k * 0x10 + 12, end) || end < start)
                return;
            scan(buf, base + data + start, end - start, nesting + 1, out);
        }
        return;
    }
    if (size >= 4 && p[0] == 0x28 && p[1] == 0xB5 && p[2] == 0x2F && p[3] == 0xFD) {  // a zstd member
        auto inner = std::make_shared<std::vector<uint8_t>>(inflate(p, size));
        if (!inner->empty()) scan(inner, 0, inner->size(), nesting + 1, out);
        return;
    }
    static const char kBntx[8] = {'B', 'N', 'T', 'X', 0, 0, 0, 0};
    for (uint64_t at = 0; at + 0x40 <= size;) {
        const uint8_t* hit = static_cast<const uint8_t*>(std::memchr(p + at, 'B', size - at));
        if (!hit) break;
        at = uint64_t(hit - p);
        if (at + 0x40 <= size && std::memcmp(hit, kBntx, 8) == 0) parse_bntx(buf, base + at, out);
        at++;
    }
}

}  // namespace

int astc_bc7_prepare() {
    const uint64_t t0 = plat::monotonic_ns();
    if (!astc_bc7_open_cache()) return 3;
    nup::Archive* a = content::archive();
    if (!a) {
        log("astc bc7 prepare: no package (data/nup.txt)");
        return 3;
    }
    // The game's packages are Switch 2 ones: GOBs as NVN 2 lays them out (nvn_device.cpp sets it when the game asks)
    set_gob_layout(GobLayout::Nvn2);
    std::vector<FileRef> files;
    uint64_t total_bytes = 0;
    for (const nup::Package& pk : a->packages()) {
        if (pk.romfs_dir == nup::kNone) continue;
        std::vector<std::pair<uint32_t, std::string>> dirs{{pk.romfs_dir, ""}};
        while (!dirs.empty()) {
            auto [dir, path] = dirs.back();
            dirs.pop_back();
            for (const nup::Entry& e : a->list(dir)) {
                const std::string p = path + "/" + e.name;
                if (e.is_dir) {
                    dirs.push_back({e.index, p});
                    continue;
                }
                bool skip = false;
                for (const char* s : kSkip) skip |= ends_with(p, s);
                if (skip) continue;
                files.push_back({e.index, p, e.size});
                total_bytes += e.size;
            }
        }
    }
    int threads = int(std::thread::hardware_concurrency()) - 2;
    if (const char* e = std::getenv("NX_ASTC_BC7_PREPARE_THREADS")) threads = std::atoi(e);
    threads = std::clamp(threads, 1, 16);
    log("astc bc7 prepare: %zu files (%.0f MB) in %zu packages, %d threads, cache %s", files.size(),
        double(total_bytes) / (1 << 20), a->packages().size(), threads, astc_bc7_cache_dir().c_str());

    std::mutex lock;
    std::condition_variable cv_work, cv_room;
    std::deque<Job> queue;
    bool producing = true;
    std::unordered_set<std::string> in_flight;
    std::atomic<uint64_t> buffered{0}, files_done{0};
    std::atomic<int> last_pct{-1};
    auto file_finished = [&] {
        const uint64_t done = ++files_done;
        const int pct = int(done * 100 / std::max<size_t>(files.size(), 1));
        int prev = last_pct.load();
        while (pct > prev && !last_pct.compare_exchange_weak(prev, pct)) {
        }
        if (pct > prev) log("astc bc7 prepare: progress %llu/%zu", (unsigned long long)done, files.size());
    };
    auto worker = [&] {
        plat::set_thread_name("astc bc7 prep");
#ifdef _WIN32
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#endif
        for (;;) {
            Job j;
            {
                std::unique_lock lk(lock);
                cv_work.wait(lk, [&] { return !queue.empty() || !producing; });
                if (queue.empty()) return;
                j = std::move(queue.front());
                queue.pop_front();
            }
            const std::string name = astc_bc7_name(j.key);
            bool mine = false;
            if (astc_bc7_lookup(j.key) != Bc7Decision::kUnknown) {
                g_n.already++;
            } else {
                std::lock_guard lk(lock);
                mine = in_flight.insert(name).second;
                if (!mine) g_n.duplicate++;
            }
            if (mine) {
                std::vector<uint8_t> linear, blocks;
                std::vector<LinearLevel> out;
                auto lv = deswizzle_texture(j.buf->data() + j.start, j.readable, j.key.width, j.key.height,
                                            j.key.layers, j.key.levels, j.bw, j.bh, 16, linear, j.key.bh0);
                if (lv.empty()) {
                    g_n.untile_failed++;
                } else {
                    decode_astc(j.key.format, linear, lv, j.key.layers, j.bw, j.bh, false);
                    for (const LinearLevel& l : lv) g_n.texels += uint64_t(l.width) * l.height * j.key.layers;
                    const Bc7Decision v = astc_bc7_build(j.key, linear, lv, blocks, out);
                    g_n.converted++;
                    (v == Bc7Decision::kBc7 ? g_n.accepted : v == Bc7Decision::kRgba8 ? g_n.rejected : g_n.unwritten)++;
                }
                std::lock_guard lk(lock);
                in_flight.erase(name);
            }
            const bool last = --*j.file_left == 0;
            j.buf.reset();
            if (last) file_finished();
            cv_room.notify_one();
        }
    };
    std::vector<std::thread> pool;
    for (int k = 0; k < threads; k++) pool.emplace_back(worker);

    for (const FileRef& f : files) {
        std::vector<uint8_t> raw(f.size);
        if (f.size && a->read(f.index, 0, raw.data(), f.size) != int64_t(f.size)) {
            g_n.unread++;
            file_finished();
            continue;
        }
        std::shared_ptr<std::vector<uint8_t>> data;
        if (ends_with(f.path, ".zs")) {
            data = std::make_shared<std::vector<uint8_t>>(inflate(raw.data(), raw.size()));
            if (data->empty()) g_n.unread++;
        } else {
            data = std::make_shared<std::vector<uint8_t>>(std::move(raw));
        }
        std::vector<Job> jobs;
        if (!data->empty()) {
            // the buffer counts as waiting until every texture of it is converted (its deleter)
            const uint64_t size = data->size();
            buffered += size;
            Buffer buf(data.get(), [data, size, &buffered](const std::vector<uint8_t>*) mutable {
                buffered -= size;
                data.reset();
            });
            data.reset();
            scan(buf, 0, buf->size(), 0, jobs);
        }
        if (jobs.empty()) {
            file_finished();
            continue;
        }
        auto left = std::make_shared<std::atomic<int>>(int(jobs.size()));
        std::unique_lock lk(lock);
        cv_room.wait(lk, [&] { return buffered.load() <= kMaxBuffered || queue.empty(); });
        for (Job& j : jobs) j.file_left = left, queue.push_back(std::move(j));
        cv_work.notify_all();
    }
    {
        std::lock_guard lk(lock);
        producing = false;
    }
    cv_work.notify_all();
    for (std::thread& t : pool) t.join();
    astc_bc7_log();  // the check's reasons and times
    const double secs = double(plat::monotonic_ns() - t0) / 1e9;
    log("astc bc7 prepare: done in %.1f s: %llu BNTX, %llu textures, %llu ASTC (%llu not 2D, %llu pitch-linear; %llu "
        "textures in BNTX of the first Switch, %llu unreadable entries); converted %llu (%.1f Mtexels): accepted %llu, kept RGBA8 %llu, "
        "not written %llu, could not untile %llu; already in the cache %llu, the same texture again %llu; files not read "
        "%llu",
        secs, (unsigned long long)g_n.bntx.load(), (unsigned long long)g_n.textures.load(),
        (unsigned long long)g_n.astc.load(), (unsigned long long)g_n.not_2d.load(), (unsigned long long)g_n.linear.load(),
        (unsigned long long)g_n.other_gob.load(), (unsigned long long)g_n.bad.load(),
        (unsigned long long)g_n.converted.load(), double(g_n.texels.load()) / 1e6, (unsigned long long)g_n.accepted.load(),
        (unsigned long long)g_n.rejected.load(), (unsigned long long)g_n.unwritten.load(),
        (unsigned long long)g_n.untile_failed.load(), (unsigned long long)g_n.already.load(),
        (unsigned long long)g_n.duplicate.load(), (unsigned long long)g_n.unread.load());
    return g_n.unwritten.load() ? 5 : 0;
}

}  // namespace nx::nvn
