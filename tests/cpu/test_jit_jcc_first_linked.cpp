// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// JIT execution tests — Jcc-first blocks entered by link or dispatcher

#include "jit_exec_test_fixture.hpp"

TEST_F(JitExecTest, DispatcherPersistedNzcvThenJccFirstMatchesInterpreter) {
    // CMP block's linked tail misses (Jcc is not in L1). The miss must
    // persist NZCV into CpuState so the next execute() of a Jcc-first
    // block fuses instead of calling jit_cond_*.
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

    const u8 cmp_code[] = {0x39, 0xD8};  // cmp eax, ebx
    auto cmp = decode_at(0x1000, cmp_code, sizeof(cmp_code));
    auto* cmp_compiled = m_jit->compile(cmp);
    ASSERT_NE(cmp_compiled, nullptr);
    CompiledBlock cmp_cb = *cmp_compiled;

    const u8 jcc_code[] = {0x7D, 0x02};  // jge +2
    auto jcc = decode_at(0x1002, jcc_code, sizeof(jcc_code));
    auto* jcc_compiled = m_jit->compile(jcc);
    ASSERT_NE(jcc_compiled, nullptr);
    CompiledBlock jcc_cb = *jcc_compiled;

    const std::pair<u32, u32> pairs[] = {
        {5, 3}, {3, 5}, {5, 5}, {0x80000000u, 1}, {1, 0x80000000u},
    };
    for (auto [eax, ebx] : pairs) {
        CpuState reference;
        reference.init_protected_mode();
        reference.gpr.set32(ESP, 0x10000);
        reference.gpr.set32(EAX, eax);
        reference.gpr.set32(EBX, ebx);
        reference.rip = 0x1000;
        Interpreter interpreter(reference, *m_memory);
        (void)interpreter.run(2);

        m_cpu->init_protected_mode();
        m_cpu->gpr.set32(ESP, 0x10000);
        m_cpu->gpr.set32(EAX, eax);
        m_cpu->gpr.set32(EBX, ebx);
        m_cpu->rip = 0x1000;
        EXPECT_EQ(test::execute_like_dispatcher(*m_jit, cmp_cb, *m_cpu),
                  JitExecResult::Continue);
        EXPECT_NE(m_cpu->nzcv_kind, 0u)
            << "link miss must persist kind eax=0x" << std::hex << eax;
        EXPECT_EQ(m_cpu->rip, 0x1002u);
        EXPECT_EQ(test::execute_like_dispatcher(*m_jit, jcc_cb, *m_cpu),
                  JitExecResult::Continue);
        EXPECT_EQ(m_cpu->rip, reference.rip)
            << "eax=0x" << std::hex << eax << " ebx=0x" << ebx;
    }
}

TEST_F(JitExecTest, LinkedCmpThenMovThenJccMatchesInterpreter) {
    // CMP in block A, MOV+JGE in block B. Register MOV is flag-neutral and
    // does not clobber host NZCV, so B must still fuse from the linked kind
    // instead of calling jit_cond_* (Intel SDM Vol. 2 MOV / Jcc).
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

    const u8 jcc_code[] = {0x89, 0xC9, 0x7D, 0x02};  // mov ecx, ecx; jge +2
    auto jcc = decode_at(0x1002, jcc_code, sizeof(jcc_code));
    auto* jcc_compiled = m_jit->compile(jcc);
    ASSERT_NE(jcc_compiled, nullptr);
    cache.insert(std::move(jcc));
    auto* cached_jcc = cache.lookup(0x1002);
    ASSERT_NE(cached_jcc, nullptr);
    publish_compiled(*cached_jcc, *jcc_compiled);

    const u8 cmp_code[] = {0x39, 0xD8};  // cmp eax, ebx
    auto cmp = decode_at(0x1000, cmp_code, sizeof(cmp_code));
    auto* cmp_compiled = m_jit->compile(cmp);
    ASSERT_NE(cmp_compiled, nullptr);

    const std::pair<u32, u32> pairs[] = {
        {5, 3}, {3, 5}, {5, 5}, {0x80000000u, 1}, {1, 0x80000000u},
    };
    for (auto [eax, ebx] : pairs) {
        CpuState reference;
        reference.init_protected_mode();
        reference.gpr.set32(ESP, 0x10000);
        reference.gpr.set32(EAX, eax);
        reference.gpr.set32(EBX, ebx);
        reference.rip = 0x1000;
        Interpreter interpreter(reference, *m_memory);
        (void)interpreter.run(3);

        m_cpu->init_protected_mode();
        m_cpu->gpr.set32(ESP, 0x10000);
        m_cpu->gpr.set32(EAX, eax);
        m_cpu->gpr.set32(EBX, ebx);
        m_cpu->rip = 0x1000;
        EXPECT_EQ(test::execute_like_dispatcher(*m_jit, *cmp_compiled, *m_cpu),
                  JitExecResult::Continue);
        EXPECT_EQ(m_cpu->rip, reference.rip)
            << "eax=0x" << std::hex << eax << " ebx=0x" << ebx;
        EXPECT_EQ(m_cpu->gpr.get32(ECX), reference.gpr.get32(ECX));
    }
}


