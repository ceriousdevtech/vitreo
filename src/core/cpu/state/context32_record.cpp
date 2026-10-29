// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - the x86 (WOW64) Windows CONTEXT record for Get/SetThreadContext

/// @file context32_record.cpp
/// @brief See context32_record.hpp for the measured contract.

#include "core/cpu/state/context32_record.hpp"

#include "core/cpu/state/cpu_state.hpp"
#include "core/cpu/state/fx_image.hpp"
#include "core/memory/guest_memory.hpp"

#include <cstring>

namespace vitreo::cpu {
namespace {

// winnt.h CONTEXT (i386) field offsets.
constexpr u32 kFlags = 0x00;
constexpr u32 kDr0 = 0x04;          // Dr0..Dr3, Dr6, Dr7
constexpr u32 kDr7 = 0x18;
constexpr u32 kFloatSave = 0x1C;    // FLOATING_SAVE_AREA, 112 bytes
constexpr u32 kFloatSaveSize = 112;
constexpr u32 kSegGs = 0x8C;
constexpr u32 kSegFs = 0x90;
constexpr u32 kSegEs = 0x94;
constexpr u32 kSegDs = 0x98;
constexpr u32 kEdi = 0x9C;          // Edi Esi Ebx Edx Ecx Eax
constexpr u32 kEbp = 0xB4;
constexpr u32 kEip = 0xB8;
constexpr u32 kSegCs = 0xBC;
constexpr u32 kEFlags = 0xC0;
constexpr u32 kEsp = 0xC4;
constexpr u32 kSegSs = 0xC8;
constexpr u32 kExtended = 0xCC;     // FXSAVE image, 512 bytes
constexpr u32 kExtendedWritten = 0x120;

// FLOATING_SAVE_AREA offsets (relative to kFloatSave).
constexpr u32 kFsControl = 0x00;
constexpr u32 kFsStatus = 0x04;
constexpr u32 kFsTag = 0x08;
constexpr u32 kFsErrorOffset = 0x0C;
constexpr u32 kFsErrorSelector = 0x10;
constexpr u32 kFsDataOffset = 0x14;
constexpr u32 kFsDataSelector = 0x18;
constexpr u32 kFsRegisters = 0x1C;  // 8 x 10 bytes, ST(0) first

// WOW64 user-mode selectors (measured).
constexpr u32 kUserCs = 0x23;
constexpr u32 kUserData = 0x2B;
constexpr u32 kUserFs = 0x53;

constexpr u32 kEFlagsKept = 0x00210FD5;  // measured SetThreadContext mask
constexpr u32 kEFlagsIf = 0x200;
constexpr u32 kDr7EnableBits = 0xFF;

// Order of the CONTEXT INTEGER fields: edi esi ebx edx ecx eax.
constexpr u8 kIntegerOrder[6] = {7, 6, 3, 2, 1, 0};

bool selected(u32 flags, u32 group) { return (flags & group) == group; }

u16 load16(const u8* p) { u16 v; std::memcpy(&v, p, 2); return v; }
u32 load32(const u8* p) { u32 v; std::memcpy(&v, p, 4); return v; }
void put16(u8* p, u16 v) { std::memcpy(p, &v, 2); }
void put32(u8* p, u32 v) { std::memcpy(p, &v, 4); }

/// The FNSAVE two-bit tag of one register from its FXSAVE abridged bit and
/// value (Intel SDM Vol. 1, x87 tag word / FXSAVE abridged tag).
u32 full_tag(bool valid, const u8* value80) {
    if (!valid) return 3;  // empty
    const u16 exponent = static_cast<u16>(load16(value80 + 8) & 0x7FFF);
    u64 mantissa;
    std::memcpy(&mantissa, value80, 8);
    if (exponent == 0x7FFF) return 2;                // special
    if (exponent == 0) return mantissa == 0 ? 1 : 2;  // zero / denormal
    return (mantissa >> 63) != 0 ? 0 : 2;             // valid / unnormal
}

/// FXSAVE image -> FLOATING_SAVE_AREA.
void image_to_float_save(const u8* image, u8* fs) {
    std::memset(fs, 0, kFloatSaveSize);
    put32(fs + kFsControl, load16(image + 0));
    const u16 status = load16(image + 2);
    put32(fs + kFsStatus, status);
    const u32 top = (status >> 11) & 7u;
    u32 tag = 0;
    for (u32 st = 0; st < 8; ++st) {
        const u32 physical = (top + st) & 7u;
        const bool valid = ((image[4] >> physical) & 1u) != 0;
        tag |= full_tag(valid, image + 32 + st * 16) << (physical * 2);
    }
    put32(fs + kFsTag, tag);
    put32(fs + kFsErrorOffset, load32(image + 8));
    put32(fs + kFsErrorSelector,
          static_cast<u32>(load16(image + 12)) |
              (static_cast<u32>(load16(image + 6) & 0x7FFu) << 16));
    put32(fs + kFsDataOffset, load32(image + 16));
    put32(fs + kFsDataSelector, load16(image + 20));
    for (u32 st = 0; st < 8; ++st) {
        std::memcpy(fs + kFsRegisters + st * 10, image + 32 + st * 16, 10);
    }
}

/// FLOATING_SAVE_AREA -> the x87 part of an FXSAVE image (MXCSR/XMM kept).
void float_save_to_image(const u8* fs, u8* image) {
    put16(image + 0, static_cast<u16>(load32(fs + kFsControl)));
    const u16 status = static_cast<u16>(load32(fs + kFsStatus));
    put16(image + 2, status);
    const u32 tag = load32(fs + kFsTag);
    u8 abridged = 0;
    for (u32 physical = 0; physical < 8; ++physical) {
        if (((tag >> (physical * 2)) & 3u) != 3u) abridged |= static_cast<u8>(1u << physical);
    }
    image[4] = abridged;
    const u32 selector = load32(fs + kFsErrorSelector);
    put16(image + 6, static_cast<u16>((selector >> 16) & 0x7FFu));
    put32(image + 8, load32(fs + kFsErrorOffset));
    put16(image + 12, static_cast<u16>(selector));
    put32(image + 16, load32(fs + kFsDataOffset));
    put16(image + 20, static_cast<u16>(load32(fs + kFsDataSelector)));
    for (u32 st = 0; st < 8; ++st) {
        std::memset(image + 32 + st * 16, 0, 16);
        std::memcpy(image + 32 + st * 16, fs + kFsRegisters + st * 10, 10);
    }
}

} // namespace

Context32Status store_context32(GuestMemory& memory, GuestAddr record,
                                const CpuState& cpu, u32 context_flags) {
    const u32 flags = context_flags | kContext32I386;
    bool ok = memory.write_u32(record + kFlags, flags).is_ok();
    auto w32 = [&](u32 off, u32 v) {
        ok = ok && memory.write_u32(record + off, v).is_ok();
    };
    if (selected(flags, kContext32DebugRegisters)) {
        for (u32 off = kDr0; off <= kDr7; off += 4) w32(off, 0);
    }
    if (selected(flags, kContext32FloatingPoint) ||
        selected(flags, kContext32ExtendedRegisters)) {
        u8 image[kFxImageBytes];
        fx_image_save(cpu, image, /*wide=*/false, /*xmm_count=*/8);
        if (selected(flags, kContext32FloatingPoint)) {
            u8 fs[kFloatSaveSize];
            image_to_float_save(image, fs);
            ok = ok && memory.write_bytes(record + kFloatSave, fs, sizeof(fs)).is_ok();
        }
        if (selected(flags, kContext32ExtendedRegisters)) {
            ok = ok && memory.write_bytes(record + kExtended, image,
                                          kExtendedWritten).is_ok();
        }
    }
    if (selected(flags, kContext32Segments)) {
        w32(kSegGs, kUserData);
        w32(kSegFs, kUserFs);
        w32(kSegEs, kUserData);
        w32(kSegDs, kUserData);
    }
    if (selected(flags, kContext32Integer)) {
        for (u32 i = 0; i < 6; ++i) w32(kEdi + i * 4, cpu.gpr.get32(kIntegerOrder[i]));
    }
    if (selected(flags, kContext32Control)) {
        w32(kEbp, cpu.gpr.get32(5));
        w32(kEip, static_cast<u32>(cpu.rip));
        w32(kSegCs, kUserCs);
        w32(kEFlags, static_cast<u32>(cpu.flags.rflags) | kEFlagsIf);
        w32(kEsp, cpu.gpr.get32(4));
        w32(kSegSs, kUserData);
    }
    return ok ? Context32Status::Ok : Context32Status::Fault;
}

Context32Status load_context32(GuestMemory& memory, GuestAddr record,
                               CpuState& cpu) {
    const auto raw_flags = memory.read_u32(record + kFlags);
    if (!raw_flags) return Context32Status::Fault;
    const u32 flags = raw_flags.value() | kContext32I386;
    auto r32 = [&](u32 off, u32& out) {
        const auto v = memory.read_u32(record + off);
        if (!v) return false;
        out = v.value();
        return true;
    };

    CpuState next = cpu;
    if (selected(flags, kContext32DebugRegisters)) {
        u32 dr7 = 0;
        if (!r32(kDr7, dr7)) return Context32Status::Fault;
        if ((dr7 & kDr7EnableBits) != 0) {
            return Context32Status::DebugRegistersUnsupported;
        }
    }
    if (selected(flags, kContext32Integer)) {
        for (u32 i = 0; i < 6; ++i) {
            u32 v = 0;
            if (!r32(kEdi + i * 4, v)) return Context32Status::Fault;
            next.gpr.set32(kIntegerOrder[i], v);
        }
    }
    if (selected(flags, kContext32Control)) {
        u32 ebp = 0, eip = 0, eflags = 0, esp = 0;
        if (!r32(kEbp, ebp) || !r32(kEip, eip) || !r32(kEFlags, eflags) ||
            !r32(kEsp, esp)) {
            return Context32Status::Fault;
        }
        next.gpr.set32(5, ebp);
        next.gpr.set32(4, esp);
        next.rip = eip;
        next.discard_lazy_flags();
        next.flags.rflags = (eflags & kEFlagsKept) | kEFlagsIf;
    }
    const bool fp = selected(flags, kContext32FloatingPoint);
    const bool ext = selected(flags, kContext32ExtendedRegisters);
    if (fp || ext) {
        u8 image[kFxImageBytes];
        fx_image_save(next, image, /*wide=*/false, /*xmm_count=*/8);
        if (ext) {
            // Only the part Windows reads (the header and XMM0-7); the rest
            // of the caller's 512 bytes is not register state.
            if (!memory.read_bytes(record + kExtended, image, kExtendedWritten).is_ok()) {
                return Context32Status::Fault;
            }
        }
        if (fp) {
            u8 fs[kFloatSaveSize];
            if (!memory.read_bytes(record + kFloatSave, fs, sizeof(fs)).is_ok()) {
                return Context32Status::Fault;
            }
            float_save_to_image(fs, image);  // x87 from FloatSave wins
        }
        const u32 mxcsr_mask = next.sse.mxcsr_mask;
        fx_image_restore_registers(next, image, /*wide=*/false, /*xmm_count=*/8);
        next.sse.mxcsr &= mxcsr_mask;
    }
    // CONTEXT_SEGMENTS is not applied: WOW64 user selectors are fixed.
    cpu = next;
    return Context32Status::Ok;
}

} // namespace vitreo::cpu
