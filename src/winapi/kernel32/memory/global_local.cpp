/// @file global_local.cpp
/// @brief Implementation of Global/Local memory allocator APIs (WIN32-082)
///
/// All allocations delegate to HeapManager's process heap so that returned
/// addresses are valid guest (x86) addresses rather than host malloc pointers.

#include "global_local.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/core/marshaler.hpp"
#include "core/memory/guest_memory.hpp"

#include <cstring>

namespace vitreo::winapi {

// =============================================================================
// GlobalLocalManager Implementation
// =============================================================================

GlobalLocalManager::GlobalLocalManager(HeapManager& heap_mgr)
    : m_heap(heap_mgr) {}

GlobalLocalManager::~GlobalLocalManager() {
    std::lock_guard<std::mutex> lock(m_mutex);
    // Free every outstanding moveable object; FIXED blocks belong to the
    // process heap and go with it.
    GuestAddr ph = m_heap.get_process_heap();
    for (auto& [handle, obj] : m_moveable) {
        if (obj.data != 0) m_heap.heap_free(ph, 0, obj.data);
        m_heap.heap_free(ph, 0, obj.cell);
    }
    m_moveable.clear();
    m_data_to_handle.clear();
}

namespace {
constexpr u32 kErrorDiscarded = 157;   // ERROR_DISCARDED
constexpr u32 kErrorNotLocked = 158;   // ERROR_NOT_LOCKED
}  // namespace

// Caller holds m_mutex.
bool GlobalLocalManager::is_fixed_block(u64 pointer) const {
    if (pointer == 0 || m_data_to_handle.count(pointer) != 0) return false;
    const u64 slot_offset = m_guest_64 ? 8 : 4;
    auto cell = m_moveable.find(pointer + slot_offset);
    if (cell != m_moveable.end() && cell->second.cell == pointer) return false;
    return m_heap.heap_size(m_heap.get_process_heap(), 0, pointer) != ~0ULL;
}

void GlobalLocalManager::write_slot(u64 handle, u64 data) {
    GuestMemory& mem = m_heap.guest_memory();
    if (m_guest_64) {
        (void)mem.write_u64(static_cast<GuestAddr>(handle), data);
    } else {
        (void)mem.write_u32(static_cast<GuestAddr>(handle), static_cast<u32>(data));
    }
}

// Caller holds m_mutex.
u64 GlobalLocalManager::new_moveable(u64 data, u64 size, bool discardable,
                                     bool ddeshare) {
    const u64 ptr = m_guest_64 ? 8 : 4;
    const u64 cell = m_heap.heap_alloc(m_heap.get_process_heap(),
                                       HEAP_ZERO_MEMORY, ptr * 2);
    if (cell == 0) return 0;
    const u64 handle = cell + ptr;
    Moveable obj;
    obj.cell = cell;
    obj.data = data;
    obj.size = data != 0 ? size : 0;
    obj.discardable = discardable;
    obj.ddeshare = ddeshare;
    m_moveable[handle] = obj;
    if (data != 0) m_data_to_handle[data] = handle;
    write_slot(handle, data);
    return handle;
}

u64 GlobalLocalManager::grow_data(u64 data, u64 new_size, u32 heap_flags,
                                  bool may_move) {
    const u32 flags = heap_flags | (may_move ? 0u : HEAP_REALLOC_IN_PLACE_ONLY);
    return m_heap.heap_realloc(m_heap.get_process_heap(), flags, data, new_size);
}

// Learn: winbase/nf-winbase-globalalloc, nf-winbase-localalloc. Invalid flags
// fail with ERROR_INVALID_PARAMETER; GMEM_MOVEABLE with dwBytes == 0 returns
// a handle to an object marked discarded (probe: moveable0 flags=0x4000).
u64 GlobalLocalManager::global_alloc(u32 flags, u64 size, bool local) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if ((flags & ~(local ? LMEM_VALID_FLAGS : GMEM_VALID_FLAGS)) != 0) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        return 0;
    }
    const GuestAddr ph = m_heap.get_process_heap();
    const u32 heap_flags = (flags & GMEM_ZEROINIT) ? HEAP_ZERO_MEMORY : 0;
    if (flags & GMEM_MOVEABLE) {
        const bool discardable =
            (flags & (local ? LMEM_DISCARDABLE : GMEM_DISCARDABLE)) != 0;
        const bool ddeshare = !local && (flags & GMEM_DDESHARE) != 0;
        u64 data = 0;
        if (size != 0) {
            data = m_heap.heap_alloc(ph, heap_flags, size);
            if (data == 0) {
                set_last_error(error_codes::ERROR_NOT_ENOUGH_MEMORY);
                return 0;
            }
        }
        const u64 handle = new_moveable(data, size, discardable, ddeshare);
        if (handle == 0) {
            if (data != 0) m_heap.heap_free(ph, 0, data);
            set_last_error(error_codes::ERROR_NOT_ENOUGH_MEMORY);
            return 0;
        }
        return handle;
    }
    // FIXED: a process-heap block. GlobalAlloc(GMEM_FIXED, 0) reports
    // GlobalSize 1 while LocalAlloc(LMEM_FIXED, 0) reports 0 (zero_fixed).
    const u64 bytes = (size == 0 && !local) ? 1 : size;
    const u64 p = m_heap.heap_alloc(ph, heap_flags, bytes);
    if (p == 0) {
        set_last_error(error_codes::ERROR_NOT_ENOUGH_MEMORY);
        return 0;
    }
    return p;
}

