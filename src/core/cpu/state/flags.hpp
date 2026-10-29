// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64

#pragma once

#include "vitreo/types.hpp"

namespace vitreo::cpu {

/// RFLAGS/EFLAGS bit positions
enum FlagBits : u64 {
    FLAG_CF = 1ULL << 0,   // Carry flag
    FLAG_PF = 1ULL << 2,   // Parity flag
    FLAG_AF = 1ULL << 4,   // Auxiliary carry flag
    FLAG_ZF = 1ULL << 6,   // Zero flag
    FLAG_SF = 1ULL << 7,   // Sign flag
    FLAG_TF = 1ULL << 8,   // Trap flag (single step)
    FLAG_IF = 1ULL << 9,   // Interrupt enable flag
    FLAG_DF = 1ULL << 10,  // Direction flag
    FLAG_OF = 1ULL << 11,  // Overflow flag
    FLAG_IOPL_MASK = 3ULL << 12,  // I/O privilege level
    FLAG_NT = 1ULL << 14,  // Nested task flag
    FLAG_RF = 1ULL << 16,  // Resume flag
    FLAG_VM = 1ULL << 17,  // Virtual 8086 mode
    FLAG_AC = 1ULL << 18,  // Alignment check
    FLAG_VIF = 1ULL << 19, // Virtual interrupt flag
    FLAG_VIP = 1ULL << 20, // Virtual interrupt pending
    FLAG_ID = 1ULL << 21,  // Identification flag
};

/// Status flags that change based on arithmetic results
constexpr u64 STATUS_FLAGS_MASK = FLAG_CF | FLAG_PF | FLAG_AF | FLAG_ZF | FLAG_SF | FLAG_OF;

/// CPU flags register
struct Flags {
    u64 rflags;
    
    constexpr Flags() noexcept : rflags(0x0002) {}  // Bit 1 is always set
    explicit constexpr Flags(u64 value) noexcept : rflags(value | 0x0002) {}
    
    // Status flag accessors
    [[nodiscard]] bool cf() const noexcept { return (rflags & FLAG_CF) != 0; }
    [[nodiscard]] bool pf() const noexcept { return (rflags & FLAG_PF) != 0; }
    [[nodiscard]] bool af() const noexcept { return (rflags & FLAG_AF) != 0; }
    [[nodiscard]] bool zf() const noexcept { return (rflags & FLAG_ZF) != 0; }
    [[nodiscard]] bool sf() const noexcept { return (rflags & FLAG_SF) != 0; }
    [[nodiscard]] bool of() const noexcept { return (rflags & FLAG_OF) != 0; }
    
    // Control flag accessors
    [[nodiscard]] bool df() const noexcept { return (rflags & FLAG_DF) != 0; }
    [[nodiscard]] bool tf() const noexcept { return (rflags & FLAG_TF) != 0; }
    [[nodiscard]] bool iflag() const noexcept { return (rflags & FLAG_IF) != 0; }
    
    // Status flag setters (branchless: single load + single store, no
    // conditional branch).  Hot path during flag materialization.
    void set_cf(bool v) noexcept { rflags = (rflags & ~FLAG_CF) | (u64(v) << 0); }
    void set_pf(bool v) noexcept { rflags = (rflags & ~FLAG_PF) | (u64(v) << 2); }
    void set_af(bool v) noexcept { rflags = (rflags & ~FLAG_AF) | (u64(v) << 4); }
    void set_zf(bool v) noexcept { rflags = (rflags & ~FLAG_ZF) | (u64(v) << 6); }
    void set_sf(bool v) noexcept { rflags = (rflags & ~FLAG_SF) | (u64(v) << 7); }
    void set_of(bool v) noexcept { rflags = (rflags & ~FLAG_OF) | (u64(v) << 11); }
    
    // Control flag setters
    void set_df(bool v) noexcept { if (v) rflags |= FLAG_DF; else rflags &= ~FLAG_DF; }
    void set_tf(bool v) noexcept { if (v) rflags |= FLAG_TF; else rflags &= ~FLAG_TF; }
    void set_if(bool v) noexcept { if (v) rflags |= FLAG_IF; else rflags &= ~FLAG_IF; }
    
