// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// JIT execution tests — shared link probe and RSB push/RET probe

#include "jit_exec_test_fixture.hpp"
#include <deque>
#include <numeric>
#include <random>

class JitL1WayTest : public JitExecTest {
protected:
    void TearDown() override {
        m_jit->set_block_cache(nullptr);
        JitCompiler::disable_jit_link();
        JitCompiler::set_shared_exit_stubs_for_test(-1);
    }

    CompiledBlock* install(GuestAddr address, const std::vector<u8>& code, bool x64,
                           bool write_code = true) {
        if (write_code)
            EXPECT_TRUE(m_memory->write_bytes(address, code.data(), code.size()));
        BasicBlock block;
        block.start_addr = address;
        block.is_64bit = x64;
        MemoryByteReader reader(code.data(), code.size());
        while (reader.remaining()) {
            DecodedInstruction instruction;
            if (decode_instruction(reader, x64 ? DecodeMode::Mode64 : DecodeMode::Mode32,
                                   instruction) != DecodeError::Success) break;
            block.instructions.push_back(instruction);
        }
        block.byte_length = static_cast<u32>(reader.position());
        block.instruction_count = static_cast<u32>(block.instructions.size());
        BasicBlock* cached = cache.insert(std::move(block));
        auto* compiled = m_jit->compile(*cached);
        EXPECT_NE(compiled, nullptr);
        if (!compiled) return nullptr;
        // Publication may discard the compiler's fully-native record. Keep
        // an owned copy so subsequent compilations cannot invalidate entry.
        records.push_back(*compiled);
        m_jit->publish_compiled_block(*cached, *compiled);
        return &records.back();
    }

    void displace_to_way1(GuestAddr address) {
        if (cache.l1_base()[BlockCache::l1_index(address) + 1].tag == address) return;
        const GuestAddr alias = BlockCache::l1_alias_after(address, BlockCache::l1_capacity());
        ASSERT_NE(alias, address);
        // Re-inserting an already resident alias need not refill L1. Make
        // this a fresh collision after a target lookup or replacement.
        cache.invalidate(alias);
        BasicBlock block;
        block.start_addr = alias;
        block.byte_length = 1;
        ASSERT_NE(cache.insert(std::move(block)), nullptr);
        EXPECT_EQ(cache.l1_base()[BlockCache::l1_index(address) + 1].tag, address);
    }

    void check_probe(const CompiledBlock& block, bool shared, u32 kind) {
        const u32 word = shared ? arm64::ldr_x_uoff(16, REG_JIT_CTX,
            (static_cast<u32>(offsetof(JitContext, link_probe)) >> 3) + kind)
            : arm64::ldp_x_off(3, 16, 17, 0);
        bool found = false;
        for (const auto& range : block_word_ranges(block))
            found |= std::find(range.begin, range.end, word) != range.end;
        EXPECT_TRUE(found) << "requested probe configuration was not emitted";
    }

    CompiledBlock* prepare(bool x64, bool shared, bool way1, bool colliding = false) {
        m_jit->set_block_cache(nullptr);
        cache.clear();
        records.clear();
        m_jit = std::make_unique<JitCompiler>(*m_memory);
        m_jit->set_block_cache(&cache);
        JitCompiler::enable_jit_link();
        JitCompiler::set_shared_exit_stubs_for_test(shared ? 1 : 0);
        if (x64) m_cpu->init_long_mode();
        else m_cpu->init_protected_mode();
        const GuestAddr target_address = colliding
            ? BlockCache::l1_alias_after(0x1000, 0x2000) : 0x2000;
        m_cpu->gpr.r[EBX] = target_address;
        m_cpu->gpr.r[EDX] = 0x1000;
        // JMP r/m32 and JMP r/m64 change no flags (Intel SDM JMP).
        auto* target = install(target_address, {0xFF, 0xE2}, x64); // jmp edx/rdx
        auto* entry = install(0x1000, {0xFF, 0xE3}, x64);  // jmp ebx/rbx
        EXPECT_NE(target, nullptr);
        if (entry) check_probe(*entry, shared, 1);
        if (way1) {
            displace_to_way1(0x1000);
            displace_to_way1(0x2000);
        }
        return entry;
    }

    BlockCache cache;
    std::deque<CompiledBlock> records;
};

