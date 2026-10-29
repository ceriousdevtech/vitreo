// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64

#pragma once

/// @file stencils.hpp
/// @brief Pre-compiled ARM64 code stencils for copy-and-patch JIT (CPU-040)

#include <vitreo/common.hpp>
#include <core/cpu/decode/opcodes.hpp>
#include <cstddef>

namespace vitreo::cpu::jit {

// ============================================================================
// Relocation types for stencil patching
// ============================================================================

/// Type of hole in a stencil that needs patching
enum class RelocType : u8 {
    CpuStateOffset,     // Offset into CpuState struct (GPR, flags, etc.)
    Immediate,          // A constant immediate value
    BranchTarget,       // Relative branch target (offset from stencil start)
    GuestAddr,          // Guest address (for RIP-relative addressing)
};

/// A single relocation entry within a stencil
struct StencilReloc {
    u32 offset;         // Byte offset within stencil where the hole is
    RelocType type;     // What kind of value to patch in
    u8 operand_idx;     // Which instruction operand provides the value (0-3)
    u8 size;            // Size of the field to patch (1, 2, or 4 bytes)
    i32 addend;         // Constant addend (e.g., struct field offset)
};

/// A pre-compiled ARM64 code stencil for one x86 instruction pattern
struct Stencil {
    const u8* code;             // Pointer to ARM64 machine code bytes
    u32 code_size;              // Size in bytes
    const StencilReloc* relocs; // Array of relocations
    u32 reloc_count;            // Number of relocations
};

// ============================================================================
// Register offset helpers
// ============================================================================

/// Offset of general purpose register in CpuState.gpr.r[index]
constexpr u32 gpr_offset(u8 reg_idx) noexcept {
    // CpuState layout: gpr is first field, GeneralRegs64 has r[16] union
    return static_cast<u32>(reg_idx) * 8;
}

/// Offset of the rflags register in CpuState
constexpr u32 rflags_offset() noexcept {
    // gpr(128) + seg(varies) + flags.rflags
    // We'll compute this at compile time from offsetof
    return 0;  // Placeholder - set by stencils.cpp via offsetof
}

// ============================================================================
// Stencil emission helpers (called by JitCompiler)
// ============================================================================

/// Get the stencil for a given opcode, or nullptr if unsupported
const Stencil* get_stencil(Opcode opcode) noexcept;

/// Check if a given opcode has a JIT stencil available
bool has_stencil(Opcode opcode) noexcept;

/// True when the host implements FEAT_FlagM (CFINV, RMIF). Mandatory from
/// Armv8.4 and set on every Apple M-series part measured, but queried rather
/// than assumed: the ADC/SBB carry-chain lowering silently produces the WRONG
/// CARRY if CFINV is a no-op or traps. Compile-time only; never from emitted
/// code.
[[nodiscard]] bool host_supports_flagm() noexcept;

/// True when the host implements FEAT_LRCPC (LDAPR: RCpc acquire loads).
/// RCpc is the weaker acquire that actually matches x86-TSO, and is cheaper
/// than LDAR, so the guest-load ordering path prefers it where advertised.
/// Gate every LDAPR use on this; fall back to ldar_*.
[[nodiscard]] bool host_supports_lrcpc() noexcept;

/// True when the host implements FEAT_AES (AESE/AESD/AESMC/AESIMC). Optional
/// in the base Armv8-A profile, so it is queried rather than assumed even
/// though every Apple M-series part advertises it: emitting AESE on a host
/// without the extension is an undefined instruction, not a slow path.
/// Compile-time only; never from emitted code.
[[nodiscard]] bool host_supports_aes() noexcept;

} // namespace vitreo::cpu::jit

// ============================================================================
// ARM64 instruction encoding helpers
// ============================================================================
//
// The arm64:: encoders are split by instruction class; every includer of
// this header sees all of them.

#include <core/cpu/jit/stencils_arm64_fields.hpp>
#include <core/cpu/jit/stencils_arm64_alu.hpp>
#include <core/cpu/jit/stencils_arm64_immediates.hpp>
#include <core/cpu/jit/stencils_arm64_shift_bitfield.hpp>
#include <core/cpu/jit/stencils_arm64_load_store.hpp>
#include <core/cpu/jit/stencils_arm64_ordered.hpp>
#include <core/cpu/jit/stencils_arm64_branch.hpp>
#include <core/cpu/jit/stencils_arm64_simd_ldst.hpp>
#include <core/cpu/jit/stencils_arm64_simd_int.hpp>
#include <core/cpu/jit/stencils_arm64_simd_permute.hpp>
#include <core/cpu/jit/stencils_arm64_fp.hpp>
#include <core/cpu/jit/stencils_arm64_crypto.hpp>
