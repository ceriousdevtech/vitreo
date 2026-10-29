/* SPDX-License-Identifier: LGPL-2.1-only
 *
 * Native Windows x64 CPU snippet bench. The generated wrapper saves the
 * Windows x64 nonvolatile machine state before installing guest registers,
 * so a snippet may use all sixteen GPRs and its own controlled stack.
 *
 * Build:
 *   x86_64-w64-mingw32-gcc -O2 -Wall -Wextra \
 *       -o cpu_perf_windows_runner_x64.exe \
 *       cpu_perf_windows_runner_x64.c -lkernel32
 */

#include "cpu_perf_windows_protocol.h"

#include <fcntl.h>
#include <io.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <windows.h>

#define WANT_MEM_VA 0x04000000ull
#define PAGE_BYTES 4096u
#define GUEST_STACK_BYTES (64u * 1024u)
#define GUEST_STACK_RESET_BYTES 512u
#define TRAMPOLINE_FIXED_BUDGET 2048u
#define ARCH_MXCSR_MASK_FALLBACK 0x0000FFBFu

typedef struct {
    uint64_t rsp;
    uint64_t rbx;
    uint64_t rbp;
    uint64_t rsi;
    uint64_t rdi;
    uint64_t r12;
    uint64_t r13;
    uint64_t r14;
    uint64_t r15;
} HostState;

static HostState g_host;
static __attribute__((aligned(16))) uint8_t g_host_fx[512];
static __attribute__((aligned(16))) uint8_t g_guest_fx[512];
static __attribute__((aligned(16))) uint8_t g_observed_fx[512];
static uint8_t* g_scratch;
static uint64_t g_scratch_va;
static uint8_t* g_guest_stack;
static uint64_t g_guest_rsp;
static uint8_t* g_code;
static uint8_t* g_snippet;
static uint8_t* g_epilogue;
static uint8_t* g_capture_end;
static uint8_t* g_recovery;
static CpuPerfWinObserved64* g_observed;
static volatile LONG g_faulted;

static uint32_t host_mxcsr_mask(void) {
    __attribute__((aligned(16))) uint8_t image[512];
    uint32_t mask;
    memset(image, 0, sizeof(image));
    __asm__ __volatile__("fxsave %0" : "=m"(image));
    memcpy(&mask, image + 28, sizeof(mask));
    /* Intel SDM Vol. 2, FXSAVE: zero MXCSR_MASK means use 0000FFBFH. */
    return mask != 0 ? mask : ARCH_MXCSR_MASK_FALLBACK;
}

static int read_exact(void* buf, size_t n) {
    uint8_t* p = (uint8_t*)buf;
    size_t got = 0;
    while (got < n) {
        size_t r = fread(p + got, 1, n - got, stdin);
        if (r == 0) return 0;
        got += r;
    }
    return 1;
}

static int write_exact(const void* buf, size_t n) {
    const uint8_t* p = (const uint8_t*)buf;
    size_t put = 0;
    while (put < n) {
        size_t w = fwrite(p + put, 1, n - put, stdout);
        if (w == 0) return 0;
        put += w;
    }
    return 1;
}

static int send_frame(uint32_t op, const void* payload, uint32_t len) {
    CpuPerfWinFrame hdr;
    hdr.magic = CPU_PERF_WIN_MAGIC;
    hdr.op = op;
    hdr.payload_len = len;
    if (!write_exact(&hdr, sizeof(hdr))) return 0;
    if (len && !write_exact(payload, len)) return 0;
    fflush(stdout);
    return 1;
}

static uint8_t* emit_u8(uint8_t* p, uint8_t value) {
    *p++ = value;
    return p;
}

static uint8_t* emit_u32(uint8_t* p, uint32_t value) {
    memcpy(p, &value, sizeof(value));
    return p + sizeof(value);
}

static uint8_t* emit_u64(uint8_t* p, uint64_t value) {
    memcpy(p, &value, sizeof(value));
    return p + sizeof(value);
}

