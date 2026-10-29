// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// JIT execution tests — direct-link patching and its invalidation

#include "jit_exec_test_fixture.hpp"

TEST_F(JitExecTest, DirectPatchedTakenJccBypassesL1AndInvalidatesByGeneration) {
    struct LinkGuard {
        LinkGuard() { JitCompiler::enable_jit_link(); }
        ~LinkGuard() { JitCompiler::disable_jit_link(); }
    } link_guard;
    BlockCache cache;
    m_jit->set_block_cache(&cache);

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

    // CMP EAX,0 ; JNZ 0x2000 ; INC ECX.  The taken Jcc is a fixed forward
    // cold exit; INC ECX remains the in-block fall-through control.
    const u8 source_code[] = {
        0x83, 0xF8, 0x00,
        0x0F, 0x85, 0xF7, 0x0F, 0x00, 0x00,
        0x41,
    };
    auto source = decode_at(0x1000, source_code, sizeof(source_code));
    ASSERT_EQ(source.instruction_count, 3u);
    BasicBlock* cached_source = cache.insert(source);
    ASSERT_NE(cached_source, nullptr);
    CompiledBlock* source_compiled = m_jit->compile(*cached_source);
    ASSERT_NE(source_compiled, nullptr);
    CompiledBlock source_cb = *source_compiled;
    m_jit->publish_compiled_block(*cached_source, *source_compiled);

    // Before the target is published, the guarded link site is one atomic B
    // to the unchanged generic fallback.  Locate it from the generation-load
    // prefix so this test observes the exact word later patched in place.
    const u32 load_link_generation = arm64::ldr_w_uoff(
        17, REG_JIT_CTX, offsetof(JitContext, link_generation) >> 2);
    const u32 publish_rip = arm64::str_x_uoff(
        REG_SCRATCH0, REG_CPU_STATE, rip_offset() >> 3);
    const u32 load_l1 = arm64::ldr_x_uoff(
        16, REG_JIT_CTX, JIT_CTX_OFF_L1_BASE >> 3);
    // A block now carries one patch slot PER constant forward edge (the
    // taken edge AND the fall-through), so collect every slot instead of
    // assuming the first generation load belongs to the taken edge. Each
    // slot's fall-through lies in the same part of the block as the slot.
    std::vector<const u32*> patch_sites;
    std::vector<const u32*> patch_site_ends;
    for (const WordRange& range : block_word_ranges(source_cb)) {
        for (const u32* scan = range.begin;;) {
            const u32* generation_load =
                std::find(scan, range.end, load_link_generation);
            if (generation_load == range.end) break;
            if (range.end - generation_load >= 5) {
                EXPECT_EQ(generation_load[1], arm64::movz_w(3, 0));
                EXPECT_EQ(generation_load[2], arm64::eor_w(3, 3, 17));
                EXPECT_EQ(generation_load[3], arm64::cbnz_w(3, 8));
                EXPECT_EQ(generation_load[4] & 0xFC000000u, 0x14000000u);
                patch_sites.push_back(generation_load + 4);
                patch_site_ends.push_back(range.end);
            }
            scan = generation_load + 1;
        }
    }
    ASSERT_FALSE(patch_sites.empty());
    auto branch_target = [](const u32* site, u32 instruction) {
        const i32 imm26 = static_cast<i32>(instruction << 6) >> 6;
        return reinterpret_cast<uintptr_t>(site) +
               static_cast<intptr_t>(imm26) * 4;
    };
    // Before the target exists, every slot must hold a B to its own local
    // fallthrough, which precedes that edge's RIP publication and L1 probe.
    std::vector<u32> fallback_words;
    for (usize k = 0; k < patch_sites.size(); ++k) {
        const u32* const patch_site = patch_sites[k];
        const u32* const source_end = patch_site_ends[k];
        const u32 fallback_branch = *patch_site;
        ASSERT_EQ(fallback_branch & 0xFC000000u, 0x14000000u);
        fallback_words.push_back(fallback_branch);
        const uintptr_t fallback_target =
            branch_target(patch_site, fallback_branch);
        // The unpatched B must fall through locally, inside this block.
        // When this edge's tail is inline (the Jcc cold exit), the fall-
        // through additionally precedes that tail's RIP publication and L1
        // probe; an edge whose tail is a SHARED stub branches to a body
        // emitted earlier in the stream, so the positional check applies
        // only when a publication actually follows the site.
        EXPECT_GT(fallback_target, reinterpret_cast<uintptr_t>(patch_site));
        EXPECT_LT(fallback_target, reinterpret_cast<uintptr_t>(source_end));
        const u32* fallback_rip =
            std::find(patch_site + 1, source_end, publish_rip);
        const u32* fallback_l1 =
            std::find(patch_site + 1, source_end, load_l1);
        if (fallback_rip != source_end && fallback_l1 != source_end) {
            EXPECT_LE(fallback_target,
                      reinterpret_cast<uintptr_t>(fallback_rip));
            EXPECT_LT(fallback_target,
                      reinterpret_cast<uintptr_t>(fallback_l1));
        }
    }

    // Make the target page executable so rewriting it below drives the real
    // executable-write observer and advances the direct-link generation.
    ASSERT_TRUE(m_memory->protect_region(
        0x2000, 0x1000, MemoryProtection::ReadWriteExecute));
    // INC EBX ; JMP 0x3000 (unresident, so each linked run returns there).
    const u8 target_code[] = {0x43, 0xE9, 0xFA, 0x0F, 0x00, 0x00};
    auto target = decode_at(0x2000, target_code, sizeof(target_code));
    BasicBlock* cached_target = cache.insert(target);
    ASSERT_NE(cached_target, nullptr);
    CompiledBlock* target_compiled = m_jit->compile(*cached_target);
    ASSERT_NE(target_compiled, nullptr);
    const uintptr_t expected_link_entry =
        reinterpret_cast<uintptr_t>(target_compiled->link_entry());
    m_jit->publish_compiled_block(*cached_target, *target_compiled);

    // Target publication atomically rewrites the taken-edge slot (and only
    // slots whose recorded target is 0x2000) to the target's linked entry.
    const u32* patch_site = nullptr;
    for (usize i = 0; i < patch_sites.size(); ++i) {
        const u32 now = *patch_sites[i];
        if (now != fallback_words[i]) {
            ASSERT_EQ(now & 0xFC000000u, 0x14000000u);
            EXPECT_EQ(branch_target(patch_sites[i], now), expected_link_entry);
            patch_site = patch_sites[i];
        }
    }
    ASSERT_NE(patch_site, nullptr)
        << "no slot was patched by target publication";

    // Evict only the target's L1 entry with an exact-index alias.  The target
    // remains hash-resident, but a generic inline L1 tail cannot enter it.
    constexpr GuestAddr alias =
        BlockCache::l1_alias_after(0x2000, BlockCache::l1_capacity());
    static_assert(alias != 0);
    static_assert(BlockCache::l1_index(alias) ==
                  BlockCache::l1_index(0x2000));
    BasicBlock alias_block;
    alias_block.start_addr = alias;
    ASSERT_NE(cache.insert(std::move(alias_block)), nullptr);
    ASSERT_NE(cache.lookup(alias), nullptr);
    const u64 l1_before = cache.l1_hits();
    const u64 hash_before = cache.hash_hits();
    const u64 misses_before = cache.total_misses();

    for (u32 i = 0; i < 32; ++i) {
        m_cpu->init_protected_mode();
        m_cpu->gpr.set32(ESP, 0x10000);
        m_cpu->gpr.set32(EAX, 1);       // JNZ taken
        m_cpu->gpr.set32(EBX, i);
        m_cpu->gpr.set32(ECX, 0x55u);   // fall-through must not execute
        m_cpu->rip = 0x1000;
        EXPECT_EQ(test::execute_like_dispatcher(*m_jit, source_cb, *m_cpu),
                  JitExecResult::Continue);
        EXPECT_EQ(m_cpu->gpr.get32(EBX), i + 1);
        EXPECT_EQ(m_cpu->gpr.get32(ECX), 0x55u);
        EXPECT_EQ(m_cpu->rip, 0x3000u);
    }
    EXPECT_EQ(cache.l1_hits(), l1_before);
    EXPECT_EQ(cache.hash_hits(), hash_before);
    EXPECT_EQ(cache.total_misses(), misses_before);

    // Rewriting the target invalidates its cache entry and bumps the global
    // link generation.  The stale embedded host pointer must not be entered;
    // the source falls through to the generic publisher at guest RIP 0x2000.
    const u64 invalidations_before = cache.invalidations();
    ASSERT_TRUE(m_memory->write_u8(0x2000, 0x4B));  // DEC EBX in new bytes
    EXPECT_GT(cache.invalidations(), invalidations_before);

    m_cpu->init_protected_mode();
    m_cpu->gpr.set32(ESP, 0x10000);
    m_cpu->gpr.set32(EAX, 1);
    m_cpu->gpr.set32(EBX, 0x12345678u);
    m_cpu->rip = 0x1000;
    EXPECT_EQ(test::execute_like_dispatcher(*m_jit, source_cb, *m_cpu), JitExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get32(EBX), 0x12345678u)
        << "stale direct patch entered invalidated host code";
    EXPECT_EQ(m_cpu->rip, 0x2000u);
    EXPECT_EQ(cache.lookup(0x2000), nullptr);
    m_jit->set_block_cache(nullptr);
}

