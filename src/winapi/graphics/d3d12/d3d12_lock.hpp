// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file d3d12_lock.hpp
/// @brief The D3D12 frontend lock: shared for the calls D3D12 lets run side
/// by side, exclusive for the rest.
///
/// Learn ("Design philosophy of command queues and command lists"): "each
/// command list is not free-threaded; however, multiple command lists can be
/// recorded concurrently", and "any thread may submit a command list to any
/// command queue at any time, and the runtime will automatically serialize
/// submission". ID3D12Device is free-threaded; descriptor creation writes
/// only the destination descriptor.
///
/// The lock map (Frontend::method decides the mode per slot, lock_mode()):
///
/// - EXCLUSIVE: every call that creates, destroys, references or changes an
///   object other than the one command list it records into -- creation,
///   AddRef/Release/QueryInterface, private data, Map, ID3D12Fence methods,
///   swap chains, tile mappings, and every manager entry point.
///   Holding it, a thread owns every object, map and pool of the frontend.
/// - RECORDING: command-list recording (every ID3D12GraphicsCommandList
///   slot from GetType on) and ID3D12CommandAllocator::Reset. This has shared
///   lifetime protection, and may also run during an explicit recording-only
///   window held by an exclusive owner. It cannot touch replay/backend state.
/// - SHARED: the descriptor writes
///   (Create*View, CreateSampler*, CopyDescriptors*), the descriptor-heap and
///   GPU-VA getters, ExecuteCommandLists, and queue Signal / Wait while no
///   Present or tile mapping is queued behind a fence on any queue
///   (Frontend::m_exclusive_queue_ops; those run under the exclusive lock,
///   they own swap-chain and tile state). Under it the object table and
///   the CPU/GPU heap and VA maps are read-only (they change only under the
///   exclusive lock), and what each shared call writes has its own owner:
///     * a command list's state and Recording: the one thread recording it
///       (the D3D12 contract above);
///     * CommandAllocator::recording_lists: atomic;
///     * descriptor records of a shader-visible heap: the heap's
///       window_mutex, which the replay's window resolve also takes (two
///       writes or a write and a resolve of one slot are ordered); records
///       of other heaps: the slot's writer (Learn: descriptors may be
///       written from any thread, never one descriptor from two at once);
///     * Device::removed_reason: atomic, its transition under
///       Frontend::m_remove_mutex;
///     * all replay state (ReplayResources, heap-window mirrors, pipeline
///       variants, typed views, acceleration structures, queue pending
///       work, fence submission and timelines): Frontend::m_replay_mutex,
///       held for the whole of a shared ExecuteCommandLists / Signal / Wait
///       (one backend queue: submissions are ordered there anyway). An
///       exclusive holder excludes every replay and does not take it.
///   Resource GetDesc also runs shared: its description is immutable after
///   creation, and the shared hold protects the resource's lifetime.
///   Fence GetCompletedValue runs shared with m_remove_mutex, protecting
///   its backend pointer from replacement and the device-removal transition.
///
/// Readers announce themselves in one of kSlots cache-line counters (no
/// shared cache line between recording threads); a writer raises a flag,
/// then waits for every counter to drain. A reader that sees the flag steps
/// back and waits for admission. The writer may explicitly admit Recording
/// callers while retaining its reservation, then closes admission and drains
/// them before resuming frontend mutations. All modes are recursive per thread (a guest
/// callback made under the lock may call back in): a nested acquisition is
/// a depth count, a shared request under an exclusive hold is satisfied by
/// it, and an exclusive request under a shared hold upgrades the hold
/// (counted in upgrades(): no frontend path does it today).

#include <vitreo/common.hpp>

#include <atomic>
#include <condition_variable>
#include <mutex>

namespace vitreo::winapi::d3d12 {

class FrontendRwLock {
public:
    enum class Mode : u8 { None, Recording, Shared, Exclusive };

    FrontendRwLock() = default;
    FrontendRwLock(const FrontendRwLock&) = delete;
    FrontendRwLock& operator=(const FrontendRwLock&) = delete;

    /// Acquire in @p mode (recursively). Returns true when the calling
    /// thread had to wait for another holder.
    bool lock(Mode mode);
    void unlock();

    /// The calling thread's hold: its depth and mode (None at depth 0).
    [[nodiscard]] u32 depth() const;
    [[nodiscard]] Mode mode() const;

    /// Drop the calling thread's hold entirely if it is exactly one deep,
    /// returning its mode (None: not dropped, the hold is deeper or absent).
    Mode release_if_outermost();
    /// Retake a hold release_if_outermost dropped.
    bool reacquire(Mode mode);

    /// An outer exclusive owner may admit independent command-list recording
    /// while retaining its writer reservation. Other readers and all writers
    /// remain excluded. Closing drains admitted recorders before returning.
    bool begin_recording_window();
    void end_recording_window();

    /// Exclusive requests made under a shared hold (see the file comment).
    [[nodiscard]] u64 upgrades() const noexcept {
        return m_upgrades.load(std::memory_order_relaxed);
    }

private:
    static constexpr usize kSlots = 32;
    struct alignas(128) Slot {
        std::atomic<u32> readers{0};
    };

    bool acquire_shared(bool recording);
    bool acquire_exclusive();
    void release_shared();
    void release_exclusive();
    [[nodiscard]] bool drained() const;
    void notify_writer();

    Slot m_slots[kSlots];
    std::atomic<bool> m_writer{false};
    std::atomic<bool> m_recording_window{false};
    std::mutex m_admission_mutex;
    std::condition_variable m_admission_cv;
    void notify_readers();
    /// Held by the exclusive owner for its whole hold.
    std::mutex m_writer_mutex;
    /// The writer's wait for readers to drain.
    std::mutex m_drain_mutex;
    std::condition_variable m_drain_cv;
    std::atomic<u64> m_upgrades{0};
};

}  // namespace vitreo::winapi::d3d12
