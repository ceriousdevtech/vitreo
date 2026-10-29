// SPDX-License-Identifier: LGPL-2.1-only
/// @file test_dxbc_to_spirv_compute.cpp
/// @brief Compute-stage translation.

#include <gtest/gtest.h>

#include "graphics/shaders/dxbc_to_spirv.hpp"
#include "graphics/shaders/dxbc_parser.hpp"
#include "graphics/shaders/spirv_to_msl.hpp"
#include "graphics/contracts/backend_device.hpp"

#include "dxbc_to_spirv_test_shaders.h"
#include "dxbc_to_spirv_test_spirv.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <tuple>
#include <vector>

using namespace vitreo;
using namespace vitreo::graphics;
using namespace vitreo::graphics::dxbc_spirv_test;

namespace {

DxbcContainer create_fxc_empty_compute_shader(u32 x = 1u, u32 y = 1u,
                                               u32 z = 1u) {
    DxbcContainer container{};
    container.shader_model_major = 5;
    container.shader_model_minor = 0;
    container.stage = ShaderStage::Compute;
    const auto append_word = [&](u32 word) {
        const auto* bytes = reinterpret_cast<const u8*>(&word);
        container.bytecode.insert(container.bytecode.end(), bytes, bytes + 4);
    };
    // Exact SHEX body of Microsoft D3DCompiler 47's empty
    // [numthreads(1,1,1)] cs_5_0 shader, with dimensions parameterized only
    // for declaration validation tests.
    append_word(0x00050050u);
    append_word(8u);
    append_word(0x0100086Au); // dcl_globalFlags refactoringAllowed
    append_word(0x0400009Bu); // dcl_thread_group
    append_word(x); append_word(y); append_word(z);
    append_word(0x0100003Eu); // ret
    return container;
}

DxbcContainer create_synthetic_dispatch_id_compute_shader() {
    DxbcContainer container{};
    container.shader_model_major = 5;
    container.shader_model_minor = 0;
    container.stage = ShaderStage::Compute;
    const auto append_word = [&](u32 word) {
        const auto* bytes = reinterpret_cast<const u8*>(&word);
        container.bytecode.insert(container.bytecode.end(), bytes, bytes + 4);
    };
    append_word(0x00050050u); // cs_5_0
    append_word(14u);
    append_word(0x0100086Au); // dcl_globalFlags refactoringAllowed
    append_word(0x02000068u); append_word(1u); // dcl_temps 1
    append_word(0x0400009Bu); append_word(1u); append_word(1u);
    append_word(1u); // dcl_thread_group 1, 1, 1
    append_word(0x04000036u); // mov r0, vThreadID
    append_word(0x001000F2u); append_word(0u);
    // Windows SDK d3d11TokenizedProgramFormat.hpp publishes
    // D3D11_SB_OPERAND_TYPE_INPUT_THREAD_ID=32 in operand bits 19:12.
    append_word(0x00020E46u);
    append_word(0x0100003Eu); // ret
    return container;
}

DxbcContainer create_synthetic_typed_uav_load_compute_shader() {
    DxbcContainer container{};
    container.shader_model_major = 5;
    container.shader_model_minor = 0;
    container.stage = ShaderStage::Compute;
    const auto append_word = [&](u32 word) {
        const auto* bytes = reinterpret_cast<const u8*>(&word);
        container.bytecode.insert(container.bytecode.end(), bytes, bytes + 4);
    };

    // Synthetic token stream from the public Windows SDK ABI. The load is
    // written back through the same typed UAV so SPIRV-Cross retains both the
    // read and its observable value without requiring another resource tier.
    append_word(0x00050050u); // cs_5_0
    append_word(26u);
    append_word(0x0100086Au); // dcl_globalFlags refactoringAllowed
    append_word(0x02000068u); append_word(1u); // dcl_temps 1
    append_word(0x0400009Bu); append_word(1u); append_word(1u);
    append_word(1u); // dcl_thread_group 1, 1, 1
    append_word(0x0400189Cu); append_word(0x0011E000u);
    append_word(0u); append_word(0x00005555u); // Texture2D<float4> u0
    append_word(0x060000A3u); // ld_uav_typed r0, vThreadID, u0
    append_word(0x001000F2u); append_word(0u);
    append_word(0x00020E46u);
    append_word(0x0011EE46u); append_word(0u);
    append_word(0x060000A4u); // store_uav_typed u0, vThreadID, r0
    append_word(0x0011E0F2u); append_word(0u);
    append_word(0x00020E46u);
    append_word(0x00100E46u); append_word(0u);
    append_word(0x0100003Eu); // ret
    return container;
}

DxbcContainer create_synthetic_compute_group_values_shader() {
    DxbcContainer container{};
    container.shader_model_major = 5;
    container.stage = ShaderStage::Compute;
    const auto append_word = [&](u32 word) {
        const auto* bytes = reinterpret_cast<const u8*>(&word);
        container.bytecode.insert(container.bytecode.end(), bytes, bytes + 4);
    };
    append_word(0x00050050u); append_word(27u);
    append_word(0x0100086Au);
    append_word(0x0400009Bu); append_word(2u); append_word(2u);
    append_word(1u); // dcl_thread_group 2, 2, 1
    append_word(0x0400189Cu); append_word(0x0011E000u);
    append_word(0u); append_word(0x00005555u); // Texture2D<float4> u0
    append_word(0x050000A4u); // store u0[vThreadID.xy] = vGroupID
    append_word(0x0011E0F2u); append_word(0u);
    append_word(0x00020E46u); append_word(0x00021E46u);
    append_word(0x050000A4u); // store u0[vGroupThreadID.xy] = same
    append_word(0x0011E0F2u); append_word(0u);
    append_word(0x00022E46u); append_word(0x00022E46u);
    append_word(0x050000A4u); // store u0[vThreadID.xy] = vGroupIndex
    append_word(0x0011E0F2u); append_word(0u);
    append_word(0x00020E46u); append_word(0x00024001u);
    append_word(0x0100003Eu);
    return container;
}

DxbcContainer create_synthetic_compute_sample_l_shader() {
    DxbcContainer container{};
    container.shader_model_major = 5;
    container.stage = ShaderStage::Compute;
    const auto append_word = [&](u32 word) {
        const auto* bytes = reinterpret_cast<const u8*>(&word);
        container.bytecode.insert(container.bytecode.end(), bytes, bytes + 4);
    };
    append_word(0x00050050u); append_word(41u);
    append_word(0x0100086Au);
    append_word(0x02000068u); append_word(1u); // dcl_temps 1
    append_word(0x0400009Bu); append_word(1u); append_word(1u);
    append_word(1u);
    append_word(0x04001858u); append_word(0x00107000u);
    append_word(0u); append_word(0x00005555u); // Texture2D<float4> t0
    append_word(0x0300005Au); append_word(0x00106000u);
    append_word(0u); // sampler s0
    append_word(0x0400189Cu); append_word(0x0011E000u);
    append_word(0u); append_word(0x00005555u); // RWTexture2D<float4> u0
    append_word(0x0E000048u); // sample_l r0, (0.5,0.5), t0, s0, 0
    append_word(0x001000F2u); append_word(0u);
    append_word(0x00004002u); append_word(0x3F000000u);
    append_word(0x3F000000u); append_word(0u); append_word(0u);
    append_word(0x00107E46u); append_word(0u);
    append_word(0x00106000u); append_word(0u);
    append_word(0x00004001u); append_word(0u);
    append_word(0x060000A4u); // store u0[vThreadID.xy] = r0
    append_word(0x0011E0F2u); append_word(0u);
    append_word(0x00020E46u); append_word(0x00100E46u); append_word(0u);
    append_word(0x0100003Eu);
    return container;
}

DxbcContainer create_synthetic_compute_raw_tgsm_shader() {
    DxbcContainer container{};
    container.shader_model_major = 5;
    container.stage = ShaderStage::Compute;
    const auto append_word = [&](u32 word) {
        const auto* bytes = reinterpret_cast<const u8*>(&word);
        container.bytecode.insert(container.bytecode.end(), bytes, bytes + 4);
    };
    append_word(0x00050050u); append_word(39u);
    append_word(0x0100086Au);
    append_word(0x02000068u); append_word(1u); // dcl_temps 1
    append_word(0x0400009Bu); append_word(2u); append_word(1u);
    append_word(1u); // dcl_thread_group 2, 1, 1
    append_word(0x0400009Fu); append_word(0x0011F000u);
    append_word(0u); append_word(16u); // dcl_tgsm_raw g0, 16
    append_word(0x0400189Cu); append_word(0x0011E000u);
    append_word(0u); append_word(0x00005555u); // u0
    append_word(0x070000A6u); // store_raw g0.x, l(0), l(1.0)
    append_word(0x0011F012u); append_word(0u);
    append_word(0x00004001u); append_word(0u);
    append_word(0x00004001u); append_word(0x3F800000u);
    append_word(0x010018BEu); // sync_g_t
    append_word(0x070000A5u); // ld_raw r0.x, l(0), g0.x
    append_word(0x00100012u); append_word(0u);
    append_word(0x00004001u); append_word(0u);
    append_word(0x0011F00Au); append_word(0u);
    append_word(0x060000A4u); // store u0[vThreadID.xy] = r0
    append_word(0x0011E0F2u); append_word(0u);
    append_word(0x00020E46u); append_word(0x00100E46u); append_word(0u);
    append_word(0x0100003Eu);
    return container;
}

} // namespace

