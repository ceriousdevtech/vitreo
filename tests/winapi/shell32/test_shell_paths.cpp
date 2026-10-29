#include <gtest/gtest.h>
#include <vitreo/common.hpp>
#include "winapi/system/shell32/shell_paths.hpp"
#include "winapi/com/ole32/com_core.hpp"
#include "winapi/com/ole32/task_memory.hpp"
#include "winapi/kernel32/handle.hpp"
#include "winapi/kernel32/memory/heap.hpp"
#include "winapi/kernel32/memory/global_local.hpp"
#include "winapi/kernel32/memory/memory.hpp"
#include "winapi/kernel32/process/process.hpp"
#include "capsule/vfs/vfs.hpp"
#include "capsule/vfs/host_filesystem.hpp"
#include "windowing/cocoa/cocoa_bridge.hpp"
#include "core/memory/guest_memory.hpp"
#include "core/cpu/state/cpu_state.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/core/marshaler.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <atomic>
#include <barrier>
#include <thread>


using namespace vitreo;
using namespace vitreo::winapi;

class ShellPathTest : public ::testing::Test {
protected:
    capsule::Vfs             vfs;
    windowing::CocoaBridge   bridge;
    GuestMemory              memory;
    HandleTable              handles;
    VirtualMemoryManager     vmm{memory};
    HeapManager              heap_mgr{vmm, memory, handles};
    ComTaskMemoryManager     task_memory{heap_mgr};
    ComCoreManager           com_mgr{task_memory};
    ProcessManager           process_mgr;
    GlobalLocalManager       local_mgr{heap_mgr};
    ShellPathManager         mgr{
        vfs, bridge, memory, process_mgr, task_memory};

    void SetUp() override { mgr.set_local_memory(&local_mgr); }

    void check_concurrent_namespace_lifetimes(bool is64) {
        ThunkRegistry registry(0x70000000);
        ASSERT_TRUE(registry.register_dll("shell32.dll", create_shell_path_thunks(mgr)).is_ok());
        mgr.set_thunk_registry(registry);
        constexpr GuestAddr main_base = 0x10000;
        constexpr usize workers = 8;
        ASSERT_TRUE(memory.map_region(main_base, 0x4000, MemoryProtection::ReadWrite).is_ok());
        for (usize i = 0; i < workers; ++i)
            ASSERT_TRUE(memory.map_region(0x20000 + i * 0x4000, 0x4000,
                                          MemoryProtection::ReadWrite).is_ok());
        const auto attach = [&](cpu::CpuState& cpu, Marshaler& m, GuestAddr base) {
            if (is64) cpu.init_long_mode(); else cpu.init_protected_mode();
            cpu.set_sp(base + 0x100);
            m.attach_cpu(cpu);
        };
        const auto argument = [&](cpu::CpuState& cpu, u64 value) {
            if (is64) cpu.gpr.rcx = value;
            else (void)memory.write_u32(cpu.sp() + 4, static_cast<u32>(value));
        };
        cpu::CpuState main_cpu;
        Marshaler main_m(memory);
        attach(main_cpu, main_m, main_base);
        argument(main_cpu, main_base + 0x800);
        mgr.sh_get_desktop_folder(main_m);
        ASSERT_EQ(main_cpu.gpr.rax, 0u);
        const GuestAddr desktop = main_m.read_ptr(main_base + 0x800).value();
        ASSERT_NE(desktop, 0u);
        argument(main_cpu, desktop);
        mgr.shell_folder_add_ref(main_m);
        const u64 before = main_cpu.gpr.rax;
        mgr.shell_folder_release(main_m);

        std::barrier start(static_cast<std::ptrdiff_t>(workers));
        std::atomic<u32> failures{0};
        std::vector<std::thread> threads;
        for (usize i = 0; i < workers; ++i) threads.emplace_back([&, i] {
            cpu::CpuState cpu;
            Marshaler m(memory);
            const GuestAddr base = 0x20000 + i * 0x4000;
            attach(cpu, m, base);
            start.arrive_and_wait();
            for (int repeat = 0; repeat < 512; ++repeat) {
                argument(cpu, base + 0x800);
                mgr.sh_get_desktop_folder(m);
                const auto folder = m.read_ptr(base + 0x800);
                if (cpu.gpr.rax != 0 || folder.is_err() || folder.value() == 0) {
                    ++failures; continue;
                }
                argument(cpu, folder.value());
                mgr.shell_folder_add_ref(m);
                if (cpu.gpr.rax == 0) ++failures;
                mgr.shell_folder_release(m);
                mgr.shell_folder_release(m);

                GuestAddr pidl = 0;
                if (mgr.get_special_folder_location(csidl::CSIDL_DRIVES, pidl) != hr::S_OK || !pidl) {
                    ++failures; continue;
                }
                std::wstring path;
                (void)mgr.get_path_from_pidl(pidl, path);
                mgr.sh_free(pidl);
                argument(cpu, base + 0x808);
                mgr.sh_get_malloc(m);
                const auto allocator = m.read_ptr(base + 0x808);
                if (cpu.gpr.rax != 0 || allocator.is_err() || allocator.value() == 0)
                    ++failures;
            }
        });
        for (auto& thread : threads) thread.join();
        EXPECT_EQ(failures.load(), 0u);
        argument(main_cpu, desktop);
        mgr.shell_folder_add_ref(main_m);
        // AddRef/Release balance comes from IUnknown's documented counter
        // contract; the allocator's initial reference count is not assumed.
        EXPECT_EQ(main_cpu.gpr.rax, before);
        mgr.shell_folder_release(main_m);
        mgr.shell_folder_release(main_m);
    }

    std::vector<std::u16string> argv(std::u16string_view cmd,
                                     std::u16string_view exe = u"C:\\app\\a.exe") {
        std::vector<std::u16string> args;
        mgr.command_line_to_argv(std::u16string(cmd), std::u16string(exe), args);
        return args;
    }
};

TEST_F(ShellPathTest, ConcurrentNamespaceLifetimes32) {
    check_concurrent_namespace_lifetimes(false);
}

TEST_F(ShellPathTest, ConcurrentNamespaceLifetimes64) {
    check_concurrent_namespace_lifetimes(true);
}

TEST_F(ShellPathTest, InitNetworkAddressControlIsExportedAndSucceeds) {
    auto exports = create_shell_path_thunks(mgr);
    auto it = std::find_if(
        exports.entries.begin(), exports.entries.end(),
        [](const ExportEntry& entry) {
            return entry.name == "InitNetworkAddressControl";
        });
    ASSERT_NE(it, exports.entries.end());
    EXPECT_EQ(it->stdcall_bytes, 0u);

    cpu::CpuState cpu;
    cpu.init_protected_mode();
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    it->handler(marshaler);
    EXPECT_EQ(cpu.gpr.rax, 1u);
}

TEST_F(ShellPathTest, FindExecutableWResolvesUnicodeAndReportsMissingAssociation) {
    char root[] = "/tmp/vitreo-find-executable-XXXXXX";
    ASSERT_NE(mkdtemp(root), nullptr);
    ASSERT_TRUE(vfs.mount_drive('C',
        std::make_unique<capsule::HostFileSystem>(root)).is_ok());
    std::ofstream(std::filesystem::path(root) / "café.exe") << "test";
    std::ofstream(std::filesystem::path(root) / "document.txt") << "test";
    constexpr GuestAddr base = 0x140000000;
    ASSERT_TRUE(memory.map_region(base, 0x2000, MemoryProtection::ReadWrite).is_ok());
    auto exports = create_shell_path_thunks(mgr);
    const auto entry = std::find_if(exports.entries.begin(), exports.entries.end(),
        [](const auto& e) { return e.name == "FindExecutableW"; });
    ASSERT_NE(entry, exports.entries.end());
    EXPECT_EQ(entry->stdcall_bytes, 12u);
    cpu::CpuState cpu;
    cpu.init_long_mode();
    Marshaler m(memory);
    m.attach_cpu(cpu);
    cpu.gpr.rcx = base;
    cpu.gpr.rdx = base + 0x400;
    cpu.gpr.r8 = base + 0x800;
    ASSERT_TRUE(m.write_wide_string(base + 0x400, "C:\\").is_ok());
    ASSERT_TRUE(m.write_wide_string(base, "café.exe").is_ok());
    entry->handler(m);
    EXPECT_GT(cpu.gpr.rax, 32u);
    EXPECT_EQ(m.read_wide_string(base + 0x800).value(), "C:\\café.exe");
    ASSERT_TRUE(m.write_wide_string(base, "document.txt").is_ok());
    entry->handler(m);
    EXPECT_EQ(cpu.gpr.rax, 31u);
    EXPECT_EQ(memory.read_u16(base + 0x800).value(), 0u);
    ASSERT_TRUE(m.write_wide_string(base, "absent.txt").is_ok());
    entry->handler(m);
    EXPECT_EQ(cpu.gpr.rax, 2u);
    std::filesystem::remove_all(root);
}

