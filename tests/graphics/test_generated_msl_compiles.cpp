// SPDX-License-Identifier: LGPL-2.1-only
/// @file test_generated_msl_compiles.cpp
/// @brief Compile every generated shader with the real Metal compiler.
///
/// The other shader tests assert on the text a generator produced, which
/// cannot catch a syntax error, an undeclared identifier, or a type mismatch.
/// Those only appear when Metal compiles the source, and at runtime they
/// surface as a pipeline that fails to build — one step removed from the
/// generator that caused it. These tests close that gap by running each
/// generator's output through `GraphicsBackendDevice::create_pipeline`.
///
/// They skip when no Metal device is present rather than failing, so a
/// headless machine still runs the rest of the suite.

#include <gtest/gtest.h>

#include <limits>
#include <cstring>
#include <CoreFoundation/CoreFoundation.h>
#include <thread>
#include <chrono>
#include <mutex>
#include <condition_variable>
#include "graphics/metal/metal_submit_queue.hpp"
#include "graphics/metal/metal_minmax_rebuild.hpp"
#include "graphics/shaders/compile_profile.hpp"

#include "graphics/shaders/ff_vertex_layout.hpp"
#include "graphics/contracts/backend_device.hpp"
#include "graphics/shaders/ff_emulation.hpp"
#include "graphics/shaders/sm3_to_msl.hpp"
#include "graphics/shaders/dxbc_to_spirv.hpp"
#include "graphics/shaders/gs_passthrough.hpp"
#include "graphics/shaders/spirv_to_msl.hpp"
#include "graphics/shaders/dxil_to_dxbc.hpp"
#include "graphics/shaders/dxil_lower_internal.hpp"
#include "../oracle_data/d3d12_sm6/d3d12_sm6_stages_shaders.h"
#include "../oracle_data/d3d12_packed_bytes_shaders.h"
#include <regex>
#include "graphics/metal/metal_device.hpp"

#include <array>
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

using namespace vitreo;
using namespace vitreo::graphics;

TEST(ScalarTempStoreMetal, OverlappingSourcesAndUntouchedNanBitsStayExact) {
    auto& device = MetalDevice::instance();
    if (!device.available()) GTEST_SKIP();
    using namespace vitreo::graphics::dxil_lower;
    // Use the audited token emitter and a dxc-generated version token, with
    // the minor version cleared for ordinary SM5.0 register operands.
    const auto original = dxil_to_dxbc(sm6s_cs_dot4, sizeof(sm6s_cs_dot4));
    ASSERT_TRUE(original.ok);
    const auto parsed = parse_dxbc(original.container.data(), original.container.size());
    ASSERT_TRUE(parsed.valid());
    u32 version = 0;
    std::memcpy(&version, parsed.bytecode.data(), sizeof(version));
    Emitter code;
    code.raw(version & ~0xFu);
    code.raw(0);
    code.inst(kDclTemps, {}, {1});
    code.inst(kDclThreadGroup, {}, {1, 1, 1});
    // OperandToken0 index dimension/type fields are defined by the SDK
    // encoding documented in dxil_lower_internal.hpp; register u0.
    const Operand uav{{(static_cast<u32>(Sm4OperandType::UnorderedAccessView) << 12) |
                       (1u << 20), 0u}};
    code.inst(kDclUavRaw, {&uav});
    Operand uav_store = uav;
    uav_store.tokens[0] |= 2u | (15u << 4); // four components, xyzw mask
    const Operand all = dst_temp(0, 15), input = imm4(0x7FC12345u, 0xFF812345u, 0x80000000u, 0x12345678u);
    const Operand whole = src_temp(0, 0xE4);
    code.inst(static_cast<u32>(Sm4Opcode::MOV), {&all, &input});
    const auto save = [&](u32 offset) {
        const Operand address = imm(offset);
        code.inst(static_cast<u32>(Sm4Opcode::STORE_RAW), {&uav_store, &address, &whole});
    };
    save(0);
    const Operand y = dst_temp(0, 2), reverse = src_temp(0, 3u | (2u << 2) | (1u << 4));
    code.inst(static_cast<u32>(Sm4Opcode::MOV), {&y, &reverse});
    save(16);
    const Operand w = dst_temp(0, 8), x_source = src_temp_scalar(0, 0);
    code.inst(static_cast<u32>(Sm4Opcode::MOV), {&w, &x_source});
    save(32);
    const Operand x = dst_temp(0, 1), new_nan = imm(0x7F8ABCDEu);
    code.inst(static_cast<u32>(Sm4Opcode::MOV), {&x, &new_nan});
    save(48);
    code.inst(static_cast<u32>(Sm4Opcode::RET), {});
    code.mutable_code()[1] = static_cast<u32>(code.size());
    DxbcContainer container{};
    container.stage = ShaderStage::Compute;
    container.shader_model_major = 5;
    container.bytecode.resize(code.size() * sizeof(u32));
    std::memcpy(container.bytecode.data(), code.code().data(), container.bytecode.size());
    const auto spirv = translate_dxbc_to_spirv(container);
    ASSERT_TRUE(spirv.valid()) << spirv.failure;
    const auto msl = compile_spirv_to_msl(spirv.spirv.data(), spirv.spirv.size(), ShaderStage::Compute);
    ASSERT_TRUE(msl.valid());
    const std::string text = msl.msl_text();
    std::smatch match;
    ASSERT_TRUE(std::regex_search(text, match, std::regex(R"(u0\w*\s*\[\[buffer\((\d+)\)\]\])"))) << text;
    BackendComputePipelineDesc pd{};
    pd.compute_entry_point = "main0";
    pd.compute_shader_msl = text;
    auto pipeline = device.create_compute_pipeline(pd);
    ASSERT_TRUE(pipeline && pipeline->valid()) << text;
    std::array<u32, 16> words{};
    BackendBufferDesc bd{};
    bd.byte_width = sizeof(words);
    bd.unordered_access = bd.shader_resource = true;
    auto output = device.create_buffer(bd, words.data());
    ASSERT_TRUE(output);
    BackendDispatchCall call{};
    call.pipeline = pipeline.get();
    call.group_count_x = call.group_count_y = call.group_count_z = 1;
    call.threads_per_group_x = call.threads_per_group_y = call.threads_per_group_z = 1;
    call.buffers[std::stoul(match[1])] = output.get();
    ASSERT_TRUE(device.dispatch(call));
    ASSERT_TRUE(device.wait_idle());
    ASSERT_TRUE(output->read(0, words.data(), sizeof(words)));
    // MOV is bit-preserving; reverse.y reads the OLD z before the y write.
    const std::array<u32, 16> expected{
        0x7FC12345u, 0xFF812345u, 0x80000000u, 0x12345678u,
        0x7FC12345u, 0x80000000u, 0x80000000u, 0x12345678u,
        0x7FC12345u, 0x80000000u, 0x80000000u, 0x7FC12345u,
        0x7F8ABCDEu, 0x80000000u, 0x80000000u, 0x7FC12345u};
    EXPECT_EQ(words, expected);
}

// SM6.4's packed-dot contract is integer byte products plus a wrapping
// accumulator. Execute dxc's existing shader against that integer definition,
// independently of the translator's float32 dot optimization.
TEST(DxilPackedDotMetal, SignedUnsignedBytesAndFullWidthAccumulatorsStayExact) {
    auto& device = MetalDevice::instance();
    if (!device.available() || !device.supports_descriptor_tables()) GTEST_SKIP();
    const auto* shader = &kSm6StageBlobs[0];
    for (const auto& candidate : kSm6StageBlobs) {
        if (std::string(candidate.name) == "cs_dot4") shader = &candidate;
    }
    ASSERT_EQ(std::string(shader->name), "cs_dot4");
    auto dxbc = dxil_to_dxbc(shader->code, shader->size);
    ASSERT_TRUE(dxbc.ok) << dxbc.failure;
    auto container = parse_dxbc(dxbc.container.data(), dxbc.container.size());
    ASSERT_TRUE(container.valid());
    DxbcTranslationOptions options{};
    options.register_remap = {
        {DxbcRegisterClass::ConstantBuffer, 0, 0, 1, 0, 0},
        {DxbcRegisterClass::UnorderedAccess, 0, 0, 1, 3, 0}};
    options.view_size_buffer.enabled = options.view_size_buffer.uav = true;
    options.view_size_buffer.uav_binding = 1;
    const auto spirv = translate_dxbc_to_spirv(container, options);
    ASSERT_TRUE(spirv.valid()) << spirv.failure;
    MslCompileOptions msl{};
    msl.argument_buffers = true;
    msl.argument_buffer_sets[0] = {2, BACKEND_DESCRIPTOR_TABLE_BUFFER_BASE, MslDescriptorClass::Buffer};
    msl.argument_buffer_sets[3] = {1, BACKEND_DESCRIPTOR_TABLE_BUFFER_BASE + 3, MslDescriptorClass::Buffer};
    const auto compiled = compile_spirv_to_msl(spirv.spirv.data(), spirv.spirv.size(), ShaderStage::Compute, msl);
    ASSERT_TRUE(compiled.valid());
    BackendComputePipelineDesc pd{};
    pd.compute_shader_msl = compiled.msl_text();
    pd.compute_entry_point = "main0";
    auto pipeline = device.create_compute_pipeline(pd);
    ASSERT_TRUE(pipeline && pipeline->valid()) << pd.compute_shader_msl;
    constexpr u32 threads = 16384, word_count = threads * 4;
    const std::array<u32, 4> constants{0xFFFFFFF7u, 0xFFFFFFFFu, 13u, 0x80000001u};
    const auto buffer = [&](const void* bytes, usize size) {
        BackendBufferDesc desc{};
        desc.byte_width = size;
        desc.shader_resource = desc.constant_buffer = desc.unordered_access = true;
        return device.create_buffer(desc, bytes);
    };
    auto cb = buffer(constants.data(), sizeof(constants));
    auto sizes = buffer(&word_count, sizeof(word_count));
    std::vector<u32> words(word_count, 0xDEADBEEFu);
    auto output = buffer(words.data(), words.size() * sizeof(u32));
    auto cbvs = device.create_descriptor_table({2, BackendDescriptorClass::Buffer});
    auto uavs = device.create_descriptor_table({1, BackendDescriptorClass::Buffer});
    ASSERT_TRUE(cb && sizes && output && cbvs && uavs);
    ASSERT_TRUE(cbvs->set_buffer(0, cb.get(), 0));
    ASSERT_TRUE(cbvs->set_buffer(1, sizes.get(), 0));
    ASSERT_TRUE(uavs->set_buffer(0, output.get(), 0));
    BackendDispatchCall call{};
    call.pipeline = pipeline.get();
    call.group_count_x = threads / 64;
    call.group_count_y = call.group_count_z = 1;
    call.threads_per_group_x = 64;
    call.threads_per_group_y = call.threads_per_group_z = 1;
    call.descriptor_tables[0] = cbvs.get();
    call.descriptor_tables[3] = uavs.get();
    ASSERT_TRUE(device.dispatch(call));
    ASSERT_TRUE(device.wait_idle());
    ASSERT_TRUE(output->read(0, words.data(), words.size() * sizeof(u32)));
    const auto integer_dot = [](u32 a, u32 b, u32 acc, bool signed_bytes) {
        i64 sum = 0;
        for (u32 shift = 0; shift < 32; shift += 8) {
            i64 x = (a >> shift) & 255u, y = (b >> shift) & 255u;
            if (signed_bytes) { if (x >= 128) x -= 256; if (y >= 128) y -= 256; }
            sum += x * y;
        }
        return acc + static_cast<u32>(sum);
    };
    for (u32 t = 0; t < threads; ++t) {
        const u32 a = t * 0x9E3779B1u ^ (t << 7);
        const u32 b = a * 0x85EBCA6Bu + constants[1];
        EXPECT_EQ(words[t * 4], integer_dot(a, b, t * 1000u, false)) << t;
        EXPECT_EQ(words[t * 4 + 1], integer_dot(a, b, t - 32u, true)) << t;
        EXPECT_EQ(words[t * 4 + 2], integer_dot(b, b, constants[0], false)) << t;
        EXPECT_EQ(words[t * 4 + 3], integer_dot(a, a, 0u - constants[3], true)) << t;
    }
}

namespace {

void execute_byte_fixture(const unsigned char* code, usize size,
                          const std::vector<u32>& input, std::vector<u32>& output,
                          u32 groups, u32 threads_per_group) {
    auto& device = MetalDevice::instance();
    const auto lowered = dxil_to_dxbc(code, size);
    ASSERT_TRUE(lowered.ok) << lowered.failure;
    const auto container = parse_dxbc(lowered.container.data(), lowered.container.size());
    ASSERT_TRUE(container.valid());
    DxbcTranslationOptions options{};
    options.register_remap = {{DxbcRegisterClass::ShaderResource, 0, 0, 1, 1, 0},
                              {DxbcRegisterClass::UnorderedAccess, 0, 0, 1, 3, 0}};
    options.view_size_buffer.enabled = options.view_size_buffer.srv = options.view_size_buffer.uav = true;
    options.view_size_buffer.srv_binding = 0;
    options.view_size_buffer.uav_binding = 1;
    const auto spirv = translate_dxbc_to_spirv(container, options);
    ASSERT_TRUE(spirv.valid()) << spirv.failure;
    MslCompileOptions msl{};
    msl.argument_buffers = true;
    msl.argument_buffer_sets[0] = {2, BACKEND_DESCRIPTOR_TABLE_BUFFER_BASE, MslDescriptorClass::Buffer};
    msl.argument_buffer_sets[1] = {1, BACKEND_DESCRIPTOR_TABLE_BUFFER_BASE + 1, MslDescriptorClass::Buffer};
    msl.argument_buffer_sets[3] = {1, BACKEND_DESCRIPTOR_TABLE_BUFFER_BASE + 3, MslDescriptorClass::Buffer};
    const auto compiled = compile_spirv_to_msl(spirv.spirv.data(), spirv.spirv.size(), ShaderStage::Compute, msl);
    ASSERT_TRUE(compiled.valid());
    BackendComputePipelineDesc pd{};
    pd.compute_shader_msl = compiled.msl_text();
    pd.compute_entry_point = "main0";
    auto pipeline = device.create_compute_pipeline(pd);
    ASSERT_TRUE(pipeline && pipeline->valid()) << pd.compute_shader_msl;
    const auto buffer = [&](const void* data, usize bytes) {
        BackendBufferDesc desc{};
        desc.byte_width = bytes;
        desc.shader_resource = desc.unordered_access = true;
        return device.create_buffer(desc, data);
    };
    const u32 in_words = static_cast<u32>(input.size()), out_words = static_cast<u32>(output.size());
    auto source = buffer(input.data(), input.size() * sizeof(u32));
    auto target = buffer(output.data(), output.size() * sizeof(u32));
    auto in_size = buffer(&in_words, sizeof(in_words)), out_size = buffer(&out_words, sizeof(out_words));
    auto limits = device.create_descriptor_table({2, BackendDescriptorClass::Buffer});
    auto srvs = device.create_descriptor_table({1, BackendDescriptorClass::Buffer});
    auto uavs = device.create_descriptor_table({1, BackendDescriptorClass::Buffer});
    ASSERT_TRUE(source && target && in_size && out_size && limits && srvs && uavs);
    ASSERT_TRUE(limits->set_buffer(0, in_size.get(), 0));
    ASSERT_TRUE(limits->set_buffer(1, out_size.get(), 0));
    ASSERT_TRUE(srvs->set_buffer(0, source.get(), 0));
    ASSERT_TRUE(uavs->set_buffer(0, target.get(), 0));
    BackendDispatchCall call{};
    call.pipeline = pipeline.get();
    call.group_count_x = groups;
    call.group_count_y = call.group_count_z = 1;
    call.threads_per_group_x = threads_per_group;
    call.threads_per_group_y = call.threads_per_group_z = 1;
    call.descriptor_tables[0] = limits.get();
    call.descriptor_tables[1] = srvs.get();
    call.descriptor_tables[3] = uavs.get();
    ASSERT_TRUE(device.dispatch(call));
    ASSERT_TRUE(device.wait_idle());
    ASSERT_TRUE(target->read(0, output.data(), output.size() * sizeof(u32)));
}

// Independent integer implementation of the Microsoft pack/unpack contract.
u32 pack_bytes(const std::array<i64, 4>& values, u32 mode) {
    u32 word = 0;
    for (u32 k = 0; k < 4; ++k) {
        i64 v = values[k];
        if (mode != 0u) v = std::clamp(v, mode == 1u ? i64{0} : i64{-128}, mode == 1u ? i64{255} : i64{127});
        word |= (static_cast<u32>(v) & 255u) << (8 * k);
    }
    return word;
}

TEST(PackedBytesMetal, Native16AllPackModesAndBothUnpacksStayExact) {
    if (!MetalDevice::instance().available()) GTEST_SKIP();
    constexpr u32 threads = 1024;
    std::vector<u32> input(threads * 20), output(threads * 12, 0xDEADBEEFu);
    constexpr std::array<i32, 12> edges{-32768, -257, -129, -128, -1, 0, 1, 127, 128, 255, 256, 32767};
    for (u32 t = 0; t < threads; ++t) {
        for (u32 k = 0; k < 16; ++k)
            input[t * 20 + k] = (t & 1) ? (t * 0x9E3779B1u + k * 0x85EBCA6Bu)
                                                   : static_cast<u32>(edges[(t / 2 + k) % edges.size()]);
        input[t * 20 + 16] = t * 0x9E3779B1u;
    }
    execute_byte_fixture(kPackedBytesNative16, sizeof(kPackedBytesNative16), input, output, threads, 1);
    ASSERT_FALSE(HasFatalFailure());
    for (u32 t = 0; t < threads; ++t) {
        for (u32 p = 0; p < 4; ++p) {
            std::array<i64, 4> v{};
            for (u32 k = 0; k < 4; ++k) {
                const u32 bits = input[t * 20 + p * 4 + k] & 0xFFFFu;
                v[k] = bits >= 32768u ? i64{bits} - 65536 : bits;
            }
            EXPECT_EQ(output[t * 12 + p], pack_bytes(v, p < 2 ? 0u : p - 1u)) << t << ':' << p;
        }
        for (u32 k = 0; k < 4; ++k) {
            const u32 byte = (input[t * 20 + 16] >> (k * 8)) & 255u;
            EXPECT_EQ(output[t * 12 + 4 + k], byte) << t;
            EXPECT_EQ(output[t * 12 + 8 + k], byte >= 128 ? byte | 0xFFFFFF00u : byte) << t;
        }
    }
}

TEST(PackedBytesMetal, Native32AllPackModesAndBothUnpacksStayExact) {
    if (!MetalDevice::instance().available()) GTEST_SKIP();
    const Sm6StageBlob* shader = nullptr;
    for (const auto& b : kSm6StageBlobs) if (std::string(b.name) == "cs_pack") shader = &b;
    ASSERT_NE(shader, nullptr);
    constexpr u32 threads = 4096;
    std::vector<u32> output(threads * 8, 0xDEADBEEFu);
    execute_byte_fixture(shader->code, shader->size, std::vector<u32>(4), output, threads / 64, 64);
    ASSERT_FALSE(HasFatalFailure());
    for (u32 t = 0; t < threads; ++t) {
        const u32 a = t * 0x9E3779B1u ^ (t << 7);
        const std::array<u32, 4> u{a & 511u, (a >> 9) & 511u, t, 300u - t * 5u};
        std::array<i64, 4> uv{}, sv{};
        for (u32 k = 0; k < 4; ++k) {
            uv[k] = u[k];
            const u32 b = u[k] - 200u;
            sv[k] = b >= 0x80000000u ? i64{b} - 0x100000000LL : b;
        }
        EXPECT_EQ(output[t * 8], pack_bytes(uv, 0)) << t;
        EXPECT_EQ(output[t * 8 + 1], pack_bytes(sv, 0)) << t;
        EXPECT_EQ(output[t * 8 + 2], pack_bytes(sv, 1)) << t;
        EXPECT_EQ(output[t * 8 + 3], pack_bytes(sv, 2)) << t;
        std::array<i64, 4> ub{}, sb{};
        for (u32 k = 0; k < 4; ++k) { ub[k] = (a >> (k * 8)) & 255u; sb[k] = ub[k] >= 128 ? ub[k] - 256 : ub[k]; }
        EXPECT_EQ(output[t * 8 + 4], static_cast<u32>(ub[0] + ub[1] * 3 + ub[2] * 5 + ub[3] * 7)) << t;
        EXPECT_EQ(output[t * 8 + 5], static_cast<u32>(sb[0] + sb[1] * 3 + sb[2] * 5 + sb[3] * 7)) << t;
        EXPECT_EQ(output[t * 8 + 6], static_cast<u32>(ub[3]) ^ static_cast<u32>(sb[2])) << t;
        EXPECT_EQ(output[t * 8 + 7], static_cast<u32>(sb[0])) << t;
    }
}

class GeneratedMslTest : public ::testing::Test {
protected:
    void SetUp() override {
        if (!MetalDevice::instance().available()) {
            GTEST_SKIP() << "no Metal device on this machine";
        }
    }

    /// Build a pipeline from a generated shader pair, returning whether Metal
    /// accepted it. A rejection prints the source so the failure names the
    /// generator output rather than just "pipeline creation failed".
    [[nodiscard]] bool compiles(
        const std::string& vertex_msl, const std::string& fragment_msl,
        const std::string& vertex_entry, const std::string& fragment_entry,
        const std::vector<BackendVertexAttribute>& attributes,
        unsigned stride, bool depth = false) {
        BackendPipelineDesc desc{};
        desc.vertex_shader_msl = vertex_msl;
        desc.fragment_shader_msl = fragment_msl;
        desc.vertex_entry_point = vertex_entry;
        desc.fragment_entry_point = fragment_entry;
        desc.vertex_attributes = attributes;
        desc.vertex_stride = stride;
        desc.color_format = PixelFormat::BGRA8Unorm;
        desc.depth_attachment = depth;
        desc.cache_key = 1;

        auto pipeline = MetalDevice::instance().create_pipeline(desc);
        const bool ok = pipeline != nullptr && pipeline->valid();
        if (!ok) {
            ADD_FAILURE() << "Metal rejected the generated pipeline.\n"
                          << "--- vertex ---\n" << vertex_msl
                          << "\n--- fragment ---\n" << fragment_msl;
        }
        return ok;
    }

