// SPDX-License-Identifier: LGPL-2.1-only

/// @file test_metal_buffer_rename.cpp
/// @brief CPU buffer writes against GPU work that is still in flight.
///
/// Learn (D3D11_MAP, "How to: Use dynamic resources"; D3DLOCK and "Using
/// Dynamic Vertex and Index Buffers"):
///   * WRITE_DISCARD / D3DLOCK_DISCARD: "If the GPU is still using the buffer
///     ... a pointer to a new region of memory is returned instead of the old
///     buffer data. This allows the GPU to continue using the old data".
///   * WRITE_NO_OVERWRITE / D3DLOCK_NOOVERWRITE: the application promises not
///     to touch data in use; the write goes to the existing storage.
///   * A plain lock / Map(WRITE): "the driver must finish using the resource
///     before returning from locking" -- work recorded earlier never sees the
///     new bytes.
///
/// Every test here makes the GPU really lag: the work that reads a buffer is
/// held behind a timeline fence (or simply not yet submitted) while the CPU
/// writes, and what that work read is checked once it runs. The expected bytes
/// are the ones the API contract names, never what Vitreo happened to leave.

#include <gtest/gtest.h>

#include "graphics/contracts/backend_device.hpp"
#include "graphics/metal/metal_buffer.hpp"
#include "graphics/metal/metal_device.hpp"
#include "graphics/metal/metal_submit_queue.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <memory>
#include <thread>
#include <vector>

using namespace vitreo::graphics;
using vitreo::u32;
using vitreo::u64;
using vitreo::u8;
using vitreo::usize;

namespace {

// The API's lock/map kinds, as MetalBuffer::update takes them.
constexpr BufferWrite kDiscard = BufferWrite::Discard;
constexpr BufferWrite kPlain = BufferWrite::Ordered;
constexpr BufferWrite kNoOverwrite = BufferWrite::NoOverwrite;

constexpr usize kMiB = 1024u * 1024u;
/// The bound the pool must keep to (256 MiB): three frames of DISCARD
/// traffic in flight, not the 1.5 GiB the per-present ring could reach.
constexpr usize kPoolCap = 256u * kMiB;
constexpr usize kProbe = 256;  // bytes each GPU read copies out

/// Record a GPU read of @p src's first @p bytes into @p dst at @p dst_offset,
/// in the order a draw binds a buffer: handle() (which may move it back to
/// its own storage), note_gpu_bind(), then the offset.
void gpu_read(MetalBuffer& src, MetalBuffer& dst, usize dst_offset,
              usize bytes = kProbe) {
    void* source = src.handle();
    src.note_gpu_bind();
    void* target = dst.handle();
    MetalSubmitQueue::instance().record_copy_buffer(
        source, target, bytes, src.gpu_offset(), dst.gpu_offset() + dst_offset);
}

[[nodiscard]] bool all_bytes(const u8* bytes, usize count, u8 value) {
    for (usize i = 0; i < count; ++i) {
        if (bytes[i] != value) return false;
    }
    return true;
}

/// Releases the GPU and drains the queue on every exit path, so a failed
/// assertion cannot leave later tests behind a closed fence.
struct GateGuard {
    BackendFence* gate;
    ~GateGuard() {
        if (gate != nullptr) (void)gate->signal_cpu(~0ull >> 1);
        MetalSubmitQueue::instance().drain();
    }
};

}  // namespace

// More DISCARD renames than the pool holds, all while the GPU has retired
// none of the work reading them. The runtime may stall when renaming memory
// runs out; it may never hand back storage a recorded read still uses. The
// old pool fell back to writing in place at 32 slabs, under the reads of
// every earlier frame.
TEST(MetalBufferRename, DiscardPastThePoolWaitsInsteadOfWritingInPlace) {
    auto& device = MetalDevice::instance();
    if (!device.available()) GTEST_SKIP() << "no Metal device";
    if (!device.supports_fences()) GTEST_SKIP() << "no timeline fences";
    void* mtl = device.native_device();
    MetalSubmitQueue::instance().drain();

    auto gate = device.create_fence(0);
    ASSERT_NE(gate, nullptr);
    GateGuard guard{gate.get()};
    // Everything recorded from here waits on the gate.
    ASSERT_TRUE(gate->wait_gpu(1));
    (void)MetalSubmitQueue::instance().submit();

    constexpr usize kBytes = 16u * kMiB;
    constexpr u32 kFrames = 40;  // > 32 slabs of the old ring, > the cap
    MetalBuffer stream(mtl, kBytes, BufferUsage::Vertex);
    MetalBuffer readback(mtl, kFrames * kProbe, BufferUsage::Storage);
    ASSERT_NE(stream.handle(), nullptr);
    ASSERT_NE(readback.handle(), nullptr);
    const u64 hazards_before = metal_buffer_hazard_writes();

    std::atomic<bool> released{false};
    std::thread releaser([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        released.store(true);
        (void)gate->signal_cpu(1);
    });
    std::vector<u8> frame(kBytes);
    usize peak_bytes = 0;
    for (u32 f = 0; f < kFrames; ++f) {
        std::memset(frame.data(), static_cast<int>(0x10 + f), frame.size());
        stream.update(frame.data(), 0, frame.size(), kDiscard);
        gpu_read(stream, readback, f * kProbe);
        peak_bytes = std::max(peak_bytes, metal_discard_pool_stats().bytes);
    }
    MetalSubmitQueue::instance().drain();
    releaser.join();

    const u8* got = static_cast<const u8*>(readback.map());
    ASSERT_NE(got, nullptr);
    for (u32 f = 0; f < kFrames; ++f) {
        EXPECT_TRUE(all_bytes(got + f * kProbe, kProbe,
                              static_cast<u8>(0x10 + f)))
            << "frame " << f << "'s read saw byte 0x" << std::hex
            << unsigned{got[f * kProbe]}
            << ": a later DISCARD wrote into storage it was still reading";
    }
    EXPECT_EQ(metal_buffer_hazard_writes(), hazards_before);
    EXPECT_LE(peak_bytes, kPoolCap) << "the pool outgrew its bound";
    EXPECT_TRUE(released.load());
}

// The measured failure (3/67 Satisfactory runs, "DISCARD slot N full at 32
// slabs; GPU has not retired gen G (completed G-3 or G-2)"): no long GPU
// stall, just a steady two-list lag. A buffer renamed once and then bound in
// every list kept its slice -- each bind re-tagged the slice's slot with the
// newest generation, so the slot was never retired at Present and every third
// frame's DISCARD traffic piled into it until the pool ran out and the next
// DISCARD wrote in place under an unexecuted read.
TEST(MetalBufferRename, SliceBoundEveryListDoesNotPinThePool) {
    auto& device = MetalDevice::instance();
    if (!device.available()) GTEST_SKIP() << "no Metal device";
    if (!device.supports_fences()) GTEST_SKIP() << "no timeline fences";
    void* mtl = device.native_device();
    auto& queue = MetalSubmitQueue::instance();
    queue.drain();

    auto gate = device.create_fence(0);
    ASSERT_NE(gate, nullptr);
    GateGuard guard{gate.get()};

    constexpr u32 kFrames = 60;
    constexpr usize kBytes = 16u * kMiB;
    const std::vector<u8> sticky_before(256, 0x5A);
    const std::vector<u8> sticky_after(256, 0xA5);
    MetalBuffer sticky(mtl, 256, BufferUsage::Uniform, sticky_before.data());
    MetalBuffer stream(mtl, kBytes, BufferUsage::Vertex);
    MetalBuffer reads(mtl, kFrames * 3 * kProbe, BufferUsage::Storage);
    ASSERT_NE(stream.handle(), nullptr);
    const u64 hazards_before = metal_buffer_hazard_writes();

    std::vector<u8> frame(kBytes);
    std::vector<u64> gens(kFrames, 0);
    usize peak_bytes = 0;
    for (u32 f = 0; f < kFrames; ++f) {
        // List f runs once the gate reaches f + 1.
        ASSERT_TRUE(gate->wait_gpu(f + 1));
        if (f == 1) {
            // Bound by frame 0's list, which has not run: renamed.
            sticky.update(sticky_after.data(), 0, sticky_after.size(),
                          kDiscard);
        }
        gpu_read(sticky, reads, (f * 3 + 0) * kProbe);
        for (u32 pass = 0; pass < 2; ++pass) {
            std::memset(frame.data(), static_cast<int>((f * 2 + pass) & 0xFF),
                        frame.size());
            stream.update(frame.data(), 0, frame.size(), kDiscard);
            gpu_read(stream, reads, (f * 3 + 1 + pass) * kProbe);
        }
        gens[f] = queue.submit();
        if (f >= 2) {
            // The GPU stays two lists behind.
            ASSERT_TRUE(gate->signal_cpu(f - 1));
            ASSERT_TRUE(queue.wait_for_generation(gens[f - 2]));
        }
        metal_note_presented_buffers();
        peak_bytes = std::max(peak_bytes, metal_discard_pool_stats().bytes);
    }
    ASSERT_TRUE(gate->signal_cpu(kFrames + 1));
    queue.drain();

    const u8* got = static_cast<const u8*>(reads.map());
    ASSERT_NE(got, nullptr);
    for (u32 f = 0; f < kFrames; ++f) {
        const u8 sticky_want = f == 0 ? 0x5A : 0xA5;
        EXPECT_TRUE(all_bytes(got + (f * 3) * kProbe, kProbe, sticky_want))
            << "frame " << f << " read the wrong sticky contents";
        for (u32 pass = 0; pass < 2; ++pass) {
            const u8 want = static_cast<u8>((f * 2 + pass) & 0xFF);
            EXPECT_TRUE(all_bytes(got + (f * 3 + 1 + pass) * kProbe, kProbe,
                                  want))
                << "frame " << f << " pass " << pass << " read 0x" << std::hex
                << unsigned{got[(f * 3 + 1 + pass) * kProbe]} << " not 0x"
                << unsigned{want};
        }
    }
    EXPECT_EQ(metal_buffer_hazard_writes(), hazards_before);
    // Three lists in flight, two 16 MiB renames each: about 96 MiB live.
    EXPECT_LE(peak_bytes, kPoolCap) << "a bound slice pinned the pool";
}

