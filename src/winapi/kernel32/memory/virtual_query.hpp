#pragma once

/// @file virtual_query.hpp
/// @brief VirtualProtect / VirtualQuery / FlushInstructionCache thunks (WIN32-036)
///
/// Provides the VirtualQueryManager for querying guest memory region info,
/// FlushInstructionCache (invalidates Vitreo's JIT code cache — the guest
/// instruction-cache analogue), and wires up thunks for VirtualProtect,
/// VirtualQuery, and FlushInstructionCache.

#include <vitreo/common.hpp>
#include "core/memory/guest_memory.hpp"
#include "winapi/kernel32/memory/memory.hpp"
#include "winapi/core/thunk_registry.hpp"

namespace vitreo::winapi {

// Forward declaration
class Marshaler;
class FileMappingManager;
class ModuleManager;

// =============================================================================
// Win32 Memory-Type Constants (not defined in memory.hpp)
// =============================================================================

constexpr u32 MEM_FREE    = 0x10000;
constexpr u32 MEM_PRIVATE = 0x20000;
constexpr u32 MEM_MAPPED  = 0x40000;
constexpr u32 MEM_IMAGE   = 0x1000000;

// =============================================================================
// MEMORY_BASIC_INFORMATION (Windows-compatible layout, 48 bytes)
// =============================================================================

/// Mirrors the Win32 MEMORY_BASIC_INFORMATION structure.
/// All fields are stored in the guest's (little-endian) format.
struct MemoryBasicInformation {
    u64 base_address;       ///< BaseAddress
    u64 allocation_base;    ///< AllocationBase
    u32 allocation_protect; ///< AllocationProtect
    u32 padding1;           ///< alignment padding
    u64 region_size;        ///< RegionSize
    u32 state;              ///< State  (MEM_COMMIT / MEM_RESERVE / MEM_FREE)
    u32 protect;            ///< Protect (PAGE_*)
    u32 type;               ///< Type   (MEM_PRIVATE / MEM_IMAGE / MEM_MAPPED)
    u32 padding2;           ///< alignment padding
};

static_assert(sizeof(MemoryBasicInformation) == 48,
              "MemoryBasicInformation must be 48 bytes (Windows ABI)");

// =============================================================================
// VirtualQueryManager
// =============================================================================

/// Provides VirtualQuery and FlushInstructionCache for a guest process.
///
/// VirtualAlloc-owned ranges are queried through VirtualMemoryManager so
/// reserved, committed, and differently protected page runs remain distinct.
/// Loaded PE images report MEM_IMAGE via an optional ModuleManager.
/// MapViewOfFile views report MEM_MAPPED via an optional FileMappingManager.
///
/// FlushInstructionCache with a non-NULL range notifies the executable-write
/// observer so JIT blocks covering that region are invalidated. A NULL base is
/// an x86 I-cache barrier (Learn: dwSize applies only when the base is set)
/// and does not wipe translations of unchanged bytes. The interpreter always
/// re-decodes.
class VirtualQueryManager {
public:
    /// Construct with the guest mapping table and, when available, the
    /// VirtualAlloc state owner. Optional @p file_mappings publishes
    /// MapViewOfFile provenance so queries report MEM_MAPPED.
    explicit VirtualQueryManager(
        GuestMemory& memory,
        VirtualMemoryManager* virtual_memory = nullptr,
        FileMappingManager* file_mappings = nullptr) noexcept;

    /// Optional late wire (RuntimeContext constructs vqm before linking).
    void set_file_mapping_manager(FileMappingManager* file_mappings) noexcept {
        m_file_mappings = file_mappings;
        if (m_virtual_memory != nullptr) {
            m_virtual_memory->set_file_mapping_manager(file_mappings);
        }
    }

    /// Optional late wire so loaded PE images report MEM_IMAGE (Learn
    /// MEMORY_BASIC_INFORMATION.Type) instead of MEM_PRIVATE.
    void set_module_manager(ModuleManager* modules) noexcept {
        m_modules = modules;
    }

