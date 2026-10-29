// SPDX-License-Identifier: LGPL-2.1-only

/// @file dxbc_to_spirv_double.cpp
/// @brief Double-precision instructions and the soft_double helpers.

#include "graphics/shaders/dxbc_to_spirv_internal.hpp"
#include "graphics/shaders/soft_double.hpp"

namespace vitreo::graphics::dxbc_spirv_detail {

namespace {

// =============================================================================
// soft_double machine: the double-precision helpers emitted as SPIR-V
// =============================================================================

/// The 32-bit integer machine soft_double::SoftDouble runs on to emit the
/// SM5 double instructions (soft_double.hpp): each operation is one SPIR-V
/// instruction appended to the function being built; values are result ids.
class SpirvIntMachine {
public:
    using U = u32;
    using B = u32;

    SpirvIntMachine(SpirvBuilder& builder, u32 type_uint, u32 type_bool,
                    u32 type_uint_pair, u32 glsl, u32 block)
        : m_b(builder), m_uint(type_uint), m_bool(type_bool),
          m_pair(type_uint_pair), m_glsl(glsl), m_block(block) {}

    U k(u32 value) { return m_b.emit_constant_int(m_uint, value); }
    U add(U a, U b) { return m_b.emit_iadd(m_uint, a, b); }
    U sub(U a, U b) { return m_b.emit_isub(m_uint, a, b); }
    U band(U a, U b) { return m_b.emit_bitwise_and(m_uint, a, b); }
    U bor(U a, U b) { return m_b.emit_bitwise_or(m_uint, a, b); }
    U bxor(U a, U b) { return m_b.emit_bitwise_xor(m_uint, a, b); }
    U shl(U a, U n) { return m_b.emit_shift_left_logical(m_uint, a, n); }
    U shr(U a, U n) { return m_b.emit_shift_right_logical(m_uint, a, n); }
    std::pair<U, U> mul_wide(U a, U b) {
        const u32 product = m_b.emit_umul_extended(m_pair, a, b);
        return {m_b.emit_composite_extract(m_uint, product, {0u}),
                m_b.emit_composite_extract(m_uint, product, {1u})};
    }
    U msb(U x) {
        return m_b.emit_ext_inst(m_uint, m_glsl, kGlslStd450FindUMsb, {x});
    }
    B ult(U a, U b) { return compare(SpvOp::OpULessThan, a, b); }
    B slt(U a, U b) { return compare(SpvOp::OpSLessThan, a, b); }
    B eq(U a, U b) { return compare(SpvOp::OpIEqual, a, b); }
    B ne(U a, U b) { return compare(SpvOp::OpINotEqual, a, b); }
    B land(B a, B b) {
        return m_b.emit_logical(SpvOp::OpLogicalAnd, m_bool, a, b);
    }
    B lor(B a, B b) { return m_b.emit_logical(SpvOp::OpLogicalOr, m_bool, a, b); }
    B lnot(B a) { return m_b.emit_logical_not(m_bool, a); }
    U sel(B c, U a, U b) { return m_b.emit_select(m_uint, c, a, b); }

