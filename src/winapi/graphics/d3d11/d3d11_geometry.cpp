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
/// the emulation cannot express: shaders the translator cannot lower, each
/// with the translator's reason (SV_CullDistance is culled in pass C). A
/// draw it cannot serve -- a topology that does not match the shader's input
/// primitive -- draws nothing, with a diagnostic naming why.

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
#include <cstring>
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
    if (!lowered.valid()) {
        why = lowered.failure;
        return lowered.invalid_program ? d3d11::E_INVALIDARG
                                       : d3d11::E_NOTIMPL;
    }
    if (!lowered.gs_lowered_to_compute) {
        why = "the geometry shader translated without its compute lowering";
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
        // SV_CullDistance culls in pass C, which sees each expanded
        // primitive whole (generate_gs_passthrough_vertex_msl).
        if (sv == NAME_CLIP_DISTANCE || sv == NAME_CULL_DISTANCE ||
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
                         /*fixed_function_push=*/true, /*dense_records=*/false,
                         lowered.gs_output_topology == 1u   ? 1u
                         : lowered.gs_output_topology == 3u ? 2u
                                                            : 3u)
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
    // ---- WS5 begin ----
    // Whether this program can be drawn as a Metal mesh: one rasterized
    // stream with a position, no UAVs (a mesh draw binds none on its geometry
    // stage), and a shape inside Apple's mesh limits; the library must lower
    // and compile. Creation never fails here -- an ineligible program keeps
    // the three-pass path with its reason.
    shader.mesh = {};
    shader.mesh_msl.clear();
    shader.mesh_ps_link.clear();
    if (rasterized_stream != 0 || lowered.gs_stream_mask != 1u) {
        shader.mesh.why = "more than one stream, or a rasterized stream other than 0";
    } else if (shader.passthrough_msl.empty()) {
        shader.mesh.why = "nothing to rasterize";
    } else if (layout.uav_flat != 0 || layout.uav_arrayed != 0) {
        shader.mesh.why = "the geometry shader declares UAVs";
    } else if (lowered.gs_output_topology == 1u) {
        // Neither pass C nor a mesh writes [[point_size]], and Metal leaves
        // the size of such a point to the stage; nothing proves the two
        // stages agree, so point output keeps the three passes.
        shader.mesh.why = "point output";
    } else {
        const graphics::GsMeshFit fit = graphics::gs_mesh_fit(
            outputs, lowered.gs_output_topology, lowered.gs_max_output_vertices);
        if (!fit.fits) {
            shader.mesh.why = fit.why;
        } else {
            graphics::GsMeshLayout mesh_layout{};
            mesh_layout.outputs = outputs;
            mesh_layout.output_topology = lowered.gs_output_topology;
            mesh_layout.vertices_per_invocation = vertices_per_invocation;
            mesh_layout.invocations_per_group = fit.invocations_per_group;
            mesh_layout.record_vec4s = lowered.gs_output_registers;
            std::string mesh_msl;
            if (!compile_geometry_mesh(bytes, {}, mesh_layout, mesh_msl)) {
                shader.mesh.why = "the mesh library does not compile";
            } else {
                shader.mesh.eligible = true;
                shader.mesh.why = nullptr;
                shader.mesh.fit = fit;
                shader.mesh.layout = std::move(mesh_layout);
            }
        }
    }
    // ---- WS5 end ----
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

    // ---- WS5 begin ----
    // The mesh path (host-offload plan WS5):
    // the capture runs in the guest's own pass, then one mesh draw expands
    // and rasterizes in the same encoder. select_gs_path keeps everything it
    // cannot draw bit-identically on the three passes below.
    // The shader's own copy (GeometryShader::mesh_msl, a node-stable map):
    // referenced, not copied, so the draw neither copies the source nor
    // hashes it again for the pipeline key (msl_hash memoises by address).
    const std::string* mesh_msl = nullptr;
    if (auto gs_it = m_geometry_shaders.find(state.geometry_shader);
        gs_it != m_geometry_shaders.end() && gs_it->second.mesh.eligible) {
        GeometryShader& gs = gs_it->second;
        const gs_mesh::GsPathChoice choice = gs_mesh::select_gs_path(
            gs.mesh, gs_mesh_draw_facts(context, gs, /*indirect=*/false,
                                        u64{primitives} * gs.instance_count));
        if (choice.path == gs_mesh::GsPath::Mesh) {
            const u32 minmax = minmax_sampler_mask(state.geometry_samplers);
            const std::vector<u32> key =
                kernel_variant_key(record_layout, minmax);
            auto built = gs.mesh_msl.find(key);
            if (built == gs.mesh_msl.end()) {
                std::string msl;
                if (!compile_geometry_mesh(gs.bytecode, record_layout,
                                           gs.mesh.layout, msl, minmax)) {
                    msl.clear();
                }
                built = gs.mesh_msl.emplace(key, std::move(msl)).first;
            }
            if (!built->second.empty()) mesh_msl = &built->second;
        } else {
            d3d11_diag("GS draw three-pass: %s", choice.why);
        }
    }
    // ---- WS5 end ----
    // A: every instance's vertices in one capture draw, at the draw's own
    // StartInstanceLocation (per-instance elements step as in the real draw).
    DrawVariant capture_draw{};
    capture_draw.kind = DrawVariant::Kind::Capture;
    capture_draw.buffer = records.get();
    capture_draw.offset = 0;
    capture_draw.target = m_stream_out_target.get();
    capture_draw.capture_in_pass = mesh_msl != nullptr;  // WS5
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
    // ---- WS5 begin ----
    if (mesh_msl != nullptr) {
        bool refused = false;
        if (geometry_mesh_draw(context, m_context_states[context].geometry_shader,
                               *mesh_msl, in, refused)) {
            return true;
        }
        // Not drawn (a pipeline that cannot be built, memoised, or a draw
        // the backend declined): the captured records are exactly what the
        // three-pass expansion reads, so it draws them instead.
        d3d11_diag("GS mesh draw not submitted (%s); three-pass",
                      refused ? "pipeline refused" : "declined");
    }
    // ---- WS5 end ----
    return geometry_expand(context, m_context_states[context].geometry_shader,
                           kernel, in, stats);
}

