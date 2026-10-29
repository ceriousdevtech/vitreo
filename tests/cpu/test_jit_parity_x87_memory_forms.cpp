// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// JIT vs Interpreter parity tests (32-bit mode): x87 FLD/FST/FSTP m32 and m64 forms
//
// Fixture and shared helpers: jit_parity_fixture.hpp.

#include "jit_parity_fixture.hpp"

// ---------------------------------------------------------------------------
// FLD / FST / FSTP m32 and m64: the architectural state, not just ST0.
//
// The lowerings for these four reassemble the IEEE encodings out of the 80-bit
// register with integer bit operations, and gate on the VALUE rather than on
// the control word. Every observable that gate has to preserve is invisible to
// a `to_double()` comparison: a NaN's payload and sign, the tag word, TOP, and
// the sticky exception bits a narrowing store raises. So this fixture compares
// the whole x87 register file bit for bit, FSW, FTW, and the stored bytes.
// ---------------------------------------------------------------------------
class JitX87MemoryFormParityTest : public JitFpuParityTest {
protected:
    struct FpuSnapshot {
        u16 status_word{};
        u16 tag_word{};
        std::vector<u64> mant;
        std::vector<u16> exp;
        std::vector<u8> mem;
        bool operator==(const FpuSnapshot& o) const {
            return status_word == o.status_word && tag_word == o.tag_word &&
                   mant == o.mant && exp == o.exp && mem == o.mem;
        }
    };

    static std::string describe(const FpuSnapshot& s) {
        std::ostringstream os;
        os << std::hex << std::setfill('0');
        os << "FSW=" << std::setw(4) << s.status_word
           << " FTW=" << std::setw(4) << s.tag_word;
        for (usize i = 0; i < s.mant.size(); ++i) {
            os << " st" << std::dec << i << std::hex << "="
               << std::setw(4) << s.exp[i] << ":"
               << std::setw(16) << s.mant[i];
        }
        os << " mem=";
        for (u8 b : s.mem) os << std::setw(2) << static_cast<u32>(b);
        return os.str();
    }

    FpuSnapshot snapshot(const std::vector<u8>& code, u16 cw, bool use_jit,
                         u32 addr, u32 len) {
        EXPECT_TRUE(m_memory->write_bytes(0x1000, code.data(), code.size()));
        BasicBlock block = build_block(code.data(), code.size());
        EXPECT_GT(block.instruction_count, 0u);
        CpuState cpu;
        init_cpu(cpu);
        cpu.fpu.control_word = cw;
        if (use_jit) {
            JitCompiler jit(*m_memory);
            auto* compiled = jit.compile(block);
            EXPECT_NE(compiled, nullptr) << "JIT declined the block";
            if (compiled) {
                EXPECT_EQ(test::execute_like_dispatcher(jit, *compiled, cpu), JitExecResult::Continue);
            }
        } else {
            Interpreter interp(cpu, *m_memory);
            EXPECT_EQ(interp.run(block.instruction_count), ExecResult::Continue);
        }
        FpuSnapshot s;
        s.status_word = cpu.fpu.status_word;
        s.tag_word = cpu.fpu.tag_word;
        for (int i = 0; i < 8; ++i) {
            s.mant.push_back(cpu.fpu.st[i].mantissa);
            s.exp.push_back(cpu.fpu.st[i].exponent_sign);
        }
        if (len != 0) {
            s.mem.resize(len);
            EXPECT_TRUE(m_memory->read_bytes(addr, s.mem.data(), len));
        }
        return s;
    }

    /// Runs `code` through both engines from the same start state and
    /// compares everything x87-observable. `addr`/`len` cover the store's
    /// destination; it is poisoned first so a store that never happens fails.
    void expect_parity(const char* what, const std::vector<u8>& code, u16 cw,
                       u32 addr, u32 len) {
        std::vector<u8> poison(len ? len : 1, 0xCD);
        if (len) ASSERT_TRUE(m_memory->write_bytes(addr, poison.data(), len));
        const FpuSnapshot i = snapshot(code, cw, /*use_jit=*/false, addr, len);
        if (len) ASSERT_TRUE(m_memory->write_bytes(addr, poison.data(), len));
        const FpuSnapshot j = snapshot(code, cw, /*use_jit=*/true, addr, len);
        EXPECT_TRUE(i == j)
            << what << " diverged at cw=0x" << std::hex << cw
            << "\n  interp: " << describe(i)
            << "\n  jit:    " << describe(j);
    }

