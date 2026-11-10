// SPDX-License-Identifier: LGPL-2.1-only

/// @file dxbc_to_spirv_tiled.cpp
/// @brief Tiled resources tier 2: shader residency status (D3D11.3 5.9.4.5)
///        and check_access_fully_mapped.

#include "graphics/shaders/dxbc_to_spirv_internal.hpp"

namespace vitreo::graphics {

bool dxbc_uses_tiled_status(const DxbcContainer& container) {
    using dxbc_spirv_detail::DxbcDecoder;
    DxbcDecoder scan(container.bytecode.data(), container.bytecode.size());
    if (!scan.has_tokens(2)) return false;
    scan.skip_tokens(2);
    while (scan.has_tokens(1)) {
        const u32 token = scan.read_token();
        const u32 opcode = token & 0x7FFu;
        u32 length = (token >> 24) & 0x7Fu;
        if (opcode == 53u) {
            // customdata: the block length follows the opcode token.
            if (!scan.has_tokens(1)) return false;
            length = scan.read_token();
            if (length < 2u) return false;
            scan.skip_tokens(length - 2u);
            continue;
        }
        // Opcodes 219-234 (dxbc_to_spirv_sm4.hpp, measured).
        if (opcode >= 219u && opcode <= 234u) return true;
        if (length == 0u || !scan.has_tokens(length - 1u)) return false;
        scan.skip_tokens(length - 1u);
    }
    return false;
}

}  // namespace vitreo::graphics

