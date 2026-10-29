// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - x86_64 native backend: guest entry and exit stubs
//
// Three shared, thread-independent stubs. Thread-independent because every
// slot they touch is reached as fs:[offset], and the FS carrier holds the running
// thread's own TEB -- so one copy of this code serves every guest thread.
//
// Offsets MUST match core/cpu/x64/native64_layout.hpp. X64Native64Test
// .LayoutMatchesStubs checks the pairing.
//
// Nothing here may call libc: these run on an FS-carrier guest thread, where
// the host's TLS pointer no longer exists.

#define N64_RAX      0x1E00
#define N64_RCX      0x1E08
#define N64_RDX      0x1E10
#define N64_RBX      0x1E18
#define N64_RSP      0x1E20
#define N64_RBP      0x1E28
#define N64_RSI      0x1E30
#define N64_RDI      0x1E38
#define N64_R8       0x1E40
#define N64_R9       0x1E48
#define N64_R10      0x1E50
#define N64_R11      0x1E58
#define N64_R12      0x1E60
#define N64_R13      0x1E68
#define N64_R14      0x1E70
#define N64_R15      0x1E78
#define N64_RIP      0x1E80
#define N64_FLAGS    0x1E88
#define N64_HOSTRSP  0x1E90
#define N64_REASON   0x1E98
#define N64_FXSAVE   0x1C00
#define N64_REPAIR_RDI 0x1EC8
#define N64_REPAIR_RIP 0x1ED0

#define EXIT_SENTINEL 0
#define EXIT_THUNK    1
#define EXIT_FAULT    2

.text

// ---------------------------------------------------------------------------
// uint64_t native64_enter(uint16_t fs_selector)   [%di] -> reason in %rax
//
// Called from C++ on a guest thread. Reloads the FS TEB carrier, parks the host frame,
// loads the guest register file from the TEB block, and jumps to the guest.
//
// **The selector must be reloaded on every entry.** A GS base that came from
// an LDT descriptor does not survive a kernel transition: the kernel restores
// the standard selectors, so GS comes back as the null selector with base 0.
// It is not restored for us and there is no notification. compat32 has the
// same constraint and answers it the same way -- it calls set_fs_selector()
// once per run() rather than once per thread.
//
// Getting this wrong is silent and looks like a guest bug: with GS null the
// loads below read low memory instead of the TEB, the whole register file
// comes back zero, and the jump goes to address 0.
//
// Control comes back through _native64_collect or _native64_sentinel, both of
// which reload N64_HOSTRSP and `ret` -- so from the caller's point of view
// this is an ordinary call that returns once the guest stops.
// ---------------------------------------------------------------------------
.globl _native64_enter
.p2align 4
_native64_enter:
    // TEB carrier first: every access below depends on it.
    movw    %di, %fs

    // Preserve the host's callee-saved registers on the host stack. The guest
    // owns all sixteen while it runs, so nothing may be left live in one.
    pushq   %rbp
    pushq   %rbx
    pushq   %r12
    pushq   %r13
    pushq   %r14
    pushq   %r15

    // Resolve the TEB once through the architectural self pointer. If GS was
    // cleared, offset 0x30 is in the unmapped null page and the Mach bridge
    // repairs it. Once cached, a later preemption cannot redirect any of this
    // boundary's transfer accesses into mapped low memory.
    .globl _native64_enter_fs_probe
_native64_enter_fs_probe:
    movq    %fs:0x30, %r11
    testq   %r11, %r11
    jz      _native64_enter_fs_trap
    cmpq    %r11, 0x30(%r11)
    jne     _native64_enter_fs_trap
    .globl _native64_enter_fs_probe_end
