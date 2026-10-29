// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo x86 oracle runner — executes raw x86 instruction bytes on the real
// CPU via Rosetta and dumps the resulting CPU state. Built for x86_64.
//
// Two modes, selected by OracleState.mode:
//   32 — the snippet is 32-bit-encoded and only the low 32 bits of GPR0..7 and
//        XMM0..7 are defined by the protocol. The runner zeroes the undefined
//        parts of the returned state so a 32-bit result is always a clean
//        zero-extension.
//   64 — the snippet is 64-bit-encoded. All 16 GPRs (full width), XMM0..15,
//        RFLAGS and RIP participate. The snippet is placed at a caller-chosen
//        fixed VA so RIP-relative operands are reproducible on the Vitreo side.
//
// RSP: if gpr[4] is 0 the host stack is left in place (legacy snippets).
// If gpr[4] is non-zero it is loaded as the live stack pointer in both
// 32- and 64-bit mode; the host RSP is parked and restored around the
// snippet. [esp+disp] then hits the scratch page on Rosetta the same
// way JIT already does. 32-bit PUSH in long mode is an 8-byte push.
//
// Wire protocol (stdin/stdout, native endian, packed):
//   in:  OracleState in_state
//        uint32_t   code_len
//        uint8_t    code[code_len]
//   out: OracleState out_state
//
// Status codes in OracleState.status:
//   0 = ok (snippet returned via RET)
//   1 = SIGSEGV
//   2 = SIGILL
//   3 = SIGFPE
//   4 = SIGBUS
//   5 = other signal
//   6 = setup error

#include "oracle_bench_protocol.h"

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <setjmp.h>
#include <sys/mman.h>
#include <unistd.h>
#include <errno.h>
#include <stddef.h>
#include <time.h>

// Fixed guest-visible VA for the per-call memory scratch page. Reachable
// with a 32-bit effective address (so `[EDI]` style ModR/M works in both
// 32-bit Vitreo and 64-bit Rosetta so long as upper bits of RDI are zero).
// The address must survive Rosetta's implicit low-memory reservations;
// empirically 0x04000000 (64 MiB) is the lowest page we can MAP_FIXED in a
// Rosetta subprocess that has inherited pipe stdin (lower pages are held by
// Rosetta's own runtime mappings once stdin is attached).
// Requires the binary to be linked with `-Wl,-pagezero_size,0x1000`.
#define ORACLE_MEM_VA     0x04000000u
#define ORACLE_MEM_SIZE   4096u
#define ORACLE_MEM_BLOB   512u  // FXSAVE image; page itself is ORACLE_MEM_SIZE

// Default VA of the snippet's first byte in 64-bit mode. Same reasoning as
// ORACLE_MEM_VA: high enough to dodge Rosetta's low reservations, low enough
// to stay trivially representable. The prologue is laid out immediately below
// it (in the preceding page) so execution falls through into the snippet.
#define ORACLE_CODE_VA    0x05000000ull

#define ORACLE_MODE32     32u
#define ORACLE_MODE64     64u

#pragma pack(push, 1)
typedef struct {
    // RAX,RCX,RDX,RBX,RSP,RBP,RSI,RDI,R8..R15 (full width).
    uint64_t gpr[16];
    uint64_t rflags;
    uint64_t rip;        // in: requested snippet VA (64-bit mode), 0 = default
                         // out: actual snippet VA
    uint32_t mxcsr;
    uint32_t mode;       // ORACLE_MODE32 or ORACLE_MODE64
    uint8_t  xmm[16][16];
    // First ORACLE_MEM_BLOB bytes of the memory scratch page (FXSAVE-sized).
    uint8_t  mem[ORACLE_MEM_BLOB];
    uint32_t mem_va;     // echoed back; the VA actually mapped.
    uint32_t status;     // 0 = ok, see codes above
    uint32_t bytes_run;  // 0 if signal hit, else code_len
    uint32_t reserved;
    // x87/MMX file, in FXSAVE legacy-image terms: st[] is TOP-RELATIVE
    // (ST(0)..ST(7)) and ftw is the ABRIDGED 8-bit tag. MMx aliases the low 8
    // bytes of st[x]. Must mirror oracle_client.hpp exactly.
    uint16_t fcw;
    uint16_t fsw;
    uint8_t  ftw;
    uint8_t  fx_pad[3];
    uint8_t  st[8][10];
} OracleState;
#pragma pack(pop)

#define OFF_GPR(i)   ((uint32_t)(offsetof(OracleState, gpr) + 8u * (uint32_t)(i)))
#define OFF_RFLAGS   ((uint32_t)offsetof(OracleState, rflags))
#define OFF_MXCSR    ((uint32_t)offsetof(OracleState, mxcsr))
#define OFF_XMM(i)   ((uint32_t)(offsetof(OracleState, xmm) + 16u * (uint32_t)(i)))
#define OFF_STATUS   ((uint32_t)offsetof(OracleState, status))