    /// Attribute list matching the fixed-function generator: the product's
    /// own FVF layout (ff_vertex_layout), the single definition of both the
    /// memory layout and the attribute numbering, so PSIZE, blend weights,
    /// palette indices and texture-coordinate sizes are covered too.
    [[nodiscard]] static std::vector<BackendVertexAttribute> ff_attributes(
        u32 fvf, unsigned& stride) {
        std::vector<BackendVertexAttribute> attributes;
        u32 bytes = 0;
        const char* rejection = nullptr;
        EXPECT_TRUE(ff_build_vertex_layout(fvf, attributes, bytes, rejection))
            << (rejection != nullptr ? rejection : "");
        stride = bytes;
        return attributes;
    }

    [[nodiscard]] bool ff_compiles(const FFPipelineState& state) {
        const FFGeneratedShaders shaders = generate_ff_shaders(state);
        EXPECT_TRUE(shaders.valid());
        unsigned stride = 0;
        const auto attributes = ff_attributes(state.fvf_flags, stride);
        return compiles(shaders.vertex_msl, shaders.fragment_msl, "ff_vertex",
                        "ff_fragment", attributes, stride);
    }
};

// FVF bits used below.
constexpr u32 FVF_XYZ = 0x002;
constexpr u32 FVF_XYZRHW = 0x004;
constexpr u32 FVF_NORMAL = 0x010;
constexpr u32 FVF_DIFFUSE = 0x040;
constexpr u32 FVF_SPECULAR = 0x080;
constexpr u32 FVF_TEX1 = 0x100;
constexpr u32 FVF_TEX2 = 0x200;

} // namespace

// =============================================================================
// Fixed-function generator
// =============================================================================

TEST_F(GeneratedMslTest, FixedFunctionPositionOnly) {
    FFPipelineState state{};
    state.fvf_flags = FVF_XYZ;
    state.lighting_enabled = false;
    state.texture_stages[0].color_op = TexStageOp::Disable;
    EXPECT_TRUE(ff_compiles(state));
}

TEST_F(GeneratedMslTest, FixedFunctionVertexColour) {
    FFPipelineState state{};
    state.fvf_flags = FVF_XYZ | FVF_DIFFUSE;
    state.lighting_enabled = false;
    EXPECT_TRUE(ff_compiles(state));
}

TEST_F(GeneratedMslTest, FixedFunctionPreTransformed) {
    FFPipelineState state{};
    state.fvf_flags = FVF_XYZRHW | FVF_DIFFUSE;
    state.lighting_enabled = false;
    const FFGeneratedShaders shaders = generate_ff_shaders(state);
    ASSERT_TRUE(shaders.valid());
    EXPECT_NE(shaders.vertex_msl.find(
                  "in.position.xy + float2(0.5, 0.5)"),
              std::string::npos) << shaders.vertex_msl;
    EXPECT_EQ(shaders.vertex_msl.find(
                  "in.position.xy - float2(0.5, 0.5)"),
              std::string::npos) << shaders.vertex_msl;
    EXPECT_TRUE(ff_compiles(state));
}

TEST_F(GeneratedMslTest, HybridPreTransformedUsesMetalPixelCenterOffset) {
    FFPipelineState state{};
    state.fvf_flags = FVF_XYZRHW | FVF_DIFFUSE;
    state.lighting_enabled = false;

    const std::string vertex =
        generate_ff_vertex_for_sm3_varyings(state, {0}, {});
    EXPECT_NE(vertex.find("in.position.xy + float2(0.5, 0.5)"),
              std::string::npos) << vertex;
    EXPECT_EQ(vertex.find("in.position.xy - float2(0.5, 0.5)"),
              std::string::npos) << vertex;
}

TEST_F(GeneratedMslTest, FixedFunctionPreTransformedWithFogFromSpecular) {
    FFPipelineState state{};
    state.fvf_flags = FVF_XYZRHW | FVF_DIFFUSE | FVF_SPECULAR;
    state.lighting_enabled = false;
    state.fog_mode = FogMode::Linear;
    EXPECT_TRUE(ff_compiles(state));
}

TEST_F(GeneratedMslTest, LightmapStageSamplesWhenTexcoordSetMissing) {
    // Stage 1 defaults to TEXCOORDINDEX 1. A TEX1 mesh has only set 0.
    // Substituting white here leaves albedo × lightmap red.
    FFPipelineState state{};
    state.fvf_flags = FVF_XYZRHW | FVF_DIFFUSE | FVF_TEX1;
    state.lighting_enabled = false;
    state.active_texture_stages = 2;
    state.texture_stages[0].color_op = TexStageOp::SelectArg1;
    state.texture_stages[0].color_arg1 = static_cast<u32>(TexStageArg::Texture);
    state.texture_stages[0].texcoord_index = 0;
    state.texture_stages[1].color_op = TexStageOp::Modulate;
    state.texture_stages[1].color_arg1 = static_cast<u32>(TexStageArg::Current);
    state.texture_stages[1].color_arg2 = static_cast<u32>(TexStageArg::Texture);
    state.texture_stages[1].texcoord_index = 1;
    const FFGeneratedShaders shaders = generate_ff_shaders(state);
    ASSERT_TRUE(shaders.valid());
    EXPECT_EQ(shaders.fragment_msl.find("tex_sample = float4(1.0)"),
              std::string::npos)
        << shaders.fragment_msl;
    EXPECT_NE(shaders.fragment_msl.find("tex1.sample"), std::string::npos)
        << shaders.fragment_msl;
    EXPECT_NE(shaders.fragment_msl.find("in.texcoord0"), std::string::npos)
        << shaders.fragment_msl;
    EXPECT_TRUE(ff_compiles(state));
}

TEST_F(GeneratedMslTest, FixedFunctionOneTextureStage) {
    FFPipelineState state{};
    state.fvf_flags = FVF_XYZ | FVF_DIFFUSE | FVF_TEX1;
    state.lighting_enabled = false;
    state.active_texture_stages = 1;
    state.texture_stages[0].color_op = TexStageOp::Modulate;
    state.texture_stages[0].color_arg1 = static_cast<u32>(TexStageArg::Texture);
    state.texture_stages[0].color_arg2 = static_cast<u32>(TexStageArg::Diffuse);
    state.texture_stages[0].alpha_op = TexStageOp::SelectArg1;
    state.texture_stages[0].alpha_arg1 = static_cast<u32>(TexStageArg::Texture);
    const FFGeneratedShaders shaders = generate_ff_shaders(state);
    ASSERT_TRUE(shaders.valid());
    EXPECT_NE(shaders.fragment_msl.find("float lod_biases[8];"),
              std::string::npos);
    EXPECT_NE(shaders.fragment_msl.find("bias(cb.lod_biases[0])"),
              std::string::npos);
    EXPECT_TRUE(ff_compiles(state));
}

TEST_F(GeneratedMslTest, FixedFunctionTwoTextureStages) {
    FFPipelineState state{};
    state.fvf_flags = FVF_XYZ | FVF_DIFFUSE | FVF_TEX2;
    state.lighting_enabled = false;
    state.active_texture_stages = 2;
    state.texture_stages[0].color_op = TexStageOp::SelectArg1;
    state.texture_stages[0].color_arg1 = static_cast<u32>(TexStageArg::Texture);
    state.texture_stages[0].alpha_op = TexStageOp::SelectArg1;
    state.texture_stages[1].color_op = TexStageOp::Add;
    state.texture_stages[1].color_arg1 = static_cast<u32>(TexStageArg::Texture);
    state.texture_stages[1].color_arg2 = static_cast<u32>(TexStageArg::Current);
    state.texture_stages[1].alpha_op = TexStageOp::Modulate;
    state.texture_stages[1].texcoord_index = 1;
    EXPECT_TRUE(ff_compiles(state));
}

TEST_F(GeneratedMslTest, FixedFunctionDotProduct3Combiner) {
    // DotProduct3 emits a differently shaped statement from every other
    // combiner, so it is the one most likely to generate invalid MSL.
    FFPipelineState state{};
    state.fvf_flags = FVF_XYZ | FVF_DIFFUSE | FVF_TEX1;
    state.lighting_enabled = false;
    state.active_texture_stages = 1;
    state.texture_stages[0].color_op = TexStageOp::DotProduct3;
    state.texture_stages[0].color_arg1 = static_cast<u32>(TexStageArg::Texture);
    state.texture_stages[0].color_arg2 = static_cast<u32>(TexStageArg::Diffuse);
    state.texture_stages[0].alpha_op = TexStageOp::SelectArg1;
    EXPECT_TRUE(ff_compiles(state));
}

TEST_F(GeneratedMslTest, FixedFunctionD3dtopEnumMatchesMsdn) {
    // Guard against regressing the off-by-one that remapped Blend* / AddSmooth.
    EXPECT_EQ(static_cast<int>(TexStageOp::AddSigned2x), 9);
    EXPECT_EQ(static_cast<int>(TexStageOp::AddSmooth), 11);
    EXPECT_EQ(static_cast<int>(TexStageOp::BlendDiffuseAlpha), 12);
    EXPECT_EQ(static_cast<int>(TexStageOp::BlendTextureAlpha), 13);
    EXPECT_EQ(static_cast<int>(TexStageOp::BlendFactorAlpha), 14);
    EXPECT_EQ(static_cast<int>(TexStageOp::Premodulate), 17);
    EXPECT_EQ(static_cast<int>(TexStageOp::BumpEnvMap), 22);
    EXPECT_EQ(static_cast<int>(TexStageOp::BumpEnvMapLuminance), 23);
    EXPECT_EQ(static_cast<int>(TexStageOp::MultiplyAdd), 25);
    EXPECT_EQ(static_cast<int>(TexStageOp::Lerp), 26);
}

TEST_F(GeneratedMslTest, FixedFunctionPremodulateChangesNextStageCurrentSource) {
    FFPipelineState state{};
    state.fvf_flags = FVF_XYZ | FVF_DIFFUSE | FVF_TEX1;
    state.lighting_enabled = false;
    state.active_texture_stages = 2;
    state.texture_stages[0].color_op = TexStageOp::Premodulate;
    state.texture_stages[0].color_arg1 = static_cast<u32>(TexStageArg::Diffuse);
    state.texture_stages[0].alpha_op = TexStageOp::SelectArg1;
    state.texture_stages[0].alpha_arg1 = static_cast<u32>(TexStageArg::Diffuse);
    state.texture_stages[1].color_op = TexStageOp::SelectArg1;
    state.texture_stages[1].color_arg1 = static_cast<u32>(TexStageArg::Current);
    state.texture_stages[1].alpha_op = TexStageOp::SelectArg1;
    state.texture_stages[1].alpha_arg1 =
        static_cast<u32>(TexStageArg::Current) | 0x20u | 0x10u;

    const FFGeneratedShaders shaders = generate_ff_shaders(state);
    ASSERT_TRUE(shaders.valid());
    EXPECT_NE(shaders.fragment_msl.find(
                  "float4 arg1c = (current * tex_sample);"),
              std::string::npos)
        << shaders.fragment_msl;
    // PREMODULATE changes the source first; ALPHAREPLICATE then COMPLEMENT
    // retain the D3DTA modifier order around that source.
    EXPECT_NE(shaders.fragment_msl.find(
                  "float4 arg1a = (float4(1.0) - (float4((current * "
                  "tex_sample).aaaa)));"),
              std::string::npos)
        << shaders.fragment_msl;
    EXPECT_TRUE(ff_compiles(state));

    const std::string hybrid = generate_ff_fragment_for_sm3_varyings(
        state, {0}, {0});
    EXPECT_NE(hybrid.find("float4 arg1c = (current * tex_sample);"),
              std::string::npos)
        << hybrid;
    EXPECT_NE(hybrid.find(
                  "float4 arg1a = (float4(1.0) - (float4((current * "
                  "tex_sample).aaaa)));"),
              std::string::npos)
        << hybrid;
}

TEST_F(GeneratedMslTest, FixedFunctionTempResultRemainsDistinctFromCurrent) {
    FFPipelineState state{};
    state.fvf_flags = FVF_XYZ | FVF_DIFFUSE | FVF_TEX2;
    state.lighting_enabled = false;
    state.active_texture_stages = 2;
    // Stage 0 writes its texture result only to TEMP. Stage 1 reads both the
    // untouched CURRENT and TEMP, then returns their sum to CURRENT.
    state.texture_stages[0].color_op = TexStageOp::SelectArg1;
    state.texture_stages[0].color_arg1 = static_cast<u32>(TexStageArg::Texture);
    state.texture_stages[0].alpha_op = TexStageOp::SelectArg1;
    state.texture_stages[0].alpha_arg1 = static_cast<u32>(TexStageArg::Texture);
    state.texture_stages[0].result_arg = static_cast<u32>(TexStageArg::Temp);
    state.texture_stages[1].color_op = TexStageOp::Add;
    state.texture_stages[1].color_arg1 = static_cast<u32>(TexStageArg::Current);
    state.texture_stages[1].color_arg2 = static_cast<u32>(TexStageArg::Temp);
    state.texture_stages[1].alpha_op = TexStageOp::SelectArg1;
    state.texture_stages[1].alpha_arg1 = static_cast<u32>(TexStageArg::Temp);

    const FFGeneratedShaders shaders = generate_ff_shaders(state);
    ASSERT_TRUE(shaders.valid());
    EXPECT_NE(shaders.fragment_msl.find("float4 temp = float4(0.0);"),
              std::string::npos) << shaders.fragment_msl;
    EXPECT_NE(shaders.fragment_msl.find("temp = stage_result;"),
              std::string::npos) << shaders.fragment_msl;
    EXPECT_NE(shaders.fragment_msl.find("float4 arg1c = current;"),
              std::string::npos) << shaders.fragment_msl;
    EXPECT_NE(shaders.fragment_msl.find("float4 arg2c = temp;"),
              std::string::npos) << shaders.fragment_msl;
    EXPECT_NE(shaders.fragment_msl.find("current = stage_result;"),
              std::string::npos) << shaders.fragment_msl;
    EXPECT_TRUE(ff_compiles(state));

    const std::string hybrid = generate_ff_fragment_for_sm3_varyings(
        state, {0}, {0, 1});
    EXPECT_NE(hybrid.find("float4 temp = float4(0.0);"), std::string::npos)
        << hybrid;
    EXPECT_NE(hybrid.find("temp = stage_result;"), std::string::npos)
        << hybrid;
    EXPECT_NE(hybrid.find("float4 arg2c = temp;"), std::string::npos)
        << hybrid;
    EXPECT_NE(hybrid.find("current = stage_result;"), std::string::npos)
        << hybrid;
}

TEST_F(GeneratedMslTest, FixedFunctionConstantsAreIndexedPerStage) {
    FFPipelineState state{};
    state.fvf_flags = FVF_XYZ | FVF_DIFFUSE | FVF_TEX2;
    state.lighting_enabled = false;
    state.active_texture_stages = 2;
    state.texture_stages[0].color_op = TexStageOp::SelectArg1;
    state.texture_stages[0].color_arg1 =
        static_cast<u32>(TexStageArg::Constant);
    state.texture_stages[1].color_op = TexStageOp::Add;
    state.texture_stages[1].color_arg1 =
        static_cast<u32>(TexStageArg::Current);
    state.texture_stages[1].color_arg2 =
        static_cast<u32>(TexStageArg::Constant);

    const FFGeneratedShaders shaders = generate_ff_shaders(state);
    ASSERT_TRUE(shaders.valid());
    EXPECT_NE(shaders.fragment_msl.find("float4 stage_constants[8];"),
              std::string::npos) << shaders.fragment_msl;
    EXPECT_NE(shaders.fragment_msl.find(
                  "float4 arg1c = cb.stage_constants[0];"),
              std::string::npos) << shaders.fragment_msl;
    EXPECT_NE(shaders.fragment_msl.find(
                  "float4 arg2c = cb.stage_constants[1];"),
              std::string::npos) << shaders.fragment_msl;
    EXPECT_TRUE(ff_compiles(state));

    const std::string hybrid = generate_ff_fragment_for_sm3_varyings(
        state, {0}, {0, 1});
    EXPECT_NE(hybrid.find("float4 stage_constants[8];"), std::string::npos)
        << hybrid;
    EXPECT_NE(hybrid.find("float4 arg1c = cb.stage_constants[0];"),
              std::string::npos) << hybrid;
    EXPECT_NE(hybrid.find("float4 arg2c = cb.stage_constants[1];"),
              std::string::npos) << hybrid;
}

TEST_F(GeneratedMslTest, FixedFunctionRangeFogSelectsDistanceInsteadOfDepth) {
    FFPipelineState depth{};
    depth.fvf_flags = FVF_XYZ | FVF_DIFFUSE;
    depth.lighting_enabled = false;
    depth.fog_mode = FogMode::Linear;
    const FFGeneratedShaders depth_shaders = generate_ff_shaders(depth);
    ASSERT_TRUE(depth_shaders.valid());
    EXPECT_NE(depth_shaders.vertex_msl.find(
                  "float fog_distance = abs(eye_position.z);"),
              std::string::npos) << depth_shaders.vertex_msl;
    EXPECT_EQ(depth_shaders.vertex_msl.find("length(eye_position)"),
              std::string::npos) << depth_shaders.vertex_msl;

    FFPipelineState range = depth;
    range.range_fog_enable = true;
    const FFGeneratedShaders range_shaders = generate_ff_shaders(range);
    ASSERT_TRUE(range_shaders.valid());
    EXPECT_NE(range_shaders.vertex_msl.find(
                  "float fog_distance = length(eye_position);"),
              std::string::npos) << range_shaders.vertex_msl;
    EXPECT_NE(depth.hash(), range.hash());
    EXPECT_TRUE(ff_compiles(depth));
    EXPECT_TRUE(ff_compiles(range));
}

TEST(FFPipelineHashTest, UniformLightAndMaterialPayloadDoesNotChangeHash) {
    FFPipelineState base{};
    base.fvf_flags = FVF_XYZ | FVF_NORMAL | FVF_DIFFUSE;
    base.lighting_enabled = true;
    base.active_light_count = 1;
    base.lights[0].enabled = true;
    base.lights[0].type = LightType::Directional;
    base.lights[0].direction[0] = 0.0f;
    base.lights[0].direction[1] = 0.0f;
    base.lights[0].direction[2] = -1.0f;
    base.material.diffuse[0] = 1.0f;
    base.fog_start = 10.0f;
    base.fog_end = 100.0f;
    base.tween_factor = 0.25f;
    base.alpha_ref = 128;

    FFPipelineState moved = base;
    moved.lights[0].direction[0] = 0.7f;
    moved.lights[0].position[0] = 12.0f;
    moved.material.diffuse[0] = 0.1f;
    moved.fog_start = 50.0f;
    moved.tween_factor = 0.9f;
    moved.alpha_ref = 16;

    EXPECT_EQ(base.hash(), moved.hash());
    const FFGeneratedShaders base_shaders = generate_ff_shaders(base);
    const FFGeneratedShaders moved_shaders = generate_ff_shaders(moved);
    ASSERT_TRUE(base_shaders.valid());
    ASSERT_TRUE(moved_shaders.valid());
    EXPECT_EQ(base_shaders.vertex_msl, moved_shaders.vertex_msl);
    EXPECT_EQ(base_shaders.fragment_msl, moved_shaders.fragment_msl);

    FFPipelineState spot = base;
    spot.lights[0].type = LightType::Spot;
    EXPECT_NE(base.hash(), spot.hash());
}

TEST_F(GeneratedMslTest, FixedFunctionNormalNormalizationIsStateControlled) {
    FFPipelineState raw{};
    raw.fvf_flags = FVF_XYZ | FVF_NORMAL | FVF_DIFFUSE;
    raw.lighting_enabled = true;
    raw.active_light_count = 1;
    raw.lights[0].enabled = true;
    raw.lights[0].type = LightType::Directional;
    const FFGeneratedShaders raw_shaders = generate_ff_shaders(raw);
    ASSERT_TRUE(raw_shaders.valid());
    EXPECT_NE(raw_shaders.vertex_msl.find(
                  "float3 world_normal = (float4(in.normal, 0.0) * cb.world_inv_transpose"),
              std::string::npos) << raw_shaders.vertex_msl;
    EXPECT_EQ(raw_shaders.vertex_msl.find(
                  "world_normal = normalize(world_normal);"),
              std::string::npos) << raw_shaders.vertex_msl;

    FFPipelineState normalized = raw;
    normalized.normalize_normals = true;
    const FFGeneratedShaders normalized_shaders =
        generate_ff_shaders(normalized);
    ASSERT_TRUE(normalized_shaders.valid());
    EXPECT_NE(normalized_shaders.vertex_msl.find(
                  "world_normal = normalize(world_normal);"),
              std::string::npos) << normalized_shaders.vertex_msl;
    EXPECT_NE(raw.hash(), normalized.hash());
    EXPECT_TRUE(ff_compiles(raw));
    EXPECT_TRUE(ff_compiles(normalized));
}

TEST_F(GeneratedMslTest, FixedFunctionLocalViewerControlsSpecularDirection) {
    FFPipelineState local{};
    local.fvf_flags = FVF_XYZ | FVF_NORMAL | FVF_DIFFUSE;
    local.lighting_enabled = true;
    local.active_light_count = 1;
    local.lights[0].enabled = true;
    local.lights[0].type = LightType::Directional;
    local.specular_enable = true;
    const FFGeneratedShaders local_shaders = generate_ff_shaders(local);
    ASSERT_TRUE(local_shaders.valid());
    EXPECT_NE(local_shaders.vertex_msl.find(
                  "normalize(cb.camera_position.xyz - world_pos)"),
              std::string::npos) << local_shaders.vertex_msl;

    FFPipelineState infinite = local;
    infinite.local_viewer = false;
    const FFGeneratedShaders infinite_shaders = generate_ff_shaders(infinite);
    ASSERT_TRUE(infinite_shaders.valid());
    EXPECT_NE(infinite_shaders.vertex_msl.find(
                  "float3 view_dir = cb.camera_direction.xyz;"),
              std::string::npos) << infinite_shaders.vertex_msl;
    EXPECT_NE(local.hash(), infinite.hash());
    EXPECT_TRUE(ff_compiles(local));
    EXPECT_TRUE(ff_compiles(infinite));
}

