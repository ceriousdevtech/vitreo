// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file dxbc_to_spirv.hpp
/// @brief Direct3D 11 shader model 4–5 bytecode (DXBC) to SPIR-V translator.
///
/// Translates SM4/SM5 shader bytecode from the SHDR/SHEX chunk to SPIR-V binary.
/// This is a clean-room implementation based on public Microsoft documentation:
/// - D3D11 Shader Model 4/5 instruction set reference
/// - D3D11 shader bytecode encoding specification
/// - SPIR-V specification 1.6
///
/// The translator refuses to emit SPIR-V for opcodes/features it cannot express
/// correctly. Unsupported operations fail honestly with a diagnostic message
/// naming the missing construct.

#include "shader_types.hpp"
#include "dxbc_parser.hpp"

#include <array>
#include <string>
#include <vector>

namespace vitreo::graphics {

// =============================================================================
// Register remap (the D3D12 binding seam)
// =============================================================================
//
// A translated resource lives at a SPIR-V (descriptor set, binding). Without
// a remap every class keeps its own set -- 0 = b#, 1 = t#, 2 = s#, 3 = u#
// (BACKEND_MAX_DESCRIPTOR_SETS' register spaces) -- and binding = register,
// which is what Direct3D 11 and the flat binding windows need; register
// space 0 is the only space that model has, so a shader model 5.1 program
// that declares another space must be translated with a remap.
//
// D3D12 states its layout in the root signature: each descriptor range (and
// root descriptor) covers a run of registers of one class in one space, and
// the frontend chooses where each run lands in its descriptor tables. One
// DxbcRegisterRemap entry is such a run. A declared array must land on
// consecutive bindings of one set (the descriptor array of SPIR-V and of a
// Metal argument buffer), which a run -- or runs that continue each other
// in both register and binding -- provides.

struct DxbcRegisterRemap {
    DxbcRegisterClass register_class{DxbcRegisterClass::ShaderResource};
    u32 space{0};
    u32 first_register{0};
    /// Registers covered; kDxbcUnboundedRange for every register from
    /// first_register on (a D3D12 range of UINT_MAX descriptors).
    u32 register_count{1};
    u32 descriptor_set{0};
    /// The binding of first_register; register first_register + k is
    /// binding first_slot + k.
    u32 first_slot{0};
};

/// One declared binding as translated: where it landed and how many
/// descriptors the SPIR-V declares from there.
struct DxbcResolvedBinding {
    DxbcBindingDeclaration declaration;
    u32 descriptor_set{0};
    u32 binding{0};
    /// 1 for a single register; the array length for a range; 0 for an
    /// unbounded range the remap does not bound -- a runtime descriptor
    /// array, which the MSL compile sizes from the set's table
    /// (MslArgumentBufferSet::slot_count).
    u32 descriptor_count{1};
};

// =============================================================================
// Translation Result
// =============================================================================

struct DxbcToSpirvResult {
    bool ok{false};
    /// Set when ok==false: the construct that has no translation.
    std::string failure;

    /// SPIR-V binary module (u32 words).
    std::vector<u32> spirv;

    /// Shader stage detected from bytecode.
    ShaderStage stage{ShaderStage::Vertex};

    /// Shader model version.
    u32 shader_model_major{0};
    u32 shader_model_minor{0};

    /// Compute thread-group dimensions declared by dcl_thread_group.
    u32 workgroup_size_x{0};
    u32 workgroup_size_y{0};
    u32 workgroup_size_z{0};

    /// Geometry-shader declarations, needed by the host to size the expansion
    /// buffer and pick a primitive type before it can dispatch. Zero for every
    /// other stage. `gs_input_primitive` is the vertex count of one input
    /// primitive (1 point, 2 line, 3 triangle, 4 line with adjacency, 6
    /// triangle with adjacency); `gs_output_topology` carries
    /// the D3D10_SB_PRIMITIVE_TOPOLOGY value the shader declared.
    u32 gs_input_primitive{0};
    u32 gs_output_topology{0};
    u32 gs_max_output_vertices{0};
    /// dcl_gsinstances: invocations per input primitive (1 when undeclared).
    u32 gs_instance_count{0};
    /// dcl_stream m0..m3 (bit n); bit 0 alone for a shader that declares
    /// none. Set for every geometry shader.
    u32 gs_stream_mask{0};
    /// vec4 slots per emitted vertex: the record stride of the expansion
    /// buffer is this times 16 bytes. Set when the geometry shader was lowered
    /// to compute.
    u32 gs_output_registers{0};
    /// True when this module is a geometry shader lowered to compute. The
    /// stage reads back as Compute, so this is the only way to tell it from a
    /// shader the guest authored as a compute shader.
    bool gs_lowered_to_compute{false};

