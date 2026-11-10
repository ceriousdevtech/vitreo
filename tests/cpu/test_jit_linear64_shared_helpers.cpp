// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
//
// The process-wide linear64 dispatch tails and the shared cold helper. Split
// from test_jit_linear.cpp; shared fixtures and matchers live in
// jit_linear_fixture.hpp.

#include "jit_linear_fixture.hpp"
#include "core/math/host_fenv.hpp"
#include <optional>

using namespace vitreo;
using namespace vitreo::cpu;
using namespace vitreo::cpu::jit;
using namespace vitreo::cpu::jit::test;

namespace {

class JitApiCrossingTest : public JitLinear64Test,
                           public ::testing::WithParamInterface<bool> {
protected:
    bool use_view() const override { return GetParam(); }
    void SetUp() override {
        if (const char* value = std::getenv("VITREO_JIT_VIEW")) previous_view = value;
        setenv("VITREO_JIT_VIEW", use_view() ? "1" : "0", 1);
        JitLinear64Test::SetUp();
        JitCompiler::enable_jit_link();
        JitCompiler::set_inchain_thunks_codegen_for_test(1);
    }
    void TearDown() override {
        JitCompiler::disable_jit_link();
        JitCompiler::set_inchain_thunks_codegen_for_test(-1);
        JitCompiler::set_shared_exit_stubs_for_test(-1);
        JitLinear64Test::TearDown();
        if (previous_view) setenv("VITREO_JIT_VIEW", previous_view->c_str(), 1);
        else unsetenv("VITREO_JIT_VIEW");
    }
    std::optional<std::string> previous_view;
};

TEST_P(JitApiCrossingTest, SharedApiCrossingRemovesPerBlockCallBody) {
    const std::vector<u8> code = {
        0x48, 0x39, 0xD1,                   // cmp rcx,rdx
        0xFF, 0x15, 0x00, 0x00, 0x00, 0x00 // call [rip]
    };
    ASSERT_TRUE(m_memory->write_bytes(kCode64, code.data(), code.size()));
    const u32 trampoline = arm64::ldr_x_uoff(
        16, REG_JIT_CTX, offsetof(JitContext, inchain_trampoline) >> 3);
    std::array<usize, 2> bytes{};
    for (int shared = 0; shared < 2; ++shared) {
        SCOPED_TRACE(shared);
        JitCompiler::set_shared_exit_stubs_for_test(shared);
        m_jit = std::make_unique<JitCompiler>(*m_memory);
        m_jit->set_block_cache(m_cache.get());
        m_jit->set_thunk_region(0x70000000u, 0x70001000u);
        for (int copy = 0; copy < 2; ++copy) {
            auto block = build64(code);
            block.start_addr += copy * 0x40;
            auto* compiled = m_jit->compile(block);
            ASSERT_NE(compiled, nullptr);
            usize calls = 0;
            for (const auto& range : block_word_ranges(*compiled)) {
                calls += std::count(range.begin, range.end, trampoline);
            }
            EXPECT_EQ(calls, shared ? 0u : 1u);
            if (copy == 0) bytes[shared] = compiled->code_size + compiled->cold_size;
        }
    }
    EXPECT_LT(bytes[1] + 64u, bytes[0]);
    std::cout << "[api-crossing] per-block bytes legacy=" << bytes[0]
              << " shared=" << bytes[1] << '\n';
}

TEST_P(JitApiCrossingTest, CrossingsPreserveInterpreterStateAndHopCap) {
    constexpr GuestAddr thunk = 0x70000000u;
    struct alignas(8) SlotMap {
        u64 count{1};
        u8 eligible{1};
    } slots;
    std::vector<u8> code = {
        0x48, 0x8B, 0x3B,             // mov rdi,[rbx]
        0x48, 0x83, 0xD6, 0x00,       // adc rsi,0
        0x49, 0x01, 0xFC,             // add r12,rdi
        0x48, 0x39, 0xD1,             // cmp rcx,rdx
        0xFF, 0xE0                    // jmp rax
    };
    ASSERT_TRUE(m_memory->write_u64(kData64, 0x1234));
    const InChainThunkResult outcomes[] = {
        InChainThunkResult::Returned, InChainThunkResult::NotServed,
        InChainThunkResult::Resume, InChainThunkResult::BreakOuter,
        InChainThunkResult::Fatal
    };
    for (bool call : {false, true}) {
    code.back() = call ? 0xD0 : 0xE0; // call rax / jmp rax
    ASSERT_TRUE(m_memory->write_bytes(kCode64, code.data(), code.size()));
    for (int shared = 0; shared < 2; ++shared) {
        for (auto outcome : outcomes) {
            SCOPED_TRACE(::testing::Message() << "call=" << call << " shared=" << shared
                         << " outcome=" << static_cast<int>(outcome));
            JitCompiler::set_shared_exit_stubs_for_test(shared);
            m_jit->set_block_cache(nullptr);
            m_cache->clear();
            m_jit = std::make_unique<JitCompiler>(*m_memory);
            m_jit->set_block_cache(m_cache.get());
            m_jit->set_thunk_region(thunk, thunk + (1u << kInChainSlotShift));
            m_jit->set_inchain_slot_map(reinterpret_cast<const u8*>(&slots));
            auto* cached = m_cache->insert(build64(code));
            ASSERT_NE(cached, nullptr);
            auto* compiled = m_jit->compile(*cached);
            ASSERT_NE(compiled, nullptr);
            CompiledBlock entry = *compiled;
            m_jit->publish_compiled_block(*cached, *compiled);

            CpuState cpu = cpu64(kData64);
            cpu.gpr.r[0] = thunk;
            cpu.gpr.r[1] = 1;
            cpu.gpr.r[2] = 0x200000;
            cpu.gpr.r[ESP] = kData64 + 0x2000;
            cpu.flags.set_cf(true);
            struct Probe {
                GuestMemory& memory;
                CpuState expected;
                InChainThunkResult outcome;
                bool call;
                u32 calls{0};
            } probe{*m_memory, cpu, outcome, call};
            const auto hook = [](CpuState& current, void* raw) {
                auto& p = *static_cast<Probe*>(raw);
                Interpreter interpreter(p.expected, p.memory);
                EXPECT_NE(interpreter.run(5), ExecResult::Exception);
                CpuState actual = current;
                actual.materialize_lazy_flags();
                p.expected.materialize_lazy_flags();
                EXPECT_EQ(actual.rip, p.expected.rip);
                EXPECT_EQ(actual.flags.rflags & STATUS_FLAGS_MASK,
                          p.expected.flags.rflags & STATUS_FLAGS_MASK);
                for (u8 g = 0; g < 16; ++g) {
                    EXPECT_EQ(actual.gpr.r[g], p.expected.gpr.r[g]) << "r" << int(g);
                }
                ++p.calls;
                if (::testing::Test::HasFailure()) return InChainThunkResult::BreakOuter;
                if (p.outcome != InChainThunkResult::NotServed) {
                    current.rip = p.expected.rip = kCode64;
                    if (p.call) {
                        current.gpr.r[ESP] += sizeof(u64);
                        p.expected.gpr.r[ESP] += sizeof(u64);
                    }
                    current.gpr.r[1] += 3;
                    p.expected.gpr.r[1] += 3;
                    ++current.gpr.r[12];
                    ++p.expected.gpr.r[12];
                }
                return p.outcome;
            };
            JitCompiler::InChainScope crossing(m_jit.get(), hook, &probe);
            EXPECT_EQ(test::execute_like_dispatcher(*m_jit, entry, cpu),
                      JitExecResult::Continue);
            if (outcome == InChainThunkResult::Returned) {
                EXPECT_GT(probe.calls, 1000u);
                EXPECT_EQ(cpu.jit_iters, 4096u);
            } else {
                EXPECT_EQ(probe.calls, 1u);
            }
            cpu.materialize_lazy_flags();
            EXPECT_EQ(cpu.rip, probe.expected.rip);
            EXPECT_EQ(cpu.flags.rflags & STATUS_FLAGS_MASK,
                      probe.expected.flags.rflags & STATUS_FLAGS_MASK);
            for (u8 g = 0; g < 16; ++g) {
                EXPECT_EQ(cpu.gpr.r[g], probe.expected.gpr.r[g]) << "r" << int(g);
            }
            const auto expected_break = outcome == InChainThunkResult::Fatal
                ? InChainBreak::Fatal
                : outcome == InChainThunkResult::BreakOuter
                    ? InChainBreak::Outer : InChainBreak::None;
            EXPECT_EQ(m_jit->take_inchain_break(), expected_break);
        }
    }
    }
}

TEST_P(JitApiCrossingTest, LinkedFpDemandRestoresModeAfterNativeCrossing) {
    constexpr GuestAddr thunk = 0x70000000u;
    constexpr GuestAddr successor = kCode64 + 0x100;
    struct alignas(8) SlotMap {
        u64 count{1};
        u8 eligible{1};
    } slots;
    const std::vector<u8> call = {
        0x48, 0x39, 0xD1, // cmp rcx,rdx
        0xFF, 0xD0        // call rax
    };
    // Intel SDM Vol. 1 8.1.5 / 10.2.3: rounding +1.5 under each RC.
    constexpr i32 expected[] = {2, 1, 2, 1};
    const std::vector<std::vector<u8>> fp_forms = {
        {0xF3, 0x0F, 0x2D, 0xD0},       // cvtss2si edx,xmm0
        {0xD9, 0xFC},                    // frndint
        {0x0F, 0xC2, 0xC1, 0x00},       // cmpps xmm0,xmm1,eq
        {0x66, 0x0F, 0xC2, 0xC1, 0x00}, // cmppd xmm0,xmm1,eq
        {0xF3, 0x0F, 0xC2, 0xC1, 0x00}, // cmpss xmm0,xmm1,eq
        {0xF2, 0x0F, 0xC2, 0xC1, 0x00}, // cmpsd xmm0,xmm1,eq
        {0x0F, 0x2F, 0xC1},             // comiss xmm0,xmm1
        {0x0F, 0x2E, 0xC1},             // ucomiss xmm0,xmm1
        {0x66, 0x0F, 0x2F, 0xC1},       // comisd xmm0,xmm1
        {0x66, 0x0F, 0x2E, 0xC1},       // ucomisd xmm0,xmm1
        {0xC5, 0xFC, 0xC2, 0xC1, 0x00}, // vcmpps ymm0,ymm0,ymm1,eq
        {0xF3, 0x0F, 0xC2, 0x03, 0x00}, // cmpss xmm0,[rbx],eq
        {0x66, 0x0F, 0x3A, 0x08, 0xC0, 0x04}, // roundps xmm0,xmm0,MXCSR
        {0x66, 0x0F, 0x3A, 0x09, 0xC0, 0x04}, // roundpd xmm0,xmm0,MXCSR
        {0x66, 0x0F, 0x3A, 0x0A, 0xC0, 0x04}, // roundss xmm0,xmm0,MXCSR
        {0x66, 0x0F, 0x3A, 0x0B, 0xC0, 0x04}, // roundsd xmm0,xmm0,MXCSR
        {0x66, 0x0F, 0x3A, 0x40, 0xC1, 0xF1}, // dpps xmm0,xmm1,lane0
        {0x66, 0x0F, 0x3A, 0x41, 0xC1, 0x31}, // dppd xmm0,xmm1,lane0
        {0xC4, 0xE2, 0x79, 0x13, 0xC1}, // vcvtph2ps xmm0,xmm1
        {0xC4, 0xE2, 0x7D, 0x13, 0xC1}, // vcvtph2ps ymm0,xmm1
    };
    ASSERT_TRUE(m_memory->write_u64(kData64, 0));
    for (usize instruction = 0; instruction < fp_forms.size(); ++instruction) {
        const bool x87 = instruction == 1;
        const bool compare = instruction >= 2 && instruction < 12;
        const bool flags_compare = instruction >= 6 && instruction <= 9;
        const bool round = instruction >= 12 && instruction < 16;
        const bool dot = instruction == 16 || instruction == 17;
        const bool half = instruction >= 18;
        const bool double_compare = instruction == 3 || instruction == 5 ||
                                    instruction == 8 || instruction == 9 ||
                                    instruction == 13 || instruction == 15 ||
                                    instruction == 17;
        for (u32 rc = 0; rc < 4; ++rc) {
            SCOPED_TRACE(::testing::Message() << "instruction=" << instruction
                         << " RC=" << rc);
            m_jit->set_block_cache(nullptr);
            m_cache->clear();
            m_jit = std::make_unique<JitCompiler>(*m_memory);
            m_jit->set_block_cache(m_cache.get());
            m_jit->set_thunk_region(thunk, thunk + (1u << kInChainSlotShift));
            m_jit->set_inchain_slot_map(reinterpret_cast<const u8*>(&slots));
            const auto& fp = fp_forms[instruction];
            ASSERT_TRUE(m_memory->write_bytes(successor, fp.data(), fp.size()));
            auto fp_block = build64(fp);
            fp_block.start_addr = successor;
            auto* fp_cached = m_cache->insert(std::move(fp_block));
            ASSERT_NE(fp_cached, nullptr);
            auto* fp_compiled = m_jit->compile(*fp_cached);
            ASSERT_NE(fp_compiled, nullptr);
            m_jit->publish_compiled_block(*fp_cached, *fp_compiled);
            ASSERT_TRUE(m_memory->write_bytes(kCode64, call.data(), call.size()));
            auto* cached = m_cache->insert(build64(call));
            ASSERT_NE(cached, nullptr);
            auto* compiled = m_jit->compile(*cached);
            ASSERT_NE(compiled, nullptr);
            CompiledBlock entry = *compiled;
            m_jit->publish_compiled_block(*cached, *compiled);

            CpuState cpu = cpu64(kData64);
            cpu.gpr.rax = thunk;
            cpu.gpr.rcx = 1;
            cpu.gpr.rdx = flags_compare ? 2 : 1;
            cpu.gpr.rsp = kData64 + 0x2000;
            cpu.fpu.control_word = static_cast<u16>(0x037Fu | (rc << 10));
            cpu.fpu.push(Float80(1.5));
            cpu.sse.mxcsr = 0x1F80u | (rc << 13);
            cpu.sse.xmm[0].u32_[0] = 0x3FC00000u;
            if (compare) {
                // Intel SDM Vol. 1 10.2.3.4: DAZ makes a positive subnormal
                // compare equal to zero, independent of RC.
                cpu.sse.mxcsr |= 0x0040u;
                cpu.sse.xmm[0] = {};
                cpu.sse.xmm[1] = {};
                cpu.sse.xmm[0].u64_[0] = 1;
            }
            if (round && double_compare) {
                cpu.sse.xmm[0].u64_[0] = 0x3FF8000000000000ull; // +1.5
            }
            if (round && rc == 2) {
                cpu.sse.mxcsr |= 0x0040u;
                cpu.sse.xmm[0] = {};
                cpu.sse.xmm[0].u64_[0] = 1;
            }
            if (dot) {
                cpu.sse.mxcsr |= 0x0040u;
                cpu.sse.xmm[0] = {};
                cpu.sse.xmm[1] = {};
                cpu.sse.xmm[0].u64_[0] = 1;
                cpu.sse.xmm[1].u64_[0] = double_compare
                    ? 0x3FF0000000000000ull : 0x3F800000u;
            }
            if (half) {
                // SDM VCVTPH2PS: DAZ is ignored; SNaN is quieted and raises IE.
                // Same source values as SatisfactoryVcvtph2psMemoryMatchesSdm.
                cpu.sse.mxcsr |= 0x8040u;
                cpu.sse.xmm[1].u32_[0] = 0x7D000001u;
            }
            unsigned calls = 0;
            const auto hook = [](CpuState& current, void* raw) {
                ++*static_cast<unsigned*>(raw);
                {
                    math::HostFpEnvironmentScope native(
                        current.fpu.control_word, current.sse.mxcsr,
                        math::HostFpEnvironmentScope::Restore::OnGuestFpUse);
                }
                current.rip = successor;
                current.gpr.rsp += sizeof(u64);
                return InChainThunkResult::Returned;
            };
            JitCompiler::InChainScope crossing(m_jit.get(), hook, &calls);
            math::clear_host_fp_exceptions();
            EXPECT_EQ(test::execute_like_dispatcher(*m_jit, entry, cpu),
                      JitExecResult::Continue);
            EXPECT_EQ(calls, 1u);
            if (compare) {
                if (flags_compare) {
                    EXPECT_EQ(cpu.sse.xmm[0].u64_[0], 1u);
                } else if (double_compare) {
                    EXPECT_EQ(cpu.sse.xmm[0].u64_[0], ~u64{0});
                } else {
                    EXPECT_EQ(cpu.sse.xmm[0].u32_[0], 0xFFFFFFFFu);
                }
            } else if (round) {
                if (rc == 2) {
                    EXPECT_EQ(cpu.sse.xmm[0].u64_[0], 0u);
                } else if (double_compare) {
                    EXPECT_EQ(cpu.sse.xmm[0].u64_[0], expected[rc] == 2
                        ? 0x4000000000000000ull : 0x3FF0000000000000ull);
                } else {
                    EXPECT_EQ(cpu.sse.xmm[0].u32_[0], expected[rc] == 2
                        ? 0x40000000u : 0x3F800000u);
                }
            } else if (dot) {
                EXPECT_EQ(cpu.sse.xmm[0].u64_[0], 0u);
            } else if (half) {
                EXPECT_EQ(cpu.sse.xmm[0].u32_[0], 0x33800000u);
                EXPECT_EQ(cpu.sse.xmm[0].u32_[1], 0x7FE00000u);
                EXPECT_EQ(math::harvest_sse_exceptions() & 1u, 1u);
                EXPECT_EQ(math::harvest_x87_exceptions(), 0u);
            } else if (x87) {
                EXPECT_DOUBLE_EQ(cpu.fpu.get_st(0).to_double(), expected[rc]);
            } else {
                EXPECT_EQ(static_cast<u32>(cpu.gpr.rdx),
                          static_cast<u32>(expected[rc]));
            }
            cpu.materialize_lazy_flags();
            EXPECT_TRUE(cpu.flags.zf());
            if (flags_compare) {
                EXPECT_FALSE(cpu.flags.cf());
                EXPECT_FALSE(cpu.flags.pf());
            }
            EXPECT_EQ(cpu.gpr.rsp, kData64 + 0x2000);
        }
    }
    math::apply_sse_rounding(0x1F80u);
    math::clear_host_fp_exceptions();
}

INSTANTIATE_TEST_SUITE_P(MemoryModels, JitApiCrossingTest, ::testing::Bool());

// Intel SDM Vol. 3A 6.6 through the process-wide scalar helper that every
// site's cold stub calls (VITREO_JIT_COLD_STUB_LEGACY unset): a fault is
// precise -- RIP names the faulting instruction, registers and RFLAGS are
// what the instruction before it left, and a faulting store commits nothing.
// `add rcx,rdx ; <access [rbx]> ; inc rsi` runs for every scalar width and
// the sign/zero-extending loads against a Mixed host page (guest pages 0 and
// 1 RW, 2 NOACCESS, 3 read-only) followed by unmapped memory: inside an
// allowed page, unaligned across a 16-byte granule and a guest page, from
// read-only, from the denied page, straddling into it, and across the host
// page into unmapped memory. Every outcome, including the AV's
// ExceptionInformation, is the interpreter's. Each case runs twice: the
// second run executes any ordered access the first rewrote in place.
// x86 guests do not reach these stubs (their linear model has its own store
// stubs and no hardware protection by default; run.cpp).
// The process-wide dispatcher tails and entry prologue of a standard-pin x64
// block (JitContext::dispatch_tail) leave exactly the architectural state the
// per-block copies did: the interpreter's GPRs, RIP, flags and exception for
// a Continue exit with live and with carried flags, a precise raise, and an
// indirect RET exit, under both emissions.
TEST_F(JitLinear64Test, SharedDispatchTailsMatchInterpreterInBothModes) {
    struct Restore {
        ~Restore() { JitCompiler::set_shared_exit_stubs_for_test(-1); }
    } restore;
    const GuestAddr data = kData64;
    ASSERT_TRUE(m_memory->protect_region(data + 0x2000, 0x1000,
                                         MemoryProtection::None));
    struct Form {
        const char* name;
        std::vector<u8> code;
        GuestAddr rbx;
    };
    const Form forms[] = {
        // add rcx,rdx ; mov [rbx],rax ; cmp rsi,rdi ; jmp +0x20
        {"store then cmp, jmp exit",
         {0x48, 0x01, 0xD1, 0x48, 0x89, 0x03, 0x48, 0x39, 0xFE, 0xEB, 0x20},
         data + 0x10},
        {"store faults",
         {0x48, 0x01, 0xD1, 0x48, 0x89, 0x03, 0x48, 0x39, 0xFE, 0xEB, 0x20},
         data + 0x2010},
        // sub rsi,rdi ; mov rax,[rbx] ; jne +0x30 ; inc rcx ; jmp +0x40
        {"load, jcc legs",
         {0x48, 0x29, 0xFE, 0x48, 0x8B, 0x03, 0x75, 0x30, 0x48, 0xFF, 0xC1,
          0xEB, 0x40},
         data + 0x18},
        // mov rsp,rbx ; add rcx,rdx ; ret
        {"ret", {0x48, 0x89, 0xDC, 0x48, 0x01, 0xD1, 0xC3}, data + 0x20},
    };
    ASSERT_TRUE(m_memory->write_u64(data + 0x20, 0x0000000140001234ull));
    const u32 shared_entry = arm64::ldr_x_uoff(
        16, REG_JIT_CTX,
        (static_cast<u32>(offsetof(JitContext, dispatch_tail)) >> 3) +
            4 /* kEntryCheckMap */);
    GuestAddr code_at = kCode64 + 0x1000;
    for (int mode = 0; mode < 2; ++mode) {
        JitCompiler::set_shared_exit_stubs_for_test(mode);
        m_jit = std::make_unique<JitCompiler>(*m_memory);
        for (const Form& f : forms) {
            SCOPED_TRACE(::testing::Message() << f.name << " shared=" << mode);
            code_at += 0x80;
            ASSERT_TRUE(m_memory->write_bytes(code_at, f.code.data(), f.code.size()));
            BasicBlock block = build64(f.code);
            block.start_addr = code_at;
            CompiledBlock* compiled = m_jit->compile(block);
            ASSERT_NE(compiled, nullptr);
            const auto* words = reinterpret_cast<const u32*>(compiled->code);
            EXPECT_EQ(words[1] == shared_entry, mode == 1)
                << "dispatcher entry is not the shared prologue";

            CpuState seed = cpu64(f.rbx);
            seed.rip = code_at;
            for (u8 g = 0; g < 16; ++g) {
                if (g == ESP || g == EBX) continue;
                seed.gpr.set64(g, 0x8F7E6D5C4B3A2918ull + 0x0102030405060708ull * g);
            }
            seed.flags.rflags = 0x202u;
            CpuState jcpu = seed;
            EXPECT_EQ(test::execute_like_dispatcher(*m_jit, *compiled, jcpu),
                      JitExecResult::Continue);
            CpuState icpu = seed;
            Interpreter interp(icpu, *m_memory);
            // Step only while RIP is inside the block: a taken Jcc leg leaves.
            bool ifault = false;
            for (u32 i = 0; i < block.instruction_count && !ifault &&
                            icpu.rip >= code_at &&
                            icpu.rip < code_at + f.code.size();
                 ++i) {
                ifault = interp.run(1) == ExecResult::Exception;
            }
            ASSERT_EQ(jcpu.pending_raise.active, ifault);
            if (ifault) EXPECT_EQ(jcpu.pending_raise.code, 0xC0000005u);
            icpu.materialize_lazy_flags();
            jcpu.materialize_lazy_flags();
            EXPECT_EQ(jcpu.rip, icpu.rip);
            EXPECT_EQ(jcpu.flags.rflags & 0x8D5u, icpu.flags.rflags & 0x8D5u);
            for (u8 g = 0; g < 16; ++g) {
                EXPECT_EQ(jcpu.gpr.get64(g), icpu.gpr.get64(g)) << "r" << int(g);
            }
        }
    }
}

TEST_F(JitLinear64Test, SharedColdHelperFaultsArePreciseAgainstInterpreter) {
    struct Form {
        std::vector<u8> code;
        int width;
        bool store;
    };
    const Form forms[] = {
        {{0x8A, 0x03}, 1, false},              // mov al,[rbx]
        {{0x66, 0x8B, 0x03}, 2, false},        // mov ax,[rbx]
        {{0x8B, 0x03}, 4, false},              // mov eax,[rbx]
        {{0x48, 0x8B, 0x03}, 8, false},        // mov rax,[rbx]
        {{0x48, 0x0F, 0xBE, 0x03}, 1, false},  // movsx rax,byte [rbx]
        {{0x0F, 0xB7, 0x03}, 2, false},        // movzx eax,word [rbx]
        {{0x48, 0x63, 0x03}, 4, false},        // movsxd rax,dword [rbx]
        {{0x88, 0x03}, 1, true},               // mov [rbx],al
        {{0x66, 0x89, 0x03}, 2, true},         // mov [rbx],ax
        {{0x89, 0x03}, 4, true},               // mov [rbx],eax
        {{0x48, 0x89, 0x03}, 8, true},         // mov [rbx],rax
    };
    const GuestAddr data = kData64;
    std::vector<u8> pattern(0x4000);
    for (usize i = 0; i < pattern.size(); ++i) {
        pattern[i] = static_cast<u8>(0x80u ^ (i * 37u) ^ (i >> 7));
    }
    ASSERT_TRUE(m_memory->write_bytes(data, pattern.data(), pattern.size()));
    ASSERT_TRUE(m_memory->protect_region(data + 0x2000, 0x1000,
                                         MemoryProtection::None));
    ASSERT_TRUE(m_memory->protect_region(data + 0x3000, 0x1000,
                                         MemoryProtection::Read));
    ASSERT_NE(m_memory->linear_check_page_map()[data >> kLinearHostPageShift] &
                  kLinearCheckMixedRead,
              0u);
    const auto reset = [&] {
        ASSERT_TRUE(m_memory->write_bytes(data, pattern.data(), 0x2000));
    };
    const auto snapshot = [&] {
        std::vector<u8> out(0x2000);
        EXPECT_TRUE(m_memory->read_bytes(data, out.data(), out.size()));
        return out;
    };
    GuestAddr code_at = kCode64 + 0x2000;
    const auto compile_at = [&](const std::vector<u8>& code) {
        code_at += 0x40;
        EXPECT_TRUE(m_memory->write_bytes(code_at, code.data(), code.size()));
        BasicBlock block = build64(code);
        block.start_addr = code_at;
        return block;
    };

    for (const Form& f : forms) {
        std::vector<u8> code = {0x48, 0x01, 0xD1};  // add rcx,rdx
        code.insert(code.end(), f.code.begin(), f.code.end());
        code.insert(code.end(), {0x48, 0xFF, 0xC6});  // inc rsi
        const BasicBlock block = compile_at(code);
        ASSERT_EQ(block.instruction_count, 3u);
        CompiledBlock* compiled = m_jit->compile(block);
        ASSERT_NE(compiled, nullptr);

        const int w = f.width;
        const GuestAddr half = static_cast<GuestAddr>(w > 1 ? w / 2 : 0);
        const GuestAddr cases[] = {
            data + 0x0010,         // allowed
            data + 0x1000 - half,  // unaligned across granule and guest page
            data + 0x3008,         // read-only page
            data + 0x2010,         // denied page
            data + 0x2000 - half,  // straddles into the denied page
            data + 0x4000 - half,  // read-only page into unmapped memory
        };
        for (const GuestAddr at : cases) {
            if (w == 1 && (at == data + 0x2000 || at == data + 0x4000)) continue;
            for (int pass = 0; pass < 2; ++pass) {
                SCOPED_TRACE(::testing::Message()
                             << "width=" << w << " store=" << f.store
                             << " form=" << (&f - forms) << " at=0x"
                             << std::hex << at << " pass=" << pass);
                CpuState seed = cpu64(at);
                seed.rip = code_at;
                for (u8 g = 0; g < 16; ++g) {
                    if (g == ESP || g == EBX) continue;
                    seed.gpr.set64(g, 0x8F7E6D5C4B3A2918ull +
                                          0x0102030405060708ull * g);
                }
                seed.flags.rflags = 0x202u;

                reset();
                CpuState jcpu = seed;
                EXPECT_EQ(test::execute_like_dispatcher(*m_jit, *compiled, jcpu),
                          JitExecResult::Continue);
                const std::vector<u8> jmem = snapshot();

                reset();
                CpuState icpu = seed;
                Interpreter interp(icpu, *m_memory);
                const bool ifault =
                    interp.run(block.instruction_count) == ExecResult::Exception;
                const std::vector<u8> imem = snapshot();

                ASSERT_EQ(jcpu.pending_raise.active, ifault);
                if (ifault) {
                    EXPECT_EQ(jcpu.pending_raise.code, 0xC0000005u);
                    ASSERT_EQ(jcpu.pending_raise.num_args, 2u);
                    EXPECT_EQ(jcpu.pending_raise.args[0],
                              interp.context().exception_error);
                    EXPECT_EQ(jcpu.pending_raise.args[1],
                              interp.context().exception_addr);
                }
                icpu.materialize_lazy_flags();
                jcpu.materialize_lazy_flags();
                EXPECT_EQ(jcpu.rip, icpu.rip);
                EXPECT_EQ(jcpu.flags.rflags & 0x8D5u, icpu.flags.rflags & 0x8D5u);
                for (u8 g = 0; g < 16; ++g) {
                    EXPECT_EQ(jcpu.gpr.get64(g), icpu.gpr.get64(g)) << "r" << int(g);
                }
                EXPECT_TRUE(jmem == imem) << "guest memory differs";
            }
        }
    }

    // White box: a hardware fault INSIDE the shared helper's direct access.
    // Make the host page read-only (guest pages 0, 1, 3 read-only, 2
    // NOACCESS) and grant guest page 3 for writing in the sub-page map alone:
    // the helper then stores to the read-only host page itself, and the fault
    // handler must attribute the fault to the calling site through the
    // helper's return address, as a precise write AV.
    if (const char* knob = std::getenv("VITREO_JIT_SUBPAGE_STUB");
        knob != nullptr && knob[0] == '0') {
        return;
    }
    ASSERT_TRUE(m_memory->protect_region(data, 0x2000, MemoryProtection::Read));
    u8* sub = const_cast<u8*>(m_memory->linear_subpage_map());
    const usize page = data >> kLinearHostPageShift;
    const u8 saved = sub[page];
    sub[page] = static_cast<u8>(saved | 0x80u);  // write bit of guest page 3
    // add rcx,rdx ; mov [rbx],rax ; add rsi,rdi. The last ADD overwrites
    // every flag, so the first ADD's flag record may be elided and rebuilt
    // only from the block's recovery descriptor -- which the handler finds
    // only by attributing the helper's fault to this block.
    const BasicBlock block = compile_at(
        {0x48, 0x01, 0xD1, 0x48, 0x89, 0x03, 0x48, 0x01, 0xFE});
    CompiledBlock* compiled = m_jit->compile(block);
    ASSERT_NE(compiled, nullptr);
    CpuState cpu = cpu64(data + 0x3010);
    cpu.rip = code_at;
    cpu.gpr.rcx = 0xFFFFFFFFu;
    cpu.gpr.rdx = 1;
    cpu.gpr.set64(EAX, 0x1111111122222222ull);
    cpu.flags.rflags = 0x202u;
    EXPECT_EQ(test::execute_like_dispatcher(*m_jit, *compiled, cpu),
              JitExecResult::Continue);
    sub[page] = saved;
    ASSERT_TRUE(cpu.pending_raise.active);
    EXPECT_EQ(cpu.pending_raise.code, 0xC0000005u);
    EXPECT_EQ(cpu.pending_raise.args[0], 1u);
    EXPECT_EQ(cpu.pending_raise.args[1], data + 0x3010);
    EXPECT_EQ(cpu.rip, code_at + 3u);
    cpu.materialize_lazy_flags();
    // SDM ADD: 0xFFFFFFFF + 1 = 0x100000000 in 64 bits; AF (carry out of
    // bit 3) and PF (low byte 0x00) set, CF/ZF/SF/OF clear.
    EXPECT_EQ(cpu.gpr.rcx, 0x100000000ull);
    EXPECT_EQ(cpu.flags.rflags & 0x8D5u, 0x014u);
    std::array<u8, 4> stored{};
    ASSERT_TRUE(m_memory->read_bytes(data + 0x3010, stored.data(), stored.size()));
    EXPECT_TRUE(std::equal(stored.begin(), stored.end(), pattern.begin() + 0x3010));
}

// The white-box case above, run twice in a fresh block. On the first run the
// helper's ordered store first takes the ordered-fault retry, whose
// trampoline returns to the access with X30 = the retry PC, inside the helper
// page; the shared helper keeps its caller's return address in X3 across the
// access for exactly that reason. The real fault that follows must still be
// attributed to the calling block through X3, or the elided ADD record is
// lost (flags 0). The second run, already patched, enters with X30 intact.
TEST_F(JitLinear64Test, SharedHelperFaultAfterAnOrderedRetryIsAttributedToTheCaller) {
    if (const char* knob = std::getenv("VITREO_JIT_SUBPAGE_STUB");
        knob != nullptr && knob[0] == '0') {
        GTEST_SKIP();
    }
    const GuestAddr data = kData64;
    std::vector<u8> pattern(0x4000);
    for (usize i = 0; i < pattern.size(); ++i) {
        pattern[i] = static_cast<u8>(0x80u ^ (i * 37u) ^ (i >> 7));
    }
    ASSERT_TRUE(m_memory->write_bytes(data, pattern.data(), pattern.size()));
    ASSERT_TRUE(m_memory->protect_region(data + 0x2000, 0x1000, MemoryProtection::None));
    ASSERT_TRUE(m_memory->protect_region(data, 0x2000, MemoryProtection::Read));
    ASSERT_TRUE(m_memory->protect_region(data + 0x3000, 0x1000, MemoryProtection::Read));
    u8* sub = const_cast<u8*>(m_memory->linear_subpage_map());
    const usize page = data >> kLinearHostPageShift;
    const u8 saved = sub[page];
    sub[page] = static_cast<u8>(saved | 0x80u);  // write bit of guest page 3
    const GuestAddr code_at = kCode64 + 0x3800;
    const std::vector<u8> code = {0x48, 0x01, 0xD1, 0x48, 0x89, 0x03, 0x48, 0x01, 0xFE};
    ASSERT_TRUE(m_memory->write_bytes(code_at, code.data(), code.size()));
    BasicBlock block = build64(code);
    block.start_addr = code_at;
    CompiledBlock* compiled = m_jit->compile(block);
    ASSERT_NE(compiled, nullptr);
    for (int pass = 0; pass < 2; ++pass) {
        SCOPED_TRACE(pass == 0 ? "first run (ordered retry)" : "second run");
        CpuState cpu = cpu64(data + 0x3010);
        cpu.rip = code_at;
        cpu.gpr.rcx = 0xFFFFFFFFu;
        cpu.gpr.rdx = 1;
        cpu.gpr.set64(EAX, 0x1111111122222222ull);
        cpu.flags.rflags = 0x202u;
        EXPECT_EQ(test::execute_like_dispatcher(*m_jit, *compiled, cpu),
                  JitExecResult::Continue);
        ASSERT_TRUE(cpu.pending_raise.active);
        EXPECT_EQ(cpu.pending_raise.code, 0xC0000005u);
        EXPECT_EQ(cpu.pending_raise.args[0], 1u);
        EXPECT_EQ(cpu.pending_raise.args[1], data + 0x3010);
        EXPECT_EQ(cpu.rip, code_at + 3u);
        cpu.materialize_lazy_flags();
        EXPECT_EQ(cpu.gpr.rcx, 0x100000000ull);
        EXPECT_EQ(cpu.flags.rflags & 0x8D5u, 0x014u) << "the elided ADD record";
    }
    sub[page] = saved;
}

}  // namespace