    /// PC = 24 / 53 / 64 crossed with all four rounding modes, exceptions
    /// masked. A store narrows, and narrowing obeys RC (Intel SDM Vol. 1
    /// 8.1.5.1) -- the old control-word guard checked only PC.
    static std::vector<u16> control_words() {
        std::vector<u16> out;
        for (u16 pc : {u16{0x0000}, u16{0x0200}, u16{0x0300}}) {
            for (u16 rc : {u16{0x0000}, u16{0x0400}, u16{0x0800}, u16{0x0C00}}) {
                out.push_back(static_cast<u16>(0x003F | pc | rc));
            }
        }
        return out;
    }

    static std::vector<u8> mem_form(u8 b0, u8 modrm, u32 addr) {
        return {b0, modrm,
                static_cast<u8>(addr), static_cast<u8>(addr >> 8),
                static_cast<u8>(addr >> 16), static_cast<u8>(addr >> 24)};
    }
    static void append(std::vector<u8>& v, const std::vector<u8>& t) {
        v.insert(v.end(), t.begin(), t.end());
    }
};

TEST_F(JitX87MemoryFormParityTest, LoadStoreM32CoversEveryBinary32Class) {
    struct Case { const char* name; u32 bits; };
    const Case cases[] = {
        {"+0",              0x00000000u},
        {"-0",              0x80000000u},
        {"1.5",             0x3FC00000u},
        {"-1.5",            0xBFC00000u},
        {"min normal",      0x00800000u},
        {"max normal",      0x7F7FFFFFu},
        {"-max normal",     0xFF7FFFFFu},
        {"min subnormal",   0x00000001u},
        {"max subnormal",   0x007FFFFFu},
        {"-subnormal",      0x80000001u},
        {"+inf",            0x7F800000u},
        {"-inf",            0xFF800000u},
        {"QNaN payload",    0x7FC00001u},
        {"-QNaN payload",   0xFFD5AAAAu},
        {"SNaN payload",    0x7F800001u},
    };
    // fld dword[src] ; fst dword[dst] ; fstp dword[dst+4]
    std::vector<u8> code = mem_form(0xD9, 0x05, 0x2000);
    append(code, mem_form(0xD9, 0x15, 0x2010));
    append(code, mem_form(0xD9, 0x1D, 0x2014));

    for (const Case& c : cases) {
        ASSERT_TRUE(m_memory->write_bytes(
            0x2000, reinterpret_cast<const u8*>(&c.bits), 4));
        for (u16 cw : control_words()) {
            expect_parity(c.name, code, cw, 0x2010, 8);
        }
    }
}

TEST_F(JitX87MemoryFormParityTest, LoadStoreM64CoversEveryBinary64Class) {
    struct Case { const char* name; u64 bits; };
    const Case cases[] = {
        {"+0",              0x0000000000000000ull},
        {"-0",              0x8000000000000000ull},
        {"1.5",             0x3FF8000000000000ull},
        {"-1.5",            0xBFF8000000000000ull},
        {"min normal",      0x0010000000000000ull},
        {"max normal",      0x7FEFFFFFFFFFFFFFull},
        {"-max normal",     0xFFEFFFFFFFFFFFFFull},
        {"min subnormal",   0x0000000000000001ull},
        {"max subnormal",   0x000FFFFFFFFFFFFFull},
        {"+inf",            0x7FF0000000000000ull},
        {"-inf",            0xFFF0000000000000ull},
        {"QNaN payload",    0x7FF8000000000001ull},
        {"-QNaN payload",   0xFFFAAAAA55555555ull},
        {"SNaN payload",    0x7FF0000000000001ull},
    };
    // fld qword[src] ; fst qword[dst] ; fstp qword[dst+8]
    std::vector<u8> code = mem_form(0xDD, 0x05, 0x2020);
    append(code, mem_form(0xDD, 0x15, 0x2030));
    append(code, mem_form(0xDD, 0x1D, 0x2038));

    for (const Case& c : cases) {
        ASSERT_TRUE(m_memory->write_bytes(
            0x2020, reinterpret_cast<const u8*>(&c.bits), 8));
        for (u16 cw : control_words()) {
            expect_parity(c.name, code, cw, 0x2030, 16);
        }
    }
}

