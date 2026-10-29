/// @file test_d3d9_shaders.cpp
/// @brief Programmable vertex and pixel shaders.
///        Part of the D3D9 frontend contract tests (see test_d3d9.cpp).

#include "d3d9_test_fixture.hpp"

#include <array>
#include <cstring>
#include <string>
#include <vector>

using namespace vitreo;
using namespace vitreo::winapi;
using vitreo::winapi::d3d9_test::D3D9ManagerTest;

// =============================================================================
// Programmable shaders
// =============================================================================

TEST_F(D3D9ManagerTest, SetShaderRejectsAPointerItDidNotHandOut) {
    const auto [factory, device] = create_device();
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 92, {device, 0x1234}),
              D3DERR_INVALIDCALL);
    // Unbinding always succeeds and reads back as null.
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 92, {device, 0}), D3D_OK);
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 93,
                     {device, static_cast<u32>(SCRATCH_ADDR + 0x400)}),
              D3D_OK);
    EXPECT_EQ(memory_.read_u32(SCRATCH_ADDR + 0x400).value(), 0u);
}

TEST_F(D3D9ManagerTest, CreateVertexShaderTranslatesAndRoundTripsItsFunction) {
    const auto [factory, device] = create_device();
    const std::vector<u32> tokens = simple_vertex_shader_tokens();
    write_shader_bytecode(SHADER_ADDR, tokens);

    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 91,
                     {device, static_cast<u32>(SHADER_ADDR),
                      static_cast<u32>(SCRATCH_ADDR)}),
              D3D_OK);
    const u32 shader = memory_.read_u32(SCRATCH_ADDR).value();
    ASSERT_NE(shader, 0u);

    // GetFunction with a null buffer is the documented size query.
    ASSERT_TRUE(memory_.write_u32(SCRATCH_ADDR + 0x100, 0).is_ok());
    EXPECT_EQ(invoke(D3D9InterfaceId::VertexShader, 4,
                     {shader, 0, static_cast<u32>(SCRATCH_ADDR + 0x100)}),
              D3D_OK);
    const u32 size = memory_.read_u32(SCRATCH_ADDR + 0x100).value();
    EXPECT_EQ(size, tokens.size() * sizeof(u32));

    // And with a buffer, it returns exactly what the guest supplied.
    EXPECT_EQ(invoke(D3D9InterfaceId::VertexShader, 4,
                     {shader, static_cast<u32>(SCRATCH_ADDR + 0x200),
                      static_cast<u32>(SCRATCH_ADDR + 0x100)}),
              D3D_OK);
    for (usize i = 0; i < tokens.size(); ++i) {
        EXPECT_EQ(memory_.read_u32(SCRATCH_ADDR + 0x200 + i * 4).value(),
                  tokens[i])
            << "token " << i;
    }

    // A buffer too small for the function is rejected rather than truncated.
    ASSERT_TRUE(memory_.write_u32(SCRATCH_ADDR + 0x100, 4).is_ok());
    EXPECT_EQ(invoke(D3D9InterfaceId::VertexShader, 4,
                     {shader, static_cast<u32>(SCRATCH_ADDR + 0x200),
                      static_cast<u32>(SCRATCH_ADDR + 0x100)}),
              D3DERR_INVALIDCALL);
}

TEST_F(D3D9ManagerTest, CreateShaderRejectsBytecodeForTheOtherStage) {
    const auto [factory, device] = create_device();
    write_shader_bytecode(SHADER_ADDR, simple_vertex_shader_tokens());
    // A vertex shader handed to CreatePixelShader.
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 106,
                     {device, static_cast<u32>(SHADER_ADDR),
                      static_cast<u32>(SCRATCH_ADDR)}),
              D3DERR_INVALIDCALL);
    EXPECT_EQ(memory_.read_u32(SCRATCH_ADDR).value(), 0u);
}

