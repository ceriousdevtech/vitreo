/// @file test_crt_math.cpp
/// @brief Unit tests for msvcrt math functions (WIN32-083)

#include <gtest/gtest.h>
#include <vitreo/common.hpp>
#include "winapi/msvcrt/math/crt_math.hpp"
#include "winapi/msvcrt/memory/crt_memory.hpp"

#include <cmath>
#include <limits>

using namespace vitreo;
using namespace vitreo::winapi;

class CrtMathTest : public ::testing::Test {};

// =============================================================================
// 1. Sin_Zero
// =============================================================================

TEST_F(CrtMathTest, Sin_Zero) {
    EXPECT_DOUBLE_EQ(CrtMathManager::crt_sin(0.0), 0.0);
}

// =============================================================================
// 2. Sin_PiOver2
// =============================================================================

TEST_F(CrtMathTest, Sin_PiOver2) {
    EXPECT_NEAR(CrtMathManager::crt_sin(M_PI / 2.0), 1.0, 1e-12);
}

// =============================================================================
// 3. Cos_Zero
// =============================================================================

TEST_F(CrtMathTest, Cos_Zero) {
    EXPECT_DOUBLE_EQ(CrtMathManager::crt_cos(0.0), 1.0);
}

// =============================================================================
// 4. Tan_PiOver4
// =============================================================================

TEST_F(CrtMathTest, Tan_PiOver4) {
    EXPECT_NEAR(CrtMathManager::crt_tan(M_PI / 4.0), 1.0, 1e-12);
}

// =============================================================================
// 5. Sqrt_Four
// =============================================================================

TEST_F(CrtMathTest, Sqrt_Four) {
    EXPECT_DOUBLE_EQ(CrtMathManager::crt_sqrt(4.0), 2.0);
}

// =============================================================================
// 6. Sqrt_Two
// =============================================================================

TEST_F(CrtMathTest, Sqrt_Two) {
    EXPECT_NEAR(CrtMathManager::crt_sqrt(2.0), 1.41421356237, 1e-10);
}

// =============================================================================
// 7. Pow_TwoToTen
// =============================================================================

TEST_F(CrtMathTest, Pow_TwoToTen) {
    EXPECT_DOUBLE_EQ(CrtMathManager::crt_pow(2.0, 10.0), 1024.0);
}

// =============================================================================
// 8. Exp_Zero
// =============================================================================

TEST_F(CrtMathTest, Exp_Zero) {
    EXPECT_DOUBLE_EQ(CrtMathManager::crt_exp(0.0), 1.0);
}

// =============================================================================
// 9. Log_One
// =============================================================================

TEST_F(CrtMathTest, Log_One) {
    EXPECT_DOUBLE_EQ(CrtMathManager::crt_log(1.0), 0.0);
}

// =============================================================================
// 10. Log10_Hundred
// =============================================================================

TEST_F(CrtMathTest, Log10_Hundred) {
    EXPECT_DOUBLE_EQ(CrtMathManager::crt_log10(100.0), 2.0);
}

// =============================================================================
// 11. Floor_Positive
// =============================================================================

TEST_F(CrtMathTest, Floor_Positive) {
    EXPECT_DOUBLE_EQ(CrtMathManager::crt_floor(3.7), 3.0);
}

// =============================================================================
// 12. Ceil_Positive
// =============================================================================

TEST_F(CrtMathTest, Ceil_Positive) {
    EXPECT_DOUBLE_EQ(CrtMathManager::crt_ceil(3.2), 4.0);
}

// =============================================================================
// 13. Fabs_Negative
// =============================================================================

TEST_F(CrtMathTest, Fabs_Negative) {
    EXPECT_DOUBLE_EQ(CrtMathManager::crt_fabs(-5.0), 5.0);
}

// =============================================================================
// 14. Fmod_SevenByThree
// =============================================================================

TEST_F(CrtMathTest, Fmod_SevenByThree) {
    EXPECT_NEAR(CrtMathManager::crt_fmod(7.0, 3.0), 1.0, 1e-12);
}

