// SPDX-License-Identifier: LGPL-2.1-only

/// @file jit_helpers_lock_carry.cpp
/// @brief JIT helpers for LOCK ADC/SBB r/m64 and r/m32, the CF-consuming
/// locked RMWs.
///
/// Split out of jit_helpers_lock.cpp.

#include "jit_helpers.hpp"
#include "jit_helpers_lock_internal.hpp"
#include "../lock_prefix.hpp"
#include "core/memory/write_watch_traps.hpp"

#include <atomic>

namespace vitreo::cpu::jit {

extern "C" void jit_lock_adc64(CpuState* cpu, GuestMemory* mem, u64 addr,
                                u64 addend) {
    // A locked RMW is a store. The host-pointer fast path below writes
    // straight through the mapping and never reaches GuestMemory, so
    // nothing else marks the page. Record it up front and
    // unconditionally: a consumer that misses an interlocked store
    // (a managed GC reading GetWriteWatch) treats the page as clean and
    // can free an object that is still live, whereas an extra dirty
    // page only costs it a rescan. Over-report, never under-report.
    write_watch::record_store(static_cast<GuestAddr>(addr), 8);
    // Intel SDM Vol. 2 ADC + LOCK: DEST := DEST + SRC + CF (atomic RMW).
    const u64 carry = cpu->lazy_cf() ? 1ull : 0ull;
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
        result = previous + addend + carry;
        if (mem->write_u64(addr, result).is_err()) {
            jit_lock_raise_av(cpu, addr, true);
            return;
        }
    } else {
        std::atomic_ref<u64> value(*reinterpret_cast<u64*>(host));
        previous = value.load(std::memory_order_seq_cst);
        for (;;) {
            result = previous + addend + carry;
            if (value.compare_exchange_weak(
                    previous, result, std::memory_order_seq_cst,
                    std::memory_order_seq_cst)) {
                break;
            }
        }
    }
    cpu->set_flag_op(CpuState::FlagOp::None);
    cpu->flags.update_zsp(result);
    cpu->flags.set_cf(result < previous || (carry != 0 && result == previous));
    constexpr u64 sign_bit = u64(1) << 63;
    cpu->flags.set_of(
        ((previous ^ result) & (addend ^ result) & sign_bit) != 0);
    cpu->flags.set_af(((previous ^ addend ^ result) & 0x10u) != 0);
}

extern "C" void jit_lock_sbb64(CpuState* cpu, GuestMemory* mem, u64 addr,
                                u64 subtrahend) {
    // A locked RMW is a store. The host-pointer fast path below writes
    // straight through the mapping and never reaches GuestMemory, so
    // nothing else marks the page. Record it up front and
    // unconditionally: a consumer that misses an interlocked store
    // (a managed GC reading GetWriteWatch) treats the page as clean and
    // can free an object that is still live, whereas an extra dirty
    // page only costs it a rescan. Over-report, never under-report.
    write_watch::record_store(static_cast<GuestAddr>(addr), 8);
    // Intel SDM Vol. 2 SBB + LOCK: DEST := DEST − (SRC + CF) (atomic RMW).
    const u64 carry = cpu->lazy_cf() ? 1ull : 0ull;
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
        result = previous - subtrahend - carry;
        if (mem->write_u64(addr, result).is_err()) {
            jit_lock_raise_av(cpu, addr, true);
            return;
        }
    } else {
        std::atomic_ref<u64> value(*reinterpret_cast<u64*>(host));
        previous = value.load(std::memory_order_seq_cst);
        for (;;) {
            result = previous - subtrahend - carry;
            if (value.compare_exchange_weak(
                    previous, result, std::memory_order_seq_cst,
                    std::memory_order_seq_cst)) {
                break;
            }
        }
    }
    cpu->set_flag_op(CpuState::FlagOp::None);
    cpu->flags.update_zsp(result);
    cpu->flags.set_cf(previous < subtrahend ||
                      (carry != 0 && previous == subtrahend));
    constexpr u64 sign_bit = u64(1) << 63;
    cpu->flags.set_of(
        ((previous ^ subtrahend) & (previous ^ result) & sign_bit) != 0);
    cpu->flags.set_af(((previous ^ subtrahend ^ result) & 0x10u) != 0);
}