TEST_F(GeneratedMslTest, FixedFunctionFlatShadeQualifiesColorVaryings) {
    FFPipelineState gouraud{};
    gouraud.fvf_flags = FVF_XYZ | FVF_DIFFUSE | FVF_SPECULAR;
    gouraud.lighting_enabled = false;
    const FFGeneratedShaders gouraud_shaders = generate_ff_shaders(gouraud);
    ASSERT_TRUE(gouraud_shaders.valid());
    EXPECT_EQ(gouraud_shaders.vertex_msl.find("[[flat]]"), std::string::npos);
    EXPECT_EQ(gouraud_shaders.fragment_msl.find("[[flat]]"),
              std::string::npos);

    FFPipelineState flat = gouraud;
    flat.flat_shading = true;
    const FFGeneratedShaders flat_shaders = generate_ff_shaders(flat);
    ASSERT_TRUE(flat_shaders.valid());
    EXPECT_NE(flat_shaders.vertex_msl.find("float4 diffuse [[flat]];"),
              std::string::npos) << flat_shaders.vertex_msl;
    EXPECT_NE(flat_shaders.fragment_msl.find("float4 specular [[flat]];"),
              std::string::npos) << flat_shaders.fragment_msl;

    const std::string hybrid_fragment = generate_ff_fragment_for_sm3_varyings(
        flat, {0, 1}, {0}, false);
    const std::string hybrid_vertex = generate_ff_vertex_for_sm3_varyings(
        flat, {0, 1}, {0});
    EXPECT_NE(hybrid_fragment.find("color0 [[user(color0), flat]]"),
              std::string::npos) << hybrid_fragment;
    EXPECT_NE(hybrid_vertex.find("color1 [[user(color1), flat]]"),
              std::string::npos) << hybrid_vertex;
    const std::string programmable = apply_sm3_flat_shading(
        "float4 color0 [[user(color0)]];\n"
        "float4 texcoord0 [[user(texcoord0)]];\n");
    EXPECT_NE(programmable.find("color0 [[user(color0), flat]]"),
              std::string::npos) << programmable;
    EXPECT_NE(programmable.find("texcoord0 [[user(texcoord0)]]"),
              std::string::npos) << programmable;
    EXPECT_NE(gouraud.hash(), flat.hash());
    EXPECT_TRUE(ff_compiles(gouraud));
    EXPECT_TRUE(ff_compiles(flat));
}

TEST_F(GeneratedMslTest, FixedFunctionPointListWritesDynamicPointSize) {
    FFPipelineState triangles{};
    triangles.fvf_flags = FVF_XYZ | FVF_DIFFUSE;
    triangles.lighting_enabled = false;
    const FFGeneratedShaders triangle_shaders = generate_ff_shaders(triangles);
    ASSERT_TRUE(triangle_shaders.valid());
    EXPECT_EQ(triangle_shaders.vertex_msl.find("[[point_size]]"),
              std::string::npos);

    FFPipelineState points = triangles;
    points.point_size_output = true;
    const FFGeneratedShaders point_shaders = generate_ff_shaders(points);
    ASSERT_TRUE(point_shaders.valid());
    EXPECT_NE(point_shaders.vertex_msl.find(
                  "float point_size [[point_size]];"),
              std::string::npos) << point_shaders.vertex_msl;
    EXPECT_NE(point_shaders.vertex_msl.find(
                  "out.point_size = cb.point_params.x;"),
              std::string::npos) << point_shaders.vertex_msl;
    EXPECT_NE(triangles.hash(), points.hash());
    EXPECT_TRUE(ff_compiles(points));
}

TEST_F(GeneratedMslTest, FixedFunctionPointScaleUsesEyeDistanceAndViewport) {
    FFPipelineState points{};
    points.fvf_flags = FVF_XYZ | FVF_DIFFUSE;
    points.lighting_enabled = false;
    points.point_size_output = true;
    points.point_scale_enabled = true;

    const FFGeneratedShaders shaders = generate_ff_shaders(points);
    ASSERT_TRUE(shaders.valid());
    EXPECT_NE(shaders.vertex_msl.find("float4 point_scale_params;"),
              std::string::npos) << shaders.vertex_msl;
    EXPECT_NE(shaders.vertex_msl.find(
                  "float3 point_eye = (float4(in.position, 1.0) * cb.world * cb.view"),
              std::string::npos) << shaders.vertex_msl;
    EXPECT_NE(shaders.vertex_msl.find("length(point_eye)"),
              std::string::npos) << shaders.vertex_msl;
    EXPECT_NE(shaders.vertex_msl.find("cb.point_scale_params.w * rsqrt"),
              std::string::npos) << shaders.vertex_msl;
    EXPECT_NE(shaders.vertex_msl.find(
                  "clamp(scaled_point_size, cb.point_params.y, "
                  "cb.point_params.z)"),
              std::string::npos) << shaders.vertex_msl;

    const std::string hybrid = generate_ff_vertex_for_sm3_varyings(
        points, {}, {});
    EXPECT_NE(hybrid.find("float4 point_scale_params;"), std::string::npos)
        << hybrid;
    EXPECT_NE(hybrid.find("length(point_eye)"), std::string::npos) << hybrid;
    EXPECT_TRUE(ff_compiles(points));
}

TEST_F(GeneratedMslTest, FixedFunctionPerVertexPointSizeOverridesRenderState) {
    FFPipelineState points{};
    points.fvf_flags = FVF_XYZ | fvf::PSIZE | FVF_DIFFUSE;
    points.lighting_enabled = false;
    points.point_size_output = true;

    const FFGeneratedShaders unscaled = generate_ff_shaders(points);
    ASSERT_TRUE(unscaled.valid());
    EXPECT_NE(unscaled.vertex_msl.find("float point_size [[attribute(1)]];"),
              std::string::npos) << unscaled.vertex_msl;
    EXPECT_NE(unscaled.vertex_msl.find(
                  "out.point_size = clamp(in.point_size, cb.point_params.y, "
                  "cb.point_params.z);"),
              std::string::npos) << unscaled.vertex_msl;

    points.point_scale_enabled = true;
    const FFGeneratedShaders scaled = generate_ff_shaders(points);
    ASSERT_TRUE(scaled.valid());
    EXPECT_NE(scaled.vertex_msl.find(
                  "float scaled_point_size = in.point_size * "
                  "cb.point_scale_params.w"),
              std::string::npos) << scaled.vertex_msl;
    EXPECT_TRUE(ff_compiles(points));
}

TEST_F(GeneratedMslTest, FixedFunctionClipPlanesUseWorldSpaceDistances) {
    FFPipelineState state{};
    state.fvf_flags = FVF_XYZ | FVF_DIFFUSE;
    state.lighting_enabled = false;
    state.clip_planes_enabled = true;

    const FFGeneratedShaders shaders = generate_ff_shaders(state);
    ASSERT_TRUE(shaders.valid());
    EXPECT_NE(shaders.vertex_msl.find(
                  "float clip_distances [[clip_distance]] [6];"),
              std::string::npos) << shaders.vertex_msl;
    EXPECT_NE(shaders.vertex_msl.find(
                  "float4 clip_world_position = "
                  "float4(in.position, 1.0) * cb.world;"),
              std::string::npos) << shaders.vertex_msl;
    EXPECT_NE(shaders.vertex_msl.find(
                  "dot(cb.clip_planes[clip_index], clip_world_position)"),
              std::string::npos) << shaders.vertex_msl;
    EXPECT_TRUE(ff_compiles(state));
}

TEST_F(GeneratedMslTest, FixedFunctionXyzb1UsesImplicitSecondWeight) {
    FFPipelineState state{};
    state.fvf_flags = fvf::XYZB1 | FVF_DIFFUSE;
    state.lighting_enabled = false;
    state.vertex_blend_weight_count = 1;

    const FFGeneratedShaders shaders = generate_ff_shaders(state);
    ASSERT_TRUE(shaders.valid());
    EXPECT_NE(shaders.vertex_msl.find("float4x4 world1;"), std::string::npos)
        << shaders.vertex_msl;
    EXPECT_NE(shaders.vertex_msl.find(
                  "float blend_weights [[attribute(1)]];"), std::string::npos)
        << shaders.vertex_msl;
    EXPECT_NE(shaders.vertex_msl.find(
                  "float blend_weight1 = 1.0 - blend_weight0;"),
              std::string::npos) << shaders.vertex_msl;
    EXPECT_NE(shaders.vertex_msl.find(
                  "blend_weight0 * (object_position * cb.world)"),
              std::string::npos) << shaders.vertex_msl;
    EXPECT_NE(shaders.vertex_msl.find(
                  "blend_weight1 * (object_position * cb.world1)"),
              std::string::npos) << shaders.vertex_msl;
    EXPECT_TRUE(ff_compiles(state));
}

TEST_F(GeneratedMslTest, FixedFunctionXyzb3UsesImplicitFourthWeight) {
    FFPipelineState state{};
    state.fvf_flags = fvf::XYZB3 | FVF_DIFFUSE;
    state.lighting_enabled = false;
    state.vertex_blend_weight_count = 3;

    const FFGeneratedShaders shaders = generate_ff_shaders(state);
    ASSERT_TRUE(shaders.valid());
    EXPECT_NE(shaders.vertex_msl.find(
                  "float3 blend_weights [[attribute(1)]];"),
              std::string::npos) << shaders.vertex_msl;
    EXPECT_NE(shaders.vertex_msl.find(
                  "float blend_weight3 = 1.0 - blend_weight0 - "
                  "blend_weight1 - blend_weight2;"), std::string::npos)
        << shaders.vertex_msl;
    EXPECT_NE(shaders.vertex_msl.find(
                  "blend_weight3 * (object_position * cb.world3)"),
              std::string::npos) << shaders.vertex_msl;
    EXPECT_TRUE(ff_compiles(state));
}

TEST_F(GeneratedMslTest, FixedFunctionIndexedBlendUsesPackedPaletteIndices) {
    FFPipelineState state{};
    state.fvf_flags = fvf::XYZB2 | fvf::LASTBETA_UBYTE4 | FVF_DIFFUSE;
    state.lighting_enabled = false;
    state.vertex_blend_weight_count = 1;
    state.indexed_vertex_blend = true;

    const FFGeneratedShaders shaders = generate_ff_shaders(state);
    ASSERT_TRUE(shaders.valid());
    EXPECT_NE(shaders.vertex_msl.find("float4x4 blend_worlds[9];"),
              std::string::npos) << shaders.vertex_msl;
    EXPECT_NE(shaders.vertex_msl.find(
                  "uchar4 blend_indices [[attribute(2)]];"),
              std::string::npos) << shaders.vertex_msl;
    EXPECT_NE(shaders.vertex_msl.find(
                  "blend_weight0 * (object_position * "
                  "cb.blend_worlds[uint(in.blend_indices.x)])"),
              std::string::npos) << shaders.vertex_msl;
    EXPECT_NE(shaders.vertex_msl.find(
                  "blend_weight1 * (object_position * "
                  "cb.blend_worlds[uint(in.blend_indices.y)])"),
              std::string::npos) << shaders.vertex_msl;
    EXPECT_TRUE(ff_compiles(state));
}

TEST_F(GeneratedMslTest, FixedFunctionIndexedZeroWeightsUsesOnePaletteIndex) {
    FFPipelineState state{};
    state.fvf_flags = fvf::XYZB1 | fvf::LASTBETA_UBYTE4 | FVF_DIFFUSE;
    state.lighting_enabled = false;
    state.vertex_blend_weight_count = 0;
    state.indexed_vertex_blend = true;

    const FFGeneratedShaders shaders = generate_ff_shaders(state);
    ASSERT_TRUE(shaders.valid());
    EXPECT_NE(shaders.vertex_msl.find(
                  "uchar4 blend_indices [[attribute(1)]];"),
              std::string::npos) << shaders.vertex_msl;
    EXPECT_NE(shaders.vertex_msl.find("float blend_weight0 = 1.0;"),
              std::string::npos) << shaders.vertex_msl;
    EXPECT_NE(shaders.vertex_msl.find(
                  "blend_weight0 * (object_position * "
                  "cb.blend_worlds[uint(in.blend_indices.x)])"),
              std::string::npos) << shaders.vertex_msl;
    EXPECT_TRUE(ff_compiles(state));
}

TEST_F(GeneratedMslTest, FixedFunctionTweeningUsesSecondaryPositionAndNormal) {
    FFPipelineState state{};
    state.fvf_flags = fvf::XYZ | fvf::NORMAL | FVF_DIFFUSE;
    state.lighting_enabled = false;
    state.vertex_tweening = true;
    state.tween_factor = 0.25f;

    const FFGeneratedShaders shaders = generate_ff_shaders(state);
    ASSERT_TRUE(shaders.valid());
    EXPECT_NE(shaders.vertex_msl.find(
                  "float3 position1 [[attribute(1)]];"), std::string::npos)
        << shaders.vertex_msl;
    EXPECT_NE(shaders.vertex_msl.find(
                  "float3 normal1 [[attribute(3)]];"), std::string::npos)
        << shaders.vertex_msl;
    EXPECT_NE(shaders.vertex_msl.find(
                  "mix(in.position, in.position1, cb.tween_params.x)"),
              std::string::npos) << shaders.vertex_msl;
    // Tweening takes a vertex declaration (POSITION0/1, NORMAL0/1), which no
    // FVF can describe: the attributes are the declaration's, in the
    // generator's numbering (position 0, position1 1, normal 2, normal1 3,
    // then the FVF's diffuse).
    const std::vector<BackendVertexAttribute> declaration{
        {0, 0, BackendVertexFormat::Float3},
        {1, 12, BackendVertexFormat::Float3},
        {2, 24, BackendVertexFormat::Float3},
        {3, 36, BackendVertexFormat::Float3},
        {4, 48, BackendVertexFormat::UByte4NormalizedBGRA}};
    EXPECT_TRUE(compiles(shaders.vertex_msl, shaders.fragment_msl, "ff_vertex",
                         "ff_fragment", declaration, 52));
}

TEST_F(GeneratedMslTest, FixedFunctionBumpEnvOffsetsNextStageCoordinates) {
    FFPipelineState state{};
    state.fvf_flags = FVF_XYZ | FVF_DIFFUSE | FVF_TEX1;
    state.lighting_enabled = false;
    state.active_texture_stages = 2;
    state.texture_stages[0].color_op = TexStageOp::BumpEnvMap;
    state.texture_stages[0].color_arg1 = static_cast<u32>(TexStageArg::Texture);
    state.texture_stages[0].alpha_op = TexStageOp::SelectArg1;
    state.texture_stages[0].bump_env_mat00 = 0x3F400000u;  // 0.75f
    state.texture_stages[0].bump_env_mat11 = 0x3F800000u;  // 1.0f
    state.texture_stages[1].color_op = TexStageOp::SelectArg1;
    state.texture_stages[1].color_arg1 = static_cast<u32>(TexStageArg::Texture);
    state.texture_stages[1].alpha_op = TexStageOp::SelectArg1;

    const FFGeneratedShaders shaders = generate_ff_shaders(state);
    ASSERT_TRUE(shaders.valid());
    EXPECT_NE(shaders.fragment_msl.find("float2 bump_offset = float2(0.0)"),
              std::string::npos)
        << shaders.fragment_msl;
    EXPECT_NE(shaders.fragment_msl.find(
                  "tex_sample.r * as_type<float>(uint(1061158912u))"),
              std::string::npos)
        << shaders.fragment_msl;
    EXPECT_NE(shaders.fragment_msl.find("uv += bump_offset;"),
              std::string::npos)
        << shaders.fragment_msl;
    EXPECT_TRUE(ff_compiles(state));

    const std::string hybrid = generate_ff_fragment_for_sm3_varyings(
        state, {0}, {0});
    EXPECT_NE(hybrid.find("float2 bump_offset = float2(0.0)"),
              std::string::npos)
        << hybrid;
    EXPECT_NE(hybrid.find(
                  "tex_sample.r * as_type<float>(uint(1061158912u))"),
              std::string::npos)
        << hybrid;
    EXPECT_NE(hybrid.find(".xy + bump_offset;"), std::string::npos) << hybrid;
}

TEST_F(GeneratedMslTest,
       FixedFunctionBumpEnvLuminanceModulatesNextStageChosenColor) {
    FFPipelineState state{};
    state.fvf_flags = FVF_XYZ | FVF_DIFFUSE | FVF_TEX1;
    state.lighting_enabled = false;
    state.active_texture_stages = 2;
    state.texture_stages[0].color_op = TexStageOp::BumpEnvMapLuminance;
    state.texture_stages[0].alpha_op = TexStageOp::SelectArg1;
    state.texture_stages[0].bump_env_mat00 = 0x3F400000u; // 0.75f
    state.texture_stages[0].bump_env_lscale = 0x3F000000u; // 0.5f
    state.texture_stages[0].bump_env_loffset = 0x3E800000u; // 0.25f
    state.texture_stages[1].color_op = TexStageOp::SelectArg1;
    state.texture_stages[1].color_arg1 = static_cast<u32>(TexStageArg::Current);
    state.texture_stages[1].alpha_op = TexStageOp::SelectArg1;

    const FFGeneratedShaders shaders = generate_ff_shaders(state);
    ASSERT_TRUE(shaders.valid());
    EXPECT_NE(shaders.fragment_msl.find(
                  "bump_luminance = tex_sample.b * "
                  "as_type<float>(uint(1056964608u)) + "
                  "as_type<float>(uint(1048576000u));"),
              std::string::npos)
        << shaders.fragment_msl;
    // Learn "Bump Mapping Formulas (Direct3D 9)": the next stage "modulates
    // the chosen color by the luminance" -- that stage's result.
    EXPECT_NE(shaders.fragment_msl.find(
                  "stage_result.rgb = clamp(stage_result.rgb * bump_luminance, "
                  "0.0, 1.0);"),
              std::string::npos)
        << shaders.fragment_msl;
    EXPECT_TRUE(ff_compiles(state));

    const std::string hybrid = generate_ff_fragment_for_sm3_varyings(
        state, {0}, {0});
    EXPECT_NE(hybrid.find("bump_luminance = tex_sample.b"),
              std::string::npos)
        << hybrid;
    EXPECT_NE(hybrid.find(
                  "stage_result.rgb = clamp(stage_result.rgb * bump_luminance"),
              std::string::npos)
        << hybrid;
}

TEST_F(GeneratedMslTest, FixedFunctionMultiplyAddAndLerpUseArg0) {
    FFPipelineState state{};
    state.fvf_flags = FVF_XYZ | FVF_DIFFUSE | FVF_TEX1;
    state.lighting_enabled = false;
    state.active_texture_stages = 1;
    state.texture_stages[0].color_op = TexStageOp::MultiplyAdd;
    state.texture_stages[0].color_arg0 = static_cast<u32>(TexStageArg::Diffuse);
    state.texture_stages[0].color_arg1 = static_cast<u32>(TexStageArg::Texture);
    state.texture_stages[0].color_arg2 = static_cast<u32>(TexStageArg::Current);
    state.texture_stages[0].alpha_op = TexStageOp::Lerp;
    state.texture_stages[0].alpha_arg0 = static_cast<u32>(TexStageArg::Diffuse);
    state.texture_stages[0].alpha_arg1 = static_cast<u32>(TexStageArg::Texture);
    state.texture_stages[0].alpha_arg2 = static_cast<u32>(TexStageArg::Current);

    const FFGeneratedShaders shaders = generate_ff_shaders(state);
    ASSERT_TRUE(shaders.valid());
    EXPECT_NE(shaders.fragment_msl.find("arg0c"), std::string::npos);
    EXPECT_NE(shaders.fragment_msl.find("arg0a"), std::string::npos);
    EXPECT_TRUE(ff_compiles(state));
}

TEST_F(GeneratedMslTest, FixedFunctionProjectedTexcoordDivide) {
    FFPipelineState state{};
    state.fvf_flags = FVF_XYZ | FVF_DIFFUSE | FVF_TEX1;
    state.lighting_enabled = false;
    state.active_texture_stages = 1;
    state.texture_stages[0].color_op = TexStageOp::SelectArg1;
    state.texture_stages[0].color_arg1 = static_cast<u32>(TexStageArg::Texture);
    // D3DTTFF_COUNT3 | D3DTTFF_PROJECTED
    state.texture_stages[0].texture_transform_flags = 0x103u;

    const std::string fs = generate_ff_fragment_for_sm3_varyings(
        state, {0}, {0});
    EXPECT_NE(fs.find("tc.xy / tc.z"), std::string::npos) << fs;
    EXPECT_NE(fs.find("texture_matrix["), std::string::npos) << fs;
}

TEST_F(GeneratedMslTest, HybridFixedFunctionClampsColourAndConsumesVertexFog) {
    FFPipelineState state{};
    state.lighting_enabled = false;
    state.active_texture_stages = 0;
    state.fog_mode = FogMode::Linear;

    const std::string fs = generate_ff_fragment_for_sm3_varyings(
        state, {0}, {}, true);
    EXPECT_NE(fs.find("clamp(in.color0, 0.0, 1.0)"),
              std::string::npos) << fs;
    EXPECT_NE(fs.find("clamp(in.fog.x, 0.0, 1.0)"),
              std::string::npos) << fs;
    EXPECT_NE(fs.find("mix(cb.fog_color.rgb, current.rgb, fog_factor)"),
              std::string::npos) << fs;

    const std::string vs = R"MSL(
#include <metal_stdlib>
using namespace metal;
struct VertexIn { float3 position [[attribute(0)]]; };
struct Sm3Varyings {
    float4 position [[position]];
    float4 color0 [[user(color0)]];
    float4 fog [[user(fog)]];
};
vertex Sm3Varyings sm3_vertex(VertexIn in [[stage_in]]) {
    Sm3Varyings out;
    out.position = float4(in.position, 1.0);
    out.color0 = float4(1.0);
    out.fog = float4(0.5);
    return out;
}
)MSL";
    EXPECT_TRUE(compiles(
        vs, fs, "sm3_vertex", "ff_fragment",
        {{0, 0, BackendVertexFormat::Float3}}, 12));
}

