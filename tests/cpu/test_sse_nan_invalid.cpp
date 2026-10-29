// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64

/// @file test_sse_nan_invalid.cpp
/// @brief SSE invalid-operation (MXCSR.IE) and NaN results in the interpreter,
///        against the Intel SDM. MXCSR is read as STMXCSR would read it: the
///        architectural value with the host's accumulated SSE flags
///        harvested into it.
///
/// Sources (expected values come from these, never from a Vitreo run):
///   - Intel SDM Vol. 1 §4.8.3.4 and Table 4-7 ("Rules for Handling NaNs"):
///     for SSE arithmetic the result is SRC1 if it is a NaN, else SRC2 if it
///     is a NaN, an SNaN quietened; an SNaN operand signals invalid operation
///     (#IA) whichever operand it is. Vol. 1 §4.9.1.2 / §11.5.2.1: an
///     invalid arithmetic operation (inf - inf, 0 * inf, 0 / 0, inf / inf)
///     also signals #IA and returns the QNaN floating-point indefinite.
///   - Intel SDM Vol. 2 COMISS/COMISD: "Invalid (if SNaN or QNaN operands)";
///     UCOMISS/UCOMISD: "Invalid (if SNaN Operands)".
///   - Intel SDM Vol. 2 CMPPS/CMPPD Table 3-1: "Invalid if SNaN operand and
///     invalid if QNaN and predicate as listed in Table 3-1" (its "Signals
///     #IA on QNAN" column), and the Unordered result column.
///   - Intel SDM Vol. 2 MINPS/MAXPS: "Invalid (including QNaN Source
///     Operand)", the second operand returned.
/// With MXCSR = 0x1F80 every exception is masked: the flag is set and the
/// masked result is written.
///
/// Every test runs on both engines, and both are held to the same SDM values.

#include "core/cpu/decode/decoder.hpp"
#include "core/cpu/interpreter/interpreter.hpp"
#include "core/cpu/jit/jit_compiler.hpp"
#include "core/math/host_fenv.hpp"
#include "jit_dispatch_execute.hpp"

#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <vector>

using namespace vitreo;
using namespace vitreo::cpu;
using namespace vitreo::cpu::jit;

namespace {

constexpr u32 kIE = 1u << 0;
constexpr u32 kMxcsr = 0x1F80u;

// f32 / f64 bit patterns.
constexpr u32 kQnan32 = 0x7FC00123u;   // QNaN with a payload
constexpr u32 kQnan32b = 0xFFC00456u;  // another QNaN (negative)
constexpr u32 kSnan32 = 0x7FA00001u;   // SNaN
constexpr u32 kSnan32q = 0x7FE00001u;  // the same SNaN quietened
constexpr u32 kOne32 = 0x3F800000u;
constexpr u32 kZero32 = 0x00000000u;
constexpr u32 kInf32 = 0x7F800000u;
constexpr u32 kNegInf32 = 0xFF800000u;
constexpr u32 kIndef32 = 0xFFC00000u;  // the QNaN floating-point indefinite

constexpr u64 kQnan64 = 0x7FF8000000000123ull;
constexpr u64 kQnan64b = 0xFFF8000000000456ull;
constexpr u64 kSnan64 = 0x7FF4000000000001ull;
constexpr u64 kSnan64q = 0x7FFC000000000001ull;
constexpr u64 kOne64 = 0x3FF0000000000000ull;
constexpr u64 kZero64 = 0x0000000000000000ull;
constexpr u64 kInf64 = 0x7FF0000000000000ull;
constexpr u64 kIndef64 = 0xFFF8000000000000ull;

enum class Engine { Interpreter, Jit };

class SseNanInvalid : public ::testing::TestWithParam<Engine> {
protected:
    void SetUp() override {
        m_memory = std::make_unique<GuestMemory>();
        ASSERT_TRUE(m_memory->map_region(0x1000, 0x10000,
                                         MemoryProtection::ReadWriteExecute));
    }