TEST_F(JitExecTest, EvictedDirectTargetWriteInvalidatesIncomingPatch) {
    struct LinkGuard {
        LinkGuard() { JitCompiler::enable_jit_link(); }
        ~LinkGuard() { JitCompiler::disable_jit_link(); }
    } link_guard;
    BlockCache cache(2);
    m_jit->set_block_cache(&cache);

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
        block.byte_length = static_cast<u32>(length);
        block.instruction_count =
            static_cast<u32>(block.instructions.size());
        return block;
    };

    const u8 source_code[] = {
        0x83, 0xF8, 0x00,                         // cmp eax,0
        0x0F, 0x85, 0xF7, 0x0F, 0x00, 0x00,       // jnz 0x2000
        0x41,                                     // inc ecx
    };
    auto source = decode_at(0x1000, source_code, sizeof(source_code));
    BasicBlock* cached_source = cache.insert(source);
    ASSERT_NE(cached_source, nullptr);
    CompiledBlock* source_compiled = m_jit->compile(*cached_source);
    ASSERT_NE(source_compiled, nullptr);
    CompiledBlock source_cb = *source_compiled;
    m_jit->publish_compiled_block(*cached_source, *source_compiled);

    const u32 generation_load = arm64::ldr_w_uoff(
        17, REG_JIT_CTX, offsetof(JitContext, link_generation) >> 2);
    std::vector<const u32*> patch_sites;
    std::vector<u32> fallback_words;
    for (const WordRange& range : block_word_ranges(source_cb)) {
        for (const u32* scan = range.begin;;) {
            const u32* generation =
                std::find(scan, range.end, generation_load);
            if (generation == range.end) break;
            ASSERT_GE(range.end - generation, 5);
            patch_sites.push_back(generation + 4);
            fallback_words.push_back(generation[4]);
            scan = generation + 1;
        }
    }
    ASSERT_FALSE(patch_sites.empty());

    const u8 target_code[] = {
        0x43,                                     // inc ebx
        0xE9, 0xFA, 0x0F, 0x00, 0x00,             // jmp 0x3000
    };
    auto target = decode_at(0x2000, target_code, sizeof(target_code));
    BasicBlock* cached_target = cache.insert(target);
    ASSERT_NE(cached_target, nullptr);
    CompiledBlock* target_compiled = m_jit->compile(*cached_target);
    ASSERT_NE(target_compiled, nullptr);
    const uintptr_t target_link =
        reinterpret_cast<uintptr_t>(target_compiled->link_entry());
    m_jit->publish_compiled_block(*cached_target, *target_compiled);

    auto branch_target = [](const u32* site, u32 instruction) {
        const i32 imm26 = static_cast<i32>(instruction << 6) >> 6;
        return reinterpret_cast<uintptr_t>(site) +
               static_cast<intptr_t>(imm26) * 4;
    };
    bool target_was_patched = false;
    for (const u32* site : patch_sites) {
        const u32 word = *site;
        target_was_patched |=
            (word & 0xFC000000u) == 0x14000000u &&
            branch_target(site, word) == target_link;
    }
    ASSERT_TRUE(target_was_patched);

    // Source becomes MRU, so inserting a third block into the two-entry cache
    // evicts the target while its incoming native branch still exists.
    ASSERT_NE(cache.insert(source), nullptr);
    BasicBlock eviction;
    eviction.start_addr = 0x4000;
    ASSERT_NE(cache.insert(std::move(eviction)), nullptr);
    ASSERT_EQ(cache.size(), 2u);
    ASSERT_EQ(cache.lookup(0x2000), nullptr);

    // The executable write must advance direct-link invalidation even though
    // the normal cached-page lookup can no longer find the evicted target.
    ASSERT_TRUE(m_memory->protect_region(
        0x2000, 0x1000, MemoryProtection::ReadWriteExecute));
    ASSERT_TRUE(m_memory->write_u8(0x2000, 0x4B)); // new guest bytes: dec ebx

    m_cpu->init_protected_mode();
    m_cpu->gpr.set32(ESP, 0x10000);
    m_cpu->gpr.set32(EAX, 1);
    m_cpu->gpr.set32(EBX, 7);
    m_cpu->gpr.set32(ECX, 0x55u);
    m_cpu->rip = 0x1000;
    EXPECT_EQ(test::execute_like_dispatcher(*m_jit, source_cb, *m_cpu), JitExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get32(EBX), 7u)
        << "stale evicted target code executed after its guest bytes changed";
    EXPECT_EQ(m_cpu->gpr.get32(ECX), 0x55u);
    EXPECT_EQ(m_cpu->rip, 0x2000u)
        << "incoming patch did not fall back after evicted-target write";
    m_jit->set_block_cache(nullptr);
}

