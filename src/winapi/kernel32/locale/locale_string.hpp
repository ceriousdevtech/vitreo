// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file locale_string.hpp
/// @brief Locale, string comparison, and SRW lock stubs for kernel32

#include <vitreo/common.hpp>
#include "winapi/core/thunk_registry.hpp"
#include "winapi/kernel32/locale/number_format.hpp"

#include <functional>
#include <mutex>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace vitreo::winapi {

class Marshaler;
class SyncManager;

/// True when Windows reads @p name as a well-formed locale name: one it
/// answers, with real or fallback data, rather than failing with
/// ERROR_INVALID_PARAMETER. Subtags are split on '-' or '_'. The grammar
/// follows every gnfex-name-* / -grammar-* / -fix1-name-* line of
/// tools/oracle_probe/getnumberformat_probe64.expected:
/// - a language of 2-3 letters, then an optional 4-letter script, then an
///   optional region of 2 letters or 3 digits;
/// - after a region, any subtags of 1-8 ASCII letters and digits;
/// - without a region, only a singleton followed by at least one subtag;
/// - or a leading singleton with subtags (x-foo, i-klingon).
/// The whole name is under 85 characters.
/// The *Ex forms use it only to say why a name they cannot answer failed.
[[nodiscard]] bool locale_name_well_formed(std::u16string_view name);

/// Stub implementations for locale, string, and SRW-lock kernel32 APIs.
///
/// All synchronization primitives are no-ops (single-threaded engine).
/// String comparisons return CSTR_EQUAL for now; locale values are
/// hard-coded to en-US / OEM-US.
class LocaleStringManager {
public:
    using GuestCall = std::function<u64(GuestAddr, const u64*, usize, bool)>;

    void set_sync_manager(SyncManager* mgr) { m_sync_manager = mgr; }
    void set_guest_call(GuestCall call) { m_guest_call = std::move(call); }
    /// When set, GetConsoleOutputCP returns 0 + ERROR_INVALID_HANDLE if the
    /// process has no console (process_contract_probe.expected after FreeConsole).
    using ConsoleAttachedQuery = std::function<bool()>;
    void set_console_attached_query(ConsoleAttachedQuery q) {
        m_console_attached = std::move(q);
    }
    [[nodiscard]] bool console_is_attached() const {
        return !m_console_attached || m_console_attached();
    }

    /// GetConsoleOutputCP / SetConsoleOutputCP — the code page the console
    /// translates written bytes with. It defaults to OEM-US, and a console
    /// application that wants UTF-8 output sets it to 65001 at startup; the
    /// getter has to observe what the setter stored or the pair is incoherent.
    [[nodiscard]] u32 get_console_output_cp() const { return m_console_output_cp; }
    void set_console_output_cp(u32 cp) { m_console_output_cp = cp; }


    /// GetThreadLocale. A new thread starts at the user default LCID (en-US).
    [[nodiscard]] u32 get_thread_locale() const;

    /// SetThreadLocale. Stores the resolved LCID for this thread and returns
    /// TRUE. An LCID that is neither installed nor one of the default aliases
    /// returns FALSE; the caller sets ERROR_INVALID_PARAMETER.
    [[nodiscard]] bool set_thread_locale(u32 locale);

    /// lstrlenW — length of a wide (UTF-16) string in guest memory
    [[nodiscard]] i32 lstrlen_w(Marshaler& m, GuestAddr ptr) const;

    /// CompareStringA — compare two ANSI strings
    [[nodiscard]] i32 compare_string_a(Marshaler& m, u32 locale, u32 flags,
                                       GuestAddr str1, i32 len1,
                                       GuestAddr str2, i32 len2) const;

    /// CompareStringW over host strings (LOCALE_USER_DEFAULT): CSTR_LESS_THAN,
    /// CSTR_EQUAL or CSTR_GREATER_THAN, 0 with the last error on failure.
    [[nodiscard]] static i32 compare_units(const std::vector<u16>& s1, const std::vector<u16>& s2, u32 flags);

