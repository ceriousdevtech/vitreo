// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64

/// @file isa_form_enum.hpp
/// @brief The closed set of instruction FORMS, produced by inverting the
///        decoder rather than by hand.
///
/// Every encoding in this tree's
/// tests and fuzzers is hand-authored, overwhelmingly reg-reg with ModRM 0xC1.
/// That is why JIT coverage has only ever been discovered by running guests:
/// no harness has ever asked "which forms exist?".
///
/// This does. It walks the bounded encoding space (opcode maps x mandatory
/// prefixes x REX x ModRM shapes x VEX fields), runs the real decoder over
/// each candidate, and keeps whatever the decoder accepts. The result is the
/// required set: every (Opcode, FormKey, bitness) a guest can actually reach.
///
/// Deliberately NOT a table: a hand-written list is exactly the artefact that
/// drifts from the decoder and produces a scoreboard nobody can trust.

#pragma once

#include "core/cpu/decode/decoder.hpp"
#include "core/cpu/jit/isa_form.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace vitreo::isa_matrix {

using vitreo::cpu::DecodedInstruction;
using vitreo::cpu::Opcode;
using vitreo::cpu::jit::FormKey;

/// One reachable instruction form, with a concrete encoding that produces it.
struct FormCase {
    Opcode opcode{Opcode::INVALID};
    FormKey form{};
    /// 32 or 64.
    unsigned mode{32};
    /// The exact bytes, truncated to the decoded length.
    std::vector<uint8_t> bytes;
    /// The decode of `bytes`, kept so the sweep can seed operands without
    /// re-deriving them.
    DecodedInstruction decoded{};

    [[nodiscard]] std::string text() const;
};

/// Enumerate every form the decoder accepts in one mode.
///
/// Bounded and deterministic: the same build always produces the same list in
/// the same order. Memory operands are emitted as `[EBX]`/`[RBX]` (ModRM
/// mod=0, rm=3) and `[EBX+disp8]` so a sweep can point one register at the
/// oracle scratch page; SIB, RIP-relative and absolute forms are enumerated
/// separately and flagged through FormKey::kRipOrAbs.
[[nodiscard]] std::vector<FormCase> enumerate_forms(unsigned mode);

/// Both modes, 32 first.
[[nodiscard]] std::vector<FormCase> enumerate_all_forms();

}  // namespace vitreo::isa_matrix
