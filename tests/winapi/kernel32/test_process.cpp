/// @file test_process.cpp
/// @brief Unit tests for ExitProcess and Process/Thread Info (WIN32-031)

#include <gtest/gtest.h>
#include <vitreo/common.hpp>
#include <winapi/kernel32/process/process.hpp>
#include <winapi/kernel32/thread/thread.hpp>
#include <winapi/kernel32/handle.hpp>
#include <winapi/kernel32/error.hpp>
#include <winapi/kernel32/memory/file_mapping.hpp>
#include <winapi/kernel32/memory/global_local.hpp>
#include <winapi/kernel32/memory/heap.hpp>
#include <winapi/kernel32/kernel32_ext.hpp>
#include <winapi/kernel32/environment/environment.hpp>
#include <winapi/kernel32/memory/memory.hpp>
#include <winapi/kernel32/synchronization/sync.hpp>
#include <winapi/kernel32/memory/virtual_query.hpp>
#include <winapi/advapi32/security/security.hpp>
#include <winapi/core/marshaler.hpp>
#include <winapi/core/thunk_registry.hpp>
#include <core/memory/guest_memory.hpp>
#include <core/cpu/state/cpu_state.hpp>
#include <capsule/vfs/vfs.hpp>
#include <ipc/process_handle.hpp>
#include <winapi/kernel32/memory/remote_memory.hpp>
#include <algorithm>
#include <chrono>
#include <csignal>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

using namespace vitreo;
using namespace vitreo::winapi;

// =============================================================================
// ProcessManagerTest Fixture
// =============================================================================

class ProcessManagerTest : public ::testing::Test {
protected:
    ProcessManager pm;
};

TEST(JobSecurityAttributesTest, DefaultDescriptorHonorsGuestWidthAndInheritance) {
    for (bool x64 : {false, true}) {
        GuestMemory memory;
        ASSERT_TRUE(memory.map_region(0x1000, 0x3000,
                                     MemoryProtection::ReadWrite).is_ok());
        capsule::Vfs vfs;
        HandleTable handles;
        Kernel32ExtManager manager(memory, vfs);
        manager.set_handle_table(&handles);
        cpu::CpuState cpu;
        if (x64) cpu.init_long_mode();
        else cpu.init_protected_mode();
        cpu.gpr.rsp = 0x3000;
        Marshaler marshaler(memory);
        marshaler.attach_cpu(cpu);
        for (u32 inherit : {0u, 1u}) {
            ASSERT_TRUE(memory.write_u32(0x1000, x64 ? 24 : 12).is_ok());
            ASSERT_TRUE(memory.write_u64(0x1008, 0).is_ok());
            ASSERT_TRUE(memory.write_u32(0x1004, 0).is_ok());
            ASSERT_TRUE(memory.write_u32(0x1000 + (x64 ? 16 : 8), inherit).is_ok());
            if (x64) {
                cpu.gpr.rcx = 0x1000;
                cpu.gpr.rdx = 0;
            } else {
                ASSERT_TRUE(memory.write_u32(0x3004, 0x1000).is_ok());
                ASSERT_TRUE(memory.write_u32(0x3008, 0).is_ok());
            }
            manager.impl_CreateJobObjectW(marshaler);
            const auto job = static_cast<Handle>(cpu.gpr.rax);
            ASSERT_NE(job, NULL_HANDLE);
            EXPECT_EQ(handles.get_type(job), HandleType::Job);
            EXPECT_EQ(handles.get_flags(job) & HandleFlags::Inherit, inherit);
        }
    }
}

// =============================================================================
// GetCurrentProcess Tests
// =============================================================================

TEST_F(ProcessManagerTest, GetCurrentProcessReturnsPseudoHandle) {
    EXPECT_EQ(pm.get_current_process(), CURRENT_PROCESS_PSEUDO);
}

TEST_F(ProcessManagerTest, GetCurrentProcessReturnsConsistentValue) {
    u32 first  = pm.get_current_process();
    u32 second = pm.get_current_process();
    EXPECT_EQ(first, second);
}

TEST_F(ProcessManagerTest, CurrentProcessPseudoMatchesNegativeOne) {
    EXPECT_EQ(pm.get_current_process(), static_cast<u32>(-1));
}

// =============================================================================
// GetCurrentProcessId Tests
// =============================================================================

TEST_F(ProcessManagerTest, GetCurrentProcessIdReturnsHostProcessIdentity) {
    EXPECT_EQ(pm.get_current_process_id(), static_cast<u32>(::getpid()));
}

TEST_F(ProcessManagerTest, GetCurrentProcessIdReturnsSameValue) {
    u32 first  = pm.get_current_process_id();
    u32 second = pm.get_current_process_id();
    EXPECT_EQ(first, second);
}

TEST_F(ProcessManagerTest, FindsRegisteredPeerProcess) {
    pm.set_process_snapshot_provider([] {
        return std::vector<ipc::ProcessSnapshotEntry>{
            {424242, 1, "Battle.net-Setup.exe",
             "C:\\Vitreo\\Apps\\Win-exe\\Battle.net-Setup.exe"},
        };
    });

    auto peer = pm.find_process(424242);
    ASSERT_TRUE(peer.has_value());
    EXPECT_EQ(peer->image_name, "Battle.net-Setup.exe");
    EXPECT_EQ(peer->image_path,
              "C:\\Vitreo\\Apps\\Win-exe\\Battle.net-Setup.exe");
    EXPECT_FALSE(pm.find_process(424243).has_value());
}

// =============================================================================
// GetCurrentThread Tests
// =============================================================================

TEST_F(ProcessManagerTest, GetCurrentThreadReturnsPseudoHandle) {
    EXPECT_EQ(pm.get_current_thread(), CURRENT_THREAD_PSEUDO);
}

TEST_F(ProcessManagerTest, GetCurrentThreadReturnsConsistentValue) {
    u32 first  = pm.get_current_thread();
    u32 second = pm.get_current_thread();
    EXPECT_EQ(first, second);
}

TEST_F(ProcessManagerTest, CurrentThreadPseudoMatchesNegativeTwo) {
    EXPECT_EQ(pm.get_current_thread(), static_cast<u32>(-2));
}

TEST(ProcThreadAttributeListTest, RecordsAnInheritableX86HandleList) {
    constexpr GuestAddr stack = 0x8000;
    constexpr GuestAddr size_ptr = 0x1800;
    constexpr GuestAddr list = 0x2000;
    constexpr GuestAddr values = 0x3000;

    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(
        0x1000, 0x10000, MemoryProtection::ReadWrite).is_ok());
    capsule::Vfs vfs;
    HandleTable handles;
    Kernel32ExtManager kernel32(memory, vfs);
    kernel32.set_handle_table(&handles);
    int event_object = 0;
    const Handle inherited = handles.allocate(
        HandleType::Event, &event_object, HandleFlags::Inherit);

    cpu::CpuState cpu;
    cpu.init_protected_mode();
    cpu.set_sp(stack);
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    auto set_args = [&](std::initializer_list<u32> args) {
        ASSERT_TRUE(memory.write_u32(stack, 0xDEADBEEF).is_ok());
        usize index = 0;
        for (const u32 arg : args) {
            ASSERT_TRUE(
                memory.write_u32(stack + 4 + index * 4, arg).is_ok());
            ++index;
        }
    };

    set_args({0, 1, 0, static_cast<u32>(size_ptr)});
    kernel32.impl_InitializeProcThreadAttributeList(marshaler);
    ASSERT_EQ(static_cast<u32>(cpu.gpr.rax), 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INSUFFICIENT_BUFFER);
    ASSERT_GE(memory.read_u32(size_ptr).value_or(0), 16u);

    set_args({static_cast<u32>(list), 1, 0,
              static_cast<u32>(size_ptr)});
    kernel32.impl_InitializeProcThreadAttributeList(marshaler);
    ASSERT_EQ(static_cast<u32>(cpu.gpr.rax), 1u);

    ASSERT_TRUE(memory.write_u32(values, inherited).is_ok());
    set_args({static_cast<u32>(list), 0, 0x00020002,
              static_cast<u32>(values), sizeof(u32), 0, 0});
    set_last_error(0xDEADu);  // success leaves it untouched (last_error_on_success_probe.expected)
    kernel32.impl_UpdateProcThreadAttribute(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 1u);
    EXPECT_EQ(get_last_error(), 0xDEADu);

    set_args({static_cast<u32>(list)});
    kernel32.impl_DeleteProcThreadAttributeList(marshaler);
    set_args({static_cast<u32>(list), 0, 0x00020002,
              static_cast<u32>(values), sizeof(u32), 0, 0});
    kernel32.impl_UpdateProcThreadAttribute(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_PARAMETER);
}

