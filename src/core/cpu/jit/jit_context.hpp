// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64

#pragma once

/// @file jit_context.hpp
/// @brief JitContext -- the struct emitted code reaches through pinned X20 --
///        and the byte offsets that generated code hard-codes into it.
///
/// Split out of jit_compiler.hpp. The offsets and their static_asserts belong
/// beside the struct they describe: every one of them is an ABI between this
/// layout and an emitted LDR, and an assert that lives 500 lines from the
/// field it constrains is an assert nobody re-reads when the field moves.
///
/// VITREO_LARGE_FILE_OK: one struct and the ABI asserts that pin its layout.
/// Splitting it would put a static_assert in a different file from the field
/// it constrains, which is the failure mode the split was made to avoid.

#include <core/cpu/jit/jit_block.hpp>
#include <core/cpu/jit/ir/ir_spill_frame.hpp>
#include <core/memory/guest_memory.hpp>

#include <atomic>
#include <array>
#include <cstddef>
#include <functional>

namespace vitreo::cpu::jit {

class JitCompiler;

/// L3, in-chain API thunk calls: what the runtime's serve hook did with the
/// API crossing an in-chain stub handed it (JitContext::inchain_hook). The
/// JIT owns everything about resuming in translated code afterwards; the
/// runtime owns the crossing itself (runtime/execution/thunk_inchain.hpp).
enum class InChainThunkResult : u8 {
    /// Nothing ran: the target is not an in-chain export, the lever is off,
    /// or a diagnostic needs the full path. CpuState is exactly as the stub
    /// published it, so the dispatcher crossing serves it as before.
    NotServed,
    /// The export ran and its RET was emulated (rip = return address); the
    /// chain may resume in translated code if nothing else forbids it.
    Returned,
    /// The export ran and returned, but the thread must pass the dispatcher
    /// before running more translated code (a SuspendThread is pending):
    /// leave execute() with Continue.
    Resume,
    /// The export ran and left the chain's business to the outer dispatcher
    /// (a raise, an unwind, a control transfer, guest code run from inside
    /// the export, a lost arena lease): leave execute(), and the chain loop
    /// breaks to the outer dispatcher.
    BreakOuter,
    /// The return address could not be read (the full path's "stack
    /// underflow during thunk RET").
    Fatal,
};

/// The runtime's in-chain serve function (cli::serve_in_chain_thunk). Runs
/// on the guest thread, inside execute(), with every guest register in
/// CpuState.
using InChainThunkHook = InChainThunkResult (*)(CpuState& cpu, void* env);

/// JitContext::inchain_break: why an in-chain crossing left execute().
enum class InChainBreak : u32 { None = 0, Outer = 1, Fatal = 2 };

/// The zero-slot in-chain slot map ({u64 count = 0}), the default until a
/// dispatcher publishes the registry's (JitCompiler::set_inchain_slot_map).
alignas(8) inline constexpr u64 kInChainEmptySlotMap = 0;
/// log2 of the thunk slab spacing the slot map is indexed by (equal to
/// winapi::ThunkRegistry::kInChainSlotShift; the runtime asserts it).
inline constexpr u32 kInChainSlotShift = 4;

/// Context struct passed to JIT code via pinned register (X20).
/// Contains helper function pointers for operations that can't be
/// done purely in JIT code (memory access through sparse regions, etc.).
struct JitContext {
    struct Read16DiagRegion {
        u64 base{0};
        u64 end{0};
        u64 calls{0};
    };
    struct Read16DiagRip {
        u64 rip{0};
        u64 calls{0};
    };

    GuestMemory* memory;
    u32 (*read32)(JitContext*, GuestAddr);
    void (*write32)(JitContext*, GuestAddr, u32);
    u32 (*eval_cond)(CpuState*, u32);
    u32 (*read8)(JitContext*, GuestAddr);
    void (*write8)(JitContext*, GuestAddr, u32);
    u32 (*read16)(JitContext*, GuestAddr);
    void (*write16)(JitContext*, GuestAddr, u32);

    // Inline memory fast-path: cached region bounds + host base pointer.
    // When guest addr ∈ [region_base, region_end), host access is:
    //   host_base[addr - region_base]
    // This avoids BLR overhead for ~80% of memory accesses.
    u32 region_base{0};    // Guest start of cached region
    u32 region_end{0};     // Guest end (exclusive) of cached region
    u8* region_host_base{nullptr};  // Host pointer at region_base offset

    // Second cached region — updated by slow-path helpers on miss.
    // Covers the most recently accessed non-stack region (.rdata, .data, heap, etc.)
    u32 region2_base{0};
    u32 region2_end{0};
    u8* region2_host_base{nullptr};

    // String-op helpers
    void (*rep_movsd)(CpuState*, GuestMemory*);  // REP MOVSD bulk copy
    void (*rep_stosd)(CpuState*, GuestMemory*);  // REP STOSD bulk fill

    // Flag materialization helper (called before INC/DEC to preserve CF)
    void (*materialize_flags)(CpuState*);  // materialize lazy flags

    // CMPXCHG8B helper
    void (*cmpxchg8b)(CpuState*, GuestMemory*, u64);  // cmpxchg8b(cpu, mem, addr)

    // FPU dispatch helper
    // addr is u64, not u32: x87 memory operands in a 64-bit block carry a
    // full canonical address. It was u32 while compile_fpu was only reachable
    // from 32-bit blocks; wiring it into compile_64bit made the truncation
    // reachable, and it would have silently corrupted any x87 access above 4GB.
    void (*fpu_op)(CpuState*, GuestMemory*, u32, u64);  // fpu_op(cpu, mem, opcode_id, addr)

    // SSE dispatch helper
    void (*sse_op)(CpuState*, GuestMemory*, u32, u32, u64);  // sse_op(cpu, mem, op_id, xmm_dst, addr_or_src)

    // CMPXCHG32 helper
    // addr is u64: a 32-bit CMPXCHG still takes a full 64-bit effective
    // address in long mode. It was u32, and the emitter matched with a MOV
    // W2,W2, so the address silently lost bits 63:32 above 4 GB.
    void (*cmpxchg32)(CpuState*, GuestMemory*, u64, u32);  // cmpxchg32(cpu, mem, addr, reg_val)

    // SHLD32 helper (returns result, sets flags on CpuState)
    u32 (*shld32)(CpuState*, u32, u32, u32);  // shld32(cpu, dest, src, count) -> result

    // 64-bit memory helpers (for 64-bit GPR instructions)
    u64 (*read64)(JitContext*, GuestAddr);
    void (*write64)(JitContext*, GuestAddr, u64);

    // Additional string-op helpers
    void (*rep_movsb)(CpuState*, GuestMemory*);  // REP MOVSB bulk copy

    // LOCK-prefix aware helpers (acquire global LOCK mutex)
    void (*lock_inc32)(CpuState*, GuestMemory*, u64);      // LOCK INC [m32]
    void (*lock_dec32)(CpuState*, GuestMemory*, u64);      // LOCK DEC [m32]
    void (*lock_xadd32)(CpuState*, GuestMemory*, u64, u32);  // LOCK XADD [m32], r32
    void (*lock_cmpxchg32)(CpuState*, GuestMemory*, u64, u32); // LOCK CMPXCHG [m32], r32
    void (*lock_cmpxchg8b)(CpuState*, GuestMemory*, u64);  // LOCK CMPXCHG8B [m64]
    void (*fused_memcpy)(CpuState*, GuestMemory*, u32, u32, u32, u64, u32); // 224
    u32 (*shrd32)(CpuState*, u32, u32, u32);  // 232: shrd32(cpu, dest, src, count)
    // 240: delegate a single instruction to the interpreter. Used as a
    // generic fallback stencil for any opcode the native JIT doesn't emit
    // for, so JIT compilation never gives up mid-block.
    void (*interpret_step)(CpuState*, GuestMemory*, u64);

