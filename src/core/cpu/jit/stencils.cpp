// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64

/// @file stencils.cpp
/// @brief ARM64 code stencil lookup for copy-and-patch JIT (CPU-040)

#include "stencils.hpp"
#include <sys/sysctl.h>
#include <cstdio>
#include <cstdlib>

namespace vitreo::cpu::jit {

// The stencil system uses a lookup table mapping x86 Opcode -> availability.
// Actual code emission is done inline by JitCompiler using the arm64::
// encoding helpers, rather than copying pre-built byte arrays. This is the
// "patch" part of copy-and-patch: we build ARM64 instructions directly with
// concrete register offsets, avoiding the need for relocation fixups.
//
// The stencil table tells the compiler which opcodes it can handle.

namespace {

// Opcodes that have JIT support (Phase 1: core ALU/memory/branch)
bool is_jitable(Opcode op) noexcept {
    // Full x87 range + EMMS / FFREE / FBLD / FBSTP: compile_fpu owns these.
    {
        const auto u = static_cast<u32>(op);
        if ((u >= static_cast<u32>(Opcode::FNCLEX) &&
             u <= static_cast<u32>(Opcode::FDIVR_M64)) ||
            op == Opcode::EMMS ||
            op == Opcode::FFREE_STI ||
            op == Opcode::FBLD_M80 ||
            op == Opcode::FBSTP_M80) {
            return true;
        }
    }
    switch (op) {
    // MOV reg, reg / reg, imm / reg, mem / mem, reg
    case Opcode::MOV_R32_IMM32:
    case Opcode::MOV_RM32_R32:
    case Opcode::MOV_R32_RM32:
    case Opcode::MOV_RM32_IMM32:
    case Opcode::MOV_R8_IMM8:
    case Opcode::MOV_RM8_R8:
    case Opcode::MOV_R8_RM8:
    case Opcode::MOV_RM8_IMM8:
    case Opcode::MOVZX_R16_RM8:
    case Opcode::MOVZX_R32_RM8:
    case Opcode::MOVZX_R32_RM16:
    case Opcode::MOVSX_R16_RM8:
    case Opcode::MOVSX_R32_RM8:
    case Opcode::MOVSX_R32_RM16:
    // ADD
    case Opcode::ADD_RM32_R32:
    case Opcode::ADD_R32_RM32:
    case Opcode::ADD_RM32_IMM32:
    case Opcode::ADD_RM32_IMM8:
    case Opcode::ADD_EAX_IMM32:
    // SUB
    case Opcode::SUB_RM32_R32:
    case Opcode::SUB_R32_RM32:
    case Opcode::SUB_RM32_IMM32:
    case Opcode::SUB_RM32_IMM8:
    case Opcode::SUB_EAX_IMM32:
    // AND
    case Opcode::AND_RM32_R32:
    case Opcode::AND_R32_RM32:
    case Opcode::AND_RM32_IMM32:
    case Opcode::AND_RM32_IMM8:
    case Opcode::AND_EAX_IMM32:
    // OR
    case Opcode::OR_RM32_R32:
    case Opcode::OR_R32_RM32:
    case Opcode::OR_RM32_IMM32:
    case Opcode::OR_RM32_IMM8:
    case Opcode::OR_EAX_IMM32:
    // XOR
    case Opcode::XOR_RM32_R32:
    case Opcode::XOR_R32_RM32:
    case Opcode::XOR_RM32_IMM32:
    case Opcode::XOR_RM32_IMM8:
    case Opcode::XOR_EAX_IMM32:
    // CMP / TEST
    case Opcode::CMP_RM32_R32:
    case Opcode::CMP_R32_RM32:
    case Opcode::CMP_RM32_IMM32:
    case Opcode::CMP_RM32_IMM8:
    case Opcode::CMP_EAX_IMM32:
    case Opcode::TEST_RM32_R32:
    case Opcode::TEST_EAX_IMM32:
    case Opcode::TEST_AL_IMM8:
    case Opcode::TEST_RM8_IMM8:
    case Opcode::TEST_RM16_R16:
    // 64-bit GPR ops
    case Opcode::MOV_R64_RM64:
    case Opcode::ADD_RM64_IMM8:
    case Opcode::ADD_RM64_R64:
    case Opcode::ADD_R64_RM64:
    case Opcode::ADD_RAX_IMM32:
    case Opcode::SUB_R64_RM64:
    case Opcode::SUB_RM64_R64:
    case Opcode::SUB_RAX_IMM32:
    case Opcode::CMP_RM64_IMM8:
    case Opcode::CMP_RM64_IMM32:
    case Opcode::CMP_RM64_R64:
    case Opcode::CMP_RAX_IMM32:
    case Opcode::TEST_RM64_R64:
    case Opcode::TEST_RM64_IMM32:
    case Opcode::TEST_RAX_IMM32:
    case Opcode::CMP_R64_RM64:
    case Opcode::XOR_R64_RM64:
    case Opcode::XOR_RM64_R64:
    case Opcode::XOR_RM64_IMM32:
    case Opcode::XOR_RM64_IMM8:
    case Opcode::XOR_RAX_IMM32:
    case Opcode::AND_R64_RM64:
    case Opcode::AND_RM64_R64:
    case Opcode::AND_RM64_IMM32:
    case Opcode::AND_RM64_IMM8:
    case Opcode::AND_RAX_IMM32:
    case Opcode::OR_R64_RM64:
    case Opcode::OR_RM64_R64:
    case Opcode::OR_RM64_IMM32:
    case Opcode::OR_RM64_IMM8:
    case Opcode::OR_RAX_IMM32:
    case Opcode::ADC_R64_RM64:
    case Opcode::ADC_RM64_R64:
    case Opcode::ADC_RM64_IMM8:
    case Opcode::ADC_RM64_IMM32:
    case Opcode::ADC_RAX_IMM32:
    case Opcode::SBB_R64_RM64:
    case Opcode::SBB_RM64_R64:
    case Opcode::SBB_RM64_IMM32:
    case Opcode::SBB_RM64_IMM8:
    case Opcode::SBB_RAX_IMM32:
    case Opcode::SBB_EAX_IMM32:
    case Opcode::ADD_RM64_IMM32:
    case Opcode::SUB_RM64_IMM32:
    case Opcode::LEA_R64_M:
    case Opcode::POP_R64:
    case Opcode::POP_RM64:
    case Opcode::PUSH_RM64:
    case Opcode::MOV_R64_IMM64:
    case Opcode::MOV_RM64_R64:
    case Opcode::MOV_RM64_IMM32:
    case Opcode::SUB_RM64_IMM8:
    case Opcode::INC_RM64:
    case Opcode::DEC_RM64:
    case Opcode::NEG_RM64:
    case Opcode::NOT_RM64:
    case Opcode::CALL_RM64:
    case Opcode::JMP_RM64:
    case Opcode::PUSH_R64:
    case Opcode::IMUL_R64_RM64:
    case Opcode::IMUL_R64_RM64_IMM32:
    case Opcode::IMUL_R64_RM64_IMM8:
    case Opcode::MUL_RM64:
    case Opcode::IMUL_RM64:
    case Opcode::DIV_RM64:
    case Opcode::IDIV_RM64:
    case Opcode::MOVSXD_R64_RM32:
    case Opcode::MOVSX_R64_RM8:
    case Opcode::CDQE:
    case Opcode::CQO:
    case Opcode::AND_R8_RM8:
    case Opcode::AND_RM8_R8:
    case Opcode::SUB_RM8_R8:
    case Opcode::SHL_RM64_IMM8:
    case Opcode::SHL_RM64_1:
    case Opcode::SHL_RM64_CL:
    case Opcode::SHR_RM64_IMM8:
    case Opcode::SHR_RM64_1:
    case Opcode::SHR_RM64_CL:
    case Opcode::SAR_RM64_IMM8:
    case Opcode::SAR_RM64_1:
    case Opcode::SAR_RM64_CL:
    case Opcode::ROL_RM64_IMM8:
    case Opcode::ROL_RM64_1:
    case Opcode::ROL_RM64_CL:
    case Opcode::ROR_RM64_IMM8:
    case Opcode::ROR_RM64_1:
    case Opcode::ROR_RM64_CL:
    case Opcode::STOSQ:
    case Opcode::MOVSQ:
    case Opcode::CMPSQ:
    case Opcode::SCASQ:
    case Opcode::CVTSI2SS_XMM_RM64:
    case Opcode::CVTSI2SD_XMM_RM64:
    case Opcode::CMOVB_R64_RM64:
    case Opcode::CMOVNB_R64_RM64:
    case Opcode::CMOVZ_R64_RM64:
    case Opcode::CMOVNZ_R64_RM64:
    case Opcode::CMOVBE_R64_RM64:
    case Opcode::CMOVNBE_R64_RM64:
    case Opcode::CMOVS_R64_RM64:
    case Opcode::CMOVNS_R64_RM64:
    case Opcode::CMOVL_R64_RM64:
    case Opcode::CMOVNL_R64_RM64:
    case Opcode::CMOVLE_R64_RM64:
    case Opcode::CMOVNLE_R64_RM64:
    case Opcode::CMOVO_R64_RM64:
    case Opcode::CMOVNO_R64_RM64:
    case Opcode::CMOVP_R64_RM64:
    case Opcode::CMOVNP_R64_RM64:

    // CMP 16-bit
    case Opcode::CMP_RM16_R16:
    case Opcode::CMP_R16_RM16:
    case Opcode::CMP_RM16_IMM16:
    case Opcode::CMP_RM16_IMM8:
    case Opcode::CMP_AX_IMM16:
    // ADD / SUB 16-bit
    case Opcode::ADD_RM16_R16:
    case Opcode::ADD_R16_RM16:
    case Opcode::ADD_RM16_IMM16:
    case Opcode::ADD_RM16_IMM8:
    case Opcode::ADD_AX_IMM16:
    case Opcode::SUB_RM16_R16:
    case Opcode::SUB_R16_RM16:
    case Opcode::SUB_RM16_IMM16:
    case Opcode::SUB_RM16_IMM8:
    case Opcode::SUB_AX_IMM16:
    // AND 16-bit
    case Opcode::AND_RM16_R16:
    case Opcode::AND_R16_RM16:
    case Opcode::AND_RM16_IMM16:
    case Opcode::AND_RM16_IMM8:
    case Opcode::AND_AX_IMM16:
    // OR 16-bit
    case Opcode::OR_RM16_R16:
    case Opcode::OR_R16_RM16:
    case Opcode::OR_RM16_IMM16:
    case Opcode::OR_RM16_IMM8:
    case Opcode::OR_AX_IMM16:
    // XOR 16-bit
    case Opcode::XOR_RM16_R16:
    case Opcode::XOR_R16_RM16:
    case Opcode::XOR_RM16_IMM16:
    case Opcode::XOR_RM16_IMM8:
    case Opcode::XOR_AX_IMM16:
    // ADC / SBB 16-bit
    case Opcode::ADC_RM16_R16:
    case Opcode::ADC_R16_RM16:
    case Opcode::ADC_RM16_IMM16:
    case Opcode::ADC_RM16_IMM8:
    case Opcode::ADC_AX_IMM16:
    case Opcode::SBB_RM16_R16:
    case Opcode::SBB_R16_RM16:
    case Opcode::SBB_RM16_IMM16:
    case Opcode::SBB_RM16_IMM8:
    case Opcode::SBB_AX_IMM16:
    case Opcode::TEST_AX_IMM16:
    case Opcode::TEST_RM16_IMM16:
    // OR 8-bit
    case Opcode::OR_AL_IMM8:
    case Opcode::OR_RM8_IMM8:
    case Opcode::OR_RM8_R8:
    case Opcode::OR_R8_RM8:
    case Opcode::ADD_AL_IMM8:
    case Opcode::ADD_R8_RM8:
    case Opcode::ADD_RM8_R8:
    case Opcode::SBB_AL_IMM8:
    case Opcode::SBB_RM8_IMM8:
    case Opcode::SBB_RM8_R8:
    case Opcode::SBB_R8_RM8:
    case Opcode::ADC_AL_IMM8:
    case Opcode::ADC_RM8_IMM8:
    case Opcode::ADC_RM8_R8:
    case Opcode::ADC_R8_RM8:
    case Opcode::XCHG_RM8_R8:
    // JMP indirect
    case Opcode::JMP_RM32:
    // MOV moffs32, EAX
    case Opcode::MOV_MOFFS32_EAX:
    // String ops
    case Opcode::MOVSB:
    case Opcode::MOVSD_STR:
    // 8-bit ALU extras
    case Opcode::AND_AL_IMM8:
    case Opcode::SUB_AL_IMM8:
    case Opcode::XOR_AL_IMM8:
    case Opcode::SUB_R8_RM8:
    // Shift / rotate 16-bit / 8-bit
    case Opcode::SHR_RM16_IMM8:
    case Opcode::SHR_RM16_1:
    case Opcode::SHR_RM16_CL:
    case Opcode::SHL_RM16_IMM8:
    case Opcode::SHL_RM16_1:
    case Opcode::SHL_RM16_CL:
    case Opcode::SAR_RM16_IMM8:
    case Opcode::SAR_RM16_1:
    case Opcode::SAR_RM16_CL:
    case Opcode::ROL_RM16_IMM8:
    case Opcode::ROL_RM16_1:
    case Opcode::ROL_RM16_CL:
    case Opcode::ROR_RM16_IMM8:
    case Opcode::ROR_RM16_1:
    case Opcode::ROR_RM16_CL:
    case Opcode::ROL_RM8_IMM8:
    case Opcode::ROL_RM8_1:
    case Opcode::ROL_RM8_CL:
    case Opcode::ROR_RM8_IMM8:
    case Opcode::ROR_RM8_1:
    case Opcode::ROR_RM8_CL:
    case Opcode::SHL_RM8_1:
    case Opcode::SHR_RM8_1:
    case Opcode::SHL_RM8_IMM8:
    case Opcode::SHR_RM8_IMM8:
    case Opcode::SAR_RM8_IMM8:
    case Opcode::SAR_RM8_1:
    case Opcode::SHL_RM8_CL:
    case Opcode::SHR_RM8_CL:
    case Opcode::SAR_RM8_CL:
    // SHL / SHR / SAR
    case Opcode::SHL_RM32_IMM8:
    case Opcode::SHR_RM32_IMM8:
    case Opcode::SAR_RM32_IMM8:
    case Opcode::SHL_RM32_1:
    case Opcode::SHR_RM32_1:
    case Opcode::SAR_RM32_1:
    case Opcode::SHL_RM32_CL:
    case Opcode::SHR_RM32_CL:
    case Opcode::SAR_RM32_CL:
    // ROR (IMM8 / 1 / CL)
    case Opcode::ROR_RM32_IMM8:
    case Opcode::ROR_RM32_1:
    case Opcode::ROR_RM32_CL:
    // ROL (IMM8 / 1)
    case Opcode::ROL_RM32_IMM8:
    case Opcode::ROL_RM32_1:
    // ROL (CL)
    case Opcode::ROL_RM32_CL:
    // RCL / RCR (IMM8 / 1 / CL) — r/m8, r/m16, r/m32
    case Opcode::RCL_RM8_IMM8:
    case Opcode::RCL_RM8_1:
    case Opcode::RCL_RM8_CL:
    case Opcode::RCR_RM8_IMM8:
    case Opcode::RCR_RM8_1:
    case Opcode::RCR_RM8_CL:
    case Opcode::RCL_RM16_IMM8:
    case Opcode::RCL_RM16_1:
    case Opcode::RCL_RM16_CL:
    case Opcode::RCR_RM16_IMM8:
    case Opcode::RCR_RM16_1:
    case Opcode::RCR_RM16_CL:
    case Opcode::RCL_RM32_IMM8:
    case Opcode::RCL_RM32_1:
    case Opcode::RCL_RM32_CL:
    case Opcode::RCR_RM32_IMM8:
    case Opcode::RCR_RM32_1:
    case Opcode::RCR_RM32_CL:
    // INC / DEC
    case Opcode::INC_RM8:
    case Opcode::INC_RM16:
    case Opcode::INC_RM32:
    case Opcode::DEC_RM8:
    case Opcode::DEC_RM16:
    case Opcode::DEC_RM32:
    // NEG / NOT
    case Opcode::NEG_RM8:
    case Opcode::NEG_RM16:
    case Opcode::NEG_RM32:
    case Opcode::NOT_RM8:
    case Opcode::NOT_RM32:
    // LEA
    case Opcode::LEA_R32_M:
    // PUSH / POP (32-bit)
    case Opcode::PUSH_R32:
    case Opcode::POP_R32:
    case Opcode::PUSH_IMM32:
    case Opcode::PUSH_IMM8:
    case Opcode::PUSHA:
    case Opcode::POPA:
    // MUL / IMUL
    case Opcode::MUL_RM8:
    case Opcode::MUL_RM16:
    case Opcode::MUL_RM32:
    case Opcode::IMUL_RM8:
    case Opcode::IMUL_RM16:
    case Opcode::IMUL_R16_RM16:
    case Opcode::IMUL_R16_RM16_IMM16:
    case Opcode::IMUL_R16_RM16_IMM8:
    case Opcode::IMUL_R32_RM32:
    case Opcode::IMUL_R32_RM32_IMM32:
    case Opcode::IMUL_R32_RM32_IMM8:
    // ADC / SBB
    case Opcode::ADC_RM32_R32:
    case Opcode::ADC_R32_RM32:
    case Opcode::ADC_RM32_IMM32:
    case Opcode::ADC_RM32_IMM8:
    case Opcode::SBB_RM32_R32:
    case Opcode::SBB_R32_RM32:
    case Opcode::SBB_RM32_IMM32:
    case Opcode::SBB_RM32_IMM8:
    // CMPXCHG8B
    case Opcode::CMPXCHG8B_M64:
    case Opcode::CMPXCHG16B_M128:
    // Jcc
    case Opcode::JO_REL8:
    case Opcode::JNO_REL8:
    case Opcode::JB_REL8:
    case Opcode::JNB_REL8:
    case Opcode::JZ_REL8:
    case Opcode::JNZ_REL8:
    case Opcode::JBE_REL8:
    case Opcode::JNBE_REL8:
    case Opcode::JS_REL8:
    case Opcode::JNS_REL8:
    case Opcode::JP_REL8:
    case Opcode::JNP_REL8:
    case Opcode::JL_REL8:
    case Opcode::JNL_REL8:
    case Opcode::JLE_REL8:
    case Opcode::JNLE_REL8:
    case Opcode::JO_REL32:
    case Opcode::JNO_REL32:
    case Opcode::JB_REL32:
    case Opcode::JNB_REL32:
    case Opcode::JZ_REL32:
    case Opcode::JNZ_REL32:
    case Opcode::JBE_REL32:
    case Opcode::JNBE_REL32:
    case Opcode::JS_REL32:
    case Opcode::JNS_REL32:
    case Opcode::JP_REL32:
    case Opcode::JNP_REL32:
    case Opcode::JL_REL32:
    case Opcode::JNL_REL32:
    case Opcode::JLE_REL32:
    case Opcode::JNLE_REL32:
    // JMP
    case Opcode::JMP_REL8:
    case Opcode::JMP_REL32:
    // LOOP / JCXZ family (AddressSize selects CX vs ECX)
    case Opcode::LOOP_REL8:
    case Opcode::LOOPE_REL8:
    case Opcode::LOOPNE_REL8:
    case Opcode::JCXZ_REL8:
    case Opcode::JECXZ_REL8:
    // CALL / RET
    case Opcode::CALL_REL32:
    case Opcode::RET_NEAR:
    case Opcode::RET_NEAR_IMM16:
    // Far JMP / CALL — Intel SDM Vol. 2, same-privilege user-mode
    case Opcode::JMP_PTR16_16:
    case Opcode::JMP_PTR16_32:
    case Opcode::JMP_M16_16:
    case Opcode::JMP_M16_32:
    case Opcode::CALL_PTR16_16:
    case Opcode::CALL_PTR16_32:
    case Opcode::CALL_M16_16:
    case Opcode::CALL_M16_32:
    case Opcode::JMP_M16_64:
    case Opcode::CALL_M16_64:
    // NOP / misc
    case Opcode::NOP:
    case Opcode::PAUSE:
    case Opcode::RDTSC:
    case Opcode::RDTSCP:
    case Opcode::LFENCE:
    case Opcode::MFENCE:
    case Opcode::SFENCE:
    case Opcode::FXSAVE_M512:
    case Opcode::FXRSTOR_M512:
    case Opcode::CLFLUSH_M8:
    case Opcode::MOVNTI_M32_R32:
    case Opcode::MOVNTI_M64_R64:
    case Opcode::LDMXCSR_M32:
    case Opcode::STMXCSR_M32:
    // LEAVE / ENTER / CDQ / CWD / CBW / CWDE / XCHG
    case Opcode::LEAVE:
    case Opcode::ENTER:
    case Opcode::CDQ:
    case Opcode::CWD:
    case Opcode::CBW:
    case Opcode::CWDE:
    case Opcode::XCHG_EAX_R32:
    // MOV 16-bit
    case Opcode::MOV_RM16_R16:
    case Opcode::MOV_R16_RM16:
    case Opcode::MOV_R16_IMM16:
    // DIV / IDIV
    case Opcode::DIV_RM8:
    case Opcode::DIV_RM16:
    case Opcode::DIV_RM32:
    case Opcode::IDIV_RM8:
    case Opcode::IDIV_RM16:
    case Opcode::IDIV_RM32:
    // 8-bit ALU
    case Opcode::CMP_RM8_IMM8:
    case Opcode::CMP_AL_IMM8:
    case Opcode::AND_RM8_IMM8:
    case Opcode::XOR_R8_RM8:
    case Opcode::XOR_RM8_R8:
    case Opcode::XOR_RM8_IMM8:
    case Opcode::TEST_RM8_R8:
    // Indirect CALL / PUSH / POP r/m32
    case Opcode::CALL_RM32:
    case Opcode::PUSH_RM32:
    case Opcode::POP_RM32:
    // MOV EAX, moffs32
    case Opcode::MOV_AL_MOFFS8:
    case Opcode::MOV_MOFFS8_AL:
    case Opcode::MOV_EAX_MOFFS32:
    // String ops
    case Opcode::STOSD:
    // SETcc (all 16 conditions)
    case Opcode::SETO_RM8:
    case Opcode::SETNO_RM8:
    case Opcode::SETB_RM8:
    case Opcode::SETNB_RM8:
    case Opcode::SETZ_RM8:
    case Opcode::SETNZ_RM8:
    case Opcode::SETBE_RM8:
    case Opcode::SETNBE_RM8:
    case Opcode::SETS_RM8:
    case Opcode::SETNS_RM8:
    case Opcode::SETP_RM8:
    case Opcode::SETNP_RM8:
    case Opcode::SETL_RM8:
    case Opcode::SETNL_RM8:
    case Opcode::SETLE_RM8:
    case Opcode::SETNLE_RM8:
    // MOV 16-bit immediate
    case Opcode::MOV_RM16_IMM16:
    // XCHG / CMPXCHG
    case Opcode::XCHG_RM16_R16:
    case Opcode::XCHG_RM32_R32:
    case Opcode::XCHG_RM64_R64:
    case Opcode::CMPXCHG_RM32_R32:
    case Opcode::CMPXCHG_RM16_R16:
    case Opcode::CMPXCHG_RM8_R8:
    case Opcode::CMPXCHG_RM64_R64:
    // XADD
    case Opcode::XADD_RM32_R32:
    case Opcode::XADD_RM16_R16:
    case Opcode::XADD_RM8_R8:
    case Opcode::XADD_RM64_R64:
    // XLAT — AL := [DS:(E/R)BX + AL]
    case Opcode::XLAT:
    // 8-bit ALU
    case Opcode::ADD_RM8_IMM8:
    case Opcode::SUB_RM8_IMM8:
    case Opcode::CMP_RM8_R8:
    case Opcode::CMP_R8_RM8:
    // IMUL single-operand
    case Opcode::IMUL_RM32:
    // TEST r/m32, imm32
    case Opcode::TEST_RM32_IMM32:
    // BT / BTS / BTR / BTC (imm8)
    case Opcode::BT_RM32_IMM8:
    case Opcode::BTS_RM32_IMM8:
    case Opcode::BTR_RM32_IMM8:
    case Opcode::BTC_RM32_IMM8:
    case Opcode::BT_RM16_IMM8:
    case Opcode::BTS_RM16_IMM8:
    case Opcode::BTR_RM16_IMM8:
    case Opcode::BTC_RM16_IMM8:
    // BSF / BSR
    case Opcode::BSF_R16_RM16:
    case Opcode::BSR_R16_RM16:
    case Opcode::BSF_R32_RM32:
    case Opcode::BSR_R32_RM32:
    case Opcode::BSF_R64_RM64:
    case Opcode::BSR_R64_RM64:
    // POPCNT
    case Opcode::POPCNT_R16_RM16:
    case Opcode::POPCNT_R32_RM32:
    case Opcode::POPCNT_R64_RM64:
    // NOP r/m (multi-byte NOP)
    case Opcode::NOP_RM:
    // CMOVcc
    case Opcode::CMOVB_R32_RM32:
    case Opcode::CMOVNB_R32_RM32:
    case Opcode::CMOVZ_R32_RM32:
    case Opcode::CMOVNZ_R32_RM32:
    case Opcode::CMOVBE_R32_RM32:
    case Opcode::CMOVNBE_R32_RM32:
    case Opcode::CMOVS_R32_RM32:
    case Opcode::CMOVNS_R32_RM32:
    case Opcode::CMOVL_R32_RM32:
    case Opcode::CMOVNL_R32_RM32:
    case Opcode::CMOVLE_R32_RM32:
    case Opcode::CMOVNLE_R32_RM32:
    case Opcode::CMOVO_R32_RM32:
    case Opcode::CMOVNO_R32_RM32:
    case Opcode::CMOVP_R32_RM32:
    case Opcode::CMOVNP_R32_RM32:
    case Opcode::CMOVB_R16_RM16:
    case Opcode::CMOVNB_R16_RM16:
    case Opcode::CMOVZ_R16_RM16:
    case Opcode::CMOVNZ_R16_RM16:
    case Opcode::CMOVBE_R16_RM16:
    case Opcode::CMOVNBE_R16_RM16:
    case Opcode::CMOVS_R16_RM16:
    case Opcode::CMOVNS_R16_RM16:
    case Opcode::CMOVL_R16_RM16:
    case Opcode::CMOVNL_R16_RM16:
    case Opcode::CMOVLE_R16_RM16:
    case Opcode::CMOVNLE_R16_RM16:
    case Opcode::CMOVO_R16_RM16:
    case Opcode::CMOVNO_R16_RM16:
    case Opcode::CMOVP_R16_RM16:
    case Opcode::CMOVNP_R16_RM16:
    case Opcode::LZCNT_R32_RM32:
    case Opcode::TZCNT_R32_RM32:
    case Opcode::LZCNT_R64_RM64:
    case Opcode::TZCNT_R64_RM64:
    // BMI2 flag-neutral data ops. ARM64's register shifts take the count from
    // the low 5 (W) / 6 (X) bits of Rm, which is exactly x86's masking for
    // SHLX/SHRX/SARX -- no explicit AND needed.
    case Opcode::SHLX_R32_RM32_R32:
    case Opcode::SHLX_R64_RM64_R64:
    case Opcode::SHRX_R32_RM32_R32:
    case Opcode::SHRX_R64_RM64_R64:
    case Opcode::SARX_R32_RM32_R32:
    case Opcode::SARX_R64_RM64_R64:
    case Opcode::RORX_R32_RM32_IMM8:
    case Opcode::RORX_R64_RM64_IMM8:
    case Opcode::MULX_R32_R32_RM32:
    case Opcode::MULX_R64_R64_RM64:
    // BMI1 flag writers: SF/ZF from the result, CF per instruction, OF clear.
    case Opcode::ANDN_R32_R32_RM32:
    case Opcode::ANDN_R64_R64_RM64:
    case Opcode::BLSI_R32_RM32:
    case Opcode::BLSI_R64_RM64:
    case Opcode::BLSMSK_R32_RM32:
    case Opcode::BLSMSK_R64_RM64:
    case Opcode::BLSR_R32_RM32:
    case Opcode::BLSR_R64_RM64:
    case Opcode::BZHI_R32_RM32_R32:
    case Opcode::BZHI_R64_RM64_R64:
    case Opcode::BEXTR_R32_RM32_R32:
    case Opcode::BEXTR_R64_RM64_R64:
    case Opcode::MOVBE_R16_M16:
    case Opcode::MOVBE_R32_M32:
    case Opcode::MOVBE_R64_M64:
    case Opcode::MOVBE_M16_R16:
    case Opcode::MOVBE_M32_R32:
    case Opcode::MOVBE_M64_R64:
    case Opcode::ADCX_R32_RM32:
    case Opcode::ADCX_R64_RM64:
    case Opcode::ADOX_R32_RM32:
    case Opcode::ADOX_R64_RM64:
    case Opcode::PDEP_R32_R32_RM32:
    case Opcode::PDEP_R64_R64_RM64:
    case Opcode::PEXT_R32_R32_RM32:
    case Opcode::PEXT_R64_R64_RM64:
    case Opcode::VFMADD132PS_XMM_XMM_XMMM128:
    case Opcode::VFMADD132PD_XMM_XMM_XMMM128:
    case Opcode::VFMADD132SS_XMM_XMM_XMMM32:
    case Opcode::VFMADD132SD_XMM_XMM_XMMM64:
    case Opcode::VFMADD213PS_XMM_XMM_XMMM128:
    case Opcode::VFMADD213PD_XMM_XMM_XMMM128:
    case Opcode::VFMADD213SS_XMM_XMM_XMMM32:
    case Opcode::VFMADD213SD_XMM_XMM_XMMM64:
    case Opcode::VFMADD231PS_XMM_XMM_XMMM128:
    case Opcode::VFMADD231PD_XMM_XMM_XMMM128:
    case Opcode::VFMADD231SS_XMM_XMM_XMMM32:
    case Opcode::VFMADD231SD_XMM_XMM_XMMM64:
    case Opcode::VFMSUB132PS_XMM_XMM_XMMM128:
    case Opcode::VFMSUB132PD_XMM_XMM_XMMM128:
    case Opcode::VFMSUB132SS_XMM_XMM_XMMM32:
    case Opcode::VFMSUB132SD_XMM_XMM_XMMM64:
    case Opcode::VFMSUB213PS_XMM_XMM_XMMM128:
    case Opcode::VFMSUB213PD_XMM_XMM_XMMM128:
    case Opcode::VFMSUB213SS_XMM_XMM_XMMM32:
    case Opcode::VFMSUB213SD_XMM_XMM_XMMM64:
    case Opcode::VFMSUB231PS_XMM_XMM_XMMM128:
    case Opcode::VFMSUB231PD_XMM_XMM_XMMM128:
    case Opcode::VFMSUB231SS_XMM_XMM_XMMM32:
    case Opcode::VFMSUB231SD_XMM_XMM_XMMM64:
    case Opcode::VFNMADD132PS_XMM_XMM_XMMM128:
    case Opcode::VFNMADD132PD_XMM_XMM_XMMM128:
    case Opcode::VFNMADD132SS_XMM_XMM_XMMM32:
    case Opcode::VFNMADD132SD_XMM_XMM_XMMM64:
    case Opcode::VFNMADD213PS_XMM_XMM_XMMM128:
    case Opcode::VFNMADD213PD_XMM_XMM_XMMM128:
    case Opcode::VFNMADD213SS_XMM_XMM_XMMM32:
    case Opcode::VFNMADD213SD_XMM_XMM_XMMM64:
    case Opcode::VFNMADD231PS_XMM_XMM_XMMM128:
    case Opcode::VFNMADD231PD_XMM_XMM_XMMM128:
    case Opcode::VFNMADD231SS_XMM_XMM_XMMM32:
    case Opcode::VFNMADD231SD_XMM_XMM_XMMM64:
    case Opcode::VFNMSUB132PS_XMM_XMM_XMMM128:
    case Opcode::VFNMSUB132PD_XMM_XMM_XMMM128:
    case Opcode::VFNMSUB132SS_XMM_XMM_XMMM32:
    case Opcode::VFNMSUB132SD_XMM_XMM_XMMM64:
    case Opcode::VFNMSUB213PS_XMM_XMM_XMMM128:
    case Opcode::VFNMSUB213PD_XMM_XMM_XMMM128:
    case Opcode::VFNMSUB213SS_XMM_XMM_XMMM32:
    case Opcode::VFNMSUB213SD_XMM_XMM_XMMM64:
    case Opcode::VFNMSUB231PS_XMM_XMM_XMMM128:
    case Opcode::VFNMSUB231PD_XMM_XMM_XMMM128:
    case Opcode::VFNMSUB231SS_XMM_XMM_XMMM32:
    case Opcode::VFNMSUB231SD_XMM_XMM_XMMM64:
    case Opcode::VFMADDSUB132PS_XMM_XMM_XMMM128:
    case Opcode::VFMADDSUB132PD_XMM_XMM_XMMM128:
    case Opcode::VFMADDSUB213PS_XMM_XMM_XMMM128:
    case Opcode::VFMADDSUB213PD_XMM_XMM_XMMM128:
    case Opcode::VFMADDSUB231PS_XMM_XMM_XMMM128:
    case Opcode::VFMADDSUB231PD_XMM_XMM_XMMM128:
    case Opcode::VFMSUBADD132PS_XMM_XMM_XMMM128:
    case Opcode::VFMSUBADD132PD_XMM_XMM_XMMM128:
    case Opcode::VFMSUBADD213PS_XMM_XMM_XMMM128:
    case Opcode::VFMSUBADD213PD_XMM_XMM_XMMM128:
    case Opcode::VFMSUBADD231PS_XMM_XMM_XMMM128:
    case Opcode::VFMSUBADD231PD_XMM_XMM_XMMM128:
    case Opcode::VGATHERDPS_XMM_VM32X_XMM:
    case Opcode::VGATHERDPS_YMM_VM32Y_YMM:
    case Opcode::VPGATHERDD_XMM_VM32X_XMM:
    case Opcode::VPGATHERDD_YMM_VM32Y_YMM:
    case Opcode::VGATHERQPS:
    case Opcode::VPGATHERQD:
    case Opcode::VGATHERDPD:
    case Opcode::VPGATHERDQ:
    case Opcode::VGATHERQPD:
    case Opcode::VPGATHERQQ:
    case Opcode::VMASKMOVPS_LOAD:
    case Opcode::VMASKMOVPS_STORE:
    case Opcode::VMASKMOVPD_LOAD:
    case Opcode::VMASKMOVPD_STORE:
    case Opcode::VPMASKMOVD_LOAD:
    case Opcode::VPMASKMOVD_STORE:
    case Opcode::VPMASKMOVQ_LOAD:
    case Opcode::VPMASKMOVQ_STORE:
    case Opcode::VTESTPS:
    case Opcode::VTESTPD:
    case Opcode::VPERMD_YMM_YMM_YMMM256:
    case Opcode::VPERMPS_YMM_YMM_YMMM256:
    case Opcode::VPERMILPS_VAR:
    case Opcode::VPERMILPD_VAR:
    case Opcode::VPERMILPS_IMM:
    case Opcode::VPERMILPD_IMM:
    case Opcode::VPBLENDVB_XMM_XMM_XMMM128_XMM:
    case Opcode::VPBLENDVB_YMM_YMM_YMMM256_YMM:
    case Opcode::VBLENDVPS_XMM_XMM_XMMM128_XMM:
    case Opcode::VBLENDVPS_YMM_YMM_YMMM256_YMM:
    case Opcode::VBLENDVPD_XMM_XMM_XMMM128_XMM:
    case Opcode::VBLENDVPD_YMM_YMM_YMMM256_YMM:
    case Opcode::VPSLLVD_XMM_XMM_XMMM128:
    case Opcode::VPSLLVD_YMM_YMM_YMMM256:
    case Opcode::VPSLLVQ_XMM_XMM_XMMM128:
    case Opcode::VPSLLVQ_YMM_YMM_YMMM256:
    case Opcode::VPSRLVD_XMM_XMM_XMMM128:
    case Opcode::VPSRLVD_YMM_YMM_YMMM256:
    case Opcode::VPSRLVQ_XMM_XMM_XMMM128:
    case Opcode::VPSRLVQ_YMM_YMM_YMMM256:
    case Opcode::VPSRAVD_XMM_XMM_XMMM128:
    case Opcode::VPSRAVD_YMM_YMM_YMMM256:
    case Opcode::VPBLENDD_XMM_XMM_XMMM128_IMM8:
    case Opcode::VPBLENDD_YMM_YMM_YMMM256_IMM8:
    case Opcode::VPERM2F128_YMM_YMM_YMMM256_IMM8:
    case Opcode::VPERM2I128_YMM_YMM_YMMM256_IMM8:
    case Opcode::VPERMQ_YMM_YMMM256_IMM8:
    case Opcode::VPERMPD_YMM_YMMM256_IMM8:
    case Opcode::VEXTRACTF128_XMMM128_YMM_IMM8:
    case Opcode::VEXTRACTI128_XMMM128_YMM_IMM8:
    case Opcode::VINSERTF128_YMM_YMM_XMMM128_IMM8:
    case Opcode::VINSERTI128_YMM_YMM_XMMM128_IMM8:
    case Opcode::VZEROUPPER:
    case Opcode::VZEROALL:
    case Opcode::VPBROADCASTB_XMM_XMMM8:
    case Opcode::VPBROADCASTB_YMM_XMMM8:
    case Opcode::VPBROADCASTW_XMM_XMMM16:
    case Opcode::VPBROADCASTW_YMM_XMMM16:
    case Opcode::VPBROADCASTD_XMM_XMMM32:
    case Opcode::VPBROADCASTD_YMM_XMMM32:
    case Opcode::VPBROADCASTQ_XMM_XMMM64:
    case Opcode::VPBROADCASTQ_YMM_XMMM64:
    case Opcode::VBROADCASTSS_XMM_XMM:
    case Opcode::VBROADCASTSS_YMM_XMM:
    case Opcode::VBROADCASTSD_YMM_XMM:
    case Opcode::VCVTPH2PS_XMM_XMMM64:
    case Opcode::VCVTPH2PS_YMM_XMMM128:
    case Opcode::BT_RM32_R32:
    case Opcode::BTS_RM32_R32:
    case Opcode::BTR_RM32_R32:
    case Opcode::BTC_RM32_R32:
    case Opcode::BT_RM16_R16:
    case Opcode::BTS_RM16_R16:
    case Opcode::BTR_RM16_R16:
    case Opcode::BTC_RM16_R16:
    case Opcode::BT_RM64_R64:
    case Opcode::BTS_RM64_R64:
    case Opcode::BTR_RM64_R64:
    case Opcode::BTC_RM64_R64:
    case Opcode::BT_RM64_IMM8:
    case Opcode::BTS_RM64_IMM8:
    case Opcode::BTR_RM64_IMM8:
    case Opcode::BTC_RM64_IMM8:
    case Opcode::STOSB:
    case Opcode::STOSW:
    case Opcode::LODSB:
    case Opcode::LODSW:
    case Opcode::LODSD:
    case Opcode::SCASB:
    case Opcode::SCASW:
    case Opcode::SCASD:
    case Opcode::MOVSW:
    case Opcode::CMPSB:
    case Opcode::CMPSW:
    case Opcode::CMPSD_STR:
    case Opcode::SHLD_RM32_R32_CL:
    case Opcode::SHRD_RM32_R32_CL:
    case Opcode::SHLD_RM16_R16_CL:
    case Opcode::SHRD_RM16_R16_CL:
    // SSE data movement
    case Opcode::MOVAPS_XMM_XMMM128:
    case Opcode::MOVAPD_XMM_XMMM128:
    case Opcode::MOVUPS_XMM_XMMM128:
    case Opcode::MOVUPD_XMM_XMMM128:
    case Opcode::MOVDQU_XMM_XMMM128:
    case Opcode::LDDQU_XMM_M128:
    case Opcode::MOVLPS_XMM_M64:
    case Opcode::MOVLPS_M64_XMM:
    case Opcode::MOVLPD_XMM_M64:
    case Opcode::MOVLPD_M64_XMM:
    case Opcode::MOVLHPS_XMM_XMM:
    case Opcode::MOVHLPS_XMM_XMM:
    case Opcode::MOVSD_XMM_XMMM64:
    case Opcode::MOVSD_XMMM64_XMM:
    case Opcode::MOVSS_XMMM32_XMM:
    // SSE data store
    case Opcode::MOVUPS_XMMM128_XMM:
    case Opcode::MOVUPD_XMMM128_XMM:
    case Opcode::MOVDQU_XMMM128_XMM:
    case Opcode::MOVAPS_XMMM128_XMM:
    case Opcode::MOVAPD_XMMM128_XMM:
    case Opcode::MOVDQA_XMMM128_XMM:
    case Opcode::MOVNTPS_M128_XMM:
    case Opcode::MOVNTPD_M128_XMM:
    case Opcode::MOVNTDQ_M128_XMM:
    case Opcode::MASKMOVDQU_XMM_XMM:
    case Opcode::MOVNTQ_M64_MM:
    case Opcode::MOVQ2DQ_XMM_MM:
    case Opcode::MOVDQ2Q_MM_XMM:
    case Opcode::CVTDQ2PD_XMM_XMMM64:
    case Opcode::CVTPD2DQ_XMM_XMMM128:
    case Opcode::CVTTPD2DQ_XMM_XMMM128:
    case Opcode::CVTPD2PS_XMM_XMMM128:
    case Opcode::CVTPI2PS_XMM_MMM64:
    case Opcode::CVTPI2PD_XMM_MMM64:
    case Opcode::CVTPS2PI_MM_XMMM64:
    case Opcode::CVTTPS2PI_MM_XMMM64:
    case Opcode::CVTPD2PI_MM_XMMM128:
    case Opcode::CVTTPD2PI_MM_XMMM128:
    case Opcode::PMADDUBSW_XMM_XMMM128:
    // MOVDQA load
    case Opcode::MOVDQA_XMM_XMMM128:
    // SSE FP bitwise
    case Opcode::XORPS_XMM_XMMM128:
    case Opcode::XORPD_XMM_XMMM128:
    case Opcode::ANDPS_XMM_XMMM128:
    case Opcode::ANDPD_XMM_XMMM128:
    case Opcode::ORPS_XMM_XMMM128:
    case Opcode::ORPD_XMM_XMMM128:
    case Opcode::ANDNPS_XMM_XMMM128:
    case Opcode::ANDNPD_XMM_XMMM128:
    // SSE conversion
    case Opcode::CVTPS2PD_XMM_XMMM64:
    // SSE2 integer data movement
    case Opcode::MOVD_XMM_RM32:
    case Opcode::MOVD_RM32_XMM:
    case Opcode::MOVD_MM_RM32:
    case Opcode::MOVD_RM32_MM:
    case Opcode::PMULUDQ_MM_MMM64:
    case Opcode::MOVQ_XMM_RM64:
    case Opcode::MOVQ_RM64_XMM:
    case Opcode::MOVQ_XMM_XMMM64:
    case Opcode::MOVQ_XMMM64_XMM:
    case Opcode::PMOVMSKB_R32_XMM:
    // SSE2 integer bitwise
    case Opcode::PXOR_XMM_XMMM128:
    case Opcode::PAND_XMM_XMMM128:
    case Opcode::POR_XMM_XMMM128:
    case Opcode::PANDN_XMM_XMMM128:
    // SSE/SSE2 packed FP arithmetic (reg-reg → native NEON; mem still helper)
    case Opcode::ADDPS_XMM_XMMM128:
    case Opcode::SUBPS_XMM_XMMM128:
    case Opcode::MULPS_XMM_XMMM128:
    case Opcode::DIVPS_XMM_XMMM128:
    case Opcode::MINPS_XMM_XMMM128:
    case Opcode::MAXPS_XMM_XMMM128:
    case Opcode::SQRTPS_XMM_XMMM128:
    case Opcode::ADDPD_XMM_XMMM128:
    case Opcode::SUBPD_XMM_XMMM128:
    case Opcode::MULPD_XMM_XMMM128:
    case Opcode::DIVPD_XMM_XMMM128:
    case Opcode::MINPD_XMM_XMMM128:
    case Opcode::MAXPD_XMM_XMMM128:
    case Opcode::SQRTPD_XMM_XMMM128:
    // Scalar FP (SS/SD): reg-reg native NEON + helper fallback for mem
    case Opcode::ADDSS_XMM_XMMM32:
    case Opcode::ADDSD_XMM_XMMM64:
    case Opcode::SUBSS_XMM_XMMM32:
    case Opcode::SUBSD_XMM_XMMM64:
    case Opcode::MULSS_XMM_XMMM32:
    case Opcode::MULSD_XMM_XMMM64:
    case Opcode::DIVSS_XMM_XMMM32:
    case Opcode::DIVSD_XMM_XMMM64:
    case Opcode::MINSS_XMM_XMMM32:
    case Opcode::MINSD_XMM_XMMM64:
    case Opcode::MAXSS_XMM_XMMM32:
    case Opcode::MAXSD_XMM_XMMM64:
    case Opcode::SQRTSS_XMM_XMMM32:
    case Opcode::SQRTSD_XMM_XMMM64:
    // SSE2 integer arithmetic
    case Opcode::PADDB_XMM_XMMM128:
    case Opcode::PADDW_XMM_XMMM128:
    case Opcode::PADDD_XMM_XMMM128:
    case Opcode::PADDQ_XMM_XMMM128:
    case Opcode::PSUBB_XMM_XMMM128:
    case Opcode::PSUBW_XMM_XMMM128:
    case Opcode::PSUBD_XMM_XMMM128:
    case Opcode::PSUBQ_XMM_XMMM128:
    case Opcode::PMULUDQ_XMM_XMMM128:
    case Opcode::PMULLW_XMM_XMMM128:
    case Opcode::PMULHW_XMM_XMMM128:
    case Opcode::PMULHUW_XMM_XMMM128:
    case Opcode::PMADDWD_XMM_XMMM128:
    // SSE2 integer comparison
    case Opcode::PCMPEQB_XMM_XMMM128:
    case Opcode::PCMPEQW_XMM_XMMM128:
    case Opcode::PCMPEQD_XMM_XMMM128:
    case Opcode::PCMPGTB_XMM_XMMM128:
    case Opcode::PCMPGTW_XMM_XMMM128:
    case Opcode::PCMPGTD_XMM_XMMM128:
    // SSE4.1/4.2 integer compare/multiply/min-max (all go native NEON)
    case Opcode::PCMPEQQ_XMM_XMMM128:
    case Opcode::PCMPGTQ_XMM_XMMM128:
    case Opcode::PMULLD_XMM_XMMM128:
    case Opcode::PMINSB_XMM_XMMM128:
    case Opcode::PMINSD_XMM_XMMM128:
    case Opcode::PMINUW_XMM_XMMM128:
    case Opcode::PMINUD_XMM_XMMM128:
    case Opcode::PMAXSB_XMM_XMMM128:
    case Opcode::PMAXSD_XMM_XMMM128:
    case Opcode::PMAXUW_XMM_XMMM128:
    case Opcode::PMAXUD_XMM_XMMM128:
    // SSE4.1 PMOVSX / PMOVZX (native NEON via SXTL/UXTL chain)
    case Opcode::PMOVSXBW_XMM_XMMM64:
    case Opcode::PMOVSXBD_XMM_XMMM32:
    case Opcode::PMOVSXBQ_XMM_XMMM16:
    case Opcode::PMOVSXWD_XMM_XMMM64:
    case Opcode::PMOVSXWQ_XMM_XMMM32:
    case Opcode::PMOVSXDQ_XMM_XMMM64:
    case Opcode::PMOVZXBW_XMM_XMMM64:
    case Opcode::PMOVZXBD_XMM_XMMM32:
    case Opcode::PMOVZXBQ_XMM_XMMM16:
    case Opcode::PMOVZXWD_XMM_XMMM64:
    case Opcode::PMOVZXWQ_XMM_XMMM32:
    case Opcode::PMOVZXDQ_XMM_XMMM64:
    // SSE2 integer min/max/avg/sad
    case Opcode::PMINUB_XMM_XMMM128:
    case Opcode::PMAXUB_XMM_XMMM128:
    case Opcode::PMINSW_XMM_XMMM128:
    case Opcode::PMAXSW_XMM_XMMM128:
    case Opcode::PAVGB_XMM_XMMM128:
    case Opcode::PAVGW_XMM_XMMM128:
    case Opcode::PSADBW_XMM_XMMM128:
    // SSE2 integer saturating add/sub
    case Opcode::PADDUSB_XMM_XMMM128:
    case Opcode::PADDUSW_XMM_XMMM128:
    case Opcode::PSUBUSB_XMM_XMMM128:
    case Opcode::PSUBUSW_XMM_XMMM128:
    // SSE2 packed shifts by imm8
    case Opcode::PSLLW_XMM_IMM8:
    case Opcode::PSLLD_XMM_IMM8:
    case Opcode::PSLLQ_XMM_IMM8:
    case Opcode::PSLLDQ_XMM_IMM8:
    case Opcode::PSRLW_XMM_IMM8:
    case Opcode::PSRLD_XMM_IMM8:
    case Opcode::PSRLQ_XMM_IMM8:
    case Opcode::PSRLDQ_XMM_IMM8:
    case Opcode::PSRAW_XMM_IMM8:
    case Opcode::PSRAD_XMM_IMM8:
    // SSE2 packed shifts by register
    case Opcode::PSLLW_XMM_XMMM128:
    case Opcode::PSLLD_XMM_XMMM128:
    case Opcode::PSLLQ_XMM_XMMM128:
    case Opcode::PSRLW_XMM_XMMM128:
    case Opcode::PSRLD_XMM_XMMM128:
    case Opcode::PSRLQ_XMM_XMMM128:
    case Opcode::PSRAW_XMM_XMMM128:
    case Opcode::PSRAD_XMM_XMMM128:
    // SSE shuffle/unpack
    case Opcode::PSHUFD_XMM_XMMM128_IMM8:
    case Opcode::PSHUFLW_XMM_XMMM128_IMM8:
    case Opcode::PSHUFHW_XMM_XMMM128_IMM8:
    case Opcode::SHUFPS_XMM_XMMM128_IMM8:
    case Opcode::SHUFPD_XMM_XMMM128_IMM8:
    case Opcode::PUNPCKLBW_XMM_XMMM128:
    case Opcode::PUNPCKHBW_XMM_XMMM128:
    case Opcode::PUNPCKLWD_XMM_XMMM128:
    case Opcode::PUNPCKHWD_XMM_XMMM128:
    case Opcode::PUNPCKLDQ_XMM_XMMM128:
    case Opcode::PUNPCKHDQ_XMM_XMMM128:
    case Opcode::PUNPCKLQDQ_XMM_XMMM128:
    case Opcode::PUNPCKHQDQ_XMM_XMMM128:
    case Opcode::UNPCKLPS_XMM_XMMM128:
    case Opcode::UNPCKHPS_XMM_XMMM128:
    case Opcode::UNPCKLPD_XMM_XMMM128:
    case Opcode::UNPCKHPD_XMM_XMMM128:
    // SSE2 insert/extract
    case Opcode::PINSRW_XMM_R32M16_IMM8:
    case Opcode::PEXTRW_R32_XMM_IMM8:
    // SHLD
    case Opcode::SHLD_RM32_R32_IMM8:
    case Opcode::SHLD_RM16_R16_IMM8:
    // MOVSS
    case Opcode::MOVSS_XMM_XMMM32:
    // AES-NI
    case Opcode::AESENC_XMM_XMMM128:
    case Opcode::AESENCLAST_XMM_XMMM128:
    case Opcode::AESDEC_XMM_XMMM128:
    case Opcode::AESDECLAST_XMM_XMMM128:
    case Opcode::AESIMC_XMM_XMMM128:
    case Opcode::AESKEYGENASSIST_XMM_XMMM128_IMM8:
    // SSE4.2 CRC32C -> ARMv8 CRC32C
    case Opcode::CRC32_R32_RM8:
    case Opcode::CRC32_R64_RM8:
    case Opcode::CRC32_R32_RM16:
    case Opcode::CRC32_R32_RM32:
    case Opcode::CRC32_R64_RM64:
    // PCLMULQDQ
    case Opcode::PCLMULQDQ_XMM_XMMM128_IMM8:
    // SSSE3 PSHUFB
    case Opcode::PSHUFB_XMM_XMMM128:
    // SHA extensions
    case Opcode::SHA256RNDS2_XMM_XMMM128:
    case Opcode::SHA256MSG1_XMM_XMMM128:
    case Opcode::SHA256MSG2_XMM_XMMM128:
    case Opcode::SHA1NEXTE_XMM_XMMM128:
    case Opcode::SHA1MSG1_XMM_XMMM128:
    case Opcode::SHA1MSG2_XMM_XMMM128:
    case Opcode::SHA1RNDS4_XMM_XMMM128_IMM8:
    // Native migrations (BSWAP / flag ops / LAHF / SAHF)
    case Opcode::BSWAP_R32:
    case Opcode::BSWAP_R64:
    case Opcode::CLC:
    case Opcode::STC:
    case Opcode::CMC:
    case Opcode::CLD:
    case Opcode::STD:
    case Opcode::LAHF:
    case Opcode::SAHF:
    // Native migrations (SSE3 dup / SSSE3 horiz / SSE3 HSUB / pack)
    case Opcode::MOVSHDUP_XMM_XMMM128:
    case Opcode::MOVSLDUP_XMM_XMMM128:
    case Opcode::PHSUBW_XMM_XMMM128:
    case Opcode::PHSUBD_XMM_XMMM128:
    case Opcode::PHADDSW_XMM_XMMM128:
    case Opcode::PHSUBSW_XMM_XMMM128:
    case Opcode::HSUBPS_XMM_XMMM128:
    case Opcode::HSUBPD_XMM_XMMM128:
    case Opcode::PACKSSWB_XMM_XMMM128:
    case Opcode::PACKSSDW_XMM_XMMM128:
    case Opcode::PACKUSWB_XMM_XMMM128:
    case Opcode::PACKUSDW_XMM_XMMM128:
    // Previously-written SSE cases that had never been whitelisted
    case Opcode::HADDPD_XMM_XMMM128:
    case Opcode::HADDPS_XMM_XMMM128:
    case Opcode::MOVDDUP_XMM_XMMM64:
    case Opcode::PABSB_XMM_XMMM128:
    case Opcode::PABSD_XMM_XMMM128:
    case Opcode::PABSW_XMM_XMMM128:
    case Opcode::PADDSB_XMM_XMMM128:
    case Opcode::PADDSW_XMM_XMMM128:
    case Opcode::PHADDD_XMM_XMMM128:
    case Opcode::PHADDW_XMM_XMMM128:
    case Opcode::PSUBSB_XMM_XMMM128:
    case Opcode::PSUBSW_XMM_XMMM128:
    case Opcode::SHRD_RM32_R32_IMM8:
    case Opcode::SHRD_RM16_R16_IMM8:
    // Newer NEON-direct SSE migrations
    case Opcode::PSIGNB_XMM_XMMM128:
    case Opcode::PSIGNW_XMM_XMMM128:
    case Opcode::PSIGND_XMM_XMMM128:
    case Opcode::PMULDQ_XMM_XMMM128:
    case Opcode::ADDSUBPS_XMM_XMMM128:
    case Opcode::ADDSUBPD_XMM_XMMM128:
    case Opcode::RCPPS_XMM_XMMM128:
    case Opcode::RSQRTPS_XMM_XMMM128:
    case Opcode::ROUNDPS_XMM_XMMM128_IMM8:
    case Opcode::ROUNDPD_XMM_XMMM128_IMM8:
    case Opcode::ROUNDSS_XMM_XMMM32_IMM8:
    case Opcode::ROUNDSD_XMM_XMMM64_IMM8:
    case Opcode::MOVMSKPS_R32_XMM:
    case Opcode::MOVMSKPD_R32_XMM:
    case Opcode::PALIGNR_XMM_XMMM128_IMM8:
    case Opcode::BLENDPS_XMM_XMMM128_IMM8:
    case Opcode::BLENDPD_XMM_XMMM128_IMM8:
    case Opcode::PBLENDW_XMM_XMMM128_IMM8:
    case Opcode::PBLENDVB_XMM_XMMM128:
    case Opcode::BLENDVPS_XMM_XMMM128:
    case Opcode::BLENDVPD_XMM_XMMM128:
    case Opcode::PCMPESTRM_XMM_XMMM128_IMM8:
    case Opcode::PCMPESTRI_XMM_XMMM128_IMM8:
    case Opcode::PCMPISTRM_XMM_XMMM128_IMM8:
    case Opcode::PCMPISTRI_XMM_XMMM128_IMM8:
    case Opcode::PHMINPOSUW_XMM_XMMM128:
    case Opcode::MOVNTDQA_XMM_M128:
    case Opcode::DPPS_XMM_XMMM128_IMM8:
    case Opcode::DPPD_XMM_XMMM128_IMM8:
    case Opcode::MPSADBW_XMM_XMMM128_IMM8:
    case Opcode::EXTRACTPS_RM32_XMM_IMM8:
    case Opcode::PEXTRD_RM32_XMM_IMM8:
    case Opcode::PEXTRQ_RM64_XMM_IMM8:
    case Opcode::PEXTRB_RM8_XMM_IMM8:
    case Opcode::PINSRD_XMM_RM32_IMM8:
    case Opcode::PINSRQ_XMM_RM64_IMM8:
    case Opcode::PINSRB_XMM_RM8_IMM8:
    case Opcode::INSERTPS_XMM_XMMM32_IMM8:
    case Opcode::CMPPS_XMM_XMMM128_IMM8:
    case Opcode::CMPPD_XMM_XMMM128_IMM8:
    case Opcode::CMPSS_XMM_XMMM32_IMM8:
    case Opcode::CMPSD_XMM_XMMM64_IMM8:
    case Opcode::COMISS_XMM_XMMM32:
    case Opcode::UCOMISS_XMM_XMMM32:
    case Opcode::COMISD_XMM_XMMM64:
    case Opcode::UCOMISD_XMM_XMMM64:
    case Opcode::PTEST_XMM_XMMM128:
    // MMX — all compile_mmx native cases (PE32 needs has_stencil so the
    // early compile_mmx path inside has_native is taken; the no-stencil
    // fallback still exists, but whitelist keeps coverage + 64-bit consistent).
    case Opcode::MOVQ_MM_MMM64:
    case Opcode::MOVQ_MMM64_MM:
    case Opcode::PADDB_MM_MMM64:
    case Opcode::PADDW_MM_MMM64:
    case Opcode::PADDD_MM_MMM64:
    case Opcode::PADDQ_MM_MMM64:
    case Opcode::PSUBB_MM_MMM64:
    case Opcode::PSUBW_MM_MMM64:
    case Opcode::PSUBD_MM_MMM64:
    case Opcode::PSUBQ_MM_MMM64:
    case Opcode::PMULLW_MM_MMM64:
    case Opcode::PMULHW_MM_MMM64:
    case Opcode::PMULHUW_MM_MMM64:
    case Opcode::PAND_MM_MMM64:
    case Opcode::PANDN_MM_MMM64:
    case Opcode::POR_MM_MMM64:
    case Opcode::PXOR_MM_MMM64:
    case Opcode::PCMPEQB_MM_MMM64:
    case Opcode::PCMPEQW_MM_MMM64:
    case Opcode::PCMPEQD_MM_MMM64:
    case Opcode::PCMPGTB_MM_MMM64:
    case Opcode::PCMPGTW_MM_MMM64:
    case Opcode::PCMPGTD_MM_MMM64:
    case Opcode::PUNPCKLBW_MM_MMM64:
    case Opcode::PUNPCKLWD_MM_MMM64:
    case Opcode::PUNPCKLDQ_MM_MMM64:
    case Opcode::PUNPCKHBW_MM_MMM64:
    case Opcode::PUNPCKHWD_MM_MMM64:
    case Opcode::PUNPCKHDQ_MM_MMM64:
    case Opcode::PUNPCKHQDQ_MM_MMM64:
    case Opcode::PACKUSWB_MM_MMM64:
    case Opcode::PACKSSWB_MM_MMM64:
    case Opcode::PACKSSDW_MM_MMM64:
    case Opcode::PSRLQ_MM_IMM8:
    case Opcode::PSLLQ_MM_IMM8:
    case Opcode::PSRLQ_MM_MMM64:
    case Opcode::PSLLQ_MM_MMM64:
    case Opcode::PSRLW_MM_IMM8:
    case Opcode::PSRAW_MM_IMM8:
    case Opcode::PSLLW_MM_IMM8:
    case Opcode::PSRLD_MM_IMM8:
    case Opcode::PSRAD_MM_IMM8:
    case Opcode::PSLLD_MM_IMM8:
    case Opcode::PSRLW_MM_MMM64:
    case Opcode::PSRAW_MM_MMM64:
    case Opcode::PSLLW_MM_MMM64:
    case Opcode::PSRLD_MM_MMM64:
    case Opcode::PSRAD_MM_MMM64:
    case Opcode::PSLLD_MM_MMM64:
    case Opcode::PMAXSW_MM_MMM64:
    case Opcode::PMINSW_MM_MMM64:
    case Opcode::PADDUSB_MM_MMM64:
    case Opcode::PADDUSW_MM_MMM64:
    case Opcode::PSUBUSB_MM_MMM64:
    case Opcode::PSUBUSW_MM_MMM64:
    case Opcode::PADDSB_MM_MMM64:
    case Opcode::PADDSW_MM_MMM64:
    case Opcode::PSUBSB_MM_MMM64:
    case Opcode::PSUBSW_MM_MMM64:
    case Opcode::PMINUB_MM_MMM64:
    case Opcode::PMAXUB_MM_MMM64:
    case Opcode::PAVGB_MM_MMM64:
    case Opcode::PAVGW_MM_MMM64:
    case Opcode::PSADBW_MM_MMM64:
    case Opcode::PMADDWD_MM_MMM64:
    case Opcode::PSHUFW_MM_MMM64_IMM8:
    case Opcode::PMOVMSKB_R32_MM:
    case Opcode::PINSRW_MM_R32M16_IMM8:
    case Opcode::PEXTRW_R32_MM_IMM8:
    case Opcode::PSHUFB_MM_MMM64:
    case Opcode::PHADDW_MM_MMM64:
    case Opcode::PHADDD_MM_MMM64:
    case Opcode::PHADDSW_MM_MMM64:
    case Opcode::PHSUBW_MM_MMM64:
    case Opcode::PHSUBD_MM_MMM64:
    case Opcode::PHSUBSW_MM_MMM64:
    case Opcode::PABSB_MM_MMM64:
    case Opcode::PABSW_MM_MMM64:
    case Opcode::PABSD_MM_MMM64:
    case Opcode::PSIGNB_MM_MMM64:
    case Opcode::PSIGNW_MM_MMM64:
    case Opcode::PSIGND_MM_MMM64:
    case Opcode::PMADDUBSW_MM_MMM64:
    case Opcode::PMULHRSW_MM_MMM64:
    case Opcode::PMULHRSW_XMM_XMMM128:
    case Opcode::PALIGNR_MM_MMM64_IMM8:
    case Opcode::MASKMOVQ_MM_MM:
    // SSE converts / high-quad moves already lowered in compile_sse
    case Opcode::CVTSI2SS_XMM_RM32:
    case Opcode::CVTSI2SD_XMM_RM32:
    case Opcode::CVTSS2SI_R32_XMMM32:
    case Opcode::CVTSD2SI_R32_XMMM64:
    case Opcode::CVTTSS2SI_R32_XMMM32:
    case Opcode::CVTTSD2SI_R32_XMMM64:
    case Opcode::CVTSS2SD_XMM_XMMM32:
    case Opcode::CVTSD2SS_XMM_XMMM64:
    case Opcode::CVTPS2DQ_XMM_XMMM128:
    case Opcode::CVTDQ2PS_XMM_XMMM128:
    case Opcode::CVTTPS2DQ_XMM_XMMM128:
    case Opcode::RCPSS_XMM_XMMM32:
    case Opcode::RSQRTSS_XMM_XMMM32:
    case Opcode::MOVHPS_XMM_M64:
    case Opcode::MOVHPS_M64_XMM:
    case Opcode::MOVHPD_XMM_M64:
    case Opcode::MOVHPD_M64_XMM:
        return true;
    default:
        return false;
    }
}

} // anonymous namespace

const Stencil* get_stencil(Opcode /*opcode*/) noexcept {
    // In copy-and-patch mode, we emit ARM64 directly in JitCompiler
    // rather than using pre-built stencil byte arrays. This function
    // exists for future use with pre-compiled stencils.
    return nullptr;
}

bool has_stencil(Opcode opcode) noexcept {
    // Historically an opcode ALLOWLIST consulted before any compiler ran: an
    // instruction missing from it could never reach a lowering, however
    // complete that lowering was. That is a reachability decision made by a
    // hand-maintained list rather than by the code that knows the answer, and
    // it is one of the ways a correct lowering silently stops being used.
    // The family compilers already decide: each returns false for an opcode it
    // does not claim, and the chain falls through to the interpreter delegate.
    // The list is therefore redundant at best and wrong at worst, so the gate
    // is open and the compilers are authoritative.
    //
    // `is_jitable` is kept, unused by the gate, only as the bisection switch
    // VITREO_JIT_STENCIL_ALLOWLIST=1 restores — so a regression suspected here
    // can be confirmed in one run rather than by reverting.
    static const bool allowlist =
        (std::getenv("VITREO_JIT_STENCIL_ALLOWLIST") != nullptr);
    if (allowlist) return is_jitable(opcode);
    return true;
}


// ---------------------------------------------------------------------------
// FEAT_FlagM host gate
// ---------------------------------------------------------------------------

bool host_supports_flagm() noexcept {
    // VITREO_JIT_NO_FLAGM=1 forces the pre-FlagM lowering. The env check is
    // deliberately NOT cached: the parity tests flip it inside a single
    // shared-process suite to cover both lowerings on the same machine, and
    // an opt-out nothing can exercise is an opt-out that is wrong.
    if (std::getenv("VITREO_JIT_NO_FLAGM") != nullptr) return false;
    static const bool s_supported = [] {
        int value = 0;
        size_t size = sizeof(value);
        if (::sysctlbyname("hw.optional.arm.FEAT_FlagM", &value, &size,
                           nullptr, 0) != 0) {
            return false;   // absent key == not advertised == do not use
        }
        return value != 0;
    }();
    return s_supported;
}


// ---------------------------------------------------------------------------
// FEAT_LRCPC host gate
// ---------------------------------------------------------------------------

bool host_supports_lrcpc() noexcept {
    // VITREO_JIT_NO_LRCPC=1 forces LDAR in place of LDAPR, so the TSO parity
    // tests can cover both acquire lowerings on the same machine. Uncached
    // for the same reason as the FlagM gate: an opt-out nothing can exercise
    // is an opt-out that is wrong.
    if (std::getenv("VITREO_JIT_NO_LRCPC") != nullptr) return false;
    static const bool s_supported = [] {
        int value = 0;
        size_t size = sizeof(value);
        if (::sysctlbyname("hw.optional.arm.FEAT_LRCPC", &value, &size,
                           nullptr, 0) != 0) {
            return false;   // absent key == not advertised == do not use
        }
        return value != 0;
    }();
    return s_supported;
}


// ---------------------------------------------------------------------------
// FEAT_AES host gate
// ---------------------------------------------------------------------------

bool host_supports_aes() noexcept {
    // VITREO_JIT_NO_AES_DIRECT=1 forces the jit_sse_op helper lowering, so the
    // AES parity tests can cover both arms on the same machine and a suspected
    // mismatch can be bisected against the interpreter's own reference path.
    // Uncached for the same reason as the FlagM gate: an opt-out nothing can
    // exercise is an opt-out that is wrong.
    if (std::getenv("VITREO_JIT_NO_AES_DIRECT") != nullptr) return false;
    static const bool s_supported = [] {
        int value = 0;
        size_t size = sizeof(value);
        if (::sysctlbyname("hw.optional.arm.FEAT_AES", &value, &size,
                           nullptr, 0) != 0) {
            return false;   // absent key == not advertised == do not use
        }
        return value != 0;
    }();
    return s_supported;
}

// ---------------------------------------------------------------------------
// Immediate-field overflow: the one exit for an encoder handed a value its
// field cannot hold. Emitting anything here would run as a different
// instruction, so stop at the emission site with what was asked for.
// ---------------------------------------------------------------------------

void arm64::imm_field_overflow(const char* encoder, u64 value,
                               u64 field_max) noexcept {
    std::fprintf(stderr,
                 "[jit] FATAL: %s: immediate 0x%llx exceeds its field "
                 "(max 0x%llx); masking it would encode a different "
                 "instruction\n",
                 encoder, static_cast<unsigned long long>(value),
                 static_cast<unsigned long long>(field_max));
    std::abort();
}

void arm64::simm_field_overflow(const char* encoder, i64 value, i64 field_min,
                                i64 field_max) noexcept {
    std::fprintf(stderr,
                 "[jit] FATAL: %s: immediate %lld is outside its field "
                 "[%lld, %lld]; masking it would encode a different "
                 "instruction\n",
                 encoder, static_cast<long long>(value),
                 static_cast<long long>(field_min),
                 static_cast<long long>(field_max));
    std::abort();
}

void arm64::imm_field_misaligned(const char* encoder, i64 value,
                                 u64 granule) noexcept {
    std::fprintf(stderr,
                 "[jit] FATAL: %s: immediate %lld is not a multiple of %llu, "
                 "the only values its field encodes\n",
                 encoder, static_cast<long long>(value),
                 static_cast<unsigned long long>(granule));
    std::abort();
}

void arm64::logical_imm_unencodable(const char* encoder, u64 value,
                                    unsigned datasize) noexcept {
    std::fprintf(stderr,
                 "[jit] FATAL: %s: 0x%llx is not a %u-bit bitmask immediate "
                 "(no N:immr:imms encodes it)\n",
                 encoder, static_cast<unsigned long long>(value), datasize);
    std::abort();
}

} // namespace vitreo::cpu::jit
