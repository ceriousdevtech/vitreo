// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64

#pragma once

#include "vitreo/types.hpp"
#include "vitreo/result.hpp"
#include "opcodes.hpp"

namespace vitreo::cpu {

/// Operand types for decoded instructions
enum class OperandType : u8 {
    None = 0,
    
    // Register operands
    Reg8,        // 8-bit register (AL, CL, etc.)
    Reg16,       // 16-bit register (AX, CX, etc.)
    Reg32,       // 32-bit register (EAX, ECX, etc.)
    Reg64,       // 64-bit register (RAX, RCX, etc.)
    Reg128,      // 128-bit register (XMM0-XMM15)
    RegMM,       // 64-bit MMX register (MM0-MM7)
    RegSeg,      // Segment register (CS, DS, etc.)
    
    // Memory operands
    Mem8,        // 8-bit memory reference
    Mem16,       // 16-bit memory reference
    Mem32,       // 32-bit memory reference
    Mem64,       // 64-bit memory reference
    Mem80,       // 80-bit memory (FPU)
    Mem128,      // 128-bit memory (SSE)
    Mem256,      // 256-bit memory (AVX)
    MemUnknown,  // Memory reference with unspecified size
    
    // Immediate operands
    Imm8,        // 8-bit immediate
    Imm16,       // 16-bit immediate
    Imm32,       // 32-bit immediate
    Imm64,       // 64-bit immediate
    
    // Relative offsets (for jumps/calls)
    Rel8,        // 8-bit relative offset
    Rel16,       // 16-bit relative offset
    Rel32,       // 32-bit relative offset
    
    // Far pointer
    FarPtr16_16, // 16:16 far pointer
    FarPtr16_32, // 16:32 far pointer
};

/// Get the operand size in bytes (0 for unknown/none)
[[nodiscard]] constexpr u8 operand_size(OperandType type) noexcept {
    switch (type) {
        case OperandType::None:      return 0;
        case OperandType::Reg8:      return 1;
        case OperandType::Reg16:     return 2;
        case OperandType::Reg32:     return 4;
        case OperandType::Reg64:     return 8;
        case OperandType::Reg128:    return 16;
        case OperandType::RegMM:     return 8;
        case OperandType::RegSeg:    return 2;
        case OperandType::Mem8:      return 1;
        case OperandType::Mem16:     return 2;
        case OperandType::Mem32:     return 4;
        case OperandType::Mem64:     return 8;
        case OperandType::Mem80:     return 10;
        case OperandType::Mem128:    return 16;
        case OperandType::Mem256:    return 32;
        case OperandType::MemUnknown: return 0;
        case OperandType::Imm8:      return 1;
        case OperandType::Imm16:     return 2;
        case OperandType::Imm32:     return 4;
        case OperandType::Imm64:     return 8;
        case OperandType::Rel8:      return 1;
        case OperandType::Rel16:     return 2;
        case OperandType::Rel32:     return 4;
        case OperandType::FarPtr16_16: return 4;
        case OperandType::FarPtr16_32: return 6;
        default:                     return 0;
    }
}

/// Check if operand type is a register
[[nodiscard]] constexpr bool is_register_operand(OperandType type) noexcept {
    return type >= OperandType::Reg8 && type <= OperandType::RegSeg;
}

/// Check if operand type is a memory reference
[[nodiscard]] constexpr bool is_memory_operand(OperandType type) noexcept {
    return type >= OperandType::Mem8 && type <= OperandType::MemUnknown;
}

/// Check if operand type is an immediate
[[nodiscard]] constexpr bool is_immediate_operand(OperandType type) noexcept {
    return type >= OperandType::Imm8 && type <= OperandType::Imm64;
}

/// Segment register indices
enum class SegReg : u8 {
    ES = 0,
    CS = 1,
    SS = 2,
    DS = 3,
    FS = 4,
    GS = 5,
    Default = 255,  // No override, use default segment
};

/// Memory operand details (ModR/M + SIB decoded)
///
/// Field ORDER is deliberate and is part of the memory budget: the four hot
/// effective-address inputs (disp, base, index, segment) occupy the first 8
/// bytes, and `absolute` — needed only by the A0-A3 moffs forms — occupies the
/// second 8. Every field except the two one-bit flags is a plain integer, so
/// the hot EA path (`calc_effective_address`, `emit_compute_ea*`) still reads
/// them with a single load and no masking. `scale` shares its byte with the
/// flags because a fifth plain byte would push the struct to 24 again; it costs
/// one AND on the (much rarer) scaled-index path only.
struct MemoryOperand {
    i32 disp;              // Displacement (full x86 i32 range)
    u8 base;               // Base register index (16 = RIP, 255 = none)
    u8 index;              // Index register index (255 = none)
    SegReg segment;        // Segment override
    u8 scale : 6;          // Scale factor (1, 2, 4, 8)
    bool vsib : 1;         // AVX2 VSIB: index is XMM/YMM, not a GPR
    bool has_absolute : 1; // Direct-memory-offset MOV (A0-A3) in 64-bit address mode
    u64 absolute;          // Full moffs address when has_absolute is true

