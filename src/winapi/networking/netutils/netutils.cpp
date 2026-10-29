// SPDX-License-Identifier: LGPL-2.1-only
/// @file netutils.cpp
/// @brief netutils.dll network-management buffer API (NetApiBuffer*)

#include "netutils.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/memory/heap.hpp"

namespace vitreo::winapi {

namespace {
constexpr u32 kNerrSuccess = 0;
constexpr u32 kErrorNotEnoughMemory = 8;
constexpr u32 kErrorInvalidParameter = 87;
}  // namespace

// =============================================================================
// NetUtilsManager Implementation
// =============================================================================

u32 NetUtilsManager::net_api_buffer_free(GuestAddr buffer) {
    // NET_API_STATUS NetApiBufferFree(LPVOID Buffer)
    // https://learn.microsoft.com/en-us/windows/win32/api/lmapibuf/nf-lmapibuf-netapibufferfree
    if (buffer == 0) return kNerrSuccess;
    if (!m_heap || !m_heap->heap_free(m_heap->get_process_heap(), 0, buffer)) return kErrorInvalidParameter;
    return kNerrSuccess;
}

void NetUtilsManager::impl_NetApiBufferAllocate(Marshaler& m) {
    // NET_API_STATUS NetApiBufferAllocate(DWORD ByteCount, LPVOID *Buffer)
    // https://learn.microsoft.com/en-us/windows/win32/api/lmapibuf/nf-lmapibuf-netapibufferallocate
    const u32 bytes = m.arg32(0);
    const GuestAddr out = m.arg_ptr(1);
    if (out == 0) { m.set_return(kErrorInvalidParameter); return; }
    const u64 block = m_heap ? m_heap->heap_alloc(m_heap->get_process_heap(), 0, bytes ? bytes : 1) : 0;
    if (block == 0) { (void)m.write_ptr(out, 0); m.set_return(kErrorNotEnoughMemory); return; }
    if (m.write_ptr(out, block).is_err()) {
        (void)m_heap->heap_free(m_heap->get_process_heap(), 0, block);
        m.set_return(kErrorInvalidParameter);
        return;
    }
    m.set_return(kNerrSuccess);
}

void NetUtilsManager::impl_NetApiBufferSize(Marshaler& m) {
    // NET_API_STATUS NetApiBufferSize(LPVOID Buffer, LPDWORD ByteCount)
    // https://learn.microsoft.com/en-us/windows/win32/api/lmapibuf/nf-lmapibuf-netapibuffersize
    const GuestAddr buffer = m.arg_ptr(0);
    const GuestAddr out = m.arg_ptr(1);
    const u64 size = (buffer && m_heap) ? m_heap->heap_size(m_heap->get_process_heap(), 0, buffer) : ~u64{0};
    if (out == 0 || size == ~u64{0}) { m.set_return(kErrorInvalidParameter); return; }
    m.set_return(m.write_u32(out, static_cast<u32>(size)).is_ok() ? kNerrSuccess : kErrorInvalidParameter);
}

void NetUtilsManager::impl_NetApiBufferReallocate(Marshaler& m) {
    // NET_API_STATUS NetApiBufferReallocate(LPVOID OldBuffer, DWORD NewByteCount, LPVOID *NewBuffer)
    // https://learn.microsoft.com/en-us/windows/win32/api/lmapibuf/nf-lmapibuf-netapibufferreallocate
    const GuestAddr old = m.arg_ptr(0);
    const u32 bytes = m.arg32(1);
    const GuestAddr out = m.arg_ptr(2);
    if (out == 0 || !m_heap) { m.set_return(kErrorInvalidParameter); return; }
    const GuestAddr heap = m_heap->get_process_heap();
    u64 block = 0;
    if (old == 0) block = m_heap->heap_alloc(heap, 0, bytes ? bytes : 1);
    else if (m_heap->heap_size(heap, 0, old) == ~u64{0}) { m.set_return(kErrorInvalidParameter); return; }
    else block = m_heap->heap_realloc(heap, 0, old, bytes ? bytes : 1);
    if (block == 0) { m.set_return(kErrorNotEnoughMemory); return; }
    m.set_return(m.write_ptr(out, block).is_ok() ? kNerrSuccess : kErrorInvalidParameter);
}

// =============================================================================
// Export Table Factory
// =============================================================================

DllExports create_netutils_thunks(NetUtilsManager& mgr) {
    DllExports exports;
    exports.entries.reserve(4);

    exports.entries.push_back(ExportEntry{
        "NetApiBufferFree", 1,
        [&mgr](Marshaler& m) { m.set_return(mgr.net_api_buffer_free(m.arg_ptr(0))); },
        4
    });
    exports.entries.push_back(ExportEntry{
        "NetApiBufferAllocate", 0,
        [&mgr](Marshaler& m) { mgr.impl_NetApiBufferAllocate(m); },
        8
    });
    exports.entries.push_back(ExportEntry{
        "NetApiBufferSize", 0,
        [&mgr](Marshaler& m) { mgr.impl_NetApiBufferSize(m); },
        8
    });
    exports.entries.push_back(ExportEntry{
        "NetApiBufferReallocate", 0,
        [&mgr](Marshaler& m) { mgr.impl_NetApiBufferReallocate(m); },
        12
    });

    return exports;
}

} // namespace vitreo::winapi
