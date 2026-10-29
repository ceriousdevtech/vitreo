// SPDX-License-Identifier: LGPL-2.1-only

/// @file shell_paths_registration.cpp
/// @brief Export table for shell32's path and folder-browsing surface.
///
/// Composition only: maps export names to ShellPathManager handlers and records
/// each one's stdcall byte count.
///
/// It carries an anonymous namespace of three helpers, which came with it
/// because they existed only for this table — nothing above the factory in
/// shell_paths.cpp referenced any of them. All three serve SHBrowseForFolder,
/// whose BROWSEINFO out-parameter callers disagree about: one passes lParam
/// as a path, another as a struct starting with an HWND, and telling those
/// apart means sniffing the guest buffer.
///
/// Split out of shell_paths.cpp, which was 2,957 lines.
///
/// The export inventory this produces is verified byte-for-byte by
/// tools/architecture/check_export_inventory.sh.
///
/// VITREO_LARGE_FILE_OK: declarative ABI table — one entry per export, each
/// naming a handler and its stdcall byte count. Plan section 4.1 exempts these
/// because length here tracks how many shell exports Vitreo implements, not how
/// much logic one file carries.

#include "winapi/system/shell32/shell_paths.hpp"
#include "winapi/common/elevation.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/kernel32/process/process.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/com/ole32/task_memory.hpp"
#include "winapi/kernel32/locale/codepage_convert.hpp"
#include "winapi/kernel32/locale/string_conv.hpp"
#include "capsule/vfs/vfs.hpp"
#include "capsule/vfs/host_filesystem.hpp"
#include "winapi/ui/comctl32/comctl32.hpp"
#include "windowing/cocoa/cocoa_bridge.hpp"
#include "core/memory/guest_memory.hpp"
#include <algorithm>
#include <mutex>
#include <cctype>
#include <cstring>
#include <sstream>
#include <cstdio>
#include "winapi/system/shell32/shell_paths_internal.hpp"