static uint8_t* emit_movabs(uint8_t* p, unsigned reg, uint64_t value) {
    p = emit_u8(p, (uint8_t)(0x48u | ((reg >> 3u) & 1u)));
    p = emit_u8(p, (uint8_t)(0xB8u + (reg & 7u)));
    return emit_u64(p, value);
}

/* mov [r11+disp32], reg */
static uint8_t* emit_store_gpr(uint8_t* p, unsigned reg, uint32_t disp) {
    p = emit_u8(p, (uint8_t)(0x49u | (((reg >> 3u) & 1u) << 2u)));
    p = emit_u8(p, 0x89);
    p = emit_u8(p, (uint8_t)(0x83u | ((reg & 7u) << 3u)));
    return emit_u32(p, disp);
}

/* mov reg, [r11+disp32] */
static uint8_t* emit_load_gpr(uint8_t* p, unsigned reg, uint32_t disp) {
    p = emit_u8(p, (uint8_t)(0x49u | (((reg >> 3u) & 1u) << 2u)));
    p = emit_u8(p, 0x8B);
    p = emit_u8(p, (uint8_t)(0x83u | ((reg & 7u) << 3u)));
    return emit_u32(p, disp);
}

/* FXSAVE/FXRSTOR [r11]. The 512-byte operands are statically 16-byte aligned. */
static uint8_t* emit_fxsave_r11(uint8_t* p) {
    p = emit_u8(p, 0x41);
    p = emit_u8(p, 0x0F);
    p = emit_u8(p, 0xAE);
    return emit_u8(p, 0x03);
}

static uint8_t* emit_fxrstor_r11(uint8_t* p) {
    p = emit_u8(p, 0x41);
    p = emit_u8(p, 0x0F);
    p = emit_u8(p, 0xAE);
    return emit_u8(p, 0x0B);
}

/* mov [rip+disp32], reg -- captures without borrowing a guest GPR. */
static uint8_t* emit_capture_gpr(uint8_t* p, unsigned reg, uint64_t* slot) {
    intptr_t delta;
    p = emit_u8(p, (uint8_t)(0x48u | (((reg >> 3u) & 1u) << 2u)));
    p = emit_u8(p, 0x89);
    p = emit_u8(p, (uint8_t)(0x05u | ((reg & 7u) << 3u)));
    delta = (uint8_t*)(void*)slot - (p + 4);
    return emit_u32(p, (uint32_t)(int32_t)delta);
}

/* pop qword ptr [rip+disp32] */
static uint8_t* emit_capture_rflags(uint8_t* p, uint64_t* slot) {
    intptr_t delta;
    p = emit_u8(p, 0x9C); /* pushfq */
    p = emit_u8(p, 0x8F);
    p = emit_u8(p, 0x05);
    delta = (uint8_t*)(void*)slot - (p + 4);
    return emit_u32(p, (uint32_t)(int32_t)delta);
}

static uint8_t* emit_save_host(uint8_t* p) {
    p = emit_movabs(p, 11, (uint64_t)(uintptr_t)&g_host);
    p = emit_store_gpr(p, 4, (uint32_t)offsetof(HostState, rsp));
    p = emit_store_gpr(p, 3, (uint32_t)offsetof(HostState, rbx));
    p = emit_store_gpr(p, 5, (uint32_t)offsetof(HostState, rbp));
    p = emit_store_gpr(p, 6, (uint32_t)offsetof(HostState, rsi));
    p = emit_store_gpr(p, 7, (uint32_t)offsetof(HostState, rdi));
    p = emit_store_gpr(p, 12, (uint32_t)offsetof(HostState, r12));
    p = emit_store_gpr(p, 13, (uint32_t)offsetof(HostState, r13));
    p = emit_store_gpr(p, 14, (uint32_t)offsetof(HostState, r14));
    p = emit_store_gpr(p, 15, (uint32_t)offsetof(HostState, r15));
    p = emit_movabs(p, 11, (uint64_t)(uintptr_t)g_host_fx);
    return emit_fxsave_r11(p);
}

