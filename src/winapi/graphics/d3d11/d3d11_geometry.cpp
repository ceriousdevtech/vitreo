// SPDX-License-Identifier: LGPL-2.1-only
/// @file d3d11_geometry.cpp
/// @brief D3D11 geometry shaders on Metal, which has no geometry stage
///        (11.16; design: .docs/design-geometry-shader-emulation.md).
///
/// A draw with a geometry program bound becomes three GPU passes:
///
///   A  capture   the bound vertex shader, translated to also write each
///                vertex's outputs into a buffer keyed by vertex id, with
///                rasterization off (execute_draw, DrawVariant::Capture);
///   B  expand    the geometry shader lowered to a compute kernel, one thread
///                per input primitive, appending INDEPENDENT output primitives
///                and counting the vertices it wrote;
///   C  draw      a generated pass-through vertex shader reading what B wrote,
///                with the guest's pixel shader, targets and states, drawn as
///                a list whose vertex count B produced -- an indirect draw
///                (execute_draw, DrawVariant::GeometryOutput).
///
/// B's input records are laid out by the VERTEX shader's capture, so the
/// kernel is compiled per (geometry shader, vertex-shader record layout).
///
/// Each invocation (input primitive x [instance(n)] instance) owns a fixed
/// region of B's output and publishes how much of it it wrote, so C draws the
/// expanded primitives in D3D's order -- by input primitive, then instance --
/// and culls the unwritten rest. (A shared atomic cursor put them in the
/// order the GPU ran threads, which overlapping or blended output shows.)
///
/// Up to four output streams (dcl_stream / emit_stream / cut_stream, 13.5.2):
/// each record holds the union of the streams' output registers, and each
/// stream has a region per invocation, so stream output reads every stream
/// and pass C rasterizes only the rasterized one. maxvertexcount bounds the
/// emits of an invocation across all streams (measured, d3d11_gsso_probe
/// [maxvertexcount]).
///
/// An indirect draw (DrawInstancedIndirect / DrawIndexedInstancedIndirect)
/// is assembled on the GPU timeline by vitreo_ia_resolve
/// (graphics/shaders/ia_resolve.hpp): the capture draw takes the resolved
/// argument block and the kernel its primitive count, so nothing waits for
/// or reads back a GPU-written count.
///
/// Creation refuses with E_NOTIMPL, never a half-working object, only what
/// the emulation cannot express: cull distances (MSL has none) and shaders
/// the translator cannot lower. A draw it cannot serve -- a topology that
/// does not match the shader's input primitive -- draws nothing, with a
/// diagnostic naming why.

#include "winapi/graphics/d3d11/d3d11.hpp"
#include "winapi/graphics/d3d11/d3d11_internal.hpp"
#include "winapi/core/marshaler.hpp"
#include "graphics/contracts/backend_device.hpp"
#include "graphics/shaders/dxbc_parser.hpp"
#include "graphics/shaders/dxbc_to_spirv.hpp"
#include "graphics/shaders/gs_passthrough.hpp"
#include "graphics/shaders/ia_resolve.hpp"
#include "graphics/shaders/stream_out_repack.hpp"
#include "core/memory/guest_memory.hpp"

#include <vitreo/ab_cycle.hpp>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <vector>

