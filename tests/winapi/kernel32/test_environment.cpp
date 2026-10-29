/// @file test_environment.cpp
/// @brief Tests for EnvironmentManager (WIN32-035)

#include <gtest/gtest.h>
#include <winapi/kernel32/system/sysinfo.hpp>
#include <string>
#include <utility>
#include <vitreo/common.hpp>
#include <core/cpu/state/cpu_state.hpp>
#include <capsule/vfs/vfs.hpp>
#include <winapi/kernel32/environment/environment.hpp>
#include <winapi/kernel32/error.hpp>
#include <winapi/kernel32/kernel32_ext.hpp>
#include <winapi/core/marshaler.hpp>
#include <core/memory/guest_memory.hpp>
#include <cstring>

using namespace vitreo;
using namespace vitreo::winapi;

// =============================================================================
// Test Fixture
// =============================================================================

class EnvironmentManagerTest : public ::testing::Test {
protected:
    GuestMemory memory_;
    EnvironmentManager em_{"test.exe --arg1 value1", memory_};
};

// =============================================================================
// Default Population Tests
// =============================================================================

TEST_F(EnvironmentManagerTest, PopulateDefaultsAddsPATH) {
    auto result = em_.get_variable("PATH");
    ASSERT_TRUE(result.is_ok());
    EXPECT_FALSE(result.value().empty());
}

TEST_F(EnvironmentManagerTest, PopulateDefaultsAddsTEMP) {
    auto result = em_.get_variable("TEMP");
    ASSERT_TRUE(result.is_ok());
    EXPECT_FALSE(result.value().empty());
}

TEST_F(EnvironmentManagerTest, PopulateDefaultsAddsTMP) {
    auto result = em_.get_variable("TMP");
    ASSERT_TRUE(result.is_ok());
    EXPECT_FALSE(result.value().empty());
}

TEST_F(EnvironmentManagerTest, PopulateDefaultsAddsSYSTEMROOT) {
    auto result = em_.get_variable("SYSTEMROOT");
    ASSERT_TRUE(result.is_ok());
    EXPECT_EQ(result.value(), "C:\\Windows");
}

TEST_F(EnvironmentManagerTest, PopulateDefaultsAddsUSERPROFILE) {
    auto result = em_.get_variable("USERPROFILE");
    ASSERT_TRUE(result.is_ok());
    EXPECT_EQ(result.value(), "C:\\Users\\Default");
}

// The default block carries the whole Windows 11 logon environment
// (Windows `set` from 64-bit and SysWOW64 cmd.exe). ProgramData and
// ALLUSERSPROFILE were missing, along with APPDATA, windir, ComSpec, OS,
// PATHEXT, the ProgramFiles family and the PROCESSOR_* set.
TEST_F(EnvironmentManagerTest, DefaultsCarryTheWindowsLogonEnvironment) {
    const std::pair<const char*, const char*> expected[] = {
        {"ProgramData", "C:\\ProgramData"},
        {"ALLUSERSPROFILE", "C:\\ProgramData"},
        {"PUBLIC", "C:\\Users\\Public"},
        {"APPDATA", "C:\\Users\\Default\\AppData\\Roaming"},
        {"HOMEDRIVE", "C:"},
        {"HOMEPATH", "\\Users\\Default"},
        {"windir", "C:\\Windows"},
        {"ComSpec", "C:\\Windows\\system32\\cmd.exe"},
        {"OS", "Windows_NT"},
        {"PATHEXT", ".COM;.EXE;.BAT;.CMD;.VBS;.VBE;.JS;.JSE;.WSF;.WSH;.MSC;.CPL"},
        {"ProgramFiles(x86)", "C:\\Program Files (x86)"},
        {"ProgramW6432", "C:\\Program Files"},
        {"CommonProgramFiles(x86)", "C:\\Program Files (x86)\\Common Files"},
        {"CommonProgramW6432", "C:\\Program Files\\Common Files"},
        {"PROCESSOR_ARCHITECTURE", "AMD64"},
        {"ProgramFiles", "C:\\Program Files"},
        {"CommonProgramFiles", "C:\\Program Files\\Common Files"},
        // The virtual CPU's leaf-1 signature 0x306C3 (GetSystemInfo agrees).
        {"PROCESSOR_IDENTIFIER", "Intel64 Family 6 Model 60 Stepping 3, GenuineIntel"},
        {"PROCESSOR_LEVEL", "6"},
        {"PROCESSOR_REVISION", "3c03"},
    };
    for (const auto& [name, value] : expected) {
        auto r = em_.get_variable(name);
        ASSERT_TRUE(r.is_ok()) << name;
        EXPECT_EQ(r.value(), value) << name;
    }
    auto ncpu = em_.get_variable("NUMBER_OF_PROCESSORS");
    ASSERT_TRUE(ncpu.is_ok());
    EXPECT_EQ(ncpu.value(), std::to_string(host_active_processor_count()));
    EXPECT_FALSE(em_.get_variable("PROCESSOR_ARCHITEW6432").is_ok());
}

