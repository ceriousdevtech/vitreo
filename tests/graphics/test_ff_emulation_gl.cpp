// SPDX-License-Identifier: LGPL-2.1-only
/// @file test_ff_emulation_gl.cpp
/// @brief The OpenGL fields of the shared fixed-function generator, checked
///        without a GPU: hashing, preconditions, uniform packing and the
///        matrix-application convention. The rendering checks (GL spec
///        formulas against the generated shaders run on Metal) are in
///        test_ff_emulation_gl_metal.cpp.

#include <gtest/gtest.h>

#include "graphics/shaders/ff_emulation.hpp"
#include "graphics/shaders/ff_uniforms.hpp"

#include <array>
#include <cstring>
#include <string>
#include <vector>

using namespace vitreo;
using namespace vitreo::graphics;

namespace {

bool contains(const std::string& text, const std::string& needle) { return text.find(needle) != std::string::npos; }

FFPipelineState gl_base() {
    FFPipelineState s{};
    s.fvf_flags = fvf::XYZ | fvf::NORMAL | fvf::DIFFUSE | fvf::TEX1 | (2u << 16);   // float4 set 0
    s.lighting_enabled = false;
    s.local_viewer = false;
    s.position_is_homogeneous = true;
    return s;
}

/// The MSL float4x4 the packed bytes at @p offset hold, as m[row][column]
/// (MSL is column-major: element (row, column) is at column * 4 + row).
std::array<std::array<float, 4>, 4> msl_matrix(const std::vector<u8>& bytes, usize offset) {
    float raw[16];
    std::memcpy(raw, bytes.data() + offset, sizeof(raw));
    std::array<std::array<float, 4>, 4> m{};
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r) m[r][c] = raw[c * 4 + r];
    return m;
}

}  // namespace

