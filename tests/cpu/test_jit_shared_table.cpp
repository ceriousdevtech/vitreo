// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
//
// The process-wide compiled-block table (JitCompileMemo::s_compiled_index) is the
// primary "is this guest RIP compiled?" store; each thread's BlockCache is an
// L1 over it. These tests execute the SAME compiled block from two threads
// through two BlockCaches — the coverage the memo tests never had — and pin
// the invalidation contract: a stale record fails closed on adoption, an
// executable writes purge it for every thread, repeated writes demote it, exit
// leaves shared code intact, and arena retirement removes the records it
// recycled.

#include <gtest/gtest.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <shared_mutex>
#include <thread>
#include <unordered_set>
#include <vector>
#include "core/cpu/jit/jit_compiler.hpp"
#include "jit_dispatch_execute.hpp"
#include "core/cpu/jit/block_cache.hpp"
#include "core/cpu/decode/decoder.hpp"

using namespace vitreo;
using namespace vitreo::cpu;
using namespace vitreo::cpu::jit;

namespace {

constexpr u8 EAX = static_cast<u8>(RegIndex::RAX);
constexpr u8 EBX = static_cast<u8>(RegIndex::RBX);
constexpr u8 ESP = static_cast<u8>(RegIndex::RSP);

constexpr GuestAddr kNativeAddr = 0x1000;
constexpr GuestAddr kDelegateAddr = 0x1100;
constexpr GuestAddr kStoreAddr = 0x2000;

// mov eax, 0x12345678 ; add eax, ebx ; mov [0x2000], eax
constexpr u8 kNativeCode[] = {0xB8, 0x78, 0x56, 0x34, 0x12,
                              0x01, 0xD8,
                              0xA3, 0x00, 0x20, 0x00, 0x00};
// cpuid — interpreter-delegate stencil, bakes &instructions[i]
constexpr u8 kDelegateCode[] = {0x0F, 0xA2};
// mov eax, 0xDEADBEEF — same length as the first native instruction
constexpr u8 kPatchedCode[] = {0xB8, 0xEF, 0xBE, 0xAD, 0xDE};
// adc eax, ebx ; mov [0x2000], eax — a carry consumer at a block start, where
// the carry's kind is unknown: it calls the process-wide carry-capture stub
// through JitContext::capture_cf_stub.
constexpr GuestAddr kAdcAddr = 0x1200;
constexpr u8 kAdcCode[] = {0x11, 0xD8, 0xA3, 0x00, 0x20, 0x00, 0x00};

struct MemoGuard {
    MemoGuard() {
        JitCompileMemo::clear_compile_memo();
        JitCompileMemo::reset_compile_memo_stats();
        JitCompileMemo::enable_compile_memo();
        JitCompiler::set_shared_adoption_for_test(1);
    }
    ~MemoGuard() {
        JitCompiler::set_shared_adoption_for_test(-1);
        JitCompileMemo::disable_compile_memo();
        JitCompileMemo::clear_compile_memo();
    }
};

class JitSharedTableTest : public ::testing::Test {
protected:
    void SetUp() override {
        m_memory = std::make_unique<GuestMemory>();
        ASSERT_TRUE(m_memory->map_region(0x1000, 0x10000,
                                         MemoryProtection::ReadWrite));
        write(kNativeAddr, kNativeCode, sizeof(kNativeCode));
        write(kDelegateAddr, kDelegateCode, sizeof(kDelegateCode));
    }

    void TearDown() override {
        // Records retain pages of this address space; the teardown hook
        // forgets them, but be explicit so a failing test cannot leak into
        // the next one.
        JitCompileMemo::clear_compile_memo();
    }

    void write(GuestAddr addr, const u8* code, usize len) {
        ASSERT_TRUE(m_memory->write_bytes(addr, code, len));
    }

    BasicBlock decode_at(GuestAddr addr, const u8* code, usize len) {
        BasicBlock block;
        block.start_addr = addr;
        MemoryByteReader reader(code, len);
        while (reader.remaining() > 0) {
            DecodedInstruction inst;
            if (decode_instruction(reader, DecodeMode::Mode32, inst) !=
                DecodeError::Success) {
                break;
            }
            block.instructions.push_back(inst);
        }
        block.byte_length = static_cast<u32>(reader.position());
        block.instruction_count = static_cast<u32>(block.instructions.size());
        return block;
    }

    static void init_cpu(CpuState& cpu, GuestAddr rip) {
        cpu.init_protected_mode();
        cpu.gpr.set32(ESP, 0x10000);
        cpu.gpr.set32(EBX, 1);
        cpu.rip = rip;
    }

    struct Outcome {
        void* code{nullptr};
        bool ok{false};
        u32 eax{0};
        u32 ebx{0};
        u32 stored{0};
        usize decoded_len{0};
        bool released{false};
    };

    // Compile `code` at `addr` through its own cache+compiler (as a guest
    // thread does), publish it into the cache entry, and execute the entry
    // exactly as the dispatcher would (via compiled_from_cache).
    Outcome compile_and_run(GuestAddr addr, const u8* code, usize len) {
        Outcome out;
        BlockCache cache(64);
        JitCompiler jit(*m_memory);
        jit.set_block_cache(&cache);
        BasicBlock* entry = cache.insert(decode_at(addr, code, len));
        CompiledBlock* compiled = jit.compile(*entry);
        if (compiled == nullptr) return out;
        jit.publish_compiled_block(*entry, *compiled);
        out.code = entry->compiled_code;
        out.ok = run_entry(jit, *entry, out);
        return out;
    }

    // Adopt the shared record at `addr` into a fresh cache+compiler pair,
    // without decoding or compiling, and execute it.
    Outcome adopt_and_run(GuestAddr addr) {
        Outcome out;
        BlockCache cache(64);
        JitCompiler jit(*m_memory);
        jit.set_block_cache(&cache);
        BasicBlock adopted;
        if (!jit.try_adopt_shared_block(addr, false, adopted)) return out;
        BasicBlock* entry = cache.insert(std::move(adopted));
        jit.link_adopted_block(*entry);
        out.code = entry->compiled_code;
        out.decoded_len = entry->instructions.size();
        out.released = entry->instructions_released;
        out.ok = run_entry(jit, *entry, out);
        return out;
    }

    bool run_entry(JitCompiler& jit, BasicBlock& entry, Outcome& out) {
        if (entry.compiled_code == nullptr) return false;
        CpuState cpu;
        init_cpu(cpu, entry.start_addr);
        (void)m_memory->write_u32(kStoreAddr, 0);
        CompiledBlock cb = compiled_from_cache(entry);
        if (test::execute_like_dispatcher(jit, cb, cpu) != JitExecResult::Continue) return false;
        out.eax = cpu.gpr.get32(EAX);
        out.ebx = cpu.gpr.get32(EBX);
        out.stored = m_memory->read_u32(kStoreAddr).value_or(0xEEEEEEEE);
        return true;
    }

    std::unique_ptr<GuestMemory> m_memory;
};

template <typename F>
void on_thread(F&& fn) {
    std::thread t(std::forward<F>(fn));
    t.join();
}

}  // namespace

// (a) Thread B reaches a block thread A already compiled: B adopts A's arena
// code at its cold miss — no decode, no emit, no arena growth — and computes
// the same result. Covers both block classes: fully native (no decode is
// even needed) and delegate-bearing (the record's decoded array is shared).
TEST_F(JitSharedTableTest, SecondThreadAdoptsWithoutEmitting) {
    MemoGuard memo;

    Outcome a_native, a_delegate;
    on_thread([&] {
        a_native = compile_and_run(kNativeAddr, kNativeCode,
                                   sizeof(kNativeCode));
        a_delegate = compile_and_run(kDelegateAddr, kDelegateCode,
                                     sizeof(kDelegateCode));
    });
    ASSERT_TRUE(a_native.ok);
    ASSERT_TRUE(a_delegate.ok);
    EXPECT_EQ(a_native.eax, 0x12345679u);
    EXPECT_EQ(a_native.stored, 0x12345679u);
    EXPECT_NE(a_delegate.ebx, 1u) << "CPUID delegate did not run";

    const auto before = JitCompileMemo::compile_memo_stats();
    ASSERT_EQ(before.records, 2u);
    ASSERT_GE(before.fresh_emits, 2u);
    ASSERT_EQ(before.cold_adoptions, 0u);
    const usize arena_before = JitCodeArena::arena_bytes_used();

    Outcome b_native, b_delegate;
    on_thread([&] {
        b_native = adopt_and_run(kNativeAddr);
        b_delegate = adopt_and_run(kDelegateAddr);
    });
    ASSERT_TRUE(b_native.ok) << "thread B did not adopt the native block";
    ASSERT_TRUE(b_delegate.ok) << "thread B did not adopt the delegate block";

    // Same arena bytes, same results.
    EXPECT_EQ(b_native.code, a_native.code);
    EXPECT_EQ(b_delegate.code, a_delegate.code);
    EXPECT_EQ(b_native.eax, a_native.eax);
    EXPECT_EQ(b_native.stored, a_native.stored);
    EXPECT_EQ(b_delegate.ebx, a_delegate.ebx);

    // Nothing was emitted or decoded on B's behalf.
    const auto after = JitCompileMemo::compile_memo_stats();
    EXPECT_EQ(after.fresh_emits, before.fresh_emits);
    EXPECT_EQ(after.cold_adoptions, 2u);
    EXPECT_EQ(after.cold_hash_mismatches, 0u);
    EXPECT_EQ(after.compile_probes, before.compile_probes)
        << "adoption must not go through compile()";
    EXPECT_EQ(JitCodeArena::arena_bytes_used(), arena_before);
    // A fully native record carries no decode and B never made one; the
    // delegate record's array is adopted as-is.
    EXPECT_TRUE(b_native.released);
    EXPECT_EQ(b_native.decoded_len, 0u);
    EXPECT_EQ(b_delegate.decoded_len, 1u);
    EXPECT_FALSE(b_delegate.released);
}