    constexpr MemoryOperand() noexcept
        : disp(0), base(255), index(255),
          segment(SegReg::Default), scale(1), vsib(false),
          has_absolute(false), absolute(0) {}
    
    /// Check if this is a RIP-relative address
    [[nodiscard]] constexpr bool is_rip_relative() const noexcept {
        return base == 16;
    }
    
    /// Check if has a base register
    [[nodiscard]] constexpr bool has_base() const noexcept {
        return base != 255 && base != 16;
    }
    
    /// Check if has an index register
    [[nodiscard]] constexpr bool has_index() const noexcept {
        return index != 255;
    }
    
    /// Check if this is displacement-only addressing
    [[nodiscard]] constexpr bool is_disp_only() const noexcept {
        return base == 255 && index == 255;
    }
};

// See the DecodedInstruction size assert below: this struct is retained per
// guest instruction for every cached block. 16 is the floor while `absolute`
// must be a u64 reachable through a `const MemoryOperand&` (the EA helpers take
// one at ~490 call sites), because a u64 member forces 8-byte alignment.
static_assert(sizeof(MemoryOperand) == 16,
              "MemoryOperand is retained per guest instruction; growth here is "
              "multiplied by 4 operands x every cached block");

/// Instruction operand
struct Operand {
    OperandType type;
    
    union {
        u8 reg;               // Register index
        i64 imm;              // Immediate/relative value
        MemoryOperand mem;    // Memory reference
    };
    
    constexpr Operand() noexcept : type(OperandType::None), imm(0) {}
    
    /// Create a register operand
    [[nodiscard]] static constexpr Operand make_reg(OperandType ty, u8 index) noexcept {
        Operand op;
        op.type = ty;
        op.reg = index;
        return op;
    }
    
    /// Create an immediate operand
    [[nodiscard]] static constexpr Operand make_imm(OperandType ty, i64 value) noexcept {
        Operand op;
        op.type = ty;
        op.imm = value;
        return op;
    }
    
    /// Create a memory operand
    [[nodiscard]] static constexpr Operand make_mem(OperandType ty, const MemoryOperand& m) noexcept {
        Operand op;
        op.type = ty;
        op.mem = m;
        return op;
    }
    
    /// Create an MMX register operand (mm0-mm7)
    [[nodiscard]] static constexpr Operand reg_mm(u8 index) noexcept {
        Operand op;
        op.type = OperandType::RegMM;
        op.reg = index & 7;  // MMX only has 8 registers
        return op;
    }
    
    /// Check if this is a valid operand (not None)
    [[nodiscard]] constexpr bool is_valid() const noexcept {
        return type != OperandType::None;
    }
};

// 24 = 1 byte of tag, 7 bytes of alignment padding, and the 16-byte union. The
// padding is forced by `i64 imm`, which must keep its full range. Operand
// reaches 16 only if MemoryOperand reaches 8, which `u64 absolute` forbids.
static_assert(sizeof(Operand) == 24,
              "four of these are retained per guest instruction for every "
              "cached block");

/// Instruction prefixes
///
/// The bit flags are grouped first so the whole struct fits in 4 bytes; that is
/// what lets DecodedInstruction's header share one 8-byte word with the opcode,
/// length and operand count.
struct Prefixes {
    // Legacy prefixes
    bool lock : 1;               // F0 - LOCK prefix
    bool rep : 1;                // F3 - REP/REPE prefix
    bool repne : 1;              // F2 - REPNE prefix
    bool operand_size : 1;       // 66 - Operand size override
    bool address_size : 1;       // 67 - Address size override