namespace vitreo::winapi {

namespace {

/// True if guest memory at `addr` looks like an ANSI filesystem path buffer
/// (empty, UNC, or `X:\…`). Used to distinguish a bare path (lParam → path)
/// from a struct (lParam → `{HWND; char path[]}`).
bool guest_looks_like_ansi_path(Marshaler& m, GuestAddr addr) {
    auto b0 = m.read_u8(addr);
    if (b0.is_err()) {
        return false;
    }
    const u8 c0 = b0.value();
    if (c0 == 0 || c0 == '\\') {
        return true;
    }
    auto b1 = m.read_u8(addr + 1);
    auto b2 = m.read_u8(addr + 2);
    return b1.is_ok() && b2.is_ok() && b1.value() == ':' &&
           (b2.value() == '\\' || b2.value() == '/');
}

bool guest_looks_like_wide_path(Marshaler& m, GuestAddr addr) {
    auto w0 = m.memory().read_u16(addr);
    if (w0.is_err()) {
        return false;
    }
    const u16 c0 = w0.value();
    if (c0 == 0 || c0 == L'\\') {
        return true;
    }
    auto w1 = m.memory().read_u16(addr + 2);
    auto w2 = m.memory().read_u16(addr + 4);
    return w1.is_ok() && w2.is_ok() && w1.value() == L':' &&
           (w2.value() == L'\\' || w2.value() == L'/');
}

/// Write the selected folder path into BROWSEINFO output buffers.
///
/// `BROWSEINFO.lParam` is opaque callback user data — **not** a path buffer.
/// One caller stores `{DWORD hwnd; char path[…]}` there and reads the path
/// from `lParam+4` after return. Writing the path at `lParam+0` overwrote
/// the HWND dword and shifted the path by 4 (`C:\Program…` → `rogram…`).
/// Callers that pass a bare path buffer as lParam are detected and written
/// at +0.
void write_browse_folder_path_ansi(Marshaler& m, GuestAddr bi_ptr,
                                   const std::string& ansi_path) {
    auto dn = m.memory().read_u32(bi_ptr + 8);
    if (dn.is_ok() && dn.value() != 0) {
        (void)m.write_ansi_string(static_cast<GuestAddr>(dn.value()), ansi_path);
        VITREO_TRACE(
            "[shell32] SHBrowseForFolderA: wrote path to pszDisplayName 0x%x: '%s'\n",
            dn.value(), ansi_path.c_str());
    }

    auto lp = m.memory().read_u32(bi_ptr + 24);
    if (lp.is_err() || lp.value() == 0) {
        return;
    }
    const GuestAddr lparam = static_cast<GuestAddr>(lp.value());
    const bool at0 = guest_looks_like_ansi_path(m, lparam);
    const bool at4 = guest_looks_like_ansi_path(m, lparam + 4);
    GuestAddr dest = 0;
    const char* kind = nullptr;
    if (at4 && !at0) {
        dest = lparam + 4;
        kind = "lParam+4 (path after header)";
    } else if (at0 && !at4) {
        dest = lparam;
        kind = "lParam (path at buffer start)";
    } else if (at0) {
        // Both empty or both path-like: only write at +0 when it already
        // holds path text. Leave an empty or HWND-zero header alone so the
        // caller falls back to pszDisplayName.
        auto b0 = m.read_u8(lparam);
        if (b0.is_ok() && b0.value() != 0) {
            dest = lparam;
            kind = "lParam (path buffer)";
        }
    }
    if (dest != 0) {
        (void)m.write_ansi_string(dest, ansi_path);
        VITREO_TRACE(
            "[shell32] SHBrowseForFolderA: wrote path to %s 0x%x: '%s'\n",
            kind, static_cast<u32>(dest), ansi_path.c_str());
    }
}

void write_browse_folder_path_wide(Marshaler& m, GuestAddr bi_ptr,
                                   const std::string& utf8_path) {
    auto dn = m.memory().read_u32(bi_ptr + 8);
    if (dn.is_ok() && dn.value() != 0) {
        (void)m.write_wide_string(static_cast<GuestAddr>(dn.value()), utf8_path);
        VITREO_TRACE(
            "[shell32] SHBrowseForFolderW: wrote path to pszDisplayName 0x%x: '%s'\n",
            dn.value(), utf8_path.c_str());
    }

    auto lp = m.memory().read_u32(bi_ptr + 24);
    if (lp.is_err() || lp.value() == 0) {
        return;
    }
    const GuestAddr lparam = static_cast<GuestAddr>(lp.value());
    const bool at0 = guest_looks_like_wide_path(m, lparam);
    const bool at4 = guest_looks_like_wide_path(m, lparam + 4);
    GuestAddr dest = 0;
    const char* kind = nullptr;
    if (at4 && !at0) {
        dest = lparam + 4;
        kind = "lParam+4 (path after header)";
    } else if (at0 && !at4) {
        dest = lparam;
        kind = "lParam (path at buffer start)";
    } else if (at0) {
        auto w0 = m.memory().read_u16(lparam);
        if (w0.is_ok() && w0.value() != 0) {
            dest = lparam;
            kind = "lParam (path buffer)";
        }
    }
    if (dest != 0) {
        (void)m.write_wide_string(dest, utf8_path);
        VITREO_TRACE(
            "[shell32] SHBrowseForFolderW: wrote path to %s 0x%x: '%s'\n",
            kind, static_cast<u32>(dest), utf8_path.c_str());
    }
}

} // namespace

DllExports create_shell_path_thunks(ShellPathManager& mgr) {
    DllExports exports;
    exports.entries.reserve(12);

    // BOOL InitNetworkAddressControl(void)
    //
    // Windows registers WC_NETADDRESS ("msctls_netaddress") so callers can
    // subsequently create the control. USER32 recognizes that named class
    // and delegates it to Vitreo's edit-like native control path.
    exports.entries.push_back(ExportEntry{
        "InitNetworkAddressControl", 0,
        [](Marshaler& m) {
            VITREO_TRACE(
                "[shell32] InitNetworkAddressControl: "
                "msctls_netaddress available -> TRUE\n");
            m.set_return(1);
        },
        0
    });

    exports.entries.push_back(ExportEntry{
        "SHGetFolderPathW", 506,
        [&mgr](Marshaler& m) {
            GuestAddr out = m.arg_ptr(4);
            if (out == 0) { m.set_return(0x80070057u /* E_INVALIDARG */); return; }
            (void)m.write_wide_string(out, "");
            auto folder_id = static_cast<i32>(m.arg(1));
            std::wstring wpath;
            // Verifies existence unless CSIDL_FLAG_DONT_VERIFY
            // (shell_paths_probe csidl 0x3a / csidl_oem_dontverify).
            u32 result = mgr.get_folder_path(folder_id, wpath, true, m.cpu_is_64bit());
            if (result != hr::S_OK) {
                VITREO_TRACE(
                    "[shell32] SHGetFolderPathW csidl=0x%08x -> "
                    "HRESULT=0x%08x\n",
                    static_cast<u32>(folder_id), result);
                m.set_return(static_cast<u64>(result));
                return;
            }
            std::string narrow(wpath.begin(), wpath.end());
            (void)m.write_wide_string(out, narrow);
            VITREO_TRACE(
                "[shell32] SHGetFolderPathW csidl=0x%08x -> \"%s\"\n",
                static_cast<u32>(folder_id), narrow.c_str());
            m.set_return(static_cast<u64>(hr::S_OK));
        },
        20
    });
    exports.entries.push_back(ExportEntry{
        "SHGetFolderPathA", 0,
        [&mgr](Marshaler& m) {
            GuestAddr out = m.arg_ptr(4);
            if (out == 0) { m.set_return(0x80070057u /* E_INVALIDARG */); return; }
            (void)m.write_ansi_string(out, "");
            auto folder_id = static_cast<i32>(m.arg(1));
            std::wstring wpath;
            u32 result = mgr.get_folder_path(folder_id, wpath, true, m.cpu_is_64bit());
            if (result != hr::S_OK) {
                m.set_return(static_cast<u64>(result));
                return;
            }
            std::string narrow(wpath.begin(), wpath.end());
            (void)m.write_ansi_string(out, narrow);
            m.set_return(static_cast<u64>(hr::S_OK));
        },
        20
    });
    exports.entries.push_back(ExportEntry{
        "SHGetKnownFolderPath", 515,
        [&mgr](Marshaler& m) {
            // HRESULT SHGetKnownFolderPath(REFKNOWNFOLDERID rfid, DWORD dwFlags,
            //   HANDLE hToken, PWSTR *ppszPath)
            // Folders and results come from the generated Windows table
            // (known_folder_table.inc); hToken selects a user, and the
            // capsule has exactly one, so NULL and its token agree.
            const GuestAddr rfid_ptr = m.arg_ptr(0);
            const u32 flags = static_cast<u32>(m.arg(1));
            const GuestAddr ppszPath = m.arg_ptr(3);
            if (ppszPath == 0) {
                m.set_return(static_cast<u64>(hr::E_INVALIDARG));
                return;
            }
            // Learn: the caller frees *ppszPath "whether SHGetKnownFolderPath
            // succeeds or not"; Windows stores NULL on failure
            // (shell_paths_probe outNull=1).
            (void)m.write_ptr(ppszPath, 0);
            u8 guid[16]{};
            if (rfid_ptr == 0 || m.memory().read_bytes(rfid_ptr, guid, 16).is_err()) {
                m.set_return(static_cast<u64>(hr::E_INVALIDARG));
                return;
            }
            std::wstring wpath;
            const u32 result =
                mgr.get_known_folder_path_by_guid(guid, flags, m.cpu_is_64bit(), wpath);
            if (result != hr::S_OK) {
                m.set_return(static_cast<u64>(result));
                return;
            }
            // The string comes from the COM task allocator (CoTaskMemFree).
            const GuestAddr str_addr = mgr.task_memory().allocate((wpath.size() + 1) * 2);
            if (str_addr == 0) {
                m.set_return(static_cast<u64>(0x8007000Eu)); // E_OUTOFMEMORY
                return;
            }
            for (usize i = 0; i < wpath.size(); ++i) {
                (void)m.write_u16(str_addr + i * 2, static_cast<u16>(wpath[i]));
            }
            (void)m.write_u16(str_addr + wpath.size() * 2, 0);
            (void)m.write_ptr(ppszPath, str_addr);
            m.set_return(static_cast<u64>(hr::S_OK));
        },
        16
    });
    exports.entries.push_back(ExportEntry{
        "CommandLineToArgvW", 273,
        [&mgr](Marshaler& m) {
            // LPWSTR* CommandLineToArgvW(LPCWSTR lpCmdLine, int* pNumArgs)
            GuestAddr cmd_ptr = m.arg_ptr(0);
            GuestAddr num_args_ptr = m.arg_ptr(1);
            GuestAddr result = mgr.command_line_to_argv_guest(m, cmd_ptr, num_args_ptr);
            m.set_return(static_cast<u64>(result));
        },
        8
    });
    // int SHCreateDirectoryExW(HWND, PCWSTR pszPath, const SECURITY_ATTRIBUTES*)
    // The return value doubles as the last error (create_directory_ex).
    exports.entries.push_back(ExportEntry{
        "SHCreateDirectoryExW", 340,
        [&mgr](Marshaler& m) {
            const GuestAddr path_addr = m.arg_ptr(1);
            const std::string path = path_addr != 0
                ? m.read_wide_string(path_addr, 32768).value_or("") : std::string{};
            m.set_return(mgr.create_directory_ex(path));
        },
        12
    });
    exports.entries.push_back(ExportEntry{
        "SHCreateDirectoryExA", 0,
        [&mgr](Marshaler& m) {
            const GuestAddr path_addr = m.arg_ptr(1);
            std::string path;
            if (path_addr != 0) {
                // ANSI (CP_ACP) path, not UTF-8.
                const std::string bytes =
                    m.read_ansi_string(path_addr, 32768).value_or("");
                std::vector<u16> wide(bytes.size() + 1, 0);
                const auto converted = codepage::multi_byte_to_wide(
                    codepage::kCpAcp, 0, reinterpret_cast<const u8*>(bytes.data()),
                    bytes.size(), wide.data(), wide.size());
                path = utf16_to_utf8(wide.data(), static_cast<int>(converted.count));
            }
            m.set_return(mgr.create_directory_ex(path));
        },
        12
    });
    // int SHCreateDirectory(HWND, PCWSTR) — shell32 ordinal 165: the same
    // operation as SHCreateDirectoryExW(hwnd, path, NULL) (Learn; measured
    // identical codes in s7_shell_probe.expected).
    exports.entries.push_back(ExportEntry{
        "SHCreateDirectory", 165,
        [&mgr](Marshaler& m) {
            const GuestAddr path_addr = m.arg_ptr(1);
            const std::string path = path_addr != 0
                ? m.read_wide_string(path_addr, 32768).value_or("") : std::string{};
            m.set_return(mgr.create_directory_ex(path));
        },
        8
    });
    // BROWSEINFOW structure (32-bit):
    //   HWND hwndOwner;         // 0
    //   LPCITEMIDLIST pidlRoot; // 4
    //   LPWSTR pszDisplayName;  // 8
    //   LPCWSTR lpszTitle;      // 12
    //   UINT ulFlags;           // 16
    //   BFFCALLBACK lpfn;       // 20
    //   LPARAM lParam;          // 24
    //   int iImage;             // 28
    exports.entries.push_back(ExportEntry{
        "SHBrowseForFolderW", 328,
        [&mgr](Marshaler& m) {
            GuestAddr bi_ptr = m.arg_ptr(0);
            if (bi_ptr == 0) {
                m.set_return(0);
                return;
            }

            GuestAddr title_ptr = 0;
            auto title_addr_result = m.memory().read_u32(bi_ptr + 12);
            if (title_addr_result.is_ok()) {
                title_ptr = title_addr_result.value();
            }

            std::wstring title = L"Select Folder";
            if (title_ptr != 0) {
                auto title_result = m.read_wide_string(title_ptr, 256);
                if (title_result.is_ok()) {
                    const std::string& narrow = title_result.value();
                    title.clear();
                    for (char c : narrow) {
                        title.push_back(static_cast<wchar_t>(static_cast<unsigned char>(c)));
                    }
                }
            }

            GuestAddr pidl = mgr.browse_for_folder(title);

            if (pidl != 0) {
                std::wstring selected_path;
                if (mgr.get_path_from_pidl(pidl, selected_path)) {
                    write_browse_folder_path_wide(
                        m, bi_ptr, wstring_to_utf8(selected_path));
                }
            }

            VITREO_TRACE( "[shell32] SHBrowseForFolderW: pidl=0x%llx, setting return\n",
                         (unsigned long long)pidl);
            m.set_return(static_cast<u64>(pidl));
        },
        4
    });

    // SHBrowseForFolderA — same BROWSEINFO layout on 32-bit; ANSI title / path
    // buffers. Callers GetProcAddress the A export.
    exports.entries.push_back(ExportEntry{
        "SHBrowseForFolderA", 0,
        [&mgr](Marshaler& m) {
            GuestAddr bi_ptr = m.arg_ptr(0);
            if (bi_ptr == 0) {
                m.set_return(0);
                return;
            }

            GuestAddr title_ptr = 0;
            auto title_addr_result = m.memory().read_u32(bi_ptr + 12);
            if (title_addr_result.is_ok()) {
                title_ptr = title_addr_result.value();
            }

            std::wstring title = L"Select Folder";
            if (title_ptr != 0) {
                auto title_result = m.read_ansi_string(title_ptr, 256);
                if (title_result.is_ok()) {
                    const std::string& narrow = title_result.value();
                    title.clear();
                    for (unsigned char c : narrow) {
                        title.push_back(static_cast<wchar_t>(c));
                    }
                }
            }

            GuestAddr pidl = mgr.browse_for_folder(title);

            if (pidl != 0) {
                std::wstring selected_path;
                if (mgr.get_path_from_pidl(pidl, selected_path)) {
                    std::string ansi_path(selected_path.begin(), selected_path.end());
                    write_browse_folder_path_ansi(m, bi_ptr, ansi_path);
                }
            }

            VITREO_TRACE(
                "[shell32] SHBrowseForFolderA: pidl=0x%llx\n",
                static_cast<unsigned long long>(pidl));
            m.set_return(static_cast<u64>(pidl));
        },
        4
    });
    // SHGetPathFromIDListW (2 args: pidl, pszPath; stdcall_bytes=8)
    exports.entries.push_back(ExportEntry{
        "SHGetPathFromIDListW", 530,
        [&mgr](Marshaler& m) {
            GuestAddr pidl = m.arg_ptr(0);
            GuestAddr path_buf = m.arg_ptr(1);

            VITREO_TRACE( "[shell32] SHGetPathFromIDListW: pidl=%llx path_buf=%llx\n",
                         (unsigned long long)pidl, (unsigned long long)path_buf);

            if (pidl == 0 || path_buf == 0) {
                m.set_return(0);  // FALSE
                return;
            }

            std::wstring path;
            if (!mgr.get_path_from_pidl(pidl, path)) {
                VITREO_TRACE( "[shell32] SHGetPathFromIDListW: PIDL not found\n");
                m.set_return(0);  // FALSE
                return;
            }

            // Convert wide path to UTF-8, then write as wide string
            std::string utf8_path = wstring_to_utf8(path);
            VITREO_TRACE( "[shell32] SHGetPathFromIDListW: path='%s'\n", utf8_path.c_str());
            
            (void)m.write_wide_string(path_buf, utf8_path);

            m.set_return(1);  // TRUE
        },
        8
    });
    exports.entries.push_back(ExportEntry{
        "SHGetPathFromIDListA", 0,
        [&mgr](Marshaler& m) {
            GuestAddr pidl = m.arg_ptr(0);
            GuestAddr path_buf = m.arg_ptr(1);
            std::wstring path;
            if (pidl == 0 || path_buf == 0 || !mgr.get_path_from_pidl(pidl, path)) {
                VITREO_TRACE(
                    "[shell32] SHGetPathFromIDListA: pidl=0x%llx path_buf=0x%llx -> FALSE\n",
                    static_cast<unsigned long long>(pidl),
                    static_cast<unsigned long long>(path_buf));
                m.set_return(0);
                return;
            }
            std::string ansi(path.begin(), path.end());
            (void)m.write_ansi_string(path_buf, ansi);
            VITREO_TRACE(
                "[shell32] SHGetPathFromIDListA: pidl=0x%llx -> '%s'\n",
                static_cast<unsigned long long>(pidl), ansi.c_str());
            m.set_return(1);
        },
        8
    });

    // void SHFree(void* pv) — shell32 ordinal 195 (dumpbin shell32.dll).
    exports.entries.push_back(ExportEntry{
        "SHFree", 195,
        [&mgr](Marshaler& m) {
            mgr.sh_free(m.arg_ptr(0));
        },
        4
    });

    // shell32 does not export CoTaskMemFree (Windows 11 export table);
    // PIDLs are ole32 task memory, freed by ole32!CoTaskMemFree.
    exports.entries.push_back(ExportEntry{
        "SHFileOperationW", 488,
        [&mgr](Marshaler& m) {
            GuestAddr file_op_ptr = m.arg_ptr(0);
            m.set_return(static_cast<u64>(mgr.file_operation(m, file_op_ptr)));
        },
        4
    });

    // SHGetFileInfoW (5 args, stdcall_bytes=20)
    exports.entries.push_back(ExportEntry{
        "SHGetFileInfoW", 500,
        [&mgr](Marshaler& m) {
            m.set_return(mgr.get_file_info(m));
        },
        20
    });

    exports.entries.push_back(ExportEntry{
        "SHGetFileInfoA", 0,
        [&mgr](Marshaler& m) { m.set_return(mgr.get_file_info_ansi(m)); },
        20
    });

    // SHGetSpecialFolderLocation (3 args: HWND, int csidl, PIDLIST_ABSOLUTE*, stdcall_bytes=12)
    exports.entries.push_back(ExportEntry{
        "SHGetSpecialFolderLocation", 535,
        [&mgr](Marshaler& m) {
            // Win32 rejects a NULL out-pidl pointer with E_INVALIDARG.
            if (m.arg(2) == 0) { m.set_return(0x80070057u /* E_INVALIDARG */); return; }
            auto csidl = static_cast<i32>(m.arg32(1));
            GuestAddr pidl = 0;
            // The PIDL out-parameter is pointer-sized, and a 32-bit caller
            // sees the WOW64 folder view.
            u32 result = mgr.get_special_folder_location(csidl, pidl, m.cpu_is_64bit());
            (void)m.write_ptr(m.arg_ptr(2), result == hr::S_OK ? pidl : 0);
            m.set_return(static_cast<u64>(result));
        },
        12
    });

    // ShellExecuteW (6 args, stdcall_bytes=24)
    // HINSTANCE ShellExecuteW(HWND hwnd, LPCWSTR lpOperation, LPCWSTR lpFile,
    //     LPCWSTR lpParameters, LPCWSTR lpDirectory, INT nShowCmd)
    exports.entries.push_back(ExportEntry{
        "ShellExecuteW", 578,
        [&mgr](Marshaler& m) {
            (void)m.arg(0); // hwnd
            GuestAddr op_ptr    = m.arg_ptr(1);
            GuestAddr file_ptr  = m.arg_ptr(2);
            GuestAddr param_ptr = m.arg_ptr(3);
            GuestAddr dir_ptr   = m.arg_ptr(4);
            i32 show_cmd        = static_cast<i32>(m.arg(5));

            std::string operation, file, params, directory;
            if (op_ptr)    { auto r = m.read_wide_string(op_ptr);    if (r.is_ok()) operation = r.value(); }
            if (file_ptr)  { auto r = m.read_wide_string(file_ptr);  if (r.is_ok()) file      = r.value(); }
            if (param_ptr) { auto r = m.read_wide_string(param_ptr); if (r.is_ok()) params    = r.value(); }
            if (dir_ptr)   { auto r = m.read_wide_string(dir_ptr);   if (r.is_ok()) directory  = r.value(); }

            u64 result = mgr.shell_execute(operation, file, params, directory,
                                           show_cmd);
            m.set_return(result);
        },
        24
    });

    // ShellExecuteA — ANSI variant. Reads strings as ANSI and delegates
    // to the same shell_execute() helper.
    exports.entries.push_back(ExportEntry{
        "ShellExecuteA", 0,
        [&mgr](Marshaler& m) {
            (void)m.arg(0); // hwnd
            GuestAddr op_ptr    = m.arg_ptr(1);
            GuestAddr file_ptr  = m.arg_ptr(2);
            GuestAddr param_ptr = m.arg_ptr(3);
            GuestAddr dir_ptr   = m.arg_ptr(4);
            i32 show_cmd        = static_cast<i32>(m.arg(5));

            std::string operation, file, params, directory;
            if (op_ptr)    { auto r = m.read_ansi_string(op_ptr);    if (r.is_ok()) operation = r.value(); }
            if (file_ptr)  { auto r = m.read_ansi_string(file_ptr);  if (r.is_ok()) file      = r.value(); }
            if (param_ptr) { auto r = m.read_ansi_string(param_ptr); if (r.is_ok()) params    = r.value(); }
            if (dir_ptr)   { auto r = m.read_ansi_string(dir_ptr);   if (r.is_ok()) directory = r.value(); }

            VITREO_TRACE("[shell32] ShellExecuteA: op='%s' file='%s' params='%s' dir='%s' show=%d\n",
                         operation.c_str(), file.c_str(), params.c_str(),
                         directory.c_str(), show_cmd);

            u64 result = mgr.shell_execute(operation, file, params, directory,
                                           show_cmd);
            m.set_return(result);
        },
        24
    });

    // FindExecutableA/W (3 args)
    // HINSTANCE FindExecutableA(LPCSTR lpFile, LPCSTR lpDirectory,
    //                           LPSTR lpResult)
    //
    // Returns a value > 32 on success and writes the associated executable's
    // path to lpResult (a MAX_PATH buffer). Values <= 32 are SE_ERR_* codes.
    // Vitreo resolves a file that is itself executable to its own path; there
    // is no document-association database in the capsule registry yet, so any
    // other existing file honestly reports SE_ERR_NOASSOC rather than
    // inventing a handler.
    for (const bool wide : {false, true}) {
    exports.entries.push_back(ExportEntry{
        wide ? "FindExecutableW" : "FindExecutableA", 0,
        [&mgr, wide](Marshaler& m) {
            constexpr u64 SE_ERR_FNF = 2;        // file not found
            constexpr u64 SE_ERR_NOASSOC = 31;   // no association
            constexpr u64 SE_SUCCESS = 42;       // any value > 32

            const GuestAddr file_ptr = m.arg_ptr(0);
            const GuestAddr dir_ptr = m.arg_ptr(1);
            const GuestAddr result_ptr = m.arg_ptr(2);

            std::string file, directory;
            if (file_ptr) {
                auto r = wide ? m.read_wide_string(file_ptr) : m.read_ansi_string(file_ptr);
                if (r.is_ok()) file = r.value();
            }
            if (dir_ptr) {
                auto r = wide ? m.read_wide_string(dir_ptr) : m.read_ansi_string(dir_ptr);
                if (r.is_ok()) directory = r.value();
            }

            const auto write_result = [&](std::string_view value) {
                return wide ? m.write_wide_string(result_ptr, value)
                            : m.write_ansi_string(result_ptr, value);
            };
            if (result_ptr == 0 || write_result("").is_err()) {
                m.set_return(5); // SE_ERR_ACCESSDENIED
                return;
            }

            if (file.empty()) {
                VITREO_TRACE("[shell32] FindExecutableA: empty file -> SE_ERR_FNF\n");
                m.set_return(SE_ERR_FNF);
                return;
            }

            // A relative name is resolved against lpDirectory when supplied.
            std::string path = file;
            const bool absolute = file.size() >= 2 && file[1] == ':';
            if (!absolute && !directory.empty()) {
                path = directory;
                if (path.back() != '\\' && path.back() != '/') path += '\\';
                path += file;
            }

            if (mgr.vfs().stat(path).is_err()) {
                VITREO_TRACE("[shell32] FindExecutableA: '%s' not found\n",
                             path.c_str());
                m.set_return(SE_ERR_FNF);
                return;
            }

            std::string lowered = path;
            std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                           [](unsigned char c) {
                               return static_cast<char>(std::tolower(c));
                           });
            const bool is_executable =
                lowered.size() >= 4 &&
                (lowered.compare(lowered.size() - 4, 4, ".exe") == 0 ||
                 lowered.compare(lowered.size() - 4, 4, ".com") == 0 ||
                 lowered.compare(lowered.size() - 4, 4, ".bat") == 0 ||
                 lowered.compare(lowered.size() - 4, 4, ".cmd") == 0 ||
                 lowered.compare(lowered.size() - 4, 4, ".pif") == 0);

            if (!is_executable) {
                VITREO_TRACE("[shell32] FindExecutableA: '%s' has no capsule "
                             "association -> SE_ERR_NOASSOC\n", path.c_str());
                m.set_return(SE_ERR_NOASSOC);
                return;
            }

            if (write_result(path).is_err()) {
                m.set_return(SE_ERR_FNF);
                return;
            }
            VITREO_TRACE("[shell32] FindExecutableA: '%s' -> '%s'\n",
                         file.c_str(), path.c_str());
            m.set_return(SE_SUCCESS);
        },
        12
    });
    }

