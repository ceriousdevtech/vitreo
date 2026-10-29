// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// JIT 64-bit dispatch-gap guard.
//
// WHY THIS FILE EXISTS
// --------------------
// The JIT has two independent opcode dispatch paths, and only one of them is
// reachable from a 64-bit block:
//
//   * `JitCompiler::compile_instruction_impl` gates the ENTIRE 32-bit native
//     compiler chain on `!m_block_is_64bit`
//     (src/core/cpu/jit/instruction_compilation.cpp:139). Everything lowered
//     only in ops/mov.cpp, ops/arithmetic.cpp, ops/shifts.cpp,
//     ops/mul_div.cpp, ops/control.cpp, ops/stack.cpp or ops/string_ops.cpp
//     is therefore invisible to x64 code.
//   * A 64-bit block reaches exactly five compilers
//     (instruction_compilation.cpp:424-511): `compile_64bit` (ops/ops64.cpp),
//     `compile_mmx` (ops/mmx64.cpp), `compile_sse` (ops/sse.cpp),
//     `compile_memory_ops` (ops/memory_ops.cpp) and `compile_fpu`
//     (ops/fpu.cpp).
//
// An opcode in the first group and not the second does NOT fail to compile:
// it silently falls through to the per-instruction interpreter delegate
// (instruction_compilation.cpp:512 onward). Correctness is preserved by
// construction, so the only symptom is a large, silent slowdown — no error, no
// warning, and no failing test. That is precisely the failure mode
// jit_coverage.hpp was written to measure: a coverage gap shows up as a
// *performance* wall rather than a crash, and is therefore invisible without
// measurement.
//
// The gap was 219 opcodes. It has been closed down to the documented
// remainder in kExemptions below by forwarding batches of cases from
// `compile_64bit` to the owning 32-bit compiler (ops64.cpp:4966 onward).
// Nothing structural prevents it from growing again: a new lowering added to
// ops/shifts.cpp is 32-bit-only by default, and no existing test notices.
//
// This is not a one-off. The same "the code exists but was never wired for
// 64-bit blocks" shape has now been found independently in flag fusion, in
// direct branch patching and in the return-address stack. It is a recurring
// structural property of this JIT, which is what makes a standing guard worth
// more than a one-time cleanup.
//
// HOW TO FIX A FAILURE OF THIS TEST
// ---------------------------------
// The failure message names the exact opcodes. For each one, decide:
//
//   (a) The 32-bit lowering is ALREADY correct in long mode — the usual case
//       for ops that touch no pointer register and use the width-correct
//       helpers (emit_compute_ea forwards to emit_compute_ea64;
//       emit_load/store_gpr8/16/32 already apply long mode's partial-register
//       rules). Route it: add a `case Opcode::X:` to the appropriate batch at
//       the bottom of `JitCompiler::compile_64bit` in
//       src/core/cpu/jit/ops/ops64.cpp, forwarding to the owning compiler and
//       guarded by the standard kill switch:
//
//           case Opcode::X:
//               if (route32_disabled()) return false;   // VITREO_JIT_NO_ROUTE64
//               return compile_shifts(instr, instr_rip, next_rip, is_last);
//
//       Then add a parity case to tests/cpu/test_jit_parity_64.cpp using
//       `check_native_parity`.
//
//   (b) The 32-bit lowering is WRONG in long mode (it rewrites a pointer
//       register at 32 bits, or the instruction's stack width changes) — then
//       make the lowering width-aware off `m_block_is_64bit` FIRST and route
//       it afterwards. LEAVE and ENTER were both this shape and were both
//       fixed that way (ops64.cpp:5085-5112).
//
//   (c) The opcode cannot occur in a 64-bit block at all (the decoder rejects
//       it in Mode64, or emits a different Opcode there) — then add it to
//       kExemptions WITH A REASON THAT CITES THE DECODER, verified, file:line.
//
// WHY SOURCE TEXT AND NOT BEHAVIOUR
// ---------------------------------
// This guard reads the dispatch switches as source text rather than compiling
// probe blocks, deliberately. The behavioural formulation needs
// VITREO_JIT_NO_DELEGATE, which the JIT reads ONCE into a function-local
// `static` (instruction_compilation.cpp:512-516) and caches for the life of
// the process; a second mechanism, the process-global compiled-block memo,
// replays a `native_insns` recorded by whichever test first compiled those
// bytes at that address. Both make a behavioural check order-dependent inside
// a shared test binary — and both fail toward a silent vacuous PASS.
// tests/cpu/test_jit_coverage.cpp documents the same trap in its own header.
// A flaky guard is worse than none, so the behavioural claim is left to
// test_jit_parity_64.cpp's per-opcode `check_native_parity` cases, and this
// file stays a pure, deterministic, exhaustive source scan with no runtime
// dependency at all.
//
// Being a source scan also makes it independent of the environment it runs
// in. In particular VITREO_JIT_NO_ROUTE64 (ops64.cpp:134, the kill switch on
// every routed case) changes what the *process* does, not what the source
// says; the scan's conclusion is about the tree. The test notes it out loud if
// that variable happens to be set, so nobody reads a green result as a
// statement about that run.
//
// It keys only on `case Opcode::NAME:` labels and on which FILE they live in,
// so renaming a helper, reordering cases, or moving a case between two files
// on the same side of the gap changes nothing. Moving one ACROSS the gap is
// exactly the event it must notice.

