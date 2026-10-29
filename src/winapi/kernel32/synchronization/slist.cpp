// SPDX-License-Identifier: LGPL-2.1-only

/// @file slist.cpp
/// @brief Interlocked singly linked lists. See slist.hpp.

#include "winapi/kernel32/synchronization/slist.hpp"

#include "core/memory/guest_memory.hpp"

#include <mutex>

namespace vitreo::winapi::slist {
namespace {

std::mutex& list_mutex() {
    static std::mutex m;
    return m;
}

struct Header {
    GuestAddr next = 0;
    u16 depth = 0;
    u64 sequence = 0;  // 48 bits on x64, 16 on x86
};

Header read_header(GuestMemory& memory, GuestAddr head, bool is64) {
    Header h;
    if (is64) {
        const u64 q0 = memory.read_u64(head).value_or(0);
        const u64 q1 = memory.read_u64(head + 8).value_or(0);
        h.depth = static_cast<u16>(q0 & 0xFFFF);
        h.sequence = q0 >> 16;
        h.next = static_cast<GuestAddr>(q1 & ~u64{0xF});
    } else {
        h.next = memory.read_u32(head).value_or(0);
        h.depth = memory.read_u16(head + 4).value_or(0);
        h.sequence = memory.read_u16(head + 6).value_or(0);
    }
    return h;
}

void write_header(GuestMemory& memory, GuestAddr head, const Header& h,
                  bool is64) {
    if (is64) {
        const u64 q0 = static_cast<u64>(h.depth) |
                       ((h.sequence & 0xFFFFFFFFFFFFull) << 16);
        // Reserved bits 0-3 of the second quadword stay whatever they were.
        const u64 reserved = memory.read_u64(head + 8).value_or(0) & 0xF;
        (void)memory.write_u64(head, q0);
        (void)memory.write_u64(head + 8, (static_cast<u64>(h.next) & ~u64{0xF}) | reserved);
    } else {
        (void)memory.write_u32(head, static_cast<u32>(h.next));
        (void)memory.write_u16(head + 4, h.depth);
        (void)memory.write_u16(head + 6, static_cast<u16>(h.sequence));
    }
}

GuestAddr read_next(GuestMemory& memory, GuestAddr entry, bool is64) {
    return is64 ? static_cast<GuestAddr>(memory.read_u64(entry).value_or(0))
                : memory.read_u32(entry).value_or(0);
}

void write_next(GuestMemory& memory, GuestAddr entry, GuestAddr next, bool is64) {
    if (is64) {
        (void)memory.write_u64(entry, next);
    } else {
        (void)memory.write_u32(entry, static_cast<u32>(next));
    }
}

// Sequence advance per push / push-list, measured on Windows 11 26100
// (tools/oracle_probe/handles_waits_probe.expected, seq_* lines): the x64
// Sequence field goes up by 2 on every push and push-list and is untouched by
// pop and flush; the x86 Sequence word is never changed.
u64 push_sequence_step(bool is64) { return is64 ? 2 : 0; }

} // namespace

void initialize(GuestMemory& memory, GuestAddr head, bool is64) {
    std::lock_guard<std::mutex> lock(list_mutex());
    if (is64) {
        (void)memory.write_u64(head, 0);
        (void)memory.write_u64(head + 8, 0);
    } else {
        (void)memory.write_u64(head, 0);
    }
}

GuestAddr push(GuestMemory& memory, GuestAddr head, GuestAddr entry, bool is64) {
    std::lock_guard<std::mutex> lock(list_mutex());
    Header h = read_header(memory, head, is64);
    const GuestAddr previous = h.next;
    write_next(memory, entry, previous, is64);
    h.next = entry;
    h.depth = static_cast<u16>(h.depth + 1);
    h.sequence += push_sequence_step(is64);
    write_header(memory, head, h, is64);
    return previous;
}

GuestAddr push_list(GuestMemory& memory, GuestAddr head, GuestAddr first,
                    GuestAddr last, u32 count, bool is64) {
    std::lock_guard<std::mutex> lock(list_mutex());
    Header h = read_header(memory, head, is64);
    const GuestAddr previous = h.next;
    write_next(memory, last, previous, is64);
    h.next = first;
    h.depth = static_cast<u16>(h.depth + count);
    h.sequence += push_sequence_step(is64);
    write_header(memory, head, h, is64);
    return previous;
}

GuestAddr pop(GuestMemory& memory, GuestAddr head, bool is64) {
    std::lock_guard<std::mutex> lock(list_mutex());
    Header h = read_header(memory, head, is64);
    const GuestAddr entry = h.next;
    if (entry == 0) return 0;
    h.next = read_next(memory, entry, is64);
    h.depth = static_cast<u16>(h.depth - 1);
    write_header(memory, head, h, is64);
    return entry;
}

GuestAddr flush(GuestMemory& memory, GuestAddr head, bool is64) {
    std::lock_guard<std::mutex> lock(list_mutex());
    Header h = read_header(memory, head, is64);
    const GuestAddr entries = h.next;
    h.next = 0;
    h.depth = 0;
    write_header(memory, head, h, is64);
    return entries;
}

GuestAddr first(GuestMemory& memory, GuestAddr head, bool is64) {
    std::lock_guard<std::mutex> lock(list_mutex());
    return read_header(memory, head, is64).next;
}

u16 depth(GuestMemory& memory, GuestAddr head, bool is64) {
    std::lock_guard<std::mutex> lock(list_mutex());
    return read_header(memory, head, is64).depth;
}

} // namespace vitreo::winapi::slist