    /// CompareStringW — compare two wide strings
    [[nodiscard]] i32 compare_string_w(Marshaler& m, u32 locale, u32 flags,
                                       GuestAddr str1, i32 len1,
                                       GuestAddr str2, i32 len2) const;

    /// GetTimeFormatA/W — expand a time picture string against a SYSTEMTIME.
    /// `wide` selects the encoding of both lpFormat and the output buffer.
    /// Returns the character count including the null terminator, or 0 on
    /// failure (LastError is set).
    [[nodiscard]] i32 get_time_format(Marshaler& m, bool wide,
                                      u32 locale, u32 flags,
                                      GuestAddr time_ptr,
                                      GuestAddr fmt_ptr,
                                      GuestAddr buf, i32 buf_size) const;

    /// GetUserDefaultUILanguage — return en-US LANGID
    [[nodiscard]] u32 get_user_default_ui_language() const { return 0x0409; }

    /// GetUserDefaultLangID — return en-US LANGID
    [[nodiscard]] u32 get_user_default_lang_id() const { return 0x0409; }

    /// GetUserDefaultLCID — return en-US LCID
    [[nodiscard]] u32 get_user_default_lcid() const { return 0x0409; }

    /// GetSystemDefaultLangID — return en-US LANGID
    [[nodiscard]] u32 get_system_default_lang_id() const { return 0x0409; }

    /// GetSystemDefaultLCID — return en-US LCID
    [[nodiscard]] u32 get_system_default_lcid() const { return 0x0409; }

    /// GetSystemDefaultUILanguage — return en-US LANGID
    [[nodiscard]] u32 get_system_default_ui_language() const { return 0x0409; }

    /// GetLocaleInfoW — return locale information for en-US
    [[nodiscard]] i32 get_locale_info_w(Marshaler& m, u32 locale, u32 lc_type,
                                        GuestAddr buf, i32 cch_buf) const;

    /// The default number format of @p locale (what GetNumberFormat uses
    /// with lpFormat NULL): the profile's LOCALE_SDECIMAL / LOCALE_STHOUSAND
    /// with IDIGITS 2, ILZERO 1, SGROUPING 3;0 and INEGNUMBER 1, the values of
    /// every profile GetLocaleInfoW answers (getnumberformat_probe64.expected
    /// gnfw-us-null-fmt, -locale-invariant, -locale-de-DE). False for an LCID
    /// GetLocaleInfoW rejects (ERROR_INVALID_PARAMETER).
    [[nodiscard]] bool number_format_defaults(u32 locale, NumberFormatSpec& out) const;

    /// The LCID of the profile a non-NULL lpLocaleName names, for the *Ex
    /// forms: LOCALE_NAME_INVARIANT ("") is 0x007F, LOCALE_NAME_SYSTEM_DEFAULT
    /// ("!x-sys-default-locale", any case) LOCALE_SYSTEM_DEFAULT, a profile's
    /// LOCALE_SNAME (ASCII case folded, '_' or '-' between subtags), a
    /// measured alternate sort of one, or a measured alias with the same
    /// number data (en, de, zh-Hans-CN) its LCID. Nullopt for every other name,
    /// which the caller fails with ERROR_INVALID_PARAMETER: Windows does that
    /// for a malformed name (getnumberformat_probe64.expected
    /// gnfex-name-not-a-locale), but answers a well-formed name it has no data
    /// for with fallback data (gnfex-name-xx-YY), and Vitreo has no data for
    /// names outside its profile table to tell the two apart.
    [[nodiscard]] std::optional<u32> locale_name_profile_lcid(std::u16string_view name) const;

    /// GetLocaleInfoEx — same data, but addressed by locale name.
    [[nodiscard]] i32 get_locale_info_ex(Marshaler& m, GuestAddr locale_name,
                                         u32 lc_type, GuestAddr buf,
                                         i32 cch_buf) const;

    /// CompareStringEx — compare two wide strings for a named locale.
    [[nodiscard]] i32 compare_string_ex(Marshaler& m, GuestAddr locale_name,
                                        u32 flags, GuestAddr str1, i32 len1,
                                        GuestAddr str2, i32 len2) const;

