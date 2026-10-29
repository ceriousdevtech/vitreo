// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file metal_backend_pipeline.hpp
/// @brief Render pipelines and the shader compile options they share
///
/// Private to vitreo_graphics_metal, split out of metal_device.mm: included
/// only by that target's .mm files, after <Metal/Metal.h> (the layering
/// check keeps Apple framework imports out of headers).

#include <vitreo/common.hpp>
#include "graphics/contracts/backend_device.hpp"

#include <string>

namespace vitreo::graphics {

std::string shader_tag(const char* stage, const std::string& msl);

[[nodiscard]] MTLCompileOptions* metal_compile_options(BackendMathMode mode,
                                                        bool invariance);

class MetalBackendPipeline final : public BackendPipeline {
public:
    MetalBackendPipeline(void* device, const BackendPipelineDesc& desc);

    ~MetalBackendPipeline() override {
        if (m_state != nullptr) CFRelease(m_state);
    }

    bool valid() const noexcept override { return m_state != nullptr; }
    void* state() const noexcept { return m_state; }

    /// Vertex streams start above CB slots (0 = D3D9 inline, 2+N = D3D11 CBs).
    /// Input-assembler streams bind at BACKEND_VERTEX_BUFFER_BASE + slot. The
    /// window is defined once in the backend contract alongside the b#/t#/u#
    /// windows so it cannot drift into them.
    static constexpr NSUInteger VERTEX_BUFFER_BASE = BACKEND_VERTEX_BUFFER_BASE;
    /// Compatibility alias for single-stream frontends (D3D9 / slot 0).
    static constexpr NSUInteger VERTEX_BUFFER_INDEX = VERTEX_BUFFER_BASE;
private:
    void* m_state{nullptr};
};

} // namespace vitreo::graphics
