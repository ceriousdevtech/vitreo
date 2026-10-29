// SPDX-License-Identifier: LGPL-2.1-only

#pragma once

#include <vitreo/types.hpp>

namespace vitreo {
class GuestMemory;
}

namespace vitreo::cpu::x64 {

/// Rewrite Win64 segment-relative memory operands for native macOS execution.
///
/// Vitreo carries the Windows TEB in the host FS base because macOS preserves
/// an LDT-derived FS base across kernel transitions. Windows guest GS accesses
/// therefore become FS accesses. Guest FS accesses must retain Win64's normal
/// zero base, so they become DS accesses (DS base is architecturally zero in
/// 64-bit mode). Instruction sizes and all non-effective prefixes are kept.
///
/// This span form is exposed for focused decoder tests. The caller must make
/// the bytes writable.
/// @param teb_shape_only translate ONLY instructions whose operand carries a
///        real Win64 TEB-access shape (segment prefix, memory operand, no
///        base, no index, small displacement). Required for anything backed by
///        a real module: a linear decode desynchronises on the jump tables,
///        literals and alignment every .text carries, and an undesynchronised
///        write turns a data byte into 0x64/0x3E. Measured on a launcher's
///        modules, that shape matches every real `mov rax, gs:[0x30]` and none of the
///        150-170 junk decodes per module.
[[nodiscard]] usize rewrite_segment_overrides(
    u8* code, usize size, bool teb_shape_only = false) noexcept;

/// One instruction stream whose first byte is a known instruction boundary.
struct CodeRange {
    usize offset;   ///< Byte offset into the span handed to the rewriter.
    usize length;   ///< Length of the stream in bytes.
};

/// Rewrite only inside the given instruction streams.
///
/// The span form above decodes an executable range linearly from its first
/// byte, which desynchronises on the data every real .text carries (jump
/// tables, embedded literals, alignment). A desynchronised decode turns a
/// data byte 0x65/0x64 into 0x64/0x3E and silently corrupts the image. When
/// the caller can name the real instruction boundaries -- an x64 PE always
/// can, through .pdata -- use this form: each range is decoded from its own
/// first byte and abandoned on the first undecodable byte, so no byte outside
/// a real instruction is ever written.
[[nodiscard]] usize rewrite_segment_overrides_ranges(u8* code, usize size,
                                                     const CodeRange* ranges,
                                                     usize count) noexcept;

/// Open, rewrite, and restore an executable guest-memory range.
///
/// With @p ranges the rewrite is confined to those instruction streams, whose
/// offsets are relative to @p base; without them the whole range is swept
/// linearly, which is only safe where no data can be interleaved with code.
[[nodiscard]] usize rewrite_segment_overrides_in_range(
    GuestMemory& memory, GuestAddr base, usize size,
    const CodeRange* ranges = nullptr, usize count = 0,
    bool teb_shape_only = false) noexcept;

} // namespace vitreo::cpu::x64
