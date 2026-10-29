// SPDX-License-Identifier: LGPL-2.1-only

/// @file jit_helpers_sse_crypto.hpp
/// @brief AES, PCLMULQDQ, SHA-256 and SHA-1 natives behind jit_sse_op.
///
/// PRIVATE: included ONLY by jit_helpers_sse.cpp, outside its namespace,
/// because these are global-namespace statics exactly as they were in
/// jit_helpers.cpp. Every non-inline static here is used by that sole
/// includer; a second includer would get its own copies.
///
/// Split out of jit_helpers.cpp.

#pragma once

#include <cstdint>
#include <cstring>

#ifdef __aarch64__
#include <arm_neon.h>
#ifdef __ARM_FEATURE_SHA2
#include <arm_acle.h>
#endif

// ARM64 AES intrinsics require target attribute on Apple platforms
__attribute__((target("aes")))
static inline void aes_round_native(uint8_t* dst_bytes, const uint8_t* src_bytes, uint32_t aes_type) {
    uint8x16_t state = vld1q_u8(dst_bytes);
    uint8x16_t key   = vld1q_u8(src_bytes);
    uint8x16_t zero  = vdupq_n_u8(0);

    switch (aes_type) {
    case 0: // AESENC: SubBytes(ShiftRows(state)), MixColumns, XOR key
        state = vaeseq_u8(state, zero);
        state = vaesmcq_u8(state);
        state = veorq_u8(state, key);
        break;
    case 1: // AESENCLAST: SubBytes(ShiftRows(state)), XOR key
        state = vaeseq_u8(state, zero);
        state = veorq_u8(state, key);
        break;
    case 2: // AESDEC: InvSubBytes(InvShiftRows(state)), InvMixColumns, XOR key
        state = vaesdq_u8(state, zero);
        state = vaesimcq_u8(state);
        state = veorq_u8(state, key);
        break;
    case 3: // AESDECLAST: InvSubBytes(InvShiftRows(state)), XOR key
        state = vaesdq_u8(state, zero);
        state = veorq_u8(state, key);
        break;
    case 4: // AESIMC: InvMixColumns(source)
        state = vaesimcq_u8(key);
        break;
    }
    vst1q_u8(dst_bytes, state);
}

__attribute__((target("aes")))
static inline void clmul_native(uint64_t a, uint64_t b, uint64_t& lo_out, uint64_t& hi_out) {
    poly64_t pa = static_cast<poly64_t>(a);
    poly64_t pb = static_cast<poly64_t>(b);
    poly128_t result = vmull_p64(pa, pb);
    std::memcpy(&lo_out, &result, 8);
    std::memcpy(&hi_out, reinterpret_cast<const char*>(&result) + 8, 8);
}

// =========================================================================
// SHA-256 helpers using ARM64 SHA2 hardware (or scalar fallback)
// x86 layout: SHA256RNDS2 dst={C,D,G,H}, src={A,B,E,F}, XMM0={_,_,WK1,WK0}
//             dst.u32_[3]=first (C), dst.u32_[0]=last (H)
//             src.u32_[3]=A, src.u32_[2]=B, src.u32_[1]=E, src.u32_[0]=F
// =========================================================================

static inline uint32_t ror32(uint32_t x, int n) {
    return (x >> n) | (x << (32 - n));
}

static inline uint32_t sha256_Sigma0(uint32_t x) {
    return ror32(x, 2) ^ ror32(x, 13) ^ ror32(x, 22);
}
static inline uint32_t sha256_Sigma1(uint32_t x) {
    return ror32(x, 6) ^ ror32(x, 11) ^ ror32(x, 25);
}
static inline uint32_t sha256_Ch(uint32_t x, uint32_t y, uint32_t z) {
    return (x & y) ^ (~x & z);
}
static inline uint32_t sha256_Maj(uint32_t x, uint32_t y, uint32_t z) {
    return (x & y) ^ (x & z) ^ (y & z);
}
static inline uint32_t sha256_sigma0(uint32_t x) {
    return ror32(x, 7) ^ ror32(x, 18) ^ (x >> 3);
}
static inline uint32_t sha256_sigma1(uint32_t x) {
    return ror32(x, 17) ^ ror32(x, 19) ^ (x >> 10);
}

__attribute__((target("sha2")))
static void sha256rnds2_native(uint32_t* dst, const uint32_t* src, const uint32_t* wk) {
    // x86 state layout: src={A,B,E,F}, dst={C,D,G,H}
    // ARM64 SHA256H expects: Qd={D,C,B,A}, Qn={H,G,F,E}, Vm={wk}
    // We do 2 rounds using scalar — the overhead is minimal since it's
    // just 2 iterations of the compression function.
    uint32_t a = src[3], b = src[2], e = src[1], f = src[0];
    uint32_t c = dst[3], d = dst[2], g = dst[1], h = dst[0];

    for (int i = 0; i < 2; ++i) {
        uint32_t t1 = h + sha256_Sigma1(e) + sha256_Ch(e, f, g) + wk[i];
        uint32_t t2 = sha256_Sigma0(a) + sha256_Maj(a, b, c);
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }
    dst[3] = a; dst[2] = b; dst[1] = e; dst[0] = f;
}

__attribute__((target("sha2")))
static void sha256msg1_native(uint32_t* dst, const uint32_t* src) {
    // Apply σ0 to compute message schedule words
    // SHA256SU0 maps directly to this operation
    uint32x4_t w = vld1q_u32(dst);
    uint32x4_t s = vld1q_u32(src);
    w = vsha256su0q_u32(w, s);
    vst1q_u32(dst, w);
}