_native64_enter_fs_probe_end:

    // Park the host stack pointer where every exit path can find it.
    movq    %rsp, N64_HOSTRSP(%r11)

    // Load the guest register file. RAX last: it carries the jump target.
    movq    N64_FLAGS(%r11), %rax
    pushq   %rax
    popfq

    movq    N64_RCX(%r11), %rcx
    movq    N64_RDX(%r11), %rdx
    movq    N64_RBX(%r11), %rbx
    movq    N64_RBP(%r11), %rbp
    movq    N64_RSI(%r11), %rsi
    movq    N64_RDI(%r11), %rdi
    movq    N64_R8(%r11),  %r8
    movq    N64_R9(%r11),  %r9
    movq    N64_R10(%r11), %r10
    movq    N64_R12(%r11), %r12
    movq    N64_R13(%r11), %r13
    movq    N64_R14(%r11), %r14
    movq    N64_R15(%r11), %r15

    // Restore the guest's x87/MMX/SSE file. XMM6-XMM15 are non-volatile in
    // the Win64 ABI, and the host C++ that just ran used them freely.
    fxrstor64 N64_FXSAVE(%r11)

    // Guest RSP last before the jump: after this the host stack is
    // unreachable except through N64_HOSTRSP.
    movq    N64_RSP(%r11), %rsp
    pushq   N64_RIP(%r11)
    movq    N64_RAX(%r11), %rax
    movq    N64_R11(%r11), %r11
    retq

.globl _native64_enter_fs_trap
_native64_enter_fs_trap:
    ud2

// ---------------------------------------------------------------------------
// _native64_collect -- guest called an imported API.
//
// Reached by the guest-side thunk stub `call *%fs:N64_COLLECT`, so the top of
// the guest stack holds the address just past that stub, which is the slot's
// dispatch gateway and therefore the thunk's identity.
//
// Captures the guest register file and returns to _native64_enter's caller
// with the guest stack exactly as the guest's own `call` left it -- the
// gateway address popped off -- so the shared dispatch path sees precisely
// what the interpreter would have produced.
// ---------------------------------------------------------------------------
.globl _native64_collect
.p2align 4
_native64_collect:
    // Preserve R11 and the guest flags, then validate and cache the TEB base.
    pushq   %r11
    pushfq
    .globl _native64_collect_fs_probe
_native64_collect_fs_probe:
    movq    %fs:0x30, %r11
    testq   %r11, %r11
    jz      _native64_collect_fs_trap
    cmpq    %r11, 0x30(%r11)
    jne     _native64_collect_fs_trap
    .globl _native64_collect_fs_probe_end
_native64_collect_fs_probe_end:
    movq    %rax, N64_RAX(%r11)
    movq    8(%rsp), %rax
    movq    %rax, N64_R11(%r11)

    // The validation changed flags, so publish the copy saved before it.
    popq    %rax
    movq    %rax, N64_FLAGS(%r11)

    // The gateway address the stub's call pushed: the thunk's identity.
    movq    8(%rsp), %rax
    movq    %rax, N64_RIP(%r11)

    // Undo that push, so the guest stack is as the guest's own call left it.
    leaq    16(%rsp), %rax
    movq    %rax, N64_RSP(%r11)

    movq    $EXIT_THUNK, %rax
    movq    %rax, N64_REASON(%r11)

    jmp     _native64_capture_rest

.globl _native64_collect_fs_trap
_native64_collect_fs_trap:
    ud2

// ---------------------------------------------------------------------------
// _native64_sentinel -- guest returned to the address pushed as its return.
//
// The guest's `ret` already popped the sentinel, so RSP is final as-is.
// ---------------------------------------------------------------------------
.globl _native64_sentinel
.p2align 4
_native64_sentinel:
    pushq   %r11
    pushfq
    .globl _native64_sentinel_fs_probe
_native64_sentinel_fs_probe:
    movq    %fs:0x30, %r11
    testq   %r11, %r11
    jz      _native64_sentinel_fs_trap
    cmpq    %r11, 0x30(%r11)
    jne     _native64_sentinel_fs_trap
    .globl _native64_sentinel_fs_probe_end
