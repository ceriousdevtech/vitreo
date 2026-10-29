// SPDX-License-Identifier: LGPL-2.1-only
/// @file test_msi_customaction.cpp
/// @brief Contract tests for the Windows Installer customaction area.
///
/// Expected values come from MsiDefs.h (msidbCustomActionType), Learn
/// "Custom Action Types" / "... Return Processing Options" / "... In-Script
/// Execution Options", and the Windows 11 measurements of
/// tools/oracle_probe/msi_customaction_* (msi_customaction_x64/x86.expected).
/// The custom-action process is a recording double: the guest side (load,
/// thread, CreateProcess) is exercised end to end by the probe package under
/// `vitreo run` (tests/winapi/msi/fixtures/vca64.msi, vca86.msi).

#include "msi_fixture.hpp"

#include <winapi/system/msi/msi_customaction.hpp>
#include <winapi/system/msi/msi_engine.hpp>

namespace vitreo::winapi::msi_test {
namespace {

using namespace vitreo::winapi::msi;

/// Records what the custom-action code asks of its process.
class FakeProcess final : public CustomActionProcess {
public:
    bool x64{true};
    u32 exit_code{0};
    bool create_fails{false};
    std::u16string last_application, last_command, last_directory;
    int started{0}, waited{0}, closed{0}, deleted{0};

    [[nodiscard]] bool is_64bit() const override { return x64; }
    u64 load_library_from_bytes(const std::vector<u8>&, std::u16string&) override { return 0; }
    u64 load_library(const std::u16string&) override { return 0; }
    u64 get_proc_address(u64, const std::string&) override { return 0; }
    void free_library(u64) override {}
    u32 call_dll_entry(u64, u32) override { return 0; }
    bool run_process(const std::u16string&, const std::u16string&, const std::u16string&, bool, u32&) override {
        return false;
    }
    bool write_temp_file(const std::vector<u8>&, const std::u16string&, std::u16string& path) override {
        path = u"C:\\Windows\\Installer\\MSI1A2B.tmp";
        return true;
    }
    void delete_file(const std::u16string&) override { ++deleted; }
    std::optional<u16> image_machine(const std::u16string&) override { return std::nullopt; }
    u64 start_dll_entry(u64, u32) override { return 0; }
    u64 start_process(const std::u16string& application, const std::u16string& command_line,
                      const std::u16string& directory) override {
        last_application = application;
        last_command = command_line;
        last_directory = directory;
        if (create_fails) return 0;
        ++started;
        return 0x1234;
    }
    u32 wait_and_close(u64) override {
        ++waited;
        return exit_code;
    }
    void close_handle(u64) override { ++closed; }
    GuestMemory& guest_memory() override { return scratch_memory; }
    GuestAddr alloc_scratch(u32) override { return 0; }
    void free_scratch(GuestAddr) override {}
    bool start_custom_action_server(bool, const std::u16string&) override { return false; }
    void pump_messages() override {}
    GuestMemory scratch_memory;
};

class Msi_customaction_Test : public MsiTest {
protected:
    void SetUp() override {
        MsiTest::SetUp();
        services.manager = &mgr;
        services.custom_actions = &process;
        set_process_custom_action_host(&process);
        session.set_property("WindowsFolder", "C:\\WINDOWS\\");
        session.set_property("SystemFolder", "C:\\WINDOWS\\SysWOW64\\");
    }
    void TearDown() override { set_process_custom_action_host(nullptr); }

    u32 run(const std::string& action, u32 type, const std::string& source, const std::string& target) {
        ActionContext ctx(session, services);
        return run_custom_action(ctx, action, type, source, target);
    }