// Learn: "The GlobalFree function will free a locked memory object."
u64 GlobalLocalManager::global_free(u64 handle) {
    if (handle == 0) {
        return 0;  // freeing NULL is a no-op
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    const GuestAddr ph = m_heap.get_process_heap();
    auto it = m_moveable.find(handle);
    if (it != m_moveable.end()) {
        if (it->second.data != 0) {
            m_data_to_handle.erase(it->second.data);
            m_heap.heap_free(ph, 0, it->second.data);
        }
        m_heap.heap_free(ph, 0, it->second.cell);
        m_moveable.erase(it);
        return 0;
    }
    if (is_fixed_block(handle) && m_heap.heap_free(ph, 0, handle)) {
        return 0;
    }
    set_last_error(error_codes::ERROR_INVALID_HANDLE);
    return handle;
}

// Learn: winbase/nf-winbase-globalrealloc (+ LocalReAlloc). Behaviour the
// page leaves open is from global_local_probe.expected.
u64 GlobalLocalManager::global_realloc(u64 handle, u64 new_size, u32 flags,
                                       bool local) {
    std::lock_guard<std::mutex> lock(m_mutex);
    const u32 valid = (local ? LMEM_VALID_FLAGS : GMEM_VALID_FLAGS) | GMEM_MODIFY;
    if ((flags & ~valid) != 0) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        return 0;
    }
    const GuestAddr ph = m_heap.get_process_heap();
    auto it = m_moveable.find(handle);

    if (flags & GMEM_MODIFY) {
        // "modifies the attributes of the memory object only (the dwBytes
        // parameter is ignored)".
        if (it != m_moveable.end()) {
            it->second.discardable =
                (flags & (local ? LMEM_DISCARDABLE : GMEM_DISCARDABLE)) != 0;
            return handle;
        }
        if (!is_fixed_block(handle)) {
            set_last_error(error_codes::ERROR_INVALID_HANDLE);
            return 0;
        }
        // GlobalReAlloc(fixed, 0, GMEM_MODIFY | GMEM_MOVEABLE) converts the
        // block into a moveable object with a new handle and a new data
        // block holding the same bytes; LocalReAlloc leaves it FIXED
        // (probe: modify_fixed_to_moveable vs l_modify_same).
        if (local || !(flags & GMEM_MOVEABLE)) {
            return handle;
        }
        const u64 size = m_heap.heap_size(ph, 0, handle);
        const u64 data = m_heap.heap_alloc(ph, 0, size);
        if (data == 0) {
            set_last_error(error_codes::ERROR_NOT_ENOUGH_MEMORY);
            return 0;
        }
        GuestMemory& mem = m_heap.guest_memory();
        const auto* src = mem.host_ptr(static_cast<GuestAddr>(handle), static_cast<usize>(size));
        auto* dst = mem.host_ptr_mut(static_cast<GuestAddr>(data), static_cast<usize>(size));
        if (size != 0 && (src == nullptr || dst == nullptr)) {
            m_heap.heap_free(ph, 0, data);
            set_last_error(error_codes::ERROR_NOT_ENOUGH_MEMORY);
            return 0;
        }
        if (size != 0) std::memcpy(dst, src, static_cast<usize>(size));
        const u64 h = new_moveable(data, size, (flags & GMEM_DISCARDABLE) != 0, false);
        if (h == 0) {
            m_heap.heap_free(ph, 0, data);
            set_last_error(error_codes::ERROR_NOT_ENOUGH_MEMORY);
            return 0;
        }
        m_heap.heap_free(ph, 0, handle);
        return h;
    }

    // Without GMEM_MODIFY only MOVEABLE / NOCOMPACT / NODISCARD / ZEROINIT
    // (and the inert DDESHARE) are accepted; discard-level bits are invalid
    // (probe: mov_disc32 / l_mov_disc err=0x57).
    if ((flags & LMEM_DISCARDABLE) != 0) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        return 0;
    }
    const u32 heap_flags = (flags & GMEM_ZEROINIT) ? HEAP_ZERO_MEMORY : 0;
    const bool moveable_flag = (flags & GMEM_MOVEABLE) != 0;

    if (it != m_moveable.end()) {
        Moveable& obj = it->second;
        if (new_size == 0) {
            // dwBytes 0 discards an unlocked object when GMEM_MOVEABLE is
            // given; otherwise the call fails and leaves last error alone
            // (realloc0_moveable_noflag / realloc0_locked).
            if (obj.lock_count > 0 || !moveable_flag) return 0;
            if (obj.data != 0) {
                m_data_to_handle.erase(obj.data);
                m_heap.heap_free(ph, 0, obj.data);
            }
            obj.data = 0;
            obj.size = 0;
            write_slot(handle, 0);
            return handle;
        }
        if (obj.data == 0) {
            const u64 data = m_heap.heap_alloc(ph, heap_flags, new_size);
            if (data == 0) {
                set_last_error(error_codes::ERROR_NOT_ENOUGH_MEMORY);
                return 0;
            }
            obj.data = data;
            obj.size = new_size;
            m_data_to_handle[data] = handle;
            write_slot(handle, data);
            return handle;
        }
        // A locked object moves only with GMEM_MOVEABLE; an unlocked one
        // may always move (locked_grow_noflag / unlocked_grow_noflag).
        const bool may_move = obj.lock_count == 0 || moveable_flag;
        const u64 data = grow_data(obj.data, new_size, heap_flags, may_move);
        if (data == 0) {
            set_last_error(error_codes::ERROR_NOT_ENOUGH_MEMORY);
            return 0;
        }
        if (data != obj.data) {
            m_data_to_handle.erase(obj.data);
            m_data_to_handle[data] = handle;
            obj.data = data;
            write_slot(handle, data);
        }
        obj.size = new_size;
        return handle;
    }

    if (!is_fixed_block(handle)) {
        set_last_error(error_codes::ERROR_INVALID_HANDLE);
        return 0;
    }
    // "If the memory is ... a GMEM_FIXED memory block and this flag is not
    // specified, the memory can only be reallocated in place."
    const u64 p = grow_data(handle, new_size, heap_flags, moveable_flag);
    if (p == 0) {
        set_last_error(error_codes::ERROR_NOT_ENOUGH_MEMORY);
        return 0;
    }
    return p;
}