TEST_F(JitL1WayTest, ConstantCallAndReturnReachWay1) {
    for (bool x64 : {false, true}) for (bool shared : {false, true}) {
        for (bool is_ret : {false, true}) {
            SCOPED_TRACE(::testing::Message() << "x64=" << x64
                         << " shared=" << shared << " ret=" << is_ret);
            ASSERT_NE(prepare(x64, shared, false), nullptr);
            m_cpu->gpr.r[EDX] = 0x5000;
            m_cpu->gpr.r[ESP] = 0x10000;
            m_cpu->gpr.r[ECX] = 0;
            ASSERT_NE(install(0x4000, {0xB9, 42, 0, 0, 0, 0xFF, 0xE2}, x64), nullptr);
            auto* entry = install(0x3000, is_ret ? std::vector<u8>{0xC3}
                : std::vector<u8>{0xE8, 0xFB, 0x0F, 0, 0}, x64);
            ASSERT_NE(entry, nullptr);
            check_probe(*entry, shared, is_ret ? (x64 ? 1 : 2) : 0);
            displace_to_way1(0x4000);
            ASSERT_TRUE(m_memory->write_u64(0x10000, 0x4000));
            m_cpu->rip = 0x3000;
            EXPECT_EQ(test::execute_like_dispatcher(*m_jit, *entry, *m_cpu),
                      JitExecResult::Continue);
            EXPECT_EQ(m_cpu->rip, 0x5000u);
            EXPECT_EQ(m_cpu->gpr.r[ECX], 42u);
            const u64 width = x64 ? 8 : 4;
            EXPECT_EQ(m_cpu->gpr.r[ESP], is_ret ? 0x10000 + width : 0x10000 - width);
        }
    }
}

TEST_F(JitL1WayTest, DispatcherHitPromotesWholeEntry) {
    const char* value = std::getenv("VITREO_JIT_L1_DISPATCH_PROMOTE");
    if (value != nullptr && value[0] == '0' && value[1] == '\0')
        GTEST_SKIP() << "dispatcher promotion disabled";
    for (bool x64 : {false, true}) for (u32 kind : {0u, 1u, 2u}) {
        ASSERT_NE(prepare(x64, false, true), nullptr);
        if (kind == 2) {
            ASSERT_NE(cache.lookup(0x1000), nullptr);
            displace_to_way1(0x1000);
        } else {
            ASSERT_NE(cache.lookup(0x2000), nullptr);
        }
        const usize set = BlockCache::l1_index(0x1000);
        const auto older = cache.l1_base()[set + 1];
        const auto newer = cache.l1_base()[set];
        ASSERT_EQ(older.tag, 0x1000u);
        BasicBlock* block = kind != 0 ? cache.lookup_fast(0x1000) : cache.lookup(0x1000);
        ASSERT_EQ(block, older.block);
        const auto check = [](const BlockCache::L1Entry& actual,
                              const BlockCache::L1Entry& expected) {
            EXPECT_EQ(actual.tag, expected.tag);
            EXPECT_EQ(actual.link_code, expected.link_code);
            EXPECT_EQ(actual.pin_layout, expected.pin_layout);
            EXPECT_EQ(actual.block, expected.block);
        };
        check(cache.l1_base()[set], older);
        check(cache.l1_base()[set + 1], newer);
        cache.invalidate(0x1000);
        EXPECT_EQ(cache.l1_base()[set].link_code, nullptr);
        EXPECT_EQ(cache.l1_base()[set + 1].tag, newer.tag);
    }
}