#include <gtest/gtest.h>

#include <cctype>
#include <cstddef>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace {

// ============================================================================
// Exemptions: 32-bit-only lowerings that must NOT be routed into compile_64bit
// ============================================================================
//
// Every entry is a claim that the opcode cannot reach a 64-bit block, or that
// it must not reuse the 32-bit lowering. Each reason below was re-verified
// against the decoder in THIS tree, not inherited from the previous revision
// of this list. The test reports stale entries so the list cannot quietly rot,
// and requires every reason to cite a source location.
struct Exemption {
    const char* opcode;
    const char* reason;
};

const Exemption kExemptions[] = {
    // ------------------------------------------------------------------
    // (1) Invalid in 64-bit mode — the decoder refuses the encoding, so no
    //     64-bit block can ever contain the opcode.
    // ------------------------------------------------------------------
    {"PUSHA",
     "0x60 is #UD in long mode (Intel SDM Vol. 2 PUSHA, '64-bit Mode: "
     "Invalid'). src/core/cpu/decode/decoder.cpp:256-262 returns "
     "DecodeError::InvalidOpcode for 0x60/0x61 when mode == Mode64 and only "
     "emits Opcode::PUSHA in the else arm, so x64 code can never produce it. "
     "(This used to decode in Mode64, which let the interpreter run a 32-bit "
     "PUSHAD against a 64-bit stack; the decoder now faults instead, which is "
     "why the exemption reason is rejection rather than truncation.)"},
    {"POPA",
     "0x61 is #UD in long mode (Intel SDM Vol. 2 POPA). Same decoder arm: "
     "src/core/cpu/decode/decoder.cpp:256-262."},
    {"CALL_PTR16_16",
     "Direct far CALL ptr16:16 (0x9A) is #UD in long mode (Intel SDM Vol. 2 "
     "CALL). src/core/cpu/decode/decoder.cpp:321-323 returns "
     "DecodeError::InvalidOpcode for 0x9A/0xEA in Mode64; CALL_PTR16_16 is "
     "emitted only in the op_size == 2 arm at :329-331."},
    {"CALL_PTR16_32",
     "Direct far CALL ptr16:32 (0x9A). Same decoder arm: "
     "src/core/cpu/decode/decoder.cpp:321-323; CALL_PTR16_32 only at "
     ":340-342."},
    {"JMP_PTR16_16",
     "Direct far JMP ptr16:16 (0xEA) is #UD in long mode (Intel SDM Vol. 2 "
     "JMP). src/core/cpu/decode/decoder.cpp:321-323; JMP_PTR16_16 only at "
     ":329."},
    {"JMP_PTR16_32",
     "Direct far JMP ptr16:32 (0xEA). Same decoder arm: "
     "src/core/cpu/decode/decoder.cpp:321-323; JMP_PTR16_32 only at :340."},

    // ------------------------------------------------------------------
    // (2) Not encodable in long mode — the encoding IS accepted, but the
    //     decoder emits the 64-bit sibling Opcode instead, and that sibling
    //     is handled on the 64-bit side. Adding a `case` for the 32-bit
    //     Opcode would be dead code implying a reachability that does not
    //     exist.
    //
    //     Near stack and indirect-branch forms have a default operand size
    //     of 64 bits in long mode and cannot encode 32 (Intel SDM Vol. 1
    //     3.4.1.1, Vol. 2 Table 2-3). A 66H override still selects the
    //     16-bit form, which is why each decoder arm below tests
    //     `op_size != 2` rather than mode alone.
    // ------------------------------------------------------------------
    {"PUSH_R32",
     "0x50+r decodes as Opcode::PUSH_R64 when mode == Mode64 && op_size != 2 "
     "— src/core/cpu/decode/opcodes/stack.cpp:38-42; PUSH_R32 is emitted only "
     "in the final else at :48-49, unreachable in Mode64 (op_size == 2 takes "
     "the PUSH_R16 arm at :44)."},
    {"POP_R32",
     "0x58+r decodes as Opcode::POP_R64 in Mode64 — "
     "src/core/cpu/decode/opcodes/stack.cpp:122-124; POP_R32 only at "
     ":129-130."},
    {"PUSH_RM32",
     "0xFF /6 decodes as Opcode::PUSH_RM64 in Mode64 — "
     "src/core/cpu/decode/opcodes/stack.cpp:318-321; PUSH_RM32 only at "
     ":327-329."},
    {"POP_RM32",
     "0x8F /0 decodes as Opcode::POP_RM64 in Mode64 — "
     "src/core/cpu/decode/opcodes/stack.cpp:170-173; POP_RM32 only at "
     ":179-181."},
    {"CALL_RM32",
     "0xFF /2 decodes as Opcode::CALL_RM64 in Mode64, unconditionally (no "
     "op_size test — a 66H near indirect call is not encodable in long mode) "
     "— src/core/cpu/decode/opcodes/stack.cpp:248-251; CALL_RM32 only at "
     ":257-259."},
    {"JMP_RM32",
     "0xFF /4 decodes as Opcode::JMP_RM64 in Mode64, unconditionally — "
     "src/core/cpu/decode/opcodes/stack.cpp:283-286; JMP_RM32 only at "
     ":292-294."},
    {"CALL_M16_32",
     "Far indirect CALL m16:32 (0xFF /3) is not produced in Mode64: the "
     "decoder emits Opcode::CALL_M16_64, or CALL_M16_16 under 66H — "
     "src/core/cpu/decode/opcodes/stack.cpp:268-272. CALL_M16_32 appears only "
     "in the non-Mode64 else at :275. (Long mode's far indirect call goes "
     "through a descriptor, which the JIT models nowhere; the interpreter "
     "delegate is the correct handler for CALL_M16_64.)"},
    {"JMP_M16_32",
     "Far indirect JMP m16:32 (0xFF /5): the decoder emits "
     "Opcode::JMP_M16_64 / JMP_M16_16 in Mode64 — "
     "src/core/cpu/decode/opcodes/stack.cpp:303-306; JMP_M16_32 only at "
     ":309."},
    {"JCXZ_REL8",
     "0xE3 decodes as Opcode::JRCXZ_REL8 in Mode64 — "
     "src/core/cpu/decode/opcodes/control.cpp:89-95; JCXZ_REL8 and "
     "JECXZ_REL8 are produced only in the non-Mode64 else at :95. "
     "JRCXZ_REL8 itself IS routed (ops64.cpp:5090-5094), so the long-mode "
     "form of this instruction is covered."},
    {"JECXZ_REL8",
     "Same encoding as JCXZ_REL8: 0xE3 is JRCXZ_REL8 in Mode64 — "
     "src/core/cpu/decode/opcodes/control.cpp:89-95."},

    // ------------------------------------------------------------------
    // (3) Reachable in a 64-bit block, but the 32-bit lowering is WRONG
    //     there, so it needs a real 64-bit lowering rather than routing.
    // ------------------------------------------------------------------
    //
    // EMPTY at the time of writing. Opcode::ENTER was the standing entry in
    // this category — reachable in long mode
    // (src/core/cpu/decode/opcodes/indirect_ctrl.cpp:30 emits it for 0xC8
    // with no mode test) but lowered against a hard-wired 32-bit stack. It
    // has since been made width-aware in ops/stack.cpp (emit_load_gpr_ptr /
    // emit_store_gpr_ptr, 8-byte slots) and routed at ops64.cpp:5111-5113,
    // so it is no longer 32-bit-only and no longer belongs here. Keep this
    // category, with its worked example, because it is the arm a future
    // failure is most likely to land in and the one where "just route it"
    // is the wrong answer.
};

