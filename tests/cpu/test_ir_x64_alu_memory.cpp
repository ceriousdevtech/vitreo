// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64

/// @file test_ir_x64_alu_memory.cpp
/// @brief Long-mode IR emission: ALU with a memory operand (M11a).

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
// Long-mode ALU with a memory operand (M11a)
// =============================================================================

// Codegen varies per form and per immediate encoding, so this sweeps the whole
// accepted shape space rather than sampling it, with asymmetric operands so an
// operand-order mistake in a flag record cannot cancel out.
class IrEmit64AluMemory : public IrEmit64Memory {
protected:
    static constexpr std::array<u64, 6> kValues{
        0ull, 1ull, 0x7FFFFFFFFFFFFFFFull, 0x8000000000000000ull,
        0xFFFFFFFFFFFFFFFFull, 0x00000000FFFFFFFFull};

    /// RAX addresses the middle of a scratch window so a signed displacement
    /// stays mapped in both directions. The WHOLE window is restored before
    /// each run and compared after it: a lowering that wrote the right value
    /// to a neighbouring slot would pass a single-qword check.
    static constexpr GuestAddr kOperand = kData + 0x20;
    static constexpr u32 kWindow = 0x40;

    void fill_window(u64 stored) {
        for (u32 offset = 0; offset < kWindow; offset += 8) {
            const u64 value = kData + offset == kOperand
                ? stored : 0xC0DE0000'00000000ull + offset;
            ASSERT_TRUE(memory.write_u64(kData + offset, value).is_ok());
        }
    }
    void read_window(std::vector<u64>& out) {
        out.clear();
        for (u32 offset = 0; offset < kWindow; offset += 8) {
            const auto value = memory.read_u64(kData + offset);
            ASSERT_TRUE(value.is_ok());
            out.push_back(value.value());
        }
    }

    void expect_form_matches_interpreter(const std::vector<u8>& bytes,
                                         unsigned instructions = 1) {
        SCOPED_TRACE(testing::Message() << "form length " << bytes.size());
        // A fresh compiler per form: the same guest address holds different
        // code each time, and a reused translation would test the wrong bytes.
        jit = std::make_unique<JitCompiler>(memory);
        auto* native = compile(bytes);
        ASSERT_NE(native, nullptr);
        std::vector<u64> oracle_memory, actual_memory;
        for (const u64 stored : kValues) {
            for (const u64 reg : kValues) {
                auto oracle = seed();
                oracle.gpr.rax = kOperand;
                oracle.gpr.rcx = reg;
                oracle.gpr.rdx = ~reg;
                auto state = oracle;
                fill_window(stored);
                {
                    Interpreter interpreter(oracle, memory);
                    for (unsigned step = 0; step < instructions; ++step)
                        ASSERT_EQ(interpreter.step(), ExecResult::Continue);
                }
                read_window(oracle_memory);
                fill_window(stored);
                ASSERT_EQ(jit->execute(*native, state), JitExecResult::Continue);
                ASSERT_FALSE(state.pending_raise.active);
                read_window(actual_memory);
                EXPECT_EQ(actual_memory, oracle_memory)
                    << "guest memory diverged, stored=" << std::hex << stored
                    << " reg=" << reg;
                for (u8 index = 0; index < 16; ++index)
                    EXPECT_EQ(state.gpr.get64(index), oracle.gpr.get64(index))
                        << "GPR " << unsigned(index) << " stored=" << std::hex
                        << stored << " reg=" << reg;
                state.materialize_lazy_flags();
                oracle.materialize_lazy_flags();
                EXPECT_EQ(state.flags.rflags, oracle.flags.rflags)
                    << "RFLAGS stored=" << std::hex << stored << " reg=" << reg;
                EXPECT_EQ(state.rip, oracle.rip);
            }
        }
    }
};

TEST_F(IrEmit64AluMemory, RegisterOperandFormsMatchInterpreter) {
    ASSERT_TRUE(memory.map_region(kData, 0x1000, MemoryProtection::ReadWrite));
    // /r opcodes for ADD, OR, AND, SUB, XOR, CMP: memory destination (op 00)
    // and register destination (op 02), plus TEST which has only the former.
    for (const u8 base : {u8{0x00}, u8{0x08}, u8{0x20}, u8{0x28}, u8{0x30}, u8{0x38}}) {
        for (const bool wide : {false, true}) {
            // 01 /r: [rax] op= rcx      03 /r: rcx op= [rax]
            std::vector<u8> to_memory{static_cast<u8>(base + 1), 0x08};
            std::vector<u8> to_register{static_cast<u8>(base + 3), 0x08};
            if (wide) {
                to_memory.insert(to_memory.begin(), 0x48);
                to_register.insert(to_register.begin(), 0x48);
            }
            expect_form_matches_interpreter(to_memory);
            expect_form_matches_interpreter(to_register);
        }
    }
    for (const bool wide : {false, true}) {
        std::vector<u8> test_form{0x85, 0x08};  // test [rax], ecx/rcx
        if (wide) test_form.insert(test_form.begin(), 0x48);
        expect_form_matches_interpreter(test_form);
    }
}

