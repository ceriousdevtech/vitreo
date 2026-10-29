// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file lzx.hpp
/// @brief LZX decompressor for the variant used by Microsoft Cabinet folders.
///
/// LZX is an LZ77 + canonical-Huffman codec with three block types, an LRU of
/// three repeated match offsets, and an optional x86 CALL-target translation
/// pass. Written against Microsoft's published LZX / LZX DELTA specification
/// and [MS-CAB]; no third-party decoder source was consulted.
///
/// The one structural fact that shapes this API: **a folder's LZX stream is
/// one continuous decode**. A CAB folder chops the stream into CFDATA chunks
/// of at most 32768 output bytes each, but the sliding window, the Huffman
/// path lengths, the LRU offsets, and even a half-finished LZX block all carry
/// across chunk boundaries. Chunk N is not decodable without having decoded
/// chunks 0..N-1, so `Decoder` is a stateful object fed chunks strictly in
/// order — there is deliberately no random-access entry point to offer.

#include <vitreo/common.hpp>

#include <array>
#include <span>
#include <vector>

namespace vitreo::compression::lzx {

/// Window sizes CAB folders may declare, as a power-of-two exponent. The
/// cabinet header carries the exponent directly (tcompMASK_LZX_WINDOW).
inline constexpr u32 kMinWindowBits = 15;
inline constexpr u32 kMaxWindowBits = 21;

/// An LZX "frame" is the unit the x86 translation pass is keyed to: the
/// encoder ran the translation over the uncompressed data in 32 KB slices, so
/// the decoder must undo it in the same slices, aligned to the same absolute
/// output positions.
inline constexpr usize kFrameSize = 32768;

/// Largest number of main-tree position slots (window 2^21). Slots run to 50
/// because the per-slot extra-bit count saturates at 17: past that point each
/// additional 128 KB of window reach costs another slot rather than another
/// extra bit. A table that stops at slot 30 (enough for a 32 KB window) does
/// not fail loudly on a 2 MB window — it silently decodes long-distance
/// matches from the wrong place.
inline constexpr u32 kMaxPositionSlots = 50;

inline constexpr u32 kMainTreeMaxSymbols = 256 + (kMaxPositionSlots * 8);
inline constexpr u32 kLengthTreeSymbols = 249;
inline constexpr u32 kAlignedTreeSymbols = 8;
inline constexpr u32 kPreTreeSymbols = 20;

/// Canonical Huffman decoding table.
///
/// Held as the by-length canonical arrays plus a direct-lookup table for the
/// short codes, because tree rebuilds are frequent (potentially once per LZX
/// block) and a pointer-based tree would cost more to build than it saves.
class HuffmanTable {
public:
    /// (Re)build from path lengths. Returns false for an over-subscribed set
    /// of lengths; an *under*-subscribed (incomplete) set is accepted, because
    /// a block that never references the aligned tree may legally leave it
    /// incomplete. Decoding an unassigned code still fails.
    [[nodiscard]] bool build(const u8* lengths, u32 count) noexcept;

    /// Fast path index width. 9 bits covers the overwhelming majority of
    /// literal/length codes in real streams.
    static constexpr u32 kFastBits = 9;
    static constexpr u32 kNoEntry = 0xFFFFFFFFu;

    [[nodiscard]] u32 fast_entry(u32 index) const noexcept { return fast_[index]; }
    [[nodiscard]] u32 limit(u32 length) const noexcept { return limit_[length]; }
    [[nodiscard]] u32 first_code(u32 length) const noexcept { return first_code_[length]; }
    [[nodiscard]] u32 first_index(u32 length) const noexcept { return first_index_[length]; }
    [[nodiscard]] u16 symbol(u32 index) const noexcept { return symbols_[index]; }

private:
    std::array<u32, 17> first_code_{};
    std::array<u32, 17> first_index_{};
    std::array<u32, 17> limit_{};
    std::vector<u16> symbols_;
    std::array<u32, 1u << kFastBits> fast_{};
};

/// Stateful LZX folder decoder.
///
/// Not thread safe, and order-dependent by construction (see file comment).
class Decoder {
public:
    /// @param window_bits log2 of the window size, 15..21.
    [[nodiscard]] static Result<Decoder, Error> create(u32 window_bits);

    /// Decode one CFDATA chunk.
    ///
    /// @param compressed   the chunk's compressed bytes, exactly.
    /// @param output_size  the chunk's declared uncompressed size (cbUncomp).
    /// @param out          destination; decoded bytes are appended.
    ///
    /// Returns the number of bytes appended, which on success always equals
    /// @p output_size. A short or corrupt chunk is an error — never a
    /// partially-filled success — because a truncated folder image looks
    /// exactly like a valid one to every caller downstream.
    [[nodiscard]] Result<usize, Error> decode_chunk(std::span<const u8> compressed,
                                                    usize output_size,
                                                    std::vector<u8>& out);

    /// Absolute uncompressed position reached so far, in the untranslated
    /// stream (which is what the window holds and what x86 translation is
    /// keyed on).
    [[nodiscard]] u64 output_position() const noexcept { return output_position_; }

    [[nodiscard]] u32 window_bits() const noexcept { return window_bits_; }

    /// True once the stream header has been seen and it asked for x86 CALL
    /// translation. Exposed for diagnostics — a cabinet whose payload is PE
    /// binaries and which reports false here is worth a second look.
    [[nodiscard]] bool x86_translation_enabled() const noexcept { return e8_enabled_; }
    [[nodiscard]] u32 x86_chunk_size() const noexcept { return e8_chunk_size_; }

private:
    Decoder() = default;

