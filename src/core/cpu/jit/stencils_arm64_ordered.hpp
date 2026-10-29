// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64

#pragma once

/// @file stencils_arm64_ordered.hpp
/// @brief ARM64 x86-TSO ordered accesses (LDAR/LDAPR/STLR) and FEAT_LSE atomics.
///
/// Part of the arm64:: encoder set; include core/cpu/jit/stencils.hpp.

#include <core/cpu/jit/stencils_arm64_fields.hpp>
#include <vitreo/common.hpp>

namespace vitreo::cpu::jit::arm64 {

/// Encode ARM64 LDAR Xt, [Xn] (64-bit acquire load).
constexpr u32 ldar_x(u8 t, u8 n) noexcept {
    return 0xC8DFFC00u | (static_cast<u32>(n) << 5)
                       | static_cast<u32>(t);
}
static_assert(ldar_x(3, 4) == 0xC8DFFC83u);

// -----------------------------------------------------------------------------
// x86-TSO ordered guest accesses
// -----------------------------------------------------------------------------
//
// x86 guarantees Total Store Order; ARM64 does not. A guest load or store
// translated to a plain LDR/STR can be reordered in ways no x86 core would
// permit, which silently corrupts lock-free guest code — .NET volatile
// semantics, Boehm GC handshakes, double-checked locking.
//
// These encodings carry the ordering INSIDE the access instruction rather
// than in a surrounding DMB: one instruction instead of two, and they order
// only what TSO requires instead of draining the whole memory pipeline.
//
//   LDAR  — acquire load, RCsc (Armv8.0, always available).
//   LDAPR — acquire load, RCpc (FEAT_LRCPC). Cheaper than LDAR, and RCpc is
//           the weaker guarantee that actually matches x86, so prefer it
//           wherever the hardware reports the feature.
//   STLR  — release store.
//
// x86-TSO permits store->load reordering (that is the store buffer), so full
// sequential consistency is NOT required; the emit_guest_* policy decides
// which form each access gets.
//
// Every constant below was verified against the system assembler rather than
// derived by hand.

/// Encode ARM64 LDARB Wt, [Xn] (8-bit acquire load).
constexpr u32 ldar_b(u8 t, u8 n) noexcept {
    return 0x08DFFC00u | (static_cast<u32>(n) << 5)
                       | static_cast<u32>(t);
}
static_assert(ldar_b(3, 4) == 0x08DFFC83u);

/// Encode ARM64 LDARH Wt, [Xn] (16-bit acquire load).
constexpr u32 ldar_h(u8 t, u8 n) noexcept {
    return 0x48DFFC00u | (static_cast<u32>(n) << 5)
                       | static_cast<u32>(t);
}
static_assert(ldar_h(3, 4) == 0x48DFFC83u);

/// Encode ARM64 LDAR Wt, [Xn] (32-bit acquire load).
constexpr u32 ldar_w(u8 t, u8 n) noexcept {
    return 0x88DFFC00u | (static_cast<u32>(n) << 5)
                       | static_cast<u32>(t);
}
static_assert(ldar_w(3, 4) == 0x88DFFC83u);
static_assert(ldar_w(0, 0) == 0x88DFFC00u);

/// Encode ARM64 STLRB Wt, [Xn] (8-bit release store).
constexpr u32 stlr_b(u8 t, u8 n) noexcept {
    return 0x089FFC00u | (static_cast<u32>(n) << 5)
                       | static_cast<u32>(t);
}
static_assert(stlr_b(3, 4) == 0x089FFC83u);

/// Encode ARM64 STLRH Wt, [Xn] (16-bit release store).
constexpr u32 stlr_h(u8 t, u8 n) noexcept {
    return 0x489FFC00u | (static_cast<u32>(n) << 5)
                       | static_cast<u32>(t);
}
static_assert(stlr_h(3, 4) == 0x489FFC83u);

/// Encode ARM64 STLR Wt, [Xn] (32-bit release store).
constexpr u32 stlr_w(u8 t, u8 n) noexcept {
    return 0x889FFC00u | (static_cast<u32>(n) << 5)
                       | static_cast<u32>(t);
}
static_assert(stlr_w(3, 4) == 0x889FFC83u);
static_assert(stlr_w(0, 0) == 0x889FFC00u);

/// Encode ARM64 STLR Xt, [Xn] (64-bit release store).
constexpr u32 stlr_x(u8 t, u8 n) noexcept {
    return 0xC89FFC00u | (static_cast<u32>(n) << 5)
                       | static_cast<u32>(t);
}
static_assert(stlr_x(3, 4) == 0xC89FFC83u);

/// FEAT_LSE atomic read-modify-write with acquire+release (the "AL" forms):
/// Rt := old [Xn]; [Xn] := old OP Rs, as one single-copy-atomic access.
/// `size` is log2 of the width in bytes (0 = B, 1 = H, 2 = W, 3 = X) and
/// `op` the o3:opc field (0 ADD, 1 CLR, 2 EOR, 3 SET, 8 SWP). Words checked
/// below against `llvm-mc -triple=aarch64 -mattr=+lse -show-encoding`.
enum class LseOp : u32 { Add = 0x0, Clr = 0x1, Eor = 0x2, Set = 0x3, Swp = 0x8 };
constexpr u32 lse_al(LseOp op, u64 size, u8 s, u8 t, u8 n) noexcept {
    return 0x38E00000u | (uimm_field("lse_al", size, 3) << 30) | (static_cast<u32>(op) << 12) |
           (static_cast<u32>(s) << 16) | (static_cast<u32>(n) << 5) |
           static_cast<u32>(t);
}
static_assert(lse_al(LseOp::Add, 2, 1, 2, 3) == 0xB8E10062u);  // ldaddal w1,w2,[x3]
static_assert(lse_al(LseOp::Add, 3, 1, 2, 3) == 0xF8E10062u);  // ldaddal x1,x2,[x3]
static_assert(lse_al(LseOp::Add, 0, 1, 2, 3) == 0x38E10062u);  // ldaddalb
static_assert(lse_al(LseOp::Add, 1, 1, 2, 3) == 0x78E10062u);  // ldaddalh
static_assert(lse_al(LseOp::Swp, 2, 1, 2, 3) == 0xB8E18062u);  // swpal w
static_assert(lse_al(LseOp::Swp, 3, 1, 2, 3) == 0xF8E18062u);  // swpal x
static_assert(lse_al(LseOp::Clr, 2, 1, 2, 3) == 0xB8E11062u);  // ldclral w
static_assert(lse_al(LseOp::Clr, 3, 1, 2, 3) == 0xF8E11062u);  // ldclral x
static_assert(lse_al(LseOp::Set, 2, 1, 2, 3) == 0xB8E13062u);  // ldsetal w
static_assert(lse_al(LseOp::Set, 3, 1, 2, 3) == 0xF8E13062u);  // ldsetal x
static_assert(lse_al(LseOp::Eor, 2, 1, 2, 3) == 0xB8E12062u);  // ldeoral w
static_assert(lse_al(LseOp::Eor, 3, 1, 2, 3) == 0xF8E12062u);  // ldeoral x
static_assert(lse_al(LseOp::Set, 0, 1, 2, 3) == 0x38E13062u);  // ldsetalb
static_assert(lse_al(LseOp::Eor, 1, 1, 2, 3) == 0x78E12062u);  // ldeoralh

/// FEAT_LSE CASAL Rs, Rt, [Xn]: if [Xn] == Rs then [Xn] := Rt; Rs := old.
/// Acquire+release; permission-checked as a store even when the compare
/// fails, which is x86's LOCK CMPXCHG write cycle too.
constexpr u32 casal(u64 size, u8 s, u8 t, u8 n) noexcept {
    return 0x08E0FC00u | (uimm_field("casal", size, 3) << 30) | (static_cast<u32>(s) << 16) |
           (static_cast<u32>(n) << 5) | static_cast<u32>(t);
}
static_assert(casal(2, 1, 2, 3) == 0x88E1FC62u);  // casal w1,w2,[x3]
static_assert(casal(3, 1, 2, 3) == 0xC8E1FC62u);  // casal x1,x2,[x3]
static_assert(casal(0, 1, 2, 3) == 0x08E1FC62u);  // casalb
static_assert(casal(1, 1, 2, 3) == 0x48E1FC62u);  // casalh

/// Encode ARM64 LDAPRB Wt, [Xn] (8-bit RCpc acquire load, FEAT_LRCPC).
constexpr u32 ldapr_b(u8 t, u8 n) noexcept {
    return 0x38BFC000u | (static_cast<u32>(n) << 5)
                       | static_cast<u32>(t);
}
static_assert(ldapr_b(3, 4) == 0x38BFC083u);

/// Encode ARM64 LDAPRH Wt, [Xn] (16-bit RCpc acquire load, FEAT_LRCPC).
constexpr u32 ldapr_h(u8 t, u8 n) noexcept {
    return 0x78BFC000u | (static_cast<u32>(n) << 5)
                       | static_cast<u32>(t);
}
static_assert(ldapr_h(3, 4) == 0x78BFC083u);

/// Encode ARM64 LDAPR Wt, [Xn] (32-bit RCpc acquire load, FEAT_LRCPC).
constexpr u32 ldapr_w(u8 t, u8 n) noexcept {
    return 0xB8BFC000u | (static_cast<u32>(n) << 5)
                       | static_cast<u32>(t);
}
static_assert(ldapr_w(3, 4) == 0xB8BFC083u);
static_assert(ldapr_w(0, 0) == 0xB8BFC000u);

/// Encode ARM64 LDAPR Xt, [Xn] (64-bit RCpc acquire load, FEAT_LRCPC).
constexpr u32 ldapr_x(u8 t, u8 n) noexcept {
    return 0xF8BFC000u | (static_cast<u32>(n) << 5)
                       | static_cast<u32>(t);
}
static_assert(ldapr_x(3, 4) == 0xF8BFC083u);

} // namespace vitreo::cpu::jit::arm64