    // Additional REP-prefix string-op fast paths.
    void (*rep_stosb)(CpuState*, GuestMemory*);   // 248
    void (*rep_stosw)(CpuState*, GuestMemory*);   // 256
    void (*rep_movsw)(CpuState*, GuestMemory*);   // 264
    void (*repe_cmpsb)(CpuState*, GuestMemory*);  // 272 (REP/REPE CMPSB → memcmp)
    void (*repne_scasb)(CpuState*, GuestMemory*); // 280 (REPNE SCASB → strlen)

    // Stage B1: pointer to BlockCache::m_l1[0] — used by the JIT-emitted
    // inline block-link tail to re-enter compiled code without returning
    // to the C++ dispatcher.  nullptr disables the inline lookup at
    // runtime (so a per-thread BlockCache that hasn't been wired in is
    // safe — the codegen still falls through to the standard epilogue).
    void** block_cache_l1{nullptr}; // 288

    // Cached stack region — populated lazily on the first block-entry
    // miss after a generation bump.  Avoids calling
    // get_region_info_cached() (and thus a TLS-protected TLB probe) on
    // every block enter where SP fell out of region2.  Invalidated when
    // stack_region_gen != memory->generation().  These fields are only
    // read/written by C++ code; they don't appear in JIT-emitted asm
    // and therefore don't need a JIT_CTX_OFF_ constant.
    u32 stack_region_base{0};
    u32 stack_region_end{0};
    u8* stack_region_host_base{nullptr};
    u64 stack_region_gen{static_cast<u64>(-1)};

    // 320: Specialized condition-evaluation helpers — one per x86 cc value
    // (0x0..0xF). Each helper materializes lazy flags and returns the result
    // of the corresponding cc test. Specializing eliminates both the
    // cond_code argument setup (movz_w(1, cond)) at every call site and the
    // 16-way switch dispatch inside the generic helper, which dominates
    // mid-extraction CPU.
    u32 (*cond_fns[16])(CpuState*); // 320..447 (16 * 8 bytes)

    // Inline *store* fast-path regions. Deliberately a separate pair from
    // region/region2 above, which back inline loads.
    //
    // A load only needs the target to be mapped; a store additionally needs
    // it to be writable and to hold no compiled code (otherwise the store
    // must go through the helper so notify_executable_write() can invalidate
    // the affected blocks). Rather than testing those conditions in emitted
    // code on every store, they are checked once at publish time: a region
    // only ever lands here if both already hold. The invariant the codegen
    // relies on is therefore:
    //
    //   addr ∈ [wregion_base, wregion_end)  ⇒  the store is permitted and
    //                                          needs no SMC notification
    //
    // which keeps the emitted sequence the same shape as the load path (two
    // bounds checks, no extra branch) and makes the safety argument a
    // property of the publisher rather than of the generated instructions.
    // Anything not covered — read-only pages, code-bearing regions, unmapped
    // addresses — simply misses and falls back to the write helper, which
    // remains the semantic reference.
    //
    // Both slots are retracted when GuestMemory::generation() changes (map /
    // unmap / protect) or when BlockCache::code_generation() changes (a block
    // was compiled into a page that may lie inside a published region).
    u32 wregion_base{0};              // 448
    u32 wregion_end{0};               // 452
    u8* wregion_host_base{nullptr};   // 456
    u32 wregion2_base{0};             // 464
    u32 wregion2_end{0};              // 468
    u8* wregion2_host_base{nullptr};  // 472

    // Linear (guest-base) memory model. When non-null, a 32-bit guest access is
    // host_base + zext(guest_addr) in one UXTW instruction — no bounds check;
    // bad accesses fault into the recoverable handler (linear_fault.*). Set by
    // execute() from GuestMemory::linear_base(). Read from JIT-emitted asm, so
    // it needs a JIT_CTX_OFF_ constant — keep it at a fixed offset (480).
    u8* linear_base{nullptr};              // 480

    // Compiled-code span [code_span_lo, code_span_hi) — first cheap reject.
    // Published from GuestMemory (process-wide). Linear stores outside the
    // span skip the page-map probe. A process-wide [min,max) often covers a
    // heap between two images; those stores then take the 2 MiB chunk
    // counters, then code_page_map (1 byte per 4 KiB). Empty span (hi == 0)
    // means "no code cached".
    u32 code_span_lo{0xFFFFFFFFu};         // 488
    u32 code_span_hi{0};                   // 492

    // Carry-only lazy-flag evaluation for ADC/SBB/INC/DEC. These instructions
    // consume or preserve CF, then replace every other pending arithmetic
    // flag, so fully materializing ZF/SF/PF/OF/AF is unnecessary.
    u32 (*capture_cf)(CpuState*){nullptr}; // 496

    // Bulk implementation of the canonical scalar byte-zeroing loop. Returns
    // non-zero only after it has performed the write and committed CPU state.
    u32 (*zero_fill_loop)(CpuState*, GuestMemory*, u32, u32, u32, u32)
        {nullptr}; // 504

    // 512: fast delegate — execute one *pre-decoded* instruction, skipping the
    // fetch+decode that interpret_step repeats every execution. The JIT bakes a
    // stable pointer to the block's own DecodedInstruction (owned by the block
    // cache, freed together with the compiled code) plus the instruction rip.
    // Re-decode dominated the x64 extraction sample profile; this halves the
    // interpreter-delegate cost. nullptr falls back to interpret_step.
    void (*interpret_decoded)(CpuState*, GuestMemory*,
                              const DecodedInstruction*, u64){nullptr}; // 512

    // Retired: a two-slot read cache for PE32+ mappings, superseded by the
    // address-indexed table below. Kept as padding because every JIT_CTX_OFF_
    // constant after this point is a fixed number that emitted code depends on.
    u64 x64_region_base{0};             // 520
    u64 x64_region_end{0};              // 528
    u8* x64_region_host_base{nullptr};   // 536
    u64 x64_region2_base{0};            // 544
    u64 x64_region2_end{0};             // 552
    u8* x64_region2_host_base{nullptr};  // 560

    // Width-specific full-address cache for 16-bit reads. Keeping this
    // separate prevents unrelated stack/image reads of other widths from
    // evicting hot word tables used by range decoders.
    u64 x64_read16_region_base{0};             // 568
    u64 x64_read16_region_end{0};              // 576
    u8* x64_read16_region_host_base{nullptr};   // 584
    u64 x64_read16_region2_base{0};             // 592
    u64 x64_read16_region2_end{0};              // 600
    u8* x64_read16_region2_host_base{nullptr};  // 608

    // XCHG with a memory operand has implicit LOCK semantics even without an
    // F0 prefix. Keep this at a fixed emitted-code offset.
    void (*atomic_xchg32)(CpuState*, GuestMemory*, u64, u32){nullptr}; // 616