// ---- Precise unlinking ------------------------------------------------------
//
// A guest code write fails closed only the patched edges whose TARGET
// translated the written bytes. It used to bump the process-wide link
// generation, which failed every patched edge in the process closed -- and
// those stayed on the L1 probe until their source or target was republished.
// A JS engine writes code all the time, so a renderer's never-rewritten
// browser-engine code ran nearly every hop through the probe.

namespace {

struct LinkScope {
    LinkScope() { JitCompiler::enable_jit_link(); }
    ~LinkScope() { JitCompiler::disable_jit_link(); }
};

BasicBlock decode_block32(GuestMemory& memory, GuestAddr address,
                          const u8* code, usize length) {
    EXPECT_TRUE(memory.write_bytes(address, code, length));
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
    block.instruction_count = static_cast<u32>(block.instructions.size());
    return block;
}

/// The generation word of every direct-link slot in `cb` whose B currently
/// reaches `entry`, i.e. every slot patched into that target.
std::vector<const u32*> slots_patched_into(const CompiledBlock& cb,
                                           uintptr_t entry) {
    const u32 load_gen = arm64::ldr_w_uoff(
        17, REG_JIT_CTX, offsetof(JitContext, link_generation) >> 2);
    std::vector<const u32*> out;
    for (const WordRange& range : block_word_ranges(cb)) {
        for (const u32* g = std::find(range.begin, range.end, load_gen);
             g != range.end; g = std::find(g + 1, range.end, load_gen)) {
            if (range.end - g < 5) continue;
            const u32 b = g[4];
            if ((b & 0xFC000000u) != 0x14000000u) continue;
            const i32 imm26 = static_cast<i32>(b << 6) >> 6;
            if (reinterpret_cast<uintptr_t>(g + 4) +
                    static_cast<intptr_t>(imm26) * 4 == entry) {
                out.push_back(g + 1);
            }
        }
    }
    return out;
}

// CMP EAX,0 ; JNZ 0x2000 ; INC ECX
constexpr u8 kSourceCode[] = {0x83, 0xF8, 0x00, 0x0F, 0x85, 0xF7,
                              0x0F, 0x00, 0x00, 0x41};
// INC EBX ; JMP 0x3000
constexpr u8 kTargetCode[] = {0x43, 0xE9, 0xFA, 0x0F, 0x00, 0x00};

}  // namespace

