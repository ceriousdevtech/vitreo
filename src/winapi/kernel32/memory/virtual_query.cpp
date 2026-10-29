/// @file virtual_query.cpp
/// @brief VirtualProtect / VirtualQuery / FlushInstructionCache implementation
///        (WIN32-036)

#include "core/memory/exec_write_source.hpp"
#include "winapi/kernel32/memory/virtual_query.hpp"
#include "winapi/kernel32/memory/file_mapping.hpp"
#include "winapi/kernel32/memory/memory.hpp"
#include "winapi/kernel32/module/module.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/core/marshaler.hpp"
#include "ipc/ipc_gate.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace vitreo::winapi {

// =============================================================================
// VirtualQueryManager
// =============================================================================

VirtualQueryManager::VirtualQueryManager(
    GuestMemory& memory,
    VirtualMemoryManager* virtual_memory,
    FileMappingManager* file_mappings) noexcept
    : m_memory(memory)
    , m_virtual_memory(virtual_memory)
    , m_file_mappings(file_mappings) {
    if (m_virtual_memory != nullptr && file_mappings != nullptr) {
        m_virtual_memory->set_file_mapping_manager(file_mappings);
    }
}

bool VirtualQueryManager::virtual_protect(u64 address, usize size,
                                          u32 new_protect,
                                          u32* old_protect) {
    m_last_protect_error = 0;
    if (m_modules != nullptr) {
        const int img = m_modules->virtual_protect_image(
            static_cast<GuestAddr>(address), size, new_protect, old_protect);
        if (img == 1) return true;
        if (img < 0) {
            m_last_protect_error =
                img == -2 ? error_codes::ERROR_INVALID_PARAMETER
                          : error_codes::ERROR_INVALID_ADDRESS;
            return false;
        }
    }
    return m_virtual_memory != nullptr &&
           m_virtual_memory->virtual_protect(
               address, size, new_protect, old_protect);
}

// -----------------------------------------------------------------------------
// virtual_query
// -----------------------------------------------------------------------------