    /// Tessellation declarations (hull and domain shaders, 11.17), as
    /// measured with fxc: domain 1 isoline / 2 tri / 3 quad; partitioning
    /// 1 integer / 2 pow2 / 3 fractional_odd / 4 fractional_even; output
    /// primitive 1 point / 2 line / 3 triangle_cw / 4 triangle_ccw.
    u32 tess_domain{0};
    u32 tess_partitioning{0};
    u32 tess_output_primitive{0};
    u32 hs_input_control_points{0};
    /// Hull: its output control points. Domain: the control points it reads
    /// (which must be the hull shader's output count).
    u32 hs_output_control_points{0};
    float hs_max_tess_factor{64.0f};
    /// Record strides of the hull kernel's output buffers, in vec4s: one slot
    /// per output register number up to the highest declared, so a record
    /// slot IS the register number and the domain shader needs no layout.
    u32 hs_cp_record_vec4s{0};
    u32 hs_pc_record_vec4s{0};
    bool hs_lowered_to_compute{false};
    bool ds_lowered{false};
    /// A domain shader lowered to the software tessellator's compute kernel
    /// (DxbcTranslationOptions::domain_to_compute); its record layout is
    /// capture_registers.
    bool ds_lowered_to_compute{false};

    /// True when this vertex shader was translated with output capture on.
    bool captures_vertex_outputs{false};
    /// vec4 slots per captured vertex record: the output register count.
    u32 capture_record_vec4s{0};
    /// The output register each record slot holds, in slot order (ascending
    /// register number). Stream-out resolves a declaration's semantic to a
    /// register through the output signature and then to its slot here.
    std::vector<u32> capture_registers;
    /// The u# register of each hidden UAV counter the shader moves
    /// (imm_atomic_alloc / imm_atomic_consume), in binding order: entry k is
    /// bound at descriptor set SPIRV_UAV_COUNTER_DESCRIPTOR_SET, binding k.
    std::vector<u32> uav_counter_registers;
    /// True when the shader performs an atomic on a typed UAV, which
    /// SPIR-V spells with OpImageTexelPointer. SPIRV-Cross lowers that to
    /// a native Metal texture atomic only at MSL 3.1 and above; below that
    /// it emulates it with a side buffer the backend would have to bind, so
    /// the consumer must raise its MSL version when this is set.
    bool uses_typed_uav_atomics{false};

    /// Constant buffer bindings used by the shader.
    std::vector<DxbcResourceBinding> cbuffer_bindings;
    /// Texture bindings (t# registers).
    std::vector<DxbcResourceBinding> texture_bindings;
    /// Sampler bindings (s# registers).
    std::vector<DxbcResourceBinding> sampler_bindings;
    /// Every binding the program declares (DxbcContainer::
    /// binding_declarations, all shader models), with the descriptor set and
    /// binding it was translated to. With a register remap -- or a 5.1
    /// program -- the three lists above carry the binding in bind_point and
    /// the descriptor count in bind_count instead of D3D11 registers.
    std::vector<DxbcResolvedBinding> bindings;
    /// Parallel to uav_counter_registers: the register space of each
    /// counter's UAV (0 before shader model 5.1).
    std::vector<u32> uav_counter_spaces;

    [[nodiscard]] bool valid() const {
        return ok && !spirv.empty();
    }
};

/// Optional host-interface specialization for a concrete D3D input layout.
/// Entries use D3D_REGISTER_COMPONENT_TYPE values (1=UINT, 2=SINT, 3=FLOAT);
/// zero retains the DXBC signature type. D3D permits an input-layout storage
/// category to differ from the shader signature with a compatibility warning,
/// while Metal requires the stage-in type to match its vertex format exactly.
/// The translator therefore fetches using this storage type and preserves the
/// resulting 32-bit lanes in the untyped SM4/5 register representation.
struct DxbcTranslationOptions {
    std::array<u8, 32> vertex_input_component_types{};

    /// Vertex-shader input registers (bit n = v#n) the input layout feeds from
    /// DXGI_FORMAT_R10G10B10A2_UINT. Metal fetches that packing only
    /// normalized (BackendVertexFormat::UInt1010102), so these inputs are
    /// declared float4 at the stage-in and a prologue restores the integers,
    /// v * (1023, 1023, 1023, 3), into the register the shader reads.
    u32 vertex_input_unpack_uint1010102{0};

