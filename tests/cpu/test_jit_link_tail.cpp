// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// JIT execution tests — linked tails and JIT-iteration publishing

#include "jit_exec_test_fixture.hpp"

TEST_F(JitExecTest, LinkedTailEntersTargetAfterNativePrologue) {
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

    const u8 target_code[] = {
        0xB8, 0x2A, 0x00, 0x00, 0x00, // mov eax,42
    };
    auto target = decode_at(0x2000, target_code, sizeof(target_code));
    auto* target_compiled = m_jit->compile(target);
    ASSERT_NE(target_compiled, nullptr);
    CompiledBlock target_cb = *target_compiled;
    cache.insert(std::move(target));
    auto* cached_target = cache.lookup(0x2000);
    ASSERT_NE(cached_target, nullptr);
    publish_compiled(*cached_target, *target_compiled);

    const u8 source_code[] = {
        0xE9, 0xFB, 0x0F, 0x00, 0x00, // jmp 0x2000 from 0x1000
    };
    auto source = decode_at(0x1000, source_code, sizeof(source_code));
    auto* source_compiled = m_jit->compile(source);
    ASSERT_NE(source_compiled, nullptr);

    m_cpu->rip = 0x1000;
    EXPECT_EQ(test::execute_like_dispatcher(*m_jit, *source_compiled, *m_cpu),
              JitExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get32(EAX), 42u);
    EXPECT_EQ(m_cpu->rip, 0x2005u);
}

TEST_F(JitExecTest, LinkedTailPinLayoutMismatchFallsBack) {
    struct Guard {
        ~Guard() { JitCompiler::disable_jit_link(); }
    } guard;

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

    const u8 target_code[] = {0x89, 0xC8}; // mov eax,ecx
    auto target = decode_at(0x1000, target_code, sizeof(target_code));
    CompiledBlock* target_compiled = m_jit->compile(target);
    ASSERT_NE(target_compiled, nullptr);
    cache.insert(std::move(target));
    BasicBlock* cached_target = cache.lookup(0x1000);
    ASSERT_NE(cached_target, nullptr);
    publish_compiled(*cached_target, *target_compiled);
    cached_target->compiled_pin_layout ^= 1; // incompatible published ABI
    cached_target->compiled_link_code = nullptr; // no safe transition entry
    // The L1 slot caches both by value. Production only reaches these fields
    // through publish_compiled() / the arena-retire path, which refresh the
    // slot themselves; a test that writes them directly has to do the same or
    // the emitted probe keeps hopping on the pre-mutation copies.
    l1_note_published(*cached_target);

    const u8 source_code[] = {
        0xB9, 0x07, 0x00, 0x00, 0x00,       // mov ecx,7
        0xE9, 0xF6, 0xEF, 0xFF, 0xFF,       // jmp 0x1000
    };
    auto source = decode_at(0x2000, source_code, sizeof(source_code));
    CompiledBlock* source_compiled = m_jit->compile(source);
    ASSERT_NE(source_compiled, nullptr);

    m_cpu->gpr.set32(EAX, 0xA5A5A5A5u);
    m_cpu->gpr.set32(ECX, 0x99u);
    m_cpu->rip = 0x2000;
    EXPECT_EQ(test::execute_like_dispatcher(*m_jit, *source_compiled, *m_cpu),
              JitExecResult::Continue);
    EXPECT_EQ(m_cpu->rip, 0x1000u);
    EXPECT_EQ(m_cpu->gpr.get32(ECX), 7u);
    EXPECT_EQ(m_cpu->gpr.get32(EAX), 0xA5A5A5A5u);
}