usize VirtualQueryManager::virtual_query(u64 address,
                                         MemoryBasicInformation& info,
                                         u64 maximum_address) const {
    // Zero-initialise all fields (including padding).
    info = MemoryBasicInformation{};

    // GetSystemInfo and VirtualQuery describe the same process address space.
    // A reserved linear guest window may lower the usual Windows x64 ceiling.
    const GuestAddr configured_ceiling = m_memory.maximum_user_address();
    if (configured_ceiling != 0) {
        maximum_address = std::min(
            maximum_address, static_cast<u64>(configured_ceiling));
    }

    if (address > maximum_address) {
        return 0;
    }

    // Page-align the queried address.
    const u64 page_base = align_down(address, static_cast<u64>(GUEST_PAGE_SIZE));

    info.base_address = page_base;

    VirtualAllocationQuery allocation{};
    if (m_virtual_memory != nullptr &&
        m_virtual_memory->virtual_query(page_base, allocation)) {
        info.base_address       = allocation.base_address;
        info.allocation_base    = allocation.allocation_base;
        info.allocation_protect = allocation.allocation_protect;
        info.region_size        = allocation.region_size;
        info.state              = allocation.state;
        info.protect            = allocation.protect;
        info.type               = MEM_PRIVATE;
        return sizeof(MemoryBasicInformation);
    }

    GuestAddr region_base = 0;
    usize     region_requested = 0;
    MemoryProtection region_prot{};

    if (m_memory.query_region(static_cast<GuestAddr>(page_base),
                              region_base, region_requested, region_prot)) {
        // Committed GuestMemory region. MapViewOfFile views report MEM_MAPPED
        // (Learn); loaded PE images report MEM_IMAGE; other regions stay
        // MEM_PRIVATE.
        u32 prot_flags = native_prot_to_win32(region_prot);

        // Learn: AllocationBase is the base of the initial allocation (the
        // whole GuestMemory mapping), not the current protection run.
        GuestAddr mapping_base = region_base;
        usize mapping_size = region_requested;
        if (m_memory.query_mapping(static_cast<GuestAddr>(page_base),
                                   mapping_base, mapping_size)) {
            info.allocation_base = mapping_base;
            GuestAddr map_region_base = 0;
            usize map_region_size = 0;
            MemoryProtection map_base_prot{};
            if (m_memory.query_region(mapping_base, map_region_base,
                                     map_region_size, map_base_prot)) {
                info.allocation_protect = native_prot_to_win32(map_base_prot);
            } else {
                info.allocation_protect = prot_flags;
            }
        } else {
            info.allocation_base = region_base;
            info.allocation_protect = prot_flags;
        }
        info.region_size        = (region_base + region_requested) - page_base;
        info.state              = MEM_COMMIT;
        info.protect            = prot_flags;
        info.type               = MEM_PRIVATE;

        if (m_file_mappings != nullptr) {
            GuestAddr view_base = 0;
            u64 view_size = 0;
            u32 view_protection = 0;
            u32 view_allocation_protection = 0;
            u64 protection_run_end = 0;
            bool view_image = false;
            bool view_reserved = false;
            if (m_file_mappings->query_view(
                    static_cast<GuestAddr>(page_base), view_base, view_size,
                    &view_protection, &protection_run_end,
                    &view_allocation_protection, &view_image,
                    &view_reserved)) {
                info.allocation_base = view_base;
                info.region_size = std::min(
                    static_cast<u64>(view_base) + view_size,
                    protection_run_end) - page_base;
                info.allocation_protect = view_allocation_protection;
                info.protect = view_protection;
                info.type = view_image ? MEM_IMAGE : MEM_MAPPED;
                if (view_reserved) {
                    info.state = MEM_RESERVE;
                    info.protect = 0;
                }
            }
        }
        if (m_modules != nullptr) {
            GuestAddr image_base = 0;
            u64 image_size = 0;
            u32 image_protect = 0;
            u32 image_alloc_protect = 0;
            u64 image_run_end = 0;
            if (m_modules->query_image_protection(
                    static_cast<GuestAddr>(page_base), image_base, image_size,
                    &image_protect, &image_run_end, &image_alloc_protect)) {
                info.allocation_base = image_base;
                info.allocation_protect = image_alloc_protect;
                info.protect = image_protect;
                info.region_size = image_run_end - page_base;
                info.type = MEM_IMAGE;
            } else {
                const auto at = m_modules->module_for_address(
                    static_cast<GuestAddr>(page_base));
                if (at.info != nullptr) {
                    // Loaded PE without protection_runs (synthetic / bare
                    // registration): Type is MEM_IMAGE and AllocationBase is
                    // the image base; Protect stays host-derived. Every image
                    // allocation is PAGE_EXECUTE_WRITECOPY, header page
                    // included (nt_query_virtual_memory_probe mbi_exe_header,
                    // mbi_ntdll_header: aprot=0x80).
                    info.allocation_base = at.info->base_address;
                    info.allocation_protect = ModuleInfo::kImageAllocationProtect;
                    info.type = MEM_IMAGE;
                }
            }
        }
    } else {
        // No mapping at this address → report as free.
        // Compute the size of the contiguous free gap up to the next region
        // or the address-space ceiling.
        GuestAddr next_base = m_memory.next_region_base(
            static_cast<GuestAddr>(page_base));
        const u64 address_limit = maximum_address + 1;
        const u64 free_end = next_base
                     ? std::min(static_cast<u64>(next_base), address_limit)
                     : address_limit;

        info.allocation_base    = 0;
        info.allocation_protect = 0;
        info.region_size        = free_end - page_base;
        info.state              = MEM_FREE;
        info.protect            = PAGE_NOACCESS;
        info.type               = 0;
    }

    return sizeof(MemoryBasicInformation);
}

// -----------------------------------------------------------------------------
// flush_instruction_cache
// -----------------------------------------------------------------------------
//
// Microsoft Learn (FlushInstructionCache): applications must call this after
// generating or modifying code so subsequent execution observes the new bytes.
// On native x86/x64 the CPU I/D caches are coherent so a NULL-base flush is
// an I-cache barrier, not a guest-byte change. Ranged flushes still invalidate
// Vitreo JIT translations covering that range.

bool VirtualQueryManager::flush_instruction_cache(
    u64 base_address,
    usize size) const noexcept {
    // Microsoft Learn (FlushInstructionCache): lpBaseAddress may be NULL;
    // dwSize is the flushed region only when the base is non-NULL.
    // On x86/x64 the CPU I/D caches are coherent, so this is an I-cache
    // barrier for bytes that were already stored — not a signal that guest
    // code changed. Vitreo's JIT cache is a translation of those bytes and
    // is already invalidated by executable stores and VirtualProtect to
    // PAGE_EXECUTE_*. Emitting the (0, 0) full-cache sentinel here discarded
    // every translated block on CRT/loader NULL flushes.
    if (base_address == 0 || size == 0) {
        return true;
    }
    exec_write_source::Scope tag(exec_write_source::kFlush);
    m_memory.notify_executable_write(static_cast<GuestAddr>(base_address), size);
    return true;
}

// =============================================================================
// Thunk Implementations
// =============================================================================

