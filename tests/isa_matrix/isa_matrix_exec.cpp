// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64

/// @file isa_matrix_exec.cpp
/// @brief See isa_matrix_exec.hpp.

#include "isa_matrix_exec.hpp"

#include "oracle/oracle_contract.hpp"

#include <cstdlib>
#include <string>
#include <cstring>

namespace vitreo::isa_matrix {

namespace {

using vitreo::cpu::Opcode;
using vitreo::cpu::OperandType;
using vitreo::oracle::State;

constexpr std::uint64_t kScratch = vitreo::oracle::kMemVa;
// Stay clear of the last 16 bytes of the 512-byte comparison window so a
// 16-byte store through a +disp8 form still lands inside what the protocol
// compares.
constexpr std::uint64_t kScratchLimit = kScratch + 512 - 32;

bool in_range(Opcode op, Opcode lo, Opcode hi) {
    const auto u = static_cast<unsigned>(op);
    return u >= static_cast<unsigned>(lo) && u <= static_cast<unsigned>(hi);
}

bool is_control_flow(Opcode op) {
    // The enum lays control transfers out contiguously, JMP through IRETQ.
    // Using the range rather than a list means a newly added branch form is
    // classified correctly the day it is decoded, instead of silently entering
    // the sweep and running off the end of the snippet.
    return in_range(op, Opcode::JMP_REL8, Opcode::IRETQ);
}

bool is_string_op(Opcode op) {
    return in_range(op, Opcode::MOVSB, Opcode::SCASQ);
}

bool is_not_user_deterministic(Opcode op) {
    switch (op) {
    case Opcode::CPUID:
    case Opcode::RDTSC:
    case Opcode::RDTSCP:
    case Opcode::SYSCALL:
    case Opcode::SYSENTER:
    case Opcode::SYSEXIT:
    case Opcode::HLT:
    case Opcode::WBINVD:
    case Opcode::UD2:
    case Opcode::IN_AL_IMM8:
    case Opcode::IN_AL_DX:
    case Opcode::OUT_IMM8_AL:
    case Opcode::OUT_IMM8_EAX:
    case Opcode::OUT_DX_AL:
    case Opcode::OUT_DX_EAX:
    case Opcode::CLI:
    case Opcode::STI:
        return true;
    default:
        return false;
    }
}

/// Does this form read or write RSP, explicitly or implicitly?
///
/// The oracle protocol does not share a stack: it loads RSP only when the
/// incoming value is non-zero, and a snippet that pushes would write memory
/// neither side compares. So a stack instruction cannot be adjudicated by this
/// sweep, and saying so is better than comparing two different stacks.
bool touches_stack(const FormCase& fc) {
    const Opcode op = fc.opcode;
    if (in_range(op, Opcode::LEAVE, Opcode::RET_FAR_IMM16)) return true;
    switch (op) {
    case Opcode::PUSHF:
    case Opcode::PUSHFQ:
    case Opcode::POPF:
    case Opcode::POPFQ:
        return true;
    default:
        break;
    }
    const char* name = vitreo::cpu::opcode_name(op);
    if (name != nullptr &&
        (std::strncmp(name, "PUSH", 4) == 0 ||
         std::strncmp(name, "POP", 3) == 0)) {
        return true;
    }
    // An explicit RSP/ESP operand, or an effective address built on it.
    for (vitreo::u8 i = 0; i < fc.decoded.operand_count; ++i) {
        const auto& o = fc.decoded.operands[i];
        if (vitreo::cpu::is_register_operand(o.type) && o.reg == 4 &&
            o.type != OperandType::Reg128 && o.type != OperandType::RegMM) {
            return true;
        }
        if (vitreo::cpu::is_memory_operand(o.type) &&
            (o.mem.base == 4 || o.mem.index == 4)) {
            return true;
        }
    }
    return false;
}

/// Whether this form has a 128-bit MEMORY operand, and therefore deserves an
/// input class whose effective address is deliberately off 16-byte alignment.
///
/// This used to be a hardcoded list of eleven opcodes -- the MOV*A* family,
/// MOVNT*, FXSAVE/FXRSTOR. That was wrong twice over.
///
/// It was wrong about the RULE: alignment is a property of the OPERAND, not
/// the opcode. Measured on silicon (tools/oracle_probe/sse_align_probe.c), a
/// misaligned m128 faults on XORPS, PXOR, PAND, POR, PADDB, PMULLW, PACKSSWB,
/// PSLLW, PCMPEQB and every other legacy packed form -- not just the aligned
/// moves. None of those were on the list.
///
/// And it was wrong about the CONSEQUENCE: every form off the list had its
/// address solved onto `kScratch`, which is 16-byte aligned, so the sweep
/// could not see an alignment defect on them AT ALL -- in either direction. A
/// lowering that forgot the check compared equal, and so did one that
/// FABRICATED a fault on a VEX form that architecturally permits misalignment.
/// The second is not hypothetical: a `VPXOR xmm2, xmm1, [rdi]` raising a bogus
/// #GP killed Steam at startup, deterministically, and this sweep reported
/// zero fault divergences while it did.
///
/// The predicate is now the operand, so both directions are exercised. The
/// sweep does not need to know which answer is correct -- it compares the two
/// engines, and the interpreter is the reference.
bool has_m128_operand(const DecodedInstruction& d) {
    for (unsigned i = 0; i < d.operand_count; ++i) {
        if (d.operands[i].type == OperandType::Mem128) return true;
    }
    return false;
}

/// Solve the register values that put this form's effective address on the
/// scratch page, offset by `ea_offset` bytes. Returns false when no assignment
/// can.
bool solve_address(const FormCase& fc, State& st, std::int64_t ea_offset = 0) {
    for (vitreo::u8 i = 0; i < fc.decoded.operand_count; ++i) {
        const auto& o = fc.decoded.operands[i];
        if (!vitreo::cpu::is_memory_operand(o.type)) continue;
        const auto& m = o.mem;
        if (m.has_absolute || m.base == 16) return false;  // moffs / RIP-rel
        if (m.base == 255 && m.index == 255) return false; // bare disp

        const std::int64_t target =
            static_cast<std::int64_t>(kScratch) + ea_offset;
        const std::int64_t want = target - m.disp;
        if (want <= 0) return false;
        const std::int64_t scale = m.scale == 0 ? 1 : m.scale;

        if (m.index == 255) {
            st.gpr[m.base] = static_cast<std::uint64_t>(want);
        } else if (m.base == 255) {
            if (want % scale != 0) return false;
            st.gpr[m.index] = static_cast<std::uint64_t>(want / scale);
        } else if (m.base == m.index) {
            const std::int64_t div = 1 + scale;
            if (want % div != 0) return false;
            st.gpr[m.base] = static_cast<std::uint64_t>(want / div);
        } else {
            st.gpr[m.index] = 0;
            st.gpr[m.base] = static_cast<std::uint64_t>(want);
        }
        if (fc.mode == 32) {
            if (st.gpr[m.base] > 0xFFFFFFFFull) return false;
            if (m.index != 255 && st.gpr[m.index] > 0xFFFFFFFFull) return false;
        }
    }
    return true;
}

/// RFLAGS bits the Intel SDM leaves UNDEFINED for this instruction.
///
/// A difference in one of these is not a correctness bug in either engine —
/// hardware itself does not promise a value. It is still worth seeing (two
/// Vitreo engines disagreeing makes a guest's behaviour depend on whether a
/// block was hot), so the sweep counts it separately rather than masking it
/// away. Without the split, roughly a fifth of the parity failures are flags
/// nobody owes, and a ratchet over that number guards noise.
///
/// Sources are the per-instruction "Flags Affected" sections of Intel SDM
/// Vol. 2. Where a flag's definedness depends on the shift count, the
/// conservative (undefined) choice is taken: a count-sensitive mask would need
/// the runtime operand, which a static classification does not have.
std::uint32_t undefined_flags(Opcode op) {
    constexpr std::uint32_t kCF = 0x001, kPF = 0x004, kAF = 0x010;
    constexpr std::uint32_t kZF = 0x040, kSF = 0x080, kOF = 0x800;
    const char* n = vitreo::cpu::opcode_name(op);
    const std::string name = n ? n : "";
    const auto starts = [&](const char* p) {
        return name.rfind(p, 0) == 0;
    };

    // BMI1/BMI2: "AF and PF are undefined" (ANDN, BLSI, BLSMSK, BLSR, BZHI,
    // BEXTR). SF/ZF are written, CF/OF defined per instruction.
    if (starts("ANDN") || starts("BLS") || starts("BZHI") || starts("BEXTR")) {
        return kAF | kPF;
    }
    // BSF/BSR: every flag but ZF is undefined.
    if (starts("BSF") || starts("BSR")) return kCF | kOF | kSF | kAF | kPF;
    // MUL/IMUL: only CF and OF are defined.
    if (starts("MUL") || starts("IMUL")) return kSF | kZF | kAF | kPF;
    // DIV/IDIV: all six undefined.
    if (starts("DIV") || starts("IDIV")) {
        return kCF | kOF | kSF | kZF | kAF | kPF;
    }
    // Logical ops: AF undefined, everything else defined.
    if (starts("AND") || starts("OR_") || starts("XOR") || starts("TEST")) {
        return kAF;
    }
    // Shifts and rotates: OF is defined only for a count of 1, and AF is
    // undefined throughout. The count is a runtime value here.
    if (starts("SHL") || starts("SHR") || starts("SAR") || starts("ROL") ||
        starts("ROR") || starts("RCL") || starts("RCR")) {
        return kOF | kAF;
    }
    // BT/BTS/BTR/BTC: only CF is defined.
    if (starts("BT")) return kOF | kSF | kZF | kAF | kPF;
    // POPCNT / LZCNT / TZCNT write ZF (and CF for LZCNT/TZCNT); the rest are
    // cleared architecturally, so nothing is undefined. AAD/AAM/AAA/AAS/DAA
    // and the x87 compares are the remaining undefined-flag families.
    if (starts("AAA") || starts("AAS") || starts("AAD") || starts("AAM") ||
        starts("DAA") || starts("DAS")) {
        return kCF | kOF | kSF | kZF | kAF | kPF;
    }
    return 0;
}

}  // namespace

const char* skip_reason_name(SkipReason r) {
    switch (r) {
    case SkipReason::None:                 return "run";
    case SkipReason::ControlFlow:          return "control-flow";
    case SkipReason::NotUserDeterministic: return "non-deterministic";
    case SkipReason::TouchesStack:         return "stack";
    case SkipReason::Unaddressable:        return "unaddressable";
    case SkipReason::StringOp:             return "string-op";
    }
    return "?";
}

SkipReason classify(const FormCase& fc) {
    if (is_control_flow(fc.opcode)) return SkipReason::ControlFlow;
    if (is_string_op(fc.opcode)) return SkipReason::StringOp;
    if (is_not_user_deterministic(fc.opcode)) {
        return SkipReason::NotUserDeterministic;
    }
    if (touches_stack(fc)) return SkipReason::TouchesStack;
    State probe{};
    if (!solve_address(fc, probe)) return SkipReason::Unaddressable;
    return SkipReason::None;
}

State seed_state(const FormCase& fc, unsigned seed) {
    State st{};
    st.mode = (fc.mode == 64) ? vitreo::oracle::kMode64
                              : vitreo::oracle::kMode32;
    st.rflags = 0x202;
    st.mxcsr = 0x1F80;
    st.fcw = 0x037F;
    // The x87 stack DEPTH is part of the input, and getting it wrong silently
    // narrows the sweep: seeding every case with a full stack (ftw=0xFF) means
    // every FLD/FILD overflows, so the ordinary push path is never compared and
    // the overflow path accounts for the entire x87 column. Vary it, and put
    // TOP where the tag word says it should be.
    switch (seed % 4) {
    case 0: st.ftw = 0x00; st.fsw = 0x3800; break;  // empty, TOP=7
    case 1: st.ftw = 0x03; st.fsw = 0x3000; break;  // two live, TOP=6
    case 2: st.ftw = 0x0F; st.fsw = 0x2000; break;  // four live, TOP=4
    default: st.ftw = 0xFF; st.fsw = 0x3800; break; // full: overflow path
    }
    // FCW precision control is the recorded divergence family (206 diffs, all
    // PC=24), so one seed selects single precision rather than leaving every
    // case at the double-extended default.
    if (seed % 4 == 2) st.fcw = 0x007F;  // PC=00 (single), RC=nearest

    // GPR seeds. Values are deliberately asymmetric per register: a lowering
    // that swaps two operands, or that reads the wrong half of one, has to
    // produce a different answer rather than the same one by luck. (A sweep
    // seeded with equal operands is how an operand-order bug survives.)
    for (int i = 0; i < 16; ++i) {
        const std::uint64_t base = 0x0F1E2D3C4B5A6900ull + (0x1111u * (i + 1));
        switch (seed % 4) {
        case 0: st.gpr[i] = base; break;
        case 1: st.gpr[i] = (i % 2) ? 0ull : 0xFFFFFFFFFFFFFFFFull; break;
        case 2: st.gpr[i] = 1ull << (i + 3); break;
        default: st.gpr[i] = base ^ 0x8000000080000000ull; break;
        }
    }
    // Carry/overflow boundary: the input class that separates a correct flag
    // lowering from one that is merely plausible.
    if (seed % 4 == 2) {
        st.gpr[0] = 0x7FFFFFFFFFFFFFFFull;
        st.gpr[1] = 1;
    }
    st.gpr[4] = 0;  // RSP stays unloaded on both sides.

    // XMM / x87 seeds, including the FP special classes: a lowering that is
    // right on finite values and wrong on NaN, infinity or a denormal is the
    // recorded failure shape for several of these instructions.
    for (int r = 0; r < 16; ++r) {
        for (int b = 0; b < 16; ++b) {
            st.xmm[r][b] = static_cast<std::uint8_t>(0x10 * (r + 1) + b);
        }
        if (seed % 4 == 1) {
            // +inf / -inf / QNaN / SNaN across the four single-precision lanes.
            static const std::uint8_t kSpecial[16] = {
                0x00, 0x00, 0x80, 0x7F,  // +inf
                0x00, 0x00, 0x80, 0xFF,  // -inf
                0x01, 0x00, 0xC0, 0x7F,  // QNaN
                0x01, 0x00, 0x80, 0x7F,  // SNaN
            };
            std::memcpy(st.xmm[r].data(), kSpecial, 16);
        } else if (seed % 4 == 3) {
            // Smallest normal and a denormal, to exercise FTZ/DAZ handling.
            static const std::uint8_t kDenorm[16] = {
                0x00, 0x00, 0x80, 0x00,  0x01, 0x00, 0x00, 0x00,
                0xFF, 0xFF, 0x7F, 0x00,  0x00, 0x00, 0x80, 0x3F,
            };
            std::memcpy(st.xmm[r].data(), kDenorm, 16);
        }
    }
    for (int r = 0; r < 8; ++r) {
        // A valid double-extended value per ST(i): exponent biased, explicit
        // integer bit set. An x87 form seeded with zeros compares nothing.
        st.st[r] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80,
                    static_cast<std::uint8_t>(0xFF + r), 0x3F};
        st.st[r][8] = static_cast<std::uint8_t>(0xFF - r);
        st.st[r][9] = 0x3F;
    }