    struct Out {
        XmmReg xmm0;
        XmmReg ymm0_high;  ///< YMM0[255:128]
        u32 mxcsr;
        bool zf, pf, cf;
    };

    [[nodiscard]] static bool jit() { return GetParam() == Engine::Jit; }

    /// Run one instruction with XMM0 = @p x0, XMM1 = @p x1 (YMM[255:128] =
    /// @p x0_high / @p x1_high), MXCSR = 0x1F80.
    Out run(const std::vector<u8>& code, const XmmReg& x0, const XmmReg& x1,
            const XmmReg& x0_high = XmmReg{}, const XmmReg& x1_high = XmmReg{}) {
        EXPECT_TRUE(m_memory->write_bytes(0x1000, code.data(), code.size()));
        CpuState cpu;
        cpu.init_long_mode();
        cpu.gpr.set64(static_cast<u8>(RegIndex::RSP), 0x10000);
        cpu.rip = 0x1000;
        cpu.sse.xmm[0] = x0;
        cpu.sse.xmm[1] = x1;
        cpu.avx.ymm_high[0] = x0_high;
        cpu.avx.ymm_high[1] = x1_high;
        cpu.sse.mxcsr = kMxcsr;
        if (!jit()) {
            // Hand the host accumulator to the x87 class, as a preceding x87
            // instruction leaves it. An SSE instruction has to claim it
            // (math::apply_sse_rounding) before it signals, or host_fenv.cpp
            // credits its invalid operation to the FSW instead of MXCSR.
            math::apply_x87_rounding(0x037F);
            math::discard_sse_exceptions();
            Interpreter interp(cpu, *m_memory);
            EXPECT_EQ(interp.step(), ExecResult::Continue);
        } else {
            // Tier 1 claims the accumulator only in front of
            // FpRoundingClass::Sse opcodes, and CMP* is deliberately outside
            // that class (instruction_compilation.cpp), so it runs here with
            // the SSE environment installed, as earlier SSE arithmetic leaves
            // it.
            math::apply_sse_rounding(kMxcsr);
            math::discard_sse_exceptions();
            BasicBlock block;
            block.start_addr = 0x1000;
            MemoryByteReader reader(code.data(), code.size());
            DecodedInstruction inst;
            EXPECT_EQ(decode_instruction(reader, DecodeMode::Mode64, inst),
                      DecodeError::Success);
            block.instructions.push_back(inst);
            block.byte_length = static_cast<u32>(reader.position());
            block.instruction_count = 1;
            block.is_64bit = true;
            JitCompiler compiler(*m_memory);
            CompiledBlock* cb = compiler.compile(block);
            EXPECT_NE(cb, nullptr) << "tier 1 declined the block";
            if (cb == nullptr) return Out{};
            EXPECT_EQ(test::execute_like_dispatcher(compiler, *cb, cpu),
                      JitExecResult::Continue);
        }
        EXPECT_EQ(cpu.rip, 0x1000 + code.size());
        cpu.materialize_lazy_flags();
        Out o;
        o.xmm0 = cpu.sse.xmm[0];
        o.ymm0_high = cpu.avx.ymm_high[0];
        o.mxcsr = cpu.sse.mxcsr | math::harvest_sse_exceptions();
        o.zf = cpu.flags.zf();
        o.pf = cpu.flags.pf();
        o.cf = cpu.flags.cf();
        return o;
    }

    static XmmReg ps(u32 l0, u32 l1 = kOne32) {
        XmmReg r;
        r.u32_[0] = l0;
        r.u32_[1] = l1;
        r.u32_[2] = kOne32;
        r.u32_[3] = kOne32;
        return r;
    }
    static XmmReg pd(u64 l0) {
        XmmReg r;
        r.u64_[0] = l0;
        r.u64_[1] = kOne64;
        return r;
    }

    std::unique_ptr<GuestMemory> m_memory;
};

struct ArithCase {
    const char* what;
    u64 a, b;      ///< lane-0 operands (SRC1/DEST, SRC2)
    u64 result;    ///< expected lane 0
    bool ie;       ///< expected MXCSR.IE
};

}  // namespace

