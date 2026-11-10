// SPDX-License-Identifier: LGPL-2.1-only
/// @file dxil_bitcode.cpp
/// @brief LLVM bitstream reader (LLVM docs/BitCodeFormat.rst, release 3.7).

#include "dxil_bitcode.hpp"

#include <algorithm>
#include <cstring>
#include <map>
#include <memory>

namespace vitreo::graphics {

namespace {

// Builtin abbreviation IDs (BitCodeFormat.rst, "Abbreviation IDs").
constexpr u32 kEndBlock = 0;
constexpr u32 kEnterSubblock = 1;
constexpr u32 kDefineAbbrev = 2;
constexpr u32 kUnabbrevRecord = 3;

// Abbreviation operand encodings ("DEFINE_ABBREV Encoding").
enum class OpKind : u8 { Literal, Fixed, Vbr, Array, Char6, Blob };

struct AbbrevOp {
    OpKind kind{OpKind::Literal};
    u64 value{0};  // literal value, or the Fixed / VBR width
};
using Abbrev = std::vector<AbbrevOp>;
using AbbrevPtr = std::shared_ptr<const Abbrev>;

// BLOCKINFO record codes ("BLOCKINFO Block").
constexpr u32 kBlockInfoSetBid = 1;
constexpr u32 kBlockInfoBlockId = 0;

class Reader {
public:
    Reader(const u8* data, usize size) : m_data(data), m_bits(size * 8u) {}

    bool fail(const char* why) {
        if (m_error.empty()) m_error = why;
        return false;
    }
    [[nodiscard]] const std::string& error() const { return m_error; }
    [[nodiscard]] bool at_end() const { return m_pos >= m_bits; }

    bool read(u32 width, u64& out) {
        out = 0;
        if (width == 0) return true;
        if (width > 64) return fail("fixed field wider than 64 bits");
        if (m_pos + width > m_bits) return fail("read past the end of the stream");
        for (u32 got = 0; got < width;) {
            const usize byte = m_pos >> 3;
            const u32 bit = static_cast<u32>(m_pos & 7u);
            const u32 take = std::min<u32>(8u - bit, width - got);
            const u64 chunk = (m_data[byte] >> bit) & ((1u << take) - 1u);
            out |= chunk << got;
            got += take;
            m_pos += take;
        }
        return true;
    }

    bool read_vbr(u32 width, u64& out) {
        out = 0;
        if (width < 2 || width > 32) return fail("VBR width out of range");
        const u64 high = u64{1} << (width - 1);
        u32 shift = 0;
        for (;;) {
            u64 piece = 0;
            if (!read(width, piece)) return false;
            if (shift >= 64) return fail("VBR value overflows 64 bits");
            out |= (piece & (high - 1)) << shift;
            if ((piece & high) == 0) return true;
            shift += width - 1;
        }
    }

    bool align32() {
        const usize aligned = (m_pos + 31u) & ~usize{31};
        if (aligned > m_bits) return fail("alignment past the end of the stream");
        m_pos = aligned;
        return true;
    }

    [[nodiscard]] usize position() const { return m_pos; }
    void seek(usize bit) { m_pos = bit; }
    [[nodiscard]] const u8* data() const { return m_data; }
    [[nodiscard]] usize bits() const { return m_bits; }