// D3DLOCK_NOOVERWRITE / WRITE_NO_OVERWRITE: the bytes go into the storage the
// GPU is reading -- the application promised they are not the ones in use --
// so no rename, and nothing to report.
TEST(MetalBufferRename, NoOverwriteWritesInPlaceBesideInFlightReads) {
    auto& device = MetalDevice::instance();
    if (!device.available()) GTEST_SKIP() << "no Metal device";
    if (!device.supports_fences()) GTEST_SKIP() << "no timeline fences";
    void* mtl = device.native_device();
    MetalSubmitQueue::instance().drain();

    auto gate = device.create_fence(0);
    ASSERT_NE(gate, nullptr);
    GateGuard guard{gate.get()};
    ASSERT_TRUE(gate->wait_gpu(1));

    const std::vector<u8> initial(4096, 0x11);
    MetalBuffer ring(mtl, initial.size(), BufferUsage::Vertex, initial.data());
    MetalBuffer reads(mtl, 2 * kProbe, BufferUsage::Storage);
    void* storage = ring.handle();
    const usize offset = ring.gpu_offset();
    gpu_read(ring, reads, 0);  // reads bytes [0, 256), behind the gate
    const u64 hazards_before = metal_buffer_hazard_writes();

    const std::vector<u8> append(kProbe, 0x22);
    ring.update(append.data(), 1024, append.size(), kNoOverwrite);
    EXPECT_EQ(ring.handle(), storage) << "NO_OVERWRITE must not rename";
    EXPECT_EQ(ring.gpu_offset(), offset);
    EXPECT_EQ(metal_buffer_hazard_writes(), hazards_before)
        << "a NO_OVERWRITE write was reported as a hazard";

    // A read recorded after the append sees it, in the same storage.
    void* source = ring.handle();
    ring.note_gpu_bind();
    MetalSubmitQueue::instance().record_copy_buffer(
        source, reads.handle(), kProbe, ring.gpu_offset() + 1024,
        reads.gpu_offset() + kProbe);
    ASSERT_TRUE(gate->signal_cpu(1));
    MetalSubmitQueue::instance().drain();
    const u8* got = static_cast<const u8*>(reads.map());
    ASSERT_NE(got, nullptr);
    EXPECT_TRUE(all_bytes(got, kProbe, 0x11));
    EXPECT_TRUE(all_bytes(got + kProbe, kProbe, 0x22));
}

// A plain write (D3D9 Lock without DISCARD or NOOVERWRITE, D3D11
// Map(WRITE) on a mappable DEFAULT buffer, UpdateSubresource): work recorded
// before it reads the old bytes, as if the lock had waited for the GPU.
// Writing in place under that work is what the second hazard looked like.
TEST(MetalBufferRename, PlainWriteNeverLandsUnderEarlierReads) {
    auto& device = MetalDevice::instance();
    if (!device.available()) GTEST_SKIP() << "no Metal device";
    if (!device.supports_fences()) GTEST_SKIP() << "no timeline fences";
    void* mtl = device.native_device();
    MetalSubmitQueue::instance().drain();

    auto gate = device.create_fence(0);
    ASSERT_NE(gate, nullptr);
    GateGuard guard{gate.get()};
    ASSERT_TRUE(gate->wait_gpu(1));

    const std::vector<u8> initial(1u << 20, 0x33);
    MetalBuffer buffer(mtl, initial.size(), BufferUsage::Vertex,
                       initial.data());
    MetalBuffer reads(mtl, 2 * kProbe, BufferUsage::Storage);
    gpu_read(buffer, reads, 0);
    const u64 hazards_before = metal_buffer_hazard_writes();

    // Two bytes inside the range the earlier read copies.
    const std::vector<u8> patch(2, 0x44);
    buffer.update(patch.data(), 110, patch.size(), kPlain);
    EXPECT_EQ(metal_buffer_hazard_writes(), hazards_before);
    gpu_read(buffer, reads, kProbe);
    ASSERT_TRUE(gate->signal_cpu(1));
    MetalSubmitQueue::instance().drain();

    const u8* got = static_cast<const u8*>(reads.map());
    ASSERT_NE(got, nullptr);
    EXPECT_TRUE(all_bytes(got, kProbe, 0x33))
        << "the read recorded before the write saw byte 0x" << std::hex
        << unsigned{got[110]} << " at +110";
    std::vector<u8> want(kProbe, 0x33);
    want[110] = want[111] = 0x44;
    EXPECT_EQ(std::memcmp(got + kProbe, want.data(), kProbe), 0)
        << "the read recorded after the write missed it";
    // And every byte the write did not touch survived the rename.
    const u8* now = static_cast<const u8*>(buffer.map());
    ASSERT_NE(now, nullptr);
    EXPECT_EQ(now[109], 0x33);
    EXPECT_EQ(now[110], 0x44);
    EXPECT_EQ(now[(1u << 20) - 1], 0x33);
}

// The CPU mirror of a renamed buffer describes it only until the GPU writes
// it (a copy or fill destination). A later partial rename must start from
// what the GPU left, and the restore to owned storage must not bring back
// the mirror's older bytes.
TEST(MetalBufferRename, GpuWriteRetiresTheCpuMirror) {
    auto& device = MetalDevice::instance();
    if (!device.available()) GTEST_SKIP() << "no Metal device";
    void* mtl = device.native_device();
    auto& queue = MetalSubmitQueue::instance();
    queue.drain();

    const std::vector<u8> a(1024, 0xA0);
    const std::vector<u8> b(1024, 0xB0);
    MetalBuffer buffer(mtl, a.size(), BufferUsage::Vertex, a.data());
    MetalBuffer scratch(mtl, kProbe, BufferUsage::Storage);
    gpu_read(buffer, scratch, 0);
    // In flight: renamed, which builds the CPU mirror (all 0xB0).
    buffer.update(b.data(), 0, b.size(), kDiscard);

    // The GPU now writes the buffer (a fill, like ClearUnorderedAccessView or
    // a copy destination would).
    buffer.note_gpu_write();
    queue.record_fill_buffer(buffer.handle(), buffer.gpu_offset(), 1024, 0xCC);
    queue.drain();

    // Bound again, then 16 bytes rewritten in place of a rename.
    gpu_read(buffer, scratch, 0);
    const std::vector<u8> head(16, 0xD0);
    buffer.update(head.data(), 0, head.size(), kDiscard);
    queue.drain();

    const u8* got = static_cast<const u8*>(buffer.map());
    ASSERT_NE(got, nullptr);
    EXPECT_TRUE(all_bytes(got, 16, 0xD0));
    EXPECT_TRUE(all_bytes(got + 16, 1024 - 16, 0xCC))
        << "byte +16 reads 0x" << std::hex << unsigned{got[16]}
        << ": the stale CPU mirror overwrote what the GPU wrote";
}

// -- Review follow-ups -------------------------------------------------------

namespace {

constexpr usize kKiB = 1024u;

/// Sets @p stop and joins @p thread on every exit path, so a failed
/// assertion cannot destroy a joinable thread.
struct ThreadGuard {
    std::thread& thread;
    std::atomic<bool>& stop;
    ~ThreadGuard() {
        stop.store(true);
        if (thread.joinable()) thread.join();
    }
};

/// Presents enough for a retired, untouched oversize slab to be released
/// (the pool keeps one for 8 idle Presents).
void present_until_oversize_idle() {
    for (u32 i = 0; i < 10; ++i) metal_note_presented_buffers();
}

/// Submit and retire @p lists lists, each with work of its own.
void retire_lists(void* mtl, u32 lists) {
    MetalBuffer scratch(mtl, 256, BufferUsage::Storage);
    for (u32 i = 0; i < lists; ++i) {
        MetalSubmitQueue::instance().record_fill_buffer(scratch.handle(), 0,
                                                        16, 0);
        MetalSubmitQueue::instance().drain();
    }
}

/// What the GPU reads at @p storage + @p at, through a copy recorded now.
[[nodiscard]] std::vector<u8> gpu_bytes(void* storage, usize at, usize bytes,
                                        void* mtl) {
    MetalBuffer out(mtl, bytes, BufferUsage::Storage);
    MetalSubmitQueue::instance().record_copy_buffer(storage, out.handle(),
                                                    bytes, at,
                                                    out.gpu_offset());
    MetalSubmitQueue::instance().drain();
    const u8* got = static_cast<const u8*>(out.map());
    return got != nullptr ? std::vector<u8>(got, got + bytes)
                          : std::vector<u8>{};
}

}  // namespace

