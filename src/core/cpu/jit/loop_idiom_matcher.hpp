// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
//
// WS2 loop-idiom matcher. Pure: reads decoded instructions only (opcode,
// operand shape, register data flow). No byte tables, no hashes, no guest
// addresses other than the block's own start.

#pragma once

#include "core/cpu/jit/block_cache.hpp"
#include "core/cpu/jit/loop_idiom_desc.hpp"

#include <optional>

namespace vitreo::cpu::jit::loop_idiom {

enum class RejectReason : u8 {
    None,
    NotSelfLoop,     ///< last instruction is not a branch back to the block start
    Length,          ///< too few / too many instructions
    Prefix,          ///< LOCK, 67H, segment override, REP on a GPR opcode
    Opcode,          ///< an instruction outside the whitelist
    StackReg,        ///< RSP/ESP/SPL used as an operand or address register
    RipRelative,     ///< RIP-relative memory operand
    LoopCarried,     ///< a written non-induction value is read before its write
    InductionShape,  ///< update width/step/count outside the accepted forms
    Stride,          ///< a stream's per-iteration stride is not +-element width
    Width,           ///< accesses of different widths
    FlagRead,        ///< a Jcc reads flags no instruction of this iteration wrote
    CondUndefined,   ///< a Jcc reads a flag its writer leaves undefined/unchanged
    InBodyBranch,    ///< a branch into the body or a non-exit control transfer
    NoExit,          ///< the loop has no exit
    StreamCount,     ///< more than two loads or more than one store stream
    KindShape,       ///< the streams do not form Fill/Copy/Scan/Compare
    Field,           ///< a value does not fit the descriptor
};

[[nodiscard]] const char* reject_reason_name(RejectReason r) noexcept;

struct MatchConfig {
    bool ordered_gpr{true};     ///< JIT lowers <= 8-byte accesses TSO-ordered
    bool ordered_vector{false}; ///< JIT lowers 16-byte accesses TSO-ordered
};

struct LoopIdiomMatch {
    Desc desc;
    InlineGate gate;
};

struct MatchResult {
    std::optional<LoopIdiomMatch> match;
    RejectReason reason{RejectReason::None};
};

[[nodiscard]] MatchResult match_loop_idiom(const BasicBlock& block,
                                           const MatchConfig& cfg) noexcept;

}  // namespace vitreo::cpu::jit::loop_idiom
