// SPDX-License-Identifier: LGPL-2.1-only

/// @file process_ext.cpp
/// @brief Launching a guest process and reporting on the ones already running.
///
/// CreateProcess in all its forms, LoadModule, and the queries around process
/// identity and exit. process.cpp owns the process table; this is the surface
/// that puts things into it.
///
/// The four helpers below travelled with it. They are what CreateProcess does
/// before it launches anything: resolve the command line to a real capsule
/// path, read the requested execution level out of the target's manifest, parse
/// the environment block, and work out a display name. None is used anywhere
/// else, so they stay file-local here rather than becoming header entries.
///
/// CreateProcessA and LoadModule were 4,000 lines away in a run named
/// "Additional Win32 stubs" — gathered in, because process creation belongs
/// with process creation.

#include "winapi/kernel32/kernel32_ext.hpp"
#include "winapi/kernel32/console/console.hpp"

#include <algorithm>
#include "winapi/kernel32/kernel32_ext_internal.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/kernel32/handle.hpp"
#include "winapi/kernel32/filesystem/file.hpp"
#include "winapi/kernel32/memory/file_mapping.hpp"
#include "winapi/common/elevation.hpp"
#include "platform/macos/elevation_authorization.hpp"
#include "winapi/kernel32/process/process.hpp"
#include "winapi/kernel32/thread/thread.hpp"
#include "winapi/kernel32/module/module.hpp"
#include "winapi/kernel32/environment/environment.hpp"
#include "winapi/kernel32/synchronization/sync.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/advapi32/security/security.hpp"
#include "capsule/vfs/vfs.hpp"
#include "core/memory/guest_memory.hpp"
#include "core/pe/parser/pe_parser.hpp"
#include "core/pe/parser/manifest_info.hpp"
#include "winapi/kernel32/locale/string_conv.hpp"
#include "winapi/kernel32/locale/codepage_convert.hpp"
#include "ipc/ipc_gate.hpp"
#include "ipc/process_handle.hpp"
#include "ipc/hwnd_registry.hpp"

#include <cstring>
#include <mutex>
#include <string>
#include <utility>
#include <fstream>
#include <vector>
#include <unistd.h>