    // ShellExecuteExW (1 arg, stdcall_bytes=4)
    // BOOL ShellExecuteExW(SHELLEXECUTEINFOW* pExecInfo)
    // BOOL ShellExecuteEx{W,A}(SHELLEXECUTEINFO* pExecInfo): one
    // implementation for both; the A form used to hard-code the x86 offsets
    // (lpVerb read as lpFile on x64) and wrote a 32-bit hInstApp.
    const auto shell_execute_ex = [&mgr](Marshaler& m, bool wide) {
            GuestAddr sei = m.arg_ptr(0);
            if (sei == 0) {
                m.set_return(0);
                return;
            }

            // SHELLEXECUTEINFOW. Every pointer field is pointer-sized and the
            // struct is pointer-aligned, so the offsets differ per bitness:
            //
            //          x86                     x64
            //   +0  cbSize(4)             +0  cbSize(4)
            //   +4  fMask(4)              +4  fMask(4)
            //   +8  hwnd(4)               +8  hwnd(8)
            //  +12  lpVerb(4)            +16  lpVerb(8)
            //  +16  lpFile(4)            +24  lpFile(8)
            //  +20  lpParameters(4)      +32  lpParameters(8)
            //  +24  lpDirectory(4)       +40  lpDirectory(8)
            //  +28  nShow(4)             +48  nShow(4)
            //  +32  hInstApp(4)          +56  hInstApp(8)
            //  +56  hProcess(4)         +104  hProcess(8)
            //
            // This read the x86 offsets unconditionally. On an x64 guest that
            // shifts every field by one slot: lpFile lands on lpVerb, so
            // Cerious AASM's "Open Directory" arrived as
            // op='' file='explore' show=22336 -- the VERB in the FILE slot and
            // garbage for nShow -- and the call could not possibly do anything
            // but fail. MEASURED 2026-09-09 from the trace.
            const bool w64 = m.cpu_is_64bit();
            const u32 off_verb   = w64 ? 16u : 12u;
            const u32 off_file   = w64 ? 24u : 16u;
            const u32 off_params = w64 ? 32u : 20u;
            const u32 off_dir    = w64 ? 40u : 24u;
            const u32 off_nshow  = w64 ? 48u : 28u;
            const u32 off_hinst  = w64 ? 56u : 32u;
            const u32 off_hproc  = w64 ? 104u : 56u;

            auto fmask_r = m.read_u32(sei + 4);
            u32 fmask = fmask_r.is_ok() ? fmask_r.value() : 0;

            auto verb_ptr_r  = m.read_ptr(sei + off_verb);
            auto file_ptr_r  = m.read_ptr(sei + off_file);
            auto param_ptr_r = m.read_ptr(sei + off_params);
            auto dir_ptr_r   = m.read_ptr(sei + off_dir);
            auto nshow_r     = m.read_u32(sei + off_nshow);

            GuestAddr verb_ptr  = verb_ptr_r.is_ok()  ? verb_ptr_r.value()  : 0;
            GuestAddr file_ptr  = file_ptr_r.is_ok()  ? file_ptr_r.value()  : 0;
            GuestAddr param_ptr = param_ptr_r.is_ok() ? param_ptr_r.value() : 0;
            GuestAddr dir_ptr   = dir_ptr_r.is_ok()   ? dir_ptr_r.value()   : 0;
            i32 show_cmd        = nshow_r.is_ok() ? static_cast<i32>(nshow_r.value()) : 1;

            // SHELLEXECUTEINFOA has the identical layout; its strings are
            // CP_ACP, not UTF-8.
            const auto read_text = [&m, wide](GuestAddr at) -> std::string {
                if (at == 0) return {};
                if (wide) return m.read_wide_string(at).value_or("");
                const std::string bytes = m.read_ansi_string(at).value_or("");
                std::vector<u16> units(bytes.size() + 1, 0);
                const auto r = codepage::multi_byte_to_wide(
                    codepage::kCpAcp, 0, reinterpret_cast<const u8*>(bytes.data()),
                    bytes.size(), units.data(), units.size());
                return utf16_to_utf8(units.data(), static_cast<int>(r.count));
            };
            std::string operation = read_text(verb_ptr);
            std::string file = read_text(file_ptr);
            std::string params = read_text(param_ptr);
            std::string directory = read_text(dir_ptr);

            VITREO_TRACE("[shell32] ShellExecuteEx%c: op='%s' file='%s' params='%s' dir='%s' show=%d mask=0x%x\n",
                         wide ? 'W' : 'A', operation.c_str(), file.c_str(),
                         params.c_str(), directory.c_str(), show_cmd, fmask);

            // fMask (Learn SHELLEXECUTEINFOW):
            constexpr u32 SEE_MASK_CLASSNAME = 0x00000001;
            constexpr u32 SEE_MASK_CLASSKEY = 0x00000003;
            constexpr u32 SEE_MASK_IDLIST = 0x00000004;
            constexpr u32 SEE_MASK_NOCLOSEPROCESS = 0x00000040;
            constexpr u32 SEE_MASK_DOENVSUBST = 0x00000200;
            constexpr u32 SEE_MASK_WAITFORINPUTIDLE = 0x02000000;
            const u32 off_idlist = w64 ? 64u : 36u;
            const u32 off_class = w64 ? 72u : 40u;
            if ((fmask & SEE_MASK_DOENVSUBST) != 0) {
                file = mgr.expand_environment(file);
                directory = mgr.expand_environment(directory);
            }
            if ((fmask & SEE_MASK_IDLIST) != 0 && file.empty()) {
                const GuestAddr pidl = static_cast<GuestAddr>(
                    m.read_ptr(sei + off_idlist).value_or(0));
                std::string pidl_file;
                if (pidl != 0 && mgr.path_for_pidl(pidl, pidl_file)) file = pidl_file;
            }
            std::string class_name;
            if ((fmask & SEE_MASK_CLASSKEY) == SEE_MASK_CLASSNAME) {
                const GuestAddr cls = static_cast<GuestAddr>(
                    m.read_ptr(sei + off_class).value_or(0));
                if (cls != 0) class_name = read_text(cls);
            } else if ((fmask & SEE_MASK_CLASSKEY) == SEE_MASK_CLASSKEY) {
                std::fprintf(stderr,
                    "[vitreo] UNIMPLEMENTED shell32!ShellExecuteExW: "
                    "SEE_MASK_CLASSKEY (hkeyClass) ignored\n");
            }
            if ((fmask & SEE_MASK_WAITFORINPUTIDLE) != 0) {
                static bool warned = false;
                if (!warned) {
                    warned = true;
                    std::fprintf(stderr,
                        "[vitreo] UNIMPLEMENTED shell32!ShellExecuteExW: "
                        "SEE_MASK_WAITFORINPUTIDLE (child input-idle not observable)\n");
                }
            }

            u32 process_handle = 0;
            u64 result = mgr.shell_execute(operation, file, params, directory,
                                           show_cmd, &process_handle, class_name);

            // hInstApp and hProcess are pointer-sized HANDLEs, so they move
            // with bitness like every other field above.
            (void)m.write_ptr(sei + off_hinst, result);

            // Learn: hProcess "is always NULL unless SEE_MASK_NOCLOSEPROCESS"
            // (shell_paths_probe see_missing hProcNull=1); a handle the
            // caller did not ask for is closed rather than leaked.
            if ((fmask & SEE_MASK_NOCLOSEPROCESS) != 0) {
                (void)m.write_ptr(sei + off_hproc, static_cast<u64>(process_handle));
            } else {
                (void)m.write_ptr(sei + off_hproc, 0);
                mgr.close_guest_handle(process_handle);
            }

            m.set_return(result > 32 ? 1u : 0u);
    };
    exports.entries.push_back(ExportEntry{
        "ShellExecuteExW", 577,
        [shell_execute_ex](Marshaler& m) { shell_execute_ex(m, true); },
        4
    });
    exports.entries.push_back(ExportEntry{
        "ShellExecuteExA", 576,
        [shell_execute_ex](Marshaler& m) { shell_execute_ex(m, false); },
        4
    });

