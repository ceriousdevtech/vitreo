#pragma once

/// @file heap.hpp
/// @brief HeapCreate / HeapAlloc / HeapFree emulation (WIN32-021)
///
/// Implements Win32 heap management on top of VirtualMemoryManager.

#include <vitreo/common.hpp>
#include "winapi/kernel32/memory/memory.hpp"
#include "winapi/kernel32/handle.hpp"

#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace vitreo::winapi {

// =============================================================================
// Win32 Heap Constants
// =============================================================================

constexpr u32 HEAP_ZERO_MEMORY           = 0x00000008;
constexpr u32 HEAP_GENERATE_EXCEPTIONS   = 0x00000004;
constexpr u32 HEAP_NO_SERIALIZE          = 0x00000001;
constexpr u32 HEAP_GROWABLE              = 0x00000002;
constexpr u32 HEAP_REALLOC_IN_PLACE_ONLY = 0x00000010;
/// HEAP_CREATE_ENABLE_EXECUTE — Learn heapapi/HeapCreate.
constexpr u32 HEAP_CREATE_ENABLE_EXECUTE = 0x00040000;

/// Largest single HeapAlloc on a non-growable (dwMaximumSize != 0) heap for
/// a 64-bit process. Measured by tools/oracle_probe/heap_contract_probe
/// (fixed_largest_block max64m/max2m = 0xfeff8 on Windows).
constexpr u64 HEAP_FIXED_MAX_BLOCK_X64 = 0x000FEFF8ULL;
/// 32-bit process: Learn "slightly less than 512 KB" (x86 probe pending).
constexpr u64 HEAP_FIXED_MAX_BLOCK_X86 = 0x0007E000ULL;

// =============================================================================
// Heap – single heap instance
// =============================================================================

/// A single Win32 heap backed by VirtualAlloc'd regions.
///
/// Uses the simplest correct strategy: every allocation gets its own
/// VirtualAlloc'd region.  This avoids the complexity of free-list
/// management while remaining accurate enough for emulation.
class Heap {
public:
    /// Construct a heap.
    /// @param vmm          Virtual memory manager backing allocations.
    /// @param guest_memory  Guest memory for data copies (realloc).
    /// @param initial_size  Initial region size hint.
    /// @param options       Win32 heap option flags.
    /// @param max_size      Nonzero → non-growable heap capped at this size.
    Heap(VirtualMemoryManager& vmm, GuestMemory& guest_memory,
         u64 initial_size, u32 options, u64 max_size = 0);

    /// Destructor – frees every outstanding allocation.
    ~Heap();

    /// No copy / move (contains references).
    Heap(const Heap&) = delete;
    Heap& operator=(const Heap&) = delete;
    Heap(Heap&&) = delete;
    Heap& operator=(Heap&&) = delete;

    /// Allocate memory from this heap.
    /// @param size  Number of bytes to allocate (may be 0 → 1 byte).
    /// @param flags Win32 allocation flags (e.g. HEAP_ZERO_MEMORY).
    /// @return Guest address, or 0 on failure.
    [[nodiscard]] u64 alloc(u64 size, u32 flags);

    /// Free a previously allocated block.
    /// @param address Guest address returned by alloc().
    /// @return true on success, false if address not tracked.
    bool free(u64 address);

    /// Re-allocate a block.
    /// @param address  Guest address of an existing allocation.
    /// @param new_size Desired new size.
    /// @param flags    Win32 flags (HEAP_REALLOC_IN_PLACE_ONLY, etc.).
    /// @return New guest address, or 0 on failure.
    [[nodiscard]] u64 realloc(u64 address, u64 new_size, u32 flags);

    /// Query the size of an allocation.
    /// @param address Guest address returned by alloc().
    /// @return Allocation size, or ~0ULL if address not tracked.
    [[nodiscard]] u64 size(u64 address) const;
    /// Number of 4 MB slabs this heap has taken from the address space.
    /// Diagnostic: a bounded live set must not grow it without bound.
    [[nodiscard]] usize slab_count() const noexcept { return m_slabs.size(); }

