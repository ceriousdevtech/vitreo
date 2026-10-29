// SPDX-License-Identifier: LGPL-2.1-only

/// @file jit_helpers_lock64.cpp
/// @brief JIT helpers for LOCK-prefixed r/m64 XCHG, ALU, INC/DEC/NOT/NEG
/// and XADD.
///
/// LOCK ADC/SBB r/m64 are in jit_helpers_lock_carry.cpp, LOCK BT* in
/// jit_helpers_lock_bt.cpp and LOCK CMPXCHG r/m64 in
/// jit_helpers_lock_cmpxchg.cpp.
///
/// Split out of jit_helpers_lock.cpp.

#include "jit_helpers.hpp"
#include "jit_helpers_lock_internal.hpp"
#include "../lock_prefix.hpp"
#include "core/memory/write_watch_traps.hpp"

#include <atomic>

namespace vitreo::cpu::jit {

// ============================================================================
// LOCK r/m64 helpers (Intel SDM Vol. 2 LOCK + XCHG implicit lock)
// ============================================================================

extern "C" void jit_atomic_xchg64(CpuState* cpu, GuestMemory* mem,
                                    u64 addr, u32 reg_idx) {
    // A locked RMW is a store. The host-pointer fast path below writes
    // straight through the mapping and never reaches GuestMemory, so
    // nothing else marks the page. Record it up front and
    // unconditionally: a consumer that misses an interlocked store
    // (a managed GC reading GetWriteWatch) treats the page as clean and
    // can free an object that is still live, whereas an extra dirty
    // page only costs it a rescan. Over-report, never under-report.
    write_watch::record_store(static_cast<GuestAddr>(addr), 8);
    // Intel SDM Vol. 2 XCHG: memory form asserts LOCK# with or without F0.
    Byte* host = mem->host_ptr_store(addr, sizeof(u64));
    LockPrefixGuard guard(host, sizeof(u64));
    const u64 replacement = cpu->gpr.get64(static_cast<u8>(reg_idx));
    u64 previous = 0;
    if (host != nullptr &&
        reinterpret_cast<uintptr_t>(host) % alignof(u64) == 0) {
        std::atomic_ref<u64> value(*reinterpret_cast<u64*>(host));
        previous = value.exchange(replacement, std::memory_order_seq_cst);
    } else {
        auto result = mem->read_u64(addr);
        if (!result.is_ok()) {
            jit_lock_raise_av(cpu, addr, true);
            return;
        }
        previous = result.value();
        if (mem->write_u64(addr, replacement).is_err()) {
            jit_lock_raise_av(cpu, addr, true);
            return;
        }
    }
    cpu->gpr.set64(static_cast<u8>(reg_idx), previous);
}

extern "C" void jit_lock_or64(CpuState* cpu, GuestMemory* mem, u64 addr,
                               u64 mask) {
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
    u64 previous = 0;
    u64 result = 0;
    if (host == nullptr ||
        reinterpret_cast<uintptr_t>(host) % alignof(u64) != 0) {
        auto read = mem->read_u64(addr);
        if (!read.is_ok()) {
            jit_lock_raise_av(cpu, addr, true);
            return;
        }
        previous = read.value();
        result = previous | mask;
        if (mem->write_u64(addr, result).is_err()) {
            jit_lock_raise_av(cpu, addr, true);
            return;
        }
    } else {
        std::atomic_ref<u64> value(*reinterpret_cast<u64*>(host));
        previous = value.fetch_or(mask, std::memory_order_seq_cst);
        result = previous | mask;
    }
    cpu->materialize_lazy_flags();
    cpu->flags.update_flags_logic(result);
    cpu->set_flag_op(CpuState::FlagOp::None);
    (void)previous;
}

extern "C" void jit_lock_and64(CpuState* cpu, GuestMemory* mem, u64 addr,
                                u64 mask) {
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
    u64 previous = 0;
    u64 result = 0;
    if (host == nullptr ||
        reinterpret_cast<uintptr_t>(host) % alignof(u64) != 0) {
        auto read = mem->read_u64(addr);
        if (!read.is_ok()) {
            jit_lock_raise_av(cpu, addr, true);
            return;
        }
        previous = read.value();
        result = previous & mask;
        if (mem->write_u64(addr, result).is_err()) {
            jit_lock_raise_av(cpu, addr, true);
            return;
        }
    } else {
        std::atomic_ref<u64> value(*reinterpret_cast<u64*>(host));
        previous = value.fetch_and(mask, std::memory_order_seq_cst);
        result = previous & mask;
    }
    cpu->materialize_lazy_flags();
    cpu->flags.update_flags_logic(result);
    cpu->set_flag_op(CpuState::FlagOp::None);
    (void)previous;
}

extern "C" void jit_lock_xor64(CpuState* cpu, GuestMemory* mem, u64 addr,
                                u64 mask) {
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
    u64 previous = 0;
    u64 result = 0;
    if (host == nullptr ||
        reinterpret_cast<uintptr_t>(host) % alignof(u64) != 0) {
        auto read = mem->read_u64(addr);
        if (!read.is_ok()) {
            jit_lock_raise_av(cpu, addr, true);
            return;
        }
        previous = read.value();
        result = previous ^ mask;
        if (mem->write_u64(addr, result).is_err()) {
            jit_lock_raise_av(cpu, addr, true);
            return;
        }
    } else {
        std::atomic_ref<u64> value(*reinterpret_cast<u64*>(host));
        previous = value.fetch_xor(mask, std::memory_order_seq_cst);
        result = previous ^ mask;
    }
    cpu->materialize_lazy_flags();
    cpu->flags.update_flags_logic(result);
    cpu->set_flag_op(CpuState::FlagOp::None);
    (void)previous;
}

extern "C" void jit_lock_add64(CpuState* cpu, GuestMemory* mem, u64 addr,
                                u64 addend) {
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
    u64 previous = 0;
    u64 result = 0;
    if (host == nullptr ||
        reinterpret_cast<uintptr_t>(host) % alignof(u64) != 0) {
        auto read = mem->read_u64(addr);
        if (!read.is_ok()) {
            jit_lock_raise_av(cpu, addr, true);
            return;
        }
        previous = read.value();
        result = previous + addend;
        if (mem->write_u64(addr, result).is_err()) {
            jit_lock_raise_av(cpu, addr, true);
            return;
        }
    } else {
        std::atomic_ref<u64> value(*reinterpret_cast<u64*>(host));
        previous = value.fetch_add(addend, std::memory_order_seq_cst);
        result = previous + addend;
    }
    cpu->materialize_lazy_flags();
    cpu->flags.update_flags_add(previous, addend, result);
    cpu->set_flag_op(CpuState::FlagOp::None);
}

extern "C" void jit_lock_sub64(CpuState* cpu, GuestMemory* mem, u64 addr,
                                u64 subtrahend) {
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
    u64 previous = 0;
    u64 result = 0;
    if (host == nullptr ||
        reinterpret_cast<uintptr_t>(host) % alignof(u64) != 0) {
        auto read = mem->read_u64(addr);
        if (!read.is_ok()) {
            jit_lock_raise_av(cpu, addr, true);
            return;
        }
        previous = read.value();
        result = previous - subtrahend;
        if (mem->write_u64(addr, result).is_err()) {
            jit_lock_raise_av(cpu, addr, true);
            return;
        }
    } else {
        std::atomic_ref<u64> value(*reinterpret_cast<u64*>(host));
        previous = value.fetch_sub(subtrahend, std::memory_order_seq_cst);
        result = previous - subtrahend;
    }
    cpu->materialize_lazy_flags();
    cpu->flags.update_flags_sub(previous, subtrahend, result);
    cpu->set_flag_op(CpuState::FlagOp::None);
}

extern "C" void jit_lock_inc64(CpuState* cpu, GuestMemory* mem, u64 addr) {
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
        const u64 val = result.value();
        const u64 incremented = val + 1ull;
        cpu->materialize_lazy_flags();
        const bool saved_cf = cpu->flags.cf();
        cpu->flags.update_flags_add(val, 1ull, incremented);
        cpu->flags.set_cf(saved_cf);
        if (mem->write_u64(addr, incremented).is_err()) {
            jit_lock_raise_av(cpu, addr, true);
            return;
        }
        cpu->set_flag_op(CpuState::FlagOp::None);
        return;
    }
    std::atomic_ref<u64> value(*reinterpret_cast<u64*>(host));
    const u64 val = value.fetch_add(1ull, std::memory_order_seq_cst);
    const u64 result = val + 1ull;
    cpu->materialize_lazy_flags();
    const bool saved_cf = cpu->flags.cf();
    cpu->flags.update_flags_add(val, 1ull, result);
    cpu->flags.set_cf(saved_cf);
    cpu->set_flag_op(CpuState::FlagOp::None);
}

