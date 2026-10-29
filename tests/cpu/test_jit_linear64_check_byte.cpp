// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
//
// The per-host-page check byte (holds-code bit), the single-guard ALU RMW and
// flags across the forced cold store path. Split from test_jit_linear.cpp;
// shared fixtures and matchers live in jit_linear_fixture.hpp.

#include "jit_linear_fixture.hpp"

using namespace vitreo;
using namespace vitreo::cpu;
using namespace vitreo::cpu::jit;
using namespace vitreo::cpu::jit::test;

namespace {

// The x64 guest-base store used to prove "this address does not hold compiled
// code" by acquire-loading a monotone [lo,hi) code envelope on EVERY store.
// That fact now lives in kLinearCheckHoldsCode, one bit of the per-host-page
// check byte the store already loads. These two tests are what make the trade
// legitimate: the first proves the obligation is still met, the second proves
// the bit's lifetime is right at 16 KiB host-page granularity.
TEST_F(JitLinear64Test, DirectStoreIntoTranslatedPageInvalidatesViaCheckByte) {
    // Victim translation at kCode64. Compiling it retains the code page, which
    // is what publishes the bit.
    const std::vector<u8> victim = {0xB8, 0x11, 0x22, 0x33, 0x44, 0xC3};
    ASSERT_TRUE(m_memory->write_bytes(kCode64, victim.data(), victim.size()));
    BasicBlock vblock = build64(victim);
    ASSERT_NE(m_jit->compile(vblock), nullptr);
    m_cache->insert(vblock);
    ASSERT_EQ(m_cache->size(), 1u);

    const u8* check = m_memory->linear_check_page_map();
    ASSERT_NE(check, nullptr);
    ASSERT_NE(check[kCode64 >> kLinearHostPageShift] & kLinearCheckHoldsCode,
              0u)
        << "translating a page did not publish kLinearCheckHoldsCode";

    // The storing block lives in a DIFFERENT 4 KiB guest page inside the SAME
    // 16 KiB host page, so the byte is the only thing that can tell this store
    // it is writing over a translation.
    const GuestAddr store_site = kCode64 + 0x2000;
    const std::vector<u8> code = {0x48, 0x89, 0x03};  // mov [rbx], rax
    ASSERT_TRUE(m_memory->write_bytes(store_site, code.data(), code.size()));
    BasicBlock sblock;
    sblock.start_addr = store_site;
    sblock.is_64bit = true;
    {
        MemoryByteReader r(code.data(), code.size());
        DecodedInstruction i;
        ASSERT_EQ(decode_instruction(r, DecodeMode::Mode64, i),
                  DecodeError::Success);
        sblock.instructions.push_back(i);
        sblock.byte_length = static_cast<u32>(r.position());
        sblock.instruction_count = 1;
    }
    CpuState cpu = cpu64(kCode64);
    cpu.rip = store_site;
    cpu.gpr.set64(EAX, 0x9090909090909090ull);
    CompiledBlock* scb = m_jit->compile(sblock);
    ASSERT_NE(scb, nullptr);
    (void)test::execute_like_dispatcher(*m_jit, *scb, cpu);

    EXPECT_EQ(m_cache->size(), 0u)
        << "a direct linear64 store over a translation skipped invalidation";
}

TEST_F(JitLinear64Test, HoldsCodeBitSurvivesUntilLastGuestPageInHostPage) {
    const u8* check = m_memory->linear_check_page_map();
    ASSERT_NE(check, nullptr);
    const usize host_page = kCode64 >> kLinearHostPageShift;

    // Two distinct 4 KiB guest code pages inside one 16 KiB host page.
    const GuestAddr first = kCode64;
    const GuestAddr second = kCode64 + 0x1000;
    ASSERT_EQ(first >> kLinearHostPageShift, second >> kLinearHostPageShift);

    m_memory->retain_jit_code_page(first);
    EXPECT_NE(check[host_page] & kLinearCheckHoldsCode, 0u);
    m_memory->retain_jit_code_page(second);
    EXPECT_NE(check[host_page] & kLinearCheckHoldsCode, 0u);

    // Releasing one of them must NOT clear the bit: the host page still
    // contains a translation, and a store to it is still self-modifying.
    m_memory->release_jit_code_page(first);
    EXPECT_NE(check[host_page] & kLinearCheckHoldsCode, 0u)
        << "the bit was cleared while another guest code page still held one";

    m_memory->release_jit_code_page(second);
    EXPECT_EQ(check[host_page] & kLinearCheckHoldsCode, 0u)
        << "the bit outlived the last translation in its host page";
}

TEST_F(JitLinear64Test, MixedProtAndHoldsCodeDoNotClearEachOther) {
    const u8* check = m_memory->linear_check_page_map();
    ASSERT_NE(check, nullptr);
    const usize host_page = kData64 >> kLinearHostPageShift;

    m_memory->retain_jit_code_page(kData64);
    ASSERT_NE(check[host_page] & kLinearCheckHoldsCode, 0u);

    // A protection change makes the page uniform, so the reconcile clears its
    // own bit. It must leave the other owner's bit alone -- the byte carries
    // two independent facts and neither publisher may speak for the other.
    ASSERT_TRUE(m_memory->protect_region(kData64, 0x4000,
                                         MemoryProtection::ReadWrite));
    EXPECT_NE(check[host_page] & kLinearCheckHoldsCode, 0u)
        << "reconcile cleared kLinearCheckHoldsCode, which it does not own";

    m_memory->release_jit_code_page(kData64);
    EXPECT_EQ(check[host_page] & kLinearCheckHoldsCode, 0u);
}

// The guest-base ALU32 memory RMW guards ONCE and then runs LDR -> ALU -> STR
// against one host address, instead of emitting a complete guard for the load
// half and another for the store half. That makes it a semantic path of its
// own, with a fast arm, a granule-crossing arm and an out-of-window arm, so it
// is swept across every offset inside a 16-byte granule rather than probed at
// one convenient address: offsets 13..15 make the dword straddle the granule
// and must divert to the checked fallback, and nothing else may.
TEST_F(JitLinear64Test, Alu32MemoryRmwCoversOrderedGranuleOffsets) {
    ASSERT_TRUE(m_memory->linear_hardware_protection());
    const std::vector<u8> code = {0x83, 0x03, 0x01};  // add dword [rbx],1
    ASSERT_TRUE(m_memory->write_bytes(kCode64, code.data(), code.size()));
    BasicBlock block = build64(code);
    ASSERT_EQ(block.instruction_count, 1u);
    CompiledBlock* compiled = m_jit->compile(block);
    ASSERT_NE(compiled, nullptr);

    // Prove the single-guard form is actually reached. A silent fall-through to
    // the two-guard path would still compute the right answer, which is exactly
    // how a lowering stops being exercised without any test going red.
    // The single-guard form is the stencil's lowering. Tier 1 keeps the load
    // and the store as separate IR accesses, each with its own three-word
    // guard, and is the smaller of the two; only the results below apply.
    if (!m_jit->ir_emit_active()) {
        std::vector<u32> emitted(compiled->code_size / 4);
        std::memcpy(emitted.data(), compiled->code, compiled->code_size);
        EXPECT_TRUE(std::any_of(emitted.begin(), emitted.end(), [](u32 w) {
            return w == arm64::add_x(17, REG_LINEAR_BASE, 9);
        })) << "RMW did not form a host address off the pinned linear base";
        // Guard-once is a property of the FAST ARM only: the checked fallback
        // below it still contains a full load guard and a full store guard,
        // so a whole-block count of check-byte loads is legitimately three.
        // The invariant is that nothing re-tests the byte between the fast
        // arm's test and the point it forms the host address.
        const u32 check_load = arm64::ldrb_reg(3, REG_LINEAR64_CHECK_MAP, 4);
        const u32 host_addr = arm64::add_x(17, REG_LINEAR_BASE, 9);
        const auto first_check =
            std::find(emitted.begin(), emitted.end(), check_load);
        ASSERT_NE(first_check, emitted.end())
            << "RMW fast arm does not test the check byte at all";
        const auto fast_host = std::find(first_check, emitted.end(), host_addr);
        ASSERT_NE(fast_host, emitted.end())
            << "RMW fast arm never forms a host address off the pinned base";
        EXPECT_EQ(std::count(std::next(first_check), fast_host, check_load), 0)
            << "RMW fast arm tests the check byte twice: the load and store "
               "halves are still guarding separately";
    }

    for (u32 offset = 0; offset < 16; ++offset) {
        SCOPED_TRACE(::testing::Message() << "granule offset " << offset);
        const GuestAddr target = kData64 + 0x80 + offset;
        constexpr u32 seed = 0x7FFFFFFEu;   // +1 twice crosses no boundary
        ASSERT_TRUE(m_memory->write_u32(target, seed).is_ok());

        CpuState jcpu = cpu64(target);
        EXPECT_EQ(test::execute_like_dispatcher(*m_jit, *compiled, jcpu), JitExecResult::Continue);
        EXPECT_FALSE(jcpu.pending_raise.active);
        const u32 jit_value = m_memory->read_u32(target).value_or(0);

        // Same instruction through the interpreter, which is the semantic
        // reference for both the value and the flags.
        ASSERT_TRUE(m_memory->write_u32(target, seed).is_ok());
        CpuState icpu = cpu64(target);
        Interpreter interp(icpu, *m_memory);
        (void)interp.run(1);
        const u32 interp_value = m_memory->read_u32(target).value_or(0);

        EXPECT_EQ(jit_value, interp_value) << "RMW result diverged";
        // Both sides must be materialised first: the JIT leaves flags as a
        // pending FlagOp whenever nothing has consumed them yet, so comparing
        // the raw field would compare one materialised state against one stale
        // one and report a divergence that is not there.
        jcpu.materialize_lazy_flags();
        icpu.materialize_lazy_flags();
        EXPECT_EQ(jcpu.flags.rflags & 0x8D5u, icpu.flags.rflags & 0x8D5u)
            << "RMW flags diverged (SF/ZF/AF/PF/CF/OF)";
    }
}

// The out-of-window arm of the same single-guard RMW. An EA outside the
// reservation must raise STATUS_ACCESS_VIOLATION, not wrap into it.
TEST_F(JitLinear64Test, Alu32MemoryRmwOutsideReservationRaisesAv) {
    const std::vector<u8> code = {0x83, 0x03, 0x01};  // add dword [rbx],1
    ASSERT_TRUE(m_memory->write_bytes(kCode64, code.data(), code.size()));
    CompiledBlock* compiled = m_jit->compile(build64(code));
    ASSERT_NE(compiled, nullptr);

    CpuState cpu = cpu64(kUnmapped64);
    (void)test::execute_like_dispatcher(*m_jit, *compiled, cpu);
    EXPECT_TRUE(cpu.pending_raise.active)
        << "an RMW outside the reservation did not fault";
}

// The NZCV save/restore around a guest store moved OUT of the fast path and
// INTO the cold stub, because the linear64 fast path is flag-clean. That makes
// the cold path the only place the guest's flags can now be lost — and it is
// the path a test is least likely to take by accident. Force it: the store
// targets a host page carrying kLinearCheckHoldsCode, so the check byte
// diverts every store on it to the helper, with an x86 flag producer before
// the store and its consumer after.
TEST_F(JitLinear64Test, FlagsSurviveStoreColdPathBetweenCmpAndJcc) {
    ASSERT_TRUE(m_memory->linear_hardware_protection());
    // Make the data page hold "translated code" so stores to it take the stub.
    m_memory->retain_jit_code_page(kData64);
    const u8* check = m_memory->linear_check_page_map();
    ASSERT_NE(check, nullptr);
    ASSERT_NE(check[kData64 >> kLinearHostPageShift] & kLinearCheckHoldsCode,
              0u);

    // cmp rax,rcx ; mov [rbx],rdx ; sete r8b  — the SETcc consumes exactly the
    // flags the store sits on top of.
    const std::vector<u8> code = {
        0x48, 0x39, 0xC8,              // cmp rax, rcx
        0x48, 0x89, 0x13,              // mov [rbx], rdx
        0x41, 0x0F, 0x94, 0xC0,        // sete r8b
    };
    ASSERT_TRUE(m_memory->write_bytes(kCode64, code.data(), code.size()));
    BasicBlock block = build64(code);
    ASSERT_EQ(block.instruction_count, 3u);
    CompiledBlock* compiled = m_jit->compile(block);
    ASSERT_NE(compiled, nullptr);

    for (const u64 rcx : {u64{0x1111}, u64{0x2222}}) {   // equal, then not
        CpuState jcpu = cpu64(kData64 + 0x200);
        jcpu.gpr.set64(EAX, 0x1111);
        jcpu.gpr.set64(static_cast<u8>(RegIndex::RCX), rcx);
        jcpu.gpr.set64(static_cast<u8>(RegIndex::RDX),
                       0xFEEDFACECAFEBEEFull);
        EXPECT_EQ(test::execute_like_dispatcher(*m_jit, *compiled, jcpu), JitExecResult::Continue);
        EXPECT_FALSE(jcpu.pending_raise.active);

        CpuState icpu = cpu64(kData64 + 0x200);
        icpu.gpr.set64(EAX, 0x1111);
        icpu.gpr.set64(static_cast<u8>(RegIndex::RCX), rcx);
        icpu.gpr.set64(static_cast<u8>(RegIndex::RDX),
                       0xFEEDFACECAFEBEEFull);
        Interpreter interp(icpu, *m_memory);
        (void)interp.run(3);

        jcpu.materialize_lazy_flags();
        icpu.materialize_lazy_flags();
        EXPECT_EQ(jcpu.gpr.get64(8) & 0xFF, icpu.gpr.get64(8) & 0xFF)
            << "SETcc after a cold-path store read the wrong flags (rcx="
            << rcx << ")";
        EXPECT_EQ(jcpu.flags.rflags & 0x8D5u, icpu.flags.rflags & 0x8D5u);
        EXPECT_EQ(m_memory->read_u64(kData64 + 0x200).value_or(0),
                  0xFEEDFACECAFEBEEFull);
    }
    m_memory->release_jit_code_page(kData64);
}

}  // namespace