extern "C" void jit_lock_adc32(CpuState* cpu, GuestMemory* mem, u64 addr,
                                u32 addend) {
    // A locked RMW is a store. The host-pointer fast path below writes
    // straight through the mapping and never reaches GuestMemory, so
    // nothing else marks the page. Record it up front and
    // unconditionally: a consumer that misses an interlocked store
    // (a managed GC reading GetWriteWatch) treats the page as clean and
    // can free an object that is still live, whereas an extra dirty
    // page only costs it a rescan. Over-report, never under-report.
    write_watch::record_store(static_cast<GuestAddr>(addr), 4);
    // Intel SDM Vol. 2 ADC + LOCK (PE32 / 32-bit operand size).
    const u32 carry = cpu->lazy_cf() ? 1u : 0u;
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
        result = previous + addend + carry;
        if (mem->write_u32(addr, result).is_err()) {
            jit_lock_raise_av(cpu, addr, true);
            return;
        }
    } else {
        std::atomic_ref<u32> value(*reinterpret_cast<u32*>(host));
        previous = value.load(std::memory_order_seq_cst);
        for (;;) {
            result = previous + addend + carry;
            if (value.compare_exchange_weak(
                    previous, result, std::memory_order_seq_cst,
                    std::memory_order_seq_cst)) {
                break;
            }
        }
    }
    cpu->set_flag_op(CpuState::FlagOp::None);
    cpu->flags.update_zsp(result);
    const u64 wide =
        static_cast<u64>(previous) + static_cast<u64>(addend) + carry;
    cpu->flags.set_cf(wide > 0xFFFFFFFFull);
    constexpr u32 sign_bit = u32(1) << 31;
    cpu->flags.set_of(
        ((previous ^ result) & (addend ^ result) & sign_bit) != 0);
    cpu->flags.set_af(((previous ^ addend ^ result) & 0x10u) != 0);
}

extern "C" void jit_lock_sbb32(CpuState* cpu, GuestMemory* mem, u64 addr,
                                u32 subtrahend) {
    // A locked RMW is a store. The host-pointer fast path below writes
    // straight through the mapping and never reaches GuestMemory, so
    // nothing else marks the page. Record it up front and
    // unconditionally: a consumer that misses an interlocked store
    // (a managed GC reading GetWriteWatch) treats the page as clean and
    // can free an object that is still live, whereas an extra dirty
    // page only costs it a rescan. Over-report, never under-report.
    write_watch::record_store(static_cast<GuestAddr>(addr), 4);
    // Intel SDM Vol. 2 SBB + LOCK (PE32 / 32-bit operand size).
    const u32 carry = cpu->lazy_cf() ? 1u : 0u;
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
        result = previous - subtrahend - carry;
        if (mem->write_u32(addr, result).is_err()) {
            jit_lock_raise_av(cpu, addr, true);
            return;
        }
    } else {
        std::atomic_ref<u32> value(*reinterpret_cast<u32*>(host));
        previous = value.load(std::memory_order_seq_cst);
        for (;;) {
            result = previous - subtrahend - carry;
            if (value.compare_exchange_weak(
                    previous, result, std::memory_order_seq_cst,
                    std::memory_order_seq_cst)) {
                break;
            }
        }
    }
    cpu->set_flag_op(CpuState::FlagOp::None);
    cpu->flags.update_zsp(result);
    cpu->flags.set_cf(static_cast<u64>(previous) <
                      static_cast<u64>(subtrahend) + carry);
    constexpr u32 sign_bit = u32(1) << 31;
    cpu->flags.set_of(
        ((previous ^ subtrahend) & (previous ^ result) & sign_bit) != 0);
    cpu->flags.set_af(((previous ^ subtrahend ^ result) & 0x10u) != 0);
}

} // namespace vitreo::cpu::jit
