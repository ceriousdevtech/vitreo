// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64

/// @file test_ir_x64_widen.cpp
/// @brief Long-mode IR emission: MOVZX / MOVSX / MOVSXD widening (M11b).

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
// Long-mode widening: MOVZX / MOVSX / MOVSXD (M11b)
// =============================================================================

// The destination is a whole 32- or 64-bit register, so these need no
// partial-register merge and define no flags. What they DO need is that a
// narrow source read is an extract and never an alias of the pin, and that a
// 32-bit destination still zero-extends to 64 (Intel SDM Vol. 2B).
class IrEmit64Widen : public IrEmit64Memory {
protected:
    static constexpr std::array<u64, 7> kValues{
        0ull, 1ull, 0x7Full, 0x80ull, 0xFFull, 0x7FFFull, 0xFFFFFFFFFFFFFFFFull};

    void expect_widen_matches_interpreter(const std::vector<u8>& bytes) {
        SCOPED_TRACE(testing::Message() << "widen form, " << bytes.size() << " bytes");
        jit = std::make_unique<JitCompiler>(memory);
        auto* native = compile(bytes);
        ASSERT_NE(native, nullptr);
        for (const u64 seed_value : kValues) {
            auto oracle = seed();
            // Every source register the forms below can name, so a lowering
            // that read the wrong architectural register is visible.
            for (u8 index = 0; index < 16; ++index)
                oracle.gpr.set64(index, seed_value ^ (0x0101010101010101ull * index));
            auto state = oracle;
            {
                Interpreter interpreter(oracle, memory);
                ASSERT_EQ(interpreter.step(), ExecResult::Continue);
            }
            ASSERT_EQ(jit->execute(*native, state), JitExecResult::Continue);
            ASSERT_FALSE(state.pending_raise.active);
            for (u8 index = 0; index < 16; ++index)
                EXPECT_EQ(state.gpr.get64(index), oracle.gpr.get64(index))
                    << "GPR " << unsigned(index) << " seed=" << std::hex << seed_value;
            state.materialize_lazy_flags();
            oracle.materialize_lazy_flags();
            EXPECT_EQ(state.flags.rflags, oracle.flags.rflags)
                << "MOVZX/MOVSX define no flags";
            EXPECT_EQ(state.rip, oracle.rip);
        }
    }
};

TEST_F(IrEmit64Widen, ByteAndWordSourcesAcrossEveryRegister) {
    // 0F B6 /r MOVZX r32,r/m8   0F B7 /r MOVZX r32,r/m16
    // 0F BE /r MOVSX r32,r/m8   0F BF /r MOVSX r32,r/m16
    for (const u8 op : {u8{0xB6}, u8{0xB7}, u8{0xBE}, u8{0xBF}}) {
        for (u8 src = 0; src < 8; ++src) {
            // No REX: a Reg8 index of 4-7 is AH/CH/DH/BH, which this IR
            // encodes as the index itself. That path must be exercised.
            const u8 modrm = static_cast<u8>(0xC0 | (1 << 3) | src);  // dst=ecx
            expect_widen_matches_interpreter({0x0F, op, modrm});
        }
    }
}

// `movzx ecx, cl` (CRC table indexing): the byte read is masked once, by
// its Trunc, so the widening adds no second mask on the dependency chain.
TEST_F(IrEmit64Widen, ByteExtractNeedsNoSecondMask) {
    const std::vector<u8> bytes{0x0F, 0xB6, 0xC9};
    expect_widen_matches_interpreter(bytes);
    auto* native = compile(bytes);
    ASSERT_NE(native, nullptr);
    const auto* words = static_cast<const u32*>(native->code);
    const usize n = native->code_size / sizeof(u32);
    usize masks = 0;
    for (usize i = 0; i < n; ++i) {
        // UXTB Wd, Wn (UBFM Wd, Wn, #0, #7) and UXTH (#0, #15).
        if ((words[i] & 0xFFFFFC00u) == 0x53001C00u ||
            (words[i] & 0xFFFFFC00u) == 0x53003C00u) ++masks;
    }
    EXPECT_EQ(masks, 1u) << ir::print(jit->ir_block());
}