// M1, the interleaving the D3D9 worker makes: it starts binding a buffer
// whose owned storage is still read (so handle() keeps the slice), the GPU
// retires that work -- the slice's slab looks free -- and the API thread's
// next DISCARD reclaims slabs before the worker finishes the bind. The old
// pool handed the slab out again and another buffer's bytes landed under the
// draw. Run alone it is deterministic for the old pool (the slice sits in the
// lowest slab, the first a reclaim takes).
TEST(MetalBufferRename, BindRacingAReclaimKeepsItsSlice) {
    auto& device = MetalDevice::instance();
    if (!device.available()) GTEST_SKIP() << "no Metal device";
    if (!device.supports_fences()) GTEST_SKIP() << "no timeline fences";
    void* mtl = device.native_device();
    auto& queue = MetalSubmitQueue::instance();
    queue.drain();

    auto gate = device.create_fence(0);
    ASSERT_NE(gate, nullptr);
    GateGuard guard{gate.get()};

    constexpr usize kSticky = 4u * kMiB;
    constexpr usize kOther = 13u * kMiB;  // does not fit the slab's tail
    MetalBuffer sticky(mtl, kSticky, BufferUsage::Vertex);
    MetalBuffer scratch(mtl, kProbe, BufferUsage::Storage);
    std::vector<std::unique_ptr<MetalBuffer>> others;
    for (u32 i = 0; i < 4; ++i) {
        others.push_back(
            std::make_unique<MetalBuffer>(mtl, kOther, BufferUsage::Vertex));
    }
    const std::vector<u8> mine(kSticky, 0x6B);
    const std::vector<u8> foreign(kOther, 0xF0);

    // List 1, held: it reads the owned storage, and the DISCARD after it
    // renames into a pool slice.
    ASSERT_TRUE(gate->wait_gpu(1));
    gpu_read(sticky, scratch, 0);
    sticky.update(mine.data(), 0, mine.size(), kDiscard);
    const u64 list1 = queue.submit();

    // The worker begins a bind: the owned storage is still read, so it keeps
    // the slice.
    void* storage = sticky.handle();
    const usize at = sticky.gpu_offset();
    // The GPU retires list 1 (the owned storage, and what the slice's slab
    // was tagged with) ...
    ASSERT_TRUE(gate->signal_cpu(1));
    ASSERT_TRUE(queue.wait_for_generation(list1));
    // ... and the API thread renames other buffers.
    for (auto& other : others) {
        gpu_read(*other, scratch, 0);
        other->update(foreign.data(), 0, foreign.size(), kDiscard);
    }
    // The worker finishes the bind and its draw reads the slice.
    sticky.note_gpu_bind();
    const auto got = gpu_bytes(storage, at, kProbe, mtl);
    ASSERT_EQ(got.size(), kProbe);
    EXPECT_TRUE(all_bytes(got.data(), kProbe, 0x6B))
        << "the bound slice reads 0x" << std::hex << unsigned{got[0]}
        << ": a reclaim handed its slab to another buffer mid-bind";
}

// M1, the handshake itself: a reclaim attempt between a bind's handle() and
// the bind backs off, because handle() pinned the slice; and a slice nobody
// pinned is reclaimed, after which its buffer leaves it for the owned storage
// and keeps its contents.
TEST(MetalBufferRename, PinnedSliceSurvivesAReclaimAttempt) {
    auto& device = MetalDevice::instance();
    if (!device.available()) GTEST_SKIP() << "no Metal device";
    if (!device.supports_fences()) GTEST_SKIP() << "no timeline fences";
    void* mtl = device.native_device();
    auto& queue = MetalSubmitQueue::instance();
    queue.drain();

    auto gate = device.create_fence(0);
    ASSERT_NE(gate, nullptr);
    GateGuard guard{gate.get()};
    const std::vector<u8> mine(64u * kKiB, 0x6C);
    MetalBuffer sticky(mtl, mine.size(), BufferUsage::Vertex);
    MetalBuffer scratch(mtl, kProbe, BufferUsage::Storage);
    const void* owned = sticky.handle();

    ASSERT_TRUE(gate->wait_gpu(1));
    gpu_read(sticky, scratch, 0);
    sticky.update(mine.data(), 0, mine.size(), kDiscard);
    const u64 list1 = queue.submit();
    void* storage = sticky.handle();  // a bind begins: keeps the slice
    const usize at = sticky.gpu_offset();
    ASSERT_NE(storage, owned);
    ASSERT_TRUE(gate->signal_cpu(1));
    ASSERT_TRUE(queue.wait_for_generation(list1));
    EXPECT_FALSE(metal_discard_pool_reclaim_for_test(storage))
        << "a reclaim took the slab of a slice a bind had pinned";
    sticky.note_gpu_bind();
    const auto got = gpu_bytes(storage, at, kProbe, mtl);
    ASSERT_EQ(got.size(), kProbe);
    EXPECT_TRUE(all_bytes(got.data(), kProbe, 0x6C))
        << "the bound slice reads 0x" << std::hex << unsigned{got[0]};

    // Unpinned (its pin retired with the list it was taken for), the slab
    // is reclaimed, and the buffer moves back with its contents intact.
    const std::vector<u8> next(mine.size(), 0x7D);
    MetalBuffer loose(mtl, next.size(), BufferUsage::Vertex);
    gpu_read(loose, scratch, 0);
    loose.update(next.data(), 0, next.size(), kDiscard);
    void* slice = loose.handle();
    ASSERT_NE(slice, nullptr);
    retire_lists(mtl, 2);  // the pinned list and the one of margin past it
    EXPECT_TRUE(metal_discard_pool_reclaim_for_test(slice))
        << "a retired, unpinned slab was not reclaimable";
    const u8* now = static_cast<const u8*>(loose.map());
    ASSERT_NE(now, nullptr);
    EXPECT_NE(loose.handle(), slice);
    EXPECT_TRUE(all_bytes(now, next.size(), 0x7D));
}

// M1 under real concurrency: one thread renames a buffer and binds its slice
// (handle(), a pause like a draw's recording, note_gpu_bind) while another
// renames other buffers freely. The schedule is legal for every frontend --
// nothing submits the binding thread's list while it binds.
TEST(MetalBufferRename, ConcurrentBindsAndReclaimsNeverShareASlice) {
    auto& device = MetalDevice::instance();
    if (!device.available()) GTEST_SKIP() << "no Metal device";
    void* mtl = device.native_device();
    auto& queue = MetalSubmitQueue::instance();
    queue.drain();

    constexpr u32 kIterations = 40;
    constexpr usize kSticky = 4u * kMiB;
    constexpr usize kOther = 13u * kMiB;
    constexpr usize kBusy = 64u * kMiB;
    MetalBuffer sticky(mtl, kSticky, BufferUsage::Vertex);
    MetalBuffer busy(mtl, kBusy, BufferUsage::Storage);
    MetalBuffer reads(mtl, kIterations * kProbe, BufferUsage::Storage);
    std::vector<std::unique_ptr<MetalBuffer>> others;
    for (u32 i = 0; i < 4; ++i) {
        others.push_back(
            std::make_unique<MetalBuffer>(mtl, kOther, BufferUsage::Vertex));
    }
    std::atomic<u32> iteration{0};
    std::atomic<u64> retire_target{0};
    std::atomic<bool> stop{false};
    std::thread renamer([&] {
        const std::vector<u8> foreign(kOther, 0xF0);
        u32 seen = 0;
        while (!stop.load()) {
            const u32 now = iteration.load();
            if (now == seen) {
                std::this_thread::yield();
                continue;
            }
            seen = now;
            // Once the binding thread's previous list -- the one that read
            // its owned storage -- has retired, the slice's slab looks free.
            while (!stop.load() && MetalSubmitQueue::instance()
                                           .completed_generation() <
                                       retire_target.load()) {
                std::this_thread::yield();
            }
            // A bounded burst per iteration, so the pool never fills and
            // never has to submit the binding thread's list itself.
            for (auto& other : others) {
                other->note_gpu_bind();
                other->update(foreign.data(), 0, foreign.size(), kDiscard);
            }
        }
    });
    ThreadGuard renamer_guard{renamer, stop};

    std::vector<u8> mine(kSticky);
    for (u32 i = 0; i < kIterations; ++i) {
        const u8 value = static_cast<u8>(0x20 + i);
        std::fill(mine.begin(), mine.end(), value);
        sticky.note_gpu_bind();
        sticky.update(mine.data(), 0, mine.size(), kDiscard);
        // GPU work in the same list, so the owned storage stays read for a
        // moment after the submit.
        queue.record_fill_buffer(busy.handle(), busy.gpu_offset(), kBusy,
                                 value);
        const u64 list = queue.submit();
        void* storage = sticky.handle();
        const usize at = sticky.gpu_offset();
        retire_target.store(list);
        iteration.store(i + 1);
        ASSERT_TRUE(queue.wait_for_generation(list));
        // A draw's recording takes a while; renames run meanwhile.
        std::this_thread::sleep_for(std::chrono::milliseconds(3));
        sticky.note_gpu_bind();
        queue.record_copy_buffer(storage, reads.handle(), kProbe, at,
                                 reads.gpu_offset() + i * kProbe);
    }
    stop.store(true);
    if (renamer.joinable()) renamer.join();
    queue.drain();

    const u8* got = static_cast<const u8*>(reads.map());
    ASSERT_NE(got, nullptr);
    u32 corrupted = 0;
    for (u32 i = 0; i < kIterations; ++i) {
        if (!all_bytes(got + i * kProbe, kProbe, static_cast<u8>(0x20 + i))) {
            ++corrupted;
        }
    }
    EXPECT_EQ(corrupted, 0u)
        << corrupted << " binds read another buffer's DISCARD bytes";
}

