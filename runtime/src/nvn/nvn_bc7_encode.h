// BC7 encoding on the CPU, for NX_ASTC_BC7 (nvn_astc_bc7.cpp): ASTC textures, decoded to RGBA8 because desktop GPUs
// have no ASTC, kept as BC7 (8 bits per texel instead of 32). Written for this project from the BC7 specification
// (Khronos Data Format Specification, BPTC); the partition and anchor tables are nvn_bc.cpp's (derived there from a
// reference decoder). Self-contained (no nvn.h, no Vulkan): tests compile it on its own.
#pragma once

#include <cstdint>

namespace nx::nvn {

// Bumped whenever the encoder's output can change: it is part of the persistent cache's key.
constexpr uint32_t kBc7EncoderVersion = 3;

struct Bc7EncodeStats {
    uint64_t blocks = 0, single = 0, two = 0, three = 0;  // blocks by the subsets of the mode chosen
    uint64_t tried2 = 0, tried3 = 0;                      // blocks that tried two and three subsets
    uint64_t rescued = 0;                                 // blocks the exhaustive pass improved
};

// One 4x4 block of RGBA8 texels (row-major) -> 16 bytes of BC7.
void bc7_encode_block(const uint8_t px[16][4], uint8_t out[16], Bc7EncodeStats* stats = nullptr);

// A width x height RGBA8 image (rows tightly packed) -> ceil(w/4) x ceil(h/4) blocks, rows of blocks tightly packed.
// Partial edge blocks repeat the last column and row.
void bc7_encode_image(const uint8_t* rgba, uint32_t width, uint32_t height, uint8_t* blocks,
                      Bc7EncodeStats* stats = nullptr);

}  // namespace nx::nvn
