// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file dxbc_to_spirv_internal.hpp
/// @brief DxbcTranslator, shared by the dxbc_to_spirv*.cpp translation units.

#include "graphics/shaders/spirv_to_msl.hpp"
#include "dxbc_to_spirv.hpp"
#include "gs_passthrough.hpp"
#include "mesh_stage.hpp"
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

/// The ids the tiled-status helpers need from the module that calls them
/// (DxbcTranslator fills it from its own types; another front end -- DXIL --
/// supplies its equivalents).
struct TiledStatusTypes {
    u32 type_bool{0};
    u32 type_uint{0};
    u32 type_float{0};
    u32 type_vec4{0};
    u32 type_bvec4{0};
    /// uvec2 / uvec3 and bvec2 / bvec3: texel coordinates and their tests.
    u32 type_uvec2{0};
    u32 type_uvec3{0};
    u32 type_bvec2{0};
    u32 type_bvec3{0};
    /// Pointer to a StorageBuffer uint (a residency word).
    u32 ptr_storage_buffer_uint{0};
};

/// Tiled resources tier 2 status (D3D11.3 5.9.4.5): free helpers over the
/// SPIR-V builder that the DXBC translator uses and the DXIL translator can
/// share. Each returns a uint: 0 when every texel or byte the access reads
/// lies in a mapped tile, 1 (a miss) otherwise. Statuses of a multi-part
/// access are ORed (5.9.4.6). The residency sources are the ones
/// DxbcTranslationOptions::tiled_status describes.
struct TiledStatus {
    /// How the sample being checked selects its LOD.
    struct Lod {
        enum class Kind : u8 { Implicit, Explicit, Gradient } kind{Kind::Implicit};
        u32 bias{0};    ///< Implicit: the Bias operand, 0 for none
        u32 lod{0};     ///< Explicit: the Lod operand
        u32 grad_x{0};  ///< Gradient: the Grad operands
        u32 grad_y{0};
    };
    /// A sample (or, @p gather, a gather of component 0) of the residency
    /// companion through the status twin sampler (@p companion: their
    /// OpSampledImage), with the access's own coordinate, offsets, LOD and
    /// MinLOD clamp (@p min_lod, 0 for none). A miss is a result below
    /// 1.0 -- in any of a gather's four lanes.
    [[nodiscard]] static u32 emit_texture_status(
        SpirvBuilder& b, const TiledStatusTypes& t, u32 companion,
        u32 coordinate, const Lod& lod, u32 const_offset, u32 offset,
        u32 min_lod, bool gather);
    /// A load of texel @p coordinate (a uvec2 or uvec3 of @p components)
    /// from the companion image @p image: mip @p lod_or_sample, or sample
    /// @p lod_or_sample when @p multisampled. A texel outside the image (or
    /// a mip / sample it lacks) reads nothing and is not a miss.
    [[nodiscard]] static u32 emit_texel_status(
        SpirvBuilder& b, const TiledStatusTypes& t, u32 image,
        u32 coordinate, u32 components, u32 lod_or_sample, bool multisampled);
    /// Units @p first .. @p last of the buffer view whose residency header
    /// entry is word @p slot of @p residency (a StorageBuffer block whose
    /// member 0 is a uint array; record layout kTiledResidency*). An access
    /// that starts past the view is not a miss; one that ends past it is
    /// checked up to the view's end.
    [[nodiscard]] static u32 emit_buffer_status(
        SpirvBuilder& b, const TiledStatusTypes& t, u32 residency, u32 slot,
        u32 first, u32 last);
    /// check_access_fully_mapped: 0xFFFFFFFF for status 0, else 0.
    [[nodiscard]] static u32 emit_check_fully_mapped(
        SpirvBuilder& b, const TiledStatusTypes& t, u32 status);
};

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
          m_decoder(container.bytecode.data(), container.bytecode.size()) {
        m_immediate_constants = decode_immediate_constant_buffer(
            container.bytecode, &m_immediate_constants_failure, &m_readonly_indexable_words);
        m_decoder.set_sm51_resource_operands(container.is_sm51_plus());
    }

    DxbcToSpirvResult run();

