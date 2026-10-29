// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// JIT execution tests — linked hops and CALL/RET with pinned registers

#include "jit_exec_test_fixture.hpp"

TEST_F(JitExecTest, LinkedHopKeepsPinnedGpr) {
    // Source writes ECX in a pin. The hop must not reload pins from
    // CpuState (still the pre-block value). Target copies ECX into EAX.
    struct LinkGuard {
        ~LinkGuard() { JitCompiler::disable_jit_link(); }
    } link_guard;

    BlockCache cache;
    m_jit->set_block_cache(&cache);
    JitCompiler::enable_jit_link();

    auto decode_at = [&](GuestAddr address, const u8* code,
                         usize length) -> BasicBlock {
        EXPECT_TRUE(m_memory->write_bytes(address, code, length));
        BasicBlock block;
        block.start_addr = address;
        MemoryByteReader reader(code, length);
        while (reader.remaining() > 0) {
            DecodedInstruction instruction;
            if (decode_instruction(reader, DecodeMode::Mode32, instruction) !=
                DecodeError::Success) {
                break;
            }
            block.instructions.push_back(instruction);
        }
        block.byte_length = static_cast<u32>(reader.position());
        block.instruction_count =
            static_cast<u32>(block.instructions.size());
        return block;
    };

    const u8 target_code[] = {0x89, 0xC8};  // mov eax, ecx
    auto target = decode_at(0x2000, target_code, sizeof(target_code));
    auto* target_compiled = m_jit->compile(target);
    ASSERT_NE(target_compiled, nullptr);
    EXPECT_GT(target_compiled->link_code_offset, sizeof(u32));
    cache.insert(std::move(target));
    auto* cached_target = cache.lookup(0x2000);
    ASSERT_NE(cached_target, nullptr);
    publish_compiled(*cached_target, *target_compiled);

    const u8 source_code[] = {
        0xB9, 0x07, 0x00, 0x00, 0x00, // mov ecx, 7
        0xE9, 0xF6, 0x0F, 0x00, 0x00, // jmp 0x2000 from 0x1005
    };
    auto source = decode_at(0x1000, source_code, sizeof(source_code));
    auto* source_compiled = m_jit->compile(source);
    ASSERT_NE(source_compiled, nullptr);

    m_cpu->gpr.set32(ECX, 0x99);
    m_cpu->gpr.set32(EAX, 0);
    m_cpu->rip = 0x1000;
    EXPECT_EQ(test::execute_like_dispatcher(*m_jit, *source_compiled, *m_cpu),
              JitExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get32(EAX), 7u);
    EXPECT_EQ(m_cpu->gpr.get32(ECX), 7u);
    EXPECT_EQ(m_cpu->rip, 0x2002u);
}

TEST_F(JitExecTest, LinkedCallRetKeepsPinnedGpr) {
    // RSB/L1 must not reload pins from stale CpuState across CALL/RET hops.
    // Linear layout:
    //   0x1000: mov ecx, 7 ; call 0x2000 ; mov edx, eax
    //   0x2000: mov eax, ecx ; ret
    // CALL ends its block; RET returns to a separate block at 0x100A.
    struct LinkGuard {
        ~LinkGuard() { JitCompiler::disable_jit_link(); }
    } link_guard;

    BlockCache cache;
    m_jit->set_block_cache(&cache);
    JitCompiler::enable_jit_link();

    auto decode_at = [&](GuestAddr address, const u8* code,
                         usize length) -> BasicBlock {
        EXPECT_TRUE(m_memory->write_bytes(address, code, length));
        BasicBlock block;
        block.start_addr = address;
        MemoryByteReader reader(code, length);
        while (reader.remaining() > 0) {
            DecodedInstruction instruction;
            if (decode_instruction(reader, DecodeMode::Mode32, instruction) !=
                DecodeError::Success) {
                break;
            }
            block.instructions.push_back(instruction);
        }
        block.byte_length = static_cast<u32>(reader.position());
        block.instruction_count =
            static_cast<u32>(block.instructions.size());
        return block;
    };

    auto compile_publish = [&](GuestAddr address, const u8* code,
                               usize length) {
        auto block = decode_at(address, code, length);
        auto* compiled = m_jit->compile(block);
        ASSERT_NE(compiled, nullptr);
        cache.insert(std::move(block));
        auto* cached = cache.lookup(address);
        ASSERT_NE(cached, nullptr);
        publish_compiled(*cached, *compiled);
    };

    // Callee @ 0x2000: mov eax, ecx ; ret
    const u8 callee_code[] = {0x89, 0xC8, 0xC3};
    compile_publish(0x2000, callee_code, sizeof(callee_code));

    // Return site @ 0x100A: mov edx, eax  (must be in L1 for RSB host_link)
    const u8 ret_site_code[] = {0x89, 0xC2};
    compile_publish(0x100A, ret_site_code, sizeof(ret_site_code));

    // Caller @ 0x1000: mov ecx, 7 ; call 0x2000
    // CALL at 0x1005, next_rip=0x100A, rel32 = 0x2000 - 0x100A = 0xFF6
    const u8 caller_code[] = {
        0xB9, 0x07, 0x00, 0x00, 0x00, // mov ecx, 7
        0xE8, 0xF6, 0x0F, 0x00, 0x00, // call 0x2000
    };
    auto caller = decode_at(0x1000, caller_code, sizeof(caller_code));
    auto* caller_compiled = m_jit->compile(caller);
    ASSERT_NE(caller_compiled, nullptr);

    m_cpu->gpr.set32(ECX, 0x99);
    m_cpu->gpr.set32(EAX, 0);
    m_cpu->gpr.set32(EDX, 0);
    m_cpu->gpr.set32(ESP, 0x10000);
    m_cpu->rip = 0x1000;
    EXPECT_EQ(test::execute_like_dispatcher(*m_jit, *caller_compiled, *m_cpu),
              JitExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get32(EAX), 7u);
    EXPECT_EQ(m_cpu->gpr.get32(EDX), 7u);
    EXPECT_EQ(m_cpu->gpr.get32(ECX), 7u);
    EXPECT_EQ(m_cpu->gpr.get32(ESP), 0x10000u);
    EXPECT_EQ(m_cpu->rip, 0x100Cu);
}

