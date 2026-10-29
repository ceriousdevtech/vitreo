// SPDX-License-Identifier: LGPL-2.1-only

/// @file d3d11_context_clear_view.cpp
/// @brief ID3D11DeviceContext1::ClearView.
///
/// clear_view for render-target, unordered-access and depth-stencil
/// views, and clear_depth_stencil_view_rects, its depth-stencil half.
/// The other view clears are in d3d11_context_clear.cpp.
///
/// Split out of d3d11_device_ext.cpp.

#include "winapi/graphics/d3d11/d3d11.hpp"
#include "winapi/graphics/d3d11/d3d11_internal.hpp"
#include "winapi/graphics/d3d11/d3d11_texel_encode.hpp"
#include "winapi/core/marshaler.hpp"
#include "graphics/contracts/backend_device.hpp"
#include "core/memory/guest_memory.hpp"

namespace vitreo::winapi {

// -----------------------------------------------------------------------------
// ClearView
// -----------------------------------------------------------------------------

void D3D11Manager::clear_view(Marshaler& m, GuestAddr self) {
    // ClearView(pView, Color[4], pRect, NumRects). D3D11.3 5.2.3.3 and Learn
    // (ID3D11DeviceContext1::ClearView): render-target, unordered-access and
    // depth-stencil views; rects are [left, right) x [top, bottom), clamped
    // to the surface -- measured -- and apply to every array slice of the
    // view; no list clears the whole view, but a NULL pRect with a non-zero
    // NumRects is dropped (measured on RTV, UAV and DSV, d3d11_capsmall_probe
    // "NULL rects count 2"). The colour converts into the view format: *NORM
    // clamped, integer formats "as integral floats ... fractions rounded to
    // zero, out of range/INF values clamped to target range, NaN to 0". On a
    // buffer view a rect's left/right are an element range.
    (void)self;
    const GuestAddr view = m.arg_ptr(1);
    const GuestAddr color_address = m.arg_ptr(2);
    const GuestAddr rects = m.arg_ptr(3);
    const u32 rect_count = static_cast<u32>(m.arg(4));
    m.set_return(0);
    if (rects == 0 && rect_count != 0) return;
    std::array<float, 4> color{};
    if (color_address == 0 ||
        m_memory.read_bytes(color_address, reinterpret_cast<Byte*>(color.data()),
                            sizeof(color)).is_err()) {
        return;
    }
    const bool is_rtv = m_render_target_views.contains(view);
    const bool is_uav = m_unordered_access_views.contains(view);
    const bool is_dsv = m_depth_stencil_views.contains(view);
    if (!is_rtv && !is_uav && !is_dsv) return;  // a video view, or not a view

    // Clamped to the non-negative quadrant; the fill clamps the far edges.
    std::vector<std::array<u32, 4>> boxes;
    for (u32 i = 0; rects != 0 && i < rect_count; ++i) {
        std::array<i32, 4> r{};
        if (m_memory.read_bytes(rects + i * 16, reinterpret_cast<Byte*>(r.data()),
                                sizeof(r)).is_err()) {
            return;
        }
        boxes.push_back({static_cast<u32>(std::max(r[0], 0)),
                         static_cast<u32>(std::max(r[1], 0)),
                         static_cast<u32>(std::max(r[2], 0)),
                         static_cast<u32>(std::max(r[3], 0))});
    }
    // A list was given: only its rects are cleared, and one of only empty
    // or inverted rects clears nothing (measured) -- never the whole view.
    const bool had_rects = !boxes.empty();

    if (is_dsv) {
        clear_depth_stencil_view_rects(m_depth_stencil_views.at(view), color,
                                       boxes, had_rects);
        return;
    }
    if (is_rtv && boxes.empty()) {
        // The whole view: ClearRenderTargetView, whose (view, color)
        // arguments sit in the same positions. Its integer conversion
        // truncates too (a06 probe: CLV RGBA8_UINT whole).
        impl_ContextMethod(m, 50);
        return;
    }

    GuestAddr texture_address = 0;
    GuestAddr buffer_address = 0;
    u32 format = 0;
    u32 mip = 0;
    u32 first_slice = 0;
    u32 slice_count = 1;
    u32 first_element = 0;
    u32 num_elements = 0;
    bool typed_buffer_view = true;
    if (is_rtv) {
        const auto& rtv = m_render_target_views.at(view);
        texture_address = rtv.texture;
        // A buffer view is a NumElements x 1 target: a rect clears the
        // elements of its x range when it covers row 0.
        buffer_address = rtv.buffer;
        first_element = rtv.first_element;
        num_elements = rtv.num_elements;
        if (buffer_address != 0) {
            std::erase_if(boxes, [](const std::array<u32, 4>& r) {
                return r[1] != 0 || r[3] == 0;
            });
            if (boxes.empty()) return;
        }
        format = rtv.format;
        mip = rtv.mip_slice;
        first_slice = rtv.first_array_slice;
        slice_count = rtv.array_size;
    } else {
        const auto& uav = m_unordered_access_views.at(view);
        texture_address = uav.texture;
        buffer_address = uav.buffer;
        first_element = uav.first_element;
        num_elements = uav.num_elements;
        typed_buffer_view =
            (uav.buffer_flags & d3d11::BUFFER_UAV_FLAG_RAW) == 0;
        format = uav.format;
        mip = uav.mip_slice;
        first_slice = uav.first_array_slice;
        slice_count = uav.array_size;
    }
    auto texture = m_textures.find(texture_address);
    if (format == 0 && texture != m_textures.end()) {
        format = texture->second.desc.format;
    }
    const auto* layout = d3d11_texel::layout_for(format);
    if (layout == nullptr) {
        d3d11_unimplemented_once("ClearView",
                                 "view format " + std::to_string(format));
        return;
    }
    d3d11_texel::TexelBytes texel{};
    (void)d3d11_texel::encode_float(*layout, color,
                                    d3d11_texel::FloatClear::ClearView, texel);

    if (buffer_address != 0) {
        auto buffer = m_buffers.find(buffer_address);
        if (buffer == m_buffers.end() || !typed_buffer_view ||
            (buffer->second.desc.misc_flags &
             D3D11_RESOURCE_MISC_BUFFER_STRUCTURED) != 0) {
            return;  // raw and structured views have no format to convert to
        }
        const usize element = layout->texel_bytes;
        std::vector<std::array<u32, 4>> ranges = boxes;
        if (ranges.empty()) ranges.push_back({0, 0, num_elements, 1});
        for (const auto& r : ranges) {
            const u32 left = std::min(r[0], num_elements);
            const u32 right = std::min(r[2], num_elements);
            if (left >= right) continue;
            const usize begin =
                (static_cast<usize>(first_element) + left) * element;
            const usize span = static_cast<usize>(right - left) * element;
            const usize width = buffer->second.desc.byte_width;
            if (begin > width || span > width - begin) continue;
            const std::span<const u8> one_texel(texel.data(), element);
            // A tiled buffer keeps no shadow (d3d11_tiled.cpp).
            if (buffer->second.shadow.size() >= begin + span) {
                replicate_element(buffer->second.shadow.data() + begin, span,
                                  one_texel);
                ++buffer->second.shadow_version;
            }
            (void)fill_buffer_on_gpu(buffer->second, begin, one_texel, span);
        }
        return;
    }
    if (texture == m_textures.end()) return;
    if (texture->second.volume) {
        // Learn: ClearView takes Texture2D views; a volume view's rects have
        // no defined meaning to follow.
        if (is_uav && boxes.empty()) {
            const auto& uav = m_unordered_access_views.at(view);
            (void)fill_texture_view_texels(texture->second, texel.data(),
                                           layout->texel_bytes, mip,
                                           uav.first_w_slice, uav.w_size,
                                           nullptr, "ClearView");
        } else {
            d3d11_unimplemented_once("ClearView",
                                     "rectangles on a Texture3D view");
        }
        return;
    }
    if (boxes.empty()) {
        (void)fill_texture_view_texels(texture->second, texel.data(),
                                       layout->texel_bytes, mip, first_slice,
                                       slice_count, nullptr, "ClearView");
        return;
    }
    for (const auto& r : boxes) {
        (void)fill_texture_view_texels(texture->second, texel.data(),
                                       layout->texel_bytes, mip, first_slice,
                                       slice_count, &r, "ClearView");
    }
}

void D3D11Manager::clear_depth_stencil_view_rects(
    const DepthStencilView& dsv, const std::array<float, 4>& color,
    const std::vector<std::array<u32, 4>>& boxes, bool had_rects) {
    // ClearView on a depth-stencil view (ClearViewAlsoSupportsDepthOnlyFormats;
    // Learn names "DSVs on depth-only resources", and WARP clears the depth
    // AND stencil planes of D24S8 / D32S8 views too). Measured
    // (d3d11_capsmall_probe "clearview"), whatever the view's read-only flags
    // or binding:
    // - depth = Color[0] converted to the view format: D16 / D24 as FLOAT ->
    //   UNORM (clamped, NaN 0, round to nearest even in float: 0.3 -> 0x4CCC
    //   / 0x4CCCCC); D32 stored as given -- not clamped (1.5, -0.5, +-Inf,
    //   NaN, -0 kept), denormals flushed to a zero of the same sign;
    // - stencil = Color[1] as UINT8: round to nearest (7.9 -> 8, 0.5 -> 0),
    //   clamped (300 -> 255, -3 -> 0).
    auto texture = m_textures.find(dsv.texture);
    if (texture == m_textures.end() || !texture->second.backend) return;
    graphics::BackendTexture2D& backend = *texture->second.backend;
    const u32 format = dsv.format != 0 ? dsv.format : texture->second.desc.format;
    const float c0 = color[0];
    u32 depth_bits = 0;
    float pass_value = 0.0f;  // what a render-pass clear stores for it
    bool pass_clearable = true;
    bool has_stencil = false;
    switch (format) {
    case 55: {  // D16_UNORM
        const u32 code = d3d11_texel::float_to_unorm(c0, 16);
        depth_bits = code;
        // As ClearDepthStencilView: the code's value nudged a quarter step up,
        // so Metal's truncating Depth16Unorm clear lands on it.
        pass_value = static_cast<float>(std::min(1.0, (code + 0.25) / 65535.0));
        break;
    }
    case 45: {  // D24_UNORM_S8_UINT: a 32-bit float plane on this host
        const u32 code = d3d11_texel::float_to_unorm(c0, 24);
        pass_value = static_cast<float>(static_cast<double>(code) / 16777215.0);
        std::memcpy(&depth_bits, &pass_value, sizeof(depth_bits));
        has_stencil = true;
        break;
    }
    case 40:    // D32_FLOAT
    case 20: {  // D32_FLOAT_S8X24_UINT
        std::memcpy(&depth_bits, &c0, sizeof(depth_bits));
        if ((depth_bits & 0x7F800000u) == 0) depth_bits &= 0x80000000u;
        std::memcpy(&pass_value, &depth_bits, sizeof(pass_value));
        pass_clearable = pass_value >= 0.0f && pass_value <= 1.0f &&
                         (depth_bits & 0x80000000u) == 0;
        has_stencil = format == 20;
        break;
    }
    default:
        return;
    }
    const float c1 = color[1];
    const u8 stencil = std::isnan(c1)
        ? u8{0}
        : static_cast<u8>(std::clamp(std::nearbyint(c1), 0.0f, 255.0f));

    const bool multisampled = texture->second.desc.sample_count > 1;
    if (!had_rects && pass_clearable) {
        (void)backend.clear_depth_stencil_aspects(
            pass_value, stencil, true, has_stencil, dsv.mip_slice,
            dsv.first_array_slice, dsv.array_size);
        return;
    }
    if (multisampled) {
        d3d11_unimplemented_once(
            "ClearView",
            "rectangles, or a depth outside [0, 1], on a multisampled "
            "depth-stencil view");
        return;
    }
    const auto& level = backend.desc();
    const u32 w = std::max(1u, level.width >> dsv.mip_slice);
    const u32 h = std::max(1u, level.height >> dsv.mip_slice);
    std::vector<std::array<u32, 4>> regions = boxes;
    if (!had_rects) regions.push_back({0, 0, w, h});
    for (const auto& r : regions) {
        const u32 left = std::min(r[0], w);
        const u32 top = std::min(r[1], h);
        const u32 right = std::min(r[2], w);
        const u32 bottom = std::min(r[3], h);
        if (left >= right || top >= bottom) continue;
        if (!backend.fill_depth_stencil_texels(
                depth_bits, has_stencil, stencil, dsv.mip_slice,
                dsv.first_array_slice, dsv.array_size, left, top,
                right - left, bottom - top)) {
            d3d11_unimplemented_once(
                "ClearView", "depth-stencil view of DXGI format " +
                                 std::to_string(format) +
                                 " in this backend's storage");
            return;
        }
    }
}

}  // namespace vitreo::winapi
