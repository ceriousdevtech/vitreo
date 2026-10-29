// SPDX-License-Identifier: LGPL-2.1-only

/// @file jit_helpers_lock.cpp
/// @brief JIT helpers for LOCK-prefixed instructions.
///
/// The x86 LOCK prefix promises the read-modify-write is atomic with respect to
/// every other core. A JIT that emits load, modify, store loses that, so each
/// helper here performs the operation through a host atomic instead — which is
/// why the LOCK forms are separate helpers rather than a flag on the ordinary
/// ones.
///
/// Split out of jit_helpers.cpp. Closed in both directions, checked with
/// tools/architecture/plan_extraction.py before moving.
///
/// This file keeps the LOCK r/m32 forms: XCHG, OR/AND/XOR/NOT/NEG,
/// ADD/SUB/INC/DEC and XADD. The other LOCK forms live beside it:
///   - jit_helpers_lock_narrow.cpp: LOCK r/m8 and r/m16, including the
///     kLockNarrow* selectors compile_locked passes in W4;
///   - jit_helpers_lock_cmpxchg.cpp: LOCK CMPXCHG r/m32 and r/m64,
///     CMPXCHG8B and CMPXCHG16B;
///   - jit_helpers_lock64.cpp: LOCK r/m64 XCHG, ALU, INC/DEC/NOT/NEG, XADD;
///   - jit_helpers_lock_carry.cpp: LOCK ADC/SBB r/m32 and r/m64;
///   - jit_helpers_lock_bt.cpp: LOCK BT/BTS/BTR/BTC on a bit string;
///   - jit_helpers_lock_internal.hpp: jit_lock_raise_av, which they share.
/// jit_fused_memcpy moved to jit_helpers_fused_memcpy.cpp and the
/// per-instruction interpreter delegate to jit_helpers_delegate.cpp.

#include "jit_helpers.hpp"
#include "jit_helpers_lock_internal.hpp"
#include "../lock_prefix.hpp"
#include "core/memory/write_watch_traps.hpp"

#include <atomic>
#include <cstdio>
#include <cstdlib>

