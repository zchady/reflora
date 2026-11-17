// NVN device (docs/NVN.md, phases N2-N3). Values come from what the game checks and from the SDK's own NVN driver
// (recompiled in sdk; NX_NVN_REAL=1 records where each of its procs lives, see nvn_trace.cpp).
#include <map>

#include <nx/runtime.h>

#include "nvn.h"
#include "nvn_tiling.h"

namespace nx {

namespace {

// nvnDeviceGetInteger(const NVNdevice*, NVNdeviceInfo pname, int* value).
// Read out of the SDK 16.2 driver (sdk+451090: a jump table over pname 0..0x62, constants inline or from a second
// table at sdk+440c80; see docs/nvn/API.md). 0, 1 are the API version: Wonder requires major == 55 and
// minor >= 15 (main+5a7b48). The "provisional" ones are fields the real nvnDeviceInitialize fills in its device
// struct; their values are placeholders until that init is read.
const std::map<uint32_t, int32_t> kDeviceInfo = {
    {0, 0x37},
    {1, 0xf},
    {2, 0xe},
    {3, 0x10000},
    {4, 0x100},
    {5, 0x8},
    {6, 0x10},
    {7, 0x4},
    {8, 0x10},
    {9, 0x20},
    {10, 0x10},
    {11, 0x4},
    {12, 0x4},
    {13, 0x4},
    {14, 0x10},
    {15, 0x20},
    {16, 0x20},
    {17, 0x100},
    {18, 0x100},
    {19, 0x4},
    {20, 0x8},
    {21, 0x200},
    {22, 0x100},
    {23, 0x2},  // provisional: the driver reads [dev+0xb4]
    {24, 0x1000},  // provisional: the driver reads [dev+0xb4]*[dev+0xac]
    {25, 0x1000},
    {26, 0x20000},
    {27, 0x10},
    {28, 0x600},
    {29, 0x400},
    {30, 0x40},
    {31, 0x600},
    {32, 0xffff},
    {33, 0xffff},
    {34, 0xffff},
    {35, 0x8},
    {36, 0x100000},
    {37, 0x1000},
    {38, 0x10},
    {39, 0x1000},
    {40, 0x1},
    {41, 0x1},
    {42, 0x1},
    {43, 0x40000},  // provisional: the driver reads [dev+0x20]<<10
    {44, 0xf},
    {45, 0x0},  // provisional: the driver reads [dev+0x3c]
    {46, 0x1},
    {47, 0x1},
    {48, 0x10},
    {49, 0x5},
    {50, 0x1},
    {51, 0x8},
    {52, 0x8},
    {53, 0x4},
    {54, 0x20},
    {55, 0x800},  // provisional: the driver reads [dev+0xac]
    {56, 0x0},  // provisional: the driver reads [dev+0x44]
    {57, 0x80},
    {58, 0x0},  // provisional: the driver reads [dev+0xc8]
    {59, 0x1},  // provisional: the driver reads !byte[dev+0x1b1d]
    {60, 0x4},
    {61, 0x0},  // provisional: the driver reads [dev+0x28]
    {62, 0x0},  // provisional: the driver reads [dev+0x40]
    {63, 0x0},  // provisional: the driver reads [dev+0x2c]
    {64, 0x0},  // provisional: the driver reads [dev+0x30]
    {65, 0x0},  // provisional: the driver reads [dev+0x34]
    {66, 0x1},
    {67, 0x1},
    {68, 0x1},
    {69, 0x1},
    {70, 0x5},
    {71, 0x1},
    {72, 0x1},
    {73, 0x1},
    {74, 0x1},
    {75, 0x1},
    {76, 0x1},
    {77, 0x10},
    {78, 0x400},
    {79, 0x20},
    {80, 0x1000},
    {81, 0x10000},
    {82, 0x10000},
    {83, 0x100},
    {84, 0x4000},
    {85, 0x40000},
    {86, 0x1000},
    {87, 0x1},
    {88, 0x4},
    {89, 0x2},
    {90, 0x0},
    {91, 0x1000},
    {92, 0x4000},
    {93, 0x1000},
    {94, 0x80},
    {95, 0x20},
    {96, 0xffff},
    {97, 0x1},
    {98, 0x0},  // provisional: the driver reads [dev+0xbc]
};

// The same for NVN 2 (NintendoSDK 21.4.1, Wonder's Switch 2 Edition): run out of that driver with
// tools/nvn_device_info.py (sdk+65b7c0, which nvnDeviceGetInteger at sdk+657190 calls). API 0xe7.0x27: Wonder requires
// major == 0xe7 and minor >= 0x27 (nnMain+0x241c) and otherwise never creates its graphics system.
const std::map<uint32_t, int32_t> kDeviceInfoNvn2 = {
    {0, 0xe7},
    {1, 0x27},
    {2, 0xe},
    {3, 0x10000},
    {4, 0x100},
    {5, 0x8},
    {6, 0x20},
    {7, 0x4},
    {8, 0x10},
    {9, 0x20},
    {10, 0x10},
    {11, 0x4},
    {12, 0x4},
    {13, 0x4},
    {14, 0x20},
    {15, 0x20},
    {16, 0x20},
    {17, 0x100},
    {18, 0x100},
    {19, 0x4},
    {20, 0x8},
    {21, 0x200},
    {22, 0x100},
    {23, 0x2},  // provisional: reads the device at 0xbc (placeholder as in the SDK 16 table)
    {24, 0x1000},  // provisional: reads the device at 0xb4, 0xbc (placeholder as in the SDK 16 table)
    {25, 0x1000},
    {26, 0x4000},
    {27, 0x10},
    {28, 0x400},
    {29, 0x400},
    {30, 0x40},
    {31, 0x400},
    {32, 0x7fffffff},
    {33, 0xffff},
    {34, 0xffff},
    {35, 0x8},
    {36, 0x100000},
    {37, 0x1000},
    {38, 0x10},
    {39, 0x1000},
    {40, 0x1},
    {41, 0x1},
    {42, 0x1},
    {43, 0x40000},  // provisional: reads the device at 0x28 (placeholder as in the SDK 16 table)
    {44, 0x10},
    {45, 0x0},  // provisional: reads the device at 0x44
    {46, 0xc9},
    {47, 0xc9},
    {48, 0x14},
    {49, 0x0},
    {50, 0x1},
    {51, 0x8},
    {52, 0x8},
    {53, 0x4},
    {54, 0x20},
    {55, 0x800},  // provisional: reads the device at 0xb0, 0xb4 (placeholder as in the SDK 16 table)
    {56, 0x0},  // provisional: reads the device at 0x4c
    {57, 0x80},
    {58, 0x0},  // provisional: reads the device at 0xd8
    {59, 0x1},
    {60, 0x4},
    {61, 0x0},  // provisional: reads the device at 0x30
    {62, 0x0},  // provisional: reads the device at 0x48
    {63, 0x0},  // provisional: reads the device at 0x34
    {64, 0x0},  // provisional: reads the device at 0x38
    {65, 0x0},  // provisional: reads the device at 0x3c
    {66, 0x1},
    {67, 0x1},
    {68, 0x1},
    {69, 0x1},
    {70, 0x5},
    {71, 0x1},
    {72, 0x1},
    {73, 0x1},
    {74, 0x1},
    {75, 0x1},
    {76, 0x1},
    {77, 0x10},
    {78, 0x800},
    {79, 0x20},
    {80, 0x1000},
    {81, 0x10000},
    {82, 0x10000},
    {83, 0x100},
    {84, 0x4000},
    {85, 0x40000},
    {86, 0x1000},
    {87, 0x1},
    {88, 0x4},
    {89, 0x2},
    {90, 0x0},
    {91, 0x1000},
    {92, 0x4000},
    {93, 0x1000},
    {94, 0x80},
    {95, 0x20},
    {96, 0xffff},
    {97, 0x1},
    {98, 0x0},  // provisional: reads the device at 0xc4
    {99, 0x100},
    {100, 0x10},
    {101, 0xffff},
    {102, 0xffff},
    {103, 0x100},
    {104, 0x1},
    {105, 0x10},
    {106, 0x10},
    {107, 0x10},
    {108, 0x1},
    {109, 0x1},
    {110, 0x1},
    {111, 0x100},
    {112, 0x0},
    {113, 0x10},
    {114, 0x8},
    {115, 0x10},
    {116, 0x20},
    {117, 0x1f},
    {118, 0x20000000},
    {119, 0x1000000},
    {120, 0x1000000},
    {121, 0x40000000},
    {122, 0x20},
    {123, 0x1},
    {124, 0x40},
    {125, 0x0},  // provisional: reads the device at 0xb8
    {126, 0x1},
    {127, 0x7fffffff},
    {128, 0x20},
    {129, 0x20},
    {130, 0x3fffff},
    {131, 0x200},
    {132, 0x100},
    {133, 0x20},
    {134, 0x3fff},
    {135, 0x0},  // provisional: reads the device at 0x115
    {136, 0x8},
    {137, 0x0},
    {138, 0x0},
    {139, 0x4},
    {140, 0x100},
    {141, 0x10},
    {142, 0x0},
    {143, 0x1},
};

// Which table: the one of the driver the game links. Property 0 (API major version) does not read the device, so the
// driver's own answer tells NVN 1 (0x37) from NVN 2 (0xe7).
const std::map<uint32_t, int32_t>& device_info(CPU& c) {
    static const std::map<uint32_t, int32_t>* table = [&] {
        uint64_t real = nvn::real_proc(c, "nvnDeviceGetInteger");
        int32_t major = 0;
        if (real) {
            uint64_t out = nvn::scratch(c, 4);
            st<int32_t>(out, 0);
            nvn::guest_call(c, real, {c.x[0], 0, out});
            major = ld<int32_t>(out);
        }
        log("nvn: driver API major version %#x -> %s table", major, major == 0xe7 ? "NVN 2" : "NVN 1");
        nvn::set_gob_layout(major == 0xe7 ? nvn::GobLayout::Nvn2 : nvn::GobLayout::Maxwell);  // how packaged textures are tiled
        return major == 0xe7 ? &kDeviceInfoNvn2 : &kDeviceInfo;
    }();
    return *table;
}

void nvnDeviceGetInteger(CPU& c) {
    uint32_t pname = uint32_t(c.x[1]);
    const auto& info = device_info(c);
    auto it = info.find(pname);
    if (it == info.end()) log("nvn: nvnDeviceGetInteger(%u) unknown -> 0 (lr %s)", pname, describe(c.x[30]).c_str());
    if (c.x[2]) st<int32_t>(c.x[2], it == info.end() ? 0 : it->second);
}

// nvnDeviceSetDepthMode(NVNdevice*, NVNdepthMode): 0 NEAR_IS_MINUS_W (clip z in -w..w, OpenGL), 1 NEAR_IS_ZERO
// (0..w, Direct3D/Vulkan). Logged, then the driver's own (diagnostic, §5w: the pipelines assume mode 0).
void nvnDeviceSetDepthMode(CPU& c) {
    log("nvn: nvnDeviceSetDepthMode(%llu)", (unsigned long long)c.x[1]);
    if (uint64_t real = nvn::real_proc(c, "nvnDeviceSetDepthMode")) call(c, real);
}

}  // namespace

NX_HLE("nvnDeviceSetDepthMode", nvnDeviceSetDepthMode);
NX_HLE("nvnDeviceGetInteger", nvnDeviceGetInteger);

}  // namespace nx
