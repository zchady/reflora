// nn::hid at the API level: player 1's Npad state comes from the platform (XInput + keyboard on Windows, game
// controllers + touch on Android). Vibration is accepted and dropped.
#include <array>
#include <atomic>
#include <deque>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include <nx/runtime.h>

#include "hooks.h"
#include "platform.h"

namespace nx {

namespace {

using namespace plat;

constexpr uint32_t kStyleFullKey = 1u << 0;
uint64_t g_sampling = 0;

// Scripted input for automated runs: NX_INPUT="3300:A,3900:A,4200:Down" holds the button for 8 frames from the
// given presented frame.
struct Scripted {
    uint64_t frame;
    uint64_t buttons;
};

std::vector<Scripted> input_script() {
    std::vector<Scripted> v;
    const char* e = std::getenv("NX_INPUT");
    if (!e) return v;
    std::string s = e;
    size_t pos = 0;
    while (pos < s.size()) {
        size_t comma = s.find(',', pos);
        std::string item = s.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
        size_t colon = item.find(':');
        if (colon != std::string::npos) {
            std::string b = item.substr(colon + 1);
            uint64_t bits = b == "A" ? kA : b == "B" ? kB : b == "X" ? kX : b == "Y" ? kY : b == "Plus" ? kPlus :
                            b == "Minus" ? kMinus : b == "Up" ? kUp : b == "Down" ? kDown : b == "Left" ? kLeft :
                            b == "Right" ? kRight : b == "L" ? kL : b == "R" ? kR : 0;
            v.push_back({std::strtoull(item.c_str(), nullptr, 10), bits});
        }
        if (comma == std::string::npos) break;
        pos = comma + 1;
    }
    return v;
}

PadState read() {
    PadState p;
    static const std::vector<Scripted> script = input_script();
    for (const auto& s : script)
        if (g_frame >= s.frame && g_frame < s.frame + 8) p.buttons |= s.buttons;
    read_pad(p);
    if (hooks::pad_filter && hooks::pad_filter(p)) p = PadState{};  // the settings overlay has the pad
    // Stick directions as buttons, like the hid sysmodule reports them.
    constexpr int32_t t = kStickMax / 2;
    if (p.lx < -t) p.buttons |= kStickLLeft;
    if (p.lx > t) p.buttons |= kStickLRight;
    if (p.ly > t) p.buttons |= kStickLUp;
    if (p.ly < -t) p.buttons |= kStickLDown;
    if (p.rx < -t) p.buttons |= kStickRLeft;
    if (p.rx > t) p.buttons |= kStickRRight;
    if (p.ry > t) p.buttons |= kStickRUp;
    if (p.ry < -t) p.buttons |= kStickRDown;
    return p;
}

// NpadFullKeyState / NpadJoyDualState / NpadHandheldState:
// {s64 sampling; u64 buttons; {s32 x, y} left; {s32 x, y} right; u32 attributes; pad}
void GetNpadState(CPU& c) {
    uint64_t out = c.x[0];
    uint32_t id = ld<uint32_t>(c.x[1]);
    std::memset(reinterpret_cast<void*>(out), 0, 0x28);
    st<int64_t>(out, int64_t(++g_sampling));
    {
        static std::set<std::pair<uint32_t, std::string>> seen;
        static std::mutex m;
        std::lock_guard lk(m);
        if (seen.insert({id, hle_current_name()}).second) log("hid: reads id %#x via %s", id, hle_current_name());
    }
    if (id != 0) return;  // only player 1 is connected
    PadState p = read();
    static bool pressed = false;
    if (p.buttons && !pressed) log("hid: first buttons %llx", (unsigned long long)p.buttons), pressed = true;
    st<uint64_t>(out + 8, p.buttons);
    st<int32_t>(out + 16, p.lx);
    st<int32_t>(out + 20, p.ly);
    st<int32_t>(out + 24, p.rx);
    st<int32_t>(out + 28, p.ry);
    st<uint32_t>(out + 32, 1);  // IsConnected
}

// GetNpadStates(State* out, int count, const u32& id) -> number written: the newest first. One sample: the current
// one for player 1's full-key style, a not-connected one otherwise.
// GetNpadStates(State* out, int count, const u32& id) -> number written, newest first. Like the real hid, a history:
// Wonder ignores the return value and reads all 16 entries it asks for, telling new samples by their sampling
// numbers, so every call adds a sample and returns up to 16 of the latest (older entries stay as they were).
void GetNpadStatesFullKey(CPU& c) {
    {
        static std::atomic<uint64_t> calls{0};
        uint64_t n = ++calls;
        if (n % 3000 == 0) log("hid: %llu full-key reads (lr %s)", (unsigned long long)n, describe(c.x[30]).c_str());
    }
    uint64_t out = c.x[0], id_ref = c.x[2];
    int n = int(c.x[1]);
    if (n < 1) {
        c.x[0] = 0;
        return;
    }
    static std::mutex m;
    static std::deque<std::array<uint8_t, 0x28>> history;
    std::lock_guard lk(m);
    c.x[0] = out;
    c.x[1] = id_ref;
    GetNpadState(c);  // the new sample, into out[0]
    if (ld<uint32_t>(id_ref) != 0) {
        c.x[0] = 1;
        return;
    }
    std::array<uint8_t, 0x28> now;
    std::memcpy(now.data(), reinterpret_cast<const void*>(out), now.size());
    history.push_front(now);
    if (history.size() > 16) history.pop_back();
    int count = std::min<int>(n, int(history.size()));
    for (int k = 1; k < count; k++) std::memcpy(reinterpret_cast<void*>(out + 0x28ull * k), history[k].data(), 0x28);
    c.x[0] = uint64_t(count);
    // NX_TRACE_HID=1: every change of the buttons read (time, reads since the last change, entries asked for), and
    // the read rate once a second: how the game samples the pad (inputs shorter than its sampling get lost).
    static const bool trace = std::getenv("NX_TRACE_HID") != nullptr;
    if (trace) {
        static uint64_t last_buttons = 0, reads = 0, since = 0, t_rate = 0, rate_reads = 0;
        uint64_t b = ld<uint64_t>(out + 8), now = plat::monotonic_ns();
        reads++, since++, rate_reads++;
        if (b != last_buttons) {
            log("hid trace: %.3f s buttons %llx -> %llx after %llu reads (asked %d, sampling %lld)", double(now) / 1e9,
                (unsigned long long)last_buttons, (unsigned long long)b, (unsigned long long)since, n,
                (long long)ld<int64_t>(out));
            last_buttons = b, since = 0;
        }
        if (now - t_rate > 1'000'000'000ull) {
            if (t_rate) log("hid trace: %llu reads in the last second", (unsigned long long)rate_reads);
            t_rate = now, rate_reads = 0;
        }
    }
}

void GetNpadStatesOther(CPU& c) {
    {
        static std::set<std::pair<uint32_t, std::string>> seen;
        static std::mutex m;
        std::lock_guard lk(m);
        uint32_t id = ld<uint32_t>(c.x[2]);
        if (seen.insert({id, hle_current_name()}).second) log("hid: reads id %#x via %s (not connected)", id, hle_current_name());
    }
    if (int(c.x[1]) < 1) {
        c.x[0] = 0;
        return;
    }
    std::memset(reinterpret_cast<void*>(c.x[0]), 0, 0x28);
    st<int64_t>(c.x[0], int64_t(g_sampling));
    c.x[0] = 1;
}

// MouseState {s64 sampling; s32 x, y, dx, dy, wheel dx, wheel dy; u32 buttons; u32 attributes}: no mouse.
void GetMouseStates(CPU& c) {
    if (int(c.x[1]) < 1) {
        c.x[0] = 0;
        return;
    }
    std::memset(reinterpret_cast<void*>(c.x[0]), 0, 0x28);
    st<int64_t>(c.x[0], int64_t(g_sampling));
    c.x[0] = 1;
}

// KeyboardState {s64 sampling; u64 modifiers; u8 keys[32]}: nothing pressed (the keyboard drives the pad).
void GetKeyboardState(CPU& c) {
    std::memset(reinterpret_cast<void*>(c.x[0]), 0, 0x30);
    st<int64_t>(c.x[0], int64_t(g_sampling));
}

void zero(CPU& c) { c.x[0] = 0; }
void one(CPU& c) { c.x[0] = 1; }

void GetNpadStyleSet(CPU& c) {
    static int logged = 0;
    if (logged++ < 4) log("hid: GetNpadStyleSet id %#x", ld<uint32_t>(c.x[0]));
    c.x[0] = ld<uint32_t>(c.x[0]) == 0 ? kStyleFullKey : 0;
}

void BindNpadStyleSetUpdateEvent(CPU& c) {  // (const u32& id, SystemEventType*, EventClearMode)
    uint64_t create = find_symbol("_ZN2nn2os17CreateSystemEventEPNS0_15SystemEventTypeENS0_14EventClearModeEb");
    uint64_t ev = c.x[1], mode = c.x[2];
    uint32_t id = ld<uint32_t>(c.x[0]);
    c.x[0] = ev;
    c.x[1] = mode;
    c.x[2] = 0;
    call(c, create);
    // Player 1's controller is there from the start: the style set "changed" (connected) once, and games wait for
    // this event before they look at the npad.
    static uint64_t signal = find_symbol("_ZN2nn2os17SignalSystemEventEPNS0_15SystemEventTypeE");
    if (signal) {
        c.x[0] = ev;
        call(c, signal);
    }
    log("hid: style set event bound for id %#x, signalled", id);
}

void GetVibrationDeviceHandles(CPU& c) { c.x[0] = 0; }  // no vibration devices
void ShowControllerSupport(CPU& c) {                     // (ControllerSupportResultInfo*, const Arg&)
    st<int8_t>(c.x[0], 1);        // player count
    st<uint32_t>(c.x[0] + 4, 0);  // selected npad id: No1
    c.x[0] = 0;
}
void noop(CPU& c) { c.x[0] = 0; }

}  // namespace

// (int* out_count, NmouseState*, int, NmouseHandle): no mouse, no states.
static void GetNmouseStates(CPU& c) {
    if (c.x[0]) st<int32_t>(c.x[0], 0);
    c.x[0] = 0;
}
// (BitFlagSet<32, NmouseType>* out, NmouseHandle): no types.
static void GetNmouseTypeSet(CPU& c) {
    if (c.x[0]) st<uint32_t>(c.x[0], 0);
}

NX_HLE("_ZN2nn3hid14InitializeNpadEv", noop);
NX_HLE("_ZN2nn3hid24SetSupportedNpadStyleSetENS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEE", noop);
NX_HLE("_ZN2nn3hid18SetNpadJoyHoldTypeENS0_15NpadJoyHoldTypeE", noop);
NX_HLE("_ZN2nn3hid22SetSupportedNpadIdTypeEPKjm", noop);
NX_HLE("_ZN2nn3hid27BindNpadStyleSetUpdateEventERKjPNS_2os15SystemEventTypeENS3_14EventClearModeE", BindNpadStyleSetUpdateEvent);
NX_HLE("_ZN2nn3hid15GetNpadStyleSetERKj", GetNpadStyleSet);
NX_HLE("_ZN2nn3hid12GetNpadStateEPNS0_16NpadFullKeyStateERKj", GetNpadState);
NX_HLE("_ZN2nn3hid12GetNpadStateEPNS0_16NpadJoyDualStateERKj", GetNpadState);
NX_HLE("_ZN2nn3hid12GetNpadStateEPNS0_17NpadHandheldStateERKj", GetNpadState);
NX_HLE("_ZN2nn3hid25GetVibrationDeviceHandlesEPNS0_21VibrationDeviceHandleEiRKjNS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEE", GetVibrationDeviceHandles);
NX_HLE("_ZN2nn3hid25InitializeVibrationDeviceERKNS0_21VibrationDeviceHandleE", noop);
NX_HLE("_ZN2nn3hid22GetVibrationDeviceInfoEPNS0_19VibrationDeviceInfoERKNS0_21VibrationDeviceHandleE", noop);
NX_HLE("_ZN2nn3hid21ShowControllerSupportEPNS0_27ControllerSupportResultInfoERKNS0_20ControllerSupportArgE", ShowControllerSupport);
// SDK 16 (Super Mario Bros. Wonder)
NX_HLE("_ZN2nn3hid13GetNpadStatesEPNS0_16NpadFullKeyStateEiRKj", GetNpadStatesFullKey);
NX_HLE("_ZN2nn3hid13GetNpadStatesEPNS0_16NpadJoyDualStateEiRKj", GetNpadStatesOther);
NX_HLE("_ZN2nn3hid13GetNpadStatesEPNS0_16NpadJoyLeftStateEiRKj", GetNpadStatesOther);
NX_HLE("_ZN2nn3hid13GetNpadStatesEPNS0_17NpadHandheldStateEiRKj", GetNpadStatesOther);
NX_HLE("_ZN2nn3hid13GetNpadStatesEPNS0_17NpadJoyRightStateEiRKj", GetNpadStatesOther);
NX_HLE("_ZN2nn3hid15InitializeMouseEv", noop);
NX_HLE("_ZN2nn3hid14GetMouseStatesEPNS0_10MouseStateEi", GetMouseStates);
NX_HLE("_ZN2nn3hid18InitializeKeyboardEv", noop);
NX_HLE("_ZN2nn3hid16GetKeyboardStateEPNS0_13KeyboardStateE", GetKeyboardState);
NX_HLE("_ZN2nn3hid14DisconnectNpadERKj", noop);
NX_HLE("_ZN2nn3hid18SwapNpadAssignmentERKjS2_", noop);
NX_HLE("_ZN2nn3hid30SetNpadJoyAssignmentModeSingleERKj", noop);
NX_HLE("_ZN2nn3hid21StartLrAssignmentModeEv", noop);
NX_HLE("_ZN2nn3hid20StopLrAssignmentModeEv", noop);
NX_HLE("_ZN2nn3hid20GetNpadJoyAssignmentERKj", zero);  // dual
NX_HLE("_ZN2nn3hid20IsVibrationPermittedEv", one);
NX_HLE("_ZN2nn3hid18SendVibrationValueERKNS0_21VibrationDeviceHandleERKNS0_14VibrationValueE", noop);
NX_HLE("_ZN2nn3hid23GetSixAxisSensorHandlesEPNS0_19SixAxisSensorHandleEiRKjNS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEE", noop);
NX_HLE("_ZN2nn3hid18StartSixAxisSensorERKNS0_19SixAxisSensorHandleE", noop);
NX_HLE("_ZN2nn3hid22GetSixAxisSensorStatesEPNS0_18SixAxisSensorStateEiRKNS0_19SixAxisSensorHandleE", zero);
// SDK 21 (Wonder, Switch 2 Edition): the Joy-Con 2 mouse devices (Jcms, Nmouse). There are none on the PC: initialize and
// start succeed, handle lists and state reads return no entries. Without these the SDK's own code asks the hid
// service for its applet resource, which the PC runtime does not provide (hid input is HLE at this level).
NX_HLE("_ZN2nn3hid14InitializeJcmsEPvm", noop);
NX_HLE("_ZN2nn3hid12FinalizeJcmsEv", noop);
NX_HLE("_ZN2nn3hid9StartJcmsERKNS0_10JcmsHandleE", noop);
NX_HLE("_ZN2nn3hid8StopJcmsERKNS0_10JcmsHandleE", noop);
NX_HLE("_ZN2nn3hid13GetJcmsHandleERKjRKNS0_8JcmsTypeE", zero);
NX_HLE("_ZN2nn3hid13GetJcmsStatesEPNS0_9JcmsStateEiRKNS0_10JcmsHandleE", zero);
NX_HLE("_ZN2nn3hid14FinalizeNmouseEv", noop);
NX_HLE("_ZN2nn3hid16GetNmouseHandlesEPNS0_12NmouseHandleEij", zero);
NX_HLE("_ZN2nn3hid15GetNmouseStatesEPiPNS0_11NmouseStateEiNS0_12NmouseHandleE", GetNmouseStates);
NX_HLE("_ZN2nn3hid16GetNmouseTypeSetEPNS_4util10BitFlagSetILi32ENS0_10NmouseTypeEEENS0_12NmouseHandleE", GetNmouseTypeSet);

}  // namespace nx

namespace nx::hooks {
PadFilter pad_filter = nullptr;
}  // namespace nx::hooks
