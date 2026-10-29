// SPDX-License-Identifier: LGPL-2.1-only

/// @file test_system_programs.cpp
/// @brief regsvr32.exe against Windows, and the generated executable that
///        hosts it. The regsvr32 rows come from
///        oracle/regsvr32/regsvr32_probe.expected (run.ps1 drives System32
///        and SysWOW64 regsvr32.exe over regdll.dll / noentry.dll on
///        winoracle); a host double plays the DLL, so every row's exit code
///        and entry-point call order is checked. The bitness hand-off rows
///        come from tools/oracle_probe/regsvr32_bitness (each regsvr32 given
///        DLLs of the other bitness).

#include <gtest/gtest.h>

#include "winapi/kernel32/module/system_programs.hpp"
#include "winapi/system/sysprog/regsvr32.hpp"

#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

using namespace vitreo;
using namespace vitreo::winapi;
using namespace vitreo::winapi::sysprog;

namespace {

const std::filesystem::path kHere = std::filesystem::path(__FILE__).parent_path();

struct Case {
    const char* label;
    u32 hr;  ///< what regdll.dll's entry points return (REGDLL_HR)
    std::vector<std::string> args;
};

// run.ps1, case for case.
const std::vector<Case> kCases = {
    {"register", 0, {"/s", "DIR\\regdll.dll"}},
    {"register-fail", 0x80004005u, {"/s", "DIR\\regdll.dll"}},
    {"unregister", 0, {"/s", "/u", "DIR\\regdll.dll"}},
    {"unregister-fail", 0x80004005u, {"/s", "/u", "DIR\\regdll.dll"}},
    {"install", 0, {"/s", "/i:hello", "DIR\\regdll.dll"}},
    {"install-noreg", 0, {"/s", "/n", "/i:hello", "DIR\\regdll.dll"}},
    {"install-nocmd", 0, {"/s", "/n", "/i", "DIR\\regdll.dll"}},
    {"uninstall", 0, {"/s", "/u", "/i:bye", "DIR\\regdll.dll"}},
    {"install-fail", 0x80004005u, {"/s", "/n", "/i:x", "DIR\\regdll.dll"}},
    {"missing-dll", 0, {"/s", "DIR\\nope.dll"}},
    {"no-entry", 0, {"/s", "DIR\\noentry.dll"}},
    {"no-args", 0, {"/s"}},
    {"n-without-i", 0, {"/s", "/n", "DIR\\regdll.dll"}},
    {"two-dlls", 0, {"/s", "DIR\\regdll.dll", "DIR\\regdll.dll"}},
    {"bad-switch", 0, {"/s", "/q", "DIR\\regdll.dll"}},
    {"relative", 0, {"/s", "regdll.dll"}},
    {"dash-switch", 0, {"-s", "DIR\\regdll.dll"}},
};

std::u16string to_u16(const std::string& s) { return std::u16string(s.begin(), s.end()); }
std::string u8(const std::u16string& s) { return std::string(s.begin(), s.end()); }

/// Plays regdll.dll / noentry.dll and records what regsvr32 did to them in
/// the probe log's words.
class FakeHost final : public Regsvr32Host {
public:
    explicit FakeHost(u32 hr) : hr_(hr) {}
    std::vector<std::string> log;
    int ole_depth = 0;