// A store from a register that really does hold more significand bits than the
// destination can take. This is the case the old PC-based guard got wrong: it
// let PC = 53 through to a lowering that TRUNCATED the significand, when
// FpuState::store_m64 rounds it under RC and raises PE. FILD m64 deposits up
// to 64 significant bits at ANY precision control, so PC never implied what
// the guard assumed.
TEST_F(JitX87MemoryFormParityTest, InexactStoreRoundsUnderRcAndRaisesPe) {
    const i64 ints[] = {
        0x7FFFFFFFFFFFFFFFll,   // 63 significant bits
        0x0123456789ABCDEFll,
        -0x7FFFFFFFFFFFFFFFll,
        0x0000000000FFFFFFll,   // 24 bits: exact in m64, inexact in m32
        0x0000000001000001ll,   // 25 bits: inexact in m32, exact in m64
        1ll,
    };
    // fild qword[src] ; fst dword[dst] ; fstp qword[dst+8]
    std::vector<u8> code = mem_form(0xDF, 0x2D, 0x2040);
    append(code, mem_form(0xD9, 0x15, 0x2050));
    append(code, mem_form(0xDD, 0x1D, 0x2058));

    for (i64 v : ints) {
        ASSERT_TRUE(m_memory->write_bytes(
            0x2040, reinterpret_cast<const u8*>(&v), 8));
        for (u16 cw : control_words()) {
            expect_parity("fild/fst", code, cw, 0x2050, 16);
        }
    }
}

// The same argument from the other direction: FLD m80 loads all 64 significand
// bits, so the value in ST(0) can be inexact in both destination formats.
// Includes the 80-bit-only encodings a binary64 source cannot produce --
// denormals, pseudo-denormals and unnormals -- which the value gate has to
// send to the interpreter rather than re-encode.
TEST_F(JitX87MemoryFormParityTest, StoreOfExtendedOnlyValuesMatchesInterpreter) {
    struct Case { const char* name; u64 mant; u16 exp; };
    const Case cases[] = {
        {"full precision",  0xFFFFFFFFFFFFFFFFull, 0x3FFF},
        {"one ulp over 53", 0x8000000000000400ull, 0x3FFF},
        {"exact in m64",    0x8000000000000800ull, 0x3FFF},
        {"exact in m32",    0x8000010000000000ull, 0x3FFF},
        {"extended denorm", 0x0000000000000001ull, 0x0000},
        {"neg denorm",      0x4000000000000000ull, 0x8000},
        {"unnormal",        0x4000000000000000ull, 0x3FFF},
        {"pseudo inf",      0x0000000000000000ull, 0x7FFF},
        {"indefinite",      0xC000000000000000ull, 0xFFFF},
        {"snan",            0x8000000000000001ull, 0x7FFF},
        {"huge exponent",   0x8000000000000000ull, 0x7000},
        {"tiny exponent",   0x8000000000000000ull, 0x0001},
    };
    // fld tbyte[src] ; fst dword[dst] ; fstp qword[dst+8]
    std::vector<u8> code = mem_form(0xDB, 0x2D, 0x2040);
    append(code, mem_form(0xD9, 0x15, 0x2050));
    append(code, mem_form(0xDD, 0x1D, 0x2058));

    for (const Case& c : cases) {
        ASSERT_TRUE(m_memory->write_bytes(
            0x2040, reinterpret_cast<const u8*>(&c.mant), 8));
        ASSERT_TRUE(m_memory->write_bytes(
            0x2048, reinterpret_cast<const u8*>(&c.exp), 2));
        for (u16 cw : control_words()) {
            expect_parity(c.name, code, cw, 0x2050, 16);
        }
    }
}

