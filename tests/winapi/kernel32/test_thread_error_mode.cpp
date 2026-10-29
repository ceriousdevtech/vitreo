/// @file test_thread_error_mode.cpp
/// @brief SetErrorMode / GetErrorMode / SetThreadErrorMode / GetThreadErrorMode.
///
/// .NET's System.IO.DisableMediaInsertionPrompt wraps every managed file
/// attribute query in SetThreadErrorMode(SEM_FAILCRITICALERRORS, out old), so a
/// missing or fake implementation breaks File.Exists for the whole guest.
///
/// Contract (Microsoft Learn, errhandlingapi.h):
///   BOOL SetThreadErrorMode(DWORD dwNewMode, LPDWORD lpOldMode)
///     - settable bits: SEM_FAILCRITICALERRORS (0x0001),
///       SEM_NOGPFAULTERRORBOX (0x0002), SEM_NOOPENFILEERRORBOX (0x8000)
///     - SEM_NOALIGNMENTFAULTEXCEPT (0x0004) is process-wide only: passing it
///       fails with ERROR_INVALID_PARAMETER and leaves the mode unchanged
///     - lpOldMode is optional and receives the previous thread error mode
///   DWORD GetThreadErrorMode(void) — the calling thread's mode, never fails
///   UINT  GetErrorMode(void)       — the process mode, never fails
///   The thread mode defaults to the process mode set by SetErrorMode and,
///   once overridden, is per-thread: it does not change the process mode.

#include <gtest/gtest.h>

#include "core/cpu/state/cpu_state.hpp"
#include "core/memory/guest_memory.hpp"
#include "capsule/vfs/vfs.hpp"
#include "winapi/kernel32/kernel32_ext.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/core/marshaler.hpp"

#include <algorithm>
#include <thread>

using namespace vitreo;
using namespace vitreo::winapi;

namespace {

constexpr u32 SEM_FAILCRITICALERRORS     = 0x0001;
constexpr u32 SEM_NOGPFAULTERRORBOX      = 0x0002;
constexpr u32 SEM_NOALIGNMENTFAULTEXCEPT = 0x0004;
constexpr u32 SEM_NOOPENFILEERRORBOX     = 0x8000;

constexpr GuestAddr kBase = 0x1000;
constexpr GuestAddr kOldModeSlot = 0x2000;

const ExportEntry* find_export(const DllExports& e, std::string_view name) {
    auto it = std::find_if(e.entries.begin(), e.entries.end(),
                           [&](const ExportEntry& x) { return x.name == name; });
    return it == e.entries.end() ? nullptr : &*it;
}

struct Env {
    GuestMemory memory;
    capsule::Vfs vfs;
    Kernel32ExtManager mgr{memory, vfs};
    cpu::CpuState cpu;
    Marshaler m{memory};

    Env() {
        (void)memory.map_region(kBase, 0x10000, MemoryProtection::ReadWrite);
        cpu.init_long_mode();
        m.attach_cpu(cpu);
    }

    u32 set_thread_mode(u32 new_mode, GuestAddr old_ptr) {
        cpu.gpr.rcx = new_mode;
        cpu.gpr.rdx = old_ptr;
        mgr.impl_SetThreadErrorMode(m);
        return static_cast<u32>(cpu.gpr.rax);
    }
    u32 get_thread_mode() {
        mgr.impl_GetThreadErrorMode(m);
        return static_cast<u32>(cpu.gpr.rax);
    }
    u32 get_process_mode() {
        mgr.impl_GetErrorMode(m);
        return static_cast<u32>(cpu.gpr.rax);
    }
    u32 set_process_mode(u32 mode) {
        cpu.gpr.rcx = mode;
        mgr.impl_SetErrorMode(m);
        return static_cast<u32>(cpu.gpr.rax);
    }
};

} // namespace

TEST(ThreadErrorModeTest, ExportsRegisteredWithWindowsArity) {
    GuestMemory memory;
    capsule::Vfs vfs;
    Kernel32ExtManager mgr(memory, vfs);
    auto exports = create_kernel32_ext_thunks(mgr);

    const auto* set_thread = find_export(exports, "SetThreadErrorMode");
    ASSERT_NE(set_thread, nullptr);
    EXPECT_EQ(set_thread->stdcall_bytes, 8u);

    const auto* get_thread = find_export(exports, "GetThreadErrorMode");
    ASSERT_NE(get_thread, nullptr);
    EXPECT_EQ(get_thread->stdcall_bytes, 0u);

    const auto* get_process = find_export(exports, "GetErrorMode");
    ASSERT_NE(get_process, nullptr);
    EXPECT_EQ(get_process->stdcall_bytes, 0u);

    const auto* set_process = find_export(exports, "SetErrorMode");
    ASSERT_NE(set_process, nullptr);
    EXPECT_EQ(set_process->stdcall_bytes, 4u);
}