namespace vitreo::winapi {

namespace {

/// Held by CreateProcess from preparing the inheritable handles until the
/// child is spawned and their FD_CLOEXEC is restored. Inheritance works by
/// clearing FD_CLOEXEC on process-wide descriptors, so two concurrent
/// CreateProcess calls (Chromium launches its GPU and utility processes from
/// different threads) would otherwise restore each other's descriptors before
/// the second spawn, and each child would inherit the other's handles. On
/// Windows the handle set a child inherits is fixed by its own CreateProcess.
std::mutex g_inherited_handle_spawn_mutex;

[[nodiscard]] bool createprocess_has_directory(const std::string& name) {
    return name.find_first_of("\\/") != std::string::npos;
}

/// Learn CreateProcessW: append .exe when lpApplicationName is NULL and the
/// module token has no extension, unless it ends in '.' or contains a path.
[[nodiscard]] std::string createprocess_maybe_append_exe(
    const std::string& name) {
    if (name.empty() || createprocess_has_directory(name)) return name;
    if (name.back() == '.') return name;
    const usize slash = name.find_last_of("\\/");
    const usize base = slash == std::string::npos ? 0 : slash + 1;
    const usize dot = name.find('.', base);
    if (dot != std::string::npos) return name;
    return name + ".exe";
}

[[nodiscard]] std::string createprocess_parent_dir(std::string_view path) {
    const usize slash = path.find_last_of("\\/");
    if (slash == std::string::npos) return {};
    return std::string(path.substr(0, slash));
}

[[nodiscard]] std::string createprocess_join_dir(const std::string& dir,
                                                 const std::string& name) {
    if (dir.empty()) return name;
    if (dir.back() == '\\' || dir.back() == '/') return dir + name;
    return dir + "\\" + name;
}

[[nodiscard]] std::vector<std::string> createprocess_split_path(
    const std::string& path_env) {
    std::vector<std::string> dirs;
    usize start = 0;
    while (start <= path_env.size()) {
        const usize semi = path_env.find(';', start);
        const usize end = semi == std::string::npos ? path_env.size() : semi;
        if (end > start) {
            dirs.emplace_back(path_env.substr(start, end - start));
        }
        if (semi == std::string::npos) break;
        start = semi + 1;
    }
    return dirs;
}

[[nodiscard]] bool createprocess_guest_file_exists(capsule::Vfs& vfs,
                                                   const std::string& path) {
    auto opened = vfs.open(path, capsule::AccessMode::Read,
                           capsule::ShareMode::Read,
                           capsule::CreationDisposition::OpenExisting);
    if (opened.is_err()) return false;
    (void)vfs.close(opened.value());
    return true;
}

} // namespace

Result<std::string, capsule::VfsError> materialize_guest_executable(
    capsule::Vfs& vfs, const std::string& guest_path) {
    auto opened = vfs.open(guest_path, capsule::AccessMode::Read,
                           capsule::ShareMode::Read,
                           capsule::CreationDisposition::OpenExisting);
    if (opened.is_err()) {
        return Result<std::string, capsule::VfsError>::err(opened.error());
    }

    const char* tmpdir = ::getenv("TMPDIR");
    if (!tmpdir || !*tmpdir) tmpdir = "/tmp";
    std::string pattern = std::string(tmpdir) + "/vitreo_vfs_launch_XXXXXX.exe";
    std::vector<char> path(pattern.begin(), pattern.end());
    path.push_back('\0');
    const int fd = ::mkstemps(path.data(), 4);
    if (fd < 0) {
        (void)vfs.close(opened.value());
        return Result<std::string, capsule::VfsError>::err(capsule::VfsError::IoError);
    }

    std::array<u8, 64 * 1024> buffer{};
    bool ok = true;
    while (ok) {
        auto read = vfs.read(opened.value(), buffer);
        if (read.is_err()) {
            ok = false;
            break;
        }
        if (read.value() == 0) break;
        usize offset = 0;
        while (offset < read.value()) {
            const ssize_t written = ::write(fd, buffer.data() + offset,
                                            read.value() - offset);
            if (written < 0) {
                if (errno == EINTR) continue;
                ok = false;
                break;
            }
            offset += static_cast<usize>(written);
        }
    }
    (void)vfs.close(opened.value());
    ::close(fd);
    if (!ok) {
        ::unlink(path.data());
        return Result<std::string, capsule::VfsError>::err(capsule::VfsError::ReadError);
    }
    return Result<std::string, capsule::VfsError>::ok(std::string(path.data()));
}

/// Read a CreateProcess environment block from guest memory into NAME=VALUE
/// entries suitable for `--env`. Unicode blocks are UTF-16LE; ANSI blocks are
/// treated as bytes (Windows ACP ≈ Latin-1 for installer paths we care about).
[[nodiscard]] std::vector<std::string> read_createprocess_environment_block(
    GuestMemory& memory, GuestAddr block, bool unicode) {
    std::vector<std::string> entries;
    if (block == 0) {
        return entries;
    }
    constexpr usize kMaxBlockBytes = 64u * 1024u;
    if (unicode) {
        std::vector<u16> units;
        units.reserve(256);
        for (usize i = 0; i + 1 < kMaxBlockBytes; i += 2) {
            const auto lo = memory.read_u8(block + i);
            const auto hi = memory.read_u8(block + i + 1);
            if (lo.is_err() || hi.is_err()) {
                break;
            }
            const u16 unit = static_cast<u16>(
                lo.value() | (static_cast<u16>(hi.value()) << 8));
            if (unit == 0) {
                if (units.empty()) {
                    break; // double-NUL terminator
                }
                const std::string entry = utf16_to_utf8(
                    units.data(), static_cast<int>(units.size()));
                if (entry.find('=') != std::string::npos) {
                    entries.push_back(entry);
                }
                units.clear();
                continue;
            }
            units.push_back(unit);
        }
        return entries;
    }

    std::string entry;
    entry.reserve(64);
    for (usize i = 0; i < kMaxBlockBytes; ++i) {
        const auto b = memory.read_u8(block + i);
        if (b.is_err()) {
            break;
        }
        if (b.value() == 0) {
            if (entry.empty()) {
                break;
            }
            if (entry.find('=') != std::string::npos) {
                entries.push_back(std::move(entry));
            }
            entry.clear();
            continue;
        }
        entry.push_back(static_cast<char>(b.value()));
    }
    return entries;
}

Result<std::pair<std::string, bool>, capsule::VfsError> resolve_guest_executable(
    capsule::Vfs& vfs, const std::string& guest_path) {
    // resolve_host_path always returns a mapped string; require the guest file
    // to actually exist before treating it as a launchable host path.
    if (!createprocess_guest_file_exists(vfs, guest_path)) {
        return Result<std::pair<std::string, bool>, capsule::VfsError>::err(
            capsule::VfsError::FileNotFound);
    }
    auto host_path = vfs.resolve_host_path(guest_path);
    if (host_path.is_ok() &&
        ::access(host_path.value().c_str(), R_OK) == 0) {
        return Result<std::pair<std::string, bool>, capsule::VfsError>::ok(
            std::make_pair(std::move(host_path.value()), false));
    }
    auto materialized = materialize_guest_executable(vfs, guest_path);
    if (materialized.is_err()) {
        return Result<std::pair<std::string, bool>, capsule::VfsError>::err(
            materialized.error());
    }
    return Result<std::pair<std::string, bool>, capsule::VfsError>::ok(
        std::make_pair(std::move(materialized.value()), true));
}

pe::RequestedExecutionLevel requested_execution_level(
    const std::string& host_path) {
    std::ifstream input(host_path, std::ios::binary);
    if (!input) return pe::RequestedExecutionLevel::Unspecified;
    input.seekg(0, std::ios::end);
    const auto end = input.tellg();
    if (end <= 0) return pe::RequestedExecutionLevel::Unspecified;
    input.seekg(0, std::ios::beg);
    std::vector<u8> bytes(static_cast<usize>(end));
    input.read(
        reinterpret_cast<char*>(bytes.data()),
        static_cast<std::streamsize>(bytes.size()));
    if (!input) return pe::RequestedExecutionLevel::Unspecified;
    auto parsed = pe::parse_pe(bytes);
    if (parsed.is_err()) {
        return pe::RequestedExecutionLevel::Unspecified;
    }
    return pe::pe_requested_execution_level(bytes, parsed.value());
}

std::string executable_display_name(const std::string& guest_path) {
    const usize slash = guest_path.find_last_of("\\/");
    const usize start = slash == std::string::npos ? 0 : slash + 1;
    std::string name = guest_path.substr(start);
    if (name.size() >= 4) {
        std::string extension = name.substr(name.size() - 4);
        std::transform(
            extension.begin(), extension.end(), extension.begin(),
            [](unsigned char ch) {
                return static_cast<char>(std::tolower(ch));
            });
        if (extension == ".exe") name.resize(name.size() - 4);
    }
    return name;
}

// =============================================================================
// Process
// =============================================================================

void Kernel32ExtManager::impl_CreateProcessW(Marshaler& m) {
    impl_CreateProcessCommon(m, 0, std::nullopt);
}

void Kernel32ExtManager::impl_CreateProcessAsUserW(Marshaler& m) {
    constexpr u32 TOKEN_ASSIGN_PRIMARY = 0x0001;
    constexpr u32 TOKEN_DUPLICATE = 0x0002;
    constexpr u32 TOKEN_QUERY = 0x0008;
    constexpr u32 REQUIRED_ACCESS =
        TOKEN_ASSIGN_PRIMARY | TOKEN_DUPLICATE | TOKEN_QUERY;

    const Handle token = static_cast<Handle>(m.arg(0));
    ipc::ipc_log("CreateProcessAsUserW ENTER token=0x%x", static_cast<unsigned>(token));
    if (m_security_stubs == nullptr ||
        !m_security_stubs->is_token_handle(token)) {
        ipc::ipc_log("CreateProcessAsUserW FAILED: ERROR_INVALID_HANDLE");
        set_last_error(error_codes::ERROR_INVALID_HANDLE);
        m.set_return(0);
        return;
    }
    if (m_security_stubs->token_type(token) != 1) {
        ipc::ipc_log("CreateProcessAsUserW FAILED: ERROR_BAD_TOKEN_TYPE");
        set_last_error(error_codes::ERROR_BAD_TOKEN_TYPE);
        m.set_return(0);
        return;
    }
    if (!m_security_stubs->token_has_access(token, REQUIRED_ACCESS)) {
        ipc::ipc_log("CreateProcessAsUserW FAILED: ERROR_ACCESS_DENIED");
        set_last_error(error_codes::ERROR_ACCESS_DENIED);
        m.set_return(0);
        return;
    }
    if (!m_security_stubs->token_has_restrictions(token) &&
        (m_elevation == nullptr || !m_elevation->is_elevated())) {
        ipc::ipc_log("CreateProcessAsUserW FAILED: ERROR_PRIVILEGE_NOT_HELD");
        set_last_error(error_codes::ERROR_PRIVILEGE_NOT_HELD);
        m.set_return(0);
        return;
    }

    ipc::ipc_log("CreateProcessAsUserW token=0x%x restricted=%d",
                 static_cast<unsigned>(token),
                 m_security_stubs->token_has_restrictions(token) ? 1 : 0);
    impl_CreateProcessCommon(m, 1, token);
}

void Kernel32ExtManager::impl_CreateProcessWithTokenW(Marshaler& m) {
    // BOOL CreateProcessWithTokenW(
    //   HANDLE hToken, DWORD dwLogonFlags,
    //   LPCWSTR lpApplicationName, LPWSTR lpCommandLine,
    //   DWORD dwCreationFlags, LPVOID lpEnvironment,
    //   LPCWSTR lpCurrentDirectory, LPSTARTUPINFOW lpStartupInfo,
    //   LPPROCESS_INFORMATION lpProcessInformation);
    // https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-createprocesswithtokenw
    constexpr u32 TOKEN_ASSIGN_PRIMARY = 0x0001;
    constexpr u32 TOKEN_DUPLICATE = 0x0002;
    constexpr u32 TOKEN_QUERY = 0x0008;
    constexpr u32 REQUIRED_ACCESS =
        TOKEN_ASSIGN_PRIMARY | TOKEN_DUPLICATE | TOKEN_QUERY;

    const Handle token = static_cast<Handle>(m.arg(0));
    const u32 logon_flags = static_cast<u32>(m.arg(1));
    if (m_security_stubs == nullptr ||
        !m_security_stubs->is_token_handle(token)) {
        set_last_error(error_codes::ERROR_INVALID_HANDLE);
        m.set_return(0);
        return;
    }
    if (m_security_stubs->token_type(token) != 1) {
        set_last_error(error_codes::ERROR_BAD_TOKEN_TYPE);
        m.set_return(0);
        return;
    }
    if (!m_security_stubs->token_has_access(token, REQUIRED_ACCESS)) {
        set_last_error(error_codes::ERROR_ACCESS_DENIED);
        m.set_return(0);
        return;
    }
    // CreateProcessWithTokenW requires SE_IMPERSONATE_NAME. Model that as the
    // same gate CreateProcessAsUserW uses: restricted tokens or an elevated
    // process. Otherwise fail honestly so callers can fall back to
    // CreateProcessW.
    if (!m_security_stubs->token_has_restrictions(token) &&
        (m_elevation == nullptr || !m_elevation->is_elevated())) {
        set_last_error(error_codes::ERROR_PRIVILEGE_NOT_HELD);
        m.set_return(0);
        return;
    }
    // LOGON_WITH_PROFILE / LOGON_NETCREDENTIALS_ONLY need profile load we do
    // not implement; fail honestly rather than pretend the profile exists.
    if (logon_flags != 0) {
        ipc::ipc_log(
            "CreateProcessWithTokenW unsupported logon_flags=0x%x",
            logon_flags);
        set_last_error(error_codes::ERROR_NOT_SUPPORTED);
        m.set_return(0);
        return;
    }

    ipc::ipc_log("CreateProcessWithTokenW token=0x%x restricted=%d",
                 static_cast<unsigned>(token),
                 m_security_stubs->token_has_restrictions(token) ? 1 : 0);
    impl_CreateProcessCommon(m, 0, token, /*with_token_w_layout=*/true);
}

void Kernel32ExtManager::impl_CreateProcessWithLogonW(Marshaler& m) {
    // BOOL CreateProcessWithLogonW(LPCWSTR lpUsername, LPCWSTR lpDomain,
    //   LPCWSTR lpPassword, DWORD dwLogonFlags, LPCWSTR lpApplicationName,
    //   LPWSTR lpCommandLine, DWORD dwCreationFlags, LPVOID lpEnvironment,
    //   LPCWSTR lpCurrentDirectory, LPSTARTUPINFOW, LPPROCESS_INFORMATION)
    // https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-createprocesswithlogonw
    constexpr u32 LOGON_WITH_PROFILE = 0x00000001;
    constexpr u32 LOGON_NETCREDENTIALS_ONLY = 0x00000002;
    constexpr u32 ERROR_LOGON_FAILURE = 1326;
    const GuestAddr user_ptr = m.arg_ptr(0);
    const GuestAddr domain_ptr = m.arg_ptr(1);
    const GuestAddr password_ptr = m.arg_ptr(2);
    const u32 logon_flags = m.arg32(3);
    if (user_ptr == 0 || (logon_flags & ~(LOGON_WITH_PROFILE |
                                          LOGON_NETCREDENTIALS_ONLY)) != 0) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return;
    }
    // LOGON_NETCREDENTIALS_ONLY: the new process runs as the caller and the
    // credentials are only presented to remote servers, so nothing local is
    // validated (Learn). Otherwise the credentials must name a local account.
    if ((logon_flags & LOGON_NETCREDENTIALS_ONLY) == 0) {
        auto read = [&](GuestAddr p) -> std::wstring {
            std::wstring s;
            for (GuestAddr a = p; p != 0 && s.size() < 1024; a += 2) {
                const u16 ch = m.read_u16(a).value_or(0);
                if (ch == 0) break;
                s.push_back(static_cast<wchar_t>(ch));
            }
            return s;
        };
        std::wstring user = read(user_ptr);
        std::wstring domain = read(domain_ptr);
        if (const usize at = user.find(L'@'); at != std::wstring::npos && domain_ptr == 0) {
            domain = user.substr(at + 1); // UPN form user@domain
            user = user.substr(0, at);
        }
        std::vector<u8> sid;
        std::wstring resolved_domain;
        u32 use = 0;
        const std::wstring qualified =
            domain.empty() || domain == L"." ? user : domain + L"\\" + user;
        const bool known =
            m_security_stubs != nullptr &&
            m_security_stubs->lookup_account_name_w(L"", qualified, sid,
                                                    resolved_domain, use) &&
            sid == SecurityStubs::synthetic_sid();
        // The capsule's only interactive account has no password, so only
        // an empty password authenticates it.
        const bool password_ok = read(password_ptr).empty();
        if (!known || !password_ok) {
            set_last_error(ERROR_LOGON_FAILURE);
            m.set_return(0);
            return;
        }
    }
    impl_CreateProcessCommon(m, 2, std::nullopt, /*with_token_w_layout=*/true);
}

