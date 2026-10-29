// SPDX-License-Identifier: LGPL-2.1-only

/// @file test_segment_override_rewrite.cpp
/// @brief Which bytes native64's guest GS->FS translation is allowed to write.
///
/// The rewrite mutates loaded guest images in place, so every byte it touches
/// that is not a real instruction is silent image corruption. These are pure
/// byte transformations -- nothing is executed -- but the rewrite only exists
/// on the Intel backend, so they build with it.

#include <gtest/gtest.h>

#include "core/cpu/x64/segment_override_rewrite.hpp"

#include <vector>

using namespace vitreo;
using namespace vitreo::cpu::x64;

namespace {

/// `mov rax, gs:[0x30]` -- the TEB read the translation exists for.
constexpr u8 kMovRaxGs30[] = {0x65, 0x48, 0x8B, 0x04, 0x25,
                              0x30, 0x00, 0x00, 0x00};

/// `jmp qword ptr [rax*8 + 0]` -- how MSVC dispatches a switch, and the
/// instruction its jump table follows.
constexpr u8 kJmpTableDispatch[] = {0xFF, 0x24, 0xC5, 0x00, 0x00, 0x00, 0x00};

}  // namespace

TEST(SegmentOverrideRewrite, TranslatesTebReadInsideAPdataRange) {
    std::vector<u8> code(std::begin(kMovRaxGs30), std::end(kMovRaxGs30));
    const CodeRange range{0, code.size()};

    EXPECT_EQ(rewrite_segment_overrides_ranges(code.data(), code.size(),
                                               &range, 1),
              1u);
    EXPECT_EQ(code[0], 0x64) << "guest GS must become the host FS carrier";
}

// A switch's jump table lives in .text directly after the indirect jmp that
// reads it, inside the same function's .pdata extent -- so "every byte inside
// a named function is code" is false. Decoding through the table
// desynchronises: entry bytes 40 65 0A 00 read as REX + a GS prefix + a
// memory operand, and rewriting that 0x65 changes the address the switch
// jumps to. Steam's protobuf parsing died exactly that way, jumping into a
// read-write data buffer.
TEST(SegmentOverrideRewrite, DoesNotRewriteAJumpTableInsideAPdataRange) {
    std::vector<u8> code;
    code.insert(code.end(), std::begin(kMovRaxGs30), std::end(kMovRaxGs30));
    code.insert(code.end(), std::begin(kJmpTableDispatch),
                std::end(kJmpTableDispatch));
    const usize table_offset = code.size();
    // Two 4-byte RVA entries that happen to contain 0x65 and 0x64.
    const u8 table[] = {0x40, 0x65, 0x0A, 0x00, 0x50, 0x64, 0x0A, 0x00};
    code.insert(code.end(), std::begin(table), std::end(table));

    // .pdata names the whole thing as one function, table included.
    const CodeRange range{0, code.size()};
    const usize rewritten =
        rewrite_segment_overrides_ranges(code.data(), code.size(), &range, 1);

    EXPECT_EQ(code[0], 0x64) << "the real TEB read is still translated";
    EXPECT_EQ(rewritten, 1u) << "only the TEB read may be rewritten";
    for (usize i = 0; i < sizeof(table); ++i) {
        EXPECT_EQ(code[table_offset + i], table[i])
            << "jump table byte " << i << " was modified";
    }
}

// The bytes after a `ret` are padding or literals, not the next instruction.
TEST(SegmentOverrideRewrite, DoesNotRewritePastAReturnInsideAPdataRange) {
    std::vector<u8> code{0xC3};  // ret
    const u8 literal[] = {0x40, 0x65, 0x0A, 0x00};
    code.insert(code.end(), std::begin(literal), std::end(literal));

    const CodeRange range{0, code.size()};
    EXPECT_EQ(rewrite_segment_overrides_ranges(code.data(), code.size(),
                                               &range, 1),
              0u);
    for (usize i = 0; i < sizeof(literal); ++i) {
        EXPECT_EQ(code[1 + i], literal[i]);
    }
}

// A branch over a literal pool still reaches the code on the far side, so
// stopping at terminators must not cost real TEB reads.
TEST(SegmentOverrideRewrite, FollowsABranchOverALiteralPool) {
    std::vector<u8> code{0xEB, 0x04};              // jmp +4
    const u8 literal[] = {0x40, 0x65, 0x0A, 0x00}; // pool, must stay intact
    code.insert(code.end(), std::begin(literal), std::end(literal));
    const usize target = code.size();
    code.insert(code.end(), std::begin(kMovRaxGs30), std::end(kMovRaxGs30));

    const CodeRange range{0, code.size()};
    EXPECT_EQ(rewrite_segment_overrides_ranges(code.data(), code.size(),
                                               &range, 1),
              1u);
    EXPECT_EQ(code[target], 0x64) << "the branch target must still translate";
    for (usize i = 0; i < sizeof(literal); ++i) {
        EXPECT_EQ(code[2 + i], literal[i]) << "literal pool byte " << i;
    }
}

// An extent .pdata does not describe is still swept linearly. When it belongs
// to a real MODULE its bytes are not a pure instruction stream, so the sweep
// must accept only the TEB-access shape -- exactly the gate sweep_gaps() uses.
// libcef.dll takes this path on every Steam process and access-violated on
// Intel where ARM never faulted at all.
TEST(SegmentOverrideRewrite, LinearSweepOfAModuleTakesOnlyTebShapes) {
    std::vector<u8> code(std::begin(kMovRaxGs30), std::end(kMovRaxGs30));
    const usize data_offset = code.size();
    // Data that desynchronises into a GS-prefixed memory operand:
    // 40 = REX, 65 = GS prefix, 0A 00 = or al,[rax].
    const u8 data[] = {0x40, 0x65, 0x0A, 0x00, 0x40, 0x64, 0x0A, 0x00};
    code.insert(code.end(), std::begin(data), std::end(data));

    std::vector<u8> unrestricted = code;
    EXPECT_GT(rewrite_segment_overrides(unrestricted.data(),
                                        unrestricted.size(),
                                        /*teb_shape_only=*/false),
              1u)
        << "the unrestricted sweep is expected to write past the real "
           "instruction; that is why the gate exists";

    EXPECT_EQ(rewrite_segment_overrides(code.data(), code.size(),
                                        /*teb_shape_only=*/true),
              1u);
    EXPECT_EQ(code[0], 0x64) << "the real TEB read is still translated";
    for (usize i = 0; i < sizeof(data); ++i) {
        EXPECT_EQ(code[data_offset + i], data[i])
            << "module data byte " << i << " was modified";
    }
}