TEST_F(ShellPathTest, SHCreateAssociationRegistrationIsExportedAndFailsHonestly) {
    auto exports = create_shell_path_thunks(mgr);
    auto it = std::find_if(
        exports.entries.begin(), exports.entries.end(),
        [](const ExportEntry& entry) {
            return entry.name == "SHCreateAssociationRegistration";
        });
    ASSERT_NE(it, exports.entries.end());
    EXPECT_EQ(it->stdcall_bytes, 8u);

    constexpr GuestAddr stack = 0x10000;
    constexpr GuestAddr out = 0x11000;
    ASSERT_TRUE(memory.map_region(stack, 0x2000,
                                  MemoryProtection::ReadWrite).is_ok());
    ASSERT_TRUE(memory.write_u64(out, 0xDEADBEEF).is_ok());

    cpu::CpuState cpu;
    cpu.init_long_mode();
    cpu.gpr.rsp = stack + 0x800;
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    // REFIID, void**
    cpu.gpr.rcx = out + 0x80;
    cpu.gpr.rdx = out;
    it->handler(marshaler);
    EXPECT_EQ(cpu.gpr.rax, 0x80040154u);  // REGDB_E_CLASSNOTREG
    EXPECT_EQ(memory.read_u64(out).value(), 0u);

    // A null ppv is a parameter error, and must not be written through.
    cpu.gpr.rdx = 0;
    it->handler(marshaler);
    EXPECT_EQ(cpu.gpr.rax, 0x80004003u);  // E_POINTER
}

TEST_F(ShellPathTest, ExplicitAppUserModelIdRoundTripsAndMatchesWindowsBoundsX86) {
    constexpr GuestAddr data = 0x10000;
    constexpr GuestAddr stack = 0x12000;
    constexpr GuestAddr output = 0x13000;
    ASSERT_TRUE(memory.map_region(data, 0x4000,
                                  MemoryProtection::ReadWrite).is_ok());

    cpu::CpuState cpu;
    cpu.init_protected_mode();
    cpu.set_sp(stack);
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);

    auto exports = create_shell_path_thunks(mgr);
    const auto set_export = std::find_if(
        exports.entries.begin(), exports.entries.end(),
        [](const ExportEntry& entry) {
            return entry.name == "SetCurrentProcessExplicitAppUserModelID";
        });
    const auto get_export = std::find_if(
        exports.entries.begin(), exports.entries.end(),
        [](const ExportEntry& entry) {
            return entry.name == "GetCurrentProcessExplicitAppUserModelID";
        });
    ASSERT_NE(set_export, exports.entries.end());
    ASSERT_NE(get_export, exports.entries.end());

    ASSERT_TRUE(memory.write_u32(stack + 4, output).is_ok());
    ASSERT_TRUE(memory.write_u32(output, 0xFFFFFFFFu).is_ok());
    get_export->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), hr::E_FAIL);
    EXPECT_EQ(memory.read_u32(output).value(), 0u);

    const auto call_set = [&](const std::u16string& value) {
        for (usize index = 0; index < value.size(); ++index) {
            EXPECT_TRUE(memory.write_u16(
                data + index * sizeof(u16),
                static_cast<u16>(value[index])).is_ok());
        }
        EXPECT_TRUE(memory.write_u16(
            data + value.size() * sizeof(u16), 0).is_ok());
        EXPECT_TRUE(memory.write_u32(stack + 4, data).is_ok());
        set_export->handler(marshaler);
        return static_cast<u32>(cpu.gpr.rax);
    };

    EXPECT_EQ(call_set(u""), hr::S_OK);
    EXPECT_EQ(call_set(u"Vitreo Test App"), hr::S_OK);
    const std::u16string maximum(127, u'A');
    EXPECT_EQ(call_set(maximum), hr::S_OK);
    EXPECT_EQ(call_set(std::u16string(128, u'B')), hr::E_INVALIDARG);

    ASSERT_TRUE(memory.write_u32(stack + 4, output).is_ok());
    ASSERT_TRUE(memory.write_u32(output, 0xFFFFFFFFu).is_ok());
    get_export->handler(marshaler);
    ASSERT_EQ(static_cast<u32>(cpu.gpr.rax), hr::S_OK);
    const GuestAddr returned = memory.read_u32(output).value();
    ASSERT_NE(returned, 0u);
    for (usize index = 0; index < maximum.size(); ++index) {
        EXPECT_EQ(memory.read_u16(returned + index * sizeof(u16)).value(),
                  static_cast<u16>(u'A'));
    }
    EXPECT_EQ(memory.read_u16(
        returned + maximum.size() * sizeof(u16)).value(), 0u);
    EXPECT_TRUE(task_memory.owns(returned));

    auto ole_exports = create_com_core_thunks(com_mgr);
    const auto free_export = std::find_if(
        ole_exports.entries.begin(), ole_exports.entries.end(),
        [](const ExportEntry& entry) {
            return entry.name == "CoTaskMemFree";
        });
    ASSERT_NE(free_export, ole_exports.entries.end());
    ASSERT_TRUE(memory.write_u32(stack + 4, returned).is_ok());
    free_export->handler(marshaler);
    EXPECT_FALSE(task_memory.owns(returned));

    ASSERT_TRUE(memory.write_u32(stack + 4, 0).is_ok());
    set_export->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), hr::E_INVALIDARG);
}

TEST_F(ShellPathTest, ExplicitAppUserModelIdWritesPointerSizedOutputX64) {
    constexpr GuestAddr data = 0x10000;
    constexpr GuestAddr output = 0x11000;
    ASSERT_TRUE(memory.map_region(data, 0x2000,
                                  MemoryProtection::ReadWrite).is_ok());

    const std::u16string value = u"Vitreo.Test.App";
    for (usize index = 0; index < value.size(); ++index) {
        ASSERT_TRUE(memory.write_u16(
            data + index * sizeof(u16),
            static_cast<u16>(value[index])).is_ok());
    }
    ASSERT_TRUE(memory.write_u16(
        data + value.size() * sizeof(u16), 0).is_ok());

    cpu::CpuState cpu;
    cpu.mode = cpu::CpuMode::Long;
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);

    auto exports = create_shell_path_thunks(mgr);
    const auto set_export = std::find_if(
        exports.entries.begin(), exports.entries.end(),
        [](const ExportEntry& entry) {
            return entry.name == "SetCurrentProcessExplicitAppUserModelID";
        });
    const auto get_export = std::find_if(
        exports.entries.begin(), exports.entries.end(),
        [](const ExportEntry& entry) {
            return entry.name == "GetCurrentProcessExplicitAppUserModelID";
        });
    ASSERT_NE(set_export, exports.entries.end());
    ASSERT_NE(get_export, exports.entries.end());

    cpu.gpr.rcx = data;
    set_export->handler(marshaler);
    ASSERT_EQ(static_cast<u32>(cpu.gpr.rax), hr::S_OK);

    ASSERT_TRUE(memory.write_u64(output, ~u64{0}).is_ok());
    cpu.gpr.rcx = output;
    get_export->handler(marshaler);
    ASSERT_EQ(static_cast<u32>(cpu.gpr.rax), hr::S_OK);
    const GuestAddr returned = memory.read_u64(output).value();
    ASSERT_NE(returned, 0u);
    EXPECT_EQ(returned >> 32, 0u);
    for (usize index = 0; index < value.size(); ++index) {
        EXPECT_EQ(memory.read_u16(returned + index * sizeof(u16)).value(),
                  static_cast<u16>(value[index]));
    }
    EXPECT_EQ(memory.read_u16(
        returned + value.size() * sizeof(u16)).value(), 0u);
    EXPECT_TRUE(task_memory.owns(returned));

    auto ole_exports = create_com_core_thunks(com_mgr);
    const auto free_export = std::find_if(
        ole_exports.entries.begin(), ole_exports.entries.end(),
        [](const ExportEntry& entry) {
            return entry.name == "CoTaskMemFree";
        });
    ASSERT_NE(free_export, ole_exports.entries.end());
    cpu.gpr.rcx = returned;
    free_export->handler(marshaler);
    EXPECT_FALSE(task_memory.owns(returned));
}

