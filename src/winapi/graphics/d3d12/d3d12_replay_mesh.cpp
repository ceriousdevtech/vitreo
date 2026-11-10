// SPDX-License-Identifier: LGPL-2.1-only

/// @file d3d12_replay_mesh.cpp
/// @brief Mesh draws that need more than one Metal draw: target-independent
/// / conservative rasterization of a mesh shader (a capture, then the
/// raster-emulation pass C), and an indirect DispatchMesh of a mesh shader
/// staged in device memory (chunks resolved on the GPU). Also pass C itself
/// (raster_emulation.hpp), shared with the vertex, geometry and domain
/// routes in d3d12_replay_draw.cpp.

#include "winapi/graphics/d3d12/d3d12_replay_internal.hpp"
#include "graphics/shaders/mesh_stage.hpp"
#include "graphics/shaders/raster_emulation.hpp"

#include <algorithm>
#include <cstring>
#include <memory>

namespace vitreo::winapi::d3d12 {

namespace {

// DirectX-Specs "Mesh Shader", DispatchMesh: each ThreadGroupCount is less
// than 64k and their product is at most 2^22 (the amplification shader's
// DispatchMesh has the same limits).
constexpr u64 kMaxDispatchMeshGroups = 1ull << 22;

/// The indirect-chunk resolve kernel: thread t writes chunk t's grid
/// (MTLDispatchThreadgroupsIndirectArguments) and its origin, a linear
/// group offset with the dispatch's X and Y and w = 1 (mesh_stage.hpp
/// offset_group_id then unflattens SV_GroupID). A dispatch past the limits
/// above launches nothing.
constexpr const char* kMeshChunkResolveMsl = R"MSL(#include <metal_stdlib>
using namespace metal;
struct vitreo_chunk_params { uint chunk; uint chunks; uint reserved0; uint reserved1; };
kernel void vitreo_mesh_chunk_resolve(constant vitreo_chunk_params& p [[buffer(0)]],
                                      device const uint* args [[buffer(1)]],
                                      device uint* grids [[buffer(2)]],
                                      device uint4* origins [[buffer(3)]],
                                      uint t [[thread_position_in_grid]])
{
    if (t >= p.chunks) return;
    const uint x = args[0], y = args[1], z = args[2];
    ulong total = ulong(x) * ulong(y) * ulong(z);
    if (x >= 65536u || y >= 65536u || z >= 65536u || total > 4194304ul) total = 0ul;
    const ulong first = ulong(t) * ulong(p.chunk);
    const uint n = first < total ? uint(min(total - first, ulong(p.chunk))) : 0u;
    grids[t * 3u] = n;
    grids[t * 3u + 1u] = 1u;
    grids[t * 3u + 2u] = 1u;
    origins[t] = uint4(uint(first), x, y, 1u);
}
)MSL";

struct ChunkParams {
    u32 chunk;
    u32 chunks;
    u32 reserved0;
    u32 reserved1;
};

}  // namespace

bool Replayer::read_indirect_words(const graphics::BackendIndirectArgs& indirect,
                                   u32* words, usize bytes) {
    if (indirect.buffer == nullptr || !indirect.buffer->valid()) return false;
    const u64 width = indirect.buffer->desc().byte_width;
    const u64 at = indirect.offset;
    if (at > width || static_cast<u64>(bytes) > width - at) return false;
    graphics::BackendBuffer* staging = nullptr;
    u64 staging_offset = 0;
    u8* host = nullptr;
    if (!reserve(bytes, staging, staging_offset, host)) return false;
    if (!f.m_backend.copy_buffer_region(staging, staging_offset, indirect.buffer, at,
                                        bytes)) {
        return false;
    }
    g_replay_cpu_waits.fetch_add(1, std::memory_order_relaxed);
    graphics::frame_profile::WaitScope profile_wait("replay:mesh-readback");
    if (auto completion = f.m_backend.flush_completion()) {
        if (!completion->wait()) return false;
    } else if (!f.m_backend.wait_idle()) {
        return false;
    }
    std::memcpy(words, host, bytes);
    return true;
}

