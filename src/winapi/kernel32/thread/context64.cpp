// SPDX-License-Identifier: LGPL-2.1-only

/// @file context64.cpp
/// @brief x64 CONTEXT record access and UNWIND_INFO interpretation.
///
/// Two Windows contracts live here, both of them byte-layout decoders:
///
///  - The AMD64 CONTEXT structure. Guest CPU state is written into a guest
///    CONTEXT record at the documented field offsets, which is what
///    RtlCaptureContext and the exception dispatcher hand to guest code.
///
///  - The UNWIND_INFO / UNWIND_CODE format from the x64 exception-handling
///    ABI. RtlVirtualUnwind replays those codes in reverse to reconstruct the
///    caller's register state one frame up.
///
/// Being pure format decoders over guest memory is what made these separable
/// from the rest of kernel32, and why they were extracted first.

#include "winapi/kernel32/thread/context64.hpp"

#include "winapi/core/marshaler.hpp"
#include "core/cpu/state/context64_record.hpp"
#include "core/cpu/state/cpu_state.hpp"

namespace vitreo::winapi {
namespace {

constexpr u32 CONTEXT_AMD64 = 0x00100000;
constexpr u32 CONTEXT_CONTROL = 0x00000001;
constexpr u32 CONTEXT_INTEGER = 0x00000002;
constexpr u32 CONTEXT_SEGMENTS = 0x00000004;
constexpr u32 CONTEXT_FLOATING_POINT = 0x00000008;
constexpr u32 CONTEXT_DEBUG_REGISTERS = 0x00000010;
constexpr u32 CONTEXT_FULL = CONTEXT_AMD64 | CONTEXT_CONTROL | CONTEXT_INTEGER | CONTEXT_FLOATING_POINT;

constexpr u8 UNW_FLAG_EHANDLER = 0x01;
constexpr u8 UNW_FLAG_UHANDLER = 0x02;
constexpr u8 UNW_FLAG_CHAININFO = 0x04;

constexpr u8 UWOP_PUSH_NONVOL = 0;
constexpr u8 UWOP_ALLOC_LARGE = 1;
constexpr u8 UWOP_ALLOC_SMALL = 2;
constexpr u8 UWOP_SET_FPREG = 3;
constexpr u8 UWOP_SAVE_NONVOL = 4;
constexpr u8 UWOP_SAVE_NONVOL_FAR = 5;
constexpr u8 UWOP_EPILOG = 6;
constexpr u8 UWOP_SAVE_XMM128 = 8;
constexpr u8 UWOP_SAVE_XMM128_FAR = 9;
constexpr u8 UWOP_PUSH_MACHFRAME = 10;

constexpr GuestAddr CONTEXT64_RSP_OFFSET = 0x98;
constexpr GuestAddr CONTEXT64_RBP_OFFSET = 0xA0;
constexpr GuestAddr CONTEXT64_RIP_OFFSET = 0xF8;
// winnt.h: CONTEXT.FltSave (XMM_SAVE_AREA32) at 0x100; its XmmRegisters[16]
// at +0xA0 -- the same storage the Xmm0..Xmm15 union members name.
constexpr GuestAddr CONTEXT64_XMM0_OFFSET = 0x1A0;

[[nodiscard]] GuestAddr context_gpr_offset(u8 reg) {
    switch (reg) {
        case 0: return 0x78; // RAX
        case 1: return 0x80; // RCX
        case 2: return 0x88; // RDX
        case 3: return 0x90; // RBX
        case 4: return 0x98; // RSP
        case 5: return 0xA0; // RBP
        case 6: return 0xA8; // RSI
        case 7: return 0xB0; // RDI
        case 8: return 0xB8; // R8
        case 9: return 0xC0; // R9
        case 10: return 0xC8; // R10
        case 11: return 0xD0; // R11
        case 12: return 0xD8; // R12
        case 13: return 0xE0; // R13
        case 14: return 0xE8; // R14
        case 15: return 0xF0; // R15
        default: return 0;
    }
}

void write_context64_gpr(Marshaler& m, GuestAddr ctx_addr, u8 reg, u64 value) {
    GuestAddr offset = context_gpr_offset(reg);
    if (offset != 0) {
        (void)m.write_u64(ctx_addr + offset, value);
    }
}

[[nodiscard]] u64 read_context64_gpr(Marshaler& m, GuestAddr ctx_addr, u8 reg) {
    GuestAddr offset = context_gpr_offset(reg);
    if (offset == 0) {
        return 0;
    }
    auto value = m.read_u64(ctx_addr + offset);
    return value.is_ok() ? value.value() : 0;
}

[[nodiscard]] bool unwind_epilog64(Marshaler& m, GuestAddr control_pc,
                                   GuestAddr function_end, GuestAddr ctx_addr) {
    u64 rsp = read_context64_gpr(m, ctx_addr, 4);
    struct RestoredReg { u8 reg; u64 value; };
    RestoredReg restored[16]{};
    usize restored_count = 0;
    bool saw_epilog_op = false;
    GuestAddr pc = control_pc;

    for (usize instruction = 0; instruction < 16 && pc < function_end;
         ++instruction) {
        u8 rex = 0;
        auto first = m.read_u8(pc);
        if (!first.is_ok()) return false;
        u8 opcode = first.value();
        if ((opcode & 0xF0u) == 0x40u) {
            rex = opcode;
            auto next = m.read_u8(++pc);
            if (!next.is_ok()) return false;
            opcode = next.value();
        }

        if (opcode >= 0x58u && opcode <= 0x5Fu) {
            const u8 reg = static_cast<u8>((opcode - 0x58u) |
                                           ((rex & 1u) ? 8u : 0u));
            auto value = m.read_u64(rsp);
            if (!value.is_ok() || restored_count == 16) return false;
            restored[restored_count++] = RestoredReg{reg, value.value()};
            rsp += 8;
            ++pc;
            saw_epilog_op = true;
            continue;
        }

        if (rex == 0x48u && opcode == 0x83u &&
            m.read_u8(pc + 1).value_or(0) == 0xC4u) {
            auto amount = m.read_u8(pc + 2);
            if (!amount.is_ok()) return false;
            rsp += amount.value();
            pc += 3;
            saw_epilog_op = true;
            continue;
        }
        if (rex == 0x48u && opcode == 0x81u &&
            m.read_u8(pc + 1).value_or(0) == 0xC4u) {
            auto amount = m.read_u32(pc + 2);
            if (!amount.is_ok()) return false;
            rsp += amount.value();
            pc += 6;
            saw_epilog_op = true;
            continue;
        }

        // Microsoft x64 permits `lea rsp, displacement[FPReg]` as the
        // frame-pointer form of fixed-stack deallocation. The ModRM/SIB form
        // must name RSP as the destination and have no index register.
        if ((rex & 0x48u) == 0x48u && opcode == 0x8Du) {
            auto modrm_read = m.read_u8(pc + 1);
            if (!modrm_read.is_ok()) return false;
            const u8 modrm = modrm_read.value();
            const u8 mod = static_cast<u8>(modrm >> 6);
            const u8 dst = static_cast<u8>(
                ((modrm >> 3) & 7u) | ((rex & 4u) ? 8u : 0u));
            u8 base = static_cast<u8>(
                (modrm & 7u) | ((rex & 1u) ? 8u : 0u));
            GuestAddr displacement_addr = pc + 2;
            if ((modrm & 7u) == 4u) {
                auto sib_read = m.read_u8(displacement_addr++);
                if (!sib_read.is_ok()) return false;
                const u8 sib = sib_read.value();
                if (((sib >> 3) & 7u) != 4u) return false;
                base = static_cast<u8>(
                    (sib & 7u) | ((rex & 1u) ? 8u : 0u));
            }
            if (dst != 4u || (mod != 1u && mod != 2u)) return false;

            i64 displacement = 0;
            if (mod == 1u) {
                auto value = m.read_u8(displacement_addr);
                if (!value.is_ok()) return false;
                displacement = static_cast<i8>(value.value());
                ++displacement_addr;
            } else {
                auto value = m.read_u32(displacement_addr);
                if (!value.is_ok()) return false;
                displacement = static_cast<i32>(value.value());
                displacement_addr += 4;
            }
            rsp = static_cast<u64>(
                static_cast<i64>(read_context64_gpr(
                    m, ctx_addr, base)) + displacement);
            pc = displacement_addr;
            saw_epilog_op = true;
            continue;
        }

        if (opcode == 0xC3u && saw_epilog_op) {
            auto caller = m.read_u64(rsp);
            if (!caller.is_ok()) return false;
            for (usize i = 0; i < restored_count; ++i) {
                write_context64_gpr(
                    m, ctx_addr, restored[i].reg, restored[i].value);
            }
            (void)m.write_u64(ctx_addr + CONTEXT64_RSP_OFFSET, rsp + 8);
            (void)m.write_u64(
                ctx_addr + CONTEXT64_RIP_OFFSET, caller.value());
            return true;
        }
        return false;
    }
    return false;
}


} // namespace

[[nodiscard]] bool unwind_context64(Marshaler& m,
                                    GuestAddr image_base,
                                    GuestAddr control_pc,
                                    GuestAddr function_entry_addr,
                                    GuestAddr ctx_addr,
                                    GuestAddr handler_data_addr,
                                    GuestAddr establisher_frame_addr,
                                    GuestAddr* handler_routine_out,
                                    u32 handler_type) {
    if (image_base == 0 || function_entry_addr == 0 || ctx_addr == 0) {
        return false;
    }

    u64 rsp = 0;
    auto rsp_read = m.read_u64(ctx_addr + CONTEXT64_RSP_OFFSET);
    if (rsp_read.is_ok()) {
        rsp = rsp_read.value();
    }
    GuestAddr establisher_frame = rsp;

    // Learn x64 EH: UNW_FLAG_CHAININFO shares unwind codes across split
    // functions. The chained node ends with a RUNTIME_FUNCTION for the
    // primary; the language handler lives on the primary. Follow the chain
    // before popping the return address.
    GuestAddr current_entry = function_entry_addr;
    u8 flags = 0;
    u8 count = 0;
    GuestAddr unwind_addr = 0;
    // Learn x64 EH, UWOP_PUSH_MACHFRAME: the frame restores RIP and RSP
    // itself, so no return address is popped afterwards.
    bool machine_frame = false;
    u64 machine_frame_rip = 0;

    for (u32 hop = 0; hop < 32; ++hop) {
    auto begin_rva = m.read_u32(current_entry + 0x0);
    auto end_rva = m.read_u32(current_entry + 0x4);
    auto unwind_rva = m.read_u32(current_entry + 0x8);
    if (!begin_rva.is_ok() || !end_rva.is_ok() || !unwind_rva.is_ok() || unwind_rva.value() == 0) {
        return false;
    }

    unwind_addr = image_base + unwind_rva.value();
    auto version_flags = m.read_u8(unwind_addr + 0x0);
    auto prolog_size = m.read_u8(unwind_addr + 0x1);
    auto unwind_code_count = m.read_u8(unwind_addr + 0x2);
    auto frame_info = m.read_u8(unwind_addr + 0x3);
    if (!version_flags.is_ok() || !prolog_size.is_ok() ||
        !unwind_code_count.is_ok() || !frame_info.is_ok()) {
        return false;
    }

    flags = static_cast<u8>(version_flags.value() >> 3);
    count = unwind_code_count.value();
    const u8 frame_reg = static_cast<u8>(frame_info.value() & 0x0F);
    const u8 frame_off = static_cast<u8>(frame_info.value() >> 4);
    const u32 offset_in_function = control_pc >= image_base + begin_rva.value()
        ? static_cast<u32>(control_pc - (image_base + begin_rva.value()))
        : 0;
    const bool in_prolog = offset_in_function < prolog_size.value();

    // Microsoft x64 EH: when ControlPc is in an epilog, simulate only the
    // remaining epilog instructions. Replaying the complete prologue unwind
    // from a partially restored RSP deallocates the frame twice.
    if (!in_prolog &&
        unwind_epilog64(m, control_pc, image_base + end_rva.value(), ctx_addr)) {
        if (handler_data_addr != 0) (void)m.write_u64(handler_data_addr, 0);
        if (establisher_frame_addr != 0) {
            (void)m.write_u64(establisher_frame_addr, establisher_frame);
        }
        if (handler_routine_out != nullptr) *handler_routine_out = 0;
        return true;
    }

    if (frame_reg != 0) {
        establisher_frame = read_context64_gpr(m, ctx_addr, frame_reg) - static_cast<u64>(frame_off) * 16;
    }
    // Learn x64 EH, "Unwind operation code": an offset-taking code is relative
    // to the lowest address of the fixed allocation -- RSP when there is no
    // frame register, otherwise FP - 16 * FrameOffset -- NOT to the running
    // unwind RSP. The two differ in any frame-pointer function that also
    // moved RSP after its prolog (alloca), and the save codes are processed
    // before SET_FPREG because they follow it in the prolog.
    const u64 frame_base = frame_reg != 0 ? establisher_frame : rsp;

    usize slot = 0;
    while (slot < count) {
        const GuestAddr code_addr = unwind_addr + 0x4 + slot * 2;
        auto code_offset = m.read_u8(code_addr + 0x0);
        auto op_byte = m.read_u8(code_addr + 0x1);
        if (!code_offset.is_ok() || !op_byte.is_ok()) {
            return false;
        }

        const u8 unwind_op = static_cast<u8>(op_byte.value() & 0x0F);
        const u8 op_info = static_cast<u8>(op_byte.value() >> 4);
        if (in_prolog && code_offset.value() > offset_in_function) {
            slot += 1;
            if (unwind_op == UWOP_ALLOC_LARGE) {
                slot += (op_info == 0) ? 1 : 2;
            } else if (unwind_op == UWOP_SAVE_NONVOL || unwind_op == UWOP_SAVE_XMM128) {
                slot += 1;
            } else if (unwind_op == UWOP_SAVE_NONVOL_FAR ||
                       unwind_op == UWOP_SAVE_XMM128_FAR) {
                slot += 2;
            }
            continue;
        }

        switch (unwind_op) {
            case UWOP_PUSH_NONVOL: {
                auto saved = m.read_u64(rsp);
                if (!saved.is_ok()) {
                    return false;
                }
                write_context64_gpr(m, ctx_addr, op_info, saved.value());
                rsp += 8;
                slot += 1;
                break;
            }
            case UWOP_ALLOC_LARGE: {
                if (op_info == 0) {
                    auto size_slot = m.read_u16(code_addr + 0x2);
                    if (!size_slot.is_ok()) {
                        return false;
                    }
                    rsp += static_cast<u64>(size_slot.value()) * 8;
                    slot += 2;
                } else {
                    auto low = m.read_u16(code_addr + 0x2);
                    auto high = m.read_u16(code_addr + 0x4);
                    if (!low.is_ok() || !high.is_ok()) {
                        return false;
                    }
                    rsp += static_cast<u64>(low.value()) |
                           (static_cast<u64>(high.value()) << 16);
                    slot += 3;
                }
                break;
            }
            case UWOP_ALLOC_SMALL:
                rsp += static_cast<u64>(op_info) * 8 + 8;
                slot += 1;
                break;
            case UWOP_SET_FPREG:
                if (frame_reg != 0) {
                    rsp = read_context64_gpr(m, ctx_addr, frame_reg) - static_cast<u64>(frame_off) * 16;
                    establisher_frame = rsp;
                }
                slot += 1;
                break;
            case UWOP_SAVE_NONVOL: {
                auto frame_offset = m.read_u16(code_addr + 0x2);
                if (!frame_offset.is_ok()) {
                    return false;
                }
                auto saved = m.read_u64(frame_base + static_cast<u64>(frame_offset.value()) * 8);
                if (!saved.is_ok()) {
                    return false;
                }
                write_context64_gpr(m, ctx_addr, op_info, saved.value());
                slot += 2;
                break;
            }
            case UWOP_SAVE_NONVOL_FAR: {
                auto low = m.read_u16(code_addr + 0x2);
                auto high = m.read_u16(code_addr + 0x4);
                if (!low.is_ok() || !high.is_ok()) {
                    return false;
                }
                const u64 frame_offset = static_cast<u64>(low.value()) |
                                         (static_cast<u64>(high.value()) << 16);
                auto saved = m.read_u64(frame_base + frame_offset);
                if (!saved.is_ok()) {
                    return false;
                }
                write_context64_gpr(m, ctx_addr, op_info, saved.value());
                slot += 3;
                break;
            }
            case UWOP_EPILOG:
                // Learn x64 EH (Win10+): epilog marker; no stack effect.
                slot += 1;
                break;
            case UWOP_SAVE_XMM128:
            case UWOP_SAVE_XMM128_FAR: {
                // Learn x64 EH: all 128 bits of nonvolatile XMM<op_info> were
                // saved at a 16-byte-scaled (short) or unscaled 32-bit (far)
                // offset from the frame base. Restoring them is the point:
                // skipping this left the caller running on whatever XMM6-15
                // the callee had put there.
                u64 offset = 0;
                if (unwind_op == UWOP_SAVE_XMM128) {
                    auto scaled = m.read_u16(code_addr + 0x2);
                    if (!scaled.is_ok()) return false;
                    offset = static_cast<u64>(scaled.value()) * 16;
                } else {
                    auto low = m.read_u16(code_addr + 0x2);
                    auto high = m.read_u16(code_addr + 0x4);
                    if (!low.is_ok() || !high.is_ok()) return false;
                    offset = static_cast<u64>(low.value()) |
                             (static_cast<u64>(high.value()) << 16);
                }
                auto lo = m.read_u64(frame_base + offset);
                auto hi = m.read_u64(frame_base + offset + 8);
                if (!lo.is_ok() || !hi.is_ok()) return false;
                const GuestAddr xmm_slot =
                    ctx_addr + CONTEXT64_XMM0_OFFSET + static_cast<GuestAddr>(op_info) * 16;
                (void)m.write_u64(xmm_slot, lo.value());
                (void)m.write_u64(xmm_slot + 8, hi.value());
                slot += (unwind_op == UWOP_SAVE_XMM128) ? 2 : 3;
                break;
            }
            case UWOP_PUSH_MACHFRAME: {
                // Learn x64 EH: RIP at [RSP] (op_info 0) or [RSP+8] after an
                // error code (op_info 1); the interrupted RSP is 24 bytes
                // above RIP. The frame supplies both, so the unwind ends here
                // without popping a return address.
                const u64 frame = rsp + (op_info == 0 ? 0u : 8u);
                auto frame_rip = m.read_u64(frame);
                auto frame_rsp = m.read_u64(frame + 24);
                if (!frame_rip.is_ok() || !frame_rsp.is_ok()) return false;
                machine_frame = true;
                machine_frame_rip = frame_rip.value();
                rsp = frame_rsp.value();
                slot += 1;
                break;
            }
            default:
                return false;
        }
    }

    if ((flags & UNW_FLAG_CHAININFO) != 0) {
        const usize aligned_count = (static_cast<usize>(count) + 1u) & ~1u;
        current_entry = unwind_addr + 0x4 + aligned_count * 2;
        continue;
    }
    break;
    } // chain hops

    u64 caller_rip = machine_frame_rip;
    if (!machine_frame) {
        auto popped = m.read_u64(rsp);
        if (!popped.is_ok()) {
            return false;
        }
        caller_rip = popped.value();
        rsp += 8;
    }

    (void)m.write_u64(ctx_addr + CONTEXT64_RSP_OFFSET, rsp);
    (void)m.write_u64(ctx_addr + CONTEXT64_RIP_OFFSET, caller_rip);
    if (establisher_frame_addr != 0) {
        (void)m.write_u64(establisher_frame_addr, establisher_frame);
    }

    if (handler_data_addr != 0) {
        GuestAddr handler_data = 0;
        // Learn, RtlVirtualUnwind: the handler is returned only when the
        // UNWIND_INFO declares one of the requested HandlerType kinds.
        if ((flags & handler_type & (UNW_FLAG_EHANDLER | UNW_FLAG_UHANDLER)) != 0) {
            const usize aligned_count = (static_cast<usize>(count) + 1u) & ~1u;
            const GuestAddr handler_rva_addr = unwind_addr + 0x4 + aligned_count * 2;
            auto handler_rva = m.read_u32(handler_rva_addr);
            if (handler_rva.is_ok() && handler_routine_out != nullptr) {
                *handler_routine_out = image_base + handler_rva.value();
            }
            handler_data = handler_rva_addr + 4;
        }
        (void)m.write_u64(handler_data_addr, handler_data);
    }

    if ((flags & handler_type & (UNW_FLAG_EHANDLER | UNW_FLAG_UHANDLER)) == 0 &&
        handler_routine_out != nullptr) {
        *handler_routine_out = 0;
    }

    return true;
}

void write_context64(Marshaler& m, GuestAddr ctx_addr) {
    if (ctx_addr == 0 || !m.has_cpu()) {
        return;
    }

    // RtlCaptureContext reports its caller: RIP is the return address and RSP
    // the caller's stack pointer after the return.
    cpu::CpuState captured = *m.cpu();
    captured.materialize_lazy_flags();
    if (auto ret_addr = m.read_u64(captured.gpr.rsp); ret_addr.is_ok()) {
        captured.rip = ret_addr.value();
        captured.gpr.rsp += 8;
    }
    // Measured (thread_context_probe CAPTURE): ContextFlags 0x10000F --
    // CONTROL|INTEGER|SEGMENTS|FLOATING_POINT, no debug registers.
    (void)cpu::store_context64(m.memory(), ctx_addr, captured,
                               CONTEXT_FULL | CONTEXT_SEGMENTS);
}

bool capture_context64_to(Marshaler& m, GuestAddr ctx_addr,
                          const cpu::CpuState& source) {
    if (ctx_addr == 0) return false;
    const auto flags = m.read_u32(ctx_addr + 0x30);
    if (!flags.is_ok()) return false;
    return cpu::store_context64(m.memory(), ctx_addr, source, flags.value()) ==
           cpu::Context64Status::Ok;
}

bool restore_context64_to(Marshaler& m, GuestAddr ctx_addr,
                          cpu::CpuState& target) {
    if (ctx_addr == 0) {
        return false;
    }
    return cpu::load_context64(m.memory(), ctx_addr, target,
                               /*restoring_current_thread=*/&target == m.cpu()) ==
           cpu::Context64Status::Ok;
}

bool restore_context64(Marshaler& m, GuestAddr ctx_addr) {
    auto* cpu = m.cpu();
    return cpu != nullptr && restore_context64_to(m, ctx_addr, *cpu);
}

} // namespace vitreo::winapi
