// SPDX-License-Identifier: LGPL-2.1-only
#include "core/compression/mszip.hpp"

#include <algorithm>
#include <array>
#include <cstring>

namespace vitreo::compression::mszip {

namespace {

constexpr u8 kSignature0 = 'C';
constexpr u8 kSignature1 = 'K';

constexpr u32 kMaxCodeBits = 15;
constexpr u32 kLiteralSymbols = 288;
constexpr u32 kDistanceSymbols = 32;
constexpr u32 kCodeLengthSymbols = 19;
constexpr u32 kEndOfBlock = 256;

/// RFC 1951 §3.2.7: the code-length alphabet is transmitted in this order, not
/// in symbol order. Reading it in symbol order builds a valid-looking but
/// wrong tree, so this permutation is load-bearing.
constexpr std::array<u8, kCodeLengthSymbols> kCodeLengthOrder = {
    16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
};

/// RFC 1951 §3.2.5 length codes 257..285: base length and extra bits.
constexpr std::array<u16, 29> kLengthBase = {
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
    35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258
};
constexpr std::array<u8, 29> kLengthExtra = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
    3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0
};

/// RFC 1951 §3.2.5 distance codes 0..29.
constexpr std::array<u16, 30> kDistanceBase = {
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
    257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289,
    16385, 24577
};
constexpr std::array<u8, 30> kDistanceExtra = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
    7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13
};

/// Canonical Huffman decoder held as the by-length counts and offsets RFC 1951
/// §3.2.2 describes. Trees are rebuilt per dynamic block, so building has to
/// be cheap; decoding walks bit by bit, which is fast enough for the block
/// sizes MSZIP allows.
class HuffmanTable {
public:
    /// Build from path lengths. Returns false for an over-subscribed set.
    ///
    /// An *incomplete* set is accepted only in the one case RFC 1951 permits:
    /// a single code of length 1, which real encoders emit for a distance
    /// alphabet with exactly one used symbol. Any other incomplete set means
    /// the stream is corrupt, and accepting it would let a decode run off into
    /// unassigned codes and silently produce wrong bytes.
    [[nodiscard]] bool build(const u8* lengths, u32 count) noexcept {
        counts_.fill(0);
        for (u32 i = 0; i < count; ++i) {
            if (lengths[i] > kMaxCodeBits) return false;
            ++counts_[lengths[i]];
        }
        counts_[0] = 0;

        // Kraft check, carried as "codes left" the way RFC 1951's own example
        // decoder does: negative means over-subscribed.
        i32 left = 1;
        u32 used = 0;
        for (u32 bits = 1; bits <= kMaxCodeBits; ++bits) {
            left <<= 1;
            left -= static_cast<i32>(counts_[bits]);
            if (left < 0) return false;
            used += counts_[bits];
        }
        if (left > 0 && !(used == 1 && counts_[1] == 1)) return false;

        std::array<u32, kMaxCodeBits + 2> offsets{};
        offsets[1] = 0;
        for (u32 bits = 1; bits <= kMaxCodeBits; ++bits) {
            offsets[bits + 1] = offsets[bits] + counts_[bits];
        }

        symbols_.assign(used, 0);
        for (u32 i = 0; i < count; ++i) {
            if (lengths[i] != 0) symbols_[offsets[lengths[i]]++] = static_cast<u16>(i);
        }
        return true;
    }

    [[nodiscard]] const std::array<u16, kMaxCodeBits + 1>& counts() const noexcept {
        return counts_;
    }
    [[nodiscard]] u16 symbol(u32 index) const noexcept { return symbols_[index]; }
    [[nodiscard]] usize symbol_count() const noexcept { return symbols_.size(); }

private:
    std::array<u16, kMaxCodeBits + 1> counts_{};
    std::vector<u16> symbols_;
};

/// LSB-first bit reader. DEFLATE fills bits from the least significant end of
/// each byte, the opposite of LZX — a reader borrowed from the LZX decoder
/// decodes every Huffman code backwards.
class BitReader {
public:
    explicit BitReader(std::span<const u8> data) noexcept : data_(data) {}

    /// Read @p n raw bits, LSB first. Returns nullopt past the end of input.
    [[nodiscard]] std::optional<u32> bits(u32 n) noexcept {
        while (bit_count_ < n) {
            if (pos_ >= data_.size()) return std::nullopt;
            buf_ |= static_cast<u32>(data_[pos_++]) << bit_count_;
            bit_count_ += 8;
        }
        const u32 value = buf_ & ((n == 32) ? 0xFFFFFFFFu : ((1u << n) - 1u));
        buf_ >>= n;
        bit_count_ -= n;
        return value;
    }

