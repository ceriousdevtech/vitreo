#include "shell_paths.hpp"
#include "winapi/system/shell32/shell_paths_internal.hpp"
#include "winapi/common/elevation.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/kernel32/filesystem/file.hpp"
#include "winapi/kernel32/process/process.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/com/ole32/task_memory.hpp"
#include "winapi/kernel32/memory/global_local.hpp"
#include "winapi/kernel32/locale/codepage_convert.hpp"
#include "capsule/vfs/vfs.hpp"
#include "capsule/vfs/host_filesystem.hpp"
#include "winapi/ui/comctl32/comctl32.hpp"
#include "windowing/cocoa/cocoa_bridge.hpp"
#include "core/memory/guest_memory.hpp"
#include <algorithm>
#include <mutex>
#include <optional>
#include <span>
#include <cctype>
#include <cstring>
#include <sstream>
#include <cstdio>

namespace vitreo::winapi {

ShellPathManager::ShellPathManager(capsule::Vfs& vfs,
                                   windowing::CocoaBridge& bridge,
                                   GuestMemory& memory,
                                   ProcessManager& process_mgr,
                                   ComTaskMemoryManager& task_memory,
                                   const GuestElevationState* elevation)
    : m_vfs{vfs}
    , m_bridge{bridge}
    , m_memory{memory}
    , m_process_mgr{process_mgr}
    , m_task_memory{task_memory}
    , m_elevation{elevation}
{
}

namespace {

struct KnownFolderRow {
    const char* name;
    struct { u32 d1; u16 d2; u16 d3; u8 d4[8]; } id;
    u32 hr64;
    const wchar_t* path64;
    u32 hr32;
    const wchar_t* path32;
};
struct CsidlRow {
    u32 csidl;
    u32 hr64;
    const wchar_t* path64;
    u32 hr32;
    const wchar_t* path32;
};

#include "winapi/system/shell32/known_folder_table.inc"

constexpr u32 kHrFileNotFound = 0x80070002u; // HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)

/// Expand the probe's %P% / %W% / %D% prefixes to the capsule's profile,
/// Windows directory and ProgramData (kernel32 environment.cpp values).
std::wstring expand_folder_template(const wchar_t* tpl) {
    std::wstring s = tpl;
    auto rep = [&](const wchar_t* key, const wchar_t* value) {
        if (s.rfind(key, 0) == 0) s = value + s.substr(3);
    };
    rep(L"%P%", L"C:\\Users\\Default");
    rep(L"%W%", L"C:\\Windows");
    rep(L"%D%", L"C:\\ProgramData");
    return s;
}

} // namespace

u32 ShellPathManager::resolve_folder(const std::wstring& path, bool create,
                                     bool verify) const {
    const std::string full(path.begin(), path.end());
    if (create) {
        usize end = 3; // Skip the drive root.
        while (end < full.size()) {
            end = full.find('\\', end);
            const std::string part = full.substr(0, end);
            auto existing = m_vfs.stat(part);
            if (existing.is_err()) {
                auto created = m_vfs.create_dir(part);
                if (created.is_err() &&
                    created.error() != capsule::VfsError::PathAlreadyExists) {
                    return hr::E_FAIL;
                }
                existing = m_vfs.stat(part);
            }
            if (existing.is_err() || !capsule::has_attribute(
                    existing.value().attributes, capsule::FileAttributes::Directory)) {
                return hr::E_FAIL;
            }
            if (end == std::string::npos) break;
            ++end;
        }
        return hr::S_OK;
    }
    if (verify) {
        // Without *_FLAG_DONT_VERIFY a folder that is not present fails with
        // HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) and no path
        // (shell_paths_probe kf_verify_screenshots / CSIDL 0x3a).
        auto st = m_vfs.stat(full);
        if (st.is_err() || !capsule::has_attribute(st.value().attributes,
                                                   capsule::FileAttributes::Directory)) {
            return kHrFileNotFound;
        }
    }
    return hr::S_OK;
}

u32 ShellPathManager::get_folder_path(i32 csidl_val, std::wstring& path,
                                      bool verify, bool is64) const {
    path.clear();
    // CSIDL_FOLDER_MASK = 0x00FF; CSIDL_FLAG_CREATE = 0x8000,
    // CSIDL_FLAG_DONT_VERIFY = 0x4000 (shlobj_core.h).
    const u32 id = static_cast<u32>(csidl_val) & 0xFFu;
    const CsidlRow* row = nullptr;
    for (const auto& r : kCsidls) {
        if (r.csidl == id) { row = &r; break; }
    }
    if (row == nullptr) return hr::E_INVALIDARG;
    const u32 result = is64 ? row->hr64 : row->hr32;
    const wchar_t* tpl = is64 ? row->path64 : row->path32;
    if (result != hr::S_OK || tpl == nullptr) return result;
    std::wstring candidate = expand_folder_template(tpl);
    const bool create = (csidl_val & 0x8000) != 0;
    const bool dont_verify = (csidl_val & 0x4000) != 0;
    if (const u32 e = resolve_folder(candidate, create, verify && !dont_verify);
        e != hr::S_OK) {
        return e;
    }
    path = std::move(candidate);
    return hr::S_OK;
}

u32 ShellPathManager::get_known_folder_path_by_guid(const u8 (&guid)[16], u32 flags,
                                                    bool is64,
                                                    std::wstring& path) const {
    path.clear();
    const KnownFolderRow* row = nullptr;
    for (const auto& r : kKnownFolders) {
        u8 bytes[16];
        std::memcpy(bytes, &r.id.d1, 4);
        std::memcpy(bytes + 4, &r.id.d2, 2);
        std::memcpy(bytes + 6, &r.id.d3, 2);
        std::memcpy(bytes + 8, r.id.d4, 8);
        if (std::memcmp(bytes, guid, 16) == 0) { row = &r; break; }
    }
    // An unregistered KNOWNFOLDERID is ERROR_FILE_NOT_FOUND, not
    // E_INVALIDARG (shell_paths_probe kf_bogus).
    if (row == nullptr) return kHrFileNotFound;
    const u32 result = is64 ? row->hr64 : row->hr32;
    const wchar_t* tpl = is64 ? row->path64 : row->path32;
    if (result != hr::S_OK || tpl == nullptr) return result;
    std::wstring candidate = expand_folder_template(tpl);
    constexpr u32 KF_FLAG_CREATE = 0x00008000u, KF_FLAG_DONT_VERIFY = 0x00004000u;
    if (const u32 e = resolve_folder(candidate, (flags & KF_FLAG_CREATE) != 0,
                                     (flags & KF_FLAG_DONT_VERIFY) == 0);
        e != hr::S_OK) {
        return e;
    }
    path = std::move(candidate);
    return hr::S_OK;
}

u32 ShellPathManager::get_known_folder_path(const std::string& folder_name,
                                             std::wstring& path) const {
    for (const auto& r : kKnownFolders) {
        if (folder_name == r.name) {
            if (r.hr64 != hr::S_OK || r.path64 == nullptr) return r.hr64;
            path = expand_folder_template(r.path64);
            return hr::S_OK;
        }
    }
    return kHrFileNotFound;
}

namespace {

std::u16string utf8_to_utf16(std::string_view in) {
    std::u16string out;
    for (usize i = 0; i < in.size();) {
        const u8 c = static_cast<u8>(in[i]);
        u32 cp = 0xFFFD;
        usize len = 1;
        if (c < 0x80) { cp = c; }
        else if ((c & 0xE0) == 0xC0 && i + 1 < in.size()) {
            cp = ((c & 0x1Fu) << 6) | (static_cast<u8>(in[i + 1]) & 0x3Fu); len = 2;
        } else if ((c & 0xF0) == 0xE0 && i + 2 < in.size()) {
            cp = ((c & 0x0Fu) << 12) | ((static_cast<u8>(in[i + 1]) & 0x3Fu) << 6) |
                 (static_cast<u8>(in[i + 2]) & 0x3Fu); len = 3;
        } else if ((c & 0xF8) == 0xF0 && i + 3 < in.size()) {
            cp = ((c & 0x07u) << 18) | ((static_cast<u8>(in[i + 1]) & 0x3Fu) << 12) |
                 ((static_cast<u8>(in[i + 2]) & 0x3Fu) << 6) |
                 (static_cast<u8>(in[i + 3]) & 0x3Fu); len = 4;
        }
        i += len;
        if (cp >= 0x10000) {
            cp -= 0x10000;
            out.push_back(static_cast<char16_t>(0xD800 + (cp >> 10)));
            out.push_back(static_cast<char16_t>(0xDC00 + (cp & 0x3FF)));
        } else {
            out.push_back(static_cast<char16_t>(cp));
        }
    }
    return out;
}

} // namespace

void ShellPathManager::command_line_to_argv(const std::u16string& cmd,
                                             const std::u16string& exe_path,
                                             std::vector<std::u16string>& args) const {
    // Learn CommandLineToArgvW, confirmed case by case on Windows 10
    // (tools/oracle_probe/shell_paths_probe.expected, argv lines):
    //  - an empty string yields the executable path as the only argument;
    //  - argv[0] is the program name: a leading quote runs to the next quote,
    //    otherwise it runs to whitespace; no backslash processing, and
    //    leading whitespace makes it the empty string;
    //  - whitespace is space or tab;
    //  - 2n backslashes + quote -> n backslashes, the quote delimits;
    //    2n+1 backslashes + quote -> n backslashes and a literal quote;
    //    other backslashes are literal;
    //  - inside quotes, "" is a literal quote and quoting continues;
    //  - "" as a whole argument is an empty argument.
    args.clear();
    if (cmd.empty()) {
        args.push_back(exe_path);
        return;
    }
    const usize n = cmd.size();
    usize i = 0;
    std::u16string first;
    if (cmd[0] == u'"') {
        ++i;
        while (i < n && cmd[i] != u'"') first.push_back(cmd[i++]);
        if (i < n) ++i; // closing quote
    } else {
        while (i < n && cmd[i] != u' ' && cmd[i] != u'\t') first.push_back(cmd[i++]);
    }
    args.push_back(std::move(first));

    for (;;) {
        while (i < n && (cmd[i] == u' ' || cmd[i] == u'\t')) ++i;
        if (i >= n) break;
        std::u16string cur;
        bool quoted = false;
        while (i < n) {
            const char16_t c = cmd[i];
            if (!quoted && (c == u' ' || c == u'\t')) break;
            if (c == u'\\') {
                usize k = 0;
                while (i + k < n && cmd[i + k] == u'\\') ++k;
                if (i + k < n && cmd[i + k] == u'"') {
                    cur.append(k / 2, u'\\');
                    if (k % 2 == 1) {
                        cur.push_back(u'"');
                        i += k + 1;
                    } else {
                        i += k; // the quote is handled as a delimiter below
                    }
                } else {
                    cur.append(k, u'\\');
                    i += k;
                }
                continue;
            }
            if (c == u'"') {
                if (quoted && i + 1 < n && cmd[i + 1] == u'"') {
                    cur.push_back(u'"');
                    i += 2;
                    continue;
                }
                quoted = !quoted;
                ++i;
                continue;
            }
            cur.push_back(c);
            ++i;
        }
        args.push_back(std::move(cur));
    }
}

GuestAddr ShellPathManager::command_line_to_argv_guest(Marshaler& m,
                                                        GuestAddr cmd_ptr,
                                                        GuestAddr num_args_ptr) {
    // Learn: pNumArgs is required; the probe shows NULL -> NULL with
    // ERROR_INVALID_PARAMETER (shell_paths_probe argv_nullcount).
    if (num_args_ptr == 0 || cmd_ptr == 0) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        return 0;
    }
    // Read the UTF-16 command line unit by unit (no UTF-8 detour).
    std::u16string cmd;
    for (GuestAddr p = cmd_ptr;; p += 2) {
        auto unit = m.read_u16(p);
        if (unit.is_err()) {
            set_last_error(error_codes::ERROR_NOACCESS);
            return 0;
        }
        if (unit.value() == 0) break;
        cmd.push_back(static_cast<char16_t>(unit.value()));
    }
    std::u16string exe;
    if (cmd.empty() && m_executable_path) {
        const std::string utf8 = m_executable_path();
        exe = utf8_to_utf16(utf8);
    }
    std::vector<std::u16string> args;
    command_line_to_argv(cmd, exe, args);