// One matrix convention (ff_emulation.hpp): every site, Direct3D and GL,
// applies a matrix in the row-vector form `p * cb.X`, and the packer stores
// transpose(M) so the MSL value equals the FFMatrix (W * V * P for the
// position, the inverse of W * V for normals). GL's to_ff
// (gl_backend_draw.cpp) passes GL arrays verbatim. This test fails when the
// shader form or the packing changes alone; GlDrawTest.
// ModelviewTranslateLandsWhereTheSpecSays and FFEmulationD3DMetal.* are the
// rendered sides of the same coupling.
TEST(FFEmulationGl, MatrixApplicationConventionIsPinned) {
    FFPipelineState d3d{};
    d3d.fvf_flags = fvf::XYZ;
    d3d.lighting_enabled = false;
    EXPECT_TRUE(contains(generate_ff_shaders(d3d).vertex_msl,
                         "out.position = float4(in.position, 1.0) * cb.world_view_proj;"))
        << "the Direct3D position site changed: update the goldens, this test and GL's to_ff together";
    FFPipelineState gl = d3d;
    gl.position_is_homogeneous = true;
    const std::string gl_vs = generate_ff_shaders(gl).vertex_msl;
    EXPECT_TRUE(contains(gl_vs, "float4 position [[attribute(0)]];"));
    EXPECT_TRUE(contains(gl_vs, "out.position = in.position * cb.world_view_proj;"));

    // Every Direct3D site that applies a matrix, in one state each, and none
    // uses the column-vector form.
    std::vector<FFPipelineState> states;
    auto add = [&](auto&& f) {
        FFPipelineState s{};
        s.fvf_flags = fvf::XYZ | fvf::NORMAL | fvf::DIFFUSE | fvf::TEX1;
        s.active_texture_stages = 1;
        s.texture_stages[0].color_op = TexStageOp::Modulate;
        s.active_light_count = 1;
        f(s);
        states.push_back(s);
    };
    add([](FFPipelineState& s) {
        s.point_size_output = s.point_scale_enabled = true;
        s.clip_planes_enabled = true;
        s.fog_mode = FogMode::Linear;
        s.texture_stages[0].texture_transform_flags = 3u | 0x100u;
    });
    add([](FFPipelineState& s) { s.texture_stages[0].texcoord_index = 0x30000u; });
    add([](FFPipelineState& s) { s.vertex_tweening = true; s.texture_stages[0].texcoord_index = 0x10000u; });
    add([](FFPipelineState& s) {
        s.fvf_flags = (s.fvf_flags & ~fvf::POSITION_MASK) | fvf::XYZB3;
        s.vertex_blend_weight_count = 2;
        s.texture_stages[0].texcoord_index = 0x20000u;
        s.fog_mode = FogMode::Exp;
    });
    add([](FFPipelineState& s) {
        s.fvf_flags = (s.fvf_flags & ~fvf::POSITION_MASK) | fvf::XYZB4 | fvf::LASTBETA_UBYTE4;
        s.vertex_blend_weight_count = 2;
        s.indexed_vertex_blend = true;
        s.texture_stages[0].texcoord_index = 0x10000u;
    });
    for (const FFPipelineState& s : states) {
        const FFGeneratedShaders g = generate_ff_shaders(s);
        ASSERT_TRUE(g.valid());
        for (const std::string& msl : {g.vertex_msl, g.fragment_msl}) {
            for (const char* column :
                 {"cb.world_view_proj * ", "cb.view * cb.world", "cb.view * float4", "cb.view * tci",
                  "cb.world * float4", "cb.world * in.", "_inv_transpose * ",
                  "_inv_transposes[uint(in.blend_indices.x)] * ", "cb.texture_matrix[0] * ", "cb.projection * "}) {
                EXPECT_FALSE(contains(msl, column)) << column << "\n" << msl;
            }
        }
    }
    {
        const std::string vs = generate_ff_shaders(states[0]).vertex_msl;
        EXPECT_TRUE(contains(vs, "(float4(in.position, 1.0) * cb.world * cb.view).xyz;")) << vs;
        EXPECT_TRUE(contains(vs, "(float4(in.normal, 0.0) * cb.world_inv_transpose).xyz;")) << vs;
        EXPECT_TRUE(contains(generate_ff_shaders(states[0]).fragment_msl, "tc = tc * cb.texture_matrix[0];"));
    }

    // A Direct3D translation (row 3) and a non-symmetric scale-shear.
    FFUniformInputs in;
    in.world_matrices[0] = {2, 0.5f, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 3, -4, 5, 1};
    in.view = {0, 0, 1, 0, 0, 2, 0, 0, -1, 0, 0, 0, 1, 2, 3, 1};
    std::vector<u8> bytes;
    pack_ff_vertex_uniforms(d3d, in, bytes);
    // FFUniforms: world, view, projection, world_view_proj, world_inv_transpose.
    const auto wvp = msl_matrix(bytes, 3 * 64);
    const std::array<float, 4> p{1, 2, 3, 1};
    std::array<float, 4> row{};
    for (int j = 0; j < 4; ++j)
        for (int k = 0; k < 4; ++k) row[j] += p[k] * wvp[k][j];   // p * M (MSL `p * cb.X`)
    // Direct3D p * W = (5, -1.5, 8, 1); then * V = (-8, -3, 5, 1) + (1, 2, 3, 0).
    const std::array<float, 4> d3d_wv{-7, -1, 8, 1};
    for (int j = 0; j < 4; ++j) EXPECT_FLOAT_EQ(row[j], d3d_wv[j]) << "row-vector form, component " << j;

    // The normal matrix packs so that `n * cb.world_inv_transpose` is the
    // Direct3D camera-space n * transpose((W V)^-1).
    const auto nm = msl_matrix(bytes, 4 * 64);
    FFMatrix inverse{};
    ASSERT_TRUE(ff_matrix_invert(ff_matrix_multiply(in.world_matrices[0], in.view), inverse));
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) EXPECT_NEAR(nm[r][c], inverse[c * 4 + r], 1e-6f) << r << "," << c;
}

