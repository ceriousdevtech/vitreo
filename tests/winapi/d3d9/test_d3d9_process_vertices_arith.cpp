/// @file test_d3d9_process_vertices_arith.cpp
/// @brief Programmable ProcessVertices: arithmetic and compare opcodes.
///        Part of the D3D9 frontend contract tests (see test_d3d9.cpp).

#include "d3d9_test_fixture.hpp"

#include <array>
#include <cstring>
#include <vector>

using namespace vitreo;
using namespace vitreo::winapi;
using vitreo::winapi::d3d9_test::D3D9ManagerTest;

TEST_F(D3D9ManagerTest, ProcessVerticesProgrammableSubRcpMinMax) {
    // Learn: ProcessVertices CPU VS subset includes sub/rcp/min/max.
    // vs_3_0: def c0=1; def c1=-2; mov r0,v0; sub r1,r0,r0; max r2,r1,c1;
    //         min oPos.xyz,r2,c0; rcp oPos.w,c0.x → clip (0,0,0,1).
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
    constexpr u32 DST_W = 0x80000000u | (0x8u << 16);
    constexpr u32 SWZ_XYZW = 0xE4u << 16;
    constexpr u32 SWZ_XXXX = 0x00u << 16;
    const std::vector<u32> alu_vs{
        0xFFFE0300u,
        31u | (2u << 24), 0u, DST_MASK | reg(1) | 0u, // dcl_position v0
        81u | (5u << 24), DST_MASK | reg(2) | 0u,     // def c0, 1,1,1,1
        fbits(1.0f), fbits(1.0f), fbits(1.0f), fbits(1.0f),
        81u | (5u << 24), DST_MASK | reg(2) | 1u, // def c1, -2,-2,-2,-2
        fbits(-2.0f), fbits(-2.0f), fbits(-2.0f), fbits(-2.0f),
        1u | (2u << 24), DST_MASK | reg(0) | 0u, // mov r0, v0
        0x80000000u | reg(1) | 0u | SWZ_XYZW,
        3u | (3u << 24), DST_MASK | reg(0) | 1u, // sub r1, r0, r0
        0x80000000u | reg(0) | 0u | SWZ_XYZW,
        0x80000000u | reg(0) | 0u | SWZ_XYZW,
        11u | (3u << 24), DST_MASK | reg(0) | 2u, // max r2, r1, c1
        0x80000000u | reg(0) | 1u | SWZ_XYZW,
        0x80000000u | reg(2) | 1u | SWZ_XYZW,
        10u | (3u << 24), DST_XYZ | reg(4) | 0u, // min oPos.xyz, r2, c0
        0x80000000u | reg(0) | 2u | SWZ_XYZW,
        0x80000000u | reg(2) | 0u | SWZ_XYZW,
        6u | (2u << 24), DST_W | reg(4) | 0u, // rcp oPos.w, c0.x
        0x80000000u | reg(2) | 0u | SWZ_XXXX,
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

TEST_F(D3D9ManagerTest, ProcessVerticesProgrammableSgn) {
    // Learn: sgn - vs / sm3_to_msl sign() — per-component -1/0/+1.
    // sgn(c0)=(-1,0,1,1) → oT0; clip (0,0,0,1) via mul with c1.
    const auto [factory, device] = create_device();
    constexpr u32 src_fvf = 0x002;           // XYZ
    constexpr u32 dest_fvf = 0x004 | 0x100;  // XYZRHW | TEX1
    constexpr u32 src_stride = 12;
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
    constexpr u32 SWZ_XYZW = 0xE4u << 16;
    const std::vector<u32> sgn_vs{
        0xFFFE0300u,
        31u | (2u << 24), 0u, DST_MASK | reg(1) | 0u, // dcl_position v0
        81u | (5u << 24), DST_MASK | reg(2) | 0u,     // def c0
        fbits(-2.5f), fbits(0.0f), fbits(3.0f), fbits(0.125f),
        81u | (5u << 24), DST_MASK | reg(2) | 1u, // def c1 = (0,0,0,1)
        fbits(0.0f), fbits(0.0f), fbits(0.0f), fbits(1.0f),
        34u | (4u << 24), DST_MASK | reg(6) | 0u, // sgn oT0, c0, r1, r2
        0x80000000u | reg(2) | 0u | SWZ_XYZW,
        0x80000000u | reg(0) | 1u | SWZ_XYZW,
        0x80000000u | reg(0) | 2u | SWZ_XYZW,
        5u | (3u << 24), DST_MASK | reg(4) | 0u, // mul oPos, c1, c1
        0x80000000u | reg(2) | 1u | SWZ_XYZW,
        0x80000000u | reg(2) | 1u | SWZ_XYZW,
        0x0000FFFFu,
    };
    write_shader_bytecode(SHADER_ADDR + 0x400, sgn_vs);
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
    // sgn(-2.5,0,3,0.125) → (-1,0,1,1); TEX1 stores .xy
    EXPECT_FLOAT_EQ(out.u, -1.0f);
    EXPECT_FLOAT_EQ(out.v, 0.0f);
}

TEST_F(D3D9ManagerTest, ProcessVerticesProgrammableLrpCmp) {
    // Learn: lrp - vs = src0*(src1-src2)+src2; cmp = (src0>=0)?src1:src2.
    // sm3_to_msl: mix(src2,src1,src0) / select(src2,src1,src0>=0).
    const auto [factory, device] = create_device();
    constexpr u32 src_fvf = 0x002;           // XYZ
    constexpr u32 dest_fvf = 0x004 | 0x100;  // XYZRHW | TEX1
    constexpr u32 src_stride = 12;
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
    constexpr u32 SWZ_XYZW = 0xE4u << 16;
    const std::vector<u32> alu_vs{
        0xFFFE0300u,
        31u | (2u << 24), 0u, DST_MASK | reg(1) | 0u, // dcl_position v0
        81u | (5u << 24), DST_MASK | reg(2) | 0u,     // def c0 = lerp t
        fbits(0.25f), fbits(0.5f), fbits(0.75f), fbits(0.0f),
        81u | (5u << 24), DST_MASK | reg(2) | 1u, // def c1 = src1
        fbits(4.0f), fbits(8.0f), fbits(12.0f), fbits(16.0f),
        81u | (5u << 24), DST_MASK | reg(2) | 2u, // def c2 = src2
        fbits(0.0f), fbits(0.0f), fbits(0.0f), fbits(0.0f),
        81u | (5u << 24), DST_MASK | reg(2) | 3u, // def c3 = cmp selector
        fbits(-1.0f), fbits(0.0f), fbits(1.0f), fbits(0.5f),
        81u | (5u << 24), DST_MASK | reg(2) | 4u, // def c4 = cmp src1
        fbits(1.0f), fbits(1.0f), fbits(1.0f), fbits(1.0f),
        81u | (5u << 24), DST_MASK | reg(2) | 5u, // def c5 = cmp src2
        fbits(0.0f), fbits(0.0f), fbits(0.0f), fbits(0.0f),
        81u | (5u << 24), DST_MASK | reg(2) | 6u, // def c6 = (0,0,0,1)
        fbits(0.0f), fbits(0.0f), fbits(0.0f), fbits(1.0f),
        // lrp oT0, c0, c1, c2 → (1, 4, 9, 0)
        18u | (4u << 24), DST_MASK | reg(6) | 0u,
        0x80000000u | reg(2) | 0u | SWZ_XYZW,
        0x80000000u | reg(2) | 1u | SWZ_XYZW,
        0x80000000u | reg(2) | 2u | SWZ_XYZW,
        // cmp r0, c3, c4, c5 → (0, 1, 1, 1); mul with c6 → clip (0,0,0,1)
        88u | (4u << 24), DST_MASK | reg(0) | 0u,
        0x80000000u | reg(2) | 3u | SWZ_XYZW,
        0x80000000u | reg(2) | 4u | SWZ_XYZW,
        0x80000000u | reg(2) | 5u | SWZ_XYZW,
        5u | (3u << 24), DST_MASK | reg(4) | 0u, // mul oPos, r0, c6
        0x80000000u | reg(0) | 0u | SWZ_XYZW,
        0x80000000u | reg(2) | 6u | SWZ_XYZW,
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
    // lrp: 0.25*(4-0)+0=1, 0.5*8=4 → TEX1 .xy
    EXPECT_FLOAT_EQ(out.u, 1.0f);
    EXPECT_FLOAT_EQ(out.v, 4.0f);
}

TEST_F(D3D9ManagerTest, ProcessVerticesProgrammableAbsFrcSltSge) {
    // Learn: ProcessVertices CPU VS subset includes abs/frc/slt/sge.
    // vs_3_0: abs+frc → oT0; slt+sge → clip (0,0,0,1) via mul with c3.
    const auto [factory, device] = create_device();
    constexpr u32 src_fvf = 0x002;           // XYZ
    constexpr u32 dest_fvf = 0x004 | 0x100;  // XYZRHW | TEX1
    constexpr u32 src_stride = 12;
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
    constexpr u32 SWZ_XYZW = 0xE4u << 16;
    const std::vector<u32> alu_vs{
        0xFFFE0300u,
        31u | (2u << 24), 0u, DST_MASK | reg(1) | 0u, // dcl_position v0
        81u | (5u << 24), DST_MASK | reg(2) | 0u,     // def c0
        fbits(-1.25f), fbits(2.0f), fbits(-0.25f), fbits(4.75f),
        81u | (5u << 24), DST_MASK | reg(2) | 1u, // def c1 = 0
        fbits(0.0f), fbits(0.0f), fbits(0.0f), fbits(0.0f),
        81u | (5u << 24), DST_MASK | reg(2) | 2u, // def c2 = 1
        fbits(1.0f), fbits(1.0f), fbits(1.0f), fbits(1.0f),
        81u | (5u << 24), DST_MASK | reg(2) | 3u, // def c3 = (0,0,0,1)
        fbits(0.0f), fbits(0.0f), fbits(0.0f), fbits(1.0f),
        35u | (2u << 24), DST_MASK | reg(0) | 0u, // abs r0, c0
        0x80000000u | reg(2) | 0u | SWZ_XYZW,
        19u | (2u << 24), DST_MASK | reg(6) | 0u, // frc oT0, r0
        0x80000000u | reg(0) | 0u | SWZ_XYZW,
        12u | (3u << 24), DST_MASK | reg(0) | 1u, // slt r1, c1, c2
        0x80000000u | reg(2) | 1u | SWZ_XYZW,
        0x80000000u | reg(2) | 2u | SWZ_XYZW,
        13u | (3u << 24), DST_MASK | reg(0) | 2u, // sge r2, c1, c1
        0x80000000u | reg(2) | 1u | SWZ_XYZW,
        0x80000000u | reg(2) | 1u | SWZ_XYZW,
        5u | (3u << 24), DST_MASK | reg(0) | 3u, // mul r3, r1, r2
        0x80000000u | reg(0) | 1u | SWZ_XYZW,
        0x80000000u | reg(0) | 2u | SWZ_XYZW,
        5u | (3u << 24), DST_MASK | reg(4) | 0u, // mul oPos, r3, c3
        0x80000000u | reg(0) | 3u | SWZ_XYZW,
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
    // abs(c0)= (1.25,2,0.25,4.75); frc → (0.25,0,0.25,0.75); TEX1 stores .xy
    EXPECT_FLOAT_EQ(out.u, 0.25f);
    EXPECT_FLOAT_EQ(out.v, 0.0f);
}

TEST_F(D3D9ManagerTest, ProcessVerticesProgrammableLitDstExpLog) {
    // Learn: ProcessVertices CPU VS subset includes lit/dst/exp/expp/log/logp.
    // lit → oT0; dst → oT1; expp/logp cancel to clip (0,0,0,1).
    const auto [factory, device] = create_device();
    constexpr u32 src_fvf = 0x002;                  // XYZ
    constexpr u32 dest_fvf = 0x004 | 0x200;         // XYZRHW | TEX2
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
    constexpr u32 DST_XYZ = 0x80000000u | (0x7u << 16);
    constexpr u32 DST_W = 0x80000000u | (0x8u << 16);
    constexpr u32 SWZ_XYZW = 0xE4u << 16;
    constexpr u32 SWZ_XXXX = 0x00u << 16;
    const std::vector<u32> alu_vs{
        0xFFFE0300u,
        31u | (2u << 24), 0u, DST_MASK | reg(1) | 0u, // dcl_position v0
        // c0: N·L=0.5, N·H=0.25, power=2 → lit = (1, 0.5, 0.0625, 1)
        81u | (5u << 24), DST_MASK | reg(2) | 0u,
        fbits(0.5f), fbits(0.25f), fbits(0.0f), fbits(2.0f),
        // c1/c2 for dst → (1, 6, 9, 0.5)
        81u | (5u << 24), DST_MASK | reg(2) | 1u,
        fbits(0.0f), fbits(3.0f), fbits(9.0f), fbits(0.0f),
        81u | (5u << 24), DST_MASK | reg(2) | 2u,
        fbits(0.0f), fbits(2.0f), fbits(0.0f), fbits(0.5f),
        // c3=0 → exp2(0)=1; c4=2 → log2(2)=1 (expp/logp ≡ exp/log on CPU)
        81u | (5u << 24), DST_MASK | reg(2) | 3u,
        fbits(0.0f), fbits(0.0f), fbits(0.0f), fbits(0.0f),
        81u | (5u << 24), DST_MASK | reg(2) | 4u,
        fbits(2.0f), fbits(2.0f), fbits(2.0f), fbits(2.0f),
        16u | (2u << 24), DST_MASK | reg(0) | 2u, // lit r2, c0
        0x80000000u | reg(2) | 0u | SWZ_XYZW,
        1u | (2u << 24), DST_MASK | reg(6) | 0u, // mov oT0, r2
        0x80000000u | reg(0) | 2u | SWZ_XYZW,
        17u | (3u << 24), DST_MASK | reg(0) | 3u, // dst r3, c1, c2
        0x80000000u | reg(2) | 1u | SWZ_XYZW,
        0x80000000u | reg(2) | 2u | SWZ_XYZW,
        1u | (2u << 24), DST_MASK | reg(6) | 1u, // mov oT1, r3
        0x80000000u | reg(0) | 3u | SWZ_XYZW,
        78u | (2u << 24), DST_MASK | reg(0) | 0u, // expp r0, c3.x
        0x80000000u | reg(2) | 3u | SWZ_XXXX,
        79u | (2u << 24), DST_MASK | reg(0) | 1u, // logp r1, c4.x
        0x80000000u | reg(2) | 4u | SWZ_XXXX,
        3u | (3u << 24), DST_XYZ | reg(4) | 0u, // sub oPos.xyz, r0, r1
        0x80000000u | reg(0) | 0u | SWZ_XYZW,
        0x80000000u | reg(0) | 1u | SWZ_XYZW,
        1u | (2u << 24), DST_W | reg(4) | 0u, // mov oPos.w, r2.w (=1)
        0x80000000u | reg(0) | 2u | (0xFFu << 16), // .wwww
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
    EXPECT_FLOAT_EQ(out.u0, 1.0f);
    EXPECT_FLOAT_EQ(out.v0, 0.5f);
    EXPECT_FLOAT_EQ(out.u1, 1.0f);
    EXPECT_FLOAT_EQ(out.v1, 6.0f);
}
