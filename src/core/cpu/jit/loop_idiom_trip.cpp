// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64

#include "core/cpu/jit/loop_idiom_trip.hpp"

namespace vitreo::cpu::jit::loop_idiom {

namespace {

constexpr u64 mask_of(u32 width) noexcept {
    return width >= 64 ? ~u64{0} : (u64{1} << width) - 1;
}

/// Inverse of an odd number mod 2^64 (Newton: each step doubles the bits).
constexpr u64 inverse_odd(u64 u) noexcept {
    u64 x = u;  // correct to 3 bits
    for (int i = 0; i < 5; ++i) x *= 2 - u * x;
    return x;
}

bool is_signed_cond(Cond c) noexcept {
    return c == Cond::L || c == Cond::GE || c == Cond::LE || c == Cond::G;
}

}  // namespace

bool eval_cont(Cond cont, u64 lhs, u64 rhs, u32 width) noexcept {
    const u64 m = mask_of(width);
    lhs &= m;
    rhs &= m;
    const u64 sign = u64{1} << (width - 1);
    const bool eq = lhs == rhs;
    const bool below = lhs < rhs;
    const bool less = (lhs ^ sign) < (rhs ^ sign);
    switch (cont) {
    case Cond::Z: return eq;
    case Cond::NZ: return !eq;
    case Cond::B: return below;
    case Cond::AE: return !below;
    case Cond::BE: return below || eq;
    case Cond::A: return !below && !eq;
    case Cond::L: return less;
    case Cond::GE: return !less;
    case Cond::LE: return less || eq;
    case Cond::G: return !less && !eq;
    }
    return false;
}

TripResult first_induction_exit(Cond cont, bool ind_is_lhs, u64 v1, i64 step,
                                u32 width, u64 bound, u64 limit) noexcept {
    using K = TripResult::Kind;
    const u64 m = mask_of(width);
    v1 &= m;
    bound &= m;
    if (limit == 0) return {K::NoExitWithin, 0};
    const u64 s = static_cast<u64>(step) & m;
    if (s == 0) {
        // Not produced by the matcher (|step| in 1..16); stay correct anyway.
        const bool c = ind_is_lhs ? eval_cont(cont, v1, bound, width)
                                  : eval_cont(cont, bound, v1, width);
        return c ? TripResult{K::NoExitWithin, limit} : TripResult{K::Exit, 1};
    }

    if (cont == Cond::NZ) {
        // Leaves at the first k with v_k == bound: (k-1)*s == bound - v1.
        const u64 d = (bound - v1) & m;
        const u32 t = static_cast<u32>(__builtin_ctzll(s));
        if (t >= width || (d & ((u64{1} << t) - 1)) != 0) {
            return {K::NoExitWithin, limit};
        }
        const u64 mod_mask = mask_of(width - t);
        const u64 j = ((d >> t) * inverse_odd(s >> t)) & mod_mask;
        if (j < limit) return {K::Exit, j + 1};
        return {K::NoExitWithin, limit};
    }
    if (cont == Cond::Z) {
        if (v1 != bound) return {K::Exit, 1};
        // v_2 = v1 + s != v1 since s != 0 (mod 2^width).
        if (limit >= 2) return {K::Exit, 2};
        return {K::NoExitWithin, 1};
    }

    // Ordered: map to an unsigned order (signed conditions bias by the sign
    // bit), find the no-wrap prefix, then binary-search the monotone predicate.
    const u64 bias = is_signed_cond(cont) ? (u64{1} << (width - 1)) : 0;
    const u64 b1 = (v1 ^ bias) & m;
    const u64 mag = step > 0 ? static_cast<u64>(step) : static_cast<u64>(-step);
    const u64 room = step > 0 ? (m - b1) : b1;  // steps before wrapping
    // Iterations 1..k_max do not wrap (saturating: room/mag + 1 overflows
    // when a 64-bit value starts at the end of its order).
    const u64 steps_room = room / mag;
    const u64 k_max = steps_room == ~u64{0} ? steps_room : steps_room + 1;
    const u64 L = k_max < limit ? k_max : limit;

    auto cont_at = [&](u64 k) {
        const u64 v = (v1 + (k - 1) * s) & m;
        return ind_is_lhs ? eval_cont(cont, v, bound, width)
                          : eval_cont(cont, bound, v, width);
    };
    if (!cont_at(1)) return {K::Exit, 1};
    if (cont_at(L)) return {K::NoExitWithin, L};
    // cont_at(lo) true, cont_at(hi) false.
    u64 lo = 1, hi = L;
    while (hi - lo > 1) {
        const u64 mid = lo + (hi - lo) / 2;
        if (cont_at(mid)) lo = mid;
        else hi = mid;
    }
    return {K::Exit, hi};
}

}  // namespace vitreo::cpu::jit::loop_idiom
