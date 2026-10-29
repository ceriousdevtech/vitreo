// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - native 32-bit guest execution (CPU compatibility mode)

#pragma once

/// @file compat32.hpp
/// @brief Executes 32-bit guest code natively on x86-64 hardware.
///
/// An x86-64 CPU runs 32-bit code natively in *compatibility mode*; this is
/// what WOW64 uses on Windows, and it is not emulation. What macOS removed in
/// Catalina was 32-bit *processes* -- i386 Mach-O binaries, the 32-bit dyld
/// and libSystem. The CPU feature is untouched, and `i386_set_ldt` still
/// installs the code segment that reaches it (verified on macOS 13 with SIP
/// enabled).
///
/// Five hardware facts shape everything here, each measured rather than
/// assumed. The fifth was found here when trampolines moved above 2GB:
///
///   1. RSP's upper 32 bits are ZEROED entering compatibility mode, because
///      the stack pointer becomes ESP. The full 64-bit RSP must be stashed in
///      memory and reloaded by hand, through a RIP-relative operand -- a
///      stack-relative one is unusable exactly when RSP is the broken thing.
///   2. DS must be loaded on entry. Long mode leaves it effectively null,
///      which is harmless because 64-bit addressing ignores it; 32-bit mode
///      dereferences it again and a null DS raises #GP.
///   3. Segment registers must be restored on exit. Returning with a guest
///      selector loaded leaves the thread in a state the kernel cannot
///      deliver a signal into -- it kills the process instead of running the
///      handler, long after and far from the swap.
///   4. A far jump pushes nothing, and a 32-bit far pointer cannot encode a
///      64-bit return address, so the way home is an indirect jump through a
///      slot in low memory rather than a return.
///   5. The absolute `[disp32]` operand the 32-bit stubs rely on means
///      something different in the 64-bit stub: there the displacement is
///      SIGN-extended, so a trampoline at or above `0x80000000` reads its own
///      slots from `0xFFFFFFFF_xxxxxxxx`. The 64-bit half is therefore
///      RIP-relative throughout. Measured: a page at `0xF0002000` faulted in
///      the exit stub with RSP already truncated and no signal handler able to
///      run, while every trampoline below 2GB kept working.
///
/// **FS, not GS.** 32-bit Windows reaches the TEB through FS, which macOS does
/// not use -- unlike GS, which holds pthread TLS and whose base cannot be
/// restored once disturbed (§5.2). That is why the 32-bit path is viable while
/// the Win64 one is still an open question.

#include <vitreo/types.hpp>
#include <vitreo/result.hpp>

namespace vitreo::cpu::x64 {

/// Guest register state transferred across the mode switch.
///
/// Deliberately a flat 32-bit view rather than Vitreo's CpuState: this is the
/// hardware hand-off, and every field here is loaded into or captured from a
/// real register. Translation to and from CpuState belongs one layer up.
struct Compat32State {
    u32 eax = 0, ecx = 0, edx = 0, ebx = 0;
    u32 esp = 0, ebp = 0, esi = 0, edi = 0;
    u32 eip = 0;
    u32 eflags = 0x202;   // IF set, bit 1 reserved-1, as Windows starts a thread
};

/// Why native guest execution returned.
enum class Compat32Exit : u8 {
    /// Guest returned to the sentinel address (normal completion of the
    /// entered call).
    ReturnedToSentinel,
    /// Guest called an imported API. `thunk_address` identifies which, and the
    /// guest stack is exactly as the guest left it -- its own return address
    /// on top -- so the host can dispatch and resume.
    ThunkCall,
    /// Guest took a CPU fault. `last_fault_status()` holds the NTSTATUS and
    /// `last_fault_address()` the faulting linear address; the captured
    /// registers are those at the faulting instruction.
    Fault,
    /// Setup failed before any guest instruction ran.
    SetupFailed,
};

/// Runs 32-bit guest code natively.
///
/// One instance owns one trampoline page below 4GB. The page holds the entry
/// and exit stubs plus the register slots, all reachable by 32-bit absolute
/// addressing so the stubs need no base register -- which matters because
/// every general register belongs to the guest at the moment of transfer.
///
/// **One executor per guest thread, never shared.** Because the stubs address
/// their slots absolutely, the register mirror, the resume address and the
/// saved 64-bit host RSP all live at fixed addresses inside *that one page*.
/// Two threads entering the same trampoline would overwrite each other's
/// registers and, far worse, each other's host RSP -- the second thread would
/// return onto the first thread's stack. Nothing about the page can be made
/// re-entrant without a per-thread base register, and there is none to spare.
/// `for_current_thread()` is therefore the normal way in; run() refuses a
/// foreign thread rather than corrupting silently.
///
/// Shared thunk/sentinel stubs cannot `call` a fixed trampoline for the same
/// reason. They are `call/jmp dword ptr fs:[disp32]` into Vitreo-owned dwords
/// at the end of the 8 KiB TEB mapping -- past every documented Win32 TEB
/// field, not an app TLS slot and not `ArbitraryUserPointer`.
/// `publish_fs_hooks()` writes this thread's collector and sentinel there
/// before `run()`.

/// Vitreo-owned dwords at the end of the 8 KiB 32-bit TEB mapping.
/// Shared native32 stubs reach this thread's trampoline through FS with no
/// spare register. Do not overlap documented Win32 TEB fields (LastError at
/// +0x34, TLS at +0xE10, TlsExpansionSlots at +0xF94).
inline constexpr u32 kTebNative32Collect32 = 0x1FF8;
inline constexpr u32 kTebNative32Sentinel  = 0x1FFC;

class Compat32Executor {
public:
    /// The executor belonging to the calling thread, created on first use.
    ///
    /// The trampoline comes from a process-wide pool of distinct pages, all
    /// below 4GB because 32-bit code reaches them by absolute address, and it
    /// is returned to the pool when the thread exits.
    [[nodiscard]] static VResult<Compat32Executor*> for_current_thread();

