/// @file test_d3d9_process_vertices_branch.cpp
/// @brief Programmable ProcessVertices: subroutines and branching.
///        Part of the D3D9 frontend contract tests (see test_d3d9.cpp).

#include "d3d9_test_fixture.hpp"

#include <array>
#include <cstring>
#include <vector>

using namespace vitreo;
using namespace vitreo::winapi;
using vitreo::winapi::d3d9_test::D3D9ManagerTest;

TEST_F(D3D9ManagerTest, ProcessVerticesProgrammableCallLabelRet) {
    // Learn: call/callnz/label/ret — forward subroutines; callnz gates on
    // defb; fall-through past label bodies matches sm3_to_msl separation.
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
    auto dst = [&](u32 type, u32 index, u32 write_mask = 0xF) {
        return 0x80000000u | reg(type) | (index & 0x7FFu) |
               ((write_mask & 0xFu) << 16);
    };
    auto src = [&](u32 type, u32 index, u32 swizzle = 0xE4) {
        return 0x80000000u | reg(type) | (index & 0x7FFu) |
               ((swizzle & 0xFFu) << 16);
    };
    auto insn = [](u32 opcode, u32 length, u32 control = 0) {
        return opcode | ((control & 0xFFu) << 16) | (length << 24);
    };
    constexpr u32 TEMP = 0, INPUT = 1, CONST = 2, RASTOUT = 4, OUTPUT = 6;
    constexpr u32 CONSTBOOL = 14, LABEL = 18;
    const std::vector<u32> call_vs{
        0xFFFE0300u,
        insn(31, 2), 0u, dst(INPUT, 0), // dcl_position v0
        insn(47, 2), dst(CONSTBOOL, 0), 1u, // defb b0 = true
        insn(47, 2), dst(CONSTBOOL, 1), 0u, // defb b1 = false
        insn(81, 5), dst(CONST, 0), fbits(1.0f), fbits(2.0f), fbits(0.0f),
        fbits(0.0f),
        insn(81, 5), dst(CONST, 1), fbits(3.0f), fbits(0.0f), fbits(0.0f),
        fbits(0.0f),
        insn(81, 5), dst(CONST, 2), fbits(0.0f), fbits(0.0f), fbits(0.0f),
        fbits(0.0f),
        insn(1, 2), dst(TEMP, 0), src(CONST, 2), // r0 = 0
        insn(25, 1), src(LABEL, 0),              // call l0 → r0.x = 1
        insn(26, 2), src(LABEL, 1), src(CONSTBOOL, 0), // callnz l1,b0 → r0.y = 2
        insn(26, 2), src(LABEL, 2), src(CONSTBOOL, 1), // callnz l2,b1 skipped
        insn(1, 2), dst(OUTPUT, 0), src(TEMP, 0),
        insn(1, 2), dst(RASTOUT, 0), src(INPUT, 0),
        insn(28, 0), // ret (end main)
        insn(30, 1), dst(LABEL, 0),
        insn(1, 2), dst(TEMP, 0, 0x1), src(CONST, 0), // r0.x = 1
        insn(28, 0),
        insn(30, 1), dst(LABEL, 1),
        insn(1, 2), dst(TEMP, 0, 0x2), src(CONST, 0), // r0.y = 2
        insn(28, 0),
        insn(30, 1), dst(LABEL, 2),
        insn(1, 2), dst(TEMP, 0, 0x1), src(CONST, 1), // would set r0.x = 3
        insn(28, 0),
        0x0000FFFFu,
    };
    write_shader_bytecode(SHADER_ADDR + 0x400, call_vs);
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
    EXPECT_FLOAT_EQ(out.u0, 1.0f);
    EXPECT_FLOAT_EQ(out.v0, 2.0f);
}

