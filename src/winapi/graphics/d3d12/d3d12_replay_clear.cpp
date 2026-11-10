// SPDX-License-Identifier: LGPL-2.1-only
/// @file d3d12_replay_clear.cpp
/// @brief Render-target, depth-stencil and unordered-access clears.
///        Split out of d3d12_replay.cpp as verbatim code motion.

#include "winapi/graphics/d3d12/d3d12_replay_internal.hpp"
#include "winapi/graphics/d3d11/d3d11_texel_encode.hpp"
#include "graphics/contracts/format_converter.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace vitreo::winapi::d3d12 {

namespace {

[[nodiscard]] u32 storage_texel_bytes(const Resource& r);

/// The level extent of @p r at @p mip.
void level_extent(const Resource& r, u32 mip, u32& width, u32& height) {
    width = static_cast<u32>(std::max<u64>(1, r.desc.width >> mip));
    height = r.desc.dimension == D3D12_RESOURCE_DIMENSION_TEXTURE1D
                 ? 1u
                 : std::max(1u, r.desc.height >> mip);
}

/// One rectangle of @p rects, or the whole @p w x @p h level when empty.
template <typename Fill>
bool for_each_rect(const std::vector<Rect>& rects, u32 w, u32 h, Fill&& fill) {
    if (rects.empty()) return fill(0u, 0u, w, h);
    bool ok = true;
    for (const Rect& rect : rects) {
        const u32 x = static_cast<u32>(std::clamp<i32>(rect.left, 0, static_cast<i32>(w)));
        const u32 y = static_cast<u32>(std::clamp<i32>(rect.top, 0, static_cast<i32>(h)));
        const u32 x1 = static_cast<u32>(std::clamp<i32>(rect.right, 0, static_cast<i32>(w)));
        const u32 y1 = static_cast<u32>(std::clamp<i32>(rect.bottom, 0, static_cast<i32>(h)));
        if (x1 <= x || y1 <= y) continue;  // nothing inside the surface
        ok = fill(x, y, x1 - x, y1 - y) && ok;
    }
    return ok;
}

/// Fill @p rects (all of the level when empty) of one mip of @p r with
/// @p texel: [left, right) x [top, bottom), clamped to the level. @p plane
/// is a chroma or depth plane the backend already stores apart from the
/// resource's own texel, when that is what the view addresses.
enum class StoreResult { Ok, Mismatch, Refused };

StoreResult store_encoded_texels(Resource& r, const u8* texel, u32 texel_bytes,
                                 u32 mip, u32 first_slice, u32 slices,
                                 const std::vector<Rect>& rects, u32 plane) {
    if (r.texture3d) {
        if (storage_texel_bytes(r) != texel_bytes) return StoreResult::Mismatch;
        u32 w = 0;
        u32 h = 0;
        level_extent(r, mip, w, h);
        const u32 depth =
            std::max(1u, static_cast<u32>(r.desc.depth_or_array_size) >> mip);
        const u32 first = std::min(first_slice, depth);
        const u32 count = std::min(slices, depth - first);
        // D3D12_RECT is the XY window; the view's W range is the depth.
        // fill_texels takes that box.
        const bool ok = for_each_rect(rects, w, h, [&](u32 x, u32 y, u32 rw, u32 rh) {
            return r.texture3d->fill_texels(texel, texel_bytes, mip, x, y, first,
                                            rw, rh, count);
        });
        return ok ? StoreResult::Ok : StoreResult::Refused;
    }
    if (!r.texture) return StoreResult::Refused;
    graphics::BackendTexture2D* target = r.texture.get();
    const graphics::FormatInfo resource_info =
        graphics::format_table(target->desc().pixel_format);
    if (graphics::is_planar_format(target->desc().pixel_format) &&
        resource_info.bytes_per_pixel != 0 &&
        texel_bytes == 2u * resource_info.bytes_per_pixel) {
        // A chroma view of NV12 / P010 (R8G8 / R16G16 / R32_UINT) is twice
        // the luma texel and fills the half-size chroma plane (the D3D11
        // clear path, d3d11_context_clear.cpp).
        graphics::BackendTexture2D* chroma = target->view_plane(
            graphics::planar_chroma_format(target->desc().pixel_format));
        if (chroma != nullptr && chroma != target) target = chroma;
    }
    const graphics::FormatInfo stored =
        graphics::format_table(target->desc().pixel_format);
    const bool depth_resource =
        resource_info.is_depth && target == r.texture.get();
    if (depth_resource) {
        // format_table reports 5 bytes for a combined depth+stencil format;
        // the backend keeps a 4-byte depth plane and a 1-byte stencil plane
        // and fills each (fill_depth_stencil / fill_depth_stencil_texels).
        const bool combined = resource_info.bytes_per_pixel == 5;
        const u32 depth_bytes = combined ? 4u : resource_info.bytes_per_pixel;
        const bool stencil = plane == 1 && combined && texel_bytes == 1;
        const bool depth = plane == 0 && texel_bytes == depth_bytes &&
                           (depth_bytes == 2 || depth_bytes == 4);
        if (!stencil && !depth) return StoreResult::Mismatch;
        const u32 w = std::max(1u, target->desc().width >> mip);
        const u32 h = std::max(1u, target->desc().height >> mip);
        u32 depth_bits = 0;
        if (depth) std::memcpy(&depth_bits, texel, texel_bytes);
        const u8 stencil_byte = stencil ? texel[0] : 0;
        const bool ok = for_each_rect(
            rects, w, h, [&](u32 x, u32 y, u32 rw, u32 rh) {
                return stencil
                           ? target->fill_depth_stencil(
                                 true, 0.0f, stencil_byte, mip, first_slice,
                                 slices, x, y, rw, rh)
                           : target->fill_depth_stencil_texels(
                                 depth_bits, false, 0, mip, first_slice, slices,
                                 x, y, rw, rh);
            });
        return ok ? StoreResult::Ok : StoreResult::Refused;
    }
    if (stored.bytes_per_pixel != texel_bytes) return StoreResult::Mismatch;
    const u32 w = std::max(1u, target->desc().width >> mip);
    const u32 h = std::max(1u, target->desc().height >> mip);
    const bool ok = for_each_rect(rects, w, h, [&](u32 x, u32 y, u32 rw, u32 rh) {
        return target->fill_texels(texel, texel_bytes, mip, first_slice, slices,
                                   x, y, rw, rh);
    });
    return ok ? StoreResult::Ok : StoreResult::Refused;
}

/// Texel bytes the backend stores @p r in (0: not a single-texel format).
u32 storage_texel_bytes(const Resource& r) {
    const graphics::PixelFormat format =
        r.texture3d ? r.texture3d->desc().pixel_format
        : r.texture ? r.texture->desc().pixel_format
                    : graphics::PixelFormat::Unknown;
    return graphics::format_table(format).bytes_per_pixel;
}

}  // namespace