    // IsUserAnAdmin — ordinal 680, 0 args, stdcall_bytes=0
    exports.entries.push_back(ExportEntry{
        "IsUserAnAdmin", 680,
        [&mgr](Marshaler& m) {
            m.set_return(static_cast<u64>(mgr.is_user_an_admin()));
        },
        0
    });

    // ShellAboutW — 4 args (hWnd, szApp, szOtherStuff, hIcon), stdcall_bytes=16
    exports.entries.push_back(ExportEntry{
        "ShellAboutW", 0,
        [](Marshaler& m) {
            [[maybe_unused]] auto hWnd       = m.arg(0);
            [[maybe_unused]] auto szApp      = m.arg(1);
            [[maybe_unused]] auto szOther    = m.arg(2);
            [[maybe_unused]] auto hIcon      = m.arg(3);
            VITREO_TRACE( "[shell32] ShellAboutW(hWnd=0x%08X) => TRUE (stub)\n",
                         static_cast<unsigned>(hWnd));
            m.set_return(1); // TRUE
        },
        16
    });

    // SHGetSpecialFolderPathW — 4 args, stdcall_bytes=16
    exports.entries.push_back(ExportEntry{
        "SHGetSpecialFolderPathW", 0,
        [&mgr](Marshaler& m) {
            // BOOL SHGetSpecialFolderPathW(HWND hwndOwner, LPWSTR lpszPath, int csidl, BOOL fCreate)
            [[maybe_unused]] auto hwnd = m.arg(0);
            auto lpszPath = m.arg_ptr(1);
            auto csidl_val = static_cast<i32>(m.arg32(2));
            // Learn: "If this value is nonzero, the folder is created";
            // otherwise a missing folder fails. 32-bit callers get the WOW64
            // view (Program Files (x86) etc.).
            constexpr i32 CSIDL_FLAG_CREATE = 0x8000;
            if (m.arg32(3) != 0) csidl_val |= CSIDL_FLAG_CREATE;

            std::wstring wpath;
            u32 hr = mgr.get_folder_path(csidl_val, wpath, true, m.cpu_is_64bit());
            if (hr == hr::S_OK && lpszPath != 0) {
                // Convert to narrow string and use write_wide_string
                std::string narrow(wpath.begin(), wpath.end());
                (void)m.write_wide_string(lpszPath, narrow);
                m.set_return(1u); // TRUE
            } else {
                m.set_return(0u); // FALSE
            }
        },
        16
    });

