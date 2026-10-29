// SPDX-License-Identifier: LGPL-2.1-only
#import <Metal/Metal.h>
#include "metal_pipeline.hpp"
#include <functional>
#include <cstdio>

namespace vitreo::graphics {

MetalPipelineCompiler::MetalPipelineCompiler(void* device)
    : m_device(device) {
    VITREO_TRACE("[pipeline] compiler created device=%p\n", device);
}

MetalPipelineCompiler::~MetalPipelineCompiler() {
    VITREO_TRACE("[pipeline] compiler destroyed, cache sizes: pso=%zu ds=%zu lib=%zu\n",
                 m_pso_cache.size(), m_ds_cache.size(), m_library_cache.size());
}

void* MetalPipelineCompiler::get_or_create(void* descriptor, u64 key) {
    std::lock_guard<std::mutex> lock(m_mutex);

    auto it = m_pso_cache.find(key);
    if (it != m_pso_cache.end()) {
        VITREO_TRACE("[pipeline] PSO cache hit key=0x%llx\n",
                     static_cast<unsigned long long>(key));
        return it->second;
    }

    id<MTLDevice> device = (__bridge id<MTLDevice>)m_device;
    MTLRenderPipelineDescriptor* desc =
        (__bridge MTLRenderPipelineDescriptor*)descriptor;

    NSError* error = nil;
    id<MTLRenderPipelineState> pso =
        [device newRenderPipelineStateWithDescriptor:desc error:&error];

        if (!pso) {
            std::fprintf(stderr, "[pipeline] PSO creation failed key=0x%llx: %s\n",
                         static_cast<unsigned long long>(key),
                         error ? [[error localizedDescription] UTF8String]
                               : "unknown");
            VITREO_TRACE("[pipeline] PSO creation failed key=0x%llx: %s\n",
                         static_cast<unsigned long long>(key),
                         error ? [[error localizedDescription] UTF8String] : "unknown");
            return nullptr;
        }

    void* result = (__bridge_retained void*)pso;
    m_pso_cache[key] = result;
    VITREO_TRACE("[pipeline] PSO created key=0x%llx pso=%p\n",
                 static_cast<unsigned long long>(key), result);
    return result;
}

void MetalPipelineCompiler::compile_async(void* descriptor, u64 key,
                                          std::function<void(void*)> callback) {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_pso_cache.find(key);
        if (it != m_pso_cache.end()) {
            VITREO_TRACE("[pipeline] async PSO cache hit key=0x%llx\n",
                         static_cast<unsigned long long>(key));
            if (callback) {
                callback(it->second);
            }
            return;
        }
    }

    id<MTLDevice> device = (__bridge id<MTLDevice>)m_device;
    MTLRenderPipelineDescriptor* desc =
        (__bridge MTLRenderPipelineDescriptor*)descriptor;

    VITREO_TRACE("[pipeline] async PSO compile started key=0x%llx\n",
                 static_cast<unsigned long long>(key));

    // Capture this + callback + key for the completion handler
    auto* self = this;
    auto cb = std::move(callback);

    [device newRenderPipelineStateWithDescriptor:desc
        completionHandler:^(id<MTLRenderPipelineState> pso, NSError* err) {
            if (!pso) {
                VITREO_TRACE("[pipeline] async PSO failed key=0x%llx: %s\n",
                             static_cast<unsigned long long>(key),
                             err ? [[err localizedDescription] UTF8String] : "unknown");
                if (cb) {
                    cb(nullptr);
                }
                return;
            }

            void* result = (__bridge_retained void*)pso;
            {
                std::lock_guard<std::mutex> lock(self->m_mutex);
                self->m_pso_cache[key] = result;
            }
            VITREO_TRACE("[pipeline] async PSO created key=0x%llx pso=%p\n",
                         static_cast<unsigned long long>(key), result);
            if (cb) {
                cb(result);
            }
        }];
}

void* MetalPipelineCompiler::get_or_create_depth_stencil(void* descriptor, u64 key) {
    std::lock_guard<std::mutex> lock(m_mutex);

    auto it = m_ds_cache.find(key);
    if (it != m_ds_cache.end()) {
        VITREO_TRACE("[pipeline] DS cache hit key=0x%llx\n",
                     static_cast<unsigned long long>(key));
        return it->second;
    }

    id<MTLDevice> device = (__bridge id<MTLDevice>)m_device;
    MTLDepthStencilDescriptor* desc =
        (__bridge MTLDepthStencilDescriptor*)descriptor;

    id<MTLDepthStencilState> state =
        [device newDepthStencilStateWithDescriptor:desc];

    if (!state) {
        VITREO_TRACE("[pipeline] DS creation failed key=0x%llx\n",
                     static_cast<unsigned long long>(key));
        return nullptr;
    }

    void* result = (__bridge_retained void*)state;
    m_ds_cache[key] = result;
    VITREO_TRACE("[pipeline] DS created key=0x%llx state=%p\n",
                 static_cast<unsigned long long>(key), result);
    return result;
}

void* MetalPipelineCompiler::compile_function(const std::string& msl_source,
                                              const std::string& entry_point) {
    std::lock_guard<std::mutex> lock(m_mutex);

    // Hash the source for library caching
    u64 src_hash = std::hash<std::string>{}(msl_source);

    id<MTLLibrary> library = nil;

    auto it = m_library_cache.find(src_hash);
    if (it != m_library_cache.end()) {
        library = (__bridge id<MTLLibrary>)it->second;
        VITREO_TRACE("[pipeline] library cache hit hash=0x%llx\n",
                     static_cast<unsigned long long>(src_hash));
    } else {
        id<MTLDevice> device = (__bridge id<MTLDevice>)m_device;
        NSString* source = [NSString stringWithUTF8String:msl_source.c_str()];
        MTLCompileOptions* options = [[MTLCompileOptions alloc] init];

        NSError* error = nil;
        library = [device newLibraryWithSource:source options:options error:&error];

        if (!library) {
            std::fprintf(stderr, "[pipeline] library compile failed: %s\n",
                         error ? [[error localizedDescription] UTF8String]
                               : "unknown");
            VITREO_TRACE("[pipeline] library compile failed: %s\n",
                         error ? [[error localizedDescription] UTF8String] : "unknown");
            return nullptr;
        }

        void* lib_ptr = (__bridge_retained void*)library;
        m_library_cache[src_hash] = lib_ptr;
        VITREO_TRACE("[pipeline] library compiled hash=0x%llx\n",
                     static_cast<unsigned long long>(src_hash));
    }

    NSString* name = [NSString stringWithUTF8String:entry_point.c_str()];
    id<MTLFunction> func = [library newFunctionWithName:name];

    if (!func) {
        VITREO_TRACE("[pipeline] function '%s' not found in library\n",
                     entry_point.c_str());
        return nullptr;
    }

    void* result = (__bridge_retained void*)func;
    VITREO_TRACE("[pipeline] function '%s' compiled func=%p\n",
                 entry_point.c_str(), result);
    return result;
}

usize MetalPipelineCompiler::cache_size() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_pso_cache.size() + m_ds_cache.size() + m_library_cache.size();
}

} // namespace vitreo::graphics