    /// Query memory information for the region containing @p address.
    /// @param address  Guest address to query.
    /// @param info     Receives the region information.
    /// @param maximum_address Highest valid application address for the guest.
    /// @return sizeof(MemoryBasicInformation) on success, 0 on failure.
    [[nodiscard]] usize virtual_query(u64 address,
                                      MemoryBasicInformation& info,
                                      u64 maximum_address = 0x00007FFF'FFFEFFFFULL) const;

    /// Flush the instruction cache for @p base_address / @p size.
    ///
    /// Matches Microsoft Learn: applications call this after generating or
    /// modifying code. @p base_address may be 0 (NULL) to flush the whole
    /// process instruction cache (size ignored). Vitreo maps this onto JIT
    /// block-cache invalidation via GuestMemory::notify_executable_write.
    /// @return Always true on this implementation's x86 path.
    [[nodiscard]] bool flush_instruction_cache(u64 base_address,
                                               usize size) const noexcept;

    /// Route VirtualProtect to the owner of MEM_MAPPED or MEM_PRIVATE state.
    [[nodiscard]] bool virtual_protect(u64 address, usize size,
                                       u32 new_protect, u32* old_protect);

    /// Dedicated Win32 error from the most recent failed virtual_protect.
    [[nodiscard]] u32 take_last_protect_error() noexcept {
        if (m_last_protect_error != 0) {
            const u32 err = m_last_protect_error;
            m_last_protect_error = 0;
            return err;
        }
        return m_virtual_memory != nullptr
                   ? m_virtual_memory->take_last_alloc_error()
                   : 0;
    }

private:
    GuestMemory& m_memory;
    VirtualMemoryManager* m_virtual_memory;
    FileMappingManager* m_file_mappings{nullptr};
    ModuleManager* m_modules{nullptr};
    u32 m_last_protect_error{0};
};

// =============================================================================
// Win32 API Thunk Implementations
// =============================================================================

/// LPVOID WINAPI VirtualAlloc(LPVOID lpAddress, SIZE_T dwSize,
///                            DWORD flAllocationType, DWORD flProtect);
void impl_VirtualAlloc(Marshaler& m, VirtualMemoryManager& vmm);

/// BOOL WINAPI VirtualFree(LPVOID lpAddress, SIZE_T dwSize, DWORD dwFreeType);
void impl_VirtualFree(Marshaler& m, VirtualMemoryManager& vmm);

/// BOOL WINAPI VirtualProtect(LPVOID lpAddress, SIZE_T dwSize,
///                            DWORD flNewProtect, PDWORD lpflOldProtect);
void impl_VirtualProtect(Marshaler& m, VirtualQueryManager& vqm);

/// SIZE_T WINAPI VirtualQuery(LPCVOID lpAddress,
///                            PMEMORY_BASIC_INFORMATION lpBuffer,
///                            SIZE_T dwLength);
void impl_VirtualQuery(Marshaler& m, VirtualQueryManager& vqm);

/// BOOL WINAPI FlushInstructionCache(HANDLE hProcess,
///                                   LPCVOID lpBaseAddress,
///                                   SIZE_T dwSize);
void impl_FlushInstructionCache(Marshaler& m, VirtualQueryManager& vqm);

/// UINT WINAPI GetWriteWatch(DWORD dwFlags, PVOID lpBaseAddress,
///                           SIZE_T dwRegionSize, PVOID *lpAddresses,
///                           ULONG_PTR *lpdwCount, LPDWORD lpdwGranularity);
void impl_GetWriteWatch(Marshaler& m, VirtualMemoryManager& vmm);

/// UINT WINAPI ResetWriteWatch(LPVOID lpBaseAddress, SIZE_T dwRegionSize);
void impl_ResetWriteWatch(Marshaler& m, VirtualMemoryManager& vmm);

// =============================================================================
// DLL Export Table
// =============================================================================

/// Build the export entries for VirtualProtect, VirtualQuery, and
/// FlushInstructionCache.
DllExports make_virtual_protect_exports(VirtualMemoryManager& vmm,
                                        VirtualQueryManager& vqm);

} // namespace vitreo::winapi
