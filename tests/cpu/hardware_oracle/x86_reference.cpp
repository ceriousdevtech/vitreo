/* SPDX-License-Identifier: LGPL-2.1-only */
/* Vitreo - Windows Runtime Engine for macOS ARM64 */

/* x86-64 differential oracle.
 *
 * Built for x86_64 and executed under the host x86 runner, this gives the CPU test suite
 * a real-silicon reference for instruction semantics and EFLAGS.
 *
 * The interpreter is the CPU semantic reference and the JIT must match it,
 * but nothing checked the interpreter itself against a
 * real x86 core. This binary is that check: it executes the same instruction
 * bytes on actual hardware and reports the architectural state back, so a
 * divergence names the interpreter rather than only the JIT.
 *
 * Protocol: VjCase records on stdin, VjResult records on stdout.
 *
 * Deliberately free of Vitreo dependencies: it is compiled for a different
 * architecture than everything around it, so it cannot link the runtime.
 */

#include <setjmp.h>
#include <stddef.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "x86_ref_proto.h"

static uint64_t g_saved_rsp;
static VjRegs g_out;
static VjRegs g_in;
static uint8_t* g_scratch;
static uint8_t* g_code;
static sigjmp_buf g_jmp;
static volatile int g_faulted;

static void OnFault(int sig) {
  (void)sig;
  g_faulted = 1;
  siglongjmp(g_jmp, 1);
}

/* --- tiny x86-64 emitter -------------------------------------------------- */

typedef struct {
  uint8_t* p;
  size_t n;
} Buf;

static void B1(Buf* b, uint8_t v) { b->p[b->n++] = v; }
static void B4(Buf* b, uint32_t v) {
  for (int i = 0; i < 4; ++i) B1(b, (uint8_t)(v >> (8 * i)));
}
static void B8(Buf* b, uint64_t v) {
  for (int i = 0; i < 8; ++i) B1(b, (uint8_t)(v >> (8 * i)));
}
static void Bn(Buf* b, const void* src, size_t n) {
  memcpy(b->p + b->n, src, n);
  b->n += n;
}

/* mov reg64, imm64 */
static void MovAbs(Buf* b, int reg, uint64_t imm) {
  B1(b, (uint8_t)(0x48 | ((reg >> 3) & 1)));
  B1(b, (uint8_t)(0xB8 + (reg & 7)));
  B8(b, imm);
}
/* mov [base + disp32], reg64   (base and reg are full 0..15) */
static void StoreReg(Buf* b, int base, int reg, int32_t disp) {
  B1(b, (uint8_t)(0x48 | (((reg >> 3) & 1) << 2) | ((base >> 3) & 1)));
  B1(b, 0x89);
  B1(b, (uint8_t)(0x80 | ((reg & 7) << 3) | (base & 7)));
  B4(b, (uint32_t)disp);
}
/* mov reg64, [base + disp32] */
static void LoadReg(Buf* b, int reg, int base, int32_t disp) {
  B1(b, (uint8_t)(0x48 | (((reg >> 3) & 1) << 2) | ((base >> 3) & 1)));
  B1(b, 0x8B);
  B1(b, (uint8_t)(0x80 | ((reg & 7) << 3) | (base & 7)));
  B4(b, (uint32_t)disp);
}
/* movups xmm_i, [base + disp32]  /  movups [base + disp32], xmm_i */
static void MovupsLoad(Buf* b, int x, int base, int32_t disp) {
  if (x >= 8 || base >= 8)
    B1(b, (uint8_t)(0x40 | (((x >> 3) & 1) << 2) | ((base >> 3) & 1)));
  B1(b, 0x0F);
  B1(b, 0x10);
  B1(b, (uint8_t)(0x80 | ((x & 7) << 3) | (base & 7)));
  B4(b, (uint32_t)disp);
}
static void MovupsStore(Buf* b, int x, int base, int32_t disp) {
  if (x >= 8 || base >= 8)
    B1(b, (uint8_t)(0x40 | (((x >> 3) & 1) << 2) | ((base >> 3) & 1)));
  B1(b, 0x0F);
  B1(b, 0x11);
  B1(b, (uint8_t)(0x80 | ((x & 7) << 3) | (base & 7)));
  B4(b, (uint32_t)disp);
}

