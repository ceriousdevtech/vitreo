// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file dxbc_to_spirv_sm4.hpp
/// @brief SM4/SM5 instruction encoding and the DXBC bytecode decoder.

#include "graphics/shaders/dxbc_to_spirv.hpp"

#include <array>
#include <cstring>
#include <map>
#include <vector>

namespace vitreo::graphics::dxbc_spirv_detail {

// =============================================================================
// SM4/SM5 Instruction Encoding
// =============================================================================

// SM4/SM5 Opcodes (subset for basic shaders)
enum class Sm4Opcode : u32 {
    ADD = 0,
    AND = 1,
    BREAK = 2,
    BREAKC = 3,
    CALL = 4,
    CALLC = 5,
    CASE = 6,
    CONTINUE = 7,
    CONTINUEC = 8,
    CUT = 9,
    DEFAULT = 10,
    DERIV_RTX = 11,
    DERIV_RTY = 12,
    DISCARD = 13,
    DIV = 14,
    DP2 = 15,
    DP3 = 16,
    DP4 = 17,
    ELSE = 18,
    EMIT = 19,
    EMITTHENCUT = 20,
    ENDIF = 21,
    ENDLOOP = 22,
    ENDSWITCH = 23,
    EQ = 24,
    EXP = 25,
    FRC = 26,
    FTOI = 27,
    FTOU = 28,
    GE = 29,
    IADD = 30,
    IF = 31,
    IEQ = 32,
    IGE = 33,
    ILT = 34,
    IMAD = 35,
    IMAX = 36,
    IMIN = 37,
    IMUL = 38,
    INE = 39,
    INEG = 40,
    ISHL = 41,
    ISHR = 42,
    ITOF = 43,
    LABEL = 44,
    LD = 45,
    LD_MS = 46,
    LOG = 47,
    LOOP = 48,
    LT = 49,
    MAD = 50,
    MIN = 51,
    MAX = 52,
    MOV = 54,
    MOVC = 55,
    MUL = 56,
    NE = 57,
    NOP = 58,
    NOT = 59,
    OR = 60,
    RESINFO = 61,
    RET = 62,
    RETC = 63,
    ROUND_NE = 64,
    ROUND_NI = 65,
    ROUND_PI = 66,
    ROUND_Z = 67,
    RSQ = 68,
    SAMPLE = 69,
    SAMPLE_C = 70,
    SAMPLE_C_LZ = 71,
    SAMPLE_L = 72,
    SAMPLE_D = 73,
    SAMPLE_B = 74,
    SQRT = 75,
    SWITCH = 76,
    SINCOS = 77,
    UDIV = 78,
    ULT = 79,
    UGE = 80,
    UMUL = 81,
    UMAD = 82,
    UMAX = 83,
    UMIN = 84,
    USHR = 85,
    UTOF = 86,
    XOR = 87,
    // D3D11.3 22.7.19 fcall and 22.3.49-51 dcl_function_body /
    // dcl_function_table / dcl_interface: D3D11_SB_OPCODE_INTERFACE_CALL and
    // the three declarations in D3D10_SB_OPCODE_TYPE (DirectX-Headers
    // D3D12TokenizedProgramFormat.hpp, which repeats the SM5 numbering).
    INTERFACE_CALL = 120,
    DCL_FUNCTION_BODY = 144,
    DCL_FUNCTION_TABLE = 145,
    DCL_INTERFACE = 146,
    DERIV_RTX_COARSE = 122,
    // d3d11TokenizedProgramFormat.hpp lists the fine form between the two
    // coarse forms already numbered 122 and 124.
    DERIV_RTX_FINE = 123,
    DERIV_RTY_COARSE = 124,
    DERIV_RTY_FINE = 125,
    GATHER4 = 109,
    // Measured with fxc on Windows (tools/oracle_probe/
    // gather_family_shaders.hlsl): gather4_c 126, gather4_po 127,
    // gather4_po_c 128.
    GATHER4_C = 126,
    GATHER4_PO = 127,
    GATHER4_PO_C = 128,
    SWAPC = 142,
    COUNTBITS = 134,
    FIRSTBIT_HI = 135,
    FIRSTBIT_LO = 136,
    FIRSTBIT_SHI = 137,
    UBFE = 138,
    IBFE = 139,
    BFI = 140,
    // d3d11TokenizedProgramFormat.hpp lists BFREV immediately after BFI and
    // before SWAPC. BFI and SWAPC were measured as 140 and 142.
    BFREV = 141,
    DCL_UNORDERED_ACCESS_VIEW_TYPED = 156,
    RCP = 129,
    F32TOF16 = 130,
    F16TOF32 = 131,
    // uaddc / usubb: fxc never emits them; the numbers are the ones fxc's
    // own disassembler names in patched bytecode (tools/oracle_probe/
    // d3d11_a09a_build.sh `check cs_p_uaddc` / `cs_p_usubb`, Windows).
    UADDC = 132,
    // Measured: fxc (Windows Kits 10.0.26100) encodes the msad4 intrinsic's
    // msad instruction as opcode 213 (tools/oracle_probe/d3d11_fl111.hlsl
    // cs_msad, token 0x90000d5), under dcl_globalFlags
    // enable11_1ShaderExtensions.
    MSAD = 213,
    USUBB = 133,
    // Named by fxc's own disassembler (/dumpbin) in the area-09b probe's
    // shaders (tools/oracle_probe/d3d11_a09b_*.hlsl, Windows): lod 108,
    // samplepos 110, sampleinfo 111, bufinfo 121, eval_snapped 203,
    // eval_sample_index 204, eval_centroid 205.
    LOD = 108,
    SAMPLEPOS = 110,
    SAMPLEINFO = 111,
    BUFINFO = 121,
    EVAL_SNAPPED = 203,
    EVAL_SAMPLE_INDEX = 204,
    EVAL_CENTROID = 205,
    DCL_RESOURCE_STRUCTURED = 162,
    LD_UAV_TYPED = 163,
    STORE_UAV_TYPED = 164,
    LD_RAW = 165,
    STORE_RAW = 166,
    LD_STRUCTURED = 167,
    STORE_STRUCTURED = 168,
    // The atomic block below is MEASURED, not recalled: fxc (Windows SDK
    // 10.0.26100) compiled a shader using every one of these and the token
    // stream was decoded alongside the /Fc listing. See
    // tools/oracle_probe/dxbc_opcode_probe.hlsl.
    //
    // The imm_atomic_ numbering here was previously wrong in two ways at
    // once, and both were silent: alloc and consume occupy 178/179, so
    // IADD..EXCH were each low by one (an imm_atomic_iadd decoded as an AND),
    // and the signed/unsigned min/max pairs were transposed (an
    // imm_atomic_imax decoded as a UMAX). SYNC=190, F32TOF16=130 and
    // COUNTBITS=134 were measured in the same run and were already correct.
    ATOMIC_AND = 169,
    ATOMIC_OR = 170,
    ATOMIC_XOR = 171,
    ATOMIC_CMP_STORE = 172,
    ATOMIC_IADD = 173,
    ATOMIC_IMAX = 174,
    ATOMIC_IMIN = 175,
    ATOMIC_UMAX = 176,
    ATOMIC_UMIN = 177,
    IMM_ATOMIC_ALLOC = 178,
    IMM_ATOMIC_CONSUME = 179,
    IMM_ATOMIC_IADD = 180,
    IMM_ATOMIC_AND = 181,
    IMM_ATOMIC_OR = 182,
    IMM_ATOMIC_XOR = 183,
    IMM_ATOMIC_EXCH = 184,
    IMM_ATOMIC_CMP_EXCH = 185,
    IMM_ATOMIC_IMAX = 186,
    IMM_ATOMIC_IMIN = 187,
    IMM_ATOMIC_UMAX = 188,
    IMM_ATOMIC_UMIN = 189,
    SYNC = 190,
    // SM5 stream forms; measured with fxc (gs_5_0 always uses them).
    EMIT_STREAM = 117,
    CUT_STREAM = 118,
    EMITTHENCUT_STREAM = 119,
    // Double precision (D3D11.3 22.14-22.17), MEASURED: fxc 10.1 (SDK
    // 10.0.26100) compiled tools/oracle_probe/dxbc_double_opcode_probe.hlsl
    // on Windows and dxbc_token_decode.py paired each /Fc mnemonic with
    // its token (dadd 0x070000bf ...).
    DADD = 191,
    DMAX = 192,
    DMIN = 193,
    DMUL = 194,
    DEQ = 195,
    DGE = 196,
    DLT = 197,
    DNE = 198,
    DMOV = 199,
    DMOVC = 200,
    DTOF = 201,
    FTOD = 202,
    // The Direct3D 11.1 extended set (D3D11_OPTIONS.
    // ExtendedDoublesShaderInstructions), same probe.
    DDIV = 210,
    DFMA = 211,
    DRCP = 212,
    DTOI = 214,
    DTOU = 215,
    ITOD = 216,
    UTOD = 217,
};

// SM4/SM5 Operand Type
enum class Sm4OperandType : u32 {
    Temp = 0,
    Input = 1,
    Output = 2,
    IndexableTemp = 3,
    Immediate32 = 4,
    Immediate64 = 5,
    Sampler = 6,
    Resource = 7,
    ConstantBuffer = 8,
    ImmediateConstantBuffer = 9,
    Label = 10,
    InputPrimitiveID = 11,
    OutputDepth = 12,
    Null = 13,
    Stream = 16,
    // Measured with fxc on Windows (tools/oracle_probe/
    // system_value_shaders.hlsl, dxbc_operand_types.py).
    OutputCoverageMask = 15,
    // Tessellation (measured with fxc, 22/25/26/27/28; 23/24 per the
    // tokenized-program format, between the measured neighbours).
    OutputControlPointId = 22,
    InputForkInstanceId = 23,
    InputJoinInstanceId = 24,
    InputControlPoint = 25,
    OutputControlPoint = 26,
    InputPatchConstant = 27,
    InputDomainPoint = 28,
    InputCoverageMask = 35,
    InputGSInstanceID = 37,
    OutputDepthGreaterEqual = 38,
    OutputDepthLessEqual = 39,
    OutputStencilRef = 41,
    UnorderedAccessView = 30,
    ThreadGroupSharedMemory = 31,
    InputThreadId = 32,
    InputThreadGroupId = 33,
    InputThreadIdInGroup = 34,
    InputThreadIdInGroupFlattened = 36,
    // `rasterizer` in sampleinfo/samplepos (0x0000e046: type 14, measured
    // in the area-09b probe's ps_eval).
    Rasterizer = 14,
    // Class linkage (D3D11_SB_OPERAND_TYPE_*, tokenized-program format):
    // `label fb#`, fcall's fp#[array][...], and this[] (22.7.20).
    FunctionBody = 17,
    FunctionTable = 18,
    Interface = 19,
    ThisPointer = 29,
};

// SM4/SM5 Operand Component Selection
enum class Sm4ComponentSelection : u32 {
    Mask = 0,
    Swizzle = 1,
    Select1 = 2,
};

struct Sm4Operand {
    Sm4OperandType type{Sm4OperandType::Temp};
    u32 index0{0};
    u32 index1{0};
    u32 num_components{4};
    u32 swizzle{0xE4}; // .xyzw
    u32 write_mask{0xF};
    u32 modifier{0};
    bool index0_relative{false};
    bool index1_relative{false};
    /// The register of the last relative index decoded (index1's when both
    /// are relative).
    Sm4OperandType relative_type{Sm4OperandType::Temp};
    u32 relative_index0{0};
    u32 relative_component{0};
    /// index0's own relative register, kept when index1 is relative too
    /// (cb[r1.x + 0][r0.w + 0] in a class method, D3D11.3 7.19.5.2).
    Sm4OperandType index0_relative_type{Sm4OperandType::Temp};
    u32 index0_relative_register{0};
    u32 index0_relative_component{0};
    /// Shader model 5.1 resource operands (DxbcDecoder::
    /// set_sm51_resource_operands): t#/s#/u# are T#[register] -- 2-D,
    /// [range ID][register] -- and a constant buffer CB#[register][element]
    /// is 3-D. The decoder leaves index0 = the range ID (every per-resource
    /// table is keyed by it), moves the register index here, and for a
    /// constant buffer moves the element index into index1, where the SM4/5
    /// cb#[element] form has it. `register_index` is the immediate part:
    /// the register is register_index + the relative register's component
    /// when register_relative, the ABSOLUTE register in its space (fxc
    /// writes T0[r0.y + 2] for element r0.y of T0[2:9]).
    bool has_register_index{false};
    u32 register_index{0};
    bool register_relative{false};
    Sm4OperandType register_relative_type{Sm4OperandType::Temp};
    u32 register_relative_register{0};
    u32 register_relative_component{0};
    /// NonUniformResourceIndex: the extended operand token (type 1,
    /// D3D10_SB_EXTENDED_OPERAND_MODIFIER) with bit 17 set -- fxc's
    /// `{ nonuniform }` (measured: 0x00020001 before T4[r0.y + 10]).
    bool nonuniform{false};
    bool is_immediate{false};
    float immediate[4]{};
    
