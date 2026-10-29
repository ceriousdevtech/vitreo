// SPDX-License-Identifier: LGPL-2.1-only

/// @file jit_helpers_lock_cmpxchg.cpp
/// @brief JIT helpers for LOCK CMPXCHG r/m32 and r/m64, CMPXCHG8B and
/// CMPXCHG16B.
///
/// The r/m8 and r/m16 compare-exchange forms are in
/// jit_helpers_lock_narrow.cpp.
///
/// Split out of jit_helpers_lock.cpp.

#include "jit_helpers.hpp"
#include "jit_helpers_lock_internal.hpp"
#include "../lock_prefix.hpp"
#include "core/memory/write_watch_traps.hpp"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <utility>

namespace vitreo::cpu::jit {

// Optional bounded diagnostics preserve the native JIT path, unlike PC
// single-stepping. Configure a guest-PC half-open range as "start:end".
static void trace_cmpxchg32(const CpuState& cpu, u64 address, u32 expected,
                            u32 desired, u32 observed, bool exchanged) {
    static const auto range = [] {
        std::pair<u64, u64> result{};
        if (const char* text = std::getenv("VITREO_CMPXCHG32_TRACE_RANGE")) {
            char* end = nullptr;
            result.first = std::strtoull(text, &end, 0);
            if (end && *end == ':') result.second = std::strtoull(end + 1, nullptr, 0);
        }
        return result;
    }();
    if (cpu.rip < range.first || cpu.rip >= range.second) return;
    static std::atomic<unsigned> count{0};
    if (count.fetch_add(1, std::memory_order_relaxed) >= 20000) return;
    std::fprintf(stderr,
        "[cmpxchg32] cpu=%p pc=0x%llx addr=0x%llx expected=%08x desired=%08x observed=%08x exchanged=%d\n",
        static_cast<const void*>(&cpu), static_cast<unsigned long long>(cpu.rip),
        static_cast<unsigned long long>(address), expected, desired, observed,
        exchanged ? 1 : 0);
}

extern "C" void jit_lock_cmpxchg32(CpuState* cpu, GuestMemory* mem,
                                     u64 addr, u32 reg_val) {
    // A locked RMW is a store. The host-pointer fast path below writes
    // straight through the mapping and never reaches GuestMemory, so
    // nothing else marks the page. Record it up front and
    // unconditionally: a consumer that misses an interlocked store
    // (a managed GC reading GetWriteWatch) treats the page as clean and
    // can free an object that is still live, whereas an extra dirty
    // page only costs it a rescan. Over-report, never under-report.
    write_watch::record_store(static_cast<GuestAddr>(addr), 4);
    Byte* host = mem->host_ptr_store(addr, sizeof(u32));
    LockPrefixGuard guard(host, sizeof(u32));
    if (host == nullptr ||
        reinterpret_cast<uintptr_t>(host) % alignof(u32) != 0) {
        auto result = mem->read_u32(addr);
        if (!result.is_ok()) {
            jit_lock_raise_av(cpu, addr, true);
            return;
        }
        const u32 rm_val = result.value();
        const u32 eax =
            static_cast<u32>(cpu->gpr.get64(static_cast<u8>(RegIndex::RAX)));
        // CMPXCHG rewrites ALL SIX status flags from (accumulator - dest)
        // (Intel SDM Vol. 2A, CMPXCHG). The pending lazy op is materialized
        // BEFORE rflags are written, not after: the AV path below returns
        // early, and leaving flag_op pending past that return would let a
        // later flag read materialize the stale op on top of these results.
        //
        // Discarding the op instead (its six flags are overwritten anyway)
        // was measured as an opt-in: 14.1% of 55.3M flag materializations on
        // that load, but only ~2-3% of load time, at the noise floor, and
        // an intermittent hang with it on was never explained. A lock-acquire
        // loop (`lock cmpxchg` + `jnz` retry) spins forever on a stale ZF
        // rather than crashing. Removed 2026-10-11. CMPXCHG8B/16B write ZF
        // only, so a discard there would also lose CF/PF/AF/SF/OF.
        cpu->materialize_lazy_flags();
        cpu->flags.update_flags_sub(eax, rm_val, eax - rm_val);
        if (eax == rm_val) {
            if (mem->write_u32(addr, reg_val).is_err()) {
                jit_lock_raise_av(cpu, addr, true);
                return;
            }
        } else {
            cpu->gpr.set32(static_cast<u8>(RegIndex::RAX), rm_val);
        }
        cpu->set_flag_op(CpuState::FlagOp::None);
        return;
    }
    const u32 eax =
        static_cast<u32>(cpu->gpr.get64(static_cast<u8>(RegIndex::RAX)));
    u32 rm_val = eax;
    std::atomic_ref<u32> value(*reinterpret_cast<u32*>(host));
    const bool exchanged = value.compare_exchange_strong(
        rm_val, reg_val, std::memory_order_seq_cst,
        std::memory_order_seq_cst);
    trace_cmpxchg32(*cpu, addr, eax, reg_val, rm_val, exchanged);
    // Same as the unaligned path above: every status flag is rewritten below,
    // and the lazy op is materialized first so no exit path can leave it
    // pending. Flags are `accumulator - DEST`, measured on real hardware:
    // tools/oracle_probe/cmpxchg_flag_probe.c and its recorded output. The
    // earlier `DEST - accumulator` here cited an oracle that was never run.
    cpu->materialize_lazy_flags();
    u32 cmp_result = eax - rm_val;
    cpu->flags.update_flags_sub(eax, rm_val, cmp_result);
    if (!exchanged) {
        cpu->gpr.set32(static_cast<u8>(RegIndex::RAX), rm_val);
    }
    cpu->set_flag_op(CpuState::FlagOp::None);
}

extern "C" void jit_lock_cmpxchg16b(CpuState* cpu, GuestMemory* mem, u64 addr) {
    // A locked RMW is a store. The host-pointer fast path below writes
    // straight through the mapping and never reaches GuestMemory, so
    // nothing else marks the page. Record it up front and
    // unconditionally: a consumer that misses an interlocked store
    // (a managed GC reading GetWriteWatch) treats the page as clean and
    // can free an object that is still live, whereas an extra dirty
    // page only costs it a rescan. Over-report, never under-report.
    write_watch::record_store(static_cast<GuestAddr>(addr), 16);
    // Intel SDM Vol. 2 CMPXCHG16B + LOCK: 16-byte-aligned m128; compare
    // RDX:RAX; equal → ZF=1 store RCX:RBX atomically; else ZF=0 load m128
    // into RDX:RAX. Unaligned is #GP (STATUS_ACCESS_VIOLATION), same as
    // unlocked jit_cmpxchg16b. No host pointer → pending_raise, never a
    // non-atomic store.
    if ((addr & 0xFu) != 0) {
        if (!cpu->pending_raise.active) {
            auto& pending = cpu->pending_raise;
            pending.active = true;
            pending.from_helper = true;
            pending.code = 0xC0000005u;
            pending.flags = 0;
            pending.num_args = 2;
            pending.args[0] = 0;
            pending.args[1] = addr;
        }
        return;
    }

    Byte* host = mem->host_ptr_store(addr, 16);
    LockPrefixGuard guard(host, 16);
    if (host == nullptr ||
        (reinterpret_cast<uintptr_t>(host) & 0xFu) != 0) {
        jit_lock_raise_av(cpu, addr, true);
        return;
    }

    const u64 rax = cpu->gpr.get64(static_cast<u8>(RegIndex::RAX));
    const u64 rdx = cpu->gpr.get64(static_cast<u8>(RegIndex::RDX));
    const u64 rbx = cpu->gpr.get64(static_cast<u8>(RegIndex::RBX));
    const u64 rcx = cpu->gpr.get64(static_cast<u8>(RegIndex::RCX));
    __uint128_t observed = (static_cast<__uint128_t>(rdx) << 64) | rax;
    const __uint128_t replacement =
        (static_cast<__uint128_t>(rcx) << 64) | rbx;
    const bool exchanged = __atomic_compare_exchange_n(
        reinterpret_cast<__uint128_t*>(host),
        &observed,
        replacement,
        false,
        __ATOMIC_SEQ_CST,
        __ATOMIC_SEQ_CST);
    cpu->materialize_lazy_flags();
    cpu->flags.set_zf(exchanged);
    if (!exchanged) {
        cpu->gpr.set64(static_cast<u8>(RegIndex::RAX),
                       static_cast<u64>(observed));
        cpu->gpr.set64(static_cast<u8>(RegIndex::RDX),
                       static_cast<u64>(observed >> 64));
    }
}

extern "C" void jit_lock_cmpxchg8b(CpuState* cpu, GuestMemory* mem, u64 addr) {
    // A locked RMW is a store. The host-pointer fast path below writes
    // straight through the mapping and never reaches GuestMemory, so
    // nothing else marks the page. Record it up front and
    // unconditionally: a consumer that misses an interlocked store
    // (a managed GC reading GetWriteWatch) treats the page as clean and
    // can free an object that is still live, whereas an extra dirty
    // page only costs it a rescan. Over-report, never under-report.
    write_watch::record_store(static_cast<GuestAddr>(addr), 8);
    Byte* host = mem->host_ptr_store(addr, sizeof(u64));
    LockPrefixGuard guard(host, sizeof(u64));
    if (host == nullptr ||
        reinterpret_cast<uintptr_t>(host) % alignof(u64) != 0) {
        const auto low = mem->read_u32(addr);
        const auto high = mem->read_u32(addr + 4);
        if (!low.is_ok() || !high.is_ok()) {
            jit_lock_raise_av(cpu, addr, true);
            return;
        }
        const u32 eax =
            static_cast<u32>(cpu->gpr.get64(static_cast<u8>(RegIndex::RAX)));
        const u32 edx =
            static_cast<u32>(cpu->gpr.get64(static_cast<u8>(RegIndex::RDX)));
        cpu->materialize_lazy_flags();
        const bool equal = low.value() == eax && high.value() == edx;
        cpu->flags.set_zf(equal);
        if (equal) {
            (void)mem->write_u32(
                addr,
                static_cast<u32>(
                    cpu->gpr.get64(static_cast<u8>(RegIndex::RBX))));
            (void)mem->write_u32(
                addr + 4,
                static_cast<u32>(
                    cpu->gpr.get64(static_cast<u8>(RegIndex::RCX))));
        } else {
            cpu->gpr.set32(static_cast<u8>(RegIndex::RAX), low.value());
            cpu->gpr.set32(static_cast<u8>(RegIndex::RDX), high.value());
        }
        return;
    }
    const u32 eax =
        static_cast<u32>(cpu->gpr.get64(static_cast<u8>(RegIndex::RAX)));
    const u32 edx =
        static_cast<u32>(cpu->gpr.get64(static_cast<u8>(RegIndex::RDX)));
    const u32 ebx =
        static_cast<u32>(cpu->gpr.get64(static_cast<u8>(RegIndex::RBX)));
    const u32 ecx =
        static_cast<u32>(cpu->gpr.get64(static_cast<u8>(RegIndex::RCX)));
    u64 observed = (static_cast<u64>(edx) << 32) | eax;
    const u64 replacement = (static_cast<u64>(ecx) << 32) | ebx;
    std::atomic_ref<u64> value(*reinterpret_cast<u64*>(host));
    const bool exchanged = value.compare_exchange_strong(
        observed, replacement, std::memory_order_seq_cst,
        std::memory_order_seq_cst);
    cpu->materialize_lazy_flags();
    cpu->flags.set_zf(exchanged);
    if (!exchanged) {
        cpu->gpr.set32(
            static_cast<u8>(RegIndex::RAX),
            static_cast<u32>(observed));
        cpu->gpr.set32(
            static_cast<u8>(RegIndex::RDX),
            static_cast<u32>(observed >> 32));
    }
}

extern "C" void jit_lock_cmpxchg64(CpuState* cpu, GuestMemory* mem,
                                     u64 addr, u64 reg_val) {
    // A locked RMW is a store. The host-pointer fast path below writes
    // straight through the mapping and never reaches GuestMemory, so
    // nothing else marks the page. Record it up front and
    // unconditionally: a consumer that misses an interlocked store
    // (a managed GC reading GetWriteWatch) treats the page as clean and
    // can free an object that is still live, whereas an extra dirty
    // page only costs it a rescan. Over-report, never under-report.
    write_watch::record_store(static_cast<GuestAddr>(addr), 8);
    Byte* host = mem->host_ptr_store(addr, sizeof(u64));
    LockPrefixGuard guard(host, sizeof(u64));
    if (host == nullptr ||
        reinterpret_cast<uintptr_t>(host) % alignof(u64) != 0) {
        auto result = mem->read_u64(addr);
        if (!result.is_ok()) {
            jit_lock_raise_av(cpu, addr, true);
            return;
        }
        const u64 rm_val = result.value();
        const u64 rax = cpu->gpr.get64(static_cast<u8>(RegIndex::RAX));
        cpu->materialize_lazy_flags();
        cpu->flags.update_flags_sub(rax, rm_val, rax - rm_val);
        if (rax == rm_val) {
            if (mem->write_u64(addr, reg_val).is_err()) {
                jit_lock_raise_av(cpu, addr, true);
                return;
            }
        } else {
            cpu->gpr.set64(static_cast<u8>(RegIndex::RAX), rm_val);
        }
        cpu->set_flag_op(CpuState::FlagOp::None);
        return;
    }
    const u64 rax = cpu->gpr.get64(static_cast<u8>(RegIndex::RAX));
    u64 rm_val = rax;
    std::atomic_ref<u64> value(*reinterpret_cast<u64*>(host));
    const bool exchanged = value.compare_exchange_strong(
        rm_val, reg_val, std::memory_order_seq_cst,
        std::memory_order_seq_cst);
    cpu->materialize_lazy_flags();
    cpu->flags.update_flags_sub(rax, rm_val, rax - rm_val);
    if (!exchanged) {
        cpu->gpr.set64(static_cast<u8>(RegIndex::RAX), rm_val);
    }
    cpu->set_flag_op(CpuState::FlagOp::None);
}

} // namespace vitreo::cpu::jit