// ---- WS5 begin ----
namespace {

/// DXGI formats whose Min/Max blending gives the same bits in any order:
/// blendable UNORM (dxgiformat.h values). Float (-0/+0, NaN), SNORM (two
/// encodings of -1), sRGB and integer formats are left out.
bool minmax_exact_dxgi_format(u32 format) {
    switch (format) {
    case 11:   // R16G16B16A16_UNORM
    case 24:   // R10G10B10A2_UNORM
    case 28:   // R8G8B8A8_UNORM
    case 35:   // R16G16_UNORM
    case 49:   // R8G8_UNORM
    case 56:   // R16_UNORM
    case 61:   // R8_UNORM
    case 65:   // A8_UNORM
    case 85:   // B5G6R5_UNORM
    case 86:   // B5G5R5A1_UNORM
    case 87:   // B8G8R8A8_UNORM
    case 88:   // B8G8R8X8_UNORM
    case 115:  // B4G4R4A4_UNORM
        return true;
    default:
        return false;
    }
}

// D3D_NAME values a pixel shader may read.
constexpr u32 NAME_PRIMITIVE_ID = 7;

}  // namespace

gs_mesh::GsDrawFacts D3D11Manager::gs_mesh_draw_facts(GuestAddr context,
                                                      GeometryShader& gs,
                                                      bool indirect,
                                                      u64 invocations) {
    const auto& state = m_context_states[context];
    gs_mesh::GsDrawFacts f{};
    f.device_supports_mesh = m_backend.supports_mesh_pipelines();
    f.indirect = indirect;
    f.invocations = invocations;
    f.statistics_query = pipeline_statistics_building(context);
    f.stream_output = !gs.stream_out.empty();
    bool wireframe = false;
    if (const auto rs = m_rasterizer_states.find(state.rasterizer_state);
        state.rasterizer_state != 0 && rs != m_rasterizer_states.end()) {
        wireframe = rs->second.desc.fill_mode == 2u;  // D3D11_FILL_WIREFRAME
    }
    f.wireframe = wireframe;
    f.line_quads = line_rasterization_mode(state) != 0 &&
                   (gs.output_topology == 3 || (gs.output_topology == 5 && wireframe));

    // The pixel shader: what it reads, and whether it writes memory or coverage.
    if (const auto ps = m_pixel_shaders.find(state.pixel_shader);
        state.pixel_shader != 0 && ps != m_pixel_shaders.end()) {
        f.ps_uav = ps->second.binding_layout.uav_flat != 0 ||
                   ps->second.binding_layout.uav_arrayed != 0;
        f.ps_writes_coverage = ps->second.writes_coverage;
        if (ps->second.msl_source_hash == 0) {
            ps->second.msl_source_hash = std::hash<std::string>{}(ps->second.msl_source);
        }
        u8& link = gs.mesh_ps_link[ps->second.msl_source_hash];
        if ((link & 1u) == 0) {
            link = 1u | 2u;
            const auto container = graphics::parse_dxbc(ps->second.bytecode.data(),
                                                        ps->second.bytecode.size());
            if (!container.valid()) link &= ~2u;
            for (const auto& e : container.input_signature) {
                const u32 sv = e.system_value_type;
                if (sv == NAME_POSITION) continue;
                if (sv == NAME_PRIMITIVE_ID) link |= 4u;
                if (sv == NAME_RENDER_TARGET_ARRAY_INDEX || sv == NAME_VIEWPORT_ARRAY_INDEX) {
                    // Refused only when the geometry shader does not write
                    // the value (D3D_NAME 4 / 5 on a delivered output): the
                    // mesh has no such per-primitive field then.
                    const u8 name = static_cast<u8>(sv);
                    const bool gs_writes = std::any_of(
                        gs.passthrough_outputs.begin(), gs.passthrough_outputs.end(),
                        [&](const graphics::GsPassthroughOutput& o) {
                            return o.delivered &&
                                   std::find(o.component_system_values.begin(),
                                             o.component_system_values.end(),
                                             name) != o.component_system_values.end();
                        });
                    if (!gs_writes) link |= 8u;
                }
                if (sv != 0 && sv != NAME_CLIP_DISTANCE) continue;  // other SVs are PS-side
                const bool written = std::any_of(
                    gs.passthrough_outputs.begin(), gs.passthrough_outputs.end(),
                    [&](const graphics::GsPassthroughOutput& o) {
                        return o.delivered && !o.is_position &&
                               o.register_num == e.register_num;
                    });
                if (!written) link &= ~2u;
            }
        }
        f.ps_inputs_covered = (link & 2u) != 0;
        f.ps_reads_builtin_primitive_id = (link & 4u) != 0;
        f.ps_reads_builtin_layer_or_viewport = (link & 8u) != 0;
    }

    // The output merger. A null blend state is the default: no blending,
    // every channel written.
    const auto blend = m_blend_states.find(state.blend_state);
    const bool have_blend = state.blend_state != 0 && blend != m_blend_states.end();
    if (have_blend) {
        f.alpha_to_coverage = blend->second.desc.alpha_to_coverage_enable != 0;
    }
    for (u32 slot = 0; slot < 8; ++slot) {
        if (state.render_targets[slot] == 0) continue;
        const auto rtv = m_render_target_views.find(state.render_targets[slot]);
        if (rtv == m_render_target_views.end()) continue;
        gs_mesh::RenderTargetFacts& rt = f.rts[slot];
        rt.bound = true;
        rt.minmax_exact = minmax_exact_dxgi_format(rtv->second.format);
        rt.write_mask = 0xF;
        if (have_blend) {
            const auto& d = blend->second.desc;
            const u32 i = d.independent_blend_enable != 0 ? slot : 0u;
            rt.write_mask = static_cast<u8>(d.render_target_write_mask[i] & 0xFu);
            rt.blend = d.blend_enable[i] != 0;
            rt.op_color = static_cast<u8>(d.blend_op[i]);
            rt.op_alpha = static_cast<u8>(d.blend_op_alpha[i]);
            rt.logic_op = d.logic_op_enable[0] != 0;
        }
    }
    // Depth and stencil apply only with a depth-stencil view bound. A null
    // state is the default: depth on, writes on, LESS, stencil off.
    if (state.depth_stencil_view != 0) {
        f.depth_enable = true;
        f.depth_write = true;
        f.depth_func = gs_mesh::kComparisonLess;
        if (const auto dss = m_depth_stencil_states.find(state.depth_stencil_state);
            state.depth_stencil_state != 0 && dss != m_depth_stencil_states.end()) {
            const auto& d = dss->second.desc;
            f.depth_enable = d.depth_enable != 0;
            f.depth_write = d.depth_write_mask != 0;
            f.depth_func = static_cast<u8>(d.depth_func);
            f.stencil_enable = d.stencil_enable != 0;
            f.stencil_write_mask = d.stencil_write_mask;
            f.stencil_ops = {static_cast<u8>(d.front_stencil_fail_op),
                             static_cast<u8>(d.front_stencil_depth_fail_op),
                             static_cast<u8>(d.front_stencil_pass_op),
                             static_cast<u8>(d.back_stencil_fail_op),
                             static_cast<u8>(d.back_stencil_depth_fail_op),
                             static_cast<u8>(d.back_stencil_pass_op)};
        }
    }
    return f;
}

