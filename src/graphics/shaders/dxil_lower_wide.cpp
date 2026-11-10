// SPDX-License-Identifier: LGPL-2.1-only
/// @file dxil_lower_wide.cpp
/// @brief 64-bit integers (Int64ShaderOps) and native 16-bit types
/// (Native16BitShaderOpsSupported, dxc -enable-16bit-types) in the DXIL ->
/// SM5.1 lowering.
///
/// The register model is dxbc_vitreo_ext.hpp's: an i64 is a component pair
/// whose arithmetic is VX_I64; an i16 is its 16 bits zero-extended in a
/// component, computed with the 32-bit instructions and truncated (signed
/// operations sign-extend first); a half is the float32 that represents it,
/// computed in float32 and rounded to half (VX_F16 Round) after every
/// operation that produces one. For +, -, *, / and sqrt float32 carries more
/// than twice half's 11 significant bits, so rounding the float32 result is
/// the correctly rounded half result (no double-rounding error).
///
/// LLVM semantics (LLVM 3.7 Language Reference): integer arithmetic wraps;
/// shifts by the operand's width or more are undefined (dxc masks the count
/// itself); trunc keeps the low bits, zext / sext extend. DXIL overloads of
/// the dx.op calls: DXIL.rst's operation table. FirstbitHi / FirstbitSHi on
/// i16 / i64 count from the type's top bit (dxc 1.6 follows the i64 call
/// with `sub 63, r` and the i16 one with `sub 15, r`: tests/oracle_data/
/// d3d12_sm6x/cs_int64x.dxil.txt, cs_halfx.dxil.txt).

#include "dxbc_vitreo_ext.hpp"
#include "dxil_lower_ext.hpp"

#include <cstring>

namespace vitreo::graphics::dxil_lower {

using namespace dxbc_vitreo_ext;

namespace {

constexpr u32 op(Sm4Opcode o) { return static_cast<u32>(o); }

/// The lowering of one wide instruction, with its emit helpers.
class Wide {
public:
    explicit Wide(LowerHost& host) : h_(host), m_(host.module()), fn_(host.function()), e_(host.body()) {}

    bool instruction(const DxilInstruction& inst);
    bool call(u32 opcode, const DxilInstruction& inst);

private:
    LowerHost& h_;
    const DxilModule& m_;
    const DxilFunction* fn_;
    Emitter& e_;

    WideKind kind(u32 id) const { return wide_kind(m_, m_.type_of(fn_, id)); }
    WideKind kind_of_type(u32 t) const { return wide_kind(m_, t); }

    // ---- emit helpers ----
    RegSlot temp() { return h_.fresh_scalar(); }
    void mov(const Operand& d, const Operand& s) { e_.inst(op(Sm4Opcode::MOV), {&d, &s}); }
    void bin(Sm4Opcode o, const Operand& d, const Operand& a, const Operand& b) { e_.inst(op(o), {&d, &a, &b}); }
    void i64op(I64Op o, const Operand& d, std::initializer_list<const Operand*> srcs) {
        const Operand c = control_operand(static_cast<u32>(o));
        usize length = 1 + c.tokens.size() + d.tokens.size();
        for (const Operand* s : srcs) length += s->tokens.size();
        e_.raw(kOpI64 | (static_cast<u32>(length) << 24));
        for (u32 t : c.tokens) e_.raw(t);
        for (u32 t : d.tokens) e_.raw(t);
        for (const Operand* s : srcs) {
            for (u32 t : s->tokens) e_.raw(t);
        }
    }
    void f16op(F16Op o, const Operand& d, const Operand& s) {
        const Operand c = control_operand(static_cast<u32>(o));
        const usize length = 1 + c.tokens.size() + d.tokens.size() + s.tokens.size();
        e_.raw(kOpF16 | (static_cast<u32>(length) << 24));
        for (const Operand* x : {&c, &d, &s}) {
            for (u32 t : x->tokens) e_.raw(t);
        }
    }
    /// Rounds value @p id's float32 in place to half.
    void round_half(RegSlot s) {
        const Operand d = scalar_dst(s), v = scalar_src(s);
        f16op(F16Op::Round, d, v);
    }
    void mask16(RegSlot s) {
        const Operand d = scalar_dst(s), v = scalar_src(s), k = imm(0xFFFFu);
        bin(Sm4Opcode::AND, d, v, k);
    }
    /// A sign-extended copy of i16 operand @p id.
    Operand sext16(u32 id) {
        const RegSlot t = temp();
        const Operand d = scalar_dst(t), w = imm(16), o = imm(0), v = h_.src(id);
        e_.inst(op(Sm4Opcode::IBFE), {&d, &w, &o, &v});
        return scalar_src(t);
    }
    /// The low / high word of i64 operand @p id as 32-bit sources.
    /// The 64 bits of an i64 or double constant.
    bool const_bits64(u32 id, u64& v) {
        if (h_.const_u64(id, v)) return true;
        const DxilConstant* c = m_.constant(fn_, id);
        if (c == nullptr || c->kind != DxilConstantKind::Float) return false;
        v = c->bits;
        return true;
    }
    Operand lo(u32 id) {
        u64 v = 0;
        if (const_bits64(id, v)) return imm(static_cast<u32>(v));
        if (h_.undef(id)) return imm(0);
        const RegSlot s = h_.slot(id);
        return src_temp_scalar(s.reg, s.comp);
    }
    Operand hi(u32 id) {
        u64 v = 0;
        if (const_bits64(id, v)) return imm(static_cast<u32>(v >> 32));
        if (h_.undef(id)) return imm(0);
        const RegSlot s = h_.slot(id);
        return src_temp_scalar(s.reg, s.comp + 1);
    }
    Operand lo_dst(u32 id) {
        const RegSlot s = h_.slot(id);
        return dst_temp(s.reg, 1u << s.comp);
    }
    Operand hi_dst(u32 id) {
        const RegSlot s = h_.slot(id);
        return dst_temp(s.reg, 1u << (s.comp + 1));
    }
    /// Signed 32-bit division on sign-extended operands (|a| / |b| through
    /// udiv, the quotient taking sign(a) ^ sign(b), the remainder sign(a)).
    void signed_divide(bool remainder, const Operand& d, const Operand& a, const Operand& b);

    /// A double source: modifier 1 negates, 2 takes the magnitude (a
    /// constant's sign bit is changed directly).
    Operand dsrc(u32 id, u32 modifier = 0) {
        u64 v = 0;
        if (const_bits64(id, v)) {
            if (modifier == 1) v ^= 1ull << 63;
            if (modifier == 2) v &= ~(1ull << 63);
            const u32 l = static_cast<u32>(v), hh = static_cast<u32>(v >> 32);
            return imm4(l, hh, l, hh);
        }
        if (h_.undef(id)) return imm(0);
        return h_.src(id, modifier);
    }
    /// f64 -> i64 / u64 toward zero (LLVM fptosi / fptoui; out of range is
    /// poison there): the 53-bit significand shifted by exponent - 1075.
    void double_to_i64(bool sign, const Operand& d, u32 x);
    /// f64 -> half, rounded once: the double is first rounded to float32
    /// toward zero with the inexact bit kept in its last place ("round to
    /// odd"; float32 has 13 more significand bits than half, so the second
    /// rounding cannot land on a false tie).
    void double_to_half(const Operand& d, u32 x);
    bool binop_f64(const DxilInstruction& inst);
    bool cmp_f64(const DxilInstruction& inst);
    bool cast_f64(const DxilInstruction& inst);
    bool call_f64(u32 opcode, const DxilInstruction& inst);

