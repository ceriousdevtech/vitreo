// SPDX-License-Identifier: LGPL-2.1-only
/// @file d3d9_pv_analyze.cpp
/// @brief ProcessVertices bytecode analysis: decodes a SM2/SM3 vertex shader
///        into the `PvCpuPlan` the CPU interpreter runs.

#include "winapi/graphics/d3d9/d3d9_pv_cpu.hpp"

#include <array>
#include <cstring>
#include <map>
#include <vector>

namespace vitreo::winapi::d3d9_pv {

namespace {

constexpr u32 PV_OPCODE_MASK = 0x0000FFFFu;
constexpr u32 PV_LENGTH_SHIFT = 24;
constexpr u32 PV_PREDICATED_BIT = 1u << 28;
constexpr u32 PV_OPCODE_END = 0xFFFFu;
constexpr u32 PV_OPCODE_COMMENT = 0xFFFEu;
constexpr u32 PV_COMMENT_LENGTH_SHIFT = 16;
constexpr u32 PV_COMMENT_LENGTH_MASK = 0x7FFFu;

enum PvOpcode : u32 {
    PV_NOP = 0,
    PV_MOV = 1,
    PV_ADD = 2,
    PV_SUB = 3,
    PV_MAD = 4,
    PV_MUL = 5,
    PV_RCP = 6,
    PV_RSQ = 7,
    PV_DP3 = 8,
    PV_DP4 = 9,
    PV_MIN = 10,
    PV_MAX = 11,
    PV_SLT = 12,
    PV_SGE = 13,
    PV_EXP = 14,
    PV_LOG = 15,
    PV_LIT = 16,
    PV_DST = 17,
    PV_LRP = 18,
    PV_FRC = 19,
    PV_M4X4 = 20,
    PV_M4X3 = 21,
    PV_M3X4 = 22,
    PV_M3X3 = 23,
    PV_M3X2 = 24,
    PV_CALL = 25,
    PV_CALLNZ = 26,
    PV_LOOP = 27,
    PV_RET = 28,
    PV_ENDLOOP = 29,
    PV_LABEL = 30,
    PV_DCL = 31,
    PV_POW = 32,
    PV_CRS = 33,
    PV_SGN = 34,
    PV_ABS = 35,
    PV_NRM = 36,
    PV_SINCOS = 37,
    PV_REP = 38,
    PV_ENDREP = 39,
    PV_IF = 40,
    PV_IFC = 41,
    PV_ELSE = 42,
    PV_ENDIF = 43,
    PV_BREAK = 44,
    PV_BREAKC = 45,
    PV_MOVA = 46,
    PV_DEFB = 47,
    PV_DEFI = 48,
    PV_EXPP = 78,
    PV_LOGP = 79,
    PV_DEF = 81,
    PV_CMP = 88,
    PV_SETP = 94,
    PV_BREAKP = 96,
};

[[nodiscard]] u32 pv_reg_type(u32 token) {
    return ((token >> 28) & 0x7u) | (((token >> 11) & 0x3u) << 3);
}

[[nodiscard]] PvParam pv_decode_dst(u32 token) {
    PvParam p;
    p.type = pv_reg_type(token);
    p.index = token & 0x7FFu;
    p.write_mask = (token >> 16) & 0xFu;
    p.result_modifier = (token >> 20) & 0xFu;
    p.shift = (token >> 24) & 0xFu;
    return p;
}

[[nodiscard]] PvParam pv_decode_src(u32 token) {
    PvParam p;
    p.type = pv_reg_type(token);
    p.index = token & 0x7FFu;
    p.swizzle = (token >> 16) & 0xFFu;
    p.source_modifier = (token >> 24) & 0xFu;
    p.relative = ((token >> 13) & 0x7u) != 0;
    return p;
}

[[nodiscard]] bool pv_accept_src(const PvParam& p, const char*& rejection) {
    if (p.relative && p.type != PV_REG_CONST) {
        rejection = "relative addressing of a non-constant register";
        return false;
    }
    switch (p.source_modifier) {
    case PV_SRCMOD_NONE:
    case PV_SRCMOD_NEG:
    case PV_SRCMOD_BIAS:
    case PV_SRCMOD_BIASNEG:
    case PV_SRCMOD_SIGN:
    case PV_SRCMOD_SIGNNEG:
    case PV_SRCMOD_ABS:
    case PV_SRCMOD_ABSNEG:
        break;
    default:
        rejection = "a source modifier ProcessVertices VS cannot apply";
        return false;
    }
    switch (p.type) {
    case PV_REG_TEMP:
        if (p.index >= PV_MAX_TEMPS) {
            rejection = "a temp register above r31";
            return false;
        }
        return true;
    case PV_REG_INPUT:
        if (p.index >= PV_MAX_INPUTS) {
            rejection = "an input register above v15";
            return false;
        }
        return true;
    case PV_REG_CONST:
        // Relative c[aL+n] / c[a0+n]: base index may be any in-range c#;
        // runtime clamps the resolved index.
        if (p.index >= graphics::SM3_MAX_FLOAT_CONSTANT_REGISTERS) {
            rejection = "a constant register above c255";
            return false;
        }
        return true;
    default:
        rejection = "a source register type ProcessVertices VS cannot read";
        return false;
    }
}

[[nodiscard]] u32 pv_mask_component_count(u32 mask) {
    u32 count = 0;
    for (u32 bit = 0; bit < 4; ++bit) {
        if ((mask & (1u << bit)) != 0) ++count;
    }
    return count;
}

[[nodiscard]] bool pv_accept_dst(const PvParam& p, PvCpuPlan& plan,
                                 const char*& rejection) {
    if (p.shift != 0) {
        rejection = "a destination shift in ProcessVertices VS";
        return false;
    }
    if ((p.result_modifier &
         ~(PV_DSTMOD_SATURATE | PV_DSTMOD_PARTIALPRECISION |
           PV_DSTMOD_CENTROID)) != 0) {
        rejection = "a destination modifier ProcessVertices VS cannot apply";
        return false;
    }
    if (p.write_mask == 0) {
        rejection = "a destination with an empty write mask";
        return false;
    }
    switch (p.type) {
    case PV_REG_TEMP:
        if (p.index >= PV_MAX_TEMPS) {
            rejection = "a temp register above r31";
            return false;
        }
        return true;
    case PV_REG_RASTOUT:
        if (p.index == 0) {
            plan.writes_opos = true;
            return true;
        }
        rejection = "oFog/oPts writes in ProcessVertices VS";
        return false;
    case PV_REG_ATTROUT:
        if (p.index >= PV_MAX_COLORS) {
            rejection = "an attribute output above oD1";
            return false;
        }
        plan.writes_color[p.index] = true;
        return true;
    case PV_REG_OUTPUT: {
        const auto declared = plan.output_decls.find(p.index);
        if (declared != plan.output_decls.end()) {
            if (declared->second.usage == graphics::Sm3Usage::Position) {
                plan.writes_opos = true;
                return true;
            }
            if (declared->second.usage == graphics::Sm3Usage::Color) {
                if (declared->second.usage_index >= PV_MAX_COLORS) {
                    rejection = "a colour output above oD1";
                    return false;
                }
                plan.writes_color[declared->second.usage_index] = true;
                return true;
            }
            if (declared->second.usage == graphics::Sm3Usage::TexCoord) {
                if (declared->second.usage_index >= PV_MAX_TEXCOORDS) {
                    rejection = "a texcoord output above oT7";
                    return false;
                }
                plan.writes_texcoord[declared->second.usage_index] = true;
                return true;
            }
            rejection = "an output usage ProcessVertices VS cannot write";
            return false;
        }
        // SM2-style oT#: register index is the texcoord set.
        if (p.index >= PV_MAX_TEXCOORDS) {
            rejection = "a texcoord output above oT7";
            return false;
        }
        plan.writes_texcoord[p.index] = true;
        return true;
    }
    default:
        rejection = "a destination register type ProcessVertices VS cannot write";
        return false;
    }
}

} // namespace

[[nodiscard]] bool analyze_vs_for_process_vertices(const u32* tokens,
                                                   usize token_count,
                                                   PvCpuPlan& plan) {
    plan = PvCpuPlan{};
    if (tokens == nullptr || token_count < 2) {
        plan.rejection = "truncated vertex shader bytecode";
        return false;
    }
    const u32 version = tokens[0];
    if ((version & 0xFFFF0000u) != 0xFFFE0000u) {
        plan.rejection = "non-vertex shader bytecode";
        return false;
    }
    const u32 major = (version >> 8) & 0xFFu;
    if (major < 2 || major > 3) {
        plan.rejection = "a vertex shader model ProcessVertices cannot run";
        return false;
    }
    plan.max_call_depth =
        major >= 3 ? PV_MAX_CALL_DEPTH_VS3 : PV_MAX_CALL_DEPTH_VS2;

    usize pos = 1;
    const auto has = [&](usize n) { return pos + n <= token_count; };
    const auto take = [&]() -> u32 { return tokens[pos++]; };
    // Open if nesting; value is true once the matching else has been seen.
    std::vector<bool> if_has_else;
    // Open loop/rep blocks for partner patching (Learn nesting).
    enum class PvLoopKind : u8 { Loop, Rep };
    struct PvOpenLoop {
        PvLoopKind kind{PvLoopKind::Loop};
        u32 start_pc{0};
        std::vector<u32> breaks;
    };
    std::vector<PvOpenLoop> loop_stack;
    // Learn / sm3_to_msl: call/label/ret subroutines (forward labels only).
    std::map<u32, u32> label_pcs;
    std::vector<u32> call_pcs;
    bool in_label = false;
    u32 current_label_pc = 0;
    bool expect_label_or_end = false;

    while (has(1)) {
        const u32 token = take();
        const u32 opcode = token & PV_OPCODE_MASK;
        if (opcode == PV_OPCODE_END) {
            if (!if_has_else.empty()) {
                plan.rejection = "an if without a matching endif";
                return false;
            }
            if (!loop_stack.empty()) {
                plan.rejection = "an unclosed loop or rep block";
                return false;
            }
            if (in_label) {
                plan.rejection = "an unclosed label block";
                return false;
            }
            for (u32 call_pc : call_pcs) {
                PvInstr& call = plan.code[call_pc];
                const auto found = label_pcs.find(call.int_reg);
                if (found == label_pcs.end()) {
                    plan.rejection = "call to undefined label";
                    return false;
                }
                if (found->second <= call_pc) {
                    plan.rejection = "a backward call in ProcessVertices VS";
                    return false;
                }
                call.partner = found->second;
            }
            if (!plan.writes_opos) {
                plan.rejection = "a vertex shader that never writes oPos";
                return false;
            }
            plan.ok = true;
            return true;
        }
        if (opcode == PV_OPCODE_COMMENT) {
            const u32 length =
                (token >> PV_COMMENT_LENGTH_SHIFT) & PV_COMMENT_LENGTH_MASK;
            if (!has(length)) {
                plan.rejection = "a truncated comment block";
                return false;
            }
            pos += length;
            continue;
        }
        const bool predicated = (token & PV_PREDICATED_BIT) != 0;
        if (expect_label_or_end && opcode != PV_LABEL) {
            plan.rejection = "code after ret must begin a new label";
            return false;
        }
        if (opcode == PV_NOP) {
            if (predicated) {
                plan.rejection = "a predicated nop in ProcessVertices VS";
                return false;
            }
            continue;
        }

        if (opcode == PV_DCL) {
            if (predicated) {
                plan.rejection = "a predicated dcl in ProcessVertices VS";
                return false;
            }
            if (!has(2)) {
                plan.rejection = "a truncated dcl";
                return false;
            }
            const u32 usage_token = take();
            const PvParam dst = pv_decode_dst(take());
            const auto usage =
                static_cast<graphics::Sm3Usage>(usage_token & 0x1Fu);
            const u32 usage_index = (usage_token >> 16) & 0xFu;
            if (dst.type == PV_REG_INPUT) {
                if (dst.index >= PV_MAX_INPUTS) {
                    plan.rejection = "an input register above v15";
                    return false;
                }
                plan.inputs.push_back({usage, usage_index, dst.index});
                continue;
            }
            if (dst.type == PV_REG_OUTPUT) {
                plan.output_decls.emplace(
                    dst.index,
                    graphics::Sm3InputElement{usage, usage_index, dst.index});
                continue;
            }
            plan.rejection = "a dcl target ProcessVertices VS cannot use";
            return false;
        }

        if (opcode == PV_DEF) {
            if (predicated) {
                plan.rejection = "a predicated def in ProcessVertices VS";
                return false;
            }
            if (!has(5)) {
                plan.rejection = "a truncated def";
                return false;
            }
            const PvParam dst = pv_decode_dst(take());
            std::array<float, 4> value{};
            for (usize i = 0; i < 4; ++i) {
                const u32 bits = take();
                std::memcpy(&value[i], &bits, sizeof(float));
            }
            if (dst.type != PV_REG_CONST ||
                dst.index >= graphics::SM3_MAX_FLOAT_CONSTANT_REGISTERS) {
                plan.rejection = "a def on a non-float constant";
                return false;
            }
            plan.def_const[dst.index] = value;
            plan.def_const_set[dst.index] = true;
            continue;
        }

        if (opcode == PV_DEFB) {
            // Learn: defb - vs — one boolean immediate; overrides ConstantB.
            if (predicated) {
                plan.rejection = "a predicated defb in ProcessVertices VS";
                return false;
            }
            if (!has(2)) {
                plan.rejection = "a truncated defb";
                return false;
            }
            const PvParam dst = pv_decode_dst(take());
            const u32 raw = take();
            if (dst.type != PV_REG_CONSTBOOL ||
                dst.index >= graphics::SM3_MAX_BOOL_CONSTANT_REGISTERS) {
                plan.rejection = "a defb on a non-boolean constant";
                return false;
            }
            plan.def_bool[dst.index] = raw != 0;
            plan.def_bool_set[dst.index] = true;
            continue;
        }

        if (opcode == PV_DEFI) {
            // Learn: defi - vs — four signed ints; override ConstantI (sm3_to_msl).
            if (predicated) {
                plan.rejection = "a predicated defi in ProcessVertices VS";
                return false;
            }
            if (!has(5)) {
                plan.rejection = "a truncated defi";
                return false;
            }
            const PvParam dst = pv_decode_dst(take());
            std::array<i32, 4> value{};
            for (usize i = 0; i < 4; ++i) {
                value[i] = static_cast<i32>(take());
            }
            if (dst.type != PV_REG_CONSTINT ||
                dst.index >= D3D9_MAX_SHADER_CONST_I) {
                plan.rejection = "a defi on a non-integer constant";
                return false;
            }
            plan.def_int[dst.index] = value;
            plan.def_int_set[dst.index] = true;
            continue;
        }

        PvInstr instr{};
        auto read_dst = [&](PvParam& out) -> bool {
            if (!has(1)) return false;
            out = pv_decode_dst(take());
            return pv_accept_dst(out, plan, plan.rejection);
        };
        auto read_src = [&](PvParam& out) -> bool {
            if (!has(1)) {
                plan.rejection = "a truncated instruction";
                return false;
            }
            out = pv_decode_src(take());
            // vs_2_0+ encodes the address register in a following source token
            // when relative addressing is set (a0 or aL). Match sm3_to_msl.
            if (out.relative && major >= 2) {
                if (!has(1)) {
                    plan.rejection = "a truncated relative address token";
                    return false;
                }
                const PvParam addr = pv_decode_src(take());
                if (addr.type == PV_REG_LOOP) {
                    out.relative_loop = true;
                    out.relative_component = 0;
                } else if (addr.type == PV_REG_ADDR) {
                    out.relative_loop = false;
                    out.relative_component = addr.swizzle & 0x3u;
                } else {
                    plan.rejection =
                        "relative addressing without an address register";
                    return false;
                }
            } else if (out.relative) {
                // vs_1_1: a0.x is implicit (ProcessVertices still refuses SM1).
                out.relative_loop = false;
                out.relative_component = 0;
            }
            return pv_accept_src(out, plan.rejection);
        };

        switch (opcode) {
        case PV_MOV:
            instr.op = PvOp::Mov;
            if (!read_dst(instr.dst) || !read_src(instr.src0)) return false;
            break;
        case PV_ADD:
            instr.op = PvOp::Add;
            if (!read_dst(instr.dst) || !read_src(instr.src0) ||
                !read_src(instr.src1)) {
                return false;
            }
            break;
        case PV_SUB:
            instr.op = PvOp::Sub;
            if (!read_dst(instr.dst) || !read_src(instr.src0) ||
                !read_src(instr.src1)) {
                return false;
            }
            break;
        case PV_MUL:
            instr.op = PvOp::Mul;
            if (!read_dst(instr.dst) || !read_src(instr.src0) ||
                !read_src(instr.src1)) {
                return false;
            }
            break;
        case PV_RCP:
            instr.op = PvOp::Rcp;
            if (!read_dst(instr.dst) || !read_src(instr.src0)) return false;
            break;
        case PV_RSQ:
            instr.op = PvOp::Rsq;
            if (!read_dst(instr.dst) || !read_src(instr.src0)) return false;
            break;
        case PV_MIN:
            instr.op = PvOp::Min;
            if (!read_dst(instr.dst) || !read_src(instr.src0) ||
                !read_src(instr.src1)) {
                return false;
            }
            break;
        case PV_MAX:
            instr.op = PvOp::Max;
            if (!read_dst(instr.dst) || !read_src(instr.src0) ||
                !read_src(instr.src1)) {
                return false;
            }
            break;
        case PV_DP3:
            instr.op = PvOp::Dp3;
            if (!read_dst(instr.dst) || !read_src(instr.src0) ||
                !read_src(instr.src1)) {
                return false;
            }
            break;
        case PV_DP4:
            instr.op = PvOp::Dp4;
            if (!read_dst(instr.dst) || !read_src(instr.src0) ||
                !read_src(instr.src1)) {
                return false;
            }
            break;
        case PV_MAD:
            instr.op = PvOp::Mad;
            if (!read_dst(instr.dst) || !read_src(instr.src0) ||
                !read_src(instr.src1) || !read_src(instr.src2)) {
                return false;
            }
            break;
        case PV_LRP:
            // Learn: lrp - vs — dest = src0*(src1-src2)+src2 (mix(src2,src1,src0)).
            instr.op = PvOp::Lrp;
            if (!read_dst(instr.dst) || !read_src(instr.src0) ||
                !read_src(instr.src1) || !read_src(instr.src2)) {
                return false;
            }
            break;
        case PV_CMP:
            // Learn: cmp - ps (same ternary in sm3_to_msl) —
            // dest = (src0 >= 0) ? src1 : src2 per channel.
            instr.op = PvOp::Cmp;
            if (!read_dst(instr.dst) || !read_src(instr.src0) ||
                !read_src(instr.src1) || !read_src(instr.src2)) {
                return false;
            }
            break;
        case PV_M4X4:
        case PV_M4X3:
        case PV_M3X4:
        case PV_M3X3:
        case PV_M3X2: {
            // Learn / sm3_to_msl: mNxM is N-component dots against M consecutive
            // constant rows (src1..src1+M-1). Forced dest mask matches row count.
            instr.op = PvOp::Matrix;
            switch (opcode) {
            case PV_M4X4:
                instr.matrix_vector_size = 4;
                instr.matrix_row_count = 4;
                break;
            case PV_M4X3:
                instr.matrix_vector_size = 4;
                instr.matrix_row_count = 3;
                break;
            case PV_M3X4:
                instr.matrix_vector_size = 3;
                instr.matrix_row_count = 4;
                break;
            case PV_M3X3:
                instr.matrix_vector_size = 3;
                instr.matrix_row_count = 3;
                break;
            default: // PV_M3X2
                instr.matrix_vector_size = 3;
                instr.matrix_row_count = 2;
                break;
            }
            if (!read_dst(instr.dst) || !read_src(instr.src0) ||
                !read_src(instr.src1)) {
                return false;
            }
            if (instr.src1.type != PV_REG_CONST) {
                plan.rejection = "a matrix multiply with a non-constant matrix";
                return false;
            }
            if (instr.src1.index + instr.matrix_row_count - 1 >=
                graphics::SM3_MAX_FLOAT_CONSTANT_REGISTERS) {
                plan.rejection = "a matrix multiply past c255";
                return false;
            }
            instr.dst.write_mask =
                static_cast<u32>((1u << instr.matrix_row_count) - 1u);
            plan.writes_opos =
                plan.writes_opos || instr.dst.type == PV_REG_RASTOUT ||
                (instr.dst.type == PV_REG_OUTPUT &&
                 plan.output_decls.count(instr.dst.index) != 0 &&
                 plan.output_decls[instr.dst.index].usage ==
                     graphics::Sm3Usage::Position);
            break;
        }
        case PV_SINCOS: {
            // Learn: sincos - vs. dest.x = cos(V), dest.y = sin(V); mask must
            // be .x / .y / .xy on a temp. SM2 also consumes two Taylor c# srcs.
            instr.op = PvOp::Sincos;
            if (!read_dst(instr.dst) || !read_src(instr.src0)) return false;
            if (instr.dst.type != PV_REG_TEMP) {
                plan.rejection = "sincos to a non-temporary register";
                return false;
            }
            const u32 mask = instr.dst.write_mask;
            if (mask != 0x1 && mask != 0x2 && mask != 0x3) {
                plan.rejection = "sincos write mask (must be .x, .y, or .xy)";
                return false;
            }
            if (major < 3) {
                if (!read_src(instr.src1) || !read_src(instr.src2)) {
                    return false;
                }
            }
            break;
        }
        case PV_ABS:
            instr.op = PvOp::Abs;
            if (!read_dst(instr.dst) || !read_src(instr.src0)) return false;
            break;
        case PV_FRC:
            instr.op = PvOp::Frc;
            if (!read_dst(instr.dst) || !read_src(instr.src0)) return false;
            break;
        case PV_NRM:
            instr.op = PvOp::Nrm;
            if (!read_dst(instr.dst) || !read_src(instr.src0)) return false;
            break;
        case PV_SGN:
            // Learn: sgn dest, src0, src1, src2 — per-component -1/0/+1.
            // src1/src2 are scratch temps; consume them so the token stream
            // stays aligned (same contract as sm3_to_msl).
            instr.op = PvOp::Sgn;
            if (!read_dst(instr.dst) || !read_src(instr.src0) ||
                !read_src(instr.src1) || !read_src(instr.src2)) {
                return false;
            }
            break;
        case PV_SLT:
            instr.op = PvOp::Slt;
            if (!read_dst(instr.dst) || !read_src(instr.src0) ||
                !read_src(instr.src1)) {
                return false;
            }
            break;
        case PV_SGE:
            instr.op = PvOp::Sge;
            if (!read_dst(instr.dst) || !read_src(instr.src0) ||
                !read_src(instr.src1)) {
                return false;
            }
            break;
        case PV_EXP:
        case PV_EXPP:
            // Learn: exp/expp - vs (SM2+) — scalar exp2; expp is partial
            // precision (≥10 bits). CPU path matches sm3_to_msl (same exp2).
            instr.op = PvOp::Exp;
            if (!read_dst(instr.dst) || !read_src(instr.src0)) return false;
            break;
        case PV_LOG:
        case PV_LOGP:
            // Learn: log/logp - vs — scalar log2(|src|); logp is partial
            // precision. Guard zero like sm3_to_msl (max(abs, 1e-30)).
            instr.op = PvOp::Log;
            if (!read_dst(instr.dst) || !read_src(instr.src0)) return false;
            break;
        case PV_LIT:
            instr.op = PvOp::Lit;
            if (!read_dst(instr.dst) || !read_src(instr.src0)) return false;
            break;
        case PV_DST:
            instr.op = PvOp::Dst;
            if (!read_dst(instr.dst) || !read_src(instr.src0) ||
                !read_src(instr.src1)) {
                return false;
            }
            break;
        case PV_POW:
            // Learn: pow - vs — scalar pow(abs(src0), src1); replicate swizzle
            // on both srcs; result replicated to all channels.
            instr.op = PvOp::Pow;
            if (!read_dst(instr.dst) || !read_src(instr.src0) ||
                !read_src(instr.src1)) {
                return false;
            }
            break;
        case PV_CRS: {
            // Learn / sm3_to_msl: cross of xyz; refuse masks that are not
            // exactly three components written.
            instr.op = PvOp::Crs;
            if (!read_dst(instr.dst) || !read_src(instr.src0) ||
                !read_src(instr.src1)) {
                return false;
            }
            if (pv_mask_component_count(instr.dst.write_mask) != 3) {
                plan.rejection = "a cross product written to a partial mask";
                return false;
            }
            break;
        }
        case PV_MOVA: {
            // Learn: mova a0, src — round-to-nearest into the integer address
            // register (component write mask allowed on vs_2_x+).
            instr.op = PvOp::Mova;
            if (!has(1)) {
                plan.rejection = "a truncated instruction";
                return false;
            }
            instr.dst = pv_decode_dst(take());
            if (instr.dst.type != PV_REG_ADDR || instr.dst.index != 0) {
                plan.rejection = "mova to a non-address register";
                return false;
            }
            if (instr.dst.shift != 0) {
                plan.rejection = "a destination shift in ProcessVertices VS";
                return false;
            }
            if (instr.dst.write_mask == 0) {
                plan.rejection = "a destination with an empty write mask";
                return false;
            }
            if (!read_src(instr.src0)) return false;
            break;
        }
        case PV_SETP: {
            // Learn: setp_comp - vs — per-channel compare into p0.
            instr.op = PvOp::Setp;
            instr.setp_comp = static_cast<u8>((token >> 16) & 0xFFu);
            if (instr.setp_comp < 1 || instr.setp_comp > 6) {
                plan.rejection = "a setp comparison control ProcessVertices "
                                 "VS cannot apply";
                return false;
            }
            if (!has(1)) {
                plan.rejection = "a truncated instruction";
                return false;
            }
            instr.dst = pv_decode_dst(take());
            if (instr.dst.type != PV_REG_PREDICATE || instr.dst.index != 0) {
                plan.rejection = "setp destination must be a predicate register";
                return false;
            }
            if (instr.dst.shift != 0) {
                plan.rejection = "a destination shift in ProcessVertices VS";
                return false;
            }
            if ((instr.dst.result_modifier &
                 ~(PV_DSTMOD_SATURATE | PV_DSTMOD_PARTIALPRECISION |
                   PV_DSTMOD_CENTROID)) != 0) {
                plan.rejection =
                    "a destination modifier ProcessVertices VS cannot apply";
                return false;
            }
            if (instr.dst.write_mask == 0) {
                plan.rejection = "a destination with an empty write mask";
                return false;
            }
            if (!read_src(instr.src0) || !read_src(instr.src1)) return false;
            break;
        }
        case PV_LOOP: {
            // Learn: loop aL, i# — i.x=count[0,255], i.y=initial aL, i.z=step.
            if (predicated) {
                plan.rejection = "a predicated loop in ProcessVertices VS";
                return false;
            }
            if (loop_stack.size() >= PV_MAX_LOOP_NESTING) {
                plan.rejection = "loop nesting deeper than ProcessVertices "
                                 "supports";
                return false;
            }
            if (!has(2)) {
                plan.rejection = "a truncated instruction";
                return false;
            }
            instr.op = PvOp::Loop;
            instr.dst = pv_decode_dst(take());
            if (instr.dst.type != PV_REG_LOOP) {
                plan.rejection = "loop destination must be the aL register";
                return false;
            }
            instr.src0 = pv_decode_src(take());
            if (instr.src0.relative ||
                instr.src0.type != PV_REG_CONSTINT ||
                instr.src0.index >= D3D9_MAX_SHADER_CONST_I) {
                plan.rejection = "loop source must be an integer constant";
                return false;
            }
            instr.int_reg = instr.src0.index;
            const u32 start_pc = static_cast<u32>(plan.code.size());
            plan.code.push_back(instr);
            loop_stack.push_back(
                PvOpenLoop{PvLoopKind::Loop, start_pc, {}});
            continue;
        }
        case PV_ENDLOOP: {
            if (predicated) {
                plan.rejection = "a predicated endloop in ProcessVertices VS";
                return false;
            }
            if (loop_stack.empty() ||
                loop_stack.back().kind != PvLoopKind::Loop) {
                plan.rejection = "endloop without a matching loop";
                return false;
            }
            instr.op = PvOp::EndLoop;
            auto open = std::move(loop_stack.back());
            loop_stack.pop_back();
            const u32 end_pc = static_cast<u32>(plan.code.size());
            instr.partner = open.start_pc + 1; // body start
            plan.code[open.start_pc].partner = end_pc;
            plan.code.push_back(instr);
            for (u32 break_pc : open.breaks) {
                plan.code[break_pc].partner = end_pc + 1;
            }
            continue;
        }
        case PV_REP: {
            // Learn: rep i# — repeat i.x times; aL is not updated.
            if (predicated) {
                plan.rejection = "a predicated rep in ProcessVertices VS";
                return false;
            }
            if (loop_stack.size() >= PV_MAX_LOOP_NESTING) {
                plan.rejection = "loop nesting deeper than ProcessVertices "
                                 "supports";
                return false;
            }
            if (!has(1)) {
                plan.rejection = "a truncated instruction";
                return false;
            }
            instr.op = PvOp::Rep;
            instr.src0 = pv_decode_src(take());
            if (instr.src0.relative ||
                instr.src0.type != PV_REG_CONSTINT ||
                instr.src0.index >= D3D9_MAX_SHADER_CONST_I) {
                plan.rejection = "rep source must be an integer constant";
                return false;
            }
            instr.int_reg = instr.src0.index;
            const u32 start_pc = static_cast<u32>(plan.code.size());
            plan.code.push_back(instr);
            loop_stack.push_back(PvOpenLoop{PvLoopKind::Rep, start_pc, {}});
            continue;
        }
        case PV_ENDREP: {
            if (predicated) {
                plan.rejection = "a predicated endrep in ProcessVertices VS";
                return false;
            }
            if (loop_stack.empty() ||
                loop_stack.back().kind != PvLoopKind::Rep) {
                plan.rejection = "endrep without a matching rep";
                return false;
            }
            instr.op = PvOp::EndRep;
            auto open = std::move(loop_stack.back());
            loop_stack.pop_back();
            const u32 end_pc = static_cast<u32>(plan.code.size());
            instr.partner = open.start_pc + 1;
            plan.code[open.start_pc].partner = end_pc;
            plan.code.push_back(instr);
            for (u32 break_pc : open.breaks) {
                plan.code[break_pc].partner = end_pc + 1;
            }
            continue;
        }
        case PV_BREAK: {
            // Learn: break — exit nearest loop/rep at endloop/endrep.
            if (predicated) {
                plan.rejection = "a predicated break in ProcessVertices VS";
                return false;
            }
            if (loop_stack.empty()) {
                plan.rejection = "break outside a loop or rep block";
                return false;
            }
            instr.op = PvOp::Break;
            const u32 break_pc = static_cast<u32>(plan.code.size());
            plan.code.push_back(instr);
            loop_stack.back().breaks.push_back(break_pc);
            continue;
        }
        case PV_BREAKC: {
            // Learn: break_comp src0, src1 — same compare controls as setp.
            if (predicated) {
                plan.rejection = "a predicated breakc in ProcessVertices VS";
                return false;
            }
            if (loop_stack.empty()) {
                plan.rejection = "breakc outside a loop or rep block";
                return false;
            }
            instr.op = PvOp::BreakC;
            instr.setp_comp = static_cast<u8>((token >> 16) & 0xFFu);
            if (instr.setp_comp < 1 || instr.setp_comp > 6) {
                plan.rejection = "a breakc comparison control ProcessVertices "
                                 "VS cannot apply";
                return false;
            }
            if (!read_src(instr.src0) || !read_src(instr.src1)) return false;
            const u32 break_pc = static_cast<u32>(plan.code.size());
            plan.code.push_back(instr);
            loop_stack.back().breaks.push_back(break_pc);
            continue;
        }
        case PV_BREAKP: {
            // Learn: breakp [!]p0.{xyzw} — predicate component + optional NOT.
            if (predicated) {
                plan.rejection = "a predicated breakp in ProcessVertices VS";
                return false;
            }
            if (loop_stack.empty()) {
                plan.rejection = "breakp outside a loop or rep block";
                return false;
            }
            if (!has(1)) {
                plan.rejection = "a truncated instruction";
                return false;
            }
            instr.op = PvOp::BreakP;
            instr.src0 = pv_decode_src(take());
            if (instr.src0.relative ||
                instr.src0.type != PV_REG_PREDICATE ||
                instr.src0.index != 0) {
                plan.rejection =
                    "breakp source must be a predicate register";
                return false;
            }
            if (instr.src0.source_modifier != PV_SRCMOD_NONE &&
                instr.src0.source_modifier != PV_SRCMOD_NOT) {
                plan.rejection =
                    "a breakp source modifier ProcessVertices VS "
                    "cannot apply";
                return false;
            }
            const u32 break_pc = static_cast<u32>(plan.code.size());
            plan.code.push_back(instr);
            loop_stack.back().breaks.push_back(break_pc);
            continue;
        }
        case PV_CALL:
        case PV_CALLNZ: {
            // Learn: call l# / callnz l#, [!]b#|[!]p0 — forward only.
            if (predicated) {
                plan.rejection = "a predicated call in ProcessVertices VS";
                return false;
            }
            if (!has(1)) {
                plan.rejection = "a truncated instruction";
                return false;
            }
            const PvParam label = pv_decode_src(take());
            if (label.relative || label.type != PV_REG_LABEL) {
                plan.rejection = "call target must be a label register";
                return false;
            }
            instr.op = (opcode == PV_CALL) ? PvOp::Call : PvOp::CallNz;
            instr.int_reg = label.index;
            if (opcode == PV_CALLNZ) {
                if (!has(1)) {
                    plan.rejection = "a truncated instruction";
                    return false;
                }
                instr.src0 = pv_decode_src(take());
                if (instr.src0.relative) {
                    plan.rejection =
                        "relative addressing of a callnz condition";
                    return false;
                }
                if (instr.src0.source_modifier != PV_SRCMOD_NONE &&
                    instr.src0.source_modifier != PV_SRCMOD_NOT) {
                    plan.rejection =
                        "a callnz source modifier ProcessVertices VS "
                        "cannot apply";
                    return false;
                }
                if (instr.src0.type == PV_REG_PREDICATE) {
                    if (instr.src0.index != 0) {
                        plan.rejection =
                            "a callnz predicate register ProcessVertices "
                            "VS cannot read";
                        return false;
                    }
                } else if (instr.src0.type == PV_REG_CONSTBOOL) {
                    if (instr.src0.index >=
                        graphics::SM3_MAX_BOOL_CONSTANT_REGISTERS) {
                        plan.rejection = "a boolean constant above b15";
                        return false;
                    }
                } else {
                    plan.rejection =
                        "callnz condition must be a bool or predicate";
                    return false;
                }
            }
            const u32 call_pc = static_cast<u32>(plan.code.size());
            plan.code.push_back(instr);
            call_pcs.push_back(call_pc);
            continue;
        }
        case PV_RET: {
            // Learn: ret — return from call, or end main from the entry path.
            if (predicated) {
                plan.rejection = "a predicated ret in ProcessVertices VS";
                return false;
            }
            instr.op = PvOp::Ret;
            if (in_label) {
                const u32 ret_pc = static_cast<u32>(plan.code.size());
                plan.code.push_back(instr);
                plan.code[current_label_pc].partner = ret_pc + 1;
                in_label = false;
                expect_label_or_end = true;
                continue;
            }
            break;
        }
        case PV_LABEL: {
            // Learn: label l# — subroutine entry after its callers.
            if (predicated) {
                plan.rejection = "a predicated label in ProcessVertices VS";
                return false;
            }
            if (in_label) {
                plan.rejection = "an unclosed label block";
                return false;
            }
            if (!if_has_else.empty() || !loop_stack.empty()) {
                plan.rejection = "a label inside an open if or loop block";
                return false;
            }
            if (!has(1)) {
                plan.rejection = "a truncated instruction";
                return false;
            }
            instr.op = PvOp::Label;
            instr.dst = pv_decode_dst(take());
            if (instr.dst.type != PV_REG_LABEL) {
                plan.rejection = "label destination must be a label register";
                return false;
            }
            if (label_pcs.count(instr.dst.index) != 0) {
                plan.rejection = "duplicate label";
                return false;
            }
            const u32 label_pc = static_cast<u32>(plan.code.size());
            plan.code.push_back(instr);
            label_pcs[instr.dst.index] = label_pc;
            in_label = true;
            current_label_pc = label_pc;
            expect_label_or_end = false;
            continue;
        }
        case PV_IF: {
            // Learn: if bool / if pred - vs — static b# or dynamic p0.comp.
            if (predicated) {
                plan.rejection = "a predicated if in ProcessVertices VS";
                return false;
            }
            if (!has(1)) {
                plan.rejection = "a truncated if";
                return false;
            }
            instr.op = PvOp::If;
            instr.src0 = pv_decode_src(take());
            if (instr.src0.relative) {
                plan.rejection = "relative addressing of an if condition";
                return false;
            }
            if (instr.src0.source_modifier != PV_SRCMOD_NONE &&
                instr.src0.source_modifier != PV_SRCMOD_NOT) {
                plan.rejection =
                    "an if source modifier ProcessVertices VS cannot apply";
                return false;
            }
            if (instr.src0.type == PV_REG_PREDICATE) {
                if (instr.src0.index != 0) {
                    plan.rejection =
                        "an if predicate register ProcessVertices VS "
                        "cannot read";
                    return false;
                }
            } else if (instr.src0.type == PV_REG_CONSTBOOL) {
                if (instr.src0.index >=
                    graphics::SM3_MAX_BOOL_CONSTANT_REGISTERS) {
                    plan.rejection = "a boolean constant above b15";
                    return false;
                }
            } else {
                plan.rejection =
                    "an if source that is not a bool or predicate";
                return false;
            }
            if_has_else.push_back(false);
            break;
        }
        case PV_IFC: {
            // Learn: if_comp - vs — same compare controls as setp/breakc;
            // replicate-swizzled scalar sources (sm3_to_msl uses .x).
            if (predicated) {
                plan.rejection = "a predicated ifc in ProcessVertices VS";
                return false;
            }
            instr.op = PvOp::If;
            instr.setp_comp = static_cast<u8>((token >> 16) & 0xFFu);
            if (instr.setp_comp < 1 || instr.setp_comp > 6) {
                plan.rejection = "an ifc comparison control ProcessVertices "
                                 "VS cannot apply";
                return false;
            }
            if (!read_src(instr.src0) || !read_src(instr.src1)) return false;
            if_has_else.push_back(false);
            break;
        }
        case PV_ELSE: {
            if (predicated) {
                plan.rejection = "a predicated else in ProcessVertices VS";
                return false;
            }
            if (if_has_else.empty()) {
                plan.rejection = "else without a matching if";
                return false;
            }
            if (if_has_else.back()) {
                plan.rejection = "a second else for the same if";
                return false;
            }
            if_has_else.back() = true;
            instr.op = PvOp::Else;
            break;
        }
        case PV_ENDIF: {
            if (predicated) {
                plan.rejection = "a predicated endif in ProcessVertices VS";
                return false;
            }
            if (if_has_else.empty()) {
                plan.rejection = "endif without a matching if";
                return false;
            }
            if_has_else.pop_back();
            instr.op = PvOp::Endif;
            break;
        }
        default:
            plan.rejection =
                "an opcode ProcessVertices VS cannot interpret";
            return false;
        }
        if (predicated) {
            // Trailing predicate source after normal operands (sm3_to_msl).
            if (!has(1)) {
                plan.rejection = "a truncated predicated instruction";
                return false;
            }
            instr.predicate = pv_decode_src(take());
            instr.predicated = true;
            if (instr.predicate.relative) {
                plan.rejection = "relative addressing of a predicate register";
                return false;
            }
            if (instr.predicate.type != PV_REG_PREDICATE ||
                instr.predicate.index != 0) {
                plan.rejection =
                    "a predicated instruction without a predicate register";
                return false;
            }
            if (instr.predicate.source_modifier != PV_SRCMOD_NONE &&
                instr.predicate.source_modifier != PV_SRCMOD_NOT) {
                plan.rejection =
                    "a predicate source modifier ProcessVertices VS "
                    "cannot apply";
                return false;
            }
        }
        (void)((token >> PV_LENGTH_SHIFT) & 0xFu);
        plan.code.push_back(instr);
    }
    plan.rejection = "vertex shader missing END";
    return false;
}

} // namespace vitreo::winapi::d3d9_pv
