// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - the x64 Windows CONTEXT record, one implementation

/// @file context64_record.cpp
/// @brief See context64_record.hpp for the measured contract.

#include "core/cpu/state/context64_record.hpp"

#include "core/cpu/state/cpu_state.hpp"
#include "core/cpu/state/fx_image.hpp"
#include "core/math/host_fenv.hpp"
#include "core/memory/guest_memory.hpp"

namespace vitreo::cpu {
namespace {

// winnt.h CONTEXT (AMD64) field offsets.
constexpr u32 kMxCsr = 0x34;
constexpr u32 kSegCs = 0x38;
constexpr u32 kSegDs = 0x3A;
constexpr u32 kSegEs = 0x3C;
constexpr u32 kSegFs = 0x3E;
constexpr u32 kSegGs = 0x40;
constexpr u32 kSegSs = 0x42;
constexpr u32 kEFlags = 0x44;
constexpr u32 kDr0 = 0x48;       // Dr0..Dr3, Dr6, Dr7: six qwords
constexpr u32 kDr7 = 0x70;
constexpr u32 kGpr0 = 0x78;      // Rax; Rcx..R15 follow in encoding order
constexpr u32 kRsp = 0x98;
constexpr u32 kRip = 0xF8;
constexpr u32 kFltSave = 0x100;  // XMM_SAVE_AREA32 == the FXSAVE image
constexpr u32 kFltSaveWritten = 160 + 16 * 16;  // x87/MXCSR header + Xmm0-15
constexpr u32 kLastBranchToRip = 0x4B0;  // ..LastExceptionFromRip at 0x4C8

// Windows x64 user-mode selectors (measured, see the header).
constexpr u16 kUserCs = 0x33;
constexpr u16 kUserDataSelector = 0x2B;
constexpr u16 kUserFs = 0x53;

// RFLAGS bit 1 is always one and user mode always runs with IF set.
constexpr u32 kEFlagsFixed = 0x202;
// DR7 L0/G0..L3/G3: any of these arms a hardware breakpoint.
constexpr u64 kDr7EnableBits = 0xFF;

bool selected(u32 flags, u32 group) { return (flags & group) == group; }

} // namespace

Context64Status store_context64(GuestMemory& memory, GuestAddr record,
                                const CpuState& cpu, u32 context_flags) {
    const u32 flags = context_flags | kContext64Amd64;
    bool ok = memory.write_u32(record + kContext64FlagsOffset, flags).is_ok();

    if (selected(flags, kContext64Control)) {
        ok = ok && memory.write_u16(record + kSegCs, kUserCs).is_ok();
        ok = ok && memory.write_u16(record + kSegSs, kUserDataSelector).is_ok();
        ok = ok && memory.write_u32(record + kEFlags,
                                    static_cast<u32>(cpu.flags.rflags) |
                                        kEFlagsFixed).is_ok();
        ok = ok && memory.write_u64(record + kRsp, cpu.gpr.rsp).is_ok();
        ok = ok && memory.write_u64(record + kRip, cpu.rip).is_ok();
    }
    if (selected(flags, kContext64Integer)) {
        for (u8 reg = 0; reg < 16 && ok; ++reg) {
            if (reg == 4) continue;  // RSP is CONTEXT_CONTROL's
            ok = memory.write_u64(record + kGpr0 + reg * 8u,
                                  cpu.gpr.get64(reg)).is_ok();
        }
    }
    if (selected(flags, kContext64Segments)) {
        ok = ok && memory.write_u16(record + kSegDs, kUserDataSelector).is_ok();
        ok = ok && memory.write_u16(record + kSegEs, kUserDataSelector).is_ok();
        ok = ok && memory.write_u16(record + kSegFs, kUserFs).is_ok();
        ok = ok && memory.write_u16(record + kSegGs, kUserDataSelector).is_ok();
    }
    if (selected(flags, kContext64FloatingPoint)) {
        u8 image[kFxImageBytes];
        fx_image_save(cpu, image, /*wide=*/false, /*xmm_count=*/16);
        ok = ok && memory.write_u32(record + kMxCsr, cpu.sse.mxcsr).is_ok();
        // Windows writes the legacy x87 area and Xmm0-15 (416 bytes) and
        // leaves FltSave.Reserved4 alone (measured).
        ok = ok && memory.write_bytes(record + kFltSave, image, kFltSaveWritten).is_ok();
    }
    if (selected(flags, kContext64DebugRegisters)) {
        for (u32 off = kDr0; off <= kDr7 && ok; off += 8) {
            ok = memory.write_u64(record + off, 0).is_ok();
        }
        for (u32 i = 0; i < 4 && ok; ++i) {
            ok = memory.write_u64(record + kLastBranchToRip + i * 8, 0).is_ok();
        }
    }
    return ok ? Context64Status::Ok : Context64Status::Fault;
}

Context64Status load_context64(GuestMemory& memory, GuestAddr record,
                               CpuState& cpu, bool restoring_current_thread) {
    const auto raw_flags = memory.read_u32(record + kContext64FlagsOffset);
    if (!raw_flags) return Context64Status::Fault;
    const u32 flags = raw_flags.value() | kContext64Amd64;

    CpuState next = cpu;
    if (selected(flags, kContext64DebugRegisters)) {
        const auto dr7 = memory.read_u64(record + kDr7);
        if (!dr7) return Context64Status::Fault;
        if ((dr7.value() & kDr7EnableBits) != 0) {
            return Context64Status::DebugRegistersUnsupported;
        }
    }
    if (selected(flags, kContext64Integer)) {
        for (u8 reg = 0; reg < 16; ++reg) {
            if (reg == 4) continue;
            const auto value = memory.read_u64(record + kGpr0 + reg * 8u);
            if (!value) return Context64Status::Fault;
            next.gpr.set64(reg, value.value());
        }
    }
    if (selected(flags, kContext64Control)) {
        const auto rsp = memory.read_u64(record + kRsp);
        const auto rip = memory.read_u64(record + kRip);
        const auto eflags = memory.read_u32(record + kEFlags);
        if (!rsp || !rip || !eflags) return Context64Status::Fault;
        next.gpr.rsp = rsp.value();
        next.rip = rip.value();
        next.discard_lazy_flags();
        next.flags.rflags = static_cast<u64>(eflags.value() | 0x2u);
    }
    bool restored_fp = false;
    if (selected(flags, kContext64FloatingPoint)) {
        u8 image[kFxImageBytes];
        const auto mxcsr = memory.read_u32(record + kMxCsr);
        if (!mxcsr ||
            !memory.read_bytes(record + kFltSave, image, sizeof(image)).is_ok()) {
            return Context64Status::Fault;
        }
        fx_image_restore_registers(next, image, /*wide=*/false, /*xmm_count=*/16);
        next.sse.mxcsr = mxcsr.value() & next.sse.mxcsr_mask;
        restored_fp = true;
    }
    // CONTEXT_SEGMENTS is not applied: x64 user-mode selectors are fixed.

    if (restored_fp && restoring_current_thread) {
        math::clear_host_fp_exceptions();
    }
    cpu = next;
    return Context64Status::Ok;
}

} // namespace vitreo::cpu
