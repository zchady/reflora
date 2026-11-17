// Texture storage layout (docs/NVN.md, N4): how many bytes of a memory pool an NVN texture occupies, and how it is
// aligned. The SDK driver computes this through its device's surface-layout object, which only its own
// nvnDeviceInitialize (talking to the GPU kernel driver) sets up, so we compute it here: Maxwell block-linear.
//
// Format facts come from the driver's own format table (0x30 bytes per NVNformat; sdk+0xd34198 in SDK 16.2,
// sdk+0xe43aa8 in SDK 21, Wonder's Switch 2 Edition, same numbering):
//   word 2: block width (byte 0), block height (byte 1); word 4: bytes per block (byte 0).
// The table has no symbol, so it is found by its contents (format_table()): at the offset a known SDK keeps it, else
// by a scan of the sdk image. Reading SDK 16's offset in SDK 21 gave garbage block sizes (0x29 RGBA16 as 16x0 blocks
// of 232 bytes, so the 4-byte default), storage sizes half the real ones and the upload read past the pool.
// Block linear: the image is cut into GOBs of 64 bytes x 8 rows; GOBs stack vertically into blocks of
// 1..16 GOBs (the block height, picked per mip level from the level's height); a level's rows of blocks are
// 64-byte aligned in width and block-aligned in height. Pre-tiled data (BNTX) is laid out the same way, so the
// sizes must not come out smaller than the real driver's.
#include <algorithm>

#include "nvn.h"

