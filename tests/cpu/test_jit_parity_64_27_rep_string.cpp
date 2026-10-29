// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// JIT vs Interpreter parity tests — 64-bit long mode (CPU-048): REP
// MOVS/STOS/CMPS/SCAS.

#include "jit_parity_64_fixture.hpp"

// ============================================================================
// REP MOVS / STOS / CMPS / SCAS in long mode
//
// Intel SDM Vol. 1 3.3.7 and Vol. 2 (MOVS/STOS/CMPS/SCAS, REP): in 64-bit
// mode the default address size is 64, so the count is RCX and the pointers
// are RSI/RDI in full width, independently of the ELEMENT width -- REP MOVSB
// walks 64-bit RSI/RDI one byte at a time. DF (RFLAGS bit 10) selects the
// step sign, and a REP whose count register is zero executes nothing at all:
// RSI/RDI are not stepped and no memory is touched.
//
// The JIT lowers short-count REP MOVS/STOS to an inline unrolled loop and
// keeps the C++ bulk helper for long counts, so each form is exercised below
// and above that threshold, in both directions, at every width.
// ============================================================================

namespace {

constexpr GuestAddr kRepSrc  = 0x3000;
constexpr GuestAddr kRepDst  = 0x6000;
constexpr usize     kRepSpan = 0x800;
// Forward runs start low in the span, backward runs start high, so a 65-element
// qword walk (520 bytes) stays inside the span in either direction.
constexpr usize     kRepFwd  = 0x080;
constexpr usize     kRepBack = 0x700;

struct RepForm {
    const char* name;
    std::vector<u8> code;
    u32 elem;
};

}  // namespace

TEST_F(JitParity64Test, NativeRepMovsMatchesInterpreterAtEveryWidthAndDirection) {
    m_compare_memory = true;
    const RepForm forms[] = {
        {"REP MOVSB", {0xF3, 0xA4}, 1},
        {"REP MOVSW", {0xF3, 0x66, 0xA5}, 2},
        {"REP MOVSD", {0xF3, 0xA5}, 4},
        {"REP MOVSQ", {0xF3, 0x48, 0xA5}, 8},
    };
    for (const RepForm& f : forms) {
        // 0/1/3 sit under the inline threshold (65 does not), so both the
        // unrolled loop and its bulk-helper tail are exercised per form.
        for (u64 count : {0ull, 1ull, 3ull, 65ull}) {
            for (bool df : {false, true}) {
                SCOPED_TRACE(std::string(f.name) + " count=" +
                             std::to_string(count) +
                             " df=" + std::to_string(df ? 1 : 0));
                // A fresh compiler per case: this loop rewrites 0x1000 with a
                // different encoding each time, which the churn guard would
                // otherwise read as self-modifying code and route to the
                // interpreter, silently defeating require_jit.
                m_jit = std::make_unique<JitCompiler>(*m_memory);
                std::vector<u8> s(kRepSpan), d(kRepSpan);
                for (usize i = 0; i < kRepSpan; ++i) {
                    s[i] = static_cast<u8>((i * 7 + f.elem) ^ 0x5A);
                    d[i] = static_cast<u8>((i * 13 + f.elem) ^ 0xA5);
                }
                ASSERT_TRUE(m_memory->write_bytes(kRepSrc, s.data(), s.size()));
                ASSERT_TRUE(m_memory->write_bytes(kRepDst, d.data(), d.size()));

                const u64 si = kRepSrc + (df ? kRepBack : kRepFwd);
                const u64 di = kRepDst + (df ? kRepBack : kRepFwd);
                ScopedEnvironmentVariable no_delegate(
                    "VITREO_JIT_NO_DELEGATE", "1");
                check_parity(f.code.data(), f.code.size(),
                    [&](CpuState& cpu) {
                        cpu.gpr.set64(RSI, si);
                        cpu.gpr.set64(RDI, di);
                        cpu.gpr.set64(RCX, count);
                        cpu.flags.set_df(df);
                    },
                    /*check_flags=*/false, /*check_xmm=*/false,
                    /*require_jit=*/true);
                if (::testing::Test::HasFatalFailure()) return;
                ASSERT_TRUE(m_last_parity_ok);

                // Absolute SDM contract, not just interpreter agreement.
                const i64 step = df ? -static_cast<i64>(f.elem)
                                    : static_cast<i64>(f.elem);
                EXPECT_EQ(m_last_jit_cpu.gpr.get64(RCX), 0u);
                EXPECT_EQ(m_last_jit_cpu.gpr.get64(RSI),
                          si + static_cast<u64>(step * static_cast<i64>(count)));
                EXPECT_EQ(m_last_jit_cpu.gpr.get64(RDI),
                          di + static_cast<u64>(step * static_cast<i64>(count)));
                if (count == 0) {
                    // "REP with count 0: no operation is performed" -- the
                    // pointers must be untouched, not stepped once.
                    EXPECT_EQ(m_last_jit_cpu.gpr.get64(RSI), si);
                    EXPECT_EQ(m_last_jit_cpu.gpr.get64(RDI), di);
                    std::vector<u8> after(kRepSpan);
                    ASSERT_TRUE(m_memory->read_bytes(kRepDst, after.data(),
                                                     after.size()));
                    EXPECT_EQ(after, d) << "count 0 wrote guest memory";
                }
            }
        }
    }
}