    // LOCK OR/AND/ADD/SUB [m32], r32/imm. Browser guests use these for
    // atomic flag words, work-queue sets, and refcount atomics
    // (Intel SDM Vol. 2: LOCK prefix).
    void (*lock_or32)(CpuState*, GuestMemory*, u64, u32){nullptr};  // 624
    void (*lock_and32)(CpuState*, GuestMemory*, u64, u32){nullptr}; // 632
    void (*lock_add32)(CpuState*, GuestMemory*, u64, u32){nullptr}; // 640
    void (*lock_sub32)(CpuState*, GuestMemory*, u64, u32){nullptr}; // 648
    void (*lock_cmpxchg8)(CpuState*, GuestMemory*, u64, u32){nullptr}; // 656
    // LOCK OR/AND [m16], r16/imm. Browser guests keep 16-bit atomic flag
    // words (for example `lock and/or word ptr [obj+0x4e], imm8`).
    void (*lock_or16)(CpuState*, GuestMemory*, u64, u32){nullptr};  // 664
    void (*lock_and16)(CpuState*, GuestMemory*, u64, u32){nullptr}; // 672

    // Split x87 m32 stores into conversion and commit phases. Generated code
    // performs the checked guest write between these calls, so FSTP changes
    // TOP/tag state only after the memory destination accepted the value.
    JitFpuStoreResult (*fpu_peek_m32)(CpuState*){nullptr}; // 680
    void (*fpu_pop)(CpuState*){nullptr};     // 688

    // Retired with the read pair above, and kept for the same reason. The
    // publish-time invariant it carried — writable, non-executable and free of
    // compiled code, so emitted stores need neither a permission test nor an
    // SMC notification — now belongs to x64_wslots.
    u64 x64_wregion_base{0};             // 696
    u64 x64_wregion_end{0};              // 704
    u8* x64_wregion_host_base{nullptr};  // 712

    // Address of the process-wide published-map pointer (not the map
    // itself). Stores LDR the slot, then the map; the map is null when
    // no MEM_WRITE_WATCH region is live so ordinary stores skip the mark.
    u8* write_watch_pages{nullptr};      // 720

    // Wider REP CMPS/SCAS (Intel SDM: AddressSize=32 → ESI/EDI/ECX).
    void (*repe_cmpsw)(CpuState*, GuestMemory*){nullptr};   // 728
    void (*repe_cmpsd)(CpuState*, GuestMemory*){nullptr};   // 736
    void (*repne_cmpsw)(CpuState*, GuestMemory*){nullptr};  // 744
    void (*repne_cmpsd)(CpuState*, GuestMemory*){nullptr};  // 752
    void (*repe_scasw)(CpuState*, GuestMemory*){nullptr};   // 760
    void (*repe_scasd)(CpuState*, GuestMemory*){nullptr};   // 768
    void (*repne_scasw)(CpuState*, GuestMemory*){nullptr};  // 776
    void (*repne_scasd)(CpuState*, GuestMemory*){nullptr};  // 784
    void (*repe_scasb)(CpuState*, GuestMemory*){nullptr};   // 792
    void (*repne_cmpsb)(CpuState*, GuestMemory*){nullptr};  // 800

    // PDEP / PEXT have no ARM64 equivalent — there is no bit-gather or
    // bit-scatter instruction — so a helper call is the correct lowering
    // rather than an open-coded loop in generated code.
    u64 (*pdep64)(u64 src, u64 mask){nullptr};  // 808
    u64 (*pext64)(u64 src, u64 mask){nullptr};  // 816

    // Generations the write-regions above were validated under. Not read from
    // JIT-emitted asm, so no JIT_CTX_OFF_ constant.
    u64 wregion_mem_gen{static_cast<u64>(-1)};
    u64 wregion_code_gen{static_cast<u64>(-1)};
    int wregion_watch_gen{-1};

    // The owning block cache, so write helpers can test code-freeness before
    // publishing wregion2. Not accessed from JIT-emitted asm.
    BlockCache* block_cache{nullptr};

    // Current CPU state — set by JitCompiler::execute() so slow-path helpers
    // can raise pending exceptions (e.g. STATUS_ACCESS_VIOLATION on unmapped
    // memory access). Not accessed from JIT-emitted asm, so no JIT_CTX_OFF_
    // constant is needed.
    CpuState* cpu{nullptr};

    // Opt-in counters for classifying checked 16-bit reads without emitting
    // one log record per access. These fields are never read by generated
    // code, so extending the diagnostic table does not change JIT offsets.
    bool read16_diag_enabled{false};
    u64 read16_diag_calls{0};
    u64 read16_diag_low_calls{0};
    u64 read16_diag_high_calls{0};
    u64 read16_diag_x86_calls{0};
    u64 read16_diag_x64_calls{0};
    u64 read16_diag_unknown_mode_calls{0};
    u64 read16_diag_distinct_regions{0};
    u64 read16_diag_region_collisions{0};
    u64 read16_diag_rip_collisions{0};
    std::array<Read16DiagRegion, 64> read16_diag_regions{};
    std::array<Read16DiagRip, 64> read16_diag_rips{};

    // The linked-tail hop budget has no field here: it counts in W22
    // (JitCompiler::kMaxLinkedHops / kHopCapTestWord).

    // ---- DIAGNOSTIC (perf window only) ------------------------------------
    // Written by generated code, drained by JitCompiler::execute(). Every
    // JIT_CTX_OFF_* hard-coded offset is <= 816 and therefore unaffected by
    // this insertion, and every field from here on is addressed through
    // offsetof() within ldr/str_w imm12 range.
    //
    // These are ONLY touched when jit_exit_stats_enabled() was true at block
    // compile time (see stage_b1_inline_block_link.cpp); otherwise they stay
    // zero and no generated instruction references them.

    /// Last `JitExitReason` stored by a linked tail. Cleared by execute()
    /// before every entry, so a stale value can never be mis-reported.
    u32 exit_reason{0};

    /// Return-predictor / linked-tail hit counts since the last dispatcher
    /// entry. Bounded by the hop budget (kMaxLinkedHops), so u32 cannot wrap
    /// between drains. Misses are the corresponding JitExitReason buckets.
    u32 ret_sticky0_hits{0};  ///< RET hop served by ret_sticky_*[0]
    u32 ret_sticky1_hits{0};  ///< RET hop served by ret_sticky_*[1]
    u32 ret_rsb_hits{0};      ///< RET hop served by the software RSB
    u32 ret_l1_hits{0};       ///< RET hop that fell through to the L1 probe
    u32 ind_l1_hits{0};       ///< indirect JMP/CALL hop served by L1
    // -----------------------------------------------------------------------

    // Appended: FST/FSTP m64 Inf/NaN/out-of-range slow path (finite normals
    // convert inline via the Float80→binary64 bit path).
    JitFpuStoreResult (*fpu_peek_m64)(CpuState*){nullptr};
    // packed: bits[7:0]=width, bit8=FISTTP (truncate, no RC). Kept as helper:
    // RC modes + integer-indefinite need interpreter parity, not a thin native.
    JitFpuStoreResult (*fpu_peek_int)(CpuState*, u32){nullptr};

