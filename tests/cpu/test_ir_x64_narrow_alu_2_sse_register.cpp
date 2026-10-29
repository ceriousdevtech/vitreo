// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64

/// @file test_ir_x64_narrow_alu_2_sse_register.cpp
/// @brief Long-mode IR emission: SSE register forms (M13a).

// These tests execute emitted ARM64, so Intel hosts must not reference the
// native-only IR backend through this translation unit.
#if defined(__aarch64__)
#include "ir_x64_memory_fixture.hpp"

using namespace vitreo;
using namespace vitreo::cpu;
using namespace vitreo::cpu::jit;
using namespace vitreo::cpu::jit::test::ir_x64_memory;

namespace {
// =============================================================================
// Long-mode SSE register forms (M13a)
// =============================================================================

// The 128-bit bitwise and copy forms, register to register. Long mode has
// SIXTEEN XMM registers, so the sweep walks all of them in both operand
// positions -- the Q-form load/store offset is a 16-scaled uimm12 and a
// register the offset cannot reach would emit a WRONG offset rather than fail.
TEST_F(IrEmit64NarrowAlu, SseRegisterFormsCoverEveryXmmRegister) {
    // `VITREO_JIT_IR_SSE` bit 3 is the production escape hatch for this
    // tranche. With it clear the forms decline on purpose, so asserting they
    // reach Tier 1 would be asserting the gate is off rather than testing it.
    if (const char* mask = std::getenv("VITREO_JIT_IR_SSE")) {
        if ((std::strtoul(mask, nullptr, 0) & 8u) == 0)
            GTEST_SKIP() << "long-mode SSE tranche disabled by VITREO_JIT_IR_SSE";
    }
    const auto run = [&](std::vector<u8> body) {
        std::string spelling;
        for (const u8 byte : body) {
            char one[4];
            std::snprintf(one, sizeof one, "%02x ", byte);
            spelling += one;
        }
        SCOPED_TRACE(testing::Message() << "bytes: " << spelling);
        jit = std::make_unique<JitCompiler>(memory);
        auto* native = compile(body);
        ASSERT_NE(native, nullptr);
        auto oracle = seed();
        for (u8 x = 0; x < 16; ++x) {
            oracle.sse.xmm[x].u64_[0] = 0x0123456789ABCDEFull ^ (0x1111111111111111ull * x);
            oracle.sse.xmm[x].u64_[1] = 0xFEDCBA9876543210ull ^ (0x2222222222222222ull * x);
        }
        auto state = oracle;
        {
            Interpreter interpreter(oracle, memory);
            ASSERT_EQ(interpreter.step(), ExecResult::Continue);
        }
        ASSERT_EQ(jit->execute(*native, state), JitExecResult::Continue);
        for (u8 x = 0; x < 16; ++x) {
            EXPECT_EQ(state.sse.xmm[x].u64_[0], oracle.sse.xmm[x].u64_[0])
                << "xmm" << unsigned(x) << " low";
            EXPECT_EQ(state.sse.xmm[x].u64_[1], oracle.sse.xmm[x].u64_[1])
                << "xmm" << unsigned(x) << " high";
        }
        for (u8 index = 0; index < 16; ++index)
            EXPECT_EQ(state.gpr.get64(index), oracle.gpr.get64(index))
                << "GPR " << unsigned(index);
    };
    // The bitwise trio at every spelling: single, double and integer.
    struct Bitwise { std::vector<u8> prefix; u8 op; const char* name; };
    const std::vector<Bitwise> kBitwise = {
        {{},      0x57, "xorps"}, {{0x66}, 0x57, "xorpd"}, {{0x66}, 0xEF, "pxor"},
        {{},      0x54, "andps"}, {{0x66}, 0x54, "andpd"}, {{0x66}, 0xDB, "pand"},
        {{},      0x56, "orps"},  {{0x66}, 0x56, "orpd"},  {{0x66}, 0xEB, "por"},
    };
    for (const Bitwise& form : kBitwise) {
        for (u8 dst = 0; dst < 16; ++dst) {
            const u8 src = static_cast<u8>((dst + 5) % 16);
            std::vector<u8> body = form.prefix;
            const u8 rex = static_cast<u8>(0x40 | ((dst >= 8) ? 0x4 : 0) |
                                                  ((src >= 8) ? 0x1 : 0));
            if (rex != 0x40) body.push_back(rex);
            body.push_back(0x0F);
            body.push_back(form.op);
            body.push_back(static_cast<u8>(0xC0 | ((dst & 7) << 3) | (src & 7)));
            run(body);
        }
        // Same register twice: `xorps xmm,xmm` is the zeroing idiom, and the
        // AND/OR spellings of it are identities.
        std::vector<u8> same = form.prefix;
        same.push_back(0x0F);
        same.push_back(form.op);
        same.push_back(0xC0);
        run(same);
    }
    // The copies, both directions, aligned and unaligned, all three domains.
    struct Copy { std::vector<u8> prefix; u8 to_reg; u8 to_rm; };
    const std::vector<Copy> kCopies = {
        {{},      0x28, 0x29},          // movaps
        {{0x66}, 0x28, 0x29},           // movapd
        {{},      0x10, 0x11},          // movups
        {{0x66}, 0x10, 0x11},           // movupd
        {{0x66}, 0x6F, 0x7F},           // movdqa
        {{0xF3}, 0x6F, 0x7F},           // movdqu
    };
    for (const Copy& form : kCopies) {
        for (u8 dst = 0; dst < 16; dst += 3) {
            const u8 src = static_cast<u8>((dst + 7) % 16);
            for (const u8 op : {form.to_reg, form.to_rm}) {
                std::vector<u8> body = form.prefix;
                const u8 rex = static_cast<u8>(0x40 | ((dst >= 8) ? 0x4 : 0) |
                                                      ((src >= 8) ? 0x1 : 0));
                if (rex != 0x40) body.push_back(rex);
                body.push_back(0x0F);
                body.push_back(op);
                body.push_back(static_cast<u8>(0xC0 | ((dst & 7) << 3) | (src & 7)));
                run(body);
            }
        }
    }
}

// The element-wise packed integer families. The ELEMENT WIDTH is the axis
// that matters: the same NEON encoder serves all four sizes with a different
// size selector, so a lowering that fixed the selector would agree on one
// width and silently corrupt the other three. Signed and unsigned min/max and
// the signed compare are driven with operands that straddle the sign boundary,
// which is the only way those tell each other apart.
TEST_F(IrEmit64NarrowAlu, SaturatingAndSse41PackedFormsMatchTheInterpreter) {
    if (const char* mask = std::getenv("VITREO_JIT_IR_SSE")) {
        if ((std::strtoul(mask, nullptr, 0) & 8u) == 0)
            GTEST_SKIP() << "long-mode SSE tranche disabled by VITREO_JIT_IR_SSE";
    }
    ASSERT_TRUE(memory.map_region(kData, 0x1000, MemoryProtection::ReadWrite));

    // Lane sets chosen for what SATURATION does, which is the whole point of
    // this tranche: every element here is at or next to a signed or unsigned
    // limit, so a wrapping lowering and a saturating one disagree immediately.
    // The last set is the asymmetric signed minimum, which is the case that
    // separates ABS-with-saturation from ABS-that-wraps, and SQSUB's clamp
    // from a plain subtract.
    static const u64 kLanes[5][2] = {
        {0x7F7F7F7F7F7F7F7Full, 0x0101010101010101ull},
        {0x8080808080808080ull, 0xFFFFFFFFFFFFFFFFull},
        {0x7FFF7FFF7FFF7FFFull, 0x0001000100010001ull},
        {0x8000000080000000ull, 0xFFFFFFFF00000001ull},
        {0x8000800080008000ull, 0x8000800080008000ull},
    };
    const auto run = [&](std::vector<u8> body, const char* what) {
        SCOPED_TRACE(testing::Message() << what);
        jit = std::make_unique<JitCompiler>(memory);
        auto* native = compile(body);
        ASSERT_NE(native, nullptr);
        for (const auto& lanes : kLanes) {
            auto oracle = seed();
            oracle.gpr.set64(0, kData);
            // Distinct per register: a lowering that read the wrong source, or
            // that wrote the destination from itself, would still agree if
            // every XMM held the same bits.
            for (u8 x = 0; x < 16; ++x) {
                oracle.sse.xmm[x].u64_[0] = lanes[0] ^ (0x0101010101010101ull * x);
                oracle.sse.xmm[x].u64_[1] = lanes[1] ^ (0x1010101010101010ull * x);
            }
            ASSERT_TRUE(memory.write_u64(kData, lanes[0]).is_ok());
            ASSERT_TRUE(memory.write_u64(kData + 8, lanes[1]).is_ok());
            auto state = oracle;
            {
                Interpreter interpreter(oracle, memory);
                ASSERT_EQ(interpreter.step(), ExecResult::Continue);
            }
            ASSERT_EQ(jit->execute(*native, state), JitExecResult::Continue);
            for (u8 x = 0; x < 16; ++x) {
                EXPECT_EQ(state.sse.xmm[x].u64_[0], oracle.sse.xmm[x].u64_[0])
                    << what << " xmm" << unsigned(x) << " low";
                EXPECT_EQ(state.sse.xmm[x].u64_[1], oracle.sse.xmm[x].u64_[1])
                    << what << " xmm" << unsigned(x) << " high";
            }
        }
    };

    // Two-byte saturating add/subtract: 66 0F xx.
    for (const auto& f : std::vector<std::pair<u8, const char*>>{
             {0xEC, "paddsb"},  {0xED, "paddsw"},
             {0xDC, "paddusb"}, {0xDD, "paddusw"},
             {0xE8, "psubsb"},  {0xE9, "psubsw"},
             {0xD8, "psubusb"}, {0xD9, "psubusw"}}) {
        run({0x66, 0x0F, f.first, 0xD9}, f.second);           // xmm3, xmm1
        run({0x66, 0x0F, f.first, 0xC0}, f.second);           // xmm0, xmm0
        run({0x66, 0x44, 0x0F, f.first, 0xCB}, f.second);     // xmm9, xmm3
        run({0x66, 0x0F, f.first, 0x18}, f.second);           // xmm3, [rax]
    }

    // Three-byte SSE4.1/4.2 binaries: 66 0F 38 xx. The min/max octet is the
    // reason the verifier now rejects a 64-bit element for that family --
    // x86 stops at dword here, and so does ARM64.
    for (const auto& f : std::vector<std::pair<u8, const char*>>{
             {0x38, "pminsb"}, {0x39, "pminsd"}, {0x3A, "pminuw"},
             {0x3B, "pminud"}, {0x3C, "pmaxsb"}, {0x3D, "pmaxsd"},
             {0x3E, "pmaxuw"}, {0x3F, "pmaxud"},
             {0x40, "pmulld"}, {0x29, "pcmpeqq"}, {0x37, "pcmpgtq"}}) {
        run({0x66, 0x0F, 0x38, f.first, 0xD9}, f.second);
        run({0x66, 0x0F, 0x38, f.first, 0xC0}, f.second);
        run({0x66, 0x44, 0x0F, 0x38, f.first, 0xCB}, f.second);
        run({0x66, 0x0F, 0x38, f.first, 0x18}, f.second);
    }

    // PABS is the only unary form in the tranche: the destination is written
    // from the source alone. `pabs xmm0, xmm0` is therefore a real test of
    // that, not a tautology -- it must replace xmm0 rather than combine it.
    for (const auto& f : std::vector<std::pair<u8, const char*>>{
             {0x1C, "pabsb"}, {0x1D, "pabsw"}, {0x1E, "pabsd"}}) {
        run({0x66, 0x0F, 0x38, f.first, 0xD9}, f.second);
        run({0x66, 0x0F, 0x38, f.first, 0xC0}, f.second);
        run({0x66, 0x44, 0x0F, 0x38, f.first, 0xCB}, f.second);
        run({0x66, 0x0F, 0x38, f.first, 0x18}, f.second);
    }
}

TEST_F(IrEmit64NarrowAlu, PackedIntegerFormsAtEveryElementWidth) {
    if (const char* mask = std::getenv("VITREO_JIT_IR_SSE")) {
        if ((std::strtoul(mask, nullptr, 0) & 8u) == 0)
            GTEST_SKIP() << "long-mode SSE tranche disabled by VITREO_JIT_IR_SSE";
    }
    ASSERT_TRUE(memory.map_region(kData, 0x1000, MemoryProtection::ReadWrite));
    struct Form { u8 op; const char* name; };
    const std::vector<Form> kForms = {
        {0xFC, "paddb"}, {0xFD, "paddw"}, {0xFE, "paddd"}, {0xD4, "paddq"},
        {0xF8, "psubb"}, {0xF9, "psubw"}, {0xFA, "psubd"}, {0xFB, "psubq"},
        {0x74, "pcmpeqb"}, {0x75, "pcmpeqw"}, {0x76, "pcmpeqd"},
        {0x64, "pcmpgtb"}, {0x65, "pcmpgtw"}, {0x66, "pcmpgtd"},
        {0xDA, "pminub"}, {0xDE, "pmaxub"}, {0xEA, "pminsw"}, {0xEE, "pmaxsw"},
        {0xD5, "pmullw"}, {0xE0, "pavgb"},  {0xE3, "pavgw"},
        {0x60, "punpcklbw"}, {0x61, "punpcklwd"}, {0x62, "punpckldq"},
        {0x6C, "punpcklqdq"},
        {0x68, "punpckhbw"}, {0x69, "punpckhwd"}, {0x6A, "punpckhdq"},
        {0x6D, "punpckhqdq"},
    };
    const auto run = [&](std::vector<u8> body, const char* what) {
        SCOPED_TRACE(testing::Message() << what);
        jit = std::make_unique<JitCompiler>(memory);
        auto* native = compile(body);
        ASSERT_NE(native, nullptr);
        // Operands straddling every boundary that separates these operations:
        // signed/unsigned ordering, element carries, and equality.
        static const u64 kLanes[4][2] = {
            {0x7F80017FFE0100FFull, 0x8000FF7F01FE8001ull},
            {0xFFFFFFFFFFFFFFFFull, 0x0000000000000001ull},
            {0x8000800080008000ull, 0x7FFF7FFF7FFF7FFFull},
            {0x0102030405060708ull, 0x0807060504030201ull},
        };
        for (const auto& lanes : kLanes) {
            // Re-seeded per lane set: carrying the previous run's state would
            // carry its ADVANCED RIP too, and the next iteration would execute
            // from the wrong address.
            auto oracle = seed();
            oracle.gpr.set64(0, kData);
            for (u8 x = 0; x < 16; ++x) {
                oracle.sse.xmm[x].u64_[0] = lanes[0] ^ (0x0101010101010101ull * x);
                oracle.sse.xmm[x].u64_[1] = lanes[1] ^ (0x1010101010101010ull * x);
            }
            ASSERT_TRUE(memory.write_u64(kData, lanes[0]).is_ok());
            ASSERT_TRUE(memory.write_u64(kData + 8, lanes[1]).is_ok());
            auto state = oracle;
            {
                Interpreter interpreter(oracle, memory);
                ASSERT_EQ(interpreter.step(), ExecResult::Continue);
            }
            ASSERT_EQ(jit->execute(*native, state), JitExecResult::Continue);
            for (u8 x = 0; x < 16; ++x) {
                EXPECT_EQ(state.sse.xmm[x].u64_[0], oracle.sse.xmm[x].u64_[0])
                    << what << " xmm" << unsigned(x) << " low";
                EXPECT_EQ(state.sse.xmm[x].u64_[1], oracle.sse.xmm[x].u64_[1])
                    << what << " xmm" << unsigned(x) << " high";
            }
        }
    };
    // PACK*: saturating narrows, destination into the low half. PACKUSWB is
    // the one that separates SQXTUN from UQXTN -- a signed source saturated
    // into an unsigned destination -- and only inputs outside the unsigned
    // range tell them apart, which the lane sets above supply.
    for (const auto& pack : std::vector<std::pair<u8, const char*>>{
             {0x63, "packsswb"}, {0x6B, "packssdw"}, {0x67, "packuswb"}}) {
        run({0x66, 0x0F, pack.first, 0xD9}, pack.second);
        run({0x66, 0x0F, pack.first, 0x18}, pack.second);
        run({0x66, 0x0F, pack.first, 0xC0}, pack.second);
    }
    // Element shifts by an immediate, at every count that matters: zero (no
    // bits move), one, width-1, exactly the width and past it -- x86 does not
    // mask the count, so the last two produce all zeros or all sign bits and
    // ARM64 cannot encode them as shifts at all.
    struct VShift { u8 op; u8 ext; const char* name; unsigned bits; };
    for (const VShift v : {VShift{0x71, 6, "psllw", 16}, VShift{0x71, 2, "psrlw", 16},
                           VShift{0x71, 4, "psraw", 16}, VShift{0x72, 6, "pslld", 32},
                           VShift{0x72, 2, "psrld", 32}, VShift{0x72, 4, "psrad", 32},
                           VShift{0x73, 6, "psllq", 64}, VShift{0x73, 2, "psrlq", 64}}) {
        for (const unsigned count : {0u, 1u, v.bits - 1, v.bits, v.bits + 1, 255u})
            run({0x66, 0x0F, v.op,
                 static_cast<u8>(0xC0 | (v.ext << 3) | 3),
                 static_cast<u8>(count)}, v.name);
    }
    for (const Form& form : kForms) {
        // Register source, at several destination/source pairings.
        run({0x66, 0x0F, form.op, 0xD9}, form.name);              // xmm3, xmm1
        run({0x66, 0x0F, form.op, 0xC0}, form.name);              // xmm0, xmm0
        run({0x66, 0x44, 0x0F, form.op, 0xCB}, form.name);        // xmm9, xmm3
        // Aligned memory source.
        run({0x66, 0x0F, form.op, 0x18}, form.name);              // xmm3, [rax]
    }
}
} // namespace
#endif
