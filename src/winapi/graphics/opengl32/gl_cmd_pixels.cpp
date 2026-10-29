// SPDX-License-Identifier: LGPL-2.1-only
/// @file gl_cmd_pixels.cpp
/// @brief OpenGL 1.1 pixel rectangles: the current raster position (spec
///        2.12), glBitmap (3.7), glDrawPixels (3.6.4-3.6.5, 4.3.1),
///        glCopyPixels (4.3.3) and glReadPixels (4.3.2), and their exports.
///
/// Design 7. Client images go through the CPU unpack and pixel transfer
/// pipeline of gl_pixels.cpp (pixel store, scale and bias, the maps, index
/// to RGBA, index arithmetic, the stencil and depth paths). The fragments
/// they produce are found on the CPU by the spec's rules (3.6.5's zoomed
/// group rectangles, 3.7's bitmap array at floor(raster - origin)) and drawn
/// as window-aligned quads whose corners lie on pixel edges, so the backend
/// rasterises exactly those fragment centres (gl_backend_draw.hpp,
/// GlPixelParams). They take the raster position's depth, colour and
/// texture coordinates and go through the normal per-fragment operations in
/// the context's open pass:
/// - glBitmap: one quad per run of 1 bits (vertical runs merged), in the
///   streaming batch. Consecutive glBitmap calls under unchanged state share
///   the pending draw: no pass, pipeline or texture per glyph.
/// - Colour groups: the zoomed image is resampled on the CPU to one texel per
///   covered fragment, uploaded to a transient RGBA32Float texture and drawn
///   as one quad (stage 0, NEAREST: the group colour becomes the fragment
///   colour; with texturing enabled the bound texture is stage 1, applied by
///   its texture environment at the raster texture coordinates, 3.6.5; fog
///   and the per-fragment operations follow).
/// - DEPTH_COMPONENT groups: quads carrying the group's depth (the raster
///   colour and texture coordinates), through every per-fragment operation.
/// - STENCIL_INDEX groups: quads per stencil value writing it under the
///   stencil mask, with only the ownership and scissor tests (4.3.1).
/// glReadPixels and glCopyPixels read through GlDrawable::read_* (which
/// record pending draws and end the pass), so the copy's source is fully
/// read before any of it is drawn.
///
/// Measured on GDI Generic (gl11_state_probe64.expected) and followed here:
/// a culled raster position clears only the valid bit; glBitmap with an
/// invalid raster position neither draws nor moves it; CURRENT_RASTER_DISTANCE
/// is the eye z; glReadPixels leaves the client bytes of pixels outside the
/// window untouched; the error order of each command.

#include "gl_commands.hpp"
#include "gl_backend_draw.hpp"
#include "gl_client_memory.hpp"
#include "gl_cmd_internal.hpp"
#include "gl_drawable.hpp"
#include "gl_exports.hpp"
#include "gl_pixels.hpp"
#include "gl_texture.hpp"

#include "core/memory/guest_memory.hpp"
#include "graphics/contracts/backend_device.hpp"
#include "graphics/contracts/backend_texture_2d.hpp"
#include "winapi/core/marshaler.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <type_traits>
#include <vector>

namespace vitreo::winapi::gl {

namespace {

std::atomic<bool> g_unreadable_logged{false};
std::atomic<bool> g_no_texture_logged{false};

/// Fragment columns (or rows) [lo, hi) a group n covers along one axis
/// (spec 3.6.5): the region between origin + zoom n and origin + zoom (n + 1),
/// of which the fragments whose centres lie inside it or on its left
/// (bottom) boundary are produced, for either sign of zoom.
struct Span {
    i64 lo{0}, hi{0};
};

Span zoom_span(double origin, double zoom, i64 n) {
    const double a = origin + zoom * static_cast<double>(n);
    const double b = origin + zoom * static_cast<double>(n + 1);
    const double lo = std::min(a, b), hi = std::max(a, b);
    // Centres c = i + 1/2 with lo <= c < hi.
    return {static_cast<i64>(std::ceil(lo - 0.5)), static_cast<i64>(std::ceil(hi - 0.5))};
}

/// The pixel groups of a draw, bottom row first.
struct PixelGroups {
    GlPixelParams::Kind kind{GlPixelParams::Kind::Image};
    std::vector<GlRgba> rgba;        ///< Image: clamped colours
    std::vector<float> depth;        ///< Depth: clamped depths
    std::vector<u32> stencil;        ///< Stencil: masked indices
};

/// Window-aligned quads (corners on pixel edges) of pixel fragments.
struct PixelQuads {
    std::vector<GlBatchVertex> vertices;
    std::vector<u32> indices;

