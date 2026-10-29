// Executes fxc bytecode from the area-09a ALU differential on Metal and
// compares with the same shaders on Windows (WARP).
//
// The shaders are fxc's (and, for opcodes fxc never emits, fxc output patched
// by tools/oracle_probe/d3d11_a09a_dxbc_patch.py and checked with fxc
// /dumpbin); a09a_alu_oracle.h holds them, the input words and WARP's output
// (tools/oracle_probe/d3d11_a09a_gen_test_header.py). Each runs under the math
// mode production picks for it (d3d11_shader_translate.cpp
// math_mode_for_dxbc), which for fxc's default refactoringAllowed is Metal's
// Relaxed -- the mode that folded fxc's isnan() (`ne x, x`) to false.

#include <gtest/gtest.h>

#include "graphics/contracts/backend_device.hpp"
#include "graphics/metal/metal_device.hpp"
#include "graphics/shaders/dxbc_parser.hpp"
#include "graphics/shaders/dxbc_to_spirv.hpp"
#include "graphics/shaders/spirv_to_msl.hpp"
#include "a09a_alu_oracle.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

using namespace vitreo;
using namespace vitreo::graphics;

namespace {

bool is_nan32(std::uint32_t u) {
    return (u & 0x7F800000u) == 0x7F800000u && (u & 0x007FFFFFu) != 0u;
}

bool is_half_nan(std::uint32_t u) {
    return (u >> 16) == 0u && (u & 0x7C00u) == 0x7C00u && (u & 0x3FFu) != 0u;
}

float as_float(std::uint32_t u) {
    float f = 0.0f;
    std::memcpy(&f, &u, sizeof(f));
    return f;
}

// Distance in units in the last place between two finite floats of the same
// sign (the correctly rounded WARP result is the reference).
std::uint32_t ulp_distance(std::uint32_t a, std::uint32_t b) {
    return a > b ? a - b : b - a;
}

const A09aOracleShader& shader(const char* name) {
    for (const auto& entry : a09a_oracle_shaders) {
        if (std::string(entry.name) == name) return entry;
    }
    ADD_FAILURE() << "no oracle shader " << name;
    return a09a_oracle_shaders[0];
}

// Translate, compile under the production math mode, dispatch with the input
// words at t0 and a 0xCD-filled u0, and return the u0 words.
std::vector<std::uint32_t> run(const A09aOracleShader& entry) {
    auto& device = MetalDevice::instance();
    const auto container = parse_dxbc(entry.dxbc, entry.dxbc_size);
    EXPECT_TRUE(container.valid()) << entry.name;
    const auto spirv = translate_dxbc_to_spirv(container);
    EXPECT_TRUE(spirv.valid()) << entry.name << ": " << spirv.failure;
    if (!spirv.valid()) return {};
    MslCompileOptions flat{};
    flat.argument_buffers = false;
    const auto msl = compile_spirv_to_msl(
        spirv.spirv.data(), spirv.spirv.size(), ShaderStage::Compute, flat);
    EXPECT_TRUE(msl.valid()) << entry.name;
    if (!msl.valid()) return {};
    const auto contract = dxbc_math_contract(container);
    BackendComputePipelineDesc pd{};
    pd.compute_entry_point = "main0";
    pd.compute_shader_msl = msl.msl_text();
    pd.math_mode = !contract.refactoring_allowed || contract.uses_precise
                       ? BackendMathMode::Safe
                       : BackendMathMode::Relaxed;
    pd.cache_key = std::hash<std::string>{}(std::string("a09a:") + entry.name);
    auto pipeline = device.create_compute_pipeline(pd);
    EXPECT_TRUE(pipeline && pipeline->valid()) << msl.msl_text();
    if (!pipeline || !pipeline->valid()) return {};

    BackendBufferDesc in_desc{};
    in_desc.byte_width = sizeof(a09a_input);
    in_desc.shader_resource = true;
    auto input = device.create_buffer(in_desc, a09a_input);
    BackendBufferDesc out_desc{};
    out_desc.byte_width = entry.words * 4u;
    out_desc.unordered_access = true;
    out_desc.shader_resource = true;
    const std::vector<std::uint8_t> fill(out_desc.byte_width, 0xCDu);
    auto output = device.create_buffer(out_desc, fill.data());
    EXPECT_TRUE(input && output);
    if (!input || !output) return {};

    BackendDispatchCall call{};
    call.pipeline = pipeline.get();
    call.group_count_x = entry.thread_groups;
    call.group_count_y = call.group_count_z = 1;
    call.threads_per_group_x = 64;
    call.threads_per_group_y = call.threads_per_group_z = 1;
    call.buffers[BACKEND_SRV_BUFFER_BASE] = input.get();
    call.buffers[BACKEND_UAV_BUFFER_BASE] = output.get();
    EXPECT_TRUE(device.dispatch(call));
    device.wait_idle();
    std::vector<std::uint32_t> words(entry.words);
    EXPECT_TRUE(output->read(0, words.data(), out_desc.byte_width));
    return words;
}

enum class Rule {
    Exact,      // bit for bit
    Float,      // bit for bit, or both float NaN (3.1.3.2: NaN bits are free)
    Half,       // bit for bit, or both a half NaN in the low 16 bits
    Ulp1,       // within 1 ulp of WARP (sqrt: "Precision is 1 ulp")
    NaNWhenNaN, // only: a NaN on WARP is a NaN here (sin/cos of INF/NaN)
};

void expect_columns(const char* name, unsigned columns,
                    const std::vector<std::pair<unsigned, Rule>>& checks) {
    auto& device = MetalDevice::instance();
    if (!device.available()) GTEST_SKIP() << "no Metal device";
    const auto& entry = shader(name);
    const auto got = run(entry);
    ASSERT_EQ(got.size(), entry.words) << name;
    const unsigned threads = entry.words / columns;
    for (unsigned t = 0; t < threads; ++t) {
        for (const auto& [column, rule] : checks) {
            const std::uint32_t want = entry.expected[t * columns + column];
            const std::uint32_t have = got[t * columns + column];
            bool ok = want == have;
            if (!ok && rule == Rule::Float) {
                ok = is_nan32(want) && is_nan32(have);
            } else if (!ok && rule == Rule::Half) {
                ok = is_half_nan(want) && is_half_nan(have);
            } else if (!ok && rule == Rule::Ulp1) {
                ok = (is_nan32(want) && is_nan32(have)) ||
                     ((want >> 31) == (have >> 31) &&
                      ulp_distance(want, have) <= 1u);
            } else if (rule == Rule::NaNWhenNaN) {
                ok = !is_nan32(want) || is_nan32(have);
            }
            EXPECT_TRUE(ok) << name << " thread " << t << " column " << column
                            << ": WARP 0x" << std::hex << want << " ("
                            << as_float(want) << "), Metal 0x" << have << " ("
                            << as_float(have) << ")";
        }
    }
}

} // namespace