// (a2) Adopted code reaches the process-wide stubs through the ADOPTING
// thread's JitContext. Thread B never compiles anything, so before the
// registry (JitCompiler::register_live_jit_context) its capture_cf_stub slot
// stayed null on this x86, non-linear64 model -- execute()'s sync covered
// linear64 only -- and A's ADC block ran BLR X16 = 0: SIGSEGV with PC = 0,
// X16 = 0 and LR in JIT code, the signature SteamService.exe /RunAsService
// showed on its ThreadEntry thread. Thread C's compiler exists BEFORE A
// compiles (a context that must be filled by the publisher, not at birth).
TEST_F(JitSharedTableTest, AdoptedBlockFindsProcessWideStubsInItsOwnContext) {
    MemoGuard memo;
    write(kAdcAddr, kAdcCode, sizeof(kAdcCode));

    BlockCache cache_c(64);
    std::unique_ptr<JitCompiler> jit_c;
    on_thread([&] {
        jit_c = std::make_unique<JitCompiler>(*m_memory);
        jit_c->set_block_cache(&cache_c);
    });

    Outcome a;
    on_thread([&] { a = compile_and_run(kAdcAddr, kAdcCode, sizeof(kAdcCode)); });
    ASSERT_TRUE(a.ok);
    EXPECT_EQ(a.eax, 1u) << "0 + EBX(1) + CF(0)";
    EXPECT_EQ(a.stored, 1u);

    Outcome b;
    on_thread([&] { b = adopt_and_run(kAdcAddr); });
    ASSERT_TRUE(b.ok) << "thread B did not adopt the ADC block";
    EXPECT_EQ(b.code, a.code) << "B must run A's arena bytes";
    EXPECT_EQ(b.eax, a.eax);
    EXPECT_EQ(b.stored, a.stored);

    Outcome c;
    on_thread([&] {
        BasicBlock adopted;
        ASSERT_TRUE(jit_c->try_adopt_shared_block(kAdcAddr, false, adopted));
        BasicBlock* entry = cache_c.insert(std::move(adopted));
        jit_c->link_adopted_block(*entry);
        c.code = entry->compiled_code;
        c.ok = run_entry(*jit_c, *entry, c);
        jit_c.reset();
    });
    ASSERT_TRUE(c.ok);
    EXPECT_EQ(c.code, a.code);
    EXPECT_EQ(c.eax, a.eax);
}

// (b) Cross-thread SMC. The guest bytes behind a published record change:
// (1) before any observer has seen the write, a probing thread must refuse
//     the record (code_hash fails closed);
// (2) once the write is observed, every stale record is purged. The patched
//     code can be compiled again without permanently falling to interpretation.
TEST_F(JitSharedTableTest, CrossThreadSmcFailsClosedThenRecompiles) {
    MemoGuard memo;

    // Thread A: the compiling thread. Its cache/compiler live on for the whole
    // test because they are what the executable-write observer is bound to.
    BlockCache cache_a(64);
    JitCompiler jit_a(*m_memory);
    jit_a.set_block_cache(&cache_a);
    BasicBlock* entry_a =
        cache_a.insert(decode_at(kNativeAddr, kNativeCode, sizeof(kNativeCode)));
    CompiledBlock* compiled_a = jit_a.compile(*entry_a);
    ASSERT_NE(compiled_a, nullptr);
    jit_a.publish_compiled_block(*entry_a, *compiled_a);
    ASSERT_NE(entry_a->compiled_code, nullptr);
    ASSERT_EQ(JitCompileMemo::compile_memo_size(), 1u);

    // Thread C adopts the block while it is still valid, then keeps its cache
    // around to prove foreign-thread convergence later.
    BlockCache cache_c(64);
    std::unique_ptr<JitCompiler> jit_c;
    bool c_adopted = false;
    std::thread thread_c([&] {
        jit_c = std::make_unique<JitCompiler>(*m_memory);
        jit_c->set_block_cache(&cache_c);
        BasicBlock adopted;
        c_adopted = jit_c->try_adopt_shared_block(kNativeAddr, false, adopted);
        if (c_adopted) cache_c.insert(std::move(adopted));
    });
    thread_c.join();
    ASSERT_TRUE(c_adopted);
    ASSERT_NE(cache_c.lookup(kNativeAddr), nullptr);

    // (1) Rewrite the first instruction. The mapping is RW, not executable,
    // so no observer fires: the table still holds the record.
    write(kNativeAddr, kPatchedCode, sizeof(kPatchedCode));
    ASSERT_EQ(JitCompileMemo::compile_memo_size(), 1u);
    Outcome stale;
    on_thread([&] { stale = adopt_and_run(kNativeAddr); });
    EXPECT_FALSE(stale.ok) << "a record whose bytes changed was adopted";
    EXPECT_EQ(JitCompileMemo::compile_memo_stats().cold_hash_mismatches, 1u);
    // compile() on the new bytes must not be served the old code either.
    on_thread([&] {
        Outcome fresh = compile_and_run(kNativeAddr, kPatchedCode,
                                        sizeof(kPatchedCode));
        EXPECT_TRUE(fresh.ok);
        EXPECT_NE(fresh.code, entry_a->compiled_code);
        EXPECT_EQ(fresh.eax, 0xDEADBEEFu);
    });
    EXPECT_EQ(JitCompileMemo::compile_memo_size(), 2u)
        << "old and new byte identities are distinct records";

    // (2) Now the write is observed on thread A (the writer). Every record
    // overlapping the written range goes, and A's own
    // entry is invalidated immediately.
    m_memory->notify_executable_write(kNativeAddr, sizeof(kPatchedCode));
    EXPECT_EQ(JitCompileMemo::compile_memo_size(), 0u);
    EXPECT_FALSE(JitCompileMemo::shared_smc_demoted(kNativeAddr));
    EXPECT_EQ(JitCompileMemo::compile_memo_stats().smc_purges, 2u);
    EXPECT_EQ(cache_a.lookup(kNativeAddr), nullptr);

    // Thread C converges at its next sync point.
    std::thread thread_c2([&] {
        EXPECT_TRUE(jit_c->synchronize_executable_writes());
        EXPECT_EQ(cache_c.lookup(kNativeAddr), nullptr);
        // The old record is gone, but the new bytes remain compilable.
        BasicBlock adopted;
        EXPECT_FALSE(jit_c->try_adopt_shared_block(kNativeAddr, false,
                                                   adopted));
        BasicBlock block = decode_at(kNativeAddr, kPatchedCode,
                                     sizeof(kPatchedCode));
        ASSERT_NE(jit_c->compile(block), nullptr);
        jit_c.reset();
    });
    thread_c2.join();
    EXPECT_NE(jit_a.compile(decode_at(kNativeAddr, kPatchedCode,
                                      sizeof(kPatchedCode))),
              nullptr);
    on_thread([&] {
        const Outcome patched = adopt_and_run(kNativeAddr);
        EXPECT_TRUE(patched.ok);
        EXPECT_EQ(patched.eax, 0xDEADBEEFu);
    });
}

TEST_F(JitSharedTableTest, LocalWriteDoesNotSkipPendingForeignCodePatch) {
    MemoGuard memo;
    BlockCache cache(64);
    JitCompiler jit(*m_memory);
    jit.set_block_cache(&cache);
    auto* entry = cache.insert(decode_at(kNativeAddr, kNativeCode,
                                         sizeof(kNativeCode)));
    auto* compiled = jit.compile(*entry);
    ASSERT_NE(compiled, nullptr);
    jit.publish_compiled_block(*entry, *compiled);

    // Another guest thread patches an embedded constant. The consuming
    // thread has not reached its next invalidation-log synchronization yet.
    std::thread writer([&] {
        write(kNativeAddr, kPatchedCode, sizeof(kPatchedCode));
        m_memory->notify_executable_write(kNativeAddr, sizeof(kPatchedCode));
    });
    writer.join();
    ASSERT_NE(cache.lookup(kNativeAddr), nullptr);

    // A local notification on an unrelated data page must not acknowledge
    // the foreign patch just because it observes the newer global counter.
    m_memory->notify_executable_write(kStoreAddr, sizeof(u32));
    EXPECT_TRUE(jit.synchronize_executable_writes());
    EXPECT_EQ(cache.lookup(kNativeAddr), nullptr);

    const auto patched = compile_and_run(kNativeAddr, kPatchedCode,
                                         sizeof(kPatchedCode));
    EXPECT_TRUE(patched.ok);
    EXPECT_EQ(patched.eax, 0xDEADBEEFu);
}

TEST_F(JitSharedTableTest, RepeatedCrossThreadRewritesStillDemote) {
    MemoGuard memo;
    write(kNativeAddr, kPatchedCode, sizeof(kPatchedCode));
    EXPECT_TRUE(compile_and_run(kNativeAddr, kPatchedCode,
                                sizeof(kPatchedCode)).ok);
    for (u32 rewrite = 1; rewrite <= 8; ++rewrite) {
        SCOPED_TRACE(rewrite);
        EXPECT_GT(JitCompileMemo::purge_shared_records_overlapping(
                     kNativeAddr, sizeof(kPatchedCode), rewrite), 0u);
        u8 changed[sizeof(kPatchedCode)];
        std::memcpy(changed, kPatchedCode, sizeof(changed));
        const u32 value = 0xDEADBEEFu + rewrite;
        std::memcpy(changed + 1, &value, sizeof(value));
        write(kNativeAddr, changed, sizeof(changed));
        // Replaying the same invalidation on another thread is not a rewrite.
        on_thread([&] {
            EXPECT_EQ(JitCompileMemo::purge_shared_records_overlapping(
                         kNativeAddr, sizeof(changed), rewrite), 0u);
            const Outcome fresh = compile_and_run(kNativeAddr, changed, sizeof(changed));
            EXPECT_EQ(fresh.ok, rewrite < 8);
            if (fresh.ok) EXPECT_EQ(fresh.eax, value);
        });
        if (rewrite < 8) {
            for (u32 observer = 0; observer < 12; ++observer) {
                on_thread([&] {
                    EXPECT_GT(JitCompileMemo::purge_shared_records_overlapping(
                        kNativeAddr, sizeof(changed), rewrite), 0u);
                    EXPECT_TRUE(compile_and_run(kNativeAddr, changed,
                                               sizeof(changed)).ok);
                });
            }
        }
        EXPECT_EQ(JitCompileMemo::shared_smc_demoted(kNativeAddr), rewrite == 8);
    }
    on_thread([&] {
        EXPECT_FALSE(compile_and_run(kNativeAddr, kPatchedCode,
                                   sizeof(kPatchedCode)).ok);
        EXPECT_FALSE(adopt_and_run(kNativeAddr).ok);
    });
}

