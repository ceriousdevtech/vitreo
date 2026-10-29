#pragma once

/// @file syslink_markup.hpp
/// @brief SysLink (WC_LINK) markup parsing and constants, shared by user32
/// (which owns the link items) and the host renderer.
///
/// Constants: Windows SDK 10.0.26100.0 um/CommCtrl.h ("SysLink control").
/// Parsing rules are measured, not documented beyond "<a> with HREF and ID":
/// tools/oracle_probe/syslink_probe.c, syslink_probe{32,64}.expected.

#include <vitreo/common.hpp>

#include <cwctype>
#include <string>
#include <string_view>
#include <vector>

namespace vitreo::windowing::syslink {

// CommCtrl.h
inline constexpr u32 kMaxLinkIdText = 48;                    // MAX_LINKID_TEXT
inline constexpr u32 kMaxUrlLength = 2048 + 32 + sizeof("://");  // L_MAX_URL_LENGTH = 2084

inline constexpr u32 LWS_TRANSPARENT = 0x0001;
inline constexpr u32 LWS_IGNORERETURN = 0x0002;
inline constexpr u32 LWS_NOPREFIX = 0x0004;
inline constexpr u32 LWS_USEVISUALSTYLE = 0x0008;
inline constexpr u32 LWS_USECUSTOMTEXT = 0x0010;
inline constexpr u32 LWS_RIGHT = 0x0020;

inline constexpr u32 LIF_ITEMINDEX = 0x00000001;
inline constexpr u32 LIF_STATE = 0x00000002;
inline constexpr u32 LIF_ITEMID = 0x00000004;
inline constexpr u32 LIF_URL = 0x00000008;

inline constexpr u32 LIS_FOCUSED = 0x00000001;
inline constexpr u32 LIS_ENABLED = 0x00000002;
inline constexpr u32 LIS_VISITED = 0x00000004;
inline constexpr u32 LIS_HOTTRACK = 0x00000008;
inline constexpr u32 LIS_DEFAULTCOLORS = 0x00000010;
/// Every LIS_* bit; LM_SETITEM with any other stateMask bit fails
/// (syslink_probe statebit5..7, statemask_undefined_clear).
inline constexpr u32 kAllStates = 0x0000001F;

inline constexpr u32 WM_USER_BASE = 0x0400;
inline constexpr u32 LM_HITTEST = WM_USER_BASE + 0x300;
inline constexpr u32 LM_GETIDEALHEIGHT = WM_USER_BASE + 0x301;
inline constexpr u32 LM_SETITEM = WM_USER_BASE + 0x302;
inline constexpr u32 LM_GETITEM = WM_USER_BASE + 0x303;
inline constexpr u32 LM_GETIDEALSIZE = LM_GETIDEALHEIGHT;

// LITEM: UINT mask; int iLink; UINT state; UINT stateMask;
//        WCHAR szID[MAX_LINKID_TEXT]; WCHAR szUrl[L_MAX_URL_LENGTH].
// No pointers, so the layout is the same for x86 and x64 (sizeof = 4280 on
// both, syslink_probe line 1).
inline constexpr u32 kLitemMask = 0;
inline constexpr u32 kLitemLink = 4;
inline constexpr u32 kLitemState = 8;
inline constexpr u32 kLitemStateMask = 12;
inline constexpr u32 kLitemId = 16;
inline constexpr u32 kLitemUrl = kLitemId + kMaxLinkIdText * 2;
inline constexpr u32 kLitemSize = kLitemUrl + kMaxUrlLength * 2;
static_assert(kLitemSize == 4280);
/// LHITTESTINFO: POINT pt; LITEM item.
inline constexpr u32 kHitTestItem = 8;
/// NMLINK: NMHDR hdr; LITEM item. NMHDR is 12 bytes (x86) / 24 bytes (x64):
/// sizeof(NMLINK) = 4292 / 4304 (syslink_probe line 1).
[[nodiscard]] constexpr u32 nmlink_item_offset(bool is_64bit) {
    return is_64bit ? 24u : 12u;
}

struct MarkupItem {
    std::wstring id;   ///< At most kMaxLinkIdText - 1 characters.
    std::wstring url;  ///< At most kMaxUrlLength - 1 characters.
    u32 start = 0;     ///< Offset of the link text in Markup::display.
    u32 length = 0;
};

struct Markup {
    std::wstring display;
    std::vector<MarkupItem> items;
    /// Display index of the prefix-underlined character, or -1.
    i32 mnemonic = -1;
};

namespace detail {

[[nodiscard]] inline bool ieq(wchar_t a, wchar_t b) {
    return std::towlower(static_cast<wint_t>(a)) == std::towlower(static_cast<wint_t>(b));
}

[[nodiscard]] inline bool starts_with_ci(std::wstring_view s, usize at, std::wstring_view word) {
    if (at + word.size() > s.size()) return false;
    for (usize i = 0; i < word.size(); ++i) {
        if (!ieq(s[at + i], word[i])) return false;
    }
    return true;
}

[[nodiscard]] inline bool is_space(wchar_t c) {
    return c == L' ' || c == L'\t' || c == L'\r' || c == L'\n';
}

[[nodiscard]] inline usize find_ci(std::wstring_view s, usize from, std::wstring_view word) {
    for (usize i = from; i + word.size() <= s.size(); ++i) {
        if (starts_with_ci(s, i, word)) return i;
    }
    return std::wstring_view::npos;
}

/// Parses `<a attr="v" ...>` starting at @p at ('<'). On success returns the
/// index just past '>' and fills id/url. Measured: values must be
/// double-quoted, '=' must touch both name and quote, unknown attributes are
/// ignored, names are case-insensitive (cases 2, 3, 7, 12, 1).
[[nodiscard]] inline usize parse_open_tag(std::wstring_view s, usize at,
                                          std::wstring& id, std::wstring& url) {
    usize i = at + 2;  // past "<a"
    if (i >= s.size() || !(s[i] == L'>' || is_space(s[i]))) return 0;
    for (;;) {
        while (i < s.size() && is_space(s[i])) ++i;
        if (i >= s.size()) return 0;
        if (s[i] == L'>') return i + 1;
        const usize name_begin = i;
        while (i < s.size() && std::iswalpha(static_cast<wint_t>(s[i]))) ++i;
        if (i == name_begin || i >= s.size() || s[i] != L'=') return 0;
        const std::wstring_view name = s.substr(name_begin, i - name_begin);
        ++i;
        if (i >= s.size() || s[i] != L'"') return 0;
        const usize value_begin = ++i;
        while (i < s.size() && s[i] != L'"') ++i;
        if (i >= s.size()) return 0;
        const std::wstring_view value = s.substr(value_begin, i - value_begin);
        ++i;
        if (i >= s.size() || !(s[i] == L'>' || is_space(s[i]))) return 0;
        if (starts_with_ci(name, 0, L"href") && name.size() == 4) {
            url.assign(value.substr(0, kMaxUrlLength - 1));
        } else if (starts_with_ci(name, 0, L"id") && name.size() == 2) {
            id.assign(value.substr(0, kMaxLinkIdText - 1));
        }
    }
}

}  // namespace detail

/// Splits SysLink markup into display text and link items. An `<a>` with no
/// matching `</a>`, or with an attribute that does not parse, is shown as
/// literal text and is not a link (cases 5, 2, 3, 12). Entities are not
/// decoded (case 13). With @p process_prefix (no LWS_NOPREFIX), '&' marks the
/// next character as the mnemonic and "&&" is a literal '&' (Learn:
/// LWS_NOPREFIX).
[[nodiscard]] inline Markup parse_markup(std::wstring_view s, bool process_prefix) {
    struct RawItem { MarkupItem item; usize begin; usize end; };
    std::wstring raw;
    std::vector<RawItem> raw_items;
    usize i = 0;
    while (i < s.size()) {
        if (s[i] == L'<' && i + 1 < s.size() && detail::ieq(s[i + 1], L'a')) {
            std::wstring id;
            std::wstring url;
            const usize body = detail::parse_open_tag(s, i, id, url);
            if (body != 0) {
                const usize close = detail::find_ci(s, body, L"</a>");
                if (close != std::wstring_view::npos) {
                    RawItem r;
                    r.item.id = std::move(id);
                    r.item.url = std::move(url);
                    r.begin = raw.size();
                    raw.append(s.substr(body, close - body));
                    r.end = raw.size();
                    raw_items.push_back(std::move(r));
                    i = close + 4;
                    continue;
                }
            }
        }
        raw.push_back(s[i]);
        ++i;
    }

    Markup out;
    std::vector<u32> map(raw.size() + 1, 0);  // raw index -> display index
    for (usize k = 0; k < raw.size(); ++k) {
        map[k] = static_cast<u32>(out.display.size());
        if (process_prefix && raw[k] == L'&' && k + 1 < raw.size()) {
            ++k;
            map[k] = static_cast<u32>(out.display.size());
            if (raw[k] != L'&' && out.mnemonic < 0) {
                out.mnemonic = static_cast<i32>(out.display.size());
            }
        }
        out.display.push_back(raw[k]);
    }
    map[raw.size()] = static_cast<u32>(out.display.size());
    for (auto& r : raw_items) {
        r.item.start = map[r.begin];
        r.item.length = map[r.end] - map[r.begin];
        out.items.push_back(std::move(r.item));
    }
    return out;
}

}  // namespace vitreo::windowing::syslink
