// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64

/// @file translation_store.hpp
/// @brief Persistent, cross-run store of JIT translations of guest IMAGE code
///        (VITREO_TRANSLATION_STORE=warm, the default; "off" or "0" disables).
///
/// A process that compiles a block of a loaded PE image
/// appends the emitter's output (everything finalize_code reads, in logical
/// unplaced form) to its own segment file; a later process that misses its
/// in-process memo for the same block restores that record and runs the same
/// finalize_code. Placement, linking, memo publication and invalidation are
/// the fresh-compile path, unchanged.
///
/// Correctness rules (each enforced here or at the hook in compile_attempt):
///  - identity: a record is found by (address, FNV of the bytes, size, mode,
///    pin layout, every CompiledKey policy, link enablement, linear mapping,
///    thunk region, JitConfig hash, image base and image identity) inside a
///    directory named by the host build (LC_UUID), host CPU and the VITREO_*
///    environment; adoption also compares the stored guest bytes with the
///    current ones byte for byte.
///  - host addresses: every helper address the emitter bakes is recorded at
///    emission (emit_host_addr_imm64) and relocated by the image slide; a
///    block that bakes any other host value is never stored, and a producer
///    scan refuses a block holding an unrecorded MOV run into the host image.
///  - process stubs: every shared stub the words load from a JitContext slot
///    is recorded (stored_stub, StoredMemHelper) and published by adoption
///    before the block runs; one adoption cannot publish refuses the record.
///  - SMC: only clean image pages (GuestImageRegistry) and only the first
///    translation of an address in a process; anything else compiles fresh.
///  - W^X: records are data, copied into this process's MAP_JIT arena by
///    finalize_code; store files are never mapped executable.
///  - integrity: every record and segment header carries a SipHash-2-4 MAC
///    under a per-capsule secret; any mismatch is refused and compiled fresh.
///  - crash safety: append-only segments, one writer each (flock), readers
///    stop at the first torn record.
///  - size: a per-directory byte cap; producers stop at it, and least
///    recently used segments of dead writers are evicted at activation.
///  - one copy: the first compile waits (bounded) for the index; a fresh
///    block the index already holds is not written again; a process that
///    adds nothing creates no segment; dead segments under 64 MB are merged
///    (deduplicated, re-authenticated, relocated, re-MACed) at activation.
///  - shadow verification: 1 in VITREO_TRANSLATION_STORE_VERIFY adoptions
///    (default 256, "all" = every one) also compiles fresh and compares; a
///    mismatch poisons the directory for every process.
#pragma once

#include <core/cpu/jit/flag_liveness.hpp>
#include <core/cpu/jit/jit_compile_memo.hpp>
#include <core/cpu/jit/jit_config.hpp>
#include <core/cpu/jit/linear_fault.hpp>
#include <core/cpu/jit/translation_store_stats.hpp>
#include <vitreo/types.hpp>

#include <array>
#include <string>
#include <vector>

namespace vitreo::cpu::jit {

/// One host-address MOV run in a block's words.
struct StoredHostReloc {
    u32 word{0};    ///< index of the first MOVZ/MOVN word
    u8 words{0};    ///< length of the run
    u8 reg{0};      ///< destination X register
    u64 value{0};   ///< absolute host address in the emitting process

    bool operator==(const StoredHostReloc&) const = default;
};

/// Process-wide shared stubs (jit_memory_emit_process_stubs.cpp) a block's
/// words reach through a JitContext slot (LDR X16, [X20, #slot]; BLR/BR).
/// The emitter publishes each one the first time a block in the process
/// asks for it; an adopted block never ran the emitter, so the record names
/// them and adoption publishes them before the code can run.
namespace stored_stub {
constexpr u32 kRaiseExit = 1u << 0;
constexpr u32 kCaptureCf = 1u << 1;
constexpr u32 kRsbPush = 1u << 2;
constexpr u32 kLinkProbeShift = 3;      ///< 3 kinds: bits 3..5
constexpr u32 kLinkProbeKinds = 3;
constexpr u32 kDispatchTailShift = 6;   ///< 8 kinds: bits 6..13
constexpr u32 kDispatchTailKinds = 8;
constexpr u32 kHotWideShift = 14;       ///< linear64_hot_wide_slot: bits 14..17
constexpr u32 kHotWideSlots = 4;
constexpr u32 kAll = (1u << 18) - 1;
}  // namespace stored_stub

/// A shared linear64 scalar cold-memory helper the words load from
/// JitContext::linear64_cold_mem. Its body is emitted from a whole access
/// site, which the record does not hold: adoption uses the process's copy
/// when one is published and otherwise refuses the record.
struct StoredMemHelper {
    u32 helper_off{0};
    u8 width{0};
    u8 is_store{0};

    bool operator==(const StoredMemHelper&) const = default;
};

/// Everything finalize_code reads from the emitter, in logical (unplaced)
/// offsets, plus the decoded shape the record was emitted from.
struct StoredEmission {
    struct LeanRange {
        u64 begin{0};
        u64 end{0};
        u32 owner{0};
        bool operator==(const LeanRange&) const = default;
    };
    struct ColdLazy {
        u64 slot_pos{0};
        u64 return_pos{0};
        u64 mixed_pos{0};
        u64 guard_pos{0};
        u8 raise_kind{0};
        u32 packed{0};
        u32 config{0};
        bool operator==(const ColdLazy&) const = default;
    };