namespace nx::nvn {

namespace {

constexpr uint64_t kFormatTables[] = {0xd34198, 0xe43aa8};  // in sdk: SDK 16.2, SDK 21
constexpr uint32_t kFormatCount = 0xd0;
constexpr uint64_t kEntry = 0x30;

// Formats whose block size every NVN table agrees on: R8, RGBA8, RGBA16, BC1, BC4 (docs/nvn/API.md).
struct Known {
    uint32_t format, block_w, block_h, bytes;
};
constexpr Known kKnown[] = {{0x01, 1, 1, 1}, {0x25, 1, 1, 4}, {0x29, 1, 1, 8}, {0x41, 4, 4, 8}, {0x49, 4, 4, 8}};

bool looks_like_table(const uint8_t* t) {
    for (const Known& k : kKnown) {
        const uint32_t* e = reinterpret_cast<const uint32_t*>(t + k.format * kEntry);
        if ((e[2] & 0xFF) != k.block_w || ((e[2] >> 8) & 0xFF) != k.block_h || (e[4] & 0xFF) != k.bytes) return false;
    }
    return true;
}

// The table's address in the loaded sdk, 0 if none of the candidates (nor any 8-byte aligned offset) matches.
uint64_t format_table() {
    static uint64_t table = [] {
        const LoadedModule* m = module_at(find_symbol("nvnBootstrapLoader"));
        if (!m || m->size < kFormatCount * kEntry) return uint64_t(0);
        const uint64_t span = kFormatCount * kEntry;
        for (uint64_t off : kFormatTables)
            if (off + span <= m->size && looks_like_table(reinterpret_cast<const uint8_t*>(m->base + off))) {
                log("nvn: format table at sdk+%#llx", (unsigned long long)off);
                return m->base + off;
            }
        for (uint64_t off = 0; off + span <= m->size; off += 8)
            if (looks_like_table(reinterpret_cast<const uint8_t*>(m->base + off))) {
                log("nvn: format table found by scan at sdk+%#llx", (unsigned long long)off);
                return m->base + off;
            }
        log("nvn: format table not found in sdk: texture sizes use 4-byte texels");
        return uint64_t(0);
    }();
    return table;
}

struct FormatInfo {
    uint32_t block_w = 1, block_h = 1, bytes = 4;
};

FormatInfo format_info(uint32_t format) {
    const uint64_t table = format_table();
    FormatInfo f;
    if (!table || format >= kFormatCount) return f;
    const uint32_t* e = reinterpret_cast<const uint32_t*>(table + uint64_t(format) * kEntry);
    uint32_t bw = e[2] & 0xFF, bh = (e[2] >> 8) & 0xFF, bytes = e[4] & 0xFF;
    if (bw && bh && bytes) f = {bw, bh, bytes};
    return f;
}

uint32_t div_up(uint32_t a, uint32_t b) { return (a + b - 1) / b; }

// Block height in GOBs for a level `rows` block rows tall: the smallest power of two >= rows / 8, at most 16.
uint32_t block_height_gobs(uint32_t rows) {
    uint32_t gobs = div_up(rows, 8), h = 1;
    while (h < gobs && h < 16) h <<= 1;
    return h;
}

struct Layout {
    uint64_t size = 0, alignment = 512;
};

Layout compute(CPU& c, uint64_t builder) {
    uint32_t format = uint32_t(driver(c, "nvnTextureBuilderGetFormat", {builder}));
    uint32_t width = std::max<uint32_t>(1, uint32_t(driver(c, "nvnTextureBuilderGetWidth", {builder})));
    uint32_t height = std::max<uint32_t>(1, uint32_t(driver(c, "nvnTextureBuilderGetHeight", {builder})));
    uint32_t depth = std::max<uint32_t>(1, uint32_t(driver(c, "nvnTextureBuilderGetDepth", {builder})));
    uint32_t levels = std::max<uint32_t>(1, uint32_t(driver(c, "nvnTextureBuilderGetLevels", {builder})));
    uint32_t samples = std::max<uint32_t>(1, uint32_t(driver(c, "nvnTextureBuilderGetSamples", {builder})));
    uint32_t stride = uint32_t(driver(c, "nvnTextureBuilderGetStride", {builder}));
    FormatInfo f = format_info(format);
    Layout l;
    if (stride) {  // linear (pitch) texture
        l.size = uint64_t(stride) * div_up(height, f.block_h) * depth;
        l.alignment = 0x100;
        return l;
    }
    uint64_t layer = 0;
    uint32_t bh0 = block_height_gobs(div_up(height, f.block_h));
    for (uint32_t level = 0; level < levels; level++) {
        uint32_t w = std::max(1u, width >> level), h = std::max(1u, height >> level);
        uint32_t cols = div_up(w, f.block_w), rows = div_up(h, f.block_h);
        uint32_t bh = std::min(bh0, block_height_gobs(rows));
        uint64_t pitch = (uint64_t(cols) * f.bytes + 63) & ~63ull;
        uint64_t tall = (uint64_t(rows) + 8 * bh - 1) / (8 * bh) * (8 * bh);
        layer += pitch * tall;
    }
    uint64_t block = 512ull * bh0;  // one block: 64 bytes x 8 rows x block height
    layer = (layer + block - 1) / block * block;
    l.size = layer * depth * samples;
    l.alignment = block;
    return l;
}

void GetStorageSize(CPU& c) { c.x[0] = compute(c, c.x[0]).size; }            // size_t (const NVNtextureBuilder*)
void GetStorageAlignment(CPU& c) { c.x[0] = compute(c, c.x[0]).alignment; }  // size_t (const NVNtextureBuilder*)
void GetStorageClass(CPU& c) { c.x[0] = 0; }  // NVNstorageClass: only matters for compression / sparse tiles

}  // namespace

uint64_t texture_storage_size(CPU& c, uint64_t builder) { return compute(c, builder).size; }

void format_block(uint32_t format, uint32_t& block_w, uint32_t& block_h, uint32_t& bytes) {
    FormatInfo f = format_info(format);
    block_w = f.block_w, block_h = f.block_h, bytes = f.bytes;
}

NX_HLE("nvnTextureBuilderGetStorageSize", GetStorageSize);
NX_HLE("nvnTextureBuilderGetStorageAlignment", GetStorageAlignment);
NX_HLE("nvnTextureBuilderGetStorageClass", GetStorageClass);
NX_HLE("nvnTextureBuilderGetRawStorageClass", GetStorageClass);

}  // namespace nx::nvn
