// SPDX-License-Identifier: LGPL-2.1-only
#include "winapi/graphics/dwrite/dwrite_internal.hpp"

#include "capsule/vfs/vfs.hpp"
#include "core/memory/guest_memory.hpp"
#include "winapi/graphics/dwrite/font_backend.hpp"
#include "winapi/graphics/gdi32/gdi32.hpp"
#include "winapi/kernel32/locale/string_conv.hpp"
#include "winapi/core/marshaler.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <sys/stat.h>
#include <iomanip>
#include <iterator>
#include <limits>
#include <sstream>
#include <string_view>
#include <unistd.h>
#include <utility>

namespace vitreo::winapi {
void DWriteManager::impl_StringsGetCount(Marshaler& m) {
    const auto strings = m_objects.find(m.arg_ptr(0));
    m.set_return(strings != m_objects.end() &&
                         strings->second.type == ObjectType::LocalizedStrings
                     ? 1u
                     : 0u);
}

void DWriteManager::impl_StringsFindLocaleName(Marshaler& m) {
    const auto strings = m_objects.find(m.arg_ptr(0));
    const GuestAddr locale_address = m.arg_ptr(1);
    const GuestAddr index_out = m.arg_ptr(2);
    const GuestAddr exists_out = m.arg_ptr(3);
    if (index_out == 0 || exists_out == 0) {
        m.set_return(E_POINTER);
        return;
    }
    if (m.write_u32(index_out, std::numeric_limits<u32>::max()).is_err() ||
        m.write_u32(exists_out, 0).is_err()) {
        m.set_return(E_POINTER);
        return;
    }
    if (strings == m_objects.end() ||
        strings->second.type != ObjectType::LocalizedStrings ||
        locale_address == 0) {
        m.set_return(E_INVALIDARG);
        return;
    }
    auto locale = read_utf16_z(locale_address, 85);
    if (locale.is_err()) {
        m.set_return(E_INVALIDARG);
        return;
    }
    constexpr std::u16string_view EN_US = u"en-us";
    const auto fold = [](char16_t ch) {
        return ch >= u'A' && ch <= u'Z'
            ? static_cast<char16_t>(ch + (u'a' - u'A'))
            : ch;
    };
    const bool matches = locale.value().size() == EN_US.size() &&
        std::equal(locale.value().begin(), locale.value().end(), EN_US.begin(),
                   [&](char16_t left, char16_t right) {
                       return fold(left) == fold(right);
                   });
    if (matches &&
        (m.write_u32(index_out, 0).is_err() ||
         m.write_u32(exists_out, 1).is_err())) {
        m.set_return(E_POINTER);
        return;
    }
    m.set_return(S_OK);
}

void DWriteManager::impl_StringsGetLocaleNameLength(Marshaler& m) {
    const auto strings = m_objects.find(m.arg_ptr(0));
    const u32 index = static_cast<u32>(m.arg(1));
    const GuestAddr length_out = m.arg_ptr(2);
    if (length_out == 0) {
        m.set_return(E_POINTER);
        return;
    }
    if (strings == m_objects.end() ||
        strings->second.type != ObjectType::LocalizedStrings || index != 0) {
        m.set_return(E_INVALIDARG);
        return;
    }
    m.set_return(m.write_u32(length_out, 5).is_ok() ? S_OK : E_POINTER);
}

void DWriteManager::impl_StringsGetLocaleName(Marshaler& m) {
    const auto strings = m_objects.find(m.arg_ptr(0));
    const u32 index = static_cast<u32>(m.arg(1));
    const GuestAddr buffer = m.arg_ptr(2);
    const u32 size = static_cast<u32>(m.arg(3));
    constexpr std::u16string_view EN_US = u"en-us";
    if (strings == m_objects.end() ||
        strings->second.type != ObjectType::LocalizedStrings || index != 0) {
        m.set_return(E_INVALIDARG);
        return;
    }
    if (buffer == 0) {
        m.set_return(E_POINTER);
        return;
    }
    if (size < EN_US.size() + 1) {
        m.set_return(E_NOT_SUFFICIENT_BUFFER);
        return;
    }
    for (usize i = 0; i < EN_US.size(); ++i) {
        if (m.write_u16(buffer + i * sizeof(u16), EN_US[i]).is_err()) {
            m.set_return(E_POINTER);
            return;
        }
    }
    m.set_return(
        m.write_u16(buffer + EN_US.size() * sizeof(u16), 0).is_ok()
            ? S_OK
            : E_POINTER);
}

void DWriteManager::impl_StringsGetStringLength(Marshaler& m) {
    const auto strings = m_objects.find(m.arg_ptr(0));
    const u32 index = static_cast<u32>(m.arg(1));
    const GuestAddr length_out = m.arg_ptr(2);
    if (length_out == 0) {
        m.set_return(E_POINTER);
        return;
    }
    if (strings == m_objects.end() ||
        strings->second.type != ObjectType::LocalizedStrings || index != 0 ||
        strings->second.localized_value.empty()) {
        m.set_return(E_INVALIDARG);
        return;
    }
    const usize length = strings->second.localized_value.size();
    if (length > std::numeric_limits<u32>::max()) {
        m.set_return(E_OUTOFMEMORY);
        return;
    }
    m.set_return(m.write_u32(length_out, static_cast<u32>(length)).is_ok()
                     ? S_OK
                     : E_POINTER);
}

void DWriteManager::impl_StringsGetString(Marshaler& m) {
    const auto strings = m_objects.find(m.arg_ptr(0));
    const u32 index = static_cast<u32>(m.arg(1));
    const GuestAddr buffer = m.arg_ptr(2);
    const u32 size = static_cast<u32>(m.arg(3));
    if (strings == m_objects.end() ||
        strings->second.type != ObjectType::LocalizedStrings || index != 0 ||
        strings->second.localized_value.empty()) {
        m.set_return(E_INVALIDARG);
        return;
    }
    if (buffer == 0) {
        m.set_return(E_POINTER);
        return;
    }
    const auto& value = strings->second.localized_value;
    if (size < value.size() + 1) {
        m.set_return(E_NOT_SUFFICIENT_BUFFER);
        return;
    }
    for (usize i = 0; i < value.size(); ++i) {
        if (m.write_u16(buffer + i * sizeof(u16), value[i]).is_err()) {
            m.set_return(E_POINTER);
            return;
        }
    }
    if (m.write_u16(buffer + value.size() * sizeof(u16), 0).is_err()) {
        m.set_return(E_POINTER);
        return;
    }
    if (dwrite_diag_enabled()) {
        std::string ascii_value;
        ascii_value.reserve(value.size());
        for (const char16_t ch : value) {
            ascii_value.push_back(ch <= 0x7fu ? static_cast<char>(ch) : '?');
        }
        dwrite_diag_printf(
                     "[dwrite-diag] LocalizedStrings::GetString "
                     "strings=0x%llx index=0 value='%s' length=%zu -> S_OK\n",
                     static_cast<unsigned long long>(m.arg_ptr(0)),
                     ascii_value.c_str(), value.size());
    }
    m.set_return(S_OK);
}

} // namespace vitreo::winapi
