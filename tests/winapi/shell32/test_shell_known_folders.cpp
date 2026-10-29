/// @file test_shell_known_folders.cpp
/// @brief shell32 contract tests: SHGetKnownFolderPath and Shell_NotifyIcon.

#include <gtest/gtest.h>
#include <vitreo/common.hpp>

#include "capsule/vfs/vfs.hpp"
#include "winapi/system/shell32/app_user_model_id.hpp"
#include "capsule/vfs/host_filesystem.hpp"
#include "core/cpu/state/cpu_state.hpp"
#include "core/memory/guest_memory.hpp"
#include "winapi/com/ole32/task_memory.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/kernel32/handle.hpp"
#include "winapi/kernel32/memory/heap.hpp"
#include "winapi/kernel32/memory/memory.hpp"
#include "winapi/kernel32/process/process.hpp"
#include "winapi/system/shell32/shell_paths.hpp"
#include "windowing/cocoa/cocoa_bridge.hpp"

#include <array>
#include <unistd.h>
#include <string>
#include <optional>
#include <filesystem>
#include <string_view>
#include <vector>

using namespace vitreo;
using namespace vitreo::winapi;

namespace {

class ShellKnownFolderTest : public ::testing::Test {
protected:
    capsule::Vfs             vfs;
    windowing::CocoaBridge   bridge;
    GuestMemory              memory;
    HandleTable              handles;
    VirtualMemoryManager     vmm{memory};
    HeapManager              heap_mgr{vmm, memory, handles};
    ComTaskMemoryManager     task_memory{heap_mgr};
    ProcessManager           process_mgr;
    ShellPathManager         mgr{vfs, bridge, memory, process_mgr, task_memory};

    const ExportEntry* find_export(std::string_view name) {
        m_exports = create_shell_path_thunks(mgr);
        for (const auto& entry : m_exports.entries) {
            if (entry.name == name) return &entry;
        }
        return nullptr;
    }

private:
    DllExports m_exports;
};

constexpr GuestAddr kRegion = 0x10000;
constexpr GuestAddr kGuid = 0x11000;
constexpr GuestAddr kOut = 0x11100;
constexpr GuestAddr kNotifyData = 0x11200;

/// Windows GUID binary layout: Data1/2/3 little-endian, Data4 byte-ordered.
void write_guid(GuestMemory& memory, GuestAddr addr, u32 d1, u16 d2, u16 d3,
                std::array<u8, 8> d4) {
    ASSERT_TRUE(memory.write_u32(addr, d1).is_ok());
    ASSERT_TRUE(memory.write_u16(addr + 4, d2).is_ok());
    ASSERT_TRUE(memory.write_u16(addr + 6, d3).is_ok());
    for (usize i = 0; i < d4.size(); ++i) {
        ASSERT_TRUE(memory.write_u8(addr + 8 + static_cast<GuestAddr>(i),
                                    d4[i]).is_ok());
    }
}

/// 32-bit stdcall frame: return address at esp, arguments from esp+4.
void call_stdcall32(const ExportEntry& entry, GuestMemory& memory,
                    cpu::CpuState& cpu, const std::vector<u32>& args) {
    constexpr GuestAddr stack = kRegion + 0x800;
    cpu.init_protected_mode();
    cpu.set_sp(stack);
    ASSERT_TRUE(memory.write_u32(stack, 0xDEADBEEF).is_ok());
    for (usize i = 0; i < args.size(); ++i) {
        ASSERT_TRUE(memory.write_u32(
            stack + 4 + static_cast<GuestAddr>(i * 4), args[i]).is_ok());
    }
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    entry.handler(marshaler);
}

// -------------------------------------------------------------------------
// SHGetKnownFolderPath — FOLDERID_LocalAppDataLow
//
// Learn ("KNOWNFOLDERID"): LocalLow is one of the known folders with no CSIDL
// equivalent, so SHGetFolderPath can never return it and SHGetKnownFolderPath
// is the only way in. The GUID was missing from the table, so every caller got
// E_INVALIDARG for a directory the capsule creates on disk.
// -------------------------------------------------------------------------

TEST_F(ShellKnownFolderTest, KnownFolderPathResolvesLocalAppDataLow) {
    const ExportEntry* entry = find_export("SHGetKnownFolderPath");
    ASSERT_NE(entry, nullptr);
    ASSERT_TRUE(memory.map_region(kRegion, 0x2000,
                                  MemoryProtection::ReadWrite).is_ok());
    // FOLDERID_LocalAppDataLow {A520A1A4-1780-4FF6-BD18-167343C5AF16}
    write_guid(memory, kGuid, 0xA520A1A4, 0x1780, 0x4FF6,
               {0xBD, 0x18, 0x16, 0x73, 0x43, 0xC5, 0xAF, 0x16});
    // Poison the out-parameter: a 32-bit store into a PWSTR* leaves the upper
    // half of a Win64 caller's variable set, producing a garbage pointer.
    ASSERT_TRUE(memory.write_u64(kOut, 0xFFFFFFFFFFFFFFFFull).is_ok());

    cpu::CpuState cpu;
    cpu.init_long_mode();
    cpu.gpr.rsp = kRegion + 0x800;
    cpu.gpr.rcx = kGuid;   // rfid
    cpu.gpr.rdx = 0x4000;  // KF_FLAG_DONT_VERIFY (no capsule drive mounted)
    cpu.gpr.r8 = 0;        // hToken
    cpu.gpr.r9 = kOut;     // ppszPath
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    entry->handler(marshaler);

    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 0u) << "S_OK";
    const u64 path_ptr = memory.read_u64(kOut).value();
    EXPECT_NE(path_ptr >> 32, 0xFFFFFFFFull)
        << "the whole pointer-sized out slot must be written";
    ASSERT_NE(path_ptr, 0u);
    EXPECT_EQ(marshaler.read_wide_string(
                  static_cast<GuestAddr>(path_ptr)).value_or(""),
              "C:\\Users\\Default\\AppData\\LocalLow");
}

