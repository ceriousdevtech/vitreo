// SPDX-License-Identifier: LGPL-2.1-only

/// @file dxbc_to_spirv_bindings.cpp
/// @brief Register binding and the descriptor model.

#include "graphics/shaders/dxbc_to_spirv_internal.hpp"

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
    const auto entry_holding = [&](u32 reg) -> const DxbcRegisterRemap* {
        for (const auto& entry : m_options.register_remap) {
            if (entry.register_class != cls || entry.space != space ||
                reg < entry.first_register) {
                continue;
            }
            if (entry.register_count == kDxbcUnboundedRange ||
                reg - entry.first_register < entry.register_count) {
                return &entry;
            }
        }
        return nullptr;
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
        ResourceRange range{};
        range.resolved.declaration = declaration;
        range.resolved.descriptor_set = set;
        range.resolved.binding = binding;
        range.is_array = m_container.is_sm51_plus() && declaration.is_array();
        range.resolved.descriptor_count = range.is_array ? covered : 1u;
        if (!m_ranges
                 .emplace(std::pair{declaration.register_class,
                                    declaration.range_id},
                          range)
                 .second) {
            return fail(what + " is declared twice");
        }
        // Rasterizer-ordered views (dcl_uav_*_rov, OpcodeToken0 bit 17)
        // need ordered access the translation does not provide; D3D12's
        // OPTIONS::ROVsSupported is not claimed, so refusing is the
        // runtime's own answer. (A D3D11 translation still ignores the bit:
        // its OPTIONS2 reports ROVs unsupported as well.)
        if (descriptor_table_translation() && declaration.rasterizer_ordered) {
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
    return m_builder.emit_load(
        m_type_uint,
        m_builder.emit_access_chain(
            m_type_ptr_storage_buffer_uint, m_sampler_state_var,
            {m_const_u0, record, m_builder.emit_constant_int(m_type_uint, word)}));
}

} // namespace vitreo::graphics::dxbc_spirv_detail
