// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file metal_mesh_pipeline.hpp
/// @brief An object+mesh render pipeline (WS5: geometry shaders drawn as
///        Metal mesh shaders, host-offload plan WS5).

#include "graphics/contracts/backend_pipeline.hpp"
#include "graphics/metal/metal_minmax_rebuild.hpp"

#include <atomic>
#include <memory>
#include <string>

namespace vitreo::graphics {

class MetalMeshPipeline final : public BackendPipeline {
public:
    MetalMeshPipeline(void* device, const BackendMeshPipelineDesc& desc);
    ~MetalMeshPipeline() override;
    MetalMeshPipeline(const MetalMeshPipeline&) = delete;
    MetalMeshPipeline& operator=(const MetalMeshPipeline&) = delete;

    bool valid() const noexcept override { return m_state != nullptr; }
    bool is_mesh() const noexcept override { return true; }
    /// id<MTLRenderPipelineState>; rebuilt with the MIN/MAX reduction first
    /// when that was compiled out (metal_minmax_rebuild.hpp).
    void* state() const noexcept {
        return m_sampler_features ? m_sampler_features->current(m_state) : m_state.load(std::memory_order_acquire);
    }
    unsigned threads_per_object_threadgroup() const noexcept { return m_object_threads; }
    unsigned threads_per_mesh_threadgroup() const noexcept { return m_mesh_threads; }
    /// The compiler's or pipeline creation's message when !valid().
    const std::string& failure() const noexcept { return m_failure; }

private:
    mutable std::atomic<void*> m_state{nullptr};
    std::unique_ptr<SamplerFeatureRebuild> m_sampler_features;
    unsigned m_object_threads{1};
    unsigned m_mesh_threads{1};
    std::string m_failure;
};

}  // namespace vitreo::graphics
