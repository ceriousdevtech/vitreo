// SPDX-License-Identifier: LGPL-2.1-only
/// @file d3d12_replay_draw.cpp
/// @brief Draws and dispatches translated into backend calls.
///        Split out of d3d12_replay.cpp as verbatim code motion.

#include "winapi/graphics/d3d12/d3d12_replay_internal.hpp"
#include "graphics/contracts/format_converter.hpp"
#include "graphics/shaders/dxbc_to_spirv.hpp"
#include "graphics/shaders/gs_passthrough.hpp"
#include "graphics/shaders/ia_resolve.hpp"
#include "graphics/shaders/raster_emulation.hpp"
#include "graphics/shaders/stream_out_repack.hpp"
#include "graphics/shaders/tessellator.hpp"

#include <algorithm>
#include <cstring>
#include <memory>

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

[[nodiscard]] u32 topology_vertices(u32 topology) {
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
    default: return 0;
    }
}

[[nodiscard]] graphics::PrimitiveAssembly assembly_of(u32 topology) {
    if (topology == D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP_ADJ) {
        return graphics::PrimitiveAssembly::StripAdjacency;
    }
    if (topology == D3D_PRIMITIVE_TOPOLOGY_LINESTRIP ||
        topology == D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP ||
        topology == D3D_PRIMITIVE_TOPOLOGY_LINESTRIP_ADJ) {
        return graphics::PrimitiveAssembly::Strip;
    }
    return graphics::PrimitiveAssembly::List;
}

[[nodiscard]] graphics::BackendPrimitiveTopology list_topology(u32 output) {
    if (output == 1u) return graphics::BackendPrimitiveTopology::PointList;
    if (output == 3u) return graphics::BackendPrimitiveTopology::LineList;
    return graphics::BackendPrimitiveTopology::TriangleList;
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

/// Shader model 5.1, or a resource the flat constant-buffer window cannot
/// hold. Matches needs_descriptor_tables in the pipeline.
[[nodiscard]] bool uses_lowered_tables(const CompiledShader& shader) {
    if (shader.table_mode) return true;
    for (const auto& declaration : shader.declarations) {
        if (declaration.space != 0 || declaration.unbounded()) return true;
        if (declaration.register_class ==
                graphics::DxbcRegisterClass::Sampler ||
            declaration.opcode == 88u || declaration.opcode == 156u) {
            return true;
        }
        if (declaration.register_class !=
            graphics::DxbcRegisterClass::ConstantBuffer) {
            return true;
        }
        if (declaration.upper >= graphics::BACKEND_MAX_CONSTANT_BUFFER_SLOTS) {
            return true;
        }
    }
    return false;
}

/// Learn: D3D12_GPU_VIRTUAL_ADDRESS is a UINT64. Metal's setBuffer:offset:
/// is pointer-width, so on arm64 every 64-bit byte index fits. A narrower
/// offset cannot name the byte.
[[nodiscard]] bool offset_fits_backend(u64 offset) noexcept {
    if constexpr (sizeof(usize) >= sizeof(u64)) {
        (void)offset;
        return true;
    } else {
        return offset <= static_cast<u64>(~usize{0});
    }
}

/// A sum of two byte offsets that does not fit in 64 bits must not wrap and
/// must not be reported as offset 0.
[[nodiscard]] bool add_byte_offset(u64 base, u64 extra, u64& out) noexcept {
    if (base > ~u64{0} - extra) return false;
    out = base + extra;
    return true;
}

/// Learn (CreateUnorderedAccessView, D3D12_BUFFER_UAV): the counter is the
/// descriptor's counter resource at CounterOffsetInBytes, not a buffer the
/// runtime invents. A descriptor with no counter resource fails the call.
/// The byte offset is the resource's placement plus CounterOffsetInBytes,
/// for a compute dispatch and for a pixel-shader draw alike.
[[nodiscard]] bool bind_uav_counters(
    Frontend& f, const ReplayState& st, u32 bind_point, u32 visibility,
    bool every_parameter, const CompiledShader& shader,
    std::array<const graphics::BackendBuffer*,
               graphics::BACKEND_MAX_UAV_COUNTER_BINDINGS>& buffers,
    std::array<u64, graphics::BACKEND_MAX_UAV_COUNTER_BINDINGS>& offsets) {
    buffers.fill(nullptr);
    offsets.fill(0);
    if (shader.uav_counter_registers.empty()) return true;
    const RootLayout* root = st.roots[bind_point].get();
    if (root == nullptr) return false;
    const auto& args = st.args[bind_point];
    const auto visible = [&](u32 vis) {
        if (every_parameter) return true;
        return vis == D3D12_SHADER_VISIBILITY_ALL || vis == visibility;
    };
    for (u32 k = 0; k < shader.uav_counter_registers.size(); ++k) {
        if (k >= graphics::BACKEND_MAX_UAV_COUNTER_BINDINGS) {
            unimplemented("ExecuteCommandLists",
                          "more UAV counters than the stage can bind");
            return false;
        }
        const u32 reg = shader.uav_counter_registers[k];
        const u32 space = k < shader.uav_counter_spaces.size()
                              ? shader.uav_counter_spaces[k]
                              : 0u;
        const Descriptor* view = nullptr;
        bool root_descriptor = false;
        for (u32 index = 0; index < root->parameters.size(); ++index) {
            const BindingParameter& parameter = root->parameters[index];
            if (!visible(parameter.visibility)) continue;
            const ReplayState::RootArg* arg =
                index < args.size() ? &args[index] : nullptr;
            if (parameter.type == D3D12_ROOT_PARAMETER_TYPE_UAV &&
                parameter.space == space && parameter.shader_register == reg) {
                root_descriptor = true;
                view = nullptr;
            }
            if (parameter.type != D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE ||
                arg == nullptr || !arg->set) {
                continue;
            }
            u32 base = 0;
            DescriptorHeap* heap = f.gpu_descriptor(arg->table, base);
            if (heap == nullptr) continue;
            for (const BindingRange& range : parameter.ranges) {
                if (range.type != D3D12_DESCRIPTOR_RANGE_TYPE_UAV ||
                    range.space != space || reg < range.base_register) {
                    continue;
                }
                const u64 available =
                    heap->records.size() >
                            static_cast<u64>(base) + range.table_offset
                        ? heap->records.size() - base - range.table_offset
                        : 0u;
                const u64 count = range.count == 0xFFFFFFFFu
                                      ? available
                                      : std::min<u64>(range.count, available);
                if (reg - range.base_register >= count) continue;
                const u64 at = static_cast<u64>(base) + range.table_offset +
                               (reg - range.base_register);
                if (at >= heap->records.size()) continue;
                view = &heap->records[at];
                root_descriptor = false;
            }
        }
        if (root_descriptor || view == nullptr ||
            view->type != DescriptorType::Uav || view->counter == 0) {
            unimplemented("ExecuteCommandLists",
                          "a UAV counter whose descriptor has no counter "
                          "resource");
            return false;
        }
        Resource* counter = f.find_serial<Resource>(
            view->counter, view->counter_serial, Kind::Resource);
        graphics::BackendBuffer* buffer =
            counter != nullptr ? counter->buffer() : nullptr;
        if (buffer == nullptr || counter == nullptr) {
            unimplemented("ExecuteCommandLists",
                          "a UAV counter whose counter resource is not a "
                          "buffer");
            return false;
        }
        // The byte offset is the counter resource's placement plus
        // CounterOffsetInBytes (UINT). A sum that does not fit in 64 bits
        // must not wrap onto a low address, including 0.
        u64 byte = 0;
        if (!add_byte_offset(counter->offset, view->counter_offset, byte)) {
            unimplemented("ExecuteCommandLists",
                          "a UAV counter whose byte offset does not fit in "
                          "64 bits");
            return false;
        }
        if (!offset_fits_backend(byte)) {
            unimplemented("ExecuteCommandLists",
                          "a UAV counter whose counter resource is not a "
                          "buffer");
            return false;
        }
        buffers[k] = buffer;
        offsets[k] = byte;
    }
    return true;
}

struct SoSlotView {
    graphics::BackendBuffer* data{nullptr};
    u64 data_offset{0};
    u64 size{0};
    graphics::BackendBuffer* filled{nullptr};
    u64 filled_offset{0};
    bool bound{false};
};

[[nodiscard]] bool so_kernels(Frontend& f) {
    const auto build =
        [&](std::unique_ptr<graphics::BackendComputePipeline>& slot,
            const char* entry, u64 key) {
            if (slot && slot->valid()) return true;
            graphics::BackendComputePipelineDesc desc{};
            desc.compute_entry_point = entry;
            desc.compute_shader_msl = graphics::stream_out_repack_msl();
            desc.cache_key = key;
            slot = f.m_backend.create_compute_pipeline(desc);
            return slot && slot->valid();
        };
    if (!build(f.m_so_prepare, graphics::kStreamOutPrepareEntryPoint,
               0xD31250B1ull) ||
        !build(f.m_so_repack, graphics::kStreamOutRepackEntryPoint,
               0xD31250A2ull) ||
        !build(f.m_so_compact, graphics::kStreamOutCompactEntryPoint,
               0xD31250A3ull)) {
        return false;
    }
    if (!f.m_so_stats) {
        graphics::BackendBufferDesc desc{};
        desc.byte_width = graphics::SO_STATS_WORDS * sizeof(u32);
        desc.shader_resource = true;
        desc.unordered_access = true;
        const std::array<u32, graphics::SO_STATS_WORDS> zero{};
        f.m_so_stats = f.m_backend.create_buffer(desc, zero.data());
    }
    return f.m_so_stats && f.m_so_stats->valid();
}

[[nodiscard]] bool so_slot(Frontend& f, u32 slot, SoSlotView& out) {
    out = {};
    if (slot >= f.m_so_targets.size() || !f.m_so_targets[slot].bound) {
        return true;
    }
    const auto& target = f.m_so_targets[slot];
    graphics::BackendBuffer* data = nullptr;
    graphics::BackendBuffer* filled = nullptr;
    u64 data_at = 0;
    u64 filled_at = 0;
    if (!f.resolve_va(target.data_va, target.size, data, data_at) ||
        !f.resolve_va(target.filled_va, 4, filled, filled_at)) {
        return false;
    }
    // BufferLocation and BufferFilledSizeLocation are GPU virtual addresses
    // (UINT64). A backend whose offset is narrower than the byte cannot
    // name it; the stream is not recorded at a truncated offset.
    if (!offset_fits_backend(data_at) || !offset_fits_backend(filled_at)) {
        unimplemented("ExecuteCommandLists",
                      "stream output could not be recorded");
        return false;
    }
    out.bound = true;
    out.data = data;
    out.data_offset = data_at;
    out.size = target.size;
    out.filled = filled;
    out.filled_offset = filled_at;
    return true;
}

/// One stream: vitreo_so_prepare, then vitreo_so_repack. A slot this stream
/// declares with nothing bound writes nothing for the stream; the others
/// are separate calls. @p stats receives the stream's {needed, written}
/// counts (SO_STATISTICS); null adds them to scratch nobody reads.
[[nodiscard]] bool dispatch_stream(
    Frontend& f, u32 stream, u32 slot_mask,
    const std::array<u32, graphics::BACKEND_MAX_STREAM_OUT_TARGETS>& strides,
    const std::vector<graphics::StreamOutRepackEntry>& entries,
    graphics::BackendBuffer* records, u32 records_offset, u32 record_vec4s,
    graphics::StreamOutIndexMode index_mode, u32 first, i32 base_vertex,
    bool strip, u32 vertices_per_primitive, u32 primitives,
    graphics::BackendBuffer* count, u32 count_offset, u32 count_mode,
    u32 max_vertices, graphics::BackendBuffer* indices, u32 indices_offset,
    std::vector<std::unique_ptr<graphics::BackendBuffer>>& kept,
    graphics::BackendBuffer* stats) {
    if (!so_kernels(f)) return false;
    std::array<SoSlotView, graphics::BACKEND_MAX_STREAM_OUT_TARGETS> slots{};
    bool any = false;
    bool missing = false;
    for (u32 slot = 0; slot < slots.size(); ++slot) {
        if (!so_slot(f, slot, slots[slot])) return false;
        any = any || slots[slot].bound;
        if ((slot_mask & (1u << slot)) != 0 && !slots[slot].bound) missing = true;
    }
    const bool write_nothing = !any || missing;
    const u32 mask = write_nothing ? 0u : slot_mask;

    graphics::SoPrepareParams params{};
    params.primitives = primitives;
    params.count_mode = count_mode;
    params.vertices_per_primitive = vertices_per_primitive;
    params.stream = stream;
    params.slot_mask = mask;
    params.write_nothing = write_nothing ? 1u : 0u;
    params.strides = strides;
    graphics::BackendBufferDesc op_desc{};
    op_desc.byte_width = graphics::SO_OP_WORDS * sizeof(u32);
    op_desc.shader_resource = true;
    op_desc.unordered_access = true;
    auto op = f.m_backend.create_buffer(op_desc, nullptr);
    if (!op || !op->valid()) return false;
    graphics::BackendBuffer* op_buffer = op.get();
    kept.push_back(std::move(op));

    graphics::BackendDispatchCall prepare{};
    prepare.pipeline = f.m_so_prepare.get();
    prepare.group_count_x = prepare.group_count_y = prepare.group_count_z = 1;
    prepare.buffers[graphics::SO_PREPARE_COUNT_INDEX] =
        count != nullptr ? count : op_buffer;
    prepare.buffer_offsets[graphics::SO_PREPARE_COUNT_INDEX] =
        count != nullptr ? count_offset : 0u;
    for (u32 slot = 0; slot < slots.size(); ++slot) {
        graphics::BackendBuffer* word = op_buffer;
        u64 word_offset = 0;
        if ((mask & (1u << slot)) != 0) {
            // SizeInBytes is a UINT64 capacity (d3d12.h). The prepare
            // constants hold that width; the filled-size word stays a UINT.
            params.sizes[slot] = slots[slot].size;
            word = slots[slot].filled;
            word_offset = slots[slot].filled_offset;
        }
        prepare.buffers[graphics::SO_PREPARE_FILLED_BASE + slot] = word;
        prepare.buffer_offsets[graphics::SO_PREPARE_FILLED_BASE + slot] =
            word_offset;
    }
    prepare.buffers[graphics::SO_PREPARE_OP_INDEX] = op_buffer;
    // The open SO_STATISTICS interval, or scratch nobody reads.
    prepare.buffers[graphics::SO_PREPARE_STATS_INDEX] =
        stats != nullptr ? stats : f.m_so_stats.get();
    prepare.constants = &params;
    prepare.constants_size = sizeof(params);
    if (!f.m_backend.dispatch(prepare)) return false;
    if (write_nothing || max_vertices == 0 || entries.empty()) return true;

    graphics::StreamOutRepackDesc desc{};
    desc.index_mode = index_mode;
    desc.first = first;
    desc.base_vertex = base_vertex;
    desc.vertices_per_primitive = vertices_per_primitive;
    desc.strip = strip;
    desc.record_vec4s = record_vec4s;
    desc.entries = entries;
    desc.strides = strides;
    const auto words = graphics::build_stream_out_control(desc);
    graphics::BackendBufferDesc control_desc{};
    control_desc.byte_width = words.size() * sizeof(u32);
    control_desc.shader_resource = true;
    auto control = f.m_backend.create_buffer(control_desc, words.data());
    if (!control || !control->valid()) return false;
    graphics::BackendBuffer* control_buffer = control.get();
    kept.push_back(std::move(control));

    graphics::BackendDispatchCall call{};
    call.pipeline = f.m_so_repack.get();
    call.threads_per_group_x = graphics::SO_REPACK_THREADS_PER_GROUP;
    call.group_count_x =
        (max_vertices + graphics::SO_REPACK_THREADS_PER_GROUP - 1) /
        graphics::SO_REPACK_THREADS_PER_GROUP;
    call.group_count_y = call.group_count_z = 1;
    call.buffers[graphics::SO_REPACK_CAPTURE_INDEX] = records;
    call.buffer_offsets[graphics::SO_REPACK_CAPTURE_INDEX] = records_offset;
    call.buffers[graphics::SO_REPACK_CONTROL_INDEX] = control_buffer;
    call.buffers[graphics::SO_REPACK_INDICES_INDEX] =
        indices != nullptr ? indices : control_buffer;
    call.buffer_offsets[graphics::SO_REPACK_INDICES_INDEX] =
        indices != nullptr ? indices_offset : 0u;
    call.buffers[graphics::SO_REPACK_OP_INDEX] = op_buffer;
    for (u32 slot = 0; slot < slots.size(); ++slot) {
        graphics::BackendBuffer* target = control_buffer;
        u64 target_offset = 0;
        if ((mask & (1u << slot)) != 0) {
            target = slots[slot].data;
            // BufferLocation's byte offset is a UINT64. Narrowing it would
            // write the stream at a low address.
            target_offset = slots[slot].data_offset;
        }
        call.buffers[graphics::SO_REPACK_TARGET_BASE + slot] = target;
        call.buffer_offsets[graphics::SO_REPACK_TARGET_BASE + slot] =
            target_offset;
    }
    return f.m_backend.dispatch(call);
}

[[nodiscard]] std::vector<graphics::StreamOutRepackEntry> so_entries(
    const StreamOutput& so, u32 stream) {
    std::vector<graphics::StreamOutRepackEntry> entries;
    for (const auto& entry : so.entries) {
        if (entry.stream != stream) continue;
        graphics::StreamOutRepackEntry out{};
        out.slot = entry.slot;
        out.dst_dword = entry.dst_dword;
        out.start_component = entry.start_component;
        out.component_count = entry.component_count;
        out.src_record = graphics::STREAM_OUT_GAP;
        if (!entry.gap) {
            const auto found = std::find(so.registers.begin(), so.registers.end(),
                                         entry.output_register);
            if (found == so.registers.end()) continue;
            out.src_record =
                static_cast<u32>(found - so.registers.begin());
        }
        entries.push_back(out);
    }
    return entries;
}

/// Geometry-shader streams, one at a time. The expansion buffer is already
/// in D3D's emission order; vitreo_so_compact turns each stream's written
/// slots into the index list the repack walks. One stream filling up does
/// not stop the others (a separate prepare per stream).
[[nodiscard]] bool geometry_stream_out(
    Frontend& f, const PipelineState& pso, graphics::BackendBuffer* expanded,
    graphics::BackendBuffer* counter, u32 invocations, u32 vertices_per_invocation,
    std::vector<std::unique_ptr<graphics::BackendBuffer>>& kept,
    graphics::BackendBuffer* stats) {
    if (!pso.stream_output.active || expanded == nullptr || counter == nullptr ||
        invocations == 0 || vertices_per_invocation == 0) {
        return true;
    }
    if (!so_kernels(f)) return false;
    const u32 output = pso.geometry.output_topology;
    const u32 vpp = output == 1u ? 1u : output == 3u ? 2u : output == 5u ? 3u : 0u;
    if (vpp == 0) {
        unimplemented("ExecuteCommandLists",
                      "stream output of this geometry-shader output topology");
        return false;
    }
    const u32 record_vec4s = static_cast<u32>(
        std::max<usize>(pso.stream_output.registers.size(), 1));
    for (u32 stream = 0; stream < D3D12_SO_STREAM_COUNT; ++stream) {
        bool declared = false;
        for (const auto& entry : pso.stream_output.entries) {
            declared = declared || entry.stream == stream;
        }
        if (!declared) continue;
        const u64 max_vertices =
            static_cast<u64>(invocations) * vertices_per_invocation;
        if (max_vertices > 0xFFFFFFFFull) return false;
        graphics::BackendBufferDesc desc{};
        desc.shader_resource = true;
        desc.unordered_access = true;
        desc.byte_width =
            static_cast<usize>(std::max<u64>(max_vertices, 1) * sizeof(u32));
        auto order = f.m_backend.create_buffer(desc, nullptr);
        desc.byte_width = 16;
        auto total = f.m_backend.create_buffer(desc, nullptr);
        if (!order || !total) return false;
        graphics::BackendBuffer* order_buffer = order.get();
        graphics::BackendBuffer* total_buffer = total.get();
        kept.push_back(std::move(order));
        kept.push_back(std::move(total));

        graphics::SoCompactParams compact{};
        compact.invocations = invocations;
        compact.capacity = vertices_per_invocation;
        compact.region_base =
            stream * invocations * vertices_per_invocation;
        graphics::BackendDispatchCall call{};
        call.pipeline = f.m_so_compact.get();
        call.group_count_x = call.group_count_y = call.group_count_z = 1;
        call.threads_per_group_x = graphics::SO_COMPACT_THREADS;
        call.constants = &compact;
        call.constants_size = sizeof(compact);
        call.buffers[graphics::SO_COMPACT_COUNT_INDEX] = total_buffer;
        call.buffers[graphics::SO_COMPACT_COUNTS_INDEX] = counter;
        call.buffer_offsets[graphics::SO_COMPACT_COUNTS_INDEX] =
            (graphics::kGsInvocationCountBase + stream * invocations) *
            sizeof(u32);
        call.buffers[graphics::SO_COMPACT_ORDER_INDEX] = order_buffer;
        call.buffers[graphics::SO_COMPACT_TOTAL_INDEX] = total_buffer;
        if (!f.m_backend.dispatch(call)) return false;
        if (!dispatch_stream(
                f, stream, pso.stream_output.stream_slots[stream],
                pso.stream_output.strides, so_entries(pso.stream_output, stream),
                expanded, 0, record_vec4s, graphics::StreamOutIndexMode::Uint32,
                0, 0, false, vpp, 0, total_buffer, 0, 2,
                static_cast<u32>(max_vertices), order_buffer, 0, kept, stats)) {
            return false;
        }
    }
    return true;
}

/// Domain-shader stream output, the "NULL GS" of CreateGeometryShaderWithStreamOutput
/// (Learn): the domain shader's outputs are the vertices written, one per
/// tessellated point, in the tessellator's list order. The repack already
/// walks a point, line, or triangle list. Any other tessellator output stays
/// unimplemented rather than becoming a guessed vertex list.
[[nodiscard]] bool domain_stream_out(
    Frontend& f, const PipelineState& pso, u32 output_primitive,
    graphics::BackendBuffer* records, u32 record_vec4s,
    graphics::BackendBuffer* indices, graphics::BackendBuffer* block,
    u32 max_vertices,
    std::vector<std::unique_ptr<graphics::BackendBuffer>>& kept,
    graphics::BackendBuffer* stats) {
    if (!pso.stream_output.active) return true;
    const u32 vpp = output_primitive == 1u   ? 1u
                    : output_primitive == 2u ? 2u
                    : (output_primitive == 3u || output_primitive == 4u) ? 3u
                                                                         : 0u;
    if (vpp == 0) {
        unimplemented("ExecuteCommandLists",
                      "stream output of this tessellator output topology");
        return false;
    }
    if (records == nullptr || indices == nullptr || block == nullptr) {
        return false;
    }
    if (max_vertices == 0) return true;
    return dispatch_stream(
        f, 0, pso.stream_output.stream_slots[0], pso.stream_output.strides,
        so_entries(pso.stream_output, 0), records, 0, record_vec4s,
        graphics::StreamOutIndexMode::Uint32, 0, 0, false, vpp, 0, block,
        graphics::tess::TESS_BLOCK_PRIMITIVES * static_cast<u32>(sizeof(u32)),
        1, max_vertices, indices, 0, kept, stats);
}

}  // namespace