    bool binop(const DxilInstruction& inst);
    bool cmp(const DxilInstruction& inst);
    bool cast(const DxilInstruction& inst);
    bool raw_load(u32 opcode, const DxilInstruction& inst);
    bool raw_store(u32 opcode, const DxilInstruction& inst);
    bool cbuffer_load(const DxilInstruction& inst);
    /// BufferStore / TextureStore of int16 values to a signed typed view:
    /// the values sign-extended, as an sint view stores them. Arguments
    /// (after the opcode): handle, @p coords coordinates from
    /// @p first_coord, the four values from @p first_value, the mask.
    bool typed_store_signed16(const DxilInstruction& inst, const ResourceView& v, u32 first_coord, u32 coords,
                              u32 first_value);
    /// After the 32-bit path wrote aggregate @p id's four components:
    /// round them to half or truncate them to 16 bits.
    bool narrow_aggregate(u32 id, WideKind k, u32 components);
    /// The DXIL signature component type of output element @p element.
    u32 output_component_type(u32 element, u32* start_row = nullptr, u32* start_col = nullptr) const;
};

void Wide::signed_divide(bool remainder, const Operand& d, const Operand& a, const Operand& b) {
    const RegSlot aa = temp(), bb = temp(), q = temp(), s = temp(), nq = temp();
    const Operand nul = null_operand(), zero = imm(0);
    {
        const Operand ad = scalar_dst(aa), bd = scalar_dst(bb);
        Operand na = a, nb = b;
        // Negated copies through ineg (a register source's modifier would
        // also do, but an immediate needs its own value).
        const RegSlot ta = temp(), tb = temp();
        const Operand tad = scalar_dst(ta), tbd = scalar_dst(tb);
        e_.inst(op(Sm4Opcode::INEG), {&tad, &a});
        e_.inst(op(Sm4Opcode::INEG), {&tbd, &b});
        na = scalar_src(ta);
        nb = scalar_src(tb);
        bin(Sm4Opcode::IMAX, ad, a, na);
        bin(Sm4Opcode::IMAX, bd, b, nb);
    }
    const Operand as = scalar_src(aa), bs = scalar_src(bb), qd = scalar_dst(q), qs = scalar_src(q);
    if (remainder) e_.inst(op(Sm4Opcode::UDIV), {&nul, &qd, &as, &bs});
    else e_.inst(op(Sm4Opcode::UDIV), {&qd, &nul, &as, &bs});
    const Operand sd = scalar_dst(s), ss = scalar_src(s);
    if (remainder) {
        bin(Sm4Opcode::ILT, sd, a, zero);
    } else {
        bin(Sm4Opcode::XOR, sd, a, b);
        bin(Sm4Opcode::ILT, sd, ss, zero);
    }
    const Operand nqd = scalar_dst(nq), nqs = scalar_src(nq);
    e_.inst(op(Sm4Opcode::INEG), {&nqd, &qs});
    e_.inst(op(Sm4Opcode::MOVC), {&d, &ss, &nqs, &qs});
}

// ---- LLVM instructions ----------------------------------------------------------

bool Wide::binop(const DxilInstruction& inst) {
    const WideKind k = kind_of_type(inst.type);
    if (k == WideKind::F64) return binop_f64(inst);
    const u32 a = inst.operands[0], b = inst.operands[1];
    const auto sub = static_cast<DxilBinop>(inst.sub);
    if (k == WideKind::I64) {
        I64Op o;
        switch (sub) {
        case DxilBinop::Add: o = I64Op::Add; break;
        case DxilBinop::Sub: o = I64Op::Sub; break;
        case DxilBinop::Mul: o = I64Op::Mul; break;
        case DxilBinop::UDiv: o = I64Op::UDiv; break;
        case DxilBinop::SDiv: o = I64Op::SDiv; break;
        case DxilBinop::URem: o = I64Op::URem; break;
        case DxilBinop::SRem: o = I64Op::SRem; break;
        case DxilBinop::Shl: o = I64Op::Shl; break;
        case DxilBinop::LShr: o = I64Op::LShr; break;
        case DxilBinop::AShr: o = I64Op::AShr; break;
        case DxilBinop::And: o = I64Op::And; break;
        case DxilBinop::Or: o = I64Op::Or; break;
        case DxilBinop::Xor: o = I64Op::Xor; break;
        default: return h_.malformed("unknown binary operator");
        }
        const Operand d = h_.dst(inst.result);
        // The shift count is an i64 too; its low word is the count.
        const Operand sa = h_.src(a), sb = h_.src(b);
        i64op(o, d, {&sa, &sb});
        return true;
    }
    const RegSlot r = h_.slot(inst.result);
    const Operand d = scalar_dst(r);
    if (k == WideKind::F16) {
        const Operand sa = h_.src(a), sb = h_.src(b);
        switch (sub) {
        case DxilBinop::Add: bin(Sm4Opcode::ADD, d, sa, sb); break;
        case DxilBinop::Sub: {
            const Operand nb = h_.src(b, 1);
            bin(Sm4Opcode::ADD, d, sa, nb);
            break;
        }
        case DxilBinop::Mul: bin(Sm4Opcode::MUL, d, sa, sb); break;
        case DxilBinop::SDiv: bin(Sm4Opcode::DIV, d, sa, sb); break;
        case DxilBinop::SRem: {
            // frem: a - b * trunc(a / b), in float32, then rounded.
            const RegSlot q = temp();
            const Operand qd = scalar_dst(q), qs = scalar_src(q), qn = src_temp_scalar(q.reg, q.comp, 1);
            bin(Sm4Opcode::DIV, qd, sa, sb);
            e_.inst(op(Sm4Opcode::ROUND_Z), {&qd, &qs});
            e_.inst(op(Sm4Opcode::MAD), {&d, &qn, &sb, &sa});
            break;
        }
        default: return h_.malformed("integer operator on a half");
        }
        round_half(r);
        return true;
    }
    // i16: 32-bit arithmetic on the zero-extended values, truncated.
    const Operand nul = null_operand();
    switch (sub) {
    case DxilBinop::Add: {
        const Operand sa = h_.src(a), sb = h_.src(b);
        e_.inst(op(Sm4Opcode::IADD), {&d, &sa, &sb});
        break;
    }
    case DxilBinop::Sub: {
        const Operand sa = h_.src(a), nb = h_.src(b, 1);
        e_.inst(op(Sm4Opcode::IADD), {&d, &sa, &nb});
        break;
    }
    case DxilBinop::Mul: {
        const Operand sa = h_.src(a), sb = h_.src(b);
        e_.inst(op(Sm4Opcode::IMUL), {&nul, &d, &sa, &sb});
        break;
    }
    case DxilBinop::Shl: {
        const Operand sa = h_.src(a), sb = h_.src(b);
        bin(Sm4Opcode::ISHL, d, sa, sb);
        break;
    }
    case DxilBinop::LShr: {
        const Operand sa = h_.src(a), sb = h_.src(b);
        bin(Sm4Opcode::USHR, d, sa, sb);
        return true;  // stays within 16 bits
    }
    case DxilBinop::AShr: {
        const Operand sa = sext16(a), sb = h_.src(b);
        bin(Sm4Opcode::ISHR, d, sa, sb);
        break;
    }
    case DxilBinop::And:
    case DxilBinop::Or:
    case DxilBinop::Xor: {
        const Operand sa = h_.src(a), sb = h_.src(b);
        bin(sub == DxilBinop::And ? Sm4Opcode::AND : sub == DxilBinop::Or ? Sm4Opcode::OR : Sm4Opcode::XOR, d, sa,
            sb);
        return true;
    }
    case DxilBinop::UDiv:
    case DxilBinop::URem: {
        const Operand sa = h_.src(a), sb = h_.src(b);
        if (sub == DxilBinop::UDiv) e_.inst(op(Sm4Opcode::UDIV), {&d, &nul, &sa, &sb});
        else e_.inst(op(Sm4Opcode::UDIV), {&nul, &d, &sa, &sb});
        break;
    }
    case DxilBinop::SDiv:
    case DxilBinop::SRem: {
        const Operand sa = sext16(a), sb = sext16(b);
        signed_divide(sub == DxilBinop::SRem, d, sa, sb);
        break;
    }
    default: return h_.malformed("unknown binary operator");
    }
    mask16(r);
    return true;
}

bool Wide::cmp(const DxilInstruction& inst) {
    const u32 a = inst.operands[0], b = inst.operands[1];
    const WideKind k = kind(a);
    if (k == WideKind::F64) return cmp_f64(inst);
    const auto pred = static_cast<DxilPredicate>(inst.sub);
    const Operand d = h_.dst(inst.result);
    if (k == WideKind::I64) {
        const Operand sa = h_.src(a), sb = h_.src(b);
        switch (pred) {
        case DxilPredicate::IEq: i64op(I64Op::Eq, d, {&sa, &sb}); break;
        case DxilPredicate::INe: i64op(I64Op::Ne, d, {&sa, &sb}); break;
        case DxilPredicate::IUlt: i64op(I64Op::ULt, d, {&sa, &sb}); break;
        case DxilPredicate::IUge: i64op(I64Op::UGe, d, {&sa, &sb}); break;
        case DxilPredicate::IUgt: i64op(I64Op::ULt, d, {&sb, &sa}); break;
        case DxilPredicate::IUle: i64op(I64Op::UGe, d, {&sb, &sa}); break;
        case DxilPredicate::ISlt: i64op(I64Op::SLt, d, {&sa, &sb}); break;
        case DxilPredicate::ISge: i64op(I64Op::SGe, d, {&sa, &sb}); break;
        case DxilPredicate::ISgt: i64op(I64Op::SLt, d, {&sb, &sa}); break;
        case DxilPredicate::ISle: i64op(I64Op::SGe, d, {&sb, &sa}); break;
        default: return h_.malformed("float predicate on an integer");
        }
        return true;
    }
    // i16: equality and the unsigned order on the zero-extended values,
    // the signed order on sign-extended copies.
    const bool sign = pred == DxilPredicate::ISlt || pred == DxilPredicate::ISge || pred == DxilPredicate::ISgt ||
                      pred == DxilPredicate::ISle;
    const Operand sa = sign ? sext16(a) : h_.src(a);
    const Operand sb = sign ? sext16(b) : h_.src(b);
    switch (pred) {
    case DxilPredicate::IEq: bin(Sm4Opcode::IEQ, d, sa, sb); break;
    case DxilPredicate::INe: bin(Sm4Opcode::INE, d, sa, sb); break;
    case DxilPredicate::IUlt: bin(Sm4Opcode::ULT, d, sa, sb); break;
    case DxilPredicate::IUge: bin(Sm4Opcode::UGE, d, sa, sb); break;
    case DxilPredicate::IUgt: bin(Sm4Opcode::ULT, d, sb, sa); break;
    case DxilPredicate::IUle: bin(Sm4Opcode::UGE, d, sb, sa); break;
    case DxilPredicate::ISlt: bin(Sm4Opcode::ILT, d, sa, sb); break;
    case DxilPredicate::ISge: bin(Sm4Opcode::IGE, d, sa, sb); break;
    case DxilPredicate::ISgt: bin(Sm4Opcode::ILT, d, sb, sa); break;
    case DxilPredicate::ISle: bin(Sm4Opcode::IGE, d, sb, sa); break;
    default: return h_.malformed("float predicate on an integer");
    }
    return true;
}

bool Wide::cast(const DxilInstruction& inst) {
    const u32 x = inst.operands[0];
    const u32 from_t = m_.type_of(fn_, x), to_t = inst.type;
    const WideKind from = kind_of_type(from_t), to = kind_of_type(to_t);
    const DxilType& ft = m_.types[from_t];
    const DxilType& tt = m_.types[to_t];
    const bool from_i1 = ft.kind == DxilTypeKind::Integer && ft.width == 1;
    const bool from_i32 = ft.kind == DxilTypeKind::Integer && ft.width == 32;
    const bool to_i1 = tt.kind == DxilTypeKind::Integer && tt.width == 1;
    const bool to_i32 = tt.kind == DxilTypeKind::Integer && tt.width == 32;
    const bool from_f32 = ft.kind == DxilTypeKind::Float, to_f32 = tt.kind == DxilTypeKind::Float;
    if (from == WideKind::F64 || to == WideKind::F64) return cast_f64(inst);
    if (from == WideKind::Other || to == WideKind::Other) {
        return h_.unimplemented("conversion " + dxil_type_name(m_, from_t) + " -> " + dxil_type_name(m_, to_t));
    }
    const auto c = static_cast<DxilCast>(inst.sub);
    const auto unsupported = [&]() {
        return h_.unimplemented("cast " + std::to_string(inst.sub) + " " + dxil_type_name(m_, from_t) + " -> " +
                                dxil_type_name(m_, to_t));
    };
    switch (c) {
    case DxilCast::Trunc: {
        const Operand d = h_.dst(inst.result);
        const Operand s = from == WideKind::I64 ? lo(x) : h_.src(x);
        if (to_i1) {
            const RegSlot t = temp();
            const Operand td = scalar_dst(t), ts = scalar_src(t), one = imm(1), zero = imm(0);
            bin(Sm4Opcode::AND, td, s, one);
            bin(Sm4Opcode::INE, d, ts, zero);
        } else if (to == WideKind::I16) {
            const Operand k = imm(0xFFFFu);
            bin(Sm4Opcode::AND, d, s, k);
        } else if (to_i32 && from == WideKind::I64) {
            mov(d, s);
        } else {
            return unsupported();
        }
        return true;
    }
    case DxilCast::ZExt:
    case DxilCast::SExt: {
        const bool sign = c == DxilCast::SExt;
        if (to == WideKind::I64) {
            const Operand dl = lo_dst(inst.result), dh = hi_dst(inst.result);
            if (from_i1) {
                // i1 is 0 / ~0: zext gives 0 / 1, sext 0 / ~0 in both words.
                const Operand s = h_.src(x);
                if (sign) {
                    mov(dl, s);
                    mov(dh, s);
                } else {
                    const Operand one = imm(1), zero = imm(0);
                    bin(Sm4Opcode::AND, dl, s, one);
                    mov(dh, zero);
                }
                return true;
            }
            const Operand s = (from == WideKind::I16 && sign) ? sext16(x) : h_.src(x);
            mov(dl, s);
            if (sign) {
                const RegSlot r = h_.slot(inst.result);
                const Operand l = src_temp_scalar(r.reg, r.comp), sh = imm(31);
                bin(Sm4Opcode::ISHR, dh, l, sh);
            } else {
                const Operand zero = imm(0);
                mov(dh, zero);
            }
            return true;
        }
        const Operand d = h_.dst(inst.result);
        if (from_i1) {
            const Operand s = h_.src(x);
            const Operand k = imm(sign ? 0xFFFFu : 1u);  // to i16 (or i32 from the host)
            bin(Sm4Opcode::AND, d, s, k);
            return true;
        }
        if (from == WideKind::I16 && to_i32) {
            if (sign) {
                const Operand w = imm(16), o = imm(0), s = h_.src(x);
                e_.inst(op(Sm4Opcode::IBFE), {&d, &w, &o, &s});
            } else {
                const Operand s = h_.src(x);
                mov(d, s);
            }
            return true;
        }
        return unsupported();
    }
    case DxilCast::FPTrunc: {
        if (!(from_f32 && to == WideKind::F16)) return unsupported();
        const Operand d = h_.dst(inst.result), s = h_.src(x);
        f16op(F16Op::Round, d, s);
        return true;
    }
    case DxilCast::FPExt: {
        if (!(from == WideKind::F16 && to_f32)) return unsupported();
        const Operand d = h_.dst(inst.result), s = h_.src(x);
        mov(d, s);  // exact
        return true;
    }
    case DxilCast::FPToSI:
    case DxilCast::FPToUI: {
        const bool sign = c == DxilCast::FPToSI;
        if (!(from_f32 || from == WideKind::F16)) return unsupported();
        const Operand s = h_.src(x);
        if (to == WideKind::I64) {
            const Operand d = h_.dst(inst.result);
            i64op(sign ? I64Op::F32ToS : I64Op::F32ToU, d, {&s});
            return true;
        }
        if (to == WideKind::I16 || to_i32) {
            const RegSlot r = h_.slot(inst.result);
            const Operand d = scalar_dst(r);
            e_.inst(op(sign ? Sm4Opcode::FTOI : Sm4Opcode::FTOU), {&d, &s});
            if (to == WideKind::I16) mask16(r);
            return true;
        }
        return unsupported();
    }
    case DxilCast::SIToFP:
    case DxilCast::UIToFP: {
        const bool sign = c == DxilCast::SIToFP;
        if (!(to_f32 || to == WideKind::F16)) return unsupported();
        const RegSlot r = h_.slot(inst.result);
        const Operand d = scalar_dst(r);
        if (from == WideKind::I64) {
            const Operand s = h_.src(x);
            i64op(sign ? I64Op::SToF32 : I64Op::UToF32, d, {&s});
        } else if (from_i1) {
            const Operand s = h_.src(x), bits = imm(sign ? 0xBF800000u : 0x3F800000u);
            bin(Sm4Opcode::AND, d, s, bits);
        } else if (from == WideKind::I16 || from_i32) {
            const Operand s = (from == WideKind::I16 && sign) ? sext16(x) : h_.src(x);
            e_.inst(op(sign ? Sm4Opcode::ITOF : Sm4Opcode::UTOF), {&d, &s});
        } else {
            return unsupported();
        }
        if (to == WideKind::F16) round_half(r);
        return true;
    }
    case DxilCast::BitCast: {
        const Operand d = h_.dst(inst.result), s = h_.src(x);
        if (from == WideKind::F16 && to == WideKind::I16) f16op(F16Op::ToBits, d, s);
        else if (from == WideKind::I16 && to == WideKind::F16) f16op(F16Op::FromBits, d, s);
        else if (from == to) mov(d, s);
        else return unsupported();
        return true;
    }
    default:
        return unsupported();
    }
}

bool Wide::instruction(const DxilInstruction& inst) {
    switch (inst.op) {
    case DxilOpcode::Binop: return binop(inst);
    case DxilOpcode::Cmp: return cmp(inst);
    case DxilOpcode::Cast: return cast(inst);
    case DxilOpcode::Load: {
        const RegSlot s = h_.slot(inst.result);
        const Operand dl = dst_temp(s.reg, 1u << s.comp), dh = dst_temp(s.reg, 1u << (s.comp + 1));
        return h_.load_element(inst.operands[0], 0, dl) && h_.load_element(inst.operands[0], 1, dh);
    }
    case DxilOpcode::Store: {
        const u32 v = inst.operands[1];
        const Operand l = lo(v), hgh = hi(v);
        return h_.store_element(inst.operands[0], 0, l) && h_.store_element(inst.operands[0], 1, hgh);
    }
    case DxilOpcode::ExtractValue: {
        const auto& members = h_.ext().aggregates.at(inst.operands[0]);
        if (inst.indices.size() != 1) return h_.unimplemented("nested extractvalue");
        if (inst.indices[0] >= members.size()) {
            return h_.unimplemented("the resource status (CheckAccessFullyMapped) of a 16- or 64-bit load");
        }
        h_.bind(inst.result, members[inst.indices[0]]);
        return true;
    }
    default:
        return h_.unimplemented("LLVM instruction form " + std::to_string(static_cast<u32>(inst.op)) +
                                " on 16- or 64-bit values");
    }
}

// ---- doubles ----------------------------------------------------------------------
//
// A double is an aligned component pair holding its IEEE 754 binary64 bits
// (low word first), the operand layout of SM5's double instructions
// (D3D11.3 22.14: dadd, dmul, ddiv, dfma, dmin / dmax, deq / dge / dlt /
// dne, dmov(c), dtof / ftod, dtoi / dtou / itod / utod), which the
// translator lowers to its exact soft-double helpers. LLVM semantics: the
// LLVM 3.7 Language Reference (fadd .. fdiv round to nearest; fcmp's ordered
// and unordered predicates; fptosi / fptoui round toward zero, out of
// range is poison; sitofp / uitofp and fptrunc round to nearest).

bool Wide::binop_f64(const DxilInstruction& inst) {
    const u32 a = inst.operands[0], b = inst.operands[1];
    const Operand d = h_.dst(inst.result);
    const Operand sa = dsrc(a);
    switch (static_cast<DxilBinop>(inst.sub)) {
    case DxilBinop::Add: {
        const Operand sb = dsrc(b);
        e_.inst(op(Sm4Opcode::DADD), {&d, &sa, &sb});
        return true;
    }
    case DxilBinop::Sub: {
        const Operand nb = dsrc(b, 1);
        e_.inst(op(Sm4Opcode::DADD), {&d, &sa, &nb});
        return true;
    }
    case DxilBinop::Mul: {
        const Operand sb = dsrc(b);
        e_.inst(op(Sm4Opcode::DMUL), {&d, &sa, &sb});
        return true;
    }
    case DxilBinop::SDiv: {  // fdiv
        const Operand sb = dsrc(b);
        e_.inst(op(Sm4Opcode::DDIV), {&d, &sa, &sb});
        return true;
    }
    case DxilBinop::SRem:
        // frem is the exact a - b * trunc(a / b); SM5 has no double trunc.
        return h_.unimplemented("frem on double");
    default: return h_.malformed("integer operator on a double");
    }
}

bool Wide::cmp_f64(const DxilInstruction& inst) {
    const u32 a = inst.operands[0], b = inst.operands[1];
    const auto pred = static_cast<DxilPredicate>(inst.sub);
    const Operand d = h_.dst(inst.result);
    const Operand sa = dsrc(a), sb = dsrc(b);
    // deq / dge / dlt are ordered (false on NaN), dne unordered (D3D11.3
    // 22.14.6-9).
    const auto c = [&](Sm4Opcode o, const Operand& x, const Operand& y, const Operand& dd) {
        e_.inst(op(o), {&dd, &x, &y});
    };
    const auto negate = [&]() {
        const RegSlot r = h_.slot(inst.result);
        const Operand rd = scalar_dst(r), rs = scalar_src(r);
        e_.inst(op(Sm4Opcode::NOT), {&rd, &rs});
    };
    const auto both = [&](Sm4Opcode o1, const Operand& x1, const Operand& y1, Sm4Opcode o2, const Operand& x2,
                          const Operand& y2, Sm4Opcode join) {
        const RegSlot r = h_.slot(inst.result), t = temp();
        const Operand td = scalar_dst(t), ts = scalar_src(t), rs = scalar_src(r);
        c(o1, x1, y1, d);
        c(o2, x2, y2, td);
        bin(join, d, rs, ts);
    };
    switch (pred) {
    case DxilPredicate::FFalse: { const Operand z = imm(0); mov(d, z); break; }
    case DxilPredicate::FTrue: { const Operand o = imm(~0u); mov(d, o); break; }
    case DxilPredicate::FOeq: c(Sm4Opcode::DEQ, sa, sb, d); break;
    case DxilPredicate::FUne: c(Sm4Opcode::DNE, sa, sb, d); break;
    case DxilPredicate::FOlt: c(Sm4Opcode::DLT, sa, sb, d); break;
    case DxilPredicate::FOge: c(Sm4Opcode::DGE, sa, sb, d); break;
    case DxilPredicate::FOgt: c(Sm4Opcode::DLT, sb, sa, d); break;
    case DxilPredicate::FOle: c(Sm4Opcode::DGE, sb, sa, d); break;
    case DxilPredicate::FUge: c(Sm4Opcode::DLT, sa, sb, d); negate(); break;
    case DxilPredicate::FUlt: c(Sm4Opcode::DGE, sa, sb, d); negate(); break;
    case DxilPredicate::FUle: c(Sm4Opcode::DLT, sb, sa, d); negate(); break;
    case DxilPredicate::FUgt: c(Sm4Opcode::DGE, sb, sa, d); negate(); break;
    case DxilPredicate::FOne: both(Sm4Opcode::DLT, sa, sb, Sm4Opcode::DLT, sb, sa, Sm4Opcode::OR); break;
    case DxilPredicate::FUeq:
        both(Sm4Opcode::DLT, sa, sb, Sm4Opcode::DLT, sb, sa, Sm4Opcode::OR);
        negate();
        break;
    case DxilPredicate::FOrd: both(Sm4Opcode::DEQ, sa, sa, Sm4Opcode::DEQ, sb, sb, Sm4Opcode::AND); break;
    case DxilPredicate::FUno: both(Sm4Opcode::DNE, sa, sa, Sm4Opcode::DNE, sb, sb, Sm4Opcode::OR); break;
    default: return h_.malformed("integer predicate on a double");
    }
    return true;
}

void Wide::double_to_i64(bool sign, const Operand& d, u32 x) {
    const Operand h = hi(x), l = lo(x);
    const RegSlot e = temp(), s = temp(), ls = temp(), rs = temp(), neg = temp();
    const RegSlot m = h_.fresh_pair(), left = h_.fresh_pair(), right = h_.fresh_pair();
    const Operand ed = scalar_dst(e), es = scalar_src(e);
    {
        const Operand w = imm(11), at = imm(20);
        e_.inst(op(Sm4Opcode::UBFE), {&ed, &w, &at, &h});
    }
    {
        // The significand with its implicit bit: (hi & 0xFFFFF) | 0x100000 : lo.
        const Operand ml = dst_temp(m.reg, 1u << m.comp), mh = dst_temp(m.reg, 1u << (m.comp + 1));
        const Operand mhs = src_temp_scalar(m.reg, m.comp + 1), frac = imm(0xFFFFFu), one = imm(0x100000u);
        mov(ml, l);
        bin(Sm4Opcode::AND, mh, h, frac);
        bin(Sm4Opcode::OR, mh, mhs, one);
    }
    const Operand sd = scalar_dst(s), ss = scalar_src(s);
    {
        const Operand bias = imm(static_cast<u32>(-1075));
        e_.inst(op(Sm4Opcode::IADD), {&sd, &es, &bias});
    }
    // Shift counts clamped to 63 (a negative count reads as a large
    // unsigned one): left by s when s >= 0, right by -s otherwise; a
    // 53-bit significand shifted right by 63 is 0 (|x| < 1, zero, denormal).
    const Operand lsd = scalar_dst(ls), lss = scalar_src(ls), rsd = scalar_dst(rs), rss = scalar_src(rs);
    const Operand k63 = imm(63);
    bin(Sm4Opcode::UMIN, lsd, ss, k63);
    {
        const RegSlot t = temp();
        const Operand td = scalar_dst(t), ts = scalar_src(t);
        e_.inst(op(Sm4Opcode::INEG), {&td, &ss});
        bin(Sm4Opcode::UMIN, rsd, ts, k63);
    }
    const Operand ms = pair_src(m), ld = pair_dst(left), rd = pair_dst(right);
    i64op(I64Op::Shl, ld, {&ms, &lss});
    i64op(I64Op::LShr, rd, {&ms, &rss});
    const RegSlot ge = temp();
    const Operand ged = scalar_dst(ge), ges = scalar_src(ge), zero = imm(0);
    bin(Sm4Opcode::IGE, ged, ss, zero);
    const Operand lsrc = pair_src(left), rsrc = pair_src(right);
    const RegSlot mag = h_.fresh_pair(), res = h_.fresh_pair();
    const Operand magd = pair_dst(mag), mags = pair_src(mag), resd = pair_dst(res), ress = pair_src(res);
    e_.inst(op(Sm4Opcode::MOVC), {&magd, &ges, &lsrc, &rsrc});
    const Operand nd = scalar_dst(neg), ns = scalar_src(neg);
    bin(Sm4Opcode::ILT, nd, h, zero);
    Operand sat;
    if (sign) {
        const Operand z64 = imm(0);
        i64op(I64Op::Sub, resd, {&z64, &mags});
        e_.inst(op(Sm4Opcode::MOVC), {&resd, &ns, &ress, &mags});
    } else {
        // A negative value converts to 0.
        const Operand z64 = imm(0);
        e_.inst(op(Sm4Opcode::MOVC), {&resd, &ns, &z64, &mags});
    }
    // Out of range saturates and NaN gives 0, as SM5's dtoi / dtou do for
    // 32 bits (D3D11.3 22.14.11-12) and the RTX 4080 does for 64 bits
    // (d3d12_dxilc_probe cs_double2 words 10-11): |x| >= 2^63 (s >= 11)
    // signed, x >= 2^64 (s >= 12) unsigned.
    const RegSlot over = temp(), nan = temp(), satp = h_.fresh_pair();
    const Operand od = scalar_dst(over), os = scalar_src(over), lim = imm(sign ? 11u : 12u);
    bin(Sm4Opcode::IGE, od, ss, lim);
    const Operand spd = pair_dst(satp), sps = pair_src(satp);
    if (sign) {
        const Operand mn = imm4(0, 0x80000000u, 0, 0x80000000u), mx = imm4(~0u, 0x7FFFFFFFu, ~0u, 0x7FFFFFFFu);
        e_.inst(op(Sm4Opcode::MOVC), {&spd, &ns, &mn, &mx});
    } else {
        const Operand mx = imm(~0u), z64 = imm(0);
        e_.inst(op(Sm4Opcode::MOVC), {&spd, &ns, &z64, &mx});
    }
    e_.inst(op(Sm4Opcode::MOVC), {&resd, &os, &sps, &ress});
    const Operand nnd = scalar_dst(nan), nns = scalar_src(nan), sx = dsrc(x), z64 = imm(0);
    e_.inst(op(Sm4Opcode::DNE), {&nnd, &sx, &sx});
    e_.inst(op(Sm4Opcode::MOVC), {&d, &nns, &z64, &ress});
}

void Wide::double_to_half(const Operand& d, u32 x) {
    const RegSlot f = temp(), inexact = temp(), bigger = temp(), bit = temp();
    const RegSlot back = h_.fresh_pair();
    const Operand fd = scalar_dst(f), fs = scalar_src(f), sx = dsrc(x), ax = dsrc(x, 2);
    e_.inst(op(Sm4Opcode::DTOF), {&fd, &sx});
    {
        const Operand bd = pair_dst(back);
        e_.inst(op(Sm4Opcode::FTOD), {&bd, &fs});
    }
    const Operand bs = pair_src(back), ab = src_temp(back.reg, back.comp == 0 ? 0x44u : 0xEEu, 2);
    const Operand id = scalar_dst(inexact), is = scalar_src(inexact);
    const Operand gd = scalar_dst(bigger), gs = scalar_src(bigger);
    e_.inst(op(Sm4Opcode::DNE), {&id, &sx, &bs});
    e_.inst(op(Sm4Opcode::DLT), {&gd, &ax, &ab});
    // Toward zero: one float32 step down in magnitude when rounding went
    // up (~0 is -1); then the sticky inexact bit in the last place.
    e_.inst(op(Sm4Opcode::IADD), {&fd, &fs, &gs});
    const Operand td = scalar_dst(bit), ts = scalar_src(bit), one = imm(1);
    bin(Sm4Opcode::AND, td, is, one);
    bin(Sm4Opcode::OR, fd, fs, ts);
    f16op(F16Op::Round, d, fs);
}

bool Wide::cast_f64(const DxilInstruction& inst) {
    const u32 x = inst.operands[0];
    const u32 from_t = m_.type_of(fn_, x), to_t = inst.type;
    const WideKind from = kind_of_type(from_t), to = kind_of_type(to_t);
    const DxilType& ft = m_.types[from_t];
    const DxilType& tt = m_.types[to_t];
    const bool from_i1 = ft.kind == DxilTypeKind::Integer && ft.width == 1;
    const bool from_i32 = ft.kind == DxilTypeKind::Integer && ft.width == 32;
    const bool to_i32 = tt.kind == DxilTypeKind::Integer && tt.width == 32;
    const bool from_f32 = ft.kind == DxilTypeKind::Float, to_f32 = tt.kind == DxilTypeKind::Float;
    const auto unsupported = [&]() {
        return h_.unimplemented("cast " + std::to_string(inst.sub) + " " + dxil_type_name(m_, from_t) + " -> " +
                                dxil_type_name(m_, to_t));
    };
    const Operand d = h_.dst(inst.result);
    switch (static_cast<DxilCast>(inst.sub)) {
    case DxilCast::FPExt: {
        // float32 (and half, held as its exact float32) -> double: exact.
        if (!(from_f32 || from == WideKind::F16)) return unsupported();
        const Operand s = h_.src(x);
        e_.inst(op(Sm4Opcode::FTOD), {&d, &s});
        return true;
    }
    case DxilCast::FPTrunc: {
        const Operand s = dsrc(x);
        if (to_f32) {
            e_.inst(op(Sm4Opcode::DTOF), {&d, &s});
            return true;
        }
        if (to == WideKind::F16) {
            double_to_half(d, x);
            return true;
        }
        return unsupported();
    }
    case DxilCast::FPToSI:
    case DxilCast::FPToUI: {
        const bool sign = static_cast<DxilCast>(inst.sub) == DxilCast::FPToSI;
        if (to == WideKind::I64) {
            double_to_i64(sign, d, x);
            return true;
        }
        if (to_i32 || to == WideKind::I16) {
            const RegSlot r = h_.slot(inst.result);
            const Operand rd = scalar_dst(r), s = dsrc(x), rs = scalar_src(r);
            e_.inst(op(sign ? Sm4Opcode::DTOI : Sm4Opcode::DTOU), {&rd, &s});
            if (to == WideKind::I16) {
                // Saturated to 16 bits as dtoi / dtou saturate to 32 (the
                // RTX 4080: d3d12_dxilc_probe cs_double16 word 6).
                if (sign) {
                    const Operand lo16 = imm(static_cast<u32>(-32768)), hi16 = imm(32767);
                    bin(Sm4Opcode::IMAX, rd, rs, lo16);
                    bin(Sm4Opcode::IMIN, rd, rs, hi16);
                } else {
                    const Operand hi16 = imm(0xFFFFu);
                    bin(Sm4Opcode::UMIN, rd, rs, hi16);
                }
                mask16(r);
            }
            return true;
        }
        return unsupported();
    }
    case DxilCast::SIToFP:
    case DxilCast::UIToFP: {
        const bool sign = static_cast<DxilCast>(inst.sub) == DxilCast::SIToFP;
        if (to != WideKind::F64) return unsupported();
        if (from == WideKind::I64) {
            // hi * 2^32 + lo, both exact as doubles, rounded once by dfma.
            const RegSlot th = h_.fresh_pair(), tl = h_.fresh_pair();
            const Operand thd = pair_dst(th), tld = pair_dst(tl), h = hi(x), l = lo(x);
            e_.inst(op(sign ? Sm4Opcode::ITOD : Sm4Opcode::UTOD), {&thd, &h});
            e_.inst(op(Sm4Opcode::UTOD), {&tld, &l});
            const Operand ths = pair_src(th), tls = pair_src(tl), two32 = imm4(0, 0x41F00000u, 0, 0x41F00000u);
            e_.inst(op(Sm4Opcode::DFMA), {&d, &ths, &two32, &tls});
            return true;
        }
        Operand s;
        if (from_i1) {
            // i1 true is 1 unsigned, -1 signed.
            const RegSlot t = temp();
            const Operand td = scalar_dst(t), b = h_.src(x), one = imm(sign ? ~0u : 1u);
            bin(Sm4Opcode::AND, td, b, one);
            s = scalar_src(t);
        } else if (from == WideKind::I16) {
            s = sign ? sext16(x) : h_.src(x);
        } else if (from_i32) {
            s = h_.src(x);
        } else {
            return unsupported();
        }
        e_.inst(op(sign ? Sm4Opcode::ITOD : Sm4Opcode::UTOD), {&d, &s});
        return true;
    }
    case DxilCast::BitCast: {
        if (!((from == WideKind::I64 && to == WideKind::F64) || (from == WideKind::F64 && to == WideKind::I64) ||
              from == to)) {
            return unsupported();
        }
        const Operand s = dsrc(x);
        mov(d, s);
        return true;
    }
    default: return unsupported();
    }
}

bool Wide::call_f64(u32 opcode, const DxilInstruction& inst) {
    const auto arg = [&](usize k) { return inst.operands[1 + k]; };
    const auto unsupported = [&]() {
        const DxilFunction* callee = m_.function_of(inst.operands[0]);
        return h_.unimplemented("dx.op " + std::to_string(opcode) + " (" + callee->name + ")");
    };
    switch (opcode) {
    case 6: {  // FAbs
        const Operand d = h_.dst(inst.result), a = dsrc(arg(1), 2);
        e_.inst(op(Sm4Opcode::DMOV), {&d, &a});
        return true;
    }
    case 7: {  // Saturate: dmov_sat (NaN -> 0, D3D11.3 22.14.10)
        const Operand d = h_.dst(inst.result), a = dsrc(arg(1));
        e_.inst(op(Sm4Opcode::DMOV) | kSaturate, {&d, &a});
        return true;
    }
    case 35: case 36: {  // FMax / FMin (a NaN operand yields the other)
        const Operand d = h_.dst(inst.result), a = dsrc(arg(1)), b = dsrc(arg(2));
        e_.inst(op(opcode == 35 ? Sm4Opcode::DMAX : Sm4Opcode::DMIN), {&d, &a, &b});
        return true;
    }
    case 46: case 47: {  // FMad (fusing allowed) / Fma (fused, DXIL.rst)
        const Operand d = h_.dst(inst.result), a = dsrc(arg(1)), b = dsrc(arg(2)), c = dsrc(arg(3));
        e_.inst(op(Sm4Opcode::DFMA), {&d, &a, &b, &c});
        return true;
    }
    case 101: {  // MakeDouble(lo, hi)
        const Operand dl = lo_dst(inst.result), dh = hi_dst(inst.result), l = h_.src(arg(1)), hh = h_.src(arg(2));
        mov(dl, l);
        mov(dh, hh);
        return true;
    }
    case 102: {  // SplitDouble -> dx.types.splitdouble { lo, hi }
        const RegSlot p = h_.fresh_pair();
        const Operand pd = pair_dst(p), s = dsrc(arg(1));
        mov(pd, s);
        h_.ext().aggregates[inst.result] = {RegSlot{p.reg, p.comp}, RegSlot{p.reg, p.comp + 1}};
        return true;
    }
    case 128: case 129: {  // BitcastI64toF64 / BitcastF64toI64
        const Operand d = h_.dst(inst.result), s = dsrc(arg(1));
        mov(d, s);
        return true;
    }
    case 132: case 133: case 134: {  // LegacyDoubleToFloat / SInt32 / UInt32
        const RegSlot r = h_.slot(inst.result);
        const Operand d = scalar_dst(r), s = dsrc(arg(1));
        e_.inst(op(opcode == 132 ? Sm4Opcode::DTOF : opcode == 133 ? Sm4Opcode::DTOI : Sm4Opcode::DTOU), {&d, &s});
        return true;
    }
    default: return unsupported();
    }
}

// ---- dx.op ----------------------------------------------------------------------

u32 Wide::output_component_type(u32 element, u32* start_row, u32* start_col) const {
    const DxilNamedMetadata* eps = m_.named("dx.entryPoints");
    if (eps == nullptr || eps->nodes.empty()) return 0;
    const DxilMetadata* ep = m_.md(eps->nodes[0]);
    if (ep == nullptr || ep->operands.size() < 3) return 0;
    const DxilMetadata* sigs = m_.md(ep->operands[2]);
    if (sigs == nullptr || sigs->operands.size() < 2) return 0;
    const DxilMetadata* outs = m_.md(sigs->operands[1]);
    if (outs == nullptr) return 0;
    for (i64 rec_index : outs->operands) {
        const DxilMetadata* rec = m_.md(rec_index);
        if (rec == nullptr || rec->operands.size() < 3) continue;
        u64 id = 0, type = 0;
        m_.md_int(rec->operands[0], id);
        m_.md_int(rec->operands[2], type);
        if (id != element) continue;
        if (start_row != nullptr && rec->operands.size() >= 10) {
            u64 row = 0, col = 0;
            m_.md_int(rec->operands[8], row);
            m_.md_int(rec->operands[9], col);
            *start_row = static_cast<i32>(static_cast<u32>(row)) < 0 ? ~0u : static_cast<u32>(row);
            *start_col = static_cast<u32>(col);
        }
        return static_cast<u32>(type);
    }
    return 0;
}

bool Wide::narrow_aggregate(u32 id, WideKind k, u32 components) {
    u32 r = 0;
    if (!h_.vector_of(id, r)) return true;  // no result kept
    for (u32 c = 0; c < components; ++c) {
        const RegSlot s{r, c};
        if (k == WideKind::F16) round_half(s);
        else if (k == WideKind::I16) mask16(s);
    }
    return true;
}

bool Wide::raw_load(u32 opcode, const DxilInstruction& inst) {
    const auto arg = [&](usize k) { return inst.operands[1 + k]; };
    ResourceView v;
    if (!h_.resource_of(arg(1), v)) return false;
    const DxilType& rt = m_.types[inst.type];
    if (rt.kind != DxilTypeKind::Struct || rt.members.empty()) return h_.malformed("raw load without a ResRet");
    const WideKind k = kind_of_type(rt.members[0]);
    u32 mask = 0xF, alignment = 0;
    if (opcode == 139) {
        if (!h_.const_u32(arg(4), mask)) return h_.malformed("non-constant raw buffer mask");
        mask &= 0xFu;
        (void)h_.const_u32(arg(5), alignment);
    }
    if (v.shape != ResourceShape::RawBuffer && v.shape != ResourceShape::StructuredBuffer) {
        return h_.unimplemented("a 16- or 64-bit load from a typed buffer through BufferLoad");
    }
    const bool structured = v.shape == ResourceShape::StructuredBuffer;
    const Sm4OperandType t = v.cls == ResourceClass::UAV ? Sm4OperandType::UnorderedAccessView : Sm4OperandType::Resource;
    u32 count = 0;
    for (u32 c = 0; c < 4; ++c) {
        if (mask & (1u << c)) count = c + 1;
    }
    // The byte address (raw) or the byte offset (structured) of the load.
    const Operand index = h_.src(arg(2));
    const Operand offset = structured ? (h_.undef(arg(3)) ? imm(0) : h_.src(arg(3))) : imm(0);
    const auto address_plus = [&](const Operand& base, u32 add) {
        if (add == 0) return base;
        const RegSlot s = temp();
        const Operand d = scalar_dst(s), a = imm(add);
        e_.inst(op(Sm4Opcode::IADD), {&d, &base, &a});
        return scalar_src(s);
    };
    // Loads dwords [first, first + n) (n <= 4) to vector register r.
    const auto load_dwords = [&](u32 r, u32 first, u32 n, const Operand& byte_base) {
        const Operand d = dst_temp(r, (1u << n) - 1u);
        const Operand res = resource(t, v.range_id, v.reg, v.nonuniform, 0xE4);
        if (structured) {
            const Operand o = address_plus(byte_base, first * 4);
            e_.inst(op(Sm4Opcode::LD_STRUCTURED), {&d, &index, &o, &res});
        } else {
            const Operand a = address_plus(byte_base, first * 4);
            e_.inst(op(Sm4Opcode::LD_RAW), {&d, &a, &res});
        }
    };
    const Operand& byte_base = structured ? offset : index;
    std::vector<RegSlot> members;
    if (k == WideKind::I64 || k == WideKind::F64) {
        const u32 dwords = count * 2;
        for (u32 first = 0; first < dwords; first += 4) {
            const u32 r = h_.fresh_vector();
            load_dwords(r, first, std::min(4u, dwords - first), byte_base);
            for (u32 c = first; c < std::min(dwords, first + 4); c += 2) members.push_back(RegSlot{r, c - first});
        }
    } else if (k == WideKind::F16 || k == WideKind::I16) {
        for (u32 c = 0; c < count; ++c) {
            const RegSlot s = temp();
            const Operand d = scalar_dst(s), w = imm(16);
            const u32 r = h_.fresh_vector();
            if (alignment >= 4) {
                // Element c is half (c & 1) of dword c / 2.
                load_dwords(r, c / 2, 1, byte_base);
                const Operand x = src_temp_scalar(r, 0), o = imm(16 * (c & 1));
                e_.inst(op(Sm4Opcode::UBFE), {&d, &w, &o, &x});
            } else {
                // 2-byte aligned: the dword holding byte (base + 2c), and
                // the half of it the byte's bit 1 selects.
                const Operand at = address_plus(byte_base, 2 * c);
                const RegSlot dw = temp(), sh = temp();
                const Operand dwd = scalar_dst(dw), dws = scalar_src(dw), shd = scalar_dst(sh), shs = scalar_src(sh);
                const Operand not3 = imm(~3u), two = imm(2), three = imm(3);
                bin(Sm4Opcode::AND, dwd, at, not3);
                bin(Sm4Opcode::AND, shd, at, two);
                bin(Sm4Opcode::ISHL, shd, shs, three);
                const Operand dl = dst_temp(r, 1);
                const Operand res = resource(t, v.range_id, v.reg, v.nonuniform, 0xE4);
                if (structured) e_.inst(op(Sm4Opcode::LD_STRUCTURED), {&dl, &index, &dws, &res});
                else e_.inst(op(Sm4Opcode::LD_RAW), {&dl, &dws, &res});
                const Operand x = src_temp_scalar(r, 0);
                e_.inst(op(Sm4Opcode::UBFE), {&d, &w, &shs, &x});
            }
            if (k == WideKind::F16) {
                const Operand ss = scalar_src(s);
                f16op(F16Op::FromBits, d, ss);
            }
            members.push_back(s);
        }
    } else {
        return h_.unimplemented("raw load of " + dxil_type_name(m_, rt.members[0]));
    }
    // Members the mask leaves out read as zero; they are never extracted.
    while (members.size() < 4) members.push_back(members.empty() ? temp() : members.back());
    h_.ext().aggregates[inst.result] = members;
    return true;
}

bool Wide::raw_store(u32 opcode, const DxilInstruction& inst) {
    const auto arg = [&](usize k) { return inst.operands[1 + k]; };
    ResourceView v;
    if (!h_.resource_of(arg(1), v)) return false;
    if (v.shape != ResourceShape::RawBuffer && v.shape != ResourceShape::StructuredBuffer) {
        return h_.unimplemented("a 16- or 64-bit store to a typed buffer through BufferStore");
    }
    const bool structured = v.shape == ResourceShape::StructuredBuffer;
    u32 mask = 0, alignment = 0;
    if (!h_.const_u32(arg(8), mask)) return h_.malformed("non-constant store mask");
    mask &= 0xFu;
    if (opcode == 140) (void)h_.const_u32(arg(9), alignment);
    u32 count = 0;
    for (u32 c = 0; c < 4; ++c) {
        if (mask & (1u << c)) count = c + 1;
    }
    const u32 value0 = arg(4);
    const WideKind k = kind(value0);
    const Operand index = h_.src(arg(2));
    const Operand offset = structured ? (h_.undef(arg(3)) ? imm(0) : h_.src(arg(3))) : imm(0);
    const Operand& byte_base = structured ? offset : index;
    const auto address_plus = [&](const Operand& base, u32 add) {
        if (add == 0) return base;
        const RegSlot s = temp();
        const Operand d = scalar_dst(s), a = imm(add);
        e_.inst(op(Sm4Opcode::IADD), {&d, &base, &a});
        return scalar_src(s);
    };
    const auto uav = [&](u32 write_mask) {
        return resource(Sm4OperandType::UnorderedAccessView, v.range_id, v.reg, v.nonuniform, ~0u, write_mask);
    };
    const auto store_dwords = [&](u32 r, u32 first, u32 write_mask) {
        const Operand u = uav(write_mask);
        const Operand val = src_temp(r, 0xE4);
        const Operand a = address_plus(byte_base, first * 4);
        if (structured) e_.inst(op(Sm4Opcode::STORE_STRUCTURED), {&u, &index, &a, &val});
        else e_.inst(op(Sm4Opcode::STORE_RAW), {&u, &a, &val});
    };
    if (k == WideKind::I64 || k == WideKind::F64) {
        for (u32 first = 0; first < count * 2; first += 4) {
            const u32 r = h_.fresh_vector();
            u32 write = 0;
            for (u32 c = first / 2; c < std::min(count, first / 2 + 2); ++c) {
                if ((mask & (1u << c)) == 0) continue;
                const u32 at = 2 * c - first;
                const Operand dl = dst_temp(r, 1u << at), dh = dst_temp(r, 1u << (at + 1));
                const Operand l = lo(arg(4 + c)), hgh = hi(arg(4 + c));
                mov(dl, l);
                mov(dh, hgh);
                write |= 3u << at;
            }
            if (write != 0) store_dwords(r, first, write);
        }
        return true;
    }
    if (k != WideKind::F16 && k != WideKind::I16) {
        return h_.unimplemented("raw store of " + dxil_type_name(m_, m_.type_of(fn_, value0)));
    }
    // The 16 bits of element c.
    const auto bits = [&](u32 c) {
        const RegSlot s = temp();
        const Operand d = scalar_dst(s), x = h_.src(arg(4 + c));
        if (k == WideKind::F16) f16op(F16Op::ToBits, d, x);
        else mov(d, x);
        return s;
    };
    for (u32 c = 0; c < count; ++c) {
        if ((mask & (1u << c)) == 0) continue;
        if (alignment >= 4 && (c & 1u) == 0 && c + 1 < count && (mask & (2u << c)) != 0) {
            // A whole dword: elements c and c + 1.
            const RegSlot l = bits(c), hs = bits(c + 1);
            const u32 r = h_.fresh_vector();
            const Operand d = dst_temp(r, 1), w = imm(16), o = imm(16), ins = scalar_src(hs), base = scalar_src(l);
            e_.inst(op(Sm4Opcode::BFI), {&d, &w, &o, &ins, &base});
            store_dwords(r, c / 2, 1);
            ++c;
            continue;
        }
        // Half a dword: the other half may belong to another store, so the
        // word is changed with two atomics, clearing this half and setting
        // its bits.
        const RegSlot b = bits(c);
        const RegSlot dw = temp(), sh = temp(), keep = temp(), set = temp();
        const Operand dwd = scalar_dst(dw), dws = scalar_src(dw), shd = scalar_dst(sh), shs = scalar_src(sh);
        if (alignment >= 4) {
            const Operand at = address_plus(byte_base, 4 * (c / 2));
            mov(dwd, at);
            const Operand s16 = imm(16 * (c & 1));
            mov(shd, s16);
        } else {
            const Operand at = address_plus(byte_base, 2 * c);
            const Operand not3 = imm(~3u), two = imm(2), three = imm(3);
            bin(Sm4Opcode::AND, dwd, at, not3);
            bin(Sm4Opcode::AND, shd, at, two);
            bin(Sm4Opcode::ISHL, shd, shs, three);
        }
        const Operand kd = scalar_dst(keep), ks = scalar_src(keep), sd = scalar_dst(set), ss = scalar_src(set);
        const Operand ffff = imm(0xFFFFu), bs = scalar_src(b);
        bin(Sm4Opcode::ISHL, kd, ffff, shs);
        e_.inst(op(Sm4Opcode::NOT), {&kd, &ks});
        bin(Sm4Opcode::ISHL, sd, bs, shs);
        const Operand u = uav(0xF);
        if (structured) {
            const u32 addr = h_.fresh_vector();
            const Operand ax = dst_temp(addr, 1), ay = dst_temp(addr, 2);
            mov(ax, index);
            mov(ay, dws);
            const Operand a = src_temp(addr, 0x54);
            e_.inst(op(Sm4Opcode::ATOMIC_AND), {&u, &a, &ks});
            e_.inst(op(Sm4Opcode::ATOMIC_OR), {&u, &a, &ss});
        } else {
            e_.inst(op(Sm4Opcode::ATOMIC_AND), {&u, &dws, &ks});
            e_.inst(op(Sm4Opcode::ATOMIC_OR), {&u, &dws, &ss});
        }
    }
    return true;
}

bool Wide::typed_store_signed16(const DxilInstruction& inst, const ResourceView& v, u32 first_coord, u32 coords,
                                u32 first_value) {
    const auto arg = [&](usize k) { return inst.operands[1 + k]; };
    u32 mask = 0;
    if (!h_.const_u32(arg(first_value + 4), mask)) return h_.malformed("non-constant store mask");
    const u32 c = h_.fresh_vector(), r = h_.fresh_vector();
    for (u32 k = 0; k < coords; ++k) {
        if (h_.undef(arg(first_coord + k))) continue;
        const Operand d = dst_temp(c, 1u << k), s = h_.src(arg(first_coord + k));
        mov(d, s);
    }
    for (u32 k = 0; k < 4; ++k) {
        if ((mask & (1u << k)) == 0) continue;
        const Operand d = dst_temp(r, 1u << k), w = imm(16), o = imm(0), s = h_.src(arg(first_value + k));
        e_.inst(op(Sm4Opcode::IBFE), {&d, &w, &o, &s});
    }
    const Operand u = resource(Sm4OperandType::UnorderedAccessView, v.range_id, v.reg, v.nonuniform, ~0u, 0xF);
    const Operand cs = src_temp(c, 0xE4), vs = src_temp(r, 0xE4);
    e_.inst(op(Sm4Opcode::STORE_UAV_TYPED), {&u, &cs, &vs});
    return true;
}

bool Wide::cbuffer_load(const DxilInstruction& inst) {
    const auto arg = [&](usize k) { return inst.operands[1 + k]; };
    ResourceView v;
    if (!h_.resource_of(arg(1), v)) return false;
    const DxilType& rt = m_.types[inst.type];
    if (rt.kind != DxilTypeKind::Struct || rt.members.empty()) return h_.malformed("cbuffer load without a CBufRet");
    const WideKind k = kind_of_type(rt.members[0]);
    const u32 r = h_.fresh_vector();
    {
        const Operand d = dst_temp(r, 0xF);
        Index row;
        u32 constant_row = 0;
        if (h_.const_u32(arg(2), constant_row)) {
            row = Index{false, constant_row, 0, 0};
        } else {
            const RegSlot s = h_.slot(arg(2));
            row = Index{true, 0, s.reg, s.comp};
        }
        const Operand cb = cbuffer(v.range_id, v.reg, row, v.nonuniform, 0xE4);
        mov(d, cb);
    }
    std::vector<RegSlot> members;
    if (k == WideKind::I64 || k == WideKind::F64) {
        members = {RegSlot{r, 0}, RegSlot{r, 2}};
    } else if (k == WideKind::F16 || k == WideKind::I16) {
        // CBufRet.f16.8 / .i16.8: element c is half (c & 1) of dword c / 2.
        for (u32 c = 0; c < rt.members.size(); ++c) {
            const RegSlot s = temp();
            const Operand d = scalar_dst(s), w = imm(16), o = imm(16 * (c & 1)), x = src_temp_scalar(r, c / 2);
            e_.inst(op(Sm4Opcode::UBFE), {&d, &w, &o, &x});
            if (k == WideKind::F16) {
                const Operand ss = scalar_src(s);
                f16op(F16Op::FromBits, d, ss);
            }
            members.push_back(s);
        }
    } else {
        return h_.unimplemented("cbuffer load of " + dxil_type_name(m_, rt.members[0]));
    }
    h_.ext().aggregates[inst.result] = members;
    return true;
}

bool Wide::call(u32 opcode, const DxilInstruction& inst) {
    const auto arg = [&](usize k) { return inst.operands[1 + k]; };
    const DxilFunction* callee = m_.function_of(inst.operands[0]);
    const std::string& name = callee->name;
    const std::string overload = name.substr(name.rfind('.') + 1);
    const WideKind ok = overload == "i64" ? WideKind::I64
                        : overload == "i16" ? WideKind::I16
                        : overload == "f16" ? WideKind::F16
                        : overload == "f64" ? WideKind::F64
                                            : WideKind::None;
    const WideKind result_kind = inst.result != ~0u ? kind_of_type(inst.type) : WideKind::None;
    const auto unsupported = [&]() {
        return h_.unimplemented("dx.op " + std::to_string(opcode) + " (" + name + ")");
    };
    if (opcode == 101 || opcode == 102 || opcode == 128 || opcode == 129 || (opcode >= 132 && opcode <= 134) ||
        (ok == WideKind::F64 && opcode != 59 && opcode != 68 && opcode != 69 && opcode != 139 && opcode != 140)) {
        return call_f64(opcode, inst);
    }
    // Unary / binary integer forms.
    if (ok == WideKind::I64) {
        const Operand d = h_.dst(inst.result);
        switch (opcode) {
        case 30: { const Operand a = h_.src(arg(1)); i64op(I64Op::BitReverse, d, {&a}); return true; }
        case 31: { const Operand a = h_.src(arg(1)); i64op(I64Op::CountBits, d, {&a}); return true; }
        case 32: { const Operand a = h_.src(arg(1)); i64op(I64Op::FirstBitLo, d, {&a}); return true; }
        case 33: { const Operand a = h_.src(arg(1)); i64op(I64Op::FirstBitHi, d, {&a}); return true; }
        case 34: { const Operand a = h_.src(arg(1)); i64op(I64Op::FirstBitSHi, d, {&a}); return true; }
        case 37: case 38: case 39: case 40: {
            const Operand a = h_.src(arg(1)), b = h_.src(arg(2));
            const I64Op o = opcode == 37 ? I64Op::SMax : opcode == 38 ? I64Op::SMin : opcode == 39 ? I64Op::UMax : I64Op::UMin;
            i64op(o, d, {&a, &b});
            return true;
        }
        case 48: case 49: {
            const Operand a = h_.src(arg(1)), b = h_.src(arg(2)), c = h_.src(arg(3));
            i64op(I64Op::Mad, d, {&a, &b, &c});
            return true;
        }
        case 78: case 79:
            return h_.unimplemented("64-bit atomics (SM 6.6 Int64 atomics are not supported)");
        default: break;
        }
    }
    if (ok == WideKind::I16) {
        switch (opcode) {
        case 30: {  // Bfrev: the 16 bits reversed
            const RegSlot r = h_.slot(inst.result);
            const Operand d = scalar_dst(r), a = h_.src(arg(1)), rs = scalar_src(r), sh = imm(16);
            e_.inst(op(Sm4Opcode::BFREV), {&d, &a});
            bin(Sm4Opcode::USHR, d, rs, sh);
            return true;
        }
        case 31: case 32: return h_.emit_call_core(inst);  // on the zero-extended value
        case 33: case 34: {
            // Counted from bit 15: the 32-bit count less 16, ~0 kept.
            const RegSlot r = h_.slot(inst.result);
            const Operand d = scalar_dst(r), rs = scalar_src(r);
            const Operand a = opcode == 34 ? sext16(arg(1)) : h_.src(arg(1));
            e_.inst(op(opcode == 33 ? Sm4Opcode::FIRSTBIT_HI : Sm4Opcode::FIRSTBIT_SHI), {&d, &a});
            const RegSlot t = temp();
            const Operand td = scalar_dst(t), ts = scalar_src(t), m16 = imm(static_cast<u32>(-16)), none = imm(~0u);
            e_.inst(op(Sm4Opcode::IADD), {&td, &rs, &m16});
            bin(Sm4Opcode::IEQ, d, rs, none);
            const Operand cond = scalar_src(r);
            e_.inst(op(Sm4Opcode::MOVC), {&d, &cond, &none, &ts});
            return true;
        }
        case 37: case 38: {
            const RegSlot r = h_.slot(inst.result);
            const Operand d = scalar_dst(r), a = sext16(arg(1)), b = sext16(arg(2));
            bin(opcode == 37 ? Sm4Opcode::IMAX : Sm4Opcode::IMIN, d, a, b);
            mask16(r);
            return true;
        }
        case 39: case 40: return h_.emit_call_core(inst);
        case 48: case 49: {
            if (!h_.emit_call_core(inst)) return false;
            mask16(h_.slot(inst.result));
            return true;
        }
        default: break;
        }
    }
    switch (opcode) {
    case 219: case 220:
        // The byte operations preserve their native16 operand/result width
        // in their internal control word; no float/half lowering is needed.
        return h_.emit_call_core(inst);
    case 124: {  // BitcastI16toF16
        const Operand d = h_.dst(inst.result), a = h_.src(arg(1));
        f16op(F16Op::FromBits, d, a);
        return true;
    }
    case 125: {  // BitcastF16toI16
        const Operand d = h_.dst(inst.result), a = h_.src(arg(1));
        f16op(F16Op::ToBits, d, a);
        return true;
    }
    case 139: return raw_load(opcode, inst);
    case 140: return raw_store(opcode, inst);
    case 59: return cbuffer_load(inst);
    case 68: {
        ResourceView v;
        if (!h_.resource_of(arg(1), v)) return false;
        if (v.shape != ResourceShape::TypedBuffer) return raw_load(opcode, inst);
        if (ok == WideKind::I64) return unsupported();
        if (!h_.emit_call_core(inst)) return false;
        return narrow_aggregate(inst.result, ok, 4);
    }
    case 69: {
        ResourceView v;
        if (!h_.resource_of(arg(1), v)) return false;
        if (v.shape != ResourceShape::TypedBuffer) return raw_store(opcode, inst);
        if (ok == WideKind::I64) return unsupported();
        if (ok == WideKind::I16 && v.element_type == static_cast<u32>(ComponentType::I16)) {
            return typed_store_signed16(inst, v, 2, 1, 4);
        }
        return h_.emit_call_core(inst);
    }
    case 60: case 61: case 62: case 63: case 64: case 65: case 66: {
        if (ok == WideKind::I64) return unsupported();
        if (!h_.emit_call_core(inst)) return false;
        return narrow_aggregate(inst.result, ok, opcode == 64 || opcode == 65 ? 1u : 4u);
    }
    case 67: {
        if (ok == WideKind::I64) return unsupported();
        ResourceView v;
        if (!h_.resource_of(arg(1), v)) return false;
        if (ok == WideKind::I16 && v.element_type == static_cast<u32>(ComponentType::I16)) {
            return typed_store_signed16(inst, v, 2, 3, 5);
        }
        return h_.emit_call_core(inst);
    }
    case 4: {  // LoadInput
        if (!h_.emit_call_core(inst)) return false;
        const RegSlot r = h_.slot(inst.result);
        if (ok == WideKind::F16) round_half(r);
        else if (ok == WideKind::I16) mask16(r);
        else return unsupported();
        return true;
    }
    case 5: {  // StoreOutput
        if (ok == WideKind::F16) return h_.emit_call_core(inst);
        if (ok != WideKind::I16) return unsupported();
        u32 element = 0;
        if (!h_.const_u32(arg(1), element)) return h_.malformed("non-constant storeOutput element");
        // A signed element (DXIL ComponentType I16 = 2, cs_res16) carries
        // its value sign-extended, as an int register does.
        u32 start_row = 0, start_col = 0;
        if (output_component_type(element, &start_row, &start_col) != 2) return h_.emit_call_core(inst);
        u32 row = 0, col = 0;
        if (!h_.const_u32(arg(2), row) || !h_.const_u32(arg(3), col)) {
            return h_.unimplemented("a dynamically indexed output row");
        }
        if (start_row == ~0u) return h_.unimplemented("an output element with no register");
        const u32 comp = start_col + col;
        Operand o;
        // o#[start_row + row].comp, a 4-component mask operand.
        o.tokens.push_back(2u | ((1u << comp) << 4) | (static_cast<u32>(Sm4OperandType::Output) << 12) | (1u << 20));
        o.tokens.push_back(start_row + row);
        const Operand w = imm(16), zero = imm(0), x = h_.src(arg(4));
        e_.inst(op(Sm4Opcode::IBFE), {&o, &w, &zero, &x});
        return true;
    }
    default: break;
    }
    // Float math on halves: the float32 path, then the result rounded.
    if (ok == WideKind::F16) {
        if (!h_.emit_call_core(inst)) return false;
        if (result_kind == WideKind::F16) round_half(h_.slot(inst.result));
        return true;
    }
    return unsupported();
}

}  // namespace

WideKind wide_kind(const DxilModule& m, u32 type) {
    if (type >= m.types.size()) return WideKind::None;
    const DxilType& t = m.types[type];
    switch (t.kind) {
    case DxilTypeKind::Half: return WideKind::F16;
    case DxilTypeKind::Double: return WideKind::F64;
    case DxilTypeKind::Integer:
        if (t.width == 16) return WideKind::I16;
        if (t.width == 64) return WideKind::I64;
        if (t.width == 8) return WideKind::Other;
        return WideKind::None;
    default: return WideKind::None;
    }
}

u32 half_bits_to_float_bits(u32 bits) {
    const u32 sign = (bits & 0x8000u) << 16;
    const u32 exponent = (bits >> 10) & 0x1Fu;
    u32 mantissa = bits & 0x3FFu;
    if (exponent == 0x1F) return sign | 0x7F800000u | (mantissa << 13);
    if (exponent != 0) return sign | ((exponent + 112u) << 23) | (mantissa << 13);
    if (mantissa == 0) return sign;
    // A denormal: normalise it (every half is a normal float32).
    u32 e = 113;
    while ((mantissa & 0x400u) == 0) {
        mantissa <<= 1;
        --e;
    }
    return sign | (e << 23) | ((mantissa & 0x3FFu) << 13);
}

bool wants_wide_instruction(LowerHost& host, const DxilInstruction& inst) {
    const DxilModule& m = host.module();
    const DxilFunction* fn = host.function();
    const auto k = [&](u32 type) { return wide_kind(m, type); };
    const auto is = [](WideKind w) {
        return w == WideKind::I16 || w == WideKind::F16 || w == WideKind::I64 || w == WideKind::F64;
    };
    switch (inst.op) {
    case DxilOpcode::Binop: return is(k(inst.type));
    case DxilOpcode::Cmp: {
        const WideKind w = k(m.type_of(fn, inst.operands[0]));
        return w == WideKind::I16 || w == WideKind::I64 || w == WideKind::F64;
    }
    case DxilOpcode::Cast: return is(k(inst.type)) || is(k(m.type_of(fn, inst.operands[0])));
    case DxilOpcode::Load: return k(inst.type) == WideKind::I64 || k(inst.type) == WideKind::F64;
    case DxilOpcode::Store: {
        const WideKind w = k(m.type_of(fn, inst.operands[1]));
        return w == WideKind::I64 || w == WideKind::F64;
    }
    case DxilOpcode::ExtractValue: return host.ext().aggregates.contains(inst.operands[0]);
    default: return false;
    }
}

bool lower_wide_instruction(LowerHost& host, const DxilInstruction& inst) {
    Wide w(host);
    return w.instruction(inst);
}

bool wants_wide_call(LowerHost& host, const DxilFunction& callee, const DxilInstruction& inst) {
    const std::string& name = callee.name;
    // GatherRaw's 16- and 64-bit overloads are gathers of a uint view
    // (Lowerer::emit_gather), whatever the element width.
    if (name.rfind("dx.op.textureGatherRaw.", 0) == 0) return false;
    const std::string overload = name.substr(name.rfind('.') + 1);
    if (overload == "i64" || overload == "i16" || overload == "f16" || overload == "f64") return true;
    const DxilModule& m = host.module();
    // Double arguments (the legacy double conversions have no overload).
    for (usize k = 1; k < inst.operands.size(); ++k) {
        if (wide_kind(m, m.type_of(host.function(), inst.operands[k])) == WideKind::F64) return true;
    }
    if (inst.result == ~0u) return false;
    const DxilType& t = m.types[inst.type];
    if (t.kind == DxilTypeKind::Struct) {
        for (u32 member : t.members) {
            const WideKind w = wide_kind(m, member);
            if (w == WideKind::I16 || w == WideKind::F16 || w == WideKind::I64 || w == WideKind::F64) return true;
        }
        return false;
    }
    const WideKind w = wide_kind(m, inst.type);
    return w == WideKind::I16 || w == WideKind::F16 || w == WideKind::I64 || w == WideKind::F64;
}

bool lower_wide_call(LowerHost& host, u32 opcode, const DxilInstruction& inst) {
    Wide w(host);
    return w.call(opcode, inst);
}

}  // namespace vitreo::graphics::dxil_lower