_Static_assert(sizeof(OracleState) == 1024, "OracleState wire size");
_Static_assert(offsetof(OracleState, fcw) == 936, "fcw offset");
_Static_assert(offsetof(OracleState, st) == 944, "st offset");
_Static_assert(OFF_RFLAGS == 128, "rflags offset");
_Static_assert(OFF_MXCSR == 144, "mxcsr offset");
_Static_assert(OFF_XMM(0) == 152, "xmm offset");
_Static_assert(OFF_STATUS == 924, "status offset");

#define ORACLE_OK_SENTINEL 0xDEADBEEFu

static sigjmp_buf g_jb;
static volatile sig_atomic_t g_signal_status = 0;
static OracleState* g_st_ptr = NULL;
static uint32_t g_mem_va = ORACLE_MEM_VA;
static int g_bench_mode = 0;
// Scratch slots the epilogue uses to park the guest's RAX and the host/guest
// stack pointers. Deliberately outside the compared state.
static uint64_t g_rax_save = 0;
static uint64_t g_host_rsp = 0;
static uint64_t g_guest_rsp = 0;

// ============================================================================
// Runtime x86-64 encoder for the prologue/epilogue thunks
// ============================================================================
//
// Both thunks address the state through RAX as base, using `mod=10` ModR/M
// with a disp32 (rm=000 selects RAX and needs no SIB). The state pointer is
// baked in as an imm64 because the thunk is assembled per call.
//
// Register load order matters:
//   * RAX is loaded last (it is the base register while the others load).
//   * RFLAGS is loaded via `push qword [rax+disp32]` / `popfq`, which needs no
//     scratch GPR — important in 64-bit mode where every GPR is caller state.
// Store order matters symmetrically:
//   * RAX is parked at an absolute address with `mov [moffs64], rax` (opcode
//     A3), the only store that needs no other register.
//   * RFLAGS is captured before any flag-writing instruction; MOV/MOVABS do
//     not write flags, so the RAX park + state-pointer reload are safe.

static uint8_t* e8(uint8_t* p, uint8_t v) { *p++ = v; return p; }
static uint8_t* e32(uint8_t* p, uint32_t v) { memcpy(p, &v, 4); return p + 4; }
static uint8_t* e64(uint8_t* p, uint64_t v) { memcpy(p, &v, 8); return p + 8; }

// mov rax, imm64
static uint8_t* emit_movabs_rax(uint8_t* p, uint64_t v) {
    p = e8(p, 0x48); p = e8(p, 0xB8); return e64(p, v);
}
// mov rcx, imm64
static uint8_t* emit_movabs_rcx(uint8_t* p, uint64_t v) {
    p = e8(p, 0x48); p = e8(p, 0xB9); return e64(p, v);
}
// mov [imm64], rax
static uint8_t* emit_mov_moffs_rax(uint8_t* p, uint64_t v) {
    p = e8(p, 0x48); p = e8(p, 0xA3); return e64(p, v);
}
// mov rcx, [rcx]
static uint8_t* emit_mov_rcx_from_rcx(uint8_t* p) {
    p = e8(p, 0x48); p = e8(p, 0x8B); return e8(p, 0x09);
}
// mov reg, [rax+disp32]  (w: 1 = 64-bit, 0 = 32-bit)
static uint8_t* emit_ld_gpr(uint8_t* p, int reg, uint32_t disp, int w) {
    uint8_t rex = (uint8_t)((w ? 0x48 : 0x40) | ((reg >> 3) << 2));
    if (rex != 0x40) p = e8(p, rex);
    p = e8(p, 0x8B);
    p = e8(p, (uint8_t)(0x80 | ((reg & 7) << 3)));
    return e32(p, disp);
}
// mov [rax+disp32], reg
static uint8_t* emit_st_gpr(uint8_t* p, int reg, uint32_t disp, int w) {
    uint8_t rex = (uint8_t)((w ? 0x48 : 0x40) | ((reg >> 3) << 2));
    if (rex != 0x40) p = e8(p, rex);
    p = e8(p, 0x89);
    p = e8(p, (uint8_t)(0x80 | ((reg & 7) << 3)));
    return e32(p, disp);
}
// movdqu xmm, [rax+disp32]
static uint8_t* emit_ld_xmm(uint8_t* p, int reg, uint32_t disp) {
    p = e8(p, 0xF3);
    uint8_t rex = (uint8_t)(0x40 | ((reg >> 3) << 2));
    if (rex != 0x40) p = e8(p, rex);
    p = e8(p, 0x0F); p = e8(p, 0x6F);
    p = e8(p, (uint8_t)(0x80 | ((reg & 7) << 3)));
    return e32(p, disp);
}
// movdqu [rax+disp32], xmm
static uint8_t* emit_st_xmm(uint8_t* p, int reg, uint32_t disp) {
    p = e8(p, 0xF3);
    uint8_t rex = (uint8_t)(0x40 | ((reg >> 3) << 2));
    if (rex != 0x40) p = e8(p, rex);
    p = e8(p, 0x0F); p = e8(p, 0x7F);
    p = e8(p, (uint8_t)(0x80 | ((reg & 7) << 3)));
    return e32(p, disp);
}
// ldmxcsr [rax+disp32] (0F AE /2) / stmxcsr [rax+disp32] (0F AE /3)
static uint8_t* emit_mxcsr(uint8_t* p, int store, uint32_t disp) {
    p = e8(p, 0x0F); p = e8(p, 0xAE);
    p = e8(p, (uint8_t)(0x80 | ((store ? 3 : 2) << 3)));
    return e32(p, disp);
}
// push qword [rax+disp32] (FF /6)
static uint8_t* emit_push_mem(uint8_t* p, uint32_t disp) {
    p = e8(p, 0xFF); p = e8(p, (uint8_t)(0x80 | (6 << 3)));
    return e32(p, disp);
}
// mov dword [rax+disp32], imm32 (C7 /0)
static uint8_t* emit_st_imm32(uint8_t* p, uint32_t disp, uint32_t imm) {
    p = e8(p, 0xC7); p = e8(p, 0x80);
    p = e32(p, disp);
    return e32(p, imm);
}