TEST_F(EnvironmentManagerTest, Wow64ViewOverridesInheritedArchitectureAndProgramFiles) {
    em_.apply_process_bitness(false);
    EXPECT_EQ(em_.get_variable("PROCESSOR_ARCHITECTURE").value(), "x86");
    EXPECT_EQ(em_.get_variable("PROCESSOR_ARCHITEW6432").value(), "AMD64");
    EXPECT_EQ(em_.get_variable("ProgramFiles").value(), "C:\\Program Files (x86)");
    EXPECT_EQ(em_.get_variable("CommonProgramFiles").value(),
              "C:\\Program Files (x86)\\Common Files");
    EXPECT_EQ(em_.get_variable("ProgramW6432").value(), "C:\\Program Files");
    // A 64-bit child that inherited the 32-bit block gets the native view.
    em_.apply_process_bitness(true);
    EXPECT_EQ(em_.get_variable("PROCESSOR_ARCHITECTURE").value(), "AMD64");
    EXPECT_FALSE(em_.get_variable("PROCESSOR_ARCHITEW6432").is_ok());
    EXPECT_EQ(em_.get_variable("ProgramFiles").value(), "C:\\Program Files");
}

TEST_F(EnvironmentManagerTest, PopulateDefaultsAddsCOMPUTERNAME) {
    auto result = em_.get_variable("COMPUTERNAME");
    ASSERT_TRUE(result.is_ok());
    EXPECT_EQ(result.value(), "VITREO");
}

TEST_F(EnvironmentManagerTest, PopulateDefaultsAddsAtLeast5Variables) {
    auto vars = em_.get_all_variables();
    EXPECT_GE(vars.size(), 5u);
}

// =============================================================================
// Set / Get Round-Trip Tests
// =============================================================================

TEST_F(EnvironmentManagerTest, SetAndGetVariableRoundTrip) {
    em_.set_variable("MY_VAR", "hello_world");
    auto result = em_.get_variable("MY_VAR");
    ASSERT_TRUE(result.is_ok());
    EXPECT_EQ(result.value(), "hello_world");
}

TEST_F(EnvironmentManagerTest, CaseInsensitiveLookup) {
    em_.set_variable("Path", "/usr/bin");
    // Lookup with different case should find the same variable
    auto result = em_.get_variable("PATH");
    ASSERT_TRUE(result.is_ok());
    EXPECT_EQ(result.value(), "/usr/bin");
}

TEST_F(EnvironmentManagerTest, CaseInsensitiveLookupLowercase) {
    em_.set_variable("MYKEY", "value123");
    auto result = em_.get_variable("mykey");
    ASSERT_TRUE(result.is_ok());
    EXPECT_EQ(result.value(), "value123");
}

TEST_F(EnvironmentManagerTest, SetVariableOverwritesExisting) {
    em_.set_variable("FOO", "original");
    em_.set_variable("FOO", "updated");
    auto result = em_.get_variable("FOO");
    ASSERT_TRUE(result.is_ok());
    EXPECT_EQ(result.value(), "updated");
}

TEST_F(EnvironmentManagerTest, SetVariableWithEmptyValueStoresEmpty) {
    em_.set_variable("EMPTY_VAR", "");
    auto result = em_.get_variable("EMPTY_VAR");
    ASSERT_TRUE(result.is_ok());
    EXPECT_EQ(result.value(), "");
}

// =============================================================================
// Delete Variable Tests
// =============================================================================

TEST_F(EnvironmentManagerTest, DeleteVariableRemovesIt) {
    em_.set_variable("TO_DELETE", "temporary");
    em_.delete_variable("TO_DELETE");
    auto result = em_.get_variable("TO_DELETE");
    EXPECT_TRUE(result.is_err());
}

