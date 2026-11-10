// SPDX-License-Identifier: LGPL-2.1-only

/// @file raw_input_registration.cpp
/// @brief RegisterRawInputDevices and the WM_INPUT producers: the keyboard and
///        mouse transitions the host reports, and the packet lifetime rules.
///        Split out of the former mixed USER32 stubs file
///        as verbatim code motion.

#include "winapi/user32/user32_ext.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/kernel32/thread/thread.hpp"
#include "windowing/model/hwnd_manager.hpp"
#include "winapi/user32/input/raw_input_internal.hpp"
#include "winapi/user32/user32_internal.hpp"
#include "winapi/kernel32/system/time.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <deque>
#include <mutex>
#include <optional>
#include <tuple>

namespace vitreo::winapi {

std::optional<User32ExtManager::RawInputRegistration>
User32ExtManager::raw_registration_locked(
    const std::optional<RawInputRegistration>& own, u16 page, u16 usage) const {
    if (own.has_value()) return own;
    if (m_raw_excluded.contains((u32{page} << 16) | usage)) return std::nullopt;
    const auto found = m_raw_page_registrations.find(page);
    if (found == m_raw_page_registrations.end()) return std::nullopt;
    return found->second;
}

void User32ExtManager::impl_RegisterRawInputDevices(Marshaler& m) {
    constexpr u32 kRidevRemove = 0x00000001u;
    constexpr u16 kGenericDesktop = 0x0001u;
    constexpr u16 kMouse = 0x0002u;
    constexpr u16 kKeyboard = 0x0006u;

    const GuestAddr devices = m.arg_ptr(0);
    const u32 count = static_cast<u32>(m.arg(1));
    const u32 size = static_cast<u32>(m.arg(2));
    const u32 expected_size = m.cpu_is_64bit() ? 16u : 12u;
    if (devices == 0 || count == 0 || size != expected_size) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return;
    }

