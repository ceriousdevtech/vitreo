// SPDX-License-Identifier: LGPL-2.1-only
/// @file test_ff_emulation_d3d_metal.cpp
/// @brief The Direct3D 9 transform pipeline of the shared fixed-function
///        generator, run on Metal and compared with Direct3D's own math
///        evaluated on the CPU in double precision (never with Vitreo output).
///
/// D3D9 has no Metal-device fixed-function draw harness (the D3D9 front-end
/// needs a window and a swap chain), so these tests drive the generator and
/// the Metal backend directly, with an FFPipelineState and FFUniformInputs
/// filled exactly as d3d9_draw_ff_state.cpp fills them: every D3DMATRIX is
/// copied verbatim (row-major, _11 first), the D3DTSS_/D3DRS_ values are the
/// raw guest values. The D3D7 counterpart, through the real ddraw front-end,
/// is D3D7DrawTest.D3D7TransformedVertexLandsWherePerspectiveMathSays.
///
/// Direct3D conventions, each from Microsoft Learn:
///   - "Transforms (Direct3D 9)", "World Transform", "View Transform",
///     "Projection Transform": row vectors, v' = v * World * View * Proj.
///   - "Camera Space Transformations (Direct3D 9)": camera-space position
///     V * wvMatrix; normal N * (wvMatrix^-1)^T on the 3x3 part, normalised
///     when NORMALIZENORMALS; light position Lp * vMatrix.
///   - "Viewports and Clipping (Direct3D 9)": X = (1 + x) * Width / 2,
///     Y = (1 - y) * Height / 2 after the divide by w.
///   - D3DXMatrixLookAtLH / D3DXMatrixPerspectiveFovLH / D3DXMatrixRotationY
///     pages: the matrices below are written out from their formulas.
///   - "Texture Coordinate Transformations (Direct3D 9)" and
///     D3DTEXTURETRANSFORMFLAGS: tc' = tc * D3DTS_TEXTUREn; PROJECTED divides
///     by the last element of the COUNT.
///   - D3DTSS_TCI: CAMERASPACEPOSITION / CAMERASPACENORMAL.
///   - "Fog Formulas (Direct3D 9)" / "Vertex Fog": linear f = (end - d) /
///     (end - start), d = camera-space z, or the camera-space distance with
///     D3DRS_RANGEFOGENABLE.
///   - D3DRENDERSTATETYPE D3DRS_POINTSCALE_A: Ss = Vh * Si *
///     sqrt(1 / (A + B * De + C * De^2)), De the camera-space distance.

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

constexpr u32 kW = 32, kH = 32;
constexpr u32 kTexcoordSize4 = 2u << 16;   // D3DFVF_TEXCOORDSIZE4(0)

/// D3DMATRIX in double: row-major, m[(r - 1) * 4 + (c - 1)] = _rc.
using M4 = std::array<double, 16>;
using V4 = std::array<double, 4>;
using D3 = std::array<double, 3>;

