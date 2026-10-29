// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// DF-only ops must not force materialize of a pending lazy FlagOp.

#include <gtest/gtest.h>

#include <array>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>

#include "core/cpu/decode/decoder.hpp"
#include "core/cpu/interpreter/interpreter.hpp"
#include "core/cpu/interpreter/ops/exec_ops.hpp"
#include "core/cpu/jit/jit_compiler.hpp"
#include "core/memory/guest_memory.hpp"

using namespace vitreo;
using namespace vitreo::cpu;
using namespace vitreo::cpu::jit;

namespace {

void seed_add32_pending(CpuState& cpu) {
    cpu.flag_op = CpuState::FlagOp::Add32;
    cpu.flag_src = 7;
    cpu.flag_dst = 12;
    cpu.flags.set_df(true);
}

class ScopedEnvironmentVariable {
public:
    ScopedEnvironmentVariable(const char* name, const char* value)
        : m_name(name) {
        if (const char* previous = std::getenv(name)) {
            m_previous = previous;
        }
        setenv(name, value, 1);
    }

    ~ScopedEnvironmentVariable() {
        if (m_previous) {
            setenv(m_name.c_str(), m_previous->c_str(), 1);
        } else {
            unsetenv(m_name.c_str());
        }
    }

    ScopedEnvironmentVariable(const ScopedEnvironmentVariable&) = delete;
    ScopedEnvironmentVariable& operator=(
        const ScopedEnvironmentVariable&) = delete;

private:
    std::string m_name;
    std::optional<std::string> m_previous;
};

} // namespace

TEST(DfPreservesLazyFlagsTest, InterpreterCldLeavesAdd32Pending) {
    CpuState cpu;
    seed_add32_pending(cpu);
    GuestMemory mem;
    ASSERT_TRUE(mem.map_region(0x1000, 0x1000, MemoryProtection::ReadWrite));

    DecodedInstruction inst;
    const u8 code[] = {0xFC};  // CLD
    MemoryByteReader reader(code, sizeof(code));
    ASSERT_EQ(decode_instruction(reader, DecodeMode::Mode32, inst),
              DecodeError::Success);

    ExecContext ctx{cpu, mem};
    ASSERT_EQ(ops::exec_flag_ops(ctx, inst), ExecResult::Continue);

    EXPECT_FALSE(cpu.flags.df());
    EXPECT_EQ(cpu.flag_op, CpuState::FlagOp::Add32);
    EXPECT_EQ(cpu.flag_src, 7u);
    EXPECT_EQ(cpu.flag_dst, 12u);
    EXPECT_EQ(cpu.try_lazy_condition(0x4), 0);  // ZF clear (12 != 0)
    EXPECT_EQ(cpu.flag_op, CpuState::FlagOp::Add32);
}

TEST(DfPreservesLazyFlagsTest, InterpreterStdLeavesAdd32Pending) {
    CpuState cpu;
    seed_add32_pending(cpu);
    cpu.flags.set_df(false);
    GuestMemory mem;
    ASSERT_TRUE(mem.map_region(0x1000, 0x1000, MemoryProtection::ReadWrite));

    DecodedInstruction inst;
    const u8 code[] = {0xFD};  // STD
    MemoryByteReader reader(code, sizeof(code));
    ASSERT_EQ(decode_instruction(reader, DecodeMode::Mode32, inst),
              DecodeError::Success);

    ExecContext ctx{cpu, mem};
    ASSERT_EQ(ops::exec_flag_ops(ctx, inst), ExecResult::Continue);

    EXPECT_TRUE(cpu.flags.df());
    EXPECT_EQ(cpu.flag_op, CpuState::FlagOp::Add32);
}