    /// Allocate segments and build the trampoline at a caller-chosen address.
    ///
    /// For tests and tools that need a known trampoline address. Ordinary
    /// execution uses for_current_thread(); the returned executor is owned by
    /// the caller and, like any executor, belongs to the thread that made it.
    ///
    /// @param trampoline_hint Where to place the page. Must be below 4GB and
    ///        clear of the guest's own mappings. Fails rather than relocating
    ///        silently, so a collision is visible.
    [[nodiscard]] static VResult<Compat32Executor*> create(u32 trampoline_hint);

    ~Compat32Executor();

    Compat32Executor(const Compat32Executor&)            = delete;
    Compat32Executor& operator=(const Compat32Executor&) = delete;

    /// Address the guest must return to in order to hand control back.
    /// Push this as the return address before entering a guest function.
    [[nodiscard]] u32 sentinel_return_address() const noexcept;

    /// Address of this trampoline's 32-bit thunk collector.
    [[nodiscard]] u32 collect32_address() const noexcept;

    /// Write this thread's collector and sentinel into the Vitreo-owned TEB
    /// slots so shared FS-relative stubs can find this trampoline.
    ///
    /// @p teb_base must be host-writable at that VA (identity mapping, or a
    /// test mapping placed there). Not an app-visible Windows field.
    void publish_fs_hooks(u32 teb_base) const noexcept;

    /// Emit the native stub that belongs at a thunk slot.
    ///
    /// Thread-independent: `call dword ptr fs:[kTebNative32Collect32]`. The
    /// return address the call pushes *is* the slot's identity, so the
    /// collector recovers which thunk was invoked from the stack, then pops
    /// it, leaving the guest stack exactly as the guest's own `call` left it.
    /// Guest code pages are shared; each thread publishes its own collector
    /// into its TEB before `run()`.
    ///
    /// Thunk slots are only STUB_SIZE (8) bytes apart, too little to hold an
    /// identifying immediate plus a jump. The FS-relative call is 7 bytes.
    ///
    /// @param dest      Host-writable pointer to the slot.
    /// @param slot_addr Guest address of the slot (identity; unused by the
    ///                  encoding, kept so callers still pass the slot).
    static void emit_thunk_stub(u8* dest, u32 slot_addr) noexcept;

    /// Size of the stub emit_thunk_stub() writes. Must fit ThunkRegistry's
    /// STUB_SIZE.
    [[nodiscard]] static constexpr usize thunk_stub_size() noexcept { return 7; }

    /// When run() returns ThunkCall, the guest address of the invoked slot.
    [[nodiscard]] u32 last_thunk_address() const noexcept;

    /// Publish guest GPRs captured from a Mach exception's thread state.
    ///
    /// A faulting guest cannot be routed through exit32 to capture its own
    /// registers: `thread_set_state` will NOT resume a thread into
    /// compatibility mode. Setting CS to the 32-bit selector reports success
    /// and the thread then resumes with CS back at the 64-bit selector, so the
    /// 32-bit stubs are unreachable and their bytes would be decoded as 64-bit
    /// instructions. Measured on this host.
    ///
    /// The exception's thread state already holds the guest registers in the
    /// low halves of RAX..RDI, so the bridge captures them directly and
    /// resumes at ret64 instead.
    /// Order: eax, ecx, edx, ebx, esp, ebp, esi, edi.
    void publish_guest_regs(const u32 gpr[8], u32 eflags) noexcept;

    /// The 64-bit stub that restores the host and returns control to run().
    /// This is where the exception bridge must resume a faulting guest, in
    /// the 64-bit mode the kernel imposes anyway.
    [[nodiscard]] u32 ret64_address() const noexcept;

