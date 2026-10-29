/// @file task_memory.cpp
/// @brief Process-owned COM task-memory allocator.

#include "winapi/com/ole32/task_memory.hpp"

#include "winapi/kernel32/memory/heap.hpp"

namespace vitreo::winapi {

ComTaskMemoryManager::ComTaskMemoryManager(
    HeapManager& heap_manager) noexcept
    : m_heap_manager{heap_manager} {}

GuestAddr ComTaskMemoryManager::allocate(u64 size) {
    const GuestAddr heap = m_heap_manager.get_process_heap();
    if (heap == INVALID_HANDLE) {
        return 0;
    }
    return static_cast<GuestAddr>(
        m_heap_manager.heap_alloc(heap, 0, size == 0 ? 1 : size));
}

void ComTaskMemoryManager::free(GuestAddr address) {
    if (address == 0) {
        return;
    }
    const GuestAddr heap = m_heap_manager.get_process_heap();
    if (heap == INVALID_HANDLE ||
        !m_heap_manager.heap_free(heap, 0, address)) {
        VITREO_TRACE(
            "[ole32] CoTaskMemFree ignored unowned pointer=0x%llx\n",
            static_cast<unsigned long long>(address));
    }
}

GuestAddr ComTaskMemoryManager::reallocate(GuestAddr address, u64 size) {
    if (address == 0) {
        return allocate(size);
    }
    if (size == 0) {
        free(address);
        return 0;
    }

    const GuestAddr heap = m_heap_manager.get_process_heap();
    if (heap == INVALID_HANDLE) {
        return 0;
    }
    return static_cast<GuestAddr>(
        m_heap_manager.heap_realloc(heap, 0, address, size));
}

std::optional<u64> ComTaskMemoryManager::allocation_size(
    GuestAddr address) const {
    if (address == 0) {
        return std::nullopt;
    }
    const GuestAddr heap = m_heap_manager.get_process_heap();
    if (heap == INVALID_HANDLE) {
        return std::nullopt;
    }
    const u64 size = m_heap_manager.heap_size(heap, 0, address);
    if (size == ~u64{0}) {
        return std::nullopt;
    }
    return size;
}

bool ComTaskMemoryManager::owns(GuestAddr address) const {
    return allocation_size(address).has_value();
}

} // namespace vitreo::winapi