// The CRC inner loop `movzx ecx, byte [rdx]` ... `movzx ecx, cl`: the mask or
// byte load writes ECX's pin itself. A result computed into a scratch and
// then copied by a MOV W puts a move on the loop-carried chain, which Apple
// cores do not eliminate for W registers. Results still match the interpreter.
TEST_F(IrEmit64Widen, ZeroExtendedSourceIsComputedIntoTheDestinationPin) {
    ASSERT_TRUE(memory.map_region(kData, 0x1000, MemoryProtection::ReadWrite));
    ASSERT_TRUE(memory.write_u64(kData + 8, 0x8090A0B0C0D0E0F0ull).is_ok());
    const std::vector<std::vector<u8>> forms{
        {0x0F, 0xB6, 0xC9},          // movzx ecx, cl
        {0x0F, 0xB7, 0xC9},          // movzx ecx, cx
        {0x0F, 0xB6, 0x48, 0x08},    // movzx ecx, byte [rax+8]
        {0x0F, 0xB7, 0x48, 0x08},    // movzx ecx, word [rax+8]
    };
    for (const auto& bytes : forms) {
        SCOPED_TRACE(testing::Message() << "form of " << bytes.size() << " bytes");
        if (bytes.size() == 3) expect_widen_matches_interpreter(bytes);
        jit = std::make_unique<JitCompiler>(memory);
        auto* native = compile(bytes);
        ASSERT_NE(native, nullptr);
        const auto* words = static_cast<const u32*>(native->code);
        const usize n = native->code_size / sizeof(u32);
        for (usize i = 1; i < n; ++i) {
            // MOV Wd, Wm (ORR Wd, WZR, Wm) copying what the op before wrote.
            if ((words[i] & 0xFFE0FFE0u) != 0x2A0003E0u) continue;
            const u32 rm = (words[i] >> 16) & 31;
            const u32 rd = words[i] & 31;
            EXPECT_FALSE(rd != rm && (words[i - 1] & 31) == rm &&
                         ((words[i - 1] & 0xFFFFFC00u) == 0x53001C00u ||
                          (words[i - 1] & 0xFFFFFC00u) == 0x53003C00u ||
                          (words[i - 1] & 0xBFFFFC00u) == 0x38BFC000u))   // LDAPRB/H
                << "word " << i << ": " << std::hex << words[i - 1] << " then "
                << words[i] << "\n" << ir::print(jit->ir_block());
        }
    }
    auto oracle = seed();
    oracle.gpr.set64(0, kData);
    oracle.gpr.set64(1, 0xFFFFFFFFFFFFFFFFull);
    auto state = oracle;
    for (const auto& bytes : {forms[2], forms[3]}) {
        jit = std::make_unique<JitCompiler>(memory);
        auto* native = compile(bytes);
        ASSERT_NE(native, nullptr);
        oracle.rip = state.rip = kCode;
        {
            Interpreter interpreter(oracle, memory);
            ASSERT_EQ(interpreter.step(), ExecResult::Continue);
        }
        ASSERT_EQ(jit->execute(*native, state), JitExecResult::Continue);
        EXPECT_EQ(state.gpr.get64(1), oracle.gpr.get64(1));
    }
}

TEST_F(IrEmit64Widen, SixtyFourBitDestinationsAndExtendedRegisters) {
    for (const u8 op : {u8{0xB6}, u8{0xB7}, u8{0xBE}, u8{0xBF}}) {
        for (const u8 src : {u8{0}, u8{1}, u8{3}, u8{8}, u8{12}, u8{15}}) {
            // REX.W + REX.B: 64-bit destination, extended source. Indices 4-7
            // are deliberately absent — with REX they name SPL/BPL/SIL/DIL,
            // which this tranche declines rather than misread.
            const u8 rex = static_cast<u8>(0x48 | (src >= 8 ? 1 : 0));
            const u8 modrm = static_cast<u8>(0xC0 | (2 << 3) | (src & 7));  // dst=rdx
            expect_widen_matches_interpreter({rex, 0x0F, op, modrm});
        }
    }
    // 63 /r MOVSXD r64, r/m32 -- sign-extension all the way from 32 bits.
    for (const u8 src : {u8{0}, u8{4}, u8{9}, u8{15}}) {
        const u8 rex = static_cast<u8>(0x48 | (src >= 8 ? 1 : 0));
        expect_widen_matches_interpreter(
            {rex, 0x63, static_cast<u8>(0xC0 | (3 << 3) | (src & 7))});
    }
}

