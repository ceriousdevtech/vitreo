// SPDX-License-Identifier: LGPL-2.1-only
// Members of dxil_lower.cpp's Lowerer that dxil_lower_stages.inc (the
// geometry, hull and domain stages, signature access, RDEF) and
// dxil_lower_ops.inc (the remaining dx.op coverage) define. Included inside
// the class body; not a standalone header.

// ---- stages (dxil_lower_stages.inc) ----
/// The stage properties of dx.entryPoints (DXIL.rst "Geometry Shader",
/// "Domain Shader", "Hull Shader"; the GS list measured from dxc 1.6,
/// d3d12_sm6/gs_quads.dxil.txt: input primitive, max vertex count, active
/// stream mask, stream topology, instance count).
struct StageState {
    u32 gs_input_primitive{0};
    u32 gs_max_vertices{0};
    u32 gs_stream_mask{0};
    u32 gs_topology{0};
    u32 gs_instances{1};
    u32 domain{0};
    u32 input_cp{0};
    u32 output_cp{0};
    u32 partitioning{0};
    u32 output_primitive{0};
    u32 max_tess_factor_bits{0};
    const DxilFunction* patch_constant_fn{nullptr};
    bool fork{false};  ///< lowering the patch-constant function
    bool have_gs{false}, have_ds{false}, have_hs{false};
};
StageState stage_;
/// A mesh or amplification shader (dxil_lower_mesh.inc).
struct MeshProgram {
    bool have{false};        ///< its kDxilMSStateTag / kDxilASStateTag
    bool dispatched{false};  ///< DispatchMesh lowered
    u32 max_vertices{0};
    u32 max_primitives{0};
    u32 topology{0};
    u32 payload_bytes{0};
    u32 payload_words{0};
    u32 groupshared_bytes{0};
    std::vector<u32> payload_layout;  ///< DxbcMeshState::payload_layout
};
MeshProgram mesh_;
bool read_mesh_property(u64 tag, i64 value);
/// The spec's limits, checked once the body is lowered.
bool check_mesh_program();
std::vector<u8> write_mesh_chunk();
/// A load of the mesh payload at byte p (byte_layout) of kind @p kind
/// (kDxbcPayload*) into @p d, in the register model.
bool emit_payload_load(const Pointer& p, const Operand& d, u32 kind);
bool emit_mesh_store(bool primitive, const DxilInstruction& inst);
bool emit_mesh_op(u32 opcode, const DxilInstruction& inst, bool& handled);
/// One dx.entryPoints property (tag 1 / 2 / 3); false only on failure.
bool read_stage_property(u64 tag, i64 value);
/// Checks the stage state read_metadata found; false on failure.
bool check_stage_state();
/// Stage declarations (dcl_gs*, dcl_tessellator_*, control point counts)
/// and every signature declaration of the geometry, hull and domain stages.
bool declare_stage();
bool declare_stage_signatures();
/// The vertex count of one geometry input primitive (the v[n][] width).
u32 gs_input_vertices() const;
bool emit_load_input_ext(const DxilInstruction& inst);
bool emit_store_output_ext(const DxilInstruction& inst);
/// LoadInput / LoadOutputControlPoint / LoadPatchConstant and the
/// EvaluateAttribute* inputs: signature row @p row_id (a constant or a
/// value) moved -- or passed through @p emit -- into @p result; a dynamic
/// row selects among the element's rows.
bool load_signature(const std::vector<SigElement>& sig, Sm4OperandType type, u32 id, u32 row_id,
                    u32 col_id, const Index* vertex, u32 result,
                    const std::function<void(const Operand&, const Operand&)>* emit = nullptr);
bool emit_system_value(Sm4OperandType type, u32 comp, const DxilInstruction& inst);
bool emit_stage_op(u32 opcode, const DxilInstruction& inst, bool& handled);
/// The hull shader: the control-point function and the patch-constant
/// function as hs_control_point_phase and one hs_fork_phase.
DxilToDxbcResult run_hull();
/// Lowers fn_ into body_ from a clean register state.
bool lower_function();
void reset_function_state();
/// A constant global array's pointer (an initialized x# array).
bool constant_global(u32 global, Pointer& out);
bool flatten_constant(u32 id, u32 type, std::vector<u32>& out);
/// The RDEF chunk (shader model 5.1 reflection): every binding by name,
/// space and range ID, and each constant buffer's size.
std::vector<u8> write_rdef();
/// A PS input register no signature entry uses (SV_SampleIndex, which DXIL
/// reads through dx.op SampleIndex and SM 5.1 through a register).
u32 sample_index_register_{~0u};

