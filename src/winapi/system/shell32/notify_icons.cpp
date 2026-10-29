/// @file notify_icons.cpp
/// @brief Shell_NotifyIconA/W and Shell_NotifyIconGetRect (see notify_icons.hpp).

#include "winapi/system/shell32/notify_icons.hpp"

#include "capsule/vfs/path_encoding.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/error.hpp"

#include <algorithm>
#include <cstring>
#include <memory>
#include <utility>

namespace vitreo::winapi {

namespace {

constexpr u32 kNimAdd = 0;
constexpr u32 kNimModify = 1;
constexpr u32 kNimDelete = 2;
constexpr u32 kNimSetFocus = 3;
constexpr u32 kNimSetVersion = 4;

constexpr u32 kNifMessage = 0x01;
constexpr u32 kNifIcon = 0x02;
constexpr u32 kNifTip = 0x04;
constexpr u32 kNifState = 0x08;
constexpr u32 kNifInfo = 0x10;
constexpr u32 kNifGuid = 0x20;
constexpr u32 kNifRealtime = 0x40;
constexpr u32 kNifShowTip = 0x80;

constexpr u32 kNisHidden = 0x01;

constexpr u32 kNiifIconMask = 0x0F;
constexpr u32 kNiifUser = 0x04;
constexpr u32 kNiifNoSound = 0x10;
constexpr u32 kNiifLargeIcon = 0x20;

// notifyicon_layout_probe: NIN_SELECT = WM_USER, NIN_KEYSELECT = +1, ...
constexpr u32 kNinSelect = 0x400;
constexpr u32 kNinBalloonShow = 0x402;
constexpr u32 kNinBalloonHide = 0x403;
constexpr u32 kNinBalloonTimeout = 0x404;
constexpr u32 kNinBalloonUserClick = 0x405;
constexpr u32 kNinPopupOpen = 0x406;
constexpr u32 kNinPopupClose = 0x407;

constexpr u32 kWmMouseMove = 0x200;
constexpr u32 kWmLButtonDown = 0x201;
constexpr u32 kWmLButtonUp = 0x202;
constexpr u32 kWmLButtonDblClk = 0x203;
constexpr u32 kWmRButtonDown = 0x204;
constexpr u32 kWmRButtonUp = 0x205;
constexpr u32 kWmContextMenu = 0x7B;

constexpr u32 kEFail = 0x80004005u;
constexpr u32 kEPointer = 0x80004003u;
constexpr u32 kEInvalidArg = 0x80070057u;
constexpr u32 kSFalse = 1;
constexpr u32 kErrorInvalidParameter = 87;

}  // namespace

/// One NOTIFYICONDATA as read from the guest, limited to the members its
/// cbSize covers. Windows takes any cbSize (even 10, s7 probe) as the largest
/// known version that fits, at least V1; members past it are not read
/// (tray probe "add id11 V1 size NIF_INFO" succeeds, "add id7 V2 size NIF_GUID"
/// identifies by hWnd + uID).
struct NotifyIconManager::Parsed {
    u32 size = 0;
    u32 hwnd = 0;
    u32 uid = 0;
    u32 flags = 0;
    u32 callback_message = 0;
    u32 hicon = 0;
    std::string tip;
    bool has_state = false;
    u32 state = 0;
    u32 state_mask = 0;
    bool has_info = false;
    std::string info;
    std::string info_title;
    u32 info_flags = 0;
    u32 version = 0;
    bool has_guid = false;
    std::array<u8, 16> guid{};
    u32 balloon_icon = 0;
};

void NotifyIconManager::set_presenter(Presenter presenter) {
    std::lock_guard lock(m_mutex);
    m_presenter = std::move(presenter);
}

void NotifyIconManager::set_window_check(std::function<bool(u32)> check) {
    std::lock_guard lock(m_mutex);
    m_window_check = std::move(check);
}

void NotifyIconManager::set_poster(std::function<void(u32, u32, u64, u64)> poster) {
    std::lock_guard lock(m_mutex);
    m_poster = std::move(poster);
}

void NotifyIconManager::set_icon_image(std::function<std::optional<IconImage>(u32)> image) {
    std::lock_guard lock(m_mutex);
    m_icon_image = std::move(image);
}

NotifyIconManager::Icon* NotifyIconManager::find(const Parsed& p) {
    // Learn (NOTIFYICONDATAW::uID): hWnd + uID, or guidItem when given, which
    // overrides uID. A GUID icon answers only to its GUID (tray probe "modify
    // uID 9 without guid" fails, "modify by guid with uID 10" succeeds).
    for (auto& icon : m_icons) {
        if (p.has_guid ? (icon.has_guid && icon.guid == p.guid)
                       : (!icon.has_guid && icon.hwnd == p.hwnd && icon.uid == p.uid)) {
            return &icon;
        }
    }
    return nullptr;
}

NotifyIconManager::Icon* NotifyIconManager::find_key(u64 key) {
    for (auto& icon : m_icons) {
        if (icon.key == key) return &icon;
    }
    return nullptr;
}

std::optional<u64> NotifyIconManager::reopen_target() const {
    std::lock_guard lock(m_mutex);
    for (auto it = m_icons.rbegin(); it != m_icons.rend(); ++it) {
        if ((it->state & kNisHidden) == 0 && it->callback_message != 0) return it->key;
    }
    return std::nullopt;
}

u32 NotifyIconManager::notify(Marshaler& m, u32 message, GuestAddr data, bool wide) {
    const bool is64 = m.cpu_is_64bit();
    const NotifyIconDataLayout& L = wide ? (is64 ? kNotifyIconDataW64 : kNotifyIconDataW32)
                                         : (is64 ? kNotifyIconDataA64 : kNotifyIconDataA32);
    auto fail = [](u32 error) {
        set_last_error(error);
        return 0u;
    };

    // tray probe "add NULL data" / "delete NULL data": ERROR_INVALID_PARAMETER.
    if (data == 0) return fail(kErrorInvalidParameter);
    const auto cb = m.read_u32(data);
    if (cb.is_err()) return fail(kErrorInvalidParameter);
    Parsed p;
    p.size = cb.value() >= L.full_size ? L.full_size
           : cb.value() >= L.v3_size   ? L.v3_size
           : cb.value() >= L.v2_size   ? L.v2_size
                                       : L.v1_size;
    auto u32_at = [&](u32 off) { return m.read_u32(data + off).value_or(0); };
    auto ptr_at = [&](u32 off) { return static_cast<u32>(m.read_ptr(data + off).value_or(0)); };
    auto text_at = [&](u32 off, usize chars) {
        if (wide) return m.read_wide_string(data + off, chars).value_or(std::string{});
        return capsule::ansi_to_utf8(m.read_ansi_string(data + off, chars).value_or(std::string{}));
    };
    p.hwnd = ptr_at(L.hwnd);
    p.uid = u32_at(L.uid);
    p.flags = u32_at(L.flags);
    p.callback_message = u32_at(L.callback_message);
    p.hicon = ptr_at(L.icon);
    // Learn (szTip): 64 characters before Windows 2000, 128 since; V1 is the
    // 64-character layout (notifyicon_layout_probe v1 = szTip + 64 chars).
    if ((p.flags & kNifTip) != 0) {
        p.tip = text_at(L.tip, p.size >= L.v2_size ? 128 : 64);
    }
    if (p.size >= L.v2_size) {
        p.has_state = true;
        p.state = u32_at(L.state);
        p.state_mask = u32_at(L.state_mask);
        p.has_info = true;
        if ((p.flags & kNifInfo) != 0) {
            p.info = text_at(L.info, 256);
            p.info_title = text_at(L.info_title, 64);
        }
        p.info_flags = u32_at(L.info_flags);
        p.version = u32_at(L.version);
    }
    if (p.size >= L.v3_size && (p.flags & kNifGuid) != 0) {
        p.has_guid = true;
        (void)m.memory().read_bytes(data + L.guid, reinterpret_cast<Byte*>(p.guid.data()), 16);
    }
    if (p.size >= L.full_size) p.balloon_icon = ptr_at(L.balloon_icon);

    std::vector<std::function<void()>> after;  // presenter calls, run unlocked
    {
        std::lock_guard lock(m_mutex);
        Icon* icon = find(p);
        auto apply = [&](Icon& target) {
            if ((p.flags & kNifMessage) != 0) target.callback_message = p.callback_message;
            if ((p.flags & kNifIcon) != 0) target.hicon = p.hicon;
            if ((p.flags & kNifTip) != 0) target.tip = p.tip;
            if ((p.flags & kNifState) != 0 && p.has_state) {
                target.state = (target.state & ~p.state_mask) | (p.state & p.state_mask);
            }
            // Learn (NIF_SHOWTIP): asks for the standard tooltip on a
            // NOTIFYICON_VERSION_4 icon; carried with every add or modify.
            target.show_tip = (p.flags & kNifShowTip) != 0;
            const Icon copy = target;
            after.push_back([this, copy] { present(copy); });
            if ((p.flags & kNifInfo) == 0 || !p.has_info) return;
            // Learn (NIF_INFO): text shows a balloon, an empty szInfo removes it.
            if (p.info.empty()) {
                std::erase_if(m_balloon_queue,
                              [&](const PendingBalloon& b) { return b.key == target.key; });
                if (m_balloon_up == target.key) {
                    m_balloon_up.reset();
                    after.push_back([this, copy] {
                        if (m_presenter.hide_balloon) m_presenter.hide_balloon(copy.key);
                        post(copy, kNinBalloonHide, icon_anchor(copy.key).first,
                             icon_anchor(copy.key).second);
                    });
                    if (auto show = pump_balloons_locked()) after.push_back(std::move(show));
                }
                return;
            }
            if (!m_presenter.show_balloon) return;  // nowhere to show it
            // Learn (NIF_REALTIME): discard what cannot be shown immediately.
            if ((p.flags & kNifRealtime) != 0 && m_balloon_up.has_value()) return;
            windowing::NotifyIconBalloon balloon;
            balloon.title = p.info_title;
            balloon.text = p.info;
            switch (p.info_flags & kNiifIconMask) {
            case 1: balloon.kind = windowing::NotifyIconBalloon::Kind::Info; break;
            case 2: balloon.kind = windowing::NotifyIconBalloon::Kind::Warning; break;
            case 3: balloon.kind = windowing::NotifyIconBalloon::Kind::Error; break;
            case kNiifUser: balloon.kind = windowing::NotifyIconBalloon::Kind::User; break;
            default: balloon.kind = windowing::NotifyIconBalloon::Kind::None; break;
            }
            if (balloon.kind == windowing::NotifyIconBalloon::Kind::User && m_icon_image) {
                // Learn (NIIF_USER): hBalloonIcon on Vista and later, the
                // tray icon when it is NULL (the legacy behaviour).
                const u32 source = p.balloon_icon != 0 ? p.balloon_icon : target.hicon;
                if (auto image = m_icon_image(source)) {
                    balloon.user_rgba = std::move(image->rgba);
                    balloon.user_width = image->width;
                    balloon.user_height = image->height;
                }
            }
            // Learn (dwInfoFlags): an icon only shows with a title.
            if (balloon.title.empty()) balloon.kind = windowing::NotifyIconBalloon::Kind::None;
            balloon.large_icon = (p.info_flags & kNiifLargeIcon) != 0;
            balloon.sound = (p.info_flags & kNiifNoSound) == 0;
            // NIIF_RESPECT_QUIET_TIME: never in quiet time here, so no effect.
            m_balloon_queue.push_back(PendingBalloon{target.key, std::move(balloon)});
            if (auto show = pump_balloons_locked()) after.push_back(std::move(show));
        };

        switch (message) {
        case kNimAdd: {
            if (icon != nullptr) return fail(kEFail);
            if (!m_window_check || !m_window_check(p.hwnd)) return fail(kEFail);
            Icon added;
            added.key = m_next_key++;
            added.hwnd = p.hwnd;
            added.uid = p.uid;
            added.has_guid = p.has_guid;
            added.guid = p.guid;
            m_icons.push_back(added);
            apply(m_icons.back());
            break;
        }
        case kNimModify:
            if (icon == nullptr) return fail(kEFail);
            apply(*icon);
            break;
        case kNimDelete: {
            if (icon == nullptr) return fail(kEFail);
            const Icon gone = *icon;
            std::erase_if(m_balloon_queue,
                          [&](const PendingBalloon& b) { return b.key == gone.key; });
            const bool balloon_up = m_balloon_up == gone.key;
            if (balloon_up) m_balloon_up.reset();
            m_icons.erase(m_icons.begin() + (icon - m_icons.data()));
            after.push_back([this, gone, balloon_up] {
                // Learn: NIN_BALLOONHIDE when the balloon goes away because
                // its icon was deleted.
                if (balloon_up) {
                    if (m_presenter.hide_balloon) m_presenter.hide_balloon(gone.key);
                    post(gone, kNinBalloonHide, 0, 0);
                }
                if (m_presenter.withdraw) m_presenter.withdraw(gone.key);
            });
            if (balloon_up) {
                if (auto show = pump_balloons_locked()) after.push_back(std::move(show));
            }
            break;
        }
        case kNimSetFocus:
            // Returns keyboard focus to the notification area; the menu bar
            // keeps none, so there is nothing to move.
            if (icon == nullptr) return fail(kEFail);
            break;
        case kNimSetVersion:
            // tray / s7 probes: 0, 3 and 4 are accepted; 1, 2 and 5 are not.
            if (icon == nullptr) return fail(kEFail);
            if (p.version != 0 && p.version != 3 && p.version != 4) return fail(kEFail);
            icon->version = p.version;
            after.push_back([this, copy = *icon] { present(copy); });
            break;
        default:
            return fail(kEFail);
        }
    }
    for (auto& action : after) action();
    // Success clears the last error (tray probe "add id1 -> 1 err=0").
    set_last_error(0);
    return 1;
}

std::function<void()> NotifyIconManager::pump_balloons_locked() {
    // Learn (NOTIFYICONDATA remarks): one balloon at a time; later ones queue
    // until it goes away.
    if (m_balloon_up.has_value() || m_balloon_queue.empty()) return {};
    auto next = std::make_shared<PendingBalloon>(std::move(m_balloon_queue.front()));
    m_balloon_queue.pop_front();
    m_balloon_up = next->key;
    return [this, next] {
        if (m_presenter.show_balloon) m_presenter.show_balloon(next->key, next->balloon);
    };
}

void NotifyIconManager::present(const Icon& icon) {
    if (!m_presenter.present) return;
    windowing::NotifyIconPresentation view;
    if (icon.hicon != 0 && m_icon_image) {
        if (auto image = m_icon_image(icon.hicon)) {
            view.rgba = std::move(image->rgba);
            view.width = image->width;
            view.height = image->height;
        }
    }
    view.tooltip = icon.tip;
    // Learn (NIF_SHOWTIP): a version 4 icon has its standard tooltip
    // suppressed for the application's pop-up UI unless NIF_SHOWTIP is set.
    view.native_tooltip = icon.version != 4 || icon.show_tip;
    view.hidden = (icon.state & kNisHidden) != 0;
    m_presenter.present(icon.key, view);
}

std::pair<i32, i32> NotifyIconManager::icon_anchor(u64 key) const {
    if (!m_presenter.frame) return {0, 0};
    const auto frame = m_presenter.frame(key);
    if (!frame) return {0, 0};
    return {(frame->left + frame->right) / 2, (frame->top + frame->bottom) / 2};
}

void NotifyIconManager::post(const Icon& icon, u32 event, i32 x, i32 y) {
    if (!m_poster || icon.callback_message == 0) return;
    u64 wparam = 0;
    u64 lparam = 0;
    if (icon.version == 4) {
        // Learn (uCallbackMessage, NOTIFYICON_VERSION_4): LOWORD(lParam) the
        // event, HIWORD(lParam) the 16-bit icon ID, wParam the X/Y anchor.
        lparam = (event & 0xFFFFu) | (static_cast<u64>(icon.uid & 0xFFFFu) << 16);
        wparam = static_cast<u64>(static_cast<u16>(x)) |
                 (static_cast<u64>(static_cast<u16>(y)) << 16);
    } else {
        // Versions 0 and 3: wParam the 32-bit icon ID, lParam the event.
        wparam = icon.uid;
        lparam = event;
    }
    m_poster(icon.hwnd, icon.callback_message, wparam, lparam);
}

void NotifyIconManager::on_input(u64 key, windowing::NotifyIconInput input, i32 x, i32 y) {
    using windowing::NotifyIconInput;
    Icon icon;
    std::vector<std::function<void()>> after;
    {
        std::lock_guard lock(m_mutex);
        Icon* found = find_key(key);
        if (found == nullptr) return;
        icon = *found;
        if (input == NotifyIconInput::BalloonClicked || input == NotifyIconInput::BalloonTimedOut) {
            if (m_balloon_up == key) m_balloon_up.reset();
            if (auto show = pump_balloons_locked()) after.push_back(std::move(show));
        }
    }
    const bool popups = icon.version == 4 && !icon.show_tip;
    const bool v3 = icon.version >= 3;
    const auto anchor = icon_anchor(key);
    // notifyicon_click_probe: the sequences a real click delivers.
    switch (input) {
    case NotifyIconInput::HoverEnter:
        if (popups) post(icon, kNinPopupOpen, anchor.first, anchor.second);
        post(icon, kWmMouseMove, x, y);
        break;
    case NotifyIconInput::HoverExit:
        if (popups) post(icon, kNinPopupClose, anchor.first, anchor.second);
        break;
    case NotifyIconInput::LeftDown:
        post(icon, kWmLButtonDown, x, y);
        break;
    case NotifyIconInput::LeftDoubleDown:
        post(icon, kWmLButtonDblClk, x, y);
        break;
    case NotifyIconInput::LeftUp:
        post(icon, kWmLButtonUp, x, y);
        if (v3) post(icon, kNinSelect, x, y);
        break;
    case NotifyIconInput::RightDown:
        post(icon, kWmRButtonDown, x, y);
        break;
    case NotifyIconInput::RightUp:
        post(icon, kWmRButtonUp, x, y);
        if (v3) post(icon, kWmContextMenu, x, y);
        break;
    case NotifyIconInput::BalloonShown:
        post(icon, kNinBalloonShow, anchor.first, anchor.second);
        break;
    case NotifyIconInput::BalloonClicked:
        post(icon, kNinBalloonUserClick, anchor.first, anchor.second);
        break;
    case NotifyIconInput::BalloonTimedOut:
        post(icon, kNinBalloonTimeout, anchor.first, anchor.second);
        break;
    case NotifyIconInput::Activate:
        // The icon's left click, as the click probe measured it, at the icon.
        post(icon, kWmMouseMove, anchor.first, anchor.second);
        post(icon, kWmLButtonDown, anchor.first, anchor.second);
        post(icon, kWmLButtonUp, anchor.first, anchor.second);
        if (v3) post(icon, kNinSelect, anchor.first, anchor.second);
        break;
    }
    for (auto& action : after) action();
}

u32 NotifyIconManager::get_rect(Marshaler& m, GuestAddr identifier, GuestAddr rect) {
    // HRESULT Shell_NotifyIconGetRect(const NOTIFYICONIDENTIFIER*, RECT*)
    // tray probe: NULL identifier or rect E_POINTER, a wrong cbSize
    // E_INVALIDARG, an unknown icon E_FAIL and a hidden one S_FALSE (both with
    // the rect zeroed), a visible one S_OK with its rectangle.
    if (identifier == 0 || rect == 0) return kEPointer;
    const bool is64 = m.cpu_is_64bit();
    const u32 size = is64 ? 40 : 28;  // notifyicon_layout_probe
    const auto cb = m.read_u32(identifier);
    if (cb.is_err() || cb.value() != size) return kEInvalidArg;
    const u32 hwnd = static_cast<u32>(m.read_ptr(identifier + (is64 ? 8 : 4)).value_or(0));
    const u32 uid = m.read_u32(identifier + (is64 ? 16 : 8)).value_or(0);
    std::array<u8, 16> guid{};
    (void)m.memory().read_bytes(identifier + (is64 ? 20 : 12), reinterpret_cast<Byte*>(guid.data()), 16);
    const bool by_guid = std::any_of(guid.begin(), guid.end(), [](u8 b) { return b != 0; });

    auto write_rect = [&](const windowing::Rect& r) {
        (void)m.write_u32(rect + 0, static_cast<u32>(r.left));
        (void)m.write_u32(rect + 4, static_cast<u32>(r.top));
        (void)m.write_u32(rect + 8, static_cast<u32>(r.right));
        (void)m.write_u32(rect + 12, static_cast<u32>(r.bottom));
    };
    std::optional<u64> key;
    bool hidden = false;
    {
        std::lock_guard lock(m_mutex);
        for (const auto& icon : m_icons) {
            if (by_guid ? (icon.has_guid && icon.guid == guid)
                        : (!icon.has_guid && icon.hwnd == hwnd && icon.uid == uid)) {
                key = icon.key;
                hidden = (icon.state & kNisHidden) != 0;
                break;
            }
        }
    }
    if (!key) {
        write_rect({});
        return kEFail;
    }
    const auto frame = hidden || !m_presenter.frame ? std::nullopt : m_presenter.frame(*key);
    if (!frame) {
        write_rect({});
        return kSFalse;
    }
    write_rect(*frame);
    return 0;
}

}  // namespace vitreo::winapi
