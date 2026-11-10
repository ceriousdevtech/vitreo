// SPDX-License-Identifier: LGPL-2.1-only

/// @file dxbc_to_spirv_denorm.cpp
/// @brief VX_F32P (dxbc_vitreo_ext.hpp): float32 arithmetic that keeps
///        denormals, for SM 6.2 "fp32-denorm-mode"="preserve".
///
/// The Apple GPU flushes float32 denormal inputs and results of every
/// arithmetic operation, compare, min / max, clamp and conversion, in every
/// Metal math mode (measured on an M4 Pro: fast, relaxed, safe, safe with
/// `#pragma METAL fp math_mode(safe)` / `fp contract(off)`, and the legacy
/// fastMathEnabled = NO); loads, stores, bitcasts, select, negate and abs
/// keep their bits. The MSL specification allows it (8.1: denormals "may
/// be flushed to zero"). So each operation is a SPIR-V function over the
/// float's bits (soft_float.hpp), emitted once after main for the
/// operations a shader uses:
///  - add, mul, fma and div first run on the hardware; when no operand and
///    not the result has a zero exponent field (no zero, no denormal, no
///    flushed or underflowed result) that result is the IEEE one and is
///    returned; otherwise the integer algorithm computes it. Shaders
///    without "preserve" never reach this file.
///  - comparisons, min / max and saturate are integer orderings of the
///    bits and need no hardware path.

#include "graphics/shaders/dxbc_to_spirv_internal.hpp"
#include "graphics/shaders/dxbc_to_spirv_int_machine.hpp"
#include "graphics/shaders/dxbc_vitreo_ext.hpp"
#include "graphics/shaders/soft_float.hpp"

