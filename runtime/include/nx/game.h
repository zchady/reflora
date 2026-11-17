// What the shared runtime asks of the game it is built for. Each game implements this in games/<game>/game.cpp
// (CMake: -DNX_GAME=<game>); everything that only makes sense for one game lives there.
#pragma once

#include <cstdint>

namespace nx::game {

struct Info {
    const char* id;               // lowercase: CMake target, library, executable, log tag ("hades")
    const char* title;            // for the player: window title, Android data folder ("Hades")
    uint64_t program_id;          // svcGetInfo(ProgramId)
    const char* android_package;  // the APK's package (games/<game>/AndroidManifest.xml)
    const char* pc_data_root;     // PC: where analysis/ and extract/ are ("private/<game>")
    // main.npdm META +0x14. Non-zero enables nn::os virtual address memory (VammManager, SDK 16) over the alias
    // region; svcGetInfo(SystemResourceSizeTotal) reports it.
    uint64_t system_resource_size = 0;
    // svcGetInfo(TotalMemorySize / TotalNonSystemMemorySize). 0: 0xCD500000 (~3.2 GiB, a Switch application). Switch 2
    // applications get far more (about 9.5 GiB measured by other Switch 2 projects).
    uint64_t total_memory = 0;
    // svcGetInfo(CoreMask): the cores main.npdm grants (ThreadInfo capability). 0: 0xF (cores 0-3, a Switch application).
    // Switch 2 applications get cores 0-5; job systems keep a queue per core and size them from this mask.
    uint64_t core_mask = 0;
    // The system counter's rate (CNTFRQ, svcGetSystemTick, GPU timestamp reports). 0: 19.2 MHz (Switch). Switch 2
    // games run at 31.25 MHz: their SDK converts ticks with 31250000, and Wonder's frame-rate governor divides its frame
    // time by it (a 19.2 MHz counter made frames look longer and, mixed with nanoseconds, locked it at 30 fps).
    uint32_t tick_hz = 0;
};
extern const Info kInfo;

// After the modules' static constructors, right before nnMain: patch globals, add virtual files.
void after_init();
// The game starts its renderer (vkCreateInstance): settings the engine may have reset while loading its config.
void renderer_start();
// The first frame was presented.
void first_frame();

}  // namespace nx::game