// A CreateProcess that returns FALSE without saying why is very hard to
// diagnose from a guest trace: the caller usually just CHECK-crashes on the
// spot, so the failure surfaces as an illegal instruction far from its cause.
// Every early-out below reports its reason at [ipc] volume.
#define VITREO_CREATEPROCESS_FAIL(reason_literal)                             \
    ipc::ipc_log("CreateProcess FAILED: %s", reason_literal)

void Kernel32ExtManager::impl_CreateProcessCommon(
    Marshaler& m, usize argument_offset, std::optional<Handle> token,
    bool with_token_w_layout, bool ansi_strings) {
    // CreateProcessW layout (argument_offset applied):
    //   app, cmd, psa, tsa, inherit, flags, env, cwd, si, pi
    // CreateProcessWithTokenW layout (with_token_w_layout):
    //   token, logonFlags, app, cmd, flags, env, cwd, si, pi
    GuestAddr application_name_ptr;
    GuestAddr command_line_ptr;
    GuestAddr process_attributes_ptr = 0;
    GuestAddr thread_attributes_ptr = 0;
    bool inherit_handles;
    u32 creation_flags;
    GuestAddr environment_ptr = 0;
    GuestAddr current_directory_ptr;
    GuestAddr startup_info_ptr;
    GuestAddr process_information_ptr;
    if (with_token_w_layout) {
        // argument_offset skips the extra leading credential arguments of
        // CreateProcessWithLogonW (user, domain, password vs. hToken).
        application_name_ptr = m.arg_ptr(argument_offset + 2);
        command_line_ptr = m.arg_ptr(argument_offset + 3);
        inherit_handles = false;
        creation_flags = m.arg32(argument_offset + 4);
        environment_ptr = m.arg_ptr(argument_offset + 5);
        current_directory_ptr = m.arg_ptr(argument_offset + 6);
        startup_info_ptr = m.arg_ptr(argument_offset + 7);
        process_information_ptr = m.arg_ptr(argument_offset + 8);
    } else {
        application_name_ptr = m.arg_ptr(argument_offset + 0);
        command_line_ptr = m.arg_ptr(argument_offset + 1);
        process_attributes_ptr = m.arg_ptr(argument_offset + 2);
        thread_attributes_ptr = m.arg_ptr(argument_offset + 3);
        inherit_handles = m.arg(argument_offset + 4) != 0;
        creation_flags = static_cast<u32>(m.arg(argument_offset + 5));
        environment_ptr = m.arg_ptr(argument_offset + 6);
        current_directory_ptr = m.arg_ptr(argument_offset + 7);
        startup_info_ptr = m.arg_ptr(argument_offset + 8);
        process_information_ptr = m.arg_ptr(argument_offset + 9);
    }

    // SECURITY_ATTRIBUTES.bInheritHandle for the returned hProcess / hThread.
    // Layout: nLength (4), pad to pointer, lpSecurityDescriptor, BOOL bInherit.
    const auto read_sa_inherit = [&](GuestAddr sa) -> bool {
        if (sa == 0) return false;
        const GuestAddr inherit_off = m.cpu_is_64bit() ? 16u : 8u;
        const auto v = m_memory.read_u32(sa + inherit_off);
        return v.is_ok() && v.value() != 0;
    };
    const bool process_handle_inherit = read_sa_inherit(process_attributes_ptr);
    const bool thread_handle_inherit = read_sa_inherit(thread_attributes_ptr);

    constexpr u32 EXTENDED_STARTUPINFO_PRESENT = 0x00080000;
    // A child created with CREATE_BREAKAWAY_FROM_JOB is not part of its
    // creator's job; it must outlive the creator, so it breaks away from the
    // session's kill-on-close ownership (no parent-death watch).
    constexpr u32 CREATE_BREAKAWAY_FROM_JOB = 0x01000000;
    const bool breakaway =
        (creation_flags & CREATE_BREAKAWAY_FROM_JOB) != 0;
    std::optional<std::vector<Handle>> selected_inherited_handles;
    std::optional<std::vector<Handle>> selected_job_handles;
    std::array<u64, 2> selected_mitigation_policy{};
    u32 selected_mitigation_policy_size = 0;
    std::optional<u32> selected_child_process_policy;
    std::optional<u64> selected_pseudo_console;
    if ((creation_flags & EXTENDED_STARTUPINFO_PRESENT) != 0) {
        if (startup_info_ptr == 0) {
            set_last_error(error_codes::ERROR_INVALID_PARAMETER);
            VITREO_CREATEPROCESS_FAIL("ERROR_INVALID_PARAMETER");
            m.set_return(0);
            return;
        }

        // STARTUPINFOEXW appends lpAttributeList immediately after
        // STARTUPINFOW: offset 68 in x86 and 104 in x64.
        const GuestAddr attribute_list_offset =
            m.cpu_is_64bit() ? 104u : 68u;
        GuestAddr attribute_list = 0;
        if (m.cpu_is_64bit()) {
            auto value =
                m_memory.read_u64(startup_info_ptr + attribute_list_offset);
            if (value.is_err()) {
                set_last_error(error_codes::ERROR_INVALID_PARAMETER);
                VITREO_CREATEPROCESS_FAIL("ERROR_INVALID_PARAMETER");
                m.set_return(0);
                return;
            }
            attribute_list = value.value();
        } else {
            auto value =
                m_memory.read_u32(startup_info_ptr + attribute_list_offset);
            if (value.is_err()) {
                set_last_error(error_codes::ERROR_INVALID_PARAMETER);
                VITREO_CREATEPROCESS_FAIL("ERROR_INVALID_PARAMETER");
                m.set_return(0);
                return;
            }
            attribute_list = value.value();
        }

        if (attribute_list != 0) {
            std::lock_guard<std::mutex> lock(m_proc_thread_attribute_mutex);
            const auto found =
                m_proc_thread_attribute_lists.find(attribute_list);
            if (found == m_proc_thread_attribute_lists.end()) {
                VITREO_TRACE(
                    "[kernel32_ext] CreateProcessW: unknown process "
                    "attribute list 0x%llx\n",
                    static_cast<unsigned long long>(attribute_list));
                set_last_error(error_codes::ERROR_INVALID_PARAMETER);
                VITREO_CREATEPROCESS_FAIL("ERROR_INVALID_PARAMETER");
                m.set_return(0);
                return;
            }
            selected_inherited_handles = found->second.inherited_handles;
            selected_job_handles = found->second.job_handles;
            selected_mitigation_policy = found->second.mitigation_policy;
            selected_mitigation_policy_size =
                found->second.mitigation_policy_size;
            selected_child_process_policy =
                found->second.child_process_policy;
            selected_pseudo_console = found->second.pseudo_console;
        }
    }

    // PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE: the child's console is the
    // pseudoconsole, so both of its pipe ends have to cross the process
    // boundary with the SAME numeric handle values, and the child has to be
    // told which they are. Windows does this inside conhost; here the two are
    // transferred explicitly and named on the child's command line.
    std::string pseudo_console_handles;
    std::vector<Handle> console_channels;
    if (selected_pseudo_console.has_value() && m_console_manager != nullptr) {
        const auto pc =
            m_console_manager->pseudo_consoles().find(*selected_pseudo_console);
        if (!pc.has_value()) {
            VITREO_TRACE("[conpty] CreateProcess: unknown HPCON 0x%llx\n",
                         static_cast<unsigned long long>(*selected_pseudo_console));
            set_last_error(error_codes::ERROR_INVALID_PARAMETER);
            VITREO_CREATEPROCESS_FAIL("ERROR_INVALID_PARAMETER");
            m.set_return(0);
            return;
        }
        for (const Handle h : {pc->input, pc->output}) {
            if (h != NULL_HANDLE) console_channels.push_back(h);
        }
        char buf[48];
        std::snprintf(buf, sizeof(buf), "%u,%u",
                      static_cast<unsigned>(pc->input),
                      static_cast<unsigned>(pc->output));
        pseudo_console_handles = buf;
        VITREO_TRACE("[conpty] CreateProcess: child console = HPCON 0x%llx "
                     "(in=0x%x out=0x%x)\n",
                     static_cast<unsigned long long>(*selected_pseudo_console),
                     pc->input, pc->output);
    }

    if (selected_inherited_handles.has_value() && !inherit_handles) {
        // PROC_THREAD_ATTRIBUTE_HANDLE_LIST requires bInheritHandles=TRUE.
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        VITREO_CREATEPROCESS_FAIL("ERROR_INVALID_PARAMETER");
        m.set_return(0);
        return;
    }
    if (selected_job_handles.has_value()) {
        std::lock_guard lock(m_job_mutex);
        for (const Handle job_handle : *selected_job_handles) {
            if (!m_jobs.contains(job_handle)) {
                set_last_error(error_codes::ERROR_INVALID_HANDLE);
                VITREO_CREATEPROCESS_FAIL("ERROR_INVALID_HANDLE");
                m.set_return(0);
                return;
            }
        }
    }

    // Learn CreateProcessW: lpCommandLine may be up to 32,767 characters
    // including the terminator. Strings are carried as UTF-8 internally, so
    // the W form converts every UTF-16 unit (surrogate pairs included) and the
    // A form converts from CP_ACP -- never narrowing each unit to its low byte
    // (process_contract_probe.expected: "xé€\U0001F600" and a
    // "dé€" current directory reach the child intact).
    constexpr usize kMaxProcessString = 32767;
    const auto read_process_string = [&](GuestAddr address) {
        if (address == 0) return std::string{};
        if (!ansi_strings) {
            auto wide = m.read_wide_string(address, kMaxProcessString);
            return wide.is_ok() ? wide.value() : std::string{};
        }
        std::string bytes;
        for (usize offset = 0; offset < kMaxProcessString; ++offset) {
            const auto byte = m_memory.read_u8(address + offset);
            if (byte.is_err() || byte.value() == 0) break;
            bytes.push_back(static_cast<char>(byte.value()));
        }
        const auto* src = reinterpret_cast<const u8*>(bytes.data());
        std::vector<u16> wide(codepage::multi_byte_to_wide(
            codepage::kAnsiCodePage, 0, src, bytes.size(), nullptr, 0).count);
        if (!wide.empty()) {
            (void)codepage::multi_byte_to_wide(codepage::kAnsiCodePage, 0, src,
                                               bytes.size(), wide.data(),
                                               wide.size());
        }
        return utf16_to_utf8(wide.data(), static_cast<int>(wide.size()));
    };

    std::string app_path;
    // Capture the FULL command line so the spawned child's GetCommandLineW
    // returns it verbatim. A child must see the exact command line it was
    // given.
    std::string full_cmdline;
    if (command_line_ptr != 0) {
        full_cmdline = read_process_string(command_line_ptr);
    }
    if (application_name_ptr != 0) {
        app_path = read_process_string(application_name_ptr);
    }
    std::string child_current_directory;
    if (current_directory_ptr != 0) {
        child_current_directory = read_process_string(current_directory_ptr);
    }

    // With lpApplicationName supplied there is exactly one candidate. Without
    // it the module name is taken from lpCommandLine, and an *unquoted* name
    // containing spaces is genuinely ambiguous: CreateProcess documents that
    // the system tries the successive prefixes ending at each space, shortest
    // first, until one names a file. Stopping at the first token instead makes
    // every unquoted path with a space unlaunchable — the caller sees
    // ERROR_FILE_NOT_FOUND for an executable that is plainly there.
    std::vector<std::string> app_candidates;
    if (app_path.empty() && !full_cmdline.empty()) {
        const std::string& cmd = full_cmdline;
        if (cmd.front() == '"') {
            auto end = cmd.find('"', 1);
            app_candidates.push_back((end != std::string::npos) ? cmd.substr(1, end - 1)
                                                                : cmd.substr(1));
        } else {
            for (usize sp = cmd.find_first_of(" \t"); sp != std::string::npos;
                 sp = cmd.find_first_of(" \t", sp + 1)) {
                app_candidates.push_back(cmd.substr(0, sp));
            }
            app_candidates.push_back(cmd);
        }
        if (!app_candidates.empty()) app_path = app_candidates.front();
    } else if (!app_path.empty()) {
        app_candidates.push_back(app_path);
    }

    // Generic diagnostic hook for child-only command-line switches. Some
    // multiprocess guests consume diagnostics in a spawned service process,
    // while their top-level executable deliberately filters unknown switches.
    // This is inert unless explicitly configured by the host.
    if (const char* extra = std::getenv("VITREO_CHILD_APPEND_CMDLINE");
        extra != nullptr && *extra != '\0') {
        if (!full_cmdline.empty() && full_cmdline.back() != ' ') {
            full_cmdline.push_back(' ');
        }
        full_cmdline.append(extra);
    }

    if (app_path.empty()) {
        set_last_error(error_codes::ERROR_FILE_NOT_FOUND);
        VITREO_CREATEPROCESS_FAIL("ERROR_FILE_NOT_FOUND");
        m.set_return(0);
        return;
    }

    // System executables the runtime implements itself are resolved by name
    // before the filesystem is consulted — there is no PE on disk to find.
    for (const std::string& candidate : app_candidates) {
        if (find_builtin_process(candidate) != nullptr) {
            app_path = candidate;
            break;
        }
        const std::string with_exe = createprocess_maybe_append_exe(candidate);
        if (with_exe != candidate &&
            find_builtin_process(with_exe) != nullptr) {
            app_path = with_exe;
            break;
        }
    }
    if (const auto* builtin = find_builtin_process(app_path)) {
        const u32 code = (*builtin)(full_cmdline);
        VITREO_TRACE("[kernel32_ext] CreateProcess builtin '%s' -> exit=%u\n",
                     app_path.c_str(), code);

        Handle process_handle = NULL_HANDLE;
        if (m_sync_manager != nullptr) {
            process_handle = m_sync_manager->create_completed_process_handle(code);
        }
        if (process_handle == NULL_HANDLE) {
            set_last_error(error_codes::ERROR_NOT_ENOUGH_MEMORY);
            VITREO_CREATEPROCESS_FAIL("ERROR_NOT_ENOUGH_MEMORY");
            m.set_return(0);
            return;
        }

        // PROCESS_INFORMATION { hProcess, hThread, dwProcessId, dwThreadId }.
        // hThread is the same already-signaled object: a parent that waits on
        // the thread rather than the process must not block forever.
        if (process_information_ptr != 0) {
            const u32 ptr_size = m.cpu_is_64bit() ? 8u : 4u;
            (void)m.write_ptr(process_information_ptr, process_handle);
            (void)m.write_ptr(process_information_ptr + ptr_size, process_handle);
            (void)m_memory.write_u32(process_information_ptr + ptr_size * 2, 0);
            (void)m_memory.write_u32(process_information_ptr + ptr_size * 2 + 4, 0);
        }

        m.set_return(1); // TRUE — the process ran and has already exited.
        return;
    }

    auto executable =
        Result<std::pair<std::string, bool>, capsule::VfsError>::err(
            capsule::VfsError::FileNotFound);
    // Learn CreateProcessW (lpApplicationName NULL): append .exe when the
    // module token has no extension/path, then search application dir, CWD,
    // System32, System, Windows, and PATH.
    std::vector<std::string> resolve_paths;
    const auto push_unique = [&](const std::string& path) {
        if (path.empty()) return;
        if (std::find(resolve_paths.begin(), resolve_paths.end(), path) ==
            resolve_paths.end()) {
            resolve_paths.push_back(path);
        }
    };
    if (application_name_ptr == 0) {
        std::string app_dir;
        if (m_module_manager != nullptr) {
            app_dir = createprocess_parent_dir(
                m_module_manager->main_module_path());
        }
        std::vector<std::string> path_dirs;
        if (m_environment_manager != nullptr) {
            auto path_var = m_environment_manager->get_variable("PATH");
            if (path_var.is_ok()) {
                path_dirs = createprocess_split_path(path_var.value());
            }
        }
        constexpr const char* kSystem32 = "C:\\Windows\\System32";
        constexpr const char* kSystem = "C:\\Windows\\System";
        constexpr const char* kWindows = "C:\\Windows";
        for (const std::string& raw : app_candidates) {
            const std::string module = createprocess_maybe_append_exe(raw);
            if (createprocess_has_directory(module)) {
                push_unique(module);
                continue;
            }
            push_unique(createprocess_join_dir(app_dir, module));
            push_unique(createprocess_join_dir(m_current_directory, module));
            push_unique(createprocess_join_dir(kSystem32, module));
            push_unique(createprocess_join_dir(kSystem, module));
            push_unique(createprocess_join_dir(kWindows, module));
            for (const auto& dir : path_dirs) {
                push_unique(createprocess_join_dir(dir, module));
            }
        }
    } else {
        for (const std::string& candidate : app_candidates) {
            push_unique(candidate);
        }
    }
    for (const std::string& path : resolve_paths) {
        auto try_exe = resolve_guest_executable(m_vfs, path);
        if (try_exe.is_ok()) {
            app_path = path;
            executable = std::move(try_exe);
            break;
        }
    }
    if (executable.is_err()) {
        VITREO_TRACE("[kernel32_ext] CreateProcessW: resolve_path failed for '%s'"
                     " cmdline='%s'\n", app_path.c_str(), full_cmdline.c_str());
        set_last_error(error_codes::ERROR_FILE_NOT_FOUND);
        VITREO_CREATEPROCESS_FAIL("ERROR_FILE_NOT_FOUND");
        m.set_return(0);
        return;
    }
    const auto& [host_path, temporary] = executable.value();
    const bool current_elevated =
        m_elevation != nullptr && m_elevation->is_elevated();
    if (!current_elevated &&
        pe::requests_elevation(requested_execution_level(host_path))) {
        if (temporary) (void)::unlink(host_path.c_str());
        VITREO_TRACE(
            "[kernel32_ext] CreateProcessW('%s') -> FALSE "
            "(manifest requires elevation)\n",
            app_path.c_str());
        set_last_error(error_codes::ERROR_ELEVATION_REQUIRED);
        VITREO_CREATEPROCESS_FAIL("ERROR_ELEVATION_REQUIRED");
        m.set_return(0);
        return;
    }

    std::string inherited_pipes;
    std::string inherited_sync;
    std::string inherited_mappings;
    // Released on every return below, after the spawn and the restore.
    std::unique_lock<std::mutex> inherited_spawn_lock(
        g_inherited_handle_spawn_mutex);
    if (inherit_handles) {
        std::optional<std::string> prepared_mappings;
        if (selected_inherited_handles.has_value()) {
            const auto selected =
                std::span<const Handle>(*selected_inherited_handles);
            if (m_file_mapping_manager) {
                prepared_mappings =
                    m_file_mapping_manager->prepare_inherited_handles(selected);
            }
            if (prepared_mappings.has_value()) {
                inherited_mappings = std::move(*prepared_mappings);
            } else if (m_file_mapping_manager) {
                set_last_error(error_codes::ERROR_NOT_ENOUGH_MEMORY);
                VITREO_CREATEPROCESS_FAIL("ERROR_NOT_ENOUGH_MEMORY");
                m.set_return(0);
                return;
            }
            if (m_file_manager) {
                inherited_pipes =
                    m_file_manager->prepare_inherited_named_pipes(selected);
            }
            if (m_sync_manager) {
                inherited_sync =
                    m_sync_manager->prepare_inherited_handles(selected);
            }
        } else {
            if (m_file_mapping_manager) {
                prepared_mappings =
                    m_file_mapping_manager->prepare_inherited_handles();
            }
            if (prepared_mappings.has_value()) {
                inherited_mappings = std::move(*prepared_mappings);
            } else if (m_file_mapping_manager) {
                set_last_error(error_codes::ERROR_NOT_ENOUGH_MEMORY);
                VITREO_CREATEPROCESS_FAIL("ERROR_NOT_ENOUGH_MEMORY");
                m.set_return(0);
                return;
            }
            if (m_file_manager) {
                inherited_pipes =
                    m_file_manager->prepare_inherited_named_pipes();
            }
            if (m_sync_manager) {
                inherited_sync = m_sync_manager->prepare_inherited_handles();
            }
        }
    }
    if (!console_channels.empty() && m_file_manager) {
        // Pseudoconsole attachment is independent of bInheritHandles and the
        // caller's handle flags (Microsoft Learn pseudoconsole launch sample).
        // Avoid importing an endpoint twice if ordinary inheritance included it.
        std::vector<Handle> console_only;
        for (const Handle h : console_channels) {
            const bool already_inherited = inherit_handles &&
                (m_file_manager->handle_table().get_flags(h) & HandleFlags::Inherit) != 0 &&
                (!selected_inherited_handles.has_value() ||
                 std::find(selected_inherited_handles->begin(),
                           selected_inherited_handles->end(), h) !=
                     selected_inherited_handles->end());
            if (!already_inherited) console_only.push_back(h);
        }
        auto descriptors = m_file_manager->prepare_inherited_named_pipes(
            console_only, /*require_inheritable=*/false);
        if (!descriptors.empty()) {
            if (!inherited_pipes.empty()) inherited_pipes.push_back(';');
            inherited_pipes += descriptors;
        }
    }
    const u32 reported_tid = m_thread_manager
        ? m_thread_manager->allocate_thread_id()
        : ipc::allocate_system_thread_id();
    if (reported_tid == 0) {
        if (temporary) (void)::unlink(host_path.c_str());
        if (m_file_manager) m_file_manager->restore_inherited_pipe_close_on_exec();
        set_last_error(error_codes::ERROR_NOT_ENOUGH_MEMORY);
        m.set_return(0);
        return;
    }
    std::vector<std::string> process_attribute_environment{
        "VITREO_PRIMARY_THREAD_ID=" + std::to_string(reported_tid)};
    if (selected_mitigation_policy_size != 0) {
        char value[2 * 16 + 2]{};
        std::snprintf(
            value, sizeof(value), "%llx:%llx",
            static_cast<unsigned long long>(selected_mitigation_policy[0]),
            static_cast<unsigned long long>(selected_mitigation_policy[1]));
        process_attribute_environment.emplace_back(
            std::string("VITREO_PROCESS_MITIGATION_POLICY=") + value);
    }
    if (selected_child_process_policy.has_value()) {
        const bool restricted =
            (*selected_child_process_policy & 0x1u) != 0;
        process_attribute_environment.emplace_back(
            restricted ? "VITREO_CHILD_PROCESS_RESTRICTED=1"
                       : "VITREO_CHILD_PROCESS_RESTRICTED=0");
    }
    process_attribute_environment.emplace_back(
        (creation_flags & CREATE_SUSPENDED) != 0
            ? "VITREO_PRIMARY_THREAD_SUSPENDED=1"
            : "VITREO_PRIMARY_THREAD_SUSPENDED=0");
    // Microsoft Learn (STARTUPINFOW.lpDesktop / CreateProcessAsUserW): the
    // child attaches to windowstation\desktop (or desktop alone). Forward the
    // name so the child USER32 manager can apply it at construction.
    if (startup_info_ptr != 0) {
        const GuestAddr desktop_field =
            startup_info_ptr + (m.cpu_is_64bit() ? 16u : 8u);
        GuestAddr desktop_name_ptr = 0;
        if (m.cpu_is_64bit()) {
            desktop_name_ptr =
                m_memory.read_u64(desktop_field).value_or(0);
        } else {
            desktop_name_ptr =
                m_memory.read_u32(desktop_field).value_or(0);
        }
        if (desktop_name_ptr != 0) {
            auto desktop_name = m.read_wide_string(desktop_name_ptr);
            if (desktop_name.is_ok() && !desktop_name.value().empty()) {
                process_attribute_environment.emplace_back(
                    std::string("VITREO_STARTUP_DESKTOP=") +
                    desktop_name.value());
                ipc::ipc_log("CreateProcess lpDesktop='%s'",
                             desktop_name.value().c_str());
            }
        }
    }
    // STARTUPINFOW.lpTitle: with STARTF_TITLEISAPPID it is the child's
    // AppUserModelID, which becomes its explicit process identity
    // (window_propstore_probe: the child's
    // GetCurrentProcessExplicitAppUserModelID returns it).
    if (startup_info_ptr != 0) {
        const GuestAddr title_field =
            startup_info_ptr + (m.cpu_is_64bit() ? 24u : 12u);
        const GuestAddr title_ptr = m.cpu_is_64bit()
            ? static_cast<GuestAddr>(m_memory.read_u64(title_field).value_or(0))
            : m_memory.read_u32(title_field).value_or(0);
        const std::string title = read_process_string(title_ptr);
        if (!title.empty()) {
            process_attribute_environment.emplace_back(
                "VITREO_STARTUP_TITLE=" + title);
        }
    }
    // STARTUPINFO: the child's GetStartupInfo reports the creator's dwFlags,
    // wShowWindow and geometry, and with STARTF_USESTDHANDLES its
    // GetStdHandle returns hStdInput/hStdOutput/hStdError (Learn STARTUPINFOW;
    // process_contract_probe.expected "redirected child"). Without that flag
    // an inheriting child starts with the creator's current std handles.
    // Handle values cross unchanged (inheritable pipes/files are imported
    // under the same numbers); this process's own console handles are named
    // by role so the child maps them to its console.
    {
        GuestStartupInfo si{};
        if (startup_info_ptr != 0) {
            const bool w64 = m.cpu_is_64bit();
            const GuestAddr base = startup_info_ptr + (w64 ? 32u : 16u);
            const auto rd32 = [&](GuestAddr a) {
                return m_memory.read_u32(a).value_or(0);
            };
            si.x = rd32(base + 0);
            si.y = rd32(base + 4);
            si.x_size = rd32(base + 8);
            si.y_size = rd32(base + 12);
            si.x_count_chars = rd32(base + 16);
            si.y_count_chars = rd32(base + 20);
            si.fill_attribute = rd32(base + 24);
            si.flags = rd32(base + 28);
            si.show_window = m_memory.read_u16(base + 32).value_or(0);
            for (u32 i = 0; i < 3; ++i) {
                const u32 v = w64
                    ? static_cast<u32>(m_memory.read_u64(startup_info_ptr + 80 + i * 8).value_or(0))
                    : rd32(startup_info_ptr + 56 + i * 4);
                (i == 0 ? si.std_input : i == 1 ? si.std_output : si.std_error) = v;
            }
        }
        const bool use_std = (si.flags & STARTF_USESTDHANDLES) != 0;
        std::string kinds;
        for (u32 i = 0; i < 3; ++i) {
            u32& slot = i == 0 ? si.std_input : i == 1 ? si.std_output : si.std_error;
            if (!use_std) {
                slot = 0;
                if (m_console_manager != nullptr) {
                    const u32 saved = get_last_error();
                    slot = m_console_manager->get_std_handle(
                        i == 0 ? STD_INPUT_HANDLE : i == 1 ? STD_OUTPUT_HANDLE
                                                           : STD_ERROR_HANDLE);
                    set_last_error(saved);
                }
            }
            const u32 role = m_console_manager != nullptr
                ? m_console_manager->console_handle_role(slot) : 0;
            char kind = 'h';
            if (role == 1) {
                kind = 'i';
            } else if (role == 2) {
                kind = 'o';
            } else if (!use_std) {
                const bool inheritable = m_handle_table != nullptr && slot != 0 &&
                    (m_handle_table->get_flags(slot) & HandleFlags::Inherit) != 0;
                kind = inherit_handles && inheritable ? 'h' : 'n';
            }
            if (!kinds.empty()) kinds.push_back(',');
            kinds.push_back(kind);
        }
        process_attribute_environment.emplace_back(
            "VITREO_STARTUP_INFO=" + encode_guest_startup_info(si));
        process_attribute_environment.emplace_back(
            "VITREO_STARTUP_STD_KINDS=" + kinds);
    }
    if (token.has_value() && m_security_stubs != nullptr) {
        auto token_environment =
            m_security_stubs->token_launch_environment(*token);
        process_attribute_environment.insert(
            process_attribute_environment.end(),
            std::make_move_iterator(token_environment.begin()),
            std::make_move_iterator(token_environment.end()));
    }

    // Honor CreateProcess lpEnvironment. NULL means inherit the parent's
    // guest environment (Windows contract). A child inherits variables the
    // parent set for it, including loader paths.
    constexpr u32 CREATE_UNICODE_ENVIRONMENT = 0x00000400;
    std::vector<std::string> guest_environment;
    if (environment_ptr != 0) {
        guest_environment = read_createprocess_environment_block(
            m_memory, environment_ptr,
            (creation_flags & CREATE_UNICODE_ENVIRONMENT) != 0);
        ipc::ipc_log("CreateProcess env block entries=%zu unicode=%d",
                     guest_environment.size(),
                     (creation_flags & CREATE_UNICODE_ENVIRONMENT) != 0 ? 1 : 0);
    } else {
        guest_environment = inherited_guest_environment();
    }

    if (std::getenv("VITREO_CREATEPROCESS_DIAG") != nullptr) {
        std::fprintf(
            stderr,
            "[process-diag] app='%s' cmdline='%s' cwd='%s' flags=0x%x "
            "inherit=%u explicit_env=%u env_entries=%zu token=%u\n",
            app_path.c_str(), full_cmdline.c_str(),
            child_current_directory.c_str(), creation_flags,
            inherit_handles ? 1u : 0u, environment_ptr != 0 ? 1u : 0u,
            guest_environment.size(), token.has_value() ? 1u : 0u);
    }

    pid_t child_pid = static_cast<pid_t>(launch_guest_worker(
        host_path, app_path, full_cmdline, child_current_directory,
        inherited_pipes, inherited_sync, inherited_mappings,
        current_elevated ? m_elevation->credential() : std::string_view{},
        std::move(process_attribute_environment),
        std::move(guest_environment), breakaway, pseudo_console_handles));
    if (temporary) {
        (void)::unlink(host_path.c_str());
    }
    if (m_file_manager) {
        m_file_manager->restore_inherited_pipe_close_on_exec();
    }
    inherited_spawn_lock.unlock();
    if (child_pid < 0) {
        set_last_error(error_codes::ERROR_ACCESS_DENIED);
        VITREO_CREATEPROCESS_FAIL("ERROR_ACCESS_DENIED");
        m.set_return(0);
        return;
    }

    // hProcess must track the real child regardless of the optional
    // shared-object IPC gate. A pre-signaled stand-in makes a live child look
    // terminated and causes launch supervisors to relaunch it indefinitely.
    Handle process_handle = 0;
    if (m_sync_manager) {
        process_handle = m_sync_manager->create_process_handle(static_cast<int>(child_pid));
    }
    if (process_handle == 0 && m_sync_manager) {
        process_handle = m_sync_manager->create_event(
            true,  // manual reset
            true,  // already signaled (legacy detached launch, no join semantics)
            std::string{}
        );
    }
    if (process_handle != 0 && process_handle_inherit && m_handle_table != nullptr) {
        (void)m_handle_table->set_flags(process_handle, HandleFlags::Inherit,
                                        HandleFlags::Inherit);
    }
    // The child uses the ID reserved before launch, including its TEB.
    const u32 thread_flags =
        thread_handle_inherit ? HandleFlags::Inherit : 0;
    const Handle thread_handle = m_file_manager
        ? m_file_manager->handle_table().allocate(
              HandleType::Thread, nullptr, thread_flags)
        : NULL_HANDLE;
    if (thread_handle != NULL_HANDLE && m_handle_table != nullptr) {
        auto* process = m_handle_table->get_as<ipc::ProcessObject>(
            process_handle, HandleType::Process);
        if (process != nullptr) {
            std::lock_guard lock(m_external_thread_mutex);
            m_external_primary_by_tid[reported_tid] = ExternalPrimaryThread{
                process,
                (creation_flags & CREATE_SUSPENDED) != 0 ? 1u : 0u,
                reported_tid,
            };
            m_external_primary_handles.emplace(thread_handle, reported_tid);
        }
    }

    if (selected_job_handles.has_value()) {
        std::vector<std::shared_ptr<JobState>> selected_jobs;
        {
            std::lock_guard lock(m_job_mutex);
            for (const Handle job_handle : *selected_job_handles) {
                const auto found = m_jobs.find(job_handle);
                if (found != m_jobs.end()) {
                    selected_jobs.push_back(found->second);
                }
            }
        }
        for (const auto& job : selected_jobs) {
            (void)add_job_member(job, process_handle);
        }
    }

    // PROCESS_INFORMATION identifies the child, never the creating process.
    const u32 reported_pid = static_cast<u32>(child_pid);
    if (process_information_ptr != 0) {
        // PROCESS_INFORMATION layout is bitness-dependent: HANDLEs are
        // pointer-sized. Writing the x86 layout on x64 made the guest read
        // hThread == dwProcessId (it then called CloseHandle(pid)) and
        // dwProcessId/dwThreadId from uninitialized bytes at +16/+20.
        if (m.cpu_is_64bit()) {
            (void)m_memory.write_u64(process_information_ptr + 0, process_handle); // hProcess
            (void)m_memory.write_u64(process_information_ptr + 8, thread_handle);  // hThread
            (void)m_memory.write_u32(process_information_ptr + 16, reported_pid);  // dwProcessId
            (void)m_memory.write_u32(process_information_ptr + 20, reported_tid);  // dwThreadId
        } else {
            (void)m_memory.write_u32(process_information_ptr + 0, process_handle); // hProcess
            (void)m_memory.write_u32(process_information_ptr + 4, thread_handle);  // hThread
            (void)m_memory.write_u32(process_information_ptr + 8, reported_pid);   // dwProcessId
            (void)m_memory.write_u32(process_information_ptr + 12, reported_tid);  // dwThreadId
        }
    }

    ipc::ipc_log("CreateProcessW '%s' -> pid=%d hProc=0x%x",
                 app_path.c_str(), static_cast<int>(child_pid),
                 static_cast<unsigned>(process_handle));
    // The command line, not the image name, is what distinguishes the members
    // of a multi-process tree: every child re-executes the same executable
    // and is identified only by its command line.
    VITREO_TRACE("[kernel32_ext] CreateProcessW('%s') -> TRUE (pid=%d, ipc=%d)"
                 " cmdline='%s'\n",
                 app_path.c_str(), static_cast<int>(child_pid),
                 ipc::ipc_enabled() ? 1 : 0, full_cmdline.c_str());
    set_last_error(error_codes::ERROR_SUCCESS);
    m.set_return(1);
}

