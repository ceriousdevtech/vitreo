// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// JIT execution tests — native x87 stack, arithmetic, load/store and BCD forms

#include "jit_exec_test_fixture.hpp"

TEST_F(JitExecTest, HotX87MemoryAndStackOpsMatchArchitecturalState) {
    const auto write_float = [&](GuestAddr address, float value) {
        u32 bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        ASSERT_TRUE(m_memory->write_u32(address, bits));
    };
    const auto read_float = [&](GuestAddr address) {
        const auto raw = m_memory->read_u32(address);
        EXPECT_TRUE(raw.is_ok());
        float value = 0.0f;
        if (raw.is_ok()) {
            const u32 bits = raw.value();
            std::memcpy(&value, &bits, sizeof(value));
        }
        return value;
    };

    write_float(0x2000, 2.0f);
    write_float(0x2004, 3.0f);
    write_float(0x2008, 4.0f);
    write_float(0x200C, 8.0f);
    write_float(0x2014, 12.0f);

    const u8 code[] = {
        0xD9, 0x05, 0x00, 0x20, 0x00, 0x00, // fld dword ptr [0x2000]
        0xD9, 0xC0,                         // fld st(0)
        0xD8, 0x05, 0x04, 0x20, 0x00, 0x00, // fadd dword ptr [0x2004]
        0xD8, 0x0D, 0x08, 0x20, 0x00, 0x00, // fmul dword ptr [0x2008]
        0xD8, 0x25, 0x0C, 0x20, 0x00, 0x00, // fsub dword ptr [0x200c]
        0xD8, 0xC1,                         // fadd st(0), st(1): 12 + 2
        0xD8, 0xE1,                         // fsub st(0), st(1): 14 - 2
        0xD8, 0xC9,                         // fmul st(0), st(1): 12 * 2
        0xD8, 0xF1,                         // fdiv st(0), st(1): 24 / 2
        0xD9, 0x15, 0x10, 0x20, 0x00, 0x00, // fst dword ptr [0x2010]
        0xDD, 0xD9,                         // fstp st(1)
        0xD9, 0x05, 0x00, 0x20, 0x00, 0x00, // fld dword ptr [0x2000]
        0xD9, 0x1D, 0x18, 0x20, 0x00, 0x00, // fstp dword ptr [0x2018]
        0xD8, 0x1D, 0x14, 0x20, 0x00, 0x00, // fcomp dword ptr [0x2014]
        0xDF, 0xE0,                         // fnstsw ax
    };

    EXPECT_EQ(jit_run(code, sizeof(code)), JitExecResult::Continue);
    EXPECT_FLOAT_EQ(read_float(0x2010), 12.0f);
    EXPECT_FLOAT_EQ(read_float(0x2018), 2.0f);
    EXPECT_NE(m_cpu->gpr.get16(EAX) & FPU_SW_C3, 0);
    EXPECT_EQ(m_cpu->gpr.get16(EAX) & (FPU_SW_C0 | FPU_SW_C2), 0);
}

