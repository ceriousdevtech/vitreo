// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
//
// 32-bit linear load/store values, faults and flags. Split from
// test_jit_linear.cpp; shared fixtures and matchers live in
// jit_linear_fixture.hpp.

#include "jit_linear_fixture.hpp"

using namespace vitreo;
using namespace vitreo::cpu;
using namespace vitreo::cpu::jit;
using namespace vitreo::cpu::jit::test;

namespace {

// MOV EAX, [EBX]  (8B 03) — 32-bit load through the linear path.
TEST_F(JitLinearTest, Load32ValueMatchesInterpreter) {
    const u32 marker = 0xDEADBEEF;
    ASSERT_TRUE(m_memory->write_bytes(kData + 0x100,
                                      reinterpret_cast<const u8*>(&marker), 4));
    std::vector<u8> code = {0x8B, 0x03};  // MOV EAX, [EBX]
    ASSERT_TRUE(m_memory->write_bytes(kCode, code.data(), code.size()));
    BasicBlock block = build_block(code);
    ASSERT_EQ(block.instruction_count, 1u);

    CpuState cpu;
    init_cpu(cpu);
    cpu.gpr.set32(EBX, static_cast<u32>(kData + 0x100));
    CompiledBlock* cb = m_jit->compile(block);
    ASSERT_NE(cb, nullptr);
    (void)test::execute_like_dispatcher(*m_jit, *cb, cpu);

    EXPECT_FALSE(cpu.pending_raise.active);
    EXPECT_EQ(cpu.gpr.get32(EAX), marker);
}

// MOV [EBX], EAX  (89 03) — 32-bit store through the linear path.
TEST_F(JitLinearTest, Store32WritesCorrectBytes) {
    std::vector<u8> code = {0x89, 0x03};  // MOV [EBX], EAX
    ASSERT_TRUE(m_memory->write_bytes(kCode, code.data(), code.size()));
    BasicBlock block = build_block(code);

    CpuState cpu;
    init_cpu(cpu);
    cpu.gpr.set32(EBX, static_cast<u32>(kData + 0x200));
    cpu.gpr.set32(EAX, 0xCAFEBABE);
    CompiledBlock* cb = m_jit->compile(block);
    ASSERT_NE(cb, nullptr);
    (void)test::execute_like_dispatcher(*m_jit, *cb, cpu);

    EXPECT_FALSE(cpu.pending_raise.active);
    u32 got = 0;
    ASSERT_TRUE(m_memory->read_bytes(kData + 0x200,
                                     reinterpret_cast<u8*>(&got), 4));
    EXPECT_EQ(got, 0xCAFEBABEu);
}

TEST_F(JitLinearTest, MsvcSehProloguePreservesStackAddressAcrossPushImmediate) {
    // Battle.net uses this exact MSVC SEH setup at the start of a static
    // initializer. Keep the block intact through its first conditional branch.
    std::vector<u8> code = {
        0x55,
        0x8B, 0xEC,
        0x64, 0xA1, 0x00, 0x00, 0x00, 0x00,
        0x6A, 0xFF,
        0x68, 0x78, 0x56, 0x34, 0x12,
        0x50,
        0x64, 0x89, 0x25, 0x00, 0x00, 0x00, 0x00,
        0x83, 0xEC, 0x10,
        0x80, 0x3D,
    };

    constexpr u32 stack_top = static_cast<u32>(kData + 0x3000);
    constexpr u32 fs_base = static_cast<u32>(kData + 0x1000);
    constexpr u32 init_flag = static_cast<u32>(kData + 0x2000);
    constexpr u32 seh_head = 0x12345678;
    for (u32 shift = 0; shift < 32; shift += 8) {
        code.push_back(static_cast<u8>(init_flag >> shift));
    }
    code.insert(code.end(), {
        0x00,
        0x0F, 0x85, 0x00, 0x00, 0x00, 0x00,
    });
    ASSERT_TRUE(m_memory->write_bytes(kCode, code.data(), code.size()));
    BasicBlock block = build_block(code);
    ASSERT_EQ(block.instruction_count, 10u);

    ASSERT_TRUE(m_memory->write_u32(fs_base, seh_head).is_ok());
    ASSERT_TRUE(m_memory->write_u8(init_flag, 0).is_ok());

    CpuState cpu;
    init_cpu(cpu);
    cpu.gpr.set32(ESP, stack_top);
    cpu.gpr.set32(EBP, 0xCAFEBABE);
    cpu.seg.fs_desc.base = fs_base;

    CompiledBlock* cb = m_jit->compile(block);
    ASSERT_NE(cb, nullptr);
    EXPECT_EQ(test::execute_like_dispatcher(*m_jit, *cb, cpu), JitExecResult::Continue);

    EXPECT_FALSE(cpu.pending_raise.active);
    EXPECT_EQ(cpu.gpr.get32(EAX), seh_head);
    EXPECT_EQ(cpu.gpr.get32(EBP), stack_top - 4);
    EXPECT_EQ(cpu.gpr.get32(ESP), stack_top - 32);
    EXPECT_EQ(m_memory->read_u32(stack_top - 4).value_or(0), 0xCAFEBABEu);
    EXPECT_EQ(m_memory->read_u32(stack_top - 8).value_or(0), 0xFFFFFFFFu);
    EXPECT_EQ(m_memory->read_u32(stack_top - 12).value_or(0), 0x12345678u);
    EXPECT_EQ(m_memory->read_u32(stack_top - 16).value_or(0), seh_head);
    EXPECT_EQ(m_memory->read_u32(fs_base).value_or(0), stack_top - 16);
}

// A load from an unmapped guest address faults in hardware and is recovered as
// a guest access violation with the precise faulting guest address.
TEST_F(JitLinearTest, Load32FromUnmappedRaisesAccessViolation) {
    std::vector<u8> code = {0x8B, 0x03};  // MOV EAX, [EBX]
    ASSERT_TRUE(m_memory->write_bytes(kCode, code.data(), code.size()));
    BasicBlock block = build_block(code);

    CpuState cpu;
    init_cpu(cpu);
    cpu.gpr.set32(EBX, static_cast<u32>(kUnmapped));
    CompiledBlock* cb = m_jit->compile(block);
    ASSERT_NE(cb, nullptr);
    (void)test::execute_like_dispatcher(*m_jit, *cb, cpu);

    ASSERT_TRUE(cpu.pending_raise.active) << "bad linear access did not raise";
    EXPECT_EQ(cpu.pending_raise.code, 0xC0000005u);
    EXPECT_EQ(cpu.pending_raise.args[1], static_cast<u64>(kUnmapped))
        << "recovered fault address is wrong";
    EXPECT_EQ(cpu.pending_raise.args[0], 0u) << "a load should be a read (0)";
}

// A store to an unmapped guest address likewise recovers, flagged as a write.
TEST_F(JitLinearTest, Store32ToUnmappedRaisesWriteAccessViolation) {
    std::vector<u8> code = {0x89, 0x03};  // MOV [EBX], EAX
    ASSERT_TRUE(m_memory->write_bytes(kCode, code.data(), code.size()));
    BasicBlock block = build_block(code);

    CpuState cpu;
    init_cpu(cpu);
    cpu.gpr.set32(EBX, static_cast<u32>(kUnmapped + 0x40));
    cpu.gpr.set32(EAX, 0x12345678);
    CompiledBlock* cb = m_jit->compile(block);
    ASSERT_NE(cb, nullptr);
    (void)test::execute_like_dispatcher(*m_jit, *cb, cpu);

    ASSERT_TRUE(cpu.pending_raise.active);
    EXPECT_EQ(cpu.pending_raise.code, 0xC0000005u);
    EXPECT_EQ(cpu.pending_raise.args[1], static_cast<u64>(kUnmapped + 0x40));
    EXPECT_EQ(cpu.pending_raise.args[0], 1u) << "a store should be a write (1)";
}

// After a recovered fault, the engine keeps working: a subsequent valid access
// on a fresh CpuState behaves normally (recovery did not corrupt global state).
TEST_F(JitLinearTest, RecoveryDoesNotBreakSubsequentExecution) {
    {
        std::vector<u8> bad = {0x8B, 0x03};
        ASSERT_TRUE(m_memory->write_bytes(kCode, bad.data(), bad.size()));
        BasicBlock b = build_block(bad);
        CpuState cpu; init_cpu(cpu); cpu.gpr.set32(EBX, static_cast<u32>(kUnmapped));
        CompiledBlock* cb = m_jit->compile(b);
        (void)test::execute_like_dispatcher(*m_jit, *cb, cpu);
        ASSERT_TRUE(cpu.pending_raise.active);
    }
    const u32 marker = 0x5A5A5A5A;
    ASSERT_TRUE(m_memory->write_bytes(kData + 0x300,
                                      reinterpret_cast<const u8*>(&marker), 4));
    std::vector<u8> good = {0x8B, 0x03};
    ASSERT_TRUE(m_memory->write_bytes(kCode, good.data(), good.size()));
    BasicBlock b = build_block(good);
    CpuState cpu; init_cpu(cpu); cpu.gpr.set32(EBX, static_cast<u32>(kData + 0x300));
    CompiledBlock* cb = m_jit->compile(b);
    (void)test::execute_like_dispatcher(*m_jit, *cb, cpu);
    EXPECT_FALSE(cpu.pending_raise.active);
    EXPECT_EQ(cpu.gpr.get32(EAX), marker);
}

// Intel SDM Vol. 3A 6.6, as JitLinear64Test.ElidedFlagRecordIsRebuiltAtA
// PreciseFault: the faulting load's state carries the flags of the ADD/CMP
// before it, whose record the JIT elided and rebuilds from its operand
// registers. Two shapes break that rebuild: after `add eax, eax` the one
// register holds the result, not the addend; and an XCHG between the
// publisher and the fault rewrites the publisher's operand through its SECOND
// operand. Either record must be kept.
TEST_F(JitLinearTest, FaultAfterElidedRecordKeepsThePublishersFlags) {
    struct Form { const char* name; std::vector<u8> bytes; u32 fault_index; };
    const Form forms[] = {
        {"add eax,eax; mov edx,[ebx]; add esi,1",
         {0x01, 0xC0, 0x8B, 0x13, 0x83, 0xC6, 0x01}, 1},
        {"cmp eax,ecx; xchg edi,ecx; mov edx,[ebx]; add esi,1",
         {0x39, 0xC8, 0x87, 0xCF, 0x8B, 0x13, 0x83, 0xC6, 0x01}, 2},
    };
    for (const Form& form : forms) {
        SCOPED_TRACE(form.name);
        ASSERT_TRUE(m_memory->write_bytes(kCode, form.bytes.data(),
                                          form.bytes.size()));
        BasicBlock block = build_block(form.bytes);
        ASSERT_EQ(block.instruction_count, form.fault_index + 2);
        CpuState seed;
        init_cpu(seed);
        seed.gpr.set32(EAX, 0x80000001u);
        seed.gpr.set32(1, 0x7FFFFFFFu);  // ECX
        seed.gpr.set32(7, 0x00000003u);  // EDI
        seed.gpr.set32(EBX, static_cast<u32>(kUnmapped));
        seed.flags.rflags = 0x2u;
        m_jit = std::make_unique<JitCompiler>(*m_memory);
        m_jit->set_block_cache(m_cache.get());
        CompiledBlock* cb = m_jit->compile(block);
        ASSERT_NE(cb, nullptr);
        CpuState jcpu = seed;
        (void)test::execute_like_dispatcher(*m_jit, *cb, jcpu);
        CpuState icpu = seed;
        { Interpreter interp(icpu, *m_memory); (void)interp.run(form.fault_index); }
        ASSERT_TRUE(jcpu.pending_raise.active);
        GuestAddr fault_rip = kCode;
        for (u32 i = 0; i < form.fault_index; ++i) {
            fault_rip += block.instructions[i].length;
        }
        EXPECT_EQ(static_cast<u32>(jcpu.rip), static_cast<u32>(fault_rip));
        icpu.materialize_lazy_flags();
        jcpu.materialize_lazy_flags();
        EXPECT_EQ(jcpu.flags.rflags & 0x8D5u, icpu.flags.rflags & 0x8D5u);
        for (u8 r = 0; r < 8; ++r) {
            EXPECT_EQ(jcpu.gpr.get32(r), icpu.gpr.get32(r)) << "r" << int(r);
        }
    }
}

// 32-bit counterpart of JitLinear64Test.SetccAndCmovBetweenPublisherAndKiller
// MatchInterpreter: SETcc / CMOVcc read NZCV while the record is elided.
TEST_F(JitLinearTest, SetccAndCmovBetweenPublisherAndKillerMatchInterpreter) {
    const std::vector<std::vector<u8>> forms = {
        {0x39, 0xC8, 0x0F, 0x94, 0xC0, 0x0F, 0x9F, 0xC2, 0x83, 0xC6, 0x01},
        {0x39, 0xC8, 0x0F, 0x4C, 0xC1, 0x0F, 0x4F, 0xD1, 0x83, 0xC6, 0x01},
        {0x85, 0xC0, 0x0F, 0x98, 0xC0, 0x0F, 0x94, 0xC1, 0x83, 0xC6, 0x01},
        {0x01, 0xC8, 0x0F, 0x90, 0xC0, 0x0F, 0x92, 0xC2, 0x83, 0xC6, 0x01},
        {0x39, 0xC8, 0x0F, 0x94, 0xC4, 0x0F, 0x9C, 0xC7, 0x83, 0xC6, 0x01},
        {0x39, 0xC8, 0x0F, 0x9A, 0xC0, 0x83, 0xC6, 0x01},
        {0x85, 0xC8, 0x0F, 0x92, 0xC0, 0x0F, 0x46, 0xD1, 0x83, 0xC6, 0x01},
    };
    const std::pair<u32, u32> operands[] = {
        {5, 5}, {3, 7}, {7, 3}, {0x80000000u, 0x7FFFFFFFu}, {0xFFFFFFFFu, 1}, {0, 0},
    };
    for (const auto& code : forms) {
        for (const auto& [eax, ecx] : operands) {
            SCOPED_TRACE(::testing::Message() << "bytes=" << code.size()
                         << " eax=0x" << std::hex << eax << " ecx=0x" << ecx);
            ASSERT_TRUE(m_memory->write_bytes(kCode, code.data(), code.size()));
            BasicBlock block = build_block(code);
            m_jit = std::make_unique<JitCompiler>(*m_memory);
            m_jit->set_block_cache(m_cache.get());
            CompiledBlock* cb = m_jit->compile(block);
            ASSERT_NE(cb, nullptr);
            CpuState seed;
            init_cpu(seed);
            seed.gpr.set32(EAX, eax);
            seed.gpr.set32(1, ecx);
            seed.gpr.set32(2, 0xDDDDDDDDu);
            seed.gpr.set32(6, 0x66666666u);
            seed.gpr.set32(7, 0x77777777u);
            seed.flags.rflags = 0x8D7u;
            CpuState jcpu = seed;
            (void)test::execute_like_dispatcher(*m_jit, *cb, jcpu);
            CpuState icpu = seed;
            { Interpreter interp(icpu, *m_memory); (void)interp.run(block.instruction_count); }
            icpu.materialize_lazy_flags();
            jcpu.materialize_lazy_flags();
            EXPECT_FALSE(jcpu.pending_raise.active);
            EXPECT_EQ(static_cast<u32>(jcpu.rip), static_cast<u32>(icpu.rip));
            EXPECT_EQ(jcpu.flags.rflags & 0x8D5u, icpu.flags.rflags & 0x8D5u);
            for (u8 r = 0; r < 8; ++r) {
                EXPECT_EQ(jcpu.gpr.get32(r), icpu.gpr.get32(r)) << "r" << int(r);
            }
        }
    }
}

// 8-bit and 16-bit linear memory ops (extended from load32/store32 for the
// decompression hot path). Value correctness against a seeded memory image.
TEST_F(JitLinearTest, Load8Load16Store8Store16) {
    // Seed a known dword.
    const u32 seed = 0x11223344;
    ASSERT_TRUE(m_memory->write_bytes(kData + 0x40,
                                      reinterpret_cast<const u8*>(&seed), 4));
    // MOVZX EAX, byte [EBX]  (0F B6 03) — load8 zero-extended.
    {
        std::vector<u8> code = {0x0F, 0xB6, 0x03};
        ASSERT_TRUE(m_memory->write_bytes(kCode, code.data(), code.size()));
        BasicBlock b = build_block(code);
        CpuState c; init_cpu(c); c.gpr.set32(EBX, static_cast<u32>(kData + 0x40));
        (void)test::execute_like_dispatcher(*m_jit, *m_jit->compile(b), c);
        EXPECT_FALSE(c.pending_raise.active);
        EXPECT_EQ(c.gpr.get32(EAX), 0x44u) << "linear load8";
    }
    // MOVZX EAX, word [EBX]  (0F B7 03) — load16 zero-extended.
    {
        std::vector<u8> code = {0x0F, 0xB7, 0x03};
        ASSERT_TRUE(m_memory->write_bytes(kCode, code.data(), code.size()));
        BasicBlock b = build_block(code);
        CpuState c; init_cpu(c); c.gpr.set32(EBX, static_cast<u32>(kData + 0x40));
        (void)test::execute_like_dispatcher(*m_jit, *m_jit->compile(b), c);
        EXPECT_FALSE(c.pending_raise.active);
        EXPECT_EQ(c.gpr.get32(EAX), 0x3344u) << "linear load16";
    }
    // MOV [EBX], AL  (88 03) — store8, must not disturb neighbours.
    {
        std::vector<u8> code = {0x88, 0x03};
        ASSERT_TRUE(m_memory->write_bytes(kCode, code.data(), code.size()));
        BasicBlock b = build_block(code);
        CpuState c; init_cpu(c); c.gpr.set32(EBX, static_cast<u32>(kData + 0x41));
        c.gpr.set32(EAX, 0xAA);
        (void)test::execute_like_dispatcher(*m_jit, *m_jit->compile(b), c);
        u32 got = 0; ASSERT_TRUE(m_memory->read_bytes(kData + 0x40,
                                    reinterpret_cast<u8*>(&got), 4));
        EXPECT_EQ(got, 0x1122AA44u) << "linear store8 (only byte 0x41)";
    }
    // MOV [EBX], AX  (66 89 03) — store16.
    {
        std::vector<u8> code = {0x66, 0x89, 0x03};
        ASSERT_TRUE(m_memory->write_bytes(kCode, code.data(), code.size()));
        BasicBlock b = build_block(code);
        CpuState c; init_cpu(c); c.gpr.set32(EBX, static_cast<u32>(kData + 0x40));
        c.gpr.set32(EAX, 0xBEEF);
        (void)test::execute_like_dispatcher(*m_jit, *m_jit->compile(b), c);
        u32 got = 0; ASSERT_TRUE(m_memory->read_bytes(kData + 0x40,
                                    reinterpret_cast<u8*>(&got), 4));
        EXPECT_EQ(got, 0x1122BEEFu) << "linear store16";
    }
}

TEST_F(JitLinearTest, CmpThenMovapsStoreThenJaeMatchesInterpreter) {
    // Linear MOVAPS [mem] uses SUBS for the same-page check. That must not
    // become the NZCV that `JAE` fuses (`CMP end, cap; MOVAPS [esp]; JAE`).
    const std::vector<u8> code = {
        0x39, 0xD8,                    // cmp eax, ebx
        0x0F, 0x29, 0x44, 0x24, 0x10,  // movaps [esp+0x10], xmm0
        0x73, 0x02,                    // jae +2
    };
    ASSERT_TRUE(m_memory->write_bytes(kCode, code.data(), code.size()));
    BasicBlock block = build_block(code);
    CompiledBlock* cb = m_jit->compile(block);
    ASSERT_NE(cb, nullptr);

    const std::pair<u32, u32> pairs[] = {
        {5, 3}, {3, 5}, {5, 5}, {0x80000000u, 1}, {1, 0x80000000u},
    };
    const u32 stack = static_cast<u32>(kData + 0x200);
    for (auto [eax, ebx] : pairs) {
        CpuState reference;
        init_cpu(reference);
        reference.gpr.set32(ESP, stack);
        reference.gpr.set32(EAX, eax);
        reference.gpr.set32(EBX, ebx);
        Interpreter interpreter(reference, *m_memory);
        (void)interpreter.run(3);

        CpuState cpu;
        init_cpu(cpu);
        cpu.gpr.set32(ESP, stack);
        cpu.gpr.set32(EAX, eax);
        cpu.gpr.set32(EBX, ebx);
        EXPECT_EQ(test::execute_like_dispatcher(*m_jit, *cb, cpu), JitExecResult::Continue);
        EXPECT_FALSE(cpu.pending_raise.active);
        EXPECT_EQ(cpu.rip, reference.rip)
            << "eax=0x" << std::hex << eax << " ebx=0x" << ebx;
    }
}

TEST_F(JitLinearTest, CmpThenMulssThenJneMatchesInterpreter) {
    const std::vector<u8> code = {
        0x39, 0xD8,              // cmp eax, ebx
        0xF3, 0x0F, 0x59, 0xC1,  // mulss xmm0, xmm1
        0x75, 0x02,              // jne +2
    };
    ASSERT_TRUE(m_memory->write_bytes(kCode, code.data(), code.size()));
    BasicBlock block = build_block(code);
    CompiledBlock* cb = m_jit->compile(block);
    ASSERT_NE(cb, nullptr);

    const std::pair<u32, u32> pairs[] = {
        {5, 3}, {3, 5}, {5, 5}, {0x80000000u, 1}, {1, 0x80000000u},
    };
    for (auto [eax, ebx] : pairs) {
        CpuState reference;
        init_cpu(reference);
        reference.gpr.set32(EAX, eax);
        reference.gpr.set32(EBX, ebx);
        reference.sse.xmm[0].f32_[0] = 2.0f;
        reference.sse.xmm[1].f32_[0] = 3.0f;
        Interpreter interpreter(reference, *m_memory);
        (void)interpreter.run(3);

        CpuState cpu;
        init_cpu(cpu);
        cpu.gpr.set32(EAX, eax);
        cpu.gpr.set32(EBX, ebx);
        cpu.sse.xmm[0].f32_[0] = 2.0f;
        cpu.sse.xmm[1].f32_[0] = 3.0f;
        EXPECT_EQ(test::execute_like_dispatcher(*m_jit, *cb, cpu), JitExecResult::Continue);
        EXPECT_FALSE(cpu.pending_raise.active);
        EXPECT_EQ(cpu.rip, reference.rip)
            << "eax=0x" << std::hex << eax << " ebx=0x" << ebx;
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// Faulting 16- and 32-byte vector stores through the 32-bit linear model,
// where a denied page is a host fault: interpreter vs JIT and both against
// Windows (simd_store_fault_cases.hpp).
// ---------------------------------------------------------------------------
#include "simd_store_fault_cases.hpp"

namespace {

void run_linear32_store_faults(GuestMemory& mem,
                               const std::function<BasicBlock(const std::vector<u8>&)>& build,
                               const std::function<void(CpuState&)>& init,
                               std::initializer_list<GuestAddr> boundaries) {
    const vitreo::cpu::test_simd_store::Env env{
        mem, kCode, false, build, init,
        [&mem](const BasicBlock& block, CpuState& cpu) {
            JitCompiler jit(mem);
            CompiledBlock* cb = jit.compile(block);
            if (cb == nullptr) {
                ADD_FAILURE() << "store block did not compile natively";
                return JitExecResult::StaleCode;
            }
            return execute_like_dispatcher(jit, *cb, cpu);
        }};
    for (GuestAddr b : boundaries) {
        SCOPED_TRACE(::testing::Message() << "boundary 0x" << std::hex << b);
        vitreo::cpu::test_simd_store::run_all_forms(env, b);
    }
}

}  // namespace

// Production default (VITREO_JIT_LINEAR32_HWPROT off): only an unmapped host
// page faults in hardware, so the boundary is a 16 KiB edge before unmapped
// memory. Read-only pages are documented as unenforced inline in this model.
TEST_F(JitLinearTest, VectorStoreFaultsMatchInterpreterAndWindows) {
    ScopedEnvironmentFlag nd("VITREO_JIT_NO_DELEGATE", "1");
    run_linear32_store_faults(
        *m_memory,
        [this](const std::vector<u8>& code) { return build_block(code); },
        [this](CpuState& c) { init_cpu(c); }, {GuestAddr{kData + kDataSize}});
}

// With hardware protection, also a 16 KiB edge before read-only pages and a
// 4 KiB edge inside one host page.
TEST_F(JitLinearTest, VectorStoreFaultsMatchInterpreterAndWindowsWithHardwareProtection) {
    ScopedEnvironmentFlag nd("VITREO_JIT_NO_DELEGATE", "1");
    GuestMemory mem;
    ASSERT_TRUE(mem.enable_linear_reservation(kReservation));
    ASSERT_TRUE(mem.enable_linear_hardware_protection());
    LinearReservationRegistration registration;
    ASSERT_TRUE(registration.acquire(mem.linear_base(), kReservation));
    ASSERT_TRUE(mem.map_region(kCode, 0x4000, MemoryProtection::ReadWrite));
    ASSERT_TRUE(mem.map_region(kData, kDataSize, MemoryProtection::ReadWrite));
    ASSERT_TRUE(mem.map_region(0x500000, 0x4000, MemoryProtection::ReadWrite));
    ASSERT_TRUE(mem.map_region(0x504000, 0x4000, MemoryProtection::Read));
    ASSERT_TRUE(mem.map_region(0x600000, 0x1000, MemoryProtection::ReadWrite));
    run_linear32_store_faults(
        mem,
        [this](const std::vector<u8>& code) { return build_block(code); },
        [this](CpuState& c) { init_cpu(c); },
        {GuestAddr{0x504000}, GuestAddr{0x601000}, GuestAddr{kData + kDataSize}});
}