TEST_F(GeneratedMslTest, FixedFunctionTextureMatrixInPureFf) {
    FFPipelineState state{};
    state.fvf_flags = FVF_XYZ | FVF_DIFFUSE | FVF_TEX1;
    state.lighting_enabled = false;
    state.active_texture_stages = 1;
    state.texture_stages[0].color_op = TexStageOp::SelectArg1;
    state.texture_stages[0].color_arg1 = static_cast<u32>(TexStageArg::Texture);
    state.texture_stages[0].texture_transform_flags = 0x2u; // COUNT2

    const FFGeneratedShaders shaders = generate_ff_shaders(state);
    ASSERT_TRUE(shaders.valid());
    EXPECT_NE(shaders.fragment_msl.find("texture_matrix[0]"), std::string::npos)
        << shaders.fragment_msl;
    EXPECT_NE(shaders.vertex_msl.find("float4 texcoord0"), std::string::npos)
        << shaders.vertex_msl;
    EXPECT_TRUE(ff_compiles(state));
}

TEST_F(GeneratedMslTest, FixedFunctionArgComplementAndAlphaReplicate) {
    // RCT3 lightmaps often use D3DTA_COMPLEMENT / D3DTA_ALPHAREPLICATE on
    // stage args; stripping those bits made modulate look neon/wrong.
    constexpr u32 kComplement = 0x10u;
    constexpr u32 kAlphaReplicate = 0x20u;
    FFPipelineState state{};
    state.fvf_flags = FVF_XYZ | FVF_DIFFUSE | FVF_TEX1;
    state.lighting_enabled = false;
    state.active_texture_stages = 1;
    state.texture_stages[0].color_op = TexStageOp::Modulate;
    state.texture_stages[0].color_arg1 =
        static_cast<u32>(TexStageArg::Texture) | kComplement;
    state.texture_stages[0].color_arg2 =
        static_cast<u32>(TexStageArg::Diffuse) | kAlphaReplicate;
    state.texture_stages[0].alpha_op = TexStageOp::SelectArg1;
    state.texture_stages[0].alpha_arg1 = static_cast<u32>(TexStageArg::Texture);

    const FFGeneratedShaders shaders = generate_ff_shaders(state);
    ASSERT_TRUE(shaders.valid());
    EXPECT_NE(shaders.fragment_msl.find("1.0) -"), std::string::npos)
        << shaders.fragment_msl;
    EXPECT_NE(shaders.fragment_msl.find(".aaaa"), std::string::npos)
        << shaders.fragment_msl;
    EXPECT_TRUE(ff_compiles(state));
}

TEST_F(GeneratedMslTest, FixedFunctionCameraSpaceTexgen) {
    FFPipelineState state{};
    state.fvf_flags = FVF_XYZ | FVF_NORMAL | FVF_TEX1;
    state.lighting_enabled = false;
    state.active_texture_stages = 1;
    state.texture_stages[0].color_op = TexStageOp::SelectArg1;
    state.texture_stages[0].color_arg1 = static_cast<u32>(TexStageArg::Texture);
    // D3DTSS_TCI_CAMERASPACENORMAL | texcoord set 0
    state.texture_stages[0].texcoord_index = 0x10000u;

    const FFGeneratedShaders shaders = generate_ff_shaders(state);
    ASSERT_TRUE(shaders.valid());
    EXPECT_NE(shaders.vertex_msl.find("tci_view_n"), std::string::npos)
        << shaders.vertex_msl;
    EXPECT_TRUE(ff_compiles(state));
}

TEST_F(GeneratedMslTest, FixedFunctionEveryCombinerOperation) {
    for (TexStageOp op : {TexStageOp::SelectArg1, TexStageOp::SelectArg2,
                          TexStageOp::Modulate, TexStageOp::Modulate2x,
                          TexStageOp::Modulate4x, TexStageOp::Add,
                          TexStageOp::AddSigned, TexStageOp::AddSigned2x,
                          TexStageOp::Subtract, TexStageOp::AddSmooth,
                          TexStageOp::BlendDiffuseAlpha,
                          TexStageOp::BlendTextureAlpha,
                          TexStageOp::BlendFactorAlpha,
                          TexStageOp::BlendTextureAlphaPM,
                          TexStageOp::BlendCurrentAlpha,
                          TexStageOp::ModulateAlphaAddColor,
                          TexStageOp::ModulateColorAddAlpha,
                          TexStageOp::ModulateInvAlphaAddColor,
                          TexStageOp::ModulateInvColorAddAlpha,
                          TexStageOp::MultiplyAdd, TexStageOp::Lerp}) {
        FFPipelineState state{};
        state.fvf_flags = FVF_XYZ | FVF_DIFFUSE | FVF_TEX1;
        state.lighting_enabled = false;
        state.active_texture_stages = 1;
        state.texture_stages[0].color_op = op;
        state.texture_stages[0].color_arg1 = static_cast<u32>(TexStageArg::Texture);
        state.texture_stages[0].color_arg2 = static_cast<u32>(TexStageArg::Diffuse);
        state.texture_stages[0].alpha_op = op;
        state.texture_stages[0].alpha_arg1 = static_cast<u32>(TexStageArg::Texture);
        state.texture_stages[0].alpha_arg2 = static_cast<u32>(TexStageArg::Current);
        EXPECT_TRUE(ff_compiles(state))
            << "combiner operation " << static_cast<int>(op);
    }
}

TEST_F(GeneratedMslTest, FixedFunctionLightingOfEveryType) {
    for (LightType type : {LightType::Directional, LightType::Point,
                           LightType::Spot}) {
        FFPipelineState state{};
        state.fvf_flags = FVF_XYZ | FVF_NORMAL;
        state.lighting_enabled = true;
        state.active_light_count = 1;
        state.lights[0].type = type;
        state.lights[0].enabled = true;
        state.specular_enable = true;
        EXPECT_TRUE(ff_compiles(state))
            << "light type " << static_cast<int>(type);
    }
}

TEST_F(GeneratedMslTest, FixedFunctionEightLights) {
    FFPipelineState state{};
    state.fvf_flags = FVF_XYZ | FVF_NORMAL;
    state.lighting_enabled = true;
    state.active_light_count = 8;
    for (auto& light : state.lights) {
        light.type = LightType::Point;
        light.enabled = true;
    }
    EXPECT_TRUE(ff_compiles(state));
}

TEST_F(GeneratedMslTest, FixedFunctionFogOfEveryMode) {
    for (FogMode mode : {FogMode::Linear, FogMode::Exp, FogMode::Exp2}) {
        FFPipelineState state{};
        state.fvf_flags = FVF_XYZ | FVF_DIFFUSE;
        state.lighting_enabled = false;
        state.fog_mode = mode;
        EXPECT_TRUE(ff_compiles(state))
            << "fog mode " << static_cast<int>(mode);
    }
}

TEST_F(GeneratedMslTest, FixedFunctionTableFogIsPerPixelZOrW) {
    FFPipelineState state{};
    state.fvf_flags = FVF_XYZ | FVF_DIFFUSE;
    state.lighting_enabled = false;
    state.fog_mode = FogMode::Linear;
    state.table_fog = true;

    FFGeneratedShaders shaders = generate_ff_shaders(state);
    ASSERT_TRUE(shaders.valid());
    EXPECT_NE(shaders.fragment_msl.find("float fog_distance = in.position.z;"),
              std::string::npos) << shaders.fragment_msl;
    EXPECT_NE(shaders.fragment_msl.find("float4 fog_params;"),
              std::string::npos) << shaders.fragment_msl;

    state.table_fog_w = true;
    shaders = generate_ff_shaders(state);
    ASSERT_TRUE(shaders.valid());
    EXPECT_NE(shaders.fragment_msl.find(
                  "1.0 / max(abs(in.position.w), 1e-6)"),
              std::string::npos) << shaders.fragment_msl;

    const std::string hybrid = generate_ff_fragment_for_sm3_varyings(
        state, {0}, {}, false);
    EXPECT_NE(hybrid.find("1.0 / max(abs(in.position.w), 1e-6)"),
              std::string::npos) << hybrid;
}

TEST_F(GeneratedMslTest, FixedFunctionAlphaTestOfEveryComparison) {
    for (u32 function = 1; function <= 8; ++function) {
        FFPipelineState state{};
        state.fvf_flags = FVF_XYZ | FVF_DIFFUSE;
        state.lighting_enabled = false;
        state.alpha_test_enable = true;
        state.alpha_test_func = function;
        state.alpha_ref = 128;
        EXPECT_TRUE(ff_compiles(state)) << "alpha function " << function;
    }
}

TEST_F(GeneratedMslTest, FixedFunctionEverythingAtOnce) {
    FFPipelineState state{};
    state.fvf_flags = FVF_XYZ | FVF_NORMAL | FVF_DIFFUSE | FVF_SPECULAR |
                      FVF_TEX2;
    state.lighting_enabled = true;
    state.active_light_count = 3;
    state.lights[0].type = LightType::Directional;
    state.lights[1].type = LightType::Point;
    state.lights[2].type = LightType::Spot;
    state.specular_enable = true;
    state.active_texture_stages = 2;
    state.texture_stages[0].color_op = TexStageOp::Modulate;
    state.texture_stages[1].color_op = TexStageOp::Add;
    state.texture_stages[1].texcoord_index = 1;
    state.fog_mode = FogMode::Exp2;
    state.alpha_test_enable = true;
    state.alpha_test_func = 5;
    EXPECT_TRUE(ff_compiles(state));
}

// =============================================================================
// Shader model 1–3 translator
// =============================================================================

namespace {

constexpr u32 DST_ALL = 0x80000000u | (0xFu << 16);

// D3DSI_GETREGTYPE splits the 5-bit register type: the low three bits live at
// 30:28 (D3DSP_REGTYPE_MASK) and the high two at 12:11 (D3DSP_REGTYPE_MASK2).
constexpr u32 register_bits(u32 type) {
    return ((type & 0x7) << 28) | (((type >> 3) & 0x3) << 11);
}
constexpr u32 dst(u32 type, u32 index, u32 mask = 0xF) {
    return 0x80000000u | register_bits(type) | index | ((mask & 0xF) << 16);
}
constexpr u32 src(u32 type, u32 index, u32 swizzle = 0xE4, u32 mod = 0) {
    return 0x80000000u | register_bits(type) | index | ((swizzle & 0xFF) << 16) |
           ((mod & 0xF) << 24);
}
constexpr u32 op(u32 opcode, u32 length) { return opcode | (length << 24); }
constexpr u32 END = 0x0000FFFF;

} // namespace

TEST_F(GeneratedMslTest, Sm3MinimalPair) {
    // vs_3_0: dcl_position v0; m4x4 oPos, v0, c0
    const std::vector<u32> vertex_tokens{
        0xFFFE0300u,
        op(31, 2), 0, dst(1, 0),
        op(20, 3), dst(4, 0), src(1, 0), src(2, 0),
        END,
    };
    // ps_3_0: mov oC0, c0
    const std::vector<u32> pixel_tokens{
        0xFFFF0300u,
        op(1, 2), dst(8, 0), src(2, 0),
        END,
    };
    const Sm3Shader vertex =
        translate_sm3_to_msl(vertex_tokens.data(), vertex_tokens.size());
    ASSERT_TRUE(vertex.ok) << vertex.failure;
    const Sm3Shader pixel =
        translate_sm3_to_msl(pixel_tokens.data(), pixel_tokens.size());
    ASSERT_TRUE(pixel.ok) << pixel.failure;

    EXPECT_TRUE(compiles(vertex.msl, pixel.msl, vertex.entry_point,
                         pixel.entry_point,
                         {{0, 0, BackendVertexFormat::Float3}}, 12));
}

TEST_F(GeneratedMslTest, Sm3TexturedPairWithVaryings) {
    // vs_3_0: dcl_position v0; dcl_texcoord0 v1; dcl_position o0;
    //         dcl_texcoord0 o1; m4x4 o0, v0, c0; mov o1, v1
    const std::vector<u32> vertex_tokens{
        0xFFFE0300u,
        op(31, 2), 0, dst(1, 0),
        op(31, 2), 5, dst(1, 1),
        op(31, 2), 0, dst(6, 0),
        op(31, 2), 5, dst(6, 1),
        op(20, 3), dst(6, 0), src(1, 0), src(2, 0),
        op(1, 2), dst(6, 1), src(1, 1),
        END,
    };
    // ps_3_0: dcl_2d s0; dcl_texcoord0 v1; texld r0, v1, s0; mov oC0, r0
    const std::vector<u32> pixel_tokens{
        0xFFFF0300u,
        op(31, 2), (2u << 27), dst(10, 0),
        op(31, 2), 5, dst(1, 1),
        op(66, 3), dst(0, 0), src(1, 1), src(10, 0),
        op(1, 2), dst(8, 0), src(0, 0),
        END,
    };
    const Sm3Shader vertex =
        translate_sm3_to_msl(vertex_tokens.data(), vertex_tokens.size());
    ASSERT_TRUE(vertex.ok) << vertex.failure;
    const Sm3Shader pixel =
        translate_sm3_to_msl(pixel_tokens.data(), pixel_tokens.size());
    ASSERT_TRUE(pixel.ok) << pixel.failure;

    EXPECT_TRUE(compiles(vertex.msl, pixel.msl, vertex.entry_point,
                         pixel.entry_point,
                         {{0, 0, BackendVertexFormat::Float3},
                          {1, 12, BackendVertexFormat::Float2}},
                         20));
}

TEST_F(GeneratedMslTest, Sm3EveryTranslatedArithmeticOpcode) {
    // One pixel shader exercising every arithmetic form the translator emits,
    // so a malformed expression for any of them fails to compile here.
    std::vector<u32> tokens{0xFFFF0300u};
    const auto binary = [&](u32 opcode) {
        tokens.insert(tokens.end(),
                      {op(opcode, 3), dst(0, 0), src(2, 0), src(2, 1)});
    };
    const auto unary = [&](u32 opcode) {
        tokens.insert(tokens.end(), {op(opcode, 2), dst(0, 0), src(2, 0)});
    };
    const auto ternary = [&](u32 opcode) {
        tokens.insert(tokens.end(), {op(opcode, 4), dst(0, 0), src(2, 0),
                                     src(2, 1), src(2, 2)});
    };

    unary(1);   // mov
    unary(19);  // frc
    unary(35);  // abs
    unary(36);  // nrm
    unary(6);   // rcp
    unary(7);   // rsq
    unary(14);  // exp
    unary(15);  // log
    unary(78);  // expp
    unary(79);  // logp
    // sgn dest, src0, src1, src2 — src1/src2 are scratch temps.
    tokens.insert(tokens.end(), {op(34, 4), dst(0, 0), src(2, 0), src(0, 1),
                                 src(0, 2)});
    binary(2);  // add
    binary(3);  // sub
    binary(5);  // mul
    binary(10); // min
    binary(11); // max
    binary(12); // slt
    binary(13); // sge
    binary(32); // pow
    binary(8);  // dp3
    binary(9);  // dp4
    ternary(4);  // mad
    ternary(18); // lrp
    ternary(88); // cmp
    ternary(80); // cnd
    ternary(90); // dp2add
    // crs needs a three-component destination.
    tokens.insert(tokens.end(),
                  {op(33, 3), dst(0, 0, 0x7), src(2, 0), src(2, 1)});
    // Every matrix form.
    for (u32 opcode : {20u, 21u, 22u, 23u, 24u}) {
        tokens.insert(tokens.end(),
                      {op(opcode, 3), dst(0, 1), src(2, 0), src(2, 4)});
    }
    tokens.insert(tokens.end(), {op(1, 2), dst(8, 0), src(0, 0), END});

    const Sm3Shader shader =
        translate_sm3_to_msl(tokens.data(), tokens.size());
    ASSERT_TRUE(shader.ok) << shader.failure;

    const std::vector<u32> vertex_tokens{
        0xFFFE0300u,
        op(31, 2), 0, dst(1, 0),
        op(1, 2), dst(4, 0), src(1, 0),
        END,
    };
    const Sm3Shader vertex =
        translate_sm3_to_msl(vertex_tokens.data(), vertex_tokens.size());
    ASSERT_TRUE(vertex.ok) << vertex.failure;

    EXPECT_TRUE(compiles(vertex.msl, shader.msl, vertex.entry_point,
                         shader.entry_point,
                         {{0, 0, BackendVertexFormat::Float3}}, 12));
}

TEST_F(GeneratedMslTest, Sm3SourceModifiersAndSaturate) {
    std::vector<u32> tokens{0xFFFF0300u};
    for (u32 modifier : {0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u, 11u, 12u}) {
        tokens.insert(tokens.end(),
                      {op(1, 2), dst(0, 0), src(2, 0, 0xE4, modifier)});
    }
    // mov_sat r0, c0
    tokens.insert(tokens.end(),
                  {op(1, 2), DST_ALL | (1u << 20), src(2, 0)});
    tokens.insert(tokens.end(), {op(1, 2), dst(8, 0), src(0, 0), END});

    const Sm3Shader pixel = translate_sm3_to_msl(tokens.data(), tokens.size());
    ASSERT_TRUE(pixel.ok) << pixel.failure;

    const std::vector<u32> vertex_tokens{
        0xFFFE0300u,
        op(31, 2), 0, dst(1, 0),
        op(1, 2), dst(4, 0), src(1, 0),
        END,
    };
    const Sm3Shader vertex =
        translate_sm3_to_msl(vertex_tokens.data(), vertex_tokens.size());
    ASSERT_TRUE(vertex.ok) << vertex.failure;

    EXPECT_TRUE(compiles(vertex.msl, pixel.msl, vertex.entry_point,
                         pixel.entry_point,
                         {{0, 0, BackendVertexFormat::Float3}}, 12));
}

TEST_F(GeneratedMslTest, Sm3PartialWriteMasksAndSwizzles) {
    std::vector<u32> tokens{0xFFFF0300u};
    for (u32 mask = 1; mask <= 0xF; ++mask) {
        tokens.insert(tokens.end(),
                      {op(2, 3), dst(0, 0, mask), src(2, 0, 0x1B),
                       src(2, 1, 0x00)});
    }
    tokens.insert(tokens.end(), {op(1, 2), dst(8, 0), src(0, 0), END});

    const Sm3Shader pixel = translate_sm3_to_msl(tokens.data(), tokens.size());
    ASSERT_TRUE(pixel.ok) << pixel.failure;

    const std::vector<u32> vertex_tokens{
        0xFFFE0300u,
        op(31, 2), 0, dst(1, 0),
        op(1, 2), dst(4, 0), src(1, 0),
        END,
    };
    const Sm3Shader vertex =
        translate_sm3_to_msl(vertex_tokens.data(), vertex_tokens.size());
    ASSERT_TRUE(vertex.ok) << vertex.failure;

    EXPECT_TRUE(compiles(vertex.msl, pixel.msl, vertex.entry_point,
                         pixel.entry_point,
                         {{0, 0, BackendVertexFormat::Float3}}, 12));
}

TEST_F(GeneratedMslTest, Sm3DefinedConstantsAndTexkill) {
    // ps_3_0: dcl_2d s0; dcl_texcoord0 v0; def c1, ...; texkill v0;
    //         texld r0, v0, s0; mul r0, r0, c1; mov oC0, r0
    const std::vector<u32> pixel_tokens{
        0xFFFF0300u,
        op(31, 2), (2u << 27), dst(10, 0),
        op(31, 2), 5, dst(1, 0),
        op(81, 5), dst(2, 1), 0x3E800000u, 0x3F000000u, 0x3F400000u,
        0x3F800000u,
        op(65, 1), dst(1, 0),
        op(66, 3), dst(0, 0), src(1, 0), src(10, 0),
        op(5, 3), dst(0, 0), src(0, 0), src(2, 1),
        op(1, 2), dst(8, 0), src(0, 0),
        END,
    };
    const Sm3Shader pixel =
        translate_sm3_to_msl(pixel_tokens.data(), pixel_tokens.size());
    ASSERT_TRUE(pixel.ok) << pixel.failure;

    const std::vector<u32> vertex_tokens{
        0xFFFE0300u,
        op(31, 2), 0, dst(1, 0),
        op(31, 2), 5, dst(1, 1),
        op(31, 2), 5, dst(6, 0),
        op(1, 2), dst(4, 0), src(1, 0),
        op(1, 2), dst(6, 0), src(1, 1),
        END,
    };
    const Sm3Shader vertex =
        translate_sm3_to_msl(vertex_tokens.data(), vertex_tokens.size());
    ASSERT_TRUE(vertex.ok) << vertex.failure;

    EXPECT_TRUE(compiles(vertex.msl, pixel.msl, vertex.entry_point,
                         pixel.entry_point,
                         {{0, 0, BackendVertexFormat::Float3},
                          {1, 12, BackendVertexFormat::Float2}},
                         20));
}

TEST_F(GeneratedMslTest, Sm3RelativeConstantLoadCompiles) {
    // D3D token encoding: vs_1_1 `mov a0.x, v0` + `mov oPos, c[a0.x]`.
    // Metal must accept sm3_c(), which indexes the constant file in a helper.
    const auto d3d_reg = [](u32 type) {
        return ((type & 0x7) << 28) | (((type >> 3) & 0x3) << 11);
    };
    const auto d3d_dst = [&](u32 type, u32 index, u32 mask = 0xF) {
        return 0x80000000u | d3d_reg(type) | index | ((mask & 0xF) << 16);
    };
    const auto d3d_src = [&](u32 type, u32 index, u32 swizzle = 0xE4) {
        return 0x80000000u | d3d_reg(type) | index |
               ((swizzle & 0xFF) << 16);
    };
    constexpr u32 ADDR = 3;
    constexpr u32 RASTOUT = 4;
    constexpr u32 INPUT = 1;
    constexpr u32 CONST = 2;
    constexpr u32 COLOROUT = 8;
    const std::vector<u32> vertex_tokens{
        0xFFFE0101u,
        op(31, 2), 0, d3d_dst(INPUT, 0),
        op(1, 2), d3d_dst(ADDR, 0, 0x1), d3d_src(INPUT, 0),
        op(1, 2), d3d_dst(RASTOUT, 0), d3d_src(CONST, 0) | (1u << 13),
        END,
    };
    const std::vector<u32> pixel_tokens{
        0xFFFF0300u,
        op(1, 2), d3d_dst(COLOROUT, 0), d3d_src(CONST, 0),
        END,
    };
    const Sm3Shader vertex =
        translate_sm3_to_msl(vertex_tokens.data(), vertex_tokens.size());
    ASSERT_TRUE(vertex.ok) << vertex.failure;
    const Sm3Shader pixel =
        translate_sm3_to_msl(pixel_tokens.data(), pixel_tokens.size());
    ASSERT_TRUE(pixel.ok) << pixel.failure;
    EXPECT_NE(vertex.msl.find("sm3_c(cb,"), std::string::npos) << vertex.msl;
    EXPECT_TRUE(compiles(vertex.msl, pixel.msl, vertex.entry_point,
                         pixel.entry_point,
                         {{0, 0, BackendVertexFormat::Float3}}, 12));
}

