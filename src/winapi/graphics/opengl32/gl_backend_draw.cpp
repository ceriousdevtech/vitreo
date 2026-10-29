// SPDX-License-Identifier: LGPL-2.1-only
/// @file gl_backend_draw.cpp
/// @brief GL state to FFPipelineState / uniforms / BackendPipelineDesc, the
///        rasterization of points, lines and polygon modes, and the
///        per-context batching of primitives into the drawable's open render
///        pass (gl_backend_draw.hpp, Design 1 and 6).

#include "winapi/graphics/opengl32/gl_backend_draw.hpp"

#include "graphics/contracts/backend_buffer.hpp"
#include "graphics/contracts/backend_device.hpp"
#include "graphics/contracts/backend_draw_call.hpp"
#include "graphics/contracts/backend_pipeline.hpp"
#include "graphics/shaders/ff_uniforms.hpp"
#include "winapi/graphics/opengl32/gl_commands.hpp"
#include "winapi/graphics/opengl32/gl_context.hpp"
#include "winapi/graphics/opengl32/gl_drawable.hpp"
#include "winapi/graphics/opengl32/gl_texture.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstddef>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

namespace vitreo::winapi::gl {

namespace {

namespace fvf = graphics::fvf;

/// A pending draw is recorded once it holds this many vertices, which bounds
/// the CPU copy and keeps one upload well inside the streaming buffer.
constexpr usize kMaxBatchVertices = 65536;
constexpr usize kInitialVertexBytes = 1u << 20;   // 13,107 vertices
constexpr usize kInitialIndexBytes = 1u << 18;    // 65,536 indices

/// GL's clip volume has -w <= z <= w (spec 2.11), Metal's 0 <= z <= w; the
/// window depth both then derive from the depth range is the same
/// (spec 2.10.1: zw = ((f - n) zd + (n + f)) / 2; Metal: n + (f - n) zd').
/// Applied after the projection: z' = (z + w) / 2. Column-major.
constexpr GlMatrix kGlToMetalClipZ{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0.5, 0, 0, 0, 0.5, 1};

/// The generator's matrix convention (ff_emulation.hpp, "Matrix
/// convention"): an FFMatrix is a Direct3D matrix applied as p * M, which
/// pack_ff_vertex_uniforms delivers to the shader as an MSL float4x4 equal
/// to M, and every generator site applies it in the row-vector form:
/// `in.position * cb.world_view_proj`, `in.position * cb.world`, `n *
/// cb.world_inv_transpose`, `tc * cb.texture_matrix`. A GL matrix G
/// (column vectors, clip = G * object) is stored column-major (m[c * 4 + r],
/// spec 2.10.2); read row-major that array IS transpose(G), the row-vector
/// form of G. So to_ff copies the array verbatim, and the generator's
/// row-major composition world * view * projection = MV^T * (Z P)^T =
/// (Z P MV)^T is exactly GL's clip = Z P MV object. With view = I the
/// packed normal matrix is the inverse of world * view = MV, so normals come
/// out as n * MV^-1 (spec 2.10.3).
///
/// Coupling: Direct3D shares the same row-vector form, so there is one
/// convention and no compensation here. If either the generator's form or
/// this verbatim copy changes alone, FFEmulationGl.
/// MatrixApplicationConventionIsPinned, GlDrawTest.
/// ModelviewTranslateLandsWhereTheSpecSays and GlDrawTest.
/// GlTransformsUnchangedByD3DConventionFix fail.
graphics::FFMatrix to_ff(const GlMatrix& m) {
    graphics::FFMatrix out{};
    for (usize i = 0; i < 16; ++i) out[i] = static_cast<float>(m[i]);
    return out;
}

u64 mix(u64 h, u64 v) {
    h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
    return h;
}

void copy4(float (&dst)[4], const GlVec4& src) {
    for (usize k = 0; k < 4; ++k) dst[k] = src[k];
}

/// One-time notes for GL state not applied to primitives.
void note_unmapped_state(const GlContext& ctx) {
    const GlState& s = ctx.state;
    struct Unmapped {
        GlCap cap;
        const char* name;
        const char* detail;
    };
    static constexpr const char* kAliased =
        "antialiasing (coverage, spec 3.10) is not implemented; primitives are rasterised aliased";
    static constexpr Unmapped kLater[] = {
        {GlCap::PointSmooth, "glEnable(GL_POINT_SMOOTH)", kAliased},
        {GlCap::LineSmooth, "glEnable(GL_LINE_SMOOTH)", kAliased},
        {GlCap::PolygonSmooth, "glEnable(GL_POLYGON_SMOOTH)", kAliased},
    };
    for (const Unmapped& u : kLater) {
        if (s.enabled(u.cap)) gl_log_unimplemented_once(u.name, u.detail);
    }
}

/// Spec 2.13.3 glColorMaterial: the material properties of one face that
/// follow the current colour (D3DMCS_COLOR1 = the vertex colour), the rest
/// from the material (0).
void color_material_sources(u32 parameter, u32& diffuse, u32& ambient, u32& specular, u32& emission) {
    diffuse = ambient = specular = emission = 0;
    switch (parameter) {
    case GL_EMISSION: emission = 1; break;
    case GL_AMBIENT: ambient = 1; break;
    case GL_DIFFUSE: diffuse = 1; break;
    case GL_SPECULAR: specular = 1; break;
    case GL_AMBIENT_AND_DIFFUSE: ambient = diffuse = 1; break;
    default: break;
    }
}

graphics::FFMaterial to_ff(const GlMaterial& m) {
    graphics::FFMaterial out{};
    for (usize k = 0; k < 4; ++k) {
        out.ambient[k] = m.ambient[k];
        out.diffuse[k] = m.diffuse[k];
        out.specular[k] = m.specular[k];
        out.emissive[k] = m.emission[k];
    }
    out.power = m.shininess;
    return out;
}

/// The polygon's facing from its clip coordinates: the sign of the
/// homogeneous determinant det(x, y, w) of each fan triangle is that of its
/// window area (eq. 2.6, y up) for the part in front of the eye (w > 0), so
/// the clipped polygon's facing is read before clipping. Positive: counter-
/// clockwise.
double polygon_area_sign(const GlWindowMap& map, std::span<const GlBatchVertex> v, std::span<const u32> polygon) {
    if (polygon.size() < 3) return 0.0;
    const auto c0 = map.to_clip(v[polygon[0]].pos);
    double sum = 0.0;
    for (usize i = 1; i + 1 < polygon.size(); ++i) {
        const auto c1 = map.to_clip(v[polygon[i]].pos);
        const auto c2 = map.to_clip(v[polygon[i + 1]].pos);
        sum += c0[0] * (c1[1] * c2[3] - c2[1] * c1[3]) - c0[1] * (c1[0] * c2[3] - c2[0] * c1[3]) +
               c0[3] * (c1[0] * c2[1] - c2[0] * c1[1]);
    }
    return sum;
}

/// Spec 3.5.5 eq. 3.6 for a polygon drawn as lines or points: the maximum
/// window-depth slope of the plane through its first three vertices, from
/// clip coordinates (z = a x + b y + c w in clip space is the plane z_d = a
/// x_d + b y_d + c, so dz_w/dx_w = (f - n) a / p_x).
double polygon_depth_slope(const GlWindowMap& map, std::span<const GlBatchVertex> v, std::span<const u32> polygon) {
    if (polygon.size() < 3) return 0.0;
    const auto det3 = [](double a0, double a1, double a2, double b0, double b1, double b2, double c0, double c1,
                         double c2) {
        return a0 * (b1 * c2 - b2 * c1) - a1 * (b0 * c2 - b2 * c0) + a2 * (b0 * c1 - b1 * c0);
    };
    // The plane through the fan triangle (v0, vi, vi+1) farthest from
    // degenerate (largest |det(x, y, w)|): a polygon whose first three
    // vertices are collinear still has its plane.
    const auto p = map.to_clip(v[polygon[0]].pos);
    std::array<double, 4> q{}, r{};
    double d = 0.0;
    for (usize i = 1; i + 1 < polygon.size(); ++i) {
        const auto qi = map.to_clip(v[polygon[i]].pos);
        const auto ri = map.to_clip(v[polygon[i + 1]].pos);
        const double di = det3(p[0], p[1], p[3], qi[0], qi[1], qi[3], ri[0], ri[1], ri[3]);
        if (std::abs(di) > std::abs(d)) {
            d = di;
            q = qi;
            r = ri;
        }
    }
    if (d == 0.0) return 0.0;
    const double a = det3(p[2], p[1], p[3], q[2], q[1], q[3], r[2], r[1], r[3]) / d;
    const double b = det3(p[0], p[2], p[3], q[0], q[2], q[3], r[0], r[2], r[3]) / d;
    const double range = (map.depth_far - map.depth_near) / 2.0;
    return std::max(std::abs(range * a / (map.width / 2.0)), std::abs(range * b / (map.height / 2.0)));
}

/// Spec 2.13.1, evaluated on the CPU in double precision for one vertex: the
/// colour c = e_cm + a_cm a_cs + sum_i att_i spot_i (a_cm a_cli + (n (.)
/// VP_i) d_cm d_cli + f_i (n (.) h_i)^s_rm s_cm s_cli), clamped to [0, 1]
/// (2.13.6), alpha d_cm's. The vertex and normal in eye coordinates (2.10.3:
/// n' = n M^-1, normalised under GL_NORMALIZE); lights as stored (eye
/// coordinates). @p back: the back colour of two-sided lighting (-n, the back
/// material). Colour material (2.13.3) takes the vertex colour for the
/// tracked properties of the face. (.) is max(d1 . d2, 0); f_i = 1 when
/// n . VP_i > 0; the spot term is (PV (.) s)^e when PV (.) s >= cos(cutoff),
/// 0 otherwise, 1 for cutoff 180; the viewer is (0, 0, 1) or, local, the
/// eye. The same equation the generator's gl_spot_model evaluates for
/// filled polygons (ff_emulation.hpp).
struct GlCpuLighting {
    const GlState& s;
    GlMatrix modelview;
    std::optional<GlMatrix> inverse;

