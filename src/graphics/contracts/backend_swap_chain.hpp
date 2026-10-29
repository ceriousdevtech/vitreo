// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file backend_swap_chain.hpp
/// @brief Swap chains, presentation colour spaces, and DirectComposition layers.

#include "graphics/contracts/backend_render_pass.hpp"
#include "graphics/contracts/backend_texture_2d.hpp"
#include "graphics/contracts/format_converter.hpp"
#include <array>
#include <memory>
#include <vector>

namespace vitreo::graphics {

struct BackendSwapChainDesc {
    void* native_view{nullptr};
    unsigned width{0};
    unsigned height{0};
    PixelFormat pixel_format{PixelFormat::Unknown};
    unsigned buffer_count{2};
    bool vsync{true};
    /// Allocate a depth/stencil attachment alongside the back buffer. A guest
    /// API that offers depth testing against the primary render target needs
    /// one; without it, depth state in a draw call has nothing to act on.
    bool depth_stencil{false};
    /// DXGI_ALPHA_MODE_PREMULTIPLIED / STRAIGHT: the layer must honour
    /// per-pixel alpha instead of compositing onto an opaque black plate.
    bool premultiplied_alpha{false};
    /// Back-buffer raster sample count. `1` is no MSAA. Present resolves
    /// into the single-sample drawable.
    unsigned sample_count{1};
};

/// How a presented image's values are to be interpreted (DXGI colour
/// spaces the presenter can honour).
enum class BackendColorSpace : u32 {
    Srgb,               ///< RGB full range, sRGB curve, BT.709 primaries
    ExtendedLinearSrgb, ///< scRGB: linear, BT.709 primaries, values past 1
    Bt2100Pq,           ///< HDR10: SMPTE ST 2084 (PQ), BT.2020 primaries
    Bt2020SrgbGamma,    ///< full range, sRGB curve, BT.2020 primaries
};

/// DirectComposition state for one swap-chain-backed visual. The matrix uses
/// Direct2D's row-vector 3x2 layout: m11, m12, m21, m22, dx, dy.
struct BackendCompositionLayerDesc {
    std::array<float, 6> transform{1.0f, 0.0f, 0.0f,
                                   1.0f, 0.0f, 0.0f};
    std::array<float, 4> clip_rect{}; // left, top, right, bottom
    bool has_clip_rect{false};
    /// IDCompositionDesktopDevice::CreateTargetForHwnd topmost layer.
    /// True places this tree above the target HWND's child windows; false
    /// places it below them.
    bool above_window_children{false};
    int z_order{0};
    BackendFilter interpolation{BackendFilter::Linear};
};

class BackendSwapChain {
public:
    virtual ~BackendSwapChain() = default;

    [[nodiscard]] virtual bool valid() const noexcept = 0;
    [[nodiscard]] virtual const BackendSwapChainDesc& desc() const noexcept = 0;
    [[nodiscard]] virtual bool clear_back_buffer(
        const std::array<float, 4>& color) = 0;

    /// Clear the swap-chain depth/stencil attachment. Default is unsupported
    /// so test backends keep using the next-pass load-action path.
    [[nodiscard]] virtual bool clear_depth(float depth) {
        (void)depth;
        return false;
    }

    [[nodiscard]] virtual bool present(unsigned sync_interval,
                                       unsigned flags) = 0;

    /// Present pixels from `source` (typically a guest swapchain Texture2D
    /// backend that received Clear/Draw). When `source` is null, present the
    /// swap chain's private back buffer (D3D9 `begin_render_pass` path).
    /// Default ignores `source` and calls `present`.
    [[nodiscard]] virtual bool present_texture(BackendTexture2D* source,
                                               unsigned sync_interval,
                                               unsigned flags) {
        (void)source;
        return present(sync_interval, flags);
    }

    /// A sub-rectangle of `source`, in source pixels, to present. A
    /// DirectComposition visual clip scopes a pooled swap chain that is
    /// larger than the content it shows (a popup draws into the top-left
    /// of a 512x512 pool entry); presenting the whole surface shows the
    /// uninitialised remainder.
    struct PresentRegion {
        unsigned x{0};
        unsigned y{0};
        unsigned width{0};
        unsigned height{0};
    };
    /// Present only `region` of `source`. Default presents the whole source.
    [[nodiscard]] virtual bool present_texture_region(
        BackendTexture2D* source, const PresentRegion& region,
        unsigned sync_interval, unsigned flags) {
        (void)region;
        return present_texture(source, sync_interval, flags);
    }

    /// True when presenting now would block the caller (the present queue
    /// is full): DXGI_PRESENT_DO_NOT_WAIT then fails instead of waiting.
    [[nodiscard]] virtual bool present_would_block() const { return false; }

    /// DXGI background colour (straight RGBA): the colour of the window
    /// area the swap chain's image does not cover. False when unsupported.
    virtual bool set_background_color(const std::array<float, 4>& rgba) {
        (void)rgba;
        return false;
    }

    /// Rotate the presented image clockwise by 90 degrees x @p quarter_turns
    /// (0..3). False when unsupported.
    virtual bool set_rotation(unsigned quarter_turns) {
        return quarter_turns == 0;
    }

    /// Tag the presented image with @p space. False when unsupported.
    virtual bool set_color_space(BackendColorSpace space) {
        return space == BackendColorSpace::Srgb;
    }

    /// DXGI_HDR_METADATA_HDR10 bytes (28) for an HDR10 image; empty clears.
    virtual bool set_hdr10_metadata(const std::vector<u8>& hdr10) {
        return hdr10.empty();
    }

    /// Copy a presentable color buffer into tightly packed RGBA8 CPU memory.
    ///
    /// When `source` is non-null, read that texture (typically a guest
    /// Texture2D that received Clear/Draw). When `source` is null, read the
    /// swap chain's private presentable back buffer (D3D9 Clear/Present path).
    /// Hosts also use this for foreign-HWND presentation. Backends that cannot
    /// produce pixels return false — callers must not invent a solid fill.
    [[nodiscard]] virtual bool readback_texture_rgba(
        BackendTexture2D* source, std::vector<u8>& rgba) {
        (void)source;
        rgba.clear();
        return false;
    }

    /// Configure this swap chain as a layer in a DirectComposition visual
    /// tree. Backends that do not expose a compositor return false so Commit
    /// can fail honestly rather than discard visible state.
    [[nodiscard]] virtual bool configure_composition_layer(
        const BackendCompositionLayerDesc& desc) {
        (void)desc;
        return false;
    }

    [[nodiscard]] virtual bool resize(unsigned width, unsigned height,
                                      PixelFormat pixel_format,
                                      unsigned buffer_count) = 0;

    /// Begin recording draws against the back buffer.
    ///
    /// `preserve_contents` keeps what is already there — a frame that was
    /// cleared first, or an earlier pass. Passing false lets the backend skip
    /// loading the attachment.
    [[nodiscard]] virtual std::unique_ptr<BackendRenderPass>
    begin_render_pass(bool preserve_contents, bool clear_depth,
                      float depth_clear_value,
                      const std::array<BackendTexture2D*,
                                       BACKEND_MAX_RENDER_TARGETS>&
                          color_targets = {}) = 0;

    /// Colour attachment draws / StretchRect should target. For an MSAA
    /// swap chain this is the multisample texture, not the 1x resolve.
    /// Lifetime is the swap chain. Default is no distinct colour target.
    [[nodiscard]] virtual BackendTexture2D* color_target() noexcept {
        return nullptr;
    }
};

} // namespace vitreo::graphics