static uint8_t* emit_restore_host(uint8_t* p) {
    p = emit_movabs(p, 11, (uint64_t)(uintptr_t)g_host_fx);
    p = emit_fxrstor_r11(p);
    p = emit_movabs(p, 11, (uint64_t)(uintptr_t)&g_host);
    p = emit_load_gpr(p, 3, (uint32_t)offsetof(HostState, rbx));
    p = emit_load_gpr(p, 5, (uint32_t)offsetof(HostState, rbp));
    p = emit_load_gpr(p, 6, (uint32_t)offsetof(HostState, rsi));
    p = emit_load_gpr(p, 7, (uint32_t)offsetof(HostState, rdi));
    p = emit_load_gpr(p, 12, (uint32_t)offsetof(HostState, r12));
    p = emit_load_gpr(p, 13, (uint32_t)offsetof(HostState, r13));
    p = emit_load_gpr(p, 14, (uint32_t)offsetof(HostState, r14));
    p = emit_load_gpr(p, 15, (uint32_t)offsetof(HostState, r15));
    p = emit_load_gpr(p, 4, (uint32_t)offsetof(HostState, rsp));
    p = emit_u8(p, 0xFC); /* cld */
    return emit_u8(p, 0xC3); /* ret through the host return address */
}

static uint64_t rewrite_va(uint64_t value) {
    if (g_scratch_va == WANT_MEM_VA) return value;
    if (value >= WANT_MEM_VA && value < WANT_MEM_VA + PAGE_BYTES) {
        return g_scratch_va + (value - WANT_MEM_VA);
    }
    return value;
}

static size_t emit_trampoline(uint8_t* page, const uint8_t* snippet,
                              uint32_t snippet_len, const uint64_t* gpr,
                              uint64_t rflags, const uint8_t xmm[16][16],
                              uint32_t mxcsr, uint16_t fcw, uint16_t fsw,
                              uint8_t ftw, const uint8_t st[8][10],
                              uint32_t mxcsr_mask) {
    uint8_t* p = emit_save_host(page);
    unsigned reg;
    uint64_t values[16];
    for (reg = 0; reg < 16; ++reg) values[reg] = rewrite_va(gpr[reg]);
    values[4] = g_guest_rsp;
    memset(g_guest_fx, 0, sizeof(g_guest_fx));
    memcpy(g_guest_fx + 0, &fcw, sizeof(fcw));
    memcpy(g_guest_fx + 2, &fsw, sizeof(fsw));
    g_guest_fx[4] = ftw;
    memcpy(g_guest_fx + 24, &mxcsr, sizeof(mxcsr));
    memcpy(g_guest_fx + 28, &mxcsr_mask, sizeof(mxcsr_mask));
    for (reg = 0; reg < 8; ++reg)
        memcpy(g_guest_fx + 32 + reg * 16u, st[reg], 10u);
    for (reg = 0; reg < 16; ++reg)
        memcpy(g_guest_fx + 160 + reg * 16u, xmm[reg], 16u);

    p = emit_movabs(p, 11, (uint64_t)(uintptr_t)g_guest_fx);
    p = emit_fxrstor_r11(p);

    /* Install the user-writable arithmetic flags while still on host stack. */
    p = emit_movabs(p, 0, (rflags & 0xCD5ull) | 0x202ull);
    p = emit_u8(p, 0x50); /* push rax */
    p = emit_u8(p, 0x9D); /* popfq */

    for (reg = 0; reg < 16; ++reg) {
        if (reg != 4) p = emit_movabs(p, reg, values[reg]);
    }
    p = emit_movabs(p, 4, values[4]); /* RSP last */

    g_snippet = p;
    memcpy(p, snippet, snippet_len);
    p += snippet_len;
    g_epilogue = p;
    for (reg = 0; reg < 16; ++reg) {
        p = emit_capture_gpr(p, reg, &g_observed->gpr[reg]);
    }
    p = emit_capture_rflags(p, &g_observed->rflags);
    p = emit_movabs(p, 11, (uint64_t)(uintptr_t)g_observed_fx);
    p = emit_fxsave_r11(p);
    g_capture_end = p;
    g_recovery = p;
    p = emit_restore_host(p);
    return (size_t)(p - page);
}

