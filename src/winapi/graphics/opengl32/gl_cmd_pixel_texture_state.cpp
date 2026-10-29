// SPDX-License-Identifier: LGPL-2.1-only
/// @file gl_cmd_pixel_texture_state.cpp
/// @brief Pixel store / transfer / zoom / map state (OpenGL 1.1 spec 3.6).
///        The texture environment, texture coordinate generation and texture
///        object state are gl_cmd_texture.cpp's.

#include "gl_cmd_internal.hpp"

namespace vitreo::winapi::gl::cmd {

using detail::as_enum;
using detail::clamp01;
using detail::color_component;
using detail::round_to_int;

// ---- Pixel store (spec 3.6.1, Table 3.1) ------------------------------------

void PixelStore(GlContext& ctx, u32 pname, double value, bool from_int) {
    (void)from_int;
    if (ctx.reject_inside_begin_end()) return;
    GlPixelStoreState& ps = ctx.state.pixel_store;
    GlPixelStoreModes* modes = nullptr;
    u32 base = 0;
    if (pname >= GL_UNPACK_SWAP_BYTES && pname <= GL_UNPACK_ALIGNMENT) {
        modes = &ps.unpack;
        base = GL_UNPACK_SWAP_BYTES;
    } else if (pname >= GL_PACK_SWAP_BYTES && pname <= GL_PACK_ALIGNMENT) {
        modes = &ps.pack;
        base = GL_PACK_SWAP_BYTES;
    } else {
        ctx.record_error(GL_INVALID_ENUM);
        return;
    }
    // Booleans take "value != 0"; integers round to nearest (spec 3.6.1).
    const i32 iv = round_to_int(value);
    switch (pname - base) {
    case 0: modes->swap_bytes = value != 0; break;
    case 1: modes->lsb_first = value != 0; break;
    case 2: case 3: case 4:
        if (iv < 0) { ctx.record_error(GL_INVALID_VALUE); return; }   // err-bad-value-glPixelStorei-neg-rowlength
        (pname - base == 2 ? modes->row_length : pname - base == 3 ? modes->skip_rows : modes->skip_pixels) = iv;
        break;
    case 5:
        if (iv != 1 && iv != 2 && iv != 4 && iv != 8) {             // err-bad-value-glPixelStorei-alignment3
            ctx.record_error(GL_INVALID_VALUE);
            return;
        }
        modes->alignment = iv;
        break;
    default: ctx.record_error(GL_INVALID_ENUM); break;
    }
}

// ---- Pixel transfer (spec 3.6.3, Table 3.2) ----------------------------------

void PixelTransfer(GlContext& ctx, u32 pname, double value, bool from_int) {
    (void)from_int;
    if (ctx.reject_inside_begin_end()) return;
    GlPixelModeState& p = ctx.state.pixel_mode;
    const float fv = static_cast<float>(value);
    switch (pname) {
    case GL_MAP_COLOR: p.map_color = value != 0; break;
    case GL_MAP_STENCIL: p.map_stencil = value != 0; break;
    case GL_INDEX_SHIFT: p.index_shift = round_to_int(value); break;
    case GL_INDEX_OFFSET: p.index_offset = round_to_int(value); break;
    case GL_RED_SCALE: p.scale[0] = fv; break;
    case GL_GREEN_SCALE: p.scale[1] = fv; break;
    case GL_BLUE_SCALE: p.scale[2] = fv; break;
    case GL_ALPHA_SCALE: p.scale[3] = fv; break;
    case GL_RED_BIAS: p.bias[0] = fv; break;
    case GL_GREEN_BIAS: p.bias[1] = fv; break;
    case GL_BLUE_BIAS: p.bias[2] = fv; break;
    case GL_ALPHA_BIAS: p.bias[3] = fv; break;
    case GL_DEPTH_SCALE: p.depth_scale = fv; break;
    case GL_DEPTH_BIAS: p.depth_bias = fv; break;
    default: ctx.record_error(GL_INVALID_ENUM); break;   // err-glPixelTransferf-bad-enum
    }
}

void PixelZoom(GlContext& ctx, float xfactor, float yfactor) {
    if (ctx.reject_inside_begin_end()) return;
    ctx.state.pixel_mode.zoom_x = xfactor;
    ctx.state.pixel_mode.zoom_y = yfactor;
}

// ---- Pixel maps (spec 3.6.3, Table 3.3) ----------------------------------------

namespace {
bool is_pixel_map(u32 map) { return map >= GL_PIXEL_MAP_I_TO_I && map <= GL_PIXEL_MAP_A_TO_A; }
}  // namespace

bool gl_pixel_map_is_index_valued(u32 map) noexcept {
    return map == GL_PIXEL_MAP_I_TO_I || map == GL_PIXEL_MAP_S_TO_S;
}

void PixelMap(GlContext& ctx, u32 map, i32 size, std::span<const float> values) {
    if (ctx.reject_inside_begin_end()) return;
    if (!is_pixel_map(map)) { ctx.record_error(GL_INVALID_ENUM); return; }
    if (size < 1 || static_cast<u32>(size) > kMaxPixelMapTable) {
        ctx.record_error(GL_INVALID_VALUE);   // err-glPixelMapfv-bad-size
        return;
    }
    // Maps addressed by an index (I_TO_x, S_TO_S) need a power-of-two size.
    const bool index_addressed = map <= GL_PIXEL_MAP_I_TO_A;
    if (index_addressed && (size & (size - 1)) != 0) {
        ctx.record_error(GL_INVALID_VALUE);   // err-glPixelMapfv-nonpow2-itoi
        return;
    }
    if (values.size() < static_cast<usize>(size)) return;   // unreadable client memory
    auto& stored = ctx.pixel_maps[map - GL_PIXEL_MAP_I_TO_I];
    stored.assign(values.begin(), values.begin() + size);
    // A stencil index entry is rounded to the nearest integer when specified.
    if (map == GL_PIXEL_MAP_S_TO_S) {
        for (float& v : stored) v = static_cast<float>(round_to_int(v));
    }
}

const std::vector<float>* GetPixelMap(GlContext& ctx, u32 map) {
    if (ctx.reject_inside_begin_end()) return nullptr;
    if (!is_pixel_map(map)) { ctx.record_error(GL_INVALID_ENUM); return nullptr; }
    return &ctx.pixel_maps[map - GL_PIXEL_MAP_I_TO_I];
}

}  // namespace vitreo::winapi::gl::cmd