// ---------------------------------------------------------------------------
// Which side of the gap each ops/*.cpp file sits on.
// ---------------------------------------------------------------------------
// Named by FILE, not by function: `compile_shifts` living in ops/shifts.cpp is
// the stable fact, and a rename of the function is invisible here.

// Reached only when the block is 32-bit
// (src/core/cpu/jit/instruction_compilation.cpp:139).
const char* const kThirtyTwoBitOnlyFiles[] = {
    "mov.cpp",     "arithmetic.cpp", "shifts.cpp",     "mul_div.cpp",
    "control.cpp", "stack.cpp",      "string_ops.cpp",
    // compile_shifts' families, split out of shifts.cpp and reached through it.
    "shifts_rm8.cpp", "shifts_rm16.cpp", "shifts_rotate_narrow.cpp",
    "shifts_rotate_rm32.cpp", "shifts_rcl_rcr_rm32.cpp",
    "shifts_rcl_rcr_narrow.cpp", "shifts_shld_shrd.cpp",
    // Split out of arithmetic.cpp's compile_arithmetic (ADR-0006 step 3),
    // reached through it. ops/carry_flag.cpp, split out at the same time, is
    // absent: it holds no `case Opcode::` labels, and a listed file with none
    // fails SourceScanIsIntact.
    "alu32.cpp", "alu16.cpp", "alu8.cpp", "adc_sbb.cpp", "unary.cpp",
};

