// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file metal_backend_render_pass.hpp
/// @brief The swap-chain render pass and the draw recording it shares
///
/// Private to vitreo_graphics_metal, split out of metal_device.mm: included
/// only by that target's .mm files, after <Metal/Metal.h> (the layering
/// check keeps Apple framework imports out of headers).

#include <vitreo/common.hpp>
#include "graphics/contracts/backend_device.hpp"
#include "graphics/metal/metal_backend_state.hpp"
#include "graphics/metal/metal_backend_texture_2d.hpp"
#include "graphics/metal/metal_submit_queue.hpp"

#include <array>

namespace vitreo::graphics {

bool record_backend_draw(const BackendDrawCall& call, bool has_depth,
                         MetalPassStateCache& cache, u64 pass_id);

/// Records draws against a colour (and optional depth) attachment onto the
/// command stream. The worker encodes and submits them.
class MetalBackendRenderPass final : public BackendRenderPass {
public:
    MetalBackendRenderPass(void* device, void* color_texture, void* depth_texture,
                           bool preserve_contents, bool clear_depth,
                           float depth_clear_value,
                           void* resolve_texture,
                           const std::array<BackendTexture2D*,
                                            BACKEND_MAX_RENDER_TARGETS>&
                               color_targets)
        : m_cache(device) {
        // Record-only: T1 already ended the previous pass (or handed it
        // to the D3D9 worker). The Metal worker serializes encode; do
        // not GPU-wait here — wait_encoded() parks until GPU complete.
        CsBeginPass begin{};
        begin.colors[0] = color_texture;
        begin.color_count = color_texture != nullptr ? 1 : 0;
        for (unsigned slot = 1; slot < BACKEND_MAX_RENDER_TARGETS; ++slot) {
            BackendTexture2D* target = color_targets[slot];
            if (target == nullptr || !target->valid()) continue;
            const auto* metal_target =
                static_cast<const MetalBackendTexture2D*>(target);
            if (!metal_target->desc().render_target) continue;
            begin.colors[slot] = metal_target->native_handle();
            begin.color_count = slot + 1;
        }
        begin.depth = depth_texture;
        begin.resolve = resolve_texture;
        begin.preserve = preserve_contents;
        begin.clear_depth = clear_depth;
        begin.depth_clear = depth_clear_value;
        begin.stencil = depth_texture != nullptr;
        m_has_depth = depth_texture != nullptr;
        m_pass_id = MetalSubmitQueue::instance().record_begin_pass(begin);
        m_open = true;
    }

    ~MetalBackendRenderPass() override {
        if (m_open) {
            MetalSubmitQueue::instance().record_end_pass(m_pass_id);
            m_open = false;
        }
    }

    bool valid() const noexcept override { return m_open; }

    bool draw(const BackendDrawCall& call) override {
        if (!m_open) {
            VITREO_TRACE("[metal] draw rejected: render pass is not open\n");
            return false;
        }
        return record_backend_draw(call, m_has_depth, m_cache, m_pass_id);
    }

    bool end() override {
        if (!m_open) return false;
        MetalSubmitQueue::instance().record_end_pass(m_pass_id);
        m_open = false;
        return true;
    }

private:
    MetalPassStateCache m_cache;
    u64 m_pass_id{0};
    bool m_open{false};
    bool m_has_depth{false};
};

} // namespace vitreo::graphics
