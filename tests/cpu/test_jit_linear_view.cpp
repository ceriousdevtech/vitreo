// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
//
// Translated code on the JIT view. Under VITREO_JIT_VIEW=1
// an x64 block addresses guest memory through GuestMemory's JIT view, whose
// hardware protection enforces what the guest allows, instead of probing the
// check map per access.
//
// Expected values are x86 semantics (Intel SDM Vol. 1 3.7 for addressing, Vol.
// 3A 4.6 for faults): each test states them; none is taken from a Vitreo run.

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include <unistd.h>

#include "core/cpu/decode/decoder.hpp"
#include "core/cpu/jit/block_cache.hpp"
#include "core/cpu/jit/jit_compile_memo.hpp"
#include "core/cpu/jit/jit_compiler.hpp"
#include "core/cpu/jit/jit_context.hpp"
#include "core/cpu/jit/jit_helpers.hpp"
#include "core/cpu/jit/linear_fault.hpp"
#include "core/cpu/jit/stencils.hpp"
#include "core/memory/guest_memory.hpp"
#include "core/memory/write_watch_traps.hpp"

using namespace vitreo;
using namespace vitreo::cpu;
using namespace vitreo::cpu::jit;
using namespace vitreo::cpu::jit::jit_detail;

namespace {

class JitLinearViewTest : public ::testing::Test {
protected:
    static constexpr GuestAddr kCode64 = 0x0000000140000000ull;
    static constexpr GuestAddr kData64 = 0x0000000180000000ull;
    static constexpr usize kReservation = usize{8} * 1024 * 1024 * 1024;

    // The suite needs both switches, but must hand the process back exactly
    // as it found it: a ctest rerun runs later suites (JitParity64Test) under
    // these same variables, and unsetting them silently ran those without the
    // JIT view.
    static void SetUpTestSuite() {
        save("VITREO_JIT_LINEAR64", s_prev_linear64);
        save("VITREO_JIT_VIEW", s_prev_view);
        setenv("VITREO_JIT_LINEAR64", "1", 1);
        setenv("VITREO_JIT_VIEW", "1", 1);
        ASSERT_TRUE(install_linear_fault_handler());
    }

    static void TearDownTestSuite() {
        restore("VITREO_JIT_VIEW", s_prev_view);
        restore("VITREO_JIT_LINEAR64", s_prev_linear64);
    }

    static void save(const char* name, std::optional<std::string>& slot) {
        const char* v = std::getenv(name);
        slot = v != nullptr ? std::optional<std::string>(v) : std::nullopt;
    }
    static void restore(const char* name, const std::optional<std::string>& slot) {
        if (slot) setenv(name, slot->c_str(), 1);
        else unsetenv(name);
    }
    static inline std::optional<std::string> s_prev_linear64;
    static inline std::optional<std::string> s_prev_view;

    void SetUp() override {
        // Test cases reuse guest VAs for different bytes.
        JitCompileMemo::clear_compile_memo();
        JitCompileMemo::disable_compile_memo();
        m_memory = std::make_unique<GuestMemory>();
        ASSERT_TRUE(m_memory->enable_linear_reservation(kReservation));
        ASSERT_TRUE(m_memory->enable_linear_hardware_protection());
        ASSERT_TRUE(m_memory->enable_linear_jit_view());
        ASSERT_TRUE(register_linear_reservation(m_memory->linear_base(),
                                                kReservation));
        ASSERT_TRUE(register_linear_jit_view(m_memory->linear_jit_view_base(),
                                             kReservation));
        ASSERT_TRUE(m_memory->map_region(kCode64, 0x4000,
                                         MemoryProtection::ReadWrite));
        ASSERT_TRUE(m_memory->map_region(kData64, 0x4000,
                                         MemoryProtection::ReadWrite));
        m_jit = std::make_unique<JitCompiler>(*m_memory);
        m_cache = std::make_unique<BlockCache>();
        m_jit->set_block_cache(m_cache.get());
    }

    void TearDown() override {
        m_jit.reset();
        if (m_memory) {
            unregister_linear_jit_view(m_memory->linear_jit_view_base());
            unregister_linear_reservation(m_memory->linear_base());
        }
    }

