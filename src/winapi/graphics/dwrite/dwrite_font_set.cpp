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
void DWriteManager::impl_CreateFontFallbackBuilder(Marshaler& m) {
    const GuestAddr self = m.arg_ptr(0);
    const GuestAddr builder_out = m.arg_ptr(1);
    if (builder_out == 0) {
        m.set_return(E_POINTER);
        return;
    }
    (void)m.write_ptr(builder_out, 0);
    const auto factory = m_objects.find(self);
    if (factory == m_objects.end() ||
        factory->second.type != ObjectType::Factory) {
        m.set_return(E_INVALIDARG);
        return;
    }
    const GuestAddr builder = create_object(ObjectType::FontFallbackBuilder);
    if (builder == 0 || m.write_ptr(builder_out, builder).is_err()) {
        if (builder != 0) m_objects.erase(builder);
        m.set_return(E_OUTOFMEMORY);
        return;
    }
    if (dwrite_diag_enabled()) {
        dwrite_diag_printf(
                     "[dwrite-diag] CreateFontFallbackBuilder factory=0x%llx "
                     "-> S_OK builder=0x%llx\n",
                     static_cast<unsigned long long>(self),
                     static_cast<unsigned long long>(builder));
    }
    m.set_return(S_OK);
}

void DWriteManager::impl_GetSystemFontSet(Marshaler& m) {
    const GuestAddr self = m.arg_ptr(0);
    const GuestAddr font_set_out = m.arg_ptr(1);
    if (font_set_out == 0) {
        m.set_return(E_POINTER);
        return;
    }
    (void)m.write_ptr(font_set_out, 0);
    const auto factory = m_objects.find(self);
    if (factory == m_objects.end() ||
        factory->second.type != ObjectType::Factory) {
        m.set_return(E_INVALIDARG);
        return;
    }
    const GuestAddr font_set = create_object(ObjectType::FontSet);
    if (font_set == 0 || m.write_ptr(font_set_out, font_set).is_err()) {
        if (font_set != 0) m_objects.erase(font_set);
        m.set_return(E_OUTOFMEMORY);
        return;
    }
    if (dwrite_diag_enabled()) {
        dwrite_diag_printf(
                     "[dwrite-diag] GetSystemFontSet factory=0x%llx "
                     "-> S_OK fontSet=0x%llx count=%zu\n",
                     static_cast<unsigned long long>(self),
                     static_cast<unsigned long long>(font_set),
                     m_font_families.size());
    }
    m.set_return(S_OK);
}

void DWriteManager::impl_GetFontDownloadQueue(Marshaler& m) {
    const GuestAddr self = m.arg_ptr(0);
    const GuestAddr queue_out = m.arg_ptr(1);
    if (queue_out == 0) {
        m.set_return(E_POINTER);
        return;
    }
    (void)m.write_ptr(queue_out, 0);
    const auto factory = m_objects.find(self);
    if (factory == m_objects.end() ||
        factory->second.type != ObjectType::Factory) {
        m.set_return(E_INVALIDARG);
        return;
    }
    const GuestAddr queue = create_object(ObjectType::FontDownloadQueue);
    if (queue == 0 || m.write_ptr(queue_out, queue).is_err()) {
        if (queue != 0) m_objects.erase(queue);
        m.set_return(E_OUTOFMEMORY);
        return;
    }
    if (dwrite_diag_enabled()) {
        dwrite_diag_printf(
                     "[dwrite-diag] GetFontDownloadQueue factory=0x%llx "
                     "-> S_OK queue=0x%llx\n",
                     static_cast<unsigned long long>(self),
                     static_cast<unsigned long long>(queue));
    }
    m.set_return(S_OK);
}

void DWriteManager::impl_FontSetGetFontCount(Marshaler& m) {
    const auto font_set = m_objects.find(m.arg_ptr(0));
    if (font_set == m_objects.end() ||
        font_set->second.type != ObjectType::FontSet) {
        m.set_return(0);
        return;
    }
    if (font_set->second.font_set_empty) {
        m.set_return(0);
        return;
    }
    if (font_set->second.font_set_single_family) {
        m.set_return(1);
        return;
    }
    m.set_return(static_cast<u32>(m_font_families.size()));
}

