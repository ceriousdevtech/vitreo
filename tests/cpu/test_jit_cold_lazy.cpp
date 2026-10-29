// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
//
// Lazy cold memory stubs (jit_cold_lazy.cpp): a linear64 scalar site's slow
// path is emitted on its first take. These tests run each first take, and
// the take after it, against the interpreter -- reads, writes, sub-page
// protection, page crossings and faulting accesses -- then the races and
// lifetimes the lazy form adds: N threads taking one slot at once, a take
// that cannot materialize, and retirement of a block whose sites are partly
// materialized.

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>

#include <libkern/OSCacheControl.h>
#include <pthread.h>
#include <sys/ucontext.h>
#include <vector>

#include "core/cpu/decode/decoder.hpp"
#include "core/cpu/interpreter/interpreter.hpp"
#include "core/cpu/jit/block_cache.hpp"
#include "core/cpu/jit/jit_code_arena.hpp"
#include "core/cpu/jit/jit_compiler.hpp"
#include "core/cpu/jit/linear_fault.hpp"
#include "core/cpu/seh/seh32.hpp"
#include "jit_dispatch_execute.hpp"

using namespace vitreo;
using namespace vitreo::cpu;
using namespace vitreo::cpu::jit;

namespace {

constexpr u8 RAX = static_cast<u8>(RegIndex::RAX);
constexpr u8 RCX = static_cast<u8>(RegIndex::RCX);
constexpr u8 RBX = static_cast<u8>(RegIndex::RBX);

constexpr GuestAddr kCode64 = 0x0000000140000000ull;
constexpr usize kCodeSize = 0x8000;
// One 16 KiB host page of four guest pages: RW, NOACCESS, READONLY, RW. The
// host page is mixed, so every access to it takes its site's slow path.
constexpr GuestAddr kMixed = 0x0000000180000000ull;
// The next host page, uniformly RW.
constexpr GuestAddr kPlain = kMixed + 0x4000;
constexpr GuestAddr kOutside = 0x00000007F0000000ull;  // past the reservation
constexpr usize kReservation64 = usize{8} * 1024 * 1024 * 1024;
constexpr usize kSnapBytes = 0x8000;

struct Form {
    std::vector<u8> code;
    int width;
    bool store;
};

const std::vector<Form>& forms() {
    static const std::vector<Form> f = {
        {{0x8A, 0x03}, 1, false},        // mov al,[rbx]
        {{0x66, 0x8B, 0x03}, 2, false},  // mov ax,[rbx]
        {{0x8B, 0x03}, 4, false},        // mov eax,[rbx]
        {{0x48, 0x8B, 0x03}, 8, false},  // mov rax,[rbx]
        {{0x88, 0x03}, 1, true},         // mov [rbx],al
        {{0x66, 0x89, 0x03}, 2, true},   // mov [rbx],ax
        {{0x89, 0x03}, 4, true},         // mov [rbx],eax
        {{0x48, 0x89, 0x03}, 8, true},   // mov [rbx],rax
        {{0x48, 0x03, 0x03}, 8, false},  // add rax,[rbx]
        {{0x48, 0x01, 0x03}, 8, true},   // add [rbx],rax (load, then store)
    };
    return f;
}

// The plain MOV forms: the ones several threads can run on one address and
// still agree with a single interpreter run.
constexpr usize kMovForms = 8;

class JitColdLazyTest : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        setenv("VITREO_JIT_LINEAR64", "1", 1);
        ASSERT_TRUE(install_linear_fault_handler());
    }

    static void TearDownTestSuite() { unsetenv("VITREO_JIT_LINEAR64"); }

    void SetUp() override {
        if (jit_cold_stub_legacy()) {
            GTEST_SKIP() << "legacy per-site cold stubs have no lazy form";
        }
        JitCompiler::set_cold_lazy_for_test(1);
        // These tests take slow paths on a mixed page to exercise
        // materialization itself; the mixed-page eager policy has its own.
        set_cold_lazy_mixed_eager_for_test(false);
        m_memory = std::make_unique<GuestMemory>();
        ASSERT_TRUE(m_memory->enable_linear_reservation(kReservation64));
        ASSERT_TRUE(m_memory->enable_linear_hardware_protection());
        if (use_view()) ASSERT_TRUE(m_memory->enable_linear_jit_view());
        ASSERT_TRUE(register_linear_reservation(m_memory->linear_base(),
                                                kReservation64));
        if (use_view()) {
            ASSERT_TRUE(register_linear_jit_view(
                m_memory->linear_jit_view_base(), kReservation64));
        }
        ASSERT_TRUE(m_memory->map_region(kCode64, kCodeSize,
                                         MemoryProtection::ReadWrite));
        ASSERT_TRUE(m_memory->map_region(kMixed, 0x8000,
                                         MemoryProtection::ReadWrite));
        for (GuestAddr a = kMixed; a < kMixed + 0x8000; a += 8) {
            ASSERT_TRUE(m_memory->write_u64(a, 0x0101010101010101ull * (a & 0xFF)));
        }
        ASSERT_TRUE(m_memory->protect_region(kMixed + 0x1000, 0x1000,
                                             MemoryProtection::None));
        ASSERT_TRUE(m_memory->protect_region(kMixed + 0x2000, 0x1000,
                                             MemoryProtection::Read));
        const u8* map = m_memory->linear_check_page_map();
        ASSERT_NE(map, nullptr);
        ASSERT_NE(map[kMixed >> kLinearHostPageShift], 0u)
            << "the first data host page must be mixed";
        m_jit = std::make_unique<JitCompiler>(*m_memory);
        m_cache = std::make_unique<BlockCache>();
        m_jit->set_block_cache(m_cache.get());
        m_code_at = kCode64;
    }

    void TearDown() override {
        set_cold_lazy_force_side_exit_for_test(false);
        set_cold_lazy_force_island_full_for_test(false);
        set_cold_lazy_mixed_eager_for_test(true);
        reset_cold_eager_for_test();
        JitCompiler::set_cold_lazy_for_test(-1);
        m_jit.reset();
        if (m_memory) {
            if (use_view()) {
                unregister_linear_jit_view(m_memory->linear_jit_view_base());
            }
            unregister_linear_reservation(m_memory->linear_base());
        }
    }

    virtual bool use_view() const { return false; }

    BasicBlock build64(const std::vector<u8>& code, GuestAddr at) {
        BasicBlock b;
        b.start_addr = at;
        b.is_64bit = true;
        MemoryByteReader r(code.data(), code.size());
        while (r.remaining() > 0) {
            DecodedInstruction i;
            if (decode_instruction(r, DecodeMode::Mode64, i) !=
                DecodeError::Success) {
                break;
            }
            b.instructions.push_back(i);
        }
        b.byte_length = static_cast<u32>(r.position());
        b.instruction_count = static_cast<u32>(b.instructions.size());
        return b;
    }

    // A fresh code address per block, so every block has fresh lazy slots.
    GuestAddr place(const std::vector<u8>& code) {
        m_code_at += 0x40;
        EXPECT_LT(m_code_at + code.size(), kCode64 + kCodeSize);
        EXPECT_TRUE(m_memory->write_bytes(m_code_at, code.data(), code.size()));
        return m_code_at;
    }

    // Data bytes through the reservation: mixed host pages are readable in
    // hardware, so this sees the denied guest page too.
    std::vector<u8> snapshot() const {
        std::vector<u8> out(kSnapBytes);
        std::memcpy(out.data(), m_memory->linear_base() + kMixed, kSnapBytes);
        return out;
    }

    void restore(const std::vector<u8>& snap) {
        u8* const host = m_memory->linear_base() + kMixed;
        // Only the guest-writable pages can have changed.
        std::memcpy(host, snap.data(), 0x1000);
        std::memcpy(host + 0x3000, snap.data() + 0x3000, kSnapBytes - 0x3000);
    }

    static CpuState initial(GuestAddr rip, GuestAddr rbx) {
        CpuState cpu;
        cpu.init_long_mode();
        cpu.rip = rip;
        for (u8 r = 0; r < 16; ++r) {
            cpu.gpr.set64(r, 0x5A5A000000000000ull | (u64{r} << 8));
        }
        cpu.gpr.set64(RAX, 0xF1E2D3C4B5A69788ull);
        cpu.gpr.set64(RBX, rbx);
        cpu.flags.rflags = 0x8D7u;
        return cpu;
    }

    struct Reference {
        CpuState cpu;
        bool raised{false};
        u32 code{0};
        u64 kind{0};
        u64 address{0};
        std::vector<u8> memory;
    };

    // The interpreter's result for the block at @p rip, memory restored.
    Reference interpret(GuestAddr rip, GuestAddr rbx, u32 instructions,
                        void (*setup)(CpuState&) = nullptr) {
        Reference ref;
        const std::vector<u8> before = snapshot();
        ref.cpu = initial(rip, rbx);
        if (setup) setup(ref.cpu);
        Interpreter interp(ref.cpu, *m_memory);
        for (u32 i = 0; i < instructions; ++i) {
            if (interp.step() == ExecResult::Exception) {
                const ExecContext& ctx = interp.context();
                ref.raised = true;
                ref.code = cpu_exception_to_status(ctx.exception);
                ref.kind = ctx.exception_error;
                ref.address = ctx.exception_addr;
                break;
            }
        }
        ref.cpu.materialize_lazy_flags();
        ref.memory = snapshot();
        restore(before);
        return ref;
    }

    void expect_matches(const Reference& ref, CpuState& cpu,
                        JitExecResult result) {
        EXPECT_EQ(result, JitExecResult::Continue);
        cpu.materialize_lazy_flags();
        EXPECT_EQ(cpu.pending_raise.active, ref.raised);
        if (ref.raised && cpu.pending_raise.active) {
            EXPECT_EQ(cpu.pending_raise.code, ref.code);
            EXPECT_EQ(cpu.pending_raise.args[0], ref.kind);
            EXPECT_EQ(cpu.pending_raise.args[1], ref.address);
            EXPECT_EQ(cpu.rip, ref.cpu.rip) << "fault not at its instruction";
        } else if (!ref.raised) {
            EXPECT_EQ(cpu.rip, ref.cpu.rip);
        }
        for (u8 r = 0; r < 16; ++r) {
            EXPECT_EQ(cpu.gpr.get64(r), ref.cpu.gpr.get64(r))
                << "GPR " << unsigned{r};
        }
        constexpr u64 kStatus = 0x8D5u;
        EXPECT_EQ(cpu.flags.rflags & kStatus, ref.cpu.flags.rflags & kStatus);
        const std::vector<u8> mem = snapshot();
        for (usize i = 0; i < mem.size(); ++i) {
            if (mem[i] != ref.memory[i]) {
                ADD_FAILURE() << "memory diverged at 0x" << std::hex
                              << (kMixed + i) << ": interp=0x"
                              << unsigned{ref.memory[i]} << " jit=0x"
                              << unsigned{mem[i]};
                break;
            }
        }
    }

    std::unique_ptr<GuestMemory> m_memory;
    std::unique_ptr<JitCompiler> m_jit;
    std::unique_ptr<BlockCache> m_cache;
    GuestAddr m_code_at{kCode64};
};

