/// @file environment.cpp
/// @brief Implementation of environment and command line APIs (WIN32-035)

#include "environment.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/kernel32/locale/string_conv.hpp"
#include "winapi/core/marshaler.hpp"
#include "core/memory/guest_memory.hpp"
#include "core/cpu/cpuid_synth.hpp"
#include "winapi/kernel32/system/sysinfo.hpp"
#include <vitreo/diagnostics.hpp>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>
#include <mutex>
#include <string_view>
#include <vector>

namespace vitreo::winapi {

namespace {

/// Count UTF-16 code units needed for a UTF-8 string (no trailing NUL).
[[nodiscard]] u32 utf8_utf16_units(std::string_view utf8) noexcept {
    u32 units = 0;
    usize i = 0;
    while (i < utf8.size()) {
        const u8 b = static_cast<u8>(utf8[i]);
        u32 cp = 0;
        if ((b & 0x80) == 0) {
            cp = b;
            i += 1;
        } else if ((b & 0xE0) == 0xC0 && i + 1 < utf8.size()) {
            cp = ((b & 0x1F) << 6) | (static_cast<u8>(utf8[i + 1]) & 0x3F);
            i += 2;
        } else if ((b & 0xF0) == 0xE0 && i + 2 < utf8.size()) {
            cp = ((b & 0x0F) << 12) |
                 ((static_cast<u8>(utf8[i + 1]) & 0x3F) << 6) |
                 (static_cast<u8>(utf8[i + 2]) & 0x3F);
            i += 3;
        } else if ((b & 0xF8) == 0xF0 && i + 3 < utf8.size()) {
            cp = ((b & 0x07) << 18) |
                 ((static_cast<u8>(utf8[i + 1]) & 0x3F) << 12) |
                 ((static_cast<u8>(utf8[i + 2]) & 0x3F) << 6) |
                 (static_cast<u8>(utf8[i + 3]) & 0x3F);
            i += 4;
        } else {
            ++i;
            continue;
        }
        units += (cp < 0x10000) ? 1u : 2u;
    }
    return units;
}

} // namespace

// =============================================================================
// Construction
// =============================================================================

EnvironmentManager::EnvironmentManager(std::string_view command_line,
                                       GuestMemory& memory)
    : m_command_line(command_line) {
    populate_defaults();

    // Map command-line slots: A at base, W at +0x8000 (no overlap; ≤32767 chars).
    auto result = memory.map_region(ENV_DATA_BASE, ENV_DATA_SIZE,
                                     MemoryProtection::ReadWrite);
    if (result.is_ok()) {
        // Learn GetCommandLineA: CP_ACP (not host UTF-8). ® is 0xAE.
        const std::string acp = utf8_to_acp(m_command_line);
        const usize a_copy =
            std::min(acp.size(), static_cast<usize>(CMDLINE_A_MAX - 1));
        GuestAddr ansi_addr = ENV_DATA_BASE;
        for (usize i = 0; i < a_copy; ++i) {
            (void)memory.write_u8(ansi_addr + i, static_cast<u8>(acp[i]));
        }
        (void)memory.write_u8(ansi_addr + a_copy, 0);
        m_cmdline_a_addr = ansi_addr;

        // Learn GetCommandLineW: UTF-16, not widened UTF-8 bytes.
        const std::vector<u16> wide = utf8_to_utf16(m_command_line);
        const usize w_copy = std::min(
            wide.size(), static_cast<usize>(CMDLINE_W_MAX / 2 - 1));
        GuestAddr wide_addr = ENV_DATA_BASE + CMDLINE_W_OFF;
        for (usize i = 0; i < w_copy; ++i) {
            (void)memory.write_u16(wide_addr + i * 2, wide[i]);
        }
        (void)memory.write_u16(wide_addr + w_copy * 2, 0);
        m_cmdline_w_addr = wide_addr;
        m_cmdline_w_bytes = static_cast<u16>(
            std::min<usize>(w_copy * 2, 0xFFFEu));
    }
}

// =============================================================================
// Variable Management
// =============================================================================

void EnvironmentManager::set_variable(std::string_view name, std::string_view value) {
    std::lock_guard<std::mutex> lock(m_lock);
    std::string key = uppercase_key(name);
    auto it = m_env_vars.find(key);
    if (it == m_env_vars.end()) {
        m_env_vars.emplace(std::move(key), EnvEntry{std::string(name), std::string(value)});
    } else {
        // Preserve the first-seen display casing (Windows keeps the existing
        // name spelling when an existing variable is updated).
        it->second.value = std::string(value);
    }
}

void EnvironmentManager::delete_variable(std::string_view name) {
    std::lock_guard<std::mutex> lock(m_lock);
    std::string key = uppercase_key(name);
    m_env_vars.erase(key);
}

VResult<std::string> EnvironmentManager::get_variable(std::string_view name) const {
    std::lock_guard<std::mutex> lock(m_lock);
    std::string key = uppercase_key(name);
    auto it = m_env_vars.find(key);
    if (it == m_env_vars.end()) {
        return VResult<std::string>::err(Error::NotFound);
    }
    return VResult<std::string>::ok(it->second.value);
}

std::map<std::string, std::string> EnvironmentManager::get_all_variables() const {
    std::lock_guard<std::mutex> lock(m_lock);
    std::map<std::string, std::string> out;
    for (const auto& [key, entry] : m_env_vars) {
        out.emplace(key, entry.value);
    }
    return out;
}

std::vector<std::pair<std::string, std::string>>
EnvironmentManager::get_all_variables_as_named() const {
    std::lock_guard<std::mutex> lock(m_lock);
    std::vector<std::pair<std::string, std::string>> out;
    out.reserve(m_env_vars.size());
    for (const auto& [key, entry] : m_env_vars) {
        (void)key;
        out.emplace_back(entry.display_name, entry.value);
    }
    return out;
}

// =============================================================================
// Command Line
// =============================================================================

std::string EnvironmentManager::get_command_line_a() const {
    return m_command_line;
}

std::string EnvironmentManager::get_command_line_w_utf8() const {
    return m_command_line;
}

// =============================================================================
// Environment Block
// =============================================================================

std::string EnvironmentManager::build_environment_block() const {
    std::lock_guard<std::mutex> lock(m_lock);
    std::string block;
    for (const auto& [key, entry] : m_env_vars) {
        (void)key;
        block += entry.display_name;
        block += '=';
        block += entry.value;
        block += '\0';
    }
    // Terminate with an extra null (double-null termination)
    block += '\0';
    return block;
}

GuestAddr EnvironmentManager::allocate_environment_strings_snapshot(
    GuestMemory& memory, bool wide) {
    const std::string block = build_environment_block();

    u64 need_bytes = 0;
    if (wide) {
        usize i = 0;
        u32 units = 0;
        while (i < block.size()) {
            if (static_cast<u8>(block[i]) == 0) {
                ++units;
                ++i;
                continue;
            }
            usize end = i;
            while (end < block.size() && static_cast<u8>(block[end]) != 0) ++end;
            // The entry and its terminating null; the null is consumed here.
            units += utf8_utf16_units(std::string_view(block.data() + i, end - i)) + 1;
            i = end + 1;
        }
        need_bytes = static_cast<u64>(units) * 2;
    } else {
        need_bytes = block.size();
    }
    if (need_bytes == 0) need_bytes = wide ? 4 : 2;
    constexpr u64 kPage = 0x1000;
    const u64 map_bytes = (need_bytes + kPage - 1) & ~(kPage - 1);

    constexpr GuestAddr kMin = ENV_DATA_BASE + ENV_DATA_SIZE;
    GuestAddr base = memory.find_free_gap(kMin, static_cast<usize>(map_bytes));
    if (base == 0 ||
        memory.map_region(base, static_cast<usize>(map_bytes),
                          MemoryProtection::ReadWrite).is_err()) {
        return 0;
    }

    if (wide) {
        GuestAddr cursor = base;
        usize i = 0;
        while (i < block.size()) {
            if (static_cast<u8>(block[i]) == 0) {
                (void)memory.write_u16(cursor, 0);
                cursor += 2;
                ++i;
                continue;
            }
            usize end = i;
            while (end < block.size() && static_cast<u8>(block[end]) != 0) ++end;
            const auto units =
                utf8_to_utf16(std::string_view(block.data() + i, end - i));
            for (u16 u : units) {
                (void)memory.write_u16(cursor, u);
                cursor += 2;
            }
            // "name=value\0": the terminator is written here, so the source
            // null is consumed. Leaving it for the loop wrote a second null
            // after every entry, which ends the block after its first
            // variable (Learn GetEnvironmentStringsW: entries separated by a
            // null, the block ended by two).
            (void)memory.write_u16(cursor, 0);
            cursor += 2;
            i = end + 1;
        }
    } else {
        for (usize i = 0; i < block.size(); ++i) {
            (void)memory.write_u8(base + i, static_cast<u8>(block[i]));
        }
    }

    {
        std::lock_guard<std::mutex> lock(m_lock);
        m_env_snapshots.push_back(EnvSnapshot{base, map_bytes, wide});
    }
    return base;
}

bool EnvironmentManager::free_environment_strings_snapshot(
    GuestMemory& memory, GuestAddr block, bool wide) {
    if (block == 0) return true; // Windows FreeEnvironmentStringsW(NULL) → TRUE
    std::lock_guard<std::mutex> lock(m_lock);
    for (auto it = m_env_snapshots.begin(); it != m_env_snapshots.end(); ++it) {
        if (it->addr == block && it->wide == wide) {
            const u64 bytes = it->bytes;
            m_env_snapshots.erase(it);
            (void)memory.unmap_region(block, static_cast<usize>(bytes));
            return true;
        }
    }
    return true; // unknown pointer: succeed (Windows does not fail Free)
}

// =============================================================================
// Default Population
// =============================================================================

void EnvironmentManager::populate_defaults() {
    // Use Windows-common display casing. A sandbox filter matches
    // Path, SystemRoot, SystemDrive, TEMP, TMP, and LOCALAPPDATA exactly when
    // copying a filtered CreateProcess environment block.
    set_variable("Path",         "C:\\Windows\\System32;C:\\Windows");
    set_variable("SystemDrive",  "C:");
    set_variable("SystemRoot",   "C:\\Windows");
    set_variable("TEMP",         "C:\\Users\\Default\\AppData\\Local\\Temp");
    set_variable("TMP",          "C:\\Users\\Default\\AppData\\Local\\Temp");
    set_variable("LOCALAPPDATA", "C:\\Users\\Default\\AppData\\Local");
    set_variable("USERPROFILE",  "C:\\Users\\Default");
    set_variable("USERNAME",     "Default");
    set_variable("COMPUTERNAME", "VITREO");

    // The rest of a Windows 11 logon environment (Windows `set`, 64-bit
    // and SysWOW64 cmd.exe): system variables from Session Manager\
    // Environment and the per-user profile variables. Paths match the
    // capsule layout and the shell-folder values (C:\Users\Default profile,
    // C:\ProgramData, C:\Users\Public). Bitness-dependent names are set by
    // apply_process_bitness().
    set_variable("ALLUSERSPROFILE", "C:\\ProgramData");
    set_variable("ProgramData",     "C:\\ProgramData");
    set_variable("PUBLIC",          "C:\\Users\\Public");
    set_variable("APPDATA",         "C:\\Users\\Default\\AppData\\Roaming");
    set_variable("HOMEDRIVE",       "C:");
    set_variable("HOMEPATH",        "\\Users\\Default");
    set_variable("USERDOMAIN",      "VITREO");
    set_variable("USERDOMAIN_ROAMINGPROFILE", "VITREO");
    set_variable("LOGONSERVER",     "\\\\VITREO");
    set_variable("SESSIONNAME",     "Console");
    set_variable("windir",          "C:\\Windows");
    set_variable("ComSpec",         "C:\\Windows\\system32\\cmd.exe");
    set_variable("OS",              "Windows_NT");
    set_variable("PATHEXT",
                 ".COM;.EXE;.BAT;.CMD;.VBS;.VBE;.JS;.JSE;.WSF;.WSH;.MSC;.CPL");
    set_variable("DriverData",
                 "C:\\Windows\\System32\\Drivers\\DriverData");
    set_variable("ProgramFiles(x86)",       "C:\\Program Files (x86)");
    set_variable("ProgramW6432",            "C:\\Program Files");
    set_variable("CommonProgramFiles(x86)",
                 "C:\\Program Files (x86)\\Common Files");
    set_variable("CommonProgramW6432",      "C:\\Program Files\\Common Files");
    set_variable("PSModulePath",
                 "C:\\Users\\Default\\Documents\\WindowsPowerShell\\Modules;"
                 "C:\\Program Files\\WindowsPowerShell\\Modules;"
                 "C:\\Windows\\system32\\WindowsPowerShell\\v1.0\\Modules");
    set_variable("NUMBER_OF_PROCESSORS",
                 std::to_string(host_active_processor_count()));

    // PROCESSOR_IDENTIFIER / LEVEL / REVISION describe the processor the
    // guest's CPUID reports (the same leaf-1 signature GetSystemInfo uses):
    // "Intel64 Family 6 Model 151 Stepping 2, GenuineIntel", level "6",
    // revision "9702" (model, stepping as two hex bytes).
    u32 a = 0, b = 0, c = 0, d = 0;
    cpu::ops::synth_cpuid(0, 0, true, a, b, c, d);
    char vendor[13] = {};
    std::memcpy(vendor + 0, &b, 4);
    std::memcpy(vendor + 4, &d, 4);
    std::memcpy(vendor + 8, &c, 4);
    cpu::ops::synth_cpuid(1, 0, true, a, b, c, d);
    u32 family = (a >> 8) & 0xFu;
    u32 model = (a >> 4) & 0xFu;
    const u32 stepping = a & 0xFu;
    if (family == 0xFu) family += (a >> 20) & 0xFFu;
    if (family == 6u || family >= 0xFu) model |= ((a >> 16) & 0xFu) << 4;
    const std::string vendor_str(vendor);
    const char* arch_word = vendor_str == "AuthenticAMD" ? "AMD64" : "Intel64";
    char buf[128];
    std::snprintf(buf, sizeof(buf), "%s Family %u Model %u Stepping %u, %s",
                  arch_word, family, model, stepping, vendor);
    set_variable("PROCESSOR_IDENTIFIER", buf);
    set_variable("PROCESSOR_LEVEL", std::to_string(family));
    std::snprintf(buf, sizeof(buf), "%02x%02x", model & 0xFFu, stepping);
    set_variable("PROCESSOR_REVISION", buf);

    apply_process_bitness(true);
}

void EnvironmentManager::apply_process_bitness(bool is_64bit) {
    // WOW64 gives a 32-bit process its own view of these names, whatever
    // environment block it inherited (SysWOW64 cmd.exe on Windows 11).
    if (is_64bit) {
        set_variable("PROCESSOR_ARCHITECTURE", "AMD64");
        delete_variable("PROCESSOR_ARCHITEW6432");
        set_variable("ProgramFiles",       "C:\\Program Files");
        set_variable("CommonProgramFiles", "C:\\Program Files\\Common Files");
    } else {
        set_variable("PROCESSOR_ARCHITECTURE", "x86");
        set_variable("PROCESSOR_ARCHITEW6432", "AMD64");
        set_variable("ProgramFiles",       "C:\\Program Files (x86)");
        set_variable("CommonProgramFiles",
                     "C:\\Program Files (x86)\\Common Files");
    }
}

// =============================================================================
// Win32 Thunk Implementations
// =============================================================================

void impl_GetCommandLineA(Marshaler& m, EnvironmentManager& em) {
    // LPSTR GetCommandLineA(void)
    m.set_return(static_cast<u64>(em.cmdline_a_addr()));
}

void impl_GetCommandLineW(Marshaler& m, EnvironmentManager& em) {
    // LPWSTR GetCommandLineW(void)
    m.set_return(static_cast<u64>(em.cmdline_w_addr()));
}

void impl_GetEnvironmentVariableA(Marshaler& m, EnvironmentManager& em) {
    // DWORD GetEnvironmentVariableA(LPCSTR lpName, LPSTR lpBuffer, DWORD nSize)
    GuestAddr name_ptr = m.arg_ptr(0);
    GuestAddr buf_ptr = m.arg_ptr(1);
    u32 buf_size = static_cast<u32>(m.arg(2));

    auto name_result = m.read_ansi_string(name_ptr);
    if (!name_result.is_ok()) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return;
    }