bool D3D11Manager::bind_geometry_mesh_stage(GuestAddr context,
                                            const DrawVariant& variant,
                                            graphics::BackendDrawCall& call,
                                            float depth_offset,
                                            std::vector<u32>& push,
                                            graphics::BackendMeshDraw& mesh) {
    const auto& state = m_context_states[context];
    const auto gs_it = m_geometry_shaders.find(state.geometry_shader);
    if (gs_it == m_geometry_shaders.end()) return false;
    const d3d11::ShaderBindingLayout& layout = gs_it->second.binding_layout;
    if (layout.uses_descriptor_tables) return false;
    // Nothing of the vertex shader's: the mesh stage runs the geometry
    // program, and the call's vertex-stage fields are what it binds.
    call.vertex_constant_buffers.fill(nullptr);
    call.vertex_constant_buffer_offsets.fill(0);
    call.vertex_textures.fill(nullptr);
    call.vertex_texture_views.fill({});
    call.vertex_textures_3d.fill(nullptr);
    call.vertex_samplers.fill({});
    call.vertex_srv_buffers.fill(nullptr);
    call.vertex_srv_buffer_offsets.fill(0);
    call.vertex_srv_texture_buffers.fill(nullptr);
    call.vertex_descriptor_tables.fill(nullptr);
    call.vertex_border_twin_mask = 0;
    call.vertex_capture_buffer = nullptr;
    call.vertex_buffer = nullptr;
    call.vertex_buffers.fill(nullptr);
    call.index_buffer = nullptr;
    call.index_count = 0;
    call.vertex_count = 0;
    call.indirect.buffer = nullptr;
    // b#, t#, s#: as bind_compute_stage binds them for the expansion kernel.
    for (u32 slot = 0; slot < graphics::BACKEND_MAX_CONSTANT_BUFFER_SLOTS; ++slot) {
        const auto buffer = m_buffers.find(state.geometry_constant_buffers[slot]);
        if (state.geometry_constant_buffers[slot] == 0 || buffer == m_buffers.end()) continue;
        call.vertex_constant_buffers[slot] = buffer->second.backend.get();
        call.vertex_constant_buffer_offsets[slot] = constant_buffer_bind_offset(
            state.geometry_constant_buffers[slot], state.geometry_constant_first[slot]);
    }
    for (u32 slot = 0; slot < graphics::BACKEND_MAX_TEXTURE_SLOTS; ++slot) {
        const GuestAddr address = state.geometry_shader_resources[slot];
        if (const auto srv = m_shader_resource_views.find(address);
            address != 0 && srv != m_shader_resource_views.end()) {
            if (srv->second.texture_buffer != nullptr) {
                call.vertex_srv_texture_buffers[slot] = srv->second.texture_buffer.get();
            }
            if (srv->second.buffer != 0 && srv->second.texture_buffer == nullptr) {
                if (const auto buffer = m_buffers.find(srv->second.buffer);
                    buffer != m_buffers.end()) {
                    call.vertex_srv_buffers[slot] = buffer->second.backend.get();
                    call.vertex_srv_buffer_offsets[slot] =
                        srv->second.first_element * srv->second.element_bytes;
                }
            }
            resolve_shader_resource(address, call.vertex_textures[slot],
                                    call.vertex_texture_views[slot],
                                    call.vertex_textures_3d[slot]);
            if (call.vertex_textures[slot] != nullptr &&
                !d3d11::fit_view_to_declaration(layout, false, slot,
                                                call.vertex_texture_views[slot])) {
                call.vertex_textures[slot] = nullptr;
            }
        }
        (void)resolve_sampler(state.geometry_samplers[slot], call.vertex_samplers[slot],
                              nullptr);
    }
    // The expansion window: records, control block, assembled ids.
    mesh = {};
    mesh.expansion_buffers = {variant.buffer, nullptr, variant.counts,
                              variant.index_buffer};
    mesh.expansion_offsets = {variant.offset, 0u, 0u, variant.index_offset};
    mesh.mesh_groups = variant.mesh_groups;
    mesh.object_threadgroups =
        (variant.mesh_groups + graphics::kMaxMeshGroupsPerObjectGroup - 1) /
        graphics::kMaxMeshGroupsPerObjectGroup;
    mesh.barrier_before_mesh = true;
    call.mesh = &mesh;
    // The push block geometry_expand binds for the kernel, plus member 7.
    push = sampler_push_block(state.geometry_samplers);
    if (push.size() * sizeof(u32) < graphics::kPushBlockBytes) {
        push.resize(graphics::kPushBlockBytes / sizeof(u32), 0u);
    }
    fill_constant_buffer_limits(push, state.geometry_constant_buffers,
                                state.geometry_constant_first,
                                state.geometry_constant_count);
    fill_typed_buffer_biases(push, state.geometry_shader_resources);
    std::memcpy(push.data() + graphics::kFixedFunctionOffset / sizeof(u32),
                &depth_offset, sizeof(depth_offset));
    call.vertex_constants = push.data();
    call.vertex_constants_size = push.size() * sizeof(u32);
    return true;
}