// =============================================================================
// 15. IsNan_DetectsNaN
// =============================================================================

TEST_F(CrtMathTest, IsNan_DetectsNaN) {
    EXPECT_NE(CrtMathManager::crt_isnan(std::numeric_limits<f64>::quiet_NaN()), 0);
    EXPECT_EQ(CrtMathManager::crt_isnan(1.0), 0);
}

// =============================================================================
// 16. Finite_DetectsInfinity
// =============================================================================

TEST_F(CrtMathTest, Finite_DetectsInfinity) {
    EXPECT_EQ(CrtMathManager::crt_finite(std::numeric_limits<f64>::infinity()), 0);
    EXPECT_NE(CrtMathManager::crt_finite(1.0), 0);
}

// =============================================================================
// 17. Hypot_ThreeFour
// =============================================================================

TEST_F(CrtMathTest, Hypot_ThreeFour) {
    EXPECT_DOUBLE_EQ(CrtMathManager::crt_hypot(3.0, 4.0), 5.0);
}

// =============================================================================
// Additional coverage: float variants, atan2, copysign, ldexp, frexp, modf
// =============================================================================

TEST_F(CrtMathTest, Atan2_OneOne) {
    EXPECT_NEAR(CrtMathManager::crt_atan2(1.0, 1.0), M_PI / 4.0, 1e-12);
}

TEST_F(CrtMathTest, Copysign_NegativeSign) {
    EXPECT_DOUBLE_EQ(CrtMathManager::crt_copysign(5.0, -1.0), -5.0);
}

TEST_F(CrtMathTest, Ldexp_Shift) {
    EXPECT_DOUBLE_EQ(CrtMathManager::crt_ldexp(1.0, 10), 1024.0);
}

TEST_F(CrtMathTest, Frexp_Decompose) {
    i32 exp = 0;
    f64 frac = CrtMathManager::crt_frexp(8.0, exp);
    EXPECT_DOUBLE_EQ(frac, 0.5);
    EXPECT_EQ(exp, 4);
}

TEST_F(CrtMathTest, Modf_Split) {
    f64 ipart = 0.0;
    f64 fpart = CrtMathManager::crt_modf(3.75, ipart);
    EXPECT_DOUBLE_EQ(ipart, 3.0);
    EXPECT_DOUBLE_EQ(fpart, 0.75);
}

TEST_F(CrtMathTest, Sinf_Zero) {
    EXPECT_FLOAT_EQ(CrtMathManager::crt_sinf(0.0f), 0.0f);
}

TEST_F(CrtMathTest, Cosf_Zero) {
    EXPECT_FLOAT_EQ(CrtMathManager::crt_cosf(0.0f), 1.0f);
}

TEST_F(CrtMathTest, Sqrtf_Four) {
    EXPECT_FLOAT_EQ(CrtMathManager::crt_sqrtf(4.0f), 2.0f);
}

TEST_F(CrtMathTest, Powf_TwoToThree) {
    EXPECT_FLOAT_EQ(CrtMathManager::crt_powf(2.0f, 3.0f), 8.0f);
}

TEST_F(CrtMathTest, Fabsf_Negative) {
    EXPECT_FLOAT_EQ(CrtMathManager::crt_fabsf(-3.5f), 3.5f);
}

TEST_F(CrtMathTest, Floorf_Positive) {
    EXPECT_FLOAT_EQ(CrtMathManager::crt_floorf(2.9f), 2.0f);
}

TEST_F(CrtMathTest, Ceilf_Positive) {
    EXPECT_FLOAT_EQ(CrtMathManager::crt_ceilf(2.1f), 3.0f);
}

