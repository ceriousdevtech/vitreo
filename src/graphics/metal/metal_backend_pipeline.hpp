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
#include "graphics/metal/metal_minmax_rebuild.hpp"

#include <atomic>
#include <memory>
#include <string>

namespace vitreo::graphics {

std::string shader_tag(const char* stage, const std::string& msl);

[[nodiscard]] MTLCompileOptions* metal_compile_options(BackendMathMode mode,
                                                        bool invariance);

/// @p msl as the source a library whose function @p entry is used compiles,
/// and that function's name in it (VITREO_METAL_COMPILE_SALT).
[[nodiscard]] NSString* metal_shader_source(const std::string& msl, const std::string& entry);
[[nodiscard]] NSString* metal_entry_name(const std::string& entry);

class MetalBackendPipeline final : public BackendPipeline {
public:
    MetalBackendPipeline(void* device, const BackendPipelineDesc& desc);

    ~MetalBackendPipeline() override {
        if (void* state = m_state.load(); state != nullptr) CFRelease(state);
    }

    bool valid() const noexcept override { return m_state.load() != nullptr; }
    /// The render pipeline state, rebuilt with the MIN/MAX reduction first
    /// when that was compiled out (metal_minmax_rebuild.hpp).
    void* state() const noexcept {
        return m_sampler_features ? m_sampler_features->current(m_state) : m_state.load(std::memory_order_acquire);
    }
    /// The state, handed to the caller (a rebuild's fresh pipeline).
    void* release_state() noexcept { return m_state.exchange(nullptr); }
    bool dynamic_vertex_strides() const noexcept override {
        return m_dynamic_vertex_strides;
    }
    /// Bit s: input stream s takes the draw's stride.
    u32 dynamic_stream_mask() const noexcept { return m_dynamic_stream_mask; }

    /// Vertex streams start above CB slots (0 = D3D9 inline, 2+N = D3D11 CBs).
    /// Input-assembler streams bind at BACKEND_VERTEX_BUFFER_BASE + slot. The
    /// window is defined once in the backend contract alongside the b#/t#/u#
    /// windows so it cannot drift into them.
    static constexpr NSUInteger VERTEX_BUFFER_BASE = BACKEND_VERTEX_BUFFER_BASE;
    /// Compatibility alias for single-stream frontends (D3D9 / slot 0).
    static constexpr NSUInteger VERTEX_BUFFER_INDEX = VERTEX_BUFFER_BASE;
private:
    mutable std::atomic<void*> m_state{nullptr};
    std::unique_ptr<SamplerFeatureRebuild> m_sampler_features;
    bool m_dynamic_vertex_strides{false};
    u32 m_dynamic_stream_mask{0};
};

} // namespace vitreo::graphics
