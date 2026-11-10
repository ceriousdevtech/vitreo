// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64

#pragma once

/// @file stencils_arm64_crypto.hpp
/// @brief ARM64 CRC32C, PMULL, SHA-1, AES and SHA-256 encoders.
///
/// Part of the arm64:: encoder set; include core/cpu/jit/stencils.hpp.

#include <core/cpu/jit/stencils_arm64_fields.hpp>
#include <vitreo/common.hpp>

#include <cstdlib>
#include <cstring>

namespace vitreo::cpu::jit::arm64 {

/// Encode ARM64 CRC32CB Wd, Wn, Wm (Castagnoli CRC over one byte).
constexpr u32 crc32cb_w(u8 d, u8 n, u8 m) noexcept {
    return 0x1AC05000u | (static_cast<u32>(m) << 16)
                       | (static_cast<u32>(n) << 5)
                       | static_cast<u32>(d);
}

/// Encode ARM64 CRC32CH Wd, Wn, Wm (Castagnoli CRC over one halfword).
constexpr u32 crc32ch_w(u8 d, u8 n, u8 m) noexcept {
    return 0x1AC05400u | (static_cast<u32>(m) << 16)
                       | (static_cast<u32>(n) << 5)
                       | static_cast<u32>(d);
}

/// Encode ARM64 CRC32CW Wd, Wn, Wm (Castagnoli CRC over one word).
constexpr u32 crc32cw_w(u8 d, u8 n, u8 m) noexcept {
    return 0x1AC05800u | (static_cast<u32>(m) << 16)
                       | (static_cast<u32>(n) << 5)
                       | static_cast<u32>(d);
}

/// Encode ARM64 CRC32CX Wd, Wn, Xm (Castagnoli CRC over one doubleword).
constexpr u32 crc32cx_w(u8 d, u8 n, u8 m) noexcept {
    return 0x9AC05C00u | (static_cast<u32>(m) << 16)
                       | (static_cast<u32>(n) << 5)
                       | static_cast<u32>(d);
}

/// REV64 Vd.4S, Vn.4S — swap the two 32-bit lanes of each 64-bit half.
constexpr u32 rev64_v4s(u8 d, u8 n) noexcept {
    return 0x4EA00800u | (static_cast<u32>(n) << 5) | static_cast<u32>(d);
}
/// SHA1C/SHA1P/SHA1M Qd, Sn, Vm.4S (FEAT_SHA1): four SHA-1 rounds with the
/// choose (op 0), parity (1) or majority (2) function.
constexpr u32 sha1_hash(u64 op, u8 d, u8 n, u8 m) noexcept {
    return 0x5E000000u | (static_cast<u32>(m) << 16)
                       | (uimm_field("sha1_hash", op, 2) << 12)
                       | (static_cast<u32>(n) << 5) | static_cast<u32>(d);
}
/// SHA1SU1 Vd.4S, Vn.4S (FEAT_SHA1).
constexpr u32 sha1su1_v(u8 d, u8 n) noexcept {
    return 0x5E281800u | (static_cast<u32>(n) << 5) | static_cast<u32>(d);
}
// llvm-mc -triple=aarch64 -mattr=+sha2 -show-encoding.
static_assert(rev64_v4s(0, 0) == 0x4EA00800u);      // rev64 v0.4s, v0.4s
static_assert(rev64_v4s(18, 21) == 0x4EA00AB2u);    // rev64 v18.4s, v21.4s
static_assert(sha1_hash(0, 0, 3, 1) == 0x5E010060u);   // sha1c q0, s3, v1.4s
static_assert(sha1_hash(1, 0, 3, 1) == 0x5E011060u);   // sha1p q0, s3, v1.4s
static_assert(sha1_hash(2, 0, 3, 1) == 0x5E012060u);   // sha1m q0, s3, v1.4s
static_assert(sha1_hash(0, 18, 20, 21) == 0x5E150292u);  // sha1c q18, s20, v21.4s
static_assert(sha1su1_v(0, 1) == 0x5E281820u);      // sha1su1 v0.4s, v1.4s
static_assert(sha1su1_v(18, 21) == 0x5E281AB2u);    // sha1su1 v18.4s, v21.4s

/// Encode PMULL Vd.1Q, Vn.1D, Vm.1D — 64×64 carry-less multiply.
/// The 128-bit polynomial product is written to the full destination vector.
constexpr u32 pmull_v1q(u8 d, u8 n, u8 m) noexcept {
    return 0x0EE0E000u
         | (static_cast<u32>(m) << 16)
         | (static_cast<u32>(n) << 5)
         | static_cast<u32>(d);
}

// ---- ARMv8 AES (FEAT_AES) ----
//
// Encoding (ARM ARM C4.1 "Cryptographic AES"):
//   31..24 = 0100 1110, size(23:22) = 00, bits 21:17 = 10100,
//   opcode(16:12), bits 11:10 = 10, Rn(9:5), Rd(4:0).
// opcode: AESE=00100, AESD=00101, AESMC=00110, AESIMC=00111.
//
// AESE/AESD are read-modify-write on Vd: Vd is both the state operand and the
// result. AESMC/AESIMC are plain Vd := f(Vn).
//
// Gate every use on host_supports_aes().

/// AESE Vd.16B, Vn.16B — Vd := AESSubBytes(AESShiftRows(Vd EOR Vn)).
constexpr u32 aese(u8 d, u8 n) noexcept {
    return 0x4E284800u | (static_cast<u32>(n) << 5) | static_cast<u32>(d);
}

/// AESD Vd.16B, Vn.16B — Vd := AESInvSubBytes(AESInvShiftRows(Vd EOR Vn)).
constexpr u32 aesd(u8 d, u8 n) noexcept {
    return 0x4E285800u | (static_cast<u32>(n) << 5) | static_cast<u32>(d);
}

/// AESMC Vd.16B, Vn.16B — Vd := AESMixColumns(Vn).
constexpr u32 aesmc(u8 d, u8 n) noexcept {
    return 0x4E286800u | (static_cast<u32>(n) << 5) | static_cast<u32>(d);
}

/// AESIMC Vd.16B, Vn.16B — Vd := AESInvMixColumns(Vn).
constexpr u32 aesimc(u8 d, u8 n) noexcept {
    return 0x4E287800u | (static_cast<u32>(n) << 5) | static_cast<u32>(d);
}

// ---- ARMv8 SHA-256 (FEAT_SHA256) ----
//
// These implement the same message-schedule primitives as the x86 SHA
// extension, so they can replace the helper-routed forms directly. The
// register-number fields match the standard three-register NEON layout.

/// SHA256SU0 Vd.4S, Vn.4S — Vd[i] += sigma0(w[i+1]), where w[4] = Vn.S[0].
/// This is exactly x86 `SHA256MSG1 xmm_d, xmm_n`.
constexpr u32 sha256su0(u8 d, u8 n) noexcept {
    return 0x5E282800u | (static_cast<u32>(n) << 5) | static_cast<u32>(d);
}

/// SHA256SU1 Vd.4S, Vn.4S, Vm.4S.
/// T0 = Vn.S[1..3]:Vm.S[0]; T1 = Vm.S[2..3]; T3 = Vd + T0;
/// result[0..1] = T3[0..1] + sigma1(T1[0..1]);
/// result[2..3] = T3[2..3] + sigma1(result[0..1]).
/// With Vn = 0 and Vm.S[0] = 0 the T0 term vanishes and the remainder is
/// exactly x86 `SHA256MSG2`.
constexpr u32 sha256su1(u8 d, u8 n, u8 m) noexcept {
    return 0x5E006000u | (static_cast<u32>(m) << 16)
                      | (static_cast<u32>(n) << 5) | static_cast<u32>(d);
}

// ---- ARMv8 SHA-256 hash rounds (FEAT_SHA256), WS3 ----
//
// ARM ARM SHA256H / SHA256H2: four SHA-256 rounds over X = {a,b,c,d} and
// Y = {e,f,g,h} (lane 0 = a / e) with W = Vm.4S, the round constants already
// added. SHA256H Qd, Qn, Vm returns the new X (Qd = X, Qn = Y); SHA256H2
// Qd, Qn, Vm returns the new Y (Qd = Y, Qn = X). Gate on host_supports_sha256().

/// SHA256H Qd, Qn, Vm.4S.
constexpr u32 sha256h(u8 d, u8 n, u8 m) noexcept {
    return 0x5E004000u | (static_cast<u32>(m) << 16)
                      | (static_cast<u32>(n) << 5) | static_cast<u32>(d);
}

/// SHA256H2 Qd, Qn, Vm.4S.
constexpr u32 sha256h2(u8 d, u8 n, u8 m) noexcept {
    return 0x5E005000u | (static_cast<u32>(m) << 16)
                      | (static_cast<u32>(n) << 5) | static_cast<u32>(d);
}
// llvm-mc -triple=aarch64 -mattr=+sha2 -show-encoding.
static_assert(sha256h(0, 1, 2) == 0x5E024020u);    // sha256h  q0, q1, v2.4s
static_assert(sha256h2(3, 4, 2) == 0x5E025083u);   // sha256h2 q3, q4, v2.4s
static_assert(sha256h(3, 4, 2) == 0x5E024083u);    // sha256h  q3, q4, v2.4s
static_assert(sha256h2(4, 5, 2) == 0x5E0250A4u);   // sha256h2 q4, q5, v2.4s

} // namespace vitreo::cpu::jit::arm64

