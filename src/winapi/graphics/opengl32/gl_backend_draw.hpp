#pragma once
/// @file gl_backend_draw.hpp
/// @brief GL primitives to the shared backend (Design 1): the GL state as an
///        FFPipelineState + uniform payload + GlRasterState, the per-context
///        streaming vertex and index buffers, the pipeline cache and the
///        batching of consecutive primitives into one draw inside the
///        drawable's open render pass.
///
/// Batching. A context appends assembled primitives to ONE pending draw. A
/// primitive whose GlBatchKey draws the same way (same pipeline, viewport,
/// draw buffer, raster state and uniform bytes: GlBatchKey::same_draw)
/// merges into it; any other key records the pending draw into the open pass
/// first and starts a new one. A state change therefore never ends the pass.
/// The pending draw is recorded at the latest when the drawable needs it
/// (GlPendingBatch): when its pass ends (SwapBuffers, glFlush, glFinish, a
/// readback, a draw-buffer change, a resize, release) and before a glClear.
///
/// Vertices and indices go into per-context streaming BackendBuffers at
/// increasing offsets (BufferWrite::NoOverwrite; a wrap to offset 0 is a
/// Discard), uploaded once per recorded draw. Pipelines are cached per
/// context by state_hash combined with the raster key and the pass layout.
///
/// Rasterization (Task 9, Design 6). Filled polygons go to the backend as
/// triangles: it culls them (glCullFace / glFrontFace), and GL's per-fragment
/// state maps onto the pipeline and draw (gl_raster_state.hpp). Points and
/// line segments are expanded on the CPU into window-aligned triangles
/// (GlPrimitiveExpander) that cover exactly the fragments of the spec's
/// aliased rules, whatever their size. When glPolygonMode draws a face as
/// GL_LINE or GL_POINT, the facing of each polygon is decided on the CPU
/// (eq. 2.6 on its clip coordinates), culled polygons are dropped there, and
/// the boundary edges / vertices are expanded the same way. Expanded
/// primitives are lit per vertex on the CPU (spec 2.13; the polygon's face
/// colour under two-sided lighting) and draw with a whole-target viewport,
/// so their fragments outside the GL viewport survive (3.3, 3.4.2).
/// Antialiased points, lines and
/// polygons (GL_*_SMOOTH) are drawn aliased after a one-time UNIMPLEMENTED.
/// Texturing (spec 3.8, gl_texture.hpp): the bound texture of the enabled
/// dimensionality is stage 0, sampled by the generator's GL sampler
/// (gl_texture_sampling) with the texture environment as its operations;
/// texture coordinates are the vertices' (s, t, r, q), replaced by glTexGen
/// (in the shader for filled polygons, on the CPU per vertex for expanded
/// primitives, as their lighting is), then the texture matrix.

#include <vitreo/common.hpp>
#include "graphics/shaders/ff_emulation.hpp"
#include "graphics/shaders/ff_uniforms.hpp"
#include "winapi/graphics/opengl32/gl_assembler.hpp"
#include "winapi/graphics/opengl32/gl_raster_state.hpp"

#include <array>
#include <memory>
#include <span>
#include <vector>

namespace vitreo::graphics {
class BackendTexture2D;
}

namespace vitreo::winapi::gl {

class GlContext;
struct GlDrawTexture;
struct GlTextureStorage;

/// How a batch's primitives reach the backend: filled polygons, or one of
/// the CPU expansions (real points and lines; polygon edges and vertices
/// under glPolygonMode).
enum class GlRasterClass : u8 { Fill, Point, Line, PolygonPoint, PolygonLine, Pixel };

/// Pixel rectangles (glBitmap, glDrawPixels, glCopyPixels; spec 3.6.5, 3.7,
/// 4.3.1, Design 7): fragments at window positions the caller computed on the
/// CPU, drawn as window-aligned quads whose corners lie on pixel edges, so the
/// backend produces exactly the covered fragment centres. Every vertex has
/// object position (0, 0, 0, 1) and its window coordinates in window_offset;
/// the key's transform puts that point at window (0, 0) with window depth
/// `window_z` and eye coordinates (0, 0, -|eye_z|, 1), so fog (3.9) sees the
/// raster position's eye distance. The viewport is the whole target (pixel
/// fragments are not clipped to the GL viewport), no lighting, texgen, user
/// clip planes, stipple or polygon offset apply, and nothing is culled.
struct GlPixelParams {
    enum class Kind : u8 {
        /// glBitmap: the fragments carry the raster colour (vertex colour) and
        /// texture coordinates (vertex tex, already divided by q), and go
        /// through texturing and every per-fragment operation.
        Fragments,
        /// glDrawPixels / glCopyPixels of colour groups: the group colour comes
        /// from @p image (stage 0, replacing the fragment colour; NEAREST, one
        /// texel per fragment). With texturing enabled the bound texture is
        /// stage 1, its texture environment applied to that colour, sampled at
        /// @p raster_tex (spec 3.6.5: the raster position's coordinates, the
        /// same for every fragment, so magnification). Then fog and the
        /// per-fragment operations.
        Image,
        /// glDrawPixels / glCopyPixels of DEPTH_COMPONENT: as Fragments, with
        /// each fragment's window depth the vertex depth_offset.x (the group's
        /// depth) instead of window_z.
        Depth,
        /// glDrawPixels / glCopyPixels of STENCIL_INDEX (spec 4.3.1): only the
        /// pixel ownership and scissor tests apply, and @p stencil_value is
        /// written under the stencil write mask; nothing else changes.
        Stencil,
    };
    Kind kind{Kind::Fragments};
    float window_z{0.0f};        ///< the raster position's z_w
    /// Kind::Image: the raster texture coordinates (s, t, r, q), undivided.
    std::array<float, 4> raster_tex{0.0f, 0.0f, 0.0f, 1.0f};
    float eye_z{0.0f};           ///< the raster distance (fog)
    std::shared_ptr<GlTextureStorage> image;   ///< Kind::Image
    graphics::FFUniformInputs::GlTextureUnit image_unit{};
    u32 stencil_value{0};        ///< Kind::Stencil, already masked to the stencil bits
};

/// Everything that decides how a batch of primitives is drawn. Built from the
/// context state by gl_batch_key.
struct GlBatchKey {
    graphics::FFPipelineState ff;
    u64 ff_hash{0};                       ///< ff.hash(): what changes the generated MSL
    u64 raster_key{0};                    ///< raster.pipeline_key(): pipeline state outside the generator
    GlRasterClass raster_class{GlRasterClass::Fill};
    /// Always Triangles once a group is assembled: points and lines arrive
    /// expanded into triangles.
    GlPrimitiveClass primitive{GlPrimitiveClass::None};
    u32 draw_buffer{0};                   ///< GL_DRAW_BUFFER
    std::array<float, 6> viewport{};      ///< backend viewport (GlDrawable::viewport_to_target)
    GlRasterState raster;                 ///< blend, masks, depth/stencil, cull, scissor
    std::vector<u8> vertex_uniforms;      ///< pack_ff_vertex_uniforms
    std::vector<u8> fragment_uniforms;    ///< pack_ff_fragment_uniforms
    /// The texture stage 0 samples (null: untextured). Held until the draw
    /// is recorded; the backend retains it from then on.
    std::shared_ptr<GlTextureStorage> texture;
    /// The texture stage 1 samples: the bound texture of a colour pixel
    /// rectangle, whose stage 0 is the image (GlPixelParams::Kind::Image).
    std::shared_ptr<GlTextureStorage> texture1;