TEST_F(JitSharedTableTest, LocalRewriteLimitSurvivesMissingSharedRecords) {
    MemoGuard memo;
    BlockCache cache(64);
    JitCompiler jit(*m_memory);
    jit.set_block_cache(&cache);
    for (u32 compilation = 0; compilation <= 8; ++compilation) {
        SCOPED_TRACE(compilation);
        BasicBlock block = decode_at(kNativeAddr, kNativeCode, sizeof(kNativeCode));
        CompiledBlock* compiled = jit.compile(block);
        if (compilation == 8) {
            EXPECT_EQ(compiled, nullptr);
            break;
        }
        ASSERT_NE(compiled, nullptr);
        jit.publish_compiled_block(block, *compiled);
        cache.insert(block);
        JitCompileMemo::clear_compile_memo();
        m_memory->notify_executable_write(kNativeAddr, sizeof(kNativeCode));
        EXPECT_EQ(cache.lookup(kNativeAddr), nullptr);
    }
}

TEST_F(JitSharedTableTest, RestoredExecutableBytesRemainCompilable) {
    MemoGuard memo;
    for (bool wide : {false, true}) {
        SCOPED_TRACE(wide);
        JitCompileMemo::clear_compile_memo();
        BlockCache cache(64);
        JitCompiler jit(*m_memory);
        jit.set_block_cache(&cache);
        write(kNativeAddr, kPatchedCode, sizeof(kPatchedCode));
        for (unsigned restoration = 0; restoration < 32; ++restoration) {
            SCOPED_TRACE(restoration);
            auto block = decode_at(kNativeAddr, kPatchedCode, sizeof(kPatchedCode));
            block.is_64bit = wide; // MOV EAX,imm32 has the same encoding.
            auto* compiled = jit.compile(block);
            ASSERT_NE(compiled, nullptr);
            jit.publish_compiled_block(block, *compiled);
            auto* entry = cache.insert(std::move(block));
            auto executable = compiled_from_cache(*entry);
            CpuState cpu;
            if (wide) cpu.init_long_mode(); else cpu.init_protected_mode();
            cpu.rip = kNativeAddr;
            ASSERT_EQ(test::execute_like_dispatcher(jit, executable, cpu),
                      JitExecResult::Continue);
            EXPECT_EQ(cpu.gpr.get64(EAX), 0xDEADBEEFu);
            // A code producer can replace bytes then restore the original
            // version before executing it again. Both writes invalidate, but
            // they must not classify one stable executable version as churn.
            write(kNativeAddr, kNativeCode, sizeof(kPatchedCode));
            m_memory->notify_executable_write(kNativeAddr, sizeof(kPatchedCode));
            write(kNativeAddr, kPatchedCode, sizeof(kPatchedCode));
            m_memory->notify_executable_write(kNativeAddr, sizeof(kPatchedCode));
            EXPECT_EQ(cache.lookup(kNativeAddr), nullptr);
        }
    }
}

TEST_F(JitSharedTableTest, CacheEvictionsAfterOneRewriteDoNotDemote) {
    MemoGuard memo;
    BlockCache cache(64);
    JitCompiler jit(*m_memory);
    jit.set_block_cache(&cache);
    for (u32 compilation = 0; compilation < 32; ++compilation) {
        SCOPED_TRACE(compilation);
        BasicBlock block = decode_at(kNativeAddr, kNativeCode, sizeof(kNativeCode));
        CompiledBlock* compiled = jit.compile(block);
        ASSERT_NE(compiled, nullptr);
        jit.publish_compiled_block(block, *compiled);
        cache.insert(block);
        JitCompileMemo::clear_compile_memo();
        if (compilation == 0) {
            m_memory->notify_executable_write(kNativeAddr, sizeof(kNativeCode));
        } else {
            cache.clear();
        }
    }
}

// (c) The compiling thread exits — its BlockCache and JitCompiler are gone —
// and the shared translation must remain executable for everyone else: the
// arena bytes, the delegate record's decoded array, and the guest code-page
// retain now belong to the record, not to any thread.
TEST_F(JitSharedTableTest, SharedCodeOutlivesCompilingThread) {
    MemoGuard memo;

    Outcome a_native, a_delegate;
    on_thread([&] {
        a_native = compile_and_run(kNativeAddr, kNativeCode,
                                   sizeof(kNativeCode));
        a_delegate = compile_and_run(kDelegateAddr, kDelegateCode,
                                     sizeof(kDelegateCode));
        // Thread A's cache and compiler are destroyed here.
    });
    ASSERT_TRUE(a_native.ok);
    ASSERT_TRUE(a_delegate.ok);
    EXPECT_EQ(JitCompileMemo::compile_memo_size(), 2u);
    // The page is still marked as holding code with no cache alive.
    EXPECT_TRUE(m_memory->jit_code_pages_overlap(kNativeAddr, 1));

    Outcome b_native, b_delegate;
    on_thread([&] {
        b_native = adopt_and_run(kNativeAddr);
        b_delegate = adopt_and_run(kDelegateAddr);
    });
    ASSERT_TRUE(b_native.ok);
    ASSERT_TRUE(b_delegate.ok);
    EXPECT_EQ(b_native.code, a_native.code);
    EXPECT_EQ(b_native.eax, a_native.eax);
    EXPECT_EQ(b_native.stored, a_native.stored);
    EXPECT_EQ(b_delegate.code, a_delegate.code);
    EXPECT_EQ(b_delegate.ebx, a_delegate.ebx);
    EXPECT_EQ(b_delegate.decoded_len, 1u);

    // Dropping the records releases the retain they held.
    JitCompileMemo::clear_compile_memo();
    EXPECT_FALSE(m_memory->jit_code_pages_overlap(kNativeAddr, 1));
}

// Destroying the address space that owns every record empties the shared
// table through the GuestMemory teardown hook. Process exit takes this path;
// it used to erase record by record, which is quadratic in blocks per page.
TEST_F(JitSharedTableTest, DestroyingTheOnlyAddressSpaceEmptiesTheTable) {
    MemoGuard memo;
    ASSERT_TRUE(compile_and_run(kNativeAddr, kNativeCode, sizeof(kNativeCode)).ok);
    ASSERT_GE(JitCompileMemo::compile_memo_size(), 1u);
    m_memory.reset();
    EXPECT_EQ(JitCompileMemo::compile_memo_size(), 0u);
}

// (d) Arena-horizon retirement: once the slot a record was emitted into is
// recycled, the record is gone from the table (and its page retain with it),
// so no thread can adopt a pointer into recycled bytes.
TEST_F(JitSharedTableTest, ArenaRetirementRemovesSharedRecords) {
    MemoGuard memo;

    Outcome a = compile_and_run(kNativeAddr, kNativeCode, sizeof(kNativeCode));
    ASSERT_TRUE(a.ok);
    ASSERT_EQ(JitCompileMemo::compile_memo_size(), 1u);
    // Adoptable before retirement.
    {
        BasicBlock probe;
        JitCompiler jit(*m_memory);
        EXPECT_TRUE(jit.try_adopt_shared_block(kNativeAddr, false, probe));
    }

    const u64 published_generation = JitCodeArena::arena_generation();
    JitCodeArena::request_arena_reclaim();
    const usize max_advances =
        JitCodeArena::arena_budget() / JitCodeArena::arena_capacity() + 4;
    usize advances = 0;
    ASSERT_TRUE(JitCodeArena::reclaim_arena_if_requested());
    while (JitCodeArena::arena_min_generation() <= published_generation &&
           advances < max_advances) {
        JitCodeArena::request_arena_reclaim();
        ASSERT_TRUE(JitCodeArena::reclaim_arena_if_requested());
        ++advances;
    }
    ASSERT_GT(JitCodeArena::arena_min_generation(), published_generation)
        << "oldest slot never retired within the arena budget";

    EXPECT_EQ(JitCompileMemo::compile_memo_size(), 0u);
    EXPECT_FALSE(m_memory->jit_code_pages_overlap(kNativeAddr, 1));
    Outcome b = adopt_and_run(kNativeAddr);
    EXPECT_FALSE(b.ok) << "a retired record was adopted";
}

// A block is adopted exactly as compile() would have reused it: a different
// emit policy (here: 64-bit mode at the same address) is not a match.
TEST_F(JitSharedTableTest, AdoptionRespectsModeDiscriminator) {
    MemoGuard memo;
    Outcome a = compile_and_run(kNativeAddr, kNativeCode, sizeof(kNativeCode));
    ASSERT_TRUE(a.ok);
    JitCompiler jit(*m_memory);
    BasicBlock probe;
    EXPECT_FALSE(jit.try_adopt_shared_block(kNativeAddr, true, probe));
    EXPECT_TRUE(jit.try_adopt_shared_block(kNativeAddr, false, probe));
    EXPECT_EQ(probe.compiled_code, a.code);
    EXPECT_EQ(probe.byte_length, sizeof(kNativeCode));
    EXPECT_EQ(probe.instruction_count, 3u);
}