// VITREO_LOG_VALLOC gates the whole virtual-memory call log (VirtualAlloc,
// VirtualFree, VirtualProtect). Resolved once: a failing allocator retries hard
// enough that getenv() would show up in the trace's own cost.
static bool log_vmem_calls() {
    static const bool enabled = std::getenv("VITREO_LOG_VALLOC") != nullptr;
    return enabled;
}

// LPVOID VirtualAlloc(LPVOID lpAddress, SIZE_T dwSize,
//                     DWORD flAllocationType, DWORD flProtect);

void impl_VirtualAlloc(Marshaler& m, VirtualMemoryManager& vmm) {
    // caller_ret names WHICH guest code allocates. A runaway allocation loop is
    // otherwise an undifferentiated wall of 64 KB commits with no suspect.
    VITREO_TRACE("[vmem] VirtualAlloc caller_ret=0x%llx size=%llu type=0x%x\n",
                 static_cast<unsigned long long>(m.guest_return_address()),
                 static_cast<unsigned long long>(m.arg(1)),
                 static_cast<unsigned>(m.arg(2)));
    const u64 lp_address     = m.arg(0);
    const u64 dw_size        = m.arg(1);
    const u32 alloc_type     = static_cast<u32>(m.arg(2));
    const u32 fl_protect     = static_cast<u32>(m.arg(3));

    // MEM_RESET / MEM_RESET_UNDO are standalone allocation types that neither
    // reserve nor commit, so they are valid without MEM_COMMIT | MEM_RESERVE.
    if (dw_size == 0 ||
        (alloc_type & (MEM_COMMIT | MEM_RESERVE | MEM_RESET | MEM_RESET_UNDO))
            == 0) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return;
    }

    // Invalid protect / exclusive flag combinations → ERROR_INVALID_PARAMETER
    // (Windows / Learn), not ERROR_NOT_ENOUGH_MEMORY.
    constexpr u32 kResetOnly = MEM_RESET | MEM_RESET_UNDO;
    if ((alloc_type & kResetOnly) != 0 &&
        (alloc_type & ~kResetOnly) != 0) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return;
    }
    if ((alloc_type & kResetOnly) == 0) {
        const u32 flag_err = virtual_alloc_flag_error(alloc_type, fl_protect);
        if (flag_err != 0) {
            set_last_error(flag_err);
            m.set_return(0);
            return;
        }
    }
    if ((alloc_type & (MEM_COMMIT | MEM_RESERVE)) != 0 &&
        !is_supported_win32_protection(fl_protect)) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return;
    }

    const u64 addr = vmm.virtual_alloc(lp_address, dw_size, alloc_type, fl_protect);
    // Logged once-per-call, so resolve the gate once rather than per call: a
    // failing allocator retries hard enough that getenv() would show up in the
    // trace's own cost.
    if (log_vmem_calls()) {
        // The guest return address is the diffable key: the same binary run on
        // a reference implementation reaches the same call sites, so a size
        // that disagrees there localises the divergence to one call site.
        std::fprintf(stderr,
            "[valloc] ret=0x%llx VirtualAlloc(hint=0x%llx size=0x%llx "
            "type=0x%x prot=0x%x) -> 0x%llx%s\n",
            (unsigned long long)m.guest_return_address(),
            (unsigned long long)lp_address, (unsigned long long)dw_size,
            alloc_type, fl_protect, (unsigned long long)addr,
            addr == 0 ? " FAIL" : "");
    }
    if (addr == 0) {
        // Learn: MEM_COMMIT (no MEM_RESERVE) on a non-NULL range that is not
        // entirely reserved → ERROR_INVALID_ADDRESS.
        const bool commit_only =
            (alloc_type & MEM_COMMIT) != 0 &&
            (alloc_type & MEM_RESERVE) == 0 &&
            lp_address != 0;
        const u32 dedicated = vmm.take_last_alloc_error();
        if (dedicated != 0) {
            set_last_error(dedicated);
        } else if (commit_only) {
            set_last_error(error_codes::ERROR_INVALID_ADDRESS);
        } else if ((alloc_type & (MEM_RESET | MEM_RESET_UNDO)) != 0) {
            set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        } else {
            set_last_error(error_codes::ERROR_NOT_ENOUGH_MEMORY);
        }
        m.set_return(0);
        return;
    }

    m.set_return(addr);
}