TEST(DxbcToSpirvTest, ComputeThreadGroupBecomesLocalSizeAndMslKernel) {
    const auto result = translate_dxbc_to_spirv(
        create_fxc_empty_compute_shader());
    ASSERT_TRUE(result.valid()) << result.failure;
    EXPECT_EQ(result.stage, ShaderStage::Compute);
    EXPECT_EQ(result.workgroup_size_x, 1u);
    EXPECT_EQ(result.workgroup_size_y, 1u);
    EXPECT_EQ(result.workgroup_size_z, 1u);
    EXPECT_TRUE(spirv_contains_execution_mode_xyz(
        result.spirv, /*LocalSize=*/17u, 1u, 1u, 1u));

    const auto msl = compile_spirv_to_msl(
        result.spirv.data(), result.spirv.size(), ShaderStage::Compute);
    ASSERT_TRUE(msl.valid());
    EXPECT_EQ(msl.workgroup_size_x, 1u);
    EXPECT_EQ(msl.workgroup_size_y, 1u);
    EXPECT_EQ(msl.workgroup_size_z, 1u);
    EXPECT_NE(std::string(msl.msl_text()).find("kernel void"),
              std::string::npos);

    const auto invalid = translate_dxbc_to_spirv(
        create_fxc_empty_compute_shader(1025u, 1u, 1u));
    EXPECT_FALSE(invalid.valid());
    EXPECT_NE(invalid.failure.find("thread-group"), std::string::npos);
}