    /// Whether primitives with @p other can share this key's draw.
    [[nodiscard]] bool same_draw(const GlBatchKey& other) const noexcept;
};

/// Fills @p out for a batch of @p raster_class from the context state
/// (reusing its vectors' storage). @p cpu_culled: the caller has already
/// dropped culled polygons (glPolygonMode classification), so the backend
/// must not cull. False when nothing can be drawn: no drawable, an empty
/// viewport, or a logic op without framebuffer fetch (refused loudly).
bool gl_batch_key(GlContext& ctx, GlRasterClass raster_class, bool cpu_culled, GlBatchKey& out);
/// The key of a pixel-rectangle draw (GlRasterClass::Pixel, @p pixel's kind).
bool gl_pixel_batch_key(GlContext& ctx, const GlPixelParams& pixel, GlBatchKey& out);

/// Spec 2.13 evaluated on the CPU for one vertex (its object position, normal
/// and colour; the front colour), as the expanded primitives are lit: the
/// raster position's colour when lighting is enabled (2.12).
[[nodiscard]] std::array<float, 4> gl_cpu_lit_color(const GlContext& ctx, const GlBatchVertex& v);
/// Spec 2.10.4 evaluated on the CPU for one vertex: replaces the enabled
/// texture coordinates of @p v by the generated ones (no texture matrix).
void gl_cpu_texgen(const GlContext& ctx, GlBatchVertex& v);

/// Appends one group (@p indices into @p vertices, a triangle list,
/// provoking vertex first) to the context's pending draw, recording the
/// pending draw first when @p key does not match it. Positions go to the
/// shader as homogeneous (x, y, z, w), clipped in clip space with their own
/// w (spec 2.11), so w = 0 and mixed-sign primitives draw as the spec
/// defines. False when nothing was appended.
bool gl_submit_batch(GlContext& ctx, const GlBatchKey& key, std::span<const GlBatchVertex> vertices,
                     std::span<const u32> indices);

/// glEnd / glDrawArrays / glDrawElements / glRect: assembles @p vertices as
/// primitive @p mode, rasterises it as the GL state says (see the file
/// comment) and submits it. Does nothing without a drawable.
void gl_draw_primitives(GlContext& ctx, u32 mode, std::span<const GlBatchVertex> vertices,
                        std::span<const u8> edge_flags);

/// Records the context's pending draw into its drawable's pass now.
void gl_flush_batch(GlContext& ctx);

/// Diagnostics (and Review Focus 1's test).
struct GlBatchStats {
    u64 pipelines_created{0};   ///< create_pipeline calls (cache misses)
    u64 draws_recorded{0};      ///< BackendDrawCalls recorded into a pass
};
[[nodiscard]] GlBatchStats gl_batch_stats(const GlContext& ctx);

/// What the pass a batch draws into offers the generator state.
struct GlTargetInfo {
    u32 color_targets{1};    ///< colour attachments (0: GL_NONE)
    bool depth{false};       ///< a depth buffer exists
};

/// The fixed-function generator's state for a GL batch of @p raster_class:
/// transforms, vertex colour, shade model, lighting / material / light model
/// (spec 2.13), fog (3.9), alpha test (4.1.3), line and polygon stipple
/// (3.4.2, 3.5.2), polygon offset (3.5.5), user clip planes (2.11), logic
/// op (4.1.8), texturing and texture coordinate generation (3.8, 2.10.4;
/// @p texture: what gl_texture_for_draw resolved, null for untextured) and
/// the expansion inputs.
void gl_ff_state(const GlContext& ctx, GlRasterClass raster_class, const GlTargetInfo& target,
                 graphics::FFPipelineState& ff, const GlDrawTexture* texture = nullptr);

}  // namespace vitreo::winapi::gl
