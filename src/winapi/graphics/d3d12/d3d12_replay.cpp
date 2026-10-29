// SPDX-License-Identifier: LGPL-2.1-only

/// @file d3d12_replay.cpp
/// @brief ExecuteCommandLists: a closed list's recording translated into
/// backend work, with root signatures and descriptor tables resolved now.
///
/// PLAN decision 6. For each draw or dispatch, the bound root signature, the
/// root arguments and the descriptor heaps are resolved into the backend's
/// descriptor tables -- one per register class (set 0 = b#, 1 = t#, 2 = s#,
/// 3 = u#; slot = register), sized to what the pipeline's shaders declare. A
/// descriptor is read here, when the list executes, never when it was
/// recorded: D3D12 lets an application write a descriptor after recording the
/// draw that reads it (the 1.0 default, and DESCRIPTORS_VOLATILE).
///
/// Each list starts from the default state (Learn, "Command list state"): no
/// pipeline, root signatures, heaps or bindings; a bundle runs with the state
/// of the list that executes it.

#include "winapi/graphics/d3d12/d3d12_internal.hpp"
#include "winapi/graphics/d3d12/d3d12_replay_internal.hpp"
#include "winapi/graphics/common/d3d_sampler.hpp"
#include "winapi/graphics/common/gpu_event_signaler.hpp"
#include "winapi/graphics/d3d11/d3d11_format_caps.hpp"
#include "winapi/graphics/d3d11/d3d11_texel_encode.hpp"
#include "graphics/contracts/format_converter.hpp"
#include "graphics/shaders/dxbc_to_spirv.hpp"
#include "core/memory/guest_memory.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <thread>