// Reached from a 64-bit block
// (src/core/cpu/jit/instruction_compilation.cpp:424-511).
const char* const kSixtyFourBitReachableFiles[] = {
    "ops64.cpp", "mmx64.cpp", "sse.cpp", "fpu.cpp",
    // Split out of fpu.cpp's compile_fpu_native (ADR-0006), reached through it.
    // Only these two of the nine ops/fpu_*.cpp families carry `case` labels
    // (the FLD-constant, FCMOVcc and ST(i)-arithmetic switches); the other
    // seven dispatch with if-chains, and a listed file with no labels fails
    // SourceScanIsIntact.
    "fpu_stack.cpp", "fpu_arith_reg.cpp",
    // Split out of the two files above (ADR-0006) and reached through them:
    // ops64.cpp delegates to compile_shifts64, sse.cpp to compile_avx_unique.
    // KEEP THIS LIST COMPLETE. Because the scan is by FILENAME, moving a case
    // label into a new file makes its opcode look unreachable from a 64-bit
    // block without anything having changed -- which is exactly what this test
    // reported when shifts64.cpp and avx.cpp first appeared.
    "shifts64.cpp", "avx.cpp",
    // compile_64bit's remaining opcode families, split out of ops64.cpp
    // (ADR-0006 step 3) and reached through it. ops/align_checks.cpp, split
    // out at the same time, is deliberately absent: its case labels are the
    // m128 alignment exclusion list, not dispatch, and every one of them is a
    // dispatch label in one of the files listed here.
    "mov64.cpp", "bit_ops64.cpp", "narrow_alu64.cpp", "arithmetic64.cpp",
    "adc_sbb64.cpp", "compare64.cpp", "unary64.cpp", "stack64.cpp",
    "control64.cpp", "flags64.cpp", "mul_div64.cpp", "misc64.cpp",
    "string_ops64.cpp", "sse64.cpp", "sse_move64.cpp",
    // compile_sse's legacy SSE opcode families and the VEX.256 lane-split
    // allowlist, split out of sse.cpp (ADR-0006 step 3).
    "sse_vex256.cpp", "sse_data.cpp", "sse_conv_packed.cpp", "sse_int.cpp",
    "sse_fp_packed.cpp", "sse_neon_stencils.cpp", "sse_fp_reciprocal.cpp",
    "sse_fp_scalar.cpp", "sse_crypto.cpp", "sse_shuffle.cpp", "sse_shift.cpp",
    "sse_insert_extract.cpp", "sse_round.cpp", "sse_cmp.cpp",
    "sse_mmx_forms.cpp", "sse_state.cpp", "sse4_helper_ops.cpp",
    "sse_conv_scalar.cpp",
    // compile_memory_ops' opcode families, split out of memory_ops.cpp and
    // reached through it. compile_locked's families (locked_*.cpp) are
    // deliberately absent: LOCK-only labels must not count as reachable.
    "memory_ops_xchg.cpp", "memory_ops_cond.cpp", "memory_ops_bt.cpp",
    "memory_ops_bitscan.cpp", "memory_ops_bmi.cpp",
    // memory_ops.cpp is handled separately: see kLockCompilerMarker.
};

/// memory_ops.cpp holds TWO compilers on opposite footings:
/// compile_memory_ops (memory_ops.cpp:95, reached by any 64-bit instruction)
/// and compile_locked (memory_ops.cpp:1784), which runs only for a
/// LOCK-prefixed instruction (instruction_compilation.cpp:127-133). A case
/// label that exists ONLY inside compile_locked does not make its opcode
/// reachable for the ordinary non-LOCK encoding — that mistake is what left
/// `and ax, imm16` delegating in x64 blocks, see the AND_RM16_* comment in
/// ops64.cpp — so the scan stops at the start of that function.
///
/// Matched on the qualified definition rather than a bare name so a mention in
/// a comment cannot move the cut point. If the marker is ever absent the scan
/// degrades to reading the whole file, which can only *hide* a gap in the
/// narrow case of a LOCK-only lowering — so the test says so out loud instead
/// of failing an otherwise-healthy tree.
const char kLockCompilerMarker[] = "JitCompiler::compile_locked";