    /// HeapCreate flOptions bits (HEAP_GENERATE_EXCEPTIONS, ENABLE_EXECUTE, …).
    [[nodiscard]] u32 options() const noexcept { return m_options; }

private:
    friend class HeapManager;
    bool initialize_identity(u32 flags, bool is_64bit);
    bool refresh_identity(bool is_64bit);
    GuestAddr m_identity = 0;
    u32 m_identity_flags = 0;
    bool m_identity_is_64bit = true;
    struct AllocationInfo {
        u64 requested_size{0};
        u64 committed_size{0};
        bool in_use{false};
        bool from_slab{false};  ///< true if sub-allocated from a slab
        u64 region_base{0};     ///< VirtualAlloc base when !from_slab
    };

    // Slab sub-allocator: blocks below the NT default VirtualMemoryThreshold
    // (RTL_HEAP_PARAMETERS, 0x7F000) come from a committed heap segment.
    // The first user pointer sits after HEAP / HEAP_SEGMENT bytes.
    static constexpr u64 SLAB_SIZE      = 4 * 1024 * 1024;  // 4 MB per slab
    static constexpr u64 SLAB_THRESHOLD = 0x7F000;           // default VirtualMemoryThreshold
    static constexpr u64 SLAB_SEGMENT_PREFIX = 0x1000;       // first-page segment header
    static constexpr u64 SLAB_ALIGN     = 16;                // 16-byte sub-allocation alignment
    static constexpr u64 LARGE_FREE_THRESHOLD = 1024 * 1024; // >= 1 MB → free immediately, never pool

    struct Slab {
        u64 base;    ///< Guest address (from VirtualAlloc)
        u64 size;    ///< Total committed size
        u64 offset;  ///< Next free byte (bump pointer)
    };

    /// Try bump-allocating @p aligned_size bytes from the current slab,
    /// creating a new slab if needed.  Returns 0 on failure.
    u64 slab_bump_alloc(u64 aligned_size);

    /// Win32 PAGE_* for heap backing: PAGE_EXECUTE_READWRITE when created with
    /// HEAP_CREATE_ENABLE_EXECUTE (heap_contract_probe block_exec_*).
    [[nodiscard]] u32 backing_protect() const noexcept {
        return (m_options & HEAP_CREATE_ENABLE_EXECUTE) != 0
                   ? PAGE_EXECUTE_READWRITE
                   : PAGE_READWRITE;
    }

    VirtualMemoryManager& m_vmm;
    GuestMemory& m_guest_memory;
    u32 m_options;
    /// 0 = growable; nonzero = Learn non-growable dwMaximumSize.
    u64 m_max_size{0};
    /// Bytes reserved/committed toward m_max_size (slabs + large regions).
    u64 m_committed_bytes{0};

    /// Protects m_allocations and m_free_pool for concurrent thread access.
    mutable std::recursive_mutex m_mutex;

    /// Tracks every mapped allocation, including freed blocks that remain
    /// reserved for later reuse.
    std::unordered_map<u64, AllocationInfo> m_allocations;

    /// Free-block pool keyed by committed_size (direct VirtualAlloc blocks).
    std::unordered_map<u64, std::vector<u64>> m_free_pool;

    /// Slab free lists keyed by aligned sub-allocation size.
    std::unordered_map<u64, std::vector<u64>> m_slab_free_lists;

    /// Active slabs (the last one is the current bump-allocation target).
    std::vector<Slab> m_slabs;
};

// =============================================================================
// HeapManager – manages all heaps for a guest process
// =============================================================================

/// Top-level manager for Win32 heap APIs.
///
/// Owns Heap instances and their mapped guest pointer identities, and exposes
/// the full set of Win32 heap functions.
class HeapManager {
public:
    /// Construct.
    /// @param vmm          Virtual memory manager for backing allocations.
    /// @param guest_memory  Guest memory for data copies.
    /// @param handles       Handle table for heap handles.
    HeapManager(VirtualMemoryManager& vmm, GuestMemory& guest_memory,
                HandleTable& handles);