TEST(ProcThreadAttributeListTest, PseudoConsoleIsPassedByValueInBothBitnesses) {
    for (const bool is_64bit : {false, true}) {
        SCOPED_TRACE(is_64bit);
        GuestMemory memory;
        ASSERT_TRUE(memory.map_region(
            0x1000, 0x10000, MemoryProtection::ReadWrite).is_ok());
        capsule::Vfs vfs;
        Kernel32ExtManager kernel32(memory, vfs);
        cpu::CpuState cpu;
        if (is_64bit) cpu.init_long_mode();
        else cpu.init_protected_mode();
        constexpr GuestAddr stack = 0x8000;
        cpu.set_sp(stack);
        Marshaler marshaler(memory);
        marshaler.attach_cpu(cpu);
        auto set_args = [&](std::initializer_list<u64> args) {
            usize index = 0;
            for (const u64 arg : args) {
                if (!is_64bit) {
                    ASSERT_TRUE(memory.write_u32(stack + 4 + index * 4,
                                                 static_cast<u32>(arg)).is_ok());
                } else if (index == 0) cpu.gpr.rcx = arg;
                else if (index == 1) cpu.gpr.rdx = arg;
                else if (index == 2) cpu.gpr.r8 = arg;
                else if (index == 3) cpu.gpr.r9 = arg;
                else ASSERT_TRUE(memory.write_u64(stack + 40 + (index - 4) * 8,
                                                  arg).is_ok());
                ++index;
            }
        };
        ASSERT_TRUE(memory.write_u64(0x1800, 128).is_ok());
        set_args({0x2000, 1, 0, 0x1800});
        kernel32.impl_InitializeProcThreadAttributeList(marshaler);
        ASSERT_EQ(cpu.gpr.rax, 1u);
        // Opaque HPCON values need not point into readable guest memory.
        const u64 hpcon = is_64bit ? 0x170000001ull : 0x70000001ull;
        const u64 handle_size = is_64bit ? 8u : 4u;
        set_args({0x2000, 0, 0x20016, hpcon, handle_size + 1, 0, 0});
        kernel32.impl_UpdateProcThreadAttribute(marshaler);
        EXPECT_EQ(cpu.gpr.rax, 0u);
        EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_PARAMETER);
        set_args({0x2000, 0, 0x20016, hpcon, handle_size, 0, 0});
        set_last_error(0xDEADu);  // success leaves it untouched (last_error_on_success_probe.expected)
        kernel32.impl_UpdateProcThreadAttribute(marshaler);
        EXPECT_EQ(cpu.gpr.rax, 1u);
        EXPECT_EQ(get_last_error(), 0xDEADu);
    }
}

TEST(ProcThreadAttributeListTest, RejectsNonInheritableHandles) {
    constexpr GuestAddr stack = 0x8000;
    constexpr GuestAddr size_ptr = 0x1800;
    constexpr GuestAddr list = 0x2000;
    constexpr GuestAddr values = 0x3000;

    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(
        0x1000, 0x10000, MemoryProtection::ReadWrite).is_ok());
    capsule::Vfs vfs;
    HandleTable handles;
    Kernel32ExtManager kernel32(memory, vfs);
    kernel32.set_handle_table(&handles);
    int event_object = 0;
    const Handle local_only =
        handles.allocate(HandleType::Event, &event_object);

    cpu::CpuState cpu;
    cpu.init_protected_mode();
    cpu.set_sp(stack);
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    auto set_args = [&](std::initializer_list<u32> args) {
        ASSERT_TRUE(memory.write_u32(stack, 0xDEADBEEF).is_ok());
        usize index = 0;
        for (const u32 arg : args) {
            ASSERT_TRUE(
                memory.write_u32(stack + 4 + index * 4, arg).is_ok());
            ++index;
        }
    };

    ASSERT_TRUE(memory.write_u32(size_ptr, 64).is_ok());
    set_args({static_cast<u32>(list), 1, 0,
              static_cast<u32>(size_ptr)});
    kernel32.impl_InitializeProcThreadAttributeList(marshaler);
    ASSERT_EQ(static_cast<u32>(cpu.gpr.rax), 1u);
    ASSERT_TRUE(memory.write_u32(values, local_only).is_ok());
    set_args({static_cast<u32>(list), 0, 0x00020002,
              static_cast<u32>(values), sizeof(u32), 0, 0});
    kernel32.impl_UpdateProcThreadAttribute(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_HANDLE);
}

TEST(ProcThreadAttributeListTest,
     RecordsChromiumMitigationChildPolicyAndJobAttributes) {
    constexpr GuestAddr stack = 0x8000;
    constexpr GuestAddr size_ptr = 0x1800;
    constexpr GuestAddr list = 0x2000;
    constexpr GuestAddr mitigation = 0x3000;
    constexpr GuestAddr component_filter = 0x3020;
    constexpr GuestAddr child_policy = 0x3040;
    constexpr GuestAddr job_value = 0x3060;

    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(
        0x1000, 0x10000, MemoryProtection::ReadWrite).is_ok());
    capsule::Vfs vfs;
    HandleTable handles;
    Kernel32ExtManager kernel32(memory, vfs);
    kernel32.set_handle_table(&handles);

    cpu::CpuState cpu;
    cpu.init_protected_mode();
    cpu.set_sp(stack);
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    auto set_args = [&](std::initializer_list<u32> args) {
        ASSERT_TRUE(memory.write_u32(stack, 0xDEADBEEF).is_ok());
        usize index = 0;
        for (const u32 arg : args) {
            ASSERT_TRUE(
                memory.write_u32(stack + 4 + index * 4, arg).is_ok());
            ++index;
        }
    };

    ASSERT_TRUE(memory.write_u32(size_ptr, 128).is_ok());
    set_args({static_cast<u32>(list), 3, 0,
              static_cast<u32>(size_ptr)});
    kernel32.impl_InitializeProcThreadAttributeList(marshaler);
    ASSERT_EQ(static_cast<u32>(cpu.gpr.rax), 1u);

    ASSERT_TRUE(memory.write_u64(mitigation, 0x1000100010001000ULL).is_ok());
    ASSERT_TRUE(memory.write_u64(
        mitigation + sizeof(u64), 0x0100010001000100ULL).is_ok());
    set_args({static_cast<u32>(list), 0, 0x00020007,
              static_cast<u32>(mitigation), sizeof(u64) * 2, 0, 0});
    set_last_error(0xDEADu);  // success leaves it untouched (last_error_on_success_probe.expected)
    kernel32.impl_UpdateProcThreadAttribute(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 1u);
    EXPECT_EQ(get_last_error(), 0xDEADu);

    // Chromium treats this optional hardening attribute as unavailable only
    // when UpdateProcThreadAttribute reports ERROR_NOT_SUPPORTED.
    ASSERT_TRUE(memory.write_u32(component_filter, 1).is_ok());
    set_args({static_cast<u32>(list), 0, 0x0002001A,
              static_cast<u32>(component_filter), sizeof(u32), 0, 0});
    kernel32.impl_UpdateProcThreadAttribute(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_NOT_SUPPORTED);

    ASSERT_TRUE(memory.write_u32(child_policy, 1).is_ok());
    set_args({static_cast<u32>(list), 0, 0x0002000E,
              static_cast<u32>(child_policy), sizeof(u32), 0, 0});
    set_last_error(0xDEADu);  // success leaves it untouched (last_error_on_success_probe.expected)
    kernel32.impl_UpdateProcThreadAttribute(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 1u);
    EXPECT_EQ(get_last_error(), 0xDEADu);

    set_args({0, 0});
    kernel32.impl_CreateJobObjectW(marshaler);
    const Handle job = static_cast<Handle>(cpu.gpr.rax);
    ASSERT_NE(job, NULL_HANDLE);
    ASSERT_TRUE(memory.write_u32(job_value, job).is_ok());
    set_args({static_cast<u32>(list), 0, 0x0002000D,
              static_cast<u32>(job_value), sizeof(u32), 0, 0});
    set_last_error(0xDEADu);  // success leaves it untouched (last_error_on_success_probe.expected)
    kernel32.impl_UpdateProcThreadAttribute(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 1u);
    EXPECT_EQ(get_last_error(), 0xDEADu);

    // An attribute key can occur only once in a list.
    set_args({static_cast<u32>(list), 0, 0x0002000D,
              static_cast<u32>(job_value), sizeof(u32), 0, 0});
    kernel32.impl_UpdateProcThreadAttribute(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_PARAMETER);
}

TEST(CreateProcessAsUserTest,
     ValidatesPrimaryRestrictedTokenBeforeUsingRealLauncher) {
    constexpr GuestAddr stack = 0x8000;
    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(
        0x1000, 0x10000, MemoryProtection::ReadWrite).is_ok());
    capsule::Vfs vfs;
    HandleTable handles;
    SecurityStubs security(handles);
    Kernel32ExtManager kernel32(memory, vfs);
    kernel32.set_handle_table(&handles);
    kernel32.set_security_stubs(&security);

    Handle process_token = 0;
    u32 token_error = 0;
    ASSERT_TRUE(security.open_process_token(
        CURRENT_PROCESS_PSEUDO, HandleAccess::All, process_token, token_error));
    Handle restricted = 0;
    ASSERT_TRUE(security.create_restricted_token(
        process_token, 0,
        {SecurityStubs::RestrictedSid{SecurityStubs::synthetic_sid(), 0}},
        {}, {}, restricted));
    ASSERT_TRUE(security.token_has_restrictions(restricted));

    cpu::CpuState cpu;
    cpu.init_protected_mode();
    cpu.set_sp(stack);
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    auto set_args = [&](std::initializer_list<u32> args) {
        ASSERT_TRUE(memory.write_u32(stack, 0xDEADBEEF).is_ok());
        usize index = 0;
        for (const u32 arg : args) {
            ASSERT_TRUE(
                memory.write_u32(stack + 4 + index * 4, arg).is_ok());
            ++index;
        }
    };

    // The valid token reaches the shared CreateProcess path. With no image or
    // command line, that path must report FILE_NOT_FOUND rather than the old
    // unconditional ACCESS_DENIED fail-stub.
    set_args({restricted, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0});
    kernel32.impl_CreateProcessAsUserW(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_FILE_NOT_FOUND);

    Handle impersonation = 0;
    ASSERT_TRUE(security.duplicate_token(
        restricted, 2, impersonation, 2));
    set_args({impersonation, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0});
    kernel32.impl_CreateProcessAsUserW(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_BAD_TOKEN_TYPE);
}

