/// @file test_kernel32_cpuz_process_environment.cpp
/// @brief Unit tests for KERNEL32 priority class, current directory, module file name and command-line contracts

#include "kernel32_cpuz_fixture.hpp"
#include <capsule/vfs/host_filesystem.hpp>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <string>
#include <unistd.h>
#include <winapi/kernel32/error.hpp>
#include <winapi/kernel32/environment/environment.hpp>
#include <winapi/kernel32/process/process.hpp>
#include <winapi/kernel32/module/module.hpp>

// =============================================================================
// Priority APIs
//
// Learn SetPriorityClass / GetPriorityClass: hProcess must be a process handle
// (PROCESS_SET_INFORMATION / PROCESS_QUERY_[LIMITED_]INFORMATION); failure
// returns 0 with GetLastError. Windows answers NULL or a value that is not a
// process handle with ERROR_INVALID_HANDLE, and success leaves the last error
// untouched (process_contract_probe.expected "== priority").
// =============================================================================

TEST_F(Kernel32CpuzTest, PriorityClass_CurrentProcessPseudoHandleRoundTrips32) {
    ProcessManager processes;
    mgr.set_process_manager(&processes);
    const u32 sp = static_cast<u32>(cpu.gpr.rsp);

    // Fresh process: NORMAL_PRIORITY_CLASS.
    ASSERT_TRUE(memory.write_u32(sp + 4, CURRENT_PROCESS_PSEUDO).is_ok());
    set_last_error(0xDEADu);
    mgr.impl_GetPriorityClass(marshaler);
    EXPECT_EQ(cpu.gpr.rax, 0x20u);
    EXPECT_EQ(get_last_error(), 0xDEADu);

    ASSERT_TRUE(memory.write_u32(sp + 8, 0x8000u).is_ok());  // ABOVE_NORMAL
    mgr.impl_SetPriorityClass(marshaler);
    EXPECT_EQ(cpu.gpr.rax, 1u);
    EXPECT_EQ(get_last_error(), 0xDEADu);

    mgr.impl_GetPriorityClass(marshaler);
    EXPECT_EQ(cpu.gpr.rax, 0x8000u);
    EXPECT_EQ(get_last_error(), 0xDEADu);
    mgr.set_process_manager(nullptr);
}

TEST_F(Kernel32CpuzTest, PriorityClass_CurrentProcessPseudoHandleRoundTrips64) {
    ProcessManager processes;
    mgr.set_process_manager(&processes);
    cpu.init_long_mode();
    cpu.gpr.rsp = STACK_BASE + 0x800;

    // x64 GetCurrentProcess() is (HANDLE)-1, all 64 bits set.
    cpu.gpr.rcx = ~u64{0};
    cpu.gpr.rdx = 0x40u;  // IDLE_PRIORITY_CLASS
    set_last_error(0xDEADu);
    mgr.impl_SetPriorityClass(marshaler);
    EXPECT_EQ(cpu.gpr.rax, 1u);

    cpu.gpr.rcx = ~u64{0};
    mgr.impl_GetPriorityClass(marshaler);
    EXPECT_EQ(cpu.gpr.rax, 0x40u);
    EXPECT_EQ(get_last_error(), 0xDEADu);
    mgr.set_process_manager(nullptr);
}

TEST_F(Kernel32CpuzTest, PriorityClass_NonProcessHandleFailsInvalidHandle) {
    ProcessManager processes;
    mgr.set_process_manager(&processes);
    const u32 sp = static_cast<u32>(cpu.gpr.rsp);

    for (const u32 handle : {0u, 0x1234u}) {
        ASSERT_TRUE(memory.write_u32(sp + 4, handle).is_ok());
        ASSERT_TRUE(memory.write_u32(sp + 8, 0x20u).is_ok());  // NORMAL
        set_last_error(0xDEADu);
        mgr.impl_SetPriorityClass(marshaler);
        EXPECT_EQ(cpu.gpr.rax, 0u) << "SetPriorityClass handle=" << handle;
        EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_HANDLE);

        set_last_error(0xDEADu);
        mgr.impl_GetPriorityClass(marshaler);
        EXPECT_EQ(cpu.gpr.rax, 0u) << "GetPriorityClass handle=" << handle;
        EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_HANDLE);
    }
    // The failed calls did not touch the current process's class.
    EXPECT_EQ(processes.priority_class(), 0x20u);
    mgr.set_process_manager(nullptr);
}