TEST_F(EnvironmentManagerTest, DeleteNonExistentVariableIsNoOp) {
    // Should not crash or error
    em_.delete_variable("DOES_NOT_EXIST");
    auto result = em_.get_variable("DOES_NOT_EXIST");
    EXPECT_TRUE(result.is_err());
}

// =============================================================================
// Get Variable Not Found
// =============================================================================

TEST_F(EnvironmentManagerTest, GetNonExistentVariableReturnsNotFound) {
    auto result = em_.get_variable("NO_SUCH_VAR");
    ASSERT_TRUE(result.is_err());
    EXPECT_EQ(result.error(), Error::NotFound);
}

// =============================================================================
// Command Line Tests
// =============================================================================

TEST_F(EnvironmentManagerTest, GetCommandLineAReturnsConstructorArg) {
    EXPECT_EQ(em_.get_command_line_a(), "test.exe --arg1 value1");
}

TEST_F(EnvironmentManagerTest, GetCommandLineWUtf8ReturnsConstructorArg) {
    EXPECT_EQ(em_.get_command_line_w_utf8(), "test.exe --arg1 value1");
}

TEST_F(EnvironmentManagerTest, GetCommandLineWPublishesFullCrashpadHandlerType) {
    const char kCmd[] =
        "C:\\Users\\Default\\AppData\\Roaming\\Spotify\\Spotify.exe "
        "--type=crashpad-handler /prefetch:4 --max-uploads=5";
    GuestMemory memory;
    EnvironmentManager em{kCmd, memory};
    const GuestAddr buf = em.cmdline_w_addr();
    ASSERT_NE(buf, 0u);

    std::string guest;
    for (u32 i = 0; i < 260; ++i) {
        const auto ch = memory.read_u16(buf + i * 2);
        ASSERT_TRUE(ch.is_ok());
        if (ch.value() == 0) {
            break;
        }
        ASSERT_LT(ch.value(), 0x80u);
        guest.push_back(static_cast<char>(ch.value()));
    }
    EXPECT_EQ(guest, kCmd);
    const auto type_at = guest.find("--type=");
    ASSERT_NE(type_at, std::string::npos);
    EXPECT_EQ(guest.substr(type_at, 23), "--type=crashpad-handler");
    EXPECT_EQ(em.cmdline_w_bytes(), static_cast<u16>(guest.size() * 2));
}

// =============================================================================
// Environment Block Tests
// =============================================================================

TEST_F(EnvironmentManagerTest, BuildEnvironmentBlockContainsKeyValuePairs) {
    em_.set_variable("BLOCK_TEST", "test_value");
    std::string block = em_.build_environment_block();
    // Should contain "BLOCK_TEST=test_value" followed by a null
    std::string expected = "BLOCK_TEST=test_value";
    EXPECT_NE(block.find(expected), std::string::npos);
}

TEST_F(EnvironmentManagerTest, BuildEnvironmentBlockEndsWithDoubleNull) {
    std::string block = em_.build_environment_block();
    ASSERT_GE(block.size(), 2u);
    // Last two bytes should be null
    EXPECT_EQ(block[block.size() - 1], '\0');
    EXPECT_EQ(block[block.size() - 2], '\0');
}

TEST_F(EnvironmentManagerTest, BuildEnvironmentBlockPreservesWindowsDisplayCasing) {
    // Chromium sandbox FilterEnvironment matches these names exactly.
    const std::string block = em_.build_environment_block();
    for (const char* name : {"Path=", "SystemRoot=", "SystemDrive=",
                             "TEMP=", "TMP=", "LOCALAPPDATA="}) {
        EXPECT_NE(block.find(name), std::string::npos) << name;
    }
}