static LONG CALLBACK on_fault(EXCEPTION_POINTERS* ep) {
    uintptr_t rip;
    CONTEXT* context;
    EXCEPTION_RECORD* record;
    DWORD i;
    if (ep == NULL || ep->ContextRecord == NULL || g_snippet == NULL ||
        g_capture_end == NULL || g_recovery == NULL || g_observed == NULL) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    context = ep->ContextRecord;
    record = ep->ExceptionRecord;
    rip = (uintptr_t)context->Rip;
    if (rip < (uintptr_t)g_snippet || rip >= (uintptr_t)g_capture_end) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    g_observed->gpr[0] = context->Rax;
    g_observed->gpr[1] = context->Rcx;
    g_observed->gpr[2] = context->Rdx;
    g_observed->gpr[3] = context->Rbx;
    g_observed->gpr[4] = context->Rsp;
    g_observed->gpr[5] = context->Rbp;
    g_observed->gpr[6] = context->Rsi;
    g_observed->gpr[7] = context->Rdi;
    g_observed->gpr[8] = context->R8;
    g_observed->gpr[9] = context->R9;
    g_observed->gpr[10] = context->R10;
    g_observed->gpr[11] = context->R11;
    g_observed->gpr[12] = context->R12;
    g_observed->gpr[13] = context->R13;
    g_observed->gpr[14] = context->R14;
    g_observed->gpr[15] = context->R15;
    g_observed->rflags = context->EFlags;
    g_observed->rip = context->Rip;
    g_observed->mxcsr = context->MxCsr;
    memcpy(g_observed->xmm, context->FltSave.XmmRegisters,
           sizeof(g_observed->xmm));
    g_observed->fcw = context->FltSave.ControlWord;
    g_observed->fsw = context->FltSave.StatusWord;
    g_observed->ftw = context->FltSave.TagWord;
    for (i = 0; i < 8u; ++i) {
        memcpy(g_observed->st[i], &context->FltSave.FloatRegisters[i], 10u);
    }
    if (record != NULL) {
        g_observed->fault_code = record->ExceptionCode;
        g_observed->fault_flags = record->ExceptionFlags;
        g_observed->fault_address =
            (uint64_t)(uintptr_t)record->ExceptionAddress;
        g_observed->parameter_count =
            record->NumberParameters < 3u ? record->NumberParameters : 3u;
        for (i = 0; i < g_observed->parameter_count; ++i) {
            g_observed->fault_information[i] =
                (uint64_t)record->ExceptionInformation[i];
        }
    }
    InterlockedExchange(&g_faulted, 1);
    context->Rip = (DWORD64)(uintptr_t)g_recovery;
    context->Rsp = g_host.rsp;
    return EXCEPTION_CONTINUE_EXECUTION;
}

static int setup_pages(void) {
    if (g_scratch != NULL && g_code != NULL && g_guest_stack != NULL) return 1;
    if (g_scratch == NULL) {
        g_scratch = (uint8_t*)VirtualAlloc((void*)(uintptr_t)WANT_MEM_VA,
                                           PAGE_BYTES,
                                           MEM_RESERVE | MEM_COMMIT,
                                           PAGE_READWRITE);
        if (g_scratch != NULL) {
            g_scratch_va = WANT_MEM_VA;
        } else {
            g_scratch = (uint8_t*)VirtualAlloc(NULL, PAGE_BYTES,
                                               MEM_RESERVE | MEM_COMMIT,
                                               PAGE_READWRITE);
            if (g_scratch == NULL) return 0;
            g_scratch_va = (uint64_t)(uintptr_t)g_scratch;
        }
    }
    if (g_code == NULL) {
        DWORD old_protect;
        g_code = (uint8_t*)VirtualAlloc(NULL, PAGE_BYTES * 2u,
                                        MEM_RESERVE | MEM_COMMIT,
                                        PAGE_EXECUTE_READWRITE);
        if (g_code == NULL) return 0;
        g_observed = (CpuPerfWinObserved64*)(void*)(g_code + PAGE_BYTES);
        if (!VirtualProtect(g_observed, PAGE_BYTES, PAGE_READWRITE,
                            &old_protect)) {
            VirtualFree(g_code, 0, MEM_RELEASE);
            g_code = NULL;
            g_observed = NULL;
            return 0;
        }
    }
    if (g_guest_stack == NULL) {
        g_guest_stack = (uint8_t*)VirtualAlloc(NULL, GUEST_STACK_BYTES,
                                               MEM_RESERVE | MEM_COMMIT,
                                               PAGE_READWRITE);
    }
    if (g_guest_stack != NULL) {
        g_guest_rsp =
            ((uint64_t)(uintptr_t)g_guest_stack + GUEST_STACK_BYTES / 2u) &
            ~15ull;
    }
    return g_guest_stack != NULL;
}

