// SPDX-License-Identifier: LGPL-2.1-only
/// @file d3d12_replay_draw.cpp
/// @brief Draws and dispatches translated into backend calls.
///        Split out of d3d12_replay.cpp as verbatim code motion.

#include "winapi/graphics/d3d12/d3d12_replay_internal.hpp"
#include "graphics/contracts/format_converter.hpp"

#include <algorithm>

namespace vitreo::winapi::d3d12 {

namespace {

[[nodiscard]] bool topology_of(u32 topology,
                               graphics::BackendPrimitiveTopology& out) {
    using T = graphics::BackendPrimitiveTopology;
    switch (topology) {
    case D3D_PRIMITIVE_TOPOLOGY_POINTLIST: out = T::PointList; return true;
    case D3D_PRIMITIVE_TOPOLOGY_LINELIST: out = T::LineList; return true;
    case D3D_PRIMITIVE_TOPOLOGY_LINESTRIP: out = T::LineStrip; return true;
    case D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST: out = T::TriangleList; return true;
    case D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP:
        out = T::TriangleStrip;
        return true;
    default:
        return false;
    }
}

[[nodiscard]] graphics::BackendCompare compare_of(u32 v) {
    using C = graphics::BackendCompare;
    switch (v) {
    case D3D12_COMPARISON_FUNC_NEVER: return C::Never;
    case D3D12_COMPARISON_FUNC_LESS: return C::Less;
    case D3D12_COMPARISON_FUNC_EQUAL: return C::Equal;
    case D3D12_COMPARISON_FUNC_LESS_EQUAL: return C::LessEqual;
    case D3D12_COMPARISON_FUNC_GREATER: return C::Greater;
    case D3D12_COMPARISON_FUNC_NOT_EQUAL: return C::NotEqual;
    case D3D12_COMPARISON_FUNC_GREATER_EQUAL: return C::GreaterEqual;
    default: return C::Always;
    }
}

[[nodiscard]] graphics::BackendStencilOperation stencil_op_of(u32 v) {
    using O = graphics::BackendStencilOperation;
    switch (v) {
    case D3D12_STENCIL_OP_ZERO: return O::Zero;
    case D3D12_STENCIL_OP_REPLACE: return O::Replace;
    case D3D12_STENCIL_OP_INCR_SAT: return O::IncrementClamp;
    case D3D12_STENCIL_OP_DECR_SAT: return O::DecrementClamp;
    case D3D12_STENCIL_OP_INVERT: return O::Invert;
    case D3D12_STENCIL_OP_INCR: return O::IncrementWrap;
    case D3D12_STENCIL_OP_DECR: return O::DecrementWrap;
    default: return O::Keep;
    }
}

}  // namespace

void Replayer::draw(ReplayState& st, bool indexed, const cmd::DrawIndexed& d,
                    const graphics::BackendIndirectArgs* indirect) {
    PipelineState* pso = st.pso;
    if (pso == nullptr || pso->compute) {
        d3d12_diag("draw without a graphics pipeline state: skipped");
        return;
    }
    graphics::BackendPrimitiveTopology topology{};
    if (!topology_of(st.topology, topology)) {
        unimplemented("ExecuteCommandLists",
                      "an undefined, adjacency or patch topology");
        return;
    }
    if (!st.roots[0]) {
        d3d12_diag("draw without a graphics root signature: skipped");
        return;
    }
    const GraphicsState& gs = pso->graphics;
    std::array<u32, graphics::BACKEND_MAX_VERTEX_BUFFERS> strides{};
    for (u32 stream = 0; stream < gs.stream_count; ++stream) {
        strides[stream] = st.vbs[gs.stream_slot[stream]].stride;
    }
    graphics::BackendPipeline* pipeline = f.pipeline_variant(*pso, strides);
    if (pipeline == nullptr) return;

    graphics::BackendDrawCall call{};
    call.pipeline = pipeline;
    call.topology = topology;
    for (u32 stream = 0; stream < gs.stream_count; ++stream) {
        const VertexBufferView& view = st.vbs[gs.stream_slot[stream]];
        graphics::BackendBuffer* buffer = nullptr;
        u64 offset = 0;
        if (view.va == 0 || !f.resolve_va(view.va, 0, buffer, offset)) {
            // Nothing bound: the stream reads zeros (Learn, IASetVertexBuffers).
            if (!rr.zeros) {
                graphics::BackendHeapDesc desc{};
                desc.size = 64u * 1024u;
                desc.cpu_visible = true;
                desc.buffers_only = true;
                rr.zeros = f.m_backend.create_heap(desc);
            }
            buffer = rr.zeros ? rr.zeros->buffer() : nullptr;
            offset = 0;
        }
        call.vertex_buffers[stream] = buffer;
        call.vertex_buffer_offsets[stream] = static_cast<unsigned>(offset);
    }
    call.vertex_buffer = call.vertex_buffers[0];
    call.vertex_buffer_offset = call.vertex_buffer_offsets[0];
    if (indexed) {
        graphics::BackendBuffer* buffer = nullptr;
        u64 offset = 0;
        if (st.ib.va == 0 || !f.resolve_va(st.ib.va, 0, buffer, offset)) {
            d3d12_diag("indexed draw with no index buffer: skipped");
            return;
        }
        call.index_buffer = buffer;
        call.index_32bit = st.ib.format == DXGI_FORMAT_R32_UINT;
        // A direct draw folds StartIndexLocation into the buffer offset (the
        // backend's first_vertex is not an index start); an indirect one
        // must not, its argument block carries it (as D3D11's draw does).
        const u64 start =
            indirect != nullptr
                ? 0u
                : static_cast<u64>(d.start_index) * (call.index_32bit ? 4u : 2u);
        call.index_buffer_offset = static_cast<unsigned>(offset + start);
        call.index_count = d.index_count;
        call.base_vertex = d.base_vertex;
        call.first_vertex = 0;
    } else {
        call.vertex_count = d.index_count;
        call.first_vertex = d.start_index;
    }
    call.instance_count = d.instance_count;
    call.first_instance = d.start_instance;
    if (indirect != nullptr) {
        call.indirect = *indirect;
    } else if (call.instance_count == 0 ||
               (indexed ? call.index_count == 0 : call.vertex_count == 0)) {
        return;  // an empty draw draws nothing
    }

    std::vector<u32> vs_push;
    std::vector<u32> ps_push;
    if (!bind_stage(st, STAGE_VERTEX, pso->vs, call.vertex_descriptor_tables,
                    vs_push) ||
        !bind_stage(st, STAGE_PIXEL, pso->ps, call.fragment_descriptor_tables,
                    ps_push)) {
        d3d12_diag("draw: bindings could not be resolved: skipped");
        return;
    }
    call.vertex_constants = vs_push.data();
    call.vertex_constants_size = vs_push.size() * 4;
    call.fragment_constants = ps_push.data();
    call.fragment_constants_size = ps_push.size() * 4;

    // Rasterizer.
    std::vector<std::array<float, 6>> viewports;
    std::vector<std::array<int, 4>> scissors;
    for (const Viewport& v : st.viewports) {
        viewports.push_back(
            {v.x, v.y, v.width, v.height, v.min_depth, v.max_depth});
    }
    for (const Rect& r : st.scissors) {
        scissors.push_back({r.left, r.top, r.right, r.bottom});
    }
    if (!viewports.empty()) call.viewport = viewports[0];
    call.scissor_enabled = !scissors.empty();
    if (!scissors.empty()) call.scissor = scissors[0];
    if (viewports.size() > 1) {
        while (scissors.size() < viewports.size()) {
            scissors.push_back(scissors.empty()
                                   ? std::array<int, 4>{0, 0, 0, 0}
                                   : scissors.back());
        }
        call.viewports = viewports.data();
        call.scissors = scissors.data();
        call.viewport_count = static_cast<unsigned>(viewports.size());
    }
    call.cull_mode = gs.cull_mode == D3D12_CULL_MODE_FRONT
                         ? graphics::BackendCullMode::Front
                     : gs.cull_mode == D3D12_CULL_MODE_BACK
                         ? graphics::BackendCullMode::Back
                         : graphics::BackendCullMode::None;
    call.fill_mode = gs.fill_mode == D3D12_FILL_MODE_WIREFRAME
                         ? graphics::BackendFillMode::Lines
                         : graphics::BackendFillMode::Fill;
    call.front_counter_clockwise = gs.front_ccw;
    call.depth_clip_enabled = gs.depth_clip;
    call.depth_bias = static_cast<float>(gs.depth_bias);
    call.depth_bias_slope_scale = gs.slope_scaled_depth_bias;
    call.depth_bias_clamp = gs.depth_bias_clamp;
    call.blend_color = st.blend_factor;
    // Output merger: the render targets the list bound.
    graphics::BackendRenderPassDesc pass{};
    pass.preserve_color_contents = true;
    pass.occlusion_counter = st.occlusion.get();
    bool any_target = false;
    unsigned width = 0;
    unsigned height = 0;
    for (u32 index = 0; index < st.targets.count && index < 8; ++index) {
        const Descriptor& rtv = st.targets.rtvs[index];
        Resource* r = resource(rtv.resource, rtv.resource_serial);
        if (r == nullptr) continue;
        if (r->texture3d) {
            pass.color_targets_3d[index] = r->texture3d.get();
            pass.color_levels[index] = rtv.most_detailed_mip;
            pass.color_depth_planes[index] = rtv.first_slice;
            pass.color_array_sizes[index] =
                rtv.array_size == 0xFFFFFFFFu ? 1u : rtv.array_size;
        } else if (r->texture) {
            pass.color_targets[index] = r->texture.get();
            const auto format = graphics::pixel_format_from_dxgi(rtv.format);
            pass.color_formats[index] =
                format != r->texture->desc().pixel_format
                    ? format
                    : graphics::PixelFormat::Unknown;
            pass.color_levels[index] = rtv.most_detailed_mip;
            pass.color_slices[index] = rtv.first_slice;
            pass.color_array_sizes[index] = std::max(1u, rtv.array_size);
            width = std::max(1u, r->texture->desc().width >>
                                     rtv.most_detailed_mip);
            height = std::max(1u, r->texture->desc().height >>
                                      rtv.most_detailed_mip);
        } else {
            continue;
        }
        any_target = true;
    }
    const bool has_depth = st.targets.has_dsv;
    bool depth_read_only = false;
    bool stencil_read_only = false;
    if (has_depth) {
        const Descriptor& dsv = st.targets.dsv;
        Resource* r = resource(dsv.resource, dsv.resource_serial);
        if (r != nullptr && r->texture) {
            pass.depth_stencil_target = r->texture.get();
            pass.depth_level = dsv.most_detailed_mip;
            pass.depth_slice = dsv.first_slice;
            pass.depth_array_size = std::max(1u, dsv.array_size);
            depth_read_only = (dsv.flags & D3D12_DSV_FLAG_READ_ONLY_DEPTH) != 0;
            stencil_read_only =
                (dsv.flags & D3D12_DSV_FLAG_READ_ONLY_STENCIL) != 0;
            any_target = true;
        }
    }
    if (!any_target) {
        // No render target and no depth: the pixel shader's UAV writes are
        // its only output. The rasterizer takes its extent from the viewport.
        pass.attachmentless_width = static_cast<unsigned>(
            std::max(1.0f, call.viewport[0] + call.viewport[2]));
        pass.attachmentless_height = static_cast<unsigned>(
            std::max(1.0f, call.viewport[1] + call.viewport[3]));
        pass.attachmentless_samples = std::max(1u, gs.sample_count);
    }
    (void)width;
    (void)height;
    // Depth-stencil state.
    call.depth_test_enabled = gs.depth_enable && pass.depth_stencil_target;
    call.depth_write_enabled =
        call.depth_test_enabled &&
        gs.depth_write_mask == D3D12_DEPTH_WRITE_MASK_ALL && !depth_read_only;
    call.depth_compare = compare_of(gs.depth_func);
    call.stencil_test_enabled = gs.stencil_enable && pass.depth_stencil_target;
    call.stencil_read_mask = gs.stencil_read_mask;
    call.stencil_write_mask = stencil_read_only ? 0 : gs.stencil_write_mask;
    call.stencil_reference = st.stencil_ref;
    const auto face = [](const GraphicsState::StencilFace& s) {
        graphics::BackendStencilFaceDesc out{};
        out.stencil_fail = stencil_op_of(s.fail);
        out.depth_fail = stencil_op_of(s.depth_fail);
        out.pass = stencil_op_of(s.pass);
        out.compare = compare_of(s.func);
        return out;
    };
    call.front_stencil = face(gs.front);
    call.back_stencil = face(gs.back);

    auto render_pass = f.m_backend.create_render_pass_standalone(pass);
    if (!render_pass || !render_pass->valid()) {
        d3d12_diag("draw: the backend refused the render pass");
        return;
    }
    const bool drew = render_pass->draw(call);
    const bool ended = render_pass->end();
    if (!drew || !ended) d3d12_diag("draw: the backend refused the draw");
}

void Replayer::dispatch(ReplayState& st, const cmd::Dispatch& d,
                        const graphics::BackendIndirectArgs* indirect) {
    PipelineState* pso = st.pso;
    if (pso == nullptr || !pso->compute || !pso->compute_pipeline) {
        d3d12_diag("dispatch without a compute pipeline state: skipped");
        return;
    }
    if (indirect == nullptr && (d.x == 0 || d.y == 0 || d.z == 0)) return;
    graphics::BackendDispatchCall call{};
    call.pipeline = pso->compute_pipeline.get();
    call.group_count_x = d.x;
    call.group_count_y = d.y;
    call.group_count_z = d.z;
    call.threads_per_group_x = pso->cs.threads_x;
    call.threads_per_group_y = pso->cs.threads_y;
    call.threads_per_group_z = pso->cs.threads_z;
    std::vector<u32> push;
    if (!bind_stage(st, STAGE_COMPUTE, pso->cs, call.descriptor_tables, push)) {
        d3d12_diag("dispatch without a compute root signature: skipped");
        return;
    }
    call.constants = push.data();
    call.constants_size = push.size() * 4;
    if (indirect != nullptr) call.indirect = *indirect;
    if (!f.m_backend.dispatch(call)) {
        d3d12_diag("dispatch: the backend refused it");
    }
}

}  // namespace vitreo::winapi::d3d12