    std::array<float, 4> lit(const GlBatchVertex& v, bool back) const {
        using V3 = std::array<double, 3>;
        const auto dot = [](const V3& a, const V3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; };
        const auto unit = [&](V3 a) {
            const double l = std::sqrt(dot(a, a));
            return l > 0.0 ? V3{a[0] / l, a[1] / l, a[2] / l} : a;
        };
        const GlMatrix& m = modelview;
        std::array<double, 4> e{};
        for (usize r = 0; r < 4; ++r) {
            e[r] = m[0 * 4 + r] * v.pos[0] + m[1 * 4 + r] * v.pos[1] + m[2 * 4 + r] * v.pos[2] + m[3 * 4 + r] * v.pos[3];
        }
        const V3 p{e[0] / e[3], e[1] / e[3], e[2] / e[3]};
        V3 n{v.normal[0], v.normal[1], v.normal[2]};
        if (inverse) {
            const GlMatrix& inv = *inverse;
            n = {n[0] * inv[0 * 4 + 0] + n[1] * inv[0 * 4 + 1] + n[2] * inv[0 * 4 + 2],
                 n[0] * inv[1 * 4 + 0] + n[1] * inv[1 * 4 + 1] + n[2] * inv[1 * 4 + 2],
                 n[0] * inv[2 * 4 + 0] + n[1] * inv[2 * 4 + 1] + n[2] * inv[2 * 4 + 2]};
        }
        if (s.enabled(GlCap::Normalize)) n = unit(n);
        if (back) n = {-n[0], -n[1], -n[2]};

        const GlLightingState& l = s.lighting;
        const GlMaterial& mat = l.material[back ? 1 : 0];
        std::array<double, 4> am{}, dm{}, sm{}, em{};
        for (usize k = 0; k < 4; ++k) {
            am[k] = mat.ambient[k];
            dm[k] = mat.diffuse[k];
            sm[k] = mat.specular[k];
            em[k] = mat.emission[k];
        }
        const u32 face = l.color_material_face;
        if (s.enabled(GlCap::ColorMaterial) &&
            (face == GL_FRONT_AND_BACK || face == (back ? GL_BACK : GL_FRONT))) {
            std::array<double, 4> c{};
            for (usize k = 0; k < 4; ++k) c[k] = v.color[k];
            switch (l.color_material_parameter) {
            case GL_EMISSION: em = c; break;
            case GL_AMBIENT: am = c; break;
            case GL_DIFFUSE: dm = c; break;
            case GL_SPECULAR: sm = c; break;
            case GL_AMBIENT_AND_DIFFUSE: am = dm = c; break;
            default: break;
            }
        }
        std::array<double, 3> c{};
        for (usize k = 0; k < 3; ++k) c[k] = em[k] + am[k] * l.model_ambient[k];
        const V3 viewer = l.model_local_viewer ? unit({-p[0], -p[1], -p[2]}) : V3{0, 0, 1};
        for (u32 i = 0; i < kMaxLights; ++i) {
            if (!s.enabled(gl_cap_offset(GlCap::Light0, i))) continue;
            const GlLight& light = l.lights[i];
            V3 vp;
            double att = 1.0;
            if (light.position[3] != 0.0f) {
                const V3 d{light.position[0] / light.position[3] - p[0], light.position[1] / light.position[3] - p[1],
                           light.position[2] / light.position[3] - p[2]};
                const double dist = std::sqrt(dot(d, d));
                att = 1.0 / (light.constant_attenuation + light.linear_attenuation * dist +
                             light.quadratic_attenuation * dist * dist);
                vp = unit(d);
            } else {
                vp = unit({light.position[0], light.position[1], light.position[2]});
            }
            double spot = 1.0;
            if (light.spot_cutoff != 180.0f) {
                const V3 sd = unit({light.spot_direction[0], light.spot_direction[1], light.spot_direction[2]});
                const double pvs = std::max(-dot(vp, sd), 0.0);
                const double cutoff = light.spot_cutoff == 90.0f ? 0.0 : std::cos(light.spot_cutoff * M_PI / 180.0);
                spot = pvs >= cutoff ? std::pow(pvs, static_cast<double>(light.spot_exponent)) : 0.0;
            }
            const double ndvp = dot(n, vp);
            const double f = ndvp > 0.0 ? 1.0 : 0.0;
            const V3 h = unit({vp[0] + viewer[0], vp[1] + viewer[1], vp[2] + viewer[2]});
            const double specular = std::pow(std::max(dot(n, h), 0.0), static_cast<double>(mat.shininess));
            for (usize k = 0; k < 3; ++k) {
                c[k] += att * spot *
                        (am[k] * light.ambient[k] + std::max(ndvp, 0.0) * dm[k] * light.diffuse[k] +
                         f * specular * sm[k] * light.specular[k]);
            }
        }
        return {static_cast<float>(std::clamp(c[0], 0.0, 1.0)), static_cast<float>(std::clamp(c[1], 0.0, 1.0)),
                static_cast<float>(std::clamp(c[2], 0.0, 1.0)), static_cast<float>(std::clamp(dm[3], 0.0, 1.0))};
    }
};

/// Whether any of s, t, r, q is generated (GL_TEXTURE_GEN_S..Q).
bool texgen_enabled(const GlState& s) {
    for (u32 c = 0; c < 4; ++c) {
        if (s.enabled(gl_cap_offset(GlCap::TextureGenS, c))) return true;
    }
    return false;
}

/// Spec 2.10.4 evaluated on the CPU in double precision for one vertex, for
/// the CPU-expanded primitives (whose pipelines generate nothing): each
/// enabled coordinate is replaced by OBJECT_LINEAR g = p_o . (object
/// coordinates), EYE_LINEAR g = p_e' . (eye coordinates) with the plane
/// stored at glTexGen time (already times M^-1), or SPHERE_MAP s, t = r_x /
/// m + 1/2, r_y / m + 1/2 with u the unit eye position, n' = n M^-1
/// (normalised under GL_NORMALIZE), r = u - 2 n' (n' . u) and m = 2 sqrt(r_x^2
/// + r_y^2 + (r_z + 1)^2). The generator's texgen evaluates the same formulas
/// for filled polygons (ff_emulation.cpp, emit_gl_texgen).
struct GlCpuTexgen {
    const GlState& s;
    GlMatrix modelview;
    std::optional<GlMatrix> inverse;