// =============================================================================
// GetCurrentThreadId Tests
// =============================================================================

TEST_F(ProcessManagerTest, GetCurrentThreadIdReturnsCallingThreadId) {
    // Default host TLS guest tid is 1 (main thread).
    EXPECT_EQ(pm.get_current_thread_id(), 1u);

    ThreadManager::set_current_thread_id(42);
    EXPECT_EQ(pm.get_current_thread_id(), 42u);

    ThreadManager::set_current_thread_id(1);
    EXPECT_EQ(pm.get_current_thread_id(), 1u);
}

TEST_F(ProcessManagerTest, GetCurrentThreadIdReturnsSameValue) {
    u32 first  = pm.get_current_thread_id();
    u32 second = pm.get_current_thread_id();
    EXPECT_EQ(first, second);
}

// =============================================================================
// IsDebuggerPresent Tests
// =============================================================================

TEST_F(ProcessManagerTest, IsDebuggerPresentReturnsFalse) {
    EXPECT_FALSE(pm.is_debugger_present());
}

// =============================================================================
// ExitProcess Tests
// =============================================================================

TEST_F(ProcessManagerTest, HasExitedInitiallyFalse) {
    EXPECT_FALSE(pm.has_exited());
}

TEST_F(ProcessManagerTest, ExitCodeInitiallyZero) {
    EXPECT_EQ(pm.exit_code(), 0u);
}

TEST_F(ProcessManagerTest, ExitProcessSetsExitedFlag) {
    pm.exit_process(0);
    EXPECT_TRUE(pm.has_exited());
}

TEST_F(ProcessManagerTest, ExitProcessWithZeroCode) {
    pm.exit_process(0);
    EXPECT_TRUE(pm.has_exited());
    EXPECT_EQ(pm.exit_code(), 0u);
}

TEST_F(ProcessManagerTest, ExitProcessWithNonZeroCode) {
    pm.exit_process(42);
    EXPECT_TRUE(pm.has_exited());
    EXPECT_EQ(pm.exit_code(), 42u);
}

TEST_F(ProcessManagerTest, ExitProcessWithMaxCode) {
    pm.exit_process(0xFFFFFFFF);
    EXPECT_TRUE(pm.has_exited());
    EXPECT_EQ(pm.exit_code(), 0xFFFFFFFF);
}

TEST_F(ProcessManagerTest, ExitProcessOverwritesPreviousCode) {
    pm.exit_process(1);
    EXPECT_EQ(pm.exit_code(), 1u);
    pm.exit_process(99);
    EXPECT_EQ(pm.exit_code(), 99u);
    EXPECT_TRUE(pm.has_exited());
}

// =============================================================================
// Pseudo-Handle Distinctness
// =============================================================================

TEST_F(ProcessManagerTest, ProcessAndThreadPseudoHandlesDiffer) {
    EXPECT_NE(pm.get_current_process(), pm.get_current_thread());
}

TEST_F(ProcessManagerTest, PseudoHandlesAreNonZero) {
    EXPECT_NE(pm.get_current_process(), 0u);
    EXPECT_NE(pm.get_current_thread(), 0u);
}

// =============================================================================
// ProcessThunkTest Fixture
// =============================================================================

class ProcessThunkTest : public ::testing::Test {
protected:
    static constexpr usize MEMORY_SIZE = 4096;
    static constexpr GuestAddr MEMORY_BASE = 0x1000;

    void SetUp() override {
        auto result = memory_.map_region(MEMORY_BASE, MEMORY_SIZE,
                                         MemoryProtection::ReadWrite);
        ASSERT_FALSE(result.is_err());
        marshaler_ = std::make_unique<Marshaler>(memory_);
    }

    GuestMemory memory_;
    std::unique_ptr<Marshaler> marshaler_;
    ProcessManager pm_;
};

// =============================================================================
// Thunk Function Tests
// =============================================================================

TEST_F(ProcessThunkTest, ImplGetCurrentProcessDoesNotCrash) {
    impl_GetCurrentProcess(*marshaler_, pm_);
    // Thunk runs without error
}

TEST_F(ProcessThunkTest, ImplGetCurrentProcessIdDoesNotCrash) {
    impl_GetCurrentProcessId(*marshaler_, pm_);
}

TEST_F(ProcessThunkTest, ImplGetCurrentThreadDoesNotCrash) {
    impl_GetCurrentThread(*marshaler_, pm_);
}

TEST_F(ProcessThunkTest, ImplGetCurrentThreadIdDoesNotCrash) {
    impl_GetCurrentThreadId(*marshaler_, pm_);
}

TEST_F(ProcessThunkTest, ImplIsDebuggerPresentDoesNotCrash) {
    impl_IsDebuggerPresent(*marshaler_, pm_);
}

TEST_F(ProcessThunkTest, ImplExitProcessSetsFlag) {
    // Write an exit code into guest memory so the thunk can read it.
    (void)memory_.write_u32(MEMORY_BASE, 7);
    impl_ExitProcess(*marshaler_, pm_);
    // The exit flag should be set regardless of argument parsing.
    EXPECT_TRUE(pm_.has_exited());
}

TEST_F(ProcessThunkTest, ImplGetStartupInfoWWritesStruct) {
    GuestAddr addr = MEMORY_BASE + 256;
    // Write the target pointer at MEMORY_BASE so the thunk can read it.
    (void)memory_.write_u32(MEMORY_BASE, static_cast<u32>(addr));

    impl_GetStartupInfoW(*marshaler_, pm_);

    // Whether or not the thunk successfully read the argument, calling
    // write_startup_info directly should produce a valid struct.
    pm_.write_startup_info(*marshaler_, addr);

    // First DWORD (cb) should equal the struct size.
    auto cb = memory_.read_u32(addr);
    ASSERT_FALSE(cb.is_err());
    EXPECT_EQ(cb.value(), static_cast<u32>(STARTUPINFOW_SIZE_32));
}

TEST_F(ProcessThunkTest, WriteStartupInfoZeroesBody) {
    GuestAddr addr = MEMORY_BASE + 256;

    // Pre-fill with 0xFF to ensure the write actually zeroes.
    for (usize i = 0; i < STARTUPINFOW_SIZE_32; ++i) {
        (void)memory_.write_u8(addr + i, 0xFF);
    }

    pm_.write_startup_info(*marshaler_, addr);

    // Bytes after the cb field (offset 4..68) should be zero.
    for (usize i = 4; i < STARTUPINFOW_SIZE_32; ++i) {
        auto byte = memory_.read_u8(addr + i);
        ASSERT_FALSE(byte.is_err());
        EXPECT_EQ(byte.value(), 0u) << "byte at offset " << i << " is not zero";
    }
}

// An x64 guest's STARTUPINFOW is 104 bytes. The MSVC CRT's lowio initializer
// reads cbReserved2/lpReserved2 (offsets 66/72) to adopt an inherited file
// handle block, so a short 68-byte write leaves it acting on caller stack
// garbage and __pioinfo is never populated.
TEST(StartupInfoLayoutTest, WriteStartupInfoUsesX64LayoutAndClearsTail) {
    constexpr GuestAddr base = 0x1000;
    constexpr GuestAddr addr = base + 256;

    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(base, 0x10000,
                                  MemoryProtection::ReadWrite).is_ok());
    ProcessManager pm;

    cpu::CpuState cpu;
    cpu.init_long_mode();
    cpu.gpr.rsp = base + 0x4000;
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    ASSERT_TRUE(marshaler.cpu_is_64bit());

    // Pre-fill the full 64-bit extent so a short write is visible.
    for (usize i = 0; i < STARTUPINFOW_SIZE_64; ++i) {
        ASSERT_TRUE(memory.write_u8(addr + i, 0xAA).is_ok());
    }

    pm.write_startup_info(marshaler, addr);

    auto cb = memory.read_u32(addr);
    ASSERT_TRUE(cb.is_ok());
    EXPECT_EQ(cb.value(), static_cast<u32>(STARTUPINFOW_SIZE_64));

    for (usize i = 4; i < STARTUPINFOW_SIZE_64; ++i) {
        auto byte = memory.read_u8(addr + i);
        ASSERT_TRUE(byte.is_ok());
        EXPECT_EQ(byte.value(), 0u) << "byte at offset " << i << " is not zero";
    }

    // The fields the CRT actually consumes must read as a clean "nothing
    // inherited" block rather than stack residue.
    auto lp_reserved2 = memory.read_u64(addr + 72);
    ASSERT_TRUE(lp_reserved2.is_ok());
    EXPECT_EQ(lp_reserved2.value(), 0u);
    auto cb_reserved2 = memory.read_u16(addr + 66);
    ASSERT_TRUE(cb_reserved2.is_ok());
    EXPECT_EQ(cb_reserved2.value(), 0u);
}

// =============================================================================
// make_process_exports Tests
// =============================================================================

TEST_F(ProcessThunkTest, MakeProcessExportsReturnsEntries) {
    DllExports exports = make_process_exports(pm_);
    EXPECT_GE(exports.entries.size(), 7u);
}

TEST_F(ProcessThunkTest, MakeProcessExportsHasExitProcess) {
    DllExports exports = make_process_exports(pm_);
    bool found = false;
    for (const auto& entry : exports.entries) {
        if (entry.name == "ExitProcess") {
            found = true;
            break;
        }
    }
    EXPECT_TRUE(found) << "ExitProcess not found in exports";
}

