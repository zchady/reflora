// Content selection from a NUP package: see content.h.
#include "content.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>

#include <nx/game.h>
#include <nx/runtime.h>

#include "platform.h"

namespace nx::content {

bool same_family(uint64_t a, uint64_t b) { return (a & kFamilyMask) == (b & kFamilyMask); }

int addon_index(uint64_t title_id) {
    uint64_t rel = title_id & 0x1FFF;
    return rel > 0x1000 ? int(rel - 0x1000) : 0;
}

namespace {

std::once_flag g_once;
std::unique_ptr<nup::Archive> g_archive;

std::string configured_path() {
    if (std::string e; plat::env_utf8("NX_NUP", e) && !e.empty()) return e;
    // nup.txt holds the package's path in UTF-8, and lives in the installation (a folder that may have accents)
    std::string line;
    if (FILE* f = plat::fopen_utf8(plat::data_root() + "/nup.txt", "rb")) {
        char buf[4096];
        const size_t n = std::fread(buf, 1, sizeof buf - 1, f);
        std::fclose(f);
        line.assign(buf, n);
        if (const size_t eol = line.find('\n'); eol != std::string::npos) line.resize(eol);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (line.size() >= 3 && (unsigned char)line[0] == 0xEF && (unsigned char)line[1] == 0xBB &&
            (unsigned char)line[2] == 0xBF)
            line.erase(0, 3);  // a BOM, if an editor added one
    }
    return line;
}

void open_once() {
    std::string path = configured_path();
    if (path.empty()) return;
    std::string err;
    g_archive = nup::Archive::open(path, &err);
    if (!g_archive) {
        log("content: cannot use %s: %s", path.c_str(), err.c_str());
        return;
    }
    log("content: %s, %zu packages", path.c_str(), g_archive->packages().size());
    for (auto& p : g_archive->packages())
        log("content:   %016llx kind %u version %u%s%s", (unsigned long long)p.title_id, p.kind, p.version,
            p.exefs_dir != nup::kNone ? " exefs" : "", p.romfs_dir != nup::kNone ? " romfs" : "");
}

}  // namespace

nup::Archive* archive() {
    std::call_once(g_once, open_once);
    return g_archive.get();
}

const nup::Package* program() {
    nup::Archive* a = archive();
    if (!a) return nullptr;
    const nup::Package* first_base = nullptr;
    for (auto& p : a->packages()) {
        if (p.title_id == game::kInfo.program_id) return &p;
        if (p.kind == nup::kBase && !first_base) first_base = &p;
    }
    return first_base;
}

std::vector<Addon> addons() {
    std::vector<Addon> out;
    const nup::Package* prog = program();
    if (!prog) return out;
    for (auto& p : archive()->packages())
        if (p.kind == nup::kAddon && p.romfs_dir != nup::kNone && addon_index(p.title_id) &&
            same_family(p.title_id, prog->title_id))
            out.push_back({addon_index(p.title_id), &p});
    std::sort(out.begin(), out.end(), [](const Addon& a, const Addon& b) { return a.index < b.index; });
    return out;
}

const nup::Package* addon(int index) {
    for (auto& a : addons())
        if (a.index == index) return a.package;
    return nullptr;
}

}  // namespace nx::content
