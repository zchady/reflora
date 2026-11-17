// Where the game's content comes from when it is a NUP package (runtime/src/nup_archive.h) instead of an extracted
// folder: the program package (its RomFS is rom:/content:) and the add-ons that belong to it (aoc:u, MountAddOnContent).
//
// The package is NX_NUP, or the path written on the first line of <data root>/nup.txt. Without either, the runtime
// keeps using <data root>/extract/romfs as before.
#pragma once

#include <cstdint>
#include <vector>

#include "nup_archive.h"

namespace nx::content {

constexpr uint64_t kFamilyMask = 0x00FF'FFFF'FFFF'E000ull;  // title ID without platform byte and low 13 bits

// The open package, or nullptr (none configured, or it failed to open: the reason is logged once).
nup::Archive* archive();
// The program to run: the package whose title ID is the game's program ID (game::kInfo), else the first base package.
const nup::Package* program();
// Add-on content of this game's family, by add-on number (title ID - application - 0x1000), ascending.
struct Addon {
    int index;
    const nup::Package* package;
};
std::vector<Addon> addons();
const nup::Package* addon(int index);

// Pure helpers (tests/content_test.cpp).
bool same_family(uint64_t a, uint64_t b);
int addon_index(uint64_t title_id);  // 1.. for add-on title IDs, 0 otherwise

}  // namespace nx::content
