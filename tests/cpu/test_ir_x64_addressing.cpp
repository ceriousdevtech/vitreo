// SPDX-License-Identifier: LGPL-2.1-only
// Intel SDM Vol. 1 §§3.3.7,3.7.5: address-size truncation precedes FS/GS addition.
#include <gtest/gtest.h>
#include <array>
#include <vector>
#include "core/cpu/decode/decoder.hpp"
#include "core/cpu/jit/ir/ir_frontend.hpp"
#include "core/cpu/interpreter/interpreter.hpp"
#include "core/cpu/interpreter/ops/helpers.hpp"
#include "core/memory/guest_memory.hpp"

using namespace vitreo;
using namespace vitreo::cpu;
namespace ir = vitreo::cpu::jit::ir;

TEST(IrAddress64Frontend, IndexedAddressOverrideAndSegmentHaveExplicitWidth) {
    const std::vector<u8> bytes{0x64,0x67,0x48,0x8B,0x44,0x8B,0xF0}; // fs:[ebx+ecx*4-16]
    MemoryByteReader reader(bytes.data(), bytes.size());
    DecodedInstruction instruction;
    ASSERT_EQ(decode_instruction(reader, DecodeMode::Mode64, instruction), DecodeError::Success);
    const u64 rip = 0x100010000ull;
    ir::IrBlock block;
    std::string why;
    const auto result = ir::build_block(&instruction, &rip, 1, rip, true, false, block, &why);
    ASSERT_TRUE(result.ok) << why << ir::decline_reason_name(result.reason);
    bool lea = false, extend = false, segment = false;
    for (const auto& op : block.insts) {
        if (op.op == ir::Op::Lea) {
            lea = true;
            EXPECT_EQ(op.type, ir::Type::I32);
            EXPECT_EQ(op.operands[2].payload, 4u);
            EXPECT_EQ(static_cast<i64>(op.operands[3].payload), -16);
        }
        if (op.op == ir::Op::Zext) { extend = true; EXPECT_EQ(op.type, ir::Type::I64); }
        if (op.op == ir::Op::SegBase) { segment = true; EXPECT_EQ(op.type, ir::Type::I64); }
    }
    EXPECT_TRUE(lea); EXPECT_TRUE(extend); EXPECT_TRUE(segment);
}

TEST(IrAddress64Interpreter, PublicAndPredecodedEntryResetAddressOverride) {
    GuestMemory memory;
    constexpr u64 high = 0x200010000ull;
    constexpr u64 low = 0x10000;
    ASSERT_TRUE(memory.map_region(low, 0x1000, MemoryProtection::ReadWrite));
    ASSERT_TRUE(memory.map_region(high, 0x1000, MemoryProtection::ReadWrite));
    ASSERT_TRUE(memory.write_u64(low, 11));
    ASSERT_TRUE(memory.write_u64(high, 22));
    const std::vector<u8> narrow{0x67,0x48,0x8B,0x03};
    const std::vector<u8> wide{0x48,0x8B,0x03};
    DecodedInstruction n,w;
    MemoryByteReader nr(narrow.data(), narrow.size()), wr(wide.data(), wide.size());
    ASSERT_EQ(decode_instruction(nr, DecodeMode::Mode64,n), DecodeError::Success);
    ASSERT_EQ(decode_instruction(wr, DecodeMode::Mode64,w), DecodeError::Success);
    for (bool predecoded : {false,true}) {
        CpuState cpu; cpu.init_long_mode(); cpu.gpr.rbx=high;
        ExecContext ctx(cpu,memory);
        Interpreter interpreter(cpu,memory);
        auto run = [&](const DecodedInstruction& i) {
            return predecoded ? interpreter.execute_predecoded(i) : execute_instruction(ctx,i);
        };
        ASSERT_EQ(run(n), ExecResult::Continue); EXPECT_EQ(cpu.gpr.rax,11u);
        ASSERT_EQ(run(w), ExecResult::Continue); EXPECT_EQ(cpu.gpr.rax,22u);
    }
}

TEST(IrAddress64Interpreter, CompatibilityModeSegmentAndWrappingStayUnchanged) {
    GuestMemory memory;
    CpuState cpu; cpu.init_protected_mode();
    cpu.gpr.rbx=0xFFFFFFFFFFFFFFF0ull;
    cpu.seg.fs_desc.base=0x30;
    ExecContext ctx(cpu,memory);
    MemoryOperand operand; operand.base=3; operand.disp=8; operand.segment=SegReg::FS;
    const auto address=ops::calc_effective_address(ctx,operand);
    ASSERT_TRUE(address); EXPECT_EQ(address.value(),0x28u);
}

