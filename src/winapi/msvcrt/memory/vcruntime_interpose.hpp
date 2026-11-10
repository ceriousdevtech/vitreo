// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file vcruntime_interpose.hpp
/// @brief Host bodies for the CRT memory/string primitives, and the
///        vcruntime140.dll lookup interposer that routes large calls to them.
///
/// Two halves:
///
///  * The checked bulk primitives (`move_bytes`, `fill_bytes`, ...) implement
///    the Windows contracts of memcpy/memmove/memset/memcmp/memchr and the
///    str/wcs scans over guest memory, including the exception record a
///    fault produces (see the contract table in
///    host-offload plan WS1). They back the
///    msvcrt/ucrtbase exports and the internal `__vitreo_vcrt_*` entries.
///
///  * The interposer: `ModuleManager::get_proc_address` hands every by-name
///    hit to `interpose_lookup`, which, for the redistributable runtime
///    VCRUNTIME140.dll (identified by its PE export-directory name), returns a
///    tiny guest stub that tail-jumps to the module's own body below a
///    measured size crossover and to the host body at or above it.
///    `VITREO_WS1=0` turns the interposer off (the EAT address is returned).
///
/// Guest-visible deltas (WS1 plan): GetProcAddress / import binding of the
/// five sized exports of VCRUNTIME140.dll return a stub address outside the
/// module image (a manual EAT walk still finds the real body); on x64 each
/// such image gets one RX page of stubs within +-2 GiB of it (mapped where
/// a VirtualAlloc would have been free). GetProcAddress BY ORDINAL is not
/// interposed (ModuleManager::get_proc_address_by_ordinal) and returns the
/// module's own body. A fault raised by a host memcpy/memmove/memset body
/// reports ExceptionAddress = an internal resume entry in the thunk region
/// (Windows: an address inside the CRT body); EXCEPTION_CONTINUE_EXECUTION
/// resumes the copy at the faulting piece, as Windows does.

#include "vitreo/types.hpp"

#include <optional>
#include <string_view>
#include <vector>

namespace vitreo { class GuestMemory; }

namespace vitreo::winapi {
class Marshaler;
class ThunkRegistry;
}