// ADD/SUB/MUL/DIV, scalar and packed, single and double: the NaN result and
// MXCSR.IE of every operand-kind combination of Table 4-7, and the invalid
// operations.
TEST_P(SseNanInvalid, ArithmeticNaNRulesAndInvalid) {
    struct Form {
        const char* name;
        std::vector<u8> code;  // op xmm0, xmm1
        bool dbl;
        const char* invalid_what;
        u64 invalid_a, invalid_b;
    };
    const Form forms[] = {
        {"addss", {0xF3, 0x0F, 0x58, 0xC1}, false, "inf + -inf", kInf32, kNegInf32},
        {"addps", {0x0F, 0x58, 0xC1}, false, "inf + -inf", kInf32, kNegInf32},
        {"subss", {0xF3, 0x0F, 0x5C, 0xC1}, false, "inf - inf", kInf32, kInf32},
        {"subps", {0x0F, 0x5C, 0xC1}, false, "inf - inf", kInf32, kInf32},
        {"mulss", {0xF3, 0x0F, 0x59, 0xC1}, false, "0 * inf", kZero32, kInf32},
        {"mulps", {0x0F, 0x59, 0xC1}, false, "0 * inf", kZero32, kInf32},
        {"divss", {0xF3, 0x0F, 0x5E, 0xC1}, false, "0 / 0", kZero32, kZero32},
        {"divps", {0x0F, 0x5E, 0xC1}, false, "inf / inf", kInf32, kInf32},
        {"addsd", {0xF2, 0x0F, 0x58, 0xC1}, true, "inf + -inf", kInf64, kInf64 | (u64{1} << 63)},
        {"addpd", {0x66, 0x0F, 0x58, 0xC1}, true, "inf + -inf", kInf64, kInf64 | (u64{1} << 63)},
        {"subsd", {0xF2, 0x0F, 0x5C, 0xC1}, true, "inf - inf", kInf64, kInf64},
        {"subpd", {0x66, 0x0F, 0x5C, 0xC1}, true, "inf - inf", kInf64, kInf64},
        {"mulsd", {0xF2, 0x0F, 0x59, 0xC1}, true, "0 * inf", kZero64, kInf64},
        {"mulpd", {0x66, 0x0F, 0x59, 0xC1}, true, "0 * inf", kZero64, kInf64},
        {"divsd", {0xF2, 0x0F, 0x5E, 0xC1}, true, "0 / 0", kZero64, kZero64},
        {"divpd", {0x66, 0x0F, 0x5E, 0xC1}, true, "inf / inf", kInf64, kInf64},
        // ADDSUBPS/PD: lane 0 is a subtraction (Intel SDM Vol. 2 ADDSUBPS).
        {"addsubps", {0xF2, 0x0F, 0xD0, 0xC1}, false, "inf - inf", kInf32, kInf32},
        {"addsubpd", {0x66, 0x0F, 0xD0, 0xC1}, true, "inf - inf", kInf64, kInf64},
    };
    for (const Form& f : forms) {
        SCOPED_TRACE(f.name);
        const std::vector<ArithCase> cases = f.dbl
            ? std::vector<ArithCase>{
                  {"QNaN, SNaN", kQnan64, kSnan64, kQnan64, true},
                  {"SNaN, QNaN", kSnan64, kQnan64b, kSnan64q, true},
                  {"1, SNaN", kOne64, kSnan64, kSnan64q, true},
                  {"SNaN, 1", kSnan64, kOne64, kSnan64q, true},
                  {"QNaN, QNaN", kQnan64, kQnan64b, kQnan64, false},
                  {"1, QNaN", kOne64, kQnan64, kQnan64, false},
                  {f.invalid_what, f.invalid_a, f.invalid_b, kIndef64, true}}
            : std::vector<ArithCase>{
                  {"QNaN, SNaN", kQnan32, kSnan32, kQnan32, true},
                  {"SNaN, QNaN", kSnan32, kQnan32b, kSnan32q, true},
                  {"1, SNaN", kOne32, kSnan32, kSnan32q, true},
                  {"SNaN, 1", kSnan32, kOne32, kSnan32q, true},
                  {"QNaN, QNaN", kQnan32, kQnan32b, kQnan32, false},
                  {"1, QNaN", kOne32, kQnan32, kQnan32, false},
                  {f.invalid_what, f.invalid_a, f.invalid_b, kIndef32, true}};
        for (const ArithCase& c : cases) {
            SCOPED_TRACE(c.what);
            const XmmReg x0 = f.dbl ? pd(c.a) : ps(static_cast<u32>(c.a));
            const XmmReg x1 = f.dbl ? pd(c.b) : ps(static_cast<u32>(c.b));
            const Out o = run(f.code, x0, x1);
            if (f.dbl) {
                EXPECT_EQ(o.xmm0.u64_[0], c.result) << std::hex << o.xmm0.u64_[0];
            } else {
                EXPECT_EQ(o.xmm0.u32_[0], static_cast<u32>(c.result))
                    << std::hex << o.xmm0.u32_[0];
            }
            EXPECT_EQ((o.mxcsr & kIE) != 0, c.ie) << "MXCSR 0x" << std::hex << o.mxcsr;
        }
    }
}

