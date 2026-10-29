// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file sm3_to_msl_internal.hpp
/// @brief Translator, shared by the sm3_to_msl*.cpp translation units.

#include "sm3_to_msl.hpp"

#include <algorithm>
#include <array>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace vitreo::graphics::sm3_msl_detail {

// -----------------------------------------------------------------------------
// Token layout
// -----------------------------------------------------------------------------
//
// Version token:  0xFFFE0000 | (major << 8) | minor  for a vertex shader,
//                 0xFFFF0000 | (major << 8) | minor  for a pixel shader.
//
// Instruction:    [15:0] opcode, [23:16] control, [27:24] length in tokens
//                 (shader model 2 and later), [28] predicated, [31] 0.
//
// Parameter:      [10:0] register number, [12:11] register type low bits,
//                 [30:28] register type high bits, [31] 1. A destination adds
//                 [19:16] write mask, [23:20] result modifier, [27:24] shift;
//                 a source adds [15:13] relative addressing, [23:16] swizzle,
//                 [27:24] source modifier.

constexpr u32 OPCODE_MASK = 0x0000FFFF;
constexpr u32 CONTROL_SHIFT = 16;
constexpr u32 CONTROL_MASK = 0xFF;
constexpr u32 LENGTH_SHIFT = 24;
constexpr u32 LENGTH_MASK = 0xF;
constexpr u32 PREDICATED_BIT = 1u << 28;

constexpr u32 OPCODE_END = 0xFFFF;
constexpr u32 OPCODE_COMMENT = 0xFFFE;
constexpr u32 COMMENT_LENGTH_SHIFT = 16;
constexpr u32 COMMENT_LENGTH_MASK = 0x7FFF;

enum Opcode : u32 {
    OP_NOP = 0, OP_MOV = 1, OP_ADD = 2, OP_SUB = 3, OP_MAD = 4, OP_MUL = 5,
    OP_RCP = 6, OP_RSQ = 7, OP_DP3 = 8, OP_DP4 = 9, OP_MIN = 10, OP_MAX = 11,
    OP_SLT = 12, OP_SGE = 13, OP_EXP = 14, OP_LOG = 15, OP_LIT = 16,
    OP_DST = 17, OP_LRP = 18, OP_FRC = 19,
    OP_M4X4 = 20, OP_M4X3 = 21, OP_M3X4 = 22, OP_M3X3 = 23, OP_M3X2 = 24,
    OP_CALL = 25, OP_CALLNZ = 26, OP_LOOP = 27, OP_RET = 28, OP_ENDLOOP = 29,
    OP_LABEL = 30, OP_DCL = 31, OP_POW = 32, OP_CRS = 33, OP_SGN = 34,
    OP_ABS = 35, OP_NRM = 36, OP_SINCOS = 37, OP_REP = 38, OP_ENDREP = 39,
    OP_IF = 40, OP_IFC = 41, OP_ELSE = 42, OP_ENDIF = 43, OP_BREAK = 44,
    OP_BREAKC = 45, OP_MOVA = 46, OP_DEFB = 47, OP_DEFI = 48,
    OP_TEXCOORD = 64, OP_TEXKILL = 65, OP_TEX = 66,
    OP_TEXBEM = 67, OP_TEXBEML = 68, OP_TEXREG2AR = 69, OP_TEXREG2GB = 70,
    OP_TEXM3X2PAD = 71, OP_TEXM3X2TEX = 72, OP_TEXM3X3PAD = 73,
    OP_TEXM3X3TEX = 74, OP_RESERVED0 = 75, OP_TEXM3X3SPEC = 76,
    OP_TEXM3X3VSPEC = 77, OP_EXPP = 78, OP_LOGP = 79, OP_CND = 80,
    OP_DEF = 81, OP_TEXREG2RGB = 82, OP_TEXDP3TEX = 83, OP_TEXM3X2DEPTH = 84,
    OP_TEXDP3 = 85, OP_TEXM3X3 = 86, OP_TEXDEPTH = 87, OP_CMP = 88,
    OP_BEM = 89, OP_DP2ADD = 90, OP_DSX = 91, OP_DSY = 92, OP_TEXLDD = 93,
    OP_SETP = 94, OP_TEXLDL = 95, OP_BREAKP = 96,
};