// A NO_OVERWRITE write on the API thread races the D3D9 worker's handle(),
// which moves the buffer from its slice back to the owned storage once that
// retires. Every byte written must survive the move.
TEST(MetalBufferRename, ConcurrentNoOverwriteAndRestoreKeepEveryByte) {
    auto& device = MetalDevice::instance();
    if (!device.available()) GTEST_SKIP() << "no Metal device";
    void* mtl = device.native_device();
    auto& queue = MetalSubmitQueue::instance();
    queue.drain();

    constexpr usize kBytes = 4u * kMiB;
    constexpr usize kChunk = 64;
    constexpr usize kBusy = 64u * kMiB;
    constexpr u32 kRounds = 12;
    MetalBuffer ring(mtl, kBytes, BufferUsage::Vertex);
    MetalBuffer busy(mtl, kBusy, BufferUsage::Storage);
    std::vector<u8> want(kBytes, 0);
    u32 lost_rounds = 0;
    for (u32 round = 0; round < kRounds; ++round) {
        // Drained, as D3D9 is at a DISCARD Unlock: rename while the owned
        // storage is read by a list still running.
        std::fill(want.begin(), want.end(), static_cast<u8>(round));
        ring.note_gpu_bind();
        ring.update(want.data(), 0, want.size(), kDiscard);
        queue.record_fill_buffer(busy.handle(), busy.gpu_offset(), kBusy,
                                 static_cast<u8>(round));
        (void)queue.submit();
        // The worker binds the buffer over and over; once the list retires
        // a bind moves it back to the owned storage.
        std::atomic<bool> stop{false};
        std::thread worker([&] {
            while (!stop.load()) {
                (void)ring.handle();
                ring.note_gpu_bind();
            }
        });
        // Meanwhile the API thread appends with NO_OVERWRITE.
        const auto until =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(8);
        usize offset = 0;
        u8 tag = 0x80;
        // Chunk after chunk, each written once (a later rewrite would hide
        // a lost one), so writes keep landing while the worker's move copies
        // the mirror.
        while (std::chrono::steady_clock::now() < until &&
               offset + kChunk <= kBytes) {
            const std::vector<u8> chunk(kChunk, tag);
            ring.update(chunk.data(), offset, chunk.size(), kNoOverwrite);
            std::fill(want.begin() + static_cast<std::ptrdiff_t>(offset),
                      want.begin() + static_cast<std::ptrdiff_t>(offset +
                                                                 kChunk),
                      tag);
            offset += kChunk;
            tag = static_cast<u8>(tag == 0xFE ? 0x80 : tag + 1);
        }
        stop.store(true);
        worker.join();
        queue.drain();
        const u8* got = static_cast<const u8*>(ring.map());
        ASSERT_NE(got, nullptr);
        if (std::memcmp(got, want.data(), kBytes) != 0) ++lost_rounds;
    }
    EXPECT_EQ(lost_rounds, 0u)
        << lost_rounds << " rounds lost NO_OVERWRITE bytes to a move";
}

// The new restore rule's core case: the slice is still read by a held list
// when the owned storage retires, a bind moves the buffer back to the owned
// storage, and then a NO_OVERWRITE write lands. The held list keeps reading
// the slice, later lists read the mirror plus the new bytes, and no other
// buffer's bytes appear anywhere.
TEST(MetalBufferRename, RestoreUnderAnInFlightSliceThenNoOverwrite) {
    auto& device = MetalDevice::instance();
    if (!device.available()) GTEST_SKIP() << "no Metal device";
    if (!device.supports_fences()) GTEST_SKIP() << "no timeline fences";
    void* mtl = device.native_device();
    auto& queue = MetalSubmitQueue::instance();
    queue.drain();

    auto gate = device.create_fence(0);
    ASSERT_NE(gate, nullptr);
    GateGuard guard{gate.get()};

    const std::vector<u8> initial(4096, 0x10);
    const std::vector<u8> renamed(4096, 0x20);
    const std::vector<u8> append(kProbe, 0x30);
    const std::vector<u8> foreign(4096, 0xEE);
    MetalBuffer b(mtl, initial.size(), BufferUsage::Vertex, initial.data());
    MetalBuffer neighbour(mtl, foreign.size(), BufferUsage::Vertex);
    MetalBuffer reads(mtl, 6 * kProbe, BufferUsage::Storage);
    const void* owned = b.handle();

    // List 1 (gate 1) reads the owned storage; the DISCARD renames.
    ASSERT_TRUE(gate->wait_gpu(1));
    gpu_read(b, reads, 0 * kProbe);
    b.update(renamed.data(), 0, renamed.size(), kDiscard);
    const u64 list1 = queue.submit();
    // List N (gate 2) reads the slice -- bytes [0, 256) and [1024, 1280).
    ASSERT_TRUE(gate->wait_gpu(2));
    ASSERT_NE(b.handle(), owned);
    gpu_read(b, reads, 1 * kProbe);
    {
        void* slice = b.handle();
        b.note_gpu_bind();
        queue.record_copy_buffer(slice, reads.handle(), kProbe,
                                 b.gpu_offset() + 1024,
                                 reads.gpu_offset() + 2 * kProbe);
    }
    (void)queue.submit();

    // The owned storage retires; list N does not.
    ASSERT_TRUE(gate->signal_cpu(1));
    ASSERT_TRUE(queue.wait_for_generation(list1));
    // Another buffer renames meanwhile: it must not get the slice's slab.
    gpu_read(neighbour, reads, 5 * kProbe);
    neighbour.update(foreign.data(), 0, foreign.size(), kDiscard);

    // A bind now moves b back to its owned storage, from the mirror.
    EXPECT_EQ(b.handle(), owned);
    gpu_read(b, reads, 3 * kProbe);
    // NO_OVERWRITE append outside every range read so far.
    b.update(append.data(), 1024, append.size(), kNoOverwrite);
    {
        void* storage = b.handle();
        b.note_gpu_bind();
        queue.record_copy_buffer(storage, reads.handle(), kProbe,
                                 b.gpu_offset() + 1024,
                                 reads.gpu_offset() + 4 * kProbe);
    }
    ASSERT_TRUE(gate->signal_cpu(2));
    queue.drain();

    const u8* got = static_cast<const u8*>(reads.map());
    ASSERT_NE(got, nullptr);
    EXPECT_TRUE(all_bytes(got + 0 * kProbe, kProbe, 0x10)) << "list 1";
    EXPECT_TRUE(all_bytes(got + 1 * kProbe, kProbe, 0x20)) << "list N, head";
    EXPECT_TRUE(all_bytes(got + 2 * kProbe, kProbe, 0x20))
        << "list N saw the later append: it went into the slice";
    EXPECT_TRUE(all_bytes(got + 3 * kProbe, kProbe, 0x20))
        << "after the restore, the mirror";
    EXPECT_TRUE(all_bytes(got + 4 * kProbe, kProbe, 0x30))
        << "after the restore, the append";
    std::vector<u8> want = renamed;
    std::copy(append.begin(), append.end(), want.begin() + 1024);
    const u8* now = static_cast<const u8*>(b.map());
    ASSERT_NE(now, nullptr);
    EXPECT_EQ(std::memcmp(now, want.data(), want.size()), 0);
}

// M2: a rename that does not fit the current slab's tail used to take a
// whole 16 MiB slab, so 9 MiB renames used 144 of the 256 MiB and the pool
// waited at 16 renames in flight. A new slab is max(16 MiB, 4 x the rename).
TEST(MetalBufferRename, LargeRenamesShareSlabsInsteadOfStrandingTails) {
    auto& device = MetalDevice::instance();
    if (!device.available()) GTEST_SKIP() << "no Metal device";
    if (!device.supports_fences()) GTEST_SKIP() << "no timeline fences";
    void* mtl = device.native_device();
    auto& queue = MetalSubmitQueue::instance();
    queue.drain();

    auto gate = device.create_fence(0);
    ASSERT_NE(gate, nullptr);
    GateGuard guard{gate.get()};
    ASSERT_TRUE(gate->wait_gpu(1));
    (void)queue.submit();

    constexpr usize kBytes = 9u * kMiB;
    constexpr u32 kRenames = 20;  // 180 MiB of slices
    MetalBuffer stream(mtl, kBytes, BufferUsage::Vertex);
    MetalBuffer reads(mtl, kRenames * kProbe, BufferUsage::Storage);
    const u64 waits_before = metal_discard_pool_stats().waits;
    std::thread releaser([&] {
        std::this_thread::sleep_for(std::chrono::seconds(2));
        (void)gate->signal_cpu(1);
    });
    std::vector<u8> frame(kBytes);
    for (u32 f = 0; f < kRenames; ++f) {
        std::fill(frame.begin(), frame.end(), static_cast<u8>(0x40 + f));
        stream.update(frame.data(), 0, frame.size(), kDiscard);
        gpu_read(stream, reads, f * kProbe);
    }
    const u64 waits = metal_discard_pool_stats().waits - waits_before;
    releaser.join();
    queue.drain();
    EXPECT_EQ(waits, 0u) << "180 MiB of 9 MiB renames hit the 256 MiB bound";
    const u8* got = static_cast<const u8*>(reads.map());
    ASSERT_NE(got, nullptr);
    for (u32 f = 0; f < kRenames; ++f) {
        EXPECT_TRUE(all_bytes(got + f * kProbe, kProbe,
                              static_cast<u8>(0x40 + f)))
            << "rename " << f;
    }
}

