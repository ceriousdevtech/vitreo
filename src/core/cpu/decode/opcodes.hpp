// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64

#pragma once

#include "vitreo/types.hpp"

namespace vitreo::cpu {

/// x86/x86-64 Opcode identifiers
/// Organized by instruction category
enum class Opcode : u16 {
    // Invalid/Unknown
    INVALID = 0,
    
    // ========================================================================
    // Data Transfer Instructions
    // ========================================================================
    
    // MOV variants
    MOV_R8_IMM8,           // B0+rb ib
    MOV_R16_IMM16,         // B8+rw iw (66h prefix)
    MOV_R32_IMM32,         // B8+rd id
    MOV_R64_IMM64,         // REX.W B8+rd io
    MOV_RM8_R8,            // 88 /r
    MOV_RM16_R16,          // 89 /r (66h prefix)
    MOV_RM32_R32,          // 89 /r
    MOV_RM64_R64,          // REX.W 89 /r
    MOV_R8_RM8,            // 8A /r
    MOV_R16_RM16,          // 8B /r (66h prefix)
    MOV_R32_RM32,          // 8B /r
    MOV_R64_RM64,          // REX.W 8B /r
    MOV_RM8_IMM8,          // C6 /0 ib
    MOV_RM16_IMM16,        // C7 /0 iw (66h prefix)
    MOV_RM32_IMM32,        // C7 /0 id
    MOV_RM64_IMM32,        // REX.W C7 /0 id (sign-extended)
    MOV_AL_MOFFS8,         // A0 
    MOV_AX_MOFFS16,        // A1 (66h prefix)
    MOV_EAX_MOFFS32,       // A1
    MOV_RAX_MOFFS64,       // REX.W A1
    MOV_MOFFS8_AL,         // A2
    MOV_MOFFS16_AX,        // A3 (66h prefix)
    MOV_MOFFS32_EAX,       // A3
    MOV_MOFFS64_RAX,       // REX.W A3
    MOV_RM16_SREG,         // 8C /r  (MOV r/m16, Sreg)
    MOV_SREG_RM16,         // 8E /r  (MOV Sreg, r/m16)
    XLAT,                   // D7 — AL := byte at DS:[(E/R)BX + AL]
    
    // MOVZX/MOVSX
    MOVZX_R16_RM8,
    MOVZX_R32_RM8,
    MOVZX_R64_RM8,
    MOVZX_R32_RM16,
    MOVZX_R64_RM16,
    MOVSX_R16_RM8,
    MOVSX_R32_RM8,
    MOVSX_R64_RM8,
    MOVSX_R32_RM16,
    MOVSX_R64_RM16,
    MOVSXD_R64_RM32,       // 63 /r (REX.W)
    
    // XCHG
    XCHG_AX_R16,
    XCHG_EAX_R32,
    XCHG_RAX_R64,
    XCHG_RM8_R8,
    XCHG_RM16_R16,
    XCHG_RM32_R32,
    XCHG_RM64_R64,
    
    // CMPXCHG
    CMPXCHG_RM8_R8,        // 0F B0 /r
    CMPXCHG_RM16_R16,      // 0F B1 /r (66h)
    CMPXCHG_RM32_R32,      // 0F B1 /r
    CMPXCHG_RM64_R64,      // REX.W 0F B1 /r
    CMPXCHG8B_M64,          // 0F C7 /1
    CMPXCHG16B_M128,        // REX.W 0F C7 /1
    
    // XADD
    XADD_RM8_R8,           // 0F C0 /r
    XADD_RM16_R16,         // 0F C1 /r (66h)
    XADD_RM32_R32,         // 0F C1 /r
    XADD_RM64_R64,         // REX.W 0F C1 /r
    
    // LEA
    LEA_R16_M,
    LEA_R32_M,
    LEA_R64_M,
    
    // PUSH
    PUSH_R16,
    PUSH_R32,
    PUSH_R64,
    PUSH_RM16,
    PUSH_RM32,
    PUSH_RM64,
    PUSH_IMM8,
    PUSH_IMM16,
    PUSH_IMM32,
    PUSH_CS,
    PUSH_SS,
    PUSH_DS,
    PUSH_ES,
    PUSH_FS,
    PUSH_GS,
    PUSHF,
    PUSHFQ,
    PUSHA,
    
    // POP
    POP_R16,
    POP_R32,
    POP_R64,
    POP_RM16,
    POP_RM32,
    POP_RM64,
    POP_DS,
    POP_ES,
    POP_SS,
    POP_FS,
    POP_GS,
    POPF,
    POPFQ,
    POPA,
    
    // ========================================================================
    // Arithmetic Instructions
    // ========================================================================
    
    // ADD
    ADD_AL_IMM8,
    ADD_AX_IMM16,
    ADD_EAX_IMM32,
    ADD_RAX_IMM32,
    ADD_RM8_IMM8,
    ADD_RM16_IMM16,
    ADD_RM32_IMM32,
    ADD_RM64_IMM32,
    ADD_RM16_IMM8,
    ADD_RM32_IMM8,
    ADD_RM64_IMM8,
    ADD_RM8_R8,
    ADD_RM16_R16,
    ADD_RM32_R32,
    ADD_RM64_R64,
    ADD_R8_RM8,
    ADD_R16_RM16,
    ADD_R32_RM32,
    ADD_R64_RM64,
    
    // SUB
    SUB_AL_IMM8,
    SUB_AX_IMM16,
    SUB_EAX_IMM32,
    SUB_RAX_IMM32,
    SUB_RM8_IMM8,
    SUB_RM16_IMM16,
    SUB_RM32_IMM32,
    SUB_RM64_IMM32,
    SUB_RM16_IMM8,
    SUB_RM32_IMM8,
    SUB_RM64_IMM8,
    SUB_RM8_R8,
    SUB_RM16_R16,
    SUB_RM32_R32,
    SUB_RM64_R64,
    SUB_R8_RM8,
    SUB_R16_RM16,
    SUB_R32_RM32,
    SUB_R64_RM64,
    
    // ADC (Add with Carry)
    ADC_AL_IMM8,
    ADC_AX_IMM16,
    ADC_EAX_IMM32,
    ADC_RAX_IMM32,
    ADC_RM8_IMM8,
    ADC_RM16_IMM16,
    ADC_RM32_IMM32,
    ADC_RM64_IMM32,
    ADC_RM16_IMM8,
    ADC_RM32_IMM8,
    ADC_RM64_IMM8,
    ADC_RM8_R8,
    ADC_RM16_R16,
    ADC_RM32_R32,
    ADC_RM64_R64,
    ADC_R8_RM8,
    ADC_R16_RM16,
    ADC_R32_RM32,
    ADC_R64_RM64,
    
    // SBB (Subtract with Borrow)
    SBB_AL_IMM8,
    SBB_AX_IMM16,
    SBB_EAX_IMM32,
    SBB_RAX_IMM32,
    SBB_RM8_IMM8,
    SBB_RM16_IMM16,
    SBB_RM32_IMM32,
    SBB_RM64_IMM32,
    SBB_RM16_IMM8,
    SBB_RM32_IMM8,
    SBB_RM64_IMM8,
    SBB_RM8_R8,
    SBB_RM16_R16,
    SBB_RM32_R32,
    SBB_RM64_R64,
    SBB_R8_RM8,
    SBB_R16_RM16,
    SBB_R32_RM32,
    SBB_R64_RM64,
    
    // CMP
    CMP_AL_IMM8,
    CMP_AX_IMM16,
    CMP_EAX_IMM32,
    CMP_RAX_IMM32,
    CMP_RM8_IMM8,
    CMP_RM16_IMM16,
    CMP_RM32_IMM32,
    CMP_RM64_IMM32,
    CMP_RM16_IMM8,
    CMP_RM32_IMM8,
    CMP_RM64_IMM8,
    CMP_RM8_R8,
    CMP_RM16_R16,
    CMP_RM32_R32,
    CMP_RM64_R64,
    CMP_R8_RM8,
    CMP_R16_RM16,
    CMP_R32_RM32,
    CMP_R64_RM64,
    
    // INC/DEC
    INC_R16,
    INC_R32,
    INC_RM8,
    INC_RM16,
    INC_RM32,
    INC_RM64,
    DEC_R16,
    DEC_R32,
    DEC_RM8,
    DEC_RM16,
    DEC_RM32,
    DEC_RM64,
    
    // NEG
    NEG_RM8,
    NEG_RM16,
    NEG_RM32,
    NEG_RM64,
    
    // MUL/IMUL
    MUL_RM8,
    MUL_RM16,
    MUL_RM32,
    MUL_RM64,
    IMUL_RM8,
    IMUL_RM16,
    IMUL_RM32,
    IMUL_RM64,
    IMUL_R16_RM16,
    IMUL_R32_RM32,
    IMUL_R64_RM64,
    IMUL_R16_RM16_IMM8,
    IMUL_R32_RM32_IMM8,
    IMUL_R64_RM64_IMM8,
    IMUL_R16_RM16_IMM16,
    IMUL_R32_RM32_IMM32,
    IMUL_R64_RM64_IMM32,
    
