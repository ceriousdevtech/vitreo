// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
//
// WS2 loop-idiom recognition: the descriptor shared by the matcher (analysis
// library) and the bulk helper (runtime library).
//
// A Desc says, in guest terms only, what one iteration of a self-looping
// block does to memory and to its induction registers, and when it leaves:
//
//   * up to two load streams and one store stream, each an affine address
//     base + (index << scale) + disp_eff whose value in iteration 1 is taken
//     from the registers at block entry (disp_eff already folds in the
//     induction updates that precede the access in body order);
//   * up to three induction registers, each updated exactly once per
//     iteration by a constant step;
//   * at most one induction exit (a predicate on an induction value against
//     an invariant register or immediate) and at most two data exits (a
//     predicate on the elements loaded in that iteration).
//
// It is packed into six guest-only u64 words that the JIT materialises as
// immediates in X2..X7. Nothing in it is a host pointer, so a block carrying
// the hook remains relocatable for the translation store.
//
// Header-only on purpose: the runtime library must not depend on the analysis
// library (src/CMakeLists.txt JIT layering).

#pragma once

#include "vitreo/types.hpp"

#include <array>
#include <optional>

namespace vitreo::cpu::jit::loop_idiom {

enum class Kind : u8 { Fill, Copy, Scan, Compare };

/// The condition under which the loop CONTINUES past an exit point, in the
/// Intel SDM Vol. 2 Jcc vocabulary on `CMP lhs, rhs`:
/// Z: lhs == rhs, NZ: lhs != rhs, B: lhs <u rhs, AE: lhs >=u rhs,
/// BE: lhs <=u rhs, A: lhs >u rhs, L: lhs <s rhs, GE: lhs >=s rhs,
/// LE: lhs <=s rhs, G: lhs >s rhs.
enum class Cond : u8 { Z, NZ, B, AE, BE, A, L, GE, LE, G };
inline constexpr u8 kCondCount = 10;

enum class ValueSrc : u8 { None, Imm, Gpr, Xmm };

/// "No register" in a stream's base/index.
inline constexpr u8 kNoReg = 16;

struct Stream {
    u8 base{kNoReg};
    u8 index{kNoReg};
    u8 scale_log2{0};
    i32 disp_eff{0};
    /// Legacy-SSE MOVDQA/MOVAPS/MOVNT* or a legacy PCMPEQx memory source:
    /// a misaligned element raises #GP (Intel SDM Vol. 2, Exception Type 1/4).
    bool align16{false};
    constexpr bool operator==(const Stream&) const = default;
};

struct Induction {
    u8 reg{0};
    i8 step{0};   ///< |step| <= 16, never 0
    bool wide{false};  ///< 64-bit update; otherwise a 32-bit write
    constexpr bool operator==(const Induction&) const = default;
};

/// v_k = entry(reg) + (phase + k - 1) * step at the compare width. An exit
/// on the update itself (DEC/SUB/INC/ADD + Jcc, or LOOP) is phase 1 against
/// an immediate 0 with Z/NZ only.
struct IndExit {
    u8 ind{0};
    Cond cont{Cond::NZ};
    ValueSrc bound{ValueSrc::Imm};  ///< Imm or Gpr
    u8 bound_reg{0};
    i32 bound_imm{0};               ///< sign-extended, truncated to the width
    bool ind_is_lhs{true};
    u8 phase{0};
    bool cmp64{false};              ///< compare width 64, else 32
    constexpr bool operator==(const IndExit&) const = default;
};

enum class DataOp : u8 {
    ValueEq,    ///< element (low cmp bytes) == value
    StreamEq,   ///< element of stream 0 == element of stream 1
    AnyLaneEq,  ///< some (1 << cmp_log2)-byte lane of the 16-byte element
                ///< equals the same lane of an invariant XMM register
};

struct DataExit {
    u8 stream{0};
    DataOp op{DataOp::ValueEq};
    bool exit_if_eq{true};  ///< the loop leaves when the predicate holds
    ValueSrc val{ValueSrc::Imm};
    u8 val_reg{0};
    bool val_high8{false};
    i32 val_imm{0};
    /// ValueEq: log2 of the compare width (a MOVZX temp may be compared
    /// wider than the element). AnyLaneEq: log2 of the PCMPEQ lane width.
    u8 cmp_log2{0};
    constexpr bool operator==(const DataExit&) const = default;
};

/// Fill source.
struct Value {
    ValueSrc src{ValueSrc::None};
    u8 reg{0};
    bool high8{false};
    i32 imm{0};
    constexpr bool operator==(const Value&) const = default;
};

struct Desc {
    Kind kind{Kind::Fill};
    u8 elem_log2{0};       ///< element width 1..16 bytes
    bool is64{false};      ///< 64-bit addressing and register file
    bool ordered{false};   ///< the JIT's own lowering of these accesses is TSO-ordered
    bool vector{false};    ///< 16-byte elements
    bool dir_down{false};  ///< stream stride is -w (else +w)
    u8 n_loads{0};         ///< streams[0..n_loads) are loads
    bool has_store{false}; ///< streams[n_loads] is the store
    Stream streams[3]{};
    u8 n_ind{0};
    Induction ind[3]{};
    bool has_ind_exit{false};
    IndExit ind_exit{};
    u8 n_data{0};
    DataExit data[2]{};
    Value fill{};
    u64 loop_rip{0};       ///< < 2^48
    constexpr bool operator==(const Desc&) const = default;