    // Whole-page and exclusion entries, in the order the call lists them: a
    // later entry for the same page or collection replaces an earlier one.
    std::vector<std::pair<u16, std::optional<RawInputRegistration>>> page_updates;
    std::vector<std::pair<u32, bool>> exclude_updates;
    std::optional<RawInputRegistration> gamepad_update;
    bool gamepad_seen=false;
    std::vector<std::pair<u32,std::optional<RawInputRegistration>>> other_updates;
    std::optional<RawInputRegistration> keyboard_update;
    std::optional<RawInputRegistration> mouse_update;
    bool keyboard_seen = false;
    bool mouse_seen = false;
    for (u32 i = 0; i < count; ++i) {
        const GuestAddr entry = devices + static_cast<GuestAddr>(i) * size;
        const auto usage_page = m.read_u16(entry);
        const auto usage = m.read_u16(entry + 2);
        const auto flags = m.read_u32(entry + 4);
        u64 target_value = 0;
        bool target_ok = false;
        if (m.cpu_is_64bit()) {
            const auto target = m.read_u64(entry + 8);
            target_ok = target.is_ok();
            if (target_ok) target_value = target.value();
        } else {
            const auto target = m.read_u32(entry + 8);
            target_ok = target.is_ok();
            if (target_ok) target_value = target.value();
        }
        if (usage_page.is_err() || usage.is_err() || flags.is_err() ||
            !target_ok) {
            set_last_error(error_codes::ERROR_NOACCESS);
            m.set_return(0);
            return;
        }
        VITREO_TRACE("[u32ext] RegisterRawInputDevices[%u] page=0x%x usage=0x%x flags=0x%x target=0x%llx\n",
                     i, usage_page.value(), usage.value(), flags.value(),
                     static_cast<unsigned long long>(target_value));
        if (raw_input_diag()) {
            std::fprintf(stderr,
                         "[rawinput] Register[%u] page=0x%x usage=0x%x "
                         "flags=0x%x target=0x%llx\n",
                         i, usage_page.value(), usage.value(), flags.value(),
                         static_cast<unsigned long long>(target_value));
        }
        // RAWINPUTDEVICE.dwFlags (winuser.h RIDEV_*): unknown bits fail with
        // ERROR_INVALID_FLAGS (s2msg_input_probe rid_bad_flags); the sink
        // modes need a target window (rid_inputsink_no_hwnd: 87).
        constexpr u32 kRidevValid = 0x00003731u;
        constexpr u32 kRidevNeedsTarget = 0x00000100u /* INPUTSINK */ |
                                          0x00001000u /* EXINPUTSINK */;
        if ((flags.value() & ~kRidevValid) != 0) {
            set_last_error(error_codes::ERROR_INVALID_FLAGS);
            m.set_return(0);
            return;
        }
        if ((flags.value() & kRidevNeedsTarget) != 0 && target_value == 0) {
            set_last_error(error_codes::ERROR_INVALID_PARAMETER);
            m.set_return(0);
            return;
        }
        const bool known_page=usage_page.value()==kGenericDesktop;
        // RIDEV_PAGEONLY (0x20) and RIDEV_EXCLUDE (0x10) share their two bits
        // with RIDEV_NOLEGACY (0x30), so the mode is the pair read together.
        // Measured (tools/oracle_probe/rawinput_pageonly_probe.expected):
        //  - usUsage 0 is valid only as exactly RIDEV_PAGEONLY, also with
        //    RIDEV_REMOVE; a plain entry, RIDEV_EXCLUDE, RIDEV_NOLEGACY or
        //    RIDEV_REMOVE alone with usage 0 is ERROR_INVALID_PARAMETER;
        //  - RIDEV_PAGEONLY with a usage is ERROR_INVALID_PARAMETER;
        //  - RIDEV_EXCLUDE needs a usage and a NULL hwndTarget.
        constexpr u32 kRidevModeMask = 0x00000030u;
        const u32 mode = flags.value() & kRidevModeMask;
        const bool removing = (flags.value() & kRidevRemove) != 0;
        const u32 collection_key = (u32{usage_page.value()} << 16) | usage.value();
        const auto invalid_parameter = [&] {
            set_last_error(error_codes::ERROR_INVALID_PARAMETER);
            m.set_return(0);
        };
        if (usage.value() == 0) {
            if (mode != hid_sdk::kRIDEV_PAGEONLY) return invalid_parameter();
            if (removing) {
                if (target_value != 0) return invalid_parameter();
                page_updates.emplace_back(usage_page.value(), std::nullopt);
                continue;
            }
            constexpr u32 kPageModes = hid_sdk::kRIDEV_PAGEONLY |
                                       0x00000100u /* INPUTSINK */ |
                                       0x00001000u /* EXINPUTSINK */ |
                                       hid_sdk::kRIDEV_DEVNOTIFY;
            if ((flags.value() & ~kPageModes) != 0) {
                static std::once_flag once;
                std::call_once(once,[]{std::fprintf(stderr,"[vitreo] UNIMPLEMENTED user32!RegisterRawInputDevices: RIDEV_PAGEONLY with RIDEV_NOHOTKEYS / RIDEV_CAPTUREMOUSE / RIDEV_APPKEYS\n");});
                set_last_error(error_codes::ERROR_NOT_SUPPORTED);m.set_return(0);return;
            }
            const auto page_hwnd = static_cast<windowing::HWND>(target_value);
            if (page_hwnd != windowing::NULL_HWND && !m_hwnd_mgr.is_valid(page_hwnd)) {
                return invalid_parameter();
            }
            page_updates.emplace_back(
                usage_page.value(),
                RawInputRegistration{flags.value(), page_hwnd, m.cpu_is_64bit()});
            continue;
        }
        if (mode == hid_sdk::kRIDEV_PAGEONLY) return invalid_parameter();
        if (mode == hid_sdk::kRIDEV_EXCLUDE && !removing) {
            if (target_value != 0) return invalid_parameter();
            if (flags.value() != hid_sdk::kRIDEV_EXCLUDE) {
                static std::once_flag once;
                std::call_once(once,[]{std::fprintf(stderr,"[vitreo] UNIMPLEMENTED user32!RegisterRawInputDevices: RIDEV_EXCLUDE combined with other flags\n");});
                set_last_error(error_codes::ERROR_NOT_SUPPORTED);m.set_return(0);return;
            }
            // The exclusion takes the collection's entry: a registration of
            // its own is replaced (and an explicit one replaces it, below).
            exclude_updates.emplace_back(collection_key, true);
            if (known_page && usage.value() == kMouse) {
                mouse_seen = true; mouse_update.reset();
            } else if (known_page && usage.value() == kKeyboard) {
                keyboard_seen = true; keyboard_update.reset();
            } else if (known_page && usage.value() == hid_sdk::kHID_USAGE_GENERIC_GAMEPAD) {
                gamepad_seen = true; gamepad_update.reset();
            } else {
                other_updates.emplace_back(collection_key, std::nullopt);
            }
            continue;
        }
        // An explicit registration, or the removal of the collection's entry.
        exclude_updates.emplace_back(collection_key, false);
        const bool is_other=!known_page || (usage.value()!=kKeyboard && usage.value()!=kMouse && usage.value()!=hid_sdk::kHID_USAGE_GENERIC_GAMEPAD);
        const u32 other_key=(u32(usage_page.value())<<16)|usage.value();
        const bool is_gamepad=known_page && usage.value()==hid_sdk::kHID_USAGE_GENERIC_GAMEPAD;
        if((is_gamepad || is_other) && (flags.value() & ~(hid_sdk::kRIDEV_DEVNOTIFY|hid_sdk::kRIDEV_REMOVE))!=0) {
            static std::once_flag once;
            std::call_once(once,[]{std::fprintf(stderr,"[vitreo] UNIMPLEMENTED user32!RegisterRawInputDevices: HID registration mode\n");});
            set_last_error(error_codes::ERROR_NOT_SUPPORTED);m.set_return(0);return;
        }
        const bool is_mouse = known_page && usage.value() == kMouse;
        if(is_gamepad) gamepad_seen=true;
        else if (is_mouse) mouse_seen = true;
        else if(!is_other) keyboard_seen = true;
        const auto hwnd = static_cast<windowing::HWND>(target_value);
        if ((flags.value() & kRidevRemove) != 0) {
            if (hwnd != windowing::NULL_HWND) {
                set_last_error(error_codes::ERROR_INVALID_PARAMETER);
                m.set_return(0);
                return;
            }
            if(is_other) other_updates.emplace_back(other_key,std::nullopt);
            else if(is_gamepad) gamepad_update.reset();
            else if (is_mouse) mouse_update.reset();
            else keyboard_update.reset();
        } else {
            if (hwnd != windowing::NULL_HWND && !m_hwnd_mgr.is_valid(hwnd)) {
                set_last_error(error_codes::ERROR_INVALID_PARAMETER);
                m.set_return(0);
                return;
            }
            RawInputRegistration registration{
                flags.value(), hwnd, m.cpu_is_64bit()};
            if(is_other) other_updates.emplace_back(other_key,registration);
            else if(is_gamepad) gamepad_update=registration;
            else if (is_mouse) mouse_update = registration;
            else keyboard_update = registration;
        }
    }

