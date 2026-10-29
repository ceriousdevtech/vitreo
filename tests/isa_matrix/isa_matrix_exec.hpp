// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64

/// @file isa_matrix_exec.hpp
/// @brief Run one instruction form on three engines and compare.
///
/// The compile probe
/// (isa_matrix_probe.hpp) answers "was this form lowered natively". This
/// answers the two questions that make that lowering worth having:
///
///   P — does the JIT agree with the interpreter, our semantic reference?
///   H — do both agree with real x86, via the Rosetta oracle?
///
/// Seeds are derived from the decoded form, not hand-written: a memory operand
/// has its base/index registers solved so the effective address lands on the
/// oracle's shared scratch page, which is what makes a memory form comparable
/// at all.

#pragma once

#include "isa_form_enum.hpp"
#include "oracle/oracle_client.hpp"

#include <string>
#include <vector>

namespace vitreo::isa_matrix {

/// Why a form is not executed by the sweep. Not executing is a normal outcome
/// for a large part of the ISA; what matters is that the reason is recorded
/// rather than the form silently vanishing from the denominator.
enum class SkipReason {
    None = 0,
    /// Transfers control: the snippet would not fall through to the epilogue.
    ControlFlow,
    /// Privileged, faulting by design, or environment-dependent (CPUID, RDTSC,
    /// syscall, INT, HLT, I/O).
    NotUserDeterministic,
    /// Writes or reads the stack pointer, which the oracle protocol does not
    /// share between the two sides for arbitrary snippets.
    TouchesStack,
    /// RIP-relative or absolute addressing: the effective address cannot be
    /// steered onto the shared scratch page.
    Unaddressable,
    /// A string operation. Its memory operands are implicit in RSI/RDI and,
    /// when REP-prefixed, its length is bounded only by RCX; both make it a
    /// separate contract from a single-form sweep (see the helper allowlist).
    StringOp,
};

[[nodiscard]] const char* skip_reason_name(SkipReason r);

struct ExecResult {
    SkipReason skipped{SkipReason::None};
    /// The interpreter refused or faulted; nothing to compare.
    bool interp_ran{false};
    /// The JIT produced and ran a block covering the whole snippet.
    bool jit_ran{false};
    /// The oracle was available and the snippet executed cleanly on it.
    bool oracle_ran{false};
    /// Empty when the JIT matched the interpreter.
    std::string parity_diff;
    /// Empty when the interpreter matched real hardware.
    std::string hardware_diff;
    /// Set when exactly one of the two engines faulted on this case.
    ///
    /// The sweep used to compare only when BOTH engines completed, so a case
    /// where the interpreter raised #GP and the JIT sailed through was dropped
    /// from the denominator entirely rather than reported. That made every
    /// fault-raising rule in the ISA -- alignment, canonicality, #DE, #UD --
    /// structurally invisible to the parity column, which is how a 32-bit
    /// MOVAPS lowering with no alignment check compared clean on every case.
    std::string fault_diff;
    /// Set when the two engines agree on every flag the SDM defines but
    /// disagree on one it leaves undefined. Not a correctness failure — but
    /// two Vitreo engines differing means guest behaviour can depend on
    /// whether a block was hot, so it is counted rather than discarded.
    std::string undefined_flag_diff;
};

/// Build the seed state for a form: registers solved so any memory operand
/// resolves to the oracle scratch page, plus deterministic non-trivial values
/// in every GPR, XMM, MXCSR and the x87 file. `seed` selects one of several
/// value sets (carries, NaN/inf, boundary immediates).
[[nodiscard]] vitreo::oracle::State seed_state(const FormCase& fc,
                                               unsigned seed);

/// Should this form be executed at all?
[[nodiscard]] SkipReason classify(const FormCase& fc);

/// Run one form under interpreter, JIT and (when available) the oracle.
[[nodiscard]] ExecResult exec_form(const FormCase& fc, unsigned seed);

}  // namespace vitreo::isa_matrix