    /// Decode one canonical Huffman code. Codes are transmitted MSB-first
    /// within the LSB-first bit stream, so the code value is accumulated by
    /// shifting each new bit in at the bottom.
    [[nodiscard]] std::optional<u16> decode(const HuffmanTable& table) noexcept {
        u32 code = 0;
        u32 first = 0;
        u32 index = 0;
        for (u32 len = 1; len <= kMaxCodeBits; ++len) {
            auto bit = bits(1);
            if (!bit.has_value()) return std::nullopt;
            code |= *bit;
            const u32 count = table.counts()[len];
            if (code < first + count) {
                const u32 slot = index + (code - first);
                if (slot >= table.symbol_count()) return std::nullopt;
                return table.symbol(slot);
            }
            index += count;
            first = (first + count) << 1;
            code <<= 1;
        }
        return std::nullopt;
    }

    void align_to_byte() noexcept {
        const u32 drop = bit_count_ % 8;
        buf_ >>= drop;
        bit_count_ -= drop;
    }

    /// Consume @p n whole bytes from the stream after align_to_byte().
    [[nodiscard]] bool read_bytes(u8* dst, usize n) noexcept {
        for (usize i = 0; i < n; ++i) {
            auto b = bits(8);
            if (!b.has_value()) return false;
            dst[i] = static_cast<u8>(*b);
        }
        return true;
    }

private:
    std::span<const u8> data_;
    usize pos_ = 0;
    u32 buf_ = 0;
    u32 bit_count_ = 0;
};

/// Fixed literal/length and distance trees, RFC 1951 §3.2.6.
void build_fixed_tables(HuffmanTable& literals, HuffmanTable& distances) {
    std::array<u8, kLiteralSymbols> lit{};
    for (u32 i = 0; i < 144; ++i) lit[i] = 8;
    for (u32 i = 144; i < 256; ++i) lit[i] = 9;
    for (u32 i = 256; i < 280; ++i) lit[i] = 7;
    for (u32 i = 280; i < kLiteralSymbols; ++i) lit[i] = 8;
    (void)literals.build(lit.data(), kLiteralSymbols);

    std::array<u8, kDistanceSymbols> dist{};
    dist.fill(5);
    (void)distances.build(dist.data(), kDistanceSymbols);
}

/// Read the dynamic Huffman header of one block (RFC 1951 §3.2.7).
[[nodiscard]] bool read_dynamic_tables(BitReader& reader,
                                       HuffmanTable& literals,
                                       HuffmanTable& distances) {
    auto hlit = reader.bits(5);
    auto hdist = reader.bits(5);
    auto hclen = reader.bits(4);
    if (!hlit || !hdist || !hclen) return false;

    const u32 literal_count = *hlit + 257;
    const u32 distance_count = *hdist + 1;
    const u32 code_length_count = *hclen + 4;
    if (literal_count > kLiteralSymbols || distance_count > kDistanceSymbols) return false;

    std::array<u8, kCodeLengthSymbols> code_lengths{};
    for (u32 i = 0; i < code_length_count; ++i) {
        auto v = reader.bits(3);
        if (!v.has_value()) return false;
        code_lengths[kCodeLengthOrder[i]] = static_cast<u8>(*v);
    }

    HuffmanTable code_length_table;
    if (!code_length_table.build(code_lengths.data(), kCodeLengthSymbols)) return false;

    // Literal and distance lengths share one run-length-coded sequence, and a
    // repeat may legally straddle the boundary between them, so they are
    // decoded into one array and split afterwards.
    std::array<u8, kLiteralSymbols + kDistanceSymbols> lengths{};
    const u32 total = literal_count + distance_count;
    u32 i = 0;
    while (i < total) {
        auto sym = reader.decode(code_length_table);
        if (!sym.has_value()) return false;
        if (*sym < 16) {
            lengths[i++] = static_cast<u8>(*sym);
            continue;
        }
        u32 repeat = 0;
        u8 value = 0;
        if (*sym == 16) {
            if (i == 0) return false;  // nothing to repeat
            value = lengths[i - 1];
            auto extra = reader.bits(2);
            if (!extra.has_value()) return false;
            repeat = 3 + *extra;
        } else if (*sym == 17) {
            auto extra = reader.bits(3);
            if (!extra.has_value()) return false;
            repeat = 3 + *extra;
        } else {
            auto extra = reader.bits(7);
            if (!extra.has_value()) return false;
            repeat = 11 + *extra;
        }
        if (i + repeat > total) return false;
        for (u32 r = 0; r < repeat; ++r) lengths[i++] = value;
    }

    if (!literals.build(lengths.data(), literal_count)) return false;
    return distances.build(lengths.data() + literal_count, distance_count);
}

} // namespace

