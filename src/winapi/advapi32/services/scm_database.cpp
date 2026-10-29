// SPDX-License-Identifier: LGPL-2.1-only
/// @file scm_database.cpp
/// @brief ScmManager registry-backed service database: create, query, enumerate,
///        change and delete.

#include "winapi/advapi32/services/scm_internal.hpp"

#include "capsule/registry/registry.hpp"

#include <algorithm>
#include <optional>
#include <string>
#include <vector>

namespace vitreo::winapi {

using namespace scm_detail;
using namespace service_constants;

namespace {

/// SERVICE_NO_CHANGE — ChangeServiceConfig's "leave this field alone" marker.
constexpr u32 SERVICE_NO_CHANGE = 0xFFFFFFFFu;

struct StoredFailureActions {
    u32 reset_period_seconds{0};
    std::wstring reboot_message;
    std::wstring command;
    std::vector<ServiceFailureAction> actions;
};

void append_u32_le(std::vector<u8>& bytes, u32 value) {
    bytes.push_back(static_cast<u8>(value));
    bytes.push_back(static_cast<u8>(value >> 8));
    bytes.push_back(static_cast<u8>(value >> 16));
    bytes.push_back(static_cast<u8>(value >> 24));
}

std::optional<u32> read_u32_le(const std::vector<u8>& bytes, usize offset) {
    if (offset > bytes.size() || bytes.size() - offset < sizeof(u32)) {
        return std::nullopt;
    }
    return static_cast<u32>(bytes[offset]) |
           (static_cast<u32>(bytes[offset + 1]) << 8) |
           (static_cast<u32>(bytes[offset + 2]) << 16) |
           (static_cast<u32>(bytes[offset + 3]) << 24);
}

u32 append_utf16_string(std::vector<u8>& bytes, const std::wstring& value) {
    if (value.empty()) return 0;
    const u32 offset = static_cast<u32>(bytes.size());
    for (wchar_t ch : value) {
        const u16 code_unit = static_cast<u16>(ch);
        bytes.push_back(static_cast<u8>(code_unit));
        bytes.push_back(static_cast<u8>(code_unit >> 8));
    }
    bytes.push_back(0);
    bytes.push_back(0);
    return offset;
}

std::wstring read_utf16_string(const std::vector<u8>& bytes, u32 offset) {
    std::wstring value;
    if (offset == 0 || offset >= bytes.size()) return value;
    for (usize cursor = offset; cursor + 1 < bytes.size(); cursor += 2) {
        const u16 code_unit =
            static_cast<u16>(bytes[cursor]) |
            static_cast<u16>(static_cast<u16>(bytes[cursor + 1]) << 8);
        if (code_unit == 0) break;
        value.push_back(static_cast<wchar_t>(code_unit));
    }
    return value;
}

std::vector<u8> encode_failure_actions(const StoredFailureActions& config) {
    // The registry representation uses a five-DWORD header with offsets
    // relative to the beginning of the value, followed by SC_ACTION entries
    // and UTF-16 strings. This keeps direct registry readers and SCM queries
    // observing one persistent configuration database.
    constexpr usize header_size = 5 * sizeof(u32);
    std::vector<u8> bytes(header_size, 0);

    u32 actions_offset = 0;
    if (!config.actions.empty()) {
        actions_offset = static_cast<u32>(bytes.size());
        for (const auto& action : config.actions) {
            append_u32_le(bytes, action.type);
            append_u32_le(bytes, action.delay_ms);
        }
    }
    const u32 reboot_offset =
        append_utf16_string(bytes, config.reboot_message);
    const u32 command_offset = append_utf16_string(bytes, config.command);

    const u32 header[] = {
        config.reset_period_seconds,
        reboot_offset,
        command_offset,
        static_cast<u32>(config.actions.size()),
        actions_offset,
    };
    for (usize index = 0; index < std::size(header); ++index) {
        const usize offset = index * sizeof(u32);
        const u32 value = header[index];
        bytes[offset] = static_cast<u8>(value);
        bytes[offset + 1] = static_cast<u8>(value >> 8);
        bytes[offset + 2] = static_cast<u8>(value >> 16);
        bytes[offset + 3] = static_cast<u8>(value >> 24);
    }
    return bytes;
}

StoredFailureActions decode_failure_actions(const std::vector<u8>& bytes) {
    StoredFailureActions config;
    const auto reset = read_u32_le(bytes, 0);
    const auto reboot_offset = read_u32_le(bytes, 4);
    const auto command_offset = read_u32_le(bytes, 8);
    const auto action_count = read_u32_le(bytes, 12);
    const auto actions_offset = read_u32_le(bytes, 16);
    if (!reset || !reboot_offset || !command_offset ||
        !action_count || !actions_offset) {
        return config;
    }

    config.reset_period_seconds = *reset;
    config.reboot_message = read_utf16_string(bytes, *reboot_offset);
    config.command = read_utf16_string(bytes, *command_offset);
    if (*action_count > 1024 || *actions_offset > bytes.size()) {
        return config;
    }
    for (u32 index = 0; index < *action_count; ++index) {
        const usize offset =
            static_cast<usize>(*actions_offset) +
            static_cast<usize>(index) * 2 * sizeof(u32);
        const auto type = read_u32_le(bytes, offset);
        const auto delay = read_u32_le(bytes, offset + sizeof(u32));
        if (!type || !delay) break;
        config.actions.push_back(ServiceFailureAction{*type, *delay});
    }
    return config;
}

/// Service names are compared case-insensitively by Windows, and end up in
/// both a registry key and a file name here, so reject separators outright
/// rather than letting one escape either namespace.
bool valid_service_name(const std::wstring& name) {
    if (name.empty() || name.size() > 256) return false;
    return name.find(L'\\') == std::wstring::npos &&
           name.find(L'/') == std::wstring::npos;
}

} // namespace

// ---- Database ---------------------------------------------------------------

bool ScmManager::create_service(const ServiceConfig& config, u32& error) {
    if (!valid_service_name(config.name)) {
        error = error_codes::ERROR_INVALID_NAME;
        return false;
    }
    if (config.image_path.empty()) {
        error = error_codes::ERROR_INVALID_PARAMETER;
        return false;
    }
    if (service_exists(config.name)) {
        error = error_codes::ERROR_SERVICE_EXISTS;
        return false;
    }

    auto key = m_registry.open_key(capsule::HKEY_LOCAL_MACHINE,
                                   service_key_path(config.name), true);
    if (key.is_err()) {
        error = error_codes::ERROR_ACCESS_DENIED;
        return false;
    }

    using capsule::RegType;
    using capsule::RegValue;
    const auto handle = key.value();
    (void)m_registry.set_value(handle, L"Type",
                               RegValue{RegType::Dword, config.service_type});
    (void)m_registry.set_value(handle, L"Start",
                               RegValue{RegType::Dword, config.start_type});
    (void)m_registry.set_value(handle, L"ErrorControl",
                               RegValue{RegType::Dword, config.error_control});
    (void)m_registry.set_value(handle, L"ImagePath",
                               RegValue{RegType::ExpandSz, config.image_path});
    (void)m_registry.set_value(
        handle, L"DisplayName",
        RegValue{RegType::Sz, config.display_name.empty() ? config.name
                                                          : config.display_name});
    (void)m_registry.set_value(
        handle, L"ObjectName",
        RegValue{RegType::Sz, config.object_name.empty()
                                  ? std::wstring(L"LocalSystem")
                                  : config.object_name});
    m_registry.close_key(handle);

    VITREO_TRACE("[scm] CreateService '%s' image='%s' type=0x%x start=%u\n",
                 to_narrow(config.name).c_str(),
                 to_narrow(config.image_path).c_str(), config.service_type,
                 config.start_type);
    error = error_codes::ERROR_SUCCESS;
    return true;
}

bool ScmManager::service_exists(const std::wstring& name) {
    if (!valid_service_name(name)) return false;
    auto key = m_registry.open_key(capsule::HKEY_LOCAL_MACHINE,
                                   service_key_path(name), false);
    if (key.is_err()) return false;
    m_registry.close_key(key.value());
    return true;
}

std::optional<ServiceConfig> ScmManager::query_config(const std::wstring& name) {
    if (!valid_service_name(name)) return std::nullopt;
    auto key = m_registry.open_key(capsule::HKEY_LOCAL_MACHINE,
                                   service_key_path(name), false);
    if (key.is_err()) return std::nullopt;
    const auto handle = key.value();

    ServiceConfig config;
    config.name = name;

    auto dword = [this, handle](const wchar_t* value_name, u32 fallback) -> u32 {
        auto v = m_registry.query_value(handle, value_name);
        if (v.is_err()) return fallback;
        if (const auto* d = std::get_if<u32>(&v.value().data)) return *d;
        return fallback;
    };
    auto text = [this, handle](const wchar_t* value_name) -> std::wstring {
        auto v = m_registry.query_value(handle, value_name);
        if (v.is_err()) return {};
        if (const auto* s = std::get_if<std::wstring>(&v.value().data)) return *s;
        return {};
    };

    config.service_type  = dword(L"Type", SERVICE_WIN32_OWN_PROCESS);
    config.start_type    = dword(L"Start", SERVICE_DEMAND_START);
    config.error_control = dword(L"ErrorControl", 0);
    config.image_path    = text(L"ImagePath");
    config.display_name  = text(L"DisplayName");
    config.object_name   = text(L"ObjectName");

    m_registry.close_key(handle);
    return config;
}

std::optional<ScmManager::ServiceConfig2> ScmManager::query_config2(
    const std::wstring& name) {
    if (!valid_service_name(name)) return std::nullopt;
    auto key = m_registry.open_key(capsule::HKEY_LOCAL_MACHINE,
                                   service_key_path(name), false);
    if (key.is_err()) return std::nullopt;
    const auto handle = key.value();
    ServiceConfig2 config;
    auto dword = [this, handle](const wchar_t* value_name, u32 fallback) -> u32 {
        auto v = m_registry.query_value(handle, value_name);
        if (v.is_err()) return fallback;
        if (const auto* d = std::get_if<u32>(&v.value().data)) return *d;
        return fallback;
    };
    if (auto v = m_registry.query_value(handle, L"Description"); v.is_ok()) {
        if (const auto* s = std::get_if<std::wstring>(&v.value().data)) config.description = *s;
    }
    if (auto v = m_registry.query_value(handle, L"FailureActions"); v.is_ok()) {
        if (const auto* b = std::get_if<std::vector<u8>>(&v.value().data)) {
            const StoredFailureActions stored = decode_failure_actions(*b);
            config.reset_period = stored.reset_period_seconds;
            config.reboot_message = stored.reboot_message;
            config.command = stored.command;
            for (const auto& a : stored.actions) config.actions.push_back({a.type, a.delay_ms});
        }
    }
    config.delayed_auto_start = dword(L"DelayedAutostart", 0);
    config.failure_actions_flag = dword(L"FailureActionsOnNonCrashFailures", 0);
    config.service_sid_type = dword(L"ServiceSidType", 0);
    // PreshutdownTimeout: 10000 ms when the service does not set one
    // (service_config2_probe level 7 on Windows 10).
    config.preshutdown_timeout = dword(L"PreshutdownTimeout", 10000);
    config.launch_protected = dword(L"LaunchProtected", 0);
    if (auto v = m_registry.query_value(handle, L"RequiredPrivileges"); v.is_ok()) {
        if (const auto* list = std::get_if<std::vector<std::wstring>>(&v.value().data)) {
            config.required_privileges = *list;
        }
    }
    m_registry.close_key(handle);
    return config;
}

std::vector<ServiceConfig> ScmManager::enumerate_services() {
    std::vector<ServiceConfig> services;
    auto root = m_registry.open_key(
        capsule::HKEY_LOCAL_MACHINE,
        L"SYSTEM\\CurrentControlSet\\Services", false);
    if (root.is_err()) return services;
    auto names = m_registry.enum_keys(root.value());
    m_registry.close_key(root.value());
    if (names.is_err()) return services;

    std::sort(names.value().begin(), names.value().end(),
              [](const std::wstring& left, const std::wstring& right) {
                  return to_lower(left) < to_lower(right);
              });
    for (const auto& name : names.value()) {
        if (auto config = query_config(name)) {
            services.push_back(std::move(*config));
        }
    }
    return services;
}

bool ScmManager::change_config(const ServiceConfig& config, u32& error) {
    auto existing = query_config(config.name);
    if (!existing) {
        error = error_codes::ERROR_SERVICE_DOES_NOT_EXIST;
        return false;
    }

    auto key = m_registry.open_key(capsule::HKEY_LOCAL_MACHINE,
                                   service_key_path(config.name), false);
    if (key.is_err()) {
        error = error_codes::ERROR_SERVICE_DOES_NOT_EXIST;
        return false;
    }
    const auto handle = key.value();

    using capsule::RegType;
    using capsule::RegValue;
    if (config.service_type != SERVICE_NO_CHANGE) {
        (void)m_registry.set_value(handle, L"Type",
                                   RegValue{RegType::Dword, config.service_type});
    }
    if (config.start_type != SERVICE_NO_CHANGE) {
        (void)m_registry.set_value(handle, L"Start",
                                   RegValue{RegType::Dword, config.start_type});
    }
    if (config.error_control != SERVICE_NO_CHANGE) {
        (void)m_registry.set_value(
            handle, L"ErrorControl",
            RegValue{RegType::Dword, config.error_control});
    }
    if (!config.image_path.empty()) {
        (void)m_registry.set_value(handle, L"ImagePath",
                                   RegValue{RegType::ExpandSz, config.image_path});
    }
    if (!config.display_name.empty()) {
        (void)m_registry.set_value(handle, L"DisplayName",
                                   RegValue{RegType::Sz, config.display_name});
    }
    if (!config.object_name.empty()) {
        (void)m_registry.set_value(handle, L"ObjectName",
                                   RegValue{RegType::Sz, config.object_name});
    }
    m_registry.close_key(handle);

    VITREO_TRACE("[scm] ChangeServiceConfig '%s'\n",
                 to_narrow(config.name).c_str());
    error = error_codes::ERROR_SUCCESS;
    return true;
}

bool ScmManager::change_description(
    const std::wstring& name,
    const std::optional<std::wstring>& description,
    u32& error) {
    auto key = m_registry.open_key(capsule::HKEY_LOCAL_MACHINE,
                                   service_key_path(name), false);
    if (key.is_err()) {
        error = error_codes::ERROR_SERVICE_DOES_NOT_EXIST;
        return false;
    }

    if (description.has_value()) {
        if (description->empty()) {
            (void)m_registry.delete_value(key.value(), L"Description");
        } else {
            const capsule::RegValue value{
                capsule::RegType::Sz, *description};
            if (m_registry.set_value(
                    key.value(), L"Description", value).is_err()) {
                m_registry.close_key(key.value());
                error = error_codes::ERROR_ACCESS_DENIED;
                return false;
            }
        }
    }
    m_registry.close_key(key.value());
    VITREO_TRACE("[scm] ChangeServiceConfig2 description '%s' chars=%zu\n",
                 to_narrow(name).c_str(),
                 description ? description->size() : 0);
    error = error_codes::ERROR_SUCCESS;
    return true;
}

bool ScmManager::change_failure_actions(
    const std::wstring& name,
    const ServiceFailureActionsUpdate& update,
    u32& error) {
    auto key = m_registry.open_key(capsule::HKEY_LOCAL_MACHINE,
                                   service_key_path(name), false);
    if (key.is_err()) {
        error = error_codes::ERROR_SERVICE_DOES_NOT_EXIST;
        return false;
    }

    StoredFailureActions stored;
    auto previous = m_registry.query_value(key.value(), L"FailureActions");
    if (previous.is_ok() &&
        previous.value().type == capsule::RegType::Binary) {
        stored = decode_failure_actions(
            std::get<std::vector<u8>>(previous.value().data));
    }

    if (update.reboot_message) {
        stored.reboot_message = *update.reboot_message;
    }
    if (update.command) {
        stored.command = *update.command;
    }
    if (update.actions) {
        stored.actions = *update.actions;
        stored.reset_period_seconds =
            stored.actions.empty() ? 0 : update.reset_period_seconds;
    }

    const capsule::RegValue value{
        capsule::RegType::Binary, encode_failure_actions(stored)};
    if (m_registry.set_value(
            key.value(), L"FailureActions", value).is_err()) {
        m_registry.close_key(key.value());
        error = error_codes::ERROR_ACCESS_DENIED;
        return false;
    }
    m_registry.close_key(key.value());
    VITREO_TRACE(
        "[scm] ChangeServiceConfig2 failure-actions '%s' reset=%u "
        "actions=%zu\n",
        to_narrow(name).c_str(), stored.reset_period_seconds,
        stored.actions.size());
    error = error_codes::ERROR_SUCCESS;
    return true;
}

std::optional<std::vector<u8>> ScmManager::query_security_descriptor(
    const std::wstring& name) {
    auto key = m_registry.open_key(capsule::HKEY_LOCAL_MACHINE,
                                   service_key_path(name), false);
    if (key.is_err()) return std::nullopt;

    auto stored = m_registry.query_value(key.value(), L"Security");
    m_registry.close_key(key.value());
    if (stored.is_ok() && stored.value().type == capsule::RegType::Binary) {
        return std::get<std::vector<u8>>(stored.value().data);
    }

    // SECURITY_DESCRIPTOR_RELATIVE with SE_SELF_RELATIVE |
    // SE_DACL_PRESENT and a zero DACL offset: a present NULL DACL grants full
    // access to the capsule's single principal.
    std::vector<u8> descriptor(20, 0);
    descriptor[0] = 1;
    descriptor[2] = 0x04;
    descriptor[3] = 0x80;
    return descriptor;
}

bool ScmManager::set_security_descriptor(
    const std::wstring& name, const std::vector<u8>& descriptor, u32& error) {
    if (descriptor.size() < 20 || descriptor[0] != 1) {
        error = error_codes::ERROR_INVALID_DATA;
        return false;
    }
    auto key = m_registry.open_key(capsule::HKEY_LOCAL_MACHINE,
                                   service_key_path(name), false);
    if (key.is_err()) {
        error = error_codes::ERROR_SERVICE_DOES_NOT_EXIST;
        return false;
    }
    const capsule::RegValue value{capsule::RegType::Binary, descriptor};
    const auto result = m_registry.set_value(key.value(), L"Security", value);
    m_registry.close_key(key.value());
    if (result.is_err()) {
        error = error_codes::ERROR_ACCESS_DENIED;
        return false;
    }
    error = error_codes::ERROR_SUCCESS;
    return true;
}

bool ScmManager::delete_service(const std::wstring& name, u32& error) {
    if (!service_exists(name)) {
        error = error_codes::ERROR_SERVICE_DOES_NOT_EXIST;
        return false;
    }

    auto parent = m_registry.open_key(
        capsule::HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Services",
        false);
    if (parent.is_err()) {
        error = error_codes::ERROR_SERVICE_DOES_NOT_EXIST;
        return false;
    }
    auto deleted = m_registry.delete_key(parent.value(), name);
    m_registry.close_key(parent.value());
    if (deleted.is_err()) {
        error = error_codes::ERROR_ACCESS_DENIED;
        return false;
    }

    remove_status_file(name);
    VITREO_TRACE("[scm] DeleteService '%s'\n", to_narrow(name).c_str());
    error = error_codes::ERROR_SUCCESS;
    return true;
}

} // namespace vitreo::winapi