// Register indices skipped by the bulk GPR loop: 0 (RAX, handled last) and
// 4 (RSP, switched only when the incoming gpr[4] is non-zero).
static int reg_is_bulk(int r) { return r != 0 && r != 4; }

// mov [rax], rsp / mov rsp, [rax]
static uint8_t* emit_st_rsp_via_rax(uint8_t* p) {
    p = e8(p, 0x48); p = e8(p, 0x89); return e8(p, 0x20);
}
static uint8_t* emit_ld_rsp_via_rax(uint8_t* p) {
    p = e8(p, 0x48); p = e8(p, 0x8B); return e8(p, 0x20);
}

// The thunk loads guest values into registers the SysV ABI makes callee-saved
// (RBX, RBP, R12-R15) and never restores them from the guest's perspective, so
// it must bracket itself with a save/restore or it returns to C with a
// corrupted frame pointer and clobbered spill registers. Host RSP is parked
// in g_host_rsp so a live-guest-RSP snippet cannot smash that frame.
static const uint8_t k_push_callee_saved[] = {
    0x55,             // push rbp
    0x53,             // push rbx
    0x41, 0x54,       // push r12
    0x41, 0x55,       // push r13
    0x41, 0x56,       // push r14
    0x41, 0x57,       // push r15
};
static const uint8_t k_pop_callee_saved[] = {
    0x41, 0x5F,       // pop r15
    0x41, 0x5E,       // pop r14
    0x41, 0x5D,       // pop r13
    0x41, 0x5C,       // pop r12
    0x5B,             // pop rbx
    0x5D,             // pop rbp
};

// ---------------------------------------------------------------------------
// x87/MMX transfer via the FXSAVE legacy image.
//
// The wire State carries FCW/FSW/FTW + ST(0..7) in FXSAVE terms, so the
// snippet runs with a CALLER-CHOSEN x87/MMX file instead of whatever FNINIT
// happened to leave behind. Without this, any instruction reading ST/MM as an
// input compares two different starting states and the result is noise.
// ---------------------------------------------------------------------------
static uint8_t g_fx_in[512]  __attribute__((aligned(16)));
static uint8_t g_fx_out[512] __attribute__((aligned(16)));

static void fx_build_in(const OracleState* st) {
    memset(g_fx_in, 0, sizeof g_fx_in);
    memcpy(g_fx_in + 0, &st->fcw, 2);
    memcpy(g_fx_in + 2, &st->fsw, 2);
    g_fx_in[4] = st->ftw;                     // abridged tag
    // MXCSR must be architecturally valid or FXRSTOR raises #GP. The prologue
    // loads the real MXCSR right after, so the default is fine here.
    uint32_t mxcsr = 0x1F80u, mxcsr_mask = 0xFFFFu;
    memcpy(g_fx_in + 24, &mxcsr, 4);
    memcpy(g_fx_in + 28, &mxcsr_mask, 4);
    for (int i = 0; i < 8; ++i) memcpy(g_fx_in + 32 + i * 16, st->st[i], 10);
    for (int i = 0; i < 16; ++i) memcpy(g_fx_in + 160 + i * 16, st->xmm[i], 16);
}

static void fx_extract_out(OracleState* st) {
    memcpy(&st->fcw, g_fx_out + 0, 2);
    memcpy(&st->fsw, g_fx_out + 2, 2);
    st->ftw = g_fx_out[4];
    for (int i = 0; i < 8; ++i) memcpy(st->st[i], g_fx_out + 32 + i * 16, 10);
}