    // int SHFileOperationA(LPSHFILEOPSTRUCTA lpFileOp)
    //
    //   typedef struct _SHFILEOPSTRUCTA {   // x86 layout, 1-byte packed
    //     HWND   hwnd;              // +0
    //     UINT   wFunc;             // +4
    //     PCZZSTR pFrom;            // +8   double-NUL terminated list
    //     PCZZSTR pTo;              // +12
    //     FILEOP_FLAGS fFlags;      // +16  (WORD)
    //     BOOL   fAnyOperationsAborted; // +18
    //     LPVOID hNameMappings;     // +22
    //     PCSTR  lpszProgressTitle; // +26
    //   } SHFILEOPSTRUCTA;          // 30 bytes, /Zp1 — the WORD at +16 makes
    //                               // every following field unaligned.
    //
    // Only the operations Vitreo can carry out through the capsule VFS are
    // performed; anything else reports a real error code so a guest never
    // believes a copy happened that did not.
    exports.entries.push_back(ExportEntry{
        "SHFileOperationA", 0,
        [&mgr](Marshaler& m) {
            constexpr u32 FO_MOVE   = 0x0001;
            constexpr u32 FO_COPY   = 0x0002;
            constexpr u32 FO_DELETE = 0x0003;
            constexpr u32 FO_RENAME = 0x0004;
            // Shell error codes (DE_*), which are not Win32 error codes.
            constexpr u32 DE_INVALIDFILES = 0x7C;
            constexpr u32 DE_OPCANCELLED  = 0x75;
            constexpr u32 ERROR_GEN_FAILURE_SHELL = 0x1F;

            const GuestAddr op = m.arg_ptr(0);
            if (op == 0) {
                m.set_return(DE_INVALIDFILES);
                return;
            }

            const bool is_64bit = m.cpu_is_64bit();
            // On x64 the pointers widen to 8 bytes but the struct stays
            // 1-byte packed, so the offsets shift rather than realign.
            const GuestAddr func_off  = is_64bit ? 8 : 4;
            const GuestAddr from_off  = is_64bit ? 16 : 8;
            const GuestAddr to_off    = is_64bit ? 24 : 12;
            const GuestAddr flags_off = is_64bit ? 32 : 16;
            const GuestAddr aborted_off = flags_off + 2;

            auto func_res = m.read_u32(op + func_off);
            auto from_res = m.read_ptr(op + from_off);
            auto to_res   = m.read_ptr(op + to_off);
            auto flags_res = m.read_u16(op + flags_off);
            if (func_res.is_err() || from_res.is_err()) {
                m.set_return(DE_INVALIDFILES);
                return;
            }
            const u32 func = func_res.value();
            const u16 flags = flags_res.is_ok() ? flags_res.value() : 0;

            // pFrom/pTo are double-NUL terminated lists of NUL-separated
            // paths, not plain strings.
            const auto read_path_list =
                [&m](GuestAddr list) -> std::vector<std::string> {
                std::vector<std::string> paths;
                if (list == 0) return paths;
                GuestAddr cursor = list;
                for (usize guard = 0; guard < 4096; ++guard) {
                    auto entry = m.read_ansi_string(cursor);
                    if (entry.is_err() || entry.value().empty()) break;
                    cursor += entry.value().size() + 1;
                    paths.push_back(entry.value());
                }
                return paths;
            };

            const std::vector<std::string> source_specs =
                read_path_list(static_cast<GuestAddr>(from_res.value()));
            const std::vector<std::string> destinations =
                read_path_list(to_res.is_ok()
                                   ? static_cast<GuestAddr>(to_res.value())
                                   : 0);

            if (source_specs.empty()) {
                m.set_return(DE_INVALIDFILES);
                return;
            }
            // A source spec may name a set ("dir\\*"), which the shell expands
            // before it does anything. FO_COPY re-expands inside
            // copy_operation, so it keeps the unexpanded specs.
            bool had_wildcard = false;
            const std::vector<std::string> sources =
                mgr.expand_wildcard_sources(source_specs, had_wildcard);
            if (sources.empty()) {
                m.set_return(DE_INVALIDFILES);
                return;
            }

            auto& vfs = mgr.vfs();
            u32 result = 0;
            bool aborted = false;

            switch (func) {
            case FO_DELETE:
                for (const std::string& source : sources) {
                    // FO_DELETE on a directory takes its contents with it.
                    if (!mgr.remove_tree(source)) {
                        std::fprintf(stderr,
                            "[shell32] SHFileOperationA FO_DELETE \"%s\" "
                            "failed\n", source.c_str());
                        result = ERROR_GEN_FAILURE_SHELL;
                        aborted = true;
                    }
                }
                break;

            case FO_RENAME:
            case FO_MOVE: {
                if (destinations.empty()) {
                    m.set_return(DE_INVALIDFILES);
                    return;
                }
                // With one destination and several sources the destination is
                // a target directory; with matching counts it is 1:1. A
                // wildcard source is always a set, so its destination is the
                // directory the set lands in.
                const bool one_to_one =
                    !had_wildcard && destinations.size() == sources.size();
                if (!one_to_one && !mgr.ensure_directory(destinations[0])) {
                    m.set_return(DE_INVALIDFILES);
                    return;
                }
                for (usize i = 0; i < sources.size(); ++i) {
                    std::string target = one_to_one ? destinations[i]
                                                    : destinations[0];
                    if (!one_to_one) {
                        const usize slash = sources[i].find_last_of("\\/");
                        const std::string leaf =
                            slash == std::string::npos
                                ? sources[i] : sources[i].substr(slash + 1);
                        if (!target.empty() && target.back() != '\\') {
                            target.push_back('\\');
                        }
                        target += leaf;
                    }
                    if (vfs.rename(sources[i], target).is_err() &&
                        !(mgr.copy_tree(sources[i], target) &&
                          mgr.remove_tree(sources[i]))) {
                        // rename refuses a cross-filesystem move, and a move
                        // onto an existing directory; the copy+delete fallback
                        // covers both.
                        std::fprintf(stderr,
                            "[shell32] SHFileOperationA FO_MOVE \"%s\" -> "
                            "\"%s\" failed\n",
                            sources[i].c_str(), target.c_str());
                        result = ERROR_GEN_FAILURE_SHELL;
                        aborted = true;
                    }
                }
                break;
            }

            case FO_COPY: {
                if (destinations.empty()) {
                    m.set_return(DE_INVALIDFILES);
                    return;
                }
                result =
                    static_cast<u32>(mgr.copy_operation(source_specs, destinations));
                aborted = result != 0;
                break;
            }

            default:
                std::fprintf(stderr,
                    "[shell32] SHFileOperationA wFunc=0x%x (flags=0x%x) is "
                    "not implemented -> DE_OPCANCELLED\n", func, flags);
                result = DE_OPCANCELLED;
                aborted = true;
                break;
            }

            (void)m.write_u32(op + aborted_off, aborted ? 1u : 0u);
            m.set_return(result);
        },
        4
    });