// =============================================================================
// Thunk marshalling — the calling convention, not the arithmetic
//
// Every test above calls a static helper directly, which is why the x64
// argument and return path went unchecked: the thunks read their operands from
// the x86 stack and returned on the x87 stack unconditionally, so on a 64-bit
// guest they took garbage in and left the result somewhere the caller never
// looks. The functions are registered, so nothing reports a missing export --
// the caller simply reads whatever XMM0 happened to hold.
//
// These tests drive the registered thunks through a Marshaler and assert on
// the register the ABI actually designates.
// =============================================================================

#include "winapi/core/marshaler.hpp"
#include "core/cpu/state/cpu_state.hpp"
#include "core/memory/guest_memory.hpp"

#include <algorithm>
#include <cstring>

namespace {

ExportEntry* find_math_entry(DllExports& exports, const char* name) {
    auto entry = std::find_if(
        exports.entries.begin(), exports.entries.end(),
        [name](const ExportEntry& candidate) { return candidate.name == name; });
    return entry == exports.entries.end() ? nullptr : &*entry;
}

double xmm0_as_double(const cpu::CpuState& cpu) {
    double value = 0.0;
    const u64 bits = cpu.sse.xmm[0].u64_[0];
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

float xmm0_as_float(const cpu::CpuState& cpu) {
    float value = 0.0f;
    const u32 bits = cpu.sse.xmm[0].u32_[0];
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

void set_xmm_double(cpu::CpuState& cpu, usize index, double value) {
    u64 bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    cpu.sse.xmm[index].u64_[0] = bits;
    cpu.sse.xmm[index].u64_[1] = 0;
}

void set_xmm_float(cpu::CpuState& cpu, usize index, float value) {
    u32 bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    cpu.sse.xmm[index].u64_[0] = bits;
    cpu.sse.xmm[index].u64_[1] = 0;
}

} // namespace

TEST_F(CrtMathTest, X64DoubleThunksUseXmmRegisters) {
    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(0x10000, 0x10000,
                                  MemoryProtection::ReadWrite).is_ok());
    CrtMathManager manager;
    CrtMemoryManager memory_manager(memory);
    DllExports exports = create_crt_math_thunks(manager, memory_manager);
    cpu::CpuState cpu;
    cpu.init_long_mode();
    Marshaler marshaler{memory};
    marshaler.attach_cpu(cpu);

    // One argument in XMM0, result in XMM0.
    ExportEntry* log_entry = find_math_entry(exports, "log");
    ASSERT_NE(log_entry, nullptr);
    set_xmm_double(cpu, 0, 1.0);
    // Seed a value the correct answer cannot be confused with: if the thunk
    // never wrote XMM0, this is what the assertion would see.
    cpu.sse.xmm[0].u64_[1] = 0xDEADBEEF;
    log_entry->handler(marshaler);
    EXPECT_NEAR(xmm0_as_double(cpu), 0.0, 1e-15);

    set_xmm_double(cpu, 0, M_E);
    log_entry->handler(marshaler);
    EXPECT_NEAR(xmm0_as_double(cpu), 1.0, 1e-12);

    // Two arguments: the second is in XMM1, not the second stack slot.
    ExportEntry* pow_entry = find_math_entry(exports, "pow");
    ASSERT_NE(pow_entry, nullptr);
    set_xmm_double(cpu, 0, 2.0);
    set_xmm_double(cpu, 1, 10.0);
    pow_entry->handler(marshaler);
    EXPECT_DOUBLE_EQ(xmm0_as_double(cpu), 1024.0);

    // Asymmetric operands: a thunk that swapped or aliased its arguments would
    // still pass a pow(2,10)-style check by coincidence.
    ExportEntry* atan2_entry = find_math_entry(exports, "atan2");
    ASSERT_NE(atan2_entry, nullptr);
    set_xmm_double(cpu, 0, 1.0);
    set_xmm_double(cpu, 1, 0.0);
    atan2_entry->handler(marshaler);
    EXPECT_NEAR(xmm0_as_double(cpu), M_PI / 2.0, 1e-12);
}

TEST_F(CrtMathTest, X64FloatThunksReturnFloatWidthInXmm0) {
    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(0x10000, 0x10000,
                                  MemoryProtection::ReadWrite).is_ok());
    CrtMathManager manager;
    CrtMemoryManager memory_manager(memory);
    DllExports exports = create_crt_math_thunks(manager, memory_manager);
    cpu::CpuState cpu;
    cpu.init_long_mode();
    Marshaler marshaler{memory};
    marshaler.attach_cpu(cpu);