// SetCurrentDirectory only accepts an existing directory (Learn;
// files_ns_probe.expected), so each test mounts a real C: holding the
// directories it names.
namespace {
std::filesystem::path mount_temp_c(capsule::Vfs& vfs, const char* tag,
                                   std::initializer_list<const char*> dirs) {
    namespace fs = std::filesystem;
    const auto root = fs::temp_directory_path() /
        (std::string("vitreo_cwd_") + tag + "_" + std::to_string(::getpid()));
    fs::remove_all(root);
    fs::create_directories(root);
    for (const char* dir : dirs) fs::create_directories(root / dir);
    EXPECT_TRUE(vfs.mount_drive(
                    'C', std::make_unique<capsule::HostFileSystem>(root.string()))
                    .is_ok());
    return root;
}
}  // namespace

TEST_F(Kernel32CpuzTest, SetCurrentDirectoryAUpdatesVfsResolverState) {
    const auto root = mount_temp_c(vfs, "a", {"Program Files (x86)/Starcraft"});
    constexpr GuestAddr path = DATA_BASE;
    ASSERT_TRUE(marshaler.write_ansi_string(path, "C:\\Program Files (x86)\\Starcraft").is_ok());
    ASSERT_TRUE(memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 4), path).is_ok());

    set_last_error(0xDEADu);
    mgr.impl_SetCurrentDirectoryA(marshaler);

    EXPECT_EQ(cpu.gpr.rax, 1u);
    EXPECT_EQ(vfs.cwd(), "C:\\Program Files (x86)\\Starcraft");
    EXPECT_EQ(get_last_error(), 0xDEADu);  // last_error_on_success_probe SetCurrentDirectoryA
    std::filesystem::remove_all(root);
}

TEST_F(Kernel32CpuzTest, SetCurrentDirectoryWResolvesDotAgainstCurrentDirectory) {
    const auto root = mount_temp_c(vfs, "dot", {"Users/Default/AppData/Roaming/Example"});
    constexpr GuestAddr path = DATA_BASE;
    mgr.set_current_directory("C:\\Users\\Default\\AppData\\Roaming\\Example");
    write_guest_wide_string(path, ".");
    ASSERT_TRUE(
        memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 4), path).is_ok());

    set_last_error(0xDEADu);
    mgr.impl_SetCurrentDirectoryW(marshaler);

    EXPECT_EQ(cpu.gpr.rax, 1u);
    EXPECT_EQ(
        vfs.cwd(), "C:\\Users\\Default\\AppData\\Roaming\\Example");
    EXPECT_EQ(get_last_error(), 0xDEADu);  // success leaves it untouched (last_error_on_success_probe.expected)
    std::filesystem::remove_all(root);
}

TEST_F(Kernel32CpuzTest, SetCurrentDirectoryAResolvesParentComponent) {
    const auto root = mount_temp_c(vfs, "parent", {"Games/Example/Bin"});
    constexpr GuestAddr path = DATA_BASE;
    mgr.set_current_directory("C:\\Games\\Example\\Bin");
    ASSERT_TRUE(marshaler.write_ansi_string(path, "..").is_ok());
    ASSERT_TRUE(
        memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 4), path).is_ok());

    mgr.impl_SetCurrentDirectoryA(marshaler);

    EXPECT_EQ(cpu.gpr.rax, 1u);
    EXPECT_EQ(vfs.cwd(), "C:\\Games\\Example");
    std::filesystem::remove_all(root);
}