void Replayer::clear_render_target(const cmd::ClearRenderTarget& c) {
    Resource* r = resource(c.rtv.resource, c.rtv.resource_serial);
    if (r == nullptr) return;
    // D3D11.3 5.2.3.1 / D3D12 ClearRenderTargetView: the colour converted to
    // the view's format by the 3.2 rules (float16/11/10 round toward zero,
    // *NORM and sRGB to nearest even) -- the backend is handed the exact
    // stored value, or, where a colour cannot express the bits (rectangles,
    // an 11/10-bit NaN), the encoded texel is stored directly.
    const u32 view_format = c.rtv.format != 0 ? c.rtv.format : r->desc.format;
    const auto* layout = d3d11_texel::layout_for(view_format);
    // A float32 channel stores a denormal clear value as zero of its sign
    // (measured, a05 clear "RGBA32F {0.1, -0.0, 1e-40, 65504}": 00000000).
    std::array<float, 4> requested = c.color;
    if (layout != nullptr) {
        for (u8 i = 0; i < layout->channel_count; ++i) {
            const auto& ch = layout->channels[i];
            float& v = requested[ch.component];
            if (ch.kind == d3d11_texel::ChannelKind::Float && ch.bits == 32 &&
                std::fpclassify(v) == FP_SUBNORMAL) {
                v = std::signbit(v) ? -0.0f : 0.0f;
            }
        }
    }
    bool store_texel = !c.rects.empty();
    if (layout != nullptr) {
        for (u8 i = 0; i < layout->channel_count; ++i) {
            const auto& ch = layout->channels[i];
            store_texel = store_texel ||
                          (ch.kind == d3d11_texel::ChannelKind::Float &&
                           ch.bits < 16 && std::isnan(requested[ch.component]));
        }
    }
    const u32 slices = r->texture3d
                           ? (c.rtv.array_size == 0xFFFFFFFFu ? 0xFFFFFFFFu
                                                              : c.rtv.array_size)
                           : std::max(1u, c.rtv.array_size);
    if (store_texel) {
        d3d11_texel::TexelBytes texel{};
        if (layout == nullptr ||
            !d3d11_texel::encode_float(*layout, requested,
                                       d3d11_texel::FloatClear::ClearView,
                                       texel) ||
            store_encoded_texels(*r, texel.data(), layout->texel_bytes,
                                 c.rtv.most_detailed_mip, c.rtv.first_slice,
                                 slices, c.rects, c.rtv.plane_slice) !=
                StoreResult::Ok) {
            unimplemented("ClearRenderTargetView",
                          "clear rectangles / an encoded texel of this format");
        }
        return;
    }
    const std::array<float, 4> color =
        layout != nullptr ? d3d11_texel::quantize_render_target_color(*layout, requested)
                          : requested;
    if (r->texture3d) {
        (void)r->texture3d->clear_render_target_slices(
            color, c.rtv.most_detailed_mip, c.rtv.first_slice,
            c.rtv.array_size);
        return;
    }
    if (!r->texture) return;
    const auto format = graphics::pixel_format_from_dxgi(c.rtv.format);
    (void)r->texture->clear_render_target_view(
        color,
        format != r->texture->desc().pixel_format ? format
                                                  : graphics::PixelFormat::Unknown,
        c.rtv.most_detailed_mip, c.rtv.first_slice,
        std::max(1u, c.rtv.array_size));
}

