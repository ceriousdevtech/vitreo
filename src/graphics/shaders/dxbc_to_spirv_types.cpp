// SPDX-License-Identifier: LGPL-2.1-only

/// @file dxbc_to_spirv_types.cpp
/// @brief SPIR-V type declarations and the temp-register helpers.

#include "graphics/shaders/dxbc_to_spirv_internal.hpp"

namespace vitreo::graphics::dxbc_spirv_detail {

bool DxbcTranslator::emit_types() {
    m_type_void = m_builder.emit_type_void();
    m_type_bool = m_builder.emit_type_bool();
    m_type_bvec2 = m_builder.emit_type_vector(m_type_bool, 2);
    m_type_bvec3 = m_builder.emit_type_vector(m_type_bool, 3);
    m_type_bvec4 = m_builder.emit_type_vector(m_type_bool, 4);
    m_type_float = m_builder.emit_type_float(32);
    m_type_vec2 = m_builder.emit_type_vector(m_type_float, 2);
    m_type_vec3 = m_builder.emit_type_vector(m_type_float, 3);
    m_type_vec4 = m_builder.emit_type_vector(m_type_float, 4);
    m_type_uint = m_builder.emit_type_int(32, false);
    m_type_uvec2 = m_builder.emit_type_vector(m_type_uint, 2);
    m_type_uvec3 = m_builder.emit_type_vector(m_type_uint, 3);
    m_type_uvec4 = m_builder.emit_type_vector(m_type_uint, 4);
    m_type_int = m_builder.emit_type_int(32, true);
    m_type_ivec2 = m_builder.emit_type_vector(m_type_int, 2);
    m_type_ivec3 = m_builder.emit_type_vector(m_type_int, 3);
    m_type_ivec4 = m_builder.emit_type_vector(m_type_int, 4);
    m_type_smul_result =
        m_builder.emit_type_struct({m_type_ivec4, m_type_ivec4});
    m_type_umul_result =
        m_builder.emit_type_struct({m_type_uvec4, m_type_uvec4});
    m_builder.set_cached_uint_type(m_type_uint);

    // D3D11 sampler MipLODBias is dynamic state, while Metal expresses bias
    // on each implicit sample operation. A compact push-constant array keeps
    // the translated shader generic across sampler-state changes.
    //
    // The same block carries the border colours of s0..s7 for the border-twin
    // emulation (member 1, at byte 64), so its layout is fixed at 192 bytes
    // whether or not a shader uses them: a frontend fills one struct.
    const u32 bias_count = m_builder.emit_constant_int(m_type_uint, 16u);
    const u32 bias_array = m_builder.emit_type_array(m_type_float, bias_count);
    m_builder.emit_decorate(bias_array, SpvDecoration::ArrayStride, 4);
    const u32 border_count = m_builder.emit_constant_int(m_type_uint, 8u);
    const u32 border_array =
        m_builder.emit_type_array(m_type_vec4, border_count);
    m_builder.emit_decorate(border_array, SpvDecoration::ArrayStride, 16);
    // Member 2 (byte 192): the bound raw/structured buffer view sizes in
    // 32-bit words, one per t# slot then one per u# slot, for the
    // out-of-bounds guard (DxbcTranslationOptions::guard_buffer_bounds,
    // kGuardPushBlockBytes in all).
    const u32 size_count = m_builder.emit_constant_int(
        m_type_uint, kGuardSizeSrvSlots + kGuardSizeUavSlots);
    const u32 size_array = m_builder.emit_type_array(m_type_uint, size_count);
    m_builder.emit_decorate(size_array, SpvDecoration::ArrayStride, 4);
    // Member 12: the cb# readable element counts (kConstantBufferLimitOffset,
    // DxbcTranslationOptions::guard_constant_buffers).
    const u32 cb_limit_array = m_builder.emit_type_array(
        m_type_uint,
        m_builder.emit_constant_int(m_type_uint, kConstantBufferLimitSlots));
    m_builder.emit_decorate(cb_limit_array, SpvDecoration::ArrayStride, 4);
    // Member 13: the MINIMUM_ / MAXIMUM_ samplers' state
    // (kMinMaxSamplerOffset, DxbcTranslationOptions::minmax_samplers).
    const u32 minmax_array = m_builder.emit_type_array(
        m_type_uvec4,
        m_builder.emit_constant_int(m_type_uint, kMinMaxSamplerSlots));
    m_builder.emit_decorate(minmax_array, SpvDecoration::ArrayStride, 16);
    const u32 bias_struct = m_builder.emit_type_struct(
        {bias_array, border_array, size_array, m_type_uint, m_type_uint,
         m_type_uint, m_type_uint, m_type_float, m_type_uint, m_type_float,
         m_type_float, border_array, cb_limit_array, minmax_array});
    m_builder.emit_decorate(bias_struct, SpvDecoration::Block);
    m_builder.emit_member_decorate(
        bias_struct, 0, SpvDecoration::Offset, 0);
    m_builder.emit_member_decorate(
        bias_struct, 1, SpvDecoration::Offset, 64);
    m_builder.emit_member_decorate(
        bias_struct, 2, SpvDecoration::Offset, kGuardSizeTableOffset);
    // Member 3: the vertex-capture SV_InstanceID bias (kInstanceIdBiasOffset).
    m_builder.emit_member_decorate(
        bias_struct, 3, SpvDecoration::Offset, kInstanceIdBiasOffset);
    // Member 4: the s# point-mip mask (kPointMipMaskOffset).
    m_builder.emit_member_decorate(
        bias_struct, 4, SpvDecoration::Offset, kPointMipMaskOffset);
    // Member 5: the rasterizer sample count (kRasterizerSampleCountOffset).
    m_builder.emit_member_decorate(
        bias_struct, 5, SpvDecoration::Offset, kRasterizerSampleCountOffset);
    // Member 6: the s# point-mip tie-nudge mask (kPointMipNudgeMaskOffset).
    m_builder.emit_member_decorate(bias_struct, kPushMemberPointMipNudgeMask,
                                   SpvDecoration::Offset,
                                   kPointMipNudgeMaskOffset);
    // Members 7-10: depth offset, SampleMask, viewport depth range
    // (kFixedFunctionOffset).
    for (u32 member = 0; member < 4; ++member) {
        m_builder.emit_member_decorate(bias_struct, kPushMemberDepthOffset + member,
                                       SpvDecoration::Offset,
                                       kFixedFunctionOffset + member * 4);
    }
    // Member 11: the border colours of s8..s15 (kBorderHighOffset).
    m_builder.emit_member_decorate(bias_struct, kPushMemberBorderHigh,
                                   SpvDecoration::Offset, kBorderHighOffset);
    m_builder.emit_member_decorate(bias_struct,
                                   kPushMemberConstantBufferLimits,
                                   SpvDecoration::Offset,
                                   kConstantBufferLimitOffset);
    m_builder.emit_member_decorate(bias_struct, kPushMemberMinMaxSamplers,
                                   SpvDecoration::Offset,
                                   kMinMaxSamplerOffset);
    m_type_ptr_push_vec4 =
        m_builder.emit_type_pointer(SpvStorageClass::PushConstant, m_type_vec4);
    m_type_ptr_push_float = m_builder.emit_type_pointer(
        SpvStorageClass::PushConstant, m_type_float);
    m_type_ptr_push_uint = m_builder.emit_type_pointer(
        SpvStorageClass::PushConstant, m_type_uint);
    m_type_ptr_push_sampler_biases = m_builder.emit_type_pointer(
        SpvStorageClass::PushConstant, bias_struct);

    m_type_ptr_uniform_vec4 =
        m_builder.emit_type_pointer(SpvStorageClass::Uniform, m_type_vec4);
    m_type_ptr_input_bool =
        m_builder.emit_type_pointer(SpvStorageClass::Input, m_type_bool);
    m_type_ptr_input_uint =
        m_builder.emit_type_pointer(SpvStorageClass::Input, m_type_uint);
    m_type_ptr_storage_buffer_uint = m_builder.emit_type_pointer(
        SpvStorageClass::StorageBuffer, m_type_uint);
    m_type_ptr_input_uvec3 =
        m_builder.emit_type_pointer(SpvStorageClass::Input, m_type_uvec3);
    m_type_ptr_function_vec4 =
        m_builder.emit_type_pointer(SpvStorageClass::Function, m_type_vec4);
    m_type_ptr_workgroup_uint =
        m_builder.emit_type_pointer(SpvStorageClass::Workgroup, m_type_uint);
    // Image-class pointers to a single texel, for atomics on a typed UAV.
    // The pointee is the image's own sampled type, so a uint UAV and an int
    // UAV need one each.
    m_type_ptr_image_uint =
        m_builder.emit_type_pointer(SpvStorageClass::Image, m_type_uint);
    m_type_ptr_image_int =
        m_builder.emit_type_pointer(SpvStorageClass::Image, m_type_int);

    m_type_image_2d =
        m_builder.emit_type_image(m_type_float, SpvDim::Dim2D);
    m_type_image_2d_array =
        m_builder.emit_type_image(m_type_float, SpvDim::Dim2D, true);
    m_type_image_3d =
        m_builder.emit_type_image(m_type_float, SpvDim::Dim3D);
    m_type_image_cube =
        m_builder.emit_type_image(m_type_float, SpvDim::DimCube);
    m_type_image_cube_array =
        m_builder.emit_type_image(m_type_float, SpvDim::DimCube, true);
    m_type_uimage_2d = m_builder.emit_type_image(m_type_uint, SpvDim::Dim2D);
    m_type_uimage_2d_array =
        m_builder.emit_type_image(m_type_uint, SpvDim::Dim2D, true);
    m_type_uimage_3d = m_builder.emit_type_image(m_type_uint, SpvDim::Dim3D);
    m_type_iimage_2d = m_builder.emit_type_image(m_type_int, SpvDim::Dim2D);
    m_type_iimage_2d_array =
        m_builder.emit_type_image(m_type_int, SpvDim::Dim2D, true);
    m_type_iimage_3d = m_builder.emit_type_image(m_type_int, SpvDim::Dim3D);
    m_type_ms_image_2d =
        m_builder.emit_type_image(m_type_float, SpvDim::Dim2D, false, true);
    m_type_ms_image_2d_array =
        m_builder.emit_type_image(m_type_float, SpvDim::Dim2D, true, true);
    m_type_ms_uimage_2d =
        m_builder.emit_type_image(m_type_uint, SpvDim::Dim2D, false, true);
    m_type_ms_uimage_2d_array =
        m_builder.emit_type_image(m_type_uint, SpvDim::Dim2D, true, true);
    m_type_ms_iimage_2d =
        m_builder.emit_type_image(m_type_int, SpvDim::Dim2D, false, true);
    m_type_ms_iimage_2d_array =
        m_builder.emit_type_image(m_type_int, SpvDim::Dim2D, true, true);
    // A D3D typed buffer is a sampled image with Dim=Buffer: unmipped,
    // unfiltered, addressed by one element index, and format-decoded by the
    // hardware from the *view's* DXGI format. That is what lets the shader
    // stay generic over a format it cannot see.
    m_type_buffer_image_float =
        m_builder.emit_type_image(m_type_float, SpvDim::DimBuffer, false,
                                  false, 1u, 0u, 0u);
    m_type_buffer_image_uint =
        m_builder.emit_type_image(m_type_uint, SpvDim::DimBuffer, false,
                                  false, 1u, 0u, 0u);
    m_type_buffer_image_int =
        m_builder.emit_type_image(m_type_int, SpvDim::DimBuffer, false,
                                  false, 1u, 0u, 0u);
    // A typed UAV declaration carries a component type but not the DXGI view
    // format. SPIR-V therefore uses a storage image with Unknown format and
    // the narrowly required write-without-format capability.
    m_type_storage_image_2d_float = m_builder.emit_type_image(
        m_type_float, SpvDim::Dim2D, false, false,
        /*Sampled=storage image*/ 2u, /*ImageFormat Unknown*/ 0u);
    m_type_storage_image_2d_uint = m_builder.emit_type_image(
        m_type_uint, SpvDim::Dim2D, false, false, 2u, 0u);
    m_type_storage_image_2d_int = m_builder.emit_type_image(
        m_type_int, SpvDim::Dim2D, false, false, 2u, 0u);
    // RWTexture3D<T>: the same storage image shape over a volume, which
    // Metal binds as texture3d<T, access::read_write>.
    // RWTexture2DArray<T>: an arrayed Dim2D storage image. Metal binds it as
    // texture2d_array<T, access::read_write>.
    m_type_storage_image_2d_array_float = m_builder.emit_type_image(
        m_type_float, SpvDim::Dim2D, /*arrayed=*/true, false, 2u, 0u);
    m_type_storage_image_2d_array_uint = m_builder.emit_type_image(
        m_type_uint, SpvDim::Dim2D, /*arrayed=*/true, false, 2u, 0u);
    m_type_storage_image_2d_array_int = m_builder.emit_type_image(
        m_type_int, SpvDim::Dim2D, /*arrayed=*/true, false, 2u, 0u);
    m_type_storage_image_3d_float = m_builder.emit_type_image(
        m_type_float, SpvDim::Dim3D, false, false,
        /*Sampled=storage image*/ 2u, /*ImageFormat Unknown*/ 0u);
    m_type_storage_image_3d_uint = m_builder.emit_type_image(
        m_type_uint, SpvDim::Dim3D, false, false, 2u, 0u);
    m_type_storage_image_3d_int = m_builder.emit_type_image(
        m_type_int, SpvDim::Dim3D, false, false, 2u, 0u);
    // RWBuffer<T>: the writable counterpart of the typed buffer SRV. Same
    // Dim=Buffer addressing by one element index, but a storage image rather
    // than a sampled one, which is Metal's texture_buffer<T, read_write>.
    m_type_storage_buffer_image_float = m_builder.emit_type_image(
        m_type_float, SpvDim::DimBuffer, false, false,
        /*Sampled=storage image*/ 2u, /*ImageFormat Unknown*/ 0u);
    // Learn (DXGI_FORMAT / resource return type): a _UINT or _SINT view holds
    // raw integers, not normalised values, and the image's sampled type has to
    // say so — a distinct family, never an alias for the float one.
    m_type_storage_buffer_image_uint = m_builder.emit_type_image(
        m_type_uint, SpvDim::DimBuffer, false, false, 2u, 0u);
    m_type_storage_buffer_image_int = m_builder.emit_type_image(
        m_type_int, SpvDim::DimBuffer, false, false, 2u, 0u);
    m_type_sampler = m_builder.emit_type_sampler();
    m_type_sampled_image_2d =
        m_builder.emit_type_sampled_image(m_type_image_2d);
    m_type_sampled_image_2d_array =
        m_builder.emit_type_sampled_image(m_type_image_2d_array);
    m_type_sampled_uimage_2d =
        m_builder.emit_type_sampled_image(m_type_uimage_2d);
    m_type_sampled_uimage_2d_array =
        m_builder.emit_type_sampled_image(m_type_uimage_2d_array);
    m_type_sampled_iimage_2d =
        m_builder.emit_type_sampled_image(m_type_iimage_2d);
    m_type_sampled_iimage_2d_array =
        m_builder.emit_type_sampled_image(m_type_iimage_2d_array);
    m_type_sampled_image_3d =
        m_builder.emit_type_sampled_image(m_type_image_3d);
    m_type_sampled_image_cube =
        m_builder.emit_type_sampled_image(m_type_image_cube);
    m_type_sampled_image_cube_array =
        m_builder.emit_type_sampled_image(m_type_image_cube_array);
    m_type_ptr_uniform_constant_buffer_image_float =
        m_builder.emit_type_pointer(SpvStorageClass::UniformConstant,
                                    m_type_buffer_image_float);
    m_type_ptr_uniform_constant_buffer_image_uint =
        m_builder.emit_type_pointer(SpvStorageClass::UniformConstant,
                                    m_type_buffer_image_uint);
    m_type_ptr_uniform_constant_buffer_image_int =
        m_builder.emit_type_pointer(SpvStorageClass::UniformConstant,
                                    m_type_buffer_image_int);
    m_type_ptr_uniform_constant_image_2d = m_builder.emit_type_pointer(
        SpvStorageClass::UniformConstant, m_type_image_2d);
    m_type_ptr_uniform_constant_image_2d_array = m_builder.emit_type_pointer(
        SpvStorageClass::UniformConstant, m_type_image_2d_array);
    m_type_ptr_uniform_constant_image_3d = m_builder.emit_type_pointer(
        SpvStorageClass::UniformConstant, m_type_image_3d);
    m_type_ptr_uniform_constant_image_cube = m_builder.emit_type_pointer(
        SpvStorageClass::UniformConstant, m_type_image_cube);
    m_type_ptr_uniform_constant_image_cube_array = m_builder.emit_type_pointer(
        SpvStorageClass::UniformConstant, m_type_image_cube_array);
    m_type_ptr_uniform_constant_uimage_2d = m_builder.emit_type_pointer(
        SpvStorageClass::UniformConstant, m_type_uimage_2d);
    m_type_ptr_uniform_constant_uimage_2d_array = m_builder.emit_type_pointer(
        SpvStorageClass::UniformConstant, m_type_uimage_2d_array);
    m_type_ptr_uniform_constant_uimage_3d = m_builder.emit_type_pointer(
        SpvStorageClass::UniformConstant, m_type_uimage_3d);
    m_type_ptr_uniform_constant_iimage_2d = m_builder.emit_type_pointer(
        SpvStorageClass::UniformConstant, m_type_iimage_2d);
    m_type_ptr_uniform_constant_iimage_2d_array = m_builder.emit_type_pointer(
        SpvStorageClass::UniformConstant, m_type_iimage_2d_array);
    m_type_ptr_uniform_constant_iimage_3d = m_builder.emit_type_pointer(
        SpvStorageClass::UniformConstant, m_type_iimage_3d);
    m_type_ptr_uniform_constant_ms_image_2d = m_builder.emit_type_pointer(
        SpvStorageClass::UniformConstant, m_type_ms_image_2d);
    m_type_ptr_uniform_constant_ms_image_2d_array =
        m_builder.emit_type_pointer(
            SpvStorageClass::UniformConstant, m_type_ms_image_2d_array);
    m_type_ptr_uniform_constant_ms_uimage_2d = m_builder.emit_type_pointer(
        SpvStorageClass::UniformConstant, m_type_ms_uimage_2d);
    m_type_ptr_uniform_constant_ms_uimage_2d_array =
        m_builder.emit_type_pointer(
            SpvStorageClass::UniformConstant, m_type_ms_uimage_2d_array);
    m_type_ptr_uniform_constant_ms_iimage_2d = m_builder.emit_type_pointer(
        SpvStorageClass::UniformConstant, m_type_ms_iimage_2d);
    m_type_ptr_uniform_constant_ms_iimage_2d_array =
        m_builder.emit_type_pointer(
            SpvStorageClass::UniformConstant, m_type_ms_iimage_2d_array);
    m_type_ptr_uniform_constant_storage_image_2d_float =
        m_builder.emit_type_pointer(
            SpvStorageClass::UniformConstant,
            m_type_storage_image_2d_float);
    m_type_ptr_uniform_constant_storage_image_2d_uint =
        m_builder.emit_type_pointer(
            SpvStorageClass::UniformConstant,
            m_type_storage_image_2d_uint);
    m_type_ptr_uniform_constant_storage_image_2d_int =
        m_builder.emit_type_pointer(
            SpvStorageClass::UniformConstant,
            m_type_storage_image_2d_int);
    m_type_ptr_uniform_constant_storage_image_2d_array_float =
        m_builder.emit_type_pointer(
            SpvStorageClass::UniformConstant,
            m_type_storage_image_2d_array_float);
    m_type_ptr_uniform_constant_storage_image_2d_array_uint =
        m_builder.emit_type_pointer(
            SpvStorageClass::UniformConstant,
            m_type_storage_image_2d_array_uint);
    m_type_ptr_uniform_constant_storage_image_2d_array_int =
        m_builder.emit_type_pointer(
            SpvStorageClass::UniformConstant,
            m_type_storage_image_2d_array_int);
    m_type_ptr_uniform_constant_storage_image_3d_float =
        m_builder.emit_type_pointer(
            SpvStorageClass::UniformConstant,
            m_type_storage_image_3d_float);
    m_type_ptr_uniform_constant_storage_image_3d_uint =
        m_builder.emit_type_pointer(
            SpvStorageClass::UniformConstant,
            m_type_storage_image_3d_uint);
    m_type_ptr_uniform_constant_storage_image_3d_int =
        m_builder.emit_type_pointer(
            SpvStorageClass::UniformConstant,
            m_type_storage_image_3d_int);
    m_type_ptr_uniform_constant_storage_buffer_image_float =
        m_builder.emit_type_pointer(
            SpvStorageClass::UniformConstant,
            m_type_storage_buffer_image_float);
    m_type_ptr_uniform_constant_storage_buffer_image_uint =
        m_builder.emit_type_pointer(
            SpvStorageClass::UniformConstant,
            m_type_storage_buffer_image_uint);
    m_type_ptr_uniform_constant_storage_buffer_image_int =
        m_builder.emit_type_pointer(
            SpvStorageClass::UniformConstant,
            m_type_storage_buffer_image_int);
    m_type_ptr_uniform_constant_sampler = m_builder.emit_type_pointer(
        SpvStorageClass::UniformConstant, m_type_sampler);

    m_func_type = m_builder.emit_type_function(m_type_void);

    m_const_u0 = m_builder.emit_constant_int(m_type_uint, 0);
    m_const_u_ffff = m_builder.emit_constant_int(m_type_uint, 0xFFFFFFFFu);
    m_const_u_zero4 = m_builder.emit_constant_composite(
        m_type_uvec4,
        {m_const_u0, m_const_u0, m_const_u0, m_const_u0});
    m_const_u_ffff4 = m_builder.emit_constant_composite(
        m_type_uvec4,
        {m_const_u_ffff, m_const_u_ffff, m_const_u_ffff, m_const_u_ffff});

    return true;
}

u32 DxbcTranslator::get_temp_variable(u32 index) {
    auto it = m_temp_vars.find(index);
    if (it != m_temp_vars.end()) {
        return it->second;
    }

    u32 ptr_type = temp_pointer_type();
    u32 var_id = m_builder.emit_variable(ptr_type, temp_storage_class());
    m_builder.emit_name(var_id, "r" + std::to_string(index));
    m_temp_vars[index] = var_id;
    m_allocated_temps.insert(index);
    return var_id;
}

u32 DxbcTranslator::temp_pointer_type() {
    if (!m_temps_private) return m_type_ptr_function_vec4;
    if (m_type_ptr_private_vec4 == 0) {
        m_type_ptr_private_vec4 =
            m_builder.emit_type_pointer(SpvStorageClass::Private, m_type_vec4);
    }
    return m_type_ptr_private_vec4;
}

u32 DxbcTranslator::get_float_type(u32 components) {
    switch (components) {
    case 1: return m_type_float;
    case 2: return m_type_vec2;
    case 3: return m_type_vec3;
    case 4: return m_type_vec4;
    default: return m_type_vec4;
    }
}

} // namespace vitreo::graphics::dxbc_spirv_detail