TEST_F(JitParity64Test, NativeRepStosMatchesInterpreterAtEveryWidthAndDirection) {
    m_compare_memory = true;
    const RepForm forms[] = {
        {"REP STOSB", {0xF3, 0xAA}, 1},
        {"REP STOSW", {0xF3, 0x66, 0xAB}, 2},
        {"REP STOSD", {0xF3, 0xAB}, 4},
        {"REP STOSQ", {0xF3, 0x48, 0xAB}, 8},
    };
    for (const RepForm& f : forms) {
        for (u64 count : {0ull, 1ull, 3ull, 65ull}) {
            for (bool df : {false, true}) {
                SCOPED_TRACE(std::string(f.name) + " count=" +
                             std::to_string(count) +
                             " df=" + std::to_string(df ? 1 : 0));
                // A fresh compiler per case: this loop rewrites 0x1000 with a
                // different encoding each time, which the churn guard would
                // otherwise read as self-modifying code and route to the
                // interpreter, silently defeating require_jit.
                m_jit = std::make_unique<JitCompiler>(*m_memory);
                std::vector<u8> d(kRepSpan);
                for (usize i = 0; i < kRepSpan; ++i) {
                    d[i] = static_cast<u8>((i * 11 + f.elem) ^ 0x3C);
                }
                ASSERT_TRUE(m_memory->write_bytes(kRepDst, d.data(), d.size()));

                const u64 di = kRepDst + (df ? kRepBack : kRepFwd);
                ScopedEnvironmentVariable no_delegate(
                    "VITREO_JIT_NO_DELEGATE", "1");
                check_parity(f.code.data(), f.code.size(),
                    [&](CpuState& cpu) {
                        // A value whose every byte differs, so a width bug
                        // (STOSB writing 4 bytes, STOSD writing 8) shows up.
                        cpu.gpr.set64(RAX, 0x8877665544332211ull);
                        cpu.gpr.set64(RDI, di);
                        cpu.gpr.set64(RCX, count);
                        cpu.flags.set_df(df);
                    },
                    /*check_flags=*/false, /*check_xmm=*/false,
                    /*require_jit=*/true);
                if (::testing::Test::HasFatalFailure()) return;
                ASSERT_TRUE(m_last_parity_ok);

                const i64 step = df ? -static_cast<i64>(f.elem)
                                    : static_cast<i64>(f.elem);
                EXPECT_EQ(m_last_jit_cpu.gpr.get64(RCX), 0u);
                EXPECT_EQ(m_last_jit_cpu.gpr.get64(RDI),
                          di + static_cast<u64>(step * static_cast<i64>(count)));
                EXPECT_EQ(m_last_jit_cpu.gpr.get64(RAX), 0x8877665544332211ull);
                if (count == 0) {
                    EXPECT_EQ(m_last_jit_cpu.gpr.get64(RDI), di);
                    std::vector<u8> after(kRepSpan);
                    ASSERT_TRUE(m_memory->read_bytes(kRepDst, after.data(),
                                                     after.size()));
                    EXPECT_EQ(after, d) << "count 0 wrote guest memory";
                }
            }
        }
    }
}

