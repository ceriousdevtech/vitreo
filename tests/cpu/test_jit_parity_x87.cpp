// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// JIT vs Interpreter parity tests (32-bit mode): x87 flag preservation and m32/m64 stores
//
// Fixture and shared helpers: jit_parity_fixture.hpp.

#include "jit_parity_fixture.hpp"

namespace {
// expect_branch_matches with an arbitrary instruction count and setup, so a
// sequence longer than "compare + Jcc" can be checked for branch parity.
void expect_branch_matches_seq(GuestMemory& mem,
                               const std::vector<u8>& code_il,
                               const std::function<void(CpuState&)>& init,
                               u32 expected_count) {
    JitCompiler jit(mem);
    const std::vector<u8>& code = code_il;
    // Sane control word for FLDCW, and non-trivial bits for the loads.
    const u32 cw = 0x027F;
    ASSERT_TRUE(mem.write_bytes(0x2020, reinterpret_cast<const u8*>(&cw), 4));
    ASSERT_TRUE(mem.write_bytes(0x1000, code.data(), code.size()));
    MemoryByteReader reader(code.data(), code.size());
    BasicBlock block; block.start_addr = 0x1000;
    while (reader.remaining() > 0) {
        DecodedInstruction inst;
        if (decode_instruction(reader, DecodeMode::Mode32, inst) !=
            DecodeError::Success) break;
        block.instructions.push_back(inst);
    }
    block.byte_length = static_cast<u32>(reader.position());
    block.instruction_count = static_cast<u32>(block.instructions.size());
    ASSERT_EQ(block.instruction_count, expected_count);

    auto setup = [&](CpuState& c) {
        c.init_protected_mode(); c.gpr.set32(ESP, 0x10000); c.rip = 0x1000;
        init(c);
    };
    CpuState icpu; setup(icpu);
    { Interpreter interp(icpu, mem); (void)interp.run(expected_count); }
    CpuState jcpu; setup(jcpu);
    ASSERT_TRUE(mem.protect_region(0x1000, 0x1000,
                                   MemoryProtection::ReadExecute).is_ok());
    CompiledBlock* cb = jit.compile(block);
    ASSERT_NE(cb, nullptr);
    (void)test::execute_like_dispatcher(jit, *cb, jcpu);
    ASSERT_TRUE(mem.protect_region(0x1000, 0x1000,
                                   MemoryProtection::ReadWrite).is_ok());
    EXPECT_EQ(static_cast<u32>(icpu.rip), static_cast<u32>(jcpu.rip))
        << "branch diverged: interp rip=0x" << std::hex
        << static_cast<u32>(icpu.rip)
        << " jit rip=0x" << static_cast<u32>(jcpu.rip);
}
}  // namespace

// The Jcc consumer of the same contract, and the shape that actually shipped:
// The Sims 3 emits `CMP EDI,4 ; FSTP dword [ESI+0x5C] ; JL` at TS3+0xc3dd59.
// A Jcc fuses off live NZCV, so an x87 store that writes NZCV without telling
// the compiler sends the branch the wrong way -- there, into an unrolled loop
// with a negative trip count that read floats as pointers.
TEST_F(JitParityTest, CmpThenX87StoreThenJccAllConditions) {
    for (u8 jcc = 0x70; jcc <= 0x7F; ++jcc) {
        for (u32 edi : {0u, 2u, 4u, 7u, 0x80000000u, 0xFFFFFFFFu}) {
            // FLD1 ; CMP EDI,4 ; FSTP dword [ESI+0x5C] ; Jcc +2
            expect_branch_matches_seq(
                *m_memory,
                {0xD9, 0xE8, 0x83, 0xFF, 0x04, 0xD9, 0x5E, 0x5C, jcc, 0x02},
                [edi](CpuState& c) {
                    c.gpr.set32(EDI, edi);
                    c.gpr.set32(ESI, 0x2000);
                },
                4);
        }
    }
}

