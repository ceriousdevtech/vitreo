// SPDX-License-Identifier: LGPL-2.1-only

/// @file metal_backend_compute.mm
/// @brief Compute pipelines and dispatch

#import <Metal/Metal.h>
#import <IOSurface/IOSurface.h>
#import <QuartzCore/CAMetalLayer.h>
#include "metal_device.hpp"
#include "graphics/shaders/compile_profile.hpp"
#include "graphics/common/diagnostic_path.hpp"
#include "graphics/metal/metal_buffer.hpp"
#include "graphics/metal/metal_swapchain.hpp"
#include "graphics/metal/metal_submit_queue.hpp"
#include "graphics/metal/metal_tiled.hpp"
#include "graphics/metal/metal_gpu_profile.hpp"
#include "graphics/metal/metal_shader_validation.hpp"
#include "graphics/contracts/delta_record.hpp"
#include "graphics/contracts/slim_record.hpp"
#include <vitreo/ab_cycle.hpp>

#include <atomic>
#include <cmath>
#include <chrono>
#include "graphics/metal/metal_texture.hpp"
#include "graphics/metal/metal_shared_resource.hpp"
#include "core/memory/host_memory_floor.hpp"

#include <mach/mach.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <limits>
#include <memory>
#include <map>
#include <unordered_map>
#include <utility>
#include <mutex>
#include <thread>
#include <deque>
#include <unistd.h>
#include <condition_variable>
#include <vector>

#include "graphics/metal/metal_backend_pipeline.hpp"
#include "graphics/metal/metal_backend_buffer.hpp"
#include "graphics/metal/metal_backend_texture_2d.hpp"
#include "graphics/metal/metal_backend_texture_3d.hpp"
#include "graphics/metal/metal_backend_state.hpp"
#include "graphics/metal/metal_backend_descriptor_table.hpp"

using namespace vitreo;

namespace vitreo::graphics {

namespace {

class MetalBackendComputePipeline final : public BackendComputePipeline {
public:
    MetalBackendComputePipeline(void* device,
                                const BackendComputePipelineDesc& desc) {
        @autoreleasepool {
            id<MTLDevice> mtl_device = (__bridge id<MTLDevice>)device;
            // Read once (metal_minmax_rebuild.hpp): built without the
            // MIN/MAX reduction, the state is rebuilt with it later.
            const unsigned compiled_features = enabled_sampler_features();
            const unsigned source_features = sampler_features_in_msl(desc.compute_shader_msl);
            if (mtl_device == nil || desc.compute_shader_msl.empty()) {
                last_pipeline_failure() = mtl_device == nil
                                              ? "no Metal device"
                                              : "empty compute MSL";
                return;
            }

            NSError* error = nil;
            const u64 library_t0 = compile_profile::now_ns();
            compile_profile::begin(compile_profile::MetalLibrary);
            id<MTLLibrary> library = [mtl_device
                newLibraryWithSource:metal_shader_source(desc.compute_shader_msl,
                                                     desc.compute_entry_point)
                             options:metal_compile_options(desc.math_mode,
                                                           false)
                               error:&error];
            compile_profile::end(compile_profile::MetalLibrary,
                                 compile_profile::now_ns() - library_t0);
            if (library == nil) {
                const char* text = error != nil
                                       ? [[error localizedDescription] UTF8String]
                                       : "unknown error";
                std::fprintf(stderr,
                             "[metal] compute shader compilation failed: %s\n",
                             text);
                last_pipeline_failure() =
                    std::string("Metal compute shader compilation failed: ") + text;
                return;
            }
            id<MTLFunction> function =
                [library newFunctionWithName:metal_entry_name(desc.compute_entry_point)];
            if (function == nil) {
                std::fprintf(stderr,
                             "[metal] compute entry point '%s' not found\n",
                             desc.compute_entry_point.c_str());
                last_pipeline_failure() = "Metal compute entry point '" +
                                          desc.compute_entry_point + "' not found";
                return;
            }

            MTLComputePipelineDescriptor* pipeline_desc =
                [[MTLComputePipelineDescriptor alloc] init];
            configure_metal_shader_validation(pipeline_desc);
            pipeline_desc.computeFunction = function;
            pipeline_desc.label =
                @(shader_tag("cs", desc.compute_shader_msl).c_str());
            const u64 state_t0 = compile_profile::now_ns();
            compile_profile::begin(compile_profile::MetalComputeState);
            id<MTLComputePipelineState> state = [mtl_device
                newComputePipelineStateWithDescriptor:pipeline_desc
                                              options:MTLPipelineOptionNone
                                           reflection:nil
                                                error:&error];
            compile_profile::end(compile_profile::MetalComputeState,
                                 compile_profile::now_ns() - state_t0);
            if (state == nil) {
                const char* text = error != nil
                                       ? [[error localizedDescription] UTF8String]
                                       : "unknown error";
                std::fprintf(stderr,
                             "[metal] compute pipeline creation failed: %s\n", text);
                last_pipeline_failure() =
                    std::string("Metal compute pipeline creation failed: ") + text;
                return;
            }
            report_metal_shader_validation(state);
            m_state = (__bridge_retained void*)state;
            if ((source_features & ~compiled_features) != 0) {
                BackendComputePipelineDesc kept = desc;
                kept.compute_shader_msl.clear();
                auto source = intern_msl_source(desc.compute_shader_msl);
                m_sampler_features = std::make_unique<SamplerFeatureRebuild>(
                    source_features, compiled_features, [device, kept = std::move(kept), source]() mutable -> void* {
                        kept.compute_shader_msl = *source;
                        MetalBackendComputePipeline fresh(device, kept);
                        return fresh.m_state.exchange(nullptr);
                    });
            }
        }
    }