/// The kill switch that guards every routed case in compile_64bit. Its
/// presence is a positive control on the routing batches: if it disappears,
/// either the routing was reworked wholesale or the file moved, and the scan's
/// premise needs re-checking by a human.
const char kRouteKillSwitch[] = "route32_disabled";

/// Positive control. Every one of these was ONCE in the gap and is now routed
/// or natively lowered on the 64-bit side. If the scan stops seeing them as
/// 64-bit-reachable, it is the parser that broke (or the routing that was
/// reverted wholesale), and a small residual gap set would be a false negative
/// rather than good news. One representative per routing batch.
const char* const kKnownRouted[] = {
    "CWDE",                 // 8/16-bit MOV family  -> compile_mov
    "ADC_R8_RM8",           // 8/16-bit ALU         -> compile_arithmetic
    "DIV_RM32",             // 32-bit MUL/DIV       -> compile_mul_div
    "MUL_RM8",              // 8/16-bit MUL/DIV     -> compile_mul_div
    "LEAVE",                // LEAVE/LOOP family    -> compile_control
    "LOOPNE_REL8",          //                      -> compile_control
    "JRCXZ_REL8",           // the long-mode E3     -> compile_control
    "ENTER",                // width-aware frame    -> compile_stack
    "MOVSB",                // string ops           -> compile_string_ops
    "SCASD",                //                      -> compile_string_ops
    "ROL_RM8_CL",           // 8/16-bit shifts      -> compile_shifts
    "SHLD_RM32_R32_CL",     // double shifts        -> compile_shifts
};

// ---------------------------------------------------------------------------
// Source location and scanning
// ---------------------------------------------------------------------------

std::string find_repo_root() {
#ifdef VITREO_SOURCE_DIR
    {
        const std::string root = VITREO_SOURCE_DIR;
        std::ifstream probe(root + "/src/core/cpu/jit/ops/ops64.cpp");
        if (probe.good()) return root;
    }
#endif
    // Fall back to walking up from the working directory: ctest runs from the
    // build tree, which normally lives inside the repo.
    std::string dir = ".";
    for (int i = 0; i < 10; ++i) {
        std::ifstream probe(dir + "/src/core/cpu/jit/ops/ops64.cpp");
        if (probe.good()) return dir;
        dir += "/..";
    }
    return {};
}