enum RegisterType : u32 {
    REG_TEMP = 0, REG_INPUT = 1, REG_CONST = 2, REG_ADDR = 3, REG_RASTOUT = 4,
    REG_ATTROUT = 5, REG_OUTPUT = 6, REG_CONSTINT = 7, REG_COLOROUT = 8,
    REG_DEPTHOUT = 9, REG_SAMPLER = 10, REG_CONST2 = 11, REG_CONST3 = 12,
    REG_CONST4 = 13, REG_CONSTBOOL = 14, REG_LOOP = 15, REG_TEMPFLOAT16 = 16,
    REG_MISCTYPE = 17, REG_LABEL = 18, REG_PREDICATE = 19,
};

enum SourceModifier : u32 {
    SRCMOD_NONE = 0, SRCMOD_NEG = 1, SRCMOD_BIAS = 2, SRCMOD_BIASNEG = 3,
    SRCMOD_SIGN = 4, SRCMOD_SIGNNEG = 5, SRCMOD_COMP = 6, SRCMOD_X2 = 7,
    SRCMOD_X2NEG = 8, SRCMOD_DZ = 9, SRCMOD_DW = 10, SRCMOD_ABS = 11,
    SRCMOD_ABSNEG = 12, SRCMOD_NOT = 13,
};

constexpr u32 DSTMOD_SATURATE = 0x1;
constexpr u32 DSTMOD_PARTIALPRECISION = 0x2;
constexpr u32 DSTMOD_CENTROID = 0x4;

struct Parameter {
    u32 type{REG_TEMP};
    u32 index{0};
    // Destination
    u32 write_mask{0xF};
    u32 result_modifier{0};
    u32 shift{0};
    // Source
    u32 swizzle{0xE4}; // .xyzw
    u32 source_modifier{SRCMOD_NONE};
    bool relative{false};
    /// Component of a0 used for relative constant indexing (0=x..3=w).
    u32 relative_component{0};
    /// Relative address is aL (loop counter) rather than a0.
    bool relative_loop{false};
};

[[nodiscard]] inline u32 register_type_of(u32 token) {
    // D3DSI_GETREGTYPE: low three bits from 30:28, next two from 12:11
    // shifted to bit positions 4:3 (D3DSP_REGTYPE_MASK2 >> 8).
    return ((token & 0x70000000u) >> 28) | ((token & 0x00001800u) >> 8);
}

[[nodiscard]] inline Parameter decode_destination(u32 token) {
    Parameter p;
    p.type = register_type_of(token);
    p.index = token & 0x7FF;
    p.write_mask = (token >> 16) & 0xF;
    p.result_modifier = (token >> 20) & 0xF;
    p.shift = (token >> 24) & 0xF;
    return p;
}

[[nodiscard]] inline Parameter decode_source(u32 token) {
    Parameter p;
    p.type = register_type_of(token);
    p.index = token & 0x7FF;
    p.relative = ((token >> 13) & 0x3) != 0;
    p.swizzle = (token >> 16) & 0xFF;
    p.source_modifier = (token >> 24) & 0xF;
    return p;
}

[[nodiscard]] inline std::string mask_to_string(u32 mask) {
    if ((mask & 0xF) == 0xF) return "";
    std::string result = ".";
    if ((mask & 0x1) != 0) result += 'x';
    if ((mask & 0x2) != 0) result += 'y';
    if ((mask & 0x4) != 0) result += 'z';
    if ((mask & 0x8) != 0) result += 'w';
    return result;
}

[[nodiscard]] inline u32 mask_component_count(u32 mask) {
    u32 count = 0;
    for (u32 bit = 0; bit < 4; ++bit) {
        if ((mask & (1u << bit)) != 0) ++count;
    }
    return count;
}

/// MSL type name for a value of @p components components.
[[nodiscard]] inline std::string vector_type(u32 components) {
    switch (components) {
    case 1:  return "float";
    case 2:  return "float2";
    case 3:  return "float3";
    default: return "float4";
    }
}

[[nodiscard]] inline std::string int_vector_type(u32 components) {
    switch (components) {
    case 1:  return "int";
    case 2:  return "int2";
    case 3:  return "int3";
    default: return "int4";
    }
}

