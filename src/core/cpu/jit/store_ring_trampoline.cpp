// SPDX-License-Identifier: LGPL-2.1-only
//
/// @file store_ring_trampoline.cpp
/// @brief Register-preserving recorder for VITREO_STORE_RING.
///
/// The JIT's linear and write-region fast paths store inline and never call a
/// helper, so a store ring fed only from the helpers misses precisely the
/// stores it exists to explain. Rerouting those stores through a helper is not
/// an option either: it changes which registers survive a store, and the
/// lowerings are written against the fast paths' much narrower clobber set.
///
/// So the recording call must be invisible. This trampoline saves every
/// volatile general register permitted by the Darwin PCS (x0-x17), all vector
/// registers, NZCV and the link register. X18 is the reserved platform register.
/// The recorder takes (addr, value, size) staged in x9/x10/x11. Restoring the
/// complete state leaves the normal memory lowering unchanged.

#include "core/memory/mem_watch.hpp"

namespace vitreo {

extern "C" void vitreo_store_ring_record(u64 addr, u64 value, u64 size, u64 rip) {
    store_ring_push(static_cast<GuestAddr>(addr), static_cast<usize>(size),
                    value, "jit.inline", rip);
}

/// A 128-bit store, recorded as its two halves. The +8 is done here rather than
/// in emitted code because every spare-looking ARM register at that point is
/// pinned (X12/X13 hold the ESP/EBP pins, X14 the linear base, X15 the ESP
/// frame), and clobbering one takes down the host, not the guest.
extern "C" void vitreo_store_ring_record16(u64 addr, u64 lo, u64 hi, u64 rip) {
    store_ring_push(static_cast<GuestAddr>(addr), 8, lo, "jit.vec.lo", rip);
    store_ring_push(static_cast<GuestAddr>(addr + 8), 8, hi, "jit.vec.hi", rip);
}

}  // namespace vitreo