void DWriteManager::impl_FontSetGetMatchingFontsFamily(Marshaler& m) {
    const GuestAddr self = m.arg_ptr(0);
    const GuestAddr family_name = m.arg_ptr(1);
    (void)m.arg(2); // weight
    (void)m.arg(3); // stretch
    (void)m.arg(4); // style
    const GuestAddr set_out = m.arg_ptr(5);
    if (set_out == 0) {
        m.set_return(E_POINTER);
        return;
    }
    (void)m.write_ptr(set_out, 0);
    const auto font_set = m_objects.find(self);
    if (font_set == m_objects.end() ||
        font_set->second.type != ObjectType::FontSet) {
        m.set_return(E_INVALIDARG);
        return;
    }
    auto name = read_utf16_z(family_name, 256);
    bool matched = false;
    usize matched_index = 0;
    if (name.is_ok() && !font_set->second.font_set_empty) {
        for (usize i = 0; i < m_font_families.size(); ++i) {
            if (m_font_families[i] == name.value()) {
                matched = true;
                matched_index = i;
                break;
            }
        }
    }
    const GuestAddr filtered = create_object(ObjectType::FontSet);
    if (filtered == 0 || m.write_ptr(set_out, filtered).is_err()) {
        if (filtered != 0) m_objects.erase(filtered);
        m.set_return(E_OUTOFMEMORY);
        return;
    }
    auto& object = m_objects.at(filtered);
    if (matched) {
        object.font_set_single_family = true;
        object.family_index = matched_index;
    } else {
        object.font_set_empty = true;
    }
    m.set_return(S_OK);
}

void DWriteManager::impl_FontSetGetMatchingFontsProperties(Marshaler& m) {
    const GuestAddr self = m.arg_ptr(0);
    (void)m.arg_ptr(1); // properties
    (void)m.arg(2); // propertyCount
    const GuestAddr set_out = m.arg_ptr(3);
    if (set_out == 0) {
        m.set_return(E_POINTER);
        return;
    }
    (void)m.write_ptr(set_out, 0);
    const auto font_set = m_objects.find(self);
    if (font_set == m_objects.end() ||
        font_set->second.type != ObjectType::FontSet) {
        m.set_return(E_INVALIDARG);
        return;
    }
    // Property filtering is not implemented yet; return an empty matching set
    // rather than failing the call (documented when no fonts match).
    const GuestAddr filtered = create_object(ObjectType::FontSet);
    if (filtered == 0 || m.write_ptr(set_out, filtered).is_err()) {
        if (filtered != 0) m_objects.erase(filtered);
        m.set_return(E_OUTOFMEMORY);
        return;
    }
    m_objects.at(filtered).font_set_empty = true;
    m.set_return(S_OK);
}

void DWriteManager::impl_FallbackBuilderAddMapping(Marshaler& m) {
    const GuestAddr self = m.arg_ptr(0);
    const auto builder = m_objects.find(self);
    if (builder == m_objects.end() ||
        builder->second.type != ObjectType::FontFallbackBuilder) {
        m.set_return(E_INVALIDARG);
        return;
    }
    // Accept mappings; CreateFontFallback currently yields the system fallback.
    (void)m.arg(1);
    (void)m.arg(2);
    (void)m.arg(3);
    (void)m.arg(4);
    (void)m.arg(5);
    (void)m.arg(6);
    (void)m.arg(7);
    (void)m.arg(8);
    m.set_return(S_OK);
}

void DWriteManager::impl_FallbackBuilderAddMappings(Marshaler& m) {
    const GuestAddr self = m.arg_ptr(0);
    (void)m.arg_ptr(1);
    const auto builder = m_objects.find(self);
    if (builder == m_objects.end() ||
        builder->second.type != ObjectType::FontFallbackBuilder) {
        m.set_return(E_INVALIDARG);
        return;
    }
    m.set_return(S_OK);
}

void DWriteManager::impl_FallbackBuilderCreateFontFallback(Marshaler& m) {
    const GuestAddr self = m.arg_ptr(0);
    const GuestAddr fallback_out = m.arg_ptr(1);
    if (fallback_out == 0) {
        m.set_return(E_POINTER);
        return;
    }
    (void)m.write_ptr(fallback_out, 0);
    const auto builder = m_objects.find(self);
    if (builder == m_objects.end() ||
        builder->second.type != ObjectType::FontFallbackBuilder) {
        m.set_return(E_INVALIDARG);
        return;
    }
    const GuestAddr fallback = create_object(ObjectType::FontFallback);
    if (fallback == 0 || m.write_ptr(fallback_out, fallback).is_err()) {
        if (fallback != 0) m_objects.erase(fallback);
        m.set_return(E_OUTOFMEMORY);
        return;
    }
    m.set_return(S_OK);
}

