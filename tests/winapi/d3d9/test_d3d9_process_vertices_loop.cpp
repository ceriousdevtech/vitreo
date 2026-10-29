/// @file test_d3d9_process_vertices_loop.cpp
/// @brief Programmable ProcessVertices: loops and predication.
///        Part of the D3D9 frontend contract tests (see test_d3d9.cpp).

#include "d3d9_test_fixture.hpp"

#include <array>
#include <cstring>
#include <vector>

using namespace vitreo;
using namespace vitreo::winapi;
using vitreo::winapi::d3d9_test::D3D9ManagerTest;

TEST_F(D3D9ManagerTest, ProcessVerticesProgrammableLoopRepBreak) {
    // Learn: loop aL,i# drives aL (count/init/step); rep repeats without aL;
    // break exits the nearest loop/rep. Matches sm3_to_msl iteration caps.
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
    // Token helpers match tests/graphics/test_sm3_to_msl.cpp encodings.
    auto dst = [&](u32 type, u32 index, u32 write_mask = 0xF) {
        return 0x80000000u | reg(type) | (index & 0x7FFu) |
               ((write_mask & 0xFu) << 16);
    };
    auto src = [&](u32 type, u32 index, u32 swizzle = 0xE4) {
        return 0x80000000u | reg(type) | (index & 0x7FFu) |
               ((swizzle & 0xFFu) << 16);
    };
    auto insn = [](u32 opcode, u32 length) {
        return opcode | (length << 24);
    };
    constexpr u32 TEMP = 0, INPUT = 1, CONST = 2, RASTOUT = 4, OUTPUT = 6;
    constexpr u32 CONSTINT = 7, LOOP = 15;
    constexpr u32 REL = 1u << 13;
    constexpr u32 SWZ_XXXX = 0x00u;
    const std::vector<u32> loop_vs{
        0xFFFE0300u,
        insn(31, 2), 0u, dst(INPUT, 0), // dcl_position v0
        // defi i0=(3,0,1,0) loop; i1=(2,0,0,0) rep; i2=(10,0,1,0) break
        insn(48, 5), dst(CONSTINT, 0), 3u, 0u, 1u, 0u,
        insn(48, 5), dst(CONSTINT, 1), 2u, 0u, 0u, 0u,
        insn(48, 5), dst(CONSTINT, 2), 10u, 0u, 1u, 0u,
        insn(81, 5), dst(CONST, 0), fbits(1.0f), fbits(0.0f), fbits(0.0f),
        fbits(0.0f),
        insn(81, 5), dst(CONST, 1), fbits(0.0f), fbits(1.0f), fbits(0.0f),
        fbits(0.0f),
        insn(81, 5), dst(CONST, 2), fbits(0.0f), fbits(0.0f), fbits(1.0f),
        fbits(0.0f),
        insn(81, 5), dst(CONST, 3), fbits(0.0f), fbits(0.0f), fbits(0.0f),
        fbits(0.0f),
        insn(1, 2), dst(TEMP, 0), src(CONST, 3), // mov r0, c3
        // loop: r0 += c0[aL] → (1,1,1,0)
        insn(27, 2), dst(LOOP, 0), src(CONSTINT, 0),
        insn(2, 3), dst(TEMP, 0), src(TEMP, 0), src(CONST, 0) | REL,
        src(LOOP, 0, SWZ_XXXX),
        insn(29, 0),
        insn(1, 2), dst(TEMP, 1), src(CONST, 3), // mov r1, c3
        // rep: r1.x += 1 twice → 2
        insn(38, 1), src(CONSTINT, 1),
        insn(2, 3), dst(TEMP, 1, 0x1), src(TEMP, 1),
        src(CONST, 0, SWZ_XXXX),
        insn(39, 0),
        // break: r1.y += 1 once despite i2.x=10
        insn(27, 2), dst(LOOP, 0), src(CONSTINT, 2),
        insn(2, 3), dst(TEMP, 1, 0x2), src(TEMP, 1),
        src(CONST, 0, SWZ_XXXX),
        insn(44, 0),
        insn(29, 0),
        // oT0.xy = (loop.x+rep.x, loop.y+break.y) = (3, 2)
        insn(2, 3), dst(TEMP, 0), src(TEMP, 0), src(TEMP, 1),
        insn(1, 2), dst(OUTPUT, 0), src(TEMP, 0),
        insn(1, 2), dst(RASTOUT, 0), src(INPUT, 0),
        0x0000FFFFu,
    };
    write_shader_bytecode(SHADER_ADDR + 0x400, loop_vs);
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
    };
    DestVert out{};
    ASSERT_TRUE(memory_
                    .read_bytes(dest_ptr, reinterpret_cast<Byte*>(&out),
                                sizeof(out))
                    .is_ok());
    // TEX1 stores .xy: (loop+rep, loop+break) = (3, 2).
    EXPECT_FLOAT_EQ(out.x, 320.0f);
    EXPECT_FLOAT_EQ(out.y, 240.0f);
    EXPECT_FLOAT_EQ(out.u0, 3.0f);
    EXPECT_FLOAT_EQ(out.v0, 2.0f);
}

