// SPDX-License-Identifier: LGPL-2.1-only

/// @file jit_helpers_lock_bt.cpp
/// @brief JIT helpers for LOCK BT/BTS/BTR/BTC on a memory bit string.
///
/// Split out of jit_helpers_lock.cpp.

#include "jit_helpers.hpp"
#include "jit_helpers_lock_internal.hpp"
#include "../lock_prefix.hpp"
#include "core/memory/write_watch_traps.hpp"

#include <atomic>

namespace vitreo::cpu::jit {

namespace {

enum class LockBtOp : u32 { Bt = 0, Bts = 1, Btr = 2, Btc = 3 };

void jit_lock_bt_family64(CpuState* cpu, GuestMemory* mem, u64 base_addr,
                          i64 bit_index, LockBtOp op) {
    // Intel SDM Vol. 2 BT/BTS/BTR/BTC memory form: bit string at
    // base + (index >> 3), bit = index & 7. LOCK is architecturally valid
    // for BTS/BTR/BTC; BT under LOCK matches the interpreter mutex path.
    const i64 byte_off = bit_index >> 3;
    const u32 bit_in_byte = static_cast<u32>(bit_index & 7);
    const u64 addr = base_addr + static_cast<u64>(byte_off);
    const u8 mask = static_cast<u8>(1u << bit_in_byte);
    const bool is_write = op != LockBtOp::Bt;
    // The touched byte is base + (index >> 3), which for a large bit index is
    // nowhere near base_addr — so this must be recorded against the computed
    // address, not the operand. Plain BT only reads and is left unrecorded.
    if (is_write) {
        write_watch::record_store(static_cast<GuestAddr>(addr), 1);
    }

    Byte* host = mem->host_ptr_store(addr, sizeof(u8));
    LockPrefixGuard guard(host, sizeof(u8));
    u8 previous = 0;
    if (host == nullptr) {
        auto read = mem->read_u8(addr);
        if (!read.is_ok()) {
            jit_lock_raise_av(cpu, addr, is_write);
            return;
        }
        previous = read.value();
        if (is_write) {
            u8 result = previous;
            switch (op) {
            case LockBtOp::Bts:
                result = static_cast<u8>(previous | mask);
                break;
            case LockBtOp::Btr:
                result = static_cast<u8>(previous & static_cast<u8>(~mask));
                break;
            case LockBtOp::Btc:
                result = static_cast<u8>(previous ^ mask);
                break;
            default:
                break;
            }
            if (mem->write_u8(addr, result).is_err()) {
                jit_lock_raise_av(cpu, addr, true);
                return;
            }
        }
    } else {
        std::atomic_ref<u8> value(*reinterpret_cast<u8*>(host));
        switch (op) {
        case LockBtOp::Bt:
            previous = value.load(std::memory_order_seq_cst);
            break;
        case LockBtOp::Bts:
            previous = value.fetch_or(mask, std::memory_order_seq_cst);
            break;
        case LockBtOp::Btr:
            previous = value.fetch_and(static_cast<u8>(~mask),
                                       std::memory_order_seq_cst);
            break;
        case LockBtOp::Btc:
            previous = value.fetch_xor(mask, std::memory_order_seq_cst);
            break;
        }
    }
    cpu->write_cf_only(((previous >> bit_in_byte) & 1) != 0);
}

}  // namespace

extern "C" void jit_lock_bt64(CpuState* cpu, GuestMemory* mem, u64 addr,
                               i64 bit_index) {
    jit_lock_bt_family64(cpu, mem, addr, bit_index, LockBtOp::Bt);
}

extern "C" void jit_lock_bts64(CpuState* cpu, GuestMemory* mem, u64 addr,
                                i64 bit_index) {
    jit_lock_bt_family64(cpu, mem, addr, bit_index, LockBtOp::Bts);
}

extern "C" void jit_lock_btr64(CpuState* cpu, GuestMemory* mem, u64 addr,
                                i64 bit_index) {
    jit_lock_bt_family64(cpu, mem, addr, bit_index, LockBtOp::Btr);
}

extern "C" void jit_lock_btc64(CpuState* cpu, GuestMemory* mem, u64 addr,
                                i64 bit_index) {
    jit_lock_bt_family64(cpu, mem, addr, bit_index, LockBtOp::Btc);
}

} // namespace vitreo::cpu::jit