TEST_F(ShellKnownFolderTest, KnownFolderPathRejectsUnknownFolderId) {
    const ExportEntry* entry = find_export("SHGetKnownFolderPath");
    ASSERT_NE(entry, nullptr);
    ASSERT_TRUE(memory.map_region(kRegion, 0x2000,
                                  MemoryProtection::ReadWrite).is_ok());
    write_guid(memory, kGuid, 0, 0, 0, {0, 0, 0, 0, 0, 0, 0, 0});

    cpu::CpuState cpu;
    cpu.init_long_mode();
    cpu.gpr.rsp = kRegion + 0x800;
    cpu.gpr.rcx = kGuid;
    cpu.gpr.rdx = 0;
    cpu.gpr.r8 = 0;
    cpu.gpr.r9 = kOut;
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    ASSERT_TRUE(memory.write_u64(kOut, 0x1234).is_ok());
    entry->handler(marshaler);

    // Windows: an unregistered KNOWNFOLDERID is
    // HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) and *ppszPath is NULL
    // (tools/oracle_probe/shell_paths_probe.expected kf_bogus).
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 0x80070002u);
    EXPECT_EQ(memory.read_u64(kOut).value(), 0u);
}

TEST_F(ShellKnownFolderTest, KnownFolderPathVerifiesAndCoversEveryWindowsFolder) {
    const ExportEntry* entry = find_export("SHGetKnownFolderPath");
    ASSERT_NE(entry, nullptr);
    ASSERT_TRUE(memory.map_region(kRegion, 0x2000,
                                  MemoryProtection::ReadWrite).is_ok());
    auto call = [&](u32 d1, u16 d2, u16 d3, std::array<u8, 8> d4, u32 flags) {
        write_guid(memory, kGuid, d1, d2, d3, d4);
        cpu::CpuState cpu;
        cpu.init_long_mode();
        cpu.gpr.rsp = kRegion + 0x800;
        cpu.gpr.rcx = kGuid;
        cpu.gpr.rdx = flags;
        cpu.gpr.r8 = 0;
        cpu.gpr.r9 = kOut;
        Marshaler marshaler(memory);
        marshaler.attach_cpu(cpu);
        entry->handler(marshaler);
        return std::pair<u32, std::string>{
            static_cast<u32>(cpu.gpr.rax),
            memory.read_u64(kOut).value() == 0
                ? std::string{}
                : marshaler.read_wide_string(
                      static_cast<GuestAddr>(memory.read_u64(kOut).value())).value_or("")};
    };
    // FOLDERID_SavedGames {4C5C32FF-BB9D-43B0-B5B4-2D72E54EAAA4}
    auto saved = call(0x4C5C32FF, 0xBB9D, 0x43B0,
                      {0xB5, 0xB4, 0x2D, 0x72, 0xE5, 0x4E, 0xAA, 0xA4}, 0x4000);
    EXPECT_EQ(saved.first, 0u);
    EXPECT_EQ(saved.second, "C:\\Users\\Default\\Saved Games");
    // Without KF_FLAG_DONT_VERIFY a folder that is not present fails with
    // ERROR_FILE_NOT_FOUND and a NULL path (probe kf_verify_screenshots).
    auto verify = call(0x4C5C32FF, 0xBB9D, 0x43B0,
                       {0xB5, 0xB4, 0x2D, 0x72, 0xE5, 0x4E, 0xAA, 0xA4}, 0);
    EXPECT_EQ(verify.first, 0x80070002u);
    EXPECT_TRUE(verify.second.empty());
    // FOLDERID_ComputerFolder is virtual: E_FAIL (probe).
    auto virt = call(0x0AC0837C, 0xBBF8, 0x452A,
                     {0x85, 0x0D, 0x79, 0xD0, 0x8E, 0x66, 0x7C, 0xA7}, 0x4000);
    EXPECT_EQ(virt.first, 0x80004005u);
}