    auto var_result = em.get_variable(name_result.value());
    if (!var_result.is_ok()) {
        set_last_error(error_codes::ERROR_ENVVAR_NOT_FOUND);
        m.set_return(0);
        return;
    }

    const std::string& value = var_result.value();
    u32 required_size = static_cast<u32>(value.size() + 1);

    if (buf_ptr == 0 || buf_size < required_size) {
        // Measured on Windows 11 (last_error_on_success_probe.expected): the
        // required-size answer is not a failure and leaves the last error
        // untouched, as does a successful copy.
        m.set_return(static_cast<u64>(required_size));
        return;
    }

    (void)m.write_ansi_string(buf_ptr, value);
    m.set_return(static_cast<u64>(value.size()));
}

void impl_GetEnvironmentVariableW(Marshaler& m, EnvironmentManager& em) {
    // DWORD GetEnvironmentVariableW(LPCWSTR lpName, LPWSTR lpBuffer, DWORD nSize)
    GuestAddr name_ptr = m.arg_ptr(0);
    GuestAddr buf_ptr = m.arg_ptr(1);
    u32 buf_size = static_cast<u32>(m.arg(2));

    auto name_result = m.read_wide_string(name_ptr);
    if (!name_result.is_ok()) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return;
    }

    auto var_result = em.get_variable(name_result.value());
    if (!var_result.is_ok()) {
        VITREO_TRACE("[env] GetEnvironmentVariableW(\"%s\") -> 0 "
                     "ERROR_ENVVAR_NOT_FOUND\n", name_result.value().c_str());
        set_last_error(error_codes::ERROR_ENVVAR_NOT_FOUND);
        m.set_return(0);
        return;
    }

    const std::string& value = var_result.value();
    const u32 wchar_count = utf8_utf16_units(value);
    const u32 required_size = wchar_count + 1; // includes terminating NUL

    if (buf_ptr == 0 || buf_size < required_size) {
        VITREO_TRACE("[env] GetEnvironmentVariableW(\"%s\") buf=0x%llx "
                     "size=%u -> %u (needs buffer)\n",
                     name_result.value().c_str(),
                     static_cast<unsigned long long>(buf_ptr), buf_size,
                     required_size);
        // Not a failure: the last error stays untouched (measured, as above).
        m.set_return(static_cast<u64>(required_size));
        return;
    }

    (void)m.write_wide_string(buf_ptr, value);
    VITREO_TRACE("[env] GetEnvironmentVariableW(\"%s\") -> \"%s\"\n",
                 name_result.value().c_str(), value.c_str());
    m.set_return(static_cast<u64>(wchar_count));
}