// m4: a rename too large for the bound's 4x slabs gets an oversize slab
// outside the bound, released once retired -- it renames rather than
// waiting on every in-flight DISCARD.
TEST(MetalBufferRename, RenameAboveTheBoundTakesAnOversizeSlab) {
    auto& device = MetalDevice::instance();
    if (!device.available()) GTEST_SKIP() << "no Metal device";
    if (!device.supports_fences()) GTEST_SKIP() << "no timeline fences";
    void* mtl = device.native_device();
    auto& queue = MetalSubmitQueue::instance();
    queue.drain();

    auto gate = device.create_fence(0);
    ASSERT_NE(gate, nullptr);
    GateGuard guard{gate.get()};
    ASSERT_TRUE(gate->wait_gpu(1));
    (void)queue.submit();

    constexpr usize kBytes = 72u * kMiB;
    constexpr u32 kRenames = 5;  // the first in place, then 288 MiB renamed
    MetalBuffer huge(mtl, kBytes, BufferUsage::Vertex);
    MetalBuffer reads(mtl, kRenames * kProbe, BufferUsage::Storage);
    const u64 waits_before = metal_discard_pool_stats().waits;
    std::thread releaser([&] {
        std::this_thread::sleep_for(std::chrono::seconds(2));
        (void)gate->signal_cpu(1);
    });
    std::vector<u8> frame(kBytes);
    u32 oversize_peak = 0;
    for (u32 f = 0; f < kRenames; ++f) {
        std::fill(frame.begin(), frame.end(), static_cast<u8>(0x50 + f));
        huge.update(frame.data(), 0, frame.size(), kDiscard);
        gpu_read(huge, reads, f * kProbe);
        oversize_peak =
            std::max(oversize_peak, metal_discard_pool_stats().oversize_slabs);
    }
    const u64 waits = metal_discard_pool_stats().waits - waits_before;
    releaser.join();
    queue.drain();
    EXPECT_EQ(waits, 0u) << "a 72 MiB DISCARD waited for the GPU";
    EXPECT_GE(oversize_peak, 4u);
    const u8* got = static_cast<const u8*>(reads.map());
    ASSERT_NE(got, nullptr);
    for (u32 f = 0; f < kRenames; ++f) {
        EXPECT_TRUE(all_bytes(got + f * kProbe, kProbe,
                              static_cast<u8>(0x50 + f)))
            << "rename " << f;
    }
    // Retired (with the list of tag margin past them) and unused at a
    // Present: released.
    (void)huge.handle();
    retire_lists(mtl, 2);
    present_until_oversize_idle();
    EXPECT_EQ(metal_discard_pool_stats().oversize_slabs, 0u);
}

// m3: a row copied from a texture into a buffer (D3D11 buffer RTVs) is a GPU
// write. The buffer must leave its pool slice before the copy takes its
// storage, or the row lands in the abandoned slice and is lost.
TEST(MetalBufferRename, RowDownloadIntoARenamedBufferLandsInItsStorage) {
    auto& device = MetalDevice::instance();
    if (!device.available()) GTEST_SKIP() << "no Metal device";
    MetalSubmitQueue::instance().drain();

    BackendBufferDesc bd{};
    bd.byte_width = 64;
    bd.shader_resource = true;
    bd.cpu_writable = true;
    const std::vector<u8> initial(64, 0x11);
    auto buffer = device.create_buffer(bd, initial.data());
    auto sink = device.create_buffer(bd, nullptr);
    ASSERT_NE(buffer, nullptr);
    ASSERT_NE(sink, nullptr);

    BackendTexture2DDesc td{};
    td.width = 4;
    td.height = 1;
    td.pixel_format = PixelFormat::RGBA8Unorm;
    td.shader_resource = true;
    const std::array<u8, 16> texels{1, 2,  3,  4,  5,  6,  7,  8,
                                    9, 10, 11, 12, 13, 14, 15, 16};
    auto texture = device.create_texture_2d(td, texels.data(), 16);
    ASSERT_NE(texture, nullptr);

    // A copy reads the buffer, so the DISCARD after it renames.
    ASSERT_TRUE(device.copy_buffer_region(sink.get(), 0, buffer.get(), 0, 64));
    const std::vector<u8> renamed(64, 0x22);
    ASSERT_TRUE(buffer->update(0, renamed.data(), renamed.size(),
                               BufferWrite::Discard));
    ASSERT_TRUE(device.copy_buffer_texture_row(buffer.get(), 0, texture.get(),
                                               4, /*to_texture=*/false));
    ASSERT_TRUE(device.wait_idle());
    std::array<u8, 64> got{};
    ASSERT_TRUE(buffer->read(0, got.data(), got.size()));
    EXPECT_TRUE(std::equal(texels.begin(), texels.end(), got.begin()))
        << "byte 0 reads 0x" << std::hex << unsigned{got[0]}
        << ": the row went into the abandoned slice";
    EXPECT_TRUE(all_bytes(got.data() + 16, 48, 0x22));
}

// -- Re-review follow-ups ----------------------------------------------------

// m1: a slice's slab is really reclaimed; then a NO_OVERWRITE write on one
// thread moves the buffer to its owned storage while another thread binds it
// (handle() then gpu_offset()) in a loop. Every pair a bind sees must be one
// storage and its own offset -- never the owned buffer with the slice's
// offset -- and no byte written may be lost. Run under TSan for the races.
TEST(MetalBufferRename, ReclaimedSliceMovesWithoutTearingConcurrentBinds) {
    auto& device = MetalDevice::instance();
    if (!device.available()) GTEST_SKIP() << "no Metal device";
    void* mtl = device.native_device();
    auto& queue = MetalSubmitQueue::instance();
    queue.drain();

    constexpr usize kBytes = 256u * kKiB;
    constexpr usize kChunk = 64;
    const std::vector<u8> initial(kBytes, 0x01);
    MetalBuffer b(mtl, kBytes, BufferUsage::Vertex, initial.data());
    MetalBuffer scratch(mtl, kProbe, BufferUsage::Storage);
    void* owned = b.handle();
    const usize owned_offset = b.gpu_offset();
    // Renamed while the owned storage is read; then everything retires and
    // the slab is reclaimed under the published slice.
    gpu_read(b, scratch, 0);
    const std::vector<u8> renamed(kBytes, 0x02);
    b.update(renamed.data(), 0, renamed.size(), kDiscard);
    void* slice = b.handle();
    const usize slice_offset = b.gpu_offset();
    ASSERT_NE(slice, owned);
    retire_lists(mtl, 2);
    ASSERT_TRUE(metal_discard_pool_reclaim_for_test(slice));

    std::atomic<bool> go{false};
    std::atomic<bool> stop{false};
    std::atomic<u32> torn{0};
    std::atomic<u32> stale{0};
    std::thread binder([&] {
        while (!go.load()) std::this_thread::yield();
        while (!stop.load()) {
            void* storage = b.handle();
            const usize at = b.gpu_offset();
            if (storage == slice) ++stale;  // the slab is someone else's
            if (storage == owned ? at != owned_offset
                                 : (storage != slice || at != slice_offset)) {
                ++torn;
            }
        }
    });
    std::vector<u8> want = renamed;
    go.store(true);
    for (usize offset = 0; offset + kChunk <= kBytes; offset += 4u * kKiB) {
        const std::vector<u8> chunk(kChunk, 0x77);
        b.update(chunk.data(), offset, chunk.size(), kNoOverwrite);
        std::fill(want.begin() + static_cast<std::ptrdiff_t>(offset),
                  want.begin() + static_cast<std::ptrdiff_t>(offset + kChunk),
                  0x77);
    }
    stop.store(true);
    binder.join();
    EXPECT_EQ(torn.load(), 0u) << "a bind saw one storage with another's offset";
    EXPECT_EQ(stale.load(), 0u) << "a bind used the reclaimed slice";
    const u8* now = static_cast<const u8*>(b.map());
    ASSERT_NE(now, nullptr);
    EXPECT_EQ(b.handle(), owned);
    EXPECT_EQ(std::memcmp(now, want.data(), want.size()), 0);
}

// m1, the tear itself: a claim that saw a pin and backed off still moved the
// slab's epoch. A NO_OVERWRITE write that pinned afterwards took that as a
// reclaim and moved the buffer to its owned storage -- between another
// thread's handle() and gpu_offset() for one bind, which then paired the
// slice with the owned storage's offset. An abandoned claim reset nothing:
// the slice is still the buffer's, and nothing moves.
TEST(MetalBufferRename, AbandonedReclaimDoesNotMoveABindInProgress) {
    auto& device = MetalDevice::instance();
    if (!device.available()) GTEST_SKIP() << "no Metal device";
    if (!device.supports_fences()) GTEST_SKIP() << "no timeline fences";
    void* mtl = device.native_device();
    auto& queue = MetalSubmitQueue::instance();
    queue.drain();

    auto gate = device.create_fence(0);
    ASSERT_NE(gate, nullptr);
    GateGuard guard{gate.get()};
    const std::vector<u8> initial(64u * kKiB, 0x08);
    MetalBuffer b(mtl, initial.size(), BufferUsage::Vertex, initial.data());
    MetalBuffer scratch(mtl, kProbe, BufferUsage::Storage);
    void* owned = b.handle();
    // Held list: the owned storage stays read; the DISCARD renames.
    ASSERT_TRUE(gate->wait_gpu(1));
    gpu_read(b, scratch, 0);
    const std::vector<u8> renamed(initial.size(), 0x09);
    b.update(renamed.data(), 0, renamed.size(), kDiscard);
    (void)queue.submit();

    // A bind begins: handle() keeps (and pins) the slice.
    void* storage = b.handle();
    ASSERT_NE(storage, owned);
    // A reclaim attempt on its slab backs off, its epoch moved.
    ASSERT_TRUE(metal_discard_pool_abandon_claim_for_test(storage));
    // Another thread appends with NO_OVERWRITE.
    const std::vector<u8> chunk(kProbe, 0x0A);
    b.update(chunk.data(), 4096, chunk.size(), kNoOverwrite);
    // The bind reads its offset: it must be the slice's.
    const usize at = b.gpu_offset();
    b.note_gpu_bind();
    EXPECT_EQ(b.handle(), storage) << "an abandoned reclaim moved the buffer";
    // Let the held list run, so the reads below are not queued behind it.
    ASSERT_TRUE(gate->signal_cpu(1));
    const auto head = gpu_bytes(storage, at, kProbe, mtl);
    const auto appended = gpu_bytes(storage, at + 4096, kProbe, mtl);
    ASSERT_EQ(head.size(), kProbe);
    ASSERT_EQ(appended.size(), kProbe);
    EXPECT_TRUE(all_bytes(head.data(), kProbe, 0x09))
        << "the bind paired the slice with another offset: 0x" << std::hex
        << unsigned{head[0]};
    EXPECT_TRUE(all_bytes(appended.data(), kProbe, 0x0A));
}

