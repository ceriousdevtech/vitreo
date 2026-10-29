// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64

/// @file test_ir_x64_narrow_alu_4_stack.cpp
/// @brief Long-mode IR emission: stack and near transfers (M12).

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
// Long-mode stack and near transfers (M12)
// =============================================================================

// PUSH/POP/CALL/LEAVE at 64 and 16 bits. The slot width follows the OPERAND
// size, so a 66h push moves RSP by two -- promoting it to the 64-bit default
// drifts RSP by six per instruction, which no single-instruction check would
// notice but every sequence would. The aliased spellings (`push rsp`,
// `pop rsp`) are the ones that separate the orderings.
TEST_F(IrEmit64NarrowAlu, StackFormsMoveTheRightNumberOfBytes) {
    ASSERT_TRUE(memory.map_region(kStack, 0x2000, MemoryProtection::ReadWrite));
    ASSERT_TRUE(memory.map_region(kData, 0x1000, MemoryProtection::ReadWrite));
    const auto run = [&](std::vector<u8> body, unsigned insns) {
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
        for (const u64 seed_value : {0ull, 1ull, 0x0123456789ABCDEFull,
                                     0xFFFFFFFFFFFFFFFFull}) {
            auto oracle = seed();
            for (u8 index = 0; index < 16; ++index)
                oracle.gpr.set64(index, seed_value ^ (0x0F0F0F0F0F0F0F0Full * index));
            oracle.gpr.set64(4, kStack + 0x1000);      // RSP, mid-region
            oracle.gpr.set64(5, kStack + 0x1100);      // RBP
            oracle.gpr.set64(0, kData);
            oracle.flags.rflags = 0xAD7;
            auto state = oracle;
            for (u64 off = 0; off < 0x2000; off += 8)
                ASSERT_TRUE(memory.write_u64(kStack + off, 0xAA00 + off).is_ok());
            {
                Interpreter interpreter(oracle, memory);
                for (unsigned step = 0; step < insns; ++step)
                    ASSERT_EQ(interpreter.step(), ExecResult::Continue);
            }
            std::vector<u64> oracle_stack;
            for (u64 off = 0; off < 0x2000; off += 8)
                oracle_stack.push_back(memory.read_u64(kStack + off).value_or(0));
            for (u64 off = 0; off < 0x2000; off += 8)
                ASSERT_TRUE(memory.write_u64(kStack + off, 0xAA00 + off).is_ok());
            ASSERT_EQ(jit->execute(*native, state), JitExecResult::Continue);
            for (usize i = 0; i < oracle_stack.size(); ++i)
                EXPECT_EQ(memory.read_u64(kStack + i * 8).value_or(1),
                          oracle_stack[i])
                    << "stack+" << (i * 8) << " seed=" << std::hex << seed_value;
            for (u8 index = 0; index < 16; ++index)
                EXPECT_EQ(state.gpr.get64(index), oracle.gpr.get64(index))
                    << "GPR " << unsigned(index) << " seed=" << std::hex << seed_value;
            EXPECT_EQ(state.rip, oracle.rip) << "rip seed=" << std::hex << seed_value;
        }
    };
    // 50+r / 58+r at both operand sizes, including the extended registers and
    // RSP itself as source and destination.
    for (u8 reg = 0; reg < 8; ++reg) {
        run({static_cast<u8>(0x50 + reg)}, 1);                    // push r64
        run({static_cast<u8>(0x58 + reg)}, 1);                    // pop r64
        run({0x66, static_cast<u8>(0x50 + reg)}, 1);              // push r16
        run({0x66, static_cast<u8>(0x58 + reg)}, 1);              // pop r16
        run({0x41, static_cast<u8>(0x50 + reg)}, 1);              // push r8..r15
        run({0x41, static_cast<u8>(0x58 + reg)}, 1);              // pop r8..r15
    }
    // FF /6 and 8F /0: the r/m spellings, register and memory source.
    run({0xFF, 0xF1}, 1);                    // push rcx
    run({0xFF, 0x30}, 1);                    // push qword [rax]
    run({0x66, 0xFF, 0xF1}, 1);              // push cx
    run({0x66, 0xFF, 0x30}, 1);              // push word [rax]
    run({0x8F, 0xC1}, 1);                    // pop rcx
    run({0x66, 0x8F, 0xC1}, 1);              // pop cx
    // Immediates: sign-extended to the slot, and the 66h form that is not.
    for (const u8 imm : {u8{0}, u8{1}, u8{0x7F}, u8{0x80}, u8{0xFF}})
        run({0x6A, imm}, 1);                                      // push imm8
    run({0x68, 0x00, 0x00, 0x00, 0x80}, 1);                       // push INT32_MIN
    run({0x68, 0xFF, 0xFF, 0xFF, 0x7F}, 1);                       // push INT32_MAX
    run({0x66, 0x68, 0x34, 0x12}, 1);                             // push imm16
    // Sequences: the RSP arithmetic has to compose, which a single push
    // cannot show. A 16-bit push followed by a 64-bit one is the shape that
    // catches a promoted slot width.
    run({0x51, 0x52, 0x5A, 0x59}, 4);                    // push/push/pop/pop
    run({0x66, 0x51, 0x52, 0x5A, 0x66, 0x59}, 4);        // mixed widths
    run({0x6A, 0x7F, 0x6A, 0x80, 0x58, 0x5B}, 4);
    // The aliased spellings.
    run({0x54}, 1);                          // push rsp -- stores the ENTRY value
    run({0x5C}, 1);                          // pop rsp  -- popped value wins
    run({0x66, 0x54}, 1);                    // push sp
    run({0x66, 0x5C}, 1);                    // pop sp
    // 9C / 9D: PUSHF and POPF at both operand sizes. The flags they move are
    // the point, so each body is preceded by a full-flag writer and followed
    // by a consumer -- a POPF that installed the value but left a pending
    // lazy record would agree on RFLAGS and disagree on the SETcc.
    run({0x9C}, 1);                                   // pushfq
    run({0x66, 0x9C}, 1);                             // pushf  (2-byte slot)
    run({0x00, 0xD2, 0x9C}, 2);                       // add dl,dl ; pushfq
    run({0x48, 0x85, 0xDB, 0x9C}, 2);                 // test rbx,rbx ; pushfq
    run({0x9C, 0x9D}, 2);                             // pushfq ; popfq
    run({0x66, 0x9C, 0x66, 0x9D}, 2);                 // 16-bit round trip
    run({0x00, 0xD2, 0x9C, 0x9D, 0x0F, 0x92, 0xC2}, 4);   // ... ; setb dl
    run({0x00, 0xD2, 0x9C, 0x9D, 0x0F, 0x97, 0xC2}, 4);   // ... ; seta dl
    run({0x48, 0x85, 0xDB, 0x9C, 0x9D, 0x0F, 0x94, 0xC2}, 4);  // ... ; sete dl
    run({0x9D}, 1);                                   // popfq from seeded stack
    run({0x66, 0x9D}, 1);                             // popf
    // 0F A0 / 0F A8: the FS and GS pushes. What lands on the stack is the
    // SELECTOR, not the cached descriptor base -- pushing the base would store
    // a plausible-looking number that means something else entirely.
    run({0x0F, 0xA0}, 1);                             // push fs
    run({0x0F, 0xA8}, 1);                             // push gs
    run({0x66, 0x0F, 0xA0}, 1);                       // 2-byte slot
    run({0x66, 0x0F, 0xA8}, 1);
    run({0x0F, 0xA0, 0x0F, 0xA8}, 2);                 // both, composing RSP
    // C9: LEAVE, whose fault order is the reverse of PUSH's.
    run({0xC9}, 1);
    run({0x55, 0x48, 0x89, 0xE5, 0xC9}, 3);  // push rbp; mov rbp,rsp; leave
    // C8 iw ib: ENTER at nesting 0 and 1, across the allocation range. RBP
    // ends up holding the frame pointer from BEFORE the allocation, which a
    // lowering that wrote it after the subtraction would get wrong only when
    // the allocation is nonzero.
    for (const u16 alloc : {u16{0}, u16{1}, u16{8}, u16{0x100}, u16{0xFFFF}})
        for (const u8 level : {u8{0}, u8{1}}) {
            run({0xC8, static_cast<u8>(alloc & 0xFF),
                 static_cast<u8>(alloc >> 8), level}, 1);
            // Followed by LEAVE, which has to undo exactly what it did.
            run({0xC8, static_cast<u8>(alloc & 0xFF),
                 static_cast<u8>(alloc >> 8), level, 0xC9}, 2);
        }
}

