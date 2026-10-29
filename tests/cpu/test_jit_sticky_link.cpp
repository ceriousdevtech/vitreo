// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// JIT execution tests — sticky RET/continuation links and the byte-addressed L1

#include "jit_exec_test_fixture.hpp"

TEST_F(JitExecTest, StickyRetSecondCallHits) {
    // Same leaf called twice: first RET warms sticky via RSB/L1; second RET
    // should reuse the sticky host link with pins/GPRs still correct.
    //   0x1000: mov ecx, 1 ; call leaf ; mov ecx, 2 ; call leaf ; mov edx, eax
    //   0x2000: mov eax, ecx ; ret
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

    // leaf @ 0x2000: mov eax, ecx ; ret
    const u8 leaf_code[] = {0x89, 0xC8, 0xC3};
    compile_publish(0x2000, leaf_code, sizeof(leaf_code));

    // After first call @ 0x100A: mov ecx, 2 ; call leaf
    // CALL at 0x100F, next_rip=0x1014, rel32 = 0x2000 - 0x1014 = 0xFEC
    const u8 after_first[] = {
        0xB9, 0x02, 0x00, 0x00, 0x00, // mov ecx, 2
        0xE8, 0xEC, 0x0F, 0x00, 0x00, // call 0x2000
    };
    compile_publish(0x100A, after_first, sizeof(after_first));

    // Return site of second call @ 0x1014: mov edx, eax
    const u8 ret_site_code[] = {0x89, 0xC2};
    compile_publish(0x1014, ret_site_code, sizeof(ret_site_code));

    // Caller start @ 0x1000: mov ecx, 1 ; call leaf
    // CALL at 0x1005, next_rip=0x100A, rel32 = 0x2000 - 0x100A = 0xFF6
    const u8 caller_code[] = {
        0xB9, 0x01, 0x00, 0x00, 0x00, // mov ecx, 1
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
    EXPECT_EQ(m_cpu->gpr.get32(EAX), 2u);
    EXPECT_EQ(m_cpu->gpr.get32(EDX), 2u);
    EXPECT_EQ(m_cpu->gpr.get32(ECX), 2u);
    EXPECT_EQ(m_cpu->gpr.get32(ESP), 0x10000u);
    EXPECT_EQ(m_cpu->rip, 0x1016u);
}

TEST_F(JitExecTest, StickyRetTwoSiteRingHits) {
    // Alternating return sites: 2-deep sticky ring must retain both RIPs so
    // the third/fourth RETs hit sticky without depending on RSB alone.
    //   0x1000: mov ecx,1 ; call leaf ; mov ecx,2 ; call leaf ;
    //           mov ecx,3 ; call leaf ; mov ecx,4 ; call leaf ; mov edx,eax
    //   0x2000: mov eax, ecx ; ret
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

    const u8 leaf_code[] = {0x89, 0xC8, 0xC3}; // mov eax, ecx ; ret
    compile_publish(0x2000, leaf_code, sizeof(leaf_code));

    // After call1 @ 0x100A: mov ecx,2 ; call leaf → ret to 0x1014
    const u8 after1[] = {
        0xB9, 0x02, 0x00, 0x00, 0x00,
        0xE8, 0xEC, 0x0F, 0x00, 0x00, // call 0x2000 (rel=0x2000-0x1014)
    };
    compile_publish(0x100A, after1, sizeof(after1));

    // After call2 @ 0x1014: mov ecx,3 ; call leaf → ret to 0x101E
    const u8 after2[] = {
        0xB9, 0x03, 0x00, 0x00, 0x00,
        0xE8, 0xE2, 0x0F, 0x00, 0x00, // call 0x2000 (rel=0x2000-0x101E)
    };
    compile_publish(0x1014, after2, sizeof(after2));

    // After call3 @ 0x101E: mov ecx,4 ; call leaf → ret to 0x1028
    const u8 after3[] = {
        0xB9, 0x04, 0x00, 0x00, 0x00,
        0xE8, 0xD8, 0x0F, 0x00, 0x00, // call 0x2000 (rel=0x2000-0x1028)
    };
    compile_publish(0x101E, after3, sizeof(after3));

    // Final return site @ 0x1028: mov edx, eax
    const u8 ret_final[] = {0x89, 0xC2};
    compile_publish(0x1028, ret_final, sizeof(ret_final));

    // Start @ 0x1000: mov ecx,1 ; call leaf → ret to 0x100A
    const u8 caller_code[] = {
        0xB9, 0x01, 0x00, 0x00, 0x00,
        0xE8, 0xF6, 0x0F, 0x00, 0x00, // call 0x2000 (rel=0x2000-0x100A)
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
    EXPECT_EQ(m_cpu->gpr.get32(EAX), 4u);
    EXPECT_EQ(m_cpu->gpr.get32(EDX), 4u);
    EXPECT_EQ(m_cpu->gpr.get32(ECX), 4u);
    EXPECT_EQ(m_cpu->gpr.get32(ESP), 0x10000u);
    EXPECT_EQ(m_cpu->rip, 0x102Au);
}

TEST_F(JitExecTest, ByteAddressedL1KeepsCoverAndSuffixResident) {
    // Sims walk shape: TEST;Jcc at RIP and Jcc-first at RIP+2 are distinct
    // byte-addressed block starts. Both must remain L1-resident so a linked
    // continue can enter either exact start without a dispatcher round trip.
    //   0x2000: jmp 0x1000
    //   0x1000: test eax,eax ; add eax,0x10 ; jmp 0x1010
    //   0x1002: add eax,0x10 ; jmp 0x1010   (suffix start)
    //   0x1010: mov edx,eax
    struct LinkGuard {
        ~LinkGuard() { JitCompiler::disable_jit_link(); }
    } link_guard;

    BlockCache cache;
    m_jit->set_block_cache(&cache);
    JitCompiler::enable_jit_link();

    ASSERT_NE(BlockCache::l1_index(0x1000), BlockCache::l1_index(0x1002));

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
                               usize length) -> CompiledBlock* {
        auto block = decode_at(address, code, length);
        auto* compiled = m_jit->compile(block);
        EXPECT_NE(compiled, nullptr);
        if (!compiled) {
            return nullptr;
        }
        cache.insert(std::move(block));
        auto* cached = cache.lookup(address);
        EXPECT_NE(cached, nullptr);
        if (!cached) {
            return nullptr;
        }
        publish_compiled(*cached, *compiled);
        return compiled;
    };

    const u8 image[] = {
        0x85, 0xC0,                   // 1000: test eax, eax
        0x83, 0xC0, 0x10,             // 1002: add eax, 0x10
        0xE9, 0x06, 0x00, 0x00, 0x00, // 1005: jmp 0x1010 (rel=0x1010-0x100A)
    };
    const u8 pred_code[] = {
        // jmp 0x1000 from 0x2000: rel = 0x1000 - 0x2005 = -0x1005
        0xE9, 0xFB, 0xEF, 0xFF, 0xFF,
    };
    const u8 exit_code[] = {
        0x89, 0xC2, // mov edx, eax
    };

    ASSERT_TRUE(m_memory->write_bytes(0x2000, pred_code, sizeof(pred_code)));
    ASSERT_TRUE(m_memory->write_bytes(0x1000, image, sizeof(image)));
    ASSERT_TRUE(m_memory->write_bytes(0x1010, exit_code, sizeof(exit_code)));

    ASSERT_NE(compile_publish(0x1010, exit_code, sizeof(exit_code)), nullptr);
    auto* long_compiled =
        compile_publish(0x1000, image, sizeof(image));
    ASSERT_NE(long_compiled, nullptr);
    auto* alias_compiled =
        compile_publish(0x1002, image + 2, sizeof(image) - 2);
    ASSERT_NE(alias_compiled, nullptr);
    // Cover and suffix own distinct byte-addressed L1 slots.
    {
        BlockCache::L1Entry* l1 = cache.l1_base();
        ASSERT_NE(l1, nullptr);
        BasicBlock* slot = l1[BlockCache::l1_index(0x1000)].block;
        ASSERT_NE(slot, nullptr);
        EXPECT_EQ(slot->start_addr, 0x1000u);
        BasicBlock* suffix_slot = l1[BlockCache::l1_index(0x1002)].block;
        ASSERT_NE(suffix_slot, nullptr);
        EXPECT_EQ(suffix_slot->start_addr, 0x1002u);
    }
    auto* pred_compiled =
        compile_publish(0x2000, pred_code, sizeof(pred_code));
    ASSERT_NE(pred_compiled, nullptr);
    EXPECT_EQ(cache.l1_base()[BlockCache::l1_index(0x1000)].block->start_addr,
              0x1000u);
    // lookup(suffix) must not disturb the cover's independent slot.
    EXPECT_NE(cache.lookup(0x1002), nullptr);
    EXPECT_EQ(cache.l1_base()[BlockCache::l1_index(0x1000)].block->start_addr,
              0x1000u);

    m_cpu->init_protected_mode();
    m_cpu->gpr.set32(ESP, 0x10000);
    m_cpu->gpr.set32(EAX, 0);
    m_cpu->gpr.set32(EDX, 0);
    m_cpu->rip = 0x2000;
    EXPECT_EQ(test::execute_like_dispatcher(*m_jit, *pred_compiled, *m_cpu), JitExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get32(EAX), 0x10u)
        << "cover must stay in L1 despite suffix alias insert";
    EXPECT_EQ(m_cpu->gpr.get32(EDX), 0x10u);
    EXPECT_EQ(m_cpu->rip, 0x1012u);
    (void)long_compiled;
    (void)alias_compiled;
}

TEST_F(JitExecTest, StickyContSecondJccLoopHits) {
    // Cross-block JB loop so taken/fallthrough use emit_linked_tail_continue
    // (not in-block loop). Later iterations L1-link and must match the
    // interpreter on GPRs/RIP.
    //   0x1000: add eax,1 ; jmp 0x1010
    //   0x1010: cmp eax,0x20 ; jb 0x1000
    //   0x1015: mov edx,eax
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
                               usize length) -> CompiledBlock* {
        auto block = decode_at(address, code, length);
        auto* compiled = m_jit->compile(block);
        EXPECT_NE(compiled, nullptr);
        if (!compiled) {
            return nullptr;
        }
        cache.insert(std::move(block));
        auto* cached = cache.lookup(address);
        EXPECT_NE(cached, nullptr);
        if (!cached) {
            return nullptr;
        }
        publish_compiled(*cached, *compiled);
        return compiled;
    };

    const u8 body_code[] = {
        0x83, 0xC0, 0x01,             // add eax, 1
        0xE9, 0x08, 0x00, 0x00, 0x00, // jmp 0x1010 (rel=0x1010-0x1008)
    };
    const u8 check_code[] = {
        0x83, 0xF8, 0x20, // cmp eax, 0x20
        0x72, 0xEB,       // jb 0x1000 (rel=0x1000-0x1015)
    };
    const u8 exit_code[] = {
        0x89, 0xC2, // mov edx, eax
    };

    ASSERT_TRUE(m_memory->write_bytes(0x1000, body_code, sizeof(body_code)));
    ASSERT_TRUE(m_memory->write_bytes(0x1010, check_code, sizeof(check_code)));
    ASSERT_TRUE(m_memory->write_bytes(0x1015, exit_code, sizeof(exit_code)));

    ASSERT_NE(compile_publish(0x1015, exit_code, sizeof(exit_code)), nullptr);
    ASSERT_NE(compile_publish(0x1010, check_code, sizeof(check_code)), nullptr);
    auto* body_compiled =
        compile_publish(0x1000, body_code, sizeof(body_code));
    ASSERT_NE(body_compiled, nullptr);

    CpuState reference;
    reference.init_protected_mode();
    reference.gpr.set32(ESP, 0x10000);
    reference.gpr.set32(EAX, 0);
    reference.gpr.set32(EDX, 0);
    reference.rip = 0x1000;
    Interpreter interpreter(reference, *m_memory);
    (void)interpreter.run(200);

    m_cpu->init_protected_mode();
    m_cpu->gpr.set32(ESP, 0x10000);
    m_cpu->gpr.set32(EAX, 0);
    m_cpu->gpr.set32(EDX, 0);
    m_cpu->rip = 0x1000;
    EXPECT_EQ(test::execute_like_dispatcher(*m_jit, *body_compiled, *m_cpu), JitExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get32(EAX), reference.gpr.get32(EAX));
    EXPECT_EQ(m_cpu->gpr.get32(EDX), reference.gpr.get32(EDX));
    EXPECT_EQ(m_cpu->rip, reference.rip);
    EXPECT_EQ(m_cpu->gpr.get32(EAX), 0x20u);
    EXPECT_EQ(m_cpu->gpr.get32(EDX), 0x20u);
    EXPECT_EQ(m_cpu->rip, 0x1017u);
}

