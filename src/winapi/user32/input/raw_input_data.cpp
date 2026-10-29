// SPDX-License-Identifier: LGPL-2.1-only

/// @file raw_input_data.cpp
/// @brief GetRawInputData and GetRawInputBuffer: the RAWINPUT packet readers.
///        Split out of the former mixed USER32 stubs file
///        as verbatim code motion.

#include "winapi/user32/user32_ext.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/user32/input/raw_input_internal.hpp"

#include <cstdio>
#include <mutex>

namespace vitreo::winapi {

void User32ExtManager::impl_GetRawInputData(Marshaler& m) {
    constexpr u32 kRidInput = 0x10000003u;
    constexpr u32 kRidHeader = 0x10000005u;
    constexpr u32 kRimTypeKeyboard = 1u;

    const u64 handle = m.arg(0);
    const u32 command = static_cast<u32>(m.arg(1));
    const GuestAddr data = m.arg_ptr(2);
    const GuestAddr size_ptr = m.arg_ptr(3);
    const u32 caller_header_size = static_cast<u32>(m.arg(4));
    // cbSizeHeader is checked before the handle (s2msg_input_probe
    // rawinputdata_bad_header: -1 / ERROR_INVALID_PARAMETER).
    if (caller_header_size != (m.cpu_is_64bit() ? 24u : 16u)) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0xFFFFFFFFu);
        return;
    }

    RawKeyboardPacket packet{};
    {
        std::lock_guard lock(m_raw_input_mutex);
        const auto it = m_raw_keyboard_packets.find(handle);
        if (it == m_raw_keyboard_packets.end()) {
            // The Windows WOW64 wrapper expands its input size before the
            // failing native query (rawinput_lifetime_probe.x86.expected).
            if (!m.cpu_is_64bit() && data != 0 && size_ptr != 0 &&
                (command == kRidInput || command == kRidHeader)) {
                if (const auto size = m.read_u32(size_ptr); size.is_ok()) {
                    (void)m.write_u32(size_ptr, command == kRidHeader
                        ? 24u : size.value() + 8u);
                }
            }
            if (raw_input_diag()) {
                std::fprintf(stderr, "[rawinput] GetRawInputData raw=0x%llx cmd=0x%x -> INVALID_HANDLE\n",
                             static_cast<unsigned long long>(handle), command);
            }
            set_last_error(error_codes::ERROR_INVALID_HANDLE);
            m.set_return(0xFFFFFFFFu);
            return;
        }
        packet = it->second;
    }

    const u32 header_size = packet.is_64bit ? 24u : 16u;
    const u32 body_size = packet.mouse ? 24u : 16u;
    const u32 packet_size = header_size + body_size;
    const u32 required = command == kRidHeader ? header_size :
                         command == kRidInput ? packet_size : 0u;
    if (required == 0 || caller_header_size != header_size || size_ptr == 0) {
        if (raw_input_diag()) {
            std::fprintf(stderr, "[rawinput] GetRawInputData cmd=0x%x cbHeader=%u expected=%u -> INVALID_PARAMETER\n",
                         command, caller_header_size, header_size);
        }
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0xFFFFFFFFu);
        return;
    }
    const auto available = m.read_u32(size_ptr);
    if (available.is_err()) {
        set_last_error(error_codes::ERROR_NOACCESS);
        m.set_return(0xFFFFFFFFu);
        return;
    }
    if (data == 0) {
        (void)m.write_u32(size_ptr, required);
        m.set_return(0u);
        return;
    }
    if (available.value() < required) {
        (void)m.write_u32(size_ptr, required);
        set_last_error(error_codes::ERROR_INSUFFICIENT_BUFFER);
        m.set_return(0xFFFFFFFFu);
        return;
    }

    bool ok = m.write_u32(data, packet.mouse ? 0u : kRimTypeKeyboard).is_ok() &&
              m.write_u32(data + 4, packet_size).is_ok();
    const u64 device = packet.mouse ? 2u : 1u;
    if (packet.is_64bit) {
        ok = ok && m.write_u64(data + 8, device).is_ok() &&
             m.write_u64(data + 16, 0u).is_ok();
    } else {
        ok = ok && m.write_u32(data + 8, static_cast<u32>(device)).is_ok() &&
             m.write_u32(data + 12, 0u).is_ok();
    }
    if (command == kRidInput) {
        const GuestAddr body = data + header_size;
        if (packet.mouse) {
            ok = ok && m.write_u16(body, 0u).is_ok() &&
                 m.write_u16(body + 2, 0u).is_ok() &&
                 m.write_u16(body + 4, packet.button_flags).is_ok() &&
                 m.write_u16(body + 6, packet.button_data).is_ok() &&
                 m.write_u32(body + 8, 0u).is_ok() &&
                 m.write_u32(body + 12, static_cast<u32>(packet.last_x)).is_ok() &&
                 m.write_u32(body + 16, static_cast<u32>(packet.last_y)).is_ok() &&
                 m.write_u32(body + 20, 0u).is_ok();
        } else {
            ok = ok && m.write_u16(body, packet.make_code).is_ok() &&
                 m.write_u16(body + 2, packet.flags).is_ok() &&
                 m.write_u16(body + 4, 0u).is_ok() &&
                 m.write_u16(body + 6, packet.virtual_key).is_ok() &&
                 m.write_u32(body + 8, packet.message).is_ok() &&
                 m.write_u32(body + 12, 0u).is_ok();
        }
    }
    // Successful full reads preserve the supplied size. WOW64's header-only
    // path writes its converted header size; native x64 preserves it too.
    if (!ok || (!packet.is_64bit && command == kRidHeader &&
                m.write_u32(size_ptr, header_size).is_err())) {
        set_last_error(error_codes::ERROR_NOACCESS);
        m.set_return(0xFFFFFFFFu);
        return;
    }
    if (raw_input_diag()) {
        std::fprintf(stderr, "[rawinput] GetRawInputData raw=0x%llx cmd=0x%x data=%d -> %u\n",
                     static_cast<unsigned long long>(handle), command,
                     data != 0 ? 1 : 0, required);
    }
    VITREO_TRACE("[u32ext] GetRawInputData raw=0x%llx command=0x%x -> %u\n",
                 static_cast<unsigned long long>(handle), command, required);
    m.set_return(required);
}