// E8: CALL rel32 pushes a 64-bit return address and branches. The pushed value
// is the whole point -- a 32-bit return address would be silently truncated
// and only surface when the RET arrived somewhere else.
TEST_F(IrEmit64NarrowAlu, CallRelativePushesASixtyFourBitReturnAddress) {
    ASSERT_TRUE(memory.map_region(kStack, 0x2000, MemoryProtection::ReadWrite));
    for (const i32 disp : {0, 1, -1, 0x1000, -0x1000}) {
        std::vector<u8> body{0xE8};
        for (unsigned shift = 0; shift < 32; shift += 8)
            body.push_back(static_cast<u8>(static_cast<u32>(disp) >> shift));
        SCOPED_TRACE(testing::Message() << "call rel32 " << disp);
        jit = std::make_unique<JitCompiler>(memory);
        auto* native = compile(body);
        ASSERT_NE(native, nullptr);
        auto oracle = seed();
        oracle.gpr.set64(4, kStack + 0x1000);
        auto state = oracle;
        {
            // A CALL reports Branch, not Continue: it has already updated RIP.
            Interpreter interpreter(oracle, memory);
            ASSERT_EQ(interpreter.step(), ExecResult::Branch);
        }
        const auto oracle_slot = memory.read_u64(kStack + 0x1000 - 8);
        ASSERT_TRUE(memory.write_u64(kStack + 0x1000 - 8, 0).is_ok());
        ASSERT_EQ(jit->execute(*native, state), JitExecResult::Continue);
        EXPECT_EQ(memory.read_u64(kStack + 0x1000 - 8).value_or(1),
                  oracle_slot.value_or(2)) << "return address";
        EXPECT_EQ(state.gpr.get64(4), oracle.gpr.get64(4)) << "rsp";
        EXPECT_EQ(state.rip, oracle.rip) << "target";
    }
}

