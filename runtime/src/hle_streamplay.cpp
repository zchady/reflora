// nn::streamplay (Switch 2 streamed local play: guests play over the console's video stream) at the API level. The PC
// has no guests: memory and initialization succeed, nothing ever connects, guest pads and devices do not exist and
// opening a session fails. Through the SDK every call goes to the stpl:u service. Wonder's Switch 2 Edition sets it up
// while starting its systems (SDK 21).
#include <memory>

#include <nx/runtime.h>

#include "kernel.h"

namespace nx {

namespace {

constexpr uint32_t kNotAvailable = (1u << 9) | 0x1FFu;  // a failure Result for "no streaming session"

void ok(CPU& c) { c.x[0] = 0; }
void zero(CPU& c) { c.x[0] = 0; }
void fail(CPU& c) { c.x[0] = kNotAvailable; }

void GetRequiredMemorySize(CPU& c) { c.x[0] = 0x1000; }  // (type, mode, int) -> size_t

void out_u32_zero(CPU& c) {  // (T* out, ...): writes a zero word
    if (c.x[0]) st<uint32_t>(c.x[0], 0);
    c.x[0] = 0;
}

void count_out_zero(CPU& c) {  // (int* count, State*, int, handle): no states
    if (c.x[0]) st<int32_t>(c.x[0], 0);
    c.x[0] = 0;
}

// os::SystemEventType* GetConnectionStateChangedEvent(): one event that never fires, made once.
void GetConnectionStateChangedEvent(CPU& c) {
    static uint64_t event = 0;
    if (!event) {
        event = kernel_alloc(0x100);
        auto w = std::make_shared<KEvent>(), r = std::make_shared<KEvent>();
        r->state = w->state = std::make_shared<EventState>();
        uint32_t hr = handle_add(r), hw = handle_add(w);
        static uint64_t attach = find_symbol("_ZN2nn2os17AttachSystemEventEPNS0_15SystemEventTypeEjbjbNS0_14EventClearModeE");
        uint64_t saved[8];
        for (int k = 0; k < 8; k++) saved[k] = c.x[k];
        c.x[0] = event, c.x[1] = hr, c.x[2] = 1, c.x[3] = hw, c.x[4] = 1, c.x[5] = 1;
        call(c, attach);
        for (int k = 0; k < 8; k++) c.x[k] = saved[k];
    }
    c.x[0] = event;
}

}  // namespace

NX_HLE("_ZN2nn10streamplay21GetRequiredMemorySizeENS0_14StreamPlayTypeENS0_13StreamingModeEi", GetRequiredMemorySize);
NX_HLE("_ZN2nn10streamplay10InitializeENS0_14StreamPlayTypeENS0_13StreamingModeEPvm", ok);
NX_HLE("_ZN2nn10streamplay8FinalizeEv", ok);
NX_HLE("_ZN2nn10streamplay31OpenSessionWithMatchmakeSupportEPNS0_11SessionInfoERKNS0_15MatchmakeConfigE", fail);
NX_HLE("_ZN2nn10streamplay12CloseSessionEv", ok);
NX_HLE("_ZN2nn10streamplay14StartStreamingEv", fail);
NX_HLE("_ZN2nn10streamplay18GetConnectionStateEPNS0_15ConnectionStateE", out_u32_zero);
NX_HLE("_ZN2nn10streamplay30GetConnectionStateChangedEventEv", GetConnectionStateChangedEvent);
NX_HLE("_ZN2nn10streamplay13GetGuestNsaIdEPNS_7account23NetworkServiceAccountIdENS0_7GuestIdE", fail);
NX_HLE("_ZN2nn10streamplay16GetGuestNicknameEPNS_7account8NicknameENS0_7GuestIdE", fail);
NX_HLE("_ZN2nn10streamplay17GetGuestPadStatesEPNS0_13GuestPadStateINS_3hid16NpadFullKeyStateEEEiRKNS0_10GuestPadIdE", zero);
NX_HLE("_ZN2nn10streamplay17GetGuestPadStatesEPNS0_13GuestPadStateINS_3hid16NpadJoyDualStateEEEiRKNS0_10GuestPadIdE", zero);
NX_HLE("_ZN2nn10streamplay17GetGuestPadStatesEPNS0_13GuestPadStateINS_3hid16NpadJoyLeftStateEEEiRKNS0_10GuestPadIdE", zero);
NX_HLE("_ZN2nn10streamplay17GetGuestPadStatesEPNS0_13GuestPadStateINS_3hid17NpadHandheldStateEEEiRKNS0_10GuestPadIdE", zero);
NX_HLE("_ZN2nn10streamplay17GetGuestPadStatesEPNS0_13GuestPadStateINS_3hid17NpadJoyRightStateEEEiRKNS0_10GuestPadIdE", zero);
NX_HLE("_ZN2nn10streamplay19GetGuestPadStyleSetERKNS0_10GuestPadIdE", zero);
NX_HLE("_ZN2nn10streamplay18GetGuestJcmsHandleERKNS0_10GuestPadIdERKNS_3hid8JcmsTypeE", zero);
NX_HLE("_ZN2nn10streamplay18GetGuestJcmsStatesEPNS0_14GuestJcmsStateEiRKNS0_15GuestJcmsHandleE", zero);
NX_HLE("_ZN2nn10streamplay20GetGuestNmouseStatesEPiPNS0_16GuestNmouseStateEiRKNS0_17GuestNmouseHandleE", count_out_zero);
NX_HLE("_ZN2nn10streamplay21GetGuestNmouseHandlesEPNS0_17GuestNmouseHandleEiRKNS0_10GuestPadIdE", zero);
NX_HLE("_ZN2nn10streamplay21GetGuestNmouseTypeSetEPNS_4util10BitFlagSetILi32ENS0_15GuestNmouseTypeEEERKNS0_17GuestNmouseHandleE", out_u32_zero);
NX_HLE("_ZN2nn10streamplay23GetGuestNpadJoyHoldTypeEv", zero);
NX_HLE("_ZN2nn10streamplay23SetGuestNpadJoyHoldTypeENS_3hid15NpadJoyHoldTypeE", ok);
NX_HLE("_ZN2nn10streamplay25SetSupportedHidFeatureSetENS_4util10BitFlagSetILi32ENS0_13HidFeatureTagEEE", ok);
NX_HLE("_ZN2nn10streamplay29SetSupportedGuestNpadStyleSetENS_4util10BitFlagSetILi32ENS_3hid12NpadStyleTagEEE", ok);
NX_HLE("_ZN2nn10streamplay27SetGuestNmousePointerRangesEii", ok);
NX_HLE("_ZN2nn10streamplay31SetGuestMergedNmouseSourceTypesENS_4util10BitFlagSetILi32ENS0_15GuestNmouseTypeEEE", ok);
NX_HLE("_ZN2nn10streamplay37SetGuestNmouseButtonConfigJoyLeftJcmsERKNS_3hid29NmouseButtonConfigJoyLeftJcmsE", ok);
NX_HLE("_ZN2nn10streamplay38SetGuestNmouseButtonConfigJoyRightJcmsERKNS_3hid30NmouseButtonConfigJoyRightJcmsE", ok);
NX_HLE("_ZN2nn10streamplay27GetGuestSixAxisSensorStatesEPNS0_23GuestSixAxisSensorStateEiRKNS0_24GuestSixAxisSensorHandleE", zero);
NX_HLE("_ZN2nn10streamplay28GetGuestSixAxisSensorHandlesEPNS0_24GuestSixAxisSensorHandleEiRKNS0_10GuestPadIdENS_4util10BitFlagSetILi32ENS_3hid12NpadStyleTagEEE", zero);
NX_HLE("_ZN2nn10streamplay30GetGuestVibrationDeviceHandlesEPNS0_26GuestVibrationDeviceHandleEiRKNS0_10GuestPadIdENS_4util10BitFlagSetILi32ENS_3hid12NpadStyleTagEEE", zero);
NX_HLE("_ZN2nn10streamplay23SendGuestVibrationValueERKNS0_26GuestVibrationDeviceHandleERKNS_3hid14VibrationValueE", ok);
NX_HLE("_ZN2nn10streamplay20GuestVibrationTarget29SetGuestVibrationDeviceHandleERKNS0_26GuestVibrationDeviceHandleE", ok);
NX_HLE("_ZN2nn10streamplay20GuestVibrationTarget31UnsetGuestVibrationDeviceHandleEv", ok);

}  // namespace nx
