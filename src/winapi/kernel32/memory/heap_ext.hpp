#pragma once

/// @file heap_ext.hpp
/// @brief Extended heap operations (WIN32-061)

#include <vitreo/common.hpp>
#include "winapi/kernel32/memory/heap.hpp"
#include "winapi/core/thunk_registry.hpp"

namespace vitreo::cpu { struct CpuState; }

namespace vitreo::winapi {

/// Arm CpuState::pending_raise for HeapAlloc/HeapReAlloc OOM when
/// HEAP_GENERATE_EXCEPTIONS is set (STATUS_NO_MEMORY, flags=0x80, p0=size).
void arm_heap_oom_exception(cpu::CpuState& cpu, u64 request_size) noexcept;

/// Extended heap operations
class HeapExtended {
public:
    HeapExtended(HeapManager& heap_mgr, HandleTable& handles);

    /// HeapValidate — validate heap or single allocation
    bool heap_validate(GuestAddr heap, u32 flags, u64 address);

    /// HeapCompact — return approximate largest free block
    u64 heap_compact(GuestAddr heap, u32 flags);

    /// GetProcessHeaps — fill array with heap handles, return count
    u32 get_process_heaps(u32 count, GuestAddr* heaps_out);

private:
    HeapManager& m_heap_mgr;
    HandleTable& m_handles;
};

/// Create heap-extended thunks for kernel32
DllExports create_heap_ext_thunks(HeapExtended& ext, HeapManager& mgr);

/// ntdll's heap: RtlCreateHeap, RtlDestroyHeap, RtlAllocateHeap,
/// RtlReAllocateHeap, RtlSizeHeap, RtlFreeHeap over the same heaps as
/// kernel32 (whose HeapAlloc / HeapReAlloc / HeapSize forward to them).
DllExports create_rtl_heap_thunks(HeapExtended& ext, HeapManager& mgr);

} // namespace vitreo::winapi
