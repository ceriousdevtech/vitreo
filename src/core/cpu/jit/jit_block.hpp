// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64

#pragma once

/// @file jit_block.hpp
/// @brief The vocabulary of a compiled block: what the JIT produces, and how
///        the rest of the runtime talks about it.
///
/// Split out of jit_compiler.hpp, which declared four unrelated things in
/// 3,555 lines. This is the part most of its 65 dependents actually wanted --
/// a run loop asking "is there code for this RIP, and why did the last block
/// exit" has no business seeing the emitter for every SSE opcode.
///
/// Nothing here emits an instruction, and nothing here needs JitCompiler.

#include <vitreo/common.hpp>
#include <core/cpu/state/cpu_state.hpp>
#include <core/cpu/jit/block_cache.hpp>
#include <core/cpu/decode/decoder.hpp>
#include <core/cpu/jit/jit_stats.hpp>

#include <atomic>
#include <array>
#include <cstddef>
#include <cstdlib>
#include <limits>
#include <memory>
#include <vector>

namespace vitreo::cpu::jit {

/// Tri-state x64 guest-base policy shared by runtime setup and codegen.
/// Unset requests a reservation but enables its JIT fast path only when guest
/// protection is hardware-enforced. Explicit 1 permits the software-guarded
/// diagnostic path; explicit 0 disables both.
[[nodiscard]] inline bool linear64_reservation_requested() noexcept {
    return environment_flag_enabled("VITREO_JIT_LINEAR64", true);
}

[[nodiscard]] inline bool linear64_fast_path_requested(
    bool hardware_protected) noexcept {
    const bool explicit_setting =
        std::getenv("VITREO_JIT_LINEAR64") != nullptr;
    return linear64_reservation_requested() &&
           (explicit_setting || hardware_protected);
}

/// Result of JIT block execution
enum class JitExecResult : u8 {
    Continue,       // Block ended normally, fetch next block at cpu.rip
    ThunkCall,      // RIP is in thunk region — return to dispatcher
    Halt,           // HLT instruction
    Unsupported,    // Hit an instruction we can't JIT — fall back
    StaleCode,      // Arena rollover gate closed before this block could execute
};

/// DIAGNOSTIC (perf window only): why generated code returned to the C++
/// dispatcher. Written by generated code into `JitContext::exit_reason` on
/// the exit path it actually takes, and read once by `JitCompiler::execute()`.
///
/// Only the linked-tail exits in stage_b1_inline_block_link.cpp store a
/// reason. Every other return (`emit_epilogue()` from an unsupported
/// instruction, a raised guest exception, an in-block Jcc cold exit that
/// never reached a linked tail, a thunk return, ...) leaves `Unattributed`,
/// which the report cross-tabulates with the returned `JitExecResult`
/// instead of guessing a label.
///
/// The stores are emitted ONLY when `jit_exit_stats_enabled()` (see
/// stage_b1_inline_block_link.cpp) is true for the process, so a run without
/// VITREO_PERF_WINDOW / VITREO_JIT_EXIT_STATS generates byte-identical code
/// to before this diagnostic existed.
enum class JitExitReason : u32 {
    Unattributed = 0,

    // Constant-successor tail (Jcc legs, fall-through, JMP rel, CALL rel):
    // emit_linked_tail_continue() / emit_cont_tail_stub_body().
    ConstTailThunkTarget,    ///< compile-time-known thunk target: no probe
    ConstTailLinkDisabled,   ///< JitContext::block_cache_l1 == nullptr
    ConstTailL1Empty,        ///< L1 slot for the target is null
    ConstTailL1AddrMismatch, ///< L1 slot holds a different guest address
    ConstTailNoCompiledCode, ///< block found, compiled_link_code == nullptr
    ConstTailHopCap,         ///< backward edge hit kMaxLinkedHops

    // RET tail: emit_linked_tail_ret() -> emit_linked_tail_reg(try_rsb=true).
    // Reaching any of these means the 2-deep sticky ring AND the RSB AND L1
    // all failed to supply a host link.
    RetTailLinkDisabled,
    RetTailL1Empty,
    RetTailL1AddrMismatch,
    RetTailNoCompiledCode,
    RetTailHopCap,

