// The pipeline catalog: pipelines described without the scene that uses them, so they can be made before the game
// asks for them (WONDER_SWITCH2_PROGRESS.md §5zd, §5zd1, §5zd2).
//
// A graphics pipeline is identified by its canonical PipelineKey (nvn_pipeline_state.h): two shader stages, which a
// record names by their hash (the .spv file name), and plain state numbers. A compute pipeline by its stage's hash
// and the one control-section value its module depends on, the shared memory size (compute_shader_for_pipeline). A
// record carries no game code or data and no driver output. From the hashes, <data root>/spirv/<hash>.spv and
// results.tsv give the very stages the game's own ShaderInfo would (shader_for_pipeline), so the create info, and the
// pipeline, are the same on any PC: each PC's driver compiles them for itself.
//
//   NX_PIPELINE_RECORD=1 | <file>   every pipeline the game makes is appended once (canonical) to
//                                   <data root>/pipeline_catalog_local.bin (or <file>)
//   NX_PIPELINE_PREWARM=<file>[;<file>...]   the catalogs to prepare. Unset: <data root>/pipeline_catalog.bin (the
//                                   project's catalog, put there by the preparer) and pipeline_catalog_local.bin, those
//                                   that exist; 0: none
//   NX_PREWARM_MODE=background      (default) after the device exists, worker threads at reduced priority make every
//                                   catalog pipeline; a draw needing one takes it ready, waits for it if a worker is
//                                   on it, or claims it and makes it itself if no worker started it yet
//                   wait            the same, but the game starts only once every pipeline is made
//                   exit            the same, then the pipeline cache and a stamp are saved and the process exits: the
//                                   graphics preparation (no gameplay). With the stamp unchanged (same GPU, driver,
//                                   runtime format, switches, catalogs and SPIR-V) it exits at once: nothing to redo.
//   NX_PREWARM_EXIT=1               = NX_PREWARM_MODE=exit
//   NX_PREWARM_THREADS=<n>          worker threads (default: logical CPUs - 2, at most 8)
//   NX_PIPELINE_VERIFY=1            every pipeline taken from the catalog: the game's own stages compared with the
//                                   catalog's (SPIR-V, shared memory size, descriptor-set layouts), a count logged
//
// The file: a 32-byte header (magic "NXPC", format version, sizeof(PipelineKey), record size, canonical-rules
// version), then entries of a record and its 64-bit FNV-1a check. A damaged or partial entry is skipped (a recording
// cut short by a crash keeps every entry before it). Version 3 records start with a kind (graphics, compute);
// version 2 files (graphics only) are still read. Another version is not read at all.
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#ifdef _WIN32
#include <filesystem>
#include <windows.h>
#endif

#include "../collect.h"
#include "nvn_pipeline.h"
#include "platform.h"

