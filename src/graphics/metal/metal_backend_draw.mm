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
#include "graphics/metal/metal_mesh_pipeline.hpp"  // WS5

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
    if (call.pipeline == nullptr || !call.pipeline->valid()) {
        VITREO_TRACE("[metal] draw rejected: pipeline is missing/invalid\n");
        return false;
    }
    // ---- WS5 begin ----
    const bool mesh_draw = call.pipeline->is_mesh();
    if (mesh_draw != call.mesh_active()) {
        VITREO_TRACE(mesh_draw
                         ? "[metal] draw rejected: mesh pipeline without mesh "
                           "dispatch\n"
                         : "[metal] draw rejected: mesh dispatch on a vertex "
                           "pipeline\n");
        return false;
    }
    void* const pipeline_state =
        mesh_draw
            ? static_cast<const MetalMeshPipeline*>(call.pipeline)->state()
            : static_cast<const MetalBackendPipeline*>(call.pipeline)->state();
    if (pipeline_state == nullptr) return false;
    // ---- WS5 end ----
    // A dynamic-stride pipeline: each stream it reads carries the draw's
    // stride in its packed slot word's bits 8.. (encode_draw splits them).
    const auto* const render_pipeline =
        mesh_draw ? nullptr : static_cast<const MetalBackendPipeline*>(call.pipeline);
    const auto stride_bits = [&](unsigned slot) -> u32 {
        if (render_pipeline == nullptr || !render_pipeline->dynamic_vertex_strides() ||
            slot >= BACKEND_MAX_VERTEX_BUFFERS ||
            (render_pipeline->dynamic_stream_mask() & (1u << slot)) == 0) {
            return 0u;
        }
        return call.vertex_buffer_strides[slot] << 8u;
    };
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
    if (!patches && call.indirect.buffer == nullptr && !mesh_draw &&  // WS5
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
            VITREO_TRACE("[metal] draw rejected: indirect argument offset %llu "
                         "is not DWORD aligned\n",
                         static_cast<unsigned long long>(call.indirect.offset));
            return false;
        }
        if (!metal_buffer_offset(call.indirect.offset, args->native_offset(),
                                 indirect_offset)) {
            return false;
        }
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
    draw.pipeline = pipeline_state;  // WS5: a vertex or a mesh pipeline
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
    draw.indirect_resources_read_only = call.indirect_resources_read_only;
    draw.stencil_reference = call.stencil_reference;
    // With no depth-stencil state the pass has no such attachment to write.
    const DepthStencilWrites ds_writes = depth_stencil_writes_of(call);
    draw.writes_depth = draw.depth_stencil != nullptr && ds_writes.depth;
    draw.writes_stencil = draw.depth_stencil != nullptr && ds_writes.stencil;
    draw.writes_depth_stencil = draw.writes_depth || draw.writes_stencil;
    draw.writes_color = call.writes_color;
    const bool pulling = call.vertex_pull_table != nullptr;
    if (pulling) {
        // Vertex pulling: the shader reads every slot through the table's
        // device addresses, so the slots' buffers are made resident (read)
        // instead of bound, and the table takes the window's first index.
        const auto* table =
            static_cast<const MetalBackendBuffer*>(call.vertex_pull_table);
        void* table_handle =
            table->valid() ? table->native_handle() : nullptr;
        usize table_off = 0;
        if (table_handle == nullptr ||
            !metal_buffer_offset(call.vertex_pull_table_offset,
                                 table->native_offset(), table_off)) {
            VITREO_TRACE("[metal] draw rejected: the vertex-pull table has "
                         "no native handle\n");
            return false;
        }
        draw.vertex_buffers[0] = table_handle;
        keep(table_handle);
        draw.vertex_offsets[0] = table_off;
        draw.vertex_slots[0] = BACKEND_VERTEX_BUFFER_BASE;
        draw.vertex_stream_count = 1;
        bind(table);
        for (unsigned slot = 0; slot < BACKEND_MAX_VERTEX_BUFFERS; ++slot) {
            const BackendBuffer* stream = call.vertex_buffers[slot];
            if (stream == nullptr || !stream->valid()) continue;
            const auto* metal_stream =
                static_cast<const MetalBackendBuffer*>(stream);
            void* handle = metal_stream->native_handle();
            if (handle == nullptr) continue;
            if (std::find(draw.resident.begin(), draw.resident.end(),
                          handle) == draw.resident.end()) {
                draw.resident.push_back(handle);
            }
            keep(handle);
            bind(metal_stream);
        }
    }
    if (!pulling && vertex_handle != nullptr) {
        usize stream_off = 0;
        if (metal_buffer_offset(call.vertex_buffer_offset, vertex->native_offset(),
                                stream_off)) {
            draw.vertex_buffers[0] = vertex_handle;
            keep(vertex_handle);
            draw.vertex_offsets[0] = stream_off;
            draw.vertex_slots[0] =
                static_cast<u32>(MetalBackendPipeline::VERTEX_BUFFER_INDEX) |
                stride_bits(0);
            draw.vertex_stream_count = 1;
            bind(vertex);
        }
    }
    for (unsigned slot = 0; slot < BACKEND_MAX_VERTEX_BUFFERS && !pulling;
         ++slot) {
        const BackendBuffer* stream = call.vertex_buffers[slot];
        if (stream == nullptr || !stream->valid()) continue;
        if (slot == 0 && stream == call.vertex_buffer) continue;
        const auto* metal_stream =
            static_cast<const MetalBackendBuffer*>(stream);
        void* handle = metal_stream->native_handle();
        if (handle == nullptr) continue;
        if (!vertex_stream_bindable(slot)) {
            static std::atomic<unsigned> logged{0};
            if (logged.fetch_add(1, std::memory_order_relaxed) == 0) {
                std::fprintf(stderr,
                             "[vitreo] UNIMPLEMENTED metal!draw: input stream "
                             "%u is past Metal's buffer argument table\n",
                             slot);
            }
            return false;
        }
        const u32 packed = draw.vertex_stream_count;
        if (packed >= draw.vertex_buffers.size()) {
            static std::atomic<unsigned> logged{0};
            if (logged.fetch_add(1, std::memory_order_relaxed) == 0) {
                std::fprintf(stderr,
                             "[vitreo] UNIMPLEMENTED metal!draw: no room to "
                             "bind input stream %u\n",
                             slot);
            }
            return false;
        }
        usize stream_off = 0;
        if (!metal_buffer_offset(call.vertex_buffer_offsets[slot],
                                 metal_stream->native_offset(), stream_off)) {
            continue;
        }
        draw.vertex_buffers[packed] = handle;
        keep(handle);
        draw.vertex_offsets[packed] = stream_off;
        draw.vertex_slots[packed] =
            static_cast<u32>(vertex_stream_buffer_index(slot)) | stride_bits(slot);
        ++draw.vertex_stream_count;
        bind(metal_stream);
    }
    // The capture buffer rides the same packed stream list with its own slot.
    // Only bindable streams are packed; a stream past Metal's table fails the
    // draw above rather than being dropped. The slot-0 alias and the capture
    // buffer need two extra entries.
    static_assert(BACKEND_BINDABLE_VERTEX_STREAMS + 2 <=
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
        usize capture_off = 0;
        if (!metal_buffer_offset(call.vertex_capture_buffer_offset,
                                 capture->native_offset(), capture_off)) {
            VITREO_TRACE("[metal] draw rejected: vertex capture offset does "
                         "not fit\n");
            return false;
        }
        draw.vertex_buffers[packed] = handle;
        keep(handle);
        draw.vertex_offsets[packed] = capture_off;
        draw.vertex_slots[packed] = BACKEND_VERTEX_CAPTURE_BUFFER_INDEX;
        ++draw.vertex_stream_count;
        bind(capture);
    }
    // ---- WS5 begin ----
    // A mesh draw has no input assembler: its geometry comes from the object
    // and mesh stages, which read the expansion buffers at their own window.
    if (mesh_draw) {
        if (draw.vertex_stream_count != 0 || indexed ||
            call.indirect.buffer != nullptr || patches) {
            VITREO_TRACE("[metal] draw rejected: a mesh draw with "
                         "input-assembler state\n");
            return false;
        }
        static_assert(BACKEND_MAX_GS_EXPANSION_BUFFERS <=
                      std::tuple_size_v<decltype(draw.vertex_buffers)>);
        for (unsigned i = 0; i < BACKEND_MAX_GS_EXPANSION_BUFFERS; ++i) {
            const BackendBuffer* buffer = call.mesh->expansion_buffers[i];
            if (buffer == nullptr) continue;
            const auto* metal_buffer =
                static_cast<const MetalBackendBuffer*>(buffer);
            void* handle = buffer->valid() ? metal_buffer->native_handle()
                                           : nullptr;
            if (handle == nullptr) {
                VITREO_TRACE("[metal] draw rejected: mesh expansion buffer "
                             "has no native handle\n");
                return false;
            }
            const u32 packed = draw.vertex_stream_count;
            usize expansion_off = 0;
            if (!metal_buffer_offset(call.mesh->expansion_offsets[i],
                                     metal_buffer->native_offset(),
                                     expansion_off)) {
                VITREO_TRACE("[metal] draw rejected: mesh expansion offset "
                             "does not fit\n");
                return false;
            }
            draw.vertex_buffers[packed] = handle;
            keep(handle);
            draw.vertex_offsets[packed] = expansion_off;
            draw.vertex_slots[packed] = BACKEND_GS_EXPANSION_BUFFER_BASE + i;
            ++draw.vertex_stream_count;
            bind(metal_buffer);
        }
        const auto* mesh =
            static_cast<const MetalMeshPipeline*>(call.pipeline);
        draw.mesh_object_groups = call.mesh->object_threadgroups;
        draw.mesh_groups = call.mesh->mesh_groups;
        draw.mesh_object_threads = mesh->threads_per_object_threadgroup();
        draw.mesh_threads = mesh->threads_per_mesh_threadgroup();
        draw.barrier_before_mesh = call.mesh->barrier_before_mesh;
        if (call.mesh->d3d12_dispatch()) {
            for (unsigned k = 0; k < 3; ++k) {
                draw.mesh_grid[k] = call.mesh->grid[k];
                draw.mesh_object_shape[k] = call.mesh->object_threads[k];
                draw.mesh_shape[k] = call.mesh->mesh_threads[k];
            }
            if (call.mesh->indirect != nullptr) {
                const auto* args = static_cast<const MetalBackendBuffer*>(call.mesh->indirect);
                void* handle = args->valid() ? args->native_handle() : nullptr;
                usize at = 0;
                if (handle == nullptr ||
                    !metal_buffer_offset(call.mesh->indirect_offset, args->native_offset(), at)) {
                    VITREO_TRACE("[metal] draw rejected: mesh indirect arguments unusable\n");
                    return false;
                }
                bind(args);
                keep(handle);
                draw.mesh_indirect = handle;
                draw.mesh_indirect_offset = at;
            }
            copy_cs_constants(draw.os_const, call.mesh->object_constants,
                              call.mesh->object_constants_size);
            if (call.mesh->statistics != nullptr) {
                const auto* record = static_cast<const MetalBackendBuffer*>(call.mesh->statistics);
                void* handle = record->valid() ? record->native_handle() : nullptr;
                if (handle == nullptr) {
                    VITREO_TRACE("[metal] draw rejected: mesh statistics record unusable\n");
                    return false;
                }
                bind(record);
                keep(handle);
                draw.mesh_statistics = handle;
                draw.mesh_statistics_offset = record->native_offset();
            }
            if (call.mesh->scratch != nullptr) {
                const auto* scratch = static_cast<const MetalBackendBuffer*>(call.mesh->scratch);
                void* handle = scratch->valid() ? scratch->native_handle() : nullptr;
                if (handle == nullptr) {
                    VITREO_TRACE("[metal] draw rejected: mesh scratch unusable\n");
                    return false;
                }
                bind(scratch);
                keep(handle);
                draw.mesh_scratch = handle;
                draw.mesh_scratch_offset = scratch->native_offset();
                for (unsigned k = 0; k < 4; ++k) draw.mesh_origin[k] = call.mesh->group_origin[k];
                draw.mesh_serialize = call.mesh->serialize;
                if (call.mesh->origin_buffer != nullptr) {
                    const auto* origin = static_cast<const MetalBackendBuffer*>(call.mesh->origin_buffer);
                    void* origin_handle = origin->valid() ? origin->native_handle() : nullptr;
                    usize at = 0;
                    if (origin_handle == nullptr ||
                        !metal_buffer_offset(call.mesh->origin_offset, origin->native_offset(), at)) {
                        VITREO_TRACE("[metal] draw rejected: mesh origin buffer unusable\n");
                        return false;
                    }
                    bind(origin);
                    keep(origin_handle);
                    draw.mesh_origin_buffer = origin_handle;
                    draw.mesh_origin_buffer_offset = at;
                }
            }
            if (call.mesh->capture != nullptr) {
                const auto* capture = static_cast<const MetalBackendBuffer*>(call.mesh->capture);
                void* handle = capture->valid() ? capture->native_handle() : nullptr;
                if (handle == nullptr) {
                    VITREO_TRACE("[metal] draw rejected: mesh capture buffer unusable\n");
                    return false;
                }
                bind(capture);
                keep(handle);
                draw.mesh_capture = handle;
                draw.mesh_capture_offset = capture->native_offset();
            }
            // D3D12 view instancing (mesh_stage.hpp vi_capture) records.
            const auto view_capture = [&](const BackendBuffer* buffer, void*& out, usize& offset) {
                if (buffer == nullptr) return true;
                const auto* records = static_cast<const MetalBackendBuffer*>(buffer);
                void* handle = records->valid() ? records->native_handle() : nullptr;
                if (handle == nullptr) return false;
                bind(records);
                keep(handle);
                out = handle;
                offset = records->native_offset();
                return true;
            };
            if (!view_capture(call.mesh->object_capture, draw.object_view_capture,
                              draw.object_view_capture_offset) ||
                !view_capture(call.mesh->mesh_capture, draw.mesh_view_capture,
                              draw.mesh_view_capture_offset)) {
                VITREO_TRACE("[metal] draw rejected: mesh view-instancing records unusable\n");
                return false;
            }
        }
    }
    // ---- WS5 end ----

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
                usize cb_off = 0;
                if (metal_buffer_offset(call.vertex_constant_buffer_offsets[i],
                                        metal_cb->native_offset(), cb_off)) {
                    draw.vs_cbuffer[i] = cb_handle;
                    keep(cb_handle);
                    bind(metal_cb);
                    draw.vs_cbuffer_off[i] = cb_off;
                }
            }
        }
        if (call.fragment_constant_buffers[i] != nullptr &&
            call.fragment_constant_buffers[i]->valid()) {
            const auto* metal_cb = static_cast<const MetalBackendBuffer*>(
                call.fragment_constant_buffers[i]);
            void* cb_handle = metal_cb->native_handle();
            if (cb_handle != nullptr) {
                usize cb_off = 0;
                if (metal_buffer_offset(
                        call.fragment_constant_buffer_offsets[i],
                        metal_cb->native_offset(), cb_off)) {
                    draw.ps_cbuffer[i] = cb_handle;
                    keep(cb_handle);
                    bind(metal_cb);
                    draw.ps_cbuffer_off[i] = cb_off;
                }
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
                usize srv_off = 0;
                if (metal_buffer_offset(call.vertex_srv_buffer_offsets[slot],
                                        metal_srv->native_offset(), srv_off)) {
                    draw.vs_cbuffer[entry] = srv_handle;
                    keep(srv_handle);
                    bind(metal_srv);
                    draw.vs_cbuffer_off[entry] = srv_off;
                }
            }
        }
        const BackendBuffer* ps_srv = call.fragment_srv_buffers[slot];
        if (ps_srv != nullptr && ps_srv->valid()) {
            const auto* metal_srv =
                static_cast<const MetalBackendBuffer*>(ps_srv);
            void* srv_handle = metal_srv->native_handle();
            if (srv_handle != nullptr) {
                usize srv_off = 0;
                if (metal_buffer_offset(call.fragment_srv_buffer_offsets[slot],
                                        metal_srv->native_offset(), srv_off)) {
                    draw.ps_cbuffer[entry] = srv_handle;
                    keep(srv_handle);
                    bind(metal_srv);
                    draw.ps_cbuffer_off[entry] = srv_off;
                }
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
    // Their storage is appended after the read-only residency prefix.
    std::vector<void*> uav_storage;
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
                            uav_storage.push_back(storage);
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
                usize uav_off = 0;
                if (!metal_buffer_offset(call.fragment_uav_buffer_offsets[slot],
                                         metal_uav->native_offset(), uav_off)) {
                    continue;
                }
                bind(metal_uav);
                draw.ps_uav_buffer[slot] = handle;
                keep(handle);
                draw.ps_uav_buffer_off[slot] = uav_off;
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
        usize counter_off = 0;
        if (!metal_buffer_offset(call.fragment_uav_counter_offsets[k],
                                 metal_counter->native_offset(), counter_off)) {
            continue;
        }
        bind(metal_counter);
        draw.ps_uav_counter[k] = handle;
        keep(handle);
        draw.ps_uav_counter_off[k] = counter_off;
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
        if (!metal_buffer_offset(call.index_buffer_offset,
                                 index->native_offset(), draw.index_offset)) {
            return false;
        }
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
    // b#, t# and s# are read-only views. Only the u# set is writable.
    // Declaring a shader-resource view writable makes Metal hazard it
    // against every later reader, including a tile that still holds it.
    auto publish_sets = [&](unsigned first, unsigned last) {
        if constexpr (kDelta) {
            return publish_descriptor_tables_for(
                       call.vertex_descriptor_tables, draw.vs_descriptor_tables,
                       draw.resident, gen, first, last) &&
                   publish_descriptor_tables_for(
                       call.fragment_descriptor_tables, draw.ps_descriptor_tables,
                       draw.resident, gen, first, last) &&
                   publish_descriptor_tables_for(
                       call.object_descriptor_tables(), draw.os_descriptor_tables,
                       draw.resident, gen, first, last);
        } else {
            return publish_descriptor_tables(
                       call.vertex_descriptor_tables, draw.vs_descriptor_tables,
                       draw.resident, first, last, &draw.resident_logs) &&
                   publish_descriptor_tables(
                       call.fragment_descriptor_tables, draw.ps_descriptor_tables,
                       draw.resident, first, last, &draw.resident_logs) &&
                   publish_descriptor_tables(
                       call.object_descriptor_tables(), draw.os_descriptor_tables,
                       draw.resident, first, last, &draw.resident_logs);
        }
    };
    if (!publish_sets(0, BACKEND_DESCRIPTOR_SET_UAV)) return false;
    draw.resident_read_count = static_cast<u32>(draw.resident.size());
    draw.resident.insert(draw.resident.end(), uav_storage.begin(),
                         uav_storage.end());
    if (!publish_sets(BACKEND_DESCRIPTOR_SET_UAV, BACKEND_MAX_DESCRIPTOR_SETS)) {
        return false;
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
    // WS5: a mesh draw is recorded whole (CsDraw's mesh fields are not part
    // of the delta encoding); the full record breaks the delta chain.
    // A heap window (a persistent table) names its residency by a token,
    // which the delta encoding does not carry: that draw is recorded whole.
    // So is one binding an acceleration structure (its bottom levels'
    // token, metal_rt_bottom_level_token).
    const auto persistent = [](const auto& tables) {
        for (const BackendDescriptorTable* table : tables) {
            const auto* metal = static_cast<const MetalBackendDescriptorTable*>(table);
            if (table != nullptr &&
                (metal->persistent() || metal->holds_acceleration_structures())) {
                return true;
            }
        }
        return false;
    };
    const bool windowed = persistent(call.vertex_descriptor_tables) ||
                          persistent(call.fragment_descriptor_tables) ||
                          persistent(call.object_descriptor_tables());
    while (!windowed && !call.mesh_active() &&
           queue.delta_eligible() &&
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