static uint8_t* emit_prologue(uint8_t* p, uint64_t state, uint32_t mode,
                              int rsp_live) {
    const int w = (mode == ORACLE_MODE64) ? 1 : 0;
    const int ngpr = (mode == ORACLE_MODE64) ? 16 : 8;
    const int nxmm = (mode == ORACLE_MODE64) ? 16 : 8;

    memcpy(p, k_push_callee_saved, sizeof(k_push_callee_saved));
    p += sizeof(k_push_callee_saved);
    p = emit_movabs_rax(p, (uint64_t)(uintptr_t)&g_host_rsp);
    p = emit_st_rsp_via_rax(p);
    p = emit_movabs_rax(p, state);
    // Load the caller's x87/MMX file. RCX is free here (GPRs load further
    // down), and this supersedes the old blanket FNINIT: a State that wants
    // FNINIT semantics simply sends fcw=0x037F, fsw=0, ftw=0.
    p = emit_movabs_rcx(p, (uint64_t)(uintptr_t)g_fx_in);
    p = e8(p, 0x0F); p = e8(p, 0xAE); p = e8(p, 0x09);   // fxrstor [rcx]
    p = emit_mxcsr(p, 0, OFF_MXCSR);
    for (int r = 0; r < nxmm; ++r) p = emit_ld_xmm(p, r, OFF_XMM(r));
    // Oracle State has no ymm_high. Legacy MOVDQU preserves dirty YMM[255:128]
    // on AVX hosts; VZEROUPPER makes the host match the interpreter's zero
    // high halves so 256-bit leftovers compare on the visible XMM lanes.
    p = e8(p, 0xC5); p = e8(p, 0xF8); p = e8(p, 0x77);  // vzeroupper
    if (mode != ORACLE_MODE64) {
        // 32-bit protocol: XMM8-15 are undefined. Zero the host registers so
        // a 64-bit-mode FXSAVE (Rosetta always runs long mode) does not leak
        // leftover XMM8-15 into the 512-byte image.
        for (int r = 8; r < 16; ++r) {
            p = e8(p, 0x66);
            p = e8(p, 0x45);  // REX.R+B
            p = e8(p, 0x0F); p = e8(p, 0xEF);
            p = e8(p, (uint8_t)(0xC0 | ((r & 7) << 3) | (r & 7)));
        }
    }
    for (int r = 0; r < ngpr; ++r) {
        if (!reg_is_bulk(r)) continue;
        p = emit_ld_gpr(p, r, OFF_GPR(r), w);
    }
    p = emit_push_mem(p, OFF_RFLAGS);
    p = e8(p, 0x9D);                        // popfq
    if (rsp_live) {
        // Host RSP is 64-bit even for 32-bit snippets. Loading the
        // zero-extended 32-bit ESP makes [esp+disp] hit the scratch
        // page on both JIT and Rosetta (dump replay). 32-bit PUSH
        // in long mode is an 8-byte push — dump prepare_body rejects
        // PUSH/POP so those bodies never take this path.
        p = emit_ld_gpr(p, 4, OFF_GPR(4), 1);
    }
    p = emit_ld_gpr(p, 0, OFF_GPR(0), w);   // RAX last
    return p;
}

static uint8_t* emit_epilogue(uint8_t* p, uint64_t state, uint32_t mode) {
    const int w = (mode == ORACLE_MODE64) ? 1 : 0;
    const int ngpr = (mode == ORACLE_MODE64) ? 16 : 8;
    const int nxmm = (mode == ORACLE_MODE64) ? 16 : 8;

    p = emit_mov_moffs_rax(p, (uint64_t)(uintptr_t)&g_rax_save);
    p = emit_movabs_rax(p, (uint64_t)(uintptr_t)&g_guest_rsp);
    p = emit_st_rsp_via_rax(p);
    p = emit_movabs_rax(p, (uint64_t)(uintptr_t)&g_host_rsp);
    p = emit_ld_rsp_via_rax(p);
    p = emit_movabs_rax(p, state);
    p = emit_st_gpr(p, 1, OFF_GPR(1), w);   // RCX out of the way first
    // Flags: nothing above this point writes them.
    p = e8(p, 0x9C);                        // pushfq
    p = e8(p, 0x59);                        // pop rcx
    p = emit_st_gpr(p, 1, OFF_RFLAGS, w);
    // Capture the x87/MMX file. FXSAVE does not modify EFLAGS, so placing it
    // after the flags read keeps "nothing above writes flags" true.
    p = emit_movabs_rcx(p, (uint64_t)(uintptr_t)g_fx_out);
    p = e8(p, 0x0F); p = e8(p, 0xAE); p = e8(p, 0x01);   // fxsave [rcx]
    for (int r = 2; r < ngpr; ++r) {
        if (!reg_is_bulk(r)) continue;
        p = emit_st_gpr(p, r, OFF_GPR(r), w);
    }
    // Recover the guest's RAX through RCX (already saved).
    p = emit_movabs_rcx(p, (uint64_t)(uintptr_t)&g_rax_save);
    p = emit_mov_rcx_from_rcx(p);
    p = emit_st_gpr(p, 1, OFF_GPR(0), w);
    p = emit_movabs_rcx(p, (uint64_t)(uintptr_t)&g_guest_rsp);
    p = emit_mov_rcx_from_rcx(p);
    p = emit_st_gpr(p, 1, OFF_GPR(4), 1);  // guest RSP, full width
    p = emit_mxcsr(p, 1, OFF_MXCSR);
    for (int r = 0; r < nxmm; ++r) p = emit_st_xmm(p, r, OFF_XMM(r));
    p = emit_st_imm32(p, OFF_STATUS, ORACLE_OK_SENTINEL);
    memcpy(p, k_pop_callee_saved, sizeof(k_pop_callee_saved));
    p += sizeof(k_pop_callee_saved);
    p = e8(p, 0xC3);                        // ret
    return p;
}