void User32ExtManager::impl_GetRawInputBuffer(Marshaler& m) {
    const GuestAddr data = m.arg_ptr(0);
    const GuestAddr size_ptr = m.arg_ptr(1);
    const u32 caller_header_size = static_cast<u32>(m.arg(2));
    // rawinputbuffer_bad_header: -1 / ERROR_INVALID_PARAMETER even with
    // nothing buffered.
    if (size_ptr == 0 || caller_header_size != (m.cpu_is_64bit() ? 24u : 16u)) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0xFFFFFFFFu);
        return;
    }

    std::lock_guard lock(m_raw_input_mutex);
    while (!m_raw_keyboard_order.empty() &&
           !m_raw_keyboard_packets.contains(m_raw_keyboard_order.front())) {
        m_raw_keyboard_order.pop_front();
    }
    if (m_raw_keyboard_order.empty()) {
        if (data == 0) (void)m.write_u32(size_ptr, 0u);
        m.set_return(0u);
        return;
    }

    const RawKeyboardPacket& first =
        m_raw_keyboard_packets.at(m_raw_keyboard_order.front());
    const u32 header_size = first.is_64bit ? 24u : 16u;
    const u32 packet_size = header_size + (first.mouse ? 24u : 16u);
    if (caller_header_size != header_size) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0xFFFFFFFFu);
        return;
    }
    if (data == 0) {
        (void)m.write_u32(size_ptr, packet_size);
        m.set_return(0u);
        return;
    }

    const auto available_result = m.read_u32(size_ptr);
    if (available_result.is_err()) {
        set_last_error(error_codes::ERROR_NOACCESS);
        m.set_return(0xFFFFFFFFu);
        return;
    }
    const u32 capacity = available_result.value();
    u32 bytes_used = 0;
    u32 count = 0;
    while (!m_raw_keyboard_order.empty()) {
        const u64 handle = m_raw_keyboard_order.front();
        const auto it = m_raw_keyboard_packets.find(handle);
        if (it == m_raw_keyboard_packets.end()) {
            m_raw_keyboard_order.pop_front();
            continue;
        }
        const RawKeyboardPacket& packet = it->second;
        const u32 current_header_size = packet.is_64bit ? 24u : 16u;
        const u32 current_packet_size =
            current_header_size + (packet.mouse ? 24u : 16u);
        if (current_header_size != header_size ||
            bytes_used + current_packet_size > capacity) {
            break;
        }
        const GuestAddr output = data + bytes_used;
        const u64 device = packet.mouse ? 2u : 1u;
        bool ok = m.write_u32(output, packet.mouse ? 0u : 1u).is_ok() &&
                  m.write_u32(output + 4, current_packet_size).is_ok();
        if (packet.is_64bit) {
            ok = ok && m.write_u64(output + 8, device).is_ok() &&
                 m.write_u64(output + 16, 0u).is_ok();
        } else {
            ok = ok && m.write_u32(output + 8, static_cast<u32>(device)).is_ok() &&
                 m.write_u32(output + 12, 0u).is_ok();
        }
        const GuestAddr body = output + current_header_size;
        if (packet.mouse) {
            ok = ok && m.write_u16(body, 0u).is_ok() &&
                 m.write_u16(body + 2, 0u).is_ok() &&
                 m.write_u16(body + 4, packet.button_flags).is_ok() &&
                 m.write_u16(body + 6, packet.button_data).is_ok() &&
                 m.write_u32(body + 8, 0u).is_ok() &&
                 m.write_u32(body + 12, static_cast<u32>(packet.last_x)).is_ok() &&
                 m.write_u32(body + 16, static_cast<u32>(packet.last_y)).is_ok() &&
                 m.write_u32(body + 20, 0u).is_ok();
        } else {
            ok = ok && m.write_u16(body, packet.make_code).is_ok() &&
                 m.write_u16(body + 2, packet.flags).is_ok() &&
                 m.write_u16(body + 4, 0u).is_ok() &&
                 m.write_u16(body + 6, packet.virtual_key).is_ok() &&
                 m.write_u32(body + 8, packet.message).is_ok() &&
                 m.write_u32(body + 12, 0u).is_ok();
        }
        if (!ok) {
            set_last_error(error_codes::ERROR_NOACCESS);
            m.set_return(0xFFFFFFFFu);
            return;
        }
        const auto& registration = packet.mouse
            ? m_raw_mouse_registration : m_raw_keyboard_registration;
        const windowing::HWND target =
            registration.has_value()
                ? (registration->target != windowing::NULL_HWND
                    ? registration->target : m_active_window)
                : windowing::NULL_HWND;
        (void)m_queue.remove_exact(target, windowing::WM::INPUT, handle);
        m_raw_keyboard_packets.erase(it);
        m_raw_keyboard_order.pop_front();
        bytes_used += current_packet_size;
        ++count;
    }
    if (count == 0) {
        (void)m.write_u32(size_ptr, packet_size);
        set_last_error(error_codes::ERROR_INSUFFICIENT_BUFFER);
        m.set_return(0xFFFFFFFFu);
        return;
    }
    if (raw_input_diag()) {
        std::fprintf(stderr,
                     "[rawinput] GetRawInputBuffer -> %u packet(s) caller=0x%llx\n",
                     count,
                     static_cast<unsigned long long>(m.guest_return_address()));
    }
    VITREO_TRACE("[u32ext] GetRawInputBuffer -> %u packet(s), %u bytes\n",
                 count, bytes_used);
    m.set_return(count);
}

} // namespace vitreo::winapi