// =============================================================================
// Geometry shader emulation
// =============================================================================
//
// Metal has no geometry stage, so a GS is lowered to a compute kernel that
// expands primitives into a buffer. Nothing short of the real Metal compiler
// proves the lowered module is well-formed: SPIRV-Cross will happily produce
// MSL that Metal then rejects, and at runtime that surfaces as a pipeline
// that fails to build, one step removed from the translator that caused it.
//
// The fixtures are real FXC gs_4_0 token streams, so this is
// DXBC -> SPIR-V -> MSL -> Metal end to end.

namespace {

/// The point-stream fixture from test_dxbc_to_spirv.cpp: point input,
/// PointStream output, maxvertexcount(1), copies SV_Position and COLOR, then
/// Append() and RestartStrip(). Kept as its own copy rather than shared,
/// because a fixture shared between a translation test and a GPU test drifts
/// to whichever one is edited next.
[[nodiscard]] DxbcContainer fxc_point_geometry_shader() {
    DxbcContainer container{};
    container.shader_model_major = 4;
    container.shader_model_minor = 0;
    container.stage = ShaderStage::Geometry;
    container.input_signature.push_back({"SV_Position", 0, 0, 0xFu, 3, 1u});
    container.input_signature.push_back({"COLOR", 0, 1, 0xFu, 3});
    container.output_signature.push_back({"SV_Position", 0, 0, 0xFu, 3, 1u});
    container.output_signature.push_back({"COLOR", 0, 1, 0xFu, 3});
    const u32 words[] = {
        0x00020040u, 0x00000025u,
        0x05000061u, 0x002010F2u, 1u, 0u, 1u,
        0x0400005Fu, 0x002010F2u, 1u, 1u,
        0x0100085Du,  // dcl_inputPrimitive point
        0x0100085Cu,  // dcl_outputTopology pointlist
        0x04000067u, 0x001020F2u, 0u, 1u,
        0x03000065u, 0x001020F2u, 1u,
        0x0200005Eu, 1u,  // dcl_maxOutputVertexCount 1
        0x06000036u, 0x001020F2u, 0u, 0x00201E46u, 0u, 0u,
        0x06000036u, 0x001020F2u, 1u, 0x00201E46u, 0u, 1u,
        0x01000013u,  // emit
        0x01000009u,  // cut
        0x0100003Eu,  // ret
    };
    for (const u32 word : words) {
        const auto* bytes = reinterpret_cast<const u8*>(&word);
        container.bytecode.insert(container.bytecode.end(), bytes, bytes + 4);
    }
    return container;
}

/// The strip fixture, parameterized by input width and output topology. The
/// triangle-strip case is the one worth putting on the GPU: it is the only
/// path with a conditional append and the OpSelect that alternates winding.
[[nodiscard]] DxbcContainer fxc_strip_geometry_shader(u32 primitive_width,
                                                     u32 output_topology) {
    DxbcContainer container{};
    container.shader_model_major = 4;
    container.shader_model_minor = 0;
    container.stage = ShaderStage::Geometry;
    container.input_signature.push_back({"SV_Position", 0, 0, 0xFu, 3, 1u});
    container.input_signature.push_back({"COLOR", 0, 1, 0xFu, 3});
    container.output_signature.push_back({"SV_Position", 0, 0, 0xFu, 3, 1u});
    container.output_signature.push_back({"COLOR", 0, 1, 0xFu, 3});
    std::vector<u32> words{
        0x00020040u, 24u + 13u * primitive_width,
        0x05000061u, 0x002010F2u, primitive_width, 0u, 1u,
        0x0400005Fu, 0x002010F2u, primitive_width, 1u,
        0x0100005Du | (primitive_width << 11),
        0x0100005Cu | (output_topology << 11),
        0x04000067u, 0x001020F2u, 0u, 1u,
        0x03000065u, 0x001020F2u, 1u,
        0x0200005Eu, primitive_width,
    };
    for (u32 vertex = 0; vertex < primitive_width; ++vertex) {
        for (const u32 word : {0x06000036u, 0x001020F2u, 0u,
                               0x00201E46u, vertex, 0u,
                               0x06000036u, 0x001020F2u, 1u,
                               0x00201E46u, vertex, 1u,
                               0x01000013u}) {
            words.push_back(word);
        }
    }
    words.push_back(0x01000009u);  // cut
    words.push_back(0x0100003Eu);  // ret
    for (const u32 word : words) {
        const auto* bytes = reinterpret_cast<const u8*>(&word);
        container.bytecode.insert(container.bytecode.end(), bytes, bytes + 4);
    }
    return container;
}

/// Lower a geometry shader and hand the result to the real Metal compiler.
[[nodiscard]] bool lowered_gs_compiles(const DxbcContainer& container,
                                       std::string& msl_out,
                                       std::string& why_not) {
    DxbcTranslationOptions options{};
    options.emulate_geometry_with_compute = true;
    const auto spirv = translate_dxbc_to_spirv(container, options);
    if (!spirv.valid()) {
        why_not = "DXBC -> SPIR-V failed: " + spirv.failure;
        return false;
    }
    if (!spirv.gs_lowered_to_compute ||
        spirv.stage != ShaderStage::Compute) {
        why_not = "the shader was not lowered to compute";
        return false;
    }
    MslCompileOptions msl_options{};
    msl_options.argument_buffers = false;
    const auto compiled = compile_spirv_to_msl(
        spirv.spirv.data(), spirv.spirv.size(), ShaderStage::Compute,
        msl_options);
    if (!compiled.valid()) {
        why_not = "SPIR-V -> MSL failed";
        return false;
    }
    msl_out = compiled.msl_text();

    BackendComputePipelineDesc desc{};
    desc.compute_shader_msl = msl_out;
    // SPIRV-Cross renames the SPIR-V "main" because MSL reserves it; the
    // D3D11 compute path names the same entry point.
    desc.compute_entry_point = "main0";
    desc.cache_key = 0x6500DEu;
    auto pipeline = MetalDevice::instance().create_compute_pipeline(desc);
    if (pipeline == nullptr || !pipeline->valid()) {
        why_not = "Metal rejected the lowered geometry shader";
        return false;
    }
    return true;
}

}  // namespace

TEST_F(GeneratedMslTest, LoweredGeometryShaderCompilesAsAComputeKernel) {
    std::string msl;
    std::string why_not;
    ASSERT_TRUE(lowered_gs_compiles(fxc_point_geometry_shader(), msl, why_not))
        << why_not << "\n--- MSL ---\n" << msl;
    EXPECT_NE(msl.find("kernel void"), std::string::npos) << msl;
    // The three expansion buffers must reach the kernel as arguments. Leaving
    // them out of the entry-point interface produced MSL that referenced them
    // without declaring them, which is how this test earned its keep.
    EXPECT_NE(msl.find("vitreo_gs_in"), std::string::npos) << msl;
    EXPECT_NE(msl.find("vitreo_gs_out"), std::string::npos) << msl;
    EXPECT_NE(msl.find("vitreo_gs_counter"), std::string::npos) << msl;
    // ...at the window the backend binds them at. SPIRV-Cross assigns indices
    // of its own choosing for any descriptor set the flat binder skips, and
    // those can land anywhere in the b#/t#/u# windows.
    EXPECT_NE(msl.find("vitreo_gs_in [[buffer(" +
                       std::to_string(BACKEND_GS_EXPANSION_BUFFER_BASE) + ")]]"),
              std::string::npos) << msl;
    EXPECT_NE(msl.find("vitreo_gs_out [[buffer(" +
                       std::to_string(BACKEND_GS_EXPANSION_BUFFER_BASE + 1) + ")]]"),
              std::string::npos) << msl;
    EXPECT_NE(msl.find("vitreo_gs_counter [[buffer(" +
                       std::to_string(BACKEND_GS_EXPANSION_BUFFER_BASE + 2) + ")]]"),
              std::string::npos) << msl;
    EXPECT_NE(msl.find("vitreo_gs_index [[buffer(" +
                       std::to_string(BACKEND_GS_EXPANSION_BUFFER_BASE + 3) + ")]]"),
              std::string::npos) << msl;
    // A compute kernel has no stage output struct. A surviving BuiltIn
    // Position made SPIRV-Cross emit one and Metal rejected the kernel.
    EXPECT_EQ(msl.find("gl_Position"), std::string::npos) << msl;
}

TEST_F(GeneratedMslTest, LoweredTriangleStripGeometryShaderCompiles) {
    // The triangle path is the one with a conditional append and the
    // winding OpSelect, so it is the one most likely to produce a module that
    // SPIRV-Cross accepts and Metal does not.
    std::string msl;
    std::string why_not;
    ASSERT_TRUE(lowered_gs_compiles(
        fxc_strip_geometry_shader(/*D3D_PRIMITIVE_TRIANGLE=*/3u,
                                  /*TOPOLOGY_TRIANGLESTRIP=*/5u),
        msl, why_not))
        << why_not << "\n--- MSL ---\n" << msl;
    // Per-invocation regions: no shared atomic cursor (which would order the
    // output by GPU scheduling instead of by input primitive).
    EXPECT_EQ(msl.find("atomic_fetch_add_explicit"), std::string::npos) << msl;
    EXPECT_EQ(msl.find("gl_Position"), std::string::npos) << msl;
}

TEST_F(GeneratedMslTest, LoweredLineStripGeometryShaderCompiles) {
    std::string msl;
    std::string why_not;
    ASSERT_TRUE(lowered_gs_compiles(
        fxc_strip_geometry_shader(/*D3D_PRIMITIVE_LINE=*/2u,
                                  /*TOPOLOGY_LINESTRIP=*/3u),
        msl, why_not))
        << why_not << "\n--- MSL ---\n" << msl;
    EXPECT_EQ(msl.find("gl_Position"), std::string::npos) << msl;
}

TEST_F(GeneratedMslTest, GsPassthroughVertexShaderCompilesAndLinks) {
    // Pass C of the geometry emulation. It must not only compile on its own --
    // it has to LINK with a fragment shader whose inputs came from the guest's
    // pixel shader, which means matching user(locnN) indices and types. A
    // pipeline build is the only thing that checks the linkage.
    const std::vector<GsPassthroughOutput> signature{
        {0u, 3u, /*is_position=*/true},
        {1u, 3u, false},
        {2u, 1u, false},   // a uint varying: Metal makes it flat implicitly
    };
    const std::string vertex_msl =
        generate_gs_passthrough_vertex_msl(signature, 6u);
    ASSERT_FALSE(vertex_msl.empty());

    // A fragment shader shaped like the guest's: same locations, same types.
    const std::string fragment_msl = R"(#include <metal_stdlib>
using namespace metal;

struct in_t {
    float4 position [[position]];
    float4 locn1 [[user(locn1)]];
    uint4 locn2 [[user(locn2)]];
};

fragment float4 fragment_main(in_t in [[stage_in]])
{
    return in.locn1 + float4(in.locn2);
}
)";

    BackendPipelineDesc desc{};
    desc.vertex_shader_msl = vertex_msl;
    desc.fragment_shader_msl = fragment_msl;
    desc.vertex_entry_point = kGsPassthroughEntryPoint;
    desc.fragment_entry_point = "fragment_main";
    // Pass C binds no vertex buffers and needs no input layout: every vertex
    // is read from gs_out by vertex_id.
    desc.vertex_attributes = {};
    desc.vertex_stride = 0;
    desc.color_format = PixelFormat::BGRA8Unorm;
    desc.cache_key = 0x6503C0u;

    auto pipeline = MetalDevice::instance().create_pipeline(desc);
    const bool ok = pipeline != nullptr && pipeline->valid();
    if (!ok) {
        ADD_FAILURE() << "Metal rejected the pass-through pipeline.\n"
                      << "--- vertex ---\n" << vertex_msl
                      << "\n--- fragment ---\n" << fragment_msl;
    }

    // gs_out must arrive at the vertex stage's t0 structured-SRV index, which
    // is what the draw call binds it at.
    EXPECT_NE(vertex_msl.find("[[buffer(" +
                              std::to_string(BACKEND_SRV_BUFFER_BASE) + ")]]"),
              std::string::npos) << vertex_msl;
    // The record stride must be the register count, matching what the
    // expansion kernel writes.
    EXPECT_NE(vertex_msl.find("vitreo_vertex_id * 3u"), std::string::npos)
        << vertex_msl;
}

TEST_F(GeneratedMslTest, GsPassthroughOfPointsWritesAOnePixelPointSize) {
    // Metal leaves a point's size undefined unless the vertex stage writes
    // [[point_size]]; D3D11.3 3.4.4 rasterizes every point as one pixel. A
    // tessellator or geometry shader with point output drew garbage-sized
    // squares (a51 ts_indirect: 228 lit pixels where Windows lights 21).
    const std::vector<GsPassthroughOutput> signature{
        {0u, 3u, /*is_position=*/true},
        {1u, 3u, false},
    };
    for (const bool dense : {false, true}) {
        const std::string points = generate_gs_passthrough_vertex_msl(
            signature, dense ? 0u : 1u, /*fixed_function_push=*/true, dense,
            /*primitive_vertices=*/1u);
        ASSERT_FALSE(points.empty());
        EXPECT_NE(points.find("[[point_size]]"), std::string::npos) << points;
        EXPECT_NE(points.find("out.vitreo_point_size = 1.0;"), std::string::npos)
            << points;
        BackendPipelineDesc desc{};
        desc.vertex_shader_msl = points;
        desc.fragment_shader_msl = R"(#include <metal_stdlib>
using namespace metal;
struct in_t { float4 position [[position]]; float4 locn1 [[user(locn1)]]; };
fragment float4 fragment_main(in_t in [[stage_in]]) { return in.locn1; }
)";
        desc.vertex_entry_point = kGsPassthroughEntryPoint;
        desc.fragment_entry_point = "fragment_main";
        desc.color_format = PixelFormat::BGRA8Unorm;
        desc.cache_key = dense ? 0x6503C2u : 0x6503C1u;
        auto pipeline = MetalDevice::instance().create_pipeline(desc);
        EXPECT_TRUE(pipeline != nullptr && pipeline->valid()) << points;
    }
    // Lines and triangles have no point size to give.
    for (const u32 vertices : {2u, 3u}) {
        const std::string msl = generate_gs_passthrough_vertex_msl(
            signature, 3u, true, false, vertices);
        EXPECT_EQ(msl.find("[[point_size]]"), std::string::npos) << msl;
    }
}

TEST_F(GeneratedMslTest, GsPassthroughCullDistanceCompilesAndTestsEveryVertex) {
    // D3D11.3 15.4.2: a primitive is discarded when one cull distance is
    // negative (or NaN) at all of its vertices. o1.x and o1.z carry cull
    // distances of triangles (3 vertices per primitive).
    GsPassthroughOutput cull{1u, 3u, false};
    cull.component_system_values = {3u, 0u, 3u, 0u};
    const std::vector<GsPassthroughOutput> signature{
        {0u, 3u, /*is_position=*/true}, cull};
    const std::string vertex_msl = generate_gs_passthrough_vertex_msl(
        signature, 6u, /*fixed_function_push=*/false, /*dense_records=*/false,
        /*primitive_vertices=*/3u);
    ASSERT_FALSE(vertex_msl.empty());
    // Both components, each over the primitive's three records.
    for (const char* lane : {"].x)", "].z)"}) {
        usize count = 0;
        for (usize at = vertex_msl.find(lane); at != std::string::npos;
             at = vertex_msl.find(lane, at + 1)) {
            ++count;
        }
        EXPECT_EQ(count, 3u) << lane << "\n" << vertex_msl;
    }
    EXPECT_NE(vertex_msl.find("% 3u"), std::string::npos) << vertex_msl;

    const std::string fragment_msl = R"(#include <metal_stdlib>
using namespace metal;

struct in_t {
    float4 position [[position]];
    float4 locn1 [[user(locn1)]];
};

fragment float4 fragment_main(in_t in [[stage_in]])
{
    return in.locn1;
}
)";
    BackendPipelineDesc desc{};
    desc.vertex_shader_msl = vertex_msl;
    desc.fragment_shader_msl = fragment_msl;
    desc.vertex_entry_point = kGsPassthroughEntryPoint;
    desc.fragment_entry_point = "fragment_main";
    desc.vertex_attributes = {};
    desc.vertex_stride = 0;
    desc.color_format = PixelFormat::BGRA8Unorm;
    desc.cache_key = 0x6503C1u;
    auto pipeline = MetalDevice::instance().create_pipeline(desc);
    EXPECT_TRUE(pipeline != nullptr && pipeline->valid()) << vertex_msl;

    // Dense records (a domain shader's points through an index list) and an
    // unknown primitive size cannot cull: the caller refuses those.
    EXPECT_TRUE(generate_gs_passthrough_vertex_msl(signature, 0u, false,
                                                   /*dense_records=*/true, 3u)
                    .empty());
    EXPECT_TRUE(generate_gs_passthrough_vertex_msl(signature, 6u).empty());
}

TEST_F(GeneratedMslTest, DenseRecordsThroughTheIndexListCullWholePrimitives) {
    // A compute-lowered domain shader's records drawn NON-indexed, one vertex
    // per entry of the tessellator's index list (at t1), so each vertex sees
    // its primitive. D3D11.3 15.4.2: a primitive is discarded when a cull
    // distance is < 0 at all of its vertices; a NaN distance counts as < 0;
    // -0.0 is not < 0.
    GsPassthroughOutput cull{1u, 3u, false};
    cull.component_system_values = {3u, 0u, 0u, 0u};
    const std::vector<GsPassthroughOutput> signature{
        {0u, 3u, /*is_position=*/true}, cull};
    const std::string vertex_msl = generate_gs_passthrough_vertex_msl(
        signature, 0u, /*fixed_function_push=*/false, /*dense_records=*/true,
        /*primitive_vertices=*/3u, /*records_through_index_list=*/true);
    ASSERT_FALSE(vertex_msl.empty());
    EXPECT_NE(vertex_msl.find("vitreo_gs_counts[vitreo_vertex_id] * 2u"),
              std::string::npos)
        << vertex_msl;
    // Without cull distances the indexed draw's shader is unchanged.
    EXPECT_EQ(generate_gs_passthrough_vertex_msl({{0u, 3u, true}}, 0u, true,
                                                 true),
              generate_gs_passthrough_vertex_msl({{0u, 3u, true}}, 0u, true,
                                                 true, 0u, false));

    auto& device = MetalDevice::instance();
    BackendPipelineDesc desc{};
    desc.vertex_shader_msl = vertex_msl;
    desc.vertex_entry_point = kGsPassthroughEntryPoint;
    desc.fragment_shader_msl = R"(#include <metal_stdlib>
using namespace metal;
struct In { float4 position [[position]]; };
fragment float4 main0(In in [[stage_in]]) { return float4(0, 1, 0, 1); }
)";
    desc.fragment_entry_point = "main0";
    desc.color_format = PixelFormat::BGRA8Unorm;
    desc.cache_key = 0x6503C2u;
    auto pipeline = device.create_pipeline(desc);
    ASSERT_TRUE(pipeline && pipeline->valid()) << vertex_msl;

    // Four triangles, one per pixel of a 4x1 target. The records are stored
    // in reverse primitive order, and the index list puts them back, so the
    // shader must read the list both for its own record and its primitive's.
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const std::array<std::array<float, 3>, 4> distances{{
        {-1.0f, -1.0f, -1.0f},  // all < 0: culled
        {-1.0f, 1.0f, -1.0f},   // one in: drawn
        {nan, -1.0f, -1.0f},    // NaN counts as < 0: culled
        {-0.0f, -1.0f, -1.0f},  // -0.0 is not < 0: drawn
    }};
    std::vector<float> records;   // [pos, (cull, 0, 0, 0)] per record
    std::vector<u32> list;
    for (u32 slot = 0; slot < 4; ++slot) {
        const u32 primitive = 3u - slot;  // stored in reverse
        const float cx = -1.0f + 0.5f * static_cast<float>(primitive) + 0.25f;
        const std::array<std::array<float, 2>, 3> corners{
            {{cx - 0.2f, -0.9f}, {cx + 0.2f, -0.9f}, {cx, 0.9f}}};
        for (u32 k = 0; k < 3; ++k) {
            records.insert(records.end(),
                           {corners[k][0], corners[k][1], 0.5f, 1.0f,
                            distances[primitive][k], 0.0f, 0.0f, 0.0f});
        }
    }
    for (u32 primitive = 0; primitive < 4; ++primitive) {
        const u32 slot = 3u - primitive;
        // Within a primitive, the corners in reverse too.
        for (u32 k = 0; k < 3; ++k) list.push_back(slot * 3u + (2u - k));
    }
    // Reversing the corners flips the winding the records carry; the
    // pipeline does not cull by facing.
    BackendBufferDesc storage{};
    storage.shader_resource = true;
    storage.byte_width = records.size() * sizeof(float);
    auto record_buffer = device.create_buffer(storage, records.data());
    storage.byte_width = list.size() * sizeof(u32);
    auto list_buffer = device.create_buffer(storage, list.data());
    ASSERT_TRUE(record_buffer && list_buffer);

    BackendTexture2DDesc td{};
    td.width = 4;
    td.height = 1;
    td.mip_levels = td.array_size = 1;
    td.pixel_format = PixelFormat::BGRA8Unorm;
    td.render_target = true;
    auto target = device.create_texture_2d(td, nullptr, 0);
    ASSERT_TRUE(target->clear_render_target({1.0f, 0.0f, 0.0f, 1.0f}));
    BackendRenderPassDesc pass_desc{};
    pass_desc.color_targets[0] = target.get();
    pass_desc.color_formats[0] = PixelFormat::BGRA8Unorm;
    pass_desc.preserve_color_contents = true;
    {
        auto pass = device.create_render_pass_standalone(pass_desc);
        BackendDrawCall call{};
        call.pipeline = pipeline.get();
        call.topology = BackendPrimitiveTopology::TriangleList;
        call.vertex_count = static_cast<u32>(list.size());
        call.vertex_srv_buffers[0] = record_buffer.get();
        call.vertex_srv_buffers[1] = list_buffer.get();
        call.viewport = {0, 0, 4, 1, 0, 1};
        ASSERT_TRUE(pass->draw(call));
        ASSERT_TRUE(pass->end());
    }
    std::array<u8, 16> pixels{};
    ASSERT_TRUE(target->readback(0, pixels.data(), 16));
    const std::array<bool, 4> drawn{false, true, false, true};
    for (usize p = 0; p < 4; ++p) {
        // BGRA8: green where drawn, the red clear where culled.
        EXPECT_EQ(pixels[p * 4 + 1], drawn[p] ? 255u : 0u) << "pixel " << p;
        EXPECT_EQ(pixels[p * 4 + 2], drawn[p] ? 0u : 255u) << "pixel " << p;
    }
}

