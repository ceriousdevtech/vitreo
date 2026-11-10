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
#include "raster_emulation_types.hpp"

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
//
// Shader model 5.1 allows more than one unbounded range of a class. They
// differ in register space or base register, and each is its own descriptor
// array. A run of kDxbcUnboundedRange covers every register from its base,
// but a later run of the same class and space owns the registers from its
// own base: that range's binding is the later run's first_slot, not an
// element of the earlier array. The binding is the table slot the frontend
// assigned, not the register number.

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
    // ---- WS5 begin ----
    /// True when the geometry shader was lowered for a mesh pipeline
    /// (DxbcTranslationOptions::emulate_geometry_with_mesh).
    bool gs_lowered_to_mesh{false};
    // ---- WS5 end ----
    /// A D3D12 mesh / amplification shader (mesh_stage.hpp): its
    /// DxbcMeshState::Kind, 0 otherwise. The module is GLCompute; only
    /// spirv_to_msl with MslCompileOptions::mesh_stage compiles it.
    u32 mesh_stage{0};
    // ---- WS5 begin ----
    // ---- WS5 end ----

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

    /// Pixel shader: each v#'s dcl_input_ps interpolation mode
    /// (D3D10_SB_INTERPOLATION_MODE: 1 constant, 2 linear, 3 centroid,
    /// 4 noperspective, 5 noperspective centroid, 6 sample, 7 noperspective
    /// sample), 0 when undeclared -- what a raster-emulation pass C places
    /// its attributes by (raster_emulation.hpp).
    std::array<u8, 32> pixel_input_interpolation{};
    /// Pixel shader: bit n set when GetAttributeAtVertex (VX_BARY
    /// AttributeAtVertex) reads v#n, whose three vertices' values the stage
    /// before must deliver at per_vertex_input_location(n, 0..2).
    u32 per_vertex_inputs{0};
    /// Pixel shader translated for the raster emulation: SV_Barycentrics
    /// read from pass C's varyings instead of Metal's barycentric_coord
    /// (whose triangle is pass C's region, not the D3D primitive). Bit 0
    /// perspective (kRasterBaryLocation), bit 1 noperspective
    /// (kRasterBaryNoPerspectiveLocation).
    u32 raster_barycentrics{0};

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
    /// The u# (5.1: the UAV range ID) of each float typed UAV the shader runs
    /// an atomic on, declared as a 32-bit unsigned image
    /// (DxbcTranslationOptions::typed_uav_uint_views): the frontend binds
    /// an R32_UINT view of the R32_FLOAT resource there.
    std::vector<u32> typed_uav_uint_view_registers;
    /// The failure is the program's, not the translator's: the spec
    /// calls it invalid (frontends return E_INVALIDARG, not E_NOTIMPL).
    bool invalid_program{false};

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
    /// The shader reads a heap window's auxiliary region
    /// (kDescriptorHeapWindowAuxBinding): its window table must have
    /// 2 * (limit + 1) slots, slot limit + 1 + k holding heap slot k's
    /// residency companion or UAV counter.
    bool heap_window_aux{false};

    [[nodiscard]] bool valid() const {
        return ok && !spirv.empty();
    }
};

/// How a pulled vertex element's bits become register lanes (D3D11.3 3.2
/// "Data Conversion"). `Uniform` formats have `components` channels of
/// `bits` each, little-endian in the element.
enum class VertexPullKind : u8 {
    None = 0,
    Float,        ///< 32-bit float channels
    Half,         ///< 16-bit float channels
    UInt,
    SInt,
    UNorm,
    SNorm,
    UNorm1010102, ///< R10G10B10A2_UNORM
    UInt1010102,  ///< R10G10B10A2_UINT
    Float111110,  ///< R11G11B10_FLOAT
    UNormBGRA8,   ///< B8G8R8A8_UNORM: bytes are B, G, R, A
    // Formats with no Metal vertex format, which only the pull reads
    // (Learn DXGI_FORMAT: components named from the lowest bits).
    UNormB5G6R5,    ///< B5G6R5_UNORM: B 0-4, G 5-10, R 11-15
    UNormB5G5R5A1,  ///< B5G5R5A1_UNORM: B 0-4, G 5-9, R 10-14, A 15
    UNormB4G4R4A4,  ///< B4G4R4A4_UNORM: B 0-3, G 4-7, R 8-11, A 12-15
    UNormBGRX8,     ///< B8G8R8X8_UNORM: bytes B, G, R, unused (alpha 1)
};

/// One input-layout element fed to v#n by the vertex shader's own fetch
/// (DxbcTranslationOptions::vertex_pull). The element's slot entry in the
/// pull table (VERTEX_PULL_TABLE_*) supplies the address, stride and size.
struct VertexPullElement {
    VertexPullKind kind{VertexPullKind::None};
    u8 components{0};  ///< 1..4 (packed kinds: 3 or 4 as the format has)
    u8 bits{0};        ///< 8, 16 or 32 per channel for uniform kinds
    u8 slot{0};        ///< D3D input slot, 0..31
    bool per_instance{false};
    u32 offset{0};     ///< AlignedByteOffset, resolved
    u32 step_rate{0};  ///< InstanceDataStepRate (per-instance only)
    /// The lanes' storage type: 1 UINT, 2 SINT, 3 FLOAT.
    u8 storage_type{3};
};