// ============================================================================

static int write_full(int fd, const void* buf, size_t n);

// Rosetta occasionally delivers a synthetic signal right after our snippet
// returns (during RET or just after). The epilogue writes ORACLE_OK_SENTINEL
// into OracleState.status as its penultimate step, so if the handler sees the
// sentinel already present we know the guest snippet ran to completion and
// treat the signal as benign.
static void normalize_out(OracleState* st);
static void on_signal(int sig) {
    if (!g_bench_mode && g_st_ptr && g_st_ptr->status == ORACLE_OK_SENTINEL) {
        g_st_ptr->status = 0;
        // Snapshot the scratch page before writing back; the normal main-line
        // path does this after fn() returns, but the benign-signal fast path
        // bypasses that. Without this, any snippet that stores to the scratch
        // page emits a state with the caller's *input* mem[] bytes, which
        // looks exactly like a real divergence to the fuzz comparator.
        memcpy(g_st_ptr->mem, (void*)(uintptr_t)g_mem_va, ORACLE_MEM_BLOB);
        // We got here because the epilogue completed, so bytes_run should be
        // the full snippet length. The main path sets this from code_len, but
        // this handler doesn't have it in scope — leave as 0 and document
        // that callers treat status==0 as authoritative.
        normalize_out(g_st_ptr);
        write_full(1, g_st_ptr, sizeof(*g_st_ptr));
        _exit(0);
    }
    switch (sig) {
        case SIGSEGV: g_signal_status = 1; break;
        case SIGILL:  g_signal_status = 2; break;
        case SIGFPE:  g_signal_status = 3; break;
        case SIGBUS:  g_signal_status = 4; break;
        default:      g_signal_status = 5; break;
    }
    siglongjmp(g_jb, 1);
}

// In 32-bit mode the thunks only touch the low halves of GPR0..7, XMM0..7 and
// EFLAGS. Zero everything the protocol leaves undefined so a 32-bit result is
// a clean zero-extension of the historical 8-register view instead of leaking
// the caller's input bytes back as if they were architectural output.
static void normalize_out(OracleState* st) {
    if (st->mode == ORACLE_MODE64) return;
    for (int i = 0; i < 8; ++i) st->gpr[i] &= 0xFFFFFFFFull;
    for (int i = 8; i < 16; ++i) st->gpr[i] = 0;
    st->rflags &= 0xFFFFFFFFull;
    for (int r = 8; r < 16; ++r) memset(st->xmm[r], 0, 16);
}

static int read_full(int fd, void* buf, size_t n) {
    uint8_t* p = (uint8_t*)buf;
    while (n) {
        ssize_t r = read(fd, p, n);
        if (r <= 0) return -1;
        p += r; n -= (size_t)r;
    }
    return 0;
}

static int write_full(int fd, const void* buf, size_t n) {
    const uint8_t* p = (const uint8_t*)buf;
    while (n) {
        ssize_t r = write(fd, p, n);
        if (r <= 0) return -1;
        p += r; n -= (size_t)r;
    }
    return 0;
}

