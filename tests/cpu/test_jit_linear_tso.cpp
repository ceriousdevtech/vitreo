// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
//
// 32-bit ordered-TSO 16-byte-granule straddles and the ESP-frame form. Split
// from test_jit_linear.cpp; shared fixtures and matchers live in
// jit_linear_fixture.hpp.

#include "jit_linear_fixture.hpp"

using namespace vitreo;
using namespace vitreo::cpu;
using namespace vitreo::cpu::jit;
using namespace vitreo::cpu::jit::test;

// x86 permits a 4-byte access at ANY address; a 16-byte-granule crossing is
// ordinary guest code (packed structs, unaligned memcpy tails). ARM's ordered
// forms do not: with FEAT_LSE2 and SCTLR.nAA=0, LDAPR/LDAR/STLR raise an
// Alignment fault (SIGBUS) the moment an access straddles a 16-byte granule.
//
// The long-mode lowering handles that with the two-word fault-patchable site
// (CrossingScalar{Load,Store}BackpatchesExactlyOnce above). The 32-bit
// linear/region lowering has no such site: emit_linear_{load,store} emit a
// BARE LDAPR/STLR under TsoMode::Ordered, and ordered_tso_site() will not
// recognise a site with no marker neighbour -- so the SIGBUS is reported to
// the guest as a spurious STATUS_ACCESS_VIOLATION and the store is lost.
//
// These two run under the shipping default (Ordered when the fault handler
// installs). They also pass under Fence. They fail if Ordered is forced
// WITHOUT fault-patchable two-word sites (emit_x64_tso_*).
namespace {
TEST_F(JitLinearTest, DefaultTsoStore32StraddlingGranuleCompletes) {
    const GuestAddr target = kData + 0x20D;
    ASSERT_EQ(reinterpret_cast<uintptr_t>(m_memory->host_ptr(target, 4)) & 0xFu,
              0xDu);
    std::vector<u8> code = {0x89, 0x03};  // MOV [EBX], EAX
    ASSERT_TRUE(m_memory->write_bytes(kCode, code.data(), code.size()));
    BasicBlock block = build_block(code);
    CpuState cpu;
    init_cpu(cpu);
    cpu.gpr.set32(EBX, static_cast<u32>(target));
    cpu.gpr.set32(EAX, 0xCAFEBABE);
    CompiledBlock* cb = m_jit->compile(block);
    ASSERT_NE(cb, nullptr);
    (void)test::execute_like_dispatcher(*m_jit, *cb, cpu);
    EXPECT_FALSE(cpu.pending_raise.active);
    u32 got = 0;
    ASSERT_TRUE(m_memory->read_bytes(target, reinterpret_cast<u8*>(&got), 4));
    EXPECT_EQ(got, 0xCAFEBABEu);
}

TEST_F(JitLinearTest, DefaultTsoLoad32StraddlingGranuleCompletes) {
    const GuestAddr target = kData + 0x30D;
    const u32 value = 0xDEADBEEF;
    ASSERT_TRUE(m_memory->write_bytes(target,
                                      reinterpret_cast<const u8*>(&value), 4));
    std::vector<u8> code = {0x8B, 0x03};  // MOV EAX, [EBX]
    ASSERT_TRUE(m_memory->write_bytes(kCode, code.data(), code.size()));
    BasicBlock block = build_block(code);
    CpuState cpu;
    init_cpu(cpu);
    cpu.gpr.set32(EBX, static_cast<u32>(target));
    CompiledBlock* cb = m_jit->compile(block);
    ASSERT_NE(cb, nullptr);
    (void)test::execute_like_dispatcher(*m_jit, *cb, cpu);
    EXPECT_FALSE(cpu.pending_raise.active);
    EXPECT_EQ(cpu.gpr.get32(EAX), value);
}

TEST_F(JitLinearTest, OrderedEspDispLoad32IsAcquireNotPlainLdr) {
    // Regression: the [esp+disp] lowering used a plain LDR plus
    // emit_tso_barrier(); under Ordered the barrier is a no-op, so stack
    // locals were unordered while stores already used STLR - Sims hung mid-
    // load waiting on spin flags.
    //
    // The contract is the ORDERING of the access, not which host register
    // holds its address. try_esp_disp_load32's X15 frame form is default-off
    // (VITREO_JIT_ESP_FRAME), so this load is currently addressed through the
    // generic guest-address scratch; asserting the acquire form only at
    // REG_ESP_FRAME/REG_SCRATCH4 made the test fail on correctly ordered code
    // and would have kept passing had the generic path lost its acquire.
    ScopedEnvironmentFlag ordered("VITREO_JIT_TSO_ORDERED", "1");
    m_jit = std::make_unique<JitCompiler>(*m_memory);
    m_jit->set_block_cache(m_cache.get());

    const std::vector<u8> code = {0x8B, 0x44, 0x24, 0x04};  // mov eax,[esp+4]
    ASSERT_TRUE(m_memory->write_bytes(kCode, code.data(), code.size()));
    BasicBlock block = build_block(code);
    CompiledBlock* cb = m_jit->compile(block);
    ASSERT_NE(cb, nullptr);

    const u32 n = cb->code_size / 4;
    const auto* words = reinterpret_cast<const u32*>(cb->code);
    bool saw_acquire = false;
    bool saw_plain_frame_ldr = false;
    for (u32 i = 0; i < n; ++i) {
        for (u8 addr_reg = 0; addr_reg < 32 && !saw_acquire; ++addr_reg) {
            if (is_acquire_load32(words[i], addr_reg)) saw_acquire = true;
        }
        // Scaled uoff #1 = +4 bytes - the old unordered ESP-frame form.
        if ((words[i] & ~0x1Fu) ==
            (arm64::ldr_w_uoff(0, REG_ESP_FRAME, 1) & ~0x1Fu)) {
            saw_plain_frame_ldr = true;
        }
    }
    EXPECT_TRUE(saw_acquire)
        << "under Ordered the [esp+4] load must carry its ordering in the "
           "access, whichever register holds the folded host address";
    EXPECT_FALSE(saw_plain_frame_ldr)
        << "a plain LDR off the ESP frame is the unordered form this guards";
}

// The opt-in ESP-frame form serves [esp+disp] for every 4-aligned disp up to
// 16380 (the reach of LDR/STR W's scaled offset). Under Ordered it folds
// esp_frame + disp into one register for LDAPR/STLR -- and that ADD's
// immediate is 12 bits. Folding disp >= 4096 through it stored to
// esp + (disp mod 4096). Intel SDM Vol. 2 MOV: the destination is the full
// effective address ESP + disp32.
//
// VITREO_JIT_ESP_FRAME is parsed once per process, and the form is Tier 0's
// while linear 32-bit blocks default to Tier 1, so this runs in its own ctest
// entry (vitreo_cpu_tests_esp_frame_on: VITREO_JIT_ESP_FRAME=1,
// VITREO_JIT_IR=0) and skips elsewhere.
TEST_F(JitLinearTest, OrderedEspFrameAccessesReachPastTwelveBitDisplacements) {
    const char* frame = std::getenv("VITREO_JIT_ESP_FRAME");
    const char* ir = std::getenv("VITREO_JIT_IR");
    if (frame == nullptr || std::strcmp(frame, "1") != 0 || ir == nullptr ||
        std::strcmp(ir, "0") != 0) {
        GTEST_SKIP() << "the ESP-frame form is opt-in and Tier 0 only; ctest "
                        "runs this as vitreo_cpu_tests_esp_frame_on";
    }
    ScopedEnvironmentFlag ordered("VITREO_JIT_TSO_ORDERED", "1");
    constexpr u32 kStack = static_cast<u32>(kData);
    constexpr u32 kValue = 0xC0DE'F00Du;
    constexpr u32 kCanary = 0x5A5A'5A5Au;
    const u32 establisher =
        arm64::add_x_uxtw(REG_ESP_FRAME, REG_LINEAR_BASE, REG_PIN_ESP);
    for (const u32 disp : {4092u, 4096u, 0x2000u, 16380u}) {
        SCOPED_TRACE(::testing::Message() << "[esp+0x" << std::hex << disp << "]");
        for (u32 off = 0; off < kDataSize; off += 4) {
            ASSERT_TRUE(m_memory->write_u32(kData + off, kCanary));
        }
        const auto run = [&](const std::vector<u8>& code, CpuState& cpu) {
            m_jit = std::make_unique<JitCompiler>(*m_memory);
            m_jit->set_block_cache(m_cache.get());
            ASSERT_TRUE(m_memory->write_bytes(kCode, code.data(), code.size()));
            BasicBlock block = build_block(code);
            CompiledBlock* cb = m_jit->compile(block);
            ASSERT_NE(cb, nullptr);
            const auto* words = reinterpret_cast<const u32*>(cb->code);
            bool framed = false;
            for (u32 i = 0; i < cb->code_size / 4; ++i) {
                framed = framed || words[i] == establisher;
            }
            ASSERT_FALSE(m_jit->ir_emit_active())
                << "Tier 1 took the block; the ESP-frame form is Tier 0's "
                   "(run with VITREO_JIT_IR=0)";
            ASSERT_TRUE(framed) << "VITREO_JIT_ESP_FRAME=1 but the access did "
                                   "not take the ESP-frame form";
            (void)test::execute_like_dispatcher(*m_jit, *cb, cpu);
            EXPECT_FALSE(cpu.pending_raise.active);
        };
        const u8 d0 = static_cast<u8>(disp), d1 = static_cast<u8>(disp >> 8);
        {
            CpuState cpu;
            init_cpu(cpu);
            cpu.gpr.set32(ESP, kStack);
            cpu.gpr.set32(EAX, kValue);
            run({0x89, 0x84, 0x24, d0, d1, 0x00, 0x00}, cpu);  // mov [esp+disp32], eax
            EXPECT_EQ(m_memory->read_u32(kStack + disp).value_or(0), kValue)
                << "the store must land at ESP + disp";
            for (u32 off = 0; off < kDataSize; off += 4) {
                if (off == disp) continue;
                ASSERT_EQ(m_memory->read_u32(kData + off).value_or(0), kCanary)
                    << "stray store at [esp+0x" << std::hex << off << "]";
            }
        }
        {
            CpuState cpu;
            init_cpu(cpu);
            cpu.gpr.set32(ESP, kStack);
            run({0x8B, 0x8C, 0x24, d0, d1, 0x00, 0x00}, cpu);  // mov ecx, [esp+disp32]
            EXPECT_EQ(cpu.gpr.get32(static_cast<u8>(RegIndex::RCX)), kValue)
                << "the load must read ESP + disp";
        }
    }
}}  // namespace