TEST(DxbcToSpirvTest, ComputeDispatchThreadIdUsesGlobalInvocationBuiltin) {
    const auto result = translate_dxbc_to_spirv(
        create_synthetic_dispatch_id_compute_shader());
    ASSERT_TRUE(result.valid()) << result.failure;
    EXPECT_TRUE(spirv_contains_builtin(
        result.spirv, /*BuiltIn GlobalInvocationId=*/28u));
    EXPECT_TRUE(spirv_contains_opcode(result.spirv, /*OpLoad=*/61u));
    EXPECT_TRUE(spirv_contains_opcode(
        result.spirv, /*OpCompositeConstruct=*/80u));
    EXPECT_TRUE(spirv_contains_opcode(result.spirv, /*OpBitcast=*/124u));
}

TEST(DxbcToSpirvTest, ComputeTypedTexture2DUavStoreWritesFloat4) {
    const auto result = translate_dxbc_to_spirv(
        create_synthetic_typed_uav_store_compute_shader());
    ASSERT_TRUE(result.valid()) << result.failure;
    EXPECT_TRUE(spirv_contains_builtin(
        result.spirv, /*BuiltIn GlobalInvocationId=*/28u));
    EXPECT_TRUE(spirv_contains_opcode(result.spirv, /*OpImageWrite=*/99u));

    MslCompileOptions options{};
    options.argument_buffers = false;
    const auto compiled = compile_spirv_to_msl(
        result.spirv.data(), result.spirv.size(), ShaderStage::Compute,
        options);
    ASSERT_TRUE(compiled.valid());
    const std::string msl = compiled.msl_text();
    EXPECT_NE(msl.find("texture2d<float, access::write>"), std::string::npos);
    // Compute UAVs are remapped to Metal texture slots 8+u# so SRV t# and
    // UAV u# remain simultaneously bindable.
    EXPECT_NE(msl.find("[[texture(8)]]"), std::string::npos) << msl;
    EXPECT_NE(msl.find("get_width()"), std::string::npos);
    EXPECT_NE(msl.find(".write("), std::string::npos);

    // Store-only access must survive argument-buffer translation as well;
    // otherwise the wider UAV_TYPED_STORE format set cannot be used there.
    options.argument_buffers = true;
    options.argument_buffer_sets[3] = {1, 5, MslDescriptorClass::Texture};
    const auto table_compiled = compile_spirv_to_msl(
        result.spirv.data(), result.spirv.size(), ShaderStage::Compute, options);
    ASSERT_TRUE(table_compiled.valid());
    const std::string table_msl = table_compiled.msl_text();
    EXPECT_NE(table_msl.find("texture2d<float, access::write>"), std::string::npos);
    EXPECT_EQ(table_msl.find("access::read_write"), std::string::npos);

    // Learn (D3D10_SB_RESOURCE_DIMENSION): TEXTURE3D (5) is RWTexture3D<T>
    // and TEXTURE2DARRAY (8) is RWTexture2DArray<T>; both translate. A cube
    // (6) typed UAV is still diagnostic.
    const auto volume_shape = translate_dxbc_to_spirv(
        create_synthetic_typed_uav_store_compute_shader(
            /*Texture3D=*/5u));
    EXPECT_TRUE(volume_shape.valid()) << volume_shape.failure;
    // Dimension 8 is how a CUBE uav arrives -- D3D exposes the six faces as
    // array slices -- so UE5's skylight convolution needs it. It is an
    // arrayed Dim2D storage image, and its coordinate and bounds check are
    // three-wide exactly as a volume's are.
    const auto array_shape = translate_dxbc_to_spirv(
        create_synthetic_typed_uav_store_compute_shader(
            /*Texture2DArray=*/8u));
    EXPECT_TRUE(array_shape.valid()) << array_shape.failure;
    const auto wrong_shape = translate_dxbc_to_spirv(
        create_synthetic_typed_uav_store_compute_shader(
            /*TextureCube=*/6u));
    EXPECT_FALSE(wrong_shape.valid());
    EXPECT_NE(wrong_shape.failure.find("UAV dimension"), std::string::npos);

    const auto uint_type = translate_dxbc_to_spirv(
        create_synthetic_typed_uav_store_compute_shader(
            /*Texture2D=*/3u, /*UINT xyzw=*/0x00004444u));
    ASSERT_TRUE(uint_type.valid()) << uint_type.failure;
    const auto uint_msl = compile_spirv_to_msl(
        uint_type.spirv.data(), uint_type.spirv.size(), ShaderStage::Compute);
    ASSERT_TRUE(uint_msl.valid());
    EXPECT_NE(std::string(uint_msl.msl_text()).find(
                  "texture2d<uint, access::write>"),
              std::string::npos);
}