bool Replayer::raster_pass_c(ReplayState& st, const std::string& msl,
                             std::unique_ptr<graphics::BackendPipeline>& slot,
                             graphics::BackendBuffer* records, u64 records_offset,
                             graphics::BackendBuffer* ids, u64 primitives,
                             graphics::BackendBuffer* limit, u64 limit_offset,
                             u32 limit_word) {
    PipelineState* pso = st.pso;
    if (primitives == 0) return true;
    if (primitives * graphics::kRasterVerticesPerPrimitive > 0xFFFFFFFFull) {
        return false;
    }
    // PIPELINE_STATISTICS: pass C runs the counting translation of the
    // raster-emulated pixel shader (statistics_fragment keeps its coverage
    // test), so PSInvocations counts the invocations the emulated
    // rasterizer runs. A pixel shader that reads its edge data from the
    // edge buffer (RasterEmulationOptions::edges_in_buffer) has that buffer
    // where the statistics record would be bound: PSInvocations is then
    // reported unmeasured, not invented.
    const bool edges_in_buffer = pso->raster.ps.edges_in_buffer;
    const std::shared_ptr<graphics::BackendBuffer> statistics =
        pso->ps.present && !edges_in_buffer ? statistics_record(st) : nullptr;
    if (edges_in_buffer && pipeline_statistics_open(st)) {
        statistics_unmeasured(st, 1u << graphics::STAT_PS_INVOCATIONS,
                              "PSInvocations of a raster-emulated pixel shader "
                              "past 124 input components (edge buffer)");
    }
    graphics::BackendPipeline* pipeline =
        statistics ? f.passthrough_variant(*pso, msl, pso->raster.statistics[&slot],
                                           graphics::BackendPrimitiveTopology::TriangleList,
                                           true)
                   : f.passthrough_variant(*pso, msl, slot,
                                           graphics::BackendPrimitiveTopology::TriangleList);
    if (pipeline == nullptr) return false;
    if (statistics && !pso->statistics_pixels) {
        statistics_unmeasured(st, 1u << graphics::STAT_PS_INVOCATIONS,
                              "PSInvocations of a pixel shader the "
                              "counting translation cannot express");
    }
    graphics::RasterEmulationParams params{};
    params.primitives = static_cast<u32>(primitives);
    params.limit_word = limit_word;
    const GraphicsState& g = pso->graphics;
    if (pso->raster.ps.mode == graphics::RasterEmulationMode::Conservative) {
        params.flags |= graphics::kRasterParamConservative;
    }
    if (g.depth_clip) params.flags |= graphics::kRasterParamDepthClip;
    if (g.sample_count > 1 &&
        (g.num_render_targets != 0 || g.dsv_format != DXGI_FORMAT_UNKNOWN)) {
        params.flags |= graphics::kRasterParamMultisampled;
    }
    if (g.front_ccw) params.flags |= graphics::kRasterParamFrontCounterClockwise;
    for (usize i = 0; i < params.viewports.size() && i < st.viewports.size(); ++i) {
        const Viewport& v = st.viewports[i];
        params.viewports[i] = {v.width, v.height, v.x, v.y};
    }
    graphics::BackendBufferDesc desc{};
    desc.byte_width = sizeof(params);
    desc.shader_resource = true;
    desc.unordered_access = true;
    auto param_buffer = f.m_backend.create_buffer(desc, &params);
    if (!param_buffer || !param_buffer->valid()) return false;
    graphics::BackendDrawCall draw{};
    draw.pipeline = pipeline;
    draw.topology = graphics::BackendPrimitiveTopology::TriangleList;
    draw.vertex_count =
        static_cast<u32>(primitives * graphics::kRasterVerticesPerPrimitive);
    draw.instance_count = 1;
    draw.vertex_srv_buffers[0] = records;
    draw.vertex_srv_buffer_offsets[0] = records_offset;
    draw.vertex_srv_buffers[1] = ids;
    draw.vertex_srv_buffers[2] = param_buffer.get();
    draw.vertex_srv_buffers[3] = limit;
    draw.vertex_srv_buffer_offsets[3] = limit_offset;
    std::unique_ptr<graphics::BackendBuffer> edges;
    if (edges_in_buffer) {
        // The pixel shader reads the edge data from the edge buffer
        // (RasterEmulationOptions::edges_in_buffer): written first by the
        // edge kernel over the same buffers, one thread a primitive.
        const auto source = pso->raster.edge_kernel_msl.find(msl);
        if (source == pso->raster.edge_kernel_msl.end()) return false;
        auto& kernel = pso->raster.edge_kernels[msl];
        if (!kernel) {
            graphics::BackendComputePipelineDesc kernel_desc{};
            kernel_desc.compute_shader_msl = source->second;
            kernel_desc.compute_entry_point = graphics::kRasterEdgeKernelEntryPoint;
            kernel_desc.cache_key = std::hash<std::string>{}(source->second);
            auto created = f.m_backend.create_compute_pipeline(kernel_desc);
            if (!created || !created->valid()) return false;
            kernel = std::move(created);
        }
        graphics::BackendBufferDesc edge_desc{};
        edge_desc.byte_width =
            static_cast<usize>(primitives * graphics::kRasterEdgeBufferSlots * 16u);
        edge_desc.shader_resource = true;
        edge_desc.unordered_access = true;
        edges = f.m_backend.create_buffer(edge_desc, nullptr);
        if (!edges || !edges->valid()) return false;
        graphics::BackendDispatchCall call{};
        call.pipeline = kernel.get();
        call.threads_per_group_x = 64;
        call.group_count_x = static_cast<u32>((primitives + 63u) / 64u);
        call.group_count_y = call.group_count_z = 1;
        constexpr unsigned base = graphics::BACKEND_SRV_BUFFER_BASE;
        call.buffers[base + 0] = records;
        call.buffer_offsets[base + 0] = records_offset;
        call.buffers[base + 1] = ids;
        call.buffers[base + 2] = param_buffer.get();
        call.buffers[base + 3] = limit;
        call.buffer_offsets[base + 3] = limit_offset;
        call.buffers[base + 4] = edges.get();
        if (!f.m_backend.dispatch(call)) return false;
        draw.fragment_statistics_buffer = edges.get();
    }
    std::vector<u32> pixel_push;
    if (!bind_stage(st, STAGE_PIXEL, pso->ps, draw.fragment_descriptor_tables, pixel_push)) {
        return false;
    }
    draw.fragment_constants = pixel_push.data();
    draw.fragment_constants_size = pixel_push.size() * sizeof(u32);
    if (statistics) draw.fragment_statistics_buffer = statistics.get();
    submit_draw(st, draw);
    return true;
}