std::string read_file(const std::string& path) {
    std::ifstream in(path);
    if (!in) return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

/// Blank out // and /* */ comment bodies and string literals so a
/// `case Opcode::X:` quoted in a comment is never mistaken for a dispatch
/// label. Newlines are preserved so line numbers stay usable.
std::string strip_comments(const std::string& src) {
    std::string out = src;
    for (std::size_t i = 0; i + 1 < out.size();) {
        if (out[i] == '/' && out[i + 1] == '/') {
            while (i < out.size() && out[i] != '\n') out[i++] = ' ';
        } else if (out[i] == '/' && out[i + 1] == '*') {
            out[i++] = ' ';
            out[i++] = ' ';
            while (i + 1 < out.size() && !(out[i] == '*' && out[i + 1] == '/')) {
                if (out[i] != '\n') out[i] = ' ';
                ++i;
            }
            if (i + 1 < out.size()) {
                out[i++] = ' ';
                out[i++] = ' ';
            }
        } else if (out[i] == '"') {
            ++i;
            while (i < out.size() && out[i] != '"') {
                if (out[i] == '\\') ++i;
                ++i;
            }
            ++i;
        } else {
            ++i;
        }
    }
    return out;
}

bool is_ident_char(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') || c == '_';
}

/// Collect every `case Opcode::NAME:` label in `src`. Hand-rolled rather than
/// std::regex so the scan is trivially predictable (and fast on 5k-line
/// files); arbitrary whitespace and line breaks between the tokens are fine.
std::set<std::string> collect_case_labels(const std::string& raw) {
    const std::string src = strip_comments(raw);
    std::set<std::string> out;
    const std::string kKey = "Opcode::";
    for (std::size_t pos = src.find(kKey); pos != std::string::npos;
         pos = src.find(kKey, pos + kKey.size())) {
        // Require a preceding `case` token.
        std::size_t back = pos;
        while (back > 0 &&
               std::isspace(static_cast<unsigned char>(src[back - 1]))) {
            --back;
        }
        if (back < 4 || src.compare(back - 4, 4, "case") != 0) continue;
        if (back > 4 && is_ident_char(src[back - 5])) continue;

        const std::size_t name_start = pos + kKey.size();
        std::size_t name_end = name_start;
        while (name_end < src.size() && is_ident_char(src[name_end])) ++name_end;
        if (name_end == name_start) continue;

        // Require the label to actually end in ':', so `case` on one line and
        // an unrelated `Opcode::X)` in a condition below it cannot match.
        std::size_t after = name_end;
        while (after < src.size() &&
               std::isspace(static_cast<unsigned char>(src[after]))) {
            ++after;
        }
        if (after >= src.size() || src[after] != ':') continue;

        out.insert(src.substr(name_start, name_end - name_start));
    }
    return out;
}

/// Everything one scan of the tree produces. Built once per test so each
/// TEST() below reads the same picture.
struct Scan {
    bool ok = false;             ///< false => the scan itself is broken
    std::string root;
    std::set<std::string> only32;      ///< labels in the 32-bit-only files
    std::set<std::string> reachable64; ///< labels reachable from a 64-bit block
    std::set<std::string> jitable;     ///< is_jitable() whitelist, stencils.cpp
    std::set<std::string> gap;         ///< only32 - reachable64, minus exempt
    std::vector<std::string> problems; ///< scan-integrity complaints
};

void note_problem(Scan& s, const std::string& msg) { s.problems.push_back(msg); }

std::set<std::string> labels_of(Scan& s, const std::string& path) {
    const std::string src = read_file(path);
    if (src.empty()) {
        note_problem(s,
            "Could not read " + path +
            "\n  This guard reads the JIT dispatch switches as source text. If "
            "the file moved or was renamed, update the file lists at the top "
            "of tests/cpu/test_jit_dispatch_gap.cpp — do NOT delete the entry, "
            "or the opcodes it owns silently stop being guarded.");
        return {};
    }
    std::set<std::string> labels = collect_case_labels(src);
    if (labels.empty()) {
        note_problem(s,
            "Parsed zero `case Opcode::NAME:` labels from " + path +
            "\n  Either the scan is broken, or that dispatch switch was "
            "rewritten in a form this parser does not recognise (a table, a "
            "macro, an if/else chain). Either way the gap can no longer be "
            "computed from it, and a green result would mean nothing.");
    }
    return labels;
}

Scan run_scan() {
    Scan s;
    s.root = find_repo_root();
    if (s.root.empty()) {
        note_problem(s,
            "Cannot locate the Vitreo source tree. Build this test with "
            "COMPILE_DEFINITIONS VITREO_SOURCE_DIR=\\\"${CMAKE_SOURCE_DIR}\\\" "
            "(see tests/CMakeLists.txt).");
        return s;
    }

    const std::string ops = s.root + "/src/core/cpu/jit/ops";

    for (const char* f : kThirtyTwoBitOnlyFiles) {
        const std::set<std::string> l = labels_of(s, ops + "/" + f);
        s.only32.insert(l.begin(), l.end());
    }
    for (const char* f : kSixtyFourBitReachableFiles) {
        const std::set<std::string> l = labels_of(s, ops + "/" + f);
        s.reachable64.insert(l.begin(), l.end());
    }

    // memory_ops.cpp, minus the LOCK-only compiler at its tail.
    {
        const std::string path = ops + "/memory_ops.cpp";
        const std::string src = read_file(path);
        if (src.empty()) {
            note_problem(s, "Could not read " + path);
        } else {
            const std::size_t cut = src.find(kLockCompilerMarker);
            if (cut == std::string::npos) {
                // Degrade loudly but do not fail: this direction can only hide
                // a LOCK-only lowering, and today that set is empty.
                std::cerr << "[ note ] " << kLockCompilerMarker
                          << " not found in memory_ops.cpp; scanning the whole "
                             "file. If compile_locked was renamed, update "
                             "kLockCompilerMarker in "
                             "tests/cpu/test_jit_dispatch_gap.cpp.\n";
            }
            const std::set<std::string> l = collect_case_labels(
                cut == std::string::npos ? src : src.substr(0, cut));
            if (l.empty()) {
                note_problem(s,
                    "Parsed zero dispatch labels from compile_memory_ops in " +
                    path + " — the scan is broken.");
            }
            s.reachable64.insert(l.begin(), l.end());
        }
    }

    // Positive control on the routing batches themselves.
    {
        const std::string src = read_file(ops + "/ops64.cpp");
        if (src.find(kRouteKillSwitch) == std::string::npos) {
            note_problem(s,
                std::string("`") + kRouteKillSwitch +
                "` no longer appears in ops/ops64.cpp. Every case that "
                "compile_64bit forwards to a 32-bit compiler is supposed to be "
                "guarded by it (VITREO_JIT_NO_ROUTE64). Its disappearance means "
                "the routing was reworked wholesale — re-check this guard's "
                "premise by hand before making it green again.");
        }
    }

    // The 32-bit native chain is additionally gated on `has_stencil(opcode)`
    // (instruction_compilation.cpp:134), which forwards to the `is_jitable`
    // whitelist in src/core/cpu/jit/stencils.cpp:22. An opcode outside that
    // whitelist never reaches a 32-bit lowering either, so it cannot be a
    // 64-bit regression — and intersecting with it also drops case labels
    // belonging to helper sub-switches rather than to dispatch.
    s.jitable = labels_of(s, s.root + "/src/core/cpu/jit/stencils.cpp");

    std::set<std::string> exempt;
    for (const Exemption& e : kExemptions) exempt.insert(e.opcode);

    for (const std::string& op : s.only32) {
        if (s.reachable64.count(op)) continue;  // routed or natively lowered
        if (!s.jitable.count(op)) continue;     // no 32-bit dispatch either
        if (exempt.count(op)) continue;         // documented above
        s.gap.insert(op);
    }

    s.ok = s.problems.empty();
    return s;
}

/// Turn any scan-integrity complaint into a hard failure. Called first by
/// every test: the whole family of failure modes here ends in "the gap
/// computes as empty", which must never be reported as health.
void require_scan_integrity(const Scan& s) {
    for (const std::string& p : s.problems) ADD_FAILURE() << p;
    ASSERT_TRUE(s.ok) << "The dispatch-gap scan did not complete. Treat this "
                         "as a broken guard, not as an absence of gaps.";
}

}  // namespace

