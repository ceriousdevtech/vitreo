// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
//
// Linear64 accesses above 4 GiB and above the window: oracle faults,
// range-by-fault, 128/256-bit and scalar widths. Split from
// test_jit_linear.cpp; shared fixtures and matchers live in
// jit_linear_fixture.hpp.

#include "jit_linear_fixture.hpp"

using namespace vitreo;
using namespace vitreo::cpu;
using namespace vitreo::cpu::jit;
using namespace vitreo::cpu::jit::test;

namespace {

// An x64 access above the user range (tools/oracle_probe/
// high_address_fault_probe.expected, Windows): a NON-CANONICAL address --
// bits 63:47 not all equal, including an 8-byte access that starts at the
// last user qword and runs into one -- is #GP, which Windows reports as
// STATUS_ACCESS_VIOLATION with ExceptionInformation[0] = 0 (even for a store)
// and [1] = ~0; a canonical KERNEL address is a page fault reported with the
// access direction and the address itself.
TEST_F(JitLinear64Test, HighAddressFaultsMatchOracle) {
    struct Case {
        GuestAddr addr;
        bool store;
        u64 info0;
        u64 info1;
    };
    const Case cases[] = {
        {0x0000800000000000ull, false, 0, ~0ull},
        {0x0000800000000000ull, true, 0, ~0ull},
        {0x1234567812345678ull, false, 0, ~0ull},
        {0x1234567812345678ull, true, 0, ~0ull},
        {0x8000000000000000ull, true, 0, ~0ull},
        {0xFFFF7FFFFFFFFFF8ull, false, 0, ~0ull},
        {0x00007FFFFFFFFFFCull, false, 0, ~0ull},
        {0x00007FFFFFFFFFFCull, true, 0, ~0ull},
        {0xFFFF800000000000ull, false, 0, 0xFFFF800000000000ull},
        {0xFFFF800000000000ull, true, 1, 0xFFFF800000000000ull},
        {0xFFFFFFFFFFFFFFF8ull, false, 0, 0xFFFFFFFFFFFFFFF8ull},
        {0xFFFFFFFFFFFFFFF8ull, true, 1, 0xFFFFFFFFFFFFFFF8ull},
        {0x00007FFFFFFF0000ull, false, 0, 0x00007FFFFFFF0000ull},
        {0x00007FFFFFFF0000ull, true, 1, 0x00007FFFFFFF0000ull},
    };
    GuestAddr code_at = kCode64;
    for (const Case& c : cases) {
        SCOPED_TRACE(::testing::Message() << std::hex << c.addr
                                          << (c.store ? " store" : " load"));
        const std::vector<u8> code = c.store
            ? std::vector<u8>{0x48, 0x89, 0x03}   // mov [rbx],rax
            : std::vector<u8>{0x48, 0x8B, 0x03};  // mov rax,[rbx]
        code_at += 0x40;
        ASSERT_TRUE(m_memory->write_bytes(code_at, code.data(), code.size()));
        BasicBlock block = build64(code);
        block.start_addr = code_at;
        CompiledBlock* compiled = m_jit->compile(block);
        ASSERT_NE(compiled, nullptr);
        CpuState cpu = cpu64(c.addr);
        cpu.rip = code_at;
        cpu.gpr.rax = 0x0123456789ABCDEFull;
        EXPECT_EQ(test::execute_like_dispatcher(*m_jit, *compiled, cpu), JitExecResult::Continue);
        ASSERT_TRUE(cpu.pending_raise.active);
        EXPECT_EQ(cpu.pending_raise.code, 0xC0000005u);
        EXPECT_EQ(cpu.pending_raise.args[0], c.info0);
        EXPECT_EQ(cpu.pending_raise.args[1], c.info1) << std::hex
            << cpu.pending_raise.args[1];
        EXPECT_EQ(cpu.gpr.rax, 0x0123456789ABCDEFull);
    }
}

// Range by fault (JitConfig::linear64_range_by_fault): no inline reservation
// test. The check map ends at the VA limit, so the unmasked map load of an EA
// above the window faults, and the handler redirects that one site to its
// range variant -- the old reservation test -- after which the checked helper
// serves it. A high-DLL address (above the 2^46 window, mapped through the
// scattered model) must still load and store correctly, before and after the
// redirect, and an in-window access through the redirected site still works.
TEST_F(JitLinear64Test, RangeByFaultSiteServesAddressesAboveTheWindow) {
    if (!m_memory->linear_check_map_ends_at_va_limit()) {
        GTEST_SKIP() << "check map not at the VA limit in this process";
    }
    if (const char* knob = std::getenv("VITREO_JIT_RANGE_FAULT");
        knob != nullptr && knob[0] == '0') {
        GTEST_SKIP() << "VITREO_JIT_RANGE_FAULT=0";
    }
    constexpr GuestAddr high = 0x00007FFA10000000ull;
    ASSERT_TRUE(m_memory->map_region(high, 0x4000, MemoryProtection::ReadWrite));
    struct Form {
        std::vector<u8> code;
        bool store;
        int width;
    };
    const Form forms[] = {
        {{0x48, 0x8B, 0x03}, false, 8},        // mov rax,[rbx]
        {{0x48, 0x89, 0x03}, true, 8},         // mov [rbx],rax
        {{0x8A, 0x03}, false, 1},              // mov al,[rbx]
        {{0x88, 0x03}, true, 1},               // mov [rbx],al
        {{0x0F, 0x10, 0x03}, false, 16},       // movups xmm0,[rbx]
        {{0x0F, 0x11, 0x03}, true, 16},        // movups [rbx],xmm0
        {{0x0F, 0x28, 0x03}, false, 16},       // movaps xmm0,[rbx]
    };
    GuestAddr code_at = kCode64;
    for (const Form& f : forms) {
        SCOPED_TRACE(::testing::Message() << "width=" << f.width
                                          << " store=" << f.store);
        code_at += 0x40;
        ASSERT_TRUE(m_memory->write_bytes(code_at, f.code.data(), f.code.size()));
        BasicBlock block = build64(f.code);
        block.start_addr = code_at;
        CompiledBlock* compiled = m_jit->compile(block);
        ASSERT_NE(compiled, nullptr);
        const u64 before = range_backpatch_count();
        // High, high again (through the patched site), then in-window.
        for (const GuestAddr at : {high + 0x100, high + 0x200, kData64 + 0x40}) {
            SCOPED_TRACE(::testing::Message() << std::hex << at);
            ASSERT_TRUE(m_memory->write_u64(at, 0x1122334455667788ull));
            ASSERT_TRUE(m_memory->write_u64(at + 8, 0x99AABBCCDDEEFF00ull));
            CpuState cpu = cpu64(at);
            cpu.rip = code_at;
            cpu.gpr.rax = 0x0123456789ABCDEFull;
            for (int i = 0; i < 16; ++i) {
                cpu.sse.xmm[0].u8_[i] = static_cast<u8>(0xC0u + i);
            }
            EXPECT_EQ(test::execute_like_dispatcher(*m_jit, *compiled, cpu), JitExecResult::Continue);
            ASSERT_FALSE(cpu.pending_raise.active);
            if (f.store) {
                const u64 want = f.width == 16 ? 0xC7C6C5C4C3C2C1C0ull
                               : f.width == 8 ? 0x0123456789ABCDEFull
                                              : 0x11223344556677EFull;
                EXPECT_EQ(m_memory->read_u64(at).value(), want);
            } else if (f.width == 16) {
                EXPECT_EQ(cpu.sse.xmm[0].u64_[0], 0x1122334455667788ull);
                EXPECT_EQ(cpu.sse.xmm[0].u64_[1], 0x99AABBCCDDEEFF00ull);
            } else {
                const u64 mask = f.width == 8 ? ~0ull : 0xFFull;
                EXPECT_EQ(cpu.gpr.rax & mask, 0x1122334455667788ull & mask);
            }
        }
        EXPECT_EQ(range_backpatch_count(), before + 1)
            << "the first above-window access redirects the site exactly once";
    }
    ASSERT_TRUE(m_memory->unmap_region(high, 0x4000));
}

// The 128-bit SSE memory operand in a long-mode block used to be the largest
// form with no inline lowering at all -- every MOVUPS called the C SSE helper.
// These two tests pin the guest-base lowering, and they run entirely above
// 4 GiB (kData64 = 6 GiB) because an EA truncated to 32 bits is silent below it
// (jit64_ea_truncation_bug_family).
TEST_F(JitLinear64Test, Movups128LoadAbove4GiBMatchesInterpreter) {
    std::array<u8, 16> bytes{};
    for (usize i = 0; i < bytes.size(); ++i) {
        bytes[i] = static_cast<u8>(0x11u * (i + 1));
    }
    ASSERT_TRUE(m_memory->write_bytes(kData64 + 0x40, bytes.data(),
                                      bytes.size()));
    const std::vector<u8> code = {0x0F, 0x10, 0x03};  // movups xmm0,[rbx]
    ASSERT_TRUE(m_memory->write_bytes(kCode64, code.data(), code.size()));
    BasicBlock block = build64(code);
    ASSERT_EQ(block.instruction_count, 1u);
    CompiledBlock* compiled = m_jit->compile(block);
    ASSERT_NE(compiled, nullptr);

    // A silent fall-back to the SSE helper would still produce the right value,
    // so assert the direct 16-byte access is really in the block.
    std::vector<u32> emitted(compiled->code_size / 4);
    std::memcpy(emitted.data(), compiled->code, compiled->code_size);
    EXPECT_TRUE(std::any_of(emitted.begin(), emitted.end(), [](u32 w) {
        return is_linear_q_access(w, false);
    })) << "128-bit linear64 load was not lowered inline";

    CpuState jcpu = cpu64(kData64 + 0x40);
    EXPECT_EQ(test::execute_like_dispatcher(*m_jit, *compiled, jcpu), JitExecResult::Continue);
    EXPECT_FALSE(jcpu.pending_raise.active);

    CpuState icpu = cpu64(kData64 + 0x40);
    Interpreter interp(icpu, *m_memory);
    (void)interp.run(1);
    for (usize i = 0; i < 16; ++i) {
        EXPECT_EQ(jcpu.sse.xmm[0].u8_[i], icpu.sse.xmm[0].u8_[i])
            << "byte " << i;
    }
}

TEST_F(JitLinear64Test, Movups128StoreAbove4GiBMatchesInterpreter) {
    const std::vector<u8> code = {0x0F, 0x11, 0x03};  // movups [rbx],xmm0
    ASSERT_TRUE(m_memory->write_bytes(kCode64, code.data(), code.size()));
    BasicBlock block = build64(code);
    ASSERT_EQ(block.instruction_count, 1u);
    CompiledBlock* compiled = m_jit->compile(block);
    ASSERT_NE(compiled, nullptr);

    std::vector<u32> emitted(compiled->code_size / 4);
    std::memcpy(emitted.data(), compiled->code, compiled->code_size);
    EXPECT_TRUE(std::any_of(emitted.begin(), emitted.end(), [](u32 w) {
        return is_linear_q_access(w, true);
    })) << "128-bit linear64 store was not lowered inline";

    const GuestAddr dst = kData64 + 0x60;
    CpuState jcpu = cpu64(dst);
    for (usize i = 0; i < 16; ++i) {
        jcpu.sse.xmm[0].u8_[i] = static_cast<u8>(0xA0u + i);
    }
    EXPECT_EQ(test::execute_like_dispatcher(*m_jit, *compiled, jcpu), JitExecResult::Continue);
    EXPECT_FALSE(jcpu.pending_raise.active);
    std::array<u8, 16> jit_bytes{};
    ASSERT_TRUE(m_memory->read_bytes(dst, jit_bytes.data(), jit_bytes.size()));

    ASSERT_TRUE(m_memory->write_u64(dst, 0));
    ASSERT_TRUE(m_memory->write_u64(dst + 8, 0));
    CpuState icpu = cpu64(dst);
    for (usize i = 0; i < 16; ++i) {
        icpu.sse.xmm[0].u8_[i] = static_cast<u8>(0xA0u + i);
    }
    Interpreter interp(icpu, *m_memory);
    (void)interp.run(1);
    std::array<u8, 16> interp_bytes{};
    ASSERT_TRUE(m_memory->read_bytes(dst, interp_bytes.data(),
                                     interp_bytes.size()));
    EXPECT_EQ(jit_bytes, interp_bytes);
}

// An unmapped 128-bit access must raise a guest AV carrying the untruncated
// 64-bit address, not silently alias into the low 4 GiB.
TEST_F(JitLinear64Test, Movups128LoadFromUnmappedAbove4GiBRaisesAv) {
    const std::vector<u8> code = {0x0F, 0x10, 0x03};  // movups xmm0,[rbx]
    ASSERT_TRUE(m_memory->write_bytes(kCode64, code.data(), code.size()));
    BasicBlock block = build64(code);
    CompiledBlock* compiled = m_jit->compile(block);
    ASSERT_NE(compiled, nullptr);

    CpuState cpu = cpu64(kUnmapped64);
    EXPECT_EQ(test::execute_like_dispatcher(*m_jit, *compiled, cpu), JitExecResult::Continue);
    ASSERT_TRUE(cpu.pending_raise.active);
    EXPECT_EQ(cpu.pending_raise.code, 0xC0000005u);
    ASSERT_EQ(cpu.pending_raise.num_args, 2u);
    EXPECT_EQ(cpu.pending_raise.args[1], kUnmapped64);
}

// The 32-byte path's cold stub: every fault a VEX.256 move can take must
// still arrive as a precise pending AV, and the faulting address must be the
// first byte that could not be accessed, exactly as the split form reported.
TEST_F(JitLinear64Test, Vex256LoadFromUnmappedAbove4GiBRaisesAv) {
    const std::vector<u8> code = {0xC5, 0xFE, 0x6F, 0x03};  // vmovdqu ymm0,[rbx]
    ASSERT_TRUE(m_memory->write_bytes(kCode64, code.data(), code.size()));
    BasicBlock block = build64(code);
    CompiledBlock* compiled = m_jit->compile(block);
    ASSERT_NE(compiled, nullptr);
    CpuState cpu = cpu64(kUnmapped64);
    EXPECT_EQ(test::execute_like_dispatcher(*m_jit, *compiled, cpu), JitExecResult::Continue);
    ASSERT_TRUE(cpu.pending_raise.active);
    EXPECT_EQ(cpu.pending_raise.code, 0xC0000005u);
    ASSERT_EQ(cpu.pending_raise.num_args, 2u);
    EXPECT_EQ(cpu.pending_raise.args[1], kUnmapped64);
}

TEST_F(JitLinear64Test, Vex256StoreToUnmappedAbove4GiBRaisesAv) {
    const std::vector<u8> code = {0xC5, 0xFE, 0x7F, 0x03};  // vmovdqu [rbx],ymm0
    ASSERT_TRUE(m_memory->write_bytes(kCode64, code.data(), code.size()));
    BasicBlock block = build64(code);
    CompiledBlock* compiled = m_jit->compile(block);
    ASSERT_NE(compiled, nullptr);
    CpuState cpu = cpu64(kUnmapped64);
    EXPECT_EQ(test::execute_like_dispatcher(*m_jit, *compiled, cpu), JitExecResult::Continue);
    ASSERT_TRUE(cpu.pending_raise.active);
    EXPECT_EQ(cpu.pending_raise.code, 0xC0000005u);
    ASSERT_EQ(cpu.pending_raise.num_args, 2u);
    EXPECT_EQ(cpu.pending_raise.args[1], kUnmapped64);
}

TEST_F(JitLinear64Test, Vex256LoadWhoseTailLeavesTheMappingReportsTheTailAddress) {
    // kData64 maps 0x4000 bytes; start 16 bytes before the end so bytes 0..15
    // are mapped and 16..31 are not. The AV names the first unmapped byte.
    const std::vector<u8> code = {0xC5, 0xFE, 0x6F, 0x03};  // vmovdqu ymm0,[rbx]
    ASSERT_TRUE(m_memory->write_bytes(kCode64, code.data(), code.size()));
    BasicBlock block = build64(code);
    CompiledBlock* compiled = m_jit->compile(block);
    ASSERT_NE(compiled, nullptr);
    const GuestAddr start = kData64 + 0x4000 - 16;
    CpuState cpu = cpu64(start);
    EXPECT_EQ(test::execute_like_dispatcher(*m_jit, *compiled, cpu), JitExecResult::Continue);
    ASSERT_TRUE(cpu.pending_raise.active);
    EXPECT_EQ(cpu.pending_raise.code, 0xC0000005u);
    ASSERT_EQ(cpu.pending_raise.num_args, 2u);
    EXPECT_EQ(cpu.pending_raise.args[1], start + 16);
}

TEST_F(JitLinear64Test, Vex256MisalignedVmovdqaStoreRaisesGpAndAlignedOneDoesNot) {
    const std::vector<u8> code = {0xC5, 0xFD, 0x7F, 0x03};  // vmovdqa [rbx],ymm0
    ASSERT_TRUE(m_memory->write_bytes(kCode64, code.data(), code.size()));
    BasicBlock block = build64(code);
    CompiledBlock* compiled = m_jit->compile(block);
    ASSERT_NE(compiled, nullptr);
    CpuState bad = cpu64(kData64 + 8);
    EXPECT_EQ(test::execute_like_dispatcher(*m_jit, *compiled, bad), JitExecResult::Continue);
    ASSERT_TRUE(bad.pending_raise.active);
    EXPECT_EQ(bad.pending_raise.code, 0xC0000005u);
    CpuState good = cpu64(kData64 + 32);
    EXPECT_EQ(test::execute_like_dispatcher(*m_jit, *compiled, good), JitExecResult::Continue);
    EXPECT_FALSE(good.pending_raise.active);
}

// The 32-byte cold path must move the DATA correctly, not only raise. A
// PAGE_NOACCESS 4 KiB page inside the 16 KiB host page makes that host page
// "mixed", which sends every access to it through the helper stub. The first
// version of the stub held one half in a V register across the other half's
// helper calls, which clobber V0-V7: raises were precise and the data was
// garbage. Steam's renderer died of it in two seconds.
TEST_F(JitLinear64Test, Vex256LoadThroughAMixedHostPageMovesAll32Bytes) {
    ASSERT_TRUE(m_memory->protect_region(kData64 + 0x1000, 0x1000,
                                         MemoryProtection::None));
    const std::vector<u8> code = {
        0xC5, 0xFE, 0x6F, 0x0A,  // vmovdqu ymm1,[rdx]   (rdx: mixed page, cold)
        0xC5, 0xFE, 0x7F, 0x09,  // vmovdqu [rcx],ymm1   (rcx: clean page, hot)
    };
    ASSERT_TRUE(m_memory->write_bytes(kCode64, code.data(), code.size()));
    BasicBlock block = build64(code);
    CompiledBlock* compiled = m_jit->compile(block);
    ASSERT_NE(compiled, nullptr);
    std::array<u8, 32> bytes{};
    for (usize i = 0; i < bytes.size(); ++i) bytes[i] = static_cast<u8>(0x60u + i);
    const GuestAddr src = kData64 + 0x2000;            // RW, same host page as the NOACCESS one
    const GuestAddr dst = kData64 + 0x4000 + 0x100;    // second mapped region? no: stay in region
    ASSERT_TRUE(m_memory->write_bytes(src, bytes.data(), bytes.size()));
    CpuState cpu = cpu64(src);
    cpu.gpr.rdx = src;
    cpu.gpr.rcx = kData64 + 0x3800;                     // RW, hot store target
    EXPECT_EQ(test::execute_like_dispatcher(*m_jit, *compiled, cpu), JitExecResult::Continue);
    ASSERT_FALSE(cpu.pending_raise.active);
    std::array<u8, 32> out{};
    ASSERT_TRUE(m_memory->read_bytes(kData64 + 0x3800, out.data(), out.size()).is_ok());
    EXPECT_EQ(out, bytes);
    (void)dst;
}

TEST_F(JitLinear64Test, Vex256StoreThroughAMixedHostPageMovesAll32Bytes) {
    ASSERT_TRUE(m_memory->protect_region(kData64 + 0x1000, 0x1000,
                                         MemoryProtection::None));
    const std::vector<u8> code = {
        0xC5, 0xFE, 0x6F, 0x0A,  // vmovdqu ymm1,[rdx]   (rdx: clean page, hot)
        0xC5, 0xFE, 0x7F, 0x09,  // vmovdqu [rcx],ymm1   (rcx: mixed page, cold)
    };
    ASSERT_TRUE(m_memory->write_bytes(kCode64, code.data(), code.size()));
    BasicBlock block = build64(code);
    CompiledBlock* compiled = m_jit->compile(block);
    ASSERT_NE(compiled, nullptr);
    std::array<u8, 32> bytes{};
    for (usize i = 0; i < bytes.size(); ++i) bytes[i] = static_cast<u8>(0x90u + i);
    const GuestAddr src = kData64 + 0x3800;             // clean host page
    const GuestAddr dst = kData64 + 0x2000;             // mixed host page, RW sub-page
    ASSERT_TRUE(m_memory->write_bytes(src, bytes.data(), bytes.size()));
    CpuState cpu = cpu64(src);
    cpu.gpr.rdx = src;
    cpu.gpr.rcx = dst;
    EXPECT_EQ(test::execute_like_dispatcher(*m_jit, *compiled, cpu), JitExecResult::Continue);
    ASSERT_FALSE(cpu.pending_raise.active);
    std::array<u8, 32> out{};
    ASSERT_TRUE(m_memory->read_bytes(dst, out.data(), out.size()).is_ok());
    EXPECT_EQ(out, bytes);
}

TEST_F(JitLinear64Test, Vex256RegisterCopyMovesBothLanesInFourQWords) {
    const std::vector<u8> code = {
        0xC5, 0xFE, 0x6F, 0x0A,  // vmovdqu ymm1,[rdx]
        0xC5, 0xFD, 0x6F, 0xD1,  // vmovdqa ymm2,ymm1
        0xC5, 0xFE, 0x7F, 0x11,  // vmovdqu [rcx],ymm2
    };
    ASSERT_TRUE(m_memory->write_bytes(kCode64, code.data(), code.size()));
    BasicBlock block = build64(code);
    ASSERT_EQ(block.instruction_count, 3u);
    CompiledBlock* compiled = m_jit->compile(block);
    ASSERT_NE(compiled, nullptr);
    const auto* words = static_cast<const u32*>(compiled->code);
    const usize n = compiled->code_size / sizeof(u32);
    // The register copy is two LDR Q + two STR Q from CpuState; no LDR X of
    // the XMM slots remains (the eight-word split form).
    EXPECT_GE(std::count_if(words, words + n, [](u32 w) {
        return (w & 0xFFC00000u) == 0x3DC00000u;   // LDR Q, unsigned offset
    }), 3);
    std::array<u8, 32> bytes{};
    for (usize i = 0; i < bytes.size(); ++i) bytes[i] = static_cast<u8>(0xC0u + i);
    ASSERT_TRUE(m_memory->write_bytes(kData64, bytes.data(), bytes.size()));
    CpuState cpu = cpu64(kData64);
    cpu.gpr.rdx = kData64;
    cpu.gpr.rcx = kData64 + 0x200;
    EXPECT_EQ(test::execute_like_dispatcher(*m_jit, *compiled, cpu), JitExecResult::Continue);
    ASSERT_FALSE(cpu.pending_raise.active);
    std::array<u8, 32> out{};
    ASSERT_TRUE(m_memory->read_bytes(kData64 + 0x200, out.data(), out.size()).is_ok());
    EXPECT_EQ(out, bytes);
}

TEST_F(JitLinear64Test, ScalarLoadWidthsAbove4GiBMatchX64MovSemantics) {
    constexpr u64 marker = 0xFEDCBA9876543210ull;
    ASSERT_TRUE(m_memory->write_bytes(kData64,
                                      reinterpret_cast<const u8*>(&marker),
                                      sizeof(marker)));

    struct Case {
        std::vector<u8> code;
        u64 expected;
    };
    const std::vector<Case> cases = {
        {{0x8A, 0x03}, 0x10ull},             // mov al,[rbx]
        {{0x66, 0x8B, 0x03}, 0x3210ull},     // mov ax,[rbx]
        {{0x8B, 0x03}, 0x76543210ull},       // mov eax,[rbx]
        {{0x48, 0x8B, 0x03}, marker},        // mov rax,[rbx]
    };

    for (const Case& c : cases) {
        ASSERT_TRUE(m_memory->write_bytes(kCode64, c.code.data(),
                                          c.code.size()));
        BasicBlock block = build64(c.code);
        ASSERT_EQ(block.instruction_count, 1u);
        CompiledBlock* compiled = m_jit->compile(block);
        ASSERT_NE(compiled, nullptr);
        CpuState cpu = cpu64(kData64);
        EXPECT_EQ(test::execute_like_dispatcher(*m_jit, *compiled, cpu), JitExecResult::Continue);
        EXPECT_FALSE(cpu.pending_raise.active);
        EXPECT_EQ(cpu.gpr.get64(EAX), c.expected);
    }
}

}  // namespace