TEST_F(JitExecTest, NativeX87StackCopyPopPreservesRotatingState) {
    m_cpu->fpu.push(Float80(6.25));
    const u8 top_before = m_cpu->fpu.top();
    const u16 tag_before = m_cpu->fpu.tag_word;
    const Float80 value_before = m_cpu->fpu.get_st(0);
    m_cpu->gpr.set64(EAX, 0xA5A5A5A5FFFF0000ULL);
    const u8 code[] = {
        0xD9, 0xC0, // fld st(0)
        0xDD, 0xD9, // fstp st(1)
        0xDF, 0xE0, // fnstsw ax
    };

    EXPECT_EQ(jit_run(code, sizeof(code)), JitExecResult::Continue);
    EXPECT_EQ(m_cpu->fpu.top(), top_before);
    EXPECT_EQ(m_cpu->fpu.tag_word, tag_before);
    EXPECT_EQ(m_cpu->fpu.get_st(0).mantissa, value_before.mantissa);
    EXPECT_EQ(m_cpu->fpu.get_st(0).exponent_sign,
              value_before.exponent_sign);
    EXPECT_EQ(m_cpu->gpr.get16(EAX), m_cpu->fpu.status_word);
    EXPECT_EQ(m_cpu->gpr.get64(EAX) & 0xFFFFFFFFFFFF0000ULL,
              0xA5A5A5A5FFFF0000ULL);

    const u8 fninit[] = {0xDB, 0xE3};
    m_cpu->rip = 0x1000;
    EXPECT_EQ(jit_run(fninit, sizeof(fninit)), JitExecResult::Continue);
    EXPECT_EQ(m_cpu->fpu.control_word, 0x037F);
    EXPECT_EQ(m_cpu->fpu.status_word, 0);
    EXPECT_EQ(m_cpu->fpu.tag_word, 0xFFFF);
}

