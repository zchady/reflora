// Small services that only need to answer "yes, fine": parental controls, add-on content, logging, power.
#include <cstdlib>
#include "content.h"
#include "ipc.h"

namespace nx {

namespace {

std::shared_ptr<Service> make(const char* name, std::function<void(Service&)> setup = {}) {
    auto s = std::make_shared<Service>(name);
    if (setup) setup(*s);
    return s;
}

std::shared_ptr<Service> parental_control() {
    return make("IParentalControlService", [](Service& s) {
        s.cmds[1403] = [](IpcCtx& c) { c.push<uint8_t>(0); return 0u; };  // IsRestrictionEnabled
        s.cmds[1001] = [](IpcCtx&) { return 0u; };                          // CheckFreeCommunicationPermission
    });
}

void pctl(Service& s) {
    s.cmds[0] = [](IpcCtx& c) { c.push_object(parental_control()); return 0u; };  // CreateService
    s.cmds[1] = [](IpcCtx& c) { c.push_object(parental_control()); return 0u; };  // CreateServiceWithoutInitialize
}

ServiceRegistrar r_pctl("pctl", [] { return make("pctl", pctl); });
ServiceRegistrar r_pctl_s("pctl:s", [] { return make("pctl:s", pctl); });
ServiceRegistrar r_pctl_a("pctl:a", [] { return make("pctl:a", pctl); });
ServiceRegistrar r_pctl_r("pctl:r", [] { return make("pctl:r", pctl); });

ServiceRegistrar r_lm("lm", [] {
    return make("lm", [](Service& s) {
        s.cmds[0] = [](IpcCtx& c) { c.push_object(make("ILogger")); return 0u; };  // OpenLogger
    });
});

ServiceRegistrar r_apm("apm", [] {
    return make("apm", [](Service& s) {
        s.cmds[0] = [](IpcCtx& c) { c.push_object(make("ISession")); return 0u; };  // OpenSession
        s.cmds[1] = [](IpcCtx& c) {  // GetPerformanceMode: boost (docked), normal with NX_HANDHELD (svc_am.cpp)
            c.push<uint32_t>(std::getenv("NX_HANDHELD") ? 0 : 1);
            return 0u;
        };
    });
});

// auddmg:u (Switch 2 SDK 21): the audio device manager behind nn::audio::devmgr. Its cmd 0 opens the session the
// devmgr functions talk to; without one, every devmgr call fails with 0xc8099 and OpenAudioRenderer aborts. The PC
// has one fixed output (hle_audio.cpp), so the session accepts every command and answers nothing.
ServiceRegistrar r_auddmg("auddmg:u", [] {
    return make("auddmg:u", [](Service& s) {
        s.cmds[0] = [](IpcCtx& c) { c.push_object(make("IAudioDeviceManagerSession")); return 0u; };
    });
});

ServiceRegistrar r_aoc("aoc:u", [] {
    return make("aoc:u", [](Service& s) {
        // Add-ons come from the NUP package (content.h): Wonder's Switch 2 Edition upgrade pack is add-on 1.
        s.cmds[2] = [](IpcCtx& c) {  // CountAddOnContent(pid) -> u32
            c.push<uint32_t>(uint32_t(content::addons().size()));
            return 0u;
        };
        s.cmds[3] = [](IpcCtx& c) {  // ListAddOnContent(u32 offset, u32 count, pid) -> u32 written, buffer of u32 indices
            uint32_t offset = c.arg<uint32_t>(0), count = c.arg<uint32_t>(4);
            Buffer out = c.out_buf(0);
            auto list = content::addons();
            uint32_t n = 0;
            for (size_t k = offset; k < list.size() && n < count && (n + 1) * 4 <= out.size; k++, n++)
                st<uint32_t>(out.addr + n * 4, uint32_t(list[k].index));
            c.push<uint32_t>(n);
            return 0u;
        };
        s.cmds[5] = [](IpcCtx& c) {  // GetAddOnContentBaseId(pid) -> u64: application ID + 0x1000 of the add-ons
            auto list = content::addons();
            c.push<uint64_t>(list.empty() ? 0 : (list[0].package->title_id & ~0x1FFFull) + 0x1000);
            return 0u;
        };
        s.cmds[7] = [](IpcCtx& c) { c.push<uint32_t>(0); return 0u; };  // PrepareAddOnContent
        s.cmds[50] = [](IpcCtx&) { return 0u; };                        // CheckAddOnContentMountStatus
        s.cmds[8] = [](IpcCtx& c) {                                       // GetAddOnContentListChangedEvent
            auto e = std::make_shared<KEvent>();
            e->state = std::make_shared<EventState>();
            c.out_copy.push_back(handle_add(e));
            return 0u;
        };
    });
});

}  // namespace

}  // namespace nx
