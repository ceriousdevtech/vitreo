// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// SSSE3/SSE4.1/AES-NI/SHA/PCLMULQDQ instruction execution
//
// Covers three-byte escape opcodes (0F 38 XX, 0F 3A XX):
//   PSHUFB, PMULLD, PALIGNR,
//   AESENC, AESENCLAST, AESDEC, AESDECLAST, AESIMC, AESKEYGENASSIST,
//   PCLMULQDQ,
//   SHA1NEXTE, SHA1MSG1, SHA1MSG2, SHA1RNDS4,
//   SHA256RNDS2, SHA256MSG1, SHA256MSG2

#include "exec_ops.hpp"
#include "helpers.hpp"
#include <cstdlib>
#include <cstring>
#include <atomic>

#ifdef __aarch64__
#include <arm_neon.h>

// ARM64 single-instruction crypto intrinsics used as fast paths on Apple
// Silicon / any AArch64 host that exposes the AES/SHA2 feature. The scalar
// code below remains as the portable fallback for non-AArch64 builds and
// for opcodes the hardware doesn't cover directly (e.g. 2-round SHA256RNDS2).
__attribute__((target("aes")))
static inline void arm64_aes_enc(uint8_t* dst, const uint8_t* key) noexcept {
    uint8x16_t state = vld1q_u8(dst);
    uint8x16_t k     = vld1q_u8(key);
    // vaeseq_u8: AddRoundKey(0) -> SubBytes -> ShiftRows. The x86 AESENC
    // operation is ShiftRows -> SubBytes -> MixColumns -> XorKey. Because
    // SubBytes and ShiftRows commute and AddRoundKey with zero is a no-op,
    // vaeseq_u8 produces ShiftRows(SubBytes(state)).
    state = vaeseq_u8(state, vdupq_n_u8(0));
    state = vaesmcq_u8(state);
    state = veorq_u8(state, k);
    vst1q_u8(dst, state);
}

__attribute__((target("aes")))
static inline void arm64_aes_enc_last(uint8_t* dst, const uint8_t* key) noexcept {
    uint8x16_t state = vld1q_u8(dst);
    uint8x16_t k     = vld1q_u8(key);
    state = vaeseq_u8(state, vdupq_n_u8(0));
    state = veorq_u8(state, k);
    vst1q_u8(dst, state);
}

__attribute__((target("aes")))
static inline void arm64_aes_dec(uint8_t* dst, const uint8_t* key) noexcept {
    uint8x16_t state = vld1q_u8(dst);
    uint8x16_t k     = vld1q_u8(key);
    state = vaesdq_u8(state, vdupq_n_u8(0));
    state = vaesimcq_u8(state);
    state = veorq_u8(state, k);
    vst1q_u8(dst, state);
}

__attribute__((target("aes")))
static inline void arm64_aes_dec_last(uint8_t* dst, const uint8_t* key) noexcept {
    uint8x16_t state = vld1q_u8(dst);
    uint8x16_t k     = vld1q_u8(key);
    state = vaesdq_u8(state, vdupq_n_u8(0));
    state = veorq_u8(state, k);
    vst1q_u8(dst, state);
}

__attribute__((target("aes")))
static inline void arm64_aes_imc(uint8_t* dst, const uint8_t* src) noexcept {
    uint8x16_t s = vld1q_u8(src);
    s = vaesimcq_u8(s);
    vst1q_u8(dst, s);
}

__attribute__((target("aes")))
static inline void arm64_clmul(uint64_t a, uint64_t b,
                                uint64_t& lo, uint64_t& hi) noexcept {
    poly128_t r = vmull_p64(static_cast<poly64_t>(a),
                             static_cast<poly64_t>(b));
    std::memcpy(&lo, &r, 8);
    std::memcpy(&hi, reinterpret_cast<const char*>(&r) + 8, 8);
}

__attribute__((target("sha2")))
static inline void arm64_sha256_msg1(uint32_t* dst,
                                      const uint32_t* src) noexcept {
    // x86 SHA256MSG1: result[i] = dst[i] + σ0(next) where next dwords
    // rotate in from dst[1..3] and src[0]. ARM64 vsha256su0q_u32(w0, w1)
    // computes the same per-lane σ0 accumulation.
    uint32x4_t w = vld1q_u32(dst);
    uint32x4_t s = vld1q_u32(src);
    w = vsha256su0q_u32(w, s);
    vst1q_u32(dst, w);
}

// PSHUFB: for each byte i in dst, if src[i] bit 7 is set dst[i]=0, else
// dst[i] = orig_dst[src[i] & 0x0F]. vqtbl1q_u8 returns 0 whenever the
// index is >= 16, and (src[i] & 0x8F) is >= 128 exactly when bit 7 is
// set — otherwise it is src[i] & 0x0F.
static inline void arm64_pshufb(uint8_t* dst, const uint8_t* src) noexcept {
    uint8x16_t table = vld1q_u8(dst);
    uint8x16_t idx   = vld1q_u8(src);
    uint8x16_t mask  = vdupq_n_u8(0x8F);
    uint8x16_t r     = vqtbl1q_u8(table, vandq_u8(idx, mask));
    vst1q_u8(dst, r);
}

// PMULLD: 4 lanes of 32x32 → low-32 signed product. Two's-complement
// makes signed/unsigned low-32 equivalent, so vmulq_u32 is correct.
static inline void arm64_pmulld(uint32_t* dst, const uint32_t* src) noexcept {
    uint32x4_t a = vld1q_u32(dst);
    uint32x4_t b = vld1q_u32(src);
    vst1q_u32(dst, vmulq_u32(a, b));
}