TEST_F(JitExecTest, NativeX87FractionalArithmeticMatchesDelegate) {
    struct FpuSnapshot {
        std::array<Float80, 8> st;
        u16 status_word;
        u16 tag_word;
        u16 control_word;
        u32 ecx;
        u64 eflags;
        u64 tbyte_lo;
        u16 tbyte_hi;
    };

    const auto write_float = [&](GuestAddr address, float value) {
        u32 bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        ASSERT_TRUE(m_memory->write_u32(address, bits));
    };
    write_float(0x2000, 1.1f);
    write_float(0x2004, -2.3f);
    write_float(0x2008, 0.7f);
    ASSERT_TRUE(m_memory->write_u32(0x200C, 42u));
    ASSERT_TRUE(m_memory->write_u64(0x2040, 0x8000000000000000ULL));
    ASSERT_TRUE(m_memory->write_u16(0x2048, 0x3FFF));

    const u8 code[] = {
        0x3D, 0x06, 0x00, 0x00, 0x00,       // cmp eax, 6 (sets carry)
        0xD9, 0x05, 0x00, 0x20, 0x00, 0x00, // fld dword ptr [0x2000]
        0x11, 0xD1,                         // adc ecx, edx
        0xD9, 0x05, 0x04, 0x20, 0x00, 0x00, // fld dword ptr [0x2004]
        0xD8, 0xE9,                         // fsubr st(0), st(1)
        0xD8, 0x0D, 0x08, 0x20, 0x00, 0x00, // fmul dword ptr [0x2008]
        0xD8, 0x35, 0x08, 0x20, 0x00, 0x00, // fdiv dword ptr [0x2008]
        0xD8, 0x3D, 0x00, 0x20, 0x00, 0x00, // fdivr dword ptr [0x2000]
        0xD9, 0xC0,                         // fld st(0)
        0xD8, 0xC1,                         // fadd st(0), st(1)
        0xD8, 0xF1,                         // fdiv st(0), st(1)
        0xD8, 0xF9,                         // fdivr st(0), st(1)
        0xD9, 0xC9,                         // fxch st(1)
        0xD9, 0xE0,                         // fchs
        0xD9, 0xE1,                         // fabs
        0xD9, 0xE8,                         // fld1
        0xD8, 0xC1,                         // fadd st(0), st(1)
        0xD9, 0xEE,                         // fldz
        0xDE, 0xC1,                         // faddp st(1), st(0)
        0xD8, 0xD1,                         // fcom st(1)
        0xD9, 0xE4,                         // ftst
        0xDB, 0x05, 0x0C, 0x20, 0x00, 0x00, // fild dword ptr [0x200C]
        0xDE, 0xC1,                         // faddp st(1), st(0)
        0xDA, 0x05, 0x0C, 0x20, 0x00, 0x00, // fiadd dword ptr [0x200C]
        0xD9, 0xFA,                         // fsqrt
        0xDB, 0xF1,                         // fcomi st(1)
        0xD9, 0x3D, 0x10, 0x20, 0x00, 0x00, // fnstcw word ptr [0x2010]
        0xD9, 0x2D, 0x10, 0x20, 0x00, 0x00, // fldcw word ptr [0x2010]
        0xDB, 0x2D, 0x40, 0x20, 0x00, 0x00, // fld tbyte ptr [0x2040]
        0xDB, 0x3D, 0x50, 0x20, 0x00, 0x00, // fstp tbyte ptr [0x2050]
        0xF9,                               // stc
        0xDA, 0xC1,                         // fcmovb st(1)
        0xDD, 0xD9,                         // fstp st(1)
    };

    const auto run = [&](bool delegate) {
        if (delegate) {
            setenv("VITREO_JIT_DISABLE_FPU_OPS", "1", 1);
        } else {
            unsetenv("VITREO_JIT_DISABLE_FPU_OPS");
        }
        // The x87 sticky exception flags live on the HOST unit until an
        // environment read harvests them (core/math/host_fenv.hpp), and the
        // harvest clears them. Two runs of the same code on one thread are
        // therefore only comparable if each starts from a clean host --
        // otherwise the first run consumes whatever an earlier test left
        // behind and the second sees none of it. Same reason the guest memory
        // region is restored between the halves.
        vitreo::math::clear_host_fp_exceptions();
        m_cpu = std::make_unique<CpuState>();
        m_cpu->init_protected_mode();
        m_cpu->gpr.set32(ESP, 0x10000);
        m_cpu->gpr.set32(EAX, 5);
        m_cpu->gpr.set32(ECX, 10);
        m_cpu->gpr.set32(EDX, 20);
        m_cpu->rip = 0x1000;
        m_jit = std::make_unique<JitCompiler>(*m_memory);
        EXPECT_EQ(jit_run(code, sizeof(code)), JitExecResult::Continue);

        FpuSnapshot snapshot{};
        std::copy(std::begin(m_cpu->fpu.st), std::end(m_cpu->fpu.st),
                  snapshot.st.begin());
        snapshot.status_word = m_cpu->fpu.status_word;
        snapshot.tag_word = m_cpu->fpu.tag_word;
        snapshot.control_word = m_cpu->fpu.control_word;
        snapshot.ecx = m_cpu->gpr.get32(ECX);
        snapshot.eflags = m_cpu->flags.rflags & (FLAG_CF | FLAG_ZF | FLAG_PF);
        const auto lo = m_memory->read_u64(0x2050);
        const auto hi = m_memory->read_u16(0x2058);
        snapshot.tbyte_lo = lo.is_ok() ? lo.value() : 0;
        snapshot.tbyte_hi = hi.is_ok() ? static_cast<u16>(hi.value()) : 0;
        return snapshot;
    };

    const FpuSnapshot native = run(false);
    const FpuSnapshot delegated = run(true);
    unsetenv("VITREO_JIT_DISABLE_FPU_OPS");

    EXPECT_EQ(native.status_word, delegated.status_word);
    EXPECT_EQ(native.tag_word, delegated.tag_word);
    EXPECT_EQ(native.control_word, delegated.control_word);
    EXPECT_EQ(native.ecx, delegated.ecx);
    EXPECT_EQ(native.ecx, 31u);
    EXPECT_EQ(native.eflags, delegated.eflags);
    EXPECT_EQ(native.tbyte_lo, delegated.tbyte_lo);
    EXPECT_EQ(native.tbyte_hi, delegated.tbyte_hi);
    EXPECT_EQ(native.tbyte_lo, 0x8000000000000000ULL);
    EXPECT_EQ(native.tbyte_hi, 0x3FFF);
    for (usize i = 0; i < native.st.size(); ++i) {
        EXPECT_EQ(native.st[i].mantissa, delegated.st[i].mantissa) << i;
        EXPECT_EQ(native.st[i].exponent_sign,
                  delegated.st[i].exponent_sign) << i;
    }
}