    // One LocalAlloc block: pointer array (argc + 1) then the strings, so a
    // single LocalFree releases everything (Learn).
    const u32 argc = static_cast<u32>(args.size());
    const u32 pointer_size = m.cpu_is_64bit() ? 8u : 4u;
    const u64 ptr_array_size = static_cast<u64>(argc + 1) * pointer_size;
    u64 strings_size = 0;
    for (const auto& arg : args) strings_size += (arg.size() + 1) * 2;
    if (m_local_memory == nullptr) {
        std::fprintf(stderr,
            "[vitreo] UNIMPLEMENTED shell32!CommandLineToArgvW: no LocalAlloc heap wired\n");
        set_last_error(error_codes::ERROR_NOT_ENOUGH_MEMORY);
        return 0;
    }
    const GuestAddr base = static_cast<GuestAddr>(
        m_local_memory->global_alloc(0, ptr_array_size + strings_size));
    if (base == 0) {
        set_last_error(error_codes::ERROR_NOT_ENOUGH_MEMORY);
        return 0;
    }
    GuestAddr str_addr = base + ptr_array_size;
    for (u32 i = 0; i < argc; ++i) {
        (void)m.write_ptr(base + i * pointer_size, str_addr);
        const auto& arg = args[i];
        if (!arg.empty()) {
            (void)m.memory().write_bytes(str_addr, reinterpret_cast<const Byte*>(arg.data()),
                                         arg.size() * 2);
        }
        (void)m.write_u16(str_addr + arg.size() * 2, 0);
        str_addr += (arg.size() + 1) * 2;
    }
    (void)m.write_ptr(base + argc * pointer_size, 0);
    (void)m.write_u32(num_args_ptr, argc);
    return base;
}

u32 ShellPathManager::create_directory_ex(std::string_view path) {
    // int SHCreateDirectoryExW(HWND, PCWSTR pszPath, const SECURITY_ATTRIBUTES*)
    // https://learn.microsoft.com/en-us/windows/win32/api/shlobj_core/nf-shlobj_core-shcreatedirectoryexw
    // Measured (tools/oracle_probe/s7_shell_probe.expected): the return value
    // is also stored as the last error (ERROR_SUCCESS included), except for
    // an over-long path, which returns ERROR_FILENAME_EXCED_RANGE with
    // ERROR_PATH_NOT_FOUND left as the last error once it passes MAX_PATH.
    //   relative or empty path        -> ERROR_BAD_PATHNAME
    //   \\server\share\...             -> ERROR_BAD_NETPATH (no such server)
    //   length >= 248                  -> ERROR_FILENAME_EXCED_RANGE
    //   existing directory or file     -> ERROR_ALREADY_EXISTS
    //   a file on the way / no drive   -> ERROR_PATH_NOT_FOUND
    //   missing parents                -> created
    auto finish = [](u32 code, u32 last_error) {
        set_last_error(last_error);
        return code;
    };
    const std::u16string units = utf8_to_utf16(path);
    const bool drive_absolute = units.size() >= 3 && units[1] == u':' &&
        (units[2] == u'\\' || units[2] == u'/') &&
        ((units[0] | 0x20) >= u'a' && (units[0] | 0x20) <= u'z');
    const bool unc = units.size() >= 2 &&
        (units[0] == u'\\' || units[0] == u'/') &&
        (units[1] == u'\\' || units[1] == u'/');
    if (unc) {
        return finish(sh_err::ERROR_BAD_NETPATH, sh_err::ERROR_BAD_NETPATH);
    }
    if (!drive_absolute) {
        return finish(sh_err::ERROR_BAD_PATHNAME, sh_err::ERROR_BAD_PATHNAME);
    }
    constexpr usize kMaxDirectoryPath = 248;  // MAX_PATH less an 8.3 name
    if (units.size() >= kMaxDirectoryPath) {
        return finish(sh_err::ERROR_FILENAME_EXCED_RANGE,
                      units.size() >= 260 ? sh_err::ERROR_PATH_NOT_FOUND
                                          : sh_err::ERROR_FILENAME_EXCED_RANGE);
    }
    const char drive = capsule::normalize_drive_letter(static_cast<char>(units[0]));
    if (!m_vfs.is_drive_mounted(drive)) {
        return finish(sh_err::ERROR_PATH_NOT_FOUND, sh_err::ERROR_PATH_NOT_FOUND);
    }

    std::string full(path);
    std::replace(full.begin(), full.end(), '/', '\\');
    while (full.size() > 3 && full.back() == '\\') full.pop_back();
    if (m_vfs.stat(full).is_ok()) {
        return finish(sh_err::ERROR_ALREADY_EXISTS, sh_err::ERROR_ALREADY_EXISTS);
    }

    // Create each missing component from the root down.
    for (usize at = 3; at <= full.size(); ++at) {
        if (at != full.size() && full[at] != '\\') continue;
        const std::string prefix = full.substr(0, at);
        if (prefix.size() <= 3) continue;
        auto info = m_vfs.stat(prefix);
        if (info.is_ok()) {
            if (!capsule::has_attribute(info.value().attributes,
                                        capsule::FileAttributes::Directory)) {
                return finish(sh_err::ERROR_PATH_NOT_FOUND, sh_err::ERROR_PATH_NOT_FOUND);
            }
            continue;
        }
        auto made = m_vfs.create_dir(prefix);
        if (made.is_err() && made.error() != capsule::VfsError::PathAlreadyExists) {
            const u32 code = vfs_error_to_win32(made.error());
            return finish(code, code);
        }
    }
    return finish(sh_err::ERROR_SUCCESS, sh_err::ERROR_SUCCESS);
}