    /// Lower a geometry shader to a COMPUTE shader that expands primitives
    /// into a buffer, because Metal has no geometry stage.
    ///
    /// See .docs/design-geometry-shader-emulation.md. Off by default: a
    /// geometry shader still translates to a SPIR-V Geometry stage, which is
    /// what the existing tests assert and what a backend with a real geometry
    /// stage would want.
    bool emulate_geometry_with_compute{false};

    /// Lower tessellation for a backend whose tessellator is fed by compute
    /// (Metal; .docs/design-tessellation-emulation.md): a HULL shader becomes
    /// a compute kernel running every phase per patch and writing control
    /// points, patch constants and Metal-format tessellation factors to
    /// buffers; a DOMAIN shader becomes a TessellationEvaluation module
    /// reading those buffers. `gs_input_record_registers` is the hull
    /// kernel's input record layout, as for a geometry shader.
    bool emulate_tessellation{false};

    /// With `emulate_tessellation`, the software-tessellator form
    /// (.docs/design-tessellation-emulation.md, "exact path"): a HULL shader
    /// writes its tessellation factors as raw float32 (kTessFactorWords per
    /// patch, maxtessfactor-clamped, D3D11.3 10.13) for Vitreo's port of the
    /// D3D11 reference tessellator, and a DOMAIN shader becomes a compute
    /// kernel -- one thread per domain point the tessellator generated --
    /// that writes every output register to a record buffer (the layout of
    /// a vertex capture), which the rasterizer pass-through, stream output
    /// and a geometry shader then read.
    bool domain_to_compute{false};

    /// Pass A of the same emulation: translate a VERTEX shader that, in
    /// addition to its normal outputs, writes each output register into the
    /// expansion input buffer at `vertex_index`. The geometry pass then reads
    /// those records.
    ///
    /// This keeps pass A an ordinary draw, so hardware vertex fetch, the input
    /// layout and the vertex buffers all work untouched -- the alternative,
    /// running the vertex shader as compute, would mean emulating attribute
    /// fetch and format decode for every DXGI vertex format.
    bool capture_vertex_outputs_to_buffer{false};

    /// With `capture_vertex_outputs_to_buffer`, for an indirect draw: key
    /// each record by the draw-relative vertex -- SV_VertexID's value,
    /// VertexIndex - BaseVertex (D3D11.3 8.16) -- plus (InstanceIndex -
    /// BaseInstance) times the per-instance key stride, both read from a
    /// 16-byte header the GPU resolve wrote at the start of the capture
    /// buffer ({stride, capacity}); records follow the header, and a key
    /// outside the stride or the capacity is not written
    /// (graphics/shaders/ia_resolve.hpp).
    bool capture_draw_relative{false};

    /// A VERTEX shader that rasterizes as usual AND, like
    /// `capture_vertex_outputs_to_buffer` with `capture_draw_relative`, writes
    /// every output register into the capture buffer, keyed draw-relative
    /// through the buffer's 16-byte {stride, capacity} header. The drawn
    /// vertex shader is the only one that runs, so none of its side effects
    /// repeat. Used while a PIPELINE_STATISTICS query is building: the
    /// clipper statistics are counted from the captured positions.
    bool capture_rasterized_vertex_outputs{false};

    /// A PIXEL shader that counts PSInvocations into the statistics record
    /// bound at descriptor set SPIRV_STATISTICS_DESCRIPTOR_SET, binding 0
    /// (graphics/shaders/pipeline_stats.hpp, counter STAT_PS_INVOCATIONS), on
    /// entry -- before any discard, and before the depth and stencil tests,
    /// which a shader writing memory runs after (WARP measured: rejected and
    /// discarded pixels count; [earlydepthstencil] ones do not). On a
    /// single-sample target it counts every lane of each 2x2 quad holding a
    /// covered pixel, helper pixels included (WARP: 4224 for a 64x64 quad
    /// of two triangles); on a multisampled one (push member 5 > 1), where
    /// a memory-writing fragment function runs per sample and helper lanes
    /// carry no coverage, each covered pixel once (its lowest covered
    /// sample's invocation), helpers excluded -- D3D11.3 20.4.7 allows both.
    bool count_pixel_invocations{false};

    /// With `emulate_geometry_with_compute`: the record layout the vertex
    /// stage's capture actually wrote -- the output register each vec4 slot
    /// holds, in slot order (DxbcToSpirvResult::capture_registers of the
    /// bound vertex shader).
    ///
    /// Empty means "the geometry shader's own input registers", which is only
    /// right when the vertex shader writes exactly those. A vertex shader that
    /// writes one register more gives every record a different stride, and
    /// the geometry shader reads its neighbour's data without failing.
    std::vector<u32> gs_input_record_registers;

