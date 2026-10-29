/// @file test_enum_process_modules.cpp
/// @brief kernel32!K32EnumProcessModules — the process-wide module list.
///
/// Contract (Microsoft Learn, psapi.h / EnumProcessModules):
///   BOOL EnumProcessModules(HANDLE hProcess, HMODULE* lphModule, DWORD cb,
///                           LPDWORD lpcbNeeded)
///     - kernel32 exports the same function as K32EnumProcessModules
///     - lphModule receives HMODULEs, which are pointer-sized
///     - lpcbNeeded receives the number of BYTES required for the full list;
///       a caller compares it against cb to decide whether to grow its buffer
///     - the first handle is the module that started the process
///     - returns 0 on failure
///
/// This is not only a process-inspection API. A process-wide symbol search
/// resolves an exported function by enumerating every loaded module here and
/// calling GetProcAddress on each: Mono's mono_dl_lookup_symbol does exactly
/// that for its main-module MonoDl, which is the fallback path for a P/Invoke
/// whose library did not open by name. Reporting "success, zero modules" makes
/// every such lookup fail with EntryPointNotFoundException for an export that
/// GetProcAddress resolves perfectly well when asked with the right handle.

#include <gtest/gtest.h>

#include "core/cpu/state/cpu_state.hpp"
#include "core/memory/guest_memory.hpp"
#include "capsule/vfs/vfs.hpp"
#include "winapi/kernel32/kernel32_ext.hpp"
#include "winapi/kernel32/module/module.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/core/thunk_registry.hpp"

#include <algorithm>
#include <string>
#include <vector>

using namespace vitreo;
using namespace vitreo::winapi;

