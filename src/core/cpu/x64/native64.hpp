// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - x86_64 native backend: Win64 guest execution

#pragma once

/// @file native64.hpp
/// @brief Executes 64-bit guest code natively on x86-64 hardware.
///
/// The guest and the host are the same architecture here, so unlike the 32-bit
/// path (compat32.hpp) there is no mode switch, no LDT code segment and no far
/// jump. Win64 reads its TEB through GS, while macOS keeps pthread TLS there.
/// Native64 therefore translates guest GS memory operands to an LDT-derived
/// FS carrier on dedicated `GuestTebThread`s.
///
/// Everything in this file therefore runs on such a thread and must stay
/// libc-free. The stubs are hand-written assembly (native64_stubs.S) and the
/// C++ here does nothing but read and write plain memory.

#include "core/cpu/x64/native64_layout.hpp"

#include <vitreo/result.hpp>
#include <vitreo/types.hpp>

namespace vitreo::cpu::x64 {

/// The guest register file, as the stubs read and write it.
///
/// This is a *view over the TEB*, not a copy: `Native64Transfer::at()` returns
/// a pointer into the live block the stubs use, so a write here is what the
/// guest resumes with. Translation to and from Vitreo's CpuState belongs one
/// layer up, exactly as with Compat32State.
struct Native64Transfer {
    u64 rax, rcx, rdx, rbx, rsp, rbp, rsi, rdi;
    u64 r8, r9, r10, r11, r12, r13, r14, r15;
    u64 rip;
    u64 flags;
    u64 host_rsp;   ///< owned by the stubs; do not write
    u64 reason;     ///< Native64Exit
    u64 collect;    ///< owned by publish_gs_hooks()
    u64 sentinel;   ///< owned by publish_gs_hooks()
    u64 fault_status;   ///< NTSTATUS, written by the Mach bridge
    u64 fault_address;
    u64 fault_write;
    u64 repair_rdi;     ///< private scratch for native64_fs_repair
    u64 repair_rip;

