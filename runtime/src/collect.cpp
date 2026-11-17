// Collection sessions (collect.h).
#include "collect.h"

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <mutex>

#include <nx/runtime.h>

#include "platform.h"

namespace nx::collect {

namespace {

struct Session {
    std::mutex lock;
    FILE* file = nullptr;
    uint64_t start_ns = 0;
    uint64_t lines = 0;
};

Session& session() {
    static Session s;
    return s;
}

std::string stamp(bool for_name) {
    const auto now = std::chrono::system_clock::now();
    const std::time_t t = std::chrono::system_clock::to_time_t(now);
    const int ms = int(std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[48];
    if (for_name)
        std::strftime(buf, sizeof buf, "%Y%m%d-%H%M%S", &tm);
    else {
        std::size_t n = std::strftime(buf, sizeof buf, "%Y-%m-%d %H:%M:%S", &tm);
        std::snprintf(buf + n, sizeof buf - n, ".%03d", ms);
    }
    return buf;
}

}  // namespace

const std::string& dir() {
    static const std::string d = plat::data_root() + "/collect";
    return d;
}

bool on() {
    static const bool enabled = [] {
        const char* e = std::getenv("NX_COLLECT");
        if (e && e[0] == '0') return false;
        const bool forced = e && e[0] == '1';
        const char* headless = std::getenv("NX_HEADLESS");
        if (!forced && headless && headless[0] == '1') return false;  // the preparer's check, test harnesses
        if (!forced) {
            FILE* f = plat::fopen_utf8(dir() + "/ENABLED", "rb");
            if (!f) return false;
            std::fclose(f);
        }
        plat::make_dirs(dir() + "/sessions/");
        Session& s = session();
        const std::string path = dir() + "/sessions/" + stamp(true) + ".tsv";
        s.file = plat::fopen_utf8(path, "ab");
        s.start_ns = plat::monotonic_ns();
        if (!s.file) {
            log("collect: cannot write %s: collection off", path.c_str());
            return false;
        }
        log("collect: collection session on: %s", path.c_str());
        // the first line: what this session is (the time a reader reads every other line against)
        std::fprintf(s.file, "%s\t0.000\tsession\tstart\tdata %s\n", stamp(false).c_str(), plat::data_root().c_str());
        std::fflush(s.file);
        return true;
    }();
    return enabled;
}

void event(const char* kind, const char* fmt, ...) {
    if (!on()) return;
    char text[1024];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(text, sizeof text, fmt, ap);
    va_end(ap);
    Session& s = session();
    std::lock_guard lk(s.lock);
    if (!s.file || s.lines >= 2'000'000) return;  // a bound for a session left running for days
    s.lines++;
    std::fprintf(s.file, "%s\t%.3f\t%s\t%s\n", stamp(false).c_str(), double(plat::monotonic_ns() - s.start_ns) / 1e9,
                 kind, text);
    std::fflush(s.file);  // a crash or a closed window keeps every line before it
}

}  // namespace nx::collect