    [[nodiscard]] bool is_scalar() const {
        return num_components == 1;
    }
};

DxbcRegisterClass register_class_of(Sm4OperandType type);

// =============================================================================
// DXBC Bytecode Decoder
// =============================================================================

class DxbcDecoder {
public:
    DxbcDecoder(const u8* bytecode, usize size)
        : m_data(bytecode), m_size(size), m_position(0) {
        // The version token: a shader model 5.1 program may index a
        // constant buffer's element (an operand's third index) relatively.
        if (size >= 4) {
            u32 version = 0;
            std::memcpy(&version, bytecode, sizeof(version));
            const u32 major = (version >> 4) & 0xFu;
            m_sm51 = major > 5u || (major == 5u && (version & 0xFu) >= 1u);
        }
    }

    [[nodiscard]] bool has_tokens(usize count) const {
        return m_position + count <= (m_size / sizeof(u32));
    }

    [[nodiscard]] u32 read_token() {
        if (!has_tokens(1)) return 0;
        u32 token;
        std::memcpy(&token, m_data + m_position * sizeof(u32), sizeof(u32));
        ++m_position;
        return token;
    }

    [[nodiscard]] u32 peek_token() const {
        if (!has_tokens(1)) return 0;
        u32 token;
        std::memcpy(&token, m_data + m_position * sizeof(u32), sizeof(u32));
        return token;
    }

