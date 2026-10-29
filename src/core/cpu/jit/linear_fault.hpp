// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
//
// Recoverable JIT fault handling.
// See .docs/guest-base-memory-mapping.md.
//
// In the linear model a bad guest access is a real SIGSEGV/SIGBUS landing on a
// PROT_NONE page inside the 4 GiB reservation. This module installs a handler
// that recognizes such faults, converts them into a guest STATUS_ACCESS_
// VIOLATION on the faulting CpuState (recovered from X19), and redirects the
// thread to a shared trampoline that unwinds the JIT block frame and returns to
// the dispatcher — the same exit software faults already take. Any fault
// outside every registered reservation is chained to the previously installed
// handler (the fatal crash dumper), so genuine host bugs still abort loudly.
//
// The same handler recognizes self-describing x64 TSO ordered-access sites in
// every JIT memory model. It redirects to a register-preserving trampoline;
// MAP_JIT mutation, and the I-cache maintenance that publishes it, then happen
// after signal return.

#pragma once

#include <vitreo/common.hpp>

namespace vitreo::cpu {
struct CpuState;
}  // namespace vitreo::cpu

namespace vitreo::cpu::jit {

/// Register a linear reservation window [base, base+size). Faults inside it are
/// recoverable. Safe to call from any thread; reads in the signal handler are
/// lock-free. A handful of concurrent reservations are supported (tests).
[[nodiscard]] bool register_linear_reservation(u8* base, usize size);

/// Remove a previously registered window (e.g. on GuestMemory teardown).
void unregister_linear_reservation(u8* base);

/// True only while the exact reservation is registered with the recoverable
/// fault handler. Direct-linear lowering must fail closed when this is false.
[[nodiscard]] bool linear_reservation_registered(u8* base, usize size);

/// Register a JIT view (GuestMemory::enable_linear_jit_view): @p view is its
/// alias of guest address 0 and @p window the reservation's size. The handler
/// owns [view - kLinearJitViewGuardBytes, view + window +
/// kLinearJitViewGuardBytes): a fault there from translated code is a lean
/// access the hardware refused, never a host crash. Lock-free reads in the
/// handler, like register_linear_reservation; requires the handler installed.
[[nodiscard]] bool register_linear_jit_view(u8* view, usize window);

/// Remove a registered JIT view before its mapping goes away.
void unregister_linear_jit_view(u8* view);

/// True only while exactly this JIT view is registered. Lean lowering must
/// fail closed (keep the checked path) when this is false.
[[nodiscard]] bool linear_jit_view_registered(u8* view, usize window);

/// Install the recoverable SIGSEGV/SIGBUS handler once, chaining to whatever
/// was installed before (the fatal crash handler). Idempotent.
[[nodiscard]] bool install_linear_fault_handler();

/// Stack-owned registration used by runtime execution. Its destructor removes
/// the fault window before the associated GuestMemory reservation is unmapped.
class LinearReservationRegistration {
public:
    LinearReservationRegistration() = default;
    ~LinearReservationRegistration();
    LinearReservationRegistration(const LinearReservationRegistration&) = delete;
    LinearReservationRegistration& operator=(
        const LinearReservationRegistration&) = delete;

