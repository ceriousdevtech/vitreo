// SPDX-License-Identifier: LGPL-2.1-only

/// @file dxbc_to_spirv_resources.cpp
/// @brief Resource variables: cb#, t#, s#, u# and g# declarations.

#include "graphics/shaders/dxbc_to_spirv_internal.hpp"

namespace vitreo::graphics::dxbc_spirv_detail {

bool DxbcTranslator::emit_resource_variables() {

    // RDEF is optional: D3DStripShader may remove reflection data while
    // leaving executable bytecode valid. DCL_CONSTANT_BUFFER remains in SHDR
    // and is the authoritative declaration for binding slot and vec4 extent.
    // Every declared binding's descriptor set and binding (the register
    // remap), and the refusal of the D3D11-register options a descriptor-
    // table translation cannot honour.
    if (!resolve_bindings()) return false;
    std::map<u32, u32> declared_cbuffers = scan_constant_buffers();
    auto [declared_textures, declared_return_tokens, declared_samplers] =
        scan_shader_resources();
    if (!add_class_linkage_declarations(declared_cbuffers, declared_textures,
                                        declared_return_tokens,
                                        declared_samplers)) {
        return false;
    }
    // StructuredBuffer SRVs. A t# register can name either a texture or a
    // structured buffer; a buffer is a SPIR-V storage buffer, not an image.
    // Microsoft Learn (StructuredBuffer): the element type is opaque to the
    // shader ISA — ld_structured addresses it as element index * stride +
    // byte offset — so a runtime array of 32-bit words is the exact model.
    // Descriptor set 1 is the t# namespace, shared with textures; the two do
    // not collide because a slot is one or the other.
    const auto emit_buffer_srv_variable = [&](u32 slot, const char* suffix) {
        const u32 words_ty = m_builder.emit_type_runtime_array(m_type_uint);
        m_builder.emit_decorate(words_ty, SpvDecoration::ArrayStride, 4);
        const u32 struct_ty = m_builder.emit_type_struct({words_ty});
        m_builder.emit_decorate(struct_ty, SpvDecoration::Block);
        m_builder.emit_member_decorate(struct_ty, 0, SpvDecoration::Offset, 0);
        // An SRV is read-only by definition; saying so lets the backend place
        // it in a read-only binding and keeps SPIRV-Cross from emitting a
        // writable Metal pointer.
        m_builder.emit_member_decorate(struct_ty, 0,
                                       SpvDecoration::NonWritable);
        const u32 ptr_ty = m_builder.emit_type_pointer(
            SpvStorageClass::StorageBuffer, struct_ty);
        return declare_resource(DxbcRegisterClass::ShaderResource, slot,
                                ptr_ty, SpvStorageClass::StorageBuffer,
                                DescriptorKind::StorageBuffer,
                                "t" + std::to_string(slot) + suffix);
    };
    for (const auto& [slot, stride] : scan_structured_srvs()) {
        const u32 var_id = emit_buffer_srv_variable(slot, "_structured");
        if (var_id == 0) return false;
        m_structured_srv_vars[slot] = var_id;
        m_structured_srv_strides[slot] = stride;
        // The t# high-water is what sizes the descriptor table. A buffer that
        // is emitted here but omitted from that list leaves a hole, and
        // argument-buffer padding aborts on the missing index.
        m_result.texture_bindings.push_back(
            {"t" + std::to_string(slot), 5, slot, 1, 0, 0});
    }
    // Learn (ByteAddressBuffer): a raw buffer has no element type and no
    // format — it is addressed by byte offset and read as 32-bit words, which
    // is precisely a storage buffer of words. Nothing needs decoding, so
    // unlike a typed buffer it is not a texture.
    for (const u32 slot : scan_raw_srvs()) {
        if (m_structured_srv_vars.contains(slot)) continue;
        const u32 var_id = emit_buffer_srv_variable(slot, "_raw");
        if (var_id == 0) return false;
        m_raw_srv_vars[slot] = var_id;
        m_result.texture_bindings.push_back(
            {"t" + std::to_string(slot), 7, slot, 1, 0, 0});
    }
    // Typed buffer SRVs (`Buffer<T>`, dcl_resource_buffer / RDEF dimension 1)
    // are *not* storage buffers. Learn (Buffer): one element is one texel of
    // the view's DXGI format, which the shader cannot see — the declaration
    // records only the scalar return type. Modelling it as a sampled image
    // with Dim=Buffer hands the element size and the format decode to the
    // hardware, which is what Metal's `texture_buffer<T>` does natively.
    const auto emit_typed_buffer_srv = [&](u32 slot, u32 return_type) {
        if (m_typed_buffer_srv_vars.contains(slot)) return true;
        m_builder.emit_capability(SpvCapability::SampledBuffer);
        const u32 ptr_type = return_type == 4u
            ? m_type_ptr_uniform_constant_buffer_image_uint
            : return_type == 3u
                ? m_type_ptr_uniform_constant_buffer_image_int
                : m_type_ptr_uniform_constant_buffer_image_float;
        const u32 var_id = declare_resource(
            DxbcRegisterClass::ShaderResource, slot, ptr_type,
            SpvStorageClass::UniformConstant,
            DescriptorKind::UniformTexelBuffer,
            "t" + std::to_string(slot) + "_buffer");
        if (var_id == 0) return false;
        m_typed_buffer_srv_vars[slot] = var_id;
        m_typed_buffer_srv_return_types[slot] = return_type;
        m_result.texture_bindings.push_back(
            {"t" + std::to_string(slot), 2, slot, 1, return_type, 1});
        return true;
    };

    const auto declared_uavs = scan_typed_uavs();
    const auto declared_tgsm = scan_raw_tgsm();
    // Learn (dcl_uav_typed / dcl_uav_raw / dcl_uav_structured, sm5 asm): a
    // UAV declaration applies to the pixel and compute stages; every stage
    // only from the Direct3D 11.1 runtime at feature level 11_1. Vitreo's
    // devices stop at 11_0, where CreateVertexShader of a vs_5_0 that writes
    // a UAV returns E_INVALIDARG (measured:
    // tools/oracle_probe/d3d11_stage_uav_probe.expected). The authored stage
    // decides, not m_result.stage: a geometry or hull shader lowered to a
    // kernel reads back as Compute.
    const auto uav_declarations = scan_uav_declaration_tokens();
    if (!uav_declarations.empty() &&
        m_container.stage != ShaderStage::Fragment &&
        m_container.stage != ShaderStage::Compute) {
        return fail("u" + std::to_string(uav_declarations.begin()->first) +
                    " declared outside the pixel and compute stages; feature "
                    "level 11_0 has no UAVs there");
    }
    const auto emit_texture = [&](u32 slot, SpvDim dimension, bool arrayed,
                                  u32 return_type, bool multisampled = false) {
        // A Texture2DMS(Array) bound to a one-sample view is declared as the
        // 2D image Metal can bind (single_sample_ms_srvs); it stays in
        // m_texture_multisampled so ld2dms, resinfo and sampleinfo accept it.
        const bool single_sample = multisampled && slot < 32u &&
            ((m_options.single_sample_ms_srvs >> slot) & 1u) != 0;
        if (single_sample) {
            multisampled = false;
            m_texture_single_sample_ms.insert(slot);
            m_texture_multisampled.insert(slot);
        }
        const bool uint_result = return_type == 4;
        const bool sint_result = return_type == 3;
        const u32 ptr_type = multisampled && uint_result
            ? (arrayed ? m_type_ptr_uniform_constant_ms_uimage_2d_array
                       : m_type_ptr_uniform_constant_ms_uimage_2d)
            : multisampled && sint_result
            ? (arrayed ? m_type_ptr_uniform_constant_ms_iimage_2d_array
                       : m_type_ptr_uniform_constant_ms_iimage_2d)
            : multisampled
            ? (arrayed ? m_type_ptr_uniform_constant_ms_image_2d_array
                       : m_type_ptr_uniform_constant_ms_image_2d)
            : uint_result
            ? (arrayed ? m_type_ptr_uniform_constant_uimage_2d_array
                       : dimension == SpvDim::Dim3D
                           ? m_type_ptr_uniform_constant_uimage_3d
                           : m_type_ptr_uniform_constant_uimage_2d)
            : sint_result
            ? (arrayed ? m_type_ptr_uniform_constant_iimage_2d_array
                       : dimension == SpvDim::Dim3D
                           ? m_type_ptr_uniform_constant_iimage_3d
                           : m_type_ptr_uniform_constant_iimage_2d)
            : arrayed && dimension == SpvDim::DimCube
            ? m_type_ptr_uniform_constant_image_cube_array
            : arrayed
            ? m_type_ptr_uniform_constant_image_2d_array
            : dimension == SpvDim::DimCube
            ? m_type_ptr_uniform_constant_image_cube
            : dimension == SpvDim::Dim3D
                ? m_type_ptr_uniform_constant_image_3d
                : m_type_ptr_uniform_constant_image_2d;
        const u32 var_id = declare_resource(
            DxbcRegisterClass::ShaderResource, slot, ptr_type,
            SpvStorageClass::UniformConstant, DescriptorKind::SampledImage,
            "t" + std::to_string(slot));
        if (var_id == 0) return false;
        m_texture_vars[slot] = var_id;
        m_texture_dimensions[slot] = dimension;
        m_texture_return_types[slot] = return_type;
        if (arrayed) m_texture_arrays.insert(slot);
        if (multisampled) m_texture_multisampled.insert(slot);
        return true;
    };
    const auto emit_cbuffer = [&](const DxbcResourceBinding& rb, u32 nvec) {
        nvec = std::max(1u, nvec);
        const u32 len_id = m_builder.emit_constant_int(m_type_uint, nvec);
        const u32 arr_ty = m_builder.emit_type_array(m_type_vec4, len_id);
        m_builder.emit_decorate(arr_ty, SpvDecoration::ArrayStride, 16);
        const u32 struct_ty = m_builder.emit_type_struct({arr_ty});
        m_builder.emit_decorate(struct_ty, SpvDecoration::Block);
        m_builder.emit_member_decorate(struct_ty, 0, SpvDecoration::Offset, 0);
        const u32 ptr_ty =
            m_builder.emit_type_pointer(SpvStorageClass::Uniform, struct_ty);
        const u32 var_id = declare_resource(
            DxbcRegisterClass::ConstantBuffer, rb.bind_point, ptr_ty,
            SpvStorageClass::Uniform, DescriptorKind::UniformBuffer,
            "cb" + std::to_string(rb.bind_point));
        if (var_id == 0) return false;
        m_cbuffer_vars[rb.bind_point] = var_id;
        m_cbuffer_sizes[rb.bind_point] = nvec;
        m_result.cbuffer_bindings.push_back(rb);
        return true;
    };

    // Preserve RDEF names and exact byte sizes when reflection is present.
    // A descriptor-table translation takes its bindings from the
    // declarations alone (an RDEF register is in some space, and 5.1 names
    // resources by range ID), and only the byte sizes from RDEF.
    for (const auto& rb : m_container.resource_bindings) {
        if (descriptor_table_translation()) break;
        if (rb.type != 0) continue;
        u32 nvec = 0;
        for (const auto& cb : m_container.constant_buffers) {
            if (cb.name == rb.name && cb.size_bytes != 0) {
                nvec = std::max(1u, (cb.size_bytes + 15u) / 16u);
                break;
            }
        }
        const auto declaration = declared_cbuffers.find(rb.bind_point);
        if (nvec == 0 && declaration != declared_cbuffers.end()) {
            nvec = declaration->second;
        }
        if (!emit_cbuffer(rb, nvec == 0 ? 16u : nvec)) return false;
    }
    for (const auto& [slot, nvec] : declared_cbuffers) {
        if (m_cbuffer_vars.contains(slot)) continue;
        // fxc may declare a dynamically indexed 5.1 buffer smaller than the
        // cbuffer it reads (CB1[1:4][2] for an 80-byte ConstantBuffer<T>
        // indexed arr[i & 3], measured); the RDEF size bounds it, and
        // without reflection the largest buffer a view can make does
        // (D3D12_REQ_CONSTANT_BUFFER_ELEMENT_COUNT, 4096 in d3d12.h).
        u32 declared = nvec;
        if (descriptor_table_translation()) {
            const u32 reflected = rdef_cbuffer_vectors(slot);
            const ResourceRange* range =
                find_range(DxbcRegisterClass::ConstantBuffer, slot);
            const bool dynamic = range != nullptr &&
                                 range->resolved.declaration.dynamically_indexed;
            declared = reflected != 0u ? std::max(nvec, reflected)
                       : dynamic       ? 4096u
                                       : nvec;
        }
        if (!emit_cbuffer({"cb" + std::to_string(slot), 0, slot, 1, 0, 0},
                          declared)) {
            return false;
        }
    }

    // D3D texture and sampler registers are separate namespaces. Keep them as
    // separate UniformConstant objects so SPIRV-Cross emits Metal texture(t#)
    // and sampler(s#) arguments matching the frontend's direct slot binds.
    for (const auto& rb : m_container.resource_bindings) {
        if (descriptor_table_translation()) break;
        if (rb.type == 2) { // D3D_SIT_TEXTURE
            // D3D_SRV_DIMENSION_BUFFER=1 is a typed buffer, not an image.
            if (rb.dimension == 1) {
                const u32 count = std::max(1u, rb.bind_count);
                for (u32 i = 0; i < count; ++i) {
                    if (!emit_typed_buffer_srv(rb.bind_point + i,
                                               rb.return_type)) {
                        return false;
                    }
                }
                continue;
            }
            // D3D_SRV_DIMENSION_TEXTURE1D=2 and TEXTURE1DARRAY=3 are 2D
            // images of height 1 underneath (m_texture_1d).
            if (rb.dimension == 2 || rb.dimension == 3) {
                const u32 count = std::max(1u, rb.bind_count);
                for (u32 i = 0; i < count; ++i) {
                    if (!emit_texture(rb.bind_point + i, SpvDim::Dim2D,
                                      rb.dimension == 3, rb.return_type)) {
                        return false;
                    }
                    m_texture_1d.insert(rb.bind_point + i);
                }
                m_result.texture_bindings.push_back(rb);
                continue;
            }
            // D3D_SRV_DIMENSION_TEXTURE2D=4, TEXTURE2DARRAY=5,
            // TEXTURE2DMS=6, TEXTURE2DMSARRAY=7, TEXTURE3D=8,
            // TEXTURECUBE=9, TEXTURECUBEARRAY=10.
            if (rb.dimension != 4 && rb.dimension != 5 && rb.dimension != 8 &&
                rb.dimension != 9 && rb.dimension != 10 &&
                rb.dimension != 6 && rb.dimension != 7) {
                return fail("texture dimension " +
                            std::to_string(rb.dimension) +
                            " is not implemented");
            }
            const SpvDim dimension = rb.dimension == 9 || rb.dimension == 10
                ? SpvDim::DimCube : rb.dimension == 8
                    ? SpvDim::Dim3D : SpvDim::Dim2D;
            const u32 count = std::max(1u, rb.bind_count);
            for (u32 i = 0; i < count; ++i) {
                const u32 slot = rb.bind_point + i;
                if (!emit_texture(slot, dimension,
                                  rb.dimension == 5 || rb.dimension == 7 ||
                                      rb.dimension == 10,
                                  rb.return_type,
                                  rb.dimension == 6 || rb.dimension == 7)) {
                    return false;
                }
            }
            m_result.texture_bindings.push_back(rb);
        } else if (rb.type == 3) { // D3D_SIT_SAMPLER
            const u32 count = std::max(1u, rb.bind_count);
            for (u32 i = 0; i < count; ++i) {
                const u32 slot = rb.bind_point + i;
                const u32 var_id = m_builder.emit_variable(
                    m_type_ptr_uniform_constant_sampler,
                    SpvStorageClass::UniformConstant);
                m_builder.emit_name(var_id, "s" + std::to_string(slot));
                m_builder.emit_decorate(
                    var_id, SpvDecoration::DescriptorSet, 2);
                m_builder.emit_decorate(
                    var_id, SpvDecoration::Binding, slot);
                m_sampler_vars[slot] = var_id;
            }
            m_result.sampler_bindings.push_back(rb);
        }
    }
    // Reflection is optional. DCL_RESOURCE and DCL_SAMPLER remain in stripped
    // executable bytecode and define the slots and resource shape needed by
    // SAMPLE. Texture2D and TextureCube declarations select distinct SPIR-V
    // image and coordinate types. Token dimensions 8 and 10 are Texture2DArray
    // and TextureCubeArray; both require SPIR-V Arrayed=1 on their base shape.
    for (const auto& [slot, dimension] : declared_textures) {
        if (m_texture_vars.contains(slot)) continue;
        if (m_typed_buffer_srv_vars.contains(slot)) continue;
        // Token dimension BUFFER=1 declares a typed buffer, which is a storage
        // buffer rather than an image.
        if (dimension == 1) {
            const u32 packed = declared_return_tokens.at(slot);
            const u32 return_type = packed & 0xFu;
            if (return_type < 1 || return_type > 5 ||
                (packed & 0xFFFFu) != return_type * 0x1111u) {
                return fail("heterogeneous or unsupported buffer return type");
            }
            if (!emit_typed_buffer_srv(slot, return_type)) return false;
            continue;
        }
        // Token dimensions: TEXTURE1D=2, TEXTURE2D=3, TEXTURE2DMS=4,
        // TEXTURE3D=5, TEXTURECUBE=6, TEXTURE1DARRAY=7, TEXTURE2DARRAY=8,
        // TEXTURE2DMSARRAY=9, TEXTURECUBEARRAY=10.
        if (dimension != 2 && dimension != 3 && dimension != 5 &&
            dimension != 6 && dimension != 7 && dimension != 8 &&
            dimension != 10 && dimension != 4 && dimension != 9) {
            return fail("texture declaration dimension " +
                        std::to_string(dimension) + " is not implemented");
        }
        const u32 packed_return = declared_return_tokens.at(slot);
        const u32 return_type = packed_return & 0xFu;
        if (return_type < 1 || return_type > 5 ||
            (packed_return & 0xFFFFu) != return_type * 0x1111u) {
            return fail("heterogeneous or unsupported texture return type");
        }
        if (dimension == 2 || dimension == 7) {
            if (!emit_texture(slot, SpvDim::Dim2D, dimension == 7,
                              return_type)) {
                return false;
            }
            m_texture_1d.insert(slot);
            m_result.texture_bindings.push_back(
                {"t" + std::to_string(slot), 2, slot, 1, 5,
                 dimension == 7 ? 3u : 2u});
            continue;
        }
        if (!emit_texture(slot, dimension == 6 || dimension == 10
                                    ? SpvDim::DimCube
                                    : dimension == 5 ? SpvDim::Dim3D
                                                     : SpvDim::Dim2D,
                          dimension == 8 || dimension == 9 || dimension == 10,
                          return_type, dimension == 4 || dimension == 9)) {
            return false;
        }
        m_result.texture_bindings.push_back(
            {"t" + std::to_string(slot), 2, slot, 1, 5,
             dimension == 4 ? 6u : dimension == 9 ? 7u
                                : dimension == 6 ? 9u : dimension == 10 ? 10u
                                : dimension == 5 ? 8u
                                : dimension == 8 ? 5u : 4u});
    }
    for (const u32 slot : declared_samplers) {
        if (m_sampler_vars.contains(slot)) continue;
        const u32 var_id = declare_resource(
            DxbcRegisterClass::Sampler, slot,
            m_type_ptr_uniform_constant_sampler,
            SpvStorageClass::UniformConstant, DescriptorKind::Sampler,
            "s" + std::to_string(slot));
        if (var_id == 0) return false;
        m_sampler_vars[slot] = var_id;
        m_result.sampler_bindings.push_back(
            {"s" + std::to_string(slot), 3, slot, 1, 0, 0});
    }

    // A descriptor-table translation reports bindings, not D3D11 registers,
    // in the three lists (DxbcToSpirvResult::bindings has the declarations).
    if (descriptor_table_translation()) {
        const auto rebind = [&](std::vector<DxbcResourceBinding>& list,
                                DxbcRegisterClass cls) {
            for (auto& entry : list) {
                const ResourceRange* range = find_range(cls, entry.bind_point);
                if (range == nullptr) continue;
                entry.bind_point = range->resolved.binding;
                entry.bind_count = range->resolved.descriptor_count;
                entry.space = range->resolved.declaration.space;
                entry.range_id = range->resolved.declaration.range_id;
            }
        };
        rebind(m_result.cbuffer_bindings, DxbcRegisterClass::ConstantBuffer);
        rebind(m_result.texture_bindings, DxbcRegisterClass::ShaderResource);
        rebind(m_result.sampler_bindings, DxbcRegisterClass::Sampler);
    }

    // DxbcTranslationOptions::sampler_state_buffer: {MipLODBias, flags, 0, 0}
    // per sampler binding, read-only (an SRV-like view of frontend state).
    if (m_options.sampler_state_buffer.enabled) {
        if (!descriptor_table_translation()) {
            return fail("sampler_state_buffer: a D3D11 translation takes its "
                        "sampler state from the push block");
        }
        std::set<u32> sampler_sets;
        for (const auto& [key, range] : m_ranges) {
            if (key.first == DxbcRegisterClass::Sampler) {
                sampler_sets.insert(range.resolved.descriptor_set);
            }
        }
        if (sampler_sets.size() > 1u) {
            return fail("sampler_state_buffer: the samplers land in more than "
                        "one descriptor set, and records are indexed by "
                        "binding");
        }
        if (!m_sampler_vars.empty()) {
            const u32 records = m_builder.emit_type_runtime_array(m_type_uvec4);
            m_builder.emit_decorate(records, SpvDecoration::ArrayStride,
                                    kSamplerStateRecordWords * 4u);
            const u32 block = m_builder.emit_type_struct({records});
            m_builder.emit_decorate(block, SpvDecoration::Block);
            m_builder.emit_member_decorate(block, 0, SpvDecoration::Offset, 0);
            m_builder.emit_member_decorate(block, 0,
                                           SpvDecoration::NonWritable);
            m_sampler_state_var = m_builder.emit_variable(
                m_builder.emit_type_pointer(SpvStorageClass::StorageBuffer,
                                            block),
                SpvStorageClass::StorageBuffer);
            m_builder.emit_name(m_sampler_state_var, "vitreo_sampler_state");
            m_builder.emit_decorate(
                m_sampler_state_var, SpvDecoration::DescriptorSet,
                m_options.sampler_state_buffer.descriptor_set);
            m_builder.emit_decorate(m_sampler_state_var,
                                    SpvDecoration::Binding,
                                    m_options.sampler_state_buffer.binding);
        }
    }

    // Border twins (DxbcTranslationOptions::border_twin_samplers): set 2,
    // binding border_twin_binding_base + n -- Metal sampler 8 + n in the flat
    // model, sampler-table entry 16 + n in a descriptor-table shader. They
    // stay out of sampler_bindings: they are not D3D registers, and counting
    // them would push the shader past the flat s# window.
    for (u32 slot = 0; slot < 16; ++slot) {
        if ((m_options.border_twin_samplers & (1u << slot)) == 0 ||
            !m_sampler_vars.contains(slot)) {
            continue;
        }
        const u32 var_id = m_builder.emit_variable(
            m_type_ptr_uniform_constant_sampler,
            SpvStorageClass::UniformConstant);
        m_builder.emit_name(var_id, "s" + std::to_string(slot) + "_border_twin");
        m_builder.emit_decorate(var_id, SpvDecoration::DescriptorSet, 2);
        m_builder.emit_decorate(var_id, SpvDecoration::Binding,
                                m_options.border_twin_binding_base + slot);
        m_border_twin_vars[slot] = var_id;
    }

    for (const auto& [slot, declaration] : declared_uavs) {
        const auto [dimension, packed_return] = declaration;
        // Windows SDK tokenized-program format (D3D10_SB_RESOURCE_DIMENSION):
        // 1 = Buffer (RWBuffer<T>), 3 = Texture2D, 5 = Texture3D,
        // 8 = Texture2DArray. Return type 5 is FLOAT. Other typed UAV shapes
        // and scalar types are separate storage-image contracts and remain
        // diagnostic.
        //
        // A cube UAV arrives as dimension 8: D3D exposes the six faces as
        // array slices, so `RWTexture2DArray<T>` is what a shader writing a
        // cubemap declares, which is what a convolution into an array target
        // requires.
        // 2 = Texture1D and 7 = Texture1DArray are height-1 2D images, as for
        // SRVs (m_uav_1d).
        const bool buffer_uav = dimension == 1u;
        const bool volume_uav = dimension == 5u;
        const bool array_uav = dimension == 8u || dimension == 7u;
        const bool uav_1d = dimension == 2u || dimension == 7u;
        if (dimension != 3u && !buffer_uav && !volume_uav && !array_uav &&
            !uav_1d) {
            return fail("typed UAV dimension " + std::to_string(dimension) +
                        " is not implemented");
        }
        // The packed token holds one 4-bit return type per component; all
        // four must agree (Windows SDK D3D10_SB_RESOURCE_RETURN_TYPE:
        // 1 = UNORM, 2 = SNORM, 3 = SINT, 4 = UINT, 5 = FLOAT). Learn: UNORM
        // and SNORM views are the float family — the normalisation is the
        // view format's, applied by the texture unit, so the shader reads and
        // writes floats exactly as it does for a FLOAT view. Refusing them
        // failed every RWBuffer<snorm float> the engine's skinning uses.
        const u32 return_type = packed_return & 0xFu;
        const bool float_family =
            return_type == 1u || return_type == 2u || return_type == 5u;
        if ((packed_return & 0xFFFFu) != (return_type * 0x1111u) ||
            (!float_family && return_type != 3u && return_type != 4u)) {
            return fail("typed UAV return type " +
                        std::to_string(packed_return & 0xFFFFu) +
                        " is not implemented");
        }
        m_uav_return_types[slot] = return_type;
        if (volume_uav) m_volume_uav_slots.insert(slot);
        if (array_uav) m_array_uav_slots.insert(slot);
        if (uav_1d) m_uav_1d.insert(slot);
        m_builder.emit_capability(
            SpvCapability::StorageImageReadWithoutFormat);
        m_builder.emit_capability(
            SpvCapability::StorageImageWriteWithoutFormat);
        if (buffer_uav) {
            // Dim=Buffer storage images need their own capability, distinct
            // from the SampledBuffer one a Buffer<T> SRV uses.
            m_builder.emit_capability(SpvCapability::ImageBuffer);
            m_buffer_uav_slots.insert(slot);
        }
        const u32 var_id = declare_resource(
            DxbcRegisterClass::UnorderedAccess, slot,
            volume_uav
                ? return_type == 4u
                    ? m_type_ptr_uniform_constant_storage_image_3d_uint
                    : return_type == 3u
                        ? m_type_ptr_uniform_constant_storage_image_3d_int
                        : m_type_ptr_uniform_constant_storage_image_3d_float
            : array_uav
                ? return_type == 4u
                    ? m_type_ptr_uniform_constant_storage_image_2d_array_uint
                    : return_type == 3u
                        ? m_type_ptr_uniform_constant_storage_image_2d_array_int
                        : m_type_ptr_uniform_constant_storage_image_2d_array_float
            : !buffer_uav
                ? return_type == 4u
                    ? m_type_ptr_uniform_constant_storage_image_2d_uint
                    : return_type == 3u
                        ? m_type_ptr_uniform_constant_storage_image_2d_int
                        : m_type_ptr_uniform_constant_storage_image_2d_float
                : return_type == 4u
                    ? m_type_ptr_uniform_constant_storage_buffer_image_uint
                    : return_type == 3u
                        ? m_type_ptr_uniform_constant_storage_buffer_image_int
                        : m_type_ptr_uniform_constant_storage_buffer_image_float,
            SpvStorageClass::UniformConstant,
            buffer_uav ? DescriptorKind::StorageTexelBuffer
                       : DescriptorKind::StorageImage,
            "u" + std::to_string(slot));
        // UAVs occupy their own D3D register namespace. Set 3 preserves the
        // translator's cb/set0, SRV/set1, sampler/set2 separation.
        if (var_id == 0) return false;
        m_uav_vars[slot] = var_id;
    }

    // RWStructuredBuffer UAVs. Learn (RWStructuredBuffer): the element type is
    // opaque to the shader ISA, exactly as for the read-only form, so the
    // resource is a storage buffer of 32-bit words rather than a storage
    // image. It is writable, so unlike an SRV it carries no NonWritable
    // decoration, and it lives in the u# namespace (descriptor set 3).
    for (const auto& [slot, stride] : scan_structured_uavs()) {
        if (m_uav_vars.contains(slot)) {
            return fail("u" + std::to_string(slot) +
                        " is declared both typed and structured");
        }
        const u32 words_ty = m_builder.emit_type_runtime_array(m_type_uint);
        m_builder.emit_decorate(words_ty, SpvDecoration::ArrayStride, 4);
        const u32 struct_ty = m_builder.emit_type_struct({words_ty});
        m_builder.emit_decorate(struct_ty, SpvDecoration::Block);
        m_builder.emit_member_decorate(struct_ty, 0, SpvDecoration::Offset, 0);
        const u32 ptr_ty = m_builder.emit_type_pointer(
            SpvStorageClass::StorageBuffer, struct_ty);
        const u32 var_id = declare_resource(
            DxbcRegisterClass::UnorderedAccess, slot, ptr_ty,
            SpvStorageClass::StorageBuffer, DescriptorKind::StorageBuffer,
            "u" + std::to_string(slot) + "_structured");
        if (var_id == 0) return false;
        m_structured_uav_vars[slot] = var_id;
        m_structured_uav_strides[slot] = stride;
    }

    // RWByteAddressBuffer UAVs: same word array as the structured form, with
    // no element stride — store_raw/ld_raw address it by byte offset.
    for (const u32 slot : scan_raw_uavs()) {
        if (m_uav_vars.contains(slot) ||
            m_structured_uav_vars.contains(slot)) {
            return fail("u" + std::to_string(slot) +
                        " is declared with more than one UAV shape");
        }
        const u32 words_ty = m_builder.emit_type_runtime_array(m_type_uint);
        m_builder.emit_decorate(words_ty, SpvDecoration::ArrayStride, 4);
        const u32 struct_ty = m_builder.emit_type_struct({words_ty});
        m_builder.emit_decorate(struct_ty, SpvDecoration::Block);
        m_builder.emit_member_decorate(struct_ty, 0, SpvDecoration::Offset, 0);
        const u32 ptr_ty = m_builder.emit_type_pointer(
            SpvStorageClass::StorageBuffer, struct_ty);
        const u32 var_id = declare_resource(
            DxbcRegisterClass::UnorderedAccess, slot, ptr_ty,
            SpvStorageClass::StorageBuffer, DescriptorKind::StorageBuffer,
            "u" + std::to_string(slot) + "_raw");
        if (var_id == 0) return false;
        m_raw_uav_vars[slot] = var_id;
    }

    // Learn (dcl_uav_typed): `_glc` declares the UAV globally coherent; its
    // absence means group coherent in a compute shader and locally coherent
    // in one pixel-shader invocation. Measured with fxc
    // (tools/oracle_probe/pixel_uav_shaders.hlsl, ps_early_coherent): the
    // flag is OpcodeToken0 bit 16 (0x0401189c vs 0x0400189c). SPIR-V's
    // Coherent is the same promise; SPIRV-Cross carries it into MSL as a
    // volatile device pointer, so the compiler neither caches nor merges the
    // accesses another invocation must see.
    constexpr u32 kGloballyCoherent = 1u << 16;
    for (const auto& [slot, token] : uav_declarations) {
        if ((token & kGloballyCoherent) == 0u) continue;
        for (const auto* vars :
             {&m_uav_vars, &m_structured_uav_vars, &m_raw_uav_vars}) {
            const auto found = vars->find(slot);
            if (found != vars->end()) {
                m_builder.emit_decorate(found->second,
                                        SpvDecoration::Coherent);
            }
        }
    }

    // Hidden UAV counters (imm_atomic_alloc / imm_atomic_consume). Declared
    // here, from a scan of the whole program, rather than at first use: the
    // entry point's interface is emitted before the body, and SPIRV-Cross
    // builds the kernel's arguments from that interface -- a counter created
    // mid-body was referenced by the MSL and never declared.
    //
    // Learn: the counter is a structured buffer UAV's, with the Count or
    // Append flag, and lives outside the buffer. Each is its own 4-byte buffer
    // (it belongs to the view, not the resource), bound in first-use order.
    for (const u32 slot : scan_uav_counters()) {
        if (!m_structured_uav_vars.contains(slot)) {
            return fail("ATOMIC: counter on u" + std::to_string(slot) +
                        ", which is not a structured UAV");
        }
        // D3D11.0 allows these in the pixel and compute stages; 11.1 allows
        // every stage, but only those two have a free Metal buffer index. A
        // geometry shader lowered to compute reads as Compute, and its
        // expansion pass binds no counters.
        if ((m_result.stage != ShaderStage::Compute &&
             m_result.stage != ShaderStage::Fragment) ||
            gs_as_compute()) {
            return fail("ATOMIC: UAV counters outside the pixel and compute "
                        "stages are not implemented");
        }
        const u32 binding =
            static_cast<u32>(m_result.uav_counter_registers.size());
        // A 5.1 UAV array moves one counter per element (fxc: imm_atomic_alloc
        // U1[r0.y + 3] on RWStructuredBuffer<uint> a[2]): an array of
        // counters on consecutive bindings, entry k of uav_counter_registers
        // still naming binding k. An unbounded array has no count to give.
        const ResourceRange* range =
            find_range(DxbcRegisterClass::UnorderedAccess, slot);
        const bool counter_array = range != nullptr && range->is_array;
        const u32 counters =
            counter_array ? range->resolved.descriptor_count : 1u;
        if (counters == 0u) {
            return fail("ATOMIC: counter on the unbounded UAV range U" +
                        std::to_string(slot) +
                        " has no fixed number of counter bindings");
        }
        if (binding + counters > SPIRV_MAX_UAV_COUNTERS) {
            return fail("ATOMIC: more than " +
                        std::to_string(SPIRV_MAX_UAV_COUNTERS) +
                        " UAV counters in one shader");
        }
        const u32 struct_ty = m_builder.emit_type_struct({m_type_uint});
        m_builder.emit_decorate(struct_ty, SpvDecoration::Block);
        m_builder.emit_member_decorate(struct_ty, 0, SpvDecoration::Offset, 0);
        const u32 ptr_ty = m_builder.emit_type_pointer(
            SpvStorageClass::StorageBuffer, struct_ty);
        u32 var_type = ptr_ty;
        if (counter_array) {
            m_builder.emit_capability(
                SpvCapability::StorageBufferArrayDynamicIndexing);
            var_type = m_builder.emit_type_pointer(
                SpvStorageClass::StorageBuffer,
                m_builder.emit_type_array(
                    struct_ty,
                    m_builder.emit_constant_int(m_type_uint, counters)));
        }
        const u32 var_id =
            m_builder.emit_variable(var_type, SpvStorageClass::StorageBuffer);
        m_builder.emit_name(var_id, "u" + std::to_string(slot) + "_counter");
        m_builder.emit_decorate(var_id, SpvDecoration::DescriptorSet,
                                SPIRV_UAV_COUNTER_DESCRIPTOR_SET);
        m_builder.emit_decorate(var_id, SpvDecoration::Binding, binding);
        for (u32 k = 0; k < counters; ++k) {
            const DxbcBindingDeclaration* declared =
                range != nullptr ? &range->resolved.declaration : nullptr;
            m_result.uav_counter_registers.push_back(
                declared != nullptr ? declared->lower + k : slot);
            m_result.uav_counter_spaces.push_back(
                declared != nullptr ? declared->space : 0u);
        }
        m_uav_counter_vars[slot] = var_id;
        if (counter_array) m_uav_counter_element_types[slot] = ptr_ty;
    }

    u32 total_tgsm_bytes = 0u;
    for (const auto& [slot, byte_count] : declared_tgsm) {
        if (m_result.stage != ShaderStage::Compute || byte_count == 0u ||
            (byte_count & 3u) != 0u || byte_count > 32768u ||
            total_tgsm_bytes > 32768u - byte_count) {
            return fail("raw TGSM declaration violates the 32 KiB contract");
        }
        total_tgsm_bytes += byte_count;
        const u32 word_count = byte_count / 4u;
        const u32 count = m_builder.emit_constant_int(m_type_uint, word_count);
        const u32 array_type = m_builder.emit_type_array(m_type_uint, count);
        const u32 pointer_type = m_builder.emit_type_pointer(
            SpvStorageClass::Workgroup, array_type);
        const u32 var_id = m_builder.emit_variable(
            pointer_type, SpvStorageClass::Workgroup);
        m_builder.emit_name(var_id, "g" + std::to_string(slot));
        m_tgsm_vars[slot] = var_id;
        m_tgsm_word_counts[slot] = word_count;
    }
    // dcl_tgsm_structured declares the same word array, sized stride * count.
    // The declaration form only changes how a use site computes its address.
    for (const auto& [slot, shape] : scan_structured_tgsm()) {
        const auto [stride, count] = shape;
        const u64 byte_count = static_cast<u64>(stride) * count;
        if (m_result.stage != ShaderStage::Compute || stride == 0u ||
            (stride & 3u) != 0u || count == 0u || byte_count > 32768u ||
            total_tgsm_bytes > 32768u - byte_count ||
            m_tgsm_vars.contains(slot)) {
            return fail("structured TGSM declaration violates the 32 KiB "
                        "contract");
        }
        total_tgsm_bytes += static_cast<u32>(byte_count);
        const u32 word_count = static_cast<u32>(byte_count / 4u);
        const u32 words = m_builder.emit_constant_int(m_type_uint, word_count);
        const u32 array_type = m_builder.emit_type_array(m_type_uint, words);
        const u32 pointer_type = m_builder.emit_type_pointer(
            SpvStorageClass::Workgroup, array_type);
        const u32 var_id = m_builder.emit_variable(
            pointer_type, SpvStorageClass::Workgroup);
        m_builder.emit_name(var_id, "g" + std::to_string(slot));
        m_tgsm_vars[slot] = var_id;
        m_tgsm_word_counts[slot] = word_count;
        m_tgsm_strides[slot] = stride;
    }

    if (m_result.stage == ShaderStage::Fragment &&
        m_options.logic_op_targets != 0) {
        // DxbcTranslationOptions::logic_op: each logic-op target's current
        // value, read through framebuffer fetch. Set 6 is outside the
        // translator's register spaces (0-3) and the expansion set (4).
        m_builder.emit_capability(SpvCapability::InputAttachment);
        m_logic_op_image_type = m_builder.emit_type_image(
            m_type_uint, SpvDim::DimSubpassData, false, false, 2u, 0u, 0u);
        const u32 pointer = m_builder.emit_type_pointer(
            SpvStorageClass::UniformConstant, m_logic_op_image_type);
        for (u32 target = 0; target < 8u; ++target) {
            if ((m_options.logic_op_targets & (1u << target)) == 0) continue;
            const u32 var = m_builder.emit_variable(
                pointer, SpvStorageClass::UniformConstant);
            m_builder.emit_name(var, "vitreo_logic_dst" +
                                         std::to_string(target));
            m_builder.emit_decorate(var, SpvDecoration::InputAttachmentIndex,
                                    target);
            m_builder.emit_decorate(var, SpvDecoration::DescriptorSet, 6);
            m_builder.emit_decorate(var, SpvDecoration::Binding, target);
            m_logic_op_inputs[target] = var;
        }
    }

    return true;
}

} // namespace vitreo::graphics::dxbc_spirv_detail
