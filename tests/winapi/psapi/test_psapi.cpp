// SPDX-License-Identifier: LGPL-2.1-only
/// @file test_psapi.cpp
/// @brief psapi.dll contract tests. Expected values come from
/// tools/oracle_probe/s7_psapi_probe.expected (x64) / .x86.expected, measured
/// on Windows 11 through the psapi.dll exports.

#include <gtest/gtest.h>
#include <vitreo/common.hpp>
#include <winapi/system/psapi/psapi.hpp>
#include <winapi/kernel32/kernel32_ext.hpp>
#include <winapi/kernel32/error.hpp>
#include <winapi/kernel32/handle.hpp>
#include <winapi/kernel32/module/module.hpp>
#include <winapi/kernel32/process/process.hpp>
#include <winapi/core/marshaler.hpp>
#include <winapi/core/thunk_registry.hpp>
#include <capsule/vfs/vfs.hpp>
#include <capsule/vfs/host_filesystem.hpp>
#include <core/cpu/state/cpu_state.hpp>
#include <core/memory/guest_memory.hpp>
#include <ipc/process_handle.hpp>

#include <algorithm>
#include <cstring>
#include <string>
#include <unistd.h>
#include <filesystem>
#include <memory>

using namespace vitreo;
using namespace vitreo::winapi;

namespace {
constexpr u32 kProcessVmRead = 0x0010;
constexpr u32 kProcessQueryInformation = 0x0400;
constexpr u32 kProcessQueryLimitedInformation = 0x1000;
constexpr u32 kSynchronize = 0x00100000;
} // namespace

class PsapiManagerTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(memory_.map_region(MEM_BASE, 0x10000, MemoryProtection::ReadWrite).is_ok());
        ASSERT_TRUE(memory_.map_region(STACK_BASE, 0x1000, MemoryProtection::ReadWrite).is_ok());
        cpu_.gpr.rsp = STACK_BASE + 0x800;
        k32_.set_handle_table(&handles_);
        k32_.set_process_manager(&processes_);
        k32_.set_module_manager(&modules_);
        // A mapped PE image so GetModuleInformation has headers to read.
        ASSERT_TRUE(memory_.map_region(kImage, 0x1000, MemoryProtection::ReadWrite).is_ok());
        ASSERT_TRUE(memory_.write_u16(kImage, 0x5a4d).is_ok());
        ASSERT_TRUE(memory_.write_u32(kImage + 0x3c, 0x80).is_ok());
        ASSERT_TRUE(memory_.write_u32(kImage + 0x80, 0x4550).is_ok());
        ASSERT_TRUE(memory_.write_u32(kImage + 0x98 + 56, 0x123000).is_ok());
        ASSERT_TRUE(memory_.write_u32(kImage + 0x98 + 16, 0x1234).is_ok());
        modules_.register_module("sample.dll", kImage, "C:\\dir\\sample.dll");
    }

    void TearDown() override { k32_.set_module_manager(nullptr); }

    const ExportEntry& entry(const char* name) {
        exports_ = create_psapi_thunks(mgr_, k32_);
        auto it = std::find_if(exports_.entries.begin(), exports_.entries.end(),
            [&](const ExportEntry& e) { return e.name == name; });
        EXPECT_NE(it, exports_.entries.end()) << name;
        return *it;
    }

    /// x64 call with up to four register arguments.
    u64 call64(const char* name, u64 a0, u64 a1, u64 a2, u64 a3) {
        cpu_.init_long_mode();
        cpu_.set_sp(STACK_BASE + 0x800);
        cpu_.gpr.rcx = a0; cpu_.gpr.rdx = a1; cpu_.gpr.r8 = a2; cpu_.gpr.r9 = a3;
        Marshaler m(memory_);
        m.attach_cpu(cpu_);
        entry(name).handler(m);
        return cpu_.gpr.rax;
    }

    /// x86 stdcall with up to four stack arguments.
    u64 call32(const char* name, u32 a0, u32 a1, u32 a2, u32 a3) {
        cpu_.init_protected_mode();
        cpu_.gpr.rsp = STACK_BASE + 0x800;
        const u32 sp = static_cast<u32>(cpu_.gpr.rsp);
        (void)memory_.write_u32(sp + 4, a0);
        (void)memory_.write_u32(sp + 8, a1);
        (void)memory_.write_u32(sp + 12, a2);
        (void)memory_.write_u32(sp + 16, a3);
        Marshaler m(memory_);
        m.attach_cpu(cpu_);
        entry(name).handler(m);
        return cpu_.gpr.rax;
    }

    void fill(GuestAddr at, u32 n, u8 v = 0xCC) {
        for (u32 i = 0; i < n; ++i) (void)memory_.write_u8(at + i, v);
    }

    Handle self_handle(u32 access) {
        auto* process = ipc::track_process(::getpid());
        return handles_.allocate(HandleType::Process, process, 0, access);
    }

    static constexpr GuestAddr MEM_BASE   = 0x1000;
    static constexpr GuestAddr STACK_BASE = 0x80000;
    static constexpr GuestAddr NAME_ADDR  = MEM_BASE + 0x500;
    static constexpr GuestAddr OUT_ADDR   = MEM_BASE + 0x900;
    static constexpr GuestAddr NEEDED_ADDR = MEM_BASE + 0xF00;
    static constexpr u64 kImage = 0x140000000ull;
    static constexpr u64 kPseudo64 = ~u64{0};
    static constexpr u32 kPseudo32 = 0xFFFFFFFFu;

    GuestMemory memory_;
    cpu::CpuState cpu_;
    capsule::Vfs vfs_;
    HandleTable handles_;
    ProcessManager processes_;
    ThunkRegistry registry_{0x70000000};
    ModuleManager modules_{registry_};
    Kernel32ExtManager k32_{memory_, vfs_};
    PsapiManager mgr_;
    DllExports exports_;
};