    if (keyboard_seen || mouse_seen || gamepad_seen || !other_updates.empty() ||
        !page_updates.empty() || !exclude_updates.empty()) {

        std::lock_guard lock(m_raw_input_mutex);
        for(const auto& [key,registration]:other_updates) {
            if(registration) m_raw_other_registrations[key]=*registration;
            else m_raw_other_registrations.erase(key);
        }
        for (const auto& [page, registration] : page_updates) {
            if (registration) m_raw_page_registrations[page] = *registration;
            else m_raw_page_registrations.erase(page);
        }
        for (const auto& [key, excluded] : exclude_updates) {
            if (excluded) m_raw_excluded.insert(key);
            else m_raw_excluded.erase(key);
        }
        if(gamepad_seen) {
            m_raw_gamepad_registration=gamepad_update;
        }
        // The gamepad collection's effective registration may have changed
        // through its own entry, its page's or an exclusion.
        if (gamepad_seen || !page_updates.empty() || !exclude_updates.empty()) {
            m_raw_gamepad_previous.clear();
            const auto gamepad_now = raw_registration_locked(
                m_raw_gamepad_registration, kGenericDesktop,
                hid_sdk::kHID_USAGE_GENERIC_GAMEPAD);
            if(gamepad_now && m_gamepad_catalog && !m_raw_gamepad_worker.joinable()) {
                m_raw_gamepad_worker=std::jthread([this](std::stop_token stop) {
                    std::stop_callback wake(stop,[this]{m_raw_gamepad_cv.notify_all();});
                    std::unique_lock lock(m_raw_input_mutex);
                    auto subscribed=[&] {
                        const auto registration = raw_registration_locked(
                            m_raw_gamepad_registration, 0x0001u,
                            hid_sdk::kHID_USAGE_GENERIC_GAMEPAD);
                        return registration && (m_application_active.load() ||
                            (registration->flags & hid_sdk::kRIDEV_DEVNOTIFY)!=0);
                    };
                    while(!stop.stop_requested()) {
                        m_raw_gamepad_cv.wait(lock,[&]{return stop.stop_requested() || subscribed();});
                        if(stop.stop_requested()) break;
                        lock.unlock();poll_raw_gamepads();lock.lock();
                        m_raw_gamepad_cv.wait_for(lock,std::chrono::milliseconds(8),[&]{return stop.stop_requested() || !subscribed();});
                    }
                });
            }
            m_raw_gamepad_cv.notify_all();
        }
        if (keyboard_seen) {
            if (keyboard_update.has_value()) {
                m_raw_keyboard_registration = keyboard_update;
            } else {
                m_raw_keyboard_registration.reset();
            }
        }
        if (mouse_seen) {
            if (mouse_update.has_value()) {
                m_raw_mouse_registration = mouse_update;
            } else {
                m_raw_mouse_registration.reset();
            }
        }
    }
    VITREO_TRACE("[u32ext] RegisterRawInputDevices count=%u size=%u keyboard=%s target=0x%x -> TRUE\n",
                 count, size, !keyboard_seen ? "unchanged" :
                 keyboard_update.has_value() ? "registered" : "removed",
                 keyboard_update.has_value() ? keyboard_update->target : 0u);
    m.set_return(1u);
}