    // DIV/IDIV
    DIV_RM8,
    DIV_RM16,
    DIV_RM32,
    DIV_RM64,
    IDIV_RM8,
    IDIV_RM16,
    IDIV_RM32,
    IDIV_RM64,
    
    // ========================================================================
    // Logical Instructions
    // ========================================================================
    
    // AND
    AND_AL_IMM8,
    AND_AX_IMM16,
    AND_EAX_IMM32,
    AND_RAX_IMM32,
    AND_RM8_IMM8,
    AND_RM16_IMM16,
    AND_RM32_IMM32,
    AND_RM64_IMM32,
    AND_RM16_IMM8,
    AND_RM32_IMM8,
    AND_RM64_IMM8,
    AND_RM8_R8,
    AND_RM16_R16,
    AND_RM32_R32,
    AND_RM64_R64,
    AND_R8_RM8,
    AND_R16_RM16,
    AND_R32_RM32,
    AND_R64_RM64,
    
    // OR
    OR_AL_IMM8,
    OR_AX_IMM16,
    OR_EAX_IMM32,
    OR_RAX_IMM32,
    OR_RM8_IMM8,
    OR_RM16_IMM16,
    OR_RM32_IMM32,
    OR_RM64_IMM32,
    OR_RM16_IMM8,
    OR_RM32_IMM8,
    OR_RM64_IMM8,
    OR_RM8_R8,
    OR_RM16_R16,
    OR_RM32_R32,
    OR_RM64_R64,
    OR_R8_RM8,
    OR_R16_RM16,
    OR_R32_RM32,
    OR_R64_RM64,
    
    // XOR
    XOR_AL_IMM8,
    XOR_AX_IMM16,
    XOR_EAX_IMM32,
    XOR_RAX_IMM32,
    XOR_RM8_IMM8,
    XOR_RM16_IMM16,
    XOR_RM32_IMM32,
    XOR_RM64_IMM32,
    XOR_RM16_IMM8,
    XOR_RM32_IMM8,
    XOR_RM64_IMM8,
    XOR_RM8_R8,
    XOR_RM16_R16,
    XOR_RM32_R32,
    XOR_RM64_R64,
    XOR_R8_RM8,
    XOR_R16_RM16,
    XOR_R32_RM32,
    XOR_R64_RM64,
    
    // NOT
    NOT_RM8,
    NOT_RM16,
    NOT_RM32,
    NOT_RM64,
    
    // TEST
    TEST_AL_IMM8,
    TEST_AX_IMM16,
    TEST_EAX_IMM32,
    TEST_RAX_IMM32,
    TEST_RM8_IMM8,
    TEST_RM16_IMM16,
    TEST_RM32_IMM32,
    TEST_RM64_IMM32,
    TEST_RM8_R8,
    TEST_RM16_R16,
    TEST_RM32_R32,
    TEST_RM64_R64,
    
    // ========================================================================
    // Shift/Rotate Instructions
    // ========================================================================
    
    // SHL/SAL (same instruction)
    SHL_RM8_1,
    SHL_RM16_1,
    SHL_RM32_1,
    SHL_RM64_1,
    SHL_RM8_CL,
    SHL_RM16_CL,
    SHL_RM32_CL,
    SHL_RM64_CL,
    SHL_RM8_IMM8,
    SHL_RM16_IMM8,
    SHL_RM32_IMM8,
    SHL_RM64_IMM8,
    
    // SHR
    SHR_RM8_1,
    SHR_RM16_1,
    SHR_RM32_1,
    SHR_RM64_1,
    SHR_RM8_CL,
    SHR_RM16_CL,
    SHR_RM32_CL,
    SHR_RM64_CL,
    SHR_RM8_IMM8,
    SHR_RM16_IMM8,
    SHR_RM32_IMM8,
    SHR_RM64_IMM8,
    
    // SAR
    SAR_RM8_1,
    SAR_RM16_1,
    SAR_RM32_1,
    SAR_RM64_1,
    SAR_RM8_CL,
    SAR_RM16_CL,
    SAR_RM32_CL,
    SAR_RM64_CL,
    SAR_RM8_IMM8,
    SAR_RM16_IMM8,
    SAR_RM32_IMM8,
    SAR_RM64_IMM8,
    
    // SHLD (double-precision shift left)
    SHLD_RM16_R16_IMM8,
    SHLD_RM32_R32_IMM8,
    SHLD_RM64_R64_IMM8,
    SHLD_RM16_R16_CL,
    SHLD_RM32_R32_CL,
    SHLD_RM64_R64_CL,
    
    // SHRD (double-precision shift right)
    SHRD_RM16_R16_IMM8,
    SHRD_RM32_R32_IMM8,
    SHRD_RM64_R64_IMM8,
    SHRD_RM16_R16_CL,
    SHRD_RM32_R32_CL,
    SHRD_RM64_R64_CL,
    
    // ROL
    ROL_RM8_1,
    ROL_RM16_1,
    ROL_RM32_1,
    ROL_RM64_1,
    ROL_RM8_CL,
    ROL_RM16_CL,
    ROL_RM32_CL,
    ROL_RM64_CL,
    ROL_RM8_IMM8,
    ROL_RM16_IMM8,
    ROL_RM32_IMM8,
    ROL_RM64_IMM8,
    
    // ROR
    ROR_RM8_1,
    ROR_RM16_1,
    ROR_RM32_1,
    ROR_RM64_1,
    ROR_RM8_CL,
    ROR_RM16_CL,
    ROR_RM32_CL,
    ROR_RM64_CL,
    ROR_RM8_IMM8,
    ROR_RM16_IMM8,
    ROR_RM32_IMM8,
    ROR_RM64_IMM8,
    
    // RCL
    RCL_RM8_1,
    RCL_RM16_1,
    RCL_RM32_1,
    RCL_RM64_1,
    RCL_RM8_CL,
    RCL_RM16_CL,
    RCL_RM32_CL,
    RCL_RM64_CL,
    RCL_RM8_IMM8,
    RCL_RM16_IMM8,
    RCL_RM32_IMM8,
    RCL_RM64_IMM8,
    
    // RCR
    RCR_RM8_1,
    RCR_RM16_1,
    RCR_RM32_1,
    RCR_RM64_1,
    RCR_RM8_CL,
    RCR_RM16_CL,
    RCR_RM32_CL,
    RCR_RM64_CL,
    RCR_RM8_IMM8,
    RCR_RM16_IMM8,
    RCR_RM32_IMM8,
    RCR_RM64_IMM8,
    
    // ========================================================================
    // Control Flow Instructions
    // ========================================================================
    
    // JMP
    JMP_REL8,
    JMP_REL16,
    JMP_REL32,
    JMP_RM16,
    JMP_RM32,
    JMP_RM64,
    JMP_PTR16_16,
    JMP_PTR16_32,
    JMP_M16_16,
    JMP_M16_32,
    JMP_M16_64,
    
    // Jcc (conditional jumps)
    JO_REL8,
    JNO_REL8,
    JB_REL8,       // JC, JNAE
    JNB_REL8,      // JNC, JAE
    JZ_REL8,       // JE
    JNZ_REL8,      // JNE
    JBE_REL8,      // JNA
    JNBE_REL8,     // JA
    JS_REL8,
    JNS_REL8,
    JP_REL8,       // JPE
    JNP_REL8,      // JPO
    JL_REL8,       // JNGE
    JNL_REL8,      // JGE
    JLE_REL8,      // JNG
    JNLE_REL8,     // JG
    
    JO_REL32,
    JNO_REL32,
    JB_REL32,
    JNB_REL32,
    JZ_REL32,
    JNZ_REL32,
    JBE_REL32,
    JNBE_REL32,
    JS_REL32,
    JNS_REL32,
    JP_REL32,
    JNP_REL32,
    JL_REL32,
    JNL_REL32,
    JLE_REL32,
    JNLE_REL32,
    
    // LOOP
    LOOP_REL8,
    LOOPE_REL8,    // LOOPZ
    LOOPNE_REL8,   // LOOPNZ
    JCXZ_REL8,
    JECXZ_REL8,
    JRCXZ_REL8,
    
    // CALL
    CALL_REL16,
    CALL_REL32,
    CALL_RM16,
    CALL_RM32,
    CALL_RM64,
    CALL_PTR16_16,
    CALL_PTR16_32,
    CALL_M16_16,
    CALL_M16_32,
    CALL_M16_64,
    
    // LEAVE / ENTER
    LEAVE,
    ENTER,
    
    // RET
    RET_NEAR,
    RET_NEAR_IMM16,
    RET_FAR,
    RET_FAR_IMM16,
    
    // INT
    INT_3,
    INT_IMM8,
    INTO,
    IRET,
    IRETD,
    IRETQ,
    
    // ========================================================================
    // Flag Instructions
    // ========================================================================
    
    CLC,
    STC,
    CMC,
    CLD,
    STD,
    CLI,
    STI,
    LAHF,
    SAHF,
    
    // ========================================================================
    // Misc Instructions
    // ========================================================================
    
    NOP,
    NOP_RM,        // Multi-byte NOP
    CPUID,
    SYSCALL,
    SYSENTER,
    SYSEXIT,
    RDTSC,
    RDTSCP,
    HLT,
    WBINVD,
    UD2,
    PAUSE,
    LFENCE,
    MFENCE,
    SFENCE,
    
