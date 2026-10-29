// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file metal_buffer.hpp
/// @brief Metal GPU buffer wrapper

#include <vitreo/common.hpp>
#include "graphics/contracts/buffer_write.hpp"
#include "graphics/metal/metal_buffer_pool.hpp"
#include <os/lock.h>
#include <atomic>
#include <vector>

namespace vitreo::graphics {

enum class BufferUsage : u8 {
    Vertex,
    Index,
    Uniform,
    Storage,
    Staging,
    Indirect,
};

/// Call after each Present. Storage the GPU has retired is reclaimed when an
/// allocation needs it, not here; a Present only releases DISCARD-pool slabs
/// that have stayed unused, so a burst does not hold its memory for good.
void metal_note_presented_buffers();

/// Occupancy of the pool DISCARD renames and ordered uploads draw from.
struct MetalDiscardPoolStats {
    /// Slabs and bytes counted against the pool's bound.
    u32 slabs{0};
    usize bytes{0};
    /// Allocations that had to wait for the GPU to retire work first.
    u64 waits{0};
    /// Slabs of renames too large for the bound's 4x slabs, under their own
    /// (larger) bound and held only while the GPU reads them.
    u32 oversize_slabs{0};
    usize oversize_bytes{0};
    /// Oversize slabs allocated since startup.
    u64 oversize_created{0};
};
[[nodiscard]] MetalDiscardPoolStats metal_discard_pool_stats();
/// CPU writes that landed in storage the GPU may still read without the
/// application's NO_OVERWRITE promise covering them.
[[nodiscard]] u64 metal_buffer_hazard_writes();
/// Test seam, never called by the runtime: run the pool's reclaim of the slab
/// holding @p storage (an id<MTLBuffer>) as an allocation needing it would,
/// and if it succeeds overwrite the slab as its next owner would. False when
/// work may still read it or a holder has pinned it.
[[nodiscard]] bool metal_discard_pool_reclaim_for_test(void* storage);
/// Test seam, never called by the runtime: leave the slab holding @p storage
/// as a reclaim that saw a pin and backed off leaves it (its epoch moved,
/// nothing reset).
[[nodiscard]] bool metal_discard_pool_abandon_claim_for_test(void* storage);
/// Test seam, never called by the runtime: while @p fail, every pool
/// allocation fails as a refused Metal allocation would, so the paths that
/// cannot rename are reachable.
void metal_discard_pool_fail_allocations_for_test(bool fail);

class MetalBuffer {
public:
    /// Create a buffer. device is id<MTLDevice> as void*.
    MetalBuffer(void* device, usize size, BufferUsage usage,
                const void* initial_data = nullptr);
    /// A buffer whose storage is @p external (an id<MTLBuffer>, retained)
    /// that other processes map too -- a shared resource's store. Its
    /// storage is never renamed and every CPU update is a queued blit, so a
    /// write lands where every process's GPU reads.
    MetalBuffer(void* device, void* external, usize size, BufferUsage usage,
                const void* initial_data = nullptr);
    ~MetalBuffer();

    /// The storage to bind now (id<MTLBuffer>), at gpu_offset(). A published
    /// pool slice is pinned against reclaim for the list being recorded; call
    /// it before note_gpu_bind() and gpu_offset() for one bind.
    void* handle();
    /// Where the published storage starts in handle(). Published after the
    /// offset (release), so a handle() that saw new storage sees its offset.
    usize gpu_offset() const {
        return m_offset.load(std::memory_order_acquire);
    }
    /// The storage is a slot of MetalConstantBufferPool's shared slabs, at
    /// m_owned_offset within m_owned, rather than an MTLBuffer of its own.
    bool pooled() const { return m_pooled; }
    /// Storage exists, i.e. handle() is non-null (a DISCARD slice is only
    /// ever handed out over existing owned storage).
    bool has_storage() const { return m_owned != nullptr; }
    void note_gpu_bind() const;
    /// delta-record (delta_record.hpp): note_gpu_bind() for recording
    /// generation @p gen, skipped when this buffer was already stamped for
    /// @p gen on the storage published now. The stamp is a high-water mark
    /// of generations, so a second stamp for the same generation and
    /// storage changes nothing; the mark lives in the buffer itself, so a
    /// new buffer in a recycled slot or address is always stamped.
    void note_gpu_bind_once(u64 gen) const;
    /// Tests only: the highest generation stamped on the storage published
    /// now (storage_gen()).
    [[nodiscard]] u64 storage_gen_for_test() const noexcept {
        return storage_gen();
    }
    usize size() const   { return m_size; }
    BufferUsage usage() const { return m_usage; }

    /// The published storage's bytes, for reading what the GPU left.
    void* map();
    /// Replace @p length bytes at @p offset; every other byte keeps its value.
    /// @p mode is the API's ordering contract (BufferWrite): NoOverwrite
    /// writes the current storage; Ordered and Discard never write storage
    /// that work already recorded may still read -- they rename into the
    /// DISCARD pool, queue a copy, or wait for the GPU.
    void update(const void* data, usize offset, usize length,
                BufferWrite mode = BufferWrite::Ordered);

    /// The GPU writes this buffer (UAV or stream-output binding). Its storage
    /// is then never renamed into the DISCARD pool: a renamed slice is rebuilt
    /// from the CPU mirror when it retires, and the mirror never sees what
    /// the GPU wrote. Updates while it is in use go through the queue instead.
    void set_gpu_written(bool gpu_written) { m_gpu_written = gpu_written; }
    /// Call before recording any GPU write into this buffer (a copy or clear
    /// destination). Moves storage out of a pool slice first, on the GPU
    /// timeline, and marks the contents as the GPU's until that work retires.
    void note_gpu_write();