bool Replayer::bind_vertex_streams(ReplayState& st,
                                   graphics::BackendDrawCall& call,
                                   bool per_instance_capture, u32 start,
                                   u32 instance) {
    const GraphicsState& gs = st.pso->graphics;
    const auto zeros = [&]() -> graphics::BackendBuffer* {
        if (!rr.zeros) {
            graphics::BackendHeapDesc desc{};
            desc.size = 64u * 1024u;
            desc.cpu_visible = true;
            desc.buffers_only = true;
            rr.zeros = f.m_backend.create_heap(desc);
        }
        return rr.zeros ? rr.zeros->buffer() : nullptr;
    };
    if (gs.vertex_pulling) {
        // Learn (D3D12_VERTEX_BUFFER_VIEW): SizeInBytes bytes from
        // BufferLocation; the shader reads 0 past them, and an unbound slot
        // is size 0.
        std::array<u32, graphics::VERTEX_PULL_TABLE_ENTRIES *
                            graphics::VERTEX_PULL_TABLE_ENTRY_WORDS>
            table{};
        if (per_instance_capture) {
            // The capture draws first instance start + instance alone.
            u32* header = &table[graphics::VERTEX_PULL_TABLE_HEADER *
                                 graphics::VERTEX_PULL_TABLE_ENTRY_WORDS];
            header[0] = 1;
            header[1] = start;
            header[2] = instance;
        }
        for (u32 slot = 0; slot < graphics::VERTEX_PULL_TABLE_SLOTS; ++slot) {
            const VertexBufferView& view = st.vbs[slot];
            graphics::BackendBuffer* buffer = nullptr;
            u64 offset = 0;
            if (view.va == 0 || view.size == 0 ||
                !f.resolve_va(view.va, 0, buffer, offset) ||
                buffer == nullptr || buffer->device_address() == 0) {
                continue;
            }
            const u64 address = buffer->device_address() + offset;
            u32* entry = &table[slot * graphics::VERTEX_PULL_TABLE_ENTRY_WORDS];
            entry[0] = static_cast<u32>(address);
            entry[1] = static_cast<u32>(address >> 32);
            entry[2] = view.stride;
            entry[3] = view.size;
            call.vertex_buffers[slot] = buffer;
        }
        graphics::BackendBuffer* buffer = nullptr;
        u64 offset = 0;
        if (!upload(table.data(), sizeof(table), buffer, offset)) return false;
        call.vertex_pull_table = buffer;
        call.vertex_pull_table_offset = offset;
        call.vertex_buffer = nullptr;
        call.vertex_buffer_offset = 0;
        return true;
    }
    for (u32 stream = 0; stream < gs.stream_count; ++stream) {
        const VertexBufferView& view = st.vbs[gs.stream_slot[stream]];
        graphics::BackendBuffer* buffer = nullptr;
        u64 offset = 0;
        if (view.va == 0 || !f.resolve_va(view.va, 0, buffer, offset)) {
            // Nothing bound: the stream reads zeros (Learn, IASetVertexBuffers).
            buffer = zeros();
            offset = 0;
        }
        call.vertex_buffers[stream] = buffer;
        call.vertex_buffer_offsets[stream] = offset;
    }
    call.vertex_buffer = call.vertex_buffers[0];
    call.vertex_buffer_offset = call.vertex_buffer_offsets[0];
    return true;
}

/// View instancing by draw-level looping (DirectX-Specs "View Instancing";
/// ViewInstancingTier 2): each enabled view is the whole draw again, with
/// that view's pipeline (its shaders see SV_ViewID == view) and its
/// D3D12_VIEW_INSTANCE_LOCATION added to the viewport and render-target
/// array indices the shaders select -- "if the result of the add is out of
/// range it clamps to 0". Views run in order, so primitive p of view v
/// reaches the rasterizer before primitive p' > p of the same view.
void Replayer::draw(ReplayState& st, bool indexed, const cmd::DrawIndexed& d,
                    const graphics::BackendIndirectArgs* indirect) {
    if (graphics::frame_profile::enabled()) [[unlikely]] {
        graphics::frame_profile::add(graphics::frame_profile::Counter::Draws);
    }
    PipelineState* pso = st.pso;
    // A vertex shader that writes SV_RenderTargetArrayIndex and reaches the
    // rasterizer itself (no geometry or tessellation emulation) applies
    // D3D11.3 17.18 in its epilogue: an index past the bound views' slices
    // selects slice 0 (Metal clamps to the last one). The slice count is the
    // smallest bound view's.
    // So does the pass-through vertex shader of an emulated geometry or
    // domain shader that writes it (push member 14 either way).
    const bool shader_layer =
        pso != nullptr && !pso->compute &&
        (pso->geometry.active ? pso->geometry.kernel.writes_layer
         : pso->tessellation.active ? pso->tessellation.domain_stage.writes_layer
                                    : pso->vs.writes_layer);
    u32 slices = ~0u;
    const auto view_slices = [](const Descriptor& v) {
        return v.array_size == 0xFFFFFFFFu ? 1u : std::max(1u, v.array_size);
    };
    for (u32 index = 0; index < st.targets.count && index < 8; ++index) {
        if (st.targets.rtvs[index].resource != 0) {
            slices = std::min(slices, view_slices(st.targets.rtvs[index]));
        }
    }
    if (st.targets.has_dsv) slices = std::min(slices, view_slices(st.targets.dsv));
    st.layer_limit = shader_layer && slices != ~0u ? (graphics::kLayerLimitKnown | slices) : 0u;
    st.layer_addend = 0;
    if (pso != nullptr && !pso->compute && pso->view_instance_count > 1 &&
        pso->vi_first != kViLoop) {
        // ViewInstancingTier 3: the stages before the first one reading
        // SV_ViewID run once; draw_lowered loops the views from there.
        st.vi_deferred = true;
        st.vi_shader_layer = shader_layer;
        draw_view(st, indexed, d, indirect);
        st.vi_deferred = false;
        st.vi_view = 0;
        return;
    }
    for_each_view(st, shader_layer,
                  [&]() { draw_view(st, indexed, d, indirect); });
}

void Replayer::for_each_view(ReplayState& st, bool shader_layer,
                             const std::function<void()>& one) {
    PipelineState* pso = st.pso;
    if (pso == nullptr || pso->compute || pso->view_instance_count == 0) {
        one();
        return;
    }
    const bool masking = (pso->view_instancing_flags &
                          D3D12_VIEW_INSTANCING_FLAG_ENABLE_VIEW_INSTANCE_MASKING) != 0;
    const std::vector<Viewport> viewports = st.viewports;
    const std::vector<Rect> scissors = st.scissors;
    const cmd::SetRenderTargets targets = st.targets;
    const u32 skip = st.statistics_skip;
    bool first = true;
    for (u32 v = 0; v < pso->view_instance_count; ++v) {
        if (masking && (st.view_mask & (1u << v)) == 0) continue;
        PipelineState* view = v == 0 ? pso : pso->views[v].get();
        if (view == nullptr) continue;
        st.vi_view = v;
        // Tier 3: the input assembler (and, in a deferred draw, every stage
        // before the loop) is counted once ("For the stages that come before
        // X, view instances do not increase those stages' pipeline
        // statistics"); the draw-level loop of a vertex shader reading
        // SV_ViewID counts its input assembly with the first view only.
        st.statistics_skip =
            skip | (first || st.vi_deferred
                        ? 0u
                        : (1u << graphics::STAT_IA_VERTICES) |
                              (1u << graphics::STAT_IA_PRIMITIVES));
        first = false;
        const auto [viewport_offset, slice_offset] = pso->view_locations[v];
        // Viewport (and its scissor) i of the view is the list's viewport
        // i + offset. A sum past the pipeline's 16 viewports is out of
        // range and clamps to 0; one below 16 that RSSetViewports did not
        // set is a disabled viewport (Learn, RSSetViewports: "Any viewports
        // not defined by the call are disabled"), which draws nothing --
        // measured, d3d12_a40_viewinst_probe "one viewport, location vp 1".
        if (!viewports.empty() && viewport_offset != 0) {
            std::vector<Rect> paired = scissors;
            while (!paired.empty() && paired.size() < viewports.size()) {
                paired.push_back(paired.back());
            }
            st.viewports.assign(D3D12_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE,
                                viewports[0]);
            st.scissors.assign(D3D12_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE,
                               paired.empty() ? Rect{} : paired[0]);
            for (u32 i = 0; i < D3D12_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE; ++i) {
                const u64 at = static_cast<u64>(i) + viewport_offset;
                if (at >= D3D12_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE) continue;
                if (at < viewports.size()) {
                    st.viewports[i] = viewports[static_cast<usize>(at)];
                    if (!paired.empty()) st.scissors[i] = paired[static_cast<usize>(at)];
                } else {
                    st.scissors[i] = Rect{};  // disabled: an empty scissor
                }
            }
            if (paired.empty()) st.scissors.clear();
        } else {
            st.viewports = viewports;
            st.scissors = scissors;
        }
        // Render-target array index i of the view is slice i + offset of
        // each bound view, or slice 0 when that is out of the view's range.
        st.targets = targets;
        if (shader_layer) {
            // The shader adds the location and applies the range rule.
            st.layer_addend = slice_offset;
            st.pso = view;
            one();
            continue;
        }
        const auto slice = [&](Descriptor& view_desc) {
            const u32 size = view_desc.array_size == 0xFFFFFFFFu
                                 ? 1u
                                 : std::max(1u, view_desc.array_size);
            if (slice_offset < size) {
                view_desc.first_slice += slice_offset;
                if (view_desc.array_size != 0xFFFFFFFFu) {
                    view_desc.array_size = size - slice_offset;
                }
            }
        };
        for (u32 index = 0; index < st.targets.count && index < 8; ++index) {
            slice(st.targets.rtvs[index]);
        }
        if (st.targets.has_dsv) slice(st.targets.dsv);
        st.pso = view;
        one();
    }
    st.pso = pso;
    st.viewports = viewports;
    st.scissors = scissors;
    st.targets = targets;
    st.statistics_skip = skip;
    st.vi_view = 0;
}