// Every scalar form, at every slow-path cause: a readable/writable guest
// page of a mixed host page, a denied one, a read-only one, crossings out of
// the mixed page into a denied page, from read-only into RW, and into the
// next host page, and an address past the reservation. The first run
// materializes the site's stub; the second runs the materialized stub. Both
// must equal the interpreter, faults included (code, kind, address, RIP, no
// partial store).
TEST_F(JitColdLazyTest, FirstTakeMatchesInterpreterForReadsWritesCrossingsAndFaults) {
    struct Target {
        const char* what;
        GuestAddr (*address)(int width);
        bool slow;  // reaches the site's slot on every run
    };
    const Target targets[] = {
        {"rw guest page", [](int) { return kMixed + 0x100; }, true},
        {"denied guest page", [](int) { return kMixed + 0x1010; }, true},
        {"read-only guest page", [](int) { return kMixed + 0x2010; }, true},
        {"crossing rw->denied",
         [](int w) { return kMixed + 0x1000 - static_cast<GuestAddr>(w / 2); },
         true},
        {"crossing ro->rw",
         [](int w) { return kMixed + 0x3000 - static_cast<GuestAddr>(w / 2); },
         true},
        {"crossing into the next host page",
         [](int w) { return kPlain - static_cast<GuestAddr>(w / 2); }, true},
        {"past the reservation", [](int) { return kOutside; }, false},
        {"plain page", [](int) { return kPlain + 0x200; }, false},
    };
    reset_cold_lazy_stats();
    u64 slow_sites = 0;
    for (const Form& f : forms()) {
        for (const Target& t : targets) {
            if (f.width == 1 && std::strncmp(t.what, "crossing", 8) == 0) {
                continue;
            }
            SCOPED_TRACE(::testing::Message()
                         << t.what << " width=" << f.width
                         << " store=" << f.store);
            const GuestAddr at = t.address(f.width);
            const GuestAddr rip = place(f.code);
            BasicBlock block = build64(f.code, rip);
            CompiledBlock* compiled = m_jit->compile(block);
            ASSERT_NE(compiled, nullptr);
            const Reference ref = interpret(rip, at, 1);
            for (int run = 0; run < 2; ++run) {
                SCOPED_TRACE(run == 0 ? "first take" : "materialized");
                const u64 before = cold_lazy_stats().materialized;
                const std::vector<u8> mem = snapshot();
                CpuState cpu = initial(rip, at);
                const JitExecResult r =
                    test::execute_like_dispatcher(*m_jit, *compiled, cpu);
                expect_matches(ref, cpu, r);
                restore(mem);
                const u64 made = cold_lazy_stats().materialized - before;
                if (run == 1) {
                    EXPECT_EQ(made, 0u) << "a materialized site re-materialized";
                } else if (t.slow) {
                    // ADD [rbx],rax has a load site and a store site; the
                    // store's is not reached when the load faults.
                    const u64 most = f.code[1] == 0x01 ? 2u : 1u;
                    EXPECT_GE(made, 1u) << "the slow path did not materialize";
                    EXPECT_LE(made, most);
                    slow_sites += made;
                }
            }
        }
    }
    const ColdLazyStats s = cold_lazy_stats();
    EXPECT_GE(s.materialized, slow_sites);
    EXPECT_EQ(s.side_exits, 0u);
    std::printf("[cold-lazy] sites=%llu materialized=%llu (%llu B)\n",
                static_cast<unsigned long long>(s.sites),
                static_cast<unsigned long long>(s.materialized),
                static_cast<unsigned long long>(s.materialized_bytes));
}