/// The vertex-pull table: one entry per D3D input slot, bound at
/// BACKEND_VERTEX_BUFFER_BASE of the vertex stage ({address lo, address hi,
/// stride, bytes from the address}); an unbound slot is all zero.
inline constexpr u32 VERTEX_PULL_TABLE_SLOTS = 32;
inline constexpr u32 VERTEX_PULL_TABLE_ENTRY_WORDS = 4;
/// The entry after the slots: {start instance is fixed (1) or the draw's
/// base instance (0), that fixed StartInstanceLocation, instance bias, 0}.
/// A capture that draws one instance at a time (first instance shifted, or
/// SV_InstanceID biased) names the real StartInstanceLocation and instance
/// number here, so per-instance elements still step as in the real draw:
/// instance = InstanceIndex - BaseInstance + bias.
inline constexpr u32 VERTEX_PULL_TABLE_HEADER = VERTEX_PULL_TABLE_SLOTS;
inline constexpr u32 VERTEX_PULL_TABLE_ENTRIES = VERTEX_PULL_TABLE_SLOTS + 1;
/// Descriptor set 4 binding of the table in a vertex shader.
inline constexpr u32 VERTEX_PULL_TABLE_BINDING = 1;

/// Optional host-interface specialization for a concrete D3D input layout.
/// Entries use D3D_REGISTER_COMPONENT_TYPE values (1=UINT, 2=SINT, 3=FLOAT);
/// zero retains the DXBC signature type. D3D permits an input-layout storage
/// category to differ from the shader signature with a compatibility warning,
/// while Metal requires the stage-in type to match its vertex format exactly.
/// The translator therefore fetches using this storage type and preserves the
/// resulting 32-bit lanes in the untyped SM4/5 register representation.
struct DxbcTranslationOptions {
    std::array<u8, 32> vertex_input_component_types{};

    /// UAVs are declared in every stage (vertex, hull, domain, geometry,
    /// mesh, amplification), not only pixel and compute. Direct3D 12 allows
    /// them at every feature level (Learn, "Hardware Tiers": "Max UAVs in
    /// all stages" is 8 / 64 / full heap by resource binding tier, from FL
    /// 11_0). Off, the D3D11.0 rule applies: Vitreo's D3D11 devices stop at
    /// 11_0, where a vs_5_0 with a UAV is E_INVALIDARG.
    bool uavs_in_every_stage{false};

    /// A rasterized vertex shader's SV_CullDistance outputs are ordinary
    /// varyings: the caller culls (a vertex capture, then a pass C that
    /// discards each primitive whose distance is negative at every vertex,
    /// D3D11.3 15.4.2), since Metal has no cull distances. Off, such a
    /// shader fails to translate.
    bool vertex_cull_distances_culled_by_caller{false};

    /// Vertex-shader input registers (bit n = v#n) the input layout feeds from
    /// DXGI_FORMAT_R10G10B10A2_UINT. Metal fetches that packing only
    /// normalized (BackendVertexFormat::UInt1010102), so these inputs are
    /// declared float4 at the stage-in and a prologue restores the integers,
    /// v * (1023, 1023, 1023, 3), into the register the shader reads.
    u32 vertex_input_unpack_uint1010102{0};

    /// Vertex pulling: when set, the vertex shader has NO stage-in. Every
    /// v#n with vertex_pull[n].kind != None is a Private register a prologue
    /// fills from the pull table (VERTEX_PULL_TABLE_*): per-vertex elements
    /// at SV_VertexID's fetch index (vertex id incl. base vertex), per-
    /// instance ones at StartInstanceLocation + instance / step rate (rate 0:
    /// StartInstanceLocation), converted per D3D11.3 3.2. An element not
    /// wholly inside the slot's size reads 0 in every channel (measured,
    /// d3d12_a51); a channel the format lacks reads the default (0, 0, 0, 1).
    /// Used when
    /// an input layout reads more slots than Metal has vertex buffer indices.
    bool vertex_pulling{false};
    std::array<VertexPullElement, 32> vertex_pull{};