TEST(DfPreservesLazyFlagsTest, JitAddCldJzMatchesInterpreter) {
    // ADD eax, ecx; CLD; JZ +5; MOV eax, 0x11; RET; MOV eax, 0x22; RET
    const u8 code[] = {
        0x01, 0xC8,
        0xFC,
        0x74, 0x05,
        0xB8, 0x11, 0x00, 0x00, 0x00,
        0xC3,
        0xB8, 0x22, 0x00, 0x00, 0x00,
        0xC3,
    };

    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(0x1000, 0x10000, MemoryProtection::ReadWrite));
    ASSERT_TRUE(memory.map_region(0x20000, 0x1000, MemoryProtection::ReadWrite));
    ASSERT_TRUE(memory.write_bytes(0x1000, code, sizeof(code)));

    const std::array<std::array<u32, 3>, 3> cases = {{
        {{1, 2, 3}},             // ZF clear → eax=0x11
        {{0, 0, 0}},             // ZF set → eax=0x22
        {{5, 0xFFFFFFFBu, 0}},   // 5+(-5)=0 → ZF set
    }};

    for (const auto& t : cases) {
        CpuState ii;
        ii.init_protected_mode();
        ii.gpr.set32(0, t[0]);
        ii.gpr.set32(1, t[1]);
        ii.gpr.set32(2, t[2]);
        ii.gpr.set32(4, 0x20800);
        ii.flags.set_df(true);
        ii.rip = 0x1000;
        ASSERT_TRUE(memory.write_u32(0x20800, 0));
        {
            Interpreter interp(ii, memory);
            for (int i = 0; i < 32 && ii.rip != 0; ++i) {
                if (interp.step() != ExecResult::Continue) break;
            }
        }

        CpuState ji;
        ji.init_protected_mode();
        ji.gpr.set32(0, t[0]);
        ji.gpr.set32(1, t[1]);
        ji.gpr.set32(2, t[2]);
        ji.gpr.set32(4, 0x20800);
        ji.flags.set_df(true);
        ji.rip = 0x1000;
        ASSERT_TRUE(memory.write_u32(0x20800, 0));
        {
            JitCompiler jit(memory);
            for (int guard = 0; guard < 16 && ji.rip != 0; ++guard) {
                u8 local[32];
                for (int k = 0; k < 32; ++k) {
                    auto r = memory.read_u8(
                        static_cast<GuestAddr>(ji.rip + k));
                    local[k] = r.is_ok() ? r.value() : 0xCC;
                }
                BasicBlock block;
                block.start_addr = static_cast<GuestAddr>(ji.rip);
                block.is_64bit = false;
                MemoryByteReader reader(local, sizeof(local));
                while (reader.remaining() > 0) {
                    DecodedInstruction inst;
                    if (decode_instruction(reader, DecodeMode::Mode32, inst)
                        != DecodeError::Success) {
                        break;
                    }
                    block.instructions.push_back(inst);
                    const char* nm = opcode_name(inst.opcode);
                    if (nm && (std::strncmp(nm, "J", 1) == 0 ||
                               std::strncmp(nm, "RET", 3) == 0)) {
                        break;
                    }
                }
                block.byte_length = 0;
                for (const auto& in : block.instructions)
                    block.byte_length += in.length;
                block.instruction_count =
                    static_cast<u32>(block.instructions.size());
                if (block.instruction_count == 0) break;
                CompiledBlock* cb = jit.compile(block);
                if (!cb) {
                    Interpreter interp(ji, memory);
                    if (interp.step() != ExecResult::Continue) break;
                    continue;
                }
                jit.execute(*cb, ji);
            }
        }

        EXPECT_EQ(ji.gpr.get32(0), ii.gpr.get32(0))
            << "eax mismatch for inputs " << t[0] << "," << t[1];
        EXPECT_EQ(ji.flags.df(), ii.flags.df());
        EXPECT_FALSE(ji.flags.df());
    }
}

TEST(DfPreservesLazyFlagsTest, TryLazyConditionReadsRflagsWhenNone) {
    CpuState cpu;
    cpu.flag_op = CpuState::FlagOp::None;
    cpu.flags.rflags = 0x002;
    cpu.flags.set_zf(true);
    EXPECT_EQ(cpu.try_lazy_condition(0x4), 1);
    EXPECT_EQ(cpu.try_lazy_condition(0x5), 0);
    cpu.flags.set_cf(true);
    EXPECT_EQ(cpu.try_lazy_condition(0x2), 1);
    EXPECT_EQ(cpu.try_lazy_condition(0x6), 1);
    EXPECT_EQ(cpu.flag_op, CpuState::FlagOp::None);
}

