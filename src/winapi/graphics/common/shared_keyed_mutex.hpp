// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file shared_keyed_mutex.hpp
/// @brief IDXGIKeyedMutex state of one shared resource, shared by every
///        opening of it in every process.
///
/// Learn IDXGIKeyedMutex::AcquireSync: the surface starts unowned and only
/// key 0 acquires it; afterwards an acquire succeeds only with the key the
/// last owner released with, stalls otherwise, and is not recursive.
/// WAIT_ABANDONED means "the shared surface and keyed mutex are no longer in
/// a consistent state". Measured on Windows 11 (d3d11_share_probe,
/// dxgi_a11_contracts_probe): a second AcquireSync by the owner is
/// DXGI_ERROR_INVALID_CALL, as is a ReleaseSync by anyone else; an opening
/// destroyed while it owns the mutex -- or a process that exits owning it --
/// abandons it, and from then on EVERY AcquireSync, whatever its key, answers
/// WAIT_ABANDONED at once and takes no ownership.
///
/// The state lives in POSIX shared memory named after the storage (its
/// shared id and the creator's nonce), so openings in other Vitreo host
/// processes exclude each other exactly like openings in one process.
/// Waiters park on a shared address (os_sync_wait_on_address).

#include <vitreo/common.hpp>

#include <memory>

namespace vitreo::ipc {
class SharedMemory;
}

namespace vitreo::graphics {
class GpuCompletion;
}

namespace vitreo::winapi {

class SharedKeyedMutex
    : public std::enable_shared_from_this<SharedKeyedMutex> {
public:
    struct Block;

    /// The state of shared storage (@p surface_id, @p nonce); every process
    /// attaching the same pair sees the same state. Null on failure.
    [[nodiscard]] static std::shared_ptr<SharedKeyedMutex> attach(
        u32 surface_id, u64 nonce);
    /// A state only this process can reach (a resource without shared
    /// storage).
    [[nodiscard]] static std::shared_ptr<SharedKeyedMutex> create_local();

    ~SharedKeyedMutex();
    SharedKeyedMutex(const SharedKeyedMutex&) = delete;
    SharedKeyedMutex& operator=(const SharedKeyedMutex&) = delete;

    enum class Result : u8 { Acquired, AlreadyOwner, Timeout, Abandoned };

    /// AcquireSync(@p key, @p timeout_ms) for the opening @p opening (unique
    /// in this process). 0xFFFFFFFF is INFINITE.
    [[nodiscard]] Result acquire(u32 opening, u64 key, u32 timeout_ms);
    /// ReleaseSync(@p key). False when @p opening does not own the mutex.
    /// @p pending is the owner's submitted GPU work the next owner must see
    /// (null: none outstanding). Openings in this process are ordered after
    /// it on the one backend queue and acquire at once; an opening in
    /// another process -- whose queue is not ordered with this one --
    /// acquires only once it has completed (waited for on a worker thread,
    /// so the releasing thread does not stall).
    [[nodiscard]] bool release(
        u32 opening, u64 key,
        std::shared_ptr<graphics::GpuCompletion> pending = nullptr);
    /// The opening is being destroyed: if it owns the mutex, abandon it.
    void opening_destroyed(u32 opening);
    /// Whether another process can reach this state (it lives in shared
    /// memory): a release must then publish GPU completion across processes.
    [[nodiscard]] bool cross_process() const noexcept {
        return m_shm != nullptr;
    }

private:
    SharedKeyedMutex() = default;
    [[nodiscard]] u64 token(u32 opening) const;
    /// The worker's half of release(): @p pending has completed.
    void publish_completion(u64 release_sequence);

    std::shared_ptr<ipc::SharedMemory> m_shm;
    void* m_map{nullptr};
    usize m_map_len{0};
    std::unique_ptr<Block> m_local;
    Block* m_block{nullptr};
};

}  // namespace vitreo::winapi
