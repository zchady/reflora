// runtime/src/nup_archive.cpp on the SYNTHETIC package tests/data/synthetic.nup (tests/nup_synth.py test_vector()).
// Usage: nup_archive_test <synthetic.nup>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "nup_archive.h"

static int fails = 0;
#define CHECK(x)                                                     \
    do {                                                             \
        if (!(x)) {                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); \
            fails++;                                                 \
        }                                                            \
    } while (0)

using namespace nx::nup;

static std::string read_all(Archive& a, uint32_t f) {
    std::string s(size_t(a.file_size(f)), '\0');
    int64_t n = a.read(f, 0, s.data(), s.size());
    return n == int64_t(s.size()) ? s : std::string("<read error>");
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: nup_archive_test <synthetic.nup> | --sha <file.nup> <package> <romfs path>...\n");
        return 2;
    }
    if (std::string(argv[1]) == "--sha" && argc >= 4) {  // manual check on a real package: SHA-256 of files as read
        std::string err;
        auto a = Archive::open(argv[2], &err);
        if (!a) return std::printf("open: %s\n", err.c_str()), 1;
        const Package& p = a->packages()[size_t(std::atoi(argv[3]))];
        for (int k = 4; k < argc; k++) {
            bool d;
            uint32_t f;
            if (!a->lookup(p.romfs_dir, argv[k], &d, &f) || d) {
                std::printf("missing %s\n", argv[k]);
                continue;
            }
            std::string s = read_all(*a, f);
            uint8_t h[32];
            sha256(s.data(), s.size(), h);
            std::printf("%s %zu ", argv[k], s.size());
            for (int i = 0; i < 8; i++) std::printf("%02x", h[i]);
            std::printf("\n");
        }
        return 0;
    }
    {  // SHA-256 known answers (FIPS 180-4 examples)
        uint8_t h[32];
        sha256("abc", 3, h);
        CHECK(h[0] == 0xba && h[1] == 0x78 && h[31] == 0xad);
        const char* m = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
        sha256(m, std::strlen(m), h);
        CHECK(h[0] == 0x24 && h[1] == 0x8d && h[31] == 0xc1);
    }
    std::string err;
    auto a = Archive::open(argv[1], &err);
    CHECK(a != nullptr);
    if (!a) {
        std::printf("open: %s\n", err.c_str());
        return 1;
    }
    CHECK(a->packages().size() == 2);
    const Package& prog = a->packages()[0];
    const Package& aoc = a->packages()[1];
    CHECK(prog.title_id == 0x040015100B514001ull && prog.kind == kBase && prog.romfs_dir != kNone);
    CHECK(aoc.title_id == 0x010015100B515001ull && aoc.kind == kAddon && aoc.exefs_dir == kNone);

    {  // the program's control.nacp from its metadata TLV (am GetDisplayVersion), none for the add-on
        std::vector<uint8_t> nacp = a->nacp(prog);
        CHECK(nacp.size() == 0x4000);
        CHECK(nacp.size() == 0x4000 && std::strcmp(reinterpret_cast<const char*>(nacp.data() + 0x3060), "9.8.7") == 0);
        CHECK(a->nacp(aoc).empty());
    }

    bool dir = false;
    uint32_t idx = 0;
    CHECK(a->lookup(prog.romfs_dir, "System/AddressTable", &dir, &idx) && dir);
    CHECK(a->lookup(prog.romfs_dir, "/System/AddressTable/a.bin", &dir, &idx) && !dir);
    CHECK(read_all(*a, idx) == "raw-file-content");
    CHECK(!a->lookup(prog.romfs_dir, "system/addresstable/a.bin", nullptr, nullptr));  // case-sensitive
    CHECK(!a->lookup(prog.romfs_dir, "System/../Stage/b.txt", nullptr, nullptr));
    CHECK(!a->lookup(prog.romfs_dir, "System/AddressTable/a.bin/x", nullptr, nullptr));
    CHECK(a->lookup(prog.romfs_dir, "", &dir, &idx) && dir && idx == prog.romfs_dir);

    CHECK(a->lookup(prog.romfs_dir, "Stage/b.txt", &dir, &idx) && !dir);
    std::string hello;
    for (int k = 0; k < 100; k++) hello += "hello ";
    CHECK(a->file_compressed(idx) && read_all(*a, idx) == hello);
    char part[5] = {};
    CHECK(a->read(idx, 597, part, 10) == 3 && std::string(part) == "lo ");  // short read at the end
    CHECK(a->read(idx, 600, part, 4) == 0);                                  // past the end

    CHECK(a->lookup(prog.romfs_dir, "System/big.bin", &dir, &idx));
    std::vector<uint8_t> want(200000);
    for (size_t k = 0; k < want.size(); k++) want[k] = uint8_t((k * 7 + k / 13) & 0xFF);
    std::vector<uint8_t> got(want.size());
    CHECK(a->read(idx, 0, got.data(), got.size()) == int64_t(got.size()) && got == want);
    // Concurrent reads of the same compressed file and an evicting cache.
    a->set_cache_limit(1);
    std::vector<std::thread> ts;
    int bad = 0;
    for (int t = 0; t < 8; t++)
        ts.emplace_back([&, t] {
            std::vector<uint8_t> b(1000);
            for (int r = 0; r < 50; r++) {
                uint64_t off = uint64_t((t * 977 + r * 3989) % 199000);
                if (a->read(idx, off, b.data(), b.size()) != 1000 || std::memcmp(b.data(), want.data() + off, 1000)) bad++;
            }
        });
    for (auto& t : ts) t.join();
    CHECK(bad == 0);

    CHECK(a->lookup(prog.romfs_dir, "empty", &dir, &idx) && !dir && a->file_size(idx) == 0);
    auto top = a->list(prog.romfs_dir);
    CHECK(top.size() == 3 && top[0].name == "Stage" && top[0].is_dir && top[1].name == "System" && top[2].name == "empty");
    CHECK(a->lookup(aoc.romfs_dir, "System/AocUpgradeVersion.txt", &dir, &idx) && read_all(*a, idx) == "1");

    // Corruption: a changed TOC byte must be refused.
    {
        std::ifstream in(argv[1], std::ios::binary);
        std::string raw((std::istreambuf_iterator<char>(in)), {});
        raw[0x80 + 0x40] ^= 1;
        std::string bad_path = "nup_archive_test.corrupt.tmp";  // in the working directory (the build dir)
        std::ofstream(bad_path, std::ios::binary).write(raw.data(), std::streamsize(raw.size()));
        CHECK(!Archive::open(bad_path, &err) && err.find("SHA-256") != std::string::npos);
        raw.pop_back();
        std::ofstream(bad_path, std::ios::binary | std::ios::trunc).write(raw.data(), std::streamsize(raw.size()));
        CHECK(!Archive::open(bad_path, &err) && err.find("file_size") != std::string::npos);
        std::remove(bad_path.c_str());
    }
    std::printf(fails ? "FAILED %d\n" : "ok\n", fails);
    return fails ? 1 : 0;
}