TEST(DfPreservesLazyFlagsTest, InterpreterClcLeavesAdd32Pending) {
    CpuState cpu;
    seed_add32_pending(cpu);
    GuestMemory mem;
    ASSERT_TRUE(mem.map_region(0x1000, 0x1000, MemoryProtection::ReadWrite));

    DecodedInstruction inst;
    const u8 code[] = {0xF8};  // CLC
    MemoryByteReader reader(code, sizeof(code));
    ASSERT_EQ(decode_instruction(reader, DecodeMode::Mode32, inst),
              DecodeError::Success);

    ExecContext ctx{cpu, mem};
    ASSERT_EQ(ops::exec_flag_ops(ctx, inst), ExecResult::Continue);

    EXPECT_EQ(cpu.flag_op, CpuState::FlagOp::Add32);
    EXPECT_EQ(cpu.cf_overlay, 1);
    EXPECT_FALSE(cpu.flags.cf());
    EXPECT_EQ(cpu.try_lazy_condition(0x4), 0);  // ZF from Add32 (12 != 0)
    EXPECT_EQ(cpu.try_lazy_condition(0x2), 0);  // CF from overlay
    EXPECT_EQ(cpu.flag_op, CpuState::FlagOp::Add32);

    cpu.materialize_lazy_flags();
    EXPECT_EQ(cpu.flag_op, CpuState::FlagOp::None);
    EXPECT_EQ(cpu.cf_overlay, 0);
    EXPECT_FALSE(cpu.flags.cf());
    EXPECT_FALSE(cpu.flags.zf());
}

TEST(DfPreservesLazyFlagsTest, InterpreterStcThenCmcHonorOverlay) {
    CpuState cpu;
    seed_add32_pending(cpu);
    GuestMemory mem;
    ASSERT_TRUE(mem.map_region(0x1000, 0x1000, MemoryProtection::ReadWrite));

    DecodedInstruction stc;
    const u8 stc_code[] = {0xF9};
    MemoryByteReader stc_reader(stc_code, sizeof(stc_code));
    ASSERT_EQ(decode_instruction(stc_reader, DecodeMode::Mode32, stc),
              DecodeError::Success);
    ExecContext ctx{cpu, mem};
    ASSERT_EQ(ops::exec_flag_ops(ctx, stc), ExecResult::Continue);
    EXPECT_EQ(cpu.flag_op, CpuState::FlagOp::Add32);
    EXPECT_EQ(cpu.cf_overlay, 2);
    EXPECT_EQ(cpu.try_lazy_condition(0x2), 1);  // JB
    EXPECT_EQ(cpu.try_lazy_condition(0x6), 1);  // JBE (CF|ZF)

    DecodedInstruction cmc;
    const u8 cmc_code[] = {0xF5};
    MemoryByteReader cmc_reader(cmc_code, sizeof(cmc_code));
    ASSERT_EQ(decode_instruction(cmc_reader, DecodeMode::Mode32, cmc),
              DecodeError::Success);
    ASSERT_EQ(ops::exec_flag_ops(ctx, cmc), ExecResult::Continue);
    EXPECT_EQ(cpu.flag_op, CpuState::FlagOp::Add32);
    EXPECT_EQ(cpu.cf_overlay, 1);
    EXPECT_EQ(cpu.try_lazy_condition(0x2), 0);
    EXPECT_EQ(cpu.try_lazy_condition(0x4), 0);
}