    /// 64-bit stub that re-enters compatibility mode at the guest state
    /// currently published in the trampoline.
    ///
    /// The kernel will not resume a thread back INTO compatibility mode, so a
    /// fault taken in 32-bit guest code cannot simply be continued. It can,
    /// however, be resumed in 64-bit mode at this stub, which far-jumps to
    /// entry32 -- the same path every normal guest entry takes. entry32 loads
    /// all eight GPRs, EFLAGS and EIP from the slots, so publishing an updated
    /// state and pointing RIP here resumes the guest exactly where and how the
    /// handler decides.
    [[nodiscard]] u32 resume32_address() const noexcept;

    /// Publish a full guest state for resume32_address() to load.
    /// Order: eax, ecx, edx, ebx, esp, ebp, esi, edi.
    void publish_guest_state(const u32 gpr[8], u32 eflags, u32 eip) noexcept;

    /// The trampoline's 512-byte FXSAVE image. The exception bridge copies a
    /// faulting thread's x86_FLOAT_STATE64 here so resume32's FXRSTOR hands
    /// the guest back its own x87/MMX/SSE state instead of whatever the host
    /// last left in the registers.
    [[nodiscard]] void* fxsave_area() noexcept;

    /// Emit the stub that belongs at a process/callback sentinel.
    ///
    /// A guest that returns to a sentinel address must leave 32-bit mode, so
    /// the sentinel needs real code there -- unlike the interpreter, nothing
    /// is watching RIP to notice. Thread-independent:
    /// `jmp dword ptr fs:[kTebNative32Sentinel]` into this thread's
    /// trampoline sentinel path.
    static void emit_exit_stub(u8* dest, u32 stub_addr) noexcept;

    /// FS selector holding this guest thread's TEB, loaded on entry to 32-bit
    /// mode and restored on exit. Zero disables (leaves the host's FS).
    ///
    /// 32-bit Windows reaches the TEB through FS. macOS does not use FS --
    /// unlike GS, whose base cannot be restored once disturbed (§5.2) -- which
    /// is precisely why the 32-bit path is viable and the Win64 one is not.
    void set_fs_selector(u16 selector) noexcept { m_fs_sel = selector; }
    [[nodiscard]] u16 fs_selector() const noexcept { return m_fs_sel; }

    /// Record a translated guest fault, so the next run() returns Fault.
    /// Called by the Mach exception bridge from its handler thread.
    void record_fault(u32 eip, u32 ntstatus, u32 address, u32 write) noexcept;

    [[nodiscard]] u32 last_fault_status() const noexcept;
    [[nodiscard]] u32 last_fault_address() const noexcept;
    [[nodiscard]] u32 last_fault_write() const noexcept;

    /// 32-bit code selector, and the trampoline page. Used by the exception
    /// bridge to classify a faulting RIP and to reach the register slots.
    [[nodiscard]] u16 code_selector() const noexcept { return m_code_sel; }
    [[nodiscard]] u8* trampoline_page() const noexcept { return m_page; }

    /// Enter 32-bit mode at @p state.eip and run until the guest reaches the
    /// sentinel. On return @p state holds the captured guest registers.
    ///
    /// The calling thread must not be holding anything that a signal handler
    /// would need: while guest segments are loaded, host libc is unreachable.
    ///
    /// Returns SetupFailed if called from a thread other than the one that
    /// created this executor. That is a caller bug, and letting it through
    /// would corrupt both threads' register state and host stacks in a way
    /// that surfaces arbitrarily far away.
    [[nodiscard]] Compat32Exit run(Compat32State& state) noexcept;

    /// Base of the trampoline page, for the execution-region map that
    /// classifies faults (guest code vs Vitreo stub vs host).
    [[nodiscard]] u32 trampoline_base() const noexcept { return m_base; }
    [[nodiscard]] static usize trampoline_size() noexcept;

    /// Trampoline pages currently handed out by the pool. Diagnostics and
    /// leak tests -- a thread that exits must give its page back.
    [[nodiscard]] static usize pooled_pages_in_use() noexcept;

    /// Reserve the per-thread trampoline pool below 4GB. Idempotent.
    /// Under identity mapping this punches MAP_FIXED through the guest's
    /// PROT_NONE reservation; the caller must record the span as a hole.
    [[nodiscard]] static bool reserve_trampoline_pool() noexcept;
    [[nodiscard]] static u32 trampoline_pool_base() noexcept;
    [[nodiscard]] static usize trampoline_pool_bytes() noexcept;

private:
    Compat32Executor() = default;

    /// Build the stubs into an already-mapped page and take ownership of it.
    [[nodiscard]] static VResult<Compat32Executor*> build_at(u32 base, u8* page,
                                                             bool pooled);

    u32  m_base     = 0;
    u16  m_code_sel = 0;
    u16  m_data_sel = 0;
    u16  m_fs_sel   = 0;       // guest TEB segment, 0 = keep the host's FS
    u8*  m_page     = nullptr;
    bool m_pooled   = false;   // page came from the pool, not a caller hint
    u64  m_owner    = 0;       // thread that may enter this trampoline
};

} // namespace vitreo::cpu::x64