    /// Abbreviations BLOCKINFO gives each block id.
    std::map<u32, std::vector<AbbrevPtr>> blockinfo;

private:
    const u8* m_data;
    usize m_bits;
    usize m_pos{0};
    std::string m_error;
};

bool read_scalar(Reader& r, const AbbrevOp& op, u64& out) {
    switch (op.kind) {
    case OpKind::Literal: out = op.value; return true;
    case OpKind::Fixed: return r.read(static_cast<u32>(op.value), out);
    case OpKind::Vbr: return r.read_vbr(static_cast<u32>(op.value), out);
    case OpKind::Char6: {
        u64 v = 0;
        if (!r.read(6, v)) return false;
        static const char kTable[] =
            "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._";
        out = static_cast<u8>(kTable[v]);
        return true;
    }
    default: return r.fail("array or blob used as a scalar operand");
    }
}

bool read_abbrev_definition(Reader& r, Abbrev& abbrev) {
    u64 count = 0;
    if (!r.read_vbr(5, count)) return false;
    for (u64 i = 0; i < count; ++i) {
        u64 literal = 0;
        if (!r.read(1, literal)) return false;
        AbbrevOp op;
        if (literal != 0) {
            op.kind = OpKind::Literal;
            if (!r.read_vbr(8, op.value)) return false;
        } else {
            u64 encoding = 0;
            if (!r.read(3, encoding)) return false;
            switch (encoding) {
            case 1:
                op.kind = OpKind::Fixed;
                if (!r.read_vbr(5, op.value)) return false;
                if (op.value > 64) return r.fail("fixed abbreviation wider than 64");
                break;
            case 2:
                op.kind = OpKind::Vbr;
                if (!r.read_vbr(5, op.value)) return false;
                // A VBR of width 0 reads nothing (LLVM treats it as fixed 0).
                if (op.value == 0) op.kind = OpKind::Fixed;
                else if (op.value < 2 || op.value > 32)
                    return r.fail("VBR abbreviation width out of range");
                break;
            case 3: op.kind = OpKind::Array; break;
            case 4: op.kind = OpKind::Char6; break;
            case 5: op.kind = OpKind::Blob; break;
            default: return r.fail("unknown abbreviation operand encoding");
            }
        }
        abbrev.push_back(op);
    }
    // An array is followed by exactly one element operand and ends the
    // abbreviation; a blob ends it.
    for (usize i = 0; i < abbrev.size(); ++i) {
        if (abbrev[i].kind == OpKind::Array && i + 2 != abbrev.size()) {
            return r.fail("array abbreviation operand is not second to last");
        }
        if (abbrev[i].kind == OpKind::Blob && i + 1 != abbrev.size()) {
            return r.fail("blob abbreviation operand is not last");
        }
    }
    if (abbrev.empty()) return r.fail("empty abbreviation");
    return true;
}

bool read_abbreviated(Reader& r, const Abbrev& abbrev, BitcodeRecord& record) {
    bool have_code = false;
    const auto push = [&](u64 v) {
        if (!have_code) {
            record.code = static_cast<u32>(v);
            have_code = true;
        } else {
            record.ops.push_back(v);
        }
    };
    for (usize i = 0; i < abbrev.size(); ++i) {
        const AbbrevOp& op = abbrev[i];
        if (op.kind == OpKind::Array) {
            u64 count = 0;
            if (!r.read_vbr(6, count)) return false;
            const AbbrevOp& element = abbrev[i + 1];
            if (count > r.bits()) return r.fail("array longer than the stream");
            for (u64 k = 0; k < count; ++k) {
                u64 v = 0;
                if (!read_scalar(r, element, v)) return false;
                push(v);
            }
            break;
        }
        if (op.kind == OpKind::Blob) {
            u64 length = 0;
            if (!r.read_vbr(6, length)) return false;
            if (!r.align32()) return false;
            if (r.position() / 8u + length > r.bits() / 8u) {
                return r.fail("blob past the end of the stream");
            }
            if (!have_code) return r.fail("blob as a record code");
            record.has_blob = true;
            record.blob.assign(r.data() + r.position() / 8u,
                               r.data() + r.position() / 8u + length);
            r.seek(r.position() + length * 8u);
            if (!r.align32()) return false;
            break;
        }
        u64 v = 0;
        if (!read_scalar(r, op, v)) return false;
        push(v);
    }
    if (!have_code) return r.fail("abbreviated record without a code");
    return true;
}

bool read_block_body(Reader& r, BitcodeBlock& block, u32 abbrev_width,
                     usize end_bit, u32 depth);

bool read_subblock(Reader& r, BitcodeBlock& parent, u32 depth) {
    u64 id = 0, width = 0, words = 0;
    if (!r.read_vbr(8, id) || !r.read_vbr(4, width) || !r.align32() ||
        !r.read(32, words)) {
        return false;
    }
    if (depth > 64) return r.fail("blocks nested too deeply");
    if (width == 0 || width > 32) return r.fail("abbreviation width out of range");
    const usize body = r.position();
    const usize end = body + static_cast<usize>(words) * 32u;
    if (end > r.bits()) return r.fail("block length past the end of the stream");
    if (id == kBlockInfoBlockId) {
        BitcodeBlock info;
        info.id = 0;
        // BLOCKINFO: abbreviations defined here belong to the block id the
        // last SETBID named.
        return read_block_body(r, info, static_cast<u32>(width), end, depth + 1);
    }
    BitcodeBlock child;
    child.id = static_cast<u32>(id);
    if (!read_block_body(r, child, static_cast<u32>(width), end, depth + 1)) {
        return false;
    }
    parent.entries.push_back({true, static_cast<u32>(parent.blocks.size())});
    parent.blocks.push_back(std::move(child));
    return true;
}

bool read_block_body(Reader& r, BitcodeBlock& block, u32 abbrev_width,
                     usize end_bit, u32 depth) {
    std::vector<AbbrevPtr> abbrevs;
    if (block.id != kBlockInfoBlockId) {
        const auto it = r.blockinfo.find(block.id);
        if (it != r.blockinfo.end()) abbrevs = it->second;
    }
    i64 info_target = -1;  // BLOCKINFO: the SETBID block id
    while (r.position() < end_bit) {
        u64 id = 0;
        if (!r.read(abbrev_width, id)) return false;
        if (id == kEndBlock) {
            if (!r.align32()) return false;
            if (r.position() != end_bit) return r.fail("block length does not match END_BLOCK");
            return true;
        }
        if (id == kEnterSubblock) {
            if (!read_subblock(r, block, depth)) return false;
            continue;
        }
        if (id == kDefineAbbrev) {
            auto abbrev = std::make_shared<Abbrev>();
            if (!read_abbrev_definition(r, *abbrev)) return false;
            if (block.id == kBlockInfoBlockId) {
                if (info_target < 0) return r.fail("BLOCKINFO abbreviation before SETBID");
                r.blockinfo[static_cast<u32>(info_target)].push_back(abbrev);
            } else {
                abbrevs.push_back(abbrev);
            }
            continue;
        }
        BitcodeRecord record;
        record.abbrev = static_cast<u32>(id);
        if (id == kUnabbrevRecord) {
            u64 code = 0, count = 0;
            if (!r.read_vbr(6, code) || !r.read_vbr(6, count)) return false;
            if (count > r.bits()) return r.fail("record longer than the stream");
            record.code = static_cast<u32>(code);
            record.ops.resize(count);
            for (u64 k = 0; k < count; ++k) {
                if (!r.read_vbr(6, record.ops[k])) return false;
            }
        } else {
            const u64 index = id - 4u;
            if (index >= abbrevs.size()) return r.fail("undefined abbreviation ID");
            if (!read_abbreviated(r, *abbrevs[index], record)) return false;
        }
        if (block.id == kBlockInfoBlockId) {
            if (record.code == kBlockInfoSetBid) {
                if (record.ops.empty()) return r.fail("SETBID without a block id");
                info_target = static_cast<i64>(record.ops[0]);
            }
            // BLOCKNAME / SETRECORDNAME carry names only.
            continue;
        }
        block.entries.push_back({false, static_cast<u32>(block.records.size())});
        block.records.push_back(std::move(record));
    }
    return r.fail("block not terminated by END_BLOCK");
}

}  // namespace

BitcodeStream read_bitcode(const u8* data, usize size) {
    BitcodeStream out;
    if (data == nullptr || size < 4) {
        out.error = "bitcode shorter than its magic";
        return out;
    }
    u32 first = 0;
    std::memcpy(&first, data, 4);
    if (first == 0x0B17C0DEu) {
        // Wrapper: [Magic, Version, Offset, Size, CPUType].
        if (size < 20) {
            out.error = "bitcode wrapper shorter than its header";
            return out;
        }
        u32 offset = 0, length = 0;
        std::memcpy(&offset, data + 8, 4);
        std::memcpy(&length, data + 12, 4);
        if (offset > size || length > size - offset) {
            out.error = "bitcode wrapper names bytes past the data";
            return out;
        }
        data += offset;
        size = length;
        if (size < 4) {
            out.error = "wrapped bitcode shorter than its magic";
            return out;
        }
    }
    if (data[0] != 'B' || data[1] != 'C' || data[2] != 0xC0 || data[3] != 0xDE) {
        out.error = "not LLVM bitcode (magic BC C0DE)";
        return out;
    }
    // The stream is a whole number of 32-bit words.
    Reader r(data, size & ~usize{3});
    r.seek(32);
    BitcodeBlock root;
    root.id = ~0u;
    while (!r.at_end()) {
        u64 id = 0;
        if (!r.read(2, id)) break;
        if (id != kEnterSubblock) {
            // Trailing zero padding after the last block is allowed.
            if (id == 0) {
                bool zeros = true;
                for (usize b = r.position() / 8u; b < size; ++b) zeros &= data[b] == 0;
                if (zeros) break;
            }
            r.fail("top-level entry is not a block");
            break;
        }
        if (!read_subblock(r, root, 0)) break;
    }
    if (!r.error().empty()) {
        out.error = r.error();
        return out;
    }
    out.blocks = std::move(root.blocks);
    out.ok = true;
    return out;
}

}  // namespace vitreo::graphics