TEST(DfPreservesLazyFlagsTest, JitAddClcJbMatchesInterpreter) {
    // ADD eax, ecx; CLC; JB +5; MOV eax, 0x11; RET; MOV eax, 0x22; RET
    // CLC forces CF=0, so JB must never take. ZF still comes from ADD.
    const u8 code[] = {
        0x01, 0xC8,
        0xF8,
        0x72, 0x05,
        0xB8, 0x11, 0x00, 0x00, 0x00,
        0xC3,
        0xB8, 0x22, 0x00, 0x00, 0x00,
        0xC3,
    };

    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(0x1000, 0x10000, MemoryProtection::ReadWrite));
    ASSERT_TRUE(memory.map_region(0x20000, 0x1000, MemoryProtection::ReadWrite));
    ASSERT_TRUE(memory.write_bytes(0x1000, code, sizeof(code)));

    const std::array<std::array<u32, 2>, 3> cases = {{
        {{1, 2}},              // 3, CF from ADD clear
        {{0xFFFFFFFFu, 1}},    // 0, CF from ADD set — CLC must hide it
        {{0, 0}},              // 0, CF clear
    }};

    for (const auto& t : cases) {
        CpuState ii;
        ii.init_protected_mode();
        ii.gpr.set32(0, t[0]);
        ii.gpr.set32(1, t[1]);
        ii.gpr.set32(4, 0x20800);
        ii.rip = 0x1000;
        ASSERT_TRUE(memory.write_u32(0x20800, 0));
        {
            Interpreter interp(ii, memory);
            for (int i = 0; i < 32 && ii.rip != 0; ++i) {
                if (interp.step() != ExecResult::Continue) break;
            }
        }

        CpuState ji;
        ji.init_protected_mode();
        ji.gpr.set32(0, t[0]);
        ji.gpr.set32(1, t[1]);
        ji.gpr.set32(4, 0x20800);
        ji.rip = 0x1000;
        ASSERT_TRUE(memory.write_u32(0x20800, 0));
        {
            JitCompiler jit(memory);
            for (int guard = 0; guard < 16 && ji.rip != 0; ++guard) {
                u8 local[32];
                for (int k = 0; k < 32; ++k) {
                    auto r = memory.read_u8(
                        static_cast<GuestAddr>(ji.rip + k));
                    local[k] = r.is_ok() ? r.value() : 0xCC;
                }
                BasicBlock block;
                block.start_addr = static_cast<GuestAddr>(ji.rip);
                block.is_64bit = false;
                MemoryByteReader reader(local, sizeof(local));
                while (reader.remaining() > 0) {
                    DecodedInstruction inst;
                    if (decode_instruction(reader, DecodeMode::Mode32, inst)
                        != DecodeError::Success) {
                        break;
                    }
                    block.instructions.push_back(inst);
                    const char* nm = opcode_name(inst.opcode);
                    if (nm && (std::strncmp(nm, "J", 1) == 0 ||
                               std::strncmp(nm, "RET", 3) == 0)) {
                        break;
                    }
                }
                block.byte_length = 0;
                for (const auto& in : block.instructions)
                    block.byte_length += in.length;
                block.instruction_count =
                    static_cast<u32>(block.instructions.size());
                if (block.instruction_count == 0) break;
                CompiledBlock* cb = jit.compile(block);
                if (!cb) {
                    Interpreter interp(ji, memory);
                    if (interp.step() != ExecResult::Continue) break;
                    continue;
                }
                jit.execute(*cb, ji);
            }
        }

        EXPECT_EQ(ji.gpr.get32(0), ii.gpr.get32(0))
            << "eax mismatch for inputs " << t[0] << "," << t[1];
        EXPECT_EQ(ji.gpr.get32(0), 0x11u)
            << "JB must not take after CLC";
        EXPECT_FALSE(ii.flags.cf());
        EXPECT_FALSE(ji.flags.cf());
    }
}