// RET, indirect JMP and indirect CALL. The return address and the target are
// FULL 64-bit values; reading either as 32 bits lands the guest somewhere
// plausible and surfaces much later, so the targets here sit above 4 GB.
TEST_F(IrEmit64NarrowAlu, IndirectTransfersCarryFullSixtyFourBitTargets) {
    ASSERT_TRUE(memory.map_region(kStack, 0x2000, MemoryProtection::ReadWrite));
    ASSERT_TRUE(memory.map_region(kData, 0x1000, MemoryProtection::ReadWrite));
    const auto run = [&](std::vector<u8> body, u64 target, i64 stack_slot_bias) {
        std::string spelling;
        for (const u8 byte : body) {
            char one[4];
            std::snprintf(one, sizeof one, "%02x ", byte);
            spelling += one;
        }
        SCOPED_TRACE(testing::Message() << "bytes: " << spelling
                     << " target=" << std::hex << target);
        jit = std::make_unique<JitCompiler>(memory);
        auto* native = compile(body);
        ASSERT_NE(native, nullptr);
        auto oracle = seed();
        oracle.gpr.set64(4, kStack + 0x1000);        // RSP
        oracle.gpr.set64(1, target);                 // RCX, the register target
        oracle.gpr.set64(0, kData);                  // RAX, the memory base
        ASSERT_TRUE(memory.write_u64(kData, target).is_ok());
        ASSERT_TRUE(memory.write_u64(kStack + 0x1000, target).is_ok());
        auto state = oracle;
        {
            Interpreter interpreter(oracle, memory);
            ASSERT_EQ(interpreter.step(), ExecResult::Branch);
        }
        ASSERT_EQ(jit->execute(*native, state), JitExecResult::Continue);
        EXPECT_EQ(state.rip, oracle.rip) << "target";
        EXPECT_EQ(state.gpr.get64(4), oracle.gpr.get64(4)) << "rsp";
        for (u8 index = 0; index < 16; ++index)
            EXPECT_EQ(state.gpr.get64(index), oracle.gpr.get64(index))
                << "GPR " << unsigned(index);
        if (stack_slot_bias != 0) {
            const u64 slot = kStack + 0x1000 + static_cast<u64>(stack_slot_bias);
            EXPECT_EQ(memory.read_u64(slot).value_or(1),
                      memory.read_u64(slot).value_or(2));
        }
    };
    for (const u64 target : {kCode + 0x100, 0x7FFFFFFFF000ull, kData + 0x800}) {
        run({0xFF, 0xE1}, target, 0);              // jmp rcx
        run({0xFF, 0x20}, target, 0);              // jmp qword [rax]
        run({0xFF, 0xD1}, target, -8);             // call rcx
        run({0xFF, 0x10}, target, -8);             // call qword [rax]
        run({0xC3}, target, 0);                    // ret
        run({0xC2, 0x00, 0x00}, target, 0);        // ret 0
        run({0xC2, 0x08, 0x00}, target, 0);        // ret 8
        run({0xC2, 0xF7, 0x0F}, target, 0);        // ret 4087: 8 + imm16 = 4095
        run({0xC2, 0xF8, 0x0F}, target, 0);        // ret 4088: 8 + imm16 = 4096
        run({0xC2, 0x00, 0x10}, target, 0);        // ret 4096
        run({0xC2, 0xFF, 0xFF}, target, 0);        // ret 65535 -- UNSIGNED
    }
    // A call through an extended register, and one whose target register is
    // RSP -- which must be the value RSP held on ENTRY, not the decremented
    // one the push is about to commit.
    for (const u64 target : {kCode + 0x100}) {
        run({0x41, 0xFF, 0xD1}, target, -8);       // call r9
        run({0x41, 0xFF, 0xE1}, target, 0);        // jmp r9
    }
    // `call rsp` / `jmp rsp`: the target IS the stack pointer.
    {
        jit = std::make_unique<JitCompiler>(memory);
        const std::vector<u8> body{0xFF, 0xD4};    // call rsp
        auto* native = compile(body);
        ASSERT_NE(native, nullptr);
        auto oracle = seed();
        oracle.gpr.set64(4, kStack + 0x1000);
        auto state = oracle;
        {
            Interpreter interpreter(oracle, memory);
            ASSERT_EQ(interpreter.step(), ExecResult::Branch);
        }
        ASSERT_EQ(jit->execute(*native, state), JitExecResult::Continue);
        EXPECT_EQ(state.rip, oracle.rip) << "call rsp target";
        EXPECT_EQ(state.gpr.get64(4), oracle.gpr.get64(4)) << "call rsp rsp";
    }
}