namespace vitreo::winapi {
namespace {

/// Expansion and capture buffers larger than this are declined rather than
/// allocated; both scale with guest-controlled counts.
constexpr u64 MAX_GEOMETRY_BYTES = 256ull << 20;

/// A direct draw's instances are captured, expanded and drawn together
/// (execute_geometry_direct), the way the indirect path always has been.
/// VITREO_D3D11_GS_BATCH=0 restores one capture, expansion and draw per
/// instance, for bisection only; under VITREO_AB_CYCLE_S the change is cycled
/// in-process by the name "gs-batch".
bool geometry_batch_active() {
    static const bool on = [] {
        const char* v = std::getenv("VITREO_D3D11_GS_BATCH");
        return v == nullptr || v[0] != '0';
    }();
    return on && !diagnostics::ab_cycle_off("gs-batch");
}

// D3D_NAME system values a geometry shader may output.
constexpr u32 NAME_POSITION = 1;
constexpr u32 NAME_CLIP_DISTANCE = 2;
constexpr u32 NAME_CULL_DISTANCE = 3;
constexpr u32 NAME_RENDER_TARGET_ARRAY_INDEX = 4;
constexpr u32 NAME_VIEWPORT_ARRAY_INDEX = 5;

/// D3D11_PRIMITIVE_TOPOLOGY -> vertices per primitive and whether it strips.
/// Zero vertices means a topology the emulation does not take (adjacency,
/// patches, undefined).
struct InputTopology {
    u32 vertices_per_primitive{0};
    bool strip{false};
    /// TRIANGLESTRIP_ADJ: primitive vertices come from the measured table
    /// (graphics::triangle_strip_adjacency_vertex), not a fixed stride.
    bool strip_adjacency{false};
};

[[nodiscard]] InputTopology input_topology(u32 topology) {
    switch (topology) {
    case 1: return {1, false};  // POINTLIST
    case 2: return {2, false};  // LINELIST
    case 3: return {2, true};   // LINESTRIP
    case 4: return {3, false};  // TRIANGLELIST
    case 5: return {3, true};   // TRIANGLESTRIP
    // With adjacency (Learn, primitive topologies): a line list is 4 vertices
    // a primitive, a line strip's primitive i is vertices i..i+3, a triangle
    // list 6, and a triangle strip picks its six by a first/middle/last,
    // odd/even table measured on Windows (d3d11_tristrip_adj_probe).
    case 10: return {4, false};         // LINELIST_ADJ
    case 11: return {4, true};          // LINESTRIP_ADJ
    case 12: return {6, false};         // TRIANGLELIST_ADJ
    case 13: return {6, true, true};    // TRIANGLESTRIP_ADJ
    default: return {};
    }
}

[[nodiscard]] graphics::BackendPrimitiveTopology list_topology(
    u32 output_topology) {
    switch (output_topology) {
    case 1: return graphics::BackendPrimitiveTopology::PointList;
    case 3: return graphics::BackendPrimitiveTopology::LineList;
    default: return graphics::BackendPrimitiveTopology::TriangleList;
    }
}

}  // namespace

// -----------------------------------------------------------------------------
// CreateGeometryShader
// -----------------------------------------------------------------------------

void D3D11Manager::create_geometry_shader(Marshaler& m, GuestAddr self) {
    // (pShaderBytecode, BytecodeLength, pClassLinkage, ppGeometryShader)
    const GuestAddr bytecode = m.arg_ptr(1);
    const u64 length = m.arg(2);
    const GuestAddr linkage = m.arg_ptr(3);
    const GuestAddr shader_out = m.arg_ptr(4);
    if (shader_out != 0) (void)m.write_ptr(shader_out, 0);
    const auto refuse = [&](u32 hr, const std::string& why) {
        d3d11_diag("ID3D11Device::CreateGeometryShader -> 0x%08x (%s)", hr,
                   why.c_str());
        m.set_return(hr);
    };
    // Same argument rules as the other Create*Shader methods here.
    if (bytecode == 0 || length == 0 || length > (1u << 24) ||
        !valid_class_linkage(m_interfaces.find(self)->second.owner,
                             linkage)) {
        refuse(d3d11::E_INVALIDARG, "bad bytecode or class linkage");
        return;
    }
    std::vector<u8> bytes(static_cast<usize>(length));
    if (m_memory.read_bytes(bytecode, bytes.data(), bytes.size()).is_err()) {
        refuse(d3d11::E_INVALIDARG, "bytecode unreadable");
        return;
    }
    const auto container = graphics::parse_dxbc(bytes.data(), bytes.size());
    if (!container.valid() ||
        container.stage != graphics::ShaderStage::Geometry) {
        refuse(d3d11::E_INVALIDARG, "not geometry-shader bytecode");
        return;
    }
    if (const char* why = shader_bytecode_refused(self, bytes.data(),
                                                  bytes.size(), {})) {
        refuse(d3d11::E_INVALIDARG, why);
        return;
    }
    if (uav_requirements_unavailable(self, bytes.data(), bytes.size())) {
        refuse(d3d11::E_INVALIDARG, "declares a UAV below feature level 11_1");
        return;
    }
    // WARP: a shader that declares interfaces needs a class linkage
    // (E_INVALIDARG without one, d3d11_classlink_probe [create]).
    if (graphics::dxbc_uses_class_linkage(container) &&
        refuse_interface_shader(linkage, "CreateGeometryShader") !=
            d3d11::S_OK) {
        refuse(d3d11::E_INVALIDARG, "shader declares interfaces");
        return;
    }

    // No stream-output declaration: stream 0 is the rasterized one
    // (D3D11.3 13.5.2). WARP measured otherwise for a multi-stream shader --
    // it rasterized every stream (d3d11_gsso_probe [streams]) -- which the
    // functional spec does not allow.
    GeometryShader shader{};
    std::string why;
    u32 built = d3d11::S_OK;
    {
        const PureWorkUnlock unlocked(*this);
        built = build_geometry_program(bytes, container,
                                       /*rasterized_stream=*/0, shader, why);
    }
    if (const u32 hr = built; hr != d3d11::S_OK) {
        refuse(hr, why);
        return;
    }

    if (shader_out == 0) {
        m.set_return(1u);  // S_FALSE: validation only
        return;
    }
    if (!ensure_storage()) {
        refuse(d3d11::E_FAIL, "no object storage");
        return;
    }
    const GuestAddr address = allocate_object(m_geometry_shader_vtable);
    if (address == 0) {
        refuse(d3d11::E_FAIL, "object allocation failed");
        return;
    }
    shader.interface_address = address;
    const auto owner = m_interfaces.find(self);
    shader.device_owner = owner != m_interfaces.end() ? owner->second.owner : 0;
    attach_shader_linkage(shader.linkage, bytes, container);
    register_class_linkage(linkage, container);
    const u32 input_primitive = shader.input_primitive;
    const u32 output_topology = shader.output_topology;
    const u32 max_output_vertices = shader.max_output_vertices;
    const u32 instances = shader.instance_count;
    const GuestAddr device_owner =
        owner != m_interfaces.end() ? owner->second.owner : 0;
    m_geometry_shaders.emplace(address, std::move(shader));
    acquire_parent(device_owner);
    if (m.write_ptr(shader_out, address).is_err()) {
        (void)release_object(address);
        refuse(d3d11::E_INVALIDARG, "out-parameter unwritable");
        return;
    }
    d3d11_diag("ID3D11Device::CreateGeometryShader -> 0x%llx in=%u out=%u "
               "maxvertexcount=%u instances=%u",
               static_cast<unsigned long long>(address), input_primitive,
               output_topology, max_output_vertices, instances);
    m.set_return(d3d11::S_OK);
}

// -----------------------------------------------------------------------------
// The program pieces, shared with CreateGeometryShaderWithStreamOutput
// -----------------------------------------------------------------------------

u32 D3D11Manager::build_geometry_program(
    const std::vector<u8>& bytes, const graphics::DxbcContainer& container,
    u32 rasterized_stream, GeometryShader& shader, std::string& why) {
    // Lower it once without a vertex-shader layout: that is what decides
    // whether the emulation can express this shader at all, and it reports
    // the declared shape. The real kernel is built per vertex shader at draw.
    graphics::DxbcTranslationOptions options{};
    options.emulate_geometry_with_compute = true;
    const auto lowered = graphics::translate_dxbc_to_spirv(container, options);
    if (!lowered.valid() || !lowered.gs_lowered_to_compute) {
        why = "the geometry emulation cannot express this shader: " +
              lowered.failure;
        return d3d11::E_NOTIMPL;
    }

    // Every stream's record holds the union of the streams' output registers
    // (ascending; D3D11.3 13.5.2). Pass C delivers the RASTERIZED stream's
    // elements as varyings, and its rasterizer system values -- clip
    // distances, render-target-array and viewport index -- as the matching
    // Metal builtins, per component; the other streams' names "are ignored"
    // (13.5.2). Cull distances have no MSL equivalent, and dropping one
    // would draw a different picture.
    std::vector<graphics::GsPassthroughOutput> outputs;
    bool writes_viewport_index = false;
    for (const auto& element : container.output_signature) {
        const u32 sv = element.system_value_type;
        const bool delivered = element.stream == rasterized_stream;
        if (delivered && sv == NAME_CULL_DISTANCE) {
            why = "SV_CullDistance output: Metal has no cull distances";
            return d3d11::E_NOTIMPL;
        }
        // Several elements can share one register (packed components, or
        // the same o# in several streams); the record holds registers, so
        // one entry each.
        auto same = std::find_if(outputs.begin(), outputs.end(),
                                 [&](const auto& o) {
                                     return o.register_num ==
                                            element.register_num;
                                 });
        if (same == outputs.end()) {
            graphics::GsPassthroughOutput o{};
            o.register_num = element.register_num;
            o.component_type = element.component_type;
            o.delivered = false;
            outputs.push_back(o);
            same = std::prev(outputs.end());
        }
        if (!delivered) continue;
        if (!same->delivered) same->component_type = element.component_type;
        same->delivered = true;
        writes_viewport_index |= sv == NAME_VIEWPORT_ARRAY_INDEX;
        if (sv == NAME_POSITION) same->is_position = true;
        if (sv == NAME_CLIP_DISTANCE ||
            sv == NAME_RENDER_TARGET_ARRAY_INDEX ||
            sv == NAME_VIEWPORT_ARRAY_INDEX) {
            for (u32 c = 0; c < 4; ++c) {
                if ((element.component_mask & (1u << c)) != 0u) {
                    same->component_system_values[c] = static_cast<u8>(sv);
                }
            }
        }
    }
    if (outputs.size() != lowered.gs_output_registers) {
        why = "the output signature and the declared outputs disagree";
        return d3d11::E_NOTIMPL;
    }
    const u32 vertices_per_invocation = graphics::gs_vertices_per_invocation(
        lowered.gs_output_topology, lowered.gs_max_output_vertices);
    if (vertices_per_invocation == 0) {
        why = "maxvertexcount cannot complete one output primitive";
        return d3d11::E_NOTIMPL;
    }
    // The draw binds the push block to every vertex stage, so the pass-through
    // applies the depth offset like the translated stages.
    const bool rasterizes =
        rasterized_stream < 4u &&
        (lowered.gs_stream_mask & (1u << rasterized_stream)) != 0u;
    std::string passthrough =
        rasterizes ? graphics::generate_gs_passthrough_vertex_msl(
                         outputs, vertices_per_invocation,
                         /*fixed_function_push=*/true)
                   : std::string{};
    // A shader with no outputs at all emits nothing (fxc leaves even the
    // output topology out) and is created like any other (measured,
    // d3d11_pstats_probe [gs] "emits nothing": GSInvocations 2, nothing
    // drawn).
    if (passthrough.empty() && rasterized_stream == 0 &&
        lowered.gs_stream_mask == 1u && !outputs.empty()) {
        // Nothing to rasterize without SV_Position. A stream-output shader
        // that rasterizes no stream needs none, and D3D leaves a rasterized
        // stream without a position undefined (13.5.2): the SO variant of a
        // multi-stream shader creates (measured) and draws nothing there.
        why = "no SV_Position output to rasterize";
        return d3d11::E_NOTIMPL;
    }
    std::string kernel_msl;
    d3d11::ShaderBindingLayout layout{};
    if (!compile_geometry_kernel(bytes, {}, kernel_msl, &layout)) {
        why = "the expansion kernel does not compile";
        return d3d11::E_NOTIMPL;
    }

    shader.program = true;
    shader.bytecode = bytes;
    shader.input_primitive = lowered.gs_input_primitive;
    shader.output_topology = lowered.gs_output_topology;
    shader.max_output_vertices = lowered.gs_max_output_vertices;
    shader.instance_count = std::max(1u, lowered.gs_instance_count);
    shader.stream_mask = lowered.gs_stream_mask;
    shader.rasterized_stream = rasterized_stream;
    shader.output_record_vec4s = lowered.gs_output_registers;
    shader.output_record_registers.clear();
    for (const auto& o : graphics::gs_record_order(outputs)) {
        shader.output_record_registers.push_back(o.register_num);
    }
    shader.passthrough_msl = std::move(passthrough);
    shader.passthrough_outputs = outputs;
    shader.line_quad_msl.clear();
    shader.writes_viewport_index = writes_viewport_index;
    shader.binding_layout = layout;
    return d3d11::S_OK;
}

// -----------------------------------------------------------------------------
// The three-pass draw
// -----------------------------------------------------------------------------

std::shared_ptr<graphics::BackendComputePipeline>
D3D11Manager::geometry_kernel_for(GuestAddr geometry_shader,
                                  const std::vector<u32>& record_layout,
                                  u32 minmax_samplers) {
    auto gs_it = m_geometry_shaders.find(geometry_shader);
    if (gs_it == m_geometry_shaders.end()) return nullptr;
    auto& kernels = gs_it->second.kernels;
    const std::vector<u32> key =
        kernel_variant_key(record_layout, minmax_samplers);
    if (kernels.find(key) == kernels.end()) {
        std::string msl;
        std::shared_ptr<graphics::BackendComputePipeline> built;
        if (compile_geometry_kernel(gs_it->second.bytecode, record_layout, msl,
                                    nullptr, false, minmax_samplers)) {
            graphics::BackendComputePipelineDesc desc{};
            desc.compute_shader_msl = msl;
            desc.compute_entry_point = "main0";
            desc.cache_key = std::hash<std::string>{}(msl);
            auto created = m_backend.create_compute_pipeline(desc);
            if (created && created->valid()) built = std::move(created);
        }
        kernels.emplace(key, std::move(built));
    }
    return kernels.at(key);
}

bool D3D11Manager::geometry_expand(
    GuestAddr context, GuestAddr geometry_shader,
    const std::shared_ptr<graphics::BackendComputePipeline>& kernel,
    const GeometryInput& in,
    const std::shared_ptr<graphics::BackendBuffer>& stats) {
    auto& state = m_context_states[context];
    const auto decline = [](const char* why) {
        d3d11_diag("geometry shader draw: nothing drawn: %s", why);
        return false;
    };
    auto gs_it = m_geometry_shaders.find(geometry_shader);
    if (gs_it == m_geometry_shaders.end() || !kernel) {
        return decline("no geometry program");
    }
    // Copied out: the draws below go through execute_draw, and nothing here
    // should hold an iterator into the shader map across them.
    const GeometryShader gs = gs_it->second;
    const u32 vertices_per_invocation = graphics::gs_vertices_per_invocation(
        gs.output_topology, gs.max_output_vertices);
    const u64 invocations = in.max_primitives * gs.instance_count;
    if (invocations == 0) return true;
    // One region per stream the shader declares, each `invocations` slots
    // of `vertices_per_invocation` vertices.
    u32 streams = 0;
    for (u32 s = 0; s < 4; ++s) {
        if ((gs.stream_mask & (1u << s)) != 0) streams = s + 1;
    }
    const u64 expanded_vertices =
        invocations * vertices_per_invocation * streams;
    const u64 expanded_bytes = expanded_vertices * gs.output_record_vec4s * 16u;
    const u64 counter_bytes =
        (graphics::kGsInvocationCountBase + invocations * streams) *
        sizeof(u32);
    // A shader that can emit nothing runs anyway while a PIPELINE_STATISTICS
    // query counts its invocations.
    if (expanded_bytes == 0 && !pipeline_statistics_building(context)) {
        return true;
    }
    if (expanded_bytes > MAX_GEOMETRY_BYTES ||
        counter_bytes > MAX_GEOMETRY_BYTES ||
        invocations > 0xFFFFFFFFull) {
        return decline("the expansion would exceed the size limit");
    }

    graphics::BackendBufferDesc storage{};
    storage.shader_resource = true;
    storage.unordered_access = true;
    storage.byte_width = static_cast<usize>(std::max<u64>(expanded_bytes, 16u));
    // Transient (gs-ring): reused once the GPU has retired the draw that
    // last held it. Nothing reads a vertex the kernel did not write first.
    auto expanded = m_transient.acquire(storage, nullptr, 0);
    // The counter buffer opens with the control block -- 1-4 describe how a
    // (primitive, vertex) pair becomes a record index: here the assembled
    // ids, `vpp` a primitive, through the kernel's 32-bit index path --
    // followed by one written-vertex count per invocation and stream, which
    // every invocation sets.
    std::vector<u32> control(static_cast<usize>(
        graphics::kGsInvocationCountBase + invocations * streams), 0u);
    control[graphics::kGsControlFlags] =
        (in.primitive_count != nullptr ? graphics::kGsFlagInvocationLimit
                                       : 0u) |
        (in.primitives_per_instance != 0 ||
                 in.primitives_per_instance_count != nullptr
             ? graphics::kGsFlagPrimitiveIdModulo
             : 0u) |
        (in.primitive_id_table ? graphics::kGsFlagPrimitiveIdTable : 0u);
    control[1] = 2u;
    control[2] = 0u;
    control[3] = 0u;
    control[4] = in.vertices_per_primitive;
    control[graphics::kGsControlInvocationLimit] =
        static_cast<u32>(in.max_primitives);
    control[graphics::kGsControlPrimitivesPerInstance] =
        in.primitives_per_instance;
    control[graphics::kGsControlStreamStride] = static_cast<u32>(invocations);
    control[graphics::kGsControlPrimitiveIdTable] = in.primitive_id_table_offset;
    storage.byte_width = static_cast<usize>(counter_bytes);
    // The whole block -- control words and every count -- is written into a
    // reused counter buffer too.
    auto counter = m_transient.acquire(storage, control.data(),
                                       static_cast<usize>(counter_bytes));
    if (!expanded || !counter) {
        return decline("expansion buffer allocation failed");
    }
    if (in.primitive_count != nullptr &&
        !m_backend.copy_buffer_region(
            counter.get(), graphics::kGsControlInvocationLimit * sizeof(u32),
            in.primitive_count, in.primitive_count_offset, sizeof(u32))) {
        // The GPU decides how many primitives there are; the kernel reads
        // the limit from the control block, placed there in command order.
        return decline("the primitive count could not be placed");
    }
    if (in.primitives_per_instance_count != nullptr &&
        !m_backend.copy_buffer_region(
            counter.get(),
            graphics::kGsControlPrimitivesPerInstance * sizeof(u32),
            in.primitives_per_instance_count,
            in.primitives_per_instance_offset, sizeof(u32))) {
        return decline("the per-instance primitive count could not be placed");
    }

    graphics::BackendDispatchCall call{};
    call.pipeline = kernel.get();
    // One thread per invocation, primitive-major.
    call.group_count_x = static_cast<u32>(invocations);
    call.group_count_y = call.group_count_z = 1;
    bind_compute_stage(state.geometry_constant_buffers,
                       state.geometry_constant_first,
                       state.geometry_shader_resources,
                       state.geometry_samplers, {}, gs.binding_layout, call);
    // The geometry stage's s# MipLODBias for explicit-LOD sampling.
    auto geometry_push = sampler_push_block(state.geometry_samplers);
    fill_constant_buffer_limits(geometry_push,
                                state.geometry_constant_buffers,
                                state.geometry_constant_first,
                                state.geometry_constant_count);
    fill_typed_buffer_biases(geometry_push, state.geometry_shader_resources);
    call.constants = geometry_push.data();
    call.constants_size = geometry_push.size() * sizeof(u32);
    constexpr unsigned base = graphics::BACKEND_GS_EXPANSION_BUFFER_BASE;
    call.buffers[base + 0] = in.records;
    call.buffer_offsets[base + 0] = in.records_offset;
    call.buffers[base + 1] = expanded.get();
    call.buffers[base + 2] = counter.get();
    call.buffers[base + 3] = in.ids;
    call.buffer_offsets[base + 3] = in.ids_offset;
    if (!m_backend.dispatch(call)) {
        return decline("expansion dispatch failed");
    }
    if (pipeline_statistics_building(context)) {
        // GSInvocations, GSPrimitives and the rasterized stream's clipper
        // counts, from what each invocation wrote (vitreo_stats_geometry).
        const u32 raster = gs.rasterized_stream;
        const bool rasterized = raster < 4 &&
                                (gs.stream_mask & (1u << raster)) != 0 &&
                                !gs.passthrough_msl.empty() &&
                                !rasterization_disabled(state);
        count_geometry_statistics(context, gs_it->second,
                                  static_cast<u32>(invocations),
                                  counter.get(), expanded.get(), rasterized);
    }

    // A program created with a stream-output declaration also streams its
    // expansion, each stream on its own (D3D11.3 14.1).
    if (!gs.stream_out.empty()) {
        GeometryStreamOutput so{};
        so.entries = gs.stream_out;
        so.strides = gs.stream_out_strides;
        so.stream_slots = gs.stream_out_slots;
        so.record_registers = gs.output_record_registers;
        so.vertices_per_invocation = vertices_per_invocation;
        so.vertices_per_primitive = gs.output_topology == 1   ? 1u
                                    : gs.output_topology == 3 ? 2u
                                                              : 3u;
        // Invocations past a GPU-decided limit wrote nothing (their counts
        // are zero), so the compaction may walk them all.
        so.invocations = invocations;
        so.invocation_stride = static_cast<u32>(invocations);
        stream_out_geometry_expansion(context, so, expanded.get(),
                                      counter.get(), stats);
    }
    // C: draw what the rasterized stream wrote. A stream-output program
    // rasterizes the stream its creation named; a plain one stream 0.
    const u32 raster = gs.rasterized_stream;
    if (raster >= 4 || (gs.stream_mask & (1u << raster)) == 0) return true;
    if (gs.passthrough_msl.empty()) {
        // D3D11.3 13.5.2: a rasterized stream without a position is
        // undefined; nothing is drawn.
        d3d11_diag("geometry shader draw: the rasterized stream %u has no "
                   "SV_Position; nothing rasterized", raster);
        return true;
    }
    DrawVariant output_draw{};
    output_draw.kind = DrawVariant::Kind::GeometryOutput;
    output_draw.buffer = expanded.get();
    output_draw.offset = static_cast<unsigned>(
        raster * invocations * vertices_per_invocation *
        gs.output_record_vec4s * 16u);
    output_draw.vertex_msl = &gs.passthrough_msl;
    output_draw.counts = counter.get();
    output_draw.counts_offset =
        static_cast<unsigned>(raster * invocations * sizeof(u32));
    output_draw.vertex_count =
        static_cast<u32>(invocations * vertices_per_invocation);
    output_draw.viewport_array = gs.writes_viewport_index;
    output_draw.topology = list_topology(gs.output_topology);
    // Lines the rasterizer state wants as rectangles -- line output, or
    // triangles in WIREFRAME fill -- are pass C's line-quad form over the
    // same regions (generate_line_quad_vertex_msl).
    const u32 line_mode = line_rasterization_mode(state);
    bool wireframe = false;
    if (const auto rs = m_rasterizer_states.find(state.rasterizer_state);
        state.rasterizer_state != 0 && rs != m_rasterizer_states.end()) {
        wireframe = rs->second.desc.fill_mode == 2u;
    }
    const u32 line_vpp = gs.output_topology == 3 ? 2u
                         : gs.output_topology == 5 && wireframe ? 3u : 0u;
    TransientBufferRing::Buffer line_params;
    std::string line_msl;  // a copy, as `gs`: no map reference across draws
    if (line_mode != 0 && line_vpp != 0) {
        auto& shader = gs_it->second;
        if (shader.line_quad_msl.empty()) {
            graphics::LineQuadSource source{};
            source.vertices_per_invocation = vertices_per_invocation;
            source.vertices_per_primitive = line_vpp;
            shader.line_quad_msl = graphics::generate_line_quad_vertex_msl(
                shader.passthrough_outputs, source,
                /*fixed_function_push=*/true);
        }
        line_msl = shader.line_quad_msl;
        const u64 primitives =
            invocations * vertices_per_invocation / line_vpp;
        if (!line_msl.empty()) {
            line_params = line_quad_params(state, line_mode,
                                           static_cast<u32>(primitives));
        }
        if (!line_params) {
            d3d11_unimplemented_once(
                "Draw", "quadrilateral / alpha-antialiased lines out of a "
                        "geometry shader drawn aliased: its outputs cannot "
                        "be drawn so");
        } else {
            const u32 triangles =
                graphics::line_quad_triangles_per_primitive(line_vpp);
            output_draw.vertex_msl = &line_msl;
            output_draw.vertex_count =
                static_cast<u32>(primitives * triangles * 3u);
            output_draw.topology =
                graphics::BackendPrimitiveTopology::TriangleList;
            output_draw.line_params = line_params.get();
            output_draw.line_quad_divisor = triangles;
            output_draw.line_coverage_alpha = line_mode == 1;
        }
    }
    execute_draw(context, false, 0, 0, 0, 1, 0, 0, 0, &output_draw);
    if (!output_draw.submitted) {
        return decline("the expanded draw was not submitted");
    }
    return true;
}

void D3D11Manager::execute_geometry(GuestAddr context, bool indexed, u32 count,
                                    u32 start_location, i32 base_vertex,
                                    u32 instance_count, u32 first_instance,
                                    const IndirectDraw& indirect) {
    auto& state = m_context_states[context];
    const auto decline = [](const char* why) {
        d3d11_diag("geometry shader draw: nothing drawn: %s", why);
    };
    const GuestAddr geometry_shader = state.geometry_shader;
    auto gs_it = m_geometry_shaders.find(geometry_shader);
    if (gs_it == m_geometry_shaders.end() || !gs_it->second.program) return;
    const InputTopology topology = input_topology(state.primitive_topology);
    if (topology.vertices_per_primitive == 0) {
        return decline("patch or undefined topology");
    }
    if (topology.vertices_per_primitive != gs_it->second.input_primitive) {
        // Windows rejects this draw too (the debug layer names it); there is
        // no primitive to hand the shader.
        return decline("the input-assembler topology does not match the "
                       "geometry shader's input primitive");
    }
    auto vs = m_vertex_shaders.find(state.vertex_shader);
    if (vs == m_vertex_shaders.end()) return decline("no vertex shader");
    if (!ensure_vertex_capture(vs->second)) {
        return decline("the vertex shader cannot be captured");
    }
    const std::vector<u32> record_layout = vs->second.capture_registers;
    const std::shared_ptr<graphics::BackendComputePipeline> kernel =
        geometry_kernel_for(geometry_shader, record_layout,
                            minmax_sampler_mask(state.geometry_samplers));
    if (!kernel) {
        return decline("the expansion kernel failed for this vertex shader");
    }
    const auto stats = stream_out_stats_record(context);
    if (!stats) return decline("no stats record");
    if (indirect.active()) {
        execute_geometry_indirect(context, indexed, indirect, kernel,
                                  record_layout, stats);
        note_stream_out_stats(context, stats);
        return;
    }
    if (geometry_batch_active() && !record_layout.empty()) {
        // Every instance in one capture, one expansion and one draw.
        (void)execute_geometry_direct(context, indexed, count, start_location,
                                      base_vertex, instance_count,
                                      first_instance, kernel, record_layout,
                                      stats);
        note_stream_out_stats(context, stats);
        return;
    }

    const u32 vpp = topology.vertices_per_primitive;
    // Primitive assembly on the CPU, as the shader receives it: strip
    // winding, cuts, list primitives holding the cut value, the strip
    // adjacency table (graphics::assemble_primitive_vertices, measured).
    DirectCapture capture{};
    if (const char* why = prepare_direct_capture(
            state, indexed, count, start_location, base_vertex,
            instance_count, vpp,
            topology.strip_adjacency
                ? graphics::PrimitiveAssembly::StripAdjacency
                : topology.strip ? graphics::PrimitiveAssembly::Strip
                                 : graphics::PrimitiveAssembly::List,
            static_cast<u32>(record_layout.size()), capture)) {
        return decline(why);
    }
    if (capture.primitives == 0) return;

    for (u32 instance = 0; instance < instance_count; ++instance) {
        // A: capture this instance's vertices.
        if (!capture_direct_instance(context, indexed, count, start_location,
                                     base_vertex, first_instance, instance,
                                     capture)) {
            return decline("the capture draw was not submitted");
        }
        // B + C: expand this instance's primitives (SV_PrimitiveID from 0 in
        // every instance, D3D11.3 8.17) and draw or stream them.
        GeometryInput in{};
        in.records = capture.records.get();
        in.records_offset =
            static_cast<u32>(capture.record_bytes * instance);
        in.ids = capture.ids.get();
        in.vertices_per_primitive = vpp;
        in.max_primitives = capture.primitives;
        if (!geometry_expand(context, geometry_shader, kernel, in, stats)) {
            break;
        }
    }
    note_stream_out_stats(context, stats);
}

const char* D3D11Manager::prepare_direct_capture(
    const ContextState& state, bool indexed, u32 count, u32 start_location,
    i32 base_vertex, u32 instance_count, u32 vpp,
    graphics::PrimitiveAssembly assembly, u32 record_vec4s,
    DirectCapture& out) {
    out = {};
    CaptureExtent extent{};
    if (const char* why = capture_extent(state, indexed, count, start_location,
                                         base_vertex, extent)) {
        return why;
    }
    std::vector<u32> indices;
    if (!draw_indices(state, indexed, count, extent, indices)) {
        return "the index data is not readable on the CPU";
    }
    const u32 cut = state.index_buffer_format == 42 ? 0xFFFFFFFFu : 0xFFFFu;
    const std::vector<u32> ids = graphics::assemble_primitive_vertices(
        indices, indexed, cut, indexed ? base_vertex : 0, vpp, assembly);
    out.primitives = static_cast<u32>(ids.size() / vpp);
    if (out.primitives == 0) return nullptr;
    // The capture writes a record for EVERY vertex the draw runs, the
    // trailing ones no primitive uses included (a 9-vertex LINELIST_ADJ
    // runs vertex 8), so the buffer is sized by the draw's whole range,
    // never by the assembled ids alone: sizing by the ids let vertex 8's
    // record overwrite the next allocation (d3d11_a07_ia_probe `A draw
    // LINELIST_ADJ n=9`).
    extent.max_vertex = std::max<u64>(
        extent.max_vertex, *std::max_element(ids.begin(), ids.end()));
    // An indexed capture runs through the draw's index values with the cut
    // values left out: nothing needs their records, and a point-list draw
    // would fetch vertex 0xFFFF / 0xFFFFFFFF.
    std::vector<u32> capture_indices;
    if (indexed) {
        for (const u32 index : indices) {
            if (index != cut) capture_indices.push_back(index);
        }
    }
    out.record_bytes = (extent.max_vertex + 1) * record_vec4s * 16u;
    if (out.record_bytes * instance_count > MAX_GEOMETRY_BYTES) {
        return "the capture would exceed the size limit";
    }
    if (!m_stream_out_target) {
        graphics::BackendTexture2DDesc td{};
        td.width = td.height = 1;
        td.mip_levels = td.array_size = 1;
        td.pixel_format = graphics::PixelFormat::RGBA8Unorm;
        td.render_target = true;
        m_stream_out_target = m_backend.create_texture_2d(td, nullptr, 0);
    }
    if (!m_stream_out_target) return "capture target unavailable";

    graphics::BackendBufferDesc storage{};
    storage.shader_resource = true;
    storage.unordered_access = true;
    storage.byte_width = static_cast<usize>(out.record_bytes * instance_count);
    // Transient (gs-ring). The capture writes every record pass B reads.
    out.records = m_transient.acquire(storage, nullptr, 0);
    if (!out.records) return "capture buffer allocation failed";
    graphics::BackendBufferDesc ids_desc{};
    ids_desc.byte_width = ids.size() * sizeof(u32);
    ids_desc.shader_resource = true;
    out.ids = m_transient.acquire(ids_desc, ids.data(), ids_desc.byte_width);
    if (!capture_indices.empty()) {
        ids_desc.byte_width = capture_indices.size() * sizeof(u32);
        ids_desc.index_buffer = true;
        out.capture_indices = m_transient.acquire(
            ids_desc, capture_indices.data(), ids_desc.byte_width);
        out.capture_index_count = static_cast<u32>(capture_indices.size());
    }
    if (!out.ids || (indexed && !out.capture_indices)) {
        return "primitive assembly buffers could not be allocated";
    }
    return nullptr;
}

bool D3D11Manager::capture_direct_instance(
    GuestAddr context, bool indexed, u32 count, u32 start_location,
    i32 base_vertex, u32 first_instance, u32 instance,
    const DirectCapture& capture) {
    DrawVariant capture_draw{};
    capture_draw.kind = DrawVariant::Kind::Capture;
    capture_draw.buffer = capture.records.get();
    capture_draw.offset = static_cast<unsigned>(capture.record_bytes * instance);
    capture_draw.target = m_stream_out_target.get();
    capture_draw.capture_indices = capture.capture_indices.get();
    capture_draw.capture_index_count = capture.capture_index_count;
    // Instance `instance` alone: SV_InstanceID = instance, and its
    // per-instance data at StartInstanceLocation + instance / step, which
    // execute_draw places from the bias.
    capture_draw.instance_id_bias = instance;
    execute_draw(context, indexed, count, start_location, base_vertex, 1,
                 first_instance, 0, 0, &capture_draw);
    return capture_draw.submitted;
}

// -----------------------------------------------------------------------------
// Lines drawn as D3D's rectangles (graphics::generate_line_quad_vertex_msl)
// -----------------------------------------------------------------------------

u32 D3D11Manager::line_rasterization_mode(const ContextState& state) const {
    if (state.rasterizer_state == 0) return 0;
    const auto rs = m_rasterizer_states.find(state.rasterizer_state);
    if (rs == m_rasterizer_states.end()) return 0;
    // Learn D3D11_RASTERIZER_DESC (feature level 10.1+): MultisampleEnable
    // selects quadrilateral lines whatever AntialiasedLineEnable says;
    // AntialiasedLineEnable alone alpha-antialiased ones. WARP applies both
    // on one-sample targets too (d3d11_a07_rs_probe `R7`).
    if (rs->second.desc.multisample_enable != 0) return 2;
    if (rs->second.desc.antialiased_line_enable != 0) return 1;
    return 0;
}

TransientBufferRing::Buffer D3D11Manager::line_quad_params(
    const ContextState& state, u32 mode, u32 primitives) {
    graphics::LineQuadParams params{};
    // D3D11.3 3.4.5: quadrilateral lines are 1.4 wide; an alpha-antialiased
    // line's rectangle is 4 wide on WARP (measured: pixels at 2 from the
    // line are written, with alpha 0).
    params.half_width = mode == 1 ? 2.0f : 0.7f;
    params.primitives = primitives;
    if (state.rasterizer_state != 0) {
        const auto rs = m_rasterizer_states.find(state.rasterizer_state);
        if (rs != m_rasterizer_states.end()) {
            params.front_counter_clockwise =
                rs->second.desc.front_counter_clockwise != 0 ? 1u : 0u;
            // D3D11_CULL_NONE 1, FRONT 2, BACK 3.
            params.cull = rs->second.desc.cull_mode >= 2u
                ? rs->second.desc.cull_mode - 1u : 0u;
        }
    } else {
        params.cull = 2;  // the default state culls back faces
    }
    for (usize i = 0; i < params.viewports.size(); ++i) {
        // A viewport of no extent draws nothing; 1 keeps the shader's
        // division finite.
        params.viewports[i][0] = std::max(state.viewports[i][2], 1.0f);
        params.viewports[i][1] = std::max(state.viewports[i][3], 1.0f);
        params.viewports[i][2] = state.viewports[i][0];
        params.viewports[i][3] = state.viewports[i][1];
    }
    graphics::BackendBufferDesc desc{};
    desc.byte_width = sizeof(params);
    desc.shader_resource = true;
    return m_transient.acquire(desc, &params, sizeof(params));
}

bool D3D11Manager::execute_line_quads(GuestAddr context, bool indexed,
                                      u32 count, u32 start_location,
                                      i32 base_vertex, u32 instance_count,
                                      u32 first_instance, u32 mode) {
    auto& state = m_context_states[context];
    const auto decline = [](const char* why) {
        d3d11_unimplemented_once(
            "Draw", std::string("quadrilateral / alpha-antialiased lines "
                                "drawn aliased: ") + why);
        return false;
    };
    // Lines, lines with adjacency (their middle two vertices), or triangles
    // in WIREFRAME fill (their edges).
    u32 vpp = 0;
    graphics::PrimitiveAssembly assembly = graphics::PrimitiveAssembly::List;
    switch (state.primitive_topology) {
    case 2: vpp = 2; break;                                            // LINELIST
    case 3: vpp = 2; assembly = graphics::PrimitiveAssembly::Strip; break;
    case 10: vpp = 4; break;                                           // LINELIST_ADJ
    case 11: vpp = 4; assembly = graphics::PrimitiveAssembly::Strip; break;
    case 4: vpp = 3; break;                                            // TRIANGLELIST
    case 5: vpp = 3; assembly = graphics::PrimitiveAssembly::Strip; break;
    default:
        return decline("a triangle-with-adjacency topology in WIREFRAME fill");
    }
    auto vs = m_vertex_shaders.find(state.vertex_shader);
    if (vs == m_vertex_shaders.end()) return decline("no vertex shader");
    if (!ensure_vertex_capture(vs->second)) {
        return decline("the vertex shader cannot be captured");
    }
    auto msl = vs->second.line_quad_msl.find(vpp);
    if (msl == vs->second.line_quad_msl.end()) {
        // The captured records, as pass C reads them: the vertex shader's
        // output registers, ascending (vs capture_registers).
        std::vector<graphics::GsPassthroughOutput> outputs;
        const auto container = graphics::parse_dxbc(
            vs->second.bytecode.data(), vs->second.bytecode.size());
        bool expressible = container.valid();
        for (const auto& element : container.output_signature) {
            const u32 sv = element.system_value_type;
            if (sv == NAME_CULL_DISTANCE) expressible = false;
            auto same = std::find_if(outputs.begin(), outputs.end(),
                                     [&](const auto& o) {
                                         return o.register_num ==
                                                element.register_num;
                                     });
            if (same == outputs.end()) {
                graphics::GsPassthroughOutput o{};
                o.register_num = element.register_num;
                o.component_type = element.component_type;
                outputs.push_back(o);
                same = std::prev(outputs.end());
            }
            if (sv == NAME_POSITION) same->is_position = true;
            if (sv == NAME_CLIP_DISTANCE ||
                sv == NAME_RENDER_TARGET_ARRAY_INDEX ||
                sv == NAME_VIEWPORT_ARRAY_INDEX) {
                for (u32 c = 0; c < 4; ++c) {
                    if ((element.component_mask & (1u << c)) != 0u) {
                        same->component_system_values[c] =
                            static_cast<u8>(sv);
                    }
                }
            }
        }
        std::vector<u32> registers;
        for (const auto& o : graphics::gs_record_order(outputs)) {
            registers.push_back(o.register_num);
        }
        graphics::LineQuadSource source{};
        source.vertices_per_primitive = vpp;
        std::string generated =
            expressible && registers == vs->second.capture_registers
                ? graphics::generate_line_quad_vertex_msl(
                      outputs, source, /*fixed_function_push=*/true)
                : std::string{};
        msl = vs->second.line_quad_msl.emplace(vpp, std::move(generated))
                  .first;
    }
    if (msl->second.empty()) {
        return decline("the vertex shader's outputs cannot be drawn so "
                       "(no position, or cull distances)");
    }
    const std::string* vertex_msl = &msl->second;

    DirectCapture capture{};
    if (const char* why = prepare_direct_capture(
            state, indexed, count, start_location, base_vertex,
            instance_count, vpp, assembly,
            static_cast<u32>(vs->second.capture_registers.size()), capture)) {
        return decline(why);
    }
    if (capture.primitives == 0) return true;
    const auto params = line_quad_params(state, mode, capture.primitives);
    if (!params) return decline("no parameter buffer");
    const bool viewport_array = vs->second.writes_viewport_index;
    const u32 triangles = graphics::line_quad_triangles_per_primitive(vpp);
    for (u32 instance = 0; instance < instance_count; ++instance) {
        if (!capture_direct_instance(context, indexed, count, start_location,
                                     base_vertex, first_instance, instance,
                                     capture)) {
            d3d11_diag("line quads: the capture draw was not submitted");
            return true;
        }
        // The rectangles, primitive order kept; SV_PrimitiveID restarts
        // with each instance (D3D11.3 8.17), as each instance is its own
        // pass.
        DrawVariant output_draw{};
        output_draw.kind = DrawVariant::Kind::GeometryOutput;
        output_draw.buffer = capture.records.get();
        output_draw.offset =
            static_cast<unsigned>(capture.record_bytes * instance);
        output_draw.vertex_msl = vertex_msl;
        output_draw.counts = capture.ids.get();
        output_draw.vertex_count = capture.primitives * triangles * 3u;
        output_draw.viewport_array = viewport_array;
        output_draw.topology = graphics::BackendPrimitiveTopology::TriangleList;
        output_draw.line_params = params.get();
        output_draw.line_quad_divisor = triangles;
        output_draw.line_coverage_alpha = mode == 1;
        execute_draw(context, false, 0, 0, 0, 1, 0, 0, 0, &output_draw);
        if (!output_draw.submitted) {
            d3d11_diag("line quads: the rectangle draw was not submitted");
            return true;
        }
    }
    return true;
}

void D3D11Manager::execute_geometry_indirect(
    GuestAddr context, bool indexed, const IndirectDraw& indirect,
    const std::shared_ptr<graphics::BackendComputePipeline>& kernel,
    const std::vector<u32>& record_layout,
    const std::shared_ptr<graphics::BackendBuffer>& stats) {
    auto& state = m_context_states[context];
    const GuestAddr geometry_shader = state.geometry_shader;
    const auto gs_it = m_geometry_shaders.find(geometry_shader);
    if (gs_it == m_geometry_shaders.end()) return;
    const InputTopology topology = input_topology(state.primitive_topology);
    const u32 vpp = topology.vertices_per_primitive;
    // What one primitive can expand to bounds how many the passes hold.
    const GeometryShader& gs = gs_it->second;
    u32 streams = 0;
    for (u32 s = 0; s < 4; ++s) {
        if ((gs.stream_mask & (1u << s)) != 0) streams = s + 1;
    }
    const u64 per_primitive =
        static_cast<u64>(gs.instance_count) *
        graphics::gs_vertices_per_invocation(gs.output_topology,
                                             gs.max_output_vertices) *
        std::max(gs.output_record_vec4s, 1u) * 16u * std::max(streams, 1u);
    const u64 max_primitives =
        per_primitive == 0 ? 0 : MAX_GEOMETRY_BYTES / per_primitive;
    IndirectCapture captured{};
    if (!capture_indirect(context, indexed, indirect, vpp,
                          topology.strip_adjacency ? 2u
                          : topology.strip         ? 1u
                                                   : 0u,
                          static_cast<u32>(record_layout.size()),
                          max_primitives, captured)) {
        return;
    }
    GeometryInput in{};
    in.records = captured.capture.get();
    in.records_offset = graphics::CAPTURE_HEADER_BYTES;
    in.ids = captured.ids.get();
    in.vertices_per_primitive = vpp;
    in.max_primitives = captured.primitive_capacity;
    in.primitive_count = captured.resolved.get();
    in.primitive_count_offset = graphics::IA_RESOLVED_PRIMITIVES * sizeof(u32);
    // SV_PrimitiveID restarts with every instance (D3D11.3 8.17).
    in.primitives_per_instance_count = captured.resolved.get();
    in.primitives_per_instance_offset =
        graphics::IA_RESOLVED_PRIMITIVES_PER_INSTANCE * sizeof(u32);
    (void)geometry_expand(context, geometry_shader, kernel, in, stats);
}

bool D3D11Manager::execute_geometry_direct(
    GuestAddr context, bool indexed, u32 count, u32 start_location,
    i32 base_vertex, u32 instance_count, u32 first_instance,
    const std::shared_ptr<graphics::BackendComputePipeline>& kernel,
    const std::vector<u32>& record_layout,
    const std::shared_ptr<graphics::BackendBuffer>& stats) {
    auto& state = m_context_states[context];
    const auto decline = [](const char* why) {
        d3d11_diag("geometry shader draw: nothing drawn: %s", why);
        return false;
    };
    const InputTopology topology = input_topology(state.primitive_topology);
    const u32 vpp = topology.vertices_per_primitive;
    const u64 record_bytes = static_cast<u64>(record_layout.size()) * 16u;
    if (vpp == 0 || record_bytes == 0) return decline("nothing to capture");

    // Primitive assembly on the CPU, laid out as the GPU resolve of an
    // indirect draw lays it out (graphics::ia_resolve_model is that kernel's
    // C++ form): record keys draw-relative -- the vertex index for a
    // non-indexed draw, the index value for an indexed one (D3D11.3 8.16) --
    // plus instance x stride, primitives instance-major, SV_PrimitiveID
    // restarting with each instance (8.17).
    CaptureExtent extent{};
    if (const char* why = capture_extent(state, indexed, count, start_location,
                                         base_vertex, extent)) {
        return decline(why);
    }
    std::vector<u32> values;
    if (!draw_indices(state, indexed, count, extent, values)) {
        return decline("the index data is not readable on the CPU");
    }
    graphics::IaResolveParams params{};
    params.indexed = indexed ? 1u : 0u;
    params.index_32bit = extent.index_mode == 2u ? 1u : 0u;
    params.index_first = 0;
    params.index_elements = static_cast<u32>(values.size());
    params.vertices_per_primitive = vpp;
    params.assembly = topology.strip_adjacency ? 2u : topology.strip ? 1u : 0u;
    // The same ceilings as the per-instance path: a draw that cannot fit is
    // declined whole, never drawn in part.
    params.record_capacity = static_cast<u32>(std::min<u64>(
        (MAX_GEOMETRY_BYTES - graphics::CAPTURE_HEADER_BYTES) / record_bytes,
        0xFFFFFFFFull));
    params.primitive_capacity = static_cast<u32>(
        MAX_GEOMETRY_BYTES / (4u * static_cast<u64>(vpp)));
    // The argument block an equivalent DrawInstancedIndirect /
    // DrawIndexedInstancedIndirect would carry; the values were read from
    // `values` (the draw's own indices), so they start at 0.
    const std::array<u32, 5> args =
        indexed ? std::array<u32, 5>{count, instance_count, 0u,
                                     static_cast<u32>(base_vertex),
                                     first_instance}
                : std::array<u32, 5>{count, instance_count, start_location,
                                     first_instance, 0u};
    std::vector<u32> ids;
    const auto resolved = graphics::ia_resolve_model(params, args, values, ids);
    if (resolved[graphics::IA_RESOLVED_OVERFLOW] != 0) {
        return decline("the capture would exceed the size limit");
    }
    const u32 primitives = resolved[graphics::IA_RESOLVED_PRIMITIVES];
    if (primitives == 0) return true;
    const u32 keys = resolved[graphics::IA_RESOLVED_VERTICES];

    if (!m_stream_out_target) {
        graphics::BackendTexture2DDesc td{};
        td.width = td.height = 1;
        td.mip_levels = td.array_size = 1;
        td.pixel_format = graphics::PixelFormat::RGBA8Unorm;
        td.render_target = true;
        m_stream_out_target = m_backend.create_texture_2d(td, nullptr, 0);
    }
    if (!m_stream_out_target) return decline("capture target unavailable");
    // The capture buffer opens with the {keys per instance, capacity} header
    // the draw-relative capture reads (graphics::CAPTURE_HEADER_BYTES);
    // records follow, every one the expansion reads written by the capture.
    graphics::BackendBufferDesc storage{};
    storage.shader_resource = true;
    storage.unordered_access = true;
    storage.byte_width = static_cast<usize>(graphics::CAPTURE_HEADER_BYTES +
                                            static_cast<u64>(keys) * record_bytes);
    const std::array<u32, 4> header{resolved[graphics::IA_RESOLVED_KEY_STRIDE],
                                    keys, 0u, 0u};
    static_assert(sizeof(header) == graphics::CAPTURE_HEADER_BYTES);
    auto records = m_transient.acquire(storage, header.data(), sizeof(header));
    graphics::BackendBufferDesc ids_desc{};
    ids_desc.shader_resource = true;
    ids_desc.byte_width = ids.size() * sizeof(u32);
    auto id_buffer = m_transient.acquire(ids_desc, ids.data(),
                                         ids_desc.byte_width);
    // An indexed capture runs the draw's index values with the strip-cut
    // values left out (they name no vertex, D3D11.3 8.12): nothing reads
    // their records, and a list draw would otherwise fetch vertex 0xFFFF.
    TransientBufferRing::Buffer capture_indices;
    std::vector<u32> kept;
    if (indexed) {
        const u32 cut = extent.index_mode == 2u ? 0xFFFFFFFFu : 0xFFFFu;
        for (const u32 v : values) {
            if (v != cut) kept.push_back(v);
        }
        graphics::BackendBufferDesc index_desc{};
        index_desc.shader_resource = true;
        index_desc.index_buffer = true;
        index_desc.byte_width = std::max<usize>(kept.size(), 1u) * sizeof(u32);
        capture_indices = m_transient.acquire(index_desc, kept.data(),
                                              kept.size() * sizeof(u32));
    }
    if (!records || !id_buffer || (indexed && !capture_indices)) {
        return decline("primitive assembly buffers could not be allocated");
    }

    // A: every instance's vertices in one capture draw, at the draw's own
    // StartInstanceLocation (per-instance elements step as in the real draw).
    DrawVariant capture_draw{};
    capture_draw.kind = DrawVariant::Kind::Capture;
    capture_draw.buffer = records.get();
    capture_draw.offset = 0;
    capture_draw.target = m_stream_out_target.get();
    capture_draw.capture_relative = true;
    if (indexed) {
        capture_draw.capture_indices = capture_indices.get();
        capture_draw.capture_index_count = static_cast<u32>(kept.size());
    }
    if (!indexed || !kept.empty()) {
        execute_draw(context, indexed, count, start_location, base_vertex,
                     instance_count, first_instance, 0, 0, &capture_draw);
        if (!capture_draw.submitted) {
            return decline("the capture draw was not submitted");
        }
    }
    // B + C: one expansion over (primitive x instance), one draw.
    GeometryInput in{};
    in.records = records.get();
    in.records_offset = graphics::CAPTURE_HEADER_BYTES;
    in.ids = id_buffer.get();
    in.vertices_per_primitive = vpp;
    in.max_primitives = primitives;
    in.primitives_per_instance =
        resolved[graphics::IA_RESOLVED_PRIMITIVES_PER_INSTANCE];
    return geometry_expand(context, state.geometry_shader, kernel, in, stats);
}

}  // namespace vitreo::winapi
