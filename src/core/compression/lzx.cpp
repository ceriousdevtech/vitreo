// SPDX-License-Identifier: LGPL-2.1-only
/// @file lzx.cpp
/// @brief LZX decompressor implementation.
///
/// Derived from Microsoft's published LZX / LZX DELTA specification and
/// [MS-CAB]; the tables below are generated from the rules those documents
/// state rather than transcribed from any existing decoder.

#include "core/compression/lzx.hpp"

#include <cstring>

namespace vitreo::compression::lzx {

namespace {

// -----------------------------------------------------------------------------
// Position slot tables
// -----------------------------------------------------------------------------
//
// extra_bits[slot] = 0 for slots 0..3, else min(17, slot/2 - 1).
// base[0] = 0, base[slot] = base[slot-1] + (1 << extra_bits[slot-1]).
//
// The saturation at 17 is the whole reason slot counts are not simply
// 2*window_bits: once a slot can only reach 128 KB further than the previous
// one, covering a 2 MB window takes 50 slots rather than 42.

struct SlotTables {
    std::array<u8, kMaxPositionSlots> extra{};
    std::array<u32, kMaxPositionSlots> base{};

    constexpr SlotTables() {
        for (u32 slot = 0; slot < kMaxPositionSlots; ++slot) {
            if (slot < 4) {
                extra[slot] = 0;
            } else {
                const u32 e = (slot / 2) - 1;
                extra[slot] = static_cast<u8>(e > 17 ? 17 : e);
            }
        }
        base[0] = 0;
        for (u32 slot = 1; slot < kMaxPositionSlots; ++slot) {
            base[slot] = base[slot - 1] + (1u << extra[slot - 1]);
        }
    }
};

constexpr SlotTables kSlots{};

/// Number of position slots for a window size. Not a formula in the spec — a
/// table — because of the extra-bit saturation described above.
[[nodiscard]] constexpr u32 position_slots_for_window(u32 window_bits) {
    switch (window_bits) {
        case 15: return 30;
        case 16: return 32;
        case 17: return 34;
        case 18: return 36;
        case 19: return 38;
        case 20: return 42;
        case 21: return 50;
        default: return 0;
    }
}

/// LZX encodes a match length of 2..257; the main-tree symbol carries lengths
/// 2..8 inline and defers anything longer to the length tree.
inline constexpr u32 kMinMatch = 2;
inline constexpr u32 kNumPrimaryLengths = 7;

/// x86 CALL translation is abandoned once the output stream passes 1 GB. The
/// encoder does the same, so a decoder that keeps translating past this point
/// corrupts every E8 byte in the tail of a large payload.
inline constexpr u64 kX86TranslationCutoff = 0x40000000ull;

/// A frame shorter than this is left untranslated: the 4-byte operand of an
/// E8 must be wholly inside the frame, and the encoder used the same margin.
inline constexpr usize kX86FrameMargin = 10;

[[nodiscard]] inline i32 read_i32le(const u8* p) noexcept {
    u32 v = static_cast<u32>(p[0]) | (static_cast<u32>(p[1]) << 8) |
            (static_cast<u32>(p[2]) << 16) | (static_cast<u32>(p[3]) << 24);
    return static_cast<i32>(v);
}

inline void write_i32le(u8* p, i32 value) noexcept {
    const u32 v = static_cast<u32>(value);
    p[0] = static_cast<u8>(v);
    p[1] = static_cast<u8>(v >> 8);
    p[2] = static_cast<u8>(v >> 16);
    p[3] = static_cast<u8>(v >> 24);
}

} // namespace

// =============================================================================
// HuffmanTable
// =============================================================================

bool HuffmanTable::build(const u8* lengths, u32 count) noexcept {
    std::array<u32, 17> length_count{};
    for (u32 i = 0; i < count; ++i) {
        const u8 len = lengths[i];
        if (len > 16) return false;
        ++length_count[len];
    }

    first_code_.fill(0);
    first_index_.fill(0);
    limit_.fill(0);
    fast_.fill(kNoEntry);

    u32 code = 0;
    u32 index = 0;
    for (u32 len = 1; len <= 16; ++len) {
        first_code_[len] = code;
        first_index_[len] = index;
        limit_[len] = code + length_count[len];
        // Over-subscription: more codes of this length than the length can
        // express. Corrupt input, not a recoverable condition.
        if (limit_[len] > (1u << len)) return false;
        index += length_count[len];
        code = limit_[len] << 1;
    }

    symbols_.assign(index, 0);
    std::array<u32, 17> next_index = first_index_;
    for (u32 sym = 0; sym < count; ++sym) {
        const u8 len = lengths[sym];
        if (len == 0) continue;
        symbols_[next_index[len]++] = static_cast<u16>(sym);
    }

    // Direct-lookup fill for the short codes.
    for (u32 len = 1; len <= kFastBits; ++len) {
        const u32 n = limit_[len] - first_code_[len];
        for (u32 i = 0; i < n; ++i) {
            const u32 sym = symbols_[first_index_[len] + i];
            const u32 start = (first_code_[len] + i) << (kFastBits - len);
            const u32 run = 1u << (kFastBits - len);
            for (u32 j = 0; j < run; ++j) {
                fast_[start + j] = (len << 16) | sym;
            }
        }
    }
    return true;
}

// =============================================================================
// Decoder construction
// =============================================================================

Result<Decoder, Error> Decoder::create(u32 window_bits) {
    if (window_bits < kMinWindowBits || window_bits > kMaxWindowBits) {
        return Result<Decoder, Error>::err(Error::InvalidArgument);
    }
    const u32 slots = position_slots_for_window(window_bits);
    if (slots == 0) return Result<Decoder, Error>::err(Error::InvalidArgument);

    Decoder d;
    d.window_bits_ = window_bits;
    d.window_mask_ = (1u << window_bits) - 1;
    d.position_slots_ = slots;
    d.main_symbols_ = 256 + (slots * 8);
    // Zero-filled: a match reaching before the start of the stream is invalid
    // and is rejected explicitly, so the fill is only here to keep the buffer
    // deterministic rather than to paper over such a match.
    d.window_.assign(static_cast<usize>(1) << window_bits, 0);
    d.main_lengths_.assign(d.main_symbols_, 0);
    return Result<Decoder, Error>::ok(std::move(d));
}

// =============================================================================
// Bitstream-level decoding
// =============================================================================

Result<u32, Error> Decoder::decode_symbol(const HuffmanTable& table) {
    const u32 bits = reader_.peek(16);
    const u32 entry = table.fast_entry(bits >> (16 - HuffmanTable::kFastBits));
    if (entry != HuffmanTable::kNoEntry) {
        reader_.remove(entry >> 16);
        return Result<u32, Error>::ok(entry & 0xFFFFu);
    }
    // Canonical decode for the long codes. Testing only against limit[] is
    // sufficient because shorter lengths were already ruled out above.
    for (u32 len = HuffmanTable::kFastBits + 1; len <= 16; ++len) {
        const u32 code = bits >> (16 - len);
        if (code < table.limit(len)) {
            const u32 sym =
                table.symbol(table.first_index(len) + (code - table.first_code(len)));
            reader_.remove(len);
            return Result<u32, Error>::ok(sym);
        }
    }
    return Result<u32, Error>::err(Error::InvalidFormat);
}

Error Decoder::read_stream_header() {
    // One bit per folder stream, not per block or per chunk: does the payload
    // carry x86 CALL translation, and if so what file size was it keyed to.
    e8_enabled_ = reader_.read(1) != 0;
    e8_chunk_size_ = e8_enabled_ ? reader_.read32() : 0;
    header_read_ = true;
    return Error::None;
}

Error Decoder::read_lengths(u8* lengths, u32 first, u32 last) {
    // Pre-tree: 20 path lengths, 4 bits each, read raw.
    std::array<u8, kPreTreeSymbols> pre_lengths{};
    for (u32 i = 0; i < kPreTreeSymbols; ++i) {
        pre_lengths[i] = static_cast<u8>(reader_.read(4));
    }
    if (!pre_table_.build(pre_lengths.data(), kPreTreeSymbols)) {
        return Error::InvalidFormat;
    }

    u32 i = first;
    while (i < last) {
        auto sym = decode_symbol(pre_table_);
        if (sym.is_err()) return sym.error();
        const u32 code = sym.value();

        if (code == 17) {
            // Run of zero lengths, 4..19.
            u32 run = reader_.read(4) + 4;
            if (run > last - i) run = last - i;
            std::memset(lengths + i, 0, run);
            i += run;
        } else if (code == 18) {
            // Run of zero lengths, 20..51.
            u32 run = reader_.read(5) + 20;
            if (run > last - i) run = last - i;
            std::memset(lengths + i, 0, run);
            i += run;
        } else if (code == 19) {
            // Run of 4..5 identical lengths, delta-coded once against the
            // length that sat at the *start* of the run before this block.
            u32 run = reader_.read(1) + 4;
            auto delta = decode_symbol(pre_table_);
            if (delta.is_err()) return delta.error();
            if (delta.value() > 16) return Error::InvalidFormat;
            const u8 value =
                static_cast<u8>((lengths[i] + 17 - delta.value()) % 17);
            if (run > last - i) run = last - i;
            std::memset(lengths + i, value, run);
            i += run;
        } else if (code <= 16) {
            // Single length, delta-coded modulo 17 against the previous
            // block's length for this symbol. This is why path lengths must
            // survive block *and* CFDATA boundaries.
            lengths[i] = static_cast<u8>((lengths[i] + 17 - code) % 17);
            ++i;
        } else {
            return Error::InvalidFormat;
        }
    }
    return Error::None;
}

Error Decoder::start_block() {
    const u32 type = reader_.read(3);
    // 24-bit uncompressed block length, most significant bits first. Split
    // 16 + 8 only because the bit buffer tops out at 17 bits per read.
    const u32 hi = reader_.read(16);
    const u32 lo = reader_.read(8);
    block_remaining_ = (hi << 8) | lo;
    block_length_odd_ = (block_remaining_ & 1) != 0;

    switch (type) {
        case 1: block_type_ = BlockType::Verbatim; break;
        case 2: block_type_ = BlockType::AlignedOffset; break;
        case 3: block_type_ = BlockType::Uncompressed; break;
        default: return Error::InvalidFormat;
    }
    if (block_remaining_ == 0) return Error::InvalidFormat;

    if (block_type_ == BlockType::Uncompressed) {
        // Leaves the bitstream: realign to a 16-bit boundary, then read the
        // three LRU offsets as plain little-endian u32 before the payload.
        reader_.realign16();
        std::array<u8, 12> lru{};
        if (!reader_.read_raw(lru.data(), lru.size())) return Error::InvalidFormat;
        r0_ = static_cast<u32>(read_i32le(lru.data() + 0));
        r1_ = static_cast<u32>(read_i32le(lru.data() + 4));
        r2_ = static_cast<u32>(read_i32le(lru.data() + 8));
        return Error::None;
    }

    if (block_type_ == BlockType::AlignedOffset) {
        // Aligned tree comes first, 8 symbols of 3 raw bits each.
        for (u32 i = 0; i < kAlignedTreeSymbols; ++i) {
            aligned_lengths_[i] = static_cast<u8>(reader_.read(3));
        }
        if (!aligned_table_.build(aligned_lengths_.data(), kAlignedTreeSymbols)) {
            return Error::InvalidFormat;
        }
    }

    // The main tree is delta-coded in two independently pre-tree'd halves:
    // the 256 literal symbols, then the position/length symbols.
    if (Error e = read_lengths(main_lengths_.data(), 0, 256); e != Error::None) return e;
    if (Error e = read_lengths(main_lengths_.data(), 256, main_symbols_); e != Error::None) {
        return e;
    }
    if (!main_table_.build(main_lengths_.data(), main_symbols_)) return Error::InvalidFormat;

    if (Error e = read_lengths(length_lengths_.data(), 0, kLengthTreeSymbols);
        e != Error::None) {
        return e;
    }
    if (!length_table_.build(length_lengths_.data(), kLengthTreeSymbols)) {
        return Error::InvalidFormat;
    }
    return Error::None;
}

// =============================================================================
// Block bodies
// =============================================================================

Error Decoder::decode_symbols(usize count) {
    const bool aligned = (block_type_ == BlockType::AlignedOffset);
    usize produced = 0;

    while (produced < count) {
        auto sym_result = decode_symbol(main_table_);
        if (sym_result.is_err()) return sym_result.error();
        const u32 symbol = sym_result.value();

        if (symbol < 256) {
            window_[window_position_] = static_cast<u8>(symbol);
            window_position_ = (window_position_ + 1) & window_mask_;
            ++produced;
            continue;
        }

        const u32 encoded = symbol - 256;
        const u32 length_header = encoded & kNumPrimaryLengths;
        const u32 position_slot = encoded >> 3;
        if (position_slot >= position_slots_) return Error::InvalidFormat;

        u32 match_length = length_header + kMinMatch;
        if (length_header == kNumPrimaryLengths) {
            auto extra_len = decode_symbol(length_table_);
            if (extra_len.is_err()) return extra_len.error();
            match_length = extra_len.value() + kNumPrimaryLengths + kMinMatch;
        }

        u32 match_offset;
        if (position_slot == 0) {
            match_offset = r0_;
        } else if (position_slot == 1) {
            match_offset = r1_;
            r1_ = r0_;
            r0_ = match_offset;
        } else if (position_slot == 2) {
            match_offset = r2_;
            r2_ = r0_;
            r0_ = match_offset;
        } else {
            const u32 extra = kSlots.extra[position_slot];
            u32 offset_bits;
            if (aligned && extra >= 3) {
                // Aligned-offset blocks split the extra bits: everything but
                // the low three comes from the bitstream, the low three come
                // from the aligned tree.
                const u32 verbatim = (extra > 3) ? reader_.read(extra - 3) : 0;
                auto aligned_sym = decode_symbol(aligned_table_);
                if (aligned_sym.is_err()) return aligned_sym.error();
                offset_bits = (verbatim << 3) | aligned_sym.value();
            } else {
                offset_bits = reader_.read(extra);
            }
            // Slots 0..2 are spent on the LRU, so the encoded position is two
            // higher than the real distance.
            match_offset = kSlots.base[position_slot] + offset_bits - 2;
            r2_ = r1_;
            r1_ = r0_;
            r0_ = match_offset;
        }

        if (match_offset == 0 || match_offset > (window_mask_ + 1)) {
            return Error::InvalidFormat;
        }
        // A match may not reach back before the start of the stream, and may
        // not run past the end of what this call was asked to produce: LZX
        // symbols never straddle a CFDATA boundary, because the next chunk
        // restarts the bitstream on a fresh byte range.
        if (static_cast<u64>(match_offset) > output_position_ + produced) {
            return Error::InvalidFormat;
        }
        if (match_length > count - produced) return Error::InvalidFormat;

        u32 src = (window_position_ - match_offset) & window_mask_;
        for (u32 i = 0; i < match_length; ++i) {
            window_[window_position_] = window_[src];
            window_position_ = (window_position_ + 1) & window_mask_;
            src = (src + 1) & window_mask_;
        }
        produced += match_length;
    }
    return Error::None;
}

Error Decoder::copy_uncompressed(usize count) {
    for (usize i = 0; i < count; ++i) {
        u8 byte = 0;
        if (!reader_.read_raw(&byte, 1)) return Error::InvalidFormat;
        window_[window_position_] = byte;
        window_position_ = (window_position_ + 1) & window_mask_;
    }
    return Error::None;
}

// =============================================================================
// Output and x86 translation
// =============================================================================

void Decoder::emit(usize count, std::vector<u8>& out) {
    const usize window_size = window_.size();
    // The bytes just produced end at window_position_ and are contiguous
    // backwards from it, modulo the window wrap.
    usize start = (window_position_ + window_size - count) % window_size;
    const usize out_base = out.size();
    out.resize(out_base + count);
    const usize first_run = (start + count <= window_size) ? count : (window_size - start);
    std::memcpy(out.data() + out_base, window_.data() + start, first_run);
    if (first_run < count) {
        std::memcpy(out.data() + out_base + first_run, window_.data(), count - first_run);
    }

    // Translation is applied to the emitted copy only. The window keeps the
    // untranslated bytes, because later matches are encoded against the
    // untranslated stream — translating in place inside the window would
    // corrupt every subsequent match that reaches into this region.
    if (!e8_enabled_ || e8_chunk_size_ == 0) return;

    u64 absolute = output_position_;
    usize offset = 0;
    while (offset < count) {
        // Frames are aligned to absolute 32 KB boundaries in the output
        // stream, not to chunk boundaries; in a CAB they coincide, but the
        // framing is what the encoder actually used.
        const usize into_frame = static_cast<usize>(absolute % kFrameSize);
        usize run = kFrameSize - into_frame;
        if (run > count - offset) run = count - offset;
        if (into_frame == 0) {
            apply_x86_translation(out.data() + out_base + offset, run, absolute);
        }
        absolute += run;
        offset += run;
    }
}

void Decoder::apply_x86_translation(u8* data, usize length, u64 absolute_position) const {
    if (absolute_position >= kX86TranslationCutoff) return;
    if (length <= kX86FrameMargin) return;

    const usize end = length - kX86FrameMargin;
    const i32 chunk_size = static_cast<i32>(e8_chunk_size_);
    usize i = 0;
    while (i < end) {
        if (data[i] != 0xE8) {
            ++i;
            continue;
        }
        // The encoder rewrote CALL rel32 operands into absolute targets so
        // that identical calls at different addresses compress alike. Undo it
        // only when the stored value is inside the range the encoder could
        // have produced; anything else is genuine data that happens to start
        // with 0xE8.
        const i32 position = static_cast<i32>(absolute_position + i);
        const i32 absolute_target = read_i32le(data + i + 1);
        if (absolute_target >= -position && absolute_target < chunk_size) {
            const i32 relative = (absolute_target >= 0)
                                     ? (absolute_target - position)
                                     : (absolute_target + chunk_size);
            write_i32le(data + i + 1, relative);
        }
        i += 5;
    }
}

// =============================================================================
// Chunk entry point
// =============================================================================

Result<usize, Error> Decoder::decode_chunk(std::span<const u8> compressed,
                                           usize output_size,
                                           std::vector<u8>& out) {
    if (output_size == 0) return Result<usize, Error>::ok(0);
    if (output_size > kFrameSize) {
        // A CAB chunk never exceeds one frame; a larger claim would break the
        // x86 translation framing silently, so refuse it.
        return Result<usize, Error>::err(Error::InvalidArgument);
    }
    if (compressed.empty()) return Result<usize, Error>::err(Error::InvalidFormat);

    reader_.reset(compressed);
    if (!header_read_) {
        if (Error e = read_stream_header(); e != Error::None) {
            return Result<usize, Error>::err(e);
        }
    }

    usize produced = 0;
    while (produced < output_size) {
        if (block_remaining_ == 0) {
            if (Error e = start_block(); e != Error::None) {
                return Result<usize, Error>::err(e);
            }
        }

        const usize run = (block_remaining_ < output_size - produced)
                              ? block_remaining_
                              : (output_size - produced);

        const Error e = (block_type_ == BlockType::Uncompressed)
                            ? copy_uncompressed(run)
                            : decode_symbols(run);
        if (e != Error::None) return Result<usize, Error>::err(e);

        block_remaining_ -= static_cast<u32>(run);
        produced += run;
        output_position_ += run;

        if (block_remaining_ == 0 && block_type_ == BlockType::Uncompressed &&
            block_length_odd_) {
            // An uncompressed block is padded back to a 16-bit boundary. If
            // the block ends exactly at the chunk end the pad byte is absent
            // and the next chunk realigns anyway, so a failed skip is not an
            // error here.
            (void)reader_.skip_raw(1);
        }
    }

    // Consumption past the end of the chunk means the stream lied about its
    // sizes. Peeking past the end is fine (see BitReader::ensure); removing
    // bits past the end is not.
    if (reader_.consumed_bits() > reader_.size_bits()) {
        return Result<usize, Error>::err(Error::InvalidFormat);
    }

    // output_position_ has already advanced; emit() needs the position of the
    // first byte it is about to copy out.
    output_position_ -= produced;
    emit(produced, out);
    output_position_ += produced;

    return Result<usize, Error>::ok(produced);
}

} // namespace vitreo::compression::lzx
