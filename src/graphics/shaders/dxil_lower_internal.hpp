// SPDX-License-Identifier: LGPL-2.1-only
#pragma once
/// @file dxil_lower_internal.hpp
/// @brief Internals of the DXIL -> SM5.1 lowering (dxil_to_dxbc.hpp):
/// the SM5.1 token emitter and the DXIL enum values the lowering decodes.

#include "dxbc_to_spirv_sm4.hpp"
#include "dxil_module.hpp"

#include <initializer_list>
#include <vector>

namespace vitreo::graphics::dxil_lower {

using dxbc_spirv_detail::Sm4Opcode;
using dxbc_spirv_detail::Sm4OperandType;

// ---- DXIL enum values ---------------------------------------------------
// Measured from dxc 1.6 listings (tests/oracle_data/d3d12_sm6/
// cs_enums.dxil.txt and cs_exec.dxil.txt): the resource record shape
// (field 6), the element-type tag 0 value, the sampler kind, the
// AtomicBinOp operation and the Barrier mode bits each HLSL construct
// compiles to.
enum class ResourceShape : u32 {
    Texture1D = 1, Texture2D = 2, Texture2DMS = 3, Texture3D = 4,
    TextureCube = 5, Texture1DArray = 6, Texture2DArray = 7,
    Texture2DMSArray = 8, TextureCubeArray = 9, TypedBuffer = 10,
    RawBuffer = 11, StructuredBuffer = 12,
    /// RaytracingAccelerationStructure (DXIL.rst resource kinds:
    /// RTAccelerationStructure = 16).
    RTAccelerationStructure = 16,
    /// DXIL.rst ResourceKind 17 / 18 (SM 6.5 sampler feedback; measured,
    /// d3d12_vinst/d3d12_feedback.hlsl listings: FeedbackTexture2D is 17).
    FeedbackTexture2D = 17, FeedbackTexture2DArray = 18,
};
/// Element types (the subset whose HLSL spelling the listing states;
/// U16 and F16 from tests/oracle_data/d3d12_sm6x/cs_res16.dxil.txt, I16
/// the signed member of that pair, as I32 / U32 are).
enum class ComponentType : u32 {
    I16 = 2, U16 = 3, I32 = 4, U32 = 5, F16 = 8, F32 = 9, SNormF32 = 13, UNormF32 = 14,
};
enum class ResourceClass : u32 { SRV = 0, UAV = 1, CBV = 2, Sampler = 3 };
/// AtomicBinOp operation (cs_enums: InterlockedAdd .. Exchange).
enum class AtomicOp : u32 {
    Add = 0, And = 1, Or = 2, Xor = 3, IMin = 4, IMax = 5, UMin = 6,
    UMax = 7, Exchange = 8,
};
/// Barrier mode bits (cs_enums: GroupMemoryBarrierWithGroupSync 9,
/// GroupMemoryBarrier 8, DeviceMemoryBarrierWithGroupSync 3,
/// DeviceMemoryBarrier 2, AllMemoryBarrierWithGroupSync 11,
/// AllMemoryBarrier 10).
constexpr u32 kBarrierSyncThreadGroup = 1;
constexpr u32 kBarrierUavFenceGlobal = 2;
constexpr u32 kBarrierUavFenceThreadGroup = 4;
constexpr u32 kBarrierTgsmFence = 8;

// ---- SM5.1 token encoding -------------------------------------------------
// d3d12TokenizedProgramFormat.hpp (Windows SDK): OpcodeToken0 opcode bits
// 0-10, instruction length bits 24-30; OperandToken0 component count bits
// 0-1, selection mode 2-3, mask / swizzle / select 4-11, type 12-19, index
// dimension 20-21, index representation 22-30 (3 bits each), extended 31.
constexpr u32 kSaturate = 1u << 13;
constexpr u32 kTestNonZero = 1u << 18;
constexpr u32 kPreciseShift = 19;
constexpr u32 kResinfoUint = 2u << 11;
constexpr u32 kRefactoringAllowed = 1u << 11;
constexpr u32 kSyncThreadsInGroup = 0x800;
constexpr u32 kSyncTgsm = 0x1000;
constexpr u32 kSyncUavGroup = 0x2000;
constexpr u32 kSyncUavGlobal = 0x4000;
constexpr u32 kGloballyCoherent = 0x10000;
constexpr u32 kRasterizerOrdered = 0x20000;
constexpr u32 kUavHasCounter = 0x800000;

// Declaration opcodes (D3D10_SB_OPCODE_TYPE).
constexpr u32 kDclResource = 88;
constexpr u32 kDclConstantBuffer = 89;
constexpr u32 kDclSampler = 90;
constexpr u32 kDclInput = 95;
constexpr u32 kDclTemps = 104;
constexpr u32 kDclIndexableTemp = 105;
constexpr u32 kDclGlobalFlags = 106;
constexpr u32 kDclThreadGroup = 155;
constexpr u32 kDclUavTyped = 156;
constexpr u32 kDclUavRaw = 157;
constexpr u32 kDclUavStructured = 158;
constexpr u32 kDclTgsmStructured = 160;
constexpr u32 kDclResourceRaw = 161;
constexpr u32 kDclResourceStructured = 162;

/// One encoded operand (OperandToken0, extended tokens, indices).
struct Operand {
    std::vector<u32> tokens;
};

struct Index {
    bool relative{false};
    u32 immediate{0};
    u32 reg{0};   ///< relative: the temp register
    u32 comp{0};  ///< relative: its component
};

/// Operand builders. `mask` / `swizzle` are the 4-bit write mask and the
/// 8-bit swizzle (2 bits per component, x in bits 0-1).
Operand dst_temp(u32 reg, u32 mask);
Operand src_temp(u32 reg, u32 swizzle, u32 modifier = 0);
Operand src_temp_scalar(u32 reg, u32 comp, u32 modifier = 0);
/// The select-1 form fxc uses for if / breakc / discard conditions.
Operand src_temp_select(u32 reg, u32 comp);
Operand imm4(u32 x, u32 y, u32 z, u32 w);
inline Operand imm(u32 v) { return imm4(v, v, v, v); }
Operand null_operand();
/// `l#` (call / label) and a one-component immediate (a switch case).
Operand label_operand(u32 label);
Operand imm1(u32 value);
/// A register with no index (vThreadID, vThreadGroupID, ...).
Operand special_src(Sm4OperandType type, u32 swizzle);
Operand special_dcl(Sm4OperandType type, u32 mask);
/// T#/U#/S#/CB# with SM5.1 indices: [range ID][register (+ index)] (and
/// [row] for a constant buffer). `swizzle` ~0u writes a mask operand of
/// `mask` (a UAV destination).
Operand resource(Sm4OperandType type, u32 range_id, const Index& reg,
                 bool nonuniform, u32 swizzle = 0xE4, u32 mask = 0);
Operand cbuffer(u32 range_id, const Index& reg, const Index& row,
                bool nonuniform, u32 swizzle);
Operand indexable(u32 array, const Index& element, u32 swizzle_or_mask,
                  bool destination);
Operand tgsm(u32 id, u32 swizzle_or_mask, bool destination);
/// The 3-D declaration operand [range ID][lower][upper].
Operand range_dcl(Sm4OperandType type, u32 range_id, u32 lower, u32 upper);

constexpr u32 swizzle_of(u32 comp) { return comp * 0x55u; }

class Emitter {
public:
    void inst(u32 opcode_and_flags, std::initializer_list<const Operand*> ops,
              std::initializer_list<u32> extra = {});
    /// An instruction with one extended opcode token (sample controls).
    void inst_ext(u32 opcode_and_flags, u32 extended, std::initializer_list<const Operand*> ops);
    void raw(u32 token) { m_code.push_back(token); }
    /// The emitted tokens, for rewriting the last instruction in place.
    std::vector<u32>& mutable_code() { return m_code; }
    [[nodiscard]] const std::vector<u32>& code() const { return m_code; }
    [[nodiscard]] usize size() const { return m_code.size(); }
    /// Instructions emitted since the last terminator check.
    [[nodiscard]] bool last_was_jump() const { return m_last_jump; }

private:
    std::vector<u32> m_code;
    bool m_last_jump{false};
};

}  // namespace vitreo::graphics::dxil_lower
