// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64

#pragma once

/// @file stencils_arm64_simd_permute.hpp
/// @brief ARM64 NEON permute, lane-move, table, widen and narrow encoders.
///
/// Part of the arm64:: encoder set; include core/cpu/jit/stencils.hpp.

#include <core/cpu/jit/stencils_arm64_simd_ldst.hpp>
#include <vitreo/common.hpp>

namespace vitreo::cpu::jit::arm64 {

/// Encode ZIP1 Vd.T, Vn.T, Vm.T — interleave low halves.
constexpr u32 zip1_v(u8 d, u8 n, u8 m, VSize sz) noexcept {
    return 0x4E003800u | (uimm_field("zip1_v", sz, 3) << 22)
                       | (static_cast<u32>(m) << 16)
                       | (static_cast<u32>(n) << 5)
                       | static_cast<u32>(d);
}

/// Encode ZIP2 Vd.T, Vn.T, Vm.T — interleave high halves.
constexpr u32 zip2_v(u8 d, u8 n, u8 m, VSize sz) noexcept {
    return 0x4E007800u | (uimm_field("zip2_v", sz, 3) << 22)
                       | (static_cast<u32>(m) << 16)
                       | (static_cast<u32>(n) << 5)
                       | static_cast<u32>(d);
}

/// Encode SXTL Vd.<2T>, Vn.<T> — signed widen low 64 bits (shift=0 SSHLL).
/// dst_sz is the destination element size (H, S, or D).
constexpr u32 sxtl_v(u8 d, u8 n, VSize dst_sz) noexcept {
    // The destination is H, S or D (USHLL/SSHLL immh = 0001/001x/01xx).
    if (dst_sz == VS_B) simm_field_overflow("sxtl_v", VS_B, VS_H, VS_D);
    const u32 immh = 1u << (uimm_field("sxtl_v", dst_sz, VS_D) - 1u);
    return 0x0F00A400u | (immh << 19)
                      | (static_cast<u32>(n) << 5) | static_cast<u32>(d);
}

/// Encode UXTL Vd.<2T>, Vn.<T> — unsigned widen low 64 bits (shift=0 USHLL).
constexpr u32 uxtl_v(u8 d, u8 n, VSize dst_sz) noexcept {
    // The destination is H, S or D (USHLL/SSHLL immh = 0001/001x/01xx).
    if (dst_sz == VS_B) simm_field_overflow("uxtl_v", VS_B, VS_H, VS_D);
    const u32 immh = 1u << (uimm_field("uxtl_v", dst_sz, VS_D) - 1u);
    return 0x2F00A400u | (immh << 19)
                      | (static_cast<u32>(n) << 5) | static_cast<u32>(d);
}

/// Encode INS Vd.<T>[di], Vn.<T>[si] — element-from-element insert.
/// VSize selects B/H/S/D lane width; di,si are lane indices.
constexpr u32 ins_v_elem(u8 d, u64 di, u8 n, u64 si, VSize sz) noexcept {
    u32 imm5 = 0, imm4 = 0;
    const u64 last = (16u >> uimm_field("ins_v_elem", sz, 3)) - 1u;
    const u32 dl = uimm_field("ins_v_elem", di, last);
    const u32 sl = uimm_field("ins_v_elem", si, last);
    switch (sz) {
    case VS_B: imm5 = (dl << 1) | 0x01; imm4 = sl;        break;
    case VS_H: imm5 = (dl << 2) | 0x02; imm4 = sl << 1;   break;
    case VS_S: imm5 = (dl << 3) | 0x04; imm4 = sl << 2;   break;
    case VS_D: imm5 = (dl << 4) | 0x08; imm4 = sl << 3;   break;
    }
    return 0x6E000400u | (imm5 << 16) | (imm4 << 11)
                      | (static_cast<u32>(n) << 5) | static_cast<u32>(d);
}

/// Encode TBL Vd.16B, {Vn.16B}, Vm.16B — single-register table lookup.
/// Byte lanes of Vm in [0..15] select from Vn; lanes >=16 produce 0.
constexpr u32 tbl_v16b_1(u8 d, u8 n, u8 m) noexcept {
    return 0x4E000000u | (static_cast<u32>(m) << 16)
                      | (static_cast<u32>(n) << 5) | static_cast<u32>(d);
}

/// TBL Vd.16B, {Vn.16B, Vn+1.16B}, Vm.16B — two-register (32-byte) table, so
/// byte indices 0..31 address a full 256-bit value. `len` field = 01.
/// The table registers must be CONSECUTIVE and wrap at V31.
constexpr u32 tbl_v16b_2(u8 d, u8 n, u8 m) noexcept {
    return 0x4E002000u | (static_cast<u32>(m) << 16)
                      | (static_cast<u32>(n) << 5) | static_cast<u32>(d);
}

/// Encode DUP Vd.2D, Vn.D[0] — broadcast 64-bit lane 0 to both D lanes.
/// Exact match for x86 MOVDDUP reg-reg semantics.
/// DUP Vd.<T>, Vn.<Ts>[0] — splat lane 0 across all lanes (Q=1).
/// imm5 selects the element size: B=00001, H=00010, S=00100, D=01000.
constexpr u32 dup_v_elem0(u8 d, u8 n, VSize sz) noexcept {
    u32 imm5 = 0;
    switch (static_cast<VSize>(uimm_field("dup_v_elem0", sz, 3))) {
    case VS_B: imm5 = 0x01; break;
    case VS_H: imm5 = 0x02; break;
    case VS_S: imm5 = 0x04; break;
    case VS_D: imm5 = 0x08; break;
    }
    return 0x4E000400u | (imm5 << 16)
                       | (static_cast<u32>(n) << 5)
                       | static_cast<u32>(d);
}

/// DUP Vd.4S, Vn.S[idx] — splat one 32-bit lane across all four.
constexpr u32 dup_v_elem_s(u8 d, u8 n, u64 idx) noexcept {
    return 0x4E000400u | (((uimm_field("dup_v_elem_s", idx, 3) << 3) | 0x04u) << 16)
                       | (static_cast<u32>(n) << 5)
                       | static_cast<u32>(d);
}
// llvm-mc -triple=aarch64 -show-encoding.
static_assert(dup_v_elem_s(0, 1, 0) == 0x4E040420u);  // dup v0.4s, v1.s[0]
static_assert(dup_v_elem_s(0, 1, 1) == 0x4E0C0420u);  // dup v0.4s, v1.s[1]
static_assert(dup_v_elem_s(0, 1, 2) == 0x4E140420u);  // dup v0.4s, v1.s[2]
static_assert(dup_v_elem_s(18, 20, 3) == 0x4E1C0692u);  // dup v18.4s, v20.s[3]

constexpr u32 dup_v_d0_to_2d(u8 d, u8 n) noexcept {
    // DUP Vd.<T>, Vn.Ts[lane]: imm5 encoding: for D lane 0, imm5=01000.
    // Base: 0100 1110 000 imm5 000001 nnnnn ddddd = 0x4E000400 | (imm5<<16)
    return 0x4E080400u
         | (static_cast<u32>(n) << 5)
         | static_cast<u32>(d);
}

/// Encode TRN1 Vd.T, Vn.T, Vm.T — pick even-indexed lanes from each.
/// Result[2i]=n[2i]; Result[2i+1]=m[2i].
constexpr u32 trn1_v(u8 d, u8 n, u8 m, VSize sz) noexcept {
    return 0x4E002800u | (uimm_field("trn1_v", sz, 3) << 22)
                      | (static_cast<u32>(m) << 16)
                      | (static_cast<u32>(n) << 5)
                      | static_cast<u32>(d);
}

/// Encode TRN2 Vd.T, Vn.T, Vm.T — pick odd-indexed lanes from each.
/// Result[2i]=n[2i+1]; Result[2i+1]=m[2i+1].
constexpr u32 trn2_v(u8 d, u8 n, u8 m, VSize sz) noexcept {
    return 0x4E006800u | (uimm_field("trn2_v", sz, 3) << 22)
                      | (static_cast<u32>(m) << 16)
                      | (static_cast<u32>(n) << 5)
                      | static_cast<u32>(d);
}

/// Encode UZP1 Vd.T, Vn.T, Vm.T — even-indexed lanes from n then m.
/// Result: [n0, n2, ..., n(E-2), m0, m2, ..., m(E-2)] for E=elements.
constexpr u32 uzp1_v(u8 d, u8 n, u8 m, VSize sz) noexcept {
    return 0x4E001800u | (uimm_field("uzp1_v", sz, 3) << 22)
                      | (static_cast<u32>(m) << 16)
                      | (static_cast<u32>(n) << 5)
                      | static_cast<u32>(d);
}

/// Encode UZP2 Vd.T, Vn.T, Vm.T — odd-indexed lanes from n then m.
/// Result: [n1, n3, ..., n(E-1), m1, m3, ..., m(E-1)].
constexpr u32 uzp2_v(u8 d, u8 n, u8 m, VSize sz) noexcept {
    return 0x4E005800u | (uimm_field("uzp2_v", sz, 3) << 22)
                      | (static_cast<u32>(m) << 16)
                      | (static_cast<u32>(n) << 5)
                      | static_cast<u32>(d);
}

/// Encode SQXTN Vd.Tb, Vn.Ta — signed saturating narrow (write low 64 bits).
/// sz selects source element size (H/S/D → narrow to B/H/S).
constexpr u32 sqxtn_v(u8 d, u8 n, VSize src_sz) noexcept {
    // src_sz for narrow encoding uses the DEST element size field
    // which is (src_sz - 1). B=00, H=01, S=10 dest sizes are valid.
    // The source is H, S or D; the field holds the destination size.
    if (src_sz == VS_B) simm_field_overflow("sqxtn_v", VS_B, VS_H, VS_D);
    const u32 dst_sz = uimm_field("sqxtn_v", src_sz, VS_D) - 1u;
    return 0x0E214800u | (dst_sz << 22)
                      | (static_cast<u32>(n) << 5)
                      | static_cast<u32>(d);
}

/// Encode SQXTN2 Vd.Tb, Vn.Ta — same, writes high 64 bits (Q=1).
constexpr u32 sqxtn2_v(u8 d, u8 n, VSize src_sz) noexcept {
    // The source is H, S or D; the field holds the destination size.
    if (src_sz == VS_B) simm_field_overflow("sqxtn2_v", VS_B, VS_H, VS_D);
    const u32 dst_sz = uimm_field("sqxtn2_v", src_sz, VS_D) - 1u;
    return 0x4E214800u | (dst_sz << 22)
                      | (static_cast<u32>(n) << 5)
                      | static_cast<u32>(d);
}

/// Encode SQXTUN Vd.Tb, Vn.Ta — signed→unsigned saturating narrow (low 64).
constexpr u32 sqxtun_v(u8 d, u8 n, VSize src_sz) noexcept {
    // The source is H, S or D; the field holds the destination size.
    if (src_sz == VS_B) simm_field_overflow("sqxtun_v", VS_B, VS_H, VS_D);
    const u32 dst_sz = uimm_field("sqxtun_v", src_sz, VS_D) - 1u;
    return 0x2E212800u | (dst_sz << 22)
                      | (static_cast<u32>(n) << 5)
                      | static_cast<u32>(d);
}

/// Encode SQXTUN2 Vd.Tb, Vn.Ta — same, writes high 64 bits.
constexpr u32 sqxtun2_v(u8 d, u8 n, VSize src_sz) noexcept {
    // The source is H, S or D; the field holds the destination size.
    if (src_sz == VS_B) simm_field_overflow("sqxtun2_v", VS_B, VS_H, VS_D);
    const u32 dst_sz = uimm_field("sqxtun2_v", src_sz, VS_D) - 1u;
    return 0x6E212800u | (dst_sz << 22)
                      | (static_cast<u32>(n) << 5)
                      | static_cast<u32>(d);
}

/// DUP Vd.T, Rn — broadcast GPR to Q=1 vector (16B/8H/4S/2D).
constexpr u32 dup_v_gpr(u8 d, u8 n, VSize sz) noexcept {
    const u32 imm5 = 1u << uimm_field("dup_v_gpr", sz, 3);
    return 0x4E000C00u | (imm5 << 16)
                      | (static_cast<u32>(n) << 5) | static_cast<u32>(d);
}

/// XTN Vd.Tb, Vn.Ta — narrow, take low half of each source element (Q=0).
/// dst_sz is the destination element size (B/H/S).
constexpr u32 xtn_v(u8 d, u8 n, VSize dst_sz) noexcept {
    return 0x0E212800u | (uimm_field("xtn_v", dst_sz, VS_S) << 22)
                      | (static_cast<u32>(n) << 5) | static_cast<u32>(d);
}

/// UMOV Wd, Vn.S[lane] — extract S lane to GPR (W reg).
constexpr u32 umov_w_s(u8 d, u8 n, u64 lane) noexcept {
    const u32 imm5 = (uimm_field("umov_w_s", lane, 3) << 3) | 0x4;
    return 0x0E003C00u | (imm5 << 16)
                      | (static_cast<u32>(n) << 5) | static_cast<u32>(d);
}
/// UMOV Wd, Vn.B[lane] — extract B lane to GPR (W reg, zero-extended).
constexpr u32 umov_w_b(u8 d, u8 n, u64 lane) noexcept {
    const u32 imm5 = (uimm_field("umov_w_b", lane, 15) << 1) | 0x1;
    return 0x0E003C00u | (imm5 << 16)
                      | (static_cast<u32>(n) << 5) | static_cast<u32>(d);
}
/// UMOV Wd, Vn.H[lane] — extract H lane to GPR (W reg, zero-extended).
constexpr u32 umov_w_h(u8 d, u8 n, u64 lane) noexcept {
    const u32 imm5 = (uimm_field("umov_w_h", lane, 7) << 2) | 0x2;
    return 0x0E003C00u | (imm5 << 16)
                      | (static_cast<u32>(n) << 5) | static_cast<u32>(d);
}
/// UMOV Xd, Vn.D[lane] — extract D lane to GPR (X reg).
constexpr u32 umov_x_d(u8 d, u8 n, u64 lane) noexcept {
    const u32 imm5 = (uimm_field("umov_x_d", lane, 1) << 4) | 0x8;
    return 0x4E003C00u | (imm5 << 16)
                      | (static_cast<u32>(n) << 5) | static_cast<u32>(d);
}

/// EXT Vd.16B, Vn.16B, Vm.16B, #imm — concat-and-shift extract.
/// Result = (Vm:Vn) >> (imm bytes) — bytes[imm..imm+15] of layout (Vn,Vm).
constexpr u32 ext_v16b(u8 d, u8 n, u8 m, u64 imm) noexcept {
    return 0x6E000000u | (static_cast<u32>(m) << 16)
                      | (uimm_field("ext_v16b", imm, 15) << 11)
                      | (static_cast<u32>(n) << 5) | static_cast<u32>(d);
}

/// INS Vd.<T>[lane], Wn — element-from-GPR insert (B/H/S use W reg).
constexpr u32 ins_v_gpr(u8 d, u64 lane, u8 n, VSize sz) noexcept {
    u32 imm5 = 0;
    const u32 l = uimm_field("ins_v_gpr", lane,
                             (16u >> uimm_field("ins_v_gpr", sz, 3)) - 1u);
    switch (sz) {
    case VS_B: imm5 = (l << 1) | 0x01; break;
    case VS_H: imm5 = (l << 2) | 0x02; break;
    case VS_S: imm5 = (l << 3) | 0x04; break;
    case VS_D: imm5 = (l << 4) | 0x08; break;
    }
    return 0x4E001C00u | (imm5 << 16)
                      | (static_cast<u32>(n) << 5) | static_cast<u32>(d);
}

/// INS Vd.D[1], Vn.D[1] — copy the upper 64-bit lane between vectors.
constexpr u32 ins_v_d1_from_d1(u8 d, u8 n) noexcept {
    return 0x6E184400u | (static_cast<u32>(n) << 5) | static_cast<u32>(d);
}

} // namespace vitreo::cpu::jit::arm64