TEST_F(GeneratedMslTest, GsPassthroughRefusesASignatureWithNoPosition) {
    // A draw with no clip-space output cannot rasterise. Generating a shader
    // that left position at zero would draw a degenerate primitive instead of
    // failing, which is the direction the emulation design rejects.
    EXPECT_TRUE(
        generate_gs_passthrough_vertex_msl({{0u, 3u, false}}, 3u).empty());
    EXPECT_TRUE(generate_gs_passthrough_vertex_msl({}, 3u).empty());
    // Nor when an invocation can complete no primitive at all.
    EXPECT_TRUE(
        generate_gs_passthrough_vertex_msl({{0u, 3u, true}}, 0u).empty());
    // Dense records (a compute-lowered domain shader's, one per tessellated
    // point, every one written) are the exception: nothing to cull, so no
    // per-invocation count is read.
    const std::string dense = generate_gs_passthrough_vertex_msl(
        {{0u, 3u, true}}, 0u, /*fixed_function_push=*/true,
        /*dense_records=*/true);
    ASSERT_FALSE(dense.empty());
    EXPECT_EQ(dense.find("const uint invocation"), std::string::npos) << dense;
    EXPECT_NE(dense.find("vitreo_vertex_id * 1u"), std::string::npos) << dense;
}

TEST_F(GeneratedMslTest, CaptureVertexShaderBuildsWithRasterizationDisabled) {
    // Pass A of the geometry emulation: the vertex shader runs only to write
    // its outputs into the expansion buffer. Rasterising those primitives
    // would draw the UNEXPANDED geometry over the frame, so the pipeline
    // discards them before the rasteriser -- which also means it has no
    // fragment function and no attachments.
    const std::string vertex_msl = R"(#include <metal_stdlib>
using namespace metal;

// Metal requires a void return when rasterization is disabled, which is why
// the translator stages a capture shader's outputs in Private variables: an
// entry point with no Output variables is what makes SPIRV-Cross emit void.
vertex void vertex_main(uint vid [[vertex_id]],
                        device float4* gs_in [[buffer()" +
        std::to_string(BACKEND_VERTEX_CAPTURE_BUFFER_INDEX) + R"()]])
{
    gs_in[vid] = float4(float(vid), 0.0, 0.0, 1.0);
}
)";
    BackendPipelineDesc desc{};
    desc.vertex_shader_msl = vertex_msl;
    desc.vertex_entry_point = "vertex_main";
    desc.rasterization_enabled = false;
    desc.color_format = PixelFormat::Unknown;
    desc.depth_attachment = false;
    desc.cache_key = 0x65CA97u;

    auto pipeline = MetalDevice::instance().create_pipeline(desc);
    EXPECT_TRUE(pipeline != nullptr && pipeline->valid())
        << "Metal rejected the rasterization-disabled capture pipeline";
}

namespace {

/// vs_4_0: `mov o0, v0; mov o1, v0` with v0 a float4 POSITION attribute, so
/// every vertex's record is two copies of what vertex fetch delivered.
[[nodiscard]] DxbcContainer fxc_fetching_vertex_shader() {
    DxbcContainer container{};
    container.shader_model_major = 4;
    container.shader_model_minor = 0;
    container.stage = ShaderStage::Vertex;
    container.input_signature.push_back({"POSITION", 0, 0, 0xFu, 3});
    container.output_signature.push_back({"SV_Position", 0, 0, 0xFu, 3, 1u});
    container.output_signature.push_back({"COLOR", 0, 1, 0xFu, 3});
    const std::vector<u32> words{
        0x00010040u, 23u,
        0x0300005Fu, 0x001010F2u, 0u,              // dcl_input v0.xyzw
        0x04000067u, 0x001020F2u, 0u, 1u,          // dcl_output_siv o0, position
        0x03000065u, 0x001020F2u, 1u,              // dcl_output o1.xyzw
        0x05000036u, 0x001020F2u, 0u, 0x00101E46u, 0u,  // mov o0, v0
        0x05000036u, 0x001020F2u, 1u, 0x00101E46u, 0u,  // mov o1, v0
        0x0100003Eu,                                // ret
    };
    for (const u32 word : words) {
        const auto* bytes = reinterpret_cast<const u8*>(&word);
        container.bytecode.insert(container.bytecode.end(), bytes, bytes + 4);
    }
    return container;
}

/// The first @p count floats of @p buffer as the GPU sees them, through a
/// compute copy into an R32Float texture (BackendBuffer has no readback).
[[nodiscard]] ::testing::AssertionResult read_floats(
    GraphicsBackendDevice& device, BackendBuffer* buffer, unsigned count,
    std::vector<float>& out) {
    const std::string source =
        "#include <metal_stdlib>\nusing namespace metal;\n"
        "kernel void main0(const device float* words [[buffer(" +
        std::to_string(BACKEND_SRV_BUFFER_BASE) + ")]],\n"
        "    texture2d<float, access::write> u0 [[texture(" +
        std::to_string(BACKEND_UAV_TEXTURE_BASE) + ")]],\n"
        "    uint3 id [[thread_position_in_grid]]) {\n"
        "    if (id.x < u0.get_width()) u0.write(float4(words[id.x]), "
        "uint2(id.x, 0));\n}\n";
    BackendComputePipelineDesc pd{};
    pd.compute_entry_point = "main0";
    pd.compute_shader_msl = source;
    pd.cache_key = 0x5EAD0F1u;
    auto pipeline = device.create_compute_pipeline(pd);
    if (!pipeline || !pipeline->valid()) {
        return ::testing::AssertionFailure() << "readback pipeline";
    }
    BackendTexture2DDesc td{};
    td.width = count;
    td.height = 1;
    td.mip_levels = td.array_size = 1;
    td.pixel_format = PixelFormat::R32Float;
    td.unordered_access = true;
    auto texture = device.create_texture_2d(td, nullptr, 0);
    if (!texture || !texture->valid()) {
        return ::testing::AssertionFailure() << "readback texture";
    }
    BackendDispatchCall call{};
    call.pipeline = pipeline.get();
    call.group_count_x = count;
    call.group_count_y = call.group_count_z = 1;
    call.buffers[BACKEND_SRV_BUFFER_BASE] = buffer;
    call.textures[BACKEND_UAV_TEXTURE_BASE] = texture.get();
    if (!device.dispatch(call)) {
        return ::testing::AssertionFailure() << "dispatch";
    }
    out.assign(count, -1.0f);
    if (!texture->readback(0, out.data(), count * sizeof(float))) {
        return ::testing::AssertionFailure() << "readback";
    }
    return ::testing::AssertionSuccess();
}

}  // namespace

// Pass A end to end: a translated vertex shader, drawn with the guest's vertex
// buffer bound at input slot 0, must write what vertex fetch delivered into the
// capture buffer and leave the vertex buffer alone.
//
// The translator once gave the vertex stage the compute stage's set-4 mapping,
// which put the capture buffer on [[buffer(22)]] -- vertex buffer 0. Nothing
// failed: two bindings at one index, one of them silently lost. Only reading
// both buffers back tells a capture that worked from one that fetched its own
// output.
TEST_F(GeneratedMslTest, CaptureDrawWritesFetchedVerticesBesideTheVertexBuffer) {
    auto& device = MetalDevice::instance();

    DxbcTranslationOptions options{};
    options.capture_vertex_outputs_to_buffer = true;
    const auto spirv =
        translate_dxbc_to_spirv(fxc_fetching_vertex_shader(), options);
    ASSERT_TRUE(spirv.valid()) << spirv.failure;
    ASSERT_EQ(spirv.capture_record_vec4s, 2u);
    MslCompileOptions msl_options{};
    msl_options.argument_buffers = false;
    const auto compiled = compile_spirv_to_msl(
        spirv.spirv.data(), spirv.spirv.size(), ShaderStage::Vertex,
        msl_options);
    ASSERT_TRUE(compiled.valid());
    const std::string msl = compiled.msl_text();
    ASSERT_NE(msl.find("[[buffer(" +
                       std::to_string(BACKEND_VERTEX_CAPTURE_BUFFER_INDEX) +
                       ")]]"),
              std::string::npos) << msl;

    BackendPipelineDesc pdesc{};
    pdesc.vertex_shader_msl = msl;
    pdesc.vertex_entry_point = "main0";
    pdesc.rasterization_enabled = false;
    pdesc.vertex_attributes = {{0, 0, BackendVertexFormat::Float4, 0}};
    pdesc.vertex_stride = pdesc.vertex_buffer_strides[0] = 16;
    // The pass has a colour target, so the pipeline declares its format even
    // though nothing is rasterised into it.
    pdesc.color_format = PixelFormat::RGBA8Unorm;
    pdesc.cache_key = 0xCA97D2A3u;
    auto pipeline = device.create_pipeline(pdesc);
    ASSERT_TRUE(pipeline != nullptr && pipeline->valid()) << msl;

    // Distinct, non-zero values per vertex and component: a capture that read
    // zeros, the wrong vertex, or its own output cannot reproduce them.
    const std::array<float, 12> positions{1, 2, 3, 4,   5, 6, 7, 8,
                                          9, 10, 11, 12};
    BackendBufferDesc vb{};
    vb.byte_width = sizeof(positions);
    vb.vertex_buffer = true;
    vb.shader_resource = true;
    auto vertices = device.create_buffer(vb, positions.data());
    ASSERT_NE(vertices, nullptr);

    constexpr unsigned kCaptureFloats = 3 * 2 * 4;
    const std::vector<float> sentinel(kCaptureFloats, -7.0f);
    BackendBufferDesc cb{};
    cb.byte_width = kCaptureFloats * sizeof(float);
    cb.shader_resource = true;
    cb.unordered_access = true;
    auto capture = device.create_buffer(cb, sentinel.data());
    ASSERT_NE(capture, nullptr);

    BackendTexture2DDesc td{};
    td.width = td.height = 1;
    td.mip_levels = td.array_size = 1;
    td.pixel_format = PixelFormat::RGBA8Unorm;
    td.render_target = true;
    auto target = device.create_texture_2d(td, nullptr, 0);
    ASSERT_NE(target, nullptr);
    BackendRenderPassDesc pass_desc{};
    pass_desc.color_targets[0] = target.get();
    pass_desc.color_formats[0] = PixelFormat::RGBA8Unorm;
    auto pass = device.create_render_pass_standalone(pass_desc);
    ASSERT_NE(pass, nullptr);

    BackendDrawCall call{};
    call.pipeline = pipeline.get();
    call.topology = BackendPrimitiveTopology::PointList;
    call.vertex_count = 3;
    call.vertex_buffer = vertices.get();
    call.vertex_buffers[0] = vertices.get();
    call.vertex_capture_buffer = capture.get();
    call.viewport = {0, 0, 1, 1, 0, 1};
    ASSERT_TRUE(pass->draw(call));
    ASSERT_TRUE(pass->end());

    std::vector<float> captured;
    ASSERT_TRUE(read_floats(device, capture.get(), kCaptureFloats, captured));
    for (unsigned vertex = 0; vertex < 3; ++vertex) {
        for (unsigned reg = 0; reg < 2; ++reg) {
            for (unsigned c = 0; c < 4; ++c) {
                EXPECT_EQ(captured[(vertex * 2 + reg) * 4 + c],
                          positions[vertex * 4 + c])
                    << "vertex " << vertex << " o" << reg << " component "
                    << c;
            }
        }
    }
    std::vector<float> fetched;
    ASSERT_TRUE(read_floats(device, vertices.get(), 12, fetched));
    for (unsigned i = 0; i < 12; ++i) {
        EXPECT_EQ(fetched[i], positions[i])
            << "the capture draw wrote into vertex buffer 0 at float " << i;
    }
}

// A vertex shader that reads SV_VertexID already has a VertexIndex builtin, and
// the capture adds its own to key the records. Stream output from exactly this
// shape -- no vertex buffers, everything from the vertex id -- is how GPU
// particle systems use it, so the capture variant must still be ONE vertex id
// that Metal accepts, and records must be keyed by it.
TEST_F(GeneratedMslTest, CaptureOfAVertexIdShaderCompilesAndKeysByThatId) {
    DxbcContainer container{};
    container.shader_model_major = 4;
    container.shader_model_minor = 0;
    container.stage = ShaderStage::Vertex;
    container.input_signature.push_back({"SV_VertexID", 0, 0, 0x1u, 1, 6u});
    container.output_signature.push_back({"SV_Position", 0, 0, 0xFu, 3, 1u});
    container.output_signature.push_back({"TEXCOORD", 0, 1, 0xFu, 3});
    const std::vector<u32> words{
        0x00010040u, 24u,
        0x04000060u, 0x00101012u, 0u, 6u,          // dcl_input_sgv v0.x, vertex_id
        0x04000067u, 0x001020F2u, 0u, 1u,          // dcl_output_siv o0, position
        0x03000065u, 0x001020F2u, 1u,              // dcl_output o1
        0x08000036u, 0x001020F2u, 0u,              // mov o0, l(0,0,0,1)
        0x00004002u, 0u, 0u, 0u, 0x3F800000u,
        0x05000056u, 0x001020F2u, 1u, 0x00101006u, 0u,  // utof o1, v0.xxxx
        0x0100003Eu,
    };
    for (const u32 word : words) {
        const auto* bytes = reinterpret_cast<const u8*>(&word);
        container.bytecode.insert(container.bytecode.end(), bytes, bytes + 4);
    }
    // Patch the length token: 2 + 4 + 4 + 3 + 8 + 5 + 1.
    const u32 length = 27u;
    std::memcpy(container.bytecode.data() + 4, &length, 4);

    DxbcTranslationOptions options{};
    options.capture_vertex_outputs_to_buffer = true;
    const auto spirv = translate_dxbc_to_spirv(container, options);
    ASSERT_TRUE(spirv.valid()) << spirv.failure;
    MslCompileOptions msl_options{};
    msl_options.argument_buffers = false;
    const auto compiled = compile_spirv_to_msl(
        spirv.spirv.data(), spirv.spirv.size(), ShaderStage::Vertex,
        msl_options);
    ASSERT_TRUE(compiled.valid());
    const std::string msl = compiled.msl_text();

    BackendPipelineDesc pdesc{};
    pdesc.vertex_shader_msl = msl;
    pdesc.vertex_entry_point = "main0";
    pdesc.rasterization_enabled = false;
    pdesc.color_format = PixelFormat::RGBA8Unorm;
    pdesc.cache_key = 0xCA97D2A4u;
    auto& device = MetalDevice::instance();
    auto pipeline = device.create_pipeline(pdesc);
    ASSERT_TRUE(pipeline != nullptr && pipeline->valid()) << msl;

    constexpr unsigned kFloats = 4 * 2 * 4;
    const std::vector<float> sentinel(kFloats, -7.0f);
    BackendBufferDesc cb{};
    cb.byte_width = kFloats * sizeof(float);
    cb.shader_resource = true;
    cb.unordered_access = true;
    auto capture = device.create_buffer(cb, sentinel.data());
    BackendTexture2DDesc td{};
    td.width = td.height = 1;
    td.mip_levels = td.array_size = 1;
    td.pixel_format = PixelFormat::RGBA8Unorm;
    td.render_target = true;
    auto target = device.create_texture_2d(td, nullptr, 0);
    BackendRenderPassDesc pass_desc{};
    pass_desc.color_targets[0] = target.get();
    pass_desc.color_formats[0] = PixelFormat::RGBA8Unorm;
    auto pass = device.create_render_pass_standalone(pass_desc);
    ASSERT_NE(pass, nullptr);
    BackendDrawCall call{};
    call.pipeline = pipeline.get();
    call.topology = BackendPrimitiveTopology::PointList;
    call.vertex_count = 4;
    call.vertex_capture_buffer = capture.get();
    call.viewport = {0, 0, 1, 1, 0, 1};
    ASSERT_TRUE(pass->draw(call));
    ASSERT_TRUE(pass->end());

    std::vector<float> captured;
    ASSERT_TRUE(read_floats(device, capture.get(), kFloats, captured));
    for (unsigned vid = 0; vid < 4; ++vid) {
        EXPECT_EQ(captured[(vid * 2 + 1) * 4], static_cast<float>(vid))
            << "record " << vid << " holds vertex " << vid << "'s TEXCOORD";
        EXPECT_EQ(captured[(vid * 2 + 0) * 4 + 3], 1.0f) << "SV_Position.w";
    }
}

namespace {

[[nodiscard]] DxbcContainer container_from_tokens(
    ShaderStage stage, const std::vector<u32>& words) {
    DxbcContainer container{};
    container.shader_model_major = 4;
    container.shader_model_minor = 0;
    container.stage = stage;
    for (const u32 word : words) {
        const auto* bytes = reinterpret_cast<const u8*>(&word);
        container.bytecode.insert(container.bytecode.end(), bytes, bytes + 4);
    }
    return container;
}

}  // namespace

// All three passes of the geometry emulation, on the GPU, into real pixels:
//
//   A  the vertex shader, captured (rasterization off)
//   B  the geometry shader, lowered to a compute kernel over the capture
//   C  the pass-through vertex shader drawing what B appended, indirectly
//
// The vertex shader writes o0 position, o1 a junk register and o2 COLOR; the
// geometry shader reads v[][0] and v[][2]. D3D links by register, so that is a
// legal pair -- and it is the case where the geometry shader's own input list
// ({0, 2}, stride 2) disagrees with the record the vertex shader wrote ({0, 1,
// 2}, stride 3). Without the explicit record layout the kernel reads COLOR
// from the junk slot and positions from the wrong records.
TEST_F(GeneratedMslTest, GeometryEmulationRendersThroughAllThreePasses) {
    auto& device = MetalDevice::instance();

    // --- Pass A: vs_4_0 { mov o0, v0; mov o1, l(9,9,9,9); mov o2, v1 }
    auto vs = container_from_tokens(ShaderStage::Vertex, {
        0x00010040u, 30u,
        0x0300005Fu, 0x001010F2u, 0u,                 // dcl_input v0
        0x0300005Fu, 0x001010F2u, 1u,                 // dcl_input v1
        0x04000067u, 0x001020F2u, 0u, 1u,             // dcl_output_siv o0, pos
        0x03000065u, 0x001020F2u, 1u,                 // dcl_output o1
        0x03000065u, 0x001020F2u, 2u,                 // dcl_output o2
        0x05000036u, 0x001020F2u, 0u, 0x00101E46u, 0u,
        0x08000036u, 0x001020F2u, 1u, 0x00004002u,
        0x41100000u, 0x41100000u, 0x41100000u, 0x41100000u,
        0x05000036u, 0x001020F2u, 2u, 0x00101E46u, 1u,
        0x0100003Eu,
    });
    // Length token: 2 + 3 + 3 + 4 + 3 + 3 + 5 + 8 + 5 + 1 = 37.
    const u32 vs_length = 37u;
    std::memcpy(vs.bytecode.data() + 4, &vs_length, 4);
    vs.input_signature.push_back({"POSITION", 0, 0, 0xFu, 3});
    vs.input_signature.push_back({"COLOR", 0, 1, 0xFu, 3});
    vs.output_signature.push_back({"SV_Position", 0, 0, 0xFu, 3, 1u});
    vs.output_signature.push_back({"JUNK", 0, 1, 0xFu, 3});
    vs.output_signature.push_back({"COLOR", 0, 2, 0xFu, 3});
    DxbcTranslationOptions vs_options{};
    vs_options.capture_vertex_outputs_to_buffer = true;
    const auto vs_spirv = translate_dxbc_to_spirv(vs, vs_options);
    ASSERT_TRUE(vs_spirv.valid()) << vs_spirv.failure;
    ASSERT_EQ(vs_spirv.capture_registers, (std::vector<u32>{0, 1, 2}));
    MslCompileOptions flat{};
    flat.argument_buffers = false;
    const auto vs_msl = compile_spirv_to_msl(
        vs_spirv.spirv.data(), vs_spirv.spirv.size(), ShaderStage::Vertex,
        flat);
    ASSERT_TRUE(vs_msl.valid());
    BackendPipelineDesc capture_desc{};
    capture_desc.vertex_shader_msl = vs_msl.msl_text();
    capture_desc.vertex_entry_point = "main0";
    capture_desc.rasterization_enabled = false;
    capture_desc.vertex_attributes = {{0, 0, BackendVertexFormat::Float4, 0},
                                      {1, 16, BackendVertexFormat::Float4, 0}};
    capture_desc.vertex_stride = capture_desc.vertex_buffer_strides[0] = 32;
    capture_desc.color_format = PixelFormat::BGRA8Unorm;
    capture_desc.cache_key = 0x6E0A0001u;
    auto capture_pipeline = device.create_pipeline(capture_desc);
    ASSERT_TRUE(capture_pipeline && capture_pipeline->valid());

    // --- Pass B: gs_4_0, triangle in, triangle strip out, reads v[][0], v[][2]
    std::vector<u32> gs_words{
        0x00020040u, 63u,
        0x05000061u, 0x002010F2u, 3u, 0u, 1u,         // dcl_input_siv v[3][0]
        0x0400005Fu, 0x002010F2u, 3u, 2u,             // dcl_input v[3][2]
        0x0100185Du,                                  // dcl_inputprimitive tri
        0x0100285Cu,                                  // dcl_outputtopology tristrip
        0x04000067u, 0x001020F2u, 0u, 1u,
        0x03000065u, 0x001020F2u, 1u,
        0x0200005Eu, 3u,                              // dcl_maxout 3
    };
    for (u32 vertex = 0; vertex < 3; ++vertex) {
        for (const u32 w : {0x06000036u, 0x001020F2u, 0u, 0x00201E46u, vertex, 0u,
                            0x06000036u, 0x001020F2u, 1u, 0x00201E46u, vertex, 2u,
                            0x01000013u}) {
            gs_words.push_back(w);
        }
    }
    gs_words.push_back(0x01000009u);
    gs_words.push_back(0x0100003Eu);
    auto gs = container_from_tokens(ShaderStage::Geometry, gs_words);
    gs.input_signature.push_back({"SV_Position", 0, 0, 0xFu, 3, 1u});
    gs.input_signature.push_back({"COLOR", 0, 2, 0xFu, 3});
    gs.output_signature.push_back({"SV_Position", 0, 0, 0xFu, 3, 1u});
    gs.output_signature.push_back({"COLOR", 0, 1, 0xFu, 3});
    DxbcTranslationOptions gs_options{};
    gs_options.emulate_geometry_with_compute = true;
    gs_options.gs_input_record_registers = vs_spirv.capture_registers;
    const auto gs_spirv = translate_dxbc_to_spirv(gs, gs_options);
    ASSERT_TRUE(gs_spirv.valid()) << gs_spirv.failure;
    ASSERT_TRUE(gs_spirv.gs_lowered_to_compute);
    const auto gs_msl = compile_spirv_to_msl(
        gs_spirv.spirv.data(), gs_spirv.spirv.size(), ShaderStage::Compute,
        flat);
    ASSERT_TRUE(gs_msl.valid());
    BackendComputePipelineDesc kernel_desc{};
    kernel_desc.compute_shader_msl = gs_msl.msl_text();
    kernel_desc.compute_entry_point = "main0";
    kernel_desc.cache_key = 0x6E0A0002u;
    auto kernel = device.create_compute_pipeline(kernel_desc);
    ASSERT_TRUE(kernel && kernel->valid()) << gs_msl.msl_text();

    // --- Pass C: pass-through vertex shader + a pixel shader reading COLOR
    const std::string passthrough = generate_gs_passthrough_vertex_msl(
        {{0u, 3u, true}, {1u, 3u, false}},
        gs_vertices_per_invocation(5u, 3u));
    ASSERT_FALSE(passthrough.empty());
    BackendPipelineDesc draw_desc{};
    draw_desc.vertex_shader_msl = passthrough;
    draw_desc.vertex_entry_point = kGsPassthroughEntryPoint;
    draw_desc.fragment_shader_msl = R"(#include <metal_stdlib>
using namespace metal;
struct In { float4 color [[user(locn1)]]; };
fragment float4 main0(In in [[stage_in]]) { return in.color; }
)";
    draw_desc.fragment_entry_point = "main0";
    draw_desc.color_format = PixelFormat::BGRA8Unorm;
    draw_desc.cache_key = 0x6E0A0003u;
    auto draw_pipeline = device.create_pipeline(draw_desc);
    ASSERT_TRUE(draw_pipeline && draw_pipeline->valid());

    // A full-target green triangle.
    const std::array<float, 24> vertices{
        -1, -1, 0, 1,   0, 1, 0, 1,
         3, -1, 0, 1,   0, 1, 0, 1,
        -1,  3, 0, 1,   0, 1, 0, 1};
    BackendBufferDesc vb{};
    vb.byte_width = sizeof(vertices);
    vb.vertex_buffer = true;
    auto vertex_buffer = device.create_buffer(vb, vertices.data());
    BackendBufferDesc storage{};
    storage.shader_resource = true;
    storage.unordered_access = true;
    storage.byte_width = 3 * 3 * 16;
    auto gs_in = device.create_buffer(storage, nullptr);
    storage.byte_width = 3 * 2 * 16;
    auto gs_out = device.create_buffer(storage, nullptr);
    // [1] non-indexed, [2] first vertex, [3] base, [4] stride; then one
    // written-vertex count per invocation at kGsInvocationCountBase.
    std::array<u32, kGsInvocationCountBase + 1> control{0, 0, 0, 0, 3};
    control[kGsInvocationCountBase] = 0xDEADu;  // the kernel must reset it
    storage.byte_width = sizeof(control);
    auto counter = device.create_buffer(storage, control.data());

    BackendTexture2DDesc td{};
    td.width = td.height = 4;
    td.mip_levels = td.array_size = 1;
    td.pixel_format = PixelFormat::BGRA8Unorm;
    td.render_target = true;
    auto target = device.create_texture_2d(td, nullptr, 0);
    ASSERT_TRUE(target->clear_render_target({1.0f, 0.0f, 0.0f, 1.0f}));
    BackendRenderPassDesc pass_desc{};
    pass_desc.color_targets[0] = target.get();
    pass_desc.color_formats[0] = PixelFormat::BGRA8Unorm;
    pass_desc.preserve_color_contents = true;

    {  // Pass A
        auto pass = device.create_render_pass_standalone(pass_desc);
        BackendDrawCall call{};
        call.pipeline = capture_pipeline.get();
        call.topology = BackendPrimitiveTopology::TriangleList;
        call.vertex_count = 3;
        call.vertex_buffer = vertex_buffer.get();
        call.vertex_buffers[0] = vertex_buffer.get();
        call.vertex_capture_buffer = gs_in.get();
        call.viewport = {0, 0, 4, 4, 0, 1};
        ASSERT_TRUE(pass->draw(call));
        ASSERT_TRUE(pass->end());
    }
    {  // Pass B: one thread per input primitive.
        BackendDispatchCall call{};
        call.pipeline = kernel.get();
        call.group_count_x = call.group_count_y = call.group_count_z = 1;
        call.buffers[BACKEND_GS_EXPANSION_BUFFER_BASE + 0] = gs_in.get();
        call.buffers[BACKEND_GS_EXPANSION_BUFFER_BASE + 1] = gs_out.get();
        call.buffers[BACKEND_GS_EXPANSION_BUFFER_BASE + 2] = counter.get();
        // Non-indexed still binds an index buffer the loads stay inside.
        call.buffers[BACKEND_GS_EXPANSION_BUFFER_BASE + 3] = gs_in.get();
        ASSERT_TRUE(device.dispatch(call));
    }
    {  // Pass C: the invocation's whole region; unwritten slots are culled.
        auto pass = device.create_render_pass_standalone(pass_desc);
        BackendDrawCall call{};
        call.pipeline = draw_pipeline.get();
        call.topology = BackendPrimitiveTopology::TriangleList;
        call.vertex_count = gs_vertices_per_invocation(5u, 3u);
        call.vertex_srv_buffers[0] = gs_out.get();
        call.vertex_srv_buffers[1] = counter.get();
        call.viewport = {0, 0, 4, 4, 0, 1};
        ASSERT_TRUE(pass->draw(call));
        ASSERT_TRUE(pass->end());
    }

    std::array<u8, 64> pixels{};
    ASSERT_TRUE(target->readback(0, pixels.data(), 16));
    for (usize p = 0; p < 16; ++p) {
        // BGRA8: green, not the red clear and not the junk register.
        EXPECT_EQ(pixels[p * 4 + 1], 255u) << "pixel " << p << " green";
        EXPECT_EQ(pixels[p * 4 + 2], 0u) << "pixel " << p << " red";
    }
}

// =============================================================================
// Area 07 fixed function (.docs/audits/2026-09-28-d3d11-contract-audit/
// area-07-fixed-function.md): pipeline state legal in D3D that Metal would
// reject or abort on, and the translator's fixed-function epilogues, compiled
// by the real Metal compiler from the probes' own fxc output.
// =============================================================================

#include "../oracle_data/d3d11_a07_shaders.h"

namespace {

constexpr const char* kA07Vertex =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "vertex float4 a07_vs(uint vid [[vertex_id]]) {\n"
    "    return float4(float(vid & 1u), float(vid >> 1), 0.5, 1.0);\n"
    "}\n";

[[nodiscard]] std::string a07_translate(const unsigned char* bytecode,
                                        usize size, ShaderStage stage,
                                        DxbcTranslationOptions options) {
    const auto container = parse_dxbc(bytecode, size);
    EXPECT_TRUE(container.valid());
    const auto spirv = translate_dxbc_to_spirv(container, options);
    EXPECT_TRUE(spirv.valid()) << spirv.failure;
    if (!spirv.valid()) return {};
    MslCompileOptions flat{};
    flat.argument_buffers = false;
    const auto msl =
        compile_spirv_to_msl(spirv.spirv.data(), spirv.spirv.size(), stage,
                             flat);
    EXPECT_TRUE(msl.valid());
    return msl.valid() ? msl.msl_text() : std::string{};
}

}  // namespace

TEST_F(GeneratedMslTest, BlendingOnAnIntegerTargetIsIgnored) {
    // D3D ignores BlendEnable on a non-blendable (integer) target; Windows
    // stores the source (d3d11_a07_om_probe `O9 R32_UINT blend ONE/ONE ADD
    // src=5 dst=7: 00000005`). Metal aborts the process in pipeline
    // validation if the attachment is left blending.
    BackendPipelineDesc desc{};
    desc.vertex_shader_msl = kA07Vertex;
    desc.vertex_entry_point = "a07_vs";
    desc.fragment_shader_msl =
        "#include <metal_stdlib>\n"
        "using namespace metal;\n"
        "fragment uint4 a07_fs() { return uint4(5u); }\n";
    desc.fragment_entry_point = "a07_fs";
    desc.color_format = PixelFormat::R32Uint;
    desc.blend_enabled = true;
    desc.source_blend = BackendBlendFactor::One;
    desc.destination_blend = BackendBlendFactor::One;
    desc.cache_key = 0xA0701u;
    auto pipeline = MetalDevice::instance().create_pipeline(desc);
    EXPECT_TRUE(pipeline != nullptr && pipeline->valid());
    // Independent blend names the integer target in slot 1 too.
    desc.color_formats[1] = PixelFormat::RGBA16Uint;
    desc.independent_blend_enabled = true;
    desc.blend_enabled_targets.fill(true);
    desc.fragment_shader_msl =
        "#include <metal_stdlib>\n"
        "using namespace metal;\n"
        "struct a07_out { uint4 a [[color(0)]]; uint4 b [[color(1)]]; };\n"
        "fragment a07_out a07_fs() { return { uint4(5u), uint4(6u) }; }\n";
    desc.cache_key = 0xA0702u;
    pipeline = MetalDevice::instance().create_pipeline(desc);
    EXPECT_TRUE(pipeline != nullptr && pipeline->valid());
}

TEST_F(GeneratedMslTest, PipelinesNameThePassDepthFormatAndAlphaToCoverage) {
    // The pipeline's depth format is the pass's own (Metal derives the
    // depth-bias unit from it, D3D11.3 15.10), and alpha-to-coverage
    // (17.18) is pipeline state.
    for (const PixelFormat depth :
         {PixelFormat::D16Unorm, PixelFormat::D32Float,
          PixelFormat::D24Unorm_S8Uint, PixelFormat::D32Float_S8Uint}) {
        BackendPipelineDesc desc{};
        desc.vertex_shader_msl = kA07Vertex;
        desc.vertex_entry_point = "a07_vs";
        desc.fragment_shader_msl =
            "#include <metal_stdlib>\n"
            "using namespace metal;\n"
            "fragment float4 a07_fs() { return float4(1.0, 1.0, 1.0, 0.5); }\n";
        desc.fragment_entry_point = "a07_fs";
        desc.color_format = PixelFormat::RGBA8Unorm;
        desc.depth_attachment = true;
        desc.depth_format = depth;
        desc.alpha_to_coverage = true;
        desc.cache_key = 0xA0710u + static_cast<u64>(depth);
        auto pipeline = MetalDevice::instance().create_pipeline(desc);
        EXPECT_TRUE(pipeline != nullptr && pipeline->valid())
            << static_cast<unsigned>(depth);
    }
}

TEST_F(GeneratedMslTest, FixedFunctionEpiloguesOfTranslatedStagesCompile) {
    // vs_pos_col with the depth-offset epilogue, ps_depth with the SV_Depth
    // viewport clamp, ps_col writing the blend state's SampleMask.
    DxbcTranslationOptions options{};
    options.fixed_function_push = true;
    const std::string vs = a07_translate(g_vs_pos_col, sizeof(g_vs_pos_col),
                                         ShaderStage::Vertex, options);
    EXPECT_NE(vs.find("vitreo_sampler_lod_biases._m7"), std::string::npos)
        << vs;
    const std::string ps_depth = a07_translate(
        g_ps_depth, sizeof(g_ps_depth), ShaderStage::Fragment, options);
    EXPECT_NE(ps_depth.find("[[depth(any)]]"), std::string::npos);
    options.apply_sample_mask = true;
    const std::string ps_mask = a07_translate(
        g_ps_col, sizeof(g_ps_col), ShaderStage::Fragment, options);
    EXPECT_NE(ps_mask.find("[[sample_mask]]"), std::string::npos) << ps_mask;
    const std::vector<BackendVertexAttribute> attributes{
        {0, 0, BackendVertexFormat::Float4},
        {1, 16, BackendVertexFormat::Float4}};
    EXPECT_TRUE(compiles(vs, ps_depth, "main0", "main0", attributes, 32,
                         /*depth=*/true));
    EXPECT_TRUE(compiles(vs, ps_mask, "main0", "main0", attributes, 32));
    // Without the option nothing reads the block's new members.
    const std::string plain = a07_translate(
        g_vs_pos_col, sizeof(g_vs_pos_col), ShaderStage::Vertex, {});
    EXPECT_EQ(plain.find("vitreo_sampler_lod_biases._m7"), std::string::npos);
}

TEST_F(GeneratedMslTest, R10G10B10A2UintInputIsRestoredFromTheNormalizedFetch) {
    // DXGI_FORMAT_R10G10B10A2_UINT has no Metal integer fetch: the stage-in
    // is UInt1010102Normalized and the prologue scales back to integers
    // (Windows: all-ones reads 1023 1023 1023 3, d3d11_a07_ia_probe `C fmt
    // 25`).
    DxbcTranslationOptions options{};
    options.vertex_input_unpack_uint1010102 = 1u << 0;  // ATTR is v0
    const std::string vs = a07_translate(g_vs_fmt_u, sizeof(g_vs_fmt_u),
                                         ShaderStage::Vertex, options);
    EXPECT_NE(vs.find("1023.0"), std::string::npos) << vs;
    const std::string ps = a07_translate(g_ps_fmt_u, sizeof(g_ps_fmt_u),
                                         ShaderStage::Fragment, {});
    BackendPipelineDesc desc{};
    desc.vertex_shader_msl = vs;
    desc.fragment_shader_msl = ps;
    desc.vertex_entry_point = desc.fragment_entry_point = "main0";
    desc.vertex_attributes = {{0, 0, BackendVertexFormat::UInt1010102}};
    desc.vertex_stride = 4;
    desc.color_format = PixelFormat::RGBA32Uint;
    desc.cache_key = 0xA0720u;
    auto pipeline = MetalDevice::instance().create_pipeline(desc);
    EXPECT_TRUE(pipeline != nullptr && pipeline->valid()) << vs;
}

namespace {

// MslCompileOptions::precise_without_helpers lowers a D3D `precise` multiply
// and add to plain operators under `#pragma clang fp contract(off)`, in a
// library compiled MTLMathModeSafe. Its correctness rests on this compiler
// fact, checked here on the real device: with the pragma, a*b - c is the
// product rounded, then the difference rounded (D3D11.3 3.1.3.1, IEEE-754
// per operation), never one fused FMA. Inputs span full mantissas, so an FMA
// would differ from the two roundings on roughly half of them. Safe mode's
// other half of the contract -- no reassociation -- is checked alongside.
TEST(PreciseContractionTest, SafeModeWithContractionOffDoesNotFuse) {
    auto& device = MetalDevice::instance();
    if (!device.available()) GTEST_SKIP() << "no Metal device";
    const std::string msl =
        "#include <metal_stdlib>\n"
        "#pragma clang fp contract(off)\n"
        "using namespace metal;\n"
        "kernel void main0(device const float* in [[buffer(" +
        std::to_string(BACKEND_SRV_BUFFER_BASE) +
        ")]], device float* out [[buffer(" +
        std::to_string(BACKEND_UAV_BUFFER_BASE) +
        ")]], uint id [[thread_position_in_grid]]) {\n"
        "    const float a = in[3 * id], b = in[3 * id + 1], c = in[3 * id + 2];\n"
        "    out[2 * id] = a * b - c;\n"
        "    const float big = c * 16777216.0f;\n"
        "    out[2 * id + 1] = (big + a) - big;\n"
        "}\n";
    BackendComputePipelineDesc pd{};
    pd.compute_entry_point = "main0";
    pd.compute_shader_msl = msl;
    pd.math_mode = BackendMathMode::Safe;
    pd.cache_key = 0xC0DE5A11u;
    auto pipeline = device.create_compute_pipeline(pd);
    ASSERT_TRUE(pipeline && pipeline->valid()) << msl;

    constexpr unsigned kThreads = 64u * 256u;
    std::vector<float> input(3u * kThreads);
    u32 state = 0x2545F491u;
    for (float& v : input) {
        state = state * 1664525u + 1013904223u;
        v = 0.5f + static_cast<float>(state >> 8) * (1.0f / 16777216.0f);
    }
    BackendBufferDesc in_desc{};
    in_desc.byte_width = static_cast<unsigned>(input.size() * sizeof(float));
    in_desc.shader_resource = true;
    auto in = device.create_buffer(in_desc, input.data());
    BackendBufferDesc out_desc{};
    out_desc.byte_width = 2u * kThreads * sizeof(float);
    out_desc.unordered_access = true;
    out_desc.shader_resource = true;
    auto out = device.create_buffer(out_desc, nullptr);
    ASSERT_TRUE(in && out);

    BackendDispatchCall call{};
    call.pipeline = pipeline.get();
    call.group_count_x = kThreads / 64u;
    call.group_count_y = call.group_count_z = 1;
    call.threads_per_group_x = 64;
    call.threads_per_group_y = call.threads_per_group_z = 1;
    call.buffers[BACKEND_SRV_BUFFER_BASE] = in.get();
    call.buffers[BACKEND_UAV_BUFFER_BASE] = out.get();
    ASSERT_TRUE(device.dispatch(call));
    device.wait_idle();
    std::vector<float> got(2u * kThreads);
    ASSERT_TRUE(out->read(0, got.data(), out_desc.byte_width));

    unsigned fused_would_differ = 0;
    for (unsigned i = 0; i < kThreads; ++i) {
        const float a = input[3 * i], b = input[3 * i + 1], c = input[3 * i + 2];
        volatile float product = a * b;  // rounded on its own
        const float unfused = product - c;
        const float fused = std::fma(a, b, -c);
        if (unfused != fused) ++fused_would_differ;
        ASSERT_EQ(got[2 * i], unfused) << "thread " << i << ": a*b-c was fused";
        // (big + a) - big rounds a to big's ulp; reassociating it to a would
        // not (Safe mode: no reassociation).
        volatile float big = c * 16777216.0f;
        volatile float sum = big + a;
        const float rounded = sum - big;
        ASSERT_EQ(got[2 * i + 1], rounded)
            << "thread " << i << ": (big + a) - big was reassociated";
    }
    EXPECT_GT(fused_would_differ, kThreads / 4u)
        << "the inputs no longer tell a fused result from an unfused one";
}

// The lowering precise_without_helpers emits for a shader that still has
// refactoringAllowed (D3D11.3 22.19.2). Safe mode, no file-scope pragma.
// A precise mul then a precise sub is two roundings. A non-precise a*b+c
// in the same shader is one FMA: that is the contraction Safe mode keeps
// once the pragma is not painted over the whole library. (big + a) - big
// is not reassociated. precise(-1 * 0) is -0 (D3D11.3 3.1.3.1).
TEST(PreciseContractionTest, IsolatedPreciseOpsLeaveOtherContractions) {
    auto& device = MetalDevice::instance();
    if (!device.available()) GTEST_SKIP() << "no Metal device";
    const std::string msl =
        "#include <metal_stdlib>\n"
        "using namespace metal;\n"
        "template<typename T> [[clang::always_inline]] T vitreoPreciseMul(T l, T r) "
        "{ _Pragma(\"clang fp contract(off) reassociate(off)\") return l * r; }\n"
        "template<typename T> [[clang::always_inline]] T vitreoPreciseSub(T l, T r) "
        "{ _Pragma(\"clang fp contract(off) reassociate(off)\") return l - r; }\n"
        "kernel void main0(device const float* in [[buffer(" +
        std::to_string(BACKEND_SRV_BUFFER_BASE) +
        ")]], device float* out [[buffer(" +
        std::to_string(BACKEND_UAV_BUFFER_BASE) +
        ")]], uint id [[thread_position_in_grid]]) {\n"
        "    const float a = in[3 * id], b = in[3 * id + 1], c = in[3 * id + 2];\n"
        "    out[4 * id] = vitreoPreciseSub(vitreoPreciseMul(a, b), c);\n"
        "    out[4 * id + 1] = a * b + c;\n"
        "    const float big = c * 16777216.0f;\n"
        "    out[4 * id + 2] = (big + a) - big;\n"
        "    out[4 * id + 3] = vitreoPreciseMul(-1.0f, 0.0f);\n"
        "}\n";
    BackendComputePipelineDesc pd{};
    pd.compute_entry_point = "main0";
    pd.compute_shader_msl = msl;
    pd.math_mode = BackendMathMode::Safe;
    pd.cache_key = 0xC0DE5A12u;
    auto pipeline = device.create_compute_pipeline(pd);
    ASSERT_TRUE(pipeline && pipeline->valid()) << msl;

    constexpr unsigned kThreads = 64u * 256u;
    std::vector<float> input(3u * kThreads);
    u32 state = 0x2545F491u;
    for (float& v : input) {
        state = state * 1664525u + 1013904223u;
        v = 0.5f + static_cast<float>(state >> 8) * (1.0f / 16777216.0f);
    }
    BackendBufferDesc in_desc{};
    in_desc.byte_width = static_cast<unsigned>(input.size() * sizeof(float));
    in_desc.shader_resource = true;
    auto in = device.create_buffer(in_desc, input.data());
    BackendBufferDesc out_desc{};
    out_desc.byte_width = 4u * kThreads * sizeof(float);
    out_desc.unordered_access = true;
    out_desc.shader_resource = true;
    auto out = device.create_buffer(out_desc, nullptr);
    ASSERT_TRUE(in && out);

    BackendDispatchCall call{};
    call.pipeline = pipeline.get();
    call.group_count_x = kThreads / 64u;
    call.group_count_y = call.group_count_z = 1;
    call.threads_per_group_x = 64;
    call.threads_per_group_y = call.threads_per_group_z = 1;
    call.buffers[BACKEND_SRV_BUFFER_BASE] = in.get();
    call.buffers[BACKEND_UAV_BUFFER_BASE] = out.get();
    ASSERT_TRUE(device.dispatch(call));
    device.wait_idle();
    std::vector<float> got(4u * kThreads);
    ASSERT_TRUE(out->read(0, got.data(), out_desc.byte_width));

    unsigned fused_would_differ = 0;
    unsigned add_would_differ = 0;
    unsigned plain_fused = 0;
    for (unsigned i = 0; i < kThreads; ++i) {
        const float a = input[3 * i], b = input[3 * i + 1], c = input[3 * i + 2];
        volatile float product = a * b;
        const float unfused = product - c;
        const float fused_sub = std::fma(a, b, -c);
        if (unfused != fused_sub) ++fused_would_differ;
        ASSERT_EQ(got[4 * i], unfused) << "thread " << i << ": precise a*b-c was fused";
        volatile float product_add = a * b;
        const float unfused_add = product_add + c;
        const float fused_add = std::fma(a, b, c);
        if (unfused_add != fused_add) {
            ++add_would_differ;
            if (got[4 * i + 1] == fused_add) ++plain_fused;
        }
        volatile float big = c * 16777216.0f;
        volatile float sum = big + a;
        const float rounded = sum - big;
        ASSERT_EQ(got[4 * i + 2], rounded)
            << "thread " << i << ": (big + a) - big was reassociated";
        ASSERT_TRUE(std::signbit(got[4 * i + 3]))
            << "precise (-1 * 0) lost its sign";
    }
    EXPECT_GT(fused_would_differ, kThreads / 4u)
        << "the inputs no longer tell a fused result from an unfused one";
    EXPECT_GT(add_would_differ, kThreads / 8u);
    // Every input whose rounded product-add differs from an FMA took the FMA.
    EXPECT_EQ(plain_fused, add_would_differ)
        << "non-precise a*b+c did not contract; the precise helpers painted "
           "the whole shader";
}

}  // namespace

namespace {
void* retained_feature_state(unsigned value) {
    unsigned bytes[8] = {value};
    return const_cast<void*>(static_cast<const void*>(CFDataCreate(
        kCFAllocatorDefault, reinterpret_cast<const UInt8*>(bytes), sizeof(bytes))));
}
unsigned feature_state_value(void* state) {
    unsigned value = 0;
    std::memcpy(&value, CFDataGetBytePtr(static_cast<CFDataRef>(state)), sizeof(value));
    return value;
}
}

TEST(SamplerFeatureRebuildTest, RebuildsEachNewRequiredFeatureAndRetainsOldStates) {
    std::atomic<void*> state{retained_feature_state(0)};
    unsigned active = 0, calls = 0;
    void* initial = state.load();
    {
        SamplerFeatureRebuild rebuild(7u, 0u, [&] {
            ++calls; return retained_feature_state(active);
        });
        EXPECT_EQ(rebuild.current(state, 0u), initial);
        active = 1;
        void* first = rebuild.current(state, active);
        ASSERT_NE(first, nullptr);
        EXPECT_EQ(feature_state_value(first), 1u);
        active = 3;
        void* second = rebuild.current(state, active);
        ASSERT_NE(second, nullptr);
        EXPECT_EQ(feature_state_value(second), 3u);
        EXPECT_EQ(feature_state_value(initial), 0u);
        EXPECT_EQ(feature_state_value(first), 1u);
        EXPECT_EQ(rebuild.current(state, active), second);
        active = 7;
        void* third = rebuild.current(state, active);
        ASSERT_NE(third, nullptr);
        EXPECT_EQ(feature_state_value(third), 7u);
        EXPECT_EQ(feature_state_value(initial), 0u);
        EXPECT_EQ(feature_state_value(first), 1u);
        EXPECT_EQ(feature_state_value(second), 3u);
        EXPECT_EQ(calls, 3u);
    }
    release_retained_metal_object(state.load());
}

TEST(SamplerFeatureRebuildTest, ConcurrentUseBuildsOneReplacement) {
    std::atomic<void*> state{retained_feature_state(0)};
    std::atomic<unsigned> calls{0};
    {
        SamplerFeatureRebuild rebuild(2u, 0u, [&] {
            calls.fetch_add(1); return retained_feature_state(2);
        });
        std::array<void*, 8> results{};
        std::vector<std::thread> threads;
        for (unsigned i = 0; i < results.size(); ++i)
            threads.emplace_back([&, i] { results[i] = rebuild.current(state, 2u); });
        for (auto& thread : threads) thread.join();
        EXPECT_EQ(calls.load(), 1u);
        for (void* result : results) EXPECT_EQ(result, state.load());
    }
    release_retained_metal_object(state.load());
}

TEST(SamplerFeatureRebuildTest, RequiredRebuildFailureDisablesNewUses) {
    std::atomic<void*> state{retained_feature_state(0)};
    unsigned calls = 0;
    {
        SamplerFeatureRebuild rebuild(3u, 0u, [&]() -> void* { ++calls; return nullptr; });
        EXPECT_EQ(rebuild.current(state, 2u), nullptr);
        EXPECT_EQ(state.load(), nullptr);
        EXPECT_EQ(rebuild.current(state, 3u), nullptr);
        EXPECT_EQ(calls, 1u);
    }
}

TEST(SamplerFeatureRebuildTest, UnusedFeaturesDoNotRebuildThePipeline) {
    std::atomic<void*> state{retained_feature_state(0)};
    unsigned calls = 0;
    {
        SamplerFeatureRebuild rebuild(2u, 0u, [&] {
            ++calls; return retained_feature_state(2);
        });
        EXPECT_EQ(rebuild.current(state, 1u), state.load());
        EXPECT_EQ(calls, 0u);
        EXPECT_NE(rebuild.current(state, 3u), nullptr);
        EXPECT_EQ(calls, 1u);
        EXPECT_EQ(rebuild.current(state, 7u), state.load());
        EXPECT_EQ(calls, 1u);
    }
    release_retained_metal_object(state.load());
}

// Run this lifecycle case by itself so the feature is initially absent. The
// deliberately invalid newly enabled arm verifies the backend's failure path.
TEST(SamplerFeatureRebuildGpuTest, RequiredFeatureCompileFailureRejectsDispatch) {
    if ((enabled_sampler_features() & static_cast<unsigned>(SamplerFeature::CustomBorder)) != 0)
        GTEST_SKIP() << "requires a fresh process";
    auto& device = MetalDevice::instance();
    if (!device.available()) GTEST_SKIP() << "no Metal device";
    BackendComputePipelineDesc pd{};
    pd.compute_entry_point = "main0";
    pd.math_mode = BackendMathMode::Safe;
    pd.compute_shader_msl =
        "#include <metal_stdlib>\nusing namespace metal;\n"
        "#ifndef VITREO_CUSTOM_BORDER\n#define VITREO_CUSTOM_BORDER 0\n#endif\n"
        "#if VITREO_CUSTOM_BORDER\n#error required_feature_compile_failure\n#endif\n"
        "kernel void main0(device uint* out [[buffer(" +
        std::to_string(BACKEND_UAV_BUFFER_BASE) + ")]]) { out[0] = 1u; }\n";
    auto pipeline = device.create_compute_pipeline(pd);
    ASSERT_TRUE(pipeline && pipeline->valid());
    BackendBufferDesc bd{};
    bd.byte_width = sizeof(u32);
    bd.unordered_access = true;
    const u32 zero = 0;
    auto output = device.create_buffer(bd, &zero);
    ASSERT_TRUE(output);
    BackendDispatchCall call{};
    call.pipeline = pipeline.get();
    call.group_count_x = call.group_count_y = call.group_count_z = 1;
    call.threads_per_group_x = call.threads_per_group_y = call.threads_per_group_z = 1;
    call.buffers[BACKEND_UAV_BUFFER_BASE] = output.get();
    ASSERT_TRUE(device.dispatch(call));
    ASSERT_TRUE(device.wait_idle());
    u32 value = 0;
    ASSERT_TRUE(output->read(0, &value, sizeof(value)));
    ASSERT_EQ(value, 1u);
    enable_sampler_feature(SamplerFeature::CustomBorder);
    EXPECT_FALSE(device.dispatch(call));
    EXPECT_FALSE(pipeline->valid());
    EXPECT_TRUE(device.wait_idle());
    ASSERT_TRUE(output->read(0, &value, sizeof(value)));
    EXPECT_EQ(value, 1u);
}

TEST_F(GeneratedMslTest, ImmediateEncodingPreservesSharedProducerAndFlushOrdering) {
    auto& device = MetalDevice::instance();
    ASSERT_TRUE(device.wait_idle());
    BackendBufferDesc desc{};
    desc.byte_width = sizeof(u32);
    const u32 first = 0x11223344u, second = 0x55667788u, zero = 0;
    auto a = device.create_buffer(desc, &first);
    auto b = device.create_buffer(desc, &second);
    auto result = device.create_buffer(desc, &zero);
    ASSERT_TRUE(a && b && result);
    auto direct = device.begin_immediate_execution();
    ASSERT_TRUE(direct);
    const u64 generation = device.recording_generation();
    ASSERT_TRUE(device.copy_buffer(result.get(), a.get()));
    EXPECT_EQ(device.last_recorded_generation(), generation);
    const auto partial = device.flush_completion();
    ASSERT_TRUE(partial);
    ASSERT_TRUE(partial->wait());
    EXPECT_FALSE(device.generation_complete(generation));
    u32 observed = 0;
    ASSERT_TRUE(result->read(0, &observed, sizeof(observed)));
    EXPECT_EQ(observed, first);

    // Another frontend can queue a later generation while the lease is open.
    // An intra-lease flush must neither execute nor retire that later work.
    std::thread producer([&] {
        EXPECT_TRUE(device.copy_buffer(result.get(), b.get()));
        device.flush();
    });
    producer.join();
    ASSERT_TRUE(device.copy_buffer(result.get(), a.get()));
    ASSERT_TRUE(device.flush_completion()->wait());
    ASSERT_TRUE(result->read(0, &observed, sizeof(observed)));
    EXPECT_EQ(observed, first);
    EXPECT_EQ(device.recording_generation(), generation);
    direct.reset();
    ASSERT_TRUE(device.wait_idle());
    ASSERT_TRUE(result->read(0, &observed, sizeof(observed)));
    EXPECT_EQ(observed, second);
    EXPECT_TRUE(device.generation_complete(generation));
}

TEST_F(GeneratedMslTest, ImmediateEncodingReadbackReleasesAndResumesOwnership) {
    auto& device = MetalDevice::instance();
    BackendBufferDesc desc{};
    desc.byte_width = sizeof(u32);
    const u32 expected = 0x12345678u, zero = 0;
    auto source = device.create_buffer(desc, &expected);
    auto result = device.create_buffer(desc, &zero);
    ASSERT_TRUE(source && result);
    auto direct = device.begin_immediate_execution();
    ASSERT_TRUE(direct);
    const u64 generation = device.recording_generation();
    ASSERT_TRUE(device.copy_buffer(result.get(), source.get()));
    ASSERT_TRUE(device.wait_generation(generation));
    EXPECT_GT(device.recording_generation(), generation);
    EXPECT_TRUE(device.generation_complete(generation));
    u32 observed = 0;
    ASSERT_TRUE(result->read(0, &observed, sizeof(observed)));
    EXPECT_EQ(observed, expected);
    ASSERT_TRUE(device.copy_buffer(result.get(), source.get()));
    direct.reset();
    EXPECT_TRUE(device.wait_idle());
}

TEST_F(GeneratedMslTest, ImmediateEncodingWaitIncludesAnotherProducersGeneration) {
    auto& device = MetalDevice::instance();
    auto fence = device.create_fence(0);
    ASSERT_TRUE(fence && fence->valid());
    auto direct = device.begin_immediate_execution();
    ASSERT_TRUE(direct);
    const u64 own = device.recording_generation();
    u64 later = 0;
    std::thread producer([&] {
        EXPECT_TRUE(fence->wait_gpu(1));
        later = device.recording_generation();
        device.flush();
    });
    producer.join();
    ASSERT_GT(later, own);
    std::thread release([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        EXPECT_TRUE(fence->signal_cpu(1));
    });
    const bool completed = device.wait_generation(later);
    const bool actually_complete = device.generation_complete(later);
    release.join();
    EXPECT_TRUE(completed);
    EXPECT_TRUE(actually_complete);
    EXPECT_GE(fence->completed_value(), 1u);
    direct.reset();
    EXPECT_TRUE(device.wait_idle());
}

TEST_F(GeneratedMslTest, ImmediateEncodingMainThreadAcquisitionPumpsRunLoop) {
    auto& device = MetalDevice::instance();
    ASSERT_TRUE(device.wait_idle());
    struct Bridge {
        std::mutex mutex;
        std::condition_variable cv;
        bool entered{false};
        bool pumped{false};
    } bridge;
    CFRunLoopSourceContext context{};
    context.info = &bridge;
    context.perform = [](void* opaque) {
        auto& state = *static_cast<Bridge*>(opaque);
        std::lock_guard lock(state.mutex);
        state.pumped = true;
        state.cv.notify_all();
    };
    CFRunLoopRef loop = CFRunLoopGetCurrent();
    CFRunLoopSourceRef source = CFRunLoopSourceCreate(nullptr, 0, &context);
    ASSERT_NE(source, nullptr);
    CFRunLoopAddSource(loop, source, kCFRunLoopDefaultMode);
    auto& queue = MetalSubmitQueue::instance();
    queue.record_encode([&](void*) {
        std::unique_lock lock(bridge.mutex);
        bridge.entered = true;
        bridge.cv.notify_all();
        CFRunLoopSourceSignal(source);
        CFRunLoopWakeUp(loop);
        // RED fails promptly rather than hanging the test process forever.
        bridge.cv.wait_for(lock, std::chrono::seconds(2), [&] { return bridge.pumped; });
    });
    (void)queue.submit();
    {
        std::unique_lock lock(bridge.mutex);
        bridge.cv.wait(lock, [&] { return bridge.entered; });
    }
    auto direct = device.begin_immediate_execution();
    bool pumped = false;
    {
        std::lock_guard lock(bridge.mutex);
        pumped = bridge.pumped;
    }
    direct.reset();
    CFRunLoopRemoveSource(loop, source, kCFRunLoopDefaultMode);
    CFRelease(source);
    EXPECT_TRUE(pumped);
    EXPECT_TRUE(device.wait_idle());
}

TEST_F(GeneratedMslTest, ImmediateEncodingMainThreadGenerationWaitPumpsRunLoop) {
    auto& device = MetalDevice::instance();
    ASSERT_TRUE(device.wait_idle());
    struct Bridge {
        std::mutex mutex;
        std::condition_variable cv;
        bool entered{false};
        bool pumped{false};
    } bridge;
    CFRunLoopSourceContext context{};
    context.info = &bridge;
    context.perform = [](void* opaque) {
        auto& state = *static_cast<Bridge*>(opaque);
        std::lock_guard lock(state.mutex);
        state.pumped = true;
        state.cv.notify_all();
    };
    CFRunLoopRef loop = CFRunLoopGetCurrent();
    CFRunLoopSourceRef source = CFRunLoopSourceCreate(nullptr, 0, &context);
    ASSERT_NE(source, nullptr);
    CFRunLoopAddSource(loop, source, kCFRunLoopDefaultMode);
    auto& queue = MetalSubmitQueue::instance();
    auto direct = device.begin_immediate_execution();
    ASSERT_TRUE(direct);
    u64 generation = 0;
    std::thread producer([&] {
    queue.record_encode([&](void*) {
        std::unique_lock lock(bridge.mutex);
        bridge.entered = true;
        bridge.cv.notify_all();
        CFRunLoopSourceSignal(source);
        CFRunLoopWakeUp(loop);
        // RED fails promptly rather than hanging the test process forever.
        bridge.cv.wait_for(lock, std::chrono::seconds(2), [&] { return bridge.pumped; });
    });
    generation = queue.recording_generation();
    (void)queue.submit();
    });
    producer.join();
    EXPECT_TRUE(device.wait_generation(generation));
    bool pumped = false;
    {
        std::lock_guard lock(bridge.mutex);
        pumped = bridge.pumped;
    }
    direct.reset();
    CFRunLoopRemoveSource(loop, source, kCFRunLoopDefaultMode);
    CFRelease(source);
    EXPECT_TRUE(pumped);
    EXPECT_TRUE(device.wait_idle());
}


namespace {
BackendPipelineDesc exact_cache_pipeline(const char* tag) {
    BackendPipelineDesc desc{};
    desc.vertex_shader_msl = std::string("// ") + tag + R"(
#include <metal_stdlib>
using namespace metal;
vertex float4 exact_cache_vertex(uint id [[vertex_id]]) {
    const float2 positions[3] = {float2(-1,-1), float2(3,-1), float2(-1,3)};
    return float4(positions[id], 0, 1);
}
)";
    desc.fragment_shader_msl = std::string("// ") + tag + R"(
#include <metal_stdlib>
using namespace metal;
fragment float4 exact_cache_fragment() { return float4(0.2,0.3,0.4,1); }
)";
    desc.vertex_entry_point = "exact_cache_vertex";
    desc.fragment_entry_point = "exact_cache_fragment";
    desc.color_format = PixelFormat::BGRA8Unorm;
    return desc;
}
}