void DWriteManager::impl_DownloadQueueIsEmpty(Marshaler& m) {
    const auto queue = m_objects.find(m.arg_ptr(0));
    m.set_return(queue != m_objects.end() &&
                         queue->second.type == ObjectType::FontDownloadQueue
                     ? 1u
                     : 0u);
}

void DWriteManager::impl_DownloadQueueBeginDownload(Marshaler& m) {
    const GuestAddr self = m.arg_ptr(0);
    (void)m.arg_ptr(1); // optional listener/context
    const auto queue = m_objects.find(self);
    if (queue == m_objects.end() ||
        queue->second.type != ObjectType::FontDownloadQueue) {
        m.set_return(E_INVALIDARG);
        return;
    }
    // Queue is always empty for local system fonts.
    m.set_return(0x00000001u); // S_FALSE
}

void DWriteManager::impl_DownloadQueueGetGenerationCount(Marshaler& m) {
    const auto queue = m_objects.find(m.arg_ptr(0));
    m.set_return(queue != m_objects.end() &&
                         queue->second.type == ObjectType::FontDownloadQueue
                     ? 0u
                     : 0u);
}

void DWriteManager::impl_GetSystemFontFallback(Marshaler& m) {
    const GuestAddr self = m.arg_ptr(0);
    const GuestAddr fallback_out = m.arg_ptr(1);
    if (fallback_out == 0) {
        m.set_return(E_POINTER);
        return;
    }
    (void)m.write_ptr(fallback_out, 0);
    const auto factory = m_objects.find(self);
    if (factory == m_objects.end() ||
        factory->second.type != ObjectType::Factory) {
        m.set_return(E_INVALIDARG);
        return;
    }

    const GuestAddr fallback = create_object(ObjectType::FontFallback);
    if (fallback == 0 || m.write_ptr(fallback_out, fallback).is_err()) {
        if (fallback != 0) m_objects.erase(fallback);
        m.set_return(E_POINTER);
        return;
    }
    if (dwrite_diag_enabled()) {
        dwrite_diag_printf(
                     "[dwrite-diag] GetSystemFontFallback factory=0x%llx "
                     "-> S_OK fallback=0x%llx\n",
                     static_cast<unsigned long long>(self),
                     static_cast<unsigned long long>(fallback));
    }
    m.set_return(S_OK);
}