void impl_SetEnvironmentVariableA(Marshaler& m, EnvironmentManager& em) {
    // BOOL SetEnvironmentVariableA(LPCSTR lpName, LPCSTR lpValue)
    GuestAddr name_ptr = m.arg_ptr(0);
    GuestAddr value_ptr = m.arg_ptr(1);

    auto name_result = m.read_ansi_string(name_ptr);
    if (!name_result.is_ok()) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return;
    }
    const std::string& name = name_result.value();
    // A leading '=' is a real name (the hidden per-drive cwd vars "=C:", and
    // even "=" / "=C"). An '=' anywhere after the first character is rejected.
    // Measured on Windows 11 (env_eq_probe): "=C:", "=", and "=C" succeed and leave
    // the last error untouched; "A=B" and "==C:" return FALSE / 0x57.
    if (name.empty() || name.find('=', 1) != std::string::npos ||
        name.size() > 32767) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return;
    }

    if (value_ptr == 0) {
        em.delete_variable(name);
    } else {
        auto value_result = m.read_ansi_string(value_ptr);
        if (!value_result.is_ok() || value_result.value().size() > 32767) {
            set_last_error(error_codes::ERROR_INVALID_PARAMETER);
            m.set_return(0);
            return;
        }
        em.set_variable(name, value_result.value());
    }

    m.set_return(1);
}

