// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - x86_64 native backend: guest/host transfer block

#pragma once

/// @file native64_layout.hpp
/// @brief Offsets of the Vitreo-private transfer block inside a Win64 TEB.
///
/// Shared with `native64_stubs.S`, which cannot include a C++ header, so every
/// constant here is duplicated there as an assembler symbol. `Native64Layout`
/// static-asserts the pairing at build time via the sizes below; the stubs
/// themselves are checked by `X64Native64Test.LayoutMatchesStubs`.
///
/// **Why the TEB.** Guest GS holds the guest TEB, so `gs:[disp32]` reaches a
/// fixed offset in *the running thread's own* TEB with no base register --
/// and at a thunk boundary there is no register to spare, since every one of
/// them belongs to the guest. That makes the entry, collector and sentinel
/// stubs thread-independent: one shared copy serves every guest thread,
/// because GS already selects which TEB it lands in.
///
/// This is strictly better than the 32-bit path, which needs one trampoline
/// page per thread precisely because its stubs address their slots absolutely
/// (see compat32.hpp). GS-relative addressing removes that whole problem.
///
/// **Placement.** The block sits at 0x1E00 in the 8 KiB TEB mapping that
/// teb_setup.cpp creates. The documented Win64 TEB ends well below that --
/// TlsExpansionSlots, the highest documented field, is at 0x1780 -- so this
/// is past every field a guest can legitimately read, not an app TLS slot.

#include <vitreo/types.hpp>

namespace vitreo::cpu::x64 {

/// Native64 maps and fills one MiB of 16-byte import-thunk slots.
inline constexpr u64 kN64ThunkRegionMapSize = 0x100000;

/// Byte offsets from the TEB base. Keep in lockstep with native64_stubs.S.
enum : u32 {
    /// 512-byte FXSAVE64 image of the guest's x87/MMX/SSE state.
    ///
    /// Not optional. The Win64 ABI makes XMM6-XMM15 **non-volatile**, so a
    /// guest holding a live value in one across an API call is entitled to
    /// get it back -- and the host C++ that services the call uses those
    /// registers freely. Without this the corruption is silent and delayed:
    /// the CRT survives hundreds of calls and then dereferences a pointer
    /// half-rebuilt from a clobbered register.
    ///
    /// FXSAVE64 requires 16-byte alignment; TEBs are page-aligned, so
    /// base + 0x1C00 always is.
    kN64FxSave = 0x1C00,

    kN64Rax   = 0x1E00,
    kN64Rcx   = 0x1E08,
    kN64Rdx   = 0x1E10,
    kN64Rbx   = 0x1E18,
    kN64Rsp   = 0x1E20,
    kN64Rbp   = 0x1E28,
    kN64Rsi   = 0x1E30,
    kN64Rdi   = 0x1E38,
    kN64R8    = 0x1E40,
    kN64R9    = 0x1E48,
    kN64R10   = 0x1E50,
    kN64R11   = 0x1E58,
    kN64R12   = 0x1E60,
    kN64R13   = 0x1E68,
    kN64R14   = 0x1E70,
    kN64R15   = 0x1E78,
    kN64Rip   = 0x1E80,
    kN64Flags = 0x1E88,

    /// Host RSP, parked by the entry stub and reloaded by every exit path.
    kN64HostRsp = 0x1E90,
    /// Why guest execution stopped; one of Native64Exit.
    kN64Reason  = 0x1E98,
    /// Address of the shared collector, read by the guest-side thunk stub.
    kN64Collect = 0x1EA0,
    /// Address of the shared sentinel stub, pushed as a guest return address.
    kN64Sentinel = 0x1EA8,

    // Fault detail, written by the Mach bridge before it redirects a faulting
    // guest thread. The registers for a fault come from the thread's Mach
    // state rather than from a stub, because the faulting instruction never
    // gets to run one.
    kN64FaultStatus  = 0x1EB0,   ///< Windows NTSTATUS for the fault
    kN64FaultAddress = 0x1EB8,   ///< faulting linear address
    kN64FaultWrite   = 0x1EC0,   ///< 1 if the access was a write

    // Scratch used only by the Mach GS-repair trampoline. This cannot share
    // kN64Rdi/kN64Rip: an interrupted entry or collector still owns those
    // saved guest values.
    kN64RepairRdi = 0x1EC8,
    kN64RepairRip = 0x1ED0,
};

/// Size of an FXSAVE64 image.
inline constexpr u32 kN64FxSaveSize = 512;
static_assert(kN64FxSave + kN64FxSaveSize <= kN64Rax,
              "FXSAVE area overlaps the register block");
static_assert(kN64FxSave % 16 == 0, "FXSAVE64 needs a 16-byte aligned operand");

/// First and last byte of the block, for mapping and overlap checks.
inline constexpr u32 kN64BlockBegin = kN64Rax;
inline constexpr u32 kN64BlockEnd   = kN64RepairRip + 8;

/// The TEB mapping must cover the block. teb_setup.cpp maps 8 KiB.
inline constexpr u32 kN64RequiredTebSize = 0x2000;
static_assert(kN64BlockEnd <= kN64RequiredTebSize,
              "transfer block runs past the TEB mapping");

/// Highest documented Win64 TEB field (TlsExpansionSlots). The block must not
/// collide with anything a guest can legitimately read.
inline constexpr u32 kWin64HighestDocumentedTebField = 0x1780;
static_assert(kN64BlockBegin > kWin64HighestDocumentedTebField,
              "transfer block overlaps documented Win64 TEB fields");
static_assert(kN64FxSave > kWin64HighestDocumentedTebField,
              "FXSAVE area overlaps documented Win64 TEB fields");

/// Why native guest execution returned. Mirrors Compat32Exit.
enum class Native64Exit : u64 {
    /// Guest returned to the sentinel: the entered call completed.
    ReturnedToSentinel = 0,
    /// Guest called an imported API. kN64Rip holds the slot's gateway address
    /// and the guest stack is exactly as the guest's own `call` left it.
    ThunkCall = 1,
    /// Guest took a CPU fault; registers are those at the faulting
    /// instruction, published by the Mach exception bridge.
    Fault = 2,
    /// Setup failed before any guest instruction ran.
    SetupFailed = 3,
};

} // namespace vitreo::cpu::x64