// HADDPS/HSUBPS lane 0 is DEST[31:0] OP DEST[63:32] (Intel SDM Vol. 2): the
// same Table 4-7 rules with both operands taken from DEST.
TEST_P(SseNanInvalid, HorizontalAddSubNaNRulesAndInvalid) {
    struct Form {
        const char* name;
        std::vector<u8> code;
    };
    for (const Form& f : {Form{"haddps", {0xF2, 0x0F, 0x7C, 0xC1}},
                          Form{"hsubps", {0xF2, 0x0F, 0x7D, 0xC1}}}) {
        SCOPED_TRACE(f.name);
        const bool sub = std::string(f.name) == "hsubps";
        for (const ArithCase& c :
             {ArithCase{"QNaN, SNaN", kQnan32, kSnan32, kQnan32, true},
              ArithCase{"1, SNaN", kOne32, kSnan32, kSnan32q, true},
              ArithCase{"QNaN, QNaN", kQnan32, kQnan32b, kQnan32, false},
              ArithCase{"invalid", kInf32, sub ? kInf32 : kNegInf32, kIndef32,
                        true}}) {
            SCOPED_TRACE(c.what);
            const Out o = run(f.code,
                              ps(static_cast<u32>(c.a), static_cast<u32>(c.b)),
                              ps(kOne32));
            EXPECT_EQ(o.xmm0.u32_[0], static_cast<u32>(c.result))
                << std::hex << o.xmm0.u32_[0];
            EXPECT_EQ((o.mxcsr & kIE) != 0, c.ie) << "MXCSR 0x" << std::hex << o.mxcsr;
        }
    }
}

// COMISS/COMISD signal invalid for a QNaN or an SNaN, UCOMISS/UCOMISD for an
// SNaN only; every unordered compare sets ZF = PF = CF = 1.
TEST_P(SseNanInvalid, ComisSignalsOnQnanUcomisOnlyOnSnan) {
    struct Form {
        const char* name;
        std::vector<u8> code;
        bool dbl;
        bool signaling;
    };
    const Form forms[] = {
        {"comiss", {0x0F, 0x2F, 0xC1}, false, true},
        {"ucomiss", {0x0F, 0x2E, 0xC1}, false, false},
        {"comisd", {0x66, 0x0F, 0x2F, 0xC1}, true, true},
        {"ucomisd", {0x66, 0x0F, 0x2E, 0xC1}, true, false},
    };
    for (const Form& f : forms) {
        SCOPED_TRACE(f.name);
        for (const bool snan : {false, true}) {
            SCOPED_TRACE(snan ? "SNaN" : "QNaN");
            const u64 nan = f.dbl ? (snan ? kSnan64 : kQnan64)
                                  : (snan ? kSnan32 : kQnan32);
            for (const bool nan_first : {false, true}) {
                const u64 one = f.dbl ? kOne64 : kOne32;
                const u64 a = nan_first ? nan : one;
                const u64 b = nan_first ? one : nan;
                const XmmReg x0 = f.dbl ? pd(a) : ps(static_cast<u32>(a));
                const XmmReg x1 = f.dbl ? pd(b) : ps(static_cast<u32>(b));
                const Out o = run(f.code, x0, x1);
                EXPECT_TRUE(o.zf && o.pf && o.cf) << "unordered";
                EXPECT_EQ((o.mxcsr & kIE) != 0, snan || f.signaling)
                    << "MXCSR 0x" << std::hex << o.mxcsr;
            }
        }
    }
}

