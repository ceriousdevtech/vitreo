// SPDX-License-Identifier: LGPL-2.1-only

/// @file dxbc_to_spirv_op_typed.cpp
/// @brief Typed resource load and store opcodes.

#include "graphics/shaders/dxbc_to_spirv_internal.hpp"

namespace vitreo::graphics::dxbc_spirv_detail {

bool DxbcTranslator::translate_typed_resource_operation(u32 opcode_token,
                                                        Sm4Opcode opcode,
                                                        u32 sample_controls) {
    switch (opcode) {
    case Sm4Opcode::LD: {
        // Learn ld: fetch an unfiltered texel using unsigned integer address
        // components. Texture2D uses xy, Texture2DArray/Texture3D use xyz,
        // and address.w selects the mip. No sampler participates.
        Sm4Operand dst, address, resource;
        if (!m_decoder.decode_operand(dst) ||
            !m_decoder.decode_operand(address) ||
            !m_decoder.decode_operand(resource)) {
            return fail("LD: bad operands");
        }
        // D3D11.3 22.4.6: ld takes no source modifiers.
        if (address.modifier != 0u || resource.modifier != 0u) {
            return fail("LD: source modifier on an operand that takes none");
        }
        if (resource.type != Sm4OperandType::Resource) {
            return fail("LD: expected texture resource operand");
        }
        // A typed buffer is read by the same ld opcode as a texture, with
        // address.x holding the element index and the remaining address
        // components unused — a buffer image has no mip chain, so address.w is
        // not a mip select and SPIR-V forbids a Lod image operand here. The
        // hardware expands the fetched texel to four components using the
        // view's format, which is Learn's Buffer.Load contract exactly.
        if (const auto typed_buffer =
                m_typed_buffer_srv_vars.find(resource.index0);
            typed_buffer != m_typed_buffer_srv_vars.end()) {
            const u32 return_type =
                m_typed_buffer_srv_return_types[resource.index0];
            const u32 image_type = return_type == 4u
                ? m_type_buffer_image_uint
                : return_type == 3u ? m_type_buffer_image_int
                                    : m_type_buffer_image_float;
            const u32 result_type = return_type == 4u
                ? m_type_uvec4
                : return_type == 3u ? m_type_ivec4 : m_type_vec4;
            u32 address_bits = 0;
            if (!load_vec4(address, address_bits)) return false;
            const u32 coordinate = m_builder.emit_composite_extract(
                m_type_uint,
                m_builder.emit_bitcast(m_type_uvec4, address_bits), {0u});
            const u32 image =
                m_builder.emit_load(image_type, typed_buffer->second);
            // The view's size-table entry (typed_buffer_bias): the bound
            // texture starts its element bias in front of FirstElement.
            const u32 entry = typed_buffer_entry(false, resource.index0);
            const u32 bias = typed_buffer_bias(entry);
            u32 loaded = 0;
            if (rgb32_buffer_srv(resource.index0)) {
                loaded = load_rgb32_buffer_element(image, result_type,
                                                   return_type, coordinate,
                                                   bias);
            } else if (entry != 0) {
                // Learn (ld; D3D11.3 22.4.6, 19.1.3.3): past the view, 0 in
                // the format's components and the default in the missing
                // ones. Tested on the index itself (element + bias would wrap
                // an index near 2^32 into the elements in front of the view)
                // and nothing is fetched for it: past NumElements the texture
                // either ends or holds other elements of the buffer.
                const u32 count = typed_buffer_count(image, bias);
                const u32 in_view = m_builder.emit_integer_compare(
                    SpvOp::OpULessThan, m_type_bool, coordinate, count);
                const u32 read_block = m_builder.allocate_id();
                const u32 outside_block = m_builder.allocate_id();
                const u32 merge_block = m_builder.allocate_id();
                m_builder.emit_selection_merge(merge_block);
                m_builder.emit_branch_conditional(in_view, read_block,
                                                  outside_block);
                m_builder.emit_label_id(read_block);
                const u32 fetched = m_builder.emit_image_fetch_plain(
                    result_type, image,
                    m_builder.emit_iadd(m_type_uint, coordinate, bias));
                m_builder.emit_branch(merge_block);
                m_builder.emit_label_id(outside_block);
                const u32 outside =
                    typed_buffer_out_of_view(result_type, return_type, entry);
                m_builder.emit_branch(merge_block);
                m_builder.emit_label_id(merge_block);
                loaded = m_builder.emit_phi(
                    result_type,
                    {{fetched, read_block}, {outside, outside_block}});
            } else {
                loaded = m_builder.emit_image_fetch_plain(result_type, image,
                                                          coordinate);
            }
            if (return_type == 4u || return_type == 3u) {
                loaded = m_builder.emit_bitcast(m_type_vec4, loaded);
            }
            loaded = apply_swizzle(loaded, resource.swizzle);
            if (!store_vec4(dst, loaded)) return false;
            break;
        }
        const auto texture_it = m_texture_vars.find(resource.index0);
        if (texture_it == m_texture_vars.end()) {
            return fail("LD: undeclared texture slot");
        }
        const SpvDim dimension = m_texture_dimensions.at(resource.index0);
        if (dimension == SpvDim::DimCube) {
            return fail("LD: TextureCube resources are not supported by D3D");
        }

        u32 address4 = 0;
        if (!load_vec4(address, address4)) return false;
        // Learn (ld): a Texture1D takes u from x and ignores yz; a
        // Texture1DArray takes its slice from y. The mip is always w.
        const bool texture_1d = m_texture_1d.contains(resource.index0);
        if (texture_1d) {
            address4 = widen_1d_address(
                address4, m_texture_arrays.contains(resource.index0),
                /*normalized=*/false);
        }
        const u32 address_u4 =
            m_builder.emit_bitcast(m_type_uvec4, address4);
        const u32 x = m_builder.emit_composite_extract(
            m_type_uint, address_u4, {0});
        const u32 y = m_builder.emit_composite_extract(
            m_type_uint, address_u4, {1});
        const u32 mip = m_builder.emit_composite_extract(
            m_type_uint, address_u4, {3});
        const bool volume = dimension == SpvDim::Dim3D;
        const bool arrayed = m_texture_arrays.contains(resource.index0);
        u32 coordinate = 0;
        if (volume || arrayed) {
            const u32 z = m_builder.emit_composite_extract(
                m_type_uint, address_u4, {2});
            coordinate = m_builder.emit_composite_construct(
                m_type_uvec3, {x, y, z});
        } else {
            coordinate = m_builder.emit_composite_construct(
                m_type_uvec2, {x, y});
        }
        const u32 return_type = m_texture_return_types.at(resource.index0);
        const bool uint_result = return_type == 4;
        const bool sint_result = return_type == 3;
        const u32 image_type = uint_result
            ? (arrayed ? m_type_uimage_2d_array
                       : volume ? m_type_uimage_3d : m_type_uimage_2d)
            : sint_result
            ? (arrayed ? m_type_iimage_2d_array
                       : volume ? m_type_iimage_3d : m_type_iimage_2d)
            : (arrayed ? m_type_image_2d_array
                       : volume ? m_type_image_3d : m_type_image_2d);
        const u32 result_type = uint_result ? m_type_uvec4
            : sint_result ? m_type_ivec4 : m_type_vec4;
        const u32 image = m_builder.emit_load(image_type, texture_it->second);
        u32 const_offset = 0;
        if (sample_controls != 0) {
            const auto decode_offset = [&](u32 shift) -> i32 {
                const u32 nibble = (sample_controls >> shift) & 0xFu;
                return static_cast<i32>(nibble) -
                       ((nibble & 0x8u) != 0 ? 16 : 0);
            };
            const u32 ox = m_builder.emit_constant_int(
                m_type_int, static_cast<u32>(decode_offset(9)));
            // Learn (ld): "The _aoffimmi v,w components are ignored for
            // texture1Ds."
            const u32 oy = m_builder.emit_constant_int(
                m_type_int,
                texture_1d ? 0u : static_cast<u32>(decode_offset(13)));
            if (volume) {
                const u32 oz = m_builder.emit_constant_int(
                    m_type_int, static_cast<u32>(decode_offset(17)));
                const_offset = m_builder.emit_constant_composite(
                    m_type_ivec3, {ox, oy, oz});
            } else {
                // Array axes are not offset; Texture2D ignores W.
                const_offset = m_builder.emit_constant_composite(
                    m_type_ivec2, {ox, oy});
            }
        }
        u32 loaded = m_builder.emit_image_fetch(
            result_type, image, coordinate, mip, const_offset);
        if (uint_result || sint_result) {
            loaded = m_builder.emit_bitcast(m_type_vec4, loaded);
        }
        loaded = apply_swizzle(loaded, resource.swizzle);
        if (!store_vec4(dst, loaded)) return false;
        break;
    }

    case Sm4Opcode::LD_MS: {
        // Learn ld2dms: unsigned xy texel address, optional z array slice,
        // and a separate scalar sample index. Mip/address.w is ignored.
        Sm4Operand dst, address, resource, sample_operand;
        if (!m_decoder.decode_operand(dst) ||
            !m_decoder.decode_operand(address) ||
            !m_decoder.decode_operand(resource) ||
            !m_decoder.decode_operand(sample_operand)) {
            return fail("LD_MS: bad operands");
        }
        if (address.modifier != 0u || sample_operand.modifier != 0u) {
            return fail("LD_MS: source modifier on an operand that takes none");
        }
        if (resource.type != Sm4OperandType::Resource ||
            !m_texture_multisampled.contains(resource.index0)) {
            return fail("LD_MS: expected a multisampled texture resource");
        }
        const auto texture_it = m_texture_vars.find(resource.index0);
        if (texture_it == m_texture_vars.end()) {
            return fail("LD_MS: undeclared texture slot");
        }
        u32 address4 = 0, sample4 = 0;
        if (!load_vec4(address, address4) ||
            !load_vec4(sample_operand, sample4)) return false;
        const u32 address_u4 = m_builder.emit_bitcast(m_type_uvec4, address4);
        const u32 sample_u4 = m_builder.emit_bitcast(m_type_uvec4, sample4);
        const u32 x = m_builder.emit_composite_extract(
            m_type_uint, address_u4, {0});
        const u32 y = m_builder.emit_composite_extract(
            m_type_uint, address_u4, {1});
        const u32 sample = m_builder.emit_composite_extract(
            m_type_uint, sample_u4, {0});
        const bool arrayed = m_texture_arrays.contains(resource.index0);
        u32 coordinate = 0;
        if (arrayed) {
            const u32 slice = m_builder.emit_composite_extract(
                m_type_uint, address_u4, {2});
            coordinate = m_builder.emit_composite_construct(
                m_type_uvec3, {x, y, slice});
        } else {
            coordinate = m_builder.emit_composite_construct(
                m_type_uvec2, {x, y});
        }
        const u32 return_type = m_texture_return_types.at(resource.index0);
        const bool uint_result = return_type == 4;
        const bool sint_result = return_type == 3;
        const u32 image_type = ms_texture_image_type(resource.index0);
        const u32 result_type = uint_result ? m_type_uvec4
            : sint_result ? m_type_ivec4 : m_type_vec4;
        u32 const_offset = 0;
        if (sample_controls != 0) {
            const auto decode_offset = [&](u32 shift) -> i32 {
                const u32 nibble = (sample_controls >> shift) & 0xFu;
                return static_cast<i32>(nibble) -
                       ((nibble & 0x8u) != 0 ? 16 : 0);
            };
            const u32 ox = m_builder.emit_constant_int(
                m_type_int, static_cast<u32>(decode_offset(9)));
            const u32 oy = m_builder.emit_constant_int(
                m_type_int, static_cast<u32>(decode_offset(13)));
            const_offset = m_builder.emit_constant_composite(
                m_type_ivec2, {ox, oy});
        }
        const u32 image = m_builder.emit_load(image_type, texture_it->second);
        // A one-sample view read as its 2D mip: every sample index is the
        // one sample (single_sample_ms_srvs).
        u32 loaded = single_sample_ms_srv(resource.index0)
            ? m_builder.emit_image_fetch(result_type, image, coordinate,
                                         m_const_u0, const_offset)
            : m_builder.emit_image_fetch_sample(result_type, image,
                                                coordinate, sample,
                                                const_offset);
        if (uint_result || sint_result) {
            loaded = m_builder.emit_bitcast(m_type_vec4, loaded);
        }
        loaded = apply_swizzle(loaded, resource.swizzle);
        if (!store_vec4(dst, loaded)) return false;
        break;
    }

    case Sm4Opcode::LD_UAV_TYPED: {
        // Microsoft ld_uav_typed (SM5): dest[.mask],
        // srcAddress[.swizzle], srcUAV[.swizzle]. Texture2D consumes two
        // unsigned address components. An out-of-bounds typed UAV read returns
        // zero rather than exposing the backend's undefined image-read result.
        // Legal wherever a UAV may be declared -- the pixel and compute stages
        // at feature level 11_0, which the declaration pass enforces.
        if ((opcode_token & 0x00002000u) != 0u || sample_controls != 0u) {
            return fail("LD_UAV_TYPED: unsupported opcode modifier");
        }
        Sm4Operand destination, address, uav;
        if (!m_decoder.decode_operand(destination) ||
            !m_decoder.decode_operand(address) ||
            !m_decoder.decode_operand(uav)) {
            return fail("LD_UAV_TYPED: bad operands");
        }
        if (destination.modifier != 0u || address.modifier != 0u ||
            uav.type != Sm4OperandType::UnorderedAccessView ||
            uav.modifier != 0u) {
            return fail("LD_UAV_TYPED: unsupported operand shape or modifier");
        }
        const auto found = m_uav_vars.find(uav.index0);
        if (found == m_uav_vars.end()) {
            return fail("LD_UAV_TYPED: undeclared u" +
                        std::to_string(uav.index0));
        }
        // Learn (RWBuffer / RWTexture2D): a Dim=Buffer UAV is addressed by a
        // single element index, a Texture2D one by an (x, y) texel position.
        // The bounds test follows the same shape.
        const bool buffer_uav = m_buffer_uav_slots.contains(uav.index0);
        u32 address_bits = 0;
        if (!load_vec4(address, address_bits)) return false;
        if (m_uav_1d.contains(uav.index0)) {
            address_bits = widen_1d_address(
                address_bits, m_array_uav_slots.contains(uav.index0), false);
        }
        const u32 address_uvec4 =
            m_builder.emit_bitcast(m_type_uvec4, address_bits);
        // A Texture2DArray takes (x, y, slice) exactly as a Texture3D takes
        // (x, y, z): three unsigned components either way. Only the image type
        // differs (arrayed Dim2D vs Dim3D), which typed_uav_types resolves.
        const bool volume_uav = m_volume_uav_slots.contains(uav.index0);
        const bool three_component_uav =
            volume_uav || m_array_uav_slots.contains(uav.index0);
        const u32 view_coordinate =
            buffer_uav
                ? m_builder.emit_composite_extract(m_type_uint, address_uvec4,
                                                   {0u})
            : three_component_uav
                ? m_builder.emit_vector_shuffle(m_type_uvec3, address_uvec4,
                                                address_uvec4, {0u, 1u, 2u})
                : m_builder.emit_vector_shuffle(m_type_uvec2, address_uvec4,
                                                address_uvec4, {0u, 1u});
        // A W-slice window of a volume (D3D11_TEX3D_UAV) counts z from its
        // first slice and ends at its size.
        u32 window_outside = 0;
        const u32 coordinate = volume_uav
            ? volume_uav_window(uav.index0, view_coordinate, window_outside)
            : view_coordinate;
        u32 image_type = 0;
        u32 texel_type = 0;
        typed_uav_types(uav.index0, buffer_uav, image_type, texel_type);
        const u32 image = m_builder.emit_load(image_type, found->second);
        u32 in_range = 0;
        // A buffer view's element bias (typed_buffer_bias): its element i is
        // texel i + bias of the bound texture, which is bias + NumElements
        // wide. The bounds test is on i, so no index wraps into the elements
        // in front of the view.
        u32 texel_coordinate = coordinate;
        u32 entry = 0;
        if (buffer_uav) {
            entry = typed_buffer_entry(true, uav.index0);
            const u32 bias = typed_buffer_bias(entry);
            const u32 length = typed_buffer_count(image, bias);
            in_range = m_builder.emit_integer_compare(
                SpvOp::OpULessThan, m_type_bool, coordinate, length);
            if (bias != 0) {
                texel_coordinate =
                    m_builder.emit_iadd(m_type_uint, coordinate, bias);
            }
        } else {
            // OpImageQuerySize on an ARRAYED image returns the layer count
            // as a third component, so the query and the compare have to be
            // three-wide for Texture2DArray as well as Texture3D -- asking a
            // uvec2 of an arrayed image is invalid SPIR-V, and the slice index
            // would go unbounded.
            const u32 dimensions = m_builder.emit_image_query_size(
                three_component_uav ? m_type_uvec3 : m_type_uvec2, image);
            const u32 in_range_components = m_builder.emit_integer_compare(
                SpvOp::OpULessThan,
                three_component_uav ? m_type_bvec3 : m_type_bvec2,
                coordinate, dimensions);
            in_range = m_builder.emit_all(m_type_bool, in_range_components);
        }
        if (window_outside != 0) {
            in_range = m_builder.emit_logical(
                SpvOp::OpLogicalAnd, m_type_bool, in_range,
                m_builder.emit_logical_not(m_type_bool, window_outside));
        }
        const u32 read_block = m_builder.allocate_id();
        const u32 zero_block = m_builder.allocate_id();
        const u32 merge_block = m_builder.allocate_id();
        m_builder.emit_selection_merge(merge_block);
        m_builder.emit_branch_conditional(in_range, read_block, zero_block);
        m_builder.emit_label_id(read_block);
        u32 loaded =
            m_builder.emit_image_read(texel_type, image, texel_coordinate);
        if (texel_type != m_type_vec4) {
            loaded = m_builder.emit_bitcast(m_type_vec4, loaded);
        }
        m_builder.emit_branch(merge_block);
        m_builder.emit_label_id(zero_block);
        // Learn (ld_uav_typed): out of bounds "is the same as the ld
        // instruction" -- for a typed buffer view, 0 and the format's default
        // alpha (typed_buffer_out_of_view); nothing is read.
        u32 zero = make_vec4_imm(0.0f, 0.0f, 0.0f, 0.0f);
        if (entry != 0) {
            const auto kind = m_uav_return_types.find(uav.index0);
            zero = typed_buffer_out_of_view(
                texel_type,
                kind == m_uav_return_types.end() ? 5u : kind->second, entry);
            if (texel_type != m_type_vec4) {
                zero = m_builder.emit_bitcast(m_type_vec4, zero);
            }
        }
        m_builder.emit_branch(merge_block);
        m_builder.emit_label_id(merge_block);
        u32 result = m_builder.emit_phi(
            m_type_vec4,
            {{loaded, read_block}, {zero, zero_block}});
        result = apply_swizzle(result, uav.swizzle);
        if (!store_vec4(destination, result)) return false;
        break;
    }

    case Sm4Opcode::STORE_UAV_TYPED: {
        // Microsoft store_uav_typed (SM5): dstUAV.xyzw,
        // dstAddress[.swizzle], src0[.swizzle]. Texture2D consumes the first
        // two unsigned address components and always writes all four lanes.
        // Legal wherever a UAV may be declared (the declaration pass refuses
        // the stages feature level 11_0 gives none).
        if ((opcode_token & 0x00002000u) != 0u || sample_controls != 0u) {
            return fail("STORE_UAV_TYPED: unsupported opcode modifier");
        }
        Sm4Operand uav, address, source;
        if (!m_decoder.decode_operand(uav) ||
            !m_decoder.decode_operand(address) ||
            !m_decoder.decode_operand(source)) {
            return fail("STORE_UAV_TYPED: bad operands");
        }
        if (uav.type != Sm4OperandType::UnorderedAccessView ||
            uav.modifier != 0u || uav.write_mask != 0xFu ||
            address.modifier != 0u || source.modifier != 0u) {
            return fail("STORE_UAV_TYPED: unsupported operand shape or modifier");
        }
        const auto found = m_uav_vars.find(uav.index0);
        if (found == m_uav_vars.end()) {
            return fail("STORE_UAV_TYPED: undeclared u" +
                        std::to_string(uav.index0));
        }
        u32 address_bits = 0;
        u32 texel = 0;
        if (!load_vec4(address, address_bits) ||
            !load_vec4(source, texel)) {
            return false;
        }
        if (m_uav_1d.contains(uav.index0)) {
            address_bits = widen_1d_address(
                address_bits, m_array_uav_slots.contains(uav.index0), false);
        }
        const bool buffer_uav = m_buffer_uav_slots.contains(uav.index0);
        const u32 address_uvec4 =
            m_builder.emit_bitcast(m_type_uvec4, address_bits);
        // A Texture2DArray takes (x, y, slice) exactly as a Texture3D takes
        // (x, y, z): three unsigned components either way. Only the image type
        // differs (arrayed Dim2D vs Dim3D), which typed_uav_types resolves.
        const bool volume_uav = m_volume_uav_slots.contains(uav.index0);
        const bool three_component_uav =
            volume_uav || m_array_uav_slots.contains(uav.index0);
        const u32 view_coordinate =
            buffer_uav
                ? m_builder.emit_composite_extract(m_type_uint, address_uvec4,
                                                   {0u})
            : three_component_uav
                ? m_builder.emit_vector_shuffle(m_type_uvec3, address_uvec4,
                                                address_uvec4, {0u, 1u, 2u})
                : m_builder.emit_vector_shuffle(m_type_uvec2, address_uvec4,
                                                address_uvec4, {0u, 1u});
        u32 window_outside = 0;
        const u32 coordinate = volume_uav
            ? volume_uav_window(uav.index0, view_coordinate, window_outside)
            : view_coordinate;
        u32 image_type = 0;
        u32 texel_type = 0;
        typed_uav_types(uav.index0, buffer_uav, image_type, texel_type);
        const u32 image = m_builder.emit_load(image_type, found->second);
        if (texel_type != m_type_vec4) {
            texel = m_builder.emit_bitcast(texel_type, texel);
        }
        // D3D's contract makes an out-of-bounds UAV store a no-op. Guard the
        // SPIR-V write explicitly instead of inheriting backend OOB behavior.
        // A buffer view's element i is texel i + its element bias
        // (typed_buffer_bias), tested on i as ld_uav_typed does.
        u32 in_range = 0;
        u32 texel_coordinate = coordinate;
        if (buffer_uav) {
            const u32 bias =
                typed_buffer_bias(typed_buffer_entry(true, uav.index0));
            const u32 length = typed_buffer_count(image, bias);
            in_range = m_builder.emit_integer_compare(
                SpvOp::OpULessThan, m_type_bool, coordinate, length);
            if (bias != 0) {
                texel_coordinate =
                    m_builder.emit_iadd(m_type_uint, coordinate, bias);
            }
        } else {
            // OpImageQuerySize on an ARRAYED image returns the layer count
            // as a third component, so the query and the compare have to be
            // three-wide for Texture2DArray as well as Texture3D -- asking a
            // uvec2 of an arrayed image is invalid SPIR-V, and the slice index
            // would go unbounded.
            const u32 dimensions = m_builder.emit_image_query_size(
                three_component_uav ? m_type_uvec3 : m_type_uvec2, image);
            const u32 in_range_components = m_builder.emit_integer_compare(
                SpvOp::OpULessThan,
                three_component_uav ? m_type_bvec3 : m_type_bvec2,
                coordinate, dimensions);
            in_range = m_builder.emit_all(m_type_bool, in_range_components);
        }
        if (window_outside != 0) {
            in_range = m_builder.emit_logical(
                SpvOp::OpLogicalAnd, m_type_bool, in_range,
                m_builder.emit_logical_not(m_type_bool, window_outside));
        }
        const u32 write_block = m_builder.allocate_id();
        const u32 merge_block = m_builder.allocate_id();
        m_builder.emit_selection_merge(merge_block);
        m_builder.emit_branch_conditional(in_range, write_block, merge_block);
        m_builder.emit_label_id(write_block);
        m_builder.emit_image_write(image, texel_coordinate, texel);
        m_builder.emit_branch(merge_block);
        m_builder.emit_label_id(merge_block);
        break;
    }

    default:
        return fail("Unsupported opcode: " +
                    std::to_string(static_cast<u32>(opcode)));
    }

    return true;
}

} // namespace vitreo::graphics::dxbc_spirv_detail