extern "C" void jit_lock_dec64(CpuState* cpu, GuestMemory* mem, u64 addr) {
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
        const u64 val = result.value();
        const u64 decremented = val - 1ull;
        cpu->materialize_lazy_flags();
        const bool saved_cf = cpu->flags.cf();
        cpu->flags.update_flags_sub(val, 1ull, decremented);
        cpu->flags.set_cf(saved_cf);
        if (mem->write_u64(addr, decremented).is_err()) {
            jit_lock_raise_av(cpu, addr, true);
            return;
        }
        cpu->set_flag_op(CpuState::FlagOp::None);
        return;
    }
    std::atomic_ref<u64> value(*reinterpret_cast<u64*>(host));
    const u64 val = value.fetch_sub(1ull, std::memory_order_seq_cst);
    const u64 result = val - 1ull;
    cpu->materialize_lazy_flags();
    const bool saved_cf = cpu->flags.cf();
    cpu->flags.update_flags_sub(val, 1ull, result);
    cpu->flags.set_cf(saved_cf);
    cpu->set_flag_op(CpuState::FlagOp::None);
}

extern "C" void jit_lock_not64(CpuState* cpu, GuestMemory* mem, u64 addr) {
    // A locked RMW is a store. The host-pointer fast path below writes
    // straight through the mapping and never reaches GuestMemory, so
    // nothing else marks the page. Record it up front and
    // unconditionally: a consumer that misses an interlocked store
    // (a managed GC reading GetWriteWatch) treats the page as clean and
    // can free an object that is still live, whereas an extra dirty
    // page only costs it a rescan. Over-report, never under-report.
    write_watch::record_store(static_cast<GuestAddr>(addr), 8);
    // Intel SDM Vol. 2 NOT: one's complement; flags unaffected.
    Byte* host = mem->host_ptr_store(addr, sizeof(u64));
    LockPrefixGuard guard(host, sizeof(u64));
    if (host == nullptr ||
        reinterpret_cast<uintptr_t>(host) % alignof(u64) != 0) {
        auto read = mem->read_u64(addr);
        if (!read.is_ok()) {
            jit_lock_raise_av(cpu, addr, true);
            return;
        }
        if (mem->write_u64(addr, ~read.value()).is_err()) {
            jit_lock_raise_av(cpu, addr, true);
            return;
        }
        return;
    }
    std::atomic_ref<u64> value(*reinterpret_cast<u64*>(host));
    u64 previous = value.load(std::memory_order_seq_cst);
    while (!value.compare_exchange_weak(
        previous, ~previous, std::memory_order_seq_cst,
        std::memory_order_seq_cst)) {}
}

