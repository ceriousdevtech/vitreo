// Regression for the public WinExec success/failure boundary.
#include <gtest/gtest.h>
#include "winapi/kernel32/kernel32_ext.hpp"
#include "winapi/core/thunk_registry.hpp"
#include "winapi/core/marshaler.hpp"
#include "core/cpu/state/cpu_state.hpp"
#include "capsule/vfs/vfs.hpp"
#include <algorithm>
#include <cstring>
#include "winapi/kernel32/error.hpp"
#include "winapi/kernel32/process/winexec_contract.inc"
#include "winapi/advapi32/security/file_execute_access.hpp"
#include "winapi/advapi32/security/security.hpp"
#include "winapi/advapi32/security/named_security.hpp"
#include "winapi/advapi32/security/sddl.hpp"
#include "winapi/advapi32/security/process_execute_contract.inc"
#include "capsule/vfs/host_filesystem.hpp"
#include <filesystem>
#include <fstream>
#include <sstream>
#include <unistd.h>
using namespace vitreo;
using namespace vitreo::winapi;

// Windows winexec_execute_rights{32,64}.expected establishes EXECUTE plus
// SYNCHRONIZE as required, even after OWNER RIGHTS removes implicit rights.
TEST(WinExecContract, FileExecutionHonorsExecuteSynchronizeDenyAndNullDacls) {
    const auto root = std::filesystem::temp_directory_path() /
        ("vitreo-winexec-acl-" + std::to_string(::getpid()));
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() { std::error_code error; std::filesystem::remove_all(path, error); }
    } cleanup{root};
    std::error_code error;
    ASSERT_TRUE(std::filesystem::create_directories(root, error));
    std::ofstream(root / "child.exe") << "authorization fixture";
    capsule::Vfs vfs;
    ASSERT_TRUE(vfs.mount_drive('C', std::make_unique<capsule::HostFileSystem>(root.string())).is_ok());
    HandleTable handles;
    SecurityStubs security(handles);
    const auto who = security.principal();
    const auto install = [&](const std::string& text) {
        std::vector<u8> bytes;
        if (sddl::string_to_sd(text, bytes) != error_codes::ERROR_SUCCESS) return false;
        named_sd::Parts parts;
        if (!named_sd::parse_sd(bytes, parts)) return false;
        return named_sd::set_file_sd(vfs, "C:\\child.exe",
            named_sd::kDacl | named_sd::kProtectedDacl, parts, who) == error_codes::ERROR_SUCCESS;
    };
    const auto mask = [](unsigned long value) {
        std::ostringstream text; text << "0x" << std::hex << value; return text.str();
    };
    const auto execute = process_execute_sdk::FILE_EXECUTE;
    const auto synchronize = process_execute_sdk::SYNCHRONIZE;
    ASSERT_TRUE(install("D:P(A;;" + mask(execute) + ";;;WD)(A;;" + mask(execute) + ";;;OW)"));
    EXPECT_EQ(check_file_execute_access(security, vfs, "C:\\child.exe"), error_codes::ERROR_ACCESS_DENIED);
    ASSERT_TRUE(install("D:P(A;;" + mask(execute | synchronize) + ";;;WD)(A;;" +
                        mask(execute | synchronize) + ";;;OW)"));
    EXPECT_EQ(check_file_execute_access(security, vfs, "C:\\child.exe"), error_codes::ERROR_SUCCESS);
    ASSERT_TRUE(install("D:P(D;;GX;;;WD)(A;;GA;;;WD)"));
    EXPECT_EQ(check_file_execute_access(security, vfs, "C:\\child.exe"), error_codes::ERROR_ACCESS_DENIED);
    ASSERT_TRUE(install("D:P"));
    EXPECT_EQ(check_file_execute_access(security, vfs, "C:\\child.exe"), error_codes::ERROR_ACCESS_DENIED);
    named_sd::Parts null_dacl; null_dacl.dacl_present = true; null_dacl.dacl_null = true;
    ASSERT_EQ(named_sd::set_file_sd(vfs, "C:\\child.exe",
        named_sd::kDacl | named_sd::kProtectedDacl, null_dacl, who), error_codes::ERROR_SUCCESS);
    EXPECT_EQ(check_file_execute_access(security, vfs, "C:\\child.exe"), error_codes::ERROR_SUCCESS);
}