// PALIGNR: concatenate dst:src as 32 bytes (src in low half, dst in high
// half) and take bytes [shift..shift+15]. For shift in [0,16) this is
// vextq_u8(src, dst, shift). For shift in [16,32) it's
// vextq_u8(dst, zero, shift-16). shift >= 32 yields zero. We return
// true when the NEON path handled the value so the caller can fall
// back to the scalar implementation for unusual shifts.
static inline void arm64_palignr(uint8_t* dst, const uint8_t* src,
                                  unsigned shift) noexcept {
    uint8x16_t d = vld1q_u8(dst);
    uint8x16_t s = vld1q_u8(src);
    uint8x16_t z = vdupq_n_u8(0);
    uint8x16_t r;
    if (shift >= 32) {
        r = z;
    } else if (shift == 0) {
        r = s;
    } else if (shift < 16) {
        // vextq_u8 requires a compile-time constant shift amount.
        switch (shift) {
        case 1:  r = vextq_u8(s, d, 1);  break;
        case 2:  r = vextq_u8(s, d, 2);  break;
        case 3:  r = vextq_u8(s, d, 3);  break;
        case 4:  r = vextq_u8(s, d, 4);  break;
        case 5:  r = vextq_u8(s, d, 5);  break;
        case 6:  r = vextq_u8(s, d, 6);  break;
        case 7:  r = vextq_u8(s, d, 7);  break;
        case 8:  r = vextq_u8(s, d, 8);  break;
        case 9:  r = vextq_u8(s, d, 9);  break;
        case 10: r = vextq_u8(s, d, 10); break;
        case 11: r = vextq_u8(s, d, 11); break;
        case 12: r = vextq_u8(s, d, 12); break;
        case 13: r = vextq_u8(s, d, 13); break;
        case 14: r = vextq_u8(s, d, 14); break;
        default: r = vextq_u8(s, d, 15); break; // case 15
        }
    } else if (shift == 16) {
        r = d;
    } else {
        switch (shift - 16) {
        case 1:  r = vextq_u8(d, z, 1);  break;
        case 2:  r = vextq_u8(d, z, 2);  break;
        case 3:  r = vextq_u8(d, z, 3);  break;
        case 4:  r = vextq_u8(d, z, 4);  break;
        case 5:  r = vextq_u8(d, z, 5);  break;
        case 6:  r = vextq_u8(d, z, 6);  break;
        case 7:  r = vextq_u8(d, z, 7);  break;
        case 8:  r = vextq_u8(d, z, 8);  break;
        case 9:  r = vextq_u8(d, z, 9);  break;
        case 10: r = vextq_u8(d, z, 10); break;
        case 11: r = vextq_u8(d, z, 11); break;
        case 12: r = vextq_u8(d, z, 12); break;
        case 13: r = vextq_u8(d, z, 13); break;
        case 14: r = vextq_u8(d, z, 14); break;
        default: r = vextq_u8(d, z, 15); break; // case 15 -> shift 31
        }
    }
    vst1q_u8(dst, r);
}
#endif // __aarch64__

