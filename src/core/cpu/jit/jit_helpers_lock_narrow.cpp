// SPDX-License-Identifier: LGPL-2.1-only

/// @file jit_helpers_lock_narrow.cpp
/// @brief JIT helpers for LOCK-prefixed r/m8 and r/m16 instructions.
///
/// OR/AND r/m16, CMPXCHG r/m8, and the width-generic r/m8/r/m16 RMW
/// (ALU, XADD, XCHG, CMPXCHG r/m16) behind the kLockNarrow* selectors that
/// compile_locked (jit/ops/memory_ops.cpp) passes in W4.
///
/// Split out of jit_helpers_lock.cpp.

#include "jit_helpers.hpp"
#include "jit_helpers_lock_internal.hpp"
#include "../lock_prefix.hpp"
#include "core/memory/write_watch_traps.hpp"

#include <atomic>
#include <optional>

namespace vitreo::cpu::jit {

extern "C" void jit_lock_or16(CpuState* cpu, GuestMemory* mem, u64 addr,
                               u32 mask) {
    // A locked RMW is a store. The host-pointer fast path below writes
    // straight through the mapping and never reaches GuestMemory, so
    // nothing else marks the page. Record it up front and
    // unconditionally: a consumer that misses an interlocked store
    // (a managed GC reading GetWriteWatch) treats the page as clean and
    // can free an object that is still live, whereas an extra dirty
    // page only costs it a rescan. Over-report, never under-report.
    write_watch::record_store(static_cast<GuestAddr>(addr), 2);
    // Intel SDM Vol. 2: LOCK OR r/m16 — atomic RMW; flags from 16-bit result.
    const u16 m = static_cast<u16>(mask);
    Byte* host = mem->host_ptr_store(addr, sizeof(u16));
    LockPrefixGuard guard(host, sizeof(u16));
    u16 previous = 0;
    u16 result = 0;
    if (host == nullptr ||
        reinterpret_cast<uintptr_t>(host) % alignof(u16) != 0) {
        auto read = mem->read_u16(addr);
        if (!read.is_ok()) {
            jit_lock_raise_av(cpu, addr, true);
            return;
        }
        previous = read.value();
        result = static_cast<u16>(previous | m);
        if (mem->write_u16(addr, result).is_err()) {
            jit_lock_raise_av(cpu, addr, true);
            return;
        }
    } else {
        std::atomic_ref<u16> value(*reinterpret_cast<u16*>(host));
        previous = value.fetch_or(m, std::memory_order_seq_cst);
        result = static_cast<u16>(previous | m);
    }
    cpu->materialize_lazy_flags();
    cpu->flags.update_flags_logic(result);
    cpu->set_flag_op(CpuState::FlagOp::None);
    (void)previous;
}

extern "C" void jit_lock_and16(CpuState* cpu, GuestMemory* mem, u64 addr,
                                u32 mask) {
    // A locked RMW is a store. The host-pointer fast path below writes
    // straight through the mapping and never reaches GuestMemory, so
    // nothing else marks the page. Record it up front and
    // unconditionally: a consumer that misses an interlocked store
    // (a managed GC reading GetWriteWatch) treats the page as clean and
    // can free an object that is still live, whereas an extra dirty
    // page only costs it a rescan. Over-report, never under-report.
    write_watch::record_store(static_cast<GuestAddr>(addr), 2);
    // Intel SDM Vol. 2: LOCK AND r/m16 — atomic RMW; flags from 16-bit result.
    const u16 m = static_cast<u16>(mask);
    Byte* host = mem->host_ptr_store(addr, sizeof(u16));
    LockPrefixGuard guard(host, sizeof(u16));
    u16 previous = 0;
    u16 result = 0;
    if (host == nullptr ||
        reinterpret_cast<uintptr_t>(host) % alignof(u16) != 0) {
        auto read = mem->read_u16(addr);
        if (!read.is_ok()) {
            jit_lock_raise_av(cpu, addr, true);
            return;
        }
        previous = read.value();
        result = static_cast<u16>(previous & m);
        if (mem->write_u16(addr, result).is_err()) {
            jit_lock_raise_av(cpu, addr, true);
            return;
        }
    } else {
        std::atomic_ref<u16> value(*reinterpret_cast<u16*>(host));
        previous = value.fetch_and(m, std::memory_order_seq_cst);
        result = static_cast<u16>(previous & m);
    }
    cpu->materialize_lazy_flags();
    cpu->flags.update_flags_logic(result);
    cpu->set_flag_op(CpuState::FlagOp::None);
    (void)previous;
}

extern "C" void jit_lock_cmpxchg8(CpuState* cpu, GuestMemory* mem,
                                    u64 addr, u32 reg_val) {
    // A locked RMW is a store. The host-pointer fast path below writes
    // straight through the mapping and never reaches GuestMemory, so
    // nothing else marks the page. Record it up front and
    // unconditionally: a consumer that misses an interlocked store
    // (a managed GC reading GetWriteWatch) treats the page as clean and
    // can free an object that is still live, whereas an extra dirty
    // page only costs it a rescan. Over-report, never under-report.
    write_watch::record_store(static_cast<GuestAddr>(addr), 1);
    // Intel SDM Vol. 2: CMPXCHG r/m8, r8 — compare AL with r/m8; on equal
    // store r8, else load r/m8 into AL. LOCK makes the RMW atomic.
    const u8 desired = static_cast<u8>(reg_val);
    Byte* host = mem->host_ptr_store(addr, sizeof(u8));
    LockPrefixGuard guard(host, sizeof(u8));
    if (host == nullptr) {
        auto result = mem->read_u8(addr);
        if (!result.is_ok()) {
            jit_lock_raise_av(cpu, addr, true);
            return;
        }
        const u8 rm_val = result.value();
        const u8 al = cpu->gpr.get8l(static_cast<u8>(RegIndex::RAX));
        cpu->materialize_lazy_flags();
        cpu->flags.update_flags_sub(al, rm_val, static_cast<u8>(al - rm_val));
        if (al == rm_val) {
            if (mem->write_u8(addr, desired).is_err()) {
                jit_lock_raise_av(cpu, addr, true);
                return;
            }
        } else {
            cpu->gpr.set8l(static_cast<u8>(RegIndex::RAX), rm_val);
        }
        cpu->set_flag_op(CpuState::FlagOp::None);
        return;
    }
    const u8 al = cpu->gpr.get8l(static_cast<u8>(RegIndex::RAX));
    u8 rm_val = al;
    std::atomic_ref<u8> value(*host);
    const bool exchanged = value.compare_exchange_strong(
        rm_val, desired, std::memory_order_seq_cst,
        std::memory_order_seq_cst);
    cpu->materialize_lazy_flags();
    cpu->flags.update_flags_sub(al, rm_val, static_cast<u8>(al - rm_val));
    if (!exchanged) {
        cpu->gpr.set8l(static_cast<u8>(RegIndex::RAX), rm_val);
    }
    cpu->set_flag_op(CpuState::FlagOp::None);
}

// ============================================================================
// LOCK r/m8 and r/m16 (Intel SDM Vol. 2 LOCK; ADD/OR/ADC/SBB/AND/SUB/XOR/
// INC/DEC/NOT/NEG/XADD/XCHG/CMPXCHG)
// ============================================================================
//
// Every one of these used to fall through compile_locked to the per-
// instruction interpreter delegate, whose dispatcher-level LockPrefixGuard has
// no operand information and therefore takes the PROCESS-WIDE exclusive
// mutex. A byte-sized lock (`lock xadd byte ptr [x], al`, a std::atomic<bool>
// or <u8> RMW) serialised every thread in the process. Measured in
// A 64-bit title: 4.9% of its LOCK operations went exclusive because of
// LOCK XADD r/m8 alone.
//
// One width-generic RMW per width. The host atomic is the matching-width
// std::atomic_ref (a byte is always naturally aligned; a misaligned word takes
// LockPrefixGuard's exclusive path, exactly as the 16-bit OR/AND helpers do).
// Flags are written only after the store has succeeded, and a faulting access
// raises STATUS_ACCESS_VIOLATION without touching flags or registers.
namespace {

// Selector passed in W4: this file's private contract with compile_locked's
// narrow family (jit/ops/locked_narrow.cpp), which passes LockNarrowOp names.
enum : u32 {
    kLockNarrowAdd = 0, kLockNarrowOr = 1, kLockNarrowAdc = 2,
    kLockNarrowSbb = 3, kLockNarrowAnd = 4, kLockNarrowSub = 5,
    kLockNarrowXor = 6, kLockNarrowInc = 8, kLockNarrowDec = 9,
    kLockNarrowNot = 10, kLockNarrowNeg = 11,
};

template <typename T>
T lock_narrow_compute(u32 op, T a, T b, u32 carry) noexcept {
    switch (op) {
    case kLockNarrowAdd: return static_cast<T>(a + b);
    case kLockNarrowOr:  return static_cast<T>(a | b);
    case kLockNarrowAdc: return static_cast<T>(a + b + carry);
    case kLockNarrowSbb: return static_cast<T>(a - b - carry);
    case kLockNarrowAnd: return static_cast<T>(a & b);
    case kLockNarrowSub: return static_cast<T>(a - b);
    case kLockNarrowXor: return static_cast<T>(a ^ b);
    case kLockNarrowInc: return static_cast<T>(a + 1u);
    case kLockNarrowDec: return static_cast<T>(a - 1u);
    case kLockNarrowNot: return static_cast<T>(~a);
    case kLockNarrowNeg: return static_cast<T>(0u - a);
    default:             return a;
    }
}

// Intel SDM Vol. 2 flag effects, per instruction, at the operand width.
template <typename T>
void lock_narrow_flags(CpuState* cpu, u32 op, T a, T b, u32 carry,
                       T r) noexcept {
    constexpr u64 kMax = static_cast<u64>(static_cast<T>(~T{0}));
    constexpr T sign = static_cast<T>(T{1} << (sizeof(T) * 8 - 1));
    if (op == kLockNarrowNot) return;  // NOT: flags unaffected
    cpu->materialize_lazy_flags();
    switch (op) {
    case kLockNarrowAdd: cpu->flags.update_flags_add(a, b, r); break;
    case kLockNarrowSub: cpu->flags.update_flags_sub(a, b, r); break;
    case kLockNarrowOr:
    case kLockNarrowAnd:
    case kLockNarrowXor: cpu->flags.update_flags_logic(r); break;
    case kLockNarrowAdc:
        cpu->flags.update_zsp(r);
        cpu->flags.set_cf(static_cast<u64>(a) + b + carry > kMax);
        cpu->flags.set_of(((a ^ r) & (b ^ r) & sign) != 0);
        cpu->flags.set_af(((a ^ b ^ r) & 0x10u) != 0);
        break;
    case kLockNarrowSbb:
        cpu->flags.update_zsp(r);
        cpu->flags.set_cf(static_cast<u64>(a) < static_cast<u64>(b) + carry);
        cpu->flags.set_of(((a ^ b) & (a ^ r) & sign) != 0);
        cpu->flags.set_af(((a ^ b ^ r) & 0x10u) != 0);
        break;
    case kLockNarrowInc: {
        const bool cf = cpu->flags.cf();           // INC: CF unaffected
        cpu->flags.update_flags_add(a, T{1}, r);
        cpu->flags.set_cf(cf);
        break;
    }
    case kLockNarrowDec: {
        const bool cf = cpu->flags.cf();           // DEC: CF unaffected
        cpu->flags.update_flags_sub(a, T{1}, r);
        cpu->flags.set_cf(cf);
        break;
    }
    case kLockNarrowNeg:                           // NEG: 0 - DEST, CF = DEST != 0
        cpu->flags.update_flags_sub(T{0}, a, r);
        cpu->flags.set_cf(a != 0);
        break;
    default: break;
    }
    cpu->set_flag_op(CpuState::FlagOp::None);
}

template <typename T>
std::optional<T> lock_narrow_read(GuestMemory* mem, u64 addr) noexcept {
    if constexpr (sizeof(T) == 1) {
        auto r = mem->read_u8(addr);
        if (!r.is_ok()) return std::nullopt;
        return r.value();
    } else {
        auto r = mem->read_u16(addr);
        if (!r.is_ok()) return std::nullopt;
        return r.value();
    }
}

template <typename T>
bool lock_narrow_write(GuestMemory* mem, u64 addr, T v) noexcept {
    if constexpr (sizeof(T) == 1) return !mem->write_u8(addr, v).is_err();
    else return !mem->write_u16(addr, v).is_err();
}

/// Atomically DEST := f(DEST); returns the previous value, or nullopt after
/// raising the access violation. `f` is pure and may run more than once.
template <typename T, typename F>
std::optional<T> lock_narrow_rmw(CpuState* cpu, GuestMemory* mem, u64 addr,
                                 F&& f) noexcept {
    // A locked RMW is a store; the host fast path never reaches GuestMemory,
    // so mark the page up front (see jit_atomic_xchg32).
    write_watch::record_store(static_cast<GuestAddr>(addr), sizeof(T));
    Byte* host = mem->host_ptr_store(addr, sizeof(T));
    LockPrefixGuard guard(host, sizeof(T));
    if (host == nullptr ||
        reinterpret_cast<uintptr_t>(host) % alignof(T) != 0) {
        const auto read = lock_narrow_read<T>(mem, addr);
        if (!read) {
            jit_lock_raise_av(cpu, addr, true);
            return std::nullopt;
        }
        if (!lock_narrow_write<T>(mem, addr, f(*read))) {
            jit_lock_raise_av(cpu, addr, true);
            return std::nullopt;
        }
        return *read;
    }
    std::atomic_ref<T> value(*reinterpret_cast<T*>(host));
    T previous = value.load(std::memory_order_seq_cst);
    while (!value.compare_exchange_weak(previous, f(previous),
                                        std::memory_order_seq_cst,
                                        std::memory_order_seq_cst)) {}
    return previous;
}

template <typename T>
void lock_narrow_alu(CpuState* cpu, GuestMemory* mem, u64 addr, u32 operand,
                     u32 op) noexcept {
    const T b = static_cast<T>(operand);
    // ADC/SBB consume CF as it stood before the instruction.
    const u32 carry =
        (op == kLockNarrowAdc || op == kLockNarrowSbb) && cpu->lazy_cf() ? 1u
                                                                         : 0u;
    const auto previous = lock_narrow_rmw<T>(cpu, mem, addr, [&](T a) {
        return lock_narrow_compute<T>(op, a, b, carry);
    });
    if (!previous) return;
    lock_narrow_flags<T>(cpu, op, *previous, b, carry,
                         lock_narrow_compute<T>(op, *previous, b, carry));
}

// r8 operand encoding for the XADD/XCHG helpers: bits 3:0 the register
// number, bit 8 set for the legacy high-byte registers AH/CH/DH/BH (register
// 4-7 with no REX prefix), which name bits 15:8 of register (n - 4).
u8 lock_read_r8(const CpuState* cpu, u32 code) noexcept {
    const u8 n = static_cast<u8>(code & 15);
    return (code & 0x100u) ? cpu->gpr.get8h(static_cast<u8>(n - 4))
                           : cpu->gpr.get8l(n);
}

void lock_write_r8(CpuState* cpu, u32 code, u8 v) noexcept {
    const u8 n = static_cast<u8>(code & 15);
    if (code & 0x100u) cpu->gpr.set8h(static_cast<u8>(n - 4), v);
    else cpu->gpr.set8l(n, v);
}

}  // namespace

extern "C" void jit_lock_alu8(CpuState* cpu, GuestMemory* mem, u64 addr,
                              u32 operand, u32 op) {
    lock_narrow_alu<u8>(cpu, mem, addr, operand, op);
}

extern "C" void jit_lock_alu16(CpuState* cpu, GuestMemory* mem, u64 addr,
                               u32 operand, u32 op) {
    lock_narrow_alu<u16>(cpu, mem, addr, operand, op);
}

// Intel SDM Vol. 2 XADD: TEMP := SRC + DEST; SRC := DEST; DEST := TEMP; flags
// from the addition. The register receives the old value only after the
// locked RMW has committed.
extern "C" void jit_lock_xadd8(CpuState* cpu, GuestMemory* mem, u64 addr,
                               u32 reg_code) {
    const u8 src = lock_read_r8(cpu, reg_code);
    const auto previous = lock_narrow_rmw<u8>(cpu, mem, addr, [&](u8 a) {
        return static_cast<u8>(a + src);
    });
    if (!previous) return;
    const u8 sum = static_cast<u8>(*previous + src);
    cpu->materialize_lazy_flags();
    cpu->flags.update_flags_add(*previous, src, sum);
    cpu->set_flag_op(CpuState::FlagOp::None);
    lock_write_r8(cpu, reg_code, *previous);
}

extern "C" void jit_lock_xadd16(CpuState* cpu, GuestMemory* mem, u64 addr,
                                u32 reg_idx) {
    const u8 r = static_cast<u8>(reg_idx & 15);
    const u16 src = cpu->gpr.get16(r);
    const auto previous = lock_narrow_rmw<u16>(cpu, mem, addr, [&](u16 a) {
        return static_cast<u16>(a + src);
    });
    if (!previous) return;
    const u16 sum = static_cast<u16>(*previous + src);
    cpu->materialize_lazy_flags();
    cpu->flags.update_flags_add(*previous, src, sum);
    cpu->set_flag_op(CpuState::FlagOp::None);
    cpu->gpr.set16(r, *previous);
}

// Intel SDM Vol. 2 XCHG: the memory form is locked with or without F0; flags
// unaffected. The 16-bit register write preserves bits 63:16.
extern "C" void jit_atomic_xchg8(CpuState* cpu, GuestMemory* mem, u64 addr,
                                 u32 reg_code) {
    const u8 src = lock_read_r8(cpu, reg_code);
    const auto previous =
        lock_narrow_rmw<u8>(cpu, mem, addr, [&](u8) { return src; });
    if (!previous) return;
    lock_write_r8(cpu, reg_code, *previous);
}

extern "C" void jit_atomic_xchg16(CpuState* cpu, GuestMemory* mem, u64 addr,
                                  u32 reg_idx) {
    const u8 r = static_cast<u8>(reg_idx & 15);
    const u16 src = cpu->gpr.get16(r);
    const auto previous =
        lock_narrow_rmw<u16>(cpu, mem, addr, [&](u16) { return src; });
    if (!previous) return;
    cpu->gpr.set16(r, *previous);
}

// Intel SDM Vol. 2 CMPXCHG r/m16, r16: compare AX with DEST; equal -> DEST :=
// SRC, else AX := DEST. Flags are those of CMP AX, DEST (accumulator minus
// destination, see jit_lock_cmpxchg32). A failed compare still performs a
// locked write of the old value on hardware; writing nothing is equivalent
// for every observer, as in jit_lock_cmpxchg8.
extern "C" void jit_lock_cmpxchg16(CpuState* cpu, GuestMemory* mem, u64 addr,
                                   u32 reg_val) {
    const u16 desired = static_cast<u16>(reg_val);
    const u16 ax = cpu->gpr.get16(static_cast<u8>(RegIndex::RAX));
    const auto previous = lock_narrow_rmw<u16>(cpu, mem, addr, [&](u16 a) {
        return a == ax ? desired : a;
    });
    if (!previous) return;
    cpu->materialize_lazy_flags();
    cpu->flags.update_flags_sub(ax, *previous,
                                static_cast<u16>(ax - *previous));
    cpu->set_flag_op(CpuState::FlagOp::None);
    if (*previous != ax) {
        cpu->gpr.set16(static_cast<u8>(RegIndex::RAX), *previous);
    }
}

} // namespace vitreo::cpu::jit