// ---------------------------------------------------------------------------
// Memo lock discipline. Every compile asks accept_smc_compilation and
// publishes through publish_record; adopters read under the shared lock.
// Each test runs both lock arms (VITREO_JIT_MEMO_LEGACY_LOCK) so the reader-
// first path is held to exactly the legacy outcomes.
// ---------------------------------------------------------------------------

namespace {

constexpr GuestAddr kBulkBase = 0x100000;
constexpr u32 kBulkStride = 16;
constexpr u32 kBulkBlocks = 0x40000 / kBulkStride;

struct LegacyLockArm {
    explicit LegacyLockArm(bool legacy) {
        JitCompileMemo::set_legacy_memo_locking_for_test(legacy ? 1 : 0);
    }
    ~LegacyLockArm() { JitCompileMemo::set_legacy_memo_locking_for_test(-1); }
};

class JitMemoLockTest : public JitSharedTableTest {
protected:
    void SetUp() override {
        JitSharedTableTest::SetUp();
        ASSERT_TRUE(m_memory->map_region(kBulkBase, kBulkBlocks * kBulkStride,
                                         MemoryProtection::ReadWrite));
        for (u32 i = 0; i < kBulkBlocks; ++i) {
            const auto code = bulk_code(i);
            write(bulk_addr(i), code.data(), code.size());
        }
    }

    static GuestAddr bulk_addr(u32 i) { return kBulkBase + i * kBulkStride; }
    // kNativeCode with a distinct MOV immediate per block.
    static std::array<u8, sizeof(kNativeCode)> bulk_code(u32 i) {
        std::array<u8, sizeof(kNativeCode)> code{};
        std::memcpy(code.data(), kNativeCode, code.size());
        std::memcpy(code.data() + 1, &i, sizeof(i));
        return code;
    }
    BasicBlock bulk_block(u32 i) {
        const auto code = bulk_code(i);
        return decode_at(bulk_addr(i), code.data(), code.size());
    }
    std::vector<BasicBlock> bulk_blocks(u32 first, u32 count) {
        std::vector<BasicBlock> blocks;
        blocks.reserve(count);
        for (u32 i = 0; i < count; ++i) blocks.push_back(bulk_block(first + i));
        return blocks;
    }

    /// Records in the table whose arena code no longer passes
    /// arena_code_live, and the total walked.
    static std::pair<usize, usize> dead_and_total_records() {
        usize dead = 0, total = 0;
        for (JitCompileMemo::Shard& shard : JitCompileMemo::s_shards) {
            std::shared_lock<JitMemoLock> lock(shard.lock);
            for (const auto& [addr, list] : shard.compiled) {
                (void)addr;
                list.for_each([&](const JitCompileMemo::CompiledEntry& e) {
                    ++total;
                    if (!JitCodeArena::arena_code_live(e.record.code,
                                                       e.record.arena_generation)) {
                        ++dead;
                    }
                });
            }
        }
        return {dead, total};
    }

    static u32 rewrite_count(GuestAddr addr) {
        JitCompileMemo::Shard& shard = JitCompileMemo::shard_for(addr);
        std::shared_lock<JitMemoLock> lock(shard.lock);
        const auto it = shard.smc_rewrites.find(addr);
        return it == shard.smc_rewrites.end() ? 0 : it->second.count;
    }

    /// Seed a rewrite history at bulk block 0 the way production does: a
    /// published record purged by an executable write.
    void seed_rewrite_history() {
        on_thread([&] {
            BlockCache cache(64);
            JitCompiler jit(*m_memory);
            jit.set_block_cache(&cache);
            BasicBlock block = bulk_block(0);
            ASSERT_NE(jit.compile(block), nullptr);
        });
        ASSERT_EQ(JitCompileMemo::purge_shared_records_overlapping(
                      bulk_addr(0), sizeof(kNativeCode), 1),
                  1u);
        bool tracked = false;
        (void)JitCompileMemo::accept_smc_compilation(bulk_addr(0), 0, tracked);
        ASSERT_TRUE(tracked);
    }
};

}  // namespace

// The churn limit trips at exactly kChurnRecompileLimit byte-identity
// changes, a repeated identity never counts, and once demoted every identity
// is refused -- identically under both lock arms.
TEST_F(JitMemoLockTest, SmcChurnLimitTripsAtTheSameCountUnderBothArms) {
    MemoGuard memo;
    for (bool legacy : {true, false}) {
        SCOPED_TRACE(legacy ? "legacy" : "reader-first");
        LegacyLockArm arm(legacy);
        JitCompileMemo::clear_compile_memo();
        seed_rewrite_history();
        // The seeding call above already moved the identity to hash 0.
        ASSERT_EQ(rewrite_count(bulk_addr(0)), 1u);
        for (u64 hash = 1; hash + 1 < JitCompileMemo::kChurnRecompileLimit; ++hash) {
            SCOPED_TRACE(hash);
            bool tracked = false;
            EXPECT_TRUE(JitCompileMemo::accept_smc_compilation(bulk_addr(0), hash, tracked));
            EXPECT_TRUE(tracked);
            EXPECT_TRUE(JitCompileMemo::accept_smc_compilation(bulk_addr(0), hash, tracked))
                << "the same identity again is not a rewrite";
            EXPECT_EQ(rewrite_count(bulk_addr(0)), hash + 1);
            EXPECT_FALSE(JitCompileMemo::shared_smc_demoted(bulk_addr(0)));
        }
        bool tracked = false;
        EXPECT_FALSE(JitCompileMemo::accept_smc_compilation(
            bulk_addr(0), JitCompileMemo::kChurnRecompileLimit - 1, tracked))
            << "the limit-th identity change demotes";
        EXPECT_EQ(rewrite_count(bulk_addr(0)), JitCompileMemo::kChurnRecompileLimit);
        EXPECT_TRUE(JitCompileMemo::shared_smc_demoted(bulk_addr(0)));
        for (u64 hash : {u64{0}, u64{1}, u64{JitCompileMemo::kChurnRecompileLimit}}) {
            EXPECT_FALSE(JitCompileMemo::accept_smc_compilation(bulk_addr(0), hash, tracked));
        }
        EXPECT_EQ(rewrite_count(bulk_addr(0)), JitCompileMemo::kChurnRecompileLimit);
        // An untracked address is accepted and reported untracked.
        EXPECT_TRUE(JitCompileMemo::accept_smc_compilation(bulk_addr(1), 5, tracked));
        EXPECT_FALSE(tracked);
    }
}

// Many threads alternating identities at one tracked address: the count
// saturates at exactly the limit (never past it) under both arms.
TEST_F(JitMemoLockTest, ConcurrentSmcChurnSaturatesAtTheLimit) {
    MemoGuard memo;
    for (bool legacy : {true, false}) {
        SCOPED_TRACE(legacy ? "legacy" : "reader-first");
        LegacyLockArm arm(legacy);
        JitCompileMemo::clear_compile_memo();
        seed_rewrite_history();
        std::atomic<bool> go{false};
        std::vector<std::thread> threads;
        for (u32 t = 0; t < 8; ++t) {
            threads.emplace_back([&, t] {
                while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
                bool tracked = false;
                for (u32 i = 0; i < 2000; ++i) {
                    (void)JitCompileMemo::accept_smc_compilation(
                        bulk_addr(0), 100 + (t + i) % 3, tracked);
                }
            });
        }
        go.store(true, std::memory_order_release);
        for (auto& t : threads) t.join();
        EXPECT_EQ(rewrite_count(bulk_addr(0)), JitCompileMemo::kChurnRecompileLimit);
        EXPECT_TRUE(JitCompileMemo::shared_smc_demoted(bulk_addr(0)));
    }
}

// A translation whose arena chunk was retired is never published, and an
// equal key is never published twice.
TEST_F(JitMemoLockTest, PublishRefusesCodeFromARetiredChunk) {
    MemoGuard memo;
    for (bool legacy : {true, false}) {
        SCOPED_TRACE(legacy ? "legacy" : "reader-first");
        LegacyLockArm arm(legacy);
        JitCompileMemo::clear_compile_memo();
        BlockCache cache(64);
        JitCompiler jit(*m_memory);
        jit.set_block_cache(&cache);
        BasicBlock block = bulk_block(0);
        CompiledBlock* compiled = jit.compile(block);
        ASSERT_NE(compiled, nullptr);
        void* const code = compiled->code;
        const u64 generation = compiled->arena_generation;
        ASSERT_EQ(JitCompileMemo::compile_memo_size(), 1u);
        JitCompileMemo::CompiledKey key{};
        {
            JitCompileMemo::Shard& shard = JitCompileMemo::shard_for(bulk_addr(0));
            std::shared_lock<JitMemoLock> lock(shard.lock);
            shard.compiled.at(bulk_addr(0)).for_each(
                [&](const JitCompileMemo::CompiledEntry& e) { key = e.key; });
        }
        auto build = [&](JitCompileMemo::CompiledRecord& record) {
            record.code = code;
            record.arena_generation = generation;
            record.instruction_count = block.instruction_count;
        };
        EXPECT_FALSE(JitCompileMemo::publish_record(key, code, generation, 0, build))
            << "an equal key is already published";
        JitCompileMemo::clear_compile_memo();
        EXPECT_TRUE(JitCompileMemo::publish_record(key, code, generation, 0, build));
        EXPECT_EQ(JitCompileMemo::compile_memo_size(), 1u);

        JitCompileMemo::clear_compile_memo();
        ASSERT_TRUE(JitCodeArena::retire_arena_chunk_containing(code));
        EXPECT_FALSE(JitCompileMemo::publish_record(key, code, generation, 0, build));
        EXPECT_EQ(JitCompileMemo::compile_memo_size(), 0u);
        {
            JitCompileMemo::Shard& shard = JitCompileMemo::shard_for(bulk_addr(0));
            std::shared_lock<JitMemoLock> lock(shard.lock);
            EXPECT_EQ(shard.compiled.count(bulk_addr(0)), 0u)
                << "a refused publish must not leave an empty list behind";
        }
        BasicBlock probe;
        EXPECT_FALSE(jit.try_adopt_shared_block(bulk_addr(0), false, probe));
    }
}