void Replayer::draw_view(ReplayState& st, bool indexed, const cmd::DrawIndexed& d,
                         const graphics::BackendIndirectArgs* indirect) {
    PipelineState* pso = st.pso;
    if (pso == nullptr || pso->compute) {
        d3d12_diag("draw without a graphics pipeline state: skipped");
        return;
    }
    // Stream output and a lowered geometry or tessellation stage both need
    // the draw's counts on the CPU. A bare ExecuteIndirect leaves them in
    // the argument buffer; the same ordered copy and wait the rest of
    // replay uses makes them visible. A buffer that cannot be read stays
    // unimplemented: the counts are not invented.
    cmd::DrawIndexed counts = d;
    if (indirect != nullptr &&
        (pso->geometry.active || pso->tessellation.active)) {
        // GPU-sourced counts through a lowered stage: assembled on the GPU
        // timeline (assemble_on_gpu); nothing is read back. A raster-
        // emulated geometry or domain output runs pass C there too.
        if (!st.roots[0]) {
            d3d12_diag("draw without a graphics root signature: skipped");
            return;
        }
        draw_lowered(st, indexed, d, indirect);
        return;
    }
    // Stream output alone, and raster emulation of the vertex shader's own
    // triangles (pass C is sized by a CPU primitive count), read the counts.
    // An adjacency topology with no geometry or tessellation stage draws
    // through the vertex capture (Frontend::ensure_vertex_replay).
    const bool adjacency = !pso->geometry.active && !pso->tessellation.active &&
                           (st.topology == D3D_PRIMITIVE_TOPOLOGY_LINELIST_ADJ ||
                            st.topology == D3D_PRIMITIVE_TOPOLOGY_LINESTRIP_ADJ ||
                            st.topology == D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST_ADJ ||
                            st.topology == D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP_ADJ);
    if (adjacency && !pso->stream_output.active && !pso->raster.active &&
        f.ensure_vertex_replay(*pso) != S_OK) {
        unimplemented("ExecuteCommandLists",
                      "an adjacency topology with no geometry shader whose vertex shader "
                      "cannot be captured");
        return;
    }
    if (indirect != nullptr &&
        (pso->stream_output.active || pso->raster.active ||
         pso->vertex_cull.active || !pso->per_vertex_msl.empty() || adjacency)) {
        if (indirect != nullptr) {
            const usize bytes = indexed ? 20u : 16u;
            u32 words[5]{};
            const auto readable = [&]() -> bool {
                if (indirect->buffer == nullptr || !indirect->buffer->valid()) {
                    return false;
                }
                const u64 width = indirect->buffer->desc().byte_width;
                const u64 at = indirect->offset;
                if (at > width || static_cast<u64>(bytes) > width - at) {
                    return false;
                }
                graphics::BackendBuffer* staging = nullptr;
                u64 staging_offset = 0;
                u8* host = nullptr;
                if (!reserve(bytes, staging, staging_offset, host)) return false;
                if (!f.m_backend.copy_buffer_region(
                        staging, staging_offset, indirect->buffer, at, bytes)) {
                    return false;
                }
                g_replay_cpu_waits.fetch_add(1, std::memory_order_relaxed);
                graphics::frame_profile::WaitScope profile_wait("replay:indirect-args-read");
                if (auto completion = f.m_backend.flush_completion()) {
                    if (!completion->wait()) return false;
                } else if (!f.m_backend.wait_idle()) {
                    return false;
                }
                std::memcpy(words, host, bytes);
                return true;
            };
            if (!readable()) {
                unimplemented("ExecuteCommandLists",
                              "an indirect draw whose argument buffer cannot "
                              "be read");
                return;
            }
            counts.index_count = words[0];
            counts.instance_count = words[1];
            counts.start_index = words[2];
            if (indexed) {
                counts.base_vertex = static_cast<i32>(words[3]);
                counts.start_instance = words[4];
            } else {
                counts.base_vertex = 0;
                counts.start_instance = words[3];
            }
            indirect = nullptr;
        }
    }
    // Raster emulation or GetAttributeAtVertex over the vertex shader's own
    // primitives: drawn by draw_lowered after any vertex-shader stream
    // output below.
    const bool vertex_lowered =
        !pso->geometry.active && !pso->tessellation.active &&
        (pso->raster.active || !pso->per_vertex_msl.empty() || adjacency ||
         pso->vertex_cull.active);
    if (pso->geometry.active || pso->tessellation.active ||
        (vertex_lowered && !pso->stream_output.active)) {
        if (!st.roots[0]) {
            d3d12_diag("draw without a graphics root signature: skipped");
            return;
        }
        // An indexed geometry or tessellation draw whose index buffer the CPU
        // cannot read (a DEFAULT heap) would copy the indexes out and wait
        // for the GPU, every draw. Its counts are known here, so it takes the
        // GPU-assembled path ExecuteIndirect uses instead (assemble_on_gpu):
        // the same argument block, written to the upload ring, read by the
        // resolve kernel on the GPU timeline. VITREO_D3D12_GS_GPU_ASSEMBLY=0
        // keeps the CPU read (A/B).
        static const bool gpu_assembly = [] {
            const char* v = std::getenv("VITREO_D3D12_GS_GPU_ASSEMBLY");
            return v == nullptr || v[0] != '0';
        }();
        if (gpu_assembly && indirect == nullptr &&
            (pso->geometry.active || pso->tessellation.active) &&
            counts.instance_count != 0 && counts.index_count != 0) {
            // The GPU-assembled path serves every instance in one capture,
            // one expansion and one draw; the CPU path below runs those
            // passes once per instance (3N encoders for a layered volume of
            // N instances, the GS choosing SV_RenderTargetArrayIndex). It is
            // taken when the draw's own counts bound what the resolve can
            // produce, so the passes are sized exactly and nothing is
            // dropped:
            //  - non-indexed: the keys are the draw-relative vertex
            //    (SV_VertexID - StartVertexLocation, ia_resolve.hpp), so
            //    VertexCountPerInstance records and at most as many
            //    primitives an instance;
            //  - indexed, 16-bit: keyed by index value, D3D's at most 0xFFFF,
            //    so 0x10000 records an instance and no more primitives than
            //    indexes. Taken when the buffer is a DEFAULT heap (the CPU
            //    path would copy the indexes out and wait for the GPU) or the
            //    draw is instanced.
            //  - indexed, 32-bit: the largest index is unknown without
            //    reading the buffer, so it keeps the CPU path.
            const bool index32 = indexed && st.ib.format == DXGI_FORMAT_R32_UINT;
            bool eligible = false;
            u64 primitive_bound = u64{counts.index_count} * counts.instance_count;
            u64 record_bound = 0;
            if (!indexed) {
                eligible = counts.instance_count > 1;
                record_bound = u64{counts.index_count} * counts.instance_count;
            } else if (st.ib.va != 0 && !index32) {
                u8 probe = 0;
                const bool cpu_readable = f.read_buffer_va(
                    st.ib.va + static_cast<u64>(counts.start_index) * 2u, &probe, 1);
                eligible = !cpu_readable || counts.instance_count > 1;
                record_bound = u64{0x10000} * counts.instance_count;
            }
            graphics::BackendBuffer* block = nullptr;
            u64 block_offset = 0;
            u8* host = nullptr;
            const usize block_bytes = indexed ? 20u : 16u;
            if (eligible && primitive_bound <= 0xFFFFFFFFull &&
                record_bound <= f.m_lowered_record_capacity &&
                reserve(block_bytes, block, block_offset, host)) {
                // D3D12_DRAW_INDEXED_ARGUMENTS / D3D12_DRAW_ARGUMENTS.
                const u32 indexed_words[5] = {
                    counts.index_count, counts.instance_count, counts.start_index,
                    static_cast<u32>(counts.base_vertex), counts.start_instance};
                const u32 draw_words[4] = {counts.index_count, counts.instance_count,
                                           counts.start_index, counts.start_instance};
                std::memcpy(host, indexed ? static_cast<const void*>(indexed_words)
                                          : static_cast<const void*>(draw_words),
                            block_bytes);
                graphics::BackendIndirectArgs args{};
                args.buffer = block;
                args.offset = block_offset;
                lowered_primitive_bound_ = static_cast<u32>(primitive_bound);
                lowered_record_bound_ = static_cast<u32>(record_bound);
                draw_lowered(st, indexed, counts, &args);
                lowered_primitive_bound_ = 0;
                lowered_record_bound_ = 0;
                return;
            }
        }
        draw_lowered(st, indexed, counts);
        return;
    }
    const auto stream_vertex_output = [&](const cmd::DrawIndexed& draw) -> bool {
        // Vertex-shader stream output is stream 0. The capture draw writes
        // every output register; vitreo_so_prepare / vitreo_so_repack then
        // append whole primitives and advance BufferFilledSize.
        const StreamOutput& so = pso->stream_output;
        u32 vpp = 0;
        bool strip = false;
        switch (st.topology) {
        case D3D_PRIMITIVE_TOPOLOGY_POINTLIST: vpp = 1; break;
        case D3D_PRIMITIVE_TOPOLOGY_LINELIST: vpp = 2; break;
        case D3D_PRIMITIVE_TOPOLOGY_LINESTRIP:
            vpp = 2;
            strip = true;
            break;
        case D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST: vpp = 3; break;
        case D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP:
            vpp = 3;
            strip = true;
            break;
        default:
            unimplemented("ExecuteCommandLists",
                          "stream output of an adjacency or patch topology");
            return false;
        }
        if (draw.instance_count == 0 || draw.index_count == 0) return true;
        std::vector<u32> indices;
        std::vector<u32> assembled;
        graphics::IndexCut cut{};
        if (!indexed) {
            indices.resize(draw.index_count);
            for (u32 i = 0; i < draw.index_count; ++i) {
                indices[i] = draw.start_index + i;
            }
        } else {
            if (st.ib.va == 0) {
                d3d12_diag("indexed stream output with no index buffer");
                return false;
            }
            const bool index32 = st.ib.format == DXGI_FORMAT_R32_UINT;
            const u32 element = index32 ? 4u : 2u;
            const u64 bytes = static_cast<u64>(draw.index_count) * element;
            const u64 start = static_cast<u64>(draw.start_index) * element;
            std::vector<u8> raw(static_cast<usize>(bytes));
            bool got = f.read_buffer_va(st.ib.va + start, raw.data(), raw.size());
            if (!got) {
                graphics::BackendBuffer* source = nullptr;
                u64 source_offset = 0;
                graphics::BackendBuffer* staging = nullptr;
                u64 staging_offset = 0;
                u8* host = nullptr;
                got = f.resolve_va(st.ib.va + start, bytes, source, source_offset) &&
                      bytes <= 0xFFFFFFFFull &&
                      reserve(static_cast<usize>(bytes), staging, staging_offset,
                              host) &&
                      f.m_backend.copy_buffer_region(staging, staging_offset,
                                                     source, source_offset,
                                                     bytes);
                if (got) {
                    g_replay_cpu_waits.fetch_add(1, std::memory_order_relaxed);
                    graphics::frame_profile::WaitScope profile_wait("replay:stream-out-index-read");
                    if (auto completion = f.m_backend.flush_completion()) {
                        got = completion->wait();
                    } else {
                        got = f.m_backend.wait_idle();
                    }
                }
                if (got && host != nullptr) std::memcpy(raw.data(), host, raw.size());
            }
            if (!got) {
                unimplemented("ExecuteCommandLists",
                              "a stream-output draw whose index buffer is not "
                              "readable");
                return false;
            }
            indices.resize(draw.index_count);
            if (index32) {
                std::memcpy(indices.data(), raw.data(), raw.size());
            } else {
                for (u32 i = 0; i < draw.index_count; ++i) {
                    indices[i] = raw[i * 2u] |
                                 (static_cast<u32>(raw[i * 2u + 1u]) << 8);
                }
            }
            cut = strip_cut_of(pso->graphics);
            assembled = graphics::assemble_primitive_vertices(
                indices, cut.active, cut.value, draw.base_vertex, vpp,
                strip ? graphics::PrimitiveAssembly::Strip
                      : graphics::PrimitiveAssembly::List);
        }
        u32 primitives = indexed
                             ? static_cast<u32>(assembled.size() / vpp)
                             : graphics::stream_out_primitive_count(
                                   vpp, strip, draw.index_count);
        u64 max_vertex = 0;
        if (!indexed) {
            max_vertex = static_cast<u64>(draw.start_index) + draw.index_count - 1u;
        } else if (!assembled.empty()) {
            for (const u32 id : assembled) {
                max_vertex = std::max(max_vertex, static_cast<u64>(id));
            }
        }
        const u32 record_vec4s =
            static_cast<u32>(std::max<usize>(so.registers.size(), 1));
        const u64 record_bytes = (max_vertex + 1u) * record_vec4s * 16u;
        constexpr u64 kMaxBytes = 256ull << 20;
        if (record_bytes == 0 ||
            record_bytes * draw.instance_count > kMaxBytes) {
            unimplemented("ExecuteCommandLists",
                          "a stream-output capture that exceeds the size limit");
            return false;
        }
        std::vector<std::unique_ptr<graphics::BackendBuffer>> kept;
        const auto make = [&](u64 bytes, const void* data,
                              bool as_index) -> graphics::BackendBuffer* {
            graphics::BackendBufferDesc desc{};
            desc.byte_width = static_cast<usize>(std::max<u64>(bytes, 16u));
            desc.shader_resource = true;
            desc.unordered_access = true;
            desc.index_buffer = as_index;
            auto created = f.m_backend.create_buffer(desc, data);
            if (!created || !created->valid()) return nullptr;
            kept.push_back(std::move(created));
            return kept.back().get();
        };
        std::vector<u32> capture_indices;
        graphics::BackendBuffer* capture_ib = nullptr;
        graphics::BackendBuffer* assembled_ids = nullptr;
        if (indexed) {
            for (const u32 index : indices) {
                if (!(cut.active && index == cut.value)) {
                    capture_indices.push_back(index);
                }
            }
            if (capture_indices.empty()) return true;
            capture_ib = make(capture_indices.size() * sizeof(u32),
                              capture_indices.data(), true);
            assembled_ids =
                make(std::max<usize>(assembled.size(), 1) * sizeof(u32),
                     assembled.empty() ? nullptr : assembled.data(), false);
            if (capture_ib == nullptr || assembled_ids == nullptr) return false;
        }
        graphics::BackendBuffer* records =
            make(record_bytes * draw.instance_count, nullptr, false);
        graphics::BackendTexture2D* target = f.capture_target();
        const GraphicsState& state = pso->graphics;
        std::array<u32, graphics::BACKEND_MAX_VERTEX_BUFFERS> strides{};
        for (u32 stream = 0; stream < state.stream_count; ++stream) {
            strides[stream] = st.vbs[state.stream_slot[stream]].stride;
        }
        graphics::BackendPipeline* capture = f.capture_variant(*pso, strides);
        if (records == nullptr || target == nullptr || capture == nullptr) {
            unimplemented("ExecuteCommandLists",
                          "the vertex shader cannot be captured for stream "
                          "output");
            return false;
        }
        const auto entries = so_entries(so, 0);
        for (u32 instance = 0; instance < draw.instance_count; ++instance) {
            graphics::BackendDrawCall capture_draw{};
            capture_draw.pipeline = capture;
            capture_draw.topology = graphics::BackendPrimitiveTopology::PointList;
            capture_draw.vertex_capture_buffer = records;
            capture_draw.vertex_capture_buffer_offset = record_bytes * instance;
            if (!bind_vertex_streams(st, capture_draw, true, draw.start_instance,
                                     instance)) {
                return false;
            }
            if (indexed) {
                capture_draw.index_buffer = capture_ib;
                capture_draw.index_32bit = true;
                capture_draw.index_count =
                    static_cast<u32>(capture_indices.size());
                capture_draw.base_vertex = draw.base_vertex;
            } else {
                capture_draw.vertex_count = draw.index_count;
                capture_draw.first_vertex = draw.start_index;
            }
            capture_draw.instance_count = 1;
            capture_draw.first_instance = draw.start_instance + instance;
            std::vector<u32> vertex_push;
            if (uses_lowered_tables(pso->vs)) {
                if (!bind_stage(st, STAGE_VERTEX, pso->vs,
                                capture_draw.vertex_descriptor_tables,
                                vertex_push)) {
                    return false;
                }
            } else if (!bind_flat_constants(
                           st, D3D12_SHADER_VISIBILITY_VERTEX, pso->vs,
                           capture_draw.vertex_constant_buffers,
                           capture_draw.vertex_constant_buffer_offsets,
                           vertex_push)) {
                return false;
            }
            // The capture draws instance `instance` alone (first instance start +
            // instance): SV_InstanceID adds it back (kInstanceIdBiasOffset), and
            // SV_StartInstanceLocation subtracts it from the base instance.
            if (vertex_push.size() * 4 < graphics::kPushBlockBytes) {
                vertex_push.resize(graphics::kPushBlockBytes / 4, 0u);
            }
            vertex_push[graphics::kInstanceIdBiasOffset / 4] = instance;
            capture_draw.vertex_constants = vertex_push.data();
            capture_draw.vertex_constants_size = vertex_push.size() * sizeof(u32);
            graphics::BackendRenderPassDesc pass{};
            pass.color_targets[0] = target;
            pass.preserve_color_contents = true;
            auto render_pass = f.m_backend.create_render_pass_standalone(pass);
            if (!render_pass || !render_pass->valid() ||
                !render_pass->draw(capture_draw) || !render_pass->end()) {
                unimplemented("ExecuteCommandLists",
                              "the stream-output capture was not submitted");
                return false;
            }
            if (!dispatch_stream(
                    f, 0, so.stream_slots[0], so.strides, entries, records,
                    static_cast<u32>(record_bytes * instance), record_vec4s,
                    indexed ? graphics::StreamOutIndexMode::Uint32
                            : graphics::StreamOutIndexMode::None,
                    indexed ? 0u : draw.start_index, 0, indexed ? false : strip,
                    vpp, primitives, nullptr, 0, 0, primitives * vpp,
                    assembled_ids, 0, kept, so_statistics_record(st))) {
                unimplemented("ExecuteCommandLists",
                              "stream output could not be recorded");
                return false;
            }
        }
        return true;
    };
    // The input assembler's primitives reach the clipper unless stream
    // output rasterizes no stream or D3D11.3 15.2 disables rasterization.
    const bool rasterized =
        !(pso->stream_output.active &&
          pso->stream_output.rasterized_stream != 0) &&
        !rasterization_disabled(st);
    if (pso->stream_output.active) {
        if (!st.roots[0]) {
            d3d12_diag("draw without a graphics root signature: skipped");
            return;
        }
        if (!stream_vertex_output(counts)) return;
        // RasterizedStream 0 is the vertex shader's only stream.
        // D3D12_SO_NO_RASTERIZED_STREAM and any other index do not rasterize.
        if (pso->stream_output.rasterized_stream != 0) {
            StatisticsDraw unrasterized{};
            count_draw(st, indexed, counts, nullptr, false, unrasterized);
            return;
        }
        if (vertex_lowered) {
            draw_lowered(st, indexed, counts);
            return;
        }
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
    // A PIPELINE_STATISTICS query open: the variant whose vertex shader
    // captures its outputs and whose pixel shader counts its invocations,
    // both into this interval's buffers.
    const std::shared_ptr<graphics::BackendBuffer> statistics =
        statistics_record(st);
    graphics::BackendPipeline* pipeline =
        f.pipeline_variant(*pso, strides, statistics != nullptr);
    if (pipeline == nullptr) return;

    graphics::BackendDrawCall call{};
    call.pipeline = pipeline;
    call.topology = topology;
    call.vertex_buffer_strides = strides;  // a dynamic-stride pipeline's
    if (!bind_vertex_streams(st, call)) {
        d3d12_diag("draw: the vertex-pull table could not be uploaded");
        return;
    }
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
                : static_cast<u64>(counts.start_index) *
                      (call.index_32bit ? 4u : 2u);
        // StartIndexLocation is folded into the byte offset. The sum is a
        // UINT64 GPU virtual address difference; wrapping it would read the
        // wrong indexes, including index 0.
        u64 combined = 0;
        if (!add_byte_offset(offset, start, combined)) {
            unimplemented("ExecuteCommandLists",
                          "a buffer view whose byte offset does not fit in "
                          "64 bits");
            return;
        }
        call.index_buffer_offset = combined;
        call.index_count = counts.index_count;
        call.base_vertex = counts.base_vertex;
        call.first_vertex = 0;
    } else {
        call.vertex_count = counts.index_count;
        call.first_vertex = counts.start_index;
    }
    call.instance_count = counts.instance_count;
    call.first_instance = counts.start_instance;
    if (indirect != nullptr) {
        call.indirect = *indirect;
    } else if (call.instance_count == 0 ||
               (indexed ? call.index_count == 0 : call.vertex_count == 0)) {
        return;  // an empty draw draws nothing
    }
    std::vector<std::unique_ptr<graphics::BackendBuffer>> cut_kept;
    if (indexed && !apply_strip_cut(st, counts, indirect, call, cut_kept)) {
        return;
    }

    std::vector<u32> vs_push;
    std::vector<u32> ps_push;
    const bool vs_bound =
        uses_lowered_tables(pso->vs)
            ? bind_stage(st, STAGE_VERTEX, pso->vs, call.vertex_descriptor_tables,
                         vs_push)
            : bind_flat_constants(st, D3D12_SHADER_VISIBILITY_VERTEX, pso->vs,
                                  call.vertex_constant_buffers,
                                  call.vertex_constant_buffer_offsets, vs_push);
    const bool ps_bound =
        !pso->ps.present ||
        (uses_lowered_tables(pso->ps)
             ? bind_stage(st, STAGE_PIXEL, pso->ps, call.fragment_descriptor_tables,
                          ps_push)
             : bind_flat_constants(st, D3D12_SHADER_VISIBILITY_PIXEL, pso->ps,
                                   call.fragment_constant_buffers,
                                   call.fragment_constant_buffer_offsets, ps_push));
    if (!vs_bound || !ps_bound) {
        d3d12_diag("draw: bindings could not be resolved: skipped");
        return;
    }
    // A D3D12 stage is compiled as an argument buffer even for one b#
    // register (buffer index 2 holds `constant T* cb0`, not the bytes).
    // The flat arrays name the buffer and its UINT64 byte offset; the
    // table's set_buffer stores gpuAddress + that offset, which is what
    // the shader loads. A raw setFragmentBuffer at the same index is the
    // wrong shape, so those slots stay empty once the table is published.
    const auto publish_flat_cbv =
        [&](u32 stage, const CompiledShader& shader,
            std::array<const graphics::BackendBuffer*,
                       graphics::BACKEND_MAX_CONSTANT_BUFFER_SLOTS>& buffers,
            std::array<u64, graphics::BACKEND_MAX_CONSTANT_BUFFER_SLOTS>&
                offsets,
            std::array<const graphics::BackendDescriptorTable*,
                       graphics::BACKEND_MAX_DESCRIPTOR_SETS>& tables,
            std::vector<u32>& push) -> bool {
        if (shader.table_mode) return true;
        const u32 slots = std::min(shader.registers.cbv_slots,
                                   graphics::BACKEND_MAX_CONSTANT_BUFFER_SLOTS);
        if (slots == 0) return true;
        // table() lives in the bindings file with internal linkage.
        auto& entry = rr.tables[stage][SET_CBV];
        if (!entry || entry->slot_count() < slots) {
            graphics::BackendDescriptorTableDesc desc{};
            desc.slot_count = std::max(slots, entry ? entry->slot_count() : 0u);
            desc.slot_class = graphics::BackendDescriptorClass::Buffer;
            entry = f.m_backend.create_descriptor_table(desc);
        }
        graphics::BackendDescriptorTable* cbv = entry.get();
        if (cbv == nullptr) return false;
        for (u32 slot = 0; slot < slots; ++slot) {
            const graphics::BackendBuffer* buffer = buffers[slot];
            const u64 offset = offsets[slot];
            if (buffer == nullptr) {
                cbv->clear_slot(slot);
                continue;
            }
            if (!cbv->set_buffer(slot, buffer, offset)) {
                // Learn: D3D12_GPU_VIRTUAL_ADDRESS is a UINT64. An offset
                // the argument buffer cannot name is not truncated.
                unimplemented("ExecuteCommandLists",
                              "a buffer view more than 4 GiB into its heap");
                cbv->clear_slot(slot);
                if (slot < graphics::kConstantBufferLimitSlots &&
                    graphics::kConstantBufferLimitOffset / 4 + slot <
                        push.size()) {
                    push[graphics::kConstantBufferLimitOffset / 4 + slot] =
                        graphics::kConstantBufferLimitKnown;
                }
            }
        }
        tables[SET_CBV] = cbv;
        buffers.fill(nullptr);
        offsets.fill(0);
        return true;
    };
    if (!uses_lowered_tables(pso->vs) &&
        !publish_flat_cbv(STAGE_VERTEX, pso->vs, call.vertex_constant_buffers,
                          call.vertex_constant_buffer_offsets,
                          call.vertex_descriptor_tables, vs_push)) {
        d3d12_diag("draw: the vertex constant-buffer table was not bound");
        return;
    }
    if (pso->ps.present && !uses_lowered_tables(pso->ps) &&
        !publish_flat_cbv(STAGE_PIXEL, pso->ps, call.fragment_constant_buffers,
                          call.fragment_constant_buffer_offsets,
                          call.fragment_descriptor_tables, ps_push)) {
        d3d12_diag("draw: the pixel constant-buffer table was not bound");
        return;
    }
    if (vs_push.size() >= graphics::kPushBlockBytes / 4) {
        vs_push[graphics::kLayerStateOffset / 4] = st.layer_limit;
        vs_push[graphics::kLayerStateOffset / 4 + 1] = st.layer_addend;
    }
    call.vertex_constants = vs_push.data();
    call.vertex_constants_size = vs_push.size() * 4;
    call.fragment_constants = ps_push.data();
    call.fragment_constants_size = ps_push.size() * 4;
    StatisticsDraw plan{};
    if (statistics) {
        count_draw(st, indexed, counts, indirect, rasterized, plan);
        if (!f.m_stats_null_capture) {
            graphics::BackendBufferDesc desc{};
            desc.byte_width = graphics::CAPTURE_HEADER_BYTES;
            desc.shader_resource = true;
            desc.unordered_access = true;
            const std::array<u32, 4> zero{};
            f.m_stats_null_capture = f.m_backend.create_buffer(desc, zero.data());
        }
        call.vertex_capture_buffer =
            plan.capture ? plan.capture.get() : f.m_stats_null_capture.get();
        call.vertex_capture_buffer_offset = 0;
        call.fragment_statistics_buffer = statistics.get();
        if (pso->ps.present && rasterized && !pso->statistics_pixels) {
            statistics_unmeasured(st, 1u << graphics::STAT_PS_INVOCATIONS,
                                  "PSInvocations of a pixel shader the "
                                  "counting translation cannot express");
        }
    }
    if (submit_draw(st, call)) finish_statistics_draw(plan);
}