    /// A structured loop run @p count times: the carried state (and the
    /// trip counter) are OpPhis of the header, whose back-edge operands are
    /// patched in once the body has produced them.
    template <std::size_t N, class Body>
    std::array<U, N> loop(u32 count, const std::array<U, N>& init,
                          Body&& body) {
        const u32 header = m_b.allocate_id();
        const u32 body_block = m_b.allocate_id();
        const u32 continue_block = m_b.allocate_id();
        const u32 merge = m_b.allocate_id();
        const u32 pre = m_block;
        m_b.emit_branch(header);
        m_b.emit_label_id(header);
        std::array<usize, N + 1> back_edges{};
        const auto phi = [&](u32 initial, usize& slot) {
            // OpPhi words: opcode, type, id, (initial, pre), (next, cont).
            slot = m_b.function_word_count() + 5u;
            return m_b.emit_phi(m_uint, {{initial, pre}, {initial, continue_block}});
        };
        const U counter = phi(k(0), back_edges[N]);
        std::array<U, N> state{};
        for (std::size_t i = 0; i < N; ++i) {
            state[i] = phi(init[i], back_edges[i]);
        }
        // OpLoopMerge must immediately precede the header's branch.
        const B go = ult(counter, k(count));
        m_b.emit_loop_merge(merge, continue_block);
        m_b.emit_branch_conditional(go, body_block, merge);
        m_b.emit_label_id(body_block);
        m_block = body_block;
        std::array<U, N> next = state;
        body(next);
        m_b.emit_branch(continue_block);
        m_b.emit_label_id(continue_block);
        const U counter_next = add(counter, k(1));
        m_b.emit_branch(header);
        for (std::size_t i = 0; i < N; ++i) {
            m_b.patch_function_word(back_edges[i], next[i]);
        }
        m_b.patch_function_word(back_edges[N], counter_next);
        m_b.emit_label_id(merge);
        m_block = merge;
        return state;
    }

private:
    B compare(SpvOp op, U a, U b) {
        return m_b.emit_integer_compare(op, m_bool, a, b);
    }