    /// s0..s15 whose sampler state has a border colour Metal cannot express
    /// (bit n = s#n). Each sample through one is taken twice -- through s#n,
    /// bound with a transparent-black border, and through its twin at sampler
    /// binding `border_twin_binding_base` + n with an opaque-white one -- and
    /// blended as `lo + (hi - lo) * border[n]`. `hi - lo` is exactly the
    /// weight the filter gave the border, so this is exact for any filter.
    /// The colours come from the push-constant block: member 1 for s0..s7,
    /// member 11 (kBorderHighOffset) for s8..s15.
    ///
    /// A comparison (sample_c, sample_c_lz, gather4_c) through a twinned s#
    /// compares against the border's red itself (D3D11.3 7.18.9.1): s# is
    /// bound with border 0 and the twin with border 1, both comparing, and
    /// the colour slot holds (red, D3D11_COMPARISON_FUNC, 0, 0). The two
    /// compared against a reference the function separates 0 from 1 by
    /// differ by exactly the border's weight (gather: in the border lanes),
    /// which is then moved onto the comparison against the red.
    u32 border_twin_samplers{0};
    /// Where the twins bind in set 2. The flat binding model has the s0..s7
    /// window and puts twins at Metal samplers 8..15
    /// (BACKEND_BORDER_TWIN_SAMPLER_BASE); a descriptor-table shader reads
    /// all sixteen s# from its sampler table, so its twins follow them there
    /// at 16 + n.
    u32 border_twin_binding_base{8};

    /// s0..s15 bound to a sampler state with a MINIMUM_ / MAXIMUM_ filter
    /// (bit n = s#n; D3D11.3 5.9.4.5.6, D3D11_OPTIONS1.MinMaxFiltering).
    /// Metal's sampler reduction does not reduce on every GPU, so sample,
    /// sample_b, sample_l and sample_d through such a slot compute it: the
    /// texels the D3D filter weights -- 7.18.7-7.18.11's addressing, mip
    /// selection and LOD, with WARP's 8-bit texel-weight and LOD fractions
    /// (a zero-weight texel does not count) -- are fetched one by one at
    /// their centres and explicit mips through s#n, which the frontend binds
    /// POINT-filtered with the state's address modes and border (the border
    /// twin completing a colour Metal lacks), and reduced per component
    /// (minNum / maxNum). The filter, MinLOD, MaxLOD and MaxAnisotropy come
    /// from push member kPushMemberMinMaxSamplers; MipLODBias from member 0.
    /// Gather ignores the filter and comparison filters cannot reduce, so
    /// neither changes.
    u32 minmax_samplers{0};

    /// t0..t31 bound to an R32G32B32 typed buffer view (bit n = t#n): a
    /// Buffer<> format every FL 10+ device loads (Learn, "Format support for
    /// Direct3D Feature Level 11.0 hardware"), with no Metal texel format.
    /// The frontend binds such a view as an R32 texture buffer of three
    /// texels per element; `ld` reads the three and completes the element
    /// as (x, y, z, 1) -- D3D11.3 §19.1, a format's missing alpha reads 1 --
    /// or (0, 0, 0, 1) past the view (measured,
    /// d3d11_a03_rgb32_buffer_probe), and `bufinfo` reports texels / 3.
    u32 rgb32_buffer_srvs{0};

    /// Typed buffer views (Buffer<T> t#, RWBuffer<T> u#) are bound from an
    /// aligned offset at or below their FirstElement (Learn,
    /// D3D11_BUFFER_SRV / D3D11_BUFFER_UAV: FirstElement is any element;
    /// BackendTextureBuffer::element_bias), and the frontend puts each one's
    /// element bias in its slot's entry of the push block's size table
    /// (member 2: t# at slot, u# at kGuardSizeSrvSlots + slot; a typed
    /// view's entry is kTypedBufferBiasMask bits of bias, plus
    /// kTypedBufferAlphaMissing when the view's format has no alpha). Every
    /// typed-buffer access then addresses element index + bias of the bound
    /// texture, whose width is bias + NumElements: ld (rgb32 views
    /// included), ld_uav_typed, store_uav_typed and the atomics. An index at
    /// or past NumElements is tested on the index itself -- the bias never
    /// wraps it into the elements in front of the view -- and nothing is
    /// read or written for it: ld and ld_uav_typed return 0 in the format's
    /// components and the default in its missing ones, alpha 1 (Learn ld:
    /// "returns 0 in all non-missing components ... and the default for
    /// missing components", D3D11.3 22.4.6 and 19.1.3.3; ld_uav_typed: "the
    /// same as the ld instruction"), a store writes nothing (Learn
    /// store_uav_typed), an atomic writes nothing and an imm_atomic returns
    /// 0 (Learn imm_atomic_*: nothing written, the return undefined; D3D11.3
    /// 7.13: every atomic behaves alike). bufinfo reports width - bias,
    /// NumElements. A u# past the table (u8+) has no entry: no bias, and the
    /// frontend must not bind a biased view there.
    /// Off by default: a frontend that does not fill -- and bind, for every
    /// stage that runs such a shader -- the block must not have its indices
    /// moved by garbage.
    bool typed_buffer_bias{false};

