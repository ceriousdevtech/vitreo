/// @file test_d3d9_process_vertices.cpp
/// @brief Fixed-function ProcessVertices.
///        Part of the D3D9 frontend contract tests (see test_d3d9.cpp).

#include "d3d9_test_fixture.hpp"

#include <array>
#include <vector>

using namespace vitreo;
using namespace vitreo::winapi;
using vitreo::winapi::d3d9_test::D3D9ManagerTest;

TEST_F(D3D9ManagerTest, ProcessVerticesTransformsXyzAndCopiesAttributes) {
    // Learn: ProcessVertices applies world*view*proj + viewport into a dest
    // VB created with a nonzero FVF (typically XYZRHW).
    const auto [factory, device] = create_device();
    constexpr u32 src_fvf = 0x002 | 0x040 | 0x100; // XYZ | DIFFUSE | TEX1
    constexpr u32 dest_fvf = 0x004 | 0x040 | 0x100; // XYZRHW | DIFFUSE | TEX1
    constexpr u32 src_stride = 12 + 4 + 8;
    constexpr u32 dest_stride = 16 + 4 + 8;

    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 26,
                     {device, src_stride * 2, 0, src_fvf, 0,
                      static_cast<u32>(SCRATCH_ADDR + 0x600), 0}),
              D3D_OK);
    const u32 src_vb = memory_.read_u32(SCRATCH_ADDR + 0x600).value();
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 26,
                     {device, dest_stride * 2, 0, dest_fvf, 0,
                      static_cast<u32>(SCRATCH_ADDR + 0x604), 0}),
              D3D_OK);
    const u32 dest_vb = memory_.read_u32(SCRATCH_ADDR + 0x604).value();
    ASSERT_NE(src_vb, 0u);
    ASSERT_NE(dest_vb, 0u);

    // Source vertex 0 at origin with diffuse + texcoord; vertex 1 unused.
    struct SrcVert {
        float x, y, z;
        u32 diffuse;
        float u, v;
    };
    const SrcVert src0{0.0f, 0.0f, 0.0f, 0xAABBCCDDu, 0.25f, 0.75f};
    ASSERT_EQ(invoke(D3D9InterfaceId::VertexBuffer, 11,
                     {src_vb, 0, 0, static_cast<u32>(SCRATCH_ADDR + 0x620), 0}),
              D3D_OK);
    const u32 src_ptr = memory_.read_u32(SCRATCH_ADDR + 0x620).value();
    ASSERT_TRUE(memory_
                    .write_bytes(src_ptr, reinterpret_cast<const Byte*>(&src0),
                                 sizeof(src0))
                    .is_ok());
    ASSERT_EQ(invoke(D3D9InterfaceId::VertexBuffer, 12, {src_vb}), D3D_OK);

    // Seed destination with a sentinel so copy behaviour is observable.
    std::array<u8, dest_stride * 2> dest_seed{};
    dest_seed.fill(0x5A);
    ASSERT_EQ(invoke(D3D9InterfaceId::VertexBuffer, 11,
                     {dest_vb, 0, 0, static_cast<u32>(SCRATCH_ADDR + 0x624), 0}),
              D3D_OK);
    const u32 dest_ptr = memory_.read_u32(SCRATCH_ADDR + 0x624).value();
    ASSERT_TRUE(memory_
                    .write_bytes(dest_ptr, dest_seed.data(), dest_seed.size())
                    .is_ok());
    ASSERT_EQ(invoke(D3D9InterfaceId::VertexBuffer, 12, {dest_vb}), D3D_OK);

    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 89, {device, src_fvf}), D3D_OK);
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 100,
                     {device, 0, src_vb, 0, src_stride}),
              D3D_OK);

    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 85,
                     {device, 0, 0, 1, dest_vb, 0, 0}),
              D3D_OK);

    struct DestVert {
        float x, y, z, rhw;
        u32 diffuse;
        float u, v;
    };
    DestVert out{};
    ASSERT_TRUE(memory_
                    .read_bytes(dest_ptr, reinterpret_cast<Byte*>(&out),
                                sizeof(out))
                    .is_ok());
    // Identity WVP + default 640×480 viewport → screen centre, rhw = 1.
    EXPECT_FLOAT_EQ(out.x, 320.0f);
    EXPECT_FLOAT_EQ(out.y, 240.0f);
    EXPECT_FLOAT_EQ(out.z, 0.0f);
    EXPECT_FLOAT_EQ(out.rhw, 1.0f);
    EXPECT_EQ(out.diffuse, 0xAABBCCDDu);
    EXPECT_FLOAT_EQ(out.u, 0.25f);
    EXPECT_FLOAT_EQ(out.v, 0.75f);
}