void impl_SetEnvironmentVariableW(Marshaler& m, EnvironmentManager& em) {
    // BOOL SetEnvironmentVariableW(LPCWSTR lpName, LPCWSTR lpValue)
    GuestAddr name_ptr = m.arg_ptr(0);
    GuestAddr value_ptr = m.arg_ptr(1);

    auto name_result = m.read_wide_string(name_ptr);
    if (!name_result.is_ok()) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return;
    }
    const std::string& name = name_result.value();
    // Same rule as the ANSI entry: a leading '=' is legal (UCRT _wchdir sets
    // "=C:" this way). An '=' after the first character is not.
    if (name.empty() || name.find('=', 1) != std::string::npos ||
        utf8_utf16_units(name) > 32767) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return;
    }

    if (value_ptr == 0) {
        em.delete_variable(name);
    } else {
        auto value_result = m.read_wide_string(value_ptr);
        if (!value_result.is_ok() ||
            utf8_utf16_units(value_result.value()) > 32767) {
            set_last_error(error_codes::ERROR_INVALID_PARAMETER);
            m.set_return(0);
            return;
        }
        em.set_variable(name, value_result.value());
    }

    m.set_return(1);
}

void impl_GetEnvironmentStringsW(Marshaler& m, EnvironmentManager& em) {
    // LPWCH GetEnvironmentStringsW(void)
    // Each call returns a fresh snapshot; FreeEnvironmentStringsW releases it.
    const GuestAddr addr =
        em.allocate_environment_strings_snapshot(m.memory(), /*wide=*/true);
    if (addr == 0) {
        set_last_error(error_codes::ERROR_NOT_ENOUGH_MEMORY);
        m.set_return(0);
        return;
    }
    m.set_return(addr);
}

