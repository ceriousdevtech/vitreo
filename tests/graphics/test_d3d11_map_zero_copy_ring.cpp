// SPDX-License-Identifier: LGPL-2.1-only

/// @file test_d3d11_map_zero_copy_ring.cpp
/// @brief WS6: BufferShadow and the zero-copy buffer ring (D3D11BufferRing)
///        on the real Metal device.
///
/// The ring's one hard rule: a slice recorded GPU work may still use is
/// never handed out again before the generation it retired in has
/// completed (Learn D3D11_MAP_WRITE_DISCARD: the GPU keeps reading the old
/// contents for work already queued). Every GPU test holds the queue behind
/// a timeline fence while the CPU runs ahead, and checks what each queued
/// read saw once it ran.

#include <gtest/gtest.h>

#include "core/memory/guest_memory.hpp"
#include "graphics/contracts/backend_device.hpp"
#include "graphics/metal/metal_device.hpp"
#include "graphics/metal/metal_submit_queue.hpp"
#include "winapi/graphics/d3d11/d3d11_buffer_ring.hpp"
#include "winapi/graphics/d3d11/d3d11_cpu_mapping.hpp"

#include "../winapi/d3d11/d3d11_test_backend.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include <set>
#include <thread>
#include <utility>
#include <vector>

namespace vitreo::winapi {
namespace {

using graphics::BackendBuffer;
using graphics::BackendBufferDesc;
using graphics::BackendBufferWindow;
using graphics::MetalDevice;
using graphics::MetalSubmitQueue;
using Ring = D3D11BufferRing;

constexpr usize kKiB = 1024;

// -- BufferShadow ------------------------------------------------------------

TEST(BufferShadow, OwnedBehavesLikeTheVector) {
    BufferShadow s;
    EXPECT_TRUE(s.empty());
    s.resize(6);
    EXPECT_EQ(s.size(), 6u);
    s[5] = 9;
    EXPECT_EQ(s.data()[5], 9);
    EXPECT_FALSE(s.is_view());
    const std::vector<u8> copy = s;
    EXPECT_EQ(copy.size(), 6u);
    EXPECT_EQ(copy[5], 9);
}

TEST(BufferShadow, ViewFollowsTheSlotItIsGiven) {
    u8 a[16];
    u8 b[16];
    std::memset(a, 1, sizeof(a));
    std::memset(b, 2, sizeof(b));
    u8* slot = a;
    BufferShadow s;
    s.resize(4);
    s.view(&slot, 16);
    EXPECT_TRUE(s.is_view());
    EXPECT_EQ(s[0], 1);
    slot = b;
    EXPECT_EQ(s[0], 2) << "a rename moves every reader with it";
    EXPECT_EQ(s.size(), 16u);
    s.resize(4);
    EXPECT_EQ(s.size(), 16u) << "a view keeps the slice's size";
    s[3] = 0x7F;
    EXPECT_EQ(b[3], 0x7F) << "writes land in the slice";
}

TEST(BufferShadow, CopyAssignmentCopiesIntoAView) {
    BufferShadow src;
    src.resize(16);
    std::memset(src.data(), 7, 16);
    u8 b[16]{};
    u8* slot = b;
    BufferShadow dst;
    dst.view(&slot, 16);
    dst = src;
    for (u8 v : b) EXPECT_EQ(v, 7);
    BufferShadow small;
    small.resize(8);
    std::memset(small.data(), 9, 8);
    dst = small;
    for (u8 v : b) EXPECT_EQ(v, 7) << "a size mismatch changes nothing";
}

TEST(BufferShadow, CopyConstructionOwnsItsBytes) {
    u8 a[8];
    u8 b[8];
    std::memset(a, 3, sizeof(a));
    std::memset(b, 4, sizeof(b));
    u8* slot = a;
    BufferShadow view;
    view.view(&slot, 8);
    const BufferShadow copy(view);
    slot = b;
    EXPECT_FALSE(copy.is_view());
    EXPECT_EQ(copy.size(), 8u);
    EXPECT_EQ(copy[0], 3);
}

// -- Kill switch -------------------------------------------------------------

TEST(D3D11BufferRing, KillSwitchParsesOnlyZeroAsOff) {
    EXPECT_TRUE(d3d11_ws6_parse(nullptr));
    EXPECT_FALSE(d3d11_ws6_parse("0"));
    EXPECT_TRUE(d3d11_ws6_parse("1"));
    EXPECT_TRUE(d3d11_ws6_parse(""));
    EXPECT_TRUE(d3d11_ws6_parse("00"));
    EXPECT_TRUE(d3d11_ws6_parse("off"));
    d3d11_ws6_override_for_test(false);
    EXPECT_FALSE(d3d11_ws6_enabled());
    d3d11_ws6_override_for_test(true);
    EXPECT_TRUE(d3d11_ws6_enabled());
    d3d11_ws6_override_for_test(std::nullopt);
}

// -- The ring on the Metal device -------------------------------------------

class D3D11BufferRingTest : public ::testing::Test {
protected:
    void SetUp() override {
        if (!device().available()) GTEST_SKIP() << "no Metal device";
        MetalSubmitQueue::instance().drain();
    }
    void TearDown() override {
        if (gate) (void)gate->signal_cpu(~0ull >> 1);
        MetalSubmitQueue::instance().drain();
    }
    static MetalDevice& device() { return MetalDevice::instance(); }
    std::shared_ptr<Ring> make_ring(bool is_64bit = true) {
        return std::make_shared<Ring>(memory, device(), is_64bit);
    }
    std::shared_ptr<D3D11RingLease> lease(Ring& ring, usize bytes) {
        std::shared_ptr<D3D11RingLease> out;
        EXPECT_EQ(ring.lease(bytes, out), Ring::LeaseStatus::Ok);
        return out;
    }
    /// A window buffer attached to @p lease. Owned by the fixture, so it
    /// outlives every lease in the test body (attach's contract: the
    /// backend buffer outlives the lease).
    BackendBuffer* window_for(D3D11RingLease& lease) {
        BackendBufferDesc desc{};
        desc.byte_width = lease.byte_width();
        desc.constant_buffer = lease.byte_width() <= 65536;
        desc.window = true;
        auto buffer = device().create_buffer(desc, nullptr);
        EXPECT_NE(buffer, nullptr);
        if (buffer) EXPECT_TRUE(lease.attach(*buffer));
        windows.push_back(std::move(buffer));
        return windows.back().get();
    }
    std::unique_ptr<BackendBuffer> plain(usize bytes) {
        BackendBufferDesc desc{};
        desc.byte_width = bytes;
        desc.shader_resource = true;
        return device().create_buffer(desc, nullptr);
    }
    /// Everything up to now, and the generation after it (kRetireMargin),
    /// has completed.
    void retire_generations() {
        auto a = plain(256);
        auto b = plain(256);
        for (int i = 0; i < 3; ++i) {
            ASSERT_TRUE(device().copy_buffer_region(b.get(), 0, a.get(), 0, 16));
            MetalSubmitQueue::instance().drain();
        }
    }
    /// Everything recorded from here waits on the gate.
    void close_gate() {
        ASSERT_TRUE(device().supports_fences());
        gate = device().create_fence(0);
        ASSERT_NE(gate, nullptr);
        ASSERT_TRUE(gate->wait_gpu(1));
        (void)MetalSubmitQueue::instance().submit();
    }
    std::thread release_gate_later(std::atomic<bool>& released,
                                   int milliseconds = 300) {
        return std::thread([this, &released, milliseconds] {
            std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
            released.store(true);
            (void)gate->signal_cpu(1);
        });
    }