TEST(DxbcToSpirvTest, ComputeTypedTexture2DUavLoadReadsOrReturnsZero) {
    const auto result = translate_dxbc_to_spirv(
        create_synthetic_typed_uav_load_compute_shader());
    ASSERT_TRUE(result.valid()) << result.failure;
    EXPECT_TRUE(spirv_contains_opcode(result.spirv, /*OpImageRead=*/98u));
    EXPECT_TRUE(spirv_contains_opcode(result.spirv, /*OpPhi=*/245u));
    EXPECT_TRUE(spirv_contains_opcode(result.spirv, /*OpImageWrite=*/99u));

    MslCompileOptions options{};
    options.argument_buffers = false;
    const auto compiled = compile_spirv_to_msl(
        result.spirv.data(), result.spirv.size(), ShaderStage::Compute,
        options);
    ASSERT_TRUE(compiled.valid());
    const std::string msl = compiled.msl_text();
    EXPECT_NE(msl.find("texture2d<float, access::read_write>"),
              std::string::npos);
    EXPECT_NE(msl.find("[[texture(8)]]"), std::string::npos);
    EXPECT_NE(msl.find(".read("), std::string::npos);
    EXPECT_NE(msl.find("float4(0.0)"), std::string::npos);
    EXPECT_NE(msl.find(".write("), std::string::npos);
}