private:
    [[nodiscard]] bool fail(const std::string& reason) {
        m_result.ok = false;
        m_result.failure = reason;
        return false;
    }
    /// fail() for a program the spec calls invalid (invalid_program).
    [[nodiscard]] bool fail_invalid(const std::string& reason) {
        m_result.invalid_program = true;
        return fail(reason);
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
    /// A `ret`/`retc` (taken) in a subroutine a hull-shader phase inlines:
    /// leave the innermost construct towards the inline frame's merge.
    bool emit_inline_return();
    /// After an ENDLOOP/ENDSWITCH inside an inlined body: a `ret` nested
    /// in that construct keeps leaving the enclosing ones.
    void emit_inline_return_propagation();
    // Double precision (D3D11.3 22.14-22.17): soft_double.hpp helpers, one
    // SPIR-V function each, emitted after main for the ones a shader calls.
    enum class DoubleHelper : u32 {
        Add, Mul, Div, Fma, Min, Max, Eq, Ge, Lt, ToFloat, FromFloat, ToInt,
        ToUint, FromInt, FromUint, Saturate, SaturateFloat, Count
    };
    bool translate_double(Sm4Opcode opcode);
    // Vitreo extension instructions (dxbc_vitreo_ext.hpp): wave / quad
    // operations, 64-bit integers and halves, for lowered DXIL.
    bool translate_vitreo_extension(u32 opcode);
    bool translate_vx_wave(u32 control);
    bool translate_vx_i64(u32 control);
    bool translate_vx_f16(u32 control);
    // D3D12 mesh / amplification shaders (dxbc_to_spirv_mesh.cpp).
    [[nodiscard]] bool mesh_stage() const { return m_container.mesh.kind != DxbcMeshState::None; }
    bool emit_mesh_stage_declarations();
    bool emit_mesh_stage_main(u32 body);
    bool translate_vx_mesh(u32 control);
    u32 mesh_scalar(const Sm4Operand& operand, bool& ok);
    void mesh_if(u32 condition, const std::function<void()>& then);
    void mesh_store_word(u32 var, u32 index, u32 value);
    const MeshStageLayout* m_mesh_layout{nullptr};
    MeshStageLayout m_mesh_default_layout;
    u32 m_mesh_set{0};
    u32 m_mesh_vertices{0};
    u32 m_mesh_primitives{0};
    u32 m_mesh_indices{0};
    u32 m_mesh_counts{0};
    u32 m_mesh_grid{0};
    u32 m_mesh_payload{0};  ///< Workgroup: the payload copy (mesh)
    i32 m_mesh_payload_tgsm{-1};
    /// Inline ray tracing (dxbc_to_spirv_rayquery.cpp).
    bool translate_vx_ray_query(u32 control);
    [[nodiscard]] std::set<u32> scan_acceleration_structures() const;
    bool declare_acceleration_structures();
    u32 rq_type_acceleration_structure();
    /// VX_F32P (SM 6.2 fp32-denorm-mode preserve): dxbc_to_spirv_denorm.cpp.
    bool translate_vx_f32p(u32 control);
    /// VX_TEX (dxbc_to_spirv_vitreo_tex.cpp).
    bool translate_vx_tex(u32 control);
    /// VX_BARY (dxbc_to_spirv_vitreo_bary.cpp).
    bool translate_vx_bary(u32 control);
    u32 m_vx_bary{0}, m_vx_bary_mode{0};
    u32 m_vx_bary_no_perspective{0}, m_vx_bary_no_perspective_mode{0};
    /// VX_TEX_OFFSET: the ivec4 offsets the next sample instruction takes
    /// (0 when none is pending).
    u32 m_vx_dynamic_offset{0};
    /// VX_BARY AttributeAtVertex: register -> its three flat per-vertex inputs.
    std::map<u32, std::array<u32, 3>> m_vx_per_vertex;
    /// Set while VX_TEX SampleCmpGrad runs sample_c with gradients.
    bool m_vx_compare_gradient{false};
    /// Set while VX_TEX SampleCmpLevel runs sample_c_lz with an LOD operand.
    bool m_vx_compare_level{false};
    /// Set while VX_TEX SampleCmpBias runs sample_c with a bias operand.
    bool m_vx_compare_bias{false};
    bool emit_f32p_helpers();
    [[nodiscard]] u32 vx_type_u64();
    [[nodiscard]] u32 vx_type_i64();
    [[nodiscard]] u32 vx_type_half();
    [[nodiscard]] u32 vx_type_u16();
    [[nodiscard]] u32 vx_builtin(SpvBuiltIn builtin, u32 pointer_type, u32 type,
                                 u32& variable, const char* name);
    [[nodiscard]] u32 vx_u64_of(u32 uvec4, u32 first = 0);
    /// This lane's index, the active lanes' ballot of @p condition, and the
    /// lowest active lane (from reductions only: dxbc_to_spirv_vitreo_ext).
    [[nodiscard]] u32 vx_lane();
    [[nodiscard]] u32 vx_ballot(u32 condition);
    [[nodiscard]] u32 vx_first_lane();
    [[nodiscard]] u32 vx_uvec4_of_u64(u32 value);
    /// The subgroup scan / reduction / match loops over every lane (64-bit
    /// values, WaveMultiPrefix*, WaveMatch): dxbc_to_spirv_vitreo_ext.cpp.
    [[nodiscard]] u32 vx_lane_loop(u32 kind, u32 op, u32 type, u32 lo, u32 hi,
                                   u32 mask);
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
        /// Non-zero: an ivec4 of SM 6.7 programmable offsets (VX_TEX_OFFSET,
        /// already in [-8, 7]) used in place of `offset`.
        u32 offset_id{0};
        /// Non-zero: descriptor-table sampler_state record, already loaded.
        /// Zero: the D3D11 push block (kPushMemberMinMaxSamplers, member 0).
        u32 filter_id{0};
        u32 min_lod_id{0};
        u32 max_lod_id{0};
        u32 max_aniso_id{0};
        u32 bias_id{0};
        /// When non-zero, OpSampledImage is emitted in the fetch's own
        /// block (SPIR-V: it cannot cross blocks). The type id is
        /// sampled_image_type. twin_sampler_id is the opaque-white twin.
        u32 sampler_id{0};
        u32 sampled_image_type{0};
        u32 twin_sampler_id{0};
    };
    [[nodiscard]] u32 emit_minmax_sample(const MinMaxSample& sample);
    /// A descriptor-table reduction (filter_id .. bias_id and sampler_id
    /// set) as a call of an out-of-line helper, one per shape: see
    /// dxbc_to_spirv_minmax_sample.cpp. @p image_type is a.image's type.
    [[nodiscard]] u32 emit_minmax_sample_call(const MinMaxSample& sample,
                                              u32 image_type);
    /// The helpers emit_minmax_sample_call named, after every caller.
    bool emit_minmax_helpers();
    struct MinMaxHelper {
        u32 image_type{0};
        u32 sampled_image_type{0};
        u32 coordinate_type{0};
        bool texture_1d{false};
        bool cube{false};
        bool volume{false};
        bool arrayed{false};
        bool explicit_lod{false};
        bool twinned{false};
        u32 function{0};
    };
    std::vector<MinMaxHelper> m_minmax_helpers;
    /// The bool specialization constant kMinMaxReductionSpecName
    /// (graphics/shaders/minmax_reduction.hpp) the table reductions' branch
    /// is ANDed with; made on first use.
    [[nodiscard]] u32 minmax_reduction_gate();
    u32 m_minmax_reduction_gate{0};
    u32 m_custom_border_gate{0};
    u32 m_non_normalized_gate{0};
    /// From @p start_token (2 = the first instruction) to the next `ret`: a
    /// hull-shader phase is scanned from its own first token.
    [[nodiscard]] u32 scan_temp_count(usize start_token = 2) const;
    struct IndexableTempDecl { u32 count; u32 components; };
    [[nodiscard]] std::map<u32, IndexableTempDecl> scan_indexable_temps(
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
    /// The size-table entry of the typed buffer view at t# (@p uav false)
    /// or u# @p slot: the push block when DxbcTranslationOptions::
    /// typed_buffer_bias is set, or the per-slot side buffer when
    /// typed_buffer_bias_buffer declared one for this class. 0 (no id)
    /// when this translation has neither (or a push-block u# past the
    /// table).
    [[nodiscard]] u32 typed_buffer_entry(bool uav, u32 slot);
    // ---- Typed buffer views read as raw memory -------------------------
    // (dxbc_to_spirv_typed_raw.cpp; kTypedBufferRaw.) A descriptor-table
    // translation reads such a view's bytes through a device address and
    // converts its DXGI format itself.
    /// Word @p word of the side-buffer entry of t#/u# @p slot (0 when the
    /// translation has no side buffer for that class).
    [[nodiscard]] u32 typed_raw_word(bool uav, u32 slot, u32 word);
    /// An if (entry is raw) { ... } else { ... } split around existing code.
    struct TypedRawSplit {
        u32 raw_end{0};
        u32 texture_block{0};
        u32 texture_end{0};
        u32 merge{0};
        u32 raw_value{0};
        bool active{false};
    };
    /// Opens the split when this translation can see a raw view at that
    /// slot; the code after it is the raw branch.
    [[nodiscard]] TypedRawSplit typed_raw_begin(bool uav, u32 slot);
    /// Ends the raw branch (with @p raw_value, 0 for none) and opens the
    /// texture branch.
    void typed_raw_else(TypedRawSplit& split, u32 raw_value);
    /// Closes the split; the phi of @p result_type, or 0 when @p
    /// result_type is 0.
    [[nodiscard]] u32 typed_raw_end(TypedRawSplit& split, u32 result_type,
                                    u32 texture_value);
    /// ld / ld_uav_typed of @p element: a 4-vector of @p result_type (the
    /// declared @p return_type, 4 UINT, 3 SINT, else float), past the view
    /// (0, 0, 0, default alpha).
    [[nodiscard]] u32 typed_raw_load(bool uav, u32 slot, u32 element,
                                     u32 return_type, u32 result_type);
    /// store_uav_typed of @p texel (uvec4 bits) at @p element: nothing past
    /// the view.
    void typed_raw_store(u32 slot, u32 element, u32 texel, u32 return_type);
    /// A typed UAV atomic @p op on u# @p slot's @p element (the 32-bit
    /// word at element * element bytes): the previous value, 0 past the
    /// view (nothing is written). @p operand / @p comparator are the uint
    /// sources, already leader-gated (m_uav_leader_var) except for an
    /// exchange, which a non-leader reads with a compare-exchange.
    [[nodiscard]] u32 typed_raw_atomic(u32 slot, u32 element, SpvOp op,
                                       bool compares, u32 operand,
                                       u32 comparator, bool leader_gated,
                                       u32 scope, u32 semantics);
    /// NumElements of the raw view.
    [[nodiscard]] u32 typed_raw_count(bool uav, u32 slot);
    /// A PhysicalStorageBuffer uint pointer at byte @p byte_lo/@p byte_hi
    /// (64-bit) past the view's address: element 0 of an OpPtrAccessChain,
    /// a memory location an atomic can take (a bare bitcast pointer value
    /// is an rvalue in the MSL emission).
    [[nodiscard]] u32 typed_raw_pointer(bool uav, u32 slot, u32 byte_lo,
                                        u32 byte_hi);
    u32 m_type_ptr_physical_uint{0};
    u32 m_type_add_carry{0};
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
    /// `count` (1..4) consecutive words at `base_word`. One test covers the
    /// span: when the last word is inside the view, the loads are plain and
    /// adjacent so MSL can widen them. A span that crosses the view uses the
    /// per-word guard (out-of-view reads 0, and a load must not run past the
    /// allocation).
    void load_guarded_word_span(bool uav, u32 slot, u32 variable,
                                u32 pointer_type, bool indexed_through_member,
                                u32 base_word, u32 count, u32* out_words);
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
    /// The u# operands of every atomic_* / imm_atomic_* (not alloc / consume).
    [[nodiscard]] std::set<u32> scan_uav_atomics() const;
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
    /// What every return of main runs: the vertex capture or domain record
    /// writes of an emulation pass, then the system-value epilogue (not
    /// for a domain pass, which has no rasterizer outputs). An early
    /// ret / retc runs it too, so its vertex or point is still recorded.
    bool emit_return_epilogue();
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
    u32 m_type_uimage_cube{0}, m_type_uimage_cube_array{0};
    u32 m_type_iimage_2d{0}, m_type_iimage_2d_array{0}, m_type_iimage_3d{0};
    u32 m_type_iimage_cube{0}, m_type_iimage_cube_array{0};
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
    u32 m_type_sampled_uimage_cube{0};
    u32 m_type_sampled_uimage_cube_array{0};
    u32 m_type_sampled_iimage_2d{0};
    u32 m_type_sampled_iimage_2d_array{0};
    u32 m_type_sampled_iimage_cube{0};
    u32 m_type_sampled_iimage_cube_array{0};
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
    u32 m_type_ptr_uniform_constant_uimage_cube{0};
    u32 m_type_ptr_uniform_constant_uimage_cube_array{0};
    u32 m_type_ptr_uniform_constant_iimage_2d{0};
    u32 m_type_ptr_uniform_constant_iimage_2d_array{0};
    u32 m_type_ptr_uniform_constant_iimage_3d{0};
    u32 m_type_ptr_uniform_constant_iimage_cube{0};
    u32 m_type_ptr_uniform_constant_iimage_cube_array{0};
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
    struct IndexableTempVar {
        u32 variable; u32 type; u32 components; u32 pointer_type;
        std::vector<u32> readonly_strides{};
    };
    std::map<u32, IndexableTempVar> m_indexable_temp_vars;
    std::map<u32, std::vector<u32>> m_readonly_indexable_words;
    std::map<u32, IndexableTempVar> m_readonly_indexable_vars;
    bool declare_readonly_indexable_arrays();
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
    /// Vertex-pulled inputs (DxbcTranslationOptions::vertex_pulling): the
    /// register, its Private variable and type.
    struct PulledVertexInput {
        u32 reg{0};
        u32 target{0};
        u32 type{0};
    };
    std::vector<PulledVertexInput> m_pulled_vertex_inputs;
    /// The pull table (set 4, VERTEX_PULL_TABLE_BINDING); 0 until declared.
    u32 m_vertex_pull_table{0};
    /// emit_vertex_pull_prologue: fill every pulled register.
    [[nodiscard]] bool emit_vertex_pull_prologue();
    /// One pulled element's four lanes as uint bits.
    [[nodiscard]] u32 vertex_pull_element(const VertexPullElement& element);
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
    /// Bounds-guarded constant-buffer elements at a literal index already
    /// loaded, by (buffer variable, element): the value and where it was
    /// made. A constant buffer cannot change during an invocation, so a
    /// later read the definition dominates (SpirvBuilder::dominates_here)
    /// takes the same value instead of a second guard.
    struct DominatingValue {
        u32 value{0};
        SpirvBuilder::DominanceMark mark;
    };
    std::map<std::pair<u32, u32>, std::vector<DominatingValue>> m_guarded_cb_elements;
    /// Runtime-owned immutable view limits, by (buffer, index SSA ID).
    /// Reuse only a dominating load; descriptor-array indices remain part
    /// of the key and no load is hoisted out of its original branch.
    std::map<std::pair<u32, u32>, std::vector<DominatingValue>> m_view_size_values;
    std::map<u32, u32> m_cbuffer_sizes;   // CB slot -> vec4 count
    std::vector<std::array<float, 4>> m_immediate_constants;
    std::string m_immediate_constants_failure;
    u32 m_immediate_constants_var{0};
    u32 m_immediate_constants_element_ptr{0};
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
    /// Float typed UAVs declared as uint images for their atomics
    /// (DxbcTranslationOptions::typed_uav_uint_views): loads return the
    /// texel's bits as x and (0, 0, 1.0) in yzw.
    std::set<u32> m_uav_float_bits_slots;

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
    /// Acceleration structures (t# range ID -> variable), ray queries and
    /// their flags words (query index -> variable).
    std::map<u32, u32> m_rq_as_vars;
    std::map<u32, u32> m_rq_vars;
    std::map<u32, u32> m_rq_flag_vars;
    u32 m_rq_type_as{0}, m_rq_type_query{0}, m_rq_type_mat43{0};
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
    /// DxbcTranslationOptions::view_size_buffer, and for the instruction
    /// being translated the table slot (binding + element, a uint id) of
    /// each descriptor-array t#/u# operand.
    u32 m_srv_view_size_var{0};
    u32 m_uav_view_size_var{0};
    std::map<u32, u32> m_view_size_index;
    /// DxbcTranslationOptions::constant_buffer_limits.
    u32 m_cbv_limit_var{0};
    /// DxbcTranslationOptions::null_descriptors.
    u32 m_srv_null_var{0};
    u32 m_uav_null_var{0};
    /// DxbcTranslationOptions::typed_buffer_bias_buffer.
    u32 m_srv_typed_bias_var{0};
    u32 m_uav_typed_bias_var{0};
    /// DxbcTranslationOptions::heap_window: the params buffer (limit and
    /// segment bases) and the heap's side buffer, declared on first use.
    u32 m_heap_window_params_var{0};
    u32 m_heap_window_side_var{0};
    /// The writable buffer array every window array aliases (declared
    /// before the first of them, see declare_heap_window_base).
    u32 m_heap_window_base_var{0};
    void declare_heap_window_base();
    /// The window buffer residency buffer (heap_window.residency_binding),
    /// declared on first use.
    u32 m_heap_window_residency_var{0};
    /// The declared length of every window array: twice
    /// kDescriptorHeapWindowLength when the shader may read the auxiliary
    /// region (kDescriptorHeapWindowAuxBinding: tiled status or a UAV
    /// counter), so all of them alias the argument buffer at one length.
    [[nodiscard]] u32 heap_window_length();
    i32 m_heap_window_length{-1};
    /// The heap's descriptor count `limit` (params word 0). 0 after fail().
    [[nodiscard]] u32 heap_window_limit();
    /// Heap slot @p slot's auxiliary slot, limit + 1 + slot.
    [[nodiscard]] u32 heap_window_aux_index(u32 slot);
    /// Declare @p name, an array of the window's length of @p element_pointer
    /// 's pointee in the window set at kDescriptorHeapWindowAuxBinding + @p
    /// binding (storage class @p storage), and note the auxiliary region.
    [[nodiscard]] u32 declare_heap_window_aux(u32 binding, u32 element_type,
                                              SpvStorageClass storage,
                                              const std::string& name);
    /// u# keys whose counter is a heap window's auxiliary counter array.
    std::set<u32> m_window_counter_slots;
    /// The range is a heap window (its run is in heap_window.descriptor_set).
    [[nodiscard]] bool is_heap_window(const ResourceRange& range) const {
        return m_options.heap_window.enabled && range.is_array &&
               range.resolved.descriptor_set ==
                   m_options.heap_window.descriptor_set;
    }
    /// A heap window element's heap slot: the slot the first segment of
    /// @p range that covers @p element maps it to, or the sentinel slot
    /// `limit` (DxbcTranslationOptions::heap_window). 0 after fail().
    [[nodiscard]] u32 heap_window_slot(const ResourceRange& range, u32 element);
    /// Word @p word of heap slot @p slot's side record. 0 after fail().
    [[nodiscard]] u32 heap_window_side_word(u32 slot, u32 word);
    /// The range @p range_id of the t# or u# class is a heap window.
    [[nodiscard]] bool heap_window_key(bool uav, u32 range_id) const {
        const ResourceRange* range = find_range(
            uav ? DxbcRegisterClass::UnorderedAccess
                : DxbcRegisterClass::ShaderResource,
            range_id);
        return range != nullptr && is_heap_window(*range);
    }
    /// One word of s# `sampler`'s record (sampler_state_buffer).
    [[nodiscard]] u32 sampler_state_word(const Sm4Operand& sampler, u32 word);
    /// The bound view's size in 32-bit words (view_size_buffer). 0 after
    /// fail(). `range_id` is the operand's index0.
    [[nodiscard]] u32 view_size_words(bool uav, u32 range_id);
    /// The CBV-table slot whose limit `src` reads (constant_buffer_limits).
    /// 0 after fail().
    [[nodiscard]] u32 cbv_limit_index(const Sm4Operand& src);
    /// The t#/u# table slot `range_id` names (view sizes and null flags).
    /// 0 after fail().
    [[nodiscard]] u32 descriptor_slot_index(bool uav, u32 range_id);
    /// True when the descriptor is null (null_descriptors). 0 when this
    /// translation has no flag buffer for the class.
    [[nodiscard]] u32 descriptor_is_null(bool uav, u32 range_id);
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
    /// Function ids of the VX_F32P helpers the shader calls (0: unused),
    /// indexed by dxbc_vitreo_ext::F32POp, and their types by source count.
    std::array<u32, 11> m_f32p_helpers{};
    std::array<u32, 4> m_f32p_function_types{};
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
    /// One per subroutine body a hull-shader phase is inlining: the body is
    /// wrapped in a single-iteration loop construct, so a `ret`/`retc`
    /// nested in its flow (D3D11.3 22.7.15/22.7.17: return to the caller)
    /// is a structured break to merge_block. A `ret` inside a loop or switch
    /// of the body sets returned_var (Private uint, 0 when the body needs
    /// none) and breaks each enclosing construct of the body in turn.
    struct InlineFrame {
        u32 merge_block{0};
        u32 returned_var{0};
        usize breakable_base{0};
        usize loop_base{0};
    };
    std::vector<InlineFrame> m_inline_stack;
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
    // ---- WS5 begin ----
    /// A geometry shader lowered for a mesh pipeline (WS5 D2).
    [[nodiscard]] bool gs_as_mesh() const {
        return gs_as_compute() && m_options.emulate_geometry_with_mesh;
    }
    bool emit_geometry_mesh_main(u32 body);
    u32 m_gs_mesh_set{0};             ///< OpExtInstImport kMeshGsExtInstSet
    u32 m_gs_mesh_counts_var{0};      ///< Workgroup uint[K]
    u32 m_gs_mesh_local_var{0};       ///< Private: index in the threadgroup
    u32 m_gs_mesh_invocation_var{0};  ///< Private: invocation from main
    u32 m_gs_mesh_threads{0};         ///< K
    // ---- WS5 end ----
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
    bool declare_function_temps(usize start_token);
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
    /// DxbcTranslationOptions::raster_emulation (dxbc_to_spirv_raster.cpp):
    /// pass C's flat per-triangle inputs, the fragment position they are
    /// evaluated at (m_re_frag_coord_declared when this translation declared
    /// it), and the Private results the prologue computes -- the coverage
    /// (vCoverage), InnerCoverage, and the centroid's offset from the pixel
    /// centre.
    std::array<u32, 3> m_re_edges{};
    /// RasterPrimitive::Quad: the fourth edge. edges_in_buffer: the edge
    /// buffer and Metal's primitive id (declared here when the shader has
    /// no SV_PrimitiveID of its own).
    u32 m_re_edge3{0};
    u32 m_re_edge_buffer{0};
    u32 m_re_primitive_id{0};
    u32 m_re_primitive_id_declared{0};
    u32 m_re_bounds{0};
    u32 m_re_inverse_w{0};
    u32 m_re_depth{0};
    u32 m_re_frag_coord{0};
    u32 m_re_frag_coord_declared{0};
    u32 m_re_mask{0};
    u32 m_re_inner{0};
    u32 m_re_centroid_offset{0};
    /// Centroid inputs under target-independent rasterization: the
    /// interpolant (Input, no centroid decoration) and the Private register
    /// the prologue fills with it at the forced pattern's centroid. Pull-
    /// model evaluation reads the interpolant (m_re_interpolants by v#).
    struct RasterCentroidInput {
        u32 source{0};
        u32 target{0};
    };
    std::vector<RasterCentroidInput> m_re_centroid_inputs;
    /// DxbcTranslationOptions::pixel_primitive_split: the register's Private
    /// shadow the prologue fills from its vertex-fed and primitive-fed inputs.
    struct PrimitiveSplitInput {
        u32 vertex_var{0};
        u32 primitive_var{0};
        u32 shadow{0};
        u32 value_type{0};
        u8 mask{0};
    };
    std::vector<PrimitiveSplitInput> m_primitive_split_inputs;
    std::map<u32, u32> m_re_interpolants;
    [[nodiscard]] bool raster_emulated() const {
        return m_result.stage == ShaderStage::Fragment &&
               m_options.raster_emulation.mode != RasterEmulationMode::Off;
    }
    [[nodiscard]] bool raster_target_independent() const {
        return raster_emulated() && m_options.raster_emulation.mode ==
                                        RasterEmulationMode::TargetIndependent;
    }
    void declare_raster_emulation_inputs();
    bool emit_raster_emulation_prologue();
    /// The forced pattern's offset of sample @p index (an int id), in
    /// pixels from the centre, as MSL interpolate_at_offset takes it.
    [[nodiscard]] u32 raster_sample_offset(u32 index);
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
    // Vitreo extension instructions: types and builtins, made on first use,
    // and the globals they add to the entry point's interface.
    u32 m_vx_u64{0}, m_vx_i64{0}, m_vx_half{0}, m_vx_u16{0};
    u32 m_vx_subgroup_size{0}, m_vx_subgroup_lane{0}, m_vx_helper{0};
    std::vector<u32> m_vx_interface;
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

    // Tiled resources tier 2 (dxbc_to_spirv_tiled.cpp).
    /// The instruction being translated is a status form: its status
    /// destination (decoded after the destination) and MinLOD clamp.
    struct TiledInstruction {
        bool active{false};
        bool clamp{false};
        bool has_status{false};
        Sm4Operand status;
        /// Why the status operand was refused; the handler's own "bad
        /// operands" failure would hide it (translate_instruction).
        std::string error;
    };
    TiledInstruction m_tiled;
    /// t# / u# range -> its residency companion variable, s# -> its status
    /// twin, and the buffer residency headers (DxbcTranslationOptions::
    /// tiled_status).
    std::map<u32, u32> m_srv_companion_vars;
    std::map<u32, u32> m_uav_companion_vars;
    std::map<u32, u32> m_status_sampler_vars;
    u32 m_srv_residency_var{0};
    u32 m_uav_residency_var{0};
    [[nodiscard]] TiledStatusTypes tiled_status_types() const;
    [[nodiscard]] bool declare_tiled_status_variables();
    /// After the destination of a status form: its status operand.
    [[nodiscard]] bool decode_tiled_status();
    /// The _cl forms' last operand, as a float id (0 for any other form).
    [[nodiscard]] bool decode_tiled_clamp(u32& min_lod);
    /// Write @p miss (a uint) to the status destination, if any.
    [[nodiscard]] bool store_tiled_status(u32 miss);
    [[nodiscard]] bool translate_check_access_fully_mapped();
    /// The loaded companion of t# (@p uav false) or u#, with its
    /// sampled-image type and whether it is multisampled. 0 after fail().
    [[nodiscard]] u32 tiled_companion(const Sm4Operand& resource, bool uav,
                                      u32& sampled_type, bool& multisampled);
    /// s#'s status twin, loaded.
    [[nodiscard]] u32 tiled_status_sampler(const Sm4Operand& sampler);
    /// Status of units @p first .. @p last of a buffer view.
    [[nodiscard]] u32 tiled_buffer_status(bool uav, u32 range_id, u32 first,
                                          u32 last);
    /// Status of a texture load from t# / u# at @p coordinate (uint vector
    /// of @p components) and mip or sample @p lod_or_sample.
    [[nodiscard]] u32 tiled_texel_status(const Sm4Operand& resource, bool uav,
                                         u32 coordinate, u32 components,
                                         u32 lod_or_sample);
    /// Status of the words a raw or structured load reads: those the
    /// destination mask selects through @p swizzle, from @p base_byte.
    [[nodiscard]] u32 tiled_words_status(bool uav, u32 range_id, u32 base_byte,
                                         u32 write_mask, u32 swizzle);
    /// @p coordinate (a uvec2 / uvec3 texel address) plus the ld / ld2dms
    /// immediate offset in @p sample_controls: u, v (not for a Texture1D)
    /// and, for a volume, w; an array slice takes none.
    [[nodiscard]] u32 tiled_offset_texel(u32 coordinate, u32 components,
                                         u32 sample_controls, bool texture_1d,
                                         bool volume);
};

} // namespace vitreo::graphics::dxbc_spirv_detail

namespace vitreo::graphics {
/// The tiled-resources status helpers (dxbc_to_spirv_tiled.cpp), for the
/// DXIL translator too.
using dxbc_spirv_detail::TiledStatus;
using dxbc_spirv_detail::TiledStatusTypes;
}  // namespace vitreo::graphics
