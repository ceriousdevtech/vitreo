// SPDX-License-Identifier: LGPL-2.1-only
#pragma once
/// @file dxil_lower_ext.hpp
/// @brief The parts of the DXIL -> SM5.1 lowering that emit Vitreo
/// extension instructions (dxbc_vitreo_ext.hpp): wave and quad intrinsics
/// (dxil_lower_wave.cpp), 64-bit integers and native 16-bit types
/// (dxil_lower_wide.cpp).
///
/// They reach the lowering's state through LowerHost, which the lowering
/// (dxil_lower.cpp) implements over its own registers, handles and
/// pointers. The register model is the one dxbc_vitreo_ext.hpp states: an
/// i64 value is an aligned component pair, an i16 a zero-extended
/// component, a half the float32 that represents it.

#include "dxil_lower_internal.hpp"
#include "dxil_module.hpp"
#include "shader_types.hpp"

#include <map>
#include <string>
#include <vector>

namespace vitreo::graphics::dxil_lower {

/// A temp register component (a pair's first component for an i64).
struct RegSlot {
    u32 reg{0};
    u32 comp{0};
};

/// What a resource handle names.
struct ResourceView {
    ResourceClass cls{ResourceClass::SRV};
    ResourceShape shape{ResourceShape::TypedBuffer};
    u32 range_id{0};
    Index reg;
    bool nonuniform{false};
    u32 element_type{0};
    u32 stride{0};
};

/// State the extension lowering keeps per program.
struct ExtState {
    /// Aggregate results (dx.types.ResRet / CBufRet of 16- or 64-bit
    /// elements) whose members live in arbitrary slots: member k of value
    /// id is aggregates[id][k].
    std::map<u32, std::vector<RegSlot>> aggregates;
};

class LowerHost {
public:
    virtual ~LowerHost() = default;
    [[nodiscard]] virtual const DxilModule& module() const = 0;
    [[nodiscard]] virtual const DxilFunction* function() const = 0;
    [[nodiscard]] virtual ShaderStage stage() const = 0;
    virtual Emitter& body() = 0;
    virtual ExtState& ext() = 0;
    /// The value's slot (a pair for an i64), allocated on first use.
    virtual RegSlot slot(u32 id) = 0;
    /// Places value @p id at @p s (an aggregate member read by extractvalue).
    virtual void bind(u32 id, RegSlot s) = 0;
    virtual RegSlot fresh_scalar() = 0;
    virtual RegSlot fresh_pair() = 0;
    virtual u32 fresh_vector() = 0;
    /// The register an aggregate result (ResRet) occupies, allocated on
    /// first use.
    virtual u32 vector(u32 id) = 0;
    /// Whether @p id is an aggregate placed in a vector register, and which.
    virtual bool vector_of(u32 id, u32& reg) = 0;
    /// The value as a source: one component replicated, an i64 pair as
    /// (lo, hi, lo, hi), constants as immediates. Modifier 1 negates.
    virtual Operand src(u32 id, u32 modifier = 0) = 0;
    virtual Operand dst(u32 id) = 0;
    virtual bool const_u32(u32 id, u32& out) = 0;
    /// Integer constant of any width (sign-extended bits).
    virtual bool const_u64(u32 id, u64& out) = 0;
    virtual bool undef(u32 id) = 0;
    virtual bool resource_of(u32 handle, ResourceView& out) = 0;
    /// Moves between a value component and scalar element @p offset of the
    /// thread-local or groupshared object @p pointer points into.
    virtual bool load_element(u32 pointer, u32 offset, const Operand& d) = 0;
    virtual bool store_element(u32 pointer, u32 offset, const Operand& v) = 0;
    /// Runs the lowering's own 32-bit path for @p inst (a dx.op call): a
    /// half overload as float, an i16 one as i32.
    virtual bool emit_call_core(const DxilInstruction& inst) = 0;
    virtual bool unimplemented(const std::string& what) = 0;
    virtual bool malformed(const std::string& what) = 0;
    [[nodiscard]] virtual bool failed() const = 0;
};

/// The width class of a DXIL type for the register model.
/// F64 is a double in an aligned component pair (low word first), computed
/// with SM5's double instructions.
enum class WideKind : u8 { None, I16, F16, I64, F64, Other };
[[nodiscard]] WideKind wide_kind(const DxilModule& m, u32 type);

/// DXIL opcodes of the wave and quad intrinsics.
[[nodiscard]] bool is_wave_opcode(u32 opcode);
/// Lowers a wave or quad intrinsic call.
bool lower_wave_call(LowerHost& host, u32 opcode, const DxilInstruction& inst);

/// DXIL opcodes 174-177: WriteSamplerFeedback, -Bias, -Level, -Grad.
[[nodiscard]] bool is_feedback_opcode(u32 opcode);
/// Lowers a sampler feedback write (dxil_lower_feedback.cpp).
bool lower_feedback_call(LowerHost& host, u32 opcode, const DxilInstruction& inst);

/// Whether an LLVM instruction (not a call) needs the 16- / 64-bit
/// lowering: arithmetic, comparisons and conversions on i16, half or i64,
/// and memory or aggregate access of i64.
[[nodiscard]] bool wants_wide_instruction(LowerHost& host, const DxilInstruction& inst);
bool lower_wide_instruction(LowerHost& host, const DxilInstruction& inst);
/// Whether a dx.op call has a 16- or 64-bit overload or operand.
[[nodiscard]] bool wants_wide_call(LowerHost& host, const DxilFunction& callee, const DxilInstruction& inst);
bool lower_wide_call(LowerHost& host, u32 opcode, const DxilInstruction& inst);

/// The float32 bits of half @p bits (exact).
[[nodiscard]] u32 half_bits_to_float_bits(u32 bits);

/// Emitters shared by the two files.
void emit_ext(Emitter& e, u32 opcode, u32 control, std::initializer_list<const Operand*> ops);
/// The control operand: one 32-bit immediate component.
[[nodiscard]] Operand control_operand(u32 control);
/// A pair destination / source at @p s.
[[nodiscard]] Operand pair_dst(RegSlot s);
[[nodiscard]] Operand pair_src(RegSlot s);
[[nodiscard]] Operand scalar_dst(RegSlot s);
[[nodiscard]] Operand scalar_src(RegSlot s);

}  // namespace vitreo::graphics::dxil_lower