// VirtualAlloc2's AllocationType / PageProtection / BaseAddress validation
// for the reserving and committing forms (MEM_RESET* are handled apart).
// Learn lists the values; which combinations Windows accepts is measured
// (tools/oracle_probe/mem_reset_probe{32,64}.expected "VirtualAlloc2 types"):
// MEM_TOP_DOWN, MEM_WRITE_WATCH (with MEM_RESERVE) and MEM_PHYSICAL combine
// as in VirtualAlloc; MEM_RESERVE_PLACEHOLDER needs MEM_RESERVE, no commit and
// PAGE_NOACCESS (MEM_TOP_DOWN allowed); unlike VirtualAlloc, the low 0x3F
// bits and unknown bits are ERROR_INVALID_PARAMETER. A size that is not a
// page multiple is rounded up (measured), except for placeholder operations,
// whose ranges must match pages exactly. Only a reservation needs an
// allocation-granularity base; MEM_COMMIT at a page-aligned base inside a
// reservation succeeds (measured at +0x11000).
static u32 virtual_alloc2_type_error(u32 type, u32 protect, u64 base, u64 size,
                                     bool is_64bit) {
    constexpr u32 kKnown = MEM_COMMIT | MEM_RESERVE | MEM_TOP_DOWN |
                           MEM_WRITE_WATCH | MEM_PHYSICAL | MEM_LARGE_PAGES |
                           MEM_RESERVE_PLACEHOLDER | MEM_REPLACE_PLACEHOLDER;
    if (size == 0 || (type & ~kKnown) != 0 ||
        (type & (MEM_COMMIT | MEM_RESERVE)) == 0) {
        return error_codes::ERROR_INVALID_PARAMETER;
    }
    const bool placeholder_op =
        (type & (MEM_RESERVE_PLACEHOLDER | MEM_REPLACE_PLACEHOLDER)) != 0;
    if (placeholder_op) {
        const bool create =
            (type & ~MEM_TOP_DOWN) == (MEM_RESERVE | MEM_RESERVE_PLACEHOLDER) &&
            protect == PAGE_NOACCESS;
        const bool replace =
            type == (MEM_RESERVE | MEM_REPLACE_PLACEHOLDER) ||
            type == (MEM_COMMIT | MEM_RESERVE | MEM_REPLACE_PLACEHOLDER);
        if ((!create && !replace) || (size % GUEST_PAGE_SIZE) != 0 ||
            !is_supported_win32_protection(protect)) {
            return error_codes::ERROR_INVALID_PARAMETER;
        }
    } else {
        // WOW64: a NULL-base commit carries its own reservation.
        const u32 effective =
            (!is_64bit && base == 0 && (type & MEM_COMMIT) != 0)
                ? (type | MEM_RESERVE) : type;
        if (const u32 err = virtual_alloc_flag_error(effective, protect)) {
            return err;
        }
        if (!is_supported_win32_protection(protect)) {
            return error_codes::ERROR_INVALID_PARAMETER;
        }
    }
    if (base != 0 && (type & MEM_RESERVE) != 0 && (base % 0x10000u) != 0) {
        return error_codes::ERROR_INVALID_PARAMETER;
    }
    return 0;
}