    std::vector<u32> words;
    u32 hot_end_pos{0};
    u32 link_entry_pos{0};
    u32 call_entry_pos{0};
    u32 insn_pos_count{0};
    std::vector<u32> insn_arm_pos;
    std::vector<u32> insn_arm_end;
    std::vector<u64> insn_rips;
    std::vector<JitFlagRecovery> insn_flag_recovery;
    std::vector<i8> insn_rsp_undo;
    std::vector<StencilFlagRecovery> stencil_flag_recovery;
    std::vector<LeanRange> lean_ranges;
    std::vector<ColdLazy> cold_lazy;
    u64 raise_entry_plain_pos{0};
    std::array<u64, 5> raise_entry_restore_pos{};
    std::vector<JitCompileMemo::DirectLinkMeta> links;
    std::array<u8, 16> pin_arm{};
    u64 pin_layout{0};
    u32 native_insns{0};
    u16 terminal_opcode{0};
    bool stack_entry_load_proof{false};
    u32 instruction_count{0};
    bool ends_with_branch{false};
    std::vector<StoredHostReloc> relocs;
    u32 process_stubs{0};  ///< stored_stub bits
    std::vector<StoredMemHelper> mem_helpers;

    void clear() noexcept;
};

/// Field-by-field equality (no padding bytes compared).
[[nodiscard]] bool stored_emission_equal(const StoredEmission& a,
                                         const StoredEmission& b) noexcept;
/// Serialize / parse the emission. decode refuses anything inconsistent
/// with its own length.
void encode_stored_emission(const StoredEmission& in, std::vector<u8>& out);
[[nodiscard]] bool decode_stored_emission(const u8* data, usize size,
                                          StoredEmission& out);
/// Offsets and sizes are inside the words; counts within the emitter's
/// limits. Necessary, not sufficient: shadow verification is the backstop.
[[nodiscard]] bool stored_emission_well_formed(const StoredEmission& e) noexcept;

/// The words emit_mov_imm64 produces for (reg, value). Returns the count.
u32 encode_mov_imm64(u8 reg, u64 value, u32 out[4]) noexcept;

/// Lookup identity of one block.
struct StoredKey {
    u64 start_addr{0};
    u64 code_hash{0};
    u64 pin_layout{0};
    u64 image_base{0};
    u64 image_id{0};
    u64 thunk_lo{0};
    u64 thunk_hi{0};
    u64 cfg_hash{0};
    u32 guest_size{0};
    u32 ir_policy{0};
    u32 flags{0};
    u32 reserved{0};

    enum Flag : u32 {
        Is64 = 1u << 0,
        NativeUd2 = 1u << 1,
        ForwardJoin = 1u << 2,
        NativeCallRet = 1u << 3,
        Linear64JitView = 1u << 4,
        InchainThunks = 1u << 5,
        LinkEnabled = 1u << 6,
        LinearMemory = 1u << 7,
    };
    bool operator==(const StoredKey&) const = default;
};
static_assert(sizeof(StoredKey) == 80);

class TranslationStore {
public:
    /// Warm mode and the store opened (directory, key, fingerprint).
    [[nodiscard]] static bool active() noexcept;
    /// Records may be appended (active, not full, no I/O error).
    [[nodiscard]] static bool producing() noexcept;

    /// Find, authenticate and parse the record for @p key whose stored bytes
    /// equal @p guest_bytes, relocated into this process. @p verify says the
    /// caller must compile fresh and compare (shadow verification).
    [[nodiscard]] static bool lookup(const StoredKey& key, const u8* guest_bytes,
                                     StoredEmission& out, bool& verify);
    /// Append a fresh emission. Validates its relocations and refuses one
    /// whose words hold an unrecorded host-image address.
    static void offer(const StoredKey& key, const u8* guest_bytes,
                      const StoredEmission& emission);
    /// Shadow verification result for a lookup that asked for it.
    static void note_verification(const StoredKey& key, bool match);

    /// First-translation-in-process rule (address granularity).
    [[nodiscard]] static bool seen(u64 start_addr) noexcept;
    static void mark_seen(u64 start_addr) noexcept;

    /// Hash of a JitConfig, field by field.
    [[nodiscard]] static u64 config_hash(const JitConfig& cfg) noexcept;

    /// The directory this process uses ("" when inactive).
    [[nodiscard]] static std::string directory();
    /// Write buffered records now.
    static void flush();

    // ---- TEST-ONLY ------------------------------------------------------
    /// Close everything and forget the activation so the next active() call
    /// re-reads the environment (VITREO_TRANSLATION_STORE_DIR etc.).
    static void reset_for_test();
    /// Block until the background index is built.
    static void wait_index_for_test();
    /// Block until activation's eviction and compaction finished.
    static void wait_gc_for_test();
    /// Records in this process's index (0 before it is ready).
    [[nodiscard]] static usize indexed_records_for_test();
    /// Delay the background index build of later activations by @p ms.
    static void set_index_delay_for_test(u32 ms) noexcept;
    /// Range of the host image whose addresses records relocate.
    static void host_image_range(u64& lo, u64& hi) noexcept;
    /// SipHash-2-4 (64-bit output), exposed for the reference-vector test.
    [[nodiscard]] static u64 siphash24(const u8 key[16], const void* data,
                                       usize size) noexcept;
};

}  // namespace vitreo::cpu::jit