// 1. GetFolderPath_AppData — returns Roaming path, S_OK
TEST_F(ShellPathTest, GetFolderPath_AppData) {
    std::wstring path;
    u32 hr = mgr.get_folder_path(csidl::CSIDL_APPDATA, path);
    EXPECT_EQ(hr, hr::S_OK);
    EXPECT_EQ(path, L"C:\\Users\\Default\\AppData\\Roaming");
}

// 2. GetFolderPath_Windows — returns C:\Windows, S_OK
TEST_F(ShellPathTest, GetFolderPath_Windows) {
    std::wstring path;
    u32 hr = mgr.get_folder_path(csidl::CSIDL_WINDOWS, path);
    EXPECT_EQ(hr, hr::S_OK);
    EXPECT_EQ(path, L"C:\\Windows");
}

// 3. GetFolderPath_System — returns System32, S_OK
TEST_F(ShellPathTest, GetFolderPath_System) {
    std::wstring path;
    u32 hr = mgr.get_folder_path(csidl::CSIDL_SYSTEM, path);
    EXPECT_EQ(hr, hr::S_OK);
    EXPECT_EQ(path, L"C:\\Windows\\system32");
}

TEST_F(ShellPathTest, GetFolderPath_CommonProgramFilesX86) {
    std::wstring path;
    const u32 result =
        mgr.get_folder_path(csidl::CSIDL_PROGRAM_FILES_COMMONX86, path);
    EXPECT_EQ(result, hr::S_OK);
    EXPECT_EQ(path, L"C:\\Program Files (x86)\\Common Files");
}

TEST_F(ShellPathTest, GetFolderPath_CommonDocuments) {
    std::wstring path;
    EXPECT_EQ(mgr.get_folder_path(csidl::CSIDL_COMMON_DOCUMENTS, path), hr::S_OK);
    EXPECT_EQ(path, L"C:\\Users\\Public\\Documents");
    EXPECT_EQ(mgr.get_known_folder_path("PublicDocuments", path), hr::S_OK);
    EXPECT_EQ(path, L"C:\\Users\\Public\\Documents");
}

TEST_F(ShellPathTest, GetFolderPath_CommonProgramFilesMasksCreateFlag) {
    char root[] = "/tmp/vitreo-folder-create-XXXXXX";
    ASSERT_NE(mkdtemp(root), nullptr);
    ASSERT_TRUE(vfs.mount_drive('C',
        std::make_unique<capsule::HostFileSystem>(root)).is_ok());
    std::wstring path;
    constexpr i32 csidl_flag_create = 0x8000;
    const u32 result = mgr.get_folder_path(
        csidl::CSIDL_PROGRAM_FILES_COMMON | csidl_flag_create, path);
    EXPECT_EQ(result, hr::S_OK);
    EXPECT_EQ(path, L"C:\\Program Files\\Common Files");
    EXPECT_TRUE(std::filesystem::is_directory(
        std::filesystem::path(root) / "Program Files/Common Files"));
    std::filesystem::remove_all(root);
}

TEST_F(ShellPathTest, CommonDocumentsCreateMakesParentsAndRejectsFile) {
    char root[] = "/tmp/vitreo-public-documents-XXXXXX";
    ASSERT_NE(mkdtemp(root), nullptr);
    ASSERT_TRUE(vfs.mount_drive('C',
        std::make_unique<capsule::HostFileSystem>(root)).is_ok());
    std::wstring path;
    EXPECT_EQ(mgr.get_folder_path(0x802e, path), hr::S_OK);
    const auto documents = std::filesystem::path(root) / "Users/Public/Documents";
    EXPECT_TRUE(std::filesystem::is_directory(documents));
    EXPECT_EQ(mgr.get_folder_path(0x802e, path), hr::S_OK);
    std::filesystem::remove(documents);
    std::ofstream(documents) << "a file is not a directory";
    EXPECT_NE(mgr.get_folder_path(0x802e, path), hr::S_OK);
    EXPECT_TRUE(path.empty());
    std::filesystem::remove_all(root);
}

// 4. GetFolderPath_UnknownCSIDL — returns E_INVALIDARG
TEST_F(ShellPathTest, GetFolderPath_UnknownCSIDL) {
    std::wstring path;
    u32 hr = mgr.get_folder_path(0x7F, path);
    EXPECT_EQ(hr, hr::E_INVALIDARG);
}

// 5. GetKnownFolderPath_Documents — returns Documents path, S_OK
TEST_F(ShellPathTest, GetKnownFolderPath_Documents) {
    std::wstring path;
    u32 hr = mgr.get_known_folder_path("Documents", path);
    EXPECT_EQ(hr, hr::S_OK);
    EXPECT_EQ(path, L"C:\\Users\\Default\\Documents");
}

// 6. GetKnownFolderPath_UnknownFolder — ERROR_FILE_NOT_FOUND (probe kf_bogus)
TEST_F(ShellPathTest, GetKnownFolderPath_UnknownFolder) {
    std::wstring path;
    u32 hr = mgr.get_known_folder_path("NoSuchFolder", path);
    EXPECT_EQ(hr, 0x80070002u);
}

// Every CSIDL row of shell_paths_probe.expected that has a path, plus the
// virtual (E_FAIL) and undefined (E_INVALIDARG) ones.
TEST_F(ShellPathTest, GetFolderPathCoversWindowsCsidls) {
    std::wstring path;
    EXPECT_EQ(mgr.get_folder_path(0x28, path), hr::S_OK);
    EXPECT_EQ(path, L"C:\\Users\\Default");
    EXPECT_EQ(mgr.get_folder_path(0x27, path), hr::S_OK);
    EXPECT_EQ(path, L"C:\\Users\\Default\\Pictures");
    EXPECT_EQ(mgr.get_folder_path(0x21, path), hr::S_OK);
    EXPECT_EQ(path, L"C:\\Users\\Default\\AppData\\Local\\Microsoft\\Windows\\INetCookies");
    EXPECT_EQ(mgr.get_folder_path(0x29, path), hr::S_OK);
    EXPECT_EQ(path, L"C:\\Windows\\SysWOW64");
    EXPECT_EQ(mgr.get_folder_path(0x26, path, false, false), hr::S_OK);
    EXPECT_EQ(path, L"C:\\Program Files (x86)");  // WOW64 view
    EXPECT_EQ(mgr.get_folder_path(0x11, path), 0x80004005u);  // CSIDL_DRIVES
    EXPECT_EQ(mgr.get_folder_path(0x0C, path), hr::E_INVALIDARG);
}

TEST_F(ShellPathTest, ShellExecuteLaunchesChildWithoutExitingCaller) {
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() /
        "vitreo-shell-execute-process-test";
    std::error_code error;
    fs::remove_all(root, error);
    ASSERT_TRUE(fs::create_directories(root / "Agent", error));
    std::ofstream(root / "Agent" / "Agent.exe", std::ios::binary).put('\0');
    ASSERT_TRUE(vfs.mount_drive('C',
        std::make_unique<capsule::HostFileSystem>(root.string())).is_ok());

    std::string launched_path;
    std::string launched_parameters;
    std::string launched_directory;
    i32 launched_show = -1;
    mgr.set_executable_launcher(
        [&](const std::string& path, const std::string& parameters,
            const std::string& directory, i32 show_cmd,
            bool request_elevation, u32& process_handle, u32& error_code) {
            launched_path = path;
            launched_parameters = parameters;
            launched_directory = directory;
            launched_show = show_cmd;
            EXPECT_FALSE(request_elevation);
            process_handle = 0x44;
            error_code = error_codes::ERROR_SUCCESS;
            return true;
        });

    u32 process_handle = 0;
    EXPECT_GT(mgr.shell_execute(
        "open", "C:\\Agent\\Agent.exe", "--session=123", "C:\\Agent", 5,
        &process_handle), 32u);
    EXPECT_EQ(launched_path, "C:\\Agent\\Agent.exe");
    EXPECT_EQ(launched_parameters, "--session=123");
    // Learn SHELLEXECUTEINFO: lpDirectory and nShow reach the child.
    EXPECT_EQ(launched_directory, "C:\\Agent");
    EXPECT_EQ(launched_show, 5);
    EXPECT_EQ(process_handle, 0x44u);
    EXPECT_FALSE(process_mgr.has_exited());
    EXPECT_FALSE(process_mgr.has_pending_launch());

    fs::remove_all(root, error);
}

