// SPDX-License-Identifier: LGPL-2.1-only

/// @file device_notifications.cpp
/// @brief Device, power-setting and suspend/resume notification registrations
///        (RegisterDeviceNotificationW, RegisterPowerSettingNotification,
///        RegisterSuspendResumeNotification and their Unregister counterparts).
///        Split out of the former mixed USER32 stubs file
///        as verbatim code motion.

#include "winapi/user32/user32_ext.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/error.hpp"

#include <array>
#include <atomic>
#include <cstdio>
#include <mutex>
#include <unordered_map>

namespace vitreo::winapi {

// ---------------------------------------------------------------------------
// Device / power / suspend-resume notification registrations.
//
// Contract: Microsoft Learn RegisterDeviceNotificationW /
// RegisterPowerSettingNotification / RegisterSuspendResumeNotification.
// Measured (s1win_misc_probe "-- notifications"): the recipient window is
// not validated; RegisterDeviceNotification ignores dbch_size and unknown
// flags, rejects a NULL filter (ERROR_INVALID_PARAMETER) and a filter type
// other than a device interface or handle (ERROR_INVALID_DATA), and sets the
// last error to 0 on success; unregistering an unknown handle is
// ERROR_INVALID_HANDLE for device notifications and ERROR_INVALID_PARAMETER
// for power and suspend-resume ones; a flags value that is neither
// DEVICE_NOTIFY_WINDOW_HANDLE nor DEVICE_NOTIFY_SERVICE_HANDLE is
// ERROR_INVALID_PARAMETER. The host raises no device-arrival or power
// events for a guest, so registered recipients are never notified.
// ---------------------------------------------------------------------------

namespace {
std::mutex g_device_notify_mutex;
std::unordered_map<u64, u64> g_device_notifications;  // handle -> recipient
u64 g_next_device_notification = 0x00DE0001ull;
} // namespace

void User32ExtManager::impl_RegisterDeviceNotificationW(Marshaler& m) {
    // HDEVNOTIFY RegisterDeviceNotification(HANDLE, LPVOID filter, DWORD flags)
    constexpr u32 DBT_DEVTYP_HANDLE = 6;
    constexpr u32 DBT_DEVTYP_DEVICEINTERFACE = 5;
    const u64 recipient = m.arg(0);
    const GuestAddr filter = m.arg_ptr(1);
    (void)m.arg(2);
    if (filter == 0) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0u);
        return;
    }
    const u32 type = m_memory.read_u32(filter + 4).value_or(0xFFFFFFFFu);
    if (type != DBT_DEVTYP_DEVICEINTERFACE && type != DBT_DEVTYP_HANDLE) {
        set_last_error(error_codes::ERROR_INVALID_DATA);
        m.set_return(0u);
        return;
    }
    u64 handle = 0;
    {
        std::lock_guard lock(g_device_notify_mutex);
        handle = g_next_device_notification++;
        g_device_notifications[handle] = recipient;
    }
    set_last_error(0);
    m.set_return(handle);
}

void User32ExtManager::impl_UnregisterDeviceNotification(Marshaler& m) {
    const u64 handle = m.arg(0);
    std::lock_guard lock(g_device_notify_mutex);
    if (g_device_notifications.erase(handle) == 0) {
        set_last_error(error_codes::ERROR_INVALID_HANDLE);
        m.set_return(0u);
        return;
    }
    set_last_error(0);
    m.set_return(1u);
}