    /// Lower a geometry shader to a COMPUTE shader that expands primitives
    /// into a buffer, because Metal has no geometry stage.
    ///
    /// See .docs/design-geometry-shader-emulation.md. Off by default: a
    /// geometry shader still translates to a SPIR-V Geometry stage, which is
    /// what the existing tests assert and what a backend with a real geometry
    /// stage would want.
    bool emulate_geometry_with_compute{false};
    // ---- WS5 begin ----
    /// With emulate_geometry_with_compute: lower for a Metal mesh pipeline
    /// (host-offload plan WS5 D2). The body
    /// is the compute lowering unchanged except that its output records and
    /// per-invocation counts go to Workgroup scratch of
    /// gs_mesh_invocations_per_group invocations, and `main` runs the body,
    /// a workgroup barrier, then the kMeshGsEpilogue marker that
    /// spirv_to_msl expands into the metal::mesh writes. Single stream only.
    bool emulate_geometry_with_mesh{false};
    u32 gs_mesh_invocations_per_group{0};
    // ---- WS5 end ----
    /// A lowered D3D12 mesh shader (DxbcContainer::mesh): which outputs to
    /// stage and where (mesh_stage.hpp). Null: SV_Position and the system
    /// values only (no pixel-shader linkage).
    const struct MeshStageLayout* mesh_layout{nullptr};
    // ---- WS5 begin ----
    // ---- WS5 end ----

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
    /// Pixel shader behind a mesh shader: component mask of each v# whose
    /// components are fed partly by mesh vertex outputs and partly by
    /// primitive outputs (DirectX-Specs "Mesh Shader": pixel inputs "must be
    /// matched to output elements from the mesh shader by the semantic name,
    /// system value type and semantic index"; "Elements must no longer be
    /// linked by the packed location"). The masked components arrive flat at
    /// primitive_split_input_location(n), the rest at location n.
    std::array<u8, 32> pixel_primitive_split{};
    /// Pixel shader: the pipeline rasterizes lines, whose third barycentric
    /// weight is "guaranteed to be exactly 0.0" (DXC wiki SV_Barycentrics);
    /// Metal's barycentric_coord leaves a few ulp there, so VX_BARY Coord
    /// writes 0.
    bool line_barycentrics{false};

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
    /// moved by garbage. A descriptor-table translation refuses this option
    /// (the index would be a range id). That translation reads
    /// `typed_buffer_bias_buffer` instead, the same packed entry, one uint
    /// per table slot.
    bool typed_buffer_bias{false};

    /// Descriptor-table element bias of a typed buffer view (Learn,
    /// D3D12_BUFFER_SRV / D3D12_BUFFER_UAV: FirstElement is any element;
    /// the push-block form above is refused for a table). The frontend
    /// writes one `uint` per table slot -- kTypedBufferBiasMask bits of
    /// BackendTextureBuffer::element_bias, plus kTypedBufferAlphaMissing
    /// when the view's format has no alpha, 0 for a null slot or a view
    /// with no bias whose format has alpha -- and binds that buffer at
    /// (set, binding). Slot k of the SRV table is word k of the SRV
    /// buffer, and the same for UAVs. A descriptor-array operand indexes
    /// binding + element, the same index as view_size_buffer and
    /// null_descriptors. ld, ld_uav_typed, store_uav_typed, the typed
    /// atomics and bufinfo add those low bits to the element index and
    /// subtract them from the texture width. Not enabled (the default),
    /// a descriptor-table translation applies no bias, and the frontend
    /// must refuse a view whose texture starts below FirstElement.
    /// Each slot is kTypedBufferEntryWords uints (word 0 the entry above,
    /// words 1-7 a kTypedBufferRaw view's address and format). A flat
    /// shader model 5.0 translation may declare it as well: its bias still
    /// comes from the push block, and the side buffer adds the raw path.
    struct TypedBufferBiasBuffer {
        bool enabled{false};
        bool srv{false};
        u32 srv_set{0};
        u32 srv_binding{0};
        bool uav{false};
        u32 uav_set{0};
        u32 uav_binding{0};
    };
    TypedBufferBiasBuffer typed_buffer_bias_buffer;

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
    /// Pixel shader behind a mesh shader: SV_PrimitiveID is the mesh
    /// shader's per-primitive output ("there is no system generated
    /// primitiveID", DirectX-Specs "Mesh Shader"), which the mesh stage
    /// passes as the flat user varying of the input's register
    /// (mesh_stage.hpp), not the rasterizer's [[primitive_id]].
    bool primitive_id_varying{false};

    /// Pixel shader drawn by a raster-emulation pass C
    /// (graphics/shaders/raster_emulation.hpp): target-independent or
    /// conservative rasterization. The shader reads its triangle's snapped
    /// edge functions (kRasterEdgeLocation..), computes the D3D coverage of
    /// its pixel, demotes the invocation when there is none, and answers
    /// vCoverage / vInnerCoverage with it. Target-independent rasterization
    /// also evaluates centroid inputs and eval_centroid / eval_sample_index
    /// at the forced pattern's samples (D3D11.3 3.5.5, 3.5.6.2); under
    /// conservative rasterization centroid is the pixel centre ("identical
    /// to the corresponding non-centroid interpolation mode").
    RasterEmulationOptions raster_emulation{};

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
    /// D3D11_LOGIC_OP / D3D12_LOGIC_OP value, the same 0..15 order) for each
    /// render target n in `logic_op_targets` (bit n). Logic ops exist "only
    /// on renderable UINT formats"; the frontend names exactly the bound
    /// UINT targets. The shader reads that target's current value through
    /// framebuffer fetch (a SubpassData input at InputAttachmentIndex n,
    /// MSL [[color(n)]]) and writes op(o#n, dst) for that target only.
    /// `logic_op` is the one op every such target uses (D3D11, and D3D12
    /// when every enabled target names the same op). When
    /// `logic_op_per_target` is set, `logic_ops[n]` is target n's op
    /// (D3D12 IndependentBlendEnable: each
    /// D3D12_RENDER_TARGET_BLEND_DESC::LogicOp).
    /// `logic_op_component_masks[n]` holds each component's bits in that
    /// target's format: the source is truncated to them (WARP measured;
    /// 17.7 allows clamp or truncate) and so is the result, which the format
    /// could not hold otherwise. Blending is off (it cannot be combined with
    /// a logic op) and the write mask still applies after the op.
    u32 logic_op{0};
    bool logic_op_per_target{false};
    std::array<u32, 8> logic_ops{};
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
    /// number an array element happens to have. Raw and structured view
    /// bounds for that translation are `view_size_buffer`, and a typed
    /// buffer's element bias is `typed_buffer_bias_buffer`, each indexed by
    /// the descriptor's table slot. Nor does it read the push block's per-s#
    /// MipLODBias / POINT-mip state: see sampler_state_buffer.
    std::vector<DxbcRegisterRemap> register_remap;

