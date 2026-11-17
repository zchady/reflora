// The monitor's real vertical blanks (Windows), for the display model's phase (nvn_queue.cpp) and the present log.
//
// A thread waits for each vblank of the monitor the window is on (D3DKMTWaitForVerticalBlankEvent, gdi32: the wait the
// compositor itself is paced by; it reads, it changes nothing) and keeps a filtered grid: the time of the last
// vblank and the refresh period. DwmGetCompositionTimingInfo, used before, describes the compositor's own schedule
// on the primary monitor and its vblank time jumped by milliseconds between calls (§5w): good enough for a period,
// not for a phase. NX_VBLANK=dwm keeps that source; display_vblank (platform_win.cpp) falls back to it when this one
// has no fresh vblank (window minimized, display off, the wait not available).
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>

#include <windows.h>

#include <nx/runtime.h>

#include "platform.h"

namespace nx {
extern HWND g_window;
}

namespace nx::plat {

namespace {

// d3dkmthk.h (WDK), the two calls used here.
using D3dkmtHandle = UINT;
struct OpenAdapterFromHdc {
    HDC hDc;
    D3dkmtHandle hAdapter;
    LUID AdapterLuid;
    UINT VidPnSourceId;
};
struct WaitForVerticalBlankEvent {
    D3dkmtHandle hAdapter;
    D3dkmtHandle hDevice;
    UINT VidPnSourceId;
};
struct CloseAdapter {
    D3dkmtHandle hAdapter;
};
using OpenFn = LONG(APIENTRY*)(OpenAdapterFromHdc*);
using WaitFn = LONG(APIENTRY*)(const WaitForVerticalBlankEvent*);
using CloseFn = LONG(APIENTRY*)(const CloseAdapter*);

std::mutex g_lock;
uint64_t g_vblank_ns = 0;   // the filtered time of the last vblank
double g_period_ns = 0;     // the filtered refresh period
uint64_t g_seen_ns = 0;     // when the thread last returned from a wait
std::atomic<bool> g_started{false};

bool source_dwm() {
    static const bool dwm = [] {
        const char* e = std::getenv("NX_VBLANK");
        return e && std::strcmp(e, "dwm") == 0;
    }();
    return dwm;
}

void vblank_thread() {
    set_thread_name("vblank");
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    HMODULE gdi = GetModuleHandleW(L"gdi32.dll");
    auto open = gdi ? reinterpret_cast<OpenFn>(GetProcAddress(gdi, "D3DKMTOpenAdapterFromHdc")) : nullptr;
    auto wait = gdi ? reinterpret_cast<WaitFn>(GetProcAddress(gdi, "D3DKMTWaitForVerticalBlankEvent")) : nullptr;
    auto close = gdi ? reinterpret_cast<CloseFn>(GetProcAddress(gdi, "D3DKMTCloseAdapter")) : nullptr;
    if (!open || !wait || !close) {
        log("vblank: D3DKMT waits not available, the compositor's timing is used");
        return;
    }
    wchar_t device[CCHDEVICENAME] = L"";
    OpenAdapterFromHdc adapter{};
    bool opened = false, logged = false;
    uint64_t checked_ns = 0, fails = 0;
    for (;;) {
        uint64_t now = monotonic_ns();
        // The monitor the window is on, looked at again every 2 s (the window may have been moved).
        if (!opened || now - checked_ns > 2'000'000'000ull) {
            checked_ns = now;
            MONITORINFOEXW mi{};
            mi.cbSize = sizeof mi;
            HMONITOR mon = g_window ? MonitorFromWindow(g_window, MONITOR_DEFAULTTOPRIMARY) : nullptr;
            if (mon && GetMonitorInfoW(mon, &mi) && (!opened || std::wcscmp(device, mi.szDevice) != 0)) {
                if (opened) {
                    CloseAdapter c{adapter.hAdapter};
                    close(&c);
                    opened = false;
                }
                if (HDC dc = CreateDCW(mi.szDevice, mi.szDevice, nullptr, nullptr)) {
                    adapter = {};
                    adapter.hDc = dc;
                    opened = open(&adapter) >= 0;
                    DeleteDC(dc);
                }
                std::wcscpy(device, mi.szDevice);
                std::lock_guard lk(g_lock);
                g_vblank_ns = 0, g_period_ns = 0;  // a new grid
            }
        }
        if (!opened) {
            Sleep(200);
            continue;
        }
        WaitForVerticalBlankEvent w{adapter.hAdapter, 0, adapter.VidPnSourceId};
        if (wait(&w) < 0) {  // the display is off, the session locked...
            if (++fails % 50 == 1) log("vblank: the wait for the monitor's vblank failed (%llu times)", (unsigned long long)fails);
            Sleep(100);
            continue;
        }
        uint64_t t = monotonic_ns();
        std::lock_guard lk(g_lock);
        g_seen_ns = t;
        if (!g_vblank_ns) {
            g_vblank_ns = t;
            continue;
        }
        double dt = double(t - g_vblank_ns);
        if (g_period_ns <= 0) {  // the first interval: a plausible refresh (20-500 Hz), refined below
            if (dt > 2e6 && dt < 50e6) g_period_ns = dt;
            g_vblank_ns = t;
            continue;
        }
        // The wake-up is late by the scheduler's latency, never early: the grid follows it slowly (a tenth of the
        // error on the phase, a hundredth on the period), and a wake-up far from the grid (vblanks missed while the
        // thread did not run, a mode change) only moves the grid when it repeats.
        double n = std::max(1.0, std::round(dt / g_period_ns));
        double err = dt - n * g_period_ns;
        static int off = 0;
        if (std::fabs(err) < g_period_ns * 0.25) {
            off = 0;
            g_period_ns += 0.01 * err / n;
            g_vblank_ns += uint64_t(int64_t(n * g_period_ns + 0.1 * err));
            if (!logged && n == 1) {
                static int settled = 0;
                if (++settled == 240) {
                    logged = true;
                    log("vblank: monitor %ls at %.3f Hz (period %.4f ms)", device, 1e9 / g_period_ns, g_period_ns / 1e6);
                }
            }
        } else if (++off >= 8) {
            off = 0;
            g_vblank_ns = t, g_period_ns = 0;
            logged = false;
        }
    }
}

}  // namespace

// The filtered grid of the window's monitor: false when there is none or it is stale.
bool monitor_vblank(uint64_t& vblank_ns, uint64_t& period_ns) {
    if (source_dwm() || headless()) return false;
    if (!g_started.exchange(true)) std::thread(vblank_thread).detach();
    std::lock_guard lk(g_lock);
    if (!g_vblank_ns || g_period_ns <= 0 || monotonic_ns() - g_seen_ns > 200'000'000ull) return false;
    vblank_ns = g_vblank_ns, period_ns = uint64_t(g_period_ns + 0.5);
    return true;
}

}  // namespace nx::plat