// Learn WinExec: success (>31) means a process was started. With an empty
// filesystem and no ProcessSession launch resources, a missing program must
// fail, independent of guest pointer width or show mode. The bound is from
// the public contract, and remains valid once real launching is implemented.
TEST(WinExecContract, MissingProgramNeverReportsSuccessInEitherAbi) {
    for (const bool is64 : {false, true}) {
        GuestMemory memory;
        ASSERT_TRUE(memory.map_region(0x1000, 0x4000,
                                      MemoryProtection::ReadWrite).is_ok());
        capsule::Vfs vfs;
        Kernel32ExtManager manager(memory, vfs);
        auto exports = create_kernel32_ext_thunks(manager);
        const auto entry = std::find_if(exports.entries.begin(), exports.entries.end(),
            [](const ExportEntry& candidate) { return candidate.name == "WinExec"; });
        ASSERT_NE(entry, exports.entries.end());
        EXPECT_EQ(entry->stdcall_bytes, 8u);
        const char command[] = "installer-child.exe /argument";
        ASSERT_TRUE(memory.write_bytes(0x1000,
            reinterpret_cast<const Byte*>(command), sizeof(command)).is_ok());
        for (const u32 show : {0u, 1u, 0xffffffffu}) {
            cpu::CpuState state;
            if (is64) {
                state.init_long_mode(); state.gpr.rsp = 0x4000;
                state.gpr.rcx = 0x1000; state.gpr.rdx = show;
            } else {
                state.init_protected_mode(); state.set_sp(0x4000);
                ASSERT_TRUE(memory.write_u32(0x4004, 0x1000).is_ok());
                ASSERT_TRUE(memory.write_u32(0x4008, show).is_ok());
            }
            Marshaler marshaler(memory); marshaler.attach_cpu(state);
            entry->handler(marshaler);
            EXPECT_LE(state.gpr.rax, 31u);
        }
    }
}

// Oracle failure observations: winexec_probe_matrix{32,64}.expected.
TEST(WinExecContract, InvalidCommandsPreserveTheMeasuredReturnAndErrorInBothAbis) {
    for (const bool is64 : {false, true}) {
        GuestMemory memory;
        ASSERT_TRUE(memory.map_region(0x1000, 0x4000,
                                      MemoryProtection::ReadWrite).is_ok());
        capsule::Vfs vfs;
        Kernel32ExtManager manager(memory, vfs);
        auto exports = create_kernel32_ext_thunks(manager);
        const auto entry = std::find_if(exports.entries.begin(), exports.entries.end(),
            [](const ExportEntry& e) { return e.name == "WinExec"; });
        ASSERT_NE(entry, exports.entries.end());
        for (const char* command : {static_cast<const char*>(nullptr), "", "   ",
                                   " child.exe", "\tchild.exe", "\"\""}) {
            if (command) {
                ASSERT_TRUE(memory.write_bytes(0x1000,
                    reinterpret_cast<const Byte*>(command), std::strlen(command) + 1).is_ok());
            }
            cpu::CpuState state;
            const GuestAddr address = command ? 0x1000 : 0;
            if (is64) {
                state.init_long_mode(); state.gpr.rsp = 0x4000;
                state.gpr.rcx = address; state.gpr.rdx = 1;
            } else {
                state.init_protected_mode(); state.set_sp(0x4000);
                ASSERT_TRUE(memory.write_u32(0x4004, static_cast<u32>(address)).is_ok());
                ASSERT_TRUE(memory.write_u32(0x4008, 1).is_ok());
            }
            Marshaler marshaler(memory); marshaler.attach_cpu(state);
            entry->handler(marshaler);
            EXPECT_EQ(state.gpr.rax, 0u);
            EXPECT_EQ(get_last_error(), winexec_contract::ERROR_INVALID_PARAMETER);
        }
    }
}

