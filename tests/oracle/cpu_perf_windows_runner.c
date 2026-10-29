/* SPDX-License-Identifier: LGPL-2.1-only
 *
 * Native Windows CPU snippet bench. Times the same fall-through x86 body
 * the Mac-side host x86 runner / JIT oracle uses. No desktop session required.
 *
 * Build:
 *   i686-w64-mingw32-gcc -O2 -Wall -Wextra -o cpu_perf_windows_runner.exe \
 *       cpu_perf_windows_runner.c -lkernel32
 *
 * Run (over SSH from the Mac):
 *   ssh -T Windows C:\oracle\bin\cpu_perf_windows_runner.exe
 */

#include "cpu_perf_windows_protocol.h"

#include <fcntl.h>
#include <io.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <windows.h>

#define WANT_MEM_VA 0x04000000u

static uint32_t g_gpr[8];
static uint32_t g_esp_save;
static uint8_t* g_scratch;
static uint32_t g_scratch_va;
static uint8_t* g_code;
static uint8_t* g_epilogue;
static volatile LONG g_faulted;

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

static uint8_t* emit_u8(uint8_t* p, uint8_t v) {
    *p++ = v;
    return p;
}

static uint8_t* emit_u32(uint8_t* p, uint32_t v) {
    memcpy(p, &v, 4);
    return p + 4;
}

static uint8_t* emit_load_gpr(uint8_t* p, int reg, uint32_t addr) {
    if (reg == 0) {
        p = emit_u8(p, 0xA1);
        return emit_u32(p, addr);
    }
    p = emit_u8(p, 0x8B);
    p = emit_u8(p, (uint8_t)(0x05 | (reg << 3)));
    return emit_u32(p, addr);
}

static size_t emit_trampoline(uint8_t* page, const uint8_t* snippet,
                              uint32_t snip_len) {
    uint8_t* p = page;
    p = emit_u8(p, 0x53); /* push ebx */
    p = emit_u8(p, 0x56);
    p = emit_u8(p, 0x57);
    p = emit_u8(p, 0x55);
    p = emit_u8(p, 0x89); /* mov [g_esp_save], esp */
    p = emit_u8(p, 0x25);
    p = emit_u32(p, (uint32_t)(uintptr_t)&g_esp_save);

    {
        const uint32_t base = (uint32_t)(uintptr_t)g_gpr;
        const int regs[7] = {0, 1, 2, 3, 5, 6, 7};
        int i;
        for (i = 0; i < 7; ++i) {
            p = emit_load_gpr(p, regs[i], base + (uint32_t)regs[i] * 4u);
        }
    }

    memcpy(p, snippet, snip_len);
    p += snip_len;
    g_epilogue = p;
    p = emit_u8(p, 0x5D);
    p = emit_u8(p, 0x5F);
    p = emit_u8(p, 0x5E);
    p = emit_u8(p, 0x5B);
    p = emit_u8(p, 0xC3);
    return (size_t)(p - page);
}