TEST(DxbcToSpirvTest, ComputeGroupSystemValuesUseExactUintBuiltins) {
    const auto result = translate_dxbc_to_spirv(
        create_synthetic_compute_group_values_shader());
    ASSERT_TRUE(result.valid()) << result.failure;
    EXPECT_TRUE(spirv_contains_builtin(result.spirv, /*WorkgroupId=*/26u));
    EXPECT_TRUE(spirv_contains_builtin(
        result.spirv, /*LocalInvocationId=*/27u));
    EXPECT_TRUE(spirv_contains_builtin(
        result.spirv, /*LocalInvocationIndex=*/29u));

    MslCompileOptions options{};
    options.argument_buffers = false;
    const auto compiled = compile_spirv_to_msl(
        result.spirv.data(), result.spirv.size(), ShaderStage::Compute,
        options);
    ASSERT_TRUE(compiled.valid());
    const std::string msl = compiled.msl_text();
    EXPECT_NE(msl.find("threadgroup_position_in_grid"), std::string::npos);
    EXPECT_NE(msl.find("thread_position_in_threadgroup"), std::string::npos);
    EXPECT_NE(msl.find("thread_index_in_threadgroup"), std::string::npos);

    DxbcContainer wrong_stage{};
    wrong_stage.shader_model_major = 5;
    wrong_stage.stage = ShaderStage::Fragment;
    wrong_stage.output_signature.push_back(
        {"SV_TARGET", 0, 0, 0xFu, 3});
    const u32 words[] = {
        0x00000050u, 7u, 0x04000036u, 0x001020F2u, 0u,
        0x00021E46u, 0x0100003Eu};
    wrong_stage.bytecode.assign(
        reinterpret_cast<const u8*>(words),
        reinterpret_cast<const u8*>(words + 7));
    const auto rejected = translate_dxbc_to_spirv(wrong_stage);
    EXPECT_FALSE(rejected.valid());
    EXPECT_NE(rejected.failure.find("outside compute"), std::string::npos);
}

TEST(DxbcToSpirvTest, ComputeSampleLUsesSrvSamplerAndExplicitLevel) {
    const auto result = translate_dxbc_to_spirv(
        create_synthetic_compute_sample_l_shader());
    ASSERT_TRUE(result.valid()) << result.failure;
    EXPECT_TRUE(spirv_contains_opcode(
        result.spirv, /*OpImageSampleExplicitLod=*/88u));
    ASSERT_EQ(result.texture_bindings.size(), 1u);
    ASSERT_EQ(result.sampler_bindings.size(), 1u);

    MslCompileOptions options{};
    options.argument_buffers = false;
    const auto compiled = compile_spirv_to_msl(
        result.spirv.data(), result.spirv.size(), ShaderStage::Compute,
        options);
    ASSERT_TRUE(compiled.valid());
    const std::string msl = compiled.msl_text();
    EXPECT_NE(msl.find("t0 [[texture(0)]]"), std::string::npos);
    EXPECT_NE(msl.find("s0 [[sampler(0)]]"), std::string::npos);
    EXPECT_NE(msl.find("u0 [[texture(8)]]"), std::string::npos);
    // The explicit level plus the sampler's MipLODBias (22.4.18).
    EXPECT_NE(msl.find("level("), std::string::npos);
    EXPECT_NE(msl.find("vitreo_sampler_lod_biases"), std::string::npos);

    // Implicit-LOD SAMPLE has derivative semantics and remains pixel-only.
    DxbcContainer implicit = create_synthetic_compute_sample_l_shader();
    constexpr usize sample_opcode_word = 20u;
    const u32 sample = 0x0E000045u;
    std::memcpy(implicit.bytecode.data() +
                    sample_opcode_word * sizeof(u32),
                &sample, sizeof(sample));
    const auto rejected = translate_dxbc_to_spirv(implicit);
    EXPECT_FALSE(rejected.valid());
    EXPECT_NE(rejected.failure.find("implicit-LOD"), std::string::npos)
        << rejected.failure;
}