namespace vitreo::graphics::dxbc_spirv_detail {

using dxbc_vitreo_ext::F32POp;

namespace {

struct F32PInfo {
    const char* name;
    u32 sources;
    bool hardware_first;  // the add / mul / fma / div fast path
};

// Indexed by F32POp.
constexpr F32PInfo kF32P[] = {
    {"vitreoF32PAdd", 2, true},  {"vitreoF32PMul", 2, true},
    {"vitreoF32PFma", 3, true},  {"vitreoF32PDiv", 2, true},
    {"vitreoF32PMin", 2, false}, {"vitreoF32PMax", 2, false},
    {"vitreoF32PSat", 1, false}, {"vitreoF32PEq", 2, false},
    {"vitreoF32PNe", 2, false},  {"vitreoF32PLt", 2, false},
    {"vitreoF32PGe", 2, false},
};
static_assert(std::size(kF32P) == static_cast<usize>(F32POp::Count));

}  // namespace

bool DxbcTranslator::translate_vx_f32p(u32 control) {
    if (control >= static_cast<u32>(F32POp::Count)) {
        return fail("VX_F32P: unknown operation " + std::to_string(control));
    }
    const F32PInfo& info = kF32P[control];
    Sm4Operand dst;
    std::array<Sm4Operand, 3> s{};
    if (!m_decoder.decode_operand(dst)) return fail("VX_F32P: bad operands");
    std::array<u32, 3> bits{};
    for (u32 i = 0; i < info.sources; ++i) {
        if (!m_decoder.decode_operand(s[i])) return fail("VX_F32P: bad operands");
        const u32 modifier = s[i].modifier;
        if (modifier > 3u) return fail("VX_F32P: unsupported source modifier");
        Sm4Operand plain = s[i];
        plain.modifier = 0;
        if (!load_bits(plain, bits[i])) return false;
        // neg / abs on the sign bit: a float negate or abs would keep a
        // denormal too (measured), but the bit form needs no float op.
        const u32 sign = 0x80000000u;
        if (modifier == 2u || modifier == 3u) {
            bits[i] = m_builder.emit_bitwise_and(m_type_uvec4, bits[i], make_uvec4_imm(~sign, ~sign, ~sign, ~sign));
        }
        if (modifier == 1u || modifier == 3u) {
            bits[i] = m_builder.emit_bitwise_xor(m_type_uvec4, bits[i], make_uvec4_imm(sign, sign, sign, sign));
        }
    }
    if (dst.type == Sm4OperandType::Null) return true;
    u32& function = m_f32p_helpers[control];
    if (function == 0) function = m_builder.allocate_id();
    const u32 zero = m_builder.emit_constant_int(m_type_uint, 0u);
    std::array<u32, 4> out{zero, zero, zero, zero};
    for (u32 c = 0; c < 4; ++c) {
        if ((dst.write_mask & (1u << c)) == 0u) continue;
        std::vector<u32> args;
        for (u32 i = 0; i < info.sources; ++i) {
            args.push_back(m_builder.emit_composite_extract(m_type_uint, bits[i], {c}));
        }
        out[c] = m_builder.emit_function_call(m_type_uint, function, args);
    }
    // Only VX_F32P Saturate clamps; a _sat bit cannot reach here (the
    // extension encoding has none) and store_vec4 must not clamp as floats.
    m_saturate_destination = false;
    const u32 value = m_builder.emit_composite_construct(m_type_uvec4, {out[0], out[1], out[2], out[3]});
    return store_vec4(dst, m_builder.emit_bitcast(m_type_vec4, value));
}

bool DxbcTranslator::emit_f32p_helpers() {
    using Soft = soft_float::SoftFloat<SpirvIntMachine>;
    static_assert(std::tuple_size<decltype(m_f32p_helpers)>::value == static_cast<usize>(F32POp::Count));
    for (usize index = 0; index < m_f32p_helpers.size(); ++index) {
        const u32 function = m_f32p_helpers[index];
        if (function == 0) continue;
        const F32PInfo& info = kF32P[index];
        if (m_type_uint_pair == 0) {
            m_type_uint_pair = m_builder.emit_type_struct({m_type_uint, m_type_uint});
        }
        u32& function_type = m_f32p_function_types[info.sources];
        if (function_type == 0) {
            function_type = m_builder.emit_type_function(m_type_uint, std::vector<u32>(info.sources, m_type_uint));
        }
        m_builder.emit_function_id(function, m_type_uint, function_type);
        m_builder.emit_name(function, info.name);
        std::vector<u32> p(info.sources);
        for (u32& parameter : p) parameter = m_builder.emit_function_parameter(m_type_uint);
        const u32 entry = m_builder.emit_label();
        const auto uint = [&](u32 v) { return m_builder.emit_constant_int(m_type_uint, v); };
        const auto soft = [&](Soft& sf) -> u32 {
            const auto mask = [&](u32 condition) {
                return m_builder.emit_select(m_type_uint, condition, uint(0xFFFFFFFFu), uint(0u));
            };
            switch (static_cast<F32POp>(index)) {
            case F32POp::Add: return sf.add(p[0], p[1]);
            case F32POp::Mul: return sf.mul(p[0], p[1]);
            case F32POp::Fma: return sf.fma(p[0], p[1], p[2]);
            case F32POp::Div: return sf.div(p[0], p[1]);
            case F32POp::Min: return sf.min(p[0], p[1]);
            case F32POp::Max: return sf.max(p[0], p[1]);
            case F32POp::Saturate: return sf.saturate(p[0]);
            case F32POp::Eq: return mask(sf.eq(p[0], p[1]));
            // Unordered: true when either is NaN (eq is false then).
            case F32POp::Ne: return m_builder.emit_not(m_type_uint, mask(sf.eq(p[0], p[1])));
            case F32POp::Lt: return mask(sf.lt(p[0], p[1]));
            case F32POp::Ge: return mask(sf.ge(p[0], p[1]));
            case F32POp::Count: break;
            }
            return 0u;
        };
        if (!info.hardware_first) {
            SpirvIntMachine machine(m_builder, m_type_uint, m_type_bool, m_type_uint_pair, m_glsl_std450, entry);
            Soft sf(machine);
            m_builder.emit_return_value(soft(sf));
            m_builder.emit_function_end();
            continue;
        }
        // The hardware result, kept when every operand and the result have
        // a nonzero exponent field: then nothing was a denormal or zero and
        // nothing underflowed, so the GPU's IEEE result stands.
        std::vector<u32> f;
        for (u32 x : p) f.push_back(m_builder.emit_bitcast(m_type_float, x));
        u32 hw = 0;
        switch (static_cast<F32POp>(index)) {
        case F32POp::Add: hw = m_builder.emit_fadd(m_type_float, f[0], f[1]); break;
        case F32POp::Mul: hw = m_builder.emit_fmul(m_type_float, f[0], f[1]); break;
        case F32POp::Div: hw = m_builder.emit_fdiv(m_type_float, f[0], f[1]); break;
        default: hw = m_builder.emit_ext_inst(m_type_float, m_glsl_std450, kGlslStd450Fma, {f[0], f[1], f[2]}); break;
        }
        const u32 hw_bits = m_builder.emit_bitcast(m_type_uint, hw);
        u32 ok = 0;
        std::vector<u32> words(p.begin(), p.end());
        words.push_back(hw_bits);
        for (u32 x : words) {
            const u32 e = m_builder.emit_bitwise_and(m_type_uint, x, uint(0x7F800000u));
            const u32 nonzero = m_builder.emit_inotequal(m_type_bool, e, uint(0u));
            ok = ok == 0 ? nonzero : m_builder.emit_logical(SpvOp::OpLogicalAnd, m_type_bool, ok, nonzero);
        }
        const u32 soft_block = m_builder.allocate_id();
        const u32 merge = m_builder.allocate_id();
        m_builder.emit_selection_merge(merge);
        m_builder.emit_branch_conditional(ok, merge, soft_block);
        m_builder.emit_label_id(soft_block);
        SpirvIntMachine machine(m_builder, m_type_uint, m_type_bool, m_type_uint_pair, m_glsl_std450, soft_block);
        Soft sf(machine);
        const u32 soft_bits = soft(sf);
        const u32 soft_end = machine.block();
        m_builder.emit_branch(merge);
        m_builder.emit_label_id(merge);
        const u32 result = m_builder.emit_phi(m_type_uint, {{hw_bits, entry}, {soft_bits, soft_end}});
        m_builder.emit_return_value(result);
        m_builder.emit_function_end();
    }
    return true;
}

}  // namespace vitreo::graphics::dxbc_spirv_detail