_native64_sentinel_fs_probe_end:
    movq    %rax, N64_RAX(%r11)
    movq    8(%rsp), %rax
    movq    %rax, N64_R11(%r11)

    popq    %rax
    movq    %rax, N64_FLAGS(%r11)

    leaq    8(%rsp), %rax
    movq    %rax, N64_RSP(%r11)

    // RIP is meaningless on this path; record the sentinel itself so a stale
    // value from the previous entry can never be mistaken for a live one.
    leaq    _native64_sentinel(%rip), %rax
    movq    %rax, N64_RIP(%r11)

    movq    $EXIT_SENTINEL, %rax
    movq    %rax, N64_REASON(%r11)

    // Explicit jump, NOT fall-through. Falling through to whatever text
    // happens to follow has now broken twice -- once on alignment padding the
    // assembler inserted, once on a stub added between the two. The edge is
    // spelled out so it cannot depend on layout.
    jmp     _native64_capture_rest

.globl _native64_sentinel_fs_trap
_native64_sentinel_fs_trap:
    ud2

// ---------------------------------------------------------------------------
// _native64_fault_return -- a faulting guest thread walks back out here.
//
// The Mach bridge has already written the faulting register file and the fault
// detail into the transfer block, so nothing is captured here; this only
// unwinds to the host frame.
//
// GS must be reloaded first. An exception can reset the segment registers, and
// every access below is GS-relative -- the bridge passes the selector in %di.
// ---------------------------------------------------------------------------
.globl _native64_fault_return
.p2align 4
_native64_fault_return:
    movw    %di, %fs
    movq    %fs:0x30, %r11
    movq    $EXIT_FAULT, %rax
    movq    %rax, N64_REASON(%r11)

    // Reason in RAX for the caller, as the shared tail does.
    movq    $EXIT_FAULT, %rax
    movq    N64_HOSTRSP(%r11), %rsp
    popq    %r15
    popq    %r14
    popq    %r13
    popq    %r12
    popq    %rbx
    popq    %rbp
    retq

// ---------------------------------------------------------------------------
// _native64_fs_repair -- resume a native64 thread whose FS carrier was cleared.
//
// The Mach bridge parks the interrupted RIP and RDI in the registered TEB,
// puts the LDT selector in RDI, and redirects here. Loading GS in the thread
// itself is required: thread_set_state accepts a selector but macOS does not
// reload its hidden descriptor base. MOV does. Neither MOV nor JMP changes
// flags, and RDI is restored before returning to the interrupted instruction,
// so the guest observes no state change.
// ---------------------------------------------------------------------------
.globl _native64_fs_repair
.p2align 4
_native64_fs_repair:
    movw    %di, %fs
    movq    %fs:0x30, %rdi
    movq    N64_REPAIR_RIP(%rdi), %rdi
    xchgq   %rdi, %fs:N64_REPAIR_RDI
    jmpq    *%fs:N64_REPAIR_RIP

// ---------------------------------------------------------------------------
// Shared tail: save the registers both exits treat identically, restore the
// host frame, and return into C++.
// ---------------------------------------------------------------------------
.globl _native64_capture_rest
_native64_capture_rest:
    // Capture the guest's FP/SSE file before any host code can touch it.
    fxsave64 N64_FXSAVE(%r11)

    movq    %rcx, N64_RCX(%r11)
    movq    %rdx, N64_RDX(%r11)
    movq    %rbx, N64_RBX(%r11)
    movq    %rbp, N64_RBP(%r11)
    movq    %rsi, N64_RSI(%r11)
    movq    %rdi, N64_RDI(%r11)
    movq    %r8,  N64_R8(%r11)
    movq    %r9,  N64_R9(%r11)
    movq    %r10, N64_R10(%r11)
    movq    %r12, N64_R12(%r11)
    movq    %r13, N64_R13(%r11)
    movq    %r14, N64_R14(%r11)
    movq    %r15, N64_R15(%r11)

    // Hand the exit reason back in RAX. C++ running on a guest thread must
    // never read it through GS: an LDT selector does not survive a kernel
    // transition, so a GS-relative load in ordinary compiled code faults at a
    // small absolute address the moment one happens. Inside a stub GS is
    // known-good, because entry reloaded it and nothing here traps.
    movq    N64_REASON(%r11), %rax

    // Back onto the host stack, then unwind exactly what _native64_enter
    // pushed.
    movq    N64_HOSTRSP(%r11), %rsp
    popq    %r15
    popq    %r14
    popq    %r13
    popq    %r12
    popq    %rbx
    popq    %rbp
    retq
