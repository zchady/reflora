// acc:u0 (accounts) and the storage objects am hands out (launch parameters).
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include <nx/runtime.h>

#include "ipc.h"
#include "platform.h"

namespace nx {

// One local user.
constexpr uint64_t kUserLo = 0x0000000000000001ull, kUserHi = 0x4E58485553455231ull;
const char kDefaultNickname[] = "Zagreus";

// The user's name as the game receives it (ProfileBase.nickname, 0x20 bytes): NX_NICKNAME (the REFLORA launcher sets
// it), else "nickname=" in <data root>/user/profile.ini (the launcher writes it too, for starts outside it), else the
// default. UTF-8, cut at a character boundary to 31 bytes so the field keeps its terminating zero.
const std::string& nickname() {
    static const std::string name = [] {
        std::string v, from = "default";
        if (const char* e = std::getenv("NX_NICKNAME"); e && *e) {
            v = e, from = "NX_NICKNAME";
        } else if (FILE* f = plat::fopen_utf8(plat::data_root() + "/user/profile.ini", "rb")) {
            char line[256];
            while (std::fgets(line, sizeof line, f)) {
                std::string l(line);
                if (l.compare(0, 3, "\xEF\xBB\xBF") == 0) l.erase(0, 3);
                while (!l.empty() && (l.back() == '\n' || l.back() == '\r')) l.pop_back();
                if (l.compare(0, 9, "nickname=") == 0) v = l.substr(9), from = "user/profile.ini";
            }
            std::fclose(f);
        }
        if (v.size() > 31) {
            size_t n = 31;
            while (n > 0 && (static_cast<unsigned char>(v[n]) & 0xC0) == 0x80) n--;
            v.resize(n);
        }
        for (char& c : v)
            if (static_cast<unsigned char>(c) < 0x20) c = ' ';
        if (v.empty()) v = kDefaultNickname, from = "default";
        log("account: nickname '%s' (%s)", v.c_str(), from.c_str());
        return v;
    }();
    return name;
}

// The user's profile picture: <data root>/user/profile.jpg (the REFLORA launcher writes it: a 256x256 JPEG, the
// console's format), at most 0x20000 bytes (nn::account::ProfileImageBytesMax). None: no picture, as before.
const std::vector<uint8_t>& profile_image() {
    static const std::vector<uint8_t> image = [] {
        std::vector<uint8_t> v;
        const std::string path = plat::data_root() + "/user/profile.jpg";
        if (FILE* f = plat::fopen_utf8(path, "rb")) {
            v.resize(0x20001);
            v.resize(std::fread(v.data(), 1, v.size(), f));
            std::fclose(f);
            if (v.size() > 0x20000 || v.size() < 4 || v[0] != 0xFF || v[1] != 0xD8) {
                log("account: %s is not a JPEG of at most 128 KiB: no profile image", path.c_str());
                v.clear();
            } else {
                log("account: profile image %s (%zu bytes)", path.c_str(), v.size());
            }
        }
        return v;
    }();
    return image;
}

// IStorage / IStorageAccessor over a byte blob (am's storage objects).
std::shared_ptr<Service> make_storage(std::vector<uint8_t> data) {
    auto blob = std::make_shared<std::vector<uint8_t>>(std::move(data));
    auto s = std::make_shared<Service>("IStorage");
    s->cmds[0] = [blob](IpcCtx& c) {  // Open -> IStorageAccessor
        auto a = std::make_shared<Service>("IStorageAccessor");
        a->cmds[0] = [blob](IpcCtx& c) { c.push<int64_t>(int64_t(blob->size())); return 0u; };  // GetSize
        a->cmds[11] = [blob](IpcCtx& c) {                                                       // Read(offset)
            int64_t off = c.arg<int64_t>(0);
            Buffer out = c.out_buf(0);
            if (off < 0 || uint64_t(off) > blob->size()) return 0x3E880u;  // out of bounds
            size_t n = std::min<size_t>(out.size, blob->size() - size_t(off));
            std::memcpy(reinterpret_cast<void*>(out.addr), blob->data() + off, n);
            return 0u;
        };
        c.push_object(a);
        return 0u;
    };
    return s;
}

std::shared_ptr<Service> preselected_user_storage() {
    std::vector<uint8_t> d(0x88, 0);
    uint32_t magic = 0xC79497CA, selected = 1;
    std::memcpy(d.data(), &magic, 4);
    std::memcpy(d.data() + 4, &selected, 4);
    std::memcpy(d.data() + 8, &kUserLo, 8);
    std::memcpy(d.data() + 16, &kUserHi, 8);
    return make_storage(std::move(d));
}

namespace {

void push_uid(IpcCtx& c) {
    c.push<uint64_t>(kUserLo);
    c.push<uint64_t>(kUserHi);
}

void write_uid_list(IpcCtx& c) {
    Buffer out = c.out_buf(0);
    if (out.addr && out.size >= 16) {
        std::memset(reinterpret_cast<void*>(out.addr), 0, out.size);
        st<uint64_t>(out.addr, kUserLo);
        st<uint64_t>(out.addr + 8, kUserHi);
    }
}

std::shared_ptr<Service> profile() {
    auto s = std::make_shared<Service>("IProfile");
    auto base = [](IpcCtx& c) {  // ProfileBase {Uid; u64 last_edit; char nickname[0x20]}
        push_uid(c);
        c.push<uint64_t>(0);
        char name[0x20] = {};
        std::memcpy(name, nickname().data(), nickname().size());
        for (char ch : name) c.push<char>(ch);
    };
    s->cmds[0] = [base](IpcCtx& c) {  // Get(out UserData buffer) -> ProfileBase
        Buffer out = c.out_buf(0);
        if (out.addr) std::memset(reinterpret_cast<void*>(out.addr), 0, out.size);
        base(c);
        return 0u;
    };
    s->cmds[1] = [base](IpcCtx& c) { base(c); return 0u; };            // GetBase
    s->cmds[10] = [](IpcCtx& c) {  // GetImageSize (0: no picture)
        c.push<uint32_t>(uint32_t(profile_image().size()));
        return 0u;
    };
    s->cmds[11] = [](IpcCtx& c) {  // LoadImage(out buffer) -> bytes written
        const auto& img = profile_image();
        Buffer out = c.out_buf(0);
        const size_t n = out.addr ? std::min<size_t>(img.size(), out.size) : 0;
        if (n) std::memcpy(reinterpret_cast<void*>(out.addr), img.data(), n);
        static bool logged = false;
        if (!logged) log("account: the game loaded the profile image (%zu bytes)", n), logged = true;
        c.push<uint32_t>(uint32_t(n));
        return 0u;
    };
    return s;
}

std::shared_ptr<Service> manager_for_application() {
    auto s = std::make_shared<Service>("IManagerForApplication");
    s->cmds[0] = [](IpcCtx& c) { c.push<uint8_t>(0); return 0u; };  // CheckAvailability
    s->cmds[1] = [](IpcCtx& c) { c.push<uint64_t>(0x1234ABCD5678EF90ull); return 0u; };  // GetAccountId
    return s;
}

void account(Service& s) {
    s.cmds[0] = [](IpcCtx& c) { c.push<int32_t>(1); return 0u; };  // GetUserCount
    s.cmds[1] = [](IpcCtx& c) {                                     // GetUserExistence(uid)
        c.push<uint8_t>(c.arg<uint64_t>(0) == kUserLo && c.arg<uint64_t>(8) == kUserHi);
        return 0u;
    };
    s.cmds[2] = [](IpcCtx& c) { write_uid_list(c); return 0u; };  // ListAllUsers
    s.cmds[3] = [](IpcCtx& c) { write_uid_list(c); return 0u; };  // ListOpenUsers
    s.cmds[4] = [](IpcCtx& c) { push_uid(c); return 0u; };        // GetLastOpenedUser
    s.cmds[5] = [](IpcCtx& c) { c.push_object(profile()); return 0u; };  // GetProfile
    s.cmds[50] = [](IpcCtx& c) { c.push<uint8_t>(1); return 0u; };  // IsUserRegistrationRequestPermitted
    s.cmds[51] = [](IpcCtx& c) { push_uid(c); return 0u; };         // TrySelectUserWithoutInteraction
    s.cmds[100] = [](IpcCtx&) { return 0u; };                       // InitializeApplicationInfo (old)
    s.cmds[101] = [](IpcCtx& c) { c.push_object(manager_for_application()); return 0u; };
    s.cmds[140] = [](IpcCtx&) { return 0u; };  // InitializeApplicationInfo
    s.cmds[141] = [](IpcCtx& c) { write_uid_list(c); return 0u; };  // ListQualifiedUsers (SDK 16)
    s.cmds[160] = [](IpcCtx&) { return 0u; };                       // InitializeApplicationInfoV2 (SDK 16)
    s.cmds[150] = [](IpcCtx& c) { c.push<uint8_t>(0); return 0u; };  // IsUserAccountSwitchLocked
}

ServiceRegistrar r_acc_u0("acc:u0", [] { auto s = std::make_shared<Service>("acc:u0"); account(*s); return s; });
ServiceRegistrar r_acc_u1("acc:u1", [] { auto s = std::make_shared<Service>("acc:u1"); account(*s); return s; });

}  // namespace

}  // namespace nx