TEST_F(JitParity64Test, NativeRepCmpsAndScasMatchInterpreterAtEveryWidth) {
    // CMPS/SCAS keep the bulk C++ helper in both bitnesses (each iteration
    // publishes flags and may exit early on ZF, which the inline unrolled
    // loop does not model). These cases pin that helper's long-mode
    // register/direction/flag contract so the MOVS/STOS work above cannot
    // regress it unnoticed.
    m_compare_memory = true;
    const RepForm forms[] = {
        {"REPE CMPSB",  {0xF3, 0xA6}, 1},
        {"REPE CMPSW",  {0xF3, 0x66, 0xA7}, 2},
        {"REPE CMPSD",  {0xF3, 0xA7}, 4},
        {"REPE CMPSQ",  {0xF3, 0x48, 0xA7}, 8},
        {"REPNE CMPSB", {0xF2, 0xA6}, 1},
        {"REPNE CMPSW", {0xF2, 0x66, 0xA7}, 2},
        {"REPNE CMPSD", {0xF2, 0xA7}, 4},
        {"REPNE CMPSQ", {0xF2, 0x48, 0xA7}, 8},
        {"REPE SCASB",  {0xF3, 0xAE}, 1},
        {"REPE SCASW",  {0xF3, 0x66, 0xAF}, 2},
        {"REPE SCASD",  {0xF3, 0xAF}, 4},
        {"REPE SCASQ",  {0xF3, 0x48, 0xAF}, 8},
        {"REPNE SCASB", {0xF2, 0xAE}, 1},
        {"REPNE SCASW", {0xF2, 0x66, 0xAF}, 2},
        {"REPNE SCASD", {0xF2, 0xAF}, 4},
        {"REPNE SCASQ", {0xF2, 0x48, 0xAF}, 8},
    };
    for (const RepForm& f : forms) {
        for (u64 count : {0ull, 1ull, 3ull, 65ull}) {
            for (bool df : {false, true}) {
                SCOPED_TRACE(std::string(f.name) + " count=" +
                             std::to_string(count) +
                             " df=" + std::to_string(df ? 1 : 0));
                // A fresh compiler per case: this loop rewrites 0x1000 with a
                // different encoding each time, which the churn guard would
                // otherwise read as self-modifying code and route to the
                // interpreter, silently defeating require_jit.
                m_jit = std::make_unique<JitCompiler>(*m_memory);
                // dst mirrors src, with element 2 of each direction's walk
                // flipped: REPE stops there, REPNE stops on the first match.
                std::vector<u8> s(kRepSpan), d(kRepSpan);
                for (usize i = 0; i < kRepSpan; ++i) {
                    s[i] = static_cast<u8>((i * 7 + f.elem) ^ 0x5A);
                    d[i] = s[i];
                }
                for (u32 b = 0; b < f.elem; ++b) {
                    d[kRepFwd + 2 * f.elem + b] ^= 0xFF;
                    d[kRepBack - 2 * f.elem + b] ^= 0xFF;
                }
                ASSERT_TRUE(m_memory->write_bytes(kRepSrc, s.data(), s.size()));
                ASSERT_TRUE(m_memory->write_bytes(kRepDst, d.data(), d.size()));

                const u64 si = kRepSrc + (df ? kRepBack : kRepFwd);
                const u64 di = kRepDst + (df ? kRepBack : kRepFwd);
                // SCAS scans [RDI] against RAX. Seed RAX from the element the
                // walk reaches third, so REPNE terminates mid-walk instead of
                // running the count out.
                u64 scan = 0;
                const usize hit = (df ? kRepBack - 2 * f.elem
                                      : kRepFwd + 2 * f.elem);
                std::memcpy(&scan, d.data() + hit, f.elem);

                ScopedEnvironmentVariable no_delegate(
                    "VITREO_JIT_NO_DELEGATE", "1");
                check_parity(f.code.data(), f.code.size(),
                    [&](CpuState& cpu) {
                        cpu.gpr.set64(RAX, scan);
                        cpu.gpr.set64(RSI, si);
                        cpu.gpr.set64(RDI, di);
                        cpu.gpr.set64(RCX, count);
                        cpu.flags.set_df(df);
                    },
                    /*check_flags=*/true, /*check_xmm=*/false,
                    /*require_jit=*/true);
                if (::testing::Test::HasFatalFailure()) return;
                ASSERT_TRUE(m_last_parity_ok);

                if (count == 0) {
                    // Neither pointer moves and the count stays zero.
                    EXPECT_EQ(m_last_jit_cpu.gpr.get64(RSI), si);
                    EXPECT_EQ(m_last_jit_cpu.gpr.get64(RDI), di);
                    EXPECT_EQ(m_last_jit_cpu.gpr.get64(RCX), 0u);
                }
            }
        }
    }
}