// A full pool waits for a slab that is still read. One already retired but
// too small to serve the rename (or to make room by its release) answers a
// wait at once, and the pool spun on it -- millions of times -- while the
// list that would free a usable slab was never waited for.
TEST(MetalBufferRename, ExhaustedPoolWaitsForALiveSlabNotARetiredOne) {
    auto& device = MetalDevice::instance();
    if (!device.available()) GTEST_SKIP() << "no Metal device";
    if (!device.supports_fences()) GTEST_SKIP() << "no timeline fences";
    void* mtl = device.native_device();
    auto& queue = MetalSubmitQueue::instance();
    // Start from an empty pool: everything retired and released.
    retire_lists(mtl, 2);
    for (u32 i = 0; i < 130; ++i) metal_note_presented_buffers();
    // One small slab, retired.
    {
        MetalBuffer small(mtl, kMiB, BufferUsage::Vertex);
        MetalBuffer scratch(mtl, kProbe, BufferUsage::Storage);
        gpu_read(small, scratch, 0);
        const std::vector<u8> bytes(kMiB, 0x11);
        small.update(bytes.data(), 0, bytes.size(), kDiscard);
    }
    retire_lists(mtl, 2);

    auto gate = device.create_fence(0);
    ASSERT_NE(gate, nullptr);
    GateGuard guard{gate.get()};
    ASSERT_TRUE(gate->wait_gpu(1));
    (void)queue.submit();

    // In the bound (below 1/12 of it): 80 MiB slabs, three of which and the
    // retired 16 MiB one fill it; the 13th rename finds it full.
    constexpr usize kBytes = 20u * kMiB;
    constexpr u32 kRenames = 14;
    MetalBuffer stream(mtl, kBytes, BufferUsage::Vertex);
    MetalBuffer reads(mtl, kRenames * kProbe, BufferUsage::Storage);
    const u64 waits_before = metal_discard_pool_stats().waits;
    std::atomic<bool> finished{false};
    std::thread releaser([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        (void)gate->signal_cpu(1);
    });
    ThreadGuard release_guard{releaser, finished};
    std::vector<u8> frame(kBytes);
    for (u32 f = 0; f < kRenames; ++f) {
        std::fill(frame.begin(), frame.end(), static_cast<u8>(0x30 + f));
        stream.update(frame.data(), 0, frame.size(), kDiscard);
        gpu_read(stream, reads, f * kProbe);
    }
    const u64 waits = metal_discard_pool_stats().waits - waits_before;
    if (releaser.joinable()) releaser.join();
    queue.drain();
    EXPECT_GE(waits, 1u);
    EXPECT_LE(waits, 8u) << "the full pool spun on a retired slab";
    const u8* got = static_cast<const u8*>(reads.map());
    ASSERT_NE(got, nullptr);
    for (u32 f = 0; f < kRenames; ++f) {
        EXPECT_TRUE(all_bytes(got + f * kProbe, kProbe,
                              static_cast<u8>(0x30 + f)))
            << "rename " << f;
    }
}

// The pool full of slices tagged for a recording with no work in it (binds
// whose draws never reached the stream). Waiting for that list used to be
// impossible -- it would never be submitted -- so the rename gave up and the
// write fell back to waiting for the buffer's own storage. The pool now gives
// the list a 4-byte fill of its own (and the list of tag margin after it),
// so the timeline advances and the rename still happens.
TEST(MetalBufferRename, EmptyRecordingStillAdvancesToFreeThePool) {
    auto& device = MetalDevice::instance();
    if (!device.available()) GTEST_SKIP() << "no Metal device";
    void* mtl = device.native_device();
    auto& queue = MetalSubmitQueue::instance();
    retire_lists(mtl, 2);
    queue.drain();  // nothing recorded from here on

    constexpr usize kBytes = 16u * kMiB;
    constexpr u32 kRenames = 24;  // past the 256 MiB bound
    MetalBuffer stream(mtl, kBytes, BufferUsage::Vertex);
    const u64 waits_before = metal_discard_pool_stats().waits;
    const u64 hazards_before = metal_buffer_hazard_writes();
    std::vector<u8> frame(kBytes);
    u32 renamed = 0;
    for (u32 f = 0; f < kRenames; ++f) {
        // Slices of one slab share its MTLBuffer: storage is the pair.
        void* before = stream.handle();
        const usize before_offset = stream.gpu_offset();
        stream.note_gpu_bind();  // a bind with no work recorded
        std::fill(frame.begin(), frame.end(), static_cast<u8>(0x40 + f));
        stream.update(frame.data(), 0, frame.size(), kDiscard);
        void* after = stream.handle();
        if (after != before || stream.gpu_offset() != before_offset) {
            ++renamed;
        }
    }
    EXPECT_GE(metal_discard_pool_stats().waits - waits_before, 1u);
    EXPECT_EQ(renamed, kRenames) << "a DISCARD fell back instead of renaming";
    EXPECT_EQ(metal_buffer_hazard_writes(), hazards_before);
    queue.drain();
    const u8* now = static_cast<const u8*>(stream.map());
    ASSERT_NE(now, nullptr);
    EXPECT_TRUE(all_bytes(now, kBytes, static_cast<u8>(0x40 + kRenames - 1)));
}

// m2: renames just above 1/8 of the bound. With oversize starting at 1/4 a
// 40 MiB rename took a 160 MiB slab, two of which do not fit the 256 MiB
// bound, so with the GPU a couple of lists behind the pool waited every few
// renames. Oversize slabs from 32 MiB up keep them flowing.
TEST(MetalBufferRename, RenamesAboveAnEighthOfTheBoundDoNotWaitWhileLagging) {
    auto& device = MetalDevice::instance();
    if (!device.available()) GTEST_SKIP() << "no Metal device";
    if (!device.supports_fences()) GTEST_SKIP() << "no timeline fences";
    void* mtl = device.native_device();
    auto& queue = MetalSubmitQueue::instance();
    queue.drain();

    auto gate = device.create_fence(0);
    ASSERT_NE(gate, nullptr);
    GateGuard guard{gate.get()};

    constexpr u32 kFrames = 14;
    constexpr usize kBytes = 40u * kMiB;
    MetalBuffer stream(mtl, kBytes, BufferUsage::Vertex);
    MetalBuffer reads(mtl, kFrames * kProbe, BufferUsage::Storage);
    const u64 waits_before = metal_discard_pool_stats().waits;
    // A pool that waits here waits on a list held by this thread's own gate:
    // open it after a while so a failure is a failure, not a hang.
    std::atomic<bool> finished{false};
    std::thread rescuer([&] {
        const auto until =
            std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!finished.load() && std::chrono::steady_clock::now() < until) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (!finished.load()) (void)gate->signal_cpu(1000);
    });
    ThreadGuard rescue_guard{rescuer, finished};
    std::vector<u8> frame(kBytes);
    std::vector<u64> gens(kFrames, 0);
    for (u32 f = 0; f < kFrames; ++f) {
        ASSERT_TRUE(gate->wait_gpu(f + 1));
        std::fill(frame.begin(), frame.end(), static_cast<u8>(0x60 + f));
        stream.update(frame.data(), 0, frame.size(), kDiscard);
        gpu_read(stream, reads, f * kProbe);
        gens[f] = queue.submit();
        if (f >= 2) {  // the GPU stays two lists behind
            (void)gate->signal_cpu(f - 1);  // refused once rescued: fine
            ASSERT_TRUE(queue.wait_for_generation(gens[f - 2]));
        }
        metal_note_presented_buffers();
    }
    const u64 waits = metal_discard_pool_stats().waits - waits_before;
    finished.store(true);
    if (rescuer.joinable()) rescuer.join();
    (void)gate->signal_cpu(kFrames + 1);
    queue.drain();
    EXPECT_EQ(waits, 0u) << "40 MiB renames waited with the GPU two lists behind";
    const u8* got = static_cast<const u8*>(reads.map());
    ASSERT_NE(got, nullptr);
    for (u32 f = 0; f < kFrames; ++f) {
        EXPECT_TRUE(all_bytes(got + f * kProbe, kProbe,
                              static_cast<u8>(0x60 + f)))
            << "frame " << f;
    }
}

