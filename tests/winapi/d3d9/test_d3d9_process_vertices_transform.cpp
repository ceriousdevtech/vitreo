/// @file test_d3d9_process_vertices_transform.cpp
/// @brief Programmable ProcessVertices: source modifiers, transcendental and matrix ops.
///        Part of the D3D9 frontend contract tests (see test_d3d9.cpp).

#include "d3d9_test_fixture.hpp"

#include <array>
#include <cstring>
#include <vector>

using namespace vitreo;
using namespace vitreo::winapi;
using vitreo::winapi::d3d9_test::D3D9ManagerTest;

TEST_F(D3D9ManagerTest, ProcessVerticesProgrammablePowAndSrcMods) {
    // Learn: pow - vs = pow(abs(src0), src1) scalar replicate; source mods
    // apply after swizzle (_abs / _bias / _bx2 match sm3_to_msl).
    // pow(-2, 3) → 8 → oT0; mov with _abs/_bias/_bx2 → oT1.
    const auto [factory, device] = create_device();
    constexpr u32 src_fvf = 0x002;           // XYZ
    constexpr u32 dest_fvf = 0x004 | 0x200;  // XYZRHW | TEX2
    constexpr u32 src_stride = 12;
    constexpr u32 dest_stride = 16 + 8 + 8;

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

    const std::array<float, 3> src0{0.0f, 0.0f, 0.0f};
    ASSERT_EQ(invoke(D3D9InterfaceId::VertexBuffer, 11,
                     {src_vb, 0, 0, static_cast<u32>(SCRATCH_ADDR + 0x620), 0}),
              D3D_OK);
    const u32 src_ptr = memory_.read_u32(SCRATCH_ADDR + 0x620).value();
    ASSERT_TRUE(memory_
                    .write_bytes(src_ptr, reinterpret_cast<const Byte*>(src0.data()),
                                 sizeof(src0))
                    .is_ok());
    ASSERT_EQ(invoke(D3D9InterfaceId::VertexBuffer, 12, {src_vb}), D3D_OK);

    ASSERT_EQ(invoke(D3D9InterfaceId::VertexBuffer, 11,
                     {dest_vb, 0, 0, static_cast<u32>(SCRATCH_ADDR + 0x624), 0}),
              D3D_OK);
    const u32 dest_ptr = memory_.read_u32(SCRATCH_ADDR + 0x624).value();
    ASSERT_EQ(invoke(D3D9InterfaceId::VertexBuffer, 12, {dest_vb}), D3D_OK);

    auto reg = [](u32 type) {
        return ((type & 0x7u) << 28) | (((type >> 3) & 0x3u) << 11);
    };
    auto fbits = [](float f) {
        u32 bits = 0;
        std::memcpy(&bits, &f, sizeof(bits));
        return bits;
    };
    constexpr u32 DST_MASK = 0x80000000u | (0xFu << 16);
    constexpr u32 DST_XY = 0x80000000u | (0x3u << 16);
    constexpr u32 SWZ_XYZW = 0xE4u << 16;
    constexpr u32 SWZ_XXXX = 0x00u << 16;
    constexpr u32 MOD_ABS = 11u << 24;
    constexpr u32 MOD_BIAS = 2u << 24;
    constexpr u32 MOD_BX2 = 4u << 24; // D3DSPSM_SIGN
    const std::vector<u32> alu_vs{
        0xFFFE0300u,
        31u | (2u << 24), 0u, DST_MASK | reg(1) | 0u, // dcl_position v0
        81u | (5u << 24), DST_MASK | reg(2) | 0u,     // def c0 = (-2, …)
        fbits(-2.0f), fbits(0.0f), fbits(0.0f), fbits(0.0f),
        81u | (5u << 24), DST_MASK | reg(2) | 1u, // def c1 = (3, …)
        fbits(3.0f), fbits(0.0f), fbits(0.0f), fbits(0.0f),
        81u | (5u << 24), DST_MASK | reg(2) | 2u, // def c2 = (-0.75, 0.25, …)
        fbits(-0.75f), fbits(0.25f), fbits(0.0f), fbits(0.0f),
        81u | (5u << 24), DST_MASK | reg(2) | 3u, // def c3 = (0,0,0,1)
        fbits(0.0f), fbits(0.0f), fbits(0.0f), fbits(1.0f),
        // pow oT0, c0.x, c1.x → pow(abs(-2), 3) = 8
        32u | (3u << 24), DST_MASK | reg(6) | 0u,
        0x80000000u | reg(2) | 0u | SWZ_XXXX,
        0x80000000u | reg(2) | 1u | SWZ_XXXX,
        // mov r0.xy, c2_abs → (0.75, 0.25)
        1u | (2u << 24), DST_XY | reg(0) | 0u,
        0x80000000u | reg(2) | 2u | SWZ_XYZW | MOD_ABS,
        // mov r1.x, c2.y_bias → 0.25 - 0.5 = -0.25
        1u | (2u << 24), 0x80000000u | (0x1u << 16) | reg(0) | 1u,
        0x80000000u | reg(2) | 2u | (0x55u << 16) | MOD_BIAS,
        // mov r1.y, c2.y_bx2 → (0.25 - 0.5) * 2 = -0.5
        1u | (2u << 24), 0x80000000u | (0x2u << 16) | reg(0) | 1u,
        0x80000000u | reg(2) | 2u | (0x55u << 16) | MOD_BX2,
        // oT0.y = bias; oT1.xy = (_abs.x, _bx2)
        1u | (2u << 24), 0x80000000u | (0x2u << 16) | reg(6) | 0u,
        0x80000000u | reg(0) | 1u | SWZ_XXXX,
        1u | (2u << 24), 0x80000000u | (0x1u << 16) | reg(6) | 1u,
        0x80000000u | reg(0) | 0u | SWZ_XXXX,
        1u | (2u << 24), 0x80000000u | (0x2u << 16) | reg(6) | 1u,
        0x80000000u | reg(0) | 1u | (0x55u << 16),
        1u | (2u << 24), DST_MASK | reg(4) | 0u, // mov oPos, c3
        0x80000000u | reg(2) | 3u | SWZ_XYZW,
        0x0000FFFFu,
    };
    write_shader_bytecode(SHADER_ADDR + 0x400, alu_vs);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 91,
                     {device, static_cast<u32>(SHADER_ADDR + 0x400),
                      static_cast<u32>(SCRATCH_ADDR + 0x810)}),
              D3D_OK);
    const u32 vs = memory_.read_u32(SCRATCH_ADDR + 0x810).value();
    ASSERT_NE(vs, 0u);

    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 89, {device, src_fvf}), D3D_OK);
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 100,
                     {device, 0, src_vb, 0, src_stride}),
              D3D_OK);
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 92, {device, vs}), D3D_OK);
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 85,
                     {device, 0, 0, 1, dest_vb, 0, 0}),
              D3D_OK);

    struct DestVert {
        float x, y, z, rhw;
        float u0, v0;
        float u1, v1;
    };
    DestVert out{};
    ASSERT_TRUE(memory_
                    .read_bytes(dest_ptr, reinterpret_cast<Byte*>(&out),
                                sizeof(out))
                    .is_ok());
    EXPECT_FLOAT_EQ(out.x, 320.0f);
    EXPECT_FLOAT_EQ(out.y, 240.0f);
    EXPECT_FLOAT_EQ(out.z, 0.0f);
    EXPECT_FLOAT_EQ(out.rhw, 1.0f);
    EXPECT_FLOAT_EQ(out.u0, 8.0f);    // pow
    EXPECT_FLOAT_EQ(out.v0, -0.25f);  // _bias
    EXPECT_FLOAT_EQ(out.u1, 0.75f);   // _abs
    EXPECT_FLOAT_EQ(out.v1, -0.5f);   // _bx2
}