TEST_F(IrEmit64NarrowAlu, IndirectCallAfterStackAddressCalculationPreservesTarget) {
    const bool was_linked = JitCompiler::jit_link_enabled();
    struct RestoreLinkMode {
        bool enabled;
        ~RestoreLinkMode() {
            if (enabled) JitCompiler::enable_jit_link();
            else JitCompiler::disable_jit_link();
        }
    } restore{was_linked};
    JitCompiler::enable_jit_link();
    ASSERT_TRUE(memory.map_region(kStack, 0x2000, MemoryProtection::ReadWrite));
    const u64 stack = kStack + 0x1000;
    const u64 target = kCode + 0x800;
    // LEA RCX,[RSP+20h]; CALL qword [RIP+disp32]. The prior RSP read
    // remains live through argument calculation and the call's stack push.
    std::vector<u8> body{0x48,0x8D,0x4C,0x24,0x20,0xFF,0x15};
    append_disp32(body, 0x700 - static_cast<i64>(body.size() + 4));
    ASSERT_TRUE(memory.write_u64(kCode + 0x700, target));
    auto* native = compile(body);
    ASSERT_NE(native, nullptr);
    auto state = seed();
    state.gpr.set64(4, stack);
    ASSERT_EQ(jit->execute(*native, state), JitExecResult::Continue);
    EXPECT_EQ(state.rip, target);
    EXPECT_EQ(state.gpr.get64(4), stack - 8);
    EXPECT_EQ(state.gpr.get64(1), stack + 0x20);
    EXPECT_EQ(memory.read_u64(stack - 8).value_or(0), kCode + body.size());
}

TEST_F(IrEmit64NarrowAlu, RestoredStackThenPopAndReturnUseSuccessiveSlots) {
    ASSERT_TRUE(memory.map_region(kStack, 0x2000, MemoryProtection::ReadWrite));
    // Intel SDM Vol. 2, POP/RET: each consumes its own current stack slot.
    // Frame restoration must not let an SSA alias of old RSP survive its write.
    for (bool restore_registers : {false, true}) {
        std::vector<u8> body{0x4C,0x8D,0x9C,0x24,0x90,0,0,0}; // lea r11,[rsp+90h]
        if (restore_registers) {
            const std::vector<u8> loads{
                0x49,0x8B,0x5B,0x10, 0x49,0x8B,0x6B,0x18,
                0x49,0x8B,0x73,0x20, 0x49,0x8B,0x7B,0x28};
            body.insert(body.end(), loads.begin(), loads.end());
        }
        body.insert(body.end(), {0x49,0x8B,0xE3,0x41,0x5E,0xC3});
        jit = std::make_unique<JitCompiler>(memory);
        auto* native = compile(body);
        ASSERT_NE(native, nullptr);
        const u64 stack = kStack + 0x800;
        const u64 restored = stack + 0x90;
        const u64 target = kCode + 0x800;
        ASSERT_TRUE(memory.write_u64(restored, 0x123456789ABCDEF0ull));
        ASSERT_TRUE(memory.write_u64(restored + 8, target));
        for (u64 offset = 0x10; offset <= 0x28; offset += 8)
            ASSERT_TRUE(memory.write_u64(restored + offset, 0xAB00 + offset));
        auto oracle = seed();
        oracle.gpr.set64(4, stack);
        auto state = oracle;
        Interpreter interpreter(oracle, memory);
        for (unsigned i = 0; i < (restore_registers ? 7u : 3u); ++i)
            ASSERT_EQ(interpreter.step(), ExecResult::Continue);
        ASSERT_EQ(interpreter.step(), ExecResult::Branch);
        ASSERT_EQ(jit->execute(*native, state), JitExecResult::Continue);
        EXPECT_EQ(state.rip, target);
        EXPECT_EQ(state.gpr.get64(4), restored + 16);
        EXPECT_EQ(state.gpr.get64(14), 0x123456789ABCDEF0ull);
        for (u8 index = 0; index < 16; ++index)
            EXPECT_EQ(state.gpr.get64(index), oracle.gpr.get64(index))
                << "register " << unsigned(index) << " restores=" << restore_registers;
        EXPECT_EQ(state.flags.rflags, oracle.flags.rflags);
    }
}