    void apply(GlBatchVertex& v) const {
        const std::array<double, 4> obj{v.pos[0], v.pos[1], v.pos[2], v.pos[3]};
        std::array<double, 4> eye{};
        const GlMatrix& m = modelview;
        for (usize r = 0; r < 4; ++r) {
            eye[r] = m[0 * 4 + r] * obj[0] + m[1 * 4 + r] * obj[1] + m[2 * 4 + r] * obj[2] + m[3 * 4 + r] * obj[3];
        }
        const auto dot4 = [](const std::array<double, 4>& a, const std::array<double, 4>& b) {
            return a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
        };
        std::array<double, 3> r{};
        double mlen = 0.0;
        bool sphere_ready = false;
        const auto sphere = [&]() {
            if (sphere_ready) return;
            sphere_ready = true;
            std::array<double, 3> u{eye[0] / eye[3], eye[1] / eye[3], eye[2] / eye[3]};
            const double ul = std::sqrt(u[0] * u[0] + u[1] * u[1] + u[2] * u[2]);
            if (ul > 0.0) u = {u[0] / ul, u[1] / ul, u[2] / ul};
            std::array<double, 3> n{v.normal[0], v.normal[1], v.normal[2]};
            if (inverse) {
                const GlMatrix& inv = *inverse;
                n = {n[0] * inv[0 * 4 + 0] + n[1] * inv[0 * 4 + 1] + n[2] * inv[0 * 4 + 2],
                     n[0] * inv[1 * 4 + 0] + n[1] * inv[1 * 4 + 1] + n[2] * inv[1 * 4 + 2],
                     n[0] * inv[2 * 4 + 0] + n[1] * inv[2 * 4 + 1] + n[2] * inv[2 * 4 + 2]};
            }
            if (s.enabled(GlCap::Normalize)) {
                const double nl = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
                if (nl > 0.0) n = {n[0] / nl, n[1] / nl, n[2] / nl};
            }
            const double nu = n[0] * u[0] + n[1] * u[1] + n[2] * u[2];
            r = {u[0] - 2.0 * n[0] * nu, u[1] - 2.0 * n[1] * nu, u[2] - 2.0 * n[2] * nu};
            mlen = 2.0 * std::sqrt(r[0] * r[0] + r[1] * r[1] + (r[2] + 1.0) * (r[2] + 1.0));
        };
        for (u32 c = 0; c < 4; ++c) {
            if (!s.enabled(gl_cap_offset(GlCap::TextureGenS, c))) continue;
            const GlTexGen& g = s.texture.gen[c];
            switch (g.mode) {
            case GL_OBJECT_LINEAR: v.tex[c] = static_cast<float>(dot4(g.object_plane, obj)); break;
            case GL_EYE_LINEAR: v.tex[c] = static_cast<float>(dot4(g.eye_plane, eye)); break;
            case GL_SPHERE_MAP:
                sphere();
                if (c < 2) v.tex[c] = static_cast<float>(r[c] / mlen + 0.5);
                break;
            default: break;
            }
        }
    }
};

}  // namespace

// ---- The batcher -------------------------------------------------------------------

class GlBatcher final : public GlPendingBatch {
public:
    GlBatcher() = default;
    GlBatcher(const GlBatcher&) = delete;
    GlBatcher& operator=(const GlBatcher&) = delete;

    ~GlBatcher() {
        if (auto d = target.lock()) {
            std::lock_guard<std::recursive_mutex> lock(d->mutex());
            d->drop_pending_batch(this);
            if (!vertices.empty()) record(*d);   // issued before the context went away
        }
    }

    void flush_into(GlDrawable& drawable) override { record(drawable); }

    /// Records the pending draw into @p d's pass (under d's lock).
    void record(GlDrawable& d);

    GlAssembler assembler;
    GlPrimitiveExpander expander;
    std::vector<GlBatchVertex> clamped_vertices;  ///< the group's vertices, colours clamped (lighting off)
    std::vector<GlBatchVertex> lit_front;         ///< ... their front lit colours (expanded primitives)
    std::vector<GlBatchVertex> lit_back;          ///< ... their back lit colours (two-sided)
    std::vector<GlBatchVertex> offset_vertices;   ///< filled polygons with their polygon offset
    std::vector<GlBatchVertex> texgen_vertices;   ///< ... their CPU-generated texture coordinates
    std::vector<u32> fill_indices;                ///< a run of filled polygons (polygon modes)
    GlBatchKey scratch_key;               ///< gl_draw_primitives' per-call key
    GlBatchKey key;                       ///< the pending draw's key
    std::vector<GlBatchVertex> vertices;  ///< the pending draw
    std::vector<u32> indices;
    std::weak_ptr<GlDrawable> target;     ///< where the pending draw goes
    GlBatchStats stats;

private:
    bool upload(graphics::GraphicsBackendDevice& device, std::unique_ptr<graphics::BackendBuffer>& buffer,
                usize& used, const void* data, usize bytes, bool index, unsigned& offset);
    graphics::BackendPipeline* pipeline_for(graphics::GraphicsBackendDevice& device, const GlPassLayout& layout);