TEST(DxbcToSpirvTest, ComputeRawGroupsharedLoadStoreAndSync) {
    DxbcContainer container = create_synthetic_compute_raw_tgsm_shader();
    const auto result = translate_dxbc_to_spirv(container);
    ASSERT_TRUE(result.valid()) << result.failure;
    EXPECT_TRUE(spirv_contains_opcode(result.spirv, /*OpAccessChain=*/65u));
    EXPECT_TRUE(spirv_contains_opcode(result.spirv, /*OpControlBarrier=*/224u));

    MslCompileOptions options{};
    options.argument_buffers = false;
    const auto compiled = compile_spirv_to_msl(
        result.spirv.data(), result.spirv.size(), ShaderStage::Compute,
        options);
    ASSERT_TRUE(compiled.valid());
    const std::string msl = compiled.msl_text();
    EXPECT_NE(msl.find("threadgroup uint"), std::string::npos);
    EXPECT_NE(msl.find("threadgroup_barrier(mem_flags::mem_threadgroup)"),
              std::string::npos);
    EXPECT_NE(msl.find("[[texture(8)]]"), std::string::npos);

    // byteCount is required to be a nonzero multiple of four.
    constexpr usize byte_count_word = 12u;
    const u32 invalid_byte_count = 18u;
    std::memcpy(container.bytecode.data() +
                    byte_count_word * sizeof(u32),
                &invalid_byte_count, sizeof(invalid_byte_count));
    const auto invalid_declaration = translate_dxbc_to_spirv(container);
    EXPECT_FALSE(invalid_declaration.valid());
    EXPECT_NE(invalid_declaration.failure.find("32 KiB"), std::string::npos);

}