    // These four were missing entirely before; the generic stub answered
    // 0xFFFFFFFF in EAX and never touched XMM0.
    struct Case { const char* name; float input; float expected; };
    const Case cases[] = {
        {"logf",   1.0f,    0.0f},
        {"log2f",  8.0f,    3.0f},
        {"truncf", 3.75f,   3.0f},
        {"truncf", -3.75f, -3.0f},
        {"logbf",  8.0f,    3.0f},
        {"sqrtf",  9.0f,    3.0f},
        {"floorf", -2.5f,  -3.0f},
        {"ceilf",  -2.5f,  -2.0f},
    };
    for (const auto& c : cases) {
        ExportEntry* entry = find_math_entry(exports, c.name);
        ASSERT_NE(entry, nullptr) << c.name;
        set_xmm_float(cpu, 0, c.input);
        entry->handler(marshaler);
        EXPECT_FLOAT_EQ(xmm0_as_float(cpu), c.expected) << c.name;
    }

    // A float result must occupy the LOW 32 bits of XMM0. Widening it to a
    // double instead produces a bit pattern the caller reads as a wildly
    // different number rather than as a failure.
    ExportEntry* truncf = find_math_entry(exports, "truncf");
    ASSERT_NE(truncf, nullptr);
    set_xmm_float(cpu, 0, 3.75f);
    truncf->handler(marshaler);
    float as_float = 0.0f;
    const u32 low_bits = cpu.sse.xmm[0].u32_[0];
    std::memcpy(&as_float, &low_bits, sizeof(as_float));
    EXPECT_FLOAT_EQ(as_float, 3.0f);

    // Two float arguments: second in XMM1.
    ExportEntry* fmodf = find_math_entry(exports, "fmodf");
    ASSERT_NE(fmodf, nullptr);
    set_xmm_float(cpu, 0, 7.0f);
    set_xmm_float(cpu, 1, 3.0f);
    fmodf->handler(marshaler);
    EXPECT_FLOAT_EQ(xmm0_as_float(cpu), 1.0f);
}

TEST_F(CrtMathTest, X64MixedDoubleAndPointerArgsUseTheRightSlots) {
    // modf and frexp take a double followed by a pointer. On x86 the pointer
    // is stack slot 2, because the double consumed slots 0 and 1; on x64 the
    // double is in XMM0 and the pointer is in RDX -- integer slot 1. Reading
    // slot 2 there picks up R8, which the caller never set.
    constexpr GuestAddr base = 0x10000;
    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(base, 0x10000,
                                  MemoryProtection::ReadWrite).is_ok());
    CrtMathManager manager;
    CrtMemoryManager memory_manager(memory);
    DllExports exports = create_crt_math_thunks(manager, memory_manager);
    cpu::CpuState cpu;
    cpu.init_long_mode();
    Marshaler marshaler{memory};
    marshaler.attach_cpu(cpu);

    ExportEntry* modf_entry = find_math_entry(exports, "modf");
    ASSERT_NE(modf_entry, nullptr);
    set_xmm_double(cpu, 0, 3.25);
    cpu.gpr.rdx = base;
    cpu.gpr.r8 = 0;  // where a slot-2 read would land
    modf_entry->handler(marshaler);
    EXPECT_NEAR(xmm0_as_double(cpu), 0.25, 1e-15);
    const u64 lo = memory.read_u32(base).value();
    const u64 hi = memory.read_u32(base + 4).value();
    double integral = 0.0;
    const u64 bits = (hi << 32) | lo;
    std::memcpy(&integral, &bits, sizeof(integral));
    EXPECT_DOUBLE_EQ(integral, 3.0);

    ExportEntry* frexp_entry = find_math_entry(exports, "frexp");
    ASSERT_NE(frexp_entry, nullptr);
    set_xmm_double(cpu, 0, 8.0);
    cpu.gpr.rdx = base + 0x40;
    cpu.gpr.r8 = 0;
    frexp_entry->handler(marshaler);
    EXPECT_NEAR(xmm0_as_double(cpu), 0.5, 1e-15);
    EXPECT_EQ(static_cast<i32>(memory.read_u32(base + 0x40).value()), 4);
}