namespace nx::nvn {

using namespace nx::vk;

namespace {

constexpr uint32_t kMagic = 0x4350584E;  // "NXPC"
constexpr uint32_t kFormatVersion = 3;   // 1: the proof of concept (no checks); 2: graphics only (still read)
// Bumped when canonical() or fill_fixed_state() change what a key means (a catalog of older rules is still read and
// its keys made canonical again, but the stamp changes, so a preparation is redone).
constexpr uint32_t kCanonicalVersion = 2;

struct Header {
    uint32_t magic = kMagic, version = kFormatVersion, key_size = sizeof(PipelineKey), record_size = 0;
    uint32_t canonical_version = kCanonicalVersion, reserved[3] = {};
};
static_assert(sizeof(Header) == 32);

enum Kind : uint32_t { kGraphics = 0, kCompute = 1 };

struct Record {
    uint32_t kind;    // Kind
    uint32_t shared;  // compute: the control section's shared memory bytes; graphics: 0
    char vs[16];      // graphics: the vertex stage; compute: the compute stage
    char fs[16];      // graphics: the fragment stage; compute: empty
    PipelineKey key;  // graphics: canonical, vs and fs null; compute: zero
};
static_assert(sizeof(Record) == 744 && offsetof(Record, key) == 40, "the format of tools/pipeline_catalog.py");
struct RecordV2 {  // format 2: a graphics record without the kind
    char vs[16];
    char fs[16];
    PipelineKey key;
};
static_assert(sizeof(RecordV2) == 736);
using PortableKey = ByteKey<sizeof(Record)>;

uint64_t fnv1a(const void* p, size_t n, uint64_t h = 0xcbf29ce484222325ull) {
    const auto* b = static_cast<const unsigned char*>(p);
    for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 0x100000001b3ull;
    return h;
}

void copy_hash(char (&out)[16], const ShaderInfo* s) {
    if (s) std::memcpy(out, s->hash.data(), std::min<size_t>(16, s->hash.size()));
}

Record record_of(const PipelineKey& k) {
    Record r;
    std::memset(&r, 0, sizeof r);
    r.kind = kGraphics;
    copy_hash(r.vs, k.vs);
    copy_hash(r.fs, k.fs);
    std::memcpy(&r.key, &k, sizeof k);
    r.key.vs = r.key.fs = nullptr;
    return r;
}

Record compute_record_of(const ShaderInfo* cs) {
    Record r;
    std::memset(&r, 0, sizeof r);
    r.kind = kCompute;
    r.shared = cs->control.shared_size;
    copy_hash(r.vs, cs);
    return r;
}

PortableKey portable(const Record& r) {
    PortableKey p;
    std::memcpy(p.b, &r, sizeof r);
    return p;
}

std::string hash_of(const char (&h)[16]) {
    size_t n = 0;
    while (n < 16 && h[n]) n++;
    return std::string(h, n);
}

struct CatalogFile {
    std::vector<Record> records;
    uint32_t canonical_version = 0, version = 0;
    size_t damaged = 0;
    bool ok = false;
    bool whole = true;  // no partial entry at the end (a recording cut by a crash leaves one)
};

CatalogFile read_catalog(const std::string& path, bool quiet = false) {
    CatalogFile out;
    FILE* f = plat::fopen_utf8(path, "rb");
    if (!f) return out;
    Header h{};
    const bool v3 = std::fread(&h, sizeof h, 1, f) == 1 && h.magic == kMagic && h.version == 3 &&
                    h.key_size == sizeof(PipelineKey) && h.record_size == sizeof(Record);
    const bool v2 = !v3 && h.magic == kMagic && h.version == 2 && h.key_size == sizeof(PipelineKey) &&
                    h.record_size == sizeof(RecordV2);
    if (!v3 && !v2) {
        if (!quiet) log("nvn/vk: pipeline catalog %s: another format or runtime version: not used", path.c_str());
        std::fclose(f);
        return out;
    }
    out.ok = true;
    out.version = h.version;
    out.canonical_version = h.canonical_version;
    std::vector<unsigned char> e(h.record_size + 8);
    {
        const int64_t at = plat::ftell64(f);
        plat::fseek64(f, 0, SEEK_END);
        out.whole = (plat::ftell64(f) - at) % int64_t(e.size()) == 0;
        plat::fseek64(f, at, SEEK_SET);
    }
    while (std::fread(e.data(), e.size(), 1, f) == 1) {
        uint64_t check;
        std::memcpy(&check, e.data() + h.record_size, 8);
        if (fnv1a(e.data(), h.record_size) != check) {
            out.damaged++;
            continue;
        }
        Record r;
        std::memset(&r, 0, sizeof r);
        if (v3) {
            std::memcpy(&r, e.data(), sizeof r);
            if (r.kind != kGraphics && r.kind != kCompute) {  // a kind this runtime does not know: skipped
                out.damaged++;
                continue;
            }
        } else {
            // vs, fs and key follow each other in both layouts
            std::memcpy(reinterpret_cast<char*>(&r) + offsetof(Record, vs), e.data(), sizeof(RecordV2));
        }
        out.records.push_back(r);
    }
    std::fclose(f);
    if (out.damaged && !quiet) log("nvn/vk: pipeline catalog %s: %zu damaged entries skipped", path.c_str(), out.damaged);
    return out;
}

// ---- recording -------------------------------------------------------------------------------------------------

struct Recorder {
    std::mutex lock;
    bool tried = false;
    FILE* file = nullptr;
    std::unordered_set<PortableKey, ByteKeyHash> seen;
};
Recorder& recorder() {
    static Recorder r;
    return r;
}

const std::string& record_path() {
    static const std::string path = [] {
        std::string e;
        plat::env_utf8("NX_PIPELINE_RECORD", e);  // a path: UTF-8 (accents)
        // a collection session (collect.h) records into its own file, never into a catalog the game prepares
        if (e.empty() && collect::on()) return collect::dir() + "/pipelines_collected.bin";
        if (e.empty() || e[0] == '0') return std::string();
        return e == "1" ? plat::data_root() + "/pipeline_catalog_local.bin" : e;
    }();
    return path;
}

// True when `rec` was written now (new to the file).
bool note_record(const Record& rec) {
    const std::string& path = record_path();
    if (path.empty()) return false;
    Recorder& r = recorder();
    std::lock_guard lk(r.lock);
    if (!r.tried) {
        r.tried = true;
        CatalogFile old = read_catalog(path, true);
        for (const Record& x : old.records) r.seen.insert(portable(x));
        // a file of another format (or of format 2: appending version-3 entries to it would mix sizes), or one whose
        // last entry a crash cut short (appending after it would misalign every later entry), is started over, with
        // its whole records first
        // its whole records first. Rewritten beside it and renamed over it once complete and flushed: a crash while
        // rewriting leaves the old file, never fewer records.
        const bool exists = [&] {
            FILE* f = plat::fopen_utf8(path, "rb");
            if (f) std::fclose(f);
            return f != nullptr;
        }();
        const bool append = old.ok && old.version == kFormatVersion && old.whole;
        if (exists && !append) {
            const std::string tmp = path + ".tmp";
            bool ok = false;
            if (FILE* f = plat::fopen_utf8(tmp, "wb")) {
                Header h;
                h.record_size = sizeof(Record);
                ok = std::fwrite(&h, sizeof h, 1, f) == 1;
                for (const Record& x : old.records) {
                    const uint64_t check = fnv1a(&x, sizeof x);
                    ok = std::fwrite(&x, sizeof x, 1, f) == 1 && std::fwrite(&check, sizeof check, 1, f) == 1 && ok;
                }
                ok = std::fflush(f) == 0 && ok;
                ok = std::fclose(f) == 0 && ok;
            }
#ifdef _WIN32
            ok = ok && MoveFileExW(std::filesystem::u8path(tmp).c_str(), std::filesystem::u8path(path).c_str(),
                                   MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
#else
            ok = ok && std::rename(tmp.c_str(), path.c_str()) == 0;
#endif
            if (!ok) {
                plat::remove_utf8(tmp);
                log("nvn/vk: pipeline catalog: cannot rewrite %s: not recording", path.c_str());
                return false;
            }
        }
        r.file = plat::fopen_utf8(path, "ab");
        if (r.file && !exists) {  // a new file: its header, on disk before the first entry
            Header h;
            h.record_size = sizeof(Record);
            std::fwrite(&h, sizeof h, 1, r.file);
            std::fflush(r.file);
        }
        log("nvn/vk: pipeline catalog: recording to %s (%zu records already there)", path.c_str(), r.seen.size());
    }
    if (!r.file || !r.seen.insert(portable(rec)).second) return false;
    const uint64_t check = fnv1a(&rec, sizeof rec);
    std::fwrite(&rec, sizeof rec, 1, r.file);
    std::fwrite(&check, sizeof check, 1, r.file);
    std::fflush(r.file);
    return true;
}

// ---- making in advance -----------------------------------------------------------------------------------------

enum JobState : uint8_t { kPending, kBuilding, kReady, kTaken, kByGame, kFailed };

struct Job {
    Record record;
    PipelineKey key;                // graphics: canonical, with this run's stages
    const ShaderInfo* cs = nullptr;  // compute: this run's stage
    std::atomic<uint8_t> state{kPending};
    VkPipeline pipeline = VK_NULL_HANDLE;
};

struct Prewarmed {
    std::unique_ptr<Job[]> jobs;
    size_t count = 0, compute = 0;
    std::unordered_map<PortableKey, size_t, ByteKeyHash> index;  // written before `active`, then read only
    std::atomic<bool> active{false};
    std::mutex lock;  // with cv: a draw waiting for a pipeline a worker is making
    std::condition_variable cv;
    std::atomic<uint64_t> served{0}, waited{0}, by_game{0}, not_in_catalog{0}, made{0}, failed{0};
    std::atomic<uint64_t> verified{0}, different{0};
};
Prewarmed& prewarmed() {
    static Prewarmed p;
    return p;
}

std::string mode() {
    if (const char* e = std::getenv("NX_PREWARM_EXIT"); e && e[0] == '1') return "exit";
    const char* m = std::getenv("NX_PREWARM_MODE");
    return m && *m ? m : "background";
}

bool verify_on() {
    static const bool on = [] {
        const char* e = std::getenv("NX_PIPELINE_VERIFY");
        return e && e[0] == '1';
    }();
    return on;
}

// NX_PIPELINE_VERIFY: the game's stage and the catalog's give the same pipeline objects. Every input of the module
// (patched_spirv: the SPIR-V, the stage, the compute shared memory size; the rest are this run's switches, the same
// for both) and of the layout (program_layout: the reflection, from the SPIR-V; set_layout objects are cached by
// content, so equal handles are equal bindings).
bool same_stage(const ShaderInfo* game, const ShaderInfo* cat) {
    if (!game || !cat) return game == cat;
    if (game->stage != cat->stage || game->spirv != cat->spirv) return false;
    if (game->stage == ShaderStage::Compute && game->control.shared_size != cat->control.shared_size) return false;
    const ProgramLayout& a = program_layout({game});
    const ProgramLayout& b = program_layout({cat});
    return std::equal(std::begin(a.sets), std::end(a.sets), std::begin(b.sets)) && a.dynamic == b.dynamic;
}

void verify_take(const Job& job, const ShaderInfo* game_a, const ShaderInfo* game_b) {
    Prewarmed& pw = prewarmed();
    const bool same = job.record.kind == kCompute
                          ? same_stage(game_a, job.cs)
                          : same_stage(game_a, job.key.vs) && same_stage(game_b, job.key.fs);
    const uint64_t n = ++pw.verified, bad = same ? pw.different.load() : ++pw.different;
    if (!same && bad <= 20)
        log("nvn/vk: pipeline catalog: verify: %s %s%s%s: the game's stage differs from the catalog's",
            job.record.kind == kCompute ? "compute" : "graphics", hash_of(job.record.vs).c_str(),
            job.record.fs[0] ? " / " : "", hash_of(job.record.fs).c_str());
    if (n == 1 || n % 100 == 0 || !same)
        log("nvn/vk: pipeline catalog: verify: %llu taken, %llu the same as the game's, %llu different",
            (unsigned long long)n, (unsigned long long)(n - bad), (unsigned long long)bad);
}

// What a prepared pipeline cache depends on: the device and driver (the driver checks its cache's header too, but
// this lets the preparation tell "already done" without compiling), the runtime's pipeline format and switches, the
// catalogs and the SPIR-V. Text, one value per line.
std::string stamp_text(uint64_t catalog_digest) {
    VkPhysicalDeviceProperties p{};
    p_vkGetPhysicalDeviceProperties(vk_physical(), &p);
    char uuid[2 * VK_UUID_SIZE + 1] = {};
    for (int i = 0; i < VK_UUID_SIZE; i++) std::snprintf(uuid + 2 * i, 3, "%02x", p.pipelineCacheUUID[i]);
    const FixedStateEnv& e = fixed_state_env();
    std::string spirv;
    if (FILE* f = plat::fopen_utf8(plat::data_root() + "/spirv/results.tsv", "rb")) {
        std::vector<char> buf(1 << 16);
        uint64_t h = 0xcbf29ce484222325ull;
        for (size_t n; (n = std::fread(buf.data(), 1, buf.size(), f)) > 0;) h = fnv1a(buf.data(), n, h);
        std::fclose(f);
        char t[32];
        std::snprintf(t, sizeof t, "%016llx", (unsigned long long)h);
        spirv = t;
    }
    char text[1024];
    std::snprintf(text, sizeof text,
                  "device %04x:%04x %s\ndriver %u\npipeline cache uuid %s\nruntime pipeline format %u/%u/%zu\n"
                  "switches clip%d clamp%d a2c%d ufloat%d slack%d greater%d stencil%d rp%d\ncatalog %016llx\n"
                  "spirv %s\n",
                  p.vendorID, p.deviceID, p.deviceName, p.driverVersion, uuid, kFormatVersion, kCanonicalVersion,
                  sizeof(PipelineKey), e.depth_clip_control, e.depth_clamp_feature, e.alpha_to_coverage,
                  e.ufloat_alpha_one, e.equal_slack, e.greater_slack, e.stencil_test, render_pass_objects() ? 1 : 0,
                  (unsigned long long)catalog_digest, spirv.c_str());
    // A measurement's salt changes every module the driver sees: its preparation is not the plain one.
    if (const char* salt = std::getenv("NX_SPIRV_SALT"); salt && *salt) return text + std::string("salt ") + salt + "\n";
    return text;
}

std::string stamp_path() {
    VkPhysicalDeviceProperties p{};
    p_vkGetPhysicalDeviceProperties(vk_physical(), &p);
    char name[64];
    std::snprintf(name, sizeof name, "/pipelines_%04x_%04x.stamp", p.vendorID, p.deviceID);
    return plat::data_root() + name;
}

// The stamp replaced in one step (a temporary file, then a rename over the old one): a crash leaves the old stamp or
// the new one, never half of one.
bool write_stamp(const std::string& stamp) {
    const std::string path = stamp_path(), tmp = path + ".tmp";
    FILE* f = plat::fopen_utf8(tmp, "wb");
    if (!f) return false;
    bool ok = std::fwrite(stamp.data(), 1, stamp.size(), f) == stamp.size();
    ok = std::fclose(f) == 0 && ok;
#ifdef _WIN32
    const auto t = std::filesystem::u8path(tmp);
    ok = ok && MoveFileExW(t.c_str(), std::filesystem::u8path(path).c_str(), MOVEFILE_REPLACE_EXISTING);
    if (!ok) DeleteFileW(t.c_str());
#else
    ok = ok && std::rename(tmp.c_str(), path.c_str()) == 0;
    if (!ok) plat::remove_utf8(tmp);
#endif
    return ok;
}

void set_worker_priority(bool background) {
#ifdef _WIN32
    if (background) SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#else
    (void)background;
#endif
}

// Every pending job: make it (unless a draw claimed it), then hand it over. `keep` false (the preparation, with no
// game to hand pipelines to): each pipeline is destroyed once made, its compiled code already in the pipeline cache,
// so the process's memory does not grow with the catalog.
void run_workers(bool background, bool keep) {
    Prewarmed& pw = prewarmed();
    unsigned threads = std::max(1u, std::min(8u, std::thread::hardware_concurrency() > 2 ? std::thread::hardware_concurrency() - 2 : 1u));
    if (const char* e = std::getenv("NX_PREWARM_THREADS")) threads = std::max(1, std::atoi(e));
    std::atomic<size_t> next{0}, done{0};
    std::atomic<uint64_t> driver_ns{0}, slowest{0}, describe_ns{0}, lock_wait_ns{0}, cpu_ns{0};
    const uint64_t t0 = plat::monotonic_ns();
    const ObjectsLockWaits others0 = objects_lock_waits();
    const bool progress = !background;
    VkPipelineCache cache = pipeline_cache();
    auto work = [&] {
        set_worker_priority(background);
        mark_catalog_worker_thread();
        for (size_t i; (i = next.fetch_add(1)) < pw.count;) {
            Job& job = pw.jobs[i];
            uint8_t want = kPending;
            if (job.state.compare_exchange_strong(want, kBuilding)) {
                VkPipeline p = VK_NULL_HANDLE;
                uint64_t a = 0;
                const uint64_t d0 = plat::monotonic_ns(), w0 = objects_lock_wait_ns_this_thread();
                if (job.record.kind == kCompute) {
                    const ProgramLayout& layout = program_layout({job.cs});
                    a = plat::monotonic_ns();  // includes making the module (graphics: made by describe, before)
                    describe_ns += a - d0;
                    p = create_compute_pipeline(job.cs, layout);
                } else {
                    std::vector<const ShaderInfo*> stages;
                    if (job.key.vs) stages.push_back(job.key.vs);
                    if (job.key.fs) stages.push_back(job.key.fs);
                    const ProgramLayout& layout = program_layout(stages);
                    auto desc = describe_graphics_pipeline(job.key, layout);
                    a = plat::monotonic_ns();
                    describe_ns += a - d0;
                    if (p_vkCreateGraphicsPipelines(vk_device(), cache, 1, &desc->ci, nullptr, &p) != VK_SUCCESS)
                        p = VK_NULL_HANDLE;
                }
                const uint64_t ns = plat::monotonic_ns() - a;
                driver_ns += ns;
                lock_wait_ns += objects_lock_wait_ns_this_thread() - w0;
                for (uint64_t s = slowest.load(); ns > s && !slowest.compare_exchange_weak(s, ns);) {}
                if (p && !keep) p_vkDestroyPipeline(vk_device(), p, nullptr);
                {
                    std::lock_guard lk(pw.lock);
                    job.pipeline = keep ? p : VK_NULL_HANDLE;
                    job.state = !p ? kFailed : keep ? kReady : kTaken;  // not kept: nothing will take it, no game runs
                    (p ? pw.made : pw.failed)++;
                }
                pw.cv.notify_all();
            }
            size_t d = ++done;
            if (progress && (d == pw.count || d % std::max<size_t>(1, pw.count / 20) == 0))
                log("nvn/vk: pipeline catalog: progress %zu/%zu", d, pw.count);
        }
#ifdef _WIN32
        FILETIME c, e, k, u;  // this worker's CPU time: below its wall time, it was waiting or not scheduled
        if (GetThreadTimes(GetCurrentThread(), &c, &e, &k, &u))
            cpu_ns += ((uint64_t(k.dwHighDateTime) << 32 | k.dwLowDateTime) +
                       (uint64_t(u.dwHighDateTime) << 32 | u.dwLowDateTime)) * 100;
#endif
    };
    std::vector<std::thread> pool;
    for (unsigned k = 0; k < threads; k++) pool.emplace_back(work);
    for (auto& t : pool) t.join();
    log("nvn/vk: pipeline catalog: %zu pipelines (%zu compute) in %.1f ms on %u threads (%llu made, %llu failed, %llu "
        "taken over by draws before a worker reached them; %.1f ms of driver time, the slowest %.1f ms)",
        pw.count, pw.compute, double(plat::monotonic_ns() - t0) / 1e6, threads, (unsigned long long)pw.made.load(),
        (unsigned long long)pw.failed.load(), (unsigned long long)pw.by_game.load(), double(driver_ns.load()) / 1e6,
        double(slowest.load()) / 1e6);
    // Where the workers' time went (§5zd9): describing (shader modules, layouts: under the lock the replay shares,
    // the waits for it included), the driver, and the CPU time they got; and how long other threads (the replay)
    // waited for that lock meanwhile.
    const ObjectsLockWaits others = objects_lock_waits();
    log("nvn/vk: pipeline catalog: workers: %.1f ms describing (%.1f ms of it waiting for the shared lock), %.1f ms "
        "in the driver, %.1f ms of CPU over %.1f ms of wall time x %u threads; other threads waited for the shared "
        "lock %llu times, %.1f ms in all (the longest wait of the run so far %.1f ms)",
        double(describe_ns.load()) / 1e6, double(lock_wait_ns.load()) / 1e6, double(driver_ns.load()) / 1e6,
        double(cpu_ns.load()) / 1e6, double(plat::monotonic_ns() - t0) / 1e6, threads,
        (unsigned long long)(others.count - others0.count), double(others.ns - others0.ns) / 1e6,
        double(others.max_ns) / 1e6);
}

// A pipeline the game needs: ready (taken), being made (waited for), not started (the caller makes it), or not in
// the catalog. `game_a`, `game_b`: the game's stages, for NX_PIPELINE_VERIFY.
VkPipeline take(const Record& rec, const ShaderInfo* game_a, const ShaderInfo* game_b) {
    Prewarmed& pw = prewarmed();
    if (!pw.active.load(std::memory_order_acquire)) return VK_NULL_HANDLE;
    auto it = pw.index.find(portable(rec));
    if (it == pw.index.end()) {
        uint64_t n = ++pw.not_in_catalog;
        if (n <= 40) {
            if (rec.kind == kCompute)
                log("nvn/vk: pipeline catalog: not in it: compute %s (shared %u bytes)", hash_of(rec.vs).c_str(),
                    rec.shared);
            else
                log("nvn/vk: pipeline catalog: not in it: vs %s fs %s (topology %u, %u targets, depth format %d)",
                    rec.vs[0] ? hash_of(rec.vs).c_str() : "-", rec.fs[0] ? hash_of(rec.fs).c_str() : "-",
                    rec.key.topology, rec.key.color_count, int(rec.key.depth_format));
        }
        return VK_NULL_HANDLE;
    }
    Job& job = pw.jobs[it->second];
    if (verify_on()) verify_take(job, game_a, game_b);
    std::unique_lock lk(pw.lock);
    for (;;) {
        uint8_t st = job.state.load();
        if (st == kReady) {
            job.state = kTaken;
            uint64_t n = ++pw.served;
            if (n == 1 || n % 250 == 0)
                log("nvn/vk: pipeline catalog: %llu taken ready, %llu waited for, %llu made by draws first, %llu not "
                    "in the catalog", (unsigned long long)n, (unsigned long long)pw.waited.load(),
                    (unsigned long long)pw.by_game.load(), (unsigned long long)pw.not_in_catalog.load());
            return job.pipeline;
        }
        if (st == kPending) {  // no worker reached it: the draw makes it now (and no worker will)
            uint8_t want = kPending;
            if (job.state.compare_exchange_strong(want, kByGame)) {
                pw.by_game++;
                return VK_NULL_HANDLE;
            }
            continue;
        }
        if (st == kBuilding) {  // a worker is on it: waiting costs less than compiling it twice
            pw.waited++;
            pw.cv.wait(lk, [&] { return job.state.load() != kBuilding; });
            continue;
        }
        return VK_NULL_HANDLE;  // failed, or taken/claimed before (not expected: the replay keeps what it took)
    }
}

}  // namespace

// A collection session keeps only what the installed catalogs lack (what this PC still compiles while playing).
bool in_installed_catalogs(const Record& rec) {
    Prewarmed& pw = prewarmed();
    return pw.active.load(std::memory_order_acquire) && pw.index.count(portable(rec));
}

bool catalog_note(const PipelineKey& key) {
    if (record_path().empty()) return false;
    const Record rec = record_of(key);
    if (collect::on() && in_installed_catalogs(rec)) return false;
    return note_record(rec);
}

bool catalog_note_compute(const ShaderInfo* cs) {
    if (!cs || record_path().empty()) return false;
    const Record rec = compute_record_of(cs);
    if (collect::on() && in_installed_catalogs(rec)) return false;
    return note_record(rec);
}

VkPipeline catalog_take(const PipelineKey& key) { return take(record_of(key), key.vs, key.fs); }

VkPipeline catalog_take_compute(const ShaderInfo* cs) { return cs ? take(compute_record_of(cs), cs, nullptr) : VK_NULL_HANDLE; }

void catalog_prewarm_start() {
    std::string all;
    if (std::string list; plat::env_utf8("NX_PIPELINE_PREWARM", list)) {  // paths: UTF-8 (accents)
        if (list.empty() || list == "0") return;
        all = list;
    } else {  // the installed game: the catalogs in its data folder, if the preparer put any there
        for (const char* name : {"/pipeline_catalog.bin", "/pipeline_catalog_local.bin"}) {
            const std::string path = plat::data_root() + name;
            if (FILE* f = plat::fopen_utf8(path, "rb")) {
                std::fclose(f);
                all += (all.empty() ? "" : ";") + path;
            }
        }
        if (all.empty()) return;
    }
    const std::string m = mode();
    if (std::getenv("NX_SPIRV_SALT")) log("nvn/vk: pipeline catalog: NX_SPIRV_SALT is set (measurement only)");

    // Every catalog listed, its keys made canonical with this run's stages, without duplicates.
    std::vector<Record> records;
    uint64_t digest = 0xcbf29ce484222325ull;
    size_t damaged = 0, files = 0;
    for (size_t at = 0; at <= all.size();) {
        size_t end = all.find(';', at);
        if (end == std::string::npos) end = all.size();
        std::string path = all.substr(at, end - at);
        at = end + 1;
        if (path.empty()) continue;
        CatalogFile c = read_catalog(path);
        files += c.ok;
        damaged += c.damaged;
        records.insert(records.end(), c.records.begin(), c.records.end());
    }
    Prewarmed& pw = prewarmed();
    pw.jobs.reset(new Job[records.size()]);
    size_t missing = 0;
    const FixedStateEnv& env = fixed_state_env();
    for (const Record& r : records) {
        Record rec;
        PipelineKey key{};
        const ShaderInfo* cs = nullptr;
        if (r.kind == kCompute) {
            cs = compute_shader_for_pipeline(hash_of(r.vs), r.shared);
            if (!cs) {
                missing++;
                continue;
            }
            rec = compute_record_of(cs);
        } else {
            std::memcpy(&key, &r.key, sizeof key);
            std::string vh = hash_of(r.vs), fh = hash_of(r.fs);
            key.vs = vh.empty() ? nullptr : shader_for_pipeline(vh);
            key.fs = fh.empty() ? nullptr : shader_for_pipeline(fh);
            if ((!vh.empty() && !key.vs) || (!fh.empty() && !key.fs)) {
                missing++;
                continue;
            }
            key = canonical(key, env);
            rec = record_of(key);
        }
        if (!pw.index.emplace(portable(rec), pw.count).second) continue;
        Job& job = pw.jobs[pw.count++];
        job.record = rec;
        job.key = key;
        job.cs = cs;
        pw.compute += cs != nullptr;
        digest = fnv1a(&job.record, sizeof job.record, digest);  // in catalog order: the same files, the same digest
    }
    collect::event("catalog", "%zu pipelines (%zu compute) from %zu file(s), mode %s", pw.count, pw.compute, files,
                   m.c_str());
    log("nvn/vk: pipeline catalog: %zu pipelines (%zu compute) from %zu file(s) (%zu records, %zu with a missing "
        "shader, %zu damaged entries); mode %s", pw.count, pw.compute, files, records.size(), missing, damaged,
        m.c_str());
    if (!pw.count) return;
    pipeline_cache();  // created here, once, before any worker uses it

    const std::string stamp = stamp_text(digest);
    if (m == "exit") {
        std::string old;
        if (FILE* f = plat::fopen_utf8(stamp_path(), "rb")) {
            char buf[4096];
            old.resize(std::fread(buf, 1, sizeof buf, f));
            std::memcpy(old.data(), buf, old.size());
            std::fclose(f);
        }
        if (old == stamp && pipeline_cache_bytes() > 0) {
            log("nvn/vk: pipeline catalog: already prepared for this GPU, driver, runtime, catalog and SPIR-V: "
                "nothing to do; exiting");
            plat::exit_process(0);
        }
    }
    pw.active.store(true, std::memory_order_release);
    if (m == "background") {
        std::thread(run_workers, true, true).detach();
        return;
    }
    run_workers(false, m != "exit");  // wait / exit: the game starts (or the process ends) once every pipeline is made
    if (m == "exit") {
        // The stamp says "done" only for a complete preparation whose cache is on disk: a preparation with failures,
        // or whose cache could not be written, is redone next time (what was compiled stays in the driver's cache).
        const bool saved = pipeline_cache_save();
        const bool stamped = saved && pw.failed == 0 && write_stamp(stamp);
        log("nvn/vk: pipeline catalog: prepared (%llu made, %llu failed), pipeline cache %s (%zu bytes), stamp %s; "
            "exiting", (unsigned long long)pw.made.load(), (unsigned long long)pw.failed.load(),
            saved ? "saved" : "NOT saved", pipeline_cache_bytes(), stamped ? "written" : "not written");
        plat::exit_process(pw.failed ? 4 : saved ? 0 : 5);
    }
}

}  // namespace nx::nvn