GuestAddr ShellPathManager::alloc_shell_memory(GuestMemory& memory, u32 size) {
    std::lock_guard lock(m_shell_allocation_mutex);
    u32 alloc_size = (size + 0xFFFu) & ~0xFFFu;
    if (alloc_size == 0) alloc_size = 0x1000;

    GuestAddr base = m_shell_alloc_next;
    m_shell_alloc_next += alloc_size;

    auto map_r = memory.map_region(base, alloc_size, MemoryProtection::ReadWrite);
    if (!map_r.is_ok()) {
        VITREO_TRACE("[shell32] alloc_shell_memory: map_region failed at "
                     "0x%08llX size=0x%X\n",
                     static_cast<unsigned long long>(base), alloc_size);
        return 0;
    }
    return base;
}

void ShellPathManager::set_current_process_explicit_app_user_model_id(
    Marshaler& m) {
    constexpr usize kMaxBufferCodeUnits = 128;
    const GuestAddr app_id_ptr = m.arg_ptr(0);
    if (app_id_ptr == 0) {
        m.set_return(hr::E_INVALIDARG);
        return;
    }

    std::u16string app_id;
    app_id.reserve(kMaxBufferCodeUnits - 1);
    for (usize index = 0; index < kMaxBufferCodeUnits; ++index) {
        auto code_unit = m.read_u16(app_id_ptr + index * sizeof(u16));
        if (code_unit.is_err()) {
            VITREO_TRACE(
                "[shell32] SetCurrentProcessExplicitAppUserModelID: "
                "unreadable AppID at code_unit=%zu\n",
                index);
            m.set_return(hr::E_INVALIDARG);
            return;
        }
        if (code_unit.value() == 0) {
            {
                std::lock_guard lock(m_app_user_model_id_mutex);
                m_explicit_app_user_model_id = std::move(app_id);
            }
            VITREO_TRACE(
                "[shell32] SetCurrentProcessExplicitAppUserModelID: "
                "stored explicit process identity\n");
            m.set_return(hr::S_OK);
            return;
        }
        app_id.push_back(static_cast<char16_t>(code_unit.value()));
    }

    VITREO_TRACE(
        "[shell32] SetCurrentProcessExplicitAppUserModelID: "
        "AppID exceeds the 128-code-unit input buffer\n");
    m.set_return(hr::E_INVALIDARG);
}

void ShellPathManager::set_explicit_app_user_model_id(std::u16string app_id) {
    std::lock_guard lock(m_app_user_model_id_mutex);
    m_explicit_app_user_model_id = std::move(app_id);
}

std::optional<std::u16string> ShellPathManager::explicit_app_user_model_id() const {
    std::lock_guard lock(m_app_user_model_id_mutex);
    return m_explicit_app_user_model_id;
}

void ShellPathManager::get_current_process_explicit_app_user_model_id(
    Marshaler& m) {
    const GuestAddr app_id_out = m.arg_ptr(0);
    if (app_id_out == 0) {
        // Windows faults for this invalid required out pointer. Vitreo cannot
        // safely fault host memory here, so fail the guest call explicitly.
        m.set_return(hr::E_INVALIDARG);
        return;
    }
    if (m.write_ptr(app_id_out, 0).is_err()) {
        m.set_return(hr::E_INVALIDARG);
        return;
    }

    std::u16string app_id;
    {
        std::lock_guard lock(m_app_user_model_id_mutex);
        if (!m_explicit_app_user_model_id.has_value()) {
            m.set_return(hr::E_FAIL);
            return;
        }
        app_id = *m_explicit_app_user_model_id;
    }

    const u32 allocation_bytes =
        static_cast<u32>((app_id.size() + 1) * sizeof(u16));
    const GuestAddr allocation = m_task_memory.allocate(allocation_bytes);
    if (allocation == 0) {
        m.set_return(hr::E_OUTOFMEMORY);
        return;
    }

    for (usize index = 0; index < app_id.size(); ++index) {
        if (m.write_u16(allocation + index * sizeof(u16),
                        static_cast<u16>(app_id[index])).is_err()) {
            m_task_memory.free(allocation);
            m.set_return(hr::E_FAIL);
            return;
        }
    }
    if (m.write_u16(allocation + app_id.size() * sizeof(u16), 0).is_err()) {
        m_task_memory.free(allocation);
        m.set_return(hr::E_FAIL);
        return;
    }
    if (m.write_ptr(app_id_out, allocation).is_err()) {
        m_task_memory.free(allocation);
        m.set_return(hr::E_INVALIDARG);
        return;
    }

    VITREO_TRACE(
        "[shell32] GetCurrentProcessExplicitAppUserModelID: "
        "returned %zu UTF-16 code units at 0x%llx\n",
        app_id.size(), static_cast<unsigned long long>(allocation));
    m.set_return(hr::S_OK);
}

// =============================================================================
// Folder Browser
// =============================================================================

/// Helper to convert UTF-8 to wide string
static std::wstring utf8_to_wstring(const std::string& s) {
    std::wstring result;
    result.reserve(s.size());
    for (size_t i = 0; i < s.size(); ) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if (c < 0x80) {
            result.push_back(static_cast<wchar_t>(c));
            ++i;
        } else if ((c & 0xE0) == 0xC0 && i + 1 < s.size()) {
            wchar_t wc = static_cast<wchar_t>(((c & 0x1F) << 6) |
                         (static_cast<unsigned char>(s[i+1]) & 0x3F));
            result.push_back(wc);
            i += 2;
        } else if ((c & 0xF0) == 0xE0 && i + 2 < s.size()) {
            wchar_t wc = static_cast<wchar_t>(((c & 0x0F) << 12) |
                         ((static_cast<unsigned char>(s[i+1]) & 0x3F) << 6) |
                         (static_cast<unsigned char>(s[i+2]) & 0x3F));
            result.push_back(wc);
            i += 3;
        } else {
            result.push_back(L'?');
            ++i;
        }
    }
    return result;
}