    /// The live transfer block inside @p teb_base.
    [[nodiscard]] static Native64Transfer* at(void* teb_base) noexcept {
        return reinterpret_cast<Native64Transfer*>(
            static_cast<u8*>(teb_base) + kN64BlockBegin);
    }
};

static_assert(sizeof(Native64Transfer) == kN64BlockEnd - kN64BlockBegin,
              "transfer struct does not match the offsets the stubs use");
static_assert(offsetof(Native64Transfer, rip) == kN64Rip - kN64BlockBegin);
static_assert(offsetof(Native64Transfer, host_rsp) ==
              kN64HostRsp - kN64BlockBegin);
static_assert(offsetof(Native64Transfer, reason) ==
              kN64Reason - kN64BlockBegin);
static_assert(offsetof(Native64Transfer, sentinel) ==
              kN64Sentinel - kN64BlockBegin);
static_assert(offsetof(Native64Transfer, fault_status) ==
              kN64FaultStatus - kN64BlockBegin);
static_assert(offsetof(Native64Transfer, fault_write) ==
              kN64FaultWrite - kN64BlockBegin);
static_assert(offsetof(Native64Transfer, repair_rip) ==
              kN64RepairRip - kN64BlockBegin);

/// Write the shared stub addresses into @p teb_base's transfer block.
///
/// Must be called before the first enter() on a thread. The guest-side thunk
/// stub reads kN64Collect through the FS carrier, so an unpublished TEB sends the first
/// import call to address zero.
///
/// @param teb_base Host-writable pointer to the TEB. Under identity mapping
///                 this is also the guest address.
void publish_gs_hooks(void* teb_base) noexcept;

/// Address a guest function must return to in order to hand control back.
/// Push it as the return address before entering a guest call.
[[nodiscard]] u64 sentinel_return_address() noexcept;

/// Address of the shared thunk collector.
[[nodiscard]] u64 collect_address() noexcept;

/// Emit the native stub belonging at a thunk slot.
///
/// `call qword ptr fs:[kN64Collect]` -- 8 bytes, exactly ThunkRegistry's
/// STUB_SIZE, and thread-independent because FS selects the calling thread's
/// own TEB. The return address the call pushes is the slot's dispatch gateway
/// and therefore the thunk's identity, which is how the collector knows what
/// was invoked without spending a register on it.
void emit_thunk_stub(u8* dest) noexcept;

/// Emit the AMD64 Control Flow Guard dispatch fallback.
///
/// GuardCFDispatchFunctionPointer receives the guarded target in RAX and
/// tail-branches to it.  Native x64 can perform that contract directly,
/// without crossing the generic host-thunk collector for every indirect call.
void emit_guard_dispatch_stub(u8* dest) noexcept;

/// Emit `mov eax, fs:[0x68]; ret` -- GetLastError, served entirely inside the
/// guest thread.
///
/// Win64 keeps the thread's last error in TEB.LastErrorValue at offset 0x68,
/// and on this backend FS already carries the guest TEB, so the guest can read
/// it without leaving its own thread. That matters out of all proportion to
/// the API: every ordinary thunk costs a guest<->host THREAD round trip here,
/// and GetLastError/SetLastError alone were ~2.1M of a launcher image's
/// 2.06M round trips while that process started. 9 bytes, inside the
/// 16-byte thunk slot.
void emit_get_last_error_stub(u8* dest) noexcept;

/// Emit `mov fs:[0x68], ecx; ret` -- SetLastError, the write half of the pair.
void emit_set_last_error_stub(u8* dest) noexcept;

/// Offset of TEB.LastErrorValue, the field both stubs touch.
inline constexpr u32 kTebLastErrorValue = 0x68;

/// Address of the stub a faulting guest thread is redirected to.
///
/// The Mach bridge captures the faulting register file itself -- the faulting
/// instruction never gets to run a stub -- writes it to the transfer block,
/// and then points RIP here so the thread walks back out to its servicer.
/// The bridge passes the thread's FS carrier selector in RDI.
[[nodiscard]] u64 fault_return_address() noexcept;

/// Address of the libc-free trampoline used by the Mach exception bridge to
/// reload a cleared FS carrier selector and retry the interrupted instruction.
[[nodiscard]] u64 fs_repair_address() noexcept;

/// Resolve a guest instruction address that must enter a Vitreo thunk when it
/// executes -- ntdll's `int 2Eh` service entry -- to the in-region dispatch
/// gateway naming that thunk. Returns 0 when the address is not such an alias.
///
/// The runtime owns the thunk registry, so it installs the resolver; the fault
/// bridge only consults it.
using DispatchAliasResolver = u64 (*)(u64 rip) noexcept;
void native64_set_dispatch_alias_resolver(DispatchAliasResolver fn) noexcept;
[[nodiscard]] u64 native64_dispatch_alias_gateway(u64 rip) noexcept;

/// A copied ntdll veneer (a browser sandbox writes one) faults at `int 2Eh`
/// or `syscall` with the service number in EAX, not at the registered alias.
using NativeServiceResolver = u64 (*)(u32 service_id) noexcept;
void native64_set_service_resolver(NativeServiceResolver fn) noexcept;
[[nodiscard]] u64 native64_service_gateway(u32 service_id) noexcept;

/// If @p fault_rip is one of the FS/TEB validation sequences in an assembly
/// boundary, return the instruction where that validation must be retried
/// after FS repair. Otherwise return zero.
[[nodiscard]] u64 fs_probe_resume_address(u64 fault_rip) noexcept;

/// Size of the stub emit_thunk_stub() writes. Must fit ThunkRegistry::STUB_SIZE.
[[nodiscard]] constexpr usize thunk_stub_size() noexcept { return 8; }

/// Run guest code until it calls a thunk, returns to the sentinel, or faults.
///
/// Reads the guest register file from the calling thread's own TEB (through
/// FS) and writes it back there, so there is no argument to pass and nothing
/// thread-specific to look up.
///
/// @param fs_selector The thread's FS TEB carrier selector, reloaded on entry.
///        It must be passed every time rather than set once per thread: an
///        Reloading also makes the transfer boundary self-contained.
///
/// **Precondition: the calling thread must be a dedicated guest thread.**
[[nodiscard]] Native64Exit enter(u16 fs_selector) noexcept;

} // namespace vitreo::cpu::x64
