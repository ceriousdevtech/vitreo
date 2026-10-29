// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64

/// @file test_vex_alias_routing.cpp
/// @brief Every advertised VEX-encoded SSE form still reaches the AVX bridge.
///
/// Vitreo has no `VADDPS` opcode. A VEX-encoded SSE instruction decodes to the
/// LEGACY `Opcode` with `prefixes.vex` set, and both execution tiers then
/// decide what to do with it by an enum RANGE CHECK:
///
///     bool opcode_is_vex_sse_family(Opcode op) {
///         return op >= Opcode::ADDSS_XMM_XMMM32 && op < Opcode::DAA;
///     }
///
/// Inside the range, `interpreter.cpp` routes to `exec_avx_sse_bridge` (the
/// VEX.NDS three-operand merge, the VEX.L lane split, the YMM[255:128]
/// zeroing) and `jit/ops/sse.cpp` gates on the same predicate. OUTSIDE it, the
/// instruction falls into the plain legacy switch: two-operand, 128-bit,
/// `vvvv` ignored, upper lane left stale.
///
/// That is not hypothetical. It shipped, it broke every in-guest BoringSSL TLS
/// handshake, and it is invisible whenever `dst == vvvv` -- which is the
/// common case in compiler output, so ordinary testing does not find it.
///
/// The predicate is correct today. But it is a range over enum VALUES, so it
/// is correct only by arrangement: appending an SSE opcode after `DAA` drops
/// it out of VEX handling, and inserting a non-SSE opcode between
/// `ADDSS_XMM_XMMM32` and `DAA` pulls it in. Neither change fails any
/// behavioural test, because the legacy lowering returns plausible numbers.
///
/// So this pins the arrangement. The form table is generated from Intel XED's
/// ISA database (`jit-refactor/tools/gen_vex_alias_table.py`), which is what
/// makes it an enumeration of the ARCHITECTURE rather than of what Vitreo
/// happens to implement.
///
/// Scope: this proves ROUTING, not semantics. That every form reaches the
/// bridge is necessary for correctness and is the property the recorded defect
/// violated; whether the bridge computes each form correctly is a separate
/// claim, owed per form in `jit-refactor/annotations.csv`.

#include "core/cpu/decode/decoder.hpp"
#include "core/cpu/interpreter/ops/exec_ops.hpp"

#include <gtest/gtest.h>

#include <vector>

using namespace vitreo;
using namespace vitreo::cpu;

#include "vex_alias_forms.inc"

namespace {

/// Synthesize the shortest legal encoding of one form, using the 3-byte VEX
/// prefix so every map/W combination is expressible.
std::vector<u8> encode_form(const VexAliasForm& f) {
    std::vector<u8> b;
    b.push_back(0xC4);                                   // 3-byte VEX
    b.push_back(static_cast<u8>(0xE0 | f.map));          // R=X=B=1, mmmmm
    b.push_back(static_cast<u8>((f.w << 7) | 0x78 |      // W, vvvv=0 (~0=1111)
                                (f.l << 2) | f.pp));     // L, pp
    b.push_back(f.opcode);
    // ModRM. Group opcodes (71/72/73) select the operation with reg, so a
    // fixed reg from the architecture table is used where the form has one.
    b.push_back(static_cast<u8>((f.mem ? 0x00 : 0xC0) | (f.reg << 3) | 0x01));
    if (f.imm) b.push_back(0x00);
    b.push_back(0x90);  // tail so a longer-than-expected decode cannot run out
    b.push_back(0x90);
    return b;
}

}  // namespace

TEST(VexAliasRouting, EveryAdvertisedVexSseFormReachesTheAvxBridge) {
    ASSERT_GT(std::size(kVexAliasForms), 800u)
        << "the generated form table looks truncated";

    int decode_failures = 0;
    int lost_vex = 0;
    int lost_vex_l = 0;
    int outside_family = 0;

    for (const auto& f : kVexAliasForms) {
        auto bytes = encode_form(f);
        MemoryByteReader reader(bytes.data(), bytes.size());
        DecodedInstruction inst;
        const DecodeError err =
            decode_instruction(reader, DecodeMode::Mode64, inst);

        if (err != DecodeError::Success) {
            if (++decode_failures <= 5) {
                ADD_FAILURE() << f.iclass << " (" << f.form_key
                              << ") does not decode; CPUID advertises it";
            }
            continue;
        }

        // The VEX bit must survive decode: it is the only thing distinguishing
        // this from the legacy instruction it shares an Opcode with.
        if (!inst.prefixes.vex) {
            if (++lost_vex <= 5) {
                ADD_FAILURE() << f.iclass << " (" << f.form_key
                              << ") decoded without prefixes.vex; it is now "
                                 "indistinguishable from the legacy form";
            }
            continue;
        }

        // VEX.L selects the 256-bit form. Losing it silently halves the work.
        if (f.l && !inst.prefixes.vex_l) {
            if (++lost_vex_l <= 5) {
                ADD_FAILURE() << f.iclass << " (" << f.form_key
                              << ") is a VEX.256 form but decoded with "
                                 "vex_l clear";
            }
        }

        // The routing property itself.
        if (!ops::opcode_is_vex_sse_family(inst.opcode)) {
            if (++outside_family <= 10) {
                ADD_FAILURE()
                    << f.iclass << " (" << f.form_key << ") decodes to "
                    << opcode_name(inst.opcode)
                    << ", which is OUTSIDE the [ADDSS_XMM_XMMM32, DAA) range. "
                       "It will take the legacy two-operand 128-bit path: "
                       "vvvv ignored, YMM[255:128] left stale.";
            }
        }
    }

    EXPECT_EQ(decode_failures, 0);
    EXPECT_EQ(lost_vex, 0);
    EXPECT_EQ(lost_vex_l, 0);
    EXPECT_EQ(outside_family, 0);
}

// The range's endpoints are the whole mechanism, so name them. If either
// endpoint moves relative to the SSE block, the test above starts failing and
// this says why in one line.
TEST(VexAliasRouting, FamilyRangeEndpointsBracketTheSseBlock) {
    EXPECT_TRUE(ops::opcode_is_vex_sse_family(Opcode::ADDSS_XMM_XMMM32));
    EXPECT_TRUE(ops::opcode_is_vex_sse_family(Opcode::ADDPD_XMM_XMMM128));
    EXPECT_TRUE(ops::opcode_is_vex_sse_family(Opcode::XORPS_XMM_XMMM128));
    EXPECT_FALSE(ops::opcode_is_vex_sse_family(Opcode::DAA));
    EXPECT_FALSE(ops::opcode_is_vex_sse_family(Opcode::ADD_R32_RM32));
    EXPECT_FALSE(ops::opcode_is_vex_sse_family(Opcode::NOP));
}