TEST_F(D3D9ManagerTest, ProcessVerticesProgrammableBreakcBreakp) {
    // Learn: breakc_comp / breakp exit the nearest loop/rep when the
    // comparison or predicate component is true (sm3_to_msl semantics).
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
    auto dst = [&](u32 type, u32 index, u32 write_mask = 0xF) {
        return 0x80000000u | reg(type) | (index & 0x7FFu) |
               ((write_mask & 0xFu) << 16);
    };
    auto src = [&](u32 type, u32 index, u32 swizzle = 0xE4) {
        return 0x80000000u | reg(type) | (index & 0x7FFu) |
               ((swizzle & 0xFFu) << 16);
    };
    auto insn = [](u32 opcode, u32 length, u32 control = 0) {
        return opcode | ((control & 0xFFu) << 16) | (length << 24);
    };
    constexpr u32 TEMP = 0, INPUT = 1, CONST = 2, RASTOUT = 4, OUTPUT = 6;
    constexpr u32 CONSTINT = 7, PREDICATE = 19;
    constexpr u32 SWZ_XXXX = 0x00u;
    const std::vector<u32> break_vs{
        0xFFFE0300u,
        insn(31, 2), 0u, dst(INPUT, 0),
        insn(48, 5), dst(CONSTINT, 0), 10u, 0u, 0u, 0u, // rep 10
        insn(81, 5), dst(CONST, 0), fbits(1.0f), fbits(0.0f), fbits(0.0f),
        fbits(0.0f),
        insn(81, 5), dst(CONST, 1), fbits(0.0f), fbits(0.0f), fbits(0.0f),
        fbits(0.0f),
        insn(81, 5), dst(CONST, 2), fbits(0.5f), fbits(0.0f), fbits(0.0f),
        fbits(0.0f),
        insn(1, 2), dst(TEMP, 0), src(CONST, 1), // r0 = 0
        // breakc_gt: first iter 0 > 0.5 false; after add 1 > 0.5 → break
        // so r0.x becomes 1.
        insn(38, 1), src(CONSTINT, 0),
        insn(45, 2, 1 /* GT */), src(TEMP, 0, SWZ_XXXX), src(CONST, 2, SWZ_XXXX),
        insn(2, 3), dst(TEMP, 0, 0x1), src(TEMP, 0), src(CONST, 0, SWZ_XXXX),
        insn(39, 0),
        // setp_gt c0.x, c2.x → true; breakp exits immediately → r0.y stays 0
        // then one forced add after a fresh rep would not run — write y via
        // a second loop that breaks on the first iteration before the add.
        insn(94, 3, 1 /* GT */), dst(PREDICATE, 0), src(CONST, 0, SWZ_XXXX),
        src(CONST, 2, SWZ_XXXX),
        insn(38, 1), src(CONSTINT, 0),
        insn(96, 1), src(PREDICATE, 0, SWZ_XXXX), // breakp p0.x
        insn(2, 3), dst(TEMP, 0, 0x2), src(TEMP, 0), src(CONST, 0, SWZ_XXXX),
        insn(39, 0),
        insn(1, 2), dst(OUTPUT, 0), src(TEMP, 0),
        insn(1, 2), dst(RASTOUT, 0), src(INPUT, 0),
        0x0000FFFFu,
    };
    write_shader_bytecode(SHADER_ADDR + 0x400, break_vs);
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
    // breakc leaves r0.x=1; breakp prevents the y add → r0.y=0.
    EXPECT_FLOAT_EQ(out.x, 320.0f);
    EXPECT_FLOAT_EQ(out.y, 240.0f);
    EXPECT_FLOAT_EQ(out.u0, 1.0f);
    EXPECT_FLOAT_EQ(out.v0, 0.0f);
}