// The slow path of a site that is not the block's first instruction, with
// the flags of the instruction before it live: the stub's NZCV spill and
// the raise entry's restore must leave exactly the interpreter's flags.
TEST_F(JitColdLazyTest, SecondInstructionSiteKeepsTheLiveFlags) {
    for (const Form& f : forms()) {
        for (const GuestAddr at : {kMixed + 0x108, kMixed + 0x1018,
                                   kMixed + 0x2018}) {
            // A faulting RMW store after a flag writer keeps the RMW's
            // flags in eager emission too: the pre-existing defect
            // JitParityTest.FaultingMemoryKillerKeepsThePriorWritersFlags.
            if (f.code[1] == 0x01 && at == kMixed + 0x2018) continue;
            SCOPED_TRACE(::testing::Message()
                         << "width=" << f.width << " store=" << f.store
                         << " at=0x" << std::hex << at);
            std::vector<u8> code = {0x48, 0x83, 0xC1, 0x7F};  // add rcx,0x7f
            code.insert(code.end(), f.code.begin(), f.code.end());
            const GuestAddr rip = place(code);
            BasicBlock block = build64(code, rip);
            ASSERT_EQ(block.instruction_count, 2u);
            CompiledBlock* compiled = m_jit->compile(block);
            ASSERT_NE(compiled, nullptr);
            const auto setup = [](CpuState& c) {
                c.gpr.set64(RCX, 0x7FFFFFFFFFFFFF90ull);
            };
            const Reference ref = interpret(rip, at, 2, setup);
            for (int run = 0; run < 2; ++run) {
                const std::vector<u8> mem = snapshot();
                CpuState cpu = initial(rip, at);
                setup(cpu);
                const JitExecResult r =
                    test::execute_like_dispatcher(*m_jit, *compiled, cpu);
                expect_matches(ref, cpu, r);
                restore(mem);
            }
        }
    }
}