void impl_GetEnvironmentStrings(Marshaler& m, EnvironmentManager& em) {
    // LPCH GetEnvironmentStringsA(void)
    const GuestAddr addr =
        em.allocate_environment_strings_snapshot(m.memory(), /*wide=*/false);
    if (addr == 0) {
        set_last_error(error_codes::ERROR_NOT_ENOUGH_MEMORY);
        m.set_return(0);
        return;
    }
    m.set_return(addr);
}

void impl_FreeEnvironmentStringsA(Marshaler& m, EnvironmentManager& em) {
    // BOOL FreeEnvironmentStringsA(LPCH lpszEnvironmentBlock)
    // Windows: NULL → TRUE without last-error change; distinct Get* pointers.
    const GuestAddr block = m.arg_ptr(0);
    (void)em.free_environment_strings_snapshot(m.memory(), block, /*wide=*/false);
    m.set_return(1);
}

void impl_FreeEnvironmentStringsW(Marshaler& m, EnvironmentManager& em) {
    // BOOL FreeEnvironmentStringsW(LPWCH lpszEnvironmentBlock)
    const GuestAddr block = m.arg_ptr(0);
    (void)em.free_environment_strings_snapshot(m.memory(), block, /*wide=*/true);
    m.set_return(1);
}

// =============================================================================
// Export Table Factory
// =============================================================================

