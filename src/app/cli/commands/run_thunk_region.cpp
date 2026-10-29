// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// CLI-002: thunk region mapping and the API-return stubs

#include "run_internal.hpp"
#include "runtime/process/runtime_context.hpp"
#include "core/memory/guest_memory.hpp"
#include "core/pe/parser/pe_parser.hpp"
#include "winapi/core/thunk_registry.hpp"
#include "winapi/kernel32/thread/thread_suspend.hpp"
#include "winapi/msvcrt/guest_accel.hpp"

#include <algorithm>
#include <iterator>
#include <vector>

namespace vitreo::cli {

namespace {

// Return instructions for APIs, one per stdcall byte count (x64: `ret`,
// x86: `ret N` at offset N), at the top of the thunk mapping. See
// ThreadSuspendRegistry::set_api_return_resolver.
const vitreo::winapi::ThunkRegistry* g_api_return_registry = nullptr;
vitreo::GuestAddr g_api_return_stubs = 0;
/// Windows reports a thread blocked in a host-backed wait inside ntdll's
/// system-service stub. Prefer a ret that lives in the synthetic ntdll image
/// so GetThreadContext's RIP is in SizeOfImage (thread_lifecycle_probe
/// BLOCKED_GET ip_in_ntdll).
vitreo::GuestAddr g_ntdll_blocked_rip = 0;
constexpr vitreo::u32 kApiReturnStubBytes = 0x104;  // N = 0..256, 4 bytes each

vitreo::GuestAddr api_return_for_gateway(vitreo::GuestAddr gateway,
                                         bool is_64bit) {
    const auto* registry = g_api_return_registry;
    if (registry == nullptr ||
        !registry->is_dispatch_address(gateway)) {
        return 0;
    }
    if (g_ntdll_blocked_rip != 0) {
        return g_ntdll_blocked_rip;
    }
    if (g_api_return_stubs == 0) return 0;
    if (is_64bit) return g_api_return_stubs;
    const vitreo::u16 bytes = registry->resolve_stdcall_bytes(gateway);
    if ((bytes & 3u) != 0 || bytes >= kApiReturnStubBytes) return 0;
    return g_api_return_stubs + bytes;
}

} // namespace

void map_thunk_region(GuestMemory& memory, RuntimeContext& ctx,
                      const pe::ParsedPe& pe) {
    // Back the thunk region with executable, readable guest memory. Thunk addresses
    // contain an 8-byte NOP entry stub followed by an internal
    // host-dispatch gateway. This gives hook engines real instructions to copy
    // or patch while preserving a path from their trampoline to the original
    // host handler. Guest code that *reads* a resolved
    // import's bytes — a browser helper does inline-hook detection
    // (`cmp byte ptr [func], 0xEB`) on GetProcAddress'd functions — would
    // fault on the unmapped region and spin in its relaunch loop. Map it
    // readable (zero-filled: 0x00 != 0xEB/0xE9, so the hook check reads "not
    // hooked"). Report it executable because these addresses are guest-visible
    // function pointers and Windows hook validators query their page protection.
    // Generous fixed size covers lazily-allocated missing-export thunks too.
    constexpr u64 THUNK_REGION_MAP_SIZE = 0x200000;  // 2 MB (131072 thunks)
    auto tr = memory.map_region(
        static_cast<GuestAddr>(ctx.thunk_registry.thunk_base()),
        static_cast<usize>(THUNK_REGION_MAP_SIZE),
        MemoryProtection::ReadWrite);
    if (tr.is_ok()) {
        std::vector<Byte> thunk_bytes(THUNK_REGION_MAP_SIZE, 0xCC);
        static constexpr Byte NOP8[] = {
            0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90
        };
        for (usize offset = 0; offset < THUNK_REGION_MAP_SIZE; offset += 16) {
            std::copy(std::begin(NOP8), std::end(NOP8),
                      thunk_bytes.begin() + static_cast<std::ptrdiff_t>(offset));
        }
        // API return instructions (`ret` / `ret N`) for threads reported
        // as blocked inside an API; see api_return_for_gateway.
        const usize stubs = THUNK_REGION_MAP_SIZE - 0x200;
        for (u32 n = 0; n < kApiReturnStubBytes; n += 4) {
            Byte* stub = thunk_bytes.data() + stubs + n;
            if (n == 0) {
                stub[0] = 0xC3;
            } else {
                stub[0] = 0xC2;
                stub[1] = static_cast<Byte>(n & 0xFF);
                stub[2] = static_cast<Byte>(n >> 8);
                stub[3] = 0xCC;
            }
        }
        g_api_return_stubs = ctx.thunk_registry.thunk_base() + stubs;
        g_api_return_registry = &ctx.thunk_registry;
        // Prefer a RIP inside synthetic ntdll (Windows system-service stub
        // range). RtlUserThreadStart's in-image veneer is always present.
        {
            const GuestAddr ntdll =
                ctx.module_mgr.get_module_handle("ntdll.dll");
            g_ntdll_blocked_rip = ntdll != 0
                ? ctx.module_mgr.get_proc_address(ntdll, "RtlUserThreadStart")
                : 0;
        }
        winapi::ThreadSuspendRegistry::instance().set_api_return_resolver(
            &api_return_for_gateway);
        (void)memory.write_bytes(ctx.thunk_registry.thunk_base(),
                                 thunk_bytes.data(), thunk_bytes.size());
        (void)memory.protect_region(ctx.thunk_registry.thunk_base(),
                                    THUNK_REGION_MAP_SIZE,
                                    MemoryProtection::ReadExecute);
    }
    // Redirect the hot CRT primitives to guest-side code now that the
    // sleds exist. This sits here rather than in native32_prepare so both
    // execution paths get it: the JIT pays a host transition per memset
    // exactly as compatibility mode pays a mode switch, and the sled patch
    // is honoured by both -- chain_thunk_call breaks out to the outer
    // interpreter for a "patched entry stub" instead of dispatching by
    // address.
    //
    // 32-bit guests ONLY. guest_accel.cpp emits 32-bit x86 and reads the
    // TEB through fs (`mov eax, fs:[0x24]`, `mov eax, fs:[0]`), which is
    // the 32-bit TEB layout -- x64 puts the TEB at gs:[0x30] and decodes
    // those opcodes differently. Patching the `jmp rel32` sled into a
    // 64-bit process sent EnterCriticalSection off into the 0xCC fill and
    // killed a 64-bit installer helper 731 instructions in, which
    // surfaced as "-5009 : 0x8000ffff" from the title's installer.
    if (!pe.is_64bit) {
        (void)winapi::install_guest_crt_accelerators(memory,
                                                     ctx.thunk_registry);
    }
}

} // namespace vitreo::cli
