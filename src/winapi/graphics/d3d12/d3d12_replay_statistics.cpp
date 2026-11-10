// SPDX-License-Identifier: LGPL-2.1-only
/// @file d3d12_replay_statistics.cpp
/// @brief PIPELINE_STATISTICS[1] and SO_STATISTICS_STREAMn queries.
///
/// D3D12_QUERY_DATA_PIPELINE_STATISTICS is D3D11's record (d3d12.h, same
/// field order), and its counters follow the D3D11.3 functional spec 20.4.7
/// that D3D12 inherits; where the spec gives a range, the rules are WARP's as
/// measured for D3D11 (graphics/shaders/pipeline_stats.hpp) and checked for
/// D3D12 by tools/oracle_probe/d3d12_a06_queries_probe. The replay counts
/// what the CPU knows -- IAVertices, a direct non-indexed draw's assembly,
/// a lowered draw's CPU-assembled keys, CSInvocations of a direct dispatch
/// -- into each open query's result, and everything else on the GPU
/// timeline into one record per Begin/End interval: an indexed or indirect
/// draw is assembled by vitreo_ia_resolve and counted by
/// vitreo_stats_vertices, the vertex shader of a rasterized draw captures
/// its outputs while it draws and vitreo_stats_primitives clips them
/// (CPrimitives), the pixel shader counts PSInvocations on entry, a
/// geometry expansion and a tessellator batch count their stages
/// (vitreo_stats_geometry / _tessellation), and an indirect dispatch's
/// argument block is copied in order. PIPELINE_STATISTICS1 adds
/// ASInvocations, MSInvocations and MSPrimitives: this device runs no
/// amplification or mesh shader (DispatchMesh fails, MeshShaderTier is
/// NOT_SUPPORTED), so they are 0 by count, not by default.
///
/// SO_STATISTICS_STREAMn: D3D12_QUERY_DATA_SO_STATISTICS {NumPrimitivesWritten,
/// PrimitivesStorageNeeded} of stream n, from the {needed, written} words
/// vitreo_so_prepare adds per stream (stream_out_repack.hpp).
///
/// ResolveQueryData runs after the GPU work recorded before it (see
/// Replayer::resolve_query). A counter a draw could not count (a vertex
/// shader the capture cannot express, a capacity past the size limit) has
/// no number to give; ResolveQueryData has no failure return, so its value
/// is what was counted and a one-time UNIMPLEMENTED line names it.

#include "winapi/graphics/d3d12/d3d12_replay_internal.hpp"
#include "graphics/shaders/mesh_stage.hpp"
#include "graphics/shaders/ia_resolve.hpp"
#include "graphics/shaders/pipeline_stats.hpp"
#include "graphics/shaders/stream_out_repack.hpp"

#include <algorithm>
#include <cstring>