    void ole_initialize() override { ++ole_depth; }
    void ole_uninitialize() override { --ole_depth; }
    u64 load_library(const std::u16string& path) override {
        const std::string p = u8(path);
        const bool regdll = p.size() >= 10 && p.compare(p.size() - 10, 10, "regdll.dll") == 0;
        const bool noentry = p.find("noentry.dll") != std::string::npos;
        if (!regdll && !noentry) return 0;
        if (regdll) log.push_back("DllMain(PROCESS_ATTACH)");
        return regdll ? 1 : 2;
    }
    u64 get_proc_address(u64 module, const char* name) override {
        if (module != 1) return 0;
        const std::string n = name;
        return n == "DllRegisterServer" ? 10 : n == "DllUnregisterServer" ? 11 : n == "DllInstall" ? 12 : 0;
    }
    void free_library(u64) override {}
    u32 call_register(u64 entry) override {
        EXPECT_EQ(ole_depth, 1) << "entry points run on an OleInitialize'd thread";
        log.push_back(entry == 10 ? "DllRegisterServer" : "DllUnregisterServer");
        return hr_;
    }
    u32 call_install(u64, bool install, const std::u16string& cmd) override {
        EXPECT_EQ(ole_depth, 1);
        log.push_back(std::string(install ? "DllInstall(TRUE)" : "DllInstall(FALSE)") + " [" + u8(cmd) + "]");
        return hr_;
    }
    // regdll.dll and noentry.dll are this regsvr32's bitness: no hand-off.
    [[nodiscard]] bool is_64bit() const override { return true; }
    u16 image_machine(const std::u16string&) override { return 0; }
    std::optional<u32> run_other_regsvr32(const std::u16string&) override {
        ADD_FAILURE() << "no case hands off";
        return std::nullopt;
    }

private:
    u32 hr_;
};

/// label -> (exit code, call lines) for one bitness of the probe output.
std::map<std::string, std::pair<u32, std::vector<std::string>>> expected(bool is64) {
    std::ifstream in(kHere / "oracle" / "regsvr32" / "regsvr32_probe.expected");
    std::map<std::string, std::pair<u32, std::vector<std::string>>> rows;
    bool section = false;
    std::string current;
    for (std::string line; std::getline(in, line);) {
        if (line.rfind("== ", 0) == 0) {
            section = line.rfind(is64 ? "== x64" : "== x86", 0) == 0;
            continue;
        }
        if (!section) continue;
        if (line.rfind("  ", 0) == 0) {
            // "  DllInstall(TRUE) arg=[hello] apt_hr=..." -> "DllInstall(TRUE) [hello]"
            const std::string what = line.substr(2, line.find(' ', 2) - 2);
            std::string call = what;
            if (what.rfind("DllInstall", 0) == 0) {
                const auto a = line.find("arg=[") + 5;
                call += " [" + line.substr(a, line.find(']', a) - a) + "]";
            }
            rows[current].second.push_back(call);
            // Every entry point ran on the main STA, OleInitialize'd (apt=3,
            // CoInitializeEx S_FALSE).
            EXPECT_NE(line.find("apt=3 coinit=0x1"), std::string::npos) << line;
            continue;
        }
        const auto exit = line.find(" exit=");
        current = line.substr(0, exit);
        rows[current].first = static_cast<u32>(std::stoul(line.substr(exit + 6)));
    }
    return rows;
}

}  // namespace

TEST(Regsvr32Oracle, EveryCaseMatchesWindowsBothBitnesses) {
    for (bool is64 : {true, false}) {
        SCOPED_TRACE(is64 ? "x64 System32" : "x86 SysWOW64");
        const auto rows = expected(is64);
        ASSERT_EQ(rows.size(), kCases.size());
        for (const auto& c : kCases) {
            SCOPED_TRACE(c.label);
            const auto it = rows.find(c.label);
            ASSERT_NE(it, rows.end());
            std::vector<std::u16string> argv{u"regsvr32.exe"};
            for (const auto& a : c.args) argv.push_back(to_u16(a.substr(0, 4) == "DIR\\" ? "C:\\vprobe\\" + a.substr(4) : a));
            FakeHost host(c.hr);
            std::u16string command_line = u"regsvr32.exe";
            for (usize i = 1; i < argv.size(); ++i) command_line += u" " + argv[i];
            EXPECT_EQ(run_regsvr32(command_line, argv, host), it->second.first);
            EXPECT_EQ(host.log, it->second.second);
            EXPECT_EQ(host.ole_depth, 0);
        }
    }
}

namespace {

/// One regsvr32 process of the bitness probe: plays bitdll_*.dll,
/// noentry_*.dll, depdll_*.dll (its import missing), text.dll and the missing
/// nope.dll, and runs the other bitness's regsvr32 as a child process, logging
/// in the probe's words.
struct BitnessProcess final : Regsvr32Host {
    struct Shared {
        u32 hr = 0;
        std::vector<std::string> log;
        bool endless = false;  ///< the hand-offs did not stop
    };
    BitnessProcess(Shared& shared, bool is64, std::string parent, std::u16string command_line, int depth)
        : shared_(shared), is64_(is64), parent_(std::move(parent)), command_line_(std::move(command_line)),
          depth_(depth) {}
    int children = 0;

