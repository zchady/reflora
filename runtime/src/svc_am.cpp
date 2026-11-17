// am services (appletOE): application proxy, focus/operation-mode state, launch parameters.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

#include "content.h"
#include "ipc.h"
#include "nup_archive.h"
#include "platform.h"

namespace nx {

namespace {

constexpr uint32_t kAmNoMessage = 0x680;  // am error 3
constexpr uint32_t kMsgFocusStateChanged = 15;
constexpr uint32_t kMsgOperationModeChanged = 30;
constexpr uint32_t kMsgPerformanceModeChanged = 31;

struct AppletState {
    std::deque<uint32_t> messages{kMsgFocusStateChanged, kMsgOperationModeChanged, kMsgPerformanceModeChanged};
    std::shared_ptr<EventState> message_event = std::make_shared<EventState>();
};

AppletState& applet() {
    static AppletState s;
    return s;
}

// The running title's control.nacp: the NUP program package's (metadata TLV or file), else <data root>/extract/
// control.nacp (an extracted game). Read once; empty (logged) when there is none.
const std::vector<uint8_t>& control_nacp() {
    static std::once_flag once;
    static std::vector<uint8_t> nacp;
    std::call_once(once, [] {
        std::string from;
        if (auto* a = content::archive())
            if (const nup::Package* p = content::program()) nacp = a->nacp(*p), from = "the NUP program package";
        if (nacp.size() != 0x4000) {
            nacp.clear();
            const std::string path = plat::data_root() + "/extract/control.nacp";
            if (FILE* f = plat::fopen_utf8(path, "rb")) {
                nacp.resize(0x4000);
                if (std::fread(nacp.data(), 1, nacp.size(), f) != nacp.size()) nacp.clear();
                std::fclose(f);
                from = path;
            }
        }
        if (nacp.empty())
            log("am: no control.nacp (NUP package or %s/extract/control.nacp): no display version",
                plat::data_root().c_str());
        else
            log("am: control.nacp from %s, display version '%.16s'", from.c_str(),
                reinterpret_cast<const char*>(nacp.data() + 0x3060));
    });
    return nacp;
}

uint32_t event_handle(std::shared_ptr<EventState> st) {
    auto e = std::make_shared<KEvent>();
    e->state = std::move(st);
    return handle_add(e);
}

std::shared_ptr<Service> svc(const char* name, std::function<void(Service&)> setup) {
    auto s = std::make_shared<Service>(name);
    setup(*s);
    return s;
}

std::shared_ptr<Service> common_state_getter() {
    return svc("ICommonStateGetter", [](Service& s) {
        s.cmds[0] = [](IpcCtx& c) {  // GetEventHandle
            applet().message_event->signaled = !applet().messages.empty();
            c.out_copy.push_back(event_handle(applet().message_event));
            return 0u;
        };
        s.cmds[1] = [](IpcCtx& c) {  // ReceiveMessage
            auto& a = applet();
            if (a.messages.empty()) {
                a.message_event->signaled = false;
                return kAmNoMessage;
            }
            c.push<uint32_t>(a.messages.front());
            a.messages.pop_front();
            if (a.messages.empty()) a.message_event->signaled = false;
            return 0u;
        };
        // NX_HANDHELD=1: handheld mode, so the game renders for 1280x720 (a lighter GPU load) instead of docked 1080p.
        static const bool handheld = std::getenv("NX_HANDHELD") != nullptr;
        s.cmds[5] = [](IpcCtx& c) { c.push<uint8_t>(handheld ? 0 : 1); return 0u; };   // GetOperationMode
        s.cmds[6] = [](IpcCtx& c) { c.push<uint32_t>(handheld ? 0 : 1); return 0u; };  // GetPerformanceMode
        s.cmds[9] = [](IpcCtx& c) { c.push<uint8_t>(1); return 0u; };   // GetCurrentFocusState: in focus
        s.cmds[60] = [](IpcCtx& c) {  // GetDefaultDisplayResolution
            c.push<uint32_t>(handheld ? 1280 : 1920);
            c.push<uint32_t>(handheld ? 720 : 1080);
            return 0u;
        };
        s.cmds[61] = [](IpcCtx& c) {  // GetDefaultDisplayResolutionChangeEvent
            c.out_copy.push_back(event_handle(std::make_shared<EventState>()));
            return 0u;
        };
        s.cmds[66] = [](IpcCtx&) { return 0u; };  // SetCpuBoostMode (nninitStartup)
    });
}

std::shared_ptr<Service> self_controller() {
    return svc("ISelfController", [](Service& s) {
        s.cmds[40] = [](IpcCtx& c) { c.push<uint64_t>(1); return 0u; };  // CreateManagedDisplayLayer
        s.cmds[0] = [](IpcCtx&) { return 0u; };                             // Exit
        s.cmds[62] = [](IpcCtx&) { return 0u; };                            // SetIdleTimeDetectionExtension
        for (uint32_t cmd : {19u, 50u, 71u, 110u})                           // AccumulatedSuspendedTickChangedEvent etc.
            s.cmds[cmd] = [](IpcCtx& c) {
                c.out_copy.push_back(event_handle(std::make_shared<EventState>()));
                return 0u;
            };
    });
}

std::shared_ptr<Service> window_controller() {
    return svc("IWindowController", [](Service& s) {
        s.cmds[1] = [](IpcCtx& c) { c.push<uint64_t>(0x51); return 0u; };  // GetAppletResourceUserId
    });
}

std::shared_ptr<Service> application_functions() {
    return svc("IApplicationFunctions", [](Service& s) {
        s.cmds[1] = [](IpcCtx& c) {  // PopLaunchParameter(kind): the preselected user, once
            static bool popped = false;
            if (c.arg<uint32_t>(0) != 2 || popped) return 0x2A80u;
            popped = true;
            c.push_object(preselected_user_storage());
            return 0u;
        };
        s.cmds[20] = [](IpcCtx& c) { c.push<uint64_t>(0); return 0u; };  // EnsureSaveData
        // GetDesiredLanguage: NX_LANGUAGE (the settings overlay's language, settings.cpp), one of the codes the game
        // knows (its table: ja, en-US, fr, de, it, es, zh-CN, ko, nl, pt, ru, zh-TW, en-GB, fr-CA, es-419, zh-Hans,
        // zh-Hant, pt-BR, pl, th), else en-US as before.
        s.cmds[21] = [](IpcCtx& c) {
            static const char* const kCodes[] = {"ja", "en-US", "fr", "de", "it", "es", "zh-CN", "ko", "nl", "pt",
                                                 "ru", "zh-TW", "en-GB", "fr-CA", "es-419", "zh-Hans", "zh-Hant",
                                                 "pt-BR", "pl", "th"};
            const char* want = std::getenv("NX_LANGUAGE");
            const char* lang = "en-US";
            for (const char* k : kCodes)
                if (want && std::strcmp(want, k) == 0) lang = k;
            static bool logged = (log("am: desired language %s", lang), true);
            (void)logged;
            uint64_t code = 0;
            std::memcpy(&code, lang, std::strlen(lang));
            c.push<uint64_t>(code);
            return 0u;
        };
        // GetDisplayVersion: the 16-byte DisplayVersion of the title's control.nacp (offset 0x3060), what
        // nn::oe::GetDisplayVersion returns and the title screen prints after "Ver." (Wonder: "1.2.1").
        s.cmds[23] = [](IpcCtx& c) {
            char v[16] = {};
            const auto& nacp = control_nacp();
            if (nacp.size() == 0x4000) std::memcpy(v, nacp.data() + 0x3060, 15);  // NUL-terminated within 16
            uint64_t w[2];
            std::memcpy(w, v, 16);
            c.push<uint64_t>(w[0]);
            c.push<uint64_t>(w[1]);
            return 0u;
        };
        s.cmds[40] = [](IpcCtx& c) { c.push<uint8_t>(0); return 0u; };  // NotifyRunning
        s.cmds[50] = [](IpcCtx& c) {                                     // GetPseudoDeviceId
            c.push<uint64_t>(0x1234);
            c.push<uint64_t>(0x5678);
            return 0u;
        };
        s.cmds[130] = [](IpcCtx& c) {  // GetGpuErrorDetectedSystemEvent
            c.out_copy.push_back(event_handle(std::make_shared<EventState>()));
            return 0u;
        };
        // Switch 2 SDK (21.4.1): nn::oe::GetOperationMode attaches this command's handle to an event and re-reads the
        // mode (ICommonStateGetter 5) when it fires (sdk+0x58e870, measured with NX_TRACE_IPC). Without a handle its
        // os::TryWait retries svcWaitSynchronization forever. Never signaled: the mode does not change on the PC.
        s.cmds[210] = [](IpcCtx& c) {
            c.out_copy.push_back(event_handle(std::make_shared<EventState>()));
            return 0u;
        };
    });
}

std::shared_ptr<Service> application_proxy() {
    return svc("IApplicationProxy", [](Service& s) {
        s.cmds[0] = [](IpcCtx& c) { c.push_object(common_state_getter()); return 0u; };
        s.cmds[1] = [](IpcCtx& c) { c.push_object(self_controller()); return 0u; };
        s.cmds[2] = [](IpcCtx& c) { c.push_object(window_controller()); return 0u; };
        s.cmds[3] = [](IpcCtx& c) { c.push_object(std::make_shared<Service>("IAudioController")); return 0u; };
        s.cmds[4] = [](IpcCtx& c) { c.push_object(std::make_shared<Service>("IDisplayController")); return 0u; };
        s.cmds[10] = [](IpcCtx& c) { c.push_object(std::make_shared<Service>("IProcessWindingController")); return 0u; };
        s.cmds[11] = [](IpcCtx& c) { c.push_object(std::make_shared<Service>("ILibraryAppletCreator")); return 0u; };
        s.cmds[20] = [](IpcCtx& c) { c.push_object(application_functions()); return 0u; };
        s.cmds[1000] = [](IpcCtx& c) { c.push_object(std::make_shared<Service>("IDebugFunctions")); return 0u; };
        // Switch 2 (SDK 20+): cmd 60 opens the graphics-scale getter behind nn::oe::GetCurrentGraphicsScale; its cmd 10
        // answers 10, 20 or 30 (public notes on Switch 2 services). 10 until a run shows what Wonder does with it.
        s.cmds[60] = [](IpcCtx& c) {
            c.push_object(svc("IGraphicsScaleGetter", [](Service& g) {
                g.cmds[10] = [](IpcCtx& c2) { c2.push<uint32_t>(10); return 0u; };
            }));
            return 0u;
        };
    });
}

ServiceRegistrar reg_applet_oe("appletOE", [] {
    return svc("appletOE", [](Service& s) {
        s.cmds[0] = [](IpcCtx& c) { c.push_object(application_proxy()); return 0u; };  // OpenApplicationProxy
        s.cmds[10] = [](IpcCtx& c) { c.push_object(application_proxy()); return 0u; };  // the Switch 2 SDK's
    });
});

}  // namespace

}  // namespace nx
