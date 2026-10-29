// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file dxbc_to_spirv_internal.hpp
/// @brief DxbcTranslator, shared by the dxbc_to_spirv*.cpp translation units.

#include "graphics/shaders/spirv_to_msl.hpp"
#include "dxbc_to_spirv.hpp"
#include "gs_passthrough.hpp"
#include "pipeline_stats.hpp"
#include "soft_double.hpp"
#include <vitreo/common.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <functional>
#include <map>
#include <set>
#include <sstream>
#include <tuple>

#include "graphics/shaders/dxbc_to_spirv_spv_defs.hpp"
#include "graphics/shaders/dxbc_to_spirv_builder.hpp"
#include "graphics/shaders/dxbc_to_spirv_sm4.hpp"

namespace vitreo::graphics::dxbc_spirv_detail {

[[nodiscard]] DxbcMathContract math_contract_of(const DxbcContainer& container);

// =============================================================================
// DXBC to SPIR-V Translator
// =============================================================================

class DxbcTranslator {
public:
    DxbcTranslator(const DxbcContainer& container,
                   const DxbcTranslationOptions& options)
        : m_container(container),
          m_options(options),
          m_decoder(container.bytecode.data(), container.bytecode.size()),
          m_immediate_constants(
              decode_immediate_constant_buffer(container.bytecode)) {
        m_decoder.set_sm51_resource_operands(container.is_sm51_plus());
    }

    DxbcToSpirvResult run();

private:
    [[nodiscard]] bool fail(const std::string& reason) {
        m_result.ok = false;
        m_result.failure = reason;
        return false;
    }

    /// What one descriptor is, for the SPIR-V capabilities its array needs.
    enum class DescriptorKind : u8 {
        UniformBuffer,       // cb#
        StorageBuffer,       // raw / structured t# and u#
        SampledImage,        // t# texture
        Sampler,             // s#
        StorageImage,        // typed u# texture
        UniformTexelBuffer,  // Buffer<T>
        StorageTexelBuffer,  // RWBuffer<T>
    };
    /// A remapped or shader model 5.1 translation: bindings come from the
    /// declarations and the remap (DxbcTranslationOptions::register_remap).
    [[nodiscard]] bool descriptor_table_translation() const {
        return m_container.is_sm51_plus() || !m_options.register_remap.empty();
    }
    /// Resolve every declared binding (m_ranges, m_result.bindings), and
    /// refuse the options a descriptor-table translation cannot honour.
    bool resolve_bindings();
    /// Registers [lower, lower + count) of (cls, space) through the remap:
    /// the set and binding of `lower`, and in `covered` how many registers
    /// land on consecutive bindings from there (0 = all of them, for an
    /// unbounded run). False when `lower` -- or, for a bounded count, any
    /// register of the run -- has no consecutive binding.
    [[nodiscard]] bool resolve_register_run(DxbcRegisterClass cls, u32 space,
                                            u32 lower, u32 count, u32& set,
                                            u32& binding, u32& covered) const;
    /// The variable of resource `key`: `pointer_type` points to one
    /// descriptor, which a descriptor-array range wraps in an array.
    /// Decorated with its resolved set and binding. 0 after fail().
    [[nodiscard]] u32 declare_resource(DxbcRegisterClass cls, u32 key,
                                       u32 pointer_type,
                                       SpvStorageClass storage,
                                       DescriptorKind kind,
                                       const std::string& name);
    /// The RDEF size, in 16-byte vectors, of constant buffer `key` (0 when
    /// reflection does not list it).
    [[nodiscard]] u32 rdef_cbuffer_vectors(u32 key) const;
    struct ResourceRange;
    /// The array element a 5.1 descriptor-array operand names: its register
    /// minus the range's lower bound (a uint id, decorated NonUniform under
    /// NonUniformResourceIndex). 0 after fail().
    [[nodiscard]] u32 array_element_index(const Sm4Operand& operand,
                                          const ResourceRange& range);
    /// A pointer to that element's descriptor.
    [[nodiscard]] u32 array_element_pointer(const Sm4Operand& operand,
                                            const ResourceRange& range);

    bool emit_header();
    bool emit_types();
    bool emit_variables();
    bool emit_resource_variables();
    bool emit_function();
    u32 declare_expansion_buffer(u32 element_type, u32 stride,
                                 const char* name, u32 binding,
                                 bool read_only);
    bool emit_geometry_expansion_buffers();
    bool emit_vertex_capture_declarations();
    bool emit_vertex_capture_epilogue();
    bool emit_geometry_input_prologue();
    bool emit_geometry_vertex(bool cut_after, u32 stream);
    void emit_geometry_cut();
    bool translate_instruction(u32 opcode_token, u32 sample_controls);
    /// translate_operation, gated to the pixel's own invocation for a u#
    /// store or result-less atomic (m_uav_leader_var).
    bool translate_gated_operation(u32 opcode_token, Sm4Opcode opcode,
                                   u32 sample_controls);
    bool scan_geometry_declarations();
    bool scan_compute_declarations();
    bool begin_pending_switch_body(Sm4Opcode opcode);
    bool scan_subroutine_labels();
    bool translate_subroutine(u32 label);
    bool emit_subroutine_functions();
    bool emit_subroutine_call(u32 label);
    // Double precision (D3D11.3 22.14-22.17): soft_double.hpp helpers, one
    // SPIR-V function each, emitted after main for the ones a shader calls.
    enum class DoubleHelper : u32 {
        Add, Mul, Div, Fma, Min, Max, Eq, Ge, Lt, ToFloat, FromFloat, ToInt,
        ToUint, FromInt, FromUint, Saturate, SaturateFloat, Count
    };
    bool translate_double(Sm4Opcode opcode);
    [[nodiscard]] u32 call_double(DoubleHelper helper,
                                  const std::vector<u32>& args);
    bool emit_double_helpers();
    [[nodiscard]] bool load_bits(const Sm4Operand& src, u32& out_id);
    // Dynamic shader linkage (D3D11.3 7.19): see scan_class_linkage.
    bool scan_class_linkage();
    bool add_class_linkage_declarations(
        std::map<u32, u32>& cbuffers, std::map<u32, u32>& textures,
        std::map<u32, u32>& return_tokens, std::set<u32>& samplers);
    [[nodiscard]] std::vector<u32> slots_reaching(u32 body) const;
    [[nodiscard]] std::vector<u32> uniform_index_candidates(
        Sm4OperandType type, u32 offset, u32 dimension) const;
    bool translate_uniformly_indexed(u32 opcode_token, u32 sample_controls,
                                     u32 resource_dimension,
                                     usize instruction_end);
    bool emit_interface_call(u32 opcode_token);
    bool load_this_pointer(const Sm4Operand& src, u32& out_id);
    [[nodiscard]] u32 relative_index_value(Sm4OperandType type, u32 reg,
                                           u32 component, u32 offset);
    [[nodiscard]] u32 temp_pointer_type();
    [[nodiscard]] SpvStorageClass temp_storage_class() const {
        return m_temps_private ? SpvStorageClass::Private
                               : SpvStorageClass::Function;
    }