TEST_F(IrEmit64AluMemory, ImmediateFormsSignExtendAtBothWidths) {
    ASSERT_TRUE(memory.map_region(kData, 0x1000, MemoryProtection::ReadWrite));
    // Intel SDM Vol. 2: 83 /n sign-extends imm8 and, at 64 bits, 81 /n and
    // F7 /0 sign-extend imm32. A zero-extending lowering passes for small
    // positive values only, so both signs of both encodings are swept.
    for (const u8 ext : {u8{0}, u8{1}, u8{4}, u8{5}, u8{6}, u8{7}}) {
        for (const u8 immediate : {u8{0x01}, u8{0x7F}, u8{0x80}, u8{0xFF}}) {
            for (const bool wide : {false, true}) {
                std::vector<u8> imm8{0x83, static_cast<u8>(0x00 | (ext << 3)),
                                     immediate};
                if (wide) imm8.insert(imm8.begin(), 0x48);
                expect_form_matches_interpreter(imm8);
            }
        }
        for (const u32 immediate : {0x00000001u, 0x7FFFFFFFu, 0x80000000u,
                                    0xFFFFFFFFu}) {
            for (const bool wide : {false, true}) {
                std::vector<u8> imm32{0x81, static_cast<u8>(0x00 | (ext << 3))};
                for (u32 byte = 0; byte < 4; ++byte)
                    imm32.push_back(static_cast<u8>(immediate >> (byte * 8)));
                if (wide) imm32.insert(imm32.begin(), 0x48);
                expect_form_matches_interpreter(imm32);
            }
        }
    }
    for (const u32 immediate : {0x00000001u, 0x7FFFFFFFu, 0x80000000u}) {
        for (const bool wide : {false, true}) {
            std::vector<u8> test_form{0xF7, 0x00};  // test dword/qword [rax], imm32
            for (u32 byte = 0; byte < 4; ++byte)
                test_form.push_back(static_cast<u8>(immediate >> (byte * 8)));
            if (wide) test_form.insert(test_form.begin(), 0x48);
            expect_form_matches_interpreter(test_form);
        }
    }
}

TEST_F(IrEmit64AluMemory, AddressingFormsReachTheSameOperand) {
    ASSERT_TRUE(memory.map_region(kData, 0x1000, MemoryProtection::ReadWrite));
    // The shared effective-address builder is reused unchanged: a signed
    // displacement in both directions must select the same qword the
    // interpreter selects, not a neighbouring one.
    expect_form_matches_interpreter({0x48,0x01,0x48,0x08});             // [rax+8]
    expect_form_matches_interpreter({0x48,0x01,0x88,0xF8,0xFF,0xFF,0xFF}); // [rax-8]
    expect_form_matches_interpreter({0x48,0x03,0x48,0x08});             // rcx += [rax+8]
    expect_form_matches_interpreter({0x48,0x39,0x48,0x08});             // cmp [rax+8], rcx
}

// The address register and the ALU operand are the same architectural
// register. The snapshot backend reloads operands around the access helper,
// so a stale reload would compute against the post-load value -- invisible
// whenever the two registers differ.
TEST_F(IrEmit64AluMemory, AliasedAddressAndOperandFormsMatchInterpreter) {
    ASSERT_TRUE(memory.map_region(kData, 0x1000, MemoryProtection::ReadWrite));
    expect_form_matches_interpreter({0x48,0x01,0x00});  // add [rax], rax
    expect_form_matches_interpreter({0x48,0x29,0x00});  // sub [rax], rax
    expect_form_matches_interpreter({0x48,0x03,0x00});  // add rax, [rax]
    expect_form_matches_interpreter({0x48,0x2B,0x00});  // sub rax, [rax]
    expect_form_matches_interpreter({0x48,0x39,0x00});  // cmp [rax], rax
    expect_form_matches_interpreter({0x48,0x85,0x00});  // test [rax], rax
}

// Several accesses in one block: each write-back goes through a helper that
// may clobber the scratch pool, so every later operand has to come back from
// its slot rather than from a register the helper owned.
TEST_F(IrEmit64AluMemory, MultipleAccessesInOneBlockKeepTheirOperands) {
    ASSERT_TRUE(memory.map_region(kData, 0x1000, MemoryProtection::ReadWrite));
    // add [rax], rcx / add rdx, [rax+8] / sub [rax], rdx / cmp [rax+8], rcx
    expect_form_matches_interpreter({0x48,0x01,0x08,
                                     0x48,0x03,0x50,0x08,
                                     0x48,0x29,0x10,
                                     0x48,0x39,0x48,0x08}, 4);
}