TEST_F(JitExecTest, InChainApiExitTestsTheSlotHotAndCallsTheExportCold) {
    SharedExitStubsOff legacy_exits;
    // L3: an API-call tail's in-chain exit has two parts. The slot-map test
    // runs on every API call the L1 probe misses, so it stays in the block's
    // hot part; the call out to the runtime runs only for an in-chain export
    // and goes to the cold part. The block must still be PLACED split --
    // nothing in either part may be PC-relative across the split (the
    // placement's pcrel refusal) -- or the cold part would land back in hot.
    // The in-chain exit hangs off the linked indirect tail (its L1 miss).
    struct CodegenGuard {
        CodegenGuard() {
            JitCompiler::enable_jit_link();
            JitCompiler::set_inchain_thunks_codegen_for_test(1);
        }
        ~CodegenGuard() {
            JitCompiler::set_inchain_thunks_codegen_for_test(-1);
            JitCompiler::disable_jit_link();
        }
    } codegen_guard;
    m_jit->set_thunk_region(0x70000000u, 0x70001000u);

    const u8 code[] = {0xFF, 0x15, 0x00, 0x20, 0x00, 0x00};  // call [0x2000]
    BasicBlock block = build_block(code, sizeof(code));
    CompiledBlock* compiled = m_jit->compile(block);
    ASSERT_NE(compiled, nullptr);
    const std::vector<WordRange> ranges = block_word_ranges(*compiled);
    ASSERT_EQ(ranges.size(), 2u) << "the block is no longer placed split";
    const WordRange& hot = ranges[0];
    const WordRange& cold = ranges[1];
    const auto count = [](const WordRange& r, u32 word) {
        return std::count(r.begin, r.end, word);
    };
    const u32 load_map = arm64::ldr_x_uoff(
        17, REG_JIT_CTX, offsetof(JitContext, inchain_slot_map) >> 3);
    const u32 load_slot = arm64::ldrb_reg(3, 17, 16);
    const u32 load_trampoline = arm64::ldr_x_uoff(
        16, REG_JIT_CTX, offsetof(JitContext, inchain_trampoline) >> 3);
    const u32 blr_x16 = 0xD63F0200u;
    EXPECT_EQ(count(hot, load_map), 1) << "the slot test left the hot part";
    EXPECT_EQ(count(hot, load_slot), 1);
    EXPECT_EQ(count(cold, load_map), 0);
    EXPECT_EQ(count(hot, load_trampoline), 0) << "the call part is hot";
    ASSERT_EQ(count(cold, load_trampoline), 1);
    const u32* at = std::find(cold.begin, cold.end, load_trampoline);
    ASSERT_LT(at + 1, cold.end);
    EXPECT_EQ(at[1], blr_x16);

    // Off, the tail is the plain continue exit: no trace of either part.
    JitCompiler::set_inchain_thunks_codegen_for_test(0);
    BasicBlock off_block = build_block(code, sizeof(code));
    CompiledBlock* off = m_jit->compile(off_block);
    ASSERT_NE(off, nullptr);
    for (const WordRange& range : block_word_ranges(*off)) {
        EXPECT_EQ(count(range, load_map), 0);
        EXPECT_EQ(count(range, load_trampoline), 0);
    }
}

TEST_F(JitExecTest, DirectLinkPublishesJitItersOnlyOnExit) {
    SharedExitStubsOff legacy_exits;
    struct LinkGuard {
        ~LinkGuard() { JitCompiler::disable_jit_link(); }
    } link_guard;

    JitCompiler::enable_jit_link();

    const u8 code[] = {
        0xE9, 0xFB, 0x0F, 0x00, 0x00, // jmp 0x2000 from 0x1000
    };
    BasicBlock block = build_block(code, sizeof(code));
    CompiledBlock* compiled = m_jit->compile(block);
    ASSERT_NE(compiled, nullptr);

    // Hot part then cold part, in buffer order: the publication is a miss
    // path, so under split placement it lives in the cold part.
    std::vector<u32> words;
    for (const WordRange& range : block_word_ranges(*compiled)) {
        words.insert(words.end(), range.begin, range.end);
    }
    const u32 br_x16 = 0xD61F0200u;
    const u32 publish_iters = arm64::str_w_uoff(
        REG_LOOP_CTR, REG_CPU_STATE, jit_iters_offset() >> 2);
    const auto br = std::find(words.begin(), words.end(), br_x16);
    const auto publish = std::find(words.begin(), words.end(), publish_iters);
    ASSERT_NE(br, words.end());
    ASSERT_NE(publish, words.end());
    EXPECT_GT(publish, br)
        << "jit_iters publication still executes before a successful link";

    // With no resident target the direct lookup reaches the cold publisher.
    m_cpu->rip = 0x1000;
    EXPECT_EQ(test::execute_like_dispatcher(*m_jit, *compiled, *m_cpu),
              JitExecResult::Continue);
    EXPECT_EQ(m_cpu->rip, 0x2000u);
    EXPECT_EQ(m_cpu->jit_iters, 1u)
        << "direct-link fallback did not publish the source block count";
}