    /// Rasterizer-ordered UAVs (dcl_uav_*_rov, OpcodeToken0 bit 17; Learn
    /// "Rasterizer Order Views"): the frontend orders them, so a pixel
    /// shader's _rov declarations translate, and DxbcResolvedBinding keeps
    /// `declaration.rasterizer_ordered` for it to hand to the MSL compile
    /// (MslCompileOptions::raster_ordered_bindings, Metal raster order
    /// group 0). Off, a descriptor-table translation refuses them and a
    /// D3D11 one ignores the bit, exactly as before. A _rov declaration in
    /// any stage but the pixel shader is refused either way (fxc accepts
    /// RasterizerOrdered* only in ps_5_x).
    bool rasterizer_ordered_views = false;

    /// Float typed UAVs that take atomics (Learn, Typed unordered access view
    /// loads / D3D12_FORMAT_SUPPORT2_UAV_ATOMIC_EXCHANGE: InterlockedExchange
    /// on an R32_FLOAT view; InterlockedCompareStore / CompareExchange of
    /// floats compare bits, D3D12 shader model 6.6 "*FloatBitwise"). Metal's
    /// texture atomics take 32-bit integer images only, so such a u# is
    /// declared as a uint image whose texels the shader moves as float bits,
    /// and the frontend binds an R32_UINT view there
    /// (DxbcToSpirvResult::typed_uav_uint_view_registers). Off, an atomic on
    /// a float typed UAV is refused, as before.
    bool typed_uav_uint_views = false;

    /// A descriptor-table translation's sampler state that Metal samplers do
    /// not carry, which the shader applies itself as the D3D11 translation
    /// does from its push block: MipLODBias (D3D11.3 7.18.11: added to the
    /// LOD of sample, sample_b, sample_l, sample_d, sample_c, sample_c_lz and
    /// lod), a POINT mip filter's round-half-up of an explicit LOD (7.18.10),
    /// a border colour Metal cannot express (7.18.9 / 7.18.9.1, the twin
    /// blend) and a MINIMUM_ / MAXIMUM_ reduction (5.9.4.5.6). The frontend
    /// fills a read-only buffer of kSamplerStateRecordWords-word records --
    /// record k for the sampler at binding k of the samplers' descriptor set,
    /// which must be one set -- and binds it at (descriptor_set, binding), in
    /// a D3D12 frontend a slot of one of its tables. A PSO exists before any
    /// sampler descriptor does, so the sample opcode branches on the record's
    /// flags instead of a per-s# bitset. Border twins, when the record asks,
    /// are extra samplers in set 2 at border_twin_binding_base + binding.
    /// Not enabled (the default), a descriptor-table translation applies
    /// none of this, and the frontend must refuse a sampler whose state it
    /// cannot honour.
    struct SamplerStateBuffer {
        bool enabled{false};
        u32 descriptor_set{0};
        u32 binding{0};
    };
    SamplerStateBuffer sampler_state_buffer;

    /// Descriptor-table raw and structured view sizes (Learn,
    /// ByteAddressBuffer / StructuredBuffer: a read past the view returns 0,
    /// a write is dropped; D3D11.3 22.4.1 bufinfo: bytes for a raw view,
    /// structures for a structured one). The frontend fills one `uint` per
    /// table slot -- the view's size in 32-bit words, 0 for a null or empty
    /// view, so every access is out of bounds -- and binds that buffer at
    /// (set, binding). Slot k of the SRV table is word k of the SRV buffer,
    /// and the same for UAVs. A descriptor-array operand indexes
    /// binding + element. Not the push-block guard: that one is keyed by a
    /// D3D11 register and treats 0 as "size unknown".
    struct ViewSizeBuffer {
        bool enabled{false};
        bool srv{false};
        u32 srv_set{0};
        u32 srv_binding{0};
        bool uav{false};
        u32 uav_set{0};
        u32 uav_binding{0};
    };
    ViewSizeBuffer view_size_buffer;

    /// Descriptor-table constant-buffer limits (D3D11.3 7.5: a read at an
    /// element past the buffer bound at that slot returns 0 in every
    /// component; an empty slot reads 0 everywhere). The frontend fills one
    /// `uint` per CBV-table slot -- the number of 16-byte vectors the view
    /// holds, 0 for a null or empty view -- and binds it at (set, binding).
    /// A descriptor-array operand indexes binding + element. The push-block
    /// guard stays the D3D11 path: it is keyed by register and treats an
    /// untagged 0 as "size unknown".
    struct ConstantBufferLimitBuffer {
        bool enabled{false};
        u32 descriptor_set{0};
        u32 binding{0};
    };
    ConstantBufferLimitBuffer constant_buffer_limits;