// -------------------------------------------------------------------------
// Shell_NotifyIcon for a window that does not exist
//
// The notification area itself is tested in test_notify_icons.cpp against the
// tray and click probes. Here the thunks are driven with an hWnd no window
// has: NIM_ADD fails with E_FAIL (notifyicon_tray_probe "add id5 NULL hwnd",
// s7_notifyicon_probe "NIM_ADD bad hwnd"), and with nothing added every other
// message, and an unknown one, fails the same way.
// -------------------------------------------------------------------------

TEST_F(ShellKnownFolderTest, NotifyIconFailsLikeWindowsWithoutATray) {
    ASSERT_TRUE(memory.map_region(kRegion, 0x2000,
                                  MemoryProtection::ReadWrite).is_ok());
    for (const char* name : {"Shell_NotifyIconW", "Shell_NotifyIconA"}) {
        const ExportEntry* entry = find_export(name);
        ASSERT_NE(entry, nullptr) << name;
        EXPECT_EQ(entry->stdcall_bytes, 8u);
        for (u32 size : {956u, 152u, 10u}) {
            ASSERT_TRUE(memory.write_u32(kNotifyData, size).is_ok());
            for (u32 message : {0u, 1u, 2u, 3u, 4u, 7u}) {
                set_last_error(0xDEADu);
                cpu::CpuState cpu;
                call_stdcall32(*entry, memory, cpu, {message, kNotifyData});
                EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 0u) << name << " msg " << message;
                EXPECT_EQ(get_last_error(), 0x80004005u) << name << " msg " << message;
            }
        }
    }
}