static LONG CALLBACK on_fault(EXCEPTION_POINTERS* ep) {
    InterlockedExchange(&g_faulted, 1);
    if (g_epilogue != NULL && ep != NULL && ep->ContextRecord != NULL) {
        ep->ContextRecord->Eip = (DWORD)(uintptr_t)g_epilogue;
        ep->ContextRecord->Esp = g_esp_save;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

static uint32_t rewrite_va(uint32_t v) {
    if (g_scratch_va == WANT_MEM_VA) return v;
    if (v >= WANT_MEM_VA && v < WANT_MEM_VA + 4096u) {
        return g_scratch_va + (v - WANT_MEM_VA);
    }
    return v;
}

static int setup_pages(void) {
    if (g_scratch != NULL && g_code != NULL) return 1;

    g_scratch = (uint8_t*)VirtualAlloc((void*)(uintptr_t)WANT_MEM_VA, 4096,
                                       MEM_RESERVE | MEM_COMMIT,
                                       PAGE_READWRITE);
    if (g_scratch != NULL) {
        g_scratch_va = WANT_MEM_VA;
    } else {
        g_scratch = (uint8_t*)VirtualAlloc(NULL, 4096, MEM_RESERVE | MEM_COMMIT,
                                           PAGE_READWRITE);
        if (g_scratch == NULL) return 0;
        g_scratch_va = (uint32_t)(uintptr_t)g_scratch;
    }

    g_code = (uint8_t*)VirtualAlloc(NULL, 4096, MEM_RESERVE | MEM_COMMIT,
                                    PAGE_EXECUTE_READWRITE);
    return g_code != NULL;
}

static uint64_t delta_ns(LARGE_INTEGER a, LARGE_INTEGER b, LARGE_INTEGER freq) {
    const uint64_t d = (uint64_t)(b.QuadPart - a.QuadPart);
    return d * 1000000000ull / (uint64_t)freq.QuadPart;
}

static void restore(const uint32_t* gpr, const uint8_t* mem) {
    int i;
    for (i = 0; i < 8; ++i) g_gpr[i] = rewrite_va(gpr[i]);
    memcpy(g_scratch, mem, CPU_PERF_WIN_MEM);
}

static uint32_t run_loop(uint32_t n, const uint32_t* gpr, const uint8_t* mem,
                         void (*fn)(void), uint64_t* out_ns) {
    LARGE_INTEGER freq, t0, t1;
    uint32_t i;
    QueryPerformanceFrequency(&freq);
    InterlockedExchange(&g_faulted, 0);
    QueryPerformanceCounter(&t0);
    for (i = 0; i < n; ++i) {
        restore(gpr, mem);
        fn();
        if (g_faulted) break;
    }
    QueryPerformanceCounter(&t1);
    *out_ns = delta_ns(t0, t1, freq);
    return g_faulted ? CPU_PERF_WIN_FAULT : CPU_PERF_WIN_OK;
}

static int handle_bench(const uint8_t* payload, uint32_t len) {
    CpuPerfWinBenchReq req;
    CpuPerfWinBenchResp resp;
    const uint8_t* code;
    size_t tramp_len;
    void (*fn)(void);

    memset(&resp, 0, sizeof(resp));
    if (len < sizeof(req)) {
        resp.status = CPU_PERF_WIN_SETUP;
        return send_frame(CPU_PERF_WIN_OP_BENCH, &resp, sizeof(resp));
    }
    memcpy(&req, payload, sizeof(req));
    if (req.code_len == 0 || req.code_len > CPU_PERF_WIN_MAX_CODE ||
        req.iters == 0 || req.iters > CPU_PERF_WIN_MAX_ITERS ||
        len != sizeof(req) + req.code_len) {
        resp.status = CPU_PERF_WIN_SETUP;
        return send_frame(CPU_PERF_WIN_OP_BENCH, &resp, sizeof(resp));
    }
    if (!setup_pages()) {
        resp.status = CPU_PERF_WIN_SETUP;
        return send_frame(CPU_PERF_WIN_OP_BENCH, &resp, sizeof(resp));
    }

    code = payload + sizeof(req);
    tramp_len = emit_trampoline(g_code, code, req.code_len);
    FlushInstructionCache(GetCurrentProcess(), g_code, tramp_len);
    fn = (void (*)(void))(void*)g_code;

    resp.status = run_loop(req.warmup ? req.warmup : 1u, req.gpr, req.mem, fn,
                           &resp.warmup_ns);
    if (resp.status == CPU_PERF_WIN_OK) {
        resp.status = run_loop(req.iters, req.gpr, req.mem, fn, &resp.run_ns);
        resp.iters = req.iters;
    }
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
        uint8_t payload[sizeof(CpuPerfWinBenchReq) + CPU_PERF_WIN_MAX_CODE];

        if (!read_exact(&hdr, sizeof(hdr))) break;
        if (hdr.magic != CPU_PERF_WIN_MAGIC ||
            hdr.payload_len > sizeof(payload)) {
            break;
        }
        if (hdr.payload_len && !read_exact(payload, hdr.payload_len)) break;

        if (hdr.op == CPU_PERF_WIN_OP_QUIT) {
            (void)send_frame(CPU_PERF_WIN_OP_QUIT, NULL, 0);
            break;
        }
        if (hdr.op == CPU_PERF_WIN_OP_HELLO) {
            CpuPerfWinHello hello;
            hello.version = CPU_PERF_WIN_VERSION;
            hello.arch_bits = CPU_PERF_WIN_ARCH_X86;
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