TEST(DxbcToSpirvTest, ComputeSyncLowersEveryFlagCombination) {
    // Learn (sync, sm5 asm): sync[_uglobal|_ugroup][_g][_t], flags in
    // OpcodeToken0 bits 11 (_t), 12 (_g), 13 (_ugroup), 14 (_uglobal).
    // SPIRV-Cross renders both OpControlBarrier and OpMemoryBarrier as MSL
    // threadgroup_barrier with mem_flags taken from the semantics.
    constexpr usize sync_word = 24u;
    const auto with_sync = [](u32 flags) {
        DxbcContainer container = create_synthetic_compute_raw_tgsm_shader();
        const u32 token = 0x010000BEu | flags;
        std::memcpy(container.bytecode.data() + sync_word * sizeof(u32),
                    &token, sizeof(token));
        return container;
    };
    struct Case {
        u32 flags;
        const char* name;
        bool control_barrier;
        const char* mem_flags;
    };
    const Case cases[] = {
        {0x0800u, "sync_t", true, "mem_flags::mem_none"},
        {0x1000u, "sync_g", false, "mem_flags::mem_threadgroup"},
        {0x1800u, "sync_g_t", true, "mem_flags::mem_threadgroup"},
        {0x2000u, "sync_ugroup", false,
         "mem_flags::mem_device | mem_flags::mem_texture"},
        {0x3800u, "sync_ugroup_g_t", true,
         "mem_flags::mem_device | mem_flags::mem_threadgroup | "
         "mem_flags::mem_texture"},
        {0x4000u, "sync_uglobal", false,
         "mem_flags::mem_device | mem_flags::mem_texture"},
        {0x5000u, "sync_uglobal_g", false,
         "mem_flags::mem_device | mem_flags::mem_threadgroup | "
         "mem_flags::mem_texture"},
        {0x5800u, "sync_uglobal_g_t", true,
         "mem_flags::mem_device | mem_flags::mem_threadgroup | "
         "mem_flags::mem_texture"},
    };
    for (const Case& c : cases) {
        SCOPED_TRACE(c.name);
        const auto result = translate_dxbc_to_spirv(with_sync(c.flags));
        ASSERT_TRUE(result.valid()) << result.failure;
        EXPECT_EQ(spirv_contains_opcode(result.spirv, /*OpControlBarrier=*/224u),
                  c.control_barrier);
        EXPECT_EQ(spirv_contains_opcode(result.spirv, /*OpMemoryBarrier=*/225u),
                  !c.control_barrier);
        MslCompileOptions options{};
        options.argument_buffers = false;
        const auto compiled = compile_spirv_to_msl(
            result.spirv.data(), result.spirv.size(), ShaderStage::Compute,
            options);
        ASSERT_TRUE(compiled.valid());
        // _t is a rendezvous: threadgroup_barrier. Without _t it is a fence
        // only (MSL 3.2 atomic_thread_fence), device-scoped for _uglobal.
        std::string expected;
        if (c.control_barrier || !msl_memory_fence_available()) {
            expected = std::string("threadgroup_barrier(") + c.mem_flags + ")";
        } else {
            expected = std::string("atomic_thread_fence(") + c.mem_flags +
                       ", memory_order_seq_cst, " +
                       ((c.flags & 0x4000u) != 0u ? "thread_scope_device"
                                                  : "thread_scope_threadgroup") +
                       ");";
        }
        EXPECT_NE(std::string(compiled.msl_text()).find(expected),
                  std::string::npos)
            << expected << "\n" << compiled.msl_text();
    }

    // No flag, or both UAV scopes, is not a valid sync.
    for (const u32 invalid : {0x0000u, 0x6000u, 0x7800u}) {
        const auto result = translate_dxbc_to_spirv(with_sync(invalid));
        EXPECT_FALSE(result.valid()) << std::hex << invalid;
        EXPECT_NE(result.failure.find("invalid flag combination"),
                  std::string::npos);
    }

    // A fence without _t may sit in divergent flow. It lowers to a fence that
    // does not rendezvous where MSL 3.2 is available and is refused by name
    // otherwise (threadgroup_barrier there could hang the group). With _t the
    // author guarantees uniform flow and it is lowered.
    const auto in_if = [&](u32 flags) {
        DxbcContainer container = with_sync(flags);
        auto& code = container.bytecode;
        const auto word_at = [&](usize index) {
            return code.begin() + static_cast<std::ptrdiff_t>(index * 4u);
        };
        const u32 endif = 0x01000015u;
        code.insert(word_at(sync_word + 1u),
                    reinterpret_cast<const u8*>(&endif),
                    reinterpret_cast<const u8*>(&endif) + 4);
        const u32 if_nz[3] = {0x0304001Fu, 0x00004001u, 1u};  // if_nz l(1)
        code.insert(word_at(sync_word), reinterpret_cast<const u8*>(if_nz),
                    reinterpret_cast<const u8*>(if_nz) + sizeof(if_nz));
        const u32 total = static_cast<u32>(code.size() / 4u);
        std::memcpy(code.data() + 4, &total, sizeof(total));
        return container;
    };
    const auto divergent_fence = translate_dxbc_to_spirv(in_if(0x1000u));
    if (msl_memory_fence_available()) {
        ASSERT_TRUE(divergent_fence.valid()) << divergent_fence.failure;
        MslCompileOptions options{};
        options.argument_buffers = false;
        const auto compiled = compile_spirv_to_msl(
            divergent_fence.spirv.data(), divergent_fence.spirv.size(),
            ShaderStage::Compute, options);
        ASSERT_TRUE(compiled.valid());
        const std::string msl = compiled.msl_text();
        EXPECT_NE(msl.find("atomic_thread_fence(mem_flags::mem_threadgroup, "
                           "memory_order_seq_cst, thread_scope_threadgroup);"),
                  std::string::npos)
            << msl;
        EXPECT_EQ(msl.find("threadgroup_barrier"), std::string::npos)
            << "a divergent fence must not become a rendezvous\n" << msl;
    } else {
        EXPECT_FALSE(divergent_fence.valid());
        EXPECT_NE(divergent_fence.failure.find("divergent"), std::string::npos)
            << divergent_fence.failure;
    }
    const auto barrier_in_if = translate_dxbc_to_spirv(in_if(0x1800u));
    EXPECT_TRUE(barrier_in_if.valid()) << barrier_in_if.failure;
}
