#pragma once

/// @file memory.hpp
/// @brief VirtualAlloc / VirtualFree / VirtualProtect emulation (WIN32-020)
///
/// Implements Win32 virtual-memory management on top of GuestMemory.

#include <vitreo/common.hpp>

#include <functional>
#include "core/memory/guest_memory.hpp"

#include <map>
#include <mutex>
#include <vector>

namespace vitreo::winapi {

class FileMappingManager;

// =============================================================================
// Win32 Allocation-Type Constants
// =============================================================================

constexpr u32 MEM_COMMIT    = 0x1000;
constexpr u32 MEM_RESERVE   = 0x2000;
constexpr u32 MEM_DECOMMIT  = 0x4000;
constexpr u32 MEM_RELEASE   = 0x8000;
constexpr u32 MEM_TOP_DOWN    = 0x00100000;
constexpr u32 MEM_WRITE_WATCH = 0x00200000;
constexpr u32 MEM_PHYSICAL    = 0x00400000;
constexpr u32 MEM_ROTATE      = 0x00800000;
constexpr u32 MEM_LARGE_PAGES = 0x20000000;
// Standalone allocation types: neither reserves nor commits. Learn states each
// "cannot be used with any other value".
constexpr u32 MEM_RESET      = 0x00080000;
constexpr u32 MEM_RESET_UNDO = 0x01000000;
constexpr u32 WRITE_WATCH_FLAG_RESET = 0x01;

/// Win32 error for an invalid VirtualAlloc flAllocationType / flProtect
/// combination (0 = flags are acceptable). Measured against Windows
/// (tools/oracle_probe/virtual_alloc_flags_probe).
[[nodiscard]] u32 virtual_alloc_flag_error(u32 allocation_type,
                                           u32 protect) noexcept;

// Windows 10 placeholder operations.  MEM_REPLACE_PLACEHOLDER shares its
// numeric value with MEM_DECOMMIT because the value is interpreted in an
// allocation-type context rather than a free-type context.
constexpr u32 MEM_REPLACE_PLACEHOLDER   = 0x00004000;
constexpr u32 MEM_RESERVE_PLACEHOLDER   = 0x00040000;
constexpr u32 MEM_COALESCE_PLACEHOLDERS = 0x00000001;
constexpr u32 MEM_PRESERVE_PLACEHOLDER  = 0x00000002;

// =============================================================================
// Win32 Page-Protection Constants
// =============================================================================

constexpr u32 PAGE_NOACCESS          = 0x01;
constexpr u32 PAGE_READONLY          = 0x02;
constexpr u32 PAGE_READWRITE         = 0x04;
constexpr u32 PAGE_WRITECOPY         = 0x08;
constexpr u32 PAGE_EXECUTE           = 0x10;
constexpr u32 PAGE_EXECUTE_READ      = 0x20;
constexpr u32 PAGE_EXECUTE_READWRITE = 0x40;
constexpr u32 PAGE_EXECUTE_WRITECOPY = 0x80;
constexpr u32 PAGE_GUARD             = 0x100;
constexpr u32 PAGE_NOCACHE           = 0x200;
constexpr u32 PAGE_WRITECOMBINE      = 0x400;
/// Same bit for PAGE_TARGETS_INVALID (VirtualAlloc) and
/// PAGE_TARGETS_NO_UPDATE (VirtualProtect); Learn memory-protection-constants.
constexpr u32 PAGE_TARGETS_INVALID   = 0x40000000;
constexpr u32 PAGE_TARGETS_NO_UPDATE = 0x40000000;

// =============================================================================
// Protection Conversion
// =============================================================================

/// Convert a Win32 PAGE_* constant to our internal MemoryProtection enum.
/// Modifiers (PAGE_GUARD / NOCACHE / WRITECOMBINE / TARGETS_*) are stripped.
[[nodiscard]] MemoryProtection win32_prot_to_native(u32 win32_prot) noexcept;

/// Whether @p win32_prot is a valid VirtualAlloc/VirtualProtect protection,
/// including documented modifiers. Rules measured by
/// tools/oracle_probe/virtual_memory_contract_probe.expected.
[[nodiscard]] bool is_supported_win32_protection(u32 win32_prot) noexcept;

/// Like is_supported_win32_protection, but also accepts PAGE_WRITECOPY and
/// PAGE_EXECUTE_WRITECOPY (with the same modifier rules). VirtualProtect
/// accepts the WRITECOPY forms on MEM_IMAGE pages and mapped views and
/// rejects them on MEM_PRIVATE only once the memory type is known
/// (tools/oracle_probe/writecopy_protect_probe.expected).
[[nodiscard]] bool is_valid_protect_request(u32 win32_prot) noexcept;

/// True when the base protection is PAGE_WRITECOPY or PAGE_EXECUTE_WRITECOPY.
[[nodiscard]] bool is_writecopy_win32_protection(u32 win32_prot) noexcept;

/// True when @p win32_prot is one of the PAGE_EXECUTE* constants.
[[nodiscard]] bool is_execute_win32_protection(u32 win32_prot) noexcept;

/// Protection value stored in allocation/page state after a successful call.
/// VirtualAlloc keeps NOCACHE/WRITECOMBINE; VirtualProtect does not (probe).
/// PAGE_TARGETS_* is never stored (VirtualQuery never reports it).
[[nodiscard]] u32 stored_win32_protection(u32 win32_prot,
                                          bool from_virtual_protect) noexcept;

/// Image (MEM_IMAGE) storage: PAGE_READWRITE → PAGE_WRITECOPY and
/// PAGE_EXECUTE_READWRITE → PAGE_EXECUTE_WRITECOPY until the page is
/// privately written (virtual_memory_contract_probe img protect / img q).
[[nodiscard]] u32 stored_image_win32_protection(u32 win32_prot) noexcept;

/// Convert an internal memory protection to its Win32 PAGE_* equivalent.
[[nodiscard]] u32 native_prot_to_win32(MemoryProtection prot) noexcept;

/// Allocation-owned state returned to VirtualQuery.  This is deliberately
/// independent of MEMORY_BASIC_INFORMATION so the memory manager does not
/// depend on the kernel32 query/thunk layer.
struct VirtualAllocationQuery {
    u64 base_address;
    u64 allocation_base;
    u64 region_size;
    u32 allocation_protect;
    u32 state;
    u32 protect;
};

/// Identity needed to restore a placeholder after a temporary section view is
/// removed. Split placeholders retain the same group identity so only pieces
/// originating from the same reservation can later be coalesced.
struct PlaceholderToken {
    u64 base{0};
    u64 size{0};
    u64 group{0};
};

// =============================================================================
// VirtualMemoryManager
// =============================================================================

/// Manages VirtualAlloc / VirtualFree / VirtualProtect for a single guest
/// process.  Delegates actual mapping work to GuestMemory.
class VirtualMemoryManager {
public:
    /// Construct with a reference to the guest's physical memory manager.
    explicit VirtualMemoryManager(
        GuestMemory& memory,
        u64 maximum_address = 0x00007FFF'FFFEFFFFULL);

    /// Attach the MEM_MAPPED owner so every VirtualProtect-family entry point
    /// updates exact mapped-view protection before falling back to MEM_PRIVATE.
    void set_file_mapping_manager(FileMappingManager* mappings) noexcept {
        m_file_mappings = mappings;
    }

    /// VirtualAlloc – reserve and/or commit virtual memory.
    /// @param address  Requested base address (0 = let the manager choose).
    /// @param size     Region size in bytes.
    /// @param allocation_type  Combination of MEM_COMMIT / MEM_RESERVE.
    /// @param protect  Win32 PAGE_* protection constant.
    /// @return Allocated guest address, or 0 on failure.
    [[nodiscard]] u64 virtual_alloc(u64 address, u64 size,
                                    u32 allocation_type, u32 protect);

    /// VirtualAlloc's MEM_RESET / MEM_RESET_UNDO forms, which neither reserve
    /// nor commit: they only tell the kernel whether the existing contents of
    /// an already committed range still matter.
    /// @return Page-aligned base address, or 0 on failure.
    [[nodiscard]] u64 virtual_alloc_reset(u64 address, u64 size,
                                          u32 allocation_type, u32 protect);

    /// Win32 error from the most recent failed virtual_alloc / reset (0 when
    /// the failure has no dedicated code beyond the thunk's defaults).
    [[nodiscard]] u32 take_last_alloc_error() noexcept {
        const u32 err = m_last_alloc_error;
        m_last_alloc_error = 0;
        return err;
    }

    /// VirtualAlloc2 local-process behavior, including placeholder reserve and
    /// private-allocation replacement. Extended-parameter validation remains
    /// in the thunk because it requires guest structure marshaling.
    [[nodiscard]] u64 virtual_alloc2(u64 address, u64 size,
                                     u32 allocation_type, u32 protect);

    /// Same as virtual_alloc, but marks the region as a Vitreo-internal arena
    /// (a Win32 heap segment) rather than a guest-visible VirtualAlloc region.
    ///
    /// Windows heaps carve their blocks out of segments the heap owns, and a
    /// HeapAlloc pointer is never the base of a caller-visible VirtualAlloc
    /// region — so `VirtualFree(block, 0, MEM_RELEASE)` always fails there.
    /// Vitreo's heap allocates its slabs through this same manager, so without
    /// this distinction a guest that passes a heap pointer to VirtualFree would
    /// unmap the entire slab and take every other live block in it with it.
    [[nodiscard]] u64 virtual_alloc_internal(u64 size, u32 protect);

    /// Release an arena obtained from virtual_alloc_internal. This is the only
    /// way to release such a region; the guest-facing virtual_free refuses to.
    bool virtual_free_internal(u64 address);

    /// VirtualFree – release or decommit virtual memory.
    /// @param address   Base address of the region.
    /// @param size      Size to free (must be 0 for MEM_RELEASE).
    /// @param free_type MEM_RELEASE or MEM_DECOMMIT.
    /// @return true on success.
    bool virtual_free(u64 address, u64 size, u32 free_type);
    bool virtual_free_locked(u64 address, u64 size, u32 free_type);

    /// DiscardVirtualMemory – discard contents without decommitting pages.
    /// The range must be guest-page aligned, committed, and PAGE_READWRITE.
    /// Vitreo represents the contractually undefined post-discard contents as
    /// zero while preserving allocation, commitment, and protection metadata.
    [[nodiscard]] bool discard_virtual_memory(u64 address, u64 size);

    /// Atomically remove an exact placeholder so another allocation owner can
    /// bind a section view at the same address. On success @p token can be
    /// passed to restore_placeholder if binding fails or the view is unmapped
    /// with MEM_PRESERVE_PLACEHOLDER.
    bool take_placeholder(u64 address, u64 size, PlaceholderToken& token);

    /// Recreate a placeholder previously removed by take_placeholder.
    bool restore_placeholder(const PlaceholderToken& token);

    /// VirtualProtect – change page protection.
    /// @param address      Base address of the region.
    /// @param size         Region size in bytes.
    /// @param new_protect  Win32 PAGE_* protection constant.
    /// @param old_protect  Receives the previous protection (may be nullptr).
    /// @return true on success.
    bool virtual_protect(u64 address, u64 size,
                         u32 new_protect, u32* old_protect);

    /// Query the allocation state run containing @p address.
    ///
    /// Returns false when the address is not owned by VirtualAlloc. Mapped
    /// images and file views remain the responsibility of GuestMemory and
    /// their owning subsystems.
    [[nodiscard]] bool virtual_query(u64 address,
                                     VirtualAllocationQuery& info) const;

    /// GetWriteWatch — report pages written in a MEM_WRITE_WATCH region
    /// since allocation or the last reset (Learn: memoryapi GetWriteWatch).
    /// A page is reported when it is committed and either the allocation
    /// dirty bit or the JIT linear page map is set. Reserved pages stay
    /// excluded. Fails when the base is not a MEM_WRITE_WATCH allocation
    /// or @p count_inout cannot hold the set.
    /// @p flags may include WRITE_WATCH_FLAG_RESET.
    /// @param count_inout In: caller array capacity. Out: pages reported.
    /// @return 0 on success, nonzero on failure.
    [[nodiscard]] u32 get_write_watch(u64 base, u64 size,
                                      u64& count_inout,
                                      u32& granularity_out,
                                      std::vector<u64>& pages_out,
                                      u32 flags = 0);

    /// ResetWriteWatch — clear the dirty set and re-arm write traps for a
    /// MEM_WRITE_WATCH region. Succeeds when the region exists and was
    /// allocated with MEM_WRITE_WATCH.
    /// @return 0 on success, nonzero on failure.
    [[nodiscard]] u32 reset_write_watch(u64 base, u64 size);

    /// Probe supplied by the runtime: true when a range lies inside a mapped
    /// section view.
    ///
    /// Pages of a SEC_RESERVE section are RESERVED by the mapping, and
    /// Microsoft Learn (CreateFileMapping, SEC_RESERVE) says they are
    /// committed by calling VirtualAlloc on them. Without this the allocator
    /// only knows its own reservation table and rejects that commit -- which
    /// is how coreclr's W^X code heap died with COR_E_EXECUTIONENGINE.
    using MappedViewProbe = std::function<bool(GuestAddr, u64)>;
    void set_mapped_view_probe(MappedViewProbe probe) {
        m_mapped_view_probe = std::move(probe);
    }

private:
    MappedViewProbe m_mapped_view_probe;
    /// Underlying guest memory manager.
    GuestMemory& m_memory;
    FileMappingManager* m_file_mappings{nullptr};
    u32 m_last_alloc_error{0};

    /// Highest address available to user-mode allocations in this process.
    u64 m_maximum_address;

    /// Protects m_next_alloc and m_allocations for concurrent thread access.
    mutable std::mutex m_mutex;

    /// Next auto-assigned base address (used when caller passes address == 0).
    u64 m_next_alloc = 0x00100000;

    struct PageState {
        bool committed;
        u32 protection;
    };

    /// Tracks each live allocation and its Windows-visible page-state runs.
    /// The map key is an offset from the allocation base. Adjacent equal runs
    /// are merged after every transition.
    struct AllocInfo {
        u64 size;
        u32 allocation_protection;
        std::map<u64, PageState> page_states;
        /// True for arenas Vitreo owns (heap segments). Guest VirtualFree must
        /// not be able to release these; see virtual_alloc_internal.
        bool internal = false;
        /// True when this allocation is currently a placeholder reservation.
        bool placeholder = false;
        /// True for a private allocation that replaced a placeholder and may
        /// be freed back to one with MEM_PRESERVE_PLACEHOLDER.
        bool replaced_placeholder = false;
        /// Origin identity shared by pieces produced from one placeholder.
        u64 placeholder_group = 0;
        /// True when VirtualAlloc included MEM_WRITE_WATCH.
        bool write_watch = false;
        /// Packed dirty bits, one per guest page. Empty when write_watch is
        /// false. Signal handlers update these atomically.
        std::vector<u8> write_watch_dirty;
        /// Packed "armed" bits: host page is PROT_READ waiting for a store.
        std::vector<u8> write_watch_armed;
        /// Packed PAGE_GUARD armed bits (host PROT_NONE). Empty until a page
        /// in this allocation is first given PAGE_GUARD.
        std::vector<u8> guard_page_armed;

        /// Run covering @p offset, or end() if the map cannot answer.
        [[nodiscard]] std::map<u64, PageState>::iterator page_run_at(u64 offset) {
            auto it = page_states.upper_bound(offset);
            if (it == page_states.begin()) return page_states.end();
            --it;
            return it;
        }
        [[nodiscard]] std::map<u64, PageState>::const_iterator
        page_run_at(u64 offset) const {
            auto it = page_states.upper_bound(offset);
            if (it == page_states.begin()) return page_states.end();
            --it;
            return it;
        }
    };
    std::map<u64, AllocInfo> m_allocations;
    u64 m_next_placeholder_group{1};

    using AllocationIterator = std::map<u64, AllocInfo>::iterator;
    using ConstAllocationIterator = std::map<u64, AllocInfo>::const_iterator;

    [[nodiscard]] AllocationIterator
    find_allocation_locked(u64 address);
    [[nodiscard]] ConstAllocationIterator
    find_allocation_locked(u64 address) const;

    static void set_page_state(AllocInfo& allocation,
                               u64 begin_offset,
                               u64 end_offset,
                               PageState state);
    [[nodiscard]] static bool range_is_committed(
        const AllocInfo& allocation,
        u64 begin_offset,
        u64 end_offset);

    void publish_write_watch_locked(u64 base, AllocInfo& allocation);
    void unpublish_write_watch_locked(u64 base, AllocInfo& allocation);
    void arm_write_watch_range_locked(u64 alloc_base, AllocInfo& allocation,
                                      u64 begin, u64 end);
    void ensure_guard_bits_locked(AllocInfo& allocation);
    void sync_guard_arm_locked(u64 alloc_base, AllocInfo& allocation,
                               u64 begin_offset, u64 end_offset,
                               u32 stored_protect);
};

} // namespace vitreo::winapi
