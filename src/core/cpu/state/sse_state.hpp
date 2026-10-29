// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64

#pragma once

#include "vitreo/types.hpp"
#include <cstring>

namespace vitreo::cpu {

/// MXCSR bits
enum MxcsrBits : u32 {
    MXCSR_IE  = 1 << 0,   // Invalid operation exception flag
    MXCSR_DE  = 1 << 1,   // Denormal exception flag
    MXCSR_ZE  = 1 << 2,   // Divide-by-zero exception flag
    MXCSR_OE  = 1 << 3,   // Overflow exception flag
    MXCSR_UE  = 1 << 4,   // Underflow exception flag
    MXCSR_PE  = 1 << 5,   // Precision exception flag
    MXCSR_DAZ = 1 << 6,   // Denormals are zeros
    MXCSR_IM  = 1 << 7,   // Invalid operation mask
    MXCSR_DM  = 1 << 8,   // Denormal mask
    MXCSR_ZM  = 1 << 9,   // Divide-by-zero mask
    MXCSR_OM  = 1 << 10,  // Overflow mask
    MXCSR_UM  = 1 << 11,  // Underflow mask
    MXCSR_PM  = 1 << 12,  // Precision mask
    MXCSR_RC_MASK = 3 << 13,  // Rounding control
    MXCSR_FTZ = 1 << 15,  // Flush to zero
};

/// SSE rounding control values
enum class SseRounding : u8 {
    Nearest = 0,    // Round to nearest (even)
    Down = 1,       // Round toward -infinity
    Up = 2,         // Round toward +infinity
    Truncate = 3,   // Round toward zero
};

/// 128-bit XMM register
struct alignas(16) XmmReg {
    union {
        u8  u8_[16];
        i8  i8_[16];
        u16 u16_[8];
        i16 i16_[8];
        u32 u32_[4];
        i32 i32_[4];
        u64 u64_[2];
        i64 i64_[2];
        f32 f32_[4];
        f64 f64_[2];
    };
    
    constexpr XmmReg() noexcept : u64_{0, 0} {}
    
    /// Clear to zero
    void clear() noexcept {
        u64_[0] = 0;
        u64_[1] = 0;
    }
    
    /// Set from raw bytes
    void set_bytes(const void* src) noexcept {
        std::memcpy(this, src, 16);
    }
    
    /// Get as raw bytes
    void get_bytes(void* dst) const noexcept {
        std::memcpy(dst, this, 16);
    }
};

static_assert(sizeof(XmmReg) == 16, "XmmReg must be 16 bytes");
static_assert(alignof(XmmReg) == 16, "XmmReg must be 16-byte aligned");

/// 256-bit YMM register (AVX)
struct alignas(32) YmmReg {
    union {
        u8  u8_[32];
        i8  i8_[32];
        u16 u16_[16];
        i16 i16_[16];
        u32 u32_[8];
        i32 i32_[8];
        u64 u64_[4];
        i64 i64_[4];
        f32 f32_[8];
        f64 f64_[4];
        XmmReg xmm[2];  // Low and high halves
    };
    
    constexpr YmmReg() noexcept : u64_{0, 0, 0, 0} {}
    
    /// Get lower 128 bits (XMM view)
    [[nodiscard]] XmmReg& low() noexcept { return xmm[0]; }
    [[nodiscard]] const XmmReg& low() const noexcept { return xmm[0]; }
    
    /// Get upper 128 bits
    [[nodiscard]] XmmReg& high() noexcept { return xmm[1]; }
    [[nodiscard]] const XmmReg& high() const noexcept { return xmm[1]; }
    
    /// Clear to zero
    void clear() noexcept {
        u64_[0] = u64_[1] = u64_[2] = u64_[3] = 0;
    }
    
    /// Clear upper 128 bits (for 128-bit operations in AVX mode)
    void clear_high() noexcept {
        u64_[2] = u64_[3] = 0;
    }
};

static_assert(sizeof(YmmReg) == 32, "YmmReg must be 32 bytes");
static_assert(alignof(YmmReg) == 32, "YmmReg must be 32-byte aligned");

/// SSE state (XMM0-XMM15 + MXCSR)
struct SseState {
    XmmReg xmm[16];     // XMM0-XMM15
    u32 mxcsr;          // MXCSR register
    u32 mxcsr_mask;     // MXCSR mask (supported bits)

    /// 32-byte staging area owned by the JIT, not architectural guest state.
    ///
    /// emit_x64_region_load128/store128 need somewhere to assemble a 128-bit
    /// value when the region probe misses and the access has to go through the
    /// 8-byte C helpers twice. The halves cannot be parked in registers across
    /// those calls: every caller-saved GPR is clobbered by the helper, X21 is
    /// reserved (fpu.cpp parks an 80-bit mantissa in it across guest loads),
    /// and X12/X13/X19/X20/X22-X28 are all live block state. Live only within
    /// one emitted access, never read by the interpreter, and deliberately
    /// last so no xmm/mxcsr offset moves.
    ///
    /// Layout while a 128-bit slow path runs:
    ///   [ 0..15] the 128-bit value being assembled (load) or written (store)
    ///   [16..23] the low half's guest address
    ///   [24..31] the high half's guest address (= low + 8)
    /// Two slots rather than one because a STORE needs the value and both
    /// addresses live at the same time.
    alignas(16) XmmReg jit_vec_scratch[2];

    constexpr SseState() noexcept 
        : xmm{}, mxcsr(0x1F80), mxcsr_mask(0xFFFF) {}
    
    /// Get rounding control
    [[nodiscard]] SseRounding rounding() const noexcept {
        return static_cast<SseRounding>((mxcsr >> 13) & 3);
    }
    
    /// Set rounding control
    void set_rounding(SseRounding rc) noexcept {
        mxcsr = (mxcsr & ~MXCSR_RC_MASK) | (static_cast<u32>(rc) << 13);
    }
    
    /// Check if exceptions are masked
    [[nodiscard]] bool exceptions_masked() const noexcept {
        return (mxcsr & (MXCSR_IM | MXCSR_DM | MXCSR_ZM | 
                         MXCSR_OM | MXCSR_UM | MXCSR_PM)) == 
               (MXCSR_IM | MXCSR_DM | MXCSR_ZM | 
                MXCSR_OM | MXCSR_UM | MXCSR_PM);
    }
    
    /// Clear exception flags
    void clear_exceptions() noexcept {
        mxcsr &= ~(MXCSR_IE | MXCSR_DE | MXCSR_ZE | 
                   MXCSR_OE | MXCSR_UE | MXCSR_PE);
    }
    
    /// Reset to initial state
    void reset() noexcept {
        for (auto& reg : xmm) {
            reg.clear();
        }
        mxcsr = 0x1F80;  // Default: all exceptions masked
    }
};

/// AVX state (upper halves of YMM0-YMM15)
struct AvxState {
    alignas(16) XmmReg ymm_high[16];  // Upper 128 bits of YMM0-YMM15
    
    constexpr AvxState() noexcept : ymm_high{} {}
    
    /// Clear all upper halves (VZEROUPPER/VZEROALL)
    void clear() noexcept {
        for (auto& reg : ymm_high) {
            reg.clear();
        }
    }
};

} // namespace vitreo::cpu
