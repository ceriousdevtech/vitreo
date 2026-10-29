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
#include "winapi/core/thunk_registry.hpp"
#include "winapi/kernel32/thread/tls.hpp"
#include "core/memory/guest_memory.hpp"

#include <cstdio>
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

struct X64Accel {
    std::initializer_list<const char*> names;  ///< every export it implements
    const u8* code;
    usize size;
};

const X64Accel kX64Accelerated[] = {
    {{"kernel32.dll!TlsGetValue", "kernelbase.dll!TlsGetValue"},
     kTlsGetValue, sizeof(kTlsGetValue)},
    {{"ucrtbase.dll!_finite", "msvcrt.dll!_finite"}, kFinite, sizeof(kFinite)},
    {{"ucrtbase.dll!_isnan", "msvcrt.dll!_isnan"}, kIsnan, sizeof(kIsnan)},
};

bool g_x64_arena_ready = false;

}  // namespace

CrtAccelBlob build_guest_x64_code(GuestAddr base) {
    CrtAccelBlob blob;
    for (const auto& fn : kX64Accelerated) {
        // 16-byte aligned entries, as a compiler would place functions.
        while (blob.code.size() % 16 != 0) blob.code.push_back(0xCC);
        const GuestAddr at = base + blob.code.size();
        for (const char* name : fn.names) {
            blob.entries.emplace_back(name, static_cast<u32>(at - base));
        }
        blob.code.insert(blob.code.end(), fn.code, fn.code + fn.size);
    }
    return blob;
}

usize install_guest_x64_accelerators(GuestMemory& memory,
                                     ThunkRegistry& registry) noexcept {
    if (std::getenv("VITREO_NO_X64_ACCEL") != nullptr) return 0;
    const GuestAddr base = registry.thunk_base() + X64_ACCEL_OFFSET;
    const CrtAccelBlob blob = build_guest_x64_code(base);
    if (blob.code.size() > X64_ACCEL_SIZE) return 0;
    if (!g_x64_arena_ready) {
        if (memory.map_region(base, X64_ACCEL_SIZE,
                              MemoryProtection::ReadWrite).is_err()) {
            return 0;
        }
        if (memory.write_bytes(base, blob.code.data(), blob.code.size())
                .is_err()) {
            return 0;
        }
        // Readable as well as executable: hook validators read the first
        // bytes of a resolved function and query its page protection.
        (void)memory.protect_region(base, X64_ACCEL_SIZE,
                                    MemoryProtection::ReadExecute);
        g_x64_arena_ready = true;
    }
    // VITREO_X64_ACCEL_ONLY=<substring>: register only the exports whose
    // "dll!name" contains it, to bisect a behaviour change to one function.
    const char* only = std::getenv("VITREO_X64_ACCEL_ONLY");
    usize redirected = 0;
    for (const auto& [name, offset] : blob.entries) {
        if (only != nullptr && name.find(only) == std::string::npos) continue;
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