    // BOOL SHGetSpecialFolderPathA(HWND, LPSTR lpszPath, int csidl, BOOL fCreate)
    exports.entries.push_back(ExportEntry{
        "SHGetSpecialFolderPathA", 0,
        [&mgr](Marshaler& m) {
            [[maybe_unused]] auto hwnd = m.arg(0);
            auto lpszPath = m.arg_ptr(1);
            auto csidl_val = static_cast<i32>(m.arg32(2));
            constexpr i32 CSIDL_FLAG_CREATE = 0x8000;
            if (m.arg32(3) != 0) csidl_val |= CSIDL_FLAG_CREATE;

            if (lpszPath == 0) {
                m.set_return(0u);
                return;
            }
            std::wstring wpath;
            const u32 hr = mgr.get_folder_path(csidl_val, wpath, true, m.cpu_is_64bit());
            if (hr != hr::S_OK) {
                m.set_return(0u);
                return;
            }
            // The guest paths this returns are Windows paths inside the
            // capsule and stay within the ANSI range, so the narrowing here
            // cannot lose a character.
            const std::string narrow(wpath.begin(), wpath.end());
            m.set_return(m.write_ansi_string(lpszPath, narrow).is_ok() ? 1u
                                                                      : 0u);
        },
        16
    });

    // ShellAboutA(hWnd, szApp, szOtherStuff, hIcon) → INT
    exports.entries.push_back(ExportEntry{
        "ShellAboutA", 0,
        [](Marshaler& m) {
            (void)m.arg(0); (void)m.arg(1); (void)m.arg(2); (void)m.arg(3);
            m.set_return(1u); // IDOK — user clicked OK
        },
        16
    });

    // SHChangeNotify(LONG wEventId, UINT uFlags, LPCVOID dwItem1, LPCVOID dwItem2) — void
    // 4 args, stdcall_bytes=16
    exports.entries.push_back(ExportEntry{
        "SHChangeNotify", 0,
        [](Marshaler& m) {
            (void)m.arg(0); (void)m.arg(1); (void)m.arg(2); (void)m.arg(3);
            // void return — no-op
        },
        16
    });

