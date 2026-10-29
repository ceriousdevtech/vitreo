// SPDX-License-Identifier: LGPL-2.1-only
#include "winapi/kernel32/system/machine_identity_registry.hpp"

#include "capsule/registry/registry.hpp"
#include "winapi/kernel32/environment/environment.hpp"

#include <cwctype>
#include <string>

namespace vitreo::winapi {

namespace {

using capsule::RegType;
using capsule::RegValue;

std::wstring widen(const std::string& text) { return std::wstring(text.begin(), text.end()); }

void set_text(capsule::Registry& registry, capsule::RegHandle key, const wchar_t* name, RegType type,
              const std::wstring& value) {
    (void)registry.set_value(key, name, RegValue{type, {value}});
}

void set_text_if_absent(capsule::Registry& registry, capsule::RegHandle key, const wchar_t* name, RegType type,
                        const std::wstring& value) {
    if (registry.query_value(key, name).is_ok()) return;
    set_text(registry, key, name, type, value);
}

} // namespace

void publish_machine_identity(capsule::Registry& registry, const std::string& computer_name,
                              const EnvironmentManager& environment) {
    // Measured on Windows 11 (machine_identity_registry.expected):
    // ComputerName\ComputerName holds ComputerName (the NetBIOS name, upper
    // case) beside a legacy default value "mnmsrvc"; ActiveComputerName holds
    // ComputerName only; Tcpip\Parameters holds Hostname and NV Hostname (the
    // DNS host name, case preserved). All REG_SZ.
    std::wstring netbios = widen(computer_name);
    for (wchar_t& c : netbios) c = static_cast<wchar_t>(std::towupper(c));
    const std::wstring dns = widen(computer_name);
    constexpr const wchar_t* kComputerName = L"SYSTEM\\CurrentControlSet\\Control\\ComputerName";
    if (auto key = registry.open_key(capsule::HKEY_LOCAL_MACHINE,
                                     std::wstring(kComputerName) + L"\\ComputerName", true);
        key.is_ok()) {
        set_text(registry, key.value(), L"ComputerName", RegType::Sz, netbios);
        set_text_if_absent(registry, key.value(), L"", RegType::Sz, L"mnmsrvc");
        registry.close_key(key.value());
    }
    if (auto key = registry.open_key(capsule::HKEY_LOCAL_MACHINE,
                                     std::wstring(kComputerName) + L"\\ActiveComputerName", true);
        key.is_ok()) {
        set_text(registry, key.value(), L"ComputerName", RegType::Sz, netbios);
        registry.close_key(key.value());
    }
    if (auto key = registry.open_key(capsule::HKEY_LOCAL_MACHINE,
                                     L"SYSTEM\\CurrentControlSet\\Services\\Tcpip\\Parameters", true);
        key.is_ok()) {
        set_text(registry, key.value(), L"Hostname", RegType::Sz, dns);
        set_text(registry, key.value(), L"NV Hostname", RegType::Sz, dns);
        registry.close_key(key.value());
    }

    // Session Manager\Environment: the system half of every process's
    // environment. Names, types and %SystemRoot% forms measured on Windows 11;
    // the values agree with Vitreo's environment (Path = C:\Windows\System32;
    // C:\Windows, the capsule's DriverData, ...).
    auto env = registry.open_key(capsule::HKEY_LOCAL_MACHINE,
                                 L"SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Environment", true);
    if (env.is_err()) return;
    const auto variable = [&](const char* name, const char* fallback) {
        auto value = environment.get_variable(name);
        return widen(value.is_ok() ? value.value() : std::string(fallback));
    };
    set_text_if_absent(registry, env.value(), L"ComSpec", RegType::ExpandSz, L"%SystemRoot%\\system32\\cmd.exe");
    set_text_if_absent(registry, env.value(), L"DriverData", RegType::Sz,
                       variable("DriverData", "C:\\Windows\\System32\\Drivers\\DriverData"));
    set_text_if_absent(registry, env.value(), L"OS", RegType::Sz, L"Windows_NT");
    set_text_if_absent(registry, env.value(), L"PATHEXT", RegType::Sz,
                       L".COM;.EXE;.BAT;.CMD;.VBS;.VBE;.JS;.JSE;.WSF;.WSH;.MSC");
    set_text_if_absent(registry, env.value(), L"PSModulePath", RegType::ExpandSz,
                       L"%ProgramFiles%\\WindowsPowerShell\\Modules;"
                       L"%SystemRoot%\\system32\\WindowsPowerShell\\v1.0\\Modules");
    set_text_if_absent(registry, env.value(), L"TEMP", RegType::ExpandSz, L"%SystemRoot%\\TEMP");
    set_text_if_absent(registry, env.value(), L"TMP", RegType::ExpandSz, L"%SystemRoot%\\TEMP");
    set_text_if_absent(registry, env.value(), L"USERNAME", RegType::Sz, L"SYSTEM");
    set_text_if_absent(registry, env.value(), L"windir", RegType::ExpandSz, L"%SystemRoot%");
    set_text_if_absent(registry, env.value(), L"Path", RegType::ExpandSz, L"%SystemRoot%\\system32;%SystemRoot%");
    // The machine's architecture and processors, as the host has them now.
    set_text(registry, env.value(), L"PROCESSOR_ARCHITECTURE", RegType::Sz, L"AMD64");
    set_text(registry, env.value(), L"NUMBER_OF_PROCESSORS", RegType::Sz, variable("NUMBER_OF_PROCESSORS", "1"));
    set_text(registry, env.value(), L"PROCESSOR_IDENTIFIER", RegType::Sz, variable("PROCESSOR_IDENTIFIER", ""));
    set_text(registry, env.value(), L"PROCESSOR_LEVEL", RegType::Sz, variable("PROCESSOR_LEVEL", ""));
    set_text(registry, env.value(), L"PROCESSOR_REVISION", RegType::Sz, variable("PROCESSOR_REVISION", ""));
    registry.close_key(env.value());
}

} // namespace vitreo::winapi