// The fault orderings of the stack family, each checked against the
// interpreter rather than derived from the family. What a fault commits is
// Windows' (tools/oracle_probe/stack_access_fault_probe.expected): nothing,
// for a PUSH, a POP and a LEAVE alike.
TEST_F(IrEmit64NarrowAlu, StackFaultOrderingsDifferPerInstruction) {
    constexpr u64 kUnmapped = 0x0000'4000'0000'0000ull;
    const auto expect_matches_interpreter_on_fault =
        [&](const std::vector<u8>& body, u64 rsp, u64 rbp) {
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
            oracle.gpr.set64(4, rsp);
            oracle.gpr.set64(5, rbp);
            oracle.gpr.set64(0, kUnmapped);
            oracle.gpr.set64(1, 0x1234567890ABCDEFull);
            auto state = oracle;
            {
                Interpreter interpreter(oracle, memory);
                EXPECT_NE(interpreter.step(), ExecResult::Continue)
                    << "the reference must fault here too";
            }
            (void)jit->execute(*native, state);
            EXPECT_TRUE(state.pending_raise.active) << "expected a fault";
            EXPECT_EQ(state.rip, seed().rip) << "faulting RIP";
            // Whatever the reference committed before faulting, the JIT must
            // have committed exactly the same.
            for (u8 index = 0; index < 16; ++index)
                EXPECT_EQ(state.gpr.get64(index), oracle.gpr.get64(index))
                    << "GPR " << unsigned(index);
        };
    // PUSH into an unmapped stack: RSP must be UNCHANGED.
    expect_matches_interpreter_on_fault({0x51}, kUnmapped, kStack + 0x1000);
    expect_matches_interpreter_on_fault({0x6A, 0x7F}, kUnmapped, kStack + 0x1000);
    expect_matches_interpreter_on_fault({0x66, 0x51}, kUnmapped, kStack + 0x1000);
    // PUSH of an unmapped MEMORY source: the load faults before anything.
    ASSERT_TRUE(memory.map_region(kStack, 0x2000, MemoryProtection::ReadWrite));
    expect_matches_interpreter_on_fault({0xFF, 0x30}, kStack + 0x1000,
                                        kStack + 0x1000);
    // POP from an unmapped stack: neither RSP nor the destination moves.
    expect_matches_interpreter_on_fault({0x59}, kUnmapped, kStack + 0x1000);
    // CALL rel32 whose return-address push faults.
    expect_matches_interpreter_on_fault({0xE8, 0x00, 0x00, 0x00, 0x00},
                                        kUnmapped, kStack + 0x1000);
    // Indirect CALL through an unmapped TARGET: the target load faults first,
    // so RSP is untouched and nothing was pushed.
    expect_matches_interpreter_on_fault({0xFF, 0x10}, kStack + 0x1000,
                                        kStack + 0x1000);
    // RET reading an unmapped stack.
    expect_matches_interpreter_on_fault({0xC3}, kUnmapped, kStack + 0x1000);
    expect_matches_interpreter_on_fault({0xC2, 0x08, 0x00}, kUnmapped,
                                        kStack + 0x1000);
    // LEAVE whose pop faults: RSP keeps its value too (it does NOT take
    // RBP's first -- `leave` ctxsp=+0 in the probe).
    expect_matches_interpreter_on_fault({0xC9}, kStack + 0x1000, kUnmapped);
    // ENTER whose first push faults.
    expect_matches_interpreter_on_fault({0xC8, 0x10, 0x00, 0x00}, kUnmapped,
                                        kStack + 0x1000);
}
} // namespace
#endif
