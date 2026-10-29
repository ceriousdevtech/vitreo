// SPDX-License-Identifier: LGPL-2.1-only
// Contract harness: execute the same x86 snippet on both the Vitreo interpreter
// and the Rosetta-backed oracle and compare the resulting CPU state.
#pragma once

#include "oracle/oracle_client.hpp"

#include <string>
#include <vector>

namespace vitreo::oracle::contract {

// Mask of architectural flags we compare (CF, PF, AF, ZF, SF, OF). Other
// RFLAGS bits (IF, TF, reserved, system bits) drift between runners and are
// not part of a userspace instruction contract.
constexpr uint32_t kArithFlagsMask = 0x0000'08D5u;

// The oracle runner executes under Rosetta, which is itself a translator and
// not silicon — so it is authoritative for *most* semantics, but not all.
//
// One known defect: Rosetta does not implement the "OF, AF, PF, SF := 0" half
// of the PTEST / VTESTPS / VTESTPD flag contract. It computes ZF and CF
// correctly and leaves the other four at their incoming values. Verified
// 2026-08-19 by running the identical probe on real x86 silicon (the
// `Windows` Windows host), which clears them exactly as Intel SDM Vol. 2
// PTEST specifies, and matches Vitreo.
//
// For those three instructions the Rosetta oracle can adjudicate ZF and CF but
// not the cleared flags, so compare only what it can actually answer. The
// cleared flags are covered by unit tests instead
// (tests/cpu/test_avx_permute_gather.cpp).
//
// Do not "fix" Vitreo to match Rosetta here. When Rosetta and the SDM
// disagree, settle it on real hardware before changing anything.
constexpr uint32_t kTestFlagsRosettaMask = 0x0000'0041u;  // ZF | CF only

// A second Rosetta defect, same shape, settled the same way. FNSTENV / FNSAVE
// write each 16-bit field into a 32-bit slot, and real hardware leaves the
// unused upper halves as ALL ONES. Rosetta zero-extends them.
//
// Probe (`fnstenv [m]`) on the `Windows` host, 2026-08-30:
//   silicon : 7f03 ffff 0000 ffff ffffffff 00000000 00000000 00000000 0000ffff
//   Vitreo  : 7f03 ffff 0000 ffff ffffffff 00000000 00000000 00000000 0000ffff
//   Rosetta : 7f03 0000 0000 0000 ffff0000 ...
//
// Vitreo is byte-identical to hardware. A guest that reloads the image with
// FLDENV/FRSTOR sees those bytes, so zero-extending is not cosmetic.
// The reserved halves sit at image offsets 2..3, 6..7, 10..11 and 26..27.

// Execute `code` through the Vitreo interpreter starting from `in`. Returns
// the resulting state projected into oracle::State layout. Low 32 bits of
// each GPR; XMM0..XMM7; EFLAGS (u32 view of RFLAGS); MXCSR. `status` is 0 on
// clean execution, non-zero on interpreter exception.
State run_on_interpreter(const State& in, const std::vector<uint8_t>& code);

// Same contract shape, executed through the Vitreo JIT backend. ARM64 only.
// `status` semantics:
//   0 = clean JIT execution (full block executed)
//   5 = JIT faulted / incomplete
//   6 = setup failure
//   7 = instruction unsupported by JIT (no stencil) — skip, don't compare
State run_on_jit(const State& in, const std::vector<uint8_t>& code);

// 64-bit (long mode) counterparts. Same `status` convention as above. The
// snippet is loaded at kCodeVa64 on both sides so RIP-relative operands
// resolve to the same effective address as they do under the oracle; pass the
// same value in `State::rip` (or leave it 0, which means kCodeVa64) when
// calling oracle::execute for the matching run.
//
// All 16 GPRs (except RSP, which neither side loads), XMM0-15, RFLAGS and
// MXCSR participate. `State::mode` on the returned value is kMode64.
State run_on_interpreter_64(const State& in, const std::vector<uint8_t>& code);
State run_on_jit_64(const State& in, const std::vector<uint8_t>& code);

// Compare two states using kArithFlagsMask for EFLAGS and ignoring GPR[ESP]
// (which neither side loads into the real RSP). The register/XMM width
// compared is taken from `State::mode` — 8 GPRs and XMM0-7 in kMode32, all 16
// of each in kMode64. MXCSR is compared. Returns empty string if identical,
// otherwise a human-readable diff.
//
// `flags_mask` narrows which RFLAGS bits participate. Pass a subset of
// kArithFlagsMask for instructions whose SDM entry leaves a flag *undefined*
// (e.g. OF after a shift with count != 1): real silicon computes a value there
// and Vitreo is free not to, so comparing it reports harness noise rather than
// a contract violation.
struct CompareOpts {
    uint32_t flags_mask = kArithFlagsMask;
    // Bit i set → skip GPR[i]. ESP/RSP (bit 4) is skipped unless compare_rsp.
    uint16_t gpr_ignore_mask = 0;
    // Exclusive of ESP. Used for RDTSC (RAX|RDX) and RDTSCP (RAX|RCX|RDX):
    // the timestamp is virtual on Vitreo and TSC_AUX is not installed.
    uint32_t mem_skip_off = 0;
    uint32_t mem_skip_len = 0;
    uint32_t mem_skip2_off = 0;
    uint32_t mem_skip2_len = 0;
    uint32_t mem_skip3_off = 0;
    uint32_t mem_skip3_len = 0;
    // When true, RSP participates (live-stack snippets). Default skip matches
    // the historical protocol where neither side loaded RSP.
    bool compare_rsp = false;

