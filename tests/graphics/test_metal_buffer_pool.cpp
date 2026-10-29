// SPDX-License-Identifier: LGPL-2.1-only

/// @file test_metal_buffer_pool.cpp
/// @brief Pooled storage for small constant buffers (MetalConstantBufferPool).
///
/// A D3D11 renderer creates thousands of small constant buffers a second, and
/// each used to be its own MTLBuffer. A small Uniform buffer now takes a slot
/// in a shared slab. These tests read the real storage: a slot is never handed
/// to a second buffer while the GPU may still read it, a new buffer reads as
/// its initial data or as zeros whatever the slot held before, and every path
/// that moves the buffer back to its owned storage lands at the slot's offset
/// rather than at the start of the slab.

#include <gtest/gtest.h>

#include "graphics/metal/metal_buffer.hpp"
#include "graphics/metal/metal_device.hpp"
#include "graphics/metal/metal_submit_queue.hpp"

#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

using namespace vitreo::graphics;
using vitreo::u32;
using vitreo::u8;
using vitreo::usize;

namespace {

using Slot = std::pair<void*, usize>;

[[nodiscard]] Slot slot_of(MetalBuffer& buffer) {
    return {buffer.handle(), buffer.gpu_offset()};
}

[[nodiscard]] std::vector<u8> pattern(usize bytes, u8 seed) {
    std::vector<u8> out(bytes);
    for (usize i = 0; i < bytes; ++i) {
        out[i] = static_cast<u8>(seed + i * 7u);
    }
    return out;
}

[[nodiscard]] bool reads_as(MetalBuffer& buffer, const std::vector<u8>& want) {
    const void* bytes = buffer.map();
    return bytes != nullptr &&
           std::memcmp(bytes, want.data(), want.size()) == 0;
}

/// Retire every generation recorded so far. An empty recording submits
/// nothing and so retires nothing, so the list gets real work first.
void retire_all(void* device) {
    MetalBuffer scratch(device, 256, BufferUsage::Storage);
    MetalSubmitQueue::instance().record_fill_buffer(scratch.handle(), 0, 16, 0);
    MetalSubmitQueue::instance().drain();
}

}  // namespace

TEST(MetalBufferPool, PooledConstantBufferReadsBackItsInitialData) {
    auto& device = MetalDevice::instance();
    if (!device.available()) GTEST_SKIP() << "no Metal device";
    const auto bytes = pattern(256, 0);
    MetalBuffer buffer(device.native_device(), 256, BufferUsage::Uniform,
                       bytes.data());
    ASSERT_NE(buffer.handle(), nullptr);
    EXPECT_TRUE(buffer.pooled());
    EXPECT_TRUE(reads_as(buffer, bytes));
    EXPECT_EQ(buffer.gpu_offset() % 256u, 0u);
}

// A slot's previous bytes never show through: D3D11 leaves a buffer created
// without data undefined, but a fresh MTLBuffer is zeroed and so is the CPU
// mirror a rename starts from, so the pool must zero too.
TEST(MetalBufferPool, RecycledSlotReadsAsZeroWithoutInitialData) {
    auto& device = MetalDevice::instance();
    if (!device.available()) GTEST_SKIP() << "no Metal device";
    void* mtl = device.native_device();
    Slot first{};
    {
        const std::vector<u8> fill(512, 0xAB);
        MetalBuffer a(mtl, 512, BufferUsage::Uniform, fill.data());
        ASSERT_TRUE(a.pooled());
        first = slot_of(a);
    }
    retire_all(mtl);
    MetalBuffer b(mtl, 512, BufferUsage::Uniform);
    ASSERT_TRUE(b.pooled());
    // The test is only meaningful if b really took a's old slot.
    ASSERT_EQ(slot_of(b), first);
    EXPECT_TRUE(reads_as(b, std::vector<u8>(512, 0)));
}