    // Indirect JMP/CALL tail: emit_linked_tail_reg(try_rsb=false).
    IndTailLinkDisabled,
    IndTailL1Empty,
    IndTailL1AddrMismatch,
    IndTailNoCompiledCode,
    IndTailHopCap,

    // Plain epilogues that end a block without a linked tail. Named so the
    // dispatcher histogram can say WHICH one, instead of "Unattributed".
    EpilogueRaise,          ///< a raised guest exception exits the block
    EpilogueZeroFillLoop,   ///< the zero-fill loop helper asked for a re-dispatch
    EpilogueUncompilable,   ///< the block was truncated at an instruction it could not compile
    EpilogueBlockEnd,       ///< the block ran off its end with no branch tail

    // DIAGNOSTIC reclassification done by execute(): an L1 miss whose target
    // is inside the API thunk region is an API CALL, not a cache miss. Only
    // a constant tail can know its target is a thunk at compile time; an
    // indirect CALL [IAT] learns it at runtime, after the probe fails.
    IndTailThunkTarget,
    ConstTailThunkTargetRuntime,

    Count
};

/// Compiled native code for one basic block
struct CompiledBlock {
    void* code{nullptr};    // mmap'd executable memory
    u32 code_size{0};       // Size of generated code in bytes
    GuestAddr guest_addr{0};// Start guest address
    u32 guest_size{0};      // Size of guest code covered
    u64 arena_generation{0};// Executable-arena generation owning code
    // Owns decoded instructions whose addresses are embedded by interpreter
    // delegate stencils. Empty for blocks compiled entirely to native code.
    std::shared_ptr<const BasicBlock> decoded_owner;
    // Instructions in this block that got a native lowering (i.e. excluding
    // interpreter-delegate instructions). Diagnostic only: supplies the
    // denominator for JIT coverage telemetry. See jit_coverage.hpp.
    // Deliberately last among the positional fields so existing aggregate
    // initialisations keep mapping to decoded_owner.
    u32 native_insns{0};
    // DIAGNOSTIC: the guest opcode that ends this block, so an exit that
    // stored no reason can still be attributed to a block shape.
    u16 terminal_opcode{0};
    // Split placement (jit_split_layout.hpp): the block's cold part is
    // [code + cold_offset, code + cold_offset + cold_size); cold_size 0 means
    // the block is the one range [code, code + code_size). Descriptive only --
    // nothing enters or patches code through them -- and both fit 16 bits,
    // as a split block lies in one 32 KiB window. They occupy former padding.
    u16 cold_size{0};
    // Hot-dump byte identity registered for this exact compiled block. Kept
    // with executable metadata so an older concurrently executing SMC
    // generation cannot charge its samples to a newly compiled generation.
    u32 hot_dump_variant{~u32{0}};
    // Byte offset of the JIT-to-JIT entry (after STR LR, pin load, and
    // linear-base load). Default skips only the one-instruction LR save.
    u32 link_code_offset{sizeof(u32)};
    // Byte offset of the native guest-CALL landing pad. An out-of-range value
    // is the canonical "not published" sentinel, so dormant scaffolding can
    // never accidentally enter the ordinary dispatcher prologue at offset 0.
    u32 call_code_offset{std::numeric_limits<u32>::max()};
    // Entry-hoisted memory authorization is valid only while execute() holds
    // GuestMemory's protection lease. Such blocks are deliberately unlinked.
    bool requires_protection_lease{false};
    u16 cold_offset{0};     // see cold_size
    // Exact register/base ABI at link_entry(). A linked hop may bypass the
    // prologue only when this equals the active chain layout.
    u64 pin_layout{0};
    // The exact decoded-instruction array whose element addresses this
    // block's interpreter-delegate stencils baked into the arena. Non-null
    // only for delegate-bearing blocks. Held so the array outlives every
    // thread that can still enter this code — including a thread that got
    // here through the process-wide compile memo and never decoded the
    // block itself. Empty for fully native blocks.
    std::shared_ptr<const DecodedInsns::Storage> delegate_insns;