TEST_F(JitExecTest, NativeX87M64LoadStoreArithmeticMatchesDelegate) {
    struct FpuSnapshot {
        std::array<Float80, 8> st;
        u16 status_word;
        u16 tag_word;
        u64 stored_fst;
        u64 stored_fstp;
        u64 stored_subnormal;
    };

    const auto write_double = [&](GuestAddr address, double value) {
        u64 bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        ASSERT_TRUE(m_memory->write_u64(address, bits));
    };
    const auto read_u64_bits = [&](GuestAddr address) {
        const auto raw = m_memory->read_u64(address);
        EXPECT_TRUE(raw.is_ok());
        return raw.is_ok() ? raw.value() : 0ull;
    };

    write_double(0x2000, 1.1);
    write_double(0x2008, -2.3);
    write_double(0x2010, 0.7);
    ASSERT_TRUE(m_memory->write_u64(0x2018, 1ull)); // smallest binary64 subnormal

    const u8 code[] = {
        0xDD, 0x05, 0x00, 0x20, 0x00, 0x00, // fld qword ptr [0x2000]
        0xDC, 0x05, 0x08, 0x20, 0x00, 0x00, // fadd qword ptr [0x2008]
        0xDC, 0x0D, 0x10, 0x20, 0x00, 0x00, // fmul qword ptr [0x2010]
        0xDC, 0x35, 0x10, 0x20, 0x00, 0x00, // fdiv qword ptr [0x2010]
        0xDC, 0x25, 0x08, 0x20, 0x00, 0x00, // fsub qword ptr [0x2008]
        0xDD, 0x15, 0x20, 0x20, 0x00, 0x00, // fst qword ptr [0x2020]
        0xDD, 0x1D, 0x28, 0x20, 0x00, 0x00, // fstp qword ptr [0x2028]
        0xDD, 0x05, 0x18, 0x20, 0x00, 0x00, // fld qword ptr [0x2018]
        0xDD, 0x1D, 0x30, 0x20, 0x00, 0x00, // fstp qword ptr [0x2030]
    };

    const auto run = [&](bool delegate) {
        if (delegate) {
            setenv("VITREO_JIT_DISABLE_FPU_OPS", "1", 1);
        } else {
            unsetenv("VITREO_JIT_DISABLE_FPU_OPS");
        }
        // The x87 sticky exception flags live on the HOST unit until an
        // environment read harvests them (core/math/host_fenv.hpp), and the
        // harvest clears them. Two runs of the same code on one thread are
        // therefore only comparable if each starts from a clean host --
        // otherwise the first run consumes whatever an earlier test left
        // behind and the second sees none of it. Same reason the guest memory
        // region is restored between the halves.
        vitreo::math::clear_host_fp_exceptions();
        m_cpu = std::make_unique<CpuState>();
        m_cpu->init_protected_mode();
        m_cpu->gpr.set32(ESP, 0x10000);
        m_cpu->rip = 0x1000;
        m_jit = std::make_unique<JitCompiler>(*m_memory);
        EXPECT_EQ(jit_run(code, sizeof(code)), JitExecResult::Continue);

        FpuSnapshot snapshot{};
        std::copy(std::begin(m_cpu->fpu.st), std::end(m_cpu->fpu.st),
                  snapshot.st.begin());
        snapshot.status_word = m_cpu->fpu.status_word;
        snapshot.tag_word = m_cpu->fpu.tag_word;
        snapshot.stored_fst = read_u64_bits(0x2020);
        snapshot.stored_fstp = read_u64_bits(0x2028);
        snapshot.stored_subnormal = read_u64_bits(0x2030);
        return snapshot;
    };

    const FpuSnapshot native = run(false);
    const FpuSnapshot delegated = run(true);
    unsetenv("VITREO_JIT_DISABLE_FPU_OPS");

    EXPECT_EQ(native.status_word, delegated.status_word);
    EXPECT_EQ(native.tag_word, delegated.tag_word);
    EXPECT_EQ(native.stored_fst, delegated.stored_fst);
    EXPECT_EQ(native.stored_fstp, delegated.stored_fstp);
    EXPECT_EQ(native.stored_subnormal, 1ull);
    EXPECT_EQ(native.stored_subnormal, delegated.stored_subnormal);
    for (usize i = 0; i < native.st.size(); ++i) {
        EXPECT_EQ(native.st[i].mantissa, delegated.st[i].mantissa) << i;
        EXPECT_EQ(native.st[i].exponent_sign,
                  delegated.st[i].exponent_sign) << i;
    }
}

