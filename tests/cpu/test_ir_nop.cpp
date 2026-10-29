// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64

/// @file test_ir_nop.cpp
/// @brief Tier 1 IR: NOP, multi-byte NOP r/m and the hint-NOP encodings.

// Intel SDM Vol. 2B NOP: "This instruction performs no operation ... The
// multi-byte form of NOP is available on processors with model encoding ...
// 0F 1F /0 NOP r/m16, NOP r/m32". The r/m operand is not accessed, so no
// address is formed and nothing can fault, whatever the ModRM/SIB/disp and
// prefixes say. Flags Affected: None. Every case runs the same bytes on the
// interpreter and asserts the block was compiled by the IR, not delegated.
#if defined(__aarch64__)
#include "ir_emit_linear_fixture.hpp"
#include "ir_x64_memory_fixture.hpp"

using namespace vitreo;
using namespace vitreo::cpu;
using namespace vitreo::cpu::jit;
using namespace vitreo::cpu::jit::test::ir_x64_memory;

namespace {

struct NopForm {
    std::vector<u8> bytes;
    const char* name;
};

// The encodings compilers emit for padding (Intel SDM Vol. 2B NOP, Table
// "Recommended Multi-Byte Sequence of NOP Instruction"), plus the forms whose
// operand would fault if it were accessed. RBX/EBX is seeded so that
// [rbx+disp] is unmapped.
const std::vector<NopForm>& nop_forms(bool long_mode) {
    static const std::vector<NopForm> common{
        {{0x90}, "nop"},
        {{0x66, 0x90}, "66 nop"},
        {{0x0F, 0x1F, 0x00}, "nop [eax]"},
        {{0x0F, 0x1F, 0x40, 0x00}, "nop [eax+0]"},
        {{0x0F, 0x1F, 0x44, 0x00, 0x00}, "nop [eax+eax+0]"},
        {{0x66, 0x0F, 0x1F, 0x44, 0x00, 0x00}, "66 nop [eax+eax+0]"},
        {{0x0F, 0x1F, 0x80, 0x00, 0x00, 0x00, 0x00}, "nop [eax+0] disp32"},
        {{0x0F, 0x1F, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00}, "nop [eax+eax+0] disp32"},
        {{0x66, 0x0F, 0x1F, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00}, "66 nop disp32 sib"},
        {{0x66, 0x66, 0x2E, 0x0F, 0x1F, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00},
         "66 66 cs nop disp32 sib"},
        {{0x0F, 0x1F, 0x83, 0x00, 0x00, 0xF0, 0x7F}, "nop [ebx+0x7ff00000]"},
        {{0x0F, 0x1F, 0x04, 0x9B}, "nop [ebx+ebx*4]"},
        {{0x0F, 0x1F, 0xC1}, "nop ecx (register form)"},
        {{0x0F, 0x1F, 0x05, 0x00, 0x00, 0xF0, 0x7F}, "nop [disp32 / rip+disp32]"},
        {{0x64, 0x0F, 0x1F, 0x03}, "fs nop [ebx]"},
        {{0xF3, 0x0F, 0x1F, 0x03}, "f3 nop [ebx]"},
        {{0xF2, 0x0F, 0x1F, 0x03}, "f2 nop [ebx]"},
        {{0x0F, 0x18, 0x0B}, "prefetcht0 [ebx] (hint nop)"},
        {{0x0F, 0x0D, 0x0B}, "prefetchw [ebx] (hint nop)"},
        {{0x0F, 0x19, 0x03}, "0f 19 reserved nop"},
        {{0xF3, 0x0F, 0x1E, 0xFB}, "endbr32"},
        {{0xF3, 0x0F, 0x1E, 0xFA}, "endbr64"},
    };
    static const std::vector<NopForm> wide = [] {
        std::vector<NopForm> forms = common;
        // 16-bit addressing in protected mode is a decoder gap (modrm.cpp),
        // so `67` is exercised in long mode only, where it selects 32-bit.
        forms.push_back({{0x67, 0x0F, 0x1F, 0x03}, "67 nop [ebx]"});
        forms.push_back({{0x48, 0x90}, "rex.w nop"});
        forms.push_back({{0x41, 0x0F, 0x1F, 0x00}, "rex.b nop [r8]"});
        forms.push_back({{0x4B, 0x0F, 0x1F, 0x44, 0x23, 0x10}, "rex.wxb nop [r11+r12+16]"});
        forms.push_back({{0x48, 0x0F, 0x1F, 0xC0}, "rex.w nop rax"});
        forms.push_back({{0x65, 0x48, 0x0F, 0x1F, 0x03}, "gs rex.w nop [rbx]"});
        return forms;
    }();
    return long_mode ? wide : common;
}

// ---- Long mode -------------------------------------------------------------

class IrX64Nop : public IrEmit64NarrowAlu {};

// Between a carry producer and its consumer: the NOP must leave the live
// flags (and the PSTATE carry claim) exactly as they were.
TEST_F(IrX64Nop, EveryFormIsCompiledAndChangesNothing) {
    ASSERT_TRUE(memory.map_region(kData, 0x1000, MemoryProtection::ReadWrite));
    const std::vector<u8> before{0x01, 0xD1};          // add ecx,edx
    const std::vector<u8> after{0x83, 0xD6, 0x00};     // adc esi,0
    for (const NopForm& form : nop_forms(true)) {
        SCOPED_TRACE(form.name);
        std::vector<u8> body = before;
        body.insert(body.end(), form.bytes.begin(), form.bytes.end());
        body.insert(body.end(), after.begin(), after.end());
        expect_alu_matches_interpreter(body, 3);
    }
}

// A block whose IR would be empty (only NOPs) is left to the stencil path by
// ir_emit_feasible, so each NOP follows one MOV here.
TEST_F(IrX64Nop, AnOperandThatWouldFaultIsNeverAccessed) {
    for (const NopForm& form : nop_forms(true)) {
        SCOPED_TRACE(form.name);
        std::vector<u8> body{0x89, 0xD1};               // mov ecx,edx
        body.insert(body.end(), form.bytes.begin(), form.bytes.end());
        jit = std::make_unique<JitCompiler>(memory);
        auto* native = compile(body);
        ASSERT_NE(native, nullptr);
        auto oracle = seed();
        for (u8 index = 0; index < 16; ++index)
            oracle.gpr.set64(index, 0x00007FFF00000000ull + index * 0x10000ull);
        auto state = oracle;
        {
            Interpreter interpreter(oracle, memory);
            ASSERT_EQ(interpreter.step(), ExecResult::Continue);
            ASSERT_EQ(interpreter.step(), ExecResult::Continue);
        }
        ASSERT_EQ(jit->execute(*native, state), JitExecResult::Continue);
        EXPECT_FALSE(state.pending_raise.active);
        EXPECT_EQ(state.rip, oracle.rip);
        EXPECT_EQ(state.rip, kCode + body.size());
        for (u8 index = 0; index < 16; ++index)
            EXPECT_EQ(state.gpr.get64(index), oracle.gpr.get64(index)) << unsigned(index);
        state.materialize_lazy_flags();
        oracle.materialize_lazy_flags();
        EXPECT_EQ(state.flags.rflags, oracle.flags.rflags);
    }
}

// A NOP after the block's last Jcc adds no IR, but the Jcc is still not the
// last instruction: the not-taken edge runs through the NOP and leaves at
// the block's end. Both edges here land on the same RIP.
TEST_F(IrX64Nop, ATrailingNopAfterAJccStillRunsOnTheFallThrough) {
    // cmp ecx,edx; jne +3; nop dword [rax]
    const std::vector<u8> body{0x39, 0xD1, 0x75, 0x03, 0x0F, 0x1F, 0x00};
    jit = std::make_unique<JitCompiler>(memory);
    auto* native = compile(body);
    ASSERT_NE(native, nullptr);
    for (const bool equal : {true, false}) {
        SCOPED_TRACE(equal ? "not taken" : "taken");
        auto oracle = seed();
        oracle.gpr.rcx = 5;
        oracle.gpr.rdx = equal ? 5 : 6;
        auto state = oracle;
        {
            Interpreter interpreter(oracle, memory);
            while (oracle.rip < kCode + body.size()) {
                const ExecResult r = interpreter.step();
                ASSERT_TRUE(r == ExecResult::Continue || r == ExecResult::Branch);
            }
        }
        ASSERT_EQ(jit->execute(*native, state), JitExecResult::Continue);
        EXPECT_EQ(state.rip, kCode + body.size());
        EXPECT_EQ(state.rip, oracle.rip);
        state.materialize_lazy_flags();
        oracle.materialize_lazy_flags();
        EXPECT_EQ(state.flags.rflags, oracle.flags.rflags);
    }
}

// ---- Protected mode (x86) --------------------------------------------------

using test::ir_emit::capture;
using test::ir_emit::FaultFormResult;
class IrX86Nop : public test::ir_emit::IrEmitLinear {};

// The ADD's flags must survive the NOP and the flag-neutral MOV unchanged.
TEST_F(IrX86Nop, EveryFormIsCompiledAndChangesNothing) {
    const std::vector<u8> before{0x01, 0xD1};          // add ecx,edx
    const std::vector<u8> after{0x89, 0xCE};           // mov esi,ecx
    for (const NopForm& form : nop_forms(false)) {
        SCOPED_TRACE(form.name);
        std::vector<u8> code = before;
        code.insert(code.end(), form.bytes.begin(), form.bytes.end());
        code.insert(code.end(), after.begin(), after.end());
        ASSERT_TRUE(m_memory->write_bytes(kLinCode, code.data(), code.size()));
        for (const u32 seed : {0u, 1u, 0x7FFFFFFFu, 0xFFFFFFFFu}) {
            CpuState oracle;
            oracle.init_protected_mode();
            for (u8 i = 0; i < 8; ++i) oracle.gpr.set32(i, seed ^ (0x01010101u * i));
            oracle.gpr.set32(3, 0x7E000000u);   // EBX: inside the reservation, unmapped
            oracle.gpr.set32(4, kLinData + 0x800);
            oracle.rip = kLinCode;
            CpuState cpu = oracle;
            {
                Interpreter interpreter(oracle, *m_memory);
                for (int step = 0; step < 3; ++step)
                    ASSERT_EQ(interpreter.step(), ExecResult::Continue);
            }
            m_jit = std::make_unique<JitCompiler>(*m_memory);
            BasicBlock block = decode_at(code);
            ASSERT_EQ(block.instruction_count, 3u);
            ASSERT_TRUE(m_jit->ir_build_block(block).ok)
                << ir::decline_reason_name(m_jit->ir_last_result().reason);
            CompiledBlock* compiled = m_jit->compile(block);
            ASSERT_NE(compiled, nullptr);
            ASSERT_TRUE(m_jit->ir_emit_active());
            m_jit->execute(*compiled, cpu);
            const FaultFormResult got = capture(cpu);
            const FaultFormResult want = capture(oracle);
            EXPECT_FALSE(got.raised);
            EXPECT_EQ(got.rip, want.rip);
            for (u8 i = 0; i < 8; ++i) EXPECT_EQ(got.gpr[i], want.gpr[i]) << unsigned(i);
            EXPECT_EQ(got.rflags, want.rflags);
        }
    }
}

}  // namespace
#endif
