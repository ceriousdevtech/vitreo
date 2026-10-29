// SPDX-License-Identifier: LGPL-2.1-only

/// @file dxbc_to_spirv_geometry.cpp
/// @brief Geometry-shader-to-compute emulation.

#include "graphics/shaders/dxbc_to_spirv_internal.hpp"

namespace vitreo::graphics::dxbc_spirv_detail {

// Descriptor set 4 carries the geometry-expansion buffers. Sets 0-3 are the
// translator's D3D register spaces (b#, t#, s#, u#), so a fifth set keeps the
// emulation from colliding with anything the guest shader declares.
//
// Records are `num_regs` consecutive vec4 slots in signature order, which is
// the layout the host fills for the input side and a later stream-out consumer
// reads on the output side. See .docs/design-geometry-shader-emulation.md.
// One storage buffer in descriptor set 4. Shared by the geometry expansion and
// by the pass-A vertex capture, which writes the buffer the expansion reads --
// so the two must agree on binding, stride and element type, and there is one
// place that says what they are.
u32 DxbcTranslator::declare_expansion_buffer(u32 element_type, u32 stride,
                                             const char* name, u32 binding,
                                             bool read_only) {
    const u32 array_ty = m_builder.emit_type_runtime_array(element_type);
    m_builder.emit_decorate(array_ty, SpvDecoration::ArrayStride, stride);
    const u32 struct_ty = m_builder.emit_type_struct({array_ty});
    m_builder.emit_decorate(struct_ty, SpvDecoration::Block);
    m_builder.emit_member_decorate(struct_ty, 0, SpvDecoration::Offset, 0);
    if (read_only) {
        m_builder.emit_member_decorate(struct_ty, 0,
                                       SpvDecoration::NonWritable);
    }
    const u32 ptr_ty = m_builder.emit_type_pointer(
        SpvStorageClass::StorageBuffer, struct_ty);
    const u32 var_id =
        m_builder.emit_variable(ptr_ty, SpvStorageClass::StorageBuffer);
    m_builder.emit_name(var_id, name);
    m_builder.emit_decorate(var_id, SpvDecoration::DescriptorSet, 4);
    m_builder.emit_decorate(var_id, SpvDecoration::Binding, binding);
    return var_id;
}

bool DxbcTranslator::emit_geometry_expansion_buffers() {
    const auto storage_buffer_of = [&](u32 element_type, u32 stride,
                                       const char* name, u32 binding,
                                       bool read_only) {
        return declare_expansion_buffer(element_type, stride, name, binding,
                                        read_only);
    };

    m_gs_in_var = storage_buffer_of(m_type_vec4, 16u, "vitreo_gs_in", 0u,
                                    /*read_only=*/true);
    m_gs_out_var = storage_buffer_of(m_type_vec4, 16u, "vitreo_gs_out", 1u,
                                     /*read_only=*/false);
    m_gs_counter_var = storage_buffer_of(m_type_uint, 4u, "vitreo_gs_counter",
                                         2u, /*read_only=*/false);
    // The guest's index buffer, read as 32-bit words. The host binds a
    // non-null buffer even for a non-indexed draw, which is what lets the
    // prologue resolve a vertex index without branching -- see
    // emit_geometry_input_prologue.
    m_gs_index_var = storage_buffer_of(m_type_uint, 4u, "vitreo_gs_index", 3u,
                                       /*read_only=*/true);

    // Strip scratch. One vec4 per output register for each of the last two
    // emitted vertices: a triangle strip needs two, a line strip one, a point
    // list none, and carrying two unconditionally costs nothing measurable
    // against the branch it would save.
    const u32 private_vec4 =
        m_builder.emit_type_pointer(SpvStorageClass::Private, m_type_vec4);
    const u32 private_uint =
        m_builder.emit_type_pointer(SpvStorageClass::Private, m_type_uint);
    for (usize i = 0; i < m_gs_emit_registers.size(); ++i) {
        const u32 prev0 =
            m_builder.emit_variable(private_vec4, SpvStorageClass::Private);
        m_builder.emit_name(prev0, "gs_strip_prev0_" + std::to_string(i));
        m_gs_strip_prev0.push_back(prev0);
        const u32 prev1 =
            m_builder.emit_variable(private_vec4, SpvStorageClass::Private);
        m_builder.emit_name(prev1, "gs_strip_prev1_" + std::to_string(i));
        m_gs_strip_prev1.push_back(prev1);
    }
    m_gs_strip_length =
        m_builder.emit_variable(private_uint, SpvStorageClass::Private);
    m_builder.emit_name(m_gs_strip_length, "gs_strip_length");
    for (u32 stream = 0; stream < 4; ++stream) {
        if ((m_gs_stream_mask & (1u << stream)) == 0u) continue;
        m_gs_stream_count_vars[stream] =
            m_builder.emit_variable(private_uint, SpvStorageClass::Private);
        m_builder.emit_name(m_gs_stream_count_vars[stream],
                            "gs_vertices_written_" + std::to_string(stream));
    }
    m_gs_emit_total =
        m_builder.emit_variable(private_uint, SpvStorageClass::Private);
    m_builder.emit_name(m_gs_emit_total, "gs_emits");
    m_gs_invocation_var =
        m_builder.emit_variable(private_uint, SpvStorageClass::Private);
    m_builder.emit_name(m_gs_invocation_var, "gs_invocation");
    m_gs_primitive_var =
        m_builder.emit_variable(private_uint, SpvStorageClass::Private);
    m_builder.emit_name(m_gs_primitive_var, "gs_primitive_id");
    m_gs_instance_var =
        m_builder.emit_variable(private_uint, SpvStorageClass::Private);
    m_builder.emit_name(m_gs_instance_var, "gs_instance_id");
    m_gs_invocation_capacity = gs_vertices_per_invocation(
        m_gs_output_topology, m_gs_max_output_vertices);
    return true;
}

// One thread per input primitive. The thread's primitive index comes from
// GlobalInvocationId.x, and the prologue copies that primitive's vertices out
// of the captured vertex-stage buffer into the Private v[vertex][register]
// arrays the translated body already knows how to read.
bool DxbcTranslator::emit_geometry_input_prologue() {
    // Shared by the lowered geometry shader and the lowered hull shader: both
    // are one thread per input primitive (patch) reading its vertices
    // (control points) out of the captured vertex-stage records.
    if (gs_as_compute()) {
        m_builder.emit_store(m_gs_strip_length,
                             m_builder.emit_constant_int(m_type_uint, 0u));
    }
    if (m_dispatch_thread_id == 0) {
        return fail("geometry emulation: no dispatch thread id to derive the "
                    "primitive index from");
    }
    const u32 thread_id =
        m_builder.emit_load(m_type_uvec3, m_dispatch_thread_id);
    m_gs_invocation_index =
        m_builder.emit_composite_extract(m_type_uint, thread_id, {0u});
    if (hs_as_compute()) {
        m_gs_primitive_index = m_gs_invocation_index;
        // Control words 8-10 (an indirect draw's patches, counted by the GPU
        // resolve): [8] flags, bit 0: patches past [9] return (the dispatch
        // is sized for the most there can be), bit 1: SV_PrimitiveID is the
        // patch modulo [10], the patches per draw instance (D3D11.3 8.17,
        // 11.7.9.1).
        const u32 member0 = m_builder.emit_constant_int(m_type_uint, 0u);
        const u32 uint_ptr0 = m_builder.emit_type_pointer(
            SpvStorageClass::StorageBuffer, m_type_uint);
        const auto control_word = [&](u32 index) {
            return m_builder.emit_load(
                m_type_uint,
                m_builder.emit_access_chain(
                    uint_ptr0, m_gs_counter_var,
                    {member0, m_builder.emit_constant_int(m_type_uint, index)}));
        };
        const auto flag_set = [&](u32 flags, u32 bit) {
            return m_builder.emit_integer_compare(
                SpvOp::OpINotEqual, m_type_bool,
                m_builder.emit_bitwise_and(
                    m_type_uint, flags,
                    m_builder.emit_constant_int(m_type_uint, bit)),
                m_builder.emit_constant_int(m_type_uint, 0u));
        };
        const u32 flags = control_word(kHullControlFlags);
        const u32 beyond = m_builder.emit_logical(
            SpvOp::OpLogicalAnd, m_type_bool, flag_set(flags, 1u),
            m_builder.emit_integer_compare(
                SpvOp::OpUGreaterThanEqual, m_type_bool, m_gs_invocation_index,
                control_word(kHullControlPatchLimit)));
        const u32 exit_block = m_builder.allocate_id();
        const u32 run_block = m_builder.allocate_id();
        m_builder.emit_selection_merge(run_block);
        m_builder.emit_branch_conditional(beyond, exit_block, run_block);
        m_builder.emit_label_id(exit_block);
        m_builder.emit_return();
        m_builder.emit_label_id(run_block);
        const u32 per_instance = control_word(kHullControlPatchesPerInstance);
        const u32 modulo = m_builder.emit_umod(
            m_type_uint, m_gs_invocation_index,
            m_builder.emit_select(
                m_type_uint,
                m_builder.emit_integer_compare(
                    SpvOp::OpIEqual, m_type_bool, per_instance,
                    m_builder.emit_constant_int(m_type_uint, 0u)),
                m_builder.emit_constant_int(m_type_uint, 1u), per_instance));
        m_builder.emit_store(
            m_gs_primitive_var,
            m_builder.emit_select(m_type_uint, flag_set(flags, 2u), modulo,
                                  m_gs_primitive_index));
    } else {
        // One thread per INVOCATION: primitive-major, instance-minor, which is
        // D3D's output order (by input primitive, then by instance).
        const u32 member0 = m_builder.emit_constant_int(m_type_uint, 0u);
        const u32 uint_ptr0 = m_builder.emit_type_pointer(
            SpvStorageClass::StorageBuffer, m_type_uint);
        const auto control_word = [&](u32 index) {
            return m_builder.emit_load(
                m_type_uint,
                m_builder.emit_access_chain(
                    uint_ptr0, m_gs_counter_var,
                    {member0, m_builder.emit_constant_int(m_type_uint, index)}));
        };
        const auto flag_set = [&](u32 flags, u32 bit) {
            return m_builder.emit_integer_compare(
                SpvOp::OpINotEqual, m_type_bool,
                m_builder.emit_bitwise_and(
                    m_type_uint, flags,
                    m_builder.emit_constant_int(m_type_uint, bit)),
                m_builder.emit_constant_int(m_type_uint, 0u));
        };
        // Control block (host-written, or by the GPU resolve of an indirect
        // draw): [0] flags, [5] invocation limit (flag 1: a dispatch sized
        // for the most invocations there can be, when the GPU decides how
        // many), [6] primitives per draw instance (flag 2: SV_PrimitiveID
        // restarts with each instance, D3D11.3 8.17), [7] invocation slots
        // per stream region, [8] word offset of a per-primitive
        // SV_PrimitiveID table in the index buffer (flag 4: a tessellated
        // input's primitives carry their patch's id, 11.7.9.1).
        const u32 flags = control_word(kGsControlFlags);
        // This invocation's written-vertex counts start at zero, so an
        // invocation that emits nothing -- or one past the limit, which
        // stops right after -- leaves no stale count behind for pass C and
        // the stream-output compaction.
        const u32 stream_stride = control_word(kGsControlStreamStride);
        for (u32 stream = 0; stream < 4; ++stream) {
            if (m_gs_stream_count_vars[stream] == 0) continue;
            m_builder.emit_store(m_gs_stream_count_vars[stream],
                                 m_builder.emit_constant_int(m_type_uint, 0u));
            const u32 slot = m_builder.emit_iadd(
                m_type_uint,
                m_builder.emit_iadd(
                    m_type_uint,
                    m_builder.emit_imul(
                        m_type_uint, stream_stride,
                        m_builder.emit_constant_int(m_type_uint, stream)),
                    m_gs_invocation_index),
                m_builder.emit_constant_int(m_type_uint,
                                            kGsInvocationCountBase));
            m_builder.emit_store(
                m_builder.emit_access_chain(uint_ptr0, m_gs_counter_var,
                                            {member0, slot}),
                m_builder.emit_constant_int(m_type_uint, 0u));
        }
        const u32 beyond = m_builder.emit_logical(
            SpvOp::OpLogicalAnd, m_type_bool, flag_set(flags, 1u),
            m_builder.emit_integer_compare(
                SpvOp::OpUGreaterThanEqual, m_type_bool,
                m_gs_invocation_index,
                control_word(kGsControlInvocationLimit)));
        {
            const u32 exit_block = m_builder.allocate_id();
            const u32 run_block = m_builder.allocate_id();
            m_builder.emit_selection_merge(run_block);
            m_builder.emit_branch_conditional(beyond, exit_block, run_block);
            m_builder.emit_label_id(exit_block);
            m_builder.emit_return();
            m_builder.emit_label_id(run_block);
        }
        m_builder.emit_store(m_gs_invocation_var, m_gs_invocation_index);
        const u32 instances =
            m_builder.emit_constant_int(m_type_uint, m_gs_instance_count);
        m_gs_primitive_index =
            m_builder.emit_udiv(m_type_uint, m_gs_invocation_index, instances);
        const u32 per_instance =
            control_word(kGsControlPrimitivesPerInstance);
        const u32 table_id = m_builder.emit_load(
            m_type_uint,
            m_builder.emit_access_chain(
                uint_ptr0, m_gs_index_var,
                {member0,
                 m_builder.emit_iadd(m_type_uint,
                                     control_word(kGsControlPrimitiveIdTable),
                                     m_gs_primitive_index)}));
        const u32 modulo_id = m_builder.emit_umod(
            m_type_uint, m_gs_primitive_index,
            m_builder.emit_select(
                m_type_uint,
                m_builder.emit_integer_compare(
                    SpvOp::OpIEqual, m_type_bool, per_instance,
                    m_builder.emit_constant_int(m_type_uint, 0u)),
                m_builder.emit_constant_int(m_type_uint, 1u), per_instance));
        const u32 primitive_id = m_builder.emit_select(
            m_type_uint, flag_set(flags, 4u), table_id,
            m_builder.emit_select(m_type_uint, flag_set(flags, 2u), modulo_id,
                                  m_gs_primitive_index));
        m_builder.emit_store(m_gs_primitive_var, primitive_id);
        m_builder.emit_store(
            m_gs_instance_var,
            m_builder.emit_umod(m_type_uint, m_gs_invocation_index, instances));
        m_builder.emit_store(m_gs_emit_total,
                             m_builder.emit_constant_int(m_type_uint, 0u));
    }

    const u32 vertices_per_primitive = input_array_width();
    if (m_gs_input_registers.empty()) return true;  // nothing to fill
    // Where each input register lives in the vertex stage's record, and the
    // record's stride. Without an explicit layout the record is assumed to
    // hold exactly this shader's inputs, in ascending register order.
    const std::vector<u32>& layout =
        m_options.gs_input_record_registers.empty()
            ? m_gs_input_registers
            : m_options.gs_input_record_registers;
    const u32 record_vec4s = static_cast<u32>(layout.size());
    std::vector<u32> record_slot(m_gs_input_registers.size());
    for (usize i = 0; i < m_gs_input_registers.size(); ++i) {
        const auto found = std::find(layout.begin(), layout.end(),
                                     m_gs_input_registers[i]);
        if (found == layout.end()) {
            return fail("geometry emulation: the vertex stage does not write "
                        "v" + std::to_string(m_gs_input_registers[i]) +
                        ", which this geometry shader reads");
        }
        record_slot[i] = static_cast<u32>(found - layout.begin());
    }

    const u32 member = m_builder.emit_constant_int(m_type_uint, 0u);
    const u32 vec4_ptr = m_builder.emit_type_pointer(
        SpvStorageClass::StorageBuffer, m_type_vec4);
    const u32 uint_ptr = m_builder.emit_type_pointer(
        SpvStorageClass::StorageBuffer, m_type_uint);

    // Draw shape, written by the host into the counter buffer before the
    // dispatch. Slot 0 is the append counter; these describe how to turn a
    // (primitive, vertex) pair into a vertex index:
    //
    //   [1] index format: 0 = non-indexed, 1 = 16-bit, 2 = 32-bit
    //   [2] first index / first vertex
    //   [3] base vertex, added to a fetched index
    //   [4] primitive stride in the index stream
    //
    // The stride is what makes strip input topologies free: a triangle LIST
    // advances 3 per primitive and a triangle STRIP advances 1, and both are
    // then `first + primitive * stride + vertex` with no branch. D3D hands a
    // geometry shader a strip's vertices in strip order, so nothing is
    // reordered here either.
    const auto counter_slot = [&](u32 index) {
        return m_builder.emit_load(
            m_type_uint,
            m_builder.emit_access_chain(
                uint_ptr, m_gs_counter_var,
                {member, m_builder.emit_constant_int(m_type_uint, index)}));
    };
    const u32 index_format = counter_slot(1u);
    const u32 first_index = counter_slot(2u);
    const u32 base_vertex = counter_slot(3u);
    const u32 primitive_stride = counter_slot(4u);

    const u32 primitive_base = m_builder.emit_imul(
        m_type_uint, m_gs_primitive_index, primitive_stride);
    const u32 stream_base =
        m_builder.emit_iadd(m_type_uint, first_index, primitive_base);

    for (u32 vertex = 0; vertex < vertices_per_primitive; ++vertex) {
        // Position of this vertex in the index stream.
        const u32 stream_slot = m_builder.emit_iadd(
            m_type_uint, stream_base,
            m_builder.emit_constant_int(m_type_uint, vertex));

        // Resolve it to a vertex index. This is deliberately BRANCHLESS: the
        // host binds a non-null index buffer even for a non-indexed draw, so
        // the loads below are always in bounds and the format is a select
        // rather than control flow around a load.
        const u32 word32 = m_builder.emit_load(
            m_type_uint,
            m_builder.emit_access_chain(uint_ptr, m_gs_index_var,
                                        {member, stream_slot}));
        const u32 packed = m_builder.emit_load(
            m_type_uint,
            m_builder.emit_access_chain(
                uint_ptr, m_gs_index_var,
                {member, m_builder.emit_shift_right_logical(
                             m_type_uint, stream_slot,
                             m_builder.emit_constant_int(m_type_uint, 1u))}));
        const u32 low16 = m_builder.emit_bitwise_and(
            m_type_uint, packed,
            m_builder.emit_constant_int(m_type_uint, 0xFFFFu));
        const u32 high16 = m_builder.emit_shift_right_logical(
            m_type_uint, packed,
            m_builder.emit_constant_int(m_type_uint, 16u));
        const u32 odd_slot = m_builder.emit_integer_compare(
            SpvOp::OpINotEqual, m_type_bool,
            m_builder.emit_bitwise_and(
                m_type_uint, stream_slot,
                m_builder.emit_constant_int(m_type_uint, 1u)),
            m_builder.emit_constant_int(m_type_uint, 0u));
        const u32 word16 =
            m_builder.emit_select(m_type_uint, odd_slot, high16, low16);
        const u32 is_32bit = m_builder.emit_integer_compare(
            SpvOp::OpIEqual, m_type_bool, index_format,
            m_builder.emit_constant_int(m_type_uint, 2u));
        const u32 fetched =
            m_builder.emit_select(m_type_uint, is_32bit, word32, word16);
        const u32 indexed =
            m_builder.emit_iadd(m_type_uint, fetched, base_vertex);
        const u32 non_indexed = m_builder.emit_integer_compare(
            SpvOp::OpIEqual, m_type_bool, index_format,
            m_builder.emit_constant_int(m_type_uint, 0u));
        const u32 vertex_index = m_builder.emit_select(
            m_type_uint, non_indexed, stream_slot, indexed);

        // The capture buffer is keyed by vertex index, because pass A is an
        // ordinary draw and its vertex shader knows nothing but its own
        // vertex id.
        const u32 record_base = m_builder.emit_imul(
            m_type_uint, vertex_index,
            m_builder.emit_constant_int(m_type_uint, record_vec4s));
        for (usize i = 0; i < m_gs_input_registers.size(); ++i) {
            const u32 reg = m_gs_input_registers[i];
            const auto var = m_input_vars.find(reg);
            const auto value_type = m_input_value_types.find(reg);
            if (var == m_input_vars.end() ||
                value_type == m_input_value_types.end()) {
                continue;
            }
            const u32 slot = m_builder.emit_iadd(
                m_type_uint, record_base,
                m_builder.emit_constant_int(m_type_uint, record_slot[i]));
            const u32 source = m_builder.emit_load(
                m_type_vec4,
                m_builder.emit_access_chain(vec4_ptr, m_gs_in_var,
                                            {member, slot}));
            // The Private input is an array indexed by vertex, exactly as the
            // geometry-stage Input variable was, so the body's access chain on
            // the vertex index keeps working.
            const u32 element_ptr = m_builder.emit_type_pointer(
                SpvStorageClass::Private, value_type->second);
            const u32 destination = m_builder.emit_access_chain(
                element_ptr, var->second,
                {m_builder.emit_constant_int(m_type_uint, vertex)});
            m_builder.emit_store(
                destination,
                value_type->second == m_type_vec4
                    ? source
                    : m_builder.emit_bitcast(value_type->second, source));
        }
    }
    return true;
}

void DxbcTranslator::emit_geometry_cut() {
    // `cut` ends the current strip. Because every primitive is written out
    // independently, ending a strip is just forgetting its history.
    m_builder.emit_store(m_gs_strip_length,
                         m_builder.emit_constant_int(m_type_uint, 0u));
}

// `emit` completes a primitive of the declared output topology and appends it
// to gs_out as an INDEPENDENT primitive. A point list appends on every emit; a
// line strip from the second vertex onward; a triangle strip from the third,
// alternating winding the way a strip does.
bool DxbcTranslator::emit_geometry_vertex(bool cut_after, u32 stream) {
    const u32 register_count = static_cast<u32>(m_gs_emit_registers.size());
    if (stream >= 4u || m_gs_stream_count_vars[stream] == 0) {
        return fail("emit to geometry stream m" + std::to_string(stream) +
                    ", which the shader does not declare");
    }
    const u32 member = m_builder.emit_constant_int(m_type_uint, 0u);
    const u32 vec4_ptr = m_builder.emit_type_pointer(
        SpvStorageClass::StorageBuffer, m_type_vec4);
    const u32 uint_ptr = m_builder.emit_type_pointer(
        SpvStorageClass::StorageBuffer, m_type_uint);

    // dcl_maxOutputVertexCount counts EMITS, on every stream, whether or not
    // the vertex completes a primitive; past it the invocation stops writing
    // (D3D11.3 13.3: "stopping writes if the Geometry Shader program
    // continues after emitting the declared maximum number of vertices";
    // measured: three 3-vertex strips under maxvertexcount(7) write two
    // triangles, five alternating point emits under 3 write s0 s1 s0).
    const u32 emitted = m_builder.emit_load(m_type_uint, m_gs_emit_total);
    const u32 within = m_builder.emit_integer_compare(
        SpvOp::OpULessThan, m_type_bool, emitted,
        m_builder.emit_constant_int(m_type_uint, m_gs_max_output_vertices));
    const u32 emit_block = m_builder.allocate_id();
    const u32 emit_done = m_builder.allocate_id();
    m_builder.emit_selection_merge(emit_done);
    m_builder.emit_branch_conditional(within, emit_block, emit_done);
    m_builder.emit_label_id(emit_block);
    m_builder.emit_store(
        m_gs_emit_total,
        m_builder.emit_iadd(m_type_uint, emitted,
                            m_builder.emit_constant_int(m_type_uint, 1u)));
    if (register_count == 0u) {
        if (cut_after) emit_geometry_cut();
        m_builder.emit_branch(emit_done);
        m_builder.emit_label_id(emit_done);
        return true;
    }

    // Snapshot the o# registers as raw vec4s; the buffer is untyped storage,
    // so a uint or int output is reinterpreted, never converted. Every
    // stream's record holds the union of the streams' registers (D3D11.3
    // 13.5.2: "the output record size is the number of scalars in the union
    // of all the stream declarations"); a stream's consumers read its own.
    std::vector<u32> current;
    current.reserve(register_count);
    for (const u32 reg : m_gs_emit_registers) {
        const auto var = m_output_vars.find(reg);
        const auto value_type = m_output_value_types.find(reg);
        if (var == m_output_vars.end() ||
            value_type == m_output_value_types.end()) {
            return fail("geometry emulation: output o" + std::to_string(reg) +
                        " was declared but has no variable");
        }
        const u32 loaded =
            m_builder.emit_load(value_type->second, var->second);
        current.push_back(value_type->second == m_type_vec4
                              ? loaded
                              : m_builder.emit_bitcast(m_type_vec4, loaded));
    }

    // This invocation's slot in the stream: s * stride + invocation, stride
    // being the control block's invocation slots per stream region.
    const u32 invocation = m_builder.emit_load(m_type_uint, m_gs_invocation_var);
    const u32 invocation_slot = m_builder.emit_iadd(
        m_type_uint,
        m_builder.emit_imul(
            m_type_uint,
            m_builder.emit_load(
                m_type_uint,
                m_builder.emit_access_chain(
                    uint_ptr, m_gs_counter_var,
                    {member, m_builder.emit_constant_int(
                                 m_type_uint, kGsControlStreamStride)})),
            m_builder.emit_constant_int(m_type_uint, stream)),
        invocation);
    const u32 region_base = m_builder.emit_imul(
        m_type_uint, invocation_slot,
        m_builder.emit_constant_int(m_type_uint, m_gs_invocation_capacity));
    const u32 local_count = m_gs_stream_count_vars[stream];

    // Append one primitive of `count` vertices into THIS invocation's region
    // of gs_out, at the next free slot. Regions are fixed per invocation, so
    // the list pass C draws is in D3D's order (by input primitive, then by
    // instance) whatever order the GPU ran the threads in -- a shared atomic
    // cursor here made overlapping output primitives land in scheduling
    // order. Vertices past the region are dropped rather than written into
    // the next invocation's region. The running count is published for
    // pass C, which culls the unwritten rest of the region.
    const auto append_primitive = [&](u32 count,
                                      const std::vector<const std::vector<u32>*>&
                                          vertices) {
        const u32 written =
            m_builder.emit_load(m_type_uint, local_count);
        const u32 next = m_builder.emit_iadd(
            m_type_uint, written,
            m_builder.emit_constant_int(m_type_uint, count));
        const u32 fits = m_builder.emit_integer_compare(
            SpvOp::OpULessThanEqual, m_type_bool, next,
            m_builder.emit_constant_int(m_type_uint,
                                        m_gs_invocation_capacity));
        const u32 write_block = m_builder.allocate_id();
        const u32 done_block = m_builder.allocate_id();
        m_builder.emit_selection_merge(done_block);
        m_builder.emit_branch_conditional(fits, write_block, done_block);
        m_builder.emit_label_id(write_block);
        const u32 base_slot =
            m_builder.emit_iadd(m_type_uint, region_base, written);
        for (u32 v = 0; v < count; ++v) {
            const u32 vertex_slot = m_builder.emit_iadd(
                m_type_uint, base_slot,
                m_builder.emit_constant_int(m_type_uint, v));
            const u32 record_base = m_builder.emit_imul(
                m_type_uint, vertex_slot,
                m_builder.emit_constant_int(m_type_uint, register_count));
            for (u32 i = 0; i < register_count; ++i) {
                const u32 slot = m_builder.emit_iadd(
                    m_type_uint, record_base,
                    m_builder.emit_constant_int(m_type_uint, i));
                m_builder.emit_store(
                    m_builder.emit_access_chain(vec4_ptr, m_gs_out_var,
                                                {member, slot}),
                    (*vertices[v])[i]);
            }
        }
        m_builder.emit_store(local_count, next);
        m_builder.emit_store(
            m_builder.emit_access_chain(
                uint_ptr, m_gs_counter_var,
                {member,
                 m_builder.emit_iadd(
                     m_type_uint, invocation_slot,
                     m_builder.emit_constant_int(m_type_uint,
                                                 kGsInvocationCountBase))}),
            next);
        m_builder.emit_branch(done_block);
        m_builder.emit_label_id(done_block);
    };

    if (m_gs_output_topology == 1u) {
        // pointlist: every emit is a whole primitive.
        append_primitive(1u, {&current});
        if (cut_after) emit_geometry_cut();
        m_builder.emit_branch(emit_done);
        m_builder.emit_label_id(emit_done);
        return true;
    }

    const u32 strip_length =
        m_builder.emit_load(m_type_uint, m_gs_strip_length);
    const bool triangles = m_gs_output_topology == 5u;
    const u32 needed = triangles ? 2u : 1u;
    const u32 completes = m_builder.emit_integer_compare(
        SpvOp::OpUGreaterThanEqual, m_type_bool, strip_length,
        m_builder.emit_constant_int(m_type_uint, needed));

    const u32 append_block = m_builder.allocate_id();
    const u32 merge_block = m_builder.allocate_id();
    m_builder.emit_selection_merge(merge_block);
    m_builder.emit_branch_conditional(completes, append_block, merge_block);
    m_builder.emit_label_id(append_block);
    if (triangles) {
        // A strip's triangles alternate winding: triangle i is
        // (v_i, v_i+1, v_i+2) for even i and (v_i, v_i+2, v_i+1) for odd i --
        // the winding flips while v_i, the leading (provoking) vertex, stays
        // first (Windows, d3d11_a09b [GS] gs_cut: 0 1 2 / 1 3 2; the IA's
        // own strips measure the same, d3d11_strip_order_probe). i ==
        // strip_length - 2, so the parity is strip_length's. OpSelect keeps
        // that a data choice rather than a second branch.
        const u32 odd = m_builder.emit_integer_compare(
            SpvOp::OpINotEqual, m_type_bool,
            m_builder.emit_bitwise_and(
                m_type_uint, strip_length,
                m_builder.emit_constant_int(m_type_uint, 1u)),
            m_builder.emit_constant_int(m_type_uint, 0u));
        std::vector<u32> first(register_count);
        std::vector<u32> second(register_count);
        std::vector<u32> third(register_count);
        for (u32 i = 0; i < register_count; ++i) {
            const u32 prev0 =
                m_builder.emit_load(m_type_vec4, m_gs_strip_prev0[i]);
            const u32 prev1 =
                m_builder.emit_load(m_type_vec4, m_gs_strip_prev1[i]);
            first[i] = prev0;
            second[i] = m_builder.emit_select(m_type_vec4, odd, current[i],
                                              prev1);
            third[i] = m_builder.emit_select(m_type_vec4, odd, prev1,
                                             current[i]);
        }
        append_primitive(3u, {&first, &second, &third});
    } else {
        std::vector<u32> previous(register_count);
        for (u32 i = 0; i < register_count; ++i) {
            previous[i] =
                m_builder.emit_load(m_type_vec4, m_gs_strip_prev1[i]);
        }
        append_primitive(2u, {&previous, &current});
    }
    m_builder.emit_branch(merge_block);
    m_builder.emit_label_id(merge_block);

    // Slide the history and count this vertex. prev1 is always the most
    // recent; prev0 only matters for triangles.
    for (u32 i = 0; i < register_count; ++i) {
        if (triangles) {
            m_builder.emit_store(
                m_gs_strip_prev0[i],
                m_builder.emit_load(m_type_vec4, m_gs_strip_prev1[i]));
        }
        m_builder.emit_store(m_gs_strip_prev1[i], current[i]);
    }
    m_builder.emit_store(
        m_gs_strip_length,
        m_builder.emit_iadd(m_type_uint, strip_length,
                            m_builder.emit_constant_int(m_type_uint, 1u)));
    if (cut_after) emit_geometry_cut();
    m_builder.emit_branch(emit_done);
    m_builder.emit_label_id(emit_done);
    return true;
}

} // namespace vitreo::graphics::dxbc_spirv_detail