// SHAddToRecentDocs: what Windows 11 records in the Recent folder
// (tools/oracle_probe/shlwapi_regsec_shell_probe{,.x86}.expected recent_*,
// interactive session). Both calling conventions.
TEST_F(ShellKnownFolderTest, AddToRecentDocsWritesRecentFolderLinks) {
    char root[] = "/tmp/vitreo-recent-docs-XXXXXX";
    ASSERT_NE(mkdtemp(root), nullptr);
    ASSERT_TRUE(vfs.mount_drive('C', std::make_unique<capsule::HostFileSystem>(root)).is_ok());
    ASSERT_TRUE(vfs.create_dir("C:\\Docs").is_ok());
    ASSERT_TRUE(vfs.create_dir("C:\\Temp").is_ok());
    for (const char* name : {"C:\\Docs\\w.txt", "C:\\Docs\\a.txt", "C:\\Docs\\pidl.txt",
                             "C:\\Docs\\tool.exe", "C:\\Temp\\t.txt"}) {
        auto h = vfs.open(name, capsule::AccessMode::Write, capsule::ShareMode::Read,
                          capsule::CreationDisposition::CreateAlways);
        ASSERT_TRUE(h.is_ok()) << name;
        (void)vfs.close(h.value());
    }
    mgr.set_environment_lookup([](std::string_view name) -> std::optional<std::string> {
        if (name == "TEMP") return std::string("C:\\Temp");
        return std::nullopt;
    });
    std::wstring recent_w;
    ASSERT_EQ(mgr.get_folder_path(0x0008, recent_w), 0u);
    std::string recent;
    for (wchar_t c : recent_w) recent.push_back(static_cast<char>(c));
    auto linked = [&](const std::string& name) { return vfs.stat(recent + "\\" + name + ".lnk").is_ok(); };
    ASSERT_TRUE(memory.map_region(kRegion, 0x4000, MemoryProtection::ReadWrite).is_ok());

    for (bool x64 : {false, true}) {
        auto call = [&](u32 flags, GuestAddr pv) {
            cpu::CpuState cpu;
            if (x64) cpu.init_long_mode(); else cpu.init_protected_mode();
            cpu.gpr.rsp = kRegion + 0x3800;
            if (x64) {
                cpu.gpr.rcx = flags;
                cpu.gpr.rdx = pv;
            } else {
                (void)memory.write_u32(cpu.gpr.rsp + 4, flags);
                (void)memory.write_u32(cpu.gpr.rsp + 8, static_cast<u32>(pv));
            }
            Marshaler m(memory);
            m.attach_cpu(cpu);
            shell::add_to_recent_docs(m, mgr, vfs);
        };
        auto wide = [&](std::u16string_view text) {
            for (usize i = 0; i < text.size(); ++i) (void)memory.write_u16(kRegion + i * 2, text[i]);
            (void)memory.write_u16(kRegion + text.size() * 2, 0);
            return kRegion;
        };
        auto ansi = [&](std::string_view text) {
            for (usize i = 0; i < text.size(); ++i) (void)memory.write_u8(kRegion + i, static_cast<u8>(text[i]));
            (void)memory.write_u8(kRegion + text.size(), 0);
            return kRegion;
        };
        call(3, wide(u"C:\\Docs\\w.txt"));                       // recent_pathw
        EXPECT_TRUE(linked("w.txt"));
        EXPECT_TRUE(linked("Docs"));                              // recent_dir_link
        call(2, ansi("C:\\Docs\\a.txt"));                        // recent_patha
        EXPECT_TRUE(linked("a.txt"));
        call(3, wide(u"C:\\Docs\\tool.exe"));                    // recent_exe
        EXPECT_FALSE(linked("tool.exe"));
        call(3, wide(u"C:\\Temp\\t.txt"));                       // recent_in_temp
        EXPECT_FALSE(linked("t.txt"));
        call(3, wide(u"C:\\Docs\\missing.txt"));                 // recent_missing_file
        EXPECT_TRUE(linked("missing.txt"));
        vfs.set_cwd("C:\\Docs");
        call(3, wide(u"relative.txt"));                          // recent_relative
        EXPECT_TRUE(linked("relative.txt"));
        call(0x99, wide(u"C:\\Docs\\bad.txt"));                  // recent_badflag
        EXPECT_FALSE(linked("bad.txt"));

        // The link is an MS-SHLLINK file (link_header_size=76).
        auto h = vfs.open(recent + "\\w.txt.lnk", capsule::AccessMode::Read,
                          capsule::ShareMode::Read, capsule::CreationDisposition::OpenExisting);
        ASSERT_TRUE(h.is_ok());
        std::vector<u8> header(0x4C);
        auto got = vfs.read(h.value(), header);
        (void)vfs.close(h.value());
        ASSERT_TRUE(got.is_ok());
        EXPECT_EQ(header[0], 0x4Cu);
        EXPECT_EQ(header[4], 0x01u);  // CLSID_ShellLink {00021401-...}
        EXPECT_EQ(header[5], 0x14u);
        EXPECT_EQ(header[6], 0x02u);

        call(3, 0);                                              // recent_clear
        EXPECT_FALSE(linked("w.txt"));
        EXPECT_FALSE(linked("a.txt"));
        EXPECT_FALSE(linked("Docs"));
    }
    std::filesystem::remove_all(root);
}

}  // namespace