    u32 get_temp_variable(u32 index);
    u32 get_float_type(u32 components);
    // Float-rule helpers (D3D11.3 3.1.3): NaN lanes of a loaded source (0
    // when the source is an immediate with no NaN), denormal flush, and the
    // Inf-or-NaN lanes (exponent all ones), all on the integer bits.
    [[nodiscard]] u32 source_nan_lanes(const Sm4Operand& operand, u32 value);
    [[nodiscard]] u32 flush_denormals(u32 value);
    [[nodiscard]] static bool immediate_has_magnitude(const Sm4Operand& operand,
                                                      float magnitude,
                                                      u32 write_mask);
    [[nodiscard]] u32 non_finite_lanes(u32 value);
    [[nodiscard]] u32 float_compare_result(Sm4Opcode opcode,
                                           const Sm4Operand& a, u32 va,
                                           const Sm4Operand& b, u32 vb);
    [[nodiscard]] u32 f32_to_f16_bits(u32 value);
    [[nodiscard]] u32 make_vec4_imm(float x, float y, float z, float w);
    [[nodiscard]] u32 make_uvec4_imm(u32 x, u32 y, u32 z, u32 w);
    [[nodiscard]] bool load_vec4(const Sm4Operand& src, u32& out_id);
    [[nodiscard]] bool load_vec4_modified(const Sm4Operand& src, u32& out_id);
    [[nodiscard]] bool store_vec4(const Sm4Operand& dst, u32 value_id);
    [[nodiscard]] bool indexable_temp_pointer(const Sm4Operand& operand,
                                              u32& pointer);
    [[nodiscard]] u32 apply_swizzle(u32 vec_id, u32 swizzle);
    /// A Texture1D[Array] address, rewritten for the height-1 2D image that
    /// backs it: (u, [slice], -, w) -> (u, row, [slice], w). The row is 0 for
    /// an integer texel address and 0.5 -- the texel centre -- for a
    /// normalized one, so no filter or border mode can blend in anything but
    /// the one row. `w` (the ld mip) is kept.
    [[nodiscard]] u32 widen_1d_address(u32 address4, bool arrayed,
                                       bool normalized);
    /// One sample / sample_b / sample_l / sample_d through a MINIMUM_ /
    /// MAXIMUM_ sampler (DxbcTranslationOptions::minmax_samplers).
    struct MinMaxSample {
        u32 image{0};
        /// The image with s#, bound POINT-filtered by the frontend.
        u32 sampled_image{0};
        /// The border twin (0: none) and the colour it completes.
        u32 twin_sampled_image{0};
        u32 border_colour{0};
        /// What a native sample would take: vec2 (2D, 1D widened), vec3
        /// (2D array, 1D array widened, 3D, cube) or vec4 (cube array).
        u32 coordinate{0};
        bool texture_1d{false};
        bool cube{false};
        bool volume{false};
        bool arrayed{false};
        u32 slot{0};
        enum class Lod : u8 { Explicit, Implicit, Gradient } lod{Lod::Explicit};
        /// Explicit: the LOD operand; Implicit: sample_b's bias (0 = none).
        u32 lod_operand{0};
        /// Gradient: the raw derivative operands (vec4).
        u32 gradient_x{0};
        u32 gradient_y{0};
        /// aoffimmi, in texels.
        std::array<i32, 3> offset{};
    };
    [[nodiscard]] u32 emit_minmax_sample(const MinMaxSample& sample);
    /// From @p start_token (2 = the first instruction) to the next `ret`: a
    /// hull-shader phase is scanned from its own first token.
    [[nodiscard]] u32 scan_temp_count(usize start_token = 2) const;
    [[nodiscard]] std::map<u32, u32> scan_indexable_temps(
        usize start_token = 2) const;
    [[nodiscard]] std::map<u32, u32> scan_constant_buffers() const;
    [[nodiscard]] std::tuple<std::map<u32, u32>, std::map<u32, u32>,
                             std::set<u32>>
    scan_shader_resources() const;
    [[nodiscard]] std::map<u32, std::pair<u32, u32>>
    scan_typed_uavs() const;

    /// dcl_resource_structured declarations: t# slot -> struct byte stride.
    [[nodiscard]] std::map<u32, u32> scan_structured_srvs() const;
    /// dcl_resource_raw declarations: the t# slots holding ByteAddressBuffers.
    [[nodiscard]] std::set<u32> scan_raw_srvs() const;
    [[nodiscard]] std::map<u32, u32> scan_raw_tgsm() const;

