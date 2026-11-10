// SPDX-License-Identifier: LGPL-2.1-only
// Intel SDM Vol.3A Event13; instruction_length_probe{32,64}.expected.
#include <gtest/gtest.h>
#include <vector>
#include "core/cpu/decode/decoder.hpp"
#include "core/cpu/interpreter/interpreter.hpp"
#include "core/cpu/jit/jit_compiler.hpp"
#include "runtime/execution/block_decoder.hpp"

using namespace vitreo;
using namespace vitreo::cpu;

namespace {
std::vector<u8> padded(usize prefixes, std::initializer_list<u8> tail) {
    std::vector<u8> code(prefixes, 0x66);
    code.insert(code.end(), tail);
    return code;
}
const std::vector<std::vector<u8>>& overlength_forms() {
    static const std::vector<std::vector<u8>> forms = {
        padded(15, {0x90}),                    // opcode after byte15
        padded(14, {0xB8, 0x22, 0x11}),        // immediate
        padded(14, {0x89, 0x02}),              // ModRM, would write memory
        padded(13, {0x8B, 0x04, 0x22}),        // SIB
        padded(10, {0x8B, 0x82, 0, 0, 0, 0}), // displacement
    };
    return forms;
}
void seed(CpuState& cpu, bool wide, GuestAddr rip) {
    if (wide) cpu.init_long_mode(); else cpu.init_protected_mode();
    cpu.rip = rip;
    cpu.gpr.rax = 0x12345678;
    cpu.gpr.rdx = 0x4000;
    cpu.gpr.rsp = 0x4FF0;
    cpu.flags.rflags = 0x203; // CF set, as in the native probe
}
void expect_length_fault(Interpreter& interpreter, CpuState& cpu,
                         GuestAddr rip, bool wide, u64 expected_flags = 0x203) {
    EXPECT_EQ(interpreter.step(), ExecResult::Exception);
    const auto& ctx = interpreter.context();
    EXPECT_EQ(ctx.exception, CpuException::GeneralProtection);
    EXPECT_EQ(ctx.exception_error, 0u);
    EXPECT_EQ(ctx.exception_addr, wide ? ~u64{0} : u64{0xFFFFFFFFu});
    EXPECT_EQ(ctx.exception_num_params, 2u);
    EXPECT_EQ(cpu.rip, rip);
    EXPECT_EQ(cpu.gpr.rax, 0x12345678u);
    EXPECT_EQ(cpu.gpr.rdx, 0x4000u);
    EXPECT_EQ(cpu.gpr.rsp, 0x4FF0u);
    cpu.materialize_lazy_flags();
    EXPECT_EQ(cpu.flags.rflags, expected_flags);
}
}

TEST(InstructionLength, FifteenBytesDecodeAndLimitResetsForEachInstruction) {
    for (const auto mode : {DecodeMode::Mode16, DecodeMode::Mode32, DecodeMode::Mode64}) {
        auto bytes = padded(14, {0x90, 0x90});
        MemoryByteReader reader(bytes.data(), bytes.size());
        DecodedInstruction insn;
        ASSERT_EQ(decode_instruction(reader, mode, insn), DecodeError::Success);
        EXPECT_EQ(insn.length, 15u);
        ASSERT_EQ(decode_instruction(reader, mode, insn), DecodeError::Success);
        EXPECT_EQ(insn.length, 1u);
        EXPECT_EQ(reader.position(), 16u);
        const auto overlong = padded(15, {0x90});
        MemoryByteReader overlong_reader(overlong.data(), overlong.size());
        EXPECT_EQ(decode_instruction(overlong_reader, mode, insn), DecodeError::InstructionTooLong);
        EXPECT_FALSE(insn.is_valid());
    }
}

TEST(InstructionLength, RexIsAModeSpecificPrefixAndImm64ReadsAreBounded) {
    for (const usize prefixes : {14u, 15u}) {
        std::vector<u8> bytes(prefixes, 0x48);
        bytes.push_back(0x90);
        DecodedInstruction insn;
        MemoryByteReader wide(bytes.data(), bytes.size());
        EXPECT_EQ(decode_instruction(wide, DecodeMode::Mode64, insn),
                  prefixes == 14 ? DecodeError::Success : DecodeError::InstructionTooLong);
        if (prefixes == 14) EXPECT_EQ(insn.length, 15u);
        MemoryByteReader narrow(bytes.data(), bytes.size());
        EXPECT_EQ(decode_instruction(narrow, DecodeMode::Mode32, insn), DecodeError::Success);
        EXPECT_EQ(insn.length, 1u); // 48h is DEC outside long mode, not REX.
    }
    const auto bytes = padded(6, {0x48, 0xB8, 1, 2, 3, 4, 5, 6, 7, 8});
    MemoryByteReader reader(bytes.data(), bytes.size());
    DecodedInstruction insn;
    EXPECT_EQ(decode_instruction(reader, DecodeMode::Mode64, insn), DecodeError::InstructionTooLong);
    EXPECT_EQ(reader.position(), 15u);
    EXPECT_FALSE(insn.is_valid());
}

TEST(InstructionLength, EveryOperandReadAndEarlyReturnObeysTheLimit) {
    for (const auto mode : {DecodeMode::Mode32, DecodeMode::Mode64}) {
        for (const auto& bytes : overlength_forms()) {
            MemoryByteReader reader(bytes.data(), bytes.size());
            DecodedInstruction insn;
            EXPECT_EQ(decode_instruction(reader, mode, insn), DecodeError::InstructionTooLong);
            EXPECT_LE(reader.position(), 15u);
            EXPECT_FALSE(insn.is_valid());
        }
        // BOUND/LES/LDS and EVEX take early returns in the decode worker.
        for (const u8 escape : {u8{0x62}, u8{0xC4}, u8{0xC5}}) {
            auto bytes = padded(14, {escape, 0x00, 0x00, 0x00});
            MemoryByteReader reader(bytes.data(), bytes.size());
            DecodedInstruction insn;
            EXPECT_EQ(decode_instruction(reader, mode, insn), DecodeError::InstructionTooLong);
            EXPECT_LE(reader.position(), 15u);
            EXPECT_FALSE(insn.is_valid());
        }
    }
}