TEST_F(JitParity64Test, RepMovsStosEmitInlineShortPathInLongMode) {
    // Reached-ness guard. The inline short-REP loop was gated off for 64-bit
    // blocks; nothing about a passing parity test would notice it silently
    // reverting to a bare bulk-helper call, because the helper is correct.
    vitreo::diagnostics::perf_window_enable_for_tests();
    vitreo::diagnostics::perf_window_start();
    const std::vector<std::vector<u8>> forms = {
        {0xF3, 0xA4},        // REP MOVSB
        {0xF3, 0x66, 0xA5},  // REP MOVSW
        {0xF3, 0xA5},        // REP MOVSD
        {0xF3, 0x48, 0xA5},  // REP MOVSQ
        {0xF3, 0xAA},        // REP STOSB
        {0xF3, 0x66, 0xAB},  // REP STOSW
        {0xF3, 0xAB},        // REP STOSD
        {0xF3, 0x48, 0xAB},  // REP STOSQ
    };
    // The window is process-global and other tests in this binary compile REP
    // forms too, so assert on this test's own deltas, never absolute totals.
    const auto base = vitreo::diagnostics::perf_window_snapshot();
    for (const auto& code : forms) {
        const u64 before =
            vitreo::diagnostics::perf_window_snapshot().rep_emit_inline;
        // A fresh compiler per form: the churn guard would otherwise see
        // 0x1000 rewritten eight times and route it to the interpreter.
        m_jit = std::make_unique<JitCompiler>(*m_memory);
        ASSERT_TRUE(m_memory->write_bytes(0x1000, code.data(), code.size()));
        BasicBlock block = build_block(code.data(), code.size());
        ASSERT_EQ(block.instruction_count, 1u);
        ASSERT_NE(m_jit->compile(block), nullptr);
        const auto snap = vitreo::diagnostics::perf_window_snapshot();
        EXPECT_GT(snap.rep_emit_inline, before)
            << "this REP form fell back to the bulk helper";
    }
    const auto after = vitreo::diagnostics::perf_window_snapshot();
    EXPECT_EQ(after.rep_emit_inline - base.rep_emit_inline, forms.size());
    EXPECT_EQ(after.rep_emit_helper, base.rep_emit_helper);
}