TEST_F(JitExecTest, NativeX87FistEnvFxamMatchesDelegate) {
    struct Snapshot {
        u16 status_word;
        u16 tag_word;
        u16 control_word;
        u32 fistp;
        u32 fisttp;
        u32 env_cw;
    };

    const auto write_float = [&](GuestAddr address, float value) {
        u32 bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        ASSERT_TRUE(m_memory->write_u32(address, bits));
    };
    write_float(0x2000, 2.5f);
    write_float(0x2004, 1.9f);

    const u8 code[] = {
        0xD9, 0x05, 0x00, 0x20, 0x00, 0x00, // fld dword ptr [0x2000]
        0xD9, 0xFC,                         // frndint
        0xDB, 0x1D, 0x10, 0x20, 0x00, 0x00, // fistp dword ptr [0x2010]
        0xD9, 0x05, 0x04, 0x20, 0x00, 0x00, // fld dword ptr [0x2004]
        0xDB, 0x0D, 0x14, 0x20, 0x00, 0x00, // fisttp dword ptr [0x2014]
        0xD9, 0x05, 0x00, 0x20, 0x00, 0x00, // fld dword ptr [0x2000]
        0xD9, 0xE5,                         // fxam
        0xD9, 0x35, 0x20, 0x20, 0x00, 0x00, // fnstenv [0x2020]
        0xD9, 0x25, 0x20, 0x20, 0x00, 0x00, // fldenv [0x2020]
    };

    const auto run = [&](bool delegate) {
        if (delegate) {
            setenv("VITREO_JIT_DISABLE_FPU_OPS", "1", 1);
        } else {
            unsetenv("VITREO_JIT_DISABLE_FPU_OPS");
        }
        // The x87 sticky exception flags live on the HOST unit until an
        // environment read harvests them (core/math/host_fenv.hpp), and the
        // harvest clears them. Two runs of the same code on one thread are
        // therefore only comparable if each starts from a clean host --
        // otherwise the first run consumes whatever an earlier test left
        // behind and the second sees none of it. Same reason the guest memory
        // region is restored between the halves.
        vitreo::math::clear_host_fp_exceptions();
        m_cpu = std::make_unique<CpuState>();
        m_cpu->init_protected_mode();
        m_cpu->gpr.set32(ESP, 0x10000);
        m_cpu->rip = 0x1000;
        m_jit = std::make_unique<JitCompiler>(*m_memory);
        EXPECT_EQ(jit_run(code, sizeof(code)), JitExecResult::Continue);
        Snapshot snapshot{};
        snapshot.status_word = m_cpu->fpu.status_word;
        snapshot.tag_word = m_cpu->fpu.tag_word;
        snapshot.control_word = m_cpu->fpu.control_word;
        snapshot.fistp = m_memory->read_u32(0x2010).value_or(0);
        snapshot.fisttp = m_memory->read_u32(0x2014).value_or(0);
        snapshot.env_cw = m_memory->read_u32(0x2020).value_or(0);
        return snapshot;
    };

    const Snapshot native = run(false);
    const Snapshot delegated = run(true);
    unsetenv("VITREO_JIT_DISABLE_FPU_OPS");

    EXPECT_EQ(native.status_word, delegated.status_word);
    EXPECT_EQ(native.tag_word, delegated.tag_word);
    EXPECT_EQ(native.control_word, delegated.control_word);
    EXPECT_EQ(native.fistp, delegated.fistp);
    EXPECT_EQ(native.fisttp, delegated.fisttp);
    EXPECT_EQ(native.env_cw, delegated.env_cw);
    EXPECT_EQ(native.fistp, 2u);
    EXPECT_EQ(native.fisttp, 1u);
}