void User32ExtManager::impl_RegisterPowerSettingNotification(Marshaler& m) {
    const u64 recipient = m.arg(0);
    const GuestAddr guid_ptr = m.arg_ptr(1);
    const u32 flags = static_cast<u32>(m.arg(2));
    constexpr u32 DEVICE_NOTIFY_WINDOW_HANDLE = 0;
    constexpr u32 DEVICE_NOTIFY_SERVICE_HANDLE = 1;

    if (flags != DEVICE_NOTIFY_WINDOW_HANDLE && flags != DEVICE_NOTIFY_SERVICE_HANDLE) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return;
    }
    // Service-status recipients need the SCM's HandlerEx callbacks
    // (SERVICE_CONTROL_POWEREVENT); that mode is not implemented.
    if (flags == DEVICE_NOTIFY_SERVICE_HANDLE) {
        static std::atomic<bool> logged{false};
        if (!logged.exchange(true)) {
            std::fprintf(stderr, "[vitreo] UNIMPLEMENTED user32!RegisterPowerSettingNotification: "
                                 "DEVICE_NOTIFY_SERVICE_HANDLE\n");
        }
        set_last_error(error_codes::ERROR_NOT_SUPPORTED);
        m.set_return(0);
        return;
    }
    if (guid_ptr == 0) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return;
    }

    std::array<u8, 16> setting_guid{};
    if (m.memory().read_bytes(
            guid_ptr, setting_guid.data(), setting_guid.size()).is_err()) {
        set_last_error(error_codes::ERROR_NOACCESS);
        m.set_return(0);
        return;
    }

    u64 handle = 0;
    {
        std::lock_guard lock(m_power_setting_mutex);
        handle = m_next_power_setting_handle++;
        m_power_setting_registrations.emplace(
            handle,
            PowerSettingRegistration{recipient, setting_guid, flags});
    }
    VITREO_TRACE(
        "[u32ext] RegisterPowerSettingNotification recipient=0x%llx "
        "flags=%u -> 0x%llx\n",
        static_cast<unsigned long long>(recipient), flags,
        static_cast<unsigned long long>(handle));
    m.set_return(handle);
}

void User32ExtManager::impl_UnregisterPowerSettingNotification(Marshaler& m) {
    const u64 handle = m.arg(0);
    {
        std::lock_guard lock(m_power_setting_mutex);
        if (m_power_setting_registrations.erase(handle) == 0) {
            set_last_error(error_codes::ERROR_INVALID_PARAMETER);
            m.set_return(0);
            return;
        }
    }

    VITREO_TRACE(
        "[u32ext] UnregisterPowerSettingNotification handle=0x%llx -> TRUE\n",
        static_cast<unsigned long long>(handle));
    m.set_return(1);
}

void User32ExtManager::impl_RegisterSuspendResumeNotification(Marshaler& m) {
    const u64 recipient = m.arg(0);
    const u32 flags = static_cast<u32>(m.arg(1));
    constexpr u32 DEVICE_NOTIFY_WINDOW_HANDLE = 0;
    constexpr u32 DEVICE_NOTIFY_SERVICE_HANDLE = 1;

    if (flags != DEVICE_NOTIFY_WINDOW_HANDLE && flags != DEVICE_NOTIFY_SERVICE_HANDLE) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return;
    }
    if (flags == DEVICE_NOTIFY_SERVICE_HANDLE) {
        static std::atomic<bool> logged{false};
        if (!logged.exchange(true)) {
            std::fprintf(stderr, "[vitreo] UNIMPLEMENTED user32!RegisterSuspendResumeNotification: "
                                 "DEVICE_NOTIFY_SERVICE_HANDLE\n");
        }
        set_last_error(error_codes::ERROR_NOT_SUPPORTED);
        m.set_return(0);
        return;
    }

    const u64 handle = m_next_suspend_resume_handle++;
    m_suspend_resume_registrations.emplace(
        handle, SuspendResumeRegistration{recipient, flags});
    VITREO_TRACE("[u32ext] RegisterSuspendResumeNotification recipient=0x%llx flags=%u -> 0x%llx\n",
                 static_cast<unsigned long long>(recipient), flags,
                 static_cast<unsigned long long>(handle));
    m.set_return(handle);
}

void User32ExtManager::impl_UnregisterSuspendResumeNotification(Marshaler& m) {
    const u64 handle = m.arg(0);
    if (m_suspend_resume_registrations.erase(handle) == 0) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return;
    }

    VITREO_TRACE("[u32ext] UnregisterSuspendResumeNotification handle=0x%llx -> TRUE\n",
                 static_cast<unsigned long long>(handle));
    m.set_return(1);
}

} // namespace vitreo::winapi