    // Appended: REP STOSQ bulk fill (AddressSize=64 → RDI/RCX, store RAX).
    void (*rep_stosq)(CpuState*, GuestMemory*){nullptr};
    // Appended: REP MOVSQ / REPE|REPNE CMPSQ|SCASQ (AddressSize=64 → RSI/RDI/RCX).
    void (*rep_movsq)(CpuState*, GuestMemory*){nullptr};
    void (*repe_cmpsq)(CpuState*, GuestMemory*){nullptr};
    void (*repne_cmpsq)(CpuState*, GuestMemory*){nullptr};
    void (*repe_scasq)(CpuState*, GuestMemory*){nullptr};
    void (*repne_scasq)(CpuState*, GuestMemory*){nullptr};

    // Appended: REP LODS (PE32 AddressSize=32 → ESI/ECX → AL/AX/EAX).
    void (*rep_lodsb)(CpuState*, GuestMemory*){nullptr};
    void (*rep_lodsw)(CpuState*, GuestMemory*){nullptr};
    void (*rep_lodsd)(CpuState*, GuestMemory*){nullptr};

    /// Direct-mapped long-mode translation caches, indexed by guest address.
    /// A pair of linearly-probed slots cannot hold a working set that
    /// interleaves stack, heap and a large data buffer: each access evicts the
    /// region the next one needs, so nearly every access falls back to a
    /// helper call. Indexing by address instead makes the capacity grow with
    /// the number of granules the guest actually touches, at the same probe
    /// cost. Owned by JitCompiler; see X64_SLOT_COUNT.
    X64MemSlot* x64_rslots{nullptr};
    X64MemSlot* x64_wslots{nullptr};

    /// 1 byte per 4 KiB guest page; non-zero iff any JIT cache associated
    /// with this GuestMemory has compiled code there. Linear stores must use
    /// the address-space-wide map so cross-thread SMC cannot bypass the helper.
    u8* code_page_map{nullptr};
    /// 2 MiB chunk counters for the same map. Linear stores whose chunk is
    /// zero skip the per-page probe even when the process-wide [lo,hi) span
    /// covers a heap sitting between two images.
    u32* code_chunk_pages{nullptr};

    /// Process-wide direct-link generation, published at execute() so a
    /// patched BR can refuse a jump after SMC or arena reclaim. Appended —
    /// not read by older fixed JIT_CTX_OFF_* constants.
    u32 link_generation{0};

    /// Software return-stack buffer for 32-bit CALL→RET linked tails.
    /// Index with `rsb_top & (kRsbDepth-1)`; `rsb_top` itself is a free-
    /// running depth proxy (0 = empty). Cleared in execute() when
    /// `link_generation` advances (SMC / arena reclaim).
    static constexpr u32 kRsbDepth = 16;
    struct RsbEntry {
        u64 guest_rip{0};
        void* host_link{nullptr};  // BasicBlock::compiled_link_code
    };
    RsbEntry rsb[kRsbDepth]{};
    u32 rsb_top{0};  // next push index; also used as depth proxy
    u32 rsb_link_generation{0}; // last generation that validated RSB
    u64 active_pin_layout{0}; // ABI shared by every block in the linked chain

    /// 2-deep sticky return ring for 32-bit RET linked tails.
    /// Probed slot0 then slot1 before RSB/L1; MRU-written on every
    /// successful RET hop via `ret_sticky_i` (next write index).
    /// Cleared with the RSB when `link_generation` advances.
    u64 ret_sticky_rip[2]{};
    void* ret_sticky_host[2]{};
    u32 ret_sticky_i{0};

    /// Reserved continue sticky slots (currently unused: continue tails
    /// probe L1 only after a incorrect-hop regression). Cleared with the
    /// RSB when `link_generation` advances.
    u64 cont_sticky_rip[2]{};
    void* cont_sticky_host[2]{};
    u32 cont_sticky_i{0};

    /// API thunk region [lo, hi). Never JIT-compiled; linked-tail indirect
    /// CALL/JMP skips L1 when the target is clearly inside. Empty when
    /// hi == 0 (or hi == lo). Set via JitCompiler::set_thunk_region().
    /// Appended — not read by older fixed JIT_CTX_OFF_* constants.
    u64 thunk_region_lo{0};
    u64 thunk_region_hi{0};

    // Live full-address compiled-code envelope. Another JIT thread may retain
    // a page while this one remains in a linked native chain, so emitted
    // stores acquire-load the GuestMemory-owned atomics rather than copies
    // refreshed only on dispatcher entry.
    const std::atomic<GuestAddr>* code_span64_lo{nullptr};
    const std::atomic<GuestAddr>* code_span64_hi{nullptr};

    // Live GuestMemory protection generation and the last execute-entry
    // snapshot whose slot invalidations this compiler drained. A mismatch
    // makes linear64 accesses use permission-aware helpers until re-entry.
    const std::atomic<u64>* protect_sequence_live{nullptr};
    u64 protect_sequence_seen{0};
    // Cold linear stores may reuse a region slot only while its code/watch
    // exclusion proofs remain current, including changes on other threads.
    const std::atomic<u64>* code_generation_live{nullptr};
    const std::atomic<int>* watch_generation_live{nullptr};

    // GuestMemory::linear_check_page_map(): one byte per 16 KiB host page of
    // the reservation, non-zero when a direct access to that page must instead
    // go through the checked helper. The reasons are separate bits with
    // separate publishers (kLinearCheckMixedProt, kLinearCheckHoldsCode) but
    // the emitted fast path asks the byte, not the bit, so one LDRB answers
    // every one of them. Null when the reservation has no map, which the
    // emitters read as "check everything".
    const u8* linear_check_map{nullptr};

    /// Host SP of this dispatcher entry's 16-byte LR frame: the value SP has
    /// right after a block prologue's STR X30, [SP, #-16]!. Native guest-CALL
    /// continuation frames (kNativeCallFrameSize each) are pushed below it, so
    /// every exit to the dispatcher -- epilogue, raise exit, fault trampoline
    /// -- discards them all by restoring SP to this value before popping the
    /// LR frame. Written by execute() for the entry it makes and restored on
    /// return, so a nested execute() never disturbs the outer chain's frames.
    u64 native_sp_base{0};

    /// Host return address for a shared linear64 cold-mem helper. Written on
    /// BL entry so the helper BLR may clobber X30. SP is not moved: the Mach
    /// fault trampoline still pops the one prologue frame. Memory helpers do
    /// not re-enter compiled code, so this slot is not nested.
    u64 cold_mem_lr{0};

    /// Process-wide stub slots: linear64_cold_mem, raise_exit,
    /// linear64_hot_mem, continue_exit, linear64_hot_mem_wide and
    /// capture_cf_stub. Compiled code loads them through the RUNNING
    /// thread's context, and that code may have been compiled by another
    /// thread (shared adoption, direct links patched into shared code), so
    /// every live context holds every published stub: JitCompiler registers
    /// its context for its lifetime and each publisher fills all registered
    /// contexts before the code that needs the stub can run
    /// (register_live_jit_context, jit_memory_emit.cpp).
    ///
    /// Process-wide linear64 try_region+helper bodies, indexed
    /// `(store ? 4 : 0) + width_log2` for widths 1/2/4/8. Sites LDR X16
    /// from here and BLR instead of baking a 4-word host address.
    void* linear64_cold_mem[8]{};

    /// Shared raise-exit tail. Sites store pins first, then LDR+BR here for
    /// persist / loop-counter / dispatcher return. Layout-agnostic.
    void* raise_exit{nullptr};