TEST_F(D3D9ManagerTest, ProcessVerticesProgrammableCrsAndRelativeConst) {
    // Learn: ProcessVertices CPU VS subset includes crs, mova, and c[a0.x+n].
    // crs((1,0,0),(0,1,0))=(0,0,1); mova a0.x←1 then oPos.w from c[a0.x+2]=c3.w.
    // (c[aL+n] is also accepted with aL=0, but CreateVertexShader/MSL refuses
    // aL relative outside loop…endloop.)
    const auto [factory, device] = create_device();
    constexpr u32 src_fvf = 0x002;  // XYZ
    constexpr u32 dest_fvf = 0x004; // XYZRHW
    constexpr u32 src_stride = 12;
    constexpr u32 dest_stride = 16;

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

    const std::array<float, 3> src0{0.0f, 0.0f, 0.0f};
    ASSERT_EQ(invoke(D3D9InterfaceId::VertexBuffer, 11,
                     {src_vb, 0, 0, static_cast<u32>(SCRATCH_ADDR + 0x620), 0}),
              D3D_OK);
    const u32 src_ptr = memory_.read_u32(SCRATCH_ADDR + 0x620).value();
    ASSERT_TRUE(memory_
                    .write_bytes(src_ptr, reinterpret_cast<const Byte*>(src0.data()),
                                 sizeof(src0))
                    .is_ok());
    ASSERT_EQ(invoke(D3D9InterfaceId::VertexBuffer, 12, {src_vb}), D3D_OK);

    ASSERT_EQ(invoke(D3D9InterfaceId::VertexBuffer, 11,
                     {dest_vb, 0, 0, static_cast<u32>(SCRATCH_ADDR + 0x624), 0}),
              D3D_OK);
    const u32 dest_ptr = memory_.read_u32(SCRATCH_ADDR + 0x624).value();
    ASSERT_EQ(invoke(D3D9InterfaceId::VertexBuffer, 12, {dest_vb}), D3D_OK);

    auto reg = [](u32 type) {
        return ((type & 0x7u) << 28) | (((type >> 3) & 0x3u) << 11);
    };
    auto fbits = [](float f) {
        u32 bits = 0;
        std::memcpy(&bits, &f, sizeof(bits));
        return bits;
    };
    constexpr u32 DST_MASK = 0x80000000u | (0xFu << 16);
    constexpr u32 DST_XYZ = 0x80000000u | (0x7u << 16);
    constexpr u32 DST_X = 0x80000000u | (0x1u << 16);
    constexpr u32 DST_W = 0x80000000u | (0x8u << 16);
    constexpr u32 SWZ_XYZW = 0xE4u << 16;
    constexpr u32 SWZ_XXXX = 0x00u << 16;
    constexpr u32 REL = 1u << 13;
    constexpr u32 ADDR = 3;
    const std::vector<u32> crs_vs{
        0xFFFE0300u,
        31u | (2u << 24), 0u, DST_MASK | reg(1) | 0u, // dcl_position v0
        81u | (5u << 24), DST_MASK | reg(2) | 0u,     // def c0, 0,0,0,9 (bait)
        fbits(0.0f), fbits(0.0f), fbits(0.0f), fbits(9.0f),
        81u | (5u << 24), DST_MASK | reg(2) | 1u, // def c1, 1,0,0,0
        fbits(1.0f), fbits(0.0f), fbits(0.0f), fbits(0.0f),
        81u | (5u << 24), DST_MASK | reg(2) | 2u, // def c2, 0,1,0,0
        fbits(0.0f), fbits(1.0f), fbits(0.0f), fbits(0.0f),
        81u | (5u << 24), DST_MASK | reg(2) | 3u, // def c3, 0,0,0,1
        fbits(0.0f), fbits(0.0f), fbits(0.0f), fbits(1.0f),
        81u | (5u << 24), DST_MASK | reg(2) | 4u, // def c4, 1,0,0,0
        fbits(1.0f), fbits(0.0f), fbits(0.0f), fbits(0.0f),
        33u | (3u << 24), DST_XYZ | reg(0) | 0u, // crs r0.xyz, c1, c2
        0x80000000u | reg(2) | 1u | SWZ_XYZW,
        0x80000000u | reg(2) | 2u | SWZ_XYZW,
        1u | (2u << 24), DST_XYZ | reg(4) | 0u, // mov oPos.xyz, r0
        0x80000000u | reg(0) | 0u | SWZ_XYZW,
        46u | (2u << 24), DST_X | reg(ADDR) | 0u, // mova a0.x, c4.x
        0x80000000u | reg(2) | 4u | SWZ_XXXX,
        // mov oPos.w, c2[a0.x] — a0.x=1 → c3; write .w = 1
        1u | (3u << 24), DST_W | reg(4) | 0u,
        0x80000000u | reg(2) | 2u | SWZ_XYZW | REL,
        0x80000000u | reg(ADDR) | 0u | SWZ_XXXX,
        0x0000FFFFu,
    };
    write_shader_bytecode(SHADER_ADDR + 0x400, crs_vs);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 91,
                     {device, static_cast<u32>(SHADER_ADDR + 0x400),
                      static_cast<u32>(SCRATCH_ADDR + 0x810)}),
              D3D_OK);
    const u32 vs = memory_.read_u32(SCRATCH_ADDR + 0x810).value();
    ASSERT_NE(vs, 0u);

    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 89, {device, src_fvf}), D3D_OK);
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 100,
                     {device, 0, src_vb, 0, src_stride}),
              D3D_OK);
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 92, {device, vs}), D3D_OK);
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 85,
                     {device, 0, 0, 1, dest_vb, 0, 0}),
              D3D_OK);

    struct DestVert {
        float x, y, z, rhw;
    };
    DestVert out{};
    ASSERT_TRUE(memory_
                    .read_bytes(dest_ptr, reinterpret_cast<Byte*>(&out),
                                sizeof(out))
                    .is_ok());
    // Clip (0,0,1,1) → viewport centre x/y, z=1, rhw=1.
    EXPECT_FLOAT_EQ(out.x, 320.0f);
    EXPECT_FLOAT_EQ(out.y, 240.0f);
    EXPECT_FLOAT_EQ(out.z, 1.0f);
    EXPECT_FLOAT_EQ(out.rhw, 1.0f);
}

