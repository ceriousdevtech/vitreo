// SPDX-License-Identifier: LGPL-2.1-only
// Executes the double-precision differential (D3D11.3 22.14-22.17) on Metal
// and compares every output word with the D3D11.3 answer.
//
// The shaders are fxc's (tools/oracle_probe/d3d11_doubles_shaders.hlsl, and
// fxc output patched for the forms fxc never emits -- _sat on every double
// instruction, |x| / -|x| on a double -- checked with fxc /dumpbin), packed
// with the input words into tools/oracle_probe/d3d11_doubles_shaders.h. The
// answers are double_probe_model.h, which test_soft_double.cpp checks
// against WARP's words (tools/oracle_probe/d3d11_doubles_probe.expected).
// Apple GPUs have no fp64: this is the translator's integer emulation
// (soft_double.hpp) under the math mode production picks for the shader.

#include <gtest/gtest.h>

#include "graphics/contracts/backend_device.hpp"
#include "graphics/metal/metal_device.hpp"
#include "graphics/shaders/dxbc_parser.hpp"
#include "graphics/shaders/dxbc_to_spirv.hpp"
#include "graphics/shaders/spirv_to_msl.hpp"
#include "double_probe_model.h"

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-const-variable"
#include "../oracle_data/d3d11_doubles_shaders.h"
#pragma clang diagnostic pop

#include <cstdint>
#include <string>
#include <vector>

using namespace vitreo;
using namespace vitreo::graphics;

namespace {

const DoubleProbeShader& shader(const char* name) {
    for (const auto& entry : g_dbl_shaders) {
        if (std::string(entry.name) == name) return entry;
    }
    ADD_FAILURE() << "no probe shader " << name;
    return g_dbl_shaders[0];
}

std::vector<std::uint32_t> run(const DoubleProbeShader& entry) {
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
    pd.cache_key = std::hash<std::string>{}(std::string("dbl:") + entry.name);
    auto pipeline = device.create_compute_pipeline(pd);
    EXPECT_TRUE(pipeline && pipeline->valid()) << msl.msl_text();
    if (!pipeline || !pipeline->valid()) return {};

    BackendBufferDesc in_desc{};
    in_desc.byte_width = sizeof(g_dbl_input);
    in_desc.shader_resource = true;
    auto input = device.create_buffer(in_desc, g_dbl_input);
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
    EXPECT_TRUE(device.wait_idle());
    std::vector<std::uint32_t> words(entry.words);
    EXPECT_TRUE(output->read(0, words.data(), out_desc.byte_width));
    return words;
}

void expect_model(const char* name) {
    auto& device = MetalDevice::instance();
    if (!device.available()) GTEST_SKIP() << "no Metal device";
    const auto& entry = shader(name);
    const auto got = run(entry);
    ASSERT_EQ(got.size(), entry.words) << name;
    const double_probe::Model model(g_dbl_input);
    const auto want = model.answer(name);
    ASSERT_EQ(want.size(), got.size()) << name;
    unsigned failures = 0;
    for (unsigned i = 0; i < got.size(); ++i) {
        const unsigned other =
            want[i].kind == double_probe::Kind::DoubleLo ? i + 1
            : want[i].kind == double_probe::Kind::DoubleHi ? i - 1
                                                           : i;
        if (double_probe::matches(got[i], want[i], got[other])) continue;
        if (++failures <= 16) {
            ADD_FAILURE() << name << " word " << i << std::hex << ": want 0x"
                          << want[i].value << (want[i].nan ? " (NaN)" : "")
                          << ", Metal 0x" << got[i];
        }
    }
    EXPECT_EQ(failures, 0u) << name;
}

} // namespace

// dadd, dadd -b, dmul, ddiv, dfma, dmin, dmax, deq/dge/dlt/dne over every
// pair of 64 edge doubles (signed zeros, denormals, DBL_MIN/MAX, INF, NaN
// payloads, conversion boundaries, rounding ties).
TEST(ShaderDoubleGpu, EdgePairsMatchD3D11) { expect_model("dbin"); }

// 2048 random triples: rounding, cancellation, fused cancellation
// fma(a, b, -(a*b)), denormal and overflowing results, drcp.
TEST(ShaderDoubleGpu, RandomTriplesMatchD3D11) { expect_model("drnd"); }