    /// Resolve the resource operand of a structured load/store to the SPIR-V
    /// variable holding its words, its structure stride, the pointer type for
    /// one word, and whether the words sit inside a Block member (true for
    /// storage buffers, false for groupshared arrays).
    /// @return false after reporting the failure
    /// The push-constant block variable, created on first use (a global,
    /// so the section does not matter) and added to the entry interface.
    [[nodiscard]] u32 push_block_var();
    /// Whether the frontend binds the push block, with the s# MipLODBias in
    /// member 0, for this stage: the draw stages (PS; VS, or the DS in its
    /// place), a dispatched CS, and the geometry and hull kernels (whose
    /// block carries the biases but no view sizes, see guards_buffer).
    ///
    /// Never in a descriptor-table translation: the block's sampler state
    /// (MipLODBias, member 0; the point-mip masks, members 4 and 6) is keyed
    /// by D3D11 s# register, and that frontend's samplers are descriptors
    /// (DxbcTranslationOptions::register_remap), so none of it is applied.
    [[nodiscard]] bool sampler_bias_bound() const {
        if (descriptor_table_translation()) return false;
        return m_result.stage == ShaderStage::Fragment ||
               m_result.stage == ShaderStage::Vertex ||
               m_result.stage == ShaderStage::Compute || ds_lowered() ||
               gs_as_compute() || hs_as_compute() || ds_as_compute();
    }
    /// Whether this translation guards the raw/structured buffer operand
    /// `type` (t#/u# only; g# is bounded by its declaration).
    [[nodiscard]] bool guards_buffer(Sm4OperandType type) const;
    /// A Texture3D u# bound over a W-slice window (kGuardSizeSrvSlots +
    /// slot of the size table, kVolumeUavWindowSizeShift): @p coordinate's z
    /// moved by FirstWSlice, with @p outside a bool that is true for a z at
    /// or past WSize (0 when the slot has no window entry).
    [[nodiscard]] u32 volume_uav_window(u32 slot, u32 coordinate,
                                        u32& outside);
    /// Whether t#slot is an R32G32B32 typed buffer view bound as three R32
    /// texels per element (DxbcTranslationOptions::rgb32_buffer_srvs).
    [[nodiscard]] bool rgb32_buffer_srv(u32 slot) const {
        return slot < 32u && ((m_options.rgb32_buffer_srvs >> slot) & 1u) != 0;
    }
    /// Whether t#slot is a Texture2DMS(Array) declaration bound to a view of
    /// a one-sample texture, declared as a plain 2D image
    /// (DxbcTranslationOptions::single_sample_ms_srvs).
    [[nodiscard]] bool single_sample_ms_srv(u32 slot) const {
        return m_texture_single_sample_ms.contains(slot);
    }
    /// The image type a multisampled t# declaration is loaded as: the
    /// multisample image, or for a single_sample_ms_srv the 2D one.
    [[nodiscard]] u32 ms_texture_image_type(u32 slot) const {
        const u32 return_type = m_texture_return_types.at(slot);
        const bool arrayed = m_texture_arrays.contains(slot);
        if (single_sample_ms_srv(slot)) {
            return return_type == 4u
                ? (arrayed ? m_type_uimage_2d_array : m_type_uimage_2d)
                : return_type == 3u
                ? (arrayed ? m_type_iimage_2d_array : m_type_iimage_2d)
                : (arrayed ? m_type_image_2d_array : m_type_image_2d);
        }
        return return_type == 4u
            ? (arrayed ? m_type_ms_uimage_2d_array : m_type_ms_uimage_2d)
            : return_type == 3u
            ? (arrayed ? m_type_ms_iimage_2d_array : m_type_ms_iimage_2d)
            : (arrayed ? m_type_ms_image_2d_array : m_type_ms_image_2d);
    }
    /// `ld` of element `element` from such a view: texels 3e, 3e+1, 3e+2 as
    /// (x, y, z, 1), or (0, 0, 0, 1) for an element past the view.
    /// `result_type` is the image's 4-vector type, `return_type` the
    /// declared return type (4 UINT, 3 SINT, else float). `bias` is the
    /// view's element bias in R32 texels (typed_buffer_bias), 0 for none.
    [[nodiscard]] u32 load_rgb32_buffer_element(u32 image, u32 result_type,
                                                u32 return_type, u32 element,
                                                u32 bias);
    /// DxbcTranslationOptions::typed_buffer_bias: the size-table entry of
    /// the typed buffer view at t# (@p uav false) or u# @p slot; 0 (no id)
    /// when this translation has none (the option is off, or a u# past the
    /// table).
    [[nodiscard]] u32 typed_buffer_entry(bool uav, u32 slot);
    /// The element bias in a typed_buffer_entry (kTypedBufferBiasMask); 0
    /// (no id) for no entry.
    [[nodiscard]] u32 typed_buffer_bias(u32 entry);
    /// What ld / ld_uav_typed return for an element past the view (D3D11.3
    /// 22.4.6, 19.1.3.3): (0, 0, 0, a) of @p result_type, a the format's
    /// default alpha -- 1 (1.0, or integer 1 for a @p return_type of 4 UINT
    /// / 3 SINT) when @p entry carries kTypedBufferAlphaMissing, else 0.
    [[nodiscard]] u32 typed_buffer_out_of_view(u32 result_type,
                                               u32 return_type, u32 entry);
    /// The element count of a typed buffer view bound as @p image: the
    /// texture's width less @p bias (an id, or 0 for none) -- NumElements.
    [[nodiscard]] u32 typed_buffer_count(u32 image, u32 bias);
    /// `word` < the bound view's size in words, or true when the frontend
    /// left the size 0 (unknown). Returns a bool id.
    [[nodiscard]] u32 buffer_word_in_bounds(bool uav, u32 slot, u32 word);
    /// Load one word through `pointer_type` from the Block member array,
    /// 0 when `in_bounds` is false (and the address is clamped to 0 so the
    /// backend never dereferences past the buffer).
    [[nodiscard]] u32 guarded_buffer_word_load(u32 variable, u32 pointer_type,
                                               u32 word, u32 in_bounds);
    /// Store one word only when `in_bounds` (D3D drops out-of-view writes).
    void guarded_buffer_word_store(u32 variable, u32 pointer_type, u32 word,
                                   u32 value, u32 in_bounds);
    [[nodiscard]] bool resolve_structured_operand(
        const Sm4Operand& resource, const char* opcode_name,
        u32& variable, u32& stride, u32& word_pointer_type,
        bool& indexed_through_member);

    /// dcl_uav_raw declarations: u# slots holding an RWByteAddressBuffer.
    [[nodiscard]] std::set<u32> scan_raw_uavs() const;

    /// dcl_uav_structured declarations: u# slot -> struct byte stride.
    [[nodiscard]] std::map<u32, u32> scan_structured_uavs() const;
    /// The u# whose hidden counter the program moves, in first-use order.
    [[nodiscard]] std::vector<u32> scan_uav_counters() const;
    /// Every dcl_uav_typed / dcl_uav_raw / dcl_uav_structured: u# slot -> the
    /// declaration's OpcodeToken0, whose bit 16 is the `_glc` flag.
    [[nodiscard]] std::map<u32, u32> scan_uav_declaration_tokens() const;
    /// OpcodeToken0 of dcl_globalFlags (opcode 106), or 0 when absent.
    [[nodiscard]] u32 scan_global_flags() const;
    /// v# register -> D3D10_SB_INTERPOLATION_MODE of its dcl_input_ps /
    /// dcl_input_ps_sgv / dcl_input_ps_siv declaration (opcodes 98-100).
    [[nodiscard]] std::map<u32, u32> scan_input_interpolation() const;
    /// True when a `dcl_input` declares @p type -- the special input operands
    /// (vCoverage, ...) that the signature does not list.
    [[nodiscard]] bool declares_input_operand(Sm4OperandType type) const;
    /// Copy clip-distance and layer components into their builtins; run
    /// before every return from main.
    bool emit_system_value_epilogue();
    [[nodiscard]] bool translate_instructions(usize end_token);

    /// dcl_tgsm_structured declarations: g# slot -> (byte stride, count).
    [[nodiscard]] std::map<u32, std::pair<u32, u32>>
    scan_structured_tgsm() const;

    const DxbcContainer& m_container;
    const DxbcTranslationOptions& m_options;
    DxbcDecoder m_decoder;
    DxbcToSpirvResult m_result;
    SpirvBuilder m_builder;

