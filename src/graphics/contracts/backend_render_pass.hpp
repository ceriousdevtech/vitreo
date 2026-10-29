// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file backend_render_pass.hpp
/// @brief Render pass descriptors and recording scopes.

#include "graphics/contracts/backend_command_stream.hpp"
#include "graphics/contracts/backend_draw_call.hpp"
#include "graphics/contracts/format_converter.hpp"
#include <array>

namespace vitreo::graphics {

/// Render pass creation descriptor supporting MRT and explicit depth/stencil.
struct BackendRenderPassDesc {
    /// Color attachments. Null entries are skipped; the first non-null sets
    /// attachment 0, etc. Up to 8 MRT targets. For swapchain rendering, slot 0
    /// is typically the back buffer texture.
    std::array<BackendTexture2D*, BACKEND_MAX_RENDER_TARGETS> color_targets{};
    /// Typed view format selected for each 2D color attachment. Unknown uses
    /// the resource format. D3D typeless resources and swap-chain sRGB RTVs
    /// require the backend to attach a format-compatible texture view.
    std::array<PixelFormat, BACKEND_MAX_RENDER_TARGETS> color_formats{};
    /// Mip subresource selected independently by each color attachment.
    std::array<unsigned, BACKEND_MAX_RENDER_TARGETS> color_levels{};
    /// First 2D-array slice and number of slices exposed by each attachment.
    /// Non-array targets use slice 0 and size 1.
    std::array<unsigned, BACKEND_MAX_RENDER_TARGETS> color_slices{};
    std::array<unsigned, BACKEND_MAX_RENDER_TARGETS> color_array_sizes{
        1, 1, 1, 1, 1, 1, 1, 1};
    /// Volume-texture color attachments. A D3D 3D render-target view renders
    /// into one W slice of a Texture3D, which Metal expresses as a depth plane
    /// on an otherwise ordinary 2D-shaped attachment. A slot is a 2D target or
    /// a 3D one, never both, so these share the attachment numbering with
    /// `color_targets` exactly as `textures_3d` shares the draw call's texture
    /// slots.
    std::array<BackendTexture3D*, BACKEND_MAX_RENDER_TARGETS>
        color_targets_3d{};
    /// Volume attachments only: the first W slice of the view. The view's
    /// W-slice count travels in `color_array_sizes`; a draw whose vertex stage
    /// writes the render-target array index renders into depth plane
    /// (first + index), and an index outside the view renders the first.
    std::array<unsigned, BACKEND_MAX_RENDER_TARGETS> color_depth_planes{};
    bool preserve_color_contents{false};

    /// Optional depth/stencil attachment. If null, depth test/write have no
    /// effect. Can be used alongside color targets or standalone (depth-only).
    BackendTexture2D* depth_stencil_target{nullptr};
    unsigned depth_level{0};
    unsigned depth_slice{0};
    unsigned depth_array_size{1};
    bool clear_depth{false};
    float depth_clear_value{1.0f};

    /// No colour and no depth attachment at all. D3D11 rasterizes a draw with
    /// no RTV and no DSV bound: the pixel shader's UAV writes are its only
    /// output (a D3D11 renderer's lighting and compositing passes). The rasterizer then takes
    /// its extent and sample count from these instead of an attachment;
    /// Metal expresses exactly that with renderTargetWidth/Height and
    /// defaultRasterSampleCount. Ignored when any attachment is present.
    unsigned attachmentless_width{0};
    unsigned attachmentless_height{0};
    unsigned attachmentless_samples{1};
    unsigned attachmentless_layers{1};

    /// Place every sample of the pass's (multisampled) attachments at the
    /// pixel centre (supports_programmable_sample_positions): a D3D11
    /// ForcedSampleCount of 1 on a multisampled target (D3D11.3 3.5.6.2),
    /// where the pixel is covered when its centre is and then writes every
    /// sample. Ignored for single-sample and attachment-less passes.
    bool center_sample_positions{false};

    /// Add the samples every draw of this pass passes through the depth and
    /// stencil tests to this counter (from create_occlusion_counter of the
    /// same backend). Null: the pass counts nothing. The counter must outlive
    /// the GPU execution of the pass.
    GpuOcclusionCounter* occlusion_counter{nullptr};
};

/// One recording scope against a render target. Created by a swap chain or by
/// create_render_pass_standalone(), submitted by `end()`; destroying it
/// without `end()` discards the work.
class BackendRenderPass {
public:
    virtual ~BackendRenderPass() = default;
    [[nodiscard]] virtual bool valid() const noexcept = 0;
    [[nodiscard]] virtual bool draw(const BackendDrawCall& call) = 0;
    [[nodiscard]] virtual bool end() = 0;
};

} // namespace vitreo::graphics