    // Bit reader over one chunk. Reset for every chunk: a CFDATA boundary is
    // also a byte boundary in the compressed stream, so bits never straddle
    // chunks even though blocks do.
    class BitReader {
    public:
        void reset(std::span<const u8> data) noexcept {
            data_ = data;
            pos_ = 0;
            buf_ = 0;
            bit_count_ = 0;
        }

        /// Guarantee @p n bits are buffered. @p n must be <= 17, which keeps
        /// the 32-bit buffer from overflowing (worst case 0 + 16 + 16 = 32).
        ///
        /// Past the end of the chunk this shifts in zero bits rather than
        /// failing: a Huffman decode legitimately *peeks* up to 16 bits while
        /// standing on the final code of a chunk. Genuine overrun is caught by
        /// consumed_bits(), which counts only bits actually removed.
        void ensure(u32 n) noexcept {
            while (bit_count_ < n) {
                u32 word = 0;
                if (pos_ + 1 < data_.size()) {
                    word = static_cast<u32>(data_[pos_]) |
                           (static_cast<u32>(data_[pos_ + 1]) << 8);
                } else if (pos_ < data_.size()) {
                    word = static_cast<u32>(data_[pos_]);
                }
                pos_ += 2;
                buf_ = (buf_ << 16) | word;
                bit_count_ += 16;
            }
        }

        /// Bits arrive as 16-bit little-endian words, consumed most
        /// significant bit first within each word. This is the single most
        /// common way an LZX decoder ends up "almost" working: byte-order and
        /// bit-order disagree, so a straight LSB-first or byte-at-a-time
        /// reader decodes the first tree correctly and diverges later.
        [[nodiscard]] u32 peek(u32 n) noexcept {
            ensure(n);
            return (buf_ >> (bit_count_ - n)) & ((n == 32) ? 0xFFFFFFFFu : ((1u << n) - 1));
        }

        void remove(u32 n) noexcept { bit_count_ -= n; }

        [[nodiscard]] u32 read(u32 n) noexcept {
            if (n == 0) return 0;
            const u32 v = peek(n);
            remove(n);
            return v;
        }

        /// Read a 32-bit field, most significant half first.
        [[nodiscard]] u32 read32() noexcept {
            const u32 hi = read(16);
            const u32 lo = read(16);
            return (hi << 16) | lo;
        }

        /// Drop to the next 16-bit boundary and hand back a raw byte cursor.
        /// Uncompressed blocks leave the bitstream entirely and read plain
        /// little-endian bytes from that boundary.
        void realign16() noexcept {
            bit_count_ -= (bit_count_ % 16);
            pos_ -= bit_count_ / 8;
            bit_count_ = 0;
            buf_ = 0;
        }

        [[nodiscard]] bool read_raw(u8* dest, usize count) noexcept {
            if (pos_ + count > data_.size()) return false;
            for (usize i = 0; i < count; ++i) dest[i] = data_[pos_ + i];
            pos_ += count;
            return true;
        }

        [[nodiscard]] bool skip_raw(usize count) noexcept {
            if (pos_ + count > data_.size()) return false;
            pos_ += count;
            return true;
        }

        /// Bits genuinely consumed. Only remove() advances this, so zero-fill
        /// peeking past the end does not make a valid chunk look overrun.
        [[nodiscard]] u64 consumed_bits() const noexcept {
            return static_cast<u64>(pos_) * 8 - bit_count_;
        }

        [[nodiscard]] usize size_bits() const noexcept { return data_.size() * 8; }

    private:
        std::span<const u8> data_;
        usize pos_ = 0;
        u32 buf_ = 0;
        u32 bit_count_ = 0;
    };

    enum class BlockType : u32 {
        Invalid = 0,
        Verbatim = 1,
        AlignedOffset = 2,
        Uncompressed = 3,
    };

    [[nodiscard]] Error read_stream_header();
    [[nodiscard]] Error start_block();
    [[nodiscard]] Error read_lengths(u8* lengths, u32 first, u32 last);
    [[nodiscard]] Error decode_symbols(usize count);
    [[nodiscard]] Error copy_uncompressed(usize count);
    [[nodiscard]] Result<u32, Error> decode_symbol(const HuffmanTable& table);

    void emit(usize count, std::vector<u8>& out);
    void apply_x86_translation(u8* data, usize length, u64 absolute_position) const;

    u32 window_bits_ = 0;
    u32 window_mask_ = 0;
    u32 position_slots_ = 0;
    u32 main_symbols_ = 0;

    std::vector<u8> window_;
    u32 window_position_ = 0;
    u64 output_position_ = 0;

    // Repeated-offset LRU. Seeded to 1 at stream start, carried across blocks
    // and chunks, and rewritten by every non-repeat match.
    u32 r0_ = 1;
    u32 r1_ = 1;
    u32 r2_ = 1;

    bool header_read_ = false;
    bool e8_enabled_ = false;
    u32 e8_chunk_size_ = 0;

    BlockType block_type_ = BlockType::Invalid;
    u32 block_remaining_ = 0;
    bool block_length_odd_ = false;

    // Path lengths persist across blocks: a block's tree is delta-coded
    // against the lengths left behind by the previous block, which is why
    // these live in the decoder rather than in start_block().
    std::vector<u8> main_lengths_;
    std::array<u8, kLengthTreeSymbols> length_lengths_{};
    std::array<u8, kAlignedTreeSymbols> aligned_lengths_{};

    HuffmanTable main_table_;
    HuffmanTable length_table_;
    HuffmanTable aligned_table_;
    HuffmanTable pre_table_;

    BitReader reader_;
};

} // namespace vitreo::compression::lzx
