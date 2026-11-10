// SPDX-License-Identifier: LGPL-2.1-only

/// @file dxbc_to_spirv_op_sample.cpp
/// @brief Texture sample and gather opcodes.

#include "graphics/shaders/dxbc_to_spirv_internal.hpp"

namespace vitreo::graphics::dxbc_spirv_detail {

bool DxbcTranslator::translate_sample_operation(u32 /*opcode_token*/,
                                                Sm4Opcode opcode,
                                                u32 sample_controls) {
    switch (opcode) {
    case Sm4Opcode::SAMPLE:
    case Sm4Opcode::SAMPLE_C:
    case Sm4Opcode::SAMPLE_C_LZ:
    case Sm4Opcode::SAMPLE_L:
    case Sm4Opcode::SAMPLE_B:
    case Sm4Opcode::SAMPLE_D:
    case Sm4Opcode::GATHER4:
    case Sm4Opcode::GATHER4_C:
    case Sm4Opcode::GATHER4_PO:
    case Sm4Opcode::GATHER4_PO_C: {
        // Learn sample_c/sample_c_lz: their first four operands match sample,
        // followed by a scalar comparison reference. sample_c uses implicit
        // LOD in a pixel shader; sample_c_lz uses level zero plus the
        // sampler's MipLODBias (22.4.20).
        const bool comparison = opcode == Sm4Opcode::SAMPLE_C ||
                                opcode == Sm4Opcode::SAMPLE_C_LZ;
        // VX_TEX SampleCmpGrad (dxbc_vitreo_ext.hpp): sample_c whose LOD
        // comes from explicit gradients, decoded after the reference.
        const bool compare_gradient =
            opcode == Sm4Opcode::SAMPLE_C && m_vx_compare_gradient;
        const bool gradients = opcode == Sm4Opcode::SAMPLE_D || compare_gradient;
        // VX_TEX SampleCmpLevel (sample_c_lz + an LOD) and SampleCmpBias
        // (sample_c + a bias): the operand follows the reference.
        const bool compare_level =
            opcode == Sm4Opcode::SAMPLE_C_LZ && m_vx_compare_level;
        const bool compare_bias =
            opcode == Sm4Opcode::SAMPLE_C && m_vx_compare_bias;
        const bool explicit_lod = opcode == Sm4Opcode::SAMPLE_L ||
                                  opcode == Sm4Opcode::SAMPLE_C_LZ ||
                                  gradients;
        // Learn (gather4_c / gather4_po / gather4_po_c, sm5):
        //   gather4_c    dst, address, resource, sampler, reference
        //   gather4_po   dst, address, offset, resource, sampler
        //   gather4_po_c dst, address, offset, resource, sampler, reference
        // _c compares four texels against the reference instead of returning
        // them; _po takes its offset from a register, 6 signed bits per axis.
        const bool gather = opcode == Sm4Opcode::GATHER4 ||
                            opcode == Sm4Opcode::GATHER4_C ||
                            opcode == Sm4Opcode::GATHER4_PO ||
                            opcode == Sm4Opcode::GATHER4_PO_C;
        const bool gather_compare = opcode == Sm4Opcode::GATHER4_C ||
                                    opcode == Sm4Opcode::GATHER4_PO_C;
        const bool programmable_offset = opcode == Sm4Opcode::GATHER4_PO ||
                                         opcode == Sm4Opcode::GATHER4_PO_C;
        if (!explicit_lod && !gather &&
            m_result.stage != ShaderStage::Fragment) {
            return fail("implicit-LOD sampling requires a pixel shader");
        }
        Sm4Operand dst, address, offset_operand, resource, sampler;
        if (!m_decoder.decode_operand(dst) || !decode_tiled_status() ||
            !m_decoder.decode_operand(address) ||
            (programmable_offset &&
             !m_decoder.decode_operand(offset_operand)) ||
            !m_decoder.decode_operand(resource) ||
            !m_decoder.decode_operand(sampler)) {
            return fail("SAMPLE: bad operands");
        }
        // D3D11.3 22.4: the sample/gather address and offset operands take
        // no source modifiers; load_vec4 would silently drop one.
        if (address.modifier != 0u ||
            (programmable_offset && offset_operand.modifier != 0u)) {
            return fail("SAMPLE: source modifier on an operand that takes none");
        }
        if (resource.type != Sm4OperandType::Resource ||
            sampler.type != Sm4OperandType::Sampler) {
            return fail("SAMPLE: expected texture and sampler operands");
        }
        if (comparison && resource.swizzle != 0u) {
            return fail("SAMPLE_C: texture operand must select .r");
        }
        // Learn (sample, aoffimmi): the sample-controls extended token carries
        // three 4-bit *signed* texel offsets, applied to the coordinate before
        // the fetch. SPIR-V expresses that as the ConstOffset image operand,
        // whose component count is the dimensionality *without* the array
        // layer. A D3D11 renderer's filter kernels use these constantly.
        u32 immediate_offset = 0;
        std::array<i32, 3> offset_texels{};
        if (sample_controls != 0) {
            const auto signed_nibble = [](u32 token, u32 shift) {
                const auto raw = static_cast<i32>((token >> shift) & 0xFu);
                return raw >= 8 ? raw - 16 : raw;
            };
            const SpvDim offset_dim =
                m_texture_dimensions.at(resource.index0);
            const i32 u = signed_nibble(sample_controls, 9);
            // A Texture1D has no v axis to offset along.
            const i32 v = m_texture_1d.contains(resource.index0)
                              ? 0
                              : signed_nibble(sample_controls, 13);
            const i32 w = signed_nibble(sample_controls, 17);
            offset_texels = {u, v, w};
            if (offset_dim == SpvDim::Dim2D) {
                immediate_offset = m_builder.emit_constant_composite(
                    m_type_ivec2,
                    {m_builder.emit_constant_int(m_type_int,
                                                 static_cast<u32>(u)),
                     m_builder.emit_constant_int(m_type_int,
                                                 static_cast<u32>(v))});
            } else if (offset_dim == SpvDim::Dim3D) {
                immediate_offset = m_builder.emit_constant_composite(
                    m_type_ivec3,
                    {m_builder.emit_constant_int(m_type_int,
                                                 static_cast<u32>(u)),
                     m_builder.emit_constant_int(m_type_int,
                                                 static_cast<u32>(v)),
                     m_builder.emit_constant_int(m_type_int,
                                                 static_cast<u32>(w))});
            } else {
                // A cube map has no ConstOffset in either API.
                return fail("sample offset on a cube map is not permitted");
            }
        }
        // VX_TEX_OFFSET: this instruction's offsets are values (SM 6.7),
        // the image operand Offset in place of ConstOffset.
        const u32 dynamic_offset = m_vx_dynamic_offset;
        m_vx_dynamic_offset = 0;
        struct DynamicOffsetScope {
            SpirvBuilder& builder;
            ~DynamicOffsetScope() { builder.set_dynamic_offsets(false); }
        } dynamic_scope{m_builder};
        if (dynamic_offset != 0) {
            if (sample_controls != 0 || gather) {
                return fail("VX_TEX_OFFSET before an instruction with offsets of its own");
            }
            const SpvDim offset_dim = m_texture_dimensions.at(resource.index0);
            if (offset_dim == SpvDim::Dim2D) {
                immediate_offset = m_builder.emit_vector_shuffle(
                    m_type_ivec2, dynamic_offset, dynamic_offset,
                    {0u, m_texture_1d.contains(resource.index0) ? 4u : 1u});
                if (m_texture_1d.contains(resource.index0)) {
                    // A Texture1D has no v axis: (u, 0).
                    immediate_offset = m_builder.emit_composite_insert(
                        m_type_ivec2, m_builder.emit_constant_int(m_type_int, 0u),
                        immediate_offset, {1});
                }
            } else if (offset_dim == SpvDim::Dim3D) {
                immediate_offset = m_builder.emit_vector_shuffle(
                    m_type_ivec3, dynamic_offset, dynamic_offset, {0u, 1u, 2u});
            } else {
                return fail("sample offset on a cube map is not permitted");
            }
            m_builder.set_dynamic_offsets(true);
        }

        auto texture_it = m_texture_vars.find(resource.index0);
        auto sampler_it = m_sampler_vars.find(sampler.index0);
        if (texture_it == m_texture_vars.end() ||
            sampler_it == m_sampler_vars.end()) {
            return fail("SAMPLE: undeclared texture or sampler slot");
        }

        u32 address4 = 0;
        if (!load_vec4(address, address4)) return false;
        const bool texture_1d = m_texture_1d.contains(resource.index0);
        if (texture_1d && gather) {
            return fail("GATHER4 does not support Texture1D");
        }
        if (texture_1d) {
            address4 = widen_1d_address(
                address4, m_texture_arrays.contains(resource.index0),
                /*normalized=*/true);
        }
        const u32 x = m_builder.emit_composite_extract(
            m_type_float, address4, {0});
        const u32 y = m_builder.emit_composite_extract(
            m_type_float, address4, {1});
        const SpvDim dimension = m_texture_dimensions.at(resource.index0);
        const u32 resource_return_type =
            m_texture_return_types.at(resource.index0);
        if (!gather &&
            (resource_return_type == 3 || resource_return_type == 4)) {
            return fail("SAMPLE: integer textures require an integer load");
        }
        const bool cube = dimension == SpvDim::DimCube;
        const bool volume = dimension == SpvDim::Dim3D;
        const bool arrayed = m_texture_arrays.contains(resource.index0);
        if (comparison && volume) {
            return fail("comparison sampling Texture3D is not supported");
        }
        if (gather && volume) {
            return fail("GATHER4 does not support Texture3D");
        }
        u32 coordinate = 0;
        if (cube && arrayed) {
            coordinate = address4;
        } else if (cube || volume || arrayed) {
            const u32 z = m_builder.emit_composite_extract(
                m_type_float, address4, {2});
            coordinate = m_builder.emit_composite_construct(
                m_type_vec3, {x, y, z});
        } else {
            coordinate = m_builder.emit_composite_construct(
                m_type_vec2, {x, y});
        }

        const bool uint_gather = gather && resource_return_type == 4u;
        const bool sint_gather = gather && resource_return_type == 3u;
        // Learn (gather4, sm5) 22.4.2: Texture2D and TextureCube, including
        // arrays. Texture1D and Texture3D already failed above. A cube
        // gather is OpImageGather of the cube image: the direction selects
        // the face and a footprint that crosses an edge reads the adjacent
        // face (D3D11.3 7.18.12). A 2D gather is not that operation.
        // 7.18.9.1's border is the same lo + (hi - lo) * border blend the
        // integer path applies below, on those cube taps. gather4_c has no
        // comparison on an integer texture (22.4.3). gather4_po is 2D-only
        // (22.4.4). The gather names the cube image; it is not a 2D gather.
        if ((uint_gather || sint_gather) && cube && gather_compare) {
            return fail("GATHER4_C: integer textures have no comparison");
        }
        if ((uint_gather || sint_gather) && cube && programmable_offset) {
            return fail("GATHER4_PO: only 2D textures take an offset");
        }
        const u32 image_type = uint_gather
            ? (arrayed && cube ? m_type_uimage_cube_array
               : arrayed ? m_type_uimage_2d_array
               : cube ? m_type_uimage_cube
                      : m_type_uimage_2d)
            : sint_gather
                ? (arrayed && cube ? m_type_iimage_cube_array
                   : arrayed ? m_type_iimage_2d_array
                   : cube ? m_type_iimage_cube
                          : m_type_iimage_2d)
                : arrayed && cube ? m_type_image_cube_array
                : arrayed ? m_type_image_2d_array
                : cube ? m_type_image_cube
                       : volume ? m_type_image_3d : m_type_image_2d;
        const u32 image =
            m_builder.emit_load(image_type, texture_it->second);
        const u32 sampler_value = m_builder.emit_load(
            m_type_sampler, sampler_it->second);
        const u32 sampled_image_type = uint_gather
            ? (arrayed && cube ? m_type_sampled_uimage_cube_array
               : arrayed ? m_type_sampled_uimage_2d_array
               : cube ? m_type_sampled_uimage_cube
                      : m_type_sampled_uimage_2d)
            : sint_gather
                ? (arrayed && cube ? m_type_sampled_iimage_cube_array
                   : arrayed ? m_type_sampled_iimage_2d_array
                   : cube ? m_type_sampled_iimage_cube
                          : m_type_sampled_iimage_2d)
                : arrayed && cube ? m_type_sampled_image_cube_array
                : arrayed ? m_type_sampled_image_2d_array
                : cube ? m_type_sampled_image_cube
                       : volume ? m_type_sampled_image_3d
                                : m_type_sampled_image_2d;
        // Built here only for the straight-line D3D11 path. A descriptor-table
        // sample branches, and OpSampledImage cannot be consumed in another
        // block, so those paths build it beside the sample.
        u32 sampled_image = 0;
        if (m_sampler_state_var == 0) {
            sampled_image = m_builder.emit_sampled_image(
                sampled_image_type, image, sampler_value);
        }

        // gather4_po: the low 6 bits of offset.xy, signed ([-32, 31]). Only
        // a 2D texture takes one (Learn).
        u32 programmable = 0;
        if (programmable_offset) {
            if (cube || volume) {
                return fail("GATHER4_PO: only 2D textures take an offset");
            }
            u32 offset4 = 0;
            if (!load_vec4(offset_operand, offset4)) return false;
            const u32 bits = m_builder.emit_bitcast(m_type_ivec4, offset4);
            const u32 xy = m_builder.emit_vector_shuffle(m_type_ivec2, bits,
                                                         bits, {0u, 1u});
            const u32 twenty_six = m_builder.emit_constant_composite(
                m_type_ivec2, {m_builder.emit_constant_int(m_type_int, 26u),
                               m_builder.emit_constant_int(m_type_int, 26u)});
            const u32 offset = m_builder.emit_shift_right_arithmetic(
                m_type_ivec2,
                m_builder.emit_shift_left_logical(m_type_ivec2, xy, twenty_six),
                twenty_six);
            // Metal's gather offset spans [-8, 7] and the hardware keeps its
            // low 4 bits, so a D3D offset past that range would wrap. Split
            // it: lo = ((off + 8) & 15) - 8 goes to the native operand, and
            // the remainder hi = off - lo (a multiple of 16) moves the
            // coordinate by hi / size texels of the most detailed mip, the
            // only one a gather reads. For |off| <= 7, hi is 0 and the
            // coordinate is untouched.
            const auto splat2 = [&](u32 value) {
                const u32 c = m_builder.emit_constant_int(m_type_int, value);
                return m_builder.emit_constant_composite(m_type_ivec2, {c, c});
            };
            const u32 lo = m_builder.emit_isub(
                m_type_ivec2,
                m_builder.emit_bitwise_and(
                    m_type_ivec2,
                    m_builder.emit_iadd(m_type_ivec2, offset, splat2(8u)),
                    splat2(15u)),
                splat2(8u));
            const u32 hi = m_builder.emit_isub(m_type_ivec2, offset, lo);
            const u32 size = arrayed
                ? m_builder.emit_vector_shuffle(
                      m_type_ivec2,
                      m_builder.emit_image_query_size(
                          m_type_ivec3, image,
                          m_builder.emit_constant_int(m_type_int, 0u)),
                      m_builder.emit_image_query_size(
                          m_type_ivec3, image,
                          m_builder.emit_constant_int(m_type_int, 0u)),
                      {0u, 1u})
                : m_builder.emit_image_query_size(
                      m_type_ivec2, image,
                      m_builder.emit_constant_int(m_type_int, 0u));
            const u32 shift = m_builder.emit_fdiv(
                m_type_vec2, m_builder.emit_convert_s_to_f(m_type_vec2, hi),
                m_builder.emit_convert_s_to_f(m_type_vec2, size));
            if (arrayed) {
                const u32 shift3 = m_builder.emit_composite_construct(
                    m_type_vec3,
                    {m_builder.emit_composite_extract(m_type_float, shift, {0}),
                     m_builder.emit_composite_extract(m_type_float, shift, {1}),
                     m_builder.emit_constant_float(m_type_float, 0.0f)});
                coordinate = m_builder.emit_fadd(m_type_vec3, coordinate,
                                                 shift3);
            } else {
                coordinate = m_builder.emit_fadd(m_type_vec2, coordinate, shift);
            }
            programmable = lo;
            m_builder.emit_capability(SpvCapability::ImageGatherExtended);
            m_builder.emit_capability(SpvCapability::ImageQuery);
        }

        u32 reference_scalar = 0;
        if (comparison || gather_compare) {
            Sm4Operand reference;
            if (!m_decoder.decode_operand(reference)) {
                return fail("SAMPLE_C: bad comparison reference operand");
            }
            u32 reference4 = 0;
            if (!load_vec4(reference, reference4)) return false;
            reference_scalar = m_builder.emit_composite_extract(
                m_type_float, reference4, {0});
        }

        u32 lod_scalar = 0;
        u32 raw_lod_operand = 0;  // before any bias (minmax_samplers)
        if (opcode == Sm4Opcode::SAMPLE_B ||
            opcode == Sm4Opcode::SAMPLE_L || compare_level || compare_bias) {
            Sm4Operand lod;
            if (!m_decoder.decode_operand(lod)) {
                return fail(opcode == Sm4Opcode::SAMPLE_L
                    ? "SAMPLE_L: bad LOD operand"
                    : "SAMPLE_B: bad LOD bias operand");
            }
            u32 lod4 = 0;
            if (!load_vec4(lod, lod4)) return false;
            lod_scalar = m_builder.emit_composite_extract(
                m_type_float, lod4, {0});
            raw_lod_operand = lod_scalar;
        }

        // The sampler state's MipLODBias (D3D11.3 7.18.11) applies to the
        // LOD however it was obtained: computed (sample, sample_b adding its
        // own bias, sample_c), given (sample_l: 22.4.18 "MIPLODBIAS ... are
        // honored"), derived from gradients (sample_d, "exactly as sample")
        // or fixed at 0 (sample_c_lz: 22.4.20, the bias applies "starting
        // from 0"). Metal samplers carry no bias, so it comes from the push
        // block; MinLOD/MaxLOD still clamp the biased LOD in the sampler.
        // (Windows, d3d11_a09b [cs_sampler_bias] and d3d11_a07 sampler M6.)
        u32 sampler_lod_bias = 0;
        const bool biased = opcode == Sm4Opcode::SAMPLE ||
                            opcode == Sm4Opcode::SAMPLE_B ||
                            opcode == Sm4Opcode::SAMPLE_C ||
                            opcode == Sm4Opcode::SAMPLE_L ||
                            opcode == Sm4Opcode::SAMPLE_C_LZ ||
                            opcode == Sm4Opcode::SAMPLE_D;
        // A descriptor-table translation reads the same state from
        // DxbcTranslationOptions::sampler_state_buffer.
        const bool state_buffer =
            !sampler_bias_bound() && m_sampler_state_var != 0;
        if (biased && (sampler_bias_bound() || state_buffer)) {
            if (state_buffer) {
                sampler_lod_bias = m_builder.emit_bitcast(
                    m_type_float,
                    sampler_state_word(sampler, kSamplerStateBiasWord));
            } else {
                const u32 member =
                    m_builder.emit_constant_int(m_type_uint, 0);
                const u32 slot =
                    m_builder.emit_constant_int(m_type_uint, sampler.index0);
                const u32 chain = m_builder.emit_access_chain(
                    m_type_ptr_push_float, push_block_var(), {member, slot});
                sampler_lod_bias = m_builder.emit_load(m_type_float, chain);
            }
            if (opcode == Sm4Opcode::SAMPLE_B ||
                opcode == Sm4Opcode::SAMPLE_L || compare_level) {
                lod_scalar = m_builder.emit_fadd(
                    m_type_float, lod_scalar, sampler_lod_bias);
            } else if (compare_bias) {
                // The bias the comparison's implicit LOD takes: the
                // instruction's plus the sampler's.
                lod_scalar = m_builder.emit_fadd(
                    m_type_float, lod_scalar, sampler_lod_bias);
                sampler_lod_bias = lod_scalar;
            } else {
                lod_scalar = sampler_lod_bias;
            }
            if (opcode == Sm4Opcode::SAMPLE_L ||
                opcode == Sm4Opcode::SAMPLE_C_LZ) {
                // 7.18.10: a POINT mip filter selects the integer part of
                // LOD + 0.5; Metal's nearest-mip rounds the .5 tie down
                // (Windows [cs_sample_l] row 27: LOD 1.5 -> mip 2). An
                // explicit LOD is known here, so the level is rounded before
                // the sampler sees it when the frontend flags s# as point.
                u32 point = 0;
                u32 nudged = 0;
                if (state_buffer) {
                    const u32 flags =
                        sampler_state_word(sampler, kSamplerStateFlagsWord);
                    const auto flag = [&](u32 bit) {
                        return m_builder.emit_inotequal(
                            m_type_bool,
                            m_builder.emit_bitwise_and(
                                m_type_uint, flags,
                                m_builder.emit_constant_int(m_type_uint, bit)),
                            m_const_u0);
                    };
                    point = flag(kSamplerStatePointMip);
                    nudged = flag(kSamplerStateBiasNudged);
                } else {
                    const u32 mask = m_builder.emit_load(
                        m_type_uint,
                        m_builder.emit_access_chain(
                            m_type_ptr_push_uint, push_block_var(),
                            {m_builder.emit_constant_int(m_type_uint, 4u)}));
                    point = m_builder.emit_inotequal(
                        m_type_bool,
                        m_builder.emit_bitwise_and(
                            m_type_uint, mask,
                            m_builder.emit_constant_int(
                                m_type_uint, 1u << (sampler.index0 & 31u))),
                        m_const_u0);
                    // The frontend may have nudged this sampler's bias by
                    // kPointMipTieNudge for implicit LODs; this rounding is
                    // exact, so take the nudge back out first.
                    const u32 nudge_mask = m_builder.emit_load(
                        m_type_uint,
                        m_builder.emit_access_chain(
                            m_type_ptr_push_uint, push_block_var(),
                            {m_builder.emit_constant_int(
                                m_type_uint, kPushMemberPointMipNudgeMask)}));
                    nudged = m_builder.emit_inotequal(
                        m_type_bool,
                        m_builder.emit_bitwise_and(
                            m_type_uint, nudge_mask,
                            m_builder.emit_constant_int(
                                m_type_uint, 1u << (sampler.index0 & 31u))),
                        m_const_u0);
                }
                const u32 exact_lod = m_builder.emit_fsub(
                    m_type_float, lod_scalar,
                    m_builder.emit_select(
                        m_type_float, nudged,
                        m_builder.emit_constant_float(m_type_float,
                                                      kPointMipTieNudge),
                        m_builder.emit_constant_float(m_type_float, 0.0f)));
                const u32 rounded = m_builder.emit_ext_inst(
                    m_type_float, m_glsl_std450, kGlslStd450Floor,
                    {m_builder.emit_fadd(
                        m_type_float, exact_lod,
                        m_builder.emit_constant_float(m_type_float, 0.5f))});
                lod_scalar = m_builder.emit_select(m_type_float, point,
                                                   rounded, lod_scalar);
            }
        } else if (compare_bias) {
            sampler_lod_bias = lod_scalar;
        } else if (opcode == Sm4Opcode::SAMPLE_C_LZ && !compare_level) {
            lod_scalar = m_builder.emit_constant_float(m_type_float, 0.0f);
        }

        u32 gradient_x = 0;
        u32 gradient_y = 0;
        u32 raw_gradient_x = 0;
        u32 raw_gradient_y = 0;
        if (gradients) {
            // Learn sample_d: explicit normalized-coordinate derivatives;
            // xyz carry du/dx,dv/dx,dw/dx and .w is ignored. Array indices
            // are never differentiated, so 2D arrays use vec2 gradients and
            // cube arrays use the cube-direction vec3 gradients.
            Sm4Operand dx_operand, dy_operand;
            if (!m_decoder.decode_operand(dx_operand) ||
                !m_decoder.decode_operand(dy_operand)) {
                return fail("SAMPLE_D: bad gradient operands");
            }
            u32 dx4 = 0;
            u32 dy4 = 0;
            if (!load_vec4(dx_operand, dx4) ||
                !load_vec4(dy_operand, dy4)) {
                return false;
            }
            raw_gradient_x = dx4;
            raw_gradient_y = dy4;
            if (texture_1d) {
                // A 1D gradient is du alone; the backing row has dv = 0.
                dx4 = widen_1d_address(dx4, false, false);
                dy4 = widen_1d_address(dy4, false, false);
            }
            if (cube || volume) {
                const u32 dx_x = m_builder.emit_composite_extract(
                    m_type_float, dx4, {0});
                const u32 dx_y = m_builder.emit_composite_extract(
                    m_type_float, dx4, {1});
                const u32 dx_z = m_builder.emit_composite_extract(
                    m_type_float, dx4, {2});
                gradient_x = m_builder.emit_composite_construct(
                    m_type_vec3, {dx_x, dx_y, dx_z});
                const u32 dy_x = m_builder.emit_composite_extract(
                    m_type_float, dy4, {0});
                const u32 dy_y = m_builder.emit_composite_extract(
                    m_type_float, dy4, {1});
                const u32 dy_z = m_builder.emit_composite_extract(
                    m_type_float, dy4, {2});
                gradient_y = m_builder.emit_composite_construct(
                    m_type_vec3, {dy_x, dy_y, dy_z});
            } else {
                const u32 dx_x = m_builder.emit_composite_extract(
                    m_type_float, dx4, {0});
                const u32 dx_y = m_builder.emit_composite_extract(
                    m_type_float, dx4, {1});
                gradient_x = m_builder.emit_composite_construct(
                    m_type_vec2, {dx_x, dx_y});
                const u32 dy_x = m_builder.emit_composite_extract(
                    m_type_float, dy4, {0});
                const u32 dy_y = m_builder.emit_composite_extract(
                    m_type_float, dy4, {1});
                gradient_y = m_builder.emit_composite_construct(
                    m_type_vec2, {dy_x, dy_y});
            }
            if (sampler_lod_bias != 0) {
                // A gradient sample takes no bias operand, and LOD =
                // log2(|gradient| * size): scaling both gradients by
                // 2^bias adds exactly the bias.
                const u32 scale = m_builder.emit_ext_inst(
                    m_type_float, m_glsl_std450, kGlslStd450Exp2,
                    {sampler_lod_bias});
                const bool three = cube || volume;
                const u32 type = three ? m_type_vec3 : m_type_vec2;
                const u32 scale_vector = three
                    ? m_builder.emit_composite_construct(
                          type, {scale, scale, scale})
                    : m_builder.emit_composite_construct(type,
                                                         {scale, scale});
                gradient_x = m_builder.emit_fmul(type, gradient_x,
                                                 scale_vector);
                gradient_y = m_builder.emit_fmul(type, gradient_y,
                                                 scale_vector);
            }
        }

        // sample_cl / sample_b_cl / sample_d_cl / sample_c_cl: the MinLOD
        // clamp, the last operand (D3D11.3 5.9.4.5.4). It bounds the LOD
        // from below as the sampler's MinLOD does; Metal's min_lod_clamp.
        u32 min_lod = 0;
        if (!decode_tiled_clamp(min_lod)) return false;

        // A descriptor-table sampler's border, reduction and non-normalized
        // flag live in the record. The PSO cannot bake them.
        const bool dynamic_state = m_sampler_state_var != 0;
        u32 dynamic_flags = 0;
        u32 custom_border = 0;
        u32 reduce_flag = 0;
        u32 non_norm_flag = 0;
        u32 dynamic_border_colour = 0;
        const auto flag_bit = [&](u32 flags_id, u32 bit) {
            return m_builder.emit_inotequal(
                m_type_bool,
                m_builder.emit_bitwise_and(
                    m_type_uint, flags_id,
                    m_builder.emit_constant_int(m_type_uint, bit)),
                m_const_u0);
        };
        if (dynamic_state) {
            dynamic_flags =
                sampler_state_word(sampler, kSamplerStateFlagsWord);
            custom_border = flag_bit(dynamic_flags, kSamplerStateCustomBorder);
            // The record's MINIMUM_ / MAXIMUM_ flag, and the process-wide
            // switch spirv_to_msl compiles the reduction out with while no
            // such sampler exists (graphics/shaders/minmax_reduction.hpp).
            reduce_flag = m_builder.emit_logical(
                SpvOp::OpLogicalAnd, m_type_bool, minmax_reduction_gate(),
                flag_bit(dynamic_flags, kSamplerStateMinMax));
            non_norm_flag = flag_bit(dynamic_flags, kSamplerStateNonNormalized);
            std::array<u32, 4> channels{};
            for (u32 c = 0; c < 4; ++c) {
                channels[c] = m_builder.emit_bitcast(
                    m_type_float,
                    sampler_state_word(sampler, kSamplerStateBorderWord + c));
            }
            dynamic_border_colour = m_builder.emit_composite_construct(
                m_type_vec4,
                {channels[0], channels[1], channels[2], channels[3]});
            // Texel coordinates (D3D12_SAMPLER_FLAG_NON_NORMALIZED_COORDINATES)
            // on a 1D or 2D view. Cube and volume samplers are not a legal
            // target of the flag; dividing those coordinates would invent a
            // mapping the flag does not have.
            if (!cube && !volume) {
                m_builder.emit_capability(SpvCapability::ImageQuery);
                const u32 size_type = arrayed ? m_type_ivec3 : m_type_ivec2;
                const u32 size = m_builder.emit_image_query_size(
                    size_type, image,
                    m_builder.emit_constant_int(m_type_int, 0u));
                const u32 sx = m_builder.emit_convert_s_to_f(
                    m_type_float,
                    m_builder.emit_composite_extract(m_type_int, size, {0}));
                const u32 sy = m_builder.emit_convert_s_to_f(
                    m_type_float,
                    m_builder.emit_composite_extract(m_type_int, size, {1}));
                const auto scale_vec = [&](u32 vec, u32 type) {
                    const u32 x = m_builder.emit_composite_extract(
                        m_type_float, vec, {0});
                    const u32 y = m_builder.emit_composite_extract(
                        m_type_float, vec, {1});
                    const u32 nx = m_builder.emit_fdiv(m_type_float, x, sx);
                    const u32 ny = m_builder.emit_fdiv(m_type_float, y, sy);
                    if (type == m_type_vec2) {
                        return m_builder.emit_composite_construct(
                            m_type_vec2, {nx, ny});
                    }
                    if (type == m_type_vec4) {
                        return m_builder.emit_composite_construct(
                            m_type_vec4,
                            {nx, ny,
                             m_builder.emit_composite_extract(m_type_float, vec,
                                                              {2}),
                             m_builder.emit_composite_extract(m_type_float, vec,
                                                              {3})});
                    }
                    return m_builder.emit_composite_construct(
                        m_type_vec3,
                        {nx, ny,
                         m_builder.emit_composite_extract(m_type_float, vec,
                                                          {2})});
                };
                const u32 coord_type = arrayed ? m_type_vec3 : m_type_vec2;
                coordinate = m_builder.emit_select(
                    coord_type, non_norm_flag,
                    scale_vec(coordinate, coord_type), coordinate);
                if (gradients && gradient_x != 0) {
                    gradient_x = m_builder.emit_select(
                        m_type_vec2, non_norm_flag,
                        scale_vec(gradient_x, m_type_vec2), gradient_x);
                    gradient_y = m_builder.emit_select(
                        m_type_vec2, non_norm_flag,
                        scale_vec(gradient_y, m_type_vec2), gradient_y);
                    raw_gradient_x = m_builder.emit_select(
                        m_type_vec4, non_norm_flag,
                        scale_vec(raw_gradient_x, m_type_vec4), raw_gradient_x);
                    raw_gradient_y = m_builder.emit_select(
                        m_type_vec4, non_norm_flag,
                        scale_vec(raw_gradient_y, m_type_vec4), raw_gradient_y);
                }
            }
        }

        // Border twin: the same operation through s#'s opaque-white-border
        // twin. `twin_sampled_image` is 0 when this sampler has none.
        // A descriptor-table translation's twins are loaded only on the
        // branch that the record's custom-border flag takes.
        u32 twin_sampled_image = 0;
        u32 border_colour = 0;
        if (!dynamic_state) {
        if (const auto twin = m_border_twin_vars.find(sampler.index0);
            twin != m_border_twin_vars.end()) {
            // The push-block float is the border a sampler record stores.
            // An integer gather converts it below (7.18.9.1, 3.2.2). A
            // UINT border of (0,0,0,0), (0,0,0,1) or (1,1,1,1) is a Metal
            // border and has no twin; any other UINT border is not stored.
            twin_sampled_image = m_builder.emit_sampled_image(
                sampled_image_type, image,
                m_builder.emit_load(m_type_sampler, twin->second));
            // s0..s7 in member 1, s8..s15 in member 11.
            border_colour = m_builder.emit_load(
                m_type_vec4,
                m_builder.emit_access_chain(
                    m_type_ptr_push_vec4, m_sampler_biases,
                    {m_builder.emit_constant_int(
                         m_type_uint, sampler.index0 < 8 ? 1u : kPushMemberBorderHigh),
                     m_builder.emit_constant_int(m_type_uint,
                                                 sampler.index0 % 8u)}));
        }
        }
        // A comparison through a twinned sampler: D3D11.3 7.18.9.1 compares
        // the reference against the border's red, any float (measured
        // unclamped even for UNORM views, d3d11_view_line_probe [cmp]),
        // where Metal's borders are 0 and 1. The frontend puts (red,
        // D3D11_COMPARISON_FUNC) in the twin's border colour.
        // `border_compare(x)` is the function applied to (reference, x) as
        // 0.0 / 1.0: FUNC - 1 is a mask of LESS (1), EQUAL (2), GREATER (4).
        // `separator` is a reference the function tells border 0 from border
        // 1 by -- .5 for the ordering functions, 0 for (NOT_)EQUAL -- so s#
        // (border 0) and its twin (border 1) compared against it differ by
        // exactly the weight the filter gave border texels: every real texel
        // compares the same through both.
        u32 separator = 0;
        std::function<u32(u32)> border_compare;
        if (twin_sampled_image != 0 && (comparison || gather_compare)) {
            const u32 func = m_builder.emit_convert_f_to_u(
                m_type_uint, m_builder.emit_composite_extract(
                                 m_type_float, border_colour, {1}));
            const u32 mask = m_builder.emit_isub(m_type_uint, func,
                                                 m_builder.emit_constant_int(
                                                     m_type_uint, 1u));
            const auto has = [&](u32 bit) {
                return m_builder.emit_inotequal(
                    m_type_bool,
                    m_builder.emit_bitwise_and(
                        m_type_uint, mask,
                        m_builder.emit_constant_int(m_type_uint, bit)),
                    m_const_u0);
            };
            const u32 wants_less = has(1u);
            const u32 wants_equal = has(2u);
            const u32 wants_greater = has(4u);
            const u32 zero = m_builder.emit_constant_float(m_type_float, 0.0f);
            const u32 one = m_builder.emit_constant_float(m_type_float, 1.0f);
            border_compare = [&, wants_less, wants_equal, wants_greater, zero,
                              one](u32 x) {
                const u32 less = m_builder.emit_logical(
                    SpvOp::OpLogicalAnd, m_type_bool, wants_less,
                    m_builder.emit_ford_lt(m_type_bool, reference_scalar, x));
                const u32 equal = m_builder.emit_logical(
                    SpvOp::OpLogicalAnd, m_type_bool, wants_equal,
                    m_builder.emit_ford_eq(m_type_bool, reference_scalar, x));
                const u32 greater = m_builder.emit_logical(
                    SpvOp::OpLogicalAnd, m_type_bool, wants_greater,
                    m_builder.emit_ford_lt(m_type_bool, x, reference_scalar));
                const u32 pass = m_builder.emit_logical(
                    SpvOp::OpLogicalOr, m_type_bool, less,
                    m_builder.emit_logical(SpvOp::OpLogicalOr, m_type_bool,
                                           equal, greater));
                return m_builder.emit_select(m_type_float, pass, one, zero);
            };
            // EQUAL (mask 2) and NOT_EQUAL (mask 5) are the functions whose
            // mask has EQUAL alone or LESS and GREATER without it.
            const u32 equality = m_builder.emit_logical(
                SpvOp::OpLogicalOr, m_type_bool,
                m_builder.emit_integer_compare(
                    SpvOp::OpIEqual, m_type_bool, mask,
                    m_builder.emit_constant_int(m_type_uint, 2u)),
                m_builder.emit_integer_compare(
                    SpvOp::OpIEqual, m_type_bool, mask,
                    m_builder.emit_constant_int(m_type_uint, 5u)));
            separator = m_builder.emit_select(
                m_type_float, equality, zero,
                m_builder.emit_constant_float(m_type_float, 0.5f));
        }

        // A MINIMUM_ / MAXIMUM_ sampler: the reduction, in the shader.
        if (!gather && !comparison && sampler.index0 < 16u &&
            (m_options.minmax_samplers & (1u << sampler.index0)) != 0 &&
            sampler_bias_bound()) {
            if (min_lod != 0 || m_tiled.has_status) {
                // The push-block reduction reads its MinLOD from the block;
                // a D3D11 translation (tier 1) has neither instruction.
                return fail("sample_cl / status through a push-block "
                            "min/max sampler");
            }
            MinMaxSample sample{};
            sample.image = image;
            sample.sampled_image = sampled_image;
            sample.twin_sampled_image = twin_sampled_image;
            sample.border_colour = border_colour;
            sample.coordinate = coordinate;
            sample.texture_1d = texture_1d;
            sample.cube = cube;
            sample.volume = volume;
            sample.arrayed = arrayed;
            sample.slot = sampler.index0;
            sample.lod = opcode == Sm4Opcode::SAMPLE_L
                ? MinMaxSample::Lod::Explicit
                : opcode == Sm4Opcode::SAMPLE_D ? MinMaxSample::Lod::Gradient
                                                : MinMaxSample::Lod::Implicit;
            sample.lod_operand = raw_lod_operand;
            sample.gradient_x = raw_gradient_x;
            sample.gradient_y = raw_gradient_y;
            sample.offset = offset_texels;
            // SM 6.7: a Texture1D's v offset is 0 like aoffimmi's.
            if (dynamic_offset != 0) {
                sample.offset_id = texture_1d ? m_builder.emit_composite_insert(
                                                    m_type_ivec4, m_builder.emit_constant_int(m_type_int, 0u),
                                                    dynamic_offset, {1})
                                              : dynamic_offset;
            }
            if (texture_1d) sample.offset[1] = 0;
            const u32 reduced =
                apply_swizzle(emit_minmax_sample(sample), resource.swizzle);
            if (!store_vec4(dst, reduced)) return false;
            break;
        }

        // lo + (hi - lo) * border, per component: hi - lo is the weight the
        // filter gave the border.
        const auto blend_border = [&](u32 lo, u32 hi, u32 border) {
            return m_builder.emit_fadd(
                m_type_vec4, lo,
                m_builder.emit_fmul(m_type_vec4,
                                    m_builder.emit_fsub(m_type_vec4, hi, lo),
                                    border));
        };
        // D3D11.3 3.2.2: a float becomes an integer format by
        // round-to-nearest-even, then clamps to the type. 7.18.9.1 clamps
        // the border into the format before it is used as a texel; the
        // gather return type is the 32-bit component. NaN has no integer
        // encoding and is 0. ftou/ftoi round toward zero, so round first
        // and then use their range selects (OpConvertFTo* is undefined
        // outside the type).
        const auto border_float_to_texel = [&](u32 blended) -> u32 {
            const u32 rounded = m_builder.emit_ext_inst(
                m_type_vec4, m_glsl_std450, kGlslStd450RoundEven, {blended});
            const u32 zero_float = make_vec4_imm(0.0f, 0.0f, 0.0f, 0.0f);
            const u32 is_nan = m_builder.emit_is_nan(m_type_bvec4, rounded);
            if (uint_gather) {
                // 2^32 is the first value outside uint32.
                const u32 upper = make_vec4_imm(
                    4294967296.0f, 4294967296.0f, 4294967296.0f, 4294967296.0f);
                const u32 below =
                    m_builder.emit_ford_lt(m_type_bvec4, rounded, zero_float);
                const u32 above =
                    m_builder.emit_ford_ge(m_type_bvec4, rounded, upper);
                u32 safe = m_builder.emit_select(
                    m_type_vec4, is_nan, zero_float, rounded);
                safe = m_builder.emit_select(m_type_vec4, below, zero_float, safe);
                safe = m_builder.emit_select(m_type_vec4, above, zero_float, safe);
                u32 result = m_builder.emit_convert_f_to_u(m_type_uvec4, safe);
                result = m_builder.emit_select(
                    m_type_uvec4, above, m_const_u_ffff4, result);
                result = m_builder.emit_select(
                    m_type_uvec4, below, m_const_u_zero4, result);
                return m_builder.emit_select(
                    m_type_uvec4, is_nan, m_const_u_zero4, result);
            }
            // 2^31 and -2^31 bound int32; both are exact floats.
            const u32 min_float = make_vec4_imm(
                -2147483648.0f, -2147483648.0f, -2147483648.0f, -2147483648.0f);
            const u32 upper_float = make_vec4_imm(
                2147483648.0f, 2147483648.0f, 2147483648.0f, 2147483648.0f);
            const u32 below =
                m_builder.emit_ford_lt(m_type_bvec4, rounded, min_float);
            const u32 above =
                m_builder.emit_ford_ge(m_type_bvec4, rounded, upper_float);
            u32 safe = m_builder.emit_select(
                m_type_vec4, is_nan, zero_float, rounded);
            safe = m_builder.emit_select(m_type_vec4, below, zero_float, safe);
            safe = m_builder.emit_select(m_type_vec4, above, zero_float, safe);
            const u32 converted =
                m_builder.emit_convert_f_to_s(m_type_ivec4, safe);
            const u32 int_zero = m_builder.emit_constant_int(m_type_int, 0u);
            const u32 int_min =
                m_builder.emit_constant_int(m_type_int, 0x80000000u);
            const u32 int_max =
                m_builder.emit_constant_int(m_type_int, 0x7FFFFFFFu);
            const u32 zero4 = m_builder.emit_constant_composite(
                m_type_ivec4, {int_zero, int_zero, int_zero, int_zero});
            const u32 min4 = m_builder.emit_constant_composite(
                m_type_ivec4, {int_min, int_min, int_min, int_min});
            const u32 max4 = m_builder.emit_constant_composite(
                m_type_ivec4, {int_max, int_max, int_max, int_max});
            u32 result = m_builder.emit_select(m_type_ivec4, below, min4, converted);
            result = m_builder.emit_select(m_type_ivec4, above, max4, result);
            return m_builder.emit_select(m_type_ivec4, is_nan, zero4, result);
        };
        bool failed = false;
        // The twin sampler for this instruction. An array element's index is
        // the same one the record uses (binding + element).
        const auto load_twin_image = [&]() -> u32 {
            const auto twin = m_border_twin_vars.find(sampler.index0);
            if (twin == m_border_twin_vars.end()) return 0;
            u32 pointer = twin->second;
            const ResourceRange* range =
                find_range(DxbcRegisterClass::Sampler, sampler.index0);
            if (range != nullptr && range->is_array) {
                u32 element = m_const_u0;
                if (const auto rec = m_sampler_record_index.find(sampler.index0);
                    rec != m_sampler_record_index.end()) {
                    element = m_builder.emit_isub(
                        m_type_uint, rec->second,
                        m_builder.emit_constant_int(
                            m_type_uint, range->resolved.binding));
                }
                if (sampler.nonuniform && sampler.register_relative) {
                    m_builder.emit_capability(
                        SpvCapability::SampledImageArrayNonUniformIndexing);
                    m_builder.emit_decorate(element, SpvDecoration::NonUniform);
                }
                pointer = m_builder.emit_access_chain(
                    m_type_ptr_uniform_constant_sampler, twin->second,
                    {element});
                if (sampler.nonuniform && sampler.register_relative) {
                    m_builder.emit_decorate(pointer, SpvDecoration::NonUniform);
                }
            }
            return m_builder.emit_sampled_image(
                sampled_image_type, image,
                m_builder.emit_load(m_type_sampler, pointer));
        };
        // Both arms store into a private temporary so a nested reduction
        // (which ends in its own merge block) can still join here.
        const u32 result_ptr_type = m_builder.emit_type_pointer(
            SpvStorageClass::Private, m_type_vec4);
        const auto choose_vec4 = [&](u32 cond, auto&& yes, auto&& no) -> u32 {
            const u32 slot = m_builder.emit_variable(
                result_ptr_type, SpvStorageClass::Private);
            // SPIR-V 1.4+ lists every global the entry point uses. The
            // interface was collected before instructions ran.
            m_entry_interface.push_back(slot);
            const u32 merge = m_builder.allocate_id();
            const u32 yes_block = m_builder.allocate_id();
            const u32 no_block = m_builder.allocate_id();
            m_builder.emit_selection_merge(merge);
            m_builder.emit_branch_conditional(cond, yes_block, no_block);
            m_builder.emit_label_id(yes_block);
            const u32 yes_value = yes();
            if (!failed) m_builder.emit_store(slot, yes_value);
            m_builder.emit_branch(merge);
            m_builder.emit_label_id(no_block);
            const u32 no_value = no();
            if (!failed) m_builder.emit_store(slot, no_value);
            m_builder.emit_branch(merge);
            m_builder.emit_label_id(merge);
            if (failed) return 0;
            return m_builder.emit_load(m_type_vec4, slot);
        };
        const auto with_dynamic_border =
            [&](u32 plain, auto&& blend_with_twin) -> u32 {
            // The record's custom-border flag selects the opaque-white
            // twin. Callers that are not a descriptor-table sample keep
            // plain: a border the record does not store is not invented.
            if (!dynamic_state) return plain;
            return choose_vec4(
                custom_border,
                [&] { return blend_with_twin(load_twin_image()); },
                [&] { return plain; });
        };

        const auto emit_ordinary = [&]() -> u32 {
        // OpSampledImage is only legal in the block that samples it.
        const u32 sampled_image = m_builder.emit_sampled_image(
            sampled_image_type, image, sampler_value);
        u32 sampled = 0;
        if (gather_compare) {
            if (uint_gather || sint_gather) {
                failed = !fail("GATHER4_C: integer textures have no comparison");
                return 0u;
            }
            sampled = m_builder.emit_image_dref_gather(
                m_type_vec4, sampled_image, coordinate, reference_scalar,
                immediate_offset, programmable);
            if (twin_sampled_image != 0) {
                // The lanes whose texel is the border compare differently
                // through s# and its twin at the separator; those take the
                // border's own comparison.
                const u32 through_lo = m_builder.emit_image_dref_gather(
                    m_type_vec4, sampled_image, coordinate, separator,
                    immediate_offset, programmable);
                const u32 through_hi = m_builder.emit_image_dref_gather(
                    m_type_vec4, twin_sampled_image, coordinate, separator,
                    immediate_offset, programmable);
                const u32 border_lane = m_builder.emit_funord_ne(
                    m_type_bvec4, through_lo, through_hi);
                const u32 border_result = border_compare(
                    m_builder.emit_composite_extract(m_type_float,
                                                     border_colour, {0}));
                sampled = m_builder.emit_select(
                    m_type_vec4, border_lane,
                    m_builder.emit_composite_construct(
                        m_type_vec4, {border_result, border_result,
                                      border_result, border_result}),
                    sampled);
            } else if (dynamic_state) {
                sampled = with_dynamic_border(sampled, [&](u32 twin_img) {
                    // Same separator test as the static twin, against the
                    // record's (red, comparison func).
                    const u32 func = m_builder.emit_convert_f_to_u(
                        m_type_uint,
                        m_builder.emit_composite_extract(
                            m_type_float, dynamic_border_colour, {1}));
                    const u32 mask = m_builder.emit_isub(
                        m_type_uint, func,
                        m_builder.emit_constant_int(m_type_uint, 1u));
                    const auto has = [&](u32 bit) {
                        return m_builder.emit_inotequal(
                            m_type_bool,
                            m_builder.emit_bitwise_and(
                                m_type_uint, mask,
                                m_builder.emit_constant_int(m_type_uint, bit)),
                            m_const_u0);
                    };
                    const u32 wants_less = has(1u);
                    const u32 wants_equal = has(2u);
                    const u32 wants_greater = has(4u);
                    const u32 zero =
                        m_builder.emit_constant_float(m_type_float, 0.0f);
                    const u32 one =
                        m_builder.emit_constant_float(m_type_float, 1.0f);
                    const u32 red = m_builder.emit_composite_extract(
                        m_type_float, dynamic_border_colour, {0});
                    const auto cmp_one = [&](u32 x) {
                        const u32 less = m_builder.emit_logical(
                            SpvOp::OpLogicalAnd, m_type_bool, wants_less,
                            m_builder.emit_ford_lt(m_type_bool,
                                                   reference_scalar, x));
                        const u32 equal = m_builder.emit_logical(
                            SpvOp::OpLogicalAnd, m_type_bool, wants_equal,
                            m_builder.emit_ford_eq(m_type_bool,
                                                   reference_scalar, x));
                        const u32 greater = m_builder.emit_logical(
                            SpvOp::OpLogicalAnd, m_type_bool, wants_greater,
                            m_builder.emit_ford_lt(m_type_bool, x,
                                                   reference_scalar));
                        const u32 pass = m_builder.emit_logical(
                            SpvOp::OpLogicalOr, m_type_bool, less,
                            m_builder.emit_logical(SpvOp::OpLogicalOr,
                                                   m_type_bool, equal,
                                                   greater));
                        return m_builder.emit_select(m_type_float, pass, one,
                                                     zero);
                    };
                    const u32 equality = m_builder.emit_logical(
                        SpvOp::OpLogicalOr, m_type_bool,
                        m_builder.emit_integer_compare(
                            SpvOp::OpIEqual, m_type_bool, mask,
                            m_builder.emit_constant_int(m_type_uint, 2u)),
                        m_builder.emit_integer_compare(
                            SpvOp::OpIEqual, m_type_bool, mask,
                            m_builder.emit_constant_int(m_type_uint, 5u)));
                    const u32 sep = m_builder.emit_select(
                        m_type_float, equality, zero,
                        m_builder.emit_constant_float(m_type_float, 0.5f));
                    const u32 through_lo = m_builder.emit_image_dref_gather(
                        m_type_vec4,
                        m_builder.emit_sampled_image(sampled_image_type, image,
                                                     sampler_value),
                        coordinate, sep, immediate_offset, programmable);
                    const u32 through_hi = m_builder.emit_image_dref_gather(
                        m_type_vec4, twin_img, coordinate, sep,
                        immediate_offset, programmable);
                    const u32 border_lane = m_builder.emit_funord_ne(
                        m_type_bvec4, through_lo, through_hi);
                    const u32 border_result = cmp_one(red);
                    return m_builder.emit_select(
                        m_type_vec4, border_lane,
                        m_builder.emit_composite_construct(
                            m_type_vec4, {border_result, border_result,
                                          border_result, border_result}),
                        sampled);
                });
            }
            sampled = apply_swizzle(sampled, resource.swizzle);
        } else if (gather) {
            const u32 component = m_builder.emit_constant_int(
                m_type_uint, sampler.swizzle & 0x3u);
            const u32 gather_type = uint_gather ? m_type_uvec4
                                    : sint_gather ? m_type_ivec4
                                                  : m_type_vec4;
            sampled = m_builder.emit_image_gather(
                gather_type, sampled_image, coordinate, component,
                immediate_offset, programmable);
            if (uint_gather || sint_gather) {
                // D3D11.3 7.18.9.1 / 3.2.2 and gather4 (22.4.2). The border
                // is a float4. lo is the gather through transparent black,
                // hi through the opaque-white twin, blended as
                // lo + (hi - lo) * border, then round-to-nearest-even into
                // the 32-bit integer return (NaN becomes 0). A lane both
                // gathers return alike keeps the texel's bits. The
                // descriptor-table path reads the float from the sampler
                // record when the custom-border flag is set. A shader-model
                // 5.0 sampler reads the same float from the push-block twin.
                // A UINT border of (0,0,0,0), (0,0,0,1) or (1,1,1,1) is
                // applied by the Metal sampler, so this blend is not used
                // for it and no other UINT border is stored.
                const u32 lo = sampled;
                const u32 plain = m_builder.emit_bitcast(m_type_vec4, lo);
                const auto blend_integer_border =
                    [&](u32 twin_img, u32 border_vec) {
                        const u32 hi = m_builder.emit_image_gather(
                            gather_type, twin_img, coordinate, component,
                            immediate_offset, programmable);
                        const u32 lo_f = uint_gather
                            ? m_builder.emit_convert_u_to_f(m_type_vec4, lo)
                            : m_builder.emit_convert_s_to_f(m_type_vec4, lo);
                        const u32 hi_f = uint_gather
                            ? m_builder.emit_convert_u_to_f(m_type_vec4, hi)
                            : m_builder.emit_convert_s_to_f(m_type_vec4, hi);
                        const u32 channel = m_builder.emit_composite_extract(
                            m_type_float, border_vec, {sampler.swizzle & 0x3u});
                        const u32 blended = blend_border(
                            lo_f, hi_f,
                            m_builder.emit_composite_construct(
                                m_type_vec4,
                                {channel, channel, channel, channel}));
                        const u32 border_texel =
                            border_float_to_texel(blended);
                        const u32 same = m_builder.emit_integer_compare(
                            SpvOp::OpIEqual, m_type_bvec4, lo, hi);
                        return m_builder.emit_bitcast(
                            m_type_vec4,
                            m_builder.emit_select(
                                gather_type, same, lo, border_texel));
                    };
                if (dynamic_state) {
                    sampled = with_dynamic_border(
                        plain, [&](u32 twin_img) {
                            return blend_integer_border(
                                twin_img, dynamic_border_colour);
                        });
                } else if (twin_sampled_image != 0) {
                    sampled = blend_integer_border(twin_sampled_image,
                                                   border_colour);
                } else {
                    sampled = plain;
                }
            } else {
                if (twin_sampled_image != 0) {
                    // Gather returns one channel of four texels, so the border
                    // term is that channel of the colour, in every lane.
                    const u32 channel = m_builder.emit_composite_extract(
                        m_type_float, border_colour, {sampler.swizzle & 0x3u});
                    const u32 twin = m_builder.emit_image_gather(
                        m_type_vec4, twin_sampled_image, coordinate, component,
                        immediate_offset, programmable);
                    sampled = blend_border(
                        sampled, twin,
                        m_builder.emit_composite_construct(
                            m_type_vec4, {channel, channel, channel, channel}));
                } else if (dynamic_state) {
                    sampled = with_dynamic_border(sampled, [&](u32 twin_img) {
                        const u32 channel = m_builder.emit_composite_extract(
                            m_type_float, dynamic_border_colour,
                            {sampler.swizzle & 0x3u});
                        const u32 hi = m_builder.emit_image_gather(
                            m_type_vec4, twin_img, coordinate, component,
                            immediate_offset, programmable);
                        return blend_border(
                            sampled, hi,
                            m_builder.emit_composite_construct(
                                m_type_vec4,
                                {channel, channel, channel, channel}));
                    });
                }
            }
            sampled = apply_swizzle(sampled, resource.swizzle);
        } else if (comparison) {
            const auto compare = [&](u32 through, u32 reference) {
                if (compare_gradient) {
                    return m_builder.emit_image_sample_dref_grad(
                        m_type_float, through, coordinate, reference,
                        gradient_x, gradient_y, immediate_offset, min_lod);
                }
                // An immediate offset (aoffimmi) applies to comparisons too.
                if (immediate_offset != 0) {
                    return opcode == Sm4Opcode::SAMPLE_C_LZ
                        ? m_builder.emit_image_sample_dref_lz_offset(
                              m_type_float, through, coordinate, reference,
                              lod_scalar, immediate_offset)
                        : m_builder.emit_image_sample_dref_offset(
                              m_type_float, through, coordinate, reference,
                              sampler_lod_bias, immediate_offset);
                }
                return opcode == Sm4Opcode::SAMPLE_C_LZ
                    ? m_builder.emit_image_sample_dref_lz(
                          m_type_float, through, coordinate, reference,
                          lod_scalar)
                    : m_builder.emit_image_sample_dref(
                          m_type_float, through, coordinate, reference,
                          sampler_lod_bias, min_lod);
            };
            u32 comparison_result = compare(sampled_image, reference_scalar);
            if (twin_sampled_image != 0) {
                // s# compared border texels against 0; move the border's
                // weight onto the comparison against its real red.
                const u32 border_weight = m_builder.emit_ext_inst(
                    m_type_float, m_glsl_std450, kGlslStd450FAbs,
                    {m_builder.emit_fsub(
                        m_type_float, compare(twin_sampled_image, separator),
                        compare(sampled_image, separator))});
                const u32 correction = m_builder.emit_fsub(
                    m_type_float,
                    border_compare(m_builder.emit_composite_extract(
                        m_type_float, border_colour, {0})),
                    border_compare(
                        m_builder.emit_constant_float(m_type_float, 0.0f)));
                comparison_result = m_builder.emit_fadd(
                    m_type_float, comparison_result,
                    m_builder.emit_fmul(m_type_float, border_weight,
                                        correction));
            } else if (dynamic_state) {
                const u32 plain = m_builder.emit_composite_construct(
                    m_type_vec4,
                    {comparison_result, comparison_result, comparison_result,
                     comparison_result});
                sampled = with_dynamic_border(plain, [&](u32 twin_img) {
                    const u32 func = m_builder.emit_convert_f_to_u(
                        m_type_uint,
                        m_builder.emit_composite_extract(
                            m_type_float, dynamic_border_colour, {1}));
                    const u32 mask = m_builder.emit_isub(
                        m_type_uint, func,
                        m_builder.emit_constant_int(m_type_uint, 1u));
                    const auto has = [&](u32 bit) {
                        return m_builder.emit_inotequal(
                            m_type_bool,
                            m_builder.emit_bitwise_and(
                                m_type_uint, mask,
                                m_builder.emit_constant_int(m_type_uint, bit)),
                            m_const_u0);
                    };
                    const u32 wants_less = has(1u);
                    const u32 wants_equal = has(2u);
                    const u32 wants_greater = has(4u);
                    const u32 zero =
                        m_builder.emit_constant_float(m_type_float, 0.0f);
                    const u32 one =
                        m_builder.emit_constant_float(m_type_float, 1.0f);
                    const auto cmp_one = [&](u32 x) {
                        const u32 less = m_builder.emit_logical(
                            SpvOp::OpLogicalAnd, m_type_bool, wants_less,
                            m_builder.emit_ford_lt(m_type_bool,
                                                   reference_scalar, x));
                        const u32 equal = m_builder.emit_logical(
                            SpvOp::OpLogicalAnd, m_type_bool, wants_equal,
                            m_builder.emit_ford_eq(m_type_bool,
                                                   reference_scalar, x));
                        const u32 greater = m_builder.emit_logical(
                            SpvOp::OpLogicalAnd, m_type_bool, wants_greater,
                            m_builder.emit_ford_lt(m_type_bool, x,
                                                   reference_scalar));
                        const u32 pass = m_builder.emit_logical(
                            SpvOp::OpLogicalOr, m_type_bool, less,
                            m_builder.emit_logical(SpvOp::OpLogicalOr,
                                                   m_type_bool, equal,
                                                   greater));
                        return m_builder.emit_select(m_type_float, pass, one,
                                                     zero);
                    };
                    const u32 equality = m_builder.emit_logical(
                        SpvOp::OpLogicalOr, m_type_bool,
                        m_builder.emit_integer_compare(
                            SpvOp::OpIEqual, m_type_bool, mask,
                            m_builder.emit_constant_int(m_type_uint, 2u)),
                        m_builder.emit_integer_compare(
                            SpvOp::OpIEqual, m_type_bool, mask,
                            m_builder.emit_constant_int(m_type_uint, 5u)));
                    const u32 sep = m_builder.emit_select(
                        m_type_float, equality, zero,
                        m_builder.emit_constant_float(m_type_float, 0.5f));
                    const auto compare_ref = [&](u32 through, u32 reference) {
                        if (compare_gradient) {
                            return m_builder.emit_image_sample_dref_grad(
                                m_type_float, through, coordinate, reference,
                                gradient_x, gradient_y, immediate_offset,
                                min_lod);
                        }
                        if (immediate_offset != 0) {
                            return opcode == Sm4Opcode::SAMPLE_C_LZ
                                ? m_builder.emit_image_sample_dref_lz_offset(
                                      m_type_float, through, coordinate,
                                      reference, lod_scalar, immediate_offset)
                                : m_builder.emit_image_sample_dref_offset(
                                      m_type_float, through, coordinate,
                                      reference, sampler_lod_bias,
                                      immediate_offset);
                        }
                        return opcode == Sm4Opcode::SAMPLE_C_LZ
                            ? m_builder.emit_image_sample_dref_lz(
                                  m_type_float, through, coordinate, reference,
                                  lod_scalar)
                            : m_builder.emit_image_sample_dref(
                                  m_type_float, through, coordinate, reference,
                                  sampler_lod_bias, min_lod);
                    };
                    const u32 border_weight = m_builder.emit_ext_inst(
                        m_type_float, m_glsl_std450, kGlslStd450FAbs,
                        {m_builder.emit_fsub(
                            m_type_float, compare_ref(twin_img, sep),
                            compare_ref(m_builder.emit_sampled_image(
                                            sampled_image_type, image,
                                            sampler_value),
                                        sep))});
                    const u32 red = m_builder.emit_composite_extract(
                        m_type_float, dynamic_border_colour, {0});
                    const u32 correction = m_builder.emit_fsub(
                        m_type_float, cmp_one(red), cmp_one(zero));
                    const u32 corrected = m_builder.emit_fadd(
                        m_type_float, comparison_result,
                        m_builder.emit_fmul(m_type_float, border_weight,
                                            correction));
                    return m_builder.emit_composite_construct(
                        m_type_vec4,
                        {corrected, corrected, corrected, corrected});
                });
            }
            if (sampled == 0u) {
                sampled = m_builder.emit_composite_construct(
                    m_type_vec4,
                    {comparison_result, comparison_result, comparison_result,
                     comparison_result});
            }
        } else {
            const auto sample = [&](u32 through) {
                return opcode == Sm4Opcode::SAMPLE_D
                ? m_builder.emit_image_sample_grad(
                      m_type_vec4, through, coordinate,
                      gradient_x, gradient_y, immediate_offset, min_lod)
                : opcode == Sm4Opcode::SAMPLE_L
                ? m_builder.emit_image_sample_explicit_lod(
                      m_type_vec4, through, coordinate, lod_scalar,
                      immediate_offset)
                : m_builder.emit_image_sample(
                      m_type_vec4, through, coordinate, lod_scalar,
                      immediate_offset, min_lod);
            };
            sampled = sample(sampled_image);
            if (twin_sampled_image != 0) {
                sampled = blend_border(sampled, sample(twin_sampled_image),
                                       border_colour);
            } else if (dynamic_state) {
                sampled = with_dynamic_border(sampled, [&](u32 twin_img) {
                    return blend_border(sampled, sample(twin_img),
                                       dynamic_border_colour);
                });
            }
            sampled = apply_swizzle(sampled, resource.swizzle);
        }
        return sampled;
        };

        u32 produced = 0;
        if (dynamic_state && !gather && !comparison) {
            const auto reduced = [&](bool with_border) -> u32 {
                MinMaxSample sample{};
                sample.image = image;
                sample.coordinate = coordinate;
                sample.texture_1d = texture_1d;
                sample.cube = cube;
                sample.volume = volume;
                sample.arrayed = arrayed;
                sample.slot = sampler.index0;
                sample.lod = opcode == Sm4Opcode::SAMPLE_L
                    ? MinMaxSample::Lod::Explicit
                    : opcode == Sm4Opcode::SAMPLE_D
                        ? MinMaxSample::Lod::Gradient
                        : MinMaxSample::Lod::Implicit;
                sample.lod_operand = raw_lod_operand;
                sample.gradient_x = raw_gradient_x;
                sample.gradient_y = raw_gradient_y;
                sample.offset = offset_texels;
                // SM 6.7: a Texture1D's v offset is 0 like aoffimmi's.
                if (dynamic_offset != 0) {
                    sample.offset_id = texture_1d ? m_builder.emit_composite_insert(
                                                        m_type_ivec4, m_builder.emit_constant_int(m_type_int, 0u),
                                                        dynamic_offset, {1})
                                                  : dynamic_offset;
                }
                if (texture_1d) sample.offset[1] = 0;
                sample.sampler_id = sampler_value;
                sample.sampled_image_type = sampled_image_type;
                sample.filter_id = m_builder.emit_bitwise_and(
                    m_type_uint,
                    m_builder.emit_shift_right_logical(
                        m_type_uint, dynamic_flags,
                        m_builder.emit_constant_int(
                            m_type_uint, kSamplerStateFilterShift)),
                    m_builder.emit_constant_int(m_type_uint,
                                                kSamplerStateFilterMask));
                sample.min_lod_id = m_builder.emit_bitcast(
                    m_type_float,
                    sampler_state_word(sampler, kSamplerStateMinLodWord));
                if (min_lod != 0) {
                    // Both bound the LOD from below (D3D11.3 5.9.4.5.4):
                    // the per-sample clamp joins the sampler's MinLOD.
                    sample.min_lod_id = m_builder.emit_ext_inst(
                        m_type_float, m_glsl_std450, kGlslStd450FMax,
                        {sample.min_lod_id, min_lod});
                }
                sample.max_lod_id = m_builder.emit_bitcast(
                    m_type_float,
                    sampler_state_word(sampler, kSamplerStateMaxLodWord));
                sample.max_aniso_id = m_builder.emit_convert_u_to_f(
                    m_type_float,
                    m_builder.emit_bitwise_and(
                        m_type_uint,
                        m_builder.emit_shift_right_logical(
                            m_type_uint, dynamic_flags,
                            m_builder.emit_constant_int(
                                m_type_uint, kSamplerStateAnisoShift)),
                        m_builder.emit_constant_int(
                            m_type_uint, kSamplerStateAnisoMask)));
                sample.bias_id = m_builder.emit_bitcast(
                    m_type_float,
                    sampler_state_word(sampler, kSamplerStateBiasWord));
                if (with_border) {
                    // The sampler load may dominate the fetch; OpSampledImage
                    // is emitted beside the fetch.
                    const auto twin = m_border_twin_vars.find(sampler.index0);
                    if (twin != m_border_twin_vars.end()) {
                        u32 pointer = twin->second;
                        const ResourceRange* range = find_range(
                            DxbcRegisterClass::Sampler, sampler.index0);
                        if (range != nullptr && range->is_array) {
                            u32 element = m_const_u0;
                            if (const auto rec =
                                    m_sampler_record_index.find(sampler.index0);
                                rec != m_sampler_record_index.end()) {
                                element = m_builder.emit_isub(
                                    m_type_uint, rec->second,
                                    m_builder.emit_constant_int(
                                        m_type_uint, range->resolved.binding));
                            }
                            pointer = m_builder.emit_access_chain(
                                m_type_ptr_uniform_constant_sampler,
                                twin->second, {element});
                        }
                        sample.twin_sampler_id =
                            m_builder.emit_load(m_type_sampler, pointer);
                    }
                    sample.border_colour = dynamic_border_colour;
                }
                return apply_swizzle(emit_minmax_sample_call(sample, image_type),
                                     resource.swizzle);
            };
            produced = choose_vec4(
                reduce_flag,
                [&] {
                    return choose_vec4(
                        custom_border, [&] { return reduced(true); },
                        [&] { return reduced(false); });
                },
                emit_ordinary);
        } else {
            produced = emit_ordinary();
        }
        if (failed) return false;
        if (!store_vec4(dst, produced)) return false;
        if (m_tiled.has_status) {
            // The same access of the residency companion through s#'s
            // status twin (DxbcTranslationOptions::tiled_status).
            u32 sampled_type = 0;
            bool multisampled = false;
            const u32 companion =
                tiled_companion(resource, false, sampled_type, multisampled);
            if (companion == 0) return false;
            if (multisampled || sampled_type == 0) {
                return fail("tiled status: a multisampled texture is not sampled");
            }
            const u32 status_sampler = tiled_status_sampler(sampler);
            if (status_sampler == 0) return false;
            TiledStatus::Lod lod{};
            if (gradients) {
                lod.kind = TiledStatus::Lod::Kind::Gradient;
                lod.grad_x = gradient_x;
                lod.grad_y = gradient_y;
            } else if (opcode == Sm4Opcode::SAMPLE_L ||
                       opcode == Sm4Opcode::SAMPLE_C_LZ) {
                lod.kind = TiledStatus::Lod::Kind::Explicit;
                lod.lod = lod_scalar;
            } else {
                // sample, sample_b, sample_c: the bias the sample took.
                lod.bias = opcode == Sm4Opcode::SAMPLE_C ? sampler_lod_bias
                                                         : lod_scalar;
            }
            const u32 miss = TiledStatus::emit_texture_status(
                m_builder, tiled_status_types(),
                m_builder.emit_sampled_image(sampled_type, companion,
                                             status_sampler),
                coordinate, lod, immediate_offset, programmable, min_lod,
                gather);
            if (!store_tiled_status(miss)) return false;
        }
        break;
    }

    default:
        return fail("Unsupported opcode: " +
                    std::to_string(static_cast<u32>(opcode)));
    }

    return true;
}

} // namespace vitreo::graphics::dxbc_spirv_detail
