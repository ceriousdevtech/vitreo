/// @file module.cpp
/// @brief Implementation of module management APIs (WIN32-033)

#include "module.hpp"
#include "module_internal.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/kernel32/locale/string_conv.hpp"
#include "winapi/core/marshaler.hpp"
#include "ipc/ipc_gate.hpp"

#include "core/pe/parser/pe_parser.hpp"
#include "core/pe/imports/api_set_resolver.hpp"
#include "capsule/vfs/filesystem.hpp"
#include <vitreo/result.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <unistd.h>
#include <cstring>
#include <filesystem>
#include <cstdio>
#include <string>
#include <vector>
#include <functional>
#include <unordered_set>

#include <vitreo/diagnostics.hpp>

/// @file notify_dll.cpp
/// @brief ModuleManager::notify_dll
///
/// Closed method cut from module.cpp.

namespace vitreo::winapi {

void ModuleManager::notify_dll(
    u32 reason, std::string_view full_name, std::string_view base_name,
    GuestAddr image_base, usize image_size) {
    if (!m_memory || !m_guest_call) return;
    std::vector<DllNotificationRegistration> registrations;
    {
        std::lock_guard<std::mutex> lock(m_dll_notification_mutex);
        registrations = m_dll_notifications;
    }
    if (registrations.empty()) return;

    VITREO_TRACE(
        "[loader] DLL notification reason=%s module=\"%.*s\" "
        "base=0x%llx size=0x%zx callbacks=%zu\n",
        reason == 1 ? "loaded" : "unloaded",
        static_cast<int>(full_name.size()), full_name.data(),
        static_cast<unsigned long long>(image_base), image_size,
        registrations.size());

    const auto full_wide = utf8_to_utf16(full_name);
    const auto base_wide = utf8_to_utf16(base_name);
    const bool is_64bit = m_is_64bit_process;
    const usize pointer_size = is_64bit ? sizeof(u64) : sizeof(u32);
    const usize data_size = is_64bit ? 40 : 20;
    const usize unicode_size = is_64bit ? 16 : 8;
    const usize needed = data_size + unicode_size * 2 +
        (full_wide.size() + base_wide.size() + 2) * sizeof(u16);
    const usize mapped_size = (needed + 0xFFFu) & ~usize{0xFFFu};
    const GuestAddr scratch =
        m_memory->find_free_gap(0x68000000ull, mapped_size);
    if (scratch == 0 || m_memory->map_region(
            scratch, mapped_size, MemoryProtection::ReadWrite).is_err()) {
        VITREO_TRACE("[loader] DLL notification scratch allocation failed\n");
        return;
    }

    const GuestAddr full_us = scratch + data_size;
    const GuestAddr base_us = full_us + unicode_size;
    const GuestAddr full_buffer = base_us + unicode_size;
    const GuestAddr base_buffer = full_buffer +
        (full_wide.size() + 1) * sizeof(u16);
    const auto write_pointer = [&](GuestAddr address, u64 value) {
        return is_64bit ? m_memory->write_u64(address, value)
                        : m_memory->write_u32(address, static_cast<u32>(value));
    };
    const auto write_unicode = [&](GuestAddr descriptor, GuestAddr buffer,
                                   const std::vector<u16>& value) {
        (void)m_memory->write_u16(
            descriptor, static_cast<u16>(value.size() * sizeof(u16)));
        (void)m_memory->write_u16(
            descriptor + 2,
            static_cast<u16>((value.size() + 1) * sizeof(u16)));
        (void)write_pointer(descriptor + (is_64bit ? 8 : 4), buffer);
        for (usize index = 0; index < value.size(); ++index) {
            (void)m_memory->write_u16(buffer + index * sizeof(u16), value[index]);
        }
        (void)m_memory->write_u16(buffer + value.size() * sizeof(u16), 0);
    };

    (void)m_memory->write_u32(scratch, 0);
    const GuestAddr full_ptr_offset = is_64bit ? 8 : 4;
    (void)write_pointer(scratch + full_ptr_offset, full_us);
    (void)write_pointer(scratch + full_ptr_offset + pointer_size, base_us);
    (void)write_pointer(scratch + full_ptr_offset + pointer_size * 2, image_base);
    (void)m_memory->write_u32(
        scratch + full_ptr_offset + pointer_size * 3,
        static_cast<u32>(image_size));
    write_unicode(full_us, full_buffer, full_wide);
    write_unicode(base_us, base_buffer, base_wide);

    for (const auto& registration : registrations) {
        {
            std::lock_guard<std::mutex> lock(m_dll_notification_mutex);
            const bool still_registered = std::any_of(
                m_dll_notifications.begin(), m_dll_notifications.end(),
                [&registration](const DllNotificationRegistration& current) {
                    return current.cookie == registration.cookie;
                });
            if (!still_registered) continue;
        }
        const u64 args[3] = {reason, scratch, registration.context};
        (void)m_guest_call(registration.callback, args, 3, is_64bit);
    }
    (void)m_memory->unmap_region(scratch, mapped_size);
}

} // namespace vitreo::winapi
