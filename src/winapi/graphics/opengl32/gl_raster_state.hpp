#pragma once
/// @file gl_raster_state.hpp
/// @brief GL 1.1 rasterization and per-fragment state (spec 3.5, 4.1) as
///        backend state: blend factors and colour write mask for the
///        BackendPipelineDesc; depth, stencil, cull, winding and scissor for
///        the BackendDrawCall; the polygon-offset resolution r of the actual
///        depth format; the logic op.
///
/// Facing. GL decides front/back by the sign of the window-coordinate area
/// (eq. 2.6, y up) and glFrontFace. The drawable maps GL window y up onto the
/// backend's top-left target by placing the viewport (viewport_to_target),
/// which does not mirror NDC, and Metal's winding follows NDC's orientation
/// (y up): measured by GlRasterTest.CullFaceAndFrontFace, both windings and
/// every cull mode, and by its two-sided lighting half ([[front_facing]]).
/// So front_counter_clockwise = (glFrontFace == GL_CCW).

#include <vitreo/common.hpp>
#include "graphics/contracts/backend_draw_call.hpp"
#include "graphics/contracts/backend_pipeline.hpp"
#include "graphics/shaders/ff_emulation.hpp"

#include <array>
#include <optional>

namespace vitreo::winapi::gl {

struct GlState;
struct GlPassLayout;
class GlDrawable;

/// The GL state that becomes backend raster state, for one batch.
struct GlRasterState {
    // ---- Pipeline (pipeline_key) ----
    bool blend{false};
    graphics::BackendBlendFactor source{graphics::BackendBlendFactor::One};
    graphics::BackendBlendFactor destination{graphics::BackendBlendFactor::Zero};
    u32 color_write_mask{0xF};   ///< bit 0 red .. bit 3 alpha

    // ---- Draw call ----
    graphics::BackendDepthStencilDesc depth_stencil{};
    u32 stencil_reference{0};
    graphics::BackendCullMode cull{graphics::BackendCullMode::None};
    bool front_counter_clockwise{false};
    bool scissor{false};
    std::array<int, 4> scissor_rect{};   ///< target left, top, right, bottom

    [[nodiscard]] u64 pipeline_key() const noexcept;
    void apply(graphics::BackendPipelineDesc& desc, u32 color_count) const;
    void apply(graphics::BackendDrawCall& call) const;
    [[nodiscard]] bool operator==(const GlRasterState& o) const noexcept;
};

/// Table 4.1 (source) / Table 4.2 (destination): the backend factor of a
/// glBlendFunc token, nullopt for a token not valid on that side.
[[nodiscard]] std::optional<graphics::BackendBlendFactor> gl_blend_factor(u32 factor, bool source) noexcept;
/// glDepthFunc / glStencilFunc / glAlphaFunc tokens (spec 4.1.3-4.1.5).
[[nodiscard]] graphics::BackendCompare gl_compare(u32 func) noexcept;
/// glStencilOp tokens (spec 4.1.4): INCR / DECR clamp.
[[nodiscard]] graphics::BackendStencilOperation gl_stencil_op(u32 op) noexcept;
/// glLogicOp token (Table 4.5) as the generator's logic op.
[[nodiscard]] graphics::FFLogicOp gl_logic_op(u32 op) noexcept;
/// The backend winding of a GL front face (see the file comment).
[[nodiscard]] bool gl_front_counter_clockwise(u32 front_face) noexcept;

/// Spec 3.5.5's r for @p depth_format: the smallest window-z difference
/// guaranteed to stay distinct in that buffer at every depth. D16Unorm:
/// one step, 1 / 65535. D32Float (the 24-bit formats' attachment): 2^-24,
/// the spacing of adjacent floats in [0.5, 1), the widest in [0, 1]. 0 for
/// no depth buffer.
[[nodiscard]] double gl_depth_resolution(graphics::PixelFormat depth_format) noexcept;

/// Fills @p out from @p s for a draw into @p layout on @p drawable (the
/// scissor box is mapped to target coordinates by it). @p channels is the
/// drawable's colour channels (no alpha channel: alpha is never written).
/// The cull mode is that of filled polygons drawn by the backend; a caller
/// that culled on the CPU overrides it.
void gl_raster_state(const GlState& s, const GlPassLayout& layout, const GlDrawable& drawable, u32 channels,
                     GlRasterState& out);

}  // namespace vitreo::winapi::gl