    // CBW/CWDE/CDQE family
    CBW,
    CWDE,
    CDQE,
    CWD,
    CDQ,
    CQO,
    
    // String operations
    MOVSB,
    MOVSW,
    MOVSD_STR,     // Disambiguate from SSE MOVSD
    MOVSQ,
    CMPSB,
    CMPSW,
    CMPSD_STR,
    CMPSQ,
    STOSB,
    STOSW,
    STOSD,
    STOSQ,
    LODSB,
    LODSW,
    LODSD,
    LODSQ,
    SCASB,
    SCASW,
    SCASD,
    SCASQ,
    
    // Bit manipulation
    BT_RM16_R16,
    BT_RM32_R32,
    BT_RM64_R64,
    BT_RM16_IMM8,
    BT_RM32_IMM8,
    BT_RM64_IMM8,
    BTS_RM16_R16,
    BTS_RM32_R32,
    BTS_RM64_R64,
    BTS_RM16_IMM8,
    BTS_RM32_IMM8,
    BTS_RM64_IMM8,
    BTR_RM16_R16,
    BTR_RM32_R32,
    BTR_RM64_R64,
    BTR_RM16_IMM8,
    BTR_RM32_IMM8,
    BTR_RM64_IMM8,
    BTC_RM16_R16,
    BTC_RM32_R32,
    BTC_RM64_R64,
    BTC_RM16_IMM8,
    BTC_RM32_IMM8,
    BTC_RM64_IMM8,
    BSF_R16_RM16,
    BSF_R32_RM32,
    BSF_R64_RM64,
    BSR_R16_RM16,
    BSR_R32_RM32,
    BSR_R64_RM64,
    POPCNT_R16_RM16,
    POPCNT_R32_RM32,
    POPCNT_R64_RM64,
    TZCNT_R16_RM16,
    LZCNT_R16_RM16,
    TZCNT_R32_RM32,
    LZCNT_R32_RM32,
    TZCNT_R64_RM64,
    LZCNT_R64_RM64,
    BSWAP_R32,
    BSWAP_R64,
    
    // CMOVcc
    CMOVO_R16_RM16,
    CMOVO_R32_RM32,
    CMOVO_R64_RM64,
    CMOVNO_R16_RM16,
    CMOVNO_R32_RM32,
    CMOVNO_R64_RM64,
    CMOVB_R16_RM16,
    CMOVB_R32_RM32,
    CMOVB_R64_RM64,
    CMOVNB_R16_RM16,
    CMOVNB_R32_RM32,
    CMOVNB_R64_RM64,
    CMOVZ_R16_RM16,
    CMOVZ_R32_RM32,
    CMOVZ_R64_RM64,
    CMOVNZ_R16_RM16,
    CMOVNZ_R32_RM32,
    CMOVNZ_R64_RM64,
    CMOVBE_R16_RM16,
    CMOVBE_R32_RM32,
    CMOVBE_R64_RM64,
    CMOVNBE_R16_RM16,
    CMOVNBE_R32_RM32,
    CMOVNBE_R64_RM64,
    CMOVS_R16_RM16,
    CMOVS_R32_RM32,
    CMOVS_R64_RM64,
    CMOVNS_R16_RM16,
    CMOVNS_R32_RM32,
    CMOVNS_R64_RM64,
    CMOVP_R16_RM16,
    CMOVP_R32_RM32,
    CMOVP_R64_RM64,
    CMOVNP_R16_RM16,
    CMOVNP_R32_RM32,
    CMOVNP_R64_RM64,
    CMOVL_R16_RM16,
    CMOVL_R32_RM32,
    CMOVL_R64_RM64,
    CMOVNL_R16_RM16,
    CMOVNL_R32_RM32,
    CMOVNL_R64_RM64,
    CMOVLE_R16_RM16,
    CMOVLE_R32_RM32,
    CMOVLE_R64_RM64,
    CMOVNLE_R16_RM16,
    CMOVNLE_R32_RM32,
    CMOVNLE_R64_RM64,
    
    // SETcc
    SETO_RM8,
    SETNO_RM8,
    SETB_RM8,
    SETNB_RM8,
    SETZ_RM8,
    SETNZ_RM8,
    SETBE_RM8,
    SETNBE_RM8,
    SETS_RM8,
    SETNS_RM8,
    SETP_RM8,
    SETNP_RM8,
    SETL_RM8,
    SETNL_RM8,
    SETLE_RM8,
    SETNLE_RM8,
    
    // ========================================================================
    // x87 FPU Instructions (CPU-030)
    // ========================================================================
    
    // FPU Control
    FNCLEX,
    FNINIT,
    FNOP,
    FWAIT,
    FDECSTP,
    FINCSTP,
    
    // FPU Load/Store (float)
    FLD_M32,
    FLD_M64,
    FLD_M80,
    FLD_STI,
    FSTP_M32,
    FSTP_M64,
    FSTP_M80,
    FSTP_STI,
    FST_STI,
    FST_M32,
    FST_M64,
    
    // FPU Load/Store (integer)
    FILD_M16,
    FILD_M32,
    FILD_M64,
    FIST_M16,
    FIST_M32,
    FISTP_M16,
    FISTP_M32,
    FISTP_M64,
    FISTTP_M16,       // DF /1: FISTTP m16int (SSE3)
    FISTTP_M32,       // DB /1: FISTTP m32int (SSE3)
    FISTTP_M64,       // DD /1: FISTTP m64int (SSE3)
    
    // FPU Constants
    FLD1,
    FLDZ,
    
    // FPU Arithmetic (register) — D8 forms: op ST(0), ST(i) → ST(0)
    FADD_STI,
    FADDP_STI,
    FSUB_STI,
    FSUBP_STI,
    FMUL_STI,
    FMULP_STI,
    FDIV_STI,
    FDIVP_STI,
    FDIVRP_STI,
    FSUBR_STI,
    FSUBRP_STI,
    FDIVR_STI,
    // DC register forms: op ST(i), ST(0) → ST(i), no pop
    FADD_TO_STI,
    FMUL_TO_STI,
    FSUB_TO_STI,
    FSUBR_TO_STI,
    FDIV_TO_STI,
    FDIVR_TO_STI,
    
    // FPU Arithmetic (memory)
    FADD_M32,
    FADD_M64,
    FMUL_M32,
    FMUL_M64,
    FSUB_M32,
    FSUB_M64,
    FDIV_M32,
    FDIV_M64,
    // DA m32int / DE m16int integer arithmetic & compare (Intel SDM Vol. 2)
    FIADD_M16,
    FIADD_M32,
    FIMUL_M16,
    FIMUL_M32,
    FICOM_M16,
    FICOM_M32,
    FICOMP_M16,
    FICOMP_M32,
    FISUB_M16,
    FISUB_M32,
    FISUBR_M16,
    FISUBR_M32,
    FIDIV_M16,
    FIDIV_M32,
    FIDIVR_M16,
    FIDIVR_M32,
    
    // FPU Comparison
    FCOM_STI,
    FCOM_M32,
    FCOM_M64,
    FCOMP_STI,
    FCOMP_M32,
    FCOMP_M64,
    FCOMPP,
    FUCOMPP,
    FUCOM_STI,
    FUCOMP_STI,
    FCOMI_STI,
    FCOMIP_STI,
    FUCOMI_STI,
    FUCOMIP_STI,
    // FCMOV — EFLAGS-conditional ST(0) ← ST(i)
    FCMOVB_STI,    // CF=1
    FCMOVE_STI,    // ZF=1
    FCMOVBE_STI,   // CF=1 | ZF=1
    FCMOVU_STI,    // PF=1
    FCMOVNB_STI,   // CF=0
    FCMOVNE_STI,   // ZF=0
    FCMOVNBE_STI,  // CF=0 & ZF=0
    FCMOVNU_STI,   // PF=0
    FTST,
    
    // FPU Status/Control
    FNSTSW_AX,
    FNSTSW_M16,
    FNSTCW_M16,
    FLDCW_M16,
    FNSTENV_M28,
    FLDENV_M28,
    FNSAVE_M108,
    FRSTOR_M108,
    
    // FPU Sign/Misc
    FCHS,
    FABS,
    FXCH_STI,
    FRNDINT,
    FSQRT,
    F2XM1,
    FYL2X,
    FYL2XP1,
    FPTAN,
    FPATAN,
    FXTRACT,
    FPREM,
    FPREM1,
    FSCALE,
    FSIN,
    FCOS,
    FSINCOS,
    FLDL2T,
    FLDL2E,
    FLDPI,
    FLDLG2,
    FLDLN2,
    FXAM,
    FSUBR_M32,
    FSUBR_M64,
    FDIVR_M32,
    FDIVR_M64,
    