// PVOID VirtualAlloc2(HANDLE Process, PVOID BaseAddress, SIZE_T Size,
//                     ULONG AllocationType, ULONG PageProtection,
//                     MEM_EXTENDED_PARAMETER* ExtendedParameters,
//                     ULONG ParameterCount);
static void impl_VirtualAlloc2(Marshaler& m, VirtualMemoryManager& vmm) {
    const u64 process = m.arg(0);
    const u64 base = m.arg(1);
    const u64 size = m.arg(2);
    const u32 allocation_type = static_cast<u32>(m.arg(3));
    const u32 protection = static_cast<u32>(m.arg(4));
    const u64 extended_parameters = m.arg(5);
    const u32 parameter_count = static_cast<u32>(m.arg(6));

    // Microsoft Learn permits NULL for the calling process. Vitreo also
    // accepts the current-process pseudo handle in either guest pointer width.
    const bool current_process =
        process == 0 || process == ~u64{0} || process == 0xFFFF'FFFFu;
    if (!current_process) {
        set_last_error(error_codes::ERROR_NOT_SUPPORTED);
        m.set_return(0);
        return;
    }
    if (parameter_count != 0) {
        // Address-requirement and NUMA extended parameters need their own
        // placement policy. Do not silently ignore them.
        (void)extended_parameters;
        set_last_error(error_codes::ERROR_NOT_SUPPORTED);
        m.set_return(0);
        return;
    }

    // MEM_RESET / MEM_RESET_UNDO stand alone and share VirtualAlloc's
    // contract, including its errors (mem_reset_probe: identical through both
    // entry points). Chromium's DiscardSystemPages reaches this with 8 GiB of
    // reserved NOACCESS pool after DiscardVirtualMemory refuses non-RW pages.
    if (allocation_type == MEM_RESET || allocation_type == MEM_RESET_UNDO) {
        const u64 result =
            vmm.virtual_alloc_reset(base, size, allocation_type, protection);
        if (result == 0) {
            const u32 err = vmm.take_last_alloc_error();
            set_last_error(err != 0 ? err : error_codes::ERROR_INVALID_PARAMETER);
            m.set_return(0);
            return;
        }
        VITREO_TRACE("[vmem] VirtualAlloc2(base=0x%llx size=0x%llx type=0x%x)"
                     " -> 0x%llx\n",
                     static_cast<unsigned long long>(base),
                     static_cast<unsigned long long>(size), allocation_type,
                     static_cast<unsigned long long>(result));
        m.set_return(result);
        return;
    }

    const u32 type_error = virtual_alloc2_type_error(
        allocation_type, protection, base, size, m.cpu_is_64bit());
    if (type_error != 0) {
        set_last_error(type_error);
        m.set_return(0);
        return;
    }
    // WOW64 measured: with a NULL base, MEM_COMMIT implies the reservation,
    // so MEM_COMMIT | MEM_WRITE_WATCH succeeds there (and fails on x64).
    const u32 effective_type =
        (!m.cpu_is_64bit() && base == 0 && (allocation_type & MEM_COMMIT) != 0)
            ? (allocation_type | MEM_RESERVE)
            : allocation_type;

    const u64 result =
        vmm.virtual_alloc2(base, size, effective_type, protection);
    if (result == 0) {
        set_last_error(
            (allocation_type & MEM_REPLACE_PLACEHOLDER) != 0
                ? error_codes::ERROR_INVALID_ADDRESS
                : error_codes::ERROR_NOT_ENOUGH_MEMORY);
        m.set_return(0);
        return;
    }
    VITREO_TRACE("[vmem] VirtualAlloc2(process=0x%llx base=0x%llx size=0x%llx"
                 " type=0x%x protect=0x%x) -> 0x%llx\n",
                 static_cast<unsigned long long>(process),
                 static_cast<unsigned long long>(base),
                 static_cast<unsigned long long>(size), allocation_type,
                 protection, static_cast<unsigned long long>(result));
    ipc::ipc_log(
        "VirtualAlloc2 process=0x%llx base=0x%llx size=0x%llx type=0x%x "
        "protect=0x%x -> 0x%llx",
        static_cast<unsigned long long>(process),
        static_cast<unsigned long long>(base),
        static_cast<unsigned long long>(size), allocation_type, protection,
        static_cast<unsigned long long>(result));
    m.set_return(result);
}

// BOOL VirtualFree(LPVOID lpAddress, SIZE_T dwSize, DWORD dwFreeType);

void impl_VirtualFree(Marshaler& m, VirtualMemoryManager& vmm) {
    const u64 lp_address = m.arg(0);
    const u64 dw_size    = m.arg(1);
    const u32 free_type  = static_cast<u32>(m.arg(2));

    if (lp_address == 0) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return;
    }

    const bool preserve_placeholder =
        free_type == (MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER);
    const bool coalesce_placeholders =
        free_type == (MEM_RELEASE | MEM_COALESCE_PLACEHOLDERS);

    // Plain MEM_RELEASE releases an entire allocation and therefore requires
    // zero size. Placeholder split/coalesce operations deliberately carry a
    // non-zero range.
    if (free_type == MEM_RELEASE && dw_size != 0) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return;
    }

    if (coalesce_placeholders && dw_size == 0) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return;
    }
    if (free_type != MEM_RELEASE && free_type != MEM_DECOMMIT &&
        !preserve_placeholder && !coalesce_placeholders) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return;
    }

    // Attribute the call to guest code: a release that lands on Vitreo-owned
    // memory is a contract bug worth naming, and the caller RVA is the entry
    // point for disassembling it.
    u64 caller_ret = 0;
    if (auto* cpu = m.cpu()) {
        auto r = m.memory().read_u32(cpu->sp());
        if (r.is_ok()) caller_ret = static_cast<u64>(r.value());
    }
    VITREO_TRACE("[vmem] guest VirtualFree(addr=0x%llx, size=%llu, type=0x%x)"
                 " caller_ret=0x%llx\n",
                 static_cast<unsigned long long>(lp_address),
                 static_cast<unsigned long long>(dw_size),
                 static_cast<unsigned>(free_type),
                 static_cast<unsigned long long>(caller_ret));

    const bool ok = vmm.virtual_free(lp_address, dw_size, free_type);
    // Shares VITREO_LOG_VALLOC with VirtualAlloc so one log interleaves the
    // whole page-allocator conversation. A no-access permission change is a
    // MEM_DECOMMIT, and a false return there is indistinguishable from an
    // allocation failure by the time it reaches the caller.
    if (log_vmem_calls()) {
        std::fprintf(stderr,
            "[vfree] ret=0x%llx VirtualFree(addr=0x%llx size=0x%llx "
            "type=0x%x) -> %d%s\n",
            (unsigned long long)m.guest_return_address(),
            (unsigned long long)lp_address, (unsigned long long)dw_size,
            free_type, ok ? 1 : 0, ok ? "" : " FAIL");
    }
    if (!ok) {
        // Flag/size rules above already returned ERROR_INVALID_PARAMETER.
        // Remaining VMM failures are "not a reservation base / not allocated"
        // (Windows VirtualFree bad_addr/not_base → ERROR_INVALID_ADDRESS).
        set_last_error(error_codes::ERROR_INVALID_ADDRESS);
        m.set_return(0);
        return;
    }

    m.set_return(1);
}