// Memory sources: the load is the only faulting op and everything it feeds
// happens after it, so a fault commits nothing. The scratch window is
// compared whole so a narrow load that read the wrong width, or wrote back at
// all, is visible.
TEST_F(IrEmit64Widen, MemorySourcesAtEveryWidth) {
    ASSERT_TRUE(memory.map_region(kData, 0x1000, MemoryProtection::ReadWrite));
    for (u32 offset = 0; offset < 0x40; offset += 8)
        ASSERT_TRUE(memory.write_u64(kData + offset,
                                     0x8090A0B0C0D0E0F0ull + offset).is_ok());
    struct Form { std::vector<u8> bytes; const char* name; };
    const std::vector<Form> forms{
        {{0x0F,0xB6,0x48,0x08}, "movzx ecx,byte [rax+8]"},
        {{0x0F,0xB7,0x48,0x08}, "movzx ecx,word [rax+8]"},
        {{0x0F,0xBE,0x48,0x08}, "movsx ecx,byte [rax+8]"},
        {{0x0F,0xBF,0x48,0x08}, "movsx ecx,word [rax+8]"},
        {{0x48,0x0F,0xB6,0x48,0x08}, "movzx rcx,byte [rax+8]"},
        {{0x48,0x0F,0xBE,0x48,0x08}, "movsx rcx,byte [rax+8]"},
        {{0x48,0x0F,0xB7,0x48,0x08}, "movzx rcx,word [rax+8]"},
        {{0x48,0x0F,0xBF,0x48,0x08}, "movsx rcx,word [rax+8]"},
        {{0x48,0x63,0x48,0x08},      "movsxd rcx,dword [rax+8]"},
    };
    for (const Form& form : forms) {
        SCOPED_TRACE(form.name);
        jit = std::make_unique<JitCompiler>(memory);
        auto* native = compile(form.bytes);
        ASSERT_NE(native, nullptr);
        for (const u64 stored : {0ull, 0x7Full, 0x80ull, 0xFFull, 0x7FFFull,
                                 0x8000ull, 0xFFFFFFFFull, 0xFFFFFFFFFFFFFFFFull}) {
            auto oracle = seed();
            oracle.gpr.rax = kData;
            oracle.gpr.rcx = 0x1111111111111111ull;
            ASSERT_TRUE(memory.write_u64(kData + 8, stored).is_ok());
            auto state = oracle;
            {
                Interpreter interpreter(oracle, memory);
                ASSERT_EQ(interpreter.step(), ExecResult::Continue);
            }
            const auto oracle_memory = memory.read_u64(kData + 8);
            ASSERT_EQ(jit->execute(*native, state), JitExecResult::Continue);
            EXPECT_EQ(memory.read_u64(kData + 8).value_or(1), oracle_memory.value_or(2))
                << "a widening load writes no memory";
            for (u8 index = 0; index < 16; ++index)
                EXPECT_EQ(state.gpr.get64(index), oracle.gpr.get64(index))
                    << "GPR " << unsigned(index) << " stored=" << std::hex << stored;
            state.materialize_lazy_flags();
            oracle.materialize_lazy_flags();
            EXPECT_EQ(state.flags.rflags, oracle.flags.rflags);
        }
    }
}

// An unmapped source must fault with the guest's own RIP and commit nothing.
TEST_F(IrEmit64Widen, FaultingNarrowLoadCommitsNothing) {
    jit = std::make_unique<JitCompiler>(memory);
    auto* native = compile({0x48,0x0F,0xBE,0x08});  // movsx rcx, byte [rax]
    ASSERT_NE(native, nullptr);
    auto oracle = seed();
    oracle.gpr.rax = 0x0000'4000'0000'0000ull;  // mapped by nothing
    oracle.gpr.rcx = 0x1234567890ABCDEFull;
    auto state = oracle;
    (void)jit->execute(*native, state);
    ASSERT_TRUE(state.pending_raise.active);
    EXPECT_EQ(state.pending_raise.code, 0xC0000005u);
    ASSERT_GE(state.pending_raise.num_args, 2u);
    EXPECT_EQ(state.pending_raise.args[0], 0u);
    EXPECT_EQ(state.pending_raise.args[1], oracle.gpr.rax);
    EXPECT_EQ(state.rip, oracle.rip) << "the faulting instruction's own RIP";
    for (u8 index = 0; index < 16; ++index)
        EXPECT_EQ(state.gpr.get64(index), oracle.gpr.get64(index)) << unsigned(index);
}