    // ========================================================================
    // SSE Arithmetic Instructions
    // ========================================================================
    ADDSS_XMM_XMMM32,
    ADDSD_XMM_XMMM64,
    ADDPS_XMM_XMMM128,
    ADDPD_XMM_XMMM128,
    SUBSS_XMM_XMMM32,
    SUBSD_XMM_XMMM64,
    SUBPS_XMM_XMMM128,
    SUBPD_XMM_XMMM128,
    MULSS_XMM_XMMM32,
    MULSD_XMM_XMMM64,
    MULPS_XMM_XMMM128,
    MULPD_XMM_XMMM128,
    DIVSS_XMM_XMMM32,
    DIVSD_XMM_XMMM64,
    DIVPS_XMM_XMMM128,
    DIVPD_XMM_XMMM128,
    SQRTSS_XMM_XMMM32,
    SQRTSD_XMM_XMMM64,
    SQRTPS_XMM_XMMM128,
    SQRTPD_XMM_XMMM128,
    MAXSS_XMM_XMMM32,
    MAXSD_XMM_XMMM64,
    MAXPS_XMM_XMMM128,
    MAXPD_XMM_XMMM128,
    MINSS_XMM_XMMM32,
    MINSD_XMM_XMMM64,
    MINPS_XMM_XMMM128,
    MINPD_XMM_XMMM128,
    COMISS_XMM_XMMM32,
    COMISD_XMM_XMMM64,
    UCOMISS_XMM_XMMM32,
    UCOMISD_XMM_XMMM64,
    RCPSS_XMM_XMMM32,
    RCPPS_XMM_XMMM128,
    RSQRTSS_XMM_XMMM32,
    RSQRTPS_XMM_XMMM128,

    // SSE3 arithmetic
    ADDSUBPD_XMM_XMMM128,
    ADDSUBPS_XMM_XMMM128,
    HADDPD_XMM_XMMM128,
    HADDPS_XMM_XMMM128,
    HSUBPD_XMM_XMMM128,
    HSUBPS_XMM_XMMM128,
    // SSE3 data movement
    MOVSLDUP_XMM_XMMM128,
    MOVSHDUP_XMM_XMMM128,
    MOVDDUP_XMM_XMMM64,

    // ========================================================================
    // SSE Data Transfer (CPU-031)
    // ========================================================================
    MOVAPS_XMM_XMMM128, MOVAPS_XMMM128_XMM,
    MOVUPS_XMM_XMMM128, MOVUPS_XMMM128_XMM,
    MOVAPD_XMM_XMMM128, MOVAPD_XMMM128_XMM,
    MOVUPD_XMM_XMMM128, MOVUPD_XMMM128_XMM,
    // Intel SDM Vol. 2: NP 0F 2B /r MOVNTPS m128,xmm; 66 0F 2B /r MOVNTPD.
    // Memory destination only; 16-byte aligned; non-temporal hint is advisory.
    MOVNTPS_M128_XMM, MOVNTPD_M128_XMM,
    MOVDQA_XMM_XMMM128, MOVDQA_XMMM128_XMM,
    MOVDQU_XMM_XMMM128, MOVDQU_XMMM128_XMM,
    LDDQU_XMM_M128,  // SSE3 F2 0F F0 /r — unaligned 128-bit load (mem only)
    MOVSS_XMM_XMMM32, MOVSS_XMMM32_XMM,
    MOVSD_XMM_XMMM64, MOVSD_XMMM64_XMM,
    MOVLPS_XMM_M64, MOVLPS_M64_XMM,
    MOVHPS_XMM_M64, MOVHPS_M64_XMM,
    MOVLPD_XMM_M64, MOVLPD_M64_XMM,
    MOVHPD_XMM_M64, MOVHPD_M64_XMM,
    MOVLHPS_XMM_XMM, MOVHLPS_XMM_XMM,
    MOVMSKPS_R32_XMM, MOVMSKPD_R32_XMM,
    PMOVMSKB_R32_XMM,
    XORPS_XMM_XMMM128, XORPD_XMM_XMMM128,
    ANDPS_XMM_XMMM128, ANDPD_XMM_XMMM128,
    ORPS_XMM_XMMM128, ORPD_XMM_XMMM128,
    ANDNPS_XMM_XMMM128, ANDNPD_XMM_XMMM128,
    FXSAVE_M512, LDMXCSR_M32, STMXCSR_M32, EMMS,

    // ========================================================================
    // MMX instructions (CPU-045).
    // ========================================================================
    // Data movement
    MOVQ_MM_MMM64,         // 0F 6F /r — MOVQ mm, mm/m64
    MOVQ_MMM64_MM,         // 0F 7F /r — MOVQ mm/m64, mm
    MOVD_MM_RM32,          // 0F 6E /r — MOVD mm, r/m32
    MOVD_RM32_MM,          // 0F 7E /r — MOVD r/m32, mm
    // Packed arithmetic
    PADDW_MM_MMM64,        // 0F FD /r — PADDW mm, mm/m64
    PADDD_MM_MMM64,        // 0F FE /r — PADDD mm, mm/m64
    PACKUSWB_MM_MMM64,     // 0F 67 /r — PACKUSWB mm, mm/m64
    PSRLD_MM_IMM8,         // 0F 72 /2 ib — PSRLD mm, imm8
    PSRAD_MM_IMM8,         // 0F 72 /4 ib — PSRAD mm, imm8
    PSLLD_MM_IMM8,         // 0F 72 /6 ib — PSLLD mm, imm8
    PADDQ_MM_MMM64,        // 0F D4 /r — PADDQ mm, mm/m64
    PMULUDQ_MM_MMM64,     // 0F F4 /r — PMULUDQ mm, mm/m64
    PAND_MM_MMM64,         // 0F DB /r — PAND mm, mm/m64
    PANDN_MM_MMM64,        // 0F DF /r — PANDN mm, mm/m64
    POR_MM_MMM64,          // 0F EB /r — POR mm, mm/m64
    PXOR_MM_MMM64,         // 0F EF /r — PXOR mm, mm/m64
    // Packed compare-equal
    PCMPEQB_MM_MMM64,      // 0F 74 /r — PCMPEQB mm, mm/m64
    PCMPEQW_MM_MMM64,      // 0F 75 /r — PCMPEQW mm, mm/m64
    PCMPEQD_MM_MMM64,      // 0F 76 /r — PCMPEQD mm, mm/m64
    PCMPGTB_MM_MMM64,      // 0F 64 /r — PCMPGTB mm, mm/m64
    PCMPGTW_MM_MMM64,      // 0F 65 /r — PCMPGTW mm, mm/m64
    PCMPGTD_MM_MMM64,      // 0F 66 /r — PCMPGTD mm, mm/m64
    // Packed shifts by immediate
    PSLLQ_MM_IMM8,         // 0F 73 /6 ib — PSLLQ mm, imm8
    PSRLQ_MM_IMM8,         // 0F 73 /2 ib — PSRLQ mm, imm8
    // Packed shifts by register
    PSLLQ_MM_MMM64,        // 0F F3 /r — PSLLQ mm, mm/m64
    PSRLQ_MM_MMM64,        // 0F D3 /r — PSRLQ mm, mm/m64
    // Unpack
    PUNPCKLBW_MM_MMM64,    // 0F 60 /r — PUNPCKLBW mm, mm/m64
    PUNPCKLWD_MM_MMM64,    // 0F 61 /r — PUNPCKLWD mm, mm/m64
    PUNPCKLDQ_MM_MMM64,    // 0F 62 /r — PUNPCKLDQ mm, mm/m64
    PUNPCKHBW_MM_MMM64,    // 0F 68 /r — PUNPCKHBW mm, mm/m64
    PUNPCKHWD_MM_MMM64,    // 0F 69 /r — PUNPCKHWD mm, mm/m64
    PUNPCKHDQ_MM_MMM64,    // 0F 6A /r — PUNPCKHDQ mm, mm/m64
    PUNPCKHQDQ_MM_MMM64,   // 0F 6D /r — PUNPCKHQDQ mm, mm/m64
    PSUBW_MM_MMM64,        // 0F F9 /r — PSUBW mm, mm/m64
    PACKSSDW_MM_MMM64,     // 0F 6B /r — PACKSSDW mm, mm/m64
    PMINSW_MM_MMM64,       // 0F EA /r — PMINSW mm, mm/m64
    PMAXSW_MM_MMM64,       // 0F EE /r — PMAXSW mm, mm/m64
    // Remaining NP 0F MMX / SSE-on-MMX forms. These must land on MM0-7
    // (Intel SDM Vol. 2); the XMM8-15 alias is not architectural MMX state.
    PADDB_MM_MMM64,        // 0F FC /r
    PSUBB_MM_MMM64,        // 0F F8 /r
    PSUBD_MM_MMM64,        // 0F FA /r
    PSUBQ_MM_MMM64,        // 0F FB /r (SSE2 on MMX)
    PADDUSB_MM_MMM64,      // 0F DC /r
    PADDUSW_MM_MMM64,      // 0F DD /r
    PSUBUSB_MM_MMM64,      // 0F D8 /r
    PSUBUSW_MM_MMM64,      // 0F D9 /r
    PADDSB_MM_MMM64,       // 0F EC /r
    PADDSW_MM_MMM64,       // 0F ED /r
    PSUBSB_MM_MMM64,       // 0F E8 /r
    PSUBSW_MM_MMM64,       // 0F E9 /r
    PACKSSWB_MM_MMM64,     // 0F 63 /r
    PMULLW_MM_MMM64,       // 0F D5 /r
    PMULHW_MM_MMM64,       // 0F E5 /r
    PMULHUW_MM_MMM64,      // 0F E4 /r (SSE on MMX)
    PMADDWD_MM_MMM64,      // 0F F5 /r
    PMINUB_MM_MMM64,       // 0F DA /r (SSE on MMX)
    PMAXUB_MM_MMM64,       // 0F DE /r (SSE on MMX)
    PAVGB_MM_MMM64,        // 0F E0 /r (SSE on MMX)
    PAVGW_MM_MMM64,        // 0F E3 /r (SSE on MMX)
    PSADBW_MM_MMM64,       // 0F F6 /r (SSE on MMX)
    PSRLW_MM_IMM8,         // 0F 71 /2 ib
    PSRAW_MM_IMM8,         // 0F 71 /4 ib
    PSLLW_MM_IMM8,         // 0F 71 /6 ib
    PMOVMSKB_R32_MM,       // 0F D7 /r (SSE on MMX)
    PINSRW_MM_R32M16_IMM8, // 0F C4 /r ib
    PEXTRW_R32_MM_IMM8,    // 0F C5 /r ib
    PSRLW_MM_MMM64,        // 0F D1 /r
    PSRAW_MM_MMM64,        // 0F E1 /r
    PSLLW_MM_MMM64,        // 0F F1 /r
    PSRLD_MM_MMM64,        // 0F D2 /r
    PSRAD_MM_MMM64,        // 0F E2 /r
    PSLLD_MM_MMM64,        // 0F F2 /r
    MASKMOVQ_MM_MM,        // 0F F7 /r
    MOVNTQ_M64_MM,         // 0F E7 /r
    MASKMOVDQU_XMM_XMM,    // 66 0F F7 /r
    MOVNTDQ_M128_XMM,      // 66 0F E7 /r
    MOVQ2DQ_XMM_MM,        // F3 0F D6 /r
    MOVDQ2Q_MM_XMM,        // F2 0F D6 /r
    // SSSE3 on MMX (NP 0F 38 / 0F 3A — no 66 prefix)
    PSHUFB_MM_MMM64,       // 0F 38 00 /r
    PHADDW_MM_MMM64,       // 0F 38 01 /r
    PHADDD_MM_MMM64,       // 0F 38 02 /r
    PHADDSW_MM_MMM64,      // 0F 38 03 /r
    PMADDUBSW_MM_MMM64,    // 0F 38 04 /r
    PHSUBW_MM_MMM64,       // 0F 38 05 /r
    PHSUBD_MM_MMM64,       // 0F 38 06 /r
    PHSUBSW_MM_MMM64,      // 0F 38 07 /r
    PSIGNB_MM_MMM64,       // 0F 38 08 /r
    PSIGNW_MM_MMM64,       // 0F 38 09 /r
    PSIGND_MM_MMM64,       // 0F 38 0A /r
    PMULHRSW_MM_MMM64,     // 0F 38 0B /r
    PABSB_MM_MMM64,        // 0F 38 1C /r
    PABSW_MM_MMM64,        // 0F 38 1D /r
    PABSD_MM_MMM64,        // 0F 38 1E /r
    PALIGNR_MM_MMM64_IMM8, // 0F 3A 0F /r ib
    MOVNTI_M32_R32,        // 0F C3 /r
    MOVNTI_M64_R64,        // REX.W 0F C3 /r
    FXRSTOR_M512,          // 0F AE /1
    CLFLUSH_M8,            // 0F AE /7 mem