    [[nodiscard]] const u8* cold_code() const noexcept {
        return cold_size != 0 ? static_cast<const u8*>(code) + cold_offset
                              : nullptr;
    }

    [[nodiscard]] void* link_entry() const noexcept {
        return code && !requires_protection_lease &&
                       link_code_offset < code_size &&
                       link_code_offset % sizeof(u32) == 0
                   ? static_cast<u8*>(code) + link_code_offset
                   : nullptr;
    }

    [[nodiscard]] void* call_entry() const noexcept {
        return code && !requires_protection_lease &&
                       call_code_offset < code_size &&
                       call_code_offset % sizeof(u32) == 0
                   ? static_cast<u8*>(code) + call_code_offset
                   : nullptr;
    }
};

/// A narrowing x87 store helper returns both the destination bit image and
/// whether the instruction may commit it. The two-word aggregate is returned
/// in X0/X1 by AAPCS64, letting generated code branch before the guest write.
struct JitFpuStoreResult {
    u64 bits;
    u64 commit;
};
static_assert(sizeof(JitFpuStoreResult) == 16);

/// Retained-decoded-IR budget for the process-wide compile memo.
///
/// Sharing a delegate-bearing block means the memo owns that block's decoded
/// array for the process lifetime, where today it is freed when the compiling
/// thread's BlockCache evicts the entry. That is the one unbounded cost of the
/// sharing, so it is metered rather than assumed small: `memo_ir_admit`
/// reserves and returns false once the budget is spent, at which point
/// delegate blocks simply stop being published and behave exactly as before.
/// Override the cap with VITREO_JIT_MEMO_IR_MB.
[[nodiscard]] bool memo_ir_admit(usize bytes) noexcept;
void memo_ir_release(usize bytes) noexcept;
void memo_ir_reset() noexcept;
[[nodiscard]] usize memo_ir_bytes() noexcept;
/// Shared-table records erased by executable writes (see
/// JitCompileMemo::purge_shared_records_overlapping).
[[nodiscard]] u64 shared_table_smc_purges() noexcept;
void reset_shared_table_smc_purges() noexcept;

/// Refresh `block`'s L1 slot after its published code changed. Defined in
/// block_cache.cpp; declared here because publish_compiled() is the one point
/// every compile path funnels through, and the L1 slot caches
/// compiled_link_code BY VALUE.
void l1_note_published(BasicBlock& block) noexcept;

inline void publish_compiled(BasicBlock& block,
                             const CompiledBlock& compiled) noexcept {
    // A delegate-bearing block's stencils hold raw pointers into one specific
    // decoded array. When this code came from the process-wide compile memo,
    // that array belongs to whichever thread first compiled the block, not to
    // this cache entry's own decode — which is an equal-valued but distinct
    // allocation. Adopt the array the code actually points at, so the cache
    // entry's lifetime keeps the baked pointers valid (and so the two threads
    // stop holding two copies of the same IR).
    if (compiled.delegate_insns &&
        compiled.delegate_insns->size() == block.instruction_count &&
        compiled.delegate_insns != block.instructions.share()) {
        block.instructions = DecodedInsns{compiled.delegate_insns};
    }
    block.compiled_code = compiled.code;
    // Publish the ABI before the entry pointer that makes it reachable to an
    // emitted L1 lookup.
    block.compiled_pin_layout = compiled.pin_layout;
    block.compiled_link_code = compiled.requires_protection_lease
                                   ? nullptr
                                   : compiled.link_entry();
    block.compiled_call_code = compiled.requires_protection_lease
                                   ? nullptr
                                   : compiled.call_entry();
    block.compiled_code_size = compiled.code_size;
    block.compiled_arena_generation = compiled.arena_generation;
    block.compiled_native_insns = compiled.native_insns;
    block.compiled_hot_dump_variant = compiled.hot_dump_variant;
    block.compiled_requires_protection_lease =
        compiled.requires_protection_lease;

    // Once every instruction has a native lowering, the executable block no
    // longer refers to the decoded instruction array. Keep the compact block
    // metadata used by the cache and linked fast path, but release the rich
    // decoder IR. Blocks containing interpreter delegates must retain it
    // because their generated stencils embed DecodedInstruction pointers.
    //
    // `native_insns == instruction_count` was a PROXY for "no delegate stencil
    // baked &instructions[i]". The exact predicate is on the compiled record
    // and is strictly wider. Gated so VITREO_JIT_RELEASE_DECODED=0 restores
    // the old behaviour completely; without the gate here an A/B would still
    // release on this path and understate the change.
    if (release_decoded_enabled() &&
        compiled.decoded_owner == nullptr && compiled.delegate_insns == nullptr) {
        block.instructions.reset();
    }

    // The L1 slot caches compiled_link_code/compiled_pin_layout by value, so
    // it must be refreshed here — the one point every compile path reaches.
    l1_note_published(block);
}

/// Reconstruct the executable record stored compactly in a BlockCache entry.
/// Keep this inverse of publish_compiled() centralized: dispatcher entry must
/// preserve the exact pin ABI and delegate lifetime associated with the code,
/// not silently substitute CompiledBlock defaults.
[[nodiscard]] inline CompiledBlock compiled_from_cache(
    const BasicBlock& block) noexcept {
    CompiledBlock compiled;
    compiled.code = block.compiled_code;
    compiled.code_size = block.compiled_code_size;
    compiled.guest_addr = block.start_addr;
    compiled.guest_size = block.byte_length;
    compiled.arena_generation = block.compiled_arena_generation;
    compiled.native_insns = block.compiled_native_insns;
    compiled.hot_dump_variant = block.compiled_hot_dump_variant;
    compiled.requires_protection_lease =
        block.compiled_requires_protection_lease;
    compiled.pin_layout = block.compiled_pin_layout;
    compiled.delegate_insns = block.instructions.share();
    // An absent or malformed cached link must not silently inherit
    // CompiledBlock's ordinary code+4 entry convention.
    compiled.link_code_offset = compiled.code_size;
    if (block.compiled_code != nullptr && block.compiled_link_code != nullptr) {
        const auto code = reinterpret_cast<uintptr_t>(block.compiled_code);
        const auto link = reinterpret_cast<uintptr_t>(block.compiled_link_code);
        if (link >= code && link - code < compiled.code_size &&
            (link - code) % sizeof(u32) == 0) {
            compiled.link_code_offset = static_cast<u32>(link - code);
        }
    }
    compiled.call_code_offset = compiled.code_size;
    if (block.compiled_code != nullptr && block.compiled_call_code != nullptr) {
        const auto code = reinterpret_cast<uintptr_t>(block.compiled_code);
        const auto call = reinterpret_cast<uintptr_t>(block.compiled_call_code);
        if (call >= code && call - code < compiled.code_size &&
            (call - code) % sizeof(u32) == 0) {
            compiled.call_code_offset = static_cast<u32>(call - code);
        }
    }
    return compiled;
}

/// One entry of a direct-mapped long-mode translation cache: the guest range
/// a granule resolved to, plus the host pointer its base maps to. Padded to 32
/// bytes so emitted code indexes the table with a single shifted add.
struct alignas(32) X64MemSlot {
    u64 base{0};
    u64 end{0};
    u8* host{nullptr};
    u64 reserved{0};
};
static_assert(sizeof(X64MemSlot) == 32,
              "emitted code indexes the slot table with LSL #5");

/// Entries per direct-mapped table, and the granule each entry covers. A
/// 64 KiB granule with 256 entries spans 16 MiB of distinct index values,
/// reducing conflicts between the many hot mappings used by large x64 guests.
/// An entry stores the whole region it resolved to, not just the granule, so
/// one publish serves every later access anywhere in that region.
constexpr u32 X64_SLOT_COUNT = 256;
constexpr u32 X64_SLOT_GRANULE_SHIFT = 16;
constexpr u32 X64_SLOT_INDEX_BITS = 8;

[[nodiscard]] constexpr u32 x64_slot_index(u64 addr) noexcept {
    return static_cast<u32>((addr >> X64_SLOT_GRANULE_SHIFT) &
                            (X64_SLOT_COUNT - 1));
}

} // namespace vitreo::cpu::jit