// A 16-bit widening destination is a PARTIAL write: `movzx ax, bl` leaves
// EAX[31:16] alone, unlike the 32- and 64-bit destinations.
TEST_F(IrEmit64Widen, SixteenBitDestinationMergesRatherThanReplaces) {
    for (const u8 op : {u8{0xB6}, u8{0xBE}}) {   // MOVZX/MOVSX r16, r/m8
        for (u8 src = 0; src < 8; ++src) {
            jit = std::make_unique<JitCompiler>(memory);
            auto* native = compile({0x66, 0x0F, op,
                                    static_cast<u8>(0xC0 | (1 << 3) | src)});
            ASSERT_NE(native, nullptr);
            for (const u64 value : {0ull, 0x7Full, 0x80ull, 0xFFull,
                                    0xFFFFFFFFFFFFFFFFull}) {
                auto oracle = seed();
                for (u8 index = 0; index < 16; ++index)
                    oracle.gpr.set64(index, value ^ (0x1234123412341234ull * index));
                auto state = oracle;
                {
                    Interpreter interpreter(oracle, memory);
                    ASSERT_EQ(interpreter.step(), ExecResult::Continue);
                }
                ASSERT_EQ(jit->execute(*native, state), JitExecResult::Continue);
                for (u8 index = 0; index < 16; ++index)
                    EXPECT_EQ(state.gpr.get64(index), oracle.gpr.get64(index))
                        << "GPR " << unsigned(index) << " value=" << std::hex << value;
            }
        }
    }
}

// moffs: MOV between the accumulator and a 64-bit absolute address.
TEST_F(IrEmit64Widen, AbsoluteAccumulatorFormsAtEveryWidth) {
    ASSERT_TRUE(memory.map_region(kData, 0x1000, MemoryProtection::ReadWrite));
    const auto address_bytes = [](GuestAddr address) {
        std::vector<u8> out;
        for (u32 i = 0; i < 8; ++i)
            out.push_back(static_cast<u8>(address >> (i * 8)));
        return out;
    };
    for (const u8 opcode : {u8{0xA0}, u8{0xA2}, u8{0xA1}, u8{0xA3}}) {
        for (const bool word : {false, true}) {
            if ((opcode == 0xA0 || opcode == 0xA2) && word) continue;
            std::vector<u8> bytes;
            if (word) bytes.push_back(0x66);
            bytes.push_back(opcode);
            for (const u8 byte : address_bytes(kData)) bytes.push_back(byte);
            if ((opcode == 0xA1 || opcode == 0xA3) && !word) continue;
            SCOPED_TRACE(testing::Message() << "opcode " << std::hex << unsigned(opcode));
            jit = std::make_unique<JitCompiler>(memory);
            auto* native = compile(bytes);
            ASSERT_NE(native, nullptr);
            for (const u64 value : {0ull, 0x5Aull, 0xFFFFull, 0xFFFFFFFFFFFFFFFFull}) {
                auto oracle = seed();
                oracle.gpr.rax = value ^ 0x1122334455667788ull;
                ASSERT_TRUE(memory.write_u64(kData, value).is_ok());
                auto state = oracle;
                {
                    Interpreter interpreter(oracle, memory);
                    ASSERT_EQ(interpreter.step(), ExecResult::Continue);
                }
                const auto oracle_memory = memory.read_u64(kData);
                ASSERT_TRUE(memory.write_u64(kData, value).is_ok());
                ASSERT_EQ(jit->execute(*native, state), JitExecResult::Continue);
                EXPECT_EQ(memory.read_u64(kData).value_or(1), oracle_memory.value_or(2));
                for (u8 index = 0; index < 16; ++index)
                    EXPECT_EQ(state.gpr.get64(index), oracle.gpr.get64(index))
                        << unsigned(index);
            }
        }
    }
}