#if defined(__aarch64__)
#include "core/cpu/jit/jit_compiler.hpp"
#include "core/cpu/interpreter/interpreter.hpp"
#include "core/memory/guest_memory.hpp"
#include "core/cpu/jit/linear_fault.hpp"
#include "ir_tier1_first_compile.hpp"
#include <cstdlib>
#include <memory>
using namespace vitreo::cpu::jit;
namespace {
constexpr u64 kAddressCode = 0x100010000ull;
constexpr u64 kAddressData = 0x200010000ull;
class IrEmit64Address : public ::testing::Test {
protected:
    vitreo::cpu::jit::test::IrTier1FirstCompile m_tier1_first_compile;
    GuestMemory memory;
    std::unique_ptr<JitCompiler> jit;
    void SetUp() override {
        if (!JitCompiler::ir_enabled_for(true)) GTEST_SKIP() << "x64 IR policy disabled";
        if (std::getenv("VITREO_TEST_IR_LINEAR64")) {
            constexpr usize size = usize{16} << 30;
            ASSERT_TRUE(install_linear_fault_handler());
            ASSERT_TRUE(memory.enable_linear_reservation(size));
            ASSERT_TRUE(memory.enable_linear_hardware_protection());
            ASSERT_TRUE(register_linear_reservation(memory.linear_base(), size));
        }
        ASSERT_TRUE(memory.map_region(kAddressCode, 0x1000, MemoryProtection::ReadWriteExecute));
        ASSERT_TRUE(memory.map_region(kAddressData, 0x1000, MemoryProtection::ReadWrite));
        jit = std::make_unique<JitCompiler>(memory);
    }
    void TearDown() override {
        jit.reset();
        if (memory.is_linear()) unregister_linear_reservation(memory.linear_base());
    }
    void check_load(const std::vector<u8>& bytes, CpuState state, u64 address, bool wide) {
        constexpr u64 value = 0xFEDCBA9876543210ull;
        ASSERT_TRUE(memory.write_bytes(kAddressCode, bytes.data(), bytes.size()));
        ASSERT_TRUE(memory.write_u64(address, value));
        state.rip = kAddressCode;
        CpuState oracle = state;
        Interpreter interpreter(oracle, memory);
        ASSERT_EQ(interpreter.step(), ExecResult::Continue);
        BasicBlock block;
        block.start_addr = kAddressCode; block.is_64bit = true;
        MemoryByteReader reader(bytes.data(), bytes.size());
        DecodedInstruction decoded;
        ASSERT_EQ(decode_instruction(reader, DecodeMode::Mode64, decoded), DecodeError::Success);
        block.instructions.push_back(decoded); block.instruction_count = 1;
        block.byte_length = static_cast<u32>(bytes.size());
        auto* native = jit->compile(block);
        ASSERT_NE(native, nullptr);
        ASSERT_TRUE(jit->ir_emit_active()) << "addressing test requires native IR";
        ASSERT_EQ(jit->execute(*native, state), JitExecResult::Continue);
        ASSERT_FALSE(state.pending_raise.active);
        EXPECT_EQ(state.gpr.rax, wide ? value : static_cast<u32>(value));
        EXPECT_EQ(state.gpr.rax, oracle.gpr.rax);
        EXPECT_EQ(state.rip, oracle.rip);
        state.materialize_lazy_flags(); oracle.materialize_lazy_flags();
        EXPECT_EQ(state.flags.rflags, oracle.flags.rflags);
    }
    CpuState seed() { CpuState s; s.init_long_mode(); s.flags.rflags=0xAD7; return s; }
};
}

TEST_F(IrEmit64Address, IndexedScalesNegativeDisplacementHighRegisters) {
    for (u8 scale=0; scale<4; ++scale) for (bool wide : {false,true}) {
        auto state=seed();
        state.gpr.r8=kAddressData+16; state.gpr.r9=3;
        // mov eax/rax,[r8+r9*(1<<scale)-16]
        check_load({static_cast<u8>(wide?0x4B:0x43),0x8B,0x44,
                    static_cast<u8>((scale<<6)|0x08),0xF0},
                   state,kAddressData+(3u<<scale),wide);
    }
}

TEST_F(IrEmit64Address, Address32WrapsBeforeFullWidthFsGsBase) {
    for (bool gs : {false,true}) {
        auto state=seed();
        state.gpr.rbx=0x12345678FFFFFFF0ull; state.gpr.rcx=8;
        state.seg.fs_desc.base=kAddressData; state.seg.gs_desc.base=kAddressData;
        // effective offset=(0xfffffff0 + 8*4 + 0x10) modulo2^32 =0x20
        check_load({static_cast<u8>(gs?0x65:0x64),0x67,0x48,0x8B,0x44,0x8B,0x10},
                   state,kAddressData+0x20,true);
    }
}

TEST_F(IrEmit64Address, IgnoredLongModeSegmentsDoNotAddDescriptorBase) {
    for (u8 prefix : {0x26,0x2E,0x36,0x3E}) {
        auto state=seed(); state.gpr.rbx=kAddressData;
        state.seg.es_desc.base=0x1000; state.seg.cs_desc.base=0x1000;
        state.seg.ss_desc.base=0x1000; state.seg.ds_desc.base=0x1000;
        check_load({prefix,0x48,0x8B,0x03},state,kAddressData,true);
    }
}

TEST_F(IrEmit64Address, AbsoluteMoffsKeepsHighAddress) {
    for(bool wide : {false,true}) {
        std::vector<u8> bytes;
        if(wide) bytes.push_back(0x48);
        bytes.push_back(0xA1);
        for(u32 shift=0;shift<64;shift+=8) bytes.push_back(static_cast<u8>(kAddressData>>shift));
        check_load(bytes,seed(),kAddressData,wide);
    }
}
#endif