    [[nodiscard]] constexpr u32 elem_bytes() const noexcept {
        return 1u << elem_log2;
    }
    [[nodiscard]] constexpr u8 n_streams() const noexcept {
        return static_cast<u8>(n_loads + (has_store ? 1 : 0));
    }
};

using Words = std::array<u64, 6>;

namespace detail {

struct BitWriter {
    Words w{};
    u32 pos{0};
    bool ok{true};
    constexpr void put(u64 v, u32 bits) noexcept {
        if (bits < 64 && (v >> bits) != 0) ok = false;
        for (u32 i = 0; i < bits; ++i, ++pos) {
            if (pos >= 384) { ok = false; return; }
            if ((v >> i) & 1u) w[pos / 64] |= u64{1} << (pos % 64);
        }
    }
    constexpr void put_signed(i64 v, u32 bits) noexcept {
        const i64 lo = -(i64{1} << (bits - 1));
        const i64 hi = (i64{1} << (bits - 1)) - 1;
        if (v < lo || v > hi) ok = false;
        put(static_cast<u64>(v) & ((u64{1} << bits) - 1), bits);
    }
    constexpr void put_bool(bool b) noexcept { put(b ? 1 : 0, 1); }
};

struct BitReader {
    const Words& w;
    u32 pos{0};
    // Word-at-a-time (at most two words per field): the helper unpacks on
    // every call, and a bit loop cost ~140 ns of it. Bits past 384 read 0.
    constexpr u64 get(u32 bits) noexcept {
        if (bits == 0) return 0;
        if (pos >= 384) { pos += bits; return 0; }
        const u32 word = pos / 64;
        const u32 shift = pos % 64;
        u64 v = w[word] >> shift;
        if (shift != 0 && shift + bits > 64 && word + 1 < 6) v |= w[word + 1] << (64 - shift);
        pos += bits;
        return bits >= 64 ? v : (v & ((u64{1} << bits) - 1));
    }
    constexpr i64 get_signed(u32 bits) noexcept {
        const u64 raw = get(bits);
        const u64 sign = u64{1} << (bits - 1);
        return static_cast<i64>((raw ^ sign) - sign);
    }
    constexpr bool get_bool() noexcept { return get(1) != 0; }
};

}  // namespace detail

/// Encode `d` into six words, or nullopt if any field is out of range.
[[nodiscard]] constexpr std::optional<Words> pack(const Desc& d) noexcept {
    detail::BitWriter b;
    if (d.elem_log2 > 4 || d.n_loads > 2 || d.n_streams() > 3 || d.n_ind > 3 ||
        d.n_data > 2 || d.loop_rip >= (u64{1} << 48)) {
        return std::nullopt;
    }
    if (d.kind == Kind::Fill && d.n_data != 0) return std::nullopt;
    b.put(static_cast<u64>(d.kind), 2);
    b.put(d.elem_log2, 3);
    b.put_bool(d.is64);
    b.put_bool(d.ordered);
    b.put_bool(d.vector);
    b.put_bool(d.dir_down);
    b.put(d.n_loads, 2);
    b.put_bool(d.has_store);
    for (const Stream& s : d.streams) {
        b.put(s.base, 5);
        b.put(s.index, 5);
        b.put(s.scale_log2, 2);
        b.put_signed(s.disp_eff, 32);
        b.put_bool(s.align16);
        if (s.base > kNoReg || s.index > kNoReg) b.ok = false;
    }
    b.put(d.n_ind, 2);
    for (const Induction& i : d.ind) {
        b.put(i.reg, 4);
        if (i.step < -16 || i.step > 16) b.ok = false;
        b.put_signed(i.step, 6);
        b.put_bool(i.wide);
    }
    b.put_bool(d.has_ind_exit);
    {
        const IndExit& e = d.ind_exit;
        b.put(e.ind, 2);
        b.put(static_cast<u64>(e.cont), 4);
        if (static_cast<u8>(e.cont) >= kCondCount) b.ok = false;
        b.put(static_cast<u64>(e.bound), 2);
        b.put(e.bound_reg, 4);
        b.put_signed(e.bound_imm, 32);
        b.put_bool(e.ind_is_lhs);
        b.put(e.phase, 1);
        b.put_bool(e.cmp64);
    }
    b.put(d.n_data, 2);
    for (u32 k = 0; k < 2; ++k) {
        if (k == 0 && d.kind == Kind::Fill) {
            // Fill has no data exits: its source shares data[0]'s bits.
            const Value& v = d.fill;
            b.put(0, 2);
            b.put(0, 2);
            b.put_bool(v.high8);
            b.put(static_cast<u64>(v.src), 2);
            b.put(v.reg, 4);
            b.put_bool(false);
            b.put_signed(v.imm, 32);
            b.put(0, 3);
            continue;
        }
        const DataExit& x = d.data[k];
        b.put(x.stream, 2);
        b.put(static_cast<u64>(x.op), 2);
        b.put_bool(x.exit_if_eq);
        b.put(static_cast<u64>(x.val), 2);
        b.put(x.val_reg, 4);
        b.put_bool(x.val_high8);
        b.put_signed(x.val_imm, 32);
        b.put(x.cmp_log2, 3);
        if (x.cmp_log2 > 4 || static_cast<u8>(x.op) > 2) b.ok = false;
    }
    b.put(d.loop_rip, 48);
    if (!b.ok) return std::nullopt;
    return b.w;
}

/// Decode six words, or nullopt on any out-of-range field.
[[nodiscard]] constexpr std::optional<Desc> unpack(const Words& w) noexcept {
    detail::BitReader r{w};
    Desc d;
    const u64 kind = r.get(2);
    d.kind = static_cast<Kind>(kind);
    d.elem_log2 = static_cast<u8>(r.get(3));
    d.is64 = r.get_bool();
    d.ordered = r.get_bool();
    d.vector = r.get_bool();
    d.dir_down = r.get_bool();
    d.n_loads = static_cast<u8>(r.get(2));
    d.has_store = r.get_bool();
    bool ok = d.elem_log2 <= 4 && d.n_loads <= 2 && d.n_streams() <= 3;
    for (Stream& s : d.streams) {
        s.base = static_cast<u8>(r.get(5));
        s.index = static_cast<u8>(r.get(5));
        s.scale_log2 = static_cast<u8>(r.get(2));
        s.disp_eff = static_cast<i32>(r.get_signed(32));
        s.align16 = r.get_bool();
        ok = ok && s.base <= kNoReg && s.index <= kNoReg;
    }
    d.n_ind = static_cast<u8>(r.get(2));
    ok = ok && d.n_ind <= 3;
    for (Induction& i : d.ind) {
        i.reg = static_cast<u8>(r.get(4));
        i.step = static_cast<i8>(r.get_signed(6));
        i.wide = r.get_bool();
        ok = ok && i.step >= -16 && i.step <= 16;
    }
    d.has_ind_exit = r.get_bool();
    {
        IndExit& e = d.ind_exit;
        e.ind = static_cast<u8>(r.get(2));
        const u64 c = r.get(4);
        ok = ok && c < kCondCount;
        e.cont = static_cast<Cond>(c);
        const u64 bs = r.get(2);
        e.bound = static_cast<ValueSrc>(bs);
        e.bound_reg = static_cast<u8>(r.get(4));
        e.bound_imm = static_cast<i32>(r.get_signed(32));
        e.ind_is_lhs = r.get_bool();
        e.phase = static_cast<u8>(r.get(1));
        e.cmp64 = r.get_bool();
    }
    d.n_data = static_cast<u8>(r.get(2));
    ok = ok && d.n_data <= 2;
    for (u32 k = 0; k < 2; ++k) {
        if (k == 0 && d.kind == Kind::Fill) {
            Value& v = d.fill;
            (void)r.get(2);
            (void)r.get(2);
            v.high8 = r.get_bool();
            v.src = static_cast<ValueSrc>(r.get(2));
            v.reg = static_cast<u8>(r.get(4));
            (void)r.get_bool();
            v.imm = static_cast<i32>(r.get_signed(32));
            (void)r.get(3);
            continue;
        }
        DataExit& x = d.data[k];
        x.stream = static_cast<u8>(r.get(2));
        const u64 op = r.get(2);
        ok = ok && op <= 2;
        x.op = static_cast<DataOp>(op);
        x.exit_if_eq = r.get_bool();
        x.val = static_cast<ValueSrc>(r.get(2));
        x.val_reg = static_cast<u8>(r.get(4));
        x.val_high8 = r.get_bool();
        x.val_imm = static_cast<i32>(r.get_signed(32));
        x.cmp_log2 = static_cast<u8>(r.get(3));
        ok = ok && x.cmp_log2 <= 4;
    }
    d.loop_rip = r.get(48);
    if (d.kind == Kind::Fill && d.n_data != 0) ok = false;
    // Every bit past the last field must be zero: a word built by anything
    // other than pack() is refused rather than half-trusted.
    if (r.pos < 384) {
        const u32 word = r.pos / 64;
        const u32 shift = r.pos % 64;
        if (shift != 0 && (w[word] >> shift) != 0) ok = false;
        for (u32 i = shift != 0 ? word + 1 : word; i < 6; ++i) {
            if (w[i] != 0) ok = false;
        }
    }
    if (!ok) return std::nullopt;
    return d;
}

/// The trip-count gate the JIT emits inline in front of the helper call, so
/// a loop that is going to run too few iterations to pay for the call never
/// makes it. Formed whenever the loop has an induction exit (its trip count
/// bounds the loop from above even when a data exit can leave earlier).
struct InlineGate {
    enum class Form : u8 {
        None,
        CountDown,   ///< remaining = reg            (dec/sub 1 + jnz, LOOP)
        NegIndexUp,  ///< remaining = -reg           (inc + jnz)
        UpToBound,   ///< remaining = (bound - reg) >> step_log2  (cmp ind, bound; jne/jb)
    };
    Form form{Form::None};
    u8 reg{0};
    u8 bound_reg{0};
    bool bound_is_imm{false};
    i32 bound_imm{0};
    u8 step_log2{0};
    bool wide{false};
    constexpr bool operator==(const InlineGate&) const = default;
};

}  // namespace vitreo::cpu::jit::loop_idiom