// N threads, each with its own compiler and JitContext (as adopting guest
// threads have), enter one block at once and take its one slot together.
// Exactly one stub is materialized per site; every thread computes the
// interpreter's result.
TEST_F(JitColdLazyTest, ConcurrentFirstTakeMaterializesOnce) {
    constexpr int kThreads = 8;
    constexpr int kRounds = 16;
    reset_cold_lazy_stats();
    for (int round = 0; round < kRounds; ++round) {
        SCOPED_TRACE(::testing::Message() << "round " << round);
        const Form& f = forms()[static_cast<usize>(round) % kMovForms];
        const GuestAddr at = kMixed + 0x200 + static_cast<GuestAddr>(round) * 16;
        const GuestAddr rip = place(f.code);
        BasicBlock block = build64(f.code, rip);
        CompiledBlock* compiled = m_jit->compile(block);
        ASSERT_NE(compiled, nullptr);
        const Reference ref = interpret(rip, at, 1);

        std::vector<std::unique_ptr<JitCompiler>> jits;
        std::vector<std::unique_ptr<BlockCache>> caches;
        for (int i = 0; i < kThreads; ++i) {
            jits.push_back(std::make_unique<JitCompiler>(*m_memory));
            caches.push_back(std::make_unique<BlockCache>());
            jits.back()->set_block_cache(caches.back().get());
        }
        const u64 before = cold_lazy_stats().materialized;
        std::atomic<int> ready{0};
        std::array<CpuState, kThreads> cpus;
        std::array<JitExecResult, kThreads> results{};
        std::vector<std::thread> threads;
        for (int i = 0; i < kThreads; ++i) {
            cpus[i] = initial(rip, at);
            threads.emplace_back([&, i] {
                ready.fetch_add(1, std::memory_order_acq_rel);
                while (ready.load(std::memory_order_acquire) < kThreads) {
                }
                CompiledBlock local = *compiled;
                results[i] = jits[i]->execute(local, cpus[i]);
            });
        }
        for (std::thread& t : threads) t.join();
        EXPECT_EQ(cold_lazy_stats().materialized - before, 1u);
        for (int i = 0; i < kThreads; ++i) {
            SCOPED_TRACE(::testing::Message() << "thread " << i);
            // Every thread stored the same value, so the memory check holds.
            expect_matches(ref, cpus[i], results[i]);
        }
        restore(snapshot());
    }
    const ColdLazyStats s = cold_lazy_stats();
    std::printf("[cold-lazy] concurrent rounds=%d threads=%d races=%llu\n",
                kRounds, kThreads, static_cast<unsigned long long>(s.races));
}

// A take that cannot materialize (island full or chunk retired; forced here)
// leaves at the instruction and the instruction is interpreted: the result
// is the interpreter's, the slot stays pending, and nothing is materialized.
// r9 Satisfactory (2026-10-20): a guest AV at a valid address, raised by the
// fault handler at a tier-1 crossing-by-fault site whose word was already
// `B variant` when the handler read it. Several threads take the same
// fresh site's alignment fault together; the first one's retry patches the
// site while another is still inside its handler, which must see a patched
// site and re-run it, never raise. Fresh blocks every round, plain host page,
// an 8-byte access crossing a 16-byte granule, loads and stores.
namespace race_seam {
// The window of the r9 race, made deterministic: between this thread's
// alignment fault at a crossing site and its handler reading the word at the
// faulting PC, another thread's retry patches that site. The seam does what
// the patched site would: it performs the access (a plain unaligned load,
// what every crossing variant does) and leaves `B #4` -- a branch the handler
// cannot recognise as any known patched shape -- at the PC.
std::atomic<uintptr_t> g_lo{0}, g_hi{0};
std::atomic<bool> g_fired{false};
void patch_under_the_handler(LinearFaultTestPoint point, uintptr_t pc,
                             void* ucontext) {
    if (point != LinearFaultTestPoint::Entry) return;
    if (pc < g_lo.load() || pc >= g_hi.load() || g_fired.load()) return;
    u32 w = 0;
    __builtin_memcpy(&w, reinterpret_cast<const void*>(pc), 4);
    const u32 family = w & 0x3FFFFC00u;
    if (family != 0x38BFC000u && family != 0x08DFFC00u) return;  // LDAPR / LDAR
    g_fired.store(true);
    auto* uc = static_cast<ucontext_t*>(ucontext);
    const u32 rt = w & 31u, rn = (w >> 5) & 31u;
    const usize size = usize{1} << (w >> 30);
    u64 value = 0;
    __builtin_memcpy(&value, reinterpret_cast<const void*>(uc->uc_mcontext->__ss.__x[rn]), size);
    uc->uc_mcontext->__ss.__x[rt] = value;
    pthread_jit_write_protect_np(0);
    const u32 b4 = 0x14000001u;  // B #4
    __builtin_memcpy(reinterpret_cast<void*>(pc), &b4, 4);
    pthread_jit_write_protect_np(1);
    sys_icache_invalidate(reinterpret_cast<void*>(pc), 4);
}
}  // namespace race_seam