#include "crt_compare_oracle.inc"

TEST_F(CrtMathTest, ComparisonAndPointerClassificationMatchWindowsBothAbis) {
    for (bool x64 : {false, true}) {
        GuestMemory memory;
        ASSERT_TRUE(memory.map_region(0x10000, 0x10000, MemoryProtection::ReadWrite).is_ok());
        CrtMathManager manager;
        CrtMemoryManager memory_manager(memory);
        auto exports = create_crt_math_thunks(manager, memory_manager);
        cpu::CpuState cpu;
        if (x64) cpu.init_long_mode(); else cpu.init_protected_mode();
        cpu.gpr.rsp = 0x18000;
        Marshaler m(memory);
        m.attach_cpu(cpu);
        for (const auto& c : kCrtCompareOracle) {
            SCOPED_TRACE(x64);
            auto args = [&] {
                if (x64) { cpu.sse.xmm[0].u64_[0] = c.a; cpu.sse.xmm[1].u64_[0] = c.b; }
                else { (void)memory.write_u64(0x18004, c.a); (void)memory.write_u64(0x1800c, c.b); }
            };
            for (const char* name : {"_dpcomp", "_ldpcomp"}) {
                auto* entry = find_math_entry(exports, name);
                ASSERT_NE(entry, nullptr);
                cpu.fpu.status_word = 0; cpu.sse.mxcsr = 0x1f80;
                args(); entry->handler(m);
                const u32 flags = x64 ? cpu.sse.mxcsr & 3u : cpu.fpu.status_word & 3u;
                const u32 expected = x64 ? c.cmpflags64 : c.cmpflags32;
                EXPECT_EQ(flags, ((expected & 0x10u) ? 1u : 0u) | ((expected & 0x80000u) ? 2u : 0u));
                EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), c.comparison);
            }
            cpu.fpu.status_word = 0; cpu.sse.mxcsr = 0x1f80;
            args(); find_math_entry(exports, "fmax")->handler(m);
            const u32 flags = x64 ? cpu.sse.mxcsr & 3u : cpu.fpu.status_word & 3u;
            const u32 expected = x64 ? c.maxflags64 : c.maxflags32;
            EXPECT_EQ(flags, ((expected & 0x10u) ? 1u : 0u) | ((expected & 0x80000u) ? 2u : 0u));
            u64 result;
            if (x64) result = cpu.sse.xmm[0].u64_[0];
            else { const double d = cpu.fpu.pop().to_double(); std::memcpy(&result, &d, 8); }
            EXPECT_EQ(result, x64 ? c.maximum64 : c.maximum32);
        }
        for (usize i = 0; i < std::size(kCrtClassOracle); ++i) {
            // First operand advances every ten oracle comparison rows.
            (void)memory.write_u64(0x11000, kCrtCompareOracle[i * std::size(kCrtClassOracle)].a);
            for (const char* name : {"_dtest", "_ldtest"}) {
                if (x64) cpu.gpr.rcx = 0x11000;
                else (void)memory.write_u32(0x18004, 0x11000);
                find_math_entry(exports, name)->handler(m);
                EXPECT_EQ(static_cast<u16>(cpu.gpr.rax), kCrtClassOracle[i]);
                EXPECT_EQ(memory.read_u64(0x11000).value(), kCrtCompareOracle[i * std::size(kCrtClassOracle)].a);
            }
        }
    }
}