// A 32-bit destination must clear bits 63:32 even though the source is narrow.
TEST_F(IrEmit64Widen, ThirtyTwoBitDestinationClearsTheHighHalf) {
    jit = std::make_unique<JitCompiler>(memory);
    auto* native = compile({0x0F, 0xB6, 0xC8});  // movzx ecx, al
    ASSERT_NE(native, nullptr);
    auto state = seed();
    state.gpr.rax = 0x1122334455667788ull;
    state.gpr.rcx = 0xFFFFFFFFFFFFFFFFull;
    ASSERT_EQ(jit->execute(*native, state), JitExecResult::Continue);
    EXPECT_EQ(state.gpr.rcx, 0x88ull) << "a 32-bit destination zero-extends to 64";
}

// The REX spelling of a byte register names SPL/BPL/SIL/DIL, which this IR
// cannot encode apart from AH/CH/DH/BH. It must decline, not read RSP's low
// byte as AH. Losing this check would be silent and produce wrong results.
// The two spellings of "Reg8 index 6" name DIFFERENT architectural registers:
// with a REX prefix it is SIL, without one it is DH. The IR normalizes the
// high bytes into their own index space so the distinction survives decoding,
// and this runs both against the interpreter with RSI and RDX seeded apart, so
// confusing them cannot pass.
TEST_F(IrEmit64Widen, RexAndLegacyByteSpellingsNameDifferentRegisters) {
    for (const bool rex : {false, true}) {
        SCOPED_TRACE(rex ? "movzx eax, sil" : "movzx eax, dh");
        std::vector<u8> bytes{0x0F, 0xB6, 0xC6};
        if (rex) bytes.insert(bytes.begin(), 0x40);
        jit = std::make_unique<JitCompiler>(memory);
        auto* native = compile(bytes);
        ASSERT_NE(native, nullptr);
        auto oracle = seed();
        oracle.gpr.rsi = 0x1122334455667711ull;   // SIL = 0x11
        oracle.gpr.rdx = 0x99887766554433EEull;   // DH  = 0x33
        oracle.gpr.rax = 0xFFFFFFFFFFFFFFFFull;
        auto state = oracle;
        {
            Interpreter interpreter(oracle, memory);
            ASSERT_EQ(interpreter.step(), ExecResult::Continue);
        }
        ASSERT_EQ(jit->execute(*native, state), JitExecResult::Continue);
        EXPECT_EQ(state.gpr.rax, oracle.gpr.rax);
        EXPECT_EQ(state.gpr.rax, rex ? 0x11ull : 0x33ull)
            << "the other spelling's register would give the other value";
        for (u8 index = 0; index < 16; ++index)
            EXPECT_EQ(state.gpr.get64(index), oracle.gpr.get64(index)) << unsigned(index);
    }
}

// Intel SDM Vol. 2A CBW/CWDE/CDQE (98) and CWD/CDQ/CQO (99). The operand
// size picks the form; REX.W outranks 66, and a REX without W is the
// default size. No flags change.
struct AccSignExtendCase {
    std::vector<u8> bytes;
    u64 rax, rdx;          // input
    u64 want_rax, want_rdx;  // Intel SDM result
};