    // Type IDs
    u32 m_type_void{0};
    u32 m_type_bool{0};
    u32 m_type_bvec2{0};
    u32 m_type_bvec4{0};
    u32 m_type_float{0};
    u32 m_type_vec2{0};
    u32 m_type_vec3{0};
    u32 m_type_vec4{0};
    u32 m_type_uint{0};
    u32 m_type_uvec2{0};
    u32 m_type_uvec3{0};
    u32 m_type_bvec3{0};
    u32 m_type_uvec4{0};
    u32 m_type_int{0};
    u32 m_type_ivec2{0};
    u32 m_type_ivec3{0};
    u32 m_type_ivec4{0};
    u32 m_type_smul_result{0};
    u32 m_type_umul_result{0};
    u32 m_type_ptr_uniform_vec4{0};
    u32 m_type_ptr_input_bool{0};
    u32 m_type_ptr_input_uint{0};
    u32 m_type_ptr_storage_buffer_uint{0};
    u32 m_type_ptr_input_uvec3{0};
    u32 m_type_ptr_function_vec4{0};
    u32 m_type_ptr_workgroup_uint{0};
    u32 m_type_ptr_image_uint{0};
    u32 m_type_ptr_image_int{0};
    u32 m_type_image_2d{0};
    u32 m_type_image_2d_array{0};
    u32 m_type_image_3d{0};
    u32 m_type_image_cube{0};
    u32 m_type_image_cube_array{0};
    u32 m_type_uimage_2d{0}, m_type_uimage_2d_array{0}, m_type_uimage_3d{0};
    u32 m_type_iimage_2d{0}, m_type_iimage_2d_array{0}, m_type_iimage_3d{0};
    u32 m_type_ms_image_2d{0}, m_type_ms_image_2d_array{0};
    u32 m_type_ms_uimage_2d{0}, m_type_ms_uimage_2d_array{0};
    u32 m_type_ms_iimage_2d{0}, m_type_ms_iimage_2d_array{0};
    u32 m_type_storage_image_2d_float{0};
    u32 m_type_storage_image_2d_uint{0};
    u32 m_type_storage_image_2d_int{0};
    u32 m_type_storage_image_2d_array_float{0};
    u32 m_type_storage_image_2d_array_uint{0};
    u32 m_type_storage_image_2d_array_int{0};
    u32 m_type_storage_image_3d_float{0};
    u32 m_type_storage_image_3d_uint{0};
    u32 m_type_storage_image_3d_int{0};
    u32 m_type_storage_buffer_image_float{0};
    u32 m_type_storage_buffer_image_uint{0};
    u32 m_type_storage_buffer_image_int{0};
    u32 m_type_sampler{0};
    u32 m_type_sampled_image_2d{0};
    u32 m_type_sampled_image_2d_array{0};
    u32 m_type_sampled_uimage_2d{0};
    u32 m_type_sampled_uimage_2d_array{0};
    u32 m_type_sampled_iimage_2d{0};
    u32 m_type_sampled_iimage_2d_array{0};
    u32 m_type_sampled_image_3d{0};
    u32 m_type_sampled_image_cube{0};
    u32 m_type_sampled_image_cube_array{0};
    u32 m_type_buffer_image_float{0};
    u32 m_type_buffer_image_uint{0};
    u32 m_type_buffer_image_int{0};
    u32 m_type_ptr_uniform_constant_buffer_image_float{0};
    u32 m_type_ptr_uniform_constant_buffer_image_uint{0};
    u32 m_type_ptr_uniform_constant_buffer_image_int{0};
    u32 m_type_ptr_uniform_constant_image_2d{0};
    u32 m_type_ptr_uniform_constant_image_2d_array{0};
    u32 m_type_ptr_uniform_constant_image_3d{0};
    u32 m_type_ptr_uniform_constant_image_cube{0};
    u32 m_type_ptr_uniform_constant_image_cube_array{0};
    u32 m_type_ptr_uniform_constant_uimage_2d{0};
    u32 m_type_ptr_uniform_constant_uimage_2d_array{0};
    u32 m_type_ptr_uniform_constant_uimage_3d{0};
    u32 m_type_ptr_uniform_constant_iimage_2d{0};
    u32 m_type_ptr_uniform_constant_iimage_2d_array{0};
    u32 m_type_ptr_uniform_constant_iimage_3d{0};
    u32 m_type_ptr_uniform_constant_ms_image_2d{0};
    u32 m_type_ptr_uniform_constant_ms_image_2d_array{0};
    u32 m_type_ptr_uniform_constant_ms_uimage_2d{0};
    u32 m_type_ptr_uniform_constant_ms_uimage_2d_array{0};
    u32 m_type_ptr_uniform_constant_ms_iimage_2d{0};
    u32 m_type_ptr_uniform_constant_ms_iimage_2d_array{0};
    u32 m_type_ptr_uniform_constant_storage_image_2d_float{0};
    u32 m_type_ptr_uniform_constant_storage_image_2d_uint{0};
    u32 m_type_ptr_uniform_constant_storage_image_2d_int{0};
    u32 m_type_ptr_uniform_constant_storage_image_2d_array_float{0};
    u32 m_type_ptr_uniform_constant_storage_image_2d_array_uint{0};
    u32 m_type_ptr_uniform_constant_storage_image_2d_array_int{0};
    u32 m_type_ptr_uniform_constant_storage_image_3d_float{0};
    u32 m_type_ptr_uniform_constant_storage_image_3d_uint{0};
    u32 m_type_ptr_uniform_constant_storage_image_3d_int{0};
    u32 m_type_ptr_uniform_constant_storage_buffer_image_float{0};
    u32 m_type_ptr_uniform_constant_storage_buffer_image_uint{0};
    u32 m_type_ptr_uniform_constant_storage_buffer_image_int{0};
    u32 m_type_ptr_uniform_constant_sampler{0};
    u32 m_type_ptr_push_float{0};
    u32 m_type_ptr_push_uint{0};
    u32 m_type_ptr_push_sampler_biases{0};
    u32 m_type_ptr_push_vec4{0};
    u32 m_func_type{0};
    u32 m_const_u0{0};
    u32 m_const_u_ffff{0};
    u32 m_const_u_zero4{0};
    u32 m_const_u_ffff4{0};
    u32 m_glsl_std450{0};

