// SPDX-License-Identifier: LGPL-2.1-only
#pragma once
#include <vitreo/common.hpp>
#include <string>
#include <unordered_map>
#include <functional>
#include <mutex>

namespace vitreo::graphics {

class MetalPipelineCompiler {
public:
    explicit MetalPipelineCompiler(void* device);  // id<MTLDevice>
    ~MetalPipelineCompiler();

    /// Get or create a render pipeline state from a descriptor.
    /// descriptor is MTLRenderPipelineDescriptor*, returns id<MTLRenderPipelineState> as void*.
    void* get_or_create(void* descriptor, u64 key);

    /// Async compilation. Callback receives id<MTLRenderPipelineState> as void*.
    void compile_async(void* descriptor, u64 key,
                       std::function<void(void*)> callback);

    /// Get or create depth-stencil state.
    /// descriptor is MTLDepthStencilDescriptor*, returns id<MTLDepthStencilState>.
    void* get_or_create_depth_stencil(void* descriptor, u64 key);

    /// Compile MSL source → MTLFunction. Returns id<MTLFunction> as void*.
    void* compile_function(const std::string& msl_source,
                           const std::string& entry_point);

    usize cache_size() const;

    MetalPipelineCompiler(const MetalPipelineCompiler&) = delete;
    MetalPipelineCompiler& operator=(const MetalPipelineCompiler&) = delete;

private:
    void* m_device = nullptr;
    std::unordered_map<u64, void*> m_pso_cache;
    std::unordered_map<u64, void*> m_ds_cache;
    std::unordered_map<u64, void*> m_library_cache;
    mutable std::mutex m_mutex;
};

} // namespace vitreo::graphics