TEST_F(ShellPathTest, RunAsRequestsElevationFromLauncher) {
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() /
        "vitreo-shell-runas-process-test";
    std::error_code error;
    fs::remove_all(root, error);
    ASSERT_TRUE(fs::create_directories(root / "Setup", error));
    std::ofstream(root / "Setup" / "Setup.exe", std::ios::binary).put('\0');
    ASSERT_TRUE(vfs.mount_drive('C',
        std::make_unique<capsule::HostFileSystem>(root.string())).is_ok());

    bool requested_elevation = false;
    mgr.set_executable_launcher(
        [&](const std::string&, const std::string&, const std::string&, i32,
            bool request_elevation, u32&, u32& error_code) {
            requested_elevation = request_elevation;
            error_code = error_codes::ERROR_CANCELLED;
            return false;
        });

    set_last_error(error_codes::ERROR_SUCCESS);
    EXPECT_EQ(mgr.shell_execute(
        "runas", "C:\\Setup\\Setup.exe", "", "", 5), 5u);
    EXPECT_TRUE(requested_elevation);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_CANCELLED);

    fs::remove_all(root, error);
}

TEST(ShellPathElevationTest, IsUserAnAdminUsesProcessState) {
    capsule::Vfs vfs;
    windowing::CocoaBridge bridge;
    GuestMemory memory;
    HandleTable handles;
    VirtualMemoryManager vmm{memory};
    HeapManager heap_mgr{vmm, memory, handles};
    ComTaskMemoryManager task_memory{heap_mgr};
    ProcessManager process_mgr;
    GuestElevationState elevated{true};
    ShellPathManager mgr{
        vfs, bridge, memory, process_mgr, task_memory, &elevated};

    EXPECT_EQ(mgr.is_user_an_admin(), 1u);
}

// CommandLineToArgvW: every case below is a line of
// tools/oracle_probe/shell_paths_probe.expected (argv case=N).
TEST_F(ShellPathTest, CommandLineToArgvMatchesWindowsOracle) {
    using V = std::vector<std::u16string>;
    EXPECT_EQ(argv(u"prog.exe a b"), (V{u"prog.exe", u"a", u"b"}));
    EXPECT_EQ(argv(u"\"C:\\dir\\\\\" x"), (V{u"C:\\dir\\\\", u"x"}));
    EXPECT_EQ(argv(u"a\\\\\\\"b c"), (V{u"a\\\\\\\"b", u"c"}));
    EXPECT_EQ(argv(u"a\\\\\"b c\" d"), (V{u"a\\\\\"b", u"c d"}));
    EXPECT_EQ(argv(u"  lead x"), (V{u"", u"lead", u"x"}));
    EXPECT_EQ(argv(u"prog \"\" y"), (V{u"prog", u"", u"y"}));
    EXPECT_EQ(argv(u"prog\tz\tw"), (V{u"prog", u"z", u"w"}));
    EXPECT_EQ(argv(u"\"C:\\a b\\p.exe\" \"q\\\"r\""), (V{u"C:\\a b\\p.exe", u"q\"r"}));
    EXPECT_EQ(argv(u"C:\\x\\\"y\" z"), (V{u"C:\\x\\\"y\"", u"z"}));
    EXPECT_EQ(argv(u"p a\\\\b"), (V{u"p", u"a\\\\b"}));
    EXPECT_EQ(argv(u"p \"a\"\"b\""), (V{u"p", u"a\"b"}));
    EXPECT_EQ(argv(u"p \u00e9t\u00e9"), (V{u"p", u"\u00e9t\u00e9"}));
    EXPECT_EQ(argv(u""), (V{u"C:\\app\\a.exe"}));
}

TEST_F(ShellPathTest, CommandLineToArgvPreservesCrashpadHandlerTypeToken) {
    const std::u16string cmd =
        u"C:\\Users\\Default\\AppData\\Roaming\\Spotify\\Spotify.exe "
        u"--type=crashpad-handler /prefetch:4 --max-uploads=5 "
        u"--monitor-self-annotation=ptype=crashpad-handler "
        u"\"--database=C:\\Users\\Default\\AppData\\Local\\Spotify\\User Data\\Crashpad\"";
    const auto args = argv(cmd);
    auto it = std::find(args.begin(), args.end(), u"--type=crashpad-handler");
    ASSERT_NE(it, args.end());
}

TEST_F(ShellPathTest, CommandLineToArgvResultIsOneLocalAllocBlock) {
    constexpr GuestAddr command_addr = 0x10000;
    constexpr GuestAddr argc_addr = 0x11000;
    ASSERT_TRUE(memory.map_region(command_addr, 0x2000,
                                  MemoryProtection::ReadWrite).is_ok());
    // Non-ASCII arguments stay UTF-16 (the old UTF-8 detour split 'é').
    const std::u16string command = u"p \u00e9";
    for (usize index = 0; index < command.size(); ++index) {
        ASSERT_TRUE(memory.write_u16(command_addr + index * 2, command[index]).is_ok());
    }
    ASSERT_TRUE(memory.write_u16(command_addr + command.size() * 2, 0).is_ok());
    cpu::CpuState cpu;
    cpu.mode = cpu::CpuMode::Long;
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    const GuestAddr argv_block =
        mgr.command_line_to_argv_guest(marshaler, command_addr, argc_addr);
    ASSERT_NE(argv_block, 0u);
    ASSERT_EQ(memory.read_u32(argc_addr).value(), 2u);
    const GuestAddr arg1 = memory.read_u64(argv_block + 8).value();
    EXPECT_EQ(memory.read_u16(arg1).value(), 0x00E9u);
    EXPECT_EQ(memory.read_u16(arg1 + 2).value(), 0u);
    // A single LocalFree releases it (shell_paths_probe localFreeOk=1).
    EXPECT_EQ(local_mgr.global_free(argv_block), 0u);
    // pNumArgs NULL -> NULL + ERROR_INVALID_PARAMETER (argv_nullcount).
    EXPECT_EQ(mgr.command_line_to_argv_guest(marshaler, command_addr, 0), 0u);
}

TEST_F(ShellPathTest, CommandLineToArgvGuestUsesPointerSizedX64Layout) {
    constexpr GuestAddr command_addr = 0x10000;
    constexpr GuestAddr argc_addr = 0x11000;
    ASSERT_TRUE(memory.map_region(command_addr, 0x2000,
                                  MemoryProtection::ReadWrite).is_ok());
    const std::u16string command = u"program.exe --type=crashpad-handler";
    for (usize index = 0; index < command.size(); ++index) {
        ASSERT_TRUE(memory.write_u16(command_addr + index * 2,
                                     command[index]).is_ok());
    }
    ASSERT_TRUE(memory.write_u16(command_addr + command.size() * 2, 0).is_ok());

    cpu::CpuState cpu;
    cpu.mode = cpu::CpuMode::Long;
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    const GuestAddr argv = mgr.command_line_to_argv_guest(
        marshaler, command_addr, argc_addr);

    ASSERT_NE(argv, 0u);
    ASSERT_EQ(memory.read_u32(argc_addr).value(), 2u);
    const GuestAddr argv0 = memory.read_u64(argv).value();
    const GuestAddr argv1 = memory.read_u64(argv + 8).value();
    EXPECT_NE(argv0, 0u);
    EXPECT_NE(argv1, 0u);
    EXPECT_EQ(memory.read_u64(argv + 16).value(), 0u);
    EXPECT_EQ(memory.read_u16(argv0).value(), static_cast<u16>('p'));
    EXPECT_EQ(memory.read_u16(argv1).value(), static_cast<u16>('-'));
    std::u16string argv1_text;
    for (u32 i = 0; i < 32; ++i) {
        const auto ch = memory.read_u16(argv1 + i * 2);
        ASSERT_TRUE(ch.is_ok());
        if (ch.value() == 0) {
            break;
        }
        argv1_text.push_back(static_cast<char16_t>(ch.value()));
    }
    EXPECT_EQ(argv1_text, u"--type=crashpad-handler");
}