namespace vitreo::cpu::ops {

static inline bool diag_logs_enabled() noexcept {
    static const bool enabled = (std::getenv("VITREO_ENABLE_DIAG_LOGS") != nullptr);
    return enabled;
}

// ============================================================================
// SSE register / operand helpers
// ============================================================================

static XmmReg& get_xmm_c(ExecContext& ctx, u8 index) noexcept {
    return ctx.cpu.sse.xmm[index & 15];
}

static Result<XmmReg, Error> read_pack_c(ExecContext& ctx, const Operand& op,
                                         bool aligned_form = true) noexcept {
    if (op.type == OperandType::Reg128) {
        return Result<XmmReg, Error>::ok(ctx.cpu.sse.xmm[op.reg & 15]);
    }
    auto addr = calc_effective_address(ctx, op.mem);
    if (!addr) return Result<XmmReg, Error>::err(addr.error());
    if (aligned_form && op.type == OperandType::Mem128 &&
        (addr.value() & 0xF) != 0) {
        ctx.exception = CpuException::GeneralProtection;
        return Result<XmmReg, Error>::err(Error::InvalidArgument);
    }
    XmmReg tmp{};
    auto r = ctx.mem.read_bytes(addr.value(), reinterpret_cast<Byte*>(&tmp), 16);
    if (!r) return Result<XmmReg, Error>::err(r.error());
    return Result<XmmReg, Error>::ok(tmp);
}

// ============================================================================
// AES helpers — GF(2^8) with irreducible polynomial x^8+x^4+x^3+x+1 (0x11B)
// ============================================================================

namespace {

// AES S-Box
alignas(64) static constexpr u8 aes_sbox[256] = {
    0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
    0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
    0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
    0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
    0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
    0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
    0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
    0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
    0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
    0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
    0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
    0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
    0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
    0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
    0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
    0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16
};

// AES Inverse S-Box
alignas(64) static constexpr u8 aes_inv_sbox[256] = {
    0x52,0x09,0x6a,0xd5,0x30,0x36,0xa5,0x38,0xbf,0x40,0xa3,0x9e,0x81,0xf3,0xd7,0xfb,
    0x7c,0xe3,0x39,0x82,0x9b,0x2f,0xff,0x87,0x34,0x8e,0x43,0x44,0xc4,0xde,0xe9,0xcb,
    0x54,0x7b,0x94,0x32,0xa6,0xc2,0x23,0x3d,0xee,0x4c,0x95,0x0b,0x42,0xfa,0xc3,0x4e,
    0x08,0x2e,0xa1,0x66,0x28,0xd9,0x24,0xb2,0x76,0x5b,0xa2,0x49,0x6d,0x8b,0xd1,0x25,
    0x72,0xf8,0xf6,0x64,0x86,0x68,0x98,0x16,0xd4,0xa4,0x5c,0xcc,0x5d,0x65,0xb6,0x92,
    0x6c,0x70,0x48,0x50,0xfd,0xed,0xb9,0xda,0x5e,0x15,0x46,0x57,0xa7,0x8d,0x9d,0x84,
    0x90,0xd8,0xab,0x00,0x8c,0xbc,0xd3,0x0a,0xf7,0xe4,0x58,0x05,0xb8,0xb3,0x45,0x06,
    0xd0,0x2c,0x1e,0x8f,0xca,0x3f,0x0f,0x02,0xc1,0xaf,0xbd,0x03,0x01,0x13,0x8a,0x6b,
    0x3a,0x91,0x11,0x41,0x4f,0x67,0xdc,0xea,0x97,0xf2,0xcf,0xce,0xf0,0xb4,0xe6,0x73,
    0x96,0xac,0x74,0x22,0xe7,0xad,0x35,0x85,0xe2,0xf9,0x37,0xe8,0x1c,0x75,0xdf,0x6e,
    0x47,0xf1,0x1a,0x71,0x1d,0x29,0xc5,0x89,0x6f,0xb7,0x62,0x0e,0xaa,0x18,0xbe,0x1b,
    0xfc,0x56,0x3e,0x4b,0xc6,0xd2,0x79,0x20,0x9a,0xdb,0xc0,0xfe,0x78,0xcd,0x5a,0xf4,
    0x1f,0xdd,0xa8,0x33,0x88,0x07,0xc7,0x31,0xb1,0x12,0x10,0x59,0x27,0x80,0xec,0x5f,
    0x60,0x51,0x7f,0xa9,0x19,0xb5,0x4a,0x0d,0x2d,0xe5,0x7a,0x9f,0x93,0xc9,0x9c,0xef,
    0xa0,0xe0,0x3b,0x4d,0xae,0x2a,0xf5,0xb0,0xc8,0xeb,0xbb,0x3c,0x83,0x53,0x99,0x61,
    0x17,0x2b,0x04,0x7e,0xba,0x77,0xd6,0x26,0xe1,0x69,0x14,0x63,0x55,0x21,0x0c,0x7d
};

/// GF(2^8) multiply
static u8 gf_mul(u8 a, u8 b) noexcept {
    u8 result = 0;
    for (int i = 0; i < 8; ++i) {
        if (b & 1) result ^= a;
        bool hi = (a & 0x80) != 0;
        a <<= 1;
        if (hi) a ^= 0x1B; // x^8+x^4+x^3+x+1
        b >>= 1;
    }
    return result;
}

/// AES SubBytes (in-place on 4x4 column-major state)
static void aes_sub_bytes(u8 state[16]) noexcept {
    for (int i = 0; i < 16; ++i)
        state[i] = aes_sbox[state[i]];
}

/// AES InvSubBytes
static void aes_inv_sub_bytes(u8 state[16]) noexcept {
    for (int i = 0; i < 16; ++i)
        state[i] = aes_inv_sbox[state[i]];
}

/// AES ShiftRows (column-major: state[row + 4*col])
static void aes_shift_rows(u8 state[16]) noexcept {
    // Row 1: shift left by 1
    u8 t = state[1];
    state[1]  = state[5];
    state[5]  = state[9];
    state[9]  = state[13];
    state[13] = t;
    // Row 2: shift left by 2
    u8 t0 = state[2], t1 = state[6];
    state[2]  = state[10];
    state[6]  = state[14];
    state[10] = t0;
    state[14] = t1;
    // Row 3: shift left by 3 (= right by 1)
    t = state[15];
    state[15] = state[11];
    state[11] = state[7];
    state[7]  = state[3];
    state[3]  = t;
}

/// AES InvShiftRows
static void aes_inv_shift_rows(u8 state[16]) noexcept {
    // Row 1: shift right by 1
    u8 t = state[13];
    state[13] = state[9];
    state[9]  = state[5];
    state[5]  = state[1];
    state[1]  = t;
    // Row 2: shift right by 2
    u8 t0 = state[2], t1 = state[6];
    state[2]  = state[10];
    state[6]  = state[14];
    state[10] = t0;
    state[14] = t1;
    // Row 3: shift right by 3 (= left by 1)
    t = state[3];
    state[3]  = state[7];
    state[7]  = state[11];
    state[11] = state[15];
    state[15] = t;
}

/// AES MixColumns (column-major: column c = state[0+4c..3+4c])
static void aes_mix_columns(u8 state[16]) noexcept {
    for (int c = 0; c < 4; ++c) {
        u8* col = &state[c * 4];
        u8 a0 = col[0], a1 = col[1], a2 = col[2], a3 = col[3];
        col[0] = gf_mul(a0, 2) ^ gf_mul(a1, 3) ^ a2 ^ a3;
        col[1] = a0 ^ gf_mul(a1, 2) ^ gf_mul(a2, 3) ^ a3;
        col[2] = a0 ^ a1 ^ gf_mul(a2, 2) ^ gf_mul(a3, 3);
        col[3] = gf_mul(a0, 3) ^ a1 ^ a2 ^ gf_mul(a3, 2);
    }
}

/// AES InvMixColumns
static void aes_inv_mix_columns(u8 state[16]) noexcept {
    for (int c = 0; c < 4; ++c) {
        u8* col = &state[c * 4];
        u8 a0 = col[0], a1 = col[1], a2 = col[2], a3 = col[3];
        col[0] = gf_mul(a0, 0x0e) ^ gf_mul(a1, 0x0b) ^ gf_mul(a2, 0x0d) ^ gf_mul(a3, 0x09);
        col[1] = gf_mul(a0, 0x09) ^ gf_mul(a1, 0x0e) ^ gf_mul(a2, 0x0b) ^ gf_mul(a3, 0x0d);
        col[2] = gf_mul(a0, 0x0d) ^ gf_mul(a1, 0x09) ^ gf_mul(a2, 0x0e) ^ gf_mul(a3, 0x0b);
        col[3] = gf_mul(a0, 0x0b) ^ gf_mul(a1, 0x0d) ^ gf_mul(a2, 0x09) ^ gf_mul(a3, 0x0e);
    }
}

// AES round constants
static constexpr u8 aes_rcon[11] = {
    0x00, 0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0x1B, 0x36
};

// ============================================================================
// SHA helpers
// ============================================================================

static u32 sha1_rol(u32 x, int n) noexcept {
    return (x << n) | (x >> (32 - n));
}

static u32 sha256_ror(u32 x, int n) noexcept {
    return (x >> n) | (x << (32 - n));
}

static u32 sha256_sigma0(u32 x) noexcept {
    return sha256_ror(x, 2) ^ sha256_ror(x, 13) ^ sha256_ror(x, 22);
}

static u32 sha256_sigma1(u32 x) noexcept {
    return sha256_ror(x, 6) ^ sha256_ror(x, 11) ^ sha256_ror(x, 25);
}

static u32 sha256_ch(u32 x, u32 y, u32 z) noexcept {
    return (x & y) ^ (~x & z);
}

static u32 sha256_maj(u32 x, u32 y, u32 z) noexcept {
    return (x & y) ^ (x & z) ^ (y & z);
}

static u32 sha256_sigma_small0(u32 x) noexcept {
    return sha256_ror(x, 7) ^ sha256_ror(x, 18) ^ (x >> 3);
}

static u32 sha256_sigma_small1(u32 x) noexcept {
    return sha256_ror(x, 17) ^ sha256_ror(x, 19) ^ (x >> 10);
}

/// Read a big-endian u32 from XmmReg at dword index
static u32 xmm_get_be32(const XmmReg& r, int idx) noexcept {
    // x86 stores in little-endian, but SHA uses big-endian dwords in XMM
    // Intel SHA extensions interpret XMM dwords as big-endian state words
    // stored in native (little-endian) byte order. The dword index
    // maps to u32_[3-idx] for descending element order.
    return r.u32_[3 - idx];
}

static void xmm_set_be32(XmmReg& r, int idx, u32 val) noexcept {
    r.u32_[3 - idx] = val;
}

// ============================================================================
// Carryless multiply helper for PCLMULQDQ
// ============================================================================

static void clmul64(u64 a, u64 b, u64& result_lo, u64& result_hi) noexcept {
    u64 lo = 0, hi = 0;
    for (int i = 0; i < 64; ++i) {
        if ((b >> i) & 1) {
            lo ^= (a << i);
            if (i > 0)
                hi ^= (a >> (64 - i));
        }
    }
    result_lo = lo;
    result_hi = hi;
}

} // anonymous namespace

// ============================================================================
// Main dispatch
// ============================================================================

ExecResult exec_crypto_ops(ExecContext& ctx, const DecodedInstruction& instr) noexcept {
    // VEX forms are unaligned unless the opcode says otherwise (Intel SDM
    // Vol. 2), which is why VAESENC and friends must not take the check.
    const bool aligned_form = !instr.prefixes.vex && !ctx.vex_origin;
    const auto& op0 = instr.operands[0];
    const auto& op1 = instr.operands[1];

    switch (instr.opcode) {

    // ========================================================================
    // PSHUFB — Packed Shuffle Bytes
    // For each byte i in dst: if src[i] bit 7 set, dst[i]=0; else dst[i]=dst[src[i]&0xF]
    // ========================================================================
    case Opcode::PSHUFB_XMM_XMMM128: {
        auto src_r = read_pack_c(ctx, op1, aligned_form);
        if (!src_r) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
        auto& dst = get_xmm_c(ctx, op0.reg);
        XmmReg src = src_r.value();
#ifdef __aarch64__
        arm64_pshufb(dst.u8_, src.u8_);
#else
        XmmReg orig = dst; // snapshot before modification
        for (int i = 0; i < 16; ++i) {
            if (src.u8_[i] & 0x80) {
                dst.u8_[i] = 0;
            } else {
                dst.u8_[i] = orig.u8_[src.u8_[i] & 0x0F];
            }
        }
#endif
        return ExecResult::Continue;
    }

    // ========================================================================
    // PMULLD — Packed Multiply Low Dword
    // ========================================================================
    case Opcode::PMULLD_XMM_XMMM128: {
        auto src_r = read_pack_c(ctx, op1, aligned_form);
        if (!src_r) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
        auto& dst = get_xmm_c(ctx, op0.reg);
        XmmReg src = src_r.value();
#ifdef __aarch64__
        arm64_pmulld(dst.u32_, src.u32_);
#else
        for (int i = 0; i < 4; ++i) {
            i64 prod = static_cast<i64>(static_cast<i32>(dst.u32_[i])) *
                       static_cast<i64>(static_cast<i32>(src.u32_[i]));
            dst.u32_[i] = static_cast<u32>(prod);
        }
#endif
        return ExecResult::Continue;
    }

    // ========================================================================
    // PALIGNR — Packed Align Right
    // Concatenates dst:src (256 bits), shifts right by imm8 bytes, takes low 128
    // ========================================================================
    case Opcode::PALIGNR_XMM_XMMM128_IMM8: {
        auto src_r = read_pack_c(ctx, op1, aligned_form);
        if (!src_r) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
        auto& dst = get_xmm_c(ctx, op0.reg);
        XmmReg src = src_r.value();
        u8 shift = static_cast<u8>(instr.operands[2].imm);

#ifdef __aarch64__
        arm64_palignr(dst.u8_, src.u8_, shift);
#else
        if (shift >= 32) {
            dst.clear();
        } else {
            // Concatenate dst:src into 32 bytes, shift right by 'shift', take low 16
            u8 concat[32];
            std::memcpy(concat, src.u8_, 16);
            std::memcpy(concat + 16, dst.u8_, 16);
            XmmReg result{};
            for (int i = 0; i < 16; ++i) {
                int idx = i + shift;
                if (idx < 32) {
                    result.u8_[i] = concat[idx];
                }
                // else remains 0
            }
            dst = result;
        }
#endif
        return ExecResult::Continue;
    }

    // ========================================================================
    // AESENC — Perform one AES encryption round
    // state = ShiftRows(state); state = SubBytes(state);
    // state = MixColumns(state); state ^= RoundKey
    // ========================================================================
    case Opcode::AESENC_XMM_XMMM128: {
        auto key_r = read_pack_c(ctx, op1, aligned_form);
        if (!key_r) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
        auto& dst = get_xmm_c(ctx, op0.reg);
        XmmReg key = key_r.value();
#ifdef __aarch64__
        arm64_aes_enc(dst.u8_, key.u8_);
#else
        u8 state[16];
        std::memcpy(state, dst.u8_, 16);
        aes_shift_rows(state);
        aes_sub_bytes(state);
        aes_mix_columns(state);
        for (int i = 0; i < 16; ++i)
            state[i] ^= key.u8_[i];
        std::memcpy(dst.u8_, state, 16);
#endif
        return ExecResult::Continue;
    }

    // ========================================================================
    // AESENCLAST — Perform last AES encryption round (no MixColumns)
    // ========================================================================
    case Opcode::AESENCLAST_XMM_XMMM128: {
        auto key_r = read_pack_c(ctx, op1, aligned_form);
        if (!key_r) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
        auto& dst = get_xmm_c(ctx, op0.reg);
        XmmReg key = key_r.value();
#ifdef __aarch64__
        arm64_aes_enc_last(dst.u8_, key.u8_);
#else
        u8 state[16];
        std::memcpy(state, dst.u8_, 16);
        aes_shift_rows(state);
        aes_sub_bytes(state);
        // No MixColumns for last round
        for (int i = 0; i < 16; ++i)
            state[i] ^= key.u8_[i];
        std::memcpy(dst.u8_, state, 16);
#endif
        return ExecResult::Continue;
    }

    // ========================================================================
    // AESDEC — Perform one AES decryption round
    // ========================================================================
    case Opcode::AESDEC_XMM_XMMM128: {
        auto key_r = read_pack_c(ctx, op1, aligned_form);
        if (!key_r) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
        auto& dst = get_xmm_c(ctx, op0.reg);
        XmmReg key = key_r.value();
#ifdef __aarch64__
        arm64_aes_dec(dst.u8_, key.u8_);
#else
        u8 state[16];
        std::memcpy(state, dst.u8_, 16);
        aes_inv_shift_rows(state);
        aes_inv_sub_bytes(state);
        aes_inv_mix_columns(state);
        for (int i = 0; i < 16; ++i)
            state[i] ^= key.u8_[i];
        std::memcpy(dst.u8_, state, 16);
#endif
        return ExecResult::Continue;
    }

    // ========================================================================
    // AESDECLAST — Perform last AES decryption round (no InvMixColumns)
    // ========================================================================
    case Opcode::AESDECLAST_XMM_XMMM128: {
        auto key_r = read_pack_c(ctx, op1, aligned_form);
        if (!key_r) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
        auto& dst = get_xmm_c(ctx, op0.reg);
        XmmReg key = key_r.value();
#ifdef __aarch64__
        arm64_aes_dec_last(dst.u8_, key.u8_);
#else
        u8 state[16];
        std::memcpy(state, dst.u8_, 16);
        aes_inv_shift_rows(state);
        aes_inv_sub_bytes(state);
        for (int i = 0; i < 16; ++i)
            state[i] ^= key.u8_[i];
        std::memcpy(dst.u8_, state, 16);
#endif
        return ExecResult::Continue;
    }

    // ========================================================================
    // AESIMC — AES Inverse Mix Columns
    // ========================================================================
    case Opcode::AESIMC_XMM_XMMM128: {
        auto src_r = read_pack_c(ctx, op1, aligned_form);
        if (!src_r) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
        auto& dst = get_xmm_c(ctx, op0.reg);
        XmmReg src = src_r.value();
#ifdef __aarch64__
        arm64_aes_imc(dst.u8_, src.u8_);
#else
        u8 state[16];
        std::memcpy(state, src.u8_, 16);
        aes_inv_mix_columns(state);
        std::memcpy(dst.u8_, state, 16);
#endif
        return ExecResult::Continue;
    }

    // ========================================================================
    // AESKEYGENASSIST — AES Key Generation Assist
    // dst[31:0]   = SubWord(src[31:0])
    // dst[63:32]  = RotWord(SubWord(src[63:32])) XOR RCON
    // dst[95:64]  = SubWord(src[95:64])
    // dst[127:96] = RotWord(SubWord(src[127:96])) XOR RCON
    // ========================================================================
    case Opcode::AESKEYGENASSIST_XMM_XMMM128_IMM8: {
        auto src_r = read_pack_c(ctx, op1, aligned_form);
        if (!src_r) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
        auto& dst = get_xmm_c(ctx, op0.reg);
        XmmReg src = src_r.value();
        u8 rcon = static_cast<u8>(instr.operands[2].imm);

        // SubWord: apply S-box to each byte of a dword
        auto sub_word = [](u32 w) -> u32 {
            return static_cast<u32>(aes_sbox[w & 0xFF]) |
                   (static_cast<u32>(aes_sbox[(w >> 8) & 0xFF]) << 8) |
                   (static_cast<u32>(aes_sbox[(w >> 16) & 0xFF]) << 16) |
                   (static_cast<u32>(aes_sbox[(w >> 24) & 0xFF]) << 24);
        };
        // RotWord: [B0,B1,B2,B3] -> [B1,B2,B3,B0]; in little-endian u32 this is a right-rotate by 8
        auto rot_word = [](u32 w) -> u32 {
            return (w >> 8) | (w << 24);
        };

        u32 x1 = sub_word(src.u32_[1]);
        u32 x3 = sub_word(src.u32_[3]);
        dst.u32_[0] = x1;                                    // SubWord(src[63:32])
        dst.u32_[1] = rot_word(x1) ^ static_cast<u32>(rcon);
        dst.u32_[2] = x3;                                    // SubWord(src[127:96])
        dst.u32_[3] = rot_word(x3) ^ static_cast<u32>(rcon);
        VITREO_TRACE("[crypto] AESKEYGENASSIST src=[%08x %08x %08x %08x] rcon=0x%02x dst=[%08x %08x %08x %08x]\n",
            src.u32_[0], src.u32_[1], src.u32_[2], src.u32_[3],
            static_cast<unsigned>(rcon),
            dst.u32_[0], dst.u32_[1], dst.u32_[2], dst.u32_[3]);
        return ExecResult::Continue;
    }

    // ========================================================================
    // PCLMULQDQ — Carry-Less Multiplication of two 64-bit values
    // ========================================================================
    case Opcode::PCLMULQDQ_XMM_XMMM128_IMM8: {
        auto src_r = read_pack_c(ctx, op1, aligned_form);
        if (!src_r) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
        auto& dst = get_xmm_c(ctx, op0.reg);
        XmmReg src = src_r.value();
        u8 imm = static_cast<u8>(instr.operands[2].imm);

        u64 a = dst.u64_[(imm & 0x01) ? 1 : 0];
        u64 b = src.u64_[(imm & 0x10) ? 1 : 0];

        u64 lo, hi;
#ifdef __aarch64__
        static const bool force_sw_clmul = std::getenv("VITREO_SW_PCLMUL") != nullptr;
        if (force_sw_clmul) {
            clmul64(a, b, lo, hi);
        } else {
            arm64_clmul(a, b, lo, hi);
        }
#else
        clmul64(a, b, lo, hi);
#endif
        dst.u64_[0] = lo;
        dst.u64_[1] = hi;
        return ExecResult::Continue;
    }

    // ========================================================================
    // SHA1NEXTE — SHA1 schedule: dst[3] = ROL5(dst[3]) + src[3]; rest = src
    // Actually: SHA1NEXTE(xmm1, xmm2/m128)
    // xmm1[127:96] = ROL(xmm1[127:96], 30) + xmm2[127:96]
    // xmm1[95:0] = xmm2[95:0]
    // ========================================================================
    case Opcode::SHA1NEXTE_XMM_XMMM128: {
        if (diag_logs_enabled()) {
            static std::atomic<uint64_t> ic{0};
            uint64_t c = ic.fetch_add(1);
            if (c < 3 || (c & (c - 1)) == 0) {
                fprintf(stderr, "[interp-sha] SHA1NEXTE count=%lu\n", (unsigned long)(c + 1));
            }
        }
        auto src_r = read_pack_c(ctx, op1, aligned_form);
        if (!src_r) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
        auto& dst = get_xmm_c(ctx, op0.reg);
        XmmReg src = src_r.value();
        u32 e = sha1_rol(dst.u32_[3], 30);
        dst = src;
        dst.u32_[3] += e;
        return ExecResult::Continue;
    }

    // ========================================================================
    // SHA1MSG1 — SHA1 message schedule part 1
    // dst[i] ^= src[i] for the first two dwords, etc.
    // Actually: W[i] = W[i-16] XOR W[i-14]
    // dst = {W0,W1,W2,W3}, src = {W4,W5,W6,W7} → conceptually
    // dst[0] ^= src[0]; dst[1] ^= src[1]; dst[2] ^= src[2]; dst[3] ^= dst[0_orig]
    // Real Intel def:
    //   dst[0] ^= dst[2]; dst[1] ^= dst[3]; dst[2] ^= src[0]; dst[3] ^= src[1]
    // ========================================================================
    case Opcode::SHA1MSG1_XMM_XMMM128: {
        auto src_r = read_pack_c(ctx, op1, aligned_form);
        if (!src_r) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
        auto& dst = get_xmm_c(ctx, op0.reg);
        XmmReg src = src_r.value();
        // SHA1MSG1 — per Intel SDM Vol 2B:
        //   W0 := DST[127:96];  W1 := DST[95:64];  W2 := DST[63:32];  W3 := DST[31:0]
        //   W4 := SRC[127:96];  W5 := SRC[95:64]
        //   DST[127:96] := W2 XOR W0
        //   DST[95:64]  := W3 XOR W1
        //   DST[63:32]  := W4 XOR W2
        //   DST[31:0]   := W5 XOR W3
        // XMM physical layout: u32_[3] is the MSB dword (bits 127:96).
        u32 W0 = dst.u32_[3], W1 = dst.u32_[2], W2 = dst.u32_[1], W3 = dst.u32_[0];
        u32 W4 = src.u32_[3], W5 = src.u32_[2];
        dst.u32_[3] = W2 ^ W0;
        dst.u32_[2] = W3 ^ W1;
        dst.u32_[1] = W4 ^ W2;
        dst.u32_[0] = W5 ^ W3;
        return ExecResult::Continue;
    }

    // ========================================================================
    // SHA1MSG2 — SHA1 message schedule part 2 (ROL1 of XOR)
    // result[i] = ROL1(dst[i] ^ src[i+1]) for i=0..2, result[3] = ROL1(dst[3] ^ result[0])
    // ========================================================================
    case Opcode::SHA1MSG2_XMM_XMMM128: {
        auto src_r = read_pack_c(ctx, op1, aligned_form);
        if (!src_r) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
        auto& dst = get_xmm_c(ctx, op0.reg);
        XmmReg src = src_r.value();
        // SHA1MSG2 — per Intel SDM Vol 2B:
        //   W13 := SRC[95:64]; W14 := SRC[63:32]; W15 := SRC[31:0]
        //   W16 := ROL1(DST[127:96] XOR W13)
        //   W17 := ROL1(DST[95:64]  XOR W14)
        //   W18 := ROL1(DST[63:32]  XOR W15)
        //   W19 := ROL1(DST[31:0]   XOR W16)
        //   DST := {W16, W17, W18, W19}   (W16 goes to [127:96])
        u32 W13 = src.u32_[2], W14 = src.u32_[1], W15 = src.u32_[0];
        u32 W16 = sha1_rol(dst.u32_[3] ^ W13, 1);
        u32 W17 = sha1_rol(dst.u32_[2] ^ W14, 1);
        u32 W18 = sha1_rol(dst.u32_[1] ^ W15, 1);
        u32 W19 = sha1_rol(dst.u32_[0] ^ W16, 1);
        dst.u32_[3] = W16;
        dst.u32_[2] = W17;
        dst.u32_[1] = W18;
        dst.u32_[0] = W19;
        return ExecResult::Continue;
    }

    // ========================================================================
    // SHA1RNDS4 — Perform four SHA1 rounds
    // imm8[1:0] selects the SHA1 function (0=Ch, 1=Parity, 2=Maj, 3=Parity)
    // dst = {A,B,C,D}, src = {next four message schedule + K values}
    // ========================================================================
    case Opcode::SHA1RNDS4_XMM_XMMM128_IMM8: {
        auto src_r = read_pack_c(ctx, op1, aligned_form);
        if (!src_r) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
        auto& dst = get_xmm_c(ctx, op0.reg);
        XmmReg src = src_r.value();
        u8 fn = static_cast<u8>(instr.operands[2].imm) & 3;

        // Per Intel SDM:
        //   Round 0: A_1 = f(B,C,D) + ROL(A,5) + W0E + K   (W0E already has E folded in via SHA1NEXTE)
        //   Round i>=1: A_(i+1) = f + ROL(A_i,5) + W_i + E_i + K, with E_(i+1) := D_i
        // The instruction ALWAYS adds a K constant selected by imm8[1:0].
        static const u32 K_CONST[4] = {
            0x5A827999u, 0x6ED9EBA1u, 0x8F1BBCDCu, 0xCA62C1D6u
        };
        // dst = {A, B, C, D} stored as u32_[3]=A, u32_[2]=B, u32_[1]=C, u32_[0]=D
        u32 a = dst.u32_[3], b = dst.u32_[2], c = dst.u32_[1], d = dst.u32_[0];
        u32 e_carry = d; // E for round 1 = original D (unused in round 0)
        const u32 k = K_CONST[fn];

        for (int i = 0; i < 4; ++i) {
            u32 f;
            switch (fn) {
                case 0: f = (b & c) ^ (~b & d); break;           // Ch
                case 1: f = b ^ c ^ d; break;                     // Parity
                case 2: f = (b & c) ^ (b & d) ^ (c & d); break;  // Maj
                default: f = b ^ c ^ d; break;                    // Parity
            }
            u32 w = src.u32_[3 - i];
            u32 temp = sha1_rol(a, 5) + f + w + k + (i == 0 ? 0u : e_carry);
            u32 new_d = c;
            u32 new_c = sha1_rol(b, 30);
            u32 new_b = a;
            u32 new_a = temp;
            e_carry = d; // E_(i+1) = D_i (pre-update)
            a = new_a; b = new_b; c = new_c; d = new_d;
        }
        dst.u32_[3] = a; dst.u32_[2] = b; dst.u32_[1] = c; dst.u32_[0] = d;
        return ExecResult::Continue;
    }

    // ========================================================================
    // SHA256RNDS2 — Two rounds of SHA-256 (implicit XMM0 for W+K)
    // dst = {C,D,G,H}, src = {A,B,E,F}, XMM0 = {-,-,WK1,WK0}
    // ========================================================================
    case Opcode::SHA256RNDS2_XMM_XMMM128: {
        if (diag_logs_enabled()) {
            static std::atomic<uint64_t> ic{0};
            uint64_t c = ic.fetch_add(1);
            if (c < 3 || (c & (c - 1)) == 0) {
                fprintf(stderr, "[interp-sha] SHA256RNDS2 count=%lu\n", (unsigned long)(c + 1));
            }
        }
        auto src_r = read_pack_c(ctx, op1, aligned_form);
        if (!src_r) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
        auto& dst = get_xmm_c(ctx, op0.reg);
        XmmReg src = src_r.value();
        XmmReg& xmm0 = ctx.cpu.sse.xmm[0]; // implicit operand

        // Intel layout: dst={C,D,G,H}, src={A,B,E,F}
        // u32_[3]=first, u32_[0]=last
        u32 a = src.u32_[3], b = src.u32_[2], e = src.u32_[1], f = src.u32_[0];
        u32 c = dst.u32_[3], d = dst.u32_[2], g = dst.u32_[1], h = dst.u32_[0];

        // Round 0 with wk = xmm0.u32_[0]
        for (int i = 0; i < 2; ++i) {
            u32 wk = xmm0.u32_[i];
            u32 t1 = h + sha256_sigma1(e) + sha256_ch(e, f, g) + wk;
            u32 t2 = sha256_sigma0(a) + sha256_maj(a, b, c);
            h = g; g = f; f = e; e = d + t1;
            d = c; c = b; b = a; a = t1 + t2;
        }

        // Output: new {A,B,E,F} in dst
        dst.u32_[3] = a; dst.u32_[2] = b; dst.u32_[1] = e; dst.u32_[0] = f;
        return ExecResult::Continue;
    }

    // ========================================================================
    // SHA256MSG1 — SHA-256 message schedule σ0
    // dst[i] += σ0(src_or_derived)
    // ========================================================================
    case Opcode::SHA256MSG1_XMM_XMMM128: {
        auto src_r = read_pack_c(ctx, op1, aligned_form);
        if (!src_r) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
        auto& dst = get_xmm_c(ctx, op0.reg);
        XmmReg src = src_r.value();
        // dst = {W3, W2, W1, W0}, src = {W7, W6, W5, W4}
        // result[i] = dst[i] + σ0(next_dword)
        // where next dwords are: dst[1], dst[2], dst[3], src[0]
#ifdef __aarch64__
        arm64_sha256_msg1(dst.u32_, src.u32_);
#else
        dst.u32_[0] += sha256_sigma_small0(dst.u32_[1]);
        dst.u32_[1] += sha256_sigma_small0(dst.u32_[2]);
        dst.u32_[2] += sha256_sigma_small0(dst.u32_[3]);
        dst.u32_[3] += sha256_sigma_small0(src.u32_[0]);
#endif
        return ExecResult::Continue;
    }

    // ========================================================================
    // SHA256MSG2 — SHA-256 message schedule σ1
    // ========================================================================
    case Opcode::SHA256MSG2_XMM_XMMM128: {
        auto src_r = read_pack_c(ctx, op1, aligned_form);
        if (!src_r) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
        auto& dst = get_xmm_c(ctx, op0.reg);
        XmmReg src = src_r.value();
        // dst = {W[i-16+σ0], ...}, src = {W[i-2], W[i-3], ...}
        // result[0] = dst[0] + σ1(src[2])
        // result[1] = dst[1] + σ1(src[3])
        // result[2] = dst[2] + σ1(result[0])
        // result[3] = dst[3] + σ1(result[1])
        dst.u32_[0] += sha256_sigma_small1(src.u32_[2]);
        dst.u32_[1] += sha256_sigma_small1(src.u32_[3]);
        dst.u32_[2] += sha256_sigma_small1(dst.u32_[0]);
        dst.u32_[3] += sha256_sigma_small1(dst.u32_[1]);
        return ExecResult::Continue;
    }

    // ========================================================================
    // SSSE3: PHADDW / PHADDD / PHADDSW — horizontal pairwise add
    // dst lane order (16-bit): [d0+d1, d2+d3, d4+d5, d6+d7, s0+s1, s2+s3, s4+s5, s6+s7]
    // ========================================================================
    case Opcode::PHADDW_XMM_XMMM128: {
        auto src_r = read_pack_c(ctx, op1, aligned_form);
        if (!src_r) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
        auto& dst = get_xmm_c(ctx, op0.reg);
        XmmReg src = src_r.value();
        i16 r[8];
        for (int i = 0; i < 4; ++i)
            r[i] = static_cast<i16>(static_cast<i32>(dst.i16_[2*i]) + static_cast<i32>(dst.i16_[2*i + 1]));
        for (int i = 0; i < 4; ++i)
            r[i + 4] = static_cast<i16>(static_cast<i32>(src.i16_[2*i]) + static_cast<i32>(src.i16_[2*i + 1]));
        for (int i = 0; i < 8; ++i) dst.i16_[i] = r[i];
        return ExecResult::Continue;
    }
    case Opcode::PHADDD_XMM_XMMM128: {
        auto src_r = read_pack_c(ctx, op1, aligned_form);
        if (!src_r) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
        auto& dst = get_xmm_c(ctx, op0.reg);
        XmmReg src = src_r.value();
        i32 r[4];
        r[0] = static_cast<i32>(static_cast<u32>(dst.i32_[0]) + static_cast<u32>(dst.i32_[1]));
        r[1] = static_cast<i32>(static_cast<u32>(dst.i32_[2]) + static_cast<u32>(dst.i32_[3]));
        r[2] = static_cast<i32>(static_cast<u32>(src.i32_[0]) + static_cast<u32>(src.i32_[1]));
        r[3] = static_cast<i32>(static_cast<u32>(src.i32_[2]) + static_cast<u32>(src.i32_[3]));
        for (int i = 0; i < 4; ++i) dst.i32_[i] = r[i];
        return ExecResult::Continue;
    }
    case Opcode::PHADDSW_XMM_XMMM128: {
        auto src_r = read_pack_c(ctx, op1, aligned_form);
        if (!src_r) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
        auto& dst = get_xmm_c(ctx, op0.reg);
        XmmReg src = src_r.value();
        auto sat = [](i32 v) -> i16 {
            if (v > 32767) return 32767; if (v < -32768) return -32768; return static_cast<i16>(v);
        };
        i16 r[8];
        for (int i = 0; i < 4; ++i)
            r[i] = sat(static_cast<i32>(dst.i16_[2*i]) + static_cast<i32>(dst.i16_[2*i + 1]));
        for (int i = 0; i < 4; ++i)
            r[i + 4] = sat(static_cast<i32>(src.i16_[2*i]) + static_cast<i32>(src.i16_[2*i + 1]));
        for (int i = 0; i < 8; ++i) dst.i16_[i] = r[i];
        return ExecResult::Continue;
    }

    // ========================================================================
    // PHSUBW / PHSUBD / PHSUBSW — horizontal pairwise subtract (a[2i] - a[2i+1])
    // ========================================================================
    case Opcode::PHSUBW_XMM_XMMM128: {
        auto src_r = read_pack_c(ctx, op1, aligned_form);
        if (!src_r) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
        auto& dst = get_xmm_c(ctx, op0.reg);
        XmmReg src = src_r.value();
        i16 r[8];
        for (int i = 0; i < 4; ++i)
            r[i] = static_cast<i16>(static_cast<i32>(dst.i16_[2*i]) - static_cast<i32>(dst.i16_[2*i + 1]));
        for (int i = 0; i < 4; ++i)
            r[i + 4] = static_cast<i16>(static_cast<i32>(src.i16_[2*i]) - static_cast<i32>(src.i16_[2*i + 1]));
        for (int i = 0; i < 8; ++i) dst.i16_[i] = r[i];
        return ExecResult::Continue;
    }
    case Opcode::PHSUBD_XMM_XMMM128: {
        auto src_r = read_pack_c(ctx, op1, aligned_form);
        if (!src_r) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
        auto& dst = get_xmm_c(ctx, op0.reg);
        XmmReg src = src_r.value();
        i32 r[4];
        r[0] = static_cast<i32>(static_cast<u32>(dst.i32_[0]) - static_cast<u32>(dst.i32_[1]));
        r[1] = static_cast<i32>(static_cast<u32>(dst.i32_[2]) - static_cast<u32>(dst.i32_[3]));
        r[2] = static_cast<i32>(static_cast<u32>(src.i32_[0]) - static_cast<u32>(src.i32_[1]));
        r[3] = static_cast<i32>(static_cast<u32>(src.i32_[2]) - static_cast<u32>(src.i32_[3]));
        for (int i = 0; i < 4; ++i) dst.i32_[i] = r[i];
        return ExecResult::Continue;
    }
    case Opcode::PHSUBSW_XMM_XMMM128: {
        auto src_r = read_pack_c(ctx, op1, aligned_form);
        if (!src_r) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
        auto& dst = get_xmm_c(ctx, op0.reg);
        XmmReg src = src_r.value();
        auto sat = [](i32 v) -> i16 {
            if (v > 32767) return 32767; if (v < -32768) return -32768; return static_cast<i16>(v);
        };
        i16 r[8];
        for (int i = 0; i < 4; ++i)
            r[i] = sat(static_cast<i32>(dst.i16_[2*i]) - static_cast<i32>(dst.i16_[2*i + 1]));
        for (int i = 0; i < 4; ++i)
            r[i + 4] = sat(static_cast<i32>(src.i16_[2*i]) - static_cast<i32>(src.i16_[2*i + 1]));
        for (int i = 0; i < 8; ++i) dst.i16_[i] = r[i];
        return ExecResult::Continue;
    }

    // ========================================================================
    // PMADDUBSW — multiply unsigned bytes (dst) by signed bytes (src),
    //   horizontal saturating add of adjacent pairs into signed words
    // ========================================================================
    case Opcode::PMADDUBSW_XMM_XMMM128: {
        auto src_r = read_pack_c(ctx, op1, aligned_form);
        if (!src_r) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
        auto& dst = get_xmm_c(ctx, op0.reg);
        XmmReg src = src_r.value();
        for (int i = 0; i < 8; ++i) {
            i32 a = static_cast<i32>(dst.u8_[2*i])     * static_cast<i32>(src.i8_[2*i]);
            i32 b = static_cast<i32>(dst.u8_[2*i + 1]) * static_cast<i32>(src.i8_[2*i + 1]);
            i32 sum = a + b;
            if (sum > 32767) sum = 32767; else if (sum < -32768) sum = -32768;
            dst.i16_[i] = static_cast<i16>(sum);
        }
        return ExecResult::Continue;
    }

    // ========================================================================
    // PSIGNB / PSIGNW / PSIGND — apply sign of src to dst lane
    //   if src<0: dst = -dst; if src==0: dst = 0; if src>0: dst unchanged
    // ========================================================================
    case Opcode::PSIGNB_XMM_XMMM128: {
        auto src_r = read_pack_c(ctx, op1, aligned_form);
        if (!src_r) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
        auto& dst = get_xmm_c(ctx, op0.reg);
        XmmReg src = src_r.value();
        for (int i = 0; i < 16; ++i) {
            i8 s = src.i8_[i];
            if (s < 0) dst.i8_[i] = static_cast<i8>(-static_cast<i32>(dst.i8_[i]));
            else if (s == 0) dst.i8_[i] = 0;
        }
        return ExecResult::Continue;
    }
    case Opcode::PSIGNW_XMM_XMMM128: {
        auto src_r = read_pack_c(ctx, op1, aligned_form);
        if (!src_r) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
        auto& dst = get_xmm_c(ctx, op0.reg);
        XmmReg src = src_r.value();
        for (int i = 0; i < 8; ++i) {
            i16 s = src.i16_[i];
            if (s < 0) dst.i16_[i] = static_cast<i16>(-static_cast<i32>(dst.i16_[i]));
            else if (s == 0) dst.i16_[i] = 0;
        }
        return ExecResult::Continue;
    }
    case Opcode::PSIGND_XMM_XMMM128: {
        auto src_r = read_pack_c(ctx, op1, aligned_form);
        if (!src_r) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
        auto& dst = get_xmm_c(ctx, op0.reg);
        XmmReg src = src_r.value();
        for (int i = 0; i < 4; ++i) {
            i32 s = dst.i32_[i]; (void)s; // silence unused if compiler optimizes
            i32 sg = src.i32_[i];
            if (sg < 0) dst.i32_[i] = static_cast<i32>(-static_cast<i64>(dst.i32_[i]));
            else if (sg == 0) dst.i32_[i] = 0;
        }
        return ExecResult::Continue;
    }

    // ========================================================================
    // PMULHRSW — packed multiply high with round and scale
    //   tmp = (a * b) >> 14; r = (tmp + 1) >> 1; per 16-bit lane
    // ========================================================================
    case Opcode::PMULHRSW_XMM_XMMM128: {
        auto src_r = read_pack_c(ctx, op1, aligned_form);
        if (!src_r) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
        auto& dst = get_xmm_c(ctx, op0.reg);
        XmmReg src = src_r.value();
        for (int i = 0; i < 8; ++i) {
            i32 prod = static_cast<i32>(dst.i16_[i]) * static_cast<i32>(src.i16_[i]);
            i32 r = ((prod >> 14) + 1) >> 1;
            dst.i16_[i] = static_cast<i16>(r & 0xFFFF);
        }
        return ExecResult::Continue;
    }

    // ========================================================================
    // PABSB / PABSW / PABSD — absolute value (INT_MIN -> INT_MIN per SDM)
    // ========================================================================
    case Opcode::PABSB_XMM_XMMM128: {
        auto src_r = read_pack_c(ctx, op1, aligned_form);
        if (!src_r) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
        auto& dst = get_xmm_c(ctx, op0.reg);
        XmmReg src = src_r.value();
        for (int i = 0; i < 16; ++i) {
            i8 v = src.i8_[i];
            dst.i8_[i] = (v < 0) ? static_cast<i8>(-static_cast<i32>(v)) : v;
        }
        return ExecResult::Continue;
    }
    case Opcode::PABSW_XMM_XMMM128: {
        auto src_r = read_pack_c(ctx, op1, aligned_form);
        if (!src_r) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
        auto& dst = get_xmm_c(ctx, op0.reg);
        XmmReg src = src_r.value();
        for (int i = 0; i < 8; ++i) {
            i16 v = src.i16_[i];
            dst.i16_[i] = (v < 0) ? static_cast<i16>(-static_cast<i32>(v)) : v;
        }
        return ExecResult::Continue;
    }
    case Opcode::PABSD_XMM_XMMM128: {
        auto src_r = read_pack_c(ctx, op1, aligned_form);
        if (!src_r) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
        auto& dst = get_xmm_c(ctx, op0.reg);
        XmmReg src = src_r.value();
        for (int i = 0; i < 4; ++i) {
            i32 v = src.i32_[i];
            dst.i32_[i] = (v < 0) ? static_cast<i32>(-static_cast<i64>(v)) : v;
        }
        return ExecResult::Continue;
    }

    // ========================================================================
    // SSE4.1 PMULDQ — signed multiply low 32-bit elements of each 64-bit lane
    //   dst[0..63]  = (i64)(i32)dst[0..31]  * (i64)(i32)src[0..31]
    //   dst[64..127]= (i64)(i32)dst[64..95] * (i64)(i32)src[64..95]
    // ========================================================================
    case Opcode::PMULDQ_XMM_XMMM128: {
        auto src_r = read_pack_c(ctx, op1, aligned_form);
        if (!src_r) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
        auto& dst = get_xmm_c(ctx, op0.reg);
        XmmReg src = src_r.value();
        i64 lo = static_cast<i64>(dst.i32_[0]) * static_cast<i64>(src.i32_[0]);
        i64 hi = static_cast<i64>(dst.i32_[2]) * static_cast<i64>(src.i32_[2]);
        dst.u64_[0] = static_cast<u64>(lo);
        dst.u64_[1] = static_cast<u64>(hi);
        return ExecResult::Continue;
    }

    default:
        ctx.exception = CpuException::InvalidOpcode;
        return ExecResult::Exception;
    }
}

} // namespace vitreo::cpu::ops