    [[nodiscard]] std::string image() const { return is64_ ? "System32\\regsvr32.exe" : "SysWOW64\\regsvr32.exe"; }
    static std::string leaf(const std::u16string& path) {
        const std::string p = u8(path);
        const auto at = p.find_last_of('\\');
        return at == std::string::npos ? p : p.substr(at + 1);
    }
    void ole_initialize() override {}
    void ole_uninitialize() override {}
    u64 load_library(const std::u16string& path) override {
        const std::string name = leaf(path);
        const std::string mine = is64_ ? "_x64.dll" : "_x86.dll";
        const bool same = name.size() > mine.size() && name.compare(name.size() - mine.size(), mine.size(), mine) == 0;
        if (same && name.rfind("bitdll", 0) == 0) return 1;
        if (same && name.rfind("noentry", 0) == 0) return 2;
        return 0;  // the other machine, depdll (nosuch.dll absent), text.dll, nope.dll
    }
    u64 get_proc_address(u64 module, const char* name) override {
        if (module != 1) return 0;
        const std::string n = name;
        return n == "DllRegisterServer" ? 10 : n == "DllUnregisterServer" ? 11 : n == "DllInstall" ? 12 : 0;
    }
    void free_library(u64) override {}
    u32 call_register(u64 entry) override {
        note(entry == 10 ? "DllRegisterServer" : "DllUnregisterServer", "(none)");
        return shared_.hr;
    }
    u32 call_install(u64, bool install, const std::u16string& cmd) override {
        note(install ? "DllInstall(TRUE)" : "DllInstall(FALSE)", u8(cmd));
        return shared_.hr;
    }
    [[nodiscard]] bool is_64bit() const override { return is64_; }
    u16 image_machine(const std::u16string& path) override {
        const std::string name = leaf(path);
        if (name == "text.dll" || name == "nope.dll") return 0;
        return name.find("_x86.dll") != std::string::npos ? 0x014C : 0x8664;
    }
    std::optional<u32> run_other_regsvr32(const std::u16string& command_line) override {
        ++children;
        if (depth_ >= 8) {
            shared_.endless = true;
            return 0;
        }
        BitnessProcess child(shared_, !is64_, image(), command_line, depth_ + 1);
        return run_regsvr32(command_line, argv_of(command_line), child);
    }

    /// CommandLineToArgvW for the probe's command lines (quotes, no
    /// backslash-quote sequences).
    static std::vector<std::u16string> argv_of(const std::u16string& cmd) {
        std::vector<std::u16string> argv;
        usize i = 0;
        std::u16string first;
        if (!cmd.empty() && cmd[0] == u'"') {
            for (++i; i < cmd.size() && cmd[i] != u'"'; ++i) first.push_back(cmd[i]);
            if (i < cmd.size()) ++i;
        } else {
            for (; i < cmd.size() && cmd[i] != u' '; ++i) first.push_back(cmd[i]);
        }
        argv.push_back(first);
        for (;;) {
            while (i < cmd.size() && cmd[i] == u' ') ++i;
            if (i >= cmd.size()) break;
            std::u16string arg;
            bool quoted = false;
            for (; i < cmd.size() && (quoted || cmd[i] != u' '); ++i) {
                if (cmd[i] == u'"') quoted = !quoted;
                else arg.push_back(cmd[i]);
            }
            argv.push_back(arg);
        }
        return argv;
    }

private:
    void note(const std::string& what, const std::string& arg) {
        shared_.log.push_back(what + " arg=[" + arg + "] bits=" + (is64_ ? "64" : "32") + " process=" + image() +
                              " parent=" + parent_ + " parent_state=running");
        shared_.log.push_back("  cmdline=[" + u8(command_line_) + "]");
    }
    Shared& shared_;
    bool is64_;
    std::string parent_;
    std::u16string command_line_;
    int depth_;
};

struct BitnessRow {
    std::string label, args;
    std::optional<u32> exit;  ///< nullopt: still running at the timeout
    int children = 0;
    std::vector<std::string> log;
    bool log_truncated = false;
};

std::vector<BitnessRow> bitness_rows(bool is64) {
    std::ifstream in(kHere.parent_path().parent_path().parent_path() / "tests" / "oracle_data" / "regsvr32_bitness" /
                     "regsvr32_bitness.expected");
    std::vector<BitnessRow> rows;
    bool section = false;
    for (std::string line; std::getline(in, line);) {
        if (line.rfind("== ", 0) == 0) {
            section = line.rfind(is64 ? "== x64" : "== x86", 0) == 0;
            continue;
        }
        if (!section || line.empty()) continue;
        if (line.rfind("  ... ", 0) == 0) {
            rows.back().log_truncated = true;
        } else if (line.rfind("  ", 0) == 0) {
            rows.back().log.push_back(line.substr(2));
        } else {
            BitnessRow row;
            const auto open = line.find(" [");
            const auto close = line.rfind("] exit=");
            row.label = line.substr(0, open);
            row.args = line.substr(open + 2, close - open - 2);
            const std::string code = line.substr(close + 7);
            if (code[0] != '(') row.exit = static_cast<u32>(std::stoul(code));
            row.children = std::stoi(line.substr(line.rfind("children=") + 9));
            rows.push_back(std::move(row));
        }
    }
    return rows;
}

}  // namespace