    MetalBuffer(const MetalBuffer&) = delete;
    MetalBuffer& operator=(const MetalBuffer&) = delete;

private:
    [[nodiscard]] void* alloc_mtl_buffer(const void* initial_data) const;
    [[nodiscard]] void* published() const noexcept {
        return m_buffer.load(std::memory_order_acquire);
    }
    /// Highest generation that may read the storage published now.
    [[nodiscard]] u64 storage_gen() const noexcept {
        return published() == m_owned
                   ? m_owned_gen.load(std::memory_order_relaxed)
                   : m_slice_gen.load(std::memory_order_relaxed);
    }
    /// Make @p buffer at @p offset the published storage: the offset first,
    /// then the buffer with release.
    void publish(void* buffer, usize offset) noexcept {
        m_offset.store(offset, std::memory_order_release);
        m_buffer.store(buffer, std::memory_order_release);
    }
    // The *_locked members run under m_mu.
    void* handle_locked();
    void note_gpu_write_locked();
    void cpu_update(const void* data, usize offset, usize length,
                    BufferWrite mode, u64 done);
    /// Leave a published pool slice: move back to the owned storage once the
    /// work that read it before the rename has retired (@p done), or else
    /// pin the slice's slab against reclaim for the list being recorded.
    /// @p may_move: false keeps a live slice even when the owned storage is
    /// free -- a NO_OVERWRITE write must not move storage another thread may
    /// be binding. Storage moves without may_move only when the slice's slab
    /// was really reclaimed, which no pinned bind lets happen.
    /// @p rewrite_all: the caller is about to write every byte, so the CPU
    /// mirror need not be copied in first. @p carry_bind: a bind may already
    /// have tagged the slice for work that will use the storage returned now.
    void settle_slice(u64 done, bool may_move, bool rewrite_all,
                      bool carry_bind);
    void move_to_owned(bool rewrite_all, bool carry_bind);
    /// Publish fresh pool storage holding the current contents. False when
    /// the pool cannot supply it.
    [[nodiscard]] bool rename_storage(bool whole);
    /// Block until no submitted work reads the current storage.
    void wait_for_storage();
    void ordered_update(const void* data, usize offset, usize length);

    /// Guards which storage is published, the mirror and the CPU writes into
    /// them. Binds read the published storage without it: a frontend renames
    /// only while nothing else binds this buffer (D3D11: one API lock; D3D9:
    /// Unlock drains its command stream first), and a NO_OVERWRITE write on
    /// another thread moves storage only off a slab that was reclaimed. An
    /// os_unfair_lock: no condition waits, and the uncontended cost is a pair
    /// of atomics.
    mutable os_unfair_lock m_mu = OS_UNFAIR_LOCK_INIT;
    void* m_device = nullptr;          // id<MTLDevice>
    void* m_owned = nullptr;           // stable id<MTLBuffer> (this object)
    /// Where this buffer's own storage starts in m_owned: 0, or the pool
    /// slot's offset in its slab. Every return to owned storage uses it.
    usize m_owned_offset{0};
    bool m_pooled{false};
    PooledSlot m_slot{};
    /// The published storage (id<MTLBuffer>) and its offset: see publish().
    std::atomic<void*> m_buffer{nullptr};
    std::atomic<usize> m_offset{0};
    /// Highest submit generation that may read (or write) the owned storage.
    /// Frozen while a pool slice is published: nothing binds the owned
    /// storage then, so it retires within the GPU's latency. Atomic: binds
    /// on the D3D9 worker raise it while the API thread reads it.
    mutable std::atomic<u64> m_owned_gen{0};
    /// Highest generation that bound the published pool slice (0: none yet).
    mutable std::atomic<u64> m_slice_gen{0};
    /// The recording generation the published slice was last pinned for
    /// (0: none). While it is still the recording and the owned storage is
    /// still read, handle() needs neither the lock nor another pin.
    std::atomic<u64> m_pin_recording{0};
    /// note_gpu_bind_once: the generation and storage (buffer and offset:
    /// two slices of one pool slab share a buffer) last stamped. Written
    /// storage first, generation last (release); a reader that sees a torn
    /// pair only ever skips a stamp whose generation is at most one already
    /// taken, which the high-water stamp covers.
    mutable std::atomic<u64> m_bind_mark_gen{0};
    mutable std::atomic<void*> m_bind_mark_storage{nullptr};
    mutable std::atomic<usize> m_bind_mark_offset{0};
    /// The DISCARD-pool slab the published slice lives in, and that slab's
    /// reclaim epoch and reset count when the slice was handed out. A slice
    /// is borrowed: the slab is reclaimed once every generation reading it
    /// has retired, and by then m_owned_gen has retired too. handle() pins
    /// the slab and then checks the epoch, so a reclaim racing a bind either
    /// sees the pin and backs off or is seen; then the reset count tells an
    /// abandoned reclaim (the slice is still ours) from a real one (leave).
    u32 m_slice_slab{0};
    u64 m_slice_epoch{0};
    u64 m_slice_resets{0};
    bool m_gpu_written{false};
    /// External storage (the shared-store constructor): never renamed.
    bool m_pinned{false};
    /// Recording generation of the last GPU write into this buffer. Until it
    /// completes the CPU-side bytes (storage and mirror) are stale, so neither
    /// a rename's copy of "old contents" nor an in-place write is correct.
    u64 m_gpu_write_gen{0};
    /// CPU mirror of the contents, kept from the first rename on: the source
    /// of a rename's "old contents" and of the restore to owned storage. It
    /// describes the buffer only until the GPU writes it (note_gpu_write
    /// clears it); the next rename rebuilds it from the retired storage.
    std::vector<u8> m_shadow;
    usize m_size = 0;
    BufferUsage m_usage = BufferUsage::Vertex;
};

} // namespace vitreo::graphics