// Compilers publish while another thread retires the chunks they are
// emitting into. Whatever interleaving happens, no record from a retired
// chunk may survive in the table (an insert racing the purge must not
// resurrect it), and no adopter may see a partially built record.
TEST_F(JitMemoLockTest, RetirementRacingPublicationNeverLeavesRetiredRecords) {
    MemoGuard memo;
    for (bool legacy : {true, false}) {
        SCOPED_TRACE(legacy ? "legacy" : "reader-first");
        LegacyLockArm arm(legacy);
        JitCompileMemo::clear_compile_memo();
        constexpr u32 kCompilers = 3;
        constexpr u32 kPerCompiler = 800;
        std::vector<std::vector<BasicBlock>> work;
        for (u32 t = 0; t < kCompilers; ++t) {
            work.push_back(bulk_blocks(t * kPerCompiler, kPerCompiler));
        }
        std::atomic<void*> latest{nullptr};
        std::atomic<u32> done{0};
        std::atomic<u64> adoptions{0}, partial{0}, retirements{0};
        // All compiles take a few milliseconds and one retirement about one,
        // so the window is held open: compiler 0 waits at its midpoint for
        // retirements while the others keep publishing.
        std::atomic<bool> go{false};
        std::vector<std::thread> threads;
        for (u32 t = 0; t < kCompilers; ++t) {
            threads.emplace_back([&, t] {
                BlockCache cache(64);
                JitCompiler jit(*m_memory);
                jit.set_block_cache(&cache);
                while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
                for (usize k = 0; k < work[t].size(); ++k) {
                    if (CompiledBlock* c = jit.compile(work[t][k])) {
                        latest.store(c->code, std::memory_order_release);
                    }
                    if (t == 0 && k == work[t].size() / 2) {
                        const auto deadline =
                            std::chrono::steady_clock::now() + std::chrono::seconds(2);
                        while (retirements.load(std::memory_order_relaxed) == 0 &&
                               std::chrono::steady_clock::now() < deadline) {
                            std::this_thread::yield();
                        }
                    }
                }
                done.fetch_add(1, std::memory_order_acq_rel);
            });
        }
        threads.emplace_back([&] {
            while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
            while (done.load(std::memory_order_acquire) < kCompilers &&
                   retirements.load(std::memory_order_relaxed) < 48) {
                if (void* c = latest.exchange(nullptr, std::memory_order_acq_rel)) {
                    if (JitCodeArena::retire_arena_chunk_containing(c)) {
                        retirements.fetch_add(1, std::memory_order_relaxed);
                    }
                }
                std::this_thread::yield();
            }
        });
        for (u32 t = 0; t < 2; ++t) {
            threads.emplace_back([&, t] {
                JitCompiler jit(*m_memory);
                u32 i = t * 997;
                while (done.load(std::memory_order_acquire) < kCompilers) {
                    BasicBlock out;
                    if (jit.try_adopt_shared_block(bulk_addr(i % (kCompilers * kPerCompiler)),
                                                   false, out)) {
                        adoptions.fetch_add(1, std::memory_order_relaxed);
                        if (out.compiled_code == nullptr ||
                            out.byte_length != sizeof(kNativeCode) ||
                            out.instruction_count != 3) {
                            partial.fetch_add(1, std::memory_order_relaxed);
                        }
                    }
                    ++i;
                }
            });
        }
        go.store(true, std::memory_order_release);
        for (auto& t : threads) t.join();
        const auto [dead, total] = dead_and_total_records();
        std::printf("[memo-lock] %s: records=%zu retired_chunks=%llu adoptions=%llu\n",
                    legacy ? "legacy" : "reader-first", total,
                    static_cast<unsigned long long>(retirements.load()),
                    static_cast<unsigned long long>(adoptions.load()));
        EXPECT_GT(retirements.load(), 0u) << "the race never retired a chunk";
        EXPECT_EQ(dead, 0u) << "a record from a retired chunk survived";
        EXPECT_EQ(partial.load(), 0u);
        // With retirement over, adoption is exact: it succeeds only for live
        // code.
        JitCompiler jit(*m_memory);
        for (u32 i = 0; i < kCompilers * kPerCompiler; ++i) {
            BasicBlock out;
            if (jit.try_adopt_shared_block(bulk_addr(i), false, out)) {
                EXPECT_TRUE(JitCodeArena::arena_code_live(
                    out.compiled_code, out.compiled_arena_generation));
            }
        }
    }
}

// Publication and adoption stress without retirement: every adoption must
// see a complete, live record.
TEST_F(JitMemoLockTest, AdoptersNeverSeePartialRecords) {
    MemoGuard memo;
    for (bool legacy : {true, false}) {
        SCOPED_TRACE(legacy ? "legacy" : "reader-first");
        LegacyLockArm arm(legacy);
        JitCompileMemo::clear_compile_memo();
        constexpr u32 kCompilers = 4;
        constexpr u32 kPerCompiler = 500;
        std::vector<std::vector<BasicBlock>> work;
        for (u32 t = 0; t < kCompilers; ++t) {
            work.push_back(bulk_blocks(t * kPerCompiler, kPerCompiler));
        }
        std::atomic<u32> done{0};
        std::atomic<u64> adoptions{0}, bad{0};
        std::vector<std::thread> threads;
        for (u32 t = 0; t < kCompilers; ++t) {
            threads.emplace_back([&, t] {
                BlockCache cache(64);
                JitCompiler jit(*m_memory);
                jit.set_block_cache(&cache);
                for (BasicBlock& block : work[t]) (void)jit.compile(block);
                done.fetch_add(1, std::memory_order_acq_rel);
            });
        }
        for (u32 t = 0; t < 4; ++t) {
            threads.emplace_back([&, t] {
                JitCompiler jit(*m_memory);
                // Interleave with the compilers' order so adoptions land on
                // freshly published records.
                u32 i = t;
                while (done.load(std::memory_order_acquire) < kCompilers) {
                    const u32 index = (i % kPerCompiler) + (i / kPerCompiler % kCompilers) * kPerCompiler;
                    BasicBlock out;
                    if (jit.try_adopt_shared_block(bulk_addr(index), false, out)) {
                        adoptions.fetch_add(1, std::memory_order_relaxed);
                        if (out.compiled_code == nullptr ||
                            out.compiled_code_size == 0 ||
                            out.byte_length != sizeof(kNativeCode) ||
                            out.instruction_count != 3 ||
                            !JitCodeArena::arena_code_live(
                                out.compiled_code, out.compiled_arena_generation)) {
                            bad.fetch_add(1, std::memory_order_relaxed);
                        }
                    }
                    i += 4;
                }
            });
        }
        for (auto& t : threads) t.join();
        EXPECT_GT(adoptions.load(), 0u);
        EXPECT_EQ(bad.load(), 0u);
        EXPECT_EQ(JitCompileMemo::compile_memo_size(), kCompilers * kPerCompiler);
    }
}