namespace {

constexpr GuestAddr kBuffer = 0x10000;
constexpr GuestAddr kNeeded = 0x11000;
constexpr GuestAddr kMainBase = 0x140000000ULL;

/// Runs the whole guest-observable resolution the way a process-wide symbol
/// search does: enumerate modules, then GetProcAddress each one in turn.
struct Env {
    GuestMemory memory;
    capsule::Vfs vfs;
    ThunkRegistry registry{0x00007FFB'00000000ULL};
    ModuleManager modules{registry};
    Kernel32ExtManager mgr{memory, vfs};
    cpu::CpuState cpu;
    Marshaler m{memory};

    Env() {
        (void)memory.map_region(kBuffer, 0x10000, MemoryProtection::ReadWrite);
        cpu.init_long_mode();
        m.attach_cpu(cpu);
        mgr.set_module_manager(&modules);

        modules.attach_dynamic_loader(memory, vfs, /*is_64bit=*/true);
        modules.set_main_module(kMainBase, "C:\\game\\game.exe", 0, 0);

        DllExports kernel32;
        kernel32.entries.push_back(
            ExportEntry{"SetThreadErrorMode", 0, [](Marshaler&) {}, 8});
        EXPECT_TRUE(
            registry.register_dll("kernel32.dll", std::move(kernel32)).is_ok());
        modules.register_module("kernel32.dll", ModuleManager::KERNEL32_BASE,
                                "C:\\Windows\\System32\\kernel32.dll");
    }

    /// Returns lpcbNeeded; `rc` receives the BOOL.
    u32 enumerate(GuestAddr buffer, u32 cb, GuestAddr needed, u32& rc) {
        cpu.gpr.rcx = 0xFFFFFFFFFFFFFFFFULL; // GetCurrentProcess() pseudo-handle
        cpu.gpr.rdx = buffer;
        cpu.gpr.r8 = cb;
        cpu.gpr.r9 = needed;
        mgr.impl_K32EnumProcessModules(m);
        rc = static_cast<u32>(cpu.gpr.rax);
        return needed ? memory.read_u32(needed).value_or(0xFFFFFFFFu) : 0u;
    }
};

} // namespace

TEST(EnumProcessModulesTest, ExportRegisteredWithWindowsArity) {
    GuestMemory memory;
    capsule::Vfs vfs;
    Kernel32ExtManager mgr(memory, vfs);
    auto exports = create_kernel32_ext_thunks(mgr);
    auto it = std::find_if(
        exports.entries.begin(), exports.entries.end(),
        [](const ExportEntry& e) { return e.name == "K32EnumProcessModules"; });
    ASSERT_NE(it, exports.entries.end());
    EXPECT_EQ(it->stdcall_bytes, 16u);
}

TEST(EnumProcessModulesTest, ReportsEveryLoadedModuleWithMainImageFirst) {
    Env env;
    u32 rc = 0;
    const u32 needed = env.enumerate(kBuffer, 0x1000, kNeeded, rc);
    EXPECT_EQ(rc, 1u);

    // Pointer-sized handles: a DWORD stride would report half the bytes and
    // leave every second slot holding a stale high half.
    ASSERT_GE(needed, 2u * sizeof(u64));
    EXPECT_EQ(needed % sizeof(u64), 0u);

    std::vector<GuestAddr> handles;
    for (u32 off = 0; off < needed; off += sizeof(u64)) {
        handles.push_back(
            static_cast<GuestAddr>(env.memory.read_u64(kBuffer + off).value()));
    }
    EXPECT_EQ(handles.front(), kMainBase);
    EXPECT_NE(std::find(handles.begin(), handles.end(),
                        ModuleManager::KERNEL32_BASE),
              handles.end());
}

// The regression this file exists for: the export is resolvable the way the
// guest actually finds it -- not merely registered in the thunk table.
TEST(EnumProcessModulesTest, ProcessWideSymbolSearchFindsKernel32Export) {
    Env env;

    // A process-wide search starts at the main module, which does not export
    // it -- exactly what Mono observes before it widens the search.
    ASSERT_EQ(env.modules.get_proc_address(kMainBase, "SetThreadErrorMode"),
              0u);

    u32 rc = 0;
    const u32 needed = env.enumerate(kBuffer, 0x1000, kNeeded, rc);
    ASSERT_EQ(rc, 1u);
    ASSERT_GT(needed, 0u);

    GuestAddr resolved = 0;
    for (u32 off = 0; off < needed && resolved == 0; off += sizeof(u64)) {
        const auto handle =
            static_cast<GuestAddr>(env.memory.read_u64(kBuffer + off).value());
        resolved = env.modules.get_proc_address(handle, "SetThreadErrorMode");
    }
    EXPECT_EQ(resolved,
              env.registry.get_thunk_address("kernel32.dll",
                                             "SetThreadErrorMode").value());
}

TEST(EnumProcessModulesTest, SmallBufferStillReportsFullByteCount) {
    Env env;
    u32 rc = 0;
    const u32 full = env.enumerate(kBuffer, 0x1000, kNeeded, rc);
    ASSERT_EQ(rc, 1u);
    ASSERT_GT(full, sizeof(u64));

    // One handle's worth of room: Windows still succeeds and still reports the
    // full requirement, which is how a caller knows to reallocate.
    ASSERT_TRUE(env.memory.write_u64(kBuffer + sizeof(u64), 0xA5A5A5A5A5A5A5A5ULL)
                    .is_ok());
    const u32 needed = env.enumerate(kBuffer, sizeof(u64), kNeeded, rc);
    EXPECT_EQ(rc, 1u);
    EXPECT_EQ(needed, full);
    EXPECT_EQ(env.memory.read_u64(kBuffer).value(), kMainBase);
    // Nothing past the caller's buffer may be written.
    EXPECT_EQ(env.memory.read_u64(kBuffer + sizeof(u64)).value(),
              0xA5A5A5A5A5A5A5A5ULL);
}

TEST(EnumProcessModulesTest, NullNeededFails) {
    Env env;
    u32 rc = 0;
    (void)env.enumerate(kBuffer, 0x1000, 0, rc);
    EXPECT_EQ(rc, 0u);
}