Result<std::vector<u8>, Error> inflate_raw(std::span<const u8> compressed,
                                           std::span<const u8> dictionary,
                                           usize expected_size) {
    using R = Result<std::vector<u8>, Error>;

    // Decode into one buffer that starts with the dictionary, so a match
    // reaching back before this block's first byte is an ordinary copy rather
    // than a special case. The prefix is trimmed off at the end.
    std::vector<u8> buffer;
    buffer.reserve(dictionary.size() + expected_size);
    buffer.insert(buffer.end(), dictionary.begin(), dictionary.end());
    const usize prefix = dictionary.size();

    BitReader reader(compressed);

    for (;;) {
        auto final_flag = reader.bits(1);
        auto type = reader.bits(2);
        if (!final_flag || !type) return R::err(Error::InvalidFormat);

        if (*type == 0) {
            // Stored block: LEN and its one's complement, byte aligned.
            reader.align_to_byte();
            std::array<u8, 4> header{};
            if (!reader.read_bytes(header.data(), 4)) return R::err(Error::InvalidFormat);
            const u32 len = static_cast<u32>(header[0]) | (static_cast<u32>(header[1]) << 8);
            const u32 nlen = static_cast<u32>(header[2]) | (static_cast<u32>(header[3]) << 8);
            if ((len ^ 0xFFFFu) != nlen) return R::err(Error::InvalidFormat);
            const usize at = buffer.size();
            buffer.resize(at + len);
            if (!reader.read_bytes(buffer.data() + at, len)) return R::err(Error::InvalidFormat);
        } else if (*type == 1 || *type == 2) {
            HuffmanTable literals;
            HuffmanTable distances;
            if (*type == 1) {
                build_fixed_tables(literals, distances);
            } else if (!read_dynamic_tables(reader, literals, distances)) {
                return R::err(Error::InvalidFormat);
            }

            for (;;) {
                auto sym = reader.decode(literals);
                if (!sym.has_value()) return R::err(Error::InvalidFormat);
                if (*sym < kEndOfBlock) {
                    buffer.push_back(static_cast<u8>(*sym));
                    continue;
                }
                if (*sym == kEndOfBlock) break;

                const u32 length_index = *sym - 257;
                if (length_index >= kLengthBase.size()) return R::err(Error::InvalidFormat);
                auto length_extra = reader.bits(kLengthExtra[length_index]);
                if (!length_extra.has_value()) return R::err(Error::InvalidFormat);
                const u32 length = kLengthBase[length_index] + *length_extra;

                auto dist_sym = reader.decode(distances);
                if (!dist_sym.has_value()) return R::err(Error::InvalidFormat);
                if (*dist_sym >= kDistanceBase.size()) return R::err(Error::InvalidFormat);
                auto dist_extra = reader.bits(kDistanceExtra[*dist_sym]);
                if (!dist_extra.has_value()) return R::err(Error::InvalidFormat);
                const u32 distance = kDistanceBase[*dist_sym] + *dist_extra;

                if (distance > buffer.size()) return R::err(Error::InvalidFormat);
                // Overlapping copies are legal and common (run-length coding),
                // so this copies byte by byte rather than with memcpy.
                usize src = buffer.size() - distance;
                for (u32 n = 0; n < length; ++n) buffer.push_back(buffer[src++]);
            }
        } else {
            return R::err(Error::InvalidFormat);  // reserved block type
        }

        if (*final_flag != 0) break;
    }

    if (buffer.size() - prefix != expected_size) return R::err(Error::InvalidFormat);
    return R::ok(std::vector<u8>(buffer.begin() + static_cast<std::ptrdiff_t>(prefix),
                                 buffer.end()));
}

Result<usize, Error> Decoder::decode_chunk(std::span<const u8> compressed,
                                           usize output_size,
                                           std::vector<u8>& out) {
    using R = Result<usize, Error>;

    if (output_size > kBlockSize) return R::err(Error::InvalidFormat);
    if (compressed.size() < 2) return R::err(Error::InvalidFormat);
    if (compressed[0] != kSignature0 || compressed[1] != kSignature1) {
        return R::err(Error::InvalidFormat);
    }

    auto decoded = inflate_raw(compressed.subspan(2),
                               std::span<const u8>(history_.data(), history_.size()),
                               output_size);
    if (decoded.is_err()) return R::err(decoded.error());

    const std::vector<u8>& bytes = decoded.value();
    out.insert(out.end(), bytes.begin(), bytes.end());

    // Carry the trailing 32 KB forward as the next block's dictionary.
    history_.insert(history_.end(), bytes.begin(), bytes.end());
    if (history_.size() > kBlockSize) {
        history_.erase(history_.begin(),
                       history_.end() - static_cast<std::ptrdiff_t>(kBlockSize));
    }

    return R::ok(bytes.size());
}

} // namespace vitreo::compression::mszip