TEST_F(JitExecTest, LinkedCmpThenJccFirstMatchesInterpreter) {
    // CMP in block A, JGE-first in block B. The link must keep ARM NZCV
    // so B.cond matches the interpreter (Intel SDM Vol. 2 Jcc).
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

    const u8 jcc_code[] = {0x7D, 0x02};  // jge +2
    auto jcc = decode_at(0x1002, jcc_code, sizeof(jcc_code));
    auto* jcc_compiled = m_jit->compile(jcc);
    ASSERT_NE(jcc_compiled, nullptr);
    cache.insert(std::move(jcc));
    auto* cached_jcc = cache.lookup(0x1002);
    ASSERT_NE(cached_jcc, nullptr);
    publish_compiled(*cached_jcc, *jcc_compiled);

    const u8 cmp_code[] = {0x39, 0xD8};  // cmp eax, ebx
    auto cmp = decode_at(0x1000, cmp_code, sizeof(cmp_code));
    auto* cmp_compiled = m_jit->compile(cmp);
    ASSERT_NE(cmp_compiled, nullptr);

    const std::pair<u32, u32> pairs[] = {
        {5, 3}, {3, 5}, {5, 5}, {0x80000000u, 1}, {1, 0x80000000u},
    };
    for (auto [eax, ebx] : pairs) {
        CpuState reference;
        reference.init_protected_mode();
        reference.gpr.set32(ESP, 0x10000);
        reference.gpr.set32(EAX, eax);
        reference.gpr.set32(EBX, ebx);
        reference.rip = 0x1000;
        Interpreter interpreter(reference, *m_memory);
        (void)interpreter.run(2);

        m_cpu->init_protected_mode();
        m_cpu->gpr.set32(ESP, 0x10000);
        m_cpu->gpr.set32(EAX, eax);
        m_cpu->gpr.set32(EBX, ebx);
        m_cpu->rip = 0x1000;
        EXPECT_EQ(test::execute_like_dispatcher(*m_jit, *cmp_compiled, *m_cpu),
                  JitExecResult::Continue);
        EXPECT_EQ(m_cpu->rip, reference.rip)
            << "eax=0x" << std::hex << eax << " ebx=0x" << ebx;
        EXPECT_NE(m_cpu->nzcv_kind, 0u);
    }
}

TEST_F(JitExecTest, LinkedTestThenJneFirstMatchesInterpreter) {
    // TEST in block A, JNZ-first in block B. Same ZF contract as
    // `test eax, eax; jne` after a call (Intel SDM Vol. 2 TEST / Jcc).
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

    const u8 jcc_code[] = {0x75, 0x02};  // jnz +2
    auto jcc = decode_at(0x1002, jcc_code, sizeof(jcc_code));
    auto* jcc_compiled = m_jit->compile(jcc);
    ASSERT_NE(jcc_compiled, nullptr);
    cache.insert(std::move(jcc));
    auto* cached_jcc = cache.lookup(0x1002);
    ASSERT_NE(cached_jcc, nullptr);
    publish_compiled(*cached_jcc, *jcc_compiled);

    const u8 test_code[] = {0x85, 0xC0};  // test eax, eax
    auto test = decode_at(0x1000, test_code, sizeof(test_code));
    auto* test_compiled = m_jit->compile(test);
    ASSERT_NE(test_compiled, nullptr);

    const u32 eaxes[] = {0u, 1u, 0x80000000u, 0xFFFFFFFFu};
    for (u32 eax : eaxes) {
        CpuState reference;
        reference.init_protected_mode();
        reference.gpr.set32(ESP, 0x10000);
        reference.gpr.set32(EAX, eax);
        reference.rip = 0x1000;
        Interpreter interpreter(reference, *m_memory);
        (void)interpreter.run(2);

        m_cpu->init_protected_mode();
        m_cpu->gpr.set32(ESP, 0x10000);
        m_cpu->gpr.set32(EAX, eax);
        m_cpu->rip = 0x1000;
        EXPECT_EQ(test::execute_like_dispatcher(*m_jit, *test_compiled, *m_cpu),
                  JitExecResult::Continue);
        EXPECT_EQ(m_cpu->rip, reference.rip) << "eax=0x" << std::hex << eax;
    }
}