// A 64-bit block never published a CarrySource, so every CF capture whose
// producer's NZCV had died paid the eight-way runtime FlagOp dispatch — with
// the answer already known at compile time. Two things had to be true for the
// named formula to be reached, and this test asserts both: the 64-bit lazy
// recorder must NAME the source, and a flag-neutral instruction that clobbers
// host NZCV must FORWARD it.
//
// Parity alone cannot see this: the dispatch is correct, just long. Assert
// reached-ness on the emit counters as well, or the wire can be removed and
// every test stays green.
TEST_F(JitParity64Test, LazyCarrySourceIsNamedAndForwardedInLongMode) {
    vitreo::diagnostics::perf_window_enable_for_tests();
    vitreo::diagnostics::perf_window_start();

    // `bswap rcx` and `push rbx; pop rbx` are "Flags Affected: None" in Intel
    // SDM Vol. 2 but do not preserve ARM64 PSTATE, which is exactly the gap:
    // NZCV is gone, the lazy record is not.
    const std::vector<u8> kBswap = {0x48, 0x0F, 0xC9};
    const std::vector<u8> kPushPop = {0x53, 0x5B};

    struct Producer { const char* name; std::vector<u8> code; };
    const std::vector<Producer> producers = {
        {"add rax,rbx", {0x48, 0x01, 0xD8}},
        {"sub rax,rbx", {0x48, 0x29, 0xD8}},
        {"and rax,rbx", {0x48, 0x21, 0xD8}},
        {"or  rax,rbx", {0x48, 0x09, 0xD8}},
        {"xor rax,rbx", {0x48, 0x31, 0xD8}},
    };
    // Asymmetric, and chosen to straddle every CF boundary: no carry, carry
    // out of the top bit, borrow, exact-equality, and the all-ones edge.
    const std::vector<std::pair<u64, u64>> operands = {
        {1, 2},
        {5, 3},
        {0xFFFFFFFFFFFFFFFFull, 1},
        {0x8000000000000000ull, 0x8000000000000000ull},
        {0, 1},
        {0x123456789ABCDEFull, 0xFEDCBA987654321ull},
        {0x100000000ull, 0xFFFFFFFFull},
        {42, 42},
    };

    u64 known = 0;
    u64 dispatch = 0;
    for (const auto& sep : {kBswap, kPushPop}) {
        for (const auto& prod : producers) {
            for (const auto& [a, b] : operands) {
                std::vector<u8> code = prod.code;
                code.insert(code.end(), sep.begin(), sep.end());
                // adc rcx, 0 — consumes CF and nothing else.
                const u8 adc[] = {0x48, 0x83, 0xD1, 0x00};
                code.insert(code.end(), std::begin(adc), std::end(adc));

                m_jit = std::make_unique<JitCompiler>(*m_memory);
                const auto before =
                    vitreo::diagnostics::perf_window_snapshot();
                check_parity(code.data(), code.size(),
                             [a = a, b = b](CpuState& cpu) {
                                 cpu.gpr.set64(RAX, a);
                                 cpu.gpr.set64(RBX, b);
                                 cpu.gpr.set64(RCX, 0x1111111111111111ull);
                             },
                             /*check_flags=*/true, /*check_xmm=*/false,
                             /*require_jit=*/true);
                ASSERT_FALSE(::testing::Test::HasFailure())
                    << prod.name << " a=" << a << " b=" << b;
                const auto after =
                    vitreo::diagnostics::perf_window_snapshot();
                known += after.cf_emit_known - before.cf_emit_known;
                dispatch +=
                    after.cf_emit_dispatch - before.cf_emit_dispatch;
            }
        }
    }

    const usize cases = 2 * producers.size() * operands.size();
    EXPECT_EQ(known, cases)
        << "a 64-bit CF capture stopped reaching the named-CarrySource "
           "formula; the publisher or the forward was removed";
    EXPECT_EQ(dispatch, 0u)
        << "a 64-bit CF capture fell back to the eight-way FlagOp dispatch";
}