TEST_F(JitL1WayTest, HitsMissesNullAndInvalidationBothModes) {
    for (bool x64 : {false, true}) for (bool shared : {false, true}) {
        for (bool way1 : {false, true}) {
            SCOPED_TRACE(::testing::Message() << "x64=" << x64
                         << " shared=" << shared << " way1=" << way1);
            auto* entry = prepare(x64, shared, way1);
            ASSERT_NE(entry, nullptr);
            m_cpu->flags.rflags |= STATUS_FLAGS_MASK;
            const u64 flags = m_cpu->flags.rflags;
            m_cpu->rip = 0x1000;
            EXPECT_EQ(test::execute_like_dispatcher(*m_jit, *entry, *m_cpu),
                      JitExecResult::Continue);
            EXPECT_EQ(m_cpu->jit_iters, 4096u) << "hit chain must reach the hop cap";
            EXPECT_EQ(m_cpu->flags.rflags, flags);

            // A matching tag with unpublished code must fail closed.
            BasicBlock* target = cache.lookup(0x2000);
            ASSERT_NE(target, nullptr);
            target->compiled_link_code = nullptr;
            cache.refresh_l1(target);
            if (way1) displace_to_way1(0x2000);
            m_cpu->rip = 0x1000;
            EXPECT_EQ(test::execute_like_dispatcher(*m_jit, *entry, *m_cpu),
                      JitExecResult::Continue);
            EXPECT_EQ(m_cpu->rip, 0x2000u);
            EXPECT_EQ(m_cpu->jit_iters, 1u);

            // Invalidation clears the stale cached pointer and tag.
            ASSERT_TRUE(cache.invalidate(0x2000));
            m_cpu->rip = 0x1000;
            EXPECT_EQ(test::execute_like_dispatcher(*m_jit, *entry, *m_cpu),
                      JitExecResult::Continue);
            EXPECT_EQ(m_cpu->rip, 0x2000u);
            EXPECT_EQ(m_cpu->jit_iters, 1u);

            // Replacing the target publishes its new body in either way.
            ASSERT_NE(install(0x2000, {0xB9, 42, 0, 0, 0, 0xFF, 0xE2}, x64), nullptr);
            if (way1) displace_to_way1(0x2000);
            m_cpu->gpr.r[ECX] = 0;
            m_cpu->rip = 0x1000;
            EXPECT_EQ(test::execute_like_dispatcher(*m_jit, *entry, *m_cpu),
                      JitExecResult::Continue);
            EXPECT_EQ(m_cpu->gpr.r[ECX], 42u);
            EXPECT_EQ(m_cpu->jit_iters, 4096u);

            m_jit->set_block_cache(nullptr);
            m_cpu->rip = 0x1000;
            EXPECT_EQ(test::execute_like_dispatcher(*m_jit, *entry, *m_cpu),
                      JitExecResult::Continue);
            EXPECT_EQ(m_cpu->rip, 0x2000u);
            EXPECT_EQ(m_cpu->jit_iters, 1u);
        }
    }
}

TEST_F(JitL1WayTest, Way1ProbePreservesPayloadFlagsAndVectors) {
    for (bool x64 : {false, true}) for (bool shared : {false, true}) {
        SCOPED_TRACE(::testing::Message() << "x64=" << x64 << " shared=" << shared);
        auto* entry = prepare(x64, shared, true);
        ASSERT_NE(entry, nullptr);
        const auto first = cache.l1_base()[BlockCache::l1_index(0x1000)];
        const auto second = cache.l1_base()[BlockCache::l1_index(0x2000)];
        const auto target1 = cache.l1_base()[BlockCache::l1_index(0x1000) + 1];
        const auto target2 = cache.l1_base()[BlockCache::l1_index(0x2000) + 1];
        for (u32 reg = 0; reg < 16; ++reg)
            for (u32 lane = 0; lane < 4; ++lane)
                m_cpu->sse.xmm[reg].u32_[lane] = 0x10203040u + reg * 16 + lane;
        m_cpu->flags.rflags |= STATUS_FLAGS_MASK;
        const u64 flags = m_cpu->flags.rflags;
        m_cpu->rip = 0x1000;
        ASSERT_EQ(test::execute_like_dispatcher(*m_jit, *entry, *m_cpu),
                  JitExecResult::Continue);
        EXPECT_EQ(m_cpu->jit_iters, 4096u);
        EXPECT_EQ(m_cpu->flags.rflags, flags);
        for (u32 reg = 0; reg < 16; ++reg)
            for (u32 lane = 0; lane < 4; ++lane)
                EXPECT_EQ(m_cpu->sse.xmm[reg].u32_[lane],
                          0x10203040u + reg * 16 + lane);
        const auto check = [](const BlockCache::L1Entry& actual,
                              const BlockCache::L1Entry& expected) {
            EXPECT_EQ(actual.tag, expected.tag);
            EXPECT_EQ(actual.link_code, expected.link_code);
            EXPECT_EQ(actual.pin_layout, expected.pin_layout);
            EXPECT_EQ(actual.block, expected.block);
        };
        check(cache.l1_base()[BlockCache::l1_index(0x1000)], first);
        check(cache.l1_base()[BlockCache::l1_index(0x1000) + 1], target1);
        check(cache.l1_base()[BlockCache::l1_index(0x2000)], second);
        check(cache.l1_base()[BlockCache::l1_index(0x2000) + 1], target2);
    }
}