namespace vitreo::cpu::jit {

// ---- WS3 host gates and kill switch ----
//
// Compile-time queries only; never called from emitted code.

/// False only when VITREO_WS3 is exactly "0": the WS3 crypto lowerings (the
/// SHA-1 / SHA-256 round / PCLMULQDQ memory / AESKEYGENASSIST / VEX.128 AES
/// direct forms and the 32-bit non-linear m128 stager) then emit nothing and
/// the pre-WS3 code is produced exactly. Read uncached so tests and the
/// direct-vs-helper benchmark can A/B in one process.
inline bool ws3_crypto_direct_enabled() noexcept {
    const char* v = std::getenv("VITREO_WS3");
    return v == nullptr || std::strcmp(v, "0") != 0;
}

// sysctl hw.optional.arm.FEAT_* (an absent key means not advertised), cached
// per process. Defined in ops/direct_armv8_aes.cpp so <sys/sysctl.h> (whose
// <sys/param.h> defines FSCALE and friends) stays out of this header.

/// FEAT_SHA1 (SHA1C/P/M/H, SHA1SU0/SU1).
[[nodiscard]] bool host_supports_sha1() noexcept;
/// FEAT_SHA256 (SHA256H/H2, SHA256SU0/SU1).
[[nodiscard]] bool host_supports_sha256() noexcept;
/// FEAT_PMULL (PMULL/PMULL2 .1Q).
[[nodiscard]] bool host_supports_pmull() noexcept;

} // namespace vitreo::cpu::jit