namespace {

/// Intel SDM Vol. 2 CMPPD/CMPPS Table 3-1, rows 00H-1FH: the result for an
/// unordered pair, and whether the predicate signals #IA on a QNaN.
struct CmpRow {
    const char* name;
    bool unordered_result;
    bool signals_on_qnan;
};
constexpr CmpRow kTable31[32] = {
    {"EQ_OQ", false, false},    {"LT_OS", false, true},
    {"LE_OS", false, true},     {"UNORD_Q", true, false},
    {"NEQ_UQ", true, false},    {"NLT_US", true, true},
    {"NLE_US", true, true},     {"ORD_Q", false, false},
    {"EQ_UQ", true, false},     {"NGE_US", true, true},
    {"NGT_US", true, true},     {"FALSE_OQ", false, false},
    {"NEQ_OQ", false, false},   {"GE_OS", false, true},
    {"GT_OS", false, true},     {"TRUE_UQ", true, false},
    {"EQ_OS", false, true},     {"LT_OQ", false, false},
    {"LE_OQ", false, false},    {"UNORD_S", true, true},
    {"NEQ_US", true, true},     {"NLT_UQ", true, false},
    {"NLE_UQ", true, false},    {"ORD_S", false, true},
    {"EQ_US", true, true},      {"NGE_UQ", true, false},
    {"NGT_UQ", true, false},    {"FALSE_OS", false, true},
    {"NEQ_OS", false, true},    {"GE_OQ", false, false},
    {"GT_OQ", false, false},    {"TRUE_US", true, true},
};

/// Table 3-1's relation for two EQUAL ordered operands, by imm8[3:0]: EQ,
/// LT, LE, UNORD, NEQ, NLT, NLE, ORD, EQ_U, NGE, NGT, FALSE, NEQ_O, GE, GT,
/// TRUE (10H-1FH repeat it).
constexpr bool kEqualOperandsResult[16] = {
    true,  false, true,  false, false, true,  false, true,
    true,  false, true,  false, false, true,  false, true,
};

struct CmpForm {
    const char* name;
    std::vector<u8> prefix;  // the bytes before imm8: op xmm0, xmm1
    bool dbl;
    u32 predicates;  // 8 (legacy) or 32 (VEX)
};

/// Every CMP form with register operands, xmm0 against xmm1. VEX: C5,
/// then R=1 (inverted), vvvv = xmm0 (inverted 1111), L = 0 and pp.
const std::vector<CmpForm>& cmp_forms() {
    static const std::vector<CmpForm> forms = {
        {"cmpps", {0x0F, 0xC2, 0xC1}, false, 8},
        {"cmpss", {0xF3, 0x0F, 0xC2, 0xC1}, false, 8},
        {"cmppd", {0x66, 0x0F, 0xC2, 0xC1}, true, 8},
        {"cmpsd", {0xF2, 0x0F, 0xC2, 0xC1}, true, 8},
        {"vcmpps", {0xC5, 0xF8, 0xC2, 0xC1}, false, 32},  // VEX.128.0F
        {"vcmppd", {0xC5, 0xF9, 0xC2, 0xC1}, true, 32},   // VEX.128.66.0F
        {"vcmpss", {0xC5, 0xFA, 0xC2, 0xC1}, false, 32},  // VEX.LIG.F3.0F
        {"vcmpsd", {0xC5, 0xFB, 0xC2, 0xC1}, true, 32},   // VEX.LIG.F2.0F
    };
    return forms;
}

}  // namespace