    // Variable IDs
    u32 m_main_func{0};
    std::map<u32, u32> m_temp_vars;
    std::map<u32, u32> m_indexable_temp_vars;
    std::map<u32, u32> m_input_vars;
    /// The v# register a pixel shader reads SV_Position from (~0: none).
    u32 m_frag_coord_register{~0u};
    /// SV_Position.xy must read the pixel centre although Metal runs the
    /// shader per sample (see emit_variables).
    bool m_frag_coord_at_pixel_centre{false};
    std::map<u32, u32> m_input_value_types;
    /// R10G10B10A2_UINT vertex inputs: the normalized stage-in, the Private
    /// register the body reads, and that register's type
    /// (DxbcTranslationOptions::vertex_input_unpack_uint1010102).
    struct UnpackedVertexInput {
        u32 source{0};
        u32 target{0};
        u32 type{0};
    };
    std::vector<UnpackedVertexInput> m_unpacked_vertex_inputs;
    std::map<u32, u32> m_input_pointer_types;
    /// Vertex-stage system values that SPIR-V models as a scalar builtin
    /// rather than a location-bound varying (SV_VertexID, SV_InstanceID):
    /// input register -> {variable id, component index within the register}.
    /// One scalar system-value builtin living in one lane of an input
    /// register. A register can hold several (fxc packs SV_PrimitiveID,
    /// SV_RenderTargetArrayIndex, ... together), and can mix them with
    /// ordinary varying lanes.
    struct ScalarBuiltin {
        u32 var{0};
        u32 component{0};
        /// SV_IsFrontFace: a bool builtin, which D3D reads as 0/~0.
        bool boolean{false};
        /// Non-zero: a second uint builtin subtracted from `var` on load.
        u32 minus_var{0};
        /// SV_InstanceID in a vertex-capture variant: add push member 3.
        bool capture_instance_bias{false};
        /// Non-zero: the value is divided by it on load
        /// (DxbcTranslationOptions::line_quad_primitive_divisor).
        u32 divisor{0};
    };
    std::map<u32, std::vector<ScalarBuiltin>> m_input_scalar_builtins;
    std::map<u32, u32> m_output_vars;
    std::map<u32, u32> m_output_value_types;
    std::map<u32, u32> m_cbuffer_vars;    // CB slot -> Uniform variable ID
    std::map<u32, u32> m_cbuffer_sizes;   // CB slot -> vec4 count
    std::vector<std::array<float, 4>> m_immediate_constants;
    std::map<u32, u32> m_texture_vars;
    /// Render target n -> its framebuffer-fetch input (logic-op variant).
    std::map<u32, u32> m_logic_op_inputs;
    u32 m_logic_op_image_type{0};
    std::map<u32, SpvDim> m_texture_dimensions;
    std::map<u32, u32> m_texture_return_types;
    std::set<u32> m_texture_arrays;
    std::set<u32> m_texture_multisampled;
    /// The multisampled declarations bound to one-sample views
    /// (DxbcTranslationOptions::single_sample_ms_srvs), declared 2D.
    std::set<u32> m_texture_single_sample_ms;
    /// Texture1D / Texture1DArray slots. The frontend backs a 1D resource
    /// with a height-1 2D texture, so these are declared as 2D images (arrayed
    /// for 1D arrays) and each use widens the 1D address -- see
    /// widen_1d_address.
    std::set<u32> m_texture_1d;
    std::map<u32, u32> m_sampler_vars;
    /// s# -> its border twin (DxbcTranslationOptions::border_twin_samplers).
    std::map<u32, u32> m_border_twin_vars;
    std::map<u32, u32> m_uav_vars;
    /// u# slots whose typed UAV is Dim=Buffer (RWBuffer<T>): the
    /// coordinate is one element index, not a 2D texel position.
    std::set<u32> m_buffer_uav_slots;
    /// Typed UAVs declared as Texture3D (RWTexture3D<T>): addressed by an
    /// (x, y, z) texel position and bounded on all three axes.
    std::set<u32> m_volume_uav_slots;
    /// Typed UAVs declared as Texture2DArray (RWTexture2DArray<T>), which is
    /// also how a cube UAV arrives -- six faces as array slices. Addressed by
    /// (x, y, slice), so the coordinate is three components exactly as for a
    /// volume, but the image is an ARRAYED Dim2D rather than Dim3D.
    std::set<u32> m_array_uav_slots;
    /// RWTexture1D[Array] slots: 2D storage images of height 1, addressed
    /// through widen_1d_address like their SRV counterparts.
    std::set<u32> m_uav_1d;
    /// Resource return type of a typed UAV: 3 = SINT, 4 = UINT, 5 = FLOAT.
    std::map<u32, u32> m_uav_return_types;

    /// Image type and result/texel vector type for a typed UAV slot, honouring
    /// its resource return type. An integer-typed image reads and writes
    /// integer vectors; the ISA carries everything as raw 32-bit lanes, so the
    /// conversion at the boundary is a bitcast, never a numeric convert.
    void typed_uav_types(u32 slot, bool buffer_uav, u32& image_type,
                         u32& vector_type) const {
        const auto found = m_uav_return_types.find(slot);
        const u32 return_type = found == m_uav_return_types.end() ? 5u
                                                                  : found->second;
        if (!buffer_uav) {
            const bool volume = m_volume_uav_slots.contains(slot);
            const bool arrayed = m_array_uav_slots.contains(slot);
            if (return_type == 4u) {
                image_type = volume ? m_type_storage_image_3d_uint
                           : arrayed ? m_type_storage_image_2d_array_uint
                                     : m_type_storage_image_2d_uint;
                vector_type = m_type_uvec4;
            } else if (return_type == 3u) {
                image_type = volume ? m_type_storage_image_3d_int
                           : arrayed ? m_type_storage_image_2d_array_int
                                     : m_type_storage_image_2d_int;
                vector_type = m_type_ivec4;
            } else {
                image_type = volume ? m_type_storage_image_3d_float
                           : arrayed ? m_type_storage_image_2d_array_float
                                     : m_type_storage_image_2d_float;
                vector_type = m_type_vec4;
            }
            return;
        }
        if (return_type == 4u) {
            image_type = m_type_storage_buffer_image_uint;
            vector_type = m_type_uvec4;
        } else if (return_type == 3u) {
            image_type = m_type_storage_buffer_image_int;
            vector_type = m_type_ivec4;
        } else {
            image_type = m_type_storage_buffer_image_float;
            vector_type = m_type_vec4;
        }
    }

    /// StructuredBuffer SRVs (t# registers backed by a storage buffer, not a
    /// texture): slot -> variable id, and slot -> struct byte stride.
    std::map<u32, u32> m_structured_srv_vars;
    std::map<u32, u32> m_structured_srv_strides;
    /// Typed buffer SRVs (`Buffer<T>`, dcl_resource_buffer). Also a storage
    /// buffer of words rather than an image: slot -> variable id, and
    /// slot -> resource return type (3 = SINT, 4 = UINT, 5 = FLOAT), which
    /// selects the defaults for the components the format does not supply.
    std::map<u32, u32> m_typed_buffer_srv_vars;
    std::map<u32, u32> m_typed_buffer_srv_return_types;
    /// ByteAddressBuffer SRVs (`dcl_resource_raw`). Byte-addressed and read as
    /// 32-bit words with no format at all, so — unlike a typed buffer — a
    /// storage buffer of words is the exact model: slot -> variable id.
    std::map<u32, u32> m_raw_srv_vars;
    std::map<u32, u32> m_tgsm_vars;
    std::map<u32, u32> m_tgsm_word_counts;
    /// Byte stride of a groupshared array declared with dcl_tgsm_structured;
    /// absent for raw (dcl_tgsm_raw) declarations, which are byte-addressed.
    std::map<u32, u32> m_tgsm_strides;
    /// RWStructuredBuffer UAVs (u# registers backed by a writable storage
    /// buffer rather than a storage image), and their structure strides.
    std::map<u32, u32> m_raw_uav_vars;
    std::map<u32, u32> m_structured_uav_vars;
    std::map<u32, u32> m_structured_uav_strides;
    /// u# -> the storage buffer holding that UAV's hidden counter, created on
    /// first use by imm_atomic_alloc / imm_atomic_consume.
    std::map<u32, u32> m_uav_counter_vars;
    /// A 5.1 UAV array's counters are an array too: u# -> the pointer type
    /// of one counter, for the element access chain.
    std::map<u32, u32> m_uav_counter_element_types;

