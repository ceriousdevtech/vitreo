// SPDX-License-Identifier: LGPL-2.1-only
/// @file combase_globalization.cpp
/// @brief Windows.System.UserProfile.GlobalizationPreferences and the string IVectorView

#include "winapi/com/combase/combase.hpp"
#include "winapi/com/combase/combase_internal.hpp"

#include <CoreFoundation/CoreFoundation.h>

#include <algorithm>
#include <string>

namespace vitreo::winapi {

namespace {

std::u16string cf_string_to_u16(CFStringRef value) {
    if (value == nullptr) return {};
    const CFIndex length = CFStringGetLength(value);
    if (length <= 0) return {};
    std::u16string result(static_cast<usize>(length), u'\0');
    CFStringGetCharacters(
        value, CFRangeMake(0, length),
        reinterpret_cast<UniChar*>(result.data()));
    return result;
}

std::u16string normalize_language_tag(std::u16string value) {
    for (char16_t& ch : value) {
        if (ch == u'_') ch = u'-';
    }
    const auto suffix = value.find(u'@');
    if (suffix != std::u16string::npos) value.resize(suffix);
    return value;
}

std::u16string calendar_identifier(CFStringRef identifier) {
    const std::u16string value = cf_string_to_u16(identifier);
    if (value.find(u"japanese") != std::u16string::npos) {
        return u"JapaneseCalendar";
    }
    if (value.find(u"buddhist") != std::u16string::npos) {
        return u"ThaiCalendar";
    }
    if (value.find(u"hebrew") != std::u16string::npos) {
        return u"HebrewCalendar";
    }
    if (value.find(u"islamic") != std::u16string::npos) {
        return u"HijriCalendar";
    }
    if (value.find(u"persian") != std::u16string::npos) {
        return u"PersianCalendar";
    }
    return u"GregorianCalendar";
}

bool uses_24_hour_clock(CFLocaleRef locale) {
    CFStringRef format = CFDateFormatterCreateDateFormatFromTemplate(
        kCFAllocatorDefault, CFSTR("j"), 0, locale);
    if (format == nullptr) return false;
    const std::u16string value = cf_string_to_u16(format);
    CFRelease(format);
    return value.find(u'H') != std::u16string::npos ||
           value.find(u'k') != std::u16string::npos;
}

} // namespace

WinrtGlobalizationPreferences detect_host_globalization_preferences() {
    WinrtGlobalizationPreferences result;
    CFLocaleRef locale = CFLocaleCopyCurrent();

    if (CFArrayRef preferred = CFLocaleCopyPreferredLanguages()) {
        const CFIndex count = CFArrayGetCount(preferred);
        for (CFIndex i = 0; i < count; ++i) {
            auto value = static_cast<CFStringRef>(
                CFArrayGetValueAtIndex(preferred, i));
            std::u16string tag = normalize_language_tag(
                cf_string_to_u16(value));
            if (!tag.empty()) result.languages.push_back(std::move(tag));
        }
        CFRelease(preferred);
    }

    if (locale != nullptr) {
        if (result.languages.empty()) {
            auto identifier = static_cast<CFStringRef>(
                CFLocaleGetValue(locale, kCFLocaleIdentifier));
            std::u16string tag = normalize_language_tag(
                cf_string_to_u16(identifier));
            if (!tag.empty()) result.languages.push_back(std::move(tag));
        }
        result.home_geographic_region = cf_string_to_u16(
            static_cast<CFStringRef>(
                CFLocaleGetValue(locale, kCFLocaleCountryCode)));
        std::u16string currency = cf_string_to_u16(
            static_cast<CFStringRef>(
                CFLocaleGetValue(locale, kCFLocaleCurrencyCode)));
        if (!currency.empty()) result.currencies.push_back(std::move(currency));
        result.calendars.push_back(calendar_identifier(
            static_cast<CFStringRef>(
                CFLocaleGetValue(locale, kCFLocaleCalendarIdentifier))));
        result.clocks.push_back(
            uses_24_hour_clock(locale) ? u"24HourClock" : u"12HourClock");
        CFRelease(locale);
    }

    if (CFCalendarRef calendar = CFCalendarCopyCurrent()) {
        const CFIndex first_weekday = CFCalendarGetFirstWeekday(calendar);
        if (first_weekday >= 1 && first_weekday <= 7) {
            result.week_starts_on = static_cast<u32>(first_weekday - 1);
        }
        CFRelease(calendar);
    }

    if (result.languages.empty()) result.languages.push_back(u"en-US");
    if (result.home_geographic_region.empty()) {
        result.home_geographic_region = u"US";
    }
    if (result.currencies.empty()) result.currencies.push_back(u"USD");
    if (result.calendars.empty()) {
        result.calendars.push_back(u"GregorianCalendar");
    }
    if (result.clocks.empty()) result.clocks.push_back(u"12HourClock");
    return result;
}

u32 CombaseManager::return_vector(
    Marshaler& m, const std::vector<std::u16string>& values) {
    auto object = m_objects.find(m.arg_ptr(0));
    const GuestAddr vector_out = m.arg_ptr(1);
    if (object == m_objects.end() ||
        object->second.type != ObjectType::GlobalizationPreferences ||
        vector_out == 0) {
        return E_INVALIDARG;
    }
    (void)m.write_ptr(vector_out, 0);
    const GuestAddr vector =
        create_vector_view(values, m.cpu_is_64bit());
    if (vector == 0) return E_OUTOFMEMORY;
    if (m.write_ptr(vector_out, vector).is_err()) return E_POINTER;
    return S_OK;
}

void CombaseManager::impl_GetCalendars(Marshaler& m) {
    m.set_return(return_vector(m, m_preferences.calendars));
}

void CombaseManager::impl_GetClocks(Marshaler& m) {
    m.set_return(return_vector(m, m_preferences.clocks));
}

void CombaseManager::impl_GetCurrencies(Marshaler& m) {
    m.set_return(return_vector(m, m_preferences.currencies));
}

void CombaseManager::impl_GetLanguages(Marshaler& m) {
    m.set_return(return_vector(m, m_preferences.languages));
}

void CombaseManager::impl_GetHomeGeographicRegion(Marshaler& m) {
    auto object = m_objects.find(m.arg_ptr(0));
    const GuestAddr region_out = m.arg_ptr(1);
    if (object == m_objects.end() ||
        object->second.type != ObjectType::GlobalizationPreferences ||
        region_out == 0) {
        m.set_return(E_INVALIDARG);
        return;
    }
    const GuestAddr region =
        create_hstring(
            m_preferences.home_geographic_region, m.cpu_is_64bit());
    if (region == 0 || m.write_ptr(region_out, region).is_err()) {
        m.set_return(E_OUTOFMEMORY);
        return;
    }
    m.set_return(S_OK);
}

void CombaseManager::impl_GetWeekStartsOn(Marshaler& m) {
    auto object = m_objects.find(m.arg_ptr(0));
    const GuestAddr day_out = m.arg_ptr(1);
    if (object == m_objects.end() ||
        object->second.type != ObjectType::GlobalizationPreferences ||
        day_out == 0 ||
        m.write_u32(day_out, m_preferences.week_starts_on).is_err()) {
        m.set_return(E_INVALIDARG);
        return;
    }
    m.set_return(S_OK);
}

void CombaseManager::impl_VectorGetAt(Marshaler& m) {
    auto object = m_objects.find(m.arg_ptr(0));
    const u32 index = static_cast<u32>(m.arg(1));
    const GuestAddr value_out = m.arg_ptr(2);
    if (object == m_objects.end() ||
        object->second.type != ObjectType::StringVectorView ||
        value_out == 0) {
        m.set_return(E_INVALIDARG);
        return;
    }
    (void)m.write_ptr(value_out, 0);
    if (index >= object->second.values.size()) {
        m.set_return(E_BOUNDS);
        return;
    }
    const GuestAddr value = create_hstring(
        object->second.values[index], m.cpu_is_64bit());
    if (value == 0 || m.write_ptr(value_out, value).is_err()) {
        m.set_return(E_OUTOFMEMORY);
        return;
    }
    m.set_return(S_OK);
}

void CombaseManager::impl_VectorGetSize(Marshaler& m) {
    auto object = m_objects.find(m.arg_ptr(0));
    const GuestAddr size_out = m.arg_ptr(1);
    if (object == m_objects.end() ||
        object->second.type != ObjectType::StringVectorView ||
        size_out == 0 ||
        m.write_u32(
            size_out, static_cast<u32>(object->second.values.size())).is_err()) {
        m.set_return(E_INVALIDARG);
        return;
    }
    m.set_return(S_OK);
}

void CombaseManager::impl_VectorIndexOf(Marshaler& m) {
    auto object = m_objects.find(m.arg_ptr(0));
    const GuestAddr index_out = m.arg_ptr(2);
    const GuestAddr found_out = m.arg_ptr(3);
    auto value = read_hstring(m.arg_ptr(1), m.cpu_is_64bit());
    if (object == m_objects.end() ||
        object->second.type != ObjectType::StringVectorView ||
        !value.has_value() || index_out == 0 || found_out == 0) {
        m.set_return(E_INVALIDARG);
        return;
    }
    const auto found = std::find(
        object->second.values.begin(), object->second.values.end(), *value);
    const bool exists = found != object->second.values.end();
    const u32 index = exists
        ? static_cast<u32>(
              std::distance(object->second.values.begin(), found))
        : 0;
    if (m.write_u32(index_out, index).is_err() ||
        m.write_u8(found_out, exists ? 1u : 0u).is_err()) {
        m.set_return(E_POINTER);
        return;
    }
    m.set_return(S_OK);
}

void CombaseManager::impl_VectorGetMany(Marshaler& m) {
    auto object = m_objects.find(m.arg_ptr(0));
    const u32 start = static_cast<u32>(m.arg(1));
    const u32 capacity = static_cast<u32>(m.arg(2));
    const GuestAddr values_out = m.arg_ptr(3);
    const GuestAddr actual_out = m.arg_ptr(4);
    if (object == m_objects.end() ||
        object->second.type != ObjectType::StringVectorView ||
        actual_out == 0 || (capacity != 0 && values_out == 0)) {
        m.set_return(E_INVALIDARG);
        return;
    }
    if (start > object->second.values.size()) {
        (void)m.write_u32(actual_out, 0);
        m.set_return(E_BOUNDS);
        return;
    }
    const u32 available =
        static_cast<u32>(object->second.values.size() - start);
    const u32 actual = std::min(capacity, available);
    const bool is_64bit = m.cpu_is_64bit();
    const usize pointer_size = is_64bit ? 8 : 4;
    for (u32 i = 0; i < actual; ++i) {
        const GuestAddr value =
            create_hstring(
                object->second.values[start + i], is_64bit);
        if (value == 0 ||
            !write_pointer(
                values_out + i * pointer_size, value, is_64bit)) {
            m.set_return(E_OUTOFMEMORY);
            return;
        }
    }
    if (m.write_u32(actual_out, actual).is_err()) {
        m.set_return(E_POINTER);
        return;
    }
    m.set_return(S_OK);
}

} // namespace vitreo::winapi