TEST_F(JitExecTest, IndirectLinkPublishesJitItersOnlyOnExit) {
    SharedExitStubsOff legacy_exits;
    struct LinkGuard {
        ~LinkGuard() { JitCompiler::disable_jit_link(); }
    } link_guard;

    JitCompiler::enable_jit_link();

    const u8 code[] = {
        0xB8, 0x00, 0x20, 0x00, 0x00, // mov eax,0x2000
        0xFF, 0xE0,                   // jmp eax
    };
    BasicBlock block = build_block(code, sizeof(code));
    CompiledBlock* compiled = m_jit->compile(block);
    ASSERT_NE(compiled, nullptr);

    // Hot part then cold part, in buffer order: the publication is a miss
    // path, so under split placement it lives in the cold part.
    std::vector<u32> words;
    for (const WordRange& range : block_word_ranges(*compiled)) {
        words.insert(words.end(), range.begin, range.end);
    }
    const u32 br_x16 = 0xD61F0200u;
    const u32 publish_iters = arm64::str_w_uoff(
        REG_LOOP_CTR, REG_CPU_STATE, jit_iters_offset() >> 2);
    const u32 publish_rip = arm64::str_x_uoff(
        REG_SCRATCH0, REG_CPU_STATE, rip_offset() >> 3);
    const auto br = std::find(words.begin(), words.end(), br_x16);
    const auto publish = std::find(words.begin(), words.end(), publish_iters);
    const auto rip_publish = std::find(words.begin(), words.end(), publish_rip);
    ASSERT_NE(br, words.end());
    ASSERT_NE(publish, words.end());
    ASSERT_NE(rip_publish, words.end());
    EXPECT_GT(publish, br)
        << "jit_iters publication still executes before a successful link";
    EXPECT_GT(rip_publish, br)
        << "RIP publication still executes before a successful link";

    // With no resident target the lookup falls through to the cold publisher.
    m_cpu->rip = 0x1000;
    EXPECT_EQ(test::execute_like_dispatcher(*m_jit, *compiled, *m_cpu),
              JitExecResult::Continue);
    EXPECT_EQ(m_cpu->rip, 0x2000u);
    EXPECT_EQ(m_cpu->jit_iters, 1u)
        << "dynamic-link fallback did not publish the source block count";
}

TEST_F(JitExecTest, LinkedTailRetEntersTargetAfterNativePrologue) {
    // Intel SDM Vol. 2 RET: RIP := [ESP]; continue at that IP.
    // Linked tail must match a compiled block at the popped address.
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

    const u8 target_code[] = {
        0xB8, 0x2A, 0x00, 0x00, 0x00, // mov eax,42
    };
    auto target = decode_at(0x2000, target_code, sizeof(target_code));
    auto* target_compiled = m_jit->compile(target);
    ASSERT_NE(target_compiled, nullptr);
    cache.insert(std::move(target));
    auto* cached_target = cache.lookup(0x2000);
    ASSERT_NE(cached_target, nullptr);
    publish_compiled(*cached_target, *target_compiled);

    const u8 source_code[] = {0xC3};  // RET
    auto source = decode_at(0x1000, source_code, sizeof(source_code));
    auto* source_compiled = m_jit->compile(source);
    ASSERT_NE(source_compiled, nullptr);

    m_cpu->gpr.set32(ESP, 0xFFFC);
    ASSERT_TRUE(m_memory->write_u32(0xFFFC, 0x2000));
    m_cpu->rip = 0x1000;
    EXPECT_EQ(test::execute_like_dispatcher(*m_jit, *source_compiled, *m_cpu),
              JitExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get32(EAX), 42u);
    EXPECT_EQ(m_cpu->gpr.get32(ESP), 0x10000u);
    EXPECT_EQ(m_cpu->rip, 0x2005u);
}