TEST_F(JitExecTest, LinkedRetImmNestedMatchesInterpreter) {
    // Nested CALL with mid `ret 4` (stdcall cleanup). JIT linked hops must
    // match the interpreter on GPRs and ESP.
    //   outer: push imm32 ; call mid ; mov ebx, eax
    //   mid:   mov ecx, 7 ; call inner ; ret 4
    //   inner: mov eax, ecx ; ret
    struct LinkGuard {
        ~LinkGuard() { JitCompiler::disable_jit_link(); }
    } link_guard;

    BlockCache cache;
    m_jit->set_block_cache(&cache);
    JitCompiler::enable_jit_link();

    auto decode_at = [&](GuestAddr address, const u8* code,
                         usize length) -> BasicBlock {
        EXPECT_TRUE(m_memory->write_bytes(address, code, length));
        BasicBlock block;
        block.start_addr = address;
        MemoryByteReader reader(code, length);
        while (reader.remaining() > 0) {
            DecodedInstruction instruction;
            if (decode_instruction(reader, DecodeMode::Mode32, instruction) !=
                DecodeError::Success) {
                break;
            }
            block.instructions.push_back(instruction);
        }
        block.byte_length = static_cast<u32>(reader.position());
        block.instruction_count =
            static_cast<u32>(block.instructions.size());
        return block;
    };

    auto compile_publish = [&](GuestAddr address, const u8* code,
                               usize length) {
        auto block = decode_at(address, code, length);
        auto* compiled = m_jit->compile(block);
        ASSERT_NE(compiled, nullptr);
        cache.insert(std::move(block));
        auto* cached = cache.lookup(address);
        ASSERT_NE(cached, nullptr);
        publish_compiled(*cached, *compiled);
    };

    // Place full linear images so the interpreter can step the same bytes.
    // outer @ 0x1000
    const u8 outer_bytes[] = {
        0x68, 0x11, 0x11, 0x11, 0x11, // push 0x11111111
        0xE8, 0xF6, 0x0F, 0x00, 0x00, // call 0x2000 (rel=0x2000-0x100A)
        0x89, 0xC3,                   // mov ebx, eax
    };
    ASSERT_TRUE(m_memory->write_bytes(0x1000, outer_bytes, sizeof(outer_bytes)));

    // mid @ 0x2000
    const u8 mid_bytes[] = {
        0xB9, 0x07, 0x00, 0x00, 0x00, // mov ecx, 7
        0xE8, 0xF6, 0x0F, 0x00, 0x00, // call 0x3000 (rel=0x3000-0x200A)
        0xC2, 0x04, 0x00,             // ret 4
    };
    ASSERT_TRUE(m_memory->write_bytes(0x2000, mid_bytes, sizeof(mid_bytes)));

    // inner @ 0x3000
    const u8 inner_bytes[] = {0x89, 0xC8, 0xC3}; // mov eax, ecx ; ret
    ASSERT_TRUE(m_memory->write_bytes(0x3000, inner_bytes, sizeof(inner_bytes)));

    compile_publish(0x3000, inner_bytes, sizeof(inner_bytes));
    compile_publish(0x200A, mid_bytes + 10, 3); // ret 4
    compile_publish(0x2000, mid_bytes, 10);     // mov ecx,7 ; call
    compile_publish(0x100A, outer_bytes + 10, 2); // mov ebx, eax

    const u8 outer_call[] = {
        0x68, 0x11, 0x11, 0x11, 0x11,
        0xE8, 0xF6, 0x0F, 0x00, 0x00,
    };
    auto outer = decode_at(0x1000, outer_call, sizeof(outer_call));
    auto* outer_compiled = m_jit->compile(outer);
    ASSERT_NE(outer_compiled, nullptr);

    CpuState reference;
    reference.init_protected_mode();
    reference.gpr.set32(ESP, 0x10000);
    reference.gpr.set32(EAX, 0);
    reference.gpr.set32(EBX, 0);
    reference.gpr.set32(ECX, 0x99);
    reference.rip = 0x1000;
    Interpreter interpreter(reference, *m_memory);
    (void)interpreter.run(16);
    ASSERT_EQ(reference.rip, 0x100Cu);

    m_cpu->init_protected_mode();
    m_cpu->gpr.set32(ESP, 0x10000);
    m_cpu->gpr.set32(EAX, 0);
    m_cpu->gpr.set32(EBX, 0);
    m_cpu->gpr.set32(ECX, 0x99);
    m_cpu->rip = 0x1000;
    EXPECT_EQ(test::execute_like_dispatcher(*m_jit, *outer_compiled, *m_cpu),
              JitExecResult::Continue);

    EXPECT_EQ(m_cpu->gpr.get32(EAX), reference.gpr.get32(EAX));
    EXPECT_EQ(m_cpu->gpr.get32(EBX), reference.gpr.get32(EBX));
    EXPECT_EQ(m_cpu->gpr.get32(ECX), reference.gpr.get32(ECX));
    EXPECT_EQ(m_cpu->gpr.get32(ESP), reference.gpr.get32(ESP));
    EXPECT_EQ(m_cpu->rip, reference.rip);
    EXPECT_EQ(m_cpu->gpr.get32(EAX), 7u);
    EXPECT_EQ(m_cpu->gpr.get32(EBX), 7u);
    EXPECT_EQ(m_cpu->gpr.get32(ESP), 0x10000u);
}