// Same contract swept across the x87 lowerings, native and helper paths
// alike. Every one of these leaves EFLAGS alone (the FCOMI family, the only
// x87 instructions that write EFLAGS, is deliberately absent), so a Jcc after
// any of them must land where the interpreter lands.
TEST_F(JitParityTest, X87OpsPreserveFlagsForFollowingJcc) {
    const std::pair<const char*, std::vector<u8>> ops[] = {
        {"FLD m32",    {0xD9, 0x46, 0x20}},
        {"FLD m64",    {0xDD, 0x46, 0x20}},
        {"FLD m80",    {0xDB, 0x6E, 0x20}},
        {"FST m32",    {0xD9, 0x56, 0x5C}},
        {"FSTP m32",   {0xD9, 0x5E, 0x5C}},
        {"FST m64",    {0xDD, 0x56, 0x5C}},
        {"FSTP m64",   {0xDD, 0x5E, 0x5C}},
        {"FSTP m80",   {0xDB, 0x7E, 0x5C}},
        {"FADD m32",   {0xD8, 0x46, 0x20}},
        {"FADD m64",   {0xDC, 0x46, 0x20}},
        {"FMUL m32",   {0xD8, 0x4E, 0x20}},
        {"FSUB m32",   {0xD8, 0x66, 0x20}},
        {"FDIV m32",   {0xD8, 0x76, 0x20}},
        {"FADD st,st1",{0xD8, 0xC1}},
        {"FADDP",      {0xDE, 0xC1}},
        {"FMULP",      {0xDE, 0xC9}},
        {"FILD m32",   {0xDB, 0x46, 0x20}},
        {"FIST m32",   {0xDB, 0x56, 0x5C}},
        {"FISTP m32",  {0xDB, 0x5E, 0x5C}},
        {"FIADD m32",  {0xDA, 0x46, 0x20}},
        {"FCHS",       {0xD9, 0xE0}},
        {"FABS",       {0xD9, 0xE1}},
        {"FSQRT",      {0xD9, 0xFA}},
        {"FRNDINT",    {0xD9, 0xFC}},
        {"FXCH",       {0xD9, 0xC9}},
        {"FCOM m32",   {0xD8, 0x56, 0x20}},
        {"FUCOMPP",    {0xDA, 0xE9}},
        {"FNSTCW m16", {0xD9, 0x7E, 0x5C}},
        {"FLDCW m16",  {0xD9, 0x6E, 0x20}},
        {"FNSTSW AX",  {0xDF, 0xE0}},
        {"FFREE st1",  {0xDD, 0xC1}},
        {"FINCSTP",    {0xD9, 0xF7}},
    };
    for (const auto& [name, body] : ops) {
        SCOPED_TRACE(name);
        for (u8 jcc : {0x72, 0x74, 0x75, 0x7C, 0x7D, 0x7E, 0x7F}) {
            for (u32 edi : {0u, 2u, 4u, 7u}) {
                // FLD1 ; FLD1 ; CMP EDI,4 ; <x87 op> ; Jcc +2
                std::vector<u8> code{0xD9, 0xE8, 0xD9, 0xE8,
                                     0x83, 0xFF, 0x04};
                code.insert(code.end(), body.begin(), body.end());
                code.push_back(jcc);
                code.push_back(0x02);
                expect_branch_matches_seq(
                    *m_memory, code,
                    [edi](CpuState& c) {
                        c.gpr.set32(EDI, edi);
                        c.gpr.set32(ESI, 0x2000);
                    },
                    5);
            }
        }
    }
}

