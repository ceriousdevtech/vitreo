// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64

#pragma once

/// @file jit_split_layout.hpp
/// @brief Hot/cold split placement of one compiled block.
///
/// A block is emitted into one buffer: the hot body and its linked-tail
/// stubs first, then the cold region (memory slow paths, align / raise / UD2
/// stubs, mid-block Jcc taken exits). Placed contiguously, every block's cold
/// bytes sit between its hot path and the next block's, so the hot code the
/// game actually runs is spread over ~1.75x the memory it needs -- measured
/// at ~1% of frame rate per 100 B of dead bytes per block
/// (block-padding probe, since removed; D3D11 menu frame-rate measurement,
/// 2026-10-05).
///
/// Split placement copies the cold region to a separate, higher address in
/// the same arena window and re-targets every branch that crosses the split.
/// The window bounds the distance so TBZ/TBNZ (+-32 KiB) still reach.

#include <vitreo/common.hpp>

#include <cstddef>

namespace vitreo::cpu::jit {

/// Re-target the PC-relative branches of a block being placed split.
///
/// `words[0, split)` is the hot part, placed at word index 0; `words[split,
/// count)` is the cold part, placed at word index `split + cold_delta`
/// relative to the hot base (`cold_delta >= 0`). Branches whose source and
/// target lie on the same side keep their encoding; a crossing branch gets
/// the displacement of its new placement.
///
/// Fails, leaving `words` unchanged, when any crossing branch would leave its
/// encoding's range or any PC-relative branch targets outside the buffer --
/// the caller then places the block contiguously, as before. Also fails on
/// any other PC-relative form (ADR, ADRP, LDR literal), which this emitter
/// does not use and this pass therefore refuses to guess about.
[[nodiscard]] bool relocate_split_branches(u32* words, usize count,
                                           usize split, i64 cold_delta) noexcept;

/// Whether execution can never continue past `word` into the next one: an
/// unconditional B, BR or RET. The hot part of a split block must end in one,
/// because the word after it is no longer the block's own cold code.
[[nodiscard]] constexpr bool ends_control_flow(u32 word) noexcept {
    return (word & 0xFC000000u) == 0x14000000u ||  // B imm26
           (word & 0xFFFFFC1Fu) == 0xD61F0000u ||  // BR Xn
           (word & 0xFFFFFC1Fu) == 0xD65F0000u;    // RET Xn
}

/// Logical byte offset in the emit buffer -> byte offset from the hot base
/// after split placement. Offsets in the hot part are unchanged.
[[nodiscard]] constexpr u32 split_placed_offset(u32 logical_bytes,
                                                u32 split_bytes,
                                                u32 cold_delta_bytes) noexcept {
    return logical_bytes < split_bytes ? logical_bytes
                                       : logical_bytes + cold_delta_bytes;
}

}  // namespace vitreo::cpu::jit