TEST_F(ShellPathTest, DesktopFolderEnumeratesMountedDrivesAndMallocFreesPidls) {
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / "vitreo-shell-namespace-test";
    std::error_code error;
    fs::remove_all(root, error);
    ASSERT_TRUE(fs::create_directories(root / "c", error));
    ASSERT_TRUE(fs::create_directories(root / "d", error));
    ASSERT_TRUE(vfs.mount_drive('C',
        std::make_unique<capsule::HostFileSystem>((root / "c").string())).is_ok());
    ASSERT_TRUE(vfs.mount_drive('D',
        std::make_unique<capsule::HostFileSystem>((root / "d").string())).is_ok());

    ThunkRegistry registry(0x70000000);
    ASSERT_TRUE(registry.register_dll("shell32.dll", create_shell_path_thunks(mgr)).is_ok());
    mgr.set_thunk_registry(registry);

    constexpr GuestAddr stack = 0x10000;
    constexpr GuestAddr output = 0x11000;
    ASSERT_TRUE(memory.map_region(stack, 0x2000, MemoryProtection::ReadWrite).is_ok());
    cpu::CpuState cpu;
    cpu.init_protected_mode();
    cpu.set_sp(stack + 0x800);
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);

    (void)memory.write_u32(cpu.sp() + 4, static_cast<u32>(output));
    mgr.sh_get_desktop_folder(marshaler);
    ASSERT_EQ(cpu.gpr.rax, 0u);
    const GuestAddr desktop = memory.read_u32(output).value();
    ASSERT_NE(desktop, 0u);
    ASSERT_NE(memory.read_u32(desktop).value(), 0u);

    (void)memory.write_u32(cpu.sp() + 4, static_cast<u32>(desktop));
    (void)memory.write_u32(cpu.sp() + 8, 0);
    (void)memory.write_u32(cpu.sp() + 12, 0x20);
    (void)memory.write_u32(cpu.sp() + 16, static_cast<u32>(output + 4));
    mgr.shell_folder_enum_objects(marshaler);
    ASSERT_EQ(cpu.gpr.rax, 0u);
    const GuestAddr enumerator = memory.read_u32(output + 4).value();
    ASSERT_NE(enumerator, 0u);

    (void)memory.write_u32(cpu.sp() + 4, static_cast<u32>(enumerator));
    (void)memory.write_u32(cpu.sp() + 8, 1);
    (void)memory.write_u32(cpu.sp() + 12, static_cast<u32>(output + 8));
    (void)memory.write_u32(cpu.sp() + 16, static_cast<u32>(output + 12));
    mgr.enum_id_list_next(marshaler);
    ASSERT_EQ(cpu.gpr.rax, 0u);
    ASSERT_EQ(memory.read_u32(output + 12).value(), 1u);
    const GuestAddr drives_pidl = memory.read_u32(output + 8).value();
    std::wstring path;
    ASSERT_TRUE(mgr.get_path_from_pidl(drives_pidl, path));
    EXPECT_EQ(path, L"shell:Drives");

    (void)memory.write_u32(cpu.sp() + 4, static_cast<u32>(desktop));
    (void)memory.write_u32(cpu.sp() + 8, static_cast<u32>(drives_pidl));
    (void)memory.write_u32(cpu.sp() + 12, 0);
    (void)memory.write_u32(cpu.sp() + 16, 0);
    (void)memory.write_u32(cpu.sp() + 20, static_cast<u32>(output + 20));
    mgr.shell_folder_bind_to_object(marshaler);
    ASSERT_EQ(cpu.gpr.rax, 0u);
    const GuestAddr drives_folder = memory.read_u32(output + 20).value();
    ASSERT_NE(drives_folder, 0u);

    (void)memory.write_u32(cpu.sp() + 4, static_cast<u32>(drives_folder));
    (void)memory.write_u32(cpu.sp() + 8, 0);
    (void)memory.write_u32(cpu.sp() + 12, 0x20);
    (void)memory.write_u32(cpu.sp() + 16, static_cast<u32>(output + 24));
    mgr.shell_folder_enum_objects(marshaler);
    ASSERT_EQ(cpu.gpr.rax, 0u);
    const GuestAddr drive_enumerator = memory.read_u32(output + 24).value();

    (void)memory.write_u32(cpu.sp() + 4, static_cast<u32>(drive_enumerator));
    (void)memory.write_u32(cpu.sp() + 8, 1);
    (void)memory.write_u32(cpu.sp() + 12, static_cast<u32>(output + 28));
    (void)memory.write_u32(cpu.sp() + 16, static_cast<u32>(output + 32));
    mgr.enum_id_list_next(marshaler);
    ASSERT_EQ(cpu.gpr.rax, 0u);
    const GuestAddr drive_pidl = memory.read_u32(output + 28).value();
    ASSERT_TRUE(mgr.get_path_from_pidl(drive_pidl, path));
    EXPECT_EQ(path, L"C:\\");

    (void)memory.write_u32(cpu.sp() + 4, static_cast<u32>(drives_folder));
    (void)memory.write_u32(cpu.sp() + 8, static_cast<u32>(drive_pidl));
    (void)memory.write_u32(cpu.sp() + 12, 0x8000);
    (void)memory.write_u32(cpu.sp() + 16, static_cast<u32>(output + 40));
    mgr.shell_folder_get_display_name(marshaler);
    ASSERT_EQ(cpu.gpr.rax, 0u);
    EXPECT_EQ(memory.read_u32(output + 40).value(), 2u);
    EXPECT_EQ(memory.read_u8(output + 44).value(), static_cast<u8>('C'));
    EXPECT_EQ(memory.read_u8(output + 45).value(), static_cast<u8>(':'));
    EXPECT_EQ(memory.read_u8(output + 46).value(), static_cast<u8>('\\'));
    EXPECT_EQ(memory.read_u8(output + 47).value(), 0u);

    (void)memory.write_u32(cpu.sp() + 4, static_cast<u32>(output + 16));
    mgr.sh_get_malloc(marshaler);
    ASSERT_EQ(cpu.gpr.rax, 0u);
    const GuestAddr allocator = memory.read_u32(output + 16).value();
    ASSERT_NE(allocator, 0u);

    (void)memory.write_u32(cpu.sp() + 4, static_cast<u32>(allocator));
    (void)memory.write_u32(cpu.sp() + 8, static_cast<u32>(drives_pidl));
    // SHGetMalloc is the COM task allocator (Learn), and PIDLs come from it:
    // IMalloc::Free returns the block to CoTaskMem.
    EXPECT_TRUE(task_memory.owns(drives_pidl));
    mgr.shell_malloc_free(marshaler);
    EXPECT_FALSE(task_memory.owns(drives_pidl));

    fs::remove_all(root, error);
}

TEST_F(ShellPathTest, SpecialFolderLocationDrivesReturnsVirtualNamespacePidl) {
    GuestAddr pidl = 0;
    ASSERT_EQ(mgr.get_special_folder_location(csidl::CSIDL_DRIVES, pidl), hr::S_OK);
    ASSERT_NE(pidl, 0u);
    EXPECT_TRUE(memory.is_mapped(pidl, 2));
    EXPECT_GT(memory.read_u16(pidl).value(), 2u);

    std::wstring path;
    ASSERT_TRUE(mgr.get_path_from_pidl(pidl, path));
    EXPECT_EQ(path, L"shell:Drives");

    const u16 item_size = memory.read_u16(pidl).value();
    const GuestAddr copy = mgr.alloc_shell_memory(memory, item_size + 2);
    ASSERT_NE(copy, 0u);
    std::vector<Byte> bytes(item_size + 2);
    ASSERT_TRUE(memory.read_bytes(pidl, bytes.data(), bytes.size()).is_ok());
    ASSERT_TRUE(memory.write_bytes(copy, bytes.data(), bytes.size()).is_ok());
    ASSERT_TRUE(mgr.get_path_from_pidl(copy, path));
    EXPECT_EQ(path, L"shell:Drives");
}