TEST_F(D3D9ManagerTest, CreateShaderRefusesBytecodeItCannotTranslate) {
    const auto [factory, device] = create_device();
    // vs_3_0 with an `if` — flow control has no translation.
    const std::vector<u32> tokens{
        0xFFFE0300u,
        31u | (2u << 24), 0u, 0x80000000u,          // dcl_position v0
        40u | (1u << 24), 0x80000000u | (2u << 11), // if c0
        0x0000FFFFu,
    };
    write_shader_bytecode(SHADER_ADDR, tokens);
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 91,
                     {device, static_cast<u32>(SHADER_ADDR),
                      static_cast<u32>(SCRATCH_ADDR)}),
              E_NOTIMPL_D3D);
    EXPECT_EQ(memory_.read_u32(SCRATCH_ADDR).value(), 0u);
}

TEST_F(D3D9ManagerTest, ProgrammableDrawUsesTheTranslatedShaderPair) {
    const auto [factory, device] = create_device();
    const u32 vertex_shader = create_vertex_shader(device);
    const u32 pixel_shader = create_pixel_shader(device);
    ASSERT_NE(vertex_shader, 0u);
    ASSERT_NE(pixel_shader, 0u);

    // The shader declares position at v0 and texcoord0 at v3, so the layout
    // must follow the shader's register numbers, not the FVF's element order.
    (void)bind_triangle_stream(device, 0x002 | 0x100);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 92, {device, vertex_shader}),
              D3D_OK);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 107, {device, pixel_shader}),
              D3D_OK);

    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 41, {device}), D3D_OK);
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 81, {device, 4, 0, 1}), D3D_OK);
    ASSERT_EQ(backend_.record.draws, 1u);

    const auto& desc = backend_.record.last_pipeline_desc;
    EXPECT_EQ(desc.vertex_entry_point, "sm3_vertex");
    EXPECT_EQ(desc.fragment_entry_point, "sm3_fragment");
    ASSERT_EQ(desc.vertex_attributes.size(), 2u);
    EXPECT_EQ(desc.vertex_attributes[0].shader_location, 0u);
    EXPECT_EQ(desc.vertex_attributes[0].offset, 0u);
    EXPECT_EQ(desc.vertex_attributes[1].shader_location, 3u);
    EXPECT_EQ(desc.vertex_attributes[1].offset, 12u);
    // Neither generated shader is the fixed-function one.
    EXPECT_EQ(desc.vertex_shader_msl.find("ff_vertex"), std::string::npos);
}

TEST_F(D3D9ManagerTest, ProgrammableVsWithFfPsKeepsDeclarationRegisterSlots) {
    // Learn: a VS input register is bound through the vertex declaration, not
    // remapped to sequential FVF order. A declaration + vs without a PS still
    // uses the dcl vn indices (here v0 and v3) on the Metal descriptor.
    const auto [factory, device] = create_device();
    const u32 vertex_shader = create_vertex_shader(device);
    ASSERT_NE(vertex_shader, 0u);

    struct Element {
        u16 stream;
        u16 offset;
        u8 type;
        u8 method;
        u8 usage;
        u8 usage_index;
    };
    const Element elements[] = {
        {0, 0, 2, 0, 0, 0},   // FLOAT3 POSITION0
        {0, 12, 1, 0, 5, 0},  // FLOAT2 TEXCOORD0
        {0xFF, 0, 0, 0, 0, 0},
    };
    ASSERT_TRUE(memory_.write_bytes(
        SCRATCH_ADDR + 0x700, reinterpret_cast<const Byte*>(elements),
        sizeof(elements)).is_ok());
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 86,
                     {device, static_cast<u32>(SCRATCH_ADDR + 0x700),
                      static_cast<u32>(SCRATCH_ADDR + 0x780)}), D3D_OK);
    const u32 declaration = memory_.read_u32(SCRATCH_ADDR + 0x780).value();
    ASSERT_NE(declaration, 0u);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 26,
                     {device, 20u * 3u, 0, 0, 0,
                      static_cast<u32>(SCRATCH_ADDR + 0x784), 0}), D3D_OK);
    const u32 buffer = memory_.read_u32(SCRATCH_ADDR + 0x784).value();
    ASSERT_NE(buffer, 0u);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 87, {device, declaration}),
              D3D_OK);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 100,
                     {device, 0, buffer, 0, 20}), D3D_OK);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 92, {device, vertex_shader}),
              D3D_OK);

    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 41, {device}), D3D_OK);
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 81, {device, 4, 0, 1}), D3D_OK);
    ASSERT_EQ(backend_.record.draws, 1u);

    const auto& desc = backend_.record.last_pipeline_desc;
    EXPECT_EQ(desc.vertex_entry_point, "sm3_vertex");
    EXPECT_EQ(desc.fragment_entry_point, "ff_fragment");
    ASSERT_EQ(desc.vertex_attributes.size(), 2u);
    EXPECT_EQ(desc.vertex_attributes[0].shader_location, 0u);
    EXPECT_EQ(desc.vertex_attributes[0].offset, 0u);
    EXPECT_EQ(desc.vertex_attributes[1].shader_location, 3u);
    EXPECT_EQ(desc.vertex_attributes[1].offset, 12u);
}

