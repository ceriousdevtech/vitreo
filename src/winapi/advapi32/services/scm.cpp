// SPDX-License-Identifier: LGPL-2.1-only
/// @file scm.cpp
/// @brief advapi32 Service Control Manager implementation. See scm.hpp.

#include "winapi/advapi32/services/scm_internal.hpp"

#include <cstdlib>
#include <string>

namespace vitreo::winapi {

using namespace scm_detail;
using namespace service_constants;

// =============================================================================
// ScmManager
// =============================================================================

ScmManager::ScmManager(capsule::Registry& registry, capsule::Vfs& vfs)
    : m_registry(registry), m_vfs(vfs) {
    // A service process is told what it is through the environment, before any
    // guest code runs. This is what lets StartServiceCtrlDispatcher honestly
    // reject a service binary that was launched directly.
    if (const char* name = std::getenv("VITREO_SERVICE_NAME");
        name != nullptr && name[0] != '\0') {
        m_own_service_name = to_wide(std::string(name));
        VITREO_TRACE("[scm] process launched as service '%s'\n", name);
    }
}

bool ScmManager::set_own_service_arguments_utf16(
    const std::vector<std::string>& encoded_arguments) {
    std::vector<std::wstring> decoded_arguments;
    decoded_arguments.reserve(encoded_arguments.size());
    for (const auto& encoded : encoded_arguments) {
        auto decoded = decode_utf16_hex(encoded);
        if (!decoded.has_value()) return false;
        decoded_arguments.push_back(std::move(*decoded));
    }
    m_own_service_arguments = std::move(decoded_arguments);
    return true;
}

void ScmManager::set_capsule(std::string capsule_root, std::string capsule_name) {
    m_capsule_root = std::move(capsule_root);
    if (!capsule_name.empty()) m_capsule_name = std::move(capsule_name);
}

bool ScmManager::is_builtin_native_service(const std::wstring& name) {
    return to_lower(name) == L"seclogon";
}

void ScmManager::install_builtin_services() {
    // Secondary Logon is the Windows service that supports "run as another
    // user" and UAC restart flows. Vitreo provides the observable operation
    // through the platform elevation broker, so no guest svchost process is
    // needed, but Windows applications must still see the real SCM entry.
    if (!service_exists(L"seclogon")) {
        ServiceConfig config;
        config.name = L"seclogon";
        config.display_name = L"Secondary Logon";
        config.image_path =
            L"%SystemRoot%\\System32\\svchost.exe -k netsvcs -p";
        config.object_name = L"LocalSystem";
        config.service_type = SERVICE_WIN32_SHARE_PROCESS;
        config.start_type = SERVICE_DEMAND_START;
        config.error_control = 1; // SERVICE_ERROR_NORMAL

        u32 error = error_codes::ERROR_SUCCESS;
        if (!create_service(config, error)) {
            VITREO_TRACE(
                "[scm] failed to install built-in seclogon service error=%u\n",
                error);
        }
    }
}

std::wstring ScmManager::service_key_path(const std::wstring& name) {
    return L"SYSTEM\\CurrentControlSet\\Services\\" + name;
}

std::string ScmManager::status_file_path(const std::wstring& name) const {
    if (m_capsule_root.empty()) return {};
    return m_capsule_root + "/runtime/services/" + to_narrow(to_lower(name)) +
           ".status";
}

// ---- Handles ----------------------------------------------------------------

u32 ScmManager::open_manager_handle() {
    std::lock_guard<std::mutex> lock(m_mutex);
    const u32 handle = m_next_handle++;
    m_handles.emplace(handle, std::wstring{});
    return handle;
}

u32 ScmManager::open_service_handle(const std::wstring& name) {
    std::lock_guard<std::mutex> lock(m_mutex);
    const u32 handle = m_next_handle++;
    m_handles.emplace(handle, name);
    return handle;
}

std::optional<std::wstring> ScmManager::handle_service_name(u32 handle) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_handles.find(handle);
    if (it == m_handles.end() || it->second.empty()) return std::nullopt;
    return it->second;
}

bool ScmManager::is_manager_handle(u32 handle) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_handles.find(handle);
    return it != m_handles.end() && it->second.empty();
}

bool ScmManager::close_handle(u32 handle) {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_handles.erase(handle) != 0;
}

// =============================================================================
// Thunks
// =============================================================================

DllExports create_advapi32_scm_thunks(ScmManager& scm) {
    DllExports exports;

    register_scm_open_start_exports(scm, exports);
    register_scm_status_control_exports(scm, exports);
    register_scm_config_exports(scm, exports);
    register_scm_service_side_exports(scm, exports);

    return exports;
}

} // namespace vitreo::winapi