// x87 writes no EFLAGS (Intel SDM Vol. 1, 8.1.4 -- only the FCOMI family
// does). The JIT keeps pending lazy flags live in NZCV, so an x87 lowering
// that emits a flag-setting compare in its own guard destroys the flags of a
// preceding arithmetic instruction. `CMP; FLD; FSTP; SETcc` is real compiled
// code -- The Sims 3 uses it to decide whether the graphics device is usable.
TEST_F(JitParityTest, CmpThenX87StoreThenReadsFlags) {
    const double one = 1.0;
    m_memory->write_bytes(0x2000, reinterpret_cast<const u8*>(&one),
                          sizeof(one));
    // CMP EAX,EBX ; FLD qword [0x2000] ; FSTP dword [0x2010] ; SETE DL
    check_parity({0x39, 0xD8,
                  0xDD, 0x05, 0x00, 0x20, 0x00, 0x00,
                  0xD9, 0x1D, 0x10, 0x20, 0x00, 0x00,
                  0x0F, 0x94, 0xC2},
        [](CpuState& c) { c.gpr.set32(EAX, 7); c.gpr.set32(EBX, 7); });
}

// FCOM/FCOMP with a memory operand converts ST(0) to binary64 for a native
// FCMP and bails to the helper when the exponent will not fit -- subnormal,
// Inf, NaN, or an extended-precision magnitude. That bail-out passes the
// operand address in REG_CALL_TMP, so the address has to be computed BEFORE
// the conversion; computing it inside the per-opcode arm left the slow exit
// handing the helper a stale register and reading a wild address. Load an
// 80-bit value whose exponent is far outside binary64's range so the slow
// exit is the one taken, and read the comparison back through FNSTSW AX,
// which carries C0/C2/C3 and TOP -- so both the result and the pop are
// compared against the interpreter.
TEST_F(JitParityTest, FcompMemSlowPathUsesRealOperandAddress) {
    // ST(0) exponents chosen either side of binary64: 0x5000 is above its
    // maximum, 0x0001 far below its minimum.
    for (u16 exp_sign : {u16{0x5000}, u16{0x0001}, u16{0xD000}}) {
        SCOPED_TRACE(exp_sign);
        for (float rhs : {1.0f, -1.0f, 0.0f, 3.5e38f}) {
            const u64 significand = 0x8000000000000000ULL;
            ASSERT_TRUE(m_memory->write_bytes(
                0x2000, reinterpret_cast<const u8*>(&significand),
                sizeof(significand)));
            ASSERT_TRUE(m_memory->write_bytes(
                0x2008, reinterpret_cast<const u8*>(&exp_sign),
                sizeof(exp_sign)));
            ASSERT_TRUE(m_memory->write_bytes(
                0x2010, reinterpret_cast<const u8*>(&rhs), sizeof(rhs)));
            // FLD tbyte [0x2000] ; FCOMP dword [0x2010] ; FNSTSW AX
            check_parity({0xDB, 0x2D, 0x00, 0x20, 0x00, 0x00,
                          0xD8, 0x1D, 0x10, 0x20, 0x00, 0x00,
                          0xDF, 0xE0});
            // Same operand, non-popping form: FCOM leaves TOP alone.
            check_parity({0xDB, 0x2D, 0x00, 0x20, 0x00, 0x00,
                          0xD8, 0x15, 0x10, 0x20, 0x00, 0x00,
                          0xDF, 0xE0});
        }
    }
}

