// SPDX-License-Identifier: LGPL-2.1-only

#include "winapi/system/setupapi/setupapi_internal.hpp"

#include <atomic>
#include <cctype>
#include <cstdio>

namespace vitreo::winapi::setupapi_internal {

namespace {

constexpr SetupClass kSetupClasses[] = {
#include "winapi/system/setupapi/setup_classes.inc"
};

constexpr u32 ERROR_INVALID_MACHINENAME_SETUP = 0xE0000220u;  // SetupAPI.h

[[nodiscard]] bool ascii_iequal(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (usize i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

/// The Ex forms' MachineName (NULL or "" is this machine) and Reserved
/// (must be NULL: ExW Ports_reserved -> 87). False with the error set.
[[nodiscard]] bool check_local_machine(Marshaler& m, bool wide, GuestAddr machine,
                                       GuestAddr reserved, const char* api) {
    if (reserved != 0) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        return false;
    }
    if (machine == 0) return true;
    auto name = wide ? m.read_wide_string(machine) : m.read_ansi_string(machine);
    if (name.is_ok() && name.value().empty()) return true;  // Ports_emptymachine
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true)) {
        std::fprintf(stderr, "[vitreo] UNIMPLEMENTED setupapi!%s: remote MachineName\n", api);
    }
    set_last_error(ERROR_INVALID_MACHINENAME_SETUP);
    return false;
}

} // namespace

/// BOOL SetupDiClassGuidsFromName[Ex]{A,W}(PCTSTR ClassName, LPGUID
///     ClassGuidList, DWORD ClassGuidListSize, PDWORD RequiredSize
///     [, PCTSTR MachineName, PVOID Reserved])
/// setup_class_probe.expected: names match case-insensitively and exactly (a
/// trailing space matches nothing); an unknown or empty name succeeds with 0
/// GUIDs; the GUIDs that fit are written, then a short list fails 122 with the
/// full count; a NULL ClassName or RequiredSize fails 87, the latter after the
/// GUIDs are written; success sets ERROR_SUCCESS.
void class_guids_from_name(Marshaler& m, bool wide, bool ex) {
    const GuestAddr name_ptr = m.arg_ptr(0);
    const GuestAddr list = m.arg_ptr(1);
    const u32 list_size = m.arg32(2);
    const GuestAddr required = m.arg_ptr(3);
    const char* api = wide ? (ex ? "SetupDiClassGuidsFromNameExW" : "SetupDiClassGuidsFromNameW")
                           : (ex ? "SetupDiClassGuidsFromNameExA" : "SetupDiClassGuidsFromNameA");
    if (ex && !check_local_machine(m, wide, m.arg_ptr(4), m.arg_ptr(5), api)) {
        m.set_return(0);
        return;
    }
    if (name_ptr == 0) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return;
    }
    auto name = wide ? m.read_wide_string(name_ptr) : m.read_ansi_string(name_ptr);
    if (name.is_err()) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return;
    }
    u32 found = 0;
    for (const SetupClass& c : kSetupClasses) {
        if (name.value().empty() || !ascii_iequal(c.name, name.value())) continue;
        if (found < list_size &&
            m.memory().write_bytes(list + found * 16u, c.guid.data(), c.guid.size()).is_err()) {
            set_last_error(ERROR_INVALID_USER_BUFFER);
            m.set_return(0);
            return;
        }
        ++found;
    }
    if (required == 0) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);  // Ports_nullreq
        m.set_return(0);
        return;
    }
    (void)m.write_u32(required, found);
    if (list_size < found) {
        set_last_error(error_codes::ERROR_INSUFFICIENT_BUFFER);  // fvevol_1
        m.set_return(0);
        return;
    }
    set_last_error(error_codes::ERROR_SUCCESS);
    m.set_return(1);
}

/// BOOL SetupDiClassNameFromGuid[Ex]{A,W}(const GUID*, PTSTR ClassName,
///     DWORD ClassNameSize, PDWORD RequiredSize [, PCTSTR, PVOID])
/// NameW rows: RequiredSize counts the NUL and is optional; a short buffer
/// fails 122 after reporting it; an unknown GUID fails ERROR_INVALID_CLASS
/// without touching it; a NULL GUID fails ERROR_INVALID_USER_BUFFER.
void class_name_from_guid(Marshaler& m, bool wide, bool ex) {
    const GuestAddr guid_ptr = m.arg_ptr(0);
    const GuestAddr out = m.arg_ptr(1);
    const u32 out_size = m.arg32(2);
    const GuestAddr required = m.arg_ptr(3);
    const char* api = wide ? (ex ? "SetupDiClassNameFromGuidExW" : "SetupDiClassNameFromGuidW")
                           : (ex ? "SetupDiClassNameFromGuidExA" : "SetupDiClassNameFromGuidA");
    if (ex && !check_local_machine(m, wide, m.arg_ptr(4), m.arg_ptr(5), api)) {
        m.set_return(0);
        return;
    }
    std::array<u8, 16> guid{};
    if (guid_ptr == 0 || m.memory().read_bytes(guid_ptr, guid.data(), guid.size()).is_err()) {
        set_last_error(ERROR_INVALID_USER_BUFFER);  // nullguid
        m.set_return(0);
        return;
    }
    const SetupClass* match = nullptr;
    for (const SetupClass& c : kSetupClasses) {
        if (c.guid == guid) { match = &c; break; }
    }
    if (match == nullptr) {
        set_last_error(ERROR_INVALID_CLASS);  // bogus
        m.set_return(0);
        return;
    }
    const std::string_view name = match->name;  // ASCII: one unit per char
    const u32 needed = static_cast<u32>(name.size()) + 1;
    if (required != 0) (void)m.write_u32(required, needed);
    if (out_size < needed) {
        set_last_error(error_codes::ERROR_INSUFFICIENT_BUFFER);  // ports_small
        m.set_return(0);
        return;
    }
    bool ok = true;
    for (usize i = 0; i <= name.size() && ok; ++i) {
        const u8 c = i < name.size() ? static_cast<u8>(name[i]) : 0;
        ok = wide ? m.memory().write_u16(out + i * 2, c).is_ok()
                  : m.memory().write_u8(out + i, c).is_ok();
    }
    if (!ok) {
        set_last_error(ERROR_INVALID_USER_BUFFER);
        m.set_return(0);
        return;
    }
    set_last_error(error_codes::ERROR_SUCCESS);
    m.set_return(1);
}

[[nodiscard]] const SetupClass* find_setup_class(const Guid& guid) {
    for (const SetupClass& c : kSetupClasses) {
        if (c.guid == guid) return &c;
    }
    return nullptr;
}

/// An installed setup class has HKLM\...\Control\Class\{guid} with its name
/// in "Class" (s7_setupapi_probe ocrk_disp). The class table records which
/// classes are installed; the key is materialized the first time it is
/// opened. Never creates a key for a class that is not installed.
void ensure_class_key(RegistryThunks* registry, const SetupClass& setup_class) {
    if (registry == nullptr) return;
    const std::string path = std::string(CLASS_KEY) + "\\" + guid_string(setup_class.guid);
    u64 key = 0;
    u32 disposition = 0;
    if (registry->reg_create_key_ex_w(HKEY_LOCAL_MACHINE_VALUE, widen(path), &key,
                                      &disposition) != error_codes::ERROR_SUCCESS) {
        return;
    }
    set_registry_value(registry, key, L"Class", REG_SZ, utf16_bytes(setup_class.name));
    (void)registry->reg_close_key(key);
}

} // namespace vitreo::winapi::setupapi_internal
