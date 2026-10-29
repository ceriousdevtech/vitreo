// SPDX-License-Identifier: LGPL-2.1-only

/// @file fault_report.cpp
/// @brief Human-readable reports for guest faults. See fault_report.hpp.
///
/// Split out of cli/commands/execution_loop.cpp. control_flow_fault_diag_enabled
/// and print_guest_location stay file-local — they are steps of the reports
/// below, not part of the interface.

#include "core/memory/mem_watch.hpp"
#include "runtime/diagnostics/fault_report.hpp"

// The sentinel lives in a shared header precisely so layers that cannot
// include CLI headers still see the value the loop recognises.
#include <vitreo/guest_sentinels.hpp>

// This file was cut from execution_loop.cpp, and it inherited that file's whole
// include block rather than the part it needed. What is left is what it uses.
#include "core/cpu/state/cpu_state.hpp"
#include "core/cpu/interpreter/interpreter.hpp"
#include "core/memory/guest_memory.hpp"
#include "core/cpu/seh/seh32.hpp"
#include "winapi/kernel32/process/process.hpp"
#include "winapi/kernel32/thread/thread.hpp"
#include "winapi/kernel32/module/module.hpp"
#include "winapi/media/winmm/winmm.hpp"
#include <vitreo/diagnostics.hpp>
#include "runtime/diagnostics/pc_trace.hpp"
#include "runtime/diagnostics/hostcall_ring.hpp"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>

