// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64

/// @file ir_emit_test_support.hpp
/// @brief Shared Tier 1 vs interpreter harness for the test_ir_emit*.cpp suites.

#pragma once

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <utility>
#include <vector>

#include "core/cpu/decode/decoder.hpp"
#include "core/cpu/interpreter/interpreter.hpp"
#include "core/cpu/jit/ir/ir_passes.hpp"
#include "core/cpu/jit/stencils.hpp"
#include "core/cpu/jit/jit_compiler.hpp"
#include "core/cpu/jit/jit_helpers.hpp"
#include "core/cpu/jit/linear_fault.hpp"
#include "core/memory/guest_memory.hpp"
#include "ir_tier1_first_compile.hpp"

namespace vitreo::cpu::jit::test::ir_emit {

inline constexpr GuestAddr kCode = 0x1000;

/// Operand seeds chosen to make an operand-order mistake observable: distinct,
/// asymmetric, and spanning the signed/unsigned boundary so CF and OF differ.
struct Seed {
    u32 eax, ecx, edx;
};
inline constexpr std::array<Seed, 6> kSeeds = {{
    {0x00000001u, 0x00000002u, 0x00000003u},
    {0x80000000u, 0x00000001u, 0xFFFFFFFFu},
    {0x7FFFFFFFu, 0x00000001u, 0x00000002u},
    {0xFFFFFFFFu, 0x00000001u, 0x80000000u},
    {0x0000000Fu, 0xFFFFFFF1u, 0x0000FFFFu},
    {0x00000000u, 0x00000000u, 0xFFFFFFFFu},
}};

inline BasicBlock decode_bytes(const std::vector<u8>& code) {
    BasicBlock block;
    block.start_addr = kCode;
    block.is_64bit = false;
    MemoryByteReader reader(code.data(), code.size());
    while (reader.remaining() > 0) {
        DecodedInstruction inst;
        if (decode_instruction(reader, DecodeMode::Mode32, inst) !=
            DecodeError::Success) {
            break;
        }
        block.instructions.push_back(inst);
    }
    block.byte_length = static_cast<u32>(reader.position());
    block.instruction_count = static_cast<u32>(block.instructions.size());
    return block;
}

inline void seed_state(CpuState& cpu, const Seed& s) {
    cpu.init_protected_mode();
    cpu.gpr.set32(0, s.eax);
    cpu.gpr.set32(1, s.ecx);
    cpu.gpr.set32(2, s.edx);
    cpu.gpr.set32(4, 0x20800);
    cpu.rip = kCode;
}

/// Run `body` under the interpreter and under Tier 1, and require that every
/// architectural output agrees.
///
/// `body` carries NO terminator on purpose. A RET or a Jcc would be declined
/// by the frontend, the block would fall back to Tier 0, and this test would
/// silently assert Tier 0 against the interpreter — passing while testing
/// nothing. A terminator-free block runs to its end and returns to the
/// dispatcher, so what executes here really is Tier 1 output.
inline void expect_tier1_matches_interpreter(const std::vector<u8>& body, bool require_native = false) {
    const std::vector<u8>& code = body;

    for (const Seed& seed : kSeeds) {
        GuestMemory memory;
        ASSERT_TRUE(memory.map_region(0x1000, 0x10000,
                                      MemoryProtection::ReadWrite));
        ASSERT_TRUE(memory.map_region(0x20000, 0x1000,
                                      MemoryProtection::ReadWrite));
        ASSERT_TRUE(memory.write_bytes(kCode, code.data(), code.size()));

        // EXACTLY the block's instructions. `body` has no terminator, so a
        // step-until-you-stop loop would run on into the zero bytes after it
        // and execute them as guest code -- silently comparing the JIT's block
        // against the interpreter's block PLUS garbage.
        const u32 insn_count =
            static_cast<u32>(decode_bytes(code).instruction_count);
        CpuState interp_cpu;
        seed_state(interp_cpu, seed);
        {
            Interpreter interp(interp_cpu, memory);
            for (u32 i = 0; i < insn_count; ++i) {
                ASSERT_EQ(interp.step(), ExecResult::Continue);
            }
        }

        CpuState jit_cpu;
        seed_state(jit_cpu, seed);
        JitCompiler jit(memory);
        BasicBlock block = decode_bytes(code);

        ASSERT_TRUE(jit.ir_build_block(block).ok)
            << "frontend must accept this body: "
            << ir::decline_reason_name(jit.ir_last_result().reason);

        CompiledBlock* compiled = jit.compile(block);
        ASSERT_NE(compiled, nullptr);
        if (require_native) ASSERT_TRUE(jit.ir_emit_active());
        jit.execute(*compiled, jit_cpu);

        // Both sides must be compared on authoritative flags: the JIT leaves a
        // lazy record behind and the interpreter may too, so reading `rflags`
        // raw would compare two different representations of the same value.
        jit_cpu.materialize_lazy_flags();
        interp_cpu.materialize_lazy_flags();

        for (u8 r = 0; r < 8; ++r) {
            EXPECT_EQ(jit_cpu.gpr.get32(r), interp_cpu.gpr.get32(r))
                << "GPR " << int(r) << " diverged for seed " << std::hex
                << seed.eax << "/" << seed.ecx << "/" << seed.edx;
        }
        EXPECT_EQ(jit_cpu.flags.rflags, interp_cpu.flags.rflags)
            << "RFLAGS diverged for seed " << std::hex << seed.eax << "/"
            << seed.ecx << "/" << seed.edx;
    }
}

/// Like `expect_tier1_matches_interpreter`, but seeds ECX to point into a
/// mapped scratch page so memory operands are valid, and compares the touched
/// GUEST MEMORY as well as registers and flags. A store lowering that wrote
/// the right value to the wrong address would pass a register-only check.
inline void expect_tier1_matches_interpreter_with_memory(const std::vector<u8>& body, bool require_native = false) {
    constexpr GuestAddr kScratch = 0x20800;
    for (const Seed& seed : kSeeds) {
        GuestMemory memory;
        ASSERT_TRUE(memory.map_region(0x1000, 0x10000,
                                      MemoryProtection::ReadWrite));
        ASSERT_TRUE(memory.map_region(0x20000, 0x1000,
                                      MemoryProtection::ReadWrite));
        ASSERT_TRUE(memory.write_bytes(kCode, body.data(), body.size()));

        const auto seed_mem = [&](CpuState& cpu) {
            cpu.init_protected_mode();
            cpu.gpr.set32(0, seed.eax);
            cpu.gpr.set32(1, kScratch);   // ECX = base pointer
            cpu.gpr.set32(2, seed.edx);
            cpu.gpr.set32(4, kScratch);
            cpu.rip = kCode;
        };
        const auto fill_scratch = [&]() {
            for (u32 off = 0; off < 0x40; off += 4) {
                ASSERT_TRUE(memory.write_u32(kScratch + off, 0xA5A50000u + off));
            }
        };

        fill_scratch();
        const u32 insn_count =
            static_cast<u32>(decode_bytes(body).instruction_count);
        CpuState interp_cpu;
        seed_mem(interp_cpu);
        {
            Interpreter interp(interp_cpu, memory);
            for (u32 i = 0; i < insn_count; ++i) {
                ASSERT_EQ(interp.step(), ExecResult::Continue);
            }
        }
        std::vector<u32> interp_mem;
        for (u32 off = 0; off < 0x40; off += 4) {
            const auto r = memory.read_u32(kScratch + off);
            ASSERT_TRUE(r.is_ok());
            interp_mem.push_back(r.value());
        }

        fill_scratch();
        CpuState jit_cpu;
        seed_mem(jit_cpu);
        JitCompiler jit(memory);
        BasicBlock block = decode_bytes(body);
        ASSERT_TRUE(jit.ir_build_block(block).ok)
            << ir::decline_reason_name(jit.ir_last_result().reason);
        CompiledBlock* compiled = jit.compile(block);
        ASSERT_NE(compiled, nullptr);
        if (require_native) ASSERT_TRUE(jit.ir_emit_active());
        jit.execute(*compiled, jit_cpu);

        jit_cpu.materialize_lazy_flags();
        interp_cpu.materialize_lazy_flags();
        for (u8 r = 0; r < 8; ++r) {
            EXPECT_EQ(jit_cpu.gpr.get32(r), interp_cpu.gpr.get32(r))
                << "GPR " << int(r);
        }
        EXPECT_EQ(jit_cpu.flags.rflags, interp_cpu.flags.rflags);
        for (u32 off = 0; off < 0x40; off += 4) {
            const auto r = memory.read_u32(kScratch + off);
            ASSERT_TRUE(r.is_ok());
            EXPECT_EQ(r.value(), interp_mem[off / 4])
                << "guest memory diverged at +" << off;
        }
    }
}

/// Compare Tier 1 against the interpreter for a stack-touching block: all eight
/// GPRs (ESP among them), flags, the exit RIP, and the stack memory itself.
inline void expect_tier1_matches_interpreter_on_stack(const std::vector<u8>& body,
                                               u32 insns_to_step) {
    constexpr GuestAddr kStackTop = 0x20F00;
    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(0x1000, 0x10000, MemoryProtection::ReadWrite));
    ASSERT_TRUE(memory.map_region(0x20000, 0x1000, MemoryProtection::ReadWrite));
    ASSERT_TRUE(memory.write_bytes(kCode, body.data(), body.size()));

