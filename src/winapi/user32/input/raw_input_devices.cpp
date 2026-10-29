// SPDX-License-Identifier: LGPL-2.1-only

/// @file raw_input_devices.cpp
/// @brief Input-device enumeration: RegisterPointerDeviceNotifications,
///        GetPointerDevices, GetRawInputDeviceList and GetRawInputDeviceInfoA/W.
///        Split out of the former mixed USER32 stubs file
///        as verbatim code motion.

#include "winapi/user32/user32_ext.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/error.hpp"
#include "windowing/model/hwnd_manager.hpp"
#include "winapi/user32/input/raw_input_internal.hpp"

#include <cstdio>
#include <cstring>
#include <vector>

namespace vitreo::winapi {

void User32ExtManager::impl_RegisterPointerDeviceNotifications(Marshaler& m) {
    const auto hwnd = static_cast<windowing::HWND>(m.arg(0));
    const bool notify_range = m.arg(1) != 0;
    if (!m_hwnd_mgr.is_valid(hwnd)) {
        set_last_error(error_codes::ERROR_INVALID_WINDOW_HANDLE);
        m.set_return(0);
        return;
    }

    m_pointer_notification_window = hwnd;
    m_pointer_notification_in_range = notify_range;
    VITREO_TRACE("[u32ext] RegisterPointerDeviceNotifications hwnd=0x%x notifyRange=%u -> TRUE\n",
                 hwnd, notify_range ? 1u : 0u);
    m.set_return(1);
}

void User32ExtManager::impl_GetPointerDevices(Marshaler& m) {
    const GuestAddr device_count = m.arg_ptr(0);
    (void)m.arg_ptr(1);
    if (device_count == 0 || !m.memory().write_u32(device_count, 0).is_ok()) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return;
    }

    VITREO_TRACE("[u32ext] GetPointerDevices -> TRUE count=0\n");
    m.set_return(1);
}

void User32ExtManager::impl_GetRawInputDeviceList(Marshaler& m) {
    // Two devices, matching the handles written into RAWINPUTHEADER:
    // keyboard 1 (RIM_TYPEKEYBOARD) and mouse 2 (RIM_TYPEMOUSE).
    constexpr u32 kDeviceCount = 2u;
    const GuestAddr list = m.arg_ptr(0);
    const GuestAddr count_ptr = m.arg_ptr(1);
    const u32 entry_size = static_cast<u32>(m.arg(2));
    const u32 expected = m.cpu_is_64bit() ? 16u : 8u;
    // s2msg_input_probe devicelist_bad_size: -1 / ERROR_INVALID_PARAMETER;
    // devicelist_null_count: -1 / ERROR_NOACCESS.
    if (entry_size != expected) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0xFFFFFFFFu);
        return;
    }
    if (count_ptr == 0) {
        set_last_error(error_codes::ERROR_NOACCESS);
        m.set_return(0xFFFFFFFFu);
        return;
    }
    if (list == 0) {
        (void)m.write_u32(count_ptr, kDeviceCount);
        m.set_return(0u);
        return;
    }
    const auto have = m.read_u32(count_ptr);
    if (have.is_err()) {
        set_last_error(error_codes::ERROR_NOACCESS);
        m.set_return(0xFFFFFFFFu);
        return;
    }
    if (have.value() < kDeviceCount) {
        (void)m.write_u32(count_ptr, kDeviceCount);
        set_last_error(error_codes::ERROR_INSUFFICIENT_BUFFER);
        m.set_return(0xFFFFFFFFu);
        return;
    }
    auto write_device = [&](GuestAddr at, u64 handle, u32 type) {
        if (m.cpu_is_64bit()) {
            return m.write_u64(at, handle).is_ok() &&
                   m.write_u32(at + 8, type).is_ok();
        }
        return m.write_u32(at, static_cast<u32>(handle)).is_ok() &&
               m.write_u32(at + 4, type).is_ok();
    };
    if (!write_device(list, 1u, 1u) ||
        !write_device(list + expected, 2u, 0u) ||
        m.write_u32(count_ptr, kDeviceCount).is_err()) {
        set_last_error(error_codes::ERROR_NOACCESS);
        m.set_return(0xFFFFFFFFu);
        return;
    }
    VITREO_TRACE("[u32ext] GetRawInputDeviceList -> %u\n", kDeviceCount);
    if (raw_input_diag()) {
        std::fprintf(stderr, "[rawinput] GetRawInputDeviceList -> %u caller=0x%llx\n",
                     kDeviceCount,
                     static_cast<unsigned long long>(m.guest_return_address()));
    }
    m.set_return(kDeviceCount);
}

