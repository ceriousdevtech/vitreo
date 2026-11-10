// SPDX-License-Identifier: LGPL-2.1-only

/// @file dxbc_to_spirv_mesh.cpp
/// @brief D3D12 mesh and amplification shaders (mesh_stage.hpp): the
/// MeshOp extension instructions (dxbc_vitreo_ext.hpp) of a lowered
/// compute program with a VXMS chunk.
///
/// The program body runs as a function. `main` clears the group's output
/// counts (mesh) or mesh grid (amplification) on thread 0, calls the body,
/// waits for the whole threadgroup and ends in a Vitreo.Mesh epilogue
/// marker that spirv_to_msl's mesh compiler expands into the metal::mesh
/// writes or the object payload and grid.
///
/// DirectX-Specs "Mesh Shader": SetMeshOutputCounts and DispatchMesh read
/// their arguments "from the first thread" (both are called from uniform
/// flow, so thread 0); output writes may come from any thread, a later
/// write of the same element wins ("Shared Output Arrays"); writes past
/// the declared array sizes are undefined and are dropped here.

#include "graphics/shaders/dxbc_to_spirv_internal.hpp"
#include "graphics/shaders/dxbc_vitreo_ext.hpp"

#include <cstring>

namespace vitreo::graphics::dxbc_spirv_detail {

using namespace dxbc_vitreo_ext;

namespace {

constexpr u32 kScopeWorkgroup = 2u;
constexpr u32 kAcquireReleaseWorkgroupMemory = 0x8u | 0x100u;

}  // namespace

bool DxbcTranslator::emit_mesh_stage_declarations() {
    const DxbcMeshState& state = m_container.mesh;
    if (state.is_amplification()) {
        m_mesh_default_layout = amplification_stage_layout(m_container);
        m_mesh_layout = &m_mesh_default_layout;
    } else if (m_options.mesh_layout != nullptr) {
        m_mesh_layout = m_options.mesh_layout;
    } else {
        std::string why;
        bool invalid = false;
        if (!build_mesh_stage_layout(m_container, nullptr, m_mesh_default_layout, why, invalid)) {
            return fail("mesh shader: " + why);
        }
        m_mesh_layout = &m_mesh_default_layout;
    }
    const auto array = [&](u32 words, const char* name) {
        const u32 type = m_builder.emit_type_array(
            m_type_uint, m_builder.emit_constant_int(m_type_uint, std::max(words, 1u)));
        const u32 var = m_builder.emit_variable(
            m_builder.emit_type_pointer(SpvStorageClass::Workgroup, type), SpvStorageClass::Workgroup);
        m_builder.emit_name(var, name);
        return var;
    };
    const MeshStageLayout& l = *m_mesh_layout;
    if (state.is_mesh()) {
        const u32 nv = std::max(state.max_vertices, 1u);
        const u32 np = std::max(state.max_primitives, 1u);
        m_mesh_vertices = array(nv * std::max(l.vertex_scalars, 1u), "vitreo_ms_vertices");
        m_mesh_primitives = array(np * std::max(l.primitive_scalars, 1u), "vitreo_ms_primitives");
        m_mesh_indices = array(np * l.vertices_per_primitive(), "vitreo_ms_indices");
        m_mesh_counts = array(2u, "vitreo_ms_counts");
        if (state.payload_words != 0) m_mesh_payload = array(state.payload_words, "vitreo_ms_payload");
        // The epilogue applies the clip-space depth offset, as every
        // translated vertex stage does (kFixedFunctionOffset).
        (void)push_block_var();
    } else {
        m_mesh_grid = array(3u, "vitreo_as_grid");
    }
    return true;
}

u32 DxbcTranslator::mesh_scalar(const Sm4Operand& operand, bool& ok) {
    u32 bits = 0;
    ok = load_bits(operand, bits);
    if (!ok) return 0;
    return m_builder.emit_composite_extract(m_type_uint, bits, {0u});
}

void DxbcTranslator::mesh_if(u32 condition, const std::function<void()>& then) {
    const u32 then_label = m_builder.allocate_id();
    const u32 merge = m_builder.allocate_id();
    m_builder.emit_selection_merge(merge);
    m_builder.emit_branch_conditional(condition, then_label, merge);
    m_builder.emit_label_id(then_label);
    then();
    m_builder.emit_branch(merge);
    m_builder.emit_label_id(merge);
}

void DxbcTranslator::mesh_store_word(u32 var, u32 index, u32 value) {
    m_builder.emit_store(
        m_builder.emit_access_chain(m_builder.emit_type_pointer(SpvStorageClass::Workgroup, m_type_uint), var,
                                    {index}),
        value);
}

bool DxbcTranslator::translate_vx_mesh(u32 control) {
    if (!mesh_stage() || m_mesh_layout == nullptr) return fail("VX_MESH outside a mesh or amplification shader");
    const MeshStageLayout& l = *m_mesh_layout;
    const DxbcMeshState& state = m_container.mesh;
    const auto op = static_cast<MeshOp>(control & 0xFFu);
    const auto c = [&](u32 v) { return m_builder.emit_constant_int(m_type_uint, v); };
    const auto lt = [&](u32 a, u32 b) { return m_builder.emit_integer_compare(SpvOp::OpULessThan, m_type_bool, a, b); };
    const auto eq = [&](u32 a, u32 b) { return m_builder.emit_integer_compare(SpvOp::OpIEqual, m_type_bool, a, b); };
    const auto first_thread = [&]() { return eq(m_builder.emit_load(m_type_uint, m_group_index), c(0)); };
    const auto decode = [&](std::initializer_list<u32*> out) {
        for (u32* v : out) {
            Sm4Operand o;
            bool ok = m_decoder.decode_operand(o);
            if (ok) *v = mesh_scalar(o, ok);
            if (!ok) return false;
        }
        return true;
    };
    const bool ms = state.is_mesh();
    switch (op) {
    case MeshOp::SetCounts: {
        u32 nv = 0, np = 0;
        if (!ms || !decode({&nv, &np})) return fail("VX_MESH SetCounts: bad operands");
        mesh_if(first_thread(), [&] {
            mesh_store_word(m_mesh_counts, c(0), nv);
            mesh_store_word(m_mesh_counts, c(1), np);
        });
        return true;
    }
    case MeshOp::StoreVertex:
    case MeshOp::StorePrimitive: {
        const bool primitive = op == MeshOp::StorePrimitive;
        const bool dynamic = (control & kMeshDynamicRow) != 0;
        u32 index = 0, value = 0, row = 0;
        if (!ms || !decode({&index, &value}) || (dynamic && !decode({&row}))) {
            return fail("VX_MESH store: bad operands");
        }
        const u32 reg = (control >> 8) & 0xFFu;
        const u32 comp = (control >> 16) & 3u;
        const u32 rows = (control >> 18) & 63u;
        const u32 count = primitive ? std::max(state.max_primitives, 1u) : std::max(state.max_vertices, 1u);
        const u32 scalars = std::max(primitive ? l.primitive_scalars : l.vertex_scalars, 1u);
        const u32 var = primitive ? m_mesh_primitives : m_mesh_vertices;
        const auto& table = primitive ? l.primitive_slot : l.vertex_slot;
        const auto store = [&](u16 slot, u32 condition) {
            mesh_if(condition, [&] {
                mesh_store_word(var, m_builder.emit_iadd(m_type_uint, m_builder.emit_imul(m_type_uint, index, c(scalars)),
                                                         c(slot)),
                                value);
            });
        };
        const u32 in_range = lt(index, c(count));
        if (reg == kMeshCullPrimitiveRegister) {
            if (!primitive) return fail("VX_MESH: SV_CullPrimitive is per primitive");
            if (l.cull_primitive_slot != kMeshNoSlot) store(l.cull_primitive_slot, in_range);
            return true;
        }
        if (!dynamic) {
            const u16 slot = table[(reg & 31u) * 4u + comp];
            if (slot != kMeshNoSlot) store(slot, in_range);  // else: read by nothing
            return true;
        }
        for (u32 k = 0; k < rows && reg + k < 32u; ++k) {
            const u16 slot = table[(reg + k) * 4u + comp];
            if (slot == kMeshNoSlot) continue;
            store(slot, m_builder.emit_logical(SpvOp::OpLogicalAnd, m_type_bool, in_range, eq(row, c(k))));
        }
        return true;
    }
    case MeshOp::EmitIndices: {
        u32 p = 0, v[3]{};
        if (!ms || !decode({&p, &v[0], &v[1], &v[2]})) return fail("VX_MESH EmitIndices: bad operands");
        const u32 vpp = l.vertices_per_primitive();
        mesh_if(lt(p, c(std::max(state.max_primitives, 1u))), [&] {
            const u32 base = m_builder.emit_imul(m_type_uint, p, c(vpp));
            for (u32 k = 0; k < vpp; ++k) {
                mesh_store_word(m_mesh_indices, m_builder.emit_iadd(m_type_uint, base, c(k)), v[k]);
            }
        });
        return true;
    }
    case MeshOp::PayloadLoad: {
        Sm4Operand dst, src;
        if (!ms || !m_decoder.decode_operand(dst) || !m_decoder.decode_operand(src)) {
            return fail("VX_MESH PayloadLoad: bad operands");
        }
        bool ok = true;
        const u32 index = mesh_scalar(src, ok);
        if (!ok) return false;
        if (m_mesh_payload == 0) return fail("VX_MESH PayloadLoad without a payload");
        // A word past the payload reads 0 (the copy is exactly its size).
        const u32 inside = lt(index, c(state.payload_words));
        const u32 clamped = m_builder.emit_select(m_type_uint, inside, index, c(0));
        const u32 loaded = m_builder.emit_load(
            m_type_uint, m_builder.emit_access_chain(
                             m_builder.emit_type_pointer(SpvStorageClass::Workgroup, m_type_uint), m_mesh_payload,
                             {clamped}));
        const u32 word = m_builder.emit_select(m_type_uint, inside, loaded, c(0));
        const u32 v = m_builder.emit_composite_construct(m_type_uvec4, {word, word, word, word});
        return store_vec4(dst, m_builder.emit_bitcast(m_type_vec4, v));
    }
    case MeshOp::DispatchMesh: {
        u32 x = 0, y = 0, z = 0;
        if (ms || !decode({&x, &y, &z})) return fail("VX_MESH DispatchMesh: bad operands");
        const u32 g = (control >> 8) & 0xFFFFu;
        if (!m_tgsm_vars.contains(g)) return fail("VX_MESH DispatchMesh: the payload is no groupshared object");
        m_mesh_payload_tgsm = static_cast<i32>(g);
        mesh_if(first_thread(), [&] {
            mesh_store_word(m_mesh_grid, c(0), x);
            mesh_store_word(m_mesh_grid, c(1), y);
            mesh_store_word(m_mesh_grid, c(2), z);
        });
        return true;
    }
    }
    return fail("VX_MESH: unknown operation " + std::to_string(control & 0xFFu));
}

bool DxbcTranslator::emit_mesh_stage_main(u32 body) {
    const DxbcMeshState& state = m_container.mesh;
    const auto c = [&](u32 v) { return m_builder.emit_constant_int(m_type_uint, v); };
    m_main_func = m_builder.emit_function(m_type_void, m_func_type);
    m_builder.emit_name(m_main_func, "main");
    (void)m_builder.emit_label();
    const u32 tid = m_builder.emit_load(m_type_uint, m_group_index);
    mesh_if(m_builder.emit_integer_compare(SpvOp::OpIEqual, m_type_bool, tid, c(0)), [&] {
        if (state.is_mesh()) {
            mesh_store_word(m_mesh_counts, c(0), c(0));
            mesh_store_word(m_mesh_counts, c(1), c(0));
        } else {
            for (u32 k = 0; k < 3; ++k) mesh_store_word(m_mesh_grid, c(k), c(0));
        }
    });
    if (m_mesh_payload != 0) {
        (void)m_builder.emit_ext_inst(m_type_void, m_mesh_set, kMeshStagePayloadCopy, {m_mesh_payload, tid});
        m_builder.emit_control_barrier(c(kScopeWorkgroup), c(kScopeWorkgroup), c(kAcquireReleaseWorkgroupMemory));
    }
    (void)m_builder.emit_function_call(m_type_void, body);
    // Device staging (MeshStageLayout::device_staging): the outputs are in
    // device memory, so the barrier before the epilogue orders it too
    // (UniformMemory: Metal's mem_device).
    const bool device = m_mesh_layout != nullptr && m_mesh_layout->device_staging && state.is_mesh();
    m_builder.emit_control_barrier(c(kScopeWorkgroup), c(kScopeWorkgroup),
                                   c(kAcquireReleaseWorkgroupMemory | (device ? 0x40u : 0u)));
    if (state.is_mesh()) {
        const u32 depth_offset = m_builder.emit_load(
            m_type_float, m_builder.emit_access_chain(m_type_ptr_push_float, push_block_var(),
                                                      {c(kPushMemberDepthOffset)}));
        (void)m_builder.emit_ext_inst(m_type_void, m_mesh_set, kMeshStageEpilogue,
                                      {m_mesh_vertices, m_mesh_primitives, m_mesh_indices, m_mesh_counts, tid,
                                       depth_offset});
    } else {
        if (m_mesh_payload_tgsm < 0) return fail("amplification shader without DispatchMesh");
        (void)m_builder.emit_ext_inst(m_type_void, m_mesh_set, kObjectStageEpilogue,
                                      {m_tgsm_vars.at(static_cast<u32>(m_mesh_payload_tgsm)), m_mesh_grid, tid});
    }
    m_builder.emit_return();
    m_builder.emit_function_end();
    m_builder.emit_execution_mode(m_main_func, SpvExecutionMode::LocalSize, state.threads[0], state.threads[1],
                                  state.threads[2]);
    m_result.mesh_stage = state.kind;
    return true;
}

}  // namespace vitreo::graphics::dxbc_spirv_detail