static uint64_t delta_ns(LARGE_INTEGER a, LARGE_INTEGER b, LARGE_INTEGER freq) {
    const uint64_t delta = (uint64_t)(b.QuadPart - a.QuadPart);
    return delta * 1000000000ull / (uint64_t)freq.QuadPart;
}

static void restore_memory(const uint8_t* mem) {
    memset(g_scratch, 0, PAGE_BYTES);
    memcpy(g_scratch, mem, CPU_PERF_WIN_MEM);
    memset((void*)(uintptr_t)(g_guest_rsp - GUEST_STACK_RESET_BYTES / 2u), 0,
           GUEST_STACK_RESET_BYTES);
    g_observed->fault_code = 0;
    g_observed->fault_flags = 0;
    g_observed->parameter_count = 0;
    g_observed->reserved = 0;
    g_observed->simd_reserved = 0;
    memset(g_observed->x87_reserved, 0, sizeof(g_observed->x87_reserved));
    g_observed->fault_address = 0;
    memset(g_observed->fault_information, 0,
           sizeof(g_observed->fault_information));
}

static uint32_t run_loop(uint32_t count, const uint8_t* mem, void (*fn)(void),
                         uint64_t* out_ns) {
    LARGE_INTEGER frequency, begin, end;
    uint32_t i;
    QueryPerformanceFrequency(&frequency);
    InterlockedExchange(&g_faulted, 0);
    QueryPerformanceCounter(&begin);
    for (i = 0; i < count; ++i) {
        restore_memory(mem);
        fn();
        if (g_faulted) break;
    }
    QueryPerformanceCounter(&end);
    *out_ns = delta_ns(begin, end, frequency);
    return g_faulted ? CPU_PERF_WIN_FAULT : CPU_PERF_WIN_OK;
}