// ---- ops (dxil_lower_ops.inc) ----
/// Ops DXIL.rst defines beyond the core set dxil_lower.cpp lowers; sets
/// @p handled when the op is one of them.
bool emit_op_ext(u32 opcode, const DxilInstruction& inst, bool& handled);
bool emit_sample(u32 opcode, const DxilInstruction& inst);
/// Whether a resource op's ResRet status (member 4) is extracted.
bool status_used(u32 result);
/// Rewrites the resource instruction emitted at @p start as its tiled-
/// resources status form (D3D11.2 _s / _cl_s) when @p result's status is
/// used: the status destination after the destination, and for sample,
/// sample_b, sample_d and sample_c (no _s form without a clamp) the
/// _cl_s form with a clamp that clamps nothing.
bool add_status(usize start, u32 result);
std::set<u32> status_users_;
const DxilFunction* status_scan_fn_{nullptr};
std::map<u32, Slot> status_slots_;
bool emit_gather(u32 opcode, const DxilInstruction& inst);
bool emit_texture_load(const DxilInstruction& inst);
bool emit_dimensions(const DxilInstruction& inst);
bool emit_transcendental(u32 opcode, const DxilInstruction& inst);
/// The texel offset operands (immediate: the sample-controls extended
/// opcode token in @p ext; otherwise a register in @p reg, @p programmable).
bool texel_offsets(const DxilInstruction& inst, usize first, u32 count, u32& ext, bool& programmable,
                   u32& reg);
/// SM 6.7 programmable offsets in @p reg (from texel_offsets): reduced to
/// their low 4 bits, sign-extended ([-8, 7], DirectX-Specs "HLSL Advanced
/// Texture Operations"), in place. With @p prefix, a VX_TEX_OFFSET naming
/// them for the sample instruction emitted next.
void reduce_programmable_offsets(u32 reg, bool prefix);
Operand sampler_component(const Handle& h, u32 channel);
/// Scratch helpers for multi-instruction expansions.
Slot tmp() { return fresh_scalar(); }