    // Clear all status flags
    void clear_status() noexcept { rflags &= ~STATUS_FLAGS_MASK; }
    
    // Get 32-bit EFLAGS value
    [[nodiscard]] u32 eflags() const noexcept { return static_cast<u32>(rflags); }
    
    /// Compute parity flag for a byte - true if even number of 1 bits
    [[nodiscard]] static bool compute_parity(u8 value) noexcept {
        // Count bits, parity flag is set if even number of 1 bits
        u8 v = value;
        v ^= v >> 4;
        v ^= v >> 2;
        v ^= v >> 1;
        return (v & 1) == 0;
    }
    
    /// Update ZF, SF, PF based on result value
    template<typename T>
    void update_zsp(T result) noexcept {
        static_assert(sizeof(T) <= 8, "Result must be at most 64 bits");
        // Branchless: compute all three bits, then mask+OR rflags once.
        constexpr u64 ZSP_MASK = FLAG_ZF | FLAG_SF | FLAG_PF;
        u64 zf = u64(result == 0) << 6;
        u64 sf = u64((result >> (sizeof(T) * 8 - 1)) & 1) << 7;
        u64 pf = u64(compute_parity(static_cast<u8>(result))) << 2;
        rflags = (rflags & ~ZSP_MASK) | zf | sf | pf;
    }

    /// Update all arithmetic flags for ADD operation
    template<typename T>
    void update_flags_add(T a, T b, T result) noexcept {
        // Branchless: compute every status bit, then perform a single
        // mask+OR on rflags.  Each set_xx call would otherwise be a
        // conditional load-modify-store; this collapses to one store.
        constexpr T sign_bit = static_cast<T>(1) << (sizeof(T) * 8 - 1);
        u64 zf = u64(result == 0) << 6;
        u64 sf = u64((result >> (sizeof(T) * 8 - 1)) & 1) << 7;
        u64 pf = u64(compute_parity(static_cast<u8>(result))) << 2;
        u64 cf = u64(result < a) << 0;                                  // unsigned overflow
        u64 of = u64((((a ^ result) & (b ^ result) & sign_bit)) != 0) << 11;
        u64 af = u64((((a ^ b ^ result) & 0x10)) != 0) << 4;
        rflags = (rflags & ~STATUS_FLAGS_MASK) | cf | pf | af | zf | sf | of;
    }

    /// Update all arithmetic flags for SUB operation
    template<typename T>
    void update_flags_sub(T a, T b, T result) noexcept {
        constexpr T sign_bit = static_cast<T>(1) << (sizeof(T) * 8 - 1);
        u64 zf = u64(result == 0) << 6;
        u64 sf = u64((result >> (sizeof(T) * 8 - 1)) & 1) << 7;
        u64 pf = u64(compute_parity(static_cast<u8>(result))) << 2;
        u64 cf = u64(a < b) << 0;                                       // borrow
        u64 of = u64((((a ^ b) & (a ^ result) & sign_bit)) != 0) << 11;
        u64 af = u64((((a ^ b ^ result) & 0x10)) != 0) << 4;
        rflags = (rflags & ~STATUS_FLAGS_MASK) | cf | pf | af | zf | sf | of;
    }

    /// Update flags for logical operations (AND, OR, XOR)
    template<typename T>
    void update_flags_logic(T result) noexcept {
        // CF=OF=AF=0 by spec; ZF/SF/PF computed from result.
        u64 zf = u64(result == 0) << 6;
        u64 sf = u64((result >> (sizeof(T) * 8 - 1)) & 1) << 7;
        u64 pf = u64(compute_parity(static_cast<u8>(result))) << 2;
        rflags = (rflags & ~STATUS_FLAGS_MASK) | zf | sf | pf;
    }
};

static_assert(sizeof(Flags) == 8, "Flags must be 8 bytes");

} // namespace vitreo::cpu
