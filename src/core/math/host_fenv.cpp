// SPDX-License-Identifier: LGPL-2.1-only

/// @file host_fenv.cpp
/// @brief See host_fenv.hpp.

#include "core/math/host_fenv.hpp"

#include <cfenv>
#include <cstdio>

#if defined(__aarch64__)
#include <sys/sysctl.h>
#elif defined(__x86_64__)
#include <xmmintrin.h>
#endif

// Reading and writing the FP environment is exactly what this file is for, so
// the compiler must not assume the default environment and hoist arithmetic
// across these calls.
#if defined(__clang__)
#pragma STDC FENV_ACCESS ON
#endif

namespace vitreo::math {

namespace {

// The host has ONE rounding mode; x86 has TWO independent architectural ones
// (x87 FCW.RC and SSE MXCSR.RC). So the host FPCR is not "the guest's rounding
// mode" -- it is whichever mode the class of the instruction being executed
// needs, installed at the moment that class runs. `install_rc` is the single
// choke point, and it memoises because writing FPCR serialises the FP pipeline
// on arm64.
//
// FLDCW is NOT rare: MSVC's float-to-int helper brackets every conversion with
// a pair of them, and the restoring half usually puts back the mode it started
// from, so the memo hit rate is high.
thread_local int g_applied_rc = -1;
// Advances whenever the installed environment or its memo changes, so a
// HostFpEnvironmentScope can tell whether anything moved it during the call.
thread_local u32 g_environment_epoch = 0;
thread_local u64 g_installed_sse_key = ~u64{0};

// ---------------------------------------------------------------------------
// Which x86 exception accumulator owns the host's flags right now.
//
// The host has ONE sticky accumulator; x86 has TWO (x87 FSW and MXCSR), and
// they are architecturally independent. Harvesting the host's flags into
// whichever one the guest happens to read is therefore wrong in both
// directions: an SSE-only snippet that rounds inexactly would raise PE in the
// x87 status word, where nothing happened. That is a real, measured effect --
// it is why CompareOpts::fsw_mask excludes the six FSW sticky bits by default,
// 138 false diffs, every one `interp=0020 oracle=0000`.
//
// So the flags are ATTRIBUTED at each class transition. apply_x87_rounding and
// apply_sse_rounding are already called before their class's arithmetic
// (because the host has one FPCR for x86's two rounding-control fields), which
// makes them exactly the right hook: whatever the host accumulated since the
// last transition belongs to the class that was running, so fold it there and
// take ownership.
enum class FpOwner : u8 { None, X87, Sse };
thread_local FpOwner g_owner = FpOwner::None;
thread_local u16 g_pending_x87 = 0;
thread_local u16 g_pending_sse = 0;

/// Read and clear the host's sticky flags, in x87 status-word layout.
///
/// MXCSR uses the same bit positions for these five (IE 0, ZE 2, OE 3, UE 4,
/// PE 5), so one conversion serves both. DE (MXCSR bit 1) has no portable host
/// equivalent and is never reported by either.
u16 raw_harvest() noexcept {
    const int raised = std::fetestexcept(FE_ALL_EXCEPT);
    if (raised == 0) return 0;
    u16 sw = 0;
    if (raised & FE_INVALID)   sw |= 0x0001u;   // IE
    if (raised & FE_DIVBYZERO) sw |= 0x0004u;   // ZE
    if (raised & FE_OVERFLOW)  sw |= 0x0008u;   // OE
    if (raised & FE_UNDERFLOW) sw |= 0x0010u;   // UE
    if (raised & FE_INEXACT)   sw |= 0x0020u;   // PE
    (void)std::feclearexcept(FE_ALL_EXCEPT);
    return sw;
}

/// Move whatever the host has accumulated into the owning class's pending set.
void attribute_to_owner() noexcept {
    const u16 f = raw_harvest();
    if (f == 0) return;
    switch (g_owner) {
        case FpOwner::X87: g_pending_x87 |= f; break;
        case FpOwner::Sse: g_pending_sse |= f; break;
        case FpOwner::None:
            // Nothing has claimed the host accumulator yet, so there is no
            // basis for attributing these to one class over the other. Give
            // them to both rather than dropping them: that preserves the
            // pre-existing x87 behaviour exactly (it used to take everything),
            // and in practice both engines install a rounding class before any
            // guest FP instruction, so this path is startup-only. It is also
            // why a host thread must start guest code with the host's flags
            // clear (see run.cpp): anything left there would be reported here.
            g_pending_x87 |= f;
            g_pending_sse |= f;
            break;
    }
}

// ---------------------------------------------------------------------------
// Denormal handling: MXCSR.FTZ (bit 15, flush tiny RESULTS to zero) and
// MXCSR.DAZ (bit 6, treat denormal INPUTS as zero), Intel SDM Vol. 1 10.2.3.
// Guests set both at startup. The x87 and SSE models must honor them, or
// every SSE/AVX result stays denormal where silicon returns signed zero.
//
// The host does it in hardware, per the FPCR, for interpreter C++ arithmetic
// and JIT-emitted ARM alike:
//   FPCR.FZ  (bit 24) flushes denormal inputs AND tiny outputs (AH = 0);
//   FPCR.FIZ (bit 0, FEAT_AFP) flushes denormal inputs only;
//   FPCR.AH  (bit 1, FEAT_AFP) makes FZ flush tiny outputs only.
// So FTZ+DAZ -> FZ|FIZ, DAZ alone -> FIZ and FTZ alone -> FZ|AH, each exact:
// measured against an Intel i9-12900KF (Windows) and the M4 with
// MULSS/ADDSS/CVTSS2SD/CVTSD2SS/MULSD over a denormal times 1, a denormal
// times 2^20 (input kept), FLT_MIN * 0.5 and (1 - 2^-24) * FLT_MIN, which
// both flush under FTZ although it rounds to FLT_MIN.
//
// AH = 1 also changes other results, all toward x86 (measured on the M4):
// FMAX/FMIN return the second operand for a NaN or +-0 pair, the default NaN
// is 0xFFC00000 (x86's QNaN indefinite), and FRECPE/FRSQRTE estimate more
// precisely. None reaches guest-visible SSE semantics: the JIT emits no
// FMAX/FMIN/FRECPE/FRSQRTE for SSE (MIN/MAX are compare+select), its
// indefinite fixups only SET the NaN sign, and the interpreter's selections
// are C++ compares.
//
// And AH = 1 is what makes ARM ADD/SUB/MUL produce x86's NaN results (Intel
// SDM Vol. 1 4.8.3.5, Table 4-7): two NaN operands return the FIRST, quieted,
// whether or not the second signals, and an invalid operation returns the
// negative default NaN, x86's indefinite. With AH clear ARM prefers the
// signalling operand and returns a positive default NaN, which is the whole
// reason the JIT checked every SSE ADD/SUB/MUL result for NaN. So the SSE
// class installs AH in EVERY mode (FZ = flush outputs, FIZ = flush inputs:
// FTZ -> FZ|AH, DAZ -> FIZ|AH, FTZ+DAZ -> FZ|FIZ|AH, neither -> AH, still
// exact against the silicon expectations of X64MxcsrFlushModesMatchX86Silicon)
// and the JIT drops the check when sse_alternate_nan_handling() says so. The
// x87 class clears AH: x87 never flushes, and its NaN rules are not SSE's.
// VITREO_JIT_SSE_NAN_GATE=1 restores AH for FTZ-only alone, and the check.
//
// x87 has no flush mode, so the x87 class installs neither bit. Memoised like
// the rounding mode: writing FPCR serialises the FP pipeline.
thread_local u32 g_applied_flush = 0xFFFFFFFFu;

#if defined(__aarch64__)
bool host_has_fiz() noexcept {
    static const bool has = [] {
        int v = 0;
        size_t len = sizeof(v);
        return ::sysctlbyname("hw.optional.arm.FEAT_AFP", &v, &len, nullptr, 0) == 0 &&
               v != 0;
    }();
    return has;
}
#endif

void report_unsupported_flush(const char* what) noexcept {
    static bool reported = false;
    if (reported) return;
    reported = true;
    std::fprintf(stderr,
                 "[vitreo] UNIMPLEMENTED MXCSR %s: denormals are not flushed for "
                 "this combination\n", what);
}

bool legacy_nan_gate() noexcept {
    static const bool on = [] {
        const char* v = std::getenv("VITREO_JIT_SSE_NAN_GATE");
        return v != nullptr && v[0] == '1';
    }();
    return on;
}

/// @param ftz MXCSR.FTZ   @param daz MXCSR.DAZ
/// @param sse the SSE class owns the environment (it alone takes AH; see above)
void install_flush(bool ftz, bool daz, bool sse) noexcept {
    const u32 key = (ftz ? 2u : 0u) | (daz ? 1u : 0u) | (sse ? 4u : 0u);
    if (g_applied_flush == key) return;
    g_applied_flush = key;
    ++g_environment_epoch;
#if defined(__aarch64__)
    constexpr u64 kFZ = u64{1} << 24;
    constexpr u64 kFIZ = u64{1} << 0;
    constexpr u64 kAH = u64{1} << 1;
    u64 want = 0;
    if (sse && host_has_fiz() && !legacy_nan_gate()) {
        want = kAH | (ftz ? kFZ : 0) | (daz ? kFIZ : 0);
    } else if (ftz && daz) {
        want = kFZ | (host_has_fiz() ? kFIZ : 0);
    } else if (daz) {
        if (host_has_fiz()) want = kFIZ;
        else report_unsupported_flush("DAZ without FTZ (host lacks FEAT_AFP)");
    } else if (ftz) {
        // FEAT_AFP provides AH as well as FIZ.
        if (host_has_fiz()) want = kFZ | kAH;
        else report_unsupported_flush("FTZ without DAZ (host lacks FEAT_AFP)");
    }
    u64 fpcr = __builtin_arm_rsr64("fpcr");
    fpcr = (fpcr & ~(kFZ | kFIZ | kAH)) | want;
    __builtin_arm_wsr64("fpcr", fpcr);
#elif defined(__x86_64__)
    // The host IS x86: its own MXCSR has both bits.
    unsigned csr = _mm_getcsr() & ~0x8040u;
    if (ftz) csr |= 0x8000u;
    if (daz) csr |= 0x0040u;
    _mm_setcsr(csr);
#endif
}

void install_rc(int rc) noexcept {
    if (g_applied_rc == rc) return;
    g_applied_rc = rc;
    ++g_environment_epoch;
    switch (rc) {
        case 1: (void)std::fesetround(FE_DOWNWARD);   break;
        case 2: (void)std::fesetround(FE_UPWARD);     break;
        case 3: (void)std::fesetround(FE_TOWARDZERO); break;
        default: (void)std::fesetround(FE_TONEAREST); break;
    }
}

}  // namespace

void apply_x87_rounding(u16 control_word) noexcept {
    g_installed_sse_key = ~u64{0};
    if (g_owner != FpOwner::X87) {
        attribute_to_owner();
        g_owner = FpOwner::X87;
    }
    install_rc(static_cast<int>((control_word >> 10) & 3u));
    install_flush(false, false, /*sse=*/false);
}

void apply_sse_rounding(u32 mxcsr) noexcept {
    if (g_owner != FpOwner::Sse) {
        attribute_to_owner();
        g_owner = FpOwner::Sse;
    }
    // MXCSR.RC is bits 14:13, and uses the same 0..3 encoding as FCW.RC.
    install_rc(static_cast<int>((mxcsr >> 13) & 3u));
    install_flush((mxcsr & 0x8000u) != 0, (mxcsr & 0x0040u) != 0,
                  /*sse=*/true);
    g_installed_sse_key = mxcsr;
}

bool sse_alternate_nan_handling() noexcept {
#if defined(__aarch64__)
    return host_has_fiz() && !legacy_nan_gate();
#else
    return false;
#endif
}

const u64* installed_sse_environment_key() noexcept {
    return &g_installed_sse_key;
}

void forget_host_rounding() noexcept {
    ++g_environment_epoch;
    g_installed_sse_key = ~u64{0};
    g_applied_rc = -1;
    g_applied_flush = 0xFFFFFFFFu;
}

u16 harvest_x87_exceptions() noexcept {
    attribute_to_owner();
    const u16 out = g_pending_x87;
    g_pending_x87 = 0;
    return out;
}

u16 harvest_sse_exceptions() noexcept {
    attribute_to_owner();
    const u16 out = g_pending_sse;
    g_pending_sse = 0;
    return out;
}

void discard_sse_exceptions() noexcept {
    attribute_to_owner();
    g_pending_sse = 0;
}

void clear_host_fp_exceptions() noexcept {
    (void)std::feclearexcept(FE_ALL_EXCEPT);
    g_pending_x87 = 0;
    g_pending_sse = 0;
}

namespace {

// The control fields install_rc / install_flush write; everything else in the
// register is the platform's and is left alone.
#if defined(__aarch64__)
constexpr u64 kGuestControlMask =
    (u64{3} << 22) |  // RMode
    (u64{1} << 24) |  // FZ
    (u64{1} << 1) |   // AH
    (u64{1} << 0);    // FIZ
u64 read_host_control() noexcept { return __builtin_arm_rsr64("fpcr"); }
void write_host_control(u64 v) noexcept { __builtin_arm_wsr64("fpcr", v); }
// FPSR IOC DZC OFC UFC IXC (bits 0-4) and IDC (bit 7). Read directly: the
// libc fenv calls cost more than the rest of a host-call transition.
bool host_flags_raised() noexcept { return (__builtin_arm_rsr64("fpsr") & 0x9Fu) != 0; }
#elif defined(__x86_64__)
constexpr u64 kGuestControlMask =
    0x6000u |  // RC
    0x8000u |  // FTZ
    0x0040u;   // DAZ
u64 read_host_control() noexcept { return _mm_getcsr(); }
void write_host_control(u64 v) noexcept { _mm_setcsr(static_cast<unsigned>(v)); }
bool host_flags_raised() noexcept { return std::fetestexcept(FE_ALL_EXCEPT) != 0; }
#else
bool host_flags_raised() noexcept { return std::fetestexcept(FE_ALL_EXCEPT) != 0; }
#endif

}  // namespace

HostFpEnvironmentScope::HostFpEnvironmentScope(const u16& control_word,
                                               const u32& mxcsr) noexcept
    : m_control_word(control_word), m_mxcsr(mxcsr) {
    if (host_flags_raised()) attribute_to_owner();
    m_saved_rc = g_applied_rc;
    m_saved_flush = g_applied_flush;
    m_saved_sse_key = g_installed_sse_key;
    m_saved_owner = static_cast<u8>(g_owner);
#if defined(__aarch64__) || defined(__x86_64__)
    m_saved_control = read_host_control();
    m_wrote_control = (m_saved_control & kGuestControlMask) != 0;
    if (m_wrote_control) {
        write_host_control(m_saved_control & ~kGuestControlMask);
    }
#if defined(__x86_64__)
    // fesetround also moved the host x87 control word's RC.
    if (m_saved_rc > 0) (void)std::fesetround(FE_TONEAREST);
#endif
#endif
    forget_host_rounding();
    m_epoch = g_environment_epoch;
}

HostFpEnvironmentScope::~HostFpEnvironmentScope() {
    const bool untouched = g_environment_epoch == m_epoch;
    const auto owner = static_cast<FpOwner>(m_saved_owner);
    if (untouched) {
        // Only host code ran: its flags are not the guest's.
        if (host_flags_raised()) (void)std::feclearexcept(FE_ALL_EXCEPT);
#if defined(__aarch64__) || defined(__x86_64__)
        // Native code leaves FPCR as it found it (AAPCS64), so it still holds
        // what the constructor wrote. Reading it back would wait on that
        // serialising write.
        if (m_wrote_control) write_host_control(m_saved_control);
#if defined(__x86_64__)
        if (m_saved_rc > 0) {
            g_applied_rc = -1;
            install_rc(m_saved_rc);
        }
#endif
#endif
        g_applied_rc = m_saved_rc;
        g_applied_flush = m_saved_flush;
        g_installed_sse_key = m_saved_sse_key;
        g_owner = owner;
        return;
    }
    // Something installed a guest mode during the call; the memo describes
    // it, not the caller's. Re-install the caller's class from its state.
    forget_host_rounding();
    switch (owner) {
        case FpOwner::X87: apply_x87_rounding(m_control_word); break;
        case FpOwner::Sse: apply_sse_rounding(m_mxcsr); break;
        case FpOwner::None:
#if defined(__aarch64__) || defined(__x86_64__)
            write_host_control(m_saved_control);
#endif
            break;
    }
}

} // namespace vitreo::math