// The whole x87 memory-form family, swept under the condition that reaches
// every native lowering's slow exit: an ST(0) whose exponent is outside
// binary64's range. Each lowering marshals its operand to the helper on that
// exit -- some as an address, some as the already-loaded value -- and the
// marshalled register has to be live on the branch that is taken. FCOM/FCOMP
// got that wrong (the address was computed after the branch); this sweep is
// what says the rest do not.
//
// Both halves of the result are published into GPRs and memory, the two things
// check_parity compares: FNSTSW AX carries C0/C2/C3 and TOP, and FSTP qword
// puts ST(0) itself where a value divergence shows up.
TEST_F(JitParityTest, X87MemoryFormsSlowPathMarshalsLiveOperand) {
    struct Form { const char* name; u8 op; u8 ext; };
    static constexpr Form kForms[] = {
        {"FADD m32", 0xD8, 0}, {"FMUL m32", 0xD8, 1},
        {"FCOM m32", 0xD8, 2}, {"FCOMP m32", 0xD8, 3},
        {"FSUB m32", 0xD8, 4}, {"FSUBR m32", 0xD8, 5},
        {"FDIV m32", 0xD8, 6}, {"FDIVR m32", 0xD8, 7},
        {"FADD m64", 0xDC, 0}, {"FMUL m64", 0xDC, 1},
        {"FCOM m64", 0xDC, 2}, {"FCOMP m64", 0xDC, 3},
        {"FSUB m64", 0xDC, 4}, {"FSUBR m64", 0xDC, 5},
        {"FDIV m64", 0xDC, 6}, {"FDIVR m64", 0xDC, 7},
        {"FIADD m32", 0xDA, 0}, {"FIMUL m32", 0xDA, 1},
        {"FICOM m32", 0xDA, 2}, {"FICOMP m32", 0xDA, 3},
        {"FISUB m32", 0xDA, 4}, {"FISUBR m32", 0xDA, 5},
        {"FIDIV m32", 0xDA, 6}, {"FIDIVR m32", 0xDA, 7},
        {"FIADD m16", 0xDE, 0}, {"FIMUL m16", 0xDE, 1},
        {"FICOM m16", 0xDE, 2}, {"FICOMP m16", 0xDE, 3},
        {"FISUB m16", 0xDE, 4}, {"FISUBR m16", 0xDE, 5},
        {"FIDIV m16", 0xDE, 6}, {"FIDIVR m16", 0xDE, 7},
        {"FST m32", 0xD9, 2},  {"FSTP m32", 0xD9, 3},
        {"FST m64", 0xDD, 2},  {"FSTP m64", 0xDD, 3},
        {"FIST m32", 0xDB, 2}, {"FISTP m32", 0xDB, 3},
        {"FIST m16", 0xDF, 2}, {"FISTP m16", 0xDF, 3},
    };
    // Exponents either side of binary64's range, plus a NaN/Inf encoding, all
    // of which force the slow exit. 0xD000 also carries the sign bit.
    for (u16 exp_sign : {u16{0x5000}, u16{0x0001}, u16{0xD000}, u16{0x7FFF}}) {
        for (u64 rhs_bits : {0x3FF0000000000000ULL,   // 1.0 / small ints
                             0xC08F400000000000ULL,   // negative
                             0x0000000000000000ULL}) {
            for (const Form& f : kForms) {
                SCOPED_TRACE(testing::Message()
                             << f.name << " exp=" << exp_sign);
                const u64 significand = 0x8000000000000000ULL;
                ASSERT_TRUE(m_memory->write_bytes(
                    0x2000, reinterpret_cast<const u8*>(&significand),
                    sizeof(significand)));
                ASSERT_TRUE(m_memory->write_bytes(
                    0x2008, reinterpret_cast<const u8*>(&exp_sign),
                    sizeof(exp_sign)));
                ASSERT_TRUE(m_memory->write_bytes(
                    0x2010, reinterpret_cast<const u8*>(&rhs_bits),
                    sizeof(rhs_bits)));
                // FLD tbyte [0x2000] ; <form> [0x2010] ; FNSTSW AX ;
                // FSTP qword [0x2020]
                std::vector<u8> code{0xDB, 0x2D, 0x00, 0x20, 0x00, 0x00};
                code.insert(code.end(),
                            {f.op, static_cast<u8>((f.ext << 3) | 5),
                             0x10, 0x20, 0x00, 0x00});
                code.insert(code.end(), {0xDF, 0xE0});
                code.insert(code.end(),
                            {0xDD, 0x1D, 0x20, 0x20, 0x00, 0x00});
                check_parity(code.data(), code.size());
            }
        }
    }
}

