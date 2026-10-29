// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
//
// Per-block precise-RIP and flag-recovery metadata of the
// recoverable-fault handler (linear_fault.cpp): publication by the
// compilers, the lock-free lookups the handler and the sampled
// perf-window helper make, the elided flag-record rebuild, and
// retirement on arena reclaim.
//
// Split out of linear_fault.cpp as pure code motion: every line below the
// includes is verbatim from that file.

#include <type_traits>
#include <new>
#include "linear_fault.hpp"
#include "jit_compiler.hpp"
#include "jit_helpers.hpp"
#include "stencils.hpp"

#include "core/cpu/state/cpu_state.hpp"
#include "core/memory/write_watch_traps.hpp"
#include "core/memory/guard_page_traps.hpp"

#include <atomic>
#include <csignal>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <limits>
#include <mutex>
#include <new>
#include <pthread.h>
#include <sys/mman.h>
#include <libkern/OSCacheControl.h>
#include <unistd.h>
#include <mach/mach_vm.h>
#include <mach/mach.h>
#include <mach/vm_param.h>

namespace vitreo::cpu::jit {

namespace {

// Per-block precise-RIP metadata. Compilers only prepend immutable nodes; the
// signal handler traverses them with an acquire load and performs no locking or
// allocation. Nodes are unlinked and freed only after the arena admission gate
// is closed and every execution/compile lease has drained.
//
// Storage is ONE allocation per block: the header, then `site_count`
// interval records, then `flag_count` flag-recovery entries for only the
// sites that elided a flag store. The public JitRipSite carries a 24-byte
// JitFlagRecovery on every site, active on few; copying it verbatim cost
// A browser helper's largest process held 176 MB of host heap at 363 B per block across
// 484k blocks (progress doc 4.3), the largest single per-block item.
//
// The records themselves are compact too, because this table is held for
// every translation in the arena for the process lifetime:
//  - a site is 8 bytes (16-bit host offsets, a 32-bit guest RIP delta from
//    the block's first site) whenever the block fits -- every real block
//    does; the 16-byte wide form (`JitRipBlock::wide`) is kept for a block
//    longer than 64 KiB of native code or one whose RIPs span 4 GiB;
//  - a flag entry is an 8-byte head plus one u64 per immediate it actually
//    carries (`RipFlagHead::imm_mask`), instead of a 32-byte entry holding
//    two u64 immediates whether or not the operand was an immediate.
// MEASURED in tests/cpu/test_jit_memory_footprint.cpp on a 10-instruction
// block with 3 elided flag stores: 320 -> 176 bytes.
struct CompactRipSite {
    u32 host_begin;
    u32 host_end;
    GuestAddr guest_rip;
};
struct NarrowRipSite {
    u16 host_begin;
    u16 host_end;
    u32 rip_delta;          ///< guest_rip - JitRipBlock::base_rip
};
struct RipFlagHead {
    u16 site;               ///< index into the site array
    u8 flag_op;
    u8 src_reg;
    u8 dst_reg;
    u8 width;
    u8 mode;
    /// bit 0: a u64 src_imm follows; bit 1: dst_imm. Bits 7:2: the
    /// descriptor's rsp_undo as a 6-bit two's-complement value.
    u8 imm_mask;
};
static_assert(sizeof(RipFlagHead) == 8 && sizeof(NarrowRipSite) == 8,
              "compact RIP records are 8-byte units");
struct JitRipBlock {
    uintptr_t lo{0};
    JitRipBlock* next{nullptr};
    GuestAddr base_rip{0};      ///< narrow sites add their delta to this
    u32 code_size{0};
    u16 site_count{0};
    u16 flag_count{0};
    JitPinMap pins{kNoJitPins};
    /// Split placement: the second, cold range sits at lo + cold_offset.
    /// Both fit 16 bits because a split block lies inside one 32 KiB window.
    u16 cold_offset{0};
    u16 cold_size{0};           ///< 0: the block is one range
    bool wide{false};           ///< sites are CompactRipSite, not Narrow
    u8 pad_[3]{};