bool D3D11Manager::geometry_mesh_draw(GuestAddr context,
                                      GuestAddr geometry_shader,
                                      const std::string& mesh_msl,
                                      const GeometryInput& in, bool& refused) {
    refused = false;
    const auto gs_it = m_geometry_shaders.find(geometry_shader);
    if (gs_it == m_geometry_shaders.end() || !gs_it->second.mesh.eligible) {
        return false;
    }
    const GeometryShader& gs = gs_it->second;
    const u64 invocations = in.max_primitives * gs.instance_count;
    if (invocations == 0) return true;
    const u32 k = gs.mesh.fit.invocations_per_group;
    const u64 groups = (invocations + k - 1) / k;
    if (invocations > 0xFFFFFFFFull || groups > 0xFFFFFFFFull) return false;
    // The control block geometry_expand writes (no count region: the counts
    // are threadgroup memory). The invocation limit is always on, since the
    // last threadgroup holds threads past the last invocation.
    std::array<u32, graphics::kGsInvocationCountBase> control{};
    control[graphics::kGsControlFlags] =
        graphics::kGsFlagInvocationLimit |
        (in.primitives_per_instance != 0 ? graphics::kGsFlagPrimitiveIdModulo : 0u) |
        (in.primitive_id_table ? graphics::kGsFlagPrimitiveIdTable : 0u);
    control[1] = 2u;
    control[2] = 0u;
    control[3] = 0u;
    control[4] = in.vertices_per_primitive;
    control[graphics::kGsControlInvocationLimit] = static_cast<u32>(invocations);
    control[graphics::kGsControlPrimitivesPerInstance] = in.primitives_per_instance;
    control[graphics::kGsControlStreamStride] = static_cast<u32>(invocations);
    control[graphics::kGsControlPrimitiveIdTable] = in.primitive_id_table_offset;
    graphics::BackendBufferDesc desc{};
    desc.shader_resource = true;
    desc.unordered_access = true;
    desc.byte_width = sizeof(control);
    auto block = m_transient.acquire(desc, control.data(), sizeof(control));
    if (!block) return false;

    DrawVariant mesh_draw{};
    mesh_draw.kind = DrawVariant::Kind::MeshGeometry;
    mesh_draw.vertex_msl = &mesh_msl;
    mesh_draw.vertex_msl_owned = true;  // the geometry shader's own string
    mesh_draw.buffer = in.records;
    mesh_draw.offset = in.records_offset;
    mesh_draw.counts = block.get();
    mesh_draw.index_buffer = in.ids;
    mesh_draw.index_offset = in.ids_offset;
    mesh_draw.topology = list_topology(gs.output_topology);
    mesh_draw.viewport_array = gs.writes_viewport_index;
    mesh_draw.mesh_groups = static_cast<u32>(groups);
    mesh_draw.mesh_threads = k;
    execute_draw(context, false, 0, 0, 0, 1, 0, 0, 0, &mesh_draw);
    refused = mesh_draw.mesh_refused;
    if (mesh_draw.submitted) ++m_gs_mesh_draws;
    return mesh_draw.submitted;
}
// ---- WS5 end ----

}  // namespace vitreo::winapi