static uint64_t monotonic_ns(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static uint32_t run_mapped_snippet(OracleState* st, void (*fn)(void),
                                   uint32_t code_len) {
    g_st_ptr = st;
    g_signal_status = 0;
    st->status = 0xBADBAD00u;
    st->bytes_run = 0;
    // Stage the caller's x87/MMX file for the prologue's FXRSTOR, and clear
    // the output image so a faulting snippet cannot leave a previous trial's
    // registers looking like this one's result.
    fx_build_in(st);
    memset(g_fx_out, 0, sizeof g_fx_out);
    if (sigsetjmp(g_jb, 1) == 0) {
        fn();
        if (st->status == ORACLE_OK_SENTINEL) {
            st->status = 0;
            st->bytes_run = code_len;
        } else {
            st->status = 6;
            st->bytes_run = 0;
        }
    } else {
        if (st->status == ORACLE_OK_SENTINEL) {
            st->status = 0;
            st->bytes_run = code_len;
        } else {
            st->status = (uint32_t)g_signal_status;
            st->bytes_run = 0;
        }
    }
    memcpy(st->mem, (void*)(uintptr_t)g_mem_va, ORACLE_MEM_BLOB);
    // Only trust the epilogue's FXSAVE when the epilogue actually ran; on a
    // fault the image is the zeroed staging buffer, not stale registers.
    if (st->status == 0) fx_extract_out(st);
    normalize_out(st);
    return st->status;
}

static int bench_main(void) {
    g_bench_mode = 1;

    OracleState initial;
    OracleState st;
    uint8_t code[4096];
    uint32_t code_len = 0;
    int loaded = 0;
    void (*fn)(void) = NULL;
    OracleBenchTimes times;
    memset(&times, 0, sizeof(times));
    memset(&initial, 0, sizeof(initial));
    memset(&st, 0, sizeof(st));

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_NODEFER;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGILL,  &sa, NULL);
    sigaction(SIGFPE,  &sa, NULL);
    sigaction(SIGBUS,  &sa, NULL);

    for (;;) {
        OracleBenchCmd cmd;
        if (read_full(0, &cmd, sizeof(cmd)) < 0) return 1;

        if (cmd.cmd == ORACLE_BENCH_CMD_QUIT) {
            uint32_t ack = 0;
            (void)write_full(1, &ack, sizeof(ack));
            return 0;
        }

        if (cmd.cmd == ORACLE_BENCH_CMD_UNLOAD) {
            loaded = 0;
            fn = NULL;
            uint32_t ack = 0;
            if (write_full(1, &ack, sizeof(ack)) < 0) return 1;
            continue;
        }

        if (cmd.cmd == ORACLE_BENCH_CMD_TIMES) {
            if (write_full(1, &times, sizeof(times)) < 0) return 1;
            continue;
        }

        if (cmd.cmd == ORACLE_BENCH_CMD_LOAD) {
            if (read_full(0, &st, sizeof(st)) < 0) return 1;
            if (read_full(0, &code_len, 4) < 0) return 1;
            if (code_len > 4096) {
                uint32_t status = 6;
                if (write_full(1, &status, sizeof(status)) < 0) return 1;
                continue;
            }
            if (code_len && read_full(0, code, code_len) < 0) return 1;
            if (st.mode != ORACLE_MODE32 && st.mode != ORACLE_MODE64) {
                uint32_t status = 6;
                if (write_full(1, &status, sizeof(status)) < 0) return 1;
                continue;
            }

            static const uint32_t kCandidates[] = {
                0x04000000u, 0x08000000u, 0x10000000u, 0x20000000u,
                0x40000000u, 0x50000000u, 0x60000000u, 0x70000000u,
            };
            void* mem_page = MAP_FAILED;
            uint32_t chosen_va = 0;
            uint32_t first_try = st.mem_va ? st.mem_va : kCandidates[0];
            mem_page = mmap((void*)(uintptr_t)first_try, ORACLE_MEM_SIZE,
                            PROT_READ|PROT_WRITE,
                            MAP_PRIVATE|MAP_ANON|MAP_FIXED, -1, 0);
            if (mem_page != MAP_FAILED && mem_page == (void*)(uintptr_t)first_try) {
                chosen_va = first_try;
            } else {
                for (size_t i = 0; i < sizeof(kCandidates)/sizeof(kCandidates[0]); ++i) {
                    if (kCandidates[i] == first_try) continue;
                    mem_page = mmap((void*)(uintptr_t)kCandidates[i], ORACLE_MEM_SIZE,
                                    PROT_READ|PROT_WRITE,
                                    MAP_PRIVATE|MAP_ANON|MAP_FIXED, -1, 0);
                    if (mem_page != MAP_FAILED &&
                        mem_page == (void*)(uintptr_t)kCandidates[i]) {
                        chosen_va = kCandidates[i];
                        break;
                    }
                    mem_page = MAP_FAILED;
                }
            }
            uint32_t status = 6;
            if (mem_page == MAP_FAILED || chosen_va == 0) {
                if (write_full(1, &status, sizeof(status)) < 0) return 1;
                continue;
            }
            memcpy(mem_page, st.mem, ORACLE_MEM_BLOB);
            st.mem_va = chosen_va;
            g_mem_va = chosen_va;

            uint8_t pro[512], epi[512];
            size_t pro_len = (size_t)(emit_prologue(pro, (uint64_t)(uintptr_t)&st, st.mode,
                st.gpr[4] != 0) - pro);
            size_t epi_len = (size_t)(emit_epilogue(epi, (uint64_t)(uintptr_t)&st, st.mode) - epi);

            uint8_t* snippet_ptr = NULL;
            if (st.mode == ORACLE_MODE64) {
                uint64_t want = st.rip ? st.rip : ORACLE_CODE_VA;
                if ((want & 0xFFFull) != 0 || pro_len > 4096 ||
                    (size_t)code_len + epi_len > 4096) {
                    if (write_full(1, &status, sizeof(status)) < 0) return 1;
                    continue;
                }
                void* base = mmap((void*)(uintptr_t)(want - 4096), 8192,
                                  PROT_READ|PROT_WRITE|PROT_EXEC,
                                  MAP_PRIVATE|MAP_ANON|MAP_FIXED, -1, 0);
                if (base == MAP_FAILED || base != (void*)(uintptr_t)(want - 4096)) {
                    if (write_full(1, &status, sizeof(status)) < 0) return 1;
                    continue;
                }
                snippet_ptr = (uint8_t*)(uintptr_t)want;
                memcpy(snippet_ptr - pro_len, pro, pro_len);
                st.rip = want;
            } else {
                size_t total = pro_len + code_len + epi_len;
                if (total > 4096) {
                    if (write_full(1, &status, sizeof(status)) < 0) return 1;
                    continue;
                }
                void* page = mmap(NULL, 4096, PROT_READ|PROT_WRITE|PROT_EXEC,
                                  MAP_PRIVATE|MAP_ANON, -1, 0);
                if (page == MAP_FAILED) {
                    if (write_full(1, &status, sizeof(status)) < 0) return 1;
                    continue;
                }
                memcpy(page, pro, pro_len);
                snippet_ptr = (uint8_t*)page + pro_len;
                st.rip = (uint64_t)(uintptr_t)snippet_ptr;
            }
            if (code_len) memcpy(snippet_ptr, code, code_len);
            memcpy(snippet_ptr + code_len, epi, epi_len);
            fn = (void (*)(void))(void*)(snippet_ptr - pro_len);
            initial = st;
            loaded = 1;
            memset(&times, 0, sizeof(times));
            status = 0;
            if (write_full(1, &status, sizeof(status)) < 0) return 1;
            continue;
        }

        if (!loaded || !fn) {
            uint32_t status = 6;
            if (cmd.cmd == ORACLE_BENCH_CMD_ONCE) {
                OracleState fail = initial;
                fail.status = 6;
                if (write_full(1, &fail, sizeof(fail)) < 0) return 1;
            } else {
                if (write_full(1, &status, sizeof(status)) < 0) return 1;
            }
            continue;
        }

        if (cmd.cmd == ORACLE_BENCH_CMD_ONCE) {
            st = initial;
            memcpy((void*)(uintptr_t)g_mem_va, st.mem, ORACLE_MEM_BLOB);
            (void)run_mapped_snippet(&st, fn, code_len);
            if (write_full(1, &st, sizeof(st)) < 0) return 1;
            continue;
        }

        if (cmd.cmd == ORACLE_BENCH_CMD_WARMUP || cmd.cmd == ORACLE_BENCH_CMD_RUN) {
            uint32_t n = cmd.n;
            uint32_t status = 0;
            uint64_t t0 = monotonic_ns();
            for (uint32_t i = 0; i < n; ++i) {
                st = initial;
                memcpy((void*)(uintptr_t)g_mem_va, st.mem, ORACLE_MEM_BLOB);
                status = run_mapped_snippet(&st, fn, code_len);
                if (status != 0) break;
            }
            uint64_t dt = monotonic_ns() - t0;
            if (cmd.cmd == ORACLE_BENCH_CMD_WARMUP) {
                times.warmup_ns = dt;
            } else {
                times.run_ns = dt;
                times.iters = n;
            }
            times.status = status;
            if (write_full(1, &status, sizeof(status)) < 0) return 1;
            continue;
        }

        uint32_t status = 6;
        if (write_full(1, &status, sizeof(status)) < 0) return 1;
    }
}