TEST_F(IrEmit64Widen, AccumulatorSignExtensionAtEveryOperandSize) {
    const std::vector<AccSignExtendCase> cases{
        // CBW: AX := SignExtend(AL); EAX[31:16] and RAX[63:32] are kept.
        {{0x66, 0x98}, 0x1122334455667780ull, 0, 0x112233445566FF80ull, 0},
        {{0x66, 0x98}, 0x112233445566807Full, 0, 0x112233445566007Full, 0},
        // CWDE: EAX := SignExtend(AX); a 32-bit write clears RAX[63:32].
        {{0x98}, 0x1122334455668000ull, 0, 0x00000000FFFF8000ull, 0},
        {{0x40, 0x98}, 0x1122334455667FFFull, 0, 0x0000000000007FFFull, 0},
        // CDQE: RAX := SignExtend(EAX).
        {{0x48, 0x98}, 0x1122334480000000ull, 0, 0xFFFFFFFF80000000ull, 0},
        {{0x66, 0x48, 0x98}, 0xFFFFFFFF7FFFFFFFull, 0, 0x000000007FFFFFFFull, 0},
        // CWD: DX := sign of AX; RDX[63:16] kept, RAX unchanged.
        {{0x66, 0x99}, 0xAAAAAAAAAAAA8000ull, 0x1111222233334444ull,
         0xAAAAAAAAAAAA8000ull, 0x111122223333FFFFull},
        {{0x66, 0x99}, 0xAAAAAAAAAAAA7FFFull, 0x111122223333FFFFull,
         0xAAAAAAAAAAAA7FFFull, 0x1111222233330000ull},
        // CDQ: EDX := sign of EAX; RDX[63:32] cleared, RAX unchanged.
        {{0x99}, 0xAAAAAAAA80000000ull, 0x1111222233334444ull,
         0xAAAAAAAA80000000ull, 0x00000000FFFFFFFFull},
        {{0x99}, 0xFFFFFFFF7FFFFFFFull, 0xFFFFFFFFFFFFFFFFull,
         0xFFFFFFFF7FFFFFFFull, 0},
        // CQO: RDX := sign of RAX.
        {{0x48, 0x99}, 0x8000000000000000ull, 0x1234ull,
         0x8000000000000000ull, 0xFFFFFFFFFFFFFFFFull},
        {{0x48, 0x99}, 0x7FFFFFFFFFFFFFFFull, 0xFFFFFFFFFFFFFFFFull,
         0x7FFFFFFFFFFFFFFFull, 0},
    };
    for (const auto& c : cases) {
        SCOPED_TRACE(testing::Message() << std::hex << "rax=" << c.rax
                                        << " first byte=" << unsigned(c.bytes[0])
                                        << " len=" << c.bytes.size());
        jit = std::make_unique<JitCompiler>(memory);
        auto* native = compile(c.bytes);
        ASSERT_NE(native, nullptr);
        auto oracle = seed();
        for (u8 index = 0; index < 16; ++index)
            oracle.gpr.set64(index, 0x0F0F0F0F0F0F0F0Full * (index + 1));
        oracle.gpr.rax = c.rax;
        oracle.gpr.rdx = c.rdx;
        auto state = oracle;
        {
            Interpreter interpreter(oracle, memory);
            ASSERT_EQ(interpreter.step(), ExecResult::Continue);
        }
        EXPECT_EQ(oracle.gpr.rax, c.want_rax) << "interpreter vs SDM";
        EXPECT_EQ(oracle.gpr.rdx, c.want_rdx) << "interpreter vs SDM";
        ASSERT_EQ(jit->execute(*native, state), JitExecResult::Continue);
        ASSERT_FALSE(state.pending_raise.active);
        for (u8 index = 0; index < 16; ++index)
            EXPECT_EQ(state.gpr.get64(index), oracle.gpr.get64(index)) << unsigned(index);
        state.materialize_lazy_flags();
        oracle.materialize_lazy_flags();
        EXPECT_EQ(state.flags.rflags, oracle.flags.rflags) << "no flag is affected";
        EXPECT_EQ(state.rip, oracle.rip);
    }
}

// Inside a block whose earlier ALU op left lazy flags, so a lowering that
// clobbered or republished the flag record would be visible.
class IrEmit64AccSignExtend : public IrEmit64NarrowAlu {};

TEST_F(IrEmit64AccSignExtend, KeepsThePriorFlagsInsideABlock) {
    ASSERT_TRUE(memory.map_region(kData, 0x1000, MemoryProtection::ReadWrite));
    for (const auto& form : std::vector<std::vector<u8>>{
             {0x66, 0x98}, {0x98}, {0x48, 0x98},
             {0x66, 0x99}, {0x99}, {0x48, 0x99}}) {
        std::vector<u8> body{0x01, 0xD1};  // add ecx, edx
        body.insert(body.end(), form.begin(), form.end());
        body.insert(body.end(), {0x83, 0xD6, 0x00});  // adc esi, 0
        expect_alu_matches_interpreter(body, 3);
    }
}
} // namespace
#endif