    // ========================================================================
    // SSE Conversion Instructions (CPU-033)
    // ========================================================================
    CVTSI2SS_XMM_RM32, CVTSI2SS_XMM_RM64,
    CVTSI2SD_XMM_RM32, CVTSI2SD_XMM_RM64,
    CVTSS2SI_R32_XMMM32, CVTSS2SI_R64_XMMM32,
    CVTSD2SI_R32_XMMM64, CVTSD2SI_R64_XMMM64,
    CVTTSS2SI_R32_XMMM32, CVTTSS2SI_R64_XMMM32,
    CVTTSD2SI_R32_XMMM64, CVTTSD2SI_R64_XMMM64,
    CVTSS2SD_XMM_XMMM32, CVTSD2SS_XMM_XMMM64,
    CVTPS2DQ_XMM_XMMM128, CVTDQ2PS_XMM_XMMM128,
    CVTPD2DQ_XMM_XMMM128, CVTDQ2PD_XMM_XMMM64,
    CVTPS2PD_XMM_XMMM64, CVTPD2PS_XMM_XMMM128,
    CVTTPD2DQ_XMM_XMMM128, CVTTPS2DQ_XMM_XMMM128,
    // MMX <-> packed FP (no mandatory prefix selects the MMX register file)
    CVTPI2PS_XMM_MMM64,    // NP 0F 2A /r
    CVTPI2PD_XMM_MMM64,    // 66 0F 2A /r
    CVTPS2PI_MM_XMMM64,    // NP 0F 2D /r
    CVTPD2PI_MM_XMMM128,   // 66 0F 2D /r
    CVTTPS2PI_MM_XMMM64,   // NP 0F 2C /r
    CVTTPD2PI_MM_XMMM128,  // 66 0F 2C /r

    // ========================================================================
    // SSE Comparison Instructions (CPU-036)
    // ========================================================================
    CMPPS_XMM_XMMM128_IMM8, CMPSS_XMM_XMMM32_IMM8,
    CMPPD_XMM_XMMM128_IMM8, CMPSD_XMM_XMMM64_IMM8,
    PCMPEQB_XMM_XMMM128, PCMPEQW_XMM_XMMM128, PCMPEQD_XMM_XMMM128,
    PCMPGTB_XMM_XMMM128, PCMPGTW_XMM_XMMM128, PCMPGTD_XMM_XMMM128,

    // ========================================================================
    // SSE2 Integer Arithmetic (CPU-037)
    // ========================================================================
    PADDB_XMM_XMMM128, PADDW_XMM_XMMM128, PADDD_XMM_XMMM128, PADDQ_XMM_XMMM128,
    PSUBB_XMM_XMMM128, PSUBW_XMM_XMMM128, PSUBD_XMM_XMMM128, PSUBQ_XMM_XMMM128,
    PADDUSB_XMM_XMMM128, PADDUSW_XMM_XMMM128, PSUBUSB_XMM_XMMM128, PSUBUSW_XMM_XMMM128,
    PADDSB_XMM_XMMM128, PADDSW_XMM_XMMM128, PSUBSB_XMM_XMMM128, PSUBSW_XMM_XMMM128,
    PACKSSWB_XMM_XMMM128, PACKSSDW_XMM_XMMM128, PACKUSWB_XMM_XMMM128,
    PMULLW_XMM_XMMM128, PMULHW_XMM_XMMM128, PMULHUW_XMM_XMMM128, PMADDWD_XMM_XMMM128, PMULUDQ_XMM_XMMM128,
    PMINUB_XMM_XMMM128, PMAXUB_XMM_XMMM128, PMINSW_XMM_XMMM128, PMAXSW_XMM_XMMM128,
    PAVGB_XMM_XMMM128, PAVGW_XMM_XMMM128, PSADBW_XMM_XMMM128,
    PAND_XMM_XMMM128, PANDN_XMM_XMMM128, POR_XMM_XMMM128, PXOR_XMM_XMMM128,
    // SSE Insert/Extract
    PINSRW_XMM_R32M16_IMM8, PEXTRW_R32_XMM_IMM8,

    // SSE2 Integer Data Movement
    MOVD_XMM_RM32, MOVD_RM32_XMM,       // 66 0F 6E / 66 0F 7E
    MOVQ_XMM_RM64, MOVQ_RM64_XMM,       // 66 REX.W 0F 6E / 66 REX.W 0F 7E
    MOVQ_XMM_XMMM64, MOVQ_XMMM64_XMM,  // F3 0F 7E / 66 0F D6
    // SSE2 Packed Shifts
    PSLLD_XMM_IMM8, PSLLQ_XMM_IMM8,     // 66 0F 72 /6, 66 0F 73 /6
    PSRLD_XMM_IMM8, PSRLQ_XMM_IMM8,     // 66 0F 72 /2, 66 0F 73 /2
    PSRAD_XMM_IMM8,                       // 66 0F 72 /4
    PSLLW_XMM_IMM8, PSRLW_XMM_IMM8, PSRAW_XMM_IMM8,  // 66 0F 71 /6, /2, /4
    PSLLDQ_XMM_IMM8, PSRLDQ_XMM_IMM8,   // 66 0F 73 /7, 66 0F 73 /3
    // SSE2 Packed Shifts by register
    PSRLW_XMM_XMMM128, PSRLD_XMM_XMMM128, PSRLQ_XMM_XMMM128,  // 0F D1/D2/D3
    PSRAW_XMM_XMMM128, PSRAD_XMM_XMMM128,                       // 0F E1/E2
    PSLLW_XMM_XMMM128, PSLLD_XMM_XMMM128, PSLLQ_XMM_XMMM128,  // 0F F1/F2/F3

