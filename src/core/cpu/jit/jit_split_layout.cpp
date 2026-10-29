// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64

#include "core/cpu/jit/jit_split_layout.hpp"
#include "core/cpu/jit/stencils.hpp"

#include <vector>

namespace vitreo::cpu::jit {

namespace {

// A64 PC-relative branch classes (Arm ARM C4.1.65 "Branches, Exception
// Generating and System instructions").
enum class BranchKind : u8 { None, Imm26, Imm19, Imm14, Adr, Other };

struct Branch {
    BranchKind kind{BranchKind::None};
    i64 disp_words{0};
};

constexpr i64 sign_extend(u64 value, unsigned bits) noexcept {
    const u64 m = u64{1} << (bits - 1);
    return static_cast<i64>((value ^ m) - m);
}

Branch decode(u32 w) noexcept {
    // B / BL: x00101 imm26.
    if ((w & 0x7C000000u) == 0x14000000u)
        return {BranchKind::Imm26, sign_extend(w & 0x03FFFFFFu, 26)};
    // B.cond / BC.cond: 0101010 0 imm19 o0 cond.
    if ((w & 0xFF000000u) == 0x54000000u)
        return {BranchKind::Imm19, sign_extend((w >> 5) & 0x7FFFFu, 19)};
    // CBZ / CBNZ: sf 011010 op imm19 Rt.
    if ((w & 0x7E000000u) == 0x34000000u)
        return {BranchKind::Imm19, sign_extend((w >> 5) & 0x7FFFFu, 19)};
    // TBZ / TBNZ: b5 011011 op b40 imm14 Rt.
    if ((w & 0x7E000000u) == 0x36000000u)
        return {BranchKind::Imm14, sign_extend((w >> 5) & 0x3FFFu, 14)};
    // ADR: 0 immlo 10000 immhi Rd, a BYTE offset immhi:immlo (21 bits). Code
    // and literals are word aligned, so an offset that is not a whole number
    // of words is not one this relocator can place. ADRP addresses 4 KiB
    // pages, which a word displacement does not describe at all.
    if ((w & 0x9F000000u) == 0x10000000u) {
        const i64 bytes = sign_extend((((w >> 5) & 0x7FFFFu) << 2) |
                                          ((w >> 29) & 3u), 21);
        if ((bytes & 3) != 0) return {BranchKind::Other, 0};
        return {BranchKind::Adr, bytes / 4};
    }
    if ((w & 0x9F000000u) == 0x90000000u) return {BranchKind::Other, 0};
    // LDR (literal) / LDRSW (literal) / PRFM (literal): opc 011 V 00 imm19,
    // a word offset in the same field as CBZ's.
    if ((w & 0x3B000000u) == 0x18000000u)
        return {BranchKind::Imm19, sign_extend((w >> 5) & 0x7FFFFu, 19)};
    return {};
}

bool fits(BranchKind kind, i64 disp) noexcept {
    switch (kind) {
    case BranchKind::Imm26: return disp >= -(i64{1} << 25) && disp < (i64{1} << 25);
    case BranchKind::Imm19: return disp >= -(i64{1} << 18) && disp < (i64{1} << 18);
    case BranchKind::Imm14: return disp >= -(i64{1} << 13) && disp < (i64{1} << 13);
    case BranchKind::Adr: return disp >= -(i64{1} << 18) && disp < (i64{1} << 18);
    default: return false;
    }
}

// Re-encodes a branch whose new displacement `fits` already admitted, through
// the same checked fields the emitters use (so a disagreement between `fits`
// and a field stops emission instead of masking).
u32 encode(u32 w, BranchKind kind, i64 disp) noexcept {
    constexpr const char* kWho = "relocate_split_branches";
    const i64 bytes = disp * 4;
    switch (kind) {
    case BranchKind::Imm26: return (w & 0xFC000000u) | arm64::imm26(kWho, bytes);
    case BranchKind::Imm19: return (w & 0xFF00001Fu) | (arm64::imm19(kWho, bytes) << 5);
    case BranchKind::Imm14: return (w & 0xFFF8001Fu) | (arm64::imm14(kWho, bytes) << 5);
    case BranchKind::Adr: {
        const u32 imm = arm64::adr_imm21(kWho, bytes);  // immhi:immlo
        return (w & 0x9F00001Fu) | ((imm & 3u) << 29) | ((imm >> 2) << 5);
    }
    default: return w;
    }
}

}  // namespace

bool relocate_split_branches(u32* words, usize count, usize split,
                             i64 cold_delta) noexcept {
    if (split > count || cold_delta < 0) return false;
    struct Patch { usize index; u32 word; };
    std::vector<Patch> patches;
    const auto placed = [&](i64 index) {
        return index < static_cast<i64>(split) ? index : index + cold_delta;
    };
    for (usize i = 0; i < count; ++i) {
        const Branch b = decode(words[i]);
        if (b.kind == BranchKind::None) continue;
        if (b.kind == BranchKind::Other) return false;
        const i64 target = static_cast<i64>(i) + b.disp_words;
        if (target < 0 || target >= static_cast<i64>(count)) return false;
        const bool src_hot = i < split;
        const bool dst_hot = target < static_cast<i64>(split);
        if (src_hot == dst_hot) continue;
        const i64 disp = placed(target) - placed(static_cast<i64>(i));
        if (!fits(b.kind, disp)) return false;
        patches.push_back({i, encode(words[i], b.kind, disp)});
    }
    for (const Patch& p : patches) words[p.index] = p.word;
    return true;
}

}  // namespace vitreo::cpu::jit