TEST_F(D3D9ManagerTest, ProcessVerticesProgrammableIfElseEndif) {
    // Learn: if bool / if pred … else … endif. Static defb and dynamic p0
    // (single component, like sm3_to_msl) select which constant reaches oT0.
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
    constexpr u32 SWZ_XXXX = 0x00u << 16;
    const std::vector<u32> if_vs{
        0xFFFE0300u,
        31u | (2u << 24), 0u, DST_MASK | reg(1) | 0u, // dcl_position v0
        47u | (2u << 24), DST_MASK | reg(14) | 0u, 1u, // defb b0, TRUE
        81u | (5u << 24), DST_MASK | reg(2) | 0u,      // def c0 = 1
        fbits(1.0f), fbits(1.0f), fbits(1.0f), fbits(1.0f),
        81u | (5u << 24), DST_MASK | reg(2) | 1u, // def c1 = 0
        fbits(0.0f), fbits(0.0f), fbits(0.0f), fbits(0.0f),
        81u | (5u << 24), DST_MASK | reg(2) | 2u, // def c2 = (3,4)
        fbits(3.0f), fbits(4.0f), fbits(0.0f), fbits(0.0f),
        81u | (5u << 24), DST_MASK | reg(2) | 3u, // def c3 = (9,9)
        fbits(9.0f), fbits(9.0f), fbits(0.0f), fbits(0.0f),
        81u | (5u << 24), DST_MASK | reg(2) | 4u, // def c4 = (5,6)
        fbits(5.0f), fbits(6.0f), fbits(0.0f), fbits(0.0f),
        // if b0 → mov oT0, c2 else mov oT0, c3
        40u | (1u << 24), 0x80000000u | reg(14) | 0u | SWZ_XYZW,
        1u | (2u << 24), DST_MASK | reg(6) | 0u,
        0x80000000u | reg(2) | 2u | SWZ_XYZW,
        42u | (0u << 24),
        1u | (2u << 24), DST_MASK | reg(6) | 0u,
        0x80000000u | reg(2) | 3u | SWZ_XYZW,
        43u | (0u << 24),
        // setp_gt p0, c1, c0 → false; if p0.x → would overwrite with c4
        94u | (1u << 16) | (3u << 24), DST_MASK | reg(19) | 0u,
        0x80000000u | reg(2) | 1u | SWZ_XYZW,
        0x80000000u | reg(2) | 0u | SWZ_XYZW,
        40u | (1u << 24), 0x80000000u | reg(19) | 0u | SWZ_XXXX,
        1u | (2u << 24), DST_MASK | reg(6) | 0u,
        0x80000000u | reg(2) | 4u | SWZ_XYZW,
        43u | (0u << 24),
        1u | (2u << 24), DST_MASK | reg(4) | 0u, // mov oPos, v0
        0x80000000u | reg(1) | 0u | SWZ_XYZW,
        0x0000FFFFu,
    };
    write_shader_bytecode(SHADER_ADDR + 0x400, if_vs);
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

TEST_F(D3D9ManagerTest, ProcessVerticesProgrammableIfc) {
    // Learn: if_comp - vs — compare-form if (same controls as setp/breakc).
    // ifc_gt c0.x, c1.x is true → oT0 = c2; nested ifc_lt false keeps it.
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
    constexpr u32 SWZ_XXXX = 0x00u << 16;
    const std::vector<u32> ifc_vs{
        0xFFFE0300u,
        31u | (2u << 24), 0u, DST_MASK | reg(1) | 0u, // dcl_position v0
        81u | (5u << 24), DST_MASK | reg(2) | 0u,      // def c0 = 2
        fbits(2.0f), fbits(0.0f), fbits(0.0f), fbits(0.0f),
        81u | (5u << 24), DST_MASK | reg(2) | 1u, // def c1 = 1
        fbits(1.0f), fbits(0.0f), fbits(0.0f), fbits(0.0f),
        81u | (5u << 24), DST_MASK | reg(2) | 2u, // def c2 = (3,4)
        fbits(3.0f), fbits(4.0f), fbits(0.0f), fbits(0.0f),
        81u | (5u << 24), DST_MASK | reg(2) | 3u, // def c3 = (9,9)
        fbits(9.0f), fbits(9.0f), fbits(0.0f), fbits(0.0f),
        81u | (5u << 24), DST_MASK | reg(2) | 4u, // def c4 = (5,6)
        fbits(5.0f), fbits(6.0f), fbits(0.0f), fbits(0.0f),
        // ifc_gt c0.x, c1.x → true → mov oT0, c2 else mov oT0, c3
        41u | (1u << 16) | (2u << 24),
        0x80000000u | reg(2) | 0u | SWZ_XXXX,
        0x80000000u | reg(2) | 1u | SWZ_XXXX,
        1u | (2u << 24), DST_MASK | reg(6) | 0u,
        0x80000000u | reg(2) | 2u | SWZ_XYZW,
        42u | (0u << 24),
        1u | (2u << 24), DST_MASK | reg(6) | 0u,
        0x80000000u | reg(2) | 3u | SWZ_XYZW,
        43u | (0u << 24),
        // ifc_lt c0.x, c1.x → false; else would overwrite with c4
        41u | (4u << 16) | (2u << 24),
        0x80000000u | reg(2) | 0u | SWZ_XXXX,
        0x80000000u | reg(2) | 1u | SWZ_XXXX,
        1u | (2u << 24), DST_MASK | reg(6) | 0u,
        0x80000000u | reg(2) | 4u | SWZ_XYZW,
        43u | (0u << 24),
        1u | (2u << 24), DST_MASK | reg(4) | 0u, // mov oPos, v0
        0x80000000u | reg(1) | 0u | SWZ_XYZW,
        0x0000FFFFu,
    };
    write_shader_bytecode(SHADER_ADDR + 0x400, ifc_vs);
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