// m3: oversize slabs have their own bound (512 MiB). Past it an oversize
// rename waits for the oldest oversize slab instead of allocating more.
TEST(MetalBufferRename, OversizeRenamesWaitAtTheirOwnBound) {
    auto& device = MetalDevice::instance();
    if (!device.available()) GTEST_SKIP() << "no Metal device";
    if (!device.supports_fences()) GTEST_SKIP() << "no timeline fences";
    void* mtl = device.native_device();
    auto& queue = MetalSubmitQueue::instance();
    retire_lists(mtl, 2);
    present_until_oversize_idle();  // release retired oversize slabs

    auto gate = device.create_fence(0);
    ASSERT_NE(gate, nullptr);
    GateGuard guard{gate.get()};
    ASSERT_TRUE(gate->wait_gpu(1));
    (void)queue.submit();

    constexpr usize kBytes = 72u * kMiB;
    constexpr u32 kRenames = 9;  // the first in place, 576 MiB renamed
    constexpr usize kOversizeBound = 512u * kMiB;
    MetalBuffer huge(mtl, kBytes, BufferUsage::Vertex);
    MetalBuffer reads(mtl, kRenames * kProbe, BufferUsage::Storage);
    const u64 waits_before = metal_discard_pool_stats().waits;
    std::atomic<bool> released{false};
    std::thread releaser([&] {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        released.store(true);
        (void)gate->signal_cpu(1);
    });
    std::vector<u8> frame(kBytes);
    usize peak = 0;
    for (u32 f = 0; f < kRenames; ++f) {
        std::fill(frame.begin(), frame.end(), static_cast<u8>(0x70 + f));
        huge.update(frame.data(), 0, frame.size(), kDiscard);
        gpu_read(huge, reads, f * kProbe);
        peak = std::max(peak, metal_discard_pool_stats().oversize_bytes);
    }
    const u64 waits = metal_discard_pool_stats().waits - waits_before;
    releaser.join();
    queue.drain();
    EXPECT_GE(waits, 1u) << "oversize renames grew past their bound";
    EXPECT_LE(peak, kOversizeBound);
    EXPECT_TRUE(released.load());
    const u8* got = static_cast<const u8*>(reads.map());
    ASSERT_NE(got, nullptr);
    for (u32 f = 0; f < kRenames; ++f) {
        EXPECT_TRUE(all_bytes(got + f * kProbe, kProbe,
                              static_cast<u8>(0x70 + f)))
            << "rename " << f;
    }
    (void)huge.handle();
    retire_lists(mtl, 2);
    present_until_oversize_idle();
    EXPECT_EQ(metal_discard_pool_stats().oversize_bytes, 0u);
}

// m4: another thread submits the list being recorded between a bind and the
// append of the work that binds -- D3D9's Present releases its lock across
// the submit, so a guest thread's draw can be split that way. The bind's
// work then lands in the next list, and the slice must stay protected
// through it: tags carry one generation of margin.
TEST(MetalBufferRename, BindSurvivesASubmitBeforeItsAppend) {
    auto& device = MetalDevice::instance();
    if (!device.available()) GTEST_SKIP() << "no Metal device";
    if (!device.supports_fences()) GTEST_SKIP() << "no timeline fences";
    void* mtl = device.native_device();
    auto& queue = MetalSubmitQueue::instance();
    queue.drain();

    auto gate = device.create_fence(0);
    ASSERT_NE(gate, nullptr);
    GateGuard guard{gate.get()};
    const std::vector<u8> mine(64u * kKiB, 0x6D);
    MetalBuffer sticky(mtl, mine.size(), BufferUsage::Vertex);
    MetalBuffer scratch(mtl, kProbe, BufferUsage::Storage);

    // List 1, held: reads the owned storage; the DISCARD renames.
    ASSERT_TRUE(gate->wait_gpu(1));
    gpu_read(sticky, scratch, 0);
    sticky.update(mine.data(), 0, mine.size(), kDiscard);
    (void)queue.submit();
    // A bind in list 2: the owned storage is still read, so it keeps the
    // slice.
    void* storage = sticky.handle();
    const usize at = sticky.gpu_offset();
    sticky.note_gpu_bind();
    // Another thread submits list 2 before the bind's work is appended, and
    // it retires.
    queue.record_fill_buffer(scratch.handle(), scratch.gpu_offset(), 16, 0);
    const u64 list2 = queue.submit();
    ASSERT_TRUE(gate->signal_cpu(1));
    ASSERT_TRUE(queue.wait_for_generation(list2));
    EXPECT_FALSE(metal_discard_pool_reclaim_for_test(storage))
        << "the slice's slab was reclaimable before the bind's work ran";
    // The bind's work, in list 3.
    const auto got = gpu_bytes(storage, at, kProbe, mtl);
    ASSERT_EQ(got.size(), kProbe);
    EXPECT_TRUE(all_bytes(got.data(), kProbe, 0x6D))
        << "the slice reads 0x" << std::hex << unsigned{got[0]};
}

// N1: what a bind costs -- handle(), note_gpu_bind(), gpu_offset() -- for a
// buffer on its owned storage and for one on a pool slice whose owned
// storage is still read (the case every DISCARD-per-draw constant buffer is
// in while the GPU lags). Reported, not asserted.
TEST(MetalBufferRename, SliceBindCost) {
    auto& device = MetalDevice::instance();
    if (!device.available()) GTEST_SKIP() << "no Metal device";
    if (!device.supports_fences()) GTEST_SKIP() << "no timeline fences";
    void* mtl = device.native_device();
    auto& queue = MetalSubmitQueue::instance();
    queue.drain();

    auto gate = device.create_fence(0);
    ASSERT_NE(gate, nullptr);
    GateGuard guard{gate.get()};
    const std::vector<u8> bytes(256, 0x5E);
    MetalBuffer owned(mtl, bytes.size(), BufferUsage::Uniform, bytes.data());
    MetalBuffer renamed(mtl, bytes.size(), BufferUsage::Uniform, bytes.data());
    MetalBuffer scratch(mtl, kProbe, BufferUsage::Storage);
    ASSERT_TRUE(gate->wait_gpu(1));
    gpu_read(renamed, scratch, 0);
    renamed.update(bytes.data(), 0, bytes.size(), kDiscard);
    (void)queue.submit();  // the owned storage stays read: list 1 is held

    constexpr u32 kBinds = 2000000;
    const auto bind_cost = [&](MetalBuffer& buffer) {
        usize sink = 0;
        const auto start = std::chrono::steady_clock::now();
        for (u32 i = 0; i < kBinds; ++i) {
            sink += reinterpret_cast<usize>(buffer.handle());
            buffer.note_gpu_bind();
            sink += buffer.gpu_offset();
        }
        const auto ns = std::chrono::duration<double, std::nano>(
                            std::chrono::steady_clock::now() - start)
                            .count();
        EXPECT_NE(sink, 0u);
        return ns / kBinds;
    };
    (void)bind_cost(owned);  // warm up
    const double owned_ns = bind_cost(owned);
    const double slice_ns = bind_cost(renamed);
    EXPECT_NE(renamed.handle(), owned.handle());
    std::printf("[bench] bind: owned storage %.1f ns, pool slice %.1f ns "
                "(%u binds each)\n",
                owned_ns, slice_ns, kBinds);
    RecordProperty("owned_ns", std::to_string(owned_ns));
    RecordProperty("slice_ns", std::to_string(slice_ns));
}

// m-c: mid-size renames beside steady small traffic, GPU two lists behind.
// With oversize starting at 1/8 of the bound a 30 MiB rename took a 120 MiB
// slab; two of them and the 16 MiB slabs small renames keep live exceed the
// bound, so every few frames a rename waited. From 1/12 up they take oversize
// slabs and the bound is left to the small traffic.
TEST(MetalBufferRename, MidSizeRenamesBesideSmallTrafficDoNotWait) {
    auto& device = MetalDevice::instance();
    if (!device.available()) GTEST_SKIP() << "no Metal device";
    if (!device.supports_fences()) GTEST_SKIP() << "no timeline fences";
    void* mtl = device.native_device();
    auto& queue = MetalSubmitQueue::instance();
    // Start from an empty pool: everything retired and released.
    retire_lists(mtl, 2);
    for (u32 i = 0; i < 130; ++i) metal_note_presented_buffers();

    auto gate = device.create_fence(0);
    ASSERT_NE(gate, nullptr);
    GateGuard guard{gate.get()};

    constexpr u32 kFrames = 16;
    constexpr usize kBig = 30u * kMiB;
    constexpr usize kSmall = kMiB;
    constexpr u32 kSmallPerFrame = 8;
    MetalBuffer big(mtl, kBig, BufferUsage::Vertex);
    std::vector<std::unique_ptr<MetalBuffer>> smalls;
    for (u32 i = 0; i < kSmallPerFrame; ++i) {
        smalls.push_back(
            std::make_unique<MetalBuffer>(mtl, kSmall, BufferUsage::Vertex));
    }
    MetalBuffer reads(mtl, kFrames * kProbe, BufferUsage::Storage);
    MetalBuffer scratch(mtl, kProbe, BufferUsage::Storage);
    const u64 waits_before = metal_discard_pool_stats().waits;
    // A pool that waits here waits on a list held by this thread's own gate:
    // open it after a while so a failure is a failure, not a hang.
    std::atomic<bool> finished{false};
    std::thread rescuer([&] {
        const auto until =
            std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!finished.load() && std::chrono::steady_clock::now() < until) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (!finished.load()) (void)gate->signal_cpu(1000);
    });
    ThreadGuard rescue_guard{rescuer, finished};
    std::vector<u8> frame(kBig);
    const std::vector<u8> small_bytes(kSmall, 0x5C);
    std::vector<u64> gens(kFrames, 0);
    for (u32 f = 0; f < kFrames; ++f) {
        ASSERT_TRUE(gate->wait_gpu(f + 1));
        std::fill(frame.begin(), frame.end(), static_cast<u8>(0x80 + f));
        big.update(frame.data(), 0, frame.size(), kDiscard);
        gpu_read(big, reads, f * kProbe);
        for (auto& small : smalls) {
            gpu_read(*small, scratch, 0);
            small->update(small_bytes.data(), 0, small_bytes.size(), kDiscard);
        }
        gens[f] = queue.submit();
        if (f >= 2) {  // the GPU stays two lists behind
            (void)gate->signal_cpu(f - 1);  // refused once rescued: fine
            ASSERT_TRUE(queue.wait_for_generation(gens[f - 2]));
        }
        metal_note_presented_buffers();
    }
    const u64 waits = metal_discard_pool_stats().waits - waits_before;
    finished.store(true);
    if (rescuer.joinable()) rescuer.join();
    (void)gate->signal_cpu(kFrames + 1);
    queue.drain();
    EXPECT_EQ(waits, 0u) << "30 MiB renames beside small ones waited";
    const u8* got = static_cast<const u8*>(reads.map());
    ASSERT_NE(got, nullptr);
    for (u32 f = 0; f < kFrames; ++f) {
        EXPECT_TRUE(all_bytes(got + f * kProbe, kProbe,
                              static_cast<u8>(0x80 + f)))
            << "frame " << f;
    }
}