    /// t0..t31 declared Texture2DMS / Texture2DMSArray and bound to a view
    /// of a ONE-sample texture (bit n = t#n): legal (Learn,
    /// D3D11_TEX2DMS_SRV names no sample count; the view itself is the
    /// multisample shape) and something Metal cannot bind as texture2d_ms.
    /// Such a t# is declared texture2d / texture2d_array over the view's one
    /// mip instead: ld2dms reads that texel whatever the sample index
    /// (measured on WARP: indices 1 and 3 return sample 0,
    /// d3d11_view_line_probe [ms1]), sampleinfo is 1, samplepos (0, 0), and
    /// resinfo reports one mip.
    u32 single_sample_ms_srvs{0};

    /// Pixel shader of an alpha-antialiased line drawn as its rectangle
    /// (graphics/shaders/gs_passthrough.hpp, generate_line_quad_vertex_msl;
    /// D3D11.3 3.4.4: the coverage "is multiplied into the Pixel Shader
    /// output o0.a"): reads the line's equation (n.x, n.y, c) in window
    /// pixels, flat at location kLineDistanceLocation, and multiplies o0.w
    /// by clamp(0.675 - 0.45 d, 0, 1), d = |n.p + c| the pixel position's
    /// distance from the line -- the coverage WARP computes (measured,
    /// d3d11_a07_rs_probe `R7 lines alpha-AA`, d3d11_view_line_probe
    /// [line]).
    bool line_coverage_alpha{false};
    /// Pixel shader of lines drawn as rectangles (two Metal triangles a
    /// line, six a wireframe triangle): SV_PrimitiveID is Metal's primitive
    /// id divided by this. 0 = unchanged.
    u32 line_quad_primitive_divisor{0};

    /// Enforce D3D's out-of-bounds contract on raw and structured buffer
    /// views (Learn, ByteAddressBuffer / StructuredBuffer: a read past the
    /// view returns 0, a write is dropped) from the view sizes the frontend
    /// puts in the push block's third member (words per slot, 0 = unknown,
    /// no check): entry t for t0..t127, then kGuardSizeSrvSlots + u for
    /// u0..u7. Off by default: a frontend that does not fill -- and BIND, for
    /// every stage that runs a guarded shader -- the block must not have its
    /// buffers guarded against garbage sizes. A Texture3D u#'s entry is its
    /// W-slice window instead (kVolumeUavWindowSizeShift).
    bool guard_buffer_bounds{false};

    /// The frontend fills push block members 4-7 (kFixedFunctionOffset) for
    /// every draw and binds the block to every stage that runs: a rasterized
    /// vertex stage (VS, lowered DS) then adds the clip-space depth offset
    /// to its position, and a pixel shader clamps an SV_Depth output to the
    /// viewport depth range. Off by default: a caller that binds no block
    /// must not have its stages read one.
    bool fixed_function_push{false};

    /// Enforce the constant-buffer bounds: a cb# read at an element past what
    /// is bound at that slot returns 0 in all components (D3D11.3 7.5 "Out of
    /// bounds access to ConstantBuffers returns 0 ... with respect to the
    /// size of the buffer bound at that slot"; a slot with no buffer reads 0
    /// everywhere), where "bound" is the *SetConstantBuffers1 window
    /// intersected with the buffer (5.3.4.3.2). The frontend puts each
    /// slot's readable element count in push member 12
    /// (kConstantBufferLimitOffset), tagged with kConstantBufferLimitKnown;
    /// an untagged entry (a block the frontend did not fill) is not checked.
    bool guard_constant_buffers{false};