TEST(FFEmulationGl, NewFieldsDefaultToDirect3DAndKeyTheHash) {
    const FFPipelineState d3d{};
    EXPECT_FALSE(d3d.position_is_homogeneous);
    EXPECT_FALSE(d3d.gl_spot_model);
    EXPECT_FALSE(d3d.two_sided_lighting);
    EXPECT_FALSE(d3d.line_stipple.enabled);
    EXPECT_FALSE(d3d.polygon_stipple);
    EXPECT_EQ(d3d.logic_op, FFLogicOp::Copy);
    EXPECT_FALSE(d3d.alpha_ref_float);
    for (const auto& unit : d3d.texgen_plane_mode)
        for (FFTexGenMode m : unit) EXPECT_EQ(m, FFTexGenMode::None);

    // Each MSL-changing field keys the pipeline hash.
    const FFPipelineState base = gl_base();
    std::vector<FFPipelineState> variants;
    auto add = [&](auto&& f) {
        FFPipelineState s = base;
        f(s);
        variants.push_back(s);
    };
    add([](FFPipelineState& s) { s.position_is_homogeneous = false; });
    add([](FFPipelineState& s) { s.gl_spot_model = true; });
    add([](FFPipelineState& s) { s.gl_spot_model = true; s.two_sided_lighting = true; });
    add([](FFPipelineState& s) {
        s.gl_spot_model = true;
        s.two_sided_lighting = true;
        s.back_diffuse_material_source = 0;
    });
    add([](FFPipelineState& s) { s.texgen_plane_mode[0][0] = FFTexGenMode::ObjectLinear; });
    add([](FFPipelineState& s) { s.texgen_plane_mode[0][0] = FFTexGenMode::EyeLinear; });
    add([](FFPipelineState& s) { s.texgen_plane_mode[0][1] = FFTexGenMode::SphereMap; });
    add([](FFPipelineState& s) { s.line_stipple.enabled = true; });
    add([](FFPipelineState& s) { s.polygon_stipple = true; });
    add([](FFPipelineState& s) { s.logic_op = FFLogicOp::Xor; });
    add([](FFPipelineState& s) { s.logic_op = FFLogicOp::Nand; });
    for (usize i = 0; i < variants.size(); ++i) {
        EXPECT_NE(variants[i].hash(), base.hash()) << "variant " << i;
        for (usize j = 0; j < i; ++j) EXPECT_NE(variants[i].hash(), variants[j].hash()) << i << " vs " << j;
    }
    // Uniform payload does not.
    FFPipelineState payload = base;
    payload.line_stipple.factor = 3;
    payload.line_stipple.pattern = 0x00FF;
    payload.alpha_ref_float = true;
    payload.back_material.diffuse[0] = 0.25f;
    payload.lights[0].gl_spot_cutoff = 30;
    EXPECT_EQ(payload.hash(), base.hash());
}

TEST(FFEmulationGl, UnsupportedCombinationsFailClosed) {
    auto invalid = [](const FFPipelineState& s) { return !generate_ff_shaders(s).valid(); };
    FFPipelineState s = gl_base();
    EXPECT_FALSE(invalid(s));
    s.vertex_tweening = true;
    EXPECT_TRUE(invalid(s)) << "homogeneous + tweening";
    s = gl_base();
    s.fvf_flags = (s.fvf_flags & ~fvf::POSITION_MASK) | fvf::XYZB2;
    s.vertex_blend_weight_count = 1;
    EXPECT_TRUE(invalid(s)) << "homogeneous + blending";
    s = gl_base();
    s.fvf_flags = (s.fvf_flags & ~fvf::POSITION_MASK) | fvf::XYZRHW;
    EXPECT_TRUE(invalid(s)) << "homogeneous + pretransformed";
    s = gl_base();
    s.active_texture_stages = 1;
    s.texture_stages[0].color_op = TexStageOp::Modulate;
    s.texture_stages[0].texcoord_index = 0x20000u;
    EXPECT_TRUE(invalid(s)) << "homogeneous + TCI";
    s = gl_base();
    s.two_sided_lighting = true;
    EXPECT_TRUE(invalid(s)) << "two-sided without the GL model";
    s = gl_base();
    s.texgen_plane_mode[1][0] = FFTexGenMode::ObjectLinear;   // only set 0 is declared
    EXPECT_TRUE(invalid(s)) << "texgen into an undeclared set";
    s = gl_base();
    s.texgen_plane_mode[0][2] = FFTexGenMode::SphereMap;
    EXPECT_TRUE(invalid(s)) << "sphere map on r";
    s = gl_base();
    s.fvf_flags &= ~fvf::NORMAL;
    s.texgen_plane_mode[0][0] = FFTexGenMode::SphereMap;
    EXPECT_TRUE(invalid(s)) << "sphere map without a normal";
}