TEST_F(JitColdLazyTest, AFaultWhoseSiteWasPatchedMeanwhileReRunsTheSite) {
    // mov rax,[rbx]: 8 bytes at an EA crossing a 16-byte granule of a plain
    // RW host page, a fresh block whose ordered load alignment-faults.
    const std::vector<u8> code = {0x48, 0x8B, 0x03};
    const GuestAddr at = kPlain + 0x40C;
    const GuestAddr rip = place(code);
    BasicBlock block = build64(code, rip);
    CompiledBlock* compiled = m_jit->compile(block);
    ASSERT_NE(compiled, nullptr);
    const Reference ref = interpret(rip, at, 1);
    race_seam::g_fired.store(false);
    race_seam::g_lo.store(reinterpret_cast<uintptr_t>(compiled->code));
    race_seam::g_hi.store(reinterpret_cast<uintptr_t>(compiled->code) + compiled->code_size);
    set_linear_fault_test_hook_for_test(&race_seam::patch_under_the_handler);
    CpuState cpu = initial(rip, at);
    const JitExecResult r = test::execute_like_dispatcher(*m_jit, *compiled, cpu);
    set_linear_fault_test_hook_for_test(nullptr);
    ASSERT_TRUE(race_seam::g_fired.load()) << "the access never alignment-faulted";
    EXPECT_FALSE(cpu.pending_raise.active)
        << "raised 0x" << std::hex << cpu.pending_raise.code << " at a valid address";
    expect_matches(ref, cpu, r);
}

TEST_F(JitColdLazyTest, ConcurrentCrossingOnAFreshSiteNeverRaises) {
    constexpr int kThreads = 8;
    constexpr int kRounds = 48;
    constexpr int kRuns = 4;
    for (int round = 0; round < kRounds; ++round) {
        SCOPED_TRACE(::testing::Message() << "round " << round);
        const bool store = (round & 1) != 0;
        // A flag writer before the access keeps NZCV live across it (the
        // lazy stub then spills it), as in compiled game code.
        const bool live_flags = (round & 2) != 0;
        std::vector<u8> code;
        if (live_flags) code = {0x48, 0x83, 0xC1, 0x7F};  // add rcx,0x7f
        if (store) code.insert(code.end(), {0x48, 0x89, 0x03});  // mov [rbx],rax
        else code.insert(code.end(), {0x48, 0x8B, 0x03});        // mov rax,[rbx]
        const GuestAddr at = kPlain + 0x100 * static_cast<GuestAddr>(round % 60) + 0xC;
        const GuestAddr rip = place(code);
        BasicBlock block = build64(code, rip);
        CompiledBlock* compiled = m_jit->compile(block);
        ASSERT_NE(compiled, nullptr);
        const Reference ref = interpret(rip, at, block.instruction_count);

        std::vector<std::unique_ptr<JitCompiler>> jits;
        std::vector<std::unique_ptr<BlockCache>> caches;
        for (int i = 0; i < kThreads; ++i) {
            jits.push_back(std::make_unique<JitCompiler>(*m_memory));
            caches.push_back(std::make_unique<BlockCache>());
            jits.back()->set_block_cache(caches.back().get());
        }
        std::atomic<int> ready{0};
        std::array<std::array<CpuState, kRuns>, kThreads> cpus;
        std::array<std::array<JitExecResult, kRuns>, kThreads> results{};
        std::vector<std::thread> threads;
        for (int i = 0; i < kThreads; ++i) {
            for (int r = 0; r < kRuns; ++r) cpus[i][r] = initial(rip, at);
            threads.emplace_back([&, i] {
                ready.fetch_add(1, std::memory_order_acq_rel);
                while (ready.load(std::memory_order_acquire) < kThreads) {
                }
                for (int r = 0; r < kRuns; ++r) {
                    CompiledBlock local = *compiled;
                    results[i][r] = test::execute_like_dispatcher(*jits[i], local, cpus[i][r]);
                }
            });
        }
        for (std::thread& t : threads) t.join();
        for (int i = 0; i < kThreads; ++i) {
            for (int r = 0; r < kRuns; ++r) {
                SCOPED_TRACE(::testing::Message() << "thread " << i << " run " << r);
                ASSERT_FALSE(cpus[i][r].pending_raise.active)
                    << "a crossing on a valid page raised: code 0x" << std::hex
                    << cpus[i][r].pending_raise.code << " address 0x"
                    << cpus[i][r].pending_raise.args[1];
                expect_matches(ref, cpus[i][r], results[i][r]);
            }
        }
        restore(snapshot());
    }
}