// BOOL VirtualProtect(LPVOID lpAddress, SIZE_T dwSize,
//                     DWORD flNewProtect, PDWORD lpflOldProtect);
//
// Win64 calling convention (x64):
//   RCX = lpAddress       → arg0 (u64)
//   RDX = dwSize          → arg1 (u64)
//   R8  = flNewProtect    → arg2 (u32)
//   R9  = lpflOldProtect  → arg3 (u64 pointer)
//
// We read the four arguments as u64 values from the guest stack, treating
// them as the standard Win64 ABI layout.

void impl_VirtualProtect(Marshaler& m, VirtualQueryManager& vqm) {
    const u64 lp_address           = m.arg(0);
    const u64 dw_size              = m.arg(1);
    const u32 fl_new_protect       = static_cast<u32>(m.arg(2));
    const GuestAddr lp_old_protect = m.arg_ptr(3);

    if (lp_old_protect == 0 ||
        !m.memory().is_range_writable(
            lp_old_protect, sizeof(u32), m.cpu_is_64bit())) {
        set_last_error(error_codes::ERROR_NOACCESS);
        m.set_return(0);
        return;
    }

    u32 old_prot = 0;
    const bool ok = vqm.virtual_protect(
        lp_address, static_cast<usize>(dw_size), fl_new_protect, &old_prot);

    if (log_vmem_calls()) {
        std::fprintf(stderr,
            "[vprot] ret=0x%llx VirtualProtect(addr=0x%llx size=0x%llx "
            "prot=0x%x) -> %d old=0x%x%s\n",
            (unsigned long long)m.guest_return_address(),
            (unsigned long long)lp_address, (unsigned long long)dw_size,
            fl_new_protect, ok ? 1 : 0, old_prot, ok ? "" : " FAIL");
    }

    if (ok) {
        (void)m.write_u32(lp_old_protect, old_prot);
        m.set_return(1);
    } else {
        // A protection the memory type refuses still stores PAGE_NOACCESS;
        // other rejections leave *lpflOldProtect untouched.
        if (old_prot == PAGE_NOACCESS) {
            (void)m.write_u32(lp_old_protect, old_prot);
        }
        const u32 dedicated = vqm.take_last_protect_error();
        set_last_error(dedicated != 0 ? dedicated
                                      : error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
    }
}

// SIZE_T VirtualQuery(LPCVOID lpAddress,
//                     PMEMORY_BASIC_INFORMATION lpBuffer,
//                     SIZE_T dwLength);
//
// The on-guest MEMORY_BASIC_INFORMATION layout is bitness-dependent:
//   - 32-bit guest: 28 bytes (4-byte pointers, no internal padding)
//   - 64-bit guest: 48 bytes (8-byte pointers + alignment padding)
// We must serialise the right layout based on the calling CPU mode,
// not blindly copy our host-side 48-byte struct.

void impl_VirtualQuery(Marshaler& m, VirtualQueryManager& vqm) {
    const u64 lp_address = m.arg(0);
    const u64 lp_buffer  = m.arg(1);
    const u64 dw_length  = m.arg(2);

    const bool is64 = m.cpu_is_64bit();
    const u64 expected_size = is64 ? 48ULL : 28ULL;

    if (lp_buffer == 0 ||
        !m.memory().is_range_writable(
            static_cast<GuestAddr>(lp_buffer), expected_size,
            m.cpu_is_64bit())) {
        set_last_error(error_codes::ERROR_NOACCESS);
        m.set_return(0);
        return;
    }
    if (dw_length < expected_size) {
        set_last_error(error_codes::ERROR_BAD_LENGTH);
        m.set_return(0);
        return;
    }

    MemoryBasicInformation info{};
    const u64 maximum_address = is64 ? 0x00007FFF'FFFEFFFFULL : 0xFFFF'FFFFULL;
    const usize written = vqm.virtual_query(lp_address, info, maximum_address);
    if (written == 0) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return;
    }

    const GuestAddr buf = static_cast<GuestAddr>(lp_buffer);
    VoidResult wr = VoidResult::ok();
    if (is64) {
        // 64-bit layout matches our host struct exactly.
        wr = m.write_struct<MemoryBasicInformation>(buf, info);
    } else {
        // 32-bit layout: 7 dwords, no padding.
        //   +0  BaseAddress       (PVOID,  4)
        //   +4  AllocationBase    (PVOID,  4)
        //   +8  AllocationProtect (DWORD,  4)
        //   +12 RegionSize        (SIZE_T, 4)
        //   +16 State             (DWORD,  4)
        //   +20 Protect           (DWORD,  4)
        //   +24 Type              (DWORD,  4)
        wr = m.write_u32(buf + 0,  static_cast<u32>(info.base_address));
        if (wr.is_ok()) wr = m.write_u32(buf + 4,  static_cast<u32>(info.allocation_base));
        if (wr.is_ok()) wr = m.write_u32(buf + 8,  info.allocation_protect);
        if (wr.is_ok()) wr = m.write_u32(buf + 12, static_cast<u32>(info.region_size));
        if (wr.is_ok()) wr = m.write_u32(buf + 16, info.state);
        if (wr.is_ok()) wr = m.write_u32(buf + 20, info.protect);
        if (wr.is_ok()) wr = m.write_u32(buf + 24, info.type);
    }
    if (wr.is_err()) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return;
    }

    static const bool log_vquery = std::getenv("VITREO_LOG_VQUERY") != nullptr;
    if (log_vquery) {
        const auto serialized_state = m.read_u32(buf + (is64 ? 32 : 16));
        const u64 rsp = m.cpu() ? m.cpu()->gpr.get64(
                                      static_cast<u8>(cpu::RegIndex::RSP))
                                : 0;
        const auto return_rip = m.read_u64(static_cast<GuestAddr>(rsp));
        std::fprintf(stderr,
            "[vquery] address=0x%llx buffer=0x%llx rsp=0x%llx return=0x%llx -> base=0x%llx size=0x%llx state=0x%x serialized_state=0x%x protect=0x%x type=0x%x\n",
            static_cast<unsigned long long>(lp_address),
            static_cast<unsigned long long>(buf),
            static_cast<unsigned long long>(rsp),
            static_cast<unsigned long long>(return_rip.is_ok()
                                                ? return_rip.value()
                                                : 0),
            static_cast<unsigned long long>(info.base_address),
            static_cast<unsigned long long>(info.region_size), info.state,
            serialized_state.is_ok() ? serialized_state.value() : 0,
            info.protect, info.type);
    }

    // VirtualQuery returns the number of bytes actually written.
    m.set_return(expected_size);
}