// Same contract for a lowering guarded on the stack tags rather than on the
// precision control: FADD ST(0),ST(0) sources a stack register.
TEST_F(JitParityTest, CmpThenX87ArithThenReadsFlags) {
    // CMP EAX,EBX ; FLD1 ; FADD ST(0),ST(0) ; SETE DL
    check_parity({0x39, 0xD8,
                  0xD9, 0xE8,
                  0xD8, 0xC0,
                  0x0F, 0x94, 0xC2},
        [](CpuState& c) { c.gpr.set32(EAX, 7); c.gpr.set32(EBX, 7); });
}

// ============================================================================
// FST / FSTP m32 native Float80→f32 (finite + Inf/NaN helper) parity
// ============================================================================

TEST_F(JitParityTest, FstM32FiniteMatchesInterpreter) {
    // fld dword [ebx]; fst dword [ebx+4]
    float src = 12.5f;
    u32 bits = 0;
    std::memcpy(&bits, &src, sizeof(bits));
    ASSERT_TRUE(m_memory->write_u32(0x2000, bits).is_ok());
    ASSERT_TRUE(m_memory->write_u32(0x2004, 0xDEADBEEFu).is_ok());
    check_parity({0xD9, 0x03, 0xD9, 0x53, 0x04},
        [](CpuState& cpu) { cpu.gpr.set32(EBX, 0x2000); },
        /*check_flags=*/false);
}

TEST_F(JitParityTest, FstM32ZeroAndNegativeMatchesInterpreter) {
    float src = -0.0f;
    u32 bits = 0;
    std::memcpy(&bits, &src, sizeof(bits));
    ASSERT_TRUE(m_memory->write_u32(0x2000, bits).is_ok());
    check_parity({0xD9, 0x03, 0xD9, 0x53, 0x04},
        [](CpuState& cpu) { cpu.gpr.set32(EBX, 0x2000); },
        /*check_flags=*/false);
}

TEST_F(JitParityTest, FstM32InfUsesHelperParity) {
    // fst dword [ebx] from ST(0)=+Inf (exp gate → jit_fpu_peek_m32).
    ASSERT_TRUE(m_memory->write_u32(0x2000, 0).is_ok());
    check_parity({0xD9, 0x13},
        [](CpuState& cpu) {
            cpu.gpr.set32(EBX, 0x2000);
            cpu.fpu.push(Float80::from_extended(
                0x8000000000000000ULL, 0x7FFF));
        },
        /*check_flags=*/false);
}

TEST_F(JitParityTest, FstpM32PopsAndStores) {
    // fstp dword [ebx] — compare store bits and x87 TOP/tag with interpreter.
    ASSERT_TRUE(m_memory->write_u32(0x2000, 0).is_ok());
    const u8 code[] = {0xD9, 0x1B}; // FSTP m32 [EBX]
    ASSERT_TRUE(m_memory->write_bytes(0x1000, code, sizeof(code)));

    std::vector<u8> initial_mem(0x10000);
    ASSERT_TRUE(m_memory->read_bytes(0x1000, initial_mem.data(),
                                      initial_mem.size()));
    BasicBlock block = build_block(code, sizeof(code));
    ASSERT_GT(block.instruction_count, 0u);

    auto setup = [](CpuState& cpu) {
        cpu.gpr.set32(EBX, 0x2000);
        cpu.fpu.push(Float80(3.141592653589793));
    };

    CpuState icpu;
    init_cpu(icpu);
    setup(icpu);
    {
        Interpreter interp(icpu, *m_memory);
        ASSERT_EQ(interp.run(block.instruction_count), ExecResult::Continue);
    }
    auto interp_bits = m_memory->read_u32(0x2000);
    ASSERT_TRUE(interp_bits.is_ok());

    ASSERT_TRUE(m_memory->write_bytes(0x1000, initial_mem.data(),
                                       initial_mem.size()));
    CpuState jcpu;
    init_cpu(jcpu);
    setup(jcpu);
    JitCompiler jit(*m_memory);
    CompiledBlock* cb = jit.compile(block);
    ASSERT_NE(cb, nullptr);
    ASSERT_EQ(test::execute_like_dispatcher(jit, *cb, jcpu), JitExecResult::Continue);
    auto jit_bits = m_memory->read_u32(0x2000);
    ASSERT_TRUE(jit_bits.is_ok());

    EXPECT_EQ(interp_bits.value(), jit_bits.value());
    EXPECT_EQ(icpu.fpu.top(), jcpu.fpu.top());
    EXPECT_EQ(icpu.fpu.tag_word, jcpu.fpu.tag_word);
    EXPECT_EQ(icpu.fpu.status_word, jcpu.fpu.status_word);
}

