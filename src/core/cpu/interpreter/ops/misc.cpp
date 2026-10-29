// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// Miscellaneous instruction execution (LEA, NOP, INT, HLT, UD2, CPUID, RDTSC, XCHG)

#include "exec_ops.hpp"
#include "core/cpu/cpuid_synth.hpp"
#include "helpers.hpp"
#include <vitreo/virtual_tsc.hpp>
#include <vitreo/time_diag.hpp>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>

namespace vitreo::cpu::ops {

u64 virtual_tsc_ticks() noexcept {
    // Both the interpreter's RDTSC/RDTSCP case and the JIT's jit_virtual_tsc_ticks
    // helper come through here, so one observation covers every guest RDTSC.
    // QueryPerformanceCounter reads ::vitreo::virtual_tsc_ticks directly and is
    // instrumented at its own call sites, keeping the two APIs distinguishable.
    const u64 tsc = ::vitreo::virtual_tsc_ticks();
    ::vitreo::time_diag_observe(::vitreo::TimeApi::Rdtsc, tsc);
    return tsc;
}

ExecResult exec_lea(ExecContext& ctx, const DecodedInstruction& instr) noexcept {
    const auto& dst_op = instr.operands[0];
    const auto& src_op = instr.operands[1];
    
    if (!is_mem_type(src_op.type)) {
        ctx.exception = CpuException::InvalidOpcode;
        return ExecResult::Exception;
    }
    
    auto addr = calc_effective_address(ctx, src_op.mem);
    if (!addr) {
        ctx.exception = CpuException::GeneralProtection;
        return ExecResult::Exception;
    }
    
    write_register(ctx, dst_op.type, dst_op.reg, addr.value());
    return ExecResult::Continue;
}

ExecResult exec_int(ExecContext& ctx, const DecodedInstruction& instr) noexcept {
    switch (instr.opcode) {
        case Opcode::INT_3:
            ctx.exception = CpuException::Breakpoint;
            ctx.interrupt_num = 3;
            // #BP is a trap: the CPU leaves RIP past the instruction and the
            // breakpoint address is that minus one, which Windows reports as
            // both ExceptionAddress and the handler's context RIP
            // (int3_record_probe). The dispatchers apply the context half.
            ctx.cpu.advance_ip(instr.length);
            ctx.exception_addr = ctx.cpu.rip - 1;
            return ExecResult::Interrupt;
        
        // INT1 / ICEBP (F1) raises #DB directly rather than through the IDT
        // gate that INT 01 (CD 01) uses. Like #BP it is a trap: the resume
        // context points past the one-byte instruction, and the trap address
        // is kept separately for EXCEPTION_RECORD.ExceptionAddress.
        case Opcode::INT_1:
            ctx.exception = CpuException::Debug;
            ctx.interrupt_num = 1;
            ctx.exception_addr = ctx.cpu.rip;
            ctx.cpu.advance_ip(instr.length);
            return ExecResult::Interrupt;

        case Opcode::INT_IMM8: {
            auto num = read_operand(ctx, instr, 0);
            if (!num) {
                ctx.exception = CpuException::GeneralProtection;
                return ExecResult::Exception;
            }
            ctx.interrupt_num = static_cast<u8>(num.value());
            // INT 3 in its two-byte form (CD 03) is the same #BP trap as
            // INT3: Windows reports the instruction end minus one -- the
            // 03 byte -- as the breakpoint address (int3_record_probe cd03).
            if (ctx.interrupt_num == 3) {
                ctx.exception = CpuException::Breakpoint;
                ctx.cpu.advance_ip(instr.length);
                ctx.exception_addr = ctx.cpu.rip - 1;
            }
            return ExecResult::Interrupt;
        }

        // INTO — Intel SDM INT n/INTO/INT3: if OF=1, interrupt 4 (like INT 4).
        // RIP is not advanced before Interrupt (same as INT_IMM8).
        case Opcode::INTO: {
            ctx.cpu.materialize_lazy_flags();
            if (!ctx.cpu.flags.of()) {
                return ExecResult::Continue;
            }
            ctx.interrupt_num = 4;
            return ExecResult::Interrupt;
        }
        
        default:
            ctx.exception = CpuException::InvalidOpcode;
            return ExecResult::Exception;
    }
}

ExecResult exec_xchg(ExecContext& ctx, const DecodedInstruction& instr) noexcept {
    if (instr.operand_count < 2) {
        ctx.exception = CpuException::InvalidOpcode;
        return ExecResult::Exception;
    }
    
    auto val1 = read_operand(ctx, instr, 0);
    auto val2 = read_operand(ctx, instr, 1);
    if (!val1 || !val2) {
        ctx.exception = CpuException::GeneralProtection;
        return ExecResult::Exception;
    }
    
    // Swap values
    auto r1 = write_operand(ctx, instr, 0, val2.value());
    auto r2 = write_operand(ctx, instr, 1, val1.value());
    if (!r1 || !r2) {
        ctx.exception = CpuException::GeneralProtection;
        return ExecResult::Exception;
    }
    
    return ExecResult::Continue;
}


ExecResult exec_misc(ExecContext& ctx, const DecodedInstruction& instr) noexcept {
    switch (instr.opcode) {
        // NOP variations. PAUSE (F3 90) is a spin-wait hint with no
        // architectural state change (Intel SDM PAUSE).
        case Opcode::NOP:
        case Opcode::NOP_RM:
        case Opcode::PAUSE:
        case Opcode::ENDBR32:
        case Opcode::ENDBR64:
            // CET landing pads execute as NOP when CET is not enabled
            // (Intel SDM Vol. 2 ENDBR32/ENDBR64).
            return ExecResult::Continue;

        case Opcode::LFENCE:
            std::atomic_thread_fence(std::memory_order_acquire);
            return ExecResult::Continue;

        case Opcode::MFENCE:
            std::atomic_thread_fence(std::memory_order_seq_cst);
            return ExecResult::Continue;

        case Opcode::SFENCE:
            std::atomic_thread_fence(std::memory_order_release);
            return ExecResult::Continue;

        case Opcode::XLAT: {
            constexpr u8 RAX = static_cast<u8>(RegIndex::RAX);
            constexpr u8 RBX = static_cast<u8>(RegIndex::RBX);

            u64 offset = 0;
            if (ctx.cpu.is_long_mode()) {
                offset = instr.prefixes.address_size
                    ? static_cast<u64>(ctx.cpu.gpr.get32(RBX))
                    : ctx.cpu.gpr.get64(RBX);
            } else if (ctx.cpu.is_real_mode()) {
                offset = instr.prefixes.address_size
                    ? static_cast<u64>(ctx.cpu.gpr.get32(RBX))
                    : static_cast<u64>(ctx.cpu.gpr.get16(RBX));
            } else {
                offset = instr.prefixes.address_size
                    ? static_cast<u64>(ctx.cpu.gpr.get16(RBX))
                    : static_cast<u64>(ctx.cpu.gpr.get32(RBX));
            }

            offset += ctx.cpu.gpr.get8l(RAX);
            if (!ctx.cpu.is_long_mode()) {
                if ((ctx.cpu.is_real_mode() &&
                     !instr.prefixes.address_size) ||
                    (ctx.cpu.is_protected_mode() &&
                     instr.prefixes.address_size)) {
                    offset = static_cast<u16>(offset);
                } else {
                    offset = static_cast<u32>(offset);
                }
            } else if (instr.prefixes.address_size) {
                offset = static_cast<u32>(offset);
            }

            const SegReg segment =
                instr.prefixes.segment == SegReg::Default
                    ? SegReg::DS
                    : instr.prefixes.segment;
            const auto segment_index =
                static_cast<SegIndex>(static_cast<u8>(segment));
            if (!ctx.cpu.is_long_mode() ||
                segment == SegReg::FS ||
                segment == SegReg::GS) {
                offset += ctx.cpu.seg.get_desc(segment_index).base;
            }

            auto value = read_memory(ctx, offset, 1);
            if (value.is_err()) {
                ctx.exception = CpuException::GeneralProtection;
                return ExecResult::Exception;
            }
            ctx.cpu.gpr.set8l(RAX, static_cast<u8>(value.value()));
            return ExecResult::Continue;
        }
        
        // HLT is privileged. Vitreo executes Windows user-mode code only, so
        // the guest is always at CPL > 0 here. Intel SDM Vol. 2 (HLT) requires
        // #GP(0) in that case; Windows exposes the fault as
        // STATUS_PRIVILEGED_INSTRUCTION rather than halting the process.
        case Opcode::HLT:
            ctx.exception = CpuException::PrivilegedInstruction;
            return ExecResult::Exception;

        // SYSCALL saves the following RIP in RCX and the current RFLAGS in
        // R11. The user-mode runtime performs the NT service dispatch after
        // this architectural transition, so no kernel target/MSR state is
        // installed here.
        case Opcode::SYSCALL:
            if (!ctx.cpu.is_long_mode()) {
                ctx.exception = CpuException::InvalidOpcode;
                return ExecResult::Exception;
            }
            ctx.cpu.materialize_lazy_flags();
            ctx.cpu.gpr.rcx = ctx.cpu.rip + instr.length;
            ctx.cpu.gpr.r11 = ctx.cpu.flags.rflags;
            ctx.cpu.rip += instr.length;
            return ExecResult::Syscall;

        // SYSENTER is the 32-bit SEP companion to SYSCALL. The user-mode
        // runtime dispatches the NT service; MSRs are not installed here.
        case Opcode::SYSENTER:
            ctx.cpu.rip += instr.length;
            return ExecResult::Syscall;
        
        // UD2 - Undefined instruction (intentional)
        case Opcode::UD2:
        // UD1 (0F B9 /r) and UD0 (0F FF /r) raise #UD exactly as UD2 does.
        // They are decoded distinctly only so the reported instruction length
        // covers their ModRM/SIB/displacement bytes (Intel SDM Vol. 2 "UD").
        case Opcode::UD1_R_RM:
        case Opcode::UD0_R_RM:
        // An EVEX-encoded instruction. Vitreo's CPUID advertises no
        // EVEX-encoded ISA (AVX-512, AVX10, APX and AMX are all hidden), and a
        // processor without those features raises #UD -- so this is the
        // architecturally correct outcome, not a shortfall. It is a named
        // opcode so the #UD can be told apart from corrupt code.
        case Opcode::EVEX_UNSUPPORTED:
            ctx.exception = CpuException::InvalidOpcode;
            return ExecResult::Exception;

        // SALC (D6): AL = CF ? 0xFF : 0x00. Undocumented; affects NO flags.
        // Decoded only outside 64-bit mode, so no mode check is needed here.
        case Opcode::SALC:
            ctx.cpu.gpr.set8l(0, ctx.cpu.flags.cf() ? u8{0xFF} : u8{0x00});
            return ExecResult::Continue;

        // ARPL r/m16, r16 (63 /r) — Adjust RPL Field of Segment Selector.
        // Intel SDM Vol. 2 ARPL:
        //   IF DEST.RPL < SRC.RPL THEN ZF = 1; DEST.RPL = SRC.RPL
        //                         ELSE ZF = 0
        // RPL is bits 1:0. Only ZF is affected -- CF/PF/AF/SF/OF are
        // explicitly unchanged, so this must not go through a flag helper that
        // writes the full arithmetic set.
        //
        // Decoded only outside 64-bit mode (there the same byte is MOVSXD).
        case Opcode::ARPL_RM16_R16: {
            const auto dest = read_operand(ctx, instr, 0);
            const auto src = read_operand(ctx, instr, 1);
            if (!dest || !src) {
                ctx.exception = CpuException::GeneralProtection;
                return ExecResult::Exception;
            }
            const u16 d = static_cast<u16>(dest.value());
            const u16 s = static_cast<u16>(src.value());
            if ((d & 3) < (s & 3)) {
                ctx.cpu.flags.set_zf(true);
                const u16 adjusted = static_cast<u16>((d & 0xFFFC) | (s & 3));
                if (!write_operand(ctx, instr, 0, adjusted)) {
                    ctx.exception = CpuException::GeneralProtection;
                    return ExecResult::Exception;
                }
            } else {
                ctx.cpu.flags.set_zf(false);
            }
            return ExecResult::Continue;
        }
        
        // CPUID - Return CPU identification
        case Opcode::CPUID: {
            const u32 leaf = ctx.cpu.gpr.get32(0);     // EAX = leaf
            const u32 subleaf = ctx.cpu.gpr.get32(1);  // ECX = subleaf
            u32 eax = 0, ebx = 0, ecx = 0, edx = 0;
            synth_cpuid(leaf, subleaf, ctx.cpu.is_long_mode(),
                        eax, ebx, ecx, edx);
            ctx.cpu.gpr.set32(0, eax);  // EAX
            ctx.cpu.gpr.set32(3, ebx);  // EBX
            ctx.cpu.gpr.set32(1, ecx);  // ECX
            ctx.cpu.gpr.set32(2, edx);  // EDX
            return ExecResult::Continue;
        }
        
        // RDTSC - monotonic virtual TSC at guest_tsc_hz(). Shares the
        // QueryPerformanceCounter timebase but not its rate, exactly as
        // real hardware reports a GHz TSC against a 10 MHz QPF.
        case Opcode::RDTSC:
        // RDTSCP (Intel SDM Vol. 2): EDX:EAX := TSC; ECX := IA32_TSC_AUX.
        // IA32_TSC_AUX is not installed; a single virtual logical processor
        // returns 0. set32 zero-extends into RCX.
        case Opcode::RDTSCP: {
            const u64 tsc = virtual_tsc_ticks();
            ctx.cpu.gpr.set32(0, static_cast<u32>(tsc));
            ctx.cpu.gpr.set32(2, static_cast<u32>(tsc >> 32));
            if (instr.opcode == Opcode::RDTSCP) {
                ctx.cpu.gpr.set32(1, 0);
            }
            return ExecResult::Continue;
        }
        
        default:
            ctx.exception = CpuException::InvalidOpcode;
            return ExecResult::Exception;
    }
}

} // namespace vitreo::cpu::ops