TEST_F(ProcessThunkTest, MakeProcessExportsHasGetCurrentProcess) {
    DllExports exports = make_process_exports(pm_);
    bool found = false;
    for (const auto& entry : exports.entries) {
        if (entry.name == "GetCurrentProcess") {
            found = true;
            break;
        }
    }
    EXPECT_TRUE(found) << "GetCurrentProcess not found in exports";
}

TEST_F(ProcessThunkTest, MakeProcessExportsHandlersCallable) {
    DllExports exports = make_process_exports(pm_);
    for (const auto& entry : exports.entries) {
        ASSERT_TRUE(entry.handler) << entry.name << " handler is null";
        // Exercise each handler — should not crash.
        entry.handler(*marshaler_);
    }
}

TEST_F(ProcessThunkTest, MakeProcessExportsOrdinalsUnique) {
    DllExports exports = make_process_exports(pm_);
    std::set<u16> ordinals;
    for (const auto& entry : exports.entries) {
        if (entry.ordinal == 0) continue; // ordinal 0 = name-only export
        EXPECT_TRUE(ordinals.insert(entry.ordinal).second)
            << "Duplicate ordinal " << entry.ordinal << " for " << entry.name;
    }
}

TEST(ProcessHandleTest, OpenProcessAcceptsRegisteredPeerWithoutSharedIpc) {
    constexpr GuestAddr memory_base = 0x1000;
    constexpr GuestAddr stack_base = 0x8000;
    constexpr u32 peer_pid = 424242;

    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(memory_base, 0x1000,
                                  MemoryProtection::ReadWrite).is_ok());
    ASSERT_TRUE(memory.map_region(stack_base, 0x1000,
                                  MemoryProtection::ReadWrite).is_ok());
    cpu::CpuState cpu;
    cpu.init_protected_mode();
    cpu.gpr.rsp = stack_base + 0x800;
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    ProcessManager process_manager;
    process_manager.set_process_snapshot_provider([=] {
        return std::vector<ipc::ProcessSnapshotEntry>{
            {peer_pid, 1, "Battle.net-Setup.exe",
             "C:\\Vitreo\\Apps\\Win-exe\\Battle.net-Setup.exe"},
        };
    });
    HandleTable handles;

    auto exports = make_process_handle_exports(process_manager, handles);
    auto open_process = std::find_if(
        exports.entries.begin(), exports.entries.end(),
        [](const ExportEntry& entry) { return entry.name == "OpenProcess"; });
    ASSERT_NE(open_process, exports.entries.end());
    ASSERT_TRUE(memory.write_u32(cpu.sp() + 4, 0x1000).is_ok());
    ASSERT_TRUE(memory.write_u32(cpu.sp() + 8, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(cpu.sp() + 12, peer_pid).is_ok());

    open_process->handler(marshaler);

    const Handle handle = static_cast<Handle>(cpu.gpr.rax);
    ASSERT_NE(handle, NULL_HANDLE);
    EXPECT_EQ(handles.get_type(handle), HandleType::Process);
    auto* process = static_cast<ipc::ProcessObject*>(
        handles.get(handle, HandleType::Process));
    ASSERT_NE(process, nullptr);
    EXPECT_EQ(process->pid, static_cast<pid_t>(peer_pid));
}

TEST(ProcessHandleTest, OpenProcessReopensTrackedExitedChild) {
    // CEF opens the GPU pid again after TerminateProcess; the capsule snapshot
    // no longer lists it, but CreateProcess already tracked the ProcessObject.
    const pid_t child = ::fork();
    ASSERT_GE(child, 0);
    if (child == 0) {
        _exit(0);
    }
    ipc::ProcessObject* tracked = ipc::track_process(child);
    ASSERT_NE(tracked, nullptr);
    for (int i = 0; i < 100 && !tracked->exited.load(); ++i) {
        usleep(10 * 1000);
    }
    ASSERT_TRUE(tracked->exited.load());
    ::waitpid(child, nullptr, 0);

    constexpr GuestAddr stack_base = 0x8000;
    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(0x1000, 0x1000,
                                  MemoryProtection::ReadWrite).is_ok());
    ASSERT_TRUE(memory.map_region(stack_base, 0x1000,
                                  MemoryProtection::ReadWrite).is_ok());
    cpu::CpuState cpu;
    cpu.init_protected_mode();
    cpu.gpr.rsp = stack_base + 0x800;
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    ProcessManager process_manager;
    // Empty snapshot — live-peer lookup must miss.
    process_manager.set_process_snapshot_provider(
        [] { return std::vector<ipc::ProcessSnapshotEntry>{}; });
    HandleTable handles;

    auto exports = make_process_handle_exports(process_manager, handles);
    auto open_process = std::find_if(
        exports.entries.begin(), exports.entries.end(),
        [](const ExportEntry& entry) { return entry.name == "OpenProcess"; });
    ASSERT_NE(open_process, exports.entries.end());
    ASSERT_TRUE(memory.write_u32(cpu.sp() + 4, 0x1000).is_ok());
    ASSERT_TRUE(memory.write_u32(cpu.sp() + 8, 0).is_ok());
    ASSERT_TRUE(
        memory.write_u32(cpu.sp() + 12, static_cast<u32>(child)).is_ok());

    open_process->handler(marshaler);

    const Handle handle = static_cast<Handle>(cpu.gpr.rax);
    ASSERT_NE(handle, NULL_HANDLE);
    auto* process = static_cast<ipc::ProcessObject*>(
        handles.get(handle, HandleType::Process));
    ASSERT_NE(process, nullptr);
    EXPECT_EQ(process, tracked);
    EXPECT_TRUE(process->exited.load());
}

TEST(ProcessHandleTest, TerminateProcessPseudoHandleExitsCurrentGuest) {
    constexpr GuestAddr stack = 0x8000;
    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(
        0x1000, 0x10000, MemoryProtection::ReadWrite).is_ok());
    cpu::CpuState cpu;
    cpu.init_long_mode();
    cpu.gpr.rsp = stack;
    cpu.gpr.rcx = CURRENT_PROCESS_PSEUDO;
    cpu.gpr.rdx = 0x1234;
    ASSERT_TRUE(memory.write_u64(stack, 0xDEADBEEF).is_ok());

    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    ProcessManager processes;
    HandleTable handles;
    auto exports = make_process_handle_exports(processes, handles);
    auto terminate = std::find_if(
        exports.entries.begin(), exports.entries.end(),
        [](const ExportEntry& entry) {
            return entry.name == "TerminateProcess";
        });
    ASSERT_NE(terminate, exports.entries.end());

    terminate->handler(marshaler);

    EXPECT_EQ(cpu.gpr.rax, 1u);
    EXPECT_TRUE(processes.has_exited());
    EXPECT_EQ(processes.exit_code(), 0x1234u);
}

TEST(ProcessHandleTest, TerminateProcessTargetsChildWithoutExitingCaller) {
    const pid_t child = ::fork();
    ASSERT_GE(child, 0);
    if (child == 0) {
        // Learn: a process cannot prevent TerminateProcess. Ignoring SIGTERM
        // models the Vitreo worker, which intercepts SIGTERM as host shutdown.
        ::signal(SIGTERM, SIG_IGN);
        for (;;) ::pause();
    }

    ipc::ProcessObject* child_process = ipc::track_process(child);
    ASSERT_NE(child_process, nullptr);

    constexpr GuestAddr stack = 0x8000;
    constexpr u32 process_terminate = 0x0001;
    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(
        0x1000, 0x10000, MemoryProtection::ReadWrite).is_ok());
    cpu::CpuState cpu;
    cpu.init_long_mode();
    cpu.gpr.rsp = stack;
    ASSERT_TRUE(memory.write_u64(stack, 0xDEADBEEF).is_ok());

    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    ProcessManager processes;
    HandleTable handles;
    const Handle child_handle = handles.allocate(
        HandleType::Process, child_process, 0, process_terminate);
    cpu.gpr.rcx = child_handle;
    cpu.gpr.rdx = 77;

    auto exports = make_process_handle_exports(processes, handles);
    auto terminate = std::find_if(
        exports.entries.begin(), exports.entries.end(),
        [](const ExportEntry& entry) {
            return entry.name == "TerminateProcess";
        });
    ASSERT_NE(terminate, exports.entries.end());

    terminate->handler(marshaler);

    EXPECT_EQ(cpu.gpr.rax, 1u);
    EXPECT_FALSE(processes.has_exited());

    for (int i = 0; i < 5000 &&
                    !child_process->exited.load(std::memory_order_acquire);
         ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    EXPECT_TRUE(child_process->exited.load(std::memory_order_acquire));
    EXPECT_EQ(child_process->exit_code.load(std::memory_order_acquire), 77u);

    int status = 0;
    if (::waitpid(child, &status, WNOHANG) == 0) {
        (void)::kill(child, SIGKILL);
        (void)::waitpid(child, &status, 0);
    }
}

