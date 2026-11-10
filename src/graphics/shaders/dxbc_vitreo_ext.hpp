// SPDX-License-Identifier: LGPL-2.1-only
#pragma once
/// @file dxbc_vitreo_ext.hpp
/// @brief Vitreo-internal extension instructions of the SM5.1 token stream
/// that the DXIL lowering (dxil_to_dxbc.hpp) emits and the DXBC translator
/// (dxbc_to_spirv_vitreo_ext.cpp) compiles.
///
/// Shader model 6 adds operations SM5.1 has no instruction for: the wave
/// and quad intrinsics (DXIL.rst opcodes 110-123, 135-136, 165-167), 64-bit
/// integer arithmetic (Int64ShaderOps) and native 16-bit floats
/// (Native16BitShaderOpsSupported). The lowering expresses each as one of
/// the instructions below. They never come from fxc or any other producer:
/// D3D10_SB_OPCODE_TYPE (d3d12TokenizedProgramFormat.hpp) ends at
/// D3D11_SB_NUM_OPCODES (0xd8 with the 5.1 additions); OpcodeToken0 bits 0-10
/// hold the opcode, so 0x700-0x7ff is unused by every real container. A
/// container that reaches the translator with one of them was written by
/// the lowering.
///
/// Encoding: OpcodeToken0 carries the opcode in bits 0-10 and the
/// instruction length in bits 24-30; every other bit is zero (no saturate,
/// no precise mask, no extended token). Token 1 is a raw control DWORD
/// (below), then the operands in the ordinary operand encoding.
///
/// Register model, as the lowering lays values out:
///  - 32-bit values (and i1 masks, 0 / ~0) occupy one temp component;
///  - a 64-bit integer occupies an aligned component pair, low word first
///    (.xy or .zw), like a DXBC double; its source operand swizzles the pair
///    to (lo, hi, lo, hi), its destination masks both components;
///  - a 16-bit integer occupies one component, zero-extended (its 16 bits
///    are the value; signed operations sign-extend first);
///  - a 16-bit float (half) occupies one component as the float32 that
///    represents it exactly (every half is a float32); each operation
///    producing a half rounds its float32 result to half (VX_F16 Round).

#include <vitreo/common.hpp>