// Ordinals and stdcall sizes of the Windows 10/11 psapi.dll export table
// (identical in System32 and SysWOW64).
TEST_F(PsapiManagerTest, OrdinalsMatchWindowsExportTable) {
    struct Row { const char* name; u16 ordinal; u16 bytes; };
    const Row rows[] = {
        {"EnumProcessModules", 5, 16},   {"EnumProcessModulesEx", 6, 20},
        {"EnumProcesses", 7, 12},        {"GetMappedFileNameA", 12, 16},
        {"GetMappedFileNameW", 13, 16},  {"GetModuleBaseNameA", 14, 16},
        {"GetModuleBaseNameW", 15, 16},  {"GetModuleFileNameExA", 16, 16},
        {"GetModuleFileNameExW", 17, 16},{"GetModuleInformation", 18, 16},
        {"GetPerformanceInfo", 19, 8},   {"GetProcessImageFileNameW", 21, 12},
        {"GetProcessMemoryInfo", 22, 12},
    };
    for (const auto& row : rows) {
        const ExportEntry& e = entry(row.name);
        EXPECT_EQ(e.ordinal, row.ordinal) << row.name;
        EXPECT_EQ(e.stdcall_bytes, row.bytes) << row.name;
    }
}

// Probe "== GetProcessMemoryInfo" (x64): 72/80/96-byte forms, cb records the
// form written, a short cb is ERROR_INSUFFICIENT_BUFFER with nothing written.
TEST_F(PsapiManagerTest, GetProcessMemoryInfoX64WritesLargestFittingForm) {
    set_last_error(0xDEAD);
    fill(OUT_ADDR, 256);
    EXPECT_EQ(call64("GetProcessMemoryInfo", kPseudo64, OUT_ADDR, 72, 0), 1u);
    EXPECT_EQ(get_last_error(), 0xDEADu);
    EXPECT_EQ(memory_.read_u32(OUT_ADDR).value(), 72u);
    EXPECT_GT(memory_.read_u32(OUT_ADDR + 4).value(), 0u);        // PageFaultCount
    const u64 peak = memory_.read_u64(OUT_ADDR + 8).value();
    const u64 ws = memory_.read_u64(OUT_ADDR + 16).value();
    EXPECT_GT(ws, 0u);
    EXPECT_GE(peak, ws);
    EXPECT_GT(memory_.read_u64(OUT_ADDR + 56).value(), 0u);       // PagefileUsage
    EXPECT_EQ(memory_.read_u8(OUT_ADDR + 72).value(), 0xCCu);

    fill(OUT_ADDR, 256);
    EXPECT_EQ(call64("GetProcessMemoryInfo", kPseudo64, OUT_ADDR, 71, 0), 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INSUFFICIENT_BUFFER);
    EXPECT_EQ(memory_.read_u32(OUT_ADDR).value(), 0xCCCCCCCCu);

    set_last_error(0xDEAD);
    EXPECT_EQ(call64("GetProcessMemoryInfo", kPseudo64, OUT_ADDR, 80, 0), 1u);
    EXPECT_EQ(memory_.read_u32(OUT_ADDR).value(), 80u);
    EXPECT_GT(memory_.read_u64(OUT_ADDR + 72).value(), 0u);       // PrivateUsage
    EXPECT_EQ(call64("GetProcessMemoryInfo", kPseudo64, OUT_ADDR, 81, 0), 1u);
    EXPECT_EQ(memory_.read_u32(OUT_ADDR).value(), 80u);
    fill(OUT_ADDR, 256);
    EXPECT_EQ(call64("GetProcessMemoryInfo", kPseudo64, OUT_ADDR, 96, 0), 1u);
    EXPECT_EQ(memory_.read_u32(OUT_ADDR).value(), 96u);
    EXPECT_GT(memory_.read_u64(OUT_ADDR + 80).value(), 0u);       // PrivateWorkingSetSize
    EXPECT_EQ(memory_.read_u8(OUT_ADDR + 96).value(), 0xCCu);
    EXPECT_EQ(call64("GetProcessMemoryInfo", kPseudo64, OUT_ADDR, 200, 0), 1u);
    EXPECT_EQ(memory_.read_u32(OUT_ADDR).value(), 96u);
    EXPECT_EQ(get_last_error(), 0xDEADu);

    EXPECT_EQ(call64("GetProcessMemoryInfo", kPseudo64, OUT_ADDR, 0, 0), 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INSUFFICIENT_BUFFER);
    EXPECT_EQ(call64("GetProcessMemoryInfo", kPseudo64, 0, 72, 0), 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_PARAMETER);
    EXPECT_EQ(call64("GetProcessMemoryInfo", 0x1234, OUT_ADDR, 72, 0), 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_HANDLE);
    EXPECT_EQ(call64("GetProcessMemoryInfo", 0, OUT_ADDR, 72, 0), 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_HANDLE);
}