u64 GlobalLocalManager::global_lock(u64 handle) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_moveable.find(handle);
    if (it != m_moveable.end()) {
        if (it->second.data == 0) {
            set_last_error(kErrorDiscarded);
            return 0;
        }
        // The lock count saturates at 255 (probe: lock300 flags=0xff).
        if (it->second.lock_count < 0xFF) ++it->second.lock_count;
        return it->second.data;
    }
    if (is_fixed_block(handle)) {
        return handle;
    }
    set_last_error(error_codes::ERROR_INVALID_HANDLE);
    return 0;
}

// Learn GlobalUnlock: FALSE with NO_ERROR when the count reaches zero;
// ERROR_NOT_LOCKED for an unlocked object. FIXED objects return TRUE and an
// unknown handle returns TRUE with ERROR_INVALID_HANDLE (probe x64).
bool GlobalLocalManager::global_unlock(u64 handle) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_moveable.find(handle);
    if (it != m_moveable.end()) {
        if (it->second.lock_count == 0) {
            set_last_error(kErrorNotLocked);
            return false;
        }
        if (--it->second.lock_count == 0) {
            set_last_error(error_codes::ERROR_SUCCESS);
            return false;
        }
        return true;
    }
    if (is_fixed_block(handle)) {
        return true;
    }
    set_last_error(error_codes::ERROR_INVALID_HANDLE);
    return true;
}