namespace vitreo::graphics::dxbc_vitreo_ext {

/// Immutable DXIL global initializer CUSTOMDATA. Unlike instructions, custom
/// data has a full DWORD length: [53 | class<<11, length, internal x-slot,
/// raw register-model words...]. No public ICB or x# declaration limits apply.
/// This private class is never emitted by a DXBC producer; the lowerer owns it.
constexpr u32 kReadonlyIndexableDataClass = 0x1fffffu;

/// Wave and quad operations. Control: kind bits 0-7, op bits 8-15, type
/// bits 16-23. Operands: dst, then the sources the kind lists.
constexpr u32 kOpWave = 0x700;
/// 64-bit integer operations. Control: I64Op. Operands: dst, a [, b [, c]].
constexpr u32 kOpI64 = 0x701;
/// 16-bit float conversions. Control: F16Op. Operands: dst, src.
constexpr u32 kOpF16 = 0x702;
/// SV_ViewID (DXIL ViewID, opcode 138; DirectX-Specs "View Instancing"):
/// the view instance the draw runs for. Control: 0. Operands: dst (one
/// uint component). The value is DxbcTranslationOptions::view_id: the
/// D3D12 frontend implements view instancing by draw-level looping
/// (ViewInstancingTier 2) and translates each view's pipeline with its id.
constexpr u32 kOpViewId = 0x704;
/// SM6.4 packed-byte integer dot and wrapping accumulator add. Control:
/// 0 signed bytes, 1 unsigned bytes. Operands: dst, accumulator, a, b
/// (one 32-bit component each). Keep this operation intact so lowering
/// need not impose SM5's saturating float-to-integer conversion contract.
constexpr u32 kOpPackedDot = 0x705;
/// SM6.6 byte packing. Control: 0 truncate, 1 clamp signed input to
/// [0,255], 2 clamp to [-128,127]; bit4 means 16-bit input lanes.
/// Operands: scalar dst, x, y, z, w (lanes held in 32-bit components).
constexpr u32 kOpPackBytes = 0x706;
/// SM6.6 byte unpacking. Control: 0 unsigned, 1 signed; bit4 means
/// 16-bit result lanes (zero-extended in the internal register model).
/// Operands: vector dst, scalar packed word.
constexpr u32 kOpUnpackBytes = 0x707;
/// One word of a sampler's DxbcTranslationOptions::sampler_state_buffer
/// record (kSamplerStateRecordWords words; the filter, address modes and
/// LOD clamps are words kSamplerStateFilterStateWord..+2). Control: the
/// word index. Operands: dst (one uint component), s# (not dynamically
/// indexed). The sampler feedback writes (dxil_lower_feedback.cpp) read the
/// state Metal sampler objects do not expose. Taken from the top of the
/// range so it never meets the opcodes other lowerings add from 0x700 up.
constexpr u32 kOpSamplerState = 0x7F0;
/// Mesh and amplification shader operations (SM 6.5, DXIL.rst opcodes
/// 168-173; DirectX-Specs "Mesh Shader"). Control: MeshOp bits 0-7, then
/// the operation's fields (MeshOp). The program is a compute program whose
/// VXMS chunk (dxbc_parser.hpp DxbcMeshState) names the stage.
constexpr u32 kOpMesh = 0x710;  // registry: mesh / amplification own 0x710-0x71F (PHASE2_BASE.md)
/// float32 operations that keep denormals (SM 6.2 "fp32-denorm-mode" =
/// "preserve"). Control: F32POp. Operands: dst, a [, b [, c]].
constexpr u32 kOpF32P = 0x703;

/// Texture operations SM 5.1 has no instruction for (shader models 6.6-6.8:
/// compute-shader derivatives, comparison sampling with gradients). Control:
/// TexOp bits 0-7; bits 9-20 the immediate texel offsets u, v, w as 4-bit
/// two's complement, where the sample-controls extended token keeps them
/// (D3D10_SB_EXTENDED_OPCODE_SAMPLE_CONTROLS); bit 24 (kTexClamp) a MinLOD
/// clamp operand ends the operand list. Operands: see TexOp.
constexpr u32 kOpTex = 0x740;
constexpr u32 kTexOffsetMask = 0x1FFE00u;
constexpr u32 kTexClamp = 1u << 24;
/// SM 6.1 barycentrics (DXIL.rst SV_Barycentrics, AttributeAtVertex 137).
/// Control: BaryOp bits 0-7; for Coord, bits 8-11 the DXIL interpolation
/// mode of the SV_Barycentrics element (2 linear, 3 centroid, 4
/// noperspective, 5 noperspective centroid, 6 sample, 7 noperspective
/// sample).
constexpr u32 kOpBary = 0x741;
/// An acceleration-structure SRV (DXIL resource shape RTAccelerationStructure,
/// a RaytracingAccelerationStructure): dcl_resource_raw's encoding -- the
/// range operand, then the space DWORD -- with no control DWORD. It takes
/// the place of a t# declaration (the binding walk counts it as one).
constexpr u32 kOpDclAccelerationStructure = 0x720;
/// Inline ray tracing (RayQuery, DXIL.rst opcodes 179-215). Control:
/// RayQueryOp bits 0-7, the query (AllocateRayQuery call) bits 8-15, aux
/// bits 16-31: TraceRayInline's template RAY_FLAGs; a 3x4 matrix getter's
/// row | column << 2; a vector getter's component. Operands per op below.
constexpr u32 kOpRayQuery = 0x721;
/// SM 6.7 programmable texel offsets (DirectX-Specs "HLSL Advanced Texture
/// Operations", Programmable Offsets: "the 4 least significant bits are
/// honored as a signed value, yielding a [-8..7] range"): the offsets of
/// the NEXT sample instruction (sample, sample_l / _b / _d / _c / _c_lz, and
/// the VX_TEX forms that become them), in place of its immediate ones.
/// Control: 0. Operands: one source holding (u, v, w) as integers already
/// reduced to [-8, 7] (the lowering sign-extends their low 4 bits).
/// Registry: C4 slice, 0x742-0x74F.
constexpr u32 kOpTexOffset = 0x742;
/// SM 6.8 SV_StartVertexLocation / SV_StartInstanceLocation (DXIL.rst
/// StartVertexLocation 256, StartInstanceLocation 257; DirectX-Specs "HLSL
/// Shader Model 6.8", Extended Command Information: StartVertexLocation of
/// DrawInstanced or BaseVertexLocation of DrawIndexedInstanced, and
/// StartInstanceLocation of either). Vertex stage. Control: 0 the start
/// vertex, 1 the start instance. Operands: dst (one uint component).
constexpr u32 kOpDrawParam = 0x743;

constexpr bool is_extension_opcode(u32 opcode) { return opcode >= 0x700 && opcode <= 0x7FF; }

enum class BaryOp : u8 {
    /// dst.xyz = the barycentric weights of the primitive's vertices 0, 1,
    /// 2 (AttributeAtVertex's order), dst.w = 0. Operands: dst.
    Coord = 0,
    /// dst = v#'s value at vertex k of the triangle (the order of Coord's
    /// weights), swizzled as the v# operand says. Operands: dst, v# (a
    /// pixel-shader input, not dynamically indexed), k (one uint
    /// component; 0, 1 or 2). The value comes from three flat inputs at
    /// per_vertex_input_location(v#, 0..2) (dxbc_to_spirv.hpp) that the
    /// pass-through vertex stage before the pixel shader writes.
    AttributeAtVertex = 1,
};

enum class TexOp : u8 {
    /// sample_c with explicit gradients (SampleCmp where no derivatives are
    /// implicit; SM 6.8 SampleCmpGrad). Operands: dst, status (a null
    /// operand when unused), address, resource, sampler, reference, ddx,
    /// ddy [, clamp]: sample_c_cl_s's list with sample_d's gradients
    /// after the reference.
    SampleCmpGrad = 0,
    /// lod from explicit gradients, for a stage without implicit ones:
    /// dst = (ClampedLOD, NonClampedLOD, 0, 0) swizzled by the resource
    /// operand, as lod (D3D11.3 22.5.6) computes from the same LOD sample_d
    /// would use (7.18.11). Operands: dst, address, resource, sampler,
    /// ddx, ddy.
    LodGrad = 1,
    /// sample_c at an explicit LOD (SM 6.7 SampleCmpLevel): sample_c_lz
    /// with the LOD operand after the reference, MipLODBias added as for
    /// sample_l. Operands: dst, status, address, resource, sampler,
    /// reference, lod.
    SampleCmpLevel = 2,
    /// sample_c with a bias added to the implicit LOD (SM 6.8
    /// SampleCmpBias, pixel stage). Operands: dst, status, address,
    /// resource, sampler, reference, bias [, clamp].
    SampleCmpBias = 3,
};

enum class WaveKind : u8 {
    IsFirstLane = 0,     ///< dst
    LaneIndex = 1,       ///< dst
    LaneCount = 2,       ///< dst
    AnyTrue = 3,         ///< dst, cond
    AllTrue = 4,         ///< dst, cond
    AllEqual = 5,        ///< dst, value (typed)
    Ballot = 6,          ///< dst.xyzw, cond
    ReadLaneAt = 7,      ///< dst, value, lane
    ReadFirst = 8,       ///< dst, value
    Active = 9,          ///< dst, value (op, type)
    Prefix = 10,         ///< dst, value (op, type): exclusive
    ActiveCountBits = 11,  ///< dst, cond
    PrefixCountBits = 12,  ///< dst, cond
    Match = 13,          ///< dst.xyzw, value (type)
    MultiPrefix = 14,    ///< dst, value, mask.xyzw (op, type)
    MultiPrefixCountBits = 15,  ///< dst, cond, mask.xyzw
    QuadReadLaneAt = 16,  ///< dst, value, quad lane (typed)
    QuadAcrossX = 17,     ///< dst, value (typed)
    QuadAcrossY = 18,
    QuadDiagonal = 19,
    /// dst: ~0 on a pixel shader's helper lanes, else 0 (SM 6.6
    /// IsHelperLane; 0 outside the pixel stage).
    IsHelperLane = 20,
};

/// Control bit: helper lanes take part as active lanes (the DXIL function
/// attribute waveops-include-helper-lanes, SM 6.7 [WaveOpsIncludeHelperLanes]).
constexpr u32 kWaveIncludeHelpers = 1u << 24;

enum class WaveOp : u8 { None = 0, Sum, Product, Min, Max, And, Or, Xor };

/// The value type of a wave operation, in the register model above.
enum class WaveType : u8 { U32 = 0, I32, F32, U64, I64, U16, I16, F16 };

constexpr bool wave_type_is_64(WaveType t) { return t == WaveType::U64 || t == WaveType::I64; }

constexpr u32 wave_control(WaveKind kind, WaveOp op, WaveType type) {
    return static_cast<u32>(kind) | (static_cast<u32>(op) << 8) | (static_cast<u32>(type) << 16);
}

/// 64-bit integer operations. Sources and destination are pairs except as
/// noted. Division and remainder by zero give all ones (as SM5 udiv does);
/// shift counts use their low 6 bits; signed division of INT64_MIN by -1
/// wraps.
enum class I64Op : u32 {
    Add = 0, Sub, Mul, UDiv, SDiv, URem, SRem, Shl, LShr, AShr, And, Or, Xor,
    UMin, UMax, SMin, SMax,
    /// dst (32-bit, 0 / ~0) = a ? b
    Eq, Ne, ULt, UGe, SLt, SGe,
    /// dst pair = a (32-bit) sign- / zero-extended
    SExt32, ZExt32,
    /// dst (32-bit float) = a converted, round to nearest even
    UToF32, SToF32,
    /// dst pair = a (32-bit float) converted, toward zero; NaN 0, out of
    /// range saturates (as SM5 ftoi / ftou do)
    F32ToU, F32ToS,
    /// dst (32-bit) = bit count / lowest set bit / highest set bit (~0
    /// when none) / highest bit differing from the sign (~0 when none)
    CountBits, FirstBitLo, FirstBitHi, FirstBitSHi,
    /// dst pair = bit reverse of a
    BitReverse,
    /// dst pair = a * b + c
    Mad,
    Count,
};

/// 16-bit float conversions (src and dst 32-bit components).
enum class F16Op : u32 {
    /// float32 -> the nearest half (ties to even, overflow to infinity,
    /// denormals kept), as float32.
    Round = 0,
    /// float32 -> that half's 16 bits, zero-extended.
    ToBits = 1,
    /// the low 16 bits as a half -> float32 (exact).
    FromBits = 2,
};

/// Mesh / amplification operations. Output registers are the OSGN (vertex)
/// and PCSG (primitive) registers of the lowered container; register
/// kMeshCullPrimitiveRegister is SV_CullPrimitive, which has none.
enum class MeshOp : u8 {
    /// SetMeshOutputCounts: numVertices, numPrimitives.
    SetCounts = 0,
    /// StoreVertexOutput: control register bits 8-15, component bits 16-17,
    /// element rows bits 18-23, kMeshDynamicRow; operands vertex index,
    /// value [, row within the element when dynamic].
    StoreVertex = 1,
    /// StorePrimitiveOutput: as StoreVertex, with the primitive index.
    StorePrimitive = 2,
    /// EmitIndices: primitive index, v0, v1, v2 (v2 is not read for lines).
    EmitIndices = 3,
    /// A mesh shader's payload (GetMeshPayload) word: dst, word index.
    PayloadLoad = 4,
    /// DispatchMesh: control payload g# bits 8-23; operands x, y, z. The
    /// payload is that groupshared object's words, in element order.
    DispatchMesh = 5,
};
constexpr u32 kMeshCullPrimitiveRegister = 0xFF;
constexpr u32 kMeshDynamicRow = 1u << 24;
constexpr u32 mesh_control(MeshOp op, u32 reg = 0, u32 comp = 0, u32 rows = 0) {
    return static_cast<u32>(op) | (reg << 8) | (comp << 16) | (rows << 18);
}
/// The ray-query operation: DXIL opcode - 179 (TraceRayInline ..
/// CommittedObjectRayDirection), then the two InstanceContributionTo-
/// HitGroupIndex getters (214, 215). TraceRayInline: the t# acceleration
/// structure, then rayFlags, instanceInclusionMask, origin x y z, tMin,
/// direction x y z, tMax. CommitProceduralPrimitiveHit: t. Abort and
/// CommitNonOpaqueTriangleHit: none. Every other: the destination.
enum class RayQueryOp : u8 {
    TraceRayInline = 0, Proceed, Abort, CommitNonOpaqueTriangleHit, CommitProceduralPrimitiveHit,
    CommittedStatus, CandidateType, CandidateObjectToWorld3x4, CandidateWorldToObject3x4,
    CommittedObjectToWorld3x4, CommittedWorldToObject3x4, CandidateProceduralPrimitiveNonOpaque,
    CandidateTriangleFrontFace, CommittedTriangleFrontFace, CandidateTriangleBarycentrics,
    CommittedTriangleBarycentrics, RayFlags, WorldRayOrigin, WorldRayDirection, RayTMin,
    CandidateTriangleRayT, CommittedRayT, CandidateInstanceIndex, CandidateInstanceID,
    CandidateGeometryIndex, CandidatePrimitiveIndex, CandidateObjectRayOrigin,
    CandidateObjectRayDirection, CommittedInstanceIndex, CommittedInstanceID,
    CommittedGeometryIndex, CommittedPrimitiveIndex, CommittedObjectRayOrigin,
    CommittedObjectRayDirection, CandidateInstanceContributionToHitGroupIndex,
    CommittedInstanceContributionToHitGroupIndex,
};

/// float32 arithmetic with denormal inputs and results kept, for shaders
/// whose functions carry "fp32-denorm-mode"="preserve" (DXIL.rst; the DXC
/// wiki "Denorm Mode" names add, subtract, multiply and divide; the
/// comparisons, min / max and saturate keep them too on WARP12 and the RTX
/// 4080, tests/oracle_data/d3d12_denorm_probe*.expected). Semantics are
/// soft_float.hpp's. Each destination component takes the operation of the
/// sources' matching (post-swizzle) components; the neg / abs source
/// modifiers act on the sign bit. Comparison results are 0 / ~0.
enum class F32POp : u32 {
    Add = 0,  ///< a + b
    Mul,      ///< a * b
    Fma,      ///< a * b + c, one rounding
    Div,      ///< a / b, correctly rounded
    Min,      ///< DXIL FMin
    Max,      ///< DXIL FMax
    Saturate, ///< clamp(a, 0, 1), NaN -> 0
    Eq,       ///< a == b (ordered)
    Ne,       ///< a != b (unordered)
    Lt,       ///< a < b (ordered)
    Ge,       ///< a >= b (ordered)
    Count,
};

}  // namespace vitreo::graphics::dxbc_vitreo_ext