namespace {

[[nodiscard]] bool run_64bit_jit_vs_interpreter(const u8* code, usize code_len,
                                                u32 eax0, u32 ecx0,
                                                CpuState& ii, CpuState& ji) {
    GuestMemory memory;
    if (!memory.map_region(0x1000, 0x10000, MemoryProtection::ReadWrite))
        return false;
    if (!memory.map_region(0x20000, 0x1000, MemoryProtection::ReadWrite))
        return false;
    if (!memory.write_bytes(0x1000, code, code_len)) return false;

    ii.init_long_mode();
    ii.gpr.set32(0, eax0);
    ii.gpr.set32(1, ecx0);
    ii.gpr.set64(4, 0x20800);
    ii.rip = 0x1000;
    if (!memory.write_u64(0x20800, 0)) return false;
    {
        Interpreter interp(ii, memory);
        for (int i = 0; i < 32 && ii.rip != 0; ++i) {
            // Jcc/RET return Branch; keep stepping so a taken JB can
            // execute the target (CLC never takes, CMC/STC can).
            ExecResult r = interp.step();
            if (r != ExecResult::Continue && r != ExecResult::Branch) break;
        }
    }

    ji.init_long_mode();
    ji.gpr.set32(0, eax0);
    ji.gpr.set32(1, ecx0);
    ji.gpr.set64(4, 0x20800);
    ji.rip = 0x1000;
    if (!memory.write_u64(0x20800, 0)) return false;
    {
        ScopedEnvironmentVariable no_delegate("VITREO_JIT_NO_DELEGATE", "1");
        JitCompiler jit(memory);
        for (int guard = 0; guard < 16 && ji.rip != 0; ++guard) {
            u8 local[32];
            for (int k = 0; k < 32; ++k) {
                auto r = memory.read_u8(
                    static_cast<GuestAddr>(ji.rip + k));
                local[k] = r.is_ok() ? r.value() : 0xCC;
            }
            BasicBlock block;
            block.start_addr = static_cast<GuestAddr>(ji.rip);
            block.is_64bit = true;
            MemoryByteReader reader(local, sizeof(local));
            while (reader.remaining() > 0) {
                DecodedInstruction inst;
                if (decode_instruction(reader, DecodeMode::Mode64, inst)
                    != DecodeError::Success) {
                    break;
                }
                block.instructions.push_back(inst);
                const char* nm = opcode_name(inst.opcode);
                if (nm && (std::strncmp(nm, "J", 1) == 0 ||
                           std::strncmp(nm, "RET", 3) == 0)) {
                    break;
                }
            }
            block.byte_length = 0;
            for (const auto& in : block.instructions)
                block.byte_length += in.length;
            block.instruction_count =
                static_cast<u32>(block.instructions.size());
            if (block.instruction_count == 0) break;
            CompiledBlock* cb = jit.compile(block);
            if (!cb) return false;
            jit.execute(*cb, ji);
        }
    }
    return true;
}

} // namespace

TEST(DfPreservesLazyFlagsTest, Jit64AddClcJbMatchesInterpreter) {
    // ADD eax, ecx; CLC; JB +5; MOV eax, 0x11; RET; MOV eax, 0x22; RET
    // CLC forces CF=0, so JB must never take. ZF still comes from ADD.
    const u8 code[] = {
        0x01, 0xC8,
        0xF8,
        0x72, 0x05,
        0xB8, 0x11, 0x00, 0x00, 0x00,
        0xC3,
        0xB8, 0x22, 0x00, 0x00, 0x00,
        0xC3,
    };

    const std::array<std::array<u32, 2>, 3> cases = {{
        {{1, 2}},              // 3, CF from ADD clear
        {{0xFFFFFFFFu, 1}},    // 0, CF from ADD set — CLC must hide it
        {{0, 0}},              // 0, CF clear
    }};

    for (const auto& t : cases) {
        CpuState ii, ji;
        ASSERT_TRUE(run_64bit_jit_vs_interpreter(
            code, sizeof(code), t[0], t[1], ii, ji))
            << "64-bit native compile failed for inputs " << t[0] << ","
            << t[1];
        EXPECT_EQ(ji.gpr.get32(0), ii.gpr.get32(0))
            << "eax mismatch for inputs " << t[0] << "," << t[1];
        EXPECT_EQ(ji.gpr.get32(0), 0x11u)
            << "JB must not take after CLC";
        EXPECT_FALSE(ii.flags.cf());
        EXPECT_FALSE(ji.flags.cf());
    }
}