void DWriteManager::impl_FallbackMapCharacters(Marshaler& m) {
    const GuestAddr self = m.arg_ptr(0);
    const GuestAddr analysis_source = m.arg_ptr(1);
    const u32 text_position = static_cast<u32>(m.arg(2));
    const u32 text_length = static_cast<u32>(m.arg(3));
    const GuestAddr base_collection = m.arg_ptr(4);
    const GuestAddr base_family_address = m.arg_ptr(5);
    const u32 weight = static_cast<u32>(m.arg(6));
    const u32 style = static_cast<u32>(m.arg(7));
    const u32 stretch = static_cast<u32>(m.arg(8));
    const GuestAddr mapped_length_out = m.arg_ptr(9);
    const GuestAddr mapped_font_out = m.arg_ptr(10);
    const GuestAddr scale_out = m.arg_ptr(11);

    const auto fallback = m_objects.find(self);
    if (fallback == m_objects.end() ||
        fallback->second.type != ObjectType::FontFallback ||
        analysis_source == 0 || mapped_length_out == 0 ||
        mapped_font_out == 0 || scale_out == 0 || weight < 1 ||
        weight > 999 || style > 2 || stretch < 1 || stretch > 9) {
        m.set_return(E_INVALIDARG);
        return;
    }
    if (base_collection != 0) {
        const auto collection = m_objects.find(base_collection);
        if (collection == m_objects.end() ||
            collection->second.type != ObjectType::FontCollection) {
            m.set_return(E_INVALIDARG);
            return;
        }
    }

    const float scale = 1.0f;
    if (m.write_u32(mapped_length_out, 0).is_err() ||
        m.write_ptr(mapped_font_out, 0).is_err() ||
        m.write_struct(scale_out, scale).is_err()) {
        m.set_return(E_POINTER);
        return;
    }
    if (text_length == 0) {
        m.set_return(S_OK);
        return;
    }
    if (!m_guest_call) {
        m.set_return(E_NOTIMPL);
        return;
    }

    const usize pointer_size = m_is_64bit ? 8 : 4;
    const auto source_vtable = read_pointer(analysis_source);
    if (source_vtable.is_err() || source_vtable.value() == 0) {
        m.set_return(E_INVALIDARG);
        return;
    }
    const auto get_text = read_pointer(
        source_vtable.value() + 3 * pointer_size);
    const auto get_locale = read_pointer(
        source_vtable.value() + 6 * pointer_size);
    if (get_text.is_err() || get_text.value() == 0 ||
        get_locale.is_err() || get_locale.value() == 0) {
        m.set_return(E_INVALIDARG);
        return;
    }

    const GuestAddr text_address_out = m_callback_scratch;
    const GuestAddr text_length_out = m_callback_scratch + 0x10;
    const GuestAddr locale_address_out = m_callback_scratch + 0x20;
    const GuestAddr locale_range_out = m_callback_scratch + 0x30;
    if (!write_pointer(text_address_out, 0) ||
        m_memory.write_u32(text_length_out, 0).is_err() ||
        !write_pointer(locale_address_out, 0) ||
        m_memory.write_u32(locale_range_out, 0).is_err()) {
        m.set_return(E_POINTER);
        return;
    }

    const u64 text_args[4]{
        analysis_source, text_position, text_address_out, text_length_out};
    const u32 text_result = static_cast<u32>(m_guest_call(
        get_text.value(), text_args, std::size(text_args), m_is_64bit));
    if ((text_result & 0x80000000u) != 0) {
        m.set_return(text_result);
        return;
    }
    const auto text_address = read_pointer(text_address_out);
    const auto available_length = m_memory.read_u32(text_length_out);
    if (text_address.is_err() || available_length.is_err() ||
        text_address.value() == 0 || available_length.value() == 0) {
        m.set_return(E_INVALIDARG);
        return;
    }
    const usize chunk_length = std::min<usize>(
        {available_length.value(), text_length, MAX_FALLBACK_TEXT_UNITS});
    auto text = read_utf16(text_address.value(), chunk_length);
    if (text.is_err()) {
        m.set_return(E_INVALIDARG);
        return;
    }

    const u64 locale_args[4]{
        analysis_source, text_position, locale_range_out, locale_address_out};
    const u32 locale_result = static_cast<u32>(m_guest_call(
        get_locale.value(), locale_args, std::size(locale_args), m_is_64bit));
    if ((locale_result & 0x80000000u) != 0) {
        m.set_return(locale_result);
        return;
    }
    std::u16string locale;
    const auto locale_address = read_pointer(locale_address_out);
    if (locale_address.is_ok() && locale_address.value() != 0) {
        auto value = read_utf16_z(locale_address.value(), 85);
        if (value.is_err()) {
            m.set_return(E_INVALIDARG);
            return;
        }
        locale = std::move(value.value());
    }

    std::u16string base_family;
    if (base_family_address != 0) {
        auto value = read_utf16_z(base_family_address, 256);
        if (value.is_err()) {
            m.set_return(E_INVALIDARG);
            return;
        }
        base_family = std::move(value.value());
    }

    auto match = dwrite::map_system_font(
        text.value(), base_family, weight, stretch, style, locale);
    if (!match || match->mapped_length == 0 ||
        match->mapped_length > chunk_length) {
        m.set_return(DWRITE_E_FILEFORMAT);
        return;
    }

    GuestAddr mapped_font = 0;
    if (match->has_font) {
        const usize family_index = ensure_family_index(match->family_name);
        mapped_font = create_object(
            ObjectType::Font, family_index, std::move(match->font));
        if (mapped_font == 0) {
            m.set_return(DWRITE_E_FILEFORMAT);
            return;
        }
    }
    if (m.write_u32(mapped_length_out, match->mapped_length).is_err() ||
        m.write_ptr(mapped_font_out, mapped_font).is_err()) {
        if (mapped_font != 0) m_objects.erase(mapped_font);
        m.set_return(E_POINTER);
        return;
    }
    if (dwrite_diag_enabled()) {
        dwrite_diag_printf(
                     "[dwrite-diag] MapCharacters source=0x%llx pos=%u "
                     "requested=%u mapped=%u font=0x%llx\n",
                     static_cast<unsigned long long>(analysis_source),
                     text_position, text_length, match->mapped_length,
                     static_cast<unsigned long long>(mapped_font));
    }
    m.set_return(S_OK);
}

} // namespace vitreo::winapi
