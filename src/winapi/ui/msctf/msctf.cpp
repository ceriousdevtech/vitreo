#include "msctf.hpp"

#include <atomic>
#include <cstdio>
#include <mutex>
#include <unordered_map>

#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/thread/thread.hpp"
#include "windowing/model/hwnd_manager.hpp"

namespace vitreo::winapi {

namespace {

// Learn inputscope.h: IS_ENUMSTRING = -5 … IS_PHRASELIST = -1, and
// IS_DEFAULT = 0 … IS_CHAT_WITHOUT_EMOJI = 68, with no gaps.
constexpr i32 kScopeMinSpecial = -5;
constexpr i32 kScopeMaxSpecial = -1;
constexpr i32 kScopeMinNamed = 0;
constexpr i32 kScopeMaxNamed = 68;
constexpr i32 kIsDefault = 0;

constexpr u32 kSOk = 0x00000000u;
constexpr u32 kEInvalidArg = 0x80070057u;
constexpr u32 kMaxItems = 1024;
constexpr usize kMaxWideChars = 4096;

struct Association {
    std::vector<i32> scopes;
    std::vector<std::string> phrases;
    std::string regexp;
    std::string srgs;
    std::string xml;
    u64 enum_string = 0;
};

std::mutex g_mu;
std::unordered_map<u32, Association> g_scopes;

void log_once(const char* what) {
    static std::atomic_flag reported = ATOMIC_FLAG_INIT;
    if (!reported.test_and_set()) {
        std::fprintf(stderr, "[vitreo] UNIMPLEMENTED msctf.dll!SetInputScope: %s\n",
                     what);
    }
}

[[nodiscard]] bool known_scope(i32 scope) {
    return (scope >= kScopeMinSpecial && scope <= kScopeMaxSpecial) ||
           (scope >= kScopeMinNamed && scope <= kScopeMaxNamed);
}

[[nodiscard]] bool same_thread(windowing::HwndManager* hwnd_mgr, u32 hwnd) {
    if (hwnd_mgr == nullptr) return false;
    const auto owner = hwnd_mgr->owner_thread_id(hwnd);
    if (!owner.has_value()) return false;
    return *owner == ThreadManager::get_current_thread_id_static();
}

// nullopt is an unreadable or unterminated string. A null pointer is the
// caller's problem and is not passed here.
[[nodiscard]] std::optional<std::string> read_pwstr(Marshaler& m, GuestAddr ptr) {
    auto text = m.read_wide_string(ptr, kMaxWideChars);
    if (text.is_err()) return std::nullopt;
    if (text.value().size() == kMaxWideChars) {
        const auto term = m.read_u16(ptr + kMaxWideChars * 2);
        if (term.is_err() || term.value() != 0) return std::nullopt;
    }
    return std::move(text.value());
}

[[nodiscard]] bool read_scopes(Marshaler& m, GuestAddr ptr, u32 count,
                               std::vector<i32>& out) {
    if (count > kMaxItems) {
        log_once("more than 1024 input scopes");
        return false;
    }
    out.clear();
    out.reserve(count);
    for (u32 i = 0; i < count; ++i) {
        const auto word = m.read_u32(ptr + static_cast<GuestAddr>(i) * 4);
        if (word.is_err()) return false;
        const i32 scope = static_cast<i32>(word.value());
        if (!known_scope(scope)) {
            log_once("input scope outside inputscope.h");
            return false;
        }
        out.push_back(scope);
    }
    return true;
}

[[nodiscard]] bool read_phrases(Marshaler& m, GuestAddr ptr, u32 count,
                                std::vector<std::string>& out) {
    if (count > kMaxItems) {
        log_once("more than 1024 input-scope phrases");
        return false;
    }
    out.clear();
    out.reserve(count);
    const GuestAddr step = m.cpu_is_64bit() ? 8 : 4;
    for (u32 i = 0; i < count; ++i) {
        const auto phrase_ptr = m.read_ptr(ptr + static_cast<GuestAddr>(i) * step);
        if (phrase_ptr.is_err() || phrase_ptr.value() == 0) return false;
        auto text = read_pwstr(m, phrase_ptr.value());
        if (!text.has_value()) return false;
        out.push_back(std::move(*text));
    }
    return true;
}

[[nodiscard]] bool read_optional_pwstr(Marshaler& m, GuestAddr ptr, std::string& out) {
    if (ptr == 0) {
        out.clear();
        return true;
    }
    auto text = read_pwstr(m, ptr);
    if (!text.has_value()) return false;
    out = std::move(*text);
    return true;
}

void store(u32 hwnd, Association association) {
    std::lock_guard lock(g_mu);
    g_scopes[hwnd] = std::move(association);
}

void clear_scope(u32 hwnd) {
    std::lock_guard lock(g_mu);
    g_scopes.erase(hwnd);
}

void impl_SetInputScope(Marshaler& m, windowing::HwndManager* hwnd_mgr) {
    const u32 hwnd = static_cast<u32>(m.arg(0));
    const i32 scope = static_cast<i32>(m.arg(1));
    // Learn: a window owned by another thread fails with E_INVALIDARG.
    // A hwnd that is not a window has no owning thread, so it fails the
    // same way. No GetLastError is documented.
    if (!same_thread(hwnd_mgr, hwnd) || !known_scope(scope)) {
        if (same_thread(hwnd_mgr, hwnd) && !known_scope(scope)) {
            log_once("input scope outside inputscope.h");
        }
        m.set_return(kEInvalidArg);
        return;
    }
    // Learn: IS_DEFAULT removes the association.
    if (scope == kIsDefault) {
        clear_scope(hwnd);
        m.set_return(kSOk);
        return;
    }
    Association next;
    next.scopes.push_back(scope);
    store(hwnd, std::move(next));
    m.set_return(kSOk);
}

void impl_SetInputScopes(Marshaler& m, windowing::HwndManager* hwnd_mgr) {
    const u32 hwnd = static_cast<u32>(m.arg(0));
    const GuestAddr scopes_ptr = m.arg_ptr(1);
    const u32 scope_count = static_cast<u32>(m.arg(2));
    const GuestAddr phrases_ptr = m.arg_ptr(3);
    const u32 phrase_count = static_cast<u32>(m.arg(4));
    const GuestAddr regexp_ptr = m.arg_ptr(5);
    const GuestAddr srgs_ptr = m.arg_ptr(6);
    if (!same_thread(hwnd_mgr, hwnd)) {
        m.set_return(kEInvalidArg);
        return;
    }
    // Learn: cInputScopes must be zero when pInputScopes is NULL.
    if ((scopes_ptr == 0 && scope_count != 0) ||
        (phrases_ptr == 0 && phrase_count != 0)) {
        m.set_return(kEInvalidArg);
        return;
    }
    Association next;
    if (scope_count != 0 &&
        !read_scopes(m, scopes_ptr, scope_count, next.scopes)) {
        m.set_return(kEInvalidArg);
        return;
    }
    if (phrase_count != 0 &&
        !read_phrases(m, phrases_ptr, phrase_count, next.phrases)) {
        m.set_return(kEInvalidArg);
        return;
    }
    if (!read_optional_pwstr(m, regexp_ptr, next.regexp) ||
        !read_optional_pwstr(m, srgs_ptr, next.srgs)) {
        m.set_return(kEInvalidArg);
        return;
    }
    // Learn: the call replaces whatever scope is associated with the window.
    store(hwnd, std::move(next));
    m.set_return(kSOk);
}

void impl_SetInputScopes2(Marshaler& m, windowing::HwndManager* hwnd_mgr) {
    const u32 hwnd = static_cast<u32>(m.arg(0));
    const GuestAddr scopes_ptr = m.arg_ptr(1);
    const u32 scope_count = static_cast<u32>(m.arg(2));
    const u64 enum_string = m.arg(3);
    const GuestAddr regexp_ptr = m.arg_ptr(4);
    const GuestAddr srgs_ptr = m.arg_ptr(5);
    if (!same_thread(hwnd_mgr, hwnd)) {
        m.set_return(kEInvalidArg);
        return;
    }
    // Learn: count must be zero when the array is NULL, and nonzero when
    // the array is non-NULL.
    if ((scopes_ptr == 0 && scope_count != 0) ||
        (scopes_ptr != 0 && scope_count == 0)) {
        m.set_return(kEInvalidArg);
        return;
    }
    Association next;
    next.enum_string = enum_string;
    if (scope_count != 0 &&
        !read_scopes(m, scopes_ptr, scope_count, next.scopes)) {
        m.set_return(kEInvalidArg);
        return;
    }
    if (!read_optional_pwstr(m, regexp_ptr, next.regexp) ||
        !read_optional_pwstr(m, srgs_ptr, next.srgs)) {
        m.set_return(kEInvalidArg);
        return;
    }
    store(hwnd, std::move(next));
    m.set_return(kSOk);
}

void impl_SetInputScopeXML(Marshaler& m, windowing::HwndManager* hwnd_mgr) {
    const u32 hwnd = static_cast<u32>(m.arg(0));
    const GuestAddr xml_ptr = m.arg_ptr(1);
    // Learn marks SetInputScopeXML "Do not use" and does not document a
    // failure HRESULT. The signature is still HRESULT, and the sibling
    // functions fail with E_INVALIDARG when the window is not owned by
    // the calling thread.
    if (!same_thread(hwnd_mgr, hwnd)) {
        m.set_return(kEInvalidArg);
        return;
    }
    std::string xml;
    if (!read_optional_pwstr(m, xml_ptr, xml)) {
        m.set_return(kEInvalidArg);
        return;
    }
    Association next;
    {
        std::lock_guard lock(g_mu);
        const auto it = g_scopes.find(hwnd);
        if (it != g_scopes.end()) next = it->second;
    }
    next.xml = std::move(xml);
    store(hwnd, std::move(next));
    m.set_return(kSOk);
}

} // namespace

std::optional<StoredInputScope> stored_input_scope(u32 hwnd) {
    std::lock_guard lock(g_mu);
    const auto it = g_scopes.find(hwnd);
    if (it == g_scopes.end()) return std::nullopt;
    StoredInputScope out;
    out.scopes = it->second.scopes;
    out.phrases = it->second.phrases;
    out.regexp = it->second.regexp;
    out.srgs = it->second.srgs;
    out.xml = it->second.xml;
    out.enum_string = it->second.enum_string;
    return out;
}

DllExports create_msctf_thunks(windowing::HwndManager* hwnd_mgr) {
    DllExports exports;
    exports.entries.reserve(4);

    // HRESULT SetInputScope(HWND, InputScope) — stdcall 8.
    exports.entries.push_back(ExportEntry{
        "SetInputScope", 0,
        [hwnd_mgr](Marshaler& m) { impl_SetInputScope(m, hwnd_mgr); },
        8
    });
    // HRESULT SetInputScopes(HWND, const InputScope*, UINT, PWSTR*, UINT,
    //                        PWSTR, PWSTR) — stdcall 28.
    exports.entries.push_back(ExportEntry{
        "SetInputScopes", 0,
        [hwnd_mgr](Marshaler& m) { impl_SetInputScopes(m, hwnd_mgr); },
        28
    });
    // HRESULT SetInputScopes2(HWND, const InputScope*, UINT, IEnumString*,
    //                         PWSTR, PWSTR) — stdcall 24.
    exports.entries.push_back(ExportEntry{
        "SetInputScopes2", 0,
        [hwnd_mgr](Marshaler& m) { impl_SetInputScopes2(m, hwnd_mgr); },
        24
    });
    // HRESULT SetInputScopeXML(HWND, PWSTR) — stdcall 8.
    exports.entries.push_back(ExportEntry{
        "SetInputScopeXML", 0,
        [hwnd_mgr](Marshaler& m) { impl_SetInputScopeXML(m, hwnd_mgr); },
        8
    });
    return exports;
}

} // namespace vitreo::winapi