// The sharded store under a CEF-like thread count: compilers publishing over
// every shard, adopters probing, and SMC acceptance (fast path, tracked and
// churning) all at once. Afterwards every published record is adoptable from
// the shard of its start page, and the churned address is demoted at exactly
// the limit.
TEST_F(JitMemoLockTest, ManyThreadsPublishAdoptAndAcceptAcrossShards) {
    MemoGuard memo;
    for (bool legacy : {true, false}) {
        SCOPED_TRACE(legacy ? "legacy" : "reader-first");
        LegacyLockArm arm(legacy);
        JitCompileMemo::clear_compile_memo();
        // Even blocks are published; odd block 1 carries the churn history,
        // seeded the way production does (a published record purged).
        const GuestAddr churn_addr = bulk_addr(1);
        on_thread([&] {
            BlockCache cache(64);
            JitCompiler jit(*m_memory);
            jit.set_block_cache(&cache);
            BasicBlock block = bulk_block(1);
            ASSERT_NE(jit.compile(block), nullptr);
        });
        ASSERT_EQ(JitCompileMemo::purge_shared_records_overlapping(
                      churn_addr, sizeof(kNativeCode), 1),
                  1u);
        ASSERT_EQ(JitCompileMemo::compile_memo_size(), 0u);

        constexpr u32 kCompilers = 16;
        constexpr u32 kPublished = kBulkBlocks / 2;
        constexpr u32 kPerCompiler = kPublished / kCompilers;
        static_assert(kPublished % kCompilers == 0);
        std::vector<std::vector<BasicBlock>> work(kCompilers);
        for (u32 t = 0; t < kCompilers; ++t) {
            work[t].reserve(kPerCompiler);
            for (u32 k = 0; k < kPerCompiler; ++k) {
                work[t].push_back(bulk_block(2 * (k * kCompilers + t)));
            }
        }
        std::atomic<bool> go{false};
        std::atomic<u32> done{0};
        std::atomic<u64> adoptions{0}, bad_adoptions{0}, bad_accepts{0},
            failed_compiles{0};
        std::vector<std::thread> threads;
        for (u32 t = 0; t < kCompilers; ++t) {
            threads.emplace_back([&, t] {
                BlockCache cache(64);
                JitCompiler jit(*m_memory);
                jit.set_block_cache(&cache);
                while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
                for (BasicBlock& block : work[t]) {
                    if (jit.compile(block) == nullptr) {
                        failed_compiles.fetch_add(1, std::memory_order_relaxed);
                    }
                }
                done.fetch_add(1, std::memory_order_acq_rel);
            });
        }
        for (u32 t = 0; t < 12; ++t) {
            threads.emplace_back([&, t] {
                JitCompiler jit(*m_memory);
                while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
                u32 i = t * 641;
                while (done.load(std::memory_order_acquire) < kCompilers) {
                    BasicBlock out;
                    const u32 index = 2 * (i % kPublished);
                    if (jit.try_adopt_shared_block(bulk_addr(index), false, out)) {
                        adoptions.fetch_add(1, std::memory_order_relaxed);
                        if (out.compiled_code == nullptr ||
                            out.start_addr != bulk_addr(index) ||
                            out.byte_length != sizeof(kNativeCode) ||
                            out.instruction_count != 3) {
                            bad_adoptions.fetch_add(1, std::memory_order_relaxed);
                        }
                    }
                    ++i;
                }
            });
        }
        for (u32 t = 0; t < 4; ++t) {
            threads.emplace_back([&, t] {
                while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
                bool tracked = false;
                for (u32 i = 0; i < 4000; ++i) {
                    (void)JitCompileMemo::accept_smc_compilation(
                        churn_addr, 100 + (t + i) % 3, tracked);
                    if (!tracked) bad_accepts.fetch_add(1, std::memory_order_relaxed);
                    // Never-rewritten addresses, in this shard and others.
                    const GuestAddr other = bulk_addr(2 * ((t * 977 + i) % kPublished));
                    if (!JitCompileMemo::accept_smc_compilation(other, i, tracked) ||
                        tracked) {
                        bad_accepts.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            });
        }
        go.store(true, std::memory_order_release);
        for (auto& t : threads) t.join();

        EXPECT_EQ(failed_compiles.load(), 0u);
        EXPECT_EQ(bad_adoptions.load(), 0u);
        EXPECT_EQ(bad_accepts.load(), 0u);
        EXPECT_EQ(rewrite_count(churn_addr), JitCompileMemo::kChurnRecompileLimit);
        EXPECT_TRUE(JitCompileMemo::shared_smc_demoted(churn_addr));
        ASSERT_EQ(JitCompileMemo::compile_memo_size(), kPublished);

        std::array<usize, JitCompileMemo::kMemoShards> per_shard{};
        usize occupied = 0, misplaced = 0;
        for (usize s = 0; s < JitCompileMemo::kMemoShards; ++s) {
            JitCompileMemo::Shard& shard = JitCompileMemo::s_shards[s];
            std::shared_lock<JitMemoLock> lock(shard.lock);
            for (const auto& [addr, list] : shard.compiled) {
                per_shard[s] += list.size();
                if (JitCompileMemo::shard_index(addr) != s) ++misplaced;
            }
            EXPECT_EQ(per_shard[s], shard.records.load()) << "shard " << s;
            occupied += per_shard[s] != 0;
        }
        EXPECT_EQ(misplaced, 0u);
        // 64 bulk pages over 64 shards; the hash must spread them.
        EXPECT_GE(occupied, JitCompileMemo::kMemoShards / 2);

        JitCompiler jit(*m_memory);
        usize adoptable = 0;
        for (u32 k = 0; k < kPublished; ++k) {
            BasicBlock out;
            if (jit.try_adopt_shared_block(bulk_addr(2 * k), false, out) &&
                out.compiled_code != nullptr) {
                ++adoptable;
            }
        }
        EXPECT_EQ(adoptable, kPublished) << "every published record adopts";
        BasicBlock out;
        EXPECT_FALSE(jit.try_adopt_shared_block(churn_addr, false, out));
    }
}

// Retiring a chunk drops every record whose code is in it, whichever shard
// holds the record, and nothing else.
TEST_F(JitMemoLockTest, ChunkRetirementPurgesEveryShard) {
    MemoGuard memo;
    for (bool legacy : {true, false}) {
        SCOPED_TRACE(legacy ? "legacy" : "reader-first");
        LegacyLockArm arm(legacy);
        JitCompileMemo::clear_compile_memo();
        // Four blocks per bulk page, compiled on four threads.
        constexpr u32 kPages = 0x40000 / 0x1000;
        constexpr u32 kPerPage = 4;
        std::vector<std::thread> threads;
        std::vector<std::vector<void*>> codes(kPerPage);
        for (u32 t = 0; t < kPerPage; ++t) {
            threads.emplace_back([&, t] {
                BlockCache cache(64);
                JitCompiler jit(*m_memory);
                jit.set_block_cache(&cache);
                for (u32 page = 0; page < kPages; ++page) {
                    BasicBlock block = bulk_block(page * (0x1000 / kBulkStride) + t);
                    CompiledBlock* c = jit.compile(block);
                    codes[t].push_back(c ? c->code : nullptr);
                }
            });
        }
        for (auto& t : threads) t.join();
        ASSERT_EQ(JitCompileMemo::compile_memo_size(), kPages * kPerPage);

        auto records_in_chunk = [](usize chunk) {
            usize n = 0;
            std::unordered_set<usize> shards;
            for (usize s = 0; s < JitCompileMemo::kMemoShards; ++s) {
                JitCompileMemo::Shard& shard = JitCompileMemo::s_shards[s];
                std::shared_lock<JitMemoLock> lock(shard.lock);
                for (const auto& [addr, list] : shard.compiled) {
                    (void)addr;
                    list.for_each([&](const JitCompileMemo::CompiledEntry& e) {
                        if (JitCodeArena::arena_chunk_index(e.record.code) == chunk) {
                            ++n;
                            shards.insert(s);
                        }
                    });
                }
            }
            return std::pair{n, shards.size()};
        };
        void* const victim_code = codes[0][0];
        ASSERT_NE(victim_code, nullptr);
        const usize victim = JitCodeArena::arena_chunk_index(victim_code);
        const auto [in_victim, victim_shards] = records_in_chunk(victim);
        ASSERT_GT(victim_shards, 1u) << "the victim chunk must span shards";
        const usize before = JitCompileMemo::compile_memo_size();

        ASSERT_TRUE(JitCodeArena::retire_arena_chunk_containing(victim_code));
        EXPECT_EQ(records_in_chunk(victim).first, 0u);
        EXPECT_EQ(JitCompileMemo::compile_memo_size(), before - in_victim)
            << "records in other chunks survive";
        const auto [dead, total] = dead_and_total_records();
        EXPECT_EQ(dead, 0u);
        EXPECT_EQ(total, before - in_victim);
        JitCompiler jit(*m_memory);
        for (u32 t = 0; t < kPerPage; ++t) {
            for (u32 page = 0; page < kPages; ++page) {
                if (JitCodeArena::arena_chunk_index(codes[t][page]) != victim) continue;
                BasicBlock out;
                EXPECT_FALSE(jit.try_adopt_shared_block(
                    bulk_addr(page * (0x1000 / kBulkStride) + t), false, out));
            }
        }
    }
}

// A record straddling a page boundary lives in its start page's shard. A
// write to only its second page (another shard) must still purge it and
// start its rewrite history.
TEST_F(JitMemoLockTest, PagePurgeReachesARecordStartingInAnotherShard) {
    MemoGuard memo;
    for (bool legacy : {true, false}) {
        SCOPED_TRACE(legacy ? "legacy" : "reader-first");
        LegacyLockArm arm(legacy);
        JitCompileMemo::clear_compile_memo();
        const GuestAddr second_page = kBulkBase + 5 * 0x1000;
        const GuestAddr start = second_page - 6;
        ASSERT_NE(JitCompileMemo::shard_index(start),
                  JitCompileMemo::shard_index(second_page));
        const auto code = bulk_code(7);
        write(start, code.data(), code.size());
        on_thread([&] {
            BlockCache cache(64);
            JitCompiler jit(*m_memory);
            jit.set_block_cache(&cache);
            BasicBlock block = decode_at(start, code.data(), code.size());
            ASSERT_EQ(block.byte_length, code.size());
            ASSERT_NE(jit.compile(block), nullptr);
        });
        ASSERT_EQ(JitCompileMemo::compile_memo_size(), 1u);
        // A write past the record's end, on the same page, purges nothing.
        EXPECT_EQ(JitCompileMemo::purge_shared_records_overlapping(
                      start + code.size(), 1, 1),
                  0u);
        EXPECT_EQ(JitCompileMemo::compile_memo_size(), 1u);
        EXPECT_EQ(JitCompileMemo::purge_shared_records_overlapping(
                      second_page + 1, 1, 2),
                  1u);
        EXPECT_EQ(JitCompileMemo::compile_memo_size(), 0u);
        bool tracked = false;
        EXPECT_TRUE(JitCompileMemo::accept_smc_compilation(start, 0, tracked));
        EXPECT_TRUE(tracked) << "the purge started a rewrite history";
        {
            JitCompileMemo::Shard& shard = JitCompileMemo::shard_for(start);
            std::shared_lock<JitMemoLock> lock(shard.lock);
            EXPECT_TRUE(shard.pages.empty()) << "page index entries released";
        }
    }
}

// BENCHMARK (disabled): 4 compiling + 4 adopting threads, the shape that
// contends s_compiled_mutex in a CEF renderer. Alternates the legacy and
// reader-first arms in the same binary, legacy first on even pairs.
// VITREO_MEMO_BENCH_THREADS=N runs N compiling + N adopting threads instead.
//   vitreo_cpu_tests --gtest_also_run_disabled_tests \
//     --gtest_filter='*MemoLockContention*'
TEST_F(JitMemoLockTest, DISABLED_MemoLockContentionAB) {
    MemoGuard memo;
    const char* threads_env = std::getenv("VITREO_MEMO_BENCH_THREADS");
    const u32 kThreads = std::clamp<u32>(
        threads_env ? static_cast<u32>(std::atoi(threads_env)) : 4, 1, 64);
    constexpr u32 kAdoptSet = 1024;
    const u32 kPerCompiler = std::min<u32>(3000, (kBulkBlocks - kAdoptSet) / kThreads);
    struct Sample {
        double seconds{0};
        u64 compiles{0};
        u64 adopt_calls{0};
        u64 adoptions{0};
    };
    auto run = [&](bool legacy) {
        LegacyLockArm arm(legacy);
        JitCompileMemo::clear_compile_memo();
        on_thread([&] {
            BlockCache cache(64);
            JitCompiler jit(*m_memory);
            jit.set_block_cache(&cache);
            auto seed = bulk_blocks(0, kAdoptSet);
            for (BasicBlock& block : seed) (void)jit.compile(block);
        });
        std::vector<std::vector<BasicBlock>> work;
        for (u32 t = 0; t < kThreads; ++t) {
            work.push_back(bulk_blocks(kAdoptSet + t * kPerCompiler, kPerCompiler));
        }
        std::atomic<u32> ready{0}, done{0};
        std::atomic<bool> go{false};
        std::atomic<u64> compiles{0}, adopt_calls{0}, adoptions{0};
        std::chrono::steady_clock::time_point start, end;
        std::vector<std::thread> threads;
        for (u32 t = 0; t < kThreads; ++t) {
            threads.emplace_back([&, t] {
                BlockCache cache(64);
                JitCompiler jit(*m_memory);
                jit.set_block_cache(&cache);
                ready.fetch_add(1);
                while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
                u64 n = 0;
                for (BasicBlock& block : work[t]) n += jit.compile(block) != nullptr;
                compiles.fetch_add(n);
                if (done.fetch_add(1, std::memory_order_acq_rel) + 1 == kThreads) {
                    end = std::chrono::steady_clock::now();
                }
            });
        }
        for (u32 t = 0; t < kThreads; ++t) {
            threads.emplace_back([&, t] {
                JitCompiler jit(*m_memory);
                ready.fetch_add(1);
                while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
                u64 calls = 0, hits = 0;
                u32 i = t * 257;
                while (done.load(std::memory_order_acquire) < kThreads) {
                    BasicBlock out;
                    hits += jit.try_adopt_shared_block(bulk_addr(i % kAdoptSet), false, out);
                    ++calls;
                    ++i;
                }
                adopt_calls.fetch_add(calls);
                adoptions.fetch_add(hits);
            });
        }
        while (ready.load() < 2 * kThreads) std::this_thread::yield();
        start = std::chrono::steady_clock::now();
        go.store(true, std::memory_order_release);
        for (auto& t : threads) t.join();
        JitCompileMemo::clear_compile_memo();
        Sample s;
        s.seconds = std::chrono::duration<double>(end - start).count();
        s.compiles = compiles.load();
        s.adopt_calls = adopt_calls.load();
        s.adoptions = adoptions.load();
        return s;
    };

    (void)run(false);  // Warm the arena and allocator; not reported.
    constexpr u32 kPairs = 8;
    std::vector<double> adopt_ratio, compile_ratio;
    std::printf("pair  arm           seconds   compiles/s   adopt_calls/s  adoptions\n");
    for (u32 pair = 0; pair < kPairs; ++pair) {
        Sample legacy, reader_first;
        if (pair % 2 == 0) {
            legacy = run(true);
            reader_first = run(false);
        } else {
            reader_first = run(false);
            legacy = run(true);
        }
        for (const auto& [name, s] : {std::pair{"legacy", legacy},
                                      std::pair{"reader-first", reader_first}}) {
            std::printf("%-4u  %-12s  %7.4f  %11.0f  %13.0f  %llu/%llu\n", pair, name,
                        s.seconds, s.compiles / s.seconds, s.adopt_calls / s.seconds,
                        static_cast<unsigned long long>(s.adoptions),
                        static_cast<unsigned long long>(s.adopt_calls));
        }
        const double a = (reader_first.adopt_calls / reader_first.seconds) /
                         (legacy.adopt_calls / legacy.seconds);
        const double c = (reader_first.compiles / reader_first.seconds) /
                         (legacy.compiles / legacy.seconds);
        adopt_ratio.push_back(a);
        compile_ratio.push_back(c);
        std::printf("      ratio reader-first/legacy: adopt %.3f  compile %.3f\n", a, c);
    }
    std::sort(adopt_ratio.begin(), adopt_ratio.end());
    std::sort(compile_ratio.begin(), compile_ratio.end());
    std::printf("median ratio: adopt %.3f (min %.3f)  compile %.3f (min %.3f)\n",
                adopt_ratio[kPairs / 2], adopt_ratio.front(),
                compile_ratio[kPairs / 2], compile_ratio.front());
}

namespace {

constexpr u32 kBulkPages = kBulkBlocks * kBulkStride / 0x1000;
constexpr u32 kBlocksPerPage = 0x1000 / kBulkStride;
constexpr GuestAddr bulk_page(u32 p) { return kBulkBase + p * 0x1000; }

/// True when the memo's page index holds a record on @p page. Bulk blocks
/// never straddle a page, so the record lives in that page's own shard.
bool memo_has_record_on(GuestAddr page) {
    JitCompileMemo::Shard& shard = JitCompileMemo::shard_for(page);
    std::shared_lock<JitMemoLock> lock(shard.lock);
    const auto it = shard.pages.find(page);
    return it != shard.pages.end() && !it->second.empty();
}

}  // namespace

// Publication retains a record's pages and removal releases them, from many
// threads at once and across code-page shards. Whatever the interleaving, a
// page reports code exactly while some record on it is live: never less
// (a store would bypass SMC detection), and not after its last record goes.
TEST_F(JitMemoLockTest, ConcurrentPublishAndPurgeKeepCodePagesExact) {
    MemoGuard memo;
    constexpr u32 kThreads = 16;
    u64 purge_generation = 1;
    for (bool legacy : {true, false}) {
        SCOPED_TRACE(legacy ? "legacy" : "reader-first");
        LegacyLockArm arm(legacy);
        JitCompileMemo::clear_compile_memo();
        ASSERT_FALSE(m_memory->jit_code_pages_overlap(kBulkBase, 0x40000));

        // Every thread compiles one block on every page, so each page's first
        // retain is raced by all of them.
        std::vector<std::thread> threads;
        for (u32 t = 0; t < kThreads; ++t) {
            threads.emplace_back([&, t] {
                BlockCache cache(64);
                JitCompiler jit(*m_memory);
                jit.set_block_cache(&cache);
                for (u32 p = 0; p < kBulkPages; ++p) {
                    BasicBlock block = bulk_block(p * kBlocksPerPage + t);
                    EXPECT_NE(jit.compile(block), nullptr);
                }
            });
        }
        for (auto& t : threads) t.join();
        threads.clear();
        ASSERT_EQ(JitCompileMemo::compile_memo_size(), kBulkPages * kThreads);
        for (u32 p = 0; p < kBulkPages; ++p) {
            EXPECT_TRUE(m_memory->jit_code_pages_overlap(bulk_page(p), 1)) << p;
        }

        // Purge every record on the even pages while other threads publish
        // more records on the odd pages.
        const u64 base_generation = purge_generation;
        purge_generation += kBulkPages;
        for (u32 t = 0; t < kThreads; ++t) {
            threads.emplace_back([&, t] {
                if (t % 2 == 0) {
                    for (u32 p = t / 2 * 2; p < kBulkPages; p += kThreads) {
                        (void)JitCompileMemo::purge_shared_records_overlapping(
                            bulk_page(p), 0x1000, base_generation + p);
                    }
                    return;
                }
                BlockCache cache(64);
                JitCompiler jit(*m_memory);
                jit.set_block_cache(&cache);
                for (u32 p = 1; p < kBulkPages; p += 2) {
                    BasicBlock block =
                        bulk_block(p * kBlocksPerPage + kThreads + t);
                    EXPECT_NE(jit.compile(block), nullptr);
                }
            });
        }
        for (auto& t : threads) t.join();
        threads.clear();
        for (u32 p = 0; p < kBulkPages; ++p) {
            const bool live = memo_has_record_on(bulk_page(p));
            EXPECT_EQ(live, p % 2 == 1) << p;
            EXPECT_EQ(m_memory->jit_code_pages_overlap(bulk_page(p), 1), live)
                << "page " << p << (live ? " lost its code mark while records "
                                           "on it are live"
                                         : " still reports code after its "
                                           "last record was purged");
        }

        (void)JitCompileMemo::purge_shared_records_overlapping(
            kBulkBase, 0x40000, purge_generation++);
        EXPECT_EQ(JitCompileMemo::compile_memo_size(), 0u);
        EXPECT_FALSE(m_memory->jit_code_pages_overlap(kBulkBase, 0x40000));
    }
}

// After a concurrent publish, a guest store into translated code is still
// detected: the writing thread's observer sees the page marked, purges the
// overlapping record (and only it), and no thread can adopt the stale code.
TEST_F(JitMemoLockTest, SmcWriteAfterConcurrentPublishPurgesTheRecord) {
    MemoGuard memo;
    ASSERT_TRUE(m_memory->protect_region(kBulkBase, kBulkBlocks * kBulkStride,
                                         MemoryProtection::ReadWriteExecute));
    constexpr u32 kThreads = 8;
    std::vector<std::thread> threads;
    for (u32 t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t] {
            BlockCache cache(64);
            JitCompiler jit(*m_memory);
            jit.set_block_cache(&cache);
            for (u32 p = 0; p < kBulkPages; ++p) {
                BasicBlock block = bulk_block(p * kBlocksPerPage + t);
                EXPECT_NE(jit.compile(block), nullptr);
            }
        });
    }
    for (auto& t : threads) t.join();
    const usize published = JitCompileMemo::compile_memo_size();
    ASSERT_EQ(published, kBulkPages * kThreads);

    // The writer: its compiler registers the executable-write observer.
    BlockCache cache(64);
    JitCompiler jit(*m_memory);
    jit.set_block_cache(&cache);
    const u32 victim = 5 * kBlocksPerPage + 3;
    const u32 patched_imm = 0xC0DE0000u;
    ASSERT_TRUE(m_memory->write_bytes(bulk_addr(victim) + 1,
                                      reinterpret_cast<const u8*>(&patched_imm),
                                      sizeof(patched_imm)));
    EXPECT_EQ(JitCompileMemo::compile_memo_size(), published - 1)
        << "the store into translated code was not detected";
    EXPECT_TRUE(m_memory->jit_code_pages_overlap(bulk_page(5), 1))
        << "the page's other records are still live";
    on_thread([&] {
        JitCompiler other(*m_memory);
        BasicBlock out;
        EXPECT_FALSE(other.try_adopt_shared_block(bulk_addr(victim), false, out));
        EXPECT_TRUE(other.try_adopt_shared_block(bulk_addr(victim + 1), false, out));
    });

    // Rewrite the whole page: every record on it goes, and so does its mark.
    std::vector<u8> page(0x1000);
    ASSERT_TRUE(m_memory->read_bytes(bulk_page(5), page.data(), page.size()));
    ASSERT_TRUE(m_memory->write_bytes(bulk_page(5), page.data(), page.size()));
    EXPECT_EQ(JitCompileMemo::compile_memo_size(), published - kThreads);
    EXPECT_FALSE(m_memory->jit_code_pages_overlap(bulk_page(5), 1));
    EXPECT_TRUE(m_memory->jit_code_pages_overlap(bulk_page(4), 1));
    EXPECT_TRUE(m_memory->jit_code_pages_overlap(bulk_page(6), 1));
}

