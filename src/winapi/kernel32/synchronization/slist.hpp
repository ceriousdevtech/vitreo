#pragma once
/// @file slist.hpp
/// @brief Interlocked singly linked lists (SLIST_HEADER / SLIST_ENTRY), both
///        bitnesses, every operation atomic with respect to every other.
///
/// Layouts are the Windows SDK's (winnt.h, 10.0.26100):
///  - x64: a 16-byte header. Bits 0-15 Depth, 16-63 Sequence; second quadword
///    bits 0-3 Reserved, 4-63 NextEntry (the entry address, which is 16-byte
///    aligned). SLIST_ENTRY is { PSLIST_ENTRY Next } -- 8 bytes.
///  - x86: an 8-byte header { SLIST_ENTRY Next (u32); WORD Depth; WORD
///    Sequence }. SLIST_ENTRY is { u32 Next }.
/// Learn describes the operations as atomic with one another; one process-wide
/// lock provides that across every guest thread.

#include <vitreo/common.hpp>

namespace vitreo {
class GuestMemory;
}

namespace vitreo::winapi::slist {

void initialize(GuestMemory& memory, GuestAddr head, bool is64);
/// Returns the previous first entry (0 if the list was empty).
[[nodiscard]] GuestAddr push(GuestMemory& memory, GuestAddr head,
                             GuestAddr entry, bool is64);
/// Pushes the chain @p first .. @p last (linked through Next) of @p count
/// entries; returns the previous first entry.
[[nodiscard]] GuestAddr push_list(GuestMemory& memory, GuestAddr head,
                                  GuestAddr first, GuestAddr last, u32 count,
                                  bool is64);
/// Removes and returns the first entry (0 if empty).
[[nodiscard]] GuestAddr pop(GuestMemory& memory, GuestAddr head, bool is64);
/// Removes every entry; returns the first (still linked to the rest).
[[nodiscard]] GuestAddr flush(GuestMemory& memory, GuestAddr head, bool is64);
/// The first entry without removing it (RtlFirstEntrySList).
[[nodiscard]] GuestAddr first(GuestMemory& memory, GuestAddr head, bool is64);
[[nodiscard]] u16 depth(GuestMemory& memory, GuestAddr head, bool is64);

} // namespace vitreo::winapi::slist