TEST(DfPreservesLazyFlagsTest, Jit64AddStcJaeMatchesInterpreter) {
    // ADD eax, ecx; STC; JAE +5; MOV eax, 0x11; RET; MOV eax, 0x22; RET
    // STC forces CF=1, so JAE (CF=0) must never take.
    const u8 code[] = {
        0x01, 0xC8,
        0xF9,
        0x73, 0x05,
        0xB8, 0x11, 0x00, 0x00, 0x00,
        0xC3,
        0xB8, 0x22, 0x00, 0x00, 0x00,
        0xC3,
    };

    const std::array<std::array<u32, 2>, 3> cases = {{
        {{1, 2}},
        {{0xFFFFFFFFu, 1}},
        {{0, 0}},
    }};

    for (const auto& t : cases) {
        CpuState ii, ji;
        ASSERT_TRUE(run_64bit_jit_vs_interpreter(
            code, sizeof(code), t[0], t[1], ii, ji))
            << "64-bit native compile failed for inputs " << t[0] << ","
            << t[1];
        EXPECT_EQ(ji.gpr.get32(0), ii.gpr.get32(0))
            << "eax mismatch for inputs " << t[0] << "," << t[1];
        EXPECT_EQ(ji.gpr.get32(0), 0x11u)
            << "JAE must not take after STC";
        EXPECT_TRUE(ii.flags.cf());
        EXPECT_TRUE(ji.flags.cf());
    }
}

TEST(DfPreservesLazyFlagsTest, Jit64AddCmcJbMatchesInterpreter) {
    // ADD eax, ecx; CMC; JB +6; MOV eax, 0x11; RET; MOV eax, 0x22; RET
    // CMC complements ADD's CF, so JB tracks the inverted carry.
    const u8 code[] = {
        0x01, 0xC8,
        0xF5,
        0x72, 0x06,
        0xB8, 0x11, 0x00, 0x00, 0x00,
        0xC3,
        0xB8, 0x22, 0x00, 0x00, 0x00,
        0xC3,
    };

    const struct {
        u32 eax, ecx, expected_eax;
        bool expected_cf;
    } cases[] = {
        {1, 2, 0x22u, true},             // ADD CF=0 → CMC CF=1 → JB taken
        {0xFFFFFFFFu, 1, 0x11u, false},  // ADD CF=1 → CMC CF=0 → JB not taken
        {0, 0, 0x22u, true},             // ADD CF=0 → CMC CF=1 → JB taken
    };

    for (const auto& t : cases) {
        CpuState ii, ji;
        ASSERT_TRUE(run_64bit_jit_vs_interpreter(
            code, sizeof(code), t.eax, t.ecx, ii, ji))
            << "64-bit native compile failed for inputs " << t.eax << ","
            << t.ecx;
        EXPECT_EQ(ji.gpr.get32(0), ii.gpr.get32(0))
            << "eax mismatch for inputs " << t.eax << "," << t.ecx;
        EXPECT_EQ(ji.gpr.get32(0), t.expected_eax)
            << "JB after CMC for inputs " << t.eax << "," << t.ecx;
        EXPECT_EQ(ii.flags.cf(), t.expected_cf);
        EXPECT_EQ(ji.flags.cf(), t.expected_cf);
    }
}

// The overlay-ordering bug was not specific to ADD. Any full-flag writer that
// leaves a lazy FlagOp live, followed by a CF-only writer, followed by a
// CF-reading Jcc, hits it — the Jcc walk matched the FlagOp and recomputed CF
// from the ALU op instead of honoring the overlay. These two lock in the
// sibling shapes that had no coverage when the ADD cases were written.

TEST(DfPreservesLazyFlagsTest, Jit64SubClcJbMatchesInterpreter) {
    // SUB eax, ecx; CLC; JB +5; MOV eax, 0x11; RET; MOV eax, 0x22; RET
    // SUB's borrow is a real CF=1; CLC must hide it from JB.
    const u8 code[] = {
        0x29, 0xC8,
        0xF8,
        0x72, 0x05,
        0xB8, 0x11, 0x00, 0x00, 0x00,
        0xC3,
        0xB8, 0x22, 0x00, 0x00, 0x00,
        0xC3,
    };

    const struct { u32 eax, ecx; } cases[] = {
        {0, 1},   // SUB borrows -> CF=1, then CLC clears it
        {5, 3},   // no borrow
        {0, 0},   // zero result
    };

    for (const auto& t : cases) {
        CpuState ii, ji;
        ASSERT_TRUE(run_64bit_jit_vs_interpreter(
            code, sizeof(code), t.eax, t.ecx, ii, ji))
            << "64-bit native compile failed for inputs " << t.eax << ","
            << t.ecx;
        EXPECT_EQ(ji.gpr.get32(0), ii.gpr.get32(0))
            << "eax mismatch for inputs " << t.eax << "," << t.ecx;
        EXPECT_EQ(ji.gpr.get32(0), 0x11u)
            << "JB must not take after CLC, inputs " << t.eax << "," << t.ecx;
        EXPECT_FALSE(ii.flags.cf());
        EXPECT_FALSE(ji.flags.cf());
    }
}