GuestAddr ShellPathManager::browse_for_folder(const std::wstring& title) {
    std::string title_utf8 = wstring_to_utf8(title);

    // Create a callback that lists VFS directories
    windowing::CocoaBridge::VfsListCallback list_callback =
        [this](const std::string& windows_path) -> std::vector<windowing::CocoaBridge::VfsDirEntry> {
            std::vector<windowing::CocoaBridge::VfsDirEntry> entries;

            VITREO_TRACE( "[shell32] list_callback: path='%s'\n", windows_path.c_str());

            // Special case: empty path means list mounted drives
            if (windows_path.empty()) {
                auto drives = m_vfs.mounted_drives();
                VITREO_TRACE( "[shell32] list_callback: listing %zu drives\n", drives.size());
                for (char d : drives) {
                    windowing::CocoaBridge::VfsDirEntry entry;
                    entry.name = std::string(1, d) + ":";
                    entry.is_directory = true;
                    entries.push_back(entry);
                    VITREO_TRACE( "[shell32]   drive: %c:\n", d);
                }
                return entries;
            }

            // List directory contents from VFS
            auto list_result = m_vfs.list_dir(windows_path);
            if (list_result.is_err()) {
                VITREO_TRACE( "[shell32] list_dir failed for '%s'\n",
                             windows_path.c_str());
                return entries;
            }

            VITREO_TRACE( "[shell32] list_dir returned %zu entries\n",
                         list_result.value().size());
            for (const auto& dir_entry : list_result.value()) {
                // Only include directories
                if (capsule::has_attribute(dir_entry.info.attributes, capsule::FileAttributes::Directory)) {
                    windowing::CocoaBridge::VfsDirEntry entry;
                    entry.name = dir_entry.name;
                    entry.is_directory = true;
                    entries.push_back(entry);
                    VITREO_TRACE( "[shell32]   dir: '%s'\n", dir_entry.name.c_str());
                }
            }
            return entries;
        };

    // Callback to create directory in VFS
    auto create_dir_callback = [this](const std::string& windows_path) -> bool {
        VITREO_TRACE( "[shell32] create_dir_callback: path='%s'\n", windows_path.c_str());
        auto result = m_vfs.create_dir(windows_path);
        if (result.is_err()) {
            VITREO_TRACE( "[shell32] create_dir failed: %d\n", 
                         static_cast<int>(result.error()));
            return false;
        }
        return true;
    };

    // Show the VFS folder picker
    std::string selected = m_bridge.show_vfs_folder_picker("C:\\", title_utf8, list_callback, create_dir_callback);

    if (selected.empty()) {
        // User cancelled
        return 0;
    }

    VITREO_TRACE( "[shell32] browse_for_folder: selected='%s'\n", selected.c_str());

    // Convert to wide string and store
    std::wstring win_path = utf8_to_wstring(selected);

    (void)win_path;
    // A real ITEMIDLIST in guest task memory: the caller frees it with
    // CoTaskMemFree/ILFree and may walk it (ILGetSize, SHGetPathFromIDList).
    return create_pidl(selected);
}

bool ShellPathManager::get_path_from_pidl(GuestAddr pidl, std::wstring& path) const {
    std::string narrow_path;
    if (!pidl_path(pidl, narrow_path)) return false;
    path.assign(narrow_path.begin(), narrow_path.end());
    return true;
}

void ShellPathManager::free_pidl(GuestAddr pidl) {
    sh_free(pidl);
}

GuestAddr ShellPathManager::il_create_from_path(const std::string& path) {
    std::string full(path);
    std::replace(full.begin(), full.end(), '/', '\\');
    while (full.size() > 3 && full.back() == '\\') full.pop_back();
    if (full.empty() || m_vfs.stat(full).is_err()) {
        const auto slash = full.find_last_of('\\');
        const bool parent_exists = slash != std::string::npos &&
            m_vfs.stat(full.substr(0, slash + 1)).is_ok();
        set_last_error(parent_exists ? error_codes::ERROR_FILE_NOT_FOUND
                                     : error_codes::ERROR_PATH_NOT_FOUND);
        return 0;
    }
    const GuestAddr pidl = create_pidl(full);
    if (pidl == 0) {
        set_last_error(error_codes::ERROR_NOT_ENOUGH_MEMORY);
        return 0;
    }
    set_last_error(error_codes::ERROR_INVALID_PARAMETER);
    return pidl;
}

void ShellPathManager::sh_free(GuestAddr allocation) {
    std::lock_guard lock(m_shell_allocation_mutex);
    // SHFree / ILFree / IMalloc::Free: COM task memory (every PIDL and
    // IMalloc block comes from there).
    if (allocation == 0) return;
    m_pidl_paths.erase(allocation);
    if (m_task_memory.owns(allocation)) m_task_memory.free(allocation);
}

// =============================================================================
// SHFileOperationW
// =============================================================================

// SHFILEOPSTRUCT operation codes
namespace fo {
    constexpr u32 FO_MOVE   = 0x0001;
    constexpr u32 FO_COPY   = 0x0002;
    constexpr u32 FO_DELETE = 0x0003;
    constexpr u32 FO_RENAME = 0x0004;
}

// SHFILEOPSTRUCT flags
namespace fof {
    constexpr u16 FOF_SILENT         = 0x0004;
    constexpr u16 FOF_NOCONFIRMATION = 0x0010;
    constexpr u16 FOF_FILESONLY      = 0x0080;
}

// Error codes
namespace de {
    constexpr int NO_ERROR             = 0;
    constexpr int DE_SAMEFILE          = 0x71;
    constexpr int DE_OPCANCELLED       = 0x75;
    constexpr int DE_DESTSUBTREE       = 0x76;
    constexpr int DE_ACCESSDENIEDSRC   = 0x78;
    constexpr int DE_FILEDESTEXISTS    = 0x80;
    constexpr int DE_INVALIDFILES      = 0x7C;
}

/// 32-bit SHFILEOPSTRUCTW layout:
///   offset 0:  HWND hwnd (4 bytes)
///   offset 4:  UINT wFunc (4 bytes)
///   offset 8:  LPCWSTR pFrom (4 bytes)
///   offset 12: LPCWSTR pTo (4 bytes)
///   offset 16: FILEOP_FLAGS fFlags (2 bytes)
///   offset 18: (2 bytes padding)
///   offset 20: BOOL fAnyOperationsAborted (4 bytes)
///   offset 24: LPVOID hNameMappings (4 bytes)
///   offset 28: LPCWSTR lpszProgressTitle (4 bytes)
/// Total: 32 bytes

/// Parse a double-null-terminated wide string list from guest memory
static std::vector<std::wstring> parse_double_null_list(Marshaler& m, GuestAddr ptr) {
    std::vector<std::wstring> paths;
    if (ptr == 0) return paths;

    GuestAddr curr = ptr;
    while (true) {
        // Read UTF-16LE chars until we hit a null
        std::wstring path;
        while (true) {
            auto ch_result = m.read_u16(curr);
            if (!ch_result.is_ok()) break;
            u16 ch = ch_result.value();
            if (ch == 0) {
                curr += 2;
                break;
            }
            path += static_cast<wchar_t>(ch);
            curr += 2;
        }
        
        // Empty string means end of list (second null)
        if (path.empty()) break;
        paths.push_back(path);
    }
    return paths;
}

/// Convert wstring to UTF-8 string for VFS
static std::string wstring_to_vfs_path(const std::wstring& ws) {
    std::string result;
    result.reserve(ws.size());
    for (wchar_t wc : ws) {
        if (wc < 0x80) {
            result.push_back(static_cast<char>(wc));
        } else if (wc < 0x800) {
            result.push_back(static_cast<char>(0xC0 | (wc >> 6)));
            result.push_back(static_cast<char>(0x80 | (wc & 0x3F)));
        } else {
            result.push_back(static_cast<char>(0xE0 | (wc >> 12)));
            result.push_back(static_cast<char>(0x80 | ((wc >> 6) & 0x3F)));
            result.push_back(static_cast<char>(0x80 | (wc & 0x3F)));
        }
    }
    return result;
}

/// Copy a file using VFS (read + write)
static bool copy_file_vfs(capsule::Vfs& vfs, const std::string& src, const std::string& dst) {
    // Open source for reading
    auto src_handle = vfs.open(src, 
        capsule::AccessMode::Read, 
        capsule::ShareMode::Read, 
        capsule::CreationDisposition::OpenExisting);
    if (src_handle.is_err()) {
        VITREO_TRACE("[shell32] copy_file_vfs: failed to open src '%s'\n", src.c_str());
        return false;
    }

    // Open destination for writing
    auto dst_handle = vfs.open(dst,
        capsule::AccessMode::Write,
        capsule::ShareMode::None,
        capsule::CreationDisposition::CreateAlways);
    if (dst_handle.is_err()) {
        (void)vfs.close(src_handle.value());
        VITREO_TRACE("[shell32] copy_file_vfs: failed to create dst '%s'\n", dst.c_str());
        return false;
    }

    // Copy in chunks
    constexpr usize CHUNK_SIZE = 65536;
    std::vector<u8> buffer(CHUNK_SIZE);
    bool success = true;
    
    while (true) {
        auto read_result = vfs.read(src_handle.value(), buffer);
        if (read_result.is_err()) {
            success = false;
            break;
        }
        usize bytes_read = read_result.value();
        if (bytes_read == 0) break; // EOF
        
        auto write_result = vfs.write(dst_handle.value(), 
            std::span<const u8>(buffer.data(), bytes_read));
        if (write_result.is_err() || write_result.value() != bytes_read) {
            success = false;
            break;
        }
    }

    (void)vfs.close(src_handle.value());
    (void)vfs.close(dst_handle.value());
    
    if (!success) {
        // Clean up partial file on failure
        (void)vfs.remove(dst);
    }
    
    VITREO_TRACE("[shell32] copy_file_vfs: '%s' -> '%s' %s\n", 
                 src.c_str(), dst.c_str(), success ? "OK" : "FAILED");
    return success;
}

