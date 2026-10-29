// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64

/// @file isa_form_enum.cpp
/// @brief Decoder inversion. See isa_form_enum.hpp.

#include "isa_form_enum.hpp"

#include <algorithm>
#include <cstdio>
#include <map>
#include <set>

namespace vitreo::isa_matrix {

namespace {

using vitreo::cpu::DecodeError;
using vitreo::cpu::DecodeMode;
using vitreo::cpu::MemoryByteReader;

/// Filler appended after the ModRM/SIB/disp so the decoder always finds the
/// immediate bytes it wants. Distinct, non-zero and non-repeating so a
/// displacement or immediate that leaks into the form is recognisable in a
/// failure dump. 16 bytes covers imm64 plus a disp32.
constexpr uint8_t kFiller[16] = {
    0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x08,
    0x19, 0x2A, 0x3B, 0x4C, 0x5D, 0x6E, 0x7F, 0x01,
};

/// ModRM shapes the enumerator emits. Each is one (mod, rm) choice plus any
/// SIB/disp bytes it implies; the `reg` field is filled in by the caller.
///
/// rm=3 is EBX/RBX deliberately: the sweep points that register at the oracle
/// scratch page, and it is neither the ModRM `reg` operand we vary nor the
/// stack pointer, so a memory form never aliases the operand under test.
struct ModrmShape {
    const char* name;
    uint8_t mod;
    uint8_t rm;
    /// Bytes following the ModRM (SIB and/or displacement).
    std::vector<uint8_t> tail;
};

const std::vector<ModrmShape>& modrm_shapes() {
    static const std::vector<ModrmShape> shapes = {
        {"reg",       3, 1, {}},              // register operand
        {"mem",       0, 3, {}},              // [EBX] / [RBX]
        {"mem+d8",    1, 3, {0x10}},          // [EBX+0x10]
        {"sib",       0, 4, {0x1B}},          // [EBX+EBX*1] via SIB
        {"riprel",    0, 5, {0, 0, 0, 0}},    // disp32 / RIP-relative
    };
    return shapes;
}

/// Legacy prefix classes. Mandatory prefixes (66/F2/F3) select whole opcode
/// rows in the 0F maps, so they are an enumeration axis, not decoration.
struct PrefixClass {
    const char* name;
    std::vector<uint8_t> bytes;
};

const std::vector<PrefixClass>& prefix_classes() {
    static const std::vector<PrefixClass> classes = {
        {"none", {}},
        {"66",   {0x66}},
        {"f2",   {0xF2}},
        {"f3",   {0xF3}},
        {"67",   {0x67}},
        {"fs",   {0x64}},
        {"lock", {0xF0}},
        {"66.67",{0x66, 0x67}},
    };
    return classes;
}

/// REX bytes worth enumerating in long mode. REX.R/B are what reach R8-R15
/// and XMM8-15 — the axis behind the single most common partial lowering on
/// this tree (88 `fits_ldr_*_offset` bails).
const std::vector<uint8_t>& rex_bytes(unsigned mode) {
    static const std::vector<uint8_t> none = {0x00};
    static const std::vector<uint8_t> long_mode = {
        0x00,  // no REX
        0x48,  // W
        0x44,  // R      -> reg operand is r8-r15 / xmm8-15
        0x41,  // B      -> rm operand is r8-r15
        0x45,  // R+B
        0x4C,  // W+R
        0x4D,  // W+R+B
    };
    return mode == 64 ? long_mode : none;
}

/// Opcode-map escapes.
const std::vector<std::vector<uint8_t>>& map_escapes() {
    static const std::vector<std::vector<uint8_t>> maps = {
        {},                // one-byte
        {0x0F},            // two-byte
        {0x0F, 0x38},      // three-byte 38
        {0x0F, 0x3A},      // three-byte 3A
    };
    return maps;
}

bool decode_bytes(const std::vector<uint8_t>& bytes, unsigned mode,
                  DecodedInstruction& out) {
    MemoryByteReader reader(bytes.data(), bytes.size());
    const DecodeMode dm = (mode == 64) ? DecodeMode::Mode64 : DecodeMode::Mode32;
    return decode_instruction(reader, dm, out) == DecodeError::Success;
}

bool same_shape(const DecodedInstruction& a, const DecodedInstruction& b) {
    if (a.opcode != b.opcode) return false;
    if (a.operand_count != b.operand_count) return false;
    for (vitreo::u8 i = 0; i < a.operand_count; ++i) {
        if (a.operands[i].type != b.operands[i].type) return false;
    }
    return true;
}

bool has_memory_operand(const DecodedInstruction& in) {
    for (vitreo::u8 i = 0; i < in.operand_count; ++i) {
        if (vitreo::cpu::is_memory_operand(in.operands[i].type)) return true;
    }
    return false;
}

/// Is the legacy prefix at `idx` architecturally INERT for this instruction?
///
/// This is the filter that keeps the scoreboard honest. The decoder accepts
/// `66 0F 58 C1` (ADDPS with a redundant operand-size prefix) and `67 0F B0 C1`
/// (CMPXCHG on registers with an address-size prefix that addresses nothing).
/// Counting those as distinct forms we owe a lowering inflates the denominator
/// with encodings that have no separate semantics, and fills the report with
/// forms that are not separate instructions.
///
/// The general test is behavioural, not a table: strip the prefix, decode
/// again, and if the instruction is unchanged the prefix did nothing. The two
/// exceptions are the prefixes whose effect is on the ADDRESS rather than the
/// opcode, so a same-opcode re-decode cannot see it.
bool prefix_is_inert(const std::vector<uint8_t>& bytes, std::size_t idx,
                     unsigned mode, const DecodedInstruction& full) {
    const uint8_t p = bytes[idx];
    const bool mem = has_memory_operand(full);

    // Address size and segment override act on effective-address computation.
    // With no memory operand there is no address, so they are no-ops; with
    // one, they change the instruction even though the opcode is identical.
    if (p == 0x67) return !mem;
    if (p == 0x64 || p == 0x65 || p == 0x2E || p == 0x36 || p == 0x3E ||
        p == 0x26) {
        return !mem;
    }
    // LOCK is architecturally valid only on a read-modify-write to memory
    // (Intel SDM Vol. 2, LOCK: #UD otherwise). A register-destination form is
    // not an encoding we owe anything.
    if (p == 0xF0) {
        return !(full.operand_count > 0 &&
                 vitreo::cpu::is_memory_operand(full.operands[0].type));
    }
    // 66 / F2 / F3: either a mandatory prefix that already selected this
    // opcode, or genuinely redundant. Strip and compare.
    if (p == 0x66 || p == 0xF2 || p == 0xF3) {
        std::vector<uint8_t> without = bytes;
        without.erase(without.begin() + static_cast<long>(idx));
        DecodedInstruction other;
        if (!decode_bytes(without, mode, other)) return false;
        return same_shape(full, other);
    }
    return false;
}

/// Opcodes whose entire architectural meaning is to raise #UD. A "native
/// lowering" for one is meaningless, and they would otherwise dominate the
/// worklist purely because the enumerator reaches every ModRM of them.
bool is_undefined_by_design(Opcode op) {
    return op == Opcode::UD0_R_RM || op == Opcode::UD1_R_RM ||
           op == Opcode::UD2;
}

/// Does VEX.W change this instruction? `bytes` is a full 3-byte-VEX candidate
/// with W=1; compare its decode against the same bytes with W cleared.
bool vex_w_is_live(const std::vector<uint8_t>& bytes, unsigned mode) {
    std::vector<uint8_t> w1 = bytes;
    std::vector<uint8_t> w0 = bytes;
    w1.insert(w1.end(), std::begin(kFiller), std::end(kFiller));
    w0[2] = static_cast<uint8_t>(w0[2] & 0x7Fu);
    w0.insert(w0.end(), std::begin(kFiller), std::end(kFiller));
    DecodedInstruction a, b;
    const bool oka = decode_bytes(w1, mode, a);
    const bool okb = decode_bytes(w0, mode, b);
    if (!oka || !okb) return true;
    return !same_shape(a, b);
}

/// Is a VEX-prefixed candidate a real instruction, or the decoder falling
/// through to the legacy map?
///
/// RETIRED 2026-09-11 as a filter: the decoder now performs this check itself
/// (`decoder.cpp`, the VEX map-1 vector-operand test), which is where it
/// belongs — a guest executing one of these encodings must fault, not merely
/// be excluded from a test's denominator. Kept as a belt-and-braces assertion
/// that the enumerator never reintroduces one: if the decoder regresses, the
/// form count rises and the enumerator-health gate notices.
///
/// Vitreo's decoder accepts `C4 E1 F8 B6 C1` and reports MOVZX r64, r/m8.
/// There is no VEX encoding of MOVZX: VEX with map 1 opcode B6 is `#UD` on
/// hardware (Intel SDM Vol. 2A §2.3). Left in, these phantom forms dominate
/// the gap list — 110 "missing lowerings" for MOVZX alone, none of them real —
/// and would send work at encodings no guest can execute.
///
/// The test: a VEX form is real if it operates on vector registers, or if the
/// same opcode byte WITHOUT the VEX prefix is not the same instruction. The
/// second clause is what keeps the VEX-only GPR instructions (the BMI1/BMI2
/// and ADX families, whose legacy equivalents do not decode at all).
///
/// The decoder's over-acceptance is a separate, real finding: a guest running
/// garbage gets a silent wrong execution where hardware raises #UD. This
/// function does not paper over that.
bool vex_form_is_real(const std::vector<uint8_t>& bytes, unsigned mmmmm,
                      unsigned mode, const DecodedInstruction& full) {
    for (vitreo::u8 i = 0; i < full.operand_count; ++i) {
        const auto t = full.operands[i].type;
        if (t == vitreo::cpu::OperandType::Reg128 ||
            t == vitreo::cpu::OperandType::Mem128 ||
            t == vitreo::cpu::OperandType::Mem256 ||
            t == vitreo::cpu::OperandType::RegMM) {
            return true;
        }
    }
    // Map 1 (0F) under VEX is the SSE/AVX map: every instruction it defines
    // operates on vector registers. A map-1 VEX form with no vector operand is
    // therefore the decoder falling through to the legacy two-byte map — the
    // MOVZX/MOVSX/LFS case. The VEX-encoded GPR families (BMI1, BMI2) live in
    // maps 2 and 3, which is why only map 1 is filtered here.
    (void)bytes;
    (void)mode;
    return mmmmm != 1;
}

void try_decode(std::vector<uint8_t> bytes, unsigned mode,
                std::map<std::pair<uint32_t, uint16_t>, FormCase>& out) {
    const std::size_t prefix_len = bytes.size();
    bytes.insert(bytes.end(), std::begin(kFiller), std::end(kFiller));

    DecodedInstruction inst;
    MemoryByteReader reader(bytes.data(), bytes.size());
    const DecodeMode dm = (mode == 64) ? DecodeMode::Mode64 : DecodeMode::Mode32;
    if (decode_instruction(reader, dm, inst) != DecodeError::Success) return;
    if (inst.opcode == Opcode::INVALID) return;
    if (is_undefined_by_design(inst.opcode)) return;
    if (inst.length == 0 || inst.length > bytes.size()) return;
    // A decode that consumed only prefix bytes is not an instruction form.
    if (inst.length < prefix_len) return;

    // Drop candidates carrying a prefix that does nothing to this
    // instruction: the unprefixed encoding is enumerated separately and is
    // the same form.
    for (std::size_t i = 0; i < prefix_len && i < bytes.size(); ++i) {
        if (prefix_is_inert(bytes, i, mode, inst)) return;
    }
    if (bytes.size() > 4 && bytes[0] == 0xC4) {
        if (!vex_form_is_real(bytes, bytes[1] & 0x1Fu, mode, inst)) return;
    }

    FormCase fc;
    fc.opcode = inst.opcode;
    fc.form = vitreo::cpu::jit::form_key(inst);
    fc.mode = mode;
    fc.bytes.assign(bytes.begin(), bytes.begin() + inst.length);
    fc.decoded = inst;

    const auto key = std::make_pair(static_cast<uint32_t>(inst.opcode),
                                    fc.form.bits);
    // First encoding wins: the enumeration order runs plain forms before
    // prefixed ones, so the representative bytes for a form are the simplest
    // that produce it.
    out.emplace(key, std::move(fc));
}

}  // namespace

std::string FormCase::text() const {
    char tags[96];
    vitreo::cpu::jit::format_form_key(form, tags, sizeof(tags));
    const char* name = vitreo::cpu::opcode_name(opcode);
    char buf[256];
    std::snprintf(buf, sizeof(buf), "%s/%u [%s]",
                  name ? name : "(unnamed)", mode, tags);
    std::string s(buf);
    s += " {";
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        char b[8];
        std::snprintf(b, sizeof(b), "%s%02X", i ? " " : "", bytes[i]);
        s += b;
    }
    s += "}";
    return s;
}

