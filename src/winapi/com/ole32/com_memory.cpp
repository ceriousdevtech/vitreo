// SPDX-License-Identifier: LGPL-2.1-only
/// @file com_memory.cpp
/// @brief Task memory (CoTaskMem*), IMalloc and CoGetMalloc, and the HGLOBAL view of
/// a memory stream.

#include "com_core.hpp"
#include "ole_guid_internal.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/com/ole32/task_memory.hpp"
#include "com_core_internal.hpp"

namespace vitreo::winapi {

GuestAddr ComCoreManager::memory_stream_hglobal(GuestAddr stream) {
    // Only CreateStreamOnHGlobal streams have an HGLOBAL; SHCreateMemStream's
    // host-vector streams answer E_INVALIDARG from GetHGlobalFromStream.
    std::lock_guard lock(m_memory_streams_mutex);
    auto it = m_memory_streams.find(stream);
    if (it == m_memory_streams.end() || it->second == nullptr) return 0;
    std::lock_guard backing_lock(it->second->backing->mutex);
    return static_cast<GuestAddr>(it->second->backing->hglobal);
}

u32 ComCoreManager::get_hglobal_from_stream(Marshaler& m) {
    const GuestAddr stream = m.arg_ptr(0);
    const GuestAddr out = m.arg_ptr(1);
    // Measured: a NULL stream is E_INVALIDARG.
    if (stream == 0) return com::E_INVALIDARG;
    if (out == 0) return com::E_INVALIDARG;
    (void)m.write_ptr(out, 0);
    const GuestAddr handle = memory_stream_hglobal(stream);
    if (handle == 0) {
        VITREO_TRACE(
            "[ole32] GetHGlobalFromStream: 0x%llx is not an HGLOBAL-backed "
            "memory stream -> E_INVALIDARG\n",
            static_cast<unsigned long long>(stream));
        return com::E_INVALIDARG;
    }
    return m.write_ptr(out, handle).is_ok() ? com::S_OK : com::E_INVALIDARG;
}

GuestAddr ComCoreManager::co_task_mem_alloc_guest(u64 size) {
    if (m_guest_task_memory == nullptr) {
        VITREO_TRACE(
            "[ole32] CoTaskMemAlloc unavailable: no process task allocator configured\n");
        return 0;
    }
    return m_guest_task_memory->allocate(size);
}

GuestAddr ComCoreManager::ensure_task_malloc_object(Marshaler& m) {
    if (m_task_malloc_object != 0) return m_task_malloc_object;
    if (m_thunks == nullptr || m_guest_task_memory == nullptr) return 0;

    auto require = [&](const char* name) -> GuestAddr {
        auto addr = m_thunks->get_thunk_address("ole32.dll", name);
        return addr.is_ok() ? addr.value() : 0;
    };
    const GuestAddr methods[] = {
        require("Vitreo_IMalloc_QueryInterface"),
        require("Vitreo_IMalloc_AddRef"),
        require("Vitreo_IMalloc_Release"),
        require("Vitreo_IMalloc_Alloc"),
        require("Vitreo_IMalloc_Realloc"),
        require("Vitreo_IMalloc_Free"),
        require("Vitreo_IMalloc_GetSize"),
        require("Vitreo_IMalloc_DidAlloc"),
        require("Vitreo_IMalloc_HeapMinimize"),
    };
    for (GuestAddr method : methods) {
        if (method == 0) return 0;
    }

    const GuestAddr pointer_size = m.cpu_is_64bit() ? 8u : 4u;
    constexpr usize kSlots = 9;
    const GuestAddr bytes =
        pointer_size * (1u + static_cast<GuestAddr>(kSlots));
    const GuestAddr object = co_task_mem_alloc_guest(bytes);
    if (object == 0) return 0;
    const GuestAddr vtable = object + pointer_size;
    if (m.write_ptr(object, vtable).is_err()) {
        co_task_mem_free_guest(object);
        return 0;
    }
    for (usize i = 0; i < kSlots; ++i) {
        if (m.write_ptr(vtable + static_cast<GuestAddr>(i) * pointer_size,
                        methods[i])
                .is_err()) {
            co_task_mem_free_guest(object);
            return 0;
        }
    }
    m_task_malloc_object = object;
    return object;
}

void ComCoreManager::impl_CoGetMalloc(Marshaler& m) {
    const u32 mem_context = static_cast<u32>(m.arg(0));
    const GuestAddr out_ptr = m.arg_ptr(1);
    if (mem_context != MEMCTX_TASK || out_ptr == 0) {
        if (out_ptr != 0) (void)m.write_ptr(out_ptr, 0);
        m.set_return(com::E_INVALIDARG);
        return;
    }
    std::lock_guard lock(m_com_mutex);
    const GuestAddr object = ensure_task_malloc_object(m);
    if (object == 0) {
        (void)m.write_ptr(out_ptr, 0);
        m.set_return(E_OUTOFMEMORY);
        return;
    }
    if (m_task_malloc_refs == 0) m_task_malloc_refs = 1;
    else ++m_task_malloc_refs;
    if (m.write_ptr(out_ptr, object).is_err()) {
        if (m_task_malloc_refs > 1) --m_task_malloc_refs;
        m.set_return(com::E_INVALIDARG);
        return;
    }
    m.set_return(com::S_OK);
}

void ComCoreManager::imalloc_query_interface(Marshaler& m) {
    const GuestAddr object = m.arg_ptr(0);
    const GuestAddr iid_ptr = m.arg_ptr(1);
    const GuestAddr out_ptr = m.arg_ptr(2);
    if (iid_ptr == 0 || out_ptr == 0) {
        m.set_return(com::E_INVALIDARG);
        return;
    }
    const auto iid = m.read_struct<Guid>(iid_ptr);
    if (iid.is_err()) {
        m.set_return(com::E_INVALIDARG);
        return;
    }
    std::lock_guard lock(m_com_mutex);
    if (object == 0 || object != m_task_malloc_object ||
        (!guid_equal(iid.value(), IID_IUNKNOWN) &&
         !guid_equal(iid.value(), IID_IMALLOC))) {
        (void)m.write_ptr(out_ptr, 0);
        m.set_return(E_NOINTERFACE);
        return;
    }
    ++m_task_malloc_refs;
    if (m.write_ptr(out_ptr, object).is_err()) {
        --m_task_malloc_refs;
        m.set_return(com::E_INVALIDARG);
        return;
    }
    m.set_return(com::S_OK);
}

void ComCoreManager::imalloc_add_ref(Marshaler& m) {
    std::lock_guard lock(m_com_mutex);
    if (m.arg_ptr(0) != m_task_malloc_object || m_task_malloc_object == 0) {
        m.set_return(0);
        return;
    }
    m.set_return(++m_task_malloc_refs);
}

void ComCoreManager::imalloc_release(Marshaler& m) {
    std::lock_guard lock(m_com_mutex);
    if (m.arg_ptr(0) != m_task_malloc_object || m_task_malloc_object == 0) {
        m.set_return(0);
        return;
    }
    // Process task allocator is immortal — never drop the last reference.
    if (m_task_malloc_refs > 1) --m_task_malloc_refs;
    m.set_return(m_task_malloc_refs);
}

void ComCoreManager::imalloc_alloc(Marshaler& m) {
    if (m.arg_ptr(0) != m_task_malloc_object) {
        m.set_return(0);
        return;
    }
    m.set_return(co_task_mem_alloc_guest(m.arg(1)));
}

void ComCoreManager::imalloc_realloc(Marshaler& m) {
    if (m.arg_ptr(0) != m_task_malloc_object) {
        m.set_return(0);
        return;
    }
    m.set_return(co_task_mem_realloc_guest(m.arg_ptr(1), m.arg(2)));
}

void ComCoreManager::imalloc_free(Marshaler& m) {
    if (m.arg_ptr(0) == m_task_malloc_object) {
        co_task_mem_free_guest(m.arg_ptr(1));
    }
    m.set_return(0);
}

void ComCoreManager::imalloc_get_size(Marshaler& m) {
    if (m.arg_ptr(0) != m_task_malloc_object ||
        m_guest_task_memory == nullptr) {
        m.set_return(0);
        return;
    }
    const auto size = m_guest_task_memory->allocation_size(m.arg_ptr(1));
    m.set_return(size.value_or(0));
}

void ComCoreManager::imalloc_did_alloc(Marshaler& m) {
    if (m.arg_ptr(0) != m_task_malloc_object ||
        m_guest_task_memory == nullptr) {
        m.set_return(static_cast<u64>(static_cast<u32>(-1)));
        return;
    }
    const GuestAddr block = m.arg_ptr(1);
    if (block == 0) {
        m.set_return(0);
        return;
    }
    m.set_return(m_guest_task_memory->owns(block) ? 1u : 0u);
}

void ComCoreManager::imalloc_heap_minimize(Marshaler& m) {
    (void)m;
    m.set_return(0);
}

void ComCoreManager::co_task_mem_free_guest(GuestAddr address) {
    if (m_guest_task_memory != nullptr) {
        m_guest_task_memory->free(address);
    }
}

GuestAddr ComCoreManager::co_task_mem_realloc_guest(
    GuestAddr address, u64 size) {
    if (m_guest_task_memory == nullptr) {
        VITREO_TRACE(
            "[ole32] CoTaskMemRealloc unavailable: no process task allocator\n");
        return 0;
    }
    return m_guest_task_memory->reallocate(address, size);
}

void* ComCoreManager::co_task_mem_alloc(usize size) {
    if (size == 0) size = 1;  // Match Windows: alloc(0) returns valid ptr
    void* ptr = std::malloc(size);
    if (ptr) {
        m_task_allocs.insert(ptr);
    }
    return ptr;
}

void ComCoreManager::co_task_mem_free(void* ptr) {
    if (!ptr) return;
    auto it = m_task_allocs.find(ptr);
    if (it != m_task_allocs.end()) {
        m_task_allocs.erase(it);
        std::free(ptr);
    }
    // Silently ignore untracked pointers (e.g., fake PIDLs from SHBrowseForFolder)
}

void* ComCoreManager::co_task_mem_realloc(void* ptr, usize size) {
    if (size == 0) {
        co_task_mem_free(ptr);
        return nullptr;
    }
    // Remove old tracking
    if (ptr) {
        m_task_allocs.erase(ptr);
    }
    void* new_ptr = std::realloc(ptr, size);
    if (new_ptr) {
        m_task_allocs.insert(new_ptr);
    }
    return new_ptr;
}

} // namespace vitreo::winapi