namespace vitreo::cpu::jit {

namespace {

std::atomic<u32> g_lock_inc32_suspicious_logs{0};

void diagnose_suspicious_inc32(CpuState* cpu, u64 addr, u32 previous) {
    if (previous < 0xFFFFFFF0u ||
        std::getenv("VITREO_LOCK_DIAG") == nullptr) {
        return;
    }
    const u32 slot = g_lock_inc32_suspicious_logs.fetch_add(
        1, std::memory_order_relaxed);
    if (slot >= 64) return;
    std::fprintf(stderr,
                 "[lock-diag] inc32-near-wrap rip=0x%llx addr=0x%llx "
                 "previous=0x%08x result=0x%08x rax=0x%llx "
                 "\n",
                 static_cast<unsigned long long>(cpu ? cpu->rip : 0),
                 static_cast<unsigned long long>(addr), previous,
                 previous + 1u,
                 static_cast<unsigned long long>(
                     cpu ? cpu->gpr.get64(static_cast<u8>(RegIndex::RAX)) : 0));
}

}  // namespace

// ============================================================================
// LOCK-prefix aware helpers
// ============================================================================

extern "C" void jit_atomic_xchg32(CpuState* cpu, GuestMemory* mem,
                                    u64 addr, u32 reg_idx) {
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
    const u32 replacement =
        static_cast<u32>(cpu->gpr.get64(static_cast<u8>(reg_idx)));
    u32 previous = 0;
    if (host != nullptr &&
        reinterpret_cast<uintptr_t>(host) % alignof(u32) == 0) {
        std::atomic_ref<u32> value(*reinterpret_cast<u32*>(host));
        previous = value.exchange(replacement, std::memory_order_seq_cst);
    } else {
        auto result = mem->read_u32(addr);
        if (!result.is_ok()) {
            jit_lock_raise_av(cpu, addr, true);
            return;
        }
        previous = result.value();
        if (mem->write_u32(addr, replacement).is_err()) {
            jit_lock_raise_av(cpu, addr, true);
            return;
        }
    }
    cpu->gpr.set32(static_cast<u8>(reg_idx), previous);
}

extern "C" void jit_lock_or32(CpuState* cpu, GuestMemory* mem, u64 addr,
                               u32 mask) {
    // A locked RMW is a store. The host-pointer fast path below writes
    // straight through the mapping and never reaches GuestMemory, so
    // nothing else marks the page. Record it up front and
    // unconditionally: a consumer that misses an interlocked store
    // (a managed GC reading GetWriteWatch) treats the page as clean and
    // can free an object that is still live, whereas an extra dirty
    // page only costs it a rescan. Over-report, never under-report.
    write_watch::record_store(static_cast<GuestAddr>(addr), 4);
    // Intel SDM Vol. 2: LOCK OR — atomic RMW; ZF/SF/PF from result, CF/OF cleared.
    Byte* host = mem->host_ptr_store(addr, sizeof(u32));
    LockPrefixGuard guard(host, sizeof(u32));
    u32 previous = 0;
    u32 result = 0;
    if (host == nullptr ||
        reinterpret_cast<uintptr_t>(host) % alignof(u32) != 0) {
        auto read = mem->read_u32(addr);
        if (!read.is_ok()) {
            jit_lock_raise_av(cpu, addr, true);
            return;
        }
        previous = read.value();
        result = previous | mask;
        if (mem->write_u32(addr, result).is_err()) {
            jit_lock_raise_av(cpu, addr, true);
            return;
        }
    } else {
        std::atomic_ref<u32> value(*reinterpret_cast<u32*>(host));
        previous = value.fetch_or(mask, std::memory_order_seq_cst);
        result = previous | mask;
    }
    cpu->materialize_lazy_flags();
    cpu->flags.update_flags_logic(result);
    cpu->set_flag_op(CpuState::FlagOp::None);
    (void)previous;
}

extern "C" void jit_lock_and32(CpuState* cpu, GuestMemory* mem, u64 addr,
                                u32 mask) {
    // A locked RMW is a store. The host-pointer fast path below writes
    // straight through the mapping and never reaches GuestMemory, so
    // nothing else marks the page. Record it up front and
    // unconditionally: a consumer that misses an interlocked store
    // (a managed GC reading GetWriteWatch) treats the page as clean and
    // can free an object that is still live, whereas an extra dirty
    // page only costs it a rescan. Over-report, never under-report.
    write_watch::record_store(static_cast<GuestAddr>(addr), 4);
    // Intel SDM Vol. 2: LOCK AND — atomic RMW; ZF/SF/PF from result, CF/OF cleared.
    Byte* host = mem->host_ptr_store(addr, sizeof(u32));
    LockPrefixGuard guard(host, sizeof(u32));
    u32 previous = 0;
    u32 result = 0;
    if (host == nullptr ||
        reinterpret_cast<uintptr_t>(host) % alignof(u32) != 0) {
        auto read = mem->read_u32(addr);
        if (!read.is_ok()) {
            jit_lock_raise_av(cpu, addr, true);
            return;
        }
        previous = read.value();
        result = previous & mask;
        if (mem->write_u32(addr, result).is_err()) {
            jit_lock_raise_av(cpu, addr, true);
            return;
        }
    } else {
        std::atomic_ref<u32> value(*reinterpret_cast<u32*>(host));
        previous = value.fetch_and(mask, std::memory_order_seq_cst);
        result = previous & mask;
    }
    cpu->materialize_lazy_flags();
    cpu->flags.update_flags_logic(result);
    cpu->set_flag_op(CpuState::FlagOp::None);
    (void)previous;
}

extern "C" void jit_lock_xor32(CpuState* cpu, GuestMemory* mem, u64 addr,
                                u32 mask) {
    // A locked RMW is a store. The host-pointer fast path below writes
    // straight through the mapping and never reaches GuestMemory, so
    // nothing else marks the page. Record it up front and
    // unconditionally: a consumer that misses an interlocked store
    // (a managed GC reading GetWriteWatch) treats the page as clean and
    // can free an object that is still live, whereas an extra dirty
    // page only costs it a rescan. Over-report, never under-report.
    write_watch::record_store(static_cast<GuestAddr>(addr), 4);
    // Intel SDM Vol. 2: LOCK XOR — atomic RMW; ZF/SF/PF from result, CF/OF cleared.
    Byte* host = mem->host_ptr_store(addr, sizeof(u32));
    LockPrefixGuard guard(host, sizeof(u32));
    u32 previous = 0;
    u32 result = 0;
    if (host == nullptr ||
        reinterpret_cast<uintptr_t>(host) % alignof(u32) != 0) {
        auto read = mem->read_u32(addr);
        if (!read.is_ok()) {
            jit_lock_raise_av(cpu, addr, true);
            return;
        }
        previous = read.value();
        result = previous ^ mask;
        if (mem->write_u32(addr, result).is_err()) {
            jit_lock_raise_av(cpu, addr, true);
            return;
        }
    } else {
        std::atomic_ref<u32> value(*reinterpret_cast<u32*>(host));
        previous = value.fetch_xor(mask, std::memory_order_seq_cst);
        result = previous ^ mask;
    }
    cpu->materialize_lazy_flags();
    cpu->flags.update_flags_logic(result);
    cpu->set_flag_op(CpuState::FlagOp::None);
    (void)previous;
}

extern "C" void jit_lock_not32(CpuState* cpu, GuestMemory* mem, u64 addr) {
    // A locked RMW is a store. The host-pointer fast path below writes
    // straight through the mapping and never reaches GuestMemory, so
    // nothing else marks the page. Record it up front and
    // unconditionally: a consumer that misses an interlocked store
    // (a managed GC reading GetWriteWatch) treats the page as clean and
    // can free an object that is still live, whereas an extra dirty
    // page only costs it a rescan. Over-report, never under-report.
    write_watch::record_store(static_cast<GuestAddr>(addr), 4);
    // Intel SDM Vol. 2 NOT: one's complement; flags unaffected. There is no
    // fetch_not, so the aligned path CASes, exactly as jit_lock_not64 does.
    Byte* host = mem->host_ptr_store(addr, sizeof(u32));
    LockPrefixGuard guard(host, sizeof(u32));
    if (host == nullptr ||
        reinterpret_cast<uintptr_t>(host) % alignof(u32) != 0) {
        auto read = mem->read_u32(addr);
        if (!read.is_ok()) {
            jit_lock_raise_av(cpu, addr, true);
            return;
        }
        if (mem->write_u32(addr, ~read.value()).is_err()) {
            jit_lock_raise_av(cpu, addr, true);
            return;
        }
        return;
    }
    std::atomic_ref<u32> value(*reinterpret_cast<u32*>(host));
    u32 previous = value.load(std::memory_order_seq_cst);
    while (!value.compare_exchange_weak(
        previous, ~previous, std::memory_order_seq_cst,
        std::memory_order_seq_cst)) {}
}

extern "C" void jit_lock_neg32(CpuState* cpu, GuestMemory* mem, u64 addr) {
    // A locked RMW is a store. The host-pointer fast path below writes
    // straight through the mapping and never reaches GuestMemory, so
    // nothing else marks the page. Record it up front and
    // unconditionally: a consumer that misses an interlocked store
    // (a managed GC reading GetWriteWatch) treats the page as clean and
    // can free an object that is still live, whereas an extra dirty
    // page only costs it a rescan. Over-report, never under-report.
    write_watch::record_store(static_cast<GuestAddr>(addr), 4);
    // Intel SDM Vol. 2 NEG: two's complement; flags from 0 − DEST, and CF is
    // set exactly when the source was nonzero. update_flags_sub is a template,
    // so passing u32 operands selects the 32-bit SF/OF/AF widths — do not
    // widen these to u64 or SF and OF come from bit 63 of a zero-extended value.
    Byte* host = mem->host_ptr_store(addr, sizeof(u32));
    LockPrefixGuard guard(host, sizeof(u32));
    u32 val = 0;
    u32 result = 0;
    if (host == nullptr ||
        reinterpret_cast<uintptr_t>(host) % alignof(u32) != 0) {
        auto read = mem->read_u32(addr);
        if (!read.is_ok()) {
            jit_lock_raise_av(cpu, addr, true);
            return;
        }
        val = read.value();
        result = static_cast<u32>(0) - val;
        if (mem->write_u32(addr, result).is_err()) {
            jit_lock_raise_av(cpu, addr, true);
            return;
        }
    } else {
        std::atomic_ref<u32> value(*reinterpret_cast<u32*>(host));
        val = value.load(std::memory_order_seq_cst);
        for (;;) {
            result = static_cast<u32>(0) - val;
            if (value.compare_exchange_weak(
                    val, result, std::memory_order_seq_cst,
                    std::memory_order_seq_cst)) {
                break;
            }
        }
    }
    cpu->materialize_lazy_flags();
    cpu->flags.update_flags_sub(static_cast<u32>(0), val, result);
    cpu->flags.set_cf(val != 0);
    cpu->set_flag_op(CpuState::FlagOp::None);
}

extern "C" void jit_lock_add32(CpuState* cpu, GuestMemory* mem, u64 addr,
                                u32 addend) {
    // A locked RMW is a store. The host-pointer fast path below writes
    // straight through the mapping and never reaches GuestMemory, so
    // nothing else marks the page. Record it up front and
    // unconditionally: a consumer that misses an interlocked store
    // (a managed GC reading GetWriteWatch) treats the page as clean and
    // can free an object that is still live, whereas an extra dirty
    // page only costs it a rescan. Over-report, never under-report.
    write_watch::record_store(static_cast<GuestAddr>(addr), 4);
    // Intel SDM Vol. 2: LOCK ADD — atomic RMW with full arithmetic flags.
    Byte* host = mem->host_ptr_store(addr, sizeof(u32));
    LockPrefixGuard guard(host, sizeof(u32));
    u32 previous = 0;
    u32 result = 0;
    if (host == nullptr ||
        reinterpret_cast<uintptr_t>(host) % alignof(u32) != 0) {
        auto read = mem->read_u32(addr);
        if (!read.is_ok()) {
            jit_lock_raise_av(cpu, addr, true);
            return;
        }
        previous = read.value();
        result = previous + addend;
        if (mem->write_u32(addr, result).is_err()) {
            jit_lock_raise_av(cpu, addr, true);
            return;
        }
    } else {
        std::atomic_ref<u32> value(*reinterpret_cast<u32*>(host));
        previous = value.fetch_add(addend, std::memory_order_seq_cst);
        result = previous + addend;
    }
    cpu->materialize_lazy_flags();
    cpu->flags.update_flags_add(previous, addend, result);
    cpu->set_flag_op(CpuState::FlagOp::None);
}

extern "C" void jit_lock_sub32(CpuState* cpu, GuestMemory* mem, u64 addr,
                                u32 subtrahend) {
    // A locked RMW is a store. The host-pointer fast path below writes
    // straight through the mapping and never reaches GuestMemory, so
    // nothing else marks the page. Record it up front and
    // unconditionally: a consumer that misses an interlocked store
    // (a managed GC reading GetWriteWatch) treats the page as clean and
    // can free an object that is still live, whereas an extra dirty
    // page only costs it a rescan. Over-report, never under-report.
    write_watch::record_store(static_cast<GuestAddr>(addr), 4);
    // Intel SDM Vol. 2: LOCK SUB — atomic RMW with full arithmetic flags.
    Byte* host = mem->host_ptr_store(addr, sizeof(u32));
    LockPrefixGuard guard(host, sizeof(u32));
    u32 previous = 0;
    u32 result = 0;
    if (host == nullptr ||
        reinterpret_cast<uintptr_t>(host) % alignof(u32) != 0) {
        auto read = mem->read_u32(addr);
        if (!read.is_ok()) {
            jit_lock_raise_av(cpu, addr, true);
            return;
        }
        previous = read.value();
        result = previous - subtrahend;
        if (mem->write_u32(addr, result).is_err()) {
            jit_lock_raise_av(cpu, addr, true);
            return;
        }
    } else {
        std::atomic_ref<u32> value(*reinterpret_cast<u32*>(host));
        previous = value.fetch_sub(subtrahend, std::memory_order_seq_cst);
        result = previous - subtrahend;
    }
    cpu->materialize_lazy_flags();
    cpu->flags.update_flags_sub(previous, subtrahend, result);
    cpu->set_flag_op(CpuState::FlagOp::None);
}

extern "C" void jit_lock_inc32(CpuState* cpu, GuestMemory* mem, u64 addr) {
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
        const u32 val = result.value();
        const u32 incremented = val + 1u;
        diagnose_suspicious_inc32(cpu, addr, val);
        cpu->materialize_lazy_flags();
        const bool saved_cf = cpu->flags.cf();
        cpu->flags.update_flags_add(val, 1u, incremented);
        cpu->flags.set_cf(saved_cf);
        if (mem->write_u32(addr, incremented).is_err()) {
            jit_lock_raise_av(cpu, addr, true);
            return;
        }
        cpu->set_flag_op(CpuState::FlagOp::None);
        return;
    }
    std::atomic_ref<u32> value(*reinterpret_cast<u32*>(host));
    const u32 val = value.fetch_add(1u, std::memory_order_seq_cst);
    u32 result = val + 1;
    diagnose_suspicious_inc32(cpu, addr, val);
    cpu->materialize_lazy_flags();
    bool saved_cf = cpu->flags.cf();
    cpu->flags.update_flags_add(val, 1u, result);
    cpu->flags.set_cf(saved_cf);
    cpu->set_flag_op(CpuState::FlagOp::None);
}