TEST_F(JitColdLazyTest, SideExitWhenMaterializationFailsMatchesInterpreter) {
    set_cold_lazy_force_side_exit_for_test(true);
    reset_cold_lazy_stats();
    u64 expected_exits = 0;
    for (const Form& f : forms()) {
        for (const GuestAddr at : {kMixed + 0x110, kMixed + 0x1020,
                                   kMixed + 0x2020}) {
            SCOPED_TRACE(::testing::Message()
                         << "width=" << f.width << " store=" << f.store
                         << " at=0x" << std::hex << at);
            std::vector<u8> code = {0x48, 0x83, 0xC1, 0x7F};  // add rcx,0x7f
            code.insert(code.end(), f.code.begin(), f.code.end());
            const GuestAddr rip = place(code);
            BasicBlock block = build64(code, rip);
            CompiledBlock* compiled = m_jit->compile(block);
            ASSERT_NE(compiled, nullptr);
            const auto setup = [](CpuState& c) {
                c.gpr.set64(RCX, 0x7FFFFFFFFFFFFF90ull);
            };
            const Reference ref = interpret(rip, at, 2, setup);
            for (int run = 0; run < 2; ++run) {
                const std::vector<u8> mem = snapshot();
                CpuState cpu = initial(rip, at);
                setup(cpu);
                const JitExecResult r =
                    test::execute_like_dispatcher(*m_jit, *compiled, cpu);
                expect_matches(ref, cpu, r);
                restore(mem);
                ++expected_exits;
            }
        }
    }
    const ColdLazyStats s = cold_lazy_stats();
    EXPECT_EQ(s.materialized, 0u);
    EXPECT_EQ(s.side_exits, expected_exits);
}

// A live chunk whose far-branch island cannot take a stub will not take it on
// any later attempt either. The failed take side-exits once (exactly the
// interpreter's result), the block is withdrawn at the next dispatch, and its
// recompile carries eager slow paths: later takes neither materialize nor
// side-exit, and still match the interpreter.
TEST_F(JitColdLazyTest, AStubThatCannotMaterializeRecompilesItsBlockEagerly) {
    set_cold_lazy_force_island_full_for_test(true);
    const std::vector<u8> code = {0x48, 0x8B, 0x03};  // mov rax,[rbx]
    const GuestAddr at = kMixed + 0x140;               // mixed host page: slow
    const GuestAddr rip = place(code);
    BasicBlock* entry = m_cache->insert(build64(code, rip));
    ASSERT_NE(entry, nullptr);
    CompiledBlock* lazy = m_jit->compile(*entry);
    ASSERT_NE(lazy, nullptr);
    const void* const lazy_code = lazy->code;
    CompiledBlock first = *lazy;
    m_jit->publish_compiled_block(*entry, *lazy);
    ASSERT_NE(entry->compiled_code, nullptr);
    const Reference ref = interpret(rip, at, 1);
    reset_cold_lazy_stats();

    const auto run_once = [&](CompiledBlock& cb) {
        const std::vector<u8> mem = snapshot();
        CpuState cpu = initial(rip, at);
        expect_matches(ref, cpu, test::execute_like_dispatcher(*m_jit, cb, cpu));
        restore(mem);
    };
    run_once(first);
    ColdLazyStats s = cold_lazy_stats();
    EXPECT_EQ(s.materialized, 0u);
    EXPECT_EQ(s.side_exits, 1u);
    EXPECT_TRUE(cold_eager_requested_for(rip, entry->byte_length));

    // The next dispatch withdraws the block that holds the failed site.
    m_jit->service_cold_residency();
    EXPECT_EQ(entry->compiled_code, nullptr);
    EXPECT_EQ(cold_lazy_stats().eager_withdrawn, 1u);

    // As the dispatcher would after re-decoding the released entry.
    BasicBlock again = build64(code, rip);
    CompiledBlock* eager = m_jit->compile(again);
    ASSERT_NE(eager, nullptr);
    EXPECT_NE(eager->code, lazy_code) << "the lazy translation was reused";
    CompiledBlock second = *eager;
    for (int run = 0; run < 3; ++run) run_once(second);
    s = cold_lazy_stats();
    EXPECT_EQ(s.materialized, 0u) << "an eager site needs no materialization";
    EXPECT_EQ(s.side_exits, 1u) << "the eager block side-exited";
}