    [[nodiscard]] bool acquire(u8* base, usize size);
    /// Also own the registration of the reservation's JIT view. Requires a
    /// successful acquire(); released by reset() with the reservation.
    [[nodiscard]] bool acquire_jit_view(u8* view, usize window);
    void reset() noexcept;
    [[nodiscard]] explicit operator bool() const noexcept {
        return m_base != nullptr;
    }

private:
    u8* m_base{nullptr};
    u8* m_view{nullptr};
};

/// Record an executable JIT arena range [lo, hi). The handler only recovers a
/// reservation fault whose faulting PC is inside an exact registered range —
/// i.e. a fault from
/// JIT-emitted code, where X19 is a valid CpuState*. Faults from C++ code
/// touching the reservation (loader writes, helpers) have garbage in X19 and
/// must chain to the crash handler, not be "recovered". Read lock-free in the
/// signal handler.
[[nodiscard]] bool note_jit_code_range(uintptr_t lo, uintptr_t hi);

/// Record the immortal page of process-wide memory helpers [lo, hi), which
/// blocks enter by BL/BLR with X30 left pointing back into the calling
/// block. A fault there is attributed through X30: the caller's pin layout
/// and flag-recovery descriptors, not a fixed layout guess. Set once.
void note_shared_mem_helper_range(uintptr_t lo, uintptr_t hi) noexcept;

/// TEST-ONLY seam: called by the linear fault handler, in signal context,
/// with where it is, the faulting PC and the ucontext. A test uses it to land
/// another thread's site patch at that point:
///  * Entry: before anything reads the instruction at that PC -- between the
///    fault and the handler's decode;
///  * BeforePatchSite: after the stale-fault test and the retry recognizers
///    have read that instruction and declined the fault, before the
///    patch-site recognizer reads it again -- between two of the handler's
///    own reads.
/// Null (the default): never called.
enum class LinearFaultTestPoint : u8 { Entry, BeforePatchSite };
using LinearFaultTestHook = void (*)(LinearFaultTestPoint point, uintptr_t pc,
                                     void* ucontext);
void set_linear_fault_test_hook_for_test(LinearFaultTestHook hook) noexcept;

/// Stop recognizing an executable range before its backing MAP_JIT segment is
/// unmapped. Publishes a new immutable handler snapshot; safe-point ownership
/// must ensure no thread is still executing in the range.
[[nodiscard]] bool forget_jit_code_range(uintptr_t lo, uintptr_t hi);

/// True when @p pc belongs to one exact registered JIT arena range. This is the
/// async-signal-safe ownership predicate used by the linear fault handler.
[[nodiscard]] bool jit_code_range_contains(uintptr_t pc) noexcept;

/// How to rebuild the guest's lazy flag record if a fault lands inside one
/// guest-instruction interval.
///
/// The architectural home of the flags is `CpuState::FlagState` — three stores
/// (`flag_op`, `flag_src`, `flag_dst`) that a JIT block pays on every
/// flag-defining instruction. Most of those stores are never read: the record
/// exists so that a *fault*, a helper call or the next block can materialize
/// EFLAGS. When the emitter can name where the record's inputs would be at
/// every point the record could be observed, it can skip the stores and
/// publish this descriptor instead, and the fault handler rebuilds the record
/// from the interrupted register file.
///
/// This is the mechanism `07_FLAGS_ARCHITECTURE.md` requires before a flag
/// representation may survive a fault boundary, and the proof that
/// `03_MILESTONES.md` M5 demands before any state store is removed.
///
/// NOTE the asymmetry with NZCV. Host PSTATE carries N/Z/C/V, which is not
/// enough to rebuild x86 EFLAGS — PF and AF have no ARM equivalent, and CF is
/// inverted for subtractions. So recovery reconstructs the *record*, whose
/// operands determine every x86 flag exactly, rather than trying to translate
/// host flag bits.
struct JitFlagRecovery {
    /// No host register: the operand is an immediate, in the paired field.
    static constexpr u8 kImmediate = 0xFF;
    /// No host register either: the operand is guest GPR `reg - kCpuStateGpr`
    /// (0-15), which this block does not pin, so its architectural value is
    /// in CpuState at every covered instruction. The stencil emitter writes
    /// an unpinned guest register through to CpuState inside the instruction
    /// that defines it (emit_store_gpr64), so the slot is current at any
    /// later fault; both fault paths harvest pins before they read it.
    static constexpr u8 kCpuStateGpr = 0x40;
    [[nodiscard]] static constexpr bool is_cpu_state_gpr(u8 reg) noexcept {
        return reg >= kCpuStateGpr && reg < kCpuStateGpr + 16;
    }

    /// How `flag_dst` is obtained. The two modes exist because the two common
    /// x86 shapes leave the record's operands in genuinely different places.
    enum Mode : u8 {
        /// The instruction wrote its result to a guest register, so that
        /// register's pin still holds `flag_dst`. `ADD/SUB/AND/OR/XOR r,r`.
        DirectDst = 0,
        /// The instruction discarded its result — `CMP` and `TEST` — so
        /// nothing holds `flag_dst`. Both x86 *inputs* survive instead
        /// (neither is written), and the handler recomputes
        /// `flag_dst = dst_field OP src` using `flag_op`. In this mode the
        /// `dst_reg`/`dst_imm` pair carries the FIRST x86 operand, not the
        /// result.
        RecomputeDst = 1,
        /// INC/DEC at 32 or 64 bits (Intel SDM Vol. 2A INC/DEC: OF, SF, ZF,
        /// AF and PF from the result, CF unchanged). `dst_reg` holds the
        /// RESULT; the original operand is result -/+ 1. CF is the one the
        /// instruction inherited, and the producer guarantees CpuState still
        /// holds that carry's source at every covered instruction (the
        /// definition before this one is published, or is the block's entry
        /// state, and nothing in between touches the flag state), so the
        /// handler reads it with `CpuState::lazy_cf` -- the same answer the
        /// JIT's CF capture would have packed -- before it overwrites the
        /// record. `src_reg` is unused (`kImmediate`).
        MergeCf = 2,
    };