TEST_F(D3D9ManagerTest, ProgrammableDrawDisablesStaticallyAbsentMrtOutputs) {
    const auto [factory, device] = create_device();
    const u32 vertex_shader = create_vertex_shader(device);
    const u32 pixel_shader = create_pixel_shader(device);
    (void)factory;
    (void)bind_triangle_stream(device, 0x002 | 0x100);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 92, {device, vertex_shader}),
              D3D_OK);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 107, {device, pixel_shader}),
              D3D_OK);

    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 41, {device}), D3D_OK);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 81, {device, 4, 0, 1}), D3D_OK);

    const auto& masks = backend_.record.last_pipeline_desc.color_write_masks;
    EXPECT_EQ(masks[0], 0xFu);
    EXPECT_EQ(masks[1], 0u);
    EXPECT_EQ(masks[2], 0u);
    EXPECT_EQ(masks[3], 0u);
}

TEST_F(D3D9ManagerTest, ProgrammableDrawUploadsTheShaderConstantFile) {
    const auto [factory, device] = create_device();
    const u32 vertex_shader = create_vertex_shader(device);
    const u32 pixel_shader = create_pixel_shader(device);
    (void)bind_triangle_stream(device, 0x002 | 0x100);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 92, {device, vertex_shader}),
              D3D_OK);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 107, {device, pixel_shader}),
              D3D_OK);

    // The vertex shader reads c0..c3 for its m4x4.
    const std::array<float, 16> constants{
        1, 0, 0, 0, 0, 2, 0, 0, 0, 0, 3, 0, 4, 5, 6, 1};
    ASSERT_TRUE(memory_.write_bytes(
        SCRATCH_ADDR + 0x700,
        reinterpret_cast<const Byte*>(constants.data()),
        sizeof(constants)).is_ok());
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 94,
                     {device, 0, static_cast<u32>(SCRATCH_ADDR + 0x700), 4}),
              D3D_OK);

    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 41, {device}), D3D_OK);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 81, {device, 4, 0, 1}), D3D_OK);

    const std::vector<u8>& uploaded =
        backend_.record.draw_vertex_constants.front();
    ASSERT_EQ(uploaded.size(), sizeof(constants));
    std::array<float, 16> read_back{};
    std::memcpy(read_back.data(), uploaded.data(), sizeof(read_back));
    EXPECT_EQ(read_back, constants);
}