TEST_F(D3D9ManagerTest, ProcessVerticesProgrammableSetpAndPredication) {
    // Learn: setp_comp fills p0; predicated math uses the instruction-token
    // bit + trailing p# source. Guard matches sm3_to_msl: if (any(p)) { … }.
    // True predicate writes oT0 from c2; false predicate skips overwrite.
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
    constexpr u32 PRED_BIT = 1u << 28;
    const std::vector<u32> pred_vs{
        0xFFFE0300u,
        31u | (2u << 24), 0u, DST_MASK | reg(1) | 0u, // dcl_position v0
        81u | (5u << 24), DST_MASK | reg(2) | 0u,     // def c0 = 1
        fbits(1.0f), fbits(1.0f), fbits(1.0f), fbits(1.0f),
        81u | (5u << 24), DST_MASK | reg(2) | 1u, // def c1 = 0
        fbits(0.0f), fbits(0.0f), fbits(0.0f), fbits(0.0f),
        81u | (5u << 24), DST_MASK | reg(2) | 2u, // def c2 = (3,4,…)
        fbits(3.0f), fbits(4.0f), fbits(0.0f), fbits(0.0f),
        81u | (5u << 24), DST_MASK | reg(2) | 3u, // def c3 = (9,9,…)
        fbits(9.0f), fbits(9.0f), fbits(0.0f), fbits(0.0f),
        // setp_gt p0, c0, c1 → true
        94u | (1u << 16) | (3u << 24), DST_MASK | reg(19) | 0u,
        0x80000000u | reg(2) | 0u | SWZ_XYZW,
        0x80000000u | reg(2) | 1u | SWZ_XYZW,
        // (p0) mov oT0, c2
        1u | PRED_BIT | (2u << 24), DST_MASK | reg(6) | 0u,
        0x80000000u | reg(2) | 2u | SWZ_XYZW,
        0x80000000u | reg(19) | 0u | SWZ_XYZW,
        // setp_gt p0, c1, c0 → false
        94u | (1u << 16) | (3u << 24), DST_MASK | reg(19) | 0u,
        0x80000000u | reg(2) | 1u | SWZ_XYZW,
        0x80000000u | reg(2) | 0u | SWZ_XYZW,
        // (p0) mov oT0, c3 — skipped
        1u | PRED_BIT | (2u << 24), DST_MASK | reg(6) | 0u,
        0x80000000u | reg(2) | 3u | SWZ_XYZW,
        0x80000000u | reg(19) | 0u | SWZ_XYZW,
        1u | (2u << 24), DST_MASK | reg(4) | 0u, // mov oPos, v0
        0x80000000u | reg(1) | 0u | SWZ_XYZW,
        0x0000FFFFu,
    };
    write_shader_bytecode(SHADER_ADDR + 0x400, pred_vs);
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
    EXPECT_FLOAT_EQ(out.u0, 3.0f);
    EXPECT_FLOAT_EQ(out.v0, 4.0f);
}