    // ---- x87 / MMX register file -------------------------------------------
    // The wire protocol has always carried FCW/FSW/FTW and ST(0)..ST(7), but
    // the comparator ignored them until 2026-08-30 -- so 86 x87 and 47 MMX
    // mnemonics were adjudicated only through their GPR/XMM/memory side
    // effects, and the stack, tag word and status word went unchecked.
    // Defaults on; narrow it per test only where the oracle genuinely cannot
    // answer, and say why.
    bool compare_x87 = true;

    // FSW bits to compare. Defaults to everything EXCEPT the sticky exception
    // bits (0x003F = IE DE ZE OE UE PE).
    //
    // Those six are excluded by default because Vitreo reads them off the HOST
    // FPU (core/math/host_fenv.hpp) and the host has one exception accumulator
    // shared between x87 and SSE, while x86 has two (FSW and MXCSR). So an
    // SSE-only snippet that rounds inexactly raises host FE_INEXACT, and the
    // harvest folds it into the x87 status word where architecturally nothing
    // happened -- 138 such false diffs across the SSE suites, every one
    // `interp=0020 oracle=0000`.
    //
    // This is the same single-host-unit aliasing that makes SSE arithmetic
    // ignore MXCSR.RC (see the DISABLED_ pin in
    // test_oracle_fp_rounding_64.cpp); both want the same real fix. Until
    // then, x87 suites opt the bits back in with kFswAll, which is where they
    // are meaningful and where they are actually checked.
    uint16_t fsw_mask = 0xFFC0u;