void User32ExtManager::note_raw_keyboard_transition(u32 virtual_key,
                                                     bool down) {
    constexpr u16 kRiKeyBreak = 0x0001u;
    constexpr u16 kRiKeyE0 = 0x0002u;
    constexpr u32 kRidevInputSink = 0x00000100u;

    RawInputRegistration registration{};
    u64 handle = 0;
    windowing::MSG message{};
    {
        std::lock_guard lock(m_raw_input_mutex);
        const auto effective =
            raw_registration_locked(m_raw_keyboard_registration, 0x0001u, 0x0006u);
        if (!effective.has_value()) return;
        registration = *effective;
        const windowing::HWND target = registration.target != windowing::NULL_HWND
            ? registration.target : m_active_window;
        if (target == windowing::NULL_HWND || !m_hwnd_mgr.is_valid(target)) return;

        const u32 scan = map_virtual_key(virtual_key, 0);
        u16 raw_flags = down ? 0u : kRiKeyBreak;
        switch (virtual_key) {
            case 0x21: case 0x22: case 0x23: case 0x24:
            case 0x25: case 0x26: case 0x27: case 0x28:
            case 0x2D: case 0x2E:
                raw_flags |= kRiKeyE0;
                break;
            default:
                break;
        }
        handle = registration.is_64bit ? m_next_raw_input_handle++
            : static_cast<u32>(m_next_raw_input_handle++);
        RawKeyboardPacket packet{};
        packet.make_code = static_cast<u16>(scan);
        packet.flags = raw_flags;
        packet.virtual_key = static_cast<u16>(virtual_key);
        packet.message = down ? windowing::WM::KEYDOWN : windowing::WM::KEYUP;
        packet.is_64bit = registration.is_64bit;
        m_raw_keyboard_packets.emplace(handle, packet);
        m_raw_keyboard_order.push_back(handle);

        message.hwnd = target;
        message.message = windowing::WM::INPUT;
        message.wParam = (registration.flags & kRidevInputSink) != 0 ? 1u : 0u;
        message.lParam = handle;
        message.time = tick_count32();
    }
    m_queue.post(message);
    if (raw_input_diag()) {
        std::fprintf(stderr, "[rawinput] post WM_INPUT kbd vk=0x%x %s hwnd=0x%x raw=0x%llx\n",
                     virtual_key, down ? "down" : "up", message.hwnd,
                     static_cast<unsigned long long>(handle));
    }
    VITREO_TRACE("[u32ext] WM_INPUT keyboard vk=0x%x %s raw=0x%llx\n",
                 virtual_key, down ? "down" : "up",
                 static_cast<unsigned long long>(handle));
}

void User32ExtManager::note_raw_mouse_button(u32 button, bool down,
                                             windowing::HWND event_hwnd) {
    u16 button_flags = 0;
    switch (button) {
        case 0: button_flags = down ? 0x0001u : 0x0002u; break;
        case 1: button_flags = down ? 0x0004u : 0x0008u; break;
        case 2: button_flags = down ? 0x0010u : 0x0020u; break;
        default: return;
    }
    post_raw_mouse_buttons(button_flags, 0, event_hwnd);
}