TEST_F(EnvironmentManagerTest, GetEnvironmentStringsWMaterializesWideBlock) {
    cpu::CpuState cpu;
    cpu.init_protected_mode();
    constexpr GuestAddr stack = 0x50000;
    ASSERT_TRUE(memory_.map_region(
        stack, 0x1000, MemoryProtection::ReadWrite).is_ok());
    cpu.set_sp(stack + 0x800);

    Marshaler m(memory_);
    m.attach_cpu(cpu);
    impl_GetEnvironmentStringsW(m, em_);
    const GuestAddr block_addr = static_cast<GuestAddr>(cpu.gpr.rax);
    ASSERT_NE(block_addr, 0u);

    // Scan for UTF-16 "Path="
    bool found_path = false;
    for (u32 i = 0; i + 10 < 0x4000; i += 2) {
        const auto u0 = memory_.read_u16(block_addr + i);
        const auto u1 = memory_.read_u16(block_addr + i + 2);
        const auto u2 = memory_.read_u16(block_addr + i + 4);
        const auto u3 = memory_.read_u16(block_addr + i + 6);
        const auto u4 = memory_.read_u16(block_addr + i + 8);
        if (u0.is_err() || u1.is_err() || u2.is_err() || u3.is_err() ||
            u4.is_err()) {
            break;
        }
        if (u0.value() == 'P' && u1.value() == 'a' && u2.value() == 't' &&
            u3.value() == 'h' && u4.value() == '=') {
            found_path = true;
            break;
        }
        if (u0.value() == 0 && u1.value() == 0) break;
    }
    EXPECT_TRUE(found_path);

    // The whole block: every variable as "name=value\0", in the same order
    // as the ANSI block, and a single extra null at the end.
    std::vector<std::string> wide_entries;
    std::string current;
    GuestAddr at = block_addr;
    for (;;) {
        const u16 unit = memory_.read_u16(at).value_or(0xFFFF);
        at += 2;
        if (unit == 0) {
            if (current.empty()) break;
            wide_entries.push_back(current);
            current.clear();
            continue;
        }
        ASSERT_LT(unit, 0x80u);
        current.push_back(static_cast<char>(unit));
    }
    std::vector<std::string> expected;
    const std::string ansi = em_.build_environment_block();
    for (usize i = 0; i < ansi.size() && ansi[i] != '\0';) {
        const usize end = ansi.find('\0', i);
        expected.push_back(ansi.substr(i, end - i));
        i = end + 1;
    }
    EXPECT_GT(expected.size(), 1u);
    EXPECT_EQ(wide_entries, expected);
}

TEST_F(EnvironmentManagerTest, BuildEnvironmentBlockEmptyAfterClearingAllVars) {
    // Remove all default variables
    auto vars = em_.get_all_variables();
    for (const auto& [key, value] : vars) {
        em_.delete_variable(key);
    }
    std::string block = em_.build_environment_block();
    // Should be a single null byte (double-null: the loop produces nothing,
    // then the trailing '\0' is appended)
    EXPECT_EQ(block.size(), 1u);
    EXPECT_EQ(block[0], '\0');
}

TEST_F(EnvironmentManagerTest, BuildEnvironmentBlockNullSeparatedEntries) {
    // Clear defaults and set exactly two variables
    auto vars = em_.get_all_variables();
    for (const auto& [key, value] : vars) {
        em_.delete_variable(key);
    }
    em_.set_variable("AAA", "111");
    em_.set_variable("BBB", "222");

    std::string block = em_.build_environment_block();
    // Expected: "AAA=111\0BBB=222\0\0"
    std::string expected;
    expected += "AAA=111";
    expected += '\0';
    expected += "BBB=222";
    expected += '\0';
    expected += '\0';
    EXPECT_EQ(block, expected);
}

// =============================================================================
// GetAllVariables Tests
// =============================================================================

TEST_F(EnvironmentManagerTest, GetAllVariablesReturnsMap) {
    auto vars = em_.get_all_variables();
    // Should include at least the defaults
    EXPECT_GE(vars.size(), 5u);
    EXPECT_NE(vars.find("PATH"), vars.end());
    EXPECT_NE(vars.find("SYSTEMROOT"), vars.end());
}

// =============================================================================
// Export Table Tests
// =============================================================================

TEST_F(EnvironmentManagerTest, MakeEnvironmentExportsReturns8Entries) {
    auto exports = make_environment_exports(em_);
    EXPECT_EQ(exports.entries.size(), 10u);
}

TEST_F(EnvironmentManagerTest, MakeEnvironmentExportsContainsExpectedNames) {
    auto exports = make_environment_exports(em_);

    // Collect all names
    std::vector<std::string> names;
    names.reserve(exports.entries.size());
    for (const auto& entry : exports.entries) {
        names.push_back(entry.name);
    }

    EXPECT_NE(std::find(names.begin(), names.end(), "GetCommandLineA"), names.end());
    EXPECT_NE(std::find(names.begin(), names.end(), "GetCommandLineW"), names.end());
    EXPECT_NE(std::find(names.begin(), names.end(), "GetEnvironmentVariableA"), names.end());
    EXPECT_NE(std::find(names.begin(), names.end(), "GetEnvironmentVariableW"), names.end());
    EXPECT_NE(std::find(names.begin(), names.end(), "SetEnvironmentVariableA"), names.end());
    EXPECT_NE(std::find(names.begin(), names.end(), "SetEnvironmentVariableW"), names.end());
    EXPECT_NE(std::find(names.begin(), names.end(), "GetEnvironmentStringsW"), names.end());
    EXPECT_NE(std::find(names.begin(), names.end(), "FreeEnvironmentStringsW"), names.end());
}