/// View instancing applies to a mesh pipeline as to any other (DirectX-
/// Specs "View Instancing": SV_ViewID is an input of the amplification and
/// mesh shaders): each enabled view dispatches again with its pipeline,
/// whose shaders were translated with that SV_ViewID, and its location.
/// The mesh stage does not apply the render-target array index rule
/// itself, so the views are re-sliced (shader_layer false).
void Replayer::dispatch_mesh(ReplayState& st, const cmd::DispatchMesh& d,
                             const graphics::BackendIndirectArgs* indirect) {
    st.layer_limit = 0;
    st.layer_addend = 0;
    const PipelineState* pso = st.pso;
    if (pso != nullptr && !pso->compute && pso->mesh.active && pso->view_instance_count > 1 &&
        (pso->vi_first == kViMeshRaster || (pso->vi_first == kViMesh && pso->mesh.as.present))) {
        // ViewInstancingTier 3: the amplification shader (and a mesh shader
        // not reading SV_ViewID) runs once.
        if (pso->mesh.ms_layout.device_staging || pso->mesh.ms_layout.raster_capture) {
            // The view-instancing records hold threadgroup-staged outputs;
            // a device-staged or raster-captured mesh stage has neither.
            unimplemented("ExecuteCommandLists",
                          "view instancing (tier 3) of a mesh pipeline staged in "
                          "device memory or drawn through raster emulation");
            return;
        }
        st.vi_deferred = true;
        dispatch_mesh_views(st, d, indirect);
        st.vi_deferred = false;
        st.vi_view = 0;
        return;
    }
    for_each_view(st, false, [&]() { dispatch_mesh_view(st, d, indirect); });
}

void Replayer::dispatch_mesh_view(ReplayState& st, const cmd::DispatchMesh& d,
                                  const graphics::BackendIndirectArgs* indirect) {
    PipelineState* pso = st.pso;
    if (pso == nullptr || pso->compute || !pso->mesh.active ||
        !pso->mesh.pipeline) {
        d3d12_diag("DispatchMesh without a mesh pipeline state: skipped");
        return;
    }
    if (!st.roots[0]) {
        d3d12_diag("DispatchMesh without a graphics root signature: skipped");
        return;
    }
    const bool capture = pso->mesh.ms_layout.raster_capture;
    const bool staged = pso->mesh.ms_layout.device_staging;
    if (indirect != nullptr && (capture || (staged && pso->mesh.as.present))) {
        // Sized by the grid on the CPU (a raster capture's records, an
        // amplified staging scratch of kMeshSlotsPerObjectGroup slots per
        // object group): the counts are read with the ordered copy and wait
        // the vertex route's raster emulation uses.
        u32 words[3]{};
        if (!read_indirect_words(*indirect, words, sizeof(words))) {
            unimplemented("ExecuteCommandLists",
                          "an indirect DispatchMesh whose argument buffer cannot be read");
            return;
        }
        // DirectX-Specs "Mesh Shader": each count below 64k, product at
        // most 2^22.
        const u64 product = u64{words[0]} * words[1] * words[2];
        if (words[0] >= 65536u || words[1] >= 65536u || words[2] >= 65536u ||
            product > (1ull << 22)) {
            d3d12_diag("indirect DispatchMesh past the ThreadGroupCount limits: skipped");
            return;
        }
        dispatch_mesh_view(st, cmd::DispatchMesh{words[0], words[1], words[2]}, nullptr);
        return;
    }
    // A zero ThreadGroupCount launches nothing.
    if (indirect == nullptr && (d.x == 0 || d.y == 0 || d.z == 0)) return;
    graphics::BackendDrawCall call{};
    graphics::BackendMeshDraw mesh{};
    call.mesh = &mesh;
    call.pipeline = pso->mesh.pipeline.get();
    const std::shared_ptr<graphics::BackendBuffer> mesh_record =
        mesh_statistics(st, call.pipeline);
    mesh.statistics = mesh_record.get();
    std::shared_ptr<graphics::BackendBuffer> pixel_record;
    // A raster capture draws no pixel: pass C counts PSInvocations.
    if (mesh_record && pso->ps.present && !capture) {
        pixel_record = statistics_record(st);
        call.fragment_statistics_buffer = pixel_record.get();
    }
    const graphics::DxbcMeshState& ms = pso->mesh.ms_layout.state;
    const graphics::DxbcMeshState& as = pso->mesh.as_layout.state;
    for (u32 k = 0; k < 3; ++k) {
        mesh.mesh_threads[k] = ms.threads[k];
        if (pso->mesh.as.present) mesh.object_threads[k] = as.threads[k];
    }
    if (indirect != nullptr) {
        mesh.indirect = indirect->buffer;
        mesh.indirect_offset = indirect->offset;
    } else {
        mesh.grid = {d.x, d.y, d.z};
    }
    // Device staging (MeshStageLayout::device_staging): every threadgroup
    // the grid launches has a slot of the scratch (an amplified one, a
    // slot among its object group's kMeshSlotsPerObjectGroup). A grid
    // whose slots exceed kScratchBytes draws in chunks, boxes of the grid
    // whose origin is added to SV_GroupID, each ordered after the last.
    std::unique_ptr<graphics::BackendBuffer> scratch;
    std::vector<std::pair<std::array<u32, 3>, std::array<u32, 3>>> chunks;  // origin, size
    if (staged && indirect == nullptr) {
        constexpr u64 kScratchBytes = 64ull << 20;
        const u64 slot_bytes = u64{pso->mesh.ms_layout.device_staging_words()} * 4u;
        const u64 unit = slot_bytes * (pso->mesh.as.present ? graphics::kMeshSlotsPerObjectGroup : 1u);
        const u64 per_chunk = std::max<u64>(1, kScratchBytes / unit);
        const u32 g[3] = {d.x, d.y, d.z};
        std::array<u32, 3> box{d.x, d.y, d.z};
        // Whole z-slabs, else rows of one slab, else runs of one row.
        if (u64{d.x} * d.y > per_chunk) {
            box[2] = 1;
            if (d.x > per_chunk) {
                box[1] = 1;
                box[0] = static_cast<u32>(per_chunk);
            } else {
                box[1] = static_cast<u32>(per_chunk / d.x);
            }
        } else {
            box[2] = static_cast<u32>(std::min<u64>(d.z, per_chunk / (u64{d.x} * d.y)));
        }
        for (u32 z = 0; z < g[2]; z += box[2]) {
            for (u32 y = 0; y < g[1]; y += box[1]) {
                for (u32 x = 0; x < g[0]; x += box[0]) {
                    chunks.push_back({{x, y, z},
                                      {std::min(box[0], g[0] - x), std::min(box[1], g[1] - y),
                                       std::min(box[2], g[2] - z)}});
                }
            }
        }
        graphics::BackendBufferDesc desc{};
        // Behind an amplification shader, one slot counter per chunk past
        // the slots (mesh_stage_device_staging), zeroed in order with the
        // draws; group_origin.w carries 2 + the first one's word.
        const u64 slots_bytes = unit * box[0] * box[1] * box[2];
        const u64 counter_bytes =
            pso->mesh.as.present ? (u64{chunks.size()} * 4u + 15u) / 16u * 16u : 0u;
        desc.byte_width = static_cast<usize>(slots_bytes + counter_bytes);
        desc.shader_resource = true;
        desc.unordered_access = true;
        scratch = f.m_backend.create_buffer(desc, nullptr);
        if (!scratch || !scratch->valid() ||
            (counter_bytes != 0 &&
             !f.m_backend.fill_buffer_region(scratch.get(), slots_bytes, counter_bytes, 0))) {
            d3d12_diag("DispatchMesh: no mesh staging scratch of %llu bytes: skipped",
                       static_cast<unsigned long long>(desc.byte_width));
            return;
        }
        mesh.scratch = scratch.get();
        if (counter_bytes != 0) mesh.group_origin[3] = static_cast<u32>(2u + slots_bytes / 4u);
    }
    std::vector<u32> ms_push;
    std::vector<u32> as_push;
    std::vector<u32> ps_push;
    const bool bound =
        bind_stage(st, STAGE_MESH, pso->mesh.ms, call.vertex_descriptor_tables,
                   ms_push) &&
        (!pso->mesh.as.present ||
         bind_stage(st, STAGE_AMPLIFICATION, pso->mesh.as,
                    mesh.object_descriptor_tables, as_push)) &&
        (!pso->ps.present ||
         bind_stage(st, STAGE_PIXEL, pso->ps, call.fragment_descriptor_tables,
                    ps_push));
    if (!bound) {
        d3d12_diag("DispatchMesh: bindings could not be resolved: skipped");
        return;
    }
    call.vertex_constants = ms_push.data();
    call.vertex_constants_size = ms_push.size() * 4;
    mesh.object_constants = as_push.data();
    mesh.object_constants_size = as_push.size() * 4;
    call.fragment_constants = ps_push.data();
    call.fragment_constants_size = ps_push.size() * 4;
    if (capture) {
        (void)submit_mesh_raster(st, call, chunks);
        return;
    }
    if (staged && indirect != nullptr) {
        (void)submit_mesh_indirect_chunks(st, call, *indirect);
        return;
    }
    if (chunks.empty()) {
        (void)submit_draw(st, call);
        return;
    }
    for (usize k = 0; k < chunks.size(); ++k) {
        // Each chunk its own out-of-line mesh dispatch (the backend reads
        // it during the draw only).
        graphics::BackendDrawCall part = call;
        graphics::BackendMeshDraw part_mesh = mesh;
        part.mesh = &part_mesh;
        part_mesh.grid = {chunks[k].second[0], chunks[k].second[1], chunks[k].second[2]};
        part_mesh.group_origin = {chunks[k].first[0], chunks[k].first[1], chunks[k].first[2],
                                  mesh.group_origin[3] != 0
                                      ? mesh.group_origin[3] + static_cast<u32>(k)
                                      : 0u};
        part_mesh.serialize = k != 0;
        if (!submit_draw(st, part)) return;
    }
}

bool Replayer::apply_strip_cut(
    ReplayState& st, const cmd::DrawIndexed& counts,
    const graphics::BackendIndirectArgs* indirect,
    graphics::BackendDrawCall& call,
    std::vector<std::unique_ptr<graphics::BackendBuffer>>& kept) {
    const u32 topology = st.topology;
    const bool strip = topology == D3D_PRIMITIVE_TOPOLOGY_LINESTRIP ||
                       topology == D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
    const u32 vpp = topology_vertices(topology);
    const bool index32 = call.index_32bit;
    const graphics::IndexCut cut = strip_cut_of(st.pso->graphics);
    const graphics::IaCutPlan plan = graphics::ia_cut_plan(strip, index32, cut);
    if (plan.rewrite == graphics::IaCutRewrite::Native || vpp == 0 || vpp > 3) {
        return true;
    }
    graphics::frame_profile::note_internal("strip-cut");
    const u32 element = index32 ? 4u : 2u;
    const auto list_topology = [&]() {
        using T = graphics::BackendPrimitiveTopology;
        return vpp == 1 ? T::PointList : vpp == 2 ? T::LineList : T::TriangleList;
    };
    const auto make = [&](usize bytes, const void* data, bool as_index)
        -> graphics::BackendBuffer* {
        graphics::BackendBufferDesc desc{};
        desc.byte_width = std::max<usize>(bytes, 16u);
        desc.shader_resource = true;
        desc.unordered_access = true;
        desc.index_buffer = as_index;
        auto created = f.m_backend.create_buffer(desc, data);
        if (!created || !created->valid()) return nullptr;
        kept.push_back(std::move(created));
        return kept.back().get();
    };
    // An UPLOAD heap holds what the GPU will read: checked and rewritten on
    // the CPU, so a draw without the values is drawn as it is.
    if (indirect == nullptr) {
        const u64 start = static_cast<u64>(counts.start_index) * element;
        std::vector<u8> raw(static_cast<usize>(counts.index_count) * element);
        if ((st.ib.size == 0 || start + raw.size() <= st.ib.size) &&
            f.read_upload_va(st.ib.va + start, raw.data(), raw.size())) {
            std::vector<u32> indices(counts.index_count);
            for (u32 i = 0; i < counts.index_count; ++i) {
                u32 v = 0;
                std::memcpy(&v, raw.data() + static_cast<usize>(i) * element,
                            element);
                indices[i] = v;
            }
            if (!graphics::ia_cut_needed(plan, indices)) return true;
            const std::vector<u32> stream =
                graphics::ia_cut_rewrite_indices(plan, cut, strip, vpp, indices);
            if (stream.empty()) return false;  // no complete primitive
            graphics::BackendBuffer* out =
                make(stream.size() * sizeof(u32), stream.data(), true);
            if (out == nullptr) {
                d3d12_diag("strip cut: the index stream could not be allocated");
                return false;
            }
            call.index_buffer = out;
            call.index_32bit = true;
            call.index_buffer_offset = 0;
            call.index_count = static_cast<u32>(stream.size());
            if (plan.rewrite != graphics::IaCutRewrite::Widen) {
                call.topology = list_topology();
            }
            return true;
        }
    }
    // Otherwise on the GPU timeline (vitreo_ia_cut_rewrite), drawn through
    // the argument block it writes.
    graphics::BackendComputePipeline* kernel = f.ia_cut_kernel();
    const u64 view_elements = st.ib.size / element;
    // A direct draw's StartIndexLocation is already in the offset.
    const u64 folded = indirect == nullptr ? counts.start_index : 0u;
    const u64 elements = view_elements > folded ? view_elements - folded : 0u;
    const u64 capacity = graphics::ia_cut_capacity(
        plan.rewrite, vpp, indirect == nullptr ? counts.index_count : elements);
    constexpr u64 kMaxIndices = u64{1} << 26;  // 256 MiB of 32-bit indices
    if (kernel == nullptr || capacity > kMaxIndices) {
        unimplemented("ExecuteCommandLists",
                      kernel == nullptr
                          ? "an IBStripCutValue Metal's restart cannot express "
                            "(the rewrite kernel could not be built)"
                          : "an IBStripCutValue rewrite of more than 2^26 "
                            "indices");
        return false;
    }
    const graphics::BackendBuffer* args = nullptr;
    u64 args_offset = 0;
    if (indirect != nullptr) {
        args = indirect->buffer;
        args_offset = indirect->offset;
    } else {
        const std::array<u32, 5> words{
            counts.index_count, counts.instance_count, 0u,
            static_cast<u32>(counts.base_vertex), counts.start_instance};
        args = make(sizeof(words), words.data(), false);
    }
    graphics::BackendBuffer* out =
        make(static_cast<usize>(std::max<u64>(capacity, 1u)) * sizeof(u32),
             nullptr, true);
    graphics::BackendBuffer* draw_args = make(32, nullptr, false);
    if (args == nullptr || out == nullptr || draw_args == nullptr) {
        d3d12_diag("strip cut: the rewrite buffers could not be allocated");
        return false;
    }
    graphics::IaCutParams params{};
    params.index_32bit = index32 ? 1u : 0u;
    // The kernel reads 32-bit words: bind the word holding the first index.
    const u64 binding = call.index_buffer_offset & ~u64{3};
    params.index_first =
        static_cast<u32>((call.index_buffer_offset - binding) / element);
    params.index_elements =
        static_cast<u32>(std::min<u64>(elements, 0xFFFFFFFFu));
    params.vertices_per_primitive = vpp;
    params.strip = strip ? 1u : 0u;
    params.rewrite = static_cast<u32>(plan.rewrite);
    params.cut_active = cut.active ? 1u : 0u;
    params.cut_value = cut.value;
    params.capacity = static_cast<u32>(capacity);
    graphics::BackendDispatchCall rewrite{};
    rewrite.pipeline = kernel;
    rewrite.group_count_x = rewrite.group_count_y = rewrite.group_count_z = 1;
    rewrite.threads_per_group_x = graphics::IA_CUT_THREADS;
    rewrite.threads_per_group_y = rewrite.threads_per_group_z = 1;
    rewrite.constants = &params;
    rewrite.constants_size = sizeof(params);
    rewrite.buffers[graphics::IA_CUT_ARGS_INDEX] = args;
    rewrite.buffer_offsets[graphics::IA_CUT_ARGS_INDEX] = args_offset;
    rewrite.buffers[graphics::IA_CUT_INDICES_INDEX] = call.index_buffer;
    rewrite.buffer_offsets[graphics::IA_CUT_INDICES_INDEX] = binding;
    rewrite.buffers[graphics::IA_CUT_OUT_INDEX] = out;
    rewrite.buffers[graphics::IA_CUT_DRAW_INDEX] = draw_args;
    if (!f.m_backend.dispatch(rewrite)) {
        unimplemented("ExecuteCommandLists",
                      "an IBStripCutValue rewrite the backend refused");
        return false;
    }
    call.index_buffer = out;
    call.index_32bit = true;
    call.index_buffer_offset = 0;
    call.indirect.buffer = draw_args;
    call.indirect.offset = 0;
    if (plan.rewrite != graphics::IaCutRewrite::Widen) {
        call.topology = list_topology();
    }
    return true;
}