void User32ExtManager::note_raw_mouse_wheel(i32 delta, bool horizontal,
                                            windowing::HWND event_hwnd) {
    // winuser.h: RI_MOUSE_WHEEL 0x0400 / RI_MOUSE_HWHEEL 0x0800; "the wheel
    // delta is stored in usButtonData" as a signed SHORT in WHEEL_DELTA units.
    // One packet per SHORT-sized chunk, like the WM_MOUSEWHEEL split.
    const u16 button_flags = horizontal ? 0x0800u : 0x0400u;
    while (delta != 0) {
        const i32 chunk = std::clamp(delta, -32760, 32760);
        delta -= chunk;
        post_raw_mouse_buttons(button_flags,
                               static_cast<u16>(static_cast<i16>(chunk)),
                               event_hwnd);
    }
}

void User32ExtManager::post_raw_mouse_buttons(u16 button_flags,
                                              u16 button_data,
                                              windowing::HWND event_hwnd) {
    constexpr u32 kRidevInputSink = 0x00000100u;
    RawInputRegistration registration{};
    u64 handle = 0;
    windowing::MSG message{};
    {
        std::lock_guard lock(m_raw_input_mutex);
        const auto effective =
            raw_registration_locked(m_raw_mouse_registration, 0x0001u, 0x0002u);
        if (!effective.has_value()) return;
        registration = *effective;
        windowing::HWND target = registration.target;
        if (target == windowing::NULL_HWND) target = event_hwnd;
        if (target == windowing::NULL_HWND) target = m_active_window;
        if (target == windowing::NULL_HWND || !m_hwnd_mgr.is_valid(target)) return;
        handle = registration.is_64bit ? m_next_raw_input_handle++
            : static_cast<u32>(m_next_raw_input_handle++);
        RawKeyboardPacket packet{};
        packet.mouse = true;
        packet.button_flags = button_flags;
        packet.button_data = button_data;
        packet.is_64bit = registration.is_64bit;
        m_raw_keyboard_packets.emplace(handle, packet);
        m_raw_keyboard_order.push_back(handle);
        message.hwnd = target;
        message.message = windowing::WM::INPUT;
        message.wParam = (registration.flags & kRidevInputSink) != 0 ? 1u : 0u;
        message.lParam = handle;
        message.time = tick_count32();
    }
    m_queue.post(message);
    if (raw_input_diag()) {
        std::fprintf(stderr, "[rawinput] post WM_INPUT mouse buttons=0x%x data=0x%x hwnd=0x%x raw=0x%llx\n",
                     button_flags, button_data, message.hwnd,
                     static_cast<unsigned long long>(handle));
    }
}

void User32ExtManager::note_raw_mouse_motion(double device_dx,
                                             double device_dy,
                                             windowing::HWND event_hwnd) {
    constexpr u32 kRidevInputSink = 0x00000100u;
    RawInputRegistration registration{};
    u64 handle = 0;
    windowing::MSG message{};
    {
        std::lock_guard lock(m_raw_input_mutex);
        const auto effective =
            raw_registration_locked(m_raw_mouse_registration, 0x0001u, 0x0002u);
        if (!effective.has_value()) return;
        // Device motion arrives with sub-count precision on macOS; RAWMOUSE
        // carries integer counts, so carry the remainder to the next event
        // instead of dropping slow motion.
        m_raw_mouse_frac_x += device_dx;
        m_raw_mouse_frac_y += device_dy;
        const i32 dx = static_cast<i32>(std::trunc(m_raw_mouse_frac_x));
        const i32 dy = static_cast<i32>(std::trunc(m_raw_mouse_frac_y));
        m_raw_mouse_frac_x -= dx;
        m_raw_mouse_frac_y -= dy;
        if (dx == 0 && dy == 0) return;
        registration = *effective;
        windowing::HWND target = registration.target;
        if (target == windowing::NULL_HWND) target = event_hwnd;
        if (target == windowing::NULL_HWND) target = m_active_window;
        if (target == windowing::NULL_HWND || !m_hwnd_mgr.is_valid(target)) return;
        handle = registration.is_64bit ? m_next_raw_input_handle++
            : static_cast<u32>(m_next_raw_input_handle++);
        RawKeyboardPacket packet{};
        packet.mouse = true;
        packet.last_x = dx;
        packet.last_y = dy;
        packet.is_64bit = registration.is_64bit;
        m_raw_keyboard_packets.emplace(handle, packet);
        m_raw_keyboard_order.push_back(handle);
        message.hwnd = target;
        message.message = windowing::WM::INPUT;
        message.wParam = (registration.flags & kRidevInputSink) != 0 ? 1u : 0u;
        message.lParam = handle;
        message.time = tick_count32();
    }
    m_queue.post(message);
}