TEST_F(JitExecTest, CallRetAfterEspWriteStillCorrect) {
    // CALL/RET must follow the guest return RIP loaded from [ESP], not a
    // stale ESP frame. Rewrite ESP before the call; stack push/pop and the
    // RSB prediction must still agree on the live stack.
    struct LinkGuard {
        ~LinkGuard() { JitCompiler::disable_jit_link(); }
    } link_guard;

    BlockCache cache;
    m_jit->set_block_cache(&cache);
    JitCompiler::enable_jit_link();

    auto decode_at = [&](GuestAddr address, const u8* code,
                         usize length) -> BasicBlock {
        EXPECT_TRUE(m_memory->write_bytes(address, code, length));
        BasicBlock block;
        block.start_addr = address;
        MemoryByteReader reader(code, length);
        while (reader.remaining() > 0) {
            DecodedInstruction instruction;
            if (decode_instruction(reader, DecodeMode::Mode32, instruction) !=
                DecodeError::Success) {
                break;
            }
            block.instructions.push_back(instruction);
        }
        block.byte_length = static_cast<u32>(reader.position());
        block.instruction_count =
            static_cast<u32>(block.instructions.size());
        return block;
    };

    auto compile_publish = [&](GuestAddr address, const u8* code,
                               usize length) {
        auto block = decode_at(address, code, length);
        auto* compiled = m_jit->compile(block);
        ASSERT_NE(compiled, nullptr);
        cache.insert(std::move(block));
        auto* cached = cache.lookup(address);
        ASSERT_NE(cached, nullptr);
        publish_compiled(*cached, *compiled);
    };

    const u8 callee_code[] = {0x89, 0xC8, 0xC3}; // mov eax, ecx ; ret
    compile_publish(0x2000, callee_code, sizeof(callee_code));

    // Return site @ 0x100F: mov edx, eax
    // Caller body: mov esp,0xF000 (5) ; mov ecx,7 (5) ; call (5) → 0x100F
    const u8 ret_site_code[] = {0x89, 0xC2};
    compile_publish(0x100F, ret_site_code, sizeof(ret_site_code));

    // call at 0x100A, next_rip=0x100F, rel32 = 0x2000 - 0x100F = 0xFF1
    const u8 caller_code[] = {
        0xBC, 0x00, 0xF0, 0x00, 0x00, // mov esp, 0xF000
        0xB9, 0x07, 0x00, 0x00, 0x00, // mov ecx, 7
        0xE8, 0xF1, 0x0F, 0x00, 0x00, // call 0x2000
    };
    auto caller = decode_at(0x1000, caller_code, sizeof(caller_code));
    auto* caller_compiled = m_jit->compile(caller);
    ASSERT_NE(caller_compiled, nullptr);

    m_cpu->gpr.set32(ESP, 0x10000);
    m_cpu->gpr.set32(ECX, 0x99);
    m_cpu->gpr.set32(EAX, 0);
    m_cpu->gpr.set32(EDX, 0);
    m_cpu->rip = 0x1000;
    EXPECT_EQ(test::execute_like_dispatcher(*m_jit, *caller_compiled, *m_cpu),
              JitExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get32(EAX), 7u);
    EXPECT_EQ(m_cpu->gpr.get32(EDX), 7u);
    EXPECT_EQ(m_cpu->gpr.get32(ECX), 7u);
    EXPECT_EQ(m_cpu->gpr.get32(ESP), 0xF000u);
    EXPECT_EQ(m_cpu->rip, 0x1011u);
}