bool Replayer::submit_draw(ReplayState& st, graphics::BackendDrawCall& call) {

    const GraphicsState& gs = st.pso->graphics;
    // Rasterizer.
    std::vector<std::array<float, 6>> viewports;
    std::vector<std::array<int, 4>> scissors;
    if (!st.viewports.empty()) {
        const Viewport& v = st.viewports.front();
        call.viewport = {v.x, v.y, v.width, v.height, v.min_depth, v.max_depth};
    }
    call.scissor_enabled = !st.scissors.empty();
    if (!st.scissors.empty()) {
        const Rect& r = st.scissors.front();
        call.scissor = {r.left, r.top, r.right, r.bottom};
    }
    // The inline fields cover the usual single viewport. Build the arrays
    // only for multiple viewports, avoiding two heap allocations per draw.
    if (st.viewports.size() > 1) {
        viewports.reserve(st.viewports.size());
        scissors.reserve(std::max(st.scissors.size(), st.viewports.size()));
        for (const Viewport& v : st.viewports) {
            viewports.push_back(
                {v.x, v.y, v.width, v.height, v.min_depth, v.max_depth});
        }
        for (const Rect& r : st.scissors) {
            scissors.push_back({r.left, r.top, r.right, r.bottom});
        }
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
    // Pass C of a raster-emulated line or point draws its region as
    // triangles: "cull modes do not apply to points" (D3D11.3 3.4.6) nor to
    // lines, and "line and point primitives reaching the rasterizer are not
    // affected at all" by the fill mode (3.5.6.4).
    if (st.pso->raster.active &&
        st.pso->raster.primitive != graphics::RasterPrimitive::Triangle &&
        call.topology == graphics::BackendPrimitiveTopology::TriangleList) {
        call.cull_mode = graphics::BackendCullMode::None;
        call.fill_mode = graphics::BackendFillMode::Fill;
    }
    call.front_counter_clockwise = gs.front_ccw;
    // A raster-emulated pass C clips 0 <= z/w <= 1 in its pixel shader
    // (raster_emulation.hpp): Metal must not cut its grown regions.
    call.depth_clip_enabled = gs.depth_clip && !st.pso->raster.active;
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
        // A raster-emulated pass C rasterizes one sample; its pixel shader
        // tests the pattern (raster_emulation.hpp).
        pass.attachmentless_samples =
            st.pso->raster.active ? 1u : std::max(1u, gs.sample_count);
    }
    (void)width;
    (void)height;
    std::array<const graphics::BackendBuffer*,
               graphics::BACKEND_MAX_UAV_COUNTER_BINDINGS>
        counters{};
    std::array<u64, graphics::BACKEND_MAX_UAV_COUNTER_BINDINGS>
        counter_offsets{};
    if (!bind_uav_counters(f, st, 0, D3D12_SHADER_VISIBILITY_PIXEL, false,
                           st.pso->ps, counters, counter_offsets)) {
        d3d12_diag("draw: a UAV counter could not be bound: skipped");
        return false;
    }
    call.fragment_uav_counters = counters;
    call.fragment_uav_counter_offsets = counter_offsets;
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
        return false;
    }
    const bool drew = render_pass->draw(call);
    const bool ended = render_pass->end();
    if (!drew || !ended) d3d12_diag("draw: the backend refused the draw");
    return drew && ended;
}

bool Replayer::bind_flat_constants(
    ReplayState& st, u32 visibility, const CompiledShader& shader,
    std::array<const graphics::BackendBuffer*,
               graphics::BACKEND_MAX_CONSTANT_BUFFER_SLOTS>& buffers,
    std::array<u64, graphics::BACKEND_MAX_CONSTANT_BUFFER_SLOTS>& offsets,
    std::vector<u32>& push) {
    buffers.fill(nullptr);
    offsets.fill(0);
    push.assign(graphics::kPushBlockBytes / 4, 0u);
    const u32 slots = std::min(shader.registers.cbv_slots,
                               graphics::BACKEND_MAX_CONSTANT_BUFFER_SLOTS);
    for (u32 slot = 0; slot < slots && slot < graphics::kConstantBufferLimitSlots;
         ++slot) {
        push[graphics::kConstantBufferLimitOffset / 4 + slot] =
            graphics::kConstantBufferLimitKnown;
    }
    if (slots == 0) return true;
    const RootLayout* root = st.roots[0].get();
    if (root == nullptr) return false;
    const auto& args = st.args[0];
    const auto visible = [&](u32 vis) {
        return vis == D3D12_SHADER_VISIBILITY_ALL || vis == visibility;
    };
    const auto note = [&](u32 slot, graphics::BackendBuffer* buffer,
                          u64 offset, u32 vectors) {
        if (slot >= slots || buffer == nullptr) return;
        buffers[slot] = buffer;
        offsets[slot] = offset;
        if (slot < graphics::kConstantBufferLimitSlots) {
            push[graphics::kConstantBufferLimitOffset / 4 + slot] =
                vectors | graphics::kConstantBufferLimitKnown;
        }
    };
    for (u32 index = 0; index < root->parameters.size(); ++index) {
        const BindingParameter& parameter = root->parameters[index];
        if (!visible(parameter.visibility)) continue;
        const ReplayState::RootArg* arg =
            index < args.size() ? &args[index] : nullptr;
        switch (parameter.type) {
        case D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS: {
            if (parameter.space != 0 || parameter.shader_register >= slots ||
                arg == nullptr) {
                break;
            }
            const auto& values = arg->constants;
            graphics::BackendBuffer* buffer = nullptr;
            u64 offset = 0;
            const u32 zero = 0;
            if (!upload(values.empty() ? static_cast<const void*>(&zero)
                                       : values.data(),
                        values.empty() ? 4u : values.size() * 4u, buffer,
                        offset)) {
                return false;
            }
            // The upload's byte offset is a UINT64. It is stored whole; a
            // backend that cannot name it leaves the slot empty.
            note(parameter.shader_register, buffer, offset,
                 values.empty() ? 0u
                                : static_cast<u32>((values.size() + 3u) / 4u));
            break;
        }
        case D3D12_ROOT_PARAMETER_TYPE_CBV: {
            if (parameter.space != 0 || parameter.shader_register >= slots ||
                arg == nullptr || !arg->set) {
                break;
            }
            graphics::BackendBuffer* buffer = nullptr;
            u64 offset = 0;
            // BufferLocation is a D3D12_GPU_VIRTUAL_ADDRESS (UINT64). The
            // byte offset into the buffer is stored whole.
            if (!f.resolve_va(arg->va, 0, buffer, offset)) {
                break;
            }
            note(parameter.shader_register, buffer, offset,
                 f.buffer_words_from_va(arg->va) / 4u);
            break;
        }
        case D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE: {
            if (arg == nullptr || !arg->set) break;
            u32 base = 0;
            DescriptorHeap* heap = f.gpu_descriptor(arg->table, base);
            if (heap == nullptr) break;
            for (const BindingRange& range : parameter.ranges) {
                if (range.type != D3D12_DESCRIPTOR_RANGE_TYPE_CBV ||
                    range.space != 0) {
                    continue;
                }
                const u64 available =
                    heap->records.size() > static_cast<u64>(base) + range.table_offset
                        ? heap->records.size() - base - range.table_offset
                        : 0u;
                const u64 count = range.count == 0xFFFFFFFFu
                                      ? available
                                      : std::min<u64>(range.count, available);
                for (u32 reg = range.base_register;
                     reg < slots && reg - range.base_register < count; ++reg) {
                    const Descriptor& descriptor =
                        heap->records[static_cast<u64>(base) + range.table_offset +
                                      (reg - range.base_register)];
                    graphics::BackendBuffer* buffer = nullptr;
                    u64 offset = 0;
                    if (descriptor.type != DescriptorType::Cbv ||
                        !f.resolve_va(descriptor.first_element,
                                      descriptor.num_elements, buffer, offset)) {
                        continue;
                    }
                    note(reg, buffer, offset, descriptor.num_elements / 16u);
                }
            }
            break;
        }
        default:
            break;
        }
    }
    if (!rr.zeros) {
        graphics::BackendHeapDesc desc{};
        desc.size = 64u * 1024u;
        desc.cpu_visible = true;
        desc.buffers_only = true;
        rr.zeros = f.m_backend.create_heap(desc);
    }
    if (rr.zeros && rr.zeros->buffer() != nullptr) {
        for (u32 slot = 0; slot < slots; ++slot) {
            if (buffers[slot] == nullptr) buffers[slot] = rr.zeros->buffer();
        }
    }
    return true;
}

bool Replayer::assemble_on_gpu(
    ReplayState& st, bool indexed, const graphics::BackendIndirectArgs& indirect,
    u32 group, graphics::PrimitiveAssembly assembly, bool tess,
    u32 record_vec4s,
    const std::array<u32, graphics::BACKEND_MAX_VERTEX_BUFFERS>& strides,
    std::vector<std::unique_ptr<graphics::BackendBuffer>>& kept,
    u32& primitives, graphics::BackendBuffer*& records,
    graphics::BackendBuffer*& ids,
    std::shared_ptr<graphics::BackendBuffer>& resolved) {
    constexpr u64 kMaxBytes = 256ull << 20;
    PipelineState* pso = st.pso;
    f.poll_lowered_feedback();
    // What one primitive needs in the passes after the capture bounds how
    // many the draw is sized for (the CPU path's size limit, per pass).
    u64 per_primitive = 16;
    if (tess) {
        const auto& program = pso->tessellation;
        const graphics::tess::TessPatchBound bound =
            graphics::tess::tess_patch_bound(
                program.tess_domain, program.partitioning,
                program.output_primitive, program.max_tess_factor);
        const u64 ds_vec4s =
            std::max<usize>(program.domain_records.size(), 1);
        per_primitive = std::max<u64>(
            {u64{program.output_control_points} * program.cp_record_vec4s * 16u,
             u64{program.pc_record_vec4s} * 16u,
             u64{std::max(bound.points, 1u)} * ds_vec4s * 16u,
             (u64{std::max(bound.indices, 1u)} * 2u + 1u) * 4u});
    } else {
        const auto& program = pso->geometry;
        u32 streams = 0;
        for (u32 stream = 0; stream < 4; ++stream) {
            if ((program.stream_mask & (1u << stream)) != 0) streams = stream + 1;
        }
        per_primitive = u64{std::max(program.instance_count, 1u)} *
                        std::max(program.vertices_per_invocation, 1u) *
                        std::max(streams, 1u) *
                        std::max(program.output_record_vec4s, 1u) * 16u;
    }
    const u64 record_size = u64{record_vec4s} * 16u;
    const u32 record_capacity = static_cast<u32>(std::min<u64>(
        f.m_lowered_record_capacity,
        (kMaxBytes - graphics::CAPTURE_HEADER_BYTES) / record_size));
    u32 primitive_capacity = static_cast<u32>(std::min<u64>(
        {u64{record_capacity}, kMaxBytes / (4u * u64{group}),
         kMaxBytes / std::max<u64>(per_primitive, 1u)}));
    u32 records_sized = record_capacity;
    if (lowered_primitive_bound_ != 0 &&
        lowered_primitive_bound_ <= primitive_capacity &&
        lowered_record_bound_ != 0 && lowered_record_bound_ <= record_capacity) {
        // A direct draw: its counts bound what it can produce.
        primitive_capacity = lowered_primitive_bound_;
        records_sized = lowered_record_bound_;
    }
    graphics::BackendComputePipeline* kernel = f.ia_resolve_kernel();
    graphics::BackendTexture2D* target = f.capture_target();
    graphics::BackendPipeline* capture = f.capture_variant(*pso, strides, true);
    if (record_capacity == 0 || primitive_capacity == 0 || kernel == nullptr ||
        target == nullptr || capture == nullptr) {
        unimplemented("ExecuteCommandLists",
                      "an indirect geometry / tessellation draw whose GPU "
                      "assembly cannot be recorded");
        return false;
    }
    graphics::BackendBufferDesc desc{};
    desc.shader_resource = true;
    desc.unordered_access = true;
    desc.byte_width = graphics::IA_RESOLVED_WORDS * sizeof(u32);
    resolved = f.m_backend.create_buffer(desc, nullptr);
    desc.byte_width = static_cast<usize>(primitive_capacity) * group * 4u;
    auto id_storage = f.m_backend.create_buffer(desc, nullptr);
    desc.byte_width = static_cast<usize>(graphics::CAPTURE_HEADER_BYTES +
                                         u64{records_sized} * record_size);
    auto record_storage = f.m_backend.create_buffer(desc, nullptr);
    if (!resolved || !id_storage || !record_storage) {
        d3d12_diag("indirect geometry / tessellation draw: the resolve "
                   "buffers could not be allocated");
        return false;
    }
    graphics::IaResolveParams params{};
    params.indexed = indexed ? 1u : 0u;
    params.vertices_per_primitive = group;
    params.assembly = assembly == graphics::PrimitiveAssembly::StripAdjacency ? 2u
                      : assembly == graphics::PrimitiveAssembly::Strip      ? 1u
                                                                            : 0u;
    params.record_capacity = records_sized;
    params.primitive_capacity = primitive_capacity;
    graphics::BackendBuffer* index_buffer = nullptr;
    u64 index_offset = 0;
    u64 index_binding = 0;
    const bool index32 = st.ib.format == DXGI_FORMAT_R32_UINT;
    if (indexed) {
        if (st.ib.va == 0 ||
            !f.resolve_va(st.ib.va, 0, index_buffer, index_offset)) {
            d3d12_diag("indexed draw with no index buffer: skipped");
            return false;
        }
        const u32 index_size = index32 ? 4u : 2u;
        // The kernel reads 32-bit words: bind the word holding the view's
        // first index, and start that many indices into it.
        index_binding = index_offset & ~u64{3};
        params.index_32bit = index32 ? 1u : 0u;
        params.index_first =
            static_cast<u32>((index_offset - index_binding) / index_size);
        params.index_elements = st.ib.size / index_size;
        params.cut_mode = graphics::ia_cut_mode(strip_cut_of(pso->graphics));
    }
    graphics::BackendDispatchCall resolve{};
    resolve.pipeline = kernel;
    resolve.group_count_x = resolve.group_count_y = resolve.group_count_z = 1;
    resolve.threads_per_group_x = graphics::IA_RESOLVE_THREADS;
    resolve.threads_per_group_y = resolve.threads_per_group_z = 1;
    resolve.constants = &params;
    resolve.constants_size = sizeof(params);
    resolve.buffers[graphics::IA_RESOLVE_ARGS_INDEX] = indirect.buffer;
    resolve.buffer_offsets[graphics::IA_RESOLVE_ARGS_INDEX] = indirect.offset;
    resolve.buffers[graphics::IA_RESOLVE_INDICES_INDEX] =
        indexed ? index_buffer : resolved.get();
    resolve.buffer_offsets[graphics::IA_RESOLVE_INDICES_INDEX] = index_binding;
    resolve.buffers[graphics::IA_RESOLVE_IDS_INDEX] = id_storage.get();
    resolve.buffers[graphics::IA_RESOLVE_OUT_INDEX] = resolved.get();
    resolve.buffers[graphics::IA_RESOLVE_CAPTURE_INDEX] = record_storage.get();
    if (!f.m_backend.dispatch(resolve)) {
        d3d12_diag("indirect geometry / tessellation draw: the resolve was "
                   "refused");
        return false;
    }
    f.m_lowered_feedback.push_back(
        {resolved, f.m_backend.recording_generation()});

    // PIPELINE_STATISTICS: the input assembly of the same argument block,
    // counted on the GPU (vitreo_ia_resolve + vitreo_stats_vertices).
    if (pipeline_statistics_open(st)) {
        StatisticsDraw plan{};
        // HSInvocations too: vitreo_stats_vertices adds the resolved patch
        // count (count_draw, STATS_FLAG_ADD_HS_INVOCATIONS).
        count_draw(st, indexed, cmd::DrawIndexed{}, &indirect, false, plan);
        finish_statistics_draw(plan);
    }

    // The capture: every instance in one draw, keyed draw-relative, with
    // the argument block the resolve copied (instances 0 when dropped).
    graphics::BackendDrawCall call{};
    call.pipeline = capture;
    call.topology = graphics::BackendPrimitiveTopology::PointList;
    call.vertex_capture_buffer = record_storage.get();
    call.vertex_capture_buffer_offset = 0;
    if (!bind_vertex_streams(st, call)) return false;
    if (indexed) {
        call.index_buffer = index_buffer;
        call.index_32bit = index32;
        call.index_buffer_offset = index_offset;
    }
    call.instance_count = 1;
    call.indirect.buffer = resolved.get();
    call.indirect.offset =
        graphics::IA_RESOLVED_CAPTURE_ARGS * static_cast<unsigned>(sizeof(u32));
    std::vector<u32> vertex_push;
    if (uses_lowered_tables(pso->vs)) {
        if (!bind_stage(st, STAGE_VERTEX, pso->vs, call.vertex_descriptor_tables,
                        vertex_push)) {
            return false;
        }
    } else if (!bind_flat_constants(st, D3D12_SHADER_VISIBILITY_VERTEX, pso->vs,
                                    call.vertex_constant_buffers,
                                    call.vertex_constant_buffer_offsets,
                                    vertex_push)) {
        return false;
    }
    call.vertex_constants = vertex_push.data();
    call.vertex_constants_size = vertex_push.size() * sizeof(u32);
    graphics::BackendRenderPassDesc pass{};
    pass.color_targets[0] = target;
    pass.preserve_color_contents = true;
    auto render_pass = f.m_backend.create_render_pass_standalone(pass);
    if (!render_pass || !render_pass->valid() || !render_pass->draw(call) ||
        !render_pass->end()) {
        d3d12_diag("indirect geometry / tessellation draw: the capture was "
                   "not submitted");
        return false;
    }
    primitives = primitive_capacity;
    ids = id_storage.get();
    records = record_storage.get();
    kept.push_back(std::move(id_storage));
    kept.push_back(std::move(record_storage));
    return true;
}