    /// A declared binding as translated (DxbcResolvedBinding) and the SPIR-V
    /// variable it became. Keyed by (class, key): the range ID in shader
    /// model 5.1, the register before (the key every per-resource table of
    /// this translator uses).
    struct ResourceRange {
        DxbcResolvedBinding resolved;
        /// A 5.1 range of more than one register: a descriptor array.
        bool is_array{false};
        u32 variable{0};
        /// The pointer type of one descriptor -- the variable's own type for
        /// a single register, its element's for an array.
        u32 element_pointer_type{0};
        DescriptorKind kind{DescriptorKind::SampledImage};
    };
    std::map<std::pair<DxbcRegisterClass, u32>, ResourceRange> m_ranges;
    [[nodiscard]] const ResourceRange* find_range(DxbcRegisterClass cls,
                                                  u32 key) const {
        const auto found = m_ranges.find({cls, key});
        return found == m_ranges.end() ? nullptr : &found->second;
    }
    /// The descriptor-array operands of the instruction being translated
    /// (collected by translate_uniformly_indexed's operand probe, bound by
    /// translate_instruction).
    std::vector<Sm4Operand> m_pending_array_operands;
    /// DxbcTranslationOptions::sampler_state_buffer: the record buffer, and
    /// for the instruction being translated the record index (binding +
    /// element, a uint id) of each descriptor-array s# operand.
    u32 m_sampler_state_var{0};
    std::map<u32, u32> m_sampler_record_index;
    /// One word of s# `sampler`'s record (sampler_state_buffer).
    [[nodiscard]] u32 sampler_state_word(const Sm4Operand& sampler, u32 word);
    u32 m_sampler_biases{0};
    u32 m_dispatch_thread_id{0};
    u32 m_group_id{0};
    u32 m_group_thread_id{0};
    u32 m_group_index{0};

    struct IfFrame {
        u32 else_block{0};
        u32 merge_block{0};
        bool has_else{false};
    };
    std::vector<IfFrame> m_if_stack;
    struct LoopFrame {
        u32 header_block{0};
        u32 continue_block{0};
        u32 merge_block{0};
    };
    std::vector<LoopFrame> m_loop_stack;
    /// Per-loop iteration counters (Private, per invocation) -- see LOOP.
    std::vector<u32> m_loop_guard_vars;
    /// The entry point, written by run() after every function body (see
    /// emit_function): its model, interface, and how many loop guards the
    /// interface already lists.
    SpvExecutionModel m_entry_model{SpvExecutionModel::Vertex};
    std::vector<u32> m_entry_interface;
    enum class BreakableKind : u8 { Loop, Switch };
    struct BreakableFrame {
        BreakableKind kind;
        u32 merge_block;
    };
    std::vector<BreakableFrame> m_breakable_stack;
    struct SwitchFrame {
        u32 selector{0};
        u32 dispatch_block{0};
        u32 merge_block{0};
        std::vector<std::pair<u32, u32>> targets;
        std::set<u32> case_values;
        std::vector<u32> pending_cases;
        bool pending_default{false};
        bool has_default{false};
        bool active_body{false};
        u32 default_block{0};
    };
    std::vector<SwitchFrame> m_switch_stack;
    bool m_block_terminated{false};
    /// Microsoft Learn "Instruction Return Type and Modifiers": OpcodeToken0
    /// bit 13 is the instruction's SATURATE modifier. It clamps every written
    /// destination component to [0,1] after the operation; silently dropping
    /// it produces unclamped results that look like arbitrary shader math
    /// errors. Set per instruction, consumed by store_vec4().
    bool m_saturate_destination{false};
    /// Function ids of the double helpers the shader calls (0: unused).
    std::array<u32, static_cast<usize>(DoubleHelper::Count)> m_double_helpers{};
    /// struct { uint; uint; }: OpUMulExtended's scalar result.
    u32 m_type_uint_pair{0};
    std::map<usize, u32> m_double_function_types;
    std::map<u32, usize> m_subroutine_labels;
    /// m_subroutine_labels / m_subroutine_functions key of `label fb#` (a
    /// function body, D3D11.3 22.3.49): fb# | kFunctionBodyKey. `label l#`
    /// keys by l#.
    static constexpr u32 kFunctionBodyKey = 0x80000000u;
    /// An interface declaration (dcl_interface fp#[arraySize][callSites] =
    /// {ft#...}, 22.3.51): interface slots base .. base + array_size - 1.
    struct InterfaceDeclaration {
        u32 base{0};
        u32 array_size{0};
        u32 call_sites{0};
        std::vector<u32> tables;
    };
    /// A t#, s# or cb# operand whose register index is itself indexed
    /// (7.11: uniform indexing, which is how a class method reaches its
    /// instance's data, t[this[i].z + n]): its register space, the literal
    /// part of the index and, for a t#, the dimension and return types its
    /// instruction names (resource-dim / return-type opcode extensions).
    struct UniformIndexUse {
        Sm4OperandType type{Sm4OperandType::Resource};
        u32 offset{0};
        u32 dimension{0};
        u32 return_tokens{0};
    };
    /// m_uniform_index_uses / m_current_body key of code outside any
    /// function body.
    static constexpr u32 kOutsideBodies = 0xFFFFFFFFu;
    std::map<u32, std::vector<u32>> m_function_tables;
    std::vector<InterfaceDeclaration> m_interfaces;
    u32 m_interface_slots{0};
    /// The class instance bound at each interface slot of a specialised
    /// program (DxbcContainer::slot_bindings); every slot unbound otherwise.
    std::vector<DxbcSlotBinding> m_slot_bindings;
    std::map<u32, std::vector<UniformIndexUse>> m_uniform_index_uses;
    /// The function body being translated (fb#), or kOutsideBodies.
    u32 m_current_body{kOutsideBodies};
    std::vector<u32> m_call_stack;
    /// Outside a hull shader every `label` body is a SPIR-V function of its
    /// own (label -> function id), so `call`/`callc` work anywhere and a
    /// `ret`/`retc` in the body returns from it. The r#/x# registers are
    /// shared with main, so they are then module-scope Private variables.
    std::map<u32, u32> m_subroutine_functions;
    bool m_in_subroutine{false};
    bool m_temps_private{false};
    u32 m_type_ptr_private_vec4{0};
    /// Whether translate_instructions last stopped at a top-level `ret`.
    bool m_stopped_at_ret{false};

    // SM4 single-stream geometry shader declaration state.
    u32 m_gs_input_primitive{0};
    bool m_gs_input_adjacency{false};
    u32 m_gs_output_topology{0};
    u32 m_gs_max_output_vertices{0};