TEST_F(D3D9ManagerTest, ProgrammableDrawRejectsAnFvfMissingADeclaredInput) {
    const auto [factory, device] = create_device();
    const u32 vertex_shader = create_vertex_shader(device);
    const u32 pixel_shader = create_pixel_shader(device);
    // No texture coordinate, but the shader declares one.
    (void)bind_triangle_stream(device, 0x002 | 0x040);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 92, {device, vertex_shader}),
              D3D_OK);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 107, {device, pixel_shader}),
              D3D_OK);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 41, {device}), D3D_OK);
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 81, {device, 4, 0, 1}),
              D3DERR_INVALIDCALL);
    EXPECT_EQ(backend_.record.draws, 0u);
}

TEST_F(D3D9ManagerTest, DrawWithVertexShaderUsesFixedFunctionPixel) {
    const auto [factory, device] = create_device();
    const u32 vertex_shader = create_vertex_shader(device);
    ASSERT_NE(vertex_shader, 0u);
    (void)bind_triangle_stream(device, 0x002 | 0x100);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 92, {device, vertex_shader}),
              D3D_OK);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 41, {device}), D3D_OK);
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 81, {device, 4, 0, 1}), D3D_OK);
    EXPECT_EQ(backend_.record.draws, 1u);
    EXPECT_NE(backend_.record.last_pipeline_desc.vertex_shader_msl.find(
                  "Sm3Varyings"),
              std::string::npos);
    EXPECT_NE(backend_.record.last_pipeline_desc.fragment_shader_msl.find(
                  "ff_fragment"),
              std::string::npos);
}

TEST_F(D3D9ManagerTest, TexturedDrawBindsTheTextureAndItsSamplerState) {
    const auto [factory, device] = create_device();
    const u32 vertex_buffer = bind_triangle_stream(device, 0x002 | 0x040 | 0x100);
    (void)vertex_buffer;
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 23,
                     {device, 4, 4, 1, 0, 21, 1,
                      static_cast<u32>(SCRATCH_ADDR + 0x500), 0}),
              D3D_OK);
    const u32 texture = memory_.read_u32(SCRATCH_ADDR + 0x500).value();
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 65, {device, 0, texture}),
              D3D_OK);
    // ADDRESSU = MIRRORONCE, ADDRESSW = CLAMP, MAGFILTER = LINEAR,
    // BORDERCOLOR = opaque white, MIPMAPLODBIAS = 2.25f.
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 69, {device, 0, 1, 5}), D3D_OK);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 69, {device, 0, 3, 3}), D3D_OK);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 69, {device, 0, 5, 2}), D3D_OK);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 69,
                     {device, 0, 4, 0xFFFFFFFFu}),
              D3D_OK);
    const float lod_bias = 2.25f;
    u32 lod_bias_bits = 0;
    std::memcpy(&lod_bias_bits, &lod_bias, sizeof(lod_bias_bits));
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 69,
                     {device, 0, 8 /* MIPMAPLODBIAS */, lod_bias_bits}),
              D3D_OK);

    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 41, {device}), D3D_OK);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 81, {device, 4, 0, 1}), D3D_OK);

    ASSERT_EQ(backend_.record.draws, 1u);
    const graphics::BackendDrawCall& call = backend_.record.draw_calls.front();
    EXPECT_NE(call.textures[0], nullptr);
    EXPECT_EQ(call.samplers[0].address_u,
              graphics::BackendAddressMode::MirrorOnce);
    EXPECT_EQ(call.samplers[0].address_w, graphics::BackendAddressMode::Clamp);
    EXPECT_EQ(call.samplers[0].mag_filter, graphics::BackendFilter::Linear);
    EXPECT_EQ(call.samplers[0].border_color, 0xFFFFFFFFu);
    EXPECT_FLOAT_EQ(call.samplers[0].lod_bias, lod_bias);
    // The generated fragment shader samples the stage.
    EXPECT_NE(backend_.record.last_pipeline_desc.fragment_shader_msl.find(
                  "tex0.sample"),
              std::string::npos);
    EXPECT_NE(backend_.record.last_pipeline_desc.fragment_shader_msl.find(
                  "bias(cb.lod_biases[0])"),
              std::string::npos);
}