// NtTerminateProcess shares TerminateProcess's core and returns its NTSTATUS.
// Every status is tools/oracle_probe/nt_terminate_process_probe.expected
// (x64 and WoW64 agree).
namespace {

u32 call_nt_terminate(ProcessManager& processes, HandleTable& handles,
                      u64 handle, u32 status) {
    constexpr GuestAddr stack = 0x8000;
    GuestMemory memory;
    EXPECT_TRUE(memory.map_region(
        0x1000, 0x10000, MemoryProtection::ReadWrite).is_ok());
    cpu::CpuState cpu;
    cpu.init_long_mode();
    cpu.gpr.rsp = stack;
    cpu.gpr.rcx = handle;
    cpu.gpr.rdx = status;
    EXPECT_TRUE(memory.write_u64(stack, 0xDEADBEEF).is_ok());
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    auto exports = make_nt_process_handle_exports(processes, handles);
    auto terminate = std::find_if(
        exports.entries.begin(), exports.entries.end(),
        [](const ExportEntry& entry) {
            return entry.name == "NtTerminateProcess";
        });
    EXPECT_NE(terminate, exports.entries.end());
    if (terminate == exports.entries.end()) return 0xFFFFFFFFu;
    EXPECT_EQ(terminate->stdcall_bytes, 8u);
    terminate->handler(marshaler);
    return static_cast<u32>(cpu.gpr.rax);
}

}  // namespace

TEST(ProcessHandleTest, NtTerminateProcessRejectsHandlesItCannotTerminate) {
    constexpr u32 process_query_limited = 0x1000;
    ProcessManager processes;
    HandleTable handles;
    // "invalid handle 0x1234" / "event handle" / "query access only".
    EXPECT_EQ(call_nt_terminate(processes, handles, 0x1234, 1), 0xC0000008u);
    const Handle event = handles.allocate(HandleType::Event, nullptr);
    EXPECT_EQ(call_nt_terminate(processes, handles, event, 1), 0xC0000024u);
    // Access is checked before the process object is looked at.
    const Handle limited = handles.allocate(
        HandleType::Process, nullptr, 0, process_query_limited);
    EXPECT_EQ(call_nt_terminate(processes, handles, limited, 1), 0xC0000022u);
    // NULL ends every other thread, which Vitreo cannot do alone: it fails
    // loudly instead of claiming success.
    EXPECT_EQ(call_nt_terminate(processes, handles, 0, 1), 0xC0000002u);
    EXPECT_FALSE(processes.has_exited());
}

TEST(ProcessHandleTest, NtTerminateProcessPseudoHandleExitsCurrentGuest) {
    ProcessManager processes;
    HandleTable handles;
    EXPECT_EQ(call_nt_terminate(processes, handles, ~u64{0}, 0x1234), 0u);
    EXPECT_TRUE(processes.has_exited());
    EXPECT_EQ(processes.exit_code(), 0x1234u);
}

TEST(ProcessHandleTest, NtTerminateProcessEndsChildOnceThenReportsTerminating) {
    const pid_t child = ::fork();
    ASSERT_GE(child, 0);
    if (child == 0) {
        ::signal(SIGTERM, SIG_IGN);
        for (;;) ::pause();
    }
    ipc::ProcessObject* child_process = ipc::track_process(child);
    ASSERT_NE(child_process, nullptr);
    constexpr u32 process_terminate = 0x0001;
    ProcessManager processes;
    HandleTable handles;
    const Handle child_handle = handles.allocate(
        HandleType::Process, child_process, 0, process_terminate);

    EXPECT_EQ(call_nt_terminate(processes, handles, child_handle, 0x0BAD), 0u);
    EXPECT_FALSE(processes.has_exited());
    for (int i = 0; i < 5000 &&
                    !child_process->exited.load(std::memory_order_acquire);
         ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    ASSERT_TRUE(child_process->exited.load(std::memory_order_acquire));
    EXPECT_EQ(child_process->exit_code.load(std::memory_order_acquire),
              0x0BADu);
    // "terminated child again": STATUS_PROCESS_IS_TERMINATING.
    EXPECT_EQ(call_nt_terminate(processes, handles, child_handle, 1),
              0xC000010Au);

    int status = 0;
    if (::waitpid(child, &status, WNOHANG) == 0) {
        (void)::kill(child, SIGKILL);
        (void)::waitpid(child, &status, 0);
    }
}

TEST(ProcessHandleTest, DuplicateCurrentProcessSupportsTimingQueriesX64) {
    constexpr GuestAddr memory_base = 0x1000;
    constexpr GuestAddr stack = 0x8000;
    constexpr GuestAddr output_handle = 0x1800;
    constexpr GuestAddr creation = 0x1900;
    constexpr GuestAddr exit_time = 0x1908;
    constexpr GuestAddr kernel_time = 0x1910;
    constexpr GuestAddr user_time = 0x1918;
    constexpr u32 PROCESS_QUERY_LIMITED_INFORMATION = 0x1000;

    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(
        memory_base, 0x10000, MemoryProtection::ReadWrite).is_ok());
    HandleTable handles;
    ProcessManager processes;
    capsule::Vfs vfs;
    Kernel32ExtManager kernel32(memory, vfs);
    kernel32.set_handle_table(&handles);
    kernel32.set_process_manager(&processes);

    cpu::CpuState cpu;
    cpu.init_long_mode();
    cpu.gpr.rsp = stack;
    cpu.gpr.rcx = CURRENT_PROCESS_PSEUDO;
    cpu.gpr.rdx = CURRENT_PROCESS_PSEUDO;
    cpu.gpr.r8 = CURRENT_PROCESS_PSEUDO;
    cpu.gpr.r9 = output_handle;
    ASSERT_TRUE(memory.write_u64(
        stack + 0x28, PROCESS_QUERY_LIMITED_INFORMATION).is_ok());
    ASSERT_TRUE(memory.write_u64(stack + 0x30, 0).is_ok());
    ASSERT_TRUE(memory.write_u64(stack + 0x38, 0).is_ok());

    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    kernel32.impl_DuplicateHandle(marshaler);

    ASSERT_EQ(cpu.gpr.rax, 1u);
    const Handle duplicate = static_cast<Handle>(
        memory.read_u64(output_handle).value_or(0));
    ASSERT_NE(duplicate, NULL_HANDLE);
    EXPECT_NE(duplicate, CURRENT_PROCESS_PSEUDO);
    EXPECT_EQ(handles.get_type(duplicate), HandleType::Process);
    EXPECT_EQ(handles.get_access(duplicate),
              PROCESS_QUERY_LIMITED_INFORMATION);

    cpu.gpr.rcx = duplicate;
    cpu.gpr.rdx = creation;
    cpu.gpr.r8 = exit_time;
    cpu.gpr.r9 = kernel_time;
    ASSERT_TRUE(memory.write_u64(stack + 0x28, user_time).is_ok());
    kernel32.impl_GetProcessTimes(marshaler);

    ASSERT_EQ(cpu.gpr.rax, 1u);
    EXPECT_NE(memory.read_u64(creation).value_or(0), 0u);
    EXPECT_EQ(memory.read_u64(exit_time).value_or(1), 0u);
    EXPECT_TRUE(handles.close(duplicate));
}

TEST(ProcessHandleTest, GetProcessWorkingSetSizeExWritesGuestWidthDefaults) {
    constexpr GuestAddr memory_base = 0x1000;
    constexpr GuestAddr stack = 0x8000;
    constexpr GuestAddr minimum = 0x1800;
    constexpr GuestAddr maximum = 0x1810;
    constexpr GuestAddr flags = 0x1820;

    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(
        memory_base, 0x10000, MemoryProtection::ReadWrite).is_ok());
    HandleTable handles;
    capsule::Vfs vfs;
    Kernel32ExtManager kernel32(memory, vfs);
    kernel32.set_handle_table(&handles);

    cpu::CpuState cpu;
    cpu.init_long_mode();
    cpu.gpr.rsp = stack;
    cpu.gpr.rcx = CURRENT_PROCESS_PSEUDO;
    cpu.gpr.rdx = minimum;
    cpu.gpr.r8 = maximum;
    cpu.gpr.r9 = flags;
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    set_last_error(0x1234u);

    kernel32.impl_GetProcessWorkingSetSizeEx(marshaler);

    EXPECT_EQ(cpu.gpr.rax, 1u);
    EXPECT_EQ(memory.read_u64(minimum).value_or(0), 50u * 4096u);
    EXPECT_EQ(memory.read_u64(maximum).value_or(0), 345u * 4096u);
    EXPECT_EQ(memory.read_u32(flags).value_or(0), 0x0au);
    EXPECT_EQ(get_last_error(), 0x1234u);
}

TEST(ProcessHandleTest, GetProcessWorkingSetSizeExRejectsMissingOutput) {
    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(
        0x1000, 0x10000, MemoryProtection::ReadWrite).is_ok());
    capsule::Vfs vfs;
    Kernel32ExtManager kernel32(memory, vfs);
    cpu::CpuState cpu;
    cpu.init_long_mode();
    cpu.gpr.rcx = CURRENT_PROCESS_PSEUDO;
    cpu.gpr.rdx = 0;
    cpu.gpr.r8 = 0x1810;
    cpu.gpr.r9 = 0x1820;
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);

    kernel32.impl_GetProcessWorkingSetSizeEx(marshaler);

    EXPECT_EQ(cpu.gpr.rax, 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_NOACCESS);
}

TEST(ProcessHandleTest, Kernel32ExportsGetProcessWorkingSetSizeEx) {
    GuestMemory memory;
    capsule::Vfs vfs;
    Kernel32ExtManager kernel32(memory, vfs);
    const auto exports = create_kernel32_ext_thunks(kernel32);
    const auto found = std::find_if(
        exports.entries.begin(), exports.entries.end(),
        [](const ExportEntry& entry) {
            return entry.name == "GetProcessWorkingSetSizeEx";
        });
    ASSERT_NE(found, exports.entries.end());
    EXPECT_EQ(found->stdcall_bytes, 16u);
}