// flag_liveness now retires a publisher's lazy record when the very next
// instruction is an ADC/SBB, and retires an ADC/SBB link when the next is
// another link or an adjacent full-flag writer. The carry then lives only in
// PSTATE across the chain.
//
// Everything that can go wrong here is invisible to a value-only comparison:
// AF and PF are the two flags the NZCV kind bits cannot carry, and a CLC/STC
// overlay that outlives its owner wins over the PSTATE carry and silently
// flips CF. So this sweeps with check_af on, and brackets every chain with
// the three overlay writers.
TEST_F(JitParity64Test, AdcSbbChainRetiresLazyRecordsWithoutLosingFlags) {
    const std::vector<u8> add1 = {0x83, 0xC0, 0x01};  // add eax, 1
    const std::vector<u8> adc0 = {0x83, 0xD2, 0x00};  // adc edx, 0
    const std::vector<u8> sbb0 = {0x83, 0xD9, 0x00};  // sbb ecx, 0
    const std::vector<u8> adcr = {0x11, 0xDA};        // adc edx, ebx
    const std::vector<u8> sbbr = {0x19, 0xD9};        // sbb ecx, ebx
    const std::vector<u8> sub1 = {0x83, 0xE8, 0x01};  // sub eax, 1
    const std::vector<u8> and1 = {0x83, 0xE0, 0x0F};  // and eax, 15
    const std::vector<u8> cmp1 = {0x83, 0xF8, 0x07};  // cmp eax, 7

    struct Chain { const char* name; std::vector<std::vector<u8>> parts; };
    const std::vector<Chain> chains = {
        {"add;adc",             {add1, adc0}},
        {"add;adc;sbb",         {add1, adc0, sbb0}},
        {"add;adc;adc;sbb",     {add1, adc0, adc0, sbb0}},
        {"sub;sbb;adc",         {sub1, sbb0, adc0}},
        {"add;adc(reg);sbb(reg)", {add1, adcr, sbbr}},
        // A chain link retired by an adjacent full-flag writer, not by
        // another link.
        {"add;adc;and",         {add1, adc0, and1}},
        {"add;adc;sub",         {add1, adc0, sub1}},
        {"sub;sbb;cmp",         {sub1, sbb0, cmp1}},
        // Two full triples: the second triple's publisher is retired by a
        // link whose own record was retired.
        {"2x(add;adc;sbb)",     {add1, adc0, sbb0, add1, adc0, sbb0}},
        // Nothing adjacent to retire the tail — the last link must publish.
        {"add;adc;nop",         {add1, adc0, {0x90}}},
        // HIGH-REGISTER destinations. R8-R15 have no pin (pinned_gpr32
        // rejects reg >= 8), so these take the ADC/SBB SLOW path, and that
        // path reads the CF/OF overlay before anything else. A retired
        // publisher that skipped its overlay clear leaves a stale CLC/STC
        // winning over the PSTATE carry here — the one shape where the
        // omission is observable, and the reason the record and the overlay
        // are retired separately.
        {"add;adc(r8d)",        {add1, {0x41,0x83,0xD0,0x00}}},
        {"add;adc(r8d);sbb(r9d)",
                                {add1, {0x41,0x83,0xD0,0x00},
                                 {0x41,0x83,0xD9,0x00}}},
        {"sub;sbb(r9d);adc(r8d)",
                                {sub1, {0x41,0x83,0xD9,0x00},
                                 {0x41,0x83,0xD0,0x00}}},
    };
    // CF/OF overlay writers. An overlay installed before the chain must be
    // retired by the chain's first publisher even though its record is not
    // stored; one installed nowhere is the control.
    struct Prefix { const char* name; std::vector<u8> code; };
    const std::vector<Prefix> prefixes = {
        {"none", {}},
        {"clc",  {0xF8}},
        {"stc",  {0xF9}},
        {"stc;cmc", {0xF9, 0xF5}},
    };
    // Straddle the CF, AF (bit 4 / nibble) and PF (low-byte parity)
    // boundaries, and keep the three registers asymmetric so a chain that
    // reads the wrong limb cannot pass.
    const std::vector<std::array<u64, 3>> regs = {
        {0x00000000u, 0x00000000u, 0x00000000u},
        {0xFFFFFFFFu, 0x00000000u, 0x00000001u},
        {0x0000000Fu, 0x000000F0u, 0x00000007u},
        {0x7FFFFFFFu, 0x80000000u, 0xFFFFFFFFu},
        {0x0000FFFFu, 0x00010000u, 0x0000000Fu},
        {0x12345678u, 0x9ABCDEF0u, 0x0FEDCBA9u},
        {0xFFFFFFFEu, 0xFFFFFFFFu, 0x00000002u},
        {0x00000001u, 0x00000001u, 0x00000001u},
    };

    for (const auto& pre : prefixes) {
        for (const auto& ch : chains) {
            std::vector<u8> code = pre.code;
            for (const auto& part : ch.parts) {
                code.insert(code.end(), part.begin(), part.end());
            }
            for (const auto& r : regs) {
                m_jit = std::make_unique<JitCompiler>(*m_memory);
                check_parity(code.data(), code.size(),
                             [&r](CpuState& cpu) {
                                 cpu.gpr.set64(RAX, r[0]);
                                 cpu.gpr.set64(RDX, r[1]);
                                 cpu.gpr.set64(RCX, r[2]);
                                 cpu.gpr.set64(RBX, 0xA5A5A5A5u);
                                 cpu.gpr.set64(R8, 0x0BADF00Du);
                                 cpu.gpr.set64(
                                     static_cast<u8>(RegIndex::R9),
                                     0xDEADBEEFu);
                             },
                             /*check_flags=*/true, /*check_xmm=*/false,
                             /*require_jit=*/false, /*check_af=*/true);
                ASSERT_FALSE(::testing::Test::HasFailure())
                    << pre.name << " / " << ch.name
                    << " eax=" << std::hex << r[0] << " edx=" << r[1]
                    << " ecx=" << r[2];
            }
        }
    }
}

