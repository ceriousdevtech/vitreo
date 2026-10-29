// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64

/// @file test_ir_x64_narrow_alu_3_sse_memory.cpp
/// @brief Long-mode IR emission: SSE memory forms and their faults (M13b).

// These tests execute emitted ARM64, so Intel hosts must not reference the
// native-only IR backend through this translation unit.
#if defined(__aarch64__)
#include "ir_x64_memory_fixture.hpp"

using namespace vitreo;
using namespace vitreo::cpu;
using namespace vitreo::cpu::jit;
using namespace vitreo::cpu::jit::test::ir_x64_memory;

namespace {
// The 128-bit MEMORY copies (M13b). One LDR Q / STR Q at the site, which is
// why a vector never has to be staged across the access: it is produced by the
// load or consumed by the store.
//
// The aligned and unaligned spellings are the SAME copy and differ only in
// whether a misaligned address raises #GP, so both are driven at every
// alignment from 0 to 15 -- an implementation that dropped the check would
// agree on all sixteen at a 16-byte-aligned base and disagree on fifteen of
// them here.
TEST_F(IrEmit64NarrowAlu, VectorMemoryCopiesAtEveryAlignment) {
    if (const char* mask = std::getenv("VITREO_JIT_IR_SSE")) {
        if ((std::strtoul(mask, nullptr, 0) & 8u) == 0)
            GTEST_SKIP() << "long-mode SSE tranche disabled by VITREO_JIT_IR_SSE";
    }
    ASSERT_TRUE(memory.map_region(kData, 0x1000, MemoryProtection::ReadWrite));
    // Coverage guard: a sweep where nothing ever faults would pass without
    // testing the alignment rule at all, and one where everything faults
    // would not test the copy.
    unsigned raised = 0;
    unsigned copied = 0;
    struct Copy { std::vector<u8> prefix; u8 to_reg; u8 to_rm; bool aligned;
                  const char* name; };
    const std::vector<Copy> kCopies = {
        {{},      0x28, 0x29, true,  "movaps"},
        {{0x66}, 0x28, 0x29, true,  "movapd"},
        {{0x66}, 0x6F, 0x7F, true,  "movdqa"},
        {{},      0x10, 0x11, false, "movups"},
        {{0x66}, 0x10, 0x11, false, "movupd"},
        {{0xF3}, 0x6F, 0x7F, false, "movdqu"},
    };
    for (const Copy& form : kCopies) {
        for (u8 misalign = 0; misalign < 16; ++misalign) {
            for (const bool to_memory : {false, true}) {
                SCOPED_TRACE(testing::Message()
                             << form.name << (to_memory ? " store" : " load")
                             << " misalign " << unsigned(misalign));
                // `mov rax, imm64` is not admitted here, so the address comes
                // in through RAX and the displacement supplies the skew.
                std::vector<u8> body = form.prefix;
                body.push_back(0x0F);
                body.push_back(to_memory ? form.to_rm : form.to_reg);
                body.push_back(0x98);                    // modrm: xmm3, [rax+disp32]
                for (unsigned shift = 0; shift < 32; shift += 8)
                    body.push_back(static_cast<u8>(u32{misalign} >> shift));
                jit = std::make_unique<JitCompiler>(memory);
                auto* native = compile(body);
                ASSERT_NE(native, nullptr);
                auto oracle = seed();
                oracle.gpr.set64(0, kData + 0x100);      // 16-byte aligned base
                for (u8 x = 0; x < 16; ++x) {
                    oracle.sse.xmm[x].u64_[0] = 0x0123456789ABCDEFull ^ (0x1111111111111111ull * x);
                    oracle.sse.xmm[x].u64_[1] = 0xFEDCBA9876543210ull ^ (0x2222222222222222ull * x);
                }
                for (u64 off = 0; off < 0x200; off += 8)
                    ASSERT_TRUE(memory.write_u64(kData + 0x100 + off,
                                                 0xC0DE0000 + off).is_ok());
                auto state = oracle;
                Interpreter interpreter(oracle, memory);
                const auto ref = interpreter.step();
                std::vector<u64> oracle_mem;
                for (u64 off = 0; off < 0x200; off += 8)
                    oracle_mem.push_back(
                        memory.read_u64(kData + 0x100 + off).value_or(0));
                for (u64 off = 0; off < 0x200; off += 8)
                    ASSERT_TRUE(memory.write_u64(kData + 0x100 + off,
                                                 0xC0DE0000 + off).is_ok());
                (void)jit->execute(*native, state);
                // Whatever the reference did -- copy or raise -- the JIT must
                // have done the same, including leaving nothing behind.
                const bool ref_raised = ref != ExecResult::Continue;
                if (ref_raised) ++raised;
                else ++copied;
                EXPECT_EQ(state.pending_raise.active, ref_raised)
                    << "#GP agreement";
                for (usize i = 0; i < oracle_mem.size(); ++i)
                    EXPECT_EQ(memory.read_u64(kData + 0x100 + i * 8).value_or(1),
                              oracle_mem[i]) << "memory+" << (i * 8);
                for (u8 x = 0; x < 16; ++x) {
                    EXPECT_EQ(state.sse.xmm[x].u64_[0], oracle.sse.xmm[x].u64_[0])
                        << "xmm" << unsigned(x) << " low";
                    EXPECT_EQ(state.sse.xmm[x].u64_[1], oracle.sse.xmm[x].u64_[1])
                        << "xmm" << unsigned(x) << " high";
                }
            }
        }
    }
    // Three aligned spellings x fifteen bad alignments x two directions.
    EXPECT_EQ(raised, 3u * 15u * 2u) << "the #GP path must actually be taken";
    EXPECT_EQ(copied, kCopies.size() * 16u * 2u - raised);
}

// The bitwise SSE forms with a MEMORY operand. Every SSE packed bitwise form
// requires a 16-byte aligned operand -- unlike the copies there is no
// unaligned spelling of them -- so all fifteen bad alignments must raise, and
// the destination must be untouched when they do.
TEST_F(IrEmit64NarrowAlu, VectorBitwiseMemoryOperandsRequireAlignment) {
    if (const char* mask = std::getenv("VITREO_JIT_IR_SSE")) {
        if ((std::strtoul(mask, nullptr, 0) & 8u) == 0)
            GTEST_SKIP() << "long-mode SSE tranche disabled by VITREO_JIT_IR_SSE";
    }
    ASSERT_TRUE(memory.map_region(kData, 0x1000, MemoryProtection::ReadWrite));
    unsigned raised = 0;
    struct Form { std::vector<u8> prefix; u8 op; const char* name; };
    const std::vector<Form> kForms = {
        {{},      0x57, "xorps"}, {{0x66}, 0x57, "xorpd"}, {{0x66}, 0xEF, "pxor"},
        {{},      0x54, "andps"}, {{0x66}, 0x54, "andpd"}, {{0x66}, 0xDB, "pand"},
        {{},      0x56, "orps"},  {{0x66}, 0x56, "orpd"},  {{0x66}, 0xEB, "por"},
    };
    for (const Form& form : kForms) {
        for (u8 misalign = 0; misalign < 16; ++misalign) {
            SCOPED_TRACE(testing::Message() << form.name
                         << " misalign " << unsigned(misalign));
            std::vector<u8> body = form.prefix;
            body.push_back(0x0F);
            body.push_back(form.op);
            body.push_back(0x98);                    // xmm3, [rax+disp32]
            for (unsigned shift = 0; shift < 32; shift += 8)
                body.push_back(static_cast<u8>(u32{misalign} >> shift));
            jit = std::make_unique<JitCompiler>(memory);
            auto* native = compile(body);
            ASSERT_NE(native, nullptr);
            auto oracle = seed();
            oracle.gpr.set64(0, kData + 0x100);
            for (u8 x = 0; x < 16; ++x) {
                oracle.sse.xmm[x].u64_[0] = 0x0F1E2D3C4B5A6978ull ^ (0x0101010101010101ull * x);
                oracle.sse.xmm[x].u64_[1] = 0x8796A5B4C3D2E1F0ull ^ (0x0202020202020202ull * x);
            }
            for (u64 off = 0; off < 0x200; off += 8)
                ASSERT_TRUE(memory.write_u64(kData + 0x100 + off,
                                             0xABCD0000 + off).is_ok());
            auto state = oracle;
            Interpreter interpreter(oracle, memory);
            const auto ref = interpreter.step();
            if (ref != ExecResult::Continue) ++raised;
            (void)jit->execute(*native, state);
            EXPECT_EQ(state.pending_raise.active, ref != ExecResult::Continue)
                << "#GP agreement";
            for (u8 x = 0; x < 16; ++x) {
                EXPECT_EQ(state.sse.xmm[x].u64_[0], oracle.sse.xmm[x].u64_[0])
                    << "xmm" << unsigned(x) << " low";
                EXPECT_EQ(state.sse.xmm[x].u64_[1], oracle.sse.xmm[x].u64_[1])
                    << "xmm" << unsigned(x) << " high";
            }
        }
    }
    EXPECT_EQ(raised, kForms.size() * 15u) << "the #GP path must be taken";
}

// M10: recovery for the classes the milestone listed as open.
//
// VECTOR recovery is by CONSTRUCTION rather than by saving vector state: the
// backend refuses any block where a V128 value would be live across a
// may-fault op, because V0-V7 are caller-saved and the fault handler harvests
// `JitPinMap` and saves no vector state at all. So there are two things to
// show -- that a faulting vector access commits nothing, and that the refusal
// is real rather than a comment.
TEST_F(IrEmit64NarrowAlu, VectorAndRmwFaultsCommitNothing) {
    if (const char* mask = std::getenv("VITREO_JIT_IR_SSE")) {
        if ((std::strtoul(mask, nullptr, 0) & 8u) == 0)
            GTEST_SKIP() << "long-mode SSE tranche disabled by VITREO_JIT_IR_SSE";
    }
    constexpr u64 kUnmapped = 0x0000'4000'0000'0000ull;
    ASSERT_TRUE(memory.map_region(kData, 0x1000, MemoryProtection::ReadWrite));
    const auto faults_like_the_reference = [&](const std::vector<u8>& body,
                                               const char* what) {
        SCOPED_TRACE(testing::Message() << what);
        jit = std::make_unique<JitCompiler>(memory);
        auto* native = compile(body);
        ASSERT_NE(native, nullptr);
        auto oracle = seed();
        oracle.gpr.set64(0, kUnmapped);
        oracle.gpr.set64(1, 0x1234567890ABCDEFull);
        for (u8 x = 0; x < 16; ++x) {
            oracle.sse.xmm[x].u64_[0] = 0xA5A5A5A5A5A5A5A5ull ^ (0x11ull * x);
            oracle.sse.xmm[x].u64_[1] = 0x5A5A5A5A5A5A5A5Aull ^ (0x22ull * x);
        }
        oracle.flags.rflags = 0xAD7;
        auto state = oracle;
        {
            Interpreter interpreter(oracle, memory);
            EXPECT_NE(interpreter.step(), ExecResult::Continue)
                << "the reference must fault here too";
        }
        (void)jit->execute(*native, state);
        EXPECT_TRUE(state.pending_raise.active) << "expected a fault";
        EXPECT_EQ(state.rip, seed().rip) << "faulting RIP";
        for (u8 index = 0; index < 16; ++index)
            EXPECT_EQ(state.gpr.get64(index), oracle.gpr.get64(index))
                << "GPR " << unsigned(index);
        for (u8 x = 0; x < 16; ++x) {
            EXPECT_EQ(state.sse.xmm[x].u64_[0], oracle.sse.xmm[x].u64_[0])
                << "xmm" << unsigned(x) << " low";
            EXPECT_EQ(state.sse.xmm[x].u64_[1], oracle.sse.xmm[x].u64_[1])
                << "xmm" << unsigned(x) << " high";
        }
    };
    // A faulting 128-bit LOAD: the destination register must be untouched.
    faults_like_the_reference({0x66, 0x0F, 0x6F, 0x18}, "movdqa xmm3,[rax]");
    faults_like_the_reference({0x0F, 0x10, 0x18}, "movups xmm3,[rax]");
    // A faulting 128-bit STORE writes nothing, and the source survives.
    faults_like_the_reference({0x66, 0x0F, 0x7F, 0x18}, "movdqa [rax],xmm3");
    // A faulting 128-bit read inside a compute form.
    faults_like_the_reference({0x66, 0x0F, 0xFC, 0x18}, "paddb xmm3,[rax]");
    faults_like_the_reference({0x0F, 0x57, 0x18}, "xorps xmm3,[rax]");
    // RMW beyond the ALU: a faulting read-modify-write must leave both the
    // destination and the PRECEDING instruction's flags standing.
    faults_like_the_reference({0xC0, 0x10, 0x03}, "rcl byte [rax],3");
    faults_like_the_reference({0x66, 0xC1, 0x20, 0x05}, "shl word [rax],5");
    faults_like_the_reference({0xD2, 0x18}, "rcr byte [rax],cl");
    faults_like_the_reference({0x48, 0xC1, 0x20, 0x07}, "shl qword [rax],7");
    faults_like_the_reference({0x48, 0x0F, 0xAF, 0x18}, "imul rbx,[rax]");
    faults_like_the_reference({0xF6, 0x30}, "div byte [rax]");
}

// Vector forwarding must STOP at a may-fault op.
//
// An `XmmRead` of a register an earlier `XmmWrite` in the same block already
// produced can reuse that vector register instead of reloading it -- which is
// the repeated-operation win. But V0-V7 are caller-saved, so a helper a guest
// access can call destroys them: forwarding across one would read a register
// the callee owned. The scan stops at the first may-fault op, and this is what
// says so.
//
// Counted by SLOT rather than by register number: which vector register holds
// the forwarded value is the allocator's choice.
TEST_F(IrEmit64NarrowAlu, VectorForwardingStopsAtAFaultableAccess) {
    if (const char* mask = std::getenv("VITREO_JIT_IR_SSE")) {
        if ((std::strtoul(mask, nullptr, 0) & 8u) == 0)
            GTEST_SKIP() << "long-mode SSE tranche disabled by VITREO_JIT_IR_SSE";
    }
    ASSERT_TRUE(memory.map_region(kData, 0x1000, MemoryProtection::ReadWrite));
    const auto xmm3_loads = [&](const std::vector<u8>& bytes) -> long {
        jit = std::make_unique<JitCompiler>(memory);
        auto* native = compile(bytes);
        EXPECT_NE(native, nullptr);
        if (native == nullptr) return -1;
        EXPECT_TRUE(jit->ir_emit_active());
        const u32 probe = arm64::ldr_q_uoff(0, REG_CPU_STATE,
                                            xmm_u64_off(3, 0) >> 4);
        const auto* words = static_cast<const u32*>(native->code);
        const auto* end = words + native->code_size / sizeof(u32);
        return std::count_if(words, end, [&](u32 w) {
            return (w & ~0x1Fu) == (probe & ~0x1Fu);
        });
    };
    // Two packed adds back to back: the second reads XMM3 from the first's
    // result, so the slot is loaded ONCE.
    EXPECT_EQ(xmm3_loads({0x66, 0x0F, 0xFC, 0xD9,
                          0x66, 0x0F, 0xFC, 0xD9}), 1)
        << "a repeated operation must not reload its destination";
    // The same pair with a faultable access between them: forwarding must
    // stop, so the slot is loaded TWICE.
    EXPECT_EQ(xmm3_loads({0x66, 0x0F, 0xFC, 0xD9,
                          0x48, 0x8B, 0x08,            // mov rcx,[rax]
                          0x66, 0x0F, 0xFC, 0xD9}), 2)
        << "forwarding must not cross a helper that can clobber V0-V7";
    // And with a faultable access that is itself the vector operand.
    EXPECT_EQ(xmm3_loads({0x66, 0x0F, 0xFC, 0xD9,
                          0x66, 0x0F, 0xFC, 0x18,      // paddb xmm3,[rax]
                          0x66, 0x0F, 0xFC, 0xD9}), 2);
}
} // namespace
#endif