    /// Pixel shader: the output-merger logic op (D3D11.3 17.7, a
    /// D3D11_LOGIC_OP value) for each render target n in `logic_op_targets`
    /// (bit n). Logic ops exist "only on renderable UINT formats"; the
    /// frontend names exactly the bound UINT targets. The shader reads the
    /// target's current value through framebuffer fetch (a SubpassData input
    /// at InputAttachmentIndex n, MSL [[color(n)]]) and writes op(o#n, dst).
    /// `logic_op_component_masks[n]` holds each component's bits in that
    /// target's format: the source is truncated to them (WARP measured;
    /// 17.7 allows clamp or truncate) and so is the result, which the format
    /// could not hold otherwise. Blending is off (it cannot be combined with
    /// a logic op) and the write mask still applies after the op.
    u32 logic_op{0};
    u32 logic_op_targets{0};
    std::array<std::array<u32, 4>, 8> logic_op_component_masks{};

    /// (class, space, register) -> (descriptor set, binding); see
    /// DxbcRegisterRemap. Empty: the per-class sets with binding = register,
    /// space 0 only.
    ///
    /// A remapped (or shader model 5.1) translation serves a descriptor-table
    /// frontend, and the options keyed by a D3D11 register -- the per-s#
    /// border twins and min/max samplers, the t# view shapes
    /// (rgb32_buffer_srvs, single_sample_ms_srvs) and the push-block bounds
    /// guards -- are refused for it rather than applied to whatever register
    /// number an array element happens to have. Nor does it read the push
    /// block's per-s# MipLODBias / POINT-mip state: see
    /// sampler_state_buffer.
    std::vector<DxbcRegisterRemap> register_remap;

    /// A descriptor-table translation's sampler state that Metal samplers do
    /// not carry, which the shader applies itself as the D3D11 translation
    /// does from its push block: MipLODBias (D3D11.3 7.18.11: added to the
    /// LOD of sample, sample_b, sample_l, sample_d, sample_c, sample_c_lz and
    /// lod) and a POINT mip filter's round-half-up of an explicit LOD
    /// (7.18.10). The frontend fills a read-only buffer of
    /// kSamplerStateRecordWords-word records -- record k for the sampler at
    /// binding k of the samplers' descriptor set, which must be one set --
    /// and binds it at (descriptor_set, binding), in a D3D12 frontend a slot
    /// of one of its tables. Not enabled (the default), a descriptor-table
    /// translation applies neither, and the frontend must refuse a sampler
    /// with a MipLODBias or a POINT mip filter it cannot honour.
    struct SamplerStateBuffer {
        bool enabled{false};
        u32 descriptor_set{0};
        u32 binding{0};
    };
    SamplerStateBuffer sampler_state_buffer;