TEST_F(JitExecTest, UnrelatedCodeWriteKeepsDirectLinkPatched) {
    LinkScope link_scope;
    BlockCache cache;
    m_jit->set_block_cache(&cache);
    ASSERT_TRUE(m_memory->protect_region(0x1000, 0x5000,
                                         MemoryProtection::ReadWriteExecute));

    auto source = decode_block32(*m_memory, 0x1000, kSourceCode,
                                 sizeof(kSourceCode));
    BasicBlock* cached_source = cache.insert(source);
    ASSERT_NE(cached_source, nullptr);
    CompiledBlock* source_compiled = m_jit->compile(*cached_source);
    ASSERT_NE(source_compiled, nullptr);
    CompiledBlock source_cb = *source_compiled;
    m_jit->publish_compiled_block(*cached_source, *source_compiled);

    auto target = decode_block32(*m_memory, 0x2000, kTargetCode,
                                 sizeof(kTargetCode));
    BasicBlock* cached_target = cache.insert(target);
    ASSERT_NE(cached_target, nullptr);
    CompiledBlock* target_compiled = m_jit->compile(*cached_target);
    ASSERT_NE(target_compiled, nullptr);
    const uintptr_t entry =
        reinterpret_cast<uintptr_t>(target_compiled->link_entry());
    m_jit->publish_compiled_block(*cached_target, *target_compiled);

    // Unrelated code at 0x4000 (INC EDX ; JMP 0x3000), compiled and cached,
    // then rewritten: a JS engine replacing code next to the linked pair.
    const u8 other_code[] = {0x42, 0xE9, 0xFA, 0xEF, 0xFF, 0xFF};
    auto other = decode_block32(*m_memory, 0x4000, other_code,
                                sizeof(other_code));
    BasicBlock* cached_other = cache.insert(other);
    ASSERT_NE(cached_other, nullptr);
    CompiledBlock* other_compiled = m_jit->compile(*cached_other);
    ASSERT_NE(other_compiled, nullptr);
    m_jit->publish_compiled_block(*cached_other, *other_compiled);

    const std::vector<const u32*> patched = slots_patched_into(source_cb, entry);
    ASSERT_FALSE(patched.empty()) << "target publication patched no slot";
    const u32 gen_before = JitCompiler::link_generation_for_test();
    for (const u32* g : patched) {
        EXPECT_EQ(*g, arm64::movz_w(3, static_cast<u16>(gen_before)));
    }

    const u64 invalidations_before = cache.invalidations();
    ASSERT_TRUE(m_memory->write_u8(0x4000, 0x4A));  // DEC EDX
    ASSERT_GT(cache.invalidations(), invalidations_before)
        << "the unrelated write did not reach the JIT";

    EXPECT_EQ(JitCompiler::link_generation_for_test(), gen_before)
        << "an unrelated code write failed every direct edge closed";
    for (const u32* g : patched) {
        EXPECT_EQ(*g, arm64::movz_w(3, static_cast<u16>(gen_before)))
            << "an unrelated code write unpatched an edge";
    }

    // And the edge is really taken: the L1 is never consulted.
    const u64 l1_before = cache.l1_hits();
    const u64 misses_before = cache.total_misses();
    m_cpu->init_protected_mode();
    m_cpu->gpr.set32(ESP, 0x10000);
    m_cpu->gpr.set32(EAX, 1);
    m_cpu->gpr.set32(EBX, 41);
    m_cpu->gpr.set32(ECX, 0x55u);
    m_cpu->rip = 0x1000;
    EXPECT_EQ(test::execute_like_dispatcher(*m_jit, source_cb, *m_cpu),
              JitExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get32(EBX), 42u);
    EXPECT_EQ(m_cpu->gpr.get32(ECX), 0x55u);
    EXPECT_EQ(m_cpu->rip, 0x3000u);
    EXPECT_EQ(cache.l1_hits(), l1_before);
    EXPECT_EQ(cache.total_misses(), misses_before);

    // A write to the TARGET still fails exactly that edge closed.
    ASSERT_TRUE(m_memory->write_u8(0x2000, 0x4B));  // DEC EBX
    for (const u32* g : patched) EXPECT_EQ(*g, arm64::movz_w(3, 0));
    m_cpu->init_protected_mode();
    m_cpu->gpr.set32(ESP, 0x10000);
    m_cpu->gpr.set32(EAX, 1);
    m_cpu->gpr.set32(EBX, 7);
    m_cpu->rip = 0x1000;
    EXPECT_EQ(test::execute_like_dispatcher(*m_jit, source_cb, *m_cpu),
              JitExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get32(EBX), 7u)
        << "stale direct patch entered rewritten target code";
    EXPECT_EQ(m_cpu->rip, 0x2000u);
    m_jit->set_block_cache(nullptr);
}