bool Kernel32ExtManager::launch_shell_executable(
    const std::string& app_path,
    const std::string& parameters,
    const std::string& directory,
    i32 show_cmd,
    bool request_elevation,
    Handle& process_handle,
    u32& error) {
    process_handle = NULL_HANDLE;
    error = error_codes::ERROR_SUCCESS;
    auto executable = resolve_guest_executable(m_vfs, app_path);
    if (executable.is_err()) {
        VITREO_TRACE("[kernel32_ext] Shell launch: resolve_path failed for '%s'\n",
                     app_path.c_str());
        error = error_codes::ERROR_FILE_NOT_FOUND;
        return false;
    }

    std::string full_cmdline = "\"" + app_path + "\"";
    if (!parameters.empty()) {
        full_cmdline += " ";
        full_cmdline += parameters;
    }

    const auto& [host_path, temporary] = executable.value();
    bool target_elevated =
        m_elevation != nullptr && m_elevation->is_elevated();
    std::string target_credential =
        target_elevated ? std::string(m_elevation->credential())
                        : std::string{};
    const auto requested_level = requested_execution_level(host_path);
    if (!target_elevated &&
        (request_elevation || pe::requests_elevation(requested_level))) {
        if (m_elevation_authorizer == nullptr) {
            error = error_codes::ERROR_ACCESS_DENIED;
            if (temporary) (void)::unlink(host_path.c_str());
            return false;
        }
        const auto authorization = m_elevation_authorizer->authorize(
            {
                executable_display_name(app_path),
                host_path,
            },
            target_credential);
        if (authorization != platform::ElevationAuthorizationResult::Authorized ||
            target_credential.empty()) {
            error =
                authorization ==
                        platform::ElevationAuthorizationResult::Cancelled
                    ? error_codes::ERROR_CANCELLED
                    : error_codes::ERROR_ACCESS_DENIED;
            if (temporary) (void)::unlink(host_path.c_str());
            return false;
        }
        target_elevated = true;
    }
    // SHELLEXECUTEINFO: lpDirectory is the child's working directory and
    // nShow its show state (STARTF_USESHOWWINDOW, wShowWindow).
    GuestStartupInfo si{};
    si.flags = 0x00000001u; // STARTF_USESHOWWINDOW
    si.show_window = static_cast<u16>(show_cmd);
    std::vector<std::string> startup_environment{
        "VITREO_STARTUP_INFO=" + encode_guest_startup_info(si)};
    // ShellExecute starts the program with CreateProcess and no environment
    // block, so the child inherits the caller's environment (Learn
    // CreateProcess lpEnvironment: NULL uses the calling process's).
    const pid_t child_pid = static_cast<pid_t>(
        launch_guest_worker(
            host_path, app_path, full_cmdline, directory, {}, {}, {},
            target_credential, std::move(startup_environment),
            inherited_guest_environment()));
    if (temporary) {
        (void)::unlink(host_path.c_str());
    }
    if (child_pid < 0) {
        error = error_codes::ERROR_ACCESS_DENIED;
        return false;
    }

    if (m_sync_manager != nullptr) {
        process_handle = m_sync_manager->create_process_handle(
            static_cast<int>(child_pid));
        if (process_handle == NULL_HANDLE) {
            process_handle = m_sync_manager->create_event(
                true, true, std::string{});
        }
    }

    ipc::ipc_log("ShellExecute '%s' -> pid=%d hProc=0x%x",
                 app_path.c_str(), static_cast<int>(child_pid),
                 static_cast<unsigned>(process_handle));
    VITREO_TRACE("[kernel32_ext] Shell launch '%s' -> pid=%d hProc=0x%x\n",
                 app_path.c_str(), static_cast<int>(child_pid),
                 static_cast<unsigned>(process_handle));
    return true;
}