TEST_F(D3D9ManagerTest, ProcessVerticesDoNotCopyDataSkipsUnaffectedFields) {
    const auto [factory, device] = create_device();
    constexpr u32 src_fvf = 0x002 | 0x040;  // XYZ | DIFFUSE
    constexpr u32 dest_fvf = 0x004 | 0x040; // XYZRHW | DIFFUSE
    constexpr u32 src_stride = 16;
    constexpr u32 dest_stride = 20;

    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 26,
                     {device, src_stride, 0, src_fvf, 0,
                      static_cast<u32>(SCRATCH_ADDR + 0x600), 0}),
              D3D_OK);
    const u32 src_vb = memory_.read_u32(SCRATCH_ADDR + 0x600).value();
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 26,
                     {device, dest_stride, 0, dest_fvf, 0,
                      static_cast<u32>(SCRATCH_ADDR + 0x604), 0}),
              D3D_OK);
    const u32 dest_vb = memory_.read_u32(SCRATCH_ADDR + 0x604).value();

    const std::array<float, 3> pos{1.0f, 2.0f, 3.0f};
    const u32 src_diffuse = 0x11223344u;
    ASSERT_EQ(invoke(D3D9InterfaceId::VertexBuffer, 11,
                     {src_vb, 0, 0, static_cast<u32>(SCRATCH_ADDR + 0x620), 0}),
              D3D_OK);
    const u32 src_ptr = memory_.read_u32(SCRATCH_ADDR + 0x620).value();
    ASSERT_TRUE(memory_
                    .write_bytes(src_ptr, reinterpret_cast<const Byte*>(pos.data()),
                                 sizeof(pos))
                    .is_ok());
    ASSERT_TRUE(memory_.write_u32(src_ptr + 12, src_diffuse).is_ok());
    ASSERT_EQ(invoke(D3D9InterfaceId::VertexBuffer, 12, {src_vb}), D3D_OK);

    ASSERT_EQ(invoke(D3D9InterfaceId::VertexBuffer, 11,
                     {dest_vb, 0, 0, static_cast<u32>(SCRATCH_ADDR + 0x624), 0}),
              D3D_OK);
    const u32 dest_ptr = memory_.read_u32(SCRATCH_ADDR + 0x624).value();
    ASSERT_TRUE(memory_.write_u32(dest_ptr + 16, 0xDEADBEEFu).is_ok());
    ASSERT_EQ(invoke(D3D9InterfaceId::VertexBuffer, 12, {dest_vb}), D3D_OK);

    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 89, {device, src_fvf}), D3D_OK);
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 100,
                     {device, 0, src_vb, 0, src_stride}),
              D3D_OK);
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 85,
                     {device, 0, 0, 1, dest_vb, 0, D3DPV_DONOTCOPYDATA}),
              D3D_OK);

    EXPECT_EQ(memory_.read_u32(dest_ptr + 16).value(), 0xDEADBEEFu);
}

TEST_F(D3D9ManagerTest, ProcessVerticesValidatesArgsAndRefusesUnsupportedVs) {
    const auto [factory, device] = create_device();
    constexpr u32 src_fvf = 0x002;
    constexpr u32 dest_fvf = 0x004;
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 26,
                     {device, 36, 0, src_fvf, 0,
                      static_cast<u32>(SCRATCH_ADDR + 0x600), 0}),
              D3D_OK);
    const u32 src_vb = memory_.read_u32(SCRATCH_ADDR + 0x600).value();
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 26,
                     {device, 48, 0, dest_fvf, 0,
                      static_cast<u32>(SCRATCH_ADDR + 0x604), 0}),
              D3D_OK);
    const u32 dest_vb = memory_.read_u32(SCRATCH_ADDR + 0x604).value();
    // Dest created with FVF 0 is illegal for ProcessVertices.
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 26,
                     {device, 48, 0, 0, 0,
                      static_cast<u32>(SCRATCH_ADDR + 0x608), 0}),
              D3D_OK);
    const u32 dest_no_fvf = memory_.read_u32(SCRATCH_ADDR + 0x608).value();

    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 85,
                     {device, 0, 0, 1, 0, 0, 0}),
              D3DERR_INVALIDCALL);

    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 89, {device, src_fvf}), D3D_OK);
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 100,
                     {device, 0, src_vb, 0, 12}),
              D3D_OK);
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 85,
                     {device, 0, 0, 1, dest_no_fvf, 0, 0}),
              D3DERR_INVALIDCALL);

    // texkill remains outside the ProcessVertices CPU subset.
    // CreateVertexShader still accepts texkill (MSL path); ProcessVertices refuses.
    auto reg = [](u32 type) {
        return ((type & 0x7u) << 28) | (((type >> 3) & 0x3u) << 11);
    };
    constexpr u32 DST_MASK = 0x80000000u | (0xFu << 16);
    constexpr u32 SWZ_XYZW = 0xE4u << 16;
    const std::vector<u32> texkill_vs{
        0xFFFE0300u,
        31u | (2u << 24), 0u, DST_MASK | reg(1) | 0u, // dcl_position v0
        // texkill r0 — MSL ok; ProcessVertices refuses.
        65u | (1u << 24), // texkill
        DST_MASK | reg(0) | 0u,
        1u | (2u << 24), DST_MASK | reg(4) | 0u, // mov oPos, v0
        0x80000000u | reg(1) | 0u | SWZ_XYZW,
        0x0000FFFFu,
    };
    write_shader_bytecode(SHADER_ADDR + 0x400, texkill_vs);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 91,
                     {device, static_cast<u32>(SHADER_ADDR + 0x400),
                      static_cast<u32>(SCRATCH_ADDR + 0x810)}),
              D3D_OK);
    const u32 vs = memory_.read_u32(SCRATCH_ADDR + 0x810).value();
    ASSERT_NE(vs, 0u);
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 92, {device, vs}), D3D_OK);
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 85,
                     {device, 0, 0, 1, dest_vb, 0, 0}),
              E_NOTIMPL_D3D);
}