// 11. SHCreateDirectoryExW: every code measured on Windows 11
// (tools/oracle_probe/s7_shell_probe.expected "== SHCreateDirectoryExW").
// The return value is also the last error.
TEST_F(ShellPathTest, CreateDirectoryExMatchesWindowsCodes) {
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / "vitreo-shcreatedir-test";
    std::error_code error;
    fs::remove_all(root, error);
    ASSERT_TRUE(fs::create_directories(root, error));
    std::ofstream(root / "f.txt").put('x');
    ASSERT_TRUE(vfs.mount_drive('C',
        std::make_unique<capsule::HostFileSystem>(root.string())).is_ok());

    EXPECT_EQ(mgr.create_directory_ex("C:\\t\\a\\b\\c"), sh_err::ERROR_SUCCESS);
    EXPECT_EQ(get_last_error(), sh_err::ERROR_SUCCESS);
    EXPECT_TRUE(fs::is_directory(root / "t" / "a" / "b" / "c"));
    EXPECT_EQ(mgr.create_directory_ex("C:\\t\\a\\b\\c"), sh_err::ERROR_ALREADY_EXISTS);
    EXPECT_EQ(get_last_error(), sh_err::ERROR_ALREADY_EXISTS);
    EXPECT_EQ(mgr.create_directory_ex("C:\\f.txt"), sh_err::ERROR_ALREADY_EXISTS);
    EXPECT_EQ(mgr.create_directory_ex("C:\\f.txt\\sub"), sh_err::ERROR_PATH_NOT_FOUND);
    EXPECT_EQ(mgr.create_directory_ex("rel\\x"), sh_err::ERROR_BAD_PATHNAME);
    EXPECT_EQ(get_last_error(), sh_err::ERROR_BAD_PATHNAME);
    EXPECT_FALSE(fs::exists(root / "rel"));
    EXPECT_EQ(mgr.create_directory_ex(""), sh_err::ERROR_BAD_PATHNAME);
    EXPECT_EQ(mgr.create_directory_ex("\\\\nosuchserver\\share\\x"), sh_err::ERROR_BAD_NETPATH);
    EXPECT_EQ(mgr.create_directory_ex("Q:\\s7\\x"), sh_err::ERROR_PATH_NOT_FOUND);
    std::string p248 = "C:\\";
    while (p248.size() < 248) p248.push_back('y');
    EXPECT_EQ(mgr.create_directory_ex(p248), sh_err::ERROR_FILENAME_EXCED_RANGE);
    EXPECT_EQ(get_last_error(), sh_err::ERROR_FILENAME_EXCED_RANGE);
    p248.pop_back();
    EXPECT_EQ(mgr.create_directory_ex(p248), sh_err::ERROR_SUCCESS);
    std::string p300 = "C:\\";
    while (p300.size() < 300) p300.push_back('x');
    EXPECT_EQ(mgr.create_directory_ex(p300), sh_err::ERROR_FILENAME_EXCED_RANGE);
    EXPECT_EQ(get_last_error(), sh_err::ERROR_PATH_NOT_FOUND);

    // ILCreateFromPathW: NULL + file-system error for a missing item; a real
    // task-allocated PIDL (ILFree/CoTaskMemFree) that round-trips otherwise.
    EXPECT_EQ(mgr.il_create_from_path("C:\\missing_dir\\x.txt"), 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_PATH_NOT_FOUND);
    const GuestAddr pidl = mgr.il_create_from_path("C:\\t\\a");
    ASSERT_NE(pidl, 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_PARAMETER);
    EXPECT_TRUE(task_memory.owns(pidl));
    std::wstring back;
    ASSERT_TRUE(mgr.get_path_from_pidl(pidl, back));
    EXPECT_EQ(back, L"C:\\t\\a");
    mgr.sh_free(pidl);
    EXPECT_FALSE(task_memory.owns(pidl));

    fs::remove_all(root, error);
}

// 12. GetFolderPath_ProgramFiles — returns correct path
TEST_F(ShellPathTest, GetFolderPath_ProgramFiles) {
    std::wstring path;
    u32 hr = mgr.get_folder_path(csidl::CSIDL_PROGRAM_FILES, path);
    EXPECT_EQ(hr, hr::S_OK);
    EXPECT_EQ(path, L"C:\\Program Files");
}

// 13. GetFolderPath_Programs — used by NSIS $SMPROGRAMS shortcuts
TEST_F(ShellPathTest, GetFolderPath_Programs) {
    std::wstring path;
    u32 hr = mgr.get_folder_path(csidl::CSIDL_PROGRAMS, path);
    EXPECT_EQ(hr, hr::S_OK);
    // Vista+ layout (shell_paths_probe.expected csidl 0x02).
    EXPECT_EQ(path, L"C:\\Users\\Default\\AppData\\Roaming\\Microsoft\\Windows\\Start Menu\\Programs");
}

// 14. GetFolderPath_StartMenu — used by installers for menu roots
TEST_F(ShellPathTest, GetFolderPath_StartMenu) {
    std::wstring path;
    u32 hr = mgr.get_folder_path(csidl::CSIDL_STARTMENU, path);
    EXPECT_EQ(hr, hr::S_OK);
    EXPECT_EQ(path, L"C:\\Users\\Default\\AppData\\Roaming\\Microsoft\\Windows\\Start Menu");
}

TEST_F(ShellPathTest, CopyOperationWritesExactDestinationName) {
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / "vitreo-shfileop-copy";
    std::error_code error;
    fs::remove_all(root, error);
    ASSERT_TRUE(fs::create_directories(root, error));
    const std::vector<u8> payload{'M', 'Z', 0x90, 0x00};
    {
        std::ofstream stream(root / "payload.ex_", std::ios::binary);
        stream.write(reinterpret_cast<const char*>(payload.data()),
                     static_cast<std::streamsize>(payload.size()));
    }
    ASSERT_TRUE(vfs.mount_drive(
        'C', std::make_unique<capsule::HostFileSystem>(root.string())).is_ok());

    EXPECT_EQ(
        mgr.copy_operation({"C:\\payload.ex_"}, {"C:\\payload.exe"}), 0);
    std::ifstream in(root / "payload.exe", std::ios::binary);
    std::vector<u8> copied(
        (std::istreambuf_iterator<char>(in)),
        std::istreambuf_iterator<char>());
    EXPECT_EQ(copied, payload);

    fs::remove_all(root, error);
}

TEST_F(ShellPathTest, CopyOperationIntoDirectoryKeepsSourceLeafName) {
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / "vitreo-shfileop-copydir";
    std::error_code error;
    fs::remove_all(root, error);
    ASSERT_TRUE(fs::create_directories(root / "dest", error));
    {
        std::ofstream stream(root / "source.bin", std::ios::binary);
        stream.put('x');
    }
    ASSERT_TRUE(vfs.mount_drive(
        'C', std::make_unique<capsule::HostFileSystem>(root.string())).is_ok());

    EXPECT_EQ(
        mgr.copy_operation({"C:\\source.bin"}, {"C:\\dest"}), 0);
    EXPECT_TRUE(fs::exists(root / "dest" / "source.bin"));

    fs::remove_all(root, error);
}