namespace vitreo::winapi::d3d12 {

void Replayer::set_root_signature(ReplayState& st,
                                  const cmd::SetRootSignature& c) {
    const u32 bind_point = c.compute ? 1u : 0u;
    std::shared_ptr<const RootLayout> layout;
    if (c.root != 0) {
        RootSignature* root =
            f.find_serial<RootSignature>(c.root, c.serial, Kind::RootSignature);
        if (root != nullptr) layout = root->layout;
    }
    // Learn ("Root signature" / SetGraphicsRootSignature): changing the
    // root signature leaves every root argument undefined; setting the same
    // one keeps them.
    if (layout != st.roots[bind_point]) {
        st.roots[bind_point] = layout;
        st.args[bind_point].assign(layout ? layout->parameters.size() : 0u,
                                   ReplayState::RootArg{});
    }
}

void Replayer::execute_indirect(ReplayState& st, const cmd::ExecuteIndirect& c) {
    CommandSignature* signature = f.find_serial<CommandSignature>(
        c.signature, c.signature_serial, Kind::CommandSignature);
    Resource* args = resource(c.args, c.args_serial);
    if (signature == nullptr || args == nullptr || args->buffer() == nullptr ||
        c.max_count == 0) {
        return;
    }
    const u32 action = signature->arguments.back().type;
    const u32 stride = signature->byte_stride;
    // A bare draw / dispatch signature with no count buffer: each command
    // is the backend's indirect draw or dispatch, read by the GPU.
    const u64 last = args->offset + c.args_offset +
                     static_cast<u64>(c.max_count - 1) * stride;
    if (signature->arguments.size() == 1 && c.count == 0 &&
        last <= 0xFFFFFFFFull) {
        for (u32 index = 0; index < c.max_count; ++index) {
            graphics::BackendIndirectArgs indirect{};
            indirect.buffer = args->buffer();
            indirect.offset = static_cast<unsigned>(
                args->offset + c.args_offset + static_cast<u64>(index) * stride);
            if (action == D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH) {
                dispatch(st, cmd::Dispatch{1, 1, 1}, &indirect);
            } else {
                draw(st, action == D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED,
                     cmd::DrawIndexed{}, &indirect);
            }
        }
        return;
    }
    // Otherwise the commands also set root arguments or buffer views, or
    // their number is in a count buffer: read them once the GPU has written
    // them, and run each as the direct calls it stands for.
    u32 count = c.max_count;
    if (c.count != 0) {
        Resource* count_buffer = resource(c.count, c.count_serial);
        u32 value = 0;
        if (count_buffer == nullptr ||
            !gpu_read(*count_buffer, c.count_offset, &value, 4)) {
            d3d12_diag("ExecuteIndirect: the count buffer could not be read");
            return;
        }
        count = std::min(count, value);
    }
    if (count == 0) return;
    u32 command_bytes = 0;
    for (const auto& argument : signature->arguments) {
        switch (argument.type) {
        case D3D12_INDIRECT_ARGUMENT_TYPE_DRAW: command_bytes += 16; break;
        case D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED: command_bytes += 20; break;
        case D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH: command_bytes += 12; break;
        case D3D12_INDIRECT_ARGUMENT_TYPE_VERTEX_BUFFER_VIEW:
        case D3D12_INDIRECT_ARGUMENT_TYPE_INDEX_BUFFER_VIEW:
            command_bytes += 16;
            break;
        case D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT:
            command_bytes += argument.num_values * 4u;
            break;
        default:
            command_bytes += 8;  // a root CBV / SRV / UAV GPU VA
            break;
        }
    }
    const u64 total = static_cast<u64>(count - 1) * stride + command_bytes;
    if (c.args_offset > args->desc.width ||
        total > args->desc.width - c.args_offset) {
        d3d12_diag("ExecuteIndirect: the commands run past the argument buffer");
        return;
    }
    std::vector<u8> data(static_cast<usize>(total));
    if (!gpu_read(*args, c.args_offset, data.data(), data.size())) {
        d3d12_diag("ExecuteIndirect: the argument buffer could not be read");
        return;
    }
    const bool compute = action == D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;
    const u32 bind_point = compute ? 1u : 0u;
    // Learn (ExecuteIndirect): what the commands set is not left behind for
    // the calls after it; restore the bindings they change.
    const auto saved_args = st.args[bind_point];
    const auto saved_vbs = st.vbs;
    const auto saved_ib = st.ib;
    const auto u32_at = [&](usize at) {
        u32 v = 0;
        std::memcpy(&v, data.data() + at, 4);
        return v;
    };
    const auto u64_at = [&](usize at) {
        u64 v = 0;
        std::memcpy(&v, data.data() + at, 8);
        return v;
    };
    for (u32 index = 0; index < count; ++index) {
        usize at = static_cast<usize>(index) * stride;
        for (const auto& argument : signature->arguments) {
            switch (argument.type) {
            case D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT: {
                auto& list = st.args[bind_point];
                if (argument.root_index < list.size() && st.roots[bind_point]) {
                    const u32 total_values = st.roots[bind_point]
                                                 ->parameters[argument.root_index]
                                                 .num_32bit_values;
                    auto& values = list[argument.root_index].constants;
                    if (values.size() < total_values) values.resize(total_values, 0u);
                    for (u32 v = 0; v < argument.num_values; ++v) {
                        if (argument.dest_offset + v < values.size()) {
                            values[argument.dest_offset + v] = u32_at(at + v * 4u);
                        }
                    }
                    list[argument.root_index].set = true;
                }
                at += argument.num_values * 4u;
                break;
            }
            case D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT_BUFFER_VIEW:
            case D3D12_INDIRECT_ARGUMENT_TYPE_SHADER_RESOURCE_VIEW:
            case D3D12_INDIRECT_ARGUMENT_TYPE_UNORDERED_ACCESS_VIEW: {
                auto& list = st.args[bind_point];
                if (argument.root_index < list.size()) {
                    list[argument.root_index].set = true;
                    list[argument.root_index].va = u64_at(at);
                }
                at += 8;
                break;
            }
            case D3D12_INDIRECT_ARGUMENT_TYPE_VERTEX_BUFFER_VIEW:
                if (argument.slot < st.vbs.size()) {
                    st.vbs[argument.slot] =
                        VertexBufferView{u64_at(at), u32_at(at + 8), u32_at(at + 12)};
                }
                at += 16;
                break;
            case D3D12_INDIRECT_ARGUMENT_TYPE_INDEX_BUFFER_VIEW:
                st.ib = cmd::SetIndexBuffer{u64_at(at), u32_at(at + 8),
                                            u32_at(at + 12)};
                at += 16;
                break;
            case D3D12_INDIRECT_ARGUMENT_TYPE_DRAW:
                draw(st, false,
                     cmd::DrawIndexed{u32_at(at), u32_at(at + 4), u32_at(at + 8),
                                      0, u32_at(at + 12)});
                at += 16;
                break;
            case D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED:
                draw(st, true,
                     cmd::DrawIndexed{u32_at(at), u32_at(at + 4), u32_at(at + 8),
                                      static_cast<i32>(u32_at(at + 12)),
                                      u32_at(at + 16)});
                at += 20;
                break;
            case D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH:
                dispatch(st, cmd::Dispatch{u32_at(at), u32_at(at + 4),
                                           u32_at(at + 8)});
                at += 12;
                break;
            default:
                break;
            }
        }
    }
    st.args[bind_point] = saved_args;
    st.vbs = saved_vbs;
    st.ib = saved_ib;
}

void Replayer::write_immediate(const cmd::WriteImmediate& c) {
    // WriteBufferImmediate: each value lands in order with the GPU work
    // around it -- a GPU copy from upload space, never a CPU store into a
    // buffer the GPU may still be reading.
    graphics::BackendBuffer* staging = nullptr;
    u64 staging_offset = 0;
    u8* host = nullptr;
    if (!reserve(c.writes.size() * 4u, staging, staging_offset, host)) {
        d3d12_diag("WriteBufferImmediate: no upload space");
        return;
    }
    for (usize index = 0; index < c.writes.size(); ++index) {
        const auto& [va, value] = c.writes[index];
        std::memcpy(host + index * 4u, &value, 4u);
        graphics::BackendBuffer* target = nullptr;
        u64 offset = 0;
        if (!f.resolve_va(va, 4, target, offset) ||
            !f.m_backend.copy_buffer_region(target, offset, staging,
                                            staging_offset + index * 4u, 4)) {
            d3d12_diag("WriteBufferImmediate: the backend refused the copy");
        }
    }
}

void Replayer::run(const std::vector<Command>& commands, ReplayState& st) {
    for (const Command& command : commands) {
        std::visit(
            [&](const auto& c) {
                using T = std::decay_t<decltype(c)>;
                if constexpr (std::is_same_v<T, cmd::SetPipelineState>) {
                    st.pso = f.find_serial<PipelineState>(c.pso, c.serial,
                                                          Kind::PipelineState);
                } else if constexpr (std::is_same_v<T, cmd::ClearState>) {
                    PipelineState* pso =
                        c.pso != 0 ? f.find_serial<PipelineState>(
                                         c.pso, c.serial, Kind::PipelineState)
                                   : nullptr;
                    st = ReplayState{};
                    st.pso = pso;
                } else if constexpr (std::is_same_v<T, cmd::SetRootSignature>) {
                    set_root_signature(st, c);
                } else if constexpr (std::is_same_v<T,
                                                    cmd::SetDescriptorHeaps>) {
                    for (u32 i = 0; i < 2; ++i) {
                        st.heaps[i] = c.heaps[i] != 0
                                          ? f.find_serial<DescriptorHeap>(
                                                c.heaps[i], c.serials[i],
                                                Kind::DescriptorHeap)
                                          : nullptr;
                    }
                } else if constexpr (std::is_same_v<T, cmd::SetRootTable>) {
                    auto& args = st.args[c.compute ? 1 : 0];
                    if (c.index < args.size()) {
                        args[c.index].set = true;
                        args[c.index].table = c.handle;
                    }
                } else if constexpr (std::is_same_v<T, cmd::SetRootConstants>) {
                    const u32 bp = c.compute ? 1u : 0u;
                    auto& args = st.args[bp];
                    if (c.index < args.size() && st.roots[bp]) {
                        const u32 total =
                            st.roots[bp]->parameters[c.index].num_32bit_values;
                        auto& values = args[c.index].constants;
                        if (values.size() < total) values.resize(total, 0u);
                        for (usize i = 0; i < c.values.size(); ++i) {
                            if (c.offset + i < values.size()) {
                                values[c.offset + i] = c.values[i];
                            }
                        }
                        args[c.index].set = true;
                    }
                } else if constexpr (std::is_same_v<T, cmd::SetRootView>) {
                    auto& args = st.args[c.compute ? 1 : 0];
                    if (c.index < args.size()) {
                        args[c.index].set = true;
                        args[c.index].va = c.va;
                    }
                } else if constexpr (std::is_same_v<T, cmd::SetTopology>) {
                    st.topology = c.topology;
                } else if constexpr (std::is_same_v<T, cmd::SetVertexBuffers>) {
                    for (usize i = 0; i < c.views.size(); ++i) {
                        if (c.start + i < st.vbs.size()) {
                            st.vbs[c.start + i] = c.views[i];
                        }
                    }
                } else if constexpr (std::is_same_v<T, cmd::SetIndexBuffer>) {
                    st.ib = c;
                } else if constexpr (std::is_same_v<T, cmd::SetViewports>) {
                    st.viewports = c.viewports;
                } else if constexpr (std::is_same_v<T, cmd::SetScissors>) {
                    st.scissors = c.rects;
                } else if constexpr (std::is_same_v<T, cmd::SetRenderTargets>) {
                    st.targets = c;
                } else if constexpr (std::is_same_v<T, cmd::SetBlendFactor>) {
                    st.blend_factor = c.factor;
                } else if constexpr (std::is_same_v<T, cmd::SetStencilRef>) {
                    st.stencil_ref = c.front;
                } else if constexpr (std::is_same_v<T, cmd::SetDepthBounds>) {
                    // Depth-bounds testing is reported unsupported; no PSO
                    // enabling it can be created, so the bounds are inert.
                } else if constexpr (std::is_same_v<T, cmd::Draw>) {
                    if (st.predicated_off) return;
                    draw(st, false,
                         cmd::DrawIndexed{c.vertex_count, c.instance_count,
                                          c.start_vertex, 0, c.start_instance});
                } else if constexpr (std::is_same_v<T, cmd::DrawIndexed>) {
                    if (st.predicated_off) return;
                    draw(st, true, c);
                } else if constexpr (std::is_same_v<T, cmd::Dispatch>) {
                    if (st.predicated_off) return;
                    dispatch(st, c);
                } else if constexpr (std::is_same_v<T, cmd::CopyBufferRegion>) {
                    if (st.predicated_off) return;
                    Resource* dst = resource(c.dst, c.dst_serial);
                    Resource* src = resource(c.src, c.src_serial);
                    if (dst != nullptr && src != nullptr && c.bytes != 0 &&
                        !f.m_backend.copy_buffer_region(
                            dst->buffer(), dst->offset + c.dst_offset,
                            src->buffer(), src->offset + c.src_offset,
                            c.bytes)) {
                        d3d12_diag("CopyBufferRegion: backend refused");
                    }
                } else if constexpr (std::is_same_v<T,
                                                    cmd::CopyTextureRegion>) {
                    if (st.predicated_off) return;
                    copy_texture_region(c);
                } else if constexpr (std::is_same_v<T, cmd::CopyResource>) {
                    if (st.predicated_off) return;
                    copy_resource(c);
                } else if constexpr (std::is_same_v<T,
                                                    cmd::ResolveSubresource>) {
                    if (st.predicated_off) return;
                    Resource* dst = resource(c.dst, c.dst_serial);
                    Resource* src = resource(c.src, c.src_serial);
                    if (dst != nullptr && src != nullptr && dst->texture &&
                        src->texture) {
                        resolve(*dst, *src, c);
                    }
                } else if constexpr (std::is_same_v<T, cmd::ClearRenderTarget>) {
                    if (st.predicated_off) return;
                    clear_render_target(c);
                } else if constexpr (std::is_same_v<T, cmd::ClearDepthStencil>) {
                    if (st.predicated_off) return;
                    clear_depth_stencil(c);
                } else if constexpr (std::is_same_v<T,
                                                    cmd::ClearUnorderedAccess>) {
                    if (st.predicated_off) return;
                    clear_unordered_access(c);
                } else if constexpr (std::is_same_v<T, cmd::Event>) {
                    if (c.kind == 0) {
                        f.m_backend.insert_debug_signpost(c.label);
                    } else if (c.kind == 1) {
                        f.m_backend.push_debug_group(c.label);
                    } else {
                        f.m_backend.pop_debug_group();
                    }
                } else if constexpr (std::is_same_v<T, cmd::ExecuteBundle>) {
                    // A bundle runs on the caller's state (Learn, "Bundles":
                    // descriptor heaps and root arguments are inherited);
                    // its pipeline starts as its own initial state.
                    PipelineState* saved = st.pso;
                    st.pso = c.bundle->initial_pso != 0
                                 ? f.find_serial<PipelineState>(
                                       c.bundle->initial_pso,
                                       c.bundle->initial_pso_serial,
                                       Kind::PipelineState)
                                 : nullptr;
                    run(c.bundle->commands, st);
                    if (st.pso == nullptr) st.pso = saved;
                } else if constexpr (std::is_same_v<T, cmd::WriteImmediate>) {
                    write_immediate(c);
                } else if constexpr (std::is_same_v<T, cmd::Barrier>) {
                    // Ordering is the backend's hazard tracking (see
                    // d3d12_command_list.cpp, ResourceBarrier).
                } else if constexpr (std::is_same_v<T, cmd::Query>) {
                    query(st, c);
                } else if constexpr (std::is_same_v<T, cmd::ResolveQuery>) {
                    resolve_query(c);
                } else if constexpr (std::is_same_v<T, cmd::SetPredication>) {
                    set_predication(st, c);
                } else if constexpr (std::is_same_v<T, cmd::ExecuteIndirect>) {
                    if (st.predicated_off) return;
                    execute_indirect(st, c);
                }
            },
            command);
    }
}

void Frontend::replay_list(CommandQueue& queue, const Recording& recording) {
    (void)queue;
    if (!m_replay) m_replay = std::make_shared<ReplayResources>();
    ReplayState state{};
    if (recording.initial_pso != 0) {
        state.pso = find_serial<PipelineState>(recording.initial_pso,
                                               recording.initial_pso_serial,
                                               Kind::PipelineState);
    }
    // Scratch textures of earlier executions the GPU has finished with.
    std::erase_if(m_replay->scratch_textures, [this](const auto& scratch) {
        return scratch.generation != 0 &&
               m_backend.generation_complete(scratch.generation);
    });
    Replayer replayer(*this, *m_replay);
    replayer.run(recording.commands, state);
}

}  // namespace vitreo::winapi::d3d12
