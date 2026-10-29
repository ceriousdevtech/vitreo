#pragma once

/// @file task_memory.hpp
/// @brief Process-owned COM task-memory allocator.

#include <vitreo/common.hpp>

#include <optional>

namespace vitreo::winapi {

class HeapManager;

/// Shared guest allocator behind CoTaskMemAlloc/Free/Realloc and COM-style
/// out-parameters returned by other Windows DLLs.
class ComTaskMemoryManager {
public:
    explicit ComTaskMemoryManager(HeapManager& heap_manager) noexcept;

    [[nodiscard]] GuestAddr allocate(u64 size);
    void free(GuestAddr address);
    [[nodiscard]] GuestAddr reallocate(GuestAddr address, u64 size);

    [[nodiscard]] std::optional<u64> allocation_size(
        GuestAddr address) const;
    [[nodiscard]] bool owns(GuestAddr address) const;

private:
    HeapManager& m_heap_manager;
};

} // namespace vitreo::winapi