namespace {

/// Builtin system executables, keyed by lower-case file name.
std::map<std::string, BuiltinProcessHandler>& builtin_process_table() {
    static std::map<std::string, BuiltinProcessHandler> table;
    return table;
}

std::string lower_file_name(const std::string& path) {
    // Accept a bare name, a Windows path, or a mixed one — a guest may launch
    // a system executable any of those ways.
    const usize slash = path.find_last_of("\\/");
    std::string name =
        (slash == std::string::npos) ? path : path.substr(slash + 1);
    for (char& c : name) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return name;
}

} // namespace

void register_builtin_process(const std::string& image_name,
                              BuiltinProcessHandler handler) {
    if (!handler) return;
    builtin_process_table()[lower_file_name(image_name)] = std::move(handler);
}

const BuiltinProcessHandler* find_builtin_process(const std::string& image_path) {
    auto& table = builtin_process_table();
    auto it = table.find(lower_file_name(image_path));
    return (it == table.end()) ? nullptr : &it->second;
}

void Kernel32ExtManager::impl_GetExitCodeProcess(Marshaler& m) {
    // BOOL GetExitCodeProcess(HANDLE hProcess, LPDWORD lpExitCode)
    //
    // Measured on Windows 11 (tools/oracle_probe/process_contract_probe.expected):
    //  - GetCurrentProcess(): TRUE, STILL_ACTIVE (259), last error untouched;
    //  - invalid / thread / file / mutex / semaphore handles and the
    //    GetCurrentThread() pseudo handle: FALSE + ERROR_INVALID_HANDLE,
    //    *lpExitCode untouched;
    //  - an EVENT handle: TRUE, *lpExitCode = 0, last error
    //    ERROR_INVALID_HANDLE (reproducible Windows quirk, ev probe x5);
    //  - a process handle lacking PROCESS_QUERY_[LIMITED_]INFORMATION:
    //    FALSE + ERROR_ACCESS_DENIED.
    constexpr u32 STILL_ACTIVE = 259;
    constexpr u32 PROCESS_QUERY_INFORMATION = 0x0400;
    constexpr u32 PROCESS_QUERY_LIMITED_INFORMATION = 0x1000;
    const auto h_process = static_cast<Handle>(m.arg(0));
    const GuestAddr exit_code_ptr = m.arg_ptr(1);
    const auto write_code = [&](u32 code) {
        if (m_memory.write_u32(exit_code_ptr, code).is_err()) {
            set_last_error(error_codes::ERROR_NOACCESS);
            return false;
        }
        return true;
    };
    if (h_process == CURRENT_PROCESS_PSEUDO) {
        m.set_return(write_code(STILL_ACTIVE) ? 1u : 0u);
        return;
    }
    const HandleType type = m_handle_table != nullptr
        ? m_handle_table->get_type(h_process) : HandleType::Invalid;
    if (type == HandleType::Event) {
        if (!write_code(0)) { m.set_return(0u); return; }
        set_last_error(error_codes::ERROR_INVALID_HANDLE);
        m.set_return(1u);
        return;
    }
    u32 code = 0;
    if (type != HandleType::Process || m_sync_manager == nullptr ||
        !m_sync_manager->process_exit_code(h_process, code)) {
        set_last_error(error_codes::ERROR_INVALID_HANDLE);
        m.set_return(0u);
        return;
    }
    const u32 access = m_handle_table->get_access(h_process);
    if (access != HandleAccess::All &&
        (access & (PROCESS_QUERY_INFORMATION |
                   PROCESS_QUERY_LIMITED_INFORMATION)) == 0) {
        set_last_error(error_codes::ERROR_ACCESS_DENIED);
        m.set_return(0u);
        return;
    }
    m.set_return(write_code(code) ? 1u : 0u);
}