extern "C" void jit_lock_dec32(CpuState* cpu, GuestMemory* mem, u64 addr) {
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
        const u32 val = result.value();
        const u32 decremented = val - 1u;
        cpu->materialize_lazy_flags();
        const bool saved_cf = cpu->flags.cf();
        cpu->flags.update_flags_sub(val, 1u, decremented);
        cpu->flags.set_cf(saved_cf);
        if (mem->write_u32(addr, decremented).is_err()) {
            jit_lock_raise_av(cpu, addr, true);
            return;
        }
        cpu->set_flag_op(CpuState::FlagOp::None);
        return;
    }
    std::atomic_ref<u32> value(*reinterpret_cast<u32*>(host));
    const u32 val = value.fetch_sub(1u, std::memory_order_seq_cst);
    u32 result = val - 1;
    cpu->materialize_lazy_flags();
    bool saved_cf = cpu->flags.cf();
    cpu->flags.update_flags_sub(val, 1u, result);
    cpu->flags.set_cf(saved_cf);
    cpu->set_flag_op(CpuState::FlagOp::None);
}

extern "C" void jit_lock_xadd32(CpuState* cpu, GuestMemory* mem,
                                  u64 addr, u32 reg_idx) {
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
        const u32 reg_val =
            static_cast<u32>(cpu->gpr.get64(static_cast<u8>(reg_idx)));
        const u32 sum = rm_val + reg_val;
        if (mem->write_u32(addr, sum).is_err()) {
            jit_lock_raise_av(cpu, addr, true);
            return;
        }
        cpu->materialize_lazy_flags();
        cpu->flags.update_flags_add(rm_val, reg_val, sum);
        cpu->gpr.set32(static_cast<u8>(reg_idx), rm_val);
        cpu->set_flag_op(CpuState::FlagOp::None);
        return;
    }
    const u32 reg_val =
        static_cast<u32>(cpu->gpr.get64(static_cast<u8>(reg_idx)));
    std::atomic_ref<u32> value(*reinterpret_cast<u32*>(host));
    const u32 rm_val =
        value.fetch_add(reg_val, std::memory_order_seq_cst);
    const u32 sum = rm_val + reg_val;
    cpu->materialize_lazy_flags();
    cpu->flags.update_flags_add(rm_val, reg_val, sum);
    cpu->gpr.set32(static_cast<u8>(reg_idx), rm_val);
    cpu->set_flag_op(CpuState::FlagOp::None);
}

} // namespace vitreo::cpu::jit
