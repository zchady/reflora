// plat::sleep_ns (runtime/src/sleep_win.cpp) lasts about what it asks for: Wonder's Switch 2 job system sleeps 20 us
// in its wait loops, which Windows' 15.6 ms timer tick had turned into ~12 ms each (WONDER_SWITCH2_PROGRESS.md §5s).
// Bounds are generous (a loaded machine can preempt us), but far below the old 4.7-16.5 ms.
#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace nx::plat {
void sleep_ns(int64_t ns);
}

static double now_ms() {
    static LARGE_INTEGER f = [] {
        LARGE_INTEGER x;
        QueryPerformanceFrequency(&x);
        return x;
    }();
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return double(t.QuadPart) * 1000.0 / double(f.QuadPart);
}

static bool check(const char* what, int64_t ns, int reps, double max_mean_ms, double max_ms) {
    std::vector<double> got;
    for (int k = 0; k < reps; k++) {
        double t0 = now_ms();
        nx::plat::sleep_ns(ns);
        got.push_back(now_ms() - t0);
    }
    double mean = 0;
    for (double v : got) mean += v;
    mean /= double(got.size());
    std::sort(got.begin(), got.end());
    double lo = got.front(), median = got[got.size() / 2];
    bool ok = lo >= double(ns) / 1e6 * 0.999 && mean <= max_mean_ms && median <= max_ms;
    std::printf("%s: asked %.3f ms, min %.3f mean %.3f median %.3f ms: %s\n", what, double(ns) / 1e6, lo, mean, median,
                ok ? "ok" : "FAIL");
    return ok;
}

int main() {
    bool ok = true;
    ok &= check("20 us (Wonder's job loops)", 20'000, 200, 1.0, 0.5);
    ok &= check("100 us", 100'000, 100, 1.2, 0.6);
    ok &= check("5 ms", 5'000'000, 20, 7.0, 6.5);
    ok &= check("16.7 ms (a vsync)", 16'666'667, 6, 19.0, 18.5);
    ok &= check("no sleep", 0, 10, 0.2, 0.1);
    return ok ? 0 : 1;
}