namespace vitreo::winapi::d3d12 {

namespace {

constexpr u32 kAllCounters = (1u << graphics::STATS_COUNTERS) - 1u;
constexpr u32 kIaCounters =
    (1u << graphics::STAT_IA_VERTICES) | (1u << graphics::STAT_IA_PRIMITIVES) |
    (1u << graphics::STAT_VS_INVOCATIONS) |
    (1u << graphics::STAT_HS_INVOCATIONS) |
    (1u << graphics::STAT_C_INVOCATIONS) | (1u << graphics::STAT_C_PRIMITIVES);
/// Capture and assembly buffers larger than this are declined: the draw is
/// still drawn, its counters reported unmeasured.
constexpr u64 kMaxStatisticsBytes = 256ull << 20;

[[nodiscard]] bool is_pipeline_query(u32 type) {
    return type == D3D12_QUERY_TYPE_PIPELINE_STATISTICS ||
           type == D3D12_QUERY_TYPE_PIPELINE_STATISTICS1;
}

[[nodiscard]] bool is_so_query(u32 type) {
    return type >= D3D12_QUERY_TYPE_SO_STATISTICS_STREAM0 &&
           type <= D3D12_QUERY_TYPE_SO_STATISTICS_STREAM3;
}

/// Vertices of an input-assembler primitive; 0 for UNDEFINED (nothing is
/// drawn).
[[nodiscard]] u32 ia_vertices(u32 topology) {
    switch (topology) {
    case D3D_PRIMITIVE_TOPOLOGY_POINTLIST: return 1;
    case D3D_PRIMITIVE_TOPOLOGY_LINELIST:
    case D3D_PRIMITIVE_TOPOLOGY_LINESTRIP: return 2;
    case D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST:
    case D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP: return 3;
    case D3D_PRIMITIVE_TOPOLOGY_LINELIST_ADJ:
    case D3D_PRIMITIVE_TOPOLOGY_LINESTRIP_ADJ: return 4;
    case D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST_ADJ:
    case D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP_ADJ: return 6;
    default:
        if (topology >= D3D_PRIMITIVE_TOPOLOGY_1_CONTROL_POINT_PATCHLIST &&
            topology <= D3D_PRIMITIVE_TOPOLOGY_32_CONTROL_POINT_PATCHLIST) {
            return topology - D3D_PRIMITIVE_TOPOLOGY_1_CONTROL_POINT_PATCHLIST +
                   1u;
        }
        return 0;
    }
}

/// 0 list (and patch list), 1 strip, 2 triangle strip with adjacency.
[[nodiscard]] u32 ia_assembly(u32 topology) {
    switch (topology) {
    case D3D_PRIMITIVE_TOPOLOGY_LINESTRIP:
    case D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP:
    case D3D_PRIMITIVE_TOPOLOGY_LINESTRIP_ADJ: return 1;
    case D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP_ADJ: return 2;
    default: return 0;
    }
}

/// The rasterized vertices of an input-assembler primitive (adjacency
/// primitives draw 1,2 of a line's four and 0,2,4 of a triangle's six).
void set_raster_pick(u32 vpp, graphics::StatsKernelParams& params) {
    switch (vpp) {
    case 1: params.raster_pick = 0; params.raster_vertices = 1; break;
    case 2: params.raster_pick = 0x0100u; params.raster_vertices = 2; break;
    case 4: params.raster_pick = 0x0201u; params.raster_vertices = 2; break;
    case 6: params.raster_pick = 0x040200u; params.raster_vertices = 3; break;
    default: params.raster_pick = 0x020100u; params.raster_vertices = 3; break;
    }
}

void set_clip_layout(const graphics::StatsClipLayout& layout,
                     graphics::StatsKernelParams& params) {
    params.position_slot = layout.position_slot;
    params.distances = layout.distances;
    for (u32 k = 0; k < layout.distances && k < 8; ++k) {
        params.distance_slots[k] =
            (layout.distance_slot[k] << 2) | layout.distance_component[k];
    }
}

const char* const kCounterNames[graphics::STATS_COUNTERS] = {
    "IAVertices",    "IAPrimitives",  "VSInvocations", "GSInvocations",
    "GSPrimitives",  "CInvocations",  "CPrimitives",   "PSInvocations",
    "HSInvocations", "DSInvocations", "CSInvocations"};

}  // namespace

bool Replayer::pipeline_statistics_open(const ReplayState& st) const {
    for (const auto& open : st.statistics) {
        if (is_pipeline_query(open.result->type)) return true;
    }
    return false;
}

bool Replayer::rasterization_disabled(const ReplayState& st) const {
    const PipelineState* pso = st.pso;
    return pso != nullptr && !pso->ps.present && !pso->graphics.depth_enable &&
           !pso->graphics.stencil_enable;
}

std::shared_ptr<graphics::BackendBuffer> Replayer::statistics_record(
    ReplayState& st) {
    if (!pipeline_statistics_open(st)) return nullptr;
    const bool opened = !st.statistics_interval;
    if (opened) {
        graphics::BackendBufferDesc desc{};
        desc.shader_resource = true;
        desc.unordered_access = true;
        desc.byte_width = graphics::STATS_RECORD_WORDS * sizeof(u32);
        const std::array<u32, graphics::STATS_RECORD_WORDS> zeros{};
        st.statistics_interval = f.m_backend.create_buffer(desc, zeros.data());
        if (st.statistics_interval && !st.statistics_interval->valid()) {
            st.statistics_interval.reset();
        }
        if (!st.statistics_interval) {
            statistics_unmeasured(
                st, kAllCounters,
                "the backend could not allocate a statistics record");
            return nullptr;
        }
        for (auto& open : st.statistics) {
            if (is_pipeline_query(open.result->type)) {
                open.result->records.push_back(st.statistics_interval);
            }
        }
    }
    return st.statistics_interval;
}

std::shared_ptr<graphics::BackendBuffer> Replayer::mesh_statistics(
    ReplayState& st, const graphics::BackendPipeline*& pipeline) {
    if (!pipeline_statistics_open(st) || st.pso == nullptr) return nullptr;
    PipelineState& pso = *st.pso;
    graphics::BackendPipeline* counting = f.mesh_statistics_pipeline(pso);
    graphics::BackendBufferDesc desc{};
    desc.byte_width = graphics::kMeshStatWords * sizeof(u32);
    desc.shader_resource = true;
    desc.unordered_access = true;
    const std::array<u32, graphics::kMeshStatWords> zeros{};
    std::shared_ptr<graphics::BackendBuffer> record =
        counting != nullptr ? f.m_backend.create_buffer(desc, zeros.data()) : nullptr;
    if (!record || !record->valid()) {
        statistics_unmeasured(st,
                              (1u << graphics::STAT_C_INVOCATIONS) |
                                  (1u << graphics::STAT_C_PRIMITIVES) |
                                  (1u << graphics::STAT_PS_INVOCATIONS),
                              "a mesh draw whose counting pipeline could not be built");
        unimplemented("ResolveQueryData",
                      "PIPELINE_STATISTICS ASInvocations / MSInvocations / "
                      "MSPrimitives of that mesh draw");
        return nullptr;
    }
    pipeline = counting;
    for (auto& open : st.statistics) {
        if (is_pipeline_query(open.result->type)) {
            open.result->mesh_records.push_back({record, !rasterization_disabled(st)});
        }
    }
    return record;
}

void Replayer::add_statistic(ReplayState& st, u32 counter, u64 value) {
    if (value == 0 || (st.statistics_skip & (1u << counter)) != 0) return;
    for (auto& open : st.statistics) {
        if (is_pipeline_query(open.result->type)) {
            open.result->cpu[counter] += value;
        }
    }
}

void Replayer::statistics_unmeasured(ReplayState& st, u32 bits,
                                     const char* why) {
    if (bits == 0) return;
    bool any = false;
    for (auto& open : st.statistics) {
        if (is_pipeline_query(open.result->type)) {
            open.result->unmeasured |= bits;
            any = true;
        }
    }
    if (any) {
        unimplemented("ResolveQueryData",
                      (std::string("PIPELINE_STATISTICS: ") + why).c_str());
    }
}

graphics::BackendBuffer* Replayer::so_statistics_record(ReplayState& st) {
    bool any = false;
    for (const auto& open : st.statistics) any |= is_so_query(open.result->type);
    if (!any) return nullptr;  // the stream-output pass uses its scratch
    if (!st.so_interval) {
        graphics::BackendBufferDesc desc{};
        desc.shader_resource = true;
        desc.unordered_access = true;
        desc.byte_width = graphics::SO_STATS_WORDS * sizeof(u32);
        const std::array<u32, graphics::SO_STATS_WORDS> zeros{};
        st.so_interval = f.m_backend.create_buffer(desc, zeros.data());
        if (st.so_interval && !st.so_interval->valid()) st.so_interval.reset();
        if (!st.so_interval) {
            unimplemented("ResolveQueryData",
                          "SO_STATISTICS: the backend could not allocate a "
                          "stream-output statistics record");
            return nullptr;
        }
        for (auto& open : st.statistics) {
            if (is_so_query(open.result->type)) {
                open.result->so_records.push_back(st.so_interval);
            }
        }
    }
    return st.so_interval.get();
}

void Replayer::count_draw(ReplayState& st, bool indexed,
                          const cmd::DrawIndexed& d,
                          const graphics::BackendIndirectArgs* indirect,
                          bool rasterized, StatisticsDraw& plan) {
    plan = StatisticsDraw{};
    if (!pipeline_statistics_open(st) || st.pso == nullptr) return;
    PipelineState& pso = *st.pso;
    const u32 vpp = ia_vertices(st.topology);
    if (vpp == 0) return;  // UNDEFINED: nothing is drawn
    const u32 assembly = ia_assembly(st.topology);
    const u32 count = d.index_count;
    const u64 instances = indirect != nullptr ? 0u : d.instance_count;

    // The CPU's share: a direct non-indexed draw assembles by formula.
    const bool on_cpu = indirect == nullptr && !indexed;
    if (indirect == nullptr) {
        // IAVertices: "not subtracting any caching" (D3D11.3 20.4.7).
        add_statistic(st, graphics::STAT_IA_VERTICES, u64{count} * instances);
    }
    u64 primitives = 0;
    if (on_cpu) {
        primitives = graphics::ia_primitives(assembly, vpp, count);
        u64 shaded = 0;
        if (assembly == 2) {
            std::vector<u32> vertices(count);
            for (u32 i = 0; i < count; ++i) vertices[i] = i;
            const auto keys = graphics::assemble_primitive_vertices(
                vertices, false, 0, 0, vpp,
                graphics::PrimitiveAssembly::StripAdjacency);
            shaded = graphics::stats_vertex_invocations(keys, vpp);
        } else {
            shaded = graphics::stats_vertex_invocations_non_indexed(
                primitives, vpp, assembly == 1);
        }
        add_statistic(st, graphics::STAT_IA_PRIMITIVES, primitives * instances);
        add_statistic(st, graphics::STAT_VS_INVOCATIONS, shaded * instances);
        // "The clipper will execute for each triangle" (culled and
        // degenerate ones too, measured).
        if (rasterized) {
            add_statistic(st, graphics::STAT_C_INVOCATIONS,
                          primitives * instances);
        }
    }
    if (on_cpu && !rasterized) return;
    graphics::frame_profile::note_internal("pipeline-stats:draw");
    const auto record = statistics_record(st);
    if (!record) return;

    // The rasterized draw's capture: its vertex shader's outputs, keyed
    // draw-relative (SV_VertexID's value + instance x stride).
    const bool capturing = rasterized && pso.statistics_clip.valid &&
                           !pso.statistics_capture_registers.empty();
    if (rasterized && !capturing) {
        statistics_unmeasured(st, 1u << graphics::STAT_C_PRIMITIVES,
                              "CPrimitives of a vertex shader whose outputs "
                              "cannot be captured (an early return, or no "
                              "SV_Position)");
    }
    const u32 record_vec4s =
        capturing ? static_cast<u32>(pso.statistics_capture_registers.size())
                  : 0u;
    graphics::BackendBufferDesc storage{};
    storage.shader_resource = true;
    storage.unordered_access = true;
    graphics::StatsKernelParams clip{};
    clip.vertices_per_primitive = vpp;
    set_raster_pick(vpp, clip);
    clip.record_vec4s = record_vec4s;
    clip.record_base = 1;  // the {stride, capacity} header
    if (capturing) set_clip_layout(pso.statistics_clip, clip);
    clip.flags = graphics::STATS_FLAG_CLIP |
                 (pso.graphics.depth_clip ? graphics::STATS_FLAG_DEPTH_CLIP : 0u);

    if (on_cpu) {
        if (!capturing) return;
        const u64 stride = count;
        const u64 capacity = stride * instances;
        const u64 capture_bytes = graphics::CAPTURE_HEADER_BYTES +
                                  capacity * record_vec4s * 16u;
        if (capture_bytes > kMaxStatisticsBytes ||
            primitives * instances > 0xFFFFFFFFull) {
            statistics_unmeasured(st, 1u << graphics::STAT_C_PRIMITIVES,
                                  "CPrimitives of a draw whose captured "
                                  "vertices exceed the size limit");
            return;
        }
        std::vector<u32> header(graphics::CAPTURE_HEADER_BYTES / 4u, 0u);
        header[0] = static_cast<u32>(stride);
        header[1] = static_cast<u32>(capacity);
        storage.byte_width = static_cast<usize>(capture_bytes);
        plan.capture = f.m_backend.create_buffer(storage, nullptr);
        if (plan.capture &&
            !plan.capture->update(0, header.data(), header.size() * 4u)) {
            plan.capture.reset();
        }
        if (!plan.capture) {
            statistics_unmeasured(st, 1u << graphics::STAT_C_PRIMITIVES,
                                  "CPrimitives: the capture buffer could not "
                                  "be allocated");
            return;
        }
        clip.key_mode = assembly == 0 ? 2u : assembly == 1 ? 3u : 4u;
        clip.primitives_per_instance = static_cast<u32>(primitives);
        clip.instances = static_cast<u32>(instances);
        clip.key_stride = static_cast<u32>(stride);
        clip.capacity = static_cast<u32>(primitives * instances);
        if (clip.capacity == 0) return;  // no complete primitive
        plan.record = record;
        plan.clip = clip;
        plan.active = true;
        return;
    }

    // The GPU assembles the draw (indexed: the index values are GPU data;
    // indirect: so are the counts): vitreo_ia_resolve, then
    // vitreo_stats_vertices. The capture's capacity is placed in its header
    // after the resolve, and the clipper flags a key past it unmeasured.
    for (usize i = 0; i < f.m_stats_feedback.size();) {
        const auto& entry = f.m_stats_feedback[i];
        if (entry.generation != 0 &&
            !f.m_backend.generation_complete(entry.generation)) {
            ++i;
            continue;
        }
        std::array<u32, graphics::IA_RESOLVED_WORDS> words{};
        if (entry.resolved &&
            entry.resolved->read(0, words.data(), sizeof(words))) {
            const u64 keys = u64{words[graphics::IA_RESOLVED_KEY_STRIDE]} *
                             words[graphics::IA_RESOLVED_INSTANCES];
            while (f.m_stats_record_capacity < keys &&
                   f.m_stats_record_capacity < (1u << 24)) {
                f.m_stats_record_capacity *= 4u;
            }
            if (words[graphics::IA_RESOLVED_OVERFLOW] != 0 &&
                f.m_stats_primitive_capacity < (1u << 22)) {
                f.m_stats_primitive_capacity *= 4u;
            }
        }
        f.m_stats_feedback.erase(f.m_stats_feedback.begin() +
                                 static_cast<std::ptrdiff_t>(i));
    }
    const bool index32 = st.ib.format == DXGI_FORMAT_R32_UINT;
    const u64 direct_primitives =
        indirect != nullptr ? 0u
                            : u64{graphics::ia_primitives(assembly, vpp, count)} *
                                  instances;
    // A 16-bit index names at most 65536 vertices per instance.
    const u64 wanted_records =
        indexed && !index32 && indirect == nullptr
            ? 65536u * std::max<u64>(instances, 1u)
            : f.m_stats_record_capacity;
    const u32 record_capacity =
        !capturing ? 0u
                   : static_cast<u32>(std::min<u64>(
                         wanted_records,
                         kMaxStatisticsBytes /
                             std::max<u64>(u64{record_vec4s} * 16u, 16u)));
    const u64 primitive_capacity = std::min<u64>(
        indirect != nullptr ? u64{f.m_stats_primitive_capacity}
                            : direct_primitives,
        kMaxStatisticsBytes / (4u * vpp));
    if (primitive_capacity == 0) return;
    graphics::BackendComputePipeline* resolve_kernel = f.ia_resolve_kernel();
    graphics::BackendComputePipeline* vertices_kernel =
        f.statistics_kernel(graphics::kStatsVerticesEntryPoint);
    storage.byte_width = graphics::IA_RESOLVED_WORDS * sizeof(u32);
    plan.resolved = f.m_backend.create_buffer(storage, nullptr);
    storage.byte_width = static_cast<usize>(primitive_capacity * vpp * 4u);
    plan.ids = f.m_backend.create_buffer(storage, nullptr);
    storage.byte_width =
        graphics::CAPTURE_HEADER_BYTES +
        (capturing ? u64{record_capacity} * record_vec4s * 16u : 0u);
    plan.capture = f.m_backend.create_buffer(storage, nullptr);
    std::shared_ptr<graphics::BackendBuffer> host_arguments;
    if (indirect == nullptr) {
        // D3D12_DRAW_[INDEXED_]ARGUMENTS of the direct draw.
        const std::array<u32, 5> words{
            count, d.instance_count, d.start_index,
            indexed ? static_cast<u32>(d.base_vertex) : d.start_instance,
            indexed ? d.start_instance : 0u};
        storage.byte_width = sizeof(words);
        host_arguments = f.m_backend.create_buffer(storage, words.data());
    }
    graphics::BackendBuffer* index_buffer = nullptr;
    u64 index_offset = 0;
    if (indexed && st.ib.va != 0) {
        (void)f.resolve_va(st.ib.va, 0, index_buffer, index_offset);
    }
    if (resolve_kernel == nullptr || vertices_kernel == nullptr ||
        !plan.resolved || !plan.ids || !plan.capture ||
        (indirect == nullptr && !host_arguments) ||
        (indexed && index_buffer == nullptr)) {
        plan = StatisticsDraw{};
        statistics_unmeasured(st, kIaCounters,
                              "the input-assembler statistics of a "
                              "GPU-assembled draw could not be recorded");
        return;
    }
    graphics::IaResolveParams params{};
    params.indexed = indexed ? 1u : 0u;
    params.vertices_per_primitive = vpp;
    params.assembly = assembly;
    params.record_capacity = 0xFFFFFFFFu;
    params.primitive_capacity = static_cast<u32>(primitive_capacity);
    u64 index_binding = 0;
    if (indexed) {
        const u32 index_size = index32 ? 4u : 2u;
        // The kernel reads 32-bit words: bind at the word holding the view's
        // first index, and start that many indices into it.
        index_binding = index_offset & ~u64{3};
        params.index_32bit = index32 ? 1u : 0u;
        params.index_first =
            static_cast<u32>((index_offset - index_binding) / index_size);
        params.index_elements = st.ib.size / index_size;
        params.cut_mode = graphics::ia_cut_mode(strip_cut_of(st.pso->graphics));
    }
    const graphics::BackendBuffer* args =
        indirect != nullptr ? indirect->buffer : host_arguments.get();
    const u64 args_offset = indirect != nullptr ? indirect->offset : 0u;
    graphics::BackendDispatchCall resolve{};
    resolve.pipeline = resolve_kernel;
    resolve.group_count_x = resolve.group_count_y = resolve.group_count_z = 1;
    resolve.threads_per_group_x = graphics::IA_RESOLVE_THREADS;
    resolve.constants = &params;
    resolve.constants_size = sizeof(params);
    resolve.buffers[graphics::IA_RESOLVE_ARGS_INDEX] = args;
    resolve.buffer_offsets[graphics::IA_RESOLVE_ARGS_INDEX] = args_offset;
    resolve.buffers[graphics::IA_RESOLVE_INDICES_INDEX] =
        indexed ? index_buffer : plan.resolved.get();
    resolve.buffer_offsets[graphics::IA_RESOLVE_INDICES_INDEX] = index_binding;
    resolve.buffers[graphics::IA_RESOLVE_IDS_INDEX] = plan.ids.get();
    resolve.buffers[graphics::IA_RESOLVE_OUT_INDEX] = plan.resolved.get();
    resolve.buffers[graphics::IA_RESOLVE_CAPTURE_INDEX] = plan.capture.get();
    graphics::StatsKernelParams vertices{};
    vertices.vertices_per_primitive = vpp;
    vertices.batch = graphics::stats_vertex_batch(vpp);
    const bool ia_counted =
        (st.statistics_skip & (1u << graphics::STAT_IA_PRIMITIVES)) == 0;
    vertices.flags =
        (ia_counted ? graphics::STATS_FLAG_ADD_IA_PRIMITIVES : 0u) |
        graphics::STATS_FLAG_ADD_VS_INVOCATIONS |
        (indirect != nullptr && ia_counted ? graphics::STATS_FLAG_ADD_IA_VERTICES
                                           : 0u) |
        (rasterized ? graphics::STATS_FLAG_ADD_C_INVOCATIONS : 0u) |
        // "HSInvocations increments once per patch": the resolved
        // primitive count of a patch list (every instance).
        (pso.tessellation.active ? graphics::STATS_FLAG_ADD_HS_INVOCATIONS
                                 : 0u);
    graphics::BackendDispatchCall count_call{};
    count_call.pipeline = vertices_kernel;
    count_call.threads_per_group_x = graphics::STATS_BATCH_THREADS;
    count_call.group_count_x = static_cast<u32>(
        (primitive_capacity + vertices.batch - 1) / vertices.batch);
    count_call.group_count_y = count_call.group_count_z = 1;
    count_call.constants = &vertices;
    count_call.constants_size = sizeof(vertices);
    count_call.buffers[graphics::STATS_RECORD_INDEX] = record.get();
    count_call.buffers[graphics::STATS_COUNTS_INDEX] = plan.resolved.get();
    count_call.buffers[graphics::STATS_IDS_INDEX] = plan.ids.get();
    count_call.buffers[graphics::STATS_ARGS_INDEX] = args;
    count_call.buffer_offsets[graphics::STATS_ARGS_INDEX] = args_offset;
    std::shared_ptr<graphics::BackendBuffer> capacity_word;
    if (capturing) {
        storage.byte_width = sizeof(u32);
        capacity_word = f.m_backend.create_buffer(storage, &record_capacity);
    }
    if (!f.m_backend.dispatch(resolve) || !f.m_backend.dispatch(count_call) ||
        (capturing &&
         (!capacity_word ||
          !f.m_backend.copy_buffer_region(plan.capture.get(), sizeof(u32),
                                          capacity_word.get(), 0,
                                          sizeof(u32))))) {
        plan = StatisticsDraw{};
        statistics_unmeasured(st, kIaCounters,
                              "the input-assembler statistics dispatch failed");
        return;
    }
    graphics::frame_profile::note_internal("pipeline-stats:assemble");
    if (indirect != nullptr || (indexed && index32)) {
        f.m_stats_feedback.push_back(
            {plan.resolved, f.m_backend.recording_generation()});
    }
    if (!capturing) {
        plan = StatisticsDraw{};
        return;
    }
    clip.key_mode = 0;
    clip.flags |= graphics::STATS_FLAG_RESOLVED_COUNTS;
    clip.capacity = static_cast<u32>(primitive_capacity);
    plan.record = record;
    plan.clip = clip;
    plan.active = true;
}

void Replayer::finish_statistics_draw(StatisticsDraw& plan) {
    if (!plan.active) return;
    plan.active = false;
    graphics::BackendComputePipeline* kernel =
        f.statistics_kernel(graphics::kStatsPrimitivesEntryPoint);
    if (kernel == nullptr) return;
    graphics::BackendDispatchCall call{};
    call.pipeline = kernel;
    call.threads_per_group_x = graphics::STATS_THREADS;
    call.group_count_x = (plan.clip.capacity + graphics::STATS_THREADS - 1) /
                         graphics::STATS_THREADS;
    call.group_count_y = call.group_count_z = 1;
    call.constants = &plan.clip;
    call.constants_size = sizeof(plan.clip);
    call.buffers[graphics::STATS_RECORD_INDEX] = plan.record.get();
    call.buffers[graphics::STATS_COUNTS_INDEX] =
        plan.resolved ? plan.resolved.get() : plan.record.get();
    call.buffers[graphics::STATS_IDS_INDEX] =
        plan.ids ? plan.ids.get() : plan.record.get();
    call.buffers[graphics::STATS_RECORDS_INDEX] = plan.capture.get();
    if (!f.m_backend.dispatch(call)) {
        d3d12_diag("PIPELINE_STATISTICS: the clipper dispatch failed");
    }
}

void Replayer::count_lowered_input(ReplayState& st, const std::vector<u32>& ids,
                                   u32 group, u32 vertex_count, u32 instances,
                                   bool tessellated) {
    if (!pipeline_statistics_open(st) || group == 0) return;
    const u64 primitives = ids.size() / group;
    add_statistic(st, graphics::STAT_IA_VERTICES,
                  u64{vertex_count} * instances);
    add_statistic(st, graphics::STAT_IA_PRIMITIVES, primitives * instances);
    add_statistic(st, graphics::STAT_VS_INVOCATIONS,
                  graphics::stats_vertex_invocations(ids, group) * instances);
    // "HSInvocations increments once per patch".
    if (tessellated) {
        add_statistic(st, graphics::STAT_HS_INVOCATIONS, primitives * instances);
    }
}

void Replayer::count_geometry(ReplayState& st, u32 invocations,
                              graphics::BackendBuffer* counter,
                              graphics::BackendBuffer* expanded,
                              bool rasterized) {
    if (!pipeline_statistics_open(st) || invocations == 0 || st.pso == nullptr) {
        return;
    }
    const auto record = statistics_record(st);
    if (!record) return;
    const PipelineState& pso = *st.pso;
    const auto& gs = pso.geometry;
    constexpr u32 kGeometryCounters = (1u << graphics::STAT_GS_INVOCATIONS) |
                                      (1u << graphics::STAT_GS_PRIMITIVES) |
                                      (1u << graphics::STAT_C_INVOCATIONS) |
                                      (1u << graphics::STAT_C_PRIMITIVES);
    graphics::BackendComputePipeline* kernel =
        f.statistics_kernel(graphics::kStatsGeometryEntryPoint);
    if (kernel == nullptr) {
        statistics_unmeasured(st, kGeometryCounters,
                              "the geometry statistics kernel is unavailable");
        return;
    }
    graphics::StatsKernelParams params{};
    params.vertices_per_primitive = gs.output_topology == 1u   ? 1u
                                    : gs.output_topology == 3u ? 2u
                                                               : 3u;
    params.invocations = invocations;
    params.gs_instances = gs.instance_count;
    params.vertices_per_invocation = gs.vertices_per_invocation;
    params.stream_mask = gs.stream_mask;
    const u32 raster_stream =
        pso.stream_output.active ? pso.stream_output.rasterized_stream : 0u;
    params.raster_stream = raster_stream < 4u ? raster_stream : 4u;
    params.record_vec4s = gs.output_record_vec4s;
    if (rasterized && params.raster_stream < 4u) {
        const auto& layout = pso.lowered_clip[params.raster_stream];
        if (layout.valid) {
            set_clip_layout(layout, params);
            params.flags =
                graphics::STATS_FLAG_CLIP |
                (pso.graphics.depth_clip ? graphics::STATS_FLAG_DEPTH_CLIP : 0u);
        } else {
            statistics_unmeasured(st,
                                  (1u << graphics::STAT_C_INVOCATIONS) |
                                      (1u << graphics::STAT_C_PRIMITIVES),
                                  "CPrimitives of a geometry shader stream "
                                  "without SV_Position");
        }
    }
    params.capacity = invocations;
    graphics::BackendDispatchCall call{};
    call.pipeline = kernel;
    call.threads_per_group_x = graphics::STATS_THREADS;
    call.group_count_x =
        (invocations + graphics::STATS_THREADS - 1) / graphics::STATS_THREADS;
    call.group_count_y = call.group_count_z = 1;
    call.constants = &params;
    call.constants_size = sizeof(params);
    call.buffers[graphics::STATS_RECORD_INDEX] = record.get();
    call.buffers[graphics::STATS_COUNTS_INDEX] = counter;
    call.buffers[graphics::STATS_RECORDS_INDEX] = expanded;
    if (!f.m_backend.dispatch(call)) {
        statistics_unmeasured(st, kGeometryCounters,
                              "the geometry statistics dispatch failed");
    }
}

void Replayer::count_raster_clipper(ReplayState& st, u32 primitives,
                                    graphics::BackendBuffer* ids,
                                    graphics::BackendBuffer* records,
                                    u32 records_offset, u32 record_vec4s) {
    if (!pipeline_statistics_open(st) || st.pso == nullptr || primitives == 0) {
        return;
    }
    constexpr u32 kClipCounters = (1u << graphics::STAT_C_INVOCATIONS) |
                                  (1u << graphics::STAT_C_PRIMITIVES);
    const PipelineState& pso = *st.pso;
    if (!pso.raster.clip.valid) {
        // "The clipper will execute for each triangle"; what it makes needs
        // the position.
        add_statistic(st, graphics::STAT_C_INVOCATIONS, primitives);
        statistics_unmeasured(st, 1u << graphics::STAT_C_PRIMITIVES,
                              "CPrimitives of a raster-emulated vertex shader "
                              "without SV_Position");
        return;
    }
    const auto record = statistics_record(st);
    if (!record) return;
    graphics::BackendComputePipeline* kernel =
        f.statistics_kernel(graphics::kStatsTessellationEntryPoint);
    // The tessellation kernel over a block that names `primitives`
    // triangles and no domain-shader invocations.
    std::array<u32, graphics::STATS_TESS_BLOCK_OVERFLOW + 1u> block{};
    block[graphics::STATS_TESS_BLOCK_PRIMITIVES] = primitives;
    graphics::BackendBuffer* block_buffer = nullptr;
    u64 block_offset = 0;
    if (kernel == nullptr ||
        !upload(block.data(), sizeof(block), block_buffer, block_offset) ||
        (records_offset % 16u) != 0u) {
        statistics_unmeasured(st, kClipCounters,
                              "the clipper counts of a raster-emulated draw "
                              "could not be recorded");
        return;
    }
    graphics::StatsKernelParams params{};
    params.vertices_per_primitive = 3;
    params.record_vec4s = record_vec4s;
    params.record_base = records_offset / 16u;
    set_clip_layout(pso.raster.clip, params);
    params.flags = graphics::STATS_FLAG_CLIP |
                   (pso.graphics.depth_clip ? graphics::STATS_FLAG_DEPTH_CLIP : 0u);
    params.capacity = primitives;
    graphics::BackendDispatchCall call{};
    call.pipeline = kernel;
    call.threads_per_group_x = graphics::STATS_THREADS;
    call.group_count_x =
        (primitives + graphics::STATS_THREADS - 1) / graphics::STATS_THREADS;
    call.group_count_y = call.group_count_z = 1;
    call.constants = &params;
    call.constants_size = sizeof(params);
    call.buffers[graphics::STATS_RECORD_INDEX] = record.get();
    call.buffers[graphics::STATS_COUNTS_INDEX] = block_buffer;
    call.buffer_offsets[graphics::STATS_COUNTS_INDEX] = block_offset;
    call.buffers[graphics::STATS_IDS_INDEX] = ids;
    call.buffers[graphics::STATS_RECORDS_INDEX] = records;
    if (!f.m_backend.dispatch(call)) {
        statistics_unmeasured(st, kClipCounters,
                              "the clipper counts of a raster-emulated draw "
                              "could not be recorded");
    }
}

void Replayer::count_clip(ReplayState& st, graphics::BackendBuffer* records,
                          u64 records_offset, graphics::BackendBuffer* ids,
                          u64 primitives) {
    if (!pipeline_statistics_open(st) || st.pso == nullptr || primitives == 0 ||
        rasterization_disabled(st)) {
        return;
    }
    constexpr u32 kClipCounters = (1u << graphics::STAT_C_INVOCATIONS) |
                                  (1u << graphics::STAT_C_PRIMITIVES);
    const PipelineState& pso = *st.pso;
    const auto record = statistics_record(st);
    graphics::BackendComputePipeline* kernel =
        f.statistics_kernel(graphics::kStatsTessellationEntryPoint);
    const auto& layout = pso.lowered_clip[0];
    if (!record || kernel == nullptr || !layout.valid ||
        primitives > 0xFFFFFFFFull) {
        statistics_unmeasured(st, kClipCounters,
                              "the clipper counts of a captured vertex-shader "
                              "draw");
        return;
    }
    // The tessellation kernel's batch block, with the primitive count and
    // no overflow; the ids are the assembled primitives' record ids.
    std::array<u32, 16> block{};
    block[graphics::STATS_TESS_BLOCK_PRIMITIVES] = static_cast<u32>(primitives);
    graphics::BackendBufferDesc desc{};
    desc.byte_width = sizeof(block);
    desc.shader_resource = true;
    desc.unordered_access = true;
    std::shared_ptr<graphics::BackendBuffer> words =
        f.m_backend.create_buffer(desc, block.data());
    if (!words || !words->valid()) {
        statistics_unmeasured(st, kClipCounters,
                              "the clipper counts of a captured vertex-shader "
                              "draw");
        return;
    }
    graphics::StatsKernelParams params{};
    params.vertices_per_primitive = std::max(pso.per_vertex_primitive, 1u);
    params.record_vec4s =
        static_cast<u32>(std::max<usize>(pso.capture_registers.size(), 1));
    set_clip_layout(layout, params);
    params.flags = graphics::STATS_FLAG_CLIP |
                   (pso.graphics.depth_clip ? graphics::STATS_FLAG_DEPTH_CLIP : 0u);
    params.capacity = static_cast<u32>(primitives);
    graphics::BackendDispatchCall call{};
    call.pipeline = kernel;
    call.threads_per_group_x = graphics::STATS_THREADS;
    call.group_count_x =
        (params.capacity + graphics::STATS_THREADS - 1) / graphics::STATS_THREADS;
    call.group_count_y = call.group_count_z = 1;
    call.constants = &params;
    call.constants_size = sizeof(params);
    call.buffers[graphics::STATS_RECORD_INDEX] = record.get();
    call.buffers[graphics::STATS_COUNTS_INDEX] = words.get();
    call.buffers[graphics::STATS_IDS_INDEX] = ids;
    call.buffers[graphics::STATS_RECORDS_INDEX] = records;
    call.buffer_offsets[graphics::STATS_RECORDS_INDEX] = records_offset;
    if (!f.m_backend.dispatch(call)) {
        statistics_unmeasured(st, kClipCounters,
                              "the clipper dispatch of a captured vertex-shader "
                              "draw failed");
        return;
    }
    f.m_lowered_feedback.push_back({words, f.m_backend.recording_generation()});
}

void Replayer::count_tessellation(ReplayState& st, u32 vpp, u32 index_capacity,
                                  graphics::BackendBuffer* block,
                                  graphics::BackendBuffer* indices,
                                  graphics::BackendBuffer* records,
                                  u32 record_vec4s, bool rasterized) {
    if (!pipeline_statistics_open(st) || st.pso == nullptr) return;
    const auto record = statistics_record(st);
    if (!record) return;
    const PipelineState& pso = *st.pso;
    constexpr u32 kTessCounters = (1u << graphics::STAT_DS_INVOCATIONS) |
                                  (1u << graphics::STAT_C_INVOCATIONS) |
                                  (1u << graphics::STAT_C_PRIMITIVES);
    graphics::BackendComputePipeline* kernel =
        f.statistics_kernel(graphics::kStatsTessellationEntryPoint);
    if (kernel == nullptr) {
        statistics_unmeasured(st, kTessCounters,
                              "the tessellation statistics kernel is "
                              "unavailable");
        return;
    }
    graphics::StatsKernelParams params{};
    params.vertices_per_primitive = vpp;
    params.flags = graphics::STATS_FLAG_ADD_DS_INVOCATIONS;
    params.record_vec4s = record_vec4s;
    if (rasterized) {
        const auto& layout = pso.lowered_clip[0];
        if (layout.valid) {
            set_clip_layout(layout, params);
            params.flags |=
                graphics::STATS_FLAG_CLIP |
                (pso.graphics.depth_clip ? graphics::STATS_FLAG_DEPTH_CLIP : 0u);
        } else {
            statistics_unmeasured(st,
                                  (1u << graphics::STAT_C_INVOCATIONS) |
                                      (1u << graphics::STAT_C_PRIMITIVES),
                                  "CPrimitives of a domain shader without "
                                  "SV_Position");
        }
    }
    params.capacity = std::max(index_capacity / std::max(vpp, 1u), 1u);
    graphics::BackendDispatchCall call{};
    call.pipeline = kernel;
    call.threads_per_group_x = graphics::STATS_THREADS;
    call.group_count_x =
        (params.capacity + graphics::STATS_THREADS - 1) / graphics::STATS_THREADS;
    call.group_count_y = call.group_count_z = 1;
    call.constants = &params;
    call.constants_size = sizeof(params);
    call.buffers[graphics::STATS_RECORD_INDEX] = record.get();
    call.buffers[graphics::STATS_COUNTS_INDEX] = block;
    call.buffers[graphics::STATS_IDS_INDEX] = indices;
    call.buffers[graphics::STATS_RECORDS_INDEX] = records;
    if (!f.m_backend.dispatch(call)) {
        statistics_unmeasured(st, kTessCounters,
                              "the tessellation statistics dispatch failed");
    }
}

void Replayer::count_dispatch(ReplayState& st, const cmd::Dispatch& d,
                              const graphics::BackendIndirectArgs* indirect) {
    if (!pipeline_statistics_open(st) || st.pso == nullptr) return;
    const u64 threads = u64{st.pso->cs.threads_x} * st.pso->cs.threads_y *
                        st.pso->cs.threads_z;
    if (indirect == nullptr) {
        // CSInvocations: "Number of individual Compute Shader threads
        // invoked" (D3D11.3 20.4.7).
        add_statistic(st, graphics::STAT_CS_INVOCATIONS,
                      u64{d.x} * d.y * d.z * threads);
        return;
    }
    // D3D12_DISPATCH_ARGUMENTS, copied in order and folded in at the resolve.
    graphics::BackendBufferDesc desc{};
    desc.byte_width = 12;
    desc.shader_resource = true;
    std::shared_ptr<graphics::BackendBuffer> snapshot =
        f.m_backend.create_buffer(desc, nullptr);
    if (!snapshot || !snapshot->valid() ||
        !f.m_backend.copy_buffer_region(snapshot.get(), 0, indirect->buffer,
                                        indirect->offset, 12)) {
        statistics_unmeasured(st, 1u << graphics::STAT_CS_INVOCATIONS,
                              "CSInvocations of an indirect dispatch whose "
                              "arguments could not be copied");
        return;
    }
    for (auto& open : st.statistics) {
        if (is_pipeline_query(open.result->type)) {
            open.result->dispatch_arguments.push_back({snapshot, threads});
        }
    }
}

/// The values ResolveQueryData writes for one statistics query, once the GPU
/// work it counted has completed: @p out holds 14 counters for
/// PIPELINE_STATISTICS[1], or {NumPrimitivesWritten,
/// PrimitivesStorageNeeded} for SO_STATISTICS_STREAMn.
void statistics_values(const StatisticsResult& result, std::array<u64, 14>& out) {
    out = result.cpu;
    u32 unmeasured = result.unmeasured;
    if (is_so_query(result.type)) {
        const u32 stream = result.type - D3D12_QUERY_TYPE_SO_STATISTICS_STREAM0;
        u64 needed = 0;
        u64 written = 0;
        bool read = true;
        for (const auto& record : result.so_records) {
            std::array<u32, graphics::SO_STATS_WORDS> words{};
            if (!record || !record->read(0, words.data(), sizeof(words))) {
                read = false;
                continue;
            }
            needed += words[stream * 2u];
            written += words[stream * 2u + 1u];
        }
        if (!read) {
            unimplemented("ResolveQueryData",
                          "SO_STATISTICS: the stream-output counts could not "
                          "be read back");
        }
        out.fill(0);
        out[0] = written;
        out[1] = needed;
        return;
    }
    for (const auto& pending : result.dispatch_arguments) {
        std::array<u32, 3> words{};
        if (!pending.arguments ||
            !pending.arguments->read(0, words.data(), sizeof(words))) {
            unmeasured |= 1u << graphics::STAT_CS_INVOCATIONS;
            continue;
        }
        out[graphics::STAT_CS_INVOCATIONS] += u64{words[0]} * words[1] *
                                              words[2] *
                                              pending.threads_per_group;
    }
    for (const auto& record : result.records) {
        std::array<u32, graphics::STATS_RECORD_WORDS> words{};
        if (!record || !record->read(0, words.data(), sizeof(words))) {
            unmeasured |= kAllCounters;
            continue;
        }
        for (u32 c = 0; c < graphics::STATS_COUNTERS; ++c) {
            out[c] += graphics::stats_record_value(words.data(), c);
        }
        unmeasured |= words[graphics::STATS_FLAGS_WORD] & kAllCounters;
    }
    if (unmeasured != 0) {
        std::string names;
        for (u32 c = 0; c < graphics::STATS_COUNTERS; ++c) {
            if ((unmeasured & (1u << c)) == 0) continue;
            if (!names.empty()) names += ", ";
            names += kCounterNames[c];
        }
        unimplemented("ResolveQueryData",
                      ("PIPELINE_STATISTICS " + names +
                       ": a draw in the bracket could not be counted (see the "
                       "line naming it); the value is what was counted")
                          .c_str());
    }
    // ASInvocations, MSInvocations, MSPrimitives (and the clipper's input)
    // from the mesh draws' records (graphics/shaders/mesh_stage.hpp).
    for (const auto& [record, clipper] : result.mesh_records) {
        std::array<u32, graphics::kMeshStatWords> words{};
        if (!record || !record->read(0, words.data(), sizeof(words))) {
            unimplemented("ResolveQueryData",
                          "PIPELINE_STATISTICS: a mesh draw's counts could not be "
                          "read back");
            continue;
        }
        const auto value = [&](u32 c) {
            return u64{words[c * 2u]} | (u64{words[c * 2u + 1u]} << 32);
        };
        out[11] += value(graphics::kMeshStatAs);
        out[12] += value(graphics::kMeshStatMs);
        out[13] += value(graphics::kMeshStatPrimitives);
        if (clipper) {
            out[graphics::STAT_C_INVOCATIONS] += value(graphics::kMeshStatClipper);
            out[graphics::STAT_C_PRIMITIVES] += value(graphics::kMeshStatClipped);
        }
    }
}

}  // namespace vitreo::winapi::d3d12