TEST_F(D3D9ManagerTest, ProcessVerticesProgrammableSincosAndM3x2) {
    // Learn: ProcessVertices CPU VS subset includes sincos and m3x2.
    // Angle 0 → (cos,sin)=(1,0); zero 3x2 → clip xy=0; oPos.w from cos.
    const auto [factory, device] = create_device();
    constexpr u32 src_fvf = 0x002;  // XYZ
    constexpr u32 dest_fvf = 0x004; // XYZRHW
    constexpr u32 src_stride = 12;
    constexpr u32 dest_stride = 16;

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

    const std::array<float, 3> src0{1.0f, 2.0f, 3.0f};
    ASSERT_EQ(invoke(D3D9InterfaceId::VertexBuffer, 11,
                     {src_vb, 0, 0, static_cast<u32>(SCRATCH_ADDR + 0x620), 0}),
              D3D_OK);
    const u32 src_ptr = memory_.read_u32(SCRATCH_ADDR + 0x620).value();
    ASSERT_TRUE(memory_
                    .write_bytes(src_ptr, reinterpret_cast<const Byte*>(src0.data()),
                                 sizeof(src0))
                    .is_ok());
    ASSERT_EQ(invoke(D3D9InterfaceId::VertexBuffer, 12, {src_vb}), D3D_OK);

    ASSERT_EQ(invoke(D3D9InterfaceId::VertexBuffer, 11,
                     {dest_vb, 0, 0, static_cast<u32>(SCRATCH_ADDR + 0x624), 0}),
              D3D_OK);
    const u32 dest_ptr = memory_.read_u32(SCRATCH_ADDR + 0x624).value();
    ASSERT_EQ(invoke(D3D9InterfaceId::VertexBuffer, 12, {dest_vb}), D3D_OK);

    auto reg = [](u32 type) {
        return ((type & 0x7u) << 28) | (((type >> 3) & 0x3u) << 11);
    };
    auto fbits = [](float f) {
        u32 bits = 0;
        std::memcpy(&bits, &f, sizeof(bits));
        return bits;
    };
    constexpr u32 DST_MASK = 0x80000000u | (0xFu << 16);
    constexpr u32 DST_XY = 0x80000000u | (0x3u << 16);
    constexpr u32 DST_Z = 0x80000000u | (0x4u << 16);
    constexpr u32 DST_W = 0x80000000u | (0x8u << 16);
    constexpr u32 SWZ_XYZW = 0xE4u << 16;
    constexpr u32 SWZ_XXXX = 0x00u << 16;
    const std::vector<u32> alu_vs{
        0xFFFE0300u,
        31u | (2u << 24), 0u, DST_MASK | reg(1) | 0u, // dcl_position v0
        // c0/c1: zero 3x2 rows; c2: angle 0
        81u | (5u << 24), DST_MASK | reg(2) | 0u,
        fbits(0.0f), fbits(0.0f), fbits(0.0f), fbits(0.0f),
        81u | (5u << 24), DST_MASK | reg(2) | 1u,
        fbits(0.0f), fbits(0.0f), fbits(0.0f), fbits(0.0f),
        81u | (5u << 24), DST_MASK | reg(2) | 2u,
        fbits(0.0f), fbits(0.0f), fbits(0.0f), fbits(0.0f),
        24u | (3u << 24), DST_XY | reg(0) | 0u, // m3x2 r0.xy, v0, c0
        0x80000000u | reg(1) | 0u | SWZ_XYZW,
        0x80000000u | reg(2) | 0u | SWZ_XYZW,
        37u | (2u << 24), DST_XY | reg(0) | 1u, // sincos r1.xy, c2.x
        0x80000000u | reg(2) | 2u | SWZ_XXXX,
        1u | (2u << 24), DST_XY | reg(4) | 0u, // mov oPos.xy, r0
        0x80000000u | reg(0) | 0u | SWZ_XYZW,
        1u | (2u << 24), DST_Z | reg(4) | 0u, // mov oPos.z, r0.x (=0)
        0x80000000u | reg(0) | 0u | SWZ_XXXX,
        1u | (2u << 24), DST_W | reg(4) | 0u, // mov oPos.w, r1.x (=cos=1)
        0x80000000u | reg(0) | 1u | SWZ_XXXX,
        0x0000FFFFu,
    };
    write_shader_bytecode(SHADER_ADDR + 0x400, alu_vs);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 91,
                     {device, static_cast<u32>(SHADER_ADDR + 0x400),
                      static_cast<u32>(SCRATCH_ADDR + 0x810)}),
              D3D_OK);
    const u32 vs = memory_.read_u32(SCRATCH_ADDR + 0x810).value();
    ASSERT_NE(vs, 0u);

    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 89, {device, src_fvf}), D3D_OK);
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 100,
                     {device, 0, src_vb, 0, src_stride}),
              D3D_OK);
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 92, {device, vs}), D3D_OK);
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 85,
                     {device, 0, 0, 1, dest_vb, 0, 0}),
              D3D_OK);

    struct DestVert {
        float x, y, z, rhw;
    };
    DestVert out{};
    ASSERT_TRUE(memory_
                    .read_bytes(dest_ptr, reinterpret_cast<Byte*>(&out),
                                sizeof(out))
                    .is_ok());
    EXPECT_FLOAT_EQ(out.x, 320.0f);
    EXPECT_FLOAT_EQ(out.y, 240.0f);
    EXPECT_FLOAT_EQ(out.z, 0.0f);
    EXPECT_FLOAT_EQ(out.rhw, 1.0f);
}