    /// Unused: held the scalar linear64 hot guards of the opt-in
    /// VITREO_JIT_LINEAR64_HOT_HELPER mode, removed 2026-10-11. Kept, never
    /// written, so every later field keeps the JitContext offset that emitted
    /// code bakes in.
    void* linear64_hot_mem[8]{};

    /// Shared Continue epilogue tail (pop dispatcher LR, MOVZ W0, RET).
    /// Sites store pins and clear nzcv kind first.
    void* continue_exit{nullptr};

    /// Process-wide 16- and 32-byte linear64 hot guards. Appended so the
    /// scalar 1/2/4/8 slots keep their emitted JitContext offsets.
    /// Indexed `(width==32 ? 2 : 0) + (is_store ? 1 : 0)`.
    void* linear64_hot_mem_wide[4]{};

    /// NT_TIB.StackLimit probe for CALL's return-address push. Sites LDR+BLR
    /// instead of baking a 4-word host address.
    void (*require_stack_limit)(CpuState*, GuestMemory*, u64){nullptr};

    /// Host-stack storage for this execute() invocation. Nested entry replaces
    /// and restores the pointer; no generated native SP offset changes.
    ir::SpillFrame* ir_spill_frame{nullptr};

    /// Native LOCK read-modify-write (compile_locked): this thread's
    /// LockPrefixReaderSlot::active and the process-wide count of non-atomic
    /// fallbacks in flight (core/cpu/lock_prefix.hpp). Emitted code joins the
    /// reader side of the same protocol LockPrefixGuard runs, so a split-lock
    /// fallback on another thread still quiesces it. Set by execute() on the
    /// thread that runs the code (JitCompiler is thread-local).
    std::atomic<u32>* lock_reader_active{nullptr};
    const std::atomic<u32>* lock_prefix_writers{nullptr};

    /// Borrowed from core/math on the thread entering execute(). Null disables
    /// the optional SSE environment guard without changing compiled code.
    const u64* installed_sse_environment{nullptr};

    /// GuestMemory::linear_subpage_map(): per mixed host page, which 4 KiB
    /// guest pages are readable (bits 0-3) and writable (bits 4-7). Read by
    /// linear64 cold memory stubs. Null disables that path.
    const u8* linear_subpage_map{nullptr};

    /// Lowest host SP at which a native guest-CALL landing pad may still push
    /// its continuation frame. Below it the pad enters the callee without a
    /// frame and the matching RET takes the ordinary L1 tail. 1 << 63 never
    /// admits a frame (fail closed). Set by execute() with native_sp_base.
    u64 native_sp_floor{u64{1} << 63};

    /// Host PC inside the block whose raise exit last ran, or 0. A block that
    /// elided lazy-flag records under a recovery plan publishes it on the way
    /// out of a SOFTWARE raise (a helper refused an access, an alignment or
    /// #XM check, a delegated instruction raised), so execute() can rebuild
    /// the elided record from that block's per-instruction descriptors. A
    /// hardware fault rebuilds it in the signal handler instead and leaves
    /// this 0. execute() consumes and clears it.
    u64 raise_block_pc{0};

    /// Process-wide carry capture (JitCompiler::materialize_capture_cf_stub):
    /// W21 = x86 CF read out of CpuState's lazy record, for a carry consumer
    /// compiled while that record's kind was unknown. Sites LDR+BLR here
    /// instead of inlining the kind walk. Clobbers X0-X4, X16, X17, X30 and
    /// NZCV; preserves the pins and X14/X15.
    void* capture_cf_stub{nullptr};

    /// GuestMemory::linear_jit_view_base(): the alias of guest address 0 in
    /// the protection-ENFORCING view of the reservation, or null. Under
    /// JitConfig::linear64_jit_view an x64 block keeps it in X15 (in place of
    /// the check map, which its remaining probes then reload from
    /// linear_check_map) and reaches guest memory as X15 + EA.
    const u8* jit_view_base{nullptr};

    /// jit_mem_write_span: a 16- or 32-byte store written as one validated
    /// span, so a store that is denied anywhere performs none of itself.
    /// Used by the vec128/vec256 cold stubs.
    void (*write_span)(JitContext*, GuestAddr, const u8*, const u8*, u64){nullptr};

    // ---- L3: in-chain API thunk calls (jit_inchain_thunk.cpp) -------------
    /// Host trampoline an API-call tail's in-chain stub BLRs to
    /// (vitreo_jit_inchain_trampoline, in the vitreo binary's text, never in
    /// the arena). Set by the constructor on hosts with a native backend.
    void* inchain_trampoline{nullptr};
    /// winapi::ThunkRegistry::in_chain_slot_map(): {u64 count, u8 eligible
    /// [count]} indexed by (rip - thunk_region_lo) >> 4. The stub calls out
    /// only for a slot marked 1, so every other thunk (a kernel32 export,
    /// the CFG dispatch) leaves through the plain continue exit at the cost
    /// of one byte load. Never null.
    const u8* inchain_slot_map{
        reinterpret_cast<const u8*>(&kInChainEmptySlotMap)};
    /// The runtime serve hook and its environment, published by the chain
    /// loop that is running this compiler (JitCompiler::InChainScope); null
    /// outside one, and the stub then leaves through the dispatcher.
    InChainThunkHook inchain_hook{nullptr};
    void* inchain_env{nullptr};
    /// The compiler that owns this context (its arena and cache state).
    JitCompiler* inchain_owner{nullptr};
    /// InChainBreak set by an in-chain crossing that left execute(); the
    /// chain loop consumes it (JitCompiler::take_inchain_break).
    u32 inchain_break{0};
    /// In-chain crossings that resumed in translated code, and those that
    /// ran but left through the dispatcher. Plain per-thread counters.
    u64 inchain_resumed{0};
    u64 inchain_left{0};

    /// Bytes of the hardware-protected linear64 reservation, or 0. The shared
    /// cold memory helpers decide a mixed-page access from linear_subpage_map
    /// only for an EA below it; 0 sends every access to the checked helper.
    u64 linear64_window_bytes{0};