void Replayer::clear_depth_stencil(const cmd::ClearDepthStencil& c) {
    Resource* r = resource(c.dsv.resource, c.dsv.resource_serial);
    if (r == nullptr || !r->texture) return;
    if (!c.rects.empty()) {
        // Learn (ClearDepthStencilView): Depth is clamped to [0, 1]; a D24
        // plane stores it as UNORM24 (nearest even, as the a05 clears
        // measured), which the float storage must hold exactly.
        float depth = std::clamp(c.depth, 0.0f, 1.0f);
        if (is_d24_family(r->desc.format)) {
            depth = static_cast<float>(std::nearbyint(depth * 16777215.0f) /
                                       16777215.0);
        }
        const u32 mip = c.dsv.most_detailed_mip;
        const u32 slices = std::max(1u, c.dsv.array_size);
        u32 w = 0;
        u32 h = 0;
        level_extent(*r, mip, w, h);
        bool ok = true;
        for (const Rect& rect : c.rects) {
            const auto clamp_to = [](i32 v, u32 limit) {
                return static_cast<u32>(std::clamp<i32>(v, 0, static_cast<i32>(limit)));
            };
            const u32 x = clamp_to(rect.left, w);
            const u32 y = clamp_to(rect.top, h);
            const u32 x1 = clamp_to(rect.right, w);
            const u32 y1 = clamp_to(rect.bottom, h);
            if (x1 <= x || y1 <= y) continue;  // nothing inside the surface
            if ((c.flags & D3D12_CLEAR_FLAG_DEPTH) != 0) {
                ok = r->texture->fill_depth_stencil(false, depth, 0, mip,
                                                    c.dsv.first_slice, slices,
                                                    x, y, x1 - x, y1 - y) &&
                     ok;
            }
            if ((c.flags & D3D12_CLEAR_FLAG_STENCIL) != 0) {
                ok = r->texture->fill_depth_stencil(true, 0.0f, c.stencil, mip,
                                                    c.dsv.first_slice, slices,
                                                    x, y, x1 - x, y1 - y) &&
                     ok;
            }
        }
        if (!ok) {
            unimplemented("ClearDepthStencilView",
                          "clear rectangles on this depth format / an MSAA "
                          "depth buffer");
        }
        return;
    }
    (void)r->texture->clear_depth_stencil_aspects(
        c.depth, c.stencil, (c.flags & D3D12_CLEAR_FLAG_DEPTH) != 0,
        (c.flags & D3D12_CLEAR_FLAG_STENCIL) != 0, c.dsv.most_detailed_mip,
        c.dsv.first_slice, std::max(1u, c.dsv.array_size));
}