// Probe (x86): 40/44/56-byte forms.
TEST_F(PsapiManagerTest, GetProcessMemoryInfoX86Forms) {
    set_last_error(0xDEAD);
    EXPECT_EQ(call32("GetProcessMemoryInfo", kPseudo32, OUT_ADDR, 40, 0), 1u);
    EXPECT_EQ(memory_.read_u32(OUT_ADDR).value(), 40u);
    EXPECT_GT(memory_.read_u32(OUT_ADDR + 12).value(), 0u);       // WorkingSetSize
    EXPECT_EQ(call32("GetProcessMemoryInfo", kPseudo32, OUT_ADDR, 44, 0), 1u);
    EXPECT_EQ(memory_.read_u32(OUT_ADDR).value(), 44u);
    EXPECT_EQ(call32("GetProcessMemoryInfo", kPseudo32, OUT_ADDR, 200, 0), 1u);
    EXPECT_EQ(memory_.read_u32(OUT_ADDR).value(), 56u);
    EXPECT_EQ(get_last_error(), 0xDEADu);
    EXPECT_EQ(call32("GetProcessMemoryInfo", kPseudo32, OUT_ADDR, 39, 0), 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INSUFFICIENT_BUFFER);
}

// Probe "== access rights": memory counters and the image name need query
// access; module queries also need PROCESS_VM_READ.
TEST_F(PsapiManagerTest, AccessRightsMatchWindows) {
    const Handle sync = self_handle(kSynchronize);
    EXPECT_EQ(call64("GetProcessMemoryInfo", sync, OUT_ADDR, 72, 0), 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_ACCESS_DENIED);

    const Handle limited = self_handle(kProcessQueryLimitedInformation);
    set_last_error(0xDEAD);
    EXPECT_EQ(call64("GetProcessMemoryInfo", limited, OUT_ADDR, 72, 0), 1u);
    EXPECT_EQ(call64("EnumProcessModules", limited, OUT_ADDR, 64, NEEDED_ADDR), 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_ACCESS_DENIED);
    EXPECT_EQ(call64("GetModuleBaseNameW", limited, 0, NAME_ADDR, 260), 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_ACCESS_DENIED);
    EXPECT_EQ(call64("GetModuleFileNameExW", limited, kImage, NAME_ADDR, 260), 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_ACCESS_DENIED);
    EXPECT_EQ(call64("GetModuleInformation", limited, kImage, OUT_ADDR, 24), 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_ACCESS_DENIED);

    const Handle query_read =
        self_handle(kProcessQueryInformation | kProcessVmRead);
    set_last_error(0xDEAD);
    EXPECT_EQ(call64("GetModuleBaseNameW", query_read, kImage, NAME_ADDR, 260), 10u);
    EXPECT_EQ(call64("GetModuleInformation", query_read, kImage, OUT_ADDR, 24), 1u);
    EXPECT_EQ(get_last_error(), 0xDEADu);
}