// The writer's observer unlinks before GuestMemory publishes the write, so a
// thread that has not consumed the write yet can still hold the stale
// translation and patch an edge into it AFTER the unlink. That patch must
// fail its own site closed.
TEST_F(JitExecTest, PatchIntoTranslationUnlinkedSinceLastSyncFailsClosed) {
    LinkScope link_scope;
    BlockCache cache;
    m_jit->set_block_cache(&cache);
    ASSERT_TRUE(m_memory->protect_region(0x1000, 0x5000,
                                         MemoryProtection::ReadWriteExecute));

    auto source = decode_block32(*m_memory, 0x1000, kSourceCode,
                                 sizeof(kSourceCode));
    BasicBlock* cached_source = cache.insert(source);
    ASSERT_NE(cached_source, nullptr);
    CompiledBlock* source_compiled = m_jit->compile(*cached_source);
    ASSERT_NE(source_compiled, nullptr);
    CompiledBlock source_cb = *source_compiled;
    m_jit->publish_compiled_block(*cached_source, *source_compiled);

    auto target = decode_block32(*m_memory, 0x2000, kTargetCode,
                                 sizeof(kTargetCode));
    BasicBlock* cached_target = cache.insert(target);
    ASSERT_NE(cached_target, nullptr);
    CompiledBlock* target_compiled = m_jit->compile(*cached_target);
    ASSERT_NE(target_compiled, nullptr);
    const uintptr_t entry =
        reinterpret_cast<uintptr_t>(target_compiled->link_entry());

    // Another thread's observer for a write into the target's bytes: this
    // thread has not seen the event and still holds the translation.
    JitCompiler::unlink_direct_links_for_test(0x2003, 1);
    m_jit->publish_compiled_block(*cached_target, *target_compiled);

    const u32 load_gen = arm64::ldr_w_uoff(
        17, REG_JIT_CTX, offsetof(JitContext, link_generation) >> 2);
    usize slots = 0;
    for (const WordRange& range : block_word_ranges(source_cb)) {
        for (const u32* g = std::find(range.begin, range.end, load_gen);
             g != range.end; g = std::find(g + 1, range.end, load_gen)) {
            ++slots;
            EXPECT_EQ(g[1], arm64::movz_w(3, 0))
                << "an edge was linked into a translation unlinked since "
                   "this thread's last sync";
        }
    }
    EXPECT_GT(slots, 0u);

    // Once this thread has consumed the writes, the same publication links.
    ASSERT_TRUE(m_memory->write_u8(0x1FFF, 0x90));  // advance the write log
    (void)m_jit->synchronize_executable_writes();
    m_jit->publish_compiled_block(*cached_target, *target_compiled);
    EXPECT_FALSE(slots_patched_into(source_cb, entry).empty())
        << "a sync did not re-admit the edge";
    m_jit->set_block_cache(nullptr);
}