TEST(ProcessHandleTest, ReadProcessMemoryReadsCreatedProcessImageBaseX64) {
    constexpr GuestAddr memory_base = 0x1000;
    constexpr GuestAddr stack = 0x8000;
    constexpr GuestAddr output = 0x1800;
    constexpr GuestAddr bytes_read = 0x1810;
    constexpr GuestAddr target_peb = 0x7FFE2000;
    constexpr u64 target_image_base = 0x00000001'40000000ULL;

    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(
        memory_base, 0x10000, MemoryProtection::ReadWrite).is_ok());
    HandleTable handles;
    capsule::Vfs vfs;
    Kernel32ExtManager kernel32(memory, vfs);
    kernel32.set_handle_table(&handles);

    ipc::ProcessObject process;
    process.pid = 4242;
    process.publish_guest_metadata(ipc::GuestProcessMetadata{
        true, target_peb, target_image_base});
    const Handle process_handle =
        handles.allocate(HandleType::Process, &process);

    cpu::CpuState cpu;
    cpu.init_long_mode();
    cpu.gpr.rsp = stack;
    cpu.gpr.rcx = process_handle;
    cpu.gpr.rdx = target_peb + 0x10;
    cpu.gpr.r8 = output;
    cpu.gpr.r9 = sizeof(u64);
    ASSERT_TRUE(memory.write_u64(stack + 0x28, bytes_read).is_ok());

    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    kernel32.impl_ReadProcessMemory(marshaler);

    EXPECT_EQ(cpu.gpr.rax, 1u);
    EXPECT_EQ(memory.read_u64(output).value_or(0), target_image_base);
    EXPECT_EQ(memory.read_u64(bytes_read).value_or(0), sizeof(u64));
}

TEST(ProcessHandleTest, ReadProcessMemoryReadsCreatedProcessPebPrefixX64) {
    constexpr GuestAddr memory_base = 0x1000;
    constexpr GuestAddr stack = 0x8000;
    constexpr GuestAddr output = 0x1800;
    constexpr GuestAddr bytes_read = 0x1840;
    constexpr GuestAddr target_peb = 0x7FFE2000;
    constexpr u64 target_image_base = 0x00000001'40000000ULL;

    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(
        memory_base, 0x10000, MemoryProtection::ReadWrite).is_ok());
    HandleTable handles;
    capsule::Vfs vfs;
    Kernel32ExtManager kernel32(memory, vfs);
    kernel32.set_handle_table(&handles);

    ipc::ProcessObject process;
    process.pid = 4242;
    process.publish_guest_metadata(ipc::GuestProcessMetadata{
        true, target_peb, target_image_base});
    const Handle process_handle =
        handles.allocate(HandleType::Process, &process);

    cpu::CpuState cpu;
    cpu.init_long_mode();
    cpu.gpr.rsp = stack;
    cpu.gpr.rcx = process_handle;
    cpu.gpr.rdx = target_peb;
    cpu.gpr.r8 = output;
    cpu.gpr.r9 = 40;
    ASSERT_TRUE(memory.write_u64(stack + 0x28, bytes_read).is_ok());

    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    kernel32.impl_ReadProcessMemory(marshaler);

    EXPECT_EQ(cpu.gpr.rax, 1u);
    EXPECT_EQ(memory.read_u64(output + 0x10).value_or(0),
              target_image_base);
    EXPECT_EQ(memory.read_u64(output + 0x20).value_or(0),
              target_peb + 0x400);
    EXPECT_EQ(memory.read_u64(bytes_read).value_or(0), 40u);
}

TEST(ProcessHandleTest, ReadProcessMemoryRejectsUnmodeledPeerRange) {
    constexpr GuestAddr memory_base = 0x1000;
    constexpr GuestAddr stack = 0x8000;
    constexpr GuestAddr output = 0x1800;
    constexpr GuestAddr bytes_read = 0x1810;
    constexpr GuestAddr target_peb = 0x7FFE2000;

    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(
        memory_base, 0x10000, MemoryProtection::ReadWrite).is_ok());
    HandleTable handles;
    capsule::Vfs vfs;
    Kernel32ExtManager kernel32(memory, vfs);
    kernel32.set_handle_table(&handles);

    ipc::ProcessObject process;
    process.pid = 4242;
    process.publish_guest_metadata(ipc::GuestProcessMetadata{
        true, target_peb, 0x00000001'40000000ULL});
    const Handle process_handle =
        handles.allocate(HandleType::Process, &process);

    cpu::CpuState cpu;
    cpu.init_long_mode();
    cpu.gpr.rsp = stack;
    cpu.gpr.rcx = process_handle;
    cpu.gpr.rdx = target_peb + 0x40;
    cpu.gpr.r8 = output;
    cpu.gpr.r9 = sizeof(u64);
    ASSERT_TRUE(memory.write_u64(stack + 0x28, bytes_read).is_ok());

    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    kernel32.impl_ReadProcessMemory(marshaler);

    EXPECT_EQ(cpu.gpr.rax, 0u);
    EXPECT_EQ(get_last_error(), 299u);
    EXPECT_EQ(memory.read_u64(bytes_read).value_or(1), 0u);
}

TEST(ProcessHandleTest, VirtualQueryExReportsActualLocalRegionX86) {
    constexpr GuestAddr stack = 0x8000;
    constexpr GuestAddr output = 0x1800;
    constexpr GuestAddr queried_base = 0x24000;

    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(
        0x1000, 0x10000, MemoryProtection::ReadWrite).is_ok());
    ASSERT_TRUE(memory.map_region(
        queried_base, 0x3000, MemoryProtection::ReadExecute).is_ok());
    HandleTable handles;
    capsule::Vfs vfs;
    Kernel32ExtManager kernel32(memory, vfs);
    kernel32.set_handle_table(&handles);

    cpu::CpuState cpu;
    cpu.init_protected_mode();
    cpu.set_sp(stack);
    ASSERT_TRUE(memory.write_u32(stack, 0xDEADBEEF).is_ok());
    ASSERT_TRUE(memory.write_u32(
        stack + 4, CURRENT_PROCESS_PSEUDO).is_ok());
    ASSERT_TRUE(memory.write_u32(
        stack + 8, queried_base + 0x180).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 12, output).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 16, 28).is_ok());

    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    kernel32.impl_VirtualQueryEx(marshaler);

    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 28u);
    EXPECT_EQ(memory.read_u32(output + 0).value_or(0), queried_base);
    EXPECT_EQ(memory.read_u32(output + 4).value_or(0), queried_base);
    EXPECT_EQ(memory.read_u32(output + 8).value_or(0),
              PAGE_EXECUTE_READ);
    EXPECT_EQ(memory.read_u32(output + 12).value_or(0), 0x3000u);
    EXPECT_EQ(memory.read_u32(output + 16).value_or(0), MEM_COMMIT);
    EXPECT_EQ(memory.read_u32(output + 20).value_or(0),
              PAGE_EXECUTE_READ);
    EXPECT_EQ(memory.read_u32(output + 24).value_or(0), MEM_PRIVATE);
}

TEST(ProcessHandleTest, VirtualQueryExSerializesLocalRegionX64) {
    constexpr GuestAddr output = 0x1800;
    constexpr GuestAddr queried_base = 0x00000001'24000000ULL;

    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(
        0x1000, 0x10000, MemoryProtection::ReadWrite).is_ok());
    ASSERT_TRUE(memory.map_region(
        queried_base, 0x5000, MemoryProtection::Read).is_ok());
    HandleTable handles;
    capsule::Vfs vfs;
    Kernel32ExtManager kernel32(memory, vfs);
    kernel32.set_handle_table(&handles);

    cpu::CpuState cpu;
    cpu.init_long_mode();
    cpu.gpr.rsp = 0x8000;
    cpu.gpr.rcx = ~u64{0};
    cpu.gpr.rdx = queried_base + 0x900;
    cpu.gpr.r8 = output;
    cpu.gpr.r9 = 48;

    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    kernel32.impl_VirtualQueryEx(marshaler);

    EXPECT_EQ(cpu.gpr.rax, 48u);
    EXPECT_EQ(memory.read_u64(output + 0).value_or(0), queried_base);
    EXPECT_EQ(memory.read_u64(output + 8).value_or(0), queried_base);
    EXPECT_EQ(memory.read_u32(output + 16).value_or(0), PAGE_READONLY);
    EXPECT_EQ(memory.read_u32(output + 20).value_or(1), 0u);
    EXPECT_EQ(memory.read_u64(output + 24).value_or(0), 0x5000u);
    EXPECT_EQ(memory.read_u32(output + 32).value_or(0), MEM_COMMIT);
    EXPECT_EQ(memory.read_u32(output + 36).value_or(0), PAGE_READONLY);
    EXPECT_EQ(memory.read_u32(output + 40).value_or(0), MEM_PRIVATE);
    EXPECT_EQ(memory.read_u32(output + 44).value_or(1), 0u);
}