// ============================================================================
// 1. The scan must be able to see what it claims to measure.
// ============================================================================
//
// Anti-false-pass. Every way this guard can break — paths moved, dispatch
// rewritten as a table, a stray `#if 0` — makes the gap compute as EMPTY,
// i.e. a pass. So the sizes are floored, and a sample of known-routed opcodes
// must still be visible on the 64-bit side. The floors sit well below the real
// sizes at the time of writing (only32 342, reachable64 1162, jitable 1096) so
// ordinary churn never trips them; they only catch collapse.
TEST(JitDispatchGapTest, SourceScanIsIntact) {
    const Scan s = run_scan();
    require_scan_integrity(s);

    if (std::getenv("VITREO_JIT_NO_ROUTE64") != nullptr) {
        std::cerr << "[ note ] VITREO_JIT_NO_ROUTE64 is set in this "
                     "environment. It disables every routed case at RUNTIME "
                     "(ops64.cpp:134). This guard is a source scan, so its "
                     "result is unchanged and still describes the tree — but "
                     "the JIT in this process does have the full gap.\n";
    }

    ASSERT_GE(s.only32.size(), 250u)
        << "Parsed only " << s.only32.size()
        << " case labels from the 32-bit-only ops files — the scan is broken, "
           "not the JIT.";
    ASSERT_GE(s.reachable64.size(), 800u)
        << "Parsed only " << s.reachable64.size()
        << " case labels from the 64-bit-reachable ops files — the scan is "
           "broken, not the JIT.";
    ASSERT_GE(s.jitable.size(), 800u)
        << "Parsed only " << s.jitable.size()
        << " opcodes from is_jitable() in stencils.cpp — the scan is broken.";

    for (const char* op : kKnownRouted) {
        EXPECT_TRUE(s.reachable64.count(op) != 0)
            << "Opcode::" << op
            << " is a positive control: it was once in the 64-bit dispatch gap "
               "and is now routed into compile_64bit. The scan no longer sees "
               "it as reachable from a 64-bit block.\n"
               "  If the routing really was reverted, that is the regression "
               "this file exists to catch — restore the `case` in "
               "src/core/cpu/jit/ops/ops64.cpp.\n"
               "  If the routing is still there, the PARSER broke, and any "
               "small gap set it reports is a false negative.";
    }
}