// CMPPS/CMPSS/CMPPD/CMPSD with a NaN in lane 0 (the other lanes ordered):
// the lane-0 mask is Table 3-1's Unordered column, and MXCSR.IE is set for an
// SNaN always and for a QNaN exactly when the predicate signals. Legacy
// encodings take imm8[2:0] (predicates 0-7); VEX takes imm8[4:0] (0-31).
TEST_P(SseNanInvalid, CmpFollowsTable31) {
    for (const CmpForm& f : cmp_forms()) {
        SCOPED_TRACE(f.name);
        for (u32 pred = 0; pred < f.predicates; ++pred) {
            SCOPED_TRACE(kTable31[pred].name);
            for (const bool snan : {false, true}) {
                SCOPED_TRACE(snan ? "SNaN" : "QNaN");
                const u64 nan = f.dbl ? (snan ? kSnan64 : kQnan64)
                                      : (snan ? kSnan32 : kQnan32);
                for (const bool nan_in_src1 : {false, true}) {
                    SCOPED_TRACE(nan_in_src1 ? "NaN in SRC1" : "NaN in SRC2");
                    std::vector<u8> code = f.prefix;
                    code.push_back(static_cast<u8>(pred));
                    const XmmReg one = f.dbl ? pd(kOne64) : ps(kOne32);
                    const XmmReg n = f.dbl ? pd(nan) : ps(static_cast<u32>(nan));
                    const Out o = nan_in_src1 ? run(code, n, one) : run(code, one, n);
                    const u64 lane0 = f.dbl ? o.xmm0.u64_[0] : o.xmm0.u32_[0];
                    const u64 all_ones = f.dbl ? ~u64{0} : u64{0xFFFFFFFFu};
                    EXPECT_EQ(lane0, kTable31[pred].unordered_result ? all_ones : 0)
                        << "lane-0 mask";
                    EXPECT_EQ((o.mxcsr & kIE) != 0,
                              snan || kTable31[pred].signals_on_qnan)
                        << "MXCSR 0x" << std::hex << o.mxcsr;
                }
            }
        }
    }
}

// Ordered operands: Table 3-1's relation for SRC1 < SRC2, SRC1 == SRC2
// (including -0 against +0) and SRC1 > SRC2, by imm8[3:0], and no invalid
// operation for any predicate (Table 3-1 signals only on a NaN operand).
TEST_P(SseNanInvalid, CmpOrderedOperandsFollowTheRelation) {
    // {less, equal, greater} result per imm8[3:0]: EQ, LT, LE, UNORD, NEQ,
    // NLT, NLE, ORD, EQ_U, NGE, NGT, FALSE, NEQ_O, GE, GT, TRUE.
    constexpr bool kRelation[16][3] = {
        {false, true, false}, {true, false, false}, {true, true, false},
        {false, false, false}, {true, false, true}, {false, true, true},
        {false, false, true}, {true, true, true},   {false, true, false},
        {true, false, false}, {true, true, false},  {false, false, false},
        {true, false, true},  {false, true, true},  {false, false, true},
        {true, true, true},
    };
    struct Pair {
        const char* what;
        u32 a32, b32;
        u64 a64, b64;
        int relation;  // 0 less, 1 equal, 2 greater
    };
    constexpr u32 kTwo32 = 0x40000000u;
    constexpr u64 kTwo64 = 0x4000000000000000ull;
    constexpr u32 kNegZero32 = 0x80000000u;
    constexpr u64 kNegZero64 = 0x8000000000000000ull;
    const Pair pairs[] = {
        {"1 < 2", kOne32, kTwo32, kOne64, kTwo64, 0},
        {"1 == 1", kOne32, kOne32, kOne64, kOne64, 1},
        {"-0 == +0", kNegZero32, kZero32, kNegZero64, kZero64, 1},
        {"2 > 1", kTwo32, kOne32, kTwo64, kOne64, 2},
        {"inf > 1", kInf32, kOne32, kInf64, kOne64, 2},
    };
    for (const CmpForm& f : cmp_forms()) {
        SCOPED_TRACE(f.name);
        for (u32 pred = 0; pred < f.predicates; ++pred) {
            SCOPED_TRACE(kTable31[pred].name);
            for (const Pair& p : pairs) {
                SCOPED_TRACE(p.what);
                std::vector<u8> code = f.prefix;
                code.push_back(static_cast<u8>(pred));
                const XmmReg x0 = f.dbl ? pd(p.a64) : ps(p.a32);
                const XmmReg x1 = f.dbl ? pd(p.b64) : ps(p.b32);
                const Out o = run(code, x0, x1);
                const u64 lane0 = f.dbl ? o.xmm0.u64_[0] : o.xmm0.u32_[0];
                const u64 all_ones = f.dbl ? ~u64{0} : u64{0xFFFFFFFFu};
                EXPECT_EQ(lane0, kRelation[pred & 15][p.relation] ? all_ones : 0)
                    << "lane-0 mask";
                EXPECT_EQ(o.mxcsr & kIE, 0u) << "MXCSR 0x" << std::hex << o.mxcsr;
            }
        }
    }
}