    // VEX prefix (AVX/AVX2/FMA/BMI). vex_vvvv is the already-uninverted
    // register number (Intel SDM Vol. 2, VEX).
    bool vex : 1;
    bool vex_l : 1;              // 1 = 256-bit

    // EVEX prefix (62 P0 P1 P2) seen. The payload -- L'L, W, the mask
    // register aaa, z, b and V' -- is deliberately NOT stored: this struct is
    // retained per guest instruction for every cached block and its size is
    // asserted below, and no consumer can use those fields while every
    // EVEX-encoded ISA is CPUID-hidden. Decoding sets this bit and the
    // instruction's length, and the opcode becomes EVEX_UNSUPPORTED. When an
    // EVEX ISA is actually implemented, the payload lands with the register
    // file it needs, not before.
    //
    // This is the last free bit in this byte. Adding another flag grows
    // Prefixes to 5 bytes, which grows DecodedInstruction past 104 and
    // breaks the static_assert -- read the memory budget comment there first.
    bool evex : 1;

    // Segment override (Default = no override)
    SegReg segment;

    // REX prefix (64-bit mode)
    u8 rex;                      // Full REX byte (0 if not present)

    u8 vex_vvvv;                 // src1 / NDD dest / gather mask

    constexpr Prefixes() noexcept
        : lock(false), rep(false), repne(false),
          operand_size(false), address_size(false),
          vex(false), vex_l(false), evex(false),
          segment(SegReg::Default), rex(0), vex_vvvv(0) {}
    
    // REX field accessors
    [[nodiscard]] constexpr bool has_rex() const noexcept { return rex != 0; }
    [[nodiscard]] constexpr bool rex_w() const noexcept { return (rex & 0x08) != 0; }
    [[nodiscard]] constexpr bool rex_r() const noexcept { return (rex & 0x04) != 0; }
    [[nodiscard]] constexpr bool rex_x() const noexcept { return (rex & 0x02) != 0; }
    [[nodiscard]] constexpr bool rex_b() const noexcept { return (rex & 0x01) != 0; }
};

/// Maximum number of operands per instruction
///
/// 4 is REQUIRED, not aspirational: the VEX /is4 forms (VPBLENDVB, VBLENDVPS,
/// VBLENDVPD — see decode/opcodes/vex.cpp) decode as dest, src1, src2 and the
/// is4 immediate. Lowering this would silently drop an operand.
constexpr usize MAX_OPERANDS = 4;

/// Decoded instruction
///
/// Member order is load-bearing for size: the four header fields are packed
/// into the single 8-byte word that the 8-byte-aligned operand array needs
/// anyway, so `prefixes` is free.
struct DecodedInstruction {
    Opcode opcode;              // Instruction opcode
    u8 length;                  // Total instruction length in bytes
    u8 operand_count;           // Number of operands
    Prefixes prefixes;          // Prefix state
    Operand operands[MAX_OPERANDS];

    constexpr DecodedInstruction() noexcept
        : opcode(Opcode::INVALID), length(0), operand_count(0),
          prefixes{}, operands{} {}
    
    /// Check if instruction is valid
    [[nodiscard]] constexpr bool is_valid() const noexcept {
        return opcode != Opcode::INVALID;
    }
    
    /// Get operand by index (returns None operand if out of bounds)
    [[nodiscard]] constexpr const Operand& operand(usize idx) const noexcept {
        static constexpr Operand invalid{};
        return idx < operand_count ? operands[idx] : invalid;
    }
    