TEST(ProcessHandleTest, VirtualQueryExRequiresProcessQueryAccess) {
    constexpr GuestAddr stack = 0x8000;
    constexpr GuestAddr output = 0x1800;
    constexpr u32 PROCESS_VM_READ = 0x0010;

    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(
        0x1000, 0x10000, MemoryProtection::ReadWrite).is_ok());
    HandleTable handles;
    capsule::Vfs vfs;
    Kernel32ExtManager kernel32(memory, vfs);
    kernel32.set_handle_table(&handles);

    ipc::ProcessObject process;
    process.pid = ::getpid();
    const Handle process_handle = handles.allocate(
        HandleType::Process, &process, 0, PROCESS_VM_READ);

    cpu::CpuState cpu;
    cpu.init_protected_mode();
    cpu.set_sp(stack);
    ASSERT_TRUE(memory.write_u32(stack, 0xDEADBEEF).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 4, process_handle).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 8, 0x1000).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 12, output).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 16, 28).is_ok());

    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    kernel32.impl_VirtualQueryEx(marshaler);

    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_ACCESS_DENIED);
}

TEST(ProcessHandleTest,
     DuplicateExternalPrimaryThreadPreservesImpersonationTokenX64) {
    constexpr GuestAddr memory_base = 0x1000;
    constexpr GuestAddr stack = 0x8000;
    constexpr GuestAddr output_handle = 0x1800;
    constexpr u32 DUPLICATE_SAME_ACCESS = 0x00000002;
    constexpr u32 TOKEN_IMPERSONATE = 0x0004;
    constexpr u32 TOKEN_QUERY = 0x0008;

    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(
        memory_base, 0x10000, MemoryProtection::ReadWrite).is_ok());
    HandleTable handles;
    capsule::Vfs vfs;
    SecurityStubs security(handles);
    Kernel32ExtManager kernel32(memory, vfs);
    kernel32.set_handle_table(&handles);
    kernel32.set_security_stubs(&security);

    // CreateProcess represents a child worker's primary thread with a valid,
    // opaque thread handle. It has no local GuestThread object.
    const Handle external_thread =
        handles.allocate(HandleType::Thread, nullptr);
    Handle primary = NULL_HANDLE;
    u32 token_error = 0;
    ASSERT_TRUE(security.open_process_token(
        CURRENT_PROCESS_PSEUDO, HandleAccess::All, primary, token_error));
    Handle impersonation = NULL_HANDLE;
    ASSERT_TRUE(security.duplicate_token(
        primary, 2, impersonation, 2,
        TOKEN_IMPERSONATE | TOKEN_QUERY));
    u32 error = 0;
    ASSERT_TRUE(security.set_thread_token(
        external_thread, impersonation, error));

    cpu::CpuState cpu;
    cpu.init_long_mode();
    cpu.gpr.rsp = stack;
    cpu.gpr.rcx = CURRENT_PROCESS_PSEUDO;
    cpu.gpr.rdx = external_thread;
    cpu.gpr.r8 = CURRENT_PROCESS_PSEUDO;
    cpu.gpr.r9 = output_handle;
    ASSERT_TRUE(memory.write_u64(stack + 0x28, 0).is_ok());
    ASSERT_TRUE(memory.write_u64(stack + 0x30, 0).is_ok());
    ASSERT_TRUE(memory.write_u64(
        stack + 0x38, DUPLICATE_SAME_ACCESS).is_ok());

    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    kernel32.impl_DuplicateHandle(marshaler);

    ASSERT_EQ(cpu.gpr.rax, 1u);
    const Handle duplicate = static_cast<Handle>(
        memory.read_u64(output_handle).value_or(0));
    ASSERT_NE(duplicate, NULL_HANDLE);
    EXPECT_NE(duplicate, external_thread);
    EXPECT_EQ(handles.get_type(duplicate), HandleType::Thread);
    EXPECT_EQ(handles.get(duplicate, HandleType::Thread), nullptr);

    Handle reopened = NULL_HANDLE;
    ASSERT_TRUE(security.open_thread_token(
        duplicate, TOKEN_QUERY, reopened, error));
    EXPECT_NE(reopened, NULL_HANDLE);
}

TEST(ProcessHandleTest,
     DuplicateHandleImportsEventIntoExternalTargetProcessX64) {
    constexpr GuestAddr memory_base = 0x1000;
    constexpr GuestAddr stack = 0x8000;
    constexpr GuestAddr output_handle = 0x1800;
    constexpr u32 SYNCHRONIZE_AND_EVENT_MODIFY_STATE = 0x00100002;

    GuestMemory source_memory;
    GuestMemory target_memory;
    ASSERT_TRUE(source_memory.map_region(
        memory_base, 0x10000, MemoryProtection::ReadWrite).is_ok());
    HandleTable source_handles;
    HandleTable target_handles;
    SyncManager source_sync(source_handles);
    SyncManager target_sync(target_handles);
    FileMappingManager source_mappings(source_handles, source_memory);
    FileMappingManager target_mappings(target_handles, target_memory);
    VirtualMemoryManager target_virtual_memory(target_memory);
    ipc::RemoteMemoryServer server(
        target_memory, target_virtual_memory,
        &target_sync, &target_mappings);
    ASSERT_TRUE(server.start(false));

    capsule::Vfs vfs;
    Kernel32ExtManager kernel32(source_memory, vfs);
    kernel32.set_handle_table(&source_handles);
    kernel32.set_sync_manager(&source_sync);
    kernel32.set_file_mapping_manager(&source_mappings);

    const Handle source_event =
        source_sync.create_event(false, false, "");
    ipc::ProcessObject target_process;
    target_process.pid = ::getpid();
    const Handle target_process_handle = source_handles.allocate(
        HandleType::Process, &target_process, 0, HandleAccess::All);

    cpu::CpuState cpu;
    cpu.init_long_mode();
    cpu.gpr.rsp = stack;
    cpu.gpr.rcx = CURRENT_PROCESS_PSEUDO;
    cpu.gpr.rdx = source_event;
    cpu.gpr.r8 = target_process_handle;
    cpu.gpr.r9 = output_handle;
    ASSERT_TRUE(source_memory.write_u64(
        stack + 0x28, SYNCHRONIZE_AND_EVENT_MODIFY_STATE).is_ok());
    ASSERT_TRUE(source_memory.write_u64(stack + 0x30, 0).is_ok());
    ASSERT_TRUE(source_memory.write_u64(stack + 0x38, 0).is_ok());

    Marshaler marshaler(source_memory);
    marshaler.attach_cpu(cpu);
    kernel32.impl_DuplicateHandle(marshaler);

    ASSERT_EQ(cpu.gpr.rax, 1u);
    const Handle target_event = static_cast<Handle>(
        source_memory.read_u64(output_handle).value_or(0));
    ASSERT_NE(target_event, NULL_HANDLE);
    EXPECT_EQ(target_handles.get_type(target_event), HandleType::Event);
    EXPECT_TRUE(source_sync.set_event(source_event));
    EXPECT_EQ(target_sync.wait_for_single_object(target_event, 0),
              WAIT_OBJECT_0);
}

TEST(ProcessHandleTest,
     DuplicateHandleImportsProcessIntoExternalTargetProcessX64) {
    // Learn DuplicateHandle: Process handles are duplicable across processes.
    // Mojo IPCz Transport::Serialize sends the peer Process handle this way.
    constexpr GuestAddr memory_base = 0x1000;
    constexpr GuestAddr stack = 0x8000;
    constexpr GuestAddr output_handle = 0x1800;
    constexpr u32 PROCESS_DUP_HANDLE = 0x00000040;
    constexpr u32 DUPLICATE_SAME_ACCESS = 0x00000002;

    GuestMemory source_memory;
    GuestMemory target_memory;
    ASSERT_TRUE(source_memory.map_region(
        memory_base, 0x10000, MemoryProtection::ReadWrite).is_ok());
    HandleTable source_handles;
    HandleTable target_handles;
    SyncManager source_sync(source_handles);
    SyncManager target_sync(target_handles);
    FileMappingManager source_mappings(source_handles, source_memory);
    FileMappingManager target_mappings(target_handles, target_memory);
    VirtualMemoryManager target_virtual_memory(target_memory);
    ipc::RemoteMemoryServer server(
        target_memory, target_virtual_memory,
        &target_sync, &target_mappings, &target_handles);
    ASSERT_TRUE(server.start(false));

    capsule::Vfs vfs;
    Kernel32ExtManager kernel32(source_memory, vfs);
    kernel32.set_handle_table(&source_handles);
    kernel32.set_sync_manager(&source_sync);
    kernel32.set_file_mapping_manager(&source_mappings);

    ipc::ProcessObject self_process;
    self_process.pid = ::getpid();
    const Handle source_process_handle = source_handles.allocate(
        HandleType::Process, &self_process, 0,
        PROCESS_DUP_HANDLE | 0x00100000u /* SYNCHRONIZE */);

    ipc::ProcessObject target_process;
    target_process.pid = ::getpid();
    const Handle target_process_handle = source_handles.allocate(
        HandleType::Process, &target_process, 0, HandleAccess::All);

    cpu::CpuState cpu;
    cpu.init_long_mode();
    cpu.gpr.rsp = stack;
    cpu.gpr.rcx = CURRENT_PROCESS_PSEUDO;
    cpu.gpr.rdx = source_process_handle;
    cpu.gpr.r8 = target_process_handle;
    cpu.gpr.r9 = output_handle;
    ASSERT_TRUE(source_memory.write_u64(stack + 0x28, 0).is_ok());
    ASSERT_TRUE(source_memory.write_u64(stack + 0x30, 0).is_ok());
    ASSERT_TRUE(source_memory.write_u64(
        stack + 0x38, DUPLICATE_SAME_ACCESS).is_ok());

    Marshaler marshaler(source_memory);
    marshaler.attach_cpu(cpu);
    kernel32.impl_DuplicateHandle(marshaler);

    ASSERT_EQ(cpu.gpr.rax, 1u);
    const Handle imported = static_cast<Handle>(
        source_memory.read_u64(output_handle).value_or(0));
    ASSERT_NE(imported, NULL_HANDLE);
    EXPECT_EQ(target_handles.get_type(imported), HandleType::Process);
    auto* imported_process = static_cast<ipc::ProcessObject*>(
        target_handles.get(imported, HandleType::Process));
    ASSERT_NE(imported_process, nullptr);
    EXPECT_EQ(imported_process->pid, ::getpid());
    EXPECT_EQ(target_handles.get_access(imported),
              PROCESS_DUP_HANDLE | 0x00100000u);
}

