// SPDX-License-Identifier: LGPL-2.1-only

/// @file dxbc_to_spirv_op_query.cpp
/// @brief Resource query and pull-model evaluation opcodes.

#include "graphics/shaders/dxbc_to_spirv_internal.hpp"

namespace vitreo::graphics::dxbc_spirv_detail {

bool DxbcTranslator::translate_resource_query_operation(u32 opcode_token,
                                                        Sm4Opcode opcode,
                                                        u32 sample_controls) {
    switch (opcode) {
    case Sm4Opcode::RESINFO: {
        // D3D11.3 22.4.14 resinfo[_uint|_rcpFloat] dest, srcMipLevel.select,
        // srcResource[.swizzle]: [width, height, depth or array size,
        // total mip count] of the VIEW at t# or u#, for the selected mip.
        // OpcodeToken0 bits 11-12: 0 float, 1 rcpFloat, 2 uint (fxc marks
        // the _uint form 0x1000). A Texture1D reports yz = 0, a 1D array its
        // size in y; a cube's z is 0 and a cube array's the cube count (the
        // spec leaves it undefined; WARP returns the count). A multisampled
        // resource has one mip and a UAV "always 1". A mip past the view
        // leaves xyz 0 -- the unbound result -- and w the mip count
        // (restriction 4). rcpFloat returns 1/width, 1/height, 1/depth
        // (INF for a 0), never for array sizes or the zero-filled lanes.
        const u32 return_mode = (opcode_token >> 11) & 0x3u;
        if (return_mode == 3u) {
            return fail("RESINFO: reserved return type 3");
        }
        if ((opcode_token & 0x00002000u) != 0u || sample_controls != 0u) {
            return fail("RESINFO: unsupported opcode modifier");
        }
        Sm4Operand dst, mip_operand, resource;
        if (!m_decoder.decode_operand(dst) ||
            !m_decoder.decode_operand(mip_operand) ||
            !m_decoder.decode_operand(resource)) {
            return fail("RESINFO: bad operands");
        }
        if (mip_operand.modifier != 0u || resource.modifier != 0u) {
            return fail("RESINFO: source modifiers are not supported");
        }
        const bool uav = resource.type == Sm4OperandType::UnorderedAccessView;
        if (!uav && resource.type != Sm4OperandType::Resource) {
            return fail("RESINFO: expected a t# or u# operand");
        }
        u32 mip4 = 0;
        if (!load_vec4(mip_operand, mip4)) return false;
        const u32 mip = m_builder.emit_composite_extract(
            m_type_uint, m_builder.emit_bitcast(m_type_uvec4, mip4), {0});

        bool one_d = false, arrayed = false, volume = false, cube = false;
        bool single_mip = false;
        u32 image = 0;
        if (uav) {
            const auto variable = m_uav_vars.find(resource.index0);
            if (variable == m_uav_vars.end() ||
                m_buffer_uav_slots.contains(resource.index0)) {
                return fail("RESINFO: u# is not a typed texture UAV");
            }
            u32 image_type = 0, vector_type = 0;
            typed_uav_types(resource.index0, false, image_type, vector_type);
            image = m_builder.emit_load(image_type, variable->second);
            one_d = m_uav_1d.contains(resource.index0);
            arrayed = m_array_uav_slots.contains(resource.index0);
            volume = m_volume_uav_slots.contains(resource.index0);
            single_mip = true;
        } else {
            const auto texture_it = m_texture_vars.find(resource.index0);
            if (texture_it == m_texture_vars.end()) {
                return fail("RESINFO: undeclared texture slot");
            }
            const SpvDim dimension = m_texture_dimensions.at(resource.index0);
            arrayed = m_texture_arrays.contains(resource.index0);
            one_d = m_texture_1d.contains(resource.index0);
            volume = dimension == SpvDim::Dim3D;
            cube = dimension == SpvDim::DimCube;
            // A one-sample view behind a multisample declaration is a 2D
            // image of one mip: the ordinary query reports it.
            single_mip = m_texture_multisampled.contains(resource.index0) &&
                         !single_sample_ms_srv(resource.index0);
            const u32 return_type = m_texture_return_types.at(resource.index0);
            const bool uint_texture = return_type == 4u;
            const bool sint_texture = return_type == 3u;
            if (cube && (uint_texture || sint_texture)) {
                return fail("RESINFO: typed integer TextureCube is not "
                            "supported");
            }
            const u32 image_type = single_mip
                ? (uint_texture ? (arrayed ? m_type_ms_uimage_2d_array
                                           : m_type_ms_uimage_2d)
                   : sint_texture ? (arrayed ? m_type_ms_iimage_2d_array
                                             : m_type_ms_iimage_2d)
                   : (arrayed ? m_type_ms_image_2d_array
                              : m_type_ms_image_2d))
                : uint_texture
                ? (arrayed ? m_type_uimage_2d_array
                           : volume ? m_type_uimage_3d : m_type_uimage_2d)
                : sint_texture
                ? (arrayed ? m_type_iimage_2d_array
                           : volume ? m_type_iimage_3d : m_type_iimage_2d)
                : arrayed && cube ? m_type_image_cube_array
                : arrayed ? m_type_image_2d_array
                : cube ? m_type_image_cube
                : volume ? m_type_image_3d : m_type_image_2d;
            image = m_builder.emit_load(image_type, texture_it->second);
        }
        m_builder.emit_capability(SpvCapability::ImageQuery);

        const u32 levels = single_mip
            ? m_builder.emit_constant_int(m_type_int, 1u)
            : m_builder.emit_image_query_levels(m_type_int, image);
        const u32 in_range = m_builder.emit_integer_compare(
            SpvOp::OpULessThan, m_type_bool, mip,
            m_builder.emit_bitcast(m_type_uint, levels));
        const bool three = volume || arrayed;
        const u32 size_type = three ? m_type_ivec3 : m_type_ivec2;
        u32 size = 0;
        if (single_mip) {
            // Multisampled and storage images have no level of detail.
            size = m_builder.emit_image_query_size(size_type, image);
        } else {
            // Querying a level the view does not have is undefined; query
            // mip 0 there and discard the result below.
            const u32 safe_mip =
                m_builder.emit_select(m_type_uint, in_range, mip, m_const_u0);
            size = m_builder.emit_image_query_size(size_type, image, safe_mip);
        }
        const u32 zero = m_builder.emit_constant_int(m_type_int, 0u);
        const u32 width = m_builder.emit_composite_extract(m_type_int, size, {0});
        const u32 height =
            m_builder.emit_composite_extract(m_type_int, size, {1});
        const u32 third = three
            ? m_builder.emit_composite_extract(m_type_int, size, {2}) : zero;
        // Lane roles: which are sizes (reciprocal under rcpFloat), which an
        // array size, which zero-filled.
        std::array<u32, 3> lanes{width, height, third};
        std::array<bool, 3> reciprocal{true, true, volume};
        if (one_d) {
            lanes = {width, arrayed ? third : zero, zero};
            reciprocal = {true, false, false};
        } else if (cube && !arrayed) {
            lanes[2] = zero;
        }
        for (u32 lane = 0; lane < 3u; ++lane) {
            lanes[lane] = m_builder.emit_select(m_type_int, in_range,
                                                lanes[lane], zero);
        }
        u32 result = 0;
        if (return_mode == 2u) {
            result = m_builder.emit_bitcast(
                m_type_vec4,
                m_builder.emit_composite_construct(
                    m_type_ivec4, {lanes[0], lanes[1], lanes[2], levels}));
        } else {
            std::array<u32, 4> floats{};
            for (u32 lane = 0; lane < 3u; ++lane) {
                floats[lane] =
                    m_builder.emit_convert_s_to_f(m_type_float, lanes[lane]);
                const bool zero_filled =
                    (one_d && lane > 0u && !(lane == 1u && arrayed)) ||
                    (cube && !arrayed && lane == 2u) ||
                    (!one_d && !three && lane == 2u);
                if (return_mode == 1u && reciprocal[lane] && !zero_filled) {
                    floats[lane] = m_builder.emit_fdiv(
                        m_type_float,
                        m_builder.emit_constant_float(m_type_float, 1.0f),
                        floats[lane]);
                }
            }
            floats[3] = m_builder.emit_convert_s_to_f(m_type_float, levels);
            result = m_builder.emit_composite_construct(
                m_type_vec4, {floats[0], floats[1], floats[2], floats[3]});
        }
        result = apply_swizzle(result, resource.swizzle);
        if (!store_vec4(dst, result)) return false;
        break;
    }

    case Sm4Opcode::BUFINFO: {
        // D3D11.3 22.4.1 bufinfo dest, srcResource: every written component
        // receives the element count of the buffer VIEW at t#/u# -- typed
        // elements for a typed buffer, bytes for a raw one, structures for
        // a structured one (Windows [cs_bufinfo]).
        Sm4Operand dst, resource;
        if (!m_decoder.decode_operand(dst) ||
            !m_decoder.decode_operand(resource)) {
            return fail("BUFINFO: bad operands");
        }
        const bool uav = resource.type == Sm4OperandType::UnorderedAccessView;
        if (!uav && resource.type != Sm4OperandType::Resource) {
            return fail("BUFINFO: expected a t# or u# operand");
        }
        const u32 slot = resource.index0;
        u32 count = 0;
        const bool typed = uav ? m_buffer_uav_slots.contains(slot)
                               : m_typed_buffer_srv_vars.contains(slot);
        if (typed) {
            u32 image_type = 0, variable = 0;
            if (uav) {
                u32 vector_type = 0;
                typed_uav_types(slot, true, image_type, vector_type);
                variable = m_uav_vars.at(slot);
            } else {
                const u32 return_type = m_typed_buffer_srv_return_types[slot];
                image_type = return_type == 4u ? m_type_buffer_image_uint
                           : return_type == 3u ? m_type_buffer_image_int
                                               : m_type_buffer_image_float;
                variable = m_typed_buffer_srv_vars.at(slot);
            }
            // The bound texture's width less the view's element bias
            // (typed_buffer_bias): NumElements.
            count = typed_buffer_count(
                m_builder.emit_load(image_type, variable),
                typed_buffer_bias(typed_buffer_entry(uav, slot)));
            if (!uav && rgb32_buffer_srv(slot)) {
                // Three R32 texels per element (rgb32_buffer_srvs).
                count = m_builder.emit_udiv(
                    m_type_uint, count,
                    m_builder.emit_constant_int(m_type_uint, 3u));
            }
        } else {
            const bool raw = uav ? m_raw_uav_vars.contains(slot)
                                 : m_raw_srv_vars.contains(slot);
            const auto& strides = uav ? m_structured_uav_strides
                                      : m_structured_srv_strides;
            const auto stride = strides.find(slot);
            if (!raw && stride == strides.end()) {
                return fail("BUFINFO: t#/u# is not a buffer view");
            }
            if (!guards_buffer(resource.type) ||
                slot >= (uav ? kGuardSizeUavSlots : kGuardSizeSrvSlots)) {
                return fail("BUFINFO: the view size is not available to this "
                            "stage");
            }
            // The bounds guard's per-slot view size, in 32-bit words.
            const u32 words = m_builder.emit_load(
                m_type_uint,
                m_builder.emit_access_chain(
                    m_type_ptr_push_uint, push_block_var(),
                    {m_builder.emit_constant_int(m_type_uint, 2u),
                     m_builder.emit_constant_int(
                         m_type_uint,
                         (uav ? kGuardSizeSrvSlots : 0u) + slot)}));
            const u32 bytes = m_builder.emit_imul(
                m_type_uint, words,
                m_builder.emit_constant_int(m_type_uint, 4u));
            count = raw ? bytes
                        : m_builder.emit_udiv(
                              m_type_uint, bytes,
                              m_builder.emit_constant_int(
                                  m_type_uint, std::max(1u, stride->second)));
        }
        const u32 result = m_builder.emit_bitcast(
            m_type_vec4,
            m_builder.emit_composite_construct(m_type_uvec4,
                                               {count, count, count, count}));
        if (!store_vec4(dst, result)) return false;
        break;
    }

    case Sm4Opcode::SAMPLEINFO:
    case Sm4Opcode::SAMPLEPOS: {
        // D3D11.3 22.4.21 sampleinfo[_uint] dest, srcResource: (SampleCount,
        // 0, 0, 0), float unless _uint (OpcodeToken0 bit 11). 22.4.22
        // samplepos dest, srcResource, sampleIndex: the sample's position
        // relative to the pixel centre, (x, y, 0, 0), zero for an index out
        // of range. srcResource is a multisampled t# or `rasterizer` (the
        // render targets' count, from the push block). Positions are the
        // standard patterns, which are also Metal's defaults for the counts
        // this GPU offers (1, 2, 4: MTLDevice getDefaultSamplePositions, in
        // 1/16 pixel: 2x (4,4)(-4,-4); 4x (-2,-6)(6,-2)(-6,2)(2,6), the
        // latter measured on WARP too, [cs_samplepos]).
        const bool info = opcode == Sm4Opcode::SAMPLEINFO;
        Sm4Operand dst, resource, index_operand;
        if (!m_decoder.decode_operand(dst) ||
            !m_decoder.decode_operand(resource) ||
            (!info && !m_decoder.decode_operand(index_operand))) {
            return fail("SAMPLEINFO/SAMPLEPOS: bad operands");
        }
        u32 count = 0;
        if (resource.type == Sm4OperandType::Rasterizer) {
            if (m_result.stage != ShaderStage::Fragment) {
                return fail("SAMPLEINFO/SAMPLEPOS: `rasterizer` outside a "
                            "pixel shader is not implemented");
            }
            count = m_builder.emit_load(
                m_type_uint,
                m_builder.emit_access_chain(
                    m_type_ptr_push_uint, push_block_var(),
                    {m_builder.emit_constant_int(m_type_uint, 5u)}));
        } else if (resource.type == Sm4OperandType::Resource &&
                   single_sample_ms_srv(resource.index0)) {
            // A one-sample view (single_sample_ms_srvs): its one sample sits
            // at the centre (measured, d3d11_view_line_probe [ms1]).
            count = m_builder.emit_constant_int(m_type_uint, 1u);
        } else if (resource.type == Sm4OperandType::Resource &&
                   m_texture_multisampled.contains(resource.index0)) {
            const u32 image_type = ms_texture_image_type(resource.index0);
            m_builder.emit_capability(SpvCapability::ImageQuery);
            count = m_builder.emit_bitcast(
                m_type_uint,
                m_builder.emit_image_query_samples(
                    m_type_int,
                    m_builder.emit_load(image_type,
                                        m_texture_vars.at(resource.index0))));
        } else {
            return fail("SAMPLEINFO/SAMPLEPOS: expected a multisampled t# or "
                        "`rasterizer`");
        }
        u32 result = 0;
        if (info) {
            const bool uint_result = (opcode_token & 0x00000800u) != 0u;
            const u32 value = uint_result
                ? m_builder.emit_bitcast(m_type_float, count)
                : m_builder.emit_convert_u_to_f(m_type_float, count);
            const u32 zero = m_builder.emit_constant_float(m_type_float, 0.0f);
            result = m_builder.emit_composite_construct(
                m_type_vec4, {value, zero, zero, zero});
        } else {
            if (index_operand.modifier != 0u) {
                return fail("SAMPLEPOS: source modifiers are not supported");
            }
            u32 index4 = 0;
            if (!load_vec4(index_operand, index4)) return false;
            const u32 index = m_builder.emit_composite_extract(
                m_type_uint, m_builder.emit_bitcast(m_type_uvec4, index4),
                {0});
            const auto is = [&](u32 value, u32 constant) {
                return m_builder.emit_integer_compare(
                    SpvOp::OpIEqual, m_type_bool, value,
                    m_builder.emit_constant_int(m_type_uint, constant));
            };
            const auto position = [&](i32 x, i32 y) {
                return make_vec4_imm(static_cast<float>(x) / 16.0f,
                                     static_cast<float>(y) / 16.0f, 0.0f,
                                     0.0f);
            };
            const u32 zero4 = make_vec4_imm(0.0f, 0.0f, 0.0f, 0.0f);
            const u32 four = m_builder.emit_select(
                m_type_vec4, is(index, 0u), position(-2, -6),
                m_builder.emit_select(
                    m_type_vec4, is(index, 1u), position(6, -2),
                    m_builder.emit_select(
                        m_type_vec4, is(index, 2u), position(-6, 2),
                        m_builder.emit_select(m_type_vec4, is(index, 3u),
                                              position(2, 6), zero4))));
            const u32 two = m_builder.emit_select(
                m_type_vec4, is(index, 0u), position(4, 4),
                m_builder.emit_select(m_type_vec4, is(index, 1u),
                                      position(-4, -4), zero4));
            // One sample sits at the centre; any other count has no
            // pattern here, which no resource on this device can have.
            result = m_builder.emit_select(
                m_type_vec4, is(count, 4u), four,
                m_builder.emit_select(
                    m_type_vec4, is(count, 2u), two, zero4));
        }
        result = apply_swizzle(result, resource.swizzle);
        if (!store_vec4(dst, result)) return false;
        break;
    }

    case Sm4Opcode::LOD: {
        // D3D11.3 22.4.26 (lod) dest, srcAddress, srcResource[.swizzle],
        // srcSampler: (ClampedLOD, NonClampedLOD, 0, 0) that sample would
        // use; pixel shader only. OpImageQueryLod returns exactly that pair
        // (MSL calculate_clamped_lod / calculate_unclamped_lod).
        if (m_container.stage != ShaderStage::Fragment) {
            return fail("LOD: requires a pixel shader");
        }
        Sm4Operand dst, address, resource, sampler;
        if (!m_decoder.decode_operand(dst) ||
            !m_decoder.decode_operand(address) ||
            !m_decoder.decode_operand(resource) ||
            !m_decoder.decode_operand(sampler)) {
            return fail("LOD: bad operands");
        }
        if (address.modifier != 0u ||
            resource.type != Sm4OperandType::Resource ||
            sampler.type != Sm4OperandType::Sampler) {
            return fail("LOD: expected address, t# and s# operands");
        }
        const auto texture_it = m_texture_vars.find(resource.index0);
        const auto sampler_it = m_sampler_vars.find(sampler.index0);
        if (texture_it == m_texture_vars.end() ||
            sampler_it == m_sampler_vars.end() ||
            m_texture_multisampled.contains(resource.index0)) {
            return fail("LOD: undeclared or multisampled resource");
        }
        const SpvDim dimension = m_texture_dimensions.at(resource.index0);
        const bool arrayed = m_texture_arrays.contains(resource.index0);
        const bool cube = dimension == SpvDim::DimCube;
        const bool volume = dimension == SpvDim::Dim3D;
        const u32 return_type = m_texture_return_types.at(resource.index0);
        if (return_type == 3u || return_type == 4u) {
            return fail("LOD: integer textures are not filtered");
        }
        const u32 image_type = arrayed && cube ? m_type_image_cube_array
            : arrayed ? m_type_image_2d_array
            : cube ? m_type_image_cube
            : volume ? m_type_image_3d : m_type_image_2d;
        const u32 sampled_type = arrayed && cube
            ? m_type_sampled_image_cube_array
            : arrayed ? m_type_sampled_image_2d_array
            : cube ? m_type_sampled_image_cube
            : volume ? m_type_sampled_image_3d : m_type_sampled_image_2d;
        const u32 sampled_image = m_builder.emit_sampled_image(
            sampled_type,
            m_builder.emit_load(image_type, texture_it->second),
            m_builder.emit_load(m_type_sampler, sampler_it->second));
        u32 address4 = 0;
        if (!load_vec4(address, address4)) return false;
        if (m_texture_1d.contains(resource.index0)) {
            address4 = widen_1d_address(address4, arrayed, true);
        }
        // The query takes the coordinate without the array layer.
        u32 coordinate = (cube || volume)
            ? m_builder.emit_vector_shuffle(m_type_vec3, address4, address4,
                                            {0, 1, 2})
            : m_builder.emit_vector_shuffle(m_type_vec2, address4, address4,
                                            {0, 1});
        // The sampler's MipLODBias is part of the LOD sample would use
        // (7.18.11; WARP: bias 1 turns LOD 2 into 3), and Metal samplers
        // carry none. LOD = log2(|derivative| * size), so scaling the
        // coordinate by 2^bias adds the bias before Metal's own clamps.
        // A cube direction's LOD does not scale with its length, so there
        // the bias is added to the results instead (the clamped one kept
        // within the view's mips; MinLOD/MaxLOD are not re-applied).
        u32 bias = 0;
        if (sampler_bias_bound()) {
            bias = m_builder.emit_load(
                m_type_float,
                m_builder.emit_access_chain(
                    m_type_ptr_push_float, push_block_var(),
                    {m_builder.emit_constant_int(m_type_uint, 0u),
                     m_builder.emit_constant_int(m_type_uint,
                                                 sampler.index0)}));
        } else if (m_sampler_state_var != 0) {
            bias = m_builder.emit_bitcast(
                m_type_float,
                sampler_state_word(sampler, kSamplerStateBiasWord));
        }
        if (bias != 0) {
            if (!cube) {
                const u32 scale = m_builder.emit_ext_inst(
                    m_type_float, m_glsl_std450, kGlslStd450Exp2, {bias});
                coordinate = volume
                    ? m_builder.emit_fmul(
                          m_type_vec3, coordinate,
                          m_builder.emit_composite_construct(
                              m_type_vec3, {scale, scale, scale}))
                    : m_builder.emit_fmul(
                          m_type_vec2, coordinate,
                          m_builder.emit_composite_construct(
                              m_type_vec2, {scale, scale}));
            }
        }
        m_builder.emit_capability(SpvCapability::ImageQuery);
        const u32 lods =
            m_builder.emit_image_query_lod(m_type_vec2, sampled_image,
                                           coordinate);
        const u32 zero = m_builder.emit_constant_float(m_type_float, 0.0f);
        u32 clamped = m_builder.emit_composite_extract(m_type_float, lods, {0});
        u32 unclamped =
            m_builder.emit_composite_extract(m_type_float, lods, {1});
        if (bias != 0 && cube) {
            const u32 top = m_builder.emit_convert_s_to_f(
                m_type_float,
                m_builder.emit_isub(
                    m_type_int,
                    m_builder.emit_image_query_levels(
                        m_type_int,
                        m_builder.emit_load(image_type, texture_it->second)),
                    m_builder.emit_constant_int(m_type_int, 1u)));
            unclamped = m_builder.emit_fadd(m_type_float, unclamped, bias);
            clamped = m_builder.emit_ext_inst(
                m_type_float, m_glsl_std450, kGlslStd450FMin,
                {m_builder.emit_ext_inst(
                     m_type_float, m_glsl_std450, kGlslStd450FMax,
                     {m_builder.emit_fadd(m_type_float, clamped, bias), zero}),
                 top});
        }
        u32 result = m_builder.emit_composite_construct(
            m_type_vec4, {clamped, unclamped, zero, zero});
        result = apply_swizzle(result, resource.swizzle);
        if (!store_vec4(dst, result)) return false;
        break;
    }

    case Sm4Opcode::EVAL_SNAPPED:
    case Sm4Opcode::EVAL_SAMPLE_INDEX:
    case Sm4Opcode::EVAL_CENTROID: {
        // D3D11.3 22.4.23-25: pull-model evaluation of pixel-shader input
        // v# at a sample index, the centroid, or an offset from the pixel
        // centre on a 16x16 grid (the low 4 bits of pixelOffset.xy, signed,
        // in 1/16 pixel). The declared centroid/sample modifier is ignored;
        // a constant-interpolated input returns its value unchanged.
        // GLSL.std.450 InterpolateAt* -- MSL interpolant<> -- evaluate the
        // input variable itself, so it must be one whole v# register.
        if (m_result.stage != ShaderStage::Fragment) {
            return fail("EVAL: requires a pixel shader");
        }
        Sm4Operand dst, source, where;
        const bool centroid = opcode == Sm4Opcode::EVAL_CENTROID;
        if (!m_decoder.decode_operand(dst) ||
            !m_decoder.decode_operand(source) ||
            (!centroid && !m_decoder.decode_operand(where))) {
            return fail("EVAL: bad operands");
        }
        if (source.type != Sm4OperandType::Input || source.index0_relative ||
            source.modifier > 3u) {
            return fail("EVAL: expected a directly indexed v# operand");
        }
        const auto variable = m_input_vars.find(source.index0);
        if (variable == m_input_vars.end() ||
            source.index0 == m_frag_coord_register) {
            return fail("EVAL: v# is not an interpolated input");
        }
        const u32 value_type = m_input_value_types.at(source.index0);
        if (value_type != m_type_vec4) {
            return fail("EVAL: v# is not a float4 interpolated input");
        }
        const auto modes = scan_input_interpolation();
        const auto mode = modes.find(source.index0);
        u32 value = 0;
        if (mode != modes.end() && mode->second == 1u) {
            value = m_builder.emit_load(m_type_vec4, variable->second);
        } else {
            m_builder.emit_capability(SpvCapability::InterpolationFunction);
            if (centroid) {
                value = m_builder.emit_ext_inst(
                    m_type_vec4, m_glsl_std450,
                    kGlslStd450InterpolateAtCentroid, {variable->second});
            } else {
                if (where.modifier != 0u) {
                    return fail("EVAL: source modifiers are not supported");
                }
                u32 where4 = 0;
                if (!load_vec4(where, where4)) return false;
                const u32 bits = m_builder.emit_bitcast(m_type_ivec4, where4);
                if (opcode == Sm4Opcode::EVAL_SAMPLE_INDEX) {
                    value = m_builder.emit_ext_inst(
                        m_type_vec4, m_glsl_std450,
                        kGlslStd450InterpolateAtSample,
                        {variable->second,
                         m_builder.emit_composite_extract(m_type_int, bits,
                                                          {0})});
                } else {
                    // Sign-extend the low 4 bits: (v << 28) >> 28.
                    const auto nibble = [&](u32 lane) {
                        const u32 shift =
                            m_builder.emit_constant_int(m_type_uint, 28u);
                        const u32 extended =
                            m_builder.emit_shift_right_arithmetic(
                                m_type_int,
                                m_builder.emit_shift_left_logical(
                                    m_type_int,
                                    m_builder.emit_composite_extract(
                                        m_type_int, bits, {lane}),
                                    shift),
                                shift);
                        return m_builder.emit_fmul(
                            m_type_float,
                            m_builder.emit_convert_s_to_f(m_type_float,
                                                          extended),
                            m_builder.emit_constant_float(m_type_float,
                                                          1.0f / 16.0f));
                    };
                    // SPIRV-Cross turns the centre-relative SPIR-V offset
                    // into MSL's corner-relative interpolate_at_offset by
                    // adding 0.4375 (an Intel workaround, per its source);
                    // Apple GPUs take the documented corner offset, so the
                    // missing 1/16 is added back (Windows [ps_eval_snap]:
                    // (4,-8) at pixel (1,2) evaluates at (1.75, 2.0); without
                    // it the M4 Pro read (1.6875, 1.9375)).
                    const u32 sixteenth =
                        m_builder.emit_constant_float(m_type_float,
                                                      1.0f / 16.0f);
                    const u32 offset = m_builder.emit_composite_construct(
                        m_type_vec2,
                        {m_builder.emit_fadd(m_type_float, nibble(0),
                                             sixteenth),
                         m_builder.emit_fadd(m_type_float, nibble(1),
                                             sixteenth)});
                    value = m_builder.emit_ext_inst(
                        m_type_vec4, m_glsl_std450,
                        kGlslStd450InterpolateAtOffset,
                        {variable->second, offset});
                }
            }
        }
        value = apply_swizzle(value, source.swizzle);
        if (source.modifier == 2u || source.modifier == 3u) {
            value = m_builder.emit_ext_inst(m_type_vec4, m_glsl_std450,
                                            kGlslStd450FAbs, {value});
        }
        if (source.modifier == 1u || source.modifier == 3u) {
            value = m_builder.emit_fnegate(m_type_vec4, value);
        }
        if (!store_vec4(dst, value)) return false;
        break;
    }

    default:
        return fail("Unsupported opcode: " +
                    std::to_string(static_cast<u32>(opcode)));
    }

    return true;
}

} // namespace vitreo::graphics::dxbc_spirv_detail