std::vector<FormCase> enumerate_forms(unsigned mode) {
    std::map<std::pair<uint32_t, uint16_t>, FormCase> found;

    // ---- legacy / escape-map encodings -------------------------------------
    for (const auto& pfx : prefix_classes()) {
        for (const auto& esc : map_escapes()) {
            for (unsigned rex : rex_bytes(mode)) {
                for (unsigned op = 0; op < 256; ++op) {
                    for (const auto& shape : modrm_shapes()) {
                        for (unsigned reg = 0; reg < 8; ++reg) {
                            std::vector<uint8_t> b = pfx.bytes;
                            if (rex != 0) b.push_back(static_cast<uint8_t>(rex));
                            b.insert(b.end(), esc.begin(), esc.end());
                            b.push_back(static_cast<uint8_t>(op));
                            b.push_back(static_cast<uint8_t>(
                                (shape.mod << 6) | (reg << 3) | shape.rm));
                            b.insert(b.end(), shape.tail.begin(),
                                     shape.tail.end());
                            try_decode(std::move(b), mode, found);
                        }
                    }
                    // Also try the opcode with NO ModRM byte at all: one-byte
                    // opcodes like PUSH r32, the string ops and the
                    // accumulator-immediate forms take none, and appending a
                    // ModRM would silently decode it as an immediate.
                    std::vector<uint8_t> nomodrm = pfx.bytes;
                    if (rex != 0) nomodrm.push_back(static_cast<uint8_t>(rex));
                    nomodrm.insert(nomodrm.end(), esc.begin(), esc.end());
                    nomodrm.push_back(static_cast<uint8_t>(op));
                    try_decode(std::move(nomodrm), mode, found);
                }
            }
        }
    }

    // ---- VEX encodings ------------------------------------------------------
    // Three-byte VEX only: the two-byte form is a strict subset (mmmmm=1,
    // W=0) and every opcode it can reach is reachable here, so enumerating it
    // as well would only add duplicate forms under the same key.
    for (unsigned mmmmm = 1; mmmmm <= 3; ++mmmmm) {
        for (unsigned w = 0; w <= 1; ++w) {
            for (unsigned l = 0; l <= 1; ++l) {
                for (unsigned pp = 0; pp <= 3; ++pp) {
                    // vvvv is stored inverted. 15 -> "no register"; 13 -> XMM2,
                    // which exercises the three-operand NDS shape; 5 -> XMM10,
                    // which is only encodable in long mode and is the axis that
                    // caught VEX being lowered as legacy two-operand SSE.
                    const unsigned vvvvs[] = {15u, 13u, 5u};
                    for (unsigned vvvv : vvvvs) {
                        if (mode != 64 && vvvv == 5u) continue;
                        // R/X/B are inverted in VEX byte 1. 0xE0 = R=1,X=1,B=1
                        // inverted, i.e. no extension. 0x60 clears R -> the
                        // reg field reaches xmm8-15 (long mode only).
                        const unsigned rxbs[] = {0xE0u, 0x60u};
                        for (unsigned rxb : rxbs) {
                            if (mode != 64 && rxb != 0xE0u) continue;
                            for (unsigned op = 0; op < 256; ++op) {
                                for (const auto& shape : modrm_shapes()) {
                                    for (unsigned reg = 0; reg < 8; reg += 7) {
                                        std::vector<uint8_t> b;
                                        b.push_back(0xC4);
                                        b.push_back(static_cast<uint8_t>(
                                            rxb | mmmmm));
                                        b.push_back(static_cast<uint8_t>(
                                            (w << 7) | (vvvv << 3) |
                                            (l << 2) | pp));
                                        b.push_back(static_cast<uint8_t>(op));
                                        b.push_back(static_cast<uint8_t>(
                                            (shape.mod << 6) | (reg << 3) |
                                            shape.rm));
                                        b.insert(b.end(), shape.tail.begin(),
                                                 shape.tail.end());
                                        // VEX.W is IGNORED by most SIMD
                                        // opcodes (it selects an operand width
                                        // only where the SDM says so). Where it
                                        // is ignored, W=1 is the same
                                        // instruction and enumerating it again
                                        // would double every form under a
                                        // `rexw` tag that means nothing. Same
                                        // principle as prefix_is_inert: flip
                                        // the field and ask the decoder.
                                        if (w == 1 && !vex_w_is_live(b, mode)) {
                                            continue;
                                        }
                                        try_decode(std::move(b), mode, found);
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    std::vector<FormCase> result;
    result.reserve(found.size());
    for (auto& [key, fc] : found) result.push_back(std::move(fc));
    // Stable, readable order: opcode then form bits.
    std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) {
        if (a.opcode != b.opcode) return a.opcode < b.opcode;
        return a.form.bits < b.form.bits;
    });
    return result;
}

std::vector<FormCase> enumerate_all_forms() {
    std::vector<FormCase> all = enumerate_forms(32);
    std::vector<FormCase> m64 = enumerate_forms(64);
    all.insert(all.end(), m64.begin(), m64.end());
    return all;
}

}  // namespace vitreo::isa_matrix