// CMPSS/CMPSD compare element 0 alone (Intel SDM Vol. 2: "CMP(SRC1[31:0],
// SRC2[31:0])"), so a NaN in the upper elements of either operand is not
// compared and signals nothing, for every predicate. The upper elements are
// SRC1's, unchanged.
TEST_P(SseNanInvalid, ScalarCmpIgnoresTheUpperElements) {
    for (const CmpForm& f : cmp_forms()) {
        const std::string name = f.name;
        const bool scalar = name == "cmpss" || name == "cmpsd" ||
                            name == "vcmpss" || name == "vcmpsd";
        if (!scalar) continue;
        SCOPED_TRACE(f.name);
        for (u32 pred = 0; pred < f.predicates; ++pred) {
            SCOPED_TRACE(kTable31[pred].name);
            for (const bool snan : {false, true}) {
                SCOPED_TRACE(snan ? "SNaN upper elements" : "QNaN upper elements");
                std::vector<u8> code = f.prefix;
                code.push_back(static_cast<u8>(pred));
                XmmReg x0, x1;
                if (f.dbl) {
                    x0.u64_[0] = kOne64;
                    x0.u64_[1] = snan ? kSnan64 : kQnan64;
                    x1.u64_[0] = kOne64;
                    x1.u64_[1] = snan ? kSnan64 : kQnan64b;
                } else {
                    x0.u32_[0] = kOne32;
                    x1.u32_[0] = kOne32;
                    for (int i = 1; i < 4; ++i) {
                        x0.u32_[i] = snan ? kSnan32 : kQnan32;
                        x1.u32_[i] = snan ? kSnan32 : kQnan32b;
                    }
                }
                const Out o = run(code, x0, x1);
                const u64 lane0 = f.dbl ? o.xmm0.u64_[0] : o.xmm0.u32_[0];
                const u64 all_ones = f.dbl ? ~u64{0} : u64{0xFFFFFFFFu};
                EXPECT_EQ(lane0, kEqualOperandsResult[pred & 15] ? all_ones : 0)
                    << "element-0 mask";
                if (f.dbl) {
                    EXPECT_EQ(o.xmm0.u64_[1], x0.u64_[1]) << "DEST[127:64]";
                } else {
                    for (int i = 1; i < 4; ++i) {
                        EXPECT_EQ(o.xmm0.u32_[i], x0.u32_[i]) << "element " << i;
                    }
                }
                EXPECT_EQ(o.mxcsr & kIE, 0u) << "MXCSR 0x" << std::hex << o.mxcsr;
            }
        }
    }
}