u64 GlobalLocalManager::global_size(u64 handle) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_moveable.find(handle);
    if (it != m_moveable.end()) {
        return it->second.size;   // 0 while discarded, last error untouched
    }
    if (is_fixed_block(handle)) {
        return m_heap.heap_size(m_heap.get_process_heap(), 0, handle);
    }
    set_last_error(error_codes::ERROR_INVALID_HANDLE);
    return 0;
}

u32 GlobalLocalManager::global_flags(u64 handle, bool local) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_moveable.find(handle);
    if (it != m_moveable.end()) {
        const Moveable& obj = it->second;
        u32 f = obj.lock_count & 0xFF;
        if (obj.discardable) f |= local ? LMEM_DISCARDABLE : GMEM_DISCARDABLE;
        if (obj.ddeshare && !local) f |= GMEM_DDESHARE;
        if (obj.data == 0) f |= GMEM_DISCARDED;
        return f;
    }
    if (is_fixed_block(handle)) {
        return 0;
    }
    set_last_error(error_codes::ERROR_INVALID_HANDLE);
    return GMEM_INVALID_HANDLE;
}

// Learn: winbase/nf-winbase-globalhandle — the handle of the object whose
// data pointer is given; a FIXED block is its own handle.
u64 GlobalLocalManager::global_handle(u64 pointer) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto d = m_data_to_handle.find(pointer);
    if (d != m_data_to_handle.end()) {
        return d->second;
    }
    if (m_moveable.count(pointer) == 0 && is_fixed_block(pointer)) {
        return pointer;
    }
    set_last_error(error_codes::ERROR_INVALID_HANDLE);
    return 0;
}

// =============================================================================
// Win32 Thunk Implementations
// =============================================================================

static void impl_GlobalAlloc(Marshaler& m, GlobalLocalManager& mgr, bool local) {
    mgr.set_guest_64bit(m.cpu_is_64bit());
    const u32 flags = m.arg32(0);
    const u64 size = m.arg(1);
    m.set_return(mgr.global_alloc(flags, size, local));
}

static void impl_GlobalFree(Marshaler& m, GlobalLocalManager& mgr) {
    mgr.set_guest_64bit(m.cpu_is_64bit());
    m.set_return(mgr.global_free(m.arg(0)));
}

static void impl_GlobalReAlloc(Marshaler& m, GlobalLocalManager& mgr, bool local) {
    mgr.set_guest_64bit(m.cpu_is_64bit());
    const u64 handle = m.arg(0);
    const u64 size = m.arg(1);
    const u32 flags = m.arg32(2);
    m.set_return(mgr.global_realloc(handle, size, flags, local));
}

static void impl_GlobalLock(Marshaler& m, GlobalLocalManager& mgr) {
    mgr.set_guest_64bit(m.cpu_is_64bit());
    m.set_return(mgr.global_lock(m.arg(0)));
}

static void impl_GlobalUnlock(Marshaler& m, GlobalLocalManager& mgr) {
    // FALSE is the NORMAL result when the lock count reaches zero; the
    // manager then sets NO_ERROR so callers can use the documented idiom
    //     if (!GlobalUnlock(h) && GetLastError() != NO_ERROR) { ...fail... }
    // (last_error_on_success_probe.expected, global_local_probe.expected).
    mgr.set_guest_64bit(m.cpu_is_64bit());
    m.set_return(mgr.global_unlock(m.arg(0)) ? 1u : 0u);
}

static void impl_GlobalSize(Marshaler& m, GlobalLocalManager& mgr) {
    mgr.set_guest_64bit(m.cpu_is_64bit());
    m.set_return(mgr.global_size(m.arg(0)));
}

static void impl_GlobalFlags(Marshaler& m, GlobalLocalManager& mgr, bool local) {
    mgr.set_guest_64bit(m.cpu_is_64bit());
    m.set_return(static_cast<u64>(mgr.global_flags(m.arg(0), local)));
}

static void impl_GlobalHandle(Marshaler& m, GlobalLocalManager& mgr) {
    mgr.set_guest_64bit(m.cpu_is_64bit());
    m.set_return(mgr.global_handle(m.arg(0)));
}