TEST(MetalBufferPool, PooledSlotIsNotReusedBeforeItsGenerationRetires) {
    auto& device = MetalDevice::instance();
    if (!device.available()) GTEST_SKIP() << "no Metal device";
    void* mtl = device.native_device();
    Slot first{};
    {
        MetalBuffer a(mtl, 1024, BufferUsage::Uniform);
        ASSERT_TRUE(a.pooled());
        first = slot_of(a);
        // Bound by the list still being recorded: live until it completes.
        a.note_gpu_bind();
    }
    MetalBuffer b(mtl, 1024, BufferUsage::Uniform);
    EXPECT_NE(slot_of(b), first) << "slot reused while the GPU may read it";
    retire_all(mtl);
    MetalBuffer c(mtl, 1024, BufferUsage::Uniform);
    EXPECT_EQ(slot_of(c), first) << "a retired slot is reused";
}

namespace {

/// Two pooled buffers of one size class with different contents; returns the
/// one that does NOT sit at the start of its slab, so a write that lands at
/// offset 0 instead of the slot is visible.
struct OffsetPair {
    std::unique_ptr<MetalBuffer> target;
    std::unique_ptr<MetalBuffer> other;
    std::vector<u8> target_bytes;
    std::vector<u8> other_bytes;
};

[[nodiscard]] OffsetPair offset_pair(void* mtl, usize bytes) {
    OffsetPair out;
    out.target_bytes = pattern(bytes, 3);
    out.other_bytes = pattern(bytes, 101);
    out.target = std::make_unique<MetalBuffer>(mtl, bytes, BufferUsage::Uniform,
                                               out.target_bytes.data());
    out.other = std::make_unique<MetalBuffer>(mtl, bytes, BufferUsage::Uniform,
                                              out.other_bytes.data());
    if (out.target->gpu_offset() == 0) {
        std::swap(out.target, out.other);
        std::swap(out.target_bytes, out.other_bytes);
    }
    return out;
}

}  // namespace

TEST(MetalBufferPool, DiscardRenameAndGpuWriteRoundTripKeepTheOwnedOffset) {
    auto& device = MetalDevice::instance();
    if (!device.available()) GTEST_SKIP() << "no Metal device";
    void* mtl = device.native_device();
    auto pair = offset_pair(mtl, 4096);
    MetalBuffer& p = *pair.target;
    ASSERT_TRUE(p.pooled());
    ASSERT_NE(p.gpu_offset(), 0u);
    const Slot owned = slot_of(p);

    p.note_gpu_bind();
    const auto updated = pattern(4096, 55);
    p.update(updated.data(), 0, updated.size(), BufferWrite::Discard);
    ASSERT_NE(slot_of(p), owned) << "DISCARD on a bound buffer renames";
    p.note_gpu_write();
    EXPECT_EQ(slot_of(p), owned) << "a GPU write moves back to the slot";

    retire_all(mtl);
    EXPECT_EQ(slot_of(p), owned);
    EXPECT_TRUE(reads_as(p, updated));
    EXPECT_TRUE(reads_as(*pair.other, pair.other_bytes))
        << "the copy back landed outside the slot";
}

TEST(MetalBufferPool, RetiredDiscardSliceRestoresIntoTheOwnedSlot) {
    auto& device = MetalDevice::instance();
    if (!device.available()) GTEST_SKIP() << "no Metal device";
    void* mtl = device.native_device();
    auto pair = offset_pair(mtl, 2048);
    MetalBuffer& p = *pair.target;
    ASSERT_TRUE(p.pooled());
    ASSERT_NE(p.gpu_offset(), 0u);
    const Slot owned = slot_of(p);

    p.note_gpu_bind();
    const auto head = pattern(64, 77);
    p.update(head.data(), 0, head.size(), BufferWrite::Discard);
    ASSERT_NE(slot_of(p), owned) << "DISCARD on a bound buffer renames";
    retire_all(mtl);

    // The slice retired: the buffer returns to its slot from the CPU mirror.
    auto want = pair.target_bytes;
    std::memcpy(want.data(), head.data(), head.size());
    EXPECT_TRUE(reads_as(p, want));
    EXPECT_EQ(slot_of(p), owned);
    EXPECT_TRUE(reads_as(*pair.other, pair.other_bytes))
        << "the restore landed outside the slot";
}