    BasicBlock build64(const std::vector<u8>& code, GuestAddr at = kCode64) {
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

    CpuState cpu64() {
        CpuState cpu;
        cpu.init_long_mode();
        cpu.rip = kCode64;
        cpu.jit_iters = 0xFFFFFFFFu;
        return cpu;
    }

    static std::vector<u32> words_of(const CompiledBlock& b) {
        std::vector<u32> w(b.code_size / 4);
        std::memcpy(w.data(), b.code, b.code_size);
        return w;
    }

    /// LDRB (register offset) with base register @p rn, any Rt/Rm.
    static usize count_ldrb_base(const std::vector<u32>& w, u8 rn) {
        constexpr u32 kMask = 0xFFE0FFE0u;  // keep opcode, option, Rn
        const u32 want = arm64::ldrb_reg(0, rn, 0) & kMask;
        return static_cast<usize>(std::count_if(
            w.begin(), w.end(), [&](u32 x) { return (x & kMask) == want; }));
    }

    /// The lean hot-path access: `ADD X17, X15, Xm` (the JIT view's base)
    /// followed by an ordered load or store through X17.
    static bool has_lean_access(const std::vector<u32>& w) {
        constexpr u32 kAddMask = 0xFFE0FFFFu;  // any Rm
        const u32 add = 0x8B000000u | (15u << 5) | 17u;
        for (usize i = 0; i + 1 < w.size(); ++i) {
            if ((w[i] & kAddMask) != add) continue;
            const u32 family = w[i + 1] & 0x3FFFFC00u;
            const bool ordered = family == 0x38BFC000u ||  // LDAPR*
                                 family == 0x08DFFC00u ||  // LDAR*
                                 family == 0x089FFC00u;    // STLR*
            if (ordered && ((w[i + 1] >> 5) & 31u) == 17u) return true;
        }
        return false;
    }

    /// The lean vector access: LDR/STR Q [X15, Xm] (128-bit), or ADD X17,
    /// X15, Xm followed by LDP/STP Q [X17] (256-bit).
    static bool has_lean_vector_access(const std::vector<u32>& w) {
        constexpr u32 kRegMask = 0xFFE0FFE0u;  // any Rt, Rm
        const u32 ldr = arm64::ldr_q_reg(0, 15, 0) & kRegMask;
        const u32 str = arm64::str_q_reg(0, 15, 0) & kRegMask;
        constexpr u32 kPairMask = ~(0x1Fu | (0x1Fu << 10));  // any Rt, Rt2
        const u32 ldp = arm64::ldp_q_off(0, 0, 17, 0) & kPairMask;
        const u32 stp = arm64::stp_q_off(0, 0, 17, 0) & kPairMask;
        const u32 add = 0x8B000000u | (15u << 5) | 17u;
        for (usize i = 0; i < w.size(); ++i) {
            if ((w[i] & kRegMask) == ldr || (w[i] & kRegMask) == str) return true;
            if (i + 1 < w.size() && (w[i] & 0xFFE0FFFFu) == add &&
                ((w[i + 1] & kPairMask) == ldp || (w[i + 1] & kPairMask) == stp)) {
                return true;
            }
        }
        return false;
    }

    CompiledBlock* compile_at(const std::vector<u8>& code, GuestAddr at = kCode64) {
        if (!m_memory->write_bytes(at, code.data(), code.size())) return nullptr;
        return m_jit->compile(build64(code, at));
    }

    std::unique_ptr<GuestMemory> m_memory;
    std::unique_ptr<JitCompiler> m_jit;
    std::unique_ptr<BlockCache> m_cache;
};

constexpr std::array<u8, 3> kLoadRaxFromRbx = {0x48, 0x8B, 0x03};   // mov rax,[rbx]
constexpr std::array<u8, 3> kStoreRaxToRbx = {0x48, 0x89, 0x03};    // mov [rbx],rax

std::vector<u8> bytes(const std::array<u8, 3>& a) { return {a.begin(), a.end()}; }

// Pins the per-block (legacy) exit emission for a test that inspects the
// words of a block's own probe, dispatcher tail or prologue; the shared
// stubs carry the same words once per process.
struct SharedExitStubsOff {
    SharedExitStubsOff() { vitreo::cpu::jit::JitCompiler::set_shared_exit_stubs_for_test(0); }
    ~SharedExitStubsOff() { vitreo::cpu::jit::JitCompiler::set_shared_exit_stubs_for_test(-1); }
};

// X15 holds the JIT view's base, not the check map: every block entry loads it
// from JitContext::jit_view_base, and any check-map probe that remains must
// take its base from JitContext instead of X15.
TEST_F(JitLinearViewTest, JitViewModeLoadsTheViewBaseIntoX15) {
    SharedExitStubsOff legacy_exits;
    const std::vector<u8> code = {0x48, 0x8B, 0x03};  // mov rax,[rbx]
    ASSERT_TRUE(m_memory->write_bytes(kCode64, code.data(), code.size()));
    BasicBlock block = build64(code);
    ASSERT_EQ(block.instruction_count, 1u);
    CompiledBlock* compiled = m_jit->compile(block);
    ASSERT_NE(compiled, nullptr);
    const std::vector<u32> w = words_of(*compiled);
    const u32 load_view = arm64::ldr_x_uoff(REG_LINEAR64_CHECK_MAP, REG_JIT_CTX,
                                            JIT_CTX_OFF_JIT_VIEW_BASE >> 3);
    const u32 load_map = arm64::ldr_x_uoff(REG_LINEAR64_CHECK_MAP, REG_JIT_CTX,
                                           JIT_CTX_OFF_LINEAR_CHECK_MAP >> 3);
    EXPECT_NE(std::find(w.begin(), w.end(), load_view), w.end())
        << "the block never loads the JIT view's base into X15";
    EXPECT_EQ(std::find(w.begin(), w.end(), load_map), w.end())
        << "X15 still receives the check map under the JIT view";
    EXPECT_EQ(count_ldrb_base(w, REG_LINEAR64_CHECK_MAP), 0u)
        << "a check-map probe still indexes X15, which is now the JIT view";

    constexpr u64 value = 0x1122334455667788ull;
    ASSERT_TRUE(m_memory->write_u64(kData64 + 0x40, value));
    CompiledBlock copy = *compiled;
    CpuState cpu = cpu64();
    cpu.gpr.rbx = kData64 + 0x40;
    EXPECT_EQ(m_jit->execute(copy, cpu), JitExecResult::Continue);
    EXPECT_FALSE(cpu.pending_raise.active);
    EXPECT_EQ(cpu.gpr.rax, value);
}

// ---------------------------------------------------------------------------
// Lean scalar sites (Task 3): the access goes straight through the JIT view;
// whatever the hardware refuses sends that one site to its checked variant.
// ---------------------------------------------------------------------------

TEST_F(JitLinearViewTest, LeanLoadReadsAUniformPage) {
    CompiledBlock* compiled = compile_at(bytes(kLoadRaxFromRbx));
    ASSERT_NE(compiled, nullptr);
    EXPECT_TRUE(has_lean_access(words_of(*compiled)))
        << "the load still goes through the check map and the host view";
    ASSERT_TRUE(m_memory->write_u64(kData64 + 0x40, 0x0102030405060708ull));
    const u64 patches = lean_site_patch_count();
    CpuState cpu = cpu64();
    cpu.gpr.rbx = kData64 + 0x40;
    EXPECT_EQ(m_jit->execute(*compiled, cpu), JitExecResult::Continue);
    EXPECT_FALSE(cpu.pending_raise.active);
    EXPECT_EQ(cpu.gpr.rax, 0x0102030405060708ull);
    EXPECT_EQ(lean_site_patch_count(), patches)
        << "a load from a uniform RW page must never fault";
}

TEST_F(JitLinearViewTest, LeanStoreWritesAUniformPage) {
    CompiledBlock* compiled = compile_at(bytes(kStoreRaxToRbx));
    ASSERT_NE(compiled, nullptr);
    EXPECT_TRUE(has_lean_access(words_of(*compiled)));
    const u64 patches = lean_site_patch_count();
    CpuState cpu = cpu64();
    cpu.gpr.rbx = kData64 + 0x80;
    cpu.gpr.rax = 0x1122334455667788ull;
    EXPECT_EQ(m_jit->execute(*compiled, cpu), JitExecResult::Continue);
    EXPECT_FALSE(cpu.pending_raise.active);
    EXPECT_EQ(m_memory->read_u64(kData64 + 0x80).value_or(0), 0x1122334455667788ull);
    EXPECT_EQ(lean_site_patch_count(), patches);
}

TEST_F(JitLinearViewTest, LeanStoreToMixedPageWritablePartPatchesOnce) {
    // .rdata beside .data in one host page: the JIT view is read-only there,
    // the guest permits the store to the RW part.
    ASSERT_TRUE(m_memory->protect_region(kData64, 0x1000, MemoryProtection::Read));
    CompiledBlock* compiled = compile_at(bytes(kStoreRaxToRbx));
    ASSERT_NE(compiled, nullptr);
    CompiledBlock copy = *compiled;
    const u64 patches = lean_site_patch_count();
    for (u64 round = 1; round <= 2; ++round) {
        CpuState cpu = cpu64();
        cpu.gpr.rbx = kData64 + 0x1000 + 8 * round;
        cpu.gpr.rax = 0xA5A5A5A500000000ull | round;
        EXPECT_EQ(m_jit->execute(copy, cpu), JitExecResult::Continue);
        EXPECT_FALSE(cpu.pending_raise.active);
        EXPECT_EQ(m_memory->read_u64(kData64 + 0x1000 + 8 * round).value_or(0),
                  0xA5A5A5A500000000ull | round);
        EXPECT_EQ(lean_site_patch_count(), patches + 1)
            << "round " << round << ": the site must patch once, then stay patched";
    }
}

TEST_F(JitLinearViewTest, LeanLoadFromNoAccessRaisesAtItsRip) {
    // add rcx,1 ; mov rax,[rbx] -- the AV belongs to the MOV, after the ADD.
    const std::vector<u8> code = {0x48, 0x83, 0xC1, 0x01, 0x48, 0x8B, 0x03};
    CompiledBlock* compiled = compile_at(code);
    ASSERT_NE(compiled, nullptr);
    ASSERT_TRUE(m_memory->protect_region(kData64, 0x4000, MemoryProtection::None));
    CpuState cpu = cpu64();
    cpu.gpr.rbx = kData64 + 0x10;
    cpu.gpr.rcx = 41;
    cpu.gpr.rax = 0xAAAAAAAAAAAAAAAAull;
    EXPECT_EQ(m_jit->execute(*compiled, cpu), JitExecResult::Continue);
    ASSERT_TRUE(cpu.pending_raise.active);
    EXPECT_EQ(cpu.pending_raise.code, 0xC0000005u);
    ASSERT_EQ(cpu.pending_raise.num_args, 2u);
    EXPECT_EQ(cpu.pending_raise.args[0], 0u) << "a read";
    EXPECT_EQ(cpu.pending_raise.args[1], kData64 + 0x10);
    EXPECT_EQ(cpu.rip, kCode64 + 4) << "the AV must name the MOV";
    EXPECT_EQ(cpu.gpr.rcx, 42u) << "the ADD before it retired";
    EXPECT_EQ(cpu.gpr.rax, 0xAAAAAAAAAAAAAAAAull) << "the faulting load wrote rax";
    // The flags are the ADD's: 41 + 1 = 42 sets none of CF/PF/AF/ZF/SF/OF
    // (42 = 0b00101010 has odd parity; no carry out of bit 3).
    cpu.materialize_lazy_flags();
    EXPECT_EQ(cpu.flags.rflags & 0x8D5u, 0u) << std::hex << cpu.flags.rflags;
}

TEST_F(JitLinearViewTest, LeanStoreToCodePageInvalidatesTheTranslation) {
    // Victim translation at kCode64; the storing block sits in another 4 KiB
    // guest page of the SAME host page, so only the code bit can tell.
    const std::vector<u8> victim = {0xB8, 0x11, 0x22, 0x33, 0x44, 0xC3};
    ASSERT_TRUE(m_memory->write_bytes(kCode64, victim.data(), victim.size()));
    BasicBlock vblock = build64(victim);
    ASSERT_NE(m_jit->compile(vblock), nullptr);
    m_cache->insert(vblock);
    ASSERT_EQ(m_cache->size(), 1u);
    const GuestAddr store_site = kCode64 + 0x2000;
    CompiledBlock* store = compile_at(bytes(kStoreRaxToRbx), store_site);
    ASSERT_NE(store, nullptr);
    EXPECT_TRUE(has_lean_access(words_of(*store)));
    CpuState cpu = cpu64();
    cpu.rip = store_site;
    cpu.gpr.rbx = kCode64;
    cpu.gpr.rax = 0x9090909090909090ull;
    (void)m_jit->execute(*store, cpu);
    EXPECT_FALSE(cpu.pending_raise.active);
    EXPECT_EQ(m_memory->read_u64(kCode64).value_or(0), 0x9090909090909090ull);
    EXPECT_EQ(m_cache->size(), 0u)
        << "a lean store over a translation skipped invalidation";
}

TEST_F(JitLinearViewTest, LeanStoreToWatchedPageIsReported) {
    std::array<u8, 8> dirty{};
    std::array<u8, 8> armed{};
    write_watch::register_region(kData64, 0x4000, dirty.data(), armed.data(),
                                 /*executable=*/false);
    CompiledBlock* compiled = compile_at(bytes(kStoreRaxToRbx));
    ASSERT_NE(compiled, nullptr);
    CpuState cpu = cpu64();
    cpu.gpr.rbx = kData64 + 0x1000;   // second 4 KiB guest page
    cpu.gpr.rax = 0x0BADC0DE0BADC0DEull;
    EXPECT_EQ(m_jit->execute(*compiled, cpu), JitExecResult::Continue);
    EXPECT_FALSE(cpu.pending_raise.active);
    const bool marked = write_watch::test_bit(dirty.data(), 1);
    write_watch::unregister_region(kData64);
    EXPECT_TRUE(marked) << "a lean store into a watched page was not recorded";
    EXPECT_EQ(m_memory->read_u64(kData64 + 0x1000).value_or(0), 0x0BADC0DE0BADC0DEull);
}

TEST_F(JitLinearViewTest, LeanLoadAboveTheWindowUsesTheRegionTable) {
    constexpr GuestAddr kHigh = 0x0000000300000000ull;  // 12 GiB > the 8 GiB window
    ASSERT_TRUE(m_memory->map_region(kHigh, 0x4000, MemoryProtection::ReadWrite));
    ASSERT_TRUE(m_memory->write_u64(kHigh + 0x20, 0xFEEDFACECAFEBEEFull));
    CompiledBlock* compiled = compile_at(bytes(kLoadRaxFromRbx));
    ASSERT_NE(compiled, nullptr);
    const u64 patches = lean_site_patch_count();
    CpuState cpu = cpu64();
    cpu.gpr.rbx = kHigh + 0x20;
    EXPECT_EQ(m_jit->execute(*compiled, cpu), JitExecResult::Continue);
    EXPECT_FALSE(cpu.pending_raise.active);
    EXPECT_EQ(cpu.gpr.rax, 0xFEEDFACECAFEBEEFull);
    EXPECT_EQ(lean_site_patch_count(), patches)
        << "an out-of-window EA takes the window branch, never a fault";
}

TEST_F(JitLinearViewTest, LeanLoadCrossingTwoRwPagesIsExact) {
    ASSERT_TRUE(m_memory->map_region(kData64 + 0x4000, 0x4000,
                                     MemoryProtection::ReadWrite));
    const u8 tail[8] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88};
    ASSERT_TRUE(m_memory->write_bytes(kData64 + 0x4000 - 4, tail, 8));
    CompiledBlock* compiled = compile_at(bytes(kLoadRaxFromRbx));
    ASSERT_NE(compiled, nullptr);
    CpuState cpu = cpu64();
    cpu.gpr.rbx = kData64 + 0x4000 - 4;
    EXPECT_EQ(m_jit->execute(*compiled, cpu), JitExecResult::Continue);
    EXPECT_FALSE(cpu.pending_raise.active);
    EXPECT_EQ(cpu.gpr.rax, 0x8877665544332211ull);
}

TEST_F(JitLinearViewTest, LeanStoreCrossingIntoNoAccessWritesNothing) {
    // x86 performs none of a faulting store (SDM Vol. 3A 6.15, #PF): the RW
    // page's last 4 bytes must survive the AV.
    ASSERT_TRUE(m_memory->map_region(kData64 + 0x4000, 0x4000,
                                     MemoryProtection::None));
    ASSERT_TRUE(m_memory->write_u32(kData64 + 0x4000 - 4, 0xC0FFEE00u));
    CompiledBlock* compiled = compile_at(bytes(kStoreRaxToRbx));
    ASSERT_NE(compiled, nullptr);
    CpuState cpu = cpu64();
    cpu.gpr.rbx = kData64 + 0x4000 - 4;
    cpu.gpr.rax = 0xFFFFFFFFFFFFFFFFull;
    EXPECT_EQ(m_jit->execute(*compiled, cpu), JitExecResult::Continue);
    ASSERT_TRUE(cpu.pending_raise.active);
    EXPECT_EQ(cpu.pending_raise.code, 0xC0000005u);
    ASSERT_EQ(cpu.pending_raise.num_args, 2u);
    EXPECT_EQ(cpu.pending_raise.args[0], 1u) << "a write";
    EXPECT_EQ(cpu.rip, kCode64);
    EXPECT_EQ(m_memory->read_u32(kData64 + 0x4000 - 4).value_or(0), 0xC0FFEE00u)
        << "the faulting store wrote part of itself";
}

TEST_F(JitLinearViewTest, ConcurrentFirstFaultsPatchOnce) {
    // Eight compilers share ONE translation through the compile memo, and
    // eight threads hit its lean site's first fault together.
    struct MemoOn {
        MemoOn() { JitCompileMemo::clear_compile_memo(); JitCompileMemo::enable_compile_memo(); }
        ~MemoOn() { JitCompileMemo::disable_compile_memo(); JitCompileMemo::clear_compile_memo(); }
    } memo_on;
    ASSERT_TRUE(m_memory->protect_region(kData64, 0x1000, MemoryProtection::Read));
    const std::vector<u8> code = bytes(kStoreRaxToRbx);
    ASSERT_TRUE(m_memory->write_bytes(kCode64, code.data(), code.size()));
    constexpr int kThreads = 8;
    std::vector<std::unique_ptr<JitCompiler>> jits;
    std::vector<CompiledBlock> blocks;
    for (int t = 0; t < kThreads; ++t) {
        jits.push_back(std::make_unique<JitCompiler>(*m_memory));
        CompiledBlock* b = jits.back()->compile(build64(code));
        ASSERT_NE(b, nullptr);
        blocks.push_back(*b);
    }
    for (int t = 1; t < kThreads; ++t) {
        ASSERT_EQ(blocks[t].code, blocks[0].code) << "the memo did not share code";
    }
    const u64 patches = lean_site_patch_count();
    std::atomic<int> wrong{0};
    std::atomic<bool> go{false};
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t] {
            while (!go.load(std::memory_order_acquire)) {}
            for (int i = 0; i < 10000; ++i) {
                CpuState cpu = cpu64();
                const GuestAddr slot = kData64 + 0x1000 + 8 * static_cast<GuestAddr>(t);
                cpu.gpr.rbx = slot;
                cpu.gpr.rax = (static_cast<u64>(t) << 32) | static_cast<u64>(i);
                CompiledBlock copy = blocks[t];
                if (jits[t]->execute(copy, cpu) != JitExecResult::Continue ||
                    cpu.pending_raise.active ||
                    m_memory->read_u64(slot).value_or(~0ull) != cpu.gpr.rax) {
                    wrong.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }
    go.store(true, std::memory_order_release);
    for (auto& th : threads) th.join();
    EXPECT_EQ(wrong.load(), 0);
    EXPECT_EQ(lean_site_patch_count(), patches + 1)
        << "concurrent first faults must patch the shared site exactly once";
}

// ---------------------------------------------------------------------------
// Lean vector sites (Task 4): one host page per access (a crossing takes the
// variant up front), so a refused access never performed any of itself.
// ---------------------------------------------------------------------------

TEST_F(JitLinearViewTest, LeanMovdquLoadReadsAUniformPage) {
    const std::vector<u8> code = {0xF3, 0x0F, 0x6F, 0x03};  // movdqu xmm0,[rbx]
    CompiledBlock* compiled = compile_at(code);
    ASSERT_NE(compiled, nullptr);
    EXPECT_TRUE(has_lean_vector_access(words_of(*compiled)))
        << "the 128-bit load still goes through the check map";
    u8 src[16];
    for (int i = 0; i < 16; ++i) src[i] = static_cast<u8>(i);
    ASSERT_TRUE(m_memory->write_bytes(kData64 + 0x100, src, 16));
    const u64 patches = lean_site_patch_count();
    CpuState cpu = cpu64();
    cpu.gpr.rbx = kData64 + 0x100;
    EXPECT_EQ(m_jit->execute(*compiled, cpu), JitExecResult::Continue);
    EXPECT_FALSE(cpu.pending_raise.active);
    for (int i = 0; i < 16; ++i) EXPECT_EQ(cpu.sse.xmm[0].u8_[i], i) << "byte " << i;
    EXPECT_EQ(lean_site_patch_count(), patches);
}

TEST_F(JitLinearViewTest, LeanMovdquStoreWritesAUniformPage) {
    const std::vector<u8> code = {0xF3, 0x0F, 0x7F, 0x03};  // movdqu [rbx],xmm0
    CompiledBlock* compiled = compile_at(code);
    ASSERT_NE(compiled, nullptr);
    EXPECT_TRUE(has_lean_vector_access(words_of(*compiled)));
    CpuState cpu = cpu64();
    cpu.gpr.rbx = kData64 + 0x204;  // unaligned, same host page
    for (int i = 0; i < 16; ++i) cpu.sse.xmm[0].u8_[i] = static_cast<u8>(0xF0 + i);
    EXPECT_EQ(m_jit->execute(*compiled, cpu), JitExecResult::Continue);
    EXPECT_FALSE(cpu.pending_raise.active);
    u8 out[16]{};
    ASSERT_TRUE(m_memory->read_bytes(kData64 + 0x204, out, 16));
    for (int i = 0; i < 16; ++i) EXPECT_EQ(out[i], 0xF0 + i) << "byte " << i;
}

TEST_F(JitLinearViewTest, LeanVectorStoreToCodePageInvalidates) {
    const std::vector<u8> victim = {0xB8, 0x11, 0x22, 0x33, 0x44, 0xC3};
    ASSERT_TRUE(m_memory->write_bytes(kCode64, victim.data(), victim.size()));
    BasicBlock vblock = build64(victim);
    ASSERT_NE(m_jit->compile(vblock), nullptr);
    m_cache->insert(vblock);
    ASSERT_EQ(m_cache->size(), 1u);
    const std::vector<u8> code = {0xF3, 0x0F, 0x7F, 0x03};  // movdqu [rbx],xmm0
    const GuestAddr store_site = kCode64 + 0x2000;
    CompiledBlock* store = compile_at(code, store_site);
    ASSERT_NE(store, nullptr);
    EXPECT_TRUE(has_lean_vector_access(words_of(*store)));
    CpuState cpu = cpu64();
    cpu.rip = store_site;
    cpu.gpr.rbx = kCode64;
    for (int i = 0; i < 16; ++i) cpu.sse.xmm[0].u8_[i] = 0x90;
    (void)m_jit->execute(*store, cpu);
    EXPECT_FALSE(cpu.pending_raise.active);
    EXPECT_EQ(m_memory->read_u64(kCode64).value_or(0), 0x9090909090909090ull);
    EXPECT_EQ(m_cache->size(), 0u)
        << "a lean vector store over a translation skipped invalidation";
}

TEST_F(JitLinearViewTest, LeanVectorStoreCrossingIntoNoAccessWritesNothing) {
    ASSERT_TRUE(m_memory->map_region(kData64 + 0x4000, 0x4000,
                                     MemoryProtection::None));
    ASSERT_TRUE(m_memory->write_u64(kData64 + 0x4000 - 8, 0x1122334455667788ull));
    const std::vector<u8> code = {0xF3, 0x0F, 0x7F, 0x03};  // movdqu [rbx],xmm0
    CompiledBlock* compiled = compile_at(code);
    ASSERT_NE(compiled, nullptr);
    CpuState cpu = cpu64();
    cpu.gpr.rbx = kData64 + 0x4000 - 8;
    for (int i = 0; i < 16; ++i) cpu.sse.xmm[0].u8_[i] = 0xEE;
    EXPECT_EQ(m_jit->execute(*compiled, cpu), JitExecResult::Continue);
    ASSERT_TRUE(cpu.pending_raise.active);
    EXPECT_EQ(cpu.pending_raise.code, 0xC0000005u);
    EXPECT_EQ(cpu.rip, kCode64);
    EXPECT_EQ(m_memory->read_u64(kData64 + 0x4000 - 8).value_or(0),
              0x1122334455667788ull)
        << "the faulting 16-byte store wrote its first half";
}

TEST_F(JitLinearViewTest, LeanVmovdquYmmLoadSpansTwoUniformPages) {
    ASSERT_TRUE(m_memory->map_region(kData64 + 0x4000, 0x4000,
                                     MemoryProtection::ReadWrite));
    u8 src[32];
    for (int i = 0; i < 32; ++i) src[i] = static_cast<u8>(0x40 + i);
    ASSERT_TRUE(m_memory->write_bytes(kData64 + 0x4000 - 16, src, 32));
    const std::vector<u8> code = {0xC5, 0xFE, 0x6F, 0x03};  // vmovdqu ymm0,[rbx]
    CompiledBlock* compiled = compile_at(code);
    ASSERT_NE(compiled, nullptr);
    CpuState cpu = cpu64();
    cpu.gpr.rbx = kData64 + 0x4000 - 16;
    EXPECT_EQ(m_jit->execute(*compiled, cpu), JitExecResult::Continue);
    EXPECT_FALSE(cpu.pending_raise.active);
    for (int i = 0; i < 16; ++i) {
        EXPECT_EQ(cpu.sse.xmm[0].u8_[i], 0x40 + i) << "low lane byte " << i;
        EXPECT_EQ(cpu.avx.ymm_high[0].u8_[i], 0x50 + i) << "high lane byte " << i;
    }
}

TEST_F(JitLinearViewTest, LeanVmovdquYmmLoadUsesTheView) {
    const std::vector<u8> code = {0xC5, 0xFE, 0x6F, 0x03};  // vmovdqu ymm0,[rbx]
    CompiledBlock* compiled = compile_at(code);
    ASSERT_NE(compiled, nullptr);
    EXPECT_TRUE(has_lean_vector_access(words_of(*compiled)))
        << "the 256-bit load still calls the out-of-line hot helper";
    u8 src[32];
    for (int i = 0; i < 32; ++i) src[i] = static_cast<u8>(0x80 + i);
    ASSERT_TRUE(m_memory->write_bytes(kData64 + 0x300, src, 32));
    CpuState cpu = cpu64();
    cpu.gpr.rbx = kData64 + 0x300;
    EXPECT_EQ(m_jit->execute(*compiled, cpu), JitExecResult::Continue);
    EXPECT_FALSE(cpu.pending_raise.active);
    for (int i = 0; i < 16; ++i) {
        EXPECT_EQ(cpu.sse.xmm[0].u8_[i], 0x80 + i);
        EXPECT_EQ(cpu.avx.ymm_high[0].u8_[i], 0x90 + i);
    }
}

// ---------------------------------------------------------------------------
// Review fix pass: the paths the first round left unexercised.
// ---------------------------------------------------------------------------

TEST_F(JitLinearViewTest, LeanMovdquLoadFromNoAccessRaisesAtItsRip) {
    // The direct LDR Q [X15, Xm] form (no ADD) faulting on a UNIFORM page:
    // the guard restart and V's own precise-RIP interval.
    const std::vector<u8> code = {0x48, 0x83, 0xC1, 0x01,          // add rcx,1
                                  0xF3, 0x0F, 0x6F, 0x03};         // movdqu xmm0,[rbx]
    CompiledBlock* compiled = compile_at(code);
    ASSERT_NE(compiled, nullptr);
    EXPECT_TRUE(has_lean_vector_access(words_of(*compiled)));
    ASSERT_TRUE(m_memory->protect_region(kData64, 0x4000, MemoryProtection::None));
    CpuState cpu = cpu64();
    cpu.gpr.rbx = kData64 + 0x40;
    cpu.gpr.rcx = 41;
    for (int i = 0; i < 16; ++i) cpu.sse.xmm[0].u8_[i] = 0x5A;
    EXPECT_EQ(m_jit->execute(*compiled, cpu), JitExecResult::Continue);
    ASSERT_TRUE(cpu.pending_raise.active);
    EXPECT_EQ(cpu.pending_raise.code, 0xC0000005u);
    ASSERT_EQ(cpu.pending_raise.num_args, 2u);
    EXPECT_EQ(cpu.pending_raise.args[0], 0u) << "a read";
    EXPECT_EQ(cpu.pending_raise.args[1], kData64 + 0x40);
    EXPECT_EQ(cpu.rip, kCode64 + 4) << "the AV must name the MOVDQU";
    EXPECT_EQ(cpu.gpr.rcx, 42u);
    for (int i = 0; i < 16; ++i) {
        EXPECT_EQ(cpu.sse.xmm[0].u8_[i], 0x5A) << "the faulting load wrote xmm0";
    }
}

TEST_F(JitLinearViewTest, LeanVmovdquYmmStoreWritesAUniformPage) {
    const std::vector<u8> code = {0xC5, 0xFE, 0x7F, 0x03};  // vmovdqu [rbx],ymm0
    CompiledBlock* compiled = compile_at(code);
    ASSERT_NE(compiled, nullptr);
    EXPECT_TRUE(has_lean_vector_access(words_of(*compiled)))
        << "the 256-bit store still calls the out-of-line hot helper";
    CpuState cpu = cpu64();
    cpu.gpr.rbx = kData64 + 0x404;  // unaligned, same host page
    for (int i = 0; i < 16; ++i) {
        cpu.sse.xmm[0].u8_[i] = static_cast<u8>(0x10 + i);
        cpu.avx.ymm_high[0].u8_[i] = static_cast<u8>(0x20 + i);
    }
    EXPECT_EQ(m_jit->execute(*compiled, cpu), JitExecResult::Continue);
    EXPECT_FALSE(cpu.pending_raise.active);
    u8 out[32]{};
    ASSERT_TRUE(m_memory->read_bytes(kData64 + 0x404, out, 32));
    for (int i = 0; i < 32; ++i) EXPECT_EQ(out[i], 0x10 + i) << "byte " << i;
}

TEST_F(JitLinearViewTest, LeanVmovdquYmmStoreToCodePageInvalidates) {
    const std::vector<u8> victim = {0xB8, 0x11, 0x22, 0x33, 0x44, 0xC3};
    ASSERT_TRUE(m_memory->write_bytes(kCode64, victim.data(), victim.size()));
    BasicBlock vblock = build64(victim);
    ASSERT_NE(m_jit->compile(vblock), nullptr);
    m_cache->insert(vblock);
    ASSERT_EQ(m_cache->size(), 1u);
    const std::vector<u8> code = {0xC5, 0xFE, 0x7F, 0x03};  // vmovdqu [rbx],ymm0
    const GuestAddr store_site = kCode64 + 0x2000;
    CompiledBlock* store = compile_at(code, store_site);
    ASSERT_NE(store, nullptr);
    CpuState cpu = cpu64();
    cpu.rip = store_site;
    cpu.gpr.rbx = kCode64;
    for (int i = 0; i < 16; ++i) {
        cpu.sse.xmm[0].u8_[i] = 0x90;
        cpu.avx.ymm_high[0].u8_[i] = 0x90;
    }
    (void)m_jit->execute(*store, cpu);
    EXPECT_FALSE(cpu.pending_raise.active);
    EXPECT_EQ(m_memory->read_u64(kCode64 + 24).value_or(0), 0x9090909090909090ull);
    EXPECT_EQ(m_cache->size(), 0u)
        << "a lean 256-bit store over a translation skipped invalidation";
}

TEST_F(JitLinearViewTest, LeanVmovdquYmmStoreCrossingIntoNoAccessWritesNothing) {
    ASSERT_TRUE(m_memory->map_region(kData64 + 0x4000, 0x4000,
                                     MemoryProtection::None));
    ASSERT_TRUE(m_memory->write_u64(kData64 + 0x4000 - 16, 0x0102030405060708ull));
    ASSERT_TRUE(m_memory->write_u64(kData64 + 0x4000 - 8, 0x1112131415161718ull));
    const std::vector<u8> code = {0xC5, 0xFE, 0x7F, 0x03};  // vmovdqu [rbx],ymm0
    CompiledBlock* compiled = compile_at(code);
    ASSERT_NE(compiled, nullptr);
    CpuState cpu = cpu64();
    cpu.gpr.rbx = kData64 + 0x4000 - 16;
    for (int i = 0; i < 16; ++i) {
        cpu.sse.xmm[0].u8_[i] = 0xEE;
        cpu.avx.ymm_high[0].u8_[i] = 0xEE;
    }
    EXPECT_EQ(m_jit->execute(*compiled, cpu), JitExecResult::Continue);
    ASSERT_TRUE(cpu.pending_raise.active);
    EXPECT_EQ(cpu.pending_raise.code, 0xC0000005u);
    EXPECT_EQ(cpu.pending_raise.args[0], 1u) << "a write";
    EXPECT_EQ(cpu.rip, kCode64);
    EXPECT_EQ(m_memory->read_u64(kData64 + 0x4000 - 16).value_or(0),
              0x0102030405060708ull) << "the faulting 32-byte store wrote its first piece";
    EXPECT_EQ(m_memory->read_u64(kData64 + 0x4000 - 8).value_or(0),
              0x1112131415161718ull);
}

TEST_F(JitLinearViewTest, PatchLogNamesTheGuestRipAndAddress) {
    // VITREO_JIT_VIEW_PATCH_LOG: one line per rewritten guard, naming the
    // guest instruction and the guest address the view refused -- what the
    // gate intersects with the hot-block weights.
    char path[] = "/tmp/vitreo-lean-patch-XXXXXX";
    const int fd = mkstemp(path);
    ASSERT_GE(fd, 0);
    close(fd);
    std::optional<std::string> prev_log;
    save("VITREO_JIT_VIEW_PATCH_LOG", prev_log);
    setenv("VITREO_JIT_VIEW_PATCH_LOG", path, 1);
    ASSERT_TRUE(m_memory->protect_region(kData64, 0x1000, MemoryProtection::Read));
    CompiledBlock* compiled = compile_at(bytes(kStoreRaxToRbx));
    ASSERT_NE(compiled, nullptr);
    CpuState cpu = cpu64();
    cpu.gpr.rbx = kData64 + 0x1008;
    cpu.gpr.rax = 7;
    EXPECT_EQ(m_jit->execute(*compiled, cpu), JitExecResult::Continue);
    restore("VITREO_JIT_VIEW_PATCH_LOG", prev_log);
    std::ifstream in(path);
    std::stringstream text;
    text << in.rdbuf();
    unlink(path);
    std::ostringstream want_rip, want_ea;
    want_rip << "rip=0x" << std::hex << kCode64;
    want_ea << "ea=0x" << std::hex << (kData64 + 0x1008);
    EXPECT_NE(text.str().find(want_rip.str()), std::string::npos) << text.str();
    EXPECT_NE(text.str().find(want_ea.str()), std::string::npos) << text.str();
}

}  // namespace