extern "C" void jit_lock_neg64(CpuState* cpu, GuestMemory* mem, u64 addr) {
    // A locked RMW is a store. The host-pointer fast path below writes
    // straight through the mapping and never reaches GuestMemory, so
    // nothing else marks the page. Record it up front and
    // unconditionally: a consumer that misses an interlocked store
    // (a managed GC reading GetWriteWatch) treats the page as clean and
    // can free an object that is still live, whereas an extra dirty
    // page only costs it a rescan. Over-report, never under-report.
    write_watch::record_store(static_cast<GuestAddr>(addr), 8);
    // Intel SDM Vol. 2 NEG: two's complement; flags from 0 − DEST.
    Byte* host = mem->host_ptr_store(addr, sizeof(u64));
    LockPrefixGuard guard(host, sizeof(u64));
    u64 val = 0;
    u64 result = 0;
    if (host == nullptr ||
        reinterpret_cast<uintptr_t>(host) % alignof(u64) != 0) {
        auto read = mem->read_u64(addr);
        if (!read.is_ok()) {
            jit_lock_raise_av(cpu, addr, true);
            return;
        }
        val = read.value();
        result = static_cast<u64>(0) - val;
        if (mem->write_u64(addr, result).is_err()) {
            jit_lock_raise_av(cpu, addr, true);
            return;
        }
    } else {
        std::atomic_ref<u64> value(*reinterpret_cast<u64*>(host));
        val = value.load(std::memory_order_seq_cst);
        for (;;) {
            result = static_cast<u64>(0) - val;
            if (value.compare_exchange_weak(
                    val, result, std::memory_order_seq_cst,
                    std::memory_order_seq_cst)) {
                break;
            }
        }
    }
    cpu->materialize_lazy_flags();
    cpu->flags.update_flags_sub(static_cast<u64>(0), val, result);
    cpu->flags.set_cf(val != 0);
    cpu->set_flag_op(CpuState::FlagOp::None);
}

extern "C" void jit_lock_xadd64(CpuState* cpu, GuestMemory* mem,
                                  u64 addr, u32 reg_idx) {
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
        const u64 reg_val = cpu->gpr.get64(static_cast<u8>(reg_idx));
        const u64 sum = rm_val + reg_val;
        if (mem->write_u64(addr, sum).is_err()) {
            jit_lock_raise_av(cpu, addr, true);
            return;
        }
        cpu->materialize_lazy_flags();
        cpu->flags.update_flags_add(rm_val, reg_val, sum);
        cpu->gpr.set64(static_cast<u8>(reg_idx), rm_val);
        cpu->set_flag_op(CpuState::FlagOp::None);
        return;
    }
    const u64 reg_val = cpu->gpr.get64(static_cast<u8>(reg_idx));
    std::atomic_ref<u64> value(*reinterpret_cast<u64*>(host));
    const u64 rm_val = value.fetch_add(reg_val, std::memory_order_seq_cst);
    const u64 sum = rm_val + reg_val;
    cpu->materialize_lazy_flags();
    cpu->flags.update_flags_add(rm_val, reg_val, sum);
    cpu->gpr.set64(static_cast<u8>(reg_idx), rm_val);
    cpu->set_flag_op(CpuState::FlagOp::None);
}

} // namespace vitreo::cpu::jit