TEST_F(GeneratedMslTest, IdenticalSynchronousPipelinesCompileOnceAcrossCallerKeys) {
    auto& device = MetalDevice::instance();
    auto desc = exact_cache_pipeline("separate caller identities");
    desc.cache_key = 1001;
    const auto before = compile_profile::snapshot();
    auto first = device.create_pipeline(desc);
    ASSERT_TRUE(first && first->valid());
    desc.cache_key = 2001;
    auto second = device.create_pipeline(desc);
    ASSERT_TRUE(second && second->valid());
    const auto after = compile_profile::snapshot();
    EXPECT_EQ(after.count[compile_profile::MetalRenderState] -
              before.count[compile_profile::MetalRenderState], 1u);
}

TEST_F(GeneratedMslTest, SynchronousPipelineCacheDistinguishesStateWithSameCallerKey) {
    auto& device = MetalDevice::instance();
    auto desc = exact_cache_pipeline("same caller identity different masks");
    desc.cache_key = 4001;
    const auto before = compile_profile::snapshot();
    auto first = device.create_pipeline(desc);
    ASSERT_TRUE(first && first->valid());
    desc.color_write_mask = 0;
    auto second = device.create_pipeline(desc);
    ASSERT_TRUE(second && second->valid());
    const auto after = compile_profile::snapshot();
    EXPECT_EQ(after.count[compile_profile::MetalRenderState] -
              before.count[compile_profile::MetalRenderState], 2u);
}