    void clear() {
        vertices.clear();
        indices.clear();
    }
    /// The fragments of window pixels [x0, x1) x [y0, y1), each corner a copy
    /// of @p proto at its window position (with @p tex per corner when given:
    /// s0, t0 at the lower left, s1, t1 at the upper right).
    void rect(i64 x0, i64 y0, i64 x1, i64 y1, const GlBatchVertex& proto, const float* tex_rect = nullptr) {
        if (x1 <= x0 || y1 <= y0) return;
        const u32 base = static_cast<u32>(vertices.size());
        const i64 xs[4] = {x0, x1, x1, x0};
        const i64 ys[4] = {y0, y0, y1, y1};
        for (u32 k = 0; k < 4; ++k) {
            GlBatchVertex v = proto;
            v.window_offset[0] = static_cast<float>(xs[k]);
            v.window_offset[1] = static_cast<float>(ys[k]);
            if (tex_rect != nullptr) {
                v.tex[0] = xs[k] == x0 ? tex_rect[0] : tex_rect[2];
                v.tex[1] = ys[k] == y0 ? tex_rect[1] : tex_rect[3];
                v.tex[2] = 0.0f;
                v.tex[3] = 1.0f;
            }
            vertices.push_back(v);
        }
        for (u32 k : {0u, 1u, 2u, 0u, 2u, 3u}) indices.push_back(base + k);
    }
};

/// A vertex carrying the raster position's associated data (spec 3.6.5,
/// 3.7): its colour and its texture coordinates with s, t, r divided by q.
GlBatchVertex raster_vertex(const GlContext& ctx) {
    const GlCurrentState& c = ctx.state.current;
    GlBatchVertex v{};
    v.pos[3] = 1.0f;
    for (usize k = 0; k < 4; ++k) v.color[k] = c.raster_color[k];
    const float q = c.raster_tex_coords[3];
    const float inv = q != 0.0f ? 1.0f / q : 1.0f;   // q <= 0: undefined (3.6.5)
    for (usize k = 0; k < 3; ++k) v.tex[k] = c.raster_tex_coords[k] * inv;
    v.tex[3] = 1.0f;
    return v;
}

GlPixelParams raster_params(const GlContext& ctx, GlPixelParams::Kind kind) {
    GlPixelParams p;
    p.kind = kind;
    p.window_z = std::clamp(ctx.state.current.raster_position[2], 0.0f, 1.0f);
    p.eye_z = ctx.state.current.raster_distance;
    p.raster_tex = ctx.state.current.raster_tex_coords;
    return p;
}

thread_local GlBatchKey t_key;
thread_local PixelQuads t_quads;

void submit(GlContext& ctx, const GlPixelParams& params, PixelQuads& quads) {
    if (quads.vertices.empty()) return;
    if (gl_pixel_batch_key(ctx, params, t_key)) (void)gl_submit_batch(ctx, t_key, quads.vertices, quads.indices);
    // The pending draw holds what it needs; the scratch key must not keep a
    // transient image (or a texture) alive.
    t_key.texture.reset();
    t_key.texture1.reset();
}

/// The window's size, or 0 x 0 without a drawable.
void window_size(const GlContext& ctx, i64& w, i64& h) {
    w = h = 0;
    if (ctx.drawable) {
        w = ctx.drawable->width();
        h = ctx.drawable->height();
    }
}

u32 next_pow2(u32 v) {
    u32 p = 1;
    while (p < v) p <<= 1;
    return p;
}

u32 log2_exact(u32 v) {
    u32 n = 0;
    while ((1u << n) < v) ++n;
    return n;
}

/// Spec 3.6.5 / 4.3.1: the groups of a width x height rectangle as fragments
/// at the current raster position under the pixel zoom.
void draw_groups(GlContext& ctx, const PixelGroups& g, i32 width, i32 height) {
    if (!ctx.drawable) return;
    i64 ww = 0, wh = 0;
    window_size(ctx, ww, wh);
    if (ww == 0 || wh == 0) return;
    const GlCurrentState& c = ctx.state.current;
    const double xr = c.raster_position[0], yr = c.raster_position[1];
    const double zx = ctx.state.pixel_mode.zoom_x, zy = ctx.state.pixel_mode.zoom_y;
    // One span per image column and row: the image's own size, refused as
    // GL_OUT_OF_MEMORY when the host cannot hold it (gl_client_memory.hpp).
    std::vector<Span> cols, rows;
    if (!gl_try_resize(cols, static_cast<usize>(width)) || !gl_try_resize(rows, static_cast<usize>(height))) {
        ctx.record_error(GL_OUT_OF_MEMORY);
        return;
    }
    for (i32 n = 0; n < width; ++n) {
        Span s = zoom_span(xr, zx, n);
        s.lo = std::clamp<i64>(s.lo, 0, ww);   // fragments outside the window do not exist
        s.hi = std::clamp<i64>(s.hi, 0, ww);
        cols[static_cast<usize>(n)] = s;
    }
    for (i32 m = 0; m < height; ++m) {
        Span s = zoom_span(yr, zy, m);
        s.lo = std::clamp<i64>(s.lo, 0, wh);
        s.hi = std::clamp<i64>(s.hi, 0, wh);
        rows[static_cast<usize>(m)] = s;
    }
    const auto group_at = [&](i32 n, i32 m) { return static_cast<usize>(m) * static_cast<usize>(width) + static_cast<usize>(n); };
    PixelQuads& quads = t_quads;
    quads.clear();

    switch (g.kind) {
    case GlPixelParams::Kind::Image: {
        // The covered window rectangle, one texel per fragment: texel (i, j)
        // is the group whose region holds that fragment's centre.
        i64 x0 = ww, x1 = 0, y0 = wh, y1 = 0;
        for (const Span& s : cols) {
            if (s.hi > s.lo) x0 = std::min(x0, s.lo), x1 = std::max(x1, s.hi);
        }
        for (const Span& s : rows) {
            if (s.hi > s.lo) y0 = std::min(y0, s.lo), y1 = std::max(y1, s.hi);
        }
        if (x1 <= x0 || y1 <= y0) return;
        const u32 cw = static_cast<u32>(x1 - x0), ch = static_cast<u32>(y1 - y0);
        std::vector<i32> col_group(cw, -1), row_group(ch, -1);
        for (i32 n = 0; n < width; ++n) {
            for (i64 i = cols[static_cast<usize>(n)].lo; i < cols[static_cast<usize>(n)].hi; ++i) {
                col_group[static_cast<usize>(i - x0)] = n;
            }
        }
        for (i32 m = 0; m < height; ++m) {
            for (i64 j = rows[static_cast<usize>(m)].lo; j < rows[static_cast<usize>(m)].hi; ++j) {
                row_group[static_cast<usize>(j - y0)] = m;
            }
        }
        const u32 pw = next_pow2(cw), ph = next_pow2(ch);
        std::vector<float> texels(static_cast<usize>(pw) * ph * 4u, 0.0f);
        for (u32 j = 0; j < ch; ++j) {
            if (row_group[j] < 0) continue;
            for (u32 i = 0; i < cw; ++i) {
                if (col_group[i] < 0) continue;
                const GlRgba& v = g.rgba[group_at(col_group[i], row_group[j])];
                float* t = &texels[(static_cast<usize>(j) * pw + i) * 4u];
                // Final conversion (3.6.4): clamped to [0, 1]. The value stays
                // floating point (RGBA32Float) through texturing, fog and
                // blending; the colour buffer quantises what is written.
                for (usize k = 0; k < 4; ++k) t[k] = std::clamp(v[k], 0.0f, 1.0f);
            }
        }
        graphics::GraphicsBackendDevice* device = graphics::active_backend_device();
        if (device == nullptr) return;
        graphics::BackendTexture2DDesc desc{};
        desc.width = pw;
        desc.height = ph;
        desc.mip_levels = 1;
        desc.pixel_format = graphics::PixelFormat::RGBA32Float;
        desc.shader_resource = true;
        std::unique_ptr<graphics::BackendTexture2D> created = device->create_texture_2d(desc, nullptr, 0);
        if (!created || !created->valid() || !created->update(0, texels.data(), pw * 16u)) {
            gl_log_once(g_no_texture_logged, "the backend refused a glDrawPixels image texture; nothing is drawn");
            return;
        }
        GlPixelParams params = raster_params(ctx, GlPixelParams::Kind::Image);
        params.image = std::make_shared<GlTextureStorage>();
        params.image->texture = std::move(created);
        params.image->bytes = texels.size() * sizeof(float);
        auto& u = params.image_unit;
        u.min_filter = GL_NEAREST;
        u.mag_filter = GL_NEAREST;
        u.wrap_s = GL_CLAMP;
        u.wrap_t = GL_CLAMP;
        u.log2_width = log2_exact(pw);
        u.log2_height = log2_exact(ph);
        u.top_level = 0;
        u.dimensions = 2;
        const float tex[4] = {0.0f, 0.0f, static_cast<float>(cw) / static_cast<float>(pw),
                              static_cast<float>(ch) / static_cast<float>(ph)};
        quads.rect(x0, y0, x1, y1, raster_vertex(ctx), tex);
        submit(ctx, params, quads);
        return;
    }
    case GlPixelParams::Kind::Depth: {
        const GlBatchVertex proto = raster_vertex(ctx);
        for (i32 m = 0; m < height; ++m) {
            const Span& r = rows[static_cast<usize>(m)];
            if (r.hi <= r.lo) continue;
            i32 n = 0;
            while (n < width) {
                // A run of groups with the same depth is one quad.
                const float z = g.depth[group_at(n, m)];
                i32 e = n + 1;
                while (e < width && g.depth[group_at(e, m)] == z) ++e;
                i64 lo = std::numeric_limits<i64>::max(), hi = std::numeric_limits<i64>::min();
                for (i32 k = n; k < e; ++k) {
                    const Span& s = cols[static_cast<usize>(k)];
                    if (s.hi > s.lo) lo = std::min(lo, s.lo), hi = std::max(hi, s.hi);
                }
                GlBatchVertex v = proto;
                v.depth_offset[0] = z;
                v.depth_offset[1] = 0.0f;
                quads.rect(lo, r.lo, hi, r.hi, v);
                n = e;
            }
        }
        submit(ctx, raster_params(ctx, GlPixelParams::Kind::Depth), quads);
        return;
    }
    case GlPixelParams::Kind::Stencil: {
        // One draw per stencil value (its reference), in value order; the
        // groups of different values cover disjoint fragments, so the order
        // does not matter.
        std::map<u32, std::vector<std::array<i64, 4>>> by_value;
        for (i32 m = 0; m < height; ++m) {
            const Span& r = rows[static_cast<usize>(m)];
            if (r.hi <= r.lo) continue;
            i32 n = 0;
            while (n < width) {
                const u32 value = g.stencil[group_at(n, m)];
                i32 e = n + 1;
                while (e < width && g.stencil[group_at(e, m)] == value) ++e;
                i64 lo = std::numeric_limits<i64>::max(), hi = std::numeric_limits<i64>::min();
                for (i32 k = n; k < e; ++k) {
                    const Span& s = cols[static_cast<usize>(k)];
                    if (s.hi > s.lo) lo = std::min(lo, s.lo), hi = std::max(hi, s.hi);
                }
                if (hi > lo) by_value[value].push_back({lo, r.lo, hi, r.hi});
                n = e;
            }
        }
        const GlBatchVertex proto = raster_vertex(ctx);
        for (const auto& [value, rects] : by_value) {
            quads.clear();
            for (const auto& q : rects) quads.rect(q[0], q[1], q[2], q[3], proto);
            GlPixelParams params = raster_params(ctx, GlPixelParams::Kind::Stencil);
            params.stencil_value = value;
            submit(ctx, params, quads);
        }
        return;
    }
    case GlPixelParams::Kind::Fragments: return;
    }
}

/// One BGRA8 pixel of a colour buffer as RGBA in [0, 1] (spec 4.3.2
/// "Conversion of RGBA values"): a buffer without alpha bits reads A = 1, for
/// glReadPixels and glCopyPixels alike, whatever the attachment holds.
GlRgba framebuffer_rgba(const GlDrawable& d, const u8* bgra) {
    return {gl_ubyte_to_float(bgra[2]), gl_ubyte_to_float(bgra[1]), gl_ubyte_to_float(bgra[0]),
            d.color_channels() == 0xFu ? gl_ubyte_to_float(bgra[3]) : 1.0f};
}

u32 stencil_mask(const GlContext& ctx) {
    const u32 bits = ctx.framebuffer().stencil_bits;
    return bits >= 32 ? 0xFFFFFFFFu : (1u << bits) - 1u;
}

bool format_valid(u32 format) { return gl_pixel_format_elements(format) != 0; }

}  // namespace

namespace cmd {

void RasterPos(GlContext& ctx, double x, double y, double z, double w) {
    if (ctx.reject_inside_begin_end()) return;
    GlState& s = ctx.state;
    GlCurrentState& c = s.current;
    // Spec 2.12: transformed as a vertex is, then clipped as a point.
    const std::array<double, 4> obj{x, y, z, w};
    const std::array<double, 4> eye = detail::transform_point(ctx.modelview(), obj);
    const std::array<double, 4> clip = detail::transform_point(ctx.projection(), eye);
    const double wc = clip[3];
    bool culled = !(-wc <= clip[0] && clip[0] <= wc && -wc <= clip[1] && clip[1] <= wc && -wc <= clip[2] &&
                    clip[2] <= wc) ||
                  wc == 0.0;   // only (0, 0, 0, 0) passes with w = 0; it has no window position
    for (u32 i = 0; i < kMaxClipPlanes && !culled; ++i) {
        if (!s.enabled(gl_cap_offset(GlCap::ClipPlane0, i))) continue;
        const auto& p = s.transform.clip_planes[i];
        if (p[0] * eye[0] + p[1] * eye[1] + p[2] * eye[2] + p[3] * eye[3] < 0.0) culled = true;
    }
    if (culled) {
        // The position and its data become indeterminate: GDI Generic keeps
        // the last valid values and clears the valid bit
        // (gl11_state_probe64.expected rasterpos-culled-*).
        c.raster_position_valid = false;
        return;
    }
    // Spec 2.10.1: the viewport and depth-range map of the point.
    const GlViewportState& vp = s.viewport;
    const double xd = clip[0] / wc, yd = clip[1] / wc, zd = clip[2] / wc;
    c.raster_position = {static_cast<float>((xd + 1.0) * vp.width / 2.0 + vp.x),
                         static_cast<float>((yd + 1.0) * vp.height / 2.0 + vp.y),
                         static_cast<float>(((vp.depth_far - vp.depth_near) * zd + vp.depth_near + vp.depth_far) / 2.0),
                         static_cast<float>(wc)};
    // The raster distance "can be approximated": GDI Generic stores the eye
    // z (rasterpos-frustum-eye-3-4-neg12: -12; rasterpos-ortho-*: 0.5).
    c.raster_distance = static_cast<float>(eye[3] != 0.0 ? eye[2] / eye[3] : eye[2]);
    // The colour and texture coordinates "just as is done for a vertex".
    GlBatchVertex v{};
    for (usize k = 0; k < 4; ++k) v.pos[k] = static_cast<float>(obj[k]);
    for (usize k = 0; k < 4; ++k) v.color[k] = c.color[k];
    for (usize k = 0; k < 3; ++k) v.normal[k] = c.normal[k];
    for (usize k = 0; k < 4; ++k) v.tex[k] = c.tex_coords[k];
    if (s.enabled(GlCap::Lighting)) {
        c.raster_color = gl_cpu_lit_color(ctx, v);
    } else {
        // Spec 2.13.6: clamped to [0, 1] (rasterpos-color-out-of-range-index5).
        for (usize k = 0; k < 4; ++k) c.raster_color[k] = std::clamp(c.color[k], 0.0f, 1.0f);
    }
    gl_cpu_texgen(ctx, v);
    const std::array<double, 4> t =
        detail::transform_point(ctx.texture_matrix(), {v.tex[0], v.tex[1], v.tex[2], v.tex[3]});
    for (usize k = 0; k < 4; ++k) c.raster_tex_coords[k] = static_cast<float>(t[k]);
    // In RGBA mode the associated colour index keeps its initial value.
    c.raster_position_valid = true;
}

void Bitmap(GlContext& ctx, i32 width, i32 height, float xorig, float yorig, float xmove, float ymove,
            const GlClientPixels& bits) {
    if (ctx.reject_inside_begin_end()) return;
    if (width < 0 || height < 0) {
        ctx.record_error(GL_INVALID_VALUE);
        return;
    }
    GlCurrentState& c = ctx.state.current;
    // Spec 3.7: an invalid raster position ignores the bitmap and is not
    // moved (bitmap-invalid-rasterpos-after-move).
    if (!c.raster_position_valid) return;
    if (width > 0 && height > 0 && ctx.drawable) {
        if (bits.null_pointer || !bits.readable) {
            gl_log_once(g_unreadable_logged, "glBitmap/glDrawPixels client memory is not readable; nothing is drawn");
        } else {
            const GlPixelStoreModes& modes = gl_unpack_modes(ctx, bits);
            const GlPixelLayout layout = gl_pixel_layout(modes, GL_COLOR_INDEX, GL_BITMAP, width, height);
            if (layout.valid && bits.bytes.size() >= layout.span_bytes) {
                // The lower left corner of the fragment array.
                const i64 xll = static_cast<i64>(std::floor(static_cast<double>(c.raster_position[0]) - xorig));
                const i64 yll = static_cast<i64>(std::floor(static_cast<double>(c.raster_position[1]) - yorig));
                i64 ww = 0, wh = 0;
                window_size(ctx, ww, wh);
                const GlBatchVertex proto = raster_vertex(ctx);
                PixelQuads& quads = t_quads;
                quads.clear();
                const auto bit = [&](i32 i, i32 j) { return gl_bitmap_bit(bits.bytes, layout, modes, i, j); };
                // Runs of 1 bits per row; a run repeated in the next row
                // extends its quad upwards.
                struct Run {
                    i32 lo, hi, y0;
                };
                std::vector<Run> open, next;
                const auto emit = [&](const Run& r, i32 y1) {
                    const i64 x0 = std::clamp<i64>(xll + r.lo, 0, ww), x1 = std::clamp<i64>(xll + r.hi, 0, ww);
                    const i64 y0c = std::clamp<i64>(yll + r.y0, 0, wh), y1c = std::clamp<i64>(yll + y1, 0, wh);
                    quads.rect(x0, y0c, x1, y1c, proto);
                };
                for (i32 j = 0; j <= height; ++j) {
                    next.clear();
                    if (j < height) {
                        i32 i = 0;
                        while (i < width) {
                            if (!bit(i, j)) {
                                ++i;
                                continue;
                            }
                            i32 e = i + 1;
                            while (e < width && bit(e, j)) ++e;
                            next.push_back({i, e, j});
                            i = e;
                        }
                    }
                    // Continue the runs of the previous row that repeat.
                    for (Run& n : next) {
                        for (Run& o : open) {
                            if (o.lo == n.lo && o.hi == n.hi && o.y0 >= 0) {
                                n.y0 = o.y0;
                                o.y0 = -1;   // continued
                                break;
                            }
                        }
                    }
                    for (const Run& o : open) {
                        if (o.y0 >= 0) emit(o, j);
                    }
                    std::swap(open, next);
                }
                submit(ctx, raster_params(ctx, GlPixelParams::Kind::Fragments), quads);
            }
        }
    }
    // Then the raster position moves, even for an empty bitmap.
    c.raster_position[0] += xmove;
    c.raster_position[1] += ymove;
}

void DrawPixels(GlContext& ctx, i32 width, i32 height, u32 format, u32 type, const GlClientPixels& pixels) {
    if (ctx.reject_inside_begin_end()) return;
    // Error order as measured (drawpixels-neg-and-bad-format: INVALID_VALUE).
    if (width < 0 || height < 0) {
        ctx.record_error(GL_INVALID_VALUE);
        return;
    }
    if (!format_valid(format) || !gl_pixel_type_valid(type, true) ||
        (type == GL_BITMAP && format != GL_COLOR_INDEX && format != GL_STENCIL_INDEX)) {
        ctx.record_error(GL_INVALID_ENUM);   // drawpixels-rgba-bitmap, drawpixels-depth-bitmap
        return;
    }
    // STENCIL_INDEX without a stencil buffer (Learn, glDrawPixels: "GL_INVALID_
    // OPERATION is generated if format is GL_STENCIL_INDEX and there is no
    // stencil buffer"; not measurable on GDI Generic, whose formats all have
    // one). No source makes DEPTH_COMPONENT without a depth buffer an error:
    // its fragments are drawn, the depth test and depth writes absent (4.1.5).
    if (format == GL_STENCIL_INDEX && ctx.framebuffer().stencil_bits == 0) {
        ctx.record_error(GL_INVALID_OPERATION);
        return;
    }
    // Spec 3.6.5: an invalid raster position ignores the command.
    if (!ctx.state.current.raster_position_valid || width == 0 || height == 0 || !ctx.drawable) return;
    if (pixels.null_pointer || !pixels.readable) {
        gl_log_once(g_unreadable_logged, "glBitmap/glDrawPixels client memory is not readable; nothing is drawn");
        return;
    }
    const GlPixelStoreModes& modes = gl_unpack_modes(ctx, pixels);
    PixelGroups g;
    if (format == GL_DEPTH_COMPONENT) {
        g.kind = GlPixelParams::Kind::Depth;
        gl_unpack_depth(ctx, modes, type, width, height, pixels.bytes, g.depth);
        if (g.depth.size() != static_cast<usize>(width) * static_cast<usize>(height)) {
            ctx.record_error(GL_OUT_OF_MEMORY);   // the groups do not fit the host
            return;
        }
    } else if (format == GL_STENCIL_INDEX) {
        g.kind = GlPixelParams::Kind::Stencil;
        std::vector<double> idx;
        gl_unpack_stencil(ctx, modes, type, width, height, pixels.bytes, idx);
        if (idx.size() != static_cast<usize>(width) * static_cast<usize>(height) ||
            !gl_try_reserve(g.stencil, idx.size())) {
            ctx.record_error(GL_OUT_OF_MEMORY);
            return;
        }
        // Final conversion: masked by 2^s - 1.
        const u32 mask = stencil_mask(ctx);
        for (double v : idx) g.stencil.push_back(static_cast<u32>(static_cast<i64>(std::floor(v))) & mask);
    } else {
        // Colour groups, or colour indices converted to RGBA (3.6.4, RGBA mode).
        g.kind = GlPixelParams::Kind::Image;
        gl_unpack_rgba(ctx, modes, format, type, width, height, pixels.bytes, g.rgba);
        if (g.rgba.size() != static_cast<usize>(width) * static_cast<usize>(height)) {
            ctx.record_error(GL_OUT_OF_MEMORY);
            return;
        }
    }
    draw_groups(ctx, g, width, height);
}

void CopyPixels(GlContext& ctx, i32 x, i32 y, i32 width, i32 height, u32 type) {
    if (ctx.reject_inside_begin_end()) return;
    if (width < 0 || height < 0) {
        ctx.record_error(GL_INVALID_VALUE);
        return;
    }
    if (type != GL_COLOR && type != GL_DEPTH && type != GL_STENCIL) {
        ctx.record_error(GL_INVALID_ENUM);
        return;
    }
    // Spec 4.3.3: read as glReadPixels would, so a missing buffer is its
    // INVALID_OPERATION (4.3.2; Learn, glCopyPixels: "if type is GL_DEPTH and
    // there is no depth buffer", likewise GL_STENCIL).
    if ((type == GL_DEPTH && ctx.framebuffer().depth_bits == 0) ||
        (type == GL_STENCIL && ctx.framebuffer().stencil_bits == 0)) {
        ctx.record_error(GL_INVALID_OPERATION);
        return;
    }
    if (!ctx.state.current.raster_position_valid || width == 0 || height == 0 || !ctx.drawable) return;
    GlDrawable& d = *ctx.drawable;
    // The whole source is read first (the reads end the pass after recording
    // what is pending), so an overlapping destination sees the old pixels.
    const i64 dw = d.width(), dh = d.height();
    const usize count = static_cast<usize>(width) * static_cast<usize>(height);
    if (count > gl_working_limit() / sizeof(GlRgba)) {
        ctx.record_error(GL_OUT_OF_MEMORY);   // gl_client_memory.hpp rule 3
        return;
    }
    const auto inside = [&](i32 i, i32 j, usize& at) {
        const i64 wx = static_cast<i64>(x) + i, wy = static_cast<i64>(y) + j;
        if (wx < 0 || wy < 0 || wx >= dw || wy >= dh) return false;
        at = static_cast<usize>(dh - 1 - wy) * static_cast<usize>(dw) + static_cast<usize>(wx);   // rows top-down
        return true;
    };
    PixelGroups g;
    if (type == GL_COLOR) {
        g.kind = GlPixelParams::Kind::Image;
        std::vector<u8> bgra;
        if (!d.read_color(ctx.state.pixel_mode.read_buffer, bgra)) return;
        // Pixels outside the window are undefined: they read as 0.
        if (!gl_try_assign(g.rgba, count, GlRgba{0, 0, 0, 0})) {
            ctx.record_error(GL_OUT_OF_MEMORY);
            return;
        }
        for (i32 j = 0; j < height; ++j) {
            for (i32 i = 0; i < width; ++i) {
                usize at = 0;
                GlRgba& out = g.rgba[static_cast<usize>(j) * static_cast<usize>(width) + static_cast<usize>(i)];
                if (inside(i, j, at)) out = framebuffer_rgba(d, &bgra[at * 4u]);
                gl_transfer_rgba(ctx, out);
            }
        }
    } else if (type == GL_DEPTH) {
        g.kind = GlPixelParams::Kind::Depth;
        std::vector<float> depth;
        if (!d.read_depth(depth)) return;
        if (!gl_try_assign(g.depth, count, 0.0f)) {
            ctx.record_error(GL_OUT_OF_MEMORY);
            return;
        }
        for (i32 j = 0; j < height; ++j) {
            for (i32 i = 0; i < width; ++i) {
                usize at = 0;
                const float z = inside(i, j, at) ? depth[at] : 0.0f;
                g.depth[static_cast<usize>(j) * static_cast<usize>(width) + static_cast<usize>(i)] =
                    std::clamp(gl_transfer_depth(ctx, z), 0.0f, 1.0f);
            }
        }
    } else {
        g.kind = GlPixelParams::Kind::Stencil;
        std::vector<u8> stencil;
        if (!d.read_stencil(stencil)) return;
        const u32 mask = stencil_mask(ctx);
        if (!gl_try_assign(g.stencil, count, 0u)) {
            ctx.record_error(GL_OUT_OF_MEMORY);
            return;
        }
        for (i32 j = 0; j < height; ++j) {
            for (i32 i = 0; i < width; ++i) {
                usize at = 0;
                const double v = inside(i, j, at) ? stencil[at] : 0.0;
                g.stencil[static_cast<usize>(j) * static_cast<usize>(width) + static_cast<usize>(i)] =
                    static_cast<u32>(static_cast<i64>(std::floor(gl_transfer_stencil(ctx, v, true)))) & mask;
            }
        }
    }
    draw_groups(ctx, g, width, height);
}

bool ReadPixels(GlContext& ctx, i32 x, i32 y, i32 width, i32 height, u32 format, u32 type, GlClientBuffer client,
                void* user) {
    if (ctx.reject_inside_begin_end()) return false;
    // Error order as measured (readpixels-neg-and-bad-format: INVALID_VALUE;
    // readpixels-bad-format-and-type: INVALID_ENUM).
    if (width < 0 || height < 0) {
        ctx.record_error(GL_INVALID_VALUE);
        return false;
    }
    if (!format_valid(format) || !gl_pixel_type_valid(type, true)) {
        ctx.record_error(GL_INVALID_ENUM);
        return false;
    }
    // Spec 4.3.2: COLOR_INDEX in RGBA mode. A BITMAP type with any format but
    // STENCIL_INDEX is INVALID_OPERATION on GDI Generic (err-bad-operation-
    // glReadPixels-bitmap-type, readpixels-depth-bitmap), not the spec's enum.
    if (format == GL_COLOR_INDEX || (type == GL_BITMAP && format != GL_STENCIL_INDEX) ||
        (format == GL_DEPTH_COMPONENT && ctx.framebuffer().depth_bits == 0) ||
        (format == GL_STENCIL_INDEX && ctx.framebuffer().stencil_bits == 0)) {
        ctx.record_error(GL_INVALID_OPERATION);
        return false;
    }
    if (width == 0 || height == 0 || !ctx.drawable) return false;
    const GlPixelStoreModes& modes = ctx.state.pixel_store.pack;
    const GlPixelLayout layout = gl_pixel_layout(modes, format, type, width, height);
    if (!layout.valid) {
        // No address range can hold the image: Windows faults in the caller.
        gl_log_once(g_unreadable_logged, "glReadPixels client memory is not accessible; nothing is written");
        return false;
    }
    GlDrawable& d = *ctx.drawable;
    const i64 dw = d.width(), dh = d.height();
    // Pixels outside the window are undefined (4.3.2): their client bytes
    // are left as they are (readpixels-outside-*). Only the rectangle's part
    // inside the window is read and packed, so the work is bounded by the
    // window, whatever width and height are (final review, Finding 3).
    const i64 x0 = std::max<i64>(x, 0), y0 = std::max<i64>(y, 0);
    const i64 x1 = std::min<i64>(i64{x} + width, dw), y1 = std::min<i64>(i64{y} + height, dh);
    if (x1 <= x0 || y1 <= y0) return false;   // nothing inside the window: nothing written
    const i32 rw = static_cast<i32>(x1 - x0), rh = static_cast<i32>(y1 - y0);
    GlPixelStoreModes region{};
    usize offset = 0;
    if (!gl_pixel_region_layout(modes, format, type, width, height, static_cast<i32>(x0 - x), static_cast<i32>(y0 - y),
                                region, offset)) {
        return false;
    }
    const usize count = static_cast<usize>(rw) * static_cast<usize>(rh);
    // Framebuffer row wy is buffer row dh - 1 - wy (top-down).
    const auto source = [&](i32 i, i32 j) {
        return static_cast<usize>(dh - 1 - (y0 + j)) * static_cast<usize>(dw) + static_cast<usize>(x0 + i);
    };
    if (format == GL_STENCIL_INDEX) {
        std::vector<u8> stencil;
        if (!d.read_stencil(stencil)) return false;
        std::vector<double> idx(count, 0.0);
        for (i32 j = 0; j < rh; ++j) {
            for (i32 i = 0; i < rw; ++i) {
                idx[static_cast<usize>(j) * static_cast<usize>(rw) + static_cast<usize>(i)] =
                    gl_transfer_stencil(ctx, stencil[source(i, j)], true);
            }
        }
        const std::span<u8> out = client(user, layout.span_bytes);
        if (out.size() < layout.span_bytes) return false;
        gl_pack_indices(region, type, rw, rh, idx, out.subspan(offset));
        return true;
    }
    std::vector<GlRgba> groups(count, GlRgba{0, 0, 0, 0});
    if (format == GL_DEPTH_COMPONENT) {
        std::vector<float> depth;
        if (!d.read_depth(depth)) return false;
        for (i32 j = 0; j < rh; ++j) {
            for (i32 i = 0; i < rw; ++i) {
                groups[static_cast<usize>(j) * static_cast<usize>(rw) + static_cast<usize>(i)][0] =
                    gl_transfer_depth(ctx, depth[source(i, j)]);
            }
        }
    } else {
        std::vector<u8> bgra;
        if (!d.read_color(ctx.state.pixel_mode.read_buffer, bgra)) return false;
        for (i32 j = 0; j < rh; ++j) {
            for (i32 i = 0; i < rw; ++i) {
                // The buffer's fixed-point components as [0, 1] values; a
                // buffer without alpha reads A = 1 (the drawable keeps it at 1).
                GlRgba c = framebuffer_rgba(d, &bgra[source(i, j) * 4u]);
                gl_transfer_rgba(ctx, c);
                groups[static_cast<usize>(j) * static_cast<usize>(rw) + static_cast<usize>(i)] = c;
            }
        }
    }
    const std::span<u8> out = client(user, layout.span_bytes);
    if (out.size() < layout.span_bytes) return false;
    gl_pack_rgba(region, format, type, rw, rh, groups, out.subspan(offset));
    return true;
}

}  // namespace cmd

// ---- Exports ---------------------------------------------------------------------------

namespace {

using namespace cmd;

/// The client bytes an unpacking pixel command reads (eq. 3.8-3.10 under the
/// UNPACK_* modes); nothing is read for arguments the command rejects.
GlClientPixels client_image(GlCall& c, GuestAddr addr, u32 format, u32 type, i32 w, i32 h, std::vector<u8>& storage) {
    if (w <= 0 || h <= 0 || !gl_pixel_type_valid(type, true) || gl_pixel_format_elements(format) == 0) {
        GlClientPixels px;
        px.unpack = &c.ctx.state.pixel_store.unpack;
        return px;
    }
    return gl_client_pixels(c, addr, gl_pixel_layout(c.ctx.state.pixel_store.unpack, format, type, w, h), storage);
}

template <typename T, usize N, bool V>
void raster_pos_cmd(GlCall& c) {
    // Not allowed inside Begin/End (spec 2.6.3): checked before reading; a
    // compiled call is checked when it executes (spec 5.4).
    if (!c.recording_list() && c.ctx.reject_inside_begin_end()) return;
    std::array<T, 4> raw{};
    if constexpr (V) {
        if (c.p(0) == 0 || c.m.memory().read_bytes(c.p(0), reinterpret_cast<Byte*>(raw.data()), N * sizeof(T)).is_err()) {
            gl_log_once(g_unreadable_logged, "glRasterPos*v client memory is not readable; the command is ignored");
            return;
        }
    } else {
        for (usize k = 0; k < N; ++k) raw[k] = gl_arg_as<T>(c, k);
    }
    std::array<double, 4> v{0, 0, 0, 1};
    for (usize k = 0; k < N; ++k) v[k] = static_cast<double>(raw[k]);
    gl_issue<&RasterPos>(c, v[0], v[1], v[2], v[3]);
}

void bitmap_cmd(GlCall& c) {
    if (!c.recording_list() && c.ctx.reject_inside_begin_end()) return;
    thread_local std::vector<u8> storage;   // no allocation per glyph once grown
    const GlClientPixels px = client_image(c, c.p(6), GL_COLOR_INDEX, GL_BITMAP, c.i(0), c.i(1), storage);
    gl_issue<&Bitmap>(c, c.i(0), c.i(1), c.f(2), c.f(3), c.f(4), c.f(5), px);
    gl_trim_scratch(storage);   // a large image does not stay with the thread
}

void draw_pixels_cmd(GlCall& c) {
    if (!c.recording_list() && c.ctx.reject_inside_begin_end()) return;
    thread_local std::vector<u8> storage;   // no allocation per glyph once grown
    const GlClientPixels px = client_image(c, c.p(4), c.u(2), c.u(3), c.i(0), c.i(1), storage);
    gl_issue<&DrawPixels>(c, c.i(0), c.i(1), c.u(2), c.u(3), px);
    gl_trim_scratch(storage);
}

void read_pixels_cmd(GlCall& c) {
    GlClientOutput out{&c, c.p(6), {}};
    const GlClientBuffer client = [](void* user, usize size) -> std::span<u8> {
        const std::span<u8> bytes = GlClientOutput::open(user, size);
        if (bytes.size() < size) {
            gl_log_once(g_unreadable_logged, "glReadPixels client memory is not accessible; nothing is written");
        }
        return bytes;
    };
    if (!ReadPixels(c.ctx, c.i(0), c.i(1), c.i(2), c.i(3), c.u(4), c.u(5), client, &out)) return;
    out.write_back();
}

// clang-format off
const GlHandlerEntry kPixelHandlers[] = {
    {"glRasterPos2d", &raster_pos_cmd<double, 2, false>}, {"glRasterPos2dv", &raster_pos_cmd<double, 2, true>},
    {"glRasterPos2f", &raster_pos_cmd<float, 2, false>},  {"glRasterPos2fv", &raster_pos_cmd<float, 2, true>},
    {"glRasterPos2i", &raster_pos_cmd<i32, 2, false>},    {"glRasterPos2iv", &raster_pos_cmd<i32, 2, true>},
    {"glRasterPos2s", &raster_pos_cmd<i16, 2, false>},    {"glRasterPos2sv", &raster_pos_cmd<i16, 2, true>},
    {"glRasterPos3d", &raster_pos_cmd<double, 3, false>}, {"glRasterPos3dv", &raster_pos_cmd<double, 3, true>},
    {"glRasterPos3f", &raster_pos_cmd<float, 3, false>},  {"glRasterPos3fv", &raster_pos_cmd<float, 3, true>},
    {"glRasterPos3i", &raster_pos_cmd<i32, 3, false>},    {"glRasterPos3iv", &raster_pos_cmd<i32, 3, true>},
    {"glRasterPos3s", &raster_pos_cmd<i16, 3, false>},    {"glRasterPos3sv", &raster_pos_cmd<i16, 3, true>},
    {"glRasterPos4d", &raster_pos_cmd<double, 4, false>}, {"glRasterPos4dv", &raster_pos_cmd<double, 4, true>},
    {"glRasterPos4f", &raster_pos_cmd<float, 4, false>},  {"glRasterPos4fv", &raster_pos_cmd<float, 4, true>},
    {"glRasterPos4i", &raster_pos_cmd<i32, 4, false>},    {"glRasterPos4iv", &raster_pos_cmd<i32, 4, true>},
    {"glRasterPos4s", &raster_pos_cmd<i16, 4, false>},    {"glRasterPos4sv", &raster_pos_cmd<i16, 4, true>},
    {"glBitmap", &bitmap_cmd},
    {"glDrawPixels", &draw_pixels_cmd},
    {"glCopyPixels", [](GlCall& c) { gl_issue<&CopyPixels>(c, c.i(0), c.i(1), c.i(2), c.i(3), c.u(4)); }},
    {"glReadPixels", &read_pixels_cmd},
};
// clang-format on

}  // namespace

std::span<const GlHandlerEntry> gl_pixel_export_handlers() { return kPixelHandlers; }

}  // namespace vitreo::winapi::gl