    // void DragFinish(HDROP hDrop): releases the HDROP.
    exports.entries.push_back(ExportEntry{
        "DragFinish", 0,
        [&mgr](Marshaler& m) { mgr.drag_finish(m.arg(0)); },
        4
    });

    // BOOL DragQueryPoint(HDROP hDrop, POINT* ppt)
    // https://learn.microsoft.com/en-us/windows/win32/api/shellapi/nf-shellapi-dragquerypoint
    exports.entries.push_back(ExportEntry{
        "DragQueryPoint", 0,
        [&mgr](Marshaler& m) {
            m.set_return(mgr.drag_query_point(m, m.arg(0), m.arg_ptr(1)));
        },
        8
    });

    // UINT DragQueryFileW(HDROP hDrop, UINT iFile, LPWSTR lpszFile, UINT cch)
    // https://learn.microsoft.com/en-us/windows/win32/api/shellapi/nf-shellapi-dragqueryfilew
    exports.entries.push_back(ExportEntry{
        "DragQueryFileW", 0,
        [&mgr](Marshaler& m) {
            m.set_return(mgr.drag_query_file(m, m.arg(0), m.arg32(1), m.arg_ptr(2),
                                             m.arg32(3), /*wide=*/true));
        },
        16
    });

    // SHAppBarMessage (2 args: dwMessage, pData, stdcall_bytes=8)
    exports.entries.push_back(ExportEntry{
        "SHAppBarMessage", 0,
        [](Marshaler& m) {
            (void)m.arg(0); (void)m.arg(1);
            VITREO_TRACE("[shell32] SHAppBarMessage -> 0\n");
            m.set_return(0);
        },
        8
    });

    // BOOL Shell_NotifyIcon{A,W}(DWORD dwMessage, PNOTIFYICONDATA lpData)
    // https://learn.microsoft.com/en-us/windows/win32/api/shellapi/nf-shellapi-shell_notifyiconw
    //
    // The notification area is the macOS menu bar: each icon is a status
    // item (notify_icons.hpp has the contract and its probes). A Dock click
    // on an app showing no window activates its icon, so an app that hides
    // in the tray still has a way back.
    const auto notify_icon_handler = [&mgr](bool wide) {
        return [&mgr, wide](Marshaler& m) {
            const u32 message = m.arg32(0);
            const GuestAddr data = m.arg_ptr(1);
            m.set_return(mgr.notify_icons().notify(m, message, data, wide));
        };
    };
    exports.entries.push_back(ExportEntry{
        "Shell_NotifyIconA", 0, notify_icon_handler(/*wide=*/false), 8
    });
    exports.entries.push_back(ExportEntry{
        "Shell_NotifyIconW", 0, notify_icon_handler(/*wide=*/true), 8
    });
    // HRESULT Shell_NotifyIconGetRect(const NOTIFYICONIDENTIFIER* identifier,
    //                                 RECT* iconLocation)
    exports.entries.push_back(ExportEntry{
        "Shell_NotifyIconGetRect", 0,
        [&mgr](Marshaler& m) {
            m.set_return(mgr.notify_icons().get_rect(m, m.arg_ptr(0), m.arg_ptr(1)));
        },
        8
    });

    // DragAcceptFiles(hWnd, fAccept) — VOID; pop 8 bytes
    exports.entries.push_back(ExportEntry{
        "DragAcceptFiles", 0,
        [](Marshaler& m) {
            (void)m.arg(0); (void)m.arg(1);
            m.set_return(0);
        },
        8
    });

    // ExtractIconExA(lpszFile, nIconIndex, phiconLarge, phiconSmall, nIcons)
    exports.entries.push_back(ExportEntry{
        "ExtractIconExA", 0,
        [](Marshaler& m) {
            (void)m.arg_ptr(0); (void)m.arg(1);
            (void)m.arg_ptr(2); (void)m.arg_ptr(3); (void)m.arg(4);
            m.set_return(0); // 0 icons extracted
        },
        20
    });

    // SHGetMalloc(ppMalloc) -> HRESULT
    exports.entries.push_back(ExportEntry{
        "SHGetMalloc", 0,
        [&mgr](Marshaler& m) { mgr.sh_get_malloc(m); },
        4
    });

    exports.entries.push_back(ExportEntry{
        "SHGetDesktopFolder", 0,
        [&mgr](Marshaler& m) { mgr.sh_get_desktop_folder(m); },
        4
    });

    auto not_implemented = [](Marshaler& m) { m.set_return(0x80004001u); };

    exports.entries.push_back({"Vitreo_ShellFolder_QueryInterface", 0,
        [&mgr](Marshaler& m) { mgr.shell_folder_query_interface(m); }, 12});
    exports.entries.push_back({"Vitreo_ShellFolder_AddRef", 0,
        [&mgr](Marshaler& m) { mgr.shell_folder_add_ref(m); }, 4});
    exports.entries.push_back({"Vitreo_ShellFolder_Release", 0,
        [&mgr](Marshaler& m) { mgr.shell_folder_release(m); }, 4});
    exports.entries.push_back({"Vitreo_ShellFolder_ParseDisplayName", 0, not_implemented, 24});
    exports.entries.push_back({"Vitreo_ShellFolder_EnumObjects", 0,
        [&mgr](Marshaler& m) { mgr.shell_folder_enum_objects(m); }, 16});
    exports.entries.push_back({"Vitreo_ShellFolder_BindToObject", 0,
        [&mgr](Marshaler& m) { mgr.shell_folder_bind_to_object(m); }, 20});
    exports.entries.push_back({"Vitreo_ShellFolder_BindToStorage", 0, not_implemented, 20});
    exports.entries.push_back({"Vitreo_ShellFolder_CompareIDs", 0,
        [&mgr](Marshaler& m) { mgr.shell_folder_compare_ids(m); }, 16});
    exports.entries.push_back({"Vitreo_ShellFolder_CreateViewObject", 0, not_implemented, 16});
    exports.entries.push_back({"Vitreo_ShellFolder_GetAttributesOf", 0,
        [&mgr](Marshaler& m) { mgr.shell_folder_get_attributes(m); }, 16});
    exports.entries.push_back({"Vitreo_ShellFolder_GetUIObjectOf", 0, not_implemented, 28});
    exports.entries.push_back({"Vitreo_ShellFolder_GetDisplayNameOf", 0,
        [&mgr](Marshaler& m) { mgr.shell_folder_get_display_name(m); }, 16});
    exports.entries.push_back({"Vitreo_ShellFolder_SetNameOf", 0, not_implemented, 24});

    exports.entries.push_back({"Vitreo_EnumIDList_QueryInterface", 0,
        [&mgr](Marshaler& m) { mgr.enum_id_list_query_interface(m); }, 12});
    exports.entries.push_back({"Vitreo_EnumIDList_AddRef", 0,
        [&mgr](Marshaler& m) { mgr.enum_id_list_add_ref(m); }, 4});
    exports.entries.push_back({"Vitreo_EnumIDList_Release", 0,
        [&mgr](Marshaler& m) { mgr.enum_id_list_release(m); }, 4});
    exports.entries.push_back({"Vitreo_EnumIDList_Next", 0,
        [&mgr](Marshaler& m) { mgr.enum_id_list_next(m); }, 16});
    exports.entries.push_back({"Vitreo_EnumIDList_Skip", 0,
        [&mgr](Marshaler& m) { mgr.enum_id_list_skip(m); }, 8});
    exports.entries.push_back({"Vitreo_EnumIDList_Reset", 0,
        [&mgr](Marshaler& m) { mgr.enum_id_list_reset(m); }, 4});
    exports.entries.push_back({"Vitreo_EnumIDList_Clone", 0,
        [&mgr](Marshaler& m) { mgr.enum_id_list_clone(m); }, 8});

