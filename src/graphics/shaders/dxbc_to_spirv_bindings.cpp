// SPDX-License-Identifier: LGPL-2.1-only

/// @file dxbc_to_spirv_bindings.cpp
/// @brief Register binding and the descriptor model.

#include "graphics/shaders/dxbc_to_spirv_internal.hpp"
#include "graphics/shaders/sampler_features.hpp"

#include <vector>

namespace vitreo::graphics::dxbc_spirv_detail {

namespace {

/// fxc's register letter of a class (b/t/u/s).
const char* register_letter(DxbcRegisterClass cls) {
    switch (cls) {
    case DxbcRegisterClass::ConstantBuffer: return "b";
    case DxbcRegisterClass::ShaderResource: return "t";
    case DxbcRegisterClass::UnorderedAccess: return "u";
    case DxbcRegisterClass::Sampler: return "s";
    }
    return "?";
}

/// The descriptor set a class keeps without a remap (0 = b#, 1 = t#,
/// 2 = s#, 3 = u#; BACKEND_MAX_DESCRIPTOR_SETS' register spaces).
u32 identity_descriptor_set(DxbcRegisterClass cls) {
    switch (cls) {
    case DxbcRegisterClass::ConstantBuffer: return 0u;
    case DxbcRegisterClass::ShaderResource: return 1u;
    case DxbcRegisterClass::Sampler: return 2u;
    case DxbcRegisterClass::UnorderedAccess: return 3u;
    }
    return 0u;
}

}  // namespace

bool DxbcTranslator::resolve_register_run(DxbcRegisterClass cls, u32 space,
                                          u32 lower, u32 count, u32& set,
                                          u32& binding, u32& covered) const {
    const bool unbounded = count == kDxbcUnboundedRange;
    if (m_options.register_remap.empty()) {
        // One binding per register in the class's own set; space 0 is the
        // only space that model has.
        if (space != 0u) return false;
        set = identity_descriptor_set(cls);
        binding = lower;
        covered = unbounded ? 0u : count;
        return true;
    }
    // The run that starts latest still covering `reg`. An earlier unbounded
    // run covers every register above its base, and the first match would
    // bind a later range inside that array. The later run is the table slot
    // assigned to that base (DxbcRegisterRemap).
    const auto entry_holding = [&](u32 reg) -> const DxbcRegisterRemap* {
        const DxbcRegisterRemap* best = nullptr;
        for (const auto& entry : m_options.register_remap) {
            if (entry.register_class != cls || entry.space != space ||
                reg < entry.first_register) {
                continue;
            }
            if (entry.register_count != kDxbcUnboundedRange &&
                reg - entry.first_register >= entry.register_count) {
                continue;
            }
            if (best == nullptr ||
                entry.first_register > best->first_register) {
                best = &entry;
            }
        }
        return best;
    };
    const DxbcRegisterRemap* entry = entry_holding(lower);
    if (entry == nullptr) return false;
    set = entry->descriptor_set;
    binding = entry->first_slot + (lower - entry->first_register);
    // Follow runs that continue this one in both register and binding.
    const u64 wanted_end = unbounded ? ~u64{0} : u64{lower} + count;
    for (;;) {
        if (entry->register_count == kDxbcUnboundedRange) {
            covered = unbounded ? 0u : count;
            return true;
        }
        const u64 end = u64{entry->first_register} + entry->register_count;
        if (!unbounded && wanted_end <= end) {
            covered = count;
            return true;
        }
        const DxbcRegisterRemap* next = nullptr;
        if (end <= 0xFFFFFFFFull) {
            next = entry_holding(static_cast<u32>(end));
        }
        if (next == nullptr || next->first_register != end ||
            next->descriptor_set != entry->descriptor_set ||
            u64{next->first_slot} !=
                u64{entry->first_slot} + entry->register_count) {
            if (!unbounded) return false;
            covered = static_cast<u32>(end - lower);
            return true;
        }
        entry = next;
    }
}

bool DxbcTranslator::resolve_bindings() {
    if (descriptor_table_translation()) {
        // Keyed by a D3D11 register and filled from the D3D11 frontend's
        // push block: in a descriptor-table translation the key is a range
        // ID or an array element, so none of them has a meaning here.
        if (m_options.border_twin_samplers != 0 ||
            m_options.minmax_samplers != 0 ||
            m_options.rgb32_buffer_srvs != 0 ||
            m_options.single_sample_ms_srvs != 0 ||
            m_options.guard_buffer_bounds ||
            m_options.guard_constant_buffers ||
            m_options.typed_buffer_bias) {
            return fail("descriptor-table translation: border twins, min/max "
                        "samplers, rgb32/single-sample views, typed-buffer "
                        "biases and the push-block bounds guards are keyed "
                        "by D3D11 register");
        }
        // fxc 10.1 cannot compile class linkage for 5.1 (an internal
        // compiler error, measured); D3D12 has no class linkage at all.
        if (dxbc_uses_class_linkage(m_container)) {
            return fail("descriptor-table translation: class linkage");
        }
    }
    for (const auto& declaration : m_container.binding_declarations) {
        const std::string what =
            std::string(register_letter(declaration.register_class)) +
            std::to_string(declaration.lower) +
            (declaration.is_array()
                 ? ":" + (declaration.unbounded()
                              ? std::string("*")
                              : std::to_string(declaration.upper))
                 : std::string()) +
            " space " + std::to_string(declaration.space);
        u32 set = 0, binding = 0, covered = 0;
        if (!resolve_register_run(declaration.register_class,
                                  declaration.space, declaration.lower,
                                  declaration.count(), set, binding,
                                  covered)) {
            return fail(m_options.register_remap.empty()
                            ? what + ": a register space other than 0 needs "
                                     "a register remap"
                            : what + " is not on consecutive bindings of the "
                                     "register remap");
        }
        for (const auto& existing : m_result.bindings) {
            if (existing.descriptor_set == set && existing.binding == binding) {
                return fail(what + " overlaps another range's binding");
            }
        }
        ResourceRange range{};
        range.resolved.declaration = declaration;
        range.resolved.descriptor_set = set;
        range.resolved.binding = binding;
        range.is_array = m_container.is_sm51_plus() && declaration.is_array();
        range.resolved.descriptor_count = range.is_array ? covered : 1u;
        if (is_heap_window(range)) {
            // A window's element is a heap slot, not a register: the array
            // spans the whole window, whatever the declaration's bound.
            range.resolved.descriptor_count = heap_window_length();
        }
        if (!m_ranges
                 .emplace(std::pair{declaration.register_class,
                                    declaration.range_id},
                          range)
                 .second) {
            return fail(what + " is declared twice");
        }
        // Rasterizer-ordered views (dcl_uav_*_rov, OpcodeToken0 bit 17).
        // With options.rasterizer_ordered_views the frontend orders them
        // through the MSL compile (raster order groups), which exist for
        // fragment functions only. Without it a descriptor-table
        // translation refuses them (that frontend does not claim
        // ROVsSupported); a D3D11 translation still ignores the bit, as its
        // OPTIONS2 reports ROVs unsupported.
        if (declaration.rasterizer_ordered && m_options.rasterizer_ordered_views &&
            m_container.stage != ShaderStage::Fragment) {
            return fail(what + ": rasterizer-ordered UAV outside a pixel shader");
        }
        if (descriptor_table_translation() && declaration.rasterizer_ordered &&
            !m_options.rasterizer_ordered_views) {
            return fail(what + ": rasterizer-ordered UAV is not implemented");
        }
        m_result.bindings.push_back(range.resolved);
    }
    return true;
}

u32 DxbcTranslator::declare_resource(DxbcRegisterClass cls, u32 key,
                                     u32 pointer_type, SpvStorageClass storage,
                                     DescriptorKind kind,
                                     const std::string& name) {
    const auto found = m_ranges.find({cls, key});
    ResourceRange* range = found == m_ranges.end() ? nullptr : &found->second;
    u32 set = 0, binding = 0, count = 1;
    bool array = false;
    if (range != nullptr) {
        set = range->resolved.descriptor_set;
        binding = range->resolved.binding;
        array = range->is_array;
        count = range->resolved.descriptor_count;
    } else {
        // A register the program does not declare (an RDEF-only array
        // element, a class instance's): space 0.
        u32 covered = 0;
        if (!resolve_register_run(cls, 0u, key, 1u, set, binding, covered)) {
            (void)fail(std::string(register_letter(cls)) +
                       std::to_string(key) + " is not in the register remap");
            return 0;
        }
    }
    u32 variable_type = pointer_type;
    if (array) {
        const u32 element = m_builder.pointee_type(pointer_type);
        const u32 array_type = count == 0u
            ? m_builder.emit_type_runtime_array(element)
            : m_builder.emit_type_array(
                  element, m_builder.emit_constant_int(m_type_uint, count));
        if (count == 0u) {
            m_builder.emit_capability(SpvCapability::RuntimeDescriptorArray);
        }
        switch (kind) {
        case DescriptorKind::UniformBuffer:
            m_builder.emit_capability(
                SpvCapability::UniformBufferArrayDynamicIndexing);
            break;
        case DescriptorKind::StorageBuffer:
            m_builder.emit_capability(
                SpvCapability::StorageBufferArrayDynamicIndexing);
            break;
        case DescriptorKind::SampledImage:
        case DescriptorKind::Sampler:
            m_builder.emit_capability(
                SpvCapability::SampledImageArrayDynamicIndexing);
            break;
        case DescriptorKind::StorageImage:
            m_builder.emit_capability(
                SpvCapability::StorageImageArrayDynamicIndexing);
            break;
        case DescriptorKind::UniformTexelBuffer:
            m_builder.emit_capability(
                SpvCapability::UniformTexelBufferArrayDynamicIndexing);
            break;
        case DescriptorKind::StorageTexelBuffer:
            m_builder.emit_capability(
                SpvCapability::StorageTexelBufferArrayDynamicIndexing);
            break;
        }
        variable_type = m_builder.emit_type_pointer(storage, array_type);
        if (m_options.heap_window.enabled &&
            set == m_options.heap_window.descriptor_set) {
            declare_heap_window_base();
        }
    }
    const u32 variable = m_builder.emit_variable(variable_type, storage);
    // 5.1 names a resource by its range: T3 rather than t3.
    std::string spelled = name;
    if (m_container.is_sm51_plus() && !spelled.empty()) {
        spelled[0] = static_cast<char>(std::toupper(
            static_cast<unsigned char>(spelled[0])));
        if (spelled.size() > 1 && spelled[1] == 'b') spelled[1] = 'B';
    }
    m_builder.emit_name(variable, spelled);
    m_builder.emit_decorate(variable, SpvDecoration::DescriptorSet, set);
    m_builder.emit_decorate(variable, SpvDecoration::Binding, binding);
    if (range != nullptr) {
        range->variable = variable;
        range->element_pointer_type = pointer_type;
        range->kind = kind;
    }
    return variable;
}

void DxbcTranslator::declare_heap_window_base() {
    if (m_heap_window_base_var != 0) return;
    // SPIRV-Cross aliases the arrays that share an argument-buffer index by
    // casting each to the type of the first one it meets (variable order),
    // and a cast may add qualifiers but never drop them: a read-only
    // buffer array first would leave a UAV array unable to alias it. This
    // writable buffer array of the window's length, declared before every
    // window array (lower id) and never read, is that first one, so every
    // texture, buffer and constant-buffer array casts from it.
    const u32 words = m_builder.emit_type_runtime_array(m_type_uint);
    m_builder.emit_decorate(words, SpvDecoration::ArrayStride, 4u);
    const u32 block = m_builder.emit_type_struct({words});
    m_builder.emit_decorate(block, SpvDecoration::Block);
    m_builder.emit_member_decorate(block, 0, SpvDecoration::Offset, 0);
    const u32 array = m_builder.emit_type_array(
        block,
        m_builder.emit_constant_int(m_type_uint, heap_window_length()));
    m_heap_window_base_var = m_builder.emit_variable(
        m_builder.emit_type_pointer(SpvStorageClass::StorageBuffer, array),
        SpvStorageClass::StorageBuffer);
    m_builder.emit_name(m_heap_window_base_var, "vitreo_heap_window_base");
    m_builder.emit_decorate(m_heap_window_base_var, SpvDecoration::DescriptorSet,
                            m_options.heap_window.descriptor_set);
    m_builder.emit_decorate(m_heap_window_base_var, SpvDecoration::Binding,
                            kDescriptorHeapWindowBaseBinding);
    m_entry_interface.push_back(m_heap_window_base_var);
}

u32 DxbcTranslator::heap_window_length() {
    if (m_heap_window_length < 0) {
        // Decided before the first window array is declared: tiled status
        // may read a window element's residency companion, and a counter
        // instruction may move a window element's counter.
        const bool aux = m_options.heap_window.enabled &&
                         (m_options.tiled_status.enabled ||
                          !scan_uav_counters().empty());
        m_heap_window_length = static_cast<i32>(
            aux ? 2u * kDescriptorHeapWindowLength : kDescriptorHeapWindowLength);
    }
    return static_cast<u32>(m_heap_window_length);
}

u32 DxbcTranslator::declare_heap_window_aux(u32 binding, u32 element_type,
                                            SpvStorageClass storage,
                                            const std::string& name) {
    const u32 length = heap_window_length();
    if (length != 2u * kDescriptorHeapWindowLength) {
        (void)fail("heap window: an auxiliary array without the auxiliary "
                   "window length");
        return 0;
    }
    declare_heap_window_base();
    const u32 array = m_builder.emit_type_array(
        element_type, m_builder.emit_constant_int(m_type_uint, length));
    const u32 variable = m_builder.emit_variable(
        m_builder.emit_type_pointer(storage, array), storage);
    m_builder.emit_name(variable, name);
    m_builder.emit_decorate(variable, SpvDecoration::DescriptorSet,
                            m_options.heap_window.descriptor_set);
    m_builder.emit_decorate(variable, SpvDecoration::Binding,
                            kDescriptorHeapWindowAuxBinding + binding);
    m_result.heap_window_aux = true;
    return variable;
}

u32 DxbcTranslator::heap_window_aux_index(u32 slot) {
    const u32 limit = heap_window_limit();
    if (limit == 0) return 0;
    return m_builder.emit_iadd(
        m_type_uint, slot,
        m_builder.emit_iadd(m_type_uint, limit,
                            m_builder.emit_constant_int(m_type_uint, 1u)));
}

u32 DxbcTranslator::rdef_cbuffer_vectors(u32 key) const {
    for (const auto& rb : m_container.resource_bindings) {
        if (rb.type != 0u) continue;  // D3D_SIT_CBUFFER
        if (m_container.is_sm51_plus() ? rb.range_id != key
                                       : rb.bind_point != key) {
            continue;
        }
        for (const auto& cb : m_container.constant_buffers) {
            if (cb.name == rb.name && cb.size_bytes != 0u) {
                return (cb.size_bytes + 15u) / 16u;
            }
        }
    }
    return 0u;
}

u32 DxbcTranslator::array_element_index(const Sm4Operand& operand,
                                        const ResourceRange& range) {
    const u32 lower = range.resolved.declaration.lower;
    u32 index = 0;
    if (!operand.register_relative) {
        index = m_builder.emit_constant_int(m_type_uint,
                                            operand.register_index - lower);
    } else {
        if (operand.register_relative_type != Sm4OperandType::Temp) {
            (void)fail("descriptor array: unsupported relative index source");
            return 0;
        }
        // The operand holds the absolute register (fxc's T0[r0.y + 2] for
        // element r0.y of T0[2:9]); the element is that minus the lower
        // bound, in the wrapping arithmetic of a uint index.
        index = relative_index_value(operand.register_relative_type,
                                     operand.register_relative_register,
                                     operand.register_relative_component,
                                     operand.register_index - lower);
    }
    if (is_heap_window(range)) {
        // The element of a heap window is a heap slot (heap_window).
        index = heap_window_slot(range, index);
        if (index == 0) return 0;
    }
    if (operand.nonuniform && operand.register_relative) {
        // NonUniformResourceIndex: the index may differ between the
        // invocations of one instruction (SPIR-V 1.5 NonUniform).
        m_builder.emit_capability(SpvCapability::ShaderNonUniform);
        switch (range.kind) {
        case DescriptorKind::UniformBuffer:
            m_builder.emit_capability(
                SpvCapability::UniformBufferArrayNonUniformIndexing);
            break;
        case DescriptorKind::StorageBuffer:
            m_builder.emit_capability(
                SpvCapability::StorageBufferArrayNonUniformIndexing);
            break;
        case DescriptorKind::SampledImage:
        case DescriptorKind::Sampler:
            m_builder.emit_capability(
                SpvCapability::SampledImageArrayNonUniformIndexing);
            break;
        case DescriptorKind::StorageImage:
            m_builder.emit_capability(
                SpvCapability::StorageImageArrayNonUniformIndexing);
            break;
        case DescriptorKind::UniformTexelBuffer:
            m_builder.emit_capability(
                SpvCapability::UniformTexelBufferArrayNonUniformIndexing);
            break;
        case DescriptorKind::StorageTexelBuffer:
            m_builder.emit_capability(
                SpvCapability::StorageTexelBufferArrayNonUniformIndexing);
            break;
        }
        m_builder.emit_decorate(index, SpvDecoration::NonUniform);
    }
    return index;
}

u32 DxbcTranslator::array_element_pointer(const Sm4Operand& operand,
                                          const ResourceRange& range) {
    const u32 index = array_element_index(operand, range);
    if (index == 0) return 0;
    const u32 pointer = m_builder.emit_access_chain(
        range.element_pointer_type, range.variable, {index});
    if (operand.nonuniform && operand.register_relative) {
        m_builder.emit_decorate(pointer, SpvDecoration::NonUniform);
    }
    return pointer;
}

u32 DxbcTranslator::sampler_state_word(const Sm4Operand& sampler, u32 word) {
    // A descriptor-array element's record index was computed where the
    // instruction bound it; a single sampler's is its binding.
    u32 record = 0;
    if (const auto bound = m_sampler_record_index.find(sampler.index0);
        bound != m_sampler_record_index.end()) {
        record = bound->second;
    } else {
        const ResourceRange* range =
            find_range(DxbcRegisterClass::Sampler, sampler.index0);
        record = m_builder.emit_constant_int(
            m_type_uint, range != nullptr ? range->resolved.binding
                                          : sampler.index0);
    }
    // One record is kSamplerStateRecordWords / 4 uvec4s: word w is
    // component w % 4 of vector w / 4.
    const u32 vec = word / 4u;
    const u32 comp = word % 4u;
    std::vector<u32> indices = {m_const_u0, record};
    if (kSamplerStateRecordWords > 4u) {
        indices.push_back(m_builder.emit_constant_int(m_type_uint, vec));
    }
    indices.push_back(m_builder.emit_constant_int(m_type_uint, comp));
    u32 value = m_builder.emit_load(
        m_type_uint,
        m_builder.emit_access_chain(m_type_ptr_storage_buffer_uint,
                                    m_sampler_state_var, indices));
    if (word == kSamplerStateFlagsWord) {
        // Record producers enable these features before publishing their
        // bits. Until then a compile-time mask removes their emulation from
        // every reader, including coordinate normalization and border twins.
        const auto gate = [&](u32& id, unsigned spec_id, const char* name) {
            if (id == 0) {
                m_builder.set_section(SpirvBuilder::Section::TypesGlobals);
                id = m_builder.allocate_id();
                m_builder.emit(SpvOp::OpSpecConstantFalse, {m_type_bool, id});
                m_builder.emit_decorate(id, SpvDecoration::SpecId, spec_id);
                m_builder.emit_name(id, name);
            }
            return id;
        };
        const u32 border_gate = gate(m_custom_border_gate, kCustomBorderSpecId,
                                      kCustomBorderSpecName);
        const u32 non_norm_gate = gate(m_non_normalized_gate, kNonNormalizedSpecId,
                                        kNonNormalizedSpecName);
        const u32 border_bit = m_builder.emit_constant_int(m_type_uint, kSamplerStateCustomBorder);
        const u32 non_norm_bit = m_builder.emit_constant_int(m_type_uint, kSamplerStateNonNormalized);
        u32 allowed = m_builder.emit_constant_int(
            m_type_uint, ~(kSamplerStateCustomBorder | kSamplerStateNonNormalized));
        allowed = m_builder.emit_bitwise_or(m_type_uint, allowed,
            m_builder.emit_select(m_type_uint, border_gate, border_bit, m_const_u0));
        allowed = m_builder.emit_bitwise_or(m_type_uint, allowed,
            m_builder.emit_select(m_type_uint, non_norm_gate, non_norm_bit, m_const_u0));
        value = m_builder.emit_bitwise_and(m_type_uint, value, allowed);
    }
    return value;
}

u32 DxbcTranslator::descriptor_slot_index(bool uav, u32 range_id) {
    // A descriptor-array element's table slot was computed where the
    // instruction bound it; a single register's is its binding.
    if (const auto bound = m_view_size_index.find(range_id);
        bound != m_view_size_index.end()) {
        return bound->second;
    }
    const ResourceRange* range = find_range(
        uav ? DxbcRegisterClass::UnorderedAccess
            : DxbcRegisterClass::ShaderResource,
        range_id);
    return m_builder.emit_constant_int(
        m_type_uint, range != nullptr ? range->resolved.binding : range_id);
}

u32 DxbcTranslator::heap_window_limit() {
    // heap_window_slot declares the params buffer.
    if (m_heap_window_params_var == 0) {
        (void)fail("heap window: the limit before any window element");
        return 0;
    }
    return m_builder.emit_load(
        m_type_uint,
        m_builder.emit_access_chain(m_type_ptr_storage_buffer_uint,
                                    m_heap_window_params_var,
                                    {m_const_u0, m_const_u0}));
}

u32 DxbcTranslator::heap_window_slot(const ResourceRange& range,
                                       u32 element) {
    const auto& window = m_options.heap_window;
    if (m_heap_window_params_var == 0) {
        const u32 words = m_builder.emit_type_runtime_array(m_type_uint);
        m_builder.emit_decorate(words, SpvDecoration::ArrayStride, 4u);
        const u32 block = m_builder.emit_type_struct({words});
        m_builder.emit_decorate(block, SpvDecoration::Block);
        m_builder.emit_member_decorate(block, 0, SpvDecoration::Offset, 0);
        m_builder.emit_member_decorate(block, 0, SpvDecoration::NonWritable);
        m_heap_window_params_var = m_builder.emit_variable(
            m_builder.emit_type_pointer(SpvStorageClass::StorageBuffer, block),
            SpvStorageClass::StorageBuffer);
        m_builder.emit_name(m_heap_window_params_var,
                            "vitreo_heap_window_params");
        m_builder.emit_decorate(m_heap_window_params_var,
                                SpvDecoration::DescriptorSet,
                                window.params_set);
        m_builder.emit_decorate(m_heap_window_params_var,
                                SpvDecoration::Binding, window.params_binding);
        // SPIR-V 1.4 lists every global the entry point uses.
        m_entry_interface.push_back(m_heap_window_params_var);
    }
    const auto word = [&](u32 k) {
        return m_builder.emit_load(
            m_type_uint,
            m_builder.emit_access_chain(
                m_type_ptr_storage_buffer_uint, m_heap_window_params_var,
                {m_const_u0, m_builder.emit_constant_int(m_type_uint, k)}));
    };
    const DxbcTranslationOptions::DescriptorHeapWindow::Array* array = nullptr;
    for (const auto& candidate : window.arrays) {
        if (candidate.register_class == range.resolved.declaration.register_class &&
            candidate.space == range.resolved.declaration.space &&
            candidate.first_register == range.resolved.declaration.lower) {
            array = &candidate;
            break;
        }
    }
    if (array == nullptr) {
        (void)fail("heap window: no segments for a window array");
        return 0;
    }
    const u32 limit = word(0u);
    // Later segments first, so the first segment covering the element is
    // the last select applied and wins.
    u32 slot = limit;
    for (usize k = array->segments.size(); k-- > 0;) {
        const auto& segment = array->segments[k];
        // offset = element - first_element wraps for an element below the
        // segment, so one unsigned compare against count rejects both
        // sides. base <= limit (the frontend clamps it), so limit - base
        // does not wrap, and a slot at or past the heap's end is rejected.
        const u32 offset = m_builder.emit_isub(
            m_type_uint, element,
            m_builder.emit_constant_int(m_type_uint, segment.first_element));
        const u32 base = word(segment.base_word);
        u32 inside = m_builder.emit_integer_compare(
            SpvOp::OpULessThan, m_type_bool, offset,
            m_builder.emit_isub(m_type_uint, limit, base));
        if (segment.count != kDxbcUnboundedRange) {
            inside = m_builder.emit_logical(
                SpvOp::OpLogicalAnd,
                m_type_bool, inside,
                m_builder.emit_integer_compare(
                    SpvOp::OpULessThan, m_type_bool, offset,
                    m_builder.emit_constant_int(m_type_uint, segment.count)));
        }
        slot = m_builder.emit_select(
            m_type_uint, inside, m_builder.emit_iadd(m_type_uint, base, offset),
            slot);
    }
    return slot;
}

u32 DxbcTranslator::heap_window_side_word(u32 slot, u32 word) {
    const auto& window = m_options.heap_window;
    if (m_heap_window_side_var == 0) {
        const u32 words = m_builder.emit_type_runtime_array(m_type_uint);
        m_builder.emit_decorate(words, SpvDecoration::ArrayStride, 4u);
        const u32 block = m_builder.emit_type_struct({words});
        m_builder.emit_decorate(block, SpvDecoration::Block);
        m_builder.emit_member_decorate(block, 0, SpvDecoration::Offset, 0);
        m_builder.emit_member_decorate(block, 0, SpvDecoration::NonWritable);
        m_heap_window_side_var = m_builder.emit_variable(
            m_builder.emit_type_pointer(SpvStorageClass::StorageBuffer, block),
            SpvStorageClass::StorageBuffer);
        m_builder.emit_name(m_heap_window_side_var, "vitreo_heap_window_side");
        m_builder.emit_decorate(m_heap_window_side_var,
                                SpvDecoration::DescriptorSet, window.side_set);
        m_builder.emit_decorate(m_heap_window_side_var, SpvDecoration::Binding,
                                window.side_binding);
        m_entry_interface.push_back(m_heap_window_side_var);
    }
    const u32 index = m_builder.emit_iadd(
        m_type_uint,
        m_builder.emit_imul(
            m_type_uint, slot,
            m_builder.emit_constant_int(m_type_uint,
                                        kDescriptorHeapWindowSideWords)),
        m_builder.emit_constant_int(m_type_uint, word));
    return m_builder.emit_load(
        m_type_uint,
        m_builder.emit_access_chain(m_type_ptr_storage_buffer_uint,
                                    m_heap_window_side_var,
                                    {m_const_u0, index}));
}

u32 DxbcTranslator::view_size_words(bool uav, u32 range_id) {
    if (heap_window_key(uav, range_id)) {
        const u32 slot = descriptor_slot_index(uav, range_id);
        if (slot == 0) return 0;
        return heap_window_side_word(slot, kDescriptorHeapWindowEntryWord);
    }
    const u32 variable = uav ? m_uav_view_size_var : m_srv_view_size_var;
    if (variable == 0) {
        (void)fail("view size buffer: not declared for this register class");
        return 0;
    }
    const u32 index = descriptor_slot_index(uav, range_id);
    if (index == 0) return 0;
    auto& cached = m_view_size_values[{variable, index}];
    for (auto it = cached.rbegin(); it != cached.rend(); ++it) {
        if (m_builder.dominates_here(it->mark)) return it->value;
    }
    // Unlike guest UAV contents, these runtime-owned descriptor metadata
    // snapshots cannot change during the invocation. Explicitly reuse the
    // dominating SSA load: native alias analysis cannot establish that a
    // guest UAV store cannot reach this independent upload allocation.
    const u32 value = m_builder.emit_load(
        m_type_uint,
        m_builder.emit_access_chain(m_type_ptr_storage_buffer_uint, variable,
                                    {m_const_u0, index}));
    // Materialization hint for the MSL emitter: forwarding this immutable
    // load back into every expression would undo the explicit CSE.
    m_builder.emit_name(value, "vitreo_view_limit_snapshot");
    cached.push_back({value, m_builder.dominance_mark()});
    return value;
}

u32 DxbcTranslator::descriptor_is_null(bool uav, u32 range_id) {
    const u32 variable = uav ? m_uav_null_var : m_srv_null_var;
    if (variable == 0) return 0;
    const u32 index = descriptor_slot_index(uav, range_id);
    if (index == 0) return 0;
    const u32 flag =
        heap_window_key(uav, range_id)
            ? heap_window_side_word(index, kDescriptorHeapWindowNullWord)
            : m_builder.emit_load(
                  m_type_uint,
                  m_builder.emit_access_chain(m_type_ptr_storage_buffer_uint,
                                              variable, {m_const_u0, index}));
    return m_builder.emit_integer_compare(SpvOp::OpINotEqual, m_type_bool,
                                          flag, m_const_u0);
}

u32 DxbcTranslator::cbv_limit_index(const Sm4Operand& src) {
    const ResourceRange* range =
        find_range(DxbcRegisterClass::ConstantBuffer, src.index0);
    const u32 binding = range != nullptr ? range->resolved.binding : src.index0;
    if (src.has_register_index && range != nullptr && range->is_array) {
        const u32 element = array_element_index(src, *range);
        if (element == 0) return 0;
        u32 slot = m_builder.emit_iadd(
            m_type_uint, m_builder.emit_constant_int(m_type_uint, binding),
            element);
        if (src.nonuniform && src.register_relative) {
            m_builder.emit_capability(
                SpvCapability::StorageBufferArrayNonUniformIndexing);
            m_builder.emit_decorate(slot, SpvDecoration::NonUniform);
        }
        return slot;
    }
    return m_builder.emit_constant_int(m_type_uint, binding);
}

} // namespace vitreo::graphics::dxbc_spirv_detail