// Probe "== GetModuleBaseNameW/A": a name that does not fit copies nSize-1
// characters plus NUL and returns nSize; nSize 0 differs between W and A.
TEST_F(PsapiManagerTest, ModuleBaseNameTruncation) {
    Marshaler m(memory_);
    set_last_error(0xDEAD);
    fill(NAME_ADDR, 64);
    EXPECT_EQ(call64("GetModuleBaseNameW", kPseudo64, kImage, NAME_ADDR, 4), 4u);
    EXPECT_EQ(memory_.read_u16(NAME_ADDR + 0).value(), u16{'s'});
    EXPECT_EQ(memory_.read_u16(NAME_ADDR + 4).value(), u16{'m'});
    EXPECT_EQ(memory_.read_u16(NAME_ADDR + 6).value(), 0u);
    EXPECT_EQ(memory_.read_u16(NAME_ADDR + 8).value(), 0xCCCCu);
    // Exactly the length: the last character gives way to the NUL.
    EXPECT_EQ(call64("GetModuleBaseNameW", kPseudo64, kImage, NAME_ADDR, 10), 10u);
    EXPECT_EQ(memory_.read_u16(NAME_ADDR + 18).value(), 0u);
    EXPECT_EQ(call64("GetModuleBaseNameW", kPseudo64, kImage, NAME_ADDR, 11), 10u);
    EXPECT_EQ(m.read_wide_string(NAME_ADDR).value(), "sample.dll");
    EXPECT_EQ(call64("GetModuleBaseNameW", kPseudo64, kImage, NAME_ADDR, 1), 1u);
    EXPECT_EQ(memory_.read_u16(NAME_ADDR).value(), 0u);
    fill(NAME_ADDR, 64);
    EXPECT_EQ(call64("GetModuleBaseNameW", kPseudo64, kImage, NAME_ADDR, 0), 0u);
    EXPECT_EQ(memory_.read_u16(NAME_ADDR).value(), 0xCCCCu);
    EXPECT_EQ(get_last_error(), 0xDEADu);

    EXPECT_EQ(call64("GetModuleBaseNameA", kPseudo64, kImage, NAME_ADDR, 4), 4u);
    EXPECT_EQ(memory_.read_u32(NAME_ADDR).value(), 0x006D6173u); // "sam\0"
    EXPECT_EQ(call64("GetModuleBaseNameA", kPseudo64, kImage, NAME_ADDR, 0), 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_PARAMETER);

    EXPECT_EQ(call64("GetModuleBaseNameW", kPseudo64, 0x10000, NAME_ADDR, 260), 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_HANDLE);
    EXPECT_EQ(call64("GetModuleBaseNameW", 0x1234, 0, NAME_ADDR, 260), 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_HANDLE);
}

