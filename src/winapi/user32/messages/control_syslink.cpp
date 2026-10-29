// SPDX-License-Identifier: LGPL-2.1-only
/// @file control_syslink.cpp
/// @brief SysLink (WC_LINK) class procedure.
///
/// Sources: Learn "About SysLink Controls", LITEM, NMLINK; SDK CommCtrl.h for
/// constants and layouts (windowing/contracts/syslink_markup.hpp). Everything
/// Learn leaves unstated is measured on Windows:
/// tools/oracle_probe/syslink_probe.c -> syslink_probe{32,64}.expected.
/// The control owns the markup, link items and LIS_* state; Cocoa only
/// renders them and answers layout queries.

#include "winapi/user32/messages/message_send.hpp"
#include "winapi/user32/user32_internal.hpp"
#include "winapi/user32/ansi_text.hpp"
#include "winapi/core/marshaler.hpp"
#include "core/memory/guest_memory.hpp"

#include <algorithm>
#include <cwctype>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace vitreo::winapi {

namespace sl = windowing::syslink;

namespace {

constexpr u32 kWmSetText = 0x000C;
constexpr u32 kWmGetText = 0x000D;
constexpr u32 kWmGetTextLength = 0x000E;
constexpr u32 kWmNotify = 0x004E;
constexpr u32 kWmGetDlgCode = 0x0087;
constexpr u32 kWmKeyDown = 0x0100;
constexpr u32 kWmLButtonDown = 0x0201;
constexpr u32 kWmLButtonUp = 0x0202;
constexpr u32 kVkTab = 0x09;
constexpr u32 kVkReturn = 0x0D;
constexpr u32 kVkShift = 0x10;
constexpr u32 kVkSpace = 0x20;
constexpr u32 kDlgcStatic = 0x0100;           // WM_GETDLGCODE (probe: 0x100)
constexpr u32 kNmClick = static_cast<u32>(-2);  // NM_CLICK
constexpr u32 kNmReturn = static_cast<u32>(-4); // NM_RETURN

[[nodiscard]] bool ieq(std::wstring_view a, std::wstring_view b) {
    if (a.size() != b.size()) return false;
    for (usize i = 0; i < a.size(); ++i) {
        if (std::towlower(static_cast<wint_t>(a[i])) != std::towlower(static_cast<wint_t>(b[i]))) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool is_syslink(const windowing::WindowData& win) {
    return ieq(win.class_name, L"SysLink") || ieq(win.native_control_class, L"SysLink");
}

/// Reads at most @p max_chars UTF-16 units, stopping at NUL.
[[nodiscard]] std::wstring read_wchars(Marshaler& m, GuestAddr at, u32 max_chars) {
    std::wstring out;
    for (u32 i = 0; i < max_chars; ++i) {
        auto c = m.read_u16(at + i * 2);
        if (!c.is_ok() || c.value() == 0) break;
        out.push_back(static_cast<wchar_t>(c.value()));
    }
    return out;
}

/// Writes @p text NUL-terminated into a WCHAR[@p capacity] field.
void write_wchars(Marshaler& m, GuestAddr at, const std::wstring& text, u32 capacity) {
    const u32 n = std::min(static_cast<u32>(text.size()), capacity - 1);
    for (u32 i = 0; i < n; ++i) (void)m.write_u16(at + i * 2, static_cast<u16>(text[i]));
    (void)m.write_u16(at + n * 2, 0);
}

[[nodiscard]] std::optional<std::wstring> read_text_arg(Marshaler& m, GuestAddr at, bool ansi) {
    if (ansi) {
        std::string bytes;
        for (u32 off = 0; off < 0x10000; ++off) {
            auto c = m.read_u8(at + off);
            if (!c.is_ok()) return std::nullopt;
            if (c.value() == 0) break;
            bytes.push_back(static_cast<char>(c.value()));
        }
        return ansi_text::to_wide(bytes);
    }
    auto s = m.read_wide_string(at);
    if (!s.is_ok()) return std::nullopt;
    return utf8_to_wstring(s.value());
}

/// One page-rounded scratch block for NMLINK (4292 / 4304 bytes). Sends are
/// synchronous and consumed before the next notification.
GuestAddr notify_scratch(Marshaler& m) {
    constexpr u32 kSize = 0x2000;
    static std::mutex mutex;
    static const GuestMemory* owner = nullptr;
    static GuestAddr s = 0;
    std::lock_guard<std::mutex> lock(mutex);
    GuestMemory& mem = m.memory();
    if (owner != &mem || s == 0 || !mem.is_mapped(s, kSize)) {
        s = 0;
        owner = &mem;
        const GuestAddr base = mem.find_free_gap(static_cast<GuestAddr>(0x3F000000), kSize);
        if (base != 0 && mem.map_region(base, kSize, MemoryProtection::ReadWrite).is_ok()) {
            s = base;
        }
    }
    return s;
}

[[nodiscard]] i32 first_enabled(const std::vector<u32>& states, i32 from, i32 step) {
    for (i32 i = from; i >= 0 && i < static_cast<i32>(states.size()); i += step) {
        if (states[static_cast<usize>(i)] & sl::LIS_ENABLED) return i;
    }
    return -1;
}

[[nodiscard]] i32 focused_item(const std::vector<u32>& states) {
    for (usize i = 0; i < states.size(); ++i) {
        if (states[i] & sl::LIS_FOCUSED) return static_cast<i32>(i);
    }
    return -1;
}

/// LIS_FOCUSED is exclusive across items (probe afterfocus0).
void set_focus_item(std::vector<u32>& states, i32 index) {
    for (usize i = 0; i < states.size(); ++i) {
        if (static_cast<i32>(i) == index) states[i] |= sl::LIS_FOCUSED;
        else states[i] &= ~sl::LIS_FOCUSED;
    }
}

}  // namespace

void MessageSendManager::control_destroyed(windowing::HWND hwnd) {
    std::lock_guard<std::mutex> lock(m_control_state_mutex);
    m_button_check_state.erase(hwnd);
    m_static_image_state.erase(hwnd);
    m_progress_state.erase(hwnd);
    m_rebar_state.erase(hwnd);
    m_tooltip_state.erase(hwnd);
    m_tab_extra.erase(hwnd);
    m_toolbar_state.erase(hwnd);
    m_syslink_state.erase(hwnd);
    m_edit_state.erase(hwnd);
}

bool MessageSendManager::dispatch_syslink_control(Marshaler& m, windowing::HWND hwnd,
                                                  u32 msg, u64 wParam, u64 lParam,
                                                  bool ansi, bool force_native) {
    windowing::WindowData* win = m_hwnd_mgr.find(hwnd);
    if (win == nullptr || !is_syslink(*win)) return false;
    if (!force_native && win->wndproc != 0) return false;
    switch (msg) {
    case kWmSetText: case kWmGetText: case kWmGetTextLength:
    case sl::LM_HITTEST: case sl::LM_GETIDEALHEIGHT: case sl::LM_SETITEM: case sl::LM_GETITEM:
    case kWmGetDlgCode: case kWmKeyDown: case kWmLButtonDown: case kWmLButtonUp:
        break;
    default:
        return false;
    }

    const u32 style = win->style;
    const bool prefix = (style & sl::LWS_NOPREFIX) == 0;
    const windowing::HWND parent = win->parent;
    const u32 ctrl_id = win->ctrl_id;
    const std::wstring initial_text = win->title;

    // Snapshot (or create) the control's state. The table lock is never held
    // across bridge or guest calls.
    SysLinkState st;
    {
        std::lock_guard<std::mutex> lock(m_control_state_mutex);
        auto it = m_syslink_state.find(hwnd);
        if (it == m_syslink_state.end()) {
            SysLinkState fresh;
            fresh.markup = initial_text;
            fresh.parsed = sl::parse_markup(fresh.markup, prefix);
            fresh.states.assign(fresh.parsed.items.size(), sl::LIS_ENABLED);
            it = m_syslink_state.emplace(hwnd, std::move(fresh)).first;
        }
        st = it->second;
    }
    bool changed = false;   // items/states to store and re-render
    const i32 count = static_cast<i32>(st.parsed.items.size());

    auto commit = [&] {
        {
            std::lock_guard<std::mutex> lock(m_control_state_mutex);
            m_syslink_state[hwnd] = st;
        }
        if (changed) m_bridge.syslink_set_content(hwnd, st.parsed, st.states, style);
    };
    auto notify = [&](u32 code, i32 index) {
        if (parent == windowing::NULL_HWND || index < 0 || index >= count) return;
        const GuestAddr nm = notify_scratch(m);
        if (nm == 0) return;
        const bool is64 = m.cpu_is_64bit();
        // NMHDR: hwndFrom, idFrom (UINT_PTR), code.
        if (is64) {
            (void)m.write_u64(nm + 0, hwnd);
            (void)m.write_u64(nm + 8, ctrl_id);
            (void)m.write_u32(nm + 16, code);
            (void)m.write_u32(nm + 20, 0);
        } else {
            (void)m.write_u32(nm + 0, hwnd);
            (void)m.write_u32(nm + 4, ctrl_id);
            (void)m.write_u32(nm + 8, code);
        }
        // The notification LITEM carries iLink, szID and szUrl; mask, state
        // and stateMask are zero (probe "click" line).
        const GuestAddr item = nm + sl::nmlink_item_offset(is64);
        (void)m.write_u32(item + sl::kLitemMask, 0);
        (void)m.write_u32(item + sl::kLitemLink, static_cast<u32>(index));
        (void)m.write_u32(item + sl::kLitemState, 0);
        (void)m.write_u32(item + sl::kLitemStateMask, 0);
        const auto& it = st.parsed.items[static_cast<usize>(index)];
        write_wchars(m, item + sl::kLitemId, it.id, sl::kMaxLinkIdText);
        write_wchars(m, item + sl::kLitemUrl, it.url, sl::kMaxUrlLength);
        // wParam is the control ID (probe "notify ... wparam=7").
        (void)send_message(parent, kWmNotify, ctrl_id, nm);
    };

    switch (msg) {
    case kWmSetText: {
        // NULL clears the text and keeps the link items (probe settextnull);
        // the same markup again changes nothing (probe resettext); any other
        // text re-parses from scratch (probe fewer/more/none/againone).
        if (lParam == 0) {
            st.markup.clear();
        } else {
            auto text = read_text_arg(m, static_cast<GuestAddr>(lParam), ansi);
            if (!text) { m.set_return(0u); return true; }
            if (*text != st.markup) {
                st.markup = std::move(*text);
                st.parsed = sl::parse_markup(st.markup, prefix);
                st.states.assign(st.parsed.items.size(), sl::LIS_ENABLED);
                st.pressed = -1;
                changed = true;
            }
        }
        if (auto* w = m_hwnd_mgr.find(hwnd)) w->title = st.markup;
        commit();
        m.set_return(1u);
        return true;
    }
    case kWmGetTextLength: {
        // W: characters. A: bytes plus one for non-empty text
        // (probe case0 "A gettextlength=48" for 47 characters).
        if (ansi) {
            const usize bytes = ansi_text::to_ansi(st.markup).size();
            m.set_return(bytes == 0 ? 0u : static_cast<u64>(bytes + 1));
        } else {
            m.set_return(static_cast<u64>(st.markup.size()));
        }
        return true;
    }
    case kWmGetText: {
        // Fits: the markup and NUL, returning the count including the NUL.
        // Too small: returns 0; W writes the truncated text and NUL, A only a
        // NUL (probe trunc/truncA). cch == 0 writes nothing.
        const u32 cch = static_cast<u32>(wParam);
        const GuestAddr buf = static_cast<GuestAddr>(lParam);
        if (cch == 0 || buf == 0) { m.set_return(0u); return true; }
        if (ansi) {
            const std::string bytes = ansi_text::to_ansi(st.markup);
            if (bytes.size() + 1 <= cch) {
                for (usize i = 0; i < bytes.size(); ++i) {
                    (void)m.write_u8(buf + static_cast<GuestAddr>(i), static_cast<u8>(bytes[i]));
                }
                (void)m.write_u8(buf + static_cast<GuestAddr>(bytes.size()), 0);
                m.set_return(static_cast<u64>(bytes.size() + 1));
            } else {
                (void)m.write_u8(buf, 0);
                m.set_return(0u);
            }
            return true;
        }
        if (st.markup.size() + 1 <= cch) {
            write_wchars(m, buf, st.markup, static_cast<u32>(st.markup.size() + 1));
            m.set_return(static_cast<u64>(st.markup.size() + 1));
        } else {
            write_wchars(m, buf, st.markup, cch);
            m.set_return(0u);
        }
        return true;
    }
    case sl::LM_GETITEM:
    case sl::LM_SETITEM: {
        // LIF_ITEMINDEX and a valid iLink are required, plus at least one of
        // LIF_STATE / LIF_ITEMID / LIF_URL. Lookup by ID alone fails. Failure
        // leaves the last error untouched (probe getitem_*, setitem_*).
        const GuestAddr p = static_cast<GuestAddr>(lParam);
        if (p == 0) { m.set_return(0u); return true; }
        const u32 mask = m.read_u32(p + sl::kLitemMask).value_or(0);
        const i32 index = static_cast<i32>(m.read_u32(p + sl::kLitemLink).value_or(0xFFFFFFFFu));
        constexpr u32 kData = sl::LIF_STATE | sl::LIF_ITEMID | sl::LIF_URL;
        if (!(mask & sl::LIF_ITEMINDEX) || !(mask & kData) || index < 0 || index >= count) {
            m.set_return(0u);
            return true;
        }
        auto& item = st.parsed.items[static_cast<usize>(index)];
        u32& state = st.states[static_cast<usize>(index)];
        if (msg == sl::LM_GETITEM) {
            if (mask & sl::LIF_STATE) {
                // state = item state & caller's stateMask (probe getitem_state_masked).
                const u32 want = m.read_u32(p + sl::kLitemStateMask).value_or(0);
                (void)m.write_u32(p + sl::kLitemState, state & want);
            }
            if (mask & sl::LIF_ITEMID) write_wchars(m, p + sl::kLitemId, item.id, sl::kMaxLinkIdText);
            if (mask & sl::LIF_URL) write_wchars(m, p + sl::kLitemUrl, item.url, sl::kMaxUrlLength);
            m.set_return(1u);
            return true;
        }
        u32 new_state = state;
        if (mask & sl::LIF_STATE) {
            const u32 state_mask = m.read_u32(p + sl::kLitemStateMask).value_or(0);
            if (state_mask & ~sl::kAllStates) {  // probe statebit5..7: FALSE, no change
                m.set_return(0u);
                return true;
            }
            const u32 bits = m.read_u32(p + sl::kLitemState).value_or(0);
            new_state = (state & ~state_mask) | (bits & state_mask);
        }
        if (mask & sl::LIF_ITEMID) {
            item.id = read_wchars(m, p + sl::kLitemId, sl::kMaxLinkIdText - 1);
        }
        if (mask & sl::LIF_URL) {
            // Changing the URL keeps LIS_VISITED (probe afterurl2); an
            // unterminated buffer keeps kMaxUrlLength - 1 characters.
            item.url = read_wchars(m, p + sl::kLitemUrl, sl::kMaxUrlLength - 1);
        }
        if (mask & sl::LIF_STATE) {
            if ((new_state & sl::LIS_FOCUSED) && !(state & sl::LIS_FOCUSED)) {
                set_focus_item(st.states, index);
            }
            st.states[static_cast<usize>(index)] = new_state;
        }
        changed = true;
        commit();
        m.set_return(1u);
        return true;
    }
    case sl::LM_HITTEST: {
        // Hit: iLink and szID are written, nothing else. Miss (or a disabled
        // link): iLink = -1 and FALSE (probe hit_*).
        const GuestAddr p = static_cast<GuestAddr>(lParam);
        if (p == 0) { m.set_return(0u); return true; }
        const i32 x = static_cast<i32>(m.read_u32(p).value_or(0));
        const i32 y = static_cast<i32>(m.read_u32(p + 4).value_or(0));
        i32 hit = m_bridge.syslink_hit_test(hwnd, x, y);
        if (hit >= count || (hit >= 0 && !(st.states[static_cast<usize>(hit)] & sl::LIS_ENABLED))) {
            hit = -1;
        }
        const GuestAddr item = p + sl::kHitTestItem;
        (void)m.write_u32(item + sl::kLitemLink, static_cast<u32>(hit));
        if (hit >= 0) {
            write_wchars(m, item + sl::kLitemId, st.parsed.items[static_cast<usize>(hit)].id,
                         sl::kMaxLinkIdText);
        }
        m.set_return(hit >= 0 ? 1u : 0u);
        return true;
    }
    case sl::LM_GETIDEALHEIGHT: {
        // wParam = maximum width (0: no wrapping, probe zero_width); lParam =
        // optional SIZE*, filled with the laid-out size; returns cy.
        i32 cx = 0;
        i32 cy = 0;
        (void)m_bridge.syslink_ideal_size(hwnd, static_cast<i32>(wParam), cx, cy);
        if (lParam != 0) {
            (void)m.write_u32(static_cast<GuestAddr>(lParam), static_cast<u32>(cx));
            (void)m.write_u32(static_cast<GuestAddr>(lParam) + 4, static_cast<u32>(cy));
        }
        m.set_return(static_cast<u64>(static_cast<u32>(cy)));
        return true;
    }
    case kWmGetDlgCode:
        m.set_return(kDlgcStatic);
        return true;
    case kWmKeyDown: {
        if (wParam == kVkTab) {
            // Tab / Shift+Tab move LIS_FOCUSED between enabled links; past
            // either end no link keeps focus (probe aftertab, tablast,
            // shifttab).
            const bool back = m_key_state && (m_key_state(kVkShift) & 0x8000) != 0;
            const i32 cur = focused_item(st.states);
            const i32 next = cur < 0
                ? first_enabled(st.states, back ? count - 1 : 0, back ? -1 : 1)
                : first_enabled(st.states, cur + (back ? -1 : 1), back ? -1 : 1);
            set_focus_item(st.states, next);
            changed = true;
            commit();
            m.set_return(1u);
            return true;
        }
        if (wParam == kVkReturn || wParam == kVkSpace) {
            // WM_KEYDOWN (not WM_CHAR) on a focused link sends NM_RETURN,
            // LWS_IGNORERETURN or not (probe enter_*, space_*, ignorereturn).
            const i32 cur = focused_item(st.states);
            commit();
            if (cur >= 0 && (st.states[static_cast<usize>(cur)] & sl::LIS_ENABLED)) {
                notify(kNmReturn, cur);
            }
            m.set_return(0u);
            return true;
        }
        m.set_return(0u);
        return true;
    }
    case kWmLButtonDown:
    case kWmLButtonUp: {
        // NM_CLICK is sent on button-up over the enabled link that was
        // pressed, after that link takes LIS_FOCUSED (probe after_down,
        // after_up, afterclick, disabled_link_click).
        const i32 x = static_cast<i16>(lParam & 0xFFFF);
        const i32 y = static_cast<i16>((lParam >> 16) & 0xFFFF);
        i32 hit = m_bridge.syslink_hit_test(hwnd, x, y);
        if (hit >= count || (hit >= 0 && !(st.states[static_cast<usize>(hit)] & sl::LIS_ENABLED))) {
            hit = -1;
        }
        if (msg == kWmLButtonDown) {
            st.pressed = hit;
            commit();
            m.set_return(0u);
            return true;
        }
        const bool click = hit >= 0 && hit == st.pressed;
        st.pressed = -1;
        if (click) {
            set_focus_item(st.states, hit);
            changed = true;
        }
        commit();
        if (click) notify(kNmClick, hit);
        m.set_return(0u);
        return true;
    }
    default:
        return false;
    }
}

}  // namespace vitreo::winapi