namespace vitreo::cli {

[[nodiscard]] bool control_flow_fault_diag_enabled() {
    static const bool enabled =
        std::getenv("VITREO_TRACE_DELEGATE_EXC") != nullptr ||
        std::getenv("VITREO_CONTROL_FLOW_FAULT_DIAG") != nullptr;
    return enabled;
}

void dump_control_flow_fault_context(const cpu::CpuState& cpu,
                                     GuestMemory& memory) {
    if (!control_flow_fault_diag_enabled()) return;

    if (cpu.is_64bit()) {
        std::fprintf(stderr, "[vitreo] Stack qwords at RSP=0x%016llx:",
                     static_cast<unsigned long long>(cpu.gpr.rsp));
        for (u64 index = 0; index < 12; ++index) {
            const GuestAddr address =
                static_cast<GuestAddr>(cpu.gpr.rsp + index * sizeof(u64));
            const auto value = memory.read_u64(address);
            if (!value.is_ok()) {
                std::fprintf(stderr, " <unreadable@+0x%llx>",
                             static_cast<unsigned long long>(index * sizeof(u64)));
                break;
            }
            std::fprintf(stderr, " %016llx",
                         static_cast<unsigned long long>(value.value()));
        }
        std::fprintf(stderr, "\n");

        u64 frame = cpu.gpr.rbp;
        for (unsigned depth = 0; depth < 8 && frame >= 0x10000; ++depth) {
            const auto previous = memory.read_u64(static_cast<GuestAddr>(frame));
            const auto return_address =
                memory.read_u64(static_cast<GuestAddr>(frame + sizeof(u64)));
            if (!previous.is_ok() || !return_address.is_ok()) break;
            std::fprintf(stderr,
                         "[vitreo] Frame[%u] RBP=0x%016llx return=0x%016llx "
                         "previous=0x%016llx\n",
                         depth,
                         static_cast<unsigned long long>(frame),
                         static_cast<unsigned long long>(return_address.value()),
                         static_cast<unsigned long long>(previous.value()));
            const u64 next = previous.value();
            if (next <= frame || next - frame > 16 * 1024 * 1024 ||
                (next & (alignof(u64) - 1)) != 0) {
                break;
            }
            frame = next;
        }
        return;
    }

    const u32 esp = static_cast<u32>(cpu.gpr.rsp);
    std::fprintf(stderr, "[vitreo] Stack dwords at ESP=0x%08x:", esp);
    for (u32 index = 0; index < 16; ++index) {
        const GuestAddr address =
            static_cast<GuestAddr>(esp + index * sizeof(u32));
        const auto value = memory.read_u32(address);
        if (!value.is_ok()) {
            std::fprintf(stderr, " <unreadable@+0x%x>",
                         static_cast<unsigned>(index * sizeof(u32)));
            break;
        }
        std::fprintf(stderr, " %08x", value.value());
    }
    std::fprintf(stderr, "\n");

    u32 frame = static_cast<u32>(cpu.gpr.rbp);
    for (unsigned depth = 0; depth < 8 && frame >= 0x10000; ++depth) {
        const auto previous = memory.read_u32(static_cast<GuestAddr>(frame));
        const auto return_address =
            memory.read_u32(static_cast<GuestAddr>(frame + sizeof(u32)));
        if (!previous.is_ok() || !return_address.is_ok()) break;
        std::fprintf(stderr,
                     "[vitreo] Frame[%u] EBP=0x%08x return=0x%08x "
                     "previous=0x%08x\n",
                     depth, frame, return_address.value(), previous.value());
        const u32 next = previous.value();
        if (next <= frame || next - frame > 16 * 1024 * 1024 ||
            (next & (alignof(u32) - 1)) != 0) {
            break;
        }
        frame = next;
    }
}

void print_guest_location(
    const char* prefix,
    GuestAddr address,
    const winapi::ThunkRegistry& thunk_registry,
    const winapi::ModuleManager* module_manager) {
    if (thunk_registry.is_in_thunk_region(address)) {
        const std::string thunk_name = thunk_registry.resolve_name(address);
        std::fprintf(
            stderr, "%s%s (0x%llx)\n", prefix, thunk_name.c_str(),
            static_cast<unsigned long long>(address));
        return;
    }

    if (module_manager != nullptr) {
        const auto module = module_manager->module_for_address(address);
        if (module.info != nullptr) {
            std::fprintf(
                stderr, "%s%s+0x%x (0x%llx)\n", prefix,
                module.info->name.c_str(), module.rva,
                static_cast<unsigned long long>(address));
            return;
        }
    }

    std::fprintf(
        stderr, "%s0x%llx\n", prefix,
        static_cast<unsigned long long>(address));
}

namespace {

void dump_guest_dwords(const char* label, GuestAddr base, GuestMemory& memory,
                       unsigned count) {
    std::fprintf(stderr, "[vitreo] %s at 0x%08x:", label,
                 static_cast<unsigned>(base));
    if (base < 0x10000) {
        std::fprintf(stderr, " <null-or-low>\n");
        return;
    }
    for (unsigned index = 0; index < count; ++index) {
        const auto value =
            memory.read_u32(static_cast<GuestAddr>(base + index * sizeof(u32)));
        if (value.is_err()) {
            std::fprintf(stderr, " <unreadable@+0x%x>",
                         static_cast<unsigned>(index * sizeof(u32)));
            break;
        }
        std::fprintf(stderr, " +0x%02x=%08x",
                     static_cast<unsigned>(index * sizeof(u32)), value.value());
    }
    std::fprintf(stderr, "\n");
}

}  // namespace

void dump_guest_fault_summary(
    const cpu::CpuState& cpu,
    GuestMemory& memory,
    const winapi::ThunkRegistry& thunk_registry,
    const winapi::ModuleManager* module_manager,
    cpu::CpuException exception) {
    print_guest_location(
        "[vitreo] Guest fault site: ", cpu.rip, thunk_registry,
        module_manager);

    // Callback return sentinel leaking into EDI/ESP is a strong signal that
    // RtlUnwind or a nested callback left nonvolatiles unrestored.
    if (!cpu.is_64bit()) {
        constexpr u32 kCbSentinel32 =
            static_cast<u32>(kCallbackReturnSentinel);
        const u32 edi32 = static_cast<u32>(cpu.gpr.rdi);
        const u32 esp32 = static_cast<u32>(cpu.gpr.rsp);
        if (edi32 == kCbSentinel32 || esp32 == kCbSentinel32 ||
            esp32 == kCbSentinel32 - 4u) {
            std::fprintf(
                stderr,
                "[vitreo] Callback sentinel in GPRs (edi=0x%08x esp=0x%08x) "
                "pending_unwind=%d — likely unrestored nonvolatiles after "
                "RtlUnwind/nested callback\n",
                edi32, esp32, cpu.pending_unwind.active ? 1 : 0);
        }
    }

    if (exception == cpu::CpuException::InvalidOpcode) {
        const auto first = memory.read_u8(cpu.rip);
        const auto second = memory.read_u8(cpu.rip + 1);
        if (first.is_ok() && second.is_ok() &&
            first.value() == 0x0f && second.value() == 0x0b) {
            std::fprintf(
                stderr,
                "[vitreo] Fault instruction: UD2 "
                "(architectural invalid-opcode trap)\n");
        }
    }

    if (cpu.is_64bit()) {
        // An RBP-only walk is not reliable for PE32+.  The x64 exception
        // directory unwinder is the correct future source for this report.
        return;
    }

    // thiscall / COM wrappers leave the object in ECX. A NULL member then
    // faults with EAX=0 and an EBP that is another heap pointer, so the
    // frame walk below prints nothing. Always dump the this-pointer and
    // the raw stack; resolve stack slots that land in a module.
    const u32 ecx = static_cast<u32>(cpu.gpr.rcx);
    dump_guest_dwords("ECX object", static_cast<GuestAddr>(ecx), memory, 16);
    if (ecx >= 0x10000) {
        const auto member = memory.read_u32(static_cast<GuestAddr>(ecx + 0x14));
        if (member.is_ok()) {
            std::fprintf(stderr, "[vitreo] [ecx+0x14]=0x%08x\n", member.value());
            if (member.value() >= 0x10000) {
                dump_guest_dwords("[ecx+0x14] object",
                                  static_cast<GuestAddr>(member.value()),
                                  memory, 8);
            }
        }
    }

    // EAX is where an x86 load most often leaves the pointer it just
    // dereferenced. Dump a window that starts *behind* it: one zeroed field in
    // otherwise plausible data and a wholly wiped page look identical from the
    // faulting address alone, and only the neighbourhood separates them.
    const u32 eax = static_cast<u32>(cpu.gpr.rax);
    if (eax >= 0x10000 + 64) {
        dump_guest_dwords("EAX neighbourhood", static_cast<GuestAddr>((eax & ~3u) - 64),
                          memory, 40);
    }

    // Retrospective: which stores landed near the pointer that faulted. This
    // is the only way to attribute a *missing* write, whose address is not
    // known until the fault happens.
    // Bound the zero run around the pointer: a run that starts and ends on a
    // page boundary means memory was dropped under the guest, while a ragged
    // run means uninitialised data.
    if (eax >= 0x10000) {
        const auto is_zero = [&](u64 a) {
            const auto v = memory.read_u32(static_cast<GuestAddr>(a));
            return v.is_ok() && v.value() == 0;
        };
        u64 probe = static_cast<u64>(eax) & ~3ull;
        if (is_zero(probe)) {
            u64 lo = probe, hi = probe;
            while (lo > 0x10000 && is_zero(lo - 4)) lo -= 4;
            while (hi < 0xfffff000ull && is_zero(hi + 4)) hi += 4;
            hi += 4;
            std::fprintf(stderr,
                "[vitreo] zero run around 0x%08x: [0x%08llx,0x%08llx) size=0x%llx "
                "lo_page_aligned=%d hi_page_aligned=%d\n",
                eax, (unsigned long long)lo, (unsigned long long)hi,
                (unsigned long long)(hi - lo),
                (lo & 0xfff) == 0 ? 1 : 0, (hi & 0xfff) == 0 ? 1 : 0);
        }
    }

    if (store_ring_enabled() && eax >= 0x10000) {
        constexpr u64 kWindow = 0x4000;
        const u64 lo = (eax > kWindow) ? (eax - kWindow) : 0;
        store_ring_dump(lo, static_cast<u64>(eax) + kWindow, 48);
    }

    const u32 ebp = static_cast<u32>(cpu.gpr.rbp);
    const u32 esi = static_cast<u32>(cpu.gpr.rsi);
    const u32 edi = static_cast<u32>(cpu.gpr.rdi);
    const u32 edx = static_cast<u32>(cpu.gpr.rdx);
    // ESI is commonly the this-pointer for an inlined x86 C++ operation.  It
    // may point into the current stack frame, which is precisely where short
    // string and small-vector corruption lives, so do not exclude stack
    // addresses from this diagnostic.
    dump_guest_dwords("ESI object", static_cast<GuestAddr>(esi), memory, 8);
    const u64 stack_pointer = static_cast<u32>(cpu.gpr.rsp);
    const u64 stack_hi =
        std::min<u64>(0xffffffffULL, stack_pointer + 2ULL * 1024 * 1024);
    const auto dump_if_heap = [&](const char* label, u32 addr) {
        if (addr < 0x10000) return;
        if (addr >= stack_pointer && addr < stack_hi) return;
        dump_guest_dwords(label, static_cast<GuestAddr>(addr), memory, 8);
    };
    dump_if_heap("EBP object", ebp);
    dump_if_heap("EDI object", edi);
    dump_if_heap("EDX object", edx);

    std::fprintf(stderr, "[vitreo] Stack dwords at ESP=0x%08x:",
                 static_cast<unsigned>(stack_pointer));
    for (unsigned index = 0; index < 16; ++index) {
        const GuestAddr address = static_cast<GuestAddr>(
            stack_pointer + index * sizeof(u32));
        const auto value = memory.read_u32(address);
        if (value.is_err()) {
            std::fprintf(stderr, " <unreadable@+0x%x>",
                         static_cast<unsigned>(index * sizeof(u32)));
            break;
        }
        std::fprintf(stderr, " %08x", value.value());
    }
    std::fprintf(stderr, "\n");
    for (unsigned index = 0; index < 16; ++index) {
        const auto value = memory.read_u32(static_cast<GuestAddr>(
            stack_pointer + index * sizeof(u32)));
        if (value.is_err() || value.value() < 0x10000) {
            continue;
        }
        const GuestAddr slot = value.value();
        const bool thunk = thunk_registry.is_in_thunk_region(slot);
        const bool in_module =
            module_manager != nullptr &&
            module_manager->module_for_address(slot).info != nullptr;
        if (!thunk && !in_module) {
            continue;
        }
        char prefix[48]{};
        std::snprintf(prefix, sizeof(prefix),
                      "[vitreo]   ESP+0x%02x -> ",
                      static_cast<unsigned>(index * sizeof(u32)));
        print_guest_location(prefix, slot, thunk_registry, module_manager);
    }

    u64 frame = static_cast<u32>(cpu.gpr.rbp);
    const u64 stack_limit =
        std::min<u64>(0xffffffffULL, stack_pointer + 2ULL * 1024 * 1024);
    bool printed_header = false;

    for (unsigned depth = 1; depth <= 16; ++depth) {
        if ((frame & 3u) != 0 || frame < stack_pointer ||
            frame + 8 > stack_limit) {
            break;
        }

        const auto next = memory.read_u32(static_cast<GuestAddr>(frame));
        const auto return_address =
            memory.read_u32(static_cast<GuestAddr>(frame + sizeof(u32)));
        if (next.is_err() || return_address.is_err() ||
            return_address.value() == 0) {
            break;
        }

        if (!printed_header) {
            std::fprintf(
                stderr,
                "[vitreo] Guest call chain "
                "(validated x86 EBP, best effort):\n");
            printed_header = true;
        }
        char prefix[48]{};
        std::snprintf(prefix, sizeof(prefix), "[vitreo]   #%u ", depth);
        print_guest_location(
            prefix, return_address.value(), thunk_registry, module_manager);

        if (next.value() <= frame || next.value() > stack_limit) break;
        frame = next.value();
    }
}

void maybe_sample_worker_pc(const cpu::CpuState& cpu,
                            GuestAddr callback_entry,
                            const winapi::ThunkRegistry& thunk_registry) {
    static const u64 interval = [] {
        const char* value = std::getenv("VITREO_THREAD_SAMPLE_EVERY");
        if (value == nullptr || value[0] == '\0') return u64{0};
        return static_cast<u64>(std::strtoull(value, nullptr, 0));
    }();
    if (interval == 0) return;
    static const GuestAddr callback_filter = [] {
        const char* value = std::getenv("VITREO_THREAD_SAMPLE_CALLBACK");
        if (value == nullptr || value[0] == '\0') return GuestAddr{0};
        return static_cast<GuestAddr>(std::strtoull(value, nullptr, 16));
    }();
    if (callback_filter != 0 && callback_entry != callback_filter) return;

    thread_local u64 block_count = 0;
    ++block_count;
    if ((block_count % interval) != 0) return;

    const std::string thunk_name =
        thunk_registry.is_in_thunk_region(cpu.rip)
            ? thunk_registry.resolve_name(cpu.rip)
            : std::string{};
    if (thunk_name.empty()) {
        std::fprintf(
            stderr,
            "[thread-sample] pid=%d tid=%u blocks=%llu callback=0x%llx "
            "rip=0x%llx\n",
            static_cast<int>(::getpid()),
            winapi::ThreadManager::get_current_thread_id_static(),
            static_cast<unsigned long long>(block_count),
            static_cast<unsigned long long>(callback_entry),
            static_cast<unsigned long long>(cpu.rip));
    } else {
        std::fprintf(
            stderr,
            "[thread-sample] pid=%d tid=%u blocks=%llu callback=0x%llx "
            "rip=0x%llx thunk=%s\n",
            static_cast<int>(::getpid()),
            winapi::ThreadManager::get_current_thread_id_static(),
            static_cast<unsigned long long>(block_count),
            static_cast<unsigned long long>(callback_entry),
            static_cast<unsigned long long>(cpu.rip),
            thunk_name.c_str());
    }
}

}  // namespace vitreo::cli