    // Rosetta does not model the MMX -> x87 aliasing bookkeeping: an MMX write
    // must set the tag word to all-valid (abridged FTW = 0xFF) and set the
    // written register's exponent field to all ones. Rosetta leaves FTW at its
    // incoming value and does not stamp the exponent.
    //
    // Settled on real silicon rather than by argument, the same way the
    // PTEST/VTEST flag defect above was. Probe: `movq mm0,[m]; movq mm1,[m+8];
    // pxor mm0,mm1; movq [m+16],mm0` seeded with FTW=0x00, run on the
    // `Windows` Windows host 2026-08-30 (tools/oracle_probe/x87_silicon_probe.c):
    //
    //   silicon : ftw=ff  ST0=ffff0808080808080808  ST1=ffff1f1e1d1c1b1a1918
    //   Vitreo  : ftw=ff  ST0=ffff0808080808080808  ST1=ffff1f1e1d1c1b1a1918
    //   Rosetta : ftw=00  (exponents unstamped)
    //
    // Vitreo matches hardware byte for byte; Rosetta is the wrong one. Do not
    // "fix" Vitreo to match it. Set this on MMX tests so the oracle adjudicates
    // the value side (which it gets right) and not the bookkeeping side.
    bool skip_x87_tags_and_regs = false;
};

// Every FSW bit, including the six sticky exception flags. For pure-x87
// snippets, where the host accumulator can only have been fed by x87.
inline constexpr uint16_t kFswAll = 0xFFFFu;

inline constexpr uint16_t kGprRax = 1u << 0;
inline constexpr uint16_t kGprRcx = 1u << 1;
inline constexpr uint16_t kGprRdx = 1u << 2;
// FXSAVE FOP/FIP/FDP (offsets 6..23): FNINIT does not reliably clear the
// last-instruction pointers on silicon the same way Vitreo's fpu.reset()
// zeros them.
inline constexpr uint32_t kFxsavePtrOff = 6;
inline constexpr uint32_t kFxsavePtrLen = 18;
// FXSAVE ST(0)..ST(7) slots: FNINIT tags them empty but does not zero the
// physical register file, so silicon leftover vs Vitreo zeros is not a
// contract failure.
inline constexpr uint32_t kFxsaveStOff = 32;
inline constexpr uint32_t kFxsaveStLen = 128;
// Legacy FXSAVE (32-bit guest): XMM8-15 and the reserved tail. The 32-bit
// oracle still runs in long mode on Rosetta, which writes XMM8-15; Vitreo's
// 32-bit FXSAVE does not. Bytes 464:511 are software-available and silicon
// does not write them (Intel SDM Vol. 2, FXSAVE).
inline constexpr uint32_t kFxsaveLegacyRsvdOff = 288;
inline constexpr uint32_t kFxsaveLegacyRsvdLen = 224;
// 64-bit FXSAVE/FXSAVE64: reserved + software-available after XMM15.
inline constexpr uint32_t kFxsave64RsvdOff = 416;
inline constexpr uint32_t kFxsave64RsvdLen = 96;

std::string compare(const State& got, const State& want, CompareOpts opts);
std::string compare(const State& got, const State& want,
                    uint32_t flags_mask = kArithFlagsMask);

// Warm-loop throughput of a snippet on the production JIT / interpreter.
// Compile (JIT) happens once; `compile_ns` is excluded from the ratio.
struct JitBenchResult {
    State out{};
    uint64_t compile_ns = 0;
    uint64_t warmup_ns = 0;
    uint64_t run_ns = 0;
    uint64_t iters = 0;
    uint32_t status = 6;
};

JitBenchResult bench_on_jit(const State& in, const std::vector<uint8_t>& code,
                            uint32_t warmup, uint32_t iters);
JitBenchResult bench_on_jit_64(const State& in,
                               const std::vector<uint8_t>& code,
                               uint32_t warmup, uint32_t iters);

// Bounded dispatcher-backed long-mode benchmark for snippets containing
// control transfers. Unlike bench_on_jit_64(), this compiles and executes each
// reached basic block until RIP reaches the byte immediately after `code`.
// Every iteration starts with the same private stack contents and RSP. The
// actual stack addresses are deliberately exposed separately so callers can
// compare the architectural balance (final_rsp - initial_rsp) across engines
// whose stack virtual addresses differ.
struct JitMultiBlockBenchResult {
    JitBenchResult bench{};
    uint64_t initial_rsp = 0;
    uint64_t final_rsp = 0;
    int64_t stack_delta = 0;
    uint32_t blocks_compiled = 0;
    uint32_t final_dispatches = 0;
};

JitMultiBlockBenchResult bench_on_jit_64_multiblock(
    const State& in, const std::vector<uint8_t>& code,
    uint32_t warmup, uint32_t iters);
JitBenchResult bench_on_interpreter(const State& in,
                                    const std::vector<uint8_t>& code,
                                    uint32_t warmup, uint32_t iters);

}  // namespace vitreo::oracle::contract