    GuestMemory memory;
    std::shared_ptr<graphics::BackendFence> gate;
    std::vector<std::unique_ptr<BackendBuffer>> windows;
};

TEST_F(D3D11BufferRingTest, SlicesAre256ByteAlignedAndGuestBytesAreTheGpuBytes) {
    auto ring = make_ring();
    auto a = lease(*ring, 100);
    auto b = lease(*ring, 100);
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    EXPECT_EQ(a->guest() % 256, 0u);
    EXPECT_EQ(b->guest() % 256, 0u) << "pData is at least 16-byte aligned";
    EXPECT_NE(a->guest(), b->guest());
    EXPECT_EQ(a->window().offset % 256, 0u);
    const u8 value = 0x5A;
    ASSERT_TRUE(memory.write_bytes(a->guest() + 99, &value, 1).is_ok());
    EXPECT_EQ(a->host()[99], 0x5A) << "the guest writes the host slice";
    auto window = window_for(*a);
    auto readback = plain(256);
    ASSERT_TRUE(device().copy_buffer_region(readback.get(), 0, window, 0,
                                            100));
    MetalSubmitQueue::instance().drain();
    u8 got[100]{};
    ASSERT_TRUE(readback->read(0, got, sizeof(got)));
    EXPECT_EQ(got[99], 0x5A) << "the GPU reads what the guest wrote";
}

TEST_F(D3D11BufferRingTest, SmallSlicesShareSlabsLargeOnesGetTheirOwnHeap) {
    auto ring = make_ring();
    std::vector<std::shared_ptr<D3D11RingLease>> held;
    for (int i = 0; i < 16; ++i) held.push_back(lease(*ring, 64 * kKiB));
    EXPECT_EQ(ring->stats().slabs, 1u) << "16 x 64 KiB fill one 1 MiB slab";
    held.push_back(lease(*ring, 64 * kKiB + 1));
    EXPECT_EQ(ring->stats().large_heaps, 1u);
    EXPECT_EQ(ring->stats().large_bytes, 128 * kKiB);
    EXPECT_EQ(ring->stats().slabs, 1u);
}

TEST_F(D3D11BufferRingTest, RingNeverReturnsASliceBeforeItsGenerationCompletes) {
    if (!device().supports_fences()) GTEST_SKIP() << "no timeline fences";
    auto ring = make_ring();
    auto l = lease(*ring, 64 * kKiB);
    ASSERT_NE(l, nullptr);
    auto window = window_for(*l);
    const u32 count =
        static_cast<u32>(ring->small_budget() / (64 * kKiB)) + 64;
    auto readback = plain(static_cast<usize>(count) * 16);
    ASSERT_NE(readback, nullptr);
    close_gate();
    std::atomic<bool> released{false};
    std::thread releaser = release_gate_later(released);
    std::set<std::pair<void*, usize>> handed;
    bool distinct = true;
    for (u32 i = 0; i < count; ++i) {
        for (u32 w = 0; w < 4; ++w) {
            const u32 word = i * 4 + w;
            std::memcpy(l->host() + w * 4, &word, 4);
        }
        ASSERT_TRUE(device().copy_buffer_region(readback.get(), i * 16u,
                                                window, 0, 16));
        if (!released.load() && ring->stats().waits == 0) {
            distinct &=
                handed.insert({l->window().storage, l->window().offset}).second;
        }
        ASSERT_TRUE(l->discard()) << "iteration " << i;
    }
    releaser.join();
    MetalSubmitQueue::instance().drain();
    EXPECT_TRUE(distinct) << "a slice was handed out twice while gated";
    EXPECT_GT(ring->stats().waits, 0u) << "the bound was reached";
    EXPECT_LE(ring->stats().small_bytes, ring->small_budget());
    std::vector<u32> words(static_cast<usize>(count) * 4);
    ASSERT_TRUE(readback->read(0, words.data(), words.size() * 4));
    for (u32 i = 0; i < count; ++i) {
        ASSERT_EQ(words[i * 4], i * 4)
            << "read " << i << " saw a later DISCARD's bytes";
    }
}

TEST_F(D3D11BufferRingTest, PerBufferSliceCapWaits) {
    if (!device().supports_fences()) GTEST_SKIP() << "no timeline fences";
    auto ring = make_ring();
    auto l = lease(*ring, 256);
    auto window = window_for(*l);
    const u32 count = Ring::kMaxSlicesPerBuffer + 8;
    auto readback = plain(static_cast<usize>(count) * 4);
    close_gate();
    std::atomic<bool> released{false};
    std::thread releaser = release_gate_later(released);
    for (u32 i = 0; i < count; ++i) {
        std::memcpy(l->host(), &i, 4);
        ASSERT_TRUE(device().copy_buffer_region(readback.get(), i * 4u,
                                                window, 0, 4));
        ASSERT_TRUE(l->discard());
    }
    releaser.join();
    MetalSubmitQueue::instance().drain();
    EXPECT_GT(ring->stats().waits, 0u);
    EXPECT_LT(ring->stats().small_bytes, ring->small_budget())
        << "the per-buffer bound, not the ring's, made it wait";
    std::vector<u32> words(count);
    ASSERT_TRUE(readback->read(0, words.data(), words.size() * 4));
    for (u32 i = 0; i < count; ++i) ASSERT_EQ(words[i], i) << i;
}

TEST_F(D3D11BufferRingTest, X86GuestSlabsStayBelowTheMaximumUserAddress) {
    memory.set_maximum_user_address(0x7FFEFFFF);
    auto ring = make_ring(/*is_64bit=*/false);
    EXPECT_EQ(ring->small_budget(), Ring::kSmallBudget32);
    EXPECT_EQ(ring->large_budget(), Ring::kLargeBudget32);
    std::vector<std::shared_ptr<D3D11RingLease>> held;
    for (int i = 0; i < 200; ++i) {
        held.push_back(lease(*ring, 64 * kKiB));
        ASSERT_NE(held.back(), nullptr);
        EXPECT_LE(held.back()->guest() + 64 * kKiB, 0x7FFF0000ull);
    }
    held.push_back(lease(*ring, 3 * 64 * kKiB));
    ASSERT_NE(held.back(), nullptr);
    EXPECT_LE(held.back()->guest() + 3 * 64 * kKiB, 0x7FFF0000ull);
}

TEST_F(D3D11BufferRingTest, ReleasedLeaseSliceIsReusedOnlyAfterRetirement) {
    if (!device().supports_fences()) GTEST_SKIP() << "no timeline fences";
    auto ring = make_ring();
    auto a = lease(*ring, 256);
    auto window = window_for(*a);
    auto readback = plain(256);
    const std::pair<void*, usize> slice_a{a->window().storage,
                                          a->window().offset};
    close_gate();
    ASSERT_TRUE(device().copy_buffer_region(readback.get(), 0, window, 0,
                                            16));
    a.reset();  // the buffer is released while its read is queued
    auto b = lease(*ring, 256);
    EXPECT_NE(std::make_pair(b->window().storage, b->window().offset), slice_a)
        << "a queued read still uses the released slice";
    ASSERT_TRUE(gate->signal_cpu(1));
    retire_generations();
    auto c = lease(*ring, 256);
    EXPECT_EQ(std::make_pair(c->window().storage, c->window().offset), slice_a)
        << "a retired slice is reused once its generation completed";
}

TEST_F(D3D11BufferRingTest, UnsupportedBackendLeasesNothing) {
    d3d11_test::TestGraphicsBackend backend;
    auto ring = std::make_shared<Ring>(memory, backend, true);
    std::shared_ptr<D3D11RingLease> out;
    EXPECT_EQ(ring->lease(256, out), Ring::LeaseStatus::Unsupported);
    EXPECT_EQ(out, nullptr);
    EXPECT_FALSE(ring->supported());
}

// Windows caps neither the number of DYNAMIC buffers nor their renames:
// the budget bounds only the bytes no live buffer leases, so CreateBuffer's
// lease always succeeds while the guest window has room.
TEST_F(D3D11BufferRingTest, LeasesPastTheBudgetStillSucceed) {
    auto ring = make_ring(/*is_64bit=*/false);
    ring->set_budgets_for_test(0, 0);
    std::vector<std::shared_ptr<D3D11RingLease>> held;
    for (int i = 0; i < 40; ++i) {
        held.push_back(lease(*ring, 64 * kKiB));
        ASSERT_NE(held.back(), nullptr) << i;
    }
    held.push_back(lease(*ring, 256 * kKiB));
    ASSERT_NE(held.back(), nullptr);
    const auto stats = ring->stats();
    EXPECT_EQ(stats.live_small_bytes, 40 * 64 * kKiB);
    EXPECT_EQ(stats.small_bytes, 3 * Ring::kSlabBytes);
    EXPECT_EQ(stats.live_large_bytes, 256 * kKiB);
    EXPECT_EQ(stats.waits, 0u) << "a new lease never waits for the budget";
}

// A DISCARD past the budget with nothing retiring waits for the buffer's own
// slice and reuses it in place: a wait is a valid outcome of any Map, a
// failure is not.
TEST_F(D3D11BufferRingTest, DiscardWithNothingToRetireWaitsAndReusesItsSlice) {
    if (!device().supports_fences()) GTEST_SKIP() << "no timeline fences";
    auto ring = make_ring(/*is_64bit=*/false);
    ring->set_budgets_for_test(0, 0);
    // Two whole slabs of 64 KiB slices, every one leased: none free.
    std::vector<std::shared_ptr<D3D11RingLease>> held;
    for (int i = 0; i < 32; ++i) held.push_back(lease(*ring, 64 * kKiB));
    auto& l = held.front();
    auto window = window_for(*l);
    auto readback = plain(16);
    std::memset(l->host(), 0x11, 16);
    close_gate();
    ASSERT_TRUE(device().copy_buffer_region(readback.get(), 0, window, 0, 16));
    const GuestAddr before = l->guest();
    std::atomic<bool> released{false};
    std::thread releaser = release_gate_later(released);
    ASSERT_TRUE(l->discard());
    EXPECT_TRUE(released.load())
        << "the slice was reused before the read queued on it ran";
    releaser.join();
    EXPECT_EQ(l->guest(), before) << "no fresh slice: the same one";
    EXPECT_EQ(ring->stats().in_place, 1u);
    std::memset(l->host(), 0x22, 16);
    MetalSubmitQueue::instance().drain();
    std::array<u8, 16> got{};
    ASSERT_TRUE(readback->read(0, got.data(), got.size()));
    for (const u8 b : got) EXPECT_EQ(b, 0x11) << "the queued read saw the new bytes";
}

// A bind is stamped with the generation being recorded when it is noted,
// but its use lands in that list only if nothing submits it first: another
// thread's submit between the stamp and the append puts the use in the next
// list (kTagMargin). The in-place DISCARD must wait for that list too, not
// hand the guest a slice the GPU has yet to read.
TEST_F(D3D11BufferRingTest, InPlaceDiscardWaitsForAUseSplitAcrossASubmit) {
    if (!device().supports_fences()) GTEST_SKIP() << "no timeline fences";
    auto& queue = MetalSubmitQueue::instance();
    auto ring = make_ring(/*is_64bit=*/false);
    ring->set_budgets_for_test(0, 0);
    std::vector<std::shared_ptr<D3D11RingLease>> held;
    for (int i = 0; i < 32; ++i) held.push_back(lease(*ring, 64 * kKiB));
    auto& l = held.front();
    auto window = window_for(*l);
    auto scratch = plain(16);
    auto readback = plain(16);
    std::memset(l->host(), 0x11, 16);
    close_gate();
    // The bind is stamped for the list being recorded (G), and work in G
    // reads the window ...
    const u64 stamped = queue.recording_generation();
    ASSERT_TRUE(device().copy_buffer_region(scratch.get(), 0, window, 0, 16));
    // ... another thread submits G ...
    ASSERT_GE(queue.submit(), stamped);
    // ... and the bound draw is appended to G+1. It reads the window's
    // bytes with no stamp of its own (straight from the ring's slab).
    const u64 use = queue.recording_generation();
    ASSERT_GT(use, stamped);
    ASSERT_TRUE(device().copy_buffer_region(readback.get(), 0,
                                            l->window().storage,
                                            l->window().offset, 16));
    const GuestAddr before = l->guest();
    std::atomic<bool> released{false};
    std::thread releaser = release_gate_later(released);
    ASSERT_TRUE(l->discard());
    EXPECT_GE(queue.completed_generation(), use)
        << "the in-place slice was handed back while the list using it "
           "was still pending";
    releaser.join();
    EXPECT_EQ(l->guest(), before) << "no fresh slice: the same one";
    EXPECT_EQ(ring->stats().in_place, 1u);
    std::memset(l->host(), 0x22, 16);
    queue.drain();
    std::array<u8, 16> got{};
    ASSERT_TRUE(readback->read(0, got.data(), got.size()));
    for (const u8 b : got) EXPECT_EQ(b, 0x11) << "the queued read saw the new bytes";
}

// Without a backend buffer attached the ring waits for everything recorded.
TEST_F(D3D11BufferRingTest, DiscardWithoutAWindowAndNothingFreeWaitsForTheRecording) {
    auto ring = make_ring(/*is_64bit=*/false);
    ring->set_budgets_for_test(0, 0);
    std::vector<std::shared_ptr<D3D11RingLease>> held;
    for (int i = 0; i < 16; ++i) held.push_back(lease(*ring, 64 * kKiB));
    const GuestAddr before = held.front()->guest();
    EXPECT_TRUE(held.front()->discard());
    EXPECT_EQ(held.front()->guest(), before);
    EXPECT_EQ(ring->stats().in_place, 1u);
}

// A window the backend refuses leaves the lease on its slice (the move is
// atomic) and the fresh slice returns unused.
TEST_F(D3D11BufferRingTest, RefusedWindowKeepsTheLeaseOnItsSlice) {
    auto ring = make_ring();
    auto l = lease(*ring, 256);
    // A plain (non-window) buffer refuses every set_window.
    auto refusing = plain(256);
    ASSERT_NE(refusing, nullptr);
    BackendBufferWindow out{};
    const GuestAddr before = l->guest();
    const auto live = ring->stats().live_small_bytes;
    EXPECT_FALSE(l->replace_window(*refusing, out));
    EXPECT_EQ(l->guest(), before);
    EXPECT_EQ(ring->stats().live_small_bytes, live)
        << "the refused slice was returned";
    // The returned slice is handed out again.
    auto other = lease(*ring, 256);
    ASSERT_NE(other, nullptr);
    EXPECT_NE(other->guest(), before);
}

TEST_F(D3D11BufferRingTest, StaleSlabsAreUnmappedFromTheGuest) {
    auto ring = make_ring();
    ring->set_stale_generations_for_test(2);
    auto a = lease(*ring, 256);
    const GuestAddr guest = a->guest();
    EXPECT_TRUE(memory.is_mapped(guest));
    a.reset();
    retire_generations();
    retire_generations();
    auto b = lease(*ring, 64 * kKiB);  // a lease sweeps idle slabs
    ASSERT_NE(b, nullptr);
    EXPECT_FALSE(memory.is_mapped(guest)) << "the idle slab was unmapped";
    EXPECT_EQ(ring->stats().slabs, 1u);
}

}  // namespace
}  // namespace vitreo::winapi