// Probe "== GetModuleFileNameExW/A": same truncation; nSize 0 is
// ERROR_INSUFFICIENT_BUFFER for both.
TEST_F(PsapiManagerTest, ModuleFileNameExTruncation) {
    Marshaler m(memory_);
    set_last_error(0xDEAD);
    EXPECT_EQ(call64("GetModuleFileNameExW", kPseudo64, kImage, NAME_ADDR, 260), 17u);
    EXPECT_EQ(m.read_wide_string(NAME_ADDR).value(), "C:\\dir\\sample.dll");
    EXPECT_EQ(call64("GetModuleFileNameExW", kPseudo64, kImage, NAME_ADDR, 4), 4u);
    EXPECT_EQ(memory_.read_u16(NAME_ADDR + 4).value(), u16{'\\'});
    EXPECT_EQ(memory_.read_u16(NAME_ADDR + 6).value(), 0u);
    EXPECT_EQ(get_last_error(), 0xDEADu);
    EXPECT_EQ(call64("GetModuleFileNameExW", kPseudo64, kImage, NAME_ADDR, 0), 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INSUFFICIENT_BUFFER);
    EXPECT_EQ(call64("GetModuleFileNameExA", kPseudo64, kImage, NAME_ADDR, 0), 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INSUFFICIENT_BUFFER);
    EXPECT_EQ(call64("GetModuleFileNameExA", kPseudo64, kImage, NAME_ADDR, 4), 4u);
    EXPECT_EQ(memory_.read_u32(NAME_ADDR).value(), 0x005C3A43u); // "C:\\\0"
    EXPECT_EQ(call64("GetModuleFileNameExA", kPseudo64, 0x10000, NAME_ADDR, 260), 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_HANDLE);
}

// Probe "== EnumProcessModules / GetModuleInformation".
TEST_F(PsapiManagerTest, EnumProcessModulesAndModuleInformation) {
    set_last_error(0xDEAD);
    EXPECT_EQ(call64("EnumProcessModules", kPseudo64, 0, 0, NEEDED_ADDR), 1u);
    const u32 needed = memory_.read_u32(NEEDED_ADDR).value();
    EXPECT_GT(needed, 0u);
    EXPECT_EQ(needed % 8, 0u);
    EXPECT_EQ(get_last_error(), 0xDEADu);
    EXPECT_EQ(call64("EnumProcessModules", kPseudo64, OUT_ADDR, 64, 0), 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_NOACCESS);
    EXPECT_EQ(call64("EnumProcessModules", 0x1234, OUT_ADDR, 64, NEEDED_ADDR), 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_HANDLE);

    set_last_error(0xDEAD);
    EXPECT_EQ(call64("GetModuleInformation", kPseudo64, kImage, OUT_ADDR, 24), 1u);
    EXPECT_EQ(memory_.read_u64(OUT_ADDR).value(), kImage);
    EXPECT_EQ(memory_.read_u32(OUT_ADDR + 8).value(), 0x123000u);
    EXPECT_EQ(memory_.read_u64(OUT_ADDR + 16).value(), kImage + 0x1234);
    EXPECT_EQ(get_last_error(), 0xDEADu);
    EXPECT_EQ(call64("GetModuleInformation", kPseudo64, kImage, OUT_ADDR, 23), 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INSUFFICIENT_BUFFER);
    EXPECT_EQ(call64("GetModuleInformation", kPseudo64, 0x10000, OUT_ADDR, 24), 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_HANDLE);
}