M4 mul(const M4& a, const M4& b) {
    M4 r{};
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            for (int k = 0; k < 4; ++k) r[i * 4 + j] += a[i * 4 + k] * b[k * 4 + j];
    return r;
}
/// Row vector times matrix: v * M.
V4 xf(const V4& v, const M4& m) {
    V4 r{};
    for (int j = 0; j < 4; ++j)
        for (int k = 0; k < 4; ++k) r[j] += v[k] * m[k * 4 + j];
    return r;
}
M4 transpose(const M4& m) {
    M4 r{};
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) r[i * 4 + j] = m[j * 4 + i];
    return r;
}
M4 inverse(const M4& m) {
    double a[4][8];
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 8; ++j) a[i][j] = j < 4 ? m[i * 4 + j] : (j - 4 == i ? 1.0 : 0.0);
    for (int c = 0; c < 4; ++c) {
        int p = c;
        for (int r = c + 1; r < 4; ++r)
            if (std::abs(a[r][c]) > std::abs(a[p][c])) p = r;
        for (int j = 0; j < 8; ++j) std::swap(a[c][j], a[p][j]);
        const double d = a[c][c];
        for (int j = 0; j < 8; ++j) a[c][j] /= d;
        for (int r = 0; r < 4; ++r) {
            if (r == c) continue;
            const double f = a[r][c];
            for (int j = 0; j < 8; ++j) a[r][j] -= f * a[c][j];
        }
    }
    M4 r{};
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) r[i * 4 + j] = a[i][j + 4];
    return r;
}
M4 translate(double x, double y, double z) { return {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, x, y, z, 1}; }
M4 scale(double x, double y, double z) { return {x, 0, 0, 0, 0, y, 0, 0, 0, 0, z, 0, 0, 0, 0, 1}; }
/// D3DXMatrixRotationY: [c 0 -s 0; 0 1 0 0; s 0 c 0; 0 0 0 1].
M4 rotation_y(double a) {
    const double c = std::cos(a), s = std::sin(a);
    return {c, 0, -s, 0, 0, 1, 0, 0, s, 0, c, 0, 0, 0, 0, 1};
}
/// D3DXMatrixRotationX: [1 0 0 0; 0 c s 0; 0 -s c 0; 0 0 0 1].
M4 rotation_x(double a) {
    const double c = std::cos(a), s = std::sin(a);
    return {1, 0, 0, 0, 0, c, s, 0, 0, -s, c, 0, 0, 0, 0, 1};
}
D3 sub(const D3& a, const D3& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
double dot(const D3& a, const D3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
D3 cross(const D3& a, const D3& b) {
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}
D3 unit(const D3& a) {
    const double l = std::sqrt(dot(a, a));
    return {a[0] / l, a[1] / l, a[2] / l};
}
/// D3DXMatrixLookAtLH: zaxis = norm(At - Eye), xaxis = norm(cross(Up,
/// zaxis)), yaxis = cross(zaxis, xaxis); rows are the axes' components with
/// -dot(axis, Eye) in the fourth row.
M4 look_at_lh(const D3& eye, const D3& at, const D3& up) {
    const D3 z = unit(sub(at, eye));
    const D3 x = unit(cross(up, z));
    const D3 y = cross(z, x);
    return {x[0], y[0], z[0], 0, x[1], y[1], z[1], 0, x[2], y[2], z[2], 0,
            -dot(x, eye), -dot(y, eye), -dot(z, eye), 1};
}
/// D3DXMatrixPerspectiveFovLH: yScale = cot(fovY / 2), xScale = yScale /
/// aspect; _33 = zf / (zf - zn), _34 = 1, _43 = -zn * zf / (zf - zn).
M4 perspective_fov_lh(double fovy, double aspect, double zn, double zf) {
    const double ys = 1.0 / std::tan(fovy / 2), xs = ys / aspect;
    return {xs, 0, 0, 0, 0, ys, 0, 0, 0, 0, zf / (zf - zn), 1, 0, 0, -zn * zf / (zf - zn), 0};
}
FFMatrix ff(const M4& m) {
    FFMatrix r{};
    for (int i = 0; i < 16; ++i) r[i] = static_cast<float>(m[i]);
    return r;
}
/// Clip -> pixel coordinates ("Viewports and Clipping"), row 0 at the top.
std::array<double, 2> screen(const V4& clip) {
    return {(1.0 + clip[0] / clip[3]) * kW / 2.0, (1.0 - clip[1] / clip[3]) * kH / 2.0};
}

/// A scene: the world, view and projection every test places geometry with.
struct Scene {
    M4 world, view, proj;
    M4 wv() const { return mul(world, view); }
    M4 wvp() const { return mul(mul(world, view), proj); }
};
Scene scene() {
    Scene s;
    s.world = mul(mul(rotation_y(0.6), rotation_x(-0.3)), translate(0.3, -0.2, 0.5));
    s.view = look_at_lh({1.0, 1.5, -6.0}, {0.2, 0.1, 0.0}, {0.0, 1.0, 0.0});
    s.proj = perspective_fov_lh(0.9, 1.0, 0.5, 50.0);
    return s;
}

/// The vertex: position, up to two blend weights, packed blend indices
/// (D3DFVF_LASTBETA_UBYTE4), normal, diffuse, a 4D texcoord.
struct Vtx {
    float pos[3];
    float weight[2];
    u8 indices[4];
    float normal[3];
    float color[4];
    float tex[4];
};

Vtx vtx(const D3& p, const D3& n = {0, 0, 1}, std::array<float, 4> tex = {0, 0, 0, 1}, float weight = 0) {
    Vtx v{};
    for (int k = 0; k < 3; ++k) v.pos[k] = static_cast<float>(p[k]);
    for (int k = 0; k < 3; ++k) v.normal[k] = static_cast<float>(n[k]);
    v.weight[0] = weight;
    v.color[0] = 0.0f;
    v.color[1] = 1.0f;
    v.color[2] = 0.0f;
    v.color[3] = 1.0f;
    for (int k = 0; k < 4; ++k) v.tex[k] = tex[k];
    return v;
}

FFUniformInputs inputs_for(const Scene& s) {
    FFUniformInputs in;
    in.world_matrices[0] = ff(s.world);
    in.view = ff(s.view);
    in.projection = ff(s.proj);
    in.viewport_width = kW;
    in.viewport_height = kH;
    in.point_size = in.point_size_min = 1.0f;
    in.point_size_max = 64.0f;
    return in;
}

FFPipelineState d3d_state(u32 fvf_flags) {
    FFPipelineState s{};
    s.fvf_flags = fvf_flags;
    s.lighting_enabled = false;
    return s;
}

struct Texture {
    u32 width = 0, height = 0;
    std::vector<float> rgba;
};

/// Draws @p vertices with the generated pipeline for @p state (or the
/// generated vertex shader with @p fragment_msl) into a fresh RGBA32Float
/// target cleared to 0 and returns its texels (row 0 = the top row).
std::vector<std::array<float, 4>> render(const FFPipelineState& state, const FFUniformInputs& inputs,
                                         const std::vector<Vtx>& vertices, BackendPrimitiveTopology topology,
                                         const std::string& fragment_msl = {}, const std::string& fragment_entry = {},
                                         const Texture* texture = nullptr) {
    auto& device = MetalDevice::instance();
    const FFGeneratedShaders shaders = generate_ff_shaders(state);
    EXPECT_TRUE(shaders.valid());
    if (!shaders.valid()) return {};

    BackendPipelineDesc pipe{};
    pipe.vertex_shader_msl = shaders.vertex_msl;
    pipe.fragment_shader_msl = fragment_msl.empty() ? shaders.fragment_msl : fragment_msl;
    pipe.vertex_entry_point = "ff_vertex";
    pipe.fragment_entry_point = fragment_entry.empty() ? "ff_fragment" : fragment_entry;
    // The generator's attribute order: position, blend weights, normal,
    // diffuse, texcoords (generate_vertex_shader, FFVertexIn).
    unsigned location = 0;
    pipe.vertex_attributes.push_back({location++, offsetof(Vtx, pos), BackendVertexFormat::Float3});
    if ((state.fvf_flags & fvf::POSITION_MASK) == fvf::XYZB1) {
        pipe.vertex_attributes.push_back({location++, offsetof(Vtx, weight), BackendVertexFormat::Float1});
    }
    if ((state.fvf_flags & fvf::POSITION_MASK) == fvf::XYZB3 && (state.fvf_flags & fvf::LASTBETA_UBYTE4) != 0) {
        // Two weights and the UBYTE4 indices in the third beta slot.
        pipe.vertex_attributes.push_back({location++, offsetof(Vtx, weight), BackendVertexFormat::Float2});
        pipe.vertex_attributes.push_back({location++, offsetof(Vtx, indices), BackendVertexFormat::UByte4});
    }
    if ((state.fvf_flags & fvf::NORMAL) != 0) {
        pipe.vertex_attributes.push_back({location++, offsetof(Vtx, normal), BackendVertexFormat::Float3});
    }
    if ((state.fvf_flags & fvf::DIFFUSE) != 0) {
        pipe.vertex_attributes.push_back({location++, offsetof(Vtx, color), BackendVertexFormat::Float4});
    }
    if ((state.fvf_flags & fvf::TEXCOUNT_MASK) != 0) {
        pipe.vertex_attributes.push_back({location++, offsetof(Vtx, tex), BackendVertexFormat::Float4});
    }
    pipe.vertex_buffer_strides[0] = sizeof(Vtx);
    pipe.vertex_stride = sizeof(Vtx);
    pipe.color_format = PixelFormat::RGBA32Float;
    pipe.color_formats[0] = PixelFormat::RGBA32Float;
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
    rt.width = kW;
    rt.height = kH;
    rt.mip_levels = 1;
    rt.array_size = 1;
    rt.pixel_format = PixelFormat::RGBA32Float;
    rt.render_target = true;
    rt.shader_resource = true;
    auto color = device.create_texture_2d(rt, nullptr, 0);
    EXPECT_TRUE(vb && color && color->valid());
    if (!vb || !color) return {};
    EXPECT_TRUE(color->clear_render_target({0, 0, 0, 0}));

    std::unique_ptr<BackendTexture2D> tex;
    if (texture != nullptr) {
        BackendTexture2DDesc td{};
        td.width = texture->width;
        td.height = texture->height;
        td.mip_levels = 1;
        td.array_size = 1;
        td.pixel_format = PixelFormat::RGBA32Float;
        td.shader_resource = true;
        tex = device.create_texture_2d(td, texture->rgba.data(), texture->width * 16);
        EXPECT_TRUE(tex && tex->valid());
        if (!tex) return {};
    }

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
    call.viewport = {0.0f, 0.0f, static_cast<float>(kW), static_cast<float>(kH), 0.0f, 1.0f};
    call.vertex_constants = vs.data();
    call.vertex_constants_size = vs.size();
    call.fragment_constants = fs.data();
    call.fragment_constants_size = fs.size();
    if (tex) {
        call.textures[0] = tex.get();
        BackendSamplerDesc sampler{};
        sampler.min_filter = sampler.mag_filter = BackendFilter::Nearest;
        sampler.mipmaps_enabled = false;
        sampler.address_u = sampler.address_v = sampler.address_w = BackendAddressMode::Clamp;
        call.samplers[0] = sampler;
    }
    EXPECT_TRUE(pass->draw(call));
    EXPECT_TRUE(pass->end());
    std::vector<u8> bytes(static_cast<usize>(kW) * kH * 16, 0);
    EXPECT_TRUE(color->readback(0, bytes.data(), kW * 16));
    std::vector<std::array<float, 4>> out(kW * kH);
    std::memcpy(out.data(), bytes.data(), bytes.size());
    return out;
}

std::array<float, 4> at(const std::vector<std::array<float, 4>>& px, u32 x, u32 y) { return px.at(y * kW + x); }

/// Signed distance of (x, y) from the line a->b, positive on its left.
double edge_distance(const std::array<double, 2>& a, const std::array<double, 2>& b, double x, double y) {
    const double dx = b[0] - a[0], dy = b[1] - a[1];
    return (dx * (y - a[1]) - dy * (x - a[0])) / std::sqrt(dx * dx + dy * dy);
}

/// Checks that exactly the pixel centres inside the screen triangle @p tri
/// are green (centres within 0.01 px of an edge are not decided).
void expect_coverage(const std::vector<std::array<float, 4>>& px, const std::array<std::array<double, 2>, 3>& tri,
                     const char* what) {
    ASSERT_FALSE(px.empty());
    u32 inside_count = 0, outside_count = 0;
    for (u32 y = 0; y < kH; ++y) {
        for (u32 x = 0; x < kW; ++x) {
            const double cx = x + 0.5, cy = y + 0.5;
            const double e0 = edge_distance(tri[0], tri[1], cx, cy), e1 = edge_distance(tri[1], tri[2], cx, cy),
                         e2 = edge_distance(tri[2], tri[0], cx, cy);
            if (std::min({std::abs(e0), std::abs(e1), std::abs(e2)}) < 0.01) continue;
            const bool inside = (e0 > 0 && e1 > 0 && e2 > 0) || (e0 < 0 && e1 < 0 && e2 < 0);
            (inside ? inside_count : outside_count)++;
            EXPECT_EQ(at(px, x, y)[1], inside ? 1.0f : 0.0f) << what << ": pixel " << x << "," << y;
        }
    }
    // The geometry is a real test: a good part of the target on both sides.
    EXPECT_GT(inside_count, 60u) << what;
    EXPECT_GT(outside_count, 60u) << what;
}

/// The pixel a size-1 point at @p clip covers; asserts the centre is not on
/// a pixel boundary.
std::array<u32, 2> point_pixel(const V4& clip) {
    const auto s = screen(clip);
    for (double c : s) {
        const double f = c - std::floor(c);
        EXPECT_TRUE(f > 0.02 && f < 0.98) << "point centre on a pixel boundary: " << c;
    }
    EXPECT_TRUE(s[0] >= 0 && s[0] < kW && s[1] >= 0 && s[1] < kH);
    return {static_cast<u32>(s[0]), static_cast<u32>(s[1])};
}

/// A fragment stage that returns one varying of the generated vertex stage.
std::string varying_fragment(const std::string& decl, const std::string& expr) {
    return "#include <metal_stdlib>\nusing namespace metal;\n"
           "struct VaryingIn { float4 position [[position]]; " +
           decl + " };\nfragment float4 varying_out(VaryingIn in [[stage_in]]) { return " + expr + "; }\n";
}

class FFEmulationD3DMetal : public ::testing::Test {
protected:
    void SetUp() override {
        if (!MetalDevice::instance().available()) GTEST_SKIP() << "no Metal device";
    }
};

} // namespace

// v' = v * World * View * Projection ("Transforms (Direct3D 9)"), for the
// single world matrix and for a one-weight vertex blend ("Geometry Blending
// (Direct3D 9)": v_world = w0 * v * World0 + (1 - w0) * v * World1).
TEST_F(FFEmulationD3DMetal, D3D9TransformedVertexLandsWherePerspectiveMathSays) {
    const Scene s = scene();
    const std::array<D3, 3> obj{{{-1.6, -1.2, 0.0}, {1.7, -0.9, 0.4}, {-0.1, 1.8, -0.3}}};
    {
        std::array<std::array<double, 2>, 3> tri{};
        std::vector<Vtx> v;
        for (int i = 0; i < 3; ++i) {
            tri[i] = screen(xf({obj[i][0], obj[i][1], obj[i][2], 1.0}, s.wvp()));
            v.push_back(vtx(obj[i]));
        }
        const auto px = render(d3d_state(fvf::XYZ | fvf::DIFFUSE), inputs_for(s), v,
                               BackendPrimitiveTopology::TriangleList);
        expect_coverage(px, tri, "single world matrix");
    }
    {
        const M4 world1 = mul(rotation_y(-0.4), translate(-0.4, 0.3, 0.2));
        const std::array<double, 3> weight{0.3, 0.8, 0.55};
        std::array<std::array<double, 2>, 3> tri{};
        std::vector<Vtx> v;
        for (int i = 0; i < 3; ++i) {
            const V4 p{obj[i][0], obj[i][1], obj[i][2], 1.0};
            const V4 w0 = xf(p, s.world), w1 = xf(p, world1);
            V4 world{};
            for (int k = 0; k < 4; ++k) world[k] = weight[i] * w0[k] + (1 - weight[i]) * w1[k];
            tri[i] = screen(xf(xf(world, s.view), s.proj));
            v.push_back(vtx(obj[i], {0, 0, 1}, {0, 0, 0, 1}, static_cast<float>(weight[i])));
        }
        FFPipelineState st = d3d_state(fvf::XYZB1 | fvf::DIFFUSE);
        st.vertex_blend_weight_count = 1;
        FFUniformInputs in = inputs_for(s);
        in.world_matrices[1] = ff(world1);
        const auto px = render(st, in, v, BackendPrimitiveTopology::TriangleList);
        expect_coverage(px, tri, "one-weight vertex blend");
    }
}

// "Camera Space Transformations (Direct3D 9)": N = norm(N * (wv^-1)^T) on
// the 3x3 part (wv need not be orthogonal), the directional light's
// direction to the light is -norm(Ldir * vMatrix) (D3DLIGHT9.Direction is a
// world-space direction), diffuse = Md * Ld * max(N . Ldir, 0) ("Diffuse
// Lighting (Direct3D 9)"). A non-uniform world scale separates the inverse
// transpose from the matrix itself; a scaled view separates camera-space
// lighting from world-space lighting.
TEST_F(FFEmulationD3DMetal, D3D9LightingUsesInverseTransposeNormals) {
    const D3 n_obj = unit({0.4, 0.7, -0.6});
    const D3 light_dir{0.3, -0.5, 0.8};
    const float light_diffuse[4] = {1.0f, 0.8f, 0.6f, 1.0f};
    struct Case {
        const char* what;
        M4 world, view;
        bool blend;
    };
    const Scene base = scene();
    const M4 world = mul(mul(scale(2.5, 0.4, 1.0), rotation_y(0.7)), translate(0.2, 0.0, 0.3));
    const std::vector<Case> cases{
        {"rigid view", world, base.view, false},
        {"scaled view", world, mul(base.view, scale(1.0, 1.6, 0.7)), false},
        {"one-weight blend", world, base.view, true},
    };
    const M4 world1 = mul(scale(0.5, 1.5, 1.0), rotation_x(0.5));
    const double w0 = 0.35;
    for (const Case& c : cases) {
        Scene s = base;
        s.world = c.world;
        s.view = c.view;
        // Camera-space normal and light direction, per the Learn formulas.
        auto camera_normal = [&](const M4& w) {
            const M4 it = transpose(inverse(mul(w, s.view)));
            return D3{n_obj[0] * it[0] + n_obj[1] * it[4] + n_obj[2] * it[8],
                      n_obj[0] * it[1] + n_obj[1] * it[5] + n_obj[2] * it[9],
                      n_obj[0] * it[2] + n_obj[1] * it[6] + n_obj[2] * it[10]};
        };
        D3 n = camera_normal(s.world);
        if (c.blend) {
            const D3 n1 = camera_normal(world1);
            for (int k = 0; k < 3; ++k) n[k] = w0 * n[k] + (1 - w0) * n1[k];
        }
        n = unit(n);
        const V4 ld = xf({light_dir[0], light_dir[1], light_dir[2], 0.0}, s.view);
        const D3 to_light = unit({-ld[0], -ld[1], -ld[2]});
        const double ndotl = std::max(dot(n, to_light), 0.0);
        ASSERT_GT(ndotl, 0.1) << c.what;

        FFPipelineState st = d3d_state((c.blend ? fvf::XYZB1 : fvf::XYZ) | fvf::NORMAL);
        st.vertex_blend_weight_count = c.blend ? 1 : 0;
        st.lighting_enabled = true;
        st.normalize_normals = true;
        st.vertex_color_enable = false;
        st.material.ambient[0] = st.material.ambient[1] = st.material.ambient[2] = 0.0f;
        st.active_light_count = 1;
        st.lights[0].type = LightType::Directional;
        st.lights[0].enabled = true;
        std::copy(std::begin(light_diffuse), std::end(light_diffuse), st.lights[0].diffuse);
        for (int k = 0; k < 3; ++k) st.lights[0].direction[k] = static_cast<float>(light_dir[k]);
        FFUniformInputs in = inputs_for(s);
        in.world_matrices[1] = ff(world1);

        // A small triangle around the object origin; directional light is
        // the same at every vertex. Read its centroid's pixel.
        const std::array<D3, 3> obj{{{-0.3, -0.25, 0.0}, {0.3, -0.25, 0.0}, {0.0, 0.3, 0.0}}};
        std::vector<Vtx> v;
        V4 centroid{};
        for (const D3& p : obj) {
            const V4 ph{p[0], p[1], p[2], 1.0};
            V4 wp = xf(ph, s.world);
            if (c.blend) {
                const V4 wp1 = xf(ph, world1);
                for (int k = 0; k < 4; ++k) wp[k] = w0 * wp[k] + (1 - w0) * wp1[k];
            }
            const V4 clip = xf(xf(wp, s.view), s.proj);
            for (int k = 0; k < 4; ++k) centroid[k] += clip[k] / 3.0;
            v.push_back(vtx(p, n_obj, {0, 0, 0, 1}, static_cast<float>(w0)));
        }
        const auto pix = screen(centroid);
        const auto px = render(st, in, v, BackendPrimitiveTopology::TriangleList);
        ASSERT_FALSE(px.empty());
        const auto got = at(px, static_cast<u32>(pix[0]), static_cast<u32>(pix[1]));
        for (int k = 0; k < 3; ++k) {
            EXPECT_NEAR(got[k], ndotl * light_diffuse[k], 2e-3) << c.what << ", channel " << k;
        }
    }
}

// tc' = tc * D3DTS_TEXTURE0 ("Texture Coordinate Transformations (Direct3D
// 9)"); COUNTn passes n elements, PROJECTED divides by the last of them
// (D3DTEXTURETRANSFORMFLAGS). Every vertex carries the same 4D texcoord, so
// the whole triangle samples one texel of an 8x8 texture whose texel (i, j)
// is ((i + 0.5) / 8, (j + 0.5) / 8, 0.25, 1).
TEST_F(FFEmulationD3DMetal, D3D9TextureTransformCount2AndProjected) {
    Texture t;
    t.width = t.height = 8;
    for (u32 j = 0; j < 8; ++j)
        for (u32 i = 0; i < 8; ++i)
            t.rgba.insert(t.rgba.end(), {(i + 0.5f) / 8, (j + 0.5f) / 8, 0.25f, 1.0f});
    // A rotation, a non-uniform scale and a translation in row 4.
    const double c = std::cos(0.5), sn = std::sin(0.5);
    const M4 tm{0.8 * c, 0.6 * sn, 0.1, 0.05, -0.8 * sn, 0.6 * c, 0.2, 0.1, 0.15, -0.1, 0.9, 0.3, 0.3, 0.25, 0.4, 0.9};
    const std::array<float, 4> tc{0.15f, 0.5f, 0.6f, 1.3f};
    const V4 r = xf({tc[0], tc[1], tc[2], tc[3]}, tm);
    struct Case {
        const char* what;
        u32 flags;
        double u, v;
    };
    const std::vector<Case> cases{
        {"COUNT2", 2u, r[0], r[1]},
        {"COUNT3|PROJECTED", 3u | 0x100u, r[0] / r[2], r[1] / r[2]},
        {"COUNT4|PROJECTED", 4u | 0x100u, r[0] / r[3], r[1] / r[3]},
        {"COUNT2|PROJECTED", 2u | 0x100u, r[0] / r[1], r[1] / r[1]},
    };
    const Scene s = scene();
    const std::array<D3, 3> obj{{{-1.6, -1.2, 0.0}, {1.7, -0.9, 0.4}, {-0.1, 1.8, -0.3}}};
    V4 centroid{};
    std::vector<Vtx> v;
    for (const D3& p : obj) {
        const V4 clip = xf({p[0], p[1], p[2], 1.0}, s.wvp());
        for (int k = 0; k < 4; ++k) centroid[k] += clip[k] / 3.0;
        v.push_back(vtx(p, {0, 0, 1}, tc));
    }
    const auto pix = screen(centroid);
    for (const Case& cs : cases) {
        // The expected coordinate is inside the texture and off a texel edge
        // (v = 1 for COUNT2|PROJECTED clamps to the last row).
        const double tu = std::min(cs.u * 8, 7.5), tv = std::min(cs.v * 8, 7.5);
        ASSERT_TRUE(tu > 0 && tv > 0) << cs.what;
        ASSERT_GT(std::abs(tu - std::round(tu)), 0.05) << cs.what;
        ASSERT_TRUE(cs.v == 1.0 || std::abs(tv - std::round(tv)) > 0.05) << cs.what;
        FFPipelineState st = d3d_state(fvf::XYZ | fvf::TEX1 | kTexcoordSize4);
        st.active_texture_stages = 1;
        st.texture_stages[0].color_op = TexStageOp::SelectArg1;
        st.texture_stages[0].color_arg1 = 2;   // D3DTA_TEXTURE
        st.texture_stages[0].alpha_op = TexStageOp::SelectArg1;
        st.texture_stages[0].alpha_arg1 = 2;
        st.texture_stages[0].texture_transform_flags = cs.flags;
        FFUniformInputs in = inputs_for(s);
        in.texture_transforms[0] = ff(tm);
        const auto px = render(st, in, v, BackendPrimitiveTopology::TriangleList, {}, {}, &t);
        ASSERT_FALSE(px.empty());
        const auto got = at(px, static_cast<u32>(pix[0]), static_cast<u32>(pix[1]));
        const u32 ti = static_cast<u32>(tu), tj = static_cast<u32>(tv);
        EXPECT_FLOAT_EQ(got[0], (ti + 0.5f) / 8) << cs.what << " u=" << cs.u;
        EXPECT_FLOAT_EQ(got[1], (tj + 0.5f) / 8) << cs.what << " v=" << cs.v;
    }
}

// D3DTSS_TCI_CAMERASPACEPOSITION: the position transformed to camera space
// (V * wv); D3DTSS_TCI_CAMERASPACENORMAL: the normal transformed to camera
// space (N * (wv^-1)^T, normalised only with NORMALIZENORMALS). "Camera Space
// Transformations (Direct3D 9)", D3DTSS_TCI.
TEST_F(FFEmulationD3DMetal, D3D9CameraSpaceTexgen) {
    Scene s = scene();
    s.world = mul(mul(scale(1.8, 0.6, 1.2), rotation_y(0.4)), translate(0.1, 0.2, -0.3));
    const std::vector<D3> obj{{-1.2, -0.8, 0.0}, {1.1, 0.9, 0.2}, {0.4, -1.3, -0.4}, {-0.9, 1.2, 0.5}};
    const std::vector<D3> nrm{{0.2, 0.9, 0.1}, {-0.7, 0.3, 0.4}, {0.5, -0.5, 0.6}, {0.1, 0.1, -1.0}};
    std::vector<Vtx> v;
    for (size_t i = 0; i < obj.size(); ++i) v.push_back(vtx(obj[i], nrm[i], {9, 9, 9, 9}));
    const std::string fragment = varying_fragment("float4 texcoord0;", "in.texcoord0");
    const M4 it = transpose(inverse(s.wv()));

    struct Case {
        const char* what;
        u32 tci;
        bool normalize;
    };
    for (const Case& c : {Case{"CAMERASPACEPOSITION", 0x20000u, false}, Case{"CAMERASPACENORMAL", 0x10000u, false},
                          Case{"CAMERASPACENORMAL + NORMALIZENORMALS", 0x10000u, true}}) {
        FFPipelineState st = d3d_state(fvf::XYZ | fvf::NORMAL | fvf::TEX1 | kTexcoordSize4);
        st.point_size_output = true;
        st.normalize_normals = c.normalize;
        st.active_texture_stages = 1;
        st.texture_stages[0].color_op = TexStageOp::SelectArg1;
        st.texture_stages[0].texcoord_index = c.tci | 0u;
        const auto px = render(st, inputs_for(s), v, BackendPrimitiveTopology::PointList, fragment, "varying_out");
        ASSERT_FALSE(px.empty());
        for (size_t i = 0; i < obj.size(); ++i) {
            const auto p = point_pixel(xf({obj[i][0], obj[i][1], obj[i][2], 1.0}, s.wvp()));
            const auto got = at(px, p[0], p[1]);
            std::array<double, 4> want{};
            if (c.tci == 0x20000u) {
                const V4 e = xf({obj[i][0], obj[i][1], obj[i][2], 1.0}, s.wv());
                want = {e[0], e[1], e[2], 1.0};
            } else {
                D3 n{};
                for (int j = 0; j < 3; ++j)
                    for (int k = 0; k < 3; ++k) n[j] += nrm[i][k] * it[k * 4 + j];
                if (c.normalize) n = unit(n);
                want = {n[0], n[1], n[2], 0.0};
            }
            for (int k = 0; k < 4; ++k) {
                EXPECT_NEAR(got[k], want[k], 1e-4 * std::max(1.0, std::abs(want[k])))
                    << c.what << ", vertex " << i << ", component " << k;
            }
        }
    }
}

// Linear vertex fog ("Fog Formulas (Direct3D 9)"): f = (end - d) / (end -
// start) with d the camera-space z, or the camera-space distance when
// D3DRS_RANGEFOGENABLE is set. Point scale (D3DRS_POINTSCALE_A): Ss = Vh * Si
// * sqrt(1 / (A + B De + C De^2)), De the camera-space distance; with A = B =
// 0 and C = 1, Si = 6 De / Vh gives a 6-pixel point (36 pixels).
TEST_F(FFEmulationD3DMetal, D3D9FogAndPointScaleUseCameraSpaceDistance) {
    Scene s = scene();
    s.world = mul(mul(scale(1.5, 0.8, 1.3), rotation_y(-0.5)), translate(0.5, -0.35, 1.0));
    const std::vector<D3> obj{{-1.2, -0.8, 0.0}, {1.1, 0.9, 0.2}, {0.4, -1.3, -0.4}, {-0.9, 1.2, 0.5}};
    std::vector<Vtx> v;
    for (const D3& p : obj) v.push_back(vtx(p));
    const std::string fragment = varying_fragment("float fog_factor;", "float4(in.fog_factor, 0.0, 0.0, 1.0)");
    for (bool range : {false, true}) {
        FFPipelineState st = d3d_state(fvf::XYZ | fvf::DIFFUSE);
        st.point_size_output = true;
        st.fog_mode = FogMode::Linear;
        st.range_fog_enable = range;
        st.fog_start = 3.0f;
        st.fog_end = 12.0f;
        const auto px = render(st, inputs_for(s), v, BackendPrimitiveTopology::PointList, fragment, "varying_out");
        ASSERT_FALSE(px.empty());
        for (size_t i = 0; i < obj.size(); ++i) {
            const V4 e = xf({obj[i][0], obj[i][1], obj[i][2], 1.0}, s.wv());
            const double d = range ? std::sqrt(e[0] * e[0] + e[1] * e[1] + e[2] * e[2]) : e[2];
            const double f = (12.0 - d) / (12.0 - 3.0);
            ASSERT_TRUE(f > 0.05 && f < 0.95) << d;
            const auto p = point_pixel(xf({obj[i][0], obj[i][1], obj[i][2], 1.0}, s.wvp()));
            EXPECT_NEAR(at(px, p[0], p[1])[0], f, 1e-4) << (range ? "range" : "z") << " fog, vertex " << i;
        }
    }
    for (size_t i = 0; i < obj.size(); ++i) {
        const V4 e = xf({obj[i][0], obj[i][1], obj[i][2], 1.0}, s.wv());
        const double de = std::sqrt(e[0] * e[0] + e[1] * e[1] + e[2] * e[2]);
        FFPipelineState st = d3d_state(fvf::XYZ | fvf::DIFFUSE);
        st.point_size_output = true;
        st.point_scale_enabled = true;
        FFUniformInputs in = inputs_for(s);
        in.point_size = static_cast<float>(6.0 * de / kH);
        in.point_size_min = 0.0f;
        in.point_scale_a = 0.0f;
        in.point_scale_b = 0.0f;
        in.point_scale_c = 1.0f;
        const auto px = render(st, in, {v[i]}, BackendPrimitiveTopology::PointList);
        ASSERT_FALSE(px.empty());
        u32 lit = 0;
        for (const auto& texel : px) lit += texel[1] == 1.0f ? 1 : 0;
        EXPECT_EQ(lit, 36u) << "point " << i << " at camera distance " << de;
    }
}

namespace {

/// A lit-points state: D3D9 lighting with one light, material diffuse and
/// specular white, no ambient or emissive, NORMALIZENORMALS on.
FFPipelineState lit_points_state(u32 fvf_flags, const FFLight& light) {
    FFPipelineState st = d3d_state(fvf_flags);
    st.point_size_output = true;
    st.lighting_enabled = true;
    st.normalize_normals = true;
    st.vertex_color_enable = false;
    st.material.ambient[0] = st.material.ambient[1] = st.material.ambient[2] = 0.0f;
    st.material.specular[0] = st.material.specular[1] = st.material.specular[2] = 1.0f;
    st.active_light_count = 1;
    st.lights[0] = light;
    st.lights[0].enabled = true;
    return st;
}

D3 mul3(const D3& v, const M4& m, double w) {
    const V4 r = xf({v[0], v[1], v[2], w}, m);
    return {r[0], r[1], r[2]};
}
/// The camera-space normal norm(N * (wv^-1)^T), 3x3 part.
D3 camera_normal(const D3& n, const M4& wv) {
    const M4 it = transpose(inverse(wv));
    D3 r{};
    for (int j = 0; j < 3; ++j)
        for (int k = 0; k < 3; ++k) r[j] += n[k] * it[k * 4 + j];
    return unit(r);
}

} // namespace

// Point and spot lights under a non-identity view. "Camera Space
// Transformations (Direct3D 9)": V = V * wv, N = norm(N * (wv^-1)^T), light
// position Lp * vMatrix; D3DLIGHT9 Direction is a world-space direction, so it
// moves by the view as a w = 0 vector. "Diffuse Lighting (Direct3D 9)":
// Cd * Ld * (N . Ldir) * Atten * Spot, Ldir = norm(Lp - V). "Attenuation and
// Spotlight Factor (Direct3D 9)": Atten = 1 / (att0 + att1 d + att2 d^2),
// d = |Lp - V|, and 0 beyond Range; rho = norm(-Ldir_light) . norm(Ldir).
// "Light Types (Direct3D 9)": Theta and Phi are full cone angles, halved
// before the cosines; rho <= cos(Phi/2) is unlit, rho > cos(Theta/2) is fully
// lit, and between them the falloff ((rho - cos(Phi/2)) / (cos(Theta/2) -
// cos(Phi/2)))^Falloff.
TEST_F(FFEmulationD3DMetal, D3D9PointSpotLightCameraSpace) {
    Scene s = scene();
    s.world = mul(mul(scale(1.3, 0.9, 1.1), rotation_y(0.35)), translate(0.2, -0.1, 0.4));
    const M4 wv = s.wv();
    const std::vector<D3> obj{{-1.4, -0.9, 0.1}, {1.2, 0.8, -0.2}, {0.3, -1.2, 0.3}, {-0.8, 1.1, -0.3},
                              {0.9, -0.3, 0.2},  {-0.2, 0.4, -0.1}, {1.4, -1.1, 0.0}, {-1.1, 0.2, 0.4}};
    const D3 n_obj = unit({0.1, 0.3, -1.0});
    std::vector<Vtx> v;
    for (const D3& p : obj) v.push_back(vtx(p, n_obj));
    const std::string fragment = varying_fragment("float4 diffuse;", "in.diffuse");
    const D3 light_world{0.6, 1.4, -3.0};
    const D3 spot_dir_world = unit({-0.15, -0.35, 1.0});
    const float ld[3] = {1.0f, 0.8f, 0.6f};
    const double att[3] = {0.9, 0.12, 0.015};

    // Camera-space vertex, normal, light position and direction (Learn).
    std::vector<D3> vc, nc;
    for (const D3& p : obj) {
        vc.push_back(mul3(p, wv, 1.0));
        nc.push_back(camera_normal(n_obj, wv));
    }
    const D3 lp = mul3(light_world, s.view, 1.0);
    const D3 ldcs = unit(mul3(spot_dir_world, s.view, 0.0));
    auto base = [&](size_t i, double& d, D3& to_light) {
        const D3 l = sub(lp, vc[i]);
        d = std::sqrt(dot(l, l));
        to_light = unit(l);
        const double ndotl = dot(nc[i], to_light);
        EXPECT_GT(ndotl, 0.1) << "vertex " << i << " faces away from the light";
        return std::max(ndotl, 0.0) / (att[0] + att[1] * d + att[2] * d * d);
    };
    auto check = [&](const FFPipelineState& st, const std::vector<double>& want, const char* what) {
        const auto px = render(st, inputs_for(s), v, BackendPrimitiveTopology::PointList, fragment, "varying_out");
        ASSERT_FALSE(px.empty());
        for (size_t i = 0; i < obj.size(); ++i) {
            const auto p = point_pixel(xf({obj[i][0], obj[i][1], obj[i][2], 1.0}, s.wvp()));
            const auto got = at(px, p[0], p[1]);
            ASSERT_LT(want[i] * ld[0], 0.98) << what << ": the expected value would clamp";
            for (int k = 0; k < 3; ++k) EXPECT_NEAR(got[k], want[i] * ld[k], 1e-3) << what << ", vertex " << i << ", channel " << k;
        }
    };

    FFLight point{};
    point.type = LightType::Point;
    for (int k = 0; k < 3; ++k) point.position[k] = static_cast<float>(light_world[k]);
    std::copy(std::begin(ld), std::end(ld), point.diffuse);
    point.diffuse[3] = 1.0f;
    for (int k = 0; k < 3; ++k) point.attenuation[k] = static_cast<float>(att[k]);
    std::vector<double> dist(obj.size());
    {
        std::vector<double> want;
        for (size_t i = 0; i < obj.size(); ++i) {
            D3 l;
            want.push_back(base(i, dist[i], l));
        }
        check(lit_points_state(fvf::XYZ | fvf::NORMAL, point), want, "point light");
    }
    {
        // Range between the 4th and 5th nearest vertex: the farther ones are
        // unlit.
        std::vector<double> sorted = dist;
        std::sort(sorted.begin(), sorted.end());
        const double range = (sorted[3] + sorted[4]) / 2;
        ASSERT_GT(sorted[4] - sorted[3], 0.05);
        FFLight ranged = point;
        ranged.range = static_cast<float>(range);
        std::vector<double> want;
        for (size_t i = 0; i < obj.size(); ++i) {
            double d;
            D3 l;
            const double value = base(i, d, l);
            want.push_back(d > range ? 0.0 : value);
        }
        check(lit_points_state(fvf::XYZ | fvf::NORMAL, ranged), want, "point light, Range");
    }
    {
        // Cone angles from the vertices' rho: two fully inside the inner
        // cone, two beyond the outer cone, the rest in the falloff band.
        std::vector<double> rho(obj.size());
        for (size_t i = 0; i < obj.size(); ++i) {
            double d;
            D3 l;
            (void)base(i, d, l);
            rho[i] = dot(D3{-ldcs[0], -ldcs[1], -ldcs[2]}, l);
        }
        std::vector<double> sorted = rho;
        std::sort(sorted.begin(), sorted.end());
        ASSERT_GT(sorted.front(), -0.5);
        const double cos_inner = (sorted[5] + sorted[6]) / 2, cos_outer = (sorted[1] + sorted[2]) / 2;
        ASSERT_GT(sorted[6] - sorted[5], 1e-3);
        ASSERT_GT(sorted[2] - sorted[1], 1e-3);
        const double falloff = 2.0;
        FFLight spot = point;
        spot.type = LightType::Spot;
        for (int k = 0; k < 3; ++k) spot.direction[k] = static_cast<float>(spot_dir_world[k]);
        spot.spot_theta = static_cast<float>(2.0 * std::acos(cos_inner));
        spot.spot_phi = static_cast<float>(2.0 * std::acos(cos_outer));
        spot.spot_falloff = static_cast<float>(falloff);
        std::vector<double> want;
        u32 inner = 0, outer = 0, band = 0;
        for (size_t i = 0; i < obj.size(); ++i) {
            double d;
            D3 l;
            const double value = base(i, d, l);
            double factor;
            if (rho[i] > cos_inner) {
                factor = 1.0;
                ++inner;
            } else if (rho[i] <= cos_outer) {
                factor = 0.0;
                ++outer;
            } else {
                factor = std::pow((rho[i] - cos_outer) / (cos_inner - cos_outer), falloff);
                ++band;
            }
            want.push_back(value * factor);
        }
        ASSERT_EQ(inner, 2u);
        ASSERT_EQ(outer, 2u);
        ASSERT_EQ(band, 4u);
        check(lit_points_state(fvf::XYZ | fvf::NORMAL, spot), want, "spot light");
    }
}

// "Specular Lighting (Direct3D 9)": Cs * Ls * (N . H)^P * Atten * Spot, in
// camera space. D3DRS_LOCALVIEWER TRUE: H = norm(norm(Cp - Vp) + Ldir), the
// camera at the camera-space origin. FALSE: H = norm((0,0,1) + Ldir).
TEST_F(FFEmulationD3DMetal, D3D9SpecularHalfVectorLocalViewer) {
    Scene s = scene();
    const M4 wv = s.wv();
    const std::vector<D3> obj{{-1.27, -0.83, 0.0}, {1.13, 0.87, 0.2}, {0.43, -1.17, -0.3}, {-0.87, 1.13, 0.4}};
    const std::vector<D3> nrm{{0.2, 0.1, -1.0}, {-0.3, 0.2, -1.0}, {0.1, -0.4, -1.0}, {0.4, 0.3, -1.0}};
    std::vector<Vtx> v;
    for (size_t i = 0; i < obj.size(); ++i) v.push_back(vtx(obj[i], nrm[i]));
    const std::string fragment = varying_fragment("float4 specular;", "in.specular");
    const D3 light_dir_world = unit({0.3, -0.4, 1.0});
    const float ls[3] = {0.9f, 0.7f, 0.5f};
    const double power = 6.0;
    const D3 to_light = unit(mul3({-light_dir_world[0], -light_dir_world[1], -light_dir_world[2]}, s.view, 0.0));

    double max_gap = 0;
    for (bool local : {true, false}) {
        FFLight light{};
        light.type = LightType::Directional;
        for (int k = 0; k < 3; ++k) light.direction[k] = static_cast<float>(light_dir_world[k]);
        light.diffuse[0] = light.diffuse[1] = light.diffuse[2] = 0.0f;
        std::copy(std::begin(ls), std::end(ls), light.specular);
        FFPipelineState st = lit_points_state(fvf::XYZ | fvf::NORMAL, light);
        st.specular_enable = true;
        st.local_viewer = local;
        st.specular_material_source = 0;   // D3DMCS_MATERIAL
        st.material.power = static_cast<float>(power);
        const auto px = render(st, inputs_for(s), v, BackendPrimitiveTopology::PointList, fragment, "varying_out");
        ASSERT_FALSE(px.empty());
        for (size_t i = 0; i < obj.size(); ++i) {
            const D3 vcam = mul3(obj[i], wv, 1.0);
            const D3 n = camera_normal(nrm[i], wv);
            ASSERT_GT(dot(n, to_light), 0.05) << i;
            const D3 to_eye = unit({-vcam[0], -vcam[1], -vcam[2]});
            const D3 h_local = unit({to_eye[0] + to_light[0], to_eye[1] + to_light[1], to_eye[2] + to_light[2]});
            // Learn-literal (0,0,1); winoracle confirmation pending.
            const D3 h_far = unit({to_light[0], to_light[1], to_light[2] + 1.0});
            const double nh_local = std::max(dot(n, h_local), 0.0), nh_far = std::max(dot(n, h_far), 0.0);
            max_gap = std::max(max_gap, std::abs(std::pow(nh_local, power) - std::pow(nh_far, power)));
            const double want = std::pow(local ? nh_local : nh_far, power);
            ASSERT_LT(want * ls[0], 0.98);
            const auto p = point_pixel(xf({obj[i][0], obj[i][1], obj[i][2], 1.0}, s.wvp()));
            const auto got = at(px, p[0], p[1]);
            for (int k = 0; k < 3; ++k)
                EXPECT_NEAR(got[k], want * ls[k], 1e-3) << (local ? "LOCALVIEWER" : "non-local") << ", vertex " << i
                                                        << ", channel " << k;
        }
    }
    // The two half vectors give visibly different highlights here.
    EXPECT_GT(max_gap, 0.05);
}

// Indexed vertex blending with a lit normal ("Indexed Vertex Blending
// (Direct3D 9)", "Geometry Blending (Direct3D 9)": normals "transformed by a
// corresponding inverse world-view matrix, weighted in the same way as the
// vertex position"): two weights and D3DFVF_LASTBETA_UBYTE4 indices, so
// blend_world_inv_transposes[idx] = (W_idx V)^-1 is read three times.
TEST_F(FFEmulationD3DMetal, D3D9IndexedBlendLightsWithWorldViewInverseTranspose) {
    Scene s = scene();
    s.view = mul(s.view, scale(1.0, 1.3, 0.8));
    std::array<M4, FF_MAX_WORLD_MATRICES> palette{};
    FFUniformInputs in = inputs_for(s);
    for (u32 m = 0; m < FF_MAX_WORLD_MATRICES; ++m) {
        palette[m] = mul(mul(scale(1.0 + 0.2 * m, 1.0 - 0.07 * m, 1.0 + 0.05 * m), rotation_y(0.15 * m)),
                         translate(0.05 * m, -0.04 * m, 0.1));
        in.world_matrices[m] = ff(palette[m]);
    }
    const std::vector<D3> obj{{-1.2, -0.8, 0.0}, {1.1, 0.9, 0.2}, {0.4, -1.2, -0.3}, {-0.9, 1.1, 0.4}};
    const std::array<std::array<u8, 3>, 4> idx{{{3, 1, 5}, {0, 7, 2}, {8, 4, 6}, {2, 5, 1}}};
    const std::array<std::array<double, 2>, 4> wts{{{0.5, 0.3}, {0.2, 0.6}, {0.45, 0.15}, {0.3, 0.3}}};
    const D3 n_obj = unit({0.2, 0.3, -1.0});
    const D3 light_dir_world = unit({0.2, -0.3, 1.0});
    const float ld[3] = {1.0f, 0.8f, 0.6f};
    const D3 to_light = unit(mul3({-light_dir_world[0], -light_dir_world[1], -light_dir_world[2]}, s.view, 0.0));

    std::vector<Vtx> v;
    std::vector<V4> clip;
    std::vector<double> want;
    for (size_t i = 0; i < obj.size(); ++i) {
        Vtx x = vtx(obj[i], n_obj);
        x.weight[0] = static_cast<float>(wts[i][0]);
        x.weight[1] = static_cast<float>(wts[i][1]);
        for (int k = 0; k < 3; ++k) x.indices[k] = idx[i][k];
        v.push_back(x);
        const double w[3] = {wts[i][0], wts[i][1], 1.0 - wts[i][0] - wts[i][1]};
        V4 world{};
        D3 n{};
        for (int k = 0; k < 3; ++k) {
            const M4& m = palette[idx[i][k]];
            const V4 p = xf({obj[i][0], obj[i][1], obj[i][2], 1.0}, m);
            const M4 it = transpose(inverse(mul(m, s.view)));
            for (int c = 0; c < 4; ++c) world[c] += w[k] * p[c];
            for (int j = 0; j < 3; ++j)
                for (int r = 0; r < 3; ++r) n[j] += w[k] * n_obj[r] * it[r * 4 + j];
        }
        clip.push_back(xf(xf(world, s.view), s.proj));
        want.push_back(std::max(dot(unit(n), to_light), 0.0));
        ASSERT_GT(want.back(), 0.1);
    }
    FFLight light{};
    light.type = LightType::Directional;
    for (int k = 0; k < 3; ++k) light.direction[k] = static_cast<float>(light_dir_world[k]);
    std::copy(std::begin(ld), std::end(ld), light.diffuse);
    FFPipelineState st = lit_points_state(fvf::XYZB3 | fvf::LASTBETA_UBYTE4 | fvf::NORMAL, light);
    st.vertex_blend_weight_count = 2;
    st.indexed_vertex_blend = true;
    const auto px = render(st, in, v, BackendPrimitiveTopology::PointList,
                           varying_fragment("float4 diffuse;", "in.diffuse"), "varying_out");
    ASSERT_FALSE(px.empty());
    for (size_t i = 0; i < obj.size(); ++i) {
        const auto p = point_pixel(clip[i]);
        const auto got = at(px, p[0], p[1]);
        for (int k = 0; k < 3; ++k) EXPECT_NEAR(got[k], want[i] * ld[k], 1e-3) << "vertex " << i << ", channel " << k;
    }
}