// The .NET DisableMediaInsertionPrompt sequence: set the failure-suppressing
// mode, observe the previous one, then restore it on dispose.
TEST(ThreadErrorModeTest, SetReturnsPreviousModeAndGetObservesIt) {
    // Fresh thread: the per-thread override starts unset.
    std::thread([] {
        Env env;
        EXPECT_EQ(env.get_thread_mode(), 0u);

        ASSERT_TRUE(env.memory.write_u32(kOldModeSlot, 0xDEADBEEFu).is_ok());
        EXPECT_EQ(env.set_thread_mode(SEM_FAILCRITICALERRORS, kOldModeSlot), 1u);
        EXPECT_EQ(env.memory.read_u32(kOldModeSlot).value_or(1u), 0u);
        EXPECT_EQ(env.get_thread_mode(), SEM_FAILCRITICALERRORS);

        // Restore: the previous value reported is the one just installed.
        EXPECT_EQ(env.set_thread_mode(SEM_NOOPENFILEERRORBOX, kOldModeSlot), 1u);
        EXPECT_EQ(env.memory.read_u32(kOldModeSlot).value_or(0u),
                  SEM_FAILCRITICALERRORS);
        EXPECT_EQ(env.get_thread_mode(), SEM_NOOPENFILEERRORBOX);
    }).join();
}

TEST(ThreadErrorModeTest, NullOldModePointerIsAccepted) {
    std::thread([] {
        Env env;
        EXPECT_EQ(env.set_thread_mode(
                      SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX, 0), 1u);
        EXPECT_EQ(env.get_thread_mode(),
                  SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    }).join();
}

TEST(ThreadErrorModeTest, AlignmentAndUndefinedBitsRejected) {
    std::thread([] {
        Env env;
        ASSERT_EQ(env.set_thread_mode(SEM_FAILCRITICALERRORS, 0), 1u);

        for (const u32 bad : {SEM_NOALIGNMENTFAULTEXCEPT,
                              SEM_FAILCRITICALERRORS |
                                  SEM_NOALIGNMENTFAULTEXCEPT,
                              0x1000u}) {
            set_last_error(0);
            ASSERT_TRUE(env.memory.write_u32(kOldModeSlot, 0x5A5A5A5Au).is_ok());
            EXPECT_EQ(env.set_thread_mode(bad, kOldModeSlot), 0u)
                << "mode=0x" << std::hex << bad;
            EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_PARAMETER);
            // Unchanged mode, and no out-parameter written on failure.
            EXPECT_EQ(env.get_thread_mode(), SEM_FAILCRITICALERRORS);
            EXPECT_EQ(env.memory.read_u32(kOldModeSlot).value_or(0u),
                      0x5A5A5A5Au);
        }
    }).join();
}

TEST(ThreadErrorModeTest, UnwritableOldModePointerFailsHonestly) {
    std::thread([] {
        Env env;
        set_last_error(0);
        // Unmapped guest address: no silently dropped store, no fake success.
        EXPECT_EQ(env.set_thread_mode(SEM_FAILCRITICALERRORS, 0x7F000000u), 0u);
        EXPECT_EQ(get_last_error(), error_codes::ERROR_NOACCESS);
        EXPECT_EQ(env.get_thread_mode(), 0u);
    }).join();
}

// The thread mode defaults to the process mode, and overriding it per thread
// leaves the process mode alone.
TEST(ThreadErrorModeTest, ThreadModeDefaultsToProcessModeAndDoesNotChangeIt) {
    std::thread([] {
        Env env;
        EXPECT_EQ(env.get_process_mode(), 0u);
        EXPECT_EQ(env.set_process_mode(SEM_NOGPFAULTERRORBOX), 0u);
        EXPECT_EQ(env.get_process_mode(), SEM_NOGPFAULTERRORBOX);
        EXPECT_EQ(env.get_thread_mode(), SEM_NOGPFAULTERRORBOX);

        EXPECT_EQ(env.set_thread_mode(SEM_FAILCRITICALERRORS, kOldModeSlot), 1u);
        EXPECT_EQ(env.memory.read_u32(kOldModeSlot).value_or(0u),
                  SEM_NOGPFAULTERRORBOX);
        EXPECT_EQ(env.get_thread_mode(), SEM_FAILCRITICALERRORS);
        EXPECT_EQ(env.get_process_mode(), SEM_NOGPFAULTERRORBOX);
    }).join();
}

// Until power assertions have a host implementation, failure must be explicit:
// a fabricated successful handle later makes a valid CloseHandle call fail.
TEST(PowerRequestExportsTest, UnsupportedRequestsNeverReturnSuccessfulHandles) {
    Env env;
    auto exports = create_kernel32_ext_thunks(env.mgr);
    const auto* create = find_export(exports, "PowerCreateRequest");
    ASSERT_NE(create, nullptr);
    create->handler(env.m);
    EXPECT_EQ(env.cpu.gpr.rax, static_cast<u64>(-1));
    EXPECT_EQ(get_last_error(), error_codes::ERROR_NOT_SUPPORTED);
    for (auto name : {"PowerSetRequest", "PowerClearRequest"}) {
        const auto* entry = find_export(exports, name);
        ASSERT_NE(entry, nullptr);
        set_last_error(0);
        entry->handler(env.m);
        EXPECT_EQ(env.cpu.gpr.rax, 0u);
        EXPECT_EQ(get_last_error(), error_codes::ERROR_NOT_SUPPORTED);
    }
}
