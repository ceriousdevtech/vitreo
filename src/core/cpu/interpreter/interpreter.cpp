// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// Interpreter - instruction dispatcher

#include "interpreter.hpp"
#include "ops/exec_ops.hpp"
#include "ops/helpers.hpp"
#include "core/memory/mem_watch.hpp"

#include <cstdio>
#include <cstdlib>
#include <atomic>

namespace vitreo::cpu {

namespace {

// Global opcode dispatch counters (env VITREO_COUNT_OPCODES=1 prints at exit)
std::atomic<uint64_t> g_opcode_counts[static_cast<size_t>(Opcode::_COUNT)]{};

bool opcode_counting_enabled() noexcept {
    static const bool enabled = [] {
        const char* value = std::getenv("VITREO_COUNT_OPCODES");
        return value != nullptr && value[0] == '1';
    }();
    return enabled;
}

struct OpcodeCountDump {
    ~OpcodeCountDump() {
        if (!opcode_counting_enabled()) return;
        std::fprintf(stderr, "[opcode-count] BEGIN\n");
        for (size_t i = 0; i < static_cast<size_t>(Opcode::_COUNT); ++i) {
            uint64_t n = g_opcode_counts[i].load();
            if (n) {
                const char* name = opcode_name(static_cast<Opcode>(i));
                std::fprintf(stderr, "[opcode-count] id=%zu %s %llu\n",
                             i, name ? name : "?",
                             (unsigned long long)n);
            }
        }
        std::fprintf(stderr, "[opcode-count] END\n");
    }
};
OpcodeCountDump g_opcode_dump;

} // namespace


// ============================================================================
// Instruction dispatch
// ============================================================================

ExecResult execute_instruction(
    ExecContext& ctx,
    const DecodedInstruction& instr
) noexcept {
    ctx.exception_num_params = 0;
    using namespace ops;
    // Every public/predecoded/delegate entry converges here. Reset per-op
    // address attributes so an earlier 67h cannot affect the next instruction.
    ctx.instr_length = instr.length;
    ctx.address_size_override = instr.prefixes.address_size;

    if (opcode_counting_enabled()) {
        g_opcode_counts[static_cast<size_t>(instr.opcode)].fetch_add(
            1, std::memory_order_relaxed);
    }

    if (read_watch_enabled()) mem_watch_cur_rip() = ctx.cpu.rip;
    if (mem_watch_enabled()) {
        mem_watch_cur_rip() = ctx.cpu.rip;
        auto& r = mem_watch_cur_registers();
        r.eax = ctx.cpu.gpr.get32(static_cast<u8>(RegIndex::RAX));
        r.ecx = ctx.cpu.gpr.get32(static_cast<u8>(RegIndex::RCX));
        r.edx = ctx.cpu.gpr.get32(static_cast<u8>(RegIndex::RDX));
        r.ebx = ctx.cpu.gpr.get32(static_cast<u8>(RegIndex::RBX));
        r.esp = static_cast<u32>(ctx.cpu.sp());
        r.ebp = ctx.cpu.gpr.get32(static_cast<u8>(RegIndex::RBP));
        r.esi = ctx.cpu.gpr.get32(static_cast<u8>(RegIndex::RSI));
        r.edi = ctx.cpu.gpr.get32(static_cast<u8>(RegIndex::RDI));
    }

    if (code_watch_enabled() && code_watch_hit(ctx.cpu.rip)) {
        const u32 esp = static_cast<u32>(ctx.cpu.sp());
        const u32 ret = ctx.mem.read_u32(esp).value_or(0);
        const u32 a1 = ctx.mem.read_u32(esp + 4).value_or(0);
        const u32 a2 = ctx.mem.read_u32(esp + 8).value_or(0);
        const u32 a3 = ctx.mem.read_u32(esp + 12).value_or(0);
        // The full GPR set plus arithmetic flags. A watch is usually set on a
        // branch whose operands live in callee-saved registers, and whether it
        // is taken can hinge on a single flag (a `jb` after a `test`, say), so
        // printing only eax/ecx tends to answer the wrong question.
        const auto& f = ctx.cpu.flags;
        std::fprintf(stderr,
            "[codewatch] rip=0x%llx ret=0x%08x arg1=0x%08x arg2=0x%08x arg3=0x%08x "
            "eax=0x%08x ecx=0x%08x edx=0x%08x ebx=0x%08x esi=0x%08x edi=0x%08x "
            "flags=[%s%s%s%s%s%s]\n",
            (unsigned long long)ctx.cpu.rip, ret, a1, a2, a3,
            ctx.cpu.gpr.get32(static_cast<u8>(RegIndex::RAX)),
            ctx.cpu.gpr.get32(static_cast<u8>(RegIndex::RCX)),
            ctx.cpu.gpr.get32(static_cast<u8>(RegIndex::RDX)),
            ctx.cpu.gpr.get32(static_cast<u8>(RegIndex::RBX)),
            ctx.cpu.gpr.get32(static_cast<u8>(RegIndex::RSI)),
            ctx.cpu.gpr.get32(static_cast<u8>(RegIndex::RDI)),
            f.cf() ? "CF " : "", f.zf() ? "ZF " : "", f.sf() ? "SF " : "",
            f.of() ? "OF " : "", f.af() ? "AF " : "", f.pf() ? "PF" : "");
        // Raw stack window. Three named args cover a scalar-argument function,
        // but anything taking a struct by value (a 16-byte timestamp, say)
        // spills past them, and then the interesting operand is off the end of
        // the line. Eight slots covers two such structs.
        {
            std::fprintf(stderr, "[codewatch]   stack:");
            for (u32 i = 1; i <= 8; ++i) {
                std::fprintf(stderr, " [+%02x]=0x%08x", i * 4,
                             ctx.mem.read_u32(esp + i * 4).value_or(0));
            }
            std::fprintf(stderr, "\n");
        }
        // Follow each arg that looks like a guest pointer one level, and show
        // the dword just below the target too. Heap-block headers (allocator
        // back-pointers, size/cookie words) live at [p-4], so an out-of-range
        // value there is usually the whole story for a CRT block-validation
        // failure.
        const u32 args[3] = {a1, a2, a3};
        for (u32 i = 0; i < 3; ++i) {
            const u32 p = args[i];
            if (p < 0x1000) continue;
            const auto below = ctx.mem.read_u32(p - 4);
            const auto at = ctx.mem.read_u32(p);
            if (!below.is_ok() && !at.is_ok()) continue;
            std::fprintf(stderr,
                "[codewatch]   arg%u=0x%08x [-4]=0x%08x [0]=0x%08x\n",
                i + 1, p, below.value_or(0), at.value_or(0));
        }
        // Dump the ASCII string pointed to by ESI and EDI. String-processing
        // code (parsers, tokenizers) keeps its cursor/buffer in these; printing
        // the bytes there is usually what identifies a malformed input.
        auto dump_str = [&](const char* who, u32 p) {
            if (p < 0x1000) return;
            // Show a window that begins 40 bytes before the cursor so the start
            // of the string a parser is mid-way through is visible, with '|'
            // marking the cursor position.
            const u32 lo = p > 0x1028 ? p - 40 : 0x1000;
            char s[160];
            u32 n = 0;
            for (u32 a = lo; a < p + 96 && n < 150; ++a) {
                if (a == p) s[n++] = '|';
                auto b = ctx.mem.read_u8(a);
                if (!b.is_ok()) break;
                u8 c = b.value();
                s[n++] = (c >= 0x20 && c < 0x7f) ? static_cast<char>(c)
                                                 : (c == 0 ? '#' : '.');
            }
            s[n] = '\0';
            std::fprintf(stderr, "[codewatch]   %s=0x%08x win[-40..]=\"%s\"\n", who, p, s);
        };
        dump_str("esi", ctx.cpu.gpr.get32(static_cast<u8>(RegIndex::RSI)));
        dump_str("edi", ctx.cpu.gpr.get32(static_cast<u8>(RegIndex::RDI)));
    }

    // VEX-encoded SSE family reuses the SSE Opcode with prefixes.vex set.
    if (instr.prefixes.vex && opcode_is_vex_sse_family(instr.opcode)) {
        return exec_avx_sse_bridge(ctx, instr);
    }

    switch (instr.opcode) {
        // NOP variations
        case Opcode::NOP:
        case Opcode::NOP_RM:
        case Opcode::PAUSE:
        case Opcode::LFENCE:
        case Opcode::MFENCE:
        case Opcode::SFENCE:
        case Opcode::XLAT:
        case Opcode::ENDBR32:
        case Opcode::ENDBR64:
            return exec_misc(ctx, instr);
        
        // MOV instructions
        case Opcode::MOV_R8_IMM8:
        case Opcode::MOV_R16_IMM16:
        case Opcode::MOV_R32_IMM32:
        case Opcode::MOV_R64_IMM64:
        case Opcode::MOV_RM8_IMM8:
        case Opcode::MOV_RM16_IMM16:
        case Opcode::MOV_RM32_IMM32:
        case Opcode::MOV_RM64_IMM32:
        case Opcode::MOV_RM8_R8:
        case Opcode::MOV_RM16_R16:
        case Opcode::MOV_RM32_R32:
        case Opcode::MOV_RM64_R64:
        case Opcode::MOV_R8_RM8:
        case Opcode::MOV_R16_RM16:
        case Opcode::MOV_R32_RM32:
        case Opcode::MOV_R64_RM64:
        // MOV moffs (A0-A3): absolute address load/store
        case Opcode::MOV_AL_MOFFS8:
        case Opcode::MOV_AX_MOFFS16:
        case Opcode::MOV_EAX_MOFFS32:
        case Opcode::MOV_RAX_MOFFS64:
        case Opcode::MOV_MOFFS8_AL:
        case Opcode::MOV_MOFFS16_AX:
        case Opcode::MOV_MOFFS32_EAX:
        case Opcode::MOV_MOFFS64_RAX:
        case Opcode::MOV_RM16_SREG:
        case Opcode::MOV_SREG_RM16:
            return exec_mov(ctx, instr);
        
        // ADD instructions
        case Opcode::ADD_AL_IMM8:
        case Opcode::ADD_AX_IMM16:
        case Opcode::ADD_EAX_IMM32:
        case Opcode::ADD_RAX_IMM32:
        case Opcode::ADD_RM8_IMM8:
        case Opcode::ADD_RM16_IMM16:
        case Opcode::ADD_RM32_IMM32:
        case Opcode::ADD_RM64_IMM32:
        case Opcode::ADD_RM16_IMM8:
        case Opcode::ADD_RM32_IMM8:
        case Opcode::ADD_RM64_IMM8:
        case Opcode::ADD_RM8_R8:
        case Opcode::ADD_RM16_R16:
        case Opcode::ADD_RM32_R32:
        case Opcode::ADD_RM64_R64:
        case Opcode::ADD_R8_RM8:
        case Opcode::ADD_R16_RM16:
        case Opcode::ADD_R32_RM32:
        case Opcode::ADD_R64_RM64:
            return exec_add(ctx, instr);
        
        // SUB instructions
        case Opcode::SUB_AL_IMM8:
        case Opcode::SUB_AX_IMM16:
        case Opcode::SUB_EAX_IMM32:
        case Opcode::SUB_RAX_IMM32:
        case Opcode::SUB_RM8_IMM8:
        case Opcode::SUB_RM16_IMM16:
        case Opcode::SUB_RM32_IMM32:
        case Opcode::SUB_RM64_IMM32:
        case Opcode::SUB_RM16_IMM8:
        case Opcode::SUB_RM32_IMM8:
        case Opcode::SUB_RM64_IMM8:
        case Opcode::SUB_RM8_R8:
        case Opcode::SUB_RM16_R16:
        case Opcode::SUB_RM32_R32:
        case Opcode::SUB_RM64_R64:
        case Opcode::SUB_R8_RM8:
        case Opcode::SUB_R16_RM16:
        case Opcode::SUB_R32_RM32:
        case Opcode::SUB_R64_RM64:
            return exec_sub(ctx, instr);
        
        // XOR instructions
        case Opcode::XOR_AL_IMM8:
        case Opcode::XOR_AX_IMM16:
        case Opcode::XOR_EAX_IMM32:
        case Opcode::XOR_RAX_IMM32:
        case Opcode::XOR_RM8_IMM8:
        case Opcode::XOR_RM16_IMM16:
        case Opcode::XOR_RM32_IMM32:
        case Opcode::XOR_RM64_IMM32:
        case Opcode::XOR_RM16_IMM8:
        case Opcode::XOR_RM32_IMM8:
        case Opcode::XOR_RM64_IMM8:
        case Opcode::XOR_RM8_R8:
        case Opcode::XOR_RM16_R16:
        case Opcode::XOR_RM32_R32:
        case Opcode::XOR_RM64_R64:
        case Opcode::XOR_R8_RM8:
        case Opcode::XOR_R16_RM16:
        case Opcode::XOR_R32_RM32:
        case Opcode::XOR_R64_RM64:
            return exec_xor(ctx, instr);
        
        // CMP instructions
        case Opcode::CMP_AL_IMM8:
        case Opcode::CMP_AX_IMM16:
        case Opcode::CMP_EAX_IMM32:
        case Opcode::CMP_RAX_IMM32:
        case Opcode::CMP_RM8_IMM8:
        case Opcode::CMP_RM16_IMM16:
        case Opcode::CMP_RM32_IMM32:
        case Opcode::CMP_RM64_IMM32:
        case Opcode::CMP_RM16_IMM8:
        case Opcode::CMP_RM32_IMM8:
        case Opcode::CMP_RM64_IMM8:
        case Opcode::CMP_RM8_R8:
        case Opcode::CMP_RM16_R16:
        case Opcode::CMP_RM32_R32:
        case Opcode::CMP_RM64_R64:
        case Opcode::CMP_R8_RM8:
        case Opcode::CMP_R16_RM16:
        case Opcode::CMP_R32_RM32:
        case Opcode::CMP_R64_RM64:
            return exec_cmp(ctx, instr);
        
        // TEST instructions
        case Opcode::TEST_AL_IMM8:
        case Opcode::TEST_AX_IMM16:
        case Opcode::TEST_EAX_IMM32:
        case Opcode::TEST_RAX_IMM32:
        case Opcode::TEST_RM8_IMM8:
        case Opcode::TEST_RM16_IMM16:
        case Opcode::TEST_RM32_IMM32:
        case Opcode::TEST_RM64_IMM32:
        case Opcode::TEST_RM8_R8:
        case Opcode::TEST_RM16_R16:
        case Opcode::TEST_RM32_R32:
        case Opcode::TEST_RM64_R64:
            return exec_test(ctx, instr);
        
        // AND instructions
        case Opcode::AND_AL_IMM8:
        case Opcode::AND_AX_IMM16:
        case Opcode::AND_EAX_IMM32:
        case Opcode::AND_RAX_IMM32:
        case Opcode::AND_RM8_IMM8:
        case Opcode::AND_RM16_IMM16:
        case Opcode::AND_RM32_IMM32:
        case Opcode::AND_RM64_IMM32:
        case Opcode::AND_RM16_IMM8:
        case Opcode::AND_RM32_IMM8:
        case Opcode::AND_RM64_IMM8:
        case Opcode::AND_RM8_R8:
        case Opcode::AND_RM16_R16:
        case Opcode::AND_RM32_R32:
        case Opcode::AND_RM64_R64:
        case Opcode::AND_R8_RM8:
        case Opcode::AND_R16_RM16:
        case Opcode::AND_R32_RM32:
        case Opcode::AND_R64_RM64:
            return exec_and(ctx, instr);
        
        // OR instructions
        case Opcode::OR_AL_IMM8:
        case Opcode::OR_AX_IMM16:
        case Opcode::OR_EAX_IMM32:
        case Opcode::OR_RAX_IMM32:
        case Opcode::OR_RM8_IMM8:
        case Opcode::OR_RM16_IMM16:
        case Opcode::OR_RM32_IMM32:
        case Opcode::OR_RM64_IMM32:
        case Opcode::OR_RM16_IMM8:
        case Opcode::OR_RM32_IMM8:
        case Opcode::OR_RM64_IMM8:
        case Opcode::OR_RM8_R8:
        case Opcode::OR_RM16_R16:
        case Opcode::OR_RM32_R32:
        case Opcode::OR_RM64_R64:
        case Opcode::OR_R8_RM8:
        case Opcode::OR_R16_RM16:
        case Opcode::OR_R32_RM32:
        case Opcode::OR_R64_RM64:
            return exec_or(ctx, instr);
        
        // NOT instructions
        case Opcode::NOT_RM8:
        case Opcode::NOT_RM16:
        case Opcode::NOT_RM32:
        case Opcode::NOT_RM64:
            return exec_not(ctx, instr);
        
        // PUSH instructions
        case Opcode::PUSH_R16:
        case Opcode::PUSH_R32:
        case Opcode::PUSH_R64:
        case Opcode::PUSH_RM16:
        case Opcode::PUSH_RM32:
        case Opcode::PUSH_RM64:
        case Opcode::PUSH_IMM8:
        case Opcode::PUSH_IMM16:
        case Opcode::PUSH_IMM32:
        case Opcode::PUSH_CS:
        case Opcode::PUSH_SS:
        case Opcode::PUSH_DS:
        case Opcode::PUSH_ES:
        case Opcode::PUSH_FS:
        case Opcode::PUSH_GS:
            return exec_push(ctx, instr);
        
        // PUSHA — push EAX,ECX,EDX,EBX,original-ESP,EBP,ESI,EDI
        case Opcode::PUSHA: {
            // Intel SDM Vol. 2 PUSHA: a faulting slot write commits no
            // register, so ESP moves once, after all eight slots are written.
            const u32 orig_esp = ctx.cpu.gpr.get32(4); // ESP
            constexpr u8 order[] = {0,1,2,3,4,5,6,7}; // EAX,ECX,EDX,EBX,ESP,EBP,ESI,EDI
            for (u8 i = 0; i < 8; ++i) {
                const u8 r = order[i];
                const u32 val = (r == 4) ? orig_esp : ctx.cpu.gpr.get32(r);
                const u64 slot = static_cast<u32>(orig_esp - 4u * (i + 1u));
                auto res = ops::write_memory(ctx, slot, val, 4);
                if (!res) {
                    // STATUS_ACCESS_VIOLATION, a write of the slot
                    // (stack_access_fault_probe.expected, `pushad`).
                    record_write_access_violation(ctx, slot, 4);
                    ctx.exception = CpuException::PageFault;
                    return ExecResult::Exception;
                }
            }
            ctx.cpu.set_sp(static_cast<u32>(orig_esp - 32u));
            return ExecResult::Continue;
        }
        // POPA — pop EDI,ESI,EBP,skip-ESP,EBX,EDX,ECX,EAX
        case Opcode::POPA: {
            // Windows on Intel (stack_access_fault_probe.expected, `popad
            // partial`): a fault on slot i leaves ESP at its starting value,
            // but the registers popped from slots 0..i-1 HOLD THEIR POPPED
            // VALUES. So each register commits as its slot is read, and ESP
            // moves once, after the eighth slot.
            constexpr u8 order[] = {7,6,5,4,3,2,1,0}; // EDI,ESI,EBP,ESP(skip),EBX,EDX,ECX,EAX
            const u32 esp = ctx.cpu.gpr.get32(4);
            for (u8 i = 0; i < 8; ++i) {
                const u64 slot = static_cast<u32>(esp + 4u * i);
                auto val = ops::read_memory(ctx, slot, 4);
                if (!val) {
                    record_read_access_violation(ctx, slot, 4);
                    ctx.exception = CpuException::PageFault;
                    return ExecResult::Exception;
                }
                if (order[i] != 4) {  // the ESP slot is discarded
                    ctx.cpu.gpr.set32(order[i], static_cast<u32>(val.value()));
                }
            }
            ctx.cpu.set_sp(esp + 32u);
            return ExecResult::Continue;
        }

        // PUSHF/POPF — operand-size attribute selects width (Intel SDM Vol. 2:
        // PUSHF/PUSHFD/PUSHFQ, POPF/POPFD/POPFQ). In 32-bit mode the default is
        // 4 bytes; a 66H prefix selects a 2-byte FLAGS push/pop. Packers such as
        // Petite use `66 9C` / `66 9D` and then address into the saved frame with
        // fixed displacements — pushing 4 bytes instead of 2 corrupts the SEH
        // handler slot and breaks their intentional ImageBase store fault.
        case Opcode::PUSHF: {
            // Materialize lazy flags first — PUSHF must observe the architectural
            // EFLAGS from the preceding arithmetic op, not a stale rflags image.
            ctx.cpu.materialize_lazy_flags();
            u8 size;
            u64 flags_image;
            if (ctx.cpu.is_64bit()) {
                // 64-bit default is PUSHFQ (8); 66H selects 16-bit PUSHF.
                if (instr.prefixes.operand_size) {
                    size = 2;
                    flags_image = ctx.cpu.flags.rflags & 0xFFFFu;
                } else {
                    size = 8;
                    flags_image = ctx.cpu.flags.rflags & 0x00FCFFFFull;
                }
            } else if (instr.prefixes.operand_size) {
                size = 2;
                flags_image = ctx.cpu.flags.rflags & 0xFFFFu;
            } else {
                size = 4;
                flags_image = static_cast<u32>(ctx.cpu.flags.rflags) & 0x00FCFFFFu;
            }
            flags_image |= 0x2u; // bit 1 always set
            auto r = push_value(ctx, flags_image, size);
            if (!r) { ctx.exception = stack_access_exception(r.error()); return ExecResult::Exception; }
            return ExecResult::Continue;
        }
        case Opcode::POPF: {
            u8 size;
            if (ctx.cpu.is_64bit()) {
                size = instr.prefixes.operand_size ? 2 : 8;
            } else {
                size = instr.prefixes.operand_size ? 2 : 4;
            }
            auto val = pop_value(ctx, size);
            if (!val) { ctx.exception = stack_access_exception(val.error()); return ExecResult::Exception; }
            // Preserve reserved bits, allow user-mode bits. Width only affects
            // how many stack bytes are consumed; the writable EFLAGS mask is
            // the same (high bits of a 16-bit pop are zero in val).
            constexpr u64 user_mask = 0x00244FD5; // CF,PF,AF,ZF,SF,TF,IF,DF,OF,AC,ID
            u64 new_flags = (ctx.cpu.flags.rflags & ~user_mask) | (val.value() & user_mask);
            new_flags |= 0x0002; // bit 1 always set
            ctx.cpu.flags.rflags = new_flags;
            ctx.cpu.set_flag_op(CpuState::FlagOp::None);
            return ExecResult::Continue;
        }
        
        // POP instructions
        case Opcode::POP_R16:
        case Opcode::POP_R32:
        case Opcode::POP_R64:
        case Opcode::POP_RM16:
        case Opcode::POP_RM32:
        case Opcode::POP_RM64:
        case Opcode::POP_DS:
        case Opcode::POP_ES:
        case Opcode::POP_SS:
        case Opcode::POP_FS:
        case Opcode::POP_GS:
            return exec_pop(ctx, instr);
        
        // JMP instructions
        case Opcode::JMP_REL8:
        case Opcode::JMP_REL16:
        case Opcode::JMP_REL32:
            return exec_jmp(ctx, instr);
        
        // Conditional jumps
        case Opcode::JO_REL8:
        case Opcode::JO_REL32:
        case Opcode::JNO_REL8:
        case Opcode::JNO_REL32:
        case Opcode::JB_REL8:
        case Opcode::JB_REL32:
        case Opcode::JNB_REL8:
        case Opcode::JNB_REL32:
        case Opcode::JZ_REL8:
        case Opcode::JZ_REL32:
        case Opcode::JNZ_REL8:
        case Opcode::JNZ_REL32:
        case Opcode::JBE_REL8:
        case Opcode::JBE_REL32:
        case Opcode::JNBE_REL8:
        case Opcode::JNBE_REL32:
        case Opcode::JS_REL8:
        case Opcode::JS_REL32:
        case Opcode::JNS_REL8:
        case Opcode::JNS_REL32:
        case Opcode::JP_REL8:
        case Opcode::JP_REL32:
        case Opcode::JNP_REL8:
        case Opcode::JNP_REL32:
        case Opcode::JL_REL8:
        case Opcode::JL_REL32:
        case Opcode::JNL_REL8:
        case Opcode::JNL_REL32:
        case Opcode::JLE_REL8:
        case Opcode::JLE_REL32:
        case Opcode::JNLE_REL8:
        case Opcode::JNLE_REL32:
            return exec_jcc(ctx, instr);

        // LOOP/JCXZ family
        case Opcode::LOOP_REL8:
        case Opcode::LOOPE_REL8:
        case Opcode::LOOPNE_REL8:
        case Opcode::JCXZ_REL8:
        case Opcode::JECXZ_REL8:
        case Opcode::JRCXZ_REL8:
            return exec_loop_jcxz(ctx, instr);
        
        // CALL instruction (relative)
        case Opcode::CALL_REL16:
        case Opcode::CALL_REL32:
            return exec_call(ctx, instr);
        
        // CALL instruction (indirect r/m)
        case Opcode::CALL_RM16:
        case Opcode::CALL_RM32:
        case Opcode::CALL_RM64:
            return exec_call_indirect(ctx, instr);

        // Far CALL (9A ptr16:16/32, FF /3 m16:16/32/64)
        case Opcode::CALL_PTR16_16:
        case Opcode::CALL_PTR16_32:
        case Opcode::CALL_M16_16:
        case Opcode::CALL_M16_32:
        case Opcode::CALL_M16_64:
            return exec_call_far(ctx, instr);
        
        // JMP instruction (indirect r/m)
        case Opcode::JMP_RM16:
        case Opcode::JMP_RM32:
        case Opcode::JMP_RM64:
            return exec_jmp_indirect(ctx, instr);

        // Far JMP (EA ptr16:16/32, FF /5 m16:16/32/64)
        case Opcode::JMP_PTR16_16:
        case Opcode::JMP_PTR16_32:
        case Opcode::JMP_M16_16:
        case Opcode::JMP_M16_32:
        case Opcode::JMP_M16_64:
            return exec_jmp_far(ctx, instr);
        
        // LEAVE
        case Opcode::LEAVE:
            return exec_leave(ctx, instr);
        
        // ENTER
        case Opcode::ENTER:
            return exec_enter(ctx, instr);
        
        // RET instruction
        case Opcode::RET_NEAR:
        case Opcode::RET_NEAR_IMM16:
            return exec_ret(ctx, instr);

        // Far RET (RETF / RETF imm16) — Intel SDM Vol. 2 RET
        case Opcode::RET_FAR:
        case Opcode::RET_FAR_IMM16:
            return exec_ret_far(ctx, instr);

        // IRET/IRETD/IRETQ — Intel SDM Vol. 2 (user-mode same-privilege)
        case Opcode::IRET:
        case Opcode::IRETD:
        case Opcode::IRETQ:
            return exec_iret(ctx, instr);
        
        // LEA instruction
        case Opcode::LEA_R16_M:
        case Opcode::LEA_R32_M:
        case Opcode::LEA_R64_M:
            return exec_lea(ctx, instr);
        
        // MOVZX/MOVSX instructions
        case Opcode::MOVZX_R16_RM8:
        case Opcode::MOVZX_R32_RM8:
        case Opcode::MOVZX_R64_RM8:
        case Opcode::MOVZX_R32_RM16:
        case Opcode::MOVZX_R64_RM16:
        case Opcode::MOVSX_R16_RM8:
        case Opcode::MOVSX_R32_RM8:
        case Opcode::MOVSX_R64_RM8:
        case Opcode::MOVSX_R32_RM16:
        case Opcode::MOVSX_R64_RM16:
        case Opcode::MOVSXD_R64_RM32:
            return exec_movzx_movsx(ctx, instr);
        
        // INT instructions
        case Opcode::INT_3:
        case Opcode::INT_1:
        case Opcode::INT_IMM8:
        case Opcode::INTO:
            return exec_int(ctx, instr);
        
        // HLT
        case Opcode::HLT:
            return exec_misc(ctx, instr);

        // SYSCALL
        case Opcode::SYSCALL:
        case Opcode::SYSENTER:
            return exec_misc(ctx, instr);
        
        // RDTSC / RDTSCP
        case Opcode::RDTSC:
        case Opcode::RDTSCP:
            return exec_misc(ctx, instr);
        
        // UD2, and the ModRM-bearing UD1/UD0 that raise the same #UD
        case Opcode::UD2:
        case Opcode::UD1_R_RM:
        case Opcode::UD0_R_RM:
        case Opcode::EVEX_UNSUPPORTED:
            return exec_misc(ctx, instr);

        // SALC (D6) and ARPL (63 /r), both non-64-bit-mode only
        case Opcode::SALC:
        case Opcode::ARPL_RM16_R16:
            return exec_misc(ctx, instr);
        
        // CPUID
        case Opcode::CPUID:
            return exec_misc(ctx, instr);
        
        // Privileged I/O and system instructions → #GP (STATUS_PRIVILEGED_INSTRUCTION)
        case Opcode::INSB: case Opcode::INSW_D:
        case Opcode::OUTSB: case Opcode::OUTSW_D:
        case Opcode::IN_AL_IMM8: case Opcode::IN_EAX_IMM8:
        case Opcode::OUT_IMM8_AL: case Opcode::OUT_IMM8_EAX:
        case Opcode::IN_AL_DX: case Opcode::IN_EAX_DX:
        case Opcode::OUT_DX_AL: case Opcode::OUT_DX_EAX:
        case Opcode::CLI: case Opcode::STI:
        case Opcode::RDMSR: case Opcode::WRMSR: case Opcode::RDPMC:
        case Opcode::WBINVD:
        case Opcode::SYSEXIT:
            ctx.exception = CpuException::PrivilegedInstruction;
            return ExecResult::Exception;
        
        // XCHG (register-encoded form)
        case Opcode::XCHG_AX_R16:
        case Opcode::XCHG_EAX_R32:
        case Opcode::XCHG_RAX_R64:
            return exec_xchg(ctx, instr);
        
        // XCHG r/m, r
        case Opcode::XCHG_RM8_R8:
        case Opcode::XCHG_RM16_R16:
        case Opcode::XCHG_RM32_R32:
        case Opcode::XCHG_RM64_R64:
            return exec_xchg_rm(ctx, instr);
        
        // CMPXCHG
        case Opcode::CMPXCHG_RM8_R8:
        case Opcode::CMPXCHG_RM16_R16:
        case Opcode::CMPXCHG_RM32_R32:
        case Opcode::CMPXCHG_RM64_R64:
            return exec_cmpxchg(ctx, instr);
        
        // CMPXCHG8B
        case Opcode::CMPXCHG8B_M64:
            return exec_cmpxchg8b(ctx, instr);

        // CMPXCHG16B
        case Opcode::CMPXCHG16B_M128:
            return exec_cmpxchg16b(ctx, instr);
        
        // XADD
        case Opcode::XADD_RM8_R8:
        case Opcode::XADD_RM16_R16:
        case Opcode::XADD_RM32_R32:
        case Opcode::XADD_RM64_R64:
            return exec_xadd(ctx, instr);
        
        // Flag manipulation
        case Opcode::CLC:
        case Opcode::STC:
        case Opcode::CMC:
        case Opcode::CLD:
        case Opcode::STD:
        case Opcode::LAHF:
        case Opcode::SAHF:
            return exec_flag_ops(ctx, instr);
        
        // SHL instructions
        case Opcode::SHL_RM8_1:
        case Opcode::SHL_RM16_1:
        case Opcode::SHL_RM32_1:
        case Opcode::SHL_RM64_1:
        case Opcode::SHL_RM8_CL:
        case Opcode::SHL_RM16_CL:
        case Opcode::SHL_RM32_CL:
        case Opcode::SHL_RM64_CL:
        case Opcode::SHL_RM8_IMM8:
        case Opcode::SHL_RM16_IMM8:
        case Opcode::SHL_RM32_IMM8:
        case Opcode::SHL_RM64_IMM8:
            return exec_shl(ctx, instr);
        
        // SHR instructions
        case Opcode::SHR_RM8_1:
        case Opcode::SHR_RM16_1:
        case Opcode::SHR_RM32_1:
        case Opcode::SHR_RM64_1:
        case Opcode::SHR_RM8_CL:
        case Opcode::SHR_RM16_CL:
        case Opcode::SHR_RM32_CL:
        case Opcode::SHR_RM64_CL:
        case Opcode::SHR_RM8_IMM8:
        case Opcode::SHR_RM16_IMM8:
        case Opcode::SHR_RM32_IMM8:
        case Opcode::SHR_RM64_IMM8:
            return exec_shr(ctx, instr);
        
        // SAR instructions
        case Opcode::SAR_RM8_1:
        case Opcode::SAR_RM16_1:
        case Opcode::SAR_RM32_1:
        case Opcode::SAR_RM64_1:
        case Opcode::SAR_RM8_CL:
        case Opcode::SAR_RM16_CL:
        case Opcode::SAR_RM32_CL:
        case Opcode::SAR_RM64_CL:
        case Opcode::SAR_RM8_IMM8:
        case Opcode::SAR_RM16_IMM8:
        case Opcode::SAR_RM32_IMM8:
        case Opcode::SAR_RM64_IMM8:
            return exec_sar(ctx, instr);
        
        // SHLD instructions (double-precision shift left)
        case Opcode::SHLD_RM16_R16_IMM8:
        case Opcode::SHLD_RM32_R32_IMM8:
        case Opcode::SHLD_RM64_R64_IMM8:
        case Opcode::SHLD_RM16_R16_CL:
        case Opcode::SHLD_RM32_R32_CL:
        case Opcode::SHLD_RM64_R64_CL:
            return exec_shld(ctx, instr);
        
        // SHRD instructions (double-precision shift right)
        case Opcode::SHRD_RM16_R16_IMM8:
        case Opcode::SHRD_RM32_R32_IMM8:
        case Opcode::SHRD_RM64_R64_IMM8:
        case Opcode::SHRD_RM16_R16_CL:
        case Opcode::SHRD_RM32_R32_CL:
        case Opcode::SHRD_RM64_R64_CL:
            return exec_shrd(ctx, instr);
        
        // ROL instructions
        case Opcode::ROL_RM8_1:
        case Opcode::ROL_RM16_1:
        case Opcode::ROL_RM32_1:
        case Opcode::ROL_RM64_1:
        case Opcode::ROL_RM8_CL:
        case Opcode::ROL_RM16_CL:
        case Opcode::ROL_RM32_CL:
        case Opcode::ROL_RM64_CL:
        case Opcode::ROL_RM8_IMM8:
        case Opcode::ROL_RM16_IMM8:
        case Opcode::ROL_RM32_IMM8:
        case Opcode::ROL_RM64_IMM8:
            return exec_rol(ctx, instr);
        
        // ROR instructions
        case Opcode::ROR_RM8_1:
        case Opcode::ROR_RM16_1:
        case Opcode::ROR_RM32_1:
        case Opcode::ROR_RM64_1:
        case Opcode::ROR_RM8_CL:
        case Opcode::ROR_RM16_CL:
        case Opcode::ROR_RM32_CL:
        case Opcode::ROR_RM64_CL:
        case Opcode::ROR_RM8_IMM8:
        case Opcode::ROR_RM16_IMM8:
        case Opcode::ROR_RM32_IMM8:
        case Opcode::ROR_RM64_IMM8:
            return exec_ror(ctx, instr);
        
        // RCL instructions
        case Opcode::RCL_RM8_1:
        case Opcode::RCL_RM16_1:
        case Opcode::RCL_RM32_1:
        case Opcode::RCL_RM64_1:
        case Opcode::RCL_RM8_CL:
        case Opcode::RCL_RM16_CL:
        case Opcode::RCL_RM32_CL:
        case Opcode::RCL_RM64_CL:
        case Opcode::RCL_RM8_IMM8:
        case Opcode::RCL_RM16_IMM8:
        case Opcode::RCL_RM32_IMM8:
        case Opcode::RCL_RM64_IMM8:
            return exec_rcl(ctx, instr);
        
        // RCR instructions
        case Opcode::RCR_RM8_1:
        case Opcode::RCR_RM16_1:
        case Opcode::RCR_RM32_1:
        case Opcode::RCR_RM64_1:
        case Opcode::RCR_RM8_CL:
        case Opcode::RCR_RM16_CL:
        case Opcode::RCR_RM32_CL:
        case Opcode::RCR_RM64_CL:
        case Opcode::RCR_RM8_IMM8:
        case Opcode::RCR_RM16_IMM8:
        case Opcode::RCR_RM32_IMM8:
        case Opcode::RCR_RM64_IMM8:
            return exec_rcr(ctx, instr);
        
        // String instructions - MOVS
        case Opcode::MOVSB:
        case Opcode::MOVSW:
        case Opcode::MOVSD_STR:
        case Opcode::MOVSQ:
            return exec_movs(ctx, instr);
        
        // String instructions - STOS
        case Opcode::STOSB:
        case Opcode::STOSW:
        case Opcode::STOSD:
        case Opcode::STOSQ:
            return exec_stos(ctx, instr);
        
        // String instructions - LODS
        case Opcode::LODSB:
        case Opcode::LODSW:
        case Opcode::LODSD:
        case Opcode::LODSQ:
            return exec_lods(ctx, instr);
        
        // String instructions - CMPS
        case Opcode::CMPSB:
        case Opcode::CMPSW:
        case Opcode::CMPSD_STR:
        case Opcode::CMPSQ:
            return exec_cmps(ctx, instr);
        
        // String instructions - SCAS
        case Opcode::SCASB:
        case Opcode::SCASW:
        case Opcode::SCASD:
        case Opcode::SCASQ:
            return exec_scas(ctx, instr);
        
        // CMOVcc instructions
        case Opcode::CMOVO_R16_RM16:
        case Opcode::CMOVO_R32_RM32:
        case Opcode::CMOVO_R64_RM64:
        case Opcode::CMOVNO_R16_RM16:
        case Opcode::CMOVNO_R32_RM32:
        case Opcode::CMOVNO_R64_RM64:
        case Opcode::CMOVB_R16_RM16:
        case Opcode::CMOVB_R32_RM32:
        case Opcode::CMOVB_R64_RM64:
        case Opcode::CMOVNB_R16_RM16:
        case Opcode::CMOVNB_R32_RM32:
        case Opcode::CMOVNB_R64_RM64:
        case Opcode::CMOVZ_R16_RM16:
        case Opcode::CMOVZ_R32_RM32:
        case Opcode::CMOVZ_R64_RM64:
        case Opcode::CMOVNZ_R16_RM16:
        case Opcode::CMOVNZ_R32_RM32:
        case Opcode::CMOVNZ_R64_RM64:
        case Opcode::CMOVBE_R16_RM16:
        case Opcode::CMOVBE_R32_RM32:
        case Opcode::CMOVBE_R64_RM64:
        case Opcode::CMOVNBE_R16_RM16:
        case Opcode::CMOVNBE_R32_RM32:
        case Opcode::CMOVNBE_R64_RM64:
        case Opcode::CMOVS_R16_RM16:
        case Opcode::CMOVS_R32_RM32:
        case Opcode::CMOVS_R64_RM64:
        case Opcode::CMOVNS_R16_RM16:
        case Opcode::CMOVNS_R32_RM32:
        case Opcode::CMOVNS_R64_RM64:
        case Opcode::CMOVP_R16_RM16:
        case Opcode::CMOVP_R32_RM32:
        case Opcode::CMOVP_R64_RM64:
        case Opcode::CMOVNP_R16_RM16:
        case Opcode::CMOVNP_R32_RM32:
        case Opcode::CMOVNP_R64_RM64:
        case Opcode::CMOVL_R16_RM16:
        case Opcode::CMOVL_R32_RM32:
        case Opcode::CMOVL_R64_RM64:
        case Opcode::CMOVNL_R16_RM16:
        case Opcode::CMOVNL_R32_RM32:
        case Opcode::CMOVNL_R64_RM64:
        case Opcode::CMOVLE_R16_RM16:
        case Opcode::CMOVLE_R32_RM32:
        case Opcode::CMOVLE_R64_RM64:
        case Opcode::CMOVNLE_R16_RM16:
        case Opcode::CMOVNLE_R32_RM32:
        case Opcode::CMOVNLE_R64_RM64:
            return exec_cmov(ctx, instr);
        
        // SETcc instructions
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
            return exec_setcc(ctx, instr);
        
        // BSF instructions
        case Opcode::BSF_R16_RM16:
        case Opcode::BSF_R32_RM32:
        case Opcode::BSF_R64_RM64:
            return exec_bsf(ctx, instr);
        
        // BSR instructions
        case Opcode::BSR_R16_RM16:
        case Opcode::BSR_R32_RM32:
        case Opcode::BSR_R64_RM64:
            return exec_bsr(ctx, instr);

        // POPCNT instructions
        case Opcode::POPCNT_R16_RM16:
        case Opcode::POPCNT_R32_RM32:
        case Opcode::POPCNT_R64_RM64:
            return exec_popcnt(ctx, instr);

        // TZCNT / LZCNT (BMI1 / ABM)
        case Opcode::TZCNT_R32_RM32:
        case Opcode::LZCNT_R32_RM32:
        case Opcode::TZCNT_R16_RM16:
        case Opcode::LZCNT_R16_RM16:
        case Opcode::TZCNT_R64_RM64:
        case Opcode::LZCNT_R64_RM64:
            return exec_bmi_ops(ctx, instr);
        
        // BT/BTS/BTR/BTC instructions
        case Opcode::BT_RM16_R16:
        case Opcode::BT_RM32_R32:
        case Opcode::BT_RM64_R64:
        case Opcode::BT_RM16_IMM8:
        case Opcode::BT_RM32_IMM8:
        case Opcode::BT_RM64_IMM8:
        case Opcode::BTS_RM16_R16:
        case Opcode::BTS_RM32_R32:
        case Opcode::BTS_RM64_R64:
        case Opcode::BTS_RM16_IMM8:
        case Opcode::BTS_RM32_IMM8:
        case Opcode::BTS_RM64_IMM8:
        case Opcode::BTR_RM16_R16:
        case Opcode::BTR_RM32_R32:
        case Opcode::BTR_RM64_R64:
        case Opcode::BTR_RM16_IMM8:
        case Opcode::BTR_RM32_IMM8:
        case Opcode::BTR_RM64_IMM8:
        case Opcode::BTC_RM16_R16:
        case Opcode::BTC_RM32_R32:
        case Opcode::BTC_RM64_R64:
        case Opcode::BTC_RM16_IMM8:
        case Opcode::BTC_RM32_IMM8:
        case Opcode::BTC_RM64_IMM8:
            return exec_bt(ctx, instr);
        
        // BSWAP instructions
        case Opcode::BSWAP_R32:
        case Opcode::BSWAP_R64:
            return exec_bswap(ctx, instr);
        
        // MUL instructions
        case Opcode::MUL_RM8:
        case Opcode::MUL_RM16:
        case Opcode::MUL_RM32:
        case Opcode::MUL_RM64:
            return exec_mul(ctx, instr);
        
        // IMUL instructions (one-operand, two-operand, and three-operand)
        case Opcode::IMUL_RM8:
        case Opcode::IMUL_RM16:
        case Opcode::IMUL_RM32:
        case Opcode::IMUL_RM64:
        case Opcode::IMUL_R16_RM16:
        case Opcode::IMUL_R32_RM32:
        case Opcode::IMUL_R64_RM64:
        case Opcode::IMUL_R16_RM16_IMM8:
        case Opcode::IMUL_R32_RM32_IMM8:
        case Opcode::IMUL_R64_RM64_IMM8:
        case Opcode::IMUL_R16_RM16_IMM16:
        case Opcode::IMUL_R32_RM32_IMM32:
        case Opcode::IMUL_R64_RM64_IMM32:
            return exec_imul(ctx, instr);
        
        // DIV instructions
        case Opcode::DIV_RM8:
        case Opcode::DIV_RM16:
        case Opcode::DIV_RM32:
        case Opcode::DIV_RM64:
            return exec_div(ctx, instr);
        
        // IDIV instructions
        case Opcode::IDIV_RM8:
        case Opcode::IDIV_RM16:
        case Opcode::IDIV_RM32:
        case Opcode::IDIV_RM64:
            return exec_idiv(ctx, instr);
        
        // CBW/CWDE/CDQE/CWD/CDQ/CQO
        case Opcode::CBW:
        case Opcode::CWDE:
        case Opcode::CDQE:
        case Opcode::CWD:
        case Opcode::CDQ:
        case Opcode::CQO:
            return exec_cdq_cqo(ctx, instr);
        
        // INC instructions
        case Opcode::INC_RM8:
        case Opcode::INC_RM16:
        case Opcode::INC_RM32:
        case Opcode::INC_RM64:
            return exec_inc(ctx, instr);
        
        // DEC instructions
        case Opcode::DEC_RM8:
        case Opcode::DEC_RM16:
        case Opcode::DEC_RM32:
        case Opcode::DEC_RM64:
            return exec_dec(ctx, instr);
        
        // NEG instructions
        case Opcode::NEG_RM8:
        case Opcode::NEG_RM16:
        case Opcode::NEG_RM32:
        case Opcode::NEG_RM64:
            return exec_neg(ctx, instr);
        
        // ADC instructions
        case Opcode::ADC_AL_IMM8:
        case Opcode::ADC_AX_IMM16:
        case Opcode::ADC_EAX_IMM32:
        case Opcode::ADC_RAX_IMM32:
        case Opcode::ADC_RM8_IMM8:
        case Opcode::ADC_RM16_IMM16:
        case Opcode::ADC_RM32_IMM32:
        case Opcode::ADC_RM64_IMM32:
        case Opcode::ADC_RM16_IMM8:
        case Opcode::ADC_RM32_IMM8:
        case Opcode::ADC_RM64_IMM8:
        case Opcode::ADC_RM8_R8:
        case Opcode::ADC_RM16_R16:
        case Opcode::ADC_RM32_R32:
        case Opcode::ADC_RM64_R64:
        case Opcode::ADC_R8_RM8:
        case Opcode::ADC_R16_RM16:
        case Opcode::ADC_R32_RM32:
        case Opcode::ADC_R64_RM64:
            return exec_adc(ctx, instr);
        
        // SBB instructions
        case Opcode::SBB_AL_IMM8:
        case Opcode::SBB_AX_IMM16:
        case Opcode::SBB_EAX_IMM32:
        case Opcode::SBB_RAX_IMM32:
        case Opcode::SBB_RM8_IMM8:
        case Opcode::SBB_RM16_IMM16:
        case Opcode::SBB_RM32_IMM32:
        case Opcode::SBB_RM64_IMM32:
        case Opcode::SBB_RM16_IMM8:
        case Opcode::SBB_RM32_IMM8:
        case Opcode::SBB_RM64_IMM8:
        case Opcode::SBB_RM8_R8:
        case Opcode::SBB_RM16_R16:
        case Opcode::SBB_RM32_R32:
        case Opcode::SBB_RM64_R64:
        case Opcode::SBB_R8_RM8:
        case Opcode::SBB_R16_RM16:
        case Opcode::SBB_R32_RM32:
        case Opcode::SBB_R64_RM64:
            return exec_sbb(ctx, instr);
        
        // x87 FPU instructions
        case Opcode::FNCLEX:
        case Opcode::FNINIT:
        case Opcode::FNOP:
        case Opcode::FWAIT:
        case Opcode::FDECSTP:
        case Opcode::FINCSTP:
        case Opcode::FLD_M32:
        case Opcode::FLD_M64:
        case Opcode::FLD_M80:
        case Opcode::FLD_STI:
        case Opcode::FSTP_M32:
        case Opcode::FSTP_M64:
        case Opcode::FSTP_M80:
        case Opcode::FSTP_STI:
        case Opcode::FST_STI:
        case Opcode::FST_M32:
        case Opcode::FST_M64:
        case Opcode::FILD_M32:
        case Opcode::FILD_M64:
        case Opcode::FISTP_M32:
        case Opcode::FISTP_M64:
        case Opcode::FISTTP_M16:
        case Opcode::FISTTP_M32:
        case Opcode::FISTTP_M64:
        case Opcode::FLD1:
        case Opcode::FLDZ:
        case Opcode::FADD_STI:
        case Opcode::FADDP_STI:
        case Opcode::FSUB_STI:
        case Opcode::FSUBP_STI:
        case Opcode::FMUL_STI:
        case Opcode::FMULP_STI:
        case Opcode::FDIV_STI:
        case Opcode::FDIVP_STI:
        case Opcode::FDIVRP_STI:
        case Opcode::FSUBR_STI:
        case Opcode::FSUBRP_STI:
        case Opcode::FDIVR_STI:
        case Opcode::FADD_TO_STI:
        case Opcode::FMUL_TO_STI:
        case Opcode::FSUB_TO_STI:
        case Opcode::FSUBR_TO_STI:
        case Opcode::FDIV_TO_STI:
        case Opcode::FDIVR_TO_STI:
        case Opcode::FADD_M32:
        case Opcode::FADD_M64:
        case Opcode::FMUL_M32:
        case Opcode::FMUL_M64:
        case Opcode::FSUB_M32:
        case Opcode::FSUB_M64:
        case Opcode::FDIV_M32:
        case Opcode::FDIV_M64:
        case Opcode::FIADD_M16:
        case Opcode::FIADD_M32:
        case Opcode::FIMUL_M16:
        case Opcode::FIMUL_M32:
        case Opcode::FICOM_M16:
        case Opcode::FICOM_M32:
        case Opcode::FICOMP_M16:
        case Opcode::FICOMP_M32:
        case Opcode::FISUB_M16:
        case Opcode::FISUB_M32:
        case Opcode::FISUBR_M16:
        case Opcode::FISUBR_M32:
        case Opcode::FIDIV_M16:
        case Opcode::FIDIV_M32:
        case Opcode::FIDIVR_M16:
        case Opcode::FIDIVR_M32:
        case Opcode::FCOM_STI:
        case Opcode::FCOM_M32:
        case Opcode::FCOM_M64:
        case Opcode::FCOMP_STI:
        case Opcode::FCOMP_M32:
        case Opcode::FCOMP_M64:
        case Opcode::FCOMPP:
        case Opcode::FUCOMPP:
        case Opcode::FUCOM_STI:
        case Opcode::FUCOMP_STI:
        case Opcode::FCOMI_STI:
        case Opcode::FCOMIP_STI:
        case Opcode::FUCOMI_STI:
        case Opcode::FUCOMIP_STI:
        case Opcode::FCMOVB_STI:
        case Opcode::FCMOVE_STI:
        case Opcode::FCMOVBE_STI:
        case Opcode::FCMOVU_STI:
        case Opcode::FCMOVNB_STI:
        case Opcode::FCMOVNE_STI:
        case Opcode::FCMOVNBE_STI:
        case Opcode::FCMOVNU_STI:
        case Opcode::FTST:
        case Opcode::FNSTSW_AX:
        case Opcode::FNSTSW_M16:
        case Opcode::FNSTCW_M16:
        case Opcode::FLDCW_M16:
        case Opcode::FNSTENV_M28:
        case Opcode::FLDENV_M28:
        case Opcode::FNSAVE_M108:
        case Opcode::FRSTOR_M108:
        case Opcode::FILD_M16:
        case Opcode::FIST_M16:
        case Opcode::FISTP_M16:
        case Opcode::FIST_M32:
        case Opcode::FCHS:
        case Opcode::FABS:
        case Opcode::FXCH_STI:
        case Opcode::FRNDINT:
        case Opcode::FSQRT:
        case Opcode::F2XM1:
        case Opcode::FYL2X:
        case Opcode::FYL2XP1:
        case Opcode::FPTAN:
        case Opcode::FPATAN:
        case Opcode::FXTRACT:
        case Opcode::FPREM:
        case Opcode::FPREM1:
        case Opcode::FSCALE:
        case Opcode::FSIN:
        case Opcode::FCOS:
        case Opcode::FSINCOS:
        case Opcode::FLDL2T:
        case Opcode::FLDL2E:
        case Opcode::FLDPI:
        case Opcode::FLDLG2:
        case Opcode::FLDLN2:
        case Opcode::FXAM:
        case Opcode::FSUBR_M32:
        case Opcode::FSUBR_M64:
        case Opcode::FDIVR_M32:
        case Opcode::FDIVR_M64:
        case Opcode::FFREE_STI:
        case Opcode::FFREEP_STI:
        case Opcode::FBLD_M80:
        case Opcode::FBSTP_M80:
            return exec_fpu(ctx, instr);
        
        // SSE arithmetic instructions
        case Opcode::ADDSS_XMM_XMMM32:
        case Opcode::ADDSD_XMM_XMMM64:
        case Opcode::ADDPS_XMM_XMMM128:
        case Opcode::ADDPD_XMM_XMMM128:
        case Opcode::SUBSS_XMM_XMMM32:
        case Opcode::SUBSD_XMM_XMMM64:
        case Opcode::SUBPS_XMM_XMMM128:
        case Opcode::SUBPD_XMM_XMMM128:
        case Opcode::MULSS_XMM_XMMM32:
        case Opcode::MULSD_XMM_XMMM64:
        case Opcode::MULPS_XMM_XMMM128:
        case Opcode::MULPD_XMM_XMMM128:
        case Opcode::DIVSS_XMM_XMMM32:
        case Opcode::DIVSD_XMM_XMMM64:
        case Opcode::DIVPS_XMM_XMMM128:
        case Opcode::DIVPD_XMM_XMMM128:
        case Opcode::SQRTSS_XMM_XMMM32:
        case Opcode::SQRTSD_XMM_XMMM64:
        case Opcode::SQRTPS_XMM_XMMM128:
        case Opcode::SQRTPD_XMM_XMMM128:
        case Opcode::MAXSS_XMM_XMMM32:
        case Opcode::MAXSD_XMM_XMMM64:
        case Opcode::MAXPS_XMM_XMMM128:
        case Opcode::MAXPD_XMM_XMMM128:
        case Opcode::MINSS_XMM_XMMM32:
        case Opcode::MINSD_XMM_XMMM64:
        case Opcode::MINPS_XMM_XMMM128:
        case Opcode::MINPD_XMM_XMMM128:
        case Opcode::COMISS_XMM_XMMM32:
        case Opcode::COMISD_XMM_XMMM64:
        case Opcode::UCOMISS_XMM_XMMM32:
        case Opcode::UCOMISD_XMM_XMMM64:
        case Opcode::RCPSS_XMM_XMMM32:
        case Opcode::RCPPS_XMM_XMMM128:
        case Opcode::RSQRTSS_XMM_XMMM32:
        case Opcode::RSQRTPS_XMM_XMMM128:
        // SSE3 arithmetic
        case Opcode::ADDSUBPD_XMM_XMMM128:
        case Opcode::ADDSUBPS_XMM_XMMM128:
        case Opcode::HADDPD_XMM_XMMM128:
        case Opcode::HADDPS_XMM_XMMM128:
        case Opcode::HSUBPD_XMM_XMMM128:
        case Opcode::HSUBPS_XMM_XMMM128:
            return exec_sse_arith(ctx, instr);
        
        // SSE Data Transfer (CPU-031)
        case Opcode::MOVAPS_XMM_XMMM128: case Opcode::MOVAPS_XMMM128_XMM:
        case Opcode::MOVUPS_XMM_XMMM128: case Opcode::MOVUPS_XMMM128_XMM:
        case Opcode::MOVAPD_XMM_XMMM128: case Opcode::MOVAPD_XMMM128_XMM:
        case Opcode::MOVUPD_XMM_XMMM128: case Opcode::MOVUPD_XMMM128_XMM:
        case Opcode::MOVNTPS_M128_XMM:   case Opcode::MOVNTPD_M128_XMM:
        case Opcode::MOVNTDQ_M128_XMM:
        case Opcode::MOVDQA_XMM_XMMM128: case Opcode::MOVDQA_XMMM128_XMM:
        case Opcode::MOVDQU_XMM_XMMM128: case Opcode::MOVDQU_XMMM128_XMM:
        case Opcode::LDDQU_XMM_M128:
        case Opcode::MOVSS_XMM_XMMM32:   case Opcode::MOVSS_XMMM32_XMM:
        case Opcode::MOVSD_XMM_XMMM64:   case Opcode::MOVSD_XMMM64_XMM:
        case Opcode::MOVLPS_XMM_M64:     case Opcode::MOVLPS_M64_XMM:
        case Opcode::MOVHPS_XMM_M64:     case Opcode::MOVHPS_M64_XMM:
        case Opcode::MOVLPD_XMM_M64:     case Opcode::MOVLPD_M64_XMM:
        case Opcode::MOVHPD_XMM_M64:     case Opcode::MOVHPD_M64_XMM:
        case Opcode::MOVLHPS_XMM_XMM:    case Opcode::MOVHLPS_XMM_XMM:
        case Opcode::MOVMSKPS_R32_XMM:   case Opcode::MOVMSKPD_R32_XMM:
        case Opcode::XORPS_XMM_XMMM128:  case Opcode::XORPD_XMM_XMMM128:
        case Opcode::ANDPS_XMM_XMMM128:  case Opcode::ANDPD_XMM_XMMM128:
        case Opcode::ORPS_XMM_XMMM128:   case Opcode::ORPD_XMM_XMMM128:
        case Opcode::ANDNPS_XMM_XMMM128: case Opcode::ANDNPD_XMM_XMMM128:
        case Opcode::FXSAVE_M512:
        case Opcode::FXRSTOR_M512:
        case Opcode::CLFLUSH_M8:
        case Opcode::MOVNTI_M32_R32:
        case Opcode::MOVNTI_M64_R64:
        case Opcode::LDMXCSR_M32:        case Opcode::STMXCSR_M32:
        // SSE3 move-duplicate
        case Opcode::MOVSLDUP_XMM_XMMM128:
        case Opcode::MOVSHDUP_XMM_XMMM128:
        case Opcode::MOVDDUP_XMM_XMMM64:
            return exec_sse_data(ctx, instr);
        
        case Opcode::EMMS:
            ctx.cpu.fpu.emms();
            return ExecResult::Continue;
        
        // MMX Instructions (CPU-045)
        case Opcode::MOVQ_MM_MMM64:
        case Opcode::MOVQ_MMM64_MM:
        case Opcode::MOVD_MM_RM32:
        case Opcode::MOVD_RM32_MM:
        case Opcode::PADDW_MM_MMM64:
        case Opcode::PSUBW_MM_MMM64:
        case Opcode::PADDD_MM_MMM64:
        case Opcode::PACKUSWB_MM_MMM64:
        case Opcode::PACKSSDW_MM_MMM64:
        case Opcode::PMINSW_MM_MMM64:
        case Opcode::PMAXSW_MM_MMM64:
        case Opcode::PSHUFW_MM_MMM64_IMM8:
        case Opcode::PSRLD_MM_IMM8:
        case Opcode::PSRAD_MM_IMM8:
        case Opcode::PSLLD_MM_IMM8:
        case Opcode::PSRLW_MM_IMM8:
        case Opcode::PSRAW_MM_IMM8:
        case Opcode::PSLLW_MM_IMM8:
        case Opcode::PADDB_MM_MMM64:
        case Opcode::PSUBB_MM_MMM64:
        case Opcode::PSUBD_MM_MMM64:
        case Opcode::PSUBQ_MM_MMM64:
        case Opcode::PADDUSB_MM_MMM64:
        case Opcode::PADDUSW_MM_MMM64:
        case Opcode::PSUBUSB_MM_MMM64:
        case Opcode::PSUBUSW_MM_MMM64:
        case Opcode::PADDSB_MM_MMM64:
        case Opcode::PADDSW_MM_MMM64:
        case Opcode::PSUBSB_MM_MMM64:
        case Opcode::PSUBSW_MM_MMM64:
        case Opcode::PACKSSWB_MM_MMM64:
        case Opcode::PMULLW_MM_MMM64:
        case Opcode::PMULHW_MM_MMM64:
        case Opcode::PMULHUW_MM_MMM64:
        case Opcode::PMADDWD_MM_MMM64:
        case Opcode::PMINUB_MM_MMM64:
        case Opcode::PMAXUB_MM_MMM64:
        case Opcode::PAVGB_MM_MMM64:
        case Opcode::PAVGW_MM_MMM64:
        case Opcode::PSADBW_MM_MMM64:
        case Opcode::PMOVMSKB_R32_MM:
        case Opcode::PINSRW_MM_R32M16_IMM8:
        case Opcode::PEXTRW_R32_MM_IMM8:
        case Opcode::PSRLW_MM_MMM64:
        case Opcode::PSRAW_MM_MMM64:
        case Opcode::PSLLW_MM_MMM64:
        case Opcode::PSRLD_MM_MMM64:
        case Opcode::PSRAD_MM_MMM64:
        case Opcode::PSLLD_MM_MMM64:
        case Opcode::MASKMOVQ_MM_MM:
        case Opcode::MOVNTQ_M64_MM:
        case Opcode::MOVQ2DQ_XMM_MM:
        case Opcode::MOVDQ2Q_MM_XMM:
        case Opcode::PSHUFB_MM_MMM64:
        case Opcode::PHADDW_MM_MMM64:
        case Opcode::PHADDD_MM_MMM64:
        case Opcode::PHADDSW_MM_MMM64:
        case Opcode::PMADDUBSW_MM_MMM64:
        case Opcode::PHSUBW_MM_MMM64:
        case Opcode::PHSUBD_MM_MMM64:
        case Opcode::PHSUBSW_MM_MMM64:
        case Opcode::PSIGNB_MM_MMM64:
        case Opcode::PSIGNW_MM_MMM64:
        case Opcode::PSIGND_MM_MMM64:
        case Opcode::PMULHRSW_MM_MMM64:
        case Opcode::PABSB_MM_MMM64:
        case Opcode::PABSW_MM_MMM64:
        case Opcode::PABSD_MM_MMM64:
        case Opcode::PALIGNR_MM_MMM64_IMM8:
        case Opcode::PADDQ_MM_MMM64:
        case Opcode::PMULUDQ_MM_MMM64:
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
        case Opcode::PSLLQ_MM_IMM8:
        case Opcode::PSRLQ_MM_IMM8:
        case Opcode::PSLLQ_MM_MMM64:
        case Opcode::PSRLQ_MM_MMM64:
        case Opcode::PUNPCKLBW_MM_MMM64:
        case Opcode::PUNPCKLWD_MM_MMM64:
        case Opcode::PUNPCKLDQ_MM_MMM64:
        case Opcode::PUNPCKHBW_MM_MMM64:
        case Opcode::PUNPCKHWD_MM_MMM64:
        case Opcode::PUNPCKHDQ_MM_MMM64:
        case Opcode::PUNPCKHQDQ_MM_MMM64:
            return exec_mmx(ctx, instr);
        
        // SSE Conversion (CPU-033)
        case Opcode::CVTSI2SS_XMM_RM32: case Opcode::CVTSI2SS_XMM_RM64:
        case Opcode::CVTSI2SD_XMM_RM32: case Opcode::CVTSI2SD_XMM_RM64:
        case Opcode::CVTSS2SI_R32_XMMM32: case Opcode::CVTSS2SI_R64_XMMM32:
        case Opcode::CVTSD2SI_R32_XMMM64: case Opcode::CVTSD2SI_R64_XMMM64:
        case Opcode::CVTTSS2SI_R32_XMMM32: case Opcode::CVTTSS2SI_R64_XMMM32:
        case Opcode::CVTTSD2SI_R32_XMMM64: case Opcode::CVTTSD2SI_R64_XMMM64:
        case Opcode::CVTSS2SD_XMM_XMMM32: case Opcode::CVTSD2SS_XMM_XMMM64:
        case Opcode::CVTPS2DQ_XMM_XMMM128: case Opcode::CVTDQ2PS_XMM_XMMM128:
        case Opcode::CVTPD2DQ_XMM_XMMM128: case Opcode::CVTDQ2PD_XMM_XMMM64:
        case Opcode::CVTPS2PD_XMM_XMMM64: case Opcode::CVTPD2PS_XMM_XMMM128:
        case Opcode::CVTTPD2DQ_XMM_XMMM128: case Opcode::CVTTPS2DQ_XMM_XMMM128:
        case Opcode::CVTPI2PS_XMM_MMM64: case Opcode::CVTPI2PD_XMM_MMM64:
        case Opcode::CVTPS2PI_MM_XMMM64: case Opcode::CVTPD2PI_MM_XMMM128:
        case Opcode::CVTTPS2PI_MM_XMMM64: case Opcode::CVTTPD2PI_MM_XMMM128:
            return exec_sse_conv(ctx, instr);
        
        // SSE2 Integer Arithmetic (CPU-037)
        // SSE Comparison (CPU-036)
        case Opcode::CMPPS_XMM_XMMM128_IMM8: case Opcode::CMPSS_XMM_XMMM32_IMM8:
        case Opcode::CMPPD_XMM_XMMM128_IMM8: case Opcode::CMPSD_XMM_XMMM64_IMM8:
        case Opcode::PCMPEQB_XMM_XMMM128: case Opcode::PCMPEQW_XMM_XMMM128:
        case Opcode::PCMPEQD_XMM_XMMM128:
        case Opcode::PCMPGTB_XMM_XMMM128: case Opcode::PCMPGTW_XMM_XMMM128:
        case Opcode::PCMPGTD_XMM_XMMM128:
            return exec_sse_cmp(ctx, instr);
        
        case Opcode::PADDB_XMM_XMMM128: case Opcode::PADDW_XMM_XMMM128:
        case Opcode::PADDD_XMM_XMMM128: case Opcode::PADDQ_XMM_XMMM128:
        case Opcode::PSUBB_XMM_XMMM128: case Opcode::PSUBW_XMM_XMMM128:
        case Opcode::PSUBD_XMM_XMMM128: case Opcode::PSUBQ_XMM_XMMM128:
        case Opcode::PADDUSB_XMM_XMMM128: case Opcode::PADDUSW_XMM_XMMM128:
        case Opcode::PSUBUSB_XMM_XMMM128: case Opcode::PSUBUSW_XMM_XMMM128:
        case Opcode::PADDSB_XMM_XMMM128: case Opcode::PADDSW_XMM_XMMM128:
        case Opcode::PSUBSB_XMM_XMMM128: case Opcode::PSUBSW_XMM_XMMM128:
        case Opcode::PACKSSWB_XMM_XMMM128: case Opcode::PACKSSDW_XMM_XMMM128:
        case Opcode::PACKUSWB_XMM_XMMM128:
        case Opcode::PMULLW_XMM_XMMM128: case Opcode::PMULHW_XMM_XMMM128:
        case Opcode::PMULHUW_XMM_XMMM128: case Opcode::PMADDWD_XMM_XMMM128:
        case Opcode::PMULUDQ_XMM_XMMM128:
        case Opcode::PMINUB_XMM_XMMM128: case Opcode::PMAXUB_XMM_XMMM128:
        case Opcode::PMINSW_XMM_XMMM128: case Opcode::PMAXSW_XMM_XMMM128:
        case Opcode::PAVGB_XMM_XMMM128: case Opcode::PAVGW_XMM_XMMM128:
        case Opcode::PSADBW_XMM_XMMM128:
        case Opcode::PAND_XMM_XMMM128: case Opcode::PANDN_XMM_XMMM128:
        case Opcode::POR_XMM_XMMM128: case Opcode::PXOR_XMM_XMMM128:
        case Opcode::MOVD_XMM_RM32: case Opcode::MOVD_RM32_XMM:
        case Opcode::MOVQ_XMM_RM64: case Opcode::MOVQ_RM64_XMM:
        case Opcode::MOVQ_XMM_XMMM64: case Opcode::MOVQ_XMMM64_XMM:
        case Opcode::PSLLD_XMM_IMM8: case Opcode::PSLLQ_XMM_IMM8:
        case Opcode::PSRLD_XMM_IMM8: case Opcode::PSRLQ_XMM_IMM8:
        case Opcode::PSRAD_XMM_IMM8:
        case Opcode::PSLLW_XMM_IMM8: case Opcode::PSRLW_XMM_IMM8:
        case Opcode::PSRAW_XMM_IMM8:
        case Opcode::PSLLDQ_XMM_IMM8: case Opcode::PSRLDQ_XMM_IMM8:
        case Opcode::PSRLW_XMM_XMMM128: case Opcode::PSRLD_XMM_XMMM128:
        case Opcode::PSRLQ_XMM_XMMM128: case Opcode::PSRAW_XMM_XMMM128:
        case Opcode::PSRAD_XMM_XMMM128: case Opcode::PSLLW_XMM_XMMM128:
        case Opcode::PSLLD_XMM_XMMM128: case Opcode::PSLLQ_XMM_XMMM128:
        case Opcode::PMOVMSKB_R32_XMM:
        case Opcode::PINSRW_XMM_R32M16_IMM8: case Opcode::PEXTRW_R32_XMM_IMM8:
        case Opcode::MASKMOVDQU_XMM_XMM:
        case Opcode::PEXTRD_RM32_XMM_IMM8: case Opcode::PINSRD_XMM_RM32_IMM8:
        case Opcode::PEXTRQ_RM64_XMM_IMM8: case Opcode::PINSRQ_XMM_RM64_IMM8:
            return exec_sse_int(ctx, instr);
        
        // SSE Shuffle & Unpack (CPU-038)
        case Opcode::SHUFPS_XMM_XMMM128_IMM8: case Opcode::SHUFPD_XMM_XMMM128_IMM8:
        case Opcode::PSHUFD_XMM_XMMM128_IMM8: case Opcode::PSHUFLW_XMM_XMMM128_IMM8:
        case Opcode::PSHUFHW_XMM_XMMM128_IMM8:
        case Opcode::UNPCKLPS_XMM_XMMM128: case Opcode::UNPCKHPS_XMM_XMMM128:
        case Opcode::UNPCKLPD_XMM_XMMM128: case Opcode::UNPCKHPD_XMM_XMMM128:
        case Opcode::PUNPCKLBW_XMM_XMMM128: case Opcode::PUNPCKLWD_XMM_XMMM128:
        case Opcode::PUNPCKLDQ_XMM_XMMM128: case Opcode::PUNPCKLQDQ_XMM_XMMM128:
        case Opcode::PUNPCKHBW_XMM_XMMM128: case Opcode::PUNPCKHWD_XMM_XMMM128:
        case Opcode::PUNPCKHDQ_XMM_XMMM128: case Opcode::PUNPCKHQDQ_XMM_XMMM128:
            return exec_sse_shuffle(ctx, instr);
        
        // SSSE3/SSE4.1/AES-NI/SHA/PCLMULQDQ (three-byte escapes)
        case Opcode::PSHUFB_XMM_XMMM128:
        case Opcode::PMULLD_XMM_XMMM128:
        case Opcode::PALIGNR_XMM_XMMM128_IMM8:
        case Opcode::AESENC_XMM_XMMM128: case Opcode::AESENCLAST_XMM_XMMM128:
        case Opcode::AESDEC_XMM_XMMM128: case Opcode::AESDECLAST_XMM_XMMM128:
        case Opcode::AESIMC_XMM_XMMM128: case Opcode::AESKEYGENASSIST_XMM_XMMM128_IMM8:
        case Opcode::PCLMULQDQ_XMM_XMMM128_IMM8:
        case Opcode::SHA1NEXTE_XMM_XMMM128: case Opcode::SHA1MSG1_XMM_XMMM128:
        case Opcode::SHA1MSG2_XMM_XMMM128: case Opcode::SHA1RNDS4_XMM_XMMM128_IMM8:
        case Opcode::SHA256RNDS2_XMM_XMMM128: case Opcode::SHA256MSG1_XMM_XMMM128:
        case Opcode::SHA256MSG2_XMM_XMMM128:
        // SSSE3 horizontal / sign / abs / multiply
        case Opcode::PHADDW_XMM_XMMM128: case Opcode::PHADDD_XMM_XMMM128:
        case Opcode::PHADDSW_XMM_XMMM128: case Opcode::PMADDUBSW_XMM_XMMM128:
        case Opcode::PHSUBW_XMM_XMMM128: case Opcode::PHSUBD_XMM_XMMM128:
        case Opcode::PHSUBSW_XMM_XMMM128:
        case Opcode::PSIGNB_XMM_XMMM128: case Opcode::PSIGNW_XMM_XMMM128:
        case Opcode::PSIGND_XMM_XMMM128: case Opcode::PMULHRSW_XMM_XMMM128:
        case Opcode::PABSB_XMM_XMMM128: case Opcode::PABSW_XMM_XMMM128:
        case Opcode::PABSD_XMM_XMMM128:
        case Opcode::PMULDQ_XMM_XMMM128:
            return exec_crypto_ops(ctx, instr);
        
        // SSE4.1/SSE4.2 instructions (CPU-041)
        case Opcode::PTEST_XMM_XMMM128:
        case Opcode::PCMPEQQ_XMM_XMMM128: case Opcode::PCMPGTQ_XMM_XMMM128:
        case Opcode::PACKUSDW_XMM_XMMM128: case Opcode::MOVNTDQA_XMM_M128:
        case Opcode::PMOVSXBW_XMM_XMMM64: case Opcode::PMOVSXBD_XMM_XMMM32:
        case Opcode::PMOVSXBQ_XMM_XMMM16: case Opcode::PMOVSXWD_XMM_XMMM64:
        case Opcode::PMOVSXWQ_XMM_XMMM32: case Opcode::PMOVSXDQ_XMM_XMMM64:
        case Opcode::PMOVZXBW_XMM_XMMM64: case Opcode::PMOVZXBD_XMM_XMMM32:
        case Opcode::PMOVZXBQ_XMM_XMMM16: case Opcode::PMOVZXWD_XMM_XMMM64:
        case Opcode::PMOVZXWQ_XMM_XMMM32: case Opcode::PMOVZXDQ_XMM_XMMM64:
        case Opcode::PMINSB_XMM_XMMM128: case Opcode::PMINSD_XMM_XMMM128:
        case Opcode::PMINUW_XMM_XMMM128: case Opcode::PMINUD_XMM_XMMM128:
        case Opcode::PMAXSB_XMM_XMMM128: case Opcode::PMAXSD_XMM_XMMM128:
        case Opcode::PMAXUW_XMM_XMMM128: case Opcode::PMAXUD_XMM_XMMM128:
        case Opcode::PHMINPOSUW_XMM_XMMM128:
        case Opcode::PBLENDVB_XMM_XMMM128: case Opcode::BLENDVPS_XMM_XMMM128:
        case Opcode::BLENDVPD_XMM_XMMM128:
        case Opcode::BLENDPS_XMM_XMMM128_IMM8: case Opcode::BLENDPD_XMM_XMMM128_IMM8:
        case Opcode::PBLENDW_XMM_XMMM128_IMM8:
        case Opcode::ROUNDPS_XMM_XMMM128_IMM8: case Opcode::ROUNDPD_XMM_XMMM128_IMM8:
        case Opcode::ROUNDSS_XMM_XMMM32_IMM8: case Opcode::ROUNDSD_XMM_XMMM64_IMM8:
        case Opcode::PINSRB_XMM_RM8_IMM8: case Opcode::PEXTRB_RM8_XMM_IMM8:
        case Opcode::EXTRACTPS_RM32_XMM_IMM8: case Opcode::INSERTPS_XMM_XMMM32_IMM8:
        case Opcode::DPPS_XMM_XMMM128_IMM8: case Opcode::DPPD_XMM_XMMM128_IMM8:
        case Opcode::MPSADBW_XMM_XMMM128_IMM8:
        case Opcode::CRC32_R32_RM8: case Opcode::CRC32_R32_RM32:
        case Opcode::CRC32_R32_RM16: case Opcode::CRC32_R64_RM8:
        case Opcode::CRC32_R64_RM64:
        case Opcode::MOVBE_R16_M16: case Opcode::MOVBE_R32_M32:
        case Opcode::MOVBE_M16_R16: case Opcode::MOVBE_M32_R32:
        case Opcode::PCMPISTRI_XMM_XMMM128_IMM8: case Opcode::PCMPISTRM_XMM_XMMM128_IMM8:
        case Opcode::PCMPESTRI_XMM_XMMM128_IMM8: case Opcode::PCMPESTRM_XMM_XMMM128_IMM8:
        case Opcode::MOVBE_R64_M64: case Opcode::MOVBE_M64_R64:
            return exec_sse4_ops(ctx, instr);

        case Opcode::DAA: case Opcode::DAS: case Opcode::AAA: case Opcode::AAS:
        case Opcode::AAM: case Opcode::AAD:
        case Opcode::BOUND_R16_M32: case Opcode::BOUND_R32_M64:
        case Opcode::LES_R16_M32: case Opcode::LES_R32_M48:
        case Opcode::LDS_R16_M32: case Opcode::LDS_R32_M48:
        case Opcode::LSS_R16_M32: case Opcode::LSS_R32_M48: case Opcode::LSS_R64_M80:
        case Opcode::LFS_R16_M32: case Opcode::LFS_R32_M48: case Opcode::LFS_R64_M80:
        case Opcode::LGS_R16_M32: case Opcode::LGS_R32_M48: case Opcode::LGS_R64_M80:
            return exec_legacy_usermode(ctx, instr);

        case Opcode::XSAVE_M: case Opcode::XRSTOR_M: case Opcode::XSAVEOPT_M:
        case Opcode::XGETBV:
            return exec_xsave_ops(ctx, instr);

        case Opcode::ADCX_R32_RM32: case Opcode::ADCX_R64_RM64:
        case Opcode::ADOX_R32_RM32: case Opcode::ADOX_R64_RM64:
        case Opcode::VCVTPH2PS_XMM_XMMM64: case Opcode::VCVTPH2PS_YMM_XMMM128:
        case Opcode::VCVTPS2PH_XMMM64_XMM_IMM8: case Opcode::VCVTPS2PH_XMMM128_YMM_IMM8:
            return exec_f16c_adx(ctx, instr);

        case Opcode::ANDN_R32_R32_RM32: case Opcode::ANDN_R64_R64_RM64:
        case Opcode::BEXTR_R32_RM32_R32: case Opcode::BEXTR_R64_RM64_R64:
        case Opcode::BLSI_R32_RM32: case Opcode::BLSI_R64_RM64:
        case Opcode::BLSMSK_R32_RM32: case Opcode::BLSMSK_R64_RM64:
        case Opcode::BLSR_R32_RM32: case Opcode::BLSR_R64_RM64:
        case Opcode::BZHI_R32_RM32_R32: case Opcode::BZHI_R64_RM64_R64:
        case Opcode::MULX_R32_R32_RM32: case Opcode::MULX_R64_R64_RM64:
        case Opcode::PDEP_R32_R32_RM32: case Opcode::PDEP_R64_R64_RM64:
        case Opcode::PEXT_R32_R32_RM32: case Opcode::PEXT_R64_R64_RM64:
        case Opcode::RORX_R32_RM32_IMM8: case Opcode::RORX_R64_RM64_IMM8:
        case Opcode::SARX_R32_RM32_R32: case Opcode::SARX_R64_RM64_R64:
        case Opcode::SHRX_R32_RM32_R32: case Opcode::SHRX_R64_RM64_R64:
        case Opcode::SHLX_R32_RM32_R32: case Opcode::SHLX_R64_RM64_R64:
            return exec_bmi_ops(ctx, instr);

        case Opcode::VZEROUPPER: case Opcode::VZEROALL:
        case Opcode::VBROADCASTSS_XMM_M32: case Opcode::VBROADCASTSS_YMM_M32:
        case Opcode::VBROADCASTSS_XMM_XMM: case Opcode::VBROADCASTSS_YMM_XMM:
        case Opcode::VBROADCASTSD_YMM_M64: case Opcode::VBROADCASTSD_YMM_XMM:
        case Opcode::VBROADCASTF128_YMM_M128:
        case Opcode::VPBROADCASTB_XMM_XMMM8: case Opcode::VPBROADCASTB_YMM_XMMM8:
        case Opcode::VPBROADCASTW_XMM_XMMM16: case Opcode::VPBROADCASTW_YMM_XMMM16:
        case Opcode::VPBROADCASTD_XMM_XMMM32: case Opcode::VPBROADCASTD_YMM_XMMM32:
        case Opcode::VPBROADCASTQ_XMM_XMMM64: case Opcode::VPBROADCASTQ_YMM_XMMM64:
        case Opcode::VBROADCASTI128_YMM_M128:
        case Opcode::VEXTRACTF128_XMMM128_YMM_IMM8:
        case Opcode::VEXTRACTI128_XMMM128_YMM_IMM8:
        case Opcode::VINSERTF128_YMM_YMM_XMMM128_IMM8:
        case Opcode::VINSERTI128_YMM_YMM_XMMM128_IMM8:
        case Opcode::VPERM2F128_YMM_YMM_YMMM256_IMM8:
        case Opcode::VPERM2I128_YMM_YMM_YMMM256_IMM8:
        case Opcode::VPERMD_YMM_YMM_YMMM256: case Opcode::VPERMPS_YMM_YMM_YMMM256:
        case Opcode::VPERMQ_YMM_YMMM256_IMM8: case Opcode::VPERMPD_YMM_YMMM256_IMM8:
        case Opcode::VPBLENDD_XMM_XMM_XMMM128_IMM8:
        case Opcode::VPBLENDD_YMM_YMM_YMMM256_IMM8:
        case Opcode::VPBLENDVB_XMM_XMM_XMMM128_XMM:
        case Opcode::VPBLENDVB_YMM_YMM_YMMM256_YMM:
        case Opcode::VBLENDVPS_XMM_XMM_XMMM128_XMM:
        case Opcode::VBLENDVPS_YMM_YMM_YMMM256_YMM:
        case Opcode::VBLENDVPD_XMM_XMM_XMMM128_XMM:
        case Opcode::VBLENDVPD_YMM_YMM_YMMM256_YMM:
        case Opcode::VPSLLVD_XMM_XMM_XMMM128: case Opcode::VPSLLVD_YMM_YMM_YMMM256:
        case Opcode::VPSLLVQ_XMM_XMM_XMMM128: case Opcode::VPSLLVQ_YMM_YMM_YMMM256:
        case Opcode::VPSRLVD_XMM_XMM_XMMM128: case Opcode::VPSRLVD_YMM_YMM_YMMM256:
        case Opcode::VPSRLVQ_XMM_XMM_XMMM128: case Opcode::VPSRLVQ_YMM_YMM_YMMM256:
        case Opcode::VPSRAVD_XMM_XMM_XMMM128: case Opcode::VPSRAVD_YMM_YMM_YMMM256:
        case Opcode::VGATHERDPS_XMM_VM32X_XMM: case Opcode::VGATHERDPS_YMM_VM32Y_YMM:
        case Opcode::VPGATHERDD_XMM_VM32X_XMM: case Opcode::VPGATHERDD_YMM_VM32Y_YMM:
        case Opcode::VPMASKMOVD_LOAD: case Opcode::VPMASKMOVD_STORE:
        case Opcode::VPMASKMOVQ_LOAD: case Opcode::VPMASKMOVQ_STORE:
        case Opcode::VMASKMOVPS_LOAD: case Opcode::VMASKMOVPS_STORE:
        case Opcode::VMASKMOVPD_LOAD: case Opcode::VMASKMOVPD_STORE:
        case Opcode::VPERMILPS_VAR: case Opcode::VPERMILPS_IMM:
        case Opcode::VPERMILPD_VAR: case Opcode::VPERMILPD_IMM:
        case Opcode::VTESTPS: case Opcode::VTESTPD:
        case Opcode::VGATHERQPS: case Opcode::VPGATHERQD:
        case Opcode::VGATHERDPD: case Opcode::VPGATHERDQ:
        case Opcode::VGATHERQPD: case Opcode::VPGATHERQQ:
            return exec_avx_ops(ctx, instr);

        case Opcode::VFMADD132PS_XMM_XMM_XMMM128: case Opcode::VFMADD132PD_XMM_XMM_XMMM128:
        case Opcode::VFMADD132SS_XMM_XMM_XMMM32: case Opcode::VFMADD132SD_XMM_XMM_XMMM64:
        case Opcode::VFMADD213PS_XMM_XMM_XMMM128: case Opcode::VFMADD213PD_XMM_XMM_XMMM128:
        case Opcode::VFMADD213SS_XMM_XMM_XMMM32: case Opcode::VFMADD213SD_XMM_XMM_XMMM64:
        case Opcode::VFMADD231PS_XMM_XMM_XMMM128: case Opcode::VFMADD231PD_XMM_XMM_XMMM128:
        case Opcode::VFMADD231SS_XMM_XMM_XMMM32: case Opcode::VFMADD231SD_XMM_XMM_XMMM64:
        case Opcode::VFMSUB132PS_XMM_XMM_XMMM128: case Opcode::VFMSUB132PD_XMM_XMM_XMMM128:
        case Opcode::VFMSUB132SS_XMM_XMM_XMMM32: case Opcode::VFMSUB132SD_XMM_XMM_XMMM64:
        case Opcode::VFMSUB213PS_XMM_XMM_XMMM128: case Opcode::VFMSUB213PD_XMM_XMM_XMMM128:
        case Opcode::VFMSUB213SS_XMM_XMM_XMMM32: case Opcode::VFMSUB213SD_XMM_XMM_XMMM64:
        case Opcode::VFMSUB231PS_XMM_XMM_XMMM128: case Opcode::VFMSUB231PD_XMM_XMM_XMMM128:
        case Opcode::VFMSUB231SS_XMM_XMM_XMMM32: case Opcode::VFMSUB231SD_XMM_XMM_XMMM64:
        case Opcode::VFNMADD132PS_XMM_XMM_XMMM128: case Opcode::VFNMADD132PD_XMM_XMM_XMMM128:
        case Opcode::VFNMADD132SS_XMM_XMM_XMMM32: case Opcode::VFNMADD132SD_XMM_XMM_XMMM64:
        case Opcode::VFNMADD213PS_XMM_XMM_XMMM128: case Opcode::VFNMADD213PD_XMM_XMM_XMMM128:
        case Opcode::VFNMADD213SS_XMM_XMM_XMMM32: case Opcode::VFNMADD213SD_XMM_XMM_XMMM64:
        case Opcode::VFNMADD231PS_XMM_XMM_XMMM128: case Opcode::VFNMADD231PD_XMM_XMM_XMMM128:
        case Opcode::VFNMADD231SS_XMM_XMM_XMMM32: case Opcode::VFNMADD231SD_XMM_XMM_XMMM64:
        case Opcode::VFNMSUB132PS_XMM_XMM_XMMM128: case Opcode::VFNMSUB132PD_XMM_XMM_XMMM128:
        case Opcode::VFNMSUB132SS_XMM_XMM_XMMM32: case Opcode::VFNMSUB132SD_XMM_XMM_XMMM64:
        case Opcode::VFNMSUB213PS_XMM_XMM_XMMM128: case Opcode::VFNMSUB213PD_XMM_XMM_XMMM128:
        case Opcode::VFNMSUB213SS_XMM_XMM_XMMM32: case Opcode::VFNMSUB213SD_XMM_XMM_XMMM64:
        case Opcode::VFNMSUB231PS_XMM_XMM_XMMM128: case Opcode::VFNMSUB231PD_XMM_XMM_XMMM128:
        case Opcode::VFNMSUB231SS_XMM_XMM_XMMM32: case Opcode::VFNMSUB231SD_XMM_XMM_XMMM64:
        case Opcode::VFMADDSUB132PS_XMM_XMM_XMMM128: case Opcode::VFMADDSUB213PS_XMM_XMM_XMMM128:
        case Opcode::VFMADDSUB231PS_XMM_XMM_XMMM128:
        case Opcode::VFMADDSUB132PD_XMM_XMM_XMMM128: case Opcode::VFMADDSUB213PD_XMM_XMM_XMMM128:
        case Opcode::VFMADDSUB231PD_XMM_XMM_XMMM128:
        case Opcode::VFMSUBADD132PS_XMM_XMM_XMMM128: case Opcode::VFMSUBADD213PS_XMM_XMM_XMMM128:
        case Opcode::VFMSUBADD231PS_XMM_XMM_XMMM128:
        case Opcode::VFMSUBADD132PD_XMM_XMM_XMMM128: case Opcode::VFMSUBADD213PD_XMM_XMM_XMMM128:
        case Opcode::VFMSUBADD231PD_XMM_XMM_XMMM128:
            return exec_fma_ops(ctx, instr);
        
        default:
            ctx.exception = CpuException::InvalidOpcode;
            return ExecResult::Exception;
    }
}

} // namespace vitreo::cpu