// A first take that is an ordinary access to a mixed host page keeps coming
// back (the data stays on that page). The take itself is served natively by
// the materialized stub, and the block is recompiled at the next dispatch
// with its slow paths eager and adjacent rather than in the far island.
TEST_F(JitColdLazyTest, AMixedPageFirstTakeRecompilesItsBlockEagerly) {
    set_cold_lazy_mixed_eager_for_test(true);
    const std::vector<u8> code = {0x48, 0x8B, 0x03};  // mov rax,[rbx]
    const GuestAddr at = kMixed + 0x140;               // readable, mixed page
    const GuestAddr rip = place(code);
    BasicBlock* entry = m_cache->insert(build64(code, rip));
    ASSERT_NE(entry, nullptr);
    CompiledBlock* lazy = m_jit->compile(*entry);
    ASSERT_NE(lazy, nullptr);
    CompiledBlock first = *lazy;
    m_jit->publish_compiled_block(*entry, *lazy);
    const Reference ref = interpret(rip, at, 1);
    reset_cold_lazy_stats();
    const auto run_once = [&](CompiledBlock& cb) {
        const std::vector<u8> mem = snapshot();
        CpuState cpu = initial(rip, at);
        expect_matches(ref, cpu, test::execute_like_dispatcher(*m_jit, cb, cpu));
        restore(mem);
    };
    run_once(first);
    ColdLazyStats s = cold_lazy_stats();
    EXPECT_EQ(s.materialized, 1u);
    EXPECT_EQ(s.mixed_eager, 1u);
    EXPECT_EQ(s.side_exits, 0u);
    EXPECT_TRUE(cold_eager_requested_for(rip, entry->byte_length));
    m_jit->service_cold_residency();
    EXPECT_EQ(entry->compiled_code, nullptr);

    BasicBlock again = build64(code, rip);
    CompiledBlock* eager = m_jit->compile(again);
    ASSERT_NE(eager, nullptr);
    CompiledBlock second = *eager;
    for (int run = 0; run < 3; ++run) run_once(second);
    s = cold_lazy_stats();
    EXPECT_EQ(s.materialized, 1u) << "the eager block materialized a stub";
    EXPECT_EQ(s.side_exits, 0u);
}

// A block with one materialized and one pending site: retiring its chunk
// retires the stub with it (the stub is in the chunk's island, its metadata
// in the chunk's range), the block is never entered again, and a recompile
// materializes afresh with the interpreter's result.
TEST_F(JitColdLazyTest, RetiringABlockRetiresItsMaterializedStubs) {
    // The load reads the mixed host page (slow); the store writes the plain
    // one (fast), so its site stays pending.
    const std::vector<u8> code = {
        0x48, 0x8B, 0x03,                    // mov rax,[rbx]
        0x89, 0x83, 0x00, 0x41, 0x00, 0x00,  // mov [rbx+0x4100],eax
    };
    const GuestAddr at = kMixed + 0x140;
    const GuestAddr rip = place(code);
    BasicBlock block = build64(code, rip);
    ASSERT_EQ(block.instruction_count, 2u);
    CompiledBlock* compiled = m_jit->compile(block);
    ASSERT_NE(compiled, nullptr);
    const Reference ref = interpret(rip, at, 2);
    reset_cold_lazy_stats();

    const std::vector<u8> mem = snapshot();
    CpuState cpu = initial(rip, at);
    expect_matches(ref, cpu, test::execute_like_dispatcher(*m_jit, *compiled, cpu));
    restore(mem);
    ASSERT_EQ(cold_lazy_stats().materialized, 1u)
        << "only the load's site takes its slow path";

    // The materialized stub: a B from the block into its chunk's island.
    const void* stub = nullptr;
    const auto* words = static_cast<const u32*>(compiled->code);
    const usize hot = compiled->code_size / 4;
    const auto scan = [&](const u32* w, usize n) {
        for (usize i = 0; i < n && stub == nullptr; ++i) {
            if ((w[i] & 0xFC000000u) != 0x14000000u) continue;
            const i64 d = static_cast<i32>(w[i] << 6) >> 6;
            const u32* to = w + i + d;
            if (JitCodeArena::in_far_branch_island(compiled->code, to)) {
                stub = to;
            }
        }
    };
    scan(words, hot);
    if (stub == nullptr && compiled->cold_size != 0) {
        scan(reinterpret_cast<const u32*>(
                 static_cast<const u8*>(compiled->code) + compiled->cold_offset),
             compiled->cold_size / 4);
    }
    ASSERT_NE(stub, nullptr) << "no branch to a materialized stub";
    JitFaultMetadata md{};
    ASSERT_TRUE(jit_fault_metadata_for_pc(reinterpret_cast<uintptr_t>(stub), md));
    EXPECT_TRUE(md.has_guest_rip);
    EXPECT_EQ(md.guest_rip, rip);

    ASSERT_TRUE(JitCodeArena::retire_arena_chunk_containing(compiled->code));
    for (int i = 0; i < 50 && JitCodeArena::arena_pending_free_count() != 0;
         ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        (void)JitCodeArena::sweep_retired_chunks();
    }
    EXPECT_FALSE(JitCodeArena::arena_code_live(compiled->code,
                                               compiled->arena_generation));
    if (JitCodeArena::arena_pending_free_count() == 0) {
        EXPECT_FALSE(jit_fault_metadata_for_pc(
            reinterpret_cast<uintptr_t>(stub), md))
            << "a retired stub kept its fault metadata";
    }
    CpuState stale = initial(rip, at);
    EXPECT_EQ(m_jit->execute(*compiled, stale), JitExecResult::StaleCode);

    // A fresh compile has fresh slots.
    m_jit = std::make_unique<JitCompiler>(*m_memory);
    m_cache = std::make_unique<BlockCache>();
    m_jit->set_block_cache(m_cache.get());
    BasicBlock again = build64(code, rip);
    CompiledBlock* recompiled = m_jit->compile(again);
    ASSERT_NE(recompiled, nullptr);
    CpuState cpu2 = initial(rip, at);
    expect_matches(ref, cpu2,
                   test::execute_like_dispatcher(*m_jit, *recompiled, cpu2));
    restore(mem);
    EXPECT_EQ(cold_lazy_stats().materialized, 2u);
}

