// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
//
// Linear64 TSO ordered accesses that cross a 16-byte granule: the crossing
// sites, their backpatch and the mid-patch states. Split from
// test_jit_linear.cpp; shared fixtures and matchers live in
// jit_linear_fixture.hpp.

#include "jit_linear_fixture.hpp"

using namespace vitreo;
using namespace vitreo::cpu;
using namespace vitreo::cpu::jit;
using namespace vitreo::cpu::jit::test;

namespace {

TEST_F(JitLinear64Test, CrossingScalarLoadBackpatchesExactlyOnce) {
    ScopedEnvironmentFlag ordered("VITREO_JIT_TSO_ORDERED", "1");
    // The two-word [access, marker] site; the marker-free one has its own
    // tests (CrossingByFault*).
    ScopedEnvironmentFlag two_word("VITREO_JIT_TSO_CROSSING_FAULT", "0");
    m_jit = std::make_unique<JitCompiler>(*m_memory);
    m_jit->set_block_cache(m_cache.get());

    constexpr u64 value = 0x8877665544332211ull;
    const GuestAddr target = kData64 + 9;
    ASSERT_EQ(reinterpret_cast<uintptr_t>(m_memory->host_ptr(target, 8)) &
                  0xFu, 9u);
    ASSERT_TRUE(m_memory->write_u64(target, value));
    const std::vector<u8> code = {0x48, 0x8B, 0x03}; // mov rax,[rbx]
    ASSERT_TRUE(m_memory->write_bytes(kCode64, code.data(), code.size()));
    BasicBlock block = build64(code);
    CompiledBlock* compiled = m_jit->compile(block);
    ASSERT_NE(compiled, nullptr);

    u32* site = nullptr;
    auto* words = static_cast<u32*>(compiled->code);
    for (u32 i = 0; i + 1 < compiled->code_size / 4; ++i) {
        if (words[i + 1] == kTsoPatchMarker &&
            ((words[i] & 0x3FFFFC00u) == 0x38BFC000u ||
             (words[i] & 0x3FFFFC00u) == 0x08DFFC00u)) {
            site = &words[i];
            break;
        }
    }
    ASSERT_NE(site, nullptr);
    const u64 before = tso_backpatch_count();

    CpuState first = cpu64(target);
    ASSERT_EQ(test::execute_like_dispatcher(*m_jit, *compiled, first), JitExecResult::Continue);
    ASSERT_FALSE(first.pending_raise.active);
    EXPECT_EQ(first.gpr.rax, value);
    EXPECT_EQ(tso_backpatch_count(), before + 1);
    EXPECT_EQ(site[1], kDmbIsh);
    EXPECT_EQ(site[0], arm64::ldr_x_uoff(site[0] & 31u,
                                         (site[0] >> 5) & 31u, 0));

    CpuState second = cpu64(target);
    ASSERT_EQ(test::execute_like_dispatcher(*m_jit, *compiled, second), JitExecResult::Continue);
    EXPECT_EQ(second.gpr.rax, value);
    EXPECT_EQ(tso_backpatch_count(), before + 1);
}

TEST_F(JitLinear64Test, CrossingScalarStoreBackpatchesExactlyOnce) {
    ScopedEnvironmentFlag ordered("VITREO_JIT_TSO_ORDERED", "1");
    // The two-word [access, marker] site; the marker-free one has its own
    // tests (CrossingByFault*).
    ScopedEnvironmentFlag two_word("VITREO_JIT_TSO_CROSSING_FAULT", "0");
    m_jit = std::make_unique<JitCompiler>(*m_memory);
    m_jit->set_block_cache(m_cache.get());

    const GuestAddr target = kData64 + 9;
    ASSERT_EQ(reinterpret_cast<uintptr_t>(m_memory->host_ptr(target, 8)) &
                  0xFu, 9u);
    const std::vector<u8> code = {0x48, 0x89, 0x03}; // mov [rbx],rax
    ASSERT_TRUE(m_memory->write_bytes(kCode64, code.data(), code.size()));
    BasicBlock block = build64(code);
    CompiledBlock* compiled = m_jit->compile(block);
    ASSERT_NE(compiled, nullptr);

    u32* site = nullptr;
    auto* words = static_cast<u32*>(compiled->code);
    for (u32 i = 0; i + 1 < compiled->code_size / 4; ++i) {
        if (words[i] == kTsoPatchMarker &&
            (words[i + 1] & 0x3FFFFC00u) == 0x089FFC00u) {
            site = &words[i];
            break;
        }
    }
    ASSERT_NE(site, nullptr);
    const u32 ordered_store = site[1];
    const u8 value_reg = static_cast<u8>(ordered_store & 31u);
    const u8 address_reg = static_cast<u8>((ordered_store >> 5) & 31u);
    const u64 before = tso_backpatch_count();

    CpuState first = cpu64(target);
    first.gpr.rax = 0x0123456789ABCDEFull;
    ASSERT_EQ(test::execute_like_dispatcher(*m_jit, *compiled, first), JitExecResult::Continue);
    ASSERT_FALSE(first.pending_raise.active);
    EXPECT_EQ(tso_backpatch_count(), before + 1);
    EXPECT_EQ(site[0], kDmbIsh);
    EXPECT_EQ(site[1], arm64::str_x_uoff(value_reg, address_reg, 0));

    CpuState second = cpu64(target);
    second.gpr.rax = 0xFEDCBA9876543210ull;
    ASSERT_EQ(test::execute_like_dispatcher(*m_jit, *compiled, second), JitExecResult::Continue);
    EXPECT_EQ(tso_backpatch_count(), before + 1);
    auto stored = m_memory->read_u64(target);
    ASSERT_TRUE(stored.is_ok());
    EXPECT_EQ(stored.value(), second.gpr.rax);
}

// The marker-free site (linear_fault.hpp, kTsoCrossingVariantWords): no
// crossing test and no marker on the fast path; the ordered access's own
// alignment fault on a 16-byte-granule crossing redirects the site, once, to
// its crossing variant. An unaligned access inside one host page then runs the
// variant's fenced plain access.
TEST_F(JitLinear64Test, CrossingByFaultSitePatchesToItsVariantOnce) {
    if (const char* knob = std::getenv("VITREO_JIT_TSO_CROSSING_FAULT");
        knob != nullptr && knob[0] == '0') {
        GTEST_SKIP() << "VITREO_JIT_TSO_CROSSING_FAULT=0 emits the two-word site";
    }
    ScopedEnvironmentFlag ordered("VITREO_JIT_TSO_ORDERED", "1");
    m_jit = std::make_unique<JitCompiler>(*m_memory);
    m_jit->set_block_cache(m_cache.get());
    const GuestAddr target = kData64 + 9;  // crosses the granule at +16
    ASSERT_EQ(reinterpret_cast<uintptr_t>(m_memory->host_ptr(target, 8)) &
                  0xFu, 9u);
    struct Form {
        std::vector<u8> code;
        bool store;
    };
    const Form forms[] = {{{0x48, 0x8B, 0x03}, false},  // mov rax,[rbx]
                          {{0x48, 0x89, 0x03}, true}};  // mov [rbx],rax
    GuestAddr code_at = kCode64;
    for (const Form& f : forms) {
        SCOPED_TRACE(f.store ? "store" : "load");
        code_at += 0x40;
        ASSERT_TRUE(m_memory->write_bytes(code_at, f.code.data(), f.code.size()));
        BasicBlock block = build64(f.code);
        block.start_addr = code_at;
        CompiledBlock* compiled = m_jit->compile(block);
        ASSERT_NE(compiled, nullptr);
        // The site: [check-byte branch, ADD Xa, X14, Xm, ordered access].
        const u32* site = nullptr;
        for (const WordRange& range : block_word_ranges(*compiled)) {
            const u32* words = range.begin;
            const usize count = static_cast<usize>(range.end - range.begin);
            for (usize i = 2; i < count && site == nullptr; ++i) {
                const u32 family = words[i] & 0x3FFFFC00u;
                const bool ordered_access = f.store ? family == 0x089FFC00u
                                                    : family == 0x38BFC000u ||
                                                          family == 0x08DFFC00u;
                if (ordered_access && is_check_byte_branch(words[i - 2]) &&
                    (words[i - 1] & 0xFFE0FFE0u) ==
                        (arm64::add_x(0, REG_LINEAR_BASE, 0) & 0xFFE0FFE0u)) {
                    site = &words[i];
                }
            }
        }
        ASSERT_NE(site, nullptr) << "no marker-free ordered site";
        EXPECT_NE(site[1], kTsoPatchMarker);
        EXPECT_NE(site[-1], kTsoPatchMarker);
        const u32 ordered_access = site[0];
        const u64 before = tso_backpatch_count();

        constexpr u64 kOld = 0x8877665544332211ull;
        constexpr u64 kNew = 0x0123456789ABCDEFull;
        for (int run = 0; run < 2; ++run) {
            ASSERT_TRUE(m_memory->write_u64(target, kOld));
            CpuState cpu = cpu64(target);
            cpu.gpr.rax = kNew;
            ASSERT_EQ(test::execute_like_dispatcher(*m_jit, *compiled, cpu), JitExecResult::Continue);
            ASSERT_FALSE(cpu.pending_raise.active) << "run " << run;
            if (f.store) {
                EXPECT_EQ(m_memory->read_u64(target).value(), kNew);
            } else {
                EXPECT_EQ(cpu.gpr.rax, kOld);
            }
            // Patched on the first run only, to a B into the block's code.
            EXPECT_EQ(tso_backpatch_count(), before + 1) << "run " << run;
            EXPECT_NE(site[0], ordered_access);
            EXPECT_EQ(site[0] & 0xFC000000u, 0x14000000u);
        }
    }
}

// A crossing whose second page the guest denies: the ordered access faults on
// alignment before it touches memory, so a crossing store writes NOTHING
// (x86 performs no part of a faulting store), and the checked helper reports
// the first inaccessible byte (split_access_fault_address_probe.expected).
TEST_F(JitLinear64Test, CrossingByFaultIntoDeniedPageRaisesAtTheBoundary) {
    ScopedEnvironmentFlag ordered("VITREO_JIT_TSO_ORDERED", "1");
    m_jit = std::make_unique<JitCompiler>(*m_memory);
    m_jit->set_block_cache(m_cache.get());
    constexpr GuestAddr second = kData64 + 0x4000;
    ASSERT_TRUE(m_memory->map_region(second, 0x4000,
                                     MemoryProtection::None));
    const u8* map = m_memory->linear_check_page_map();
    ASSERT_NE(map, nullptr);
    ASSERT_EQ(map[kData64 >> kLinearHostPageShift], 0u);
    ASSERT_EQ(map[second >> kLinearHostPageShift], 0u)
        << "a uniformly denied host page is enforced by the hardware";
    struct Form {
        std::vector<u8> code;
        int width;
        bool store;
    };
    const Form forms[] = {
        {{0x66, 0x8B, 0x03}, 2, false}, {{0x8B, 0x03}, 4, false},
        {{0x48, 0x8B, 0x03}, 8, false}, {{0x66, 0x89, 0x03}, 2, true},
        {{0x89, 0x03}, 4, true},        {{0x48, 0x89, 0x03}, 8, true},
    };
    GuestAddr code_at = kCode64;
    for (const Form& f : forms) {
        for (int into = 1; into < f.width; ++into) {
            SCOPED_TRACE(::testing::Message() << "width=" << f.width
                                              << " store=" << f.store
                                              << " bytes past=" << into);
            const GuestAddr at = second - static_cast<GuestAddr>(f.width - into);
            ASSERT_TRUE(m_memory->write_u64(second - 8, 0x1122334455667788ull));
            code_at += 0x40;
            ASSERT_TRUE(m_memory->write_bytes(code_at, f.code.data(),
                                              f.code.size()));
            BasicBlock block = build64(f.code);
            block.start_addr = code_at;
            CompiledBlock* compiled = m_jit->compile(block);
            ASSERT_NE(compiled, nullptr);
            for (int run = 0; run < 2; ++run) {  // before and after the patch
                CpuState cpu = cpu64(at);
                cpu.rip = code_at;
                cpu.gpr.rax = 0xA5A5A5A5A5A5A5A5ull;
                EXPECT_EQ(test::execute_like_dispatcher(*m_jit, *compiled, cpu),
                          JitExecResult::Continue);
                ASSERT_TRUE(cpu.pending_raise.active) << "run " << run;
                EXPECT_EQ(cpu.pending_raise.code, 0xC0000005u);
                EXPECT_EQ(cpu.pending_raise.args[0], f.store ? 1u : 0u);
                EXPECT_EQ(cpu.pending_raise.args[1], second);
                EXPECT_EQ(cpu.gpr.rax, 0xA5A5A5A5A5A5A5A5ull);
                EXPECT_EQ(m_memory->read_u64(second - 8).value(),
                          0x1122334455667788ull)
                    << "a faulting crossing store wrote its first part";
            }
        }
    }
}

// RV There Yet r4 and Satisfactory r9 (2026-10-20): '[linear-fault] ADRALN
// raised as a guest AV: ... insn=140009dd ... meta=1 site=1' at a valid
// address, a crashed Foreground Worker. Two threads take a fresh crossing-by-
// fault site's alignment fault together; the first one's retry rewrites the
// site to `B variant` while the second is still classifying ITS fault --
// after the stale-fault test and the patched-site test have read the ordered
// access, before the site recognizer reads the word again and finds a B.
// Neither shape matched, and the fault, attributed to its instruction (meta=1
// site=1), became the guest's. The seam lands the site's own patch exactly
// there. The access that faulted no longer exists, so the thread must re-run
// the site through its variant and never raise; a crossing into unmapped
// memory, on which the variant's plain access faults for real, must still
// raise at the guest address.
namespace patch_while_classified {
std::atomic<uintptr_t> g_site{0};
std::atomic<u32> g_patched{0};
std::atomic<int> g_landed{0};
std::atomic<bool> g_attributed{false};
void land_patch(LinearFaultTestPoint point, uintptr_t pc, void*) {
    if (point != LinearFaultTestPoint::BeforePatchSite || pc != g_site.load() ||
        g_landed.fetch_add(1) != 0) {
        return;
    }
    JitFaultMetadata md{};
    g_attributed.store(jit_fault_metadata_for_pc(pc, md) && md.has_guest_rip);
    pthread_jit_write_protect_np(0);
    __atomic_store_n(reinterpret_cast<u32*>(pc), g_patched.load(), __ATOMIC_RELEASE);
    pthread_jit_write_protect_np(1);
    sys_icache_invalidate(reinterpret_cast<void*>(pc), 4);
}
}  // namespace patch_while_classified

TEST_F(JitLinear64Test, CrossingFaultWhoseSiteIsPatchedWhileClassifiedReRunsTheSite) {
    if (const char* knob = std::getenv("VITREO_JIT_TSO_CROSSING_FAULT");
        knob != nullptr && knob[0] == '0') {
        GTEST_SKIP() << "VITREO_JIT_TSO_CROSSING_FAULT=0 emits the two-word site";
    }
    namespace seam = patch_while_classified;
    ScopedEnvironmentFlag ordered("VITREO_JIT_TSO_ORDERED", "1");
    m_jit = std::make_unique<JitCompiler>(*m_memory);
    m_jit->set_block_cache(m_cache.get());
    // 8 bytes at +12 cross the granule at +16 (the logged guest=..3c).
    const GuestAddr mapped = kData64 + 12;
    const GuestAddr unmapped = kUnmapped64 + 12;
    ASSERT_EQ(reinterpret_cast<uintptr_t>(m_memory->host_ptr(mapped, 8)) & 0xFu,
              12u);
    struct Form {
        std::vector<u8> code;
        bool store;
    };
    const Form forms[] = {{{0x48, 0x8B, 0x03}, false},  // mov rax,[rbx]
                          {{0x48, 0x89, 0x03}, true}};  // mov [rbx],rax
    constexpr u64 kOld = 0x8877665544332211ull;
    constexpr u64 kNew = 0x0123456789ABCDEFull;
    constexpr u64 kUntouched = 0xA5A5A5A5A5A5A5A5ull;
    GuestAddr code_at = kCode64;
    for (const Form& f : forms) {
        for (const GuestAddr target : {mapped, unmapped}) {
            SCOPED_TRACE(::testing::Message()
                         << (f.store ? "store" : "load") << " to "
                         << (target == mapped ? "mapped" : "unmapped"));
            code_at += 0x40;
            ASSERT_TRUE(m_memory->write_bytes(code_at, f.code.data(), f.code.size()));
            BasicBlock block = build64(f.code);
            block.start_addr = code_at;
            CompiledBlock* compiled = m_jit->compile(block);
            ASSERT_NE(compiled, nullptr);
            // The site: [check-byte branch, ADD Xa, X14, Xm, ordered access].
            u32* site = nullptr;
            for (const WordRange& range : block_word_ranges(*compiled)) {
                const u32* words = range.begin;
                const usize count = static_cast<usize>(range.end - range.begin);
                for (usize i = 2; i < count && site == nullptr; ++i) {
                    const u32 family = words[i] & 0x3FFFFC00u;
                    const bool ordered_access = f.store ? family == 0x089FFC00u
                                                        : family == 0x38BFC000u ||
                                                              family == 0x08DFFC00u;
                    if (ordered_access && is_check_byte_branch(words[i - 2]) &&
                        (words[i - 1] & 0xFFE0FFE0u) ==
                            (arm64::add_x(0, REG_LINEAR_BASE, 0) & 0xFFE0FFE0u)) {
                        site = const_cast<u32*>(&words[i]);
                    }
                }
            }
            ASSERT_NE(site, nullptr) << "no marker-free ordered site";
            const u32 ordered_access = site[0];
            // What the other thread's retry writes: the site's own patch, taken
            // from a crossing run, after which the site is made fresh again.
            {
                CpuState cpu = cpu64(mapped);
                cpu.rip = code_at;
                ASSERT_EQ(test::execute_like_dispatcher(*m_jit, *compiled, cpu),
                          JitExecResult::Continue);
                ASSERT_FALSE(cpu.pending_raise.active);
            }
            const u32 patched = site[0];
            ASSERT_EQ(patched & 0xFC000000u, 0x14000000u) << "not patched to B variant";
            pthread_jit_write_protect_np(0);
            __atomic_store_n(site, ordered_access, __ATOMIC_RELEASE);
            pthread_jit_write_protect_np(1);
            sys_icache_invalidate(site, 4);

            ASSERT_TRUE(m_memory->write_u64(mapped, kOld));
            seam::g_site.store(reinterpret_cast<uintptr_t>(site));
            seam::g_patched.store(patched);
            seam::g_landed.store(0);
            seam::g_attributed.store(false);
            set_linear_fault_test_hook_for_test(&seam::land_patch);
            CpuState cpu = cpu64(target);
            cpu.rip = code_at;
            cpu.gpr.rax = f.store ? kNew : kUntouched;
            ::testing::internal::CaptureStderr();
            const JitExecResult r = test::execute_like_dispatcher(*m_jit, *compiled, cpu);
            const std::string handler_log = ::testing::internal::GetCapturedStderr();
            set_linear_fault_test_hook_for_test(nullptr);
            EXPECT_EQ(r, JitExecResult::Continue);
            ASSERT_EQ(seam::g_landed.load(), 1)
                << "the ordered access never alignment-faulted into the seam";
            EXPECT_TRUE(seam::g_attributed.load())
                << "the fault must be attributed to its instruction (meta=1 site=1)";
            EXPECT_EQ(site[0], patched);
            if (target == mapped) {
                EXPECT_FALSE(cpu.pending_raise.active)
                    << "raised 0x" << std::hex << cpu.pending_raise.code
                    << " at a valid address";
                if (f.store) {
                    EXPECT_EQ(m_memory->read_u64(mapped).value_or(0), kNew);
                } else {
                    EXPECT_EQ(cpu.gpr.rax, kOld);
                }
            } else {
                ASSERT_TRUE(cpu.pending_raise.active);
                EXPECT_EQ(cpu.pending_raise.code, 0xC0000005u);
                EXPECT_EQ(cpu.pending_raise.args[0], f.store ? 1u : 0u);
                EXPECT_EQ(cpu.pending_raise.args[1], unmapped);
                EXPECT_EQ(cpu.rip, code_at);
                EXPECT_EQ(cpu.gpr.rax, f.store ? kNew : kUntouched);
                // The variant's plain access took a PERMISSION fault on the
                // reservation's PROT_NONE. XNU reports every SIGBUS as
                // BUS_ADRALN, so only the saved ESR can say it was not an
                // alignment fault, and the handler must not call it one.
                EXPECT_EQ(handler_log.find("raised as a guest AV"), std::string::npos)
                    << handler_log;
            }
        }
    }
}

// A crossing out of a MIXED first host page into a MIXED second one. The
// check-byte branch now carries crossings too, so the sub-page entry must
// refuse them: deciding by the first host page's sub-page byte alone would
// read the LAST byte's guest-page index out of the wrong host page. Host page
// 1's guest page 0 is RW (and 1 NOACCESS, which makes it mixed); host page
// 2's guest page 0 is NOACCESS, but page 2 is mixed and so readable and
// writable in hardware. A sub-page entry that indexed the last byte within
// page 1 would find RW and perform an access that writes into, or reads, a
// denied guest page with nothing to stop it.
TEST_F(JitLinear64Test, CrossingByFaultOutOfMixedPageTakesTheHelper) {
    ScopedEnvironmentFlag ordered("VITREO_JIT_TSO_ORDERED", "1");
    m_jit = std::make_unique<JitCompiler>(*m_memory);
    m_jit->set_block_cache(m_cache.get());
    constexpr GuestAddr second = kData64 + 0x4000;
    ASSERT_TRUE(m_memory->protect_region(kData64 + 0x1000, 0x1000,
                                         MemoryProtection::None));
    ASSERT_TRUE(m_memory->map_region(second, 0x4000,
                                     MemoryProtection::ReadWrite));
    ASSERT_TRUE(m_memory->write_u64(second + 0x1000, 0));  // a live RW page
    ASSERT_TRUE(m_memory->protect_region(second, 0x1000,
                                         MemoryProtection::None));
    const u8* map = m_memory->linear_check_page_map();
    ASSERT_NE(map[kData64 >> kLinearHostPageShift] & kLinearCheckMixedRead, 0u);
    ASSERT_NE(map[second >> kLinearHostPageShift] & kLinearCheckMixedRead, 0u);
    const std::vector<std::vector<u8>> forms = {
        {0x48, 0x8B, 0x03},  // mov rax,[rbx]
        {0x48, 0x89, 0x03},  // mov [rbx],rax
    };
    GuestAddr code_at = kCode64;
    for (const auto& code : forms) {
        const bool store = code[1] == 0x89;
        SCOPED_TRACE(store ? "store" : "load");
        code_at += 0x40;
        ASSERT_TRUE(m_memory->write_bytes(code_at, code.data(), code.size()));
        BasicBlock block = build64(code);
        block.start_addr = code_at;
        CompiledBlock* compiled = m_jit->compile(block);
        ASSERT_NE(compiled, nullptr);
        ASSERT_TRUE(m_memory->write_u64(second - 8, 0x1122334455667788ull));
        CpuState cpu = cpu64(second - 4);
        cpu.rip = code_at;
        cpu.gpr.rax = 0xA5A5A5A5A5A5A5A5ull;
        EXPECT_EQ(test::execute_like_dispatcher(*m_jit, *compiled, cpu), JitExecResult::Continue);
        ASSERT_TRUE(cpu.pending_raise.active);
        EXPECT_EQ(cpu.pending_raise.code, 0xC0000005u);
        EXPECT_EQ(cpu.pending_raise.args[0], store ? 1u : 0u);
        EXPECT_EQ(cpu.pending_raise.args[1], second);
        EXPECT_EQ(cpu.gpr.rax, 0xA5A5A5A5A5A5A5A5ull);
        EXPECT_EQ(m_memory->read_u64(second - 8).value(), 0x1122334455667788ull);
        // The denied guest page itself: what host access would have written.
        // Mixed, so readable in hardware through the reservation.
        ASSERT_NE(m_memory->linear_base(), nullptr);
        const u8* host = m_memory->linear_base() + second;
        u32 denied = 0;
        std::memcpy(&denied, host, sizeof(denied));
        EXPECT_EQ(denied, 0u) << "a crossing store wrote a denied guest page";
    }
}

// The two-word TSO patch is not atomic: [marker, STLR] -> [DMB, STLR] ->
// [DMB, STR] in two separate CASes. A SECOND thread executing the same hot
// site with a granule-crossing address inside that window takes its alignment
// SIGBUS against the half-patched pair, and a recognizer that accepts only
// the marker sends exactly that fault down the guest-ACCESS_VIOLATION path.
// Measured live: CEF's GPU and network-service processes crash-looped on
// 0xc0000005 at stable hot sites, every faulting address straddling a 16-byte
// granule — the original "73s vs 41s" Steam regression. These tests put the
// site into the mid-patch state BY HAND — precisely thread B's world — and
// require the access to succeed and the patch to complete.
TEST_F(JitLinear64Test, CrossingStoreInMidPatchStateStillSucceeds) {
    ScopedEnvironmentFlag ordered("VITREO_JIT_TSO_ORDERED", "1");
    ScopedEnvironmentFlag two_word("VITREO_JIT_TSO_CROSSING_FAULT", "0");
    m_jit = std::make_unique<JitCompiler>(*m_memory);
    m_jit->set_block_cache(m_cache.get());

    const GuestAddr target = kData64 + 12;   // 8-byte store crosses at +12
    ASSERT_EQ(reinterpret_cast<uintptr_t>(m_memory->host_ptr(target, 8)) &
                  0xFu, 12u);
    const std::vector<u8> code = {0x48, 0x89, 0x03};  // mov [rbx],rax
    ASSERT_TRUE(m_memory->write_bytes(kCode64, code.data(), code.size()));
    CompiledBlock* compiled = m_jit->compile(build64(code));
    ASSERT_NE(compiled, nullptr);

    // Find the [marker, STLR] pair and advance it to the mid-patch state.
    u32* site = nullptr;
    auto* words = static_cast<u32*>(compiled->code);
    for (u32 i = 0; i + 1 < compiled->code_size / 4; ++i) {
        if (words[i] == kTsoPatchMarker &&
            (words[i + 1] & 0x3FFFFC00u) == 0x089FFC00u) {
            site = &words[i];
            break;
        }
    }
    ASSERT_NE(site, nullptr) << "no [marker, STLR] site in the store block";
    pthread_jit_write_protect_np(0);
    site[0] = kDmbIsh;                       // thread A got exactly this far
    pthread_jit_write_protect_np(1);
    sys_icache_invalidate(site, 8);

    CpuState cpu = cpu64(target);
    cpu.gpr.set64(EAX, 0x1122334455667788ull);
    ASSERT_EQ(test::execute_like_dispatcher(*m_jit, *compiled, cpu), JitExecResult::Continue);
    EXPECT_FALSE(cpu.pending_raise.active)
        << "a crossing store at a half-patched site raised a bogus AV";
    EXPECT_EQ(m_memory->read_u64(target).value_or(0), 0x1122334455667788ull);
    // And the patch is COMPLETE afterwards, not merely survived.
    EXPECT_EQ(site[0], kDmbIsh);
    EXPECT_EQ((site[1] & 0x3FC00000u) != 0u ||
                  (site[1] & 0x3FFFFC00u) != 0x089FFC00u, true)
        << "the STLR was not replaced by a plain store";
}

TEST_F(JitLinear64Test, CrossingLoadInMidPatchStateStillSucceeds) {
    ScopedEnvironmentFlag ordered("VITREO_JIT_TSO_ORDERED", "1");
    // The two-word [access, marker] site; the marker-free one has its own
    // tests (CrossingByFault*).
    ScopedEnvironmentFlag two_word("VITREO_JIT_TSO_CROSSING_FAULT", "0");
    m_jit = std::make_unique<JitCompiler>(*m_memory);
    m_jit->set_block_cache(m_cache.get());

    constexpr u64 value = 0x8877665544332211ull;
    const GuestAddr target = kData64 + 9;
    ASSERT_TRUE(m_memory->write_u64(target, value));
    const std::vector<u8> code = {0x48, 0x8B, 0x03};  // mov rax,[rbx]
    ASSERT_TRUE(m_memory->write_bytes(kCode64, code.data(), code.size()));
    CompiledBlock* compiled = m_jit->compile(build64(code));
    ASSERT_NE(compiled, nullptr);

    u32* site = nullptr;
    auto* words = static_cast<u32*>(compiled->code);
    for (u32 i = 0; i + 1 < compiled->code_size / 4; ++i) {
        if (words[i + 1] == kTsoPatchMarker &&
            ((words[i] & 0x3FFFFC00u) == 0x38BFC000u ||
             (words[i] & 0x3FFFFC00u) == 0x08DFFC00u)) {
            site = &words[i];
            break;
        }
    }
    ASSERT_NE(site, nullptr);
    pthread_jit_write_protect_np(0);
    site[1] = kDmbIsh;                       // load order: marker goes first
    pthread_jit_write_protect_np(1);
    sys_icache_invalidate(site, 8);

    CpuState cpu = cpu64(target);
    ASSERT_EQ(test::execute_like_dispatcher(*m_jit, *compiled, cpu), JitExecResult::Continue);
    EXPECT_FALSE(cpu.pending_raise.active)
        << "a crossing load at a half-patched site raised a bogus AV";
    EXPECT_EQ(cpu.gpr.rax, value);
}

}  // namespace