    // ========================================================================
    // SSE Shuffle & Unpack Instructions (CPU-038)
    // ========================================================================
    SHUFPS_XMM_XMMM128_IMM8, SHUFPD_XMM_XMMM128_IMM8,
    PSHUFD_XMM_XMMM128_IMM8, PSHUFLW_XMM_XMMM128_IMM8, PSHUFHW_XMM_XMMM128_IMM8,
    PSHUFW_MM_MMM64_IMM8,
    UNPCKLPS_XMM_XMMM128, UNPCKHPS_XMM_XMMM128,
    UNPCKLPD_XMM_XMMM128, UNPCKHPD_XMM_XMMM128,
    PUNPCKLBW_XMM_XMMM128, PUNPCKLWD_XMM_XMMM128,
    PUNPCKLDQ_XMM_XMMM128, PUNPCKLQDQ_XMM_XMMM128,
    PUNPCKHBW_XMM_XMMM128, PUNPCKHWD_XMM_XMMM128,
    PUNPCKHDQ_XMM_XMMM128, PUNPCKHQDQ_XMM_XMMM128,

    // ========================================================================
    // SSSE3 / SSE4.1 / AES-NI / SHA / PCLMULQDQ (three-byte escapes)
    // ========================================================================
    // SSSE3 (0F 38)
    PSHUFB_XMM_XMMM128,      // 66 0F 38 00
    PHADDW_XMM_XMMM128,      // 66 0F 38 01
    PHADDD_XMM_XMMM128,      // 66 0F 38 02
    PHADDSW_XMM_XMMM128,     // 66 0F 38 03
    PMADDUBSW_XMM_XMMM128,   // 66 0F 38 04
    PHSUBW_XMM_XMMM128,      // 66 0F 38 05
    PHSUBD_XMM_XMMM128,      // 66 0F 38 06
    PHSUBSW_XMM_XMMM128,     // 66 0F 38 07
    PSIGNB_XMM_XMMM128,      // 66 0F 38 08
    PSIGNW_XMM_XMMM128,      // 66 0F 38 09
    PSIGND_XMM_XMMM128,      // 66 0F 38 0A
    PMULHRSW_XMM_XMMM128,    // 66 0F 38 0B
    PABSB_XMM_XMMM128,       // 66 0F 38 1C
    PABSW_XMM_XMMM128,       // 66 0F 38 1D
    PABSD_XMM_XMMM128,       // 66 0F 38 1E
    // SSE4.1 (0F 38)
    PBLENDVB_XMM_XMMM128,    // 66 0F 38 10 (implicit XMM0)
    BLENDVPS_XMM_XMMM128,    // 66 0F 38 14 (implicit XMM0)
    BLENDVPD_XMM_XMMM128,    // 66 0F 38 15 (implicit XMM0)
    PTEST_XMM_XMMM128,       // 66 0F 38 17
    PMOVSXBW_XMM_XMMM64,     // 66 0F 38 20
    PMOVSXBD_XMM_XMMM32,     // 66 0F 38 21
    PMOVSXBQ_XMM_XMMM16,     // 66 0F 38 22
    PMOVSXWD_XMM_XMMM64,     // 66 0F 38 23
    PMOVSXWQ_XMM_XMMM32,     // 66 0F 38 24
    PMOVSXDQ_XMM_XMMM64,     // 66 0F 38 25
    MOVNTDQA_XMM_M128,        // 66 0F 38 2A
    PACKUSDW_XMM_XMMM128,    // 66 0F 38 2B
    PCMPEQQ_XMM_XMMM128,     // 66 0F 38 29
    PMOVZXBW_XMM_XMMM64,     // 66 0F 38 30
    PMOVZXBD_XMM_XMMM32,     // 66 0F 38 31
    PMOVZXBQ_XMM_XMMM16,     // 66 0F 38 32
    PMOVZXWD_XMM_XMMM64,     // 66 0F 38 33
    PMOVZXWQ_XMM_XMMM32,     // 66 0F 38 34
    PMOVZXDQ_XMM_XMMM64,     // 66 0F 38 35
    PMINSB_XMM_XMMM128,      // 66 0F 38 38
    PMINSD_XMM_XMMM128,      // 66 0F 38 39
    PMINUW_XMM_XMMM128,      // 66 0F 38 3A
    PMINUD_XMM_XMMM128,      // 66 0F 38 3B
    PMAXSB_XMM_XMMM128,      // 66 0F 38 3C
    PMAXSD_XMM_XMMM128,      // 66 0F 38 3D
    PMAXUW_XMM_XMMM128,      // 66 0F 38 3E
    PMAXUD_XMM_XMMM128,      // 66 0F 38 3F
    PMULLD_XMM_XMMM128,      // 66 0F 38 40
    PMULDQ_XMM_XMMM128,      // 66 0F 38 28
    PHMINPOSUW_XMM_XMMM128,  // 66 0F 38 41
    // SSE4.2 (0F 38)
    PCMPGTQ_XMM_XMMM128,     // 66 0F 38 37
    CRC32_R32_RM8,            // F2 0F 38 F0
    CRC32_R32_RM16,           // 66 F2 0F 38 F1
    CRC32_R32_RM32,           // F2 0F 38 F1
    CRC32_R64_RM8,            // F2 REX.W 0F 38 F0
    CRC32_R64_RM64,           // F2 REX.W 0F 38 F1
    // MOVBE (byte-swap on load/store)
    MOVBE_R16_M16,            // 66 0F 38 F0
    MOVBE_R32_M32,            // 0F 38 F0
    MOVBE_M16_R16,            // 66 0F 38 F1
    MOVBE_M32_R32,            // 0F 38 F1
    // AES-NI (0F 38)
    AESENC_XMM_XMMM128,      // 66 0F 38 DC
    AESENCLAST_XMM_XMMM128,  // 66 0F 38 DD
    AESDEC_XMM_XMMM128,      // 66 0F 38 DE
    AESDECLAST_XMM_XMMM128,  // 66 0F 38 DF
    AESIMC_XMM_XMMM128,      // 66 0F 38 DB
    // SHA (0F 38) — note: no 66 prefix
    SHA1NEXTE_XMM_XMMM128,   // 0F 38 C8
    SHA1MSG1_XMM_XMMM128,    // 0F 38 C9
    SHA1MSG2_XMM_XMMM128,    // 0F 38 CA
    SHA256RNDS2_XMM_XMMM128, // 0F 38 CB (implicit XMM0)
    SHA256MSG1_XMM_XMMM128,  // 0F 38 CC
    SHA256MSG2_XMM_XMMM128,  // 0F 38 CD
    // SSSE3 (0F 3A)
    PALIGNR_XMM_XMMM128_IMM8,  // 66 0F 3A 0F
    // SSE4.1 (0F 3A)
    ROUNDPS_XMM_XMMM128_IMM8,  // 66 0F 3A 08
    ROUNDPD_XMM_XMMM128_IMM8,  // 66 0F 3A 09
    ROUNDSS_XMM_XMMM32_IMM8,   // 66 0F 3A 0A
    ROUNDSD_XMM_XMMM64_IMM8,   // 66 0F 3A 0B
    BLENDPS_XMM_XMMM128_IMM8,  // 66 0F 3A 0C
    BLENDPD_XMM_XMMM128_IMM8,  // 66 0F 3A 0D
    PBLENDW_XMM_XMMM128_IMM8,  // 66 0F 3A 0E
    PEXTRB_RM8_XMM_IMM8,       // 66 0F 3A 14
    EXTRACTPS_RM32_XMM_IMM8,   // 66 0F 3A 17
    PEXTRD_RM32_XMM_IMM8,      // 66 0F 3A 16
    PEXTRQ_RM64_XMM_IMM8,      // 66 REX.W 0F 3A 16
    PINSRB_XMM_RM8_IMM8,       // 66 0F 3A 20
    INSERTPS_XMM_XMMM32_IMM8,  // 66 0F 3A 21
    PINSRD_XMM_RM32_IMM8,      // 66 0F 3A 22
    PINSRQ_XMM_RM64_IMM8,      // 66 REX.W 0F 3A 22
    DPPS_XMM_XMMM128_IMM8,     // 66 0F 3A 40
    DPPD_XMM_XMMM128_IMM8,     // 66 0F 3A 41
    MPSADBW_XMM_XMMM128_IMM8,  // 66 0F 3A 42
    // SSE4.2 (0F 3A)
    PCMPESTRM_XMM_XMMM128_IMM8, // 66 0F 3A 60
    PCMPESTRI_XMM_XMMM128_IMM8, // 66 0F 3A 61
    PCMPISTRM_XMM_XMMM128_IMM8, // 66 0F 3A 62
    PCMPISTRI_XMM_XMMM128_IMM8, // 66 0F 3A 63
    // PCLMULQDQ (0F 3A)
    PCLMULQDQ_XMM_XMMM128_IMM8, // 66 0F 3A 44
    // AES-NI (0F 3A)
    AESKEYGENASSIST_XMM_XMMM128_IMM8, // 66 0F 3A DF
    // SHA (0F 3A)
    SHA1RNDS4_XMM_XMMM128_IMM8, // 0F 3A CC