// Adoption re-hashes the guest bytes through the region table while another
// thread keeps changing the protection of the very pages being adopted. A
// readable page always adopts; a page the guest made inaccessible never does;
// a page made executable again purges its records (the transition counts as
// an executable write).
TEST_F(JitMemoLockTest, AdoptionObservesConcurrentProtectionChanges) {
    MemoGuard memo;
    constexpr u32 kSet = 2 * kBlocksPerPage;  // pages 0 and 1
    on_thread([&] {
        BlockCache cache(64);
        JitCompiler jit(*m_memory);
        jit.set_block_cache(&cache);
        for (u32 i = 0; i < kSet; ++i) {
            BasicBlock block = bulk_block(i);
            ASSERT_NE(jit.compile(block), nullptr);
        }
    });
    ASSERT_EQ(JitCompileMemo::compile_memo_size(), kSet);

    std::atomic<bool> stop{false};
    std::thread protector([&] {
        while (!stop.load(std::memory_order_acquire)) {
            (void)m_memory->protect_region(kBulkBase, 0x2000,
                                           MemoryProtection::Read);
            (void)m_memory->protect_region(kBulkBase, 0x2000,
                                           MemoryProtection::ReadWrite);
        }
    });
    std::atomic<u64> failures{0}, wrong{0};
    std::vector<std::thread> adopters;
    for (u32 t = 0; t < 8; ++t) {
        adopters.emplace_back([&, t] {
            JitCompiler jit(*m_memory);
            for (u32 i = 0; i < 20000; ++i) {
                const u32 k = (i * 7 + t * 131) % kSet;
                BasicBlock out;
                if (!jit.try_adopt_shared_block(bulk_addr(k), false, out)) {
                    failures.fetch_add(1);
                } else if (out.byte_length != sizeof(kNativeCode) ||
                           out.compiled_code == nullptr) {
                    wrong.fetch_add(1);
                }
            }
        });
    }
    for (auto& t : adopters) t.join();
    stop.store(true, std::memory_order_release);
    protector.join();
    EXPECT_EQ(failures.load(), 0u) << "a readable page failed to adopt";
    EXPECT_EQ(wrong.load(), 0u);

    ASSERT_TRUE(m_memory->protect_region(kBulkBase, 0x1000,
                                         MemoryProtection::None));
    on_thread([&] {
        JitCompiler jit(*m_memory);
        BasicBlock out;
        EXPECT_FALSE(jit.try_adopt_shared_block(bulk_addr(0), false, out))
            << "adopted code from a page the guest made inaccessible";
        EXPECT_TRUE(jit.try_adopt_shared_block(bulk_addr(kBlocksPerPage), false, out));
    });
    ASSERT_TRUE(m_memory->protect_region(kBulkBase, 0x1000,
                                         MemoryProtection::ReadWrite));
    on_thread([&] {
        JitCompiler jit(*m_memory);
        BasicBlock out;
        EXPECT_TRUE(jit.try_adopt_shared_block(bulk_addr(0), false, out));
    });
    // The protecting thread's compiler is the observer that purges.
    BlockCache cache(64);
    JitCompiler observer(*m_memory);
    observer.set_block_cache(&cache);
    ASSERT_TRUE(m_memory->protect_region(kBulkBase, 0x1000,
                                         MemoryProtection::ReadExecute));
    EXPECT_EQ(JitCompileMemo::compile_memo_size(), kSet - kBlocksPerPage)
        << "the transition to executable did not purge the page's records";
    EXPECT_FALSE(m_memory->jit_code_pages_overlap(bulk_page(0), 1));
    EXPECT_TRUE(m_memory->jit_code_pages_overlap(bulk_page(1), 1));
}