// x9 = guest address, x10 = value, x11 = width in bytes, x16 = guest RIP.
// The 16-byte variant takes x9 = address, x10 = low half, x11 = high half.
__asm__(
    ".p2align 2\n"
    ".globl _vitreo_store_ring_trampoline\n"
    "_vitreo_store_ring_trampoline:\n"
    "    sub sp, sp, #704\n"
    "    stp x29, x30, [sp, #0]\n"
    "    mov x29, sp\n"
    "    stp x0,  x1,  [sp, #16]\n"
    "    stp x2,  x3,  [sp, #32]\n"
    "    stp x4,  x5,  [sp, #48]\n"
    "    stp x6,  x7,  [sp, #64]\n"
    "    stp x8,  x9,  [sp, #80]\n"
    "    stp x10, x11, [sp, #96]\n"
    "    stp x12, x13, [sp, #112]\n"
    "    stp x14, x15, [sp, #128]\n"
    "    stp x16, x17, [sp, #144]\n"
    "    stp q0, q1, [sp, #192]\n"
    "    stp q2, q3, [sp, #224]\n"
    "    stp q4, q5, [sp, #256]\n"
    "    stp q6, q7, [sp, #288]\n"
    "    stp q8, q9, [sp, #320]\n"
    "    stp q10, q11, [sp, #352]\n"
    "    stp q12, q13, [sp, #384]\n"
    "    stp q14, q15, [sp, #416]\n"
    "    stp q16, q17, [sp, #448]\n"
    "    stp q18, q19, [sp, #480]\n"
    "    stp q20, q21, [sp, #512]\n"
    "    stp q22, q23, [sp, #544]\n"
    "    stp q24, q25, [sp, #576]\n"
    "    stp q26, q27, [sp, #608]\n"
    "    stp q28, q29, [sp, #640]\n"
    "    stp q30, q31, [sp, #672]\n"
    "    mrs x17, nzcv\n"
    "    str x17, [sp, #160]\n"
    "    mov x0, x9\n"
    "    mov x1, x10\n"
    "    mov x2, x11\n"
    "    mov x3, x16\n"
    "    bl  _vitreo_store_ring_record\n"
    "    ldr x17, [sp, #160]\n"
    "    msr nzcv, x17\n"
    "    ldp q0, q1, [sp, #192]\n"
    "    ldp q2, q3, [sp, #224]\n"
    "    ldp q4, q5, [sp, #256]\n"
    "    ldp q6, q7, [sp, #288]\n"
    "    ldp q8, q9, [sp, #320]\n"
    "    ldp q10, q11, [sp, #352]\n"
    "    ldp q12, q13, [sp, #384]\n"
    "    ldp q14, q15, [sp, #416]\n"
    "    ldp q16, q17, [sp, #448]\n"
    "    ldp q18, q19, [sp, #480]\n"
    "    ldp q20, q21, [sp, #512]\n"
    "    ldp q22, q23, [sp, #544]\n"
    "    ldp q24, q25, [sp, #576]\n"
    "    ldp q26, q27, [sp, #608]\n"
    "    ldp q28, q29, [sp, #640]\n"
    "    ldp q30, q31, [sp, #672]\n"
    "    ldp x0,  x1,  [sp, #16]\n"
    "    ldp x2,  x3,  [sp, #32]\n"
    "    ldp x4,  x5,  [sp, #48]\n"
    "    ldp x6,  x7,  [sp, #64]\n"
    "    ldp x8,  x9,  [sp, #80]\n"
    "    ldp x10, x11, [sp, #96]\n"
    "    ldp x12, x13, [sp, #112]\n"
    "    ldp x14, x15, [sp, #128]\n"
    "    ldp x16, x17, [sp, #144]\n"
    "    ldp x29, x30, [sp, #0]\n"
    "    add sp, sp, #704\n"
    "    ret\n"
    ".p2align 2\n"
    ".globl _vitreo_store_ring_trampoline16\n"
    "_vitreo_store_ring_trampoline16:\n"
    "    sub sp, sp, #704\n"
    "    stp x29, x30, [sp, #0]\n"
    "    mov x29, sp\n"
    "    stp x0,  x1,  [sp, #16]\n"
    "    stp x2,  x3,  [sp, #32]\n"
    "    stp x4,  x5,  [sp, #48]\n"
    "    stp x6,  x7,  [sp, #64]\n"
    "    stp x8,  x9,  [sp, #80]\n"
    "    stp x10, x11, [sp, #96]\n"
    "    stp x12, x13, [sp, #112]\n"
    "    stp x14, x15, [sp, #128]\n"
    "    stp x16, x17, [sp, #144]\n"
    "    stp q0, q1, [sp, #192]\n"
    "    stp q2, q3, [sp, #224]\n"
    "    stp q4, q5, [sp, #256]\n"
    "    stp q6, q7, [sp, #288]\n"
    "    stp q8, q9, [sp, #320]\n"
    "    stp q10, q11, [sp, #352]\n"
    "    stp q12, q13, [sp, #384]\n"
    "    stp q14, q15, [sp, #416]\n"
    "    stp q16, q17, [sp, #448]\n"
    "    stp q18, q19, [sp, #480]\n"
    "    stp q20, q21, [sp, #512]\n"
    "    stp q22, q23, [sp, #544]\n"
    "    stp q24, q25, [sp, #576]\n"
    "    stp q26, q27, [sp, #608]\n"
    "    stp q28, q29, [sp, #640]\n"
    "    stp q30, q31, [sp, #672]\n"
    "    mrs x17, nzcv\n"
    "    str x17, [sp, #160]\n"
    "    mov x0, x9\n"
    "    mov x1, x10\n"
    "    mov x2, x11\n"
    "    mov x3, x16\n"
    "    bl  _vitreo_store_ring_record16\n"
    "    ldr x17, [sp, #160]\n"
    "    msr nzcv, x17\n"
    "    ldp q0, q1, [sp, #192]\n"
    "    ldp q2, q3, [sp, #224]\n"
    "    ldp q4, q5, [sp, #256]\n"
    "    ldp q6, q7, [sp, #288]\n"
    "    ldp q8, q9, [sp, #320]\n"
    "    ldp q10, q11, [sp, #352]\n"
    "    ldp q12, q13, [sp, #384]\n"
    "    ldp q14, q15, [sp, #416]\n"
    "    ldp q16, q17, [sp, #448]\n"
    "    ldp q18, q19, [sp, #480]\n"
    "    ldp q20, q21, [sp, #512]\n"
    "    ldp q22, q23, [sp, #544]\n"
    "    ldp q24, q25, [sp, #576]\n"
    "    ldp q26, q27, [sp, #608]\n"
    "    ldp q28, q29, [sp, #640]\n"
    "    ldp q30, q31, [sp, #672]\n"
    "    ldp x0,  x1,  [sp, #16]\n"
    "    ldp x2,  x3,  [sp, #32]\n"
    "    ldp x4,  x5,  [sp, #48]\n"
    "    ldp x6,  x7,  [sp, #64]\n"
    "    ldp x8,  x9,  [sp, #80]\n"
    "    ldp x10, x11, [sp, #96]\n"
    "    ldp x12, x13, [sp, #112]\n"
    "    ldp x14, x15, [sp, #128]\n"
    "    ldp x16, x17, [sp, #144]\n"
    "    ldp x29, x30, [sp, #0]\n"
    "    add sp, sp, #704\n"
    "    ret\n");
