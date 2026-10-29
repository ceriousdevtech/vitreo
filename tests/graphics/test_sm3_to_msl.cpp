// SPDX-License-Identifier: LGPL-2.1-only
/// @file test_sm3_to_msl.cpp
/// @brief Contract tests for the D3D9 shader-model 1–3 to MSL translator.

#include <gtest/gtest.h>

#include "graphics/shaders/sm3_to_msl.hpp"

#include <cstring>
#include <string>
#include <vector>

using namespace vitreo;
using namespace vitreo::graphics;

namespace {

// -----------------------------------------------------------------------------
// Token construction, mirroring what the D3D9 shader assembler emits
// -----------------------------------------------------------------------------

constexpr u32 vs_version(u32 major, u32 minor) {
    return 0xFFFE0000u | (major << 8) | minor;
}
constexpr u32 ps_version(u32 major, u32 minor) {
    return 0xFFFF0000u | (major << 8) | minor;
}
constexpr u32 END_TOKEN = 0x0000FFFF;

enum Reg : u32 {
    TEMP = 0, INPUT = 1, CONST = 2, TEXTURE = 3, RASTOUT = 4, ATTROUT = 5,
    OUTPUT = 6, COLOROUT = 8, SAMPLER = 10,
};

/// Register type is split across bits 12:11 (shifted to type bits 4:3) and
/// 30:28 (type bits 2:0), matching D3DSI_GETREGTYPE.
constexpr u32 register_bits(u32 type) {
    return ((type & 0x7) << 28) | (((type >> 3) & 0x3) << 11);
}

constexpr u32 dst(u32 type, u32 index, u32 write_mask = 0xF,
                  u32 modifier = 0) {
    return 0x80000000u | register_bits(type) | (index & 0x7FF) |
           ((write_mask & 0xF) << 16) | ((modifier & 0xF) << 20);
}

constexpr u32 src(u32 type, u32 index, u32 swizzle = 0xE4,
                  u32 modifier = 0) {
    return 0x80000000u | register_bits(type) | (index & 0x7FF) |
           ((swizzle & 0xFF) << 16) | ((modifier & 0xF) << 24);
}

constexpr u32 instruction(u32 opcode, u32 length, u32 control = 0) {
    return opcode | (control << 16) | (length << 24);
}

/// Swizzle selecting the same component four times, e.g. `.wwww`.
constexpr u32 replicate(u32 component) {
    return component | (component << 2) | (component << 4) | (component << 6);
}

u32 float_token(float value) {
    u32 bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

Sm3Shader translate(const std::vector<u32>& tokens) {
    return translate_sm3_to_msl(tokens.data(), tokens.size());
}

/// A minimal vertex shader: dcl_position v0; mov oPos, v0.
std::vector<u32> minimal_vertex_shader() {
    return {
        vs_version(3, 0),
        instruction(31 /* DCL */, 2), 0 /* usage position, index 0 */,
        dst(INPUT, 0),
        instruction(1 /* MOV */, 2), dst(RASTOUT, 0), src(INPUT, 0),
        END_TOKEN,
    };
}

/// A minimal pixel shader: mov oC0, c0.
std::vector<u32> minimal_pixel_shader() {
    return {
        ps_version(3, 0),
        instruction(1 /* MOV */, 2), dst(COLOROUT, 0), src(CONST, 0),
        END_TOKEN,
    };
}

} // namespace

// =============================================================================
// Stream framing
// =============================================================================

TEST(Sm3ToMslTest, RejectsAnEmptyStream) {
    const Sm3Shader shader = translate_sm3_to_msl(nullptr, 0);
    EXPECT_FALSE(shader.ok);
    EXPECT_FALSE(shader.failure.empty());
}

TEST(Sm3ToMslTest, RejectsAStreamThatIsNotAD3D9Shader) {
    const Sm3Shader shader = translate({0x44584243u, END_TOKEN});
    EXPECT_FALSE(shader.ok);
    EXPECT_NE(shader.failure.find("not a D3D9 shader"), std::string::npos);
}

TEST(Sm3ToMslTest, RejectsAStreamWithNoEndInstruction) {
    const Sm3Shader shader = translate({vs_version(3, 0)});
    EXPECT_FALSE(shader.ok);
    EXPECT_NE(shader.failure.find("END"), std::string::npos);
}

TEST(Sm3ToMslTest, IdentifiesStageAndShaderModel) {
    const Sm3Shader vertex = translate(minimal_vertex_shader());
    ASSERT_TRUE(vertex.ok) << vertex.failure;
    EXPECT_EQ(vertex.stage, ShaderStage::Vertex);
    EXPECT_EQ(vertex.major, 3u);
    EXPECT_EQ(vertex.minor, 0u);
    EXPECT_EQ(vertex.entry_point, "sm3_vertex");

    const Sm3Shader pixel = translate(minimal_pixel_shader());
    ASSERT_TRUE(pixel.ok) << pixel.failure;
    EXPECT_EQ(pixel.stage, ShaderStage::Fragment);
    EXPECT_EQ(pixel.entry_point, "sm3_fragment");
}

TEST(Sm3ToMslTest, VertexFogOutputBecomesAnInterpolatedVarying) {
    const std::vector<u32> tokens{
        vs_version(1, 1),
        instruction(1 /* MOV */, 2), dst(RASTOUT, 0), src(INPUT, 0),
        instruction(1 /* MOV */, 2), dst(RASTOUT, 1), src(CONST, 0),
        END_TOKEN,
    };

    const Sm3Shader shader = translate(tokens);
    ASSERT_TRUE(shader.ok) << shader.failure;
    EXPECT_TRUE(shader.writes_fog);
    EXPECT_NE(shader.msl.find("float4 fog [[user(fog)]]"),
              std::string::npos) << shader.msl;
    EXPECT_NE(shader.msl.find("out.fog = cb.c[0];"),
              std::string::npos) << shader.msl;
}

TEST(Sm3ToMslTest, PointSizeOutputIsSpecializedAndClampedForPointDraws) {
    const Sm3Shader shader = translate({
        vs_version(2, 0),
        instruction(31 /* DCL */, 2), 0 /* position0 */, dst(INPUT, 0),
        instruction(1 /* MOV */, 2), dst(RASTOUT, 0), src(INPUT, 0),
        instruction(1 /* MOV */, 2), dst(RASTOUT, 2), src(CONST, 0),
        END_TOKEN,
    });
    ASSERT_TRUE(shader.ok) << shader.failure;
    EXPECT_TRUE(shader.writes_point_size);
    EXPECT_NE(shader.msl.find("float4 point_size_value"), std::string::npos)
        << shader.msl;

    const std::string written =
        apply_sm3_point_size(shader.msl, shader.writes_point_size);
    EXPECT_NE(written.find("float point_size [[point_size]];"),
              std::string::npos) << written;
    EXPECT_NE(written.find(
                  "clamp(point_size_value.x, cb.point_params.y, "
                  "cb.point_params.z)"),
              std::string::npos) << written;

    const Sm3Shader fallback = translate(minimal_vertex_shader());
    ASSERT_TRUE(fallback.ok) << fallback.failure;
    const std::string defaulted = apply_sm3_point_size(fallback.msl, false);
    EXPECT_NE(defaulted.find("out.point_size = cb.point_params.x;"),
              std::string::npos) << defaulted;
}

TEST(Sm3ToMslTest, ClipPlaneSpecializationUsesClipSpacePosition) {
    const std::vector<u32> vs = {
        0xFFFE0200u,
        instruction(1 /* MOV */, 2), dst(RASTOUT, 0), src(INPUT, 0),
        END_TOKEN,
    };
    const Sm3Shader shader = translate_sm3_to_msl(vs.data(), vs.size());
    ASSERT_TRUE(shader.ok) << shader.failure;
    const std::string clipped = apply_sm3_clip_planes(shader.msl);
    EXPECT_NE(clipped.find("float clip_distances [[clip_distance]] [6];"),
              std::string::npos) << clipped;
    EXPECT_NE(clipped.find("float4 clip_planes[6];"), std::string::npos)
        << clipped;
    EXPECT_NE(clipped.find(
                  "dot(cb.clip_planes[clip_index], out.position)"),
              std::string::npos) << clipped;
}

TEST(Sm3ToMslTest, SkipsCommentBlocks) {
    // A comment token carries its length in bits 30:16; the payload is the
    // HLSL source or debug data a compiler embedded.
    std::vector<u32> tokens{vs_version(3, 0)};
    tokens.push_back(0xFFFEu | (3u << 16)); // comment, three tokens
    tokens.insert(tokens.end(), {0xDEADBEEF, 0xCAFEBABE, 0x12345678});
    tokens.push_back(instruction(31, 2));
    tokens.push_back(0);
    tokens.push_back(dst(INPUT, 0));
    tokens.push_back(instruction(1, 2));
    tokens.push_back(dst(RASTOUT, 0));
    tokens.push_back(src(INPUT, 0));
    tokens.push_back(END_TOKEN);

    const Sm3Shader shader = translate(tokens);
    ASSERT_TRUE(shader.ok) << shader.failure;
    EXPECT_NE(shader.msl.find("out.position = in.v0"), std::string::npos);
}

// =============================================================================
// Declarations
// =============================================================================

TEST(Sm3ToMslTest, ReportsVertexInputDeclarations) {
    const std::vector<u32> tokens{
        vs_version(3, 0),
        instruction(31, 2), 0 /* position */, dst(INPUT, 0),
        instruction(31, 2), 3 /* normal */, dst(INPUT, 1),
        // Texcoord usage 5, usage index 2.
        instruction(31, 2), 5u | (2u << 16), dst(INPUT, 4),
        instruction(1, 2), dst(RASTOUT, 0), src(INPUT, 0),
        END_TOKEN,
    };
    const Sm3Shader shader = translate(tokens);
    ASSERT_TRUE(shader.ok) << shader.failure;
    ASSERT_EQ(shader.inputs.size(), 3u);
    EXPECT_EQ(shader.inputs[0].usage, Sm3Usage::Position);
    EXPECT_EQ(shader.inputs[0].register_index, 0u);
    EXPECT_EQ(shader.inputs[1].usage, Sm3Usage::Normal);
    EXPECT_EQ(shader.inputs[1].register_index, 1u);
    EXPECT_EQ(shader.inputs[2].usage, Sm3Usage::TexCoord);
    EXPECT_EQ(shader.inputs[2].usage_index, 2u);
    EXPECT_EQ(shader.inputs[2].register_index, 4u);

    // Each declared input becomes an attribute at its register index.
    EXPECT_NE(shader.msl.find("float4 v0 [[attribute(0)]]"),
              std::string::npos);
    EXPECT_NE(shader.msl.find("float4 v4 [[attribute(4)]]"),
              std::string::npos);
}

TEST(Sm3ToMslTest, ReportsTheConstantRegisterHighWaterMark) {
    const std::vector<u32> tokens{
        vs_version(3, 0),
        instruction(31, 2), 0, dst(INPUT, 0),
        instruction(20 /* M4X4 */, 3), dst(RASTOUT, 0), src(INPUT, 0),
        src(CONST, 8),
        END_TOKEN,
    };
    const Sm3Shader shader = translate(tokens);
    ASSERT_TRUE(shader.ok) << shader.failure;
    // m4x4 reads c8..c11.
    EXPECT_EQ(shader.float_constant_count, 12u);
}

TEST(Sm3ToMslTest, DefinedConstantsBecomeLiteralsNotUniformReads) {
    const std::vector<u32> tokens{
        ps_version(3, 0),
        instruction(81 /* DEF */, 5), dst(CONST, 3),
        float_token(0.25f), float_token(0.5f), float_token(0.75f),
        float_token(1.0f),
        instruction(1, 2), dst(COLOROUT, 0), src(CONST, 3),
        END_TOKEN,
    };
    const Sm3Shader shader = translate(tokens);
    ASSERT_TRUE(shader.ok) << shader.failure;
    EXPECT_NE(shader.msl.find("const float4 def3 = float4(0.25, 0.5, 0.75, 1)"),
              std::string::npos);
    EXPECT_NE(shader.msl.find("out.color = def3"), std::string::npos);
    // A defined constant must not also be read from the uniform buffer.
    EXPECT_EQ(shader.msl.find("cb.c[3]"), std::string::npos);
    EXPECT_EQ(shader.float_constant_count, 0u);
}

TEST(Sm3ToMslTest, PixelColorOutputsMapToFourRenderTargets) {
    const std::vector<u32> tokens{
        ps_version(3, 0),
        instruction(1 /* MOV */, 2), dst(COLOROUT, 0), src(CONST, 0),
        instruction(1 /* MOV */, 2), dst(COLOROUT, 1), src(CONST, 1),
        instruction(1 /* MOV */, 2), dst(COLOROUT, 2), src(CONST, 2),
        instruction(1 /* MOV */, 2), dst(COLOROUT, 3), src(CONST, 3),
        END_TOKEN,
    };
    const Sm3Shader shader = translate(tokens);
    ASSERT_TRUE(shader.ok) << shader.failure;
    EXPECT_NE(shader.msl.find("float4 color [[color(0)]];"),
              std::string::npos);
    EXPECT_NE(shader.msl.find("float4 color1 [[color(1)]];"),
              std::string::npos);
    EXPECT_NE(shader.msl.find("float4 color2 [[color(2)]];"),
              std::string::npos);
    EXPECT_NE(shader.msl.find("float4 color3 [[color(3)]];"),
              std::string::npos);
    EXPECT_NE(shader.msl.find("out.color1 = cb.c[1];"), std::string::npos);
    EXPECT_NE(shader.msl.find("out.color2 = cb.c[2];"), std::string::npos);
    EXPECT_NE(shader.msl.find("out.color3 = cb.c[3];"), std::string::npos);
    EXPECT_TRUE(shader.color_outputs_written[0]);
    EXPECT_TRUE(shader.color_outputs_written[1]);
    EXPECT_TRUE(shader.color_outputs_written[2]);
    EXPECT_TRUE(shader.color_outputs_written[3]);
}

TEST(Sm3ToMslTest, StaticallyAbsentMrtOutputsAreNotDeclaredOrWritten) {
    const std::vector<u32> tokens{
        ps_version(3, 0),
        instruction(1 /* MOV */, 2), dst(COLOROUT, 0), src(CONST, 0),
        END_TOKEN,
    };
    const Sm3Shader shader = translate(tokens);
    ASSERT_TRUE(shader.ok) << shader.failure;
    EXPECT_TRUE(shader.color_outputs_written[0]);
    EXPECT_FALSE(shader.color_outputs_written[1]);
    EXPECT_FALSE(shader.color_outputs_written[2]);
    EXPECT_FALSE(shader.color_outputs_written[3]);
    EXPECT_EQ(shader.msl.find("[[color(1)]]"), std::string::npos);
    EXPECT_EQ(shader.msl.find("out.color1"), std::string::npos);
}

TEST(Sm3ToMslTest, DefinedIntegerConstantsBecomeLiterals) {
    // Learn: defi immediates take precedence over Set*ShaderConstantI.
    constexpr u32 CONSTINT = 7;
    const std::vector<u32> tokens{
        vs_version(3, 0),
        instruction(31 /* DCL */, 2), 0, dst(INPUT, 0),
        instruction(48 /* DEFI */, 5), dst(CONSTINT, 1),
        2u, 3u, 4u, 5u,
        instruction(1 /* MOV */, 2), dst(TEMP, 0), src(CONSTINT, 1),
        instruction(1 /* MOV */, 2), dst(RASTOUT, 0), src(INPUT, 0),
        END_TOKEN,
    };
    const Sm3Shader shader = translate(tokens);
    ASSERT_TRUE(shader.ok) << shader.failure;
    EXPECT_NE(shader.msl.find("const int4 defi1 = int4(2, 3, 4, 5)"),
              std::string::npos)
        << shader.msl;
    EXPECT_NE(shader.msl.find("float4(defi1)"), std::string::npos) << shader.msl;
    EXPECT_EQ(shader.msl.find("cb.i["), std::string::npos);
    EXPECT_EQ(shader.int_constant_count, 0u);
}

TEST(Sm3ToMslTest, RuntimeIntegerAndBoolConstantsBindThroughUniformBlock) {
    constexpr u32 CONSTINT = 7;
    constexpr u32 CONSTBOOL = 14;
    const std::vector<u32> tokens{
        vs_version(3, 0),
        instruction(31 /* DCL */, 2), 0, dst(INPUT, 0),
        // if b2
        instruction(40 /* IF */, 1), src(CONSTBOOL, 2),
        instruction(1 /* MOV */, 2), dst(TEMP, 0), src(CONSTINT, 0),
        instruction(43 /* ENDIF */, 0),
        instruction(1 /* MOV */, 2), dst(RASTOUT, 0), src(INPUT, 0),
        END_TOKEN,
    };
    const Sm3Shader shader = translate(tokens);
    ASSERT_TRUE(shader.ok) << shader.failure;
    EXPECT_NE(shader.msl.find("int4 i[1]"), std::string::npos) << shader.msl;
    EXPECT_NE(shader.msl.find("int b[3]"), std::string::npos) << shader.msl;
    EXPECT_NE(shader.msl.find("if ((cb.b[2] != 0))"), std::string::npos)
        << shader.msl;
    EXPECT_NE(shader.msl.find("float4(cb.i[0])"), std::string::npos)
        << shader.msl;
    EXPECT_EQ(shader.int_constant_count, 1u);
    EXPECT_EQ(shader.bool_constant_count, 3u);
}

TEST(Sm3ToMslTest, DefinedBoolConstantsBecomeLiteralsInIf) {
    constexpr u32 CONSTBOOL = 14;
    const std::vector<u32> tokens{
        vs_version(3, 0),
        instruction(31 /* DCL */, 2), 0, dst(INPUT, 0),
        instruction(47 /* DEFB */, 2), dst(CONSTBOOL, 0), 1u,
        instruction(40 /* IF */, 1), src(CONSTBOOL, 0),
        instruction(1 /* MOV */, 2), dst(TEMP, 0), src(INPUT, 0),
        instruction(43 /* ENDIF */, 0),
        instruction(1 /* MOV */, 2), dst(RASTOUT, 0), src(TEMP, 0),
        END_TOKEN,
    };
    const Sm3Shader shader = translate(tokens);
    ASSERT_TRUE(shader.ok) << shader.failure;
    EXPECT_NE(shader.msl.find("const bool defb0 = true"), std::string::npos)
        << shader.msl;
    EXPECT_NE(shader.msl.find("if (defb0)"), std::string::npos) << shader.msl;
    EXPECT_EQ(shader.msl.find("cb.b["), std::string::npos);
    EXPECT_EQ(shader.bool_constant_count, 0u);
}

TEST(Sm3ToMslTest, RepEndrepEmitsClampedForLoop) {
    // Learn: rep i# — iterate i.x times in [0,255].
    constexpr u32 CONSTINT = 7;
    const std::vector<u32> tokens{
        vs_version(3, 0),
        instruction(31 /* DCL */, 2), 0, dst(INPUT, 0),
        instruction(48 /* DEFI */, 5), dst(CONSTINT, 2),
        3u, 0u, 0u, 0u,
        instruction(38 /* REP */, 1), src(CONSTINT, 2),
        instruction(2 /* ADD */, 3), dst(TEMP, 0), src(TEMP, 0), src(CONST, 0),
        instruction(39 /* ENDREP */, 0),
        instruction(1 /* MOV */, 2), dst(RASTOUT, 0), src(INPUT, 0),
        END_TOKEN,
    };
    const Sm3Shader shader = translate(tokens);
    ASSERT_TRUE(shader.ok) << shader.failure;
    EXPECT_NE(shader.msl.find("clamp((defi2).x, 0, 255)"), std::string::npos)
        << shader.msl;
    EXPECT_NE(shader.msl.find("for (int _rk0 = 0; _rk0 < _rc0; ++_rk0)"),
              std::string::npos)
        << shader.msl;
    EXPECT_NE(shader.msl.find("r0 = r0 + cb.c[0];"), std::string::npos)
        << shader.msl;
}

TEST(Sm3ToMslTest, LoopEndloopUsesALAndRelativeConstants) {
    // Learn: loop aL, i# ; add r1, r0, c2[aL] ; endloop
    constexpr u32 CONSTINT = 7;
    constexpr u32 LOOP = 15;
    const std::vector<u32> tokens{
        vs_version(3, 0),
        instruction(31 /* DCL */, 2), 0, dst(INPUT, 0),
        instruction(48 /* DEFI */, 5), dst(CONSTINT, 3),
        2u, 1u, 1u, 0u, // count=2, aL0=1, step=1
        instruction(27 /* LOOP */, 2), dst(LOOP, 0), src(CONSTINT, 3),
        instruction(2 /* ADD */, 3), dst(TEMP, 1), src(TEMP, 0),
        src(CONST, 2) | (1u << 13), src(LOOP, 0),
        instruction(29 /* ENDLOOP */, 0),
        instruction(1 /* MOV */, 2), dst(RASTOUT, 0), src(INPUT, 0),
        END_TOKEN,
    };
    const Sm3Shader shader = translate(tokens);
    ASSERT_TRUE(shader.ok) << shader.failure;
    EXPECT_NE(shader.msl.find("int aL = clamp(_li0.y, 0, 255);"),
              std::string::npos)
        << shader.msl;
    EXPECT_NE(shader.msl.find("sm3_c(cb, aL + 2)"), std::string::npos)
        << shader.msl;
    EXPECT_NE(shader.msl.find("aL += _li0.z;"), std::string::npos) << shader.msl;
    EXPECT_EQ(shader.float_constant_count, 256u);
    // Relative c[aL] reserves the full VS float file: 256 * 16 = 4096 bytes,
    // Metal's set*Bytes ceiling. `defi` is an immediate, not packed into
    // cb.i[], so the bound constant blob is exactly 4 KiB and must take the
    // bump-allocated slab path rather than a recycled inline bind.
    EXPECT_EQ(shader.int_constant_count, 0u);
    const usize packed_bytes =
        static_cast<usize>(shader.float_constant_count) * 16u;
    EXPECT_EQ(packed_bytes, 4096u);
}

TEST(Sm3ToMslTest, BreakEmitsInsideLoop) {
    constexpr u32 CONSTINT = 7;
    constexpr u32 LOOP = 15;
    const std::vector<u32> tokens{
        vs_version(3, 0),
        instruction(31 /* DCL */, 2), 0, dst(INPUT, 0),
        instruction(48 /* DEFI */, 5), dst(CONSTINT, 0),
        8u, 0u, 1u, 0u,
        instruction(27 /* LOOP */, 2), dst(LOOP, 0), src(CONSTINT, 0),
        instruction(2 /* ADD */, 3), dst(TEMP, 0), src(TEMP, 0), src(CONST, 0),
        instruction(44 /* BREAK */, 0),
        instruction(29 /* ENDLOOP */, 0),
        instruction(1 /* MOV */, 2), dst(RASTOUT, 0), src(INPUT, 0),
        END_TOKEN,
    };
    const Sm3Shader shader = translate(tokens);
    ASSERT_TRUE(shader.ok) << shader.failure;
    EXPECT_NE(shader.msl.find("break;"), std::string::npos) << shader.msl;
}

TEST(Sm3ToMslTest, BreakcEmitsConditionalBreak) {
    // Learn: break_gt src0, src1 — scalar compare, then break.
    constexpr u32 CONSTINT = 7;
    const std::vector<u32> tokens{
        vs_version(3, 0),
        instruction(31 /* DCL */, 2), 0, dst(INPUT, 0),
        instruction(48 /* DEFI */, 5), dst(CONSTINT, 1),
        4u, 0u, 0u, 0u,
        instruction(38 /* REP */, 1), src(CONSTINT, 1),
        instruction(45 /* BREAKC */, 2, 1 /* GT */), src(CONST, 0, replicate(0)),
        src(CONST, 1, replicate(0)),
        instruction(2 /* ADD */, 3), dst(TEMP, 0), src(TEMP, 0), src(CONST, 2),
        instruction(39 /* ENDREP */, 0),
        instruction(1 /* MOV */, 2), dst(RASTOUT, 0), src(INPUT, 0),
        END_TOKEN,
    };
    const Sm3Shader shader = translate(tokens);
    ASSERT_TRUE(shader.ok) << shader.failure;
    EXPECT_NE(shader.msl.find("if ((cb.c[0].x) > (cb.c[1].x)) {"),
              std::string::npos)
        << shader.msl;
    EXPECT_NE(shader.msl.find("break;"), std::string::npos) << shader.msl;
}

TEST(Sm3ToMslTest, BreakpEmitsPredicateBreak) {
    // Learn: breakp p0.x — optional NOT via source modifier.
    constexpr u32 CONSTINT = 7;
    constexpr u32 PREDICATE = 19;
    const std::vector<u32> tokens{
        vs_version(3, 0),
        instruction(31 /* DCL */, 2), 0, dst(INPUT, 0),
        instruction(48 /* DEFI */, 5), dst(CONSTINT, 0),
        4u, 0u, 0u, 0u,
        instruction(94 /* SETP */, 3, 1 /* GT */), dst(PREDICATE, 0),
        src(CONST, 0, replicate(0)), src(CONST, 1, replicate(0)),
        instruction(38 /* REP */, 1), src(CONSTINT, 0),
        instruction(96 /* BREAKP */, 1), src(PREDICATE, 0, replicate(0)),
        instruction(2 /* ADD */, 3), dst(TEMP, 0), src(TEMP, 0), src(CONST, 2),
        instruction(39 /* ENDREP */, 0),
        instruction(1 /* MOV */, 2), dst(RASTOUT, 0), src(INPUT, 0),
        END_TOKEN,
    };
    const Sm3Shader shader = translate(tokens);
    ASSERT_TRUE(shader.ok) << shader.failure;
    EXPECT_NE(shader.msl.find("if (p0.x) {"), std::string::npos) << shader.msl;
    EXPECT_NE(shader.msl.find("break;"), std::string::npos) << shader.msl;
}

TEST(Sm3ToMslTest, UnclosedLoopIsRejected) {
    constexpr u32 CONSTINT = 7;
    constexpr u32 LOOP = 15;
    const std::vector<u32> tokens{
        vs_version(3, 0),
        instruction(31 /* DCL */, 2), 0, dst(INPUT, 0),
        instruction(27 /* LOOP */, 2), dst(LOOP, 0), src(CONSTINT, 0),
        instruction(1 /* MOV */, 2), dst(RASTOUT, 0), src(INPUT, 0),
        END_TOKEN,
    };
    const Sm3Shader shader = translate(tokens);
    EXPECT_FALSE(shader.ok);
    EXPECT_NE(shader.failure.find("unclosed loop"), std::string::npos)
        << shader.failure;
}

// =============================================================================
// Instruction translation
// =============================================================================

TEST(Sm3ToMslTest, TranslatesArithmetic) {
    const std::vector<u32> tokens{
        ps_version(3, 0),
        instruction(2 /* ADD */, 3), dst(TEMP, 0), src(CONST, 0),
        src(CONST, 1),
        instruction(5 /* MUL */, 3), dst(TEMP, 1), src(TEMP, 0), src(CONST, 2),
        instruction(4 /* MAD */, 4), dst(TEMP, 2), src(TEMP, 0), src(TEMP, 1),
        src(CONST, 3),
        instruction(10 /* MIN */, 3), dst(TEMP, 3), src(TEMP, 2),
        src(CONST, 4),
        instruction(1, 2), dst(COLOROUT, 0), src(TEMP, 3),
        END_TOKEN,
    };
    const Sm3Shader shader = translate(tokens);
    ASSERT_TRUE(shader.ok) << shader.failure;
    EXPECT_NE(shader.msl.find("r0 = cb.c[0] + cb.c[1];"), std::string::npos);
    EXPECT_NE(shader.msl.find("r1 = r0 * cb.c[2];"), std::string::npos);
    EXPECT_NE(shader.msl.find("r2 = r0 * r1 + cb.c[3];"), std::string::npos);
    EXPECT_NE(shader.msl.find("r3 = min(r2, cb.c[4]);"), std::string::npos);
}

TEST(Sm3ToMslTest, DotProductsReplicateAcrossTheWriteMask) {
    const std::vector<u32> tokens{
        ps_version(3, 0),
        // dp3 r0.xyz, c0, c1
        instruction(8 /* DP3 */, 3), dst(TEMP, 0, 0x7), src(CONST, 0),
        src(CONST, 1),
        // dp4 r1.w, c0, c1
        instruction(9 /* DP4 */, 3), dst(TEMP, 1, 0x8), src(CONST, 0),
        src(CONST, 1),
        instruction(1, 2), dst(COLOROUT, 0), src(TEMP, 0),
        END_TOKEN,
    };
    const Sm3Shader shader = translate(tokens);
    ASSERT_TRUE(shader.ok) << shader.failure;
    EXPECT_NE(shader.msl.find("r0.xyz = float3(dot(cb.c[0].xyz, cb.c[1].xyz))"),
              std::string::npos);
    EXPECT_NE(shader.msl.find("r1.w = dot(cb.c[0], cb.c[1])"),
              std::string::npos);
}

TEST(Sm3ToMslTest, ScalarInstructionsReadOneComponentAndReplicate) {
    const std::vector<u32> tokens{
        ps_version(3, 0),
        // rcp r0, c0.w
        instruction(6 /* RCP */, 2), dst(TEMP, 0), src(CONST, 0, replicate(3)),
        // rsq r1.x, c1.y
        instruction(7 /* RSQ */, 2), dst(TEMP, 1, 0x1),
        src(CONST, 1, replicate(1)),
        instruction(1, 2), dst(COLOROUT, 0), src(TEMP, 0),
        END_TOKEN,
    };
    const Sm3Shader shader = translate(tokens);
    ASSERT_TRUE(shader.ok) << shader.failure;
    EXPECT_NE(shader.msl.find("r0 = float4((1.0 / cb.c[0].w))"),
              std::string::npos);
    EXPECT_NE(shader.msl.find("r1.x = rsqrt(abs(cb.c[1].y))"),
              std::string::npos);
}

TEST(Sm3ToMslTest, MatrixMultipliesExpandIntoConsecutiveConstantRows) {
    const std::vector<u32> tokens{
        vs_version(3, 0),
        instruction(31, 2), 0, dst(INPUT, 0),
        instruction(20 /* M4X4 */, 3), dst(RASTOUT, 0), src(INPUT, 0),
        src(CONST, 0),
        END_TOKEN,
    };
    const Sm3Shader shader = translate(tokens);
    ASSERT_TRUE(shader.ok) << shader.failure;
    EXPECT_NE(shader.msl.find("out.position.x = dot(in.v0, cb.c[0]);"),
              std::string::npos);
    EXPECT_NE(shader.msl.find("out.position.y = dot(in.v0, cb.c[1]);"),
              std::string::npos);
    EXPECT_NE(shader.msl.find("out.position.z = dot(in.v0, cb.c[2]);"),
              std::string::npos);
    EXPECT_NE(shader.msl.find("out.position.w = dot(in.v0, cb.c[3]);"),
              std::string::npos);

    // m3x3 uses three-component rows and writes three components.
    const std::vector<u32> m3x3{
        vs_version(3, 0),
        instruction(31, 2), 0, dst(INPUT, 0),
        instruction(23 /* M3X3 */, 3), dst(TEMP, 0, 0x7), src(INPUT, 0),
        src(CONST, 4),
        instruction(1, 2), dst(RASTOUT, 0), src(TEMP, 0),
        END_TOKEN,
    };
    const Sm3Shader shader3 = translate(m3x3);
    ASSERT_TRUE(shader3.ok) << shader3.failure;
    EXPECT_NE(shader3.msl.find("r0.x = dot(in.v0.xyz, cb.c[4].xyz);"),
              std::string::npos);
    EXPECT_NE(shader3.msl.find("r0.z = dot(in.v0.xyz, cb.c[6].xyz);"),
              std::string::npos);
    EXPECT_EQ(shader3.msl.find("r0.w = dot"), std::string::npos);
}

TEST(Sm3ToMslTest, AppliesSourceModifiersAndTheSaturateResultModifier) {
    const std::vector<u32> tokens{
        ps_version(3, 0),
        // mov_sat r0, -c0
        instruction(1, 2), dst(TEMP, 0, 0xF, 0x1 /* saturate */),
        src(CONST, 0, 0xE4, 1 /* negate */),
        // mov r1, 1 - c1
        instruction(1, 2), dst(TEMP, 1), src(CONST, 1, 0xE4, 6 /* complement */),
        // mov r2, |c2|
        instruction(1, 2), dst(TEMP, 2), src(CONST, 2, 0xE4, 11 /* abs */),
        instruction(1, 2), dst(COLOROUT, 0), src(TEMP, 0),
        END_TOKEN,
    };
    const Sm3Shader shader = translate(tokens);
    ASSERT_TRUE(shader.ok) << shader.failure;
    EXPECT_NE(shader.msl.find("r0 = saturate((-cb.c[0]));"),
              std::string::npos);
    EXPECT_NE(shader.msl.find("r1 = (float4(1.0) - cb.c[1]);"),
              std::string::npos);
    EXPECT_NE(shader.msl.find("r2 = abs(cb.c[2]);"), std::string::npos);
}

TEST(Sm3ToMslTest, SwizzlesNarrowToTheDestinationWriteMask) {
    const std::vector<u32> tokens{
        ps_version(3, 0),
        // mov r0.xy, c0.zwxy  → the .zw of the source reaches .xy
        instruction(1, 2), dst(TEMP, 0, 0x3),
        src(CONST, 0, 2u | (3u << 2) | (0u << 4) | (1u << 6)),
        instruction(1, 2), dst(COLOROUT, 0), src(TEMP, 0),
        END_TOKEN,
    };
    const Sm3Shader shader = translate(tokens);
    ASSERT_TRUE(shader.ok) << shader.failure;
    EXPECT_NE(shader.msl.find("r0.xy = cb.c[0].zw;"), std::string::npos);
}

TEST(Sm3ToMslTest, SincosWritesCosineToXAndSineToY) {
    // Learn: sincos - ps (ps_3_0): dest.x = cos(V), dest.y = sin(V).
    const std::vector<u32> tokens{
        ps_version(3, 0),
        // sincos r0.xy, r1.xxxx
        instruction(37 /* SINCOS */, 2), dst(TEMP, 0, 0x3),
        src(TEMP, 1, replicate(0)),
        instruction(1, 2), dst(COLOROUT, 0), src(TEMP, 0),
        END_TOKEN,
    };
    const Sm3Shader shader = translate(tokens);
    ASSERT_TRUE(shader.ok) << shader.failure;
    EXPECT_NE(shader.msl.find("r0.xy = float2(cos(r1.x), sin(r1.x));"),
              std::string::npos)
        << shader.msl;
}

TEST(Sm3ToMslTest, DsxDsyEmitMetalDerivatives) {
    // Learn: dsx/dsy - ps → Metal dfdx/dfdy.
    const std::vector<u32> tokens{
        ps_version(3, 0),
        instruction(31, 2), 5u, dst(INPUT, 0),
        instruction(91 /* DSX */, 2), dst(TEMP, 0), src(INPUT, 0),
        instruction(92 /* DSY */, 2), dst(TEMP, 1), src(INPUT, 0),
        instruction(2 /* ADD */, 3), dst(COLOROUT, 0), src(TEMP, 0),
        src(TEMP, 1),
        END_TOKEN,
    };
    const Sm3Shader shader = translate(tokens);
    ASSERT_TRUE(shader.ok) << shader.failure;
    EXPECT_NE(shader.msl.find("r0 = dfdx(in.texcoord0);"), std::string::npos)
        << shader.msl;
    EXPECT_NE(shader.msl.find("r1 = dfdy(in.texcoord0);"), std::string::npos)
        << shader.msl;
}

TEST(Sm3ToMslTest, Sm2DeclaresTextureCoordinateRegisters) {
    // Learn: dcl - ps `dcl t0` uses register type TEXTURE (3).
    const std::vector<u32> tokens{
        ps_version(2, 0),
        instruction(31 /* DCL */, 2), 0, dst(TEXTURE, 0),
        instruction(1 /* MOV */, 2), dst(COLOROUT, 0), src(TEXTURE, 0),
        END_TOKEN,
    };
    const Sm3Shader shader = translate(tokens);
    ASSERT_TRUE(shader.ok) << shader.failure;
    EXPECT_NE(shader.msl.find("float4 texcoord0 [[user(texcoord0)]]"),
              std::string::npos)
        << shader.msl;
    EXPECT_NE(shader.msl.find("out.color = in.texcoord0;"), std::string::npos)
        << shader.msl;
}

TEST(Sm3ToMslTest, Sm2ColorRegistersMapByInputIndex) {
    // Learn: dcl - ps — `dcl v0` / `dcl v1` are COLOR0 / COLOR1. ps_2_0 does
    // not carry a D3DDECLUSAGE in the leading DCL token; the register index
    // selects the colour varying. Sims 3 UI shaders add v1.wwww into the
    // sample (specular alpha); mapping both v# to color0 bleaches the menu.
    constexpr u32 kSat = 1; // D3DSPDM_SATURATE
    const std::vector<u32> tokens{
        ps_version(2, 0),
        instruction(31 /* DCL */, 2), 0x80000000u, dst(INPUT, 0),
        instruction(31 /* DCL */, 2), 0x80000000u, dst(INPUT, 1),
        instruction(31 /* DCL */, 2), 0x80000000u, dst(TEXTURE, 0, 0x3),
        instruction(31 /* DCL */, 2), 0x90000000u, dst(SAMPLER, 0),
        instruction(0x42 /* TEX */, 3), dst(TEMP, 0), src(TEXTURE, 0),
        src(SAMPLER, 0),
        instruction(2 /* ADD */, 3), dst(TEMP, 0, 0xF, kSat), src(TEMP, 0),
        src(INPUT, 1, replicate(3)),
        instruction(1 /* MOV */, 2), dst(COLOROUT, 0), src(TEMP, 0),
        END_TOKEN,
    };
    const Sm3Shader shader = translate(tokens);
    ASSERT_TRUE(shader.ok) << shader.failure;
    EXPECT_NE(shader.msl.find("float4 color1 [[user(color1)]]"),
              std::string::npos)
        << shader.msl;
    EXPECT_NE(shader.msl.find("saturate(r0 + in.color1.wwww)"),
              std::string::npos)
        << shader.msl;
    EXPECT_EQ(shader.msl.find("in.color0.wwww"), std::string::npos)
        << shader.msl;
}

TEST(Sm3ToMslTest, SincosSm2ConsumesTaylorConstantSources) {
    // ps_2_0 form still carries D3DSINCOSCONST1/2 sources; ignore values.
    const std::vector<u32> tokens{
        ps_version(2, 0),
        instruction(37 /* SINCOS */, 4), dst(TEMP, 0, 0x1),
        src(TEMP, 1, replicate(0)), src(CONST, 0), src(CONST, 1),
        instruction(1, 2), dst(COLOROUT, 0), src(TEMP, 0),
        END_TOKEN,
    };
    const Sm3Shader shader = translate(tokens);
    ASSERT_TRUE(shader.ok) << shader.failure;
    EXPECT_NE(shader.msl.find("r0.x = cos(r1.x);"), std::string::npos)
        << shader.msl;
}

TEST(Sm3ToMslTest, NormalizationUsesXyzLengthForAllFourComponents) {
    const std::vector<u32> tokens{
        ps_version(3, 0),
        instruction(36 /* NRM */, 2), dst(TEMP, 0), src(CONST, 0),
        instruction(1, 2), dst(COLOROUT, 0), src(TEMP, 0),
        END_TOKEN,
    };
    const Sm3Shader shader = translate(tokens);
    ASSERT_TRUE(shader.ok) << shader.failure;
    EXPECT_NE(shader.msl.find(
                  "dot((cb.c[0]).xyz, (cb.c[0]).xyz)"),
              std::string::npos);
    EXPECT_NE(shader.msl.find("3.402823466e+38f"), std::string::npos);
    EXPECT_NE(shader.msl.find("(cb.c[0]) *"), std::string::npos);
    EXPECT_EQ(shader.msl.find("float4(normalize("), std::string::npos);
}

TEST(Sm3ToMslTest, TexkillReadsDestinationRegisterXyzWithoutASwizzle) {
    const std::vector<u32> tokens{
        ps_version(3, 0),
        instruction(1, 2), dst(TEMP, 0), src(CONST, 0),
        instruction(65 /* TEXKILL */, 1), dst(TEMP, 0),
        instruction(1, 2), dst(COLOROUT, 0), src(TEMP, 0),
        END_TOKEN,
    };
    const Sm3Shader shader = translate(tokens);
    ASSERT_TRUE(shader.ok) << shader.failure;
    EXPECT_NE(shader.msl.find(
                  "if (any(r0.xyz < float3(0.0))) discard_fragment();"),
              std::string::npos);
}

TEST(Sm3ToMslTest, TextureSamplingBindsTheDeclaredSampler) {
    const std::vector<u32> tokens{
        ps_version(3, 0),
        // dcl_2d s1
        instruction(31, 2), (2u << 27), dst(SAMPLER, 1),
        // dcl_texcoord0 v0
        instruction(31, 2), 5u, dst(INPUT, 0),
        // texld r0, v0, s1
        instruction(66 /* TEX */, 3), dst(TEMP, 0), src(INPUT, 0),
        src(SAMPLER, 1),
        instruction(1, 2), dst(COLOROUT, 0), src(TEMP, 0),
        END_TOKEN,
    };
    const Sm3Shader shader = translate(tokens);
    ASSERT_TRUE(shader.ok) << shader.failure;
    EXPECT_TRUE(shader.samplers_used[1]);
    EXPECT_FALSE(shader.samplers_used[0]);
    EXPECT_NE(shader.msl.find("texture2d<float> tex1 [[texture(1)]]"),
              std::string::npos);
    EXPECT_NE(shader.msl.find("sampler samp1 [[sampler(1)]]"),
              std::string::npos);
    EXPECT_NE(shader.msl.find("tex1.sample(samp1, in.texcoord0.xy)"),
              std::string::npos);
}

TEST(Sm3ToMslTest, CubeSamplerDeclarationEmitsTexturecubeAndXyzCoords) {
    // dcl_cube s0 (D3DSTT_CUBE = 3 in bits 27:24); texld uses .xyz.
    const std::vector<u32> tokens{
        ps_version(3, 0),
        instruction(31, 2), (3u << 27), dst(SAMPLER, 0),
        instruction(31, 2), 5u, dst(INPUT, 0),
        instruction(66 /* TEX */, 3), dst(TEMP, 0), src(INPUT, 0),
        src(SAMPLER, 0),
        instruction(1, 2), dst(COLOROUT, 0), src(TEMP, 0),
        END_TOKEN,
    };
    const Sm3Shader shader = translate(tokens);
    ASSERT_TRUE(shader.ok) << shader.failure;
    EXPECT_NE(shader.msl.find("texturecube<float> tex0 [[texture(0)]]"),
              std::string::npos)
        << shader.msl;
    EXPECT_NE(shader.msl.find("tex0.sample(samp0, in.texcoord0.xyz)"),
              std::string::npos)
        << shader.msl;
}

TEST(Sm3ToMslTest, VolumeSamplerDeclarationEmitsTexture3d) {
    const std::vector<u32> tokens{
        ps_version(3, 0),
        instruction(31, 2), (4u << 27), dst(SAMPLER, 0),
        instruction(31, 2), 5u, dst(INPUT, 0),
        instruction(66 /* TEX */, 3), dst(TEMP, 0), src(INPUT, 0),
        src(SAMPLER, 0),
        instruction(1, 2), dst(COLOROUT, 0), src(TEMP, 0),
        END_TOKEN,
    };
    const Sm3Shader shader = translate(tokens);
    ASSERT_TRUE(shader.ok) << shader.failure;
    EXPECT_NE(shader.msl.find("texture3d<float> tex0 [[texture(0)]]"),
              std::string::npos)
        << shader.msl;
}

TEST(Sm3ToMslTest, TexldpDividesCoordinatesByWBeforeSampling) {
    const std::vector<u32> tokens{
        ps_version(3, 0),
        instruction(31, 2), (2u << 27), dst(SAMPLER, 0),
        instruction(31, 2), 5u, dst(INPUT, 0),
        // texldp r0, v0, s0  (D3DSI_TEXLD_PROJECT = control bit 0)
        instruction(66 /* TEX */, 3, 1), dst(TEMP, 0), src(INPUT, 0),
        src(SAMPLER, 0),
        instruction(1, 2), dst(COLOROUT, 0), src(TEMP, 0),
        END_TOKEN,
    };
    const Sm3Shader shader = translate(tokens);
    ASSERT_TRUE(shader.ok) << shader.failure;
    EXPECT_NE(shader.msl.find("/ max(abs("), std::string::npos)
        << shader.msl;
    EXPECT_NE(shader.msl.find("tex0.sample(samp0,"), std::string::npos);
}

TEST(Sm3ToMslTest, TexldlSamplesWithExplicitLevelFromW) {
    // Learn: texldl - ps — LOD from src0.w → Metal sample(..., level(w)).
    const std::vector<u32> tokens{
        ps_version(3, 0),
        instruction(31, 2), (2u << 27), dst(SAMPLER, 0),
        instruction(31, 2), 5u, dst(INPUT, 0),
        // texldl r0, v0, s0
        instruction(95 /* TEXLDL */, 3), dst(TEMP, 0), src(INPUT, 0),
        src(SAMPLER, 0),
        instruction(1, 2), dst(COLOROUT, 0), src(TEMP, 0),
        END_TOKEN,
    };
    const Sm3Shader shader = translate(tokens);
    ASSERT_TRUE(shader.ok) << shader.failure;
    EXPECT_TRUE(shader.samplers_used[0]);
    EXPECT_NE(shader.msl.find(
                  "tex0.sample(samp0, (in.texcoord0).xy, "
                  "level((in.texcoord0).w))"),
              std::string::npos)
        << shader.msl;
}

TEST(Sm3ToMslTest, SamplerLodBiasAddsToImplicitTexldbAndTexldlLod) {
    const std::vector<u32> texldl_tokens{
        ps_version(3, 0),
        instruction(31, 2), (2u << 27), dst(SAMPLER, 0),
        instruction(31, 2), 5u, dst(INPUT, 0),
        instruction(95 /* TEXLDL */, 3), dst(TEMP, 0), src(INPUT, 0),
        src(SAMPLER, 0),
        instruction(1, 2), dst(COLOROUT, 0), src(TEMP, 0),
        END_TOKEN,
    };
    const Sm3Shader texldl = translate(texldl_tokens);
    ASSERT_TRUE(texldl.ok) << texldl.failure;
    std::array<float, SM3_MAX_SAMPLERS> biases{};
    biases[0] = 2.0f;
    const std::string explicit_lod =
        apply_sm3_sampler_lod_bias(texldl.msl, biases);
    EXPECT_NE(explicit_lod.find("level((in.texcoord0).w + 2.0)"),
              std::string::npos) << explicit_lod;

    const std::vector<u32> texldb_tokens{
        ps_version(3, 0),
        instruction(31, 2), (2u << 27), dst(SAMPLER, 0),
        instruction(31, 2), 5u, dst(INPUT, 0),
        instruction(66 /* TEX */, 3, 2 /* TEXLD_BIAS */), dst(TEMP, 0),
        src(INPUT, 0), src(SAMPLER, 0),
        instruction(1, 2), dst(COLOROUT, 0), src(TEMP, 0),
        END_TOKEN,
    };
    const Sm3Shader texldb = translate(texldb_tokens);
    ASSERT_TRUE(texldb.ok) << texldb.failure;
    biases[0] = 1.5f;
    const std::string instruction_bias =
        apply_sm3_sampler_lod_bias(texldb.msl, biases);
    EXPECT_NE(instruction_bias.find("bias((in.texcoord0).w + 1.5)"),
              std::string::npos) << instruction_bias;

    std::vector<u32> implicit_tokens = texldb_tokens;
    implicit_tokens[7] = instruction(66 /* TEX */, 3);
    const Sm3Shader implicit = translate(implicit_tokens);
    ASSERT_TRUE(implicit.ok) << implicit.failure;
    const std::string implicit_bias =
        apply_sm3_sampler_lod_bias(implicit.msl, biases);
    EXPECT_NE(implicit_bias.find(
                  "tex0.sample(samp0, in.texcoord0.xy, bias(1.5))"),
              std::string::npos) << implicit_bias;
}

TEST(Sm3ToMslTest, VertexShaderModel3TexldlBindsVertexTextureSampler) {
    // Learn: vs_3_0 exposes four vertex samplers; texldl takes explicit LOD
    // from the coordinate's w component.
    const std::vector<u32> tokens{
        vs_version(3, 0),
        instruction(31, 2), 0, dst(INPUT, 0),
        instruction(31, 2), (2u << 27), dst(SAMPLER, 0),
        instruction(95 /* TEXLDL */, 3), dst(TEMP, 0), src(INPUT, 0),
        src(SAMPLER, 0),
        instruction(1, 2), dst(RASTOUT, 0), src(TEMP, 0),
        END_TOKEN,
    };
    const Sm3Shader shader = translate(tokens);
    ASSERT_TRUE(shader.ok) << shader.failure;
    EXPECT_TRUE(shader.samplers_used[0]);
    EXPECT_NE(shader.msl.find("texture2d<float> tex0 [[texture(0)]]"),
              std::string::npos) << shader.msl;
    EXPECT_NE(shader.msl.find(
                  "tex0.sample(samp0, (in.v0).xy, level((in.v0).w))"),
              std::string::npos) << shader.msl;
}

TEST(Sm3ToMslTest, VertexShaderSubroutineReceivesTexldlSamplerArguments) {
    constexpr u32 LABEL = 18;
    const std::vector<u32> tokens{
        vs_version(3, 0),
        instruction(31, 2), 0, dst(INPUT, 0),
        instruction(31, 2), (2u << 27), dst(SAMPLER, 3),
        instruction(25 /* CALL */, 1), src(LABEL, 0),
        instruction(1, 2), dst(RASTOUT, 0), src(INPUT, 0),
        instruction(30 /* LABEL */, 1), dst(LABEL, 0),
        instruction(95 /* TEXLDL */, 3), dst(TEMP, 0), src(INPUT, 0),
        src(SAMPLER, 3),
        instruction(28 /* RET */, 0),
        END_TOKEN,
    };
    const Sm3Shader shader = translate(tokens);
    ASSERT_TRUE(shader.ok) << shader.failure;
    EXPECT_NE(shader.msl.find(
                  "texture2d<float> tex3, sampler samp3"),
              std::string::npos) << shader.msl;
    EXPECT_NE(shader.msl.find(
                  "sm3_l0(r0, aL, in, out, cb, tex3, samp3)"),
              std::string::npos) << shader.msl;
    EXPECT_NE(shader.msl.find(
                  "tex3.sample(samp3, (in.v0).xy, level((in.v0).w))"),
              std::string::npos) << shader.msl;
}

TEST(Sm3ToMslTest, TexlddSamplesWithExplicitGradients) {
    // Learn: texldd - ps — sample with src2/src3 as ddx/ddy gradients.
    const std::vector<u32> tokens{
        ps_version(3, 0),
        instruction(31, 2), (2u << 27), dst(SAMPLER, 1),
        instruction(31, 2), 5u, dst(INPUT, 0),
        // texldd r0, v0, s1, r1, r2
        instruction(93 /* TEXLDD */, 5), dst(TEMP, 0), src(INPUT, 0),
        src(SAMPLER, 1), src(TEMP, 1), src(TEMP, 2),
        instruction(1, 2), dst(COLOROUT, 0), src(TEMP, 0),
        END_TOKEN,
    };
    const Sm3Shader shader = translate(tokens);
    ASSERT_TRUE(shader.ok) << shader.failure;
    EXPECT_TRUE(shader.samplers_used[1]);
    EXPECT_NE(shader.msl.find(
                  "tex1.sample(samp1, in.texcoord0.xy, "
                  "gradient2d(r1.xy, r2.xy))"),
              std::string::npos)
        << shader.msl;
}

TEST(Sm3ToMslTest, TexlddOnCubeUsesGradientcubeAndXyz) {
    const std::vector<u32> tokens{
        ps_version(3, 0),
        instruction(31, 2), (3u << 27), dst(SAMPLER, 0),
        instruction(31, 2), 5u, dst(INPUT, 0),
        instruction(93 /* TEXLDD */, 5), dst(TEMP, 0), src(INPUT, 0),
        src(SAMPLER, 0), src(TEMP, 1), src(TEMP, 2),
        instruction(1, 2), dst(COLOROUT, 0), src(TEMP, 0),
        END_TOKEN,
    };
    const Sm3Shader shader = translate(tokens);
    ASSERT_TRUE(shader.ok) << shader.failure;
    EXPECT_NE(shader.msl.find(
                  "tex0.sample(samp0, in.texcoord0.xyz, "
                  "gradientcube(r1.xyz, r2.xyz))"),
              std::string::npos)
        << shader.msl;
}

TEST(Sm3ToMslTest, Sm1TexHonoursRuntimeProjectedTransformFlags) {
    // ps_1_1: tex t0 ; mov r0, t0
    const std::vector<u32> tokens{
        ps_version(1, 1),
        instruction(66 /* TEX */, 1), dst(TEXTURE, 0),
        instruction(1 /* MOV */, 2), dst(TEMP, 0), src(TEXTURE, 0),
        instruction(1, 2), dst(COLOROUT, 0), src(TEMP, 0),
        END_TOKEN,
    };
    const u32 ttf = 0x103u; // D3DTTFF_COUNT3 | D3DTTFF_PROJECTED
    const Sm3Shader shader =
        translate_sm3_to_msl(tokens.data(), tokens.size(), &ttf, 1);
    ASSERT_TRUE(shader.ok) << shader.failure;
    EXPECT_NE(shader.msl.find(".z"), std::string::npos) << shader.msl;
    EXPECT_NE(shader.msl.find("/ max(abs("), std::string::npos) << shader.msl;
}

TEST(Sm3ToMslTest, Sm1TexProjectedWithZeroCountDividesByW) {
    // Programmable VS path: COUNT=0 | PROJECTED → divide by .w (MSDN).
    const std::vector<u32> tokens{
        ps_version(1, 1),
        instruction(66 /* TEX */, 1), dst(TEXTURE, 0),
        instruction(1 /* MOV */, 2), dst(TEMP, 0), src(TEXTURE, 0),
        instruction(1, 2), dst(COLOROUT, 0), src(TEMP, 0),
        END_TOKEN,
    };
    const u32 ttf = 0x100u; // D3DTTFF_PROJECTED only
    const Sm3Shader shader =
        translate_sm3_to_msl(tokens.data(), tokens.size(), &ttf, 1);
    ASSERT_TRUE(shader.ok) << shader.failure;
    EXPECT_NE(shader.msl.find(".w"), std::string::npos) << shader.msl;
    EXPECT_NE(shader.msl.find("/ max(abs("), std::string::npos) << shader.msl;
}

TEST(Sm3ToMslTest, VaryingsUseMatchingNamesAcrossTheTwoStages) {
    const std::vector<u32> vertex_tokens{
        vs_version(3, 0),
        instruction(31, 2), 0, dst(INPUT, 0),
        instruction(31, 2), 5u, dst(INPUT, 1),
        // dcl_position o0 / dcl_texcoord0 o1
        instruction(31, 2), 0, dst(OUTPUT, 0),
        instruction(31, 2), 5u, dst(OUTPUT, 1),
        instruction(1, 2), dst(OUTPUT, 0), src(INPUT, 0),
        instruction(1, 2), dst(OUTPUT, 1), src(INPUT, 1),
        END_TOKEN,
    };
    const Sm3Shader vertex = translate(vertex_tokens);
    ASSERT_TRUE(vertex.ok) << vertex.failure;
    // A declared position output writes the built-in position, not a varying.
    EXPECT_NE(vertex.msl.find("out.position = in.v0;"), std::string::npos);
    EXPECT_NE(vertex.msl.find("float4 texcoord0 [[user(texcoord0)]]"),
              std::string::npos);
    EXPECT_NE(vertex.msl.find("out.texcoord0 = float4(0.0, 0.0, 0.0, 1.0);"),
              std::string::npos)
        << vertex.msl;

    const std::vector<u32> pixel_tokens{
        ps_version(3, 0),
        instruction(31, 2), 5u, dst(INPUT, 1),
        instruction(1, 2), dst(COLOROUT, 0), src(INPUT, 1),
        END_TOKEN,
    };
    const Sm3Shader pixel = translate(pixel_tokens);
    ASSERT_TRUE(pixel.ok) << pixel.failure;
    // The pixel shader reads texcoord usage index 0 from its declaration.
    EXPECT_NE(pixel.msl.find("float4 texcoord0 [[user(texcoord0)]]"),
              std::string::npos);
    EXPECT_NE(pixel.msl.find("out.color = in.texcoord0;"), std::string::npos);
}

TEST(Sm3ToMslTest, TemporariesStartAtZero) {
    const std::vector<u32> tokens{
        ps_version(3, 0),
        instruction(2 /* ADD */, 3), dst(TEMP, 5), src(CONST, 0), src(CONST, 1),
        instruction(1, 2), dst(COLOROUT, 0), src(TEMP, 5),
        END_TOKEN,
    };
    const Sm3Shader shader = translate(tokens);
    ASSERT_TRUE(shader.ok) << shader.failure;
    EXPECT_NE(shader.msl.find("float4 r5 = float4(0.0);"), std::string::npos);
}

// =============================================================================
// Honest refusal
// =============================================================================

TEST(Sm3ToMslTest, CallRetLabelEmitsMetalSubroutine) {
    // Learn: call l# / label l# / ret — forward subroutine call.
    constexpr u32 LABEL = 18;
    const std::vector<u32> tokens{
        vs_version(3, 0),
        instruction(31 /* DCL */, 2), 0, dst(INPUT, 0),
        instruction(25 /* CALL */, 1), src(LABEL, 0),
        instruction(1 /* MOV */, 2), dst(RASTOUT, 0), src(TEMP, 0),
        instruction(30 /* LABEL */, 1), dst(LABEL, 0),
        instruction(1 /* MOV */, 2), dst(TEMP, 0), src(INPUT, 0),
        instruction(28 /* RET */, 0),
        END_TOKEN,
    };
    const Sm3Shader shader = translate(tokens);
    ASSERT_TRUE(shader.ok) << shader.failure;
    EXPECT_NE(shader.msl.find("void sm3_l0("), std::string::npos) << shader.msl;
    EXPECT_NE(shader.msl.find("sm3_l0("), std::string::npos) << shader.msl;
    EXPECT_NE(shader.msl.find("r0 = in.v0;"), std::string::npos) << shader.msl;
}

TEST(Sm3ToMslTest, CallnzBoolGatesTheSubroutine) {
    constexpr u32 LABEL = 18;
    constexpr u32 CONSTBOOL = 14;
    const std::vector<u32> tokens{
        vs_version(3, 0),
        instruction(31 /* DCL */, 2), 0, dst(INPUT, 0),
        instruction(47 /* DEFB */, 2), dst(CONSTBOOL, 1), 1u,
        instruction(26 /* CALLNZ */, 2), src(LABEL, 2), src(CONSTBOOL, 1),
        instruction(1 /* MOV */, 2), dst(RASTOUT, 0), src(INPUT, 0),
        instruction(30 /* LABEL */, 1), dst(LABEL, 2),
        instruction(1 /* MOV */, 2), dst(TEMP, 0), src(CONST, 0),
        instruction(28 /* RET */, 0),
        END_TOKEN,
    };
    const Sm3Shader shader = translate(tokens);
    ASSERT_TRUE(shader.ok) << shader.failure;
    EXPECT_NE(shader.msl.find("if (defb1) {"), std::string::npos) << shader.msl;
    EXPECT_NE(shader.msl.find("sm3_l2("), std::string::npos) << shader.msl;
}

TEST(Sm3ToMslTest, TranslatesVFaceAndODepth) {
    // Learn: vFace sign / oDepth scalar output (ps_3_0).
    constexpr u32 MISCTYPE = 17;
    constexpr u32 DEPTHOUT = 9;
    const std::vector<u32> tokens{
        ps_version(3, 0),
        instruction(31 /* DCL */, 2), 0, dst(MISCTYPE, 1), // dcl vFace
        instruction(31 /* DCL */, 2), 0, dst(MISCTYPE, 0), // dcl vPos.xy
        instruction(1 /* MOV */, 2), dst(DEPTHOUT, 0, 0x1),
        src(MISCTYPE, 0, replicate(0)),
        instruction(1 /* MOV */, 2), dst(COLOROUT, 0),
        src(MISCTYPE, 1),
        END_TOKEN,
    };
    const Sm3Shader shader = translate(tokens);
    ASSERT_TRUE(shader.ok) << shader.failure;
    EXPECT_NE(shader.msl.find("bool front_facing [[front_facing]]"),
              std::string::npos)
        << shader.msl;
    EXPECT_NE(shader.msl.find("const float vFace = front_facing ? 1.0 : -1.0"),
              std::string::npos)
        << shader.msl;
    EXPECT_NE(shader.msl.find("float depth [[depth(any)]]"), std::string::npos)
        << shader.msl;
    EXPECT_NE(shader.msl.find("out.depth ="), std::string::npos) << shader.msl;
}

TEST(Sm3ToMslTest, TranslatesDstDistanceVector) {
    // Learn: dst dest = (1, src0.y*src1.y, src0.z, src1.w)
    const std::vector<u32> tokens{
        vs_version(3, 0),
        instruction(31 /* DCL */, 2), 0, dst(INPUT, 0),
        instruction(17 /* DST */, 3), dst(TEMP, 0), src(CONST, 0), src(CONST, 1),
        instruction(1 /* MOV */, 2), dst(RASTOUT, 0), src(INPUT, 0),
        END_TOKEN,
    };
    const Sm3Shader shader = translate(tokens);
    ASSERT_TRUE(shader.ok) << shader.failure;
    EXPECT_NE(shader.msl.find("float4(1.0, (cb.c[0].y) * (cb.c[1].y), cb.c[0].z, "
                              "cb.c[1].w)"),
              std::string::npos)
        << shader.msl;
}

TEST(Sm3ToMslTest, RefusesPixelShaderRelativeAddressing) {
    const std::vector<u32> relative{
        ps_version(3, 0),
        instruction(1, 2), dst(COLOROUT, 0),
        src(CONST, 0) | (1u << 13),
        END_TOKEN,
    };
    const Sm3Shader b = translate(relative);
    EXPECT_FALSE(b.ok);
    EXPECT_NE(b.failure.find("relative addressing"), std::string::npos);
}

TEST(Sm3ToMslTest, VertexShaderAddressRegisterIndexesConstants) {
    // Minimal vs_1_1: dcl_position v0; mov a0.x, v0.x ; mov oPos, c[a0.x + 0]
    constexpr u32 ADDR = 3;
    constexpr u32 RASTOUT = 4;
    const std::vector<u32> tokens{
        0xFFFE0101u, // vs_1_1
        instruction(31 /* DCL */, 2), 0 /* position */, dst(INPUT, 0),
        instruction(1, 2), dst(ADDR, 0, 0x1), src(INPUT, 0),
        instruction(1, 2), dst(RASTOUT, 0), src(CONST, 0) | (1u << 13),
        END_TOKEN,
    };
    const Sm3Shader shader = translate(tokens);
    ASSERT_TRUE(shader.ok) << shader.failure;
    EXPECT_NE(shader.msl.find("int4 a0 = int4(0);"), std::string::npos)
        << shader.msl;
    // vs_1_1 `mov a0.x` converts to int by rounding to nearest, not by
    // truncation: a blend index arriving as 4.9999995 must select c[5].
    EXPECT_NE(shader.msl.find("a0.x = int(round("), std::string::npos)
        << shader.msl;
    EXPECT_NE(shader.msl.find("sm3_c(cb, a0.x + 0)"), std::string::npos)
        << shader.msl;
    EXPECT_NE(shader.msl.find("const int idx = max(min(i, n - 1), 0);"),
              std::string::npos)
        << shader.msl;
    EXPECT_NE(shader.msl.find("const float4 v = cb.c[idx];"),
              std::string::npos)
        << shader.msl;
    EXPECT_NE(shader.msl.find("return (i < 0 || i >= n) ? float4(0.0) : v;"),
              std::string::npos)
        << shader.msl;
    EXPECT_EQ(shader.float_constant_count, 96u);
}

TEST(Sm3ToMslTest, ApplyFogMixesFogColorAndAddsVarying) {
    const std::vector<u32> tokens{
        ps_version(1, 1),
        instruction(1, 2), dst(TEMP, 0), src(CONST, 0),
        END_TOKEN,
    };
    const Sm3Shader shader = translate(tokens);
    ASSERT_TRUE(shader.ok) << shader.failure;
    const std::string unchanged = apply_sm3_fog(shader.msl, false);
    EXPECT_EQ(unchanged, shader.msl);
    const std::string fogged = apply_sm3_fog(shader.msl, true);
    EXPECT_NE(fogged.find("float4 fog [[user(fog)]];"), std::string::npos)
        << fogged;
    EXPECT_NE(fogged.find("float4 fog_color;"), std::string::npos) << fogged;
    EXPECT_NE(fogged.find("mix(cb.fog_color.rgb, out.color.rgb, "
                          "clamp(in.fog.x, 0.0, 1.0))"),
              std::string::npos)
        << fogged;
}

TEST(Sm3ToMslTest, ApplyTableFogUsesPerPixelZOrReciprocalWDepth) {
    const std::vector<u32> tokens{
        ps_version(1, 1),
        instruction(1, 2), dst(TEMP, 0), src(CONST, 0),
        END_TOKEN,
    };
    const Sm3Shader shader = translate(tokens);
    ASSERT_TRUE(shader.ok) << shader.failure;

    const std::string z_fog = apply_sm3_table_fog(shader.msl, 3, false);
    EXPECT_NE(z_fog.find("float4 table_fog_params;"), std::string::npos)
        << z_fog;
    EXPECT_NE(z_fog.find("float4 table_fog_color;"), std::string::npos)
        << z_fog;
    EXPECT_NE(z_fog.find("float table_fog_distance = in.frag_position.z;"),
              std::string::npos)
        << z_fog;
    EXPECT_NE(z_fog.find("cb.table_fog_params.y - table_fog_distance"),
              std::string::npos)
        << z_fog;
    EXPECT_NE(z_fog.find("mix(cb.table_fog_color.rgb, out.color.rgb"),
              std::string::npos)
        << z_fog;

    const std::string w_fog = apply_sm3_table_fog(shader.msl, 2, true);
    EXPECT_NE(w_fog.find(
                  "float table_fog_distance = 1.0 / max(abs(in.frag_position.w), 1e-6);"),
              std::string::npos)
        << w_fog;
    EXPECT_NE(w_fog.find("cb.table_fog_params.z * cb.table_fog_params.z * "
                         "table_fog_distance * table_fog_distance"),
              std::string::npos)
        << w_fog;

    EXPECT_EQ(apply_sm3_table_fog(shader.msl, 0, false), shader.msl);
}

TEST(Sm3ToMslTest, ApplyAlphaTestInsertsDiscardAndUniform) {
    const std::vector<u32> tokens{
        ps_version(1, 1),
        instruction(66, 1), dst(TEXTURE, 0),
        instruction(1, 2), dst(TEMP, 0), src(TEXTURE, 0),
        END_TOKEN,
    };
    const Sm3Shader shader = translate(tokens);
    ASSERT_TRUE(shader.ok) << shader.failure;
    const std::string always = apply_sm3_alpha_test(shader.msl, 8);
    EXPECT_EQ(always, shader.msl);
    const std::string greater = apply_sm3_alpha_test(shader.msl, 5);
    EXPECT_NE(greater.find("float4 alpha_test;"), std::string::npos) << greater;
    EXPECT_NE(greater.find("if (out.color.a <= cb.alpha_test.x) discard_fragment();"),
              std::string::npos)
        << greater;
}

TEST(Sm3ToMslTest, PositionTInputMapsScreenSpaceToClipSpace) {
    // vs_1_1: dcl_positiont v0; mov oPos, v0
    const std::vector<u32> tokens{
        vs_version(1, 1),
        instruction(31, 2), 9u /* POSITIONT */, dst(INPUT, 0),
        instruction(1, 2), dst(RASTOUT, 0), src(INPUT, 0),
        END_TOKEN,
    };
    const Sm3Shader shader = translate(tokens);
    ASSERT_TRUE(shader.ok) << shader.failure;
    EXPECT_TRUE(shader.needs_viewport);
    EXPECT_NE(shader.msl.find("float4 viewport;"), std::string::npos)
        << shader.msl;
    EXPECT_NE(shader.msl.find("screen.x / max(cb.viewport.x, 1.0)"),
              std::string::npos)
        << shader.msl;
}

TEST(Sm3ToMslTest, MovaMaskedXyzUsesInt3NotInt4) {
    // Learn: vs_2_x+ a0 is a vector; `mova a0.xyz, v0` is legal. The source
    // is already a float3, so MSL must construct int3 — int4(float3) fails
    // Metal compilation (and was retried on every draw).
    constexpr u32 ADDR = 3;
    constexpr u32 RASTOUT = 4;
    const std::vector<u32> tokens{
        vs_version(3, 0),
        instruction(31 /* DCL */, 2), 0 /* position */, dst(INPUT, 0),
        instruction(46 /* MOVA */, 2), dst(ADDR, 0, 0x7), src(INPUT, 0),
        instruction(1, 2), dst(RASTOUT, 0), src(INPUT, 0),
        END_TOKEN,
    };
    const Sm3Shader shader = translate(tokens);
    ASSERT_TRUE(shader.ok) << shader.failure;
    EXPECT_NE(shader.msl.find("a0.xyz = int3(round("), std::string::npos)
        << shader.msl;
    EXPECT_EQ(shader.msl.find("int4(round("), std::string::npos) << shader.msl;
}

TEST(Sm3ToMslTest, PredicatedInstructionWrapsTheBody) {
    const std::vector<u32> tokens{
        ps_version(3, 0),
        // setp_gt p0, c0, c1
        instruction(94 /* SETP */, 3, 1 /* GT */), dst(19 /* PREDICATE */, 0),
        src(CONST, 0), src(CONST, 1),
        // (p0) mov oC0, c2
        instruction(1, 2) | (1u << 28), dst(COLOROUT, 0), src(CONST, 2),
        src(19 /* PREDICATE */, 0),
        END_TOKEN,
    };
    const Sm3Shader shader = translate(tokens);
    ASSERT_TRUE(shader.ok) << shader.failure;
    EXPECT_NE(shader.msl.find("bool4 p0 = bool4(false);"), std::string::npos);
    EXPECT_NE(shader.msl.find("p0 = (cb.c[0] > cb.c[1]);"), std::string::npos);
    EXPECT_NE(shader.msl.find("if (any(p0)) {"), std::string::npos);
    EXPECT_NE(shader.msl.find("out.color = cb.c[2];"), std::string::npos);
}

TEST(Sm3ToMslTest, IfElseEndifOnPredicateEmitsMslBraces) {
    // Learn: if bool / else / endif — SM3 dynamic form uses a predicate.
    const std::vector<u32> tokens{
        vs_version(3, 0),
        instruction(31 /* DCL */, 2), 0, dst(INPUT, 0),
        instruction(94 /* SETP */, 3, 1 /* GT */), dst(19 /* PREDICATE */, 0),
        src(CONST, 0, replicate(0)), src(CONST, 1, replicate(0)),
        // if p0.x
        instruction(40 /* IF */, 1), src(19 /* PREDICATE */, 0, replicate(0)),
        instruction(1 /* MOV */, 2), dst(TEMP, 0), src(INPUT, 0),
        instruction(42 /* ELSE */, 0),
        instruction(1 /* MOV */, 2), dst(TEMP, 0), src(CONST, 2),
        instruction(43 /* ENDIF */, 0),
        instruction(1 /* MOV */, 2), dst(RASTOUT, 0), src(TEMP, 0),
        END_TOKEN,
    };
    const Sm3Shader shader = translate(tokens);
    ASSERT_TRUE(shader.ok) << shader.failure;
    EXPECT_NE(shader.msl.find("if (p0.x) {"), std::string::npos) << shader.msl;
    EXPECT_NE(shader.msl.find("} else {"), std::string::npos) << shader.msl;
    EXPECT_NE(shader.msl.find("r0 = in.v0;"), std::string::npos) << shader.msl;
    EXPECT_NE(shader.msl.find("r0 = cb.c[2];"), std::string::npos) << shader.msl;
}

TEST(Sm3ToMslTest, TranslatesLitLightingCoefficients) {
    // dcl_position v0; lit r0, c0; mov oPos, v0
    const std::vector<u32> lit{
        vs_version(3, 0),
        instruction(31 /* DCL */, 2), 0 /* position */, dst(INPUT, 0),
        instruction(16 /* LIT */, 2), dst(TEMP, 0), src(CONST, 0),
        instruction(1 /* MOV */, 2), dst(RASTOUT, 0), src(INPUT, 0),
        END_TOKEN,
    };
    const Sm3Shader shader = translate(lit);
    ASSERT_TRUE(shader.ok) << shader.failure;
    EXPECT_NE(shader.msl.find("float4(1.0, max("), std::string::npos);
    EXPECT_NE(shader.msl.find("clamp("), std::string::npos);
    EXPECT_NE(shader.msl.find("pow(max("), std::string::npos);
}

TEST(Sm3ToMslTest, RefusesInstructionsWithNoTranslation) {
    const std::vector<u32> unknown{
        ps_version(3, 0),
        instruction(0x1234, 1),
        END_TOKEN,
    };
    EXPECT_FALSE(translate(unknown).ok);
    EXPECT_NE(translate(unknown).failure.find("opcode"), std::string::npos);
}

TEST(Sm3ToMslTest, RefusesAVertexShaderThatNeverWritesAPosition) {
    const std::vector<u32> tokens{
        vs_version(3, 0),
        instruction(31, 2), 0, dst(INPUT, 0),
        instruction(1, 2), dst(TEMP, 0), src(INPUT, 0),
        END_TOKEN,
    };
    const Sm3Shader shader = translate(tokens);
    EXPECT_FALSE(shader.ok);
    EXPECT_NE(shader.failure.find("position"), std::string::npos);
}

TEST(Sm3ToMslTest, RefusesAPixelShaderThatNeverWritesAColour) {
    const std::vector<u32> tokens{
        ps_version(3, 0),
        instruction(1, 2), dst(TEMP, 0), src(CONST, 0),
        END_TOKEN,
    };
    const Sm3Shader shader = translate(tokens);
    EXPECT_FALSE(shader.ok);
    EXPECT_NE(shader.failure.find("colour"), std::string::npos);
}

TEST(Sm3ToMslTest, ShaderModelOneTexSamplesIntoTextureRegister) {
    // ps_1_1: tex t0 / mov r0, t0
    const std::vector<u32> tokens{
        ps_version(1, 1),
        instruction(66 /* TEX */, 1), dst(TEXTURE, 0),
        instruction(1 /* MOV */, 2), dst(TEMP, 0), src(TEXTURE, 0),
        END_TOKEN,
    };
    const Sm3Shader shader = translate(tokens);
    ASSERT_TRUE(shader.ok) << shader.failure;
    EXPECT_TRUE(shader.samplers_used[0]);
    EXPECT_NE(shader.msl.find("float4 t0 = in.texcoord0;"), std::string::npos);
    EXPECT_NE(shader.msl.find("t0 = tex0.sample(samp0, (t0).xy);"),
              std::string::npos);
    EXPECT_NE(shader.msl.find("r0 = t0;"), std::string::npos);
    EXPECT_NE(shader.msl.find("out.color = r0;"), std::string::npos);
}

TEST(Sm3ToMslTest, ShaderModelOneTexcoordCopiesTheVarying) {
    const std::vector<u32> tokens{
        ps_version(1, 1),
        instruction(64 /* TEXCOORD */, 1), dst(TEXTURE, 0),
        instruction(1 /* MOV */, 2), dst(TEMP, 0), src(TEXTURE, 0),
        END_TOKEN,
    };
    const Sm3Shader shader = translate(tokens);
    ASSERT_TRUE(shader.ok) << shader.failure;
    EXPECT_NE(shader.msl.find("t0 = in.texcoord0;"), std::string::npos);
}

TEST(Sm3ToMslTest, ShaderModelOneDependentCoordinatesUsePartialWrites) {
    const std::vector<u32> tokens{
        ps_version(1, 1),
        instruction(71 /* TEXM3X2PAD */, 2), dst(TEXTURE, 1),
        src(TEXTURE, 0),
        instruction(73 /* TEXM3X3PAD */, 2), dst(TEXTURE, 2),
        src(TEXTURE, 0),
        instruction(1 /* MOV */, 2), dst(TEMP, 0), src(TEXTURE, 2),
        END_TOKEN,
    };
    const Sm3Shader shader = translate(tokens);
    ASSERT_TRUE(shader.ok) << shader.failure;
    EXPECT_NE(shader.msl.find("t1.xy = in.texcoord0.xy;"), std::string::npos)
        << shader.msl;
    EXPECT_NE(shader.msl.find("t2.xyz = in.texcoord0.xyz;"), std::string::npos)
        << shader.msl;
    EXPECT_EQ(shader.msl.find("t1 = in.texcoord0.xy;"), std::string::npos);
    EXPECT_EQ(shader.msl.find("t2 = in.texcoord0.xyz;"), std::string::npos);
}

TEST(Sm3ToMslTest, ShaderModelOneTexldIntoTempWithCoordinateSource) {
    // Some compilers emit `texld r0, t0` even under a ps_1_1 version token.
    const std::vector<u32> tokens{
        ps_version(1, 1),
        instruction(66 /* TEX */, 2), dst(TEMP, 0), src(TEXTURE, 0),
        END_TOKEN,
    };
    const Sm3Shader shader = translate(tokens);
    ASSERT_TRUE(shader.ok) << shader.failure;
    EXPECT_TRUE(shader.samplers_used[0]);
    EXPECT_NE(shader.msl.find("r0 = tex0.sample(samp0, (in.texcoord0).xy);"),
              std::string::npos);
    EXPECT_NE(shader.msl.find("out.color = r0;"), std::string::npos);
}

TEST(Sm3ToMslTest, Const3BankFoldsIntoLowConstantIndices) {
    constexpr u32 CONST3 = 12;
    const std::vector<u32> tokens{
        ps_version(3, 0),
        instruction(1, 2), dst(COLOROUT, 0), src(CONST3, 2),
        END_TOKEN,
    };
    const Sm3Shader shader = translate(tokens);
    ASSERT_TRUE(shader.ok) << shader.failure;
    EXPECT_NE(shader.msl.find("cb.c[2]"), std::string::npos);
    EXPECT_EQ(shader.float_constant_count, 3u);
}

TEST(Sm3ToMslTest, RefusesShaderModelsOutsideOneToThree) {
    const Sm3Shader shader = translate({vs_version(4, 0), END_TOKEN});
    EXPECT_FALSE(shader.ok);
    EXPECT_NE(shader.failure.find("shader model 4"), std::string::npos);
}