TEST_F(GeneratedMslTest, ConcurrentIdenticalSynchronousPipelinesCompileOnce) {
    auto& device = MetalDevice::instance();
    const auto source = exact_cache_pipeline("concurrent exact descriptions");
    std::array<std::unique_ptr<BackendPipeline>, 6> pipelines;
    std::array<std::thread, 6> workers;
    std::atomic<bool> start{false};
    const auto before = compile_profile::snapshot();
    for (unsigned i = 0; i < workers.size(); ++i) {
        workers[i] = std::thread([&, i] {
            while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
            auto desc = source;
            desc.cache_key = i + 5001;
            pipelines[i] = device.create_pipeline(desc);
        });
    }
    start.store(true, std::memory_order_release);
    for (auto& worker : workers) worker.join();
    for (const auto& pipeline : pipelines) ASSERT_TRUE(pipeline && pipeline->valid());
    const auto after = compile_profile::snapshot();
    EXPECT_EQ(after.count[compile_profile::MetalRenderState] -
              before.count[compile_profile::MetalRenderState], 1u);
}

TEST_F(GeneratedMslTest, SynchronousPipelineCacheDistinguishesCompiledFunctions) {
    auto& device = MetalDevice::instance();
    auto desc = exact_cache_pipeline("same render state different function");
    const auto before = compile_profile::snapshot();
    auto first = device.create_pipeline(desc);
    ASSERT_TRUE(first && first->valid());
    desc.fragment_shader_msl.replace(desc.fragment_shader_msl.find("0.2,0.3,0.4"),
                                     std::strlen("0.2,0.3,0.4"), "0.7,0.6,0.5");
    auto second = device.create_pipeline(desc);
    ASSERT_TRUE(second && second->valid());
    const auto after = compile_profile::snapshot();
    EXPECT_EQ(after.count[compile_profile::MetalRenderState] -
              before.count[compile_profile::MetalRenderState], 2u);
}

TEST_F(GeneratedMslTest, FailedSynchronousNativePipelineCreationCanRetry) {
    auto& device = MetalDevice::instance();
    auto desc = exact_cache_pipeline("native failure retry");
    // Metal requires an integer fragment output for an integer attachment.
    // The library is valid; native pipeline creation must reject this pair.
    desc.color_format = PixelFormat::R32Uint;
    const auto before = compile_profile::snapshot();
    auto first = device.create_pipeline(desc);
    EXPECT_FALSE(first && first->valid());
    auto second = device.create_pipeline(desc);
    EXPECT_FALSE(second && second->valid());
    const auto after = compile_profile::snapshot();
    EXPECT_EQ(after.count[compile_profile::MetalRenderState] -
              before.count[compile_profile::MetalRenderState], 2u);
}
