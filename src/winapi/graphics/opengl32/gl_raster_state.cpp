// SPDX-License-Identifier: LGPL-2.1-only
/// @file gl_raster_state.cpp
/// @brief GL rasterization / per-fragment state to backend state
///        (gl_raster_state.hpp).

#include "winapi/graphics/opengl32/gl_raster_state.hpp"

#include "winapi/graphics/opengl32/gl_context.hpp"
#include "winapi/graphics/opengl32/gl_drawable.hpp"
#include "winapi/graphics/opengl32/gl_enums.hpp"

#include <algorithm>
#include <cmath>

namespace vitreo::winapi::gl {

namespace {

using graphics::BackendBlendFactor;
using graphics::BackendCompare;
using graphics::BackendStencilOperation;

u64 mix(u64 h, u64 v) noexcept {
    h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
    return h;
}

}  // namespace

std::optional<BackendBlendFactor> gl_blend_factor(u32 factor, bool source) noexcept {
    switch (factor) {
    case GL_ZERO: return BackendBlendFactor::Zero;
    case GL_ONE: return BackendBlendFactor::One;
    case GL_SRC_ALPHA: return BackendBlendFactor::SourceAlpha;
    case GL_ONE_MINUS_SRC_ALPHA: return BackendBlendFactor::OneMinusSourceAlpha;
    case GL_DST_ALPHA: return BackendBlendFactor::DestinationAlpha;
    case GL_ONE_MINUS_DST_ALPHA: return BackendBlendFactor::OneMinusDestinationAlpha;
    // Table 4.1 only.
    case GL_DST_COLOR: return source ? std::optional(BackendBlendFactor::DestinationColor) : std::nullopt;
    case GL_ONE_MINUS_DST_COLOR:
        return source ? std::optional(BackendBlendFactor::OneMinusDestinationColor) : std::nullopt;
    // (f, f, f, 1), f = min(A_s, 1 - A_d).
    case GL_SRC_ALPHA_SATURATE:
        return source ? std::optional(BackendBlendFactor::SourceAlphaSaturated) : std::nullopt;
    // Table 4.2 only.
    case GL_SRC_COLOR: return source ? std::nullopt : std::optional(BackendBlendFactor::SourceColor);
    case GL_ONE_MINUS_SRC_COLOR:
        return source ? std::nullopt : std::optional(BackendBlendFactor::OneMinusSourceColor);
    default: return std::nullopt;
    }
}

BackendCompare gl_compare(u32 func) noexcept {
    switch (func) {
    case GL_NEVER: return BackendCompare::Never;
    case GL_LESS: return BackendCompare::Less;
    case GL_EQUAL: return BackendCompare::Equal;
    case GL_LEQUAL: return BackendCompare::LessEqual;
    case GL_GREATER: return BackendCompare::Greater;
    case GL_NOTEQUAL: return BackendCompare::NotEqual;
    case GL_GEQUAL: return BackendCompare::GreaterEqual;
    default: return BackendCompare::Always;
    }
}

BackendStencilOperation gl_stencil_op(u32 op) noexcept {
    switch (op) {
    case GL_ZERO: return BackendStencilOperation::Zero;
    case GL_REPLACE: return BackendStencilOperation::Replace;
    // "values clamp at 0 and the maximum representable value" (4.1.4).
    case GL_INCR: return BackendStencilOperation::IncrementClamp;
    case GL_DECR: return BackendStencilOperation::DecrementClamp;
    case GL_INVERT: return BackendStencilOperation::Invert;
    default: return BackendStencilOperation::Keep;
    }
}

graphics::FFLogicOp gl_logic_op(u32 op) noexcept {
    if (op < GL_CLEAR || op > GL_SET) return graphics::FFLogicOp::Copy;
    return static_cast<graphics::FFLogicOp>(op - GL_CLEAR);
}

bool gl_front_counter_clockwise(u32 front_face) noexcept { return front_face == GL_CCW; }

double gl_depth_resolution(graphics::PixelFormat depth_format) noexcept {
    switch (depth_format) {
    case graphics::PixelFormat::D16Unorm: return 1.0 / 65535.0;
    case graphics::PixelFormat::Unknown: return 0.0;
    default: return std::ldexp(1.0, -24);   // D32Float(_S8Uint)
    }
}

u64 GlRasterState::pipeline_key() const noexcept {
    u64 h = mix(0x51ED270B27B0FA6Bull, blend ? 1u : 0u);
    if (blend) {
        h = mix(h, static_cast<u64>(source));
        h = mix(h, static_cast<u64>(destination));
    }
    return mix(h, color_write_mask);
}

void GlRasterState::apply(graphics::BackendPipelineDesc& desc, u32 color_count) const {
    desc.blend_enabled = blend;
    desc.source_blend = desc.source_blend_alpha = source;
    desc.destination_blend = desc.destination_blend_alpha = destination;
    desc.blend_operation = desc.blend_operation_alpha = graphics::BackendBlendOperation::Add;
    desc.independent_blend_enabled = false;   // one GL blend function for every buffer (4.1.6)
    desc.color_write_mask = color_write_mask;
    desc.color_write_masks.fill(color_count != 0 ? color_write_mask : 0u);
}

void GlRasterState::apply(graphics::BackendDrawCall& call) const {
    graphics::apply_depth_stencil_desc(call, depth_stencil);
    call.stencil_reference = stencil_reference;
    call.cull_mode = cull;
    call.front_counter_clockwise = front_counter_clockwise;
    call.scissor_enabled = scissor;
    call.scissor = scissor_rect;
}

bool GlRasterState::operator==(const GlRasterState& o) const noexcept {
    return blend == o.blend && source == o.source && destination == o.destination &&
           color_write_mask == o.color_write_mask && graphics::same_depth_stencil_desc(depth_stencil, o.depth_stencil) &&
           stencil_reference == o.stencil_reference && cull == o.cull &&
           front_counter_clockwise == o.front_counter_clockwise && scissor == o.scissor &&
           scissor_rect == o.scissor_rect;
}

void gl_raster_state(const GlState& s, const GlPassLayout& layout, const GlDrawable& drawable, u32 channels,
                     GlRasterState& out) {
    out = GlRasterState{};
    // 4.2.2: the colour write mask; a buffer without alpha never stores it.
    const auto& m = s.color_buffer.color_writemask;
    out.color_write_mask = ((m[0] ? 1u : 0u) | (m[1] ? 2u : 0u) | (m[2] ? 4u : 0u) | (m[3] ? 8u : 0u)) & channels;

    // 4.1.6: blending, bypassed while the colour logic op is on (4.1.8).
    if (s.enabled(GlCap::Blend) && !s.enabled(GlCap::ColorLogicOp)) {
        const auto src = gl_blend_factor(s.color_buffer.blend_src, true);
        const auto dst = gl_blend_factor(s.color_buffer.blend_dst, false);
        // glBlendFunc stores only valid tokens (INVALID_ENUM otherwise).
        out.blend = true;
        out.source = src.value_or(BackendBlendFactor::One);
        out.destination = dst.value_or(BackendBlendFactor::Zero);
    }

    // 4.1.5: without a depth buffer the test always passes; disabled, the
    // comparison and the depth update are bypassed.
    const bool has_depth = layout.depth_format != graphics::PixelFormat::Unknown;
    if (has_depth && s.enabled(GlCap::DepthTest)) {
        out.depth_stencil.depth_test_enabled = true;
        out.depth_stencil.depth_compare = gl_compare(s.depth_buffer.func);
        out.depth_stencil.depth_write_enabled = s.depth_buffer.writemask;
    }
    // 4.1.4: without a stencil buffer the test always passes and nothing is
    // modified. ref is clamped to [0, 2^s - 1]; the masks' s low bits apply.
    if (layout.has_stencil && s.enabled(GlCap::StencilTest)) {
        constexpr u32 kStencilMax = 0xFF;   // the 8-bit stencil of every format with one
        const auto& st = s.stencil_buffer;
        out.depth_stencil.stencil_test_enabled = true;
        out.depth_stencil.stencil_read_mask = static_cast<u8>(st.value_mask & kStencilMax);
        out.depth_stencil.stencil_write_mask = static_cast<u8>(st.writemask & kStencilMax);
        graphics::BackendStencilFaceDesc face{};
        face.compare = gl_compare(st.func);
        face.stencil_fail = gl_stencil_op(st.fail);
        face.depth_fail = gl_stencil_op(st.pass_depth_fail);
        face.pass = gl_stencil_op(st.pass_depth_pass);
        out.depth_stencil.front_stencil = out.depth_stencil.back_stencil = face;
        out.stencil_reference = static_cast<u32>(std::clamp<i64>(st.ref, 0, kStencilMax));
    }

    // 3.5.1: culling of filled polygons (FRONT_AND_BACK culls every polygon;
    // the caller drops them, the backend cannot).
    out.front_counter_clockwise = gl_front_counter_clockwise(s.polygon.front_face);
    if (s.enabled(GlCap::CullFace)) {
        out.cull = s.polygon.cull_face_mode == GL_FRONT  ? graphics::BackendCullMode::Front
                   : s.polygon.cull_face_mode == GL_BACK ? graphics::BackendCullMode::Back
                                                         : graphics::BackendCullMode::None;
    }

    // 4.1.2: the scissor box, in window coordinates (y up).
    if (s.enabled(GlCap::ScissorTest)) {
        out.scissor = true;
        out.scissor_rect = drawable.window_rect_to_target(s.scissor.x, s.scissor.y, s.scissor.width, s.scissor.height);
    }
}

}  // namespace vitreo::winapi::gl
