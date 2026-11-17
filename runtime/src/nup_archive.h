// Read-only NUP package access for the runtime (format: github.com/a55uka/nup SPEC.md, MIT; see tools/nup.py for the
// Python reader). The RomFS of the program and its add-ons is served straight from the package: nothing is
// extracted. Files stored with zstd are inflated whole on first read and kept in a byte-bounded cache.
//
// Standalone (no runtime dependencies), so tests/nup_archive_test.cpp builds it alone.
#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace nx::nup {

constexpr uint32_t kNone = 0xFFFFFFFFu;

enum Kind : uint8_t { kBase = 0, kUpdate = 1, kAddon = 2 };

struct Package {
    uint64_t title_id;
    uint32_t root_dir, exefs_dir, romfs_dir, version;
    uint8_t kind, flags;
    uint64_t meta_offset = 0;  // the package's metadata TLVs in the file (0: none)
    uint32_t meta_size = 0;
};

struct Entry {  // a directory listing item
    std::string name;
    bool is_dir;
    uint32_t index;  // into dirs (is_dir) or files
    uint64_t size;   // files: uncompressed size
};

class Archive {
public:
    // Opens and validates (superblock, TOC SHA-256, every offset in bounds). nullptr and *err on failure.
    static std::unique_ptr<Archive> open(const std::string& path, std::string* err);
    ~Archive();

    const std::vector<Package>& packages() const { return packages_; }
    const std::string& path() const { return path_; }

    // Looks `rel` ('/'-separated, case-sensitive like Horizon's RomFS, leading '/' ignored) up under directory
    // `dir`. Returns false when it does not exist. "" or "/" is `dir` itself.
    bool lookup(uint32_t dir, std::string_view rel, bool* is_dir, uint32_t* index) const;
    std::vector<Entry> list(uint32_t dir) const;
    // The package's control.nacp: the 0x4000-byte metadata TLV (packers number the TLVs differently, so the size
    // decides, as tools/nup.py does), else a control.nacp file at the package's root. Empty when it has neither.
    std::vector<uint8_t> nacp(const Package& p);
    uint64_t file_size(uint32_t file) const;
    bool file_compressed(uint32_t file) const;

    // Reads up to `size` bytes at `off` of a file; returns the count (0 past the end), or -1 on an I/O or
    // decompression error. Thread-safe.
    int64_t read(uint32_t file, uint64_t off, void* dst, uint64_t size);

    // Bytes of inflated zstd files kept in memory (least recently used dropped first). Default 768 MiB.
    void set_cache_limit(uint64_t bytes);
    uint64_t cache_bytes() const;

private:
    struct Dir {
        uint32_t name_off, name_len, first_child, child_count;
    };
    struct File {
        uint32_t name_off, name_len;
        uint64_t size, extent_offset, extent_size;
        uint8_t codec;
    };
    struct Cached {
        uint32_t file;
        std::shared_ptr<std::vector<uint8_t>> data;
        uint64_t stamp;
    };

    Archive() = default;
    bool load(std::string* err);
    std::string_view name(uint32_t off, uint32_t len) const;
    std::string_view node_name(uint32_t node) const;
    bool pread(uint64_t off, void* dst, uint64_t size);
    std::shared_ptr<std::vector<uint8_t>> inflate(uint32_t file);

    std::string path_;
    void* handle_ = nullptr;  // HANDLE on Windows, int fd elsewhere (stored as pointer-sized value)
    uint64_t file_size_ = 0, blob_offset_ = 0;
    std::vector<Package> packages_;
    std::vector<Dir> dirs_;
    std::vector<File> files_;
    std::vector<uint32_t> children_;
    std::string pool_;

    mutable std::mutex cache_lock_;
    std::vector<Cached> cache_;
    uint64_t cache_bytes_ = 0, cache_limit_ = 768ull << 20, stamp_ = 0;
};

// SHA-256 (FIPS 180-4), used for the TOC check.
void sha256(const void* data, size_t size, uint8_t out[32]);

}  // namespace nx::nup
