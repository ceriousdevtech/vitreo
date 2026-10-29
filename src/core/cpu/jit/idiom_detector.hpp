// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// SSE idiom detection for JIT fusion (CPU-041)

#pragma once

#include <core/cpu/decode/decoder.hpp>
#include <core/cpu/jit/block_cache.hpp>
#include <core/cpu/state/registers.hpp>
#include <optional>
#include <vector>

namespace vitreo::cpu::jit {

enum class IdiomType : u8 {
    SseMemcpy,    // N MOVDQA/MOVDQU loads + N stores, sequential offsets
    SseMemsetZero // N MOVDQA stores from XMM that was XORed with itself
};

struct IdiomMatch {
    IdiomType type;
    u32 start_idx;       // First instruction index in block
    u32 end_idx;         // One past last instruction index
    u8 src_base_reg;     // GPR base for source (SseMemcpy)
    u8 dst_base_reg;     // GPR base for destination
    u32 byte_count;      // Total bytes transferred
    u8 xmm_regs[8]{};    // XMM dests of the loads (guest-visible)
    u8 xmm_count{0};
};

/// Scan a BasicBlock for fusable SSE idioms.
/// Returns matches sorted by start_idx (non-overlapping).
std::vector<IdiomMatch> detect_idioms(const BasicBlock& block) noexcept;

/// Match the canonical scalar zero-fill loop emitted by 32-bit compilers:
///
///   mov byte ptr [dst], 0
///   lea dst, [dst + 1]
///   sub count, 1
///   jne loop
///
/// The match is deliberately exact. In particular, address-size and segment
/// overrides are rejected so the bulk helper can use ordinary 32-bit linear
/// addresses without changing guest-visible addressing semantics.
struct ZeroFillLoopMatch {
    u8 dst_reg;
    u8 count_reg;
    u32 branch_offset;
    u32 fallthrough_offset;
};

std::optional<ZeroFillLoopMatch>
detect_zero_fill_loop(const BasicBlock& block) noexcept;

} // namespace vitreo::cpu::jit
