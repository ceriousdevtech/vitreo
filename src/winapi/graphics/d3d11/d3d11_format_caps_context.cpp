// SPDX-License-Identifier: LGPL-2.1-only

/// @file d3d11_format_caps_context.cpp
/// @brief The device's cached backend format-caps answers.
///
/// format_caps_context reads what the backend supports (sample counts,
/// float32 filtering and resolve, shared, extended-shared and tiled
/// storage, framebuffer fetch) once and stamps the caller's feature level
/// on a copy; extended_nv12_sharing and multisample_quality_levels answer
/// from it. The tables it feeds are the d3d11:: free functions of
/// d3d11_format_caps.cpp.
///
/// Split out of d3d11_device_ext.cpp.

#include "winapi/graphics/d3d11/d3d11.hpp"
#include "winapi/graphics/d3d11/d3d11_internal.hpp"
#include "winapi/core/marshaler.hpp"
#include "graphics/contracts/backend_device.hpp"
#include "graphics/contracts/format_converter.hpp"
#include "core/memory/guest_memory.hpp"

namespace vitreo::winapi {

d3d11::FormatCapsContext D3D11Manager::format_caps_context(
    u32 feature_level) const {
    // Guest threads may query capabilities concurrently (ID3D11Device is
    // free-threaded); the backend answers are read once.
    std::call_once(m_format_caps_once, [this] {
        d3d11::FormatCapsContext base{};
        base.sample_count_2 = m_backend.supports_texture_sample_count(2);
        base.sample_count_4 = m_backend.supports_texture_sample_count(4);
        base.sample_count_8 = m_backend.supports_texture_sample_count(8);
        base.sample_count_16 = m_backend.supports_texture_sample_count(16);
        base.float32_filtering = m_backend.supports_float32_filtering();
        base.float32_msaa_resolve =
            m_backend.supports_float32_multisample_resolve();
        for (usize i = 1; i < base.shared_storage.size(); ++i) {
            base.shared_storage[i] = m_backend.supports_shared_texture(
                static_cast<graphics::PixelFormat>(i));
        }
        base.extended_sharing = m_backend.supports_extended_sharing();
        base.logic_ops = m_backend.supports_framebuffer_fetch();
        // Tiled resources: a format is TILED where Windows reports it
        // (the Windows FORMAT_SUPPORT2 row) and a tiled texture of its
        // storage format has the D3D standard tile shape here -- a tile is
        // then exactly one Metal sparse page, so GetResourceTiling, the
        // mappings and CopyTiles' linear layout all hold.
        base.tiled_resources = m_backend.supports_tiled_resources();
        for (u32 format = 0;
             base.tiled_resources && format < base.tiled_format.size();
             ++format) {
            if ((d3d11::windows_support2(format) &
                 d3d11::FORMAT_SUPPORT2_TILED) == 0) {
                continue;
            }
            const graphics::PixelFormat storage =
                d3d11::texture_storage_format(format);
            for (const u32 samples : {1u, 4u}) {
                u32 want_w = 0;
                u32 want_h = 0;
                unsigned have_w = 0;
                unsigned have_h = 0;
                const bool same =
                    d3d11::standard_tile_shape(format, samples, want_w,
                                               want_h) &&
                    m_backend.tiled_texture_tile_shape(storage, samples,
                                                       have_w, have_h) &&
                    have_w == want_w && have_h == want_h;
                (samples == 1 ? base.tiled_format
                              : base.tiled_format_4x)[format] = same;
            }
            // 4x only where a single-sample tiled texture exists too.
            base.tiled_format_4x[format] =
                base.tiled_format_4x[format] && base.tiled_format[format];
        }
        m_format_caps_base = base;
    });
    d3d11::FormatCapsContext context = *m_format_caps_base;
    context.feature_level = feature_level;
    return context;
}

d3d11::FormatCapsContext D3D11Manager::format_caps_context(
    const Device& device) const {
    d3d11::FormatCapsContext context =
        format_caps_context(device.feature_level);
    context.dxgi_1_0_formats_only = device.d3d10_api_10_0;
    return context;
}

bool D3D11Manager::extended_nv12_sharing(u32 feature_level) const {
    // An NV12 Texture2D exists and is shareable (FORMAT_SUPPORT2 SHAREABLE),
    // and every shape of it shares.
    return m_backend.supports_extended_sharing() &&
           (d3d11::check_format_support2(103u,
                                         format_caps_context(feature_level))
                .mask &
            d3d11::FORMAT_SUPPORT2_SHAREABLE) != 0;
}

u32 D3D11Manager::multisample_quality_levels(u32 dxgi_format,
                                             u32 sample_count) const {
    // The quality levels resource creation validates DXGI_SAMPLE_DESC
    // against: the same table CheckMultisampleQualityLevels answers from.
    // Measured on Windows for depth as well as colour (d3d11_msaa_depth_probe
    // and d3d11_a01_device_caps_probe): one level at every sample count the
    // format renders (or is a depth target) at.
    u32 levels = 0;
    (void)d3d11::check_multisample_quality_levels(
        dxgi_format, sample_count,
        format_caps_context(d3d11::FEATURE_LEVEL_11_0), levels);
    return levels;
}

}  // namespace vitreo::winapi
