// SPDX-License-Identifier: LGPL-2.1-only

/// @file test_wow64_fsredir.cpp
/// @brief WOW64 file-system redirection against Windows: every "path ->
///        what it reached" row and every Wow64*FsRedirection row of
///        oracle/wow64_fsredir_probe.expected (winoracle, Windows 11 26200,
///        elevated; the probe's setup step writes "native" under System32
///        and "wow" under SysWOW64), replayed through capsule::Vfs and the
///        sysinfo thunks for the 32-bit and the 64-bit process.

#include <gtest/gtest.h>

#include "core/cpu/state/cpu_state.hpp"
#include "core/memory/guest_memory.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/kernel32/system/sysinfo.hpp"

#include <capsule/vfs/host_filesystem.hpp>
#include <capsule/vfs/vfs.hpp>

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace vitreo;
using namespace vitreo::winapi;

namespace {

const std::filesystem::path kHere = std::filesystem::path(__FILE__).parent_path();
constexpr const char* kDirs[] = {"", "catroot", "catroot2", "drivers/etc", "driverstore", "logfiles",
                                 "spool", "drivers", "config", "wbem"};

/// The probe's output for one process, keyed by its label column.
std::map<std::string, std::string> expected_rows(bool is64) {
    std::ifstream in(kHere / "oracle" / "wow64_fsredir_probe.expected");
    std::map<std::string, std::string> rows;
    bool in_section = false;
    for (std::string line; std::getline(in, line);) {
        if (line.rfind("bits=", 0) == 0) {
            in_section = line.rfind(is64 ? "bits=64" : "bits=32", 0) == 0;
            continue;
        }
        if (!in_section || line.size() < 29) continue;
        std::string label = line.substr(0, 28);
        label.erase(label.find_last_not_of(' ') + 1);
        rows[label] = line.substr(29);
    }
    return rows;
}

class Wow64Fixture {
public:
    explicit Wow64Fixture(bool is64) {
        root_ = std::filesystem::temp_directory_path() /
                ("vitreo-wow64-" + std::to_string(::getpid()) + (is64 ? "-64" : "-32"));
        std::error_code ec;
        std::filesystem::remove_all(root_, ec);
        for (const char* d : kDirs) {
            for (const auto& [dir, text] : {std::pair{"System32", "native"}, std::pair{"SysWOW64", "wow"}}) {
                const auto at = root_ / "Windows" / dir / d;
                std::filesystem::create_directories(at);
                // driverstore refused the probe's write (ERROR_ACCESS_DENIED).
                if (std::string(d) != "driverstore") std::ofstream(at / "vprobe_fsr.txt") << text;
            }
        }
        std::filesystem::create_directories(root_ / "Windows" / "Temp");
        EXPECT_TRUE(vfs.mount_drive('C', std::make_unique<capsule::HostFileSystem>(root_.string())).is_ok());
        vfs.set_wow64_view(!is64);
    }
    ~Wow64Fixture() {
        std::error_code ec;
        std::filesystem::remove_all(root_, ec);
    }

    /// What the probe's reach() prints for @p path.
    std::string reach(const std::string& path) {
        auto h = vfs.open(path, capsule::AccessMode::Read, capsule::ShareMode::Read,
                          capsule::CreationDisposition::OpenExisting);
        if (h.is_err()) {
            return h.error() == capsule::VfsError::PathNotFound ? "err3" : "err2";
        }
        std::array<u8, 32> buf{};
        auto n = vfs.read(h.value(), buf);
        (void)vfs.close(h.value());
        return std::string(reinterpret_cast<const char*>(buf.data()), n.is_ok() ? n.value() : 0);
    }