// BOOL FlushInstructionCache(HANDLE hProcess,
//                            LPCVOID lpBaseAddress,
//                            SIZE_T dwSize);
//
// learn.microsoft.com/.../nf-processthreadsapi-flushinstructioncache

void impl_FlushInstructionCache(Marshaler& m, VirtualQueryManager& vqm) {
    // hProcess is accepted but Vitreo only hosts one guest address space in
    // this process; the current implementation returns success
    // for the local case on x86.
    (void)m.arg(0);
    const u64 base = m.arg(1);
    const u64 size = m.arg(2);
    const usize flush_size =
        size > static_cast<u64>(std::numeric_limits<usize>::max())
            ? std::numeric_limits<usize>::max()
            : static_cast<usize>(size);
    (void)vqm.flush_instruction_cache(base, flush_size);
    m.set_return(1);
}

void impl_GetWriteWatch(Marshaler& m, VirtualMemoryManager& vmm) {
    // UINT GetWriteWatch(DWORD dwFlags, PVOID lpBaseAddress, SIZE_T dwRegionSize,
    //                    PVOID *lpAddresses, ULONG_PTR *lpdwCount,
    //                    LPDWORD lpdwGranularity)
    const u32 flags = static_cast<u32>(m.arg(0));
    const u64 base = m.arg(1);
    const u64 size = m.arg(2);
    const GuestAddr addresses_ptr = m.arg_ptr(3);
    const GuestAddr count_ptr = m.arg_ptr(4);
    const GuestAddr granularity_ptr = m.arg_ptr(5);

    if (count_ptr == 0 || granularity_ptr == 0) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(1);
        return;
    }

    const bool is_64 = m.cpu_is_64bit();
    u64 count = 0;
    if (is_64) {
        auto r = m.read_u64(count_ptr);
        if (r.is_err()) {
            set_last_error(error_codes::ERROR_INVALID_PARAMETER);
            m.set_return(1);
            return;
        }
        count = r.value();
    } else {
        auto r = m.read_u32(count_ptr);
        if (r.is_err()) {
            set_last_error(error_codes::ERROR_INVALID_PARAMETER);
            m.set_return(1);
            return;
        }
        count = r.value();
    }

    u32 granularity = 0;
    std::vector<u64> pages;
    const u32 status = vmm.get_write_watch(base, size, count, granularity, pages,
                                           flags);
    if (status != 0) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(status);
        return;
    }
    // Under-reporting here is the one unsafe direction: a collector that is
    // told "no pages written" frees objects whose only surviving references
    // were stored since the last reset. Report the shape of every answer so a
    // GC-flavoured crash can be attributed or ruled out without guessing.
    static const bool log_ww = std::getenv("VITREO_LOG_WRITEWATCH") != nullptr;
    if (log_ww) {
        static thread_local u32 calls = 0;
        static thread_local u32 empty = 0;
        ++calls;
        if (pages.empty()) ++empty;
        std::fprintf(stderr,
            "[writewatch] base=0x%llx size=0x%llx flags=0x%x -> %zu pages "
            "(call %u, empty %u)\n",
            static_cast<unsigned long long>(base),
            static_cast<unsigned long long>(size), flags,
            pages.size(), calls, empty);
    }
    if (!pages.empty() && addresses_ptr == 0) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(1);
        return;
    }
    for (usize i = 0; i < pages.size(); ++i) {
        if (is_64) {
            (void)m.write_u64(addresses_ptr + i * sizeof(u64), pages[i]);
        } else {
            (void)m.write_u32(addresses_ptr + i * sizeof(u32),
                              static_cast<u32>(pages[i]));
        }
    }

    if (is_64) {
        (void)m.write_u64(count_ptr, count);
    } else {
        (void)m.write_u32(count_ptr, static_cast<u32>(count));
    }
    (void)m.write_u32(granularity_ptr, granularity);
    m.set_return(0);
}