void User32ExtManager::discard_raw_input_for_thread(u32 tid) {
    std::lock_guard lock(m_raw_input_mutex);
    const auto it = m_current_raw_input_by_thread.find(tid);
    if (it == m_current_raw_input_by_thread.end()) return;
    m_raw_keyboard_packets.erase(it->second);
    m_current_raw_input_by_thread.erase(it);
}

// Windows rawinput_lifetime_probe (x86/x64): packet reads and DefWindowProc
// leave the current handle valid. Only the next successful message removal
// retires it; PM_NOREMOVE and an empty PM_REMOVE do not. Buffer membership
// ends when WM_INPUT leaves the queue, separately from packet lifetime.
void User32ExtManager::note_removed_raw_input_message(
    const windowing::MSG& message) {
    const u32 tid = ThreadManager::get_current_thread_id_static();
    std::lock_guard lock(m_raw_input_mutex);
    if (const auto old = m_current_raw_input_by_thread.find(tid);
        old != m_current_raw_input_by_thread.end()) {
        m_raw_keyboard_packets.erase(old->second);
        m_current_raw_input_by_thread.erase(old);
    }
    if (message.message == windowing::WM::INPUT &&
        m_raw_keyboard_packets.contains(message.lParam)) {
        std::erase(m_raw_keyboard_order, message.lParam);
        m_current_raw_input_by_thread.emplace(tid, message.lParam);
    }
}

void User32ExtManager::impl_GetRegisteredRawInputDevices(Marshaler& m) {
    const GuestAddr out=m.arg_ptr(0),count=m.arg_ptr(1);
    const u32 stride=m.cpu_is_64bit() ? 16u:12u;
    if(m.arg32(2)!=stride) {set_last_error(error_codes::ERROR_INVALID_PARAMETER);m.set_return(0xFFFFFFFFu);return;}
    if(!count) {set_last_error(error_codes::ERROR_NOACCESS);m.set_return(0xFFFFFFFFu);return;}
    // Measured (rawinput_pageonly_probe.expected): the entries come back
    // sorted by usage page, then usage -- a page entry (usage 0) first -- with
    // RIDEV_DEVNOTIFY not reported; an exclusion is {page, usage,
    // RIDEV_EXCLUDE, NULL}.
    std::vector<std::tuple<u16,u16,RawInputRegistration>> rows;
    {
        std::lock_guard lock(m_raw_input_mutex);
        std::map<u32, RawInputRegistration> sorted;
        const auto listed = [](RawInputRegistration registration) {
            registration.flags &= ~hid_sdk::kRIDEV_DEVNOTIFY;
            return registration;
        };
        for (const auto& [page, registration] : m_raw_page_registrations) {
            sorted[u32{page} << 16] = listed(registration);
        }
        for (const u32 key : m_raw_excluded) {
            sorted[key] = RawInputRegistration{hid_sdk::kRIDEV_EXCLUDE,
                                               windowing::NULL_HWND, m.cpu_is_64bit()};
        }
        if(m_raw_mouse_registration) sorted[(1u<<16)|2u]=listed(*m_raw_mouse_registration);
        if(m_raw_gamepad_registration) sorted[(1u<<16)|hid_sdk::kHID_USAGE_GENERIC_GAMEPAD]=listed(*m_raw_gamepad_registration);
        if(m_raw_keyboard_registration) sorted[(1u<<16)|6u]=listed(*m_raw_keyboard_registration);
        for(const auto& [key,registration]:m_raw_other_registrations) sorted[key]=listed(registration);
        for (const auto& [key, registration] : sorted) {
            rows.emplace_back(static_cast<u16>(key>>16),static_cast<u16>(key),registration);
        }
    }
    const auto capacity=m.read_u32(count);
    if(capacity.is_err()) {set_last_error(error_codes::ERROR_NOACCESS);m.set_return(0xFFFFFFFFu);return;}
    if(!out || capacity.value()<rows.size()) {
        set_last_error(m.write_u32(count,static_cast<u32>(rows.size())).is_err() ? error_codes::ERROR_NOACCESS : error_codes::ERROR_INSUFFICIENT_BUFFER);
        m.set_return(0xFFFFFFFFu);return;
    }
    for(usize i=0;i<rows.size();++i) {
        const auto& [page,usage,registration]=rows[i];const GuestAddr at=out+i*stride;
        bool ok=m.write_u16(at,page).is_ok() && m.write_u16(at+2,usage).is_ok() && m.write_u32(at+4,registration.flags).is_ok();
        ok=ok && (m.cpu_is_64bit() ? m.write_u64(at+8,registration.target).is_ok() : m.write_u32(at+8,registration.target).is_ok());
        if(!ok) {set_last_error(error_codes::ERROR_NOACCESS);m.set_return(0xFFFFFFFFu);return;}
    }
    m.set_return(rows.size());
}

