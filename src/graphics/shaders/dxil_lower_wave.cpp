// SPDX-License-Identifier: LGPL-2.1-only
/// @file dxil_lower_wave.cpp
/// @brief DXIL wave and quad intrinsics -> the VX_WAVE extension
/// instruction (dxbc_vitreo_ext.hpp).
///
/// Opcodes and operand order: DXIL.rst's operation table (110
/// WaveIsFirstLane .. 123 QuadOp, 135 WaveAllBitCount, 136
/// WavePrefixBitCount, 165 WaveMatch, 166 WaveMultiPrefixOp, 167
/// WaveMultiPrefixBitCount). The enum operands were read off dxc 1.6
/// listings of tests/oracle_data/d3d12_sm6x (cs_wavex, cs_wave16) and
/// d3d12_sm6 (cs_wave):
///  - WaveOpKind (WaveActiveOp / WavePrefixOp): 0 Sum, 1 Product, 2 Min,
///    3 Max; SignedOpKind: 0 signed, 1 unsigned (WaveActiveMin(int) is
///    (2, 0), WaveActiveMax(uint) (3, 1));
///  - WaveBitOpKind (WaveActiveBit): 0 And, 1 Or, 2 Xor;
///  - WaveMultiPrefixOpKind: 0 Sum, 1 And, 2 Or, 3 Xor, 4 Product;
///  - QuadOpKind: 0 ReadAcrossX, 1 ReadAcrossY, 2 ReadAcrossDiagonal.

#include "dxbc_vitreo_ext.hpp"
#include "dxil_lower_ext.hpp"