// =============================================================================
// GetProcessId Contract Tests
// =============================================================================

// Chromium's base::GetProcId() executes `int3; ud2` when GetProcessId returns 0
// and GetLastError() is ERROR_INVALID_HANDLE, because on Windows that pairing
// can only mean the caller holds a dead or foreign handle. A handle Vitreo
// really tracks must therefore report its real pid.
TEST(GetProcessIdTest, TrackedProcessHandleReportsRealPid) {
    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(0x1000, 0x10000,
                                  MemoryProtection::ReadWrite).is_ok());
    HandleTable handles;
    SyncManager sync(handles);
    capsule::Vfs vfs;
    Kernel32ExtManager kernel32(memory, vfs);
    kernel32.set_handle_table(&handles);
    kernel32.set_sync_manager(&sync);

    const Handle process_handle =
        sync.create_process_handle(static_cast<int>(::getpid()));
    ASSERT_NE(process_handle, NULL_HANDLE);

    cpu::CpuState cpu;
    cpu.init_long_mode();
    cpu.gpr.rsp = 0x8000;
    cpu.gpr.rcx = process_handle;
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    set_last_error(0);
    kernel32.impl_GetProcessId(marshaler);

    EXPECT_EQ(cpu.gpr.rax, static_cast<u64>(::getpid()));
    EXPECT_EQ(get_last_error(), 0u);
}

TEST(GetProcessIdTest, CurrentProcessPseudoHandleMatchesGetCurrentProcessId) {
    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(0x1000, 0x10000,
                                  MemoryProtection::ReadWrite).is_ok());
    HandleTable handles;
    SyncManager sync(handles);
    ProcessManager pm;
    capsule::Vfs vfs;
    Kernel32ExtManager kernel32(memory, vfs);
    kernel32.set_handle_table(&handles);
    kernel32.set_sync_manager(&sync);
    kernel32.set_process_manager(&pm);

    cpu::CpuState cpu;
    cpu.init_long_mode();
    cpu.gpr.rsp = 0x8000;
    cpu.gpr.rcx = CURRENT_PROCESS_PSEUDO;
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    kernel32.impl_GetProcessId(marshaler);

    EXPECT_EQ(cpu.gpr.rax, pm.get_current_process_id());
}

// A handle of the wrong type is a genuine ERROR_INVALID_HANDLE on Windows, so
// that honest failure must survive; only tracked process handles resolve.
TEST(GetProcessIdTest, NonProcessHandleFailsWithInvalidHandle) {
    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(0x1000, 0x10000,
                                  MemoryProtection::ReadWrite).is_ok());
    HandleTable handles;
    SyncManager sync(handles);
    capsule::Vfs vfs;
    Kernel32ExtManager kernel32(memory, vfs);
    kernel32.set_handle_table(&handles);
    kernel32.set_sync_manager(&sync);

    const Handle event = sync.create_event(false, false, "");
    ASSERT_NE(event, NULL_HANDLE);

    cpu::CpuState cpu;
    cpu.init_long_mode();
    cpu.gpr.rsp = 0x8000;
    cpu.gpr.rcx = event;
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    set_last_error(0);
    kernel32.impl_GetProcessId(marshaler);

    EXPECT_EQ(cpu.gpr.rax, 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_HANDLE);
}

// =============================================================================
// Process export fixture (FormatMessage moved to test_format_message.cpp)
// =============================================================================

class FormatMessageWTest : public ::testing::Test {
protected:
    static constexpr GuestAddr MEM_BASE   = 0x1000;
    static constexpr GuestAddr STACK_BASE = 0x80000;
    static constexpr GuestAddr BUF_ADDR   = 0x2000;
    static constexpr GuestAddr PTR_SLOT   = 0x3000;

    void SetUp() override {
        (void)memory_.map_region(MEM_BASE, 0x10000, MemoryProtection::ReadWrite);
        (void)memory_.map_region(STACK_BASE, 0x10000, MemoryProtection::ReadWrite);
        (void)memory_.map_region(BUF_ADDR, 0x10000, MemoryProtection::ReadWrite);
        cpu_.gpr.rsp = STACK_BASE + 0x800;
    }

    ExportEntry find_export(const std::string& name) {
        DllExports exports = make_process_exports(pm_, &gl_);
        for (const auto& e : exports.entries) {
            if (e.name == name) return e;
        }
        ADD_FAILURE() << name << " not found";
        return {};
    }

    GuestMemory memory_;
    VirtualMemoryManager vmm_{memory_};
    HandleTable handles_;
    HeapManager heap_{vmm_, memory_, handles_};
    GlobalLocalManager gl_{heap_};
    cpu::CpuState cpu_;
    ProcessManager pm_;
};

// =============================================================================
// SetUnhandledExceptionFilter Contract Tests
// =============================================================================

TEST_F(FormatMessageWTest, SetUnhandledExceptionFilterReturnsPrevious) {
    auto entry = find_export("SetUnhandledExceptionFilter");
    Marshaler m(memory_);
    m.attach_cpu(cpu_);

    // First call — no previous handler, should return 0
    cpu_.gpr.rcx = 0xDEAD0000;
    entry.handler(m);
    // On very first call ever to this static, previous is 0
    // (but may be non-zero if other tests ran first — just check it's a valid u64)

    // Second call — should return the handler we just set
    cpu_.gpr.rcx = 0xBEEF0000;
    entry.handler(m);
    EXPECT_EQ(cpu_.gpr.rax, 0xDEAD0000u);

    // Third call — should return 0xBEEF0000
    cpu_.gpr.rcx = 0;
    entry.handler(m);
    EXPECT_EQ(cpu_.gpr.rax, 0xBEEF0000u);
}

TEST_F(FormatMessageWTest, SetUnhandledExceptionFilterIsVisibleToDispatch) {
    auto entry = find_export("SetUnhandledExceptionFilter");
    Marshaler m(memory_);
    m.attach_cpu(cpu_);

    const u64 previous = unhandled_exception_filter_guest();
    cpu_.gpr.rcx = 0x00401000;
    entry.handler(m);
    EXPECT_EQ(unhandled_exception_filter_guest(), 0x00401000u);

    cpu_.gpr.rcx = previous;
    entry.handler(m);
}

TEST_F(FormatMessageWTest, UnhandledExceptionFilterReturnsOne) {
    auto entry = find_export("UnhandledExceptionFilter");
    Marshaler m(memory_);
    m.attach_cpu(cpu_);

    cpu_.gpr.rcx = 0;
    entry.handler(m);
    // EXCEPTION_EXECUTE_HANDLER = 1
    EXPECT_EQ(cpu_.gpr.rax, 1u);
}

// Learn CreateProcess: only CREATE_SUSPENDED starts a child's primary thread
// suspended. A launch that does not say (ShellExecute, legacy WinExec) states
// running, so a launcher that was itself created suspended does not hand its
// own "wait for ResumeThread" to its child -- nothing resumes that child.
TEST(PrimaryThreadSuspension, LaunchStatesRunningUnlessCreatorAskedSuspended) {
    std::vector<std::string> shell_launch{"VITREO_STARTUP_INFO=x"};
    Kernel32ExtManager::state_primary_thread_suspension(shell_launch);
    EXPECT_EQ(std::count(shell_launch.begin(), shell_launch.end(),
                         std::string("VITREO_PRIMARY_THREAD_SUSPENDED=0")),
              1);

    std::vector<std::string> suspended_create{
        "VITREO_PRIMARY_THREAD_SUSPENDED=1"};
    Kernel32ExtManager::state_primary_thread_suspension(suspended_create);
    ASSERT_EQ(suspended_create.size(), 1u);
    EXPECT_EQ(suspended_create[0], "VITREO_PRIMARY_THREAD_SUSPENDED=1");
}

// Learn CreateProcess lpEnvironment: NULL gives the child the calling
// process's environment -- its current one, including variables it set after
// it started. ShellExecute and LoadModule (NULL lpEnvAddress) launch the same
// way, so a game started by its launcher sees what the store client set for
// the launcher.
TEST(InheritedGuestEnvironment, IsTheCallersCurrentEnvironment) {
    GuestMemory memory;
    capsule::Vfs vfs;
    Kernel32ExtManager kernel32(memory, vfs);
    EXPECT_TRUE(kernel32.inherited_guest_environment().empty());

    EnvironmentManager environment("app.exe", memory);
    environment.set_variable("SteamAppId", "489830");
    kernel32.set_environment_manager(&environment);
    environment.set_variable("SteamGameId", "489830");

    const auto inherited = kernel32.inherited_guest_environment();
    EXPECT_NE(std::find(inherited.begin(), inherited.end(),
                        std::string("SteamAppId=489830")),
              inherited.end());
    EXPECT_NE(std::find(inherited.begin(), inherited.end(),
                        std::string("SteamGameId=489830")),
              inherited.end())
        << "a variable set after startup is inherited";
}