TEST(DfPreservesLazyFlagsTest, Jit64AndStcJbMatchesInterpreter) {
    // AND eax, ecx; STC; JB +6; MOV eax, 0x11; RET; MOV eax, 0x22; RET
    // AND always clears CF, so only the overlay can make JB take. If the Jcc
    // walk matched And32 first it would compute CF=0 and never branch.
    // rel8 is 6, not 5: the taken target must clear the intervening RET and
    // land on the second MOV (a displacement of 5 lands on the RET itself and
    // silently leaves EAX holding the AND result).
    const u8 code[] = {
        0x21, 0xC8,
        0xF9,
        0x72, 0x06,
        0xB8, 0x11, 0x00, 0x00, 0x00,
        0xC3,
        0xB8, 0x22, 0x00, 0x00, 0x00,
        0xC3,
    };

    const struct { u32 eax, ecx; } cases[] = {
        {0xFFFFFFFFu, 0xFFFFFFFFu},
        {0xF0F0F0F0u, 0x0F0F0F0Fu},  // zero result
        {0, 0},
    };

    for (const auto& t : cases) {
        CpuState ii, ji;
        ASSERT_TRUE(run_64bit_jit_vs_interpreter(
            code, sizeof(code), t.eax, t.ecx, ii, ji))
            << "64-bit native compile failed for inputs " << t.eax << ","
            << t.ecx;
        EXPECT_EQ(ji.gpr.get32(0), ii.gpr.get32(0))
            << "eax mismatch for inputs " << t.eax << "," << t.ecx;
        EXPECT_EQ(ji.gpr.get32(0), 0x22u)
            << "JB must take after STC, inputs " << t.eax << "," << t.ecx;
        EXPECT_TRUE(ii.flags.cf());
        EXPECT_TRUE(ji.flags.cf());
    }
}

TEST(DfPreservesLazyFlagsTest, InterpreterRor1LeavesAdd32Pending) {
    CpuState cpu;
    seed_add32_pending(cpu);
    cpu.gpr.set32(0, 1);
    GuestMemory mem;
    ASSERT_TRUE(mem.map_region(0x1000, 0x1000, MemoryProtection::ReadWrite));

    DecodedInstruction inst;
    const u8 code[] = {0xD1, 0xC8};  // ROR EAX, 1
    MemoryByteReader reader(code, sizeof(code));
    ASSERT_EQ(decode_instruction(reader, DecodeMode::Mode32, inst),
              DecodeError::Success);

    ExecContext ctx{cpu, mem};
    ASSERT_EQ(ops::exec_ror(ctx, inst), ExecResult::Continue);

    EXPECT_EQ(cpu.gpr.get32(0), 0x80000000u);
    EXPECT_EQ(cpu.flag_op, CpuState::FlagOp::Add32);
    EXPECT_EQ(cpu.cf_overlay, 2);
    EXPECT_EQ(cpu.of_overlay, 2);
    EXPECT_EQ(cpu.try_lazy_condition(0x4), 0);  // ZF from Add32
    EXPECT_EQ(cpu.try_lazy_condition(0x2), 1);  // CF from overlay
    EXPECT_EQ(cpu.try_lazy_condition(0x0), 1);  // OF from overlay
    EXPECT_EQ(cpu.flag_op, CpuState::FlagOp::Add32);

    cpu.materialize_lazy_flags();
    EXPECT_EQ(cpu.flag_op, CpuState::FlagOp::None);
    EXPECT_EQ(cpu.cf_overlay, 0);
    EXPECT_EQ(cpu.of_overlay, 0);
    EXPECT_TRUE(cpu.flags.cf());
    EXPECT_TRUE(cpu.flags.of());
    EXPECT_FALSE(cpu.flags.zf());
}

