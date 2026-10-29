// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64

#pragma once

#include "vitreo/types.hpp"
#include <cstring>

namespace vitreo::cpu {

/// Register indices for the 16 general-purpose registers
enum class RegIndex : u8 {
    RAX = 0, RCX = 1, RDX = 2, RBX = 3,
    RSP = 4, RBP = 5, RSI = 6, RDI = 7,
    R8  = 8, R9  = 9, R10 = 10, R11 = 11,
    R12 = 12, R13 = 13, R14 = 14, R15 = 15,
    
    // Aliases for 32-bit mode
    EAX = RAX, ECX = RCX, EDX = RDX, EBX = RBX,
    ESP = RSP, EBP = RBP, ESI = RSI, EDI = RDI,
};

/// 8-bit register encoding (varies between legacy and REX modes)
enum class Reg8Index : u8 {
    // Legacy encoding (no REX)
    AL = 0, CL = 1, DL = 2, BL = 3,
    AH = 4, CH = 5, DH = 6, BH = 7,
    
    // REX encoding (REX.R/REX.B set)
    // AL-BL same, but 4-7 become SPL, BPL, SIL, DIL
    SPL = 4, BPL = 5, SIL = 6, DIL = 7,
    R8B = 8, R9B = 9, R10B = 10, R11B = 11,
    R12B = 12, R13B = 13, R14B = 14, R15B = 15,
};

/// General-purpose registers (64-bit)
struct GeneralRegs64 {
    union {
        u64 r[16];
        struct {
            u64 rax, rcx, rdx, rbx;
            u64 rsp, rbp, rsi, rdi;
            u64 r8, r9, r10, r11;
            u64 r12, r13, r14, r15;
        };
    };
    
    constexpr GeneralRegs64() noexcept : r{} {}
    
    /// Get 64-bit register by index
    [[nodiscard]] u64 get64(u8 index) const noexcept {
        return r[index & 0x0F];
    }
    
    /// Set 64-bit register by index
    void set64(u8 index, u64 value) noexcept {
        r[index & 0x0F] = value;
    }
    
    /// Get 32-bit register by index (zero-extended read, clear upper 32 bits on write)
    [[nodiscard]] u32 get32(u8 index) const noexcept {
        return static_cast<u32>(r[index & 0x0F]);
    }
    
    /// Set 32-bit register by index (clears upper 32 bits)
    void set32(u8 index, u32 value) noexcept {
        r[index & 0x0F] = value;  // Zero-extends to 64 bits
    }
    
    /// Get 16-bit register by index
    [[nodiscard]] u16 get16(u8 index) const noexcept {
        return static_cast<u16>(r[index & 0x0F]);
    }
    
    /// Set 16-bit register by index (preserves upper 48 bits)
    void set16(u8 index, u16 value) noexcept {
        r[index & 0x0F] = (r[index & 0x0F] & 0xFFFFFFFFFFFF0000ULL) | value;
    }
    
    /// Get 8-bit low register by index (AL, CL, etc.)
    [[nodiscard]] u8 get8l(u8 index) const noexcept {
        return static_cast<u8>(r[index & 0x0F]);
    }
    
    /// Set 8-bit low register by index (preserves upper 56 bits)
    void set8l(u8 index, u8 value) noexcept {
        r[index & 0x0F] = (r[index & 0x0F] & 0xFFFFFFFFFFFFFF00ULL) | value;
    }
    
    /// Get 8-bit high register (AH, CH, DH, BH) - only valid for index 0-3
    [[nodiscard]] u8 get8h(u8 index) const noexcept {
        return static_cast<u8>(r[index & 0x03] >> 8);
    }
    
    /// Set 8-bit high register (preserves other bits)
    void set8h(u8 index, u8 value) noexcept {
        u8 idx = index & 0x03;
        r[idx] = (r[idx] & 0xFFFFFFFFFFFF00FFULL) | (static_cast<u64>(value) << 8);
    }
};

/// General-purpose registers (32-bit mode view)
struct GeneralRegs32 {
    union {
        u32 r[8];
        struct {
            u32 eax, ecx, edx, ebx;
            u32 esp, ebp, esi, edi;
        };
    };
    
    constexpr GeneralRegs32() noexcept : r{} {}
    
    /// Get 32-bit register by index
    [[nodiscard]] u32 get32(u8 index) const noexcept {
        return r[index & 0x07];
    }
    
    /// Set 32-bit register by index
    void set32(u8 index, u32 value) noexcept {
        r[index & 0x07] = value;
    }
    
    /// Get 16-bit register by index
    [[nodiscard]] u16 get16(u8 index) const noexcept {
        return static_cast<u16>(r[index & 0x07]);
    }
    
    /// Set 16-bit register by index (preserves upper 16 bits)
    void set16(u8 index, u16 value) noexcept {
        r[index & 0x07] = (r[index & 0x07] & 0xFFFF0000U) | value;
    }
    
    /// Get 8-bit low register by index
    [[nodiscard]] u8 get8l(u8 index) const noexcept {
        return static_cast<u8>(r[index & 0x07]);
    }
    
    /// Set 8-bit low register by index
    void set8l(u8 index, u8 value) noexcept {
        r[index & 0x07] = (r[index & 0x07] & 0xFFFFFF00U) | value;
    }
    
    /// Get 8-bit high register (AH, CH, DH, BH)
    [[nodiscard]] u8 get8h(u8 index) const noexcept {
        return static_cast<u8>(r[index & 0x03] >> 8);
    }
    
    /// Set 8-bit high register
    void set8h(u8 index, u8 value) noexcept {
        u8 idx = index & 0x03;
        r[idx] = (r[idx] & 0xFFFF00FFU) | (static_cast<u32>(value) << 8);
    }
};

static_assert(sizeof(GeneralRegs64) == 128, "GeneralRegs64 must be 128 bytes");
static_assert(sizeof(GeneralRegs32) == 32, "GeneralRegs32 must be 32 bytes");

} // namespace vitreo::cpu