// The NSIS/electron-builder install step is
// SHFileOperation(FO_MOVE, "<temp>\7z-out\*" -> "<programs>\<app>"): a
// wildcard source, a staged tree of files and directories, and a destination
// directory that already exists. Vitreo used to open the literal "...\*",
// fail, and report DE_ACCESSDENIEDSRC, which the installer answered by
// retrying the move forever — the install "hang".
TEST_F(ShellPathTest, WildcardSourceExpandsToDirectoryContents) {
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / "vitreo-shfileop-glob";
    std::error_code error;
    fs::remove_all(root, error);
    ASSERT_TRUE(fs::create_directories(root / "stage" / "resources" / "sub", error));
    ASSERT_TRUE(fs::create_directories(root / "dest" / "resources", error));
    {
        std::ofstream(root / "stage" / "app.exe", std::ios::binary).put('a');
        std::ofstream(root / "stage" / "notes.txt", std::ios::binary).put('b');
        std::ofstream(root / "stage" / "resources" / "sub" / "deep.dat",
                      std::ios::binary).put('c');
    }
    ASSERT_TRUE(vfs.mount_drive(
        'C', std::make_unique<capsule::HostFileSystem>(root.string())).is_ok());

    bool had_wildcard = false;
    const std::vector<std::string> expanded =
        mgr.expand_wildcard_sources({"C:\\stage\\*"}, had_wildcard);
    EXPECT_TRUE(had_wildcard);
    ASSERT_EQ(expanded.size(), 3u);
    EXPECT_EQ(expanded[0], "C:\\stage\\app.exe");
    EXPECT_EQ(expanded[1], "C:\\stage\\notes.txt");
    EXPECT_EQ(expanded[2], "C:\\stage\\resources");

    // The whole tree lands inside the destination directory, and the existing
    // "resources" folder there is merged rather than refused.
    EXPECT_EQ(mgr.copy_operation({"C:\\stage\\*"}, {"C:\\dest"}), 0);
    EXPECT_TRUE(fs::exists(root / "dest" / "app.exe"));
    EXPECT_TRUE(fs::exists(root / "dest" / "notes.txt"));
    EXPECT_TRUE(fs::exists(root / "dest" / "resources" / "sub" / "deep.dat"));

    fs::remove_all(root, error);
}

// A wildcard that expands to exactly one entry still means "the destination is
// the directory it lands in" — not "rename that entry to the destination".
TEST_F(ShellPathTest, SingleWildcardMatchStillCopiesIntoDestination) {
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / "vitreo-shfileop-glob1";
    std::error_code error;
    fs::remove_all(root, error);
    ASSERT_TRUE(fs::create_directories(root / "stage", error));
    std::ofstream(root / "stage" / "only.bin", std::ios::binary).put('z');
    ASSERT_TRUE(vfs.mount_drive(
        'C', std::make_unique<capsule::HostFileSystem>(root.string())).is_ok());

    EXPECT_EQ(mgr.copy_operation({"C:\\stage\\*"}, {"C:\\dest"}), 0);
    EXPECT_TRUE(fs::exists(root / "dest" / "only.bin"));
    EXPECT_FALSE(fs::is_regular_file(root / "dest"));

    fs::remove_all(root, error);
}

// remove_tree is what FO_DELETE needs: VFS remove() alone only takes an empty
// directory, so a non-recursive FO_DELETE silently left the tree in place.
TEST_F(ShellPathTest, RemoveTreeDeletesPopulatedDirectory) {
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / "vitreo-shfileop-rmtree";
    std::error_code error;
    fs::remove_all(root, error);
    ASSERT_TRUE(fs::create_directories(root / "tree" / "inner", error));
    std::ofstream(root / "tree" / "inner" / "leaf.txt", std::ios::binary).put('q');
    ASSERT_TRUE(vfs.mount_drive(
        'C', std::make_unique<capsule::HostFileSystem>(root.string())).is_ok());

    EXPECT_TRUE(mgr.remove_tree("C:\\tree"));
    EXPECT_FALSE(fs::exists(root / "tree"));

    fs::remove_all(root, error);
}

// shell_paths_probe.expected: CSIDL_COMMON_OEM_LINKS (0x3a) is absent on a
// stock install, so the verified lookup fails with ERROR_FILE_NOT_FOUND, but
// CSIDL_FLAG_DONT_VERIFY returns "%ProgramData%\OEM Links"
// (csidl_oem_dontverify). The generated table used to hold only the failure.
TEST_F(ShellPathTest, CommonOemLinksHonoursDontVerify) {
    std::wstring path;
    EXPECT_EQ(mgr.get_folder_path(0x3A, path, true), 0x80070002u);
    EXPECT_TRUE(path.empty());
    EXPECT_EQ(mgr.get_folder_path(0x3A | 0x4000, path, true), hr::S_OK);
    EXPECT_EQ(path, L"C:\\ProgramData\\OEM Links");
}

