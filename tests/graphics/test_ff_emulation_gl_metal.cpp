// SPDX-License-Identifier: LGPL-2.1-only
/// @file test_ff_emulation_gl_metal.cpp
/// @brief The OpenGL fields of the shared fixed-function generator, run on
///        Metal and compared with the GL 1.1 spec's formulas evaluated on the
///        CPU (never with the generator's own output).
///
/// Geometry is placed with the row-vector convention (ff_emulation.hpp): an
/// FFMatrix is applied as p * M. `kOrtho` maps eye (x, y) to the pixel whose
/// centre is (x, y) in image coordinates (row 0 at the top) and eye z in
/// [-100, 100] to depth [0, 1]. Values read back from RGBA32Float targets
/// are the fragment outputs themselves.

#include <gtest/gtest.h>

#include "graphics/contracts/backend_device.hpp"
#include "graphics/metal/metal_device.hpp"
#include "graphics/shaders/ff_emulation.hpp"
#include "graphics/shaders/ff_uniforms.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <string>
#include <vector>

using namespace vitreo;
using namespace vitreo::graphics;

namespace {

/// The test vertex: the same field order and offsets as GL's GlBatchVertex.
struct Vtx {
    float pos[4];
    float normal[3];
    float color[4];
    float tex[4];
    float line_distance;
};
static_assert(sizeof(Vtx) == 64);

constexpr u32 kFvf = fvf::XYZ | fvf::NORMAL | fvf::DIFFUSE | fvf::TEX1 | (2u << 16);   // texcoord 0 is float4

FFMatrix ortho(float w, float h) {
    return {2.0f / w, 0, 0, 0, 0, -2.0f / h, 0, 0, 0, 0, 1.0f / 200.0f, 0, -1, 1, 0.5f, 1};
}

FFPipelineState gl_state() {
    FFPipelineState s{};
    s.fvf_flags = kFvf;
    s.position_is_homogeneous = true;
    s.lighting_enabled = false;
    s.local_viewer = false;
    s.vertex_color_enable = true;
    s.specular_enable = false;
    return s;
}

struct Target {
    u32 width, height;
    PixelFormat format;
    std::array<float, 4> clear;
};

/// Draws @p vertices with the generated pipeline for @p state (or the
/// generated vertex shader with @p fragment_msl / @p fragment_entry) into a
/// fresh target and returns its bytes (row 0 = the top row).
std::vector<u8> render(const FFPipelineState& state, const FFUniformInputs& inputs, const std::vector<Vtx>& vertices,
                       BackendPrimitiveTopology topology, const Target& target, bool front_ccw = false,
                       const std::string& fragment_msl = {}, const std::string& fragment_entry = {}) {
    auto& device = MetalDevice::instance();
    const FFGeneratedShaders shaders = generate_ff_shaders(state);
    EXPECT_TRUE(shaders.valid());
    if (!shaders.valid()) return {};

    BackendPipelineDesc pipe{};
    pipe.vertex_shader_msl = shaders.vertex_msl;
    pipe.fragment_shader_msl = fragment_msl.empty() ? shaders.fragment_msl : fragment_msl;
    pipe.vertex_entry_point = "ff_vertex";
    pipe.fragment_entry_point = fragment_entry.empty() ? "ff_fragment" : fragment_entry;
    unsigned location = 0;
    pipe.vertex_attributes.push_back({location++, offsetof(Vtx, pos),
                                      state.position_is_homogeneous ? BackendVertexFormat::Float4
                                                                    : BackendVertexFormat::Float3});
    pipe.vertex_attributes.push_back({location++, offsetof(Vtx, normal), BackendVertexFormat::Float3});
    pipe.vertex_attributes.push_back({location++, offsetof(Vtx, color), BackendVertexFormat::Float4});
    pipe.vertex_attributes.push_back({location++, offsetof(Vtx, tex), BackendVertexFormat::Float4});
    if (state.line_stipple.enabled) {
        pipe.vertex_attributes.push_back({location++, offsetof(Vtx, line_distance), BackendVertexFormat::Float1});
    }
    pipe.vertex_buffer_strides[0] = sizeof(Vtx);
    pipe.vertex_stride = sizeof(Vtx);
    pipe.color_format = target.format;
    pipe.color_formats[0] = target.format;
    pipe.color_write_mask = 0xF;
    pipe.color_write_masks[0] = 0xF;
    pipe.input_primitive_class = topology == BackendPrimitiveTopology::PointList ? BackendPrimitiveClass::Point
                                                                                 : BackendPrimitiveClass::Triangle;
    pipe.cache_key = shaders.state_hash ^ std::hash<std::string>{}(pipe.fragment_shader_msl);
    auto pipeline = device.create_pipeline(pipe);
    EXPECT_TRUE(pipeline && pipeline->valid()) << shaders.vertex_msl << "\n----\n" << pipe.fragment_shader_msl;
    if (!pipeline || !pipeline->valid()) return {};

    BackendBufferDesc vb_desc{};
    vb_desc.byte_width = vertices.size() * sizeof(Vtx);
    vb_desc.vertex_buffer = true;
    auto vb = device.create_buffer(vb_desc, vertices.data());
    BackendTexture2DDesc rt{};
    rt.width = target.width;
    rt.height = target.height;
    rt.mip_levels = 1;
    rt.array_size = 1;
    rt.pixel_format = target.format;
    rt.render_target = true;
    rt.shader_resource = true;
    auto color = device.create_texture_2d(rt, nullptr, 0);
    EXPECT_TRUE(vb && color && color->valid());
    if (!vb || !color) return {};
    EXPECT_TRUE(color->clear_render_target(target.clear));

    std::vector<u8> vs, fs;
    pack_ff_vertex_uniforms(state, inputs, vs);
    pack_ff_fragment_uniforms(state, inputs, fs);
    BackendRenderPassDesc pass_desc{};
    pass_desc.color_targets[0] = color.get();
    pass_desc.preserve_color_contents = true;
    auto pass = device.create_render_pass_standalone(pass_desc);
    EXPECT_TRUE(pass && pass->valid());
    BackendDrawCall call{};
    call.pipeline = pipeline.get();
    call.topology = topology;
    call.vertex_buffer = vb.get();
    call.vertex_buffers[0] = vb.get();
    call.vertex_count = static_cast<unsigned>(vertices.size());
    call.viewport = {0.0f, 0.0f, static_cast<float>(target.width), static_cast<float>(target.height), 0.0f, 1.0f};
    call.vertex_constants = vs.data();
    call.vertex_constants_size = vs.size();
    call.fragment_constants = fs.data();
    call.fragment_constants_size = fs.size();
    call.front_counter_clockwise = front_ccw;
    EXPECT_TRUE(pass->draw(call));
    EXPECT_TRUE(pass->end());
    const u32 texel = target.format == PixelFormat::RGBA32Float ? 16u : 4u;
    std::vector<u8> pixels(static_cast<usize>(target.width) * target.height * texel, 0);
    EXPECT_TRUE(color->readback(0, pixels.data(), target.width * texel));
    return pixels;
}

std::array<float, 4> texel_f(const std::vector<u8>& px, u32 width, u32 x, u32 y) {
    std::array<float, 4> c{};
    std::memcpy(c.data(), px.data() + (static_cast<usize>(y) * width + x) * 16, 16);
    return c;
}

/// BGRA8 bytes -> RGBA.
std::array<u8, 4> texel_8(const std::vector<u8>& px, u32 width, u32 x, u32 y) {
    const u8* p = px.data() + (static_cast<usize>(y) * width + x) * 4;
    return {p[2], p[1], p[0], p[3]};
}

class FFEmulationGlMetal : public ::testing::Test {
protected:
    void SetUp() override {
        if (!MetalDevice::instance().available()) GTEST_SKIP() << "no Metal device";
    }
};

// ---- GL 1.1 spec 2.13.1 on the CPU ----------------------------------------------------

using D3 = std::array<double, 3>;
D3 sub(const D3& a, const D3& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
D3 add(const D3& a, const D3& b) { return {a[0] + b[0], a[1] + b[1], a[2] + b[2]}; }
D3 scale(const D3& a, double k) { return {a[0] * k, a[1] * k, a[2] * k}; }
double dot(const D3& a, const D3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
D3 unit(const D3& a) { return scale(a, 1.0 / std::sqrt(dot(a, a))); }

struct SpecLight {
    D3 ambient, diffuse, specular;
    std::array<double, 4> position;   // eye space
    D3 spot_direction;
    double spot_exponent, spot_cutoff;   // degrees; 180 = no spot
    double k0, k1, k2;
};
struct SpecMaterial {
    D3 ambient, diffuse, specular, emission;
    double alpha, shininess;
};

/// c = e_cm + a_cm a_cs + sum_i att_i spot_i (a_cm a_cli + (n (.) VP_pli) d_cm d_cli
///     + f_i (n (.) h_i)^s_rm s_cm s_cli), (.) the clamped dot max(d1 . d2, 0)
///     (eq. 2.2), f_i = 1 when n (.) VP_pli != 0, the spot (PV (.) s)^srli
///     when PV (.) s >= cos(crli) (eq. 2.5); each dot clamped at 0 (the spec's
/// "dot-with-max"), clamped to [0, 1] (spec 2.13.6 with lighting).
std::array<double, 4> spec_light(const D3& V, const D3& n, const std::vector<SpecLight>& lights,
                                 const SpecMaterial& m, const D3& scene_ambient, bool local_viewer) {
    D3 c = add(m.emission, {m.ambient[0] * scene_ambient[0], m.ambient[1] * scene_ambient[1],
                            m.ambient[2] * scene_ambient[2]});
    for (const SpecLight& l : lights) {
        D3 VP;
        double att = 1.0;
        if (l.position[3] != 0.0) {
            const D3 P{l.position[0] / l.position[3], l.position[1] / l.position[3], l.position[2] / l.position[3]};
            const D3 d3 = sub(P, V);
            const double d = std::sqrt(dot(d3, d3));
            VP = scale(d3, 1.0 / d);
            att = 1.0 / (l.k0 + l.k1 * d + l.k2 * d * d);
        } else {
            VP = unit({l.position[0], l.position[1], l.position[2]});
        }
        double spot = 1.0;
        if (l.spot_cutoff != 180.0) {
            const double sd = dot(scale(VP, -1.0), unit(l.spot_direction));
            // cos(90 degrees) is exactly 0.
            const double cutoff_cos = l.spot_cutoff == 90.0 ? 0.0 : std::cos(l.spot_cutoff * M_PI / 180.0);
            const double pv_s = std::max(sd, 0.0);
            spot = pv_s >= cutoff_cos ? (l.spot_exponent == 0.0 ? 1.0 : std::pow(pv_s, l.spot_exponent)) : 0.0;
        }
        const double ndvp = dot(n, VP);
        const D3 eye = local_viewer ? unit(scale(V, -1.0)) : D3{0, 0, 1};
        const D3 h = unit(add(VP, eye));
        const double f = std::max(ndvp, 0.0) != 0.0 ? 1.0 : 0.0;
        const double nh = std::max(dot(n, h), 0.0);
        const double sp = f * (m.shininess == 0.0 ? 1.0 : std::pow(nh, m.shininess));
        for (int k = 0; k < 3; ++k) {
            c[k] += att * spot *
                    (m.ambient[k] * l.ambient[k] + std::max(ndvp, 0.0) * m.diffuse[k] * l.diffuse[k] +
                     sp * m.specular[k] * l.specular[k]);
        }
    }
    return {std::clamp(c[0], 0.0, 1.0), std::clamp(c[1], 0.0, 1.0), std::clamp(c[2], 0.0, 1.0),
            std::clamp(m.alpha, 0.0, 1.0)};
}

void set_light(FFLight& f, const SpecLight& l) {
    f.enabled = true;
    for (int k = 0; k < 3; ++k) {
        f.ambient[k] = static_cast<float>(l.ambient[k]);
        f.diffuse[k] = static_cast<float>(l.diffuse[k]);
        f.specular[k] = static_cast<float>(l.specular[k]);
        f.position[k] = static_cast<float>(l.position[k]);
        f.direction[k] = static_cast<float>(l.spot_direction[k]);
    }
    f.ambient[3] = f.diffuse[3] = f.specular[3] = 1;
    f.gl_position_w = static_cast<float>(l.position[3]);
    f.gl_spot_exponent = static_cast<float>(l.spot_exponent);
    f.gl_spot_cutoff = static_cast<float>(l.spot_cutoff);
    f.attenuation[0] = static_cast<float>(l.k0);
    f.attenuation[1] = static_cast<float>(l.k1);
    f.attenuation[2] = static_cast<float>(l.k2);
}

void set_material(FFMaterial& f, const SpecMaterial& m) {
    for (int k = 0; k < 3; ++k) {
        f.ambient[k] = static_cast<float>(m.ambient[k]);
        f.diffuse[k] = static_cast<float>(m.diffuse[k]);
        f.specular[k] = static_cast<float>(m.specular[k]);
        f.emissive[k] = static_cast<float>(m.emission[k]);
    }
    f.diffuse[3] = static_cast<float>(m.alpha);
    f.power = static_cast<float>(m.shininess);
}

const std::vector<SpecLight> kLights = {
    // A spotlight: positional, attenuated, cutoff 40, exponent 3.
    {{0.05, 0.02, 0.0}, {0.5, 0.4, 0.3}, {0.6, 0.6, 0.5}, {8, 0.5, 5, 1}, {0, 0, -1}, 3, 40, 0.5, 0.05, 0.01},
    // A positional light with no spot (cutoff 180) and w = 2.
    {{0.0, 0.05, 0.05}, {0.3, 0.2, 0.4}, {0.3, 0.2, 0.2}, {-4, 6, 8, 2}, {0, 0, -1}, 0, 180, 1, 0.1, 0.0},
    // A directional light.
    {{0.02, 0.02, 0.02}, {0.2, 0.3, 0.1}, {0.2, 0.2, 0.4}, {0.3, 0.2, 1, 0}, {0, 0, -1}, 0, 180, 1, 0, 0},
    // A directional light with a spot cutoff: the spec's spot term still
    // applies (P_pli V is the light's direction negated).
    {{0.0, 0.0, 0.03}, {0.2, 0.1, 0.2}, {0.1, 0.3, 0.1}, {-0.2, 0.1, 1, 0}, {0.1, 0, -1}, 2, 60, 1, 0, 0},
};

}  // namespace

namespace {

/// Lights 16 points (eye positions along a row, varied normals, w = 2 for
/// odd points, GL_NORMALIZE on) with @p lights and compares each pixel with
/// spec_light().
void check_lit_points(const std::vector<SpecLight>& lights, const SpecMaterial& m, const D3& scene, bool local,
                      const char* what) {
    constexpr u32 kW = 16;
    FFPipelineState s = gl_state();
    s.lighting_enabled = true;
    s.gl_spot_model = true;
    s.vertex_color_enable = false;
    s.normalize_normals = true;
    s.local_viewer = local;
    s.point_size_output = true;
    s.active_light_count = static_cast<u32>(lights.size());
    for (usize i = 0; i < lights.size(); ++i) set_light(s.lights[i], lights[i]);
    set_material(s.material, m);
    for (int k = 0; k < 3; ++k) s.global_ambient[k] = static_cast<float>(scene[k]);
    FFUniformInputs in;
    in.projection = ortho(kW, 1);
    in.point_size = in.point_size_min = in.point_size_max = 1;

    std::vector<Vtx> v;
    std::vector<D3> positions, normals;
    for (u32 i = 0; i < kW; ++i) {
        const D3 p{i + 0.5, 0.5, -2.0 - 0.25 * i};
        const D3 n = unit({std::sin(i * 0.45), 0.3 - 0.05 * i, std::cos(i * 0.45)});
        positions.push_back(p);
        normals.push_back(n);
        Vtx x{};
        const float w = (i & 1) ? 2.0f : 1.0f;   // homogeneous object coordinates
        x.pos[0] = static_cast<float>(p[0]) * w;
        x.pos[1] = static_cast<float>(p[1]) * w;
        x.pos[2] = static_cast<float>(p[2]) * w;
        x.pos[3] = w;
        for (int k = 0; k < 3; ++k) x.normal[k] = static_cast<float>(n[k] * 1.7);   // GL_NORMALIZE is on
        x.color[3] = 1;
        v.push_back(x);
    }
    const auto px =
        render(s, in, v, BackendPrimitiveTopology::PointList, {kW, 1, PixelFormat::RGBA32Float, {9, 9, 9, 9}});
    ASSERT_FALSE(px.empty());
    for (u32 i = 0; i < kW; ++i) {
        const auto want = spec_light(positions[i], normals[i], lights, m, scene, local);
        const auto got = texel_f(px, kW, i, 0);
        for (int k = 0; k < 4; ++k) {
            EXPECT_NEAR(got[k], want[k], 2e-4) << what << ": point " << i << " channel " << k << " local=" << local
                                               << " shininess=" << m.shininess;
        }
    }
}

const SpecMaterial kMaterial{{0.2, 0.1, 0.05}, {0.6, 0.5, 0.4}, {0.5, 0.5, 0.5}, {0.02, 0.03, 0.01}, 0.7, 7};
const D3 kScene{0.1, 0.12, 0.08};

}  // namespace

TEST_F(FFEmulationGlMetal, GlSpotlightFollowsSpecFormula) {
    for (bool local : {false, true}) {
        for (double shininess : {7.0, 0.0}) {
            SpecMaterial m = kMaterial;
            m.shininess = shininess;
            check_lit_points(kLights, m, kScene, local, "four lights");
        }
    }
}

// Eq. 2.5 compares the clamped dot PV (.) s with cos(cutoff): with cutoff 90
// and exponent 0, a vertex behind the light (PV . s < 0) still gets spot 1;
// with exponent 2 it gets 0^2 = 0. Every point here is behind the light.
TEST_F(FFEmulationGlMetal, GlSpotCutoff90LightsBehindTheLight) {
    for (double exponent : {0.0, 2.0}) {
        const std::vector<SpecLight> lights = {
            {{0.1, 0.1, 0.1}, {0.6, 0.5, 0.4}, {0.4, 0.4, 0.4}, {8, 0.5, -20, 1}, {0, 0, -1}, exponent, 90, 1, 0, 0},
        };
        check_lit_points(lights, kMaterial, kScene, false,
                         exponent == 0.0 ? "cutoff 90, exponent 0" : "cutoff 90, exponent 2");
    }
    // The oracle itself: behind a 90-degree, exponent-0 spot is lit.
    const std::vector<SpecLight> behind = {
        {{0, 0, 0}, {1, 1, 1}, {0, 0, 0}, {0, 0, -20, 1}, {0, 0, -1}, 0, 90, 1, 0, 0}};
    const SpecMaterial white{{0, 0, 0}, {1, 1, 1}, {0, 0, 0}, {0, 0, 0}, 1, 0};
    EXPECT_GT(spec_light({0, 0, 0}, {0, 0, -1}, behind, white, {0, 0, 0}, false)[0], 0.5);
}

// Spec 2.13.1: with LIGHT_MODEL_TWO_SIDE the back colour uses the back
// material and the negated normal, and back-facing polygons take it.
TEST_F(FFEmulationGlMetal, TwoSidedLightingSelectsBackMaterial) {
    constexpr u32 kS = 8;
    const std::vector<SpecLight> lights = {
        {{0, 0, 0}, {0.8, 0.1, 0.0}, {0, 0, 0}, {0, 0, 1, 0}, {0, 0, -1}, 0, 180, 1, 0, 0},
        {{0, 0, 0}, {0.0, 0.2, 0.9}, {0, 0, 0}, {0.2, 0, -1, 0}, {0, 0, -1}, 0, 180, 1, 0, 0},
    };
    const SpecMaterial front{{0.1, 0.1, 0.1}, {1.0, 0.9, 0.8}, {0, 0, 0}, {0.05, 0, 0}, 0.9, 0};
    const SpecMaterial back{{0.2, 0.3, 0.1}, {0.5, 0.6, 0.7}, {0, 0, 0}, {0, 0, 0.05}, 0.4, 0};
    const D3 scene{0.2, 0.2, 0.2};
    FFPipelineState s = gl_state();
    s.lighting_enabled = true;
    s.gl_spot_model = true;
    s.two_sided_lighting = true;
    s.vertex_color_enable = false;
    s.active_light_count = 2;
    for (usize i = 0; i < lights.size(); ++i) set_light(s.lights[i], lights[i]);
    set_material(s.material, front);
    set_material(s.back_material, back);
    for (int k = 0; k < 3; ++k) s.global_ambient[k] = static_cast<float>(scene[k]);
    FFUniformInputs in;
    in.projection = ortho(kS, kS);
    // One triangle covering the target: (0,0) -> (16,0) -> (0,16) in image
    // coordinates, clockwise as it appears in the image.
    std::vector<Vtx> v(3);
    const float corners[3][2] = {{0, 0}, {16, 0}, {0, 16}};
    for (int i = 0; i < 3; ++i) {
        v[i].pos[0] = corners[i][0];
        v[i].pos[1] = corners[i][1];
        v[i].pos[2] = -1;
        v[i].pos[3] = 1;
        v[i].normal[2] = 1;
    }
    const D3 n{0, 0, 1};
    const auto want_front = spec_light({4, 4, -1}, n, lights, front, scene, false);
    const auto want_back = spec_light({4, 4, -1}, scale(n, -1), lights, back, scene, false);
    ASSERT_GT(std::abs(want_front[0] - want_back[0]), 0.1);
    // Front-facing in the image (counter-clockwise front off): the front colour.
    // With counter-clockwise front, the same triangle is back-facing.
    for (bool ccw : {false, true}) {
        const auto px =
            render(s, in, v, BackendPrimitiveTopology::TriangleList, {kS, kS, PixelFormat::RGBA32Float, {9, 9, 9, 9}},
                   ccw);
        ASSERT_FALSE(px.empty());
        const auto& want = ccw ? want_back : want_front;
        for (u32 y = 0; y < kS; ++y)
            for (u32 x = 0; x < kS; ++x) {
                const auto got = texel_f(px, kS, x, y);
                for (int k = 0; k < 4; ++k) EXPECT_NEAR(got[k], want[k], 2e-4) << x << "," << y << " ccw=" << ccw;
            }
    }
}

// Spec 2.13.1: points (and lines) always take the front colour under
// two-sided lighting, whichever winding the pipeline calls front.
TEST_F(FFEmulationGlMetal, TwoSidedLightingPointsUseTheFrontColour) {
    constexpr u32 kW = 4;
    const std::vector<SpecLight> lights = {
        {{0, 0, 0}, {0.8, 0.1, 0.0}, {0, 0, 0}, {0, 0, 1, 0}, {0, 0, -1}, 0, 180, 1, 0, 0},
        {{0, 0, 0}, {0.0, 0.2, 0.9}, {0, 0, 0}, {0.2, 0, -1, 0}, {0, 0, -1}, 0, 180, 1, 0, 0},
    };
    const SpecMaterial front{{0.1, 0.1, 0.1}, {1.0, 0.9, 0.8}, {0, 0, 0}, {0.05, 0, 0}, 0.9, 0};
    const SpecMaterial back{{0.2, 0.3, 0.1}, {0.5, 0.6, 0.7}, {0, 0, 0}, {0, 0, 0.05}, 0.4, 0};
    const D3 scene{0.2, 0.2, 0.2};
    FFPipelineState s = gl_state();
    s.lighting_enabled = true;
    s.gl_spot_model = true;
    s.two_sided_lighting = true;
    s.vertex_color_enable = false;
    s.point_size_output = true;
    s.active_light_count = 2;
    for (usize i = 0; i < lights.size(); ++i) set_light(s.lights[i], lights[i]);
    set_material(s.material, front);
    set_material(s.back_material, back);
    for (int k = 0; k < 3; ++k) s.global_ambient[k] = static_cast<float>(scene[k]);
    EXPECT_EQ(generate_ff_shaders(s).fragment_msl.find("[[front_facing]]"), std::string::npos);
    FFUniformInputs in;
    in.projection = ortho(kW, 1);
    in.point_size = in.point_size_min = in.point_size_max = 1;
    std::vector<Vtx> v(kW);
    for (u32 i = 0; i < kW; ++i) {
        v[i].pos[0] = i + 0.5f;
        v[i].pos[1] = 0.5f;
        v[i].pos[2] = -1;
        v[i].pos[3] = 1;
        v[i].normal[2] = 1;
    }
    const auto want = spec_light({0, 0, -1}, {0, 0, 1}, lights, front, scene, false);
    for (bool ccw : {false, true}) {
        const auto px = render(s, in, v, BackendPrimitiveTopology::PointList,
                               {kW, 1, PixelFormat::RGBA32Float, {9, 9, 9, 9}}, ccw);
        ASSERT_FALSE(px.empty());
        for (u32 x = 0; x < kW; ++x) {
            const auto got = texel_f(px, kW, x, 0);
            for (int k = 0; k < 4; ++k) EXPECT_NEAR(got[k], want[k], 2e-4) << x << " ccw=" << ccw;
        }
    }
}

// Spec 2.10.4: EYE_LINEAR is p' . (xe, ye, ze, we) with the eye coordinates
// of the vertex (p' already carries M^-1 from glTexGen time); OBJECT_LINEAR
// is p . (xo, yo, zo, wo); SPHERE_MAP is the reflection-vector formula; a
// disabled coordinate is the vertex's own.
TEST_F(FFEmulationGlMetal, EyeLinearTexgenUsesPlaneInEyeSpace) {
    constexpr u32 kW = 12;
    // Modelview: rotation about y, a scale and a translation (GL math form,
    // column vectors), passed to the generator in the row-vector form
    // world = transpose(M).
    const double c = std::cos(0.7), sn = std::sin(0.7);
    const double M[4][4] = {{1.5 * c, 0, 1.5 * sn, 2}, {0, 0.8, 0, -1}, {-sn, 0, c, -3}, {0, 0, 0, 1}};
    FFMatrix world{};
    for (int r = 0; r < 4; ++r)
        for (int col = 0; col < 4; ++col) world[col * 4 + r] = static_cast<float>(M[r][col]);
    FFMatrix inverse_world{};
    ASSERT_TRUE(ff_matrix_invert(world, inverse_world));
    FFUniformInputs in;
    in.world_matrices[0] = world;
    // clip = p * world * world^-1 * ortho = p * ortho: object (x, y) is the pixel.
    in.projection = ff_matrix_multiply(inverse_world, ortho(kW, 1));
    in.point_size = in.point_size_min = in.point_size_max = 1;
    const std::array<float, 4> eye_s{0.3f, -0.2f, 0.5f, 0.25f}, obj_t{-0.1f, 0.4f, 0.2f, 1.0f},
        eye_r{0.05f, 0.1f, -0.3f, 2.0f};
    in.texgen_eye_planes[0][0] = eye_s;
    in.texgen_object_planes[0][1] = obj_t;
    in.texgen_eye_planes[0][2] = eye_r;
    in.texgen_object_planes[0][0] = {9, 9, 9, 9};   // unused: s is eye-linear
    in.texgen_eye_planes[0][1] = {9, 9, 9, 9};      // unused: t is object-linear

    const std::string fragment =
        "#include <metal_stdlib>\nusing namespace metal;\n"
        "struct TexgenIn { float4 position [[position]]; float4 texcoord0; };\n"
        "fragment float4 texgen_out(TexgenIn in [[stage_in]]) { return in.texcoord0; }\n";

    std::vector<Vtx> v;
    std::vector<std::array<double, 4>> obj;
    std::vector<D3> normals;
    for (u32 i = 0; i < kW; ++i) {
        const double w = (i % 3 == 0) ? 0.5 : 1.0;
        const std::array<double, 4> o{(i + 0.5) * w, 0.5 * w, (-1.0 - 0.3 * i) * w, w};
        obj.push_back(o);
        const D3 n{std::sin(i * 0.5), 0.4, std::cos(i * 0.5)};
        normals.push_back(n);
        Vtx x{};
        for (int k = 0; k < 4; ++k) x.pos[k] = static_cast<float>(o[k]);
        for (int k = 0; k < 3; ++k) x.normal[k] = static_cast<float>(n[k]);
        x.tex[0] = 0.11f;
        x.tex[1] = 0.22f;
        x.tex[2] = 0.33f;
        x.tex[3] = 0.75f + 0.01f * i;
        v.push_back(x);
    }
    auto eye_of = [&](const std::array<double, 4>& o) {
        std::array<double, 4> e{};
        for (int r = 0; r < 4; ++r)
            for (int k = 0; k < 4; ++k) e[r] += M[r][k] * o[k];
        return e;
    };
    auto dot4 = [](const std::array<float, 4>& p, const std::array<double, 4>& x) {
        return p[0] * x[0] + p[1] * x[1] + p[2] * x[2] + p[3] * x[3];
    };

    {
        FFPipelineState s = gl_state();
        s.point_size_output = true;
        s.texgen_plane_mode[0] = {FFTexGenMode::EyeLinear, FFTexGenMode::ObjectLinear, FFTexGenMode::EyeLinear,
                                  FFTexGenMode::None};
        const auto px = render(s, in, v, BackendPrimitiveTopology::PointList,
                               {kW, 1, PixelFormat::RGBA32Float, {9, 9, 9, 9}}, false, fragment, "texgen_out");
        ASSERT_FALSE(px.empty());
        for (u32 i = 0; i < kW; ++i) {
            const auto e = eye_of(obj[i]);
            const auto got = texel_f(px, kW, i, 0);
            EXPECT_NEAR(got[0], dot4(eye_s, e), 1e-4) << "s, point " << i;
            EXPECT_NEAR(got[1], dot4(obj_t, obj[i]), 1e-4) << "t, point " << i;
            EXPECT_NEAR(got[2], dot4(eye_r, e), 1e-4) << "r, point " << i;
            EXPECT_NEAR(got[3], v[i].tex[3], 1e-6) << "q, point " << i;
            // A generator that used object coordinates for EYE_LINEAR fails here.
            ASSERT_GT(std::abs(dot4(eye_s, e) - dot4(eye_s, obj[i])), 1e-2);
        }
    }
    {
        FFPipelineState s = gl_state();
        s.point_size_output = true;
        s.normalize_normals = true;
        s.texgen_plane_mode[0] = {FFTexGenMode::SphereMap, FFTexGenMode::SphereMap, FFTexGenMode::None,
                                  FFTexGenMode::None};
        const auto px = render(s, in, v, BackendPrimitiveTopology::PointList,
                               {kW, 1, PixelFormat::RGBA32Float, {9, 9, 9, 9}}, false, fragment, "texgen_out");
        ASSERT_FALSE(px.empty());
        // n' = n M^-1 (row vector, upper 3x3), normalized (GL_NORMALIZE).
        double Minv[4][4];
        for (int r = 0; r < 4; ++r)
            for (int col = 0; col < 4; ++col) Minv[r][col] = inverse_world[col * 4 + r];   // math M^-1
        for (u32 i = 0; i < kW; ++i) {
            const auto e = eye_of(obj[i]);
            const D3 u = unit({e[0] / e[3], e[1] / e[3], e[2] / e[3]});
            D3 np{};
            for (int j = 0; j < 3; ++j)
                for (int k = 0; k < 3; ++k) np[j] += normals[i][k] * Minv[k][j];
            np = unit(np);
            const D3 r = sub(u, scale(np, 2.0 * dot(np, u)));
            const double m = 2.0 * std::sqrt(r[0] * r[0] + r[1] * r[1] + (r[2] + 1) * (r[2] + 1));
            const auto got = texel_f(px, kW, i, 0);
            EXPECT_NEAR(got[0], r[0] / m + 0.5, 1e-4) << "s, point " << i;
            EXPECT_NEAR(got[1], r[1] / m + 0.5, 1e-4) << "t, point " << i;
            EXPECT_NEAR(got[2], v[i].tex[2], 1e-6) << "r, point " << i;
            EXPECT_NEAR(got[3], v[i].tex[3], 1e-6) << "q, point " << i;
        }
    }
}

// Spec 3.4.2: fragment s of a stippled line is drawn when bit
// floor(s / factor) mod 16 of the pattern is 1. The quad's line_distance runs
// 0..32 across the row, so the fragment at pixel x has counter s = x.
TEST_F(FFEmulationGlMetal, LineStipplePatternDiscardsPerFactor) {
    constexpr u32 kW = 40;
    constexpr u16 kPattern = 0xB38F;
    for (u32 factor : {1u, 2u, 3u, 256u, 0u}) {
        FFPipelineState s = gl_state();
        s.line_stipple.enabled = true;
        s.line_stipple.factor = factor;   // 0 clamps to 1 (glLineStipple clamps to [1, 256])
        s.line_stipple.pattern = kPattern;
        FFUniformInputs in;
        in.projection = ortho(kW, 1);
        std::vector<Vtx> v;
        const float xs[6] = {0, kW, 0, kW, kW, 0};
        const float ys[6] = {0, 0, 1, 0, 1, 1};
        for (int i = 0; i < 6; ++i) {
            Vtx x{};
            x.pos[0] = xs[i];
            x.pos[1] = ys[i];
            x.pos[3] = 1;
            x.color[1] = 1;
            x.color[3] = 1;
            x.line_distance = xs[i];
            v.push_back(x);
        }
        const auto px =
            render(s, in, v, BackendPrimitiveTopology::TriangleList, {kW, 1, PixelFormat::BGRA8Unorm, {0, 0, 0, 1}});
        ASSERT_FALSE(px.empty());
        const u32 r = std::clamp(factor, 1u, 256u);
        for (u32 x = 0; x < kW; ++x) {
            const bool on = ((kPattern >> ((x / r) % 16)) & 1u) != 0;
            const auto got = texel_8(px, kW, x, 0);
            EXPECT_EQ(got[1], on ? 255 : 0) << "pixel " << x << " factor " << factor;
        }
    }
}

// Spec 3.5.2: a polygon fragment at window (x, y) is kept when mask bit
// (x mod 32, y mod 32) is 1 (rows here in framebuffer orientation).
TEST_F(FFEmulationGlMetal, PolygonStippleMasksPixelsModulo32) {
    constexpr u32 kW = 40, kH = 36;
    FFPipelineState s = gl_state();
    s.polygon_stipple = true;
    FFUniformInputs in;
    in.projection = ortho(kW, kH);
    for (u32 r = 0; r < 32; ++r) in.polygon_stipple_rows[r] = 0x9E3779B9u * (r + 1) ^ (r << 7);
    std::vector<Vtx> v;
    const float xs[3] = {0, 2 * kW, 0};
    const float ys[3] = {0, 0, 2 * kH};
    for (int i = 0; i < 3; ++i) {
        Vtx x{};
        x.pos[0] = xs[i];
        x.pos[1] = ys[i];
        x.pos[3] = 1;
        x.color[0] = 1;
        x.color[3] = 1;
        v.push_back(x);
    }
    const auto px =
        render(s, in, v, BackendPrimitiveTopology::TriangleList, {kW, kH, PixelFormat::BGRA8Unorm, {0, 0, 0, 1}});
    ASSERT_FALSE(px.empty());
    for (u32 y = 0; y < kH; ++y)
        for (u32 x = 0; x < kW; ++x) {
            const bool on = ((in.polygon_stipple_rows[y % 32] >> (x % 32)) & 1u) != 0;
            EXPECT_EQ(texel_8(px, kW, x, y)[0], on ? 255 : 0) << x << "," << y;
        }
}

// Spec 4.1.10, Table 4.5: the logic op combines the fragment's colour s with
// the framebuffer's d bitwise; the generator reads d through framebuffer
// fetch. Every op is checked; XOR is the classic case.
TEST_F(FFEmulationGlMetal, LogicOpXorUsesFramebufferFetch) {
    if (!MetalDevice::instance().supports_framebuffer_fetch()) GTEST_SKIP() << "no framebuffer fetch";
    constexpr u32 kS = 4;
    const std::array<u8, 4> src{0x0F, 0xF0, 0xA5, 0x3C}, dst{0x33, 0xCC, 0x5A, 0xF0};
    for (u8 op = 0; op < 16; ++op) {
        FFPipelineState s = gl_state();
        s.logic_op = static_cast<FFLogicOp>(op);
        if (s.logic_op == FFLogicOp::Xor) {
            EXPECT_NE(generate_ff_shaders(s).fragment_msl.find("[[color(0)]]"), std::string::npos);
        }
        if (s.logic_op == FFLogicOp::Copy) {
            EXPECT_EQ(generate_ff_shaders(s).fragment_msl.find("[[color(0)]]"), std::string::npos);
        }
        FFUniformInputs in;
        in.projection = ortho(kS, kS);
        std::vector<Vtx> v(3);
        const float corners[3][2] = {{0, 0}, {2 * kS, 0}, {0, 2 * kS}};
        for (int i = 0; i < 3; ++i) {
            v[i].pos[0] = corners[i][0];
            v[i].pos[1] = corners[i][1];
            v[i].pos[3] = 1;
            for (int k = 0; k < 4; ++k) v[i].color[k] = src[k] / 255.0f;
        }
        const auto px = render(s, in, v, BackendPrimitiveTopology::TriangleList,
                               {kS, kS, PixelFormat::BGRA8Unorm,
                                {dst[0] / 255.0f, dst[1] / 255.0f, dst[2] / 255.0f, dst[3] / 255.0f}});
        ASSERT_FALSE(px.empty());
        for (int k = 0; k < 4; ++k) {
            const u32 a = src[k], b = dst[k];
            u32 want = 0;
            switch (op) {
            case 0: want = 0; break;
            case 1: want = a & b; break;
            case 2: want = a & ~b; break;
            case 3: want = a; break;
            case 4: want = ~a & b; break;
            case 5: want = b; break;
            case 6: want = a ^ b; break;
            case 7: want = a | b; break;
            case 8: want = ~(a | b); break;
            case 9: want = ~(a ^ b); break;
            case 10: want = ~b; break;
            case 11: want = a | ~b; break;
            case 12: want = ~a; break;
            case 13: want = ~a | b; break;
            case 14: want = ~(a & b); break;
            case 15: want = 0xFF; break;
            }
            for (u32 y = 0; y < kS; ++y)
                for (u32 x = 0; x < kS; ++x) {
                    EXPECT_EQ(texel_8(px, kS, x, y)[k], want & 0xFF) << "op " << int(op) << " channel " << k;
                }
        }
    }
}
