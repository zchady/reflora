// nn::fs at the API level, on host files. The game imports these by name (main), so no fsp-srv IPC is involved:
//   rom:   -> private/extract/romfs (the RomFS with the update applied), or the program's RomFS inside a NUP package
//             (content.h) when there is no extracted folder
//   add-on content (MountAddOnContent) -> the add-on's RomFS inside the NUP package
//   save:  -> private/user/save/<mount>
//   cache: -> private/user/cache<index>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>

#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

#include <nx/runtime.h>

#include "hitch.h"
#include "content.h"
#include "platform.h"

namespace fs = std::filesystem;

namespace nx {

namespace {

// NX_TRACE_FS=1: every file the game opens (and the ones it looks for and does not find).
bool trace_fs() {
    static const bool on = g_trace || std::getenv("NX_TRACE_FS") != nullptr;
    return on;
}

constexpr uint32_t kOk = 0;
constexpr uint32_t kPathNotFound = 0x202;       // fs 1
constexpr uint32_t kPathAlreadyExists = 0x402;  // fs 2
constexpr uint32_t kTargetNotFound = 0x2EE202;  // fs 6001-ish: generic "not mounted"

std::mutex g_fs;
std::map<std::string, fs::path> g_mounts;
// Mounts served from a NUP package (read only): the archive and the directory index of the mount's root.
struct NupMount {
    nup::Archive* ar;
    uint32_t dir;
};
std::map<std::string, NupMount> g_nup_mounts;
// Open files: shared so a read in progress survives a close from another thread. Reads run outside g_fs (which
// only guards the tables): one global lock made every streaming thread (Bink animations, FMOD banks) wait behind
// any big read, felt as hitches.
struct File {
    FILE* f;
    std::string path;
    nup::Archive* ar = nullptr;  // a file inside a NUP package (f is null)
    uint32_t nfile = 0;
    File(FILE* file, std::string p) : f(file), path(std::move(p)) {}
    File(nup::Archive* a, uint32_t n, std::string p) : f(nullptr), path(std::move(p)), ar(a), nfile(n) {}
    File(const File&) = delete;  // owns the FILE: one instance only
    File& operator=(const File&) = delete;
    ~File() {
        if (f) std::fclose(f);
    }
};
std::map<uint64_t, std::shared_ptr<File>> g_files;

// Files the runtime adds or replaces (mods): key = path under the mount point, '/' separated, lower case.
std::map<std::string, std::string> g_virtual;

std::string virtual_key(const fs::path& p) {
    auto g = p.lexically_normal().generic_u8string();
    std::string s(g.begin(), g.end());
    for (char& ch : s) ch = char(std::tolower(static_cast<unsigned char>(ch)));
    return s;
}

// The virtual file whose key the resolved path ends with, or nullptr.
const std::string* find_virtual(const fs::path& resolved) {
    if (g_virtual.empty() || resolved.empty()) return nullptr;
    std::string k = virtual_key(resolved);
    for (auto& [key, data] : g_virtual)
        if (k.size() >= key.size() + 1 && k.compare(k.size() - key.size(), key.size(), key) == 0 &&
            k[k.size() - key.size() - 1] == '/')
            return &data;
    return nullptr;
}
struct DirItem {
    std::string name;
    bool dir;
    uint64_t size;
};
struct Dir {
    std::vector<DirItem> entries;
    size_t pos = 0;
};
std::map<uint64_t, Dir> g_dirs;
uint64_t g_next = 1;

fs::path data_root() { return fs::u8path(plat::data_root()); }

std::string utf8(const fs::path& p) {
    auto s = p.u8string();
    return std::string(s.begin(), s.end());
}

std::string cstr(uint64_t p) { return p ? std::string(reinterpret_cast<const char*>(p)) : std::string(); }

// "mount:/a/b" -> host path; empty when the mount is unknown.
fs::path resolve(const std::string& guest) {
    auto colon = guest.find(':');
    if (colon == std::string::npos) return {};
    auto it = g_mounts.find(guest.substr(0, colon));
    if (it == g_mounts.end()) return {};
    std::string rest = guest.substr(colon + 1);
    while (!rest.empty() && (rest[0] == '/' || rest[0] == '\\')) rest.erase(0, 1);
    return it->second / fs::u8path(rest);
}

void mount(const std::string& name, const fs::path& p, bool create = true) {
    if (create) fs::create_directories(p);
    g_nup_mounts.erase(name);
    g_mounts[name] = p;
    log("fs: mounted %s: -> %s", name.c_str(), utf8(p).c_str());  // UTF-8, as the log file
}

bool is_nup_mount(const std::string& guest) {
    auto colon = guest.find(':');
    return colon != std::string::npos && g_nup_mounts.count(guest.substr(0, colon));
}

// "mount:/a/b" on a NUP mount: the archive and the node; false when the path does not exist there.
bool resolve_nup(const std::string& guest, nup::Archive** ar, bool* is_dir, uint32_t* index) {
    auto colon = guest.find(':');
    if (colon == std::string::npos) return false;
    auto it = g_nup_mounts.find(guest.substr(0, colon));
    if (it == g_nup_mounts.end()) return false;
    *ar = it->second.ar;
    return it->second.ar->lookup(it->second.dir, std::string_view(guest).substr(colon + 1), is_dir, index);
}

bool mount_nup(const std::string& name, const nup::Package* pkg, const char* what) {
    nup::Archive* ar = content::archive();
    if (!ar || !pkg || pkg->romfs_dir == nup::kNone) return false;
    g_mounts.erase(name);
    g_nup_mounts[name] = {ar, pkg->romfs_dir};
    log("fs: mounted %s: -> %s RomFS of %016llx in %s", name.c_str(), what, (unsigned long long)pkg->title_id,
        ar->path().c_str());
    return true;
}

// A folder with something in it (an extracted RomFS; an empty one is not).
bool has_files(const fs::path& dir) {
    std::error_code ec;
    return fs::is_directory(dir, ec) && fs::directory_iterator(dir, ec) != fs::directory_iterator();
}

// The program's RomFS: the extracted folder when it holds files, else the NUP package. The folder is never created
// here: a run without the NUP used to leave an empty extract/romfs behind, which then won over the NUP the next time
// (the Galaxy Tab S10+'s first run with the NUP read no game files and crashed, WONDER_SWITCH2_PROGRESS.md §5zd15).
uint32_t mount_rom(const std::string& name) {
    fs::path dir = data_root() / "extract" / "romfs";
    if (!has_files(dir) && mount_nup(name, content::program(), "program")) return kOk;
    if (!has_files(dir)) log("fs: %s: no NUP and no extracted RomFS in %s", name.c_str(), utf8(dir).c_str());
    mount(name, dir, false);
    return kOk;
}

// ---- mounts ---------------------------------------------------------------------------------------------------

void QueryMountRomCacheSize(CPU& c) {
    st<uint64_t>(c.x[0], 0x1000);
    c.x[0] = kOk;
}

void MountRom(CPU& c) {
    std::lock_guard lk(g_fs);
    c.x[0] = mount_rom(cstr(c.x[0]));
}

// SDK 21 (Wonder, Switch 2 Edition): MountRom(name, int program index, cache, size) mounts the RomFS of a program of
// the same application. Wonder asks for program 0 as "conorig" when content:/System/AddressTable is missing.
void MountRomOfProgram(CPU& c) {
    std::lock_guard lk(g_fs);
    std::string name = cstr(c.x[0]);
    int index = int(c.x[1]);
    const nup::Package* own = content::program();
    if (!own || int(own->title_id & 0xF) == index) {
        c.x[0] = mount_rom(name);
        return;
    }
    for (auto& p : content::archive()->packages())
        if (p.kind == nup::kBase && p.title_id == ((own->title_id & ~0xFull) | uint64_t(index)) &&
            mount_nup(name, &p, "program")) {
            c.x[0] = kOk;
            return;
        }
    log("fs: MountRom(%s, program %d): that program is not in the package", name.c_str(), index);
    c.x[0] = kTargetNotFound;
}

void QueryMountRomCacheSizeOfProgram(CPU& c) {
    st<uint64_t>(c.x[0], 0x1000);
    c.x[0] = kOk;
}

// MountAddOnContent(name, int add-on index, cache, size): the add-on's RomFS (Wonder: "aocup", index 1), from the
// package, or from <data root>/extract/aoc<index> when extracted.
void MountAddOnContent(CPU& c) {
    std::lock_guard lk(g_fs);
    std::string name = cstr(c.x[0]);
    int index = int(c.x[1]);
    if (mount_nup(name, content::addon(index), "add-on")) {
        c.x[0] = kOk;
        return;
    }
    fs::path dir = data_root() / "extract" / ("aoc" + std::to_string(index));
    std::error_code ec;
    if (fs::is_directory(dir, ec)) {
        mount(name, dir);
        c.x[0] = kOk;
        return;
    }
    log("fs: MountAddOnContent(%s, %d): no such add-on", name.c_str(), index);
    c.x[0] = kTargetNotFound;
}

void QueryMountAddOnContentCacheSize(CPU& c) {
    st<uint64_t>(c.x[0], 0x1000);
    c.x[0] = kOk;
}

void EnsureSaveData(CPU& c) { c.x[0] = kOk; }

void MountSaveData(CPU& c) {
    std::lock_guard lk(g_fs);
    std::string name = cstr(c.x[0]);
    mount(name, data_root() / "user" / "save");
    c.x[0] = kOk;
}

void Unmount(CPU& c) {
    std::lock_guard lk(g_fs);
    g_mounts.erase(cstr(c.x[0]));
    g_nup_mounts.erase(cstr(c.x[0]));
    c.x[0] = kOk;
}

void Commit(CPU& c) { c.x[0] = kOk; }
void EnableGlobalFileDataCache(CPU& c) { c.x[0] = kOk; }
void DisableGlobalFileDataCache(CPU& c) { c.x[0] = kOk; }

// ---- files ----------------------------------------------------------------------------------------------------

void CreateFile(CPU& c) {
    std::lock_guard lk(g_fs);
    fs::path p = resolve(cstr(c.x[0]));
    int64_t size = int64_t(c.x[1]);
    if (p.empty() || !fs::exists(p.parent_path())) {
        c.x[0] = kPathNotFound;
        return;
    }
    if (fs::exists(p)) {
        c.x[0] = kPathAlreadyExists;
        return;
    }
    FILE* f = plat::fopen_utf8(utf8(p), "wb");
    if (f) {
        if (size > 0) {
            plat::fseek64(f, size - 1, SEEK_SET);
            fputc(0, f);
        }
        fclose(f);
    }
    c.x[0] = f ? kOk : kPathNotFound;
}

void DeleteFile(CPU& c) {
    std::lock_guard lk(g_fs);
    fs::path p = resolve(cstr(c.x[0]));
    std::error_code ec;
    c.x[0] = !p.empty() && fs::remove(p, ec) ? kOk : kPathNotFound;
}

void CreateDirectory(CPU& c) {
    std::lock_guard lk(g_fs);
    fs::path p = resolve(cstr(c.x[0]));
    if (p.empty()) {
        c.x[0] = kPathNotFound;
        return;
    }
    if (fs::exists(p)) {
        c.x[0] = kPathAlreadyExists;
        return;
    }
    std::error_code ec;
    c.x[0] = fs::create_directory(p, ec) ? kOk : kPathNotFound;
}

void GetEntryType(CPU& c) {  // (DirectoryEntryType* out, path)
    std::lock_guard lk(g_fs);
    std::string guest = cstr(c.x[1]);
    if (is_nup_mount(guest)) {
        nup::Archive* ar;
        bool dir;
        uint32_t idx;
        if (!resolve_nup(guest, &ar, &dir, &idx)) {
            if (trace_fs()) log("fs: GetEntryType(%s) not found (package)", guest.c_str());
            c.x[0] = kPathNotFound;
            return;
        }
        st<uint32_t>(c.x[0], dir ? 0 : 1);
        c.x[0] = kOk;
        return;
    }
    fs::path p = resolve(guest);
    if (find_virtual(p)) {
        st<uint32_t>(c.x[0], 1);
        c.x[0] = kOk;
        return;
    }
    std::error_code ec;
    if (p.empty() || !fs::exists(p, ec)) {
        c.x[0] = kPathNotFound;
        return;
    }
    st<uint32_t>(c.x[0], fs::is_directory(p) ? 0 : 1);
    c.x[0] = kOk;
}

void OpenFile(CPU& c) {  // (FileHandle* out, path, int mode: 1 read, 2 write, 4 append)
    std::lock_guard lk(g_fs);
    std::string guest = cstr(c.x[1]);
    int mode = int(c.x[2]);
    if (is_nup_mount(guest)) {  // read only, like the RomFS on console
        nup::Archive* ar;
        bool dir;
        uint32_t idx;
        if (!resolve_nup(guest, &ar, &dir, &idx) || dir || (mode & 6)) {
            if (trace_fs()) log("fs: OpenFile(%s, %d) not found (package)", guest.c_str(), mode);
            c.x[0] = kPathNotFound;
            return;
        }
        uint64_t h = g_next++;
        g_files[h] = std::make_shared<File>(ar, idx, guest);
        st<uint64_t>(c.x[0], h);
        if (trace_fs()) log("fs: OpenFile(%s) -> %llu (package)", guest.c_str(), (unsigned long long)h);
        c.x[0] = kOk;
        return;
    }
    fs::path p = resolve(guest);
    if (const std::string* v = find_virtual(p)) {  // served from memory, through a temporary file
        FILE* f = std::tmpfile();
        if (!f) {
            c.x[0] = kPathNotFound;
            return;
        }
        std::fwrite(v->data(), 1, v->size(), f);
        std::fflush(f);
        uint64_t h = g_next++;
        g_files[h] = std::make_shared<File>(f, guest);
        st<uint64_t>(c.x[0], h);
        if (trace_fs()) log("fs: OpenFile(%s) -> virtual, %zu bytes", guest.c_str(), v->size());
        c.x[0] = kOk;
        return;
    }
    std::error_code ec;
    if (p.empty() || !fs::is_regular_file(p, ec)) {
        if (trace_fs()) log("fs: OpenFile(%s) not found", guest.c_str());
        c.x[0] = kPathNotFound;
        return;
    }
    FILE* f = plat::fopen_utf8(utf8(p), (mode & 2) ? "r+b" : "rb");
    if (!f) {
        c.x[0] = kPathNotFound;
        return;
    }
    uint64_t h = g_next++;
    g_files[h] = std::make_shared<File>(f, guest);
    st<uint64_t>(c.x[0], h);
    if (trace_fs()) log("fs: OpenFile(%s, %d) -> %llu", guest.c_str(), mode, (unsigned long long)h);
    c.x[0] = kOk;
}

std::shared_ptr<File> open_file(uint64_t h) {
    std::lock_guard lk(g_fs);
    auto it = g_files.find(h);
    return it == g_files.end() ? nullptr : it->second;
}

void CloseFile(CPU& c) {
    std::lock_guard lk(g_fs);
    g_files.erase(c.x[0]);
}

void GetFileSize(CPU& c) {  // (s64* out, FileHandle)
    auto file = open_file(c.x[1]);
    if (!file) {
        c.x[0] = kTargetNotFound;
        return;
    }
    if (file->ar) {
        st<int64_t>(c.x[0], int64_t(file->ar->file_size(file->nfile)));
        c.x[0] = kOk;
        return;
    }
    std::lock_guard lk(g_fs);  // the FILE position is shared with writers
    FILE* f = file->f;
    int64_t cur = plat::ftell64(f);
    plat::fseek64(f, 0, SEEK_END);
    st<int64_t>(c.x[0], plat::ftell64(f));
    if (g_trace) log("fs: size %s = %lld", file->path.c_str(), (long long)ld<int64_t>(c.x[0]));
    plat::fseek64(f, cur, SEEK_SET);
    c.x[0] = kOk;
}

uint64_t read_at(const File& file, int64_t off, uint64_t buf, uint64_t size) {
    uint64_t t0 = plat::monotonic_ns();
    uint64_t n;
    if (file.ar) {
        int64_t got = file.ar->read(file.nfile, uint64_t(off), reinterpret_cast<void*>(buf), size);
        if (got < 0) log("fs: read error in package: %s at %lld", file.path.c_str(), (long long)off);
        n = got < 0 ? 0 : uint64_t(got);
    } else {
        n = plat::read_at(file.f, off, reinterpret_cast<void*>(buf), size);
    }
    uint64_t dt = plat::monotonic_ns() - t0, ms = dt / 1'000'000;
    hitch::add(hitch::kFsReads, 1), hitch::add(hitch::kFsReadBytes, n), hitch::add(hitch::kFsReadNs, dt);
    if (g_trace) log("fs: read %s at %lld size %llu -> %llu", file.path.c_str(), (long long)off, (unsigned long long)size,
                     (unsigned long long)n);
    if (trace_fs() && file.path.find(".bwav") != std::string::npos) {  // streamed audio: is it being consumed?
        static std::mutex m;
        static std::map<std::string, uint64_t> reads;
        std::lock_guard lk(m);
        uint64_t k = ++reads[file.path];
        if (k <= 3 || k % 50 == 0) log("fs: stream read #%llu %s at %lld", (unsigned long long)k, file.path.c_str(), (long long)off);
    }
    if (ms >= 30)
        log("fs: slow read %s: %llu bytes at %lld took %llu ms", file.path.c_str(), (unsigned long long)size,
            (long long)off, (unsigned long long)ms);
    return n;
}

void ReadFileOut(CPU& c) {  // (size_t* out, FileHandle, s64 off, void* buf, size_t size)
    auto file = open_file(c.x[1]);
    if (!file) {
        c.x[0] = kTargetNotFound;
        return;
    }
    st<uint64_t>(c.x[0], read_at(*file, int64_t(c.x[2]), c.x[3], c.x[4]));
    c.x[0] = kOk;
}

void ReadFile(CPU& c) {  // (FileHandle, s64 off, void* buf, size_t size)
    auto file = open_file(c.x[0]);
    if (!file) {
        c.x[0] = kTargetNotFound;
        return;
    }
    read_at(*file, int64_t(c.x[1]), c.x[2], c.x[3]);
    c.x[0] = kOk;
}

void WriteFile(CPU& c) {  // (FileHandle, s64 off, const void* buf, size_t size, const WriteOption&)
    auto file = open_file(c.x[0]);
    if (!file) {
        c.x[0] = kTargetNotFound;
        return;
    }
    if (!file->f) {  // package files are read only
        c.x[0] = kPathNotFound;
        return;
    }
    std::lock_guard lk(g_fs);
    plat::fseek64(file->f, int64_t(c.x[1]), SEEK_SET);
    fwrite(reinterpret_cast<const void*>(c.x[2]), 1, c.x[3], file->f);
    fflush(file->f);
    hitch::add(hitch::kSaveWrites, 1), hitch::add(hitch::kSaveWriteBytes, c.x[3]);  // positional reads bypass the FILE buffer (saves only: rare and small)
    c.x[0] = kOk;
}

void FlushFile(CPU& c) {
    if (auto file = open_file(c.x[0]); file && file->f) {
        std::lock_guard lk(g_fs);
        fflush(file->f);
    }
    c.x[0] = kOk;
}

// ---- directories ----------------------------------------------------------------------------------------------

void OpenDirectory(CPU& c) {  // (DirectoryHandle* out, path, int mode: 1 dirs, 2 files)
    std::lock_guard lk(g_fs);
    std::string guest = cstr(c.x[1]);
    int mode = int(c.x[2]);
    Dir d;
    if (is_nup_mount(guest)) {
        nup::Archive* ar;
        bool isdir;
        uint32_t idx;
        if (!resolve_nup(guest, &ar, &isdir, &idx) || !isdir) {
            c.x[0] = kPathNotFound;
            return;
        }
        for (auto& e : ar->list(idx))
            if ((e.is_dir && (mode & 1)) || (!e.is_dir && (mode & 2))) d.entries.push_back({e.name, e.is_dir, e.size});
    } else {
        fs::path p = resolve(guest);
        std::error_code ec;
        if (p.empty() || !fs::is_directory(p, ec)) {
            c.x[0] = kPathNotFound;
            return;
        }
        for (auto& e : fs::directory_iterator(p, ec)) {
            bool dir = e.is_directory();
            if (!((dir && (mode & 1)) || (!dir && (mode & 2)))) continue;
            auto u8 = e.path().filename().u8string();
            d.entries.push_back({std::string(u8.begin(), u8.end()), dir, dir ? 0 : uint64_t(e.file_size(ec))});
        }
    }
    uint64_t h = g_next++;
    g_dirs[h] = std::move(d);
    st<uint64_t>(c.x[0], h);
    c.x[0] = kOk;
}

void ReadDirectory(CPU& c) {  // (s64* out_count, DirectoryEntry* entries, DirectoryHandle, s64 max)
    std::lock_guard lk(g_fs);
    auto it = g_dirs.find(c.x[2]);
    if (it == g_dirs.end()) {
        c.x[0] = kTargetNotFound;
        return;
    }
    Dir& d = it->second;
    int64_t n = 0, max = int64_t(c.x[3]);
    for (; n < max && d.pos < d.entries.size(); n++, d.pos++) {
        const auto& e = d.entries[d.pos];
        uint64_t out = c.x[1] + uint64_t(n) * 0x310;
        std::memset(reinterpret_cast<void*>(out), 0, 0x310);
        std::memcpy(reinterpret_cast<void*>(out), e.name.c_str(), std::min<size_t>(e.name.size(), 0x300));
        st<int8_t>(out + 0x304, e.dir ? 0 : 1);
        st<int64_t>(out + 0x308, e.dir ? 0 : int64_t(e.size));
    }
    st<int64_t>(c.x[0], n);
    c.x[0] = kOk;
}

void GetDirectoryEntryCount(CPU& c) {  // (s64* out, DirectoryHandle)
    std::lock_guard lk(g_fs);
    auto it = g_dirs.find(c.x[1]);
    st<int64_t>(c.x[0], it == g_dirs.end() ? 0 : int64_t(it->second.entries.size()));
    c.x[0] = kOk;
}

void CloseDirectory(CPU& c) {
    std::lock_guard lk(g_fs);
    g_dirs.erase(c.x[0]);
}

// ---- cache storage --------------------------------------------------------------------------------------------

void OpenCacheStorageList(CPU& c) {
    st<uint64_t>(c.x[0], 1);
    c.x[0] = kOk;
}
void ReadCacheStorageList(CPU& c) {  // (int* out, CacheStorageInfo*, handle, int)
    st<int32_t>(c.x[0], 0);
    c.x[0] = kOk;
}
void CloseCacheStorageList(CPU& c) {}
void CreateCacheStorage(CPU& c) { c.x[0] = kOk; }
void DeleteCacheStorage(CPU& c) { c.x[0] = kOk; }
void MountCacheStorage(CPU& c) {  // (name, int index)
    std::lock_guard lk(g_fs);
    mount(cstr(c.x[0]), data_root() / "user" / ("cache" + std::to_string(int(c.x[1]))));
    c.x[0] = kOk;
}

// ---- SDK 16 additions (Super Mario Bros. Wonder) ---------------------------------------------------------------
// MountRom(name) without a cache buffer, and ReadFile(size_t* out, FileHandle, s64, void*, size_t, const ReadOption&)
// are the older calls with one argument less / more: the same functions serve them.

void IsSaveDataExisting(CPU& c) { c.x[0] = 1; }  // (const account::Uid&) -> bool: EnsureSaveData made it

void SetFileSize(CPU& c) {  // (FileHandle, s64 size)
    auto file = open_file(c.x[0]);
    if (!file) {
        c.x[0] = kTargetNotFound;
        return;
    }
    if (!file->f) {
        c.x[0] = kPathNotFound;
        return;
    }
    std::lock_guard lk(g_fs);
    std::fflush(file->f);
#if defined(_WIN32)
    bool ok = _chsize_s(_fileno(file->f), int64_t(c.x[1])) == 0;
#else
    bool ok = ftruncate(fileno(file->f), off_t(c.x[1])) == 0;
#endif
    c.x[0] = ok ? kOk : kPathNotFound;
}

void DeleteDirectoryRecursively(CPU& c) {
    std::lock_guard lk(g_fs);
    fs::path p = resolve(cstr(c.x[0]));
    std::error_code ec;
    if (p.empty() || !fs::is_directory(p, ec)) {
        c.x[0] = kPathNotFound;
        return;
    }
    fs::remove_all(p, ec);
    c.x[0] = ec ? kPathNotFound : kOk;
}

}  // namespace

NX_HLE("_ZN2nn2fs22QueryMountRomCacheSizeEPm", QueryMountRomCacheSize);
NX_HLE("_ZN2nn2fs8MountRomEPKcPvm", MountRom);
NX_HLE("_ZN2nn2fs14EnsureSaveDataERKNS_7account3UidE", EnsureSaveData);
NX_HLE("_ZN2nn2fs13MountSaveDataEPKcRKNS_7account3UidE", MountSaveData);
NX_HLE("_ZN2nn2fs7UnmountEPKc", Unmount);
NX_HLE("_ZN2nn2fs10DeleteFileEPKc", DeleteFile);
NX_HLE("_ZN2nn2fs10CreateFileEPKcl", CreateFile);
NX_HLE("_ZN2nn2fs8OpenFileEPNS0_10FileHandleEPKci", OpenFile);
NX_HLE("_ZN2nn2fs11GetFileSizeEPlNS0_10FileHandleE", GetFileSize);
NX_HLE("_ZN2nn2fs15CreateDirectoryEPKc", CreateDirectory);
NX_HLE("_ZN2nn2fs12GetEntryTypeEPNS0_18DirectoryEntryTypeEPKc", GetEntryType);
NX_HLE("_ZN2nn2fs9CloseFileENS0_10FileHandleE", CloseFile);
NX_HLE("_ZN2nn2fs8ReadFileEPmNS0_10FileHandleElPvm", ReadFileOut);
NX_HLE("_ZN2nn2fs8ReadFileENS0_10FileHandleElPvm", ReadFile);
NX_HLE("_ZN2nn2fs9WriteFileENS0_10FileHandleElPKvmRKNS0_11WriteOptionE", WriteFile);
NX_HLE("_ZN2nn2fs9FlushFileENS0_10FileHandleE", FlushFile);
NX_HLE("_ZN2nn2fs25EnableGlobalFileDataCacheEPvm", EnableGlobalFileDataCache);
NX_HLE("_ZN2nn2fs26DisableGlobalFileDataCacheEv", DisableGlobalFileDataCache);
NX_HLE("_ZN2nn2fs6CommitEPKc", Commit);
NX_HLE("_ZN2nn2fs13OpenDirectoryEPNS0_15DirectoryHandleEPKci", OpenDirectory);
NX_HLE("_ZN2nn2fs13ReadDirectoryEPlPNS0_14DirectoryEntryENS0_15DirectoryHandleEl", ReadDirectory);
NX_HLE("_ZN2nn2fs14CloseDirectoryENS0_15DirectoryHandleE", CloseDirectory);
NX_HLE("_ZN2nn2fs22GetDirectoryEntryCountEPlNS0_15DirectoryHandleE", GetDirectoryEntryCount);
NX_HLE("_ZN2nn2fs20OpenCacheStorageListEPNS0_22CacheStorageListHandleE", OpenCacheStorageList);
NX_HLE("_ZN2nn2fs20ReadCacheStorageListEPiPNS0_16CacheStorageInfoENS0_22CacheStorageListHandleEi", ReadCacheStorageList);
NX_HLE("_ZN2nn2fs21CloseCacheStorageListENS0_22CacheStorageListHandleE", CloseCacheStorageList);
NX_HLE("_ZN2nn2fs18CreateCacheStorageEill", CreateCacheStorage);
NX_HLE("_ZN2nn2fs18DeleteCacheStorageEi", DeleteCacheStorage);
NX_HLE("_ZN2nn2fs17MountCacheStorageEPKci", MountCacheStorage);
NX_HLE("_ZN2nn2fs8MountRomEPKc", MountRom);
NX_HLE("_ZN2nn2fs8ReadFileEPmNS0_10FileHandleElPvmRKNS0_10ReadOptionE", ReadFileOut);
NX_HLE("_ZN2nn2fs18IsSaveDataExistingERKNS_7account3UidE", IsSaveDataExisting);
NX_HLE("_ZN2nn2fs11SetFileSizeENS0_10FileHandleEl", SetFileSize);
NX_HLE("_ZN2nn2fs26DeleteDirectoryRecursivelyEPKc", DeleteDirectoryRecursively);
// SDK 21 (Wonder, Switch 2 Edition)
NX_HLE("_ZN2nn2fs8MountRomEPKciPvm", MountRomOfProgram);
NX_HLE("_ZN2nn2fs22QueryMountRomCacheSizeEPmi", QueryMountRomCacheSizeOfProgram);
NX_HLE("_ZN2nn2fs17MountAddOnContentEPKciPvm", MountAddOnContent);
NX_HLE("_ZN2nn2fs31QueryMountAddOnContentCacheSizeEPmi", QueryMountAddOnContentCacheSize);

void add_virtual_file(const std::string& path, std::string data) {
    std::lock_guard lk(g_fs);
    g_virtual[virtual_key(fs::u8path(path))] = std::move(data);
}

std::string read_game_file(const std::string& relative_to_romfs) {
    std::error_code ec;
    const nup::Package* prog = content::program();
    if (!fs::is_directory(data_root() / "extract" / "romfs", ec) && prog && prog->romfs_dir != nup::kNone) {
        nup::Archive* ar = content::archive();
        bool dir;
        uint32_t idx;
        if (!ar->lookup(prog->romfs_dir, relative_to_romfs, &dir, &idx) || dir) return {};
        std::string s(size_t(ar->file_size(idx)), '\0');
        return ar->read(idx, 0, s.data(), s.size()) == int64_t(s.size()) ? s : std::string();
    }
    FILE* f = plat::fopen_utf8(utf8(data_root() / "extract" / "romfs" / fs::u8path(relative_to_romfs)), "rb");
    if (!f) return {};
    std::string s;
    char buf[65536];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) s.append(buf, n);
    std::fclose(f);
    return s;
}

}  // namespace nx
