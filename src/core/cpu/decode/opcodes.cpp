// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64

#include <iterator>
#include "opcodes.hpp"

namespace vitreo::cpu {

#include "opcode_names_generated.inc"

const char* opcode_name(Opcode op) noexcept {
    switch (op) {
        case Opcode::INVALID: return "INVALID";
        
        // MOV
        case Opcode::MOV_R8_IMM8: return "MOV r8, imm8";
        case Opcode::MOV_R16_IMM16: return "MOV r16, imm16";
        case Opcode::MOV_R32_IMM32: return "MOV r32, imm32";
        case Opcode::MOV_R64_IMM64: return "MOV r64, imm64";
        case Opcode::MOV_RM8_R8: return "MOV r/m8, r8";
        case Opcode::MOV_RM16_R16: return "MOV r/m16, r16";
        case Opcode::MOV_RM32_R32: return "MOV r/m32, r32";
        case Opcode::MOV_RM16_SREG: return "MOV r/m16, Sreg";
        case Opcode::MOV_SREG_RM16: return "MOV Sreg, r/m16";
        case Opcode::MOV_RM64_R64: return "MOV r/m64, r64";
        case Opcode::MOV_R8_RM8: return "MOV r8, r/m8";
        case Opcode::MOV_R16_RM16: return "MOV r16, r/m16";
        case Opcode::MOV_R32_RM32: return "MOV r32, r/m32";
        case Opcode::MOV_R64_RM64: return "MOV r64, r/m64";
        case Opcode::MOV_RM8_IMM8: return "MOV r/m8, imm8";
        case Opcode::MOV_RM16_IMM16: return "MOV r/m16, imm16";
        case Opcode::MOV_RM32_IMM32: return "MOV r/m32, imm32";
        case Opcode::MOV_RM64_IMM32: return "MOV r/m64, imm32";
        case Opcode::MOV_AL_MOFFS8: return "MOV AL, moffs8";
        case Opcode::MOV_AX_MOFFS16: return "MOV AX, moffs16";
        case Opcode::MOV_EAX_MOFFS32: return "MOV EAX, moffs32";
        case Opcode::MOV_RAX_MOFFS64: return "MOV RAX, moffs64";
        case Opcode::MOV_MOFFS8_AL: return "MOV moffs8, AL";
        case Opcode::MOV_MOFFS16_AX: return "MOV moffs16, AX";
        case Opcode::MOV_MOFFS32_EAX: return "MOV moffs32, EAX";
        case Opcode::MOV_MOFFS64_RAX: return "MOV moffs64, RAX";
        
        // MOVZX/MOVSX
        case Opcode::MOVZX_R16_RM8: return "MOVZX r16, r/m8";
        case Opcode::MOVZX_R32_RM8: return "MOVZX r32, r/m8";
        case Opcode::MOVZX_R64_RM8: return "MOVZX r64, r/m8";
        case Opcode::MOVZX_R32_RM16: return "MOVZX r32, r/m16";
        case Opcode::MOVZX_R64_RM16: return "MOVZX r64, r/m16";
        case Opcode::MOVSX_R16_RM8: return "MOVSX r16, r/m8";
        case Opcode::MOVSX_R32_RM8: return "MOVSX r32, r/m8";
        case Opcode::MOVSX_R64_RM8: return "MOVSX r64, r/m8";
        case Opcode::MOVSX_R32_RM16: return "MOVSX r32, r/m16";
        case Opcode::MOVSX_R64_RM16: return "MOVSX r64, r/m16";
        case Opcode::MOVSXD_R64_RM32: return "MOVSXD r64, r/m32";
        
        // XCHG
        case Opcode::XCHG_AX_R16: return "XCHG AX, r16";
        case Opcode::XCHG_EAX_R32: return "XCHG EAX, r32";
        case Opcode::XCHG_RAX_R64: return "XCHG RAX, r64";
        case Opcode::XCHG_RM8_R8: return "XCHG r/m8, r8";
        case Opcode::XCHG_RM16_R16: return "XCHG r/m16, r16";
        case Opcode::XCHG_RM32_R32: return "XCHG r/m32, r32";
        case Opcode::XCHG_RM64_R64: return "XCHG r/m64, r64";
        
        // LEA
        case Opcode::LEA_R16_M: return "LEA r16, m";
        case Opcode::LEA_R32_M: return "LEA r32, m";
        case Opcode::LEA_R64_M: return "LEA r64, m";
        
        // PUSH
        case Opcode::PUSH_R16: return "PUSH r16";
        case Opcode::PUSH_R32: return "PUSH r32";
        case Opcode::PUSH_R64: return "PUSH r64";
        case Opcode::PUSH_RM16: return "PUSH r/m16";
        case Opcode::PUSH_RM32: return "PUSH r/m32";
        case Opcode::PUSH_RM64: return "PUSH r/m64";
        case Opcode::PUSH_IMM8: return "PUSH imm8";
        case Opcode::PUSH_IMM16: return "PUSH imm16";
        case Opcode::PUSH_IMM32: return "PUSH imm32";
        case Opcode::PUSH_CS: return "PUSH CS";
        case Opcode::PUSH_SS: return "PUSH SS";
        case Opcode::PUSH_DS: return "PUSH DS";
        case Opcode::PUSH_ES: return "PUSH ES";
        case Opcode::PUSH_FS: return "PUSH FS";
        case Opcode::PUSH_GS: return "PUSH GS";
        case Opcode::PUSHF: return "PUSHF";
        case Opcode::PUSHFQ: return "PUSHFQ";
        case Opcode::PUSHA: return "PUSHA";
        
        // POP
        case Opcode::POP_R16: return "POP r16";
        case Opcode::POP_R32: return "POP r32";
        case Opcode::POP_R64: return "POP r64";
        case Opcode::POP_RM16: return "POP r/m16";
        case Opcode::POP_RM32: return "POP r/m32";
        case Opcode::POP_RM64: return "POP r/m64";
        case Opcode::POP_DS: return "POP DS";
        case Opcode::POP_ES: return "POP ES";
        case Opcode::POP_SS: return "POP SS";
        case Opcode::POP_FS: return "POP FS";
        case Opcode::POP_GS: return "POP GS";
        case Opcode::POPF: return "POPF";
        case Opcode::POPFQ: return "POPFQ";
        case Opcode::POPA: return "POPA";
        
        // ADD
        case Opcode::ADD_AL_IMM8: return "ADD AL, imm8";
        case Opcode::ADD_AX_IMM16: return "ADD AX, imm16";
        case Opcode::ADD_EAX_IMM32: return "ADD EAX, imm32";
        case Opcode::ADD_RAX_IMM32: return "ADD RAX, imm32";
        case Opcode::ADD_RM8_IMM8: return "ADD r/m8, imm8";
        case Opcode::ADD_RM16_IMM16: return "ADD r/m16, imm16";
        case Opcode::ADD_RM32_IMM32: return "ADD r/m32, imm32";
        case Opcode::ADD_RM64_IMM32: return "ADD r/m64, imm32";
        case Opcode::ADD_RM16_IMM8: return "ADD r/m16, imm8";
        case Opcode::ADD_RM32_IMM8: return "ADD r/m32, imm8";
        case Opcode::ADD_RM64_IMM8: return "ADD r/m64, imm8";
        case Opcode::ADD_RM8_R8: return "ADD r/m8, r8";
        case Opcode::ADD_RM16_R16: return "ADD r/m16, r16";
        case Opcode::ADD_RM32_R32: return "ADD r/m32, r32";
        case Opcode::ADD_RM64_R64: return "ADD r/m64, r64";
        case Opcode::ADD_R8_RM8: return "ADD r8, r/m8";
        case Opcode::ADD_R16_RM16: return "ADD r16, r/m16";
        case Opcode::ADD_R32_RM32: return "ADD r32, r/m32";
        case Opcode::ADD_R64_RM64: return "ADD r64, r/m64";
        
        // SUB
        case Opcode::SUB_AL_IMM8: return "SUB AL, imm8";
        case Opcode::SUB_AX_IMM16: return "SUB AX, imm16";
        case Opcode::SUB_EAX_IMM32: return "SUB EAX, imm32";
        case Opcode::SUB_RAX_IMM32: return "SUB RAX, imm32";
        case Opcode::SUB_RM8_IMM8: return "SUB r/m8, imm8";
        case Opcode::SUB_RM16_IMM16: return "SUB r/m16, imm16";
        case Opcode::SUB_RM32_IMM32: return "SUB r/m32, imm32";
        case Opcode::SUB_RM64_IMM32: return "SUB r/m64, imm32";
        case Opcode::SUB_RM16_IMM8: return "SUB r/m16, imm8";
        case Opcode::SUB_RM32_IMM8: return "SUB r/m32, imm8";
        case Opcode::SUB_RM64_IMM8: return "SUB r/m64, imm8";
        case Opcode::SUB_RM8_R8: return "SUB r/m8, r8";
        case Opcode::SUB_RM16_R16: return "SUB r/m16, r16";
        case Opcode::SUB_RM32_R32: return "SUB r/m32, r32";
        case Opcode::SUB_RM64_R64: return "SUB r/m64, r64";
        case Opcode::SUB_R8_RM8: return "SUB r8, r/m8";
        case Opcode::SUB_R16_RM16: return "SUB r16, r/m16";
        case Opcode::SUB_R32_RM32: return "SUB r32, r/m32";
        case Opcode::SUB_R64_RM64: return "SUB r64, r/m64";

        // SBB
        case Opcode::SBB_AL_IMM8: return "SBB AL, imm8";
        case Opcode::SBB_AX_IMM16: return "SBB AX, imm16";
        case Opcode::SBB_EAX_IMM32: return "SBB EAX, imm32";
        case Opcode::SBB_RAX_IMM32: return "SBB RAX, imm32";
        case Opcode::SBB_RM8_IMM8: return "SBB r/m8, imm8";
        case Opcode::SBB_RM16_IMM16: return "SBB r/m16, imm16";
        case Opcode::SBB_RM32_IMM32: return "SBB r/m32, imm32";
        case Opcode::SBB_RM64_IMM32: return "SBB r/m64, imm32";
        case Opcode::SBB_RM16_IMM8: return "SBB r/m16, imm8";
        case Opcode::SBB_RM32_IMM8: return "SBB r/m32, imm8";
        case Opcode::SBB_RM64_IMM8: return "SBB r/m64, imm8";
        case Opcode::SBB_RM8_R8: return "SBB r/m8, r8";
        case Opcode::SBB_RM16_R16: return "SBB r/m16, r16";
        case Opcode::SBB_RM32_R32: return "SBB r/m32, r32";
        case Opcode::SBB_RM64_R64: return "SBB r/m64, r64";
        case Opcode::SBB_R8_RM8: return "SBB r8, r/m8";
        case Opcode::SBB_R16_RM16: return "SBB r16, r/m16";
        case Opcode::SBB_R32_RM32: return "SBB r32, r/m32";
        case Opcode::SBB_R64_RM64: return "SBB r64, r/m64";
        
        // CMP
        case Opcode::CMP_AL_IMM8: return "CMP AL, imm8";
        case Opcode::CMP_AX_IMM16: return "CMP AX, imm16";
        case Opcode::CMP_EAX_IMM32: return "CMP EAX, imm32";
        case Opcode::CMP_RAX_IMM32: return "CMP RAX, imm32";
        case Opcode::CMP_RM8_IMM8: return "CMP r/m8, imm8";
        case Opcode::CMP_RM16_IMM16: return "CMP r/m16, imm16";
        case Opcode::CMP_RM32_IMM32: return "CMP r/m32, imm32";
        case Opcode::CMP_RM64_IMM32: return "CMP r/m64, imm32";
        case Opcode::CMP_RM16_IMM8: return "CMP r/m16, imm8";
        case Opcode::CMP_RM32_IMM8: return "CMP r/m32, imm8";
        case Opcode::CMP_RM64_IMM8: return "CMP r/m64, imm8";
        case Opcode::CMP_RM8_R8: return "CMP r/m8, r8";
        case Opcode::CMP_RM16_R16: return "CMP r/m16, r16";
        case Opcode::CMP_RM32_R32: return "CMP r/m32, r32";
        case Opcode::CMP_RM64_R64: return "CMP r/m64, r64";
        case Opcode::CMP_R8_RM8: return "CMP r8, r/m8";
        case Opcode::CMP_R16_RM16: return "CMP r16, r/m16";
        case Opcode::CMP_R32_RM32: return "CMP r32, r/m32";
        case Opcode::CMP_R64_RM64: return "CMP r64, r/m64";
        
        // INC/DEC
        case Opcode::INC_R16: return "INC r16";
        case Opcode::INC_R32: return "INC r32";
        case Opcode::INC_RM8: return "INC r/m8";
        case Opcode::INC_RM16: return "INC r/m16";
        case Opcode::INC_RM32: return "INC r/m32";
        case Opcode::INC_RM64: return "INC r/m64";
        case Opcode::DEC_R16: return "DEC r16";
        case Opcode::DEC_R32: return "DEC r32";
        case Opcode::DEC_RM8: return "DEC r/m8";
        case Opcode::DEC_RM16: return "DEC r/m16";
        case Opcode::DEC_RM32: return "DEC r/m32";
        case Opcode::DEC_RM64: return "DEC r/m64";
        
        // NEG
        case Opcode::NEG_RM8: return "NEG r/m8";
        case Opcode::NEG_RM16: return "NEG r/m16";
        case Opcode::NEG_RM32: return "NEG r/m32";
        case Opcode::NEG_RM64: return "NEG r/m64";
        
        // XOR
        case Opcode::XOR_AL_IMM8: return "XOR AL, imm8";
        case Opcode::XOR_AX_IMM16: return "XOR AX, imm16";
        case Opcode::XOR_EAX_IMM32: return "XOR EAX, imm32";
        case Opcode::XOR_RAX_IMM32: return "XOR RAX, imm32";
        case Opcode::XOR_RM8_IMM8: return "XOR r/m8, imm8";
        case Opcode::XOR_RM16_IMM16: return "XOR r/m16, imm16";
        case Opcode::XOR_RM32_IMM32: return "XOR r/m32, imm32";
        case Opcode::XOR_RM64_IMM32: return "XOR r/m64, imm32";
        case Opcode::XOR_RM16_IMM8: return "XOR r/m16, imm8";
        case Opcode::XOR_RM32_IMM8: return "XOR r/m32, imm8";
        case Opcode::XOR_RM64_IMM8: return "XOR r/m64, imm8";
        case Opcode::XOR_RM8_R8: return "XOR r/m8, r8";
        case Opcode::XOR_RM16_R16: return "XOR r/m16, r16";
        case Opcode::XOR_RM32_R32: return "XOR r/m32, r32";
        case Opcode::XOR_RM64_R64: return "XOR r/m64, r64";
        case Opcode::XOR_R8_RM8: return "XOR r8, r/m8";
        case Opcode::XOR_R16_RM16: return "XOR r16, r/m16";
        case Opcode::XOR_R32_RM32: return "XOR r32, r/m32";
        case Opcode::XOR_R64_RM64: return "XOR r64, r/m64";

        // NOT
        case Opcode::NOT_RM8: return "NOT r/m8";
        case Opcode::NOT_RM16: return "NOT r/m16";
        case Opcode::NOT_RM32: return "NOT r/m32";
        case Opcode::NOT_RM64: return "NOT r/m64";

        // AND
        case Opcode::AND_AL_IMM8: return "AND AL, imm8";
        case Opcode::AND_AX_IMM16: return "AND AX, imm16";
        case Opcode::AND_EAX_IMM32: return "AND EAX, imm32";
        case Opcode::AND_RAX_IMM32: return "AND RAX, imm32";
        case Opcode::AND_RM8_IMM8: return "AND r/m8, imm8";
        case Opcode::AND_RM16_IMM16: return "AND r/m16, imm16";
        case Opcode::AND_RM32_IMM32: return "AND r/m32, imm32";
        case Opcode::AND_RM64_IMM32: return "AND r/m64, imm32";
        case Opcode::AND_RM16_IMM8: return "AND r/m16, imm8";
        case Opcode::AND_RM32_IMM8: return "AND r/m32, imm8";
        case Opcode::AND_RM64_IMM8: return "AND r/m64, imm8";
        case Opcode::AND_RM8_R8: return "AND r/m8, r8";
        case Opcode::AND_RM16_R16: return "AND r/m16, r16";
        case Opcode::AND_RM32_R32: return "AND r/m32, r32";
        case Opcode::AND_RM64_R64: return "AND r/m64, r64";
        case Opcode::AND_R8_RM8: return "AND r8, r/m8";
        case Opcode::AND_R16_RM16: return "AND r16, r/m16";
        case Opcode::AND_R32_RM32: return "AND r32, r/m32";
        case Opcode::AND_R64_RM64: return "AND r64, r/m64";

        // OR
        case Opcode::OR_AL_IMM8: return "OR AL, imm8";
        case Opcode::OR_AX_IMM16: return "OR AX, imm16";
        case Opcode::OR_EAX_IMM32: return "OR EAX, imm32";
        case Opcode::OR_RAX_IMM32: return "OR RAX, imm32";
        case Opcode::OR_RM8_IMM8: return "OR r/m8, imm8";
        case Opcode::OR_RM16_IMM16: return "OR r/m16, imm16";
        case Opcode::OR_RM32_IMM32: return "OR r/m32, imm32";
        case Opcode::OR_RM64_IMM32: return "OR r/m64, imm32";
        case Opcode::OR_RM16_IMM8: return "OR r/m16, imm8";
        case Opcode::OR_RM32_IMM8: return "OR r/m32, imm8";
        case Opcode::OR_RM64_IMM8: return "OR r/m64, imm8";
        case Opcode::OR_RM8_R8: return "OR r/m8, r8";
        case Opcode::OR_RM16_R16: return "OR r/m16, r16";
        case Opcode::OR_RM32_R32: return "OR r/m32, r32";
        case Opcode::OR_RM64_R64: return "OR r/m64, r64";
        case Opcode::OR_R8_RM8: return "OR r8, r/m8";
        case Opcode::OR_R16_RM16: return "OR r16, r/m16";
        case Opcode::OR_R32_RM32: return "OR r32, r/m32";
        case Opcode::OR_R64_RM64: return "OR r64, r/m64";
        
        // TEST
        case Opcode::TEST_AL_IMM8: return "TEST AL, imm8";
        case Opcode::TEST_AX_IMM16: return "TEST AX, imm16";
        case Opcode::TEST_EAX_IMM32: return "TEST EAX, imm32";
        case Opcode::TEST_RAX_IMM32: return "TEST RAX, imm32";
        case Opcode::TEST_RM8_IMM8: return "TEST r/m8, imm8";
        case Opcode::TEST_RM16_IMM16: return "TEST r/m16, imm16";
        case Opcode::TEST_RM32_IMM32: return "TEST r/m32, imm32";
        case Opcode::TEST_RM64_IMM32: return "TEST r/m64, imm32";
        case Opcode::TEST_RM8_R8: return "TEST r/m8, r8";
        case Opcode::TEST_RM16_R16: return "TEST r/m16, r16";
        case Opcode::TEST_RM32_R32: return "TEST r/m32, r32";
        case Opcode::TEST_RM64_R64: return "TEST r/m64, r64";

        // ROR
        case Opcode::ROR_RM32_1: return "ROR r/m32, 1";
        case Opcode::ROR_RM32_IMM8: return "ROR r/m32, imm8";
        
        // JMP
        case Opcode::JMP_REL8: return "JMP rel8";
        case Opcode::JMP_REL16: return "JMP rel16";
        case Opcode::JMP_REL32: return "JMP rel32";
        case Opcode::JMP_RM16: return "JMP r/m16";
        case Opcode::JMP_RM32: return "JMP r/m32";
        case Opcode::JMP_RM64: return "JMP r/m64";
        case Opcode::JMP_PTR16_16: return "JMP ptr16:16";
        case Opcode::JMP_PTR16_32: return "JMP ptr16:32";
        case Opcode::JMP_M16_16: return "JMP m16:16";
        case Opcode::JMP_M16_32: return "JMP m16:32";
        case Opcode::JMP_M16_64: return "JMP m16:64";
        
        // Jcc rel8
        case Opcode::JO_REL8: return "JO rel8";
        case Opcode::JNO_REL8: return "JNO rel8";
        case Opcode::JB_REL8: return "JB rel8";
        case Opcode::JNB_REL8: return "JNB rel8";
        case Opcode::JZ_REL8: return "JZ rel8";
        case Opcode::JNZ_REL8: return "JNZ rel8";
        case Opcode::JBE_REL8: return "JBE rel8";
        case Opcode::JNBE_REL8: return "JNBE rel8";
        case Opcode::JS_REL8: return "JS rel8";
        case Opcode::JNS_REL8: return "JNS rel8";
        case Opcode::JP_REL8: return "JP rel8";
        case Opcode::JNP_REL8: return "JNP rel8";
        case Opcode::JL_REL8: return "JL rel8";
        case Opcode::JNL_REL8: return "JNL rel8";
        case Opcode::JLE_REL8: return "JLE rel8";
        case Opcode::JNLE_REL8: return "JNLE rel8";
        
        // Jcc rel32
        case Opcode::JO_REL32: return "JO rel32";
        case Opcode::JNO_REL32: return "JNO rel32";
        case Opcode::JB_REL32: return "JB rel32";
        case Opcode::JNB_REL32: return "JNB rel32";
        case Opcode::JZ_REL32: return "JZ rel32";
        case Opcode::JNZ_REL32: return "JNZ rel32";
        case Opcode::JBE_REL32: return "JBE rel32";
        case Opcode::JNBE_REL32: return "JNBE rel32";
        case Opcode::JS_REL32: return "JS rel32";
        case Opcode::JNS_REL32: return "JNS rel32";
        case Opcode::JP_REL32: return "JP rel32";
        case Opcode::JNP_REL32: return "JNP rel32";
        case Opcode::JL_REL32: return "JL rel32";
        case Opcode::JNL_REL32: return "JNL rel32";
        case Opcode::JLE_REL32: return "JLE rel32";
        case Opcode::JNLE_REL32: return "JNLE rel32";
        
        // CALL
        case Opcode::CALL_REL16: return "CALL rel16";
        case Opcode::CALL_REL32: return "CALL rel32";
        case Opcode::CALL_RM16: return "CALL r/m16";
        case Opcode::CALL_RM32: return "CALL r/m32";
        case Opcode::CALL_RM64: return "CALL r/m64";
        case Opcode::CALL_PTR16_16: return "CALL ptr16:16";
        case Opcode::CALL_PTR16_32: return "CALL ptr16:32";
        case Opcode::CALL_M16_16: return "CALL m16:16";
        case Opcode::CALL_M16_32: return "CALL m16:32";
        case Opcode::CALL_M16_64: return "CALL m16:64";
        
        // LEAVE / ENTER
        case Opcode::LEAVE: return "LEAVE";
        case Opcode::ENTER: return "ENTER";
        
        // RET
        case Opcode::RET_NEAR: return "RET";
        case Opcode::RET_NEAR_IMM16: return "RET imm16";
        case Opcode::RET_FAR: return "RETF";
        case Opcode::RET_FAR_IMM16: return "RETF imm16";
        
        // INT / IRET
        case Opcode::INT_3: return "INT 3";
        case Opcode::INT_IMM8: return "INT imm8";
        case Opcode::INTO: return "INTO";
        case Opcode::IRET: return "IRET";
        case Opcode::IRETD: return "IRETD";
        case Opcode::IRETQ: return "IRETQ";
        
        // Flags
        case Opcode::CLC: return "CLC";
        case Opcode::STC: return "STC";
        case Opcode::CMC: return "CMC";
        case Opcode::CLD: return "CLD";
        case Opcode::STD: return "STD";
        case Opcode::CLI: return "CLI";
        case Opcode::STI: return "STI";
        
        // Misc
        case Opcode::NOP: return "NOP";
        case Opcode::NOP_RM: return "NOP r/m";
        case Opcode::CPUID: return "CPUID";
        case Opcode::SYSCALL: return "SYSCALL";
        case Opcode::SYSENTER: return "SYSENTER";
        case Opcode::SYSEXIT: return "SYSEXIT";
        case Opcode::RDTSC: return "RDTSC";
        case Opcode::RDTSCP: return "RDTSCP";
        case Opcode::UD2: return "UD2";
        case Opcode::HLT: return "HLT";
        case Opcode::WBINVD: return "WBINVD";
        case Opcode::PAUSE: return "PAUSE";
        case Opcode::LFENCE: return "LFENCE";
        case Opcode::MFENCE: return "MFENCE";
        case Opcode::SFENCE: return "SFENCE";
        
        // SSE data movement
        case Opcode::MOVLPS_XMM_M64: return "MOVLPS xmm, m64";
        case Opcode::MOVLPS_M64_XMM: return "MOVLPS m64, xmm";
        case Opcode::MOVLPD_XMM_M64: return "MOVLPD xmm, m64";
        case Opcode::MOVLPD_M64_XMM: return "MOVLPD m64, xmm";

        // SSE Arithmetic
        case Opcode::ADDSS_XMM_XMMM32: return "ADDSS xmm, xmm/m32";
        case Opcode::ADDSD_XMM_XMMM64: return "ADDSD xmm, xmm/m64";
        case Opcode::ADDPS_XMM_XMMM128: return "ADDPS xmm, xmm/m128";
        case Opcode::ADDPD_XMM_XMMM128: return "ADDPD xmm, xmm/m128";
        case Opcode::SUBSS_XMM_XMMM32: return "SUBSS xmm, xmm/m32";
        case Opcode::SUBSD_XMM_XMMM64: return "SUBSD xmm, xmm/m64";
        case Opcode::SUBPS_XMM_XMMM128: return "SUBPS xmm, xmm/m128";
        case Opcode::SUBPD_XMM_XMMM128: return "SUBPD xmm, xmm/m128";
        case Opcode::MULSS_XMM_XMMM32: return "MULSS xmm, xmm/m32";
        case Opcode::MULSD_XMM_XMMM64: return "MULSD xmm, xmm/m64";
        case Opcode::MULPS_XMM_XMMM128: return "MULPS xmm, xmm/m128";
        case Opcode::MULPD_XMM_XMMM128: return "MULPD xmm, xmm/m128";
        case Opcode::DIVSS_XMM_XMMM32: return "DIVSS xmm, xmm/m32";
        case Opcode::DIVSD_XMM_XMMM64: return "DIVSD xmm, xmm/m64";
        case Opcode::DIVPS_XMM_XMMM128: return "DIVPS xmm, xmm/m128";
        case Opcode::DIVPD_XMM_XMMM128: return "DIVPD xmm, xmm/m128";
        case Opcode::SQRTSS_XMM_XMMM32: return "SQRTSS xmm, xmm/m32";
        case Opcode::SQRTSD_XMM_XMMM64: return "SQRTSD xmm, xmm/m64";
        case Opcode::SQRTPS_XMM_XMMM128: return "SQRTPS xmm, xmm/m128";
        case Opcode::SQRTPD_XMM_XMMM128: return "SQRTPD xmm, xmm/m128";
        case Opcode::MAXSS_XMM_XMMM32: return "MAXSS xmm, xmm/m32";
        case Opcode::MAXSD_XMM_XMMM64: return "MAXSD xmm, xmm/m64";
        case Opcode::MAXPS_XMM_XMMM128: return "MAXPS xmm, xmm/m128";
        case Opcode::MINSS_XMM_XMMM32: return "MINSS xmm, xmm/m32";
        case Opcode::MINSD_XMM_XMMM64: return "MINSD xmm, xmm/m64";
        case Opcode::MINPS_XMM_XMMM128: return "MINPS xmm, xmm/m128";
        case Opcode::COMISS_XMM_XMMM32: return "COMISS xmm, xmm/m32";
        case Opcode::COMISD_XMM_XMMM64: return "COMISD xmm, xmm/m64";
        case Opcode::UCOMISS_XMM_XMMM32: return "UCOMISS xmm, xmm/m32";
        case Opcode::UCOMISD_XMM_XMMM64: return "UCOMISD xmm, xmm/m64";
        case Opcode::RCPSS_XMM_XMMM32: return "RCPSS xmm, xmm/m32";
        case Opcode::RCPPS_XMM_XMMM128: return "RCPPS xmm, xmm/m128";
        case Opcode::RSQRTSS_XMM_XMMM32: return "RSQRTSS xmm, xmm/m32";
        case Opcode::RSQRTPS_XMM_XMMM128: return "RSQRTPS xmm, xmm/m128";
        
        // SSE2 Integer Arithmetic (CPU-037)
        case Opcode::PADDB_XMM_XMMM128: return "PADDB xmm, xmm/m128";
        case Opcode::PADDW_XMM_XMMM128: return "PADDW xmm, xmm/m128";
        case Opcode::PADDD_XMM_XMMM128: return "PADDD xmm, xmm/m128";
        case Opcode::PADDQ_XMM_XMMM128: return "PADDQ xmm, xmm/m128";
        case Opcode::PSUBB_XMM_XMMM128: return "PSUBB xmm, xmm/m128";
        case Opcode::PSUBW_XMM_XMMM128: return "PSUBW xmm, xmm/m128";
        case Opcode::PSUBD_XMM_XMMM128: return "PSUBD xmm, xmm/m128";
        case Opcode::PSUBQ_XMM_XMMM128: return "PSUBQ xmm, xmm/m128";
        case Opcode::PADDUSB_XMM_XMMM128: return "PADDUSB xmm, xmm/m128";
        case Opcode::PADDUSW_XMM_XMMM128: return "PADDUSW xmm, xmm/m128";
        case Opcode::PSUBUSB_XMM_XMMM128: return "PSUBUSB xmm, xmm/m128";
        case Opcode::PSUBUSW_XMM_XMMM128: return "PSUBUSW xmm, xmm/m128";
        case Opcode::PMULLW_XMM_XMMM128: return "PMULLW xmm, xmm/m128";
        case Opcode::PMULHW_XMM_XMMM128: return "PMULHW xmm, xmm/m128";
        case Opcode::PMULHUW_XMM_XMMM128: return "PMULHUW xmm, xmm/m128";
        case Opcode::PMADDWD_XMM_XMMM128: return "PMADDWD xmm, xmm/m128";
        case Opcode::PMINUB_XMM_XMMM128: return "PMINUB xmm, xmm/m128";
        case Opcode::PMAXUB_XMM_XMMM128: return "PMAXUB xmm, xmm/m128";
        case Opcode::PMINSW_XMM_XMMM128: return "PMINSW xmm, xmm/m128";
        case Opcode::PMAXSW_XMM_XMMM128: return "PMAXSW xmm, xmm/m128";
        case Opcode::PAVGB_XMM_XMMM128: return "PAVGB xmm, xmm/m128";
        case Opcode::PAVGW_XMM_XMMM128: return "PAVGW xmm, xmm/m128";
        case Opcode::PSADBW_XMM_XMMM128: return "PSADBW xmm, xmm/m128";
        case Opcode::PAND_XMM_XMMM128: return "PAND xmm, xmm/m128";
        case Opcode::PANDN_XMM_XMMM128: return "PANDN xmm, xmm/m128";
        case Opcode::POR_XMM_XMMM128: return "POR xmm, xmm/m128";
        case Opcode::PXOR_XMM_XMMM128: return "PXOR xmm, xmm/m128";
        case Opcode::PINSRW_XMM_R32M16_IMM8: return "PINSRW xmm, r32/m16, imm8";
        case Opcode::PEXTRW_R32_XMM_IMM8: return "PEXTRW r32, xmm, imm8";
        
        // Three-byte escapes (SSSE3/SSE4.1/AES-NI/SHA/PCLMULQDQ)
        case Opcode::PSHUFB_XMM_XMMM128: return "PSHUFB xmm, xmm/m128";
        case Opcode::PMULLD_XMM_XMMM128: return "PMULLD xmm, xmm/m128";
        case Opcode::PALIGNR_XMM_XMMM128_IMM8: return "PALIGNR xmm, xmm/m128, imm8";
        case Opcode::PEXTRD_RM32_XMM_IMM8: return "PEXTRD r/m32, xmm, imm8";
        case Opcode::PINSRD_XMM_RM32_IMM8: return "PINSRD xmm, r/m32, imm8";
        case Opcode::PEXTRQ_RM64_XMM_IMM8: return "PEXTRQ r/m64, xmm, imm8";
        case Opcode::PINSRQ_XMM_RM64_IMM8: return "PINSRQ xmm, r/m64, imm8";
        case Opcode::AESENC_XMM_XMMM128: return "AESENC xmm, xmm/m128";
        case Opcode::AESENCLAST_XMM_XMMM128: return "AESENCLAST xmm, xmm/m128";
        case Opcode::AESDEC_XMM_XMMM128: return "AESDEC xmm, xmm/m128";
        case Opcode::AESDECLAST_XMM_XMMM128: return "AESDECLAST xmm, xmm/m128";
        case Opcode::AESIMC_XMM_XMMM128: return "AESIMC xmm, xmm/m128";
        case Opcode::AESKEYGENASSIST_XMM_XMMM128_IMM8: return "AESKEYGENASSIST xmm, xmm/m128, imm8";
        case Opcode::PCLMULQDQ_XMM_XMMM128_IMM8: return "PCLMULQDQ xmm, xmm/m128, imm8";
        case Opcode::SHA1NEXTE_XMM_XMMM128: return "SHA1NEXTE xmm, xmm/m128";
        case Opcode::SHA1MSG1_XMM_XMMM128: return "SHA1MSG1 xmm, xmm/m128";
        case Opcode::SHA1MSG2_XMM_XMMM128: return "SHA1MSG2 xmm, xmm/m128";
        case Opcode::SHA1RNDS4_XMM_XMMM128_IMM8: return "SHA1RNDS4 xmm, xmm/m128, imm8";
        case Opcode::SHA256RNDS2_XMM_XMMM128: return "SHA256RNDS2 xmm, xmm/m128";
        case Opcode::SHA256MSG1_XMM_XMMM128: return "SHA256MSG1 xmm, xmm/m128";
        case Opcode::SHA256MSG2_XMM_XMMM128: return "SHA256MSG2 xmm, xmm/m128";
        
        // x87 integer-memory ops (DA m32int / DE m16int)
        case Opcode::FIADD_M16: return "FIADD m16int";
        case Opcode::FIADD_M32: return "FIADD m32int";
        case Opcode::FIMUL_M16: return "FIMUL m16int";
        case Opcode::FIMUL_M32: return "FIMUL m32int";
        case Opcode::FICOM_M16: return "FICOM m16int";
        case Opcode::FICOM_M32: return "FICOM m32int";
        case Opcode::FICOMP_M16: return "FICOMP m16int";
        case Opcode::FICOMP_M32: return "FICOMP m32int";
        case Opcode::FISUB_M16: return "FISUB m16int";
        case Opcode::FISUB_M32: return "FISUB m32int";
        case Opcode::FISUBR_M16: return "FISUBR m16int";
        case Opcode::FISUBR_M32: return "FISUBR m32int";
        case Opcode::FIDIV_M16: return "FIDIV m16int";
        case Opcode::FIDIV_M32: return "FIDIV m32int";
        case Opcode::FIDIVR_M16: return "FIDIVR m16int";
        case Opcode::FIDIVR_M32: return "FIDIVR m32int";

        // MMX instructions (CPU-045)
        case Opcode::MOVQ_MM_MMM64: return "MOVQ mm, mm/m64";
        case Opcode::MOVQ_MMM64_MM: return "MOVQ mm/m64, mm";
        case Opcode::MOVD_MM_RM32: return "MOVD mm, r/m32";
        case Opcode::MOVD_RM32_MM: return "MOVD r/m32, mm";
        case Opcode::PADDW_MM_MMM64: return "PADDW mm, mm/m64";
        case Opcode::PSUBW_MM_MMM64: return "PSUBW mm, mm/m64";
        case Opcode::PADDD_MM_MMM64: return "PADDD mm, mm/m64";
        case Opcode::PACKUSWB_MM_MMM64: return "PACKUSWB mm, mm/m64";
        case Opcode::PACKSSDW_MM_MMM64: return "PACKSSDW mm, mm/m64";
        case Opcode::PMINSW_MM_MMM64: return "PMINSW mm, mm/m64";
        case Opcode::PMAXSW_MM_MMM64: return "PMAXSW mm, mm/m64";
        case Opcode::PADDB_MM_MMM64: return "PADDB mm, mm/m64";
        case Opcode::PSUBB_MM_MMM64: return "PSUBB mm, mm/m64";
        case Opcode::PSUBD_MM_MMM64: return "PSUBD mm, mm/m64";
        case Opcode::PSUBQ_MM_MMM64: return "PSUBQ mm, mm/m64";
        case Opcode::PADDUSB_MM_MMM64: return "PADDUSB mm, mm/m64";
        case Opcode::PADDUSW_MM_MMM64: return "PADDUSW mm, mm/m64";
        case Opcode::PSUBUSB_MM_MMM64: return "PSUBUSB mm, mm/m64";
        case Opcode::PSUBUSW_MM_MMM64: return "PSUBUSW mm, mm/m64";
        case Opcode::PADDSB_MM_MMM64: return "PADDSB mm, mm/m64";
        case Opcode::PADDSW_MM_MMM64: return "PADDSW mm, mm/m64";
        case Opcode::PSUBSB_MM_MMM64: return "PSUBSB mm, mm/m64";
        case Opcode::PSUBSW_MM_MMM64: return "PSUBSW mm, mm/m64";
        case Opcode::PACKSSWB_MM_MMM64: return "PACKSSWB mm, mm/m64";
        case Opcode::PMULLW_MM_MMM64: return "PMULLW mm, mm/m64";
        case Opcode::PMULHW_MM_MMM64: return "PMULHW mm, mm/m64";
        case Opcode::PMULHUW_MM_MMM64: return "PMULHUW mm, mm/m64";
        case Opcode::PMADDWD_MM_MMM64: return "PMADDWD mm, mm/m64";
        case Opcode::PMINUB_MM_MMM64: return "PMINUB mm, mm/m64";
        case Opcode::PMAXUB_MM_MMM64: return "PMAXUB mm, mm/m64";
        case Opcode::PAVGB_MM_MMM64: return "PAVGB mm, mm/m64";
        case Opcode::PAVGW_MM_MMM64: return "PAVGW mm, mm/m64";
        case Opcode::PSADBW_MM_MMM64: return "PSADBW mm, mm/m64";
        case Opcode::PSRLW_MM_IMM8: return "PSRLW mm, imm8";
        case Opcode::PSRAW_MM_IMM8: return "PSRAW mm, imm8";
        case Opcode::PSLLW_MM_IMM8: return "PSLLW mm, imm8";
        case Opcode::PMOVMSKB_R32_MM: return "PMOVMSKB r32, mm";
        case Opcode::PINSRW_MM_R32M16_IMM8: return "PINSRW mm, r32/m16, imm8";
        case Opcode::PEXTRW_R32_MM_IMM8: return "PEXTRW r32, mm, imm8";
        case Opcode::PSRLW_MM_MMM64: return "PSRLW mm, mm/m64";
        case Opcode::PSRAW_MM_MMM64: return "PSRAW mm, mm/m64";
        case Opcode::PSLLW_MM_MMM64: return "PSLLW mm, mm/m64";
        case Opcode::PSRLD_MM_MMM64: return "PSRLD mm, mm/m64";
        case Opcode::PSRAD_MM_MMM64: return "PSRAD mm, mm/m64";
        case Opcode::PSLLD_MM_MMM64: return "PSLLD mm, mm/m64";
        case Opcode::MASKMOVQ_MM_MM: return "MASKMOVQ mm, mm";
        case Opcode::MOVNTQ_M64_MM: return "MOVNTQ m64, mm";
        case Opcode::MASKMOVDQU_XMM_XMM: return "MASKMOVDQU xmm, xmm";
        case Opcode::MOVNTDQ_M128_XMM: return "MOVNTDQ m128, xmm";
        case Opcode::MOVQ2DQ_XMM_MM: return "MOVQ2DQ xmm, mm";
        case Opcode::MOVDQ2Q_MM_XMM: return "MOVDQ2Q mm, xmm";
        case Opcode::PSHUFB_MM_MMM64: return "PSHUFB mm, mm/m64";
        case Opcode::PHADDW_MM_MMM64: return "PHADDW mm, mm/m64";
        case Opcode::PHADDD_MM_MMM64: return "PHADDD mm, mm/m64";
        case Opcode::PHADDSW_MM_MMM64: return "PHADDSW mm, mm/m64";
        case Opcode::PMADDUBSW_MM_MMM64: return "PMADDUBSW mm, mm/m64";
        case Opcode::PHSUBW_MM_MMM64: return "PHSUBW mm, mm/m64";
        case Opcode::PHSUBD_MM_MMM64: return "PHSUBD mm, mm/m64";
        case Opcode::PHSUBSW_MM_MMM64: return "PHSUBSW mm, mm/m64";
        case Opcode::PSIGNB_MM_MMM64: return "PSIGNB mm, mm/m64";
        case Opcode::PSIGNW_MM_MMM64: return "PSIGNW mm, mm/m64";
        case Opcode::PSIGND_MM_MMM64: return "PSIGND mm, mm/m64";
        case Opcode::PMULHRSW_MM_MMM64: return "PMULHRSW mm, mm/m64";
        case Opcode::PABSB_MM_MMM64: return "PABSB mm, mm/m64";
        case Opcode::PABSW_MM_MMM64: return "PABSW mm, mm/m64";
        case Opcode::PABSD_MM_MMM64: return "PABSD mm, mm/m64";
        case Opcode::PALIGNR_MM_MMM64_IMM8: return "PALIGNR mm, mm/m64, imm8";
        case Opcode::MOVNTI_M32_R32: return "MOVNTI m32, r32";
        case Opcode::MOVNTI_M64_R64: return "MOVNTI m64, r64";
        case Opcode::FXRSTOR_M512: return "FXRSTOR m512";
        case Opcode::CLFLUSH_M8: return "CLFLUSH m8";
        case Opcode::CRC32_R32_RM8: return "CRC32 r32, r/m8";
        case Opcode::CRC32_R32_RM16: return "CRC32 r32, r/m16";
        case Opcode::CRC32_R32_RM32: return "CRC32 r32, r/m32";
        case Opcode::CRC32_R64_RM8: return "CRC32 r64, r/m8";
        case Opcode::CRC32_R64_RM64: return "CRC32 r64, r/m64";
        case Opcode::PSHUFW_MM_MMM64_IMM8: return "PSHUFW mm, mm/m64, imm8";
        case Opcode::PSRLD_MM_IMM8: return "PSRLD mm, imm8";
        case Opcode::PSRAD_MM_IMM8: return "PSRAD mm, imm8";
        case Opcode::PSLLD_MM_IMM8: return "PSLLD mm, imm8";
        case Opcode::PADDQ_MM_MMM64: return "PADDQ mm, mm/m64";
        case Opcode::PAND_MM_MMM64: return "PAND mm, mm/m64";
        case Opcode::POR_MM_MMM64: return "POR mm, mm/m64";
        case Opcode::PXOR_MM_MMM64: return "PXOR mm, mm/m64";
        case Opcode::PSLLQ_MM_IMM8: return "PSLLQ mm, imm8";
        case Opcode::PSRLQ_MM_IMM8: return "PSRLQ mm, imm8";
        case Opcode::PSLLQ_MM_MMM64: return "PSLLQ mm, mm/m64";
        case Opcode::PSRLQ_MM_MMM64: return "PSRLQ mm, mm/m64";
        case Opcode::PUNPCKLDQ_MM_MMM64: return "PUNPCKLDQ mm, mm/m64";
        case Opcode::PUNPCKHDQ_MM_MMM64: return "PUNPCKHDQ mm, mm/m64";
        case Opcode::PUNPCKHQDQ_MM_MMM64: return "PUNPCKHQDQ mm, mm/m64";
        case Opcode::CVTPI2PS_XMM_MMM64: return "CVTPI2PS xmm, mm/m64";
        case Opcode::CVTPI2PD_XMM_MMM64: return "CVTPI2PD xmm, mm/m64";
        case Opcode::CVTPS2PI_MM_XMMM64: return "CVTPS2PI mm, xmm/m64";
        case Opcode::CVTPD2PI_MM_XMMM128: return "CVTPD2PI mm, xmm/m128";
        case Opcode::CVTTPS2PI_MM_XMMM64: return "CVTTPS2PI mm, xmm/m64";
        case Opcode::CVTTPD2PI_MM_XMMM128: return "CVTTPD2PI mm, xmm/m128";

        // AVX/AVX2 in-lane permute, test, masked move, and wide gathers.
        case Opcode::VPERMILPS_VAR: return "VPERMILPS x/ymm, x/ymm, x/ymm/m";
        case Opcode::VPERMILPS_IMM: return "VPERMILPS x/ymm, x/ymm/m, imm8";
        case Opcode::VPERMILPD_VAR: return "VPERMILPD x/ymm, x/ymm, x/ymm/m";
        case Opcode::VPERMILPD_IMM: return "VPERMILPD x/ymm, x/ymm/m, imm8";
        case Opcode::VTESTPS: return "VTESTPS x/ymm, x/ymm/m";
        case Opcode::VTESTPD: return "VTESTPD x/ymm, x/ymm/m";
        case Opcode::VMASKMOVPS_LOAD: return "VMASKMOVPS x/ymm, x/ymm, m";
        case Opcode::VMASKMOVPS_STORE: return "VMASKMOVPS m, x/ymm, x/ymm";
        case Opcode::VMASKMOVPD_LOAD: return "VMASKMOVPD x/ymm, x/ymm, m";
        case Opcode::VMASKMOVPD_STORE: return "VMASKMOVPD m, x/ymm, x/ymm";
        case Opcode::VGATHERQPS: return "VGATHERQPS xmm, vm64x/y, xmm";
        case Opcode::VPGATHERQD: return "VPGATHERQD xmm, vm64x/y, xmm";
        case Opcode::VGATHERDPD: return "VGATHERDPD x/ymm, vm32x, x/ymm";
        case Opcode::VPGATHERDQ: return "VPGATHERDQ x/ymm, vm32x, x/ymm";
        case Opcode::VGATHERQPD: return "VGATHERQPD x/ymm, vm64x/y, x/ymm";
        case Opcode::VPGATHERQQ: return "VPGATHERQQ x/ymm, vm64x/y, x/ymm";

        // Late additions. These are named because each one exists to be
        // TOLD APART from something: ARPL from MOVSXD, FFREEP from FFREE,
        // UD0/UD1 from an ordinary decode failure, and EVEX_UNSUPPORTED from
        // corrupt bytes. A "?" in a fault log defeats the point of decoding
        // them distinctly in the first place.
        case Opcode::ARPL_RM16_R16: return "ARPL r/m16, r16";
        case Opcode::SALC: return "SALC";
        case Opcode::FFREEP_STI: return "FFREEP ST(i)";
        case Opcode::INT_1: return "INT1 (ICEBP)";
        case Opcode::UD1_R_RM: return "UD1 r32, r/m32";
        case Opcode::UD0_R_RM: return "UD0 r32, r/m32";
        case Opcode::EVEX_UNSUPPORTED: return "EVEX (unsupported ISA) -> #UD";

        default: break;
    }
    // No hand-written readable form. Fall back to the enum IDENTIFIER rather
    // than "?": 807 of 1365 opcodes had no pretty name, so the Tier 1 coverage
    // histogram -- the metric that decides what to lower next -- listed its
    // top blocker as "?", and reading it meant counting enum entries in a
    // header by hand. Regenerate the table with tools/gen_opcode_names.py.
    const auto i = static_cast<usize>(op);
    if (i < std::size(kOpcodeIdentifiers)) return kOpcodeIdentifiers[i];
    return "?";
}


// Moved here from interpreter/ops/avx_sse_bridge.cpp (where it was
// `is_avx_scalar`) so the decoder can reject the encoding rather than letting
// it decode and fault later. See the declaration in opcodes.hpp.
bool sse_writes_only_flags(Opcode op) noexcept {
    switch (op) {
        case Opcode::COMISS_XMM_XMMM32:
        case Opcode::UCOMISS_XMM_XMMM32:
        case Opcode::COMISD_XMM_XMMM64:
        case Opcode::UCOMISD_XMM_XMMM64:
        case Opcode::PTEST_XMM_XMMM128:
            return true;
        default:
            return false;
    }
}

bool vex_l_is_reserved(Opcode op) noexcept {
    switch (op) {
        // Scalar forms are NOT here: their opcode column reads VEX.LIG
        // ("L ignored", Intel SDM Vol. 2A §3.1.1.2) and real silicon
        // executes them with L=1 exactly as with L=0 -- measured on an
        // i9-12900KF by tools/oracle_probe/vex_scalar_round_probe.c
        // (VROUNDSD/VROUNDSS/VADDSS/VADDSD/VSQRTSD/VCOMISD/VCMPSD/VMOVSD/
        // VMOVSS/VCVTSI2SD/VFMADD213SD/VFMADD213SS, all code=0). Listing
        // them here fabricated a #UD on an encoding hardware runs. They
        // are `vex_l_is_ignored` below, and the decoder clears VEX.L for
        // them so every consumer sees the 128-bit form.
        //
        // What remains is the VEX.128-only set, each entry's encoding
        // column reading VEX.128 (same probe: every one of these raised
        // STATUS_ILLEGAL_INSTRUCTION on silicon with L=1).
        case Opcode::INSERTPS_XMM_XMMM32_IMM8:
        case Opcode::EXTRACTPS_RM32_XMM_IMM8:
        // MASKMOVDQU has only a 128-bit encoding (Intel SDM Vol. 2,
        // VMASKMOVDQU is VEX.128.66.0F.WIG F7 — there is no 256-bit form), so
        // L=1 is reserved here too.
        case Opcode::MASKMOVDQU_XMM_XMM:
        // The rest of the VEX.128-only set, per each instruction's operand
        // encoding table in Intel SDM Vol. 2. Three groups:
        //
        //   * Data movement between an XMM and a GPR or a 64-bit memory
        //     location (VMOVD/VMOVQ, VMOVLPS/VMOVHPS/VMOVLPD/VMOVHPD,
        //     VMOVLHPS/VMOVHLPS) — each names a sub-128-bit quantity, so a
        //     256-bit form would have nothing to widen.
        //   * Element insert/extract (VPINSRB/W/D/Q, VPEXTRB/W/D/Q) — the
        //     element index addresses one 128-bit register.
        //   * Fixed-width algorithms: the string compares, AES-NI, PCLMULQDQ,
        //     PHMINPOSUW and VDPPD. Their 256-bit encodings exist only under
        //     extensions this CPUID does not advertise (VAES, VPCLMULQDQ), so
        //     L=1 is #UD for this CPU. VDPPS is deliberately NOT here: unlike
        //     VDPPD it does have a 256-bit form.
        case Opcode::AESDECLAST_XMM_XMMM128:
        case Opcode::AESDEC_XMM_XMMM128:
        case Opcode::AESENCLAST_XMM_XMMM128:
        case Opcode::AESENC_XMM_XMMM128:
        case Opcode::AESIMC_XMM_XMMM128:
        case Opcode::AESKEYGENASSIST_XMM_XMMM128_IMM8:
        case Opcode::DPPD_XMM_XMMM128_IMM8:
        case Opcode::MOVD_RM32_XMM:
        case Opcode::MOVD_XMM_RM32:
        case Opcode::MOVHLPS_XMM_XMM:
        case Opcode::MOVHPD_M64_XMM:
        case Opcode::MOVHPD_XMM_M64:
        case Opcode::MOVHPS_M64_XMM:
        case Opcode::MOVHPS_XMM_M64:
        case Opcode::MOVLHPS_XMM_XMM:
        case Opcode::MOVLPD_M64_XMM:
        case Opcode::MOVLPD_XMM_M64:
        case Opcode::MOVLPS_M64_XMM:
        case Opcode::MOVLPS_XMM_M64:
        case Opcode::MOVQ_RM64_XMM:
        case Opcode::MOVQ_XMMM64_XMM:
        case Opcode::MOVQ_XMM_RM64:
        case Opcode::MOVQ_XMM_XMMM64:
        case Opcode::PCLMULQDQ_XMM_XMMM128_IMM8:
        case Opcode::PCMPESTRI_XMM_XMMM128_IMM8:
        case Opcode::PCMPESTRM_XMM_XMMM128_IMM8:
        case Opcode::PCMPISTRI_XMM_XMMM128_IMM8:
        case Opcode::PCMPISTRM_XMM_XMMM128_IMM8:
        case Opcode::PEXTRB_RM8_XMM_IMM8:
        case Opcode::PEXTRD_RM32_XMM_IMM8:
        case Opcode::PEXTRQ_RM64_XMM_IMM8:
        case Opcode::PEXTRW_R32_XMM_IMM8:
        case Opcode::PHMINPOSUW_XMM_XMMM128:
        case Opcode::PINSRB_XMM_RM8_IMM8:
        case Opcode::PINSRD_XMM_RM32_IMM8:
        case Opcode::PINSRQ_XMM_RM64_IMM8:
        case Opcode::PINSRW_XMM_R32M16_IMM8:
            return true;
        default:
            return false;
    }
}

bool vex_l_is_ignored(Opcode op) noexcept {
    switch (op) {
        // Intel SDM Vol. 2: every scalar SSE/AVX form is encoded VEX.LIG --
        // VADDSS is "VEX.LIG.F3.0F.WIG 58 /r", VROUNDSD "VEX.LIG.66.0F3A.WIG
        // 0B /r ib", VFMADD213SD "VEX.LIG.66.0F38.W1 A9 /r", VCVTSI2SD
        // "VEX.LIG.F2.0F.W0 2A /r", VCMPSD "VEX.LIG.F2.0F.WIG C2 /r ib" --
        // and §3.1.1.2 defines LIG as "VEX.L is ignored". Confirmed on
        // silicon (see vex_l_is_reserved above).
        case Opcode::ADDSS_XMM_XMMM32:
        case Opcode::SUBSS_XMM_XMMM32:
        case Opcode::MULSS_XMM_XMMM32:
        case Opcode::DIVSS_XMM_XMMM32:
        case Opcode::SQRTSS_XMM_XMMM32:
        case Opcode::MAXSS_XMM_XMMM32:
        case Opcode::MINSS_XMM_XMMM32:
        case Opcode::COMISS_XMM_XMMM32:
        case Opcode::UCOMISS_XMM_XMMM32:
        case Opcode::RCPSS_XMM_XMMM32:
        case Opcode::RSQRTSS_XMM_XMMM32:
        case Opcode::ADDSD_XMM_XMMM64:
        case Opcode::SUBSD_XMM_XMMM64:
        case Opcode::MULSD_XMM_XMMM64:
        case Opcode::DIVSD_XMM_XMMM64:
        case Opcode::SQRTSD_XMM_XMMM64:
        case Opcode::MAXSD_XMM_XMMM64:
        case Opcode::MINSD_XMM_XMMM64:
        case Opcode::COMISD_XMM_XMMM64:
        case Opcode::UCOMISD_XMM_XMMM64:
        case Opcode::CMPSS_XMM_XMMM32_IMM8:
        case Opcode::CMPSD_XMM_XMMM64_IMM8:
        case Opcode::MOVSS_XMM_XMMM32:
        case Opcode::MOVSS_XMMM32_XMM:
        case Opcode::MOVSD_XMM_XMMM64:
        case Opcode::MOVSD_XMMM64_XMM:
        case Opcode::CVTSS2SD_XMM_XMMM32:
        case Opcode::CVTSD2SS_XMM_XMMM64:
        case Opcode::CVTSI2SS_XMM_RM32:
        case Opcode::CVTSI2SS_XMM_RM64:
        case Opcode::CVTSI2SD_XMM_RM32:
        case Opcode::CVTSI2SD_XMM_RM64:
        case Opcode::CVTSS2SI_R32_XMMM32:
        case Opcode::CVTSS2SI_R64_XMMM32:
        case Opcode::CVTSD2SI_R32_XMMM64:
        case Opcode::CVTSD2SI_R64_XMMM64:
        case Opcode::CVTTSS2SI_R32_XMMM32:
        case Opcode::CVTTSS2SI_R64_XMMM32:
        case Opcode::CVTTSD2SI_R32_XMMM64:
        case Opcode::CVTTSD2SI_R64_XMMM64:
        case Opcode::ROUNDSS_XMM_XMMM32_IMM8:
        case Opcode::ROUNDSD_XMM_XMMM64_IMM8:
        case Opcode::VFMADD132SD_XMM_XMM_XMMM64:
        case Opcode::VFMADD132SS_XMM_XMM_XMMM32:
        case Opcode::VFMADD213SD_XMM_XMM_XMMM64:
        case Opcode::VFMADD213SS_XMM_XMM_XMMM32:
        case Opcode::VFMADD231SD_XMM_XMM_XMMM64:
        case Opcode::VFMADD231SS_XMM_XMM_XMMM32:
        case Opcode::VFMSUB132SD_XMM_XMM_XMMM64:
        case Opcode::VFMSUB132SS_XMM_XMM_XMMM32:
        case Opcode::VFMSUB213SD_XMM_XMM_XMMM64:
        case Opcode::VFMSUB213SS_XMM_XMM_XMMM32:
        case Opcode::VFMSUB231SD_XMM_XMM_XMMM64:
        case Opcode::VFMSUB231SS_XMM_XMM_XMMM32:
        case Opcode::VFNMADD132SD_XMM_XMM_XMMM64:
        case Opcode::VFNMADD132SS_XMM_XMM_XMMM32:
        case Opcode::VFNMADD213SD_XMM_XMM_XMMM64:
        case Opcode::VFNMADD213SS_XMM_XMM_XMMM32:
        case Opcode::VFNMADD231SD_XMM_XMM_XMMM64:
        case Opcode::VFNMADD231SS_XMM_XMM_XMMM32:
        case Opcode::VFNMSUB132SD_XMM_XMM_XMMM64:
        case Opcode::VFNMSUB132SS_XMM_XMM_XMMM32:
        case Opcode::VFNMSUB213SD_XMM_XMM_XMMM64:
        case Opcode::VFNMSUB213SS_XMM_XMM_XMMM32:
        case Opcode::VFNMSUB231SD_XMM_XMM_XMMM64:
        case Opcode::VFNMSUB231SS_XMM_XMM_XMMM32:
            return true;
        default:
            return false;
    }
}

bool opcode_has_no_vex_form(Opcode op) noexcept {
    switch (op) {
        // Intel SDM Vol. 2 SHA1NEXTE/SHA1MSG1/SHA1MSG2/SHA1RNDS4/SHA256RNDS2/
        // SHA256MSG1/SHA256MSG2: the only encodings are NP 0F 38/3A; there is
        // no VEX form of any SHA instruction. Silicon raises #UD on
        // `C4 E2 78 CB C1` (tools/oracle_probe/vex_scalar_round_probe.c);
        // Vitreo used to execute it as SHA256RNDS2.
        case Opcode::SHA1NEXTE_XMM_XMMM128:
        case Opcode::SHA1MSG1_XMM_XMMM128:
        case Opcode::SHA1MSG2_XMM_XMMM128:
        case Opcode::SHA1RNDS4_XMM_XMMM128_IMM8:
        case Opcode::SHA256RNDS2_XMM_XMMM128:
        case Opcode::SHA256MSG1_XMM_XMMM128:
        case Opcode::SHA256MSG2_XMM_XMMM128:
            return true;
        default:
            return false;
    }
}

// Intel SDM Vol. 2, LOCK: the prefix is legal only on this set, and only when
// the destination is a MEMORY operand — "if the LOCK prefix is used with one
// of these instructions and the source operand is a memory operand, an
// undefined opcode exception may be generated", and on any other instruction
// it is #UD outright.
//
// Vitreo accepted it on everything, so `LOCK CMP`, `LOCK MOV` and even
// `LOCK FST m32` decoded and executed where hardware faults. That is a
// correctness gap on its own, and it also put ~1,000 encodings no guest can
// execute into the ISA matrix's delegate column.
//
// The caller checks the memory-destination half; this answers the opcode half.
bool lock_prefix_is_legal(Opcode op) noexcept {
    switch (op) {
    case Opcode::ADC_RM16_IMM16:
    case Opcode::ADC_RM16_IMM8:
    case Opcode::ADC_RM16_R16:
    case Opcode::ADC_RM32_IMM32:
    case Opcode::ADC_RM32_IMM8:
    case Opcode::ADC_RM32_R32:
    case Opcode::ADC_RM64_IMM32:
    case Opcode::ADC_RM64_IMM8:
    case Opcode::ADC_RM64_R64:
    case Opcode::ADC_RM8_IMM8:
    case Opcode::ADC_RM8_R8:
    case Opcode::ADD_RM16_IMM16:
    case Opcode::ADD_RM16_IMM8:
    case Opcode::ADD_RM16_R16:
    case Opcode::ADD_RM32_IMM32:
    case Opcode::ADD_RM32_IMM8:
    case Opcode::ADD_RM32_R32:
    case Opcode::ADD_RM64_IMM32:
    case Opcode::ADD_RM64_IMM8:
    case Opcode::ADD_RM64_R64:
    case Opcode::ADD_RM8_IMM8:
    case Opcode::ADD_RM8_R8:
    case Opcode::AND_RM16_IMM16:
    case Opcode::AND_RM16_IMM8:
    case Opcode::AND_RM16_R16:
    case Opcode::AND_RM32_IMM32:
    case Opcode::AND_RM32_IMM8:
    case Opcode::AND_RM32_R32:
    case Opcode::AND_RM64_IMM32:
    case Opcode::AND_RM64_IMM8:
    case Opcode::AND_RM64_R64:
    case Opcode::AND_RM8_IMM8:
    case Opcode::AND_RM8_R8:
    case Opcode::BTC_RM16_IMM8:
    case Opcode::BTC_RM16_R16:
    case Opcode::BTC_RM32_IMM8:
    case Opcode::BTC_RM32_R32:
    case Opcode::BTC_RM64_IMM8:
    case Opcode::BTC_RM64_R64:
    case Opcode::BTR_RM16_IMM8:
    case Opcode::BTR_RM16_R16:
    case Opcode::BTR_RM32_IMM8:
    case Opcode::BTR_RM32_R32:
    case Opcode::BTR_RM64_IMM8:
    case Opcode::BTR_RM64_R64:
    case Opcode::BTS_RM16_IMM8:
    case Opcode::BTS_RM16_R16:
    case Opcode::BTS_RM32_IMM8:
    case Opcode::BTS_RM32_R32:
    case Opcode::BTS_RM64_IMM8:
    case Opcode::BTS_RM64_R64:
    case Opcode::CMPXCHG16B_M128:
    case Opcode::CMPXCHG8B_M64:
    case Opcode::CMPXCHG_RM16_R16:
    case Opcode::CMPXCHG_RM32_R32:
    case Opcode::CMPXCHG_RM64_R64:
    case Opcode::CMPXCHG_RM8_R8:
    case Opcode::DEC_RM16:
    case Opcode::DEC_RM32:
    case Opcode::DEC_RM64:
    case Opcode::DEC_RM8:
    case Opcode::INC_RM16:
    case Opcode::INC_RM32:
    case Opcode::INC_RM64:
    case Opcode::INC_RM8:
    case Opcode::NEG_RM16:
    case Opcode::NEG_RM32:
    case Opcode::NEG_RM64:
    case Opcode::NEG_RM8:
    case Opcode::NOT_RM16:
    case Opcode::NOT_RM32:
    case Opcode::NOT_RM64:
    case Opcode::NOT_RM8:
    case Opcode::OR_RM16_IMM16:
    case Opcode::OR_RM16_IMM8:
    case Opcode::OR_RM16_R16:
    case Opcode::OR_RM32_IMM32:
    case Opcode::OR_RM32_IMM8:
    case Opcode::OR_RM32_R32:
    case Opcode::OR_RM64_IMM32:
    case Opcode::OR_RM64_IMM8:
    case Opcode::OR_RM64_R64:
    case Opcode::OR_RM8_IMM8:
    case Opcode::OR_RM8_R8:
    case Opcode::SBB_RM16_IMM16:
    case Opcode::SBB_RM16_IMM8:
    case Opcode::SBB_RM16_R16:
    case Opcode::SBB_RM32_IMM32:
    case Opcode::SBB_RM32_IMM8:
    case Opcode::SBB_RM32_R32:
    case Opcode::SBB_RM64_IMM32:
    case Opcode::SBB_RM64_IMM8:
    case Opcode::SBB_RM64_R64:
    case Opcode::SBB_RM8_IMM8:
    case Opcode::SBB_RM8_R8:
    case Opcode::SUB_RM16_IMM16:
    case Opcode::SUB_RM16_IMM8:
    case Opcode::SUB_RM16_R16:
    case Opcode::SUB_RM32_IMM32:
    case Opcode::SUB_RM32_IMM8:
    case Opcode::SUB_RM32_R32:
    case Opcode::SUB_RM64_IMM32:
    case Opcode::SUB_RM64_IMM8:
    case Opcode::SUB_RM64_R64:
    case Opcode::SUB_RM8_IMM8:
    case Opcode::SUB_RM8_R8:
    case Opcode::XADD_RM16_R16:
    case Opcode::XADD_RM32_R32:
    case Opcode::XADD_RM64_R64:
    case Opcode::XADD_RM8_R8:
    case Opcode::XCHG_RM16_R16:
    case Opcode::XCHG_RM32_R32:
    case Opcode::XCHG_RM64_R64:
    case Opcode::XCHG_RM8_R8:
    case Opcode::XOR_RM16_IMM16:
    case Opcode::XOR_RM16_IMM8:
    case Opcode::XOR_RM16_R16:
    case Opcode::XOR_RM32_IMM32:
    case Opcode::XOR_RM32_IMM8:
    case Opcode::XOR_RM32_R32:
    case Opcode::XOR_RM64_IMM32:
    case Opcode::XOR_RM64_IMM8:
    case Opcode::XOR_RM64_R64:
    case Opcode::XOR_RM8_IMM8:
    case Opcode::XOR_RM8_R8:
        return true;
    default:
        return false;
    }
}

} // namespace vitreo::cpu