// A REP string operation that walks off the end of a mapping must FAULT.
// The C++ REP helpers used to stop quietly instead: STOS discarded the write
// result outright and then cleared RCX, so a fill into an unmapped page was
// indistinguishable from one that completed, and LODS returned with RSI/RCX at
// their ENTRY values, losing the whole walk. Intel SDM Vol. 2 (STOS/LODS) with
// Vol. 3 paging: #PF on the faulting element, which Windows delivers as
// STATUS_ACCESS_VIOLATION, and the registers must be left restartable there.
//
// The count deliberately exceeds kShortRepElems (64) so the inline short-REP
// loop declines and the C++ helper is the thing under test.
TEST_F(JitParity64Test, RepStringWalkingOffAMappingFaultsAndCommitsProgress) {
    // SetUp maps [0x1000, 0x11000); everything at or above 0x11000 is unmapped.
    constexpr u64 kEnd = 0x11000;
    constexpr u64 kStart = kEnd - 0x100;   // 64 dwords still inside
    constexpr u64 kCount = 1000;

    struct Case {
        const char* name;
        std::vector<u8> code;
        u8 ptr_reg;       // RDI for STOS, RSI for LODS
        bool is_write;
    };
    const std::vector<Case> cases = {
        {"rep stosd", {0xF3, 0xAB}, RDI, true},
        {"rep movsd", {0xF3, 0xA5}, RDI, true},
        {"rep lodsd", {0xF3, 0xAD}, RSI, false},
        {"rep scasd", {0xF3, 0xAF}, RDI, false},
    };

    for (const auto& c : cases) {
        m_jit = std::make_unique<JitCompiler>(*m_memory);
        ASSERT_TRUE(m_memory->write_bytes(0x1000, c.code.data(),
                                          c.code.size()));
        BasicBlock block = build_block(c.code.data(), c.code.size());
        CompiledBlock* cb = m_jit->compile(block);
        ASSERT_NE(cb, nullptr) << c.name;

        CpuState cpu;
        init_cpu(cpu);
        cpu.gpr.set64(RAX, 0x11223344u);
        cpu.gpr.set64(RCX, kCount);
        // MOVS reads RSI and writes RDI; point both at the straddle so the
        // faulting side is unambiguous. SCAS must not match before the edge,
        // so the needle in EAX is not a value the mapped region holds.
        cpu.gpr.set64(RSI, kStart);
        cpu.gpr.set64(RDI, kStart);
        cpu.rip = 0x1000;
        test::execute_like_dispatcher(*m_jit, *cb, cpu);

        ASSERT_TRUE(cpu.pending_raise.active)
            << c.name << " stopped silently instead of faulting";
        EXPECT_TRUE(cpu.pending_raise.from_helper) << c.name;
        EXPECT_EQ(cpu.pending_raise.code, 0xC0000005u) << c.name;
        EXPECT_EQ(cpu.pending_raise.num_args, 2u) << c.name;
        EXPECT_EQ(cpu.pending_raise.args[1], kEnd)
            << c.name << ": fault must name the first unmapped element";

        // Restartable: the pointer sits ON the faulting element and the count
        // reflects only what was really processed.
        constexpr u64 kDone = (kEnd - kStart) / 4;
        EXPECT_EQ(cpu.gpr.get64(c.ptr_reg), kEnd)
            << c.name << ": pointer not committed at the faulting element";
        EXPECT_EQ(cpu.gpr.get64(RCX), kCount - kDone)
            << c.name << ": count does not reflect the elements processed";
    }
}