TEST_F(JitExecTest, StickyContAlternatingJccRingHits) {
    // Alternating JZ taken/fallthrough so a mono continue cache would thrash;
    // L1 must retain both fixed continue targets. Cross-block so paths use
    // emit_linked_tail_continue (not in-block loop).
    //   0x1000: add eax,1 ; xor ebx,1 ; test ebx,ebx ; jz 0x1020
    //   0x100A: cmp eax,0x20 ; jb 0x1000          // fallthrough leg
    //   0x100F: mov edx,eax                      // exit (fallthrough)
    //   0x1020: cmp eax,0x20 ; jb 0x1000          // taken leg
    //   0x1025: mov edx,eax                      // exit (taken)
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
                               usize length) -> CompiledBlock* {
        auto block = decode_at(address, code, length);
        auto* compiled = m_jit->compile(block);
        EXPECT_NE(compiled, nullptr);
        if (!compiled) {
            return nullptr;
        }
        cache.insert(std::move(block));
        auto* cached = cache.lookup(address);
        EXPECT_NE(cached, nullptr);
        if (!cached) {
            return nullptr;
        }
        publish_compiled(*cached, *compiled);
        return compiled;
    };

    // 0x1000..0x1009: add; xor; test; jz → taken 0x1020 / fallthrough 0x100A
    const u8 head_code[] = {
        0x83, 0xC0, 0x01, // add eax, 1
        0x83, 0xF3, 0x01, // xor ebx, 1
        0x85, 0xDB,       // test ebx, ebx
        0x74, 0x16,       // jz 0x1020 (rel=0x1020-0x100A)
    };
    // Fallthrough leg @ 0x100A
    const u8 ft_check[] = {
        0x83, 0xF8, 0x20, // cmp eax, 0x20
        0x72, 0xF1,       // jb 0x1000 (rel=0x1000-0x100F)
    };
    const u8 ft_exit[] = {
        0x89, 0xC2, // mov edx, eax
    };
    // Taken leg @ 0x1020
    const u8 tk_check[] = {
        0x83, 0xF8, 0x20, // cmp eax, 0x20
        0x72, 0xDB,       // jb 0x1000 (rel=0x1000-0x1025)
    };
    const u8 tk_exit[] = {
        0x89, 0xC2, // mov edx, eax
    };

    ASSERT_TRUE(m_memory->write_bytes(0x1000, head_code, sizeof(head_code)));
    ASSERT_TRUE(m_memory->write_bytes(0x100A, ft_check, sizeof(ft_check)));
    ASSERT_TRUE(m_memory->write_bytes(0x100F, ft_exit, sizeof(ft_exit)));
    ASSERT_TRUE(m_memory->write_bytes(0x1020, tk_check, sizeof(tk_check)));
    ASSERT_TRUE(m_memory->write_bytes(0x1025, tk_exit, sizeof(tk_exit)));

    ASSERT_NE(compile_publish(0x100F, ft_exit, sizeof(ft_exit)), nullptr);
    ASSERT_NE(compile_publish(0x1025, tk_exit, sizeof(tk_exit)), nullptr);
    ASSERT_NE(compile_publish(0x100A, ft_check, sizeof(ft_check)), nullptr);
    ASSERT_NE(compile_publish(0x1020, tk_check, sizeof(tk_check)), nullptr);
    auto* head_compiled =
        compile_publish(0x1000, head_code, sizeof(head_code));
    ASSERT_NE(head_compiled, nullptr);

    CpuState reference;
    reference.init_protected_mode();
    reference.gpr.set32(ESP, 0x10000);
    reference.gpr.set32(EAX, 0);
    reference.gpr.set32(EBX, 0);
    reference.gpr.set32(EDX, 0);
    reference.rip = 0x1000;
    Interpreter interpreter(reference, *m_memory);
    (void)interpreter.run(400);

    m_cpu->init_protected_mode();
    m_cpu->gpr.set32(ESP, 0x10000);
    m_cpu->gpr.set32(EAX, 0);
    m_cpu->gpr.set32(EBX, 0);
    m_cpu->gpr.set32(EDX, 0);
    m_cpu->rip = 0x1000;
    EXPECT_EQ(test::execute_like_dispatcher(*m_jit, *head_compiled, *m_cpu), JitExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get32(EAX), reference.gpr.get32(EAX));
    EXPECT_EQ(m_cpu->gpr.get32(EBX), reference.gpr.get32(EBX));
    EXPECT_EQ(m_cpu->gpr.get32(EDX), reference.gpr.get32(EDX));
    EXPECT_EQ(m_cpu->rip, reference.rip);
    EXPECT_EQ(m_cpu->gpr.get32(EAX), 0x20u);
    EXPECT_EQ(m_cpu->gpr.get32(EDX), 0x20u);
}
