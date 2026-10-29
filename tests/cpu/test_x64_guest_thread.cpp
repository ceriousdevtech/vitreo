// SPDX-License-Identifier: LGPL-2.1-only

/// @file test_x64_guest_thread.cpp
/// @brief The GS-swapped guest thread and its handoff to a host servicer.
///
/// These are the tests that decide whether native Win64 execution is possible
/// on macOS/x86_64 at all. `X64SegmentsTest.HostGsBaseCannotBeRestored`
/// establishes that a thread entering guest GS can never run host libc again;
/// what remains is whether such a thread can still get host work done by
/// handing it to a thread that has its TLS -- and, critically, whether the
/// host servicer is genuinely unaffected, since it is where every WinAPI thunk
/// will run.

#include <gtest/gtest.h>

#include "core/cpu/x64/x64_guest_thread.hpp"
#include "core/cpu/x64/x64_segments.hpp"

#include <sys/mman.h>

#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

using namespace vitreo;
using namespace vitreo::cpu::x64;

namespace {

/// A TEB must be reachable through a descriptor, so it has to live below 4GB.
constexpr u64 kTebAddr = 0x30000000ULL;
/// Offset of NT_TIB.Self in a Win64 TEB.
constexpr u64 kTebSelfOffset = 0x30;

u64* map_fake_teb() {
    void* p = mmap(reinterpret_cast<void*>(kTebAddr), 0x1000,
                   PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
    if (p == MAP_FAILED) return nullptr;
    auto* teb = static_cast<u64*>(p);
    teb[kTebSelfOffset / sizeof(u64)] = kTebAddr;
    return teb;
}

/// Shared between the guest thread and the test. Plain data: the guest side
/// may not allocate.
struct Shared {
    volatile u64 teb_self_seen{0};   ///< fs:[0x30] read through TEB carrier
    volatile u64 teb_self_after{0};  ///< the same, after a host round trip
    volatile u64 hops_done{0};
    volatile u64 body_entered{0};
    u64 hops_requested{0};
};

/// Read the guest TEB self-pointer through FS. One instruction, no libc.
inline u64 read_fs_teb_self() noexcept {
    u64 v = 0;
    __asm__ volatile("movq %%fs:0x30, %0" : "=r"(v));
    return v;
}

/// Guest-side body. LIBC-FREE: no allocation, no printf, no errno, no
/// thread_local. Only atomics, inline asm, and plain memory.
void guest_body(GuestHostHop& hop, void* user) {
    auto* sh = static_cast<Shared*>(user);
    sh->body_entered = 1;
    sh->teb_self_seen = read_fs_teb_self();

    for (u64 i = 0; i < sh->hops_requested; ++i) {
        hop.guest_request();
        sh->hops_done = i + 1;
        // GS must still be the guest's after the host ran arbitrary libc code
        // on its own thread. If servicing perturbed this thread's segment
        // state, this read is where it shows up.
        sh->teb_self_after = read_fs_teb_self();
    }
}

} // namespace

class X64GuestThreadTest : public ::testing::Test {
protected:
    void SetUp() override {
        teb_ = map_fake_teb();
        ASSERT_NE(teb_, nullptr) << "could not map a TEB below 4GB";
    }
    void TearDown() override {
        if (teb_ != nullptr) munmap(teb_, 0x1000);
    }
    u64* teb_{nullptr};
};

/// The whole design in one test: guest GS holds the guest TEB, the guest hands
/// work to the host, the host does it with full libc, and the guest carries on.
TEST_F(X64GuestThreadTest, GuestThreadHopsToHostAndBack) {
    Shared sh{};
    sh.hops_requested = 64;

    GuestTebThread thread;
    ASSERT_TRUE(thread.start(kTebAddr, &guest_body, &sh).is_ok());

    u64 serviced = 0;
    std::string scratch;
    const bool clean = thread.serve([&] {
        // Deliberately libc-heavy: this stands in for a WinAPI thunk, which is
        // ordinary host C++. It must be safe here even though the requesting
        // thread could not run a line of it.
        ++serviced;
        scratch = "serviced " + std::to_string(serviced);
        auto* block = std::malloc(128);
        ASSERT_NE(block, nullptr);
        std::memset(block, 0xAB, 128);
        std::free(block);
    }, /*timeout_ns=*/5'000'000'000ULL);

    EXPECT_TRUE(clean) << "host wait timed out; guest thread is wedged";
    EXPECT_EQ(sh.body_entered, 1u);
    EXPECT_EQ(serviced, sh.hops_requested);
    EXPECT_EQ(sh.hops_done, sh.hops_requested);
    EXPECT_EQ(thread.hop().serviced(), sh.hops_requested);
    EXPECT_TRUE(thread.wait_until_exited());
}

/// The guest TEB really is what GS resolves to on the guest thread -- not a
/// stale host value that happened to read back plausibly.
TEST_F(X64GuestThreadTest, GuestSeesItsOwnTebThroughGs) {
    Shared sh{};
    sh.hops_requested = 1;

    GuestTebThread thread;
    ASSERT_TRUE(thread.start(kTebAddr, &guest_body, &sh).is_ok());
    const bool clean = thread.serve([] {}, 5'000'000'000ULL);

    ASSERT_TRUE(clean);
    EXPECT_EQ(sh.teb_self_seen, kTebAddr);
    EXPECT_EQ(sh.teb_self_after, kTebAddr)
        << "guest GS did not survive a host round trip";
    EXPECT_TRUE(thread.wait_until_exited());
}

/// The host thread keeps its own TLS throughout. If servicing a guest request
/// disturbed the host's GS, this is where it would surface -- and it is the
/// property that lets the shared WinAPI implementation run unmodified.
TEST_F(X64GuestThreadTest, HostTlsSurvivesServicing) {
    u64 host_tls_before = 0;
    __asm__ volatile("movq %%gs:0x00, %0" : "=r"(host_tls_before));
    ASSERT_NE(host_tls_before, 0u);

    Shared sh{};
    sh.hops_requested = 32;

    GuestTebThread thread;
    ASSERT_TRUE(thread.start(kTebAddr, &guest_body, &sh).is_ok());

    std::vector<u64> tls_samples;
    const bool clean = thread.serve([&] {
        u64 v = 0;
        __asm__ volatile("movq %%gs:0x00, %0" : "=r"(v));
        tls_samples.push_back(v);
    }, 5'000'000'000ULL);

    ASSERT_TRUE(clean);
    ASSERT_EQ(tls_samples.size(), sh.hops_requested);
    for (const u64 v : tls_samples) EXPECT_EQ(v, host_tls_before);

    u64 host_tls_after = 0;
    __asm__ volatile("movq %%gs:0x00, %0" : "=r"(host_tls_after));
    EXPECT_EQ(host_tls_after, host_tls_before);
    EXPECT_TRUE(thread.wait_until_exited());
}

/// Two guest threads run concurrently against their own hops. Steam is heavily
/// threaded, so a design that only works for one guest thread is not a design.
TEST_F(X64GuestThreadTest, TwoGuestThreadsRunConcurrently) {
    Shared a{};
    Shared b{};
    a.hops_requested = 16;
    b.hops_requested = 16;

    GuestTebThread ta;
    GuestTebThread tb;
    ASSERT_TRUE(ta.start(kTebAddr, &guest_body, &a).is_ok());
    ASSERT_TRUE(tb.start(kTebAddr, &guest_body, &b).is_ok());

    // Each guest thread needs its own servicer; one host thread serving both
    // would deadlock the moment it blocked on the wrong one.
    std::thread hb([&] { (void)tb.serve([] {}, 5'000'000'000ULL); });
    const bool clean_a = ta.serve([] {}, 5'000'000'000ULL);
    hb.join();

    EXPECT_TRUE(clean_a);
    EXPECT_EQ(a.hops_done, a.hops_requested);
    EXPECT_EQ(b.hops_done, b.hops_requested);
    EXPECT_TRUE(ta.wait_until_exited());
    EXPECT_TRUE(tb.wait_until_exited());
}

/// A guest thread that never asks for anything must not hang its servicer
/// forever -- the timeout is the only way to notice, since it cannot be
/// signalled.
TEST_F(X64GuestThreadTest, WedgedGuestIsReportedRatherThanHangingForever) {
    GuestHostHop hop;
    EXPECT_EQ(hop.host_wait(50'000'000ULL), GuestHostHop::Wait::TimedOut);
}