    capsule::Vfs vfs;

private:
    std::filesystem::path root_;
};

void path_rows_match_windows(bool is64) {
    auto rows = expected_rows(is64);
    ASSERT_FALSE(rows.empty());
    Wow64Fixture f(is64);
    capsule::Vfs::set_thread_redirection_disabled(false);
    usize checked = 0;
    const auto check = [&](const std::string& label) {
        const auto it = rows.find(label);
        ASSERT_NE(it, rows.end()) << label;
        const auto arrow = it->second.rfind(" -> ");
        ASSERT_NE(arrow, std::string::npos) << label;
        const std::string path = it->second.substr(0, arrow);
        EXPECT_EQ(f.reach(path), it->second.substr(arrow + 4)) << label << ": " << path;
        ++checked;
    };
    for (const char* label : {"system32", "catroot", "catroot2", "drivers\\etc", "driverstore", "logfiles", "spool",
                              "drivers", "config", "wbem", "sysnative", "sysnative-etc", "syswow64", "upper",
                              "slash", "dotdot", "long-prefix", "nt-global"}) {
        check(label);
    }
    {
        // GetFileAttributes(C:\Windows\Sysnative): a directory in WOW64,
        // ERROR_FILE_NOT_FOUND natively.
        const auto st = f.vfs.stat("C:\\Windows\\Sysnative");
        const std::string got = st.is_ok() ? "attrs(Sysnative) -> 0x10 err=0" : "attrs(Sysnative) -> 0xffffffff err=2";
        EXPECT_EQ(got, rows["sysnative-dir"]);
    }
    f.vfs.set_cwd("C:\\Windows\\System32");
    check("relative");
    check("relative-etc");
    f.vfs.set_cwd("C:\\");

    // The per-thread switch: disabled here, still on for a new thread.
    capsule::Vfs::set_thread_redirection_disabled(true);
    check("disabled-system32");
    check("disabled-sysnative");
    std::string other;
    std::thread([&] { other = f.reach("C:\\Windows\\System32\\vprobe_fsr.txt"); }).join();
    const std::string& row = rows["per-thread other"];
    EXPECT_EQ(other, row.substr(row.rfind(" -> ") + 4));
    capsule::Vfs::set_thread_redirection_disabled(false);
    check("reverted");
    EXPECT_EQ(checked, 23u);
}

struct Thunks {
    explicit Thunks(bool is64) : x64(is64) {
        EXPECT_TRUE(memory.map_region(0x10000, 0x10000, MemoryProtection::ReadWrite).is_ok());
        if (x64) cpu.init_long_mode(); else cpu.init_protected_mode();
        cpu.set_sp(0x18000);
        m.attach_cpu(cpu);
        exports = create_sysinfo_thunks(sysinfo);
    }
    u64 call(const char* name, u64 arg) {
        if (x64) cpu.gpr.rcx = arg;
        else (void)memory.write_u32(0x18004, static_cast<u32>(arg));
        for (auto& e : exports.entries) {
            if (e.name == name) {
                EXPECT_EQ(e.stdcall_bytes, 4u) << name;
                e.handler(m);
                return x64 ? cpu.gpr.rax : static_cast<u32>(cpu.gpr.rax);
            }
        }
        ADD_FAILURE() << "missing " << name;
        return 0;
    }
    bool x64;
    GuestMemory memory;
    cpu::CpuState cpu;
    Marshaler m{memory};
    SysInfoManager sysinfo;
    DllExports exports;
};

/// "ret=%d err=%#lx[ old=%p]" for the API rows.
std::string api_row(u64 ret, u32 err) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "ret=%d err=%#x", static_cast<int>(ret), err);
    return buf;
}

void api_rows_match_windows(bool is64) {
    auto rows = expected_rows(is64);
    Thunks t(is64);
    capsule::Vfs::set_thread_redirection_disabled(false);
    constexpr GuestAddr kOld = 0x11000;
    const auto prefix = [&](const char* label) {
        const std::string& r = rows[label];
        const auto old = r.find(" old=");
        return old == std::string::npos ? r : r.substr(0, old);
    };
    const auto old_value = [&](const char* label) {
        const std::string& r = rows[label];
        return std::stoull(r.substr(r.find(" old=") + 5), nullptr, 16);
    };
    const auto read_old = [&] { return is64 ? t.memory.read_u64(kOld).value() : t.memory.read_u32(kOld).value(); };

    (void)t.memory.write_u64(kOld, 0x1234);
    set_last_error(0xdead);
    u64 r = t.call("Wow64DisableWow64FsRedirection", kOld);
    EXPECT_EQ(api_row(r, get_last_error()), prefix("disable"));
    EXPECT_EQ(read_old(), old_value("disable"));
    const u64 first_old = read_old();
    (void)t.memory.write_u64(kOld, 0x5678);
    set_last_error(0xdead);
    r = t.call("Wow64DisableWow64FsRedirection", kOld);
    EXPECT_EQ(api_row(r, get_last_error()), prefix("disable-again"));
    EXPECT_EQ(read_old(), old_value("disable-again"));
    set_last_error(0xdead);
    r = t.call("Wow64RevertWow64FsRedirection", read_old());
    EXPECT_EQ(api_row(r, get_last_error()), rows["revert-inner"]);
    set_last_error(0xdead);
    r = t.call("Wow64RevertWow64FsRedirection", first_old);
    EXPECT_EQ(api_row(r, get_last_error()), rows["revert"]);
    EXPECT_FALSE(capsule::Vfs::thread_redirection_disabled());
    set_last_error(0xdead);
    r = t.call("Wow64EnableWow64FsRedirection", 0);
    EXPECT_EQ(api_row(r, get_last_error()), rows["enable(FALSE)"]);
    EXPECT_EQ(capsule::Vfs::thread_redirection_disabled(), !is64);
    set_last_error(0xdead);
    r = t.call("Wow64EnableWow64FsRedirection", 1);
    EXPECT_EQ(api_row(r, get_last_error()), rows["enable(TRUE)"]);
    EXPECT_FALSE(capsule::Vfs::thread_redirection_disabled());
    set_last_error(0xdead);
    r = t.call("Wow64DisableWow64FsRedirection", 0);
    EXPECT_EQ(api_row(r, get_last_error()), rows["disable(NULL)"]);
    capsule::Vfs::set_thread_redirection_disabled(false);
}

}  // namespace

TEST(Wow64FsRedirectionOracle, PathsMatchWindowsX86) { path_rows_match_windows(false); }
TEST(Wow64FsRedirectionOracle, PathsMatchWindowsX64) { path_rows_match_windows(true); }
TEST(Wow64FsRedirectionOracle, ApisMatchWindowsX86) { api_rows_match_windows(false); }
TEST(Wow64FsRedirectionOracle, ApisMatchWindowsX64) { api_rows_match_windows(true); }