TEST_F(D3D9ManagerTest, ProcessVerticesProgrammableM4x4Transforms) {
    // Learn: ProcessVertices with a bound VS applies the shader then viewport.
    // vs_3_0: dcl_position v0; dcl_texcoord0 v3; m4x4 oPos, v0, c0; mov oT0, v3.
    const auto [factory, device] = create_device();
    constexpr u32 src_fvf = 0x002 | 0x100;  // XYZ | TEX1
    constexpr u32 dest_fvf = 0x004 | 0x100; // XYZRHW | TEX1
    constexpr u32 src_stride = 12 + 8;
    constexpr u32 dest_stride = 16 + 8;

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

    struct SrcVert {
        float x, y, z;
        float u, v;
    };
    const SrcVert src0{0.0f, 0.0f, 0.0f, 0.25f, 0.75f};
    ASSERT_EQ(invoke(D3D9InterfaceId::VertexBuffer, 11,
                     {src_vb, 0, 0, static_cast<u32>(SCRATCH_ADDR + 0x620), 0}),
              D3D_OK);
    const u32 src_ptr = memory_.read_u32(SCRATCH_ADDR + 0x620).value();
    ASSERT_TRUE(memory_
                    .write_bytes(src_ptr, reinterpret_cast<const Byte*>(&src0),
                                 sizeof(src0))
                    .is_ok());
    ASSERT_EQ(invoke(D3D9InterfaceId::VertexBuffer, 12, {src_vb}), D3D_OK);

    ASSERT_EQ(invoke(D3D9InterfaceId::VertexBuffer, 11,
                     {dest_vb, 0, 0, static_cast<u32>(SCRATCH_ADDR + 0x624), 0}),
              D3D_OK);
    const u32 dest_ptr = memory_.read_u32(SCRATCH_ADDR + 0x624).value();
    ASSERT_EQ(invoke(D3D9InterfaceId::VertexBuffer, 12, {dest_vb}), D3D_OK);

    const u32 vs = create_vertex_shader(device);
    ASSERT_NE(vs, 0u);
    // Identity matrix in c0–c3 (row-major float4s) → clip = (0,0,0,1).
    const std::array<float, 16> identity{
        1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1,
    };
    ASSERT_TRUE(memory_
                    .write_bytes(SCRATCH_ADDR + 0x700,
                                 reinterpret_cast<const Byte*>(identity.data()),
                                 sizeof(identity))
                    .is_ok());
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 94,
                     {device, 0, static_cast<u32>(SCRATCH_ADDR + 0x700), 4}),
              D3D_OK);

    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 89, {device, src_fvf}), D3D_OK);
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 100,
                     {device, 0, src_vb, 0, src_stride}),
              D3D_OK);
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 92, {device, vs}), D3D_OK);
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 85,
                     {device, 0, 0, 1, dest_vb, 0, 0}),
              D3D_OK);

    struct DestVert {
        float x, y, z, rhw;
        float u, v;
    };
    DestVert out{};
    ASSERT_TRUE(memory_
                    .read_bytes(dest_ptr, reinterpret_cast<Byte*>(&out),
                                sizeof(out))
                    .is_ok());
    EXPECT_FLOAT_EQ(out.x, 320.0f);
    EXPECT_FLOAT_EQ(out.y, 240.0f);
    EXPECT_FLOAT_EQ(out.z, 0.0f);
    EXPECT_FLOAT_EQ(out.rhw, 1.0f);
    EXPECT_FLOAT_EQ(out.u, 0.25f);
    EXPECT_FLOAT_EQ(out.v, 0.75f);
}