// =============================================================================
// Export Table Factory
// =============================================================================

DllExports create_global_local_thunks(GlobalLocalManager& mgr) {
    DllExports exports;
    exports.entries.reserve(14);

    // Global* APIs
    // GlobalAlloc (2 args, stdcall_bytes=8)
    exports.entries.push_back(ExportEntry{
        "GlobalAlloc", 1,
        [&mgr](Marshaler& m) { impl_GlobalAlloc(m, mgr, false); },
        8
    });
    // GlobalFree (1 arg, stdcall_bytes=4)
    exports.entries.push_back(ExportEntry{
        "GlobalFree", 2,
        [&mgr](Marshaler& m) { impl_GlobalFree(m, mgr); },
        4
    });
    // GlobalReAlloc (3 args, stdcall_bytes=12)
    exports.entries.push_back(ExportEntry{
        "GlobalReAlloc", 3,
        [&mgr](Marshaler& m) { impl_GlobalReAlloc(m, mgr, false); },
        12
    });
    // GlobalLock (1 arg, stdcall_bytes=4)
    exports.entries.push_back(ExportEntry{
        "GlobalLock", 4,
        [&mgr](Marshaler& m) { impl_GlobalLock(m, mgr); },
        4
    });
    // GlobalUnlock (1 arg, stdcall_bytes=4)
    exports.entries.push_back(ExportEntry{
        "GlobalUnlock", 5,
        [&mgr](Marshaler& m) { impl_GlobalUnlock(m, mgr); },
        4
    });
    // GlobalSize (1 arg, stdcall_bytes=4)
    exports.entries.push_back(ExportEntry{
        "GlobalSize", 6,
        [&mgr](Marshaler& m) { impl_GlobalSize(m, mgr); },
        4
    });
    // GlobalFlags (1 arg, stdcall_bytes=4)
    exports.entries.push_back(ExportEntry{
        "GlobalFlags", 7,
        [&mgr](Marshaler& m) { impl_GlobalFlags(m, mgr, false); },
        4
    });
    // GlobalHandle (1 arg, stdcall_bytes=4)
    exports.entries.push_back(ExportEntry{
        "GlobalHandle", 0,
        [&mgr](Marshaler& m) { impl_GlobalHandle(m, mgr); },
        4
    });

    // Local* APIs — same implementation as Global*
    // LocalAlloc (2 args, stdcall_bytes=8)
    exports.entries.push_back(ExportEntry{
        "LocalAlloc", 8,
        [&mgr](Marshaler& m) { impl_GlobalAlloc(m, mgr, true); },
        8
    });
    // LocalFree (1 arg, stdcall_bytes=4)
    exports.entries.push_back(ExportEntry{
        "LocalFree", 9,
        [&mgr](Marshaler& m) { impl_GlobalFree(m, mgr); },
        4
    });
    // LocalReAlloc (3 args, stdcall_bytes=12)
    exports.entries.push_back(ExportEntry{
        "LocalReAlloc", 10,
        [&mgr](Marshaler& m) { impl_GlobalReAlloc(m, mgr, true); },
        12
    });
    // LocalLock (1 arg, stdcall_bytes=4)
    exports.entries.push_back(ExportEntry{
        "LocalLock", 11,
        [&mgr](Marshaler& m) { impl_GlobalLock(m, mgr); },
        4
    });
    // LocalUnlock (1 arg, stdcall_bytes=4)
    exports.entries.push_back(ExportEntry{
        "LocalUnlock", 12,
        [&mgr](Marshaler& m) { impl_GlobalUnlock(m, mgr); },
        4
    });
    // LocalSize (1 arg, stdcall_bytes=4)
    exports.entries.push_back(ExportEntry{
        "LocalSize", 13,
        [&mgr](Marshaler& m) { impl_GlobalSize(m, mgr); },
        4
    });
    // LocalFlags (1 arg, stdcall_bytes=4)
    exports.entries.push_back(ExportEntry{
        "LocalFlags", 14,
        [&mgr](Marshaler& m) { impl_GlobalFlags(m, mgr, true); },
        4
    });
    // LocalHandle (1 arg, stdcall_bytes=4)
    exports.entries.push_back(ExportEntry{
        "LocalHandle", 0,
        [&mgr](Marshaler& m) { impl_GlobalHandle(m, mgr); },
        4
    });

    return exports;
}

} // namespace vitreo::winapi
