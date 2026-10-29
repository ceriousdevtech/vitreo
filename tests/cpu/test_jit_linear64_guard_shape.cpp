// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
//
// The emitted shape and branch reach of linear64 access guards. Split from
// test_jit_linear.cpp; shared fixtures and matchers live in
// jit_linear_fixture.hpp.

#include "jit_linear_fixture.hpp"

using namespace vitreo;
using namespace vitreo::cpu;
using namespace vitreo::cpu::jit;
using namespace vitreo::cpu::jit::test;

namespace {

// Intel SDM Vol. 2 VMOVDQU/VMOVDQA (VEX.128): the same 16-byte move as the
// legacy form plus a zeroed YMM high lane. Chromium's memmove is sixteen of
// these per iteration, and on Steam's renderer each one was lowered as two
// guarded 64-bit scalar accesses (67 ARM words a load, 102 a store) although
// the single guarded 16-byte path exists for the legacy encoding.
TEST_F(JitLinear64Test, LegacyMovdquWithoutRangeProofTakesTheSingle16BytePath) {
    const std::vector<u8> code = {
        0xF3, 0x0F, 0x6F, 0x0A,  // movdqu xmm1,[rdx]
        0x66, 0x0F, 0x7F, 0x09,  // movdqa [rcx],xmm1
    };
    ASSERT_TRUE(m_memory->write_bytes(kCode64, code.data(), code.size()));
    BasicBlock block = build64(code);
    CompiledBlock* compiled = m_jit->compile(block);
    ASSERT_NE(compiled, nullptr);
    const auto* words = static_cast<const u32*>(compiled->code);
    const usize n = compiled->code_size / sizeof(u32);
    EXPECT_EQ(std::count_if(words, words + n,
                            [](u32 w) { return is_linear_q_access(w, false); }),
              1);
    const std::vector<u32> emitted(words, words + n);
    // Each access carries its own inline reservation test
    // (emit_linear64_vec128_checked).
    EXPECT_EQ(count_inline_linear64_reservation_checks(emitted), 2u)
        << "each unproven m128 access must test the reservation";
    EXPECT_EQ(std::count_if(words, words + n, [](u32 w) {
        return (w & 0xFFFFFC00u) == 0xF8BFC000u;   // LDAPR X
    }), 0);
    EXPECT_GE(std::count(words, words + n, kDmbIsh), 2)
        << "x86 TSO requires an acquire fence after the vector load and a "
           "release fence before the vector store";
}

TEST_F(JitLinear64Test, Vex256MovdquLoadAndVmovdqaStoreTakeTheSingle32BytePath) {
    const std::vector<u8> code = {
        0xC5, 0xFE, 0x6F, 0x0A,  // vmovdqu ymm1,[rdx]   (VEX.256, L=1)
        0xC5, 0xFD, 0x7F, 0x09,  // vmovdqa [rcx],ymm1
    };
    ASSERT_TRUE(m_memory->write_bytes(kCode64, code.data(), code.size()));
    BasicBlock block = build64(code);
    ASSERT_EQ(block.instruction_count, 2u);
    CompiledBlock* compiled = m_jit->compile(block);
    ASSERT_NE(compiled, nullptr);
    const auto* words = static_cast<const u32*>(compiled->code);
    const usize n = compiled->code_size / sizeof(u32);
    EXPECT_EQ(std::count(words, words + n, arm64::ldp_q_off(0, 1, 17, 0)), 1)
        << "the VEX.256 load must be one direct 32-byte LDP";
    EXPECT_EQ(std::count(words, words + n, arm64::stp_q_off(0, 1, 17, 0)), 1)
        << "the VEX.256 store must be one direct 32-byte STP";
    const std::vector<u32> emitted256(words, words + n);
    EXPECT_TRUE(has_linear64_hot_helper_call(emitted256, 32))
        << "unproven m256 load must use the process-wide reservation helper";
    EXPECT_TRUE(has_linear64_hot_helper_call(emitted256, 32, true))
        << "unproven m256 store must use the process-wide reservation helper";
    // No guarded scalar 64-bit access may remain: LDAPR Xt (the ordered
    // load the split path uses) and STLR Xt.
    EXPECT_EQ(std::count_if(words, words + n, [](u32 w) {
        return (w & 0xFFFFFC00u) == 0xF8BFC000u;   // LDAPR X
    }), 0) << "split 64-bit scalar load emitted for a VEX.128 move";
    EXPECT_EQ(std::count_if(words, words + n, [](u32 w) {
        return (w & 0xFFFFFC00u) == 0xC89FFC00u;   // STLR X
    }), 0) << "split 64-bit scalar store emitted for a VEX.128 move";

    // Semantics: all 32 bytes move through ymm1 (both lanes).
    std::array<u8, 32> bytes{};
    for (usize i = 0; i < bytes.size(); ++i) bytes[i] = static_cast<u8>(0xA0u + i);
    ASSERT_TRUE(m_memory->write_bytes(kData64, bytes.data(), bytes.size()));
    CpuState jcpu = cpu64(kData64);
    jcpu.gpr.rdx = kData64;
    jcpu.gpr.rcx = kData64 + 0x100;
    EXPECT_EQ(test::execute_like_dispatcher(*m_jit, *compiled, jcpu), JitExecResult::Continue);
    ASSERT_FALSE(jcpu.pending_raise.active);
    std::array<u8, 32> out{};
    ASSERT_TRUE(m_memory->read_bytes(kData64 + 0x100, out.data(), out.size()).is_ok());
    EXPECT_EQ(out, bytes);
}

TEST_F(JitLinear64Test, ScalarDoubleLoadCrossesAdjacentGuestRegions) {
    constexpr GuestAddr target = kData64 + 0x4000 - 4;
    ASSERT_TRUE(m_memory->map_region(kData64 + 0x4000, 0x4000,
                                     MemoryProtection::ReadWrite));
    ASSERT_TRUE(m_memory->write_u64(target, 0x400921FB54442D18ULL));
    // movsd xmm0,[rbx]; movq rax,xmm0
    const std::vector<u8> code = {0xF2, 0x0F, 0x10, 0x03,
                                  0x66, 0x48, 0x0F, 0x7E, 0xC0};
    ASSERT_TRUE(m_memory->write_bytes(kCode64, code.data(), code.size()));
    CompiledBlock* compiled = m_jit->compile(build64(code));
    ASSERT_NE(compiled, nullptr);
    CpuState cpu = cpu64(target);
    EXPECT_EQ(test::execute_like_dispatcher(*m_jit, *compiled, cpu), JitExecResult::Continue);
    EXPECT_FALSE(cpu.pending_raise.active);
    EXPECT_EQ(cpu.gpr.rax, 0x400921FB54442D18ULL);
}

TEST_F(JitLinear64Test, DirectLoadCarriesPermissionAndLiveProtectGuards) {
    const std::vector<u8> code = {0x8B, 0x03}; // mov eax,[rbx]
    ASSERT_TRUE(m_memory->write_bytes(kCode64, code.data(), code.size()));
    BasicBlock block = build64(code);
    CompiledBlock* linear = m_jit->compile(block);
    ASSERT_NE(linear, nullptr);
    const u32 linear_words = linear->code_size / 4;
    std::vector<u32> emitted(linear_words);
    std::memcpy(emitted.data(), linear->code, linear->code_size);

    EXPECT_TRUE(has_guarded_linear64_load32(emitted))
        << "no exact-address ordered load with guarded crossing fallback";
    EXPECT_FALSE(has_linear64_hot_helper_call(emitted, 4));
    const bool range_by_fault = count_range_fault_sites(emitted) != 0;
    EXPECT_TRUE(range_by_fault ||
                std::any_of(emitted.begin(), emitted.end(), [](u32 word) {
        return (word & 0xFF00001Fu) == 0xB5000008u; // CBNZ X8,cold
    })) << "linear64 range check does not branch cold with CBNZ X8";
    // A host-page crossing is diverted before the single-page check byte is
    // trusted: by the ordered load's own alignment fault on the marker-free
    // site (linear_fault.hpp, the default), or by the inline crossing bit
    // under VITREO_JIT_TSO_CROSSING_FAULT=0.
    const char* crossing_knob = std::getenv("VITREO_JIT_TSO_CROSSING_FAULT");
    const bool crossing_by_fault =
        crossing_knob == nullptr || crossing_knob[0] != '0';
    if (crossing_by_fault) {
        bool marker_free_site = false;
        for (usize i = 1; i + 1 < emitted.size(); ++i) {
            if (is_check_byte_branch(emitted[i - 1]) &&
                (emitted[i] & 0xFFE0FFFFu) ==
                    (arm64::add_x(17, REG_LINEAR_BASE, 0) & 0xFFE0FFFFu) &&
                is_acquire_load32(emitted[i + 1], 17)) {
                marker_free_site = true;
            }
        }
        EXPECT_TRUE(marker_free_site)
            << "linear64 load is not the marker-free ordered site";
        EXPECT_EQ(std::count(emitted.begin(), emitted.end(), kTsoPatchMarker), 0)
            << "a crossing-by-fault load still carries the patch marker";
    } else
    EXPECT_TRUE([&] {
        // EOR X8, ea, X17 ; UBFX X8, X8, #14, #1 ; ORR X8, X8, ea, LSR #bits ;
        // CBNZ X8 -- the crossing bit and the reservation test share one
        // branch, which is taken before the single-page check byte is read.
        for (usize i = 0; i + 3 < emitted.size(); ++i) {
            const u32 eor_mask = ~(u32{0x1F} << 5);
            if ((emitted[i] & eor_mask) !=
                (arm64::eor_x(8, 0, 17) & eor_mask)) continue;
            if (emitted[i + 1] == arm64::ubfx_x(
                    8, 8, static_cast<u8>(kLinearHostPageShift), 1) &&
                (emitted[i + 2] & 0xFFE003FFu) == 0xAA400108u &&
                (emitted[i + 3] & 0xFF00001Fu) == 0xB5000008u) {
                return true;
            }
        }
        return false;
    }()) << "linear64 load does not divert a host-page crossing before its "
            "single-page permission check";
    // With hardware enforcement the per-access permission work is ONE byte
    // load off the mixed-page map plus a CBNZ: the 16 KiB host page states the
    // guest's protection exactly wherever its 4 KiB guest pages agree, and only
    // a disagreeing ("mixed") page still needs the software check.
    // Off the PINNED map base (REG_LINEAR64_CHECK_MAP), not a JitContext
    // reload: that reload was one more dependent load on the address chain of
    // every access, and x15 became free when the global write-watch probe
    // turned into kLinearCheckWatched.
    // The index is formed in X3 itself, so an EA left in X4 needs no copy.
    EXPECT_EQ(static_cast<usize>(std::count(
                  emitted.begin(), emitted.end(),
                  arm64::ldrb_reg(3, REG_LINEAR64_CHECK_MAP, 3))),
              1u)
        << "linear64 load does not read exactly one check byte off the pinned "
           "map";
    EXPECT_EQ(std::find(emitted.begin(), emitted.end(),
                        arm64::ldrb_reg(8, REG_LINEAR64_CHECK_MAP, 4)),
              emitted.end())
        << "linear64 load still reads the first endpoint's check byte";
    EXPECT_EQ(std::find(emitted.begin(), emitted.end(),
                        arm64::ldr_x_uoff(3, REG_JIT_CTX,
                                          JIT_CTX_OFF_LINEAR_CHECK_MAP >> 3)),
              emitted.end())
        << "linear64 load still reloads the check-map base per access";
    // A load tests ONE bit, not the whole byte: a host page that merely holds
    // translated code is safe to READ directly, and making loads take the
    // helper there would tax every constant pool inside .text.
    //
    // And the bit it tests is kLinearCheckMixedRead, not kLinearCheckMixedProt.
    // "Mixed" means the 4 KiB guest pages inside disagree, and they almost
    // always disagree about WRITABILITY -- a PE's 4 KiB SectionAlignment puts
    // .rdata beside .data in one 16 KiB host page as a matter of course. Those
    // pages are uniformly READABLE, so gating loads on the write-oriented bit
    // sent 686,080,048 harmless reads to the helper on sc_blocksize rung 1,
    // 100% of that workload's helper reads. The read bit is set only when
    // hardware could actually admit a read the guest denies.
    EXPECT_TRUE(std::any_of(emitted.begin(), emitted.end(), [](u32 word) {
        return (word & 0xFFF8001Fu) ==
               (arm64::tbnz(3, kLinearCheckMixedReadBit, 0) & 0xFFF8001Fu);
    })) << "linear64 load does not branch cold on a read-unsafe host page";
    EXPECT_FALSE(std::any_of(emitted.begin(), emitted.end(), [](u32 word) {
        return (word & 0xFFF8001Fu) ==
               (arm64::tbnz(3, kLinearCheckMixedProtBit, 0) & 0xFFF8001Fu);
    })) << "linear64 load still gates on the WRITE-oriented mixed bit, which "
           "sends every read on a .rdata/.data host page to the helper";

    // Direct loads must not pay for the cold scattered/mixed-page cache probe.
    // The first branch to the cold path: the reservation CBNZ X8, or under
    // range-by-fault the check-byte TBNZ W3.
    const auto range_branch = std::find_if(emitted.begin(), emitted.end(),
                                           [range_by_fault](u32 word) {
        return range_by_fault
            ? (word & 0xFFF8001Fu) ==
                  (arm64::tbnz(3, kLinearCheckMixedReadBit, 0) & 0xFFF8001Fu)
            : (word & 0xFF00001Fu) == 0xB5000008u;
    });
    ASSERT_NE(range_branch, emitted.end());
    const i32 displacement = range_by_fault
        ? static_cast<i32>((*range_branch >> 5) & 0x3FFFu)
        : static_cast<i32>((*range_branch >> 5) & 0x7FFFFu);
    ASSERT_GT(displacement, 0);
    // Placed contiguously the cold body follows inside `emitted`; placed
    // split it is the block's cold part, and all of `emitted` precedes it.
    if (displacement > emitted.end() - range_branch) {
        ASSERT_NE(linear->cold_code(), nullptr);
        const auto* target = static_cast<const u32*>(linear->code) +
                             (range_branch - emitted.begin()) + displacement;
        const auto* cold = reinterpret_cast<const u32*>(linear->cold_code());
        ASSERT_GE(target, cold);
        ASSERT_LT(target, cold + linear->cold_size / 4);
    }
    const auto cold_begin =
        range_branch + std::min<std::ptrdiff_t>(displacement,
                                                emitted.end() - range_branch);
    EXPECT_EQ(std::find(emitted.begin(), cold_begin,
                        arm64::ldr_x_uoff(3, REG_JIT_CTX,
                                          JIT_CTX_OFF_PROTECT_SEQUENCE_LIVE >> 3)),
              cold_begin)
        << "hardware-protected linear64 load still reads the protect generation";
    EXPECT_EQ(std::find(emitted.begin(), cold_begin,
                        arm64::ldr_x_uoff(3, REG_JIT_CTX,
                                          JIT_CTX_OFF_X64_RSLOTS >> 3)),
              cold_begin)
        << "hardware-protected linear64 load still probes the region slots";
}

TEST_F(JitLinear64Test, WidenedLoadsUseLongRangeCrossingBranches) {
    // A maximal widened block appends thousands of words of cold memory
    // stubs. UBFX+CBNZ preserves the crossing predicate and gives every load
    // imm19 reach; a TBNZ here regresses the production failure where a Steam
    // child placed its stub 9,099 words away (beyond imm14's 8,191 maximum).

    std::vector<u8> code;
    code.reserve(192 * 2);
    for (u32 i = 0; i < 192; ++i) {
        code.insert(code.end(), {0x8B, 0x03}); // mov eax,[rbx]
    }
    ASSERT_TRUE(m_memory->write_bytes(kCode64, code.data(), code.size()));

    // Inline guards keep one reservation check per load, and every
    // hot-to-stub branch must reach its stub for the block to compile.
    m_jit = std::make_unique<JitCompiler>(*m_memory);
    m_jit->set_block_cache(m_cache.get());
    BasicBlock block = build64(code);
    ASSERT_EQ(block.instruction_count, 192u);
    CompiledBlock* compiled = m_jit->compile(block);
    ASSERT_NE(compiled, nullptr);
    const auto* words = static_cast<const u32*>(compiled->code);
    std::vector<u32> emitted(words,
                             words + compiled->code_size / sizeof(u32));
    EXPECT_EQ(count_inline_linear64_reservation_checks(emitted), 192u);
    EXPECT_FALSE(has_linear64_hot_helper_call(emitted, 4));
}

// The vector form of the test above: a block of enough unaligned 128-bit
// loads and stores that its cold stubs sit far beyond imm14 (+/-32 KiB) from
// the first access. Every hot-to-stub branch must reach -- a TBNZ crossing
// test aborted a Steam child in emit_flush_cold_mem_stubs -- and the block
// must still compute the right values.
TEST_F(JitLinear64Test, WidenedVectorAccessesReachTheirStubs) {
    std::vector<u8> code;
    for (u32 i = 0; i < 384; ++i) {
        code.insert(code.end(), {0x0F, 0x10, 0x03});  // movups xmm0,[rbx]
        code.insert(code.end(), {0x0F, 0x11, 0x43, 0x10});  // movups [rbx+16],xmm0
    }
    ASSERT_TRUE(m_memory->write_bytes(kCode64, code.data(), code.size()));
    BasicBlock block = build64(code);
    CompiledBlock* compiled = m_jit->compile(block);
    ASSERT_NE(compiled, nullptr);
    ASSERT_TRUE(m_memory->write_u64(kData64 + 0x41, 0x1122334455667788ull));
    ASSERT_TRUE(m_memory->write_u64(kData64 + 0x49, 0x99AABBCCDDEEFF00ull));
    CpuState cpu = cpu64(kData64 + 0x41);
    EXPECT_EQ(test::execute_like_dispatcher(*m_jit, *compiled, cpu), JitExecResult::Continue);
    EXPECT_FALSE(cpu.pending_raise.active);
    EXPECT_EQ(m_memory->read_u64(kData64 + 0x51).value(), 0x1122334455667788ull);
    EXPECT_EQ(m_memory->read_u64(kData64 + 0x59).value(), 0x99AABBCCDDEEFF00ull);
}

}  // namespace
