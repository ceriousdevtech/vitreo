// SPDX-License-Identifier: LGPL-2.1-only

/// @file register_window_message.cpp
/// @brief RegisterWindowMessageA/W and the user atom table lookup they share with
///        RegisterClipboardFormat. Split out of the former mixed USER32 stubs file
///        as verbatim code motion.

#include "winapi/user32/user32_ext.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/error.hpp"

#include <mutex>
#include <string>
#include <utility>

namespace vitreo::winapi {

u32 User32ExtManager::register_window_message(const std::string& name) {
    // RegisterWindowMessage and RegisterClipboardFormat share the user atom
    // table: one case-insensitive name -> one value in 0xC000..0xFFFF for
    // both (s2msg_queue_probe regmsg same_ci=1 same_as_clipfmt=1). An empty
    // name fails with the last error untouched (regmsg_empty); a name over
    // 255 UTF-16 units fails with ERROR_MORE_DATA (regmsg_299_chars).
    if (name.empty()) return 0;
    usize units = 0;
    for (usize i = 0; i < name.size(); ++i) {
        const auto c = static_cast<unsigned char>(name[i]);
        if ((c & 0xC0u) != 0x80u) units += (c >= 0xF0u) ? 2u : 1u;
    }
    if (units > 255) {
        set_last_error(error_codes::ERROR_MORE_DATA);
        return 0;
    }
    // Atom lookups fold case (ASCII here; the name is kept as registered for
    // GetClipboardFormatName).
    std::string key = name;
    for (char& ch : key) {
        if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch + 32);
    }
    std::lock_guard lock(m_registered_message_mutex);
    const auto existing = m_clip_format_ids.find(key);
    if (existing != m_clip_format_ids.end()) return existing->second;
    if (m_next_clip_format > 0xFFFFu) return 0;
    const u32 id = m_next_clip_format++;
    m_clip_format_ids.emplace(std::move(key), id);
    m_clip_format_names[id] = name;
    return id;
}

void User32ExtManager::impl_RegisterWindowMessageW(Marshaler& m) {
    const GuestAddr name_ptr = m.arg_ptr(0);
    if (name_ptr == 0) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return;
    }
    const auto name = m.read_wide_string(name_ptr);
    if (name.is_err()) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return;
    }
    const u32 id = register_window_message(name.value());
    VITREO_TRACE(
        "[user32_ext] RegisterWindowMessageW('%s') -> 0x%x\n",
        name.value().c_str(), id);
    m.set_return(id);
}

void User32ExtManager::impl_RegisterWindowMessageA(Marshaler& m) {
    const GuestAddr name_ptr = m.arg_ptr(0);
    if (name_ptr == 0) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return;
    }
    const auto name = m.read_ansi_string(name_ptr);
    if (name.is_err()) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return;
    }
    const u32 id = register_window_message(name.value());
    VITREO_TRACE(
        "[user32_ext] RegisterWindowMessageA('%s') -> 0x%x\n",
        name.value().c_str(), id);
    m.set_return(id);
}

} // namespace vitreo::winapi