// files_ns_probe.expected SetCurrentDirectoryW_* rows. A failed call leaves
// the current directory where it was.
TEST_F(Kernel32CpuzTest, SetCurrentDirectoryWFollowsTheProbedErrors) {
    const auto root = mount_temp_c(vfs, "errors", {"Start", "Caf\xC3\xA9"});
    { std::ofstream(root / "plain.txt") << "x"; }
    mgr.set_current_directory("C:\\Start");
    constexpr GuestAddr path = DATA_BASE;
    ASSERT_TRUE(
        memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 4), path).is_ok());
    auto call = [&](const std::u16string& name) {
        for (u32 i = 0; i < name.size(); ++i) {
            (void)memory.write_u16(path + i * 2, static_cast<u16>(name[i]));
        }
        (void)memory.write_u16(path + static_cast<u32>(name.size()) * 2, 0);
        set_last_error(0xDEADu);
        mgr.impl_SetCurrentDirectoryW(marshaler);
        return std::pair<u64, u32>{cpu.gpr.rax, get_last_error()};
    };
    using Result = std::pair<u64, u32>;

    EXPECT_EQ(call(u"C:\\Missing"), (Result{0u, error_codes::ERROR_FILE_NOT_FOUND}));
    EXPECT_EQ(call(u"C:\\Missing\\Child"), (Result{0u, error_codes::ERROR_PATH_NOT_FOUND}));
    EXPECT_EQ(call(u"C:\\plain.txt"), (Result{0u, error_codes::ERROR_DIRECTORY}));
    EXPECT_EQ(call(u""), (Result{0u, error_codes::ERROR_INVALID_NAME}));
    EXPECT_EQ(vfs.cwd(), "C:\\Start");

    EXPECT_EQ(call(u"C:\\Caf\u00E9"), (Result{1u, 0xDEADu}));
    EXPECT_EQ(vfs.cwd(), "C:\\Caf\xC3\xA9");
    std::filesystem::remove_all(root);
}

TEST_F(Kernel32CpuzTest, GetModuleFileNameAWritesAcpRegisteredSign) {
    // Host module path is UTF-8. GetModuleFileNameA must write CP_ACP
    // (Learn *A TCHAR), so ® is 0xAE, not UTF-8 C2 AE.
    ThunkRegistry registry{0x7FFB00000000ull};
    ModuleManager modules{registry};
    modules.set_main_module(
        0x400000,
        "C:\\Program Files\\Atari\\RollerCoaster Tycoon\xC2\xAE 3\\RCT3.exe");

    constexpr GuestAddr buf = DATA_BASE + 0x400;
    ASSERT_TRUE(memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 4), 0).is_ok());
    ASSERT_TRUE(memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 8), buf).is_ok());
    ASSERT_TRUE(
        memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 12), 260).is_ok());

    impl_GetModuleFileNameA(marshaler, modules);

    EXPECT_GT(cpu.gpr.rax, 0u);
    bool found_ae = false;
    bool found_c2 = false;
    for (u32 i = 0; i < static_cast<u32>(cpu.gpr.rax); ++i) {
        const auto ch = memory.read_u8(buf + i);
        ASSERT_TRUE(ch.is_ok());
        if (ch.value() == 0xAE) found_ae = true;
        if (ch.value() == 0xC2) found_c2 = true;
    }
    EXPECT_TRUE(found_ae);
    EXPECT_FALSE(found_c2);
}

TEST_F(Kernel32CpuzTest, GetCurrentDirectoryAWritesAcpRegisteredSign) {
    mgr.set_current_directory(
        "C:\\Program Files\\Atari\\RollerCoaster Tycoon\xC2\xAE 3");
    constexpr GuestAddr buf = DATA_BASE + 0x400;
    ASSERT_TRUE(
        memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 4), 260).is_ok());
    ASSERT_TRUE(
        memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 8), buf).is_ok());

    mgr.impl_GetCurrentDirectoryA(marshaler);

    EXPECT_GT(cpu.gpr.rax, 0u);
    bool found_ae = false;
    bool found_c2 = false;
    for (u32 i = 0; i < static_cast<u32>(cpu.gpr.rax); ++i) {
        const auto ch = memory.read_u8(buf + i);
        ASSERT_TRUE(ch.is_ok());
        if (ch.value() == 0xAE) found_ae = true;
        if (ch.value() == 0xC2) found_c2 = true;
    }
    EXPECT_TRUE(found_ae);
    EXPECT_FALSE(found_c2);
}