namespace {

bool write_raw_device_info(Marshaler& m, bool wide) {
    constexpr u32 kRidiDeviceName = 0x20000007u;
    constexpr u32 kRidiDeviceInfo = 0x2000000bu;
    const u64 device = m.arg(0);
    const u32 command = static_cast<u32>(m.arg(1));
    const GuestAddr data = m.arg_ptr(2);
    const GuestAddr size_ptr = m.arg_ptr(3);
    const bool mouse = device == 2u;
    const bool keyboard = device == 1u;
    if ((!mouse && !keyboard) || size_ptr == 0) {
        set_last_error(error_codes::ERROR_INVALID_HANDLE);
        m.set_return(0xFFFFFFFFu);
        return true;
    }
    if (command == kRidiDeviceName) {
        const char* name = mouse
            ? "\\\\?\\HID#VID_046D&PID_C52B#8&mouse&0&0000#{378de44c-56ef-11d1-bc8c-00a0c91405dd}"
            : "\\\\?\\HID#VID_046D&PID_C52B#8&keyboard&0&0000#{884b96c3-56ef-11d1-bc8c-00a0c91405dd}";
        const u32 chars = static_cast<u32>(std::strlen(name) + 1u);
        const u32 bytes = wide ? chars * 2u : chars;
        if (data == 0) {
            (void)m.write_u32(size_ptr, chars);
            m.set_return(0u);
            return true;
        }
        const auto have = m.read_u32(size_ptr);
        if (have.is_err() || have.value() < chars) {
            (void)m.write_u32(size_ptr, chars);
            set_last_error(have.is_err() ? error_codes::ERROR_NOACCESS
                                         : error_codes::ERROR_INSUFFICIENT_BUFFER);
            m.set_return(0xFFFFFFFFu);
            return true;
        }
        if (wide) {
            std::vector<u16> wide_name(chars);
            for (u32 i = 0; i < chars; ++i) wide_name[i] = static_cast<u8>(name[i]);
            if (m.memory().write_bytes(data, reinterpret_cast<const Byte*>(wide_name.data()),
                                       bytes).is_err()) {
                set_last_error(error_codes::ERROR_NOACCESS);
                m.set_return(0xFFFFFFFFu);
                return true;
            }
        } else if (m.memory().write_bytes(data, reinterpret_cast<const Byte*>(name), bytes).is_err()) {
            set_last_error(error_codes::ERROR_NOACCESS);
            m.set_return(0xFFFFFFFFu);
            return true;
        }
        (void)m.write_u32(size_ptr, chars);
        m.set_return(chars);
        return true;
    }
    if (command != kRidiDeviceInfo) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0xFFFFFFFFu);
        return true;
    }
    constexpr u32 kInfoSize = 32u;
    if (data == 0) {
        (void)m.write_u32(size_ptr, kInfoSize);
        m.set_return(0u);
        return true;
    }
    const auto have = m.read_u32(size_ptr);
    if (have.is_err() || have.value() < kInfoSize) {
        (void)m.write_u32(size_ptr, kInfoSize);
        set_last_error(have.is_err() ? error_codes::ERROR_NOACCESS
                                     : error_codes::ERROR_INSUFFICIENT_BUFFER);
        m.set_return(0xFFFFFFFFu);
        return true;
    }
    Byte info[kInfoSize]{};
    auto put32 = [&](u32 offset, u32 value) {
        std::memcpy(info + offset, &value, 4);
    };
    put32(0, kInfoSize);
    put32(4, mouse ? 0u : 1u);
    if (mouse) {
        put32(8, 1u);
        put32(12, 5u);
        put32(16, 100u);
        put32(20, 1u);
    } else {
        put32(8, 81u);
        put32(12, 0u);
        put32(16, 1u);
        put32(20, 12u);
        put32(24, 3u);
        put32(28, 101u);
    }
    if (m.memory().write_bytes(data, info, kInfoSize).is_err()) {
        set_last_error(error_codes::ERROR_NOACCESS);
        m.set_return(0xFFFFFFFFu);
        return true;
    }
    (void)m.write_u32(size_ptr, kInfoSize);
    VITREO_TRACE("[u32ext] GetRawInputDeviceInfo device=%llu -> %u\n",
                 static_cast<unsigned long long>(device), kInfoSize);
    m.set_return(kInfoSize);
    return true;
}

}  // namespace

void User32ExtManager::impl_GetRawInputDeviceInfoA(Marshaler& m) {
    const u64 device = m.arg(0), command = m.arg(1);
    (void)write_raw_device_info(m, false);
    if (raw_input_diag()) {
        std::fprintf(stderr, "[rawinput] GetRawInputDeviceInfoA dev=0x%llx cmd=0x%llx -> 0x%llx\n",
                     static_cast<unsigned long long>(device),
                     static_cast<unsigned long long>(command),
                     static_cast<unsigned long long>(m.cpu() ? m.cpu()->gpr.rax : 0));
    }
}

void User32ExtManager::impl_GetRawInputDeviceInfoW(Marshaler& m) {
    const u64 device = m.arg(0), command = m.arg(1);
    (void)write_raw_device_info(m, true);
    if (raw_input_diag()) {
        std::fprintf(stderr, "[rawinput] GetRawInputDeviceInfoW dev=0x%llx cmd=0x%llx -> 0x%llx\n",
                     static_cast<unsigned long long>(device),
                     static_cast<unsigned long long>(command),
                     static_cast<unsigned long long>(m.cpu() ? m.cpu()->gpr.rax : 0));
    }
}

} // namespace vitreo::winapi