void Replayer::draw_lowered(ReplayState& st, bool indexed,
                            const cmd::DrawIndexed& d,
                            const graphics::BackendIndirectArgs* indirect) {
    if (graphics::frame_profile::enabled()) [[unlikely]] {
        const PipelineState* lowered = st.pso;
        graphics::frame_profile::note_internal(
            lowered == nullptr                    ? "lowered:none"
            : lowered->tessellation.active        ? "lowered:tessellation"
            : lowered->geometry.active            ? "lowered:geometry"
            : lowered->raster.active              ? "lowered:raster"
            : lowered->vertex_cull.active         ? "lowered:vertex-cull"
            : !lowered->per_vertex_msl.empty()    ? "lowered:per-vertex"
                                                  : "lowered:adjacency");
    }
    PipelineState* pso = st.pso;
    if (indirect == nullptr && (d.instance_count == 0 || d.index_count == 0)) {
        return;
    }
    // ViewInstancingTier 3 (Replayer::draw, st.vi_deferred): @p body runs
    // once per view when the pipeline's views start at @p stage
    // (PipelineState::vi_first), with `pso` the view's pipeline and the
    // view's location applied; once otherwise. Each view's work is that of
    // its own pipeline (translated with its SV_ViewID); the stages before
    // ran once with view 0's, as "the implementation of view instancing
    // always occurs at the first shader stage that references SV_ViewID".
    PipelineState* const base = pso;
    const auto at_stage = [&](u32 stage, const auto& body) -> bool {
        if (!st.vi_deferred || base->vi_first != stage) return body();
        bool ok = true;
        st.pso = base;
        for_each_view(st, st.vi_shader_layer, [&]() {
            pso = st.pso;
            if (ok && !body()) ok = false;
        });
        pso = base;
        st.pso = base;
        return ok;
    };
    constexpr u64 kMaxBytes = 256ull << 20;
    const GraphicsState& gs = pso->graphics;
    std::array<u32, graphics::BACKEND_MAX_VERTEX_BUFFERS> strides{};
    for (u32 stream = 0; stream < gs.stream_count; ++stream) {
        strides[stream] = st.vbs[gs.stream_slot[stream]].stride;
    }

    const bool tess = pso->tessellation.active;
    u32 group = 0;
    graphics::PrimitiveAssembly assembly = graphics::PrimitiveAssembly::List;
    if (tess) {
        if (st.topology < D3D_PRIMITIVE_TOPOLOGY_1_CONTROL_POINT_PATCHLIST ||
            st.topology > D3D_PRIMITIVE_TOPOLOGY_32_CONTROL_POINT_PATCHLIST) {
            d3d12_diag("tessellated draw: not a patch-list topology");
            return;
        }
        group = st.topology -
                (D3D_PRIMITIVE_TOPOLOGY_1_CONTROL_POINT_PATCHLIST - 1u);
        if (group != pso->tessellation.input_control_points) {
            d3d12_diag("tessellated draw: the patch size differs from the hull "
                       "shader");
            return;
        }
    } else if (!pso->geometry.active && pso->vertex_cull.active) {
        // A vertex shader's cull distances: the input assembler's
        // primitives, as lists, each culled whole by pass C.
        group = topology_vertices(st.topology);
        assembly = assembly_of(st.topology);
        if (group == 0 || group > 3) {
            unimplemented("ExecuteCommandLists",
                          "SV_CullDistance of a vertex shader drawn with an "
                          "adjacency or patch topology");
            return;
        }
    } else if (!pso->geometry.active) {
        // Raster emulation with no geometry shader: the input assembler's
        // triangles, each drawn by pass C. GetAttributeAtVertex: its
        // points, lines or triangles, each drawn by the pass-through.
        const bool lists_or_strips =
            st.topology == D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST ||
            st.topology == D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP ||
            st.topology == D3D_PRIMITIVE_TOPOLOGY_POINTLIST ||
            st.topology == D3D_PRIMITIVE_TOPOLOGY_LINELIST ||
            st.topology == D3D_PRIMITIVE_TOPOLOGY_LINESTRIP;
        // An adjacency topology draws its primitives without the adjacent
        // vertices (ensure_vertex_replay): assembled whole, then compacted.
        if (!lists_or_strips && topology_vertices(st.topology) < 4) {
            d3d12_diag("lowered draw: an undefined or patch topology");
            return;
        }
        group = lists_or_strips ? topology_vertices(st.topology)
                                : topology_vertices(st.topology) / 2u;
        const u32 expected =
            pso->raster.active
                ? (pso->graphics.topology_type == D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT  ? 1u
                   : pso->graphics.topology_type == D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE ? 2u
                                                                                       : 3u)
                : pso->per_vertex_primitive;
        if (group != expected) {
            d3d12_diag("lowered draw: the topology does not match the "
                       "pipeline's primitive topology type");
            return;
        }
        if (!lists_or_strips) group = topology_vertices(st.topology);
        assembly = assembly_of(st.topology);
    } else {
        group = topology_vertices(st.topology);
        assembly = assembly_of(st.topology);
        if (group == 0 || group != pso->geometry.input_primitive) {
            d3d12_diag("geometry shader draw: the input-assembler topology "
                       "does not match the geometry shader");
            return;
        }
    }

    const auto note_uncounted_pixels = [&]() {
        if (pso->ps.present && !pso->statistics_pixels) {
            statistics_unmeasured(st, 1u << graphics::STAT_PS_INVOCATIONS,
                                  "PSInvocations of a pixel shader the "
                                  "counting translation cannot express");
        }
    };
    constexpr u32 kLoweredCounters = (1u << graphics::STAT_GS_INVOCATIONS) |
                                     (1u << graphics::STAT_GS_PRIMITIVES) |
                                     (1u << graphics::STAT_C_INVOCATIONS) |
                                     (1u << graphics::STAT_C_PRIMITIVES);
    std::vector<std::unique_ptr<graphics::BackendBuffer>> kept;
    const auto make = [&](u64 bytes, const void* data,
                          bool index) -> graphics::BackendBuffer* {
        graphics::BackendBufferDesc desc{};
        desc.byte_width = static_cast<usize>(std::max<u64>(bytes, 16u));
        desc.shader_resource = true;
        desc.unordered_access = true;
        desc.index_buffer = index;
        auto created = f.m_backend.create_buffer(desc, data);
        if (!created || !created->valid()) return nullptr;
        kept.push_back(std::move(created));
        return kept.back().get();
    };
    const u32 record_vec4s =
        static_cast<u32>(std::max<usize>(pso->capture_registers.size(), 1));
    // What the passes below read: the captured records (from
    // `records_offset` for instance 0, `record_bytes` apart), the primitive
    // keys, and how many primitives the passes are sized for. A GPU-
    // assembled draw (ExecuteIndirect) runs them once over every instance,
    // sized for `primitives` and stopped at the count `resolved` holds.
    u32 primitives = 0;
    u64 record_bytes = 0;
    u32 records_base = 0;
    u32 loop_instances = d.instance_count;
    graphics::BackendBuffer* records = nullptr;
    graphics::BackendBuffer* id_buffer = nullptr;
    graphics::BackendBuffer* capture_ib = nullptr;
    std::vector<u32> capture_indices;
    graphics::BackendTexture2D* target = nullptr;
    graphics::BackendPipeline* capture = nullptr;
    std::shared_ptr<graphics::BackendBuffer> resolved;
    if (indirect != nullptr) {
        if (!assemble_on_gpu(st, indexed, *indirect, group, assembly, tess,
                             record_vec4s, strides, kept, primitives, records,
                             id_buffer, resolved)) {
            return;
        }
        records_base = graphics::CAPTURE_HEADER_BYTES;
        loop_instances = 1;
    } else {
    std::vector<u32> indices;
    graphics::IndexCut cut{};
    if (!indexed) {
        indices.resize(d.index_count);
        for (u32 i = 0; i < d.index_count; ++i) indices[i] = d.start_index + i;
    } else {
        if (st.ib.va == 0) {
            d3d12_diag("indexed draw with no index buffer: skipped");
            return;
        }
        const bool index32 = st.ib.format == DXGI_FORMAT_R32_UINT;
        const u32 element = index32 ? 4u : 2u;
        const u64 bytes = static_cast<u64>(d.index_count) * element;
        const u64 start = static_cast<u64>(d.start_index) * element;
        if (st.ib.size != 0 && start + bytes > st.ib.size) {
            d3d12_diag("indexed draw: the index range is outside the view");
            return;
        }
        std::vector<u8> raw(static_cast<usize>(bytes));
        // Learn (DrawIndexedInstanced): IndexCountPerInstance indexes are
        // read from the index-buffer view at StartIndexLocation, in the
        // view's format, from whatever heap holds the buffer. A DEFAULT
        // heap has no CPU mapping. Geometry and tessellation draws both
        // assemble here, so when the mapping cannot be read the bytes are
        // copied out with the same ordered copy and wait ExecuteIndirect
        // uses (gpu_read). A copy or wait that fails stays unimplemented:
        // the indexes are not invented.
        const auto read_indexes = [&]() -> bool {
            if (f.read_buffer_va(st.ib.va + start, raw.data(), raw.size())) {
                return true;
            }
            graphics::BackendBuffer* source = nullptr;
            u64 source_offset = 0;
            if (!f.resolve_va(st.ib.va + start, bytes, source, source_offset)) {
                return false;
            }
            graphics::BackendBuffer* staging = nullptr;
            u64 staging_offset = 0;
            u8* host = nullptr;
            std::unique_ptr<graphics::BackendHeap> large;
            if (bytes <= CONSTANT_CHUNK_BYTES / 4) {
                if (!reserve(static_cast<usize>(bytes), staging, staging_offset,
                             host)) {
                    return false;
                }
            } else {
                graphics::BackendHeapDesc desc{};
                desc.size = (bytes + 0xFFFFu) & ~u64{0xFFFFu};
                desc.cpu_visible = true;
                desc.buffers_only = true;
                large = f.m_backend.create_heap(desc);
                if (!large || large->buffer() == nullptr ||
                    large->host_pointer() == nullptr) {
                    return false;
                }
                staging = large->buffer();
                host = static_cast<u8*>(large->host_pointer());
            }
            if (staging == nullptr || host == nullptr ||
                !f.m_backend.copy_buffer_region(staging, staging_offset, source,
                                                source_offset, bytes)) {
                return false;
            }
            g_replay_cpu_waits.fetch_add(1, std::memory_order_relaxed);
            graphics::frame_profile::WaitScope profile_wait("replay:lowered-stage-readback");
            if (auto completion = f.m_backend.flush_completion()) {
                if (!completion->wait()) return false;
            } else if (!f.m_backend.wait_idle()) {
                return false;
            }
            std::memcpy(raw.data(), host, raw.size());
            return true;
        };
        if (!read_indexes()) {
            unimplemented("ExecuteCommandLists",
                          "a geometry or tessellation draw whose index buffer "
                          "is not CPU-readable");
            return;
        }
        indices.resize(d.index_count);
        if (index32) {
            std::memcpy(indices.data(), raw.data(), raw.size());
        } else {
            for (u32 i = 0; i < d.index_count; ++i) {
                indices[i] = raw[i * 2u] | (static_cast<u32>(raw[i * 2u + 1u]) << 8);
            }
        }
        cut = strip_cut_of(pso->graphics);
    }
    std::vector<u32> ids = graphics::assemble_primitive_vertices(
        indices, indexed && cut.active, cut.value, indexed ? d.base_vertex : 0,
        group, assembly);
    primitives = static_cast<u32>(ids.size() / group);
    if (primitives == 0) return;
    // PIPELINE_STATISTICS: the input assembly the CPU just did, for every
    // instance (each assembles alike).
    count_lowered_input(st, ids, group, d.index_count, d.instance_count, tess);
    if (!tess && !pso->geometry.active && group >= 4) {
        // Adjacency with no geometry shader: a line is vertices 1, 2 of
        // its four, a triangle 0, 2, 4 of its six (D3D11.3 primitive
        // topologies; measured, d3d12_a66_c6_probe "adj *": the provoking
        // vertex is the first of those).
        std::vector<u32> drawn;
        drawn.reserve(ids.size() / 2);
        for (usize p = 0; p + group <= ids.size(); p += group) {
            if (group == 4) {
                drawn.push_back(ids[p + 1]);
                drawn.push_back(ids[p + 2]);
            } else {
                drawn.push_back(ids[p + 0]);
                drawn.push_back(ids[p + 2]);
                drawn.push_back(ids[p + 4]);
            }
        }
        group /= 2;
        ids = std::move(drawn);
    }

    u64 max_vertex = 0;
    if (!indexed) {
        max_vertex = static_cast<u64>(d.start_index) + d.index_count - 1u;
    } else {
        for (const u32 id : ids) {
            max_vertex = std::max(max_vertex, static_cast<u64>(id));
        }
    }
    record_bytes = (max_vertex + 1u) * record_vec4s * 16u;
    if (record_bytes == 0 ||
        record_bytes * d.instance_count > kMaxBytes) {
        d3d12_diag("the vertex capture would exceed the size limit");
        return;
    }

    if (indexed) {
        for (const u32 index : indices) {
            if (!(cut.active && index == cut.value)) {
                capture_indices.push_back(index);
            }
        }
        if (capture_indices.empty()) return;
        capture_ib = make(capture_indices.size() * sizeof(u32),
                          capture_indices.data(), true);
        if (capture_ib == nullptr) return;
    }
    records = make(record_bytes * d.instance_count, nullptr, false);
    id_buffer = make(ids.size() * sizeof(u32), ids.data(), false);
    target = f.capture_target();
    capture = f.capture_variant(*pso, strides);
    if (records == nullptr || id_buffer == nullptr || target == nullptr ||
        capture == nullptr) {
        d3d12_diag("geometry / tessellation draw: capture resources failed");
        return;
    }
    }

    const auto bind_vertices = [&](graphics::BackendDrawCall& call,
                                   u32 instance) {
        return bind_vertex_streams(st, call, true, d.start_instance, instance);
    };
    const auto bind_cbs = [&](graphics::BackendDispatchCall& call, u32 visibility,
                              const CompiledShader& shader,
                              std::vector<u32>& push) {
        if (uses_lowered_tables(shader)) {
            // bind_stage knows vertex and pixel visibility. A geometry, hull
            // or domain parameter is marked ALL for that call and every other
            // parameter PIXEL, which bind_stage skips on the vertex stage.
            if (!st.roots[0]) return false;
            auto filtered = std::make_shared<RootLayout>(*st.roots[0]);
            const auto keep = [&](u32 vis) {
                return vis == D3D12_SHADER_VISIBILITY_ALL || vis == visibility;
            };
            for (BindingParameter& parameter : filtered->parameters) {
                parameter.visibility = keep(parameter.visibility)
                                           ? D3D12_SHADER_VISIBILITY_ALL
                                           : D3D12_SHADER_VISIBILITY_PIXEL;
            }
            for (BindingSampler& sampler : filtered->static_samplers) {
                sampler.visibility = keep(sampler.visibility)
                                         ? D3D12_SHADER_VISIBILITY_ALL
                                         : D3D12_SHADER_VISIBILITY_PIXEL;
            }
            const auto saved = st.roots[0];
            st.roots[0] = filtered;
            const bool bound = bind_stage(st, STAGE_VERTEX, shader,
                                          call.descriptor_tables, push);
            st.roots[0] = saved;
            if (!bound) return false;
        } else {
            std::array<const graphics::BackendBuffer*,
                       graphics::BACKEND_MAX_CONSTANT_BUFFER_SLOTS>
                buffers{};
            std::array<u64, graphics::BACKEND_MAX_CONSTANT_BUFFER_SLOTS>
                offsets{};
            if (!bind_flat_constants(st, visibility, shader, buffers, offsets,
                                     push)) {
                return false;
            }
            for (u32 slot = 0; slot < buffers.size(); ++slot) {
                if (buffers[slot] == nullptr) continue;
                const unsigned index =
                    graphics::BACKEND_CONSTANT_BUFFER_BASE + slot;
                call.buffers[index] = buffers[slot];
                call.buffer_offsets[index] = offsets[slot];
            }
        }
        call.constants = push.data();
        call.constants_size = push.size() * sizeof(u32);
        return true;
    };
    const auto attach_counters = [&](graphics::BackendDispatchCall& call,
                                     u32 visibility,
                                     const CompiledShader& shader) {
        std::array<const graphics::BackendBuffer*,
                   graphics::BACKEND_MAX_UAV_COUNTER_BINDINGS>
            counters{};
        std::array<u64, graphics::BACKEND_MAX_UAV_COUNTER_BINDINGS>
            counter_offsets{};
        if (!bind_uav_counters(f, st, 0, visibility, false, shader, counters,
                               counter_offsets)) {
            return false;
        }
        for (u32 k = 0; k < counters.size(); ++k) {
            if (counters[k] == nullptr) continue;
            const unsigned index =
                graphics::BACKEND_UAV_COUNTER_BUFFER_BASE + k;
            if (index >= call.buffers.size() || call.buffers[index] != nullptr) {
                unimplemented("ExecuteCommandLists",
                              "a UAV counter overlaps a geometry-expansion "
                              "buffer");
                return false;
            }
            call.buffers[index] = counters[k];
            call.buffer_offsets[index] = counter_offsets[k];
        }
        return true;
    };
    const auto ensure = [&](std::shared_ptr<graphics::BackendComputePipeline>& slot,
                            const CompiledShader& shader) {
        if (!slot) {
            graphics::BackendComputePipelineDesc desc{};
            desc.compute_shader_msl = shader.msl;
            desc.compute_entry_point = "main0";
            desc.math_mode = shader.math_mode;
            desc.cache_key = std::hash<std::string>{}(shader.msl);
            auto created = f.m_backend.create_compute_pipeline(desc);
            if (created && created->valid()) slot = std::move(created);
        }
        return slot && slot->valid() ? slot.get() : nullptr;
    };

    // Target-independent / conservative rasterization (raster_emulation.hpp):
    // pass C over `primitives` triangles of `records` -- through record ids,
    // per-invocation counts or a tessellator index list at `ids` -- up to
    // the GPU's count at `limit` word `limit_word` when there is one.
    const auto raster_pass = [&](const std::string& msl,
                                 std::unique_ptr<graphics::BackendPipeline>& slot,
                                 graphics::BackendBuffer* records,
                                 u64 records_offset,
                                 graphics::BackendBuffer* ids, u64 primitives,
                                 graphics::BackendBuffer* limit,
                                 u32 limit_word) -> bool {
        return raster_pass_c(st, msl, slot, records, records_offset, ids,
                             primitives, limit, 0, limit_word);
    };

    // GetAttributeAtVertex with no lowered stage: the captured vertex records
    // drawn as a triangle list through the assembled record ids at t1
    // (pso->per_vertex_msl), the push block as for the other pass-throughs.
    const auto per_vertex_pass = [&](graphics::BackendBuffer* records,
                                     u64 records_offset,
                                     graphics::BackendBuffer* ids,
                                     u64 primitives) -> bool {
        const u32 n = pso->per_vertex_primitive;
        if (primitives == 0) return true;
        if (primitives * n > 0xFFFFFFFFull) return false;
        // PIPELINE_STATISTICS: the input assembly and vertex shader were
        // counted with the capture (count_lowered_input), the clipper by
        // count_clip; the counting pixel shader counts this pass's pixels.
        const std::shared_ptr<graphics::BackendBuffer> statistics =
            statistics_record(st);
        const bool own = !pso->per_vertex_msl.empty();
        const std::string& msl = own ? pso->per_vertex_msl : pso->adjacency_msl;
        const graphics::BackendPrimitiveTopology topology =
            n == 1u   ? graphics::BackendPrimitiveTopology::PointList
            : n == 2u ? graphics::BackendPrimitiveTopology::LineList
                      : graphics::BackendPrimitiveTopology::TriangleList;
        graphics::BackendPipeline* pipeline =
            statistics ? f.passthrough_variant(*pso, msl,
                                               own ? pso->per_vertex_statistics_pipeline
                                                   : pso->adjacency_statistics_pipeline,
                                               topology, true)
                       : f.passthrough_variant(*pso, msl,
                                               own ? pso->per_vertex_pipeline
                                                   : pso->adjacency_pipeline,
                                               topology);
        if (pipeline == nullptr) return false;
        graphics::BackendDrawCall draw{};
        draw.pipeline = pipeline;
        draw.topology = topology;
        draw.vertex_count = static_cast<u32>(primitives * n);
        draw.instance_count = 1;
        draw.vertex_srv_buffers[0] = records;
        draw.vertex_srv_buffer_offsets[0] = records_offset;
        draw.vertex_srv_buffers[1] = ids;
        std::vector<u32> passthrough_push(graphics::kPushBlockBytes / 4, 0u);
        passthrough_push[graphics::kLayerStateOffset / 4] = st.layer_limit;
        passthrough_push[graphics::kLayerStateOffset / 4 + 1] = st.layer_addend;
        draw.vertex_constants = passthrough_push.data();
        draw.vertex_constants_size = passthrough_push.size() * sizeof(u32);
        std::vector<u32> pixel_push;
        if (!bind_stage(st, STAGE_PIXEL, pso->ps, draw.fragment_descriptor_tables,
                        pixel_push)) {
            return false;
        }
        draw.fragment_constants = pixel_push.data();
        draw.fragment_constants_size = pixel_push.size() * sizeof(u32);
        if (statistics) {
            draw.fragment_statistics_buffer = statistics.get();
            note_uncounted_pixels();
        }
        submit_draw(st, draw);
        return true;
    };

    // B + C for one instance's captured records: a geometry expansion, or the
    // hull kernel, software tessellator and domain kernel (then a geometry
    // shader, when one follows the domain shader).
    const auto draw_produced = [&](graphics::BackendBuffer* produced,
                                   u32 produced_offset,
                                   graphics::BackendBuffer* produced_ids,
                                   u32 vertices_per_primitive, u32 count,
                                   graphics::BackendBuffer* gpu_count,
                                   u32 gpu_count_offset, bool primitive_ids,
                                   u32 primitive_id_offset) -> bool {
        if (!pso->geometry.active) return true;
        const PipelineState::GeometryProgram& program = pso->geometry;
        graphics::BackendComputePipeline* kernel =
            ensure(pso->geometry.pipeline, program.kernel);
        if (kernel == nullptr) return false;
        u32 streams = 0;
        for (u32 stream = 0; stream < 4; ++stream) {
            if ((program.stream_mask & (1u << stream)) != 0) streams = stream + 1;
        }
        const u64 invocations =
            static_cast<u64>(count) * std::max(program.instance_count, 1u);
        if (invocations == 0) return true;
        const u64 expanded_bytes = invocations * program.vertices_per_invocation *
                                   std::max(streams, 1u) *
                                   std::max(program.output_record_vec4s, 1u) * 16u;
        const u64 counter_bytes =
            (static_cast<u64>(graphics::kGsInvocationCountBase) +
             invocations * std::max(streams, 1u)) *
            sizeof(u32);
        if (expanded_bytes > kMaxBytes || counter_bytes > kMaxBytes) return false;
        std::vector<u32> control(
            static_cast<usize>(graphics::kGsInvocationCountBase +
                               invocations * std::max(streams, 1u)),
            0u);
        // Fed by vitreo_ia_resolve: SV_PrimitiveID is the primitive's index
        // within its instance, the resolved per-instance count.
        const bool per_instance_ids =
            resolved && gpu_count == resolved.get() && !primitive_ids;
        control[graphics::kGsControlFlags] =
            (gpu_count != nullptr ? graphics::kGsFlagInvocationLimit : 0u) |
            (primitive_ids ? graphics::kGsFlagPrimitiveIdTable : 0u) |
            (per_instance_ids ? graphics::kGsFlagPrimitiveIdModulo : 0u);
        control[1] = 2u;
        control[4] = vertices_per_primitive;
        control[graphics::kGsControlInvocationLimit] = count;
        control[graphics::kGsControlStreamStride] = static_cast<u32>(invocations);
        control[graphics::kGsControlPrimitiveIdTable] = primitive_id_offset;
        graphics::BackendBuffer* expanded = make(expanded_bytes, nullptr, false);
        graphics::BackendBuffer* counter =
            make(counter_bytes, control.data(), false);
        if (expanded == nullptr || counter == nullptr) return false;
        if (gpu_count != nullptr &&
            !f.m_backend.copy_buffer_region(
                counter, graphics::kGsControlInvocationLimit * sizeof(u32),
                gpu_count, gpu_count_offset, sizeof(u32))) {
            return false;
        }
        if (per_instance_ids &&
            !f.m_backend.copy_buffer_region(
                counter,
                graphics::kGsControlPrimitivesPerInstance * sizeof(u32),
                resolved.get(),
                graphics::IA_RESOLVED_PRIMITIVES_PER_INSTANCE * sizeof(u32),
                sizeof(u32))) {
            return false;
        }
        graphics::BackendDispatchCall dispatch{};
        dispatch.pipeline = kernel;
        dispatch.group_count_x = static_cast<u32>(invocations);
        dispatch.group_count_y = dispatch.group_count_z = 1;
        dispatch.threads_per_group_x = program.kernel.threads_x;
        dispatch.threads_per_group_y = program.kernel.threads_y;
        dispatch.threads_per_group_z = program.kernel.threads_z;
        std::vector<u32> push;
        if (!bind_cbs(dispatch, D3D12_SHADER_VISIBILITY_GEOMETRY, program.kernel,
                      push)) {
            return false;
        }
        constexpr unsigned base = graphics::BACKEND_GS_EXPANSION_BUFFER_BASE;
        dispatch.buffers[base + 0] = produced;
        dispatch.buffer_offsets[base + 0] = produced_offset;
        dispatch.buffers[base + 1] = expanded;
        dispatch.buffers[base + 2] = counter;
        dispatch.buffers[base + 3] = produced_ids;
        if (!attach_counters(dispatch, D3D12_SHADER_VISIBILITY_GEOMETRY,
                             program.kernel)) {
            return false;
        }
        if (!f.m_backend.dispatch(dispatch)) return false;
        if (pso->stream_output.active) {
            if (invocations > 0xFFFFFFFFull ||
                !geometry_stream_out(
                    f, *pso, expanded, counter, static_cast<u32>(invocations),
                    program.vertices_per_invocation, kept,
                    so_statistics_record(st))) {
                d3d12_diag("geometry stream output was not recorded");
                return false;
            }
        }
        const u32 raster_stream = pso->stream_output.active
                                      ? pso->stream_output.rasterized_stream
                                      : 0u;
        const bool rasterize =
            raster_stream != D3D12_SO_NO_RASTERIZED_STREAM &&
            !program.passthrough_msl.empty() &&
            raster_stream < D3D12_SO_STREAM_COUNT &&
            (program.stream_mask & (1u << raster_stream)) != 0;
        // GSInvocations, GSPrimitives of every stream, and the rasterized
        // stream's clipper counts.
        if (invocations <= 0xFFFFFFFFull) {
            count_geometry(st, static_cast<u32>(invocations), counter, expanded,
                           rasterize && !rasterization_disabled(st));
        } else {
            statistics_unmeasured(st, kLoweredCounters,
                                  "a geometry expansion of more than 2^32 "
                                  "invocations");
        }
        if (!rasterize) return true;
        // Tier 3 with no stage before the rasterizer reading SV_ViewID: the
        // expansion ran once, each view draws it.
        return at_stage(kViRaster, [&]() -> bool {
        const PipelineState::GeometryProgram& gprogram = pso->geometry;
        if (pso->raster.active) {
            // Whole output primitives of 1, 2 or 3 vertices per region.
            const u32 per_primitive = gprogram.output_topology == 1u   ? 1u
                                      : gprogram.output_topology == 3u ? 2u
                                                                       : 3u;
            return raster_pass(gprogram.passthrough_msl, pso->geometry.passthrough,
                               expanded, 0, counter,
                               invocations * gprogram.vertices_per_invocation /
                                   per_primitive,
                               nullptr, 0);
        }
        const std::shared_ptr<graphics::BackendBuffer> statistics =
            statistics_record(st);
        const graphics::BackendPrimitiveTopology topology =
            list_topology(gprogram.output_topology);
        graphics::BackendPipeline* pipeline =
            statistics ? f.passthrough_variant(
                             *pso, gprogram.passthrough_msl,
                             pso->geometry_statistics_passthrough, topology, true)
                       : f.passthrough_variant(*pso, gprogram.passthrough_msl,
                                               pso->geometry.passthrough, topology);
        if (pipeline == nullptr) return false;
        graphics::BackendDrawCall draw{};
        draw.pipeline = pipeline;
        draw.topology = topology;
        draw.vertex_count = static_cast<u32>(invocations *
                                             gprogram.vertices_per_invocation);
        draw.instance_count = 1;
        draw.vertex_srv_buffers[0] = expanded;
        draw.vertex_srv_buffers[1] = counter;
        // The pass-through's push block: its depth offset (0) and the
        // render-target array index rule (kLayerStateOffset).
        std::vector<u32> passthrough_push(graphics::kPushBlockBytes / 4, 0u);
        passthrough_push[graphics::kLayerStateOffset / 4] = st.layer_limit;
        passthrough_push[graphics::kLayerStateOffset / 4 + 1] = st.layer_addend;
        draw.vertex_constants = passthrough_push.data();
        draw.vertex_constants_size = passthrough_push.size() * sizeof(u32);
        std::vector<u32> pixel_push;
        if (!bind_stage(st, STAGE_PIXEL, pso->ps, draw.fragment_descriptor_tables,
                        pixel_push)) {
            return false;
        }
        draw.fragment_constants = pixel_push.data();
        draw.fragment_constants_size = pixel_push.size() * sizeof(u32);
        if (statistics) {
            draw.fragment_statistics_buffer = statistics.get();
            note_uncounted_pixels();
        }
        submit_draw(st, draw);
        return true;
        });
    };

    for (u32 instance = 0; instance < loop_instances; ++instance) {
        if (indirect == nullptr) {
        graphics::BackendDrawCall capture_draw{};
        capture_draw.pipeline = capture;
        // A capture runs every vertex once. Metal drops the tail of an
        // incomplete primitive, and adjacency has no topology here; the
        // expansion assembles primitives from the id list (as D3D11's
        // capture does).
        capture_draw.topology = graphics::BackendPrimitiveTopology::PointList;
        capture_draw.vertex_capture_buffer = records;
        capture_draw.vertex_capture_buffer_offset = record_bytes * instance;
        if (!bind_vertices(capture_draw, instance)) return;
        if (indexed) {
            capture_draw.index_buffer = capture_ib;
            capture_draw.index_32bit = true;
            capture_draw.index_count = static_cast<u32>(capture_indices.size());
            capture_draw.base_vertex = d.base_vertex;
        } else {
            capture_draw.vertex_count = d.index_count;
            capture_draw.first_vertex = d.start_index;
        }
        capture_draw.instance_count = 1;
        capture_draw.first_instance = d.start_instance + instance;
        std::vector<u32> vertex_push;
        if (uses_lowered_tables(pso->vs)) {
            if (!bind_stage(st, STAGE_VERTEX, pso->vs,
                            capture_draw.vertex_descriptor_tables,
                            vertex_push)) {
                return;
            }
        } else if (!bind_flat_constants(
                       st, D3D12_SHADER_VISIBILITY_VERTEX, pso->vs,
                       capture_draw.vertex_constant_buffers,
                       capture_draw.vertex_constant_buffer_offsets,
                       vertex_push)) {
            return;
        }
        // The capture draws instance `instance` alone (first instance start +
        // instance): SV_InstanceID adds it back (kInstanceIdBiasOffset), and
        // SV_StartInstanceLocation subtracts it from the base instance.
        if (vertex_push.size() * 4 < graphics::kPushBlockBytes) {
            vertex_push.resize(graphics::kPushBlockBytes / 4, 0u);
        }
        vertex_push[graphics::kInstanceIdBiasOffset / 4] = instance;
        capture_draw.vertex_constants = vertex_push.data();
        capture_draw.vertex_constants_size = vertex_push.size() * sizeof(u32);
        graphics::BackendRenderPassDesc pass{};
        pass.color_targets[0] = target;
        pass.preserve_color_contents = true;
        auto render_pass = f.m_backend.create_render_pass_standalone(pass);
        if (!render_pass || !render_pass->valid() ||
            !render_pass->draw(capture_draw) || !render_pass->end()) {
            d3d12_diag("geometry / tessellation draw: the capture was not "
                       "submitted");
            return;
        }
        }

        const u32 record_offset =
            records_base + static_cast<u32>(record_bytes * instance);
        if (!tess && !pso->geometry.active && pso->vertex_cull.active &&
            !pso->raster.active) {
            // Pass C over this instance's primitives: vertex v draws record
            // ids[v], and a primitive with one cull distance negative at all
            // its vertices is moved outside the clip volume.
            if (pipeline_statistics_open(st)) {
                statistics_unmeasured(
                    st, (1u << graphics::STAT_C_INVOCATIONS) |
                            (1u << graphics::STAT_C_PRIMITIVES) |
                            (1u << graphics::STAT_PS_INVOCATIONS),
                    "the clipper and pixel counts of a vertex shader's "
                    "cull-distance pass");
            }
            const graphics::BackendPrimitiveTopology topology =
                group == 1u   ? graphics::BackendPrimitiveTopology::PointList
                : group == 2u ? graphics::BackendPrimitiveTopology::LineList
                              : graphics::BackendPrimitiveTopology::TriangleList;
            graphics::BackendPipeline* pipeline = f.passthrough_variant(
                *pso, pso->vertex_cull.vertex_msl, pso->vertex_cull.pipeline,
                topology);
            if (pipeline == nullptr) return;
            graphics::BackendDrawCall draw{};
            draw.pipeline = pipeline;
            draw.topology = topology;
            draw.vertex_count = primitives * group;
            draw.instance_count = 1;
            draw.vertex_srv_buffers[0] = records;
            draw.vertex_srv_buffer_offsets[0] = record_offset;
            draw.vertex_srv_buffers[1] = id_buffer;
            // The pass-through's push block (see the geometry pass-through).
            std::vector<u32> passthrough_push(graphics::kPushBlockBytes / 4, 0u);
            passthrough_push[graphics::kLayerStateOffset / 4] = st.layer_limit;
            passthrough_push[graphics::kLayerStateOffset / 4 + 1] = st.layer_addend;
            draw.vertex_constants = passthrough_push.data();
            draw.vertex_constants_size = passthrough_push.size() * sizeof(u32);
            std::vector<u32> pixel_push;
            if (pso->ps.present &&
                !bind_stage(st, STAGE_PIXEL, pso->ps,
                            draw.fragment_descriptor_tables, pixel_push)) {
                return;
            }
            draw.fragment_constants = pixel_push.data();
            draw.fragment_constants_size = pixel_push.size() * sizeof(u32);
            submit_draw(st, draw);
            continue;
        }
        if (!tess && !pso->geometry.active && !pso->raster.active) {
            // GetAttributeAtVertex over the vertex shader's own primitives,
            // or a tier-3 view-instanced vertex shader that does not read
            // SV_ViewID: list vertices through their record ids. The
            // clipper runs before the first view-dependent stage: once.
            count_clip(st, records, record_offset, id_buffer, primitives);
            if (!at_stage(kViRaster, [&]() {
                    return per_vertex_pass(records, record_offset, id_buffer,
                                           primitives);
                })) {
                d3d12_diag("GetAttributeAtVertex draw: the pass-through was "
                           "not submitted");
                return;
            }
            continue;
        }
        if (!tess && !pso->geometry.active) {
            // Raster emulation of the vertex shader's own triangles (never
            // GPU-assembled: draw reads an indirect draw's counts first).
            // The clipper counts run over the captured records (the
            // geometry and domain paths count theirs in count_geometry /
            // count_tessellation).
            if (!rasterization_disabled(st)) {
                count_raster_clipper(st, primitives, id_buffer, records,
                                     record_offset, record_vec4s);
            }
            if (!at_stage(kViRaster, [&]() {
                    return raster_pass(pso->raster.vertex_msl, pso->raster.pipeline,
                                       records, record_offset, id_buffer,
                                       primitives, nullptr, 0);
                })) {
                d3d12_diag("raster-emulated draw: pass C was not submitted");
                return;
            }
            continue;
        }
        if (!tess) {
            // A GPU-assembled draw: the kernel stops at the resolved
            // primitive count, and SV_PrimitiveID restarts with every
            // instance (D3D11.3 8.17) at the resolved per-instance count.
            if (!at_stage(kViGeometry, [&]() {
                    return draw_produced(records, record_offset, id_buffer, group,
                                         primitives, resolved.get(),
                                         graphics::IA_RESOLVED_PRIMITIVES * 4u,
                                         false, 0);
                })) {
                d3d12_diag("geometry shader draw: the expansion was not "
                           "submitted");
                return;
            }
            continue;
        }

        // Tessellation: the hull kernel and the software tessellator (T),
        // then the domain kernel and what follows it (D). Tier 3 loops the
        // views around T + D (a hull shader reading SV_ViewID), D (a domain
        // shader reading it) or the stages after (at_stage).
        u32 hull_runs = 0;
        const auto tessellate = [&]() -> bool {
        // A hull kernel run for a view after the first: "For the stages X
        // and later, view instances increase those stages' pipeline
        // statistics" (count_lowered_input counted the first).
        if (hull_runs++ > 0 && !resolved) {
            add_statistic(st, graphics::STAT_HS_INVOCATIONS, primitives);
        }
            const PipelineState::TessellationProgram& program = pso->tessellation;
            graphics::BackendComputePipeline* hull =
                ensure(pso->tessellation.hull_pipeline, program.hull);
            graphics::BackendComputePipeline* domain_kernel =
                ensure(pso->tessellation.domain_pipeline, program.domain_stage);
            graphics::BackendComputePipeline* patch = nullptr;
            graphics::BackendComputePipeline* scan = nullptr;
            if (hull == nullptr || domain_kernel == nullptr ||
                !f.tessellator_kernels(patch, scan)) {
                d3d12_diag("tessellated draw: a kernel is unavailable");
                return false;
            }
            const u64 cp_bytes = static_cast<u64>(primitives) *
                                 program.output_control_points * program.cp_record_vec4s *
                                 16u;
            const u64 pc_bytes =
                static_cast<u64>(primitives) * program.pc_record_vec4s * 16u;
            const u64 factor_bytes =
                static_cast<u64>(primitives) * graphics::kTessFactorWords * 4u;
            if (cp_bytes > kMaxBytes || pc_bytes > kMaxBytes) {
                d3d12_diag("tessellated draw: a pass would exceed the size limit");
                return false;
            }
            std::array<u32, graphics::kHullControlWords> hull_control{};
            hull_control[1] = 2u;
            hull_control[4] = group;
            hull_control[5] = program.cp_record_vec4s;
            hull_control[6] = program.pc_record_vec4s;
            if (resolved) {
                // GPU-counted patches: those past [9] return, SV_PrimitiveID is
                // the patch modulo [10] (both copied from the resolve below).
                hull_control[graphics::kHullControlFlags] = 3u;
            }
            graphics::BackendBuffer* cp_out = make(cp_bytes, nullptr, false);
            graphics::BackendBuffer* pc_out = make(pc_bytes, nullptr, false);
            graphics::BackendBuffer* factors = make(factor_bytes, nullptr, false);
            graphics::BackendBuffer* hull_words =
                make(sizeof(hull_control), hull_control.data(), false);
            if (cp_out == nullptr || pc_out == nullptr || factors == nullptr ||
                hull_words == nullptr) {
                return false;
            }
            if (resolved &&
                (!f.m_backend.copy_buffer_region(
                     hull_words, graphics::kHullControlPatchLimit * 4u,
                     resolved.get(), graphics::IA_RESOLVED_PRIMITIVES * 4u, 4u) ||
                 !f.m_backend.copy_buffer_region(
                     hull_words, graphics::kHullControlPatchesPerInstance * 4u,
                     resolved.get(),
                     graphics::IA_RESOLVED_PRIMITIVES_PER_INSTANCE * 4u, 4u))) {
                return false;
            }
            graphics::BackendDispatchCall hull_call{};
            hull_call.pipeline = hull;
            hull_call.group_count_x = primitives;
            hull_call.group_count_y = hull_call.group_count_z = 1;
            hull_call.threads_per_group_x = program.hull.threads_x;
            hull_call.threads_per_group_y = program.hull.threads_y;
            hull_call.threads_per_group_z = program.hull.threads_z;
            std::vector<u32> hull_push;
            if (!bind_cbs(hull_call, D3D12_SHADER_VISIBILITY_HULL, program.hull,
                          hull_push)) {
                return false;
            }
            constexpr unsigned base = graphics::BACKEND_GS_EXPANSION_BUFFER_BASE;
            hull_call.buffers[base + 0] = records;
            hull_call.buffer_offsets[base + 0] = record_offset;
            hull_call.buffers[base + 1] = cp_out;
            hull_call.buffers[base + 2] = hull_words;
            hull_call.buffers[base + 3] = id_buffer;
            hull_call.buffers[base + 4] = pc_out;
            hull_call.buffers[base + 5] = factors;
            if (!attach_counters(hull_call, D3D12_SHADER_VISIBILITY_HULL,
                                 program.hull)) {
                return false;
            }
            if (!f.m_backend.dispatch(hull_call)) return false;

            const graphics::tess::TessPatchBound bound = graphics::tess::tess_patch_bound(
                program.tess_domain, program.partitioning, program.output_primitive,
                program.max_tess_factor);
            const u32 out_vertices = program.output_primitive == 1u   ? 1u
                                     : program.output_primitive == 2u ? 2u
                                                                      : 3u;
            const u32 ds_vec4s =
                static_cast<u32>(std::max<usize>(program.domain_records.size(), 1));
            const u32 point_capacity =
                primitives * std::max(bound.points, 1u);
            const u32 index_capacity =
                primitives * std::max(bound.indices, 1u);
            const u64 point_bytes = static_cast<u64>(point_capacity) * 16u;
            const u64 index_bytes =
                (static_cast<u64>(index_capacity) + index_capacity / out_vertices + 1u) *
                4u;
            const u64 domain_bytes =
                static_cast<u64>(point_capacity) * ds_vec4s * 16u;
            if (point_bytes > kMaxBytes || index_bytes > kMaxBytes ||
                domain_bytes > kMaxBytes) {
                d3d12_diag("tessellated draw: a pass would exceed the size limit");
                return false;
            }
            graphics::BackendBuffer* counts =
                make(static_cast<u64>(primitives) * 8u, nullptr, false);
            graphics::BackendBuffer* points = make(point_bytes, nullptr, false);
            graphics::BackendBuffer* connectivity = make(index_bytes, nullptr, false);
            graphics::BackendBuffer* domain_records =
                make(domain_bytes, nullptr, false);
            std::array<u32, graphics::tess::TESS_BLOCK_WORDS> block_init{};
            block_init[graphics::kDomainControlCpStride] = program.cp_record_vec4s;
            block_init[graphics::kDomainControlPcStride] = program.pc_record_vec4s;
            graphics::BackendBuffer* block =
                make(sizeof(block_init), block_init.data(), false);
            graphics::BackendBuffer* args =
                make(graphics::tess::TESS_ARGS_WORDS * sizeof(u32), nullptr, false);
            if (counts == nullptr || points == nullptr || connectivity == nullptr ||
                domain_records == nullptr || block == nullptr || args == nullptr) {
                return false;
            }
            graphics::tess::TessPatchParams params{};
            params.domain = program.tess_domain;
            params.partitioning = program.partitioning;
            params.output_primitive = program.output_primitive;
            params.patches = primitives;
            params.patches_per_instance = primitives;
            // GPU-counted: patches past the resolved count are empty, and the
            // per-instance count comes from the same block ({all, per instance}
            // are its first two words).
            params.count_mode = resolved ? 1u : 0u;
            params.write_primitive_ids = pso->geometry.active ? 1u : 0u;
            params.factor_words = graphics::kTessFactorWords;
            params.point_capacity = point_capacity;
            params.index_capacity = index_capacity;
            graphics::BackendDispatchCall tess_call{};
            tess_call.pipeline = patch;
            tess_call.threads_per_group_x = 32;
            tess_call.group_count_x = (primitives + 31u) / 32u;
            tess_call.group_count_y = tess_call.group_count_z = 1;
            tess_call.constants = &params;
            tess_call.constants_size = sizeof(params);
            tess_call.buffers[graphics::tess::TESS_FACTORS_INDEX] = factors;
            tess_call.buffers[graphics::tess::TESS_COUNTS_INDEX] = counts;
            tess_call.buffers[graphics::tess::TESS_POINTS_INDEX] = points;
            tess_call.buffers[graphics::tess::TESS_INDICES_INDEX] = connectivity;
            tess_call.buffers[graphics::tess::TESS_PRIMITIVE_IDS_INDEX] = connectivity;
            tess_call.buffer_offsets[graphics::tess::TESS_PRIMITIVE_IDS_INDEX] =
                index_capacity * 4u;
            tess_call.buffers[graphics::tess::TESS_PATCH_COUNT_INDEX] =
                resolved ? resolved.get() : counts;
            static_assert(graphics::IA_RESOLVED_PRIMITIVES == 0 &&
                          graphics::IA_RESOLVED_PRIMITIVES_PER_INSTANCE == 1);
            if (!f.m_backend.dispatch(tess_call)) return false;

            graphics::tess::TessScanParams scan_params{};
            scan_params.patches = primitives;
            scan_params.vertices_per_primitive = out_vertices;
            scan_params.point_capacity = point_capacity;
            scan_params.index_capacity = index_capacity;
            graphics::BackendDispatchCall scan_call{};
            scan_call.pipeline = scan;
            scan_call.threads_per_group_x = graphics::tess::TESS_SCAN_THREADS;
            scan_call.group_count_x = scan_call.group_count_y = scan_call.group_count_z = 1;
            scan_call.constants = &scan_params;
            scan_call.constants_size = sizeof(scan_params);
            scan_call.buffers[graphics::tess::TESS_SCAN_COUNTS_INDEX] = counts;
            scan_call.buffers[graphics::tess::TESS_SCAN_BLOCK_INDEX] = block;
            scan_call.buffers[graphics::tess::TESS_SCAN_ARGS_INDEX] = args;
            if (!f.m_backend.dispatch(scan_call)) return false;
            params.mode = 1;
            tess_call.constants = &params;
            if (!f.m_backend.dispatch(tess_call)) return false;

        const auto domain_on = [&]() -> bool {
        // The view's domain kernel (translated with its SV_ViewID).
        const PipelineState::TessellationProgram& vprogram = pso->tessellation;
        graphics::BackendComputePipeline* vdomain =
            ensure(pso->tessellation.domain_pipeline, vprogram.domain_stage);
        if (vdomain == nullptr) return false;
                graphics::BackendDispatchCall domain_call{};
                domain_call.pipeline = vdomain;
                domain_call.threads_per_group_x = graphics::kDomainComputeThreads;
                domain_call.group_count_x =
                    (point_capacity + graphics::kDomainComputeThreads - 1u) /
                    graphics::kDomainComputeThreads;
                domain_call.group_count_y = domain_call.group_count_z = 1;
                std::vector<u32> domain_push;
                if (!bind_cbs(domain_call, D3D12_SHADER_VISIBILITY_DOMAIN,
                              vprogram.domain_stage,
                              domain_push)) {
                    return false;
                }
                domain_call.buffers[base + 0] = points;
                domain_call.buffers[base + 1] = cp_out;
                domain_call.buffers[base + 2] = block;
                domain_call.buffers[base + 3] = domain_records;
                domain_call.buffers[base + 4] = pc_out;
                if (!attach_counters(domain_call, D3D12_SHADER_VISIBILITY_DOMAIN,
                                     vprogram.domain_stage)) {
                    return false;
                }
                if (!f.m_backend.dispatch(domain_call)) return false;
                // DSInvocations, and the clipper counts when the domain shader's
                // vertices are what the rasterizer gets.
                count_tessellation(
                    st, out_vertices, index_capacity, block, connectivity,
                    domain_records, ds_vec4s,
                    !pso->geometry.active &&
                        !(pso->stream_output.active &&
                          pso->stream_output.rasterized_stream != 0) &&
                        !rasterization_disabled(st));

                if (pso->stream_output.active && !pso->geometry.active) {
                    if (!domain_stream_out(f, *pso, vprogram.output_primitive,
                                           domain_records, ds_vec4s, connectivity, block,
                                           index_capacity, kept,
                                           so_statistics_record(st))) {
                        d3d12_diag("domain stream output was not recorded");
                        return false;
                    }
                }

                if (pso->geometry.active) {
                    if (!at_stage(kViGeometry, [&]() {
                            return draw_produced(
                                domain_records, 0, connectivity, out_vertices,
                                index_capacity / out_vertices, block,
                                graphics::tess::TESS_BLOCK_PRIMITIVES * sizeof(u32),
                                true, index_capacity);
                        })) {
                        d3d12_diag("tessellated draw: the geometry expansion was not "
                                   "submitted");
                        return false;
                    }
                    return true;
                }
                // RasterizedStream 0 is the domain shader's only stream. The draw
                // uses the passthrough built with the pipeline. Every other index,
                // including D3D12_SO_NO_RASTERIZED_STREAM, stops after the repack.
                if (pso->stream_output.active &&
                    pso->stream_output.rasterized_stream != 0) {
                    return true;
                }
                // Tier 3, no stage reading SV_ViewID before the rasterizer:
                // each view draws the domain shader's vertices.
                const auto tess_raster = [&]() -> bool {
                const PipelineState::TessellationProgram& rprogram = pso->tessellation;
                if (pso->raster.active) {
                    if (!raster_pass(rprogram.passthrough_msl,
                                     pso->tessellation.passthrough, domain_records, 0,
                                     connectivity, index_capacity / out_vertices, block,
                                     graphics::tess::TESS_BLOCK_PRIMITIVES)) {
                        d3d12_diag("raster-emulated tessellated draw: pass C was not "
                                   "submitted");
                    }
                    return true;
                }
                const std::shared_ptr<graphics::BackendBuffer> statistics =
                    statistics_record(st);
                const graphics::BackendPrimitiveTopology topology =
                    out_vertices == 1u   ? graphics::BackendPrimitiveTopology::PointList
                    : out_vertices == 2u ? graphics::BackendPrimitiveTopology::LineList
                                         : graphics::BackendPrimitiveTopology::TriangleList;
                graphics::BackendPipeline* pipeline =
                    statistics ? f.passthrough_variant(
                                     *pso, rprogram.passthrough_msl,
                                     pso->domain_statistics_passthrough, topology, true)
                               : f.passthrough_variant(*pso, rprogram.passthrough_msl,
                                                       pso->tessellation.passthrough,
                                                       topology);
                if (pipeline == nullptr) return false;
                graphics::BackendDrawCall raster{};
                raster.pipeline = pipeline;
                raster.topology = topology;
                raster.vertex_srv_buffers[0] = domain_records;
                raster.instance_count = 1;
                if (rprogram.passthrough_through_index_list) {
                    // Cull distances: one vertex per index-list entry, the list at
                    // t1. The indexed argument block {count, 1, 0, 0, 0} reads as
                    // the non-indexed {count, 1, 0, 0}.
                    raster.vertex_srv_buffers[1] = connectivity;
                    raster.vertex_count = index_capacity;
                } else {
                    raster.vertex_srv_buffers[1] = domain_records;
                    raster.index_buffer = connectivity;
                    raster.index_32bit = true;
                    raster.index_count = index_capacity;
                }
                raster.indirect.buffer = args;
                raster.indirect.offset =
                    graphics::tess::TESS_ARGS_DRAW * static_cast<unsigned>(sizeof(u32));
                // The pass-through's push block (see the geometry pass-through).
                std::vector<u32> passthrough_push(graphics::kPushBlockBytes / 4, 0u);
                passthrough_push[graphics::kLayerStateOffset / 4] = st.layer_limit;
                passthrough_push[graphics::kLayerStateOffset / 4 + 1] = st.layer_addend;
                raster.vertex_constants = passthrough_push.data();
                raster.vertex_constants_size = passthrough_push.size() * sizeof(u32);
                std::vector<u32> pixel_push;
                if (!bind_stage(st, STAGE_PIXEL, pso->ps, raster.fragment_descriptor_tables,
                                pixel_push)) {
                    return false;
                }
                raster.fragment_constants = pixel_push.data();
                raster.fragment_constants_size = pixel_push.size() * sizeof(u32);
                if (statistics) {
                    raster.fragment_statistics_buffer = statistics.get();
                    note_uncounted_pixels();
                }
                submit_draw(st, raster);
                return true;
                };
                return at_stage(kViRaster, tess_raster);
        };
        return at_stage(kViDomain, domain_on);
        };
        if (!at_stage(kViHull, tessellate)) return;
    }
}