/// Check if path refers to a directory
static bool is_directory(capsule::Vfs& vfs, const std::string& path) {
    auto stat_result = vfs.stat(path);
    if (stat_result.is_err()) return false;
    return capsule::has_attribute(stat_result.value().attributes, 
                                   capsule::FileAttributes::Directory);
}

/// Extract filename from path
static std::string get_filename(const std::string& path) {
    auto pos = path.find_last_of("\\/");
    if (pos == std::string::npos) return path;
    return path.substr(pos + 1);
}

/// Combine base path and filename
static std::string combine_path(const std::string& base, const std::string& filename) {
    if (base.empty()) return filename;
    char last = base.back();
    if (last == '\\' || last == '/') return base + filename;
    return base + "\\" + filename;
}

/// Case-insensitive DOS glob over a single path component ('*' and '?').
static bool glob_match(const std::string& pattern, const std::string& name) {
    const char* p = pattern.c_str();
    const char* n = name.c_str();
    const char* star_p = nullptr;
    const char* star_n = nullptr;
    while (*n != '\0') {
        if (*p == '*') {
            star_p = p++;
            star_n = n;
        } else if (*p == '?' ||
                   std::tolower(static_cast<unsigned char>(*p)) ==
                   std::tolower(static_cast<unsigned char>(*n))) {
            ++p;
            ++n;
        } else if (star_p != nullptr) {
            p = star_p + 1;
            n = ++star_n;
        } else {
            return false;
        }
    }
    while (*p == '*') ++p;
    return *p == '\0';
}

static bool has_wildcard(const std::string& s) {
    return s.find('*') != std::string::npos || s.find('?') != std::string::npos;
}

/// The directory part of a path, without its trailing separator.
static std::string get_directory(const std::string& path) {
    const auto pos = path.find_last_of("\\/");
    if (pos == std::string::npos) return {};
    // "C:\file" keeps the root separator, otherwise the parent of a top-level
    // entry would come out as the bare drive letter.
    if (pos == 0 || (pos == 2 && path.size() > 2 && path[1] == ':')) {
        return path.substr(0, pos + 1);
    }
    return path.substr(0, pos);
}

bool ShellPathManager::ensure_directory(const std::string& path) {
    if (path.empty()) return false;
    if (is_directory(m_vfs, path)) return true;
    const std::string parent = get_directory(path);
    // A bare drive root ("C:\\") is not something the VFS creates.
    if (!parent.empty() && parent != path && parent.size() > 3) {
        (void)ensure_directory(parent);
    }
    return m_vfs.create_dir(path).is_ok() || is_directory(m_vfs, path);
}

bool ShellPathManager::copy_tree(const std::string& src, const std::string& dst) {
    if (!is_directory(m_vfs, src)) {
        return copy_file_vfs(m_vfs, src, dst);
    }
    if (!ensure_directory(dst)) {
        VITREO_TRACE("[shell32] copy_tree: failed to create dst dir '%s'\n",
                     dst.c_str());
        return false;
    }
    auto entries = m_vfs.list_dir(src);
    if (entries.is_err()) {
        VITREO_TRACE("[shell32] copy_tree: failed to list '%s'\n", src.c_str());
        return false;
    }
    bool ok = true;
    for (const auto& entry : entries.value()) {
        if (entry.name == "." || entry.name == "..") continue;
        if (!copy_tree(combine_path(src, entry.name),
                       combine_path(dst, entry.name))) {
            ok = false;
        }
    }
    return ok;
}

bool ShellPathManager::remove_tree(const std::string& path) {
    if (is_directory(m_vfs, path)) {
        auto entries = m_vfs.list_dir(path);
        if (entries.is_ok()) {
            for (const auto& entry : entries.value()) {
                if (entry.name == "." || entry.name == "..") continue;
                (void)remove_tree(combine_path(path, entry.name));
            }
        }
    }
    return m_vfs.remove(path).is_ok();
}

std::vector<std::string> ShellPathManager::expand_wildcard_sources(
    const std::vector<std::string>& sources, bool& had_wildcard) {
    had_wildcard = false;
    std::vector<std::string> expanded;
    expanded.reserve(sources.size());
    for (const auto& spec : sources) {
        if (!has_wildcard(spec)) {
            expanded.push_back(spec);
            continue;
        }
        had_wildcard = true;
        const std::string dir = get_directory(spec);
        const std::string pattern = get_filename(spec);
        // Only the leaf may be a pattern; Windows does not walk a wildcard
        // directory component either, so such a spec matches nothing.
        if (has_wildcard(dir) || dir.empty()) {
            VITREO_TRACE("[shell32] wildcard source '%s': no usable directory\n",
                         spec.c_str());
            continue;
        }
        auto entries = m_vfs.list_dir(dir);
        if (entries.is_err()) {
            VITREO_TRACE("[shell32] wildcard source '%s': cannot list '%s'\n",
                         spec.c_str(), dir.c_str());
            continue;
        }
        std::vector<std::string> matches;
        for (const auto& entry : entries.value()) {
            if (entry.name == "." || entry.name == "..") continue;
            if (glob_match(pattern, entry.name)) {
                matches.push_back(combine_path(dir, entry.name));
            }
        }
        // Directory order is filesystem-defined; a stable order keeps the
        // operation reproducible for a caller watching progress.
        std::sort(matches.begin(), matches.end());
        VITREO_TRACE("[shell32] wildcard source '%s' -> %zu match(es)\n",
                     spec.c_str(), matches.size());
        expanded.insert(expanded.end(), matches.begin(), matches.end());
    }
    return expanded;
}

int ShellPathManager::copy_operation(
    const std::vector<std::string>& source_specs,
    const std::vector<std::string>& destinations) {
    if (source_specs.empty() || destinations.empty()) {
        return de::DE_INVALIDFILES;
    }

    bool had_wildcard = false;
    const std::vector<std::string> sources =
        expand_wildcard_sources(source_specs, had_wildcard);
    if (sources.empty()) {
        return de::DE_INVALIDFILES;
    }

    std::string dest_base = destinations[0];
    bool dest_is_dir = is_directory(m_vfs, dest_base);
    // A wildcard source names a set, so the destination is the directory the
    // set lands in even when it expanded to a single entry.
    if (!dest_is_dir && (sources.size() > 1 || had_wildcard)) {
        dest_is_dir = ensure_directory(dest_base);
    }

    int error_code = de::NO_ERROR;
    for (const auto& src_path : sources) {
        std::string dst_path;
        if (dest_is_dir) {
            dst_path = combine_path(dest_base, get_filename(src_path));
        } else {
            dst_path = dest_base;
        }
        if (src_path == dst_path) continue;
        if (!copy_tree(src_path, dst_path)) {
            error_code = de::DE_ACCESSDENIEDSRC;
        }
    }
    return error_code;
}