TEST_F(JitL1WayTest, DISABLED_WideLinkProbeBenchmark) {
    const char* shared_env = std::getenv("VITREO_BENCH_SHARED_PROBE");
    const bool shared = shared_env && shared_env[0] == '1';
    ASSERT_NE(prepare(true, shared, false), nullptr);
    m_jit->set_block_cache(nullptr);
    cache.clear();
    records.clear();
    m_jit->set_block_cache(&cache);
    constexpr u32 nodes = 4096;
    constexpr GuestAddr base = 0x100000;
    constexpr u32 stride = 32;
    ASSERT_TRUE(m_memory->map_region(base, nodes * stride, MemoryProtection::ReadWrite));
    const std::vector<u8> code{0x48, 0x8B, 0x40, 8, 0xFF, 0xE0};
    std::vector<u32> order(nodes);
    std::iota(order.begin(), order.end(), 0);
    std::mt19937 random(0x5137);
    std::shuffle(order.begin() + 1, order.end(), random);
    for (u32 i = 0; i < nodes; ++i) {
        const GuestAddr address = base + order[i] * stride;
        ASSERT_TRUE(m_memory->write_bytes(address, code.data(), code.size()));
        ASSERT_TRUE(m_memory->write_u64(address + 8,
                                       base + order[(i + 1) % nodes] * stride));
    }
    CompiledBlock* entry = nullptr;
    u64 native_bytes = 0;
    for (u32 i = 0; i < nodes; ++i) {
        auto* compiled = install(base + i * stride, code, true, false);
        ASSERT_NE(compiled, nullptr);
        native_bytes += compiled->code_size + compiled->cold_size;
        if (i == 0) entry = compiled;
    }
    const GuestAddr expected = base + order[4095 % nodes] * stride;
    const u64 flags = m_cpu->flags.rflags;
    bool valid = true;
    const auto execute = [&] {
        m_cpu->rip = base;
        m_cpu->gpr.r[EAX] = base;
        valid &= m_jit->execute(*entry, *m_cpu) == JitExecResult::Continue;
        valid &= m_cpu->jit_iters == 4096;
        valid &= m_cpu->rip == expected && m_cpu->gpr.r[EAX] == expected;
        valid &= m_cpu->flags.rflags == flags;
    };
    for (u32 i = 0; i < 1024; ++i) execute();
    constexpr u32 executions = 10000;
    const auto start = std::chrono::steady_clock::now();
    for (u32 i = 0; i < executions; ++i) execute();
    const double seconds = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - start).count();
    EXPECT_TRUE(valid);
    constexpr u64 probes = u64{executions} * 4095;
    std::printf("WIDE_LINK_BENCH seconds=%.9f probes=%llu ns_per_probe=%.3f "
                "nodes=%u native_bytes=%llu\n", seconds,
                static_cast<unsigned long long>(probes), seconds * 1e9 / probes,
                nodes, static_cast<unsigned long long>(native_bytes));
}

TEST_F(JitL1WayTest, DISABLED_Way1ProbeBenchmark) {
    const char* shared_env = std::getenv("VITREO_BENCH_SHARED_PROBE");
    const bool shared = shared_env && shared_env[0] == '1';
    const char* way_env = std::getenv("VITREO_BENCH_WAY");
    const bool way1 = !way_env || way_env[0] != '0';
    const bool null_code = way_env && way_env[0] == 'n';
    const bool colliding = way_env && way_env[0] == 'c';
    auto* entry = prepare(true, shared, way1 && !colliding, colliding);
    ASSERT_NE(entry, nullptr);
    const GuestAddr target_address = m_cpu->gpr.r[EBX];
    if (null_code) {
        BasicBlock* target = cache.lookup(0x2000);
        ASSERT_NE(target, nullptr);
        target->compiled_link_code = nullptr;
        cache.refresh_l1(target);
    }
    if (std::getenv("VITREO_BENCH_DISPATCH_LOOKUP") != nullptr) {
        ASSERT_NE(cache.lookup(0x1000), nullptr);
        ASSERT_NE(cache.lookup(target_address), nullptr);
    }
    const u32 executions = null_code ? 5000000 : 100000;
    bool valid = true;
    const auto start = std::chrono::steady_clock::now();
    for (u32 i = 0; i < executions; ++i) {
        m_cpu->rip = 0x1000;
        valid &= m_jit->execute(*entry, *m_cpu) == JitExecResult::Continue;
        valid &= m_cpu->jit_iters == (null_code ? 1u : 4096u);
    }
    const double seconds = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - start).count();
    EXPECT_TRUE(valid);
    EXPECT_EQ(m_cpu->rip, target_address);
    EXPECT_EQ(m_cpu->gpr.r[EBX], target_address);
    EXPECT_EQ(m_cpu->gpr.r[EDX], 0x1000u);
    for (GuestAddr address : {GuestAddr{0x1000}, target_address}) {
        const usize set = BlockCache::l1_index(address);
        EXPECT_TRUE(cache.l1_base()[set].tag == address ||
                    cache.l1_base()[set + 1].tag == address);
    }
    const u64 probes = u64{executions} * (null_code ? 1 : 4096 - 1);
    std::printf("WAY1_BENCH seconds=%.9f probes=%llu ns_per_probe=%.3f entry_bytes=%u\n",
                seconds, static_cast<unsigned long long>(probes), seconds * 1e9 / probes,
                entry->code_size + entry->cold_size);
}