/// A swizzle narrowed to the components a destination write mask selects.
/// `.xyzw` on a full write is the identity and is elided.
[[nodiscard]] inline std::string swizzle_for_mask(u32 swizzle, u32 mask) {
    constexpr char components[] = "xyzw";
    if ((mask & 0xF) == 0xF && swizzle == 0xE4) return "";
    std::string result = ".";
    for (u32 i = 0; i < 4; ++i) {
        if ((mask & (1u << i)) == 0) continue;
        result += components[(swizzle >> (i * 2)) & 0x3];
    }
    return result;
}

// =============================================================================
// Translator
// =============================================================================

class Translator {
public:
    Translator(const u32* tokens, usize count, const u32* stage_ttf,
               usize stage_ttf_count)
        : m_tokens(tokens), m_count(count) {
        if (stage_ttf != nullptr && stage_ttf_count > 0) {
            const usize n = std::min(stage_ttf_count, m_stage_ttf.size());
            for (usize i = 0; i < n; ++i) m_stage_ttf[i] = stage_ttf[i];
        }
    }

    Sm3Shader run();

private:
    [[nodiscard]] bool fail(const std::string& reason) {
        m_result.ok = false;
        m_result.failure = reason;
        return false;
    }

    [[nodiscard]] bool has(usize needed) const {
        return m_position + needed <= m_count;
    }

    [[nodiscard]] u32 take() { return m_tokens[m_position++]; }

    [[nodiscard]] bool parse_version();
    [[nodiscard]] bool parse_instruction(bool& finished);
    [[nodiscard]] bool parse_predicated_instruction(u32 token, bool& finished);
    [[nodiscard]] bool parse_instruction_body(u32 token, bool& finished);
    [[nodiscard]] bool handle_declaration(u32 control);
    [[nodiscard]] bool handle_define();
    [[nodiscard]] bool handle_define_int();
    [[nodiscard]] bool handle_define_bool();
    /// int4 expression for an `i#` source (`defiN` or `cb.i[N]`).
    [[nodiscard]] bool int_constant_expression(const Parameter& p,
                                               std::string& out);
    [[nodiscard]] bool enclosing_loop_counter() const;
    void flush_body();
    [[nodiscard]] std::string subroutine_param_list() const;
    [[nodiscard]] std::string subroutine_arg_list() const;
    [[nodiscard]] std::string expand_calls(std::string body) const;
    void emit_call(u32 label);

    /// Reference to a source operand, narrowed to @p mask and with the
    /// operand's modifier applied.
    [[nodiscard]] bool source_expression(const Parameter& p, u32 mask,
                                         std::string& out);
    [[nodiscard]] bool destination_name(const Parameter& p, std::string& out);
    /// Emit `dst<mask> = <expression>;`, honouring the saturate modifier.
    [[nodiscard]] bool assign(const Parameter& dst,
                              const std::string& expression);
    /// Emit an assignment whose right-hand side is a scalar replicated across
    /// every written component.
    [[nodiscard]] bool assign_scalar(const Parameter& dst,
                                     const std::string& scalar);

    /// Emit a 2D UV expression for SM1 `tex`, honouring runtime
    /// D3DTTFF_PROJECTED (COUNT3→÷z, COUNT4→÷w) for the sampler stage.
    [[nodiscard]] std::string sm1_sample_uv(u32 stage,
                                            const std::string& tc4) const {
        const u32 ttf =
            stage < m_stage_ttf.size() ? m_stage_ttf[stage] : 0u;
        const u32 count = ttf & 0xFFu;
        const bool projected = (ttf & 0x100u) != 0;
        // MSDN: with a programmable VS, COUNT* should be 0 and projection
        // still divides by the last element (.w). COUNT3 → ÷z, COUNT4 → ÷w.
        if (projected) {
            const char* denom = (count == 3u) ? "z" : "w";
            return "((" + tc4 + ").xy / max(abs((" + tc4 + ")." + denom +
                   "), 1e-6))";
        }
        return "(" + tc4 + ").xy";
    }

    void note_constant(u32 index) {
        if (index >= SM3_MAX_FLOAT_CONSTANT_REGISTERS) return;
        if (index + 1 > m_result.float_constant_count) {
            m_result.float_constant_count = index + 1;
        }
    }

    void note_int_constant(u32 index) {
        if (index >= SM3_MAX_INT_CONSTANT_REGISTERS) return;
        if (index + 1 > m_result.int_constant_count) {
            m_result.int_constant_count = index + 1;
        }
    }

