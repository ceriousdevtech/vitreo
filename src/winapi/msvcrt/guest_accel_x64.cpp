// SPDX-License-Identifier: LGPL-2.1-only

/// @file guest_accel_x64.cpp
/// @brief Guest-side x64 implementations of hot, tiny exports. See
///        guest_accel.hpp.
///
/// Each function is the bytes llvm-mc produced for the Intel-syntax source in
/// its comment (x86_64, MS x64 ABI: argument in RCX / XMM0, result in RAX;
/// RAX RCX RDX R8-R11 and EFLAGS are volatile, DF is 0 on entry and exit).
/// tests/winapi/test_guest_accel_x64.cpp runs them through the interpreter and
/// the JIT against the contracts below.

#include "winapi/msvcrt/guest_accel.hpp"
#include "winapi/msvcrt/memory/vcruntime_interpose.hpp"
#include "winapi/core/thunk_registry.hpp"
#include "winapi/kernel32/thread/tls.hpp"
#include "core/memory/guest_memory.hpp"

#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cstdlib>
#include <initializer_list>
#include <string>

namespace vitreo::winapi {

namespace {

// LPVOID TlsGetValue(DWORD dwTlsIndex) -- Learn TlsGetValue; tls.cpp layout.
// The x64 TEB holds TLS_MINIMUM_AVAILABLE slots at gs:[0x1480] and a pointer
// to TLS_EXPANSION_SLOTS more at gs:[0x1780] (0 until a TlsSetValue on this
// thread allocates it). Success clears LastErrorValue (gs:[0x68]); an index
// at or above TLS_TOTAL_SLOTS returns 0 with ERROR_INVALID_PARAMETER.
//
//     mov  eax, ecx
//     cmp  eax, 0x40
//     jae  expansion
//     mov  rax, qword ptr gs:[8*rax + 0x1480]
//     mov  dword ptr gs:[0x68], 0
//     ret
//   expansion:
//     cmp  eax, 0x440
//     jae  invalid
//     mov  dword ptr gs:[0x68], 0
//     mov  rdx, qword ptr gs:[0x1780]
//     test rdx, rdx
//     jz   unallocated
//     mov  rax, qword ptr [rdx + 8*rax - 0x200]
//     ret
//   unallocated:
//     xor  eax, eax
//     ret
//   invalid:
//     mov  dword ptr gs:[0x68], 87
//     xor  eax, eax
//     ret
constexpr u8 kTlsGetValue[] = {
    0x89, 0xc8,
    0x83, 0xf8, 0x40,
    0x73, 0x16,
    0x65, 0x48, 0x8b, 0x04, 0xc5, 0x80, 0x14, 0x00, 0x00,
    0x65, 0xc7, 0x04, 0x25, 0x68, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xc3,
    0x3d, 0x40, 0x04, 0x00, 0x00,
    0x73, 0x26,
    0x65, 0xc7, 0x04, 0x25, 0x68, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x65, 0x48, 0x8b, 0x14, 0x25, 0x80, 0x17, 0x00, 0x00,
    0x48, 0x85, 0xd2,
    0x74, 0x09,
    0x48, 0x8b, 0x84, 0xc2, 0x00, 0xfe, 0xff, 0xff,
    0xc3,
    0x31, 0xc0,
    0xc3,
    0x65, 0xc7, 0x04, 0x25, 0x68, 0x00, 0x00, 0x00, 0x57, 0x00, 0x00, 0x00,
    0x31, 0xc0,
    0xc3,
};
static_assert(TLS_MINIMUM_AVAILABLE == 0x40 && TLS_TOTAL_SLOTS == 0x440,
              "kTlsGetValue encodes these bounds");

// int _finite(double x) -- 1 when x is neither infinite nor NaN, else 0
// (tools/oracle_probe/crt_classify_probe.expected: ucrtbase and msvcrt return
// exactly 1 or 0). Integer test of the exponent, so MXCSR.DAZ does not apply.
//
//     movq  rax, xmm0
//     shl   rax, 1
//     shr   rax, 53
//     cmp   eax, 0x7FF
//     setne al
//     movzx eax, al
//     ret
constexpr u8 kFinite[] = {
    0x66, 0x48, 0x0f, 0x7e, 0xc0,
    0x48, 0xd1, 0xe0,
    0x48, 0xc1, 0xe8, 0x35,
    0x3d, 0xff, 0x07, 0x00, 0x00,
    0x0f, 0x95, 0xc0,
    0x0f, 0xb6, 0xc0,
    0xc3,
};

// int _isnan(double x) -- 1 for any NaN (quiet or signalling, either sign),
// else 0 (crt_classify_probe.expected). |x| as bits exceeds the infinity's.
//
//     movq   rax, xmm0
//     shl    rax, 1
//     movabs rcx, 0xFFE0000000000000
//     cmp    rax, rcx
//     seta   al
//     movzx  eax, al
//     ret
constexpr u8 kIsnan[] = {
    0x66, 0x48, 0x0f, 0x7e, 0xc0,
    0x48, 0xd1, 0xe0,
    0x48, 0xb9, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xe0, 0xff,
    0x48, 0x39, 0xc8,
    0x0f, 0x97, 0xc0,
    0x0f, 0xb6, 0xc0,
    0xc3,
};

// ---- Critical sections ------------------------------------------------------
//
// The uncontended halves of Learn EnterCriticalSection / TryEnterCriticalSection
// / LeaveCriticalSection, on the protocol critical_section.cpp implements for
// the host halves (RTL_CRITICAL_SECTION x64: LockCount +8, RecursionCount +0xC,
// OwningThread +0x10; Server 2003 SP1+ LockCount: -1 free, -2 held with no
// waiters, -2-4*n held with n waiters, the waiter encoding published only by
// the host's enter_slow). Windows serves these cases in user mode with one
// interlocked instruction; so does this. Everything else tail-jumps into the
// host with RCX and the return address untouched:
//   - a held section: __vitreo_cs_enter_slow (spin count, waiter
//     registration, keyed-event park, ContentionCount, debug record);
//   - a release of a section that has ever been contended, or that finds
//     waiters encoded: the unlock is completed here and
//     __vitreo_cs_leave_wake wakes one (it never touches the section).
// OwningThread is the TEB's ClientId.UniqueThread (gs:[0x48]), the id the
// host writes there; a releaser clears it BEFORE unlocking, so a stale read in
// a recursion test can never match a thread that does not own the section.
// A NULL or unmapped section faults on its first access, as on Windows.
//
// The host halves contend on the guest's LockCount only when that word is
// 4-byte aligned (lock_word: an ARM64 atomic needs natural alignment); a
// misaligned section -- Windows permits it, and a class that keeps its
// CRITICAL_SECTION in a byte array produces one -- is the host's private
// mutex instead. Each function therefore tests the section's alignment first
// (LockCount is at +8) and hands a misaligned one to the full host export, so
// every caller of that section takes the same path.

// VOID EnterCriticalSection(LPCRITICAL_SECTION cs)
//
//     test cl, 3
//     jnz  host
//     mov  rax, qword ptr gs:[0x48]
//     cmp  qword ptr [rcx + 0x10], rax
//     je   recursive
//     mov  edx, 0xFFFFFFFE
//     mov  eax, 0xFFFFFFFF
//     lock cmpxchg dword ptr [rcx + 8], edx
//     jne  slow
//     mov  rax, qword ptr gs:[0x48]
//     mov  qword ptr [rcx + 0x10], rax
//     mov  dword ptr [rcx + 0xC], 1
//     ret
//   recursive:
//     inc  dword ptr [rcx + 0xC]
//     ret
//   slow:
//     jmp  kernel32!__vitreo_cs_enter_slow        ; rel32 at +0x3F
//   host:
//     jmp  kernel32!EnterCriticalSection (host)   ; rel32 at +0x44
constexpr u8 kEnterCriticalSection[] = {
    0xf6, 0xc1, 0x03,
    0x75, 0x3e,
    0x65, 0x48, 0x8b, 0x04, 0x25, 0x48, 0x00, 0x00, 0x00,
    0x48, 0x39, 0x41, 0x10,
    0x74, 0x26,
    0xba, 0xfe, 0xff, 0xff, 0xff,
    0xb8, 0xff, 0xff, 0xff, 0xff,
    0xf0, 0x0f, 0xb1, 0x51, 0x08,
    0x75, 0x19,
    0x65, 0x48, 0x8b, 0x04, 0x25, 0x48, 0x00, 0x00, 0x00,
    0x48, 0x89, 0x41, 0x10,
    0xc7, 0x41, 0x0c, 0x01, 0x00, 0x00, 0x00,
    0xc3,
    0xff, 0x41, 0x0c,
    0xc3,
    0xe9, 0x00, 0x00, 0x00, 0x00,
    0xe9, 0x00, 0x00, 0x00, 0x00,
};
constexpr usize kEnterSlowRel32 = 0x3F;
constexpr usize kEnterHostRel32 = 0x44;

// BOOL TryEnterCriticalSection(LPCRITICAL_SECTION cs) -- Learn: nonzero when
// the section was entered or the caller already owns it, zero (at once, no
// wait) when another thread owns it.
//
//     test cl, 3
//     jnz  host
//     mov  rax, qword ptr gs:[0x48]
//     cmp  qword ptr [rcx + 0x10], rax
//     je   recursive
//     mov  edx, 0xFFFFFFFE
//     mov  eax, 0xFFFFFFFF
//     lock cmpxchg dword ptr [rcx + 8], edx
//     jne  busy
//     mov  rax, qword ptr gs:[0x48]
//     mov  qword ptr [rcx + 0x10], rax
//     mov  dword ptr [rcx + 0xC], 1
//     mov  eax, 1
//     ret
//   recursive:
//     inc  dword ptr [rcx + 0xC]
//     mov  eax, 1
//     ret
//   busy:
//     xor  eax, eax
//     ret
//   host:
//     jmp  kernel32!TryEnterCriticalSection (host) ; rel32 at +0x4C
constexpr u8 kTryEnterCriticalSection[] = {
    0xf6, 0xc1, 0x03,
    0x75, 0x46,
    0x65, 0x48, 0x8b, 0x04, 0x25, 0x48, 0x00, 0x00, 0x00,
    0x48, 0x39, 0x41, 0x10,
    0x74, 0x2b,
    0xba, 0xfe, 0xff, 0xff, 0xff,
    0xb8, 0xff, 0xff, 0xff, 0xff,
    0xf0, 0x0f, 0xb1, 0x51, 0x08,
    0x75, 0x23,
    0x65, 0x48, 0x8b, 0x04, 0x25, 0x48, 0x00, 0x00, 0x00,
    0x48, 0x89, 0x41, 0x10,
    0xc7, 0x41, 0x0c, 0x01, 0x00, 0x00, 0x00,
    0xb8, 0x01, 0x00, 0x00, 0x00,
    0xc3,
    0xff, 0x41, 0x0c,
    0xb8, 0x01, 0x00, 0x00, 0x00,
    0xc3,
    0x31, 0xc0,
    0xc3,
    0xe9, 0x00, 0x00, 0x00, 0x00,
};
constexpr usize kTryEnterHostRel32 = 0x4C;

// VOID LeaveCriticalSection(LPCRITICAL_SECTION cs). RecursionCount is
// compared unsigned, as the host's leave_accel does.
//
// The release is the guest's alone only for a section that has NEVER been
// contended: LockSemaphore is still NULL until the host's enter_slow
// registers its first waiter, and from then on reads (HANDLE)-1 (Windows'
// keyed-event sentinel; publish_lock_semaphore). A contended section always
// goes to the host after unlocking, as the x86 fast path does, because
// LockCount alone cannot say "nobody is parked": a release resets it to -1,
// a fast-path acquire that barges in before the woken waiter runs makes it
// -2 again, and the waiter -- registered, but back asleep without
// republishing -- would then miss the next release's wake. The waiter
// publishes LockSemaphore before its first acquire attempt, so a releaser
// that reads NULL here and wins the -2 -> -1 CAS cannot strand it: any
// waiter that will park has encoded itself into LockCount first and makes
// the CAS fail.
//
//     test cl, 3
//     jnz  host
//     cmp  dword ptr [rcx + 0xC], 1
//     ja   nested
//     mov  dword ptr [rcx + 0xC], 0
//     mov  qword ptr [rcx + 0x10], 0
//     cmp  qword ptr [rcx + 0x18], 0
//     jne  contended
//     mov  edx, 0xFFFFFFFF
//     mov  eax, 0xFFFFFFFE
//     lock cmpxchg dword ptr [rcx + 8], edx
//     jne  contended
//     ret
//   contended:
//     mov  eax, 0xFFFFFFFF
//     xchg dword ptr [rcx + 8], eax
//     jmp  kernel32!__vitreo_cs_leave_wake        ; rel32 at +0x3C
//   nested:
//     dec  dword ptr [rcx + 0xC]
//     ret
//   host:
//     jmp  kernel32!LeaveCriticalSection (host)   ; rel32 at +0x45
constexpr u8 kLeaveCriticalSection[] = {
    0xf6, 0xc1, 0x03,
    0x75, 0x3f,
    0x83, 0x79, 0x0c, 0x01,
    0x77, 0x35,
    0xc7, 0x41, 0x0c, 0x00, 0x00, 0x00, 0x00,
    0x48, 0xc7, 0x41, 0x10, 0x00, 0x00, 0x00, 0x00,
    0x48, 0x83, 0x79, 0x18, 0x00,
    0x75, 0x12,
    0xba, 0xff, 0xff, 0xff, 0xff,
    0xb8, 0xfe, 0xff, 0xff, 0xff,
    0xf0, 0x0f, 0xb1, 0x51, 0x08,
    0x75, 0x01,
    0xc3,
    0xb8, 0xff, 0xff, 0xff, 0xff,
    0x87, 0x41, 0x08,
    0xe9, 0x00, 0x00, 0x00, 0x00,
    0xff, 0x49, 0x0c,
    0xc3,
    0xe9, 0x00, 0x00, 0x00, 0x00,
};
constexpr usize kLeaveWakeRel32 = 0x3C;
constexpr usize kLeaveHostRel32 = 0x45;

struct X64Accel {
    std::initializer_list<const char*> names;  ///< every export it implements
    const u8* code;
    usize size;
    /// Tail-jumps into host exports: each rel32's offset in `code` and the
    /// export it must reach ("dll.dll!Name"); a null target ends the list.
    struct Extern {
        usize rel32 = 0;
        const char* target = nullptr;
    };
    Extern externs[2] = {};
};

const X64Accel kX64Accelerated[] = {
    {{"kernel32.dll!TlsGetValue", "kernelbase.dll!TlsGetValue"},
     kTlsGetValue, sizeof(kTlsGetValue)},
    {{"ucrtbase.dll!_finite", "msvcrt.dll!_finite"}, kFinite, sizeof(kFinite)},
    {{"ucrtbase.dll!_isnan", "msvcrt.dll!_isnan"}, kIsnan, sizeof(kIsnan)},
    {{"kernel32.dll!EnterCriticalSection", "kernelbase.dll!EnterCriticalSection",
      "ntdll.dll!RtlEnterCriticalSection"},
     kEnterCriticalSection, sizeof(kEnterCriticalSection),
     {{kEnterSlowRel32, "kernel32.dll!__vitreo_cs_enter_slow"},
      {kEnterHostRel32, "kernel32.dll!EnterCriticalSection"}}},
    {{"kernel32.dll!TryEnterCriticalSection",
      "kernelbase.dll!TryEnterCriticalSection",
      "ntdll.dll!RtlTryEnterCriticalSection"},
     kTryEnterCriticalSection, sizeof(kTryEnterCriticalSection),
     {{kTryEnterHostRel32, "kernel32.dll!TryEnterCriticalSection"}}},
    {{"kernel32.dll!LeaveCriticalSection", "kernelbase.dll!LeaveCriticalSection",
      "ntdll.dll!RtlLeaveCriticalSection"},
     kLeaveCriticalSection, sizeof(kLeaveCriticalSection),
     {{kLeaveWakeRel32, "kernel32.dll!__vitreo_cs_leave_wake"},
      {kLeaveHostRel32, "kernel32.dll!LeaveCriticalSection"}}},
};

/// The critical-section entries share one LockCount word with the host halves,
/// which reach it only under the linear memory model (lock_word), and the
/// whole protocol has one switch (VITREO_NO_CS_ACCEL).
bool is_critical_section_entry(const std::string& name) noexcept {
    return name.find("CriticalSection") != std::string::npos;
}

}  // namespace

bool ensure_x64_accel_arena(GuestMemory& memory, GuestAddr thunk_base) noexcept {
    const GuestAddr base = thunk_base + X64_ACCEL_OFFSET;
    if (memory.fetch_ptr(base, X64_ACCEL_SIZE) != nullptr) return true;
    return memory.map_region(base, X64_ACCEL_SIZE,
                             MemoryProtection::ReadExecute).is_ok();
}

CrtAccelBlob build_guest_x64_code(GuestAddr base) {
    CrtAccelBlob blob;
    for (const auto& fn : kX64Accelerated) {
        // 16-byte aligned entries, as a compiler would place functions.
        while (blob.code.size() % 16 != 0) blob.code.push_back(0xCC);
        const GuestAddr at = base + blob.code.size();
        for (const char* name : fn.names) {
            blob.entries.emplace_back(name, static_cast<u32>(at - base));
        }
        for (const X64Accel::Extern& x : fn.externs) {
            if (x.target == nullptr) break;
            blob.extern_fixups.emplace_back(blob.code.size() + x.rel32, x.target);
        }
        blob.code.insert(blob.code.end(), fn.code, fn.code + fn.size);
    }
    return blob;
}

usize install_guest_x64_accelerators(GuestMemory& memory,
                                     ThunkRegistry& registry) noexcept {
    // The checked CRT memory bodies' fault-resume record lives in this arena
    // whether or not the accelerators below are installed.
    (void)vcrt::publish_resume_record(memory, registry, /*x64=*/true);
    if (std::getenv("VITREO_NO_X64_ACCEL") != nullptr) return 0;
    const GuestAddr base = registry.thunk_base() + X64_ACCEL_OFFSET;
    CrtAccelBlob blob = build_guest_x64_code(base);
    if (blob.code.size() > X64_ACCEL_RESUME_OFFSET) return 0;
    // Each tail-jump must reach its host export, or the functions that use
    // it are not registered: a fast path whose slow half is a jump into
    // nothing is worse than the host thunk it replaces. The critical sections
    // also need the protocol on and the host's linear lock-word access.
    // VITREO_NO_CS_ACCEL is the protocol's one switch (critical_section.cpp
    // cs_accel_enabled; the x86 install reads it the same way).
    static const bool cs_accel = std::getenv("VITREO_NO_CS_ACCEL") == nullptr;
    bool cs_ok = cs_accel && memory.is_linear();
    for (const auto& [slot, target_name] : blob.extern_fixups) {
        const auto bang = target_name.find('!');
        auto target = registry.get_thunk_address(target_name.substr(0, bang),
                                                 target_name.substr(bang + 1),
                                                 /*include_internal=*/true);
        const i64 rel = target.is_ok()
                            ? static_cast<i64>(target.value()) -
                                  static_cast<i64>(base + slot + 4)
                            : 0;
        if (target.is_err() || rel < INT32_MIN || rel > INT32_MAX) {
            cs_ok = false;
            continue;
        }
        const i32 rel32 = static_cast<i32>(rel);
        std::memcpy(blob.code.data() + slot, &rel32, sizeof(rel32));
    }
    if (!ensure_x64_accel_arena(memory, registry.thunk_base())) return 0;
    const Byte* live = memory.fetch_ptr(base, blob.code.size());
    if (live == nullptr ||
        std::memcmp(live, blob.code.data(), blob.code.size()) != 0) {
        if (!write_accel_code(memory, base, blob.code.data(), blob.code.size())) {
            return 0;
        }
    }
    // VITREO_X64_ACCEL_ONLY=<substring>: register only the exports whose
    // "dll!name" contains it, to bisect a behaviour change to one function.
    const char* only = std::getenv("VITREO_X64_ACCEL_ONLY");
    usize redirected = 0;
    for (const auto& [name, offset] : blob.entries) {
        if (only != nullptr && name.find(only) == std::string::npos) continue;
        if (!cs_ok && is_critical_section_entry(name)) continue;
        const auto bang = name.find('!');
        auto thunk = registry.get_thunk_address(name.substr(0, bang),
                                                name.substr(bang + 1));
        if (thunk.is_err()) continue;
        registry.set_guest_implementation(thunk.value(), base + offset);
        ++redirected;
    }
    if (std::getenv("VITREO_LOG_CRT_ACCEL") != nullptr) {
        std::fprintf(stderr,
                     "[x64-accel] %zu exports run guest-side (%zu bytes at "
                     "0x%llx)\n",
                     redirected, blob.code.size(),
                     static_cast<unsigned long long>(base));
    }
    return redirected;
}

}  // namespace vitreo::winapi