// BENCHMARK (disabled): N adopting threads (VITREO_MEMO_BENCH_THREADS, default
// 4) while N/4 (min 1) threads churn VirtualProtect, the V8 shape. Arms:
//   none      no churn;
//   unrelated protection flips and map/unmap on memory nothing adopts;
//   overlap   protection flips (RW <-> R, records survive) on the pages being
//             adopted, so every flip shoots down the adopters' TLB entry and
//             the re-hash goes through m_regions_mutex.
//   vitreo_cpu_tests --gtest_also_run_disabled_tests \
//     --gtest_filter='*AdoptUnderProtectChurn*'
TEST_F(JitMemoLockTest, DISABLED_AdoptUnderProtectChurn) {
    MemoGuard memo;
    const char* threads_env = std::getenv("VITREO_MEMO_BENCH_THREADS");
    const u32 kThreads = std::clamp<u32>(
        threads_env ? static_cast<u32>(std::atoi(threads_env)) : 4, 1, 64);
    const u32 kChurners = std::max<u32>(1, kThreads / 4);
    constexpr u32 kAdoptSet = 1024;
    constexpr GuestAddr kUnrelated = 0x800000;
    ASSERT_TRUE(m_memory->map_region(kUnrelated, 0x10000,
                                     MemoryProtection::ReadWrite));
    on_thread([&] {
        BlockCache cache(64);
        JitCompiler jit(*m_memory);
        jit.set_block_cache(&cache);
        auto seed = bulk_blocks(0, kAdoptSet);
        for (BasicBlock& block : seed) ASSERT_NE(jit.compile(block), nullptr);
    });
    enum class Arm { None, Unrelated, Overlap };
    auto run = [&](Arm arm) {
        std::atomic<bool> go{false}, stop{false};
        std::atomic<u32> ready{0};
        std::atomic<u64> calls{0}, hits{0}, protects{0};
        std::vector<std::thread> threads;
        for (u32 t = 0; t < kThreads; ++t) {
            threads.emplace_back([&, t] {
                JitCompiler jit(*m_memory);
                ready.fetch_add(1);
                while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
                u64 n = 0, h = 0;
                u32 i = t * 257;
                while (!stop.load(std::memory_order_acquire)) {
                    BasicBlock out;
                    h += jit.try_adopt_shared_block(bulk_addr(i % kAdoptSet), false, out);
                    ++n;
                    ++i;
                }
                calls.fetch_add(n);
                hits.fetch_add(h);
            });
        }
        if (arm != Arm::None) {
            for (u32 c = 0; c < kChurners; ++c) {
                threads.emplace_back([&, c] {
                    ready.fetch_add(1);
                    while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
                    u64 n = 0;
                    const GuestAddr scratch = kUnrelated + 0x20000 + c * 0x10000;
                    while (!stop.load(std::memory_order_acquire)) {
                        if (arm == Arm::Overlap) {
                            const GuestAddr page = kBulkBase + (n % 4) * 0x1000;
                            (void)m_memory->protect_region(page, 0x1000, MemoryProtection::Read);
                            (void)m_memory->protect_region(page, 0x1000, MemoryProtection::ReadWrite);
                        } else {
                            (void)m_memory->protect_region(kUnrelated + c * 0x1000, 0x1000,
                                                           MemoryProtection::Read);
                            (void)m_memory->protect_region(kUnrelated + c * 0x1000, 0x1000,
                                                           MemoryProtection::ReadWrite);
                            (void)m_memory->map_region(scratch, 0x4000, MemoryProtection::ReadWrite);
                            (void)m_memory->unmap_region(scratch, 0x4000);
                        }
                        ++n;
                    }
                    protects.fetch_add(n);
                });
            }
        }
        while (ready.load() < threads.size()) std::this_thread::yield();
        const auto start = std::chrono::steady_clock::now();
        go.store(true, std::memory_order_release);
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        stop.store(true, std::memory_order_release);
        const double seconds = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - start).count();
        for (auto& t : threads) t.join();
        return std::array<double, 3>{calls.load() / seconds,
                                     static_cast<double>(hits.load()) /
                                         std::max<u64>(1, calls.load()),
                                     protects.load() / seconds};
    };
    (void)run(Arm::None);
    std::printf("adopters=%u churners=%u\n", kThreads, kChurners);
    std::printf("arm        adopt_calls/s   hit_ratio   churn_iters/s\n");
    for (int rep = 0; rep < 3; ++rep) {
        for (const auto& [name, arm] : {std::pair{"none", Arm::None},
                                        std::pair{"unrelated", Arm::Unrelated},
                                        std::pair{"overlap", Arm::Overlap}}) {
            const auto r = run(arm);
            std::printf("%-9s  %13.0f   %9.4f   %13.0f\n", name, r[0], r[1], r[2]);
        }
    }
}