namespace vitreo::winapi::vcrt {

enum class Fn : u8 {
    Memcpy, Memmove, Memset, Memcmp, Memchr,
    Strchr, Strrchr, Strstr, Wcschr, Wcsrchr, Wcsstr,
    Count
};
inline constexpr usize kFnCount = static_cast<usize>(Fn::Count);

/// Exact (case-sensitive) export name -> Fn.
[[nodiscard]] std::optional<Fn> fn_for_export(std::string_view name) noexcept;
/// The public C name ("memcpy", ...).
[[nodiscard]] const char* export_name(Fn) noexcept;
/// "__vitreo_vcrt_memcpy" ... "__vitreo_vcrt_wcsstr".
[[nodiscard]] const char* internal_export_name(Fn) noexcept;

inline constexpr u32 kStatusAccessViolation = 0xC0000005u;
inline constexpr u32 kStatusGuardPageViolation = 0x80000001u;

/// The exception a primitive would take: ExceptionInformation[0] = write,
/// [1] = address (inside the first inaccessible page the operation touched).
struct Fault {
    bool write{false};
    GuestAddr address{0};
    u32 code{kStatusAccessViolation};
};

/// memmove semantics (also memcpy's: every Windows memcpy is overlap-safe,
/// tests/oracle_data/crt_memcpy_overlap_probe*.expected). A forward copy runs
/// ascending, an overlapping dst > src copy descending; on a fault every byte
/// before the faulting piece (in copy order) has been written.
///
/// Progress is counted in copy order: @p resume_done bytes of the operation
/// are taken as already done (0 for a fresh call), and on a fault
/// @p done_out (when non-null) receives the bytes completed before the
/// faulting piece. Forward copy and fill: the low bytes; backward copy: the
/// high bytes. The direction depends only on (dst, src, n), so a resumed
/// call with the same arguments continues exactly where the fault stopped.
[[nodiscard]] std::optional<Fault> move_bytes(GuestMemory&, GuestAddr dst,
                                              GuestAddr src, u64 n,
                                              u64 resume_done = 0,
                                              u64* done_out = nullptr) noexcept;
[[nodiscard]] std::optional<Fault> fill_bytes(GuestMemory&, GuestAddr dst, u8 c,
                                              u64 n, u64 resume_done = 0,
                                              u64* done_out = nullptr) noexcept;

struct CmpResult {
    int value{0};  ///< exactly -1, 0 or +1 (unsigned byte compare)
    std::optional<Fault> fault;
};
[[nodiscard]] CmpResult compare_bytes(const GuestMemory&, GuestAddr a, GuestAddr b,
                                      u64 n) noexcept;

struct FindResult {
    GuestAddr at{0};  ///< 0 = NULL
    std::optional<Fault> fault;
};
[[nodiscard]] FindResult find_byte(const GuestMemory&, GuestAddr p, u8 c, u64 n) noexcept;
/// strchr (last=false) / strrchr (last=true); the terminator takes part.
[[nodiscard]] FindResult find_char(const GuestMemory&, GuestAddr s, u8 c, bool last) noexcept;
/// wcschr / wcsrchr over UTF-16 units; @p s may be odd.
[[nodiscard]] FindResult find_wchar(const GuestMemory&, GuestAddr s, u16 c, bool last) noexcept;
[[nodiscard]] FindResult find_str(const GuestMemory&, GuestAddr h, GuestAddr needle) noexcept;
[[nodiscard]] FindResult find_wstr(const GuestMemory&, GuestAddr h, GuestAddr needle) noexcept;

/// Where a faulting store-side body resumes after EXCEPTION_CONTINUE_EXECUTION.
enum class ResumeKind : u8 { Move = 0, Fill = 1 };
struct Resume {
    ResumeKind kind{ResumeKind::Move};
    u64 done{0};  ///< bytes completed in copy order before the fault
};

/// Raise @p f in the guest when the thunk returns (cpu->pending_raise:
/// code, NumberParameters 2, [0] = write, [1] = address). A guard-page fault
/// also disarms the guard: PAGE_GUARD is one-shot.
///
/// The faulting context names a point the call can continue from, as a
/// Windows fault names the faulting access. With @p resume (memcpy/memmove/
/// memset) and a published resume record, RIP is the internal resume entry
/// of that kind and EAX/RAX holds the progress; the arguments are untouched
/// (x86 stack, x64 RCX/RDX/R8), so continuing finishes the remaining bytes
/// and returns the original destination. Without one (the read-only
/// scans), RIP stays at the entry of this body: continuing re-runs the
/// scan, which reads the same unchanged bytes and gives the same result.
void deliver(Marshaler&, const Fault&, std::optional<Resume> resume = std::nullopt) noexcept;

/// The cdecl/x64 body of @p fn: reads the arguments, sets the return value,
/// delivers any fault (the return register is then unspecified).
void run_body(Fn fn, Marshaler& m) noexcept;

/// "__vitreo_vcrt_memmove_resume" / "__vitreo_vcrt_memset_resume": the
/// internal ucrtbase.dll entries a faulted copy/fill continues from.
[[nodiscard]] const char* resume_export_name(ResumeKind) noexcept;
/// Body of a resume entry: the original arguments of the faulted call plus
/// the progress in EAX/RAX (clamped: a value above n restarts from 0).
void run_resume(ResumeKind kind, Marshaler& m) noexcept;

/// Write the resume record ({magic, thunk base, move slot, fill slot}) into
/// the accelerator arena of @p x64 ? the x64 arena at thunk base +
/// X64_ACCEL_OFFSET : the x86 arena, mapping it if needed. The record lives
/// in guest memory, so it belongs to exactly this process. False when the
/// registry has no resume entries (then a fault re-runs the call).
bool publish_resume_record(GuestMemory&, ThunkRegistry&, bool x64) noexcept;

// ---------------------------------------------------------------------------
// Size-branch stubs (guest code in the guest_accel arena's stub region)
// ---------------------------------------------------------------------------
//
// Every instruction keeps [ESP]/[RSP] = the caller's return address and the
// stack pointer unchanged, and touches no register but the flags (x86
// __cdecl / x64 calling convention; x64 leaf rule, so a fault raised at the
// host gateway unwinds straight to the caller without a RUNTIME_FUNCTION).

/// cmp dword [esp+0Ch], threshold ; jb original ; jmp dword [cell] ; cell:
/// dd host_slot. 24 bytes, padded with INT3 to CRT_ACCEL_STUB_SLOT_X86. The
/// host branch is indirect so the JIT can serve it in-chain (L3-K).
[[nodiscard]] std::vector<u8> encode_stub_x86(GuestAddr at, u32 threshold,
                                              GuestAddr host_slot, GuestAddr original);
/// cmp r8, threshold (imm32, threshold < 2^31; the unsigned branch sends a
/// size of 2^63 or more to the host) ; jb rel32 original ; jmp qword [rip+0]
/// + abs64 host_slot. Padded with INT3 to X64_ACCEL_STUB_SLOT. Empty when the
/// threshold does not fit the imm32 or @p original is out of rel32 reach of
/// @p at: there is no far form. A far guest branch (an indirect jump) costs
/// ~20 ns per call on the JIT, measured in VcrtBench.StubOverheadProbe,
/// which would regress every small call; the interposer instead places the
/// x64 stubs on a page within reach of each image.
[[nodiscard]] std::vector<u8> encode_stub_x64(GuestAddr at, u32 threshold,
                                              GuestAddr host_slot, GuestAddr original);

// ---------------------------------------------------------------------------
// Interposer
// ---------------------------------------------------------------------------

/// False when VITREO_WS1=0 (decided once), or as forced by force_for_test.
[[nodiscard]] bool enabled() noexcept;
enum class Force : int { None = 0, On = 1, Off = 2 };
void force_for_test(Force) noexcept;

/// Size thresholds: a call with size >= threshold takes the host body.
/// kNeverHost = the export is never interposed.
inline constexpr u32 kNeverHost = 0xFFFFFFFFu;
[[nodiscard]] u32 threshold(Fn, u32 bitness) noexcept;
/// Test hook: override one threshold; std::nullopt restores the baked value.
void set_threshold_for_test(Fn, u32 bitness, std::optional<u32> value) noexcept;

/// True when the PE image at [base, image_end) names itself VCRUNTIME140.dll
/// (IMAGE_EXPORT_DIRECTORY.Name, ASCII case-insensitive). Every read is
/// bounds-checked against the image; any malformation gives false.
[[nodiscard]] bool is_vcruntime140_image(const GuestMemory&, GuestAddr base,
                                         GuestAddr image_end) noexcept;

struct LookupModule {
    GuestAddr base{0};
    GuestAddr image_end{0};
    u32 bitness{0};  ///< 32 or 64
};

/// The address GetProcAddress / import binding hands out for @p export_name
/// of @p mod, whose EAT holds @p original. Returns @p original unless the
/// interposer is enabled, the export is one of the eleven with a threshold,
/// the module is VCRUNTIME140.dll, the host slot resolves and a stub slot
/// is available (x86: the arena's stub region; x64: a page within rel32
/// reach of the image, allocated on first use). A threshold of 0 for a function without a size
/// argument returns the host slot itself; otherwise a size-branch stub.
[[nodiscard]] GuestAddr interpose_lookup(GuestMemory&, ThunkRegistry&,
                                         const LookupModule& mod,
                                         std::string_view export_name,
                                         GuestAddr original) noexcept;

}  // namespace vitreo::winapi::vcrt