void Kernel32ExtManager::impl_CreateProcessA(Marshaler& m) {
    // CreateProcessA has the same process, inheritance, startup-info and
    // PROCESS_INFORMATION contracts as CreateProcessW; only its string
    // arguments differ. Keep both paths on the same implementation so ANSI
    // launches do not silently lose inherited IPC handles or hThread.
    impl_CreateProcessCommon(
        m, 0, std::nullopt, /*with_token_w_layout=*/false,
        /*ansi_strings=*/true);
}

void Kernel32ExtManager::impl_LoadModule(Marshaler& m) {
    // DWORD LoadModule(LPCSTR lpModuleName, LPVOID lpParameterBlock)
    //
    // The legacy 16-bit-era launcher. Windows implements it over CreateProcess
    // and returns a value > 31 on success, or a small error code (2/3/11).
    // lpParameterBlock is LOADPARMS32:
    //   +0 lpEnvAddress, +4 lpCmdLine, +8 lpCmdShow, +12 dwReserved
    // lpCmdLine points at a Pascal-style command tail: a length byte followed
    // by that many characters (no null terminator required).
    constexpr u64 LOAD_SUCCESS = 33; // any value > 31

    GuestAddr module_name_ptr = m.arg_ptr(0);
    GuestAddr parameter_block_ptr = m.arg_ptr(1);

    std::string module_name;
    if (module_name_ptr != 0) {
        auto r = m.read_ansi_string(module_name_ptr, 4096);
        if (r.is_ok()) module_name = r.value();
    }
    if (module_name.empty()) {
        VITREO_TRACE("[k32ext] LoadModule: empty module name\n");
        set_last_error(error_codes::ERROR_FILE_NOT_FOUND);
        m.set_return(error_codes::ERROR_FILE_NOT_FOUND);
        return;
    }

    std::string command_tail;
    if (parameter_block_ptr != 0) {
        const auto cmd_ptr = m.read_u32(parameter_block_ptr + 4);
        if (cmd_ptr.is_ok() && cmd_ptr.value() != 0) {
            const GuestAddr tail = cmd_ptr.value();
            const auto length = m.read_u8(tail);
            if (length.is_ok()) {
                for (u32 i = 0; i < length.value(); ++i) {
                    auto ch = m.read_u8(tail + 1 + i);
                    if (ch.is_err()) break;
                    command_tail.push_back(static_cast<char>(ch.value()));
                }
            }
        }
    }

    auto executable = resolve_guest_executable(m_vfs, module_name);
    if (executable.is_err()) {
        VITREO_TRACE("[k32ext] LoadModule: resolve failed for '%s'\n",
                     module_name.c_str());
        set_last_error(error_codes::ERROR_FILE_NOT_FOUND);
        m.set_return(error_codes::ERROR_FILE_NOT_FOUND);
        return;
    }

    const auto& [host_path, temporary] = executable.value();
    const bool current_elevated =
        m_elevation != nullptr && m_elevation->is_elevated();
    if (!current_elevated &&
        pe::requests_elevation(requested_execution_level(host_path))) {
        if (temporary) (void)::unlink(host_path.c_str());
        VITREO_TRACE(
            "[k32ext] LoadModule('%s') -> elevation required\n",
            module_name.c_str());
        set_last_error(error_codes::ERROR_ELEVATION_REQUIRED);
        m.set_return(error_codes::ERROR_ACCESS_DENIED);
        return;
    }
    const std::string command_line =
        command_tail.empty() ? module_name : module_name + " " + command_tail;
    // Learn LoadModule (LOADPARMS32.lpEnvAddress): NULL starts the new
    // process with the calling process's environment.
    std::vector<std::string> guest_environment;
    if (parameter_block_ptr == 0 ||
        m.read_u32(parameter_block_ptr).value_or(1) == 0) {
        guest_environment = inherited_guest_environment();
    }
    pid_t child_pid = static_cast<pid_t>(
        launch_guest_worker(
            host_path, module_name, command_line, {}, {}, {}, {},
            current_elevated ? m_elevation->credential()
                             : std::string_view{},
            {}, std::move(guest_environment)));
    if (temporary) {
        (void)::unlink(host_path.c_str());
    }
    if (child_pid < 0) {
        VITREO_TRACE("[k32ext] LoadModule('%s') -> launch failed\n",
                     module_name.c_str());
        set_last_error(error_codes::ERROR_ACCESS_DENIED);
        m.set_return(error_codes::ERROR_ACCESS_DENIED);
        return;
    }

    VITREO_TRACE("[k32ext] LoadModule('%s' tail='%s') -> %llu (pid=%d)\n",
                 module_name.c_str(), command_tail.c_str(),
                 static_cast<unsigned long long>(LOAD_SUCCESS),
                 static_cast<int>(child_pid));
    set_last_error(error_codes::ERROR_SUCCESS);
    m.set_return(LOAD_SUCCESS);
}

} // namespace vitreo::winapi
