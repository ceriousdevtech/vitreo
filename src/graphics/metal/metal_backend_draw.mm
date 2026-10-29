// SPDX-License-Identifier: LGPL-2.1-only

/// @file metal_backend_draw.mm
/// @brief Draw resolution into the command stream

#import <Metal/Metal.h>
#import <IOSurface/IOSurface.h>
#import <QuartzCore/CAMetalLayer.h>
#include "metal_device.hpp"
#include "graphics/common/diagnostic_path.hpp"
#include "graphics/metal/metal_buffer.hpp"
#include "graphics/metal/metal_swapchain.hpp"
#include "graphics/metal/metal_submit_queue.hpp"
#include "graphics/metal/metal_tiled.hpp"
#include "graphics/metal/metal_gpu_profile.hpp"
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

#include "graphics/metal/metal_backend_enums.hpp"
#include "graphics/metal/metal_backend_buffer.hpp"
#include "graphics/metal/metal_backend_texture_2d.hpp"
#include "graphics/metal/metal_backend_texture_3d.hpp"
#include "graphics/metal/metal_backend_state.hpp"
#include "graphics/metal/metal_backend_descriptor_table.hpp"
#include "graphics/metal/metal_backend_render_pass.hpp"
#include "graphics/metal/metal_backend_pipeline.hpp"

using namespace vitreo;

