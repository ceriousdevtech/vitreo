// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file host_fenv.hpp
/// @brief Bridge between the guest x87 control/status words and the host FPU.
///
/// Two guest-visible behaviours were missing, and both would be ruinous to
/// implement per-operation:
///
///   * **Rounding control.** The guest's RC field has to change how arithmetic
///     rounds. Rounding every result by hand costs work on every single FP op.
///   * **Sticky exception flags.** `_statusfp`, `FNSTSW` and friends report
///     which exceptions have occurred since the last clear. Computing that per
///     operation costs work on every single FP op too.
///
/// Neither needs to be. The host FPU already does both jobs for free: it
/// rounds according to its own mode, and it accumulates IEEE sticky flags as a
/// side effect of ordinary arithmetic (on arm64 these live in FPSR, which the
/// JIT's native lowerings feed just by executing). So instead of touching the
/// hot path:
///
///   * **configure** the host rounding mode when the guest changes its control
///     word -- FLDCW, FINIT, FRSTOR, `_controlfp`;
///   * **harvest** the host's sticky flags only when the guest actually looks
///     -- FNSTSW, FNSTENV, FNSAVE, `_statusfp`, `_clearfp`.
///
/// That is the same shape as the lazy EFLAGS machinery, and it makes the
/// interpreter and the JIT native path agree by construction: both run on the
/// same host FPU, so neither can drift from the other.
///
/// The flags are per-thread, as the C standard requires, so one guest thread
/// cannot see another's.

#include <vitreo/common.hpp>

namespace vitreo::math {

/// Point the host FPU at the rounding mode the x87 control word selects.
/// Bits 10-11: 00 nearest, 01 down, 10 up, 11 toward zero.
///
/// x86 has TWO independent rounding-control fields -- x87's FCW.RC and SSE's
/// MXCSR.RC -- and the host has one FPCR. Installing "the" guest rounding mode
/// once is therefore wrong: an x87 FLDCW that selects chop would silently
/// re-round every later SSE operation, which on real silicon it cannot.
///
/// So the mode is installed PER INSTRUCTION CLASS, at the point that class
/// executes: call this before x87 arithmetic and apply_sse_rounding before SSE
/// arithmetic. Both memoise on the mode actually installed, so the steady state
/// (a run of same-class ops) costs a load, a compare and a not-taken branch.
void apply_x87_rounding(u16 control_word) noexcept;

/// Point the host FPU at the rounding mode MXCSR selects (bits 14:13, same
/// 0..3 encoding as FCW.RC) and at its denormal handling (FTZ bit 15, DAZ
/// bit 6). The SSE counterpart of apply_x87_rounding, which clears the flush
/// bits because x87 never flushes.
void apply_sse_rounding(u32 mxcsr) noexcept;

/// True when apply_sse_rounding installs FPCR.AH in every MXCSR mode (the host
/// has FEAT_AFP and VITREO_JIT_SSE_NAN_GATE is not set). ARM ADD/SUB/MUL then
/// produce exactly x86's NaN results (Intel SDM Vol. 1 4.8.3.5): the first
/// NaN operand quieted, and the negative default NaN for an invalid
/// operation. Code that has had the SSE environment installed may use them
/// with no NaN fix-up. Constant for the process.
[[nodiscard]] bool sse_alternate_nan_handling() noexcept;

/// Read-only certificate for this host thread's installed SSE environment.
/// Equals the zero-extended full MXCSR only after apply_sse_rounding completes;
/// otherwise it is outside the u32 range. Invalidated by x87 installation and
/// forget_host_rounding. Borrow only on the executing thread, while it lives.
/// Equality permits skipping apply_sse_rounding, including its ownership check.
[[nodiscard]] const u64* installed_sse_environment_key() noexcept;

/// Drop the memo of which mode is installed, forcing the next apply_* call to
/// write FPCR. For anything that changes the host mode behind this file's back
/// (a fresh thread, a restored context).
void forget_host_rounding() noexcept;

/// Report and clear the sticky exceptions attributed to the **x87** class.
///
/// Returns the bits in x87 status-word layout: IE(0) ZE(2) OE(3) UE(4) PE(5).
/// DE(1) has no portable host equivalent and is never reported.
///
/// x86 has two independent sticky accumulators, FSW and MXCSR, and the host
/// has one. They are kept apart by attributing the host's flags to whichever
/// class was running at each apply_*_rounding transition — see the commentary
/// in host_fenv.cpp. Taking the host's flags directly, as this function used
/// to, gives x87 credit for SSE's rounding and vice versa.
[[nodiscard]] u16 harvest_x87_exceptions() noexcept;

/// The MXCSR counterpart. Returns the same five bits in the same positions
/// (MXCSR: IE 0, DE 1, ZE 2, OE 3, UE 4, PE 5), ready to OR into MXCSR's low
/// bits. Call it wherever the guest can OBSERVE MXCSR — STMXCSR, FXSAVE/XSAVE
/// and the CRT's _statusfp.
[[nodiscard]] u16 harvest_sse_exceptions() noexcept;

/// Drop the SSE class's pending flags without reporting them. LDMXCSR writes
/// the sticky bits outright, so anything accumulated before it is superseded.
void discard_sse_exceptions() noexcept;

/// Drop the host's accumulated flags without reporting them (FNCLEX/_clearfp).
void clear_host_fp_exceptions() noexcept;

/// The host FP environment for one host call (a WinAPI handler).
///
/// The guest's rounding and flush modes live in the host FPCR (MXCSR on an
/// x86 host) between guest instructions, and the SSE class installs FPCR.AH
/// in every mode (see host_fenv.cpp). Native code -- Security.framework,
/// AppKit, Metal, libc -- is compiled for the platform's default environment
/// (AAPCS64 treats FPCR's mode fields as global state no callee may assume
/// changed), and macOS certificate trust evaluation measurably fails under
/// AH = 1. So the handler runs under the host default, and the guest's
/// environment is put back for the guest's next instruction, the way a
/// Windows API leaves the caller's MXCSR and x87 control word as they were.
///
/// Construction folds the guest's pending host flags into their class, saves
/// the installed environment, installs the host default and forgets the memo,
/// so guest code a handler runs (a callback) installs its own mode afresh.
/// Destruction drops the flags host code raised and restores the saved
/// environment exactly; if anything installed a guest mode meanwhile (a
/// nested callback, _controlfp), it re-installs the mode @p control_word /
/// @p mxcsr select for the class that owned the environment before the call.
/// Both are read at destruction: a handler may change them.
class HostFpEnvironmentScope {
public:
    HostFpEnvironmentScope(const u16& control_word, const u32& mxcsr) noexcept;
    ~HostFpEnvironmentScope();
    HostFpEnvironmentScope(const HostFpEnvironmentScope&) = delete;
    HostFpEnvironmentScope& operator=(const HostFpEnvironmentScope&) = delete;

private:
    const u16& m_control_word;
    const u32& m_mxcsr;
    u64 m_saved_control{0};   ///< FPCR (arm64) / MXCSR (x86_64)
    int m_saved_rc{-1};
    u32 m_saved_flush{0xFFFFFFFFu};
    u64 m_saved_sse_key{~u64{0}};
    u8 m_saved_owner{0};
    u32 m_epoch{0};
    bool m_wrote_control{false};
};

} // namespace vitreo::math