    /// `FlagState::FlagOp` as a raw byte. Zero is `FlagOp::None`, which means
    /// this interval elides nothing and the handler must not touch the record.
    u8 flag_op{0};
    /// Host X register holding `flag_src` (the x86 SOURCE operand), or
    /// `kImmediate`.
    u8 src_reg{kImmediate};
    /// `DirectDst`: host X register holding `flag_dst`. `RecomputeDst`: host X
    /// register holding the first x86 operand. `kImmediate` for either.
    u8 dst_reg{kImmediate};
    /// Meaningful low bits of each host register: 32 or 64. A 32-bit guest's
    /// pins carry bits 31:0 only, and a record rebuilt from the full 64-bit
    /// host value would disagree with the interpreter on every flag above the
    /// operand width.
    u8 width{32};
    /// One of `Mode`.
    u8 mode{DirectDst};
    u64 src_imm{0};
    u64 dst_imm{0};
    /// Bytes to ADD to the guest stack pointer when a fault or raise is
    /// attributed to this instruction, in [-32, 31]. An instruction that
    /// moves RSP before its fault-capable access (Tier 0's x64 PUSH r64 keeps
    /// `SUB pin, pin, #8; STR` at zero extra cost) records the move here, and
    /// both fault paths undo it, so the guest sees the precise state: a
    /// faulting PUSH commits nothing (Intel SDM Vol. 2 PUSH;
    /// tools/oracle_probe/stack_access_fault_probe.expected).
    i8 rsp_undo{0};