    /// End of everything the block owns (its cold range, if any).
    [[nodiscard]] uintptr_t hi() const noexcept {
        return cold_size != 0 ? lo + cold_offset + cold_size : lo + code_size;
    }
    [[nodiscard]] bool contains(uintptr_t pc) const noexcept {
        if (pc < lo) return false;
        const uintptr_t off = pc - lo;
        return off < code_size ||
               (off >= cold_offset && off < static_cast<uintptr_t>(cold_offset) + cold_size);
    }
    [[nodiscard]] const u8* payload() const noexcept {
        return reinterpret_cast<const u8*>(this) + sizeof(JitRipBlock);
    }
    [[nodiscard]] u8* payload() noexcept {
        return reinterpret_cast<u8*>(this) + sizeof(JitRipBlock);
    }
    [[nodiscard]] usize site_bytes() const noexcept {
        return static_cast<usize>(site_count) *
               (wide ? sizeof(CompactRipSite) : sizeof(NarrowRipSite));
    }
    [[nodiscard]] const u8* flag_entries() const noexcept {
        return payload() + site_bytes();
    }
    [[nodiscard]] u8* flag_entries() noexcept { return payload() + site_bytes(); }

    /// Host interval and guest RIP of site @p i in either layout.
    void site(u32 i, u32& host_begin, u32& host_end,
              GuestAddr& guest_rip) const noexcept {
        if (wide) {
            const auto* s = reinterpret_cast<const CompactRipSite*>(payload()) + i;
            host_begin = s->host_begin;
            host_end = s->host_end;
            guest_rip = s->guest_rip;
        } else {
            const auto* s = reinterpret_cast<const NarrowRipSite*>(payload()) + i;
            host_begin = s->host_begin;
            host_end = s->host_end;
            guest_rip = base_rip + s->rip_delta;
        }
    }
};
static_assert(sizeof(JitRipBlock) == 56, "RIP block header is 56 bytes");
static_assert(sizeof(JitRipBlock) % 8 == 0, "site array must start aligned");
static_assert(std::is_trivially_destructible_v<JitRipBlock>,
              "retire frees the raw allocation without a destructor");

/// Bytes a flag entry for @p flags occupies: the head plus each immediate it
/// carries. An immediate is carried when the operand IS an immediate or the
/// field is non-zero, so the reconstruction is bit-exact either way.
[[nodiscard]] inline u8 rip_flag_imm_mask(const JitFlagRecovery& flags) noexcept {
    u8 mask = 0;
    if (!flags.active()) return mask;  // an rsp_undo-only entry: no operands
    if (flags.src_reg == JitFlagRecovery::kImmediate || flags.src_imm != 0) mask |= 1u;
    if (flags.dst_reg == JitFlagRecovery::kImmediate || flags.dst_imm != 0) mask |= 2u;
    return mask;
}
[[nodiscard]] inline u8 rip_undo_bits(i8 rsp_undo) noexcept {
    return static_cast<u8>((static_cast<u8>(rsp_undo) & 0x3Fu) << 2);
}
[[nodiscard]] inline i8 rip_undo_from_head(u8 imm_mask) noexcept {
    // Sign-extend the 6-bit field in bits 7:2.
    return static_cast<i8>(static_cast<i8>(imm_mask & 0xFCu) >> 2);
}
[[nodiscard]] inline usize rip_flag_entry_bytes(u8 imm_mask) noexcept {
    return sizeof(RipFlagHead) +
           sizeof(u64) * (static_cast<usize>(imm_mask & 1u) +
                          static_cast<usize>((imm_mask >> 1) & 1u));
}
inline u64 load_u64_unaligned(const u8* p) noexcept {
    u64 v;
    std::memcpy(&v, p, sizeof(v));
    return v;
}
inline void store_u64_unaligned(u8* p, u64 v) noexcept {
    std::memcpy(p, &v, sizeof(v));
}
// Nodes are bucketed by the 64 KiB host granule their code STARTS in, and a
// lookup walks the PC's granule plus each earlier granule that a block of the
// largest published span could have started in. One flat list made every
// lookup O(live blocks): 1.6M blocks in a 64-bit title put a pointer chase of
// that length on every JIT fault and on every sampled perf-window helper
// access, which spent most of a 22 s measurement window inside the walk.
// Granules further apart than the table alias to one bucket. That is still
// correct, because a node matches only when its [lo, hi) contains the PC.
constexpr unsigned kRipGranuleShift = 16;
constexpr usize kRipBucketCount = usize{1} << 16;
constexpr usize kRipBucketMask = kRipBucketCount - 1;
std::atomic<JitRipBlock*> g_jit_rip_buckets[kRipBucketCount];
static_assert(std::atomic<JitRipBlock*>::is_always_lock_free,
              "signal-handler RIP-map pointer must be lock-free");
/// Most granules any published block extends past its start granule. It only
/// grows, and it is stored (release) before the node is linked.
std::atomic<uintptr_t> g_jit_rip_max_span{0};
std::mutex g_jit_rip_blocks_mutex;

[[nodiscard]] inline std::atomic<JitRipBlock*>& rip_bucket(
    uintptr_t granule) noexcept {
    return g_jit_rip_buckets[granule & kRipBucketMask];
}

} // namespace

/// Largest valid `FlagState::FlagOp` byte. `JitFlagRecovery` carries the enum
/// as a raw byte so the fault interface does not depend on CpuState's headers,
/// so this is the one place the two representations are tied together.
constexpr u8 kMaxFlagOpByte =
    static_cast<u8>(CpuState::FlagOp::Ror64_1);

bool publish_jit_rip_block(uintptr_t code, u32 code_size,
                           const JitRipSite* sites, u32 site_count,
                           const JitPinMap& pins, u32 cold_offset,
                           u32 cold_size) {
    const auto reject = [&](const char* reason) {
        std::fprintf(stderr,
            "[linear-fault] RIP metadata rejected: %s native=%llx bytes=%u sites=%u\n",
            reason, static_cast<unsigned long long>(code), code_size, site_count);
        return false;
    };
    if (code == 0 || code_size == 0 || sites == nullptr || site_count == 0) {
        return reject("empty block or sites");
    }
    if (cold_size != 0 &&
        (cold_offset < code_size ||
         static_cast<u64>(cold_offset) + cold_size >
             std::numeric_limits<u16>::max())) {
        return reject("cold range overlaps or exceeds 16 bits");
    }
    const auto in_block = [&](u32 begin, u32 end) {
        return end <= code_size ||
               (cold_size != 0 && begin >= cold_offset &&
                end <= cold_offset + cold_size);
    };
    u32 previous_end = 0;
    for (u32 i = 0; i < site_count; ++i) {
        const JitRipSite& site = sites[i];
        if (site.host_begin < previous_end || site.host_begin >= site.host_end ||
            !in_block(site.host_begin, site.host_end)) {
            return reject("overlapping or out-of-range instruction sites");
        }
        // A flag-recovery descriptor names host registers that the signal
        // handler will index in the interrupted register file, and a raw
        // `FlagOp` byte it will cast back to the enum. Reject a malformed one
        // HERE, where the caller still gets a false and falls back to keeping
        // the record stores, rather than in the handler, where the only
        // options left are a wrong reconstruction or a crash.
        if (site.flags.rsp_undo < -32 || site.flags.rsp_undo > 31) {
            return reject("rsp undo out of range");
        }
        if (site.flags.active()) {
            if (site.flags.flag_op > kMaxFlagOpByte) return reject("flag opcode");
            if (site.flags.width != 32 && site.flags.width != 64) return reject("flag width");
            if (site.flags.mode != JitFlagRecovery::DirectDst &&
                site.flags.mode != JitFlagRecovery::RecomputeDst &&
                site.flags.mode != JitFlagRecovery::MergeCf) {
                return reject("flag recovery mode");
            }
            if (site.flags.mode == JitFlagRecovery::MergeCf) {
                const auto op = static_cast<CpuState::FlagOp>(site.flags.flag_op);
                const bool wide = op == CpuState::FlagOp::Inc64 ||
                                  op == CpuState::FlagOp::Dec64;
                const bool narrow = op == CpuState::FlagOp::Inc32 ||
                                    op == CpuState::FlagOp::Dec32;
                if (!(wide || narrow) || site.flags.width != (wide ? 64 : 32) ||
                    site.flags.dst_reg == JitFlagRecovery::kImmediate) {
                    return reject("unsupported carry-merge recovery");
                }
            }
            // RecomputeDst asks the handler to redo the x86 operation. Only
            // the ten ops it implements may be published in that mode —
            // otherwise the handler would reach its default and silently
            // decline to reconstruct, leaving the guest whatever flags the
            // elided store did not write.
            if (site.flags.mode == JitFlagRecovery::RecomputeDst) {
                const auto op = static_cast<CpuState::FlagOp>(
                    site.flags.flag_op);
                if (op != CpuState::FlagOp::Add32 &&
                    op != CpuState::FlagOp::Sub32 &&
                    op != CpuState::FlagOp::And32 &&
                    op != CpuState::FlagOp::Or32 &&
                    op != CpuState::FlagOp::Xor32 &&
                    op != CpuState::FlagOp::Add64 &&
                    op != CpuState::FlagOp::Sub64 &&
                    op != CpuState::FlagOp::And64 &&
                    op != CpuState::FlagOp::Or64 &&
                    op != CpuState::FlagOp::Xor64) {
                    return reject("unsupported flag recomputation");
                }
            }
            if (site.flags.src_reg != JitFlagRecovery::kImmediate &&
                !JitFlagRecovery::is_cpu_state_gpr(site.flags.src_reg) &&
                site.flags.src_reg >= 29) {
                return reject("flag source register");
            }
            if (site.flags.dst_reg != JitFlagRecovery::kImmediate &&
                !JitFlagRecovery::is_cpu_state_gpr(site.flags.dst_reg) &&
                site.flags.dst_reg >= 29) {
                return reject("flag destination register");
            }
        }
        previous_end = site.host_end;
    }
    for (u8 guest = 0; guest < 16; ++guest) {
        const u8 host = pins.host_reg[guest];
        if (host != 0xFF && host >= 29) return reject("pin register");
        if (host == 0xFF) continue;
        for (u8 prior = 0; prior < guest; ++prior) {
            if (pins.host_reg[prior] == host) return reject("duplicate pin register");
        }
    }

    if (site_count > std::numeric_limits<u16>::max()) {
        return reject("too many sites");
    }
    // Narrow layout whenever every host offset fits 16 bits and every guest
    // RIP is within 4 GiB above the first site's.
    const GuestAddr base_rip = sites[0].guest_rip;
    bool wide = code_size > std::numeric_limits<u16>::max();
    for (u32 i = 0; i < site_count && !wide; ++i) {
        const GuestAddr rip = sites[i].guest_rip;
        if (rip < base_rip || rip - base_rip > std::numeric_limits<u32>::max()) {
            wide = true;
        }
    }
    u32 flag_count = 0;
    usize flag_bytes = 0;
    for (u32 i = 0; i < site_count; ++i) {
        if (!sites[i].flags.published()) continue;
        ++flag_count;
        flag_bytes += rip_flag_entry_bytes(rip_flag_imm_mask(sites[i].flags));
    }
    const usize site_bytes =
        static_cast<usize>(site_count) *
        (wide ? sizeof(CompactRipSite) : sizeof(NarrowRipSite));
    void* raw = ::operator new(sizeof(JitRipBlock) + site_bytes + flag_bytes,
                               std::nothrow);
    if (raw == nullptr) return reject("allocation failure");
    auto* block = new (raw) JitRipBlock{};
    block->lo = code;
    block->code_size = code_size;
    block->base_rip = base_rip;
    block->site_count = static_cast<u16>(site_count);
    block->flag_count = static_cast<u16>(flag_count);
    block->pins = pins;
    block->wide = wide;
    block->cold_offset = static_cast<u16>(cold_offset);
    block->cold_size = static_cast<u16>(cold_size);
    if (wide) {
        auto* compact = reinterpret_cast<CompactRipSite*>(block->payload());
        for (u32 i = 0; i < site_count; ++i) {
            compact[i] = CompactRipSite{sites[i].host_begin, sites[i].host_end,
                                        sites[i].guest_rip};
        }
    } else {
        auto* narrow = reinterpret_cast<NarrowRipSite*>(block->payload());
        for (u32 i = 0; i < site_count; ++i) {
            narrow[i] = NarrowRipSite{
                static_cast<u16>(sites[i].host_begin),
                static_cast<u16>(sites[i].host_end),
                static_cast<u32>(sites[i].guest_rip - base_rip)};
        }
    }
    u8* cursor = block->flag_entries();
    for (u32 i = 0; i < site_count; ++i) {
        const JitFlagRecovery& flags = sites[i].flags;
        if (!flags.published()) continue;
        const u8 imm_mask = rip_flag_imm_mask(flags);
        RipFlagHead head{static_cast<u16>(i), flags.flag_op, flags.src_reg,
                         flags.dst_reg, flags.width, flags.mode,
                         static_cast<u8>(imm_mask | rip_undo_bits(flags.rsp_undo))};
        std::memcpy(cursor, &head, sizeof(head));
        cursor += sizeof(head);
        if (imm_mask & 1u) {
            store_u64_unaligned(cursor, flags.src_imm);
            cursor += sizeof(u64);
        }
        if (imm_mask & 2u) {
            store_u64_unaligned(cursor, flags.dst_imm);
            cursor += sizeof(u64);
        }
    }

    const uintptr_t first = block->lo >> kRipGranuleShift;
    const uintptr_t span = ((block->hi() - 1) >> kRipGranuleShift) - first;
    std::lock_guard<std::mutex> lock(g_jit_rip_blocks_mutex);
    if (span > g_jit_rip_max_span.load(std::memory_order_relaxed)) {
        g_jit_rip_max_span.store(span, std::memory_order_release);
    }
    std::atomic<JitRipBlock*>& head = rip_bucket(first);
    block->next = head.load(std::memory_order_relaxed);
    head.store(block, std::memory_order_release);
    return true;
}

bool jit_fault_metadata_for_pc(uintptr_t host_pc,
                               JitFaultMetadata& out) noexcept {
    const uintptr_t granule = host_pc >> kRipGranuleShift;
    const uintptr_t span = g_jit_rip_max_span.load(std::memory_order_acquire);
    for (uintptr_t back = 0; back <= span && back <= granule; ++back) {
        for (const JitRipBlock* block =
                 rip_bucket(granule - back).load(std::memory_order_acquire);
             block != nullptr; block = block->next) {
            if (!block->contains(host_pc)) continue;
            out.pins = block->pins;
            out.has_guest_rip = false;
            out.flags = JitFlagRecovery{};
            const u32 offset = static_cast<u32>(host_pc - block->lo);
            for (u32 i = 0; i < block->site_count; ++i) {
                u32 host_begin, host_end;
                GuestAddr guest_rip;
                block->site(i, host_begin, host_end, guest_rip);
                if (offset >= host_begin && offset < host_end) {
                    out.guest_rip = guest_rip;
                    out.has_guest_rip = true;
                    // Entries are in site order and variable-length; walk
                    // them until the site index passes ours.
                    const u8* cursor = block->flag_entries();
                    for (u32 k = 0; k < block->flag_count; ++k) {
                        RipFlagHead head;
                        std::memcpy(&head, cursor, sizeof(head));
                        if (head.site > i) break;
                        const u8* imm = cursor + sizeof(head);
                        if (head.site == i) {
                            out.flags.flag_op = head.flag_op;
                            out.flags.src_reg = head.src_reg;
                            out.flags.dst_reg = head.dst_reg;
                            out.flags.width = head.width;
                            out.flags.mode = head.mode;
                            out.flags.rsp_undo = rip_undo_from_head(head.imm_mask);
                            if (head.imm_mask & 1u) {
                                out.flags.src_imm = load_u64_unaligned(imm);
                                imm += sizeof(u64);
                            }
                            if (head.imm_mask & 2u) {
                                out.flags.dst_imm = load_u64_unaligned(imm);
                            }
                            break;
                        }
                        cursor += rip_flag_entry_bytes(head.imm_mask);
                    }
                    return true;
                }
                if (offset < host_begin) break;
            }
            return true;
        }
    }
    return false;
}

bool jit_guest_rip_for_pc(uintptr_t host_pc, GuestAddr& guest_rip) noexcept {
    JitFaultMetadata metadata{};
    if (!jit_fault_metadata_for_pc(host_pc, metadata) ||
        !metadata.has_guest_rip) {
        return false;
    }
    guest_rip = metadata.guest_rip;
    return true;
}

bool jit_rebuild_flag_record(const JitFlagRecovery& fr, u64 src_value,
                             u64 dst_value, CpuState& cpu) noexcept {
    if (!fr.active()) return false;
    const auto operand = [&](u8 reg, u64 value, u64 imm) -> u64 {
        const u64 raw = reg == JitFlagRecovery::kImmediate ? imm : value;
        return fr.width == 64 ? raw : static_cast<u64>(static_cast<u32>(raw));
    };
    u64 src = operand(fr.src_reg, src_value, fr.src_imm);
    u64 dst = operand(fr.dst_reg, dst_value, fr.dst_imm);
    if (fr.mode == JitFlagRecovery::MergeCf) {
        // The record INC/DEC would have published (the JIT's own packing,
        // which FlagState materializes): flag_src = (CF << 32) | original at
        // 32 bits, CF << 32 alone at 64 (the original is derived from the
        // result there), flag_dst = the result. The inherited CF is still
        // CpuState's (see MergeCf), so it is read before anything is written.
        const u64 cf = cpu.lazy_cf() ? 1u : 0u;
        switch (static_cast<CpuState::FlagOp>(fr.flag_op)) {
        case CpuState::FlagOp::Inc32:
            src = (cf << 32) | static_cast<u32>(static_cast<u32>(dst) - 1u);
            break;
        case CpuState::FlagOp::Dec32:
            src = (cf << 32) | static_cast<u32>(static_cast<u32>(dst) + 1u);
            break;
        case CpuState::FlagOp::Inc64:
        case CpuState::FlagOp::Dec64:
            src = cf << 32;
            break;
        default:
            return false;
        }
    }
    if (fr.mode == JitFlagRecovery::RecomputeDst) {
        // CMP and TEST discard their result, so nothing holds flag_dst. Both
        // inputs survive instead -- neither operand is written -- and the
        // result is recomputed here from the op. Only the ops the producer is
        // allowed to elide appear; publication rejects anything else, and the
        // default is a belt-and-braces stop rather than a silently wrong
        // record.
        const u64 a = dst;
        switch (static_cast<CpuState::FlagOp>(fr.flag_op)) {
        case CpuState::FlagOp::Add32: case CpuState::FlagOp::Add64:
            dst = a + src; break;
        case CpuState::FlagOp::Sub32: case CpuState::FlagOp::Sub64:
            dst = a - src; break;
        case CpuState::FlagOp::And32: case CpuState::FlagOp::And64:
            dst = a & src; break;
        case CpuState::FlagOp::Or32: case CpuState::FlagOp::Or64:
            dst = a | src; break;
        case CpuState::FlagOp::Xor32: case CpuState::FlagOp::Xor64:
            dst = a ^ src; break;
        default:
            return false;
        }
        if (fr.width == 32) dst = static_cast<u32>(dst);
    }
    cpu.flag_src = src;
    cpu.flag_dst = dst;
    cpu.flag_op = static_cast<CpuState::FlagOp>(fr.flag_op);
    // Elision is offered only for ops that define every arithmetic flag, so
    // no NZCV claim or CF/OF overlay from an earlier instruction can still be
    // live at this point -- the same reasoning that lets
    // `discard_lazy_flags` drop them for a full-flag writer.
    cpu.nzcv_kind = 0;
    cpu.nzcv_bits = 0;
    cpu.cf_overlay = 0;
    cpu.of_overlay = 0;
    return true;
}

bool jit_flag_recovery_for_guest_rip(uintptr_t block_pc, GuestAddr guest_rip,
                                     JitFaultMetadata& out) noexcept {
    const uintptr_t granule = block_pc >> kRipGranuleShift;
    const uintptr_t span = g_jit_rip_max_span.load(std::memory_order_acquire);
    for (uintptr_t back = 0; back <= span && back <= granule; ++back) {
        for (const JitRipBlock* block =
                 rip_bucket(granule - back).load(std::memory_order_acquire);
             block != nullptr; block = block->next) {
            if (!block->contains(block_pc)) continue;
            out.pins = block->pins;
            out.guest_rip = guest_rip;
            out.has_guest_rip = false;
            out.flags = JitFlagRecovery{};
            // A guest RIP appears at most once among a block's sites. Each
            // flag entry names its site; scan them in order and stop at the
            // first whose site carries this RIP.
            const u8* cursor = block->flag_entries();
            for (u32 k = 0; k < block->flag_count; ++k) {
                RipFlagHead head;
                std::memcpy(&head, cursor, sizeof(head));
                u32 host_begin, host_end;
                GuestAddr site_rip;
                block->site(head.site, host_begin, host_end, site_rip);
                if (site_rip == guest_rip) {
                    const u8* imm = cursor + sizeof(head);
                    out.has_guest_rip = true;
                    out.flags.flag_op = head.flag_op;
                    out.flags.src_reg = head.src_reg;
                    out.flags.dst_reg = head.dst_reg;
                    out.flags.width = head.width;
                    out.flags.mode = head.mode;
                    out.flags.rsp_undo = rip_undo_from_head(head.imm_mask);
                    if (head.imm_mask & 1u) {
                        out.flags.src_imm = load_u64_unaligned(imm);
                        imm += sizeof(u64);
                    }
                    if (head.imm_mask & 2u) {
                        out.flags.dst_imm = load_u64_unaligned(imm);
                    }
                    return true;
                }
                cursor += rip_flag_entry_bytes(head.imm_mask);
            }
            return true;
        }
    }
    return false;
}

void retire_jit_rip_range(uintptr_t lo, uintptr_t hi) noexcept {
    if (lo >= hi) return;
    std::lock_guard<std::mutex> lock(g_jit_rip_blocks_mutex);
    // A block wholly inside [lo, hi) starts in one of these granules. Past
    // kRipBucketCount granules every bucket is visited exactly once.
    const uintptr_t first = lo >> kRipGranuleShift;
    const uintptr_t granules = ((hi - 1) >> kRipGranuleShift) - first + 1;
    const usize visits =
        granules < kRipBucketCount ? static_cast<usize>(granules)
                                   : kRipBucketCount;
    for (usize g = 0; g < visits; ++g) {
        std::atomic<JitRipBlock*>& head = rip_bucket(first + g);
        JitRipBlock* kept = nullptr;
        JitRipBlock* node = head.load(std::memory_order_relaxed);
        while (node != nullptr) {
            JitRipBlock* const next = node->next;
            if (node->lo >= lo && node->hi() <= hi) {
                ::operator delete(static_cast<void*>(node));
            } else {
                node->next = kept;
                kept = node;
            }
            node = next;
        }
        head.store(kept, std::memory_order_release);
    }
}

} // namespace vitreo::cpu::jit
