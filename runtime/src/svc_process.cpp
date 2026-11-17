// Process-level SVCs and state that do not touch the scheduler: svcGetInfo, thread core masks, transfer memory
// mappings, and the initial static TLS every thread starts with. kernel.cpp dispatches to these.
#include <algorithm>
#include <cstring>
#include <vector>

#include <nx/game.h>
#include <nx/runtime.h>

#include "kernel.h"

namespace nx {

// ---- svcGetInfo -------------------------------------------------------------------------------------------------

static uint64_t core_mask() { return game::kInfo.core_mask ? game::kInfo.core_mask : 0xFull; }

static uint64_t total_memory() { return game::kInfo.total_memory ? game::kInfo.total_memory : 0xCD500000ull; }

uint64_t get_info(uint32_t type, uint32_t handle, uint64_t sub, uint32_t& rc) {
    (void)handle;
    rc = 0;
    switch (type) {
        case 0: return core_mask();                 // AllowedCpuIdBitmask (main.npdm's cores)
        case 1: return 0xFFFFFFFFFFFFFFFFull >> 0;  // AllowedThreadPriorityMask
        case 2: return kAliasBase;
        case 3: return kAliasSize;
        case 4: return kHeapBase;
        case 5: return kHeapMax;
        case 6: return total_memory();  // TotalMemoryAvailable (~3.2 GiB for a Switch application)
        case 7: return heap_size() + 0x4000000;
        case 8: return 0;  // IsCurrentProcessBeingDebugged
        case 9: return 0;  // ResourceLimit handle
        case 10: return 0;  // IdleTickCount
        case 11: return 0x1234567890ABCDEFull ^ sub;  // RandomEntropy
        case 12: return kAddressSpaceBase;
        case 13: return kAddressSpaceSize;
        case 14: return kStackBase;
        case 15: return kStackSize;
        case 16: return game::kInfo.system_resource_size;  // SystemResourceSizeTotal
        case 17: return 0;
        case 18: return game::kInfo.program_id;  // ProgramId
        case 20: {  // UserExceptionContextAddress: a page of ours (SDK 16 keeps its dying-message region at +0x1C0)
            static uint64_t ctx = kernel_alloc(0x1000);
            return ctx;
        }
        case 21: return total_memory();
        case 22: return heap_size() + 0x4000000;
        case 23: return 1;  // IsApplication
        case 28: return 0;  // AliasRegionExtraSize (18.0.0+)
        // Switch 2 kernel (SDK 20+; nnosInitialize aborts without them). Public notes on Switch 2 services describe
        // 29/30 as a region right after the alias region that may be empty, 31/32 as "remote memory" (none), and 33
        // as 0x1000.
        case 29: return kAliasBase + kAliasSize;
        case 30: return 0;
        case 31: return 0;
        case 32: return 0;
        case 33: return 0x1000;
        case 0xF0000002: return 0;  // ThreadTickCount
        default:
            log("svcGetInfo: unknown type %u sub %llu", type, (unsigned long long)sub);
            rc = 0xF001;
            return 0;
    }
}

// ---- svcSetThreadCoreMask ---------------------------------------------------------------------------------------

// (handle, ideal core, affinity mask): the core a thread then reports with svcGetCurrentProcessorNumber. Engines
// keep one job queue per core and pick theirs that way (Wonder's workers never saw their jobs while every thread
// reported core 0).
uint32_t set_thread_core_mask(uint32_t handle, int32_t ideal, uint64_t mask) {
    auto t = std::dynamic_pointer_cast<KThread>(handle_get(handle));
    if (!t || ideal == -3) return 0;  // -3: keep the ideal core
    if (ideal == -2) ideal = 0;       // the process's default core (main.npdm)
    if (ideal < 0 || !((mask >> ideal) & 1))
        for (int k = 0; k < 64; k++)
            if ((mask >> k) & 1) {
                ideal = k;
                break;
            }
    if (ideal >= 0) t->core = ideal;
    return 0;
}

// ---- transfer memory --------------------------------------------------------------------------------------------

// MapTransferMemory(handle, addr, size, perm): the owner cannot touch the memory while it is mapped, so a copy
// behaves the same (SDK 16's in-process audio renderer maps its work buffer this way).
uint32_t map_transfer_memory(uint32_t handle, uint64_t addr, uint64_t size) {
    auto tm = std::dynamic_pointer_cast<KTransferMemory>(handle_get(handle));
    if (!tm) return 0xE401;
    commit(addr, size);
    std::memcpy(reinterpret_cast<void*>(addr), reinterpret_cast<void*>(tm->addr), std::min(size, tm->size));
    vm_set(addr, size, kMemAlias, 3);
    return 0;
}

// UnmapTransferMemory(handle, addr, size): the contents go back to the owner.
uint32_t unmap_transfer_memory(uint32_t handle, uint64_t addr, uint64_t size) {
    auto tm = std::dynamic_pointer_cast<KTransferMemory>(handle_get(handle));
    if (tm) std::memcpy(reinterpret_cast<void*>(tm->addr), reinterpret_cast<void*>(addr), std::min(size, tm->size));
    decommit(addr, size);
    vm_set(addr, size, kMemFree, 0);
    return 0;
}

// ---- static TLS -------------------------------------------------------------------------------------------------

namespace {
std::vector<uint8_t> g_tls_template;
}

void set_tls_template(uint64_t start, uint64_t end) {
    g_tls_template.assign(reinterpret_cast<const uint8_t*>(start), reinterpret_cast<const uint8_t*>(end));
}

void init_tls_block(uint64_t block) {
    if (!g_tls_template.empty()) std::memcpy(reinterpret_cast<void*>(block + 0x10), g_tls_template.data(), g_tls_template.size());
}

}  // namespace nx
