// SPDX-License-Identifier: LGPL-2.1-only
#pragma once
/// @file dxil_bitcode.hpp
/// @brief LLVM bitstream reader for the bitcode inside a DXIL program.
///
/// DXIL encodes its module as LLVM 3.7 bitcode (DXIL.rst, "LLVM Bitcode
/// version"). This is the generic bitstream layer of that format, written
/// from LLVM's "LLVM Bitcode File Format" document (docs/BitCodeFormat.rst,
/// release 3.7): the magic, abbreviation IDs, ENTER_SUBBLOCK / END_BLOCK /
/// DEFINE_ABBREV / UNABBREV_RECORD, the literal / fixed / VBR / array /
/// char6 / blob operand encodings, and the BLOCKINFO block. The reader
/// materialises the whole stream as a tree: every block keeps its records
/// and sub-blocks in stream order, which the module reader
/// (dxil_module.hpp) walks. Nothing here knows the IR's record codes.

#include "shader_types.hpp"
#include <string>
#include <vector>

namespace vitreo::graphics {

/// One record: its code and operands. A blob operand's bytes are in
/// `blob` (has_blob); every other operand, array elements included, is one
/// entry of `ops` in stream order.
struct BitcodeRecord {
    u32 code{0};
    std::vector<u64> ops;
    bool has_blob{false};
    std::vector<u8> blob;
    /// The abbreviation it was read with (3 = UNABBREV_RECORD).
    u32 abbrev{3};
};

/// One block. `entries` interleaves records and sub-blocks in stream order:
/// a non-negative `child` indexes `blocks`, otherwise `record` indexes
/// `records`.
struct BitcodeBlock {
    u32 id{0};
    std::vector<BitcodeRecord> records;
    std::vector<BitcodeBlock> blocks;
    struct Entry {
        bool is_block{false};
        u32 index{0};
    };
    std::vector<Entry> entries;
};

struct BitcodeStream {
    bool ok{false};
    std::string error;
    /// The top-level blocks (a DXIL module has one, MODULE_BLOCK 8). The
    /// BLOCKINFO block (id 0) is consumed and not listed.
    std::vector<BitcodeBlock> blocks;
};

/// Reads the bitstream at @p data: "BC" 0xC0DE, optionally behind the
/// 0x0B17C0DE wrapper header. Fails (ok == false, error set) on a bad
/// magic, an abbreviation or block length that runs past the data, an
/// undefined abbreviation ID, or a block left unterminated.
[[nodiscard]] BitcodeStream read_bitcode(const u8* data, usize size);

}  // namespace vitreo::graphics