void Replayer::dispatch(ReplayState& st, const cmd::Dispatch& d,
                        const graphics::BackendIndirectArgs* indirect) {
    if (graphics::frame_profile::enabled()) [[unlikely]] {
        graphics::frame_profile::add(graphics::frame_profile::Counter::Dispatches);
    }
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
    std::array<const graphics::BackendBuffer*,
               graphics::BACKEND_MAX_UAV_COUNTER_BINDINGS>
        counters{};
    std::array<u64, graphics::BACKEND_MAX_UAV_COUNTER_BINDINGS>
        counter_offsets{};
    if (!bind_uav_counters(f, st, 1, D3D12_SHADER_VISIBILITY_ALL, true, pso->cs,
                           counters, counter_offsets)) {
        d3d12_diag("dispatch: a UAV counter could not be bound: skipped");
        return;
    }
    for (u32 k = 0; k < counters.size(); ++k) {
        if (counters[k] == nullptr) continue;
        const unsigned index = graphics::BACKEND_UAV_COUNTER_BUFFER_BASE + k;
        call.buffers[index] = counters[k];
        call.buffer_offsets[index] = counter_offsets[k];
    }
    if (indirect != nullptr) call.indirect = *indirect;
    if (!f.m_backend.dispatch(call)) {
        d3d12_diag("dispatch: the backend refused it");
        return;
    }
    count_dispatch(st, d, indirect);
}

}  // namespace vitreo::winapi::d3d12