    ~MetalBackendComputePipeline() override {
        if (void* state = m_state.load(); state != nullptr) CFRelease(state);
    }

    bool valid() const noexcept override { return m_state.load() != nullptr; }
    /// Rebuilt with the MIN/MAX reduction first when that was compiled out.
    void* state() const noexcept {
        return m_sampler_features ? m_sampler_features->current(m_state) : m_state.load(std::memory_order_acquire);
    }

private:
    mutable std::atomic<void*> m_state{nullptr};
    std::unique_ptr<SamplerFeatureRebuild> m_sampler_features;
};

} // namespace

std::unique_ptr<BackendComputePipeline> MetalDevice::create_compute_pipeline(
    const BackendComputePipelineDesc& desc) {
    if (!available()) return nullptr;
    auto pipeline = std::make_unique<MetalBackendComputePipeline>(m_device, desc);
    if (!pipeline->valid()) return nullptr;
    return pipeline;
}

bool MetalDevice::dispatch(const BackendDispatchCall& call) {
    if (!available()) return false;
    const auto* pipeline =
        static_cast<const MetalBackendComputePipeline*>(call.pipeline);
    if (pipeline == nullptr || !pipeline->valid()) return false;
    // A GPU-sourced dispatch has no CPU-side counts to validate: zero is a legal
    // value for the GPU to have written, and Metal treats a zero count as a
    // no-op dispatch. Only a direct dispatch is rejected for a zero count.
    void* indirect_handle = nullptr;
    usize indirect_offset = 0;
    if (call.indirect.active()) {
        const auto* args =
            static_cast<const MetalBackendBuffer*>(call.indirect.buffer);
        if (!args->valid()) return false;
        indirect_handle = args->native_handle();
        if (indirect_handle == nullptr) return false;
        // Learn (DispatchIndirect) takes a DWORD-aligned offset, as Metal does.
        if ((call.indirect.offset % 4) != 0) {
            VITREO_TRACE("[metal] dispatch rejected: indirect argument offset "
                         "%llu is not DWORD aligned\n",
                         static_cast<unsigned long long>(call.indirect.offset));
            return false;
        }
        if (!metal_buffer_offset(call.indirect.offset, args->native_offset(),
                                 indirect_offset)) {
            return false;
        }
    } else if (call.group_count_x == 0 || call.group_count_y == 0 ||
               call.group_count_z == 0) {
        return false;
    }

    CsCompute compute{};
    compute.pipeline = pipeline->state();
    if (compute.pipeline == nullptr) return false;
    compute.gx = call.group_count_x;
    compute.gy = call.group_count_y;
    compute.gz = call.group_count_z;
    compute.tx = call.threads_per_group_x;
    compute.ty = call.threads_per_group_y;
    compute.tz = call.threads_per_group_z;
    if (indirect_handle != nullptr) {
        static_cast<const MetalBackendBuffer*>(call.indirect.buffer)
            ->note_gpu_bind();
        compute.indirect_args = indirect_handle;
        retain_cs_handle(indirect_handle);
        compute.indirect_offset = indirect_offset;
    }
    if (!publish_descriptor_tables(call.descriptor_tables,
                                  compute.descriptor_tables,
                                  compute.resident, 0,
                                  BACKEND_DESCRIPTOR_SET_UAV,
                                  &compute.resident_logs)) {
        return false;
    }
    compute.resident_read_count = static_cast<u32>(compute.resident.size());
    if (!publish_descriptor_tables(call.descriptor_tables,
                                  compute.descriptor_tables, compute.resident,
                                  BACKEND_DESCRIPTOR_SET_UAV,
                                  BACKEND_MAX_DESCRIPTOR_SETS,
                                  &compute.resident_logs)) return false;
    static const bool read_usage = [] {
        const char* text = std::getenv("VITREO_METAL_COMPUTE_READ_USAGE");
        return text == nullptr || text[0] != '0';
    }();
    // Persistent logs declare usage incrementally on the encoder. Preserve
    // that existing path conservatively: a flat read declaration must never
    // downgrade an overlapping log resource whose writable usage is retained.
    if (!read_usage || !compute.resident_logs.empty() ||
        diagnostics::ab_cycle_off("metal-compute-read-usage")) {
        compute.resident_read_count = 0;
    }

    @autoreleasepool {
        MetalPassStateCache cache(m_device);
        for (unsigned i = 0; i < BACKEND_MAX_COMPUTE_BUFFERS; ++i) {
            if (call.buffers[i] == nullptr || !call.buffers[i]->valid()) {
                continue;
            }
            const auto* metal_buf =
                static_cast<const MetalBackendBuffer*>(call.buffers[i]);
            void* buffer_handle = metal_buf->native_handle();
            if (buffer_handle == nullptr) continue;
            usize buffer_off = 0;
            if (!metal_buffer_offset(call.buffer_offsets[i],
                                     metal_buf->native_offset(), buffer_off)) {
                continue;
            }
            compute.buffers[i] = buffer_handle;
            compute.buffer_off[i] = buffer_off;
            metal_buf->note_gpu_bind();
        }
        for (unsigned i = 0; i < BACKEND_MAX_COMPUTE_TEXTURES; ++i) {
            if (call.textures_3d[i] != nullptr && call.textures_3d[i]->valid()) {
                // A volume binds whole: Metal cannot view a W-slice window of
                // a 3D texture, and the D3D11 frontend only forwards full
                // mip-0 volume views.
                void* volume_handle =
                    static_cast<const MetalBackendTexture3D*>(
                        call.textures_3d[i])->native_handle();
                if (volume_handle != nullptr) compute.textures[i] = volume_handle;
                continue;
            }
            if (call.texture_buffers[i] != nullptr &&
                call.texture_buffers[i]->valid()) {
                // A typed buffer (Buffer<T> SRV or RWBuffer<T> UAV) binds as
                // the texture_buffer the shader declares at this index.
                auto* typed = const_cast<MetalBackendTextureBuffer*>(
                    static_cast<const MetalBackendTextureBuffer*>(
                        call.texture_buffers[i]));
                void* typed_handle = typed->native_shader_handle();
                if (typed_handle != nullptr) {
                    typed->note_gpu_bind();
                    compute.textures[i] = typed_handle;
                    if (void* storage = typed->storage_handle()) {
                        compute.resident.push_back(storage);
                        retain_cs_handle(storage);
                    }
                }
                continue;
            }
            if (call.textures[i] == nullptr || !call.textures[i]->valid()) {
                continue;
            }
            void* texture_handle =
                static_cast<const MetalBackendTexture2D*>(call.textures[i])
                    ->native_shader_handle(call.texture_views[i], true);
            if (texture_handle == nullptr) continue;
            compute.textures[i] = texture_handle;
        }
        for (unsigned i = 0; i < call.samplers.size() &&
                             i < compute.samplers.size();
             ++i) {
            if (!call.sampler_bound[i]) continue;
            compute.samplers[i] = cache.sampler_state(call.samplers[i]);
        }
        for (unsigned i = 0; i < call.samplers.size(); ++i) {
            if ((call.border_twin_mask & (1u << i)) == 0) continue;
            compute.samplers[BACKEND_BORDER_TWIN_SAMPLER_BASE + i] =
                cache.sampler_state(border_twin(call.samplers[i]));
        }
        if (call.constants != nullptr && call.constants_size != 0) {
            const auto* bytes = static_cast<const u8*>(call.constants);
            compute.constants.assign(bytes, bytes + call.constants_size);
        }
        MetalSubmitQueue::instance().record_compute(std::move(compute));
    }
    return true;
}

} // namespace vitreo::graphics