    /// IsValidLocale — check if a locale is valid
    [[nodiscard]] u32 is_valid_locale(u32 locale, u32 flags) const;

    /// EnumSystemLocalesA/W — invoke LOCALE_ENUMPROC with LCID hex strings
    /// (e.g. "00000409"). Continues until locales are exhausted or the
    /// callback returns FALSE. Returns nonzero on success.
    [[nodiscard]] u32 enum_system_locales(Marshaler& m, bool wide,
                                          GuestAddr callback, u32 flags);

    /// EnumSystemLocalesEx — invoke LOCALE_ENUMPROCEX with locale names
    /// (e.g. L"en-US"). Returns nonzero on success.
    [[nodiscard]] u32 enum_system_locales_ex(Marshaler& m, GuestAddr callback,
                                             u32 flags, u64 lparam);

    /// EnumUILanguagesA/W — invoke UILANGUAGE_ENUMPROC(lang, lParam) once per
    /// installed UI language: the system default UI language, then the user
    /// default UI language when it differs. MUI_LANGUAGE_ID (default) passes
    /// "0409", MUI_LANGUAGE_NAME passes "en-US". A callback FALSE stops the
    /// enumeration and the call still returns TRUE. Flag validation and errors
    /// follow tools/oracle_probe/enum_ui_languages_probe.expected.
    [[nodiscard]] u32 enum_ui_languages(Marshaler& m, bool wide,
                                        GuestAddr callback, u32 flags,
                                        u64 lparam);

    /// EnumCalendarInfoA/W. Calls the callback once per calendar string.
    /// A callback return of FALSE stops the enumeration; the API still
    /// returns TRUE. Failure returns 0 and sets LastError.
    [[nodiscard]] u32 enum_calendar_info(Marshaler& m, bool wide,
                                         GuestAddr callback, u32 locale,
                                         u32 calendar, u32 cal_type);

    /// GetACP — process ANSI code page (1252, or 65001 with UTF-8 manifest).
    [[nodiscard]] u32 get_acp() const;

    /// GetOEMCP — return OEM code page
    [[nodiscard]] u32 get_oem_cp() const { return 437; }

    /// FindStringOrdinal — ordinal string search
    [[nodiscard]] i32 find_string_ordinal(Marshaler& m, u32 flags,
                                           GuestAddr source_ptr, i32 cch_source,
                                           GuestAddr value_ptr, i32 cch_value,
                                           u32 ignore_case) const;

    void acquire_srw_lock_exclusive(GuestAddr lock) const;
    void release_srw_lock_exclusive(GuestAddr lock) const;
    [[nodiscard]] bool try_acquire_srw_lock_exclusive(GuestAddr lock) const;
    void acquire_srw_lock_shared(GuestAddr lock) const;
    /// True when this released the LAST shared holder.
    [[nodiscard]] bool release_srw_lock_shared(GuestAddr lock) const;
    [[nodiscard]] bool try_acquire_srw_lock_shared(GuestAddr lock) const;
    void wake_all_condition_variable(GuestAddr cv) const;
    [[nodiscard]] u32 sleep_condition_variable_srw(GuestAddr cv,
                                                   GuestAddr lock,
                                                   u32 ms,
                                                   u32 flags) const;

private:
    // Console output code page; 437 (OEM-US) is the fresh-console default.
    u32 m_console_output_cp{437};
    // Per calling thread. Absent means the user-default LCID, 0x0409.
    mutable std::mutex m_thread_locale_mu;
    mutable std::unordered_map<u32, u32> m_thread_locales;
    ConsoleAttachedQuery m_console_attached;

    [[nodiscard]] GuestAddr ensure_enum_scratch(Marshaler& m);

    SyncManager* m_sync_manager{nullptr};
    GuestCall m_guest_call;
};

/// Create locale/string/SRW thunks for kernel32
DllExports create_locale_string_thunks(LocaleStringManager& mgr);

} // namespace vitreo::winapi