    const auto seed = [&](CpuState& cpu) {
        cpu.init_protected_mode();
        cpu.gpr.set32(0, 0x1111'2222u);
        cpu.gpr.set32(1, 0x3333'4444u);
        cpu.gpr.set32(2, 0x5555'6666u);
        cpu.gpr.set32(4, kStackTop);
        cpu.rip = kCode;
    };
    const auto fill_stack = [&]() {
        for (u32 off = 0; off < 0x40; off += 4) {
            ASSERT_TRUE(memory.write_u32(kStackTop - 0x20 + off,
                                         0xB6B60000u + off));
        }
    };

    fill_stack();
    CpuState interp_cpu;
    seed(interp_cpu);
    {
        Interpreter interp(interp_cpu, memory);
        for (u32 i = 0; i < insns_to_step; ++i) {
            // `Branch` is a normal outcome here: CALL and RET update EIP
            // themselves. Anything else (Exception, Halt) means the oracle
            // did not execute what the test intended.
            const ExecResult r = interp.step();
            ASSERT_TRUE(r == ExecResult::Continue || r == ExecResult::Branch)
                << "interpreter stopped at instruction " << i;
        }
    }
    std::vector<u32> interp_stack;
    for (u32 off = 0; off < 0x40; off += 4) {
        const auto r = memory.read_u32(kStackTop - 0x20 + off);
        ASSERT_TRUE(r.is_ok());
        interp_stack.push_back(r.value());
    }

    fill_stack();
    CpuState jit_cpu;
    seed(jit_cpu);
    JitCompiler jit(memory);
    BasicBlock block = decode_bytes(body);
    ASSERT_TRUE(jit.ir_build_block(block).ok)
        << ir::decline_reason_name(jit.ir_last_result().reason);
    CompiledBlock* compiled = jit.compile(block);
    ASSERT_NE(compiled, nullptr);
    ASSERT_TRUE(jit.ir_emit_active())
        << "block fell back to Tier 0; this test would be asserting Tier 0";
    jit.execute(*compiled, jit_cpu);

    jit_cpu.materialize_lazy_flags();
    interp_cpu.materialize_lazy_flags();
    for (u8 r = 0; r < 8; ++r) {
        EXPECT_EQ(jit_cpu.gpr.get32(r), interp_cpu.gpr.get32(r))
            << "GPR " << int(r) << (r == 4 ? " (ESP)" : "");
    }
    EXPECT_EQ(jit_cpu.rip, interp_cpu.rip) << "exit RIP";
    EXPECT_EQ(jit_cpu.flags.rflags, interp_cpu.flags.rflags);
    for (u32 off = 0; off < 0x40; off += 4) {
        const auto r = memory.read_u32(kStackTop - 0x20 + off);
        ASSERT_TRUE(r.is_ok());
        EXPECT_EQ(r.value(), interp_stack[off / 4])
            << "stack memory diverged at top-0x20+" << off;
    }
}

inline void expect_long_mode_scalar(const std::vector<std::vector<u8>>& bodies, bool require_native = true) {
    constexpr GuestAddr kCode = 0x100001000ull;
    constexpr u64 seeds[] = {0, 1, 0xFFFFFFFFFFFFFFFFull, 0x8000000000000000ull,
        0x7FFFFFFFFFFFFFFFull, 0x80000000ull, 0x7FFFFFFFull, 0xFEDCBA9887654321ull};
    for (usize seed = 0; seed < std::size(seeds); ++seed) {
    for (const auto& body : bodies) {
        GuestMemory memory;
        ASSERT_TRUE(memory.map_region(kCode, 0x1000, MemoryProtection::ReadWrite));
        ASSERT_TRUE(memory.write_bytes(kCode, body.data(), body.size()));
        BasicBlock block;
        block.start_addr = kCode;
        block.is_64bit = true;
        MemoryByteReader reader(body.data(), body.size());
        while (reader.remaining()) {
            DecodedInstruction instruction;
            ASSERT_EQ(decode_instruction(reader, DecodeMode::Mode64, instruction), DecodeError::Success);
            block.instructions.push_back(instruction);
        }
        block.instruction_count = static_cast<u32>(block.instructions.size());
        block.byte_length = static_cast<u32>(body.size());
        CpuState expected;
        expected.init_long_mode();
        for (u8 reg = 0; reg < 16; ++reg)
            expected.gpr.set64(reg, seeds[(reg + seed) % std::size(seeds)]);
        expected.rip = kCode;
        expected.flags.rflags = 0xAD7;
        CpuState actual = expected;
        Interpreter interpreter(expected, memory);
        for (u32 i = 0; i < block.instruction_count; ++i) {
            const auto result = interpreter.step();
            ASSERT_TRUE(result == ExecResult::Continue ||
                        (i + 1 == block.instruction_count && result == ExecResult::Branch));
        }
        JitCompiler jit(memory);
        ASSERT_TRUE(jit.ir_build_block(block).ok) << ir::decline_reason_name(jit.ir_last_result().reason) << " opcode " << unsigned(block.instructions[0].opcode);
        auto* compiled = jit.compile(block);
        ASSERT_NE(compiled, nullptr);
        ASSERT_EQ(jit.ir_emit_active(), require_native);
        if (!require_native)
            EXPECT_EQ(jit.ir_last_result().reason, ir::DeclineReason::BackendRegisterPressure);
        jit.execute(*compiled, actual);
        for (u8 reg = 0; reg < 16; ++reg)
            EXPECT_EQ(actual.gpr.get64(reg), expected.gpr.get64(reg)) << "GPR " << unsigned(reg);
        actual.materialize_lazy_flags();
        expected.materialize_lazy_flags();
        EXPECT_EQ(actual.flags.rflags, expected.flags.rflags);
        EXPECT_EQ(actual.rip, expected.rip);
    }

    }
}

}  // namespace vitreo::cpu::jit::test::ir_emit

/// Skip unless this process was started with the Tier 1 gate on; otherwise
/// make every first compile in the rest of the test Tier 1. These tests
/// compile a block once and assert on the IR's emission, and under tiering
/// (jit_compiler_cold_residency.hpp) a first compile is tier 0.
#define REQUIRE_IR_GATE()                                                     \
    do {                                                                      \
        if (!JitCompiler::ir_enabled_for(false))                              \
            GTEST_SKIP() << "Tier 1 gate off; run vitreo_cpu_tests_ir_gate_on"; \
    } while (0);                                                              \
    const ::vitreo::cpu::jit::test::IrTier1FirstCompile                       \
        require_ir_gate_tier1_scope_