    // ========================================================================
    // Legacy decimal / bounds / far-pointer (32-bit user-mode ISA)
    // ========================================================================
    DAA,                    // 27 — #UD in 64-bit
    DAS,                    // 2F
    AAA,                    // 37
    AAS,                    // 3F
    AAM,                    // D4 ib
    AAD,                    // D5 ib
    BOUND_R16_M32,          // 62 /r (16-bit)
    BOUND_R32_M64,          // 62 /r (32-bit)
    LES_R16_M32,            // C4 /r
    LES_R32_M48,
    LDS_R16_M32,            // C5 /r
    LDS_R32_M48,
    LSS_R16_M32,            // 0F B2 /r
    LSS_R32_M48,
    LSS_R64_M80,
    LFS_R16_M32,            // 0F B4 /r
    LFS_R32_M48,
    LFS_R64_M80,
    LGS_R16_M32,            // 0F B5 /r
    LGS_R32_M48,
    LGS_R64_M80,

    // x87
    FFREE_STI,              // DD C0+i
    FBLD_M80,               // DF /4
    FBSTP_M80,              // DF /6

    // CET landing pads (NOP when CET is not enabled)
    ENDBR32,                // F3 0F 1E FB
    ENDBR64,                // F3 0F 1E FA

    // XSAVE family
    XSAVE_M,                // 0F AE /4 mem
    XRSTOR_M,               // 0F AE /5 mem
    XSAVEOPT_M,             // 0F AE /6 mem
    XGETBV,                 // 0F 01 D0

    // AVX zeroing
    VZEROUPPER,             // VEX.128 0F 77
    VZEROALL,               // VEX.256 0F 77

    // ADX
    ADCX_R32_RM32,          // 66 0F 38 F6
    ADCX_R64_RM64,
    ADOX_R32_RM32,          // F3 0F 38 F6
    ADOX_R64_RM64,

    // MOVBE 64-bit
    MOVBE_R64_M64,
    MOVBE_M64_R64,

    // BMI1 / BMI2
    ANDN_R32_R32_RM32,
    ANDN_R64_R64_RM64,
    BEXTR_R32_RM32_R32,
    BEXTR_R64_RM64_R64,
    BLSI_R32_RM32,
    BLSI_R64_RM64,
    BLSMSK_R32_RM32,
    BLSMSK_R64_RM64,
    BLSR_R32_RM32,
    BLSR_R64_RM64,
    BZHI_R32_RM32_R32,
    BZHI_R64_RM64_R64,
    MULX_R32_R32_RM32,
    MULX_R64_R64_RM64,
    PDEP_R32_R32_RM32,
    PDEP_R64_R64_RM64,
    PEXT_R32_R32_RM32,
    PEXT_R64_R64_RM64,
    RORX_R32_RM32_IMM8,
    RORX_R64_RM64_IMM8,
    SARX_R32_RM32_R32,
    SARX_R64_RM64_R64,
    SHRX_R32_RM32_R32,
    SHRX_R64_RM64_R64,
    SHLX_R32_RM32_R32,
    SHLX_R64_RM64_R64,

    // F16C
    VCVTPH2PS_XMM_XMMM64,
    VCVTPH2PS_YMM_XMMM128,
    VCVTPS2PH_XMMM64_XMM_IMM8,
    VCVTPS2PH_XMMM128_YMM_IMM8,

    // AVX / AVX2 unique (VEX-encoded SSE ops reuse the SSE Opcode + prefixes.vex)
    VBROADCASTSS_XMM_M32,
    VBROADCASTSS_YMM_M32,
    VBROADCASTSS_XMM_XMM,
    VBROADCASTSS_YMM_XMM,
    VBROADCASTSD_YMM_M64,
    VBROADCASTSD_YMM_XMM,
    VBROADCASTF128_YMM_M128,
    VPBROADCASTB_XMM_XMMM8,
    VPBROADCASTB_YMM_XMMM8,
    VPBROADCASTW_XMM_XMMM16,
    VPBROADCASTW_YMM_XMMM16,
    VPBROADCASTD_XMM_XMMM32,
    VPBROADCASTD_YMM_XMMM32,
    VPBROADCASTQ_XMM_XMMM64,
    VPBROADCASTQ_YMM_XMMM64,
    VBROADCASTI128_YMM_M128,
    VEXTRACTF128_XMMM128_YMM_IMM8,
    VEXTRACTI128_XMMM128_YMM_IMM8,
    VINSERTF128_YMM_YMM_XMMM128_IMM8,
    VINSERTI128_YMM_YMM_XMMM128_IMM8,
    VPERM2F128_YMM_YMM_YMMM256_IMM8,
    VPERM2I128_YMM_YMM_YMMM256_IMM8,
    VPERMD_YMM_YMM_YMMM256,
    VPERMPS_YMM_YMM_YMMM256,
    VPERMQ_YMM_YMMM256_IMM8,
    VPERMPD_YMM_YMMM256_IMM8,
    VPBLENDD_XMM_XMM_XMMM128_IMM8,
    VPBLENDD_YMM_YMM_YMMM256_IMM8,
    // VEX.66.0F3A.W0 4C /r /is4 — mask in imm8[7:4] (Intel SDM Vol. 2 VPBLENDVB)
    VPBLENDVB_XMM_XMM_XMMM128_XMM,
    VPBLENDVB_YMM_YMM_YMMM256_YMM,
    // VEX.66.0F3A.W0 4A /r /is4 — mask in imm8[7:4] (Intel SDM Vol. 2 VBLENDVPS)
    VBLENDVPS_XMM_XMM_XMMM128_XMM,
    VBLENDVPS_YMM_YMM_YMMM256_YMM,
    // VEX.66.0F3A.W0 4B /r /is4 — mask in imm8[7:4] (Intel SDM Vol. 2 VBLENDVPD)
    VBLENDVPD_XMM_XMM_XMMM128_XMM,
    VBLENDVPD_YMM_YMM_YMMM256_YMM,
    VPSLLVD_XMM_XMM_XMMM128,
    VPSLLVD_YMM_YMM_YMMM256,
    VPSLLVQ_XMM_XMM_XMMM128,
    VPSLLVQ_YMM_YMM_YMMM256,
    VPSRLVD_XMM_XMM_XMMM128,
    VPSRLVD_YMM_YMM_YMMM256,
    VPSRLVQ_XMM_XMM_XMMM128,
    VPSRLVQ_YMM_YMM_YMMM256,
    VPSRAVD_XMM_XMM_XMMM128,
    VPSRAVD_YMM_YMM_YMMM256,
    VGATHERDPS_XMM_VM32X_XMM,
    VGATHERDPS_YMM_VM32Y_YMM,
    VPGATHERDD_XMM_VM32X_XMM,
    VPGATHERDD_YMM_VM32Y_YMM,
    // VEX.66.0F38.W0/W1 8C/8E — VPMASKMOVD/Q load/store (Intel SDM Vol. 2)
    VPMASKMOVD_LOAD,
    VPMASKMOVD_STORE,
    VPMASKMOVQ_LOAD,
    VPMASKMOVQ_STORE,

    // AVX permute-in-lane, test, and float masked move. Like VPMASKMOV* above,
    // these do not encode the L bit in the name: the 128 vs 256 form is read
    // from prefixes.vex_l at execution, which keeps one arm per mnemonic.
    // VEX.66.0F38 0C /r (variable) and VEX.66.0F3A 04 /r ib (immediate).
    VPERMILPS_VAR,
    VPERMILPS_IMM,
    // VEX.66.0F38 0D /r (variable) and VEX.66.0F3A 05 /r ib (immediate).
    VPERMILPD_VAR,
    VPERMILPD_IMM,
    // VEX.66.0F38 0E /r, 0F /r — write ZF/CF from element sign bits.
    VTESTPS,
    VTESTPD,
    // VEX.66.0F38 2C/2E /r (PS load/store), 2D/2F /r (PD load/store).
    // Mask is VEX.vvvv; memory operand only.
    VMASKMOVPS_LOAD,
    VMASKMOVPS_STORE,
    VMASKMOVPD_LOAD,
    VMASKMOVPD_STORE,

    // AVX2 gathers beyond the W0 dword-index forms above. Three distinct
    // index/element shapes; the float and integer members of each pair move
    // identical bits, so they share an execution path.
    VGATHERQPS,     // VEX.66.0F38.W0 93 /r — qword indices, dword elements
    VPGATHERQD,     // VEX.66.0F38.W0 91 /r — same shape as VGATHERQPS
    VGATHERDPD,     // VEX.66.0F38.W1 92 /r — dword indices, qword elements
    VPGATHERDQ,     // VEX.66.0F38.W1 90 /r — same shape as VGATHERDPD
    VGATHERQPD,     // VEX.66.0F38.W1 93 /r — qword indices, qword elements
    VPGATHERQQ,     // VEX.66.0F38.W1 91 /r — same shape as VGATHERQPD