namespace vitreo::graphics {

namespace {

void copy_cs_constants(CsDraw::PushBytes& dest, const void* src, usize size) {
    dest.clear();
    if (src != nullptr && size > 0) {
        const auto* bytes = static_cast<const u8*>(src);
        dest.assign(bytes, bytes + size);
    }
}

/// VITREO_METAL_DELTA_VERIFY (diagnostic): every native state @p call names
/// must have been built from exactly the desc it is paired with.
void verify_native_states(const BackendDrawCall& call) {
    const auto fail = [](const char* what, unsigned slot) {
        std::fprintf(stderr,
                     "[r3] VERIFY FAILED: %s %u names a native state built "
                     "from a different desc\n",
                     what, slot);
        std::abort();
    };
    const BackendNativeStates* states = call.native_states;
    if (states == nullptr) return;
    for (unsigned slot = 0; slot < BACKEND_MAX_TEXTURE_SLOTS; ++slot) {
        if (states->samplers[slot] != nullptr &&
            !same_sampler_desc(states->samplers[slot]->desc(),
                               call.samplers[slot])) {
            fail("fragment sampler", slot);
        }
        if (states->vertex_samplers[slot] != nullptr &&
            !same_sampler_desc(states->vertex_samplers[slot]->desc(),
                               call.vertex_samplers[slot])) {
            fail("vertex sampler", slot);
        }
    }
    if (states->depth_stencil != nullptr &&
        !same_depth_stencil_desc(states->depth_stencil->desc(),
                                 depth_stencil_desc_of(call))) {
        fail("depth-stencil state", 0);
    }
}

/// Resolve @p call into @p draw: every native handle and offset, as the
/// encoder will bind them. Legacy (kDelta false): the draw holds a reference
/// on every handle it names and stamps every buffer it binds, as a full
/// queue op. delta-record (kDelta true): no references (record_draw_delta
/// takes them on what the delta carries) and each buffer is stamped once per
/// recording generation @p gen and storage.
template <bool kDelta>
bool fill_backend_draw(const BackendDrawCall& call, bool has_depth,
                       MetalPassStateCache& cache, CsDraw& draw, u64 gen) {
    const auto keep = [](void* handle) {
        if constexpr (!kDelta) retain_cs_handle(handle);
        (void)handle;
    };
    const auto bind = [gen](const auto* object) {
        if constexpr (kDelta) {
            object->note_gpu_bind_once(gen);
        } else {
            object->note_gpu_bind();
        }
        (void)gen;
    };
    const auto* pipeline =
        static_cast<const MetalBackendPipeline*>(call.pipeline);
    if (pipeline == nullptr || !pipeline->valid()) {
        VITREO_TRACE("[metal] draw rejected: pipeline is missing/invalid\n");
        return false;
    }
    // A draw with no vertex buffer is legal, not a bad draw. Microsoft Learn
    // (SV_VertexID / Draw): a vertex shader may generate its positions from
    // the vertex index alone, with no input layout and nothing bound to the
    // input assembler. Every fullscreen pass of a D3D11 renderer, including
    // the final composite to the back buffer, draws that way, so rejecting
    // these left the window black while every other draw succeeded.
    if (call.vertex_buffer != nullptr && !call.vertex_buffer->valid()) {
        VITREO_TRACE("[metal] draw rejected: vertex buffer is invalid\n");
        return false;
    }
    const bool indexed = call.index_buffer != nullptr;
    if (indexed && !call.index_buffer->valid()) {
        VITREO_TRACE("[metal] draw rejected: index buffer is invalid\n");
        return false;
    }
    // An indirect draw's counts live in its argument buffer and the ones here
    // are ignored (BackendDrawCall::indirect), so a zero here is not a bad
    // draw. Checking them anyway dropped every D3D11 Draw*Indirect, whose
    // frontend correctly passes 0 -- the S4 test never saw it because it
    // always supplied a non-zero dummy.
    const bool patches = call.tessellation_factors != nullptr;
    if (patches &&
        (call.patch_count == 0 || call.patch_control_points == 0 ||
         call.patch_control_points > 32 || !call.tessellation_factors->valid())) {
        VITREO_TRACE("[metal] draw rejected: bad patch draw\n");
        return false;
    }
    if (!patches && call.indirect.buffer == nullptr &&
        ((indexed ? call.index_count == 0 : call.vertex_count == 0) ||
         call.instance_count == 0)) {
        VITREO_TRACE("[metal] draw rejected: zero vertex/index/instance "
                     "count\n");
        return false;
    }
    // A depth-stencil state left enabled while no depth-stencil target is
    // bound is not a bad draw. Microsoft Learn (OMSetRenderTargets /
    // ID3D11DepthStencilState): depth and stencil testing require a bound
    // depth-stencil view, so with none bound the tests simply do not occur
    // and the draw still rasterises. Dropping the draw here silently deleted
    // exactly those draws -- including a swap-chain composite that leaves a
    // guest's back buffer with whatever the render target held before.
    // `depth_stencil_state(call, has_depth)` already yields a null state for
    // this case, and Metal's default (compare Always, no depth write) is the
    // same observable behaviour.

    const auto* vertex =
        static_cast<const MetalBackendBuffer*>(call.vertex_buffer);
    void* vertex_handle = vertex != nullptr ? vertex->native_handle() : nullptr;
    if (vertex != nullptr && vertex_handle == nullptr) {
        VITREO_TRACE("[metal] draw rejected: vertex buffer has no "
                     "native handle\n");
        return false;
    }
    void* index_handle = nullptr;
    const MetalBackendBuffer* index = nullptr;
    if (indexed) {
        index = static_cast<const MetalBackendBuffer*>(call.index_buffer);
        index_handle = index->native_handle();
        if (index_handle == nullptr) {
            VITREO_TRACE("[metal] draw rejected: index buffer has no "
                         "native handle\n");
            return false;
        }
    }
    // A GPU-sourced draw reads its counts from a buffer. Resolve it here so the
    // draw is rejected before recording if the argument buffer is unusable —
    // the alternative is an encoder that reads garbage counts.
    void* indirect_handle = nullptr;
    usize indirect_offset = 0;
    if (call.indirect.active()) {
        const auto* args =
            static_cast<const MetalBackendBuffer*>(call.indirect.buffer);
        indirect_handle = args->native_handle();
        if (indirect_handle == nullptr) {
            VITREO_TRACE("[metal] draw rejected: indirect argument buffer has "
                         "no native handle\n");
            return false;
        }
        // Learn (DrawIndexedInstancedIndirect): AlignedByteOffsetForArgs is a
        // DWORD-aligned byte offset. Metal requires the same of
        // indirectBufferOffset, so a misaligned offset is a caller error, not
        // something to round.
        if ((call.indirect.offset % 4) != 0) {
            VITREO_TRACE("[metal] draw rejected: indirect argument offset %u "
                         "is not DWORD aligned\n", call.indirect.offset);
            return false;
        }
        indirect_offset = call.indirect.offset + args->native_offset();
    }

    // native-state (R3c): the states the frontend built at creation, when it
    // names them. Their natives are the store's, exactly what the desc
    // lookup returns for the same desc (verify mode checks the pairing).
    const BackendNativeStates* const native_states =
        call.native_states != nullptr &&
                delta_record::active(delta_record::Part::NativeState)
            ? call.native_states
            : nullptr;
    if (native_states && delta_record::verify()) verify_native_states(call);
    draw.pipeline = pipeline->state();
    keep(draw.pipeline);
    if (native_states != nullptr && has_depth &&
        native_states->depth_stencil != nullptr) {
        draw.depth_stencil =
            static_cast<const MetalBackendDepthStencilState*>(
                native_states->depth_stencil)->native();
    } else {
        draw.depth_stencil = cache.depth_stencil_state(call, has_depth);
    }
    keep(draw.depth_stencil);
    draw.stencil_reference = call.stencil_reference;
    draw.writes_depth_stencil =
        draw.depth_stencil != nullptr &&
        (call.depth_write_enabled ||
         (call.stencil_test_enabled && call.stencil_write_mask != 0));
    if (vertex_handle != nullptr) {
        draw.vertex_buffers[0] = vertex_handle;
        keep(vertex_handle);
        draw.vertex_offsets[0] =
            call.vertex_buffer_offset + vertex->native_offset();
        draw.vertex_slots[0] =
            static_cast<u32>(MetalBackendPipeline::VERTEX_BUFFER_INDEX);
        draw.vertex_stream_count = 1;
        bind(vertex);
    }
    for (unsigned slot = 0; slot < BACKEND_MAX_VERTEX_BUFFERS; ++slot) {
        const BackendBuffer* stream = call.vertex_buffers[slot];
        if (stream == nullptr || !stream->valid()) continue;
        if (slot == 0 && stream == call.vertex_buffer) continue;
        const auto* metal_stream =
            static_cast<const MetalBackendBuffer*>(stream);
        void* handle = metal_stream->native_handle();
        if (handle == nullptr) continue;
        const u32 packed = draw.vertex_stream_count;
        if (packed >= draw.vertex_buffers.size()) break;
        draw.vertex_buffers[packed] = handle;
        keep(handle);
        draw.vertex_offsets[packed] =
            call.vertex_buffer_offsets[slot] + metal_stream->native_offset();
        draw.vertex_slots[packed] =
            static_cast<u32>(MetalBackendPipeline::VERTEX_BUFFER_BASE + slot);
        ++draw.vertex_stream_count;
        bind(metal_stream);
    }
    // The capture buffer rides the same packed stream list with its own slot,
    // so it needs no second binding path. There is always room: the list holds
    // 16 and the input-assembler streams are at most BACKEND_MAX_VERTEX_BUFFERS
    // plus the slot-0 alias.
    static_assert(BACKEND_MAX_VERTEX_BUFFERS + 2 <=
                      std::tuple_size_v<decltype(draw.vertex_buffers)>,
                  "no room for the capture buffer in the packed streams");
    if (call.vertex_capture_buffer != nullptr &&
        call.vertex_capture_buffer->valid()) {
        const auto* capture =
            static_cast<const MetalBackendBuffer*>(call.vertex_capture_buffer);
        void* handle = capture->native_handle();
        if (handle == nullptr) {
            VITREO_TRACE("[metal] draw rejected: vertex capture buffer has no "
                         "native handle\n");
            return false;
        }
        const u32 packed = draw.vertex_stream_count;
        draw.vertex_buffers[packed] = handle;
        keep(handle);
        draw.vertex_offsets[packed] =
            call.vertex_capture_buffer_offset + capture->native_offset();
        draw.vertex_slots[packed] = BACKEND_VERTEX_CAPTURE_BUFFER_INDEX;
        ++draw.vertex_stream_count;
        bind(capture);
    }

    // push-intern (slim_record.hpp): hand record_draw the caller's bytes,
    // which it copies into the recording's arena under its lock; otherwise
    // each stage's block goes into a vector of its own.
    if (call.vertex_constants_size <= 0xFFFFFFFFu &&
        call.fragment_constants_size <= 0xFFFFFFFFu &&
        (kDelta || slim_record::active(slim_record::Part::PushIntern))) {
        if (call.vertex_constants != nullptr && call.vertex_constants_size != 0) {
            draw.vs_push = static_cast<const u8*>(call.vertex_constants);
            draw.vs_push_size = static_cast<u32>(call.vertex_constants_size);
        }
        if (call.fragment_constants != nullptr &&
            call.fragment_constants_size != 0) {
            draw.ps_push = static_cast<const u8*>(call.fragment_constants);
            draw.ps_push_size = static_cast<u32>(call.fragment_constants_size);
        }
        draw.push_pending = draw.vs_push != nullptr || draw.ps_push != nullptr;
    } else {
        copy_cs_constants(draw.vs_const, call.vertex_constants,
                          call.vertex_constants_size);
        copy_cs_constants(draw.ps_const, call.fragment_constants,
                          call.fragment_constants_size);
    }

    for (unsigned i = 0; i < BACKEND_MAX_CONSTANT_BUFFER_SLOTS; ++i) {
        if (call.vertex_constant_buffers[i] != nullptr &&
            call.vertex_constant_buffers[i]->valid()) {
            const auto* metal_cb = static_cast<const MetalBackendBuffer*>(
                call.vertex_constant_buffers[i]);
            void* cb_handle = metal_cb->native_handle();
            if (cb_handle != nullptr) {
                draw.vs_cbuffer[i] = cb_handle;
                keep(cb_handle);
                bind(metal_cb);
                draw.vs_cbuffer_off[i] =
                    call.vertex_constant_buffer_offsets[i] +
                    metal_cb->native_offset();
            }
        }
        if (call.fragment_constant_buffers[i] != nullptr &&
            call.fragment_constant_buffers[i]->valid()) {
            const auto* metal_cb = static_cast<const MetalBackendBuffer*>(
                call.fragment_constant_buffers[i]);
            void* cb_handle = metal_cb->native_handle();
            if (cb_handle != nullptr) {
                draw.ps_cbuffer[i] = cb_handle;
                keep(cb_handle);
                bind(metal_cb);
                draw.ps_cbuffer_off[i] =
                    call.fragment_constant_buffer_offsets[i] +
                    metal_cb->native_offset();
            }
        }
    }

    // StructuredBuffer SRVs share the cbuffer command-stream array: the
    // encoder binds entry i at Metal buffer index 2 + i, so placing t#slot at
    // BACKEND_MAX_CONSTANT_BUFFER_SLOTS + slot lands it exactly on the
    // 2 + BACKEND_MAX_CONSTANT_BUFFER_SLOTS + slot index the translator
    // assigned, without a second binding path.
    for (unsigned slot = 0; slot < BACKEND_MAX_TEXTURE_SLOTS; ++slot) {
        const unsigned entry = BACKEND_MAX_CONSTANT_BUFFER_SLOTS + slot;
        if (entry >= draw.vs_cbuffer.size()) break;
        const BackendBuffer* vs_srv = call.vertex_srv_buffers[slot];
        if (vs_srv != nullptr && vs_srv->valid()) {
            const auto* metal_srv =
                static_cast<const MetalBackendBuffer*>(vs_srv);
            void* srv_handle = metal_srv->native_handle();
            if (srv_handle != nullptr) {
                draw.vs_cbuffer[entry] = srv_handle;
                keep(srv_handle);
                bind(metal_srv);
                draw.vs_cbuffer_off[entry] =
                    call.vertex_srv_buffer_offsets[slot] +
                    metal_srv->native_offset();
            }
        }
        const BackendBuffer* ps_srv = call.fragment_srv_buffers[slot];
        if (ps_srv != nullptr && ps_srv->valid()) {
            const auto* metal_srv =
                static_cast<const MetalBackendBuffer*>(ps_srv);
            void* srv_handle = metal_srv->native_handle();
            if (srv_handle != nullptr) {
                draw.ps_cbuffer[entry] = srv_handle;
                keep(srv_handle);
                bind(metal_srv);
                draw.ps_cbuffer_off[entry] =
                    call.fragment_srv_buffer_offsets[slot] +
                    metal_srv->native_offset();
            }
        }
    }

    for (unsigned slot = 0; slot < BACKEND_MAX_TEXTURE_SLOTS; ++slot) {
        void* texture_handle = nullptr;
        const BackendTexture2D* texture = call.textures[slot];
        if (texture != nullptr && texture->valid()) {
            texture_handle =
                static_cast<const MetalBackendTexture2D*>(texture)
                    ->native_shader_handle(call.texture_views[slot],
                                           call.samplers[slot].mipmaps_enabled);
        } else {
            const BackendTexture3D* texture_3d = call.textures_3d[slot];
            if (texture_3d != nullptr && texture_3d->valid()) {
                texture_handle =
                    static_cast<const MetalBackendTexture3D*>(texture_3d)
                        ->native_handle();
            }
        }
        // A typed buffer SRV is a texture in this slot, so it competes for the
        // same index as a Texture2D or Texture3D would. A t# register names
        // exactly one resource, so at most one of these is ever set.
        if (texture_handle == nullptr) {
            const BackendTextureBuffer* buffer_texture =
                call.fragment_srv_texture_buffers[slot];
            if (buffer_texture != nullptr && buffer_texture->valid()) {
                auto* typed = const_cast<MetalBackendTextureBuffer*>(
                    static_cast<const MetalBackendTextureBuffer*>(
                        buffer_texture));
                texture_handle = typed->native_shader_handle();
                if (texture_handle != nullptr) {
                    bind(typed);
                    if (void* storage = typed->storage_handle()) {
                        draw.resident.push_back(storage);
                        keep(storage);
                    }
                }
            }
        }
        // A sampler binds whether or not a texture shares its number: D3D's
        // s# and t# are independent, and a shader sampling t2 through s1
        // needs s1 bound with t1 empty.
        // Sampler states belong to the process-lifetime store
        // (MetalPassStateCache::release), so a draw holds no reference to
        // them: release_draw / retain_draw skip the sampler arrays.
        const auto* native =
            native_states ? static_cast<const MetalBackendSamplerState*>(
                                native_states->samplers[slot])
                          : nullptr;
        draw.samplers[slot] = native != nullptr
                                  ? native->native()
                                  : cache.sampler_state(call.samplers[slot]);
        if ((call.fragment_border_twin_mask & (1u << slot)) != 0) {
            void*& twin =
                draw.samplers[BACKEND_BORDER_TWIN_SAMPLER_BASE + slot];
            twin = native != nullptr
                       ? native->twin()
                       : cache.sampler_state(border_twin(call.samplers[slot]));
        }
        if (texture_handle == nullptr) continue;
        draw.textures[slot] = texture_handle;
        keep(texture_handle);
    }
    // Fragment-stage u# registers, from OMSetRenderTargetsAndUnorderedAccessViews.
    // A u# slot names a 2D texture, a volume, or a buffer -- never more than one.
    for (unsigned slot = 0; slot < BACKEND_MAX_TEXTURE_SLOTS; ++slot) {
        const unsigned texture_index = BACKEND_UAV_TEXTURE_BASE + slot;
        if (texture_index < draw.textures.size()) {
            void* uav_handle = nullptr;
            const BackendTexture2D* uav = call.fragment_uav_textures[slot];
            if (uav != nullptr && uav->valid()) {
                uav_handle = static_cast<const MetalBackendTexture2D*>(uav)
                                 ->native_shader_handle(
                                     call.fragment_uav_texture_views[slot],
                                     /*mipmaps_enabled=*/false);
            }
            if (uav_handle == nullptr) {
                const BackendTexture3D* volume =
                    call.fragment_uav_textures_3d[slot];
                if (volume != nullptr && volume->valid()) {
                    uav_handle =
                        static_cast<const MetalBackendTexture3D*>(volume)
                            ->native_handle();
                }
            }
            if (uav_handle == nullptr) {
                // RWBuffer<T>: a read_write texture_buffer over the buffer's
                // own storage, in the same texture window.
                const BackendTextureBuffer* typed =
                    call.fragment_uav_texture_buffers[slot];
                if (typed != nullptr && typed->valid()) {
                    auto* metal_typed = const_cast<MetalBackendTextureBuffer*>(
                        static_cast<const MetalBackendTextureBuffer*>(typed));
                    uav_handle = metal_typed->native_shader_handle();
                    if (uav_handle != nullptr) {
                        bind(metal_typed);
                        if (void* storage = metal_typed->storage_handle()) {
                            draw.resident.push_back(storage);
                            keep(storage);
                        }
                    }
                }
            }
            if (uav_handle != nullptr) {
                draw.textures[texture_index] = uav_handle;
                keep(uav_handle);
            }
        }
        const BackendBuffer* uav_buffer = call.fragment_uav_buffers[slot];
        if (uav_buffer != nullptr && uav_buffer->valid()) {
            const auto* metal_uav =
                static_cast<const MetalBackendBuffer*>(uav_buffer);
            void* handle = metal_uav->native_handle();
            if (handle != nullptr) {
                bind(metal_uav);
                draw.ps_uav_buffer[slot] = handle;
                keep(handle);
                draw.ps_uav_buffer_off[slot] =
                    call.fragment_uav_buffer_offsets[slot] +
                    metal_uav->native_offset();
            }
        }
    }
    for (unsigned k = 0; k < BACKEND_MAX_UAV_COUNTER_BINDINGS; ++k) {
        const BackendBuffer* counter = call.fragment_uav_counters[k];
        if (counter == nullptr || !counter->valid()) continue;
        const auto* metal_counter =
            static_cast<const MetalBackendBuffer*>(counter);
        void* handle = metal_counter->native_handle();
        if (handle == nullptr) continue;
        bind(metal_counter);
        draw.ps_uav_counter[k] = handle;
        keep(handle);
        draw.ps_uav_counter_off[k] = metal_counter->native_offset();
    }
    if (call.fragment_statistics_buffer != nullptr &&
        call.fragment_statistics_buffer->valid()) {
        const auto* record = static_cast<const MetalBackendBuffer*>(
            call.fragment_statistics_buffer);
        void* handle = record->native_handle();
        if (handle != nullptr) {
            bind(record);
            draw.ps_statistics = handle;
            keep(handle);
            draw.ps_statistics_off = record->native_offset();
        }
    }
    for (unsigned slot = 0; slot < BACKEND_MAX_TEXTURE_SLOTS; ++slot) {
        void* texture_handle = nullptr;
        const BackendTexture2D* texture = call.vertex_textures[slot];
        if (texture != nullptr && texture->valid()) {
            texture_handle =
                static_cast<const MetalBackendTexture2D*>(texture)
                    ->native_shader_handle(call.vertex_texture_views[slot],
                        call.vertex_samplers[slot].mipmaps_enabled);
        } else {
            const BackendTexture3D* texture_3d =
                call.vertex_textures_3d[slot];
            if (texture_3d != nullptr && texture_3d->valid()) {
                texture_handle =
                    static_cast<const MetalBackendTexture3D*>(texture_3d)
                        ->native_handle();
            }
        }
        if (texture_handle == nullptr) {
            const BackendTextureBuffer* buffer_texture =
                call.vertex_srv_texture_buffers[slot];
            if (buffer_texture != nullptr && buffer_texture->valid()) {
                auto* typed = const_cast<MetalBackendTextureBuffer*>(
                    static_cast<const MetalBackendTextureBuffer*>(
                        buffer_texture));
                texture_handle = typed->native_shader_handle();
                if (texture_handle != nullptr) {
                    bind(typed);
                    if (void* storage = typed->storage_handle()) {
                        draw.resident.push_back(storage);
                        keep(storage);
                    }
                }
            }
        }
        // Independent of the texture at the same number, as above.
        const auto* vertex_native =
            native_states ? static_cast<const MetalBackendSamplerState*>(
                                native_states->vertex_samplers[slot])
                          : nullptr;
        draw.vertex_samplers[slot] =
            vertex_native != nullptr
                ? vertex_native->native()
                : cache.sampler_state(call.vertex_samplers[slot]);
        if ((call.vertex_border_twin_mask & (1u << slot)) != 0) {
            void*& twin =
                draw.vertex_samplers[BACKEND_BORDER_TWIN_SAMPLER_BASE + slot];
            twin = vertex_native != nullptr
                       ? vertex_native->twin()
                       : cache.sampler_state(
                             border_twin(call.vertex_samplers[slot]));
        }
        if (texture_handle == nullptr) continue;
        draw.vertex_textures[slot] = texture_handle;
        keep(texture_handle);
    }

    draw.viewport = call.viewport;
    if (call.viewports != nullptr) {
        // All 16 slots, so an index past the D3D count lands on a disabled
        // slot: an empty scissor rect draws nothing, which is Windows'
        // behaviour (measured). Metal's own out-of-range rule is not relied on.
        const unsigned count =
            std::min(call.viewport_count, BACKEND_MAX_VIEWPORTS);
        draw.viewports.assign(BACKEND_MAX_VIEWPORTS, call.viewports[0]);
        draw.scissors.assign(BACKEND_MAX_VIEWPORTS, {0u, 0u, 0u, 0u});
        const auto clamp_u32 = [](float v) {
            return v > 0.0f ? static_cast<u32>(v) : 0u;
        };
        for (unsigned i = 0; i < count; ++i) {
            const auto& vp = call.viewports[i];
            draw.viewports[i] = vp;
            if (call.scissor_enabled && call.scissors != nullptr) {
                // Clipped to the render target's origin: a rect straddling
                // the top-left edge keeps its covered part (D3D11.3 15.7).
                const auto& r = call.scissors[i];
                const int left = std::max(r[0], 0);
                const int top = std::max(r[1], 0);
                draw.scissors[i] = {static_cast<u32>(left),
                                    static_cast<u32>(top),
                                    static_cast<u32>(std::max(r[2] - left, 0)),
                                    static_cast<u32>(std::max(r[3] - top, 0))};
            } else {
                draw.scissors[i] = {clamp_u32(vp[0]), clamp_u32(vp[1]),
                                    std::max(clamp_u32(vp[2]), 1u),
                                    std::max(clamp_u32(vp[3]), 1u)};
            }
        }
    }
    draw.blend_color = call.blend_color;
    if (call.scissor_enabled) {
        // D3D11.3 15.7: only the part of the rect on the render target can
        // be drawn -- a negative left/top is clipped to 0, not wrapped by the
        // unsigned Metal rect -- and an empty or inverted rect draws nothing
        // (measured, d3d11_a07_rs_probe `R3`). A zero-area Metal scissor
        // rect passes no fragment, as the viewport-array path relies on.
        const int left = std::max(call.scissor[0], 0);
        const int top = std::max(call.scissor[1], 0);
        draw.scissor = true;
        draw.scissor_x = static_cast<u32>(left);
        draw.scissor_y = static_cast<u32>(top);
        draw.scissor_w = static_cast<u32>(std::max(call.scissor[2] - left, 0));
        draw.scissor_h = static_cast<u32>(std::max(call.scissor[3] - top, 0));
    }
    switch (call.cull_mode) {
    case BackendCullMode::None:
        draw.cull_mode = MTLCullModeNone;
        break;
    case BackendCullMode::Front:
        draw.cull_mode = MTLCullModeFront;
        break;
    case BackendCullMode::Back:
        draw.cull_mode = MTLCullModeBack;
        break;
    }
    draw.fill_mode = call.fill_mode == BackendFillMode::Lines
                         ? MTLTriangleFillModeLines
                         : MTLTriangleFillModeFill;
    draw.depth_clip_mode = call.depth_clip_enabled
                               ? MTLDepthClipModeClip
                               : MTLDepthClipModeClamp;
    draw.depth_bias = call.depth_bias;
    draw.depth_bias_slope_scale = call.depth_bias_slope_scale;
    draw.depth_bias_clamp = call.depth_bias_clamp;
    draw.winding = call.front_counter_clockwise ? MTLWindingCounterClockwise
                                                : MTLWindingClockwise;
    draw.primitive = static_cast<u32>(metal_primitive(call.topology));
    draw.vertex_count = call.vertex_count;
    draw.index_count = call.index_count;
    draw.instance_count = call.instance_count;
    draw.first_vertex = call.first_vertex;
    draw.first_instance = call.first_instance;
    draw.base_vertex = call.base_vertex;
    draw.indexed = indexed;
    if (indexed) {
        bind(index);
        draw.index_buffer = index_handle;
        keep(index_handle);
        draw.index_offset =
            call.index_buffer_offset + index->native_offset();
        draw.index_type =
            call.index_32bit ? MTLIndexTypeUInt32 : MTLIndexTypeUInt16;
    }
    if (indirect_handle != nullptr) {
        bind(static_cast<const MetalBackendBuffer*>(call.indirect.buffer));
        draw.indirect_args = indirect_handle;
        keep(indirect_handle);
        draw.indirect_offset = indirect_offset;
    }
    if (patches) {
        const auto* factors =
            static_cast<const MetalBackendBuffer*>(call.tessellation_factors);
        void* handle = factors->native_handle();
        if (handle == nullptr) return false;
        bind(factors);
        draw.tess_factors = handle;
        draw.tess_factors_offset = factors->native_offset();
        keep(handle);
        draw.patch_count = call.patch_count;
        draw.patch_control_points = call.patch_control_points;
    }
    if constexpr (kDelta) {
        if (!publish_descriptor_tables_for(call.vertex_descriptor_tables,
                                           draw.vs_descriptor_tables,
                                           draw.resident, gen) ||
            !publish_descriptor_tables_for(call.fragment_descriptor_tables,
                                           draw.ps_descriptor_tables,
                                           draw.resident, gen)) {
            return false;
        }
    } else {
        if (!publish_descriptor_tables(call.vertex_descriptor_tables,
                                       draw.vs_descriptor_tables,
                                       draw.resident) ||
            !publish_descriptor_tables(call.fragment_descriptor_tables,
                                       draw.ps_descriptor_tables,
                                       draw.resident)) {
            return false;
        }
    }

    return true;
}

} // namespace

bool record_backend_draw(const BackendDrawCall& call, bool has_depth,
                         MetalPassStateCache& cache, u64 pass_id) {
    auto& queue = MetalSubmitQueue::instance();
    // delta-record (delta_record.hpp): resolve into this thread's scratch
    // draw and record what changed. A recording handed off meanwhile
    // (record_draw_delta false) resolves again under the new generation.
    while (queue.delta_eligible() &&
           delta_record::active(delta_record::Part::DeltaRecord)) {
        const u64 gen = queue.recording_generation();
        CsDraw& draw = MetalSubmitQueue::delta_scratch();
        if (!fill_backend_draw<true>(call, has_depth, cache, draw, gen)) {
            return false;
        }
        if (queue.record_draw_delta(gen, pass_id)) return true;
    }
    CsDraw draw{};
    if (!fill_backend_draw<false>(call, has_depth, cache, draw, 0)) {
        return false;
    }
    queue.record_draw(std::move(draw), pass_id);
    return true;
}

} // namespace vitreo::graphics