TEST_F(JitParityTest, FnstswAxNativeParity) {
    // Already-native status-word store into AX.
    check_parity({0xDF, 0xE0},
        [](CpuState& cpu) {
            cpu.fpu.status_word = 0x1234;
            cpu.gpr.set32(EAX, 0xABCDEF00u);
        },
        /*check_flags=*/false);
}

// ============================================================================
// FST / FSTP m64 native Float80→f64 (finite + Inf/NaN helper) parity
// ============================================================================

TEST_F(JitParityTest, FstM64FiniteMatchesInterpreter) {
    // fld qword [ebx]; fst qword [ebx+8]
    double src = 12.5;
    u64 bits = 0;
    std::memcpy(&bits, &src, sizeof(bits));
    ASSERT_TRUE(m_memory->write_u64(0x2000, bits).is_ok());
    ASSERT_TRUE(m_memory->write_u64(0x2008, 0xDEADBEEFCAFEBABEull).is_ok());
    check_parity({0xDD, 0x03, 0xDD, 0x53, 0x08},
        [](CpuState& cpu) { cpu.gpr.set32(EBX, 0x2000); },
        /*check_flags=*/false);
}

TEST_F(JitParityTest, FstM64ZeroAndNegativeMatchesInterpreter) {
    double src = -0.0;
    u64 bits = 0;
    std::memcpy(&bits, &src, sizeof(bits));
    ASSERT_TRUE(m_memory->write_u64(0x2000, bits).is_ok());
    check_parity({0xDD, 0x03, 0xDD, 0x53, 0x08},
        [](CpuState& cpu) { cpu.gpr.set32(EBX, 0x2000); },
        /*check_flags=*/false);
}

TEST_F(JitParityTest, FstM64InfUsesHelperParity) {
    // fst qword [ebx] from ST(0)=+Inf (exp gate → jit_fpu_peek_m64).
    ASSERT_TRUE(m_memory->write_u64(0x2000, 0).is_ok());
    check_parity({0xDD, 0x13},
        [](CpuState& cpu) {
            cpu.gpr.set32(EBX, 0x2000);
            cpu.fpu.push(Float80::from_extended(
                0x8000000000000000ULL, 0x7FFF));
        },
        /*check_flags=*/false);
}