    SpirvBuilder& m_b;
    u32 m_uint;
    u32 m_bool;
    u32 m_pair;
    u32 m_glsl;
    u32 m_block;  // the block being emitted (a loop's pre-header)
};

} // namespace

// -----------------------------------------------------------------------------
// Double precision (D3D11.3 3.1.4, 22.14-22.17)
// -----------------------------------------------------------------------------
//
// A double is a register pair, low word in .x/.z and high word in .y/.w.
// Apple GPUs have no fp64, so every instruction is exact IEEE-754 software
// arithmetic on 32-bit integers (soft_double.hpp). Each operation is a SPIR-V
// function emitted once, after main, and only when the shader calls it.

namespace {

struct DoubleHelperInfo {
    const char* name;
    u32 params;     // 32-bit words: two per double
    bool pair;      // returns a double (uvec2 low, high) rather than a uint
};

// Indexed by DxbcTranslator::DoubleHelper.
constexpr DoubleHelperInfo kDoubleHelpers[] = {
    {"vitreoDAdd", 4, true},   {"vitreoDMul", 4, true},
    {"vitreoDDiv", 4, true},   {"vitreoDFma", 6, true},
    {"vitreoDMin", 4, true},   {"vitreoDMax", 4, true},
    {"vitreoDEq", 4, false},   {"vitreoDGe", 4, false},
    {"vitreoDLt", 4, false},   {"vitreoDToF", 2, false},
    {"vitreoFToD", 1, true},   {"vitreoDToI", 2, false},
    {"vitreoDToU", 2, false},  {"vitreoIToD", 1, true},
    {"vitreoUToD", 1, true},   {"vitreoDSat", 2, true},
    {"vitreoFSat", 1, false},
};

} // namespace

bool DxbcTranslator::load_bits(const Sm4Operand& src, u32& out_id) {
    // Immediates are built as uint constants: a double's low word is as
    // likely as not a float denormal or NaN pattern, which a float literal
    // in the generated MSL need not preserve.
    if (src.is_immediate || src.type == Sm4OperandType::Immediate32) {
        std::array<u32, 4> words{};
        for (u32 i = 0; i < 4; ++i) {
            std::memcpy(&words[i], &src.immediate[i], sizeof(u32));
        }
        out_id = make_uvec4_imm(words[0], words[1], words[2], words[3]);
        return true;
    }
    u32 value = 0;
    if (!load_vec4(src, value)) return false;
    out_id = m_builder.emit_bitcast(m_type_uvec4, value);
    return true;
}

u32 DxbcTranslator::call_double(DoubleHelper helper,
                                const std::vector<u32>& args) {
    static_assert(std::size(kDoubleHelpers) ==
                  static_cast<usize>(DoubleHelper::Count));
    u32& function = m_double_helpers[static_cast<usize>(helper)];
    if (function == 0) function = m_builder.allocate_id();
    const bool pair = kDoubleHelpers[static_cast<usize>(helper)].pair;
    return m_builder.emit_function_call(pair ? m_type_uvec2 : m_type_uint,
                                        function, args);
}

bool DxbcTranslator::translate_double(Sm4Opcode opcode) {
    // Shapes (22.14-22.17): the number of sources, whether the result is a
    // double pair per lane or one 32-bit value per double, and whether the
    // sources are doubles (ftod/itod/utod read 32-bit .x and .y).
    u32 sources = 2;
    bool double_result = true;
    bool double_sources = true;
    const char* name = "double";
    switch (opcode) {
    case Sm4Opcode::DADD: name = "DADD"; break;
    case Sm4Opcode::DMAX: name = "DMAX"; break;
    case Sm4Opcode::DMIN: name = "DMIN"; break;
    case Sm4Opcode::DMUL: name = "DMUL"; break;
    case Sm4Opcode::DDIV: name = "DDIV"; break;
    case Sm4Opcode::DFMA: name = "DFMA"; sources = 3; break;
    case Sm4Opcode::DMOVC: name = "DMOVC"; sources = 3; break;
    case Sm4Opcode::DMOV: name = "DMOV"; sources = 1; break;
    case Sm4Opcode::DRCP: name = "DRCP"; sources = 1; break;
    case Sm4Opcode::DEQ: name = "DEQ"; double_result = false; break;
    case Sm4Opcode::DGE: name = "DGE"; double_result = false; break;
    case Sm4Opcode::DLT: name = "DLT"; double_result = false; break;
    case Sm4Opcode::DNE: name = "DNE"; double_result = false; break;
    case Sm4Opcode::DTOF:
        name = "DTOF"; sources = 1; double_result = false; break;
    case Sm4Opcode::DTOI:
        name = "DTOI"; sources = 1; double_result = false; break;
    case Sm4Opcode::DTOU:
        name = "DTOU"; sources = 1; double_result = false; break;
    case Sm4Opcode::FTOD:
        name = "FTOD"; sources = 1; double_sources = false; break;
    case Sm4Opcode::ITOD:
        name = "ITOD"; sources = 1; double_sources = false; break;
    case Sm4Opcode::UTOD:
        name = "UTOD"; sources = 1; double_sources = false; break;
    default:
        return fail("double: unexpected opcode");
    }
    const std::string label(name);
    Sm4Operand dst;
    std::array<Sm4Operand, 3> src{};
    if (!m_decoder.decode_operand(dst)) return fail(label + ": bad operands");
    for (u32 i = 0; i < sources; ++i) {
        if (!m_decoder.decode_operand(src[i])) {
            return fail(label + ": bad operands");
        }
    }
    // Source modifiers each instruction's syntax admits: [-]src[_abs] on a
    // double (the sign of its high word: 22.16.1 "the modifiers ... assume
    // the data is floating point"), dtof [-] only, dmovc's 32-bit condition
    // none, ftod [-] (a float negate), itod [-] (an integer negate), utod
    // none.
    std::array<u32, 3> bits{};
    for (u32 i = 0; i < sources; ++i) {
        u32 allowed = 3u;
        if (opcode == Sm4Opcode::DTOF || opcode == Sm4Opcode::FTOD ||
            opcode == Sm4Opcode::ITOD) {
            allowed = 1u;
        }
        if (opcode == Sm4Opcode::UTOD ||
            (opcode == Sm4Opcode::DMOVC && i == 0)) {
            allowed = 0u;
        }
        if (src[i].modifier > allowed) {
            return fail(label + ": unsupported source modifier");
        }
        if (!load_bits(src[i], bits[i])) return false;
        const u32 modifier = src[i].modifier;
        if (modifier == 0u) continue;
        if (!double_sources) {
            bits[i] = opcode == Sm4Opcode::ITOD
                ? m_builder.emit_bitcast(
                      m_type_uvec4,
                      m_builder.emit_snegate(
                          m_type_ivec4,
                          m_builder.emit_bitcast(m_type_ivec4, bits[i])))
                : m_builder.emit_bitwise_xor(
                      m_type_uvec4, bits[i],
                      make_uvec4_imm(0x80000000u, 0x80000000u, 0x80000000u,
                                     0x80000000u));
            continue;
        }
        if (modifier == 1u) {
            bits[i] = m_builder.emit_bitwise_xor(
                m_type_uvec4, bits[i],
                make_uvec4_imm(0u, 0x80000000u, 0u, 0x80000000u));
        } else if (modifier == 2u) {
            bits[i] = m_builder.emit_bitwise_and(
                m_type_uvec4, bits[i],
                make_uvec4_imm(~0u, 0x7FFFFFFFu, ~0u, 0x7FFFFFFFu));
        } else {
            bits[i] = m_builder.emit_bitwise_or(
                m_type_uvec4, bits[i],
                make_uvec4_imm(0u, 0x80000000u, 0u, 0x80000000u));
        }
    }
    if (dst.type == Sm4OperandType::Null) return true;

    // The destination names the lanes: a double result fills .xy (from the
    // post-swizzle .xy sources) and/or .zw (from .zw); a 32-bit result has
    // one or two components, the first taking the first double's result.
    // ftod/itod/utod convert source .x into .xy and source .y into .zw.
    const u32 mask = dst.write_mask;
    std::vector<u32> lanes;
    std::vector<u32> components;
    if (double_result) {
        if (mask == 0x3u) {
            lanes = {0u};
        } else if (mask == 0xCu) {
            lanes = {1u};
        } else if (mask == 0xFu) {
            lanes = {0u, 1u};
        } else {
            return fail(label + ": destination mask must be .xy, .zw or .xyzw");
        }
    } else {
        for (u32 c = 0; c < 4; ++c) {
            if ((mask & (1u << c)) != 0u) components.push_back(c);
        }
        if (components.empty() || components.size() > 2u) {
            return fail(label + ": destination must name one or two components");
        }
        lanes = components.size() == 1u ? std::vector<u32>{0u}
                                        : std::vector<u32>{0u, 1u};
    }

    const auto word = [&](u32 vector, u32 component) {
        return m_builder.emit_composite_extract(m_type_uint, vector,
                                                {component});
    };
    const auto pair_args = [&](u32 vector, u32 lane) {
        return std::vector<u32>{word(vector, 2u * lane),
                                word(vector, 2u * lane + 1u)};
    };
    const auto join = [](std::vector<u32> a, const std::vector<u32>& b) {
        a.insert(a.end(), b.begin(), b.end());
        return a;
    };
    const u32 zero = m_builder.emit_constant_int(m_type_uint, 0u);
    std::array<u32, 4> out{zero, zero, zero, zero};
    for (u32 lane : lanes) {
        u32 result = 0;
        switch (opcode) {
        case Sm4Opcode::DADD:
        case Sm4Opcode::DMUL:
        case Sm4Opcode::DDIV:
        case Sm4Opcode::DMIN:
        case Sm4Opcode::DMAX:
        case Sm4Opcode::DEQ:
        case Sm4Opcode::DGE:
        case Sm4Opcode::DLT:
        case Sm4Opcode::DNE: {
            const DoubleHelper helper =
                opcode == Sm4Opcode::DADD ? DoubleHelper::Add
                : opcode == Sm4Opcode::DMUL ? DoubleHelper::Mul
                : opcode == Sm4Opcode::DDIV ? DoubleHelper::Div
                : opcode == Sm4Opcode::DMIN ? DoubleHelper::Min
                : opcode == Sm4Opcode::DMAX ? DoubleHelper::Max
                : opcode == Sm4Opcode::DGE  ? DoubleHelper::Ge
                : opcode == Sm4Opcode::DLT  ? DoubleHelper::Lt
                                            : DoubleHelper::Eq;
            result = call_double(helper, join(pair_args(bits[0], lane),
                                              pair_args(bits[1], lane)));
            if (opcode == Sm4Opcode::DNE) {
                // NaN: deq false, so dne true (22.15.4).
                result = m_builder.emit_not(m_type_uint, result);
            }
            break;
        }
        case Sm4Opcode::DFMA:
            result = call_double(
                DoubleHelper::Fma,
                join(join(pair_args(bits[0], lane), pair_args(bits[1], lane)),
                     pair_args(bits[2], lane)));
            break;
        case Sm4Opcode::DRCP:
            result = call_double(
                DoubleHelper::Div,
                join({zero, m_builder.emit_constant_int(m_type_uint,
                                                        0x3FF00000u)},
                     pair_args(bits[0], lane)));
            break;
        case Sm4Opcode::DMOV:
            result = m_builder.emit_composite_construct(
                m_type_uvec2, pair_args(bits[0], lane));
            break;
        case Sm4Opcode::DMOVC: {
            // 22.16.2: the lane's condition is the source's post-swizzle
            // .x (for .xy) or .y (for .zw), any bit set.
            const u32 condition = m_builder.emit_inotequal(
                m_type_bool, word(bits[0], lane), zero);
            const auto taken = pair_args(bits[1], lane);
            const auto other = pair_args(bits[2], lane);
            result = m_builder.emit_composite_construct(
                m_type_uvec2,
                {m_builder.emit_select(m_type_uint, condition, taken[0],
                                       other[0]),
                 m_builder.emit_select(m_type_uint, condition, taken[1],
                                       other[1])});
            break;
        }
        case Sm4Opcode::DTOF:
        case Sm4Opcode::DTOI:
        case Sm4Opcode::DTOU:
            result = call_double(opcode == Sm4Opcode::DTOF ? DoubleHelper::ToFloat
                                 : opcode == Sm4Opcode::DTOI ? DoubleHelper::ToInt
                                                             : DoubleHelper::ToUint,
                                 pair_args(bits[0], lane));
            break;
        case Sm4Opcode::FTOD:
        case Sm4Opcode::ITOD:
        case Sm4Opcode::UTOD:
            result = call_double(opcode == Sm4Opcode::FTOD ? DoubleHelper::FromFloat
                                 : opcode == Sm4Opcode::ITOD ? DoubleHelper::FromInt
                                                             : DoubleHelper::FromUint,
                                 {word(bits[0], lane)});
            break;
        default:
            return fail(label + ": unexpected opcode");
        }
        // _sat, measured on WARP for every double instruction: a double
        // result saturates as a double, dtof's as a float (denormals and -0
        // to +0), and the integer results (deq..dne, dtoi, dtou) ignore it.
        if (m_saturate_destination) {
            if (double_result) {
                result = call_double(
                    DoubleHelper::Saturate,
                    {m_builder.emit_composite_extract(m_type_uint, result, {0u}),
                     m_builder.emit_composite_extract(m_type_uint, result, {1u})});
            } else if (opcode == Sm4Opcode::DTOF) {
                result = call_double(DoubleHelper::SaturateFloat, {result});
            }
        }
        if (double_result) {
            out[2u * lane] =
                m_builder.emit_composite_extract(m_type_uint, result, {0u});
            out[2u * lane + 1u] =
                m_builder.emit_composite_extract(m_type_uint, result, {1u});
        } else {
            out[components[lane]] = result;
        }
    }
    // The saturate is done; store_vec4 must not clamp the words as floats.
    m_saturate_destination = false;
    const u32 value = m_builder.emit_composite_construct(
        m_type_uvec4, {out[0], out[1], out[2], out[3]});
    return store_vec4(dst, m_builder.emit_bitcast(m_type_vec4, value));
}

bool DxbcTranslator::emit_double_helpers() {
    using SoftDouble = soft_double::SoftDouble<SpirvIntMachine>;
    using W = SoftDouble::W;
    for (usize index = 0; index < m_double_helpers.size(); ++index) {
        const u32 function = m_double_helpers[index];
        if (function == 0) continue;
        const DoubleHelperInfo& info = kDoubleHelpers[index];
        if (m_type_uint_pair == 0) {
            m_type_uint_pair =
                m_builder.emit_type_struct({m_type_uint, m_type_uint});
        }
        const u32 return_type = info.pair ? m_type_uvec2 : m_type_uint;
        u32& function_type =
            m_double_function_types[info.params * 2u + (info.pair ? 1u : 0u)];
        if (function_type == 0) {
            function_type = m_builder.emit_type_function(
                return_type, std::vector<u32>(info.params, m_type_uint));
        }
        m_builder.emit_function_id(function, return_type, function_type);
        m_builder.emit_name(function, info.name);
        std::vector<u32> p(info.params);
        for (u32& parameter : p) {
            parameter = m_builder.emit_function_parameter(m_type_uint);
        }
        const u32 entry = m_builder.emit_label();
        SpirvIntMachine machine(m_builder, m_type_uint, m_type_bool,
                                m_type_uint_pair, m_glsl_std450, entry);
        SoftDouble sd(machine);
        const auto pair = [&](W w) {
            return m_builder.emit_composite_construct(m_type_uvec2,
                                                      {w.lo, w.hi});
        };
        const auto mask = [&](u32 condition) {
            return m_builder.emit_select(
                m_type_uint, condition,
                m_builder.emit_constant_int(m_type_uint, 0xFFFFFFFFu),
                m_builder.emit_constant_int(m_type_uint, 0u));
        };
        const auto arg = [&](u32 first) { return W{p[first], p[first + 1u]}; };
        u32 result = 0;
        switch (static_cast<DoubleHelper>(index)) {
        case DoubleHelper::Add: result = pair(sd.add(arg(0), arg(2))); break;
        case DoubleHelper::Mul: result = pair(sd.mul(arg(0), arg(2))); break;
        case DoubleHelper::Div: result = pair(sd.div(arg(0), arg(2))); break;
        case DoubleHelper::Fma:
            result = pair(sd.fma(arg(0), arg(2), arg(4)));
            break;
        case DoubleHelper::Min: result = pair(sd.min(arg(0), arg(2))); break;
        case DoubleHelper::Max: result = pair(sd.max(arg(0), arg(2))); break;
        case DoubleHelper::Eq: result = mask(sd.eq(arg(0), arg(2))); break;
        case DoubleHelper::Ge: result = mask(sd.ge(arg(0), arg(2))); break;
        case DoubleHelper::Lt: result = mask(sd.lt(arg(0), arg(2))); break;
        case DoubleHelper::ToFloat: result = sd.to_float(arg(0)); break;
        case DoubleHelper::FromFloat: result = pair(sd.from_float(p[0])); break;
        case DoubleHelper::ToInt: result = sd.to_int(arg(0)); break;
        case DoubleHelper::ToUint: result = sd.to_uint(arg(0)); break;
        case DoubleHelper::FromInt: result = pair(sd.from_int(p[0])); break;
        case DoubleHelper::FromUint: result = pair(sd.from_uint(p[0])); break;
        case DoubleHelper::Saturate: result = pair(sd.saturate(arg(0))); break;
        case DoubleHelper::SaturateFloat:
            result = sd.saturate_float(p[0]);
            break;
        case DoubleHelper::Count:
            return fail("double: helper index out of range");
        }
        m_builder.emit_return_value(result);
        m_builder.emit_function_end();
    }
    return true;
}

} // namespace vitreo::graphics::dxbc_spirv_detail