TEST_F(PsapiManagerTest, MappedFileNameUsesAddressAndBoundsUnicodeOutput) {
    cpu_.init_long_mode();
    cpu_.set_sp(STACK_BASE + 0x800);
    Marshaler m(memory_);
    m.attach_cpu(cpu_);
    mgr_.set_mapped_path_resolver([](GuestAddr address) {
        // The runtime's resolver reports NT device paths
        // (win32_to_nt_device_path).
        return address >= 0x140000000ull && address < 0x140010000ull
            ? std::string{"\\Device\\HarddiskVolume3\\folder\\image.dll"} : std::string{};
    });
    cpu_.gpr.rcx = ~u64{0};
    cpu_.gpr.rdx = 0x140000123;
    cpu_.gpr.r8 = NAME_ADDR;
    cpu_.gpr.r9 = 128;
    mgr_.get_mapped_file_name(m, true);
    EXPECT_EQ(m.read_wide_string(NAME_ADDR).value(), "\\Device\\HarddiskVolume3\\folder\\image.dll");
    EXPECT_EQ(cpu_.gpr.rax, 40u);
    cpu_.gpr.r9 = 4;
    ASSERT_TRUE(memory_.write_u16(NAME_ADDR + 8, 0xaaaa).is_ok());
    mgr_.get_mapped_file_name(m, true);
    EXPECT_EQ(cpu_.gpr.rax, 4u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INSUFFICIENT_BUFFER);
    EXPECT_EQ(memory_.read_u16(NAME_ADDR + 6).value(), 0u);
    EXPECT_EQ(memory_.read_u16(NAME_ADDR + 8).value(), 0xaaaau);
    cpu_.gpr.rdx = 0x141000000;
    mgr_.get_mapped_file_name(m, true);
    EXPECT_EQ(cpu_.gpr.rax, 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_FILE_NOT_FOUND);
}


// process_image_name_probe.expected (x86 = x64): the NT device path of the
// image (C: is \Device\HarddiskVolume3, as QueryDosDevice reports), its
// length without the terminator and the last error untouched; a buffer with
// no room for the terminator is 0 / ERROR_INSUFFICIENT_BUFFER, untouched; a
// NULL, unknown or non-process handle is 0 / ERROR_INVALID_HANDLE.
TEST_F(PsapiManagerTest, ProcessImageFileNameIsTheDevicePathLikeWindows) {
    const auto root = std::filesystem::temp_directory_path() / "vitreo-psapi-image";
    std::filesystem::create_directories(root);
    ASSERT_TRUE(vfs_.mount_drive('C', std::make_unique<capsule::HostFileSystem>(root.string())).is_ok());
    modules_.set_main_module(kImage, "C:\\Games\\StarCraft.exe");
    const std::string expected = "\\Device\\HarddiskVolume3\\Games\\StarCraft.exe";
    ASSERT_EQ(win32_to_nt_device_path(vfs_, "C:\\Games\\StarCraft.exe").value_or(""), expected);

    set_last_error(0xDEAD);
    EXPECT_EQ(call64("GetProcessImageFileNameW", kPseudo64, NAME_ADDR, 300, 0), expected.size());
    EXPECT_EQ(Marshaler(memory_).read_wide_string(NAME_ADDR).value(), expected);
    EXPECT_EQ(get_last_error(), 0xDEADu);

    fill(NAME_ADDR, 0x200, 0x41);
    EXPECT_EQ(call32("GetProcessImageFileNameA", kPseudo32, static_cast<u32>(NAME_ADDR), 300, 0),
              expected.size());
    EXPECT_EQ(Marshaler(memory_).read_ansi_string(NAME_ADDR).value(), expected);

    // Exactly the path's length leaves no room for the terminator.
    fill(NAME_ADDR, 0x200, 0x41);
    for (const u32 size : {0u, 1u, 10u, static_cast<u32>(expected.size())}) {
        SCOPED_TRACE(size);
        EXPECT_EQ(call64("GetProcessImageFileNameW", kPseudo64, NAME_ADDR, size, 0), 0u);
        EXPECT_EQ(get_last_error(), error_codes::ERROR_INSUFFICIENT_BUFFER);
        EXPECT_EQ(memory_.read_u8(NAME_ADDR).value(), 0x41u);
    }
    for (const u64 handle : {u64{0}, u64{0x123454}}) {
        EXPECT_EQ(call64("GetProcessImageFileNameW", handle, NAME_ADDR, 300, 0), 0u);
        EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_HANDLE);
    }
    (void)vfs_.unmount_drive('C');
    std::filesystem::remove_all(root);
}
