// SPDX-License-Identifier: LGPL-2.1-only

/// @file restore_context64.cpp
/// @brief x64 RtlRestoreContext, including STATUS_LONGJUMP and
///        STATUS_UNWIND_CONSOLIDATE (frame consolidation).
///
/// Microsoft Learn, RtlRestoreContext: with STATUS_UNWIND_CONSOLIDATE the
/// ExceptionInformation holds a callback "such as a catch handler". The frames
/// between RtlRestoreContext and the target context are consolidated before
/// the callback runs, hiding them from exception handling while their stack
/// data -- the throw object -- stays present. The callback returns the new
/// program counter. MSVC's vcruntime runs every x64 catch block this way:
/// __CxxFrameHandler3/4 call RtlUnwindEx with such a record and pass the
/// catching frame's call site as TargetIp, so skipping the callback resumes
/// at that call site with the catch body never run.
///
/// The consolidated frame is ordinary guest code with ordinary unwind data,
/// registered like a JIT's (RtlAddFunctionTable), so every unwinder -- ours,
/// the guest's RtlVirtualUnwind, a guest's own walker -- passes from the
/// callback's frame straight to the target frame:
///
///   F-0x20  callback home space           <- RSP throughout the trampoline
///   F       CONTEXT copy (0x4D0 bytes)       target nonvolatiles, XMM6-15
///   F+0x4D0 machine frame {RIP, CS, EFLAGS, RSP, SS} = target RIP / RSP
///
/// Its unwind codes restore the nonvolatiles and XMM6-15 from the copy and
/// then pop the machine frame (Learn, "x64 exception handling").

#include "winapi/kernel32/thread/restore_context64.hpp"

#include "winapi/kernel32/thread/context64.hpp"
#include "winapi/kernel32/module/module.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/core/thunk_registry.hpp"
#include "core/cpu/seh/seh32.hpp"
#include "core/cpu/state/cpu_state.hpp"
#include "core/memory/guest_memory.hpp"

#include <array>
#include <cstdio>
#include <mutex>
#include <vector>

