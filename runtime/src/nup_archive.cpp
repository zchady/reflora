// NUP package reader for the runtime: see nup_archive.h.
#include "nup_archive.h"

#include <algorithm>
#include <cstring>

#if defined(_WIN32)
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

#define ZSTD_STATIC_LINKING_ONLY
#include "../third_party/zstd/zstd.h"

namespace nx::nup {

namespace {

template <typename T>
T rd(const uint8_t* p) {
    T v;
    std::memcpy(&v, p, sizeof v);
    return v;
}

constexpr uint64_t kSuper = 0x80, kPkg = 0x40, kDir = 0x18, kFile = 0x50;

}  // namespace

// ---- SHA-256 --------------------------------------------------------------------------------------------------

void sha256(const void* data, size_t size, uint8_t out[32]) {
    static const uint32_t k[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98,
        0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
        0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8,
        0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
        0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819,
        0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
        0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
        0xc67178f2};
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    auto rotr = [](uint32_t x, int n) { return (x >> n) | (x << (32 - n)); };
    auto block = [&](const uint8_t* p) {
        uint32_t w[64];
        for (int i = 0; i < 16; i++) w[i] = uint32_t(p[4 * i]) << 24 | uint32_t(p[4 * i + 1]) << 16 | uint32_t(p[4 * i + 2]) << 8 | p[4 * i + 3];
        for (int i = 16; i < 64; i++) {
            uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; i++) {
            uint32_t t1 = hh + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + k[i] + w[i];
            uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
            hh = g, g = f, f = e, e = d + t1, d = c, c = b, b = a, a = t1 + t2;
        }
        h[0] += a, h[1] += b, h[2] += c, h[3] += d, h[4] += e, h[5] += f, h[6] += g, h[7] += hh;
    };
    const uint8_t* p = static_cast<const uint8_t*>(data);
    size_t n = size;
    for (; n >= 64; n -= 64, p += 64) block(p);
    uint8_t tail[128] = {};
    std::memcpy(tail, p, n);
    tail[n] = 0x80;
    size_t tl = n + 9 <= 64 ? 64 : 128;
    uint64_t bits = uint64_t(size) * 8;
    for (int i = 0; i < 8; i++) tail[tl - 1 - i] = uint8_t(bits >> (8 * i));
    block(tail);
    if (tl == 128) block(tail + 64);
    for (int i = 0; i < 8; i++)
        for (int j = 0; j < 4; j++) out[4 * i + j] = uint8_t(h[i] >> (24 - 8 * j));
}

// ---- file access ----------------------------------------------------------------------------------------------

Archive::~Archive() {
#if defined(_WIN32)
    if (handle_) CloseHandle(static_cast<HANDLE>(handle_));
#else
    if (handle_) close(int(reinterpret_cast<intptr_t>(handle_)) - 1);
#endif
}

bool Archive::pread(uint64_t off, void* dst, uint64_t size) {
    auto* out = static_cast<uint8_t*>(dst);
    while (size) {
        uint32_t chunk = uint32_t(std::min<uint64_t>(size, 1u << 30));
#if defined(_WIN32)
        OVERLAPPED ov{};
        ov.Offset = DWORD(off);
        ov.OffsetHigh = DWORD(off >> 32);
        DWORD got = 0;
        if (!ReadFile(static_cast<HANDLE>(handle_), out, chunk, &got, &ov) || got != chunk) return false;
#else
        ssize_t got = ::pread(int(reinterpret_cast<intptr_t>(handle_)) - 1, out, chunk, off_t(off));
        if (got != ssize_t(chunk)) return false;
#endif
        out += chunk, off += chunk, size -= chunk;
    }
    return true;
}

std::unique_ptr<Archive> Archive::open(const std::string& path, std::string* err) {
    std::unique_ptr<Archive> a(new Archive());
    a->path_ = path;
#if defined(_WIN32)
    int wn = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
    std::wstring w(size_t(wn > 0 ? wn : 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, w.data(), wn);
    HANDLE h = CreateFileW(w.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        if (err) *err = "cannot open " + path;
        return nullptr;
    }
    a->handle_ = h;
    LARGE_INTEGER sz;
    GetFileSizeEx(h, &sz);
    a->file_size_ = uint64_t(sz.QuadPart);
#else
    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) {
        if (err) *err = "cannot open " + path;
        return nullptr;
    }
    a->handle_ = reinterpret_cast<void*>(intptr_t(fd) + 1);
    a->file_size_ = uint64_t(lseek(fd, 0, SEEK_END));
#endif
    if (!a->load(err)) return nullptr;
    return a;
}

bool Archive::load(std::string* err) {
    auto fail = [&](const char* why) {
        if (err) *err = path_ + ": " + why;
        return false;
    };
    uint8_t sb[kSuper];
    if (file_size_ < kSuper || !pread(0, sb, kSuper)) return fail("too small");
    if (std::memcmp(sb, "NUP\x01", 4) != 0) return fail("not a NUP v1 file");
    uint16_t hsz = rd<uint16_t>(sb + 4);
    uint64_t toc_off = rd<uint64_t>(sb + 8), toc_size = rd<uint64_t>(sb + 0x10);
    blob_offset_ = rd<uint64_t>(sb + 0x38);
    uint64_t declared = rd<uint64_t>(sb + 0x40);
    uint32_t pkg_count = rd<uint32_t>(sb + 0x48), flags = rd<uint32_t>(sb + 0x4C);
    if (hsz < kSuper || toc_off != hsz) return fail("bad header size");
    if (flags) return fail("unknown superblock flags");
    if (declared != file_size_) return fail("file_size does not match the file (truncated?)");
    if (blob_offset_ % 16 || blob_offset_ < toc_off + toc_size || blob_offset_ > file_size_ || toc_size > (1ull << 31))
        return fail("bad TOC/blob layout");
    if (!pkg_count) return fail("no packages");
    std::vector<uint8_t> toc(toc_size);
    if (!pread(toc_off, toc.data(), toc_size)) return fail("cannot read the TOC");
    uint8_t sha[32];
    sha256(toc.data(), toc.size(), sha);
    if (std::memcmp(sha, sb + 0x18, 32) != 0) return fail("TOC SHA-256 mismatch");

    const uint8_t* t = toc.data();
    if (toc_size < 0x30 || std::memcmp(t, "TOC\x01", 4) != 0 || rd<uint32_t>(t + 4) != pkg_count) return fail("bad TOC header");
    uint64_t dc = rd<uint64_t>(t + 8), fc = rd<uint64_t>(t + 0x10);
    uint64_t pool_off = rd<uint64_t>(t + 0x18), pool_size = rd<uint64_t>(t + 0x20);
    uint64_t dbase = 0x30 + pkg_count * kPkg, fbase = dbase + dc * kDir, cbase = fbase + fc * kFile;
    if (dc > (1u << 31) || fc > (1u << 31) || cbase > toc_size || pool_off + pool_size > toc_size || pool_off < cbase)
        return fail("TOC truncated");
    pool_.assign(reinterpret_cast<const char*>(t + pool_off), size_t(pool_size));

    for (uint32_t i = 0; i < pkg_count; i++) {
        const uint8_t* p = t + 0x30 + i * kPkg;
        Package pk{rd<uint64_t>(p), rd<uint32_t>(p + 8), rd<uint32_t>(p + 0xC), rd<uint32_t>(p + 0x10),
                   rd<uint32_t>(p + 0x14), p[0x18], p[0x19]};
        pk.meta_offset = rd<uint64_t>(p + 0x20), pk.meta_size = rd<uint32_t>(p + 0x28);  // <QIIIIBBHIQI
        if (pk.kind >= 5) return fail("unknown package kind");
        for (uint32_t d : {pk.root_dir, pk.exefs_dir, pk.romfs_dir})
            if (d != kNone && d >= dc) return fail("package directory out of range");
        packages_.push_back(pk);
    }
    uint64_t slots = 0;
    for (uint64_t i = 0; i < dc; i++) {
        const uint8_t* p = t + dbase + i * kDir;
        Dir d{rd<uint32_t>(p), rd<uint32_t>(p + 4), rd<uint32_t>(p + 8), rd<uint32_t>(p + 0xC)};
        if (uint64_t(d.name_off) + d.name_len > pool_size) return fail("directory name out of the string pool");
        slots = std::max<uint64_t>(slots, uint64_t(d.first_child) + d.child_count);
        dirs_.push_back(d);
    }
    if (cbase + slots * 4 > pool_off) return fail("children array truncated");
    children_.resize(size_t(slots));
    std::memcpy(children_.data(), t + cbase, size_t(slots) * 4);
    for (uint32_t c : children_)
        if (c >= dc + fc) return fail("child node out of range");
    for (uint64_t i = 0; i < fc; i++) {
        const uint8_t* p = t + fbase + i * kFile;
        File f{rd<uint32_t>(p), rd<uint32_t>(p + 4), rd<uint64_t>(p + 8), rd<uint64_t>(p + 0x10), rd<uint64_t>(p + 0x18),
               p[0x40]};
        if (uint64_t(f.name_off) + f.name_len > pool_size) return fail("file name out of the string pool");
        if (f.codec > 1) return fail("unknown codec");
        if (f.extent_size && (f.extent_offset < blob_offset_ || f.extent_offset + f.extent_size > file_size_))
            return fail("extent out of the blob region");
        if (f.codec == 0 && f.extent_size != f.size) return fail("raw extent size differs from the file size");
        files_.push_back(f);
    }
    return true;
}

// ---- tree -----------------------------------------------------------------------------------------------------

std::string_view Archive::name(uint32_t off, uint32_t len) const { return std::string_view(pool_).substr(off, len); }

std::string_view Archive::node_name(uint32_t node) const {
    if (node < dirs_.size()) return name(dirs_[node].name_off, dirs_[node].name_len);
    const File& f = files_[node - dirs_.size()];
    return name(f.name_off, f.name_len);
}

bool Archive::lookup(uint32_t dir, std::string_view rel, bool* is_dir, uint32_t* index) const {
    if (dir >= dirs_.size()) return false;
    uint32_t cur = dir;
    bool cur_dir = true;
    size_t pos = 0;
    while (pos < rel.size()) {
        while (pos < rel.size() && (rel[pos] == '/' || rel[pos] == '\\')) pos++;
        if (pos >= rel.size()) break;
        size_t end = rel.find_first_of("/\\", pos);
        std::string_view part = rel.substr(pos, end == std::string_view::npos ? std::string_view::npos : end - pos);
        pos = end == std::string_view::npos ? rel.size() : end;
        if (part == ".") continue;
        if (!cur_dir || part == "..") return false;
        const Dir& d = dirs_[cur];
        // Children are sorted bytewise by name (SPEC 3.3): binary search.
        auto first = children_.begin() + d.first_child, last = first + d.child_count;
        auto it = std::lower_bound(first, last, part, [&](uint32_t n, std::string_view v) { return node_name(n) < v; });
        if (it == last || node_name(*it) != part) return false;
        cur_dir = *it < dirs_.size();
        cur = cur_dir ? *it : uint32_t(*it - dirs_.size());
    }
    if (is_dir) *is_dir = cur_dir;
    if (index) *index = cur;
    return true;
}

std::vector<Entry> Archive::list(uint32_t dir) const {
    std::vector<Entry> out;
    if (dir >= dirs_.size()) return out;
    const Dir& d = dirs_[dir];
    for (uint32_t k = 0; k < d.child_count; k++) {
        uint32_t n = children_[d.first_child + k];
        bool isd = n < dirs_.size();
        uint32_t idx = isd ? n : uint32_t(n - dirs_.size());
        out.push_back({std::string(node_name(n)), isd, idx, isd ? 0 : files_[idx].size});
    }
    return out;
}

uint64_t Archive::file_size(uint32_t file) const { return file < files_.size() ? files_[file].size : 0; }
bool Archive::file_compressed(uint32_t file) const { return file < files_.size() && files_[file].codec == 1; }

// ---- contents -------------------------------------------------------------------------------------------------

std::shared_ptr<std::vector<uint8_t>> Archive::inflate(uint32_t file) {
    {
        std::lock_guard lk(cache_lock_);
        for (auto& c : cache_)
            if (c.file == file) {
                c.stamp = ++stamp_;
                return c.data;
            }
    }
    const File& f = files_[file];
    std::vector<uint8_t> packed(size_t(f.extent_size));
    if (!pread(f.extent_offset, packed.data(), f.extent_size)) return nullptr;
    auto out = std::make_shared<std::vector<uint8_t>>(size_t(f.size));
    size_t n = ZSTD_decompress(out->data(), out->size(), packed.data(), packed.size());
    if (ZSTD_isError(n) || n != f.size) return nullptr;
    std::lock_guard lk(cache_lock_);
    for (auto& c : cache_)  // another thread inflated it meanwhile
        if (c.file == file) return c.data;
    cache_.push_back({file, out, ++stamp_});
    cache_bytes_ += f.size;
    while (cache_bytes_ > cache_limit_ && cache_.size() > 1) {
        auto lru = std::min_element(cache_.begin(), cache_.end(), [](auto& a, auto& b) { return a.stamp < b.stamp; });
        cache_bytes_ -= lru->data->size();
        cache_.erase(lru);
    }
    return out;
}

std::vector<uint8_t> Archive::nacp(const Package& p) {
    constexpr uint32_t kNacpSize = 0x4000;
    std::vector<uint8_t> out;
    if (p.meta_offset && p.meta_size && p.meta_size < (16u << 20)) {
        std::vector<uint8_t> meta(p.meta_size);
        if (pread(p.meta_offset, meta.data(), meta.size()))
            for (size_t o = 0; o + 8 <= meta.size();) {
                const uint32_t size = rd<uint32_t>(meta.data() + o + 4);
                if (o + 8 + uint64_t(size) > meta.size()) break;
                if (size == kNacpSize) return std::vector<uint8_t>(meta.begin() + o + 8, meta.begin() + o + 8 + size);
                o += 8 + size;
            }
    }
    bool is_dir = false;
    uint32_t file = 0;
    if (p.root_dir != kNone && lookup(p.root_dir, "control.nacp", &is_dir, &file) && !is_dir &&
        file_size(file) == kNacpSize) {
        out.resize(kNacpSize);
        if (read(file, 0, out.data(), kNacpSize) != int64_t(kNacpSize)) out.clear();
    }
    return out;
}

int64_t Archive::read(uint32_t file, uint64_t off, void* dst, uint64_t size) {
    if (file >= files_.size()) return -1;
    const File& f = files_[file];
    if (off >= f.size || !size) return 0;
    uint64_t n = std::min(size, f.size - off);
    if (f.codec == 0) return pread(f.extent_offset + off, dst, n) ? int64_t(n) : -1;
    auto data = inflate(file);  // held while copying: eviction by another thread cannot free it
    if (!data) return -1;
    std::memcpy(dst, data->data() + off, size_t(n));
    return int64_t(n);
}

void Archive::set_cache_limit(uint64_t bytes) {
    std::lock_guard lk(cache_lock_);
    cache_limit_ = bytes;
}

uint64_t Archive::cache_bytes() const {
    std::lock_guard lk(cache_lock_);
    return cache_bytes_;
}

}  // namespace nx::nup