bool Replayer::submit_mesh_raster(
    ReplayState& st, graphics::BackendDrawCall& call,
    const std::vector<std::pair<std::array<u32, 3>, std::array<u32, 3>>>& chunks) {
    PipelineState* pso = st.pso;
    const graphics::MeshStageLayout& l = pso->mesh.ms_layout;
    // Every primitive the grid may output: each mesh group's
    // max_primitives; an object group launches at most
    // kMeshSlotsPerObjectGroup mesh groups (Metal's mesh grid limit).
    u64 groups = u64{call.mesh->grid[0]} * call.mesh->grid[1] * call.mesh->grid[2];
    if (pso->mesh.as.present) groups *= graphics::kMeshSlotsPerObjectGroup;
    const u64 capacity = groups * std::max(l.state.max_primitives, 1u);
    const u64 primitive_bytes = 3u * u64{l.capture.size()} * 16u;
    constexpr u64 kMaxCaptureBytes = 256ull << 20;
    if (capacity == 0) return true;
    if (capacity > (kMaxCaptureBytes - 16u) / primitive_bytes) {
        unimplemented("ExecuteCommandLists",
                      "a conservative / target-independent mesh draw whose primitives "
                      "may need more than 256 MB of captured records");
        return false;
    }
    graphics::BackendBufferDesc desc{};
    desc.byte_width = static_cast<usize>(16u + capacity * primitive_bytes);
    desc.shader_resource = true;
    desc.unordered_access = true;
    auto capture = f.m_backend.create_buffer(desc, nullptr);
    const u32 header[graphics::kMeshCaptureHeaderWords] = {0u, static_cast<u32>(capacity), 0u, 0u};
    desc.byte_width = sizeof(header);
    auto head = f.m_backend.create_buffer(desc, header);
    graphics::BackendTexture2D* target = f.capture_target();
    if (!capture || !capture->valid() || !head || !head->valid() || target == nullptr ||
        !f.m_backend.copy_buffer_region(capture.get(), 0, head.get(), 0, sizeof(header))) {
        d3d12_diag("raster-emulated mesh draw: the capture could not be allocated");
        return false;
    }
    // The capture runs in a pass of its own on the 1x1 capture target (the
    // vertex route's pass A): the mesh pipeline draws no primitive, and
    // pass C reads what its mesh stage wrote after that pass.
    graphics::BackendRenderPassDesc pass{};
    pass.color_targets[0] = target;
    pass.preserve_color_contents = true;
    auto render_pass = f.m_backend.create_render_pass_standalone(pass);
    if (!render_pass || !render_pass->valid()) {
        d3d12_diag("raster-emulated mesh draw: the capture pass was not created");
        return false;
    }
    // The out-of-line mesh dispatch is the caller's; this draw's copy
    // (and each chunk's) carries the capture (the backend reads it during
    // the draw only).
    graphics::BackendMeshDraw mesh = *call.mesh;
    mesh.capture = capture.get();
    graphics::BackendDrawCall captured = call;
    captured.mesh = &mesh;
    bool drawn = true;
    if (chunks.empty()) {
        drawn = render_pass->draw(captured);
    } else {
        for (usize k = 0; k < chunks.size() && drawn; ++k) {
            graphics::BackendDrawCall part = captured;
            graphics::BackendMeshDraw part_mesh = mesh;
            part.mesh = &part_mesh;
            part_mesh.grid = {chunks[k].second[0], chunks[k].second[1], chunks[k].second[2]};
            // .w: the amplified chunk's slot counter (dispatch_mesh_view).
            part_mesh.group_origin = {chunks[k].first[0], chunks[k].first[1], chunks[k].first[2],
                                      mesh.group_origin[3] != 0
                                          ? mesh.group_origin[3] + static_cast<u32>(k)
                                          : 0u};
            part_mesh.serialize = k != 0;
            drawn = render_pass->draw(part);
        }
    }
    if (!render_pass->end() || !drawn) {
        d3d12_diag("raster-emulated mesh draw: the capture was not submitted");
        return false;
    }
    if (!raster_pass_c(st, pso->raster.vertex_msl, pso->raster.pipeline, capture.get(),
                       16u, capture.get(), capacity, capture.get(), 0u, 0u)) {
        d3d12_diag("raster-emulated mesh draw: pass C was not submitted");
        return false;
    }
    return true;
}

