// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <shared_mutex>
#include <thread>
#include <vector>

#include "core/cpu/jit/jit_memo_lock.hpp"

using vitreo::cpu::jit::JitMemoLock;

namespace {

// Writers keep two plain counters equal; a reader that ever sees them differ
// overlapped a writer, and a final total short of the writes lost one to a
// writer/writer overlap. More threads than cores so holders get preempted
// and waiters reach the sleeping path.
TEST(JitMemoLock, ExcludesWritersFromReadersAndEachOther) {
    JitMemoLock lock;
    unsigned long long a = 0, b = 0;
    constexpr int kWriters = 8;
    constexpr int kReaders = 24;
    constexpr int kWrites = 20000;
    std::atomic<bool> go{false};
    std::atomic<int> writers_done{0};
    std::atomic<unsigned long long> torn{0}, reads{0};
    std::vector<std::thread> threads;
    for (int w = 0; w < kWriters; ++w) {
        threads.emplace_back([&] {
            while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
            for (int i = 0; i < kWrites; ++i) {
                std::unique_lock<JitMemoLock> g(lock);
                ++a;
                ++b;
            }
            writers_done.fetch_add(1, std::memory_order_acq_rel);
        });
    }
    for (int r = 0; r < kReaders; ++r) {
        threads.emplace_back([&] {
            while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
            unsigned long long n = 0;
            while (writers_done.load(std::memory_order_acquire) < kWriters) {
                std::shared_lock<JitMemoLock> g(lock);
                if (a != b) torn.fetch_add(1, std::memory_order_relaxed);
                ++n;
            }
            reads.fetch_add(n, std::memory_order_relaxed);
        });
    }
    go.store(true, std::memory_order_release);
    for (auto& t : threads) t.join();
    EXPECT_EQ(torn.load(), 0u);
    EXPECT_EQ(a, static_cast<unsigned long long>(kWriters) * kWrites);
    EXPECT_EQ(b, a);
    EXPECT_GT(reads.load(), 0u);
}

// Readers that re-enter back to back never leave the lock free, so only the
// waiting-writer bit lets a writer in. The writer must finish while the
// readers are still running.
TEST(JitMemoLock, WaitingWriterIsNotStarvedByOverlappingReaders) {
    JitMemoLock lock;
    std::atomic<bool> stop{false};
    std::vector<std::thread> readers;
    for (int r = 0; r < 8; ++r) {
        readers.emplace_back([&] {
            while (!stop.load(std::memory_order_acquire)) {
                std::shared_lock<JitMemoLock> g(lock);
                for (int spin = 0; spin < 64; ++spin) {
                    std::atomic_signal_fence(std::memory_order_seq_cst);
                }
            }
        });
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < 2000; ++i) {
        std::unique_lock<JitMemoLock> g(lock);
    }
    const auto elapsed = std::chrono::steady_clock::now() - start;
    stop.store(true, std::memory_order_release);
    for (auto& t : readers) t.join();
    EXPECT_LT(elapsed, std::chrono::seconds(10));
}

TEST(JitMemoLock, TryLockRespectsHolders) {
    JitMemoLock lock;
    ASSERT_TRUE(lock.try_lock_shared());
    EXPECT_TRUE(lock.try_lock_shared()) << "readers share";
    EXPECT_FALSE(lock.try_lock()) << "a writer waits for readers";
    lock.unlock_shared();
    lock.unlock_shared();
    ASSERT_TRUE(lock.try_lock());
    EXPECT_FALSE(lock.try_lock());
    EXPECT_FALSE(lock.try_lock_shared());
    lock.unlock();
    EXPECT_TRUE(lock.try_lock_shared());
    lock.unlock_shared();
}

}  // namespace