// The only value in this IR that CANNOT live in a register across an access:
// a read-modify-write's result is produced before the store and published as
// flags after it, so the store helper runs in between. That makes the
// invocation-local slot storage mandatory here no matter which passes run.
// The result of `add [rax], rcx` is read again AFTER the store, by the flag
// record. It crosses the store helper in X21, the one callee-saved register
// no scalar memory stub touches, rather than in a spill slot -- and the
// behaviour has to be identical either way, on the helper path the default
// configuration takes and through a nested store-helper call.
TEST_F(IrEmit64AluMemory, ReadModifyWriteResultCrossesTheStoreInTheSurvivorRegister) {
    ASSERT_TRUE(memory.map_region(kData, 0x1000, MemoryProtection::ReadWrite));
    jit = std::make_unique<JitCompiler>(memory);
    // add [rax], rcx ; push rbx -- two values that must outlive a store: the
    // sum (for its flag record) and the new RSP (committed after the push's
    // store, so a faulting push leaves RSP alone).
    auto* native = compile({0x48,0x01,0x08, 0x53});
    ASSERT_NE(native, nullptr);
    EXPECT_FALSE(uses_spill_backing(*native))
        << "a value crossing one store needs no slot";
    for (u64 add : {0ull, 1ull, 0x7FFFFFFFFFFFFFFFull, 0xFFFFFFFFFFFFFFFFull}) {
        ASSERT_TRUE(memory.write_u64(kData, 0x8000000000000001ull));
        auto state = seed();
        state.gpr.rax = kData;
        state.gpr.rcx = add;
        state.gpr.rbx = 0x0123456789ABCDEFull;
        state.gpr.rsp = kData + 0x800;
        auto oracle = state;
        Interpreter interpreter(oracle, memory);
        ASSERT_TRUE(memory.write_u64(kData, 0x8000000000000001ull));
        for (unsigned i = 0; i < 2; ++i)
            ASSERT_EQ(interpreter.step(), ExecResult::Continue);
        const u64 want_sum = memory.read_u64(kData).value_or(0);
        const u64 want_push = memory.read_u64(kData + 0x7F8).value_or(0);
        ASSERT_TRUE(memory.write_u64(kData, 0x8000000000000001ull));
        ASSERT_TRUE(memory.write_u64(kData + 0x7F8, 0));
        ASSERT_EQ(jit->execute(*native, state), JitExecResult::Continue);
        ASSERT_FALSE(state.pending_raise.active);
        EXPECT_EQ(memory.read_u64(kData).value_or(1), want_sum);
        EXPECT_EQ(memory.read_u64(kData + 0x7F8).value_or(1), want_push);
        for (u8 reg = 0; reg < 16; ++reg)
            EXPECT_EQ(state.gpr.get64(reg), oracle.gpr.get64(reg)) << unsigned(reg);
        state.materialize_lazy_flags();
        oracle.materialize_lazy_flags();
        EXPECT_EQ(state.flags.rflags, oracle.flags.rflags) << std::hex << add;
    }
}

// A read-modify-write whose write-back faults must leave both the memory and
// the architectural flags exactly as the preceding instruction left them:
// x86 publishes an instruction's flags only when it retires.
TEST_F(IrEmit64AluMemory, FaultingWriteBackPublishesThePrecedingFlags) {
    ASSERT_TRUE(memory.map_region(kData, 0x1000, MemoryProtection::Read));
    // add rdx, rdx  (publishes flags)   then   add [rax], rcx  (store faults)
    const std::vector<u8> bytes{0x48,0x01,0xD2, 0x48,0x01,0x08};
    auto* native = compile(bytes);
    ASSERT_NE(native, nullptr);
    auto oracle = seed();
    oracle.gpr.rax = kData;  // a read-only page: the write-back must fault
    oracle.gpr.rcx = 0x0123456789ABCDEFull;
    oracle.gpr.rdx = 0x4000000000000000ull;  // doubling overflows: OF=1, SF=1
    auto state = oracle;
    {
        Interpreter interpreter(oracle, memory);
        ASSERT_EQ(interpreter.step(), ExecResult::Continue);  // only the ADD
    }
    const auto before = memory.read_u64(kData);
    ASSERT_TRUE(before.is_ok());
    (void)jit->execute(*native, state);
    ASSERT_TRUE(state.pending_raise.active);
    EXPECT_EQ(state.pending_raise.code, 0xC0000005u);
    ASSERT_GE(state.pending_raise.num_args, 2u);
    EXPECT_EQ(state.pending_raise.args[0], 1u);  // write attempted
    EXPECT_EQ(state.pending_raise.args[1], kData);
    EXPECT_EQ(state.rip, oracle.rip);
    EXPECT_EQ(memory.read_u64(kData).value_or(~u64{0}), before.value());
    for (u8 index = 0; index < 16; ++index)
        EXPECT_EQ(state.gpr.get64(index), oracle.gpr.get64(index))
            << unsigned(index);
    state.materialize_lazy_flags();
    oracle.materialize_lazy_flags();
    EXPECT_EQ(state.flags.rflags, oracle.flags.rflags)
        << "the attempted ALU must not have published its flags";
}
} // namespace
#endif