int ShellPathManager::file_operation(Marshaler& m, GuestAddr file_op_ptr) {
    if (file_op_ptr == 0) {
        VITREO_TRACE("[shell32] SHFileOperationW: null struct pointer\n");
        return de::DE_INVALIDFILES;
    }

    // Read SHFILEOPSTRUCTW fields
    auto wfunc_r = m.read_u32(file_op_ptr + 4);
    auto pfrom_r = m.read_u32(file_op_ptr + 8);
    auto pto_r   = m.read_u32(file_op_ptr + 12);
    auto fflags_r = m.read_u16(file_op_ptr + 16);
    
    if (!wfunc_r.is_ok() || !pfrom_r.is_ok() || !pto_r.is_ok() || !fflags_r.is_ok()) {
        VITREO_TRACE("[shell32] SHFileOperationW: failed to read struct\n");
        return de::DE_INVALIDFILES;
    }

    u32 wfunc  = wfunc_r.value();
    u32 pfrom  = pfrom_r.value();
    u32 pto    = pto_r.value();
    u16 fflags = fflags_r.value();

    // Parse pFrom (double-null-terminated list)
    std::vector<std::wstring> from_paths = parse_double_null_list(m, pfrom);
    
    // Parse pTo (double-null-terminated list, may be empty for FO_DELETE)
    std::vector<std::wstring> to_paths = parse_double_null_list(m, pto);

    VITREO_TRACE("[shell32] SHFileOperationW: wFunc=0x%x fFlags=0x%04x from_count=%zu to_count=%zu\n",
                 wfunc, fflags, from_paths.size(), to_paths.size());

    if (from_paths.empty()) {
        VITREO_TRACE("[shell32] SHFileOperationW: no source files\n");
        return de::DE_INVALIDFILES;
    }

    // Log paths for debugging
    for (const auto& p : from_paths) {
        std::string narrow = wstring_to_vfs_path(p);
        VITREO_TRACE("[shell32]   pFrom: '%s'\n", narrow.c_str());
    }
    for (const auto& p : to_paths) {
        std::string narrow = wstring_to_vfs_path(p);
        VITREO_TRACE("[shell32]   pTo: '%s'\n", narrow.c_str());
    }

    bool any_aborted = false;
    int error_code = de::NO_ERROR;

    // Every source may be a wildcard pattern; wFunc decides what to do with
    // the expansion, but the expansion itself is the same for all of them.
    std::vector<std::string> source_specs;
    source_specs.reserve(from_paths.size());
    for (const auto& from_w : from_paths) {
        source_specs.push_back(wstring_to_vfs_path(from_w));
    }

    switch (wfunc) {
        case fo::FO_COPY: {
            const std::vector<std::string>& sources = source_specs;
            std::vector<std::string> dests;
            dests.reserve(to_paths.size());
            for (const auto& to_w : to_paths) {
                dests.push_back(wstring_to_vfs_path(to_w));
            }
            error_code = copy_operation(sources, dests);
            if (error_code != de::NO_ERROR) any_aborted = true;
            break;
        }

        case fo::FO_MOVE: {
            if (to_paths.empty()) {
                VITREO_TRACE("[shell32] SHFileOperationW FO_MOVE: no destination\n");
                error_code = de::DE_INVALIDFILES;
                break;
            }

            bool had_wildcard = false;
            const std::vector<std::string> sources =
                expand_wildcard_sources(source_specs, had_wildcard);
            if (sources.empty()) {
                VITREO_TRACE("[shell32] FO_MOVE: no source matched\n");
                error_code = de::DE_INVALIDFILES;
                any_aborted = true;
                break;
            }

            const std::string dest_base = wstring_to_vfs_path(to_paths[0]);
            bool dest_is_dir = is_directory(m_vfs, dest_base);
            // A wildcard source names a set, so the destination is the
            // directory that set lands in even when it expanded to one entry.
            if (!dest_is_dir && (sources.size() > 1 || had_wildcard)) {
                dest_is_dir = ensure_directory(dest_base);
            }

            for (const auto& src_path : sources) {
                const std::string dst_path =
                    dest_is_dir ? combine_path(dest_base, get_filename(src_path))
                                : dest_base;

                if (src_path == dst_path) {
                    VITREO_TRACE("[shell32] FO_MOVE: skipping same file '%s'\n",
                                 src_path.c_str());
                    continue;
                }

                auto result = m_vfs.rename(src_path, dst_path);
                if (result.is_ok()) {
                    VITREO_TRACE("[shell32] FO_MOVE: '%s' -> '%s' OK\n",
                                 src_path.c_str(), dst_path.c_str());
                    continue;
                }
                VITREO_TRACE("[shell32] FO_MOVE: rename failed '%s' -> '%s'\n",
                             src_path.c_str(), dst_path.c_str());
                // Copy+delete fallback: rename refuses a cross-filesystem move,
                // and also a move onto an existing directory, which is exactly
                // what an installer does when it merges a staged tree into an
                // install directory that already has some of those folders.
                if (copy_tree(src_path, dst_path) && remove_tree(src_path)) {
                    VITREO_TRACE("[shell32] FO_MOVE: copied '%s' -> '%s'\n",
                                 src_path.c_str(), dst_path.c_str());
                } else {
                    any_aborted = true;
                    error_code = de::DE_ACCESSDENIEDSRC;
                }
            }
            break;
        }

        case fo::FO_DELETE: {
            bool had_wildcard = false;
            const std::vector<std::string> targets =
                expand_wildcard_sources(source_specs, had_wildcard);
            if (targets.empty()) {
                // A wildcard that matched nothing has nothing to delete; an
                // explicit path that does not exist is still an error.
                if (had_wildcard) break;
                error_code = de::DE_INVALIDFILES;
                any_aborted = true;
                break;
            }
            for (const auto& path : targets) {
                // FO_DELETE on a directory removes what is inside it too;
                // VFS remove() alone only takes an empty directory.
                if (!remove_tree(path)) {
                    VITREO_TRACE("[shell32] FO_DELETE: failed to remove '%s'\n",
                                 path.c_str());
                    any_aborted = true;
                    error_code = de::DE_ACCESSDENIEDSRC;
                } else {
                    VITREO_TRACE("[shell32] FO_DELETE: removed '%s'\n",
                                 path.c_str());
                }
            }
            break;
        }

        case fo::FO_RENAME: {
            if (from_paths.size() != 1 || to_paths.empty()) {
                VITREO_TRACE("[shell32] SHFileOperationW FO_RENAME: invalid params\n");
                error_code = de::DE_INVALIDFILES;
                break;
            }
            
            std::string src_path = wstring_to_vfs_path(from_paths[0]);
            std::string dst_path = wstring_to_vfs_path(to_paths[0]);
            
            auto result = m_vfs.rename(src_path, dst_path);
            if (result.is_err()) {
                VITREO_TRACE("[shell32] FO_RENAME: failed '%s' -> '%s'\n",
                             src_path.c_str(), dst_path.c_str());
                any_aborted = true;
                error_code = de::DE_ACCESSDENIEDSRC;
            } else {
                VITREO_TRACE("[shell32] FO_RENAME: '%s' -> '%s' OK\n",
                             src_path.c_str(), dst_path.c_str());
            }
            break;
        }

        default:
            VITREO_TRACE("[shell32] SHFileOperationW: unknown wFunc=0x%x\n", wfunc);
            error_code = de::DE_INVALIDFILES;
            break;
    }

    // Write fAnyOperationsAborted (offset 20)
    (void)m.write_u32(file_op_ptr + 20, any_aborted ? 1 : 0);

    VITREO_TRACE("[shell32] SHFileOperationW: returning %d, aborted=%d\n", 
                 error_code, any_aborted ? 1 : 0);
    return error_code;
}

