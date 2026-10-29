// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
//
// Mixed 16 KiB host pages decided per 4 KiB guest page, for scalar and vector
// linear64 accesses. Split from test_jit_linear.cpp; shared fixtures and
// matchers live in jit_linear_fixture.hpp.

#include "jit_linear_fixture.hpp"

using namespace vitreo;
using namespace vitreo::cpu;
using namespace vitreo::cpu::jit;
using namespace vitreo::cpu::jit::test;

namespace {

// Two 4 KiB guest pages inside ONE 16 KiB host page, protected differently.
//
// This is the case that decides the whole hardware-enforcement design, and
// both obvious answers are wrong. Protecting the host page most-restrictively
// (PROT_READ, because one guest page is read-only) would deny the store to the
// read-write neighbour that the guest explicitly permits — and PE images make
// that the common case, since sections carry different protections at 4 KiB
// SectionAlignment, so a hot store into the first page of .data would fault on
// every iteration. Protecting it most-permissively alone would let the store
// into the READ-ONLY page through silently, which is the bug hardware
// enforcement exists to remove.
//
// The answer is most-permissive PLUS a mixed-page marking that sends every
// access to this host page through the checked helper, so the protection runs
// (still the authority) decide. Both halves are asserted below, at 6 GiB,
// where a 64-bit address truncated through 32-bit arithmetic would not alias.
TEST_F(JitLinear64Test, SubHostPageProtectionsAreBothEnforced) {
    ASSERT_EQ(kData64 % 0x4000u, 0u) << "fixture must start a 16 KiB host page";

    // First 4 KiB read-only; the remaining three 4 KiB pages stay read-write.
    ASSERT_TRUE(m_memory->write_u64(kData64 + 0x100, 0x1111111111111111ull));
    ASSERT_TRUE(m_memory->write_u64(kData64 + 0x2000, 0x2222222222222222ull));
    ASSERT_TRUE(m_memory->protect_region(kData64, 0x1000,
                                         MemoryProtection::Read));

    const u8* mixed = m_memory->linear_check_page_map();
    ASSERT_NE(mixed, nullptr);
    EXPECT_NE(mixed[kData64 >> 14], 0u)
        << "a host page whose guest pages disagree must be marked mixed";

    const std::vector<u8> store_code = {0x48, 0x89, 0x03}; // mov [rbx],rax
    ASSERT_TRUE(m_memory->write_bytes(kCode64, store_code.data(),
                                      store_code.size()));
    CompiledBlock* store = m_jit->compile(build64(store_code));
    ASSERT_NE(store, nullptr);

    // (a) The read-only 4 KiB page rejects the store, as a write AV at the
    //     exact 64-bit address, leaving the old value intact.
    CpuState ro = cpu64(kData64 + 0x100);
    ro.gpr.set64(EAX, 0xDEADBEEFDEADBEEFull);
    EXPECT_EQ(test::execute_like_dispatcher(*m_jit, *store, ro), JitExecResult::Continue);
    ASSERT_TRUE(ro.pending_raise.active)
        << "store into the read-only half of a mixed host page was allowed";
    EXPECT_EQ(ro.pending_raise.code, 0xC0000005u);
    EXPECT_EQ(ro.pending_raise.args[0], 1u);
    EXPECT_EQ(ro.pending_raise.args[1], kData64 + 0x100);
    EXPECT_EQ(m_memory->read_u64(kData64 + 0x100).value_or(0),
              0x1111111111111111ull);

    // (b) The read-write 4 KiB page of the SAME host page still takes the
    //     store. This is what most-restrictive rounding would have broken.
    CpuState rw = cpu64(kData64 + 0x2000);
    rw.gpr.set64(EAX, 0xFEEDFACEFEEDFACEull);
    EXPECT_EQ(test::execute_like_dispatcher(*m_jit, *store, rw), JitExecResult::Continue);
    EXPECT_FALSE(rw.pending_raise.active)
        << "store into the read-write half of a mixed host page was denied";
    EXPECT_EQ(m_memory->read_u64(kData64 + 0x2000).value_or(0),
              0xFEEDFACEFEEDFACEull);

    // (c) Reading the read-only page is still permitted.
    const std::vector<u8> load_code = {0x48, 0x8B, 0x03}; // mov rax,[rbx]
    ASSERT_TRUE(m_memory->write_bytes(kCode64 + 0x100, load_code.data(),
                                      load_code.size()));
    BasicBlock load_block = build64(load_code);
    load_block.start_addr = kCode64 + 0x100;
    CompiledBlock* load = m_jit->compile(load_block);
    ASSERT_NE(load, nullptr);
    CpuState rd = cpu64(kData64 + 0x100);
    rd.rip = kCode64 + 0x100;
    EXPECT_EQ(test::execute_like_dispatcher(*m_jit, *load, rd), JitExecResult::Continue);
    EXPECT_FALSE(rd.pending_raise.active);
    EXPECT_EQ(rd.gpr.get64(EAX), 0x1111111111111111ull);

    // Restoring a uniform protection clears the marking again, so the page
    // goes back to the direct path rather than paying the helper forever.
    ASSERT_TRUE(m_memory->protect_region(kData64, 0x4000,
                                         MemoryProtection::ReadWrite));
    EXPECT_EQ(mixed[kData64 >> 14], 0u)
        << "a host page that became uniform again is still marked mixed";
}

// Intel SDM Vol. 3A, "Interrupt 14—Page-Fault Exception (#PF)": every page
// translation needed by a memory operand must permit the access.  An 8-byte
// operand can touch two 16 KiB host pages even though it is much smaller than
// either page.  If the first host page is mixed and the second is uniform,
// checking only the last page's mixed bit silently bypasses the forbidden
// 4 KiB guest page at the start of the operand.
TEST_F(JitLinear64Test, CrossHostBoundaryLoadChecksFirstAndLastMixedPages) {
    constexpr GuestAddr second_host = kData64 + 0x4000;
    constexpr GuestAddr target = second_host - 4;
    ASSERT_TRUE(m_memory->map_region(second_host, 0x4000,
                                     MemoryProtection::ReadWrite));
    ASSERT_TRUE(m_memory->write_u32(target, 0x55667788u));
    ASSERT_TRUE(m_memory->write_u32(second_host, 0x11223344u));

    // Make only the final 4 KiB guest page of the first host page inaccessible.
    // Its RW neighbours keep the host page readable, so only the mixed map can
    // enforce the guest protection.
    ASSERT_TRUE(m_memory->protect_region(kData64 + 0x3000, 0x1000,
                                         MemoryProtection::None));
    const u8* mixed = m_memory->linear_check_page_map();
    ASSERT_NE(mixed, nullptr);
    ASSERT_NE(mixed[(target) >> kLinearHostPageShift] &
                        kLinearCheckMixedProt,
                    0u);
    ASSERT_EQ(mixed[(target + 7) >> kLinearHostPageShift] &
                        kLinearCheckMixedProt,
                    0u);

    const std::vector<u8> code = {0x48, 0x8B, 0x03}; // mov rax,[rbx]
    ASSERT_TRUE(m_memory->write_bytes(kCode64, code.data(), code.size()));
    CompiledBlock* compiled = m_jit->compile(build64(code));
    ASSERT_NE(compiled, nullptr);

    CpuState cpu = cpu64(target);
    cpu.gpr.set64(EAX, 0xA5A5A5A5A5A5A5A5ull);
    EXPECT_EQ(test::execute_like_dispatcher(*m_jit, *compiled, cpu), JitExecResult::Continue);
    ASSERT_TRUE(cpu.pending_raise.active)
        << "cross-host load bypassed a forbidden first guest page";
    EXPECT_EQ(cpu.pending_raise.code, 0xC0000005u);
    EXPECT_EQ(cpu.pending_raise.args[0], 0u);
    EXPECT_EQ(cpu.pending_raise.args[1], target);
    EXPECT_EQ(cpu.gpr.get64(EAX), 0xA5A5A5A5A5A5A5A5ull);
}

// A mixed host page -- its four 4 KiB guest pages disagree -- is decided per
// guest page in the cold stub from GuestMemory::linear_subpage_map(), without
// the C++ helper. Guest pages here: 0 and 1 RW, 2 NOACCESS, 3 read-only. Every
// width loads and stores what it should where the guest allows it, and raises
// where it does not, including an access that straddles an allowed and a
// denied guest page.
TEST_F(JitLinear64Test, MixedHostPageAccessIsDecidedPerGuestPage) {
    ASSERT_TRUE(m_memory->protect_region(kData64 + 0x2000, 0x1000,
                                         MemoryProtection::None));
    ASSERT_TRUE(m_memory->protect_region(kData64 + 0x3000, 0x1000,
                                         MemoryProtection::Read));
    const u8* check = m_memory->linear_check_page_map();
    const u8* sub = m_memory->linear_subpage_map();
    ASSERT_NE(check, nullptr);
    ASSERT_NE(sub, nullptr);
    const usize page = kData64 >> kLinearHostPageShift;
    ASSERT_NE(check[page] & kLinearCheckMixedRead, 0u);
    // Read: guest pages 0, 1, 3. Write: 0, 1.
    EXPECT_EQ(sub[page], 0x3Bu);

    struct Form {
        std::vector<u8> code;
        int width;
        bool store;
    };
    const Form forms[] = {
        {{0x8A, 0x03}, 1, false},        {{0x66, 0x8B, 0x03}, 2, false},
        {{0x8B, 0x03}, 4, false},        {{0x48, 0x8B, 0x03}, 8, false},
        {{0x88, 0x03}, 1, true},         {{0x66, 0x89, 0x03}, 2, true},
        {{0x89, 0x03}, 4, true},         {{0x48, 0x89, 0x03}, 8, true},
    };
    const u64 mask_for[9] = {0, 0xFF, 0xFFFF, 0, 0xFFFFFFFFull, 0, 0, 0,
                             ~0ull};
    GuestAddr code_at = kCode64;  // each run compiles its own block
    const auto run = [&](const Form& f, GuestAddr at) {
        code_at += 0x40;
        EXPECT_TRUE(m_memory->write_bytes(code_at, f.code.data(), f.code.size()));
        BasicBlock b = build64(f.code);
        b.start_addr = code_at;
        CompiledBlock* compiled = m_jit->compile(b);
        EXPECT_NE(compiled, nullptr);
        CpuState cpu = cpu64(at);
        cpu.rip = code_at;
        cpu.gpr.set64(EAX, 0x0123456789ABCDEFull);
        EXPECT_EQ(test::execute_like_dispatcher(*m_jit, *compiled, cpu), JitExecResult::Continue);
        return cpu;
    };
    for (const Form& f : forms) {
        SCOPED_TRACE(::testing::Message() << "width=" << f.width
                                          << " store=" << f.store);
        const u64 mask = mask_for[f.width];
        // Allowed guest pages: 0 and 1 both ways, 3 for loads.
        for (const GuestAddr at : {kData64 + 0x0010, kData64 + 0x1FF0,
                                   kData64 + 0x3008}) {
            const bool allowed = !f.store || at < kData64 + 0x2000;
            ASSERT_TRUE(m_memory->write_u64(at, 0x1122334455667788ull) ||
                        at >= kData64 + 0x3000);
            const u64 before = m_memory->read_u64(at).value();
            CpuState cpu = run(f, at);
            EXPECT_EQ(cpu.pending_raise.active, !allowed) << std::hex << at;
            if (!allowed) {
                EXPECT_EQ(cpu.pending_raise.args[0], 1u);
                EXPECT_EQ(cpu.pending_raise.args[1], at);
                EXPECT_EQ(m_memory->read_u64(at).value(), before);
            } else if (f.store) {
                EXPECT_EQ(m_memory->read_u64(at).value() & mask,
                          0x0123456789ABCDEFull & mask);
            } else {
                EXPECT_EQ(cpu.gpr.get64(EAX) & mask, before & mask);
            }
        }
        // Denied guest page 2, and a multi-byte access that starts in the
        // allowed guest page 1 and ends in the denied guest page 2.
        for (const GuestAddr at : {kData64 + 0x2010,
                                   kData64 + 0x2000 - (f.width > 1 ? 1 : 0)}) {
            if (f.width == 1 && at == kData64 + 0x2000) continue;
            CpuState cpu = run(f, at);
            ASSERT_TRUE(cpu.pending_raise.active) << std::hex << at;
            EXPECT_EQ(cpu.pending_raise.code, 0xC0000005u);
            EXPECT_EQ(cpu.pending_raise.args[0], f.store ? 1u : 0u);
            // The inaccessible address: the access's first byte in the denied
            // guest page, as x86's CR2 and Windows' ExceptionInformation[1].
            EXPECT_EQ(cpu.pending_raise.args[1],
                      std::max<GuestAddr>(at, kData64 + 0x2000));
            EXPECT_EQ(cpu.gpr.get64(EAX), 0x0123456789ABCDEFull);
        }
    }

    // The knob that disables the sub-page path sends every case above to the
    // helper, which must give the same answers; the white box below needs it.
    if (const char* knob = std::getenv("VITREO_JIT_SUBPAGE_STUB");
        knob != nullptr && knob[0] == '0') {
        return;
    }
    // White box: the decision really is the sub-page byte's. Grant guest page
    // 2 in the map alone (the region table still denies it, and the host page
    // is readable as the most permissive of its guest pages): a load that
    // consults the byte now reads it; the helper would still raise.
    u8* writable_sub = const_cast<u8*>(sub);
    const u8 saved = writable_sub[page];
    writable_sub[page] = static_cast<u8>(saved | 0x04u);
    CpuState granted = run(forms[3], kData64 + 0x2010);
    EXPECT_FALSE(granted.pending_raise.active)
        << "the cold stub did not decide the mixed page from the sub-page map";
    writable_sub[page] = saved;
}

// The vector forms of the test above. SSE loads and stores are guarded by the
// shared out-of-line hot helper rather than a per-site cold stub entry, so the
// per-guest-page decision lives there: same guest pages (0 and 1 RW, 2
// NOACCESS, 3 read-only), every SSE width.
TEST_F(JitLinear64Test, MixedHostPageVectorAccessIsDecidedPerGuestPage) {
    ASSERT_TRUE(m_memory->protect_region(kData64 + 0x2000, 0x1000,
                                         MemoryProtection::None));
    ASSERT_TRUE(m_memory->protect_region(kData64 + 0x3000, 0x1000,
                                         MemoryProtection::Read));
    const u8* sub = m_memory->linear_subpage_map();
    ASSERT_NE(sub, nullptr);
    const usize page = kData64 >> kLinearHostPageShift;
    ASSERT_EQ(sub[page], 0x3Bu);

    struct Form {
        std::vector<u8> code;
        int width;
        bool store;
    };
    const Form forms[] = {
        {{0x0F, 0x10, 0x03}, 16, false},        // movups xmm0,[rbx]
        {{0xF2, 0x0F, 0x10, 0x03}, 8, false},   // movsd xmm0,[rbx]
        {{0xF3, 0x0F, 0x10, 0x03}, 4, false},   // movss xmm0,[rbx]
        {{0x0F, 0x11, 0x03}, 16, true},         // movups [rbx],xmm0
        {{0xF2, 0x0F, 0x11, 0x03}, 8, true},    // movsd [rbx],xmm0
        {{0xF3, 0x0F, 0x11, 0x03}, 4, true},    // movss [rbx],xmm0
    };
    const auto pattern = [](int i) { return static_cast<u8>(0xC0u + i); };
    GuestAddr code_at = kCode64;
    const auto run = [&](const Form& f, GuestAddr at) {
        code_at += 0x40;
        EXPECT_TRUE(m_memory->write_bytes(code_at, f.code.data(), f.code.size()));
        BasicBlock b = build64(f.code);
        b.start_addr = code_at;
        CompiledBlock* compiled = m_jit->compile(b);
        EXPECT_NE(compiled, nullptr);
        CpuState cpu = cpu64(at);
        cpu.rip = code_at;
        for (int i = 0; i < 16; ++i) cpu.sse.xmm[0].u8_[i] = pattern(i);
        EXPECT_EQ(test::execute_like_dispatcher(*m_jit, *compiled, cpu), JitExecResult::Continue);
        return cpu;
    };
    const auto memory_byte = [&](GuestAddr a) {
        return m_memory->read_u8(a).value();
    };
    for (const Form& f : forms) {
        SCOPED_TRACE(::testing::Message() << "width=" << f.width
                                          << " store=" << f.store);
        for (const GuestAddr at : {kData64 + 0x0010, kData64 + 0x2000 - 16,
                                   kData64 + 0x3008}) {
            const bool allowed = !f.store || at < kData64 + 0x2000;
            if (at < kData64 + 0x3000) {
                for (int i = 0; i < 16; i += 8) {
                    ASSERT_TRUE(m_memory->write_u64(at + i,
                                                    0x1122334455667788ull));
                }
            }
            u8 before[16];
            for (int i = 0; i < 16; ++i) before[i] = memory_byte(at + i);
            CpuState cpu = run(f, at);
            EXPECT_EQ(cpu.pending_raise.active, !allowed) << std::hex << at;
            for (int i = 0; i < f.width; ++i) {
                if (!allowed) {
                    EXPECT_EQ(memory_byte(at + i), before[i]) << i;
                } else if (f.store) {
                    EXPECT_EQ(memory_byte(at + i), pattern(i)) << i;
                } else {
                    EXPECT_EQ(cpu.sse.xmm[0].u8_[i], before[i]) << i;
                }
            }
            if (!allowed) {
                EXPECT_EQ(cpu.pending_raise.args[0], 1u);
                EXPECT_EQ(cpu.pending_raise.args[1], at);
            }
        }
        // Denied guest page 2, and an access that starts in the allowed guest
        // page 1 and ends in it (the address is the first denied byte).
        for (const GuestAddr at : {kData64 + 0x2010,
                                   kData64 + 0x2000 - f.width / 2}) {
            CpuState cpu = run(f, at);
            ASSERT_TRUE(cpu.pending_raise.active) << std::hex << at;
            EXPECT_EQ(cpu.pending_raise.code, 0xC0000005u);
            EXPECT_EQ(cpu.pending_raise.args[0], f.store ? 1u : 0u);
            EXPECT_EQ(cpu.pending_raise.args[1],
                      std::max<GuestAddr>(at, kData64 + 0x2000));
            for (int i = 0; i < 16; ++i) {
                EXPECT_EQ(cpu.sse.xmm[0].u8_[i], pattern(i)) << i;
            }
        }
    }

    if (const char* knob = std::getenv("VITREO_JIT_SUBPAGE_STUB");
        knob != nullptr && knob[0] == '0') {
        return;
    }
    // White box, as above: grant guest page 2 in the map alone. A 128-bit
    // load that consults the byte reads it; the helper would still raise.
    u8* writable_sub = const_cast<u8*>(sub);
    const u8 saved = writable_sub[page];
    writable_sub[page] = static_cast<u8>(saved | 0x04u);
    CpuState granted = run(forms[0], kData64 + 0x2010);
    EXPECT_FALSE(granted.pending_raise.active)
        << "the vector guard did not decide the mixed page from the sub-page map";
    writable_sub[page] = saved;
}

// MOVAPS: the 16-byte alignment check (#GP) makes a page crossing impossible,
// so the guard keeps only the reservation test (emit_linear64_vec128_checked,
// aligned16). Same mixed host page as above: aligned accesses are decided per
// guest page, and a misaligned one still raises instead of reaching memory.
TEST_F(JitLinear64Test, AlignedVectorAccessOnMixedHostPage) {
    ASSERT_TRUE(m_memory->protect_region(kData64 + 0x2000, 0x1000,
                                         MemoryProtection::None));
    ASSERT_TRUE(m_memory->protect_region(kData64 + 0x3000, 0x1000,
                                         MemoryProtection::Read));
    const std::vector<u8> load = {0x0F, 0x28, 0x03};   // movaps xmm0,[rbx]
    const std::vector<u8> store = {0x0F, 0x29, 0x03};  // movaps [rbx],xmm0
    GuestAddr code_at = kCode64;
    const auto run = [&](const std::vector<u8>& code, GuestAddr at) {
        code_at += 0x40;
        EXPECT_TRUE(m_memory->write_bytes(code_at, code.data(), code.size()));
        BasicBlock b = build64(code);
        b.start_addr = code_at;
        CompiledBlock* compiled = m_jit->compile(b);
        EXPECT_NE(compiled, nullptr);
        CpuState cpu = cpu64(at);
        cpu.rip = code_at;
        for (int i = 0; i < 16; ++i) {
            cpu.sse.xmm[0].u8_[i] = static_cast<u8>(0xC0u + i);
        }
        EXPECT_EQ(test::execute_like_dispatcher(*m_jit, *compiled, cpu), JitExecResult::Continue);
        return cpu;
    };
    for (const GuestAddr at : {kData64 + 0x0000, kData64 + 0x1FF0}) {
        ASSERT_TRUE(m_memory->write_u64(at, 0x1122334455667788ull));
        ASSERT_TRUE(m_memory->write_u64(at + 8, 0x99AABBCCDDEEFF00ull));
        CpuState l = run(load, at);
        EXPECT_FALSE(l.pending_raise.active) << std::hex << at;
        EXPECT_EQ(l.sse.xmm[0].u64_[0], 0x1122334455667788ull);
        EXPECT_EQ(l.sse.xmm[0].u64_[1], 0x99AABBCCDDEEFF00ull);
        CpuState st = run(store, at);
        EXPECT_FALSE(st.pending_raise.active) << std::hex << at;
        EXPECT_EQ(m_memory->read_u64(at).value(), 0xC7C6C5C4C3C2C1C0ull);
    }
    // Read-only guest page 3: loads succeed, stores raise a write AV.
    {
        CpuState l = run(load, kData64 + 0x3010);
        EXPECT_FALSE(l.pending_raise.active);
        CpuState st = run(store, kData64 + 0x3010);
        ASSERT_TRUE(st.pending_raise.active);
        EXPECT_EQ(st.pending_raise.code, 0xC0000005u);
        EXPECT_EQ(st.pending_raise.args[0], 1u);
        EXPECT_EQ(st.pending_raise.args[1], kData64 + 0x3010);
    }
    // Denied guest page 2, aligned: an access violation at the access.
    for (const auto* code : {&load, &store}) {
        CpuState cpu = run(*code, kData64 + 0x2010);
        ASSERT_TRUE(cpu.pending_raise.active);
        EXPECT_EQ(cpu.pending_raise.code, 0xC0000005u);
        EXPECT_EQ(cpu.pending_raise.args[1], kData64 + 0x2010);
    }
    // Misaligned, straddling allowed page 1 and denied page 2: MOVAPS faults
    // on the alignment, before any byte is read or written.
    for (const auto* code : {&load, &store}) {
        ASSERT_TRUE(m_memory->write_u64(kData64 + 0x1FF8, 0x5555555555555555ull));
        CpuState cpu = run(*code, kData64 + 0x1FF8);
        ASSERT_TRUE(cpu.pending_raise.active);
        EXPECT_EQ(m_memory->read_u64(kData64 + 0x1FF8).value(),
                  0x5555555555555555ull);
    }
}

// The mirror image of the test above, and the one that makes the crossing
// decision observable: the FIRST byte's host page is clean, the SECOND host
// page is mixed (its first 4 KiB guest page is NOACCESS, the rest RW, so the
// host page itself must stay readable). A load that consulted only the first
// byte's check byte would read through the hardware page and succeed; the
// crossing test must route it to the checked helper, which denies it. Both the
// register-EA form and the compile-time-constant (moffs) form are covered.
TEST_F(JitLinear64Test, CrossHostBoundaryLoadIntoMixedSecondPageRaises) {
    constexpr GuestAddr second_host = kData64 + 0x4000;
    constexpr GuestAddr target = second_host - 4;
    ASSERT_TRUE(m_memory->map_region(second_host, 0x4000,
                                     MemoryProtection::ReadWrite));
    ASSERT_TRUE(m_memory->protect_region(second_host, 0x1000,
                                         MemoryProtection::None));
    const u8* map = m_memory->linear_check_page_map();
    ASSERT_NE(map, nullptr);
    ASSERT_EQ(map[target >> kLinearHostPageShift], 0u);
    ASSERT_NE(map[second_host >> kLinearHostPageShift] &
                  kLinearCheckMixedRead,
              0u);

    std::vector<u8> moffs = {0x48, 0xA1};  // mov rax,[moffs64]
    for (int i = 0; i < 8; ++i) {
        moffs.push_back(static_cast<u8>(target >> (8 * i)));
    }
    const std::vector<std::vector<u8>> forms = {
        {0x48, 0x8B, 0x03},  // mov rax,[rbx]
        moffs,
    };
    for (const auto& code : forms) {
        SCOPED_TRACE(::testing::Message() << "form length " << code.size());
        ASSERT_TRUE(m_memory->write_bytes(kCode64, code.data(), code.size()));
        m_jit = std::make_unique<JitCompiler>(*m_memory);
        m_jit->set_block_cache(m_cache.get());
        CompiledBlock* compiled = m_jit->compile(build64(code));
        ASSERT_NE(compiled, nullptr);
        CpuState cpu = cpu64(target);
        cpu.gpr.set64(EAX, 0xA5A5A5A5A5A5A5A5ull);
        EXPECT_EQ(test::execute_like_dispatcher(*m_jit, *compiled, cpu), JitExecResult::Continue);
        ASSERT_TRUE(cpu.pending_raise.active)
            << "cross-host load read through a forbidden second guest page";
        EXPECT_EQ(cpu.pending_raise.code, 0xC0000005u);
        EXPECT_EQ(cpu.pending_raise.args[0], 0u);
        EXPECT_EQ(cpu.gpr.get64(EAX), 0xA5A5A5A5A5A5A5A5ull);
    }
}

}  // namespace