TEST_F(EnvironmentManagerTest, MakeEnvironmentExportsAllHandlersNonNull) {
    auto exports = make_environment_exports(em_);
    for (const auto& entry : exports.entries) {
        EXPECT_TRUE(static_cast<bool>(entry.handler))
            << "Handler for " << entry.name << " is null";
    }
}

TEST_F(EnvironmentManagerTest, ExpandEnvironmentStringsAExpandsKnownVariable) {
    em_.set_variable("VITREO_TEST_VAR", "expanded");

    capsule::Vfs vfs;
    Kernel32ExtManager mgr(memory_, vfs);
    mgr.set_environment_manager(&em_);

    constexpr GuestAddr stack = 0x50000;
    constexpr GuestAddr src = 0x51000;
    constexpr GuestAddr dst = 0x52000;
    ASSERT_TRUE(memory_.map_region(stack, 0x1000, MemoryProtection::ReadWrite).is_ok());
    ASSERT_TRUE(memory_.map_region(src, 0x1000, MemoryProtection::ReadWrite).is_ok());
    ASSERT_TRUE(memory_.map_region(dst, 0x1000, MemoryProtection::ReadWrite).is_ok());

    const char* input = "%VITREO_TEST_VAR%\\bin";
    for (usize i = 0; input[i] != '\0'; ++i) {
        ASSERT_TRUE(memory_.write_u8(src + i, static_cast<u8>(input[i])).is_ok());
    }
    ASSERT_TRUE(memory_.write_u8(src + std::strlen(input), 0).is_ok());

    cpu::CpuState cpu;
    cpu.init_protected_mode();
    cpu.set_sp(stack + 0x800);
    cpu.mode = cpu::CpuMode::Long;

    auto exports = create_kernel32_ext_thunks(mgr);
    ExportEntry* entry = nullptr;
    for (auto& e : exports.entries) {
        if (e.name == "ExpandEnvironmentStringsA") {
            entry = &e;
            break;
        }
    }
    ASSERT_NE(entry, nullptr);
    EXPECT_EQ(entry->stdcall_bytes, 12u);

    Marshaler m(memory_);
    m.attach_cpu(cpu);
    cpu.gpr.rcx = src;
    cpu.gpr.rdx = dst;
    cpu.gpr.r8 = 64;
    entry->handler(m);

    EXPECT_EQ(cpu.gpr.rax, 13u); // "expanded\\bin" + NUL
    std::string out;
    for (u32 i = 0; i < 64; ++i) {
        auto ch = memory_.read_u8(dst + i);
        ASSERT_TRUE(ch.is_ok());
        if (ch.value() == 0) break;
        out.push_back(static_cast<char>(ch.value()));
    }
    EXPECT_EQ(out, "expanded\\bin");
}

// Learn "Environment Variables": names are case-insensitive for lookup, and
// the environment block keeps each name as it was written. A child that
// inherits the block (CreateProcess lpEnvironment == NULL) and the CRT's
// _environ/_wenviron see those spellings. Vitreo handed children the upper-
// cased lookup keys, so Steam's "SteamAppId" reached a grandchild as
// "STEAMAPPID".
TEST_F(EnvironmentManagerTest, NamedVariablesPreserveTheirSpellingInBlockOrder) {
    for (const auto& [key, value] : em_.get_all_variables()) {
        em_.delete_variable(key);
    }
    em_.set_variable("SteamAppId", "526870");
    em_.set_variable("aaa_lower", "1");
    em_.set_variable("Path", "C:\\Windows");
    // Updating through a differently-cased name keeps the first spelling.
    em_.set_variable("STEAMAPPID", "7");

    const auto named = em_.get_all_variables_as_named();
    const std::vector<std::pair<std::string, std::string>> expected{
        {"aaa_lower", "1"},
        {"Path", "C:\\Windows"},
        {"SteamAppId", "7"},
    };
    EXPECT_EQ(named, expected);
    EXPECT_EQ(em_.get_variable("steamappid").value(), "7");
}