DllExports make_environment_exports(EnvironmentManager& em) {
    DllExports exports;
    exports.entries.reserve(8);

    exports.entries.push_back(ExportEntry{
        "GetCommandLineA", 1,
        [&em](Marshaler& m) { impl_GetCommandLineA(m, em); }
    });
    exports.entries.push_back(ExportEntry{
        "GetCommandLineW", 2,
        [&em](Marshaler& m) { impl_GetCommandLineW(m, em); }
    });
    exports.entries.push_back(ExportEntry{
        "GetEnvironmentVariableA", 3,
        [&em](Marshaler& m) { impl_GetEnvironmentVariableA(m, em); },
        12
    });
    exports.entries.push_back(ExportEntry{
        "GetEnvironmentVariableW", 4,
        [&em](Marshaler& m) { impl_GetEnvironmentVariableW(m, em); },
        12
    });
    exports.entries.push_back(ExportEntry{
        "SetEnvironmentVariableA", 5,
        [&em](Marshaler& m) { impl_SetEnvironmentVariableA(m, em); },
        8
    });
    exports.entries.push_back(ExportEntry{
        "SetEnvironmentVariableW", 6,
        [&em](Marshaler& m) { impl_SetEnvironmentVariableW(m, em); },
        8
    });
    exports.entries.push_back(ExportEntry{
        "GetEnvironmentStringsW", 7,
        [&em](Marshaler& m) { impl_GetEnvironmentStringsW(m, em); }
    });
    exports.entries.push_back(ExportEntry{
        "GetEnvironmentStrings", 0,
        [&em](Marshaler& m) { impl_GetEnvironmentStrings(m, em); }
    });
    exports.entries.push_back(ExportEntry{
        "FreeEnvironmentStringsA", 0,
        [&em](Marshaler& m) { impl_FreeEnvironmentStringsA(m, em); },
        4
    });
    exports.entries.push_back(ExportEntry{
        "FreeEnvironmentStringsW", 8,
        [&em](Marshaler& m) { impl_FreeEnvironmentStringsW(m, em); },
        4
    });

    return exports;
}

} // namespace vitreo::winapi