    // FMA3 (L bit selects 128 vs 256)
    VFMADD132PS_XMM_XMM_XMMM128,
    VFMADD132PD_XMM_XMM_XMMM128,
    VFMADD132SS_XMM_XMM_XMMM32,
    VFMADD132SD_XMM_XMM_XMMM64,
    VFMADD213PS_XMM_XMM_XMMM128,
    VFMADD213PD_XMM_XMM_XMMM128,
    VFMADD213SS_XMM_XMM_XMMM32,
    VFMADD213SD_XMM_XMM_XMMM64,
    VFMADD231PS_XMM_XMM_XMMM128,
    VFMADD231PD_XMM_XMM_XMMM128,
    VFMADD231SS_XMM_XMM_XMMM32,
    VFMADD231SD_XMM_XMM_XMMM64,
    VFMSUB132PS_XMM_XMM_XMMM128,
    VFMSUB132PD_XMM_XMM_XMMM128,
    VFMSUB132SS_XMM_XMM_XMMM32,
    VFMSUB132SD_XMM_XMM_XMMM64,
    VFMSUB213PS_XMM_XMM_XMMM128,
    VFMSUB213PD_XMM_XMM_XMMM128,
    VFMSUB213SS_XMM_XMM_XMMM32,
    VFMSUB213SD_XMM_XMM_XMMM64,
    VFMSUB231PS_XMM_XMM_XMMM128,
    VFMSUB231PD_XMM_XMM_XMMM128,
    VFMSUB231SS_XMM_XMM_XMMM32,
    VFMSUB231SD_XMM_XMM_XMMM64,
    VFNMADD132PS_XMM_XMM_XMMM128,
    VFNMADD132PD_XMM_XMM_XMMM128,
    VFNMADD132SS_XMM_XMM_XMMM32,
    VFNMADD132SD_XMM_XMM_XMMM64,
    VFNMADD213PS_XMM_XMM_XMMM128,
    VFNMADD213PD_XMM_XMM_XMMM128,
    VFNMADD213SS_XMM_XMM_XMMM32,
    VFNMADD213SD_XMM_XMM_XMMM64,
    VFNMADD231PS_XMM_XMM_XMMM128,
    VFNMADD231PD_XMM_XMM_XMMM128,
    VFNMADD231SS_XMM_XMM_XMMM32,
    VFNMADD231SD_XMM_XMM_XMMM64,
    VFNMSUB132PS_XMM_XMM_XMMM128,
    VFNMSUB132PD_XMM_XMM_XMMM128,
    VFNMSUB132SS_XMM_XMM_XMMM32,
    VFNMSUB132SD_XMM_XMM_XMMM64,
    VFNMSUB213PS_XMM_XMM_XMMM128,
    VFNMSUB213PD_XMM_XMM_XMMM128,
    VFNMSUB213SS_XMM_XMM_XMMM32,
    VFNMSUB213SD_XMM_XMM_XMMM64,
    VFNMSUB231PS_XMM_XMM_XMMM128,
    VFNMSUB231PD_XMM_XMM_XMMM128,
    VFNMSUB231SS_XMM_XMM_XMMM32,
    VFNMSUB231SD_XMM_XMM_XMMM64,
    VFMADDSUB132PS_XMM_XMM_XMMM128,
    VFMADDSUB213PS_XMM_XMM_XMMM128,
    VFMADDSUB231PS_XMM_XMM_XMMM128,
    VFMADDSUB132PD_XMM_XMM_XMMM128,
    VFMADDSUB213PD_XMM_XMM_XMMM128,
    VFMADDSUB231PD_XMM_XMM_XMMM128,
    VFMSUBADD132PS_XMM_XMM_XMMM128,
    VFMSUBADD213PS_XMM_XMM_XMMM128,
    VFMSUBADD231PS_XMM_XMM_XMMM128,
    VFMSUBADD132PD_XMM_XMM_XMMM128,
    VFMSUBADD213PD_XMM_XMM_XMMM128,
    VFMSUBADD231PD_XMM_XMM_XMMM128,

    // ========================================================================
    // Privileged / I/O port instructions (always #GP in ring-3)
    // ========================================================================
    INSB,               // 6C
    INSW_D,             // 6D
    OUTSB,              // 6E
    OUTSW_D,            // 6F
    IN_AL_IMM8,         // E4
    IN_EAX_IMM8,        // E5
    OUT_IMM8_AL,        // E6
    OUT_IMM8_EAX,       // E7
    IN_AL_DX,           // EC
    IN_EAX_DX,          // ED
    OUT_DX_AL,          // EE
    OUT_DX_EAX,         // EF
    RDMSR,              // 0F 32
    WRMSR,              // 0F 30
    RDPMC,              // 0F 33

    // ========================================================================
    // Late additions
    // ========================================================================
    //
    // These entries are appended rather than filed with their families for a
    // reason that no longer binds: the numeric value of an `Opcode` used to be
    // persisted by an on-disk IR store (removed 2026-10-11), and a mid-enum
    // insertion would have reinterpreted its stale entries. Entries may be
    // filed with their families again. These are left in place because moving
    // them is a renumbering with no benefit.

    // 63 /r in 16/32-bit mode. The same byte is MOVSXD in 64-bit mode only
    // (Intel SDM Vol. 2: ARPL is "N.E." in 64-bit; MOVSXD is 64-bit only).
    ARPL_RM16_R16,      // 63 /r  (non-64-bit modes)

    // Undocumented but architecturally present on every x86 implementation,
    // and emitted by real compilers and hand-written assembly.
    SALC,               // D6     AL = CF ? 0xFF : 0x00; #UD in 64-bit mode
    FFREEP_STI,         // DF C0+i  FFREE ST(i) then pop

    // Exception-generating forms. Each must report the correct instruction
    // LENGTH, which a bare InvalidOpcode decode failure cannot do.
    INT_1,              // F1     ICEBP/INT1 -> #DB
    UD1_R_RM,           // 0F B9 /r
    UD0_R_RM,           // 0F FF /r

    // An EVEX-encoded instruction (62 P0 P1 P2 ...) whose ISA Vitreo's CPUID
    // does not advertise -- today that is every one of them: AVX-512, AVX10,
    // APX and AMX are all hidden. It raises #UD, which is what a processor
    // without those features does.
    //
    // It exists as a named opcode rather than a decode failure so that the
    // #UD is DIAGNOSABLE (an EVEX instruction Vitreo chose not to implement,
    // not corrupt code) and so that its LENGTH is right: without decoding the
    // prefix, the escape byte alone is consumed and the reported instruction
    // is one byte long.
    EVEX_UNSUPPORTED,   // 62 P0 P1 P2 <opcode> <modrm> ...

    // ========================================================================
    // Sentinel - Keep last
    // ========================================================================
    _COUNT
};

/// Get the number of defined opcodes
constexpr usize opcode_count() { return static_cast<usize>(Opcode::_COUNT); }

/// Check if opcode is valid (not INVALID)
[[nodiscard]] constexpr bool is_valid_opcode(Opcode op) noexcept {
    return op != Opcode::INVALID && op < Opcode::_COUNT;
}

/// Get a string name for the opcode (for debugging)
[[nodiscard]] const char* opcode_name(Opcode op) noexcept;

/// True when this instruction is scalar or otherwise 128-bit-only, so a VEX
/// encoding with L=1 is reserved and must raise #UD (Intel SDM Vol. 2A
/// §2.3.6.1 — "VEX.L must be 0, otherwise instructions will #UD").
///
/// This is ISA knowledge about an opcode, so it lives beside the opcode enum
/// and is consulted by BOTH the decoder (which rejects the encoding) and the
/// interpreter's AVX bridge (which used to own the only copy). Keeping one
/// definition matters: while the decoder did not check, a 256-bit `VCOMISS`
/// decoded happily, the interpreter raised #UD at execution, and the ISA
/// matrix counted the form as a missing 256-bit lowering.
[[nodiscard]] bool vex_l_is_reserved(Opcode op) noexcept;

/// True for the VEX.LIG forms (Intel SDM Vol. 2A §3.1.1.2, "VEX.L is
/// ignored"): the scalar SSE arithmetic/compare/convert/move/round forms and
/// scalar FMA. The decoder clears VEX.L on these so that an L=1 encoding --
/// which silicon executes exactly as L=0 -- reaches every consumer as the
/// 128-bit form instead of being reported as a missing 256-bit lowering or,
/// worse, faulted.
[[nodiscard]] bool vex_l_is_ignored(Opcode op) noexcept;

/// True for legacy-map opcodes that have NO VEX encoding at all (the SHA
/// extensions). A VEX prefix on them is #UD on hardware.
[[nodiscard]] bool opcode_has_no_vex_form(Opcode op) noexcept;

/// True for the SSE forms that write ONLY EFLAGS (COMISS/UCOMISS/COMISD/
/// UCOMISD/PTEST): their first operand is a SOURCE. A VEX.128 encoding zeroes
/// YMM[255:128] of the register it WRITES, so these zero nothing -- treating
/// operand 0 as a destination wiped the upper half of a live register
/// (measured on silicon, VEX sweep).
[[nodiscard]] bool sse_writes_only_flags(Opcode op) noexcept;

/// True when a LOCK prefix is architecturally legal on this instruction
/// (Intel SDM Vol. 2, LOCK). The prefix additionally requires a MEMORY
/// destination, which the decoder checks separately.
[[nodiscard]] bool lock_prefix_is_legal(Opcode op) noexcept;

} // namespace vitreo::cpu