    /// Pixel shader: write the fragment's coverage as the blend state's
    /// SampleMask (push block member 5), ANDed with the shader's own oMask
    /// when it writes one. D3D11.3 17.17: SampleMask "is always applied",
    /// on single-sample targets too, where bit 0 gates the write. A variant:
    /// a coverage-writing fragment function is only worth it when the mask
    /// clears a sample the target has. Requires fixed_function_push.
    bool apply_sample_mask{false};
};

/// What a shader's bytecode allows its compiler to do with float math
/// (D3D11.3 22.3.2, 22.19.2): refactoring only under dcl_globalFlags
/// refactoringAllowed, and never on an instruction marked precise.
struct DxbcMathContract {
    bool refactoring_allowed{false};
    /// Any instruction (not a declaration) carries a precise mask.
    bool uses_precise{false};
};
[[nodiscard]] DxbcMathContract dxbc_math_contract(
    const DxbcContainer& container);

/// DxbcTranslationOptions::sampler_state_buffer records: word 0 the
/// sampler's MipLODBias (f32 bits), word 1 flags, words 2-3 zero.
inline constexpr u32 kSamplerStateRecordWords = 4;
inline constexpr u32 kSamplerStateBiasWord = 0;
inline constexpr u32 kSamplerStateFlagsWord = 1;
/// The sampler's mip filter is POINT.
inline constexpr u32 kSamplerStatePointMip = 1u << 0;
/// Word 0 includes kPointMipTieNudge (see kPointMipNudgeMaskOffset), which
/// an explicit LOD removes again before rounding.
inline constexpr u32 kSamplerStateBiasNudged = 1u << 1;

/// The fragment input location of an alpha-antialiased line's equation
/// (DxbcTranslationOptions::line_coverage_alpha): past v0..v31, so no
/// pixel-shader register can hold it.
inline constexpr u32 kLineDistanceLocation = 32;
/// The bounds guard's size table (push block member 2, byte offset 192): one
/// u32 per shader-resource slot (D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT)
/// then one per unordered-access slot (D3D11_PS_CS_UAV_REGISTER_COUNT).
/// A typed buffer view's entry is its element bias
/// (DxbcTranslationOptions::typed_buffer_bias).
/// A Texture3D u#'s entry is its W-slice window instead (D3D11_TEX3D_UAV:
/// the view's z counts from FirstWSlice and stops before WSize): (WSize <<
/// kVolumeUavWindowSizeShift) | FirstWSlice, 0 for the whole mip, which is
/// what is bound.
inline constexpr u32 kGuardSizeSrvSlots = 128;
/// A typed buffer view's size-table entry
/// (DxbcTranslationOptions::typed_buffer_bias): its element bias in the low
/// bits (less than one 256-byte alignment's worth of elements), and
/// kTypedBufferAlphaMissing when the view's format has no alpha component,
/// so a read past the view returns alpha 1 (D3D11.3 19.1.3.3) rather than 0.
inline constexpr u32 kTypedBufferBiasMask = 0xFFFFu;
inline constexpr u32 kTypedBufferAlphaMissing = 0x80000000u;
inline constexpr u32 kGuardSizeUavSlots = 8;
inline constexpr u32 kVolumeUavWindowSizeShift = 16;
inline constexpr u32 kGuardSizeTableOffset = 192;
inline constexpr u32 kGuardPushBlockBytes =
    kGuardSizeTableOffset + (kGuardSizeSrvSlots + kGuardSizeUavSlots) * 4;
/// Member 3: added to SV_InstanceID by a vertex-capture variant. The geometry,
/// stream-output and tessellation emulations capture instance k of an
/// instanced draw as its own one-instance draw at StartInstanceLocation + k,
/// so the per-instance fetch is right but InstanceIndex - BaseInstance is 0;
/// this word restores k (D3D11 DrawInstanced: SV_InstanceID counts from 0
/// across the whole draw).
inline constexpr u32 kInstanceIdBiasOffset = kGuardPushBlockBytes;
/// Member 4: bit n set when s#n's sampler state has a POINT mip filter. An
/// explicit LOD (sample_l, sample_c_lz) then selects mip floor(LOD + 0.5) in
/// the shader (D3D11.3 7.18.10), where Metal's nearest-mip would round the
/// tie at .5 down.
inline constexpr u32 kPointMipMaskOffset = kInstanceIdBiasOffset + 4;
/// Member 5 (fragment stage): the sample count of the bound render targets,
/// for sampleinfo/samplepos on `rasterizer` (D3D11.3 22.4.21/22), which
/// Metal exposes to no SPIR-V construct.
inline constexpr u32 kRasterizerSampleCountOffset = kInstanceIdBiasOffset + 8;
/// Member 6: bit n set when the frontend added kPointMipTieNudge to s#n's
/// bias (a point-mip sampler whose min and mag filters match): Apple GPUs
/// move to the next mip only at a fraction of 0.515625, so the nudge makes an
/// implicit LOD's .5 tie round up as D3D11.3 7.18.10 requires. An explicit
/// LOD, which the shader rounds itself, removes it again first.
inline constexpr u32 kPointMipNudgeMaskOffset = kInstanceIdBiasOffset + 12;
inline constexpr float kPointMipTieNudge = 1.0f / 64.0f;
/// Members 7-10: fixed-function state the translated stages apply
/// themselves, filled for every draw.
///   +0  f32  vertex stages: clip-space depth offset (NDC units, added as
///            z += offset * w) that carries the constant part of a UNORM
///            depth bias when the depth buffer is stored as float (D3D11.3
///            15.10); 0 = none.
///   +4  u32  pixel shader: the blend state's SampleMask
///            (DxbcTranslationOptions::apply_sample_mask).
///   +8  f32  pixel shader: viewport MinDepth, and
///   +12 f32  MaxDepth: an SV_Depth output is clamped to them (D3D11.3 15.6:
///            the viewport clamp applies wherever z came from).
inline constexpr u32 kFixedFunctionOffset = kInstanceIdBiasOffset + 16;
/// Member 11: the border colours of s8..s15 for the border twins
/// (DxbcTranslationOptions::border_twin_samplers), eight float4s; member 1
/// holds s0..s7.
inline constexpr u32 kBorderHighOffset = kFixedFunctionOffset + 16;
/// Member 12: per cb# slot, the number of 16-byte elements a shader may read
/// (DxbcTranslationOptions::guard_constant_buffers): the *SetConstantBuffers1
/// window (NumConstants, 4096 for a whole-buffer bind) intersected with the
/// buffer past FirstConstant, 0 for an empty slot, OR kConstantBufferLimitKnown.
/// One entry per D3D11_COMMONSHADER_CONSTANT_BUFFER_HW_SLOT_COUNT (15) slot,
/// padded to 16.
inline constexpr u32 kConstantBufferLimitOffset = kBorderHighOffset + 8 * 16;
inline constexpr u32 kConstantBufferLimitSlots = 16;
inline constexpr u32 kConstantBufferLimitKnown = 0x80000000u;
/// Member 13: per s# slot, a MINIMUM_ / MAXIMUM_ sampler's state for the
/// shader-side reduction (DxbcTranslationOptions::minmax_samplers), a uvec4:
/// {D3D11_FILTER, MinLOD (f32 bits), MaxLOD (f32 bits), effective
/// MaxAnisotropy}. Zero for any other slot.
inline constexpr u32 kMinMaxSamplerOffset =
    kConstantBufferLimitOffset + kConstantBufferLimitSlots * 4;
inline constexpr u32 kMinMaxSamplerSlots = 16;
/// The whole block, padded to 16 bytes.
inline constexpr u32 kPushBlockBytes =
    kMinMaxSamplerOffset + kMinMaxSamplerSlots * 16;
/// A compute-lowered domain shader's threads per group, and its control
/// block (set 4 binding 2, u32 words): [5] control-point record stride,
/// [6] patch-constant record stride, both in vec4s, [7] domain points.
inline constexpr u32 kDomainComputeThreads = 64;
inline constexpr u32 kDomainControlCpStride = 5;
inline constexpr u32 kDomainControlPcStride = 6;
inline constexpr u32 kDomainControlPointCount = 7;
/// A hull kernel's control block (set 4 binding 2): [1]-[4] as a geometry
/// kernel's (how a patch's control points are found), [5] / [6] its output
/// strides, and for an indirect draw [8] flags (bit 0: patches past [9]
/// return; bit 1: SV_PrimitiveID is the patch modulo [10]).
inline constexpr u32 kHullControlFlags = 8;
inline constexpr u32 kHullControlPatchLimit = 9;
inline constexpr u32 kHullControlPatchesPerInstance = 10;
inline constexpr u32 kHullControlWords = 16;
/// Words per patch of a hull kernel's raw float factors
/// (domain_to_compute): tri {Ueq0, Veq0, Weq0, inside}, quad {Ueq0, Veq0,
/// Ueq1, Veq1, insideU, insideV}, isoline {density, detail}, as the D3D11
/// reference tessellator takes them.
inline constexpr u32 kTessFactorWords = 8;

/// SPIR-V member indices of the push struct past member 3, which the
/// translator addresses by index while the frontends fill it by byte offset.
inline constexpr u32 kPushMemberPointMipMask = 4;
inline constexpr u32 kPushMemberRasterizerSampleCount = 5;
inline constexpr u32 kPushMemberPointMipNudgeMask = 6;
inline constexpr u32 kPushMemberDepthOffset = 7;
inline constexpr u32 kPushMemberSampleMask = 8;
inline constexpr u32 kPushMemberMinDepth = 9;
inline constexpr u32 kPushMemberMaxDepth = 10;
inline constexpr u32 kPushMemberBorderHigh = 11;
inline constexpr u32 kPushMemberConstantBufferLimits = 12;
inline constexpr u32 kPushMemberMinMaxSamplers = 13;
static_assert(kFixedFunctionOffset >= kPointMipNudgeMaskOffset + 4);

// =============================================================================
// API
// =============================================================================

/// Translate a parsed DXBC container to SPIR-V.
/// The container must have been successfully parsed via parse_dxbc().
/// 
/// @param container  Parsed DXBC container with bytecode, signatures, resources
/// @return Translation result; check valid() for success
/// Iterations a translated loop may run per invocation before its guard
/// breaks out (VITREO_SHADER_LOOP_CAP, default 2^14; the heaviest loop
/// measured needs 1200; 0 disables guards).
/// Stands in for Windows' Timeout Detection and Recovery: a runaway shader
/// loop otherwise pins the GPU until the whole Mac freezes.
[[nodiscard]] u32 shader_loop_iteration_cap();

[[nodiscard]] DxbcToSpirvResult translate_dxbc_to_spirv(
    const DxbcContainer& container,
    const DxbcTranslationOptions& options = {});

} // namespace vitreo::graphics