// D3D11.3 3.1.3.1: `eq`/`ge` with a NaN operand are false and `ne` is true.
// fxc writes isnan(x) as `ne r, x, x` and !isnan(x) as `eq r, x, x`; Metal's
// Relaxed mode folded both, and the (x, x) compares, to constants. Columns 9
// and 10 are x*1.0 and x*-1.0, which flush a denormal (3.1.3.2).
TEST(ShaderAluGpu, SelfComparesSeeNaNUnderRelaxedMath) {
    expect_columns("fself", 16,
                   {{0, Rule::Exact}, {1, Rule::Exact}, {3, Rule::Exact},
                    {11, Rule::Exact}, {9, Rule::Float}, {10, Rule::Float}});
}

// frc (22.10.7): [0, 1), +-INF -> NaN; sqrt (22.10.21) within 1 ulp; sin and
// cos of INF/NaN are NaN (22.10.20); f32tof16 rounds toward zero and clamps to
// the largest half (3.2.2); isnan(x) honoured; mov -x and |x|*1.0 flush a
// denormal (3.1.3.2).
TEST(ShaderAluGpu, UnaryFloatSpecialValuesMatchWinoracle) {
    expect_columns("funary", 32,
                   {{0, Rule::Float},
                    {24, Rule::Float},
                    {7, Rule::Ulp1},
                    {31, Rule::Ulp1},
                    {10, Rule::NaNWhenNaN},
                    {11, Rule::NaNWhenNaN},
                    {26, Rule::NaNWhenNaN},
                    {27, Rule::NaNWhenNaN},
                    {12, Rule::Float},
                    {13, Rule::Float},
                    {15, Rule::Float},
                    {18, Rule::Half},
                    {28, Rule::Half},
                    {21, Rule::Exact}});
}

// sqrt over the whole exponent range (the accuracy set, column 2).
TEST(ShaderAluGpu, SqrtIsWithinOneUlp) {
    expect_columns("acc", 8, {{2, Rule::Ulp1}});
}

// f32tof16 with fxc's `-x` / `-|x|` source modifiers (22.13.2 `[-]src`),
// and f16tof32 with a `-` modifier, which WARP applies to the float result
// (p_f16neg, patched; halfneg is fxc's own ineg form).
TEST(ShaderAluGpu, HalfConversionsTakeSourceModifiers) {
    expect_columns("halfneg", 4,
                   {{0, Rule::Half}, {1, Rule::Half}, {2, Rule::Float},
                    {3, Rule::Float}});
    expect_columns("p_f16neg", 4,
                   {{0, Rule::Half}, {1, Rule::Half}, {2, Rule::Float},
                    {3, Rule::Float}});
}

// uaddc / usubb (22.12.8 / 22.12.14): sum or difference plus the carry or
// borrow as 0/1, with dest0 aliasing src0.
TEST(ShaderAluGpu, AddWithCarryAndSubtractWithBorrow) {
    expect_columns("p_uaddc", 2, {{0, Rule::Exact}, {1, Rule::Exact}});
    expect_columns("p_usubb", 2, {{0, Rule::Exact}, {1, Rule::Exact}});
}

// Subroutines (22.7.10-17): `call` inside a switch (fxc's [call] form), at
// the top level, `callc_nz`, and `retc` / `if_nz ret endif` inside the body.
TEST(ShaderAluGpu, SubroutinesCallAnywhereAndReturnFromTheBody) {
    for (const char* name :
         {"call", "p_topcall", "p_callc", "p_subretc", "p_subifret"}) {
        expect_columns(name, 1, {{0, Rule::Exact}});
    }
}