    /// Per-slot null descriptors (D3D11.3 22.4.14 / 22.4.1: resinfo and
    /// bufinfo on an unbound view return 0 in every component, including the
    /// mip count). The frontend writes 1 for a null or missing descriptor and
    /// 0 for a real one, one `uint` per table slot, and binds that buffer at
    /// (set, binding). A descriptor-array operand indexes binding + element.
    /// The query must not load the image when the flag is set: a cleared
    /// argument-buffer slot is not a zero-size texture.
    struct NullDescriptorFlags {
        bool enabled{false};
        bool srv{false};
        u32 srv_set{0};
        u32 srv_binding{0};
        bool uav{false};
        u32 uav_set{0};
        u32 uav_binding{0};
    };
    NullDescriptorFlags null_descriptors;

    /// SV_ViewID (dxbc_vitreo_ext::kOpViewId): the view instance this
    /// translation runs for. View instancing is draw-level looping: each
    /// view has its own pipeline, translated with its index.
    u32 view_id{0};
    /// D3D12 resource binding tiers 2 and 3 (Learn, "Hardware Tiers": SRVs,
    /// and at tier 3 CBVs and UAVs, in all descriptor tables per stage =
    /// "full heap"): a t#, u# or b# descriptor array whose register remap
    /// places it in `descriptor_set` reads the shader-visible CBV/SRV/UAV
    /// heap itself instead of a per-draw copy of it. The frontend declares
    /// each such array as a run of register_count kDescriptorHeapWindowLength
    /// at its own first_slot (binding) of that set, and keeps one argument
    /// buffer per heap whose slot k is heap descriptor k, plus one sentinel
    /// null slot at the heap's end. Every window array of a shader aliases
    /// that one argument buffer (spirv_to_msl: Metal 3 argument-buffer
    /// aliasing), so a shader may have any number of them, of any type.
    ///
    /// `arrays` names each window array and the pieces of it the root
    /// signature supplies: a segment covers elements [first_element,
    /// first_element + count) (count kDxbcUnboundedRange: every element from
    /// first_element on) and maps element e to heap slot
    /// params[base_word] + (e - first_element). params is the uint buffer at
    /// (params_set, params_binding); its word 0 is the heap's descriptor
    /// count `limit`, and the frontend writes each segment's first heap slot
    /// (never above limit) at its base_word. An element no segment covers,
    /// or one whose slot is at or past limit, reads slot `limit`, the
    /// sentinel (D3D12: such an index is undefined; this keeps it inside the
    /// buffer). The first segment that covers an element wins.
    ///
    /// The view-size, null-flag, typed-bias and constant-buffer-limit
    /// lookups of a window element read the heap's side buffer at
    /// (side_set, side_binding) instead of the per-table buffers:
    /// kDescriptorHeapWindowSideWords uints per heap slot, word
    /// kDescriptorHeapWindowEntryWord the view-size / typed-bias entry of an
    /// SRV or UAV (bind_view's return) or the vector count of a CBV, and
    /// word kDescriptorHeapWindowNullWord 1 for a null descriptor. Off (the
    /// default), nothing changes.
    struct DescriptorHeapWindow {
        bool enabled{false};
        u32 descriptor_set{0};
        u32 params_set{0};
        u32 params_binding{0};
        u32 side_set{0};
        u32 side_binding{0};
        /// Tiled status of a window buffer element: a residency buffer in
        /// the kTiledResidency* layout whose header is indexed by the
        /// element's record id (side word kDescriptorHeapWindowResidencyWord,
        /// 0 for a view of a resource that is not tiled).
        u32 residency_set{0};
        u32 residency_binding{0};
        struct Segment {
            u32 first_element{0};
            u32 count{0};
            u32 base_word{0};
        };
        struct Array {
            DxbcRegisterClass register_class{DxbcRegisterClass::ShaderResource};
            u32 space{0};
            u32 first_register{0};
            std::vector<Segment> segments;
        };
        std::vector<Array> arrays;
    };
    DescriptorHeapWindow heap_window;
    /// Tiled resources tier 2 (D3D11.3 5.9.4.5): the status the _s forms of
    /// gather4, ld, ld2dms, ld_uav_typed, ld_raw, ld_structured, sample_l,
    /// sample_c_lz and the sample*_cl forms write -- 0 when every texel or
    /// byte the access reads is in a mapped tile, a miss (1) otherwise, so
    /// statuses can be ORed (5.9.4.6) -- and check_access_fully_mapped of
    /// it. A resource that is not tiled is always fully mapped; so is an
    /// access outside the view (it reads 0 without touching a tile).
    ///
    /// A texture's status comes from its residency companion: a float
    /// texture with the resource's size, mips, array and tile grid that
    /// reads 1.0 in mapped tiles and 0.0 in unmapped ones (the backend's
    /// set_residency_texture binds it, or a texture of 1.0 for a resource
    /// that is not tiled). t#'s companion is SRV-set binding
    /// srv_companion_base + t#'s binding, u#'s is SRV-set binding
    /// uav_companion_base + u#'s binding, declared with the shader's own dimension. A sample or
    /// gather reads it with the same coordinate, offset, LOD, bias,
    /// gradients and clamp through s#'s status twin -- sampler-set binding
    /// status_sampler_base + s#'s binding: s#'s filter and addressing with
    /// no comparison, no min/max reduction and an opaque white border -- so
    /// the companion's result is 1.0 exactly when every texel the filter
    /// gave weight lies in a mapped tile (the gather's four texels: every
    /// lane 1.0). A load reads the companion texel it would read.
    ///
    /// A buffer view's status comes from its residency record
    /// (kTiledResidency*): one word per table slot of the SRV / UAV class,
    /// the record's word offset in the same buffer or 0 for a view of a
    /// resource that is not tiled, bound at (set, binding). The first and
    /// last byte the access reads name the tiles checked.
    struct TiledStatus {
        bool enabled{false};
        u32 srv_companion_base{0};
        u32 uav_companion_base{0};
        u32 status_sampler_base{0};
        bool srv_residency{false};
        u32 srv_residency_set{0};
        u32 srv_residency_binding{0};
        bool uav_residency{false};
        u32 uav_residency_set{0};
        u32 uav_residency_binding{0};
    };
    TiledStatus tiled_status;

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

/// The program uses a tiled-resources tier-2 instruction (opcodes 219-234:
/// a status form or check_access_fully_mapped), so its translation needs
/// DxbcTranslationOptions::tiled_status when any status operand is real.
[[nodiscard]] bool dxbc_uses_tiled_status(const DxbcContainer& container);

/// DxbcTranslationOptions::tiled_status buffer residency, one buffer per
/// class: word k is table slot k's record offset in words (0 = the view's
/// resource is not tiled, every access mapped). A record:
///   [0] tiles in the resource (64 KiB each)
///   [1] the tile the view starts in
///   [2] the view's start byte within that tile
///   [3] the view's size in units (elements of a typed view, else bytes)
///   [4] bytes per unit (a typed view's element size, else 1)
///   [5...] one bit per tile of the resource, 1 = mapped (bit t of word
///          5 + t / 32)
/// The buffer is at least kTiledResidencyMinWords long, so a slot with no
/// record reads words that exist.
inline constexpr u32 kTiledResidencyTiles = 0;
inline constexpr u32 kTiledResidencyFirstTile = 1;
inline constexpr u32 kTiledResidencyStartByte = 2;
inline constexpr u32 kTiledResidencyViewUnits = 3;
inline constexpr u32 kTiledResidencyUnitBytes = 4;
inline constexpr u32 kTiledResidencyBits = 5;
inline constexpr u32 kTiledResidencyMinWords = 8;

/// DxbcTranslationOptions::heap_window: the declared length of a heap
/// window array -- D3D12_MAX_SHADER_VISIBLE_DESCRIPTOR_HEAP_SIZE_TIER_2
/// (1,000,000, d3d12.h) descriptors plus the sentinel null slot.
inline constexpr u32 kDescriptorHeapWindowLength = 1000000u + 1u;
/// Shader model 6.6 dynamic resources (HLSL ResourceDescriptorHeap[] /
/// SamplerDescriptorHeap[], DXIL createHandleFromHeap) in the SM 5.1 token
/// stream the DXIL lowering writes: an ordinary dcl_* declaration of an
/// unbounded range in register space kDxbcResourceHeapSpace (t#, u#, b#) or
/// kDxbcSamplerHeapSpace (s#) is the whole shader-visible CBV/SRV/UAV or
/// sampler heap, and element e of it is heap slot e. Each range of one
/// space needs its own lower bound (one per resource type the shader
/// reads; the operand's register index is lower + slot, as fxc writes
/// T0[r0.x + lower]). These spaces never come from fxc with a usable root
/// signature: d3d12.h reserves 0xFFFFFFF0 and above
/// (CreateRootSignature fails E_FAIL), so no real range can name them.
/// The D3D12 frontend requires the root signature flag
/// D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED (0x400) /
/// SAMPLER_HEAP_DIRECTLY_INDEXED (0x800) for them.
inline constexpr u32 kDxbcResourceHeapSpace = 0xFFFFFFF0u;
inline constexpr u32 kDxbcSamplerHeapSpace = 0xFFFFFFF1u;
/// The binding of the never-read writable buffer array every window array
/// aliases (dxbc_to_spirv_bindings.cpp declare_heap_window_base). Window
/// arrays take bindings 0, 1, ... below it.
inline constexpr u32 kDescriptorHeapWindowBaseBinding = 0xFFFFu;
/// Side-buffer record of one heap slot (DxbcTranslationOptions::heap_window).
/// Words 0-7 are an SRV's or UAV's typed-buffer entry record as a
/// descriptor table holds it (kTypedBufferEntryWords: word 0 the entry,
/// which is also the view size, or a CBV's vector count; words 1-6 a view
/// read as raw memory, kTypedBufferRaw), so a window element reads them as
/// a table slot does (word 7 an SRV's Shader4ComponentMapping and the
/// shared-exponent flag). Word 8, past the
/// record, is the null flag. Word 9 is the slot's buffer residency record
/// id (heap_window.residency_binding; 0 when the view's resource is not a
/// tiled buffer).
inline constexpr u32 kDescriptorHeapWindowSideWords = 10;
inline constexpr u32 kDescriptorHeapWindowEntryWord = 0;
inline constexpr u32 kDescriptorHeapWindowNullWord = 8;
inline constexpr u32 kDescriptorHeapWindowResidencyWord = 9;
/// A heap window's auxiliary region: when a shader reads it
/// (DxbcToSpirvResult::heap_window_aux) its window table has 2 * (limit +
/// 1) slots and slot limit + 1 + k holds heap slot k's auxiliary
/// descriptor -- a texture view's residency companion (tiled status, in
/// the view's own dimension; a 1.0 texture for one that is not tiled) or a
/// UAV's counter buffer at its CounterOffsetInBytes. The shader declares
/// each window array's companion or counter array at this binding plus the
/// array's own, of length 2 * kDescriptorHeapWindowLength, aliasing the
/// same argument buffer; every window array of such a shader is declared
/// at that length.
inline constexpr u32 kDescriptorHeapWindowAuxBinding = 0x8000u;

/// DxbcTranslationOptions::sampler_state_buffer records, three uvec4s:
///   word 0  MipLODBias (f32 bits)
///   word 1  flags (below)
///   word 2  MinLOD (f32 bits), when kSamplerStateMinMax is set
///   word 3  MaxLOD (f32 bits), when kSamplerStateMinMax is set
///   words 4-7  border rgba (f32). A comparison border is
///             (red, D3D11_COMPARISON_FUNC as f32, 0, 0), the same packing
///             the D3D11 push block uses. Zero when the border is one of
///             Metal's colours and the twin is not used.
///   word 8  the whole filter state, always (kSamplerStateFilterStateWord)
///   word 9  MinLOD (f32 bits), always
///   word 10 MaxLOD (f32 bits), always
///   word 11 0
/// Words 8-10 serve the sampler feedback writes (dxil_lower_feedback.cpp),
/// which compute which mips and texels a sample would read.
inline constexpr u32 kSamplerStateRecordWords = 12;
inline constexpr u32 kSamplerStateBiasWord = 0;
inline constexpr u32 kSamplerStateFlagsWord = 1;
inline constexpr u32 kSamplerStateMinLodWord = 2;
inline constexpr u32 kSamplerStateMaxLodWord = 3;
/// First of the four border floats (words 4-7, the second uvec4).
inline constexpr u32 kSamplerStateBorderWord = 4;
/// The sampler's mip filter is POINT.
inline constexpr u32 kSamplerStatePointMip = 1u << 0;
/// Word 0 includes kPointMipTieNudge (see kPointMipNudgeMaskOffset), which
/// an explicit LOD removes again before rounding.
inline constexpr u32 kSamplerStateBiasNudged = 1u << 1;
/// Border colour Metal cannot express: sample the twin (opaque white, or
/// comparison border 1) and blend. D3D11.3 7.18.9 / 7.18.9.1.
inline constexpr u32 kSamplerStateCustomBorder = 1u << 2;
/// MINIMUM_ or MAXIMUM_ reduction. Filter and the effective MaxAnisotropy
/// are packed into this word and MinLOD and MaxLOD are words 2 and 3 for
/// every sampler (not only reducing ones).
/// D3D11.3 5.9.4.5.6. Gather ignores it; a comparison filter cannot set it.
inline constexpr u32 kSamplerStateMinMax = 1u << 3;
/// D3D12_SAMPLER_FLAG_NON_NORMALIZED_COORDINATES: coordinates are texel
/// units. The sample opcode divides by the view size. Metal samplers stay
/// normalized; BackendSamplerDesc has no such field.
inline constexpr u32 kSamplerStateNonNormalized = 1u << 4;
/// Bits 8-16 of the flags word: the D3D11_FILTER / D3D12_FILTER value
/// (0x1D5 fits). Bits 17-21: effective MaxAnisotropy, the power of two at
/// or below the state's, as the D3D11 min/max path passes it.
inline constexpr u32 kSamplerStateFilterShift = 8u;
inline constexpr u32 kSamplerStateFilterMask = 0x1FFu;
inline constexpr u32 kSamplerStateAnisoShift = 17u;
inline constexpr u32 kSamplerStateAnisoMask = 0x1Fu;
/// Word 8: D3D12_FILTER in bits 0-8, D3D12_TEXTURE_ADDRESS_MODE U / V / W
/// in bits 9-11 / 12-14 / 15-17, MaxAnisotropy (clamped to 16) in bits
/// 18-22. Words 9 / 10: MinLOD / MaxLOD.
inline constexpr u32 kSamplerStateFilterStateWord = 8;
inline constexpr u32 kSamplerStateFullMinLodWord = 9;
inline constexpr u32 kSamplerStateFullMaxLodWord = 10;
inline constexpr u32 kSamplerStateAddressUShift = 9u;
inline constexpr u32 kSamplerStateAddressVShift = 12u;
inline constexpr u32 kSamplerStateAddressWShift = 15u;
inline constexpr u32 kSamplerStateAddressMask = 0x7u;
inline constexpr u32 kSamplerStateMaxAnisoShift = 18u;
/// Bits 22-26 of the flags word (word 1): MaxAnisotropy as the sampler
/// state gives it, 1-16 (the footprint the Metal sampler filters with;
/// VX_TEX LodGrad reads it).
inline constexpr u32 kSamplerStateFlagsMaxAnisoShift = 22u;

/// The fragment input location of an alpha-antialiased line's equation
/// (DxbcTranslationOptions::line_coverage_alpha): past v0..v31, so no
/// pixel-shader register can hold it.
inline constexpr u32 kLineDistanceLocation = 32;
/// The fragment input location of input register @p reg at vertex @p k
/// (0-2) of the pixel's triangle, for GetAttributeAtVertex (VX_BARY
/// AttributeAtVertex): flat copies a pass-through vertex stage writes
/// (gs_passthrough.hpp, per_vertex_registers), past every other location
/// (v0..v31, kLineDistanceLocation, the raster emulation's 33-38).
[[nodiscard]] constexpr u32 per_vertex_input_location(u32 reg, u32 k) { return 40u + reg * 3u + k; }
/// The fragment input location of the per-primitive components of input
/// register @p reg when a mesh shader feeds the register's other components
/// from vertex outputs (DxbcTranslationOptions::pixel_primitive_split):
/// past the per-vertex copies.
[[nodiscard]] constexpr u32 primitive_split_input_location(u32 reg) { return 136u + reg; }
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
/// A descriptor-table typed buffer entry (typed_buffer_bias_buffer) is
/// kTypedBufferEntryWords uints per table slot. Word 0 is the packed entry
/// above. When it carries kTypedBufferRaw the view has no texture: the
/// shader reads and writes the buffer's bytes itself and decodes the DXGI
/// format from words 1-7 (a backend that cannot alias the storage as a
/// texture -- Metal makes no texture of a placement sparse buffer). Words:
/// 1-2 the 64-bit device address of element 0 (low, high), 3 NumElements,
/// 4 the element's bytes, 5 channels R | G << 16, 6 channels B | A << 16
/// (TypedRawChannel), 7 an SRV's Shader4ComponentMapping (the D3D12
/// encoding; D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING for none, unused by a
/// UAV). Bit 16 of word 7 marks a shared-exponent element
/// (R9G9B9E5_SHAREDEXP): channels R, G, B are its 9-bit mantissas, Float
/// kind, scaled by 2^(e - 24) for the exponent e in bits 27-31. The bias
/// bits are 0 for such a view.
inline constexpr u32 kTypedBufferRaw = 0x40000000u;
inline constexpr u32 kTypedBufferEntryWords = 8;
// A heap window's side record carries the same words at the same offsets
// (typed_raw_word), with the null flag in the word after them.
static_assert(kDescriptorHeapWindowSideWords == kTypedBufferEntryWords + 2);
static_assert(kDescriptorHeapWindowNullWord == kTypedBufferEntryWords);
static_assert(kDescriptorHeapWindowResidencyWord == kTypedBufferEntryWords + 1);
inline constexpr u32 kTypedRawSharedExponent = 0x10000u;
/// One channel of a raw typed view: its bit offset in the element (bits
/// 0-6), its width (7-12; 0 when the format lacks the channel, which reads
/// 0, or 1 for alpha), and how its bits convert (13-15).
enum class TypedRawKind : u32 {
    Unorm = 0, Snorm = 1, Uint = 2, Sint = 3, Float = 4, Float16 = 5,
    Float11 = 6, Float10 = 7,
};
[[nodiscard]] constexpr u32 typed_raw_channel(u32 offset, u32 width,
                                              TypedRawKind kind) {
    return (offset & 0x7Fu) | ((width & 0x3Fu) << 7) |
           (static_cast<u32>(kind) << 13);
}
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
// ---- WS5 begin ----
/// The extended-instruction import of a mesh-lowered geometry shader
/// (DxbcTranslationOptions::emulate_geometry_with_mesh). Not NonSemantic.*:
/// these instructions have effects, and only spirv_to_msl's mesh compiler
/// gives them meaning (any other consumer must reject the module).
inline constexpr const char* kMeshGsExtInstSet = "Vitreo.MeshGS";
/// () -> uint: the invocation (input primitive x GS instance) this thread runs.
inline constexpr u32 kMeshGsInvocationIndex = 1;
/// (scratch_var, counts_var, depth_offset) -> void: write the mesh.
inline constexpr u32 kMeshGsEpilogue = 2;
/// () -> uint: the thread's index in its mesh threadgroup.
inline constexpr u32 kMeshGsLocalIndex = 3;
// ---- WS5 end ----
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
/// Member 14 (vertex stages), a uvec4: the render-target array index rule
/// of D3D11.3 17.18 / DirectX-Specs "View Instancing" for an
/// SV_RenderTargetArrayIndex the translated stage writes itself:
///   +0  kLayerLimitKnown | the bound render-target / depth view's slice
///       count (0 without the flag: no rule, the index is stored as is),
///   +4  added to the index first (the view instance's
///       D3D12_VIEW_INSTANCE_LOCATION::RenderTargetArrayIndex).
/// An index (plus the addend) at or past the slice count selects slice 0,
/// where Metal would clamp to the last slice.
inline constexpr u32 kLayerStateOffset = kMinMaxSamplerOffset + kMinMaxSamplerSlots * 16;
inline constexpr u32 kLayerLimitKnown = 0x80000000u;
/// The whole block, padded to 16 bytes.
inline constexpr u32 kPushBlockBytes = kLayerStateOffset + 16;
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
inline constexpr u32 kPushMemberLayerState = 14;
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
