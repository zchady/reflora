// NX_ASTC_BC7=1 (Windows, off by default; nvn_astc_bc7.cpp): ASTC textures, which desktop GPUs cannot sample, kept as
// BC7 (8 bits per texel) instead of RGBA8 (32), from a persistent cache of conversions that passed a quality check
// against the RGBA8 decode. An image is made BC7 only when the cache already holds an accepted conversion of exactly
// its bytes; otherwise it is RGBA8 as before (the reference path). The cache is filled ahead of play by
// NX_ASTC_BC7_PREPARE (nvn_astc_bc7_prepare.cpp: every ASTC texture in the game's package), and during play by one
// idle-priority thread that converts what an RGBA8 upload decoded, for the next session.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "nvn_tiling.h"

namespace nx::nvn {

// What identifies a conversion: the hash and size of the texture's tiled ASTC bytes (texture_hash), the image's
// shape, the block height those bytes are untiled with, and (in the file name) the versions of the ASTC decoder, the
// BC7 encoder and the quality check.
struct Bc7Key {
    uint64_t hash = 0, tiled = 0;
    uint32_t format = 0, width = 0, height = 0, layers = 0, levels = 0, bh0 = 0;
};

enum class Bc7Decision { kUnknown, kBc7, kRgba8 };

bool astc_bc7_on();                   // switched on, and the device samples BC7 (decided at device creation)
void astc_bc7_init(bool bc_feature);  // nvn_vk.cpp, once the features are known
// The cache folder (NX_ASTC_BC7_CACHE, else <data root>/astc_bc7_cache), created if needed, and its index of
// verdicts. For the preparation, which needs no GPU. False when it cannot be used.
bool astc_bc7_open_cache();
const std::string& astc_bc7_cache_dir();
std::string astc_bc7_name(const Bc7Key& k);  // the key's file name, without extension
Bc7Decision astc_bc7_lookup(const Bc7Key& k);
// The cached blocks of an accepted conversion (all levels, each level's layers back to back) and their layout.
bool astc_bc7_load(const Bc7Key& k, std::vector<uint8_t>& blocks, std::vector<LinearLevel>& levels);
// Converts RGBA8 levels (decode_astc's layout) to BC7, judges every level and layer against them, and records the
// verdict in the cache (the blocks only when accepted). Returns the verdict; `blocks`/`out_levels` get the BC7 data
// when accepted. kUnknown: the verdict could not be written (it is then tried again another time).
Bc7Decision astc_bc7_build(const Bc7Key& k, const std::vector<uint8_t>& rgba, const std::vector<LinearLevel>& levels,
                           std::vector<uint8_t>& blocks, std::vector<LinearLevel>& out_levels);
// An RGBA8 upload's decode of a texture with no verdict yet, for the converter: one thread at idle priority converts
// and judges it for the next session (no work on the game's threads; dropped above a memory cap, tried another time).
void astc_bc7_enqueue(const Bc7Key& k, std::vector<uint8_t>&& rgba, std::vector<LinearLevel>&& levels);
// Something a BC7 image could not serve (image_remake_rgba8): this key is RGBA8 for the rest of the session. `why`:
// "copy", "texel writes", "view", "bytes changed", "no conversion to load" (counted, the first few logged).
void astc_bc7_demote(const Bc7Key& k, const char* why);
// Statistics (with each VRAM line).
void astc_bc7_note_image(bool bc7, uint64_t hash_ns);  // image_of's decision
void astc_bc7_note_upload(bool from_memory, uint64_t bytes, uint64_t ns);
void astc_bc7_note_problem(const char* what);  // the same counters as astc_bc7_demote, without a key
void astc_bc7_log();

// NX_ASTC_BC7_PREPARE=1 (nvn_astc_bc7_prepare.cpp): every ASTC texture of the game's package converted and judged
// into the cache, then the process exits (boot, once the game's modules are loaded and before it starts). Returns
// the exit code: 0 done, 3 no package or no cache folder, 5 some verdicts could not be written.
int astc_bc7_prepare();

}  // namespace nx::nvn