TEST_F(JitParityTest, FstpM64PopsAndStores) {
    // fstp qword [ebx] — compare store bits and x87 TOP/tag with interpreter.
    ASSERT_TRUE(m_memory->write_u64(0x2000, 0).is_ok());
    const u8 code[] = {0xDD, 0x1B}; // FSTP m64 [EBX]
    ASSERT_TRUE(m_memory->write_bytes(0x1000, code, sizeof(code)));

    std::vector<u8> initial_mem(0x10000);
    ASSERT_TRUE(m_memory->read_bytes(0x1000, initial_mem.data(),
                                      initial_mem.size()));
    BasicBlock block = build_block(code, sizeof(code));
    ASSERT_GT(block.instruction_count, 0u);

    auto setup = [](CpuState& cpu) {
        cpu.gpr.set32(EBX, 0x2000);
        cpu.fpu.push(Float80(3.141592653589793));
    };

    CpuState icpu;
    init_cpu(icpu);
    setup(icpu);
    {
        Interpreter interp(icpu, *m_memory);
        ASSERT_EQ(interp.run(block.instruction_count), ExecResult::Continue);
    }
    auto interp_bits = m_memory->read_u64(0x2000);
    ASSERT_TRUE(interp_bits.is_ok());

    ASSERT_TRUE(m_memory->write_bytes(0x1000, initial_mem.data(),
                                       initial_mem.size()));
    CpuState jcpu;
    init_cpu(jcpu);
    setup(jcpu);
    JitCompiler jit(*m_memory);
    CompiledBlock* cb = jit.compile(block);
    ASSERT_NE(cb, nullptr);
    ASSERT_EQ(test::execute_like_dispatcher(jit, *cb, jcpu), JitExecResult::Continue);
    auto jit_bits = m_memory->read_u64(0x2000);
    ASSERT_TRUE(jit_bits.is_ok());

    EXPECT_EQ(interp_bits.value(), jit_bits.value());
    EXPECT_EQ(icpu.fpu.top(), jcpu.fpu.top());
    EXPECT_EQ(icpu.fpu.tag_word, jcpu.fpu.tag_word);
    EXPECT_EQ(icpu.fpu.status_word, jcpu.fpu.status_word);
}

// x87 arithmetic whose RESULT is subnormal must not be flushed to zero.
// The native ARM64 lowering gated on "biased exponent == 0" and treated that
// whole class as +-0, so any subnormal sum/difference was stored as 0. Measured
// against real Windows via tests/crt_oracle/crt_surface_runner: log(1e-320)
// returned -inf instead of -736.827, because the FADD feeding it flushed.
// Six arithmetic sites shared the gate.
TEST_F(JitParityTest, X87SubnormalResultIsNotFlushed) {
    constexpr GuestAddr kA = 0x2000, kB = 0x2008, kOut = 0x2010;
    // fld qword [kA] ; fadd qword [kB] ; fstp qword [kOut]
    const u8 code[] = {
        0xDD, 0x05, 0x00, 0x20, 0x00, 0x00,
        0xDC, 0x05, 0x08, 0x20, 0x00, 0x00,
        0xDD, 0x1D, 0x10, 0x20, 0x00, 0x00,
    };
    struct Case { const char* name; u64 a; u64 b; };
    const Case cases[] = {
        // smallest positive subnormal + 0  -> still subnormal
        {"min-subnormal + 0",      0x0000000000000001ull, 0x0000000000000000ull},
        // 1e-320 + 0 -- the exact value the CRT probe caught
        {"1e-320 + 0",             0x00000000000FBE76ull, 0x0000000000000000ull},
        // two subnormals summing to a subnormal
        {"subnormal + subnormal",  0x0000000000000003ull, 0x0000000000000004ull},
        // normal - normal cancelling into the subnormal range
        {"normal - normal -> sub", 0x0010000000000000ull, 0x800FFFFFFFFFFFFFull},
        // a genuine zero must still be stored as zero
        {"+0 + +0",                0x0000000000000000ull, 0x0000000000000000ull},
        {"-0 + +0",                0x8000000000000000ull, 0x0000000000000000ull},
    };
    for (const Case& c : cases) {
        SCOPED_TRACE(c.name);
        ASSERT_TRUE(m_memory->write_u64(kA, c.a).is_ok());
        ASSERT_TRUE(m_memory->write_u64(kB, c.b).is_ok());
        ASSERT_TRUE(m_memory->write_u64(kOut, 0xDEADBEEFDEADBEEFull).is_ok());
        // check_parity runs both paths and compares memory, so the stored
        // result is compared directly.
        check_parity(code, sizeof(code));
    }
}