// drcp, dtof, dtoi, dtou, dmov -x and deq x,x over the edges and 1024 values
// around float ties, int limits and reciprocal overflow.
TEST(ShaderDoubleGpu, UnaryAndConversionsMatchD3D11) {
    expect_model("dun");
    expect_model("duni");
}

// ftod (float denormals, NaN payloads), itod, utod.
TEST(ShaderDoubleGpu, ConversionsToDoubleMatchD3D11) {
    expect_model("dconv");
    expect_model("iuni");
}

// Two doubles per register: .xyzw masks, .zwxy swizzles, two-result dtof /
// dlt into .xy / .zw, per-lane dmovc conditions, itod into .xyzw.
TEST(ShaderDoubleGpu, RegisterPairLanesMatchD3D11) { expect_model("dvec"); }

// Every instruction once, then with _sat on each (patched), and a double's
// |x| / -|x| source modifiers (patched).
TEST(ShaderDoubleGpu, ModifiersAndSaturateMatchD3D11) {
    expect_model("dmod");
    expect_model("p_dsat");
    expect_model("p_dabs");
    expect_model("p_dnabs");
}

// Double immediates (d(x, y): 0.1L, 1e300L, a denormal, a float promoted)
// and fxc's own dmov_sat for saturate(x).
TEST(ShaderDoubleGpu, ImmediatesAndSaturateMatchD3D11) {
    expect_model("dimm");
    // A one-component d() (patched): its first DWORD in every component.
    expect_model("p_dimm1");
}

// The vertex and pixel stages: fxc's vs_5_0 / ps_5_0 with ftod, dmul and
// dfma with d() immediates, ddiv, itod, dmin, dadd_sat of a negated source
// and dtof/dtou straight into an output register, translated and compiled
// into a Metal render pipeline.
TEST(ShaderDoubleGpu, GraphicsStagesCompile) {
    auto& device = MetalDevice::instance();
    if (!device.available()) GTEST_SKIP() << "no Metal device";
    const auto msl_of = [](const unsigned char* dxbc, std::size_t size,
                           ShaderStage stage) {
        const auto container = parse_dxbc(dxbc, size);
        EXPECT_TRUE(container.valid());
        const auto spirv = translate_dxbc_to_spirv(container);
        EXPECT_TRUE(spirv.valid()) << spirv.failure;
        if (!spirv.valid()) return std::string{};
        MslCompileOptions flat{};
        flat.argument_buffers = false;
        const auto msl = compile_spirv_to_msl(
            spirv.spirv.data(), spirv.spirv.size(), stage, flat);
        EXPECT_TRUE(msl.valid());
        return msl.valid() ? std::string(msl.msl_text()) : std::string{};
    };
    BackendPipelineDesc pd{};
    pd.vertex_shader_msl = msl_of(g_dbl_vs_dbl_dxbc, sizeof(g_dbl_vs_dbl_dxbc),
                                  ShaderStage::Vertex);
    pd.fragment_shader_msl = msl_of(
        g_dbl_ps_dbl_dxbc, sizeof(g_dbl_ps_dbl_dxbc), ShaderStage::Fragment);
    ASSERT_FALSE(pd.vertex_shader_msl.empty());
    ASSERT_FALSE(pd.fragment_shader_msl.empty());
    EXPECT_NE(pd.vertex_shader_msl.find("vitreoDFma"), std::string::npos);
    EXPECT_NE(pd.fragment_shader_msl.find("vitreoDSat"), std::string::npos);
    pd.vertex_entry_point = "main0";
    pd.fragment_entry_point = "main0";
    pd.vertex_attributes = {{0, 0, BackendVertexFormat::Float4, 0}};
    pd.vertex_stride = pd.vertex_buffer_strides[0] = 16;
    pd.color_format = PixelFormat::RGBA32Float;
    pd.cache_key = std::hash<std::string>{}("dbl:graphics");
    auto pipeline = device.create_pipeline(pd);
    EXPECT_TRUE(pipeline != nullptr && pipeline->valid())
        << pd.vertex_shader_msl << pd.fragment_shader_msl;
}

// dfma over every edge triple of cs_dbin, one value per thread group.
TEST(ShaderDoubleGpu, FusedMultiplyAddIsExact) { expect_model("fmauni"); }