    /// o# registers to flush on `emit`, in signature order -- the record
    /// layout of the expansion buffer.
    std::vector<u32> m_gs_emit_registers;
    /// v# registers the prologue fills, in signature order -- the record
    /// layout of the captured vertex-stage buffer.
    std::vector<u32> m_gs_input_registers;
    /// Expansion buffers, descriptor set 4 (0-3 are cb/SRV/sampler/UAV).
    u32 m_gs_in_var{0};       ///< binding 0: captured vertex-stage records.
    u32 m_gs_out_var{0};      ///< binding 1: expanded output vertices.
    u32 m_gs_counter_var{0};  ///< binding 2: slot 0 is the append counter.
    u32 m_gs_index_var{0};    ///< binding 3: the guest index buffer.
    /// Private scratch: the last two emitted vertices of the current strip,
    /// one vec4 per output register, plus how many vertices this strip has.
    /// This is what lets `emit` write independent primitives instead of a
    /// strip -- Metal has no primitive restart, so a strip that `cut` breaks
    /// cannot be drawn as one strip.
    std::vector<u32> m_gs_strip_prev0;
    std::vector<u32> m_gs_strip_prev1;
    u32 m_gs_strip_length{0};
    u32 m_gs_primitive_index{0};
    /// dcl_gsinstances, and the per-invocation decomposition of the thread
    /// index: invocation = primitive * instances + instance. vPrim and
    /// vGSInstanceID are read from Private copies so a subroutine body can
    /// load them too.
    u32 m_gs_instance_count{1};
    u32 m_gs_invocation_index{0};
    u32 m_gs_primitive_var{0};
    u32 m_gs_instance_var{0};
    /// This invocation's capacity per stream, and how many vertices it has
    /// written to each declared stream (Private). Its regions are found from
    /// the invocation index (Private too, so an `emit` inside a subroutine
    /// body sees it) and the control block's per-stream stride.
    u32 m_gs_invocation_capacity{0};
    std::array<u32, 4> m_gs_stream_count_vars{};
    u32 m_gs_invocation_var{0};
    /// Emits so far, across every stream: dcl_maxOutputVertexCount limits
    /// those, not the list vertices written (D3D11.3 13.3: "based on how many
    /// emit calls the Shader makes" -- measured, d3d11_gsso_probe
    /// [maxvertexcount] and gs_ptmax).
    u32 m_gs_emit_total{0};
    /// dcl_stream m0..m3 (bit n); stream 0 alone when none is declared.
    u32 m_gs_stream_mask{0};
    /// Pass A: the vertex shader writes its outputs into the expansion input
    /// buffer as well as to the rasteriser. See
    /// .docs/design-geometry-shader-emulation.md.
    [[nodiscard]] bool vs_capture() const {
        return m_container.stage == ShaderStage::Vertex &&
               m_options.capture_vertex_outputs_to_buffer;
    }
    u32 m_capture_buffer{0};
    u32 m_capture_vertex_index{0};
    /// Input variables by SpvBuiltIn, so a second declaration of the same
    /// builtin (which Metal rejects as a redefined attribute) reuses the
    /// first; and those the capture added to the entry-point interface.
    std::map<u32, u32> m_builtin_inputs;
    std::vector<u32> m_capture_extra_inputs;
    [[nodiscard]] u32 builtin_input(SpvBuiltIn builtin, const char* name);
    /// The shader's own SV_VertexID variable, when it reads one. The capture
    /// keys records by this same variable: a second VertexIndex builtin
    /// becomes a second [[vertex_id]] parameter, which Metal rejects as a
    /// redefinition -- so every SV_VertexID shader failed to capture.
    u32 m_vertex_id_input{0};
    std::vector<u32> m_capture_registers;
    /// Set wherever an OpReturn is emitted before the function epilogue. A
    /// capture epilogue would not run on such a path, so capture refuses
    /// rather than dropping those vertices silently.
    bool m_emitted_early_return{false};
    /// True when this geometry shader is being lowered to a compute shader
    /// that expands primitives into a buffer (Metal has no geometry stage).
    /// See .docs/design-geometry-shader-emulation.md. Asked by every site that
    /// would otherwise emit a geometry-stage construct, so the mode cannot be
    /// honoured in one place and missed in another.
    [[nodiscard]] bool gs_as_compute() const {
        return m_container.stage == ShaderStage::Geometry &&
               m_options.emulate_geometry_with_compute;
    }
    /// A hull shader lowered to a per-patch compute kernel, and a domain
    /// shader lowered to a TessellationEvaluation module fed by its buffers
    /// (.docs/design-tessellation-emulation.md).
    [[nodiscard]] bool hs_as_compute() const {
        return m_container.stage == ShaderStage::Hull &&
               m_options.emulate_tessellation;
    }
    [[nodiscard]] bool ds_lowered() const {
        return m_container.stage == ShaderStage::Domain &&
               m_options.emulate_tessellation && !m_options.domain_to_compute;
    }
    /// A domain shader lowered to a compute kernel for the software
    /// tessellator: one thread per domain point, outputs to a record buffer
    /// (DxbcTranslationOptions::domain_to_compute).
    [[nodiscard]] bool ds_as_compute() const {
        return m_container.stage == ShaderStage::Domain &&
               m_options.emulate_tessellation && m_options.domain_to_compute;
    }
    /// The stage applies push block members 7-10 in its epilogue
    /// (DxbcTranslationOptions::fixed_function_push): a rasterized vertex
    /// stage with a position output, or a pixel shader writing SV_Depth or
    /// the blend state's SampleMask.
    [[nodiscard]] bool fixed_function_epilogue() const {
        if (!m_options.fixed_function_push) return false;
        if (m_result.stage == ShaderStage::Fragment) {
            return m_depth_output != 0 || m_options.apply_sample_mask;
        }
        return !gs_as_compute() && !vs_capture() && !hs_as_compute() &&
               (m_container.stage == ShaderStage::Vertex || ds_lowered());
    }
    /// Stages whose inputs are v[vertex][register] arrays.
    [[nodiscard]] bool arrayed_inputs() const {
        return m_container.stage == ShaderStage::Geometry ||
               m_container.stage == ShaderStage::Hull ||
               m_container.stage == ShaderStage::Domain;
    }
    [[nodiscard]] u32 input_array_width() const {
        return m_container.stage == ShaderStage::Geometry ? m_gs_input_primitive
               : m_container.stage == ShaderStage::Hull   ? m_hs_in_cp
                                                          : m_ds_in_cp;
    }
    /// Inputs staged in Private variables by a prologue.
    [[nodiscard]] bool private_inputs() const {
        return gs_as_compute() || hs_as_compute() || ds_lowered() ||
               ds_as_compute();
    }