    exports.entries.push_back({"Vitreo_ShellMalloc_QueryInterface", 0,
        [&mgr](Marshaler& m) { mgr.shell_malloc_query_interface(m); }, 12});
    exports.entries.push_back({"Vitreo_ShellMalloc_AddRef", 0,
        [&mgr](Marshaler& m) { mgr.shell_malloc_add_ref(m); }, 4});
    exports.entries.push_back({"Vitreo_ShellMalloc_Release", 0,
        [&mgr](Marshaler& m) { mgr.shell_malloc_release(m); }, 4});
    exports.entries.push_back({"Vitreo_ShellMalloc_Alloc", 0,
        [&mgr](Marshaler& m) { mgr.shell_malloc_alloc(m); }, 8});
    exports.entries.push_back({"Vitreo_ShellMalloc_Realloc", 0,
        [&mgr](Marshaler& m) { mgr.shell_malloc_realloc(m); }, 12});
    exports.entries.push_back({"Vitreo_ShellMalloc_Free", 0,
        [&mgr](Marshaler& m) { mgr.shell_malloc_free(m); }, 8});
    exports.entries.push_back({"Vitreo_ShellMalloc_GetSize", 0,
        [&mgr](Marshaler& m) { mgr.shell_malloc_get_size(m); }, 8});
    exports.entries.push_back({"Vitreo_ShellMalloc_DidAlloc", 0,
        [&mgr](Marshaler& m) { mgr.shell_malloc_did_alloc(m); }, 8});
    exports.entries.push_back({"Vitreo_ShellMalloc_HeapMinimize", 0,
        [&mgr](Marshaler& m) { mgr.shell_malloc_heap_minimize(m); }, 4});

    // PIDL helpers imported by ordinal: #190, #155, and
    // SHOpenFolderAndSelectItems.
    // Ordinals from the Windows 11 shell32.dll export table.
    //
    // void ILFree(PIDLIST_RELATIVE) — #155. Learn: frees with the COM task
    // allocator; NULL is ignored. Last error untouched (s7_shell_probe).
    exports.entries.push_back(ExportEntry{
        "ILFree", 155,
        [&mgr](Marshaler& m) { mgr.sh_free(m.arg_ptr(0)); },
        4
    });
    // UINT ILGetSize(PCUIDLIST_RELATIVE) — #152: bytes including the
    // two-byte terminator; 0 for NULL (s7_shell_probe).
    exports.entries.push_back(ExportEntry{
        "ILGetSize", 152,
        [](Marshaler& m) {
            GuestAddr at = m.arg_ptr(0);
            if (at == 0) { m.set_return(0); return; }
            u64 size = 0;
            for (u32 guard = 0; guard < 65536; ++guard) {
                const u16 cb = m.read_u16(at).value_or(0);
                if (cb == 0) break;
                size += cb;
                at += cb;
            }
            m.set_return(static_cast<u32>(size + 2));
        },
        4
    });
    // PIDLIST_ABSOLUTE ILCreateFromPathW(PCWSTR) — #190. NULL with the file
    // system error when the item does not exist; on success Windows leaves
    // ERROR_INVALID_PARAMETER as the last error (s7_shell_probe, x86 and x64).
    exports.entries.push_back(ExportEntry{
        "ILCreateFromPathW", 190,
        [&mgr](Marshaler& m) {
            const GuestAddr path_addr = m.arg_ptr(0);
            const std::string path = path_addr != 0
                ? m.read_wide_string(path_addr, 32768).value_or("") : std::string{};
            m.set_return(mgr.il_create_from_path(path));
        },
        4
    });
    // ULONG SHChangeNotifyRegister(HWND, int, LONG, UINT, int,
    //                              const SHChangeNotifyEntry*) — #2.
    // Change notifications come from Explorer's notification server; a
    // Windows session without it (measured over ssh, s7_shell_probe) fails
    // every registration with 0 / ERROR_INSUFFICIENT_BUFFER and a NULL hwnd
    // with ERROR_INVALID_WINDOW_HANDLE. Vitreo has no such server.
    exports.entries.push_back(ExportEntry{
        "SHChangeNotifyRegister", 2,
        [](Marshaler& m) {
            if (m.arg_ptr(0) == 0) {
                set_last_error(error_codes::ERROR_INVALID_WINDOW_HANDLE);
                m.set_return(0);
                return;
            }
            static std::once_flag once;
            std::call_once(once, [] {
                std::fprintf(stderr, "[vitreo] UNIMPLEMENTED shell32!SHChangeNotifyRegister: "
                                     "no shell change-notification server\n");
            });
            set_last_error(error_codes::ERROR_INSUFFICIENT_BUFFER);
            m.set_return(0);
        },
        24
    });
    // BOOL SHChangeNotifyDeregister(ULONG) — #4. Nothing can be registered
    // (above); FALSE with the last error cleared, as measured without the
    // notification server.
    exports.entries.push_back(ExportEntry{
        "SHChangeNotifyDeregister", 4,
        [](Marshaler& m) {
            (void)m.arg32(0);
            set_last_error(error_codes::ERROR_SUCCESS);
            m.set_return(0);
        },
        4
    });

    // SHRestricted (ordinal 100) -> 0 (no restrictions)
    exports.entries.push_back(ExportEntry{
        "SHRestricted", 100,
        [](Marshaler& m) {
            (void)m.arg(0);
            m.set_return(0);
        },
        4
    });

    // SHFlushClipboard: Windows 11 exports it only as an anonymous ordinal
    // (#121, forwarded to SHUNIMPL), never by name, so it is not published.
    exports.entries.push_back(ExportEntry{
        "SHOpenFolderAndSelectItems", 0,
        [](Marshaler& m) {
            (void)m.arg_ptr(0); (void)m.arg(1);
            (void)m.arg_ptr(2); (void)m.arg(3);
            m.set_return(0x80004001u); // E_NOTIMPL
        },
        16
    });

    // UINT DragQueryFileA(HDROP hDrop, UINT iFile, LPSTR lpszFile, UINT cch)
    exports.entries.push_back(ExportEntry{
        "DragQueryFileA", 0,
        [&mgr](Marshaler& m) {
            m.set_return(mgr.drag_query_file(m, m.arg(0), m.arg32(1), m.arg_ptr(2),
                                             m.arg32(3), /*wide=*/false));
        },
        16
    });

    // HRESULT SetCurrentProcessExplicitAppUserModelID(PCWSTR AppID)
    exports.entries.push_back(ExportEntry{
        "SetCurrentProcessExplicitAppUserModelID", 0,
        [&mgr](Marshaler& m) {
            mgr.set_current_process_explicit_app_user_model_id(m);
        },
        4
    });
    // HRESULT GetCurrentProcessExplicitAppUserModelID(PWSTR* AppID)
    exports.entries.push_back(ExportEntry{
        "GetCurrentProcessExplicitAppUserModelID", 0,
        [&mgr](Marshaler& m) {
            mgr.get_current_process_explicit_app_user_model_id(m);
        },
        4
    });

    // SHGetPropertyStoreForWindow is registered beside the other shell32
    // exports that return ole32 objects (dll_registry_shell_ole.cpp).

    // HRESULT SHCreateAssociationRegistration(REFIID riid, void** ppv)
    // Learn (shobjidl_core.h): creates the stock Windows
    // IApplicationAssociationRegistration object, which reads the machine's
    // Default Programs association database. The capsule has no such database
    // and no stock implementation to hand out, so clear *ppv and report that
    // the class is not registered. The export must still exist: chrome.dll
    // delay-loads it, and its delay-load failure hook calls ImmediateCrash for
    // any module outside its small allowlist, so a missing procedure kills the
    // process outright instead of surfacing an HRESULT the caller can handle.
    exports.entries.push_back(ExportEntry{
        "SHCreateAssociationRegistration", 0,
        [](Marshaler& m) {
            (void)m.arg_ptr(0);  // riid
            const GuestAddr ppv = m.arg_ptr(1);
            if (ppv == 0) {
                m.set_return(0x80004003u);  // E_POINTER
                return;
            }
            (void)m.write_ptr(ppv, 0);
            VITREO_TRACE(
                "[shell32] SHCreateAssociationRegistration -> "
                "REGDB_E_CLASSNOTREG (no association registration database)\n");
            m.set_return(0x80040154u);  // REGDB_E_CLASSNOTREG
        },
        8
    });

    return exports;
}

} // namespace vitreo::winapi