    [[nodiscard]] bool active() const noexcept { return flag_op != 0; }
    /// Whether the descriptor carries anything a fault path must apply.
    [[nodiscard]] bool published() const noexcept {
        return active() || rsp_undo != 0;
    }
};

/// One exact guest-instruction interval inside a compiled native block. Host
/// offsets are byte offsets from the block's executable base and are half-open.
struct JitRipSite {
    u32 host_begin{0};
    u32 host_end{0};
    GuestAddr guest_rip{0};
    /// Inactive unless this interval elided its flag-record stores.
    JitFlagRecovery flags{};
};

/// Immutable guest-GPR -> host-GPR assignment for one compiled block. A value
/// of 0xFF means the guest register remains authoritative in CpuState.
struct JitPinMap {
    u8 host_reg[16];
};

inline constexpr JitPinMap kNoJitPins{{
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
}};

/// Collision-free identity for the pin ABI at a JIT-to-JIT entry. Eight
/// 5-bit fields identify the guest owning host slots X12, X13, X23..X28
/// (zero means unused, guest+1 otherwise); bit 40 distinguishes pin width.
[[nodiscard]] inline u64
jit_pin_layout_id(const JitPinMap& pins, bool is_64bit) noexcept {
    static constexpr u8 kHostSlots[8] = {12, 13, 23, 24, 25, 26, 27, 28};
    u64 id = is_64bit ? (u64{1} << 40) : 0;
    for (u8 slot = 0; slot < 8; ++slot) {
        for (u8 guest = 0; guest < 16; ++guest) {
            if (pins.host_reg[guest] == kHostSlots[slot]) {
                id |= static_cast<u64>(guest + 1) << (slot * 5);
                break;
            }
        }
    }
    return id;
}

struct JitFaultMetadata {
    JitPinMap pins{kNoJitPins};
    GuestAddr guest_rip{0};
    bool has_guest_rip{false};
    /// Only meaningful when `has_guest_rip` — a fault in block-owned cold or
    /// gap code belongs to no guest instruction and so elides no record.
    JitFlagRecovery flags{};
};

/// Publish immutable host-PC -> guest-RIP and pin metadata for one compiled
/// block.
/// Publication must happen before the executable pointer becomes reachable by
/// another thread. The fault handler reads the resulting map lock-free.
///
/// A block placed split (jit_split_layout.hpp) owns a second range,
/// [code + cold_offset, code + cold_offset + cold_size), above its first;
/// `cold_size` 0 means it has none. Site offsets are from `code` in either
/// range, and the gap between the two belongs to other blocks.
[[nodiscard]] bool publish_jit_rip_block(uintptr_t code, u32 code_size,
                                         const JitRipSite* sites,
                                         u32 site_count,
                                         const JitPinMap& pins,
                                         u32 cold_offset = 0,
                                         u32 cold_size = 0);

/// Recover a compiled block's live pin map and, when @p host_pc belongs to an
/// instruction interval, its precise guest RIP. Async-signal-safe. Returns
/// true when the PC belongs to published block metadata; has_guest_rip
/// distinguishes block-owned cold/gap code from a mapped instruction.
[[nodiscard]] bool jit_fault_metadata_for_pc(uintptr_t host_pc,
                                              JitFaultMetadata& out) noexcept;

/// Recover the guest instruction owning @p host_pc. Async-signal-safe: this
/// performs only lock-free loads and reads immutable metadata.
[[nodiscard]] bool jit_guest_rip_for_pc(uintptr_t host_pc,
                                        GuestAddr& guest_rip) noexcept;

/// The flag-recovery descriptor of the instruction at @p guest_rip in the
/// block owning @p block_pc (any host PC inside that block, hot or cold part).
/// For a SOFTWARE raise: the raise exit of a block that elided flag records
/// publishes its own PC, and the dispatcher rebuilds the record from the
/// flushed guest registers. Returns false when no published block owns the
/// PC; `out.has_guest_rip` says whether that block has a descriptor for the
/// RIP (`out.flags` is then the descriptor, and `out.pins` the block's pins).
[[nodiscard]] bool jit_flag_recovery_for_guest_rip(uintptr_t block_pc,
                                                   GuestAddr guest_rip,
                                                   JitFaultMetadata& out) noexcept;

/// Rebuild the lazy flag record @p fr elides into @p cpu, from the values of
/// its operand registers (ignored for an immediate operand). Shared by the
/// hardware-fault handler, which reads them from the interrupted register
/// file, and the software-raise path, which reads the flushed guest
/// registers. Async-signal-safe. False, with @p cpu untouched, when @p fr is
/// inactive or names an op it cannot recompute.
bool jit_rebuild_flag_record(const JitFlagRecovery& fr, u64 src_value,
                             u64 dst_value, CpuState& cpu) noexcept;

/// Retire every block wholly contained in [lo, hi). The caller must first
/// prevent new JIT admission and drain every arena lease.
void retire_jit_rip_range(uintptr_t lo, uintptr_t hi) noexcept;

/// Marker paired with a fault-patchable x64 TSO access. It is an otherwise
/// unused MOV XZR,XZR, so the SIGBUS handler can identify these exact two-word
/// sites without consulting compiler-owned maps or allocating metadata.
inline constexpr u32 kTsoPatchMarker = 0xAA1F03FFu;

/// The marker-free x64 linear64 scalar site (width 2, 4 or 8, fault-patched
/// TSO), whose fast path also carries no host-page crossing test:
///
///     TBNZ W3, #MixedRead, T   |   CBNZ X3, T     ; check-byte branch
///     ADD  Xa, X14, Xea                           ; host address
///     LDAPR/LDAR/STLR Rt, [Xa]                    ; the site
///
/// The ordered access alignment-faults on any access that crosses a 16-byte
/// granule (FEAT_LSE2), so on every crossing of a 16 KiB host page and of the
/// reservation's end, before it touches memory. The SIGBUS handler rewrites
/// the site's one word to `B variant`, where variant = T - 4 *
/// tso_crossing_variant_words() is the site's crossing variant in the block's
/// cold part: a host-page crossing test that sends a crossing to the checked
/// helper, the fenced plain access, and a final `B site + 4`. The variant is
/// a second precise-RIP interval of its instruction, so a fault of its plain
/// access is attributed like the site's own. JitCompiler's cold-stub emitter
/// lays the variant out immediately before T; the handler verifies the final
/// branch before it patches anything.
///
/// Under VITREO_JIT_COLD_STUB_LEGACY=1 the variant instead publishes the RIP
/// itself (four MOVZ/MOVK and a STR) and is kTsoCrossingVariantWordsLegacy
/// long. jit_cold_stub_legacy() is read once, so the emitter and the handler
/// always agree on the length.
inline constexpr u32 kTsoCrossingVariantWordsLegacy = 11;
inline constexpr u32 kTsoCrossingVariantWordsShared = 6;
/// VITREO_JIT_COLD_STUB_LEGACY=1, read once for the process: the per-site
/// cold memory stubs of before the shared slow path (A/B).
[[nodiscard]] bool jit_cold_stub_legacy() noexcept;
[[nodiscard]] inline u32 tso_crossing_variant_words() noexcept {
    return jit_cold_stub_legacy() ? kTsoCrossingVariantWordsLegacy
                                  : kTsoCrossingVariantWordsShared;
}

/// The range-by-fault linear64 guard (JitConfig::linear64_range_by_fault):
///
///     LSR  X3, Xea, #14                     ; the site: an UNMASKED index
///     LDRB W3, [X15, X3]                    ; faults for any EA >= reservation
///     TBNZ W3, #MixedRead, T  |  CBNZ X3, T ; check-byte branch
///
/// The check map ends at the VA limit, so the LDRB faults for every EA the
/// reservation does not cover, top byte included. The handler rewrites the
/// LSR to `B variant`, the site's range variant: the old reservation test
/// (an outside EA takes the checked helper), the masked index, the check-byte
/// load, and `B` back to the check-byte branch. It sits immediately before
/// the crossing variant when the site has one, else immediately before T.
inline constexpr u32 kRangeVariantWords = 5;

/// A lazy cold memory slot (JitCompiler::cold_lazy_enabled). The site's
/// check-byte branch (or a lean site's guard) targets T:
///
///     T:  BL lazy_entry        ; `B stub` once materialized
///         .word w0..w3         ; the record, never executed
///
/// `lazy_entry` is the block's `LDR X16, [X20, #cold_lazy_entry]; BR X16`.
/// The materialized stub is the eager slow path, variants included and laid
/// out as eagerly, so a materialized T's B lands where an eager check-byte
/// branch would have (the ordinary entry, or a lean site's checked variant).
/// Offsets are in words:
///
///     w0  [15:0]  return label - T        [31:16] raise entry - T
///     w1  [15:0]  check-byte branch - return label (kNone: no such branch)
///         [31:16] a word of the owning instruction's interval - return label
///     w2  site bits (below)
///     w3  [15:0]  live XMM cache entries  [17:16] TsoMode
///         [23:18] linear64 address bits
struct ColdLazyRecord {
    static constexpr u32 kWords = 4;
    static constexpr u16 kNone = 0x8000;
    // w2
    static constexpr u32 kWidthMask = 0xFu;
    static constexpr u32 kStore = 1u << 4;
    static constexpr u32 kSpillNzcv = 1u << 5;
    static constexpr u32 kRestoreNzcv = 1u << 6;
    static constexpr u32 kRangeVariant = 1u << 7;
    static constexpr u32 kCrossingVariant = 1u << 8;
    static constexpr u32 kLeanVariant = 1u << 9;
    static constexpr u32 kLeanLocator = 1u << 10;
    static constexpr unsigned kAddrShift = 11;
    static constexpr unsigned kStoreRegShift = 16;
    static constexpr unsigned kDstShift = 21;
    static constexpr unsigned kKindShift = 26;
    static constexpr u32 kPinsLive = 1u << 29;
    static constexpr u32 kLinearBasePinned = 1u << 30;
    static constexpr u32 kJitView = 1u << 31;

