// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
//
// Native linear64 LOCK read-modify-write. Split from test_jit_linear.cpp;
// shared fixtures and matchers live in jit_linear_fixture.hpp.

#include "jit_linear_fixture.hpp"

using namespace vitreo;
using namespace vitreo::cpu;
using namespace vitreo::cpu::jit;
using namespace vitreo::cpu::jit::test;

namespace {

// Native LOCK read-modify-write (compile_locked's LSE fast path) against the
// interpreter, which does the same RMW through the Intel SDM definitions: the
// memory result, the register write-backs (XADD/XCHG's source, CMPXCHG's
// accumulator -- written only on a mismatch, so a 32-bit match keeps
// RAX[63:32]) and every arithmetic flag, both materialised and as the SETcc
// that follows in the same block reads it. INC/DEC run with CF clear and set:
// they must carry it through. Every form must actually take the native path
// (an LSE or CAS word in the block), or the test proves nothing.
TEST_F(JitLinear64Test, NativeLockRmwMatchesInterpreter) {
    struct Form { const char* name; std::vector<u8> bytes; bool cmpxchg; bool wide; };
    const std::vector<Form> forms = {
        {"lock xadd [rbx],ecx", {0xF0,0x0F,0xC1,0x0B}, false, false},
        {"lock inc dword [rbx]", {0xF0,0xFF,0x03}, false, false},
        {"lock dec dword [rbx]", {0xF0,0xFF,0x0B}, false, false},
        {"lock cmpxchg [rbx],ecx", {0xF0,0x0F,0xB1,0x0B}, true, false},
        {"lock xchg [rbx],ecx", {0xF0,0x87,0x0B}, false, false},
        {"lock add [rbx],ecx", {0xF0,0x01,0x0B}, false, false},
        {"lock sub [rbx],ecx", {0xF0,0x29,0x0B}, false, false},
        {"lock or [rbx],ecx", {0xF0,0x09,0x0B}, false, false},
        {"lock and [rbx],ecx", {0xF0,0x21,0x0B}, false, false},
        {"lock add dword [rbx],-3", {0xF0,0x83,0x03,0xFD}, false, false},
        {"lock sub dword [rbx],imm32", {0xF0,0x81,0x2B,0x78,0x56,0x34,0x12}, false, false},
        {"lock xadd [rbx],rcx", {0xF0,0x48,0x0F,0xC1,0x0B}, false, true},
        {"lock inc qword [rbx]", {0xF0,0x48,0xFF,0x03}, false, true},
        {"lock dec qword [rbx]", {0xF0,0x48,0xFF,0x0B}, false, true},
        {"lock cmpxchg [rbx],rcx", {0xF0,0x48,0x0F,0xB1,0x0B}, true, true},
        {"lock xchg [rbx],rcx", {0xF0,0x48,0x87,0x0B}, false, true},
        {"lock add [rbx],rcx", {0xF0,0x48,0x01,0x0B}, false, true},
        {"lock sub [rbx],rcx", {0xF0,0x48,0x29,0x0B}, false, true},
        {"lock or [rbx],rcx", {0xF0,0x48,0x09,0x0B}, false, true},
        {"lock and [rbx],rcx", {0xF0,0x48,0x21,0x0B}, false, true},
        {"lock xor [rbx],rcx", {0xF0,0x48,0x31,0x0B}, false, true},
        {"lock or qword [rbx],-1", {0xF0,0x48,0x83,0x0B,0xFF}, false, true},
        {"lock and qword [rbx],imm32", {0xF0,0x48,0x81,0x23,0xF0,0xFF,0x00,0x80}, false, true},
        // Unprefixed XCHG with memory is locked too, and leaves every flag.
        {"xchg [rbx],ecx", {0x87,0x0B}, false, false},
        {"xchg [rbx],rcx", {0x48,0x87,0x0B}, false, true},
    };
    // setb r8b ; sete r9b ; seto r10b ; sets r11b
    const std::vector<u8> readers = {0x41,0x0F,0x92,0xC0, 0x41,0x0F,0x94,0xC1,
                                     0x41,0x0F,0x90,0xC2, 0x41,0x0F,0x98,0xC3};
    const std::vector<std::pair<u64, u64>> values = {
        {0, 0}, {1, 1}, {0x7FFFFFFFu, 1}, {0xFFFFFFFFu, 1},
        {0x80000000u, 0x80000000u}, {0x12345678u, 0x9ABCDEF0u},
        {0x7FFFFFFFFFFFFFFFull, 1}, {0xFFFFFFFFFFFFFFFFull, 1},
        {0x8000000000000000ull, 0x8000000000000000ull},
        {0x0123456789ABCDEFull, 0xFEDCBA9876543210ull},
    };
    constexpr u64 kFlagMask = 0x8D5;  // OF SF ZF AF PF CF
    GuestAddr code_at = kCode64;
    for (const Form& form : forms) {
        std::vector<u8> code = form.bytes;
        code.insert(code.end(), readers.begin(), readers.end());
        // A fresh address per form: rewriting one code page again and again
        // is what the self-modifying-code guard is for.
        code_at += 0x80;
        ASSERT_TRUE(m_memory->write_bytes(code_at, code.data(), code.size()));
        BasicBlock basic = build64(code);
        basic.start_addr = code_at;
        CompiledBlock* block = m_jit->compile(basic);
        ASSERT_NE(block, nullptr) << form.name;
        const auto* words = static_cast<const u32*>(block->code);
        const usize count = block->code_size / sizeof(u32);
        EXPECT_TRUE(std::any_of(words, words + count, [](u32 w) {
            return (w & 0x3F200C00u) == 0x38200000u ||   // LSE atomic
                   (w & 0x3FA07C00u) == 0x08A07C00u;     // CAS
        })) << form.name << ": no native atomic emitted";
        for (const auto& [mem_value, src_value] : values) {
            const u64 mem = form.wide ? mem_value : (mem_value & 0xFFFFFFFFu);
            for (const bool match : {true, false}) {
                if (!form.cmpxchg && !match) continue;
                for (const u64 cf : {0ull, 1ull}) {
                    // A 32-bit CMPXCHG match must leave RAX[63:32] alone.
                    const u64 rax = (form.wide ? 0 : 0xDEADBEEF00000000ull) |
                                    (match ? mem : (mem ^ 0x5A));
                    const auto seed = [&](CpuState& c) {
                        c.init_long_mode();
                        c.rip = code_at;
                        c.gpr.set64(0, rax);
                        c.gpr.set64(1, src_value);
                        c.gpr.set64(3, kData64);
                        c.flags.rflags = 0x2 | cf;
                        EXPECT_TRUE(m_memory->write_u64(kData64, mem ^ 0));
                        if (!form.wide) {
                            EXPECT_TRUE(m_memory->write_u32(kData64 + 4,
                                                            0xCAFEF00Du));
                        }
                    };
                    CpuState oracle;
                    seed(oracle);
                    {
                        Interpreter interp(oracle, *m_memory);
                        for (int i = 0; i < 5; ++i) {
                            ASSERT_EQ(interp.step(), ExecResult::Continue)
                                << form.name;
                        }
                    }
                    oracle.materialize_lazy_flags();
                    const u64 oracle_mem = m_memory->read_u64(kData64).value_or(0);

                    CpuState jit_cpu;
                    seed(jit_cpu);
                    (void)test::execute_like_dispatcher(*m_jit, *block, jit_cpu);
                    ASSERT_FALSE(jit_cpu.pending_raise.active) << form.name;
                    jit_cpu.materialize_lazy_flags();
                    const u64 jit_mem = m_memory->read_u64(kData64).value_or(0);

                    const std::string what = std::string(form.name) +
                        " mem=" + std::to_string(mem) + " src=" +
                        std::to_string(src_value) + " cf=" + std::to_string(cf) +
                        (form.cmpxchg ? (match ? " match" : " mismatch") : "");
                    EXPECT_EQ(jit_mem, oracle_mem) << what;
                    EXPECT_EQ(jit_cpu.gpr.get64(0), oracle.gpr.get64(0)) << what << " RAX";
                    EXPECT_EQ(jit_cpu.gpr.get64(1), oracle.gpr.get64(1)) << what << " RCX";
                    EXPECT_EQ(jit_cpu.flags.rflags & kFlagMask,
                              oracle.flags.rflags & kFlagMask) << what;
                    for (u8 r = 8; r <= 11; ++r) {
                        EXPECT_EQ(jit_cpu.gpr.get64(r), oracle.gpr.get64(r))
                            << what << " SETcc r" << int(r);
                    }
                    EXPECT_EQ(lock_prefix_slot(g_lock_prefix_tls).active.load(), 0u)
                        << what << ": reader slot left set";
                }
            }
        }
    }
}

// The reader protocol under contention. Four threads each run their own
// compiled `lock inc` loop natively on one counter while a fifth hammers a
// split-lock `lock xadd`, whose non-atomic fallback must wait for every native
// reader to leave (LockPrefixGuard::enter_exclusive_locked) and hold them off
// while it runs. No increment may be lost, the fallback's own updates must
// land, and nothing may deadlock.
TEST_F(JitLinear64Test, NativeLockRmwIsAtomicAgainstSplitLockFallback) {
    const GuestAddr counter = kData64;
    const GuestAddr split = kData64 + 0x103;  // straddles a 4-byte boundary
    ASSERT_TRUE(m_memory->write_u64(counter, 0));
    ASSERT_TRUE(m_memory->write_u64(split - 3, 0));
    ASSERT_TRUE(m_memory->write_u64(split + 5, 0));
    // top: lock inc dword [rbx] ; dec rcx ; jnz top
    const std::vector<u8> inc_loop = {0xF0, 0xFF, 0x03, 0x48, 0xFF, 0xC9,
                                      0x0F, 0x85, 0xF4, 0xFF, 0xFF, 0xFF};
    // top: lock add [rbx],edx ; dec rcx ; jnz top   (EDX = 1)
    const std::vector<u8> split_loop = {0xF0, 0x01, 0x13, 0x48, 0xFF, 0xC9,
                                        0x0F, 0x85, 0xF4, 0xFF, 0xFF, 0xFF};
    const GuestAddr inc_at = kCode64 + 0x1000;
    const GuestAddr split_at = kCode64 + 0x1100;
    ASSERT_TRUE(m_memory->write_bytes(inc_at, inc_loop.data(), inc_loop.size()));
    ASSERT_TRUE(m_memory->write_bytes(split_at, split_loop.data(), split_loop.size()));

    constexpr u64 kIncs = 200000;
    constexpr int kIncThreads = 4;
    constexpr u64 kSplits = 20000;
    const auto run_loop = [&](GuestAddr at, const std::vector<u8>& code,
                              GuestAddr rbx, u64 iterations, bool xadd) {
        JitCompiler jit(*m_memory);
        BlockCache cache;
        jit.set_block_cache(&cache);
        BasicBlock basic = build64(code);
        basic.start_addr = at;
        CompiledBlock* block = jit.compile(basic);
        ASSERT_NE(block, nullptr);
        CpuState cpu;
        cpu.init_long_mode();
        cpu.rip = at;
        cpu.gpr.set64(3, rbx);
        cpu.gpr.set64(1, iterations);
        const GuestAddr jnz = at + code.size() - 6;
        const GuestAddr done = at + code.size();
        while (cpu.rip != done) {
            if (cpu.rip == jnz) {
                cpu.rip = cpu.gpr.get64(1) != 0 ? at : done;
                continue;
            }
            ASSERT_EQ(cpu.rip, at);
            if (xadd) cpu.gpr.set64(2, 1);
            cpu.jit_iters = 0xFFFFFFFFu;
            ASSERT_EQ(test::execute_like_dispatcher(jit, *block, cpu), JitExecResult::Continue);
            ASSERT_FALSE(cpu.pending_raise.active);
        }
    };
    std::vector<std::thread> threads;
    for (int t = 0; t < kIncThreads; ++t) {
        threads.emplace_back(run_loop, inc_at, std::cref(inc_loop), counter,
                             kIncs, false);
    }
    threads.emplace_back(run_loop, split_at, std::cref(split_loop), split,
                         kSplits, true);
    for (auto& t : threads) t.join();

    EXPECT_EQ(m_memory->read_u32(counter).value_or(0), kIncs * kIncThreads);
    EXPECT_EQ(m_memory->read_u32(split).value_or(0), kSplits);
}

// The native path's two exits. A split lock (misaligned operand) is x86's
// bus-locked case and must take the helper's non-atomic fallback, still with
// the right result. A read-only page is enforced by the host page: the LSE
// store faults, the guest sees an access violation reported as a WRITE (x86
// reports a faulting LOCK RMW as a write), memory is unchanged, and the
// thread's reader slot is cleared -- otherwise the next split-lock fallback
// on any thread would wait on it forever.
TEST_F(JitLinear64Test, NativeLockRmwSplitLockAndReadOnlyFault) {
    const std::vector<u8> xadd = {0xF0, 0x0F, 0xC1, 0x0B};  // lock xadd [rbx],ecx
    ASSERT_TRUE(m_memory->write_bytes(kCode64, xadd.data(), xadd.size()));
    CompiledBlock* block = m_jit->compile(build64(xadd));
    ASSERT_NE(block, nullptr);

    ASSERT_TRUE(m_memory->write_u64(kData64, 0x1111111122222222ull));
    auto split = cpu64(kData64 + 2);  // straddles bytes 2..5
    split.gpr.set64(1, 0x10);
    EXPECT_EQ(test::execute_like_dispatcher(*m_jit, *block, split), JitExecResult::Continue);
    EXPECT_FALSE(split.pending_raise.active);
    EXPECT_EQ(split.gpr.get64(1), 0x11112222u) << "old value into ECX";
    EXPECT_EQ(m_memory->read_u64(kData64).value_or(0), 0x1111111122322222ull);

    const GuestAddr ro = kData64 + 0x2000;
    ASSERT_TRUE(m_memory->write_u32(ro, 7));
    ASSERT_TRUE(m_memory->protect_region(ro, 0x1000, MemoryProtection::Read));
    auto denied = cpu64(ro);
    denied.gpr.set64(1, 1);
    (void)test::execute_like_dispatcher(*m_jit, *block, denied);
    ASSERT_TRUE(denied.pending_raise.active);
    EXPECT_EQ(denied.pending_raise.code, 0xC0000005u);
    EXPECT_EQ(denied.pending_raise.args[0], 1u) << "a LOCK RMW faults as a write";
    EXPECT_EQ(denied.pending_raise.args[1], ro);
    EXPECT_EQ(m_memory->read_u32(ro).value_or(0), 7u);
    EXPECT_EQ(denied.gpr.get64(1), 1u) << "no write-back after the fault";
    EXPECT_EQ(lock_prefix_slot(g_lock_prefix_tls).active.load(), 0u)
        << "reader slot left set by the faulting atomic";
    ASSERT_TRUE(m_memory->protect_region(ro, 0x1000, MemoryProtection::ReadWrite));
}

}  // namespace
