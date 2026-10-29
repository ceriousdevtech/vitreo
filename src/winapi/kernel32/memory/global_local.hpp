#pragma once

/// @file global_local.hpp
/// @brief Global and Local memory allocator APIs (WIN32-082)
///
/// Delegates all allocations to HeapManager (process heap) so that returned
/// pointers live in guest address space rather than the host malloc heap.

#include <vitreo/common.hpp>
#include "winapi/core/thunk_registry.hpp"
#include "winapi/kernel32/memory/heap.hpp"

#include <mutex>
#include <unordered_map>

namespace vitreo::winapi {

class Marshaler;

// =============================================================================
// Memory Flags (winbase.h / minwinbase.h, SDK 10.0.26100.0)
// =============================================================================
constexpr u32 GMEM_FIXED       = 0x0000;
constexpr u32 GMEM_MOVEABLE    = 0x0002;
constexpr u32 GMEM_NOCOMPACT   = 0x0010;
constexpr u32 GMEM_NODISCARD   = 0x0020;
constexpr u32 GMEM_ZEROINIT    = 0x0040;
constexpr u32 GMEM_MODIFY      = 0x0080;
constexpr u32 GMEM_DISCARDABLE = 0x0100;
constexpr u32 GMEM_DDESHARE    = 0x2000;
constexpr u32 GMEM_DISCARDED   = 0x4000;
constexpr u32 GMEM_INVALID_HANDLE = 0x8000;
constexpr u32 GMEM_VALID_FLAGS = 0x7F72;
constexpr u32 LMEM_DISCARDABLE = 0x0F00;
constexpr u32 LMEM_VALID_FLAGS = 0x0F72;
constexpr u32 GPTR           = 0x0040;  // GMEM_FIXED | GMEM_ZEROINIT
constexpr u32 GHND           = 0x0042;  // GMEM_MOVEABLE | GMEM_ZEROINIT
constexpr u32 LPTR           = 0x0040;  // LMEM_FIXED | LMEM_ZEROINIT

// =============================================================================
// GlobalLocalManager
// =============================================================================
/// Global* and Local* share one object model, as on Windows (a LocalAlloc'd
/// block can be freed with GlobalFree and vice versa). Measured on Windows 11
/// by tools/oracle_probe/global_local_probe.c (+ .expected):
///  - a FIXED object is a plain process-heap block; any process-heap block is
///    accepted as one (GlobalSize/GlobalFree of a HeapAlloc pointer work);
///  - a MOVEABLE handle is the address of a guest-memory slot that holds the
///    object's data pointer (NULL while discarded); x64 handles end in 8,
///    x86 handles in 4 (the slot is the second half of an 8/16-byte cell).
class GlobalLocalManager {
public:
    /// @param heap_mgr  HeapManager that owns the process heap.
    explicit GlobalLocalManager(HeapManager& heap_mgr);
    ~GlobalLocalManager();

    /// Pointer size of the guest (moveable slot layout). Thunks set it from
    /// the calling CPU; internal callers only create FIXED objects.
    void set_guest_64bit(bool is_64) noexcept { m_guest_64 = is_64; }

    // GlobalAlloc / LocalAlloc (@p local selects the Local flag vocabulary)
    [[nodiscard]] u64 global_alloc(u32 flags, u64 size, bool local = false);

    // GlobalFree / LocalFree — returns 0 on success, handle on failure
    u64 global_free(u64 handle);

    // GlobalReAlloc / LocalReAlloc
    [[nodiscard]] u64 global_realloc(u64 handle, u64 new_size, u32 flags,
                                     bool local = false);

    // GlobalLock — returns pointer
    [[nodiscard]] u64 global_lock(u64 handle);

    // GlobalUnlock (last error follows the measured Windows rules)
    bool global_unlock(u64 handle);

    // GlobalSize
    [[nodiscard]] u64 global_size(u64 handle) const;

    // GlobalFlags / LocalFlags
    [[nodiscard]] u32 global_flags(u64 handle, bool local = false) const;

    // GlobalHandle / LocalHandle
    [[nodiscard]] u64 global_handle(u64 pointer) const;

private:
    struct Moveable {
        u64  cell = 0;          // process-heap block holding the handle slot
        u64  data = 0;          // current data block (0 while discarded)
        u64  size = 0;
        u32  lock_count = 0;
        bool discardable = false;
        bool ddeshare = false;
    };

    [[nodiscard]] bool is_fixed_block(u64 pointer) const;
    void write_slot(u64 handle, u64 data);
    [[nodiscard]] u64 new_moveable(u64 data, u64 size, bool discardable,
                                   bool ddeshare);
    [[nodiscard]] u64 grow_data(u64 data, u64 new_size, u32 heap_flags,
                                bool may_move);

    HeapManager& m_heap;
    mutable std::mutex m_mutex;
    std::unordered_map<u64, Moveable> m_moveable;   // handle -> object
    std::unordered_map<u64, u64> m_data_to_handle;  // data block -> handle
    bool m_guest_64 = true;
};

DllExports create_global_local_thunks(GlobalLocalManager& mgr);

} // namespace vitreo::winapi
