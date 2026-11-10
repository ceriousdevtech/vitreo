// SPDX-License-Identifier: LGPL-2.1-only
//
// ViewInstancingTier 3 for mesh pipelines (DirectX-Specs "View Instancing":
// "The implementation of view instancing always occurs at the first shader
// stage that references SV_ViewID or the rasterizer, whichever is
// earlier"; "Mesh Shader": "Amplification shaders do not support SV_ViewID
// input"). The amplification shader runs once: a capture pass records each
// group's DispatchMesh grid and payload, and every view replays them in an
// object function of its own (mesh_stage.hpp mesh_vi_object_replay_msl).
// A mesh shader that does not read SV_ViewID runs once too: the first view
// draws it while it records each group's outputs, the other views draw the
// records (mesh_vi_mesh_replay_msl). Pipeline statistics follow: "For the
// stages that come before X, view instances do not increase those stages'
// pipeline statistics"; MSPrimitives: "If SV_ViewID is referenced from a
// later stage like PS, then view instances don't increment MSPrimitives".

#include "winapi/graphics/d3d12/d3d12_replay_internal.hpp"

#include "graphics/shaders/mesh_stage.hpp"
#include "graphics/shaders/pipeline_stats.hpp"

#include <algorithm>
#include <cstring>

namespace vitreo::winapi::d3d12 {

namespace {

[[nodiscard]] bool is_pipeline_query(u32 type) {
    return type == D3D12_QUERY_TYPE_PIPELINE_STATISTICS ||
           type == D3D12_QUERY_TYPE_PIPELINE_STATISTICS1;
}

}  // namespace

void Replayer::dispatch_mesh_views(ReplayState& st, const cmd::DispatchMesh& d,
                                   const graphics::BackendIndirectArgs* indirect) {
    PipelineState* const pso = st.pso;
    if (!st.roots[0]) {
        d3d12_diag("DispatchMesh without a graphics root signature: skipped");
        return;
    }
    // The group grid on the CPU: the records are sized by it.
    std::array<u32, 3> grid{d.x, d.y, d.z};
    if (indirect != nullptr) {
        graphics::BackendBuffer* staging = nullptr;
        u64 staging_offset = 0;
        u8* host = nullptr;
        bool read = indirect->buffer != nullptr && indirect->buffer->valid() &&
                    indirect->offset <= indirect->buffer->desc().byte_width &&
                    indirect->buffer->desc().byte_width - indirect->offset >= 12u &&
                    reserve(12u, staging, staging_offset, host) &&
                    f.m_backend.copy_buffer_region(staging, staging_offset, indirect->buffer,
                                                   indirect->offset, 12u);
        if (read) {
            g_replay_cpu_waits.fetch_add(1, std::memory_order_relaxed);
            graphics::frame_profile::WaitScope profile_wait("replay:view-mesh-readback");
            if (auto completion = f.m_backend.flush_completion()) {
                read = completion->wait();
            } else {
                read = f.m_backend.wait_idle();
            }
        }
        if (!read || host == nullptr) {
            unimplemented("ExecuteCommandLists",
                          "a view-instanced indirect DispatchMesh whose arguments cannot be read");
            return;
        }
        std::memcpy(grid.data(), host, 12u);
    }
    if (grid[0] == 0 || grid[1] == 0 || grid[2] == 0) return;
    const u64 groups = u64{grid[0]} * grid[1] * grid[2];
    constexpr u64 kMaxBytes = 256ull << 20;
    const bool statistics = pipeline_statistics_open(st);
    const bool amplified = pso->mesh.as.present;
    const bool mesh_once = pso->vi_first == kViMeshRaster;
    const u32 payload_words = pso->mesh.payload_words;
    std::vector<std::shared_ptr<graphics::BackendBuffer>> kept;
    const auto make = [&](u64 bytes, const void* data) -> graphics::BackendBuffer* {
        graphics::BackendBufferDesc desc{};
        desc.byte_width = static_cast<usize>(std::max<u64>(bytes, 16u));
        desc.shader_resource = true;
        desc.unordered_access = true;
        std::shared_ptr<graphics::BackendBuffer> created = f.m_backend.create_buffer(desc, data);
        if (!created || !created->valid()) return nullptr;
        kept.push_back(created);
        return created.get();
    };
    const auto keep_alive = [&]() {
        for (auto& buffer : kept) {
            f.m_lowered_feedback.push_back({buffer, f.m_backend.recording_generation()});
        }
    };
    const auto too_large = [&]() {
        keep_alive();
        unimplemented("ExecuteCommandLists",
                      "view instancing (tier 3) records of a mesh dispatch past 256 MB");
    };

    // One dispatch of @p v's @p pipeline: its bindings, @p g groups, the
    // capture records, and the statistics (a mesh record when @p mesh_counts,
    // the pixel shader's always).
    const auto submit = [&](PipelineState* v, graphics::BackendPipeline* pipeline,
                            const std::array<u32, 3>& g, graphics::BackendBuffer* object_records,
                            graphics::BackendBuffer* mesh_records, bool mesh_counts) -> bool {
        if (pipeline == nullptr) return false;
        graphics::BackendDrawCall call{};
        // The out-of-line mesh dispatch (BackendDrawCall::mesh): this
        // submission's own, read during the draw only.
        graphics::BackendMeshDraw mesh{};
        call.mesh = &mesh;
        call.pipeline = pipeline;
        std::shared_ptr<graphics::BackendBuffer> record;
        if (statistics && mesh_counts) {
            graphics::BackendBufferDesc desc{};
            desc.byte_width = graphics::kMeshStatWords * sizeof(u32);
            desc.shader_resource = true;
            desc.unordered_access = true;
            const std::array<u32, graphics::kMeshStatWords> zeros{};
            record = f.m_backend.create_buffer(desc, zeros.data());
            if (!record || !record->valid()) return false;
            for (auto& open : st.statistics) {
                if (is_pipeline_query(open.result->type)) {
                    open.result->mesh_records.push_back({record, !rasterization_disabled(st)});
                }
            }
            mesh.statistics = record.get();
        }
        std::shared_ptr<graphics::BackendBuffer> pixels;
        if (statistics && v->ps.present) {
            pixels = statistics_record(st);
            call.fragment_statistics_buffer = pixels.get();
        }
        const graphics::DxbcMeshState& ms = v->mesh.ms_layout.state;
        const graphics::DxbcMeshState& as = v->mesh.as_layout.state;
        for (u32 k = 0; k < 3; ++k) {
            mesh.mesh_threads[k] = ms.threads[k];
            if (v->mesh.as.present) mesh.object_threads[k] = as.threads[k];
        }
        mesh.grid = {g[0], g[1], g[2]};
        mesh.object_capture = object_records;
        mesh.mesh_capture = mesh_records;
        std::vector<u32> ms_push;
        std::vector<u32> as_push;
        std::vector<u32> ps_push;
        const bool bound =
            bind_stage(st, STAGE_MESH, v->mesh.ms, call.vertex_descriptor_tables, ms_push) &&
            (!v->mesh.as.present ||
             bind_stage(st, STAGE_AMPLIFICATION, v->mesh.as, mesh.object_descriptor_tables,
                        as_push)) &&
            (!v->ps.present ||
             bind_stage(st, STAGE_PIXEL, v->ps, call.fragment_descriptor_tables, ps_push));
        if (!bound) {
            d3d12_diag("DispatchMesh: bindings could not be resolved: skipped");
            return false;
        }
        call.vertex_constants = ms_push.data();
        call.vertex_constants_size = ms_push.size() * 4;
        mesh.object_constants = as_push.data();
        mesh.object_constants_size = as_push.size() * 4;
        call.fragment_constants = ps_push.data();
        call.fragment_constants_size = ps_push.size() * 4;
        if (record) kept.push_back(record);
        return submit_draw(st, call);
    };
    if (statistics) {
        statistics_unmeasured(st, 1u << graphics::STAT_C_PRIMITIVES,
                              "CPrimitives of a mesh draw (the clipper's output is not "
                              "modelled for mesh primitives)");
    }

    // The amplification shader, once: each group's grid and payload.
    graphics::BackendBuffer* as_records = nullptr;
    u64 mesh_groups = groups;
    if (amplified) {
        const u64 bytes = groups * graphics::mesh_vi_as_record_words(payload_words) * 4u;
        if (bytes > kMaxBytes) return too_large();
        as_records = make(bytes, nullptr);
        if (as_records == nullptr ||
            !submit(pso, f.mesh_view_pipeline(*pso, kMeshViewAsCapture, statistics), grid,
                    as_records, nullptr, true)) {
            unimplemented("ExecuteCommandLists",
                          "view instancing (tier 3): the amplification capture pass");
            keep_alive();
            return;
        }
        if (mesh_once) {
            // The mesh groups each amplification group launches, read back:
            // the mesh records of group s start at the sum before it.
            const usize words = static_cast<usize>(bytes / 4u);
            std::vector<u32> host(words);
            graphics::BackendBuffer* staging = nullptr;
            u64 staging_offset = 0;
            u8* mapped = nullptr;
            bool read = reserve(static_cast<usize>(bytes), staging, staging_offset, mapped) &&
                        f.m_backend.copy_buffer_region(staging, staging_offset, as_records, 0,
                                                       bytes);
            if (read) {
                g_replay_cpu_waits.fetch_add(1, std::memory_order_relaxed);
                graphics::frame_profile::WaitScope profile_wait("replay:view-mesh-readback");
                if (auto completion = f.m_backend.flush_completion()) {
                    read = completion->wait();
                } else {
                    read = f.m_backend.wait_idle();
                }
            }
            if (!read || mapped == nullptr) {
                unimplemented("ExecuteCommandLists",
                              "view instancing (tier 3): the amplification records could "
                              "not be read");
                keep_alive();
                return;
            }
            std::memcpy(host.data(), mapped, words * 4u);
            const u32 rec = graphics::mesh_vi_as_record_words(payload_words);
            u64 total = 0;
            for (u64 s = 0; s < groups; ++s) {
                u32* r = host.data() + s * rec;
                r[3] = static_cast<u32>(total);
                total += u64{r[0]} * r[1] * r[2];
                if (total > 0xFFFFFFFFull) return too_large();
            }
            mesh_groups = total;
            as_records = make(bytes, host.data());
            if (as_records == nullptr) {
                keep_alive();
                return;
            }
        }
    }
    graphics::BackendBuffer* ms_records = nullptr;
    if (mesh_once) {
        if (mesh_groups == 0) {
            keep_alive();
            return;
        }
        const u64 bytes = mesh_groups * graphics::mesh_vi_ms_record_words(pso->mesh.ms_layout) * 4u;
        if (bytes > kMaxBytes) return too_large();
        ms_records = make(bytes, nullptr);
        if (ms_records == nullptr) {
            keep_alive();
            return;
        }
    }

    bool first = true;
    bool ok = true;
    for_each_view(st, false, [&]() {
        if (!ok) return;
        PipelineState* v = st.pso;
        if (!mesh_once) {
            // The mesh shader reads SV_ViewID: it runs per view behind the
            // replayed amplification groups.
            ok = submit(v, f.mesh_view_pipeline(*v, kMeshViewStub, statistics), grid, as_records,
                        nullptr, true);
        } else if (first) {
            ok = amplified
                     ? submit(v, f.mesh_view_pipeline(*v, kMeshViewStubCapture, statistics), grid,
                              as_records, ms_records, true)
                     : submit(v, f.mesh_view_pipeline(*v, kMeshViewMsCapture, statistics), grid,
                              nullptr, ms_records, true);
        } else {
            ok = submit(v, f.mesh_view_pipeline(*v, kMeshViewReplay, statistics),
                        {static_cast<u32>(mesh_groups), 1u, 1u}, nullptr, ms_records, false);
        }
        first = false;
    });
    if (!ok) {
        unimplemented("ExecuteCommandLists",
                      "view instancing (tier 3) of a mesh dispatch: a view was not drawn");
    }
    keep_alive();
}

}  // namespace vitreo::winapi::d3d12
