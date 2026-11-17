// Performance hints (docs/nvn/ANDROID-PORT.md A5, DEBUG-2.md #84): on Android the threads that make a frame (the
// game's render thread, which also replays NVN, and the presenting one) join an ADPF session (APerformanceHint,
// Android 13+) with a 16.7 ms target, and each frame's duration is reported. The system then raises the big cores'
// clocks when a frame runs late instead of guessing from load, which on phones keeps a steady 60 fps cooler and faster
// than the default governor. The API is looked up with dlsym, so older Android versions just go without.
// NX_ADPF=0: off. Other platforms: nothing.
#include "platform.h"

#if defined(__ANDROID__)
#include <dlfcn.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>

#include <cerrno>
#include <dirent.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <map>
#include <string>

#include <algorithm>
#include <cstdlib>
#include <mutex>
#include <vector>

#include <nx/runtime.h>
#endif

namespace nx::plat {

#if defined(__ANDROID__)
namespace {

using GetManager = void* (*)();
using CreateSession = void* (*)(void*, const int32_t*, size_t, int64_t);
using ReportActual = int (*)(void*, int64_t);
using CloseSession = void (*)(void*);

struct Api {
    GetManager get_manager = nullptr;
    CreateSession create = nullptr;
    ReportActual report = nullptr;
    CloseSession close = nullptr;
    bool ok = false;
};

const Api& api() {
    static const Api a = [] {
        Api x;
        const char* e = std::getenv("NX_ADPF");
        if (e && e[0] == '0') return x;
        void* lib = dlopen("libandroid.so", RTLD_NOW);
        if (!lib) return x;
        x.get_manager = reinterpret_cast<GetManager>(dlsym(lib, "APerformanceHint_getManager"));
        x.create = reinterpret_cast<CreateSession>(dlsym(lib, "APerformanceHint_createSession"));
        x.report = reinterpret_cast<ReportActual>(dlsym(lib, "APerformanceHint_reportActualWorkDuration"));
        x.close = reinterpret_cast<CloseSession>(dlsym(lib, "APerformanceHint_closeSession"));
        x.ok = x.get_manager && x.create && x.report && x.close;
        return x;
    }();
    return a;
}

std::mutex g_lock;
std::vector<int32_t> g_tids;
void* g_session = nullptr;
bool g_dirty = false;

}  // namespace

// NX_GPU_TURBO=1 (an experiment, off by default): the Adreno kernel driver's power control off, so the GPU runs at its
// highest clock (thermal limits still apply). The idea is libadrenotools' adrenotools_set_turbo (BSD-2-Clause); this
// is written from the kernel's msm_kgsl UAPI: IOCTL_KGSL_SETPROPERTY (type 0x09, nr 0x32) with KGSL_PROP_PWRCTRL (0xE)
// and the value 0. The 30-fps lock lowered the GPU clock and frames then took longer (MEASUREMENTS.md, phase 4).
// The setting outlives the process (the GPU stayed at its top clock with the game closed), so every start sets it:
// off with NX_GPU_TURBO=1, back on (the driver's normal clock control) otherwise.
void gpu_turbo() {
    const char* e = std::getenv("NX_GPU_TURBO");
    const bool turbo = e && e[0] == '1';
    struct Property {
        unsigned int type;
        void* value;
        size_t sizebytes;
    };
    int fd = open("/dev/kgsl-3d0", O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        log("gpu turbo: /dev/kgsl-3d0 not opened (errno %d)", errno);
        return;
    }
    unsigned int enable = turbo ? 0 : 1;
    Property p{0xE, &enable, sizeof enable};
    int r = ioctl(fd, _IOW(0x09, 0x32, Property), &p);
    log("gpu turbo: KGSL power control %s: %s (errno %d)", turbo ? "off (NX_GPU_TURBO=1)" : "on (normal)",
        r == 0 ? "done" : "refused", r == 0 ? 0 : errno);
    if (!turbo) close(fd);
    // The descriptor stays open: the driver ties the setting to it.
}

void perf_hint_thread() {
    if (!api().ok) return;
    thread_local bool joined = false;
    if (joined) return;
    joined = true;
    std::lock_guard lk(g_lock);
    int32_t tid = int32_t(syscall(SYS_gettid));
    if (g_tids.size() < 8 && std::find(g_tids.begin(), g_tids.end(), tid) == g_tids.end())
        g_tids.push_back(tid), g_dirty = true;
}

// NX_ADPF_BUSY=<n> (Galaxy Tab S10+, WONDER_SWITCH2_PROGRESS.md §5zd19): the session also takes the n threads of
// the process that used the most CPU over the last 300 frames (from /proc/self/task). Wonder's Switch 2 build makes
// a frame on its main thread and 8 job workers, none of them in the session: the governor kept their cores at
// 0.8-1.0 GHz. Re-chosen every 300 frames; 0 or unset: only the threads that joined themselves (as before).
namespace {
int busy_count() {
    static const int n = [] {
        const char* e = std::getenv("NX_ADPF_BUSY");
        return e ? std::clamp(std::atoi(e), 0, 32) : 0;
    }();
    return n;
}

// tid -> utime + stime (clock ticks), and the name, of every thread of the process.
std::map<int32_t, std::pair<uint64_t, std::string>> task_times() {
    std::map<int32_t, std::pair<uint64_t, std::string>> m;
    DIR* d = opendir("/proc/self/task");
    if (!d) return m;
    while (dirent* e = readdir(d)) {
        int32_t tid = std::atoi(e->d_name);
        if (tid <= 0) continue;
        char path[64], buf[512];
        std::snprintf(path, sizeof path, "/proc/self/task/%d/stat", tid);
        FILE* f = std::fopen(path, "r");
        if (!f) continue;
        size_t n = std::fread(buf, 1, sizeof buf - 1, f);
        std::fclose(f);
        buf[n] = 0;
        char* open_paren = std::strchr(buf, '(');
        char* close_paren = std::strrchr(buf, ')');
        if (!open_paren || !close_paren) continue;
        std::string name(open_paren + 1, close_paren);
        unsigned long long ut = 0, st = 0;
        // after ")": state(3) ppid pgrp session tty tpgid flags minflt cminflt majflt cmajflt utime(14) stime(15)
        if (std::sscanf(close_paren + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %llu %llu", &ut, &st) == 2)
            m[tid] = {ut + st, name};
    }
    closedir(d);
    return m;
}

std::vector<int32_t> g_busy_tids;

// Every 300 frames: the n busiest threads since the last look. True when the set changed.
bool pick_busy_threads() {
    static uint64_t frames = 0;
    static std::map<int32_t, std::pair<uint64_t, std::string>> last;
    if (!busy_count() || ++frames % 300) return false;
    auto now = task_times();
    std::vector<std::pair<uint64_t, int32_t>> used;
    for (auto& [tid, v] : now) {
        auto it = last.find(tid);
        uint64_t d = it == last.end() ? 0 : v.first - std::min(v.first, it->second.first);
        if (d) used.push_back({d, tid});
    }
    last = std::move(now);
    std::sort(used.rbegin(), used.rend());
    std::vector<int32_t> pick;
    std::string desc;
    for (size_t k = 0; k < used.size() && pick.size() < size_t(busy_count()); k++) {
        pick.push_back(used[k].second);
        desc += (desc.empty() ? "" : ", ") + last[used[k].second].second + " " + std::to_string(used[k].first);
    }
    std::sort(pick.begin(), pick.end());
    if (pick == g_busy_tids) return false;
    g_busy_tids = pick;
    log("adpf: busiest threads (CPU ticks over 300 frames): %s", desc.c_str());
    return true;
}
}  // namespace

void perf_hint_frame(int64_t ns) {
    const Api& a = api();
    if (!a.ok || ns <= 0) return;
    std::lock_guard lk(g_lock);
    if (pick_busy_threads()) g_dirty = true;
    // NX_ADPF_AB=<frames> (with NX_ADPF_BUSY): the busiest threads are in the session only every other <frames>
    // frames, so one run compares both in the same scenes (the frame lines between two "adpf: A/B" lines belong to it).
    static const uint64_t ab = std::getenv("NX_ADPF_AB") ? std::strtoull(std::getenv("NX_ADPF_AB"), nullptr, 10) : 0;
    static uint64_t frames = 0;
    static bool with_busy = true;
    if (ab && ++frames % ab == 0) {
        with_busy = !with_busy, g_dirty = true;
        log("adpf: A/B %s from frame %llu", with_busy ? "busy" : "base", (unsigned long long)frames);
    }
    if (g_dirty && !g_tids.empty()) {  // a new thread made part of the frame: a session with all of them
        std::vector<int32_t> all = g_tids;  // those that joined themselves, then the busiest
        for (int32_t t : with_busy ? g_busy_tids : std::vector<int32_t>{})
            if (std::find(all.begin(), all.end(), t) == all.end()) all.push_back(t);
        if (g_session) a.close(g_session);
        g_session = a.create(a.get_manager(), all.data(), all.size(), 16'666'667);
        g_dirty = false;
        log("adpf: %s for %zu threads", g_session ? "session made" : "no session", all.size());
    }
    if (g_session) a.report(g_session, ns);
}
#else
void gpu_turbo() {}
void perf_hint_thread() {}
void perf_hint_frame(int64_t) {}
#endif

}  // namespace nx::plat