// ============================================================================
// 2. The guard proper.
// ============================================================================
TEST(JitDispatchGapTest, NoLoweringIsReachableOnlyFromThirtyTwoBitBlocks) {
    const Scan s = run_scan();
    require_scan_integrity(s);
    // Re-assert the floors: this test must not be able to pass on a collapsed
    // scan even if someone runs it with --gtest_filter and skips test 1.
    ASSERT_GE(s.only32.size(), 250u) << "scan collapsed; see SourceScanIsIntact";
    ASSERT_GE(s.reachable64.size(), 800u)
        << "scan collapsed; see SourceScanIsIntact";

    if (s.gap.empty()) return;

    std::ostringstream msg;
    msg << "\n"
        << s.gap.size()
        << " opcode(s) have a JIT lowering that only a 32-BIT block can reach. "
           "In a 64-bit block each one silently falls back to the interpreter "
           "delegate — no error, no crash, just a large slowdown:\n\n";
    for (const std::string& op : s.gap) msg << "    Opcode::" << op << "\n";
    msg << "\nFIX: for each opcode, add a `case` to the routing batches at the "
           "bottom of JitCompiler::compile_64bit "
           "(src/core/cpu/jit/ops/ops64.cpp, from ~line 4966), forwarding to "
           "the compiler that owns it:\n\n"
           "        case Opcode::X:\n"
           "            if (route32_disabled()) return false;\n"
           "            return compile_shifts(instr, instr_rip, next_rip, "
           "is_last);\n\n"
           "Route only when the 32-bit lowering is already correct in long "
           "mode: no pointer register touched at 32 bits, EA via "
           "emit_compute_ea (which forwards to emit_compute_ea64), data via "
           "the width-correct emit_load/store_gpr8/16/32 helpers. If it is "
           "not, make the lowering width-aware off m_block_is_64bit first and "
           "route it after — LEAVE and ENTER were both fixed that way. If the "
           "opcode cannot occur in a 64-bit block at all, add it to "
           "kExemptions in tests/cpu/test_jit_dispatch_gap.cpp WITH A REASON "
           "THAT CITES THE DECODER, file:line, verified — not assumed. Then "
           "add a check_native_parity case to "
           "tests/cpu/test_jit_parity_64.cpp.";
    ADD_FAILURE() << msg.str();
}

// ============================================================================
// 3. The exemption list must stay honest.
// ============================================================================
//
// An exemption is a standing claim about the decoder. Two ways it rots:
//   * the opcode stops being 32-bit-only (routed, or dropped from is_jitable)
//     — the entry is then dead weight that would hide a future regression of
//     that same opcode. PUSHA/POPA, LEAVE, LOOP*, JRCXZ and ENTER all reached
//     this state at various points.
//   * the opcode name is misspelled or its lowering was deleted — the entry
//     then exempts nothing while looking like a documented decision.
TEST(JitDispatchGapTest, ExemptionListIsCurrent) {
    const Scan s = run_scan();
    require_scan_integrity(s);
    ASSERT_GE(s.only32.size(), 250u) << "scan collapsed; see SourceScanIsIntact";

    // A wholesale exemption spree is the easy way to make this guard green
    // without fixing anything. The list is small and structural by nature, so
    // a ceiling costs nothing and forces the conversation.
    const std::size_t kExemptionCount =
        sizeof(kExemptions) / sizeof(kExemptions[0]);
    EXPECT_LE(kExemptionCount, 24u)
        << "kExemptions has grown to " << kExemptionCount
        << " entries. Every entry is a claim that an opcode cannot reach a "
           "64-bit block; there are only so many such encodings. If this is "
           "legitimate growth, raise the ceiling deliberately in the same "
           "commit — do not let a batch of 'probably fine' entries in "
           "unnoticed.";

    for (const Exemption& e : kExemptions) {
        // Every reason must cite where it was verified. Not a style rule: an
        // uncited reason cannot be re-checked when the decoder changes, and
        // the whole value of this list is that it can be.
        const std::string reason = e.reason;
        EXPECT_NE(reason.find(".cpp:"), std::string::npos)
            << "The exemption for Opcode::" << e.opcode
            << " has no `file.cpp:line` citation. Every exemption is a claim "
               "about the decoder and must name the code that makes it true, "
               "so the next person can re-verify it instead of trusting it.";

        if (!s.only32.count(e.opcode)) {
            ADD_FAILURE()
                << "kExemptions names Opcode::" << e.opcode
                << ", which the scan does not find as a `case` label in any "
                   "32-bit-only ops file. Either it is misspelled, or its "
                   "lowering was deleted — in both cases the entry is "
                   "meaningless and must go.\n  Recorded reason was: "
                << e.reason;
            continue;
        }

        const bool still_thirty_two_bit_only =
            !s.reachable64.count(e.opcode) && s.jitable.count(e.opcode) != 0;
        EXPECT_TRUE(still_thirty_two_bit_only)
            << "Stale exemption: Opcode::" << e.opcode
            << " is no longer a 32-bit-only lowering — it is now dispatched "
               "from a 64-bit block, or no longer jitable at all. Delete its "
               "entry from kExemptions in "
               "tests/cpu/test_jit_dispatch_gap.cpp so the opcode is guarded "
               "again.\n  Recorded reason was: "
            << e.reason;
    }
}