namespace vitreo::graphics::dxbc_spirv_detail {

namespace {

u32 constant_u(SpirvBuilder& b, const TiledStatusTypes& t, u32 value) {
    return b.emit_constant_int(t.type_uint, value);
}

u32 miss_of(SpirvBuilder& b, const TiledStatusTypes& t, u32 missed) {
    return b.emit_select(t.type_uint, missed, constant_u(b, t, 1u),
                         constant_u(b, t, 0u));
}

}  // namespace

u32 TiledStatus::emit_texture_status(SpirvBuilder& b, const TiledStatusTypes& t,
                                     u32 companion, u32 coordinate,
                                     const Lod& lod, u32 const_offset,
                                     u32 offset, u32 min_lod, bool gather) {
    const u32 one = b.emit_constant_float(t.type_float, 1.0f);
    if (gather) {
        // Every lane 1.0: the four texels are mapped (D3D11.3 5.9.4.5).
        const u32 lanes = b.emit_image_gather(t.type_vec4, companion, coordinate,
                                              constant_u(b, t, 0u),
                                              const_offset, offset);
        const u32 ones = b.emit_constant_composite(t.type_vec4,
                                                   {one, one, one, one});
        const u32 mapped =
            b.emit_all(t.type_bool, b.emit_ford_ge(t.type_bvec4, lanes, ones));
        return miss_of(b, t, b.emit_logical_not(t.type_bool, mapped));
    }
    u32 sampled = 0;
    switch (lod.kind) {
    case Lod::Kind::Implicit:
        sampled = b.emit_image_sample(t.type_vec4, companion, coordinate,
                                      lod.bias, const_offset, min_lod);
        break;
    case Lod::Kind::Explicit:
        // sample_l / sample_c_lz have no clamp form (min_lod is 0 here).
        sampled = b.emit_image_sample_explicit_lod(t.type_vec4, companion,
                                                   coordinate, lod.lod,
                                                   const_offset);
        break;
    case Lod::Kind::Gradient:
        sampled = b.emit_image_sample_grad(t.type_vec4, companion, coordinate,
                                           lod.grad_x, lod.grad_y,
                                           const_offset, min_lod);
        break;
    }
    // The companion holds 1.0 in mapped tiles and 0.0 elsewhere, so the
    // filtered value is the weight on mapped texels: 1.0 exactly when no
    // texel with weight is unmapped.
    const u32 value = b.emit_composite_extract(t.type_float, sampled, {0u});
    return miss_of(b, t, b.emit_ford_lt(t.type_bool, value, one));
}

u32 TiledStatus::emit_texel_status(SpirvBuilder& b, const TiledStatusTypes& t,
                                   u32 image, u32 coordinate, u32 components,
                                   u32 lod_or_sample, bool multisampled) {
    b.emit_capability(SpvCapability::ImageQuery);
    const u32 vec_type = components == 3u ? t.type_uvec3 : t.type_uvec2;
    const u32 bvec_type = components == 3u ? t.type_bvec3 : t.type_bvec2;
    u32 in_range = 0;
    u32 index = lod_or_sample;
    if (multisampled) {
        const u32 size = b.emit_image_query_size(vec_type, image);
        const u32 samples = b.emit_image_query_samples(t.type_uint, image);
        in_range = b.emit_logical(
            SpvOp::OpLogicalAnd, t.type_bool,
            b.emit_all(t.type_bool,
                       b.emit_integer_compare(SpvOp::OpULessThan, bvec_type,
                                              coordinate, size)),
            b.emit_integer_compare(SpvOp::OpULessThan, t.type_bool,
                                   lod_or_sample, samples));
    } else {
        const u32 levels = b.emit_image_query_levels(t.type_uint, image);
        const u32 level_ok = b.emit_integer_compare(
            SpvOp::OpULessThan, t.type_bool, lod_or_sample, levels);
        index = b.emit_select(t.type_uint, level_ok, lod_or_sample,
                              constant_u(b, t, 0u));
        const u32 size = b.emit_image_query_size(vec_type, image, index);
        in_range = b.emit_logical(
            SpvOp::OpLogicalAnd, t.type_bool, level_ok,
            b.emit_all(t.type_bool,
                       b.emit_integer_compare(SpvOp::OpULessThan, bvec_type,
                                              coordinate, size)));
    }
    // Outside the view the load reads 0 without touching a tile: not a miss.
    // The fetch itself stays inside the image.
    const u32 keep = b.emit_select(t.type_uint, in_range,
                                   constant_u(b, t, 0xFFFFFFFFu),
                                   constant_u(b, t, 0u));
    std::vector<u32> keeps(components, keep);
    const u32 safe = b.emit_bitwise_and(
        vec_type, coordinate, b.emit_composite_construct(vec_type, keeps));
    const u32 safe_index = b.emit_bitwise_and(t.type_uint, index, keep);
    const u32 texel = multisampled
        ? b.emit_image_fetch_sample(t.type_vec4, image, safe, safe_index)
        : b.emit_image_fetch(t.type_vec4, image, safe, safe_index);
    const u32 value = b.emit_composite_extract(t.type_float, texel, {0u});
    const u32 unmapped = b.emit_ford_lt(
        t.type_bool, value, b.emit_constant_float(t.type_float, 1.0f));
    return miss_of(b, t, b.emit_logical(SpvOp::OpLogicalAnd, t.type_bool,
                                        in_range, unmapped));
}

u32 TiledStatus::emit_buffer_status(SpirvBuilder& b, const TiledStatusTypes& t,
                                    u32 residency, u32 slot, u32 first,
                                    u32 last) {
    const u32 zero = constant_u(b, t, 0u);
    const auto word = [&](u32 index) {
        return b.emit_load(t.type_uint,
                           b.emit_access_chain(t.ptr_storage_buffer_uint,
                                               residency, {zero, index}));
    };
    const auto at = [&](u32 base, u32 offset) {
        return b.emit_iadd(t.type_uint, base, constant_u(b, t, offset));
    };
    const auto ult = [&](u32 x, u32 y) {
        return b.emit_integer_compare(SpvOp::OpULessThan, t.type_bool, x, y);
    };
    // Slot 0 of a header with no record reads words 0..7, which exist
    // (kTiledResidencyMinWords); every index below stays inside the record.
    const u32 record = word(slot);
    const u32 tiled = b.emit_integer_compare(SpvOp::OpINotEqual, t.type_bool,
                                             record, zero);
    const u32 tiles = b.emit_select(
        t.type_uint, tiled, word(at(record, kTiledResidencyTiles)), zero);
    const u32 first_tile = word(at(record, kTiledResidencyFirstTile));
    const u32 start = word(at(record, kTiledResidencyStartByte));
    const u32 units = word(at(record, kTiledResidencyViewUnits));
    const u32 unit_bytes = word(at(record, kTiledResidencyUnitBytes));
    const u32 in_view = ult(first, units);
    // The last unit inside the view (units >= 1 whenever in_view).
    const u32 view_last = b.emit_isub(t.type_uint, units, constant_u(b, t, 1u));
    const u32 last_in = b.emit_select(t.type_uint, ult(last, units), last,
                                      view_last);
    const u32 first_byte = b.emit_imul(t.type_uint, first, unit_bytes);
    const u32 last_byte = b.emit_isub(
        t.type_uint,
        b.emit_iadd(t.type_uint, b.emit_imul(t.type_uint, last_in, unit_bytes),
                    unit_bytes),
        constant_u(b, t, 1u));
    const u32 sixteen = constant_u(b, t, 16u);
    const auto tile_missing = [&](u32 byte) {
        // tile = first_tile + byte / 64 KiB + carry of (start + byte % 64 KiB)
        const u32 low = b.emit_bitwise_and(t.type_uint, byte,
                                           constant_u(b, t, 0xFFFFu));
        const u32 tile = b.emit_iadd(
            t.type_uint,
            b.emit_iadd(t.type_uint, first_tile,
                        b.emit_shift_right_logical(t.type_uint, byte, sixteen)),
            b.emit_shift_right_logical(
                t.type_uint, b.emit_iadd(t.type_uint, start, low), sixteen));
        const u32 inside = ult(tile, tiles);
        const u32 safe = b.emit_select(t.type_uint, inside, tile, zero);
        const u32 bits = word(b.emit_iadd(
            t.type_uint, at(record, kTiledResidencyBits),
            b.emit_shift_right_logical(t.type_uint, safe,
                                       constant_u(b, t, 5u))));
        const u32 bit = b.emit_bitwise_and(
            t.type_uint,
            b.emit_shift_right_logical(
                t.type_uint, bits,
                b.emit_bitwise_and(t.type_uint, safe, constant_u(b, t, 31u))),
            constant_u(b, t, 1u));
        return b.emit_logical(
            SpvOp::OpLogicalAnd, t.type_bool, inside,
            b.emit_integer_compare(SpvOp::OpIEqual, t.type_bool, bit, zero));
    };
    const u32 missing =
        b.emit_logical(SpvOp::OpLogicalOr, t.type_bool, tile_missing(first_byte),
                       tile_missing(last_byte));
    return miss_of(b, t,
                   b.emit_logical(SpvOp::OpLogicalAnd, t.type_bool, tiled,
                                  b.emit_logical(SpvOp::OpLogicalAnd,
                                                 t.type_bool, in_view,
                                                 missing)));
}

u32 TiledStatus::emit_check_fully_mapped(SpirvBuilder& b,
                                         const TiledStatusTypes& t,
                                         u32 status) {
    // D3D11.3 5.9.4.5: true (all bits set) when the status reports no miss.
    return b.emit_select(
        t.type_uint,
        b.emit_integer_compare(SpvOp::OpIEqual, t.type_bool, status,
                               constant_u(b, t, 0u)),
        constant_u(b, t, 0xFFFFFFFFu), constant_u(b, t, 0u));
}

TiledStatusTypes DxbcTranslator::tiled_status_types() const {
    TiledStatusTypes t;
    t.type_bool = m_type_bool;
    t.type_uint = m_type_uint;
    t.type_float = m_type_float;
    t.type_vec4 = m_type_vec4;
    t.type_bvec4 = m_type_bvec4;
    t.type_uvec2 = m_type_uvec2;
    t.type_uvec3 = m_type_uvec3;
    t.type_bvec2 = m_type_bvec2;
    t.type_bvec3 = m_type_bvec3;
    t.ptr_storage_buffer_uint = m_type_ptr_storage_buffer_uint;
    return t;
}

bool DxbcTranslator::declare_tiled_status_variables() {
    const auto& options = m_options.tiled_status;
    if (!options.enabled) return true;
    // A twin with the shape of the range it follows: one descriptor, or an
    // array of the same length at the same offset from its base.
    const auto declare = [&](DxbcRegisterClass cls, u32 key, u32 set, u32 base,
                             u32 element_pointer, const std::string& name) {
        const ResourceRange* range = find_range(cls, key);
        const u32 binding = range != nullptr ? range->resolved.binding : key;
        u32 type = element_pointer;
        if (range != nullptr && range->is_array) {
            const u32 count = range->resolved.descriptor_count;
            const u32 element = m_builder.pointee_type(element_pointer);
            const u32 array = count == 0u
                ? m_builder.emit_type_runtime_array(element)
                : m_builder.emit_type_array(
                      element, m_builder.emit_constant_int(m_type_uint, count));
            if (count == 0u) {
                m_builder.emit_capability(SpvCapability::RuntimeDescriptorArray);
            }
            m_builder.emit_capability(
                SpvCapability::SampledImageArrayDynamicIndexing);
            type = m_builder.emit_type_pointer(SpvStorageClass::UniformConstant,
                                               array);
        }
        const u32 variable =
            m_builder.emit_variable(type, SpvStorageClass::UniformConstant);
        m_builder.emit_name(variable, name);
        m_builder.emit_decorate(variable, SpvDecoration::DescriptorSet, set);
        m_builder.emit_decorate(variable, SpvDecoration::Binding, base + binding);
        return variable;
    };
    // A descriptor-heap window's companions (heap_window) are its
    // auxiliary region: one array aliasing the window's argument buffer,
    // element limit + 1 + k heap slot k's companion
    // (kDescriptorHeapWindowAuxBinding).
    const auto declare_window = [&](DxbcRegisterClass cls, u32 key,
                                    u32 element_pointer,
                                    const std::string& name) -> u32 {
        const ResourceRange* range = find_range(cls, key);
        if (range == nullptr) return 0;
        return declare_heap_window_aux(range->resolved.binding,
                                       m_builder.pointee_type(element_pointer),
                                       SpvStorageClass::UniformConstant, name);
    };
    // The companion's float image of t#'s declared shape.
    for (const auto& [slot, var] : m_texture_vars) {
        (void)var;
        const bool ms = m_texture_multisampled.contains(slot) &&
                        !m_texture_single_sample_ms.contains(slot);
        const bool arrayed = m_texture_arrays.contains(slot);
        const SpvDim dim = m_texture_dimensions.at(slot);
        const u32 pointer = ms
            ? (arrayed ? m_type_ptr_uniform_constant_ms_image_2d_array
                       : m_type_ptr_uniform_constant_ms_image_2d)
            : dim == SpvDim::DimCube
                ? (arrayed ? m_type_ptr_uniform_constant_image_cube_array
                           : m_type_ptr_uniform_constant_image_cube)
            : dim == SpvDim::Dim3D ? m_type_ptr_uniform_constant_image_3d
            : arrayed ? m_type_ptr_uniform_constant_image_2d_array
                      : m_type_ptr_uniform_constant_image_2d;
        if (heap_window_key(false, slot)) {
            const u32 window = declare_window(
                DxbcRegisterClass::ShaderResource, slot, pointer,
                "T" + std::to_string(slot) + "_residency");
            if (window == 0) return false;
            m_srv_companion_vars[slot] = window;
            continue;
        }
        m_srv_companion_vars[slot] =
            declare(DxbcRegisterClass::ShaderResource, slot, 1u,
                    options.srv_companion_base, pointer,
                    "t" + std::to_string(slot) + "_residency");
    }
    for (const auto& [slot, var] : m_uav_vars) {
        (void)var;
        if (m_buffer_uav_slots.contains(slot)) continue;
        const u32 pointer = m_volume_uav_slots.contains(slot)
            ? m_type_ptr_uniform_constant_image_3d
            : m_array_uav_slots.contains(slot)
                ? m_type_ptr_uniform_constant_image_2d_array
                : m_type_ptr_uniform_constant_image_2d;
        if (heap_window_key(true, slot)) {
            const u32 window = declare_window(
                DxbcRegisterClass::UnorderedAccess, slot, pointer,
                "U" + std::to_string(slot) + "_residency");
            if (window == 0) return false;
            m_uav_companion_vars[slot] = window;
            continue;
        }
        m_uav_companion_vars[slot] =
            declare(DxbcRegisterClass::UnorderedAccess, slot, 1u,
                    options.uav_companion_base, pointer,
                    "u" + std::to_string(slot) + "_residency");
    }
    for (const auto& [slot, var] : m_sampler_vars) {
        (void)var;
        m_status_sampler_vars[slot] =
            declare(DxbcRegisterClass::Sampler, slot, 2u,
                    options.status_sampler_base,
                    m_type_ptr_uniform_constant_sampler,
                    "s" + std::to_string(slot) + "_status_twin");
    }
    const auto declare_residency = [&](bool which, u32 set, u32 binding,
                                       bool any, const char* name) -> u32 {
        if (!which || !any) return 0;
        const u32 words = m_builder.emit_type_runtime_array(m_type_uint);
        m_builder.emit_decorate(words, SpvDecoration::ArrayStride, 4u);
        const u32 block = m_builder.emit_type_struct({words});
        m_builder.emit_decorate(block, SpvDecoration::Block);
        m_builder.emit_member_decorate(block, 0, SpvDecoration::Offset, 0);
        m_builder.emit_member_decorate(block, 0, SpvDecoration::NonWritable);
        const u32 variable = m_builder.emit_variable(
            m_builder.emit_type_pointer(SpvStorageClass::StorageBuffer, block),
            SpvStorageClass::StorageBuffer);
        m_builder.emit_name(variable, name);
        m_builder.emit_decorate(variable, SpvDecoration::DescriptorSet, set);
        m_builder.emit_decorate(variable, SpvDecoration::Binding, binding);
        return variable;
    };
    m_srv_residency_var = declare_residency(
        options.srv_residency, options.srv_residency_set,
        options.srv_residency_binding,
        !m_raw_srv_vars.empty() || !m_structured_srv_vars.empty() ||
            !m_typed_buffer_srv_vars.empty(),
        "vitreo_srv_residency");
    bool buffer_uav = !m_raw_uav_vars.empty() || !m_structured_uav_vars.empty();
    for (const auto& [slot, var] : m_uav_vars) {
        (void)var;
        if (m_buffer_uav_slots.contains(slot)) buffer_uav = true;
    }
    m_uav_residency_var = declare_residency(
        options.uav_residency, options.uav_residency_set,
        options.uav_residency_binding, buffer_uav, "vitreo_uav_residency");
    return true;
}

bool DxbcTranslator::decode_tiled_status() {
    if (!m_tiled.active) return true;
    if (!m_decoder.decode_operand(m_tiled.status)) return false;
    if (m_tiled.status.type == Sm4OperandType::Null) return true;
    // Measured (dxbc_tiled_opcode_probe): a temp with one component.
    const u32 mask = m_tiled.status.write_mask;
    if (m_tiled.status.modifier != 0u || mask == 0u ||
        (mask & (mask - 1u)) != 0u) {
        m_tiled.error = "tiled status: the status destination is not one "
                        "component";
        return fail(m_tiled.error);
    }
    if (!m_options.tiled_status.enabled) {
        m_tiled.error = "tiled status: the residency status needs "
                        "DxbcTranslationOptions::tiled_status";
        return fail(m_tiled.error);
    }
    m_tiled.has_status = true;
    return true;
}

bool DxbcTranslator::decode_tiled_clamp(u32& min_lod) {
    min_lod = 0;
    if (!m_tiled.active || !m_tiled.clamp) return true;
    Sm4Operand clamp;
    if (!m_decoder.decode_operand(clamp)) {
        return fail("sample_cl: bad MinLOD clamp operand");
    }
    u32 clamp4 = 0;
    if (!load_vec4(clamp, clamp4)) return false;
    min_lod = m_builder.emit_composite_extract(m_type_float, clamp4, {0u});
    return true;
}

bool DxbcTranslator::store_tiled_status(u32 miss) {
    if (!m_tiled.has_status) return true;
    // The status is an integer; the instruction's saturate is its value's.
    const bool saturate = m_saturate_destination;
    m_saturate_destination = false;
    const u32 bits = m_builder.emit_bitcast(m_type_float, miss);
    const bool stored = store_vec4(
        m_tiled.status,
        m_builder.emit_composite_construct(m_type_vec4,
                                           {bits, bits, bits, bits}));
    m_saturate_destination = saturate;
    return stored;
}

bool DxbcTranslator::translate_check_access_fully_mapped() {
    // check_access_fully_mapped dest[.mask], src0.select_component.
    Sm4Operand dst, src;
    if (!m_decoder.decode_operand(dst) || !m_decoder.decode_operand(src)) {
        return fail("CHECK_ACCESS_FULLY_MAPPED: bad operands");
    }
    u32 src4 = 0;
    if (!load_vec4(src, src4)) return false;
    const u32 status = m_builder.emit_composite_extract(
        m_type_uint, m_builder.emit_bitcast(m_type_uvec4, src4), {0u});
    const u32 result = m_builder.emit_bitcast(
        m_type_float,
        TiledStatus::emit_check_fully_mapped(m_builder, tiled_status_types(),
                                             status));
    return store_vec4(dst, m_builder.emit_composite_construct(
                               m_type_vec4, {result, result, result, result}));
}

u32 DxbcTranslator::tiled_companion(const Sm4Operand& resource, bool uav,
                                    u32& sampled_type, bool& multisampled) {
    auto& vars = uav ? m_uav_companion_vars : m_srv_companion_vars;
    const auto found = vars.find(resource.index0);
    if (found == vars.end()) {
        (void)fail(std::string("tiled status: ") + (uav ? "u" : "t") +
                   std::to_string(resource.index0) + " has no residency companion");
        return 0;
    }
    const u32 slot = resource.index0;
    u32 image_type = 0;
    multisampled = false;
    if (uav) {
        image_type = m_volume_uav_slots.contains(slot) ? m_type_image_3d
                     : m_array_uav_slots.contains(slot) ? m_type_image_2d_array
                                                        : m_type_image_2d;
        sampled_type = 0;
    } else {
        const bool arrayed = m_texture_arrays.contains(slot);
        const SpvDim dim = m_texture_dimensions.at(slot);
        multisampled = m_texture_multisampled.contains(slot) &&
                       !m_texture_single_sample_ms.contains(slot);
        if (multisampled) {
            image_type = arrayed ? m_type_ms_image_2d_array : m_type_ms_image_2d;
            sampled_type = 0;
        } else if (dim == SpvDim::DimCube) {
            image_type = arrayed ? m_type_image_cube_array : m_type_image_cube;
            sampled_type = arrayed ? m_type_sampled_image_cube_array
                                   : m_type_sampled_image_cube;
        } else if (dim == SpvDim::Dim3D) {
            image_type = m_type_image_3d;
            sampled_type = m_type_sampled_image_3d;
        } else {
            image_type = arrayed ? m_type_image_2d_array : m_type_image_2d;
            sampled_type = arrayed ? m_type_sampled_image_2d_array
                                   : m_type_sampled_image_2d;
        }
    }
    u32 pointer = found->second;
    const ResourceRange* range = find_range(
        uav ? DxbcRegisterClass::UnorderedAccess
            : DxbcRegisterClass::ShaderResource,
        slot);
    if (range != nullptr && range->is_array) {
        // The element the instruction names: its table slot minus the
        // range's binding (descriptor_slot_index). A heap window element's
        // slot is its heap slot, whose companion is in the auxiliary region.
        const u32 table_slot = descriptor_slot_index(uav, slot);
        if (table_slot == 0) return 0;
        const u32 element =
            is_heap_window(*range)
                ? heap_window_aux_index(table_slot)
                : m_builder.emit_isub(
                      m_type_uint, table_slot,
                      m_builder.emit_constant_int(m_type_uint,
                                                  range->resolved.binding));
        if (element == 0) return 0;
        pointer = m_builder.emit_access_chain(
            m_builder.emit_type_pointer(SpvStorageClass::UniformConstant,
                                        image_type),
            found->second, {element});
        if (resource.nonuniform && resource.register_relative) {
            m_builder.emit_capability(
                SpvCapability::SampledImageArrayNonUniformIndexing);
            m_builder.emit_decorate(pointer, SpvDecoration::NonUniform);
        }
    }
    const u32 image = m_builder.emit_load(image_type, pointer);
    if (resource.nonuniform && resource.register_relative) {
        m_builder.emit_decorate(image, SpvDecoration::NonUniform);
    }
    return image;
}

u32 DxbcTranslator::tiled_status_sampler(const Sm4Operand& sampler) {
    const auto found = m_status_sampler_vars.find(sampler.index0);
    if (found == m_status_sampler_vars.end()) {
        (void)fail("tiled status: s" + std::to_string(sampler.index0) +
                   " has no status twin");
        return 0;
    }
    u32 pointer = found->second;
    const ResourceRange* range =
        find_range(DxbcRegisterClass::Sampler, sampler.index0);
    if (range != nullptr && range->is_array) {
        // The record index is binding + element (translate_instruction).
        u32 element = m_const_u0;
        if (const auto rec = m_sampler_record_index.find(sampler.index0);
            rec != m_sampler_record_index.end()) {
            element = m_builder.emit_isub(
                m_type_uint, rec->second,
                m_builder.emit_constant_int(m_type_uint, range->resolved.binding));
        } else {
            (void)fail("tiled status: a sampler array element without its "
                       "record index");
            return 0;
        }
        pointer = m_builder.emit_access_chain(
            m_type_ptr_uniform_constant_sampler, found->second, {element});
        if (sampler.nonuniform && sampler.register_relative) {
            m_builder.emit_capability(
                SpvCapability::SampledImageArrayNonUniformIndexing);
            m_builder.emit_decorate(pointer, SpvDecoration::NonUniform);
        }
    }
    return m_builder.emit_load(m_type_sampler, pointer);
}

u32 DxbcTranslator::tiled_buffer_status(bool uav, u32 range_id, u32 first,
                                        u32 last) {
    if (heap_window_key(uav, range_id)) {
        // The element's record id (side word kDescriptorHeapWindowResidency
        // Word) indexes the window's residency buffer, whose header word r
        // is record r's offset (0: not a tiled buffer, fully mapped).
        const u32 slot = descriptor_slot_index(uav, range_id);
        if (slot == 0) return 0;
        const u32 record =
            heap_window_side_word(slot, kDescriptorHeapWindowResidencyWord);
        if (m_heap_window_residency_var == 0) {
            const auto& window = m_options.heap_window;
            const u32 words = m_builder.emit_type_runtime_array(m_type_uint);
            m_builder.emit_decorate(words, SpvDecoration::ArrayStride, 4u);
            const u32 block = m_builder.emit_type_struct({words});
            m_builder.emit_decorate(block, SpvDecoration::Block);
            m_builder.emit_member_decorate(block, 0, SpvDecoration::Offset, 0);
            m_builder.emit_member_decorate(block, 0, SpvDecoration::NonWritable);
            m_heap_window_residency_var = m_builder.emit_variable(
                m_builder.emit_type_pointer(SpvStorageClass::StorageBuffer, block),
                SpvStorageClass::StorageBuffer);
            m_builder.emit_name(m_heap_window_residency_var,
                                "vitreo_heap_window_residency");
            m_builder.emit_decorate(m_heap_window_residency_var,
                                    SpvDecoration::DescriptorSet,
                                    window.residency_set);
            m_builder.emit_decorate(m_heap_window_residency_var,
                                    SpvDecoration::Binding,
                                    window.residency_binding);
            m_entry_interface.push_back(m_heap_window_residency_var);
        }
        return TiledStatus::emit_buffer_status(m_builder, tiled_status_types(),
                                               m_heap_window_residency_var,
                                               record, first, last);
    }
    const u32 residency = uav ? m_uav_residency_var : m_srv_residency_var;
    if (residency == 0) {
        (void)fail(std::string("tiled status: no ") + (uav ? "UAV" : "SRV") +
                   " buffer residency binding");
        return 0;
    }
    const u32 slot = descriptor_slot_index(uav, range_id);
    if (slot == 0) return 0;
    return TiledStatus::emit_buffer_status(m_builder, tiled_status_types(),
                                           residency, slot, first, last);
}

u32 DxbcTranslator::tiled_texel_status(const Sm4Operand& resource, bool uav,
                                       u32 coordinate, u32 components,
                                       u32 lod_or_sample) {
    u32 sampled_type = 0;
    bool multisampled = false;
    const u32 image = tiled_companion(resource, uav, sampled_type, multisampled);
    if (image == 0) return 0;
    return TiledStatus::emit_texel_status(m_builder, tiled_status_types(),
                                          image, coordinate, components,
                                          lod_or_sample, multisampled);
}

u32 DxbcTranslator::tiled_words_status(bool uav, u32 range_id, u32 base_byte,
                                       u32 write_mask, u32 swizzle) {
    u32 low = 3u;
    u32 high = 0u;
    for (u32 lane = 0; lane < 4u; ++lane) {
        if ((write_mask & (1u << lane)) == 0u) continue;
        const u32 source = (swizzle >> (lane * 2u)) & 0x3u;
        low = std::min(low, source);
        high = std::max(high, source);
    }
    if (low > high) low = high = 0u;
    const u32 first = m_builder.emit_iadd(
        m_type_uint, base_byte, m_builder.emit_constant_int(m_type_uint, low * 4u));
    const u32 last = m_builder.emit_iadd(
        m_type_uint, base_byte,
        m_builder.emit_constant_int(m_type_uint, high * 4u + 3u));
    return tiled_buffer_status(uav, range_id, first, last);
}

u32 DxbcTranslator::tiled_offset_texel(u32 coordinate, u32 components,
                                       u32 sample_controls, bool texture_1d,
                                       bool volume) {
    if (sample_controls == 0u) return coordinate;
    const auto nibble = [&](u32 shift) {
        const u32 raw = (sample_controls >> shift) & 0xFu;
        return raw >= 8u ? raw - 16u : raw;  // two's complement in a u32
    };
    std::vector<u32> offsets{
        m_builder.emit_constant_int(m_type_uint, nibble(9)),
        m_builder.emit_constant_int(m_type_uint, texture_1d ? 0u : nibble(13))};
    if (components == 3u) {
        offsets.push_back(
            m_builder.emit_constant_int(m_type_uint, volume ? nibble(17) : 0u));
    }
    const u32 type = components == 3u ? m_type_uvec3 : m_type_uvec2;
    // Wrapping add: a negative offset below 0 becomes a huge texel, outside.
    return m_builder.emit_iadd(type, coordinate,
                               m_builder.emit_constant_composite(type, offsets));
}

}  // namespace vitreo::graphics::dxbc_spirv_detail