TEST_F(JitParity64Test, NativeRepMovsStosAboveFourGigDoNotTruncatePointers) {
    // The inline short-REP loop steps RSI/RDI itself, so it must step them at
    // ADDRESS width. Every case above runs below 4 GB, where a 32-bit step is
    // indistinguishable from a 64-bit one -- exactly the truncation this
    // lowering must not have. Run the same forms at 0x2_4000_0000 /
    // 0x3_4000_0000, whose low 32 bits both land in a mapped shadow region:
    // a truncating step writes the shadow, and the shadow is snapshotted and
    // byte-compared against the interpreter.
    constexpr GuestAddr kHighSrc = 0x240000000ull;
    constexpr GuestAddr kHighDst = 0x340000000ull;
    constexpr GuestAddr kShadow  = 0x40000000ull;  // low 32 bits of both
    constexpr usize kHighSpan = 0x1000;
    map_tracked_region(kHighSrc, kHighSpan);
    map_tracked_region(kHighDst, kHighSpan);
    map_tracked_region(kShadow, kHighSpan);
    m_compare_memory = true;

    const RepForm forms[] = {
        {"REP MOVSB", {0xF3, 0xA4}, 1},
        {"REP MOVSW", {0xF3, 0x66, 0xA5}, 2},
        {"REP MOVSD", {0xF3, 0xA5}, 4},
        {"REP MOVSQ", {0xF3, 0x48, 0xA5}, 8},
        {"REP STOSB", {0xF3, 0xAA}, 1},
        {"REP STOSW", {0xF3, 0x66, 0xAB}, 2},
        {"REP STOSD", {0xF3, 0xAB}, 4},
        {"REP STOSQ", {0xF3, 0x48, 0xAB}, 8},
    };
    for (const RepForm& f : forms) {
        const bool stos = f.name[4] == 'S';
        for (u64 count : {1ull, 3ull, 65ull}) {
            for (bool df : {false, true}) {
                SCOPED_TRACE(std::string(f.name) + " high count=" +
                             std::to_string(count) +
                             " df=" + std::to_string(df ? 1 : 0));
                m_jit = std::make_unique<JitCompiler>(*m_memory);
                std::vector<u8> s(kHighSpan), d(kHighSpan), z(kHighSpan, 0x7E);
                for (usize i = 0; i < kHighSpan; ++i) {
                    s[i] = static_cast<u8>((i * 7 + f.elem) ^ 0x5A);
                    d[i] = static_cast<u8>((i * 13 + f.elem) ^ 0xA5);
                }
                ASSERT_TRUE(m_memory->write_bytes(kHighSrc, s.data(), s.size()));
                ASSERT_TRUE(m_memory->write_bytes(kHighDst, d.data(), d.size()));
                ASSERT_TRUE(m_memory->write_bytes(kShadow, z.data(), z.size()));

                const u64 si = kHighSrc + (df ? kRepBack : kRepFwd);
                const u64 di = kHighDst + (df ? kRepBack : kRepFwd);
                ScopedEnvironmentVariable no_delegate(
                    "VITREO_JIT_NO_DELEGATE", "1");
                check_parity(f.code.data(), f.code.size(),
                    [&](CpuState& cpu) {
                        cpu.gpr.set64(RAX, 0x8877665544332211ull);
                        cpu.gpr.set64(RSI, si);
                        cpu.gpr.set64(RDI, di);
                        cpu.gpr.set64(RCX, count);
                        cpu.flags.set_df(df);
                    },
                    /*check_flags=*/false, /*check_xmm=*/false,
                    /*require_jit=*/true);
                if (::testing::Test::HasFatalFailure()) return;
                ASSERT_TRUE(m_last_parity_ok);

                const i64 step = df ? -static_cast<i64>(f.elem)
                                    : static_cast<i64>(f.elem);
                const u64 walked =
                    static_cast<u64>(step * static_cast<i64>(count));
                EXPECT_EQ(m_last_jit_cpu.gpr.get64(RCX), 0u);
                EXPECT_EQ(m_last_jit_cpu.gpr.get64(RDI), di + walked);
                if (!stos) {
                    EXPECT_EQ(m_last_jit_cpu.gpr.get64(RSI), si + walked);
                }
                // The shadow page must be exactly as seeded: a truncated EA is
                // the only thing that could have written it.
                std::vector<u8> shadow(kHighSpan);
                ASSERT_TRUE(m_memory->read_bytes(kShadow, shadow.data(),
                                                 shadow.size()));
                EXPECT_EQ(shadow, z) << "a REP step truncated to 32 bits";
            }
        }
    }
}