void Replayer::clear_unordered_access(const cmd::ClearUnorderedAccess& c) {
    Resource* r = resource(c.uav.resource, c.uav.resource_serial);
    if (r == nullptr) return;
    const char* method = c.is_float ? "ClearUnorderedAccessViewFloat"
                                    : "ClearUnorderedAccessViewUint";
    std::array<float, 4> color{};
    std::memcpy(color.data(), c.values.data(), sizeof(color));
    if (r->feedback_map) {
        // DirectX-Specs "Sampler Feedback": ClearUnorderedAccessViewUint
        // clears a feedback map to "no mips requested"; the four values are
        // ignored, and so are rectangles. Only the Uint form is specified.
        if (c.is_float) {
            unimplemented(method, "a sampler feedback map (the spec clears "
                                  "them with ClearUnorderedAccessViewUint)");
            return;
        }
        graphics::BackendBuffer* buffer = r->buffer();
        if (buffer == nullptr) return;
        const u64 begin = r->offset + graphics::sampler_feedback::kHeaderWords * 4u;
        const u64 span = static_cast<u64>(r->feedback.words_per_slice) * r->feedback.slices * 4u;
        if (span == 0) return;
        const std::vector<u8> zeros(static_cast<usize>(span), 0);
        graphics::BackendBufferDesc desc{};
        desc.byte_width = static_cast<usize>(span);
        auto source = f.m_backend.create_buffer(desc, zeros.data());
        if (!source || !source->valid() ||
            !f.m_backend.copy_buffer_region(buffer, begin, source.get(), 0, span)) {
            unimplemented(method, "a feedback map clear the backend refused");
        }
        return;
    }
    if (r->is_buffer()) {
        graphics::BackendBuffer* buffer = r->buffer();
        if (buffer == nullptr) return;
        const bool raw = (c.uav.flags & D3D12_BUFFER_UAV_FLAG_RAW) != 0;
        std::vector<u8> element;
        u64 element_bytes = 0;
        if (raw || c.uav.structure_stride != 0 || c.uav.format == 0) {
            // D3D11.3 5.2.3.2 (the D3D12 clear's contract): a raw or
            // structured view takes Values[0] as a 32-bit word in every
            // element; the Float form is invalid on it.
            if (c.is_float) return;
            element.resize(4);
            std::memcpy(element.data(), &c.values[0], 4);
            element_bytes = raw ? 4u : c.uav.structure_stride;
        } else {
            const auto* layout = d3d11_texel::layout_for(c.uav.format);
            if (layout == nullptr) {
                // No texel encoding for the format (d3d11_texel layouts).
                unimplemented(method,
                              ("a typed buffer view of DXGI format " +
                               std::to_string(c.uav.format) +
                               ", which has no texel encoding here")
                                  .c_str());
                return;
            }
            d3d11_texel::TexelBytes texel{};
            if (c.is_float) {
                // D3D12 (unlike D3D11's "invalid" for these views) stores the
                // truncated float in an integer channel: measured on WARP and
                // the RTX 4080, d3d12_a41_format_sweep_probe [uavbuf] / [uavtex]
                // "float" on 3, 4, 12, 14, 25, 30, 32 -> {0, 0, 0, 1} for
                // {0.25, 0.5, 0.75, 1.0}. That is ClearView's rule.
                if (!d3d11_texel::encode_float(
                        *layout, color, d3d11_texel::FloatClear::ClearView,
                        texel)) {
                    return;
                }
            } else {
                texel = d3d11_texel::encode_uint(*layout, c.values);
            }
            element.assign(texel.begin(), texel.begin() + layout->texel_bytes);
            element_bytes = layout->texel_bytes;
        }
        const u64 begin = r->offset + c.uav.first_element * element_bytes;
        const u64 span = static_cast<u64>(c.uav.num_elements) * element_bytes;
        if (span == 0 || element.empty() || span % element.size() != 0) return;
        const u64 width = buffer->desc().byte_width;
        if (begin > width || span > width - begin ||
            span > std::numeric_limits<usize>::max()) {
            return;
        }
        std::vector<u8> pattern(static_cast<usize>(span));
        for (usize at = 0; at < pattern.size(); at += element.size()) {
            std::memcpy(pattern.data() + at, element.data(), element.size());
        }
        // In order with the GPU work around it: a copy from a buffer holding
        // the pattern. The queue retains the Metal buffer, so the wrapper
        // can drop when this returns.
        graphics::BackendBufferDesc desc{};
        desc.byte_width = static_cast<usize>(span);
        auto source = f.m_backend.create_buffer(desc, pattern.data());
        if (!source || !source->valid() ||
            !f.m_backend.copy_buffer_region(buffer, begin, source.get(), 0,
                                            span)) {
            unimplemented(method, "a buffer clear the backend refused");
        }
        return;
    }
    const u32 format = c.uav.format != 0 ? c.uav.format : r->desc.format;
    const auto* layout = d3d11_texel::layout_for(format);
    if (layout == nullptr) {
        // No texel encoding for the format: the video formats (AYUV, Y410,
        // Y416, YUY2, Y210, Y216), whose typed UAVs WARP reports but
        // removes the device clearing (d3d12_a41_format_sweep_probe
        // [uavtex] 100), and the RTX 4080 does not offer.
        unimplemented(method, ("a texture view of DXGI format " +
                               std::to_string(format) +
                               ", which has no texel encoding here")
                                  .c_str());
        return;
    }
    d3d11_texel::TexelBytes texel{};
    if (c.is_float) {
        // As for a buffer view above: an integer channel takes the
        // truncated float (measured, d3d12_a41_format_sweep_probe [uavtex]).
        if (!d3d11_texel::encode_float(*layout, color,
                                       d3d11_texel::FloatClear::ClearView,
                                       texel)) {
            return;
        }
    } else {
        texel = d3d11_texel::encode_uint(*layout, c.values);
    }
    const u32 slices = r->texture3d
                           ? (c.uav.array_size == 0xFFFFFFFFu ? 0xFFFFFFFFu
                                                              : c.uav.array_size)
                           : std::max(1u, c.uav.array_size == 0xFFFFFFFFu
                                              ? r->array_size() - c.uav.first_slice
                                              : c.uav.array_size);
    switch (store_encoded_texels(*r, texel.data(), layout->texel_bytes,
                                 c.uav.most_detailed_mip, c.uav.first_slice,
                                 slices, c.rects, c.uav.plane_slice)) {
    case StoreResult::Ok:
        return;
    case StoreResult::Mismatch:
        unimplemented(method, "storage of a different texel size than DXGI's");
        return;
    case StoreResult::Refused:
        unimplemented(method, "a texture fill the backend refused");
        return;
    }
}

}  // namespace vitreo::winapi::d3d12
