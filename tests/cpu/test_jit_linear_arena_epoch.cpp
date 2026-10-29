// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
//
// Unpatching direct links into retired chunks and the epoch-based chunk free.
// Split from test_jit_linear.cpp; shared fixtures and matchers live in
// jit_linear_fixture.hpp.

#include "jit_linear_fixture.hpp"

using namespace vitreo;
using namespace vitreo::cpu;
using namespace vitreo::cpu::jit;
using namespace vitreo::cpu::jit::test;

namespace {

// A direct link patched from a block in one chunk into a block in another
// must be failed closed again when the target's chunk retires — before the
// bytes can be reused — without bumping the process-wide link generation.
TEST_F(JitLinearTest, IncomingDirectLinkIntoRetiredChunkIsUnpatched) {
    struct LinkGuard {
        LinkGuard() { JitCompiler::enable_jit_link(); }
        ~LinkGuard() { JitCompiler::disable_jit_link(); }
    } link_guard;

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
    const GuestAddr source_addr = kCode;
    const GuestAddr target_addr = kCode + 0x1000;
    // CMP EAX,0 ; JNZ target ; INC ECX
    const u8 source_code[] = {0x83, 0xF8, 0x00,
                              0x0F, 0x85, 0xF7, 0x0F, 0x00, 0x00,
                              0x41};
    // INC EBX ; JMP kCode+0x2000 (nothing cached there, so the run returns)
    const u8 target_code[] = {0x43, 0xE9, 0xFA, 0x0F, 0x00, 0x00};

    BasicBlock source = decode_at(source_addr, source_code, sizeof(source_code));
    ASSERT_EQ(source.instruction_count, 3u);
    BasicBlock* cached_source = m_cache->insert(source);
    CompiledBlock* source_compiled = m_jit->compile(*cached_source);
    ASSERT_NE(source_compiled, nullptr);
    const CompiledBlock source_cb = *source_compiled;
    m_jit->publish_compiled_block(*cached_source, *source_compiled);

    // Locate the patch slots from the generation-load prefix.
    const u32 load_link_generation = arm64::ldr_w_uoff(
        17, REG_JIT_CTX, offsetof(JitContext, link_generation) >> 2);
    std::vector<const u32*> gen_sites;
    for (const WordRange& range : block_word_ranges(source_cb)) {
        for (const u32* scan = range.begin;;) {
            const u32* load = std::find(scan, range.end, load_link_generation);
            if (load == range.end) break;
            if (range.end - load >= 5 && load[1] == arm64::movz_w(3, 0)) {
                gen_sites.push_back(load + 1);
            }
            scan = load + 1;
        }
    }
    ASSERT_FALSE(gen_sites.empty());

    // Target in a different chunk.
    m_jit->release_arena_chunk(/*seal=*/true);
    BasicBlock target = decode_at(target_addr, target_code, sizeof(target_code));
    BasicBlock* cached_target = m_cache->insert(target);
    CompiledBlock* target_compiled = m_jit->compile(*cached_target);
    ASSERT_NE(target_compiled, nullptr);
    const CompiledBlock target_cb = *target_compiled;
    ASSERT_NE(JitCodeArena::arena_chunk_index(source_cb.code),
              JitCodeArena::arena_chunk_index(target_cb.code));
    m_jit->publish_compiled_block(*cached_target, *target_compiled);

    // Exactly the taken-edge slot got patched: its MOVZ now carries the
    // live link generation.
    const u32* patched = nullptr;
    for (const u32* site : gen_sites) {
        if (*site != arm64::movz_w(3, 0)) {
            ASSERT_EQ(patched, nullptr) << "more than one slot patched";
            patched = site;
        }
    }
    ASSERT_NE(patched, nullptr) << "direct link was not patched";
    const u32 patched_word = *patched;
    const u32 link_generation_before = m_jit->link_generation_for_test();

    // Retire exactly the target's chunk (this compiler's current one).
    ASSERT_TRUE(JitCodeArena::retire_arena_chunk_containing(target_cb.code));
    ASSERT_FALSE(JitCodeArena::arena_code_live(target_cb.code,
                                              target_cb.arena_generation));
    ASSERT_TRUE(JitCodeArena::arena_code_live(source_cb.code,
                                             source_cb.arena_generation));
    EXPECT_EQ(*patched, arm64::movz_w(3, 0))
        << "incoming link into the retired chunk still armed";
    EXPECT_NE(*patched, patched_word);
    EXPECT_EQ(m_jit->link_generation_for_test(), link_generation_before)
        << "retirement fell back to the process-wide link generation bump";

    // Taking the edge from the source must not enter the retired chunk:
    // the guard fails closed to the generic tail, which publishes RIP.
    (void)m_jit->synchronize_arena_generation();
    CpuState cpu;
    init_cpu(cpu);
    cpu.gpr.set32(EAX, 1);          // JNZ taken
    cpu.gpr.set32(EBX, 0x100);
    BasicBlock* live_source = m_cache->lookup(source_addr);
    ASSERT_NE(live_source, nullptr);
    ASSERT_NE(live_source->compiled_code, nullptr);
    CompiledBlock run = compiled_from_cache(*live_source);
    EXPECT_EQ(test::execute_like_dispatcher(*m_jit, run, cpu), JitExecResult::Continue);
    EXPECT_EQ(cpu.rip, target_addr);
    EXPECT_EQ(cpu.gpr.get32(EBX), 0x100u) << "entered the retired target";
}

// A direct link whose target is out of B's +-128 MiB reach is patched through
// a veneer in the source chunk's far-branch island. Such a site used to stay
// unpatched for good -- its slot guard ran and then the L1 probe anyway, on
// every hop -- and in a game most CALL rel sites were such sites, because a
// call joins code compiled far apart in time. Retiring the target's chunk
// must still fail the site closed.
TEST_F(JitLinearTest, FarDirectLinkPatchesThroughTheSourceChunkIsland) {
    struct LinkGuard {
        LinkGuard() { JitCompiler::enable_jit_link(); }
        ~LinkGuard() { JitCompiler::disable_jit_link(); }
    } link_guard;

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
    const GuestAddr source_addr = kCode;
    const GuestAddr target_addr = kCode + 0x1000;
    // CMP EAX,0 ; JNZ target ; INC ECX
    const u8 source_code[] = {0x83, 0xF8, 0x00,
                              0x0F, 0x85, 0xF7, 0x0F, 0x00, 0x00,
                              0x41};
    // INC EBX ; JMP kCode+0x2000 (nothing cached there, so the run returns)
    const u8 target_code[] = {0x43, 0xE9, 0xFA, 0x0F, 0x00, 0x00};

    BasicBlock source = decode_at(source_addr, source_code, sizeof(source_code));
    BasicBlock* cached_source = m_cache->insert(source);
    CompiledBlock* source_compiled = m_jit->compile(*cached_source);
    ASSERT_NE(source_compiled, nullptr);
    const CompiledBlock source_cb = *source_compiled;
    m_jit->publish_compiled_block(*cached_source, *source_compiled);

    const u32 load_link_generation = arm64::ldr_w_uoff(
        17, REG_JIT_CTX, offsetof(JitContext, link_generation) >> 2);
    std::vector<const u32*> gen_sites;
    for (const WordRange& range : block_word_ranges(source_cb)) {
        for (const u32* scan = range.begin;;) {
            const u32* load = std::find(scan, range.end, load_link_generation);
            if (load == range.end) break;
            if (range.end - load >= 5 && load[1] == arm64::movz_w(3, 0)) {
                gen_sites.push_back(load + 1);
            }
            scan = load + 1;
        }
    }
    ASSERT_FALSE(gen_sites.empty());

    // Move this compiler's allocation past B's reach from the source: seal
    // its chunk and land a distinct filler block in the next one, until the
    // filler sits more than 128 MiB (plus a chunk of slack) away.
    const usize chunk_bytes = JitCodeArena::arena_capacity();
    const i64 far_enough = (i64{128} << 20) + static_cast<i64>(chunk_bytes);
    const auto distance = [&](const void* code) {
        return std::llabs(static_cast<long long>(
            reinterpret_cast<intptr_t>(code) -
            reinterpret_cast<intptr_t>(source_cb.code)));
    };
    bool placed = false;
    for (usize i = 0; i < JitCodeArena::arena_chunk_count() && !placed; ++i) {
        m_jit->release_arena_chunk(/*seal=*/true);
        // MOV EAX, imm32 ; RET, each at its own address (rewriting one
        // address trips the recompile-churn limit).
        const u32 imm = 0x51000000u + static_cast<u32>(i);
        const GuestAddr filler_addr = kCode + 0x3000 + (i % 0x80) * 0x20;
        const u8 filler_code[] = {0xB8, static_cast<u8>(imm),
                                  static_cast<u8>(imm >> 8),
                                  static_cast<u8>(imm >> 16),
                                  static_cast<u8>(imm >> 24), 0xC3};
        BasicBlock filler = decode_at(filler_addr, filler_code,
                                      sizeof(filler_code));
        CompiledBlock* filler_cb = m_jit->compile(filler);
        ASSERT_NE(filler_cb, nullptr) << "filler " << i;
        placed = distance(filler_cb->code) > far_enough;
    }
    if (!placed) GTEST_SKIP() << "the arena cannot hold two blocks 128 MiB apart";

    BasicBlock target = decode_at(target_addr, target_code, sizeof(target_code));
    BasicBlock* cached_target = m_cache->insert(target);
    CompiledBlock* target_compiled = m_jit->compile(*cached_target);
    ASSERT_NE(target_compiled, nullptr);
    const CompiledBlock target_cb = *target_compiled;
    ASSERT_GT(distance(target_cb.code), (i64{128} << 20));
    const u64 patches_before = stats::link_patches_performed();
    m_jit->publish_compiled_block(*cached_target, *target_compiled);
    EXPECT_EQ(stats::link_patches_performed() - patches_before, 1u);

    // The taken-edge slot is armed, and its B reaches the source chunk's
    // island, not the target.
    const u32* patched = nullptr;
    for (const u32* site : gen_sites) {
        if (*site != arm64::movz_w(3, 0)) {
            ASSERT_EQ(patched, nullptr) << "more than one slot patched";
            patched = site;
        }
    }
    ASSERT_NE(patched, nullptr) << "out-of-range direct link was not patched";
    const u32 branch = patched[3];  // MOVZ, EOR, CBNZ, then the patched B
    ASSERT_EQ(branch & 0xFC000000u, 0x14000000u) << "slot word is not a B";
    const auto* veneer = reinterpret_cast<const u8*>(patched + 3) +
                         static_cast<i64>(static_cast<i32>(branch << 6) >> 6) * 4;
    EXPECT_TRUE(JitCodeArena::in_far_branch_island(source_cb.code, veneer));

    // Taking the edge runs the target through the veneer.
    (void)m_jit->synchronize_arena_generation();
    CpuState cpu;
    init_cpu(cpu);
    cpu.gpr.set32(EAX, 1);          // JNZ taken
    cpu.gpr.set32(EBX, 0x100);
    CompiledBlock run = compiled_from_cache(*m_cache->lookup(source_addr));
    EXPECT_EQ(test::execute_like_dispatcher(*m_jit, run, cpu), JitExecResult::Continue);
    EXPECT_EQ(cpu.rip, kCode + 0x2000);
    EXPECT_EQ(cpu.gpr.get32(EBX), 0x101u) << "the target did not run";

    // Retiring the target's chunk fails the far site closed like a near one.
    ASSERT_TRUE(JitCodeArena::retire_arena_chunk_containing(target_cb.code));
    EXPECT_EQ(*patched, arm64::movz_w(3, 0))
        << "far incoming link into the retired chunk still armed";
    (void)m_jit->synchronize_arena_generation();
    init_cpu(cpu);
    cpu.gpr.set32(EAX, 1);
    cpu.gpr.set32(EBX, 0x100);
    BasicBlock* live_source = m_cache->lookup(source_addr);
    ASSERT_NE(live_source, nullptr);
    ASSERT_NE(live_source->compiled_code, nullptr);
    run = compiled_from_cache(*live_source);
    EXPECT_EQ(test::execute_like_dispatcher(*m_jit, run, cpu), JitExecResult::Continue);
    EXPECT_EQ(cpu.rip, target_addr);
    EXPECT_EQ(cpu.gpr.get32(EBX), 0x100u) << "entered the retired target";
}

// The opportunistic sweep is rate-limited to one scan per millisecond
// process-wide (constructor_destructor.cpp, kMinSweepIntervalNs), because it
// reads every occupancy line and runs at every cold dispatcher entry. A test
// that calls it several times in a row would otherwise get 0 from the rate
// limiter and read it as an epoch decision -- which makes a "the free must
// wait" assertion pass for the wrong reason. Wait past the interval so every
// sweep below is a real decision.
static usize sweep_after_rate_limit() {
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    return JitCodeArena::sweep_retired_chunks();
}

// A thread parked in a host call publishes no epoch, so a chunk retired
// while it is parked is recycled without waiting for it — and on return the
// thread must re-dispatch rather than continue into cached pointers.
TEST_F(JitLinearTest, ParkedHostCallDoesNotDelayChunkFreeAndRedispatches) {
    using Safepoint = JitCodeArena::HostCallSafepoint;
    std::vector<u8> code = {0xB8, 0x78, 0x56, 0x34, 0x12};
    ASSERT_TRUE(m_memory->write_bytes(kCode, code.data(), code.size()));
    BasicBlock block = build_block(code);
    CompiledBlock* compiled = m_jit->compile(block);
    ASSERT_NE(compiled, nullptr);
    const CompiledBlock cb = *compiled;

    std::atomic<bool> parked_ready{false};
    std::atomic<bool> host_call_done{false};
    std::atomic<Safepoint::Resume> resume{Safepoint::Resume::Continue};
    std::thread parked([&] {
        JitCodeArena::ArenaReadLease chain_lease{
            JitCodeArena::ArenaReadLease::StableExecution{}};
        ASSERT_TRUE(static_cast<bool>(chain_lease));
        Safepoint safepoint{&chain_lease};
        safepoint.park();
        parked_ready.store(true, std::memory_order_release);
        while (!host_call_done.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        resume.store(safepoint.unpark(), std::memory_order_release);
    });
    while (!parked_ready.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }

    const u64 frees_before = JitCodeArena::arena_chunk_frees();
    ASSERT_TRUE(JitCodeArena::retire_arena_chunk_containing(cb.code));
    ASSERT_FALSE(JitCodeArena::arena_code_live(cb.code, cb.arena_generation));
    // Nobody is inside: the sweep frees everything pending at once (this
    // chunk and any left behind by earlier tests), parked thread
    // notwithstanding, and no gate is involved.
    EXPECT_GE(sweep_after_rate_limit(), 1u);
    EXPECT_GE(JitCodeArena::arena_chunk_frees() - frees_before, 1u);
    EXPECT_EQ(JitCodeArena::arena_pending_free_count(), 0u);
    EXPECT_FALSE(JitCodeArena::arena_reclaim_pending());

    host_call_done.store(true, std::memory_order_release);
    parked.join();
    EXPECT_EQ(resume.load(std::memory_order_acquire),
              Safepoint::Resume::Redispatch);
}


// A thread inside native code at the retirement epoch delays the free —
// nothing else does — and the free happens as soon as it publishes a later
// epoch (which every dispatcher entry does) or leaves.
TEST_F(JitLinearTest, ThreadInsideAtRetirementDelaysFreeUntilLaterEpoch) {
    std::vector<u8> code = {0xB8, 0x78, 0x56, 0x34, 0x12};
    ASSERT_TRUE(m_memory->write_bytes(kCode, code.data(), code.size()));
    BasicBlock block = build_block(code);
    CompiledBlock* compiled = m_jit->compile(block);
    ASSERT_NE(compiled, nullptr);
    const CompiledBlock cb = *compiled;

    enum class Step { Hold, Refresh, Release, Done };
    std::atomic<Step> step{Step::Hold};
    std::atomic<bool> inside{false};
    std::atomic<bool> refreshed{false};
    std::thread runner([&] {
        JitCodeArena::ArenaReadLease chain{
            JitCodeArena::ArenaReadLease::StableExecution{}};
        ASSERT_TRUE(static_cast<bool>(chain));
        inside.store(true, std::memory_order_release);
        while (step.load(std::memory_order_acquire) == Step::Hold) {
            std::this_thread::yield();
        }
        // What execute() does at every dispatcher entry.
        JitCodeArena::ArenaReadLease::refresh_epoch();
        refreshed.store(true, std::memory_order_release);
        while (step.load(std::memory_order_acquire) != Step::Done) {
            std::this_thread::yield();
        }
    });
    while (!inside.load(std::memory_order_acquire)) std::this_thread::yield();

    // Start from a clean pending set so the counts below are exact.
    (void)sweep_after_rate_limit();
    const u64 frees_before = JitCodeArena::arena_chunk_frees();
    const usize pending_before = JitCodeArena::arena_pending_free_count();
    ASSERT_TRUE(JitCodeArena::retire_arena_chunk_containing(cb.code));
    ASSERT_FALSE(JitCodeArena::arena_code_live(cb.code, cb.arena_generation));
    const usize retired = 1;
    ASSERT_EQ(JitCodeArena::arena_pending_free_count(), pending_before + 1);

    // Runner entered before the retirement: the free must wait for it.
    EXPECT_EQ(sweep_after_rate_limit(), 0u);
    EXPECT_EQ(JitCodeArena::arena_pending_free_count(), pending_before + 1);
    EXPECT_EQ(JitCodeArena::arena_chunk_frees(), frees_before);
    EXPECT_FALSE(JitCodeArena::arena_reclaim_pending())
        << "a pending free closed the gate";

    // It publishes a later epoch without leaving its chain: free proceeds.
    step.store(Step::Refresh, std::memory_order_release);
    while (!refreshed.load(std::memory_order_acquire)) std::this_thread::yield();
    EXPECT_GE(sweep_after_rate_limit(), retired);
    EXPECT_EQ(JitCodeArena::arena_pending_free_count(), 0u);
    EXPECT_GE(JitCodeArena::arena_chunk_frees() - frees_before, retired);

    step.store(Step::Done, std::memory_order_release);
    runner.join();
}

}  // namespace