TEST(MetalBufferPool, InPlaceUpdateWritesTheOwnedSlotOnly) {
    auto& device = MetalDevice::instance();
    if (!device.available()) GTEST_SKIP() << "no Metal device";
    void* mtl = device.native_device();
    auto pair = offset_pair(mtl, 8192);
    MetalBuffer& p = *pair.target;
    ASSERT_TRUE(p.pooled());
    ASSERT_NE(p.gpu_offset(), 0u);

    const auto mid = pattern(32, 9);
    p.update(mid.data(), 16, mid.size(), BufferWrite::Ordered);
    auto want = pair.target_bytes;
    std::memcpy(want.data() + 16, mid.data(), mid.size());
    EXPECT_TRUE(reads_as(p, want));
    EXPECT_TRUE(reads_as(*pair.other, pair.other_bytes));
}

TEST(MetalBufferPool, LargeAndNonUniformBuffersAreNotPooled) {
    auto& device = MetalDevice::instance();
    if (!device.available()) GTEST_SKIP() << "no Metal device";
    void* mtl = device.native_device();
    MetalBuffer large(mtl, 65537, BufferUsage::Uniform);
    MetalBuffer storage(mtl, 256, BufferUsage::Storage);
    ASSERT_NE(large.handle(), nullptr);
    ASSERT_NE(storage.handle(), nullptr);
    EXPECT_FALSE(large.pooled());
    EXPECT_FALSE(storage.pooled());
    EXPECT_EQ(large.gpu_offset(), 0u);
    EXPECT_EQ(storage.gpu_offset(), 0u);
    EXPECT_NE(large.handle(), storage.handle());
}

// CreateBuffer allocates storage outside the device lock, so buffers are
// created and destroyed on several threads at once while generations retire.
// A slot handed out twice shows as one live buffer's bytes overwritten.
TEST(MetalBufferPool, ConcurrentCreateAndDestroyNeverShareALiveSlot) {
    auto& device = MetalDevice::instance();
    if (!device.available()) GTEST_SKIP() << "no Metal device";
    void* mtl = device.native_device();
    std::atomic<bool> stop{false};
    std::thread retirer([&] {
        while (!stop.load()) {
            retire_all(mtl);
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    });
    std::atomic<u32> corrupted{0};
    std::atomic<u32> unpooled{0};
    std::vector<std::thread> workers;
    for (u32 t = 0; t < 8; ++t) {
        workers.emplace_back([&, t] {
            struct Live {
                std::unique_ptr<MetalBuffer> buffer;
                std::vector<u8> bytes;
            };
            std::vector<Live> ring(4);
            for (u32 i = 0; i < 400; ++i) {
                Live& entry = ring[i % ring.size()];
                if (entry.buffer && !reads_as(*entry.buffer, entry.bytes)) {
                    corrupted.fetch_add(1);
                }
                const usize bytes = usize{256} << (i % 3);
                entry.bytes = pattern(bytes, static_cast<u8>(t * 31u + i));
                entry.buffer = std::make_unique<MetalBuffer>(
                    mtl, bytes, BufferUsage::Uniform, entry.bytes.data());
                if (!entry.buffer->pooled()) unpooled.fetch_add(1);
                // Every few, a GPU use keeps the slot past its release.
                if (i % 5 == 0) entry.buffer->note_gpu_bind();
            }
            for (Live& entry : ring) {
                if (entry.buffer && !reads_as(*entry.buffer, entry.bytes)) {
                    corrupted.fetch_add(1);
                }
            }
        });
    }
    for (auto& worker : workers) worker.join();
    stop.store(true);
    retirer.join();
    EXPECT_EQ(corrupted.load(), 0u);
    EXPECT_EQ(unpooled.load(), 0u);
}