TEST(Regsvr32Oracle, HandsOtherBitnessDllsToTheOtherRegsvr32) {
    const std::map<std::string, u32> hr = {{"register-fail", 0x80004005u},
                                           {"register-hr-1", 0x00000001u},
                                           {"unregister-fail", 0x80070005u},
                                           {"install-fail", 0x80004005u}};
    for (bool is64 : {true, false}) {
        SCOPED_TRACE(is64 ? "x64 System32" : "x86 SysWOW64");
        const auto rows = bitness_rows(is64);
        ASSERT_EQ(rows.size(), 31u);
        for (const auto& row : rows) {
            SCOPED_TRACE(row.label);
            BitnessProcess::Shared shared;
            if (const auto it = hr.find(row.label); it != hr.end()) shared.hr = it->second;
            // System.Diagnostics.Process: the quoted program path, a space,
            // the arguments.
            const std::u16string command_line =
                to_u16(std::string("\"C:\\WINDOWS\\") + (is64 ? "System32" : "SysWOW64") + "\\regsvr32.exe\" " + row.args);
            BitnessProcess top(shared, is64, "v1.0\\powershell.exe", command_line, 0);
            const u32 code = run_regsvr32(command_line, BitnessProcess::argv_of(command_line), top);
            EXPECT_EQ(top.children, row.children);
            if (row.exit) {
                EXPECT_EQ(code, *row.exit);
                EXPECT_FALSE(shared.endless);
                EXPECT_EQ(shared.log, row.log);
            } else {
                // Killed at the timeout: endless mixed-bitness hand-offs, or
                // the result message box (no /s) waiting in the child.
                EXPECT_EQ(shared.endless, row.log_truncated);
                ASSERT_GE(shared.log.size(), row.log.size());
                EXPECT_EQ(std::vector<std::string>(shared.log.begin(), shared.log.begin() + row.log.size()), row.log);
            }
        }
    }
}

TEST(Regsvr32, CommandLineArgumentsDropOnlyTheProgramToken) {
    EXPECT_EQ(command_line_arguments(u"\"C:\\W\\System32\\regsvr32.exe\" /s \"a b.dll\""), u" /s \"a b.dll\"");
    EXPECT_EQ(command_line_arguments(u"regsvr32.exe  /s x.dll"), u"  /s x.dll");
    EXPECT_EQ(command_line_arguments(u"regsvr32.exe\t/s"), u"\t/s");
    EXPECT_EQ(command_line_arguments(u" /s x.dll"), u" /s x.dll");  // a hand-off child's own line
    EXPECT_EQ(command_line_arguments(u"\"unterminated /s"), u"");
    EXPECT_EQ(command_line_arguments(u""), u"");
}

TEST(SystemProgramImage, IsAnExecutableThatImportsItsHostEntry) {
    for (bool is64 : {true, false}) {
        SCOPED_TRACE(is64 ? "x64" : "x86");
        const auto image = build_system_program_image(is64);
        const auto le16 = [&](usize at) { return static_cast<vitreo::u16>(image[at] | (image[at + 1] << 8)); };
        const auto le32 = [&](usize at) {
            return static_cast<u32>(image[at] | (image[at + 1] << 8) | (image[at + 2] << 16) | (image[at + 3] << 24));
        };
        ASSERT_EQ(le16(0), 0x5A4Du);
        const u32 nt = le32(0x3C);
        EXPECT_EQ(le32(nt), 0x00004550u);
        EXPECT_EQ(le16(nt + 4), is64 ? 0x8664u : 0x014Cu);
        EXPECT_EQ(le16(nt + 22) & 0x2000u, 0u) << "not a DLL";
        EXPECT_NE(le16(nt + 22) & 0x0002u, 0u) << "executable";
        const usize opt = nt + 24;
        EXPECT_EQ(le16(opt + 0x44), 2u) << "regsvr32 is a GUI-subsystem program";
        EXPECT_EQ(le32(opt + 0x10), 0x1000u);
        const std::string text(image.begin(), image.end());
        EXPECT_NE(text.find(std::string(kSystemProgramDll)), std::string::npos);
        EXPECT_NE(text.find(std::string(kSystemProgramEntry)), std::string::npos);
        EXPECT_NE(text.find("ExitProcess"), std::string::npos);
    }
    EXPECT_TRUE(is_system_program("REGSVR32.EXE"));
    EXPECT_FALSE(is_system_program("regsvr32.dll"));
}