// ---- compute-shader quads (dxil_lower_quads.inc) ----
/// SM 6.6 quads of a compute shader (DirectX-Specs "Derivative Operations
/// in Compute, Mesh and Amplification Shaders"): numthreads X % 4 == 0 with
/// Y == Z == 1 is the 1D layout, quad = four consecutive SV_GroupIndex
/// values; X and Y even is the 2D layout, quad = (x, y), (x+1, y),
/// (x, y+1), (x+1, y+1) for even x, y, lane order (0)(1)/(2)(3).
/// A Metal quadgroup is four consecutive thread_index_in_threadgroup
/// values (measured on the M4 Pro over 1D, 2D and 3D threadgroup shapes:
/// simdgroup = index / 32, lane = index % 32, quad lane = lane % 4), so the
/// 1D layout is the hardware's and the 2D layout renumbers the threads:
/// hardware index L is quad L / 4, lane L % 4, quads laid row-major over
/// the (X / 2) x (Y / 2) x Z grid of quads.
enum class QuadLayout : u8 { None, Linear, Grid };
QuadLayout quad_layout_{QuadLayout::None};
/// Decides quad_layout_ from fn_'s operations and numthreads.
bool plan_compute_quads();
/// SV_GroupThreadID.@p comp (0-2), SV_GroupIndex (3) or SV_DispatchThreadID
/// .@p comp (4-6) under the Grid layout, computed into @p d.
void grid_thread_id(u32 which, const Operand& d);
/// d = a - b (VX_F32P under fp32-denorm-mode preserve).
void quad_fsub(const Operand& d, const Operand& a, const Operand& b);
/// DerivCoarseX/Y, DerivFineX/Y (@p kind 0-3) of @p v from the quad.
void quad_derivative(u32 kind, const Operand& d, const Operand& v);
/// The coarse x / y gradients of the coordinate values @p ids (undef ones
/// 0) into two fresh vector registers.
void quad_gradients(const std::vector<u32>& ids, u32& gx, u32& gy);
/// Sample / SampleBias / SampleCmp / CalculateLOD outside the pixel stage
/// from quad gradients; sets @p status_done when the status operand (if
/// used) was written by the instruction itself.
bool emit_quad_sample(u32 opcode, const DxilInstruction& inst, bool& status_done);
/// SampleCmpLevel (224), SampleCmpGrad (254), SampleCmpBias (255): VX_TEX.
bool emit_sample_cmp_ext(u32 opcode, const DxilInstruction& inst);
/// The status destination of a VX_TEX sample (a null operand when unused).
Operand tex_status(u32 result);
// ---- inline ray tracing (dxil_lower_rt.inc) ----
/// AllocateRayQuery .. RayQuery_* (opcodes 178-215).
bool emit_ray_query(u32 opcode, const DxilInstruction& inst);
/// AllocateRayQuery result -> query index, and each query's template flags.
std::map<u32, u32> ray_queries_;
std::vector<u32> ray_query_flags_;
/// The acceleration structure each query traces (TraceRayInline), and the
/// companion blob views made for InstanceContributionToHitGroupIndex.
std::map<u32, Handle> ray_query_as_;
std::map<u32, u32> rq_companions_;
/// One companion per single acceleration structure (after read_resources).
void add_rq_companions();

// ---- raytracing pipelines (dxil_lower_rtpipe.inc) ----
/// Set while lowering one function copy of a megakernel.
RtFunctionContext* rt_{nullptr};
bool emit_rt_op(u32 opcode, const DxilInstruction& inst, bool& handled);
bool rt_memory(const DxilInstruction& inst, bool& handled);
bool emit_trace_ray(const DxilInstruction& inst);
bool emit_report_hit(const DxilInstruction& inst);
bool emit_call_shader(const DxilInstruction& inst);
bool lower_function_at(u32 base);
Slot rt_query(u32 query, dxbc_vitreo_ext::RayQueryOp o, u32 aux = 0);
void rt_query_op(u32 query, dxbc_vitreo_ext::RayQueryOp o, std::initializer_list<const Operand*> ops);
Slot rt_word(u32 range_id, u32 reg, const Operand& byte_offset);
Operand rt_const(u32 word);
void rt_copy(const Pointer& p, u32 array, u32 count, bool into_array);
u32 rt_pointee_words(u32 pointer_value);
void rt_call_switch(const Slot& id, const std::vector<std::pair<u32, u32>>& cases);
Slot rt_record_id(u32 table, const Slot& index, u32 stride_word, u32 count_word, const Slot& rec);
Slot rt_hit_group(u32 query, bool committed, const Handle& as, const Operand& contribution,
                  const Operand& multiplier);
/// Library resources: a load of a resource global (or a GEP into one) ->
/// the handle CreateHandleForLib makes of it.
std::map<u32, Handle> rt_lib_handles_;
std::map<u32, Handle> rt_gep_handles_;
/// Companion range ID -> its t# register (space kDxilRtSpace).
std::map<u32, u32> rt_companion_regs_;
/// A megakernel copy's shared heap-indexed resources (RtHeapResources).
RtHeapResources* rt_heap_{nullptr};
/// Redirect @p h (a library resource) to what this copy's local root
/// signature binds it with (dxil_raytracing.hpp, local root arguments).
bool rt_local_handle(Handle& h);
/// A raw word of the resource range @p range_id at register index @p at.
Slot rt_word_at(u32 range_id, const Index& at, const Operand& byte_offset);
/// The companion (range id, first register) of acceleration structure
/// @p as, ~0u id when it has none.
std::pair<u32, u32> rt_companion_of(const Resource& as);
/// The heap-indexed record of @p like's type (heap_resource).
const Resource* heap_record(Resource like, bool sampler);
