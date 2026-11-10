// SPDX-License-Identifier: LGPL-2.1-only
/// @file test_host_fp_environment_scope.cpp
/// @brief math::HostFpEnvironmentScope: a WinAPI handler runs native code
///        under the platform's default FP environment, and the guest's
///        rounding / flush / NaN mode and sticky flags survive the call.
///
/// The default is AAPCS64's (FPCR RMode = nearest, FZ = FIZ = AH = 0) or, on
/// an x86 host, MXCSR RC = nearest with FTZ = DAZ = 0. The guest side is Intel
/// SDM Vol. 1 10.2.3 (MXCSR) and 8.1.5 (FCW.RC): a call must leave both as the
/// caller had them.

#include "core/math/host_fenv.hpp"

#include <gtest/gtest.h>

#include <cfenv>
#include <cfloat>
#include <atomic>
#include <thread>

namespace vitreo::math {
namespace {

constexpr u32 kMxcsrDefault = 0x1F80u;
constexpr u32 kMxcsrFtzDaz = kMxcsrDefault | 0x8000u | 0x0040u;
constexpr u32 kRcDown = 0x2000u;  // MXCSR.RC = 01
constexpr u32 kRcChop = 0x6000u;  // MXCSR.RC = 11
constexpr u16 kFcwDefault = 0x037Fu;
constexpr u16 kFcwRoundUp = kFcwDefault | 0x0800u;  // FCW.RC = 10

/// Inexact 1/3 the compiler cannot fold.
void third() {
    volatile float one = 1.0f;
    volatile float three = 3.0f;
    volatile float r = one / three;
    (void)r;
}

/// FLT_MIN / 2 is denormal; FTZ flushes it, the default keeps it.
float half_flt_min() {
    volatile float m = FLT_MIN;
    volatile float two = 2.0f;
    return m / two;
}

/// Leave every test with nothing installed, as a fresh guest thread starts.
class HostFpEnvironmentScopeTest : public ::testing::Test {
protected:
    void SetUp() override { reset(); }
    void TearDown() override { reset(); }
    static void reset() {
        apply_x87_rounding(kFcwDefault);
        apply_sse_rounding(kMxcsrDefault);
        clear_host_fp_exceptions();
        forget_host_rounding();
        (void)std::fesetround(FE_TONEAREST);
#if defined(__aarch64__)
        __builtin_arm_wsr64("fpcr", 0);
#endif
    }
};

TEST_F(HostFpEnvironmentScopeTest, HandlerRunsUnderPlatformDefault) {
    u16 fcw = kFcwDefault;
    u32 mxcsr = kMxcsrFtzDaz | kRcChop;
    apply_sse_rounding(mxcsr);
    ASSERT_EQ(std::fegetround(), FE_TOWARDZERO);
    {
        HostFpEnvironmentScope scope(fcw, mxcsr);
        EXPECT_EQ(std::fegetround(), FE_TONEAREST);
        EXPECT_NE(half_flt_min(), 0.0f) << "denormal flushed inside the host call";
#if defined(__aarch64__)
        const u64 fpcr = __builtin_arm_rsr64("fpcr");
        EXPECT_EQ(fpcr & ((u64{3} << 22) | (u64{1} << 24) | 0x3u), 0u)
            << "FPCR RMode/FZ/AH/FIZ must be clear for native code";
#endif
    }
    EXPECT_EQ(std::fegetround(), FE_TOWARDZERO);
    EXPECT_EQ(half_flt_min(), 0.0f) << "guest FTZ lost after the call";
    // The memo still certifies the guest's MXCSR for the JIT's block guard.
    EXPECT_EQ(*installed_sse_environment_key(), u64{mxcsr});
}

TEST_F(HostFpEnvironmentScopeTest, X87RoundingSurvivesTheCall) {
    u16 fcw = kFcwRoundUp;
    u32 mxcsr = kMxcsrDefault;
    apply_x87_rounding(fcw);
    ASSERT_EQ(std::fegetround(), FE_UPWARD);
    {
        HostFpEnvironmentScope scope(fcw, mxcsr);
        EXPECT_EQ(std::fegetround(), FE_TONEAREST);
    }
    EXPECT_EQ(std::fegetround(), FE_UPWARD);
}

TEST_F(HostFpEnvironmentScopeTest, GuestFlagsKeptHostFlagsDropped) {
    u16 fcw = kFcwDefault;
    u32 mxcsr = kMxcsrDefault;
    apply_sse_rounding(mxcsr);
    clear_host_fp_exceptions();
    third();  // guest SSE: PE
    {
        HostFpEnvironmentScope scope(fcw, mxcsr);
        volatile float big = FLT_MAX;
        volatile float r = big * big;  // host: OE | PE
        (void)r;
    }
    EXPECT_EQ(harvest_sse_exceptions(), 0x0020u)
        << "only the guest's inexact may be reported";

    clear_host_fp_exceptions();
    {
        HostFpEnvironmentScope scope(fcw, mxcsr);
        third();
    }
    EXPECT_EQ(harvest_sse_exceptions(), 0u) << "a host-only inexact leaked";
}

TEST_F(HostFpEnvironmentScopeTest, NestedInstallReappliesCallersLiveMode) {
    // A callback the handler runs (or _controlfp) installs a guest mode; the
    // caller resumes with the mode its own state now selects.
    u16 fcw = kFcwDefault;
    u32 mxcsr = kMxcsrDefault;
    apply_sse_rounding(mxcsr);
    {
        HostFpEnvironmentScope scope(fcw, mxcsr);
        mxcsr = kMxcsrDefault | kRcDown;     // e.g. _controlfp(_RC_DOWN)
        apply_sse_rounding(kMxcsrDefault | kRcChop);  // a nested callback's
    }
    EXPECT_EQ(std::fegetround(), FE_DOWNWARD);
    EXPECT_EQ(*installed_sse_environment_key(), u64{kMxcsrDefault | kRcDown});
}

TEST_F(HostFpEnvironmentScopeTest, ForgetDuringCallReappliesLiveMode) {
    // NtContinue / longjmp load a new MXCSR and forget the memo.
    u16 fcw = kFcwDefault;
    u32 mxcsr = kMxcsrDefault | kRcChop;
    apply_sse_rounding(mxcsr);
    {
        HostFpEnvironmentScope scope(fcw, mxcsr);
        mxcsr = kMxcsrFtzDaz;
        forget_host_rounding();
    }
    EXPECT_EQ(std::fegetround(), FE_TONEAREST);
    EXPECT_EQ(half_flt_min(), 0.0f) << "restored context's FTZ not installed";
    EXPECT_EQ(*installed_sse_environment_key(), u64{kMxcsrFtzDaz});
}

TEST_F(HostFpEnvironmentScopeTest, ScopesNest) {
    u16 fcw = kFcwDefault;
    u32 mxcsr = kMxcsrDefault | kRcChop;
    apply_sse_rounding(mxcsr);
    {
        HostFpEnvironmentScope outer(fcw, mxcsr);
        {
            HostFpEnvironmentScope inner(fcw, mxcsr);
            EXPECT_EQ(std::fegetround(), FE_TONEAREST);
        }
        EXPECT_EQ(std::fegetround(), FE_TONEAREST);
    }
    EXPECT_EQ(std::fegetround(), FE_TOWARDZERO);
}

TEST_F(HostFpEnvironmentScopeTest, CertificateAndExceptionOwnersStayThreadLocal) {
    // C++ [basic.stc.thread]: distinct objects per thread, with storage lasting
    // until thread exit. The JIT borrows the certificate's stable address.
    std::atomic<bool> ready{false};
    std::atomic<bool> release{false};
    const u64* worker_certificate = nullptr;
    bool worker_address_stable = false;
    u16 worker_x87 = 0, worker_sse = 0;
    std::thread worker([&] {
        reset();
        u16 fcw = kFcwRoundUp;
        u32 mxcsr = kMxcsrDefault | kRcChop;
        apply_x87_rounding(fcw);
        third();
        apply_sse_rounding(mxcsr);
        third();
        worker_certificate = installed_sse_environment_key();
        {
            HostFpEnvironmentScope outer(fcw, mxcsr);
            HostFpEnvironmentScope inner(fcw, mxcsr);
            third(); // Host-only inexact must not change either guest owner.
        }
        worker_address_stable = worker_certificate == installed_sse_environment_key()
                                && *worker_certificate == mxcsr;
        ready.store(true, std::memory_order_release);
        ready.notify_one();
        release.wait(false, std::memory_order_acquire);
        worker_x87 = harvest_x87_exceptions();
        worker_sse = harvest_sse_exceptions();
        reset();
    });
    // Start the main thread's guest environment after native thread creation.
    reset();
    const u32 main_mxcsr = kMxcsrFtzDaz | kRcDown;
    apply_sse_rounding(main_mxcsr);
    clear_host_fp_exceptions();
    const u64* main_certificate = installed_sse_environment_key();
    ready.wait(false, std::memory_order_acquire);
    EXPECT_NE(main_certificate, worker_certificate);
    EXPECT_EQ(*main_certificate, main_mxcsr);
    EXPECT_TRUE(worker_address_stable);
    EXPECT_EQ(std::fegetround(), FE_DOWNWARD);
    EXPECT_EQ(harvest_x87_exceptions(), 0u);
    EXPECT_EQ(harvest_sse_exceptions(), 0u);
    EXPECT_EQ(main_certificate, installed_sse_environment_key());
    release.store(true, std::memory_order_release);
    release.notify_one();
    worker.join();
    EXPECT_EQ(worker_x87, 0x0020u);
    EXPECT_EQ(worker_sse, 0x0020u);
}

TEST_F(HostFpEnvironmentScopeTest, FreshThreadKeepsUnclaimedCertificateAndDropsHostFlags) {
    u64 initial_key = 0, final_key = 0;
    u16 x87_flags = 1, sse_flags = 1;
    bool stable_address = false;
    u16 fcw = kFcwDefault;
    u32 mxcsr = kMxcsrDefault;
    HostFpEnvironmentScope main_native(fcw, mxcsr);
    std::thread worker([&] {
        const u64* certificate = installed_sse_environment_key();
        initial_key = *certificate;
        // Runtime startup clears inherited native flags before guest execution.
        clear_host_fp_exceptions();
        {
            HostFpEnvironmentScope scope(fcw, mxcsr);
            third();
        }
        final_key = *certificate;
        stable_address = certificate == installed_sse_environment_key();
        x87_flags = harvest_x87_exceptions();
        sse_flags = harvest_sse_exceptions();
    });
    worker.join();
    EXPECT_EQ(initial_key, ~u64{0});
    EXPECT_EQ(final_key, ~u64{0});
    EXPECT_TRUE(stable_address);
    EXPECT_EQ(x87_flags, 0u);
    EXPECT_EQ(sse_flags, 0u);
}

#if defined(__aarch64__) && defined(__APPLE__)
TEST_F(HostFpEnvironmentScopeTest, NativeFlagsExcludedFromCallbackOwners) {
    for (const auto restore : {HostFpEnvironmentScope::Restore::Immediately,
                              HostFpEnvironmentScope::Restore::OnGuestFpUse}) {
        for (const bool caller_sse : {false, true}) {
            for (const bool callback_sse : {false, true}) {
                SCOPED_TRACE(::testing::Message() << "restore=" << int(restore)
                             << " caller SSE=" << caller_sse
                             << " callback SSE=" << callback_sse);
                u16 fcw = kFcwRoundUp;
                u32 mxcsr = kMxcsrFtzDaz | kRcDown;
                clear_host_fp_exceptions();
                if (caller_sse) apply_sse_rounding(mxcsr);
                else apply_x87_rounding(fcw);
                __builtin_arm_wsr64("fpsr", FE_INEXACT);
                {
                    HostFpEnvironmentScope native(fcw, mxcsr, restore);
                    __builtin_arm_wsr64("fpsr", FE_OVERFLOW);
                    if (callback_sse) apply_sse_rounding(mxcsr);
                    else apply_x87_rounding(fcw);
                    EXPECT_EQ(std::fetestexcept(FE_ALL_EXCEPT), 0);
                    __builtin_arm_wsr64("fpsr",
                        __builtin_arm_rsr64("fpsr") | FE_INVALID);
                }
                // Intel SDM Vol. 1 8.1.3 / 10.2.3.1: only guest arithmetic
                // contributes to the independent sticky accumulators.
                const u16 expected_sse = (caller_sse ? 0x0020u : 0u) |
                                         (callback_sse ? 0x0001u : 0u);
                const u16 expected_x87 = (caller_sse ? 0u : 0x0020u) |
                                         (callback_sse ? 0u : 0x0001u);
                EXPECT_EQ(harvest_sse_exceptions(), expected_sse);
                EXPECT_EQ(harvest_x87_exceptions(), expected_x87);
            }
        }
    }
}

TEST_F(HostFpEnvironmentScopeTest, ReentrantNativeReturnDropsHostFlags) {
    for (const auto restore : {HostFpEnvironmentScope::Restore::Immediately,
                              HostFpEnvironmentScope::Restore::OnGuestFpUse}) {
        for (const bool caller_sse : {false, true}) {
            for (const bool callback_sse : {false, true}) {
                u16 fcw = kFcwRoundUp;
                u32 mxcsr = kMxcsrDefault | kRcChop;
                clear_host_fp_exceptions();
                if (caller_sse) apply_sse_rounding(mxcsr);
                else apply_x87_rounding(fcw);
                {
                    HostFpEnvironmentScope outer(fcw, mxcsr, restore);
                    {
                        HostFpEnvironmentScope inner(fcw, mxcsr);
                        if (callback_sse) apply_sse_rounding(mxcsr);
                        else apply_x87_rounding(fcw);
                        __builtin_arm_wsr64("fpsr", FE_INVALID);
                    }
                    EXPECT_EQ(std::fegetround(), FE_TONEAREST);
                    __builtin_arm_wsr64("fpsr", FE_UNDERFLOW | FE_OVERFLOW);
                }
                EXPECT_EQ(std::fegetround(),
                          caller_sse ? FE_TOWARDZERO : FE_UPWARD);
                EXPECT_EQ(harvest_sse_exceptions(), callback_sse ? 0x0001u : 0u);
                EXPECT_EQ(harvest_x87_exceptions(), callback_sse ? 0u : 0x0001u);
            }
        }
    }
}

TEST_F(HostFpEnvironmentScopeTest, DeferredStatusAccessWaitsForGuestFpUse) {
    const u64 original = __builtin_arm_rsr64("fpsr");
    constexpr int flags[] = {FE_INVALID, FE_DIVBYZERO, FE_OVERFLOW,
                              FE_UNDERFLOW, FE_INEXACT, FE_FLUSHTOZERO};
    for (const bool sse : {false, true}) {
        for (unsigned combination = 0; combination < (1u << 6); ++combination) {
            u16 fcw = kFcwRoundUp;
            u32 mxcsr = kMxcsrFtzDaz | kRcChop;
            clear_host_fp_exceptions();
            if (sse) apply_sse_rounding(mxcsr);
            else apply_x87_rounding(fcw);
            const u64 guest_control = __builtin_arm_rsr64("fpcr");
            __builtin_arm_wsr64("fpsr", FE_INEXACT);
            // QC is outside FE_ALL_EXCEPT and must survive the lazy clear.
            asm volatile("movi v0.16b, #127\n\tsqadd v0.16b, v0.16b, v0.16b"
                         ::: "v0");
            const u64 preserved = __builtin_arm_rsr64("fpsr") &
                                  ~static_cast<u64>(FE_ALL_EXCEPT);
            u64 native_status = preserved;
            for (unsigned bit = 0; bit < 6; ++bit) {
                if (combination & (1u << bit)) native_status |= flags[bit];
            }
            for (unsigned call = 0; call < 4; ++call) {
                {
                    HostFpEnvironmentScope native(
                        fcw, mxcsr, HostFpEnvironmentScope::Restore::OnGuestFpUse);
                    if (call != 0) {
                        EXPECT_EQ(__builtin_arm_rsr64("fpsr"), native_status);
                    }
                    __builtin_arm_wsr64("fpsr", native_status);
                }
                // AAPCS64: FPSR may have any value across native interfaces.
                // Retention verifies that neither boundary clears it eagerly.
                EXPECT_EQ(__builtin_arm_rsr64("fpsr"), native_status);
            }
            if (sse) apply_sse_rounding(mxcsr);
            else apply_x87_rounding(fcw);
            EXPECT_EQ(__builtin_arm_rsr64("fpsr"), preserved);
            EXPECT_EQ(__builtin_arm_rsr64("fpcr"), guest_control);
            EXPECT_EQ(sse ? harvest_sse_exceptions() : harvest_x87_exceptions(),
                      0x0020u);
            EXPECT_EQ(sse ? harvest_x87_exceptions() : harvest_sse_exceptions(), 0u);
        }
    }
    __builtin_arm_wsr64("fpsr", original);
}

TEST_F(HostFpEnvironmentScopeTest, DeferredRestorePreservesEveryRoundingAndFlushMode) {
    for (const bool sse : {false, true}) {
        for (u32 rc = 0; rc < 4; ++rc) {
            for (u32 flush = 0; flush < 4; ++flush) {
                SCOPED_TRACE(::testing::Message() << "SSE=" << sse
                             << " RC=" << rc << " flush=" << flush);
                u16 fcw = static_cast<u16>(kFcwDefault | (rc << 10));
                u32 mxcsr = kMxcsrDefault | (rc << 13) |
                            ((flush & 1) ? 0x0040u : 0u) |
                            ((flush & 2) ? 0x8000u : 0u);
                clear_host_fp_exceptions();
                if (sse) apply_sse_rounding(mxcsr);
                else apply_x87_rounding(fcw);
                const u64 guest_control = __builtin_arm_rsr64("fpcr");
                (void)std::feraiseexcept(FE_INVALID | FE_INEXACT);
                {
                    HostFpEnvironmentScope scope(
                        fcw, mxcsr, HostFpEnvironmentScope::Restore::OnGuestFpUse);
                    EXPECT_EQ(std::fegetround(), FE_TONEAREST);
                    EXPECT_NE(half_flt_min(), 0.0f);
                    (void)std::feraiseexcept(FE_OVERFLOW);
                }
                EXPECT_EQ(std::fegetround(), FE_TONEAREST);
                EXPECT_GT(*installed_sse_environment_key(), u64{0xFFFFFFFFu});
                EXPECT_EQ(sse ? harvest_sse_exceptions() : harvest_x87_exceptions(),
                          0x0021u);
                EXPECT_EQ(sse ? harvest_x87_exceptions() : harvest_sse_exceptions(),
                          0u);
                if (sse) apply_sse_rounding(mxcsr);
                else apply_x87_rounding(fcw);
                EXPECT_EQ(__builtin_arm_rsr64("fpcr"), guest_control);
                if (sse) EXPECT_EQ(*installed_sse_environment_key(), u64{mxcsr});
            }
        }
    }
}

TEST_F(HostFpEnvironmentScopeTest, ConsecutiveNativeCallsNeverReinstallGuestMode) {
    u16 fcw = kFcwRoundUp;
    u32 mxcsr = kMxcsrFtzDaz | kRcChop;
    apply_sse_rounding(mxcsr);
    third();
    u64 native_control = 0;
    for (unsigned call = 0; call < 128; ++call) {
        HostFpEnvironmentScope scope(
            fcw, mxcsr, HostFpEnvironmentScope::Restore::OnGuestFpUse);
        if (call == 0) native_control = __builtin_arm_rsr64("fpcr");
        EXPECT_EQ(__builtin_arm_rsr64("fpcr"), native_control);
        third();
    }
    EXPECT_EQ(__builtin_arm_rsr64("fpcr"), native_control);
    EXPECT_GT(*installed_sse_environment_key(), u64{0xFFFFFFFFu});
    EXPECT_EQ(harvest_sse_exceptions(), 0x0020u);
    EXPECT_EQ(harvest_x87_exceptions(), 0u);
    apply_x87_rounding(fcw);
    EXPECT_EQ(std::fegetround(), FE_UPWARD);
    apply_sse_rounding(mxcsr);
    EXPECT_EQ(std::fegetround(), FE_TOWARDZERO);
    EXPECT_EQ(half_flt_min(), 0.0f);
}

TEST_F(HostFpEnvironmentScopeTest, DeferredRestoreKeepsReentrantAndContextChangeFallback) {
    for (const bool sse : {false, true}) {
        u16 fcw = kFcwRoundUp;
        u32 mxcsr = kMxcsrDefault | kRcChop;
        if (sse) apply_sse_rounding(mxcsr);
        else apply_x87_rounding(fcw);
        {
            HostFpEnvironmentScope outer(
                fcw, mxcsr, HostFpEnvironmentScope::Restore::OnGuestFpUse);
            {
                HostFpEnvironmentScope inner(fcw, mxcsr);
                apply_x87_rounding(kFcwDefault);
                third();
            }
            fcw = kFcwDefault | 0x0400u;
            mxcsr = kMxcsrFtzDaz | kRcDown;
            forget_host_rounding();
        }
        EXPECT_EQ(std::fegetround(), FE_DOWNWARD);
        if (sse) {
            EXPECT_EQ(*installed_sse_environment_key(), u64{mxcsr});
            EXPECT_EQ(half_flt_min(), 0.0f);
        } else {
            EXPECT_NE(half_flt_min(), 0.0f);
            EXPECT_GT(*installed_sse_environment_key(), u64{0xFFFFFFFFu});
        }
        clear_host_fp_exceptions();
    }
}

TEST_F(HostFpEnvironmentScopeTest, HarvestMatchesSdkMasksForBothGuestOwners) {
    const u64 original = __builtin_arm_rsr64("fpsr");
    constexpr int flags[] = {FE_INVALID, FE_DIVBYZERO, FE_OVERFLOW,
                              FE_UNDERFLOW, FE_INEXACT, FE_FLUSHTOZERO};
    // Existing guest sticky-bit mapping: IE, ZE, OE, UE, PE; no host DE.
    constexpr u16 guest_flags[] = {0x0001u, 0x0004u, 0x0008u,
                                   0x0010u, 0x0020u, 0u};
    for (const bool sse : {false, true}) {
        for (unsigned combination = 0; combination < (1u << 6); ++combination) {
            clear_host_fp_exceptions();
            if (sse) apply_sse_rounding(kMxcsrDefault);
            else apply_x87_rounding(kFcwDefault);
            u64 raised = 0;
            u16 expected = 0;
            for (unsigned bit = 0; bit < 6; ++bit) {
                if (combination & (1u << bit)) {
                    raised |= static_cast<u64>(flags[bit]);
                    expected |= guest_flags[bit];
                }
            }
            __builtin_arm_wsr64("fpsr", raised);
            EXPECT_EQ(std::fetestexcept(FE_ALL_EXCEPT), raised);
            const u16 actual = sse ? harvest_sse_exceptions()
                                   : harvest_x87_exceptions();
            EXPECT_EQ(actual, expected) << "SSE=" << sse
                                        << " SDK combination=" << combination;
            EXPECT_EQ(std::fetestexcept(FE_ALL_EXCEPT), 0);
            EXPECT_EQ(sse ? harvest_x87_exceptions() : harvest_sse_exceptions(),
                      0u) << "flags leaked to the other guest accumulator";
        }
    }
    __builtin_arm_wsr64("fpsr", original);
}

TEST_F(HostFpEnvironmentScopeTest, StatusClearMatchesEverySdkExceptionMask) {
    u16 fcw = kFcwDefault;
    u32 mxcsr = kMxcsrDefault;
    const u64 original = __builtin_arm_rsr64("fpsr");
    // Saturating SIMD sets QC, which is outside the SDK's FE_ALL_EXCEPT.
    // Clearing exception flags must preserve it, as feclearexcept does.
    asm volatile("movi v0.16b, #127\n\tsqadd v0.16b, v0.16b, v0.16b"
                 ::: "v0");
    const u64 preserved = __builtin_arm_rsr64("fpsr") &
                          ~static_cast<u64>(FE_ALL_EXCEPT);
    EXPECT_NE(preserved, 0u);
    constexpr int flags[] = {FE_INVALID, FE_DIVBYZERO, FE_OVERFLOW,
                              FE_UNDERFLOW, FE_INEXACT, FE_FLUSHTOZERO};
    for (unsigned combination = 0; combination < (1u << 6); ++combination) {
        u64 raised = 0;
        for (unsigned bit = 0; bit < 6; ++bit) {
            if (combination & (1u << bit)) raised |= static_cast<u64>(flags[bit]);
        }
        __builtin_arm_wsr64("fpsr", preserved | raised);
        clear_host_fp_exceptions();
        const u64 actual = __builtin_arm_rsr64("fpsr");
        EXPECT_EQ(actual, preserved) << "SDK flag combination " << combination;
        // Exercise both optimized boundaries, not only the explicit reset API.
        __builtin_arm_wsr64("fpsr", preserved | raised);
        {
            HostFpEnvironmentScope scope(fcw, mxcsr);
            EXPECT_EQ(__builtin_arm_rsr64("fpsr"), preserved)
                << "scope entry, SDK flag combination " << combination;
            __builtin_arm_wsr64("fpsr", preserved | raised);
        }
        EXPECT_EQ(__builtin_arm_rsr64("fpsr"), preserved)
            << "scope exit, SDK flag combination " << combination;
    }
    __builtin_arm_wsr64("fpsr", original);
}
#endif

}  // namespace
}  // namespace vitreo::math