    [[nodiscard]] static constexpr i64 lo(u32 w) noexcept {
        return static_cast<i16>(static_cast<u16>(w));
    }
    [[nodiscard]] static constexpr i64 hi(u32 w) noexcept {
        return static_cast<i16>(static_cast<u16>(w >> 16));
    }
    [[nodiscard]] static constexpr bool has_mixed(u32 w1) noexcept {
        return static_cast<u16>(w1) != kNone;
    }
    [[nodiscard]] static constexpr u32 pack(i64 lo_words, i64 hi_words) noexcept {
        return static_cast<u32>(static_cast<u16>(lo_words)) |
               static_cast<u32>(static_cast<u16>(hi_words)) << 16;
    }
};

/// Number of ordered scalar sites converted to their unaligned-safe fenced
/// form. Exposed for the focused one-fault regression and diagnostics.
[[nodiscard]] u64 tso_backpatch_count() noexcept;

/// Number of range-by-fault sites redirected to their range variant.
[[nodiscard]] u64 range_backpatch_count() noexcept;

/// Number of JIT-view lean sites (JitConfig::linear64_jit_view) redirected to
/// their checked variant after the enforcing view refused an access.
[[nodiscard]] u64 lean_site_patch_count() noexcept;

} // namespace vitreo::cpu::jit