    // -------------------------------------------------------------------------
    // Win32 API surface
    // -------------------------------------------------------------------------

    /// HeapCreate – create a new heap.
    /// @param options      Win32 heap option flags.
    /// @param initial_size Initial committed size (hint).
    /// @param max_size     Maximum heap size (0 = growable).
    /// @return Handle to newly created heap, or INVALID_HANDLE on failure.
    [[nodiscard]] GuestAddr heap_create(u32 options, u64 initial_size, u64 max_size, bool rtl = false);

    /// HeapDestroy – destroy a heap and free all its memory.
    /// @param heap Handle previously returned by heap_create.
    /// @return true on success.
    bool heap_destroy(GuestAddr heap);

    /// HeapAlloc – allocate memory from a heap.
    /// @param heap  Heap handle.
    /// @param flags Win32 allocation flags.
    /// @param size  Number of bytes.
    /// @return Guest address, or 0 on failure.
    [[nodiscard]] u64 heap_alloc(GuestAddr heap, u32 flags, u64 size);
    [[nodiscard]] usize heap_slab_count(GuestAddr heap) const;

    /// flOptions of an existing heap (0 if handle unknown).
    [[nodiscard]] u32 heap_options(GuestAddr heap) const;

    /// True if `heap` is a live Heap handle in this manager.
    [[nodiscard]] bool heap_valid(GuestAddr heap) const;

    /// HeapFree – free a heap allocation.
    /// @param heap    Heap handle.
    /// @param flags   Must be 0 (reserved).
    /// @param address Guest address to free.
    /// @return true on success.
    bool heap_free(GuestAddr heap, u32 flags, u64 address);

    /// HeapReAlloc – resize a heap allocation.
    /// @param heap     Heap handle.
    /// @param flags    Win32 re-allocation flags.
    /// @param address  Guest address of existing allocation.
    /// @param new_size Desired new size.
    /// @return New guest address, or 0 on failure.
    [[nodiscard]] u64 heap_realloc(GuestAddr heap, u32 flags,
                                   u64 address, u64 new_size);

    /// HeapSize – query the size of a heap allocation.
    /// @param heap    Heap handle.
    /// @param flags   Must be 0 (reserved).
    /// @param address Guest address of existing allocation.
    /// @return Size in bytes, or ~0ULL on failure.
    [[nodiscard]] u64 heap_size(GuestAddr heap, u32 flags, u64 address);

    /// Every live heap, the process heap first (GetProcessHeaps).
    [[nodiscard]] std::vector<GuestAddr> heap_handles();

    /// GetProcessHeap – return the default process heap (lazily created).
    /// @return Handle to the process heap.
    [[nodiscard]] GuestAddr get_process_heap();

    /// Bind the guest PEB so GetProcessHeap also writes PEB->ProcessHeap
    /// (x64 +0x30 / x86 +0x18). Call once after TEB/PEB setup.
    void bind_peb(u64 peb_address, bool is_64bit) noexcept;

    /// Guest memory the heaps live in (Global/Local handle slots are written
    /// through it).
    [[nodiscard]] GuestMemory& guest_memory() noexcept { return m_guest_memory; }

private:
    void sync_peb_process_heap_locked();

    VirtualMemoryManager& m_vmm;
    GuestMemory& m_guest_memory;
    HandleTable& m_handles;

    /// Protects m_heaps and m_process_heap for concurrent thread access.
    mutable std::recursive_mutex m_mgr_mutex;

    /// Default process heap handle (lazily created).
    GuestAddr m_process_heap = 0;

    /// Guest PEB for ProcessHeap sync (0 = unbound).
    u64 m_peb_address = 0;
    bool m_peb_is_64bit = true;

    /// Heap identities are mapped user-mode pointers, not kernel handles.
    /// This map owns both each allocation arena and its projected heap header.
    std::unordered_map<GuestAddr, std::unique_ptr<Heap>> m_heaps;
};

} // namespace vitreo::winapi