// Intel SDM Vol. 1 8.5.1.1. A push onto a register the tag word does not mark
// Empty is a stack OVERFLOW and a read of an Empty register is a stack
// UNDERFLOW: both set IE | SF, both deliver the indefinite QNaN with #I
// masked, and both are decided by the tag word rather than by the value. The
// native FLD lowerings used to move TOP and store unconditionally.
TEST_F(JitX87MemoryFormParityTest, StackFaultsMatchInterpreter) {
    const u32 one = 0x3F800000u;
    ASSERT_TRUE(m_memory->write_bytes(
        0x2000, reinterpret_cast<const u8*>(&one), 4));

    // Nine pushes onto an eight-register stack: the ninth overflows.
    for (int n = 1; n <= 9; ++n) {
        std::vector<u8> code;
        for (int i = 0; i < n; ++i) append(code, mem_form(0xD9, 0x05, 0x2000));
        expect_parity("fld overflow", code, 0x037F, 0, 0);
    }

    // FSTP m32 / m64 from an empty stack: underflow, and TOP still advances.
    std::vector<u8> under32 = mem_form(0xD9, 0x1D, 0x2010);
    expect_parity("fstp m32 underflow", under32, 0x037F, 0x2010, 4);
    std::vector<u8> under64 = mem_form(0xDD, 0x1D, 0x2010);
    expect_parity("fstp m64 underflow", under64, 0x037F, 0x2010, 8);

    // Underflow where the stale register bits happen to be a perfectly
    // ordinary finite value: only the tag can tell the lowering to bail.
    std::vector<u8> stale = mem_form(0xD9, 0x05, 0x2000);   // fld -> ST0
    append(stale, {0xDD, 0xD8});                             // fstp st(0) (pop)
    append(stale, mem_form(0xD9, 0x1D, 0x2010));             // fstp m32
    expect_parity("stale-value underflow", stale, 0x037F, 0x2010, 4);
}

// x87 writes no EFLAGS (Intel SDM Vol. 1 8.1.4), and the JIT carries pending
// lazy flags in NZCV. The load/store lowerings gate on CBZ/CBNZ/TBZ for that
// reason; the m64 pair used to gate on SUBS with no spill at all.
TEST_F(JitX87MemoryFormParityTest, MemoryFormsPreserveFlagsForFollowingJcc) {
    const u32 one = 0x3F800000u;
    const u64 oned = 0x3FF0000000000000ull;
    ASSERT_TRUE(m_memory->write_bytes(
        0x2000, reinterpret_cast<const u8*>(&one), 4));
    ASSERT_TRUE(m_memory->write_bytes(
        0x2020, reinterpret_cast<const u8*>(&oned), 8));

    struct Form { const char* name; std::vector<u8> body; };
    std::vector<Form> forms;
    {
        std::vector<u8> b = mem_form(0xD9, 0x05, 0x2000);
        append(b, mem_form(0xD9, 0x1D, 0x2010));
        forms.push_back({"fld/fstp m32", b});
    }
    {
        std::vector<u8> b = mem_form(0xDD, 0x05, 0x2020);
        append(b, mem_form(0xDD, 0x1D, 0x2010));
        forms.push_back({"fld/fstp m64", b});
    }

    for (const Form& f : forms) {
        for (u32 lhs : {5u, 7u}) {
            // mov eax,lhs ; cmp eax,5 ; <x87> ; sete bl
            std::vector<u8> code = {
                0xB8, static_cast<u8>(lhs), 0x00, 0x00, 0x00,
                0x83, 0xF8, 0x05,
            };
            append(code, f.body);
            append(code, {0x0F, 0x94, 0xC3});
            check_parity(code.data(), code.size(), {}, /*check_flags=*/true);
        }
    }
}