TEST_F(JitExecTest, LinkedTestThenJccFirstMatchesInterpreter) {
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

    const u8 jcc_code[] = {0x7D, 0x02};  // jge +2
    auto jcc = decode_at(0x1002, jcc_code, sizeof(jcc_code));
    auto* jcc_compiled = m_jit->compile(jcc);
    ASSERT_NE(jcc_compiled, nullptr);
    cache.insert(std::move(jcc));
    auto* cached_jcc = cache.lookup(0x1002);
    ASSERT_NE(cached_jcc, nullptr);
    publish_compiled(*cached_jcc, *jcc_compiled);

    const u8 test_code[] = {0x85, 0xED};  // test ebp, ebp
    auto test = decode_at(0x1000, test_code, sizeof(test_code));
    auto* test_compiled = m_jit->compile(test);
    ASSERT_NE(test_compiled, nullptr);

    const u32 ebps[] = {0u, 1u, 0x80000000u, 0xFFFFFFFFu};
    for (u32 ebp : ebps) {
        CpuState reference;
        reference.init_protected_mode();
        reference.gpr.set32(ESP, 0x10000);
        reference.gpr.set32(EBP, ebp);
        reference.rip = 0x1000;
        Interpreter interpreter(reference, *m_memory);
        (void)interpreter.run(2);

        m_cpu->init_protected_mode();
        m_cpu->gpr.set32(ESP, 0x10000);
        m_cpu->gpr.set32(EBP, ebp);
        m_cpu->rip = 0x1000;
        EXPECT_EQ(test::execute_like_dispatcher(*m_jit, *test_compiled, *m_cpu),
                  JitExecResult::Continue);
        EXPECT_EQ(m_cpu->rip, reference.rip) << "ebp=0x" << std::hex << ebp;
    }
}

TEST_F(JitExecTest, JccFirstViaDispatcherUsesHelper) {
    // execute() clobbers NZCV and the epilogue clears nzcv_kind. A
    // Jcc-first block entered from C++ must still match via FlagOp.
    const u8 cmp_code[] = {0x39, 0xD8};  // cmp eax, ebx
    ASSERT_TRUE(m_memory->write_bytes(0x1000, cmp_code, sizeof(cmp_code)));
    const u8 jcc_code[] = {0x7D, 0x02};  // jge +2
    ASSERT_TRUE(m_memory->write_bytes(0x1002, jcc_code, sizeof(jcc_code)));

    BasicBlock jcc_block;
    jcc_block.start_addr = 0x1002;
    MemoryByteReader reader(jcc_code, sizeof(jcc_code));
    DecodedInstruction inst;
    ASSERT_EQ(decode_instruction(reader, DecodeMode::Mode32, inst),
              DecodeError::Success);
    jcc_block.instructions.push_back(inst);
    jcc_block.byte_length = 2;
    jcc_block.instruction_count = 1;
    CompiledBlock* jcc_compiled = m_jit->compile(jcc_block);
    ASSERT_NE(jcc_compiled, nullptr);

    const std::pair<u32, u32> pairs[] = {{5, 3}, {3, 5}, {0x80000000u, 1}};
    for (auto [eax, ebx] : pairs) {
        CpuState reference;
        reference.init_protected_mode();
        reference.gpr.set32(ESP, 0x10000);
        reference.gpr.set32(EAX, eax);
        reference.gpr.set32(EBX, ebx);
        reference.rip = 0x1000;
        Interpreter interpreter(reference, *m_memory);
        (void)interpreter.run(2);

        m_cpu->init_protected_mode();
        m_cpu->gpr.set32(ESP, 0x10000);
        m_cpu->gpr.set32(EAX, eax);
        m_cpu->gpr.set32(EBX, ebx);
        m_cpu->rip = 0x1000;
        Interpreter prep(*m_cpu, *m_memory);
        (void)prep.run(1);
        EXPECT_EQ(m_cpu->rip, 0x1002u);
        EXPECT_EQ(m_cpu->nzcv_kind, 0u);
        EXPECT_EQ(test::execute_like_dispatcher(*m_jit, *jcc_compiled, *m_cpu),
                  JitExecResult::Continue);
        EXPECT_EQ(m_cpu->rip, reference.rip)
            << "eax=0x" << std::hex << eax << " ebx=0x" << ebx;
    }
}

TEST_F(JitExecTest, LinkedTestThenJbFirstDoesNotTake) {
    // TEST clears CF. JB must not fire. A Sub-map B.CC on ANDS (C=0)
    // would take and is exactly why unsigned Jccs stay on the helper.
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

    const u8 jb_code[] = {0x72, 0x02};  // jb +2
    auto jb = decode_at(0x1002, jb_code, sizeof(jb_code));
    auto* jb_compiled = m_jit->compile(jb);
    ASSERT_NE(jb_compiled, nullptr);
    cache.insert(std::move(jb));
    auto* cached_jb = cache.lookup(0x1002);
    ASSERT_NE(cached_jb, nullptr);
    publish_compiled(*cached_jb, *jb_compiled);

    const u8 test_code[] = {0x85, 0xC0};  // test eax, eax
    auto test = decode_at(0x1000, test_code, sizeof(test_code));
    auto* test_compiled = m_jit->compile(test);
    ASSERT_NE(test_compiled, nullptr);

    m_cpu->init_protected_mode();
    m_cpu->gpr.set32(ESP, 0x10000);
    m_cpu->gpr.set32(EAX, 1);
    m_cpu->rip = 0x1000;
    EXPECT_EQ(test::execute_like_dispatcher(*m_jit, *test_compiled, *m_cpu),
              JitExecResult::Continue);
    EXPECT_EQ(m_cpu->rip, 0x1004u);
}