TEST_F(JitExecTest, NativeX87FnsaveFrstorMathMatchesDelegate) {
    struct Snapshot {
        u16 status_word;
        u16 tag_word;
        u16 control_word;
        u64 st0_mant;
        u16 st0_exp;
        u32 save_cw;
        u32 save_sw;
        u64 save_st0_mant;
    };

    const auto write_float = [&](GuestAddr address, float value) {
        u32 bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        ASSERT_TRUE(m_memory->write_u32(address, bits));
    };
    write_float(0x2000, 2.0f);
    write_float(0x2004, 4.0f);

    const u8 code[] = {
        0xD9, 0x05, 0x00, 0x20, 0x00, 0x00, // fld dword ptr [0x2000]
        0xD9, 0xFE,                         // fsin
        0xD9, 0x05, 0x04, 0x20, 0x00, 0x00, // fld dword ptr [0x2004]
        0xD9, 0xF1,                         // fyl2x
        0xDD, 0x35, 0x00, 0x21, 0x00, 0x00, // fnsave [0x2100]
        0xDD, 0x25, 0x00, 0x21, 0x00, 0x00, // frstor [0x2100]
    };

    const auto run = [&](bool delegate) {
        if (delegate) {
            setenv("VITREO_JIT_DISABLE_FPU_OPS", "1", 1);
        } else {
            unsetenv("VITREO_JIT_DISABLE_FPU_OPS");
        }
        // The x87 sticky exception flags live on the HOST unit until an
        // environment read harvests them (core/math/host_fenv.hpp), and the
        // harvest clears them. Two runs of the same code on one thread are
        // therefore only comparable if each starts from a clean host --
        // otherwise the first run consumes whatever an earlier test left
        // behind and the second sees none of it. Same reason the guest memory
        // region is restored between the halves.
        vitreo::math::clear_host_fp_exceptions();
        m_cpu = std::make_unique<CpuState>();
        m_cpu->init_protected_mode();
        m_cpu->gpr.set32(ESP, 0x10000);
        m_cpu->rip = 0x1000;
        m_jit = std::make_unique<JitCompiler>(*m_memory);
        EXPECT_EQ(jit_run(code, sizeof(code)), JitExecResult::Continue);
        Snapshot snapshot{};
        snapshot.status_word = m_cpu->fpu.status_word;
        snapshot.tag_word = m_cpu->fpu.tag_word;
        snapshot.control_word = m_cpu->fpu.control_word;
        snapshot.st0_mant = m_cpu->fpu.get_st(0).mantissa;
        snapshot.st0_exp = m_cpu->fpu.get_st(0).exponent_sign;
        snapshot.save_cw = m_memory->read_u32(0x2100).value_or(0);
        snapshot.save_sw = m_memory->read_u32(0x2104).value_or(0);
        snapshot.save_st0_mant = m_memory->read_u64(0x211C).value_or(0);
        return snapshot;
    };

    const Snapshot native = run(false);
    const Snapshot delegated = run(true);
    unsetenv("VITREO_JIT_DISABLE_FPU_OPS");

    EXPECT_EQ(native.status_word, delegated.status_word);
    EXPECT_EQ(native.tag_word, delegated.tag_word);
    EXPECT_EQ(native.control_word, delegated.control_word);
    EXPECT_EQ(native.st0_mant, delegated.st0_mant);
    EXPECT_EQ(native.st0_exp, delegated.st0_exp);
    EXPECT_EQ(native.save_cw, delegated.save_cw);
    EXPECT_EQ(native.save_sw, delegated.save_sw);
    EXPECT_EQ(native.save_st0_mant, delegated.save_st0_mant);
}