// S7 audit, x64 layouts measured in tools/oracle_probe/s7_shell_probe.expected:
// SHGetFileInfoW wrote x86 offsets on x64 and faked an HICON;
// ShellExecuteExA read x86 SHELLEXECUTEINFO offsets on x64;
// SHChangeNotifyRegister was missing (#2 was bound to SHGetKnownFolderPath).
TEST_F(ShellPathTest, S7X64FileInfoExecuteExAAndChangeNotify) {
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / "vitreo-s7-shell-x64";
    std::error_code error;
    fs::remove_all(root, error);
    ASSERT_TRUE(fs::create_directories(root / "d", error));
    std::ofstream(root / "f.txt").put('x');
    ASSERT_TRUE(vfs.mount_drive('C',
        std::make_unique<capsule::HostFileSystem>(root.string())).is_ok());
    constexpr GuestAddr kData = 0x100000;
    constexpr GuestAddr kStack = 0x200000;
    ASSERT_TRUE(memory.map_region(kData, 0x10000, MemoryProtection::ReadWrite).is_ok());
    ASSERT_TRUE(memory.map_region(kStack, 0x1000, MemoryProtection::ReadWrite).is_ok());
    cpu::CpuState cpu;
    cpu.init_long_mode();
    cpu.gpr.rsp = kStack + 0x800;
    Marshaler m(memory);
    m.attach_cpu(cpu);
    constexpr GuestAddr kPath = kData;
    constexpr GuestAddr kInfo = kData + 0x1000;
    const auto file_info = [&](const char* path, u32 flags) {
        EXPECT_TRUE(m.write_wide_string(kPath, path).is_ok());
        for (u32 i = 0; i < 0x2B8; ++i) EXPECT_TRUE(memory.write_u8(kInfo + i, 0xCC).is_ok());
        cpu.gpr.rcx = kPath;
        cpu.gpr.rdx = 0;
        cpu.gpr.r8 = kInfo;
        cpu.gpr.r9 = 0x2B8;
        EXPECT_TRUE(memory.write_u64(cpu.gpr.rsp + 0x28, flags).is_ok());
        set_last_error(0xDEAD);
        return mgr.get_file_info(m);
    };
    constexpr u32 kDisplay = 0x200, kType = 0x400, kAttrs = 0x800;
    EXPECT_EQ(file_info("C:\\d", kDisplay | kType | kAttrs), 1u);
    EXPECT_EQ(get_last_error(), 0u);
    EXPECT_EQ(memory.read_u64(kInfo).value(), 0u);                 // hIcon
    EXPECT_EQ(memory.read_u32(kInfo + 8).value(), 0xCCCCCCCCu);    // iIcon untouched
    EXPECT_EQ(memory.read_u32(kInfo + 12).value(), 0xF080017Fu);
    EXPECT_EQ(m.read_wide_string(kInfo + 16).value(), "d");
    EXPECT_EQ(m.read_wide_string(kInfo + 536).value(), "File folder");
    EXPECT_EQ(file_info("C:\\f.txt", kDisplay | kType | kAttrs), 1u);
    EXPECT_EQ(memory.read_u32(kInfo + 12).value(), 0x40400177u);
    EXPECT_EQ(m.read_wide_string(kInfo + 536).value(), "Text Document");
    EXPECT_EQ(file_info("C:\\f.txt", kAttrs), 1u);
    EXPECT_EQ(memory.read_u16(kInfo + 16).value(), 0u);            // cleared
    EXPECT_EQ(memory.read_u16(kInfo + 536).value(), 0u);
    EXPECT_EQ(file_info("C:\\s7_definitely_missing_dir\\x.txt", kAttrs), 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_PATH_NOT_FOUND);

    auto exports = create_shell_path_thunks(mgr);
    const auto find = [&](std::string_view name) -> const ExportEntry* {
        for (const auto& e : exports.entries) if (e.name == name) return &e;
        return nullptr;
    };
    // ShellExecuteExA (x64 SHELLEXECUTEINFOA): lpFile @24, hInstApp @56,
    // hProcess @104. A missing document: FALSE, hInstApp SE_ERR_FNF (2),
    // last error 2, hProcess NULL.
    const auto* see_a = find("ShellExecuteExA");
    ASSERT_NE(see_a, nullptr);
    EXPECT_EQ(see_a->ordinal, 576u);
    constexpr GuestAddr kSei = kData + 0x3000;
    constexpr GuestAddr kFile = kData + 0x3200;
    for (u32 i = 0; i < 112; ++i) ASSERT_TRUE(memory.write_u8(kSei + i, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(kSei, 112).is_ok());
    const char missing[] = "Q:\\s7_missing.txt";
    for (usize i = 0; i < sizeof(missing); ++i) {
        ASSERT_TRUE(memory.write_u8(kFile + i, static_cast<u8>(missing[i])).is_ok());
    }
    ASSERT_TRUE(memory.write_u64(kSei + 24, kFile).is_ok());
    ASSERT_TRUE(memory.write_u64(kSei + 56, 0x7777).is_ok());
    ASSERT_TRUE(memory.write_u64(kSei + 104, 0x7777).is_ok());
    cpu.gpr.rcx = kSei;
    see_a->handler(m);
    EXPECT_EQ(cpu.gpr.rax, 0u);
    EXPECT_EQ(memory.read_u64(kSei + 56).value(), 2u);
    EXPECT_EQ(memory.read_u64(kSei + 104).value(), 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_FILE_NOT_FOUND);

    // SHChangeNotifyRegister #2: NULL hwnd -> 0 + ERROR_INVALID_WINDOW_HANDLE.
    // SHChangeNotifyDeregister #4: an unknown id -> FALSE, last error 0.
    const auto* reg = find("SHChangeNotifyRegister");
    const auto* dereg = find("SHChangeNotifyDeregister");
    ASSERT_NE(reg, nullptr);
    ASSERT_NE(dereg, nullptr);
    EXPECT_EQ(reg->ordinal, 2u);
    EXPECT_EQ(dereg->ordinal, 4u);
    EXPECT_EQ(reg->stdcall_bytes, 24u);
    cpu.gpr.rcx = 0;
    cpu.gpr.rdx = 0x1000;  // SHCNRF_ShellLevel
    cpu.gpr.r8 = 0x7FFFFFFF;
    cpu.gpr.r9 = 0x407;
    ASSERT_TRUE(memory.write_u64(cpu.gpr.rsp + 0x28, 1).is_ok());
    ASSERT_TRUE(memory.write_u64(cpu.gpr.rsp + 0x30, kData + 0x4000).is_ok());
    reg->handler(m);
    EXPECT_EQ(cpu.gpr.rax, 0u);
    EXPECT_EQ(get_last_error(), 1400u);
    set_last_error(0xDEAD);
    cpu.gpr.rcx = 0x7777;
    dereg->handler(m);
    EXPECT_EQ(cpu.gpr.rax, 0u);
    EXPECT_EQ(get_last_error(), 0u);
    fs::remove_all(root, error);
}

// HDROP: an HGLOBAL holding DROPFILES {pFiles, pt, fNC, fWide} and a
// double-null-terminated file list. Expected values are
// tools/oracle_probe/shell_hdrop_probe.expected (Windows, identical x86/x64).
TEST_F(ShellPathTest, DragQueryPointAndFileReadTheDropFilesBlock) {
    Marshaler m(memory);
    const char* files[] = {"C:\\a\\first.txt", "C:\\bb\\second file.dat"};
    auto make_drop = [&](bool wide, bool non_client, i32 x, i32 y) -> u64 {
        const u64 unit = wide ? 2 : 1;
        u64 chars = 1;
        for (const char* f : files) chars += std::strlen(f) + 1;
        const u64 handle = local_mgr.global_alloc(0x0042 /*MOVEABLE|ZEROINIT*/, 20 + chars * unit);
        EXPECT_NE(handle, 0u);
        const GuestAddr base = static_cast<GuestAddr>(local_mgr.global_lock(handle));
        EXPECT_NE(base, 0u);
        EXPECT_TRUE(m.write_u32(base, 20).is_ok());
        EXPECT_TRUE(m.write_u32(base + 4, static_cast<u32>(x)).is_ok());
        EXPECT_TRUE(m.write_u32(base + 8, static_cast<u32>(y)).is_ok());
        EXPECT_TRUE(m.write_u32(base + 12, non_client ? 1u : 0u).is_ok());
        EXPECT_TRUE(m.write_u32(base + 16, wide ? 1u : 0u).is_ok());
        GuestAddr p = base + 20;
        for (const char* f : files) {
            for (const char* c = f; *c != 0; ++c) {
                if (wide) { EXPECT_TRUE(m.write_u16(p, static_cast<u16>(*c)).is_ok()); p += 2; }
                else { EXPECT_TRUE(m.write_u8(p, static_cast<u8>(*c)).is_ok()); p += 1; }
            }
            p += unit;  // terminator (zero-initialised)
        }
        (void)local_mgr.global_unlock(handle);
        return handle;
    };
    constexpr GuestAddr kPoint = 0x30000, kBuf = 0x30100;
    ASSERT_TRUE(memory.map_region(0x30000, 0x1000, MemoryProtection::ReadWrite).is_ok());

    const u64 ansi_client = make_drop(false, false, 12, 34);
    const u64 wide_nonclient = make_drop(true, true, -5, 600);

    set_last_error(0xDEAD);
    EXPECT_EQ(mgr.drag_query_point(m, ansi_client, kPoint), 1u);
    EXPECT_EQ(m.read_u32(kPoint).value(), 12u);
    EXPECT_EQ(m.read_u32(kPoint + 4).value(), 34u);
    EXPECT_EQ(get_last_error(), 0u);
    EXPECT_EQ(mgr.drag_query_point(m, wide_nonclient, kPoint), 0u) << "fNC drop";
    EXPECT_EQ(static_cast<i32>(m.read_u32(kPoint).value()), -5);
    EXPECT_EQ(m.read_u32(kPoint + 4).value(), 600u);

    for (const u64 drop : {ansi_client, wide_nonclient}) {
        for (const bool wide : {false, true}) {
            SCOPED_TRACE(testing::Message() << "drop=" << drop << " wide=" << wide);
            EXPECT_EQ(mgr.drag_query_file(m, drop, 0xFFFFFFFFu, 0, 0, wide), 2u);
            EXPECT_EQ(mgr.drag_query_file(m, drop, 0, 0, 0, wide), 14u);
            EXPECT_EQ(mgr.drag_query_file(m, drop, 1, 0, 0, wide), 21u);
            EXPECT_EQ(mgr.drag_query_file(m, drop, 1, kBuf, 64, wide), 21u);
            const std::string name = wide ? m.read_wide_string(kBuf).value()
                                          : m.read_ansi_string(kBuf).value();
            EXPECT_EQ(name, "C:\\bb\\second file.dat");
            // cch 6: five characters and the terminator.
            EXPECT_EQ(mgr.drag_query_file(m, drop, 0, kBuf, 6, wide), 5u);
            const std::string cut = wide ? m.read_wide_string(kBuf).value()
                                         : m.read_ansi_string(kBuf).value();
            EXPECT_EQ(cut, "C:\\a\\");
            // Out of range: 0, buffer untouched.
            ASSERT_TRUE(m.write_u32(kBuf, 0x41414141u).is_ok());
            EXPECT_EQ(mgr.drag_query_file(m, drop, 2, kBuf, 64, wide), 0u);
            EXPECT_EQ(m.read_u32(kBuf).value(), 0x41414141u);
        }
    }

    // A NULL HDROP: 0, the point and the last error untouched.
    ASSERT_TRUE(m.write_u32(kPoint, 0xFFFFFFF9u).is_ok());
    set_last_error(0xDEAD);
    EXPECT_EQ(mgr.drag_query_point(m, 0, kPoint), 0u);
    EXPECT_EQ(m.read_u32(kPoint).value(), 0xFFFFFFF9u);
    EXPECT_EQ(mgr.drag_query_file(m, 0, 0xFFFFFFFFu, 0, 0, true), 0u);
    EXPECT_EQ(get_last_error(), 0xDEADu);

    mgr.drag_finish(ansi_client);
    mgr.drag_finish(wide_nonclient);
}