    // Tessellation declarations (scan_tessellation_declarations).
    enum class HsPhaseKind : u8 { ControlPoint, Fork, Join };
    struct HsPhase {
        HsPhaseKind kind{HsPhaseKind::Fork};
        usize begin{0};  ///< first token after the phase opcode
        usize end{0};    ///< token index of the next phase opcode / the end
        u32 instances{1};
    };
    std::vector<HsPhase> m_hs_phases;
    u32 m_hs_in_cp{0};
    u32 m_hs_out_cp{0};
    u32 m_ds_in_cp{0};
    u32 m_tess_domain{0};
    u32 m_tess_partitioning{0};
    u32 m_tess_output_primitive{0};
    float m_hs_max_tess_factor{64.0f};
    /// Hull: control-point output registers (OSGN) and patch-constant
    /// registers (PCSG); ocp[reg] is a Private array over output control
    /// points, pc[reg] a Private vec4. Domain: pc[reg] are its vpc inputs.
    std::map<u32, u32> m_hs_ocp_vars;
    std::map<u32, u32> m_hs_ocp_types;
    std::map<u32, u32> m_tess_pc_vars;
    std::map<u32, u32> m_tess_pc_types;
    /// The control-point phase's o# staging variables (m_output_vars as
    /// declared from OSGN), swapped with the patch constants per phase.
    std::map<u32, u32> m_hs_cp_staging_vars;
    std::map<u32, u32> m_hs_cp_staging_types;
    u32 m_hs_cp_id_var{0};
    u32 m_hs_fork_id_var{0};
    u32 m_hs_join_id_var{0};
    u32 m_hs_pc_buffer{0};      ///< binding 4: patch constants
    u32 m_hs_factor_buffer{0};  ///< binding 5: Metal tessellation factors
    u32 m_ds_tess_coord{0};
    u32 m_ds_patch_id{0};
    /// The domain point a compute-lowered domain shader's thread runs, and
    /// its patch's slot in the hull kernel's output (Private).
    u32 m_ds_point_index{0};
    u32 m_ds_cp_patch{0};
    [[nodiscard]] bool scan_tessellation_declarations();
    [[nodiscard]] bool emit_tessellation_declarations();
    [[nodiscard]] bool emit_hull_program();
    [[nodiscard]] bool emit_hull_phase_functions();
    void declare_function_temps(usize start_token);
    std::vector<u32> m_hs_phase_functions;
    [[nodiscard]] bool emit_domain_input_prologue();
    [[nodiscard]] bool emit_domain_compute_prologue();
    [[nodiscard]] bool emit_domain_compute_epilogue();
    [[nodiscard]] bool emit_hull_epilogue();
    u32 m_compute_group_size_x{0};
    u32 m_compute_group_size_y{0};
    u32 m_compute_group_size_z{0};

    u32 m_position_output{0};
    u32 m_depth_output{0};
    /// oDepthGE / oDepthLE: SV_Depth with a conservative-depth bound.
    bool m_depth_greater{false};
    bool m_depth_less{false};
    /// oMask (SV_Coverage out) and vCoverage (in): SampleMask builtins, each
    /// a one-element uint array.
    u32 m_sample_mask_output{0};
    /// DxbcTranslationOptions::line_coverage_alpha: the line's equation
    /// input, and the fragment position it is evaluated at (the shader's own
    /// SV_Position input when it has one).
    u32 m_line_distance_input{0};
    u32 m_line_frag_coord{0};
    /// m_line_frag_coord when this translation declared it (not the
    /// shader's SV_Position), for the interface list.
    u32 m_line_frag_coord_declared{0};
    /// m_sample_mask_output exists only to carry the blend state's
    /// SampleMask: the shader writes no oMask, so the epilogue stores the
    /// mask rather than ANDing it in.
    bool m_sample_mask_from_state_only{false};
    u32 m_sample_mask_input{0};
    /// D3D runs a pixel shader that is not at sample frequency once per
    /// pixel, so each u# write and atomic happens once per pixel. Metal runs
    /// a fragment function that writes memory once per covered sample
    /// (measured on Apple9: every invocation sees the pixel-centre position
    /// and the whole raster coverage in [[sample_mask]], and only
    /// [[sample_id]] tells them apart). The invocation at the lowest covered
    /// sample stands for the pixel: the prologue stores that test in
    /// m_uav_leader_var (a Private bool, so subroutines read it too), and
    /// every side effect is gated on it (gate_uav_side_effect). Zero when the
    /// shader needs no gate.
    u32 m_uav_leader_sample_id{0};
    u32 m_uav_leader_var{0};
    [[nodiscard]] u32 load_uav_leader() {
        return m_builder.emit_load(m_type_bool, m_uav_leader_var);
    }
    /// PSInvocations counting (DxbcTranslationOptions::count_pixel_invocations):
    /// the statistics record, HelperInvocation, the subgroup lane, and -- for a
    /// shader that runs per sample itself, which has no m_uav_leader_var --
    /// the SampleId the pixel test reads.
    u32 m_stats_record_var{0};
    u32 m_stats_helper_var{0};
    u32 m_stats_lane_var{0};
    u32 m_stats_sample_id{0};
    bool m_stats_sample_id_declared{false};
    bool emit_pixel_statistics_declarations();
    void emit_pixel_statistics_prologue();
    /// A vertex shader that rasterizes and captures its outputs
    /// (DxbcTranslationOptions::capture_rasterized_vertex_outputs).
    [[nodiscard]] bool vs_capture_rasterized() const {
        return m_container.stage == ShaderStage::Vertex &&
               m_options.capture_rasterized_vertex_outputs &&
               !m_options.capture_vertex_outputs_to_buffer;
    }
    [[nodiscard]] bool translate_operation(u32 opcode_token, Sm4Opcode opcode,
                                           u32 sample_controls);
    [[nodiscard]] bool translate_flow_control_operation(
        u32 opcode_token, Sm4Opcode opcode, u32 sample_controls);
    [[nodiscard]] bool translate_integer_operation(
        u32 opcode_token, Sm4Opcode opcode, u32 sample_controls);
    [[nodiscard]] bool translate_bitwise_operation(
        u32 opcode_token, Sm4Opcode opcode, u32 sample_controls);
    [[nodiscard]] bool translate_float_operation(
        u32 opcode_token, Sm4Opcode opcode, u32 sample_controls);
    [[nodiscard]] bool translate_compare_convert_operation(
        u32 opcode_token, Sm4Opcode opcode, u32 sample_controls);
    [[nodiscard]] bool translate_resource_query_operation(
        u32 opcode_token, Sm4Opcode opcode, u32 sample_controls);
    [[nodiscard]] bool translate_typed_resource_operation(
        u32 opcode_token, Sm4Opcode opcode, u32 sample_controls);
    [[nodiscard]] bool translate_buffer_operation(
        u32 opcode_token, Sm4Opcode opcode, u32 sample_controls);
    [[nodiscard]] bool translate_atomic_operation(
        u32 opcode_token, Sm4Opcode opcode, u32 sample_controls);
    [[nodiscard]] bool translate_sample_operation(
        u32 opcode_token, Sm4Opcode opcode, u32 sample_controls);
    /// oStencilRef: FragStencilRefEXT.
    u32 m_stencil_output{0};
    /// SV_ClipDistance components in clip-array order, {register, component},
    /// and the ClipDistance builtin the epilogue copies them into. The
    /// register itself stays an ordinary varying, which is how a pixel
    /// shader that reads SV_ClipDistance receives it.
    std::vector<std::pair<u32, u32>> m_clip_sources;
    u32 m_clip_distance_output{0};
    /// SV_RenderTargetArrayIndex written by a vertex shader: {register,
    /// component} and the Layer builtin.
    std::pair<u32, u32> m_layer_source{};
    u32 m_layer_output{0};
    std::pair<u32, u32> m_viewport_source{};
    u32 m_viewport_output{0};
    bool m_derivative_control_capability_emitted{false};
    std::set<u32> m_allocated_temps;
};

} // namespace vitreo::graphics::dxbc_spirv_detail
