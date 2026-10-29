// SPDX-License-Identifier: LGPL-2.1-only
/// @file shared_keyed_mutex.cpp
/// @brief IDXGIKeyedMutex state shared across openings and processes.

#include "winapi/graphics/common/shared_keyed_mutex.hpp"

#include "graphics/contracts/backend_device.hpp"
#include "ipc/shared_memory.hpp"

#include <libproc.h>
#include <os/os_sync_wait_on_address.h>
#include <sys/proc.h>
#include <sys/proc_info.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <csignal>
#include <string>
#include <thread>
#include <unistd.h>

namespace vitreo::winapi {

/// The shared state. Zero-filled memory is the initial state: unowned,
/// key 0, not abandoned (Learn AcquireSync: "On initial creation, the
/// surface is unowned ... only a key of 0 will succeed").
struct SharedKeyedMutex::Block {
    std::atomic<u32> lock;
    std::atomic<u32> wake_gen;
    u32 abandoned;
    /// The process whose last release is still completing on its GPU queue
    /// (0: none); only other processes wait for it.
    u32 unsynced_pid;
    u64 key;
    /// (host pid << 32) | opening; 0 when unowned.
    u64 owner;
    /// Releases so far, and which of them unsynced_pid names.
    u64 release_sequence;
    u64 unsynced_sequence;
};

namespace {

constexpr u32 kInfinite = 0xFFFFFFFFu;

bool process_alive(u32 pid) {
    if (pid == 0) return false;
    // Darwin: kill(pid, 0) succeeds for a zombie, and a Vitreo host reaps
    // its workers lazily -- but a process that has exited owns nothing (on
    // Windows its devices are gone and the keyed mutex is abandoned).
    proc_bsdinfo info{};
    const int n = ::proc_pidinfo(static_cast<pid_t>(pid), PROC_PIDTBSDINFO, 0,
                                 &info, static_cast<int>(sizeof(info)));
    if (n == static_cast<int>(sizeof(info))) return info.pbi_status != SZOMB;
    return ::kill(static_cast<pid_t>(pid), 0) == 0 || errno == EPERM;
}

class BlockLock {
public:
    explicit BlockLock(std::atomic<u32>& word) : m_word(word) {
        u32 expected = 0;
        while (!m_word.compare_exchange_weak(expected, 1u,
                                             std::memory_order_acquire)) {
            expected = 0;
            std::this_thread::yield();
        }
    }
    ~BlockLock() { m_word.store(0u, std::memory_order_release); }
    BlockLock(const BlockLock&) = delete;
    BlockLock& operator=(const BlockLock&) = delete;

private:
    std::atomic<u32>& m_word;
};

void wake(SharedKeyedMutex::Block& block) {
    block.wake_gen.fetch_add(1u, std::memory_order_acq_rel);
    (void)os_sync_wake_by_address_all(&block.wake_gen, sizeof(u32),
                                      OS_SYNC_WAKE_BY_ADDRESS_SHARED);
}

/// Waits for released GPU work off the guest threads, then publishes it to
/// other processes. One per host process, never joined (a completion wait
/// must not hold up process exit).
class CompletionWorker {
public:
    using Task = std::function<void()>;
    static CompletionWorker& instance() {
        static CompletionWorker* worker = new CompletionWorker();
        return *worker;
    }
    void post(Task task) {
        {
            std::lock_guard lock(m_mutex);
            m_tasks.push_back(std::move(task));
        }
        m_ready.notify_one();
    }

private:
    CompletionWorker() {
        std::thread([this] {
            for (;;) {
                Task task;
                {
                    std::unique_lock lock(m_mutex);
                    m_ready.wait(lock, [this] { return !m_tasks.empty(); });
                    task = std::move(m_tasks.front());
                    m_tasks.pop_front();
                }
                task();
            }
        }).detach();
    }
    std::mutex m_mutex;
    std::condition_variable m_ready;
    std::deque<Task> m_tasks;
};

}  // namespace

std::shared_ptr<SharedKeyedMutex> SharedKeyedMutex::attach(u32 surface_id,
                                                           u64 nonce) {
    const std::string name = "dxgi-keyed-mutex:" +
                             std::to_string(surface_id) + ":" +
                             std::to_string(nonce);
    auto shm = ipc::SharedMemory::create_or_open(name, sizeof(Block), true);
    if (shm == nullptr) return nullptr;
    void* map = shm->map_view(0, sizeof(Block));
    if (map == nullptr) return nullptr;
    auto mutex = std::shared_ptr<SharedKeyedMutex>(new SharedKeyedMutex());
    mutex->m_shm = std::move(shm);
    mutex->m_map = map;
    mutex->m_map_len = sizeof(Block);
    mutex->m_block = static_cast<Block*>(map);
    return mutex;
}

std::shared_ptr<SharedKeyedMutex> SharedKeyedMutex::create_local() {
    auto mutex = std::shared_ptr<SharedKeyedMutex>(new SharedKeyedMutex());
    mutex->m_local = std::make_unique<Block>();
    mutex->m_block = mutex->m_local.get();
    return mutex;
}

SharedKeyedMutex::~SharedKeyedMutex() {
    if (m_map != nullptr) ipc::SharedMemory::unmap_view(m_map, m_map_len);
}

u64 SharedKeyedMutex::token(u32 opening) const {
    return (static_cast<u64>(static_cast<u32>(::getpid())) << 32) | opening;
}

SharedKeyedMutex::Result SharedKeyedMutex::acquire(u32 opening, u64 key,
                                                   u32 timeout_ms) {
    using clock = std::chrono::steady_clock;
    const u64 me = token(opening);
    const u32 self = static_cast<u32>(::getpid());
    const auto deadline = clock::now() + std::chrono::milliseconds(
                                             timeout_ms == kInfinite
                                                 ? 0u
                                                 : timeout_ms);
    Block& block = *m_block;
    for (;;) {
        const u32 seen = block.wake_gen.load(std::memory_order_acquire);
        {
            BlockLock lock(block.lock);
            if (block.owner == me) return Result::AlreadyOwner;
            if (block.abandoned != 0) return Result::Abandoned;
            // An owner whose process is gone can never release.
            if (block.owner != 0 &&
                !process_alive(static_cast<u32>(block.owner >> 32))) {
                block.owner = 0;
                block.abandoned = 1;
                wake(block);
                return Result::Abandoned;
            }
            // Another process's release is visible here only once its GPU
            // work has completed (release()); a dead releaser has none left.
            if (block.unsynced_sequence != 0 && block.unsynced_pid != self &&
                !process_alive(block.unsynced_pid)) {
                block.unsynced_sequence = 0;
                block.unsynced_pid = 0;
            }
            const bool visible = block.unsynced_sequence == 0 ||
                                 block.unsynced_pid == self;
            if (block.owner == 0 && block.key == key && visible) {
                block.owner = me;
                return Result::Acquired;
            }
        }
        if (timeout_ms == 0) return Result::Timeout;
        u64 park_ns = 50'000'000;  // re-check owner liveness at 20 Hz
        if (timeout_ms != kInfinite) {
            const auto now = clock::now();
            if (now >= deadline) return Result::Timeout;
            park_ns = std::min<u64>(
                park_ns,
                static_cast<u64>(std::chrono::duration_cast<
                                     std::chrono::nanoseconds>(deadline - now)
                                     .count()));
        }
        (void)os_sync_wait_on_address_with_timeout(
            &block.wake_gen, seen, sizeof(u32),
            m_shm != nullptr ? OS_SYNC_WAIT_ON_ADDRESS_SHARED
                             : OS_SYNC_WAIT_ON_ADDRESS_NONE,
            OS_CLOCK_MACH_ABSOLUTE_TIME, park_ns);
    }
}

bool SharedKeyedMutex::release(u32 opening, u64 key,
                               std::shared_ptr<graphics::GpuCompletion> pending) {
    Block& block = *m_block;
    u64 sequence = 0;
    {
        BlockLock lock(block.lock);
        if (block.abandoned != 0 || block.owner != token(opening)) {
            return false;
        }
        block.owner = 0;
        block.key = key;
        sequence = ++block.release_sequence;
        if (pending != nullptr && m_shm != nullptr) {
            block.unsynced_sequence = sequence;
            block.unsynced_pid = static_cast<u32>(::getpid());
        } else if (block.unsynced_pid == static_cast<u32>(::getpid())) {
            // A newer release with nothing outstanding supersedes ours.
            block.unsynced_sequence = 0;
            block.unsynced_pid = 0;
        }
    }
    wake(block);
    if (pending != nullptr && m_shm != nullptr) {
        auto self = shared_from_this();
        CompletionWorker::instance().post(
            [self, sequence, pending = std::move(pending)] {
                (void)pending->wait();
                self->publish_completion(sequence);
            });
    }
    return true;
}

void SharedKeyedMutex::publish_completion(u64 release_sequence) {
    Block& block = *m_block;
    {
        BlockLock lock(block.lock);
        if (block.unsynced_sequence != release_sequence) return;
        block.unsynced_sequence = 0;
        block.unsynced_pid = 0;
    }
    wake(block);
}

void SharedKeyedMutex::opening_destroyed(u32 opening) {
    Block& block = *m_block;
    {
        BlockLock lock(block.lock);
        if (block.owner != token(opening)) return;
        block.owner = 0;
        block.abandoned = 1;
    }
    wake(block);
}

}  // namespace vitreo::winapi