TEST_F(JitExecTest, LinkedTailRetAfterCalleeSavedPopEpiloguePreservesStack) {
    // Spotify's media worker returns through this exact epilogue shape:
    //   mov ecx,[edi]; and dword [edi],0; mov [esi],ecx
    //   pop edi; pop esi; mov eax,ebx; pop ebx; pop ebp; ret
    // Intel SDM Vol. 2 POP/RET require each pop to advance ESP by four in
    // 32-bit mode and RET to continue at the final popped address.
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

    const u8 target_code[] = {
        0xBA, 0x78, 0x56, 0x34, 0x12, // mov edx,0x12345678
    };
    auto target = decode_at(0x2000, target_code, sizeof(target_code));
    auto* target_compiled = m_jit->compile(target);
    ASSERT_NE(target_compiled, nullptr);
    cache.insert(std::move(target));
    auto* cached_target = cache.lookup(0x2000);
    ASSERT_NE(cached_target, nullptr);
    publish_compiled(*cached_target, *target_compiled);

    const u8 source_code[] = {
        0x8B, 0x0F,       // mov ecx,[edi]
        0x83, 0x27, 0x00, // and dword ptr [edi],0
        0x89, 0x0E,       // mov [esi],ecx
        0x5F,             // pop edi
        0x5E,             // pop esi
        0x8B, 0xC3,       // mov eax,ebx
        0x5B,             // pop ebx
        0x5D,             // pop ebp
        0xC3,             // ret
    };
    auto source = decode_at(0x1000, source_code, sizeof(source_code));
    ASSERT_EQ(source.instruction_count, 9u);
    ASSERT_EQ(source.byte_length, sizeof(source_code));
    auto* source_compiled = m_jit->compile(source);
    ASSERT_NE(source_compiled, nullptr);

    constexpr GuestAddr kStack = 0xFFEC;
    constexpr u32 kSavedEdi = 0x31313131;
    constexpr u32 kSavedEsi = 0x42424242;
    constexpr u32 kSavedEbx = 0x53535353;
    constexpr u32 kSavedEbp = 0x64646464;
    ASSERT_TRUE(m_memory->write_u32(0x3000, 0xA1B2C3D4));
    ASSERT_TRUE(m_memory->write_u32(0x3010, 0));
    ASSERT_TRUE(m_memory->write_u32(kStack + 0, kSavedEdi));
    ASSERT_TRUE(m_memory->write_u32(kStack + 4, kSavedEsi));
    ASSERT_TRUE(m_memory->write_u32(kStack + 8, kSavedEbx));
    ASSERT_TRUE(m_memory->write_u32(kStack + 12, kSavedEbp));
    ASSERT_TRUE(m_memory->write_u32(kStack + 16, 0x2000));

    m_cpu->gpr.set32(EDI, 0x3000);
    m_cpu->gpr.set32(ESI, 0x3010);
    m_cpu->gpr.set32(EBX, 0x11223344);
    m_cpu->gpr.set32(EBP, 0x77777777);
    m_cpu->gpr.set32(ESP, kStack);
    m_cpu->rip = 0x1000;

    EXPECT_EQ(test::execute_like_dispatcher(*m_jit, *source_compiled, *m_cpu),
              JitExecResult::Continue);
    EXPECT_EQ(m_memory->read_u32(0x3000).value(), 0u);
    EXPECT_EQ(m_memory->read_u32(0x3010).value(), 0xA1B2C3D4u);
    EXPECT_EQ(m_cpu->gpr.get32(EAX), 0x11223344u);
    EXPECT_EQ(m_cpu->gpr.get32(EDX), 0x12345678u);
    EXPECT_EQ(m_cpu->gpr.get32(EDI), kSavedEdi);
    EXPECT_EQ(m_cpu->gpr.get32(ESI), kSavedEsi);
    EXPECT_EQ(m_cpu->gpr.get32(EBX), kSavedEbx);
    EXPECT_EQ(m_cpu->gpr.get32(EBP), kSavedEbp);
    EXPECT_EQ(m_cpu->gpr.get32(ESP), 0x10000u);
    EXPECT_EQ(m_cpu->rip, 0x2005u);
}