TEST(FFEmulationGl, NewPathsUseOnlyTheRowVectorForm) {
    FFPipelineState s = gl_base();
    s.lighting_enabled = true;
    s.gl_spot_model = true;
    s.two_sided_lighting = true;
    s.active_light_count = 2;
    s.normalize_normals = true;
    s.point_size_output = true;
    s.point_scale_enabled = true;
    s.clip_planes_enabled = true;
    s.fog_mode = FogMode::Linear;
    s.texgen_plane_mode[0] = {FFTexGenMode::SphereMap, FFTexGenMode::EyeLinear, FFTexGenMode::ObjectLinear,
                              FFTexGenMode::None};
    s.active_texture_stages = 1;
    s.texture_stages[0].color_op = TexStageOp::Modulate;
    s.texture_stages[0].texture_transform_flags = 4;
    const FFGeneratedShaders g = generate_ff_shaders(s);
    ASSERT_TRUE(g.valid());
    for (const char* legacy : {"cb.world_view_proj * ", "cb.view * cb.world", "cb.world * float4", "cb.world * in.",
                               "cb.world_inv_transpose * ", "cb.view * float4", "cb.view * tci",
                               "cb.texture_matrix[0] * "}) {
        EXPECT_FALSE(contains(g.vertex_msl, legacy)) << legacy << "\n" << g.vertex_msl;
        EXPECT_FALSE(contains(g.fragment_msl, legacy)) << legacy << "\n" << g.fragment_msl;
    }
    EXPECT_TRUE(contains(g.vertex_msl, "in.position * cb.world"));
    EXPECT_TRUE(contains(g.vertex_msl, " * cb.world_inv_transpose"));
    EXPECT_TRUE(contains(g.fragment_msl, "tc = tc * cb.texture_matrix[0];"));
}

TEST(FFEmulationGl, AlphaRefFloatPacksTheFloatReference) {
    FFPipelineState s = gl_base();
    s.alpha_test_enable = true;
    s.alpha_test_func = 5;
    s.alpha_ref = 0x40;
    FFUniformInputs in;
    in.alpha_reference = 0.3f;
    std::vector<u8> d3d, gl, clamped;
    pack_ff_fragment_uniforms(s, in, d3d);
    s.alpha_ref_float = true;
    pack_ff_fragment_uniforms(s, in, gl);
    in.alpha_reference = 1.5f;
    pack_ff_fragment_uniforms(s, in, clamped);
    ASSERT_EQ(d3d.size(), gl.size());
    // texture_factor, stage_constants[8], lod_biases[8], then alpha_ref.
    const usize at = 16 + 8 * 16 + 8 * 4;
    float a = 0, b = 0, c = 0;
    std::memcpy(&a, d3d.data() + at, 4);
    std::memcpy(&b, gl.data() + at, 4);
    std::memcpy(&c, clamped.data() + at, 4);
    EXPECT_FLOAT_EQ(a, 64.0f / 255.0f);
    EXPECT_FLOAT_EQ(b, 0.3f);
    EXPECT_FLOAT_EQ(c, 1.0f);
}

// The packed texgen planes follow each coordinate's mode; the vertex block
// grows by 16 float4s exactly when a linear mode is in use.
TEST(FFEmulationGl, TexgenPlanesPackPerMode) {
    FFPipelineState s = gl_base();
    FFUniformInputs in;
    for (u32 u = 0; u < 4; ++u)
        for (u32 c = 0; c < 4; ++c) {
            in.texgen_object_planes[u][c] = {1.0f + u, 2.0f + c, 3, 4};
            in.texgen_eye_planes[u][c] = {-1.0f - u, -2.0f - c, -3, -4};
        }
    std::vector<u8> none, linear;
    pack_ff_vertex_uniforms(s, in, none);
    s.texgen_plane_mode[0] = {FFTexGenMode::ObjectLinear, FFTexGenMode::EyeLinear, FFTexGenMode::None,
                              FFTexGenMode::SphereMap};
    pack_ff_vertex_uniforms(s, in, linear);
    ASSERT_EQ(linear.size(), none.size() + 16 * 16);
    float planes[16][4];
    std::memcpy(planes, linear.data() + none.size(), sizeof(planes));
    EXPECT_FLOAT_EQ(planes[0][0], 1.0f);
    EXPECT_FLOAT_EQ(planes[0][1], 2.0f);
    EXPECT_FLOAT_EQ(planes[1][0], -1.0f);
    EXPECT_FLOAT_EQ(planes[1][1], -3.0f);
    for (int k = 0; k < 4; ++k) {
        EXPECT_EQ(planes[2][k], 0.0f);
        EXPECT_EQ(planes[3][k], 0.0f);
        EXPECT_EQ(planes[4][k], 0.0f);
    }
}