/* Builds prologue + snippet + epilogue into g_code. */
static void BuildStub(const VjCase* c) {
  Buf b = {g_code, 0};

  /* Save the host's callee-saved registers and stack pointer. */
  B1(&b, 0x53);                                /* push rbx */
  B1(&b, 0x55);                                /* push rbp */
  B1(&b, 0x41); B1(&b, 0x54);                  /* push r12 */
  B1(&b, 0x41); B1(&b, 0x55);                  /* push r13 */
  B1(&b, 0x41); B1(&b, 0x56);                  /* push r14 */
  B1(&b, 0x41); B1(&b, 0x57);                  /* push r15 */
  MovAbs(&b, 0, (uint64_t)(uintptr_t)&g_saved_rsp);
  B1(&b, 0x48); B1(&b, 0x89); B1(&b, 0x20);    /* mov [rax], rsp */

  /* Install the guest register state.  rax addresses g_in throughout. */
  MovAbs(&b, 0, (uint64_t)(uintptr_t)&g_in);
  for (int x = 0; x < 16; ++x)
    MovupsLoad(&b, x, 0, (int32_t)(offsetof(VjRegs, xmm) + 16 * x));
  /* EFLAGS, while the host stack is still current. */
  B1(&b, 0x8B); B1(&b, 0x88);                  /* mov ecx, [rax+disp32] */
  B4(&b, (uint32_t)offsetof(VjRegs, eflags));
  B1(&b, 0x51);                                /* push rcx */
  B1(&b, 0x9D);                                /* popfq */
  for (int r = 1; r < 16; ++r)                 /* everything except rax */
    LoadReg(&b, r, 0, (int32_t)(8 * r));
  LoadReg(&b, 0, 0, 0);                        /* rax last */

  /* Reset the x87 stack and control word.  Without this a case that leaves
     values on the stack would perturb the next one, and the oracle would stop
     being a function of its inputs. */
  B1(&b, 0xDB); B1(&b, 0xE3);                  /* fninit */

  /* The snippet under test. */
  Bn(&b, c->code, c->code_len);

  /* Capture the resulting state.  `mov [moffs64], rax` needs no register. */
  B1(&b, 0x48); B1(&b, 0xA3);
  B8(&b, (uint64_t)(uintptr_t)&g_out.gpr[0]);
  MovAbs(&b, 0, (uint64_t)(uintptr_t)&g_out);
  for (int r = 1; r < 16; ++r) StoreReg(&b, 0, r, (int32_t)(8 * r));
  for (int x = 0; x < 16; ++x)
    MovupsStore(&b, x, 0, (int32_t)(offsetof(VjRegs, xmm) + 16 * x));

  /* Switch back to the host stack before touching it, then read EFLAGS. */
  MovAbs(&b, 1, (uint64_t)(uintptr_t)&g_saved_rsp);
  B1(&b, 0x48); B1(&b, 0x8B); B1(&b, 0x21);    /* mov rsp, [rcx] */
  B1(&b, 0x9C);                                /* pushfq */
  B1(&b, 0x59);                                /* pop rcx */
  B1(&b, 0x89); B1(&b, 0x88);                  /* mov [rax+disp32], ecx */
  B4(&b, (uint32_t)offsetof(VjRegs, eflags));

  B1(&b, 0x41); B1(&b, 0x5F);                  /* pop r15 */
  B1(&b, 0x41); B1(&b, 0x5E);                  /* pop r14 */
  B1(&b, 0x41); B1(&b, 0x5D);                  /* pop r13 */
  B1(&b, 0x41); B1(&b, 0x5C);                  /* pop r12 */
  B1(&b, 0x5D);                                /* pop rbp */
  B1(&b, 0x5B);                                /* pop rbx */
  B1(&b, 0xC3);                                /* ret */
}

int main(void) {
  g_scratch = (uint8_t*)mmap((void*)(uintptr_t)VJ_SCRATCH_ADDR, VJ_SCRATCH,
                             PROT_READ | PROT_WRITE,
                             MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
  g_code = (uint8_t*)mmap(NULL, 65536, PROT_READ | PROT_WRITE | PROT_EXEC,
                          MAP_PRIVATE | MAP_ANON, -1, 0);
  if (g_scratch == MAP_FAILED || g_code == MAP_FAILED) {
    fprintf(stderr, "x86_reference: mmap failed\n");
    return 1;
  }

  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = OnFault;
  sa.sa_flags = SA_NODEFER;
  sigaction(SIGSEGV, &sa, NULL);
  sigaction(SIGBUS, &sa, NULL);
  sigaction(SIGFPE, &sa, NULL);
  sigaction(SIGILL, &sa, NULL);
  sigaction(SIGTRAP, &sa, NULL);

  /* Tell the driver where the scratch region lives so it can build snippets
     with matching displacements. */
  uint64_t base = (uint64_t)(uintptr_t)g_scratch;
  if (fwrite(&base, sizeof(base), 1, stdout) != 1) return 1;
  fflush(stdout);

  VjCase c;
  VjResult r;
  while (fread(&c, sizeof(c), 1, stdin) == 1) {
    if (c.code_len > VJ_MAX_CODE) break;
    memcpy(g_scratch, c.data, VJ_SCRATCH);
    g_in = c.in;
    g_in.gpr[VJ_BASE_REG] = base;
    g_in.gpr[4] = base + VJ_STACK_OFFSET;   /* rsp */
    /* Only the flags software can actually set. */
    g_in.eflags = (g_in.eflags & 0x00000CD5u) | 2u;
    memset(&g_out, 0, sizeof(g_out));

    BuildStub(&c);
    g_faulted = 0;
    if (sigsetjmp(g_jmp, 1) == 0) {
      // The generated stub is data until it is called; copying the pointer
      // through a variable of function-pointer type keeps this well-formed.
      void (*entry)(void);
      memcpy(&entry, &g_code, sizeof(entry));
      entry();
    }

    memset(&r, 0, sizeof(r));
    r.out = g_out;
    r.faulted = (uint32_t)g_faulted;
    memcpy(r.data, g_scratch, VJ_SCRATCH);
    if (fwrite(&r, sizeof(r), 1, stdout) != 1) return 1;
    fflush(stdout);
  }
  return 0;
}
