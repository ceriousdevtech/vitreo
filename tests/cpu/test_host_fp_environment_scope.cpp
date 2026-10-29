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

}  // namespace
}  // namespace vitreo::math
