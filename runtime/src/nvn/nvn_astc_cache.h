// ASTC textures decoded ahead of their first use (nvn_astc_cache.cpp).
#pragma once

#include <cstdint>
#include <vector>

#include "nvn_tiling.h"

namespace nx::nvn {

// What the background decode of an ASTC texture needs, read from the texture object under the objects lock
// (nvn_objects.cpp): its memory, format, the image's size as image_of makes it, and the block height its upload
// will use (fitted_block_height). False for a texture that is not a block-linear 2D / 2D-array ASTC texture.
struct AstcSource {
    uint64_t memory = 0, readable = 0, tiled_size = 0;
    uint32_t format = 0, width = 0, height = 0, layers = 0, levels = 0, bh0 = 0;
};
bool texture_astc_source(uint64_t guest, AstcSource& out);

// The block height an upload untiles a packaged texture with: the guess halved until the layout fits `gap`, the bytes
// to the next texture of its pool (nvn_upload.cpp).
uint32_t fitted_block_height(uint32_t guess, uint64_t gap, uint32_t width, uint32_t height, uint32_t layers,
                             uint32_t levels, uint32_t bw, uint32_t bh, uint32_t bytes);

// A packaged 2D array given 1 GOB (nvn_objects.cpp: the model BNTX arrays are tiled so) when the default block
// height's layout also fits `gap` and leaves less of it unused: the effect resources' arrays (VFXB), which keep the
// default (a 32-layer 128x128 BC5 smoke flipbook in 0xc0000 bytes: 0xbc000 with 1 GOB, 0xbfe00 with 4; the 105-layer
// model arrays: 4 GOBs do not fit). Else bh0.
uint32_t array_block_height(uint32_t bh0, uint64_t gap, uint32_t width, uint32_t height, uint32_t layers,
                            uint32_t levels, uint32_t bw, uint32_t bh, uint32_t bytes);

// Queues a texture just initialized by the game (nvnTextureInitialize) for decoding in the background.
void astc_predecode(uint64_t guest);
// The upload's side: the decoded texels and levels when a background decode of exactly this texture, from these
// bytes (hash of its tiled data), is there; waits for one in progress. False: decode as before.
bool astc_cache_take(uint64_t memory, uint32_t format, uint32_t width, uint32_t height, uint32_t layers,
                     uint32_t levels, uint32_t bh0, uint64_t hash, std::vector<uint8_t>& rgba,
                     std::vector<LinearLevel>& levels_out);
uint64_t texture_hash(const uint8_t* p, uint64_t n);
// nvn_upload.cpp: untiled ASTC blocks of every level -> RGBA8. foreground: on the replay thread (counted as its
// host work and in the hitch attribution); false: a background decode.
void decode_astc(uint32_t format, std::vector<uint8_t>& linear, std::vector<LinearLevel>& levels, uint32_t layers,
                 uint32_t bw, uint32_t bh, bool foreground = true);

}  // namespace nx::nvn