// On the JIT view a scalar site is lean (linear_fault.hpp): its guard goes
// to the variant V, and the view refusing an access makes the handler patch
// the guard to `B V`. Lazily, V is the slot: the patched guard enters it,
// and the materialized stub begins with V, whose own check-byte branch
// must be complete. Every form, against the interpreter, twice.
class JitColdLazyViewTest : public JitColdLazyTest {
protected:
    static void SetUpTestSuite() {
        setenv("VITREO_JIT_VIEW", "1", 1);
        JitColdLazyTest::SetUpTestSuite();
    }
    static void TearDownTestSuite() {
        unsetenv("VITREO_JIT_VIEW");
        JitColdLazyTest::TearDownTestSuite();
    }
    bool use_view() const override { return true; }
};

TEST_F(JitColdLazyViewTest, LeanSitesMaterializeTheirVariantOnFirstTake) {
    reset_cold_lazy_stats();
    for (const Form& f : forms()) {
        for (const GuestAddr at : {kMixed + 0x180, kMixed + 0x1030,
                                   kMixed + 0x2030,
                                   kMixed + 0x1000 - 4}) {
            if (f.width == 1 && at == kMixed + 0x1000 - 4) continue;
            SCOPED_TRACE(::testing::Message()
                         << "width=" << f.width << " store=" << f.store
                         << " at=0x" << std::hex << at);
            const GuestAddr rip = place(f.code);
            BasicBlock block = build64(f.code, rip);
            CompiledBlock* compiled = m_jit->compile(block);
            ASSERT_NE(compiled, nullptr);
            const Reference ref = interpret(rip, at, 1);
            for (int run = 0; run < 2; ++run) {
                const std::vector<u8> mem = snapshot();
                CpuState cpu = initial(rip, at);
                const JitExecResult r =
                    test::execute_like_dispatcher(*m_jit, *compiled, cpu);
                expect_matches(ref, cpu, r);
                restore(mem);
            }
        }
    }
    const ColdLazyStats s = cold_lazy_stats();
    EXPECT_GT(s.materialized, 0u);
    EXPECT_EQ(s.side_exits, 0u);
    EXPECT_GT(lean_site_patch_count(), 0u);
}

// Slow-path-heavy kernel: one block of 16 loads and stores, every one in a
// mixed host page, executed repeatedly in interleaved pairs of eager
// (legacy emission) and lazy (materialized) stubs. The materialized stub
// is the eager stub's words behind one more branch, so the per-take cost
// should match within noise.
TEST_F(JitColdLazyTest, DISABLED_MixedProtectionSlowPathBenchmark) {
    std::vector<u8> code;
    for (int i = 0; i < 8; ++i) {
        const u8 d = static_cast<u8>(i * 16);
        const u8 load[] = {0x48, 0x8B, 0x43, d};   // mov rax,[rbx+d]
        const u8 store[] = {0x48, 0x89, 0x43, static_cast<u8>(d + 8)};
        code.insert(code.end(), std::begin(load), std::end(load));
        code.insert(code.end(), std::begin(store), std::end(store));
    }
    constexpr int kPairs = 5;
    constexpr int kExecs = 200000;
    const auto time_mode = [&](int lazy) {
        JitCompiler::set_cold_lazy_for_test(lazy);
        m_jit = std::make_unique<JitCompiler>(*m_memory);
        m_cache = std::make_unique<BlockCache>();
        m_jit->set_block_cache(m_cache.get());
        const GuestAddr rip = place(code);
        BasicBlock block = build64(code, rip);
        CompiledBlock* compiled = m_jit->compile(block);
        EXPECT_NE(compiled, nullptr);
        CpuState cpu = initial(rip, kMixed + 0x400);
        (void)m_jit->execute(*compiled, cpu);  // first take, not timed
        const auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < kExecs; ++i) {
            cpu.rip = rip;
            (void)m_jit->execute(*compiled, cpu);
        }
        const auto t1 = std::chrono::steady_clock::now();
        EXPECT_FALSE(cpu.pending_raise.active);
        return std::chrono::duration<double, std::nano>(t1 - t0).count() /
               kExecs;
    };
    std::vector<double> eager, lazy;
    for (int p = 0; p < kPairs; ++p) {
        if (p % 2 == 0) {
            eager.push_back(time_mode(0));
            lazy.push_back(time_mode(1));
        } else {
            lazy.push_back(time_mode(1));
            eager.push_back(time_mode(0));
        }
    }
    std::sort(eager.begin(), eager.end());
    std::sort(lazy.begin(), lazy.end());
    std::printf("[cold-lazy] mixed-protection block (16 slow sites): eager "
                "median %.1f ns/exec, lazy median %.1f ns/exec, ratio %.3f\n",
                eager[kPairs / 2], lazy[kPairs / 2],
                lazy[kPairs / 2] / eager[kPairs / 2]);
    std::printf("[cold-lazy] eager=");
    for (double v : eager) std::printf(" %.1f", v);
    std::printf(" lazy=");
    for (double v : lazy) std::printf(" %.1f", v);
    std::printf("\n");
}

}  // namespace