bool Replayer::submit_mesh_indirect_chunks(ReplayState& st, graphics::BackendDrawCall& call,
                                           const graphics::BackendIndirectArgs& indirect) {
    PipelineState* pso = st.pso;
    // One scratch slot per mesh group of a chunk, the chunk a linear run of
    // the dispatch's groups; enough chunks to cover the largest dispatch
    // D3D12 allows. The kernel writes each chunk's grid (0 past the
    // dispatch) and origin on the GPU timeline: nothing is read back.
    constexpr u64 kScratchBytes = 256ull << 20;
    const u64 slot_bytes = u64{pso->mesh.ms_layout.device_staging_words()} * 4u;
    const u64 chunk = std::min<u64>(kMaxDispatchMeshGroups,
                                    std::max<u64>(1, kScratchBytes / slot_bytes));
    const u64 chunks = (kMaxDispatchMeshGroups + chunk - 1) / chunk;
    if (!f.m_mesh_chunk_resolve) {
        graphics::BackendComputePipelineDesc desc{};
        desc.compute_shader_msl = kMeshChunkResolveMsl;
        desc.compute_entry_point = "vitreo_mesh_chunk_resolve";
        desc.cache_key = 0x3E5C4A2Bull;
        f.m_mesh_chunk_resolve = f.m_backend.create_compute_pipeline(desc);
    }
    graphics::BackendComputePipeline* kernel =
        f.m_mesh_chunk_resolve && f.m_mesh_chunk_resolve->valid() ? f.m_mesh_chunk_resolve.get()
                                                                  : nullptr;
    graphics::BackendBufferDesc desc{};
    desc.shader_resource = true;
    desc.unordered_access = true;
    desc.byte_width = static_cast<usize>(chunk * slot_bytes);
    auto scratch = f.m_backend.create_buffer(desc, nullptr);
    desc.byte_width = static_cast<usize>(chunks * 12u);
    auto grids = f.m_backend.create_buffer(desc, nullptr);
    desc.byte_width = static_cast<usize>(chunks * 16u);
    auto origins = f.m_backend.create_buffer(desc, nullptr);
    if (kernel == nullptr || !scratch || !scratch->valid() || !grids || !grids->valid() ||
        !origins || !origins->valid()) {
        unimplemented("ExecuteCommandLists",
                      "an indirect DispatchMesh of a mesh shader staged in device memory "
                      "whose chunk resolve cannot be recorded");
        return false;
    }
    ChunkParams params{static_cast<u32>(chunk), static_cast<u32>(chunks), 0u, 0u};
    graphics::BackendDispatchCall resolve{};
    resolve.pipeline = kernel;
    resolve.group_count_x = static_cast<unsigned>((chunks + 63u) / 64u);
    resolve.group_count_y = resolve.group_count_z = 1;
    resolve.threads_per_group_x = 64;
    resolve.constants = &params;
    resolve.constants_size = sizeof(params);
    resolve.buffers[1] = indirect.buffer;
    resolve.buffer_offsets[1] = indirect.offset;
    resolve.buffers[2] = grids.get();
    resolve.buffers[3] = origins.get();
    if (!f.m_backend.dispatch(resolve)) {
        d3d12_diag("indirect DispatchMesh: the chunk resolve was refused");
        return false;
    }
    for (u64 k = 0; k < chunks; ++k) {
        // Each chunk its own out-of-line mesh dispatch.
        graphics::BackendDrawCall part = call;
        graphics::BackendMeshDraw part_mesh = *call.mesh;
        part.mesh = &part_mesh;
        part_mesh.scratch = scratch.get();
        part_mesh.grid = {0u, 0u, 0u};
        part_mesh.indirect = grids.get();
        part_mesh.indirect_offset = k * 12u;
        part_mesh.origin_buffer = origins.get();
        part_mesh.origin_offset = k * 16u;
        part_mesh.serialize = k != 0;
        if (!submit_draw(st, part)) return false;
    }
    return true;
}

}  // namespace vitreo::winapi::d3d12