u64 ShellPathManager::get_file_info(Marshaler& m) const {
    // DWORD_PTR SHGetFileInfoW(LPCWSTR pszPath, DWORD dwFileAttributes,
    //                          SHFILEINFOW* psfi, UINT cbFileInfo, UINT uFlags)
    // https://learn.microsoft.com/en-us/windows/win32/api/shellapi/nf-shellapi-shgetfileinfow
    //
    // Measured on Windows 11 (tools/oracle_probe/s7_shell_probe.expected):
    //  - SHFILEINFOW is {HICON hIcon; int iIcon; DWORD dwAttributes;
    //    WCHAR szDisplayName[260]; WCHAR szTypeName[80]}: fields at
    //    0/8/12/16/536 on x64 (sizeof 696), 0/4/8/12/532 on x86 (692).
    //  - hIcon, szDisplayName[0] and szTypeName[0] are cleared on every call;
    //    iIcon and dwAttributes are only written when asked for. cbFileInfo is
    //    not checked.
    //  - Success sets the last error to 0; a missing path returns 0 with the
    //    file-system error; psfi NULL returns 0 with the last error untouched.
    //  - SHGFI_EXETYPE returns MAKELONG('PE', subsystem version) for GUI
    //    images, 'PE' alone for console images, 0 for non-executables.
    const GuestAddr path_addr = m.arg_ptr(0);
    const u32 file_attributes = m.arg32(1);
    const GuestAddr psfi = m.arg_ptr(2);
    const u32 flags = m.arg32(4);
    const bool is64 = m.cpu_is_64bit();

    constexpr u32 SHGFI_ICON = 0x100, SHGFI_DISPLAYNAME = 0x200,
                  SHGFI_TYPENAME = 0x400, SHGFI_ATTRIBUTES = 0x800,
                  SHGFI_ICONLOCATION = 0x1000, SHGFI_EXETYPE = 0x2000,
                  SHGFI_SYSICONINDEX = 0x4000, SHGFI_PIDL = 0x8,
                  SHGFI_USEFILEATTRIBUTES = 0x10;
    constexpr u32 FILE_ATTRIBUTE_DIRECTORY = 0x10, FILE_ATTRIBUTE_READONLY = 0x1,
                  FILE_ATTRIBUTE_HIDDEN = 0x2;
    // SFGAO sets the shell namespace reports for plain file-system items
    // (measured: a writable file 0x40400177, a folder 0xF080017F).
    constexpr u32 kFileSfgao = 0x40400177u;
    constexpr u32 kFolderSfgao = 0xF080017Fu;
    constexpr u32 SFGAO_READONLY = 0x00040000u, SFGAO_HIDDEN = 0x00080000u;

    std::string path;
    if ((flags & SHGFI_PIDL) != 0) {
        if (!pidl_path(path_addr, path)) {
            set_last_error(error_codes::ERROR_INVALID_PARAMETER);
            return 0;
        }
    } else if (path_addr != 0) {
        path = m.read_wide_string(path_addr).value_or("");
    }

    if ((flags & SHGFI_EXETYPE) != 0) {
        // EXETYPE is exclusive of the other flags and never touches psfi.
        std::vector<u8> image(4096, 0);
        {
            auto file = m_vfs.open(path, capsule::AccessMode::Read,
                                   capsule::ShareMode::All,
                                   capsule::CreationDisposition::OpenExisting);
            if (file.is_err()) return 0;
            auto got = m_vfs.read(file.value(), std::span<u8>(image));
            (void)m_vfs.close(file.value());
            if (got.is_err()) return 0;
            image.resize(got.value());
        }
        auto u16_at = [&](usize at) -> u32 {
            return at + 2 <= image.size()
                ? static_cast<u32>(image[at] | (image[at + 1] << 8)) : 0;
        };
        auto u32_at = [&](usize at) -> u32 {
            return at + 4 <= image.size()
                ? u16_at(at) | (u16_at(at + 2) << 16) : 0;
        };
        if (u16_at(0) != 0x5A4D) return 0;                // 'MZ'
        const u32 pe = u32_at(0x3C);
        if (u32_at(pe) != 0x00004550) return 0x5A4D;       // plain MS-DOS
        const usize optional = pe + 24;
        const u32 subsystem = u16_at(optional + 68);
        constexpr u32 IMAGE_SUBSYSTEM_WINDOWS_CUI = 3;
        set_last_error(error_codes::ERROR_SUCCESS);
        if (subsystem == IMAGE_SUBSYSTEM_WINDOWS_CUI) return 0x4550;
        const u32 version = (u16_at(optional + 48) << 8) | (u16_at(optional + 50) & 0xFF);
        return 0x4550 | (static_cast<u64>(version) << 16);
    }

    if ((flags & (SHGFI_ICON | SHGFI_SYSICONINDEX | SHGFI_ICONLOCATION)) != 0) {
        // The shell's icon for a file type has no host counterpart Vitreo can
        // hand out yet; failing is honest where an invented HICON is not.
        static std::once_flag once;
        std::call_once(once, [] {
            std::fprintf(stderr, "[vitreo] UNIMPLEMENTED shell32!SHGetFileInfoW: "
                                 "SHGFI_ICON/SYSICONINDEX/ICONLOCATION\n");
        });
        set_last_error(error_codes::ERROR_NOT_SUPPORTED);
        return 0;
    }

    if (psfi == 0) return 0;

    // Resolve the item: USEFILEATTRIBUTES takes the caller's attributes and
    // never touches the file system.
    const bool use_attributes = (flags & SHGFI_USEFILEATTRIBUTES) != 0;
    u32 attributes = file_attributes;
    if (!use_attributes) {
        auto info = m_vfs.stat(path);
        if (path.empty() || info.is_err()) {
            const auto slash = path.find_last_of("\\/");
            const bool parent_exists =
                slash != std::string::npos &&
                m_vfs.stat(path.substr(0, slash + 1)).is_ok();
            set_last_error(parent_exists ? error_codes::ERROR_FILE_NOT_FOUND
                                         : error_codes::ERROR_PATH_NOT_FOUND);
            return 0;
        }
        attributes = static_cast<u32>(info.value().attributes);
    }
    const bool directory = (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;

    const u32 ptr = is64 ? 8u : 4u;
    const GuestAddr icon_at = psfi, iicon_at = psfi + ptr,
                    attributes_at = psfi + ptr + 4,
                    display_at = psfi + ptr + 8,
                    type_at = display_at + 260 * 2;
    (void)m.write_ptr(icon_at, 0);
    (void)m.write_u16(display_at, 0);
    (void)m.write_u16(type_at, 0);
    (void)iicon_at;

    auto write_wide = [&](GuestAddr at, std::string_view text, usize capacity) {
        const std::u16string units = utf8_to_utf16(text);
        const usize count = std::min(units.size(), capacity - 1);
        for (usize i = 0; i < count; ++i) (void)m.write_u16(at + i * 2, units[i]);
        (void)m.write_u16(at + count * 2, 0);
    };

    std::string leaf = path;
    while (leaf.size() > 3 && (leaf.back() == '\\' || leaf.back() == '/')) leaf.pop_back();
    if (const auto slash = leaf.find_last_of("\\/");
        slash != std::string::npos && leaf.size() > 3) {
        leaf = leaf.substr(slash + 1);
    }

    if ((flags & SHGFI_DISPLAYNAME) != 0) write_wide(display_at, leaf, 260);

    if ((flags & SHGFI_TYPENAME) != 0) {
        std::string type_name;
        const auto dot = leaf.rfind('.');
        std::string ext = (!directory && dot != std::string::npos) ? leaf.substr(dot) : "";
        for (auto& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (directory) type_name = "File folder";
        else if (ext == ".exe") type_name = "Application";
        else if (ext == ".txt" || ext == ".log") type_name = "Text Document";
        else if (ext == ".ini") type_name = "Configuration settings";
        else if (ext.size() > 1) {
            // No registered file type: "<EXT> File" (measured "ZZQ File").
            type_name = ext.substr(1);
            for (auto& c : type_name) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            type_name += " File";
        } else type_name = "File";
        write_wide(type_at, type_name, 80);
    }

    if ((flags & SHGFI_ATTRIBUTES) != 0) {
        u32 sfgao = 0;
        if (!use_attributes) {
            sfgao = directory ? kFolderSfgao : kFileSfgao;
            if ((attributes & FILE_ATTRIBUTE_READONLY) != 0 && !directory) sfgao |= SFGAO_READONLY;
            if ((attributes & FILE_ATTRIBUTE_HIDDEN) != 0) sfgao |= SFGAO_HIDDEN;
        }
        (void)m.write_u32(attributes_at, sfgao);
    }

    set_last_error(error_codes::ERROR_SUCCESS);
    return 1;
}

u64 ShellPathManager::get_file_info_ansi(Marshaler& m) {
    const GuestAddr path_ptr = m.arg_ptr(0);
    const u32 file_attributes = static_cast<u32>(m.arg(1));
    const GuestAddr output = m.arg_ptr(2);
    const u32 output_size = static_cast<u32>(m.arg(3));
    const u32 flags = static_cast<u32>(m.arg(4));

    constexpr u32 shgfi_display_name = 0x200;
    constexpr u32 shgfi_type_name = 0x400;
    constexpr u32 shgfi_attributes = 0x800;
    constexpr u32 shgfi_pidl = 0x8;
    constexpr u32 shgfi_use_file_attributes = 0x10;
    constexpr u32 shgfi_system_icon_index = 0x4000;
    constexpr u32 file_attribute_directory = 0x10;
    constexpr u32 sfgao_file_system = 0x40000000;
    constexpr u32 structure_size = 352;

    if (output == 0 || output_size < structure_size || path_ptr == 0) return 0;

    std::string path;
    if ((flags & shgfi_pidl) != 0) {
        if (!pidl_path(path_ptr, path)) return 0;
    } else {
        path = m.read_ansi_string(path_ptr).value_or("");
        if (path.empty()) return 0;
    }

    std::vector<Byte> zeroes(structure_size, 0);
    (void)m.memory().write_bytes(output, zeroes.data(), zeroes.size());

    auto write_ansi = [&](GuestAddr address, const std::string& value, usize capacity) {
        std::vector<Byte> bytes(capacity, 0);
        std::memcpy(bytes.data(), value.data(), std::min(value.size(), capacity - 1));
        (void)m.memory().write_bytes(address, bytes.data(), bytes.size());
    };

    if ((flags & shgfi_display_name) != 0) {
        const usize slash = path.find_last_of("\\/");
        const std::string display_name = slash == std::string::npos ? path : path.substr(slash + 1);
        write_ansi(output + 12, display_name, 260);
    }
    if ((flags & shgfi_type_name) != 0) {
        const bool directory = ((flags & shgfi_use_file_attributes) != 0 &&
                                (file_attributes & file_attribute_directory) != 0) ||
                               m_vfs.list_dir(path).is_ok();
        write_ansi(output + 272, directory ? "File folder" : "File", 80);
    }
    if ((flags & shgfi_attributes) != 0) {
        (void)m.write_u32(output + 8, sfgao_file_system);
    }

    VITREO_TRACE("[shell32] SHGetFileInfoA: path='%s' flags=0x%x\n",
                 path.c_str(), flags);
    if ((flags & shgfi_system_icon_index) != 0) {
        if (m_comctl32_manager == nullptr) return 0;
        if (m_system_image_list == 0) {
            constexpr u32 ilc_mask = 0x1;
            constexpr u32 ilc_color8 = 0x8;
            m_system_image_list = m_comctl32_manager->create_image_list(
                16, 16, ilc_mask | ilc_color8, 1, 1);
        }
        return m_system_image_list;
    }
    return 1;
}

u32 ShellPathManager::get_special_folder_location(i32 csidl, GuestAddr& pidl, bool is64) {
    std::string path;
    if (csidl == csidl::CSIDL_DRIVES) {
        path = "shell:Drives";
    } else {
        std::wstring wide_path;
        u32 result = get_folder_path(csidl, wide_path, false, is64);
        if (result != hr::S_OK) {
            VITREO_TRACE("[shell32] SHGetSpecialFolderLocation: unknown csidl=0x%x\n", csidl);
            return hr::E_INVALIDARG;
        }
        path.assign(wide_path.begin(), wide_path.end());
    }

    GuestAddr addr = create_pidl(path);
    if (addr == 0) return hr::E_OUTOFMEMORY;
    pidl = addr;

    VITREO_TRACE( "[shell32] SHGetSpecialFolderLocation: csidl=0x%x path='%s' pidl=0x%llx\n",
                 csidl, path.c_str(), (unsigned long long)addr);
    return hr::S_OK;
}


// =============================================================================
// Thunk Registration
// =============================================================================

} // namespace vitreo::winapi

// =============================================================================
// HDROP (DragQueryPoint / DragQueryFileA / DragQueryFileW / DragFinish)
// =============================================================================

namespace vitreo::winapi {
namespace {

/// The contents of a DROPFILES block, file names as UTF-16 units.
struct DropFiles {
    i32 x = 0;
    i32 y = 0;
    bool non_client = false;
    std::vector<std::u16string> files;
};

/// Reads the DROPFILES an HDROP holds; nullopt for a handle that does not
/// lock (NULL or freed). DROPFILES is 20 bytes in both bitnesses: DWORD
/// pFiles, POINT pt, BOOL fNC, BOOL fWide.
std::optional<DropFiles> read_drop_files(Marshaler& m, GlobalLocalManager& memory,
                                         u64 hdrop) {
    if (hdrop == 0) return std::nullopt;
    const GuestAddr base = static_cast<GuestAddr>(memory.global_lock(hdrop));
    if (base == 0) return std::nullopt;
    const u64 size = memory.global_size(hdrop);
    DropFiles out;
    const u32 files_offset = m.read_u32(base).value_or(0);
    out.x = static_cast<i32>(m.read_u32(base + 4).value_or(0));
    out.y = static_cast<i32>(m.read_u32(base + 8).value_or(0));
    out.non_client = m.read_u32(base + 12).value_or(0) != 0;
    const bool wide = m.read_u32(base + 16).value_or(0) != 0;
    const u32 ansi_page = codepage::ansi_code_page();
    u64 offset = files_offset;
    while (offset < size) {
        std::u16string name;
        if (wide) {
            while (offset + 1 < size) {
                const u16 unit = m.read_u16(base + offset).value_or(0);
                offset += 2;
                if (unit == 0) break;
                name.push_back(static_cast<char16_t>(unit));
            }
        } else {
            std::vector<u8> bytes;
            while (offset < size) {
                const u8 byte = m.read_u8(base + offset).value_or(0);
                ++offset;
                if (byte == 0) break;
                bytes.push_back(byte);
            }
            if (!bytes.empty()) {
                std::vector<u16> units(bytes.size());
                const auto converted = codepage::multi_byte_to_wide(
                    ansi_page, 0, bytes.data(), bytes.size(), units.data(), units.size());
                name.assign(units.begin(), units.begin() + converted.count);
            }
        }
        if (name.empty()) break;  // the list ends with an empty string
        out.files.push_back(std::move(name));
    }
    (void)memory.global_unlock(hdrop);
    return out;
}

}  // namespace

u32 ShellPathManager::drag_query_point(Marshaler& m, u64 hdrop, GuestAddr point) {
    if (m_local_memory == nullptr) return 0;
    const auto drop = read_drop_files(m, *m_local_memory, hdrop);
    if (!drop) return 0;  // measured: point and last error untouched
    if (point != 0) {
        (void)m.write_u32(point, static_cast<u32>(drop->x));
        (void)m.write_u32(point + 4, static_cast<u32>(drop->y));
    }
    set_last_error(0);  // measured: 0 after a successful query
    return drop->non_client ? 0u : 1u;
}

u32 ShellPathManager::drag_query_file(Marshaler& m, u64 hdrop, u32 index,
                                      GuestAddr buffer, u32 cch, bool wide) {
    if (m_local_memory == nullptr) return 0;
    const auto drop = read_drop_files(m, *m_local_memory, hdrop);
    if (!drop) return 0;
    if (index == 0xFFFFFFFFu) return static_cast<u32>(drop->files.size());
    if (index >= drop->files.size()) return 0;  // measured: buffer untouched
    const std::u16string& name = drop->files[index];
    if (wide) {
        if (buffer == 0) return static_cast<u32>(name.size());
        if (cch == 0) return 0;
        const u32 copied = static_cast<u32>(std::min<usize>(name.size(), cch - 1));
        for (u32 i = 0; i < copied; ++i) (void)m.write_u16(buffer + i * 2, name[i]);
        (void)m.write_u16(buffer + copied * 2, 0);
        return copied;
    }
    // ANSI: the name in the process code page; "characters" are bytes.
    std::vector<u8> bytes(name.size() * 4 + 1);
    const auto converted = codepage::wide_to_multi_byte(
        codepage::ansi_code_page(), 0, reinterpret_cast<const u16*>(name.data()),
        name.size(), bytes.data(), bytes.size(), nullptr, false);
    bytes.resize(converted.count);
    if (buffer == 0) return static_cast<u32>(bytes.size());
    if (cch == 0) return 0;
    const u32 copied = static_cast<u32>(std::min<usize>(bytes.size(), cch - 1));
    for (u32 i = 0; i < copied; ++i) (void)m.write_u8(buffer + i, bytes[i]);
    (void)m.write_u8(buffer + copied, 0);
    return copied;
}

void ShellPathManager::drag_finish(u64 hdrop) {
    if (m_local_memory != nullptr && hdrop != 0) (void)m_local_memory->global_free(hdrop);
}

}  // namespace vitreo::winapi