static int handle_bench(const uint8_t* payload, uint32_t len) {
    CpuPerfWinBenchReq64 req;
    CpuPerfWinBenchResp64 resp;
    size_t trampoline_len;
    void (*fn)(void);
    uint32_t mxcsr_mask;
    unsigned reg;

    memset(&resp, 0, sizeof(resp));
    if (len < sizeof(req)) {
        resp.timing.status = CPU_PERF_WIN_SETUP;
        return send_frame(CPU_PERF_WIN_OP_BENCH, &resp, sizeof(resp));
    }
    memcpy(&req, payload, sizeof(req));
    mxcsr_mask = host_mxcsr_mask();
    if (req.code_len > CPU_PERF_WIN_MAX_CODE ||
        req.code_len > PAGE_BYTES - TRAMPOLINE_FIXED_BUDGET ||
        req.iters == 0 || req.iters > CPU_PERF_WIN_MAX_ITERS ||
        req.reserved != 0 || req.simd_reserved != 0 ||
        req.x87_reserved[0] != 0 || req.x87_reserved[1] != 0 ||
        req.x87_reserved[2] != 0 ||
        (req.mxcsr & ~mxcsr_mask) != 0 || req.gpr[4] != 0 ||
        len != sizeof(req) + req.code_len || !setup_pages()) {
        resp.timing.status = CPU_PERF_WIN_SETUP;
        return send_frame(CPU_PERF_WIN_OP_BENCH, &resp, sizeof(resp));
    }

    trampoline_len =
        emit_trampoline(g_code, payload + sizeof(req), req.code_len, req.gpr,
                        req.rflags, req.xmm, req.mxcsr, req.fcw, req.fsw,
                        req.ftw, req.st, mxcsr_mask);
    if (trampoline_len > PAGE_BYTES) {
        resp.timing.status = CPU_PERF_WIN_SETUP;
        return send_frame(CPU_PERF_WIN_OP_BENCH, &resp, sizeof(resp));
    }
    FlushInstructionCache(GetCurrentProcess(), g_code, trampoline_len);
    fn = (void (*)(void))(void*)g_code;

    resp.timing.status = run_loop(req.warmup ? req.warmup : 1u, req.mem, fn,
                                  &resp.timing.warmup_ns);
    if (resp.timing.status == CPU_PERF_WIN_OK) {
        resp.timing.status =
            run_loop(req.iters, req.mem, fn, &resp.timing.run_ns);
        if (resp.timing.status == CPU_PERF_WIN_OK) {
            resp.timing.iters = req.iters;
        }
    }
    memcpy(&resp.observed, g_observed, sizeof(resp.observed));
    if (resp.timing.status == CPU_PERF_WIN_OK) {
        memcpy(&resp.observed.fcw, g_observed_fx + 0,
               sizeof(resp.observed.fcw));
        memcpy(&resp.observed.fsw, g_observed_fx + 2,
               sizeof(resp.observed.fsw));
        resp.observed.ftw = g_observed_fx[4];
        memcpy(&resp.observed.mxcsr, g_observed_fx + 24,
               sizeof(resp.observed.mxcsr));
        for (reg = 0; reg < 8u; ++reg) {
            memcpy(resp.observed.st[reg], g_observed_fx + 32 + reg * 16u,
                   10u);
        }
        memcpy(resp.observed.xmm, g_observed_fx + 160,
               sizeof(resp.observed.xmm));
    }
    if (resp.timing.status == CPU_PERF_WIN_OK) {
        resp.observed.rip = (uint64_t)(uintptr_t)g_epilogue;
    }
    resp.observed.initial_rsp = g_guest_rsp;
    resp.observed.code_base = (uint64_t)(uintptr_t)g_snippet;
    memcpy(resp.observed.mem, g_scratch, CPU_PERF_WIN_MEM);
    return send_frame(CPU_PERF_WIN_OP_BENCH, &resp, sizeof(resp));
}

int main(void) {
    PVOID veh;
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
    veh = AddVectoredExceptionHandler(1, on_fault);
    if (veh == NULL) return 1;

    for (;;) {
        CpuPerfWinFrame hdr;
        uint8_t payload[sizeof(CpuPerfWinBenchReq64) + CPU_PERF_WIN_MAX_CODE];
        if (!read_exact(&hdr, sizeof(hdr))) break;
        if (hdr.magic != CPU_PERF_WIN_MAGIC || hdr.payload_len > sizeof(payload))
            break;
        if (hdr.payload_len && !read_exact(payload, hdr.payload_len)) break;

        if (hdr.op == CPU_PERF_WIN_OP_QUIT) {
            (void)send_frame(CPU_PERF_WIN_OP_QUIT, NULL, 0);
            break;
        }
        if (hdr.op == CPU_PERF_WIN_OP_HELLO) {
            CpuPerfWinHello hello;
            hello.version = CPU_PERF_WIN_VERSION;
            hello.arch_bits = CPU_PERF_WIN_ARCH_X64;
            if (!send_frame(CPU_PERF_WIN_OP_HELLO, &hello, sizeof(hello))) break;
            continue;
        }
        if (hdr.op == CPU_PERF_WIN_OP_BENCH) {
            if (!handle_bench(payload, hdr.payload_len)) break;
            continue;
        }
        break;
    }

    RemoveVectoredExceptionHandler(veh);
    return 0;
}