TEST(InstructionLength, ValidFifteenByteInstructionAtPageEndExecutes) {
    for (const bool wide : {false, true}) {
        GuestMemory memory;
        ASSERT_TRUE(memory.map_region(0x1000, 0x1000, MemoryProtection::ReadWriteExecute));
        const auto bytes = padded(14, {0x90});
        ASSERT_TRUE(memory.write_bytes(0x2000 - 15, bytes.data(), bytes.size()));
        CpuState cpu;
        seed(cpu, wide, 0x2000 - 15);
        Interpreter interpreter(cpu, memory);
        EXPECT_EQ(interpreter.step(), ExecResult::Continue);
        EXPECT_EQ(cpu.rip, 0x2000u);
        EXPECT_EQ(cpu.gpr.rax, 0x12345678u);
        EXPECT_EQ(cpu.flags.rflags, 0x203u);
        EXPECT_EQ(interpreter.step(), ExecResult::Exception);
        EXPECT_EQ(interpreter.context().exception, CpuException::PageFault);
        EXPECT_EQ(interpreter.context().exception_addr, 0x2000u);
    }
}

TEST(InstructionLength, InterpreterFaultsBeforeAnyInstructionEffects) {
    for (const bool wide : {false, true}) {
        GuestMemory memory;
        ASSERT_TRUE(memory.map_region(0x1000, 0x4000, MemoryProtection::ReadWriteExecute));
        for (const auto& bytes : overlength_forms()) {
            ASSERT_TRUE(memory.write_bytes(0x1000, bytes.data(), bytes.size()));
            ASSERT_TRUE(memory.write_u32(0x4000, 0x76543210));
            CpuState cpu;
            seed(cpu, wide, 0x1000);
            Interpreter interpreter(cpu, memory);
            expect_length_fault(interpreter, cpu, 0x1000, wide);
            EXPECT_EQ(memory.read_u32(0x4000).value(), 0x76543210u);
        }
    }
}

TEST(InstructionLength, OracleFetchBoundaryPriority) {
    for (const bool wide : {false, true}) {
        for (const usize readable : {14u, 15u, 16u}) {
            for (const auto& bytes : {padded(15, {0x90}), padded(14, {0xB8, 0x22, 0x11})}) {
                GuestMemory memory;
                ASSERT_TRUE(memory.map_region(0x1000, 0x1000, MemoryProtection::ReadWriteExecute));
                const GuestAddr start = 0x2000 - readable;
                ASSERT_TRUE(memory.write_bytes(start, bytes.data(), readable));
                CpuState cpu;
                seed(cpu, wide, start);
                Interpreter interpreter(cpu, memory);
                if (readable >= 15) {
                    expect_length_fault(interpreter, cpu, start, wide);
                } else {
                    EXPECT_EQ(interpreter.step(), ExecResult::Exception);
                    EXPECT_EQ(interpreter.context().exception, CpuException::PageFault);
                    EXPECT_EQ(interpreter.context().exception_error, 8u);
                    EXPECT_EQ(interpreter.context().exception_addr, 0x2000u);
                    EXPECT_EQ(cpu.rip, start);
                    EXPECT_EQ(cpu.gpr.rax, 0x12345678u);
                }
            }
        }
    }
}

TEST(InstructionLength, JitCommitsValidPrefixThenUsesInterpreterForFault) {
    for (const bool wide : {false, true}) {
        cli::reset_decode_memo();
        GuestMemory memory;
        ASSERT_TRUE(memory.map_region(0x1000, 0x4000, MemoryProtection::ReadWriteExecute));
        std::vector<u8> bytes{0xB9, 0x55, 0, 0, 0, 0x83, 0xC1, 0x01}; // mov ecx,55h; add ecx,1
        const auto invalid = padded(14, {0x89, 0x02});
        bytes.insert(bytes.end(), invalid.begin(), invalid.end());
        ASSERT_TRUE(memory.write_bytes(0x1000, bytes.data(), bytes.size()));
        ASSERT_TRUE(memory.write_u32(0x4000, 0x76543210));
        auto block = cli::decode_basic_block(memory, 0x1000, wide);
        ASSERT_EQ(block.instruction_count, 2u);
        EXPECT_EQ(block.byte_length, 8u);
        jit::JitCompiler jit(memory);
        auto* compiled = jit.compile(block);
        ASSERT_NE(compiled, nullptr);
        CpuState cpu;
        seed(cpu, wide, 0x1000);
        EXPECT_EQ(jit.execute(*compiled, cpu), jit::JitExecResult::Continue);
        EXPECT_EQ(cpu.rip, 0x1008u);
        EXPECT_EQ(cpu.gpr.rcx, 0x56u);
        // Materialize a copy; leave the executing CPU's lazy state intact
        // until after its decode fault, which must preserve that state too.
        CpuState committed = cpu;
        committed.materialize_lazy_flags();
        Interpreter interpreter(cpu, memory);
        expect_length_fault(interpreter, cpu, 0x1008, wide, committed.flags.rflags);
        EXPECT_EQ(memory.read_u32(0x4000).value(), 0x76543210u);
        EXPECT_EQ(cli::decode_basic_block(memory, cpu.rip, wide).instruction_count, 0u);
    }
    cli::reset_decode_memo();
}