void impl_ResetWriteWatch(Marshaler& m, VirtualMemoryManager& vmm) {
    // UINT ResetWriteWatch(LPVOID lpBaseAddress, SIZE_T dwRegionSize)
    const u64 base = m.arg(0);
    const u64 size = m.arg(1);
    const u32 status = vmm.reset_write_watch(base, size);
    if (status != 0) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
    }
    m.set_return(status);
}

// =============================================================================
// DLL Export Table
// =============================================================================

DllExports make_virtual_protect_exports(VirtualMemoryManager& vmm,
                                        VirtualQueryManager& vqm) {
    DllExports exports;
    exports.entries.reserve(8);

    // VirtualAlloc
    exports.entries.push_back(ExportEntry{
        "VirtualAlloc", 0,
        [&vmm](Marshaler& m) { impl_VirtualAlloc(m, vmm); },
        16
    });

    exports.entries.push_back(ExportEntry{
        "VirtualAlloc2", 0,
        [&vmm](Marshaler& m) { impl_VirtualAlloc2(m, vmm); },
        28
    });

    // VirtualFree
    exports.entries.push_back(ExportEntry{
        "VirtualFree", 0,
        [&vmm](Marshaler& m) { impl_VirtualFree(m, vmm); },
        12
    });

    // VirtualProtect
    exports.entries.push_back(ExportEntry{
        "VirtualProtect", 0,
        [&vqm](Marshaler& m) { impl_VirtualProtect(m, vqm); },
        16
    });

    // VirtualQuery
    exports.entries.push_back(ExportEntry{
        "VirtualQuery", 0,
        [&vqm](Marshaler& m) { impl_VirtualQuery(m, vqm); },
        12
    });

    // FlushInstructionCache
    exports.entries.push_back(ExportEntry{
        "FlushInstructionCache", 0,
        [&vqm](Marshaler& m) { impl_FlushInstructionCache(m, vqm); },
        12
    });

    // GetWriteWatch (6 args, stdcall_bytes=24)
    exports.entries.push_back(ExportEntry{
        "GetWriteWatch", 0,
        [&vmm](Marshaler& m) { impl_GetWriteWatch(m, vmm); },
        24
    });

    // ResetWriteWatch (2 args, stdcall_bytes=8)
    exports.entries.push_back(ExportEntry{
        "ResetWriteWatch", 0,
        [&vmm](Marshaler& m) { impl_ResetWriteWatch(m, vmm); },
        8
    });

    return exports;
}

} // namespace vitreo::winapi