    FakeProcess process;
    Session session;
    Services services;
};

// MsiDefs.h enum msidbCustomActionType.
TEST_P(Msi_customaction_Test, TypeBitsMatchMsiDefs) {
    EXPECT_EQ(kCaDll, 0x001u);
    EXPECT_EQ(kCaExe, 0x002u);
    EXPECT_EQ(kCaTextData, 0x003u);
    EXPECT_EQ(kCaJScript, 0x005u);
    EXPECT_EQ(kCaVBScript, 0x006u);
    EXPECT_EQ(kCaInstall, 0x007u);
    EXPECT_EQ(kCaBinaryData, 0x000u);
    EXPECT_EQ(kCaSourceFile, 0x010u);
    EXPECT_EQ(kCaDirectory, 0x020u);
    EXPECT_EQ(kCaProperty, 0x030u);
    EXPECT_EQ(kCaContinue, 0x040u);
    EXPECT_EQ(kCaAsync, 0x080u);
    EXPECT_EQ(kCaFirstSequence, 0x100u);
    EXPECT_EQ(kCaOncePerProcess, 0x200u);
    EXPECT_EQ(kCaClientRepeat, 0x300u);
    EXPECT_EQ(kCaInScript, 0x400u);
    EXPECT_EQ(kCaRollback, 0x100u);
    EXPECT_EQ(kCaCommit, 0x200u);
    EXPECT_EQ(kCaNoImpersonate, 0x800u);
    EXPECT_EQ(kCa64BitScript, 0x1000u);
    EXPECT_EQ(kCaHideTarget, 0x2000u);
    EXPECT_EQ(kCaTSAware, 0x4000u);
    EXPECT_EQ(kCaPatchUninstall, 0x8000u);
}

// Measured (cases ret0..ret1234): 0 success, 259 skip the rest, 1602 user
// exit, 1603 failure, 1604 suspend, 1626 not called; 1 and 1234 fail.
TEST_P(Msi_customaction_Test, DllReturnValues) {
    EXPECT_EQ(dll_return_to_action_code(0), 0u);
    EXPECT_EQ(dll_return_to_action_code(259), 259u);
    EXPECT_EQ(dll_return_to_action_code(1602), 1602u);
    EXPECT_EQ(dll_return_to_action_code(1603), 1603u);
    EXPECT_EQ(dll_return_to_action_code(1604), 1604u);
    EXPECT_EQ(dll_return_to_action_code(1626), 1626u);
    EXPECT_EQ(dll_return_to_action_code(1), 1603u);
    EXPECT_EQ(dll_return_to_action_code(1234), 1603u);
}

// Type 51: MsiSetProperty(Source, formatted Target).
TEST_P(Msi_customaction_Test, Type51SetsFormattedProperty) {
    session.set_property("ProductName", "Vitreo CA Probe");
    EXPECT_EQ(run("SetCaDefer", 51, "CaDefer", "defer-data [ProductName]"), 0u);
    EXPECT_EQ(session.property("CaDefer"), "defer-data Vitreo CA Probe");
    EXPECT_EQ(run("SetCnt", 51, "CNT", "[CNT]x"), 0u);
    EXPECT_EQ(run("SetCnt", 51, "CNT", "[CNT]x"), 0u);
    EXPECT_EQ(session.property("CNT"), "xx");
}

// Measured (t1075): the in-script bit on a type 51 action is error 2723 and
// fails; Continue does not apply to a property action.
TEST_P(Msi_customaction_Test, InScriptPropertyActionIsUnsupported) {
    EXPECT_EQ(run("Set1075", 1075, "PROP1075", "deferred-51"), 1603u);
    EXPECT_EQ(session.property("PROP1075"), "");
}

// Type 19 always fails (err1, err2).
TEST_P(Msi_customaction_Test, Type19Fails) {
    EXPECT_EQ(run("CaErr19Text", 19, "", "literal error"), 1603u);
    EXPECT_EQ(run("CaErr19Index", 19, "", "25000"), 1603u);
}

// Measured (bad4, bad8): basic types 0 and 4 are error 2717 and fail.
TEST_P(Msi_customaction_Test, InvalidBasicTypesFail) {
    EXPECT_EQ(run("CaBadType4", 4, "cax64", "Info"), 1603u);
    EXPECT_EQ(run("CaBadType8", 8, "cax64", "Info"), 1603u);
    EXPECT_EQ(run("CaBadType0", 0, "cax64", "Info"), 1603u);
    EXPECT_EQ(process.started, 0);
}

// Measured (rasync1): Async on a rollback action is error 2723.
TEST_P(Msi_customaction_Test, AsyncRollbackIsUnsupported) {
    EXPECT_EQ(run("CaRollbackAsync", 1409, "cax64", "SleepRet"), 1603u);
}

// Type 50: the property holds the program; the command line is the quoted
// program then Target; the working directory is %WINDIR%\system32 (the
// installer service's, measured for every EXE action without a directory).
TEST_P(Msi_customaction_Test, Type50CommandLineAndDirectory) {
    session.set_property("EXEPATH", "C:\\WINDOWS\\SysWOW64\\cmd.exe");
    process.exit_code = 0;
    EXPECT_EQ(run("CaExe50", 50, "EXEPATH", "/c exit 0"), 0u);
    EXPECT_EQ(process.last_application, u"C:\\WINDOWS\\SysWOW64\\cmd.exe");
    EXPECT_EQ(process.last_command, u"\"C:\\WINDOWS\\SysWOW64\\cmd.exe\" /c exit 0");
    EXPECT_EQ(process.last_directory, u"C:\\WINDOWS\\system32");
    EXPECT_EQ(process.waited, 1);
}

// Measured (exe1, CaExe50Cont): a non-zero exit code is error 1722 and a
// failure; with Continue it is success.
TEST_P(Msi_customaction_Test, ExeExitCodeProcessing) {
    session.set_property("EXEPATH", "C:\\WINDOWS\\SysWOW64\\cmd.exe");
    process.exit_code = 5;
    EXPECT_EQ(run("CaExe50", 50, "EXEPATH", "/c exit 5"), 1603u);
    process.exit_code = 3;
    EXPECT_EQ(run("CaExe50Cont", 50 + 64, "EXEPATH", "/c exit 3"), 0u);
}

// Type 34: Target (formatted) is the whole command line; Source the
// working directory.
TEST_P(Msi_customaction_Test, Type34UsesDirectoryAndFormattedTarget) {
    session.set_property("TARGETDIR", "C:\\");
    process.exit_code = 0;
    EXPECT_EQ(run("CaExe34", 34, "TARGETDIR", "\"[SystemFolder]cmd.exe\" /c exit 0"), 0u);
    EXPECT_TRUE(process.last_application.empty());
    EXPECT_EQ(process.last_command, u"\"C:\\WINDOWS\\SysWOW64\\cmd.exe\" /c exit 0");
    EXPECT_EQ(process.last_directory, u"C:\\");
}

// A program that cannot be started fails (1721); Continue makes it success.
TEST_P(Msi_customaction_Test, ExeThatCannotStart) {
    session.set_property("EXEPATH", "C:\\missing.exe");
    process.create_fails = true;
    EXPECT_EQ(run("CaExeMissing", 50, "EXEPATH", ""), 1603u);
    EXPECT_EQ(run("CaExeMissingCont", 50 + 64, "EXEPATH", ""), 0u);
}

// Async + Continue (192) on an EXE: not waited for. Async alone (128):
// waited for when the session ends, and (measured exe3: exit 4 logged as
// 1603, install exit 0) its result never fails the install.
TEST_P(Msi_customaction_Test, AsyncExe) {
    session.set_property("EXEPATH", "C:\\WINDOWS\\SysWOW64\\cmd.exe");
    process.exit_code = 4;
    EXPECT_EQ(run("CaNoWait", 50 + 192, "EXEPATH", "/c exit 9"), 0u);
    EXPECT_EQ(process.waited, 0);
    EXPECT_EQ(process.closed, 1);
    EXPECT_EQ(run("CaAsync", 50 + 128, "EXEPATH", "/c exit 4"), 0u);
    EXPECT_EQ(process.waited, 0);
    EXPECT_EQ(finish_async_custom_actions(session), 0u);
    EXPECT_EQ(process.waited, 1);
}

// No Active Scripting engine: script actions fail loudly (1720), and the
// Continue bit turns that into success as for any action failure.
TEST_P(Msi_customaction_Test, ScriptActionsFailLoudly) {
    EXPECT_EQ(run("CaVbs", 38, "", "Function F: F = 1: End Function"), 1603u);
    EXPECT_EQ(run("CaJs", 37 + 64, "", "function f() { return 1; }"), 0u);
}

// Execution scheduling options are ignored outside the server-side execute
// sequence (no engine: no sequence).
TEST_P(Msi_customaction_Test, SchedulingOptionsOutsideExecuteSequence) {
    EXPECT_EQ(run("SetCntClient", 819, "CNT_CLIENT", "x"), 0u);
    EXPECT_EQ(session.property("CNT_CLIENT"), "x");
}

INSTANTIATE_MSI_BITNESS(Msi_customaction_Test);

}  // namespace
}  // namespace vitreo::winapi::msi_test
