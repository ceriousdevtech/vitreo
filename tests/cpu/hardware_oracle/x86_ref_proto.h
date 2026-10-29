/* Wire format shared by the AArch64 test driver and the x86-64 oracle.
   Kept plain C so the oracle can be built with -arch x86_64. */
#ifndef VJ_X86_REF_PROTO_H
#define VJ_X86_REF_PROTO_H

#include <stdint.h>

#define VJ_SCRATCH 4096
/* Both sides map the scratch buffer at this address, so a snippet that
   computes a pointer (LEA, address arithmetic) produces identical register
   values on hardware and in the emulator. */
#define VJ_SCRATCH_ADDR 0x0000000200000000ull
#define VJ_MAX_CODE 96
/* Where the guest's stack pointer starts inside the scratch region. */
#define VJ_STACK_OFFSET 2048
/* Snippets address memory only through this register, so the oracle and the
   emulator can use different real addresses for the same scratch buffer. */
#define VJ_BASE_REG 15 /* r15 */

typedef struct {
  uint64_t gpr[16];
  uint32_t eflags;
  uint32_t pad;
  uint8_t xmm[16][16];
} VjRegs;

typedef struct {
  uint32_t code_len;
  uint32_t pad;
  uint8_t code[VJ_MAX_CODE];
  VjRegs in;
  uint8_t data[VJ_SCRATCH];
} VjCase;

typedef struct {
  VjRegs out;
  uint8_t data[VJ_SCRATCH];
  uint32_t faulted;
  uint32_t pad;
} VjResult;

#endif
