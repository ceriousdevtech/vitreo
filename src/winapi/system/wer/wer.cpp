// SPDX-License-Identifier: LGPL-2.1-only
/// @file wer.cpp
/// @brief wer.dll — Windows Error Reporting (see wer.hpp).

#include "winapi/system/wer/wer.hpp"

#include "capsule/registry/registry.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/kernel32/locale/string_conv.hpp"

#include <cstdio>
#include <vector>
#include <optional>
#include <algorithm>
#include <cwctype>
#include <mutex>
#include <set>
#include <string_view>
#include <unordered_map>

namespace vitreo::winapi {

namespace {

struct ExportRow {
    const char* name;
    u16 ordinal;
    u16 stdcall_bytes;
};

constexpr ExportRow kExports[] = {
#define WER_EXPORT(name, ord, bytes) {#name, ord, bytes},
#include "winapi/system/wer/wer_export_table.inc"
#undef WER_EXPORT
};

constexpr u32 E_NOTIMPL_ = 0x80004001u;
constexpr u32 ERROR_CALL_NOT_IMPLEMENTED_ = 120;

/// Learn: WerAddExcludedApplication remarks. The list is per user under HKCU
/// or for all users under HKLM; probe wer_exclusion (32 and 64-bit) shows
/// the key, a REG_DWORD 1 per excluded file name, and that a 32-bit caller
/// writes HKLM's 64-bit view (the capsule registry's HKLM\Software).
constexpr std::wstring_view kExcludedKey =
    L"Software\\Microsoft\\Windows\\Windows Error Reporting\\ExcludedApplications";

void log_unimplemented(std::string_view api, std::string_view what) {
    static std::mutex mutex;
    static std::set<std::string> seen;
    std::string key = std::string(api) + ":" + std::string(what);
    std::lock_guard<std::mutex> lock(mutex);
    if (!seen.insert(key).second) return;
    std::fprintf(stderr, "[vitreo] UNIMPLEMENTED wer!%.*s: %.*s\n", static_cast<int>(api.size()), api.data(),
                 static_cast<int>(what.size()), what.data());
}

/// A guest PCWSTR as a wide string (surrogate pairs combined); nullopt for
/// NULL or an unreadable pointer.
std::optional<std::wstring> read_wide(Marshaler& m, GuestAddr p) {
    if (p == 0) return std::nullopt;
    std::wstring out;
    for (u32 i = 0;; ++i) {
        auto unit = m.read_u16(p + i * 2u);
        if (unit.is_err()) return std::nullopt;
        u32 c = unit.value();
        if (c == 0) break;
        if (c >= 0xD800 && c < 0xDC00) {
            auto low = m.read_u16(p + (i + 1) * 2u);
            if (low.is_ok() && low.value() >= 0xDC00 && low.value() < 0xE000) {
                c = 0x10000u + ((c - 0xD800u) << 10) + (low.value() - 0xDC00u);
                ++i;
            }
        }
        out.push_back(static_cast<wchar_t>(c));
    }
    return out;
}

bool iequal(std::wstring_view a, std::wstring_view b) {
    if (a.size() != b.size()) return false;
    for (usize i = 0; i < a.size(); ++i)
        if (std::towupper(static_cast<wint_t>(a[i])) != std::towupper(static_cast<wint_t>(b[i]))) return false;
    return true;
}

/// The value name the list keys on: the file-name part (probe:
/// "C:\\Dir\\vitreo_path.exe" is stored as "vitreo_path.exe").
std::wstring file_part(const std::wstring& name) {
    const usize slash = name.find_last_of(L'\\');
    return slash == std::wstring::npos ? name : name.substr(slash + 1);
}

u32 hresult_for(Error e) {
    return e == Error::AccessDenied ? wer::E_ACCESSDENIED_ : wer::E_FAIL_;
}

/// The return the documented non-HRESULT exports fail with; HRESULT exports
/// fail with E_NOTIMPL.
enum class FailKind { Hresult, NullWithError, VoidCall };

FailKind fail_kind(std::string_view name) {
    // wct.h: OpenThreadWaitChainSession returns HWCT (NULL on failure,
    // GetLastError); GetThreadWaitChain returns BOOL. CloseThreadWaitChainSession,
    // RegisterWaitChainCOMCallback and WerFreeString return VOID.
    if (name == "OpenThreadWaitChainSession" || name == "GetThreadWaitChain") return FailKind::NullWithError;
    if (name == "CloseThreadWaitChainSession" || name == "RegisterWaitChainCOMCallback" ||
        name == "WerFreeString")
        return FailKind::VoidCall;
    return FailKind::Hresult;
}

}  // namespace

namespace wer {

u32 add_excluded_application(capsule::Registry& registry, const std::wstring& exe_name, bool all_users) {
    if (exe_name.empty()) return E_INVALIDARG_;
    const std::wstring value = file_part(exe_name);
    if (value.empty()) return E_INVALIDARG_;
    auto key = registry.open_key(all_users ? capsule::HKEY_LOCAL_MACHINE : capsule::HKEY_CURRENT_USER,
                                 kExcludedKey, true);
    if (key.is_err()) return hresult_for(key.error());
    // A name already listed in another case keeps its stored spelling
    // (probe: adding VITREO_WER_PROBE.EXE after vitreo_wer_probe.exe).
    std::wstring stored = value;
    if (auto names = registry.enum_values(key.value()); names.is_ok()) {
        for (const std::wstring& n : names.value())
            if (iequal(n, value)) stored = n;
    }
    capsule::RegValue one;
    one.type = capsule::RegType::Dword;
    one.data = u32{1};
    auto set = registry.set_value(key.value(), stored, one);
    registry.close_key(key.value());
    return set.is_err() ? hresult_for(set.error()) : S_OK_;
}

u32 remove_excluded_application(capsule::Registry& registry, const std::wstring& exe_name, bool all_users) {
    if (exe_name.empty()) return E_INVALIDARG_;
    const std::wstring value = file_part(exe_name);
    if (value.empty()) return E_INVALIDARG_;
    auto key = registry.open_key(all_users ? capsule::HKEY_LOCAL_MACHINE : capsule::HKEY_CURRENT_USER,
                                 kExcludedKey, false);
    if (key.is_err()) return key.error() == Error::AccessDenied ? E_ACCESSDENIED_ : E_FAIL_;
    std::optional<std::wstring> stored;
    if (auto names = registry.enum_values(key.value()); names.is_ok()) {
        for (const std::wstring& n : names.value())
            if (iequal(n, value)) stored = n;
    }
    u32 hr = E_FAIL_;  // probe: removing a name that is not listed
    if (stored) {
        auto del = registry.delete_value(key.value(), *stored);
        hr = del.is_err() ? hresult_for(del.error()) : S_OK_;
    }
    registry.close_key(key.value());
    return hr;
}

}  // namespace wer

std::vector<std::string> wer_export_names() {
    std::vector<std::string> names;
    for (const ExportRow& row : kExports) names.emplace_back(row.name);
    return names;
}

DllExports create_wer_thunks(capsule::Registry& registry) {
    using Handler = std::function<void(Marshaler&)>;
    std::unordered_map<std::string, Handler> handlers;

    // HRESULT WerAddExcludedApplication(PCWSTR pwzExeName, BOOL bAllUsers)
    handlers["WerAddExcludedApplication"] = [&registry](Marshaler& m) {
        const auto name = read_wide(m, static_cast<GuestAddr>(m.arg(0)));
        const bool all = static_cast<u32>(m.arg(1)) != 0;
        m.set_return(name ? wer::add_excluded_application(registry, *name, all) : wer::E_INVALIDARG_);
    };
    // HRESULT WerRemoveExcludedApplication(PCWSTR pwzExeName, BOOL bAllUsers)
    handlers["WerRemoveExcludedApplication"] = [&registry](Marshaler& m) {
        const auto name = read_wide(m, static_cast<GuestAddr>(m.arg(0)));
        const bool all = static_cast<u32>(m.arg(1)) != 0;
        m.set_return(name ? wer::remove_excluded_application(registry, *name, all) : wer::E_INVALIDARG_);
    };

    DllExports exports;
    exports.entries.reserve(std::size(kExports));
    for (const ExportRow& row : kExports) {
        Handler fn;
        if (auto it = handlers.find(row.name); it != handlers.end()) {
            fn = std::move(it->second);
        } else {
            const std::string name = row.name;
            fn = [name](Marshaler& m) {
                switch (fail_kind(name)) {
                    case FailKind::NullWithError:
                        log_unimplemented(name, "wait chain traversal is not implemented");
                        set_last_error(ERROR_CALL_NOT_IMPLEMENTED_);
                        m.set_return(0);
                        break;
                    case FailKind::VoidCall:
                        log_unimplemented(name, "export has no implementation");
                        m.set_return(0);
                        break;
                    case FailKind::Hresult:
                        log_unimplemented(name, "error reporting is not implemented");
                        m.set_return(E_NOTIMPL_);
                        break;
                }
            };
        }
        exports.entries.push_back(ExportEntry{row.name, row.ordinal, std::move(fn), row.stdcall_bytes});
    }
    return exports;
}

DllExports create_faultrep_thunks(capsule::Registry& registry) {
    // EFaultRepRetVal (errorrep.h).
    constexpr u32 frrvErrNoDW = 4;
    DllExports exports;

    // EFaultRepRetVal ReportFault(LPEXCEPTION_POINTERS pep, DWORD dwOpt)
    // https://learn.microsoft.com/en-us/windows/win32/api/errorrep/nf-errorrep-reportfault
    // The capsule has no error reporting client, which Learn reports as
    // frrvErrNoDW: "The error reporting client was unable to launch. The
    // system will perform its default actions."
    exports.entries.push_back(ExportEntry{"ReportFault", 0, [](Marshaler& m) {
        (void)m.arg(0);
        (void)m.arg(1);
        m.set_return(frrvErrNoDW);
    }, 8});

    // BOOL AddERExcludedApplication{A,W}(LPCTSTR szApplication)
    // https://learn.microsoft.com/en-us/windows/win32/api/errorrep/nf-errorrep-adderexcludedapplicationw
    // "stores the excluded application list under the HKEY_LOCAL_MACHINE
    // hive": the all-users list WerAddExcludedApplication(..., TRUE) keeps.
    // A failing HRESULT becomes FALSE with its Win32 code as the last error.
    auto add_excluded = [&registry](Marshaler& m, std::optional<std::wstring> name) {
        const u32 hr = name ? wer::add_excluded_application(registry, *name, true)
                            : wer::E_INVALIDARG_;
        if ((hr & 0x80000000u) != 0) {
            set_last_error(hr & 0xFFFFu);
            m.set_return(0);
            return;
        }
        m.set_return(1);
    };
    exports.entries.push_back(ExportEntry{"AddERExcludedApplicationW", 0,
        [add_excluded](Marshaler& m) {
            add_excluded(m, read_wide(m, static_cast<GuestAddr>(m.arg(0))));
        }, 4});
    exports.entries.push_back(ExportEntry{"AddERExcludedApplicationA", 0,
        [add_excluded](Marshaler& m) {
            const GuestAddr p = static_cast<GuestAddr>(m.arg(0));
            std::optional<std::wstring> name;
            if (p != 0) {
                if (auto text = m.read_ansi_string(p); text.is_ok()) {
                    // The ANSI entry point takes CP_ACP text.
                    const std::string& bytes = text.value();
                    std::vector<u16> units(bytes.size() + 1, 0);
                    const int n = StringConverter::multi_byte_to_wide_char(
                        0 /* CP_ACP */, 0, bytes.data(), static_cast<int>(bytes.size()),
                        units.data(), static_cast<int>(units.size()));
                    name = std::wstring(units.begin(), units.begin() + std::max(n, 0));
                }
            }
            add_excluded(m, std::move(name));
        }, 4});
    return exports;
}

}  // namespace vitreo::winapi
