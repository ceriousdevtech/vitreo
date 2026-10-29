// SPDX-License-Identifier: LGPL-2.1-only

/// @file x64_exception_bridge.cpp
/// @brief Mach exception port handler for native 32-bit guest faults.

#include "core/cpu/x64/x64_exception_bridge.hpp"

#include "core/cpu/x64/native64.hpp"
#include "core/memory/write_watch_traps.hpp"
#include "core/cpu/x64/cpuid_intercept.hpp"
#include "core/cpu/cpuid_synth.hpp"

#include <cstddef>
#include <cstdio>
#include <cstdlib>

#include <mach/mach.h>
#include <mach-o/dyld.h>
#include <mach-o/getsect.h>
#include <mach/ndr.h>
#include <mach/thread_status.h>

#include <atomic>
#include <cstring>
#include <mutex>
#include <pthread.h>
#include <sched.h>
#include <unordered_map>
#include <mach/thread_info.h>

namespace vitreo::cpu::x64 {

namespace {

std::mutex                                      g_exec_mu;
std::unordered_map<u64, Compat32Executor*>      g_exec_by_tid;
std::atomic<u16>                                g_guest_cs{0};
struct Native64ThreadInfo {
    u64 teb{0};
    u16 fs{0};
};
std::mutex                                      g_native64_mutex;
std::unordered_map<u32, Native64ThreadInfo>     g_native64_threads;

/// Registration of a native64 guest thread, or {0,0} if not one.
[[nodiscard]] Native64ThreadInfo native64_info_for(u32 mach_port) noexcept {
    std::lock_guard<std::mutex> lock(g_native64_mutex);
    const auto it = g_native64_threads.find(mach_port);
    return it == g_native64_threads.end() ? Native64ThreadInfo{} : it->second;
}

[[nodiscard]] u64 native64_teb_for(u32 mach_port) noexcept {
    return native64_info_for(mach_port).teb;
}

// Both defined below, next to the state they read.
[[nodiscard]] u32 ntstatus_for(exception_type_t exception,
                               int64_t code0) noexcept;
[[nodiscard]] bool rip_in_host_text(u64 rip) noexcept;

const bool s_trace_native64_faults =
    std::getenv("VITREO_X64_EXC_TRACE") != nullptr;

/// Hand a faulting native64 guest thread back to its servicer.
///
/// The faulting instruction never runs a stub, so the register file is
/// captured here from the thread's Mach state and written straight into the
/// transfer block. The thread is then redirected to `native64_fault_return`,
/// which only has to unwind to the host frame.
///
/// Resuming in place IS possible here, unlike compat32 -- the guest runs in
/// long mode exactly as the host does, so thread_set_state works.
[[nodiscard]] bool redirect_native64_fault(mach_port_t thread,
                                           const Native64ThreadInfo& info,
                                           exception_type_t exception,
                                           i64 code0, i64 code1) {
    x86_thread_state64_t st{};
    mach_msg_type_number_t n = x86_THREAD_STATE64_COUNT;
    if (thread_get_state(thread, x86_THREAD_STATE64,
                         reinterpret_cast<thread_state_t>(&st),
                         &n) != KERN_SUCCESS) {
        return false;
    }

    // Only GUEST code faults are the guest's to handle. A fault in Vitreo's
    // own text happens to be on a guest thread, but it is a Vitreo bug, and
    // handing it to the guest's SEH would dress it up as an access violation
    // the guest never caused -- and resume through a HOSTRSP that belongs to a
    // frame the fault never reached.
    if (rip_in_host_text(st.__rip)) return false;

    // ntdll's `int 2Eh` service entry. It is a guest fault only in the sense
    // that the host CPU had nowhere to send it: the instruction's contract is
    // to enter Vitreo's implementation of that service, not to raise an
    // exception. Reproduce exactly what a thunk stub's `call fs:[collect]`
    // does -- push the slot's dispatch gateway, which is the thunk's identity,
    // and continue in the shared collector -- so the service returns to the
    // veneer's caller with its NTSTATUS in RAX.
    u64 gateway = native64_dispatch_alias_gateway(st.__rip);
    if (gateway == 0) {
        u8 opcode[2]{};
        vm_size_t got = 0;
        if (vm_read_overwrite(
                mach_task_self(), static_cast<vm_address_t>(st.__rip),
                sizeof(opcode), reinterpret_cast<vm_address_t>(opcode),
                &got) == KERN_SUCCESS &&
            got == sizeof(opcode) &&
            ((opcode[0] == 0xCD && opcode[1] == 0x2E) ||
             (opcode[0] == 0x0F && opcode[1] == 0x05))) {
            gateway = native64_service_gateway(
                static_cast<u32>(st.__rax));
        }
    }
    if (gateway != 0) {
        const u64 rsp = st.__rsp - 8;
        // Identity mapping: the guest stack is host-addressable here.
        std::memcpy(reinterpret_cast<void*>(rsp), &gateway, sizeof(gateway));
        st.__rsp = rsp;
        st.__rip = collect_address();
        if (thread_set_state(thread, x86_THREAD_STATE64,
                             reinterpret_cast<thread_state_t>(&st),
                             x86_THREAD_STATE64_COUNT) != KERN_SUCCESS) {
            return false;
        }
        return true;
    }

    auto* x = reinterpret_cast<Native64Transfer*>(
        static_cast<u8*>(reinterpret_cast<void*>(info.teb)) + kN64BlockBegin);

    x->rax = st.__rax; x->rcx = st.__rcx; x->rdx = st.__rdx; x->rbx = st.__rbx;
    x->rsp = st.__rsp; x->rbp = st.__rbp; x->rsi = st.__rsi; x->rdi = st.__rdi;
    x->r8  = st.__r8;  x->r9  = st.__r9;  x->r10 = st.__r10; x->r11 = st.__r11;
    x->r12 = st.__r12; x->r13 = st.__r13; x->r14 = st.__r14; x->r15 = st.__r15;
    x->rip = st.__rip;
    x->flags = st.__rflags;
    x->fault_status  = ntstatus_for(exception, code0);
    x->fault_address = static_cast<u64>(code1);
    // Mach does not report direction; the SEH record's read/write flag is
    // reported as a read rather than guessed.
    x->fault_write = 0;

    // The guest's FP/SSE file, so a handler that continues execution resumes
    // with what the faulting instruction had.
    {
        x86_float_state64_t f{};
        mach_msg_type_number_t fc = x86_FLOAT_STATE64_COUNT;
        if (thread_get_state(thread, x86_FLOAT_STATE64,
                             reinterpret_cast<thread_state_t>(&f),
                             &fc) == KERN_SUCCESS) {
            std::memcpy(reinterpret_cast<u8*>(info.teb) + kN64FxSave,
                        &f.__fpu_fcw, kN64FxSaveSize);
        }
    }

    // %di carries the GS selector: an exception can reset GS, and everything
    // the stub touches is GS-relative.
    if (s_trace_native64_faults) {
        std::fprintf(stderr,
                     "[exc] native64 fault redirect: thread=%u teb=0x%llx "
                     "fs=0x%04x guest_rip=0x%llx status=0x%08x "
                     "fault_addr=0x%llx rax=0x%llx rcx=0x%llx "
                     "rdx=0x%llx rsp=0x%llx\n",
                     static_cast<unsigned>(thread),
                     static_cast<unsigned long long>(info.teb),
                     static_cast<unsigned>(info.fs),
                     static_cast<unsigned long long>(st.__rip),
                     static_cast<unsigned>(x->fault_status),
                     static_cast<unsigned long long>(x->fault_address),
                     static_cast<unsigned long long>(st.__rax),
                     static_cast<unsigned long long>(st.__rcx),
                     static_cast<unsigned long long>(st.__rdx),
                     static_cast<unsigned long long>(st.__rsp));
        u64 stack_words[8]{};
        vm_size_t stack_bytes = 0;
        if (vm_read_overwrite(
                mach_task_self(), static_cast<vm_address_t>(st.__rsp),
                sizeof(stack_words),
                reinterpret_cast<vm_address_t>(stack_words),
                &stack_bytes) == KERN_SUCCESS &&
            stack_bytes == sizeof(stack_words)) {
            std::fprintf(
                stderr,
                "[exc] native64 stack: +00=0x%llx +08=0x%llx "
                "+10=0x%llx +18=0x%llx +20=0x%llx +28=0x%llx "
                "+30=0x%llx +38=0x%llx\n",
                static_cast<unsigned long long>(stack_words[0]),
                static_cast<unsigned long long>(stack_words[1]),
                static_cast<unsigned long long>(stack_words[2]),
                static_cast<unsigned long long>(stack_words[3]),
                static_cast<unsigned long long>(stack_words[4]),
                static_cast<unsigned long long>(stack_words[5]),
                static_cast<unsigned long long>(stack_words[6]),
                static_cast<unsigned long long>(stack_words[7]));
        }
        if (st.__rip < 0x1000) {
            u64 return_address = 0;
            vm_size_t got = 0;
            const kern_return_t read_kr = vm_read_overwrite(
                mach_task_self(), static_cast<vm_address_t>(st.__rsp),
                sizeof(return_address),
                reinterpret_cast<vm_address_t>(&return_address), &got);
            std::fprintf(stderr,
                         "[exc]   null target rsp=0x%llx [rsp]=0x%llx "
                         "rax=0x%llx rcx=0x%llx rdx=0x%llx r8=0x%llx "
                         "r9=0x%llx (read=%d/%llu)\n",
                         static_cast<unsigned long long>(st.__rsp),
                         static_cast<unsigned long long>(return_address),
                         static_cast<unsigned long long>(st.__rax),
                         static_cast<unsigned long long>(st.__rcx),
                         static_cast<unsigned long long>(st.__rdx),
                         static_cast<unsigned long long>(st.__r8),
                         static_cast<unsigned long long>(st.__r9), read_kr,
                         static_cast<unsigned long long>(got));
        }
    }
    if (info.fs == 0) {
        std::fprintf(stderr,
                     "[exc] native64 thread %u registered with a null FS "
                     "selector; cannot redirect\n",
                     static_cast<unsigned>(thread));
        return false;
    }

    st.__rdi = info.fs;
    st.__rip = fault_return_address();
    return thread_set_state(thread, x86_THREAD_STATE64,
                            reinterpret_cast<thread_state_t>(&st),
                            x86_THREAD_STATE64_COUNT) == KERN_SUCCESS;
}
std::atomic<bool>              g_started{false};
std::atomic<bool>              g_handler_ready{false};
mach_port_t                    g_port = MACH_PORT_NULL;
pthread_t                      g_thread{};

// EXCEPTION_DEFAULT | MACH_EXCEPTION_CODES request. Two port descriptors
// (thread, task) then the exception identity and 64-bit codes.
//
// MIG declares its request and reply structs under `#pragma pack(4)`, and that
// packing is the wire format, not a detail. Without it the compiler aligns
// `code` to 8 and moves it 4 bytes past where the kernel wrote it: code[0]
// then reads as (code0_hi | code1_lo << 32) -- a huge number that matches no
// kern_return_t -- and code[1] reads the message trailer, which is why the
// faulting address in every guest ACCESS_VIOLATION used to be reported as 0.
#pragma pack(push, 4)
struct ExceptionRequest {
    mach_msg_header_t Head;
    mach_msg_body_t msgh_body;
    mach_msg_port_descriptor_t thread;
    mach_msg_port_descriptor_t task;
    NDR_record_t NDR;
    exception_type_t exception;
    mach_msg_type_number_t codeCnt;
    int64_t code[2];
};

struct ExceptionReply {
    mach_msg_header_t Head;
    NDR_record_t NDR;
    kern_return_t RetCode;
};
#pragma pack(pop)

static_assert(offsetof(ExceptionRequest, codeCnt) + sizeof(mach_msg_type_number_t)
                  == offsetof(ExceptionRequest, code),
              "MACH_EXCEPTION_CODES payload must follow codeCnt with no padding");

constexpr u32 kStatusAccessViolation       = 0xC0000005u;
constexpr u32 kStatusIllegalInstruction    = 0xC000001Du;
constexpr u32 kStatusIntegerDivideByZero   = 0xC0000094u;
constexpr u32 kStatusIntegerOverflow       = 0xC0000095u;
constexpr u32 kStatusBreakpoint            = 0x80000003u;

u32 ntstatus_for(exception_type_t exception, int64_t code0) noexcept {
    switch (exception) {
    case EXC_BAD_ACCESS:
        return kStatusAccessViolation;
    case EXC_BAD_INSTRUCTION:
        return kStatusIllegalInstruction;
    case EXC_ARITHMETIC:
        // EXC_I386_DIV = 1 (integer divide by zero)
        if (code0 == 1) return kStatusIntegerDivideByZero;
        return kStatusIntegerOverflow;
    case EXC_BREAKPOINT:
        return kStatusBreakpoint;
    default:
        return kStatusAccessViolation;
    }
}

u64 current_thread_id() noexcept {
    u64 tid = 0;
    pthread_threadid_np(nullptr, &tid);
    return tid;
}

u64 mach_thread_id(mach_port_t thread) noexcept {
    thread_identifier_info_data_t info{};
    mach_msg_type_number_t count = THREAD_IDENTIFIER_INFO_COUNT;
    const kern_return_t kr = thread_info(
        thread, THREAD_IDENTIFIER_INFO,
        reinterpret_cast<thread_info_t>(&info), &count);
    if (kr != KERN_SUCCESS) return 0;
    return info.thread_id;
}

Compat32Executor* executor_for_mach_thread(mach_port_t thread) noexcept {
    const u64 tid = mach_thread_id(thread);
    if (tid == 0) return nullptr;
    std::lock_guard<std::mutex> lk(g_exec_mu);
    auto it = g_exec_by_tid.find(tid);
    return it == g_exec_by_tid.end() ? nullptr : it->second;
}

/// Bounds of this executable's __TEXT, resolved once.
///
/// Fault classification cannot use "below 4GB is guest": that holds for a
/// 32-bit guest, but a Win64 image loads at 0x140000000 and its whole
/// execution range sits above the line. The durable discriminator is the
/// other way round -- Vitreo's own text is one known range, and every other
/// executable address in the process belongs to the guest.
[[nodiscard]] bool rip_in_host_text(u64 rip) noexcept {
    static const struct Range {
        u64 lo{0};
        u64 hi{0};
        Range() {
            unsigned long size = 0;
            const u8* p = getsegmentdata(
                reinterpret_cast<const mach_header_64*>(
                    _dyld_get_image_header(0)),
                "__TEXT", &size);
            if (p != nullptr) {
                lo = reinterpret_cast<u64>(p);
                hi = lo + size;
            }
        }
    } s_text;
    return s_text.hi != 0 && rip >= s_text.lo && rip < s_text.hi;
}

/// Describe a fault nobody is going to handle, before the process dies of it.
void report_unhandled_fault(mach_port_t thread, exception_type_t exception,
                            i64 code0, i64 code1) noexcept {
    x86_thread_state64_t st{};
    mach_msg_type_number_t n = x86_THREAD_STATE64_COUNT;
    if (thread_get_state(thread, x86_THREAD_STATE64,
                         reinterpret_cast<thread_state_t>(&st),
                         &n) != KERN_SUCCESS) {
        std::fprintf(stderr,
                     "[exc] unhandled exception %d (thread state unavailable)\n",
                     exception);
        return;
    }
    const u64 rip = st.__rip;
    std::fprintf(stderr,
                 "[exc] UNHANDLED %s on mach thread %u at rip=0x%llx (%s) "
                 "cs=0x%llx gs=0x%llx fs=0x%llx code=0x%llx/0x%llx\n",
                 exception == EXC_BAD_ACCESS      ? "EXC_BAD_ACCESS"
                 : exception == EXC_BAD_INSTRUCTION ? "EXC_BAD_INSTRUCTION"
                 : exception == EXC_ARITHMETIC    ? "EXC_ARITHMETIC"
                 : exception == EXC_BREAKPOINT    ? "EXC_BREAKPOINT"
                                                  : "exception",
                 static_cast<unsigned>(thread),
                 static_cast<unsigned long long>(rip),
                 rip_in_host_text(rip) ? "host" : "guest",
                 static_cast<unsigned long long>(st.__cs),
                 static_cast<unsigned long long>(st.__gs),
                 static_cast<unsigned long long>(st.__fs),
                 static_cast<unsigned long long>(code0),
                 static_cast<unsigned long long>(code1));

    // The whole register file. A fetch fault at a wild RIP has two very
    // different causes -- the guest ran and then jumped through a bad pointer,
    // or the register file was never loaded correctly in the first place --
    // and only the registers tell them apart. An all-zero file means the
    // second.
    std::fprintf(stderr,
                 "[exc]   rax=%016llx rcx=%016llx rdx=%016llx rbx=%016llx\n"
                 "[exc]   rsp=%016llx rbp=%016llx rsi=%016llx rdi=%016llx\n"
                 "[exc]   r8 =%016llx r9 =%016llx r10=%016llx r11=%016llx\n"
                 "[exc]   r12=%016llx r13=%016llx r14=%016llx r15=%016llx\n"
                 "[exc]   rflags=%016llx\n",
                 (unsigned long long)st.__rax, (unsigned long long)st.__rcx,
                 (unsigned long long)st.__rdx, (unsigned long long)st.__rbx,
                 (unsigned long long)st.__rsp, (unsigned long long)st.__rbp,
                 (unsigned long long)st.__rsi, (unsigned long long)st.__rdi,
                 (unsigned long long)st.__r8,  (unsigned long long)st.__r9,
                 (unsigned long long)st.__r10, (unsigned long long)st.__r11,
                 (unsigned long long)st.__r12, (unsigned long long)st.__r13,
                 (unsigned long long)st.__r14, (unsigned long long)st.__r15,
                 (unsigned long long)st.__rflags);

    // The native64 transfer block, if a guest thread registered its TEB. This
    // is what says whether the register file above was ever loaded.
    if (const u64 teb = native64_teb_for(static_cast<u32>(thread)); teb != 0) {
        u64 blk[6]{};
        vm_size_t got = 0;
        if (vm_read_overwrite(mach_task_self(),
                              static_cast<vm_address_t>(teb + 0x1E00),
                              sizeof(blk),
                              reinterpret_cast<vm_address_t>(blk),
                              &got) == KERN_SUCCESS && got == sizeof(blk)) {
            std::fprintf(stderr,
                         "[exc]   xfer@0x%llx: rax=%016llx rcx=%016llx "
                         "rdx=%016llx rbx=%016llx rsp=%016llx rbp=%016llx\n",
                         (unsigned long long)(teb + 0x1E00),
                         (unsigned long long)blk[0], (unsigned long long)blk[1],
                         (unsigned long long)blk[2], (unsigned long long)blk[3],
                         (unsigned long long)blk[4], (unsigned long long)blk[5]);
        }
        u64 tail[4]{};
        if (vm_read_overwrite(mach_task_self(),
                              static_cast<vm_address_t>(teb + 0x1E80),
                              sizeof(tail),
                              reinterpret_cast<vm_address_t>(tail),
                              &got) == KERN_SUCCESS && got == sizeof(tail)) {
            std::fprintf(stderr,
                         "[exc]   xfer rip=%016llx flags=%016llx "
                         "hostrsp=%016llx reason=%llu\n",
                         (unsigned long long)tail[0], (unsigned long long)tail[1],
                         (unsigned long long)tail[2], (unsigned long long)tail[3]);
        }
    }

    // A fetch fault at a null or wild RIP means something transferred control
    // there. If it was a CALL, the return address it pushed is still on top of
    // the stack and names the culprit -- which the RIP alone never can.
    if (rip < 0x1000) {
        u64 ret = 0;
        vm_size_t got = 0;
        if (vm_read_overwrite(mach_task_self(),
                              static_cast<vm_address_t>(st.__rsp),
                              sizeof(ret),
                              reinterpret_cast<vm_address_t>(&ret),
                              &got) == KERN_SUCCESS && got == sizeof(ret)) {
            std::fprintf(stderr,
                         "[exc]   rsp=0x%llx [rsp]=0x%llx <- transferred to "
                         "0x%llx from here\n",
                         static_cast<unsigned long long>(st.__rsp),
                         static_cast<unsigned long long>(ret),
                         static_cast<unsigned long long>(rip));
        }
    }
}

bool handle_guest_fault(mach_port_t thread, exception_type_t exception,
                        int64_t code0, int64_t code1) {
    // MEM_WRITE_WATCH: a clean page armed PROT_READ took its first store.
    // Mark it dirty, restore write access, and resume the thread untouched --
    // the faulting instruction simply re-executes. This must run before the
    // executor lookup: it is a Vitreo bookkeeping trap, not a guest fault, and
    // it is the only recoverable fault that needs no guest state at all.
    static const bool s_trace_all =
        std::getenv("VITREO_X64_EXC_TRACE") != nullptr;
    if (s_trace_all) {
        std::fprintf(stderr,
                     "[exc-trace] thread=%u exception=%d code=0x%llx/0x%llx\n",
                     static_cast<unsigned>(thread), exception,
                     (unsigned long long)code0, (unsigned long long)code1);
    }

    if (exception == EXC_BAD_ACCESS && code0 == KERN_PROTECTION_FAILURE &&
        static_cast<u64>(code1) < 0x100000000ull &&
        write_watch::handle_fault(static_cast<GuestAddr>(code1))) {
        if (s_trace_all) {
            std::fprintf(stderr, "[exc-trace]   -> write-watch handled\n");
        }
        return true;
    }

    Compat32Executor* exec = executor_for_mach_thread(thread);
    const bool dbg = std::getenv("VITREO_X64_EXC_DEBUG") != nullptr;
    if (exec == nullptr || exec->trampoline_page() == nullptr) {
        // No compat32 executor. Either this is a host thread -- a genuine
        // Vitreo bug, crash honestly -- or a native64 guest thread, which has
        // no executor by design because Win64 needs no trampoline.
        //
        // Say which, and where. A native64 guest fault currently has no
        // dispatch path, but declining it silently produced a bare SIGILL
        // with no indication of the faulting instruction, which is not an
        // honest crash -- it is an unreadable one.
        // A native64 guest thread has no compat32 executor by design. Hand
        // the fault to its servicer, which owns the SEH/VEH dispatch.
        if (const auto info = native64_info_for(static_cast<u32>(thread));
            info.teb != 0) {
            // A cleared GS is a host transition artifact, not a Windows
            // exception. Repair it and retry the interrupted instruction
            // before classifying the fault for guest SEH.
            if (redirect_native64_fault(thread, info, exception, code0, code1)) {
                return true;
            }
            std::fprintf(stderr,
                         "[exc] native64 fault redirect failed\n");
        }

        report_unhandled_fault(thread, exception, code0, code1);
        if (native64_teb_for(static_cast<u32>(thread)) != 0) {
            // This thread blocks the fault signals, so returning false does
            // not crash it -- the kernel simply retries the faulting
            // instruction forever, presenting as a thread pinned at 100% with
            // no walkable stack. Terminate on purpose instead.
            std::fprintf(stderr,
                         "[exc] native64 guest thread has no handler for this "
                         "fault; terminating (no SEH path yet)\n");
            std::fflush(stderr);
            std::abort();
        }
        if (dbg) std::fprintf(stderr, "[exc] no executor bound\n");
        return false;
    }

    x86_thread_state64_t state{};
    mach_msg_type_number_t count = x86_THREAD_STATE64_COUNT;
    kern_return_t kr = thread_get_state(
        thread, x86_THREAD_STATE64,
        reinterpret_cast<thread_state_t>(&state), &count);
    if (kr != KERN_SUCCESS) {
        if (dbg) std::fprintf(stderr, "[exc] thread_get_state failed: %d\n", kr);
        return false;
    }

    // Capture the faulting thread's x87/MMX/SSE file into the trampoline's
    // FXSAVE slot. Every resume below goes through resume32, whose FXRSTOR
    // hands the guest whatever this slot holds -- without this refresh it
    // would restore the state saved at the LAST ret64 exit, not the state
    // live at this fault, and a codec loop interrupted mid-computation
    // (write-watch, patched CPUID) would silently continue with stale
    // registers. The Apple x86_FLOAT_STATE64 payload from __fpu_fcw onward
    // is laid out exactly as the 512-byte FXSAVE64 image.
    {
        x86_float_state64_t fstate{};
        mach_msg_type_number_t fcount = x86_FLOAT_STATE64_COUNT;
        if (thread_get_state(thread, x86_FLOAT_STATE64,
                             reinterpret_cast<thread_state_t>(&fstate),
                             &fcount) == KERN_SUCCESS) {
            std::memcpy(exec->fxsave_area(), &fstate.__fpu_fcw, 512);
        }
    }

    const u64 rip64 = state.__rip;
    const u32 rip = static_cast<u32>(rip64);
    // Host text lives at 0x400000000. Anything below 4GB is guest or trampoline.
    if (rip64 >= 0x100000000ull) {
        if (dbg) std::fprintf(stderr, "[exc] rip 0x%llx above 4GB (host)\n",
                              (unsigned long long)rip64);
        return false;
    }

    // A UD2 at a recorded site is our stand-in for CPUID (the instruction is
    // unprivileged and cannot be trapped, so it is rewritten in guest code).
    // An unrecorded UD2 is a genuine illegal instruction and falls through.
    if (exception == EXC_BAD_INSTRUCTION && is_cpuid_patch_site(rip)) {
        u32 a = 0, b = 0, c = 0, d = 0;
        const u32 leaf    = static_cast<u32>(state.__rax);
        const u32 subleaf = static_cast<u32>(state.__rcx);
        // This bridge resumes through entry32/compatibility mode, so publish
        // the 32-bit execution profile rather than long-mode-only features.
        ops::synth_cpuid(leaf, subleaf, false, a, b, c, d);

        // The guest cannot be resumed in place: the kernel will not put the
        // thread back into compatibility mode. So publish the post-CPUID state
        // and re-enter through entry32, which is how every normal guest entry
        // already works.
        const u32 gpr[8] = {
            a,                                  // EAX
            c,                                  // ECX
            d,                                  // EDX
            b,                                  // EBX
            static_cast<u32>(state.__rsp),
            static_cast<u32>(state.__rbp),
            static_cast<u32>(state.__rsi),
            static_cast<u32>(state.__rdi),
        };
        exec->publish_guest_state(gpr, static_cast<u32>(state.__rflags),
                                  rip + 2);   // past the 2-byte UD2

        state.__rip = exec->resume32_address();
        kr = thread_set_state(
            thread, x86_THREAD_STATE64,
            reinterpret_cast<thread_state_t>(&state), x86_THREAD_STATE64_COUNT);
        if (dbg) {
            std::fprintf(stderr,
                "[cpuid] leaf=0x%x sub=0x%x -> %08x %08x %08x %08x "
                "resume 0x%x (set_state=%d)\n",
                leaf, subleaf, a, b, c, d, rip + 2, kr);
        }
        return kr == KERN_SUCCESS;
    }

    const u32 tramp_lo = exec->trampoline_base();
    const u32 tramp_hi = tramp_lo + static_cast<u32>(exec->trampoline_size());
    if (rip >= tramp_lo && rip < tramp_hi) {
        // Fault inside the trampoline itself: a stub bug, not a guest AV.
        if (dbg) std::fprintf(stderr,
            "[exc] rip 0x%x in trampoline; exc=%d addr=0x%llx cs=0x%llx "
            "(guest cs=0x%x) rsp=0x%llx\n",
            rip, exception, (unsigned long long)code1,
            (unsigned long long)state.__cs, exec->code_selector(),
            (unsigned long long)state.__rsp);
        return false;
    }

    const u32 ntstatus = ntstatus_for(exception, code0);
    const u32 fault_addr = (exception == EXC_BAD_ACCESS)
                               ? static_cast<u32>(code1)
                               : rip;
    // EXC_BAD_ACCESS code[0] is the kern return; we cannot cheaply know
    // read vs write from THREAD_STATE64. Leave write=0 (read) unless the
    // exception is a protection failure that typically means a store.
    const u32 write = (exception == EXC_BAD_ACCESS &&
                       code0 == KERN_PROTECTION_FAILURE) ? 1u : 0u;

    // Capture the guest registers from the exception state. The guest cannot
    // be routed through exit32 to capture them itself: the kernel will not
    // resume a thread into compatibility mode (setting CS to the 32-bit
    // selector silently reverts to the 64-bit one), so the 32-bit stubs are
    // unreachable from here. The state we already hold has the guest's
    // registers in the low halves of RAX..RDI.
    const u32 gpr[8] = {
        static_cast<u32>(state.__rax), static_cast<u32>(state.__rcx),
        static_cast<u32>(state.__rdx), static_cast<u32>(state.__rbx),
        static_cast<u32>(state.__rsp), static_cast<u32>(state.__rbp),
        static_cast<u32>(state.__rsi), static_cast<u32>(state.__rdi),
    };
    exec->publish_guest_regs(gpr, static_cast<u32>(state.__rflags));
    exec->record_fault(rip, ntstatus, fault_addr, write);

    // Resume in exit32, still in compatibility mode, so the normal capture
    // path stores the faulting GPRs and far-jumps back to 64-bit.
    // Resume at the 64-bit ret64 stub, in the 64-bit mode the kernel is going
    // to impose anyway. ret64 reloads the host RSP and segments and jumps to
    // run()'s resume point, so run() returns Fault with everything captured.
    // CS is deliberately left alone: it cannot be set to a 32-bit selector.
    state.__rip = exec->ret64_address();
    kr = thread_set_state(
        thread, x86_THREAD_STATE64,
        reinterpret_cast<thread_state_t>(&state), x86_THREAD_STATE64_COUNT);
    if (dbg) std::fprintf(stderr,
        "[exc] guest fault rip=0x%x -> ret64 0x%x (cs 0x%llx, left as-is), "
        "set_state=%d\n",
        rip, exec->ret64_address(),
        (unsigned long long)state.__cs, kr);
    return kr == KERN_SUCCESS;
}

void* handler_main(void*) {
    g_handler_ready.store(true, std::memory_order_release);
    alignas(16) unsigned char buf[4096];
    for (;;) {
        auto* hdr = reinterpret_cast<mach_msg_header_t*>(buf);
        hdr->msgh_size = sizeof(buf);
        hdr->msgh_local_port = g_port;
        const kern_return_t kr = mach_msg(
            hdr, MACH_RCV_MSG, 0, sizeof(buf),
            g_port, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
        if (kr != MACH_MSG_SUCCESS) continue;

        auto* req = reinterpret_cast<ExceptionRequest*>(buf);
        const bool handled = handle_guest_fault(
            req->thread.name, req->exception,
            req->codeCnt > 0 ? req->code[0] : 0,
            req->codeCnt > 1 ? req->code[1] : 0);

        ExceptionReply reply{};
        reply.Head.msgh_bits =
            MACH_MSGH_BITS(MACH_MSGH_BITS_REMOTE(hdr->msgh_bits), 0);
        reply.Head.msgh_size = sizeof(reply);
        reply.Head.msgh_remote_port = hdr->msgh_remote_port;
        reply.Head.msgh_local_port = MACH_PORT_NULL;
        reply.Head.msgh_id = hdr->msgh_id + 100;
        reply.NDR = req->NDR;
        reply.RetCode = handled ? KERN_SUCCESS : KERN_FAILURE;

        (void)mach_msg(&reply.Head, MACH_SEND_MSG, sizeof(reply), 0,
                       MACH_PORT_NULL, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
    }
    return nullptr;
}

} // namespace

VoidResult X64ExceptionBridge::start() {
    // fork() keeps only the calling thread, so a child inherits `g_started`
    // set while the handler thread that flag stands for is gone. Every
    // exception then goes to a port nobody is receiving on and the child dies
    // with no diagnosis. Reset the flag in the child so the next start() here
    // really does spawn a handler.
    //
    // The receive right does not survive usefully either, so the port is
    // cleared alongside it rather than reused.
    static const bool s_atfork_installed = [] {
        return pthread_atfork(nullptr, nullptr, [] {
            g_started.store(false, std::memory_order_release);
            g_port = MACH_PORT_NULL;
        }) == 0;
    }();
    (void)s_atfork_installed;

    bool expected = false;
    if (!g_started.compare_exchange_strong(expected, true)) {
        return VoidResult::ok();
    }

    kern_return_t kr = mach_port_allocate(
        mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &g_port);
    if (kr != KERN_SUCCESS) {
        g_started.store(false);
        return VoidResult::err(Error::NotSupported);
    }
    kr = mach_port_insert_right(
        mach_task_self(), g_port, g_port, MACH_MSG_TYPE_MAKE_SEND);
    if (kr != KERN_SUCCESS) {
        mach_port_deallocate(mach_task_self(), g_port);
        g_port = MACH_PORT_NULL;
        g_started.store(false);
        return VoidResult::err(Error::NotSupported);
    }

    if (pthread_create(&g_thread, nullptr, handler_main, nullptr) != 0) {
        mach_port_deallocate(mach_task_self(), g_port);
        g_port = MACH_PORT_NULL;
        g_started.store(false);
        return VoidResult::err(Error::NotSupported);
    }
    pthread_detach(g_thread);
    while (!g_handler_ready.load(std::memory_order_acquire)) {
        sched_yield();
    }
    return VoidResult::ok();
}

void X64ExceptionBridge::bind_executor(Compat32Executor* exec) noexcept {
    const u64 tid = current_thread_id();
    std::lock_guard<std::mutex> lk(g_exec_mu);
    if (exec == nullptr) {
        g_exec_by_tid.erase(tid);
        return;
    }
    g_exec_by_tid[tid] = exec;
    g_guest_cs.store(exec->code_selector(), std::memory_order_release);
}

void X64ExceptionBridge::register_native64_thread(u32 mach_port, u64 teb_base,
                                                  u16 fs_selector) noexcept {
    std::lock_guard<std::mutex> lock(g_native64_mutex);
    g_native64_threads[mach_port] = Native64ThreadInfo{teb_base, fs_selector};
}

void X64ExceptionBridge::unregister_native64_thread(u32 mach_port) noexcept {
    std::lock_guard<std::mutex> lock(g_native64_mutex);
    g_native64_threads.erase(mach_port);
}

u16 X64ExceptionBridge::guest_cs() noexcept {
    return g_guest_cs.load(std::memory_order_acquire);
}

VoidResult X64ExceptionBridge::install_on_current_thread() {
    if (g_port == MACH_PORT_NULL) {
        auto started = start();
        if (started.is_err()) return started;
    }
    const exception_mask_t mask =
        EXC_MASK_BAD_ACCESS | EXC_MASK_BAD_INSTRUCTION |
        EXC_MASK_ARITHMETIC | EXC_MASK_BREAKPOINT;
    // mach_thread_self() hands back a *send right* whose user reference count
    // the caller owns; thread_set_exception_ports takes its own reference, so
    // ours has to go back or every thread that installs the bridge leaks a
    // port name for the life of the process.
    const mach_port_t self = mach_thread_self();
    const kern_return_t kr = thread_set_exception_ports(
        self, mask, g_port,
        static_cast<exception_behavior_t>(
            EXCEPTION_DEFAULT | MACH_EXCEPTION_CODES),
        THREAD_STATE_NONE);
    mach_port_deallocate(mach_task_self(), self);
    if (kr != KERN_SUCCESS) {
        return VoidResult::err(Error::NotSupported);
    }
    return VoidResult::ok();
}

} // namespace vitreo::cpu::x64