    /// Add an operand
    constexpr bool add_operand(const Operand& op) noexcept {
        if (operand_count >= MAX_OPERANDS) return false;
        operands[operand_count++] = op;
        return true;
    }
};

// ---------------------------------------------------------------------------
// MEMORY BUDGET — do not "just add a field".
//
// One DecodedInstruction is RETAINED for every guest instruction of every
// cached block, for the lifetime of that block. A measured multi-process launcher run holds
// 957,731 compiled x86 instructions. Each byte added here therefore costs ~1 MB of
// dirty host heap per process in that run, and every 8 bytes added to Operand
// costs four times that. This struct was cut from 144 to 104 bytes
// (MemoryOperand 24 -> 16, Operand 32 -> 24, Prefixes 5 -> 4); growing it back
// is a memory regression measured in hundreds of megabytes across processes,
// not a rounding error.
//
// If a new field is genuinely required, put it in the 7 bytes of tail padding
// inside Operand (offsets 1..7, forced by `i64 imm`) or in Prefixes' spare bit
// before you enlarge anything.
// ---------------------------------------------------------------------------
static_assert(sizeof(DecodedInstruction) == 104,
              "DecodedInstruction is retained per guest instruction for every "
              "cached block; growth here is a memory regression measured in "
              "hundreds of MB. Read the comment above before changing this.");

/// Decoder error types
enum class DecodeError : u8 {
    Success = 0,
    UnexpectedEnd,        // Ran out of bytes
    InvalidOpcode,        // Unrecognized opcode
    InvalidModRM,         // Invalid ModR/M encoding
    InvalidPrefix,        // Invalid prefix combination
    InvalidOperand,       // Invalid operand encoding
    UnsupportedMode,      // Instruction not supported in current mode
};

/// Decoder mode (affects instruction decoding)
enum class DecodeMode : u8 {
    Mode16,    // 16-bit real/protected mode
    Mode32,    // 32-bit protected mode
    Mode64,    // 64-bit long mode
};

/// Byte reader interface for decoder
class ByteReader {
public:
    virtual ~ByteReader() = default;
    
    /// Read a single byte, returns false on end of input
    [[nodiscard]] virtual bool read_u8(u8& out) noexcept = 0;
    
    /// Get current position (bytes read so far)
    [[nodiscard]] virtual usize position() const noexcept = 0;
    
    /// Read a 16-bit little-endian value
    [[nodiscard]] bool read_u16(u16& out) noexcept {
        u8 lo, hi;
        if (!read_u8(lo) || !read_u8(hi)) return false;
        out = static_cast<u16>(lo) | (static_cast<u16>(hi) << 8);
        return true;
    }
    
    /// Read a 32-bit little-endian value
    [[nodiscard]] bool read_u32(u32& out) noexcept {
        u16 lo, hi;
        if (!read_u16(lo) || !read_u16(hi)) return false;
        out = static_cast<u32>(lo) | (static_cast<u32>(hi) << 16);
        return true;
    }
    
    /// Read a 64-bit little-endian value
    [[nodiscard]] bool read_u64(u64& out) noexcept {
        u32 lo, hi;
        if (!read_u32(lo) || !read_u32(hi)) return false;
        out = static_cast<u64>(lo) | (static_cast<u64>(hi) << 32);
        return true;
    }
    
    /// Read signed values
    [[nodiscard]] bool read_i8(i8& out) noexcept {
        u8 val;
        if (!read_u8(val)) return false;
        out = static_cast<i8>(val);
        return true;
    }
    
    [[nodiscard]] bool read_i16(i16& out) noexcept {
        u16 val;
        if (!read_u16(val)) return false;
        out = static_cast<i16>(val);
        return true;
    }
    
    [[nodiscard]] bool read_i32(i32& out) noexcept {
        u32 val;
        if (!read_u32(val)) return false;
        out = static_cast<i32>(val);
        return true;
    }
};

/// Simple byte reader from memory buffer
class MemoryByteReader final : public ByteReader {
public:
    MemoryByteReader(const u8* data, usize size) noexcept 
        : m_data(data), m_size(size), m_pos(0) {}
    
    [[nodiscard]] bool read_u8(u8& out) noexcept override {
        if (m_pos >= m_size) return false;
        out = m_data[m_pos++];
        return true;
    }
    
    /// Get current position
    [[nodiscard]] usize position() const noexcept override { return m_pos; }
    
    /// Get remaining bytes
    [[nodiscard]] usize remaining() const noexcept { return m_size - m_pos; }
    
    /// Reset to beginning
    void reset() noexcept { m_pos = 0; }
    
private:
    const u8* m_data;
    usize m_size;
    usize m_pos;
};

/// Decode a single instruction from a byte stream
/// Returns DecodeError::Success on success, error code on failure
[[nodiscard]] DecodeError decode_instruction(
    ByteReader& reader, 
    DecodeMode mode,
    DecodedInstruction& out
) noexcept;

} // namespace vitreo::cpu