    /// Process-wide linked-tail probes, indexed by JitCompiler::LinkProbe.
    /// Entered by BR with X0 = guest target and X30 = the site's miss
    /// continuation.
    void* link_probe[4]{};
    /// Process-wide software-RSB push: BLR with X2 = guest return RIP.
    /// Clobbers X2-X4, X16, X17, X30 and NZCV.
    void* rsb_push{nullptr};
    /// Process-wide dispatcher tails, indexed by JitCompiler::DispatchTail.
    void* dispatch_tail[8]{};
    /// Host entry of the lazy cold memory stubs (jit_cold_lazy.cpp,
    /// vitreo_jit_cold_lazy_trampoline). A block's lazy entry LDRs it and
    /// BRs with X30 pointing at the taking site's record.
    void* cold_lazy_entry{nullptr};
    /// Guest RIP of an instruction whose lazy slow path could not be
    /// materialized: the take left through the block's raise exit with
    /// pending_raise inactive, and execute() interprets that one instruction
    /// (JitCompiler::finish_cold_side_exit). Zero otherwise.
    u64 cold_side_exit_rip{0};
    bool cold_side_exit{false};
    /// Header of the cold-entry counter run whose piece just crossed the
    /// residency threshold (JitCompiler::kColdCounter tail stores it); the
    /// dispatcher consumes and clears it. Zero when nothing is pending.
    u64 cold_residency_request{0};
    /// The counter tail's X14, X16 and X17 across its body: a counted
    /// piece's entry may hold live values in all three.
    u64 cold_counter_save[3]{};
    /// Base of the process-wide tier-0 entry down-counters (jit_ir_tier.cpp);
    /// a bound tier site decrements its slot's counter through it inline.
    u32* tier_counters{nullptr};
    /// Indexes of the tier slots that crossed the threshold since the
    /// dispatcher last ran (JitCompiler::kTierCounter tail appends and parks
    /// the slot's counter; a site that finds the ring full re-arms and asks
    /// again later). A nonzero count makes the dispatcher service them.
    u64 tier_requests[1024]{};
    u32 tier_request_count{0};
};
static_assert(offsetof(JitContext, atomic_xchg32) == 616);

constexpr u32 JIT_CTX_OFF_IR_SPILL_FRAME =
    static_cast<u32>(offsetof(JitContext, ir_spill_frame));
static_assert(JIT_CTX_OFF_IR_SPILL_FRAME % 8 == 0);
static_assert(JIT_CTX_OFF_IR_SPILL_FRAME / 8 < 4096);

constexpr u32 JIT_CTX_OFF_X64_RSLOTS =
    static_cast<u32>(offsetof(JitContext, x64_rslots));
constexpr u32 JIT_CTX_OFF_X64_WSLOTS =
    static_cast<u32>(offsetof(JitContext, x64_wslots));
constexpr u32 JIT_CTX_OFF_REP_STOSQ =
    static_cast<u32>(offsetof(JitContext, rep_stosq));
constexpr u32 JIT_CTX_OFF_REP_MOVSQ =
    static_cast<u32>(offsetof(JitContext, rep_movsq));
constexpr u32 JIT_CTX_OFF_REPE_CMPSQ =
    static_cast<u32>(offsetof(JitContext, repe_cmpsq));
constexpr u32 JIT_CTX_OFF_REPNE_CMPSQ =
    static_cast<u32>(offsetof(JitContext, repne_cmpsq));
constexpr u32 JIT_CTX_OFF_REPE_SCASQ =
    static_cast<u32>(offsetof(JitContext, repe_scasq));
constexpr u32 JIT_CTX_OFF_REPNE_SCASQ =
    static_cast<u32>(offsetof(JitContext, repne_scasq));
constexpr u32 JIT_CTX_OFF_REP_LODSB =
    static_cast<u32>(offsetof(JitContext, rep_lodsb));
constexpr u32 JIT_CTX_OFF_REP_LODSW =
    static_cast<u32>(offsetof(JitContext, rep_lodsw));
constexpr u32 JIT_CTX_OFF_REP_LODSD =
    static_cast<u32>(offsetof(JitContext, rep_lodsd));
constexpr u32 JIT_CTX_OFF_CODE_PAGE_MAP =
    static_cast<u32>(offsetof(JitContext, code_page_map));
constexpr u32 JIT_CTX_OFF_CODE_CHUNK_PAGES =
    static_cast<u32>(offsetof(JitContext, code_chunk_pages));
constexpr u32 JIT_CTX_OFF_CODE_SPAN64_LO =
    static_cast<u32>(offsetof(JitContext, code_span64_lo));
constexpr u32 JIT_CTX_OFF_CODE_SPAN64_HI =
    static_cast<u32>(offsetof(JitContext, code_span64_hi));
constexpr u32 JIT_CTX_OFF_PROTECT_SEQUENCE_LIVE =
    static_cast<u32>(offsetof(JitContext, protect_sequence_live));
constexpr u32 JIT_CTX_OFF_PROTECT_SEQUENCE_SEEN =
    static_cast<u32>(offsetof(JitContext, protect_sequence_seen));
constexpr u32 JIT_CTX_OFF_CODE_GENERATION_LIVE =
    static_cast<u32>(offsetof(JitContext, code_generation_live));
constexpr u32 JIT_CTX_OFF_WATCH_GENERATION_LIVE =
    static_cast<u32>(offsetof(JitContext, watch_generation_live));
constexpr u32 JIT_CTX_OFF_WREGION_CODE_GEN =
    static_cast<u32>(offsetof(JitContext, wregion_code_gen));
constexpr u32 JIT_CTX_OFF_WREGION_WATCH_GEN =
    static_cast<u32>(offsetof(JitContext, wregion_watch_gen));
constexpr u32 JIT_CTX_OFF_NATIVE_SP_BASE =
    static_cast<u32>(offsetof(JitContext, native_sp_base));
constexpr u32 JIT_CTX_OFF_NATIVE_SP_FLOOR =
    static_cast<u32>(offsetof(JitContext, native_sp_floor));
constexpr u32 JIT_CTX_OFF_COLD_MEM_LR =
    static_cast<u32>(offsetof(JitContext, cold_mem_lr));
constexpr u32 JIT_CTX_OFF_RAISE_BLOCK_PC =
    static_cast<u32>(offsetof(JitContext, raise_block_pc));
constexpr u32 JIT_CTX_OFF_CAPTURE_CF_STUB =
    static_cast<u32>(offsetof(JitContext, capture_cf_stub));
static_assert(JIT_CTX_OFF_CAPTURE_CF_STUB % 8 == 0);
static_assert(JIT_CTX_OFF_CAPTURE_CF_STUB / 8 <= 4095,
              "emitted LDR (unsigned offset) cannot reach capture_cf_stub");
constexpr u32 JIT_CTX_OFF_LINEAR64_COLD_MEM =
    static_cast<u32>(offsetof(JitContext, linear64_cold_mem));
constexpr u32 JIT_CTX_OFF_RAISE_EXIT =
    static_cast<u32>(offsetof(JitContext, raise_exit));
constexpr u32 JIT_CTX_OFF_LINEAR64_HOT_MEM =
    static_cast<u32>(offsetof(JitContext, linear64_hot_mem));
constexpr u32 JIT_CTX_OFF_CONTINUE_EXIT =
    static_cast<u32>(offsetof(JitContext, continue_exit));
constexpr u32 JIT_CTX_OFF_LINEAR64_HOT_MEM_WIDE =
    static_cast<u32>(offsetof(JitContext, linear64_hot_mem_wide));
constexpr u32 JIT_CTX_OFF_LINK_PROBE =
    static_cast<u32>(offsetof(JitContext, link_probe));
constexpr u32 JIT_CTX_OFF_RSB_PUSH =
    static_cast<u32>(offsetof(JitContext, rsb_push));
static_assert(JIT_CTX_OFF_LINK_PROBE % 8 == 0 &&
                  (JIT_CTX_OFF_LINK_PROBE + 3 * 8) / 8 <= 4095,
              "emitted LDR (unsigned offset) cannot reach link_probe");
static_assert(JIT_CTX_OFF_RSB_PUSH % 8 == 0 &&
                  JIT_CTX_OFF_RSB_PUSH / 8 <= 4095,
              "emitted LDR (unsigned offset) cannot reach rsb_push");
constexpr u32 JIT_CTX_OFF_DISPATCH_TAIL =
    static_cast<u32>(offsetof(JitContext, dispatch_tail));
static_assert(JIT_CTX_OFF_DISPATCH_TAIL % 8 == 0 &&
                  (JIT_CTX_OFF_DISPATCH_TAIL + 7 * 8) / 8 <= 4095,
              "emitted LDR (unsigned offset) cannot reach dispatch_tail");
constexpr u32 JIT_CTX_OFF_COLD_LAZY_ENTRY =
    static_cast<u32>(offsetof(JitContext, cold_lazy_entry));
static_assert(JIT_CTX_OFF_COLD_LAZY_ENTRY % 8 == 0 &&
                  JIT_CTX_OFF_COLD_LAZY_ENTRY / 8 <= 4095,
              "emitted LDR (unsigned offset) cannot reach cold_lazy_entry");
constexpr u32 JIT_CTX_OFF_COLD_RESIDENCY_REQUEST =
    static_cast<u32>(offsetof(JitContext, cold_residency_request));
static_assert(JIT_CTX_OFF_COLD_RESIDENCY_REQUEST % 8 == 0 &&
                  JIT_CTX_OFF_COLD_RESIDENCY_REQUEST / 8 <= 4095,
              "emitted STR (unsigned offset) cannot reach cold_residency_request");
constexpr u32 JIT_CTX_OFF_COLD_COUNTER_SAVE =
    static_cast<u32>(offsetof(JitContext, cold_counter_save));
static_assert(JIT_CTX_OFF_COLD_COUNTER_SAVE % 8 == 0 &&
                  JIT_CTX_OFF_COLD_COUNTER_SAVE / 8 + 2 <= 4095,
              "emitted STR (unsigned offset) cannot reach cold_counter_save");
constexpr u32 JIT_CTX_OFF_TIER_COUNTERS =
    static_cast<u32>(offsetof(JitContext, tier_counters));
static_assert(JIT_CTX_OFF_TIER_COUNTERS % 8 == 0 &&
                  JIT_CTX_OFF_TIER_COUNTERS / 8 <= 4095,
              "emitted LDR (unsigned offset) cannot reach tier_counters");
constexpr u32 JIT_CTX_TIER_REQUEST_RING =
    static_cast<u32>(sizeof(JitContext::tier_requests) / sizeof(u64));
static_assert((JIT_CTX_TIER_REQUEST_RING & (JIT_CTX_TIER_REQUEST_RING - 1)) == 0,
              "the tier tail tests one bit of the count for full");
constexpr u32 JIT_CTX_OFF_TIER_REQUESTS =
    static_cast<u32>(offsetof(JitContext, tier_requests));
static_assert(JIT_CTX_OFF_TIER_REQUESTS % 8 == 0 &&
                  JIT_CTX_OFF_TIER_REQUESTS / 8 <= 4095,
              "emitted STR (unsigned offset) cannot reach tier_requests");
constexpr u32 JIT_CTX_OFF_TIER_REQUEST_COUNT =
    static_cast<u32>(offsetof(JitContext, tier_request_count));
static_assert(JIT_CTX_OFF_TIER_REQUEST_COUNT % 4 == 0 &&
                  JIT_CTX_OFF_TIER_REQUEST_COUNT / 4 <= 4095,
              "emitted LDR/STR (unsigned offset) cannot reach tier_request_count");
constexpr u32 JIT_CTX_OFF_REQUIRE_STACK_LIMIT =
    static_cast<u32>(offsetof(JitContext, require_stack_limit));
/// Native guest-CALL continuation frame, pushed by a block's call landing pad
/// below the dispatcher LR frame:
///   [SP+0]  host continuation (the BL's X30)
///   [SP+8]  reserved: every block may spill NZCV to [SP,#8]
///   [SP+16] guest return RIP the CALL pushed
///   [SP+24] tag: the owning JitContext* (X20)
/// A RET returns natively only when the popped guest RIP and the tag both
/// match. execute() places a {0, 0} pair directly above the dispatcher LR
/// frame, so the bottom of every chain can never match: no JitContext is at
/// address zero.
constexpr u32 kNativeCallFrameSize = 32;
constexpr u32 kNativeCallNzcvOffset = 8;
constexpr u32 kNativeCallGuestRipOffset = 16;
constexpr u32 kNativeCallTagOffset = 24;
constexpr usize kNativeCallStackReserve = 64u * 1024u;
constexpr u32 kNativeCallMaxFrames = 256;
/// native_sp_floor value that admits no frame.
constexpr u64 kNativeCallNoFrames = u64{1} << 63;

/// Lowest SP at which a landing pad may push one more continuation frame,
/// for a downward-growing pthread stack and a chain whose dispatcher frame
/// lies at or below `entry_sp`. At most kNativeCallMaxFrames frames, and the
/// bottom kNativeCallStackReserve bytes stay free for host signal delivery,
/// helpers and dispatcher fallback. Invalid bounds/SP fail closed.
[[nodiscard]] constexpr u64 native_sp_floor_for_stack(
    uintptr_t entry_sp, uintptr_t stack_low, uintptr_t stack_high) noexcept {
    if (stack_high <= stack_low || entry_sp < stack_low ||
        entry_sp > stack_high ||
        stack_high - stack_low <= kNativeCallStackReserve) {
        return kNativeCallNoFrames;
    }
    const uintptr_t safe_low = stack_low + kNativeCallStackReserve;
    constexpr uintptr_t span =
        uintptr_t{kNativeCallMaxFrames} * kNativeCallFrameSize;
    const uintptr_t cap_low = entry_sp > span ? entry_sp - span : 0;
    // The pad pushes when SP >= floor, landing the frame at SP - 32.
    const uintptr_t lowest_frame = cap_low > safe_low ? cap_low : safe_low;
    if (lowest_frame >= entry_sp) return kNativeCallNoFrames;
    return static_cast<u64>(lowest_frame + kNativeCallFrameSize);
}
static_assert(kNativeCallFrameSize % 16 == 0,
              "Apple AArch64 ABI requires 16-byte-aligned SP");
static_assert(kNativeCallNzcvOffset + sizeof(u64) <=
              kNativeCallGuestRipOffset);
static_assert(kNativeCallGuestRipOffset + sizeof(u64) == kNativeCallTagOffset,
              "RET loads the guest RIP and the tag with one LDP");
static_assert(kNativeCallTagOffset + sizeof(u64) <= kNativeCallFrameSize);
static_assert((JIT_CTX_OFF_NATIVE_SP_BASE & 7) == 0 &&
              (JIT_CTX_OFF_NATIVE_SP_BASE >> 3) < 4096);
static_assert((JIT_CTX_OFF_NATIVE_SP_FLOOR & 7) == 0 &&
              (JIT_CTX_OFF_NATIVE_SP_FLOOR >> 3) < 4096,
              "emitted LDR (unsigned offset) cannot reach native_sp_floor");
static_assert((JIT_CTX_OFF_RAISE_BLOCK_PC & 7) == 0 &&
              (JIT_CTX_OFF_RAISE_BLOCK_PC >> 3) < 4096,
              "emitted STR (unsigned offset) cannot reach raise_block_pc");
static_assert(JIT_CTX_OFF_X64_RSLOTS % 8 == 0);
static_assert(JIT_CTX_OFF_X64_RSLOTS / 8 <= 4095,
              "emitted LDR (unsigned offset) cannot reach the slot table");
static_assert(JIT_CTX_OFF_X64_WSLOTS / 8 <= 4095,
              "emitted LDR (unsigned offset) cannot reach the slot table");
static_assert(JIT_CTX_OFF_REP_STOSQ % 8 == 0);
static_assert(JIT_CTX_OFF_REP_STOSQ / 8 <= 4095,
              "emitted LDR (unsigned offset) cannot reach rep_stosq");
static_assert(JIT_CTX_OFF_REP_MOVSQ % 8 == 0);
static_assert(JIT_CTX_OFF_REP_MOVSQ / 8 <= 4095,
              "emitted LDR (unsigned offset) cannot reach rep_movsq");
static_assert(JIT_CTX_OFF_REPE_CMPSQ / 8 <= 4095);
static_assert(JIT_CTX_OFF_REPNE_CMPSQ / 8 <= 4095);
static_assert(JIT_CTX_OFF_REPE_SCASQ / 8 <= 4095);
static_assert(JIT_CTX_OFF_REPNE_SCASQ / 8 <= 4095);
static_assert(JIT_CTX_OFF_REP_LODSB % 8 == 0);
static_assert(JIT_CTX_OFF_REP_LODSB / 8 <= 4095);
static_assert(JIT_CTX_OFF_REP_LODSW / 8 <= 4095);
static_assert(JIT_CTX_OFF_REP_LODSD / 8 <= 4095);
static_assert(JIT_CTX_OFF_CODE_PAGE_MAP % 8 == 0);
static_assert(JIT_CTX_OFF_COLD_MEM_LR % 8 == 0);
static_assert(JIT_CTX_OFF_COLD_MEM_LR / 8 <= 4095,
              "emitted LDR/STR (unsigned offset) cannot reach cold_mem_lr");
static_assert(JIT_CTX_OFF_LINEAR64_COLD_MEM % 8 == 0);
static_assert((JIT_CTX_OFF_LINEAR64_COLD_MEM + 7 * 8) / 8 <= 4095,
              "emitted LDR (unsigned offset) cannot reach linear64_cold_mem");
static_assert(JIT_CTX_OFF_RAISE_EXIT % 8 == 0);
static_assert(JIT_CTX_OFF_RAISE_EXIT / 8 <= 4095,
              "emitted LDR (unsigned offset) cannot reach raise_exit");
static_assert(JIT_CTX_OFF_LINEAR64_HOT_MEM % 8 == 0);
static_assert((JIT_CTX_OFF_LINEAR64_HOT_MEM + 7 * 8) / 8 <= 4095,
              "emitted LDR (unsigned offset) cannot reach linear64_hot_mem");
static_assert(JIT_CTX_OFF_CONTINUE_EXIT % 8 == 0);
static_assert(JIT_CTX_OFF_CONTINUE_EXIT / 8 <= 4095,
              "emitted LDR (unsigned offset) cannot reach continue_exit");
static_assert(JIT_CTX_OFF_LINEAR64_HOT_MEM_WIDE % 8 == 0);
static_assert((JIT_CTX_OFF_LINEAR64_HOT_MEM_WIDE + 3 * 8) / 8 <= 4095,
              "emitted LDR (unsigned offset) cannot reach linear64_hot_mem_wide");
static_assert(JIT_CTX_OFF_REQUIRE_STACK_LIMIT % 8 == 0);
static_assert(JIT_CTX_OFF_REQUIRE_STACK_LIMIT / 8 <= 4095,
              "emitted LDR (unsigned offset) cannot reach require_stack_limit");
static_assert(JIT_CTX_OFF_CODE_PAGE_MAP / 8 <= 4095,
              "emitted LDR (unsigned offset) cannot reach code_page_map");
static_assert(JIT_CTX_OFF_CODE_CHUNK_PAGES % 8 == 0);
static_assert(JIT_CTX_OFF_CODE_CHUNK_PAGES / 8 <= 4095,
              "emitted LDR (unsigned offset) cannot reach code_chunk_pages");
static_assert(JIT_CTX_OFF_CODE_SPAN64_LO % 8 == 0);
static_assert(JIT_CTX_OFF_CODE_SPAN64_HI % 8 == 0);
static_assert(JIT_CTX_OFF_CODE_SPAN64_HI / 8 <= 4095,
              "emitted LDR (unsigned offset) cannot reach x64 code span");
constexpr u32 JIT_CTX_OFF_LINEAR_CHECK_MAP =
    static_cast<u32>(offsetof(JitContext, linear_check_map));
constexpr u32 JIT_CTX_OFF_LINEAR_SUBPAGE_MAP =
    static_cast<u32>(offsetof(JitContext, linear_subpage_map));
static_assert(JIT_CTX_OFF_LINEAR_SUBPAGE_MAP % 8 == 0);
static_assert(JIT_CTX_OFF_LINEAR_SUBPAGE_MAP / 8 <= 4095,
              "emitted LDR (unsigned offset) cannot reach linear_subpage_map");
constexpr u32 JIT_CTX_OFF_LINEAR64_WINDOW_BYTES =
    static_cast<u32>(offsetof(JitContext, linear64_window_bytes));
static_assert(JIT_CTX_OFF_LINEAR64_WINDOW_BYTES % 8 == 0);
static_assert(JIT_CTX_OFF_LINEAR64_WINDOW_BYTES / 8 <= 4095,
              "emitted LDR (unsigned offset) cannot reach linear64_window_bytes");
constexpr u32 JIT_CTX_OFF_LOCK_READER_ACTIVE =
    static_cast<u32>(offsetof(JitContext, lock_reader_active));
constexpr u32 JIT_CTX_OFF_LOCK_PREFIX_WRITERS =
    static_cast<u32>(offsetof(JitContext, lock_prefix_writers));
static_assert(JIT_CTX_OFF_LOCK_READER_ACTIVE % 8 == 0);
static_assert(JIT_CTX_OFF_LOCK_READER_ACTIVE / 8 <= 4095,
              "emitted LDR (unsigned offset) cannot reach lock_reader_active");
static_assert(JIT_CTX_OFF_LOCK_PREFIX_WRITERS % 8 == 0);
static_assert(JIT_CTX_OFF_LOCK_PREFIX_WRITERS / 8 <= 4095,
              "emitted LDR (unsigned offset) cannot reach lock_prefix_writers");
static_assert(JIT_CTX_OFF_PROTECT_SEQUENCE_LIVE / 8 <= 4095);
static_assert(JIT_CTX_OFF_PROTECT_SEQUENCE_SEEN / 8 <= 4095);
static_assert(JIT_CTX_OFF_LINEAR_CHECK_MAP % 8 == 0);
static_assert(JIT_CTX_OFF_LINEAR_CHECK_MAP / 8 <= 4095,
              "emitted LDR (unsigned offset) cannot reach linear_check_map");
constexpr u32 JIT_CTX_OFF_JIT_VIEW_BASE =
    static_cast<u32>(offsetof(JitContext, jit_view_base));
constexpr u32 JIT_CTX_OFF_WRITE_SPAN =
    static_cast<u32>(offsetof(JitContext, write_span));
static_assert(JIT_CTX_OFF_WRITE_SPAN % 8 == 0);
static_assert(JIT_CTX_OFF_WRITE_SPAN / 8 <= 4095,
              "emitted LDR (unsigned offset) cannot reach write_span");
static_assert(JIT_CTX_OFF_JIT_VIEW_BASE % 8 == 0);
static_assert(JIT_CTX_OFF_JIT_VIEW_BASE / 8 <= 4095,
              "emitted LDR (unsigned offset) cannot reach jit_view_base");

} // namespace vitreo::cpu::jit