namespace vitreo::graphics::dxil_lower {

using namespace dxbc_vitreo_ext;

void emit_ext(Emitter& e, u32 opcode, u32 control, std::initializer_list<const Operand*> ops) {
    const Operand c = control_operand(control);
    std::vector<const Operand*> all{&c};
    all.insert(all.end(), ops.begin(), ops.end());
    // Emitter::inst takes an initializer list; build the token stream here.
    usize length = 1;
    for (const Operand* op : all) length += op->tokens.size();
    e.raw(opcode | (static_cast<u32>(length) << 24));
    for (const Operand* op : all) {
        for (u32 t : op->tokens) e.raw(t);
    }
}

Operand control_operand(u32 control) { return imm4(control, control, control, control); }

Operand pair_dst(RegSlot s) { return dst_temp(s.reg, 3u << s.comp); }
Operand pair_src(RegSlot s) { return src_temp(s.reg, s.comp == 0 ? 0x44u : 0xEEu); }
Operand scalar_dst(RegSlot s) { return dst_temp(s.reg, 1u << s.comp); }
Operand scalar_src(RegSlot s) { return src_temp_scalar(s.reg, s.comp); }

bool is_wave_opcode(u32 opcode) {
    return (opcode >= 110 && opcode <= 123) || opcode == 135 || opcode == 136 ||
           (opcode >= 165 && opcode <= 167) || opcode == 221 || opcode == 222;
}

namespace {

/// The wave value type of @p type with signedness @p sop (1 unsigned).
/// @p moves_only: the operation only moves the value between lanes
/// (ReadLaneAt / ReadFirst / quad reads), so a double moves as its 64 bits.
bool wave_type(LowerHost& host, u32 type, bool unsigned_op, WaveType& out, bool moves_only = false) {
    const DxilModule& m = host.module();
    const DxilType& t = m.types[type];
    if (t.kind == DxilTypeKind::Double && moves_only) { out = WaveType::U64; return true; }
    if (t.kind == DxilTypeKind::Float) { out = WaveType::F32; return true; }
    if (t.kind == DxilTypeKind::Half) { out = WaveType::F16; return true; }
    if (t.kind == DxilTypeKind::Integer) {
        switch (t.width) {
        case 1:
        case 32: out = unsigned_op ? WaveType::U32 : WaveType::I32; return true;
        case 16: out = unsigned_op ? WaveType::U16 : WaveType::I16; return true;
        case 64: out = unsigned_op ? WaveType::U64 : WaveType::I64; return true;
        default: break;
        }
    }
    return host.unimplemented("wave operation on " + dxil_type_name(m, type));
}

/// One VX_WAVE instruction with no op (moves, ballots, queries).
void wave_raw(Emitter& e, WaveKind kind, WaveType type, const Operand& dst, std::initializer_list<const Operand*> srcs) {
    const Operand c = control_operand(wave_control(kind, WaveOp::None, type));
    usize length = 1 + c.tokens.size() + dst.tokens.size();
    for (const Operand* x : srcs) length += x->tokens.size();
    e.raw(kOpWave | (static_cast<u32>(length) << 24));
    for (u32 t : c.tokens) e.raw(t);
    for (u32 t : dst.tokens) e.raw(t);
    for (const Operand* x : srcs) {
        for (u32 t : x->tokens) e.raw(t);
    }
}

/// WaveActiveOp / WavePrefixOp on doubles (Sum, Product, Min, Max): a loop
/// over the wave's lanes in lane order. Each active lane's value is read
/// with a 64-bit ReadLaneAt under a branch that every lane takes alike (the
/// ballot and the counter are uniform), so the shuffle never reads an
/// inactive lane; a prefix keeps only the lanes below its own. The
/// accumulator starts at the operation's identity (+0, 1, +inf, -inf).
bool wave_double_reduce(LowerHost& host, bool prefix, u32 op, u32 value, const Operand& d) {
    Emitter& e = host.body();
    const auto o = [](Sm4Opcode x) { return static_cast<u32>(x); };
    const auto wave = [&](WaveKind kind, WaveType type, const Operand& dst, std::initializer_list<const Operand*> srcs) {
        wave_raw(e, kind, type, dst, srcs);
    };
    const u32 ballot = host.fresh_vector();
    const RegSlot lane = host.fresh_scalar(), count = host.fresh_scalar(), me = host.fresh_scalar();
    const RegSlot t = host.fresh_scalar(), bit = host.fresh_scalar();
    const RegSlot acc = host.fresh_pair(), v = host.fresh_pair(), r = host.fresh_pair();
    {
        const Operand bd = dst_temp(ballot, 0xF), all = imm(~0u);
        wave(WaveKind::Ballot, WaveType::U32, bd, {&all});
        const Operand cd = scalar_dst(count), md = scalar_dst(me);
        wave(WaveKind::LaneCount, WaveType::U32, cd, {});
        wave(WaveKind::LaneIndex, WaveType::U32, md, {});
        const Operand ld = scalar_dst(lane), zero = imm(0);
        e.inst(o(Sm4Opcode::MOV), {&ld, &zero});
        // +0, 1.0, +inf, -inf.
        static constexpr u32 kIdentityHi[] = {0u, 0x3FF00000u, 0x7FF00000u, 0xFFF00000u};
        const Operand ad = pair_dst(acc), id = imm4(0, kIdentityHi[op], 0, kIdentityHi[op]);
        e.inst(o(Sm4Opcode::MOV), {&ad, &id});
    }
    const Operand ls = scalar_src(lane), cs = scalar_src(count), ts = scalar_src(t), td = scalar_dst(t);
    const Operand bs = scalar_src(bit), bdst = scalar_dst(bit);
    e.inst(o(Sm4Opcode::LOOP), {});
    e.inst(o(Sm4Opcode::UGE), {&td, &ls, &cs});
    e.inst(o(Sm4Opcode::BREAKC) | kTestNonZero, {&ts});
    {
        // The lane's ballot bit: word lane / 32 (lanes up to 128).
        const Operand five = imm(5), k31 = imm(31), one = imm(1), two = imm(2), three = imm(3);
        e.inst(o(Sm4Opcode::USHR), {&td, &ls, &five});
        const Operand bx = src_temp_scalar(ballot, 0), by = src_temp_scalar(ballot, 1);
        const Operand bz = src_temp_scalar(ballot, 2), bw = src_temp_scalar(ballot, 3);
        const RegSlot sel = host.fresh_scalar();
        const Operand sd = scalar_dst(sel), ss = scalar_src(sel);
        e.inst(o(Sm4Opcode::IEQ), {&sd, &ts, &one});
        e.inst(o(Sm4Opcode::MOVC), {&bdst, &ss, &by, &bx});
        e.inst(o(Sm4Opcode::IEQ), {&sd, &ts, &two});
        e.inst(o(Sm4Opcode::MOVC), {&bdst, &ss, &bz, &bs});
        e.inst(o(Sm4Opcode::IEQ), {&sd, &ts, &three});
        e.inst(o(Sm4Opcode::MOVC), {&bdst, &ss, &bw, &bs});
        e.inst(o(Sm4Opcode::AND), {&td, &ls, &k31});
        e.inst(o(Sm4Opcode::USHR), {&bdst, &bs, &ts});
        e.inst(o(Sm4Opcode::AND), {&bdst, &bs, &one});
    }
    e.inst(o(Sm4Opcode::IF) | kTestNonZero, {&bs});
    {
        const Operand vd = pair_dst(v), x = host.src(value);
        wave(WaveKind::ReadLaneAt, WaveType::U64, vd, {&x, &ls});
        const Operand as = pair_src(acc), vs = pair_src(v), rd = pair_dst(r), rs = pair_src(r), ad = pair_dst(acc);
        static constexpr Sm4Opcode kOps[] = {Sm4Opcode::DADD, Sm4Opcode::DMUL, Sm4Opcode::DMIN, Sm4Opcode::DMAX};
        e.inst(o(kOps[op]), {&rd, &as, &vs});
        if (prefix) {
            const Operand mes = scalar_src(me);
            e.inst(o(Sm4Opcode::ULT), {&td, &ls, &mes});
            e.inst(o(Sm4Opcode::MOVC), {&ad, &ts, &rs, &as});
        } else {
            e.inst(o(Sm4Opcode::MOV), {&ad, &rs});
        }
    }
    e.inst(o(Sm4Opcode::ENDIF), {});
    {
        const Operand ld = scalar_dst(lane), one = imm(1);
        e.inst(o(Sm4Opcode::IADD), {&ld, &ls, &one});
    }
    e.inst(o(Sm4Opcode::ENDLOOP), {});
    const Operand as = pair_src(acc);
    e.inst(o(Sm4Opcode::MOV), {&d, &as});
    return true;
}

}  // namespace

bool lower_wave_call(LowerHost& host, u32 opcode, const DxilInstruction& inst) {
    const DxilModule& m = host.module();
    const DxilFunction* fn = host.function();
    Emitter& body = host.body();
    const auto arg = [&](usize k) { return inst.operands[1 + k]; };
    const auto i8 = [&](usize k, u32& out) {
        if (!host.const_u32(arg(k), out)) return host.malformed("non-constant wave operation kind");
        out &= 0xFFu;
        return true;
    };
    const auto value_type = [&](bool unsigned_op, WaveType& out, bool moves_only = false) {
        return wave_type(host, m.type_of(fn, arg(1)), unsigned_op, out, moves_only);
    };
    // dst for a scalar / pair result, or the four components of an
    // aggregate (ballot, match).
    const auto emit = [&](WaveKind kind, WaveOp op, WaveType type, std::initializer_list<const Operand*> srcs) {
        const bool aggregate = kind == WaveKind::Ballot || kind == WaveKind::Match;
        const Operand d = aggregate ? dst_temp(host.vector(inst.result), 0xF) : host.dst(inst.result);
        std::vector<const Operand*> ops{&d};
        ops.insert(ops.end(), srcs.begin(), srcs.end());
        const u32 helpers = fn->waveops_include_helper_lanes ? kWaveIncludeHelpers : 0u;
        const Operand c = control_operand(wave_control(kind, op, type) | helpers);
        usize length = 1 + c.tokens.size();
        for (const Operand* o : ops) length += o->tokens.size();
        body.raw(kOpWave | (static_cast<u32>(length) << 24));
        for (u32 t : c.tokens) body.raw(t);
        for (const Operand* o : ops) {
            for (u32 t : o->tokens) body.raw(t);
        }
        return true;
    };
    const auto mask_vector = [&](usize first) {
        const u32 r = host.fresh_vector();
        for (u32 k = 0; k < 4; ++k) {
            const Operand d = dst_temp(r, 1u << k);
            const Operand s = host.src(arg(first + k));
            body.inst(static_cast<u32>(Sm4Opcode::MOV), {&d, &s});
        }
        return src_temp(r, 0xE4);
    };
    WaveType type = WaveType::U32;
    u32 op = 0, sop = 0;
    switch (opcode) {
    case 110: return emit(WaveKind::IsFirstLane, WaveOp::None, WaveType::U32, {});
    case 221: return emit(WaveKind::IsHelperLane, WaveOp::None, WaveType::U32, {});  // IsHelperLane (SM 6.6)
    case 222: {
        // QuadVote(cond, op): QuadVoteOpKind Any = 0, All = 1 (dxc 1.6
        // listing d3d12_dxilc/ps_helper). Every lane of the quad, helpers
        // included, as the quad reads are.
        if (!i8(2, op) || op > 1) return op > 1 ? host.malformed("unknown QuadVoteOpKind") : false;
        const Operand v = host.src(arg(1));
        RegSlot t[3];
        for (u32 k = 0; k < 3; ++k) {
            t[k] = host.fresh_scalar();
            const Operand d = scalar_dst(t[k]);
            const WaveKind kind = k == 0 ? WaveKind::QuadAcrossX : k == 1 ? WaveKind::QuadAcrossY : WaveKind::QuadDiagonal;
            const Operand c = control_operand(wave_control(kind, WaveOp::None, WaveType::U32));
            usize length = 1 + c.tokens.size() + d.tokens.size() + v.tokens.size();
            body.raw(kOpWave | (static_cast<u32>(length) << 24));
            for (const Operand* o : {&c, &d, &v}) {
                for (u32 tok : o->tokens) body.raw(tok);
            }
        }
        const Operand d = host.dst(inst.result);
        const u32 join = static_cast<u32>(op == 0 ? Sm4Opcode::OR : Sm4Opcode::AND);
        const Operand a0 = scalar_src(t[0]), a1 = scalar_src(t[1]), a2 = scalar_src(t[2]);
        body.inst(join, {&d, &v, &a0});
        const Operand r = host.src(inst.result);
        body.inst(join, {&d, &r, &a1});
        body.inst(join, {&d, &r, &a2});
        return true;
    }
    case 111: return emit(WaveKind::LaneIndex, WaveOp::None, WaveType::U32, {});
    case 112: return emit(WaveKind::LaneCount, WaveOp::None, WaveType::U32, {});
    case 113:
    case 114:
    case 116:
    case 135:
    case 136: {
        const Operand v = host.src(arg(1));
        const WaveKind kind = opcode == 113   ? WaveKind::AnyTrue
                              : opcode == 114 ? WaveKind::AllTrue
                              : opcode == 116 ? WaveKind::Ballot
                              : opcode == 135 ? WaveKind::ActiveCountBits
                                              : WaveKind::PrefixCountBits;
        return emit(kind, WaveOp::None, WaveType::U32, {&v});
    }
    case 115:
    case 118:
    case 165: {
        if (opcode == 115 && m.types[m.type_of(fn, arg(1))].kind == DxilTypeKind::Double) {
            // AllEqual on doubles compares as doubles (-0 == +0, a NaN is
            // never equal): every active lane's value == the first lane's.
            const RegSlot p = host.fresh_pair(), c = host.fresh_scalar();
            const Operand pd = pair_dst(p), ps = pair_src(p), x = host.src(arg(1));
            wave_raw(body, WaveKind::ReadFirst, WaveType::U64, pd, {&x});
            const Operand cd = scalar_dst(c), cs = scalar_src(c);
            body.inst(static_cast<u32>(Sm4Opcode::DEQ), {&cd, &x, &ps});
            return emit(WaveKind::AllTrue, WaveOp::None, WaveType::U32, {&cs});
        }
        if (!value_type(true, type, opcode != 115)) return false;
        const Operand v = host.src(arg(1));
        const WaveKind kind = opcode == 115 ? WaveKind::AllEqual : opcode == 118 ? WaveKind::ReadFirst : WaveKind::Match;
        return emit(kind, WaveOp::None, type, {&v});
    }
    case 117:
    case 122: {
        if (!value_type(true, type, true)) return false;
        const Operand v = host.src(arg(1));
        const Operand lane = host.src(arg(2));
        return emit(opcode == 117 ? WaveKind::ReadLaneAt : WaveKind::QuadReadLaneAt, WaveOp::None, type, {&v, &lane});
    }
    case 123: {
        if (!i8(2, op) || op > 2) return op > 2 ? host.malformed("unknown QuadOpKind") : false;
        if (!value_type(true, type, true)) return false;
        const Operand v = host.src(arg(1));
        const WaveKind kind = op == 0 ? WaveKind::QuadAcrossX : op == 1 ? WaveKind::QuadAcrossY : WaveKind::QuadDiagonal;
        return emit(kind, WaveOp::None, type, {&v});
    }
    case 119:
    case 121: {
        if (!i8(2, op) || !i8(3, sop)) return false;
        if (op > 3 || sop > 1) return host.malformed("unknown WaveOpKind / SignedOpKind");
        if (m.types[m.type_of(fn, arg(1))].kind == DxilTypeKind::Double) {
            const Operand d = host.dst(inst.result);
            return wave_double_reduce(host, opcode == 121, op, arg(1), d);
        }
        if (!value_type(sop == 1, type)) return false;
        static constexpr WaveOp kOps[] = {WaveOp::Sum, WaveOp::Product, WaveOp::Min, WaveOp::Max};
        const Operand v = host.src(arg(1));
        return emit(opcode == 119 ? WaveKind::Active : WaveKind::Prefix, kOps[op], type, {&v});
    }
    case 120: {
        if (!i8(2, op)) return false;
        if (op > 2) return host.malformed("unknown WaveBitOpKind");
        if (!value_type(true, type)) return false;
        static constexpr WaveOp kOps[] = {WaveOp::And, WaveOp::Or, WaveOp::Xor};
        const Operand v = host.src(arg(1));
        return emit(WaveKind::Active, kOps[op], type, {&v});
    }
    case 166: {
        if (!i8(6, op) || !i8(7, sop)) return false;
        if (op > 4 || sop > 1) return host.malformed("unknown WaveMultiPrefixOpKind / SignedOpKind");
        if (!value_type(sop == 1, type)) return false;
        static constexpr WaveOp kOps[] = {WaveOp::Sum, WaveOp::And, WaveOp::Or, WaveOp::Xor, WaveOp::Product};
        const Operand v = host.src(arg(1));
        const Operand mask = mask_vector(2);
        return emit(WaveKind::MultiPrefix, kOps[op], type, {&v, &mask});
    }
    case 167: {
        const Operand v = host.src(arg(1));
        const Operand mask = mask_vector(2);
        return emit(WaveKind::MultiPrefixCountBits, WaveOp::None, WaveType::U32, {&v, &mask});
    }
    default:
        return host.unimplemented("dx.op " + std::to_string(opcode));
    }
}

}  // namespace vitreo::graphics::dxil_lower