void User32ExtManager::set_gamepad_foreground_state(std::shared_ptr<std::atomic<bool>> state) {
    std::lock_guard lock(m_activation_mutex);m_gamepad_foreground_state=std::move(state);
    if(m_gamepad_foreground_state) m_gamepad_foreground_state->store(m_application_active.load(),std::memory_order_release);
}
void User32ExtManager::set_gamepad_catalog(std::shared_ptr<GamepadHidCatalog> catalog) {
    if(m_raw_gamepad_worker.joinable()) {m_raw_gamepad_worker.request_stop();m_raw_gamepad_worker.join();}
    m_gamepad_catalog=std::move(catalog);

}
void User32ExtManager::poll_raw_gamepads() {
    if(!m_gamepad_catalog) return;
    const auto devices=m_gamepad_catalog->devices();
    windowing::HWND active{};bool application_active=false;
    {std::lock_guard lock(m_activation_mutex);active=m_active_window;application_active=m_application_active.load();}
    std::vector<windowing::MSG> messages;
    {
        std::lock_guard lock(m_raw_input_mutex);
        std::unordered_set<u64> present;
        for(const auto& device:devices) present.insert(device.raw_handle());
        auto previous=std::move(m_raw_gamepad_devices);m_raw_gamepad_devices=present;
        const auto effective = raw_registration_locked(
            m_raw_gamepad_registration, 0x0001u, hid_sdk::kHID_USAGE_GENERIC_GAMEPAD);
        if(!effective) return;
        const auto registration=*effective;
        const auto target=registration.target ? registration.target : active;
        if(!target || !m_hwnd_mgr.is_valid(target)) return;
        if(registration.flags & hid_sdk::kRIDEV_DEVNOTIFY) {
            auto notify=[&](u64 device,bool arrival) {
                windowing::MSG message{};message.hwnd=target;message.message=hid_sdk::kWM_INPUT_DEVICE_CHANGE;
                message.wParam=arrival ? hid_sdk::kGIDC_ARRIVAL:hid_sdk::kGIDC_REMOVAL;message.lParam=device;message.time=tick_count32();messages.push_back(message);
            };
            for(auto device:present) if(!previous.contains(device)) notify(device,true);
            for(auto device:previous) if(!present.contains(device)) notify(device,false);
        }
        std::erase_if(m_raw_gamepad_previous,[&](const auto& pair){return !present.contains(pair.first);});
        for(const auto& device:devices) {
            if(!application_active) continue;
            const auto found=m_raw_gamepad_previous.find(device.raw_handle());
            if(found!=m_raw_gamepad_previous.end() && found->second==device.report) continue;
            m_raw_gamepad_previous[device.raw_handle()]=device.report;
            const u64 handle=registration.is_64bit ? m_next_raw_input_handle++ : static_cast<u32>(m_next_raw_input_handle++);
            RawKeyboardPacket packet{};packet.hid=true;packet.hid_device=device.raw_handle();
            packet.hid_report=device.report;packet.is_64bit=registration.is_64bit;
            m_raw_keyboard_packets.emplace(handle,packet);m_raw_keyboard_order.push_back(handle);
            windowing::MSG message{};message.hwnd=target;message.message=windowing::WM::INPUT;
            message.lParam=handle;message.time=tick_count32();messages.push_back(message);
        }
    }
    for(const auto& message:messages) m_queue.post(message);
}

} // namespace vitreo::winapi