// VEX.256 VCMPPS/VCMPPD compare all eight (four) elements: a NaN only in
// YMM1[255:128] gives that element Table 3-1's unordered result and signals
// exactly as it would in the low half.
TEST_P(SseNanInvalid, Vex256CmpSignalsForTheHighHalf) {
    struct Form {
        const char* name;
        std::vector<u8> prefix;
        bool dbl;
    };
    // C5, R=1, vvvv = ymm0, L = 1, pp: VEX.256.0F / VEX.256.66.0F C2 /r ib.
    const Form forms[] = {
        {"vcmpps ymm", {0xC5, 0xFC, 0xC2, 0xC1}, false},
        {"vcmppd ymm", {0xC5, 0xFD, 0xC2, 0xC1}, true},
    };
    for (const Form& f : forms) {
        SCOPED_TRACE(f.name);
        for (u32 pred = 0; pred < 32; ++pred) {
            SCOPED_TRACE(kTable31[pred].name);
            for (const bool snan : {false, true}) {
                SCOPED_TRACE(snan ? "SNaN" : "QNaN");
                const u64 nan = f.dbl ? (snan ? kSnan64 : kQnan64)
                                      : (snan ? kSnan32 : kQnan32);
                std::vector<u8> code = f.prefix;
                code.push_back(static_cast<u8>(pred));
                const XmmReg one = f.dbl ? pd(kOne64) : ps(kOne32);
                const XmmReg high1 = f.dbl ? pd(nan) : ps(static_cast<u32>(nan));
                const Out o = run(code, one, one, one, high1);
                const u64 high0 = f.dbl ? o.ymm0_high.u64_[0] : o.ymm0_high.u32_[0];
                const u64 low0 = f.dbl ? o.xmm0.u64_[0] : o.xmm0.u32_[0];
                const u64 all_ones = f.dbl ? ~u64{0} : u64{0xFFFFFFFFu};
                EXPECT_EQ(high0, kTable31[pred].unordered_result ? all_ones : 0)
                    << "element in YMM[255:128]";
                EXPECT_EQ(low0, kEqualOperandsResult[pred & 15] ? all_ones : 0)
                    << "element 0";
                EXPECT_EQ((o.mxcsr & kIE) != 0,
                          snan || kTable31[pred].signals_on_qnan)
                    << "MXCSR 0x" << std::hex << o.mxcsr;
            }
        }
    }
}

// MINPS/MAXPS/MINSS/MAXSD signal invalid for any NaN operand, QNaN included,
// and return the second operand.
TEST_P(SseNanInvalid, MinMaxSignalOnAnyNaNAndReturnTheSource) {
    struct Form {
        const char* name;
        std::vector<u8> code;
        bool dbl;
    };
    const Form forms[] = {
        {"minps", {0x0F, 0x5D, 0xC1}, false},
        {"maxps", {0x0F, 0x5F, 0xC1}, false},
        {"minss", {0xF3, 0x0F, 0x5D, 0xC1}, false},
        {"maxsd", {0xF2, 0x0F, 0x5F, 0xC1}, true},
    };
    for (const Form& f : forms) {
        SCOPED_TRACE(f.name);
        for (const bool snan : {false, true}) {
            SCOPED_TRACE(snan ? "SNaN" : "QNaN");
            const u64 nan = f.dbl ? (snan ? kSnan64 : kQnan64)
                                  : (snan ? kSnan32 : kQnan32);
            const u64 one = f.dbl ? kOne64 : kOne32;
            const XmmReg x0 = f.dbl ? pd(nan) : ps(static_cast<u32>(nan));
            const XmmReg x1 = f.dbl ? pd(one) : ps(static_cast<u32>(one));
            const Out o = run(f.code, x0, x1);
            const u64 lane0 = f.dbl ? o.xmm0.u64_[0] : o.xmm0.u32_[0];
            EXPECT_EQ(lane0, one) << "the second operand";
            EXPECT_NE(o.mxcsr & kIE, 0u) << "MXCSR 0x" << std::hex << o.mxcsr;
        }
    }
}

INSTANTIATE_TEST_SUITE_P(
    Engines, SseNanInvalid,
    ::testing::Values(Engine::Interpreter, Engine::Jit),
    [](const ::testing::TestParamInfo<Engine>& info) {
        return std::string(info.param == Engine::Jit ? "Tier1" : "Interpreter");
    });
