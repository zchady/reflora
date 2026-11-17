// ASTC textures decoded ahead of their first use (astc_predecode / astc_cache_take).
//
// Desktop GPUs have no ASTC: each texture is untiled and decoded to RGBA8 on the CPU (nvn_upload.cpp) when a replay
// first samples it. A course streams its textures in while the player runs, and each batch decoded on the replay
// thread took 5-55 ms: the frame missed its display time (shown for 2 vsyncs) and the game's governor doubled the
// next frame's time step, a jump in the movement (runs motionA, stutterE, the user's sessions; §5w). Here a texture
// in an ASTC format is queued when the game initializes it (its data is in its pool by then: BNTX files are read
// before their textures are made), decoded by background threads at a lower priority, and kept until the upload
// takes it. The upload checks a hash of the texture's tiled bytes, so a result decoded from data that changed since
// is never used (the upload then decodes it itself, as before). NX_ASTC_PREDECODE=0: off (comparison).
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <tuple>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

#include "nvn.h"
#include "nvn_astc.h"
#include "nvn_astc_cache.h"
#include "nvn_tiling.h"
#include "nvn_vk.h"
#include "platform.h"

namespace nx::nvn {

namespace {

struct Key {
    uint64_t memory = 0;
    uint32_t format = 0, width = 0, height = 0, layers = 0, levels = 0, bh0 = 0;
    bool operator<(const Key& o) const {
        return std::tie(memory, format, width, height, layers, levels, bh0) <
               std::tie(o.memory, o.format, o.width, o.height, o.layers, o.levels, o.bh0);
    }
};

enum State { kQueued, kRunning, kDone, kFailed };
struct Entry {
    State state = kQueued;
    uint64_t hash = 0, bytes = 0, queued_ns = 0;
    std::vector<uint8_t> rgba;
    std::vector<LinearLevel> levels;
};

std::mutex g_lock;
std::condition_variable g_work_cv, g_done_cv;
std::deque<uint64_t> g_queue;  // guest textures to look at
std::map<Key, std::shared_ptr<Entry>> g_entries;
uint64_t g_bytes = 0;  // decoded bytes held
constexpr uint64_t kMaxBytes = 1536ull << 20;
constexpr uint64_t kMaxAgeNs = 60'000'000'000ull;
// Statistics, logged every 500 uploads: taken ready, waited for, decoded by the upload (none queued, or data changed).
std::atomic<uint64_t> g_hits{0}, g_waits{0}, g_wait_ns{0}, g_misses{0}, g_stale{0}, g_decoded{0}, g_skipped{0},
    g_changed{0};

bool on() {
    static const bool v = [] {
        const char* e = std::getenv("NX_ASTC_PREDECODE");
        return !(e && e[0] == '0');
    }();
    return v;
}

void drop_locked(std::map<Key, std::shared_ptr<Entry>>::iterator it) {
    g_bytes -= it->second->bytes;
    g_entries.erase(it);
}

// Entries never taken (textures never drawn): the oldest go after a minute, or when the held bytes pass the cap.
void trim_locked(uint64_t now) {
    for (auto it = g_entries.begin(); it != g_entries.end();) {
        auto next = std::next(it);
        if (it->second->state != kRunning && now - it->second->queued_ns > kMaxAgeNs) drop_locked(it);
        it = next;
    }
    while (g_bytes > kMaxBytes) {
        auto oldest = g_entries.end();
        for (auto it = g_entries.begin(); it != g_entries.end(); ++it)
            if (it->second->state == kDone && (oldest == g_entries.end() || it->second->queued_ns < oldest->second->queued_ns))
                oldest = it;
        if (oldest == g_entries.end()) break;
        drop_locked(oldest);
    }
}

void work(uint64_t guest) {
    AstcSource s;
    if (!texture_astc_source(guest, s)) return;
    Key key{s.memory, s.format, s.width, s.height, s.layers, s.levels, s.bh0};
    auto e = std::make_shared<Entry>();
    e->state = kRunning;
    e->queued_ns = plat::monotonic_ns();
    {
        std::lock_guard lk(g_lock);
        auto it = g_entries.find(key);
        if (it != g_entries.end()) {
            if (it->second->state == kRunning) return;  // another worker has it
            drop_locked(it);
        }
        if (g_bytes > kMaxBytes) {
            g_skipped++;
            return;
        }
        g_entries[key] = e;
    }
    uint32_t bw = 1, bh = 1, bytes = 16;
    format_block(s.format, bw, bh, bytes);
    const uint8_t* tiled = reinterpret_cast<const uint8_t*>(s.memory);
    uint64_t hash = texture_hash(tiled, std::min(s.readable, s.tiled_size));
    std::vector<uint8_t> linear;
    auto levels = deswizzle_texture(tiled, s.readable, s.width, s.height, s.layers, s.levels, bw, bh, bytes, linear, s.bh0);
    bool ok = !levels.empty();
    if (ok) decode_astc(s.format, linear, levels, s.layers, bw, bh, false);
    // Written while it was read (a file still streaming in): not a decode of one state of the data.
    if (ok && texture_hash(tiled, std::min(s.readable, s.tiled_size)) != hash) ok = false, g_changed++;
    std::lock_guard lk(g_lock);
    e->hash = hash;
    e->state = ok ? kDone : kFailed;
    if (ok) {
        e->bytes = linear.size();
        e->rgba.swap(linear);
        e->levels.swap(levels);
        auto it = g_entries.find(key);
        if (it != g_entries.end() && it->second == e) g_bytes += e->bytes;
        g_decoded++;
    }
    trim_locked(plat::monotonic_ns());
    g_done_cv.notify_all();
}

void worker() {
    plat::set_thread_name("astc predecode");
#ifdef _WIN32
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#endif
    for (;;) {
        uint64_t guest;
        {
            std::unique_lock lk(g_lock);
            g_work_cv.wait(lk, [] { return !g_queue.empty(); });
            guest = g_queue.front();
            g_queue.pop_front();
        }
        work(guest);
    }
}

}  // namespace

void astc_predecode(uint64_t guest) {
    if (!on()) return;
    static std::once_flag once;
    std::call_once(once, [] {
        unsigned n = std::clamp(std::thread::hardware_concurrency() / 4, 1u, 3u);
        for (unsigned k = 0; k < n; k++) std::thread(worker).detach();
        log("nvn/vk: ASTC textures decoded ahead of use by %u threads (NX_ASTC_PREDECODE=0: off)", n);
    });
    {
        std::lock_guard lk(g_lock);
        g_queue.push_back(guest);
    }
    g_work_cv.notify_one();
}

bool astc_cache_take(uint64_t memory, uint32_t format, uint32_t width, uint32_t height, uint32_t layers,
                     uint32_t levels, uint32_t bh0, uint64_t hash, std::vector<uint8_t>& rgba,
                     std::vector<LinearLevel>& out_levels) {
    if (!on()) return false;
    Key key{memory, format, width, height, layers, levels, bh0};
    bool result = false;
    {
        std::unique_lock lk(g_lock);
        auto it = g_entries.find(key);
        if (it == g_entries.end()) {
            g_misses++;
        } else {
            std::shared_ptr<Entry> e = it->second;
            if (e->state == kRunning) {  // being decoded: waiting is shorter than starting over
                uint64_t t0 = plat::monotonic_ns();
                g_done_cv.wait_for(lk, std::chrono::milliseconds(200), [&] { return e->state != kRunning; });
                g_waits++, g_wait_ns += plat::monotonic_ns() - t0;
                it = g_entries.find(key);
            }
            if (it != g_entries.end() && it->second == e && e->state == kDone && e->hash == hash) {
                rgba.swap(e->rgba);
                out_levels.swap(e->levels);
                result = true;
                g_hits++;
            } else if (e->state == kDone || e->state == kFailed) {
                g_stale++;
            } else {
                g_misses++;
            }
            if (it != g_entries.end() && it->second == e && e->state != kRunning) drop_locked(it);
        }
    }
    uint64_t n = g_hits + g_misses + g_stale;
    if (n % 100 == 0)
        log("nvn/vk: ASTC decoded ahead: %llu uploads took it ready, %llu after waiting (%.1f ms), %llu decoded by the "
            "upload (%llu stale), %llu decoded in the background, %llu changed while decoded, %llu skipped (memory cap)",
            (unsigned long long)g_hits.load(), (unsigned long long)g_waits.load(), double(g_wait_ns.load()) / 1e6,
            (unsigned long long)(g_misses + g_stale), (unsigned long long)g_stale.load(),
            (unsigned long long)g_decoded.load(), (unsigned long long)g_changed.load(), (unsigned long long)g_skipped.load());
    return result;
}

uint64_t texture_hash(const uint8_t* p, uint64_t n) {
    uint64_t h[4] = {0x243F6A8885A308D3ull, 0x13198A2E03707344ull, 0xA4093822299F31D0ull, 0x082EFA98EC4E6C89ull};
    const uint64_t m = 0x9E3779B97F4A7C15ull;
    uint64_t k = 0;
    for (; k + 32 <= n; k += 32)
        for (int l = 0; l < 4; l++) {
            uint64_t w;
            std::memcpy(&w, p + k + 8 * l, 8);
            h[l] = (h[l] ^ w) * m;
            h[l] ^= h[l] >> 31;
        }
    uint64_t r = n * m;
    for (int l = 0; l < 4; l++) r = (r ^ h[l]) * m, r ^= r >> 29;
    for (; k < n; k++) r = (r ^ p[k]) * m;
    return r ^ (r >> 32);
}

}  // namespace nx::nvn