// m-d: the lock-free handle() under fire. One thread binds a pinned slice
// over and over within one recording (the fast path), another keeps trying
// to reclaim its slab, and the API thread appends with NO_OVERWRITE. Every
// reclaim must be refused, and every bind must see the slice with its own
// offset. Run under TSan for the races.
TEST(MetalBufferRename, FastPathBindsRaceReclaimsAndNoOverwriteWrites) {
    auto& device = MetalDevice::instance();
    if (!device.available()) GTEST_SKIP() << "no Metal device";
    void* mtl = device.native_device();
    auto& queue = MetalSubmitQueue::instance();
    queue.drain();

    constexpr usize kBytes = 256u * kKiB;
    constexpr usize kChunk = 64;
    const std::vector<u8> initial(kBytes, 0x01);
    MetalBuffer b(mtl, kBytes, BufferUsage::Vertex, initial.data());
    MetalBuffer scratch(mtl, kProbe, BufferUsage::Storage);
    // Bound in the list being recorded, then renamed in it: the owned storage
    // stays read, and nothing submits the list while the threads run.
    gpu_read(b, scratch, 0);
    const std::vector<u8> renamed(kBytes, 0x02);
    b.update(renamed.data(), 0, renamed.size(), kDiscard);
    void* slice = b.handle();
    const usize slice_offset = b.gpu_offset();

    std::atomic<bool> go{false};
    std::atomic<bool> stop{false};
    std::atomic<u32> torn{0};
    std::atomic<u32> binds{0};
    std::atomic<u32> attempts{0};
    std::atomic<u32> granted{0};
    std::thread binder([&] {
        while (!go.load()) std::this_thread::yield();
        while (!stop.load()) {
            void* storage = b.handle();
            const usize at = b.gpu_offset();
            if (storage != slice || at != slice_offset) ++torn;
            ++binds;
        }
    });
    ThreadGuard binder_guard{binder, stop};
    std::thread reclaimer([&] {
        while (!go.load()) std::this_thread::yield();
        while (!stop.load()) {
            if (metal_discard_pool_reclaim_for_test(slice)) ++granted;
            ++attempts;
        }
    });
    ThreadGuard reclaimer_guard{reclaimer, stop};
    std::vector<u8> want = renamed;
    go.store(true);
    for (usize offset = kProbe; offset + kChunk <= kBytes; offset += kChunk) {
        const std::vector<u8> chunk(kChunk, 0x7E);
        b.update(chunk.data(), offset, chunk.size(), kNoOverwrite);
        std::fill(want.begin() + static_cast<std::ptrdiff_t>(offset),
                  want.begin() + static_cast<std::ptrdiff_t>(offset + kChunk),
                  0x7E);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    stop.store(true);
    if (binder.joinable()) binder.join();
    if (reclaimer.joinable()) reclaimer.join();
    EXPECT_GT(binds.load(), 0u);
    EXPECT_GT(attempts.load(), 0u);
    EXPECT_EQ(granted.load(), 0u) << "a reclaim took a pinned slice's slab";
    EXPECT_EQ(torn.load(), 0u) << "a bind saw other storage or offset";
    const u8* now = static_cast<const u8*>(b.map());
    ASSERT_NE(now, nullptr);
    EXPECT_EQ(b.handle(), slice);
    EXPECT_EQ(std::memcmp(now, want.data(), want.size()), 0);
    queue.drain();
}

// m-e: when no renaming memory can be had at all (here, every allocation
// refused), a plain write waits for the GPU to finish with the buffer and
// then writes. The wait may let the published slice's slab retire, so the
// write settles again first -- here it moves back to the owned storage,
// which the wait retired. Work recorded before keeps the old bytes, work
// after sees the new ones, and nothing is written under a read.
TEST(MetalBufferRename, RefusedRenameWaitsThenWritesWhereNothingReads) {
    auto& device = MetalDevice::instance();
    if (!device.available()) GTEST_SKIP() << "no Metal device";
    void* mtl = device.native_device();
    auto& queue = MetalSubmitQueue::instance();
    queue.drain();

    const std::vector<u8> initial(64u * kKiB, 0x01);
    MetalBuffer b(mtl, initial.size(), BufferUsage::Vertex, initial.data());
    MetalBuffer reads(mtl, 3 * kProbe, BufferUsage::Storage);
    void* owned = b.handle();
    gpu_read(b, reads, 0);  // the owned storage, read
    const std::vector<u8> renamed(initial.size(), 0x02);
    b.update(renamed.data(), 0, renamed.size(), kDiscard);
    void* slice = b.handle();
    const usize slice_offset = b.gpu_offset();
    ASSERT_NE(slice, owned);
    gpu_read(b, reads, kProbe);  // the slice, read
    const u64 hazards_before = metal_buffer_hazard_writes();

    metal_discard_pool_fail_allocations_for_test(true);
    const std::vector<u8> patch(kProbe, 0x03);
    b.update(patch.data(), 0, patch.size(), kPlain);
    metal_discard_pool_fail_allocations_for_test(false);
    EXPECT_EQ(metal_buffer_hazard_writes(), hazards_before);
    // Where the write went: not the slice, whose slab the wait let retire.
    const auto old_slice = gpu_bytes(slice, slice_offset, kProbe, mtl);
    ASSERT_EQ(old_slice.size(), kProbe);
    EXPECT_TRUE(all_bytes(old_slice.data(), kProbe, 0x02))
        << "the write landed in the slice after the wait instead of settling";
    EXPECT_EQ(b.handle(), owned);
    gpu_read(b, reads, 2 * kProbe);
    queue.drain();

    const u8* got = static_cast<const u8*>(reads.map());
    ASSERT_NE(got, nullptr);
    EXPECT_TRUE(all_bytes(got, kProbe, 0x01));
    EXPECT_TRUE(all_bytes(got + kProbe, kProbe, 0x02));
    EXPECT_TRUE(all_bytes(got + 2 * kProbe, kProbe, 0x03));
    const u8* now = static_cast<const u8*>(b.map());
    ASSERT_NE(now, nullptr);
    EXPECT_TRUE(all_bytes(now, kProbe, 0x03));
    EXPECT_TRUE(all_bytes(now + kProbe, initial.size() - kProbe, 0x02));
}

// m-f: a 25 MiB buffer (an oversize rename) DISCARDed every other frame,
// the GPU keeping up. Released after a single idle Present, its slab was
// freed in the frame between and allocated (and zeroed) again at the next
// DISCARD -- milliseconds of RHI time each. It must be reused instead.
TEST(MetalBufferRename, OversizeSlabSurvivesAlternateFrameDiscards) {
    auto& device = MetalDevice::instance();
    if (!device.available()) GTEST_SKIP() << "no Metal device";
    void* mtl = device.native_device();
    auto& queue = MetalSubmitQueue::instance();
    retire_lists(mtl, 2);
    present_until_oversize_idle();  // no oversize slab to start from

    constexpr usize kBytes = 25u * kMiB;
    constexpr u32 kFrames = 12;
    MetalBuffer stream(mtl, kBytes, BufferUsage::Vertex);
    MetalBuffer reads(mtl, kFrames * kProbe, BufferUsage::Storage);
    MetalBuffer scratch(mtl, kProbe, BufferUsage::Storage);
    std::vector<u8> frame(kBytes);
    const u64 created_before = metal_discard_pool_stats().oversize_created;
    u32 discards = 0;
    for (u32 f = 0; f < kFrames; ++f) {
        if (f % 2 == 0) {
            gpu_read(stream, scratch, 0);  // in flight: the DISCARD renames
            std::fill(frame.begin(), frame.end(), static_cast<u8>(0x90 + f));
            stream.update(frame.data(), 0, frame.size(), kDiscard);
            gpu_read(stream, reads, f * kProbe);
            ++discards;
        }
        // Every frame has work of its own, and the GPU keeps up.
        queue.record_fill_buffer(scratch.handle(), scratch.gpu_offset(), 16,
                                 0);
        queue.drain();
        metal_note_presented_buffers();
    }
    const u64 created =
        metal_discard_pool_stats().oversize_created - created_before;
    EXPECT_EQ(discards, kFrames / 2);
    EXPECT_EQ(created, 1u) << "the oversize slab was freed and allocated again "
                              "between DISCARDs";
    const u8* got = static_cast<const u8*>(reads.map());
    ASSERT_NE(got, nullptr);
    for (u32 f = 0; f < kFrames; f += 2) {
        EXPECT_TRUE(all_bytes(got + f * kProbe, kProbe,
                              static_cast<u8>(0x90 + f)))
            << "frame " << f;
    }
}