// The process-wide link probes (JitContext::link_probe) replace each block's
// L1 / sticky / RSB probe copy. A block keeps only the entry -- LDR the
// probe, BLR, CBZ X16 to its miss continuation, BR X16 -- and a miss still
// publishes RIP and jit_iters exactly as the per-block copy did, in both
// bitnesses.
TEST_F(JitExecTest, SharedLinkProbeMissPublishesRipAndIters) {
    struct Guard {
        Guard() {
            JitCompiler::set_shared_exit_stubs_for_test(1);
            JitCompiler::enable_jit_link();
        }
        ~Guard() {
            JitCompiler::disable_jit_link();
            JitCompiler::set_shared_exit_stubs_for_test(-1);
        }
    } guard;
    using namespace vitreo::cpu::jit;
    const u32 inline_l1_ldp = arm64::ldp_x_off(3, 16, 17, 0);
    const u32 br_x16 = 0xD61F0200u;
    const u32 blr_x16 = 0xD63F0200u;
    struct Form {
        const char* name;
        bool x64;
        std::vector<u8> code;
        u64 target;
        u32 probe;  // JitCompiler::LinkProbe
    };
    const Form forms[] = {
        // CALL rel32 carries no direct-link slot by default, so its tail
        // probes L1 without a budget (forward edge).
        {"call rel32", false, {0xE8, 0xFB, 0x0F, 0x00, 0x00}, 0x2000, 0},
        {"backward jmp", false, {0x90, 0xEB, 0xFD}, 0x1000, 1},
        {"jmp eax", false, {0xB8, 0x00, 0x20, 0x00, 0x00, 0xFF, 0xE0}, 0x2000, 1},
        {"ret", false, {0xC3}, 0x3456, 2},
        {"x64 jmp rax", true,
         {0x48, 0xC7, 0xC0, 0x00, 0x20, 0x00, 0x00, 0xFF, 0xE0}, 0x2000, 1},
        // x64 RET probes L1 alone (ops64.cpp RET_NEAR), always budgeted.
        {"x64 ret", true, {0xC3}, 0x3456, 1},
    };
    for (const Form& form : forms) {
        SCOPED_TRACE(form.name);
        // Each form recompiles 0x1000; a fresh compiler keeps that under the
        // rewrite limit.
        m_jit = std::make_unique<JitCompiler>(*m_memory);
        BasicBlock block = form.x64 ? build_block64(form.code.data(), form.code.size())
                                    : build_block(form.code.data(), form.code.size());
        // The per-block emission of the same tail: the shared entry must
        // stand exactly where it carried its own probe.
        JitCompiler::set_shared_exit_stubs_for_test(0);
        CompiledBlock* legacy = m_jit->compile(block);
        JitCompiler::set_shared_exit_stubs_for_test(1);
        ASSERT_NE(legacy, nullptr);
        bool legacy_probe = false;
        for (const WordRange& range : block_word_ranges(*legacy)) {
            legacy_probe |= std::count(range.begin, range.end, inline_l1_ldp) != 0;
        }
        CompiledBlock* compiled = m_jit->compile(block);
        ASSERT_NE(compiled, nullptr);
        const u32 load_probe = arm64::ldr_x_uoff(
            16, REG_JIT_CTX,
            (static_cast<u32>(offsetof(JitContext, link_probe)) >> 3) + form.probe);
        bool entry = false;
        for (const WordRange& range : block_word_ranges(*compiled)) {
            EXPECT_EQ(std::count(range.begin, range.end, inline_l1_ldp), 0)
                << "the block still carries its own L1 probe";
            for (const u32* w = range.begin; w + 3 < range.end; ++w) {
                if (w[0] == load_probe) {
                    EXPECT_EQ(w[1], blr_x16);
                    EXPECT_EQ(w[2], arm64::cbz_x(16, 8));
                    EXPECT_EQ(w[3], br_x16);
                    entry = true;
                }
            }
        }
        EXPECT_EQ(entry, legacy_probe) << "shared probe entry";

        m_cpu->rip = 0x1000;
        m_cpu->jit_iters = 0;
        m_cpu->gpr.r[static_cast<u8>(RegIndex::RSP)] = 0x10000;
        ASSERT_TRUE(m_memory->write_u64(0x10000, 0x3456));
        EXPECT_EQ(test::execute_like_dispatcher(*m_jit, *compiled, *m_cpu),
                  JitExecResult::Continue);
        EXPECT_EQ(m_cpu->rip, form.target);
        EXPECT_EQ(m_cpu->jit_iters, 1u);
    }
}