TEST(DfPreservesLazyFlagsTest, JitAddRorLeavesAdd32Pending) {
    // ADD eax, ecx; ROR eax, 1 — ROR must not retire the ADD FlagOp.
    const u8 code[] = {0x01, 0xC8, 0xD1, 0xC8};
    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(0x1000, 0x10000, MemoryProtection::ReadWrite));
    ASSERT_TRUE(memory.write_bytes(0x1000, code, sizeof(code)));

    BasicBlock block;
    block.start_addr = 0x1000;
    block.is_64bit = false;
    MemoryByteReader reader(code, sizeof(code));
    while (reader.remaining() > 0) {
        DecodedInstruction inst;
        ASSERT_EQ(decode_instruction(reader, DecodeMode::Mode32, inst),
                  DecodeError::Success);
        block.instructions.push_back(inst);
        block.byte_length += inst.length;
    }
    block.instruction_count = static_cast<u32>(block.instructions.size());

    CpuState cpu;
    cpu.init_protected_mode();
    cpu.gpr.set32(0, 1);
    cpu.gpr.set32(1, 2);
    cpu.rip = 0x1000;
    JitCompiler jit(memory);
    CompiledBlock* cb = jit.compile(block);
    ASSERT_NE(cb, nullptr);
    jit.execute(*cb, cpu);

    EXPECT_EQ(cpu.gpr.get32(0), 0x80000001u);  // ROR of 3
    EXPECT_EQ(cpu.flag_op, CpuState::FlagOp::Add32);
    EXPECT_EQ(cpu.try_lazy_condition(0x4), 0);  // ZF from ADD 3
    EXPECT_EQ(cpu.try_lazy_condition(0x2), 1);  // CF from ROR
    EXPECT_EQ(cpu.flag_op, CpuState::FlagOp::Add32);
}

TEST(DfPreservesLazyFlagsTest, JitLeavesAdd32PendingForLaterTryLazy) {
    // Run-loop no longer settles after every JIT chain. A later Jcc must
    // be able to answer ZF from the previous block's pending FlagOp.
    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(0x1000, 0x10000, MemoryProtection::ReadWrite));

    const u8 add_code[] = {0x01, 0xC8};  // ADD eax, ecx
    ASSERT_TRUE(memory.write_bytes(0x1000, add_code, sizeof(add_code)));

    BasicBlock block;
    block.start_addr = 0x1000;
    block.is_64bit = false;
    MemoryByteReader reader(add_code, sizeof(add_code));
    DecodedInstruction inst;
    ASSERT_EQ(decode_instruction(reader, DecodeMode::Mode32, inst),
              DecodeError::Success);
    block.instructions.push_back(inst);
    block.byte_length = inst.length;
    block.instruction_count = 1;

    const struct {
        u32 eax, ecx;
        int zf;
    } cases[] = {
        {1, 2, 0},
        {0, 0, 1},
        {5, 0xFFFFFFFBu, 1},
    };

    for (const auto& t : cases) {
        CpuState cpu;
        cpu.init_protected_mode();
        cpu.gpr.set32(0, t.eax);
        cpu.gpr.set32(1, t.ecx);
        cpu.rip = 0x1000;
        JitCompiler jit(memory);
        CompiledBlock* cb = jit.compile(block);
        ASSERT_NE(cb, nullptr);
        jit.execute(*cb, cpu);
        EXPECT_EQ(cpu.flag_op, CpuState::FlagOp::Add32);
        EXPECT_EQ(cpu.try_lazy_condition(0x4), t.zf)
            << "eax=" << t.eax << " ecx=" << t.ecx;
        EXPECT_EQ(cpu.flag_op, CpuState::FlagOp::Add32);
    }
}
