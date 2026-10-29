// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file metal_buffer_pool.hpp
/// @brief Shared slab storage for small constant buffers.

#include <vitreo/common.hpp>

#include <array>
#include <deque>
#include <mutex>
#include <vector>

namespace vitreo::graphics {

/// One slot of a MetalConstantBufferPool slab.
struct PooledSlot {
    void* buffer{nullptr};  ///< the slab's id<MTLBuffer>, owned by the pool
    usize offset{0};        ///< the slot's byte offset in the slab
    u8* contents{nullptr};  ///< the slot's CPU address
    u32 size_class{0};      ///< slot bytes = kMinBytes << size_class
    u32 index{0};           ///< the slab's index in the pool
};

/// Shared-storage slabs for small constant buffers.
///
/// A D3D11 renderer creates and releases thousands of small constant buffers
/// a second, and each was an MTLBuffer of its own: an IOKit allocation on
/// creation and a deallocation on release, on the threads that build the
/// frame. A buffer of at most kMaxBytes takes a
/// slot of a 1 MiB shared slab instead. Slots are powers of two from
/// kMinBytes, so every slot offset is a multiple of 256: the constant-buffer
/// binding alignment on every Metal GPU family.
///
/// A released slot is reused only once MetalSubmitQueue::completed_generation
/// reaches the generation given at release, so no command list that may still
/// read it sees the next owner's bytes. Slabs live for the process. Buffers
/// are created on any thread (D3D11 CreateBuffer runs outside the device
/// lock), so all state is behind one mutex.
class MetalConstantBufferPool {
public:
    static MetalConstantBufferPool& instance();

    static constexpr usize kMinBytes = 256;
    static constexpr usize kMaxBytes = 65536;
    static constexpr usize kSlabBytes = usize{1} << 20;

    /// A slot of at least @p bytes (1..kMaxBytes) in a slab of @p device
    /// (id<MTLDevice> as void*). Its contents are unspecified. False, with
    /// @p out untouched, for any other size, for a device other than the one
    /// the pool's slabs belong to, or when a slab cannot be allocated.
    [[nodiscard]] bool acquire(void* device, usize bytes, PooledSlot& out);

    /// Give @p slot back. It is reused once completed_generation() is at
    /// least @p retire_generation.
    void release(const PooledSlot& slot, u64 retire_generation);

    MetalConstantBufferPool(const MetalConstantBufferPool&) = delete;
    MetalConstantBufferPool& operator=(const MetalConstantBufferPool&) = delete;

private:
    MetalConstantBufferPool() = default;

    static constexpr usize kClasses = 9;  // 256 B .. 64 KiB

    struct Retiring {
        PooledSlot slot;
        u64 generation{0};
    };
    struct SizeClass {
        /// Reusable now. Most recently retired last, and taken from the back.
        std::vector<PooledSlot> free;
        /// Released, in release order, until their generation completes.
        std::deque<Retiring> retiring;
        /// The slab still being carved into fresh slots, if any.
        bool carving{false};
        u32 carve_slab{0};
        usize carve_next{0};
    };

    std::mutex m_mu;
    void* m_device{nullptr};       ///< id<MTLDevice> the slabs belong to
    std::vector<void*> m_slabs;    ///< retained id<MTLBuffer>, never freed
    std::array<SizeClass, kClasses> m_classes{};
};

} // namespace vitreo::graphics
