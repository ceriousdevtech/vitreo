// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file mszip.hpp
/// @brief MSZIP decompressor for cabinet folders.
///
/// MSZIP is the compression [MS-CAB] §2.5 defines for `tcompTYPE_MSZIP`: each
/// CFDATA block carries the two-byte signature `CK` followed by one complete
/// raw DEFLATE stream (RFC 1951 — no zlib wrapper, no Adler checksum), and
/// decodes to at most 32768 bytes.
///
/// The landmine this API exists to make unavoidable: **the DEFLATE stream in
/// block N may reference the output of block N-1**. Each block is a *separate*
/// DEFLATE stream — its own final block, its own Huffman trees, its own bit
/// alignment — but the 32 KB LZ77 history window carries across the boundary,
/// exactly as if the previous block's output had been loaded as a preset
/// dictionary. Inflating each block independently does not fail: it produces
/// correct bytes until the first cross-block match and then quietly emits
/// garbage, so `Decoder` is stateful and must be fed a folder's chunks in
/// order.

#include <vitreo/common.hpp>

#include <span>
#include <vector>

namespace vitreo::compression::mszip {

/// Largest output a single MSZIP block may produce, which is also the size of
/// the history window carried to the next block.
inline constexpr usize kBlockSize = 32768;

/// Stateful MSZIP folder decoder. Not thread safe; order-dependent by
/// construction (see the file comment).
class Decoder {
public:
    /// Decode one CFDATA chunk.
    ///
    /// @param compressed   the chunk's compressed bytes, exactly, starting at
    ///                     the `CK` signature.
    /// @param output_size  the chunk's declared uncompressed size (cbUncomp).
    /// @param out          destination; decoded bytes are appended.
    ///
    /// Returns the number of bytes appended, which on success always equals
    /// @p output_size. A short or corrupt chunk is an error, never a
    /// partially-filled success: a truncated folder image is indistinguishable
    /// from a valid one to every caller downstream.
    [[nodiscard]] Result<usize, Error> decode_chunk(std::span<const u8> compressed,
                                                    usize output_size,
                                                    std::vector<u8>& out);

private:
    /// The previous block's output, capped at kBlockSize — the preset
    /// dictionary for the next block.
    std::vector<u8> history_;
};

/// Inflate one raw DEFLATE stream with an optional preset dictionary.
///
/// @param dictionary  bytes logically preceding the output; back-references
///                    may reach into them. Pass an empty span for none.
///
/// Exposed separately from Decoder because the dictionary handoff is the part
/// worth testing directly.
[[nodiscard]] Result<std::vector<u8>, Error> inflate_raw(
    std::span<const u8> compressed,
    std::span<const u8> dictionary,
    usize expected_size);

} // namespace vitreo::compression::mszip