TEST_F(Kernel32CpuzTest, GetCurrentDirectoryWDecodesUtf8ToUtf16) {
    // Host cwd is UTF-8. GetCurrentDirectoryW must write UTF-16 code units
    // (Learn), not widen each UTF-8 byte. ® is U+00AE, encoded C2 AE.
    mgr.set_current_directory(
        "C:\\Program Files\\Atari\\RollerCoaster Tycoon\xC2\xAE 3");
    constexpr GuestAddr buf = DATA_BASE + 0x400;
    ASSERT_TRUE(
        memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 4), 260).is_ok());
    ASSERT_TRUE(
        memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 8), buf).is_ok());

    mgr.impl_GetCurrentDirectoryW(marshaler);

    EXPECT_GT(cpu.gpr.rax, 0u);
    bool found_registered_sign = false;
    bool found_widened_c2 = false;
    for (u32 i = 0; i < static_cast<u32>(cpu.gpr.rax); ++i) {
        const auto ch = memory.read_u16(buf + i * 2);
        ASSERT_TRUE(ch.is_ok());
        if (ch.value() == 0x00AE) found_registered_sign = true;
        if (ch.value() == 0x00C2) found_widened_c2 = true;
    }
    EXPECT_TRUE(found_registered_sign);
    EXPECT_FALSE(found_widened_c2);
}

TEST_F(Kernel32CpuzTest, GetCommandLineAWritesAcpRegisteredSign) {
    // Host command line is UTF-8. GetCommandLineA must write CP_ACP
    // (Learn *A TCHAR), so ® is 0xAE, not UTF-8 C2 AE.
    EnvironmentManager em{
        "C:\\Program Files\\Atari\\RollerCoaster Tycoon\xC2\xAE 3\\RCT3.exe",
        memory};
    impl_GetCommandLineA(marshaler, em);
    const GuestAddr buf = static_cast<GuestAddr>(cpu.gpr.rax);
    ASSERT_NE(buf, 0u);

    bool found_ae = false;
    bool found_c2 = false;
    for (u32 i = 0; i < 260; ++i) {
        const auto ch = memory.read_u8(buf + i);
        ASSERT_TRUE(ch.is_ok());
        if (ch.value() == 0) break;
        if (ch.value() == 0xAE) found_ae = true;
        if (ch.value() == 0xC2) found_c2 = true;
    }
    EXPECT_TRUE(found_ae);
    EXPECT_FALSE(found_c2);
}

TEST_F(Kernel32CpuzTest, GetCommandLineWWritesUtf16RegisteredSign) {
    EnvironmentManager em{
        "C:\\Program Files\\Atari\\RollerCoaster Tycoon\xC2\xAE 3\\RCT3.exe",
        memory};
    impl_GetCommandLineW(marshaler, em);
    const GuestAddr buf = static_cast<GuestAddr>(cpu.gpr.rax);
    ASSERT_NE(buf, 0u);

    bool found_registered_sign = false;
    bool found_widened_c2 = false;
    for (u32 i = 0; i < 260; ++i) {
        const auto ch = memory.read_u16(buf + i * 2);
        ASSERT_TRUE(ch.is_ok());
        if (ch.value() == 0) break;
        if (ch.value() == 0x00AE) found_registered_sign = true;
        if (ch.value() == 0x00C2) found_widened_c2 = true;
    }
    EXPECT_TRUE(found_registered_sign);
    EXPECT_FALSE(found_widened_c2);
}

TEST_F(Kernel32CpuzTest, SetCurrentDirectoryWRejectsNullPath) {
    ASSERT_TRUE(
        memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 4), 0).is_ok());

    mgr.impl_SetCurrentDirectoryW(marshaler);

    EXPECT_EQ(cpu.gpr.rax, 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_PARAMETER);
}
