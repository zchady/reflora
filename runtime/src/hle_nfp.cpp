// nn::nfp (amiibo) at the API level: the PC has no NFC reader. Initialize succeeds, the device list is empty, the
// availability event never fires and every per-device call reports that the device is not there. Wonder's Switch 2
// Edition (SDK 21) initializes nfp while starting its systems; through the SDK it would need the nfp:user service.
#include <memory>

#include <nx/runtime.h>

#include "kernel.h"

namespace nx {

namespace {

constexpr uint32_t kNfcDeviceNotFound = (64u << 9) | 115u;  // nfp module 115, "NFC device not found"

void ok(CPU& c) { c.x[0] = 0; }
void not_found(CPU& c) { c.x[0] = kNfcDeviceNotFound; }

void ListDevices(CPU& c) {  // (DeviceHandle* out, int* count, int max) -> Result
    if (c.x[1]) st<int32_t>(c.x[1], 0);
    c.x[0] = 0;
}

void GetDeviceState(CPU& c) { c.x[0] = 0; }  // DeviceState_Init

void AttachAvailabilityChangeEvent(CPU& c) {  // (os::SystemEventType*): an event that never fires
    auto w = std::make_shared<KEvent>(), r = std::make_shared<KEvent>();
    r->state = w->state = std::make_shared<EventState>();
    uint32_t hr = handle_add(r), hw = handle_add(w);
    static uint64_t attach = find_symbol("_ZN2nn2os17AttachSystemEventEPNS0_15SystemEventTypeEjbjbNS0_14EventClearModeE");
    uint64_t saved[8];
    for (int k = 0; k < 8; k++) saved[k] = c.x[k];
    c.x[1] = hr, c.x[2] = 1, c.x[3] = hw, c.x[4] = 1, c.x[5] = 1;
    call(c, attach);
    for (int k = 0; k < 8; k++) c.x[k] = saved[k];
}

}  // namespace

NX_HLE("_ZN2nn3nfp10InitializeEv", ok);
NX_HLE("_ZN2nn3nfp8FinalizeEv", ok);
NX_HLE("_ZN2nn3nfp11ListDevicesEPNS0_12DeviceHandleEPii", ListDevices);
NX_HLE("_ZN2nn3nfp14GetDeviceStateERKNS0_12DeviceHandleE", GetDeviceState);
NX_HLE("_ZN2nn3nfp29AttachAvailabilityChangeEventEPNS_2os15SystemEventTypeE", AttachAvailabilityChangeEvent);
NX_HLE("_ZN2nn3nfp14StartDetectionERKNS0_12DeviceHandleE", not_found);
NX_HLE("_ZN2nn3nfp13StopDetectionERKNS0_12DeviceHandleE", not_found);
NX_HLE("_ZN2nn3nfp10GetTagInfoEPNS0_7TagInfoERKNS0_12DeviceHandleE", not_found);
NX_HLE("_ZN2nn3nfp12GetModelInfoEPNS0_9ModelInfoERKNS0_12DeviceHandleE", not_found);
NX_HLE("_ZN2nn3nfp5MountERKNS0_12DeviceHandleENS0_9ModelTypeENS0_11MountTargetE", not_found);
NX_HLE("_ZN2nn3nfp5FlushERKNS0_12DeviceHandleE", not_found);
NX_HLE("_ZN2nn3nfp9GetNpadIdEPjRKNS0_12DeviceHandleE", not_found);
NX_HLE("_ZN2nn3nfp19OpenApplicationAreaERKNS0_12DeviceHandleEj", not_found);
NX_HLE("_ZN2nn3nfp18GetApplicationAreaEPvPmRKNS0_12DeviceHandleEm", not_found);
NX_HLE("_ZN2nn3nfp18SetApplicationAreaERKNS0_12DeviceHandleEPKvm", not_found);
NX_HLE("_ZN2nn3nfp21CreateApplicationAreaERKNS0_12DeviceHandleERKNS0_25ApplicationAreaCreateInfoE", not_found);
NX_HLE("_ZN2nn3nfp23RecreateApplicationAreaERKNS0_12DeviceHandleERKNS0_25ApplicationAreaCreateInfoE", not_found);
NX_HLE("_ZN2nn3nfp13StartRestorerEPNS0_12DeviceHandleERKNS0_24AmiiboSettingsStartParamERKNS0_7TagInfoE", not_found);
NX_HLE("_ZN2nn3nfp19StartGameDataEraserEPNS0_12DeviceHandleERKNS0_24AmiiboSettingsStartParamERKNS0_7TagInfoE", not_found);
NX_HLE("_ZN2nn3nfp29StartNicknameAndOwnerSettingsEPNS0_12DeviceHandleEPbPNS0_12RegisterInfoERKNS0_24AmiiboSettingsStartParamERKNS0_7TagInfoE", not_found);
NX_HLE("_ZN2nn3nfp29StartNicknameAndOwnerSettingsEPNS0_12DeviceHandleEPbPNS0_12RegisterInfoERKNS0_24AmiiboSettingsStartParamERKNS0_7TagInfoERKS4_", not_found);

}  // namespace nx