namespace vitreo::winapi {
namespace {

// winnt.h AMD64 CONTEXT.
constexpr u32 kCtxSize = 0x4D0;
constexpr u32 kCtxFlags = 0x30;
constexpr u32 kCtxMxCsr = 0x34;
constexpr u32 kCtxEFlags = 0x44;
constexpr u32 kCtxRbx = 0x90;
constexpr u32 kCtxRsp = 0x98;
constexpr u32 kCtxRbp = 0xA0;
constexpr u32 kCtxRsi = 0xA8;
constexpr u32 kCtxRdi = 0xB0;
constexpr u32 kCtxR12 = 0xD8;
constexpr u32 kCtxR13 = 0xE0;
constexpr u32 kCtxR14 = 0xE8;
constexpr u32 kCtxR15 = 0xF0;
constexpr u32 kCtxRip = 0xF8;
constexpr u32 kCtxFltControlWord = 0x100;
constexpr u32 kCtxFltMxCsr = 0x118;
constexpr u32 kCtxXmm0 = 0x1A0;
constexpr u32 kContextFull = 0x0010000B;  // AMD64 | CONTROL | INTEGER | FLOATING_POINT

// MSVC setjmp.h x64 _JUMP_BUFFER.
constexpr u32 kJbRbx = 0x08;
constexpr u32 kJbRsp = 0x10;
constexpr u32 kJbRbp = 0x18;
constexpr u32 kJbRsi = 0x20;
constexpr u32 kJbRdi = 0x28;
constexpr u32 kJbR12 = 0x30;
constexpr u32 kJbRip = 0x50;
constexpr u32 kJbMxCsr = 0x58;
constexpr u32 kJbFpCsr = 0x5C;
constexpr u32 kJbXmm6 = 0x60;

// EXCEPTION_RECORD64.
constexpr u32 kErecCode = 0x00;
constexpr u32 kErecNumberParameters = 0x18;
constexpr u32 kErecInformation = 0x20;

// Consolidation frame and trampoline page.
constexpr u32 kHomeSpace = 0x20;
constexpr u32 kMachineFrameSize = 40;
constexpr u32 kCeilingMargin = 0x40;
constexpr GuestAddr kPageSize = 0x1000;
constexpr GuestAddr kTrampolineSlot = 0xF0000;  // free slot in the synthetic arena
constexpr u32 kRuntimeFunctionOffset = 0x100;
constexpr u32 kUnwindInfoOffset = 0x110;

constexpr u8 UWOP_ALLOC_LARGE = 1;
constexpr u8 UWOP_SAVE_NONVOL = 4;
constexpr u8 UWOP_SAVE_XMM128 = 8;
constexpr u8 UWOP_PUSH_MACHFRAME = 10;

[[nodiscard]] u16 code(u8 offset, u8 op, u8 info) {
    return static_cast<u16>(offset | (static_cast<u16>(op | (info << 4)) << 8));
}

/// Builds the trampoline page once per process. Returns its entry point, or 0
/// when the page cannot be mapped or registered.
[[nodiscard]] GuestAddr consolidation_trampoline(GuestMemory& memory,
                                                 ModuleManager& modules) {
    static std::mutex mutex;
    static GuestAddr entry = 0;
    std::lock_guard<std::mutex> lock(mutex);
    if (entry != 0) return entry;

    const auto nt_continue =
        modules.registry().get_thunk_address("ntdll.dll", "NtContinue");
    if (nt_continue.is_err()) {
        std::fprintf(stderr,
            "[seh64] frame consolidation: ntdll!NtContinue is not registered\n");
        return 0;
    }
    const GuestAddr page = modules.synthetic_module_arena() + kTrampolineSlot;

    // Entered with RSP = F - 0x20 already (the callback's home space), so the
    // function has no prolog and one set of unwind codes is exact at every
    // instruction.
    // T0: call rax                ; rcx = ExceptionRecord, rax = callback
    //     mov [rsp+0x118],rax     ; CONTEXT.Rip (0x20 + 0xF8) = continuation
    //     lea rcx,[rsp+0x20]      ; the CONTEXT copy
    //     xor edx,edx             ; TestAlert = FALSE
    //     mov rax,NtContinue
    //     call rax                ; does not return
    //     int3
    std::vector<u8> text = {
        0xFF, 0xD0,
        0x48, 0x89, 0x84, 0x24, 0x18, 0x01, 0x00, 0x00,
        0x48, 0x8D, 0x4C, 0x24, 0x20,
        0x31, 0xD2,
        0x48, 0xB8,
    };
    const u64 target = nt_continue.value();
    for (int i = 0; i < 8; ++i) text.push_back(static_cast<u8>(target >> (8 * i)));
    text.insert(text.end(), {0xFF, 0xD0, 0xCC});
    constexpr u8 kPrologSize = 0;

    // Unwind codes. Offsets are relative to the fixed-allocation base, which
    // is RSP throughout: F - 0x20.
    std::vector<u16> codes;
    const std::array<std::pair<u8, u32>, 8> nonvolatile = {{
        {3, kCtxRbx}, {5, kCtxRbp}, {6, kCtxRsi}, {7, kCtxRdi},
        {12, kCtxR12}, {13, kCtxR13}, {14, kCtxR14}, {15, kCtxR15},
    }};
    for (const auto& [reg, offset] : nonvolatile) {
        codes.push_back(code(0, UWOP_SAVE_NONVOL, reg));
        codes.push_back(static_cast<u16>((kHomeSpace + offset) / 8));
    }
    for (u8 reg = 6; reg < 16; ++reg) {
        codes.push_back(code(0, UWOP_SAVE_XMM128, reg));
        codes.push_back(static_cast<u16>((kHomeSpace + kCtxXmm0 + reg * 16u) / 16));
    }
    codes.push_back(code(0, UWOP_ALLOC_LARGE, 0));
    codes.push_back(static_cast<u16>((kHomeSpace + kCtxSize) / 8));
    codes.push_back(code(0, UWOP_PUSH_MACHFRAME, 0));
    // CountOfCodes excludes the alignment slot Learn requires after an odd
    // count; counting it would replay a spurious PUSH_NONVOL RAX.
    const u8 count_of_codes = static_cast<u8>(codes.size());
    if (codes.size() % 2 != 0) codes.push_back(0);

    std::vector<u8> image(kPageSize, 0xCC);
    std::copy(text.begin(), text.end(), image.begin());
    auto put32 = [&](u32 at, u32 value) {
        for (u32 i = 0; i < 4; ++i) image[at + i] = static_cast<u8>(value >> (8 * i));
    };
    put32(kRuntimeFunctionOffset + 0, 0);
    put32(kRuntimeFunctionOffset + 4, static_cast<u32>(text.size()));
    put32(kRuntimeFunctionOffset + 8, kUnwindInfoOffset);
    image[kUnwindInfoOffset + 0] = 0x01;  // version 1, no handler
    image[kUnwindInfoOffset + 1] = kPrologSize;
    image[kUnwindInfoOffset + 2] = count_of_codes;
    image[kUnwindInfoOffset + 3] = 0;     // no frame register
    for (usize i = 0; i < codes.size(); ++i) {
        image[kUnwindInfoOffset + 4 + i * 2] = static_cast<u8>(codes[i]);
        image[kUnwindInfoOffset + 5 + i * 2] = static_cast<u8>(codes[i] >> 8);
    }

    if (memory.map_region(page, kPageSize, MemoryProtection::ReadWrite).is_err() ||
        memory.write_bytes(page, image.data(), image.size()).is_err() ||
        memory.protect_region(page, kPageSize, MemoryProtection::ReadExecute).is_err()) {
        std::fprintf(stderr,
            "[seh64] frame consolidation: cannot map trampoline at 0x%llx\n",
            static_cast<unsigned long long>(page));
        return 0;
    }
    if (!modules.add_dynamic_function_table(page + kRuntimeFunctionOffset, 1, page)) {
        std::fprintf(stderr,
            "[seh64] frame consolidation: cannot register trampoline unwind data\n");
        return 0;
    }
    entry = page;
    return entry;
}

[[nodiscard]] bool copy_u64(GuestMemory& memory, GuestAddr to, GuestAddr from) {
    auto value = memory.read_u64(from);
    return value.is_ok() && memory.write_u64(to, value.value()).is_ok();
}

/// STATUS_LONGJUMP: the jump buffer's non-volatile state replaces the
/// context's (Learn, RtlRestoreContext).
[[nodiscard]] bool apply_jump_buffer(GuestMemory& memory, GuestAddr ctx,
                                     GuestAddr jb) {
    bool ok = copy_u64(memory, ctx + kCtxRbx, jb + kJbRbx) &&
              copy_u64(memory, ctx + kCtxRsp, jb + kJbRsp) &&
              copy_u64(memory, ctx + kCtxRbp, jb + kJbRbp) &&
              copy_u64(memory, ctx + kCtxRsi, jb + kJbRsi) &&
              copy_u64(memory, ctx + kCtxRdi, jb + kJbRdi) &&
              copy_u64(memory, ctx + kCtxRip, jb + kJbRip);
    for (u32 i = 0; ok && i < 4; ++i) {  // R12..R15
        ok = copy_u64(memory, ctx + kCtxR12 + i * 8, jb + kJbR12 + i * 8);
    }
    for (u32 i = 0; ok && i < 10; ++i) {  // XMM6..XMM15
        const GuestAddr to = ctx + kCtxXmm0 + (6 + i) * 16;
        const GuestAddr from = jb + kJbXmm6 + i * 16;
        ok = copy_u64(memory, to, from) && copy_u64(memory, to + 8, from + 8);
    }
    auto mxcsr = memory.read_u32(jb + kJbMxCsr);
    auto fpcsr = memory.read_u16(jb + kJbFpCsr);
    ok = ok && mxcsr.is_ok() && fpcsr.is_ok() &&
         memory.write_u32(ctx + kCtxMxCsr, mxcsr.value()).is_ok() &&
         memory.write_u32(ctx + kCtxFltMxCsr, mxcsr.value()).is_ok() &&
         memory.write_u16(ctx + kCtxFltControlWord, fpcsr.value()).is_ok();
    return ok;
}

} // namespace

bool restore_context64_with_record(Marshaler& m, ModuleManager& modules,
                                   GuestAddr ctx_addr,
                                   GuestAddr exception_record,
                                   u64 stack_ceiling) {
    auto* cpu = m.cpu();
    if (cpu == nullptr || ctx_addr == 0) return false;
    GuestMemory& memory = m.memory();

    u32 code = 0;
    if (exception_record != 0) {
        auto read = memory.read_u32(exception_record + kErecCode);
        if (read.is_err()) return false;
        code = read.value();
    }

    if (code == STATUS_LONGJUMP_CODE) {
        auto jb = memory.read_u64(exception_record + kErecInformation);
        if (jb.is_err() || !apply_jump_buffer(memory, ctx_addr, jb.value())) {
            return false;
        }
    }

    if (code != STATUS_UNWIND_CONSOLIDATE_CODE) {
        return restore_context64(m, ctx_addr);
    }

    const auto nparams = memory.read_u32(exception_record + kErecNumberParameters);
    const auto callback = memory.read_u64(exception_record + kErecInformation);
    if (nparams.is_err() || nparams.value() < 1 || callback.is_err() ||
        callback.value() == 0) {
        return false;
    }
    const GuestAddr trampoline = consolidation_trampoline(memory, modules);
    if (trampoline == 0) return false;

    // Build the frame below everything still live.
    const u64 frame_bytes = kCtxSize + kMachineFrameSize;
    const u64 frame = (stack_ceiling - kCeilingMargin - frame_bytes) & ~u64{0xF};
    std::vector<u8> context(kCtxSize);
    if (memory.read_bytes(ctx_addr, context.data(), context.size()).is_err() ||
        memory.write_bytes(frame, context.data(), context.size()).is_err()) {
        return false;
    }
    const u32 flags = memory.read_u32(ctx_addr + kCtxFlags).value_or(0);
    const u64 target_rip = memory.read_u64(ctx_addr + kCtxRip).value_or(0);
    const u64 target_rsp = memory.read_u64(ctx_addr + kCtxRsp).value_or(0);
    const u32 eflags = memory.read_u32(ctx_addr + kCtxEFlags).value_or(0x202);
    const GuestAddr machine = frame + kCtxSize;
    if (memory.write_u32(frame + kCtxFlags, flags | kContextFull).is_err() ||
        memory.write_u64(machine + 0, target_rip).is_err() ||
        memory.write_u64(machine + 8, 0x33).is_err() ||
        memory.write_u64(machine + 16, eflags).is_err() ||
        memory.write_u64(machine + 24, target_rsp).is_err() ||
        memory.write_u64(machine + 32, 0x2B).is_err()) {
        return false;
    }

    // The callback starts with the target's registers, like any frame
    // restored from that context, then gets the consolidation entry state.
    if (!restore_context64(m, frame)) return false;
    cpu->gpr.rcx = exception_record;
    cpu->gpr.rax = callback.value();
    cpu->set_sp(frame - kHomeSpace);
    cpu->rip = trampoline;
    return true;
}

void rtl_restore_context(Marshaler& m, ModuleManager* modules) {
    auto* cpu = m.cpu();
    if (cpu == nullptr) return;
    const GuestAddr context = m.arg_ptr(0);
    const GuestAddr record = m.arg_ptr(1);
    bool restored = false;
    if (m.cpu_is_64bit()) {
        restored = modules != nullptr &&
                   restore_context64_with_record(m, *modules, context, record,
                                                 cpu->gpr.rsp);
    } else if (record == 0) {
        // x86 has no jump-buffer or consolidation form to apply.
        restored = context != 0 &&
                   cpu::read_context_32(m.memory(), static_cast<u32>(context),
                                        *cpu).is_ok();
    } else {
        std::fprintf(stderr,
            "[vitreo] UNIMPLEMENTED ntdll!RtlRestoreContext: x86 caller with "
            "an ExceptionRecord\n");
    }
    if (restored) {
        cpu->thunk_control_transfer = true;
        return;
    }
    cpu->pending_raise.active = true;
    cpu->pending_raise.from_helper = false;
    cpu->pending_raise.code = cpu::STATUS_ACCESS_VIOLATION;
    cpu->pending_raise.flags = 1;  // EXCEPTION_NONCONTINUABLE
    cpu->pending_raise.num_args = 2;
    cpu->pending_raise.args[0] = 0;  // read
    cpu->pending_raise.args[1] = context;
}

} // namespace vitreo::winapi