    void note_bool_constant(u32 index) {
        if (index >= SM3_MAX_BOOL_CONSTANT_REGISTERS) return;
        if (index + 1 > m_result.bool_constant_count) {
            m_result.bool_constant_count = index + 1;
        }
    }

    /// Absolute float-constant index for Const / Const2 / Const3 / Const4.
    /// D3D9 hardware only exposes 256 float4 constants. Effects-framework
    /// bank encodings (Const2/3/4 at +2048/+4096/+6144) that land past that
    /// file are folded back to the low 11-bit index — the only values a D3D9
    /// device can bind — rather than inventing a 4k-entry constant buffer.
    [[nodiscard]] bool constant_index(const Parameter& p, u32& out) {
        u32 base = 0;
        switch (p.type) {
        case REG_CONST:  base = 0; break;
        case REG_CONST2: base = 2048; break;
        case REG_CONST3: base = 4096; break;
        case REG_CONST4: base = 6144; break;
        default: return false;
        }
        u32 index = base + p.index;
        if (index >= SM3_MAX_FLOAT_CONSTANT_REGISTERS) {
            index = p.index;
            if (index >= SM3_MAX_FLOAT_CONSTANT_REGISTERS) {
                return fail("constant register c" + std::to_string(index) +
                            " above the D3D9 float-constant file");
            }
        }
        out = index;
        return true;
    }

    [[nodiscard]] std::string build_msl() const;

    const u32* m_tokens;
    usize m_count;
    usize m_position{0};
    /// D3DTSS_TEXTURETRANSFORMFLAGS per sampler stage (ps_1_x projective).
    std::array<u32, 16> m_stage_ttf{};
    /// D3DSTT_* from `dcl_* s#` bits 27:24 (2 = 2D, 3 = cube, 4 = volume).
    /// Zero means undeclared; treated as 2D at sample / prologue time.
    std::array<u8, 16> m_sampler_dim{};

    Sm3Shader m_result;
    std::ostringstream m_body;
    /// Accumulated main / label bodies for call/ret/label subroutines.
    std::string m_main_body;
    std::map<u32, std::string> m_label_bodies;
    std::optional<u32> m_current_label;
    std::set<u32> m_labels_defined;
    std::set<u32> m_labels_called;
    bool m_uses_subroutines{false};
    /// After `ret` from a label, only another label (or END) may follow.
    bool m_expect_label_or_end{false};
    std::set<u32> m_temps_used;
    /// Pixel-shader texture registers t# (REG_ADDR), writable in SM1.
    std::set<u32> m_texregs_used;
    /// Vertex-shader address register a0 (REG_ADDR as destination).
    bool m_address_register_used{false};
    /// Shader uses c[a0+n] / c[aL+n]; MSL must return zeros out of range.
    bool m_relative_const_used{false};
    /// Pixel-shader vFace (REG_MISCTYPE index 1).
    bool m_vface_used{false};
    /// Pixel-shader oDepth write.
    bool m_depth_written{false};
    std::set<u32> m_predicates_used;
    /// Open if/ifc nesting depth; else/endif require a non-zero value.
    u32 m_flow_depth{0};
    enum class LoopKind : u8 { Loop, Rep };
    /// Open loop/rep blocks (Learn nesting); id suffixes generated MSL names.
    std::vector<std::pair<LoopKind, u32>> m_loop_stack;
    u32 m_loop_serial{0};
    std::set<u32> m_outputs_written;
    std::set<u32> m_colors_written;
    std::set<u32> m_texcoords_written;
    bool m_position_written{false};
    /// oPts / PSIZE writes remain a local until point-list pipeline
    /// specialization adds Metal's [[point_size]] output and clamp.
    bool m_need_pts_sink{false};
    /// `def c#` constants, emitted as literals rather than uniform reads.
    std::map<u32, std::array<float, 4>> m_defined_constants;
    /// `defi i#` immediate integer constants (Learn: take precedence over API).
    std::map<u32, std::array<i32, 4>> m_defined_int_constants;
    /// `defb b#` immediate boolean constants.
    std::map<u32, bool> m_defined_bool_constants;
    /// Vertex shader output declarations, `dcl_usage o#` in SM3.
    std::map<u32, Sm3InputElement> m_output_declarations;
    /// Pixel shader input declarations, `dcl_usage v#` in SM3.
    std::map<u32, Sm3InputElement> m_input_declarations;
};

} // namespace vitreo::graphics::sm3_msl_detail