    graphics::GraphicsBackendDevice* m_device{nullptr};
    std::unique_ptr<graphics::BackendBuffer> m_vertex_buffer;
    std::unique_ptr<graphics::BackendBuffer> m_index_buffer;
    usize m_vertex_used{0};
    usize m_index_used{0};
    std::unordered_map<u64, std::unique_ptr<graphics::BackendPipeline>> m_pipelines;
};

void GlBatcherDeleter::operator()(GlBatcher* batcher) const noexcept { delete batcher; }

namespace {

GlBatcher& batcher_of(GlContext& ctx) {
    if (!ctx.batcher) ctx.batcher.reset(new GlBatcher());
    return *ctx.batcher;
}

}  // namespace

bool GlBatcher::upload(graphics::GraphicsBackendDevice& device, std::unique_ptr<graphics::BackendBuffer>& buffer,
                       usize& used, const void* data, usize bytes, bool index, unsigned& offset) {
    const usize initial = index ? kInitialIndexBytes : kInitialVertexBytes;
    if (!buffer || buffer->desc().byte_width < bytes) {
        usize capacity = buffer ? buffer->desc().byte_width : initial;
        while (capacity < bytes) capacity *= 2;
        graphics::BackendBufferDesc desc{};
        desc.byte_width = capacity;
        desc.vertex_buffer = !index;
        desc.index_buffer = index;
        desc.cpu_writable = true;
        buffer = device.create_buffer(desc, nullptr);
        if (buffer && !buffer->valid()) buffer.reset();
        used = 0;
        if (!buffer) return false;
    }
    // Appends never touch bytes a recorded draw reads (NoOverwrite); when
    // the buffer is full the next draw starts it over in fresh storage
    // (Discard), leaving the GPU the old contents.
    graphics::BufferWrite mode = graphics::BufferWrite::NoOverwrite;
    if (used + bytes > buffer->desc().byte_width) {
        used = 0;
        mode = graphics::BufferWrite::Discard;
    }
    if (!buffer->update(used, data, bytes, mode)) return false;
    offset = static_cast<unsigned>(used);
    used += bytes;
    return true;
}

graphics::BackendPipeline* GlBatcher::pipeline_for(graphics::GraphicsBackendDevice& device,
                                                   const GlPassLayout& layout) {
    u64 cache_key = mix(key.ff_hash, key.raster_key);
    cache_key = mix(cache_key, (u64{layout.color_count} << 8) | static_cast<u64>(layout.depth_format));
    auto& slot = m_pipelines[cache_key];
    if (slot) return slot.get();

    const graphics::FFGeneratedShaders shaders = graphics::generate_ff_shaders(key.ff);
    if (!shaders.valid()) {
        m_pipelines.erase(cache_key);
        return nullptr;
    }
    graphics::BackendPipelineDesc desc{};
    desc.vertex_shader_msl = shaders.vertex_msl;
    desc.fragment_shader_msl = shaders.fragment_msl;
    // generate_ff_shaders names its entry points ff_vertex / ff_fragment; the
    // fragment writes every colour target itself (FFPipelineState::
    // color_targets).
    desc.vertex_entry_point = "ff_vertex";
    desc.fragment_entry_point = "ff_fragment";
    // The generator numbers attributes in FVF order (ff_vertex_layout.hpp):
    // position, [normal], diffuse, [texture coordinates], then the GL
    // attributes in the order line_distance, window_offset, depth_offset.
    // GlBatchVertex holds them at fixed offsets; position is the float4
    // object coordinates (position_is_homogeneous).
    unsigned location = 0;
    const auto attribute = [&](unsigned offset, graphics::BackendVertexFormat format) {
        desc.vertex_attributes.push_back({location++, offset, format});
    };
    attribute(offsetof(GlBatchVertex, pos), graphics::BackendVertexFormat::Float4);
    if ((key.ff.fvf_flags & fvf::NORMAL) != 0) attribute(offsetof(GlBatchVertex, normal), graphics::BackendVertexFormat::Float3);
    if ((key.ff.fvf_flags & fvf::DIFFUSE) != 0) attribute(offsetof(GlBatchVertex, color), graphics::BackendVertexFormat::Float4);
    // Texture coordinate set 0: (s, t, r, q), D3DFVF_TEXCOORDSIZE4(0).
    if (key.ff.texcoord_count() != 0) attribute(offsetof(GlBatchVertex, tex), graphics::BackendVertexFormat::Float4);
    if (key.ff.line_stipple.enabled) attribute(offsetof(GlBatchVertex, line_distance), graphics::BackendVertexFormat::Float1);
    if (key.ff.window_offset) attribute(offsetof(GlBatchVertex, window_offset), graphics::BackendVertexFormat::Float2);
    if (key.ff.depth_offset) attribute(offsetof(GlBatchVertex, depth_offset), graphics::BackendVertexFormat::Float2);
    desc.vertex_buffer_strides[0] = sizeof(GlBatchVertex);
    desc.vertex_stride = sizeof(GlBatchVertex);
    desc.color_format = layout.color_count != 0 ? graphics::PixelFormat::BGRA8Unorm : graphics::PixelFormat::Unknown;
    for (u32 i = 0; i < layout.color_count; ++i) desc.color_formats[i] = graphics::PixelFormat::BGRA8Unorm;
    key.raster.apply(desc, layout.color_count);
    desc.depth_attachment = layout.depth_format != graphics::PixelFormat::Unknown;
    desc.depth_format = layout.depth_format;
    desc.input_primitive_class = graphics::BackendPrimitiveClass::Triangle;
    desc.cache_key = cache_key;
    slot = device.create_pipeline(desc);
    ++stats.pipelines_created;
    if (slot && !slot->valid()) slot.reset();
    if (!slot) {
        m_pipelines.erase(cache_key);
        return nullptr;
    }
    return slot.get();
}

void GlBatcher::record(GlDrawable& d) {
    static std::atomic<bool> no_pipeline{false}, no_buffer{false}, refused{false};
    struct Clear {
        GlBatcher& b;
        ~Clear() {
            b.vertices.clear();
            b.indices.clear();
        }
    } clear{*this};
    if (vertices.empty() || indices.empty()) return;
    graphics::GraphicsBackendDevice* device = graphics::active_backend_device();
    if (device == nullptr) return;
    if (device != m_device) {
        // Pipelines and buffers belong to the device that made them.
        m_pipelines.clear();
        m_vertex_buffer.reset();
        m_index_buffer.reset();
        m_vertex_used = m_index_used = 0;
        m_device = device;
    }
    const GlPassLayout layout = d.pass_layout(key.draw_buffer);
    // A zero-sized window, or nothing to draw into: GL_NONE without a depth
    // buffer. (GL_NONE with one draws depth/stencil only.)
    if (layout.width == 0 || layout.height == 0) return;
    if (layout.color_count == 0 && layout.depth_format == graphics::PixelFormat::Unknown) return;
    graphics::BackendPipeline* pipeline = pipeline_for(*device, layout);
    if (pipeline == nullptr) {
        gl_log_once(no_pipeline, "the backend refused a GL fixed-function pipeline; primitives are not drawn");
        return;
    }
    unsigned vertex_offset = 0, index_offset = 0;
    if (!upload(*device, m_vertex_buffer, m_vertex_used, vertices.data(), vertices.size() * sizeof(GlBatchVertex),
                false, vertex_offset) ||
        !upload(*device, m_index_buffer, m_index_used, indices.data(), indices.size() * sizeof(u32), true,
                index_offset)) {
        gl_log_once(no_buffer, "the backend refused the GL streaming buffers; primitives are not drawn");
        return;
    }
    graphics::BackendDrawCall call{};
    call.pipeline = pipeline;
    call.topology = graphics::BackendPrimitiveTopology::TriangleList;
    call.vertex_buffer = m_vertex_buffer.get();
    call.vertex_buffer_offset = vertex_offset;
    call.vertex_buffers[0] = m_vertex_buffer.get();
    call.vertex_buffer_offsets[0] = vertex_offset;
    call.index_buffer = m_index_buffer.get();
    call.index_32bit = true;
    call.index_buffer_offset = index_offset;
    call.index_count = static_cast<unsigned>(indices.size());
    call.viewport = key.viewport;
    call.vertex_constants = key.vertex_uniforms.data();
    call.vertex_constants_size = key.vertex_uniforms.size();
    call.fragment_constants = key.fragment_uniforms.data();
    call.fragment_constants_size = key.fragment_uniforms.size();
    // Stage 0's texture; the generator's GL sampler reads it with read(), so
    // the default sampler slot is unused.
    call.textures[0] = key.texture ? key.texture->texture.get() : nullptr;
    call.textures[1] = key.texture1 ? key.texture1->texture.get() : nullptr;
    key.raster.apply(call);
    // Recording resolves the texture's shader view, which the backend
    // memoises per texture without a lock of its own; contexts sharing the
    // texture record on their own threads (GlTextureStorage::record_mutex).
    std::unique_lock<std::mutex> texture_lock;
    if (key.texture) texture_lock = std::unique_lock<std::mutex>(key.texture->record_mutex);
    // A pixel rectangle's stage 1 (the bound texture; stage 0 is its own
    // transient image, never shared, so the two locks never nest the other way).
    std::unique_lock<std::mutex> texture1_lock;
    if (key.texture1) texture1_lock = std::unique_lock<std::mutex>(key.texture1->record_mutex);
    if (!d.record_draw(key.draw_buffer, call)) {
        gl_log_once(refused, "the backend refused a GL draw");
        return;
    }
    ++stats.draws_recorded;
}

// ---- State mapping -----------------------------------------------------------------

void gl_ff_state(const GlContext& ctx, GlRasterClass raster_class, const GlTargetInfo& target,
                 graphics::FFPipelineState& ff, const GlDrawTexture* texture) {
    const GlState& s = ctx.state;
    ff = graphics::FFPipelineState{};
    // Spec 2.13 lights each vertex and rasterization interpolates the lit
    // colours (3.3, 3.4). Filled polygons are lit per vertex in the shader;
    // CPU-expanded points and lines arrive with their vertices' lit colours
    // already (gl_light_vertex), so their pipelines do not light.
    const bool lighting = s.enabled(GlCap::Lighting) && raster_class == GlRasterClass::Fill;
    // Spec 2.10.4 for filled polygons in the shader; expanded primitives
    // arrive with their vertices' generated coordinates (GlCpuTexgen).
    const bool textured = texture != nullptr && texture->enabled;
    const bool shader_texgen = textured && raster_class == GlRasterClass::Fill && texgen_enabled(s);
    bool sphere_map = false;
    if (shader_texgen) {
        for (u32 c = 0; c < 4; ++c) {
            if (!s.enabled(gl_cap_offset(GlCap::TextureGenS, c))) continue;
            const u32 mode = s.texture.gen[c].mode;
            ff.texgen_plane_mode[0][c] = mode == GL_OBJECT_LINEAR ? graphics::FFTexGenMode::ObjectLinear
                                         : mode == GL_EYE_LINEAR  ? graphics::FFTexGenMode::EyeLinear
                                                                  : graphics::FFTexGenMode::SphereMap;
            sphere_map = sphere_map || mode == GL_SPHERE_MAP;
        }
    }
    // Position, [the normal: lighting or sphere-map texgen], the vertex
    // colour, [texture coordinate set 0 as a float4 (s, t, r, q)].
    ff.fvf_flags = fvf::XYZ | fvf::DIFFUSE | (lighting || sphere_map ? fvf::NORMAL : 0u) |
                   (textured ? fvf::TEX1 | (2u << 16) : 0u);
    if (sphere_map) ff.normalize_normals = s.enabled(GlCap::Normalize);
    // Spec 2.10: object coordinates are homogeneous; (x, y, z, w) go to the
    // shader unchanged and are clipped in clip space (2.11) with their own w.
    ff.position_is_homogeneous = true;
    // Spec 2.13.7: FLAT takes the provoking vertex's colour; the assembler
    // put it first, which is the vertex Metal's [[flat]] reads (expanded
    // points and lines give every corner the provoking vertex's data).
    ff.flat_shading = s.lighting.shade_model == GL_FLAT;
    // Points and lines arrive as CPU-expanded triangles (Design 6).
    ff.point_size_output = false;
    ff.window_offset = raster_class != GlRasterClass::Fill;
    ff.color_targets = std::max<u32>(target.color_targets, 1u);
    // ---- Texturing (spec 3.8.6): one stage, the texture environment's
    // function of Tables 3.18-3.19, GL's own sampling rules ----
    ff.active_texture_stages = textured ? 1u : 0u;
    if (textured) {
        ff.texture_stages[0] = texture->stage;
        ff.gl_texture_sampling = true;
    }
    ff.specular_enable = false;      // GL 1.1 adds specular into the primary colour
    ff.vertex_color_enable = true;   // the primary colour is the vertex colour, or what colour material says

    // ---- Lighting (spec 2.13) ----
    ff.lighting_enabled = lighting;
    ff.local_viewer = false;
    if (lighting) {
        const GlLightingState& l = s.lighting;
        ff.gl_spot_model = true;
        ff.normalize_normals = s.enabled(GlCap::Normalize);
        ff.local_viewer = l.model_local_viewer;
        u32 n = 0;
        for (u32 i = 0; i < kMaxLights; ++i) {
            if (!s.enabled(gl_cap_offset(GlCap::Light0, i))) continue;
            const GlLight& src = l.lights[i];
            graphics::FFLight& dst = ff.lights[n++];
            // The type is ignored under gl_spot_model; one value for all
            // keeps the pipeline hash independent of the light positions.
            dst.type = graphics::LightType::Point;
            dst.enabled = true;
            copy4(dst.ambient, src.ambient);
            copy4(dst.diffuse, src.diffuse);
            copy4(dst.specular, src.specular);
            // Stored in eye coordinates at glLight time (2.13.2); view = I.
            for (usize k = 0; k < 3; ++k) {
                dst.position[k] = src.position[k];
                dst.direction[k] = src.spot_direction[k];
            }
            dst.gl_position_w = src.position[3];
            dst.gl_spot_exponent = src.spot_exponent;
            dst.gl_spot_cutoff = src.spot_cutoff;
            dst.attenuation[0] = src.constant_attenuation;
            dst.attenuation[1] = src.linear_attenuation;
            dst.attenuation[2] = src.quadratic_attenuation;
        }
        ff.active_light_count = n;
        ff.material = to_ff(l.material[0]);
        ff.back_material = to_ff(l.material[1]);
        for (usize k = 0; k < 4; ++k) ff.global_ambient[k] = l.model_ambient[k];
        // Filled polygons select the back colour by their facing.
        ff.two_sided_lighting = l.model_two_side;
        u32 d = 0, a = 0, sp = 0, e = 0;
        color_material_sources(0, d, a, sp, e);
        ff.diffuse_material_source = ff.back_diffuse_material_source = d;
        ff.ambient_material_source = ff.back_ambient_material_source = a;
        ff.specular_material_source = ff.back_specular_material_source = sp;
        ff.emissive_material_source = ff.back_emissive_material_source = e;
        if (s.enabled(GlCap::ColorMaterial)) {
            color_material_sources(l.color_material_parameter, d, a, sp, e);
            if (l.color_material_face == GL_FRONT || l.color_material_face == GL_FRONT_AND_BACK) {
                ff.diffuse_material_source = d;
                ff.ambient_material_source = a;
                ff.specular_material_source = sp;
                ff.emissive_material_source = e;
            }
            if (l.color_material_face == GL_BACK || l.color_material_face == GL_FRONT_AND_BACK) {
                ff.back_diffuse_material_source = d;
                ff.back_ambient_material_source = a;
                ff.back_specular_material_source = sp;
                ff.back_emissive_material_source = e;
            }
        }
    }

    // ---- Fog (spec 3.9), with z the eye-coordinate distance ----
    ff.fog_mode = graphics::FogMode::None;
    if (s.enabled(GlCap::Fog)) {
        ff.fog_mode = s.fog.mode == GL_LINEAR ? graphics::FogMode::Linear
                      : s.fog.mode == GL_EXP2 ? graphics::FogMode::Exp2
                                              : graphics::FogMode::Exp;
        ff.range_fog_enable = true;
        ff.fog_start = s.fog.start;
        ff.fog_end = s.fog.end;
        ff.fog_density = s.fog.density;
        for (usize k = 0; k < 4; ++k) ff.fog_color[k] = std::clamp(s.fog.color[k], 0.0f, 1.0f);
    }

    // ---- Alpha test (spec 4.1.3): GL_NEVER..GL_ALWAYS in D3DCMP order ----
    ff.alpha_test_enable = s.enabled(GlCap::AlphaTest);
    if (ff.alpha_test_enable) {
        ff.alpha_test_func = s.color_buffer.alpha_func - GL_NEVER + 1u;
        ff.alpha_ref_float = true;
    }

    // ---- Stipple (3.4.2 lines and polygon edges; 3.5.2 filled polygons) ----
    if ((raster_class == GlRasterClass::Line || raster_class == GlRasterClass::PolygonLine) &&
        s.enabled(GlCap::LineStipple)) {
        ff.line_stipple.enabled = true;
        ff.line_stipple.factor = static_cast<u32>(s.line.stipple_repeat);
        ff.line_stipple.pattern = s.line.stipple_pattern;
    }
    ff.polygon_stipple = raster_class == GlRasterClass::Fill && s.enabled(GlCap::PolygonStipple);

    // ---- Polygon offset (3.5.5), per polygon mode ----
    const GlCap offset_cap = raster_class == GlRasterClass::PolygonLine    ? GlCap::PolygonOffsetLine
                             : raster_class == GlRasterClass::PolygonPoint ? GlCap::PolygonOffsetPoint
                                                                           : GlCap::PolygonOffsetFill;
    // (Pixel rectangles are not polygons; gl_pixel_batch_key uses the field
    // for DEPTH_COMPONENT groups.)
    ff.depth_offset = target.depth && raster_class != GlRasterClass::Point && raster_class != GlRasterClass::Line &&
                      raster_class != GlRasterClass::Pixel && s.enabled(offset_cap);

    // ---- User clip planes (2.11) for filled polygons; expanded primitives
    // were clipped on the CPU ----
    if (raster_class == GlRasterClass::Fill) {
        for (u32 i = 0; i < kMaxClipPlanes; ++i) {
            if (s.enabled(gl_cap_offset(GlCap::ClipPlane0, i))) ff.clip_planes_enabled = true;
        }
    }

    // ---- Logic op (4.1.8); nothing to apply without a colour buffer ----
    if (s.enabled(GlCap::ColorLogicOp) && target.color_targets != 0) {
        ff.logic_op = gl_logic_op(s.color_buffer.logic_op_mode);
    }
}

bool GlBatchKey::same_draw(const GlBatchKey& o) const noexcept {
    return primitive == o.primitive && raster_class == o.raster_class && draw_buffer == o.draw_buffer &&
           ff_hash == o.ff_hash && raster_key == o.raster_key && raster == o.raster && viewport == o.viewport &&
           vertex_uniforms == o.vertex_uniforms && fragment_uniforms == o.fragment_uniforms && texture == o.texture &&
           texture1 == o.texture1;
}

namespace {

bool batch_key(GlContext& ctx, GlRasterClass raster_class, bool cpu_culled, const GlPixelParams* pixel,
               GlBatchKey& out) {
    GlDrawable* d = ctx.drawable.get();
    if (d == nullptr) return false;
    const GlState& s = ctx.state;
    const GlViewportState& vp = s.viewport;
    // The viewport maps every primitive to nothing; pixel rectangles do not
    // go through it.
    if (pixel == nullptr && (vp.width <= 0 || vp.height <= 0)) return false;
    note_unmapped_state(ctx);
    out.raster_class = raster_class;
    out.primitive = GlPrimitiveClass::Triangles;
    out.draw_buffer = s.color_buffer.draw_buffer;
    const GlPassLayout layout = d->pass_layout(out.draw_buffer);
    const GlTargetInfo target{layout.color_count, layout.depth_format != graphics::PixelFormat::Unknown};

    // Spec 4.1.8: the logic op reads the destination (framebuffer fetch).
    // Without it the op cannot be applied; drawing without it would be a
    // different picture, so the primitives are refused.
    const bool stencil_write = pixel != nullptr && pixel->kind == GlPixelParams::Kind::Stencil;
    if (!stencil_write && s.enabled(GlCap::ColorLogicOp) && s.color_buffer.logic_op_mode != GL_COPY &&
        layout.color_count != 0) {
        graphics::GraphicsBackendDevice* device = graphics::active_backend_device();
        if (device == nullptr || !device->supports_framebuffer_fetch()) {
            gl_log_unimplemented_once("glEnable(GL_COLOR_LOGIC_OP)",
                                      "the backend device has no framebuffer fetch; primitives are not drawn");
            return false;
        }
    }

    out.viewport = d->viewport_to_target(vp.x, vp.y, vp.width, vp.height, vp.depth_near, vp.depth_far);
    gl_raster_state(s, layout, *d, d->color_channels(), out.raster);
    if (cpu_culled || raster_class != GlRasterClass::Fill) out.raster.cull = graphics::BackendCullMode::None;
    // Spec 3.8.6: the bound texture of the enabled dimensionality, unless it
    // is null or an incomplete mipmap (then as if texturing were disabled).
    GlDrawTexture texture;
    // Kind::Image: the bound texture, applied after the image (stage 1).
    GlDrawTexture bound;
    if (pixel == nullptr || pixel->kind == GlPixelParams::Kind::Fragments || pixel->kind == GlPixelParams::Kind::Depth) {
        texture = gl_texture_for_draw(ctx);
    } else if (pixel->kind == GlPixelParams::Kind::Image && pixel->image) {
        // Stage 0: the group colour replaces the fragment colour, which is
        // the colour texturing (3.8) then sees as C_f.
        texture.enabled = true;
        texture.storage = pixel->image;
        texture.stage = gl_tex_env_stage(GL_REPLACE, GL_RGBA);
        texture.unit = pixel->image_unit;
        bound = gl_texture_for_draw(ctx);
    }
    out.texture = texture.enabled ? texture.storage : nullptr;
    out.texture1 = bound.enabled ? bound.storage : nullptr;
    gl_ff_state(ctx, raster_class, target, out.ff, &texture);
    if (bound.enabled) {
        // Stage 1: the texture environment of the bound texture, C_f the
        // stage 0 result (CURRENT). Its coordinates are set 0 times a
        // texture matrix that sends every image coordinate (s, t, 0, 1) to
        // the raster position's (spec 3.6.5), constant across the draw.
        out.ff.active_texture_stages = 2;
        out.ff.texture_stages[1] = bound.stage;
    }
    if (pixel != nullptr) {
        // Spec 3.6.5: a DEPTH_COMPONENT group gives the fragment its depth.
        out.ff.depth_offset = pixel->kind == GlPixelParams::Kind::Depth && target.depth;
        if (stencil_write) {
            // Spec 4.3.1: stencil groups bypass texturing, fog and every
            // per-fragment operation but the pixel ownership and scissor
            // tests; the stencil index is written under the stencil mask.
            out.ff.fog_mode = graphics::FogMode::None;
            out.ff.alpha_test_enable = false;
            out.ff.logic_op = graphics::FFLogicOp::Copy;
            GlRasterState& r = out.raster;
            r.blend = false;
            r.color_write_mask = 0;
            r.depth_stencil = graphics::BackendDepthStencilDesc{};
            r.depth_stencil.stencil_test_enabled = layout.has_stencil;
            r.depth_stencil.stencil_read_mask = 0xFF;
            r.depth_stencil.stencil_write_mask = static_cast<u8>(s.stencil_buffer.writemask & 0xFFu);
            graphics::BackendStencilFaceDesc face{};
            face.compare = graphics::BackendCompare::Always;
            face.stencil_fail = face.depth_fail = face.pass = graphics::BackendStencilOperation::Replace;
            r.depth_stencil.front_stencil = r.depth_stencil.back_stencil = face;
            r.stencil_reference = pixel->stencil_value;
        }
    }
    out.ff_hash = out.ff.hash();
    out.raster_key = mix(out.raster.pipeline_key(), static_cast<u64>(raster_class));

    // Spec 2.10: clip = P * M * object; the backend's depth range is [0, w].
    // world = modelview, so `in.position * cb.world` is eye space (what GL
    // lighting, fog, user clip planes and EYE_LINEAR texgen use); view =
    // identity; projection = Z * P (to_ff).
    graphics::FFUniformInputs inputs;
    inputs.world_matrices[0] = to_ff(ctx.modelview());
    inputs.view = graphics::FF_IDENTITY_MATRIX;
    GlMatrix projection = ctx.projection();
    inputs.viewport_width = static_cast<float>(vp.width);
    inputs.viewport_height = static_cast<float>(vp.height);
    if (raster_class != GlRasterClass::Fill) {
        // Expanded points and lines were clipped on the CPU by their centre
        // or as segments (2.11); their squares and parallelograms produce
        // fragments outside the viewport too (3.3, 3.4.2), which the
        // backend's clip volume would cut at the viewport. They draw with a
        // whole-target viewport instead, the GL viewport map folded into the
        // projection: X_ndc = x_ndc p_x / W + (2 o_x + p_x) / W - 1, so x_w is
        // unchanged (likewise y; depth keeps the GL depth range).
        const double tw = d->width(), th = d->height();
        if (tw > 0 && th > 0) {
            GlMatrix fold = gl_identity_matrix();
            fold[0] = vp.width / tw;
            fold[5] = vp.height / th;
            fold[12] = (2.0 * vp.x + vp.width) / tw - 1.0;
            fold[13] = (2.0 * vp.y + vp.height) / th - 1.0;
            projection = gl_multiply(fold, projection);
            out.viewport = d->viewport_to_target(0, 0, static_cast<i32>(tw), static_cast<i32>(th), vp.depth_near,
                                                 vp.depth_far);
            inputs.viewport_width = static_cast<float>(tw);
            inputs.viewport_height = static_cast<float>(th);
        }
    }
    inputs.projection = to_ff(gl_multiply(kGlToMetalClipZ, projection));
    if (pixel != nullptr) {
        // Pixel rectangles (GlPixelParams): object (0, 0, 0, 1) is the eye
        // point (0, 0, -|eye_z|, 1) (fog's distance, 3.9), which the
        // projection sends to clip (-1, -1, z, 1): window (0, 0) of the whole
        // target at Metal depth z = z_w (the depth range is [0, 1] here).
        // Each vertex's window_offset is its window position. A Depth
        // draw's z is 0 and its depth_offset.x the group's depth.
        GlMatrix world = gl_identity_matrix();
        world[14] = -std::abs(static_cast<double>(pixel->eye_z));
        GlMatrix to_window{};
        to_window[12] = -1.0;
        to_window[13] = -1.0;
        to_window[14] = pixel->kind == GlPixelParams::Kind::Depth ? 0.0 : static_cast<double>(pixel->window_z);
        to_window[15] = 1.0;
        inputs.world_matrices[0] = to_ff(world);
        inputs.projection = to_ff(to_window);
        const u32 tw = d->width(), th = d->height();
        out.viewport = d->viewport_to_target(0, 0, static_cast<i32>(tw), static_cast<i32>(th), 0.0, 1.0);
        inputs.viewport_width = static_cast<float>(tw);
        inputs.viewport_height = static_cast<float>(th);
    }
    inputs.alpha_reference = s.color_buffer.alpha_ref;
    if (texture.enabled) {
        // Spec 3.8.5: TEXTURE_ENV_COLOR is C_c (the TFACTOR argument).
        for (usize k = 0; k < 4; ++k) inputs.texture_factor[k] = s.texture.env_color[k];
        // Spec 2.10.2: the texture matrix, row-vector form like the others.
        // A pixel rectangle's coordinates are final: the raster position's
        // went through the matrix at glRasterPos (2.12), an image's are its
        // own.
        inputs.texture_transforms[0] = pixel != nullptr ? graphics::FF_IDENTITY_MATRIX : to_ff(ctx.texture_matrix());
        if (bound.enabled) {
            // Row-vector form p * M with only row 3 set: (s, t, r, w) * M =
            // w * raster_tex, and the image coordinates have w = 1.
            graphics::FFMatrix constant{};
            for (usize k = 0; k < 4; ++k) constant[12 + k] = pixel->raster_tex[k];
            inputs.texture_transforms[1] = constant;
            inputs.gl_textures[1] = bound.unit;
        }
        inputs.gl_textures[0] = texture.unit;
        for (u32 c = 0; c < 4; ++c) {
            for (usize k = 0; k < 4; ++k) {
                inputs.texgen_object_planes[0][c][k] = static_cast<float>(s.texture.gen[c].object_plane[k]);
                inputs.texgen_eye_planes[0][c][k] = static_cast<float>(s.texture.gen[c].eye_plane[k]);
            }
        }
    }
    for (u32 i = 0; i < kMaxClipPlanes; ++i) {
        if (!s.enabled(gl_cap_offset(GlCap::ClipPlane0, i))) continue;
        inputs.clip_plane_enable_mask |= 1u << i;
        for (usize k = 0; k < 4; ++k) inputs.clip_planes[i][k] = static_cast<float>(s.transform.clip_planes[i][k]);
    }
    if (out.ff.polygon_stipple) {
        // Spec 3.5.2: bit (x_w mod 32, y_w mod 32) of the pattern, y_w up;
        // the generator indexes rows by target row (row 0 at the top), and
        // target row t is window row H - 1 - t.
        const u32 h = d->height();
        for (u32 t = 0; t < 32; ++t) {
            const u32 window_row = static_cast<u32>((static_cast<i64>(h) - 1 - t) & 31);
            inputs.polygon_stipple_rows[t] = s.polygon_stipple.rows[window_row];
        }
    }
    graphics::pack_ff_vertex_uniforms(out.ff, inputs, out.vertex_uniforms);
    graphics::pack_ff_fragment_uniforms(out.ff, inputs, out.fragment_uniforms);
    return true;
}

}  // namespace

bool gl_batch_key(GlContext& ctx, GlRasterClass raster_class, bool cpu_culled, GlBatchKey& out) {
    return batch_key(ctx, raster_class, cpu_culled, nullptr, out);
}

bool gl_pixel_batch_key(GlContext& ctx, const GlPixelParams& pixel, GlBatchKey& out) {
    return batch_key(ctx, GlRasterClass::Pixel, true, &pixel, out);
}

std::array<float, 4> gl_cpu_lit_color(const GlContext& ctx, const GlBatchVertex& v) {
    const GlCpuLighting lighting{ctx.state, ctx.modelview(), gl_invert(ctx.modelview())};
    return lighting.lit(v, false);
}

void gl_cpu_texgen(const GlContext& ctx, GlBatchVertex& v) {
    const GlCpuTexgen texgen{ctx.state, ctx.modelview(), gl_invert(ctx.modelview())};
    texgen.apply(v);
}

// ---- Submission ---------------------------------------------------------------------

bool gl_submit_batch(GlContext& ctx, const GlBatchKey& key, std::span<const GlBatchVertex> vertices,
                     std::span<const u32> indices) {
    std::shared_ptr<GlDrawable> d = ctx.drawable;
    if (!d || vertices.empty() || indices.empty() || key.primitive == GlPrimitiveClass::None) return false;
    GlBatcher& b = batcher_of(ctx);
    // A batch still pending on another drawable (the context moved) is drawn
    // there first, before this drawable's lock is taken.
    if (auto previous = b.target.lock(); previous && previous != d) {
        std::lock_guard<std::recursive_mutex> lock(previous->mutex());
        previous->drop_pending_batch(&b);
        b.record(*previous);
    }
    std::lock_guard<std::recursive_mutex> lock(d->mutex());
    if (!b.vertices.empty() &&
        (!b.key.same_draw(key) || b.vertices.size() + vertices.size() > kMaxBatchVertices)) {
        d->drop_pending_batch(&b);
        b.record(*d);
    }
    // Another context's batch pending here was issued first: it is recorded
    // now, before this one can be.
    d->set_pending_batch(&b);
    b.target = d;
    if (b.vertices.empty()) b.key = key;

    // Lighting off: spec 2.13.6 clamps the vertex colours to [0, 1]. The
    // corners of an expanded point or line are not vertices: they may lie
    // beyond a segment's ends, where eq. 3.1 extrapolates the (clamped)
    // vertex colours, so gl_draw_primitives clamped before expanding.
    const bool clamp = !key.ff.lighting_enabled && key.raster_class == GlRasterClass::Fill;
    const u32 base = static_cast<u32>(b.vertices.size());
    for (const GlBatchVertex& in : vertices) {
        GlBatchVertex v = in;
        if (clamp) {
            for (float& c : v.color) c = std::clamp(c, 0.0f, 1.0f);
        }
        b.vertices.push_back(v);
    }
    usize appended = 0;
    for (usize p = 0; p + 3 <= indices.size(); p += 3) {
        if (indices[p] >= vertices.size() || indices[p + 1] >= vertices.size() || indices[p + 2] >= vertices.size()) {
            continue;
        }
        for (u32 k = 0; k < 3; ++k) b.indices.push_back(base + indices[p + k]);
        appended += 3;
    }
    if (appended == 0) {
        b.vertices.resize(base);
        if (b.vertices.empty()) d->drop_pending_batch(&b);
        return false;
    }
    return true;
}

namespace {

/// Submits one rasterised group as @p raster_class.
void submit_group(GlContext& ctx, GlBatcher& b, GlRasterClass raster_class, bool cpu_culled,
                  std::span<const GlBatchVertex> vertices, std::span<const u32> indices) {
    if (vertices.empty() || indices.empty()) return;
    if (!gl_batch_key(ctx, raster_class, cpu_culled, b.scratch_key)) return;
    (void)gl_submit_batch(ctx, b.scratch_key, vertices, indices);
}

}  // namespace

void gl_draw_primitives(GlContext& ctx, u32 mode, std::span<const GlBatchVertex> vertices,
                        std::span<const u8> edge_flags) {
    if (!ctx.drawable || vertices.empty()) return;
    GlBatcher& b = batcher_of(ctx);
    GlAssembler& as = b.assembler;
    as.emit(mode, vertices, edge_flags);
    const std::span<const u32> idx = as.indices();
    if (idx.empty()) return;
    const GlState& s = ctx.state;
    const bool flat = s.lighting.shade_model == GL_FLAT;
    if (!s.enabled(GlCap::Lighting)) {
        // Spec 2.13.6: with lighting off the vertex colours are clamped to
        // [0, 1], before rasterization interpolates them.
        b.clamped_vertices.assign(vertices.begin(), vertices.end());
        for (GlBatchVertex& v : b.clamped_vertices) {
            for (float& c : v.color) c = std::clamp(c, 0.0f, 1.0f);
        }
        vertices = b.clamped_vertices;
    }
    // Spec 2.10.4: each vertex's generated texture coordinates, which
    // rasterization interpolates (3.3, 3.4). The CPU-expanded primitives
    // take them from here; filled polygons generate the same values in the
    // shader (their copies here are overwritten there).
    const bool expanded = as.primitive_class() != GlPrimitiveClass::Triangles || s.polygon.mode[0] != GL_FILL ||
                          s.polygon.mode[1] != GL_FILL;
    if (expanded && (s.enabled(GlCap::Texture1D) || s.enabled(GlCap::Texture2D)) && texgen_enabled(s)) {
        const GlCpuTexgen texgen{s, ctx.modelview(), gl_invert(ctx.modelview())};
        b.texgen_vertices.assign(vertices.begin(), vertices.end());
        for (GlBatchVertex& v : b.texgen_vertices) texgen.apply(v);
        vertices = b.texgen_vertices;
    }

    // The CPU view of the transform (points, lines, polygon modes).
    GlPrimitiveExpander& ex = b.expander;
    ex.clear();
    ex.map.clip = gl_multiply(ctx.projection(), ctx.modelview());
    ex.map.eye = ctx.modelview();
    ex.map.x = s.viewport.x;
    ex.map.y = s.viewport.y;
    ex.map.width = s.viewport.width;
    ex.map.height = s.viewport.height;
    ex.map.depth_near = s.viewport.depth_near;
    ex.map.depth_far = s.viewport.depth_far;
    ex.map.clip_planes.clear();
    for (u32 i = 0; i < kMaxClipPlanes; ++i) {
        if (s.enabled(gl_cap_offset(GlCap::ClipPlane0, i))) ex.map.clip_planes.push_back(s.transform.clip_planes[i]);
    }
    ex.point_size = s.point.size;
    ex.line_width = s.line.width;
    ex.front_is_ccw = s.polygon.front_face == GL_CCW;
    const float no_offset[2] = {0.0f, 0.0f};

    // Spec 2.13: each vertex is lit, and rasterization interpolates the lit
    // colours (eq. 3.1 / 3.2 along a segment, the vertex's for a point). For
    // the CPU-expanded primitives the vertices are lit here, so the
    // expansion extrapolates lit colours to its corners; filled polygons are
    // lit by the shader.
    const bool cpu_lighting = s.enabled(GlCap::Lighting);
    const GlCpuLighting lighting{s, ctx.modelview(),
                                 cpu_lighting ? gl_invert(ctx.modelview()) : std::optional<GlMatrix>{}};
    const auto lit_copy = [&](std::vector<GlBatchVertex>& out, bool back) {
        out.assign(vertices.begin(), vertices.end());
        for (GlBatchVertex& v : out) {
            const std::array<float, 4> c = lighting.lit(v, back);
            std::copy(c.begin(), c.end(), v.color);
        }
    };

    switch (as.primitive_class()) {
    case GlPrimitiveClass::Points: {
        // Points always take the front colour (2.13.1).
        std::span<const GlBatchVertex> pts = vertices;
        if (cpu_lighting) {
            lit_copy(b.lit_front, false);
            pts = b.lit_front;
        }
        for (u32 i : idx) ex.point(pts[i], pts[i], true, no_offset);
        submit_group(ctx, b, GlRasterClass::Point, false, ex.vertices, ex.indices);
        return;
    }
    case GlPrimitiveClass::Lines: {
        // Lines always take the front colour (2.13.1).
        std::span<const GlBatchVertex> lv = vertices;
        if (cpu_lighting) {
            lit_copy(b.lit_front, false);
            lv = b.lit_front;
        }
        // Spec 3.4.2: the stipple counter is reset by glBegin and before each
        // segment of GL_LINES, and continues along strips and loops. The
        // assembler lists a segment (provoking = its end, start).
        float counter = 0.0f;
        for (usize k = 0; k + 1 < idx.size(); k += 2) {
            if (mode == GL_LINES) counter = 0.0f;
            const GlBatchVertex& end = lv[idx[k]];
            ex.segment(lv[idx[k + 1]], end, flat ? &end : nullptr, true, counter, no_offset);
        }
        submit_group(ctx, b, GlRasterClass::Line, false, ex.vertices, ex.indices);
        return;
    }
    case GlPrimitiveClass::Triangles: break;
    default: return;
    }

    // ---- Polygons ----
    const bool cull = s.enabled(GlCap::CullFace);
    if (cull && s.polygon.cull_face_mode == GL_FRONT_AND_BACK) return;   // 3.5.1: every polygon is culled
    const GlPassLayout layout = ctx.drawable->pass_layout(s.color_buffer.draw_buffer);
    const double r = gl_depth_resolution(layout.depth_format);
    const auto offset_for = [&](double slope, bool cpu_slope) {
        // o = m * factor + r * units (eq. 3.7).
        const double units = r * s.polygon.offset_units;
        return cpu_slope ? std::array<float, 2>{static_cast<float>(slope * s.polygon.offset_factor + units), 0.0f}
                         : std::array<float, 2>{static_cast<float>(units), s.polygon.offset_factor};
    };
    std::span<const GlBatchVertex> fill_vertices = vertices;
    if (s.enabled(GlCap::PolygonOffsetFill) && r != 0.0) {
        const std::array<float, 2> o = offset_for(0.0, false);
        b.offset_vertices.assign(vertices.begin(), vertices.end());
        for (GlBatchVertex& v : b.offset_vertices) {
            v.depth_offset[0] = o[0];
            v.depth_offset[1] = o[1];
        }
        fill_vertices = b.offset_vertices;
    }

    if (s.polygon.mode[0] == GL_FILL && s.polygon.mode[1] == GL_FILL) {
        // The backend culls (front_counter_clockwise, gl_raster_state.hpp).
        submit_group(ctx, b, GlRasterClass::Fill, false, fill_vertices, idx);
        return;
    }

    // Spec 3.5.4: facing first (eq. 2.6), then culling, then each face's
    // mode. Consecutive polygons of one mode are submitted together, in
    // order.
    GlRasterClass run = GlRasterClass::Fill;
    b.fill_indices.clear();
    const auto flush_run = [&]() {
        if (run == GlRasterClass::Fill) {
            submit_group(ctx, b, GlRasterClass::Fill, true, fill_vertices, b.fill_indices);
            b.fill_indices.clear();
        } else {
            submit_group(ctx, b, run, true, ex.vertices, ex.indices);
            ex.clear();
        }
    };
    const std::span<const GlAssembler::Polygon> polygons = as.polygons();
    const std::span<const u32> poly_vertices = as.polygon_vertices();
    const std::span<const u8> boundary = as.polygon_boundary();
    // Edges and vertices drawn as lines or points: their polygon's lit
    // colours, the back ones for a back-facing polygon under two-sided
    // lighting (2.13.1; lit before the polygon mode applies, 3.5.4).
    const bool two_sided = cpu_lighting && s.lighting.model_two_side;
    if (cpu_lighting) {
        lit_copy(b.lit_front, false);
        if (two_sided) lit_copy(b.lit_back, true);
    }
    const auto edge_data = [&](bool front) -> std::span<const GlBatchVertex> {
        if (!cpu_lighting) return vertices;
        return front || !two_sided ? std::span<const GlBatchVertex>(b.lit_front) : std::span<const GlBatchVertex>(b.lit_back);
    };
    for (const GlAssembler::Polygon& p : polygons) {
        const std::span<const u32> pv = poly_vertices.subspan(p.first_vertex, p.vertex_count);
        const double area = polygon_area_sign(ex.map, vertices, pv);
        const bool front = ex.front_is_ccw ? area > 0.0 : area < 0.0;
        if (cull && (s.polygon.cull_face_mode == GL_FRONT) == front) continue;
        const u32 polygon_mode = s.polygon.mode[front ? 0 : 1];
        const GlRasterClass cls = polygon_mode == GL_LINE    ? GlRasterClass::PolygonLine
                                  : polygon_mode == GL_POINT ? GlRasterClass::PolygonPoint
                                                             : GlRasterClass::Fill;
        if (cls != run) {
            flush_run();
            run = cls;
        }
        if (cls == GlRasterClass::Fill) {
            const auto tri = idx.subspan(static_cast<usize>(p.first_triangle) * 3, static_cast<usize>(p.triangle_count) * 3);
            b.fill_indices.insert(b.fill_indices.end(), tri.begin(), tri.end());
            continue;
        }
        // Flat shading: the polygon's provoking vertex (first of its first
        // triangle) colours its edges and vertices (2.13.7).
        const std::span<const GlBatchVertex> ev = edge_data(front);
        const GlBatchVertex& provoking = ev[idx[static_cast<usize>(p.first_triangle) * 3]];
        const GlCap offset_cap = cls == GlRasterClass::PolygonLine ? GlCap::PolygonOffsetLine : GlCap::PolygonOffsetPoint;
        std::array<float, 2> offset{0.0f, 0.0f};
        if (s.enabled(offset_cap) && r != 0.0) offset = offset_for(polygon_depth_slope(ex.map, vertices, pv), true);
        if (cls == GlRasterClass::PolygonPoint) {
            // The vertices that begin a boundary edge, each once.
            for (u32 i = 0; i < p.vertex_count; ++i) {
                if (boundary[p.first_vertex + i] == 0) continue;
                const GlBatchVertex& v = ev[pv[i]];
                ex.point(v, flat ? provoking : v, front, offset.data());
            }
        } else {
            // The boundary edges; the stipple counter is reset at the first
            // rasterised edge of the polygon only (3.5.4).
            float counter = 0.0f;
            for (u32 i = 0; i < p.vertex_count; ++i) {
                if (boundary[p.first_vertex + i] == 0) continue;
                ex.segment(ev[pv[i]], ev[pv[(i + 1) % p.vertex_count]], flat ? &provoking : nullptr, front,
                           counter, offset.data());
            }
        }
    }
    flush_run();
}

void gl_flush_batch(GlContext& ctx) {
    if (!ctx.batcher) return;
    GlBatcher& b = *ctx.batcher;
    if (auto d = b.target.lock()) {
        std::lock_guard<std::recursive_mutex> lock(d->mutex());
        d->drop_pending_batch(&b);
        b.record(*d);
    }
}

GlBatchStats gl_batch_stats(const GlContext& ctx) {
    return ctx.batcher ? ctx.batcher->stats : GlBatchStats{};
}

}  // namespace vitreo::winapi::gl