    for (std::size_t i = 0; i < st.mem.size(); ++i) {
        st.mem[i] = static_cast<std::uint8_t>((i * 7u + seed * 13u) & 0xFF);
    }

    // One of the four input classes probes an unaligned effective address on
    // the forms where alignment is architectural. Fall back to the aligned
    // solve when the offset makes the address unsolvable (a scaled-index form
    // can need the target to be divisible by the scale).
    const std::int64_t ea_offset =
        (seed == 3u && has_m128_operand(fc.decoded)) ? 1 : 0;
    if (ea_offset == 0 || !solve_address(fc, st, ea_offset)) {
        (void)solve_address(fc, st);
    }
    (void)kScratchLimit;
    return st;
}

ExecResult exec_form(const FormCase& fc, unsigned seed) {
    namespace oc = vitreo::oracle::contract;
    ExecResult r;
    r.skipped = classify(fc);
    if (r.skipped != SkipReason::None) return r;

    const State in = seed_state(fc, seed);
    const std::vector<std::uint8_t> code = fc.bytes;
    const bool is64 = (fc.mode == 64);

    const State interp = is64 ? oc::run_on_interpreter_64(in, code)
                              : oc::run_on_interpreter(in, code);
    r.interp_ran = (interp.status == 0);

    const State jit = is64 ? oc::run_on_jit_64(in, code)
                           : oc::run_on_jit(in, code);
    r.jit_ran = (jit.status == 0);

    oc::CompareOpts opts;
    // The x87 tag/exponent bookkeeping and the sticky FSW exception bits are
    // adjudicated by the dedicated x87 suites, which know when the oracle can
    // answer. Comparing them here would report known Rosetta defects as
    // thousands of matrix failures.
    opts.skip_x87_tags_and_regs = true;

    // Fault parity, before the both-ran gate below drops the case. Status 0
    // is "ran to the end of the snippet" and 5 is "stopped early", which for a
    // straight-line snippet means the guest raised. 6 (setup) and 7 (the JIT
    // declined to translate) are not verdicts about the guest, so only 0-vs-5
    // is adjudicable.
    const bool interp_verdict = interp.status == 0 || interp.status == 5;
    const bool jit_verdict = jit.status == 0 || jit.status == 5;
    if (interp_verdict && jit_verdict && interp.status != jit.status) {
        r.fault_diff = (interp.status == 5)
            ? "interpreter faulted, JIT did not"
            : "JIT faulted, interpreter did not";
    }

    const std::uint32_t undef = undefined_flags(fc.opcode);
    if (r.interp_ran && r.jit_ran) {
        oc::CompareOpts strict = opts;
        strict.flags_mask = oc::kArithFlagsMask & ~undef;
        r.parity_diff = oc::compare(jit, interp, strict);
        if (r.parity_diff.empty() && undef != 0) {
            // Everything the SDM defines matches. Anything left is a flag
            // neither engine owes a value for — recorded, not failed.
            r.undefined_flag_diff = oc::compare(jit, interp, opts);
        }
    }

    // The hardware column costs one oracle subprocess per case (~22 ms), which
    // is ~45 minutes over the whole matrix — far too slow for a per-commit
    // gate, and the parity column alone sweeps the ISA in ~19 s. So hardware
    // is opt-in and the test reports whether it actually ran, rather than
    // quietly sampling and presenting the result as full coverage.
    if (std::getenv("VITREO_ISA_MATRIX_HARDWARE") != nullptr &&
        vitreo::oracle::available()) {
        State want{};
        if (vitreo::oracle::execute(in, code, &want) && want.status == 0) {
            r.oracle_ran = true;
            if (r.interp_ran) {
                oc::CompareOpts strict = opts;
                strict.flags_mask = oc::kArithFlagsMask & ~undef;
                // Rosetta does not model the MMX -> x87 transition: it leaves
                // the tag word at its incoming value and does not stamp the
                // exponents. `skip_x87_tags_and_regs` already carves out FTW
                // and ST() for that reason, settled on real silicon (see
                // oracle_contract.hpp), but FSW is compared on its own path,
                // so FSW.TOP still reports every MMX instruction as a
                // divergence — 68 on PACKSSDW alone.
                //
                // Mask TOP for the HARDWARE column only. The parity column
                // keeps comparing it, because there both sides are Vitreo and
                // must agree.
                //
                // OPEN: whether an MMX instruction really sets TOP to 0 is a
                // question Rosetta cannot answer. Vitreo's interpreter does
                // (FpuState::mmx_enter). Settle it on Windows the way the FTW
                // question was, not by argument.
                for (vitreo::u8 i = 0; i < fc.decoded.operand_count; ++i) {
                    if (fc.decoded.operands[i].type == OperandType::RegMM) {
                        strict.fsw_mask &= static_cast<std::uint16_t>(~0x3800u);
                        break;
                    }
                }
                r.hardware_diff = oc::compare(interp, want, strict);
            }
        }
    }
    return r;
}

}  // namespace vitreo::isa_matrix