TEST_F(D3D9ManagerTest, ProcessVerticesProgrammableDp4Transforms) {
    // Learn: four dp4 writes to oPos components equal m4x4 against c0–c3.
    // vs_3_0: dcl_position v0; dp4 oPos.x/y/z/w, v0, c0/c1/c2/c3.
    const auto [factory, device] = create_device();
    constexpr u32 src_fvf = 0x002;  // XYZ
    constexpr u32 dest_fvf = 0x004; // XYZRHW
    constexpr u32 src_stride = 12;
    constexpr u32 dest_stride = 16;

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

    const std::array<float, 3> src0{0.0f, 0.0f, 0.0f};
    ASSERT_EQ(invoke(D3D9InterfaceId::VertexBuffer, 11,
                     {src_vb, 0, 0, static_cast<u32>(SCRATCH_ADDR + 0x620), 0}),
              D3D_OK);
    const u32 src_ptr = memory_.read_u32(SCRATCH_ADDR + 0x620).value();
    ASSERT_TRUE(memory_
                    .write_bytes(src_ptr, reinterpret_cast<const Byte*>(src0.data()),
                                 sizeof(src0))
                    .is_ok());
    ASSERT_EQ(invoke(D3D9InterfaceId::VertexBuffer, 12, {src_vb}), D3D_OK);

    ASSERT_EQ(invoke(D3D9InterfaceId::VertexBuffer, 11,
                     {dest_vb, 0, 0, static_cast<u32>(SCRATCH_ADDR + 0x624), 0}),
              D3D_OK);
    const u32 dest_ptr = memory_.read_u32(SCRATCH_ADDR + 0x624).value();
    ASSERT_EQ(invoke(D3D9InterfaceId::VertexBuffer, 12, {dest_vb}), D3D_OK);

    auto reg = [](u32 type) {
        return ((type & 0x7u) << 28) | (((type >> 3) & 0x3u) << 11);
    };
    constexpr u32 DST_MASK = 0x80000000u | (0xFu << 16);
    constexpr u32 DST_X = 0x80000000u | (0x1u << 16);
    constexpr u32 DST_Y = 0x80000000u | (0x2u << 16);
    constexpr u32 DST_Z = 0x80000000u | (0x4u << 16);
    constexpr u32 DST_W = 0x80000000u | (0x8u << 16);
    const std::vector<u32> dp4_vs{
        0xFFFE0300u,
        31u | (2u << 24), 0u, DST_MASK | reg(1) | 0u, // dcl_position v0
        9u | (3u << 24), DST_X | reg(4) | 0u,         // dp4 oPos.x, v0, c0
        0x80000000u | reg(1) | 0u | (0xE4u << 16),
        0x80000000u | reg(2) | 0u | (0xE4u << 16),
        9u | (3u << 24), DST_Y | reg(4) | 0u,
        0x80000000u | reg(1) | 0u | (0xE4u << 16),
        0x80000000u | reg(2) | 1u | (0xE4u << 16),
        9u | (3u << 24), DST_Z | reg(4) | 0u,
        0x80000000u | reg(1) | 0u | (0xE4u << 16),
        0x80000000u | reg(2) | 2u | (0xE4u << 16),
        9u | (3u << 24), DST_W | reg(4) | 0u,
        0x80000000u | reg(1) | 0u | (0xE4u << 16),
        0x80000000u | reg(2) | 3u | (0xE4u << 16),
        0x0000FFFFu,
    };
    write_shader_bytecode(SHADER_ADDR + 0x400, dp4_vs);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 91,
                     {device, static_cast<u32>(SHADER_ADDR + 0x400),
                      static_cast<u32>(SCRATCH_ADDR + 0x810)}),
              D3D_OK);
    const u32 vs = memory_.read_u32(SCRATCH_ADDR + 0x810).value();
    ASSERT_NE(vs, 0u);

    // Identity matrix in c0–c3 → clip = (0,0,0,1) for position (0,0,0,1).
    const std::array<float, 16> identity{
        1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1,
    };
    ASSERT_TRUE(memory_
                    .write_bytes(SCRATCH_ADDR + 0x700,
                                 reinterpret_cast<const Byte*>(identity.data()),
                                 sizeof(identity))
                    .is_ok());
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 94,
                     {device, 0, static_cast<u32>(SCRATCH_ADDR + 0x700), 4}),
              D3D_OK);

    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 89, {device, src_fvf}), D3D_OK);
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 100,
                     {device, 0, src_vb, 0, src_stride}),
              D3D_OK);
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 92, {device, vs}), D3D_OK);
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 85,
                     {device, 0, 0, 1, dest_vb, 0, 0}),
              D3D_OK);

    struct DestVert {
        float x, y, z, rhw;
    };
    DestVert out{};
    ASSERT_TRUE(memory_
                    .read_bytes(dest_ptr, reinterpret_cast<Byte*>(&out),
                                sizeof(out))
                    .is_ok());
    EXPECT_FLOAT_EQ(out.x, 320.0f);
    EXPECT_FLOAT_EQ(out.y, 240.0f);
    EXPECT_FLOAT_EQ(out.z, 0.0f);
    EXPECT_FLOAT_EQ(out.rhw, 1.0f);
}