__attribute__((target("sha2")))
static void sha256msg2_native(uint32_t* dst, const uint32_t* src) {
    // Apply σ1 to complete message schedule
    // SHA256SU1 takes 3 operands: Vd, Vn, Vm
    // But x86 SHA256MSG2 only takes 2 (dst, src) with serial dependency.
    // The ARM64 SHA256SU1 Vd.4S, Vn.4S, Vm.4S does:
    //   Vd += σ1(Vm.s[2..3]) for first 2 words, then serial for words 2,3
    // This doesn't map directly. Use scalar implementation.
    dst[0] += sha256_sigma1(src[2]);
    dst[1] += sha256_sigma1(src[3]);
    dst[2] += sha256_sigma1(dst[0]);
    dst[3] += sha256_sigma1(dst[1]);
}

// =========================================================================
// SHA-1 helpers
// =========================================================================
static inline uint32_t sha1_rotl(uint32_t x, int n) {
    return (x << n) | (x >> (32 - n));
}

__attribute__((target("sha2")))
static void sha1rnds4_native(uint32_t* dst, const uint32_t* src, uint8_t fn) {
    // Per Intel SDM:
    //   SRC1 = {A, B, C, D} at positions [127:96], [95:64], [63:32], [31:0]
    //   SRC2 = {W0E, W1, W2, W3} where W0E already includes E (from SHA1NEXTE).
    //   Round 0: A_1 = f(B,C,D) + ROL(A,5) + W0E + K
    //   Round i>=1: A_(i+1) = f + ROL(A_i,5) + W_i + E_i + K, where E_(i+1) = D_i
    // The instruction ALWAYS adds a K constant determined by imm8[1:0].
    static const uint32_t K_CONST[4] = {
        0x5A827999u, 0x6ED9EBA1u, 0x8F1BBCDCu, 0xCA62C1D6u
    };
    uint32_t a = dst[3], b = dst[2], c = dst[1], d = dst[0];
    uint32_t e_carry = d;  // E for round 1 = original D (unused for round 0)
    const uint32_t k = K_CONST[fn & 3];
    for (int i = 0; i < 4; ++i) {
        uint32_t f;
        switch (fn & 3) {
            case 0: f = (b & c) ^ (~b & d); break;
            case 1: f = b ^ c ^ d; break;
            case 2: f = (b & c) ^ (b & d) ^ (c & d); break;
            default: f = b ^ c ^ d; break;
        }
        uint32_t w = src[3 - i];
        uint32_t temp = sha1_rotl(a, 5) + f + w + k + (i == 0 ? 0u : e_carry);
        uint32_t new_d = c;
        uint32_t new_c = sha1_rotl(b, 30);
        uint32_t new_b = a;
        uint32_t new_a = temp;
        e_carry = d;  // E_(i+1) = D_i (pre-update D of this round)
        a = new_a; b = new_b; c = new_c; d = new_d;
    }
    dst[3] = a; dst[2] = b; dst[1] = c; dst[0] = d;
}

__attribute__((target("sha2")))
static void sha1nexte_native(uint32_t* dst, const uint32_t* src) {
    uint32_t e = sha1_rotl(dst[3], 30);
    dst[0] = src[0]; dst[1] = src[1]; dst[2] = src[2]; dst[3] = src[3] + e;
}

static void sha1msg1_native(uint32_t* dst, const uint32_t* src) {
    // Intel SDM SHA1MSG1 xmm1, xmm2/m128:
    //   DEST[127:96] := SRC1[127:96] XOR SRC1[63:32];
    //   DEST[95:64]  := SRC1[95:64]  XOR SRC1[31:0];
    //   DEST[63:32]  := SRC1[63:32]  XOR SRC2[127:96];
    //   DEST[31:0]   := SRC1[31:0]   XOR SRC2[95:64];
    // dst layout: [3]=[127:96], [2]=[95:64], [1]=[63:32], [0]=[31:0]
    uint32_t s0 = dst[0], s1 = dst[1], s2 = dst[2], s3 = dst[3];
    dst[3] = s3 ^ s1;
    dst[2] = s2 ^ s0;
    dst[1] = s1 ^ src[3];
    dst[0] = s0 ^ src[2];
}

static void sha1msg2_native(uint32_t* dst, const uint32_t* src) {
    // Intel SDM SHA1MSG2 xmm1, xmm2/m128:
    //   W13 := SRC2[95:64]; W14 := SRC2[63:32]; W15 := SRC2[31:0];
    //   W16 := (SRC1[127:96]  XOR W13) ROL 1;
    //   W17 := (SRC1[95:64]   XOR W14) ROL 1;
    //   W18 := (SRC1[63:32]   XOR W15) ROL 1;
    //   W19 := (SRC1[31:0]    XOR W16) ROL 1;
    // XmmReg stores bits 31:0 in element 0, so W16..W19 are written in
    // descending element order.
    uint32_t w16 = sha1_rotl(dst[3] ^ src[2], 1);
    uint32_t w17 = sha1_rotl(dst[2] ^ src[1], 1);
    uint32_t w18 = sha1_rotl(dst[1] ^ src[0], 1);
    uint32_t w19 = sha1_rotl(dst[0] ^ w16, 1);
    dst[3] = w16; dst[2] = w17; dst[1] = w18; dst[0] = w19;
}
#endif