static int oneshot_main(void) {
    OracleState st;
    if (read_full(0, &st, sizeof(st)) < 0) return 1;

    uint32_t code_len = 0;
    if (read_full(0, &code_len, 4) < 0) return 1;
    if (code_len > 4096) { st.status = 6; goto out; }

    uint8_t code[4096];
    if (code_len && read_full(0, code, code_len) < 0) return 1;

    if (st.mode != ORACLE_MODE32 && st.mode != ORACLE_MODE64) {
        fprintf(stderr, "oracle: bad mode %u\n", st.mode);
        st.status = 6; goto out;
    }

    // Map the scratch memory page at a guest VA reachable from a
    // zero-extended 32-bit effective address. Under Rosetta the usable
    // low-4GB layout varies run-to-run due to dyld ASLR, so we try a
    // small cascade of fixed addresses. Input `st.mem_va` may request a
    // specific VA (non-zero); if that fails, we fall back. The chosen
    // VA is written to st.mem_va in the output so callers can cache it.
    static const uint32_t kCandidates[] = {
        0x04000000u, 0x08000000u, 0x10000000u, 0x20000000u,
        0x40000000u, 0x50000000u, 0x60000000u, 0x70000000u,
    };
    void* mem_page = MAP_FAILED;
    uint32_t chosen_va = 0;
    // If caller passed a specific mem_va, try it first.
    uint32_t first_try = st.mem_va ? st.mem_va : kCandidates[0];
    mem_page = mmap((void*)(uintptr_t)first_try, ORACLE_MEM_SIZE,
                    PROT_READ|PROT_WRITE,
                    MAP_PRIVATE|MAP_ANON|MAP_FIXED, -1, 0);
    if (mem_page != MAP_FAILED && mem_page == (void*)(uintptr_t)first_try) {
        chosen_va = first_try;
    } else {
        for (size_t i = 0; i < sizeof(kCandidates)/sizeof(kCandidates[0]); ++i) {
            if (kCandidates[i] == first_try) continue;
            mem_page = mmap((void*)(uintptr_t)kCandidates[i], ORACLE_MEM_SIZE,
                            PROT_READ|PROT_WRITE,
                            MAP_PRIVATE|MAP_ANON|MAP_FIXED, -1, 0);
            if (mem_page != MAP_FAILED && mem_page == (void*)(uintptr_t)kCandidates[i]) {
                chosen_va = kCandidates[i];
                break;
            }
            mem_page = MAP_FAILED;
        }
    }
    if (mem_page == MAP_FAILED || chosen_va == 0) {
        fprintf(stderr, "oracle: no candidate VA mappable (errno=%d)\n", errno);
        st.status = 6;
        goto out;
    }
    memcpy(mem_page, st.mem, ORACLE_MEM_BLOB);
    st.mem_va = chosen_va;
    g_mem_va = chosen_va;

    // Assemble the thunks first so we know how much room the prologue needs.
    uint8_t pro[512], epi[512];
    // The state pointer is baked in as an imm64, so encode against &st.
    size_t pro_len = (size_t)(emit_prologue(pro, (uint64_t)(uintptr_t)&st, st.mode,
        st.gpr[4] != 0) - pro);
    size_t epi_len = (size_t)(emit_epilogue(epi, (uint64_t)(uintptr_t)&st, st.mode) - epi);

    // Executable window. In 64-bit mode the snippet must start at a known VA
    // so RIP-relative operands are reproducible: reserve two pages ending one
    // page above the requested VA and lay the prologue immediately below the
    // snippet so it falls through. 32-bit mode has no RIP-relative addressing,
    // so an anonymous mapping is fine (and cannot fail on a busy VA).
    uint64_t snippet_va = 0;
    uint8_t* snippet_ptr = NULL;
    if (st.mode == ORACLE_MODE64) {
        uint64_t want = st.rip ? st.rip : ORACLE_CODE_VA;
        if ((want & 0xFFFull) != 0) {
            fprintf(stderr, "oracle: code VA %llx not page-aligned\n",
                    (unsigned long long)want);
            st.status = 6; goto out;
        }
        if (pro_len > 4096 || (size_t)code_len + epi_len > 4096) {
            fprintf(stderr, "oracle: snippet too large\n"); st.status = 6; goto out;
        }
        void* base = mmap((void*)(uintptr_t)(want - 4096), 8192,
                          PROT_READ|PROT_WRITE|PROT_EXEC,
                          MAP_PRIVATE|MAP_ANON|MAP_FIXED, -1, 0);
        if (base == MAP_FAILED || base != (void*)(uintptr_t)(want - 4096)) {
            fprintf(stderr, "oracle: code VA %llx unmappable (errno=%d)\n",
                    (unsigned long long)want, errno);
            st.status = 6; goto out;
        }
        snippet_va = want;
        snippet_ptr = (uint8_t*)(uintptr_t)want;
        memcpy(snippet_ptr - pro_len, pro, pro_len);
    } else {
        size_t total = pro_len + code_len + epi_len;
        if (total > 4096) { fprintf(stderr, "oracle: snippet too large (%zu)\n", total); st.status = 6; goto out; }
        void* page = mmap(NULL, 4096, PROT_READ|PROT_WRITE|PROT_EXEC,
                          MAP_PRIVATE|MAP_ANON, -1, 0);
        if (page == MAP_FAILED) { perror("oracle: mmap"); st.status = 6; goto out; }
        memcpy(page, pro, pro_len);
        snippet_ptr = (uint8_t*)page + pro_len;
        snippet_va = (uint64_t)(uintptr_t)snippet_ptr;
    }
    if (code_len) memcpy(snippet_ptr, code, code_len);
    memcpy(snippet_ptr + code_len, epi, epi_len);
    st.rip = snippet_va;

    // Install signal handlers for fault classification during the snippet.
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_NODEFER;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGILL,  &sa, NULL);
    sigaction(SIGFPE,  &sa, NULL);
    sigaction(SIGBUS,  &sa, NULL);

    g_st_ptr = &st;
    g_signal_status = 0;
    st.status = 0xBADBAD00u;  // pre-init; epilogue overwrites with ORACLE_OK_SENTINEL
    st.bytes_run = 0;
    // Same x87/MMX staging as run_mapped_snippet. This is the single-shot
    // request path; missing it here means the FXRSTOR loads a stale image and
    // the epilogue's FXSAVE result is never read back, so the caller sees its
    // own input echoed and every x87 comparison silently tests nothing.
    fx_build_in(&st);
    memset(g_fx_out, 0, sizeof g_fx_out);

    typedef void (*fn_t)(void);
    fn_t fn = (fn_t)(void*)(snippet_ptr - pro_len);

    if (sigsetjmp(g_jb, 1) == 0) {
        fn();
        if (st.status == ORACLE_OK_SENTINEL) {
            st.status = 0;
            st.bytes_run = code_len;
        } else {
            // Epilogue didn't complete but fn returned; shouldn't happen
            st.status = 6;
            st.bytes_run = 0;
        }
    } else {
        if (st.status == ORACLE_OK_SENTINEL) {
            // Signal fired after epilogue completed - benign
            st.status = 0;
            st.bytes_run = code_len;
        } else {
            st.status = g_signal_status;
            st.bytes_run = 0;
        }
    }

    // Snapshot the scratch page back into the state blob.
    memcpy(st.mem, (void*)(uintptr_t)g_mem_va, ORACLE_MEM_BLOB);
    if (st.status == 0) fx_extract_out(&st);
    normalize_out(&st);

out:
    if (write_full(1, &st, sizeof(st)) < 0) return 1;
    return 0;
}

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--bench") == 0) return bench_main();
    }
    return oneshot_main();
}
