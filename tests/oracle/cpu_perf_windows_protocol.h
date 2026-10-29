// SPDX-License-Identifier: LGPL-2.1-only
// Native Windows snippet-bench protocol. The MinGW PE and the Mac client
// share this layout. Frames travel over `ssh -T Windows` stdin/stdout.
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CPU_PERF_WIN_MAGIC 0x43505542u /* 'CPUB' */
#define CPU_PERF_WIN_VERSION 5u
#define CPU_PERF_WIN_ARCH_X86 32u
#define CPU_PERF_WIN_ARCH_X64 64u
#define CPU_PERF_WIN_MAX_CODE 256u
#define CPU_PERF_WIN_MEM 512u
#define CPU_PERF_WIN_MAX_ITERS 10000000u

#define CPU_PERF_WIN_OP_HELLO 1u
#define CPU_PERF_WIN_OP_BENCH 2u
#define CPU_PERF_WIN_OP_QUIT 3u

#define CPU_PERF_WIN_OK 0u
#define CPU_PERF_WIN_FAULT 1u
#define CPU_PERF_WIN_SETUP 6u

#pragma pack(push, 1)
typedef struct {
    uint32_t magic;
    uint32_t op;
    uint32_t payload_len;
} CpuPerfWinFrame;

typedef struct {
    uint32_t version;
    uint32_t arch_bits;
} CpuPerfWinHello;

typedef struct {
    uint32_t code_len; /* 1..MAX */
    uint32_t warmup;
    uint32_t iters;
    uint32_t gpr[8]; /* EAX ECX EDX EBX ESP EBP ESI EDI */
    uint8_t mem[CPU_PERF_WIN_MEM];
} CpuPerfWinBenchReq;

typedef struct {
    uint32_t code_len; /* 0..MAX; zero is the x64 wrapper control */
    uint32_t warmup;
    uint32_t iters;
    uint32_t reserved;
    /* RAX RCX RDX RBX RSP RBP RSI RDI R8..R15. RSP must be zero in v5;
       the runner substitutes its reset-on-each-iteration controlled stack. */
    uint64_t gpr[16];
    uint64_t rflags;
    /* Architectural SSE state. MXCSR bits 16..31 are reserved. */
    uint32_t mxcsr;
    uint32_t simd_reserved;
    uint8_t xmm[16][16];
    /* FXSAVE-compatible x87 state. FTW is abridged; ST is logical ST(0..7). */
    uint16_t fcw;
    uint16_t fsw;
    uint8_t ftw;
    uint8_t x87_reserved[3];
    uint8_t st[8][10];
    uint8_t mem[CPU_PERF_WIN_MEM];
} CpuPerfWinBenchReq64;

typedef struct {
    uint32_t status;
    uint32_t reserved;
    uint64_t warmup_ns;
    uint64_t run_ns;
    uint64_t iters;
} CpuPerfWinBenchResp;

typedef struct {
    uint64_t gpr[16]; /* RAX RCX RDX RBX RSP RBP RSI RDI R8..R15 */
    uint64_t rflags;
    uint64_t rip;
    uint64_t initial_rsp;
    uint64_t code_base;
    /* Final SIMD state, including exception status accumulated by the body. */
    uint32_t mxcsr;
    uint32_t simd_reserved;
    uint8_t xmm[16][16];
    uint16_t fcw;
    uint16_t fsw;
    uint8_t ftw;
    uint8_t x87_reserved[3];
    uint8_t st[8][10];
    uint8_t mem[CPU_PERF_WIN_MEM];
    uint32_t fault_code;
    uint32_t fault_flags;
    uint32_t parameter_count;
    uint32_t reserved;
    uint64_t fault_address;
    uint64_t fault_information[3];
} CpuPerfWinObserved64;

typedef struct {
    CpuPerfWinBenchResp timing;
    CpuPerfWinObserved64 observed;
} CpuPerfWinBenchResp64;
#pragma pack(pop)

#if defined(__cplusplus)
static_assert(sizeof(CpuPerfWinFrame) == 12);
static_assert(sizeof(CpuPerfWinHello) == 8);
static_assert(sizeof(CpuPerfWinBenchReq) == 556);
static_assert(sizeof(CpuPerfWinBenchReq64) == 1016);
static_assert(sizeof(CpuPerfWinBenchResp) == 32);
static_assert(sizeof(CpuPerfWinObserved64) == 1072);
static_assert(sizeof(CpuPerfWinBenchResp64) == 1104);
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(CpuPerfWinFrame) == 12, "CpuPerfWinFrame layout");
_Static_assert(sizeof(CpuPerfWinHello) == 8, "CpuPerfWinHello layout");
_Static_assert(sizeof(CpuPerfWinBenchReq) == 556, "CpuPerfWinBenchReq layout");
_Static_assert(sizeof(CpuPerfWinBenchReq64) == 1016,
               "CpuPerfWinBenchReq64 layout");
_Static_assert(sizeof(CpuPerfWinBenchResp) == 32,
               "CpuPerfWinBenchResp layout");
_Static_assert(sizeof(CpuPerfWinObserved64) == 1072,
               "CpuPerfWinObserved64 layout");
_Static_assert(sizeof(CpuPerfWinBenchResp64) == 1104,
               "CpuPerfWinBenchResp64 layout");
#endif

#ifdef __cplusplus
}
#endif