    void skip_tokens(usize count) {
        m_position += count;
    }

    void set_position(usize position) { m_position = position; }

    [[nodiscard]] usize position() const { return m_position; }

    [[nodiscard]] bool decode_operand(Sm4Operand& op);
    [[nodiscard]] bool decode_operand_tokens(Sm4Operand& op);
    [[nodiscard]] Sm4Opcode decode_opcode(u32 token) const {
        return static_cast<Sm4Opcode>(token & 0x7FF);
    }
    [[nodiscard]] u32 decode_instruction_length(u32 token) const {
        return (token >> 24) & 0x7F;
    }

    /// Uniform indexing of t#/s# (D3D11.3 7.11) is translated once per slot
    /// the index can hold: while an override is set, the Nth top-level
    /// operand decoded (0 = the first after the opcode tokens) comes back
    /// with index0 = the override and no relative index0.
    void set_index0_override(u32 ordinal, u32 index0) {
        m_index0_overrides[ordinal] = index0;
    }
    void clear_index0_overrides() { m_index0_overrides.clear(); }
    void begin_instruction_operands() { m_operand_ordinal = 0; }

    /// Decode instruction operands of a shader model 5.1 program: t#/s#/u#/
    /// cb# operands carry a range ID and a register (Sm4Operand::
    /// has_register_index). Not for declarations, whose 3-D operand is
    /// [range ID][lower][upper].
    void set_sm51_resource_operands(bool on) { m_sm51_resource_operands = on; }

private:
    bool m_sm51{false};
    bool m_sm51_resource_operands{false};
    /// The relative register of the last operand's index1 and its index2
    /// (immediate part and relative register), kept for the 5.1
    /// normalisation in decode_operand.
    struct IndexCapture {
        u32 immediate{0};
        bool relative{false};
        Sm4OperandType type{Sm4OperandType::Temp};
        u32 index{0};
        u32 component{0};
    };
    IndexCapture m_index1_relative;
    IndexCapture m_index2;
    std::map<u32, u32> m_index0_overrides;
    u32 m_operand_ordinal{0};
    u32 m_operand_depth{0};
    const u8* m_data;
    usize m_size;
    usize m_position;
};

std::vector<std::array<float, 4>> decode_immediate_constant_buffer(
    const std::vector<u8>& bytecode);

} // namespace vitreo::graphics::dxbc_spirv_detail