// A CALL / RET round trip through the shared RSB push and RET probe: the
// CALL's push records the return block's link, the RET hops to it without
// returning to the dispatcher, and the chain leaves with the guest state the
// interpreter would produce.
TEST_F(JitExecTest, SharedRsbPushAndRetProbeLinkACallReturn) {
    struct Guard {
        Guard() {
            JitCompiler::set_shared_exit_stubs_for_test(1);
            JitCompiler::enable_jit_link();
        }
        ~Guard() {
            JitCompiler::disable_jit_link();
            JitCompiler::set_shared_exit_stubs_for_test(-1);
        }
    } guard;
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
        block.instruction_count = static_cast<u32>(block.instructions.size());
        return block;
    };
    auto install = [&](GuestAddr address, const u8* code, usize length) {
        BasicBlock block = decode_at(address, code, length);
        CompiledBlock* compiled = m_jit->compile(block);
        EXPECT_NE(compiled, nullptr);
        cache.insert(std::move(block));
        BasicBlock* cached = cache.lookup(address);
        EXPECT_NE(cached, nullptr);
        if (cached != nullptr && compiled != nullptr) {
            publish_compiled(*cached, *compiled);
        }
        return compiled;
    };
    // 0x2000: inc eax ; ret
    const u8 callee[] = {0x40, 0xC3};
    // 0x1005: add eax, 0x10 ; jmp 0x3000 (never compiled: the chain leaves)
    const u8 continuation[] = {0x83, 0xC0, 0x10, 0xE9, 0xF3, 0x1F, 0x00, 0x00};
    ASSERT_NE(install(0x2000, callee, sizeof(callee)), nullptr);
    ASSERT_NE(install(0x1005, continuation, sizeof(continuation)), nullptr);
    // 0x1000: call 0x2000 -- compiled after its successors so the RSB push
    // finds the continuation's link.
    const u8 caller[] = {0xE8, 0xFB, 0x0F, 0x00, 0x00};
    CompiledBlock* entry = install(0x1000, caller, sizeof(caller));
    ASSERT_NE(entry, nullptr);

    m_cpu->gpr.set32(EAX, 1);
    m_cpu->gpr.set32(ESP, 0x10000);
    m_cpu->rip = 0x1000;
    EXPECT_EQ(test::execute_like_dispatcher(*m_jit, *entry, *m_cpu),
              JitExecResult::Continue);
    EXPECT_EQ(m_cpu->rip, 0x3000u);
    EXPECT_EQ(m_cpu->gpr.get32(EAX), 0x12u);
    EXPECT_EQ(m_cpu->gpr.get32(ESP), 0x10000u);
    EXPECT_EQ(m_memory->read_u32(0xFFFC).value_or(0), 0x1005u);
    // W22 starts at 1 and counts the RET hop (a RET is always budgeted); the
    // forward CALL and JMP edges are not counted.
    EXPECT_EQ(m_cpu->jit_iters, 2u) << "the RET did not hop through the probe";
    m_jit->set_block_cache(nullptr);
}
