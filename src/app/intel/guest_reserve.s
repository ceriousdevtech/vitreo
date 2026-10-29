// SPDX-License-Identifier: LGPL-2.1-only
//
// Occupies the ENTIRE 32-bit guest window in the executable's Mach-O so dyld
// maps it before libmalloc. Without this, libmalloc's arenas land inside the
// guest's address space during dyld init -- before any constructor can claim
// it -- and a 0x400000-based EXE larger than 4MB (RCT3 is 13.7MB, no .reloc)
// cannot load at all.
//
// It used to cover only 0x400000-0x8000000 (124MB), which left arenas at
// 0x100000-0x400000 and 0x8000000-0xa800000 sitting inside the guest's 2GB.
// That cost ~37MB and, worse, fragmented the window: The Sims 3 (not
// large-address-aware, so a 2GB ceiling) uses ~1.9GB and could no longer find
// a 196MB contiguous run. It retried forever, which is the loading-screen
// stall. Covering the whole window to 0x80000000 keeps the space contiguous.
//
// Released in reserve_low_guest_space(), then re-claimed PROT_NONE for the
// guest. Do not take the address of this symbol in code: __TEXT is at
// 0x400000000 and a RIP-relative 32-bit offset cannot reach it.
// Size must match kGuestReserveSize in early_low_reservation.hpp.

.globl _vitreo_guest_reserve
.zerofill __GUEST,__reserve,_vitreo_guest_reserve,0x7FF00000,12