TEST_F(JitExecTest, EmmsSetsAllTagsEmpty) {
    // Intel SDM Vol. 2 EMMS: x87FPUTagWord := FFFFH (all tags empty).
    m_cpu->fpu.tag_word = 0x0000;
    const u8 code[] = {0x0F, 0x77};
    EXPECT_EQ(jit_run(code, sizeof(code)), JitExecResult::Continue);
    EXPECT_EQ(m_cpu->fpu.tag_word, 0xFFFF);
}

TEST_F(JitExecTest, FfreeStiMarksTagEmptyPreservesTop) {
    // Intel SDM Vol. 2 FFREE: TAG(ST(i)) ← Empty; TOP and ST contents unchanged.
    m_cpu->fpu.push(Float80(1.0));
    m_cpu->fpu.push(Float80(2.0));
    const u8 top_before = m_cpu->fpu.top();
    const Float80 st0_before = m_cpu->fpu.get_st(0);
    const Float80 st1_before = m_cpu->fpu.get_st(1);
    const u8 phys1 = m_cpu->fpu.st_index(1);
    ASSERT_NE(m_cpu->fpu.get_tag(phys1), FpuTag::Empty);

    const u8 code[] = {0xDD, 0xC1}; // ffree st(1)
    EXPECT_EQ(jit_run(code, sizeof(code)), JitExecResult::Continue);

    EXPECT_EQ(m_cpu->fpu.top(), top_before);
    EXPECT_EQ(m_cpu->fpu.get_tag(phys1), FpuTag::Empty);
    EXPECT_EQ(m_cpu->fpu.get_st(0).mantissa, st0_before.mantissa);
    EXPECT_EQ(m_cpu->fpu.get_st(0).exponent_sign, st0_before.exponent_sign);
    EXPECT_EQ(m_cpu->fpu.get_st(1).mantissa, st1_before.mantissa);
    EXPECT_EQ(m_cpu->fpu.get_st(1).exponent_sign, st1_before.exponent_sign);
}

TEST_F(JitExecTest, FbldM80PushesPackedBcd) {
    // Intel SDM Vol. 2 FBLD: convert m80bcd → push ST(0).
    // Packed BCD for +1234: digits in bytes 0..8, sign in byte 9.
    u8 bcd[10]{};
    bcd[0] = 0x34; // digits 3,4
    bcd[1] = 0x12; // digits 1,2
    ASSERT_TRUE(m_memory->write_bytes(0x3000, bcd, sizeof(bcd)));
    m_cpu->gpr.set32(EAX, 0x3000);

    const u8 code[] = {0xDF, 0x20}; // fbld [eax]
    EXPECT_EQ(jit_run(code, sizeof(code)), JitExecResult::Continue);
    EXPECT_DOUBLE_EQ(m_cpu->fpu.get_st(0).to_double(), 1234.0);
}

TEST_F(JitExecTest, FbstpM80StoresPackedBcdAndPops) {
    // Intel SDM Vol. 2 FBSTP: store ST(0) as m80bcd and pop.
    m_cpu->fpu.push(Float80(-5678.0));
    const u8 top_before = m_cpu->fpu.top();
    m_cpu->gpr.set32(EAX, 0x3010);

    const u8 code[] = {0xDF, 0x30}; // fbstp [eax]
    EXPECT_EQ(jit_run(code, sizeof(code)), JitExecResult::Continue);

    EXPECT_EQ(m_cpu->fpu.top(), static_cast<u8>((top_before + 1) & 7));
    u8 bcd[10]{};
    ASSERT_TRUE(m_memory->read_bytes(0x3010, bcd, sizeof(bcd)));
    EXPECT_EQ(bcd[0], 0x78);
    EXPECT_EQ(bcd[1], 0x56);
    EXPECT_EQ(bcd[9], 0x80);
}
