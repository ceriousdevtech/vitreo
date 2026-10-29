// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file d3d9_pv_cpu.hpp
/// @brief Shared surface of the ProcessVertices CPU vertex-shader interpreter:
///        the decoded plan and its register/modifier constants, plus the two
///        entry points the analyzer and interpreter translation units export.
///
/// Private to the D3D9 frontend. These types were in an anonymous namespace in
/// d3d9_draw.cpp, next to the analyzer, the interpreter and `process_vertices`;
/// they live here so those three can be separate translation units.

#include "winapi/graphics/d3d9/d3d9_state.hpp"

#include <array>
#include <map>
#include <vector>

namespace vitreo::winapi::d3d9_pv {

// -----------------------------------------------------------------------------
// ProcessVertices programmable-VS subset (CPU, no Metal)
// -----------------------------------------------------------------------------
//
// Learn: ProcessVertices applies the bound vertex shader, then viewport →
// XYZRHW. Vitreo interprets a small SM2/SM3 float subset — mov/add/sub/mul/
// mad/lrp/cmp/rcp/rsq/min/max/dp3/dp4/m4x4/m4x3/m3x4/m3x3/m3x2/pow/abs/frc/
// nrm/sgn/slt/sge/exp/expp/log/logp/lit/dst/sincos/crs/mova/setp plus loop/
// rep/endloop/endrep/break/breakc/breakp/ifc (aL driven like sm3_to_msl) and
// call/callnz/label/ret (forward subroutines, nesting capped like Learn
// vs_2_0/vs_3_0) on temps, constants (incl. c[aL+n] / c[a0+n]), and inputs
// writing oPos / oD# / oT# — with source mods none/neg/bias/bx2/abs (+neg
// variants). Predicated arithmetic uses the instruction-token bit and a
// trailing predicate source, guarded like sm3_to_msl (`if (any(p))`).
// Static/dynamic if/else/endif (b# / p0 component + optional NOT, defb)
// match sm3_to_msl. Refuses other VS (e.g. texkill/texldl) with E_NOTIMPL.

enum PvRegType : u32 {
    PV_REG_TEMP = 0,
    PV_REG_INPUT = 1,
    PV_REG_CONST = 2,
    PV_REG_ADDR = 3,
    PV_REG_RASTOUT = 4,
    PV_REG_ATTROUT = 5,
    PV_REG_OUTPUT = 6,
    PV_REG_CONSTINT = 7,
    PV_REG_CONSTBOOL = 14,
    PV_REG_LOOP = 15,
    PV_REG_LABEL = 18,
    PV_REG_PREDICATE = 19,
};

/// Learn: loop/rep iteration count is [0, 255]; refuse deeper nesting than
/// typical vs_2_0/3_0 static loop limits.
inline constexpr u32 PV_MAX_LOOP_COUNT = 255;
inline constexpr u32 PV_MAX_LOOP_NESTING = 4;
/// Learn: call nesting — vs_2_0 forbids nesting (depth 1); vs_3_0 allows 4.
inline constexpr u32 PV_MAX_CALL_DEPTH_VS2 = 1;
inline constexpr u32 PV_MAX_CALL_DEPTH_VS3 = 4;

// D3DSPSM_* — match sm3_to_msl SourceModifier.
inline constexpr u32 PV_SRCMOD_NONE = 0;
inline constexpr u32 PV_SRCMOD_NEG = 1;
inline constexpr u32 PV_SRCMOD_BIAS = 2;
inline constexpr u32 PV_SRCMOD_BIASNEG = 3;
inline constexpr u32 PV_SRCMOD_SIGN = 4; // _bx2
inline constexpr u32 PV_SRCMOD_SIGNNEG = 5;
inline constexpr u32 PV_SRCMOD_ABS = 11;
inline constexpr u32 PV_SRCMOD_ABSNEG = 12;
inline constexpr u32 PV_SRCMOD_NOT = 13;
inline constexpr u32 PV_DSTMOD_SATURATE = 0x1;
inline constexpr u32 PV_DSTMOD_PARTIALPRECISION = 0x2;
inline constexpr u32 PV_DSTMOD_CENTROID = 0x4;
inline constexpr u32 PV_MAX_TEMPS = 32;
inline constexpr u32 PV_MAX_INPUTS = 16;
inline constexpr u32 PV_MAX_COLORS = 2;
inline constexpr u32 PV_MAX_TEXCOORDS = 8;

struct PvParam {
    u32 type{PV_REG_TEMP};
    u32 index{0};
    u32 write_mask{0xF};
    u32 result_modifier{0};
    u32 shift{0};
    u32 swizzle{0xE4};
    u32 source_modifier{PV_SRCMOD_NONE};
    bool relative{false};
    /// Relative address is aL (loop counter) rather than a0.
    bool relative_loop{false};
    /// Component of a0 used for relative constant indexing (0=x..3=w).
    u32 relative_component{0};
};

enum class PvOp : u8 {
    Mov,
    Add,
    Sub,
    Mul,
    Mad,
    Lrp,
    Cmp,
    Rcp,
    Rsq,
    Min,
    Max,
    Dp3,
    Dp4,
    Matrix, // m4x4 / m4x3 / m3x4 / m3x3 / m3x2
    Abs,
    Frc,
    Nrm,
    Sgn,
    Slt,
    Sge,
    Exp,
    Log,
    Lit,
    Dst,
    Pow,
    Sincos,
    Crs,
    Mova,
    Setp,
    Loop,
    EndLoop,
    Rep,
    EndRep,
    Break,
    BreakC,
    BreakP,
    Call,
    CallNz,
    Ret,
    Label,
    If,
    Else,
    Endif,
};

struct PvInstr {
    PvOp op{PvOp::Mov};
    PvParam dst{};
    PvParam src0{};
    PvParam src1{};
    PvParam src2{};
    u8 matrix_vector_size{4}; // 3 → dp3 rows; 4 → dp4 rows
    u8 matrix_row_count{4};
    /// setp/breakc/ifc compare control: 1=GT … 6=LE (Learn *_comp - vs).
    /// Non-zero on PvOp::If means if_comp rather than if bool/pred.
    u8 setp_comp{0};
    bool predicated{false};
    PvParam predicate{};
    /// loop/rep: i# register index; call/callnz: label index until resolved.
    u32 int_reg{0};
    /// Loop/Rep → End* PC; End* → body start PC; Break* → PC after End*;
    /// Call/CallNz → Label PC; Label → PC after its Ret (fall-through skip).
    u32 partner{0};
};

struct PvCpuPlan {
    bool ok{false};
    const char* rejection{"an unsupported ProcessVertices vertex shader"};
    std::vector<graphics::Sm3InputElement> inputs;
    std::map<u32, graphics::Sm3InputElement> output_decls;
    std::array<std::array<float, 4>, graphics::SM3_MAX_FLOAT_CONSTANT_REGISTERS>
        def_const{};
    std::array<bool, graphics::SM3_MAX_FLOAT_CONSTANT_REGISTERS> def_const_set{};
    std::array<std::array<i32, 4>, D3D9_MAX_SHADER_CONST_I> def_int{};
    std::array<bool, D3D9_MAX_SHADER_CONST_I> def_int_set{};
    std::array<bool, graphics::SM3_MAX_BOOL_CONSTANT_REGISTERS> def_bool{};
    std::array<bool, graphics::SM3_MAX_BOOL_CONSTANT_REGISTERS> def_bool_set{};
    std::vector<PvInstr> code;
    bool writes_opos{false};
    std::array<bool, PV_MAX_COLORS> writes_color{};
    std::array<bool, PV_MAX_TEXCOORDS> writes_texcoord{};
    /// Learn call nesting depth for this shader model.
    u32 max_call_depth{PV_MAX_CALL_DEPTH_VS3};
};

/// Decode and validate a SM2/SM3 vertex shader into a CPU plan. Returns false
/// with `plan.rejection` set when the shader is outside the supported subset.
[[nodiscard]] bool analyze_vs_for_process_vertices(const u32* tokens,
                                                   usize token_count,
                                                   PvCpuPlan& plan);

/// Interpret a decoded plan for one vertex.
[[nodiscard]] bool run_vs_cpu_process_vertices(
    const PvCpuPlan& plan, const std::array<float, 4>* inputs,
    const float* vs_const_f, const i32* vs_const_i, const u32* vs_const_b,
    std::array<float, 4>& opos,
    std::array<std::array<float, 4>, PV_MAX_COLORS>& ocolor,
    std::array<std::array<float, 4>, PV_MAX_TEXCOORDS>& otexcoord);

} // namespace vitreo::winapi::d3d9_pv
