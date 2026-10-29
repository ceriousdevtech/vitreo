#include "resources.hpp"
#include "windowing/model/diag_flags.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/kernel32/module/module.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/user32/user32_internal.hpp"
#include "winapi/user32/messages/user32_case.hpp"
#include "windowing/resources/resource_walker.hpp"
#include "windowing/cocoa/cocoa_bridge.hpp"
#include "windowing/model/message_box_layout.hpp"
#include "windowing/model/hwnd_manager.hpp"
#include "ipc/hwnd_registry.hpp"
#include "core/memory/guest_memory.hpp"
#include "winapi/kernel32/thread/thread.hpp"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwctype>
#include <algorithm>
#include <vector>

namespace vitreo::winapi {

namespace {

constexpr u32 kMbTaskModal = 0x00002000u;

void set_window_enabled(windowing::HwndManager* mgr,
                        windowing::CocoaBridge* bridge,
                        windowing::HWND hwnd, bool enabled) {
    if (mgr == nullptr || hwnd == 0) return;
    windowing::WindowData* win = mgr->find(hwnd);
    if (win == nullptr) return;
    if (win->enabled == enabled) return;
    win->enabled = enabled;
    if (bridge != nullptr) {
        (void)bridge->set_control_enabled(hwnd, enabled);
    }
}

} // namespace

i32 ResourceManager::message_box(windowing::HWND owner,
                                  const std::string& caption,
                                  const std::string& text, u32 type) const {
    // Unattended answer: the MB_DEFBUTTONn button of the uType button set
    // (message_box_layout.hpp; Learn nf-winuser-messageboxw).
    auto default_button = [type]() -> i32 {
        return windowing::message_box::default_result(type);
    };

    // s2msg_msgbox_probe: an owner that is not a window fails with
    // ERROR_INVALID_WINDOW_HANDLE and a button set past MB_CANCELTRYCONTINUE
    // (6) with ERROR_INVALID_MSGBOX_STYLE, before anything is shown; a box
    // that was shown returns its button with the last error set to 0.
    if (owner != 0 && owner != windowing::HWND_DESKTOP && !window_exists(owner)) {
        set_last_error(error_codes::ERROR_INVALID_WINDOW_HANDLE);
        return 0;
    }
    if ((type & 0xFu) > 6u) {
        set_last_error(error_codes::ERROR_INVALID_MSGBOX_STYLE);
        return 0;
    }
    set_last_error(error_codes::ERROR_SUCCESS);

    // Learn About Dialog Boxes: disable the owner (or every top-level window
    // of this thread when MB_TASKMODAL) while the box is up.
    std::vector<windowing::HWND> disabled;
    auto disable_owner = [&](windowing::HWND hwnd) {
        if (hwnd == 0 || m_hwnd_mgr == nullptr) return;
        windowing::WindowData* win = m_hwnd_mgr->find(hwnd);
        if (win == nullptr || !win->enabled) return;
        set_window_enabled(m_hwnd_mgr, m_cocoa_bridge, hwnd, false);
        disabled.push_back(hwnd);
    };
    if ((type & kMbTaskModal) != 0 && m_hwnd_mgr != nullptr) {
        const u32 tid = ThreadManager::get_current_thread_id_static();
        for (windowing::HWND top :
             m_hwnd_mgr->enum_children(windowing::HWND_DESKTOP)) {
            const windowing::WindowData* win = m_hwnd_mgr->find(top);
            if (win != nullptr && win->owner_thread_id == tid) {
                disable_owner(top);
            }
        }
    } else {
        disable_owner(owner);
    }

    auto restore = [&]() {
        for (auto it = disabled.rbegin(); it != disabled.rend(); ++it) {
            set_window_enabled(m_hwnd_mgr, m_cocoa_bridge, *it, true);
        }
        disabled.clear();
    };

    i32 result = 0;
    // Headless / automated bring-up: answer the default button without a
    // Cocoa modal. Caption/text are already logged by the MessageBox thunks.
    if (const char* auto_mb = std::getenv("VITREO_MESSAGEBOX_AUTO");
        auto_mb != nullptr && auto_mb[0] != '\0' &&
        std::strcmp(auto_mb, "0") != 0) {
        result = default_button();
        std::fprintf(stderr,
                     "[msgbox] VITREO_MESSAGEBOX_AUTO -> %d ([%s] '%s')\n",
                     result, caption.c_str(), text.c_str());
        restore();
        return result;
    }

    // Show a real modal alert when the Cocoa bridge is available. Answering
    // with the default button without displaying anything dismisses the
    // dialog before the user can see it.
    if (m_cocoa_bridge != nullptr) {
        result = static_cast<i32>(
            m_cocoa_bridge->show_message_box(caption, text, type));
        restore();
        return result;
    }

    result = default_button();
    restore();
    return result;
}

i32 ResourceManager::task_dialog(
    const std::string& title,
    const std::string& instruction,
    const std::string& content,
    const std::vector<windowing::TaskDialogButton>& buttons,
    int default_button,
    const std::string& verification_text,
    bool& verification_checked,
    bool warning) const {
    if (buttons.empty()) return 0;
    const int fallback =
        buttons[static_cast<usize>(
                    default_button >= 0 &&
                            default_button < static_cast<int>(buttons.size())
                        ? default_button
                        : 0)]
            .id;

    if (const char* auto_mb = std::getenv("VITREO_MESSAGEBOX_AUTO");
        auto_mb != nullptr && auto_mb[0] != '\0' &&
        std::strcmp(auto_mb, "0") != 0) {
        std::fprintf(stderr,
                     "[msgbox] VITREO_MESSAGEBOX_AUTO -> %d ([%s] '%s')\n",
                     fallback, title.c_str(), instruction.c_str());
        return fallback;
    }

    if (m_cocoa_bridge != nullptr) {
        return static_cast<i32>(m_cocoa_bridge->show_task_dialog(
            title, instruction, content, buttons, default_button,
            verification_text, verification_checked, warning));
    }
    return fallback;
}

u32 ResourceManager::load_cursor(GuestAddr instance, u32 cursor_id) {
    if (instance == 0) {
        return idc::is_system_cursor(cursor_id)
            ? CURSOR_HANDLE_BASE + cursor_id
            : 0;
    }

    // MAKEINTRESOURCE ordinals occupy the low 16 bits. A larger value is a
    // guest string pointer, which this path does not resolve yet.
    if (cursor_id >= 0x10000u) {
        return 0;
    }

    GuestAddr image_base = 0;
    u32 rsrc_rva = 0;
    u32 rsrc_size = 0;
    if (m_image_base != 0 &&
        static_cast<GuestAddr>(instance) == m_image_base &&
        m_rsrc_rva != 0) {
        image_base = m_image_base;
        rsrc_rva = m_rsrc_rva;
        rsrc_size = m_rsrc_size;
    } else if (auto info = try_get_resource_dir_from_module(
                   m_memory, static_cast<GuestAddr>(instance))) {
        image_base = info->image_base;
        rsrc_rva = info->rsrc_rva;
        rsrc_size = info->rsrc_size;
    } else {
        return 0;
    }

    const IconSourceKey key{image_base, cursor_id};
    {
        std::scoped_lock lock(m_cursor_mutex);
        const auto found = m_shared_cursors.find(key);
        if (found != m_shared_cursors.end()) {
            return found->second;
        }
    }

    auto decoded = windowing::extract_cursor_by_group_id(
        m_memory, image_base, rsrc_rva, rsrc_size,
        static_cast<u16>(cursor_id));
    if (decoded.rgba.empty()) {
        // LoadCursor(hInstance, MAKEINTRESOURCE(id)) loads from the module
        // only. A missing RT_GROUP_CURSOR is NULL — not a stock fallback.
        // Returning IDC_ARROW makes WM_SETCURSOR call SetCursor(arrow) on
        // every mouse move and undoes SetCursor(NULL).
        return 0;
    }

    CursorImage image;
    image.width = decoded.width;
    image.height = decoded.height;
    image.hotspot_x = decoded.hotspot_x;
    image.hotspot_y = decoded.hotspot_y;
    image.rgba = std::move(decoded.rgba);

    std::scoped_lock lock(m_cursor_mutex);
    const auto found = m_shared_cursors.find(key);
    if (found != m_shared_cursors.end()) {
        return found->second;
    }
    const u32 handle = m_next_cursor_handle++;
    m_cursors.emplace(handle, CursorState{std::move(image), true});
    m_shared_cursors.emplace(key, handle);
    return handle;
}

u32 ResourceManager::create_cursor(u32 hotspot_x, u32 hotspot_y,
                                   u32 width, u32 height,
                                   const u8* and_plane, const u8* xor_plane) {
    auto decoded = windowing::decode_and_xor_cursor(
        width, height, hotspot_x, hotspot_y, and_plane, xor_plane);
    if (decoded.rgba.empty()) {
        return 0;
    }
    CursorImage image;
    image.width = decoded.width;
    image.height = decoded.height;
    image.hotspot_x = decoded.hotspot_x;
    image.hotspot_y = decoded.hotspot_y;
    image.rgba = std::move(decoded.rgba);
    std::scoped_lock lock(m_cursor_mutex);
    const u32 handle = m_next_cursor_handle++;
    m_cursors.emplace(handle, CursorState{std::move(image), false});
    return handle;
}

u32 ResourceManager::create_animated_cursor_rgba(
    u32 hotspot_x, u32 hotspot_y, u32 width, u32 height,
    std::vector<std::vector<u8>> frames, std::vector<u32> delays_ms) {
    const usize expected =
        static_cast<usize>(width) * static_cast<usize>(height) * 4u;
    if (width == 0 || height == 0 || width > 256 || height > 256 ||
        frames.empty() || frames.size() != delays_ms.size()) {
        return 0;
    }
    for (auto& frame : frames) {
        if (frame.size() < expected) return 0;
        frame.resize(expected);
    }
    CursorImage image;
    image.width = width;
    image.height = height;
    image.hotspot_x = hotspot_x;
    image.hotspot_y = hotspot_y;
    image.rgba = frames.front();
    // One frame is a still cursor. Leaving `frames` populated would make the
    // host start a timer that can never change anything.
    if (frames.size() > 1) {
        image.frames = std::move(frames);
        image.frame_delays_ms = std::move(delays_ms);
    }
    std::scoped_lock lock(m_cursor_mutex);
    const u32 handle = m_next_cursor_handle++;
    m_cursors.emplace(handle, CursorState{std::move(image), false});
    return handle;
}

u32 ResourceManager::create_cursor_rgba(u32 hotspot_x, u32 hotspot_y,
                                        u32 width, u32 height,
                                        std::vector<u8> rgba) {
    const usize expected =
        static_cast<usize>(width) * static_cast<usize>(height) * 4u;
    if (width == 0 || height == 0 || width > 256 || height > 256 ||
        rgba.size() < expected) {
        return 0;
    }
    rgba.resize(expected);
    CursorImage image;
    image.width = width;
    image.height = height;
    image.hotspot_x = hotspot_x;
    image.hotspot_y = hotspot_y;
    image.rgba = std::move(rgba);
    std::scoped_lock lock(m_cursor_mutex);
    const u32 handle = m_next_cursor_handle++;
    m_cursors.emplace(handle, CursorState{std::move(image), false});
    return handle;
}

bool ResourceManager::destroy_cursor(u32 cursor) {
    std::scoped_lock lock(m_cursor_mutex);
    const auto found = m_cursors.find(cursor);
    if (found == m_cursors.end() || found->second.shared) {
        return false;
    }
    m_cursors.erase(found);
    return true;
}

std::optional<ResourceManager::CursorImage>
ResourceManager::cursor_image(u32 handle) const {
    if (handle == 0) {
        return std::nullopt;
    }
    if (handle == 1) {
        CursorImage image;
        image.is_system = true;
        image.system_id = idc::IDC_ARROW;
        return image;
    }
    if (handle >= CURSOR_HANDLE_BASE) {
        const u32 system_id = handle - CURSOR_HANDLE_BASE;
        if (idc::is_system_cursor(system_id)) {
            CursorImage image;
            image.is_system = true;
            image.system_id = system_id;
            return image;
        }
    }
    std::scoped_lock lock(m_cursor_mutex);
    const auto found = m_cursors.find(handle);
    if (found == m_cursors.end()) {
        return std::nullopt;
    }
    return found->second.image;
}

u32 ResourceManager::load_icon(GuestAddr instance, u32 icon_id) {
    if (instance == 0) {
        switch (icon_id) {
            case idi::IDI_APPLICATION:
            case idi::IDI_ERROR:
            case idi::IDI_WARNING:
                return ICON_HANDLE_BASE + icon_id;
            default:
                return 0;
        }
    }

    // Microsoft Learn (winuser!LoadIconW): non-NULL hInstance loads a module
    // icon by MAKEINTRESOURCE ordinal (or name). Named icons are not wired yet.
    if (icon_id >= 0x10000u) {
        return 0;
    }

    GuestAddr image_base = 0;
    u32 rsrc_rva = 0;
    u32 rsrc_size = 0;
    if (m_image_base != 0 &&
        static_cast<GuestAddr>(instance) == m_image_base &&
        m_rsrc_rva != 0) {
        image_base = m_image_base;
        rsrc_rva = m_rsrc_rva;
        rsrc_size = m_rsrc_size;
    } else if (auto info = try_get_resource_dir_from_module(
                   m_memory, static_cast<GuestAddr>(instance))) {
        image_base = info->image_base;
        rsrc_rva = info->rsrc_rva;
        rsrc_size = info->rsrc_size;
    } else {
        return 0;
    }

    const u16 resource_id = static_cast<u16>(icon_id);
    const IconSourceKey key{image_base, resource_id};
    {
        std::scoped_lock lock(m_icon_mutex);
        const auto found = m_shared_icons.find(key);
        if (found != m_shared_icons.end()) {
            return found->second;
        }
    }

    auto ico_data = windowing::extract_icon_by_group_id(
        m_memory, image_base, rsrc_rva, rsrc_size, resource_id);
    if (ico_data.empty()) {
        return 0;
    }

    // LoadIcon selects SM_CXICON/SM_CYICON (typically 32). Match LoadIconMetric
    // LIM_LARGE so IconList / common-control hosts get a usable HICON.
    return register_icon_image(std::move(ico_data), key, 32, 32);
}

u32 ResourceManager::resource_miss_error(GuestAddr instance, bool icon) const {
    constexpr u32 kTypeNotFound = 1813, kNameNotFound = 1814;
    if (instance == 0) return icon ? kTypeNotFound : kNameNotFound;  // "LoadIconW(12345)=1813", "LoadCursorW(12345)=1814"
    auto info = try_get_resource_dir_from_module(m_memory, instance);
    if (!info) return kTypeNotFound;
    const auto names = windowing::enumerate_resource_names(m_memory, info->image_base, info->rsrc_rva,
                                                           info->rsrc_size,
                                                           icon ? windowing::RT::GROUP_ICON : windowing::RT::GROUP_CURSOR,
                                                           false, nullptr, 0);
    return names.empty() ? kTypeNotFound : kNameNotFound;
}

u32 ResourceManager::load_cursor_named(GuestAddr instance, const std::u16string& name) {
    if (instance == 0 || name.empty() || name.size() > 0xFFFF) return 0;
    auto info = try_get_resource_dir_from_module(m_memory, instance);
    if (!info) return 0;
    auto decoded = windowing::extract_cursor_by_group_name(m_memory, info->image_base, info->rsrc_rva,
                                                           info->rsrc_size, name.data(),
                                                           static_cast<u16>(name.size()));
    if (decoded.rgba.empty()) return 0;
    CursorImage image;
    image.width = decoded.width;
    image.height = decoded.height;
    image.hotspot_x = decoded.hotspot_x;
    image.hotspot_y = decoded.hotspot_y;
    image.rgba = std::move(decoded.rgba);
    std::scoped_lock lock(m_cursor_mutex);
    const u32 handle = m_next_cursor_handle++;
    m_cursors.emplace(handle, CursorState{std::move(image), true});
    return handle;
}

u32 ResourceManager::load_icon_named(GuestAddr instance, const std::u16string& name) {
    if (instance == 0 || name.empty() || name.size() > 0xFFFF) return 0;
    auto info = try_get_resource_dir_from_module(m_memory, instance);
    if (!info) return 0;
    auto ico = windowing::extract_icon_by_group_name(m_memory, info->image_base, info->rsrc_rva, info->rsrc_size,
                                                     name.data(), static_cast<u16>(name.size()));
    if (ico.empty()) return 0;
    return register_icon_image(std::move(ico), std::nullopt, 0, 0);
}

u32 ResourceManager::load_icon_metric(GuestAddr instance, u16 icon_id, u32 metric) {
    if (metric > 1) {
        return 0;
    }

    if (instance == 0) {
        return load_icon(0, icon_id);
    }

    GuestAddr image_base = 0;
    u32 rsrc_rva = 0;
    u32 rsrc_size = 0;
    if (m_image_base != 0 &&
        static_cast<GuestAddr>(instance) == m_image_base &&
        m_rsrc_rva != 0) {
        image_base = m_image_base;
        rsrc_rva = m_rsrc_rva;
        rsrc_size = m_rsrc_size;
    } else if (auto info = try_get_resource_dir_from_module(
                   m_memory, static_cast<GuestAddr>(instance))) {
        image_base = info->image_base;
        rsrc_rva = info->rsrc_rva;
        rsrc_size = info->rsrc_size;
    } else {
        return 0;
    }

    auto ico_data = windowing::extract_icon_by_group_id(
        m_memory, image_base, rsrc_rva, rsrc_size, icon_id);
    if (ico_data.empty()) {
        return 0;
    }

    const i32 size = metric == 0 ? 16 : 32;
    return register_icon_image(
        std::move(ico_data),
        IconSourceKey{image_base, icon_id},
        size, size);
}

u32 ResourceManager::register_icon_image(
    std::vector<u8> ico_data,
    std::optional<IconSourceKey> shared_source,
    i32 requested_width,
    i32 requested_height
) {
    if (ico_data.size() < 6 || ico_data[0] != 0 || ico_data[1] != 0 ||
        ico_data[2] != 1 || ico_data[3] != 0 ||
        (ico_data[4] == 0 && ico_data[5] == 0)) {
        return 0;
    }
    const usize image_count = static_cast<usize>(ico_data[4]) |
        (static_cast<usize>(ico_data[5]) << 8);
    if (image_count > (ico_data.size() - 6) / 16) {
        return 0;
    }
    const usize directory_end = 6 + image_count * 16;
    auto read_le32 = [&ico_data](usize offset) {
        return static_cast<u32>(ico_data[offset]) |
            (static_cast<u32>(ico_data[offset + 1]) << 8) |
            (static_cast<u32>(ico_data[offset + 2]) << 16) |
            (static_cast<u32>(ico_data[offset + 3]) << 24);
    };
    for (usize index = 0; index < image_count; ++index) {
        const usize entry = 6 + index * 16;
        const u32 image_size = read_le32(entry + 8);
        const u32 image_offset = read_le32(entry + 12);
        if (image_size == 0 || image_offset < directory_end ||
            image_offset > ico_data.size() ||
            image_size > ico_data.size() - image_offset) {
            return 0;
        }
    }

    std::scoped_lock lock(m_icon_mutex);
    if (shared_source.has_value()) {
        const auto found = m_shared_icons.find(*shared_source);
        if (found != m_shared_icons.end()) {
            return found->second;
        }
    }

    const u32 handle = m_next_icon_handle++;
    m_icons.emplace(handle, IconState{
        std::move(ico_data), requested_width, requested_height,
        shared_source.has_value()});
    if (shared_source.has_value()) {
        m_shared_icons.emplace(*shared_source, handle);
    }
    return handle;
}

u32 ResourceManager::create_icon_object(IconPlanes planes) {
    if (!planes.valid()) return 0;
    if (planes.is_icon) {
        std::scoped_lock lock(m_icon_mutex);
        const u32 handle = m_next_icon_handle++;
        m_icons.emplace(handle, IconState{{}, static_cast<i32>(planes.width),
                                          static_cast<i32>(planes.height), false});
        m_icon_planes.emplace(handle, std::move(planes));
        return handle;
    }
    CursorImage image;
    image.width = planes.width;
    image.height = planes.height;
    image.hotspot_x = planes.hotspot_x;
    image.hotspot_y = planes.hotspot_y;
    image.rgba = icon_display_rgba(planes);
    u32 handle = 0;
    {
        std::scoped_lock lock(m_cursor_mutex);
        handle = m_next_cursor_handle++;
        m_cursors.emplace(handle, CursorState{std::move(image), false});
    }
    std::scoped_lock lock(m_icon_mutex);
    m_icon_planes.emplace(handle, std::move(planes));
    return handle;
}

ResourceManager::IconKind ResourceManager::icon_kind(u32 handle) const {
    if (handle == 0) return IconKind::None;
    if (handle == 1) return IconKind::StockSystem;
    if (handle >= CURSOR_HANDLE_BASE && handle < ICON_HANDLE_BASE &&
        idc::is_system_cursor(handle - CURSOR_HANDLE_BASE)) {
        return IconKind::StockSystem;
    }
    if (handle == ICON_HANDLE_BASE + idi::IDI_APPLICATION ||
        handle == ICON_HANDLE_BASE + idi::IDI_ERROR ||
        handle == ICON_HANDLE_BASE + idi::IDI_WARNING) {
        return IconKind::StockSystem;
    }
    {
        std::scoped_lock lock(m_icon_mutex);
        if (const auto it = m_icons.find(handle); it != m_icons.end()) {
            return it->second.shared ? IconKind::Shared : IconKind::Created;
        }
    }
    std::scoped_lock lock(m_cursor_mutex);
    if (const auto it = m_cursors.find(handle); it != m_cursors.end()) {
        return it->second.shared ? IconKind::Shared : IconKind::Created;
    }
    return IconKind::None;
}

std::optional<IconPlanes> ResourceManager::icon_planes(u32 handle) {
    {
        std::scoped_lock lock(m_icon_mutex);
        if (const auto it = m_icon_planes.find(handle); it != m_icon_planes.end()) {
            return it->second;
        }
        if (const auto it = m_icons.find(handle); it != m_icons.end() && !it->second.ico_data.empty()) {
            const u32 w = it->second.requested_width > 0 ? static_cast<u32>(it->second.requested_width) : 32u;
            const u32 h = it->second.requested_height > 0 ? static_cast<u32>(it->second.requested_height) : 32u;
            auto planes = decode_icon_file(it->second.ico_data, w, h);
            if (!planes) return std::nullopt;
            planes->is_icon = true;
            planes->hotspot_x = planes->width / 2;
            planes->hotspot_y = planes->height / 2;
            m_icon_planes.emplace(handle, *planes);
            return planes;
        }
    }
    std::optional<IconPlanes> planes;
    {
        std::scoped_lock lock(m_cursor_mutex);
        const auto it = m_cursors.find(handle);
        if (it == m_cursors.end() || it->second.image.rgba.empty()) return std::nullopt;
        const CursorImage& image = it->second.image;
        // A cursor kept only as presenter RGBA: the alpha plane stands in for
        // the AND mask.
        planes = icon_planes_from_rgba(image.width, image.height, image.rgba);
        planes->is_icon = false;
        planes->hotspot_x = image.hotspot_x;
        planes->hotspot_y = image.hotspot_y;
    }
    std::scoped_lock lock(m_icon_mutex);
    m_icon_planes.emplace(handle, *planes);
    return planes;
}

bool ResourceManager::destroy_icon_object(u32 handle) {
    switch (icon_kind(handle)) {
    case IconKind::None:
        return false;
    case IconKind::Shared:
    case IconKind::StockSystem:
        // s3gdi_icon_probe "DestroyIcon(shared)=1 ... still-valid=1".
        return true;
    case IconKind::Created:
        break;
    }
    {
        std::scoped_lock lock(m_icon_mutex);
        m_icons.erase(handle);
        m_icon_planes.erase(handle);
        m_destroyed_icons.insert(handle);
    }
    std::scoped_lock lock(m_cursor_mutex);
    m_cursors.erase(handle);
    return true;
}

bool ResourceManager::icon_was_destroyed(u32 handle) const {
    std::scoped_lock lock(m_icon_mutex);
    return m_destroyed_icons.contains(handle);
}

bool ResourceManager::destroy_icon(u32 icon) {
    std::scoped_lock lock(m_icon_mutex);
    const auto found = m_icons.find(icon);
    if (found == m_icons.end() || found->second.shared) {
        return false;
    }
    m_icons.erase(found);
    return true;
}

void ResourceManager::set_resource_info(GuestAddr image_base, u32 rsrc_rva, u32 rsrc_size) {
    m_image_base = image_base;
    m_rsrc_rva   = rsrc_rva;
    m_rsrc_size  = rsrc_size;
}

i32 ResourceManager::load_string(GuestAddr instance, u32 id, GuestAddr buf,
                                 u32 cch_max, bool is_64bit) const {
    // Win32: hInstance is the module base (or an AS_DATAFILE tagged handle)
    // for the resource module. Learn LoadStringW.
    GuestAddr module_base = m_image_base;
    u32 rsrc_rva = m_rsrc_rva;
    u32 rsrc_size = m_rsrc_size;
    if (instance != 0 && instance != m_image_base) {
        if (m_module_mgr == nullptr) return 0;
        GuestAddr untagged = instance;
        u32 module_rva = 0;
        u32 module_size = 0;
        if (!m_module_mgr->resource_directory_for_base(
                instance, module_rva, module_size, &untagged)) {
            return 0;
        }
        module_base = untagged;
        rsrc_rva = module_rva;
        rsrc_size = module_size;
    }

    // s3gdi_usermisc_probe: a missing string is 0 with
    // ERROR_RESOURCE_NAME_NOT_FOUND and an emptied buffer; cchBufferMax 0
    // stores a read-only pointer to the resource text in *lpBuffer and
    // returns its length ("LoadStringW(101, cch0)=23 ptr=ok").
    auto miss = [&]() {
        if (buf != 0 && cch_max > 0) (void)m_memory.write_u16(buf, 0);
        set_last_error(1814u);  // ERROR_RESOURCE_NAME_NOT_FOUND
        return 0;
    };
    if (module_base == 0 || rsrc_rva == 0 || rsrc_size == 0) {
        return miss();
    }
    if (buf == 0) return 0;

    // String tables are stored in blocks of 16 strings.
    // Resource name/id for a block: (id / 16) + 1
    // Index inside the block: id % 16
    const u16 block_id = static_cast<u16>((id / 16u) + 1u);
    const u16 index_in_block = static_cast<u16>(id % 16u);

    auto entry = windowing::find_resource(
        m_memory, module_base, rsrc_rva, rsrc_size,
        windowing::RT::STRING, block_id);

    if (!entry.data || entry.size < 2) {
        return miss();
    }

    // Walk length-prefixed UTF-16LE strings to the desired index.
    const u8* p = entry.data;
    const u8* end = entry.data + entry.size;
    for (u16 i = 0; i < 16; ++i) {
        if (p + 2 > end) {
            return miss();
        }
        const u16 len = static_cast<u16>(p[0] | (static_cast<u16>(p[1]) << 8));
        p += 2;

        const u32 bytes = static_cast<u32>(len) * 2u;
        if (p + bytes > end) {
            return miss();
        }

        if (i == index_in_block) {
            if (len == 0) {
                // An empty string: 0 with the error untouched and an empty
                // buffer ("LoadStringW(102 empty)=0 err=57005 buf0=0000").
                if (cch_max > 0) (void)m_memory.write_u16(buf, 0);
                return 0;
            }
            if (cch_max == 0) {
                const GuestAddr text = entry.guest_addr + static_cast<GuestAddr>(p - entry.data);
                if (is_64bit) (void)m_memory.write_u64(buf, text);
                else (void)m_memory.write_u32(buf, static_cast<u32>(text));
                return static_cast<i32>(len);
            }
            // Copy up to cch_max-1 chars; always NUL-terminate.
            const u32 copy_len = (cch_max > 0)
                ? std::min<u32>(static_cast<u32>(len), cch_max - 1)
                : 0;

            for (u32 j = 0; j < copy_len; ++j) {
                const u16 ch = static_cast<u16>(p[j * 2] | (static_cast<u16>(p[j * 2 + 1]) << 8));
                (void)m_memory.write_u16(buf + j * 2, ch);
            }
            (void)m_memory.write_u16(buf + copy_len * 2, 0);
            return static_cast<i32>(copy_len);
        }

        p += bytes;
    }

    return miss();
}

u32 ResourceManager::get_desktop_window() const {
    return windowing::HWND_DESKTOP;
}

u32 ResourceManager::get_foreground_window() const {
    if (m_cocoa_bridge != nullptr) {
        return m_cocoa_bridge->active_hwnd();
    }
    return windowing::HWND_DESKTOP;
}

bool ResourceManager::window_exists(u32 hwnd) const {
    if (m_hwnd_mgr != nullptr && m_hwnd_mgr->is_valid(static_cast<windowing::HWND>(hwnd))) {
        return true;
    }
    // A peer process's window is a valid HWND too.
    return m_hwnd_mgr == nullptr ||
           ipc::hwnd_registry_lookup(hwnd).has_value();
}

i32 ResourceManager::set_foreground_window(u32 hwnd) const {
    if (m_cocoa_bridge == nullptr) {
        return 1;
    }
    if (!m_cocoa_bridge->activate_window(hwnd)) {
        return 0;
    }
    m_cocoa_bridge->set_active_hwnd(hwnd);
    return 1;
}

// =============================================================================
// Thunk Registration
// =============================================================================

namespace {

// IsDialogMessage(hDlg, lpMsg). Vitreo has no dialog keyboard interface
// (WM_GETDLGCODE, Tab/arrow traversal over WS_TABSTOP/WS_GROUP, Enter to the
// default push button, Esc to IDCANCEL, mnemonics -- GetNextDlgTabItem is a
// stub too), so nothing is processed: FALSE, and the caller translates and
// dispatches the message itself, which is what Learn prescribes for a
// message the dialog manager does not take. Keyboard messages are the ones
// Windows would have processed, so they are reported once.
void is_dialog_message_unprocessed(Marshaler& m) {
    const GuestAddr msg = m.arg_ptr(1);
    if (msg != 0) {
        const u32 message = m.memory()
                                .read_u32(msg + (m.cpu_is_64bit() ? 8u : 4u))
                                .value_or(0);
        if (message >= 0x0100 && message <= 0x0109) {  // WM_KEYFIRST..WM_UNICHAR
            static std::atomic<bool> logged{false};
            if (!logged.exchange(true)) {
                std::fprintf(stderr,
                    "[vitreo] UNIMPLEMENTED user32!IsDialogMessage: dialog "
                    "keyboard navigation (Tab, arrows, Enter, Esc, mnemonics) "
                    "is not processed\n");
            }
        }
    }
    m.set_return(0);
}

} // namespace

DllExports create_resource_thunks(ResourceManager& mgr) {
    DllExports exports;
    exports.entries.reserve(20);

    // MessageBoxW (4 args, stdcall_bytes=16)
    exports.entries.push_back(ExportEntry{
        "MessageBoxW", 1,
        [&mgr](Marshaler& m) {
            const auto owner =
                static_cast<windowing::HWND>(m.arg(0));
            // Learn: NULL lpText → empty; NULL lpCaption → "Error".
            std::string text;
            if (m.arg_ptr(1) != 0) {
                auto text_res = m.read_wide_string(m.arg_ptr(1));
                text = text_res.is_ok() ? text_res.value() : "";
            }
            std::string caption = "Error";
            if (m.arg_ptr(2) != 0) {
                auto caption_res = m.read_wide_string(m.arg_ptr(2));
                caption = caption_res.is_ok() ? caption_res.value() : "Error";
            }
            u32 type = static_cast<u32>(m.arg(3));
            // Always log: a modal MessageBox with VITREO_TRACE off looks like a
            // silent hang (mach_msg in NSAlert). Caption/text are the diagnosis.
            std::fprintf(stderr,
                         "[msgbox] MessageBoxW: [%s] '%s' type=0x%x "
                         "owner=0x%x caller=0x%llx\n",
                         caption.c_str(), text.c_str(), type,
                         static_cast<unsigned>(owner),
                         static_cast<unsigned long long>(
                             m.guest_return_address()));
            i32 result = mgr.message_box(owner, caption, text, type);
            m.set_return(static_cast<u64>(result));
        },
        16
    });
    // MessageBoxA (4 args, stdcall_bytes=16)
    exports.entries.push_back(ExportEntry{
        "MessageBoxA", 2,
        [&mgr](Marshaler& m) {
            const auto owner =
                static_cast<windowing::HWND>(m.arg(0));
            std::string text;
            if (m.arg_ptr(1) != 0) {
                auto text_res = m.read_ansi_string(m.arg_ptr(1));
                text = text_res.is_ok() ? text_res.value() : "";
            }
            std::string caption = "Error";
            if (m.arg_ptr(2) != 0) {
                auto caption_res = m.read_ansi_string(m.arg_ptr(2));
                caption = caption_res.is_ok() ? caption_res.value() : "Error";
            }
            u32 type = static_cast<u32>(m.arg(3));
            std::fprintf(stderr,
                         "[msgbox] MessageBoxA: [%s] '%s' type=0x%x owner=0x%x\n",
                         caption.c_str(), text.c_str(), type,
                         static_cast<unsigned>(owner));
            i32 result = mgr.message_box(owner, caption, text, type);
            m.set_return(static_cast<u64>(result));
        },
        16
    });
    // MessageBoxExW (5 args, stdcall_bytes=20). Microsoft documents that
    // MessageBoxEx and MessageBox currently have identical behavior; the
    // language ID selects localized system button labels.
    exports.entries.push_back(ExportEntry{
        "MessageBoxExW", 0,
        [&mgr](Marshaler& m) {
            const auto owner =
                static_cast<windowing::HWND>(m.arg(0));
            std::string text;
            if (m.arg_ptr(1) != 0) {
                auto text_res = m.read_wide_string(m.arg_ptr(1));
                text = text_res.is_ok() ? text_res.value() : "";
            }
            std::string caption = "Error";
            if (m.arg_ptr(2) != 0) {
                auto caption_res = m.read_wide_string(m.arg_ptr(2));
                caption = caption_res.is_ok() ? caption_res.value() : "Error";
            }
            const u32 type = static_cast<u32>(m.arg(3));
            const u16 language = static_cast<u16>(m.arg(4));
            std::fprintf(stderr,
                         "[msgbox] MessageBoxExW: [%s] '%s' type=0x%x"
                         " language=0x%x owner=0x%x\n",
                         caption.c_str(), text.c_str(), type, language,
                         static_cast<unsigned>(owner));
            const i32 result = mgr.message_box(owner, caption, text, type);
            m.set_return(static_cast<u64>(result));
        },
        20
    });
    // LoadCursorW/A, LoadIconW/A (hInstance, lpName): pointer-sized
    // arguments; a name is MAKEINTRESOURCE or a string. Failures carry
    // ERROR_RESOURCE_TYPE/NAME_NOT_FOUND (s3gdi_usermisc_probe).
    const auto load_named = [&mgr](Marshaler& m, bool cursor, bool wide) {
        const GuestAddr instance = m.arg_ptr(0);
        const GuestAddr name = m.arg_ptr(1);
        u32 handle = 0;
        if (name != 0 && name < 0x10000) {
            handle = cursor ? mgr.load_cursor(instance, static_cast<u32>(name))
                            : mgr.load_icon(instance, static_cast<u32>(name));
        } else if (name != 0) {
            std::u16string text;
            for (u32 i = 0; i < 256; ++i) {
                u32 unit = 0;
                if (wide) {
                    const auto r = m.read_u16(name + i * 2);
                    if (r.is_err()) break;
                    unit = r.value();
                } else {
                    const auto r = m.read_u8(name + i);
                    if (r.is_err()) break;
                    unit = r.value();
                }
                if (unit == 0) break;
                // Resource names compare upper-cased (Learn: FindResource).
                if (unit >= 'a' && unit <= 'z') unit -= 32;
                text.push_back(static_cast<char16_t>(unit));
            }
            handle = cursor ? mgr.load_cursor_named(instance, text) : mgr.load_icon_named(instance, text);
        }
        if (handle == 0) set_last_error(mgr.resource_miss_error(instance, !cursor));
        if (cursor && windowing::diag::input_diag()) {
            std::fprintf(stderr, "[input] LoadCursor instance=0x%llx name=0x%llx -> 0x%x\n",
                         static_cast<unsigned long long>(instance), static_cast<unsigned long long>(name), handle);
        }
        m.set_return(handle);
    };
    exports.entries.push_back(ExportEntry{"LoadCursorW", 3, [load_named](Marshaler& m) { load_named(m, true, true); }, 8});
    exports.entries.push_back(ExportEntry{"LoadCursorA", 4, [load_named](Marshaler& m) { load_named(m, true, false); }, 8});
    // LoadCursorFromFileA/W (1 arg, stdcall_bytes=4). Learn: loads a cursor
    // from a .cur or .ani file and returns NULL on failure. Vitreo has no
    // .cur/.ani decoder, so the honest answer is NULL with
    // ERROR_FILE_NOT_FOUND -- a caller is required to cope, and SimCity 4 does.
    //
    // Registering it at all is what matters most here: the generic
    // missing-export stub pops 0 stdcall bytes, so an unimplemented entry left
    // this call's 4 argument bytes on the guest stack and the caller
    // eventually returned through a corrupted frame.
    const auto load_cursor_from_file = [](Marshaler& m) {
        set_last_error(error_codes::ERROR_FILE_NOT_FOUND);
        m.set_return(0u);
    };
    exports.entries.push_back(ExportEntry{
        "LoadCursorFromFileA", 0, load_cursor_from_file, 4
    });
    exports.entries.push_back(ExportEntry{
        "LoadCursorFromFileW", 0, load_cursor_from_file, 4
    });
    exports.entries.push_back(ExportEntry{"LoadIconW", 5, [load_named](Marshaler& m) { load_named(m, false, true); }, 8});
    exports.entries.push_back(ExportEntry{"LoadIconA", 6, [load_named](Marshaler& m) { load_named(m, false, false); }, 8});
    // LoadStringW (4 args, stdcall_bytes=16)
    exports.entries.push_back(ExportEntry{
        "LoadStringW", 7,
        [&mgr](Marshaler& m) {
            // HINSTANCE is pointer-sized (AS_DATAFILE handles live in the
            // high arena — truncating to u32 loses the mapping base).
            GuestAddr instance = m.arg_ptr(0);
            u32 id = static_cast<u32>(m.arg(1));
            GuestAddr buf = m.arg_ptr(2);
            u32 cch_max = static_cast<u32>(m.arg(3));
            m.set_return(static_cast<u64>(
                mgr.load_string(instance, id, buf, cch_max, m.cpu_is_64bit())));
        },
        16
    });
    // GetDesktopWindow (0 args, stdcall_bytes=0)
    exports.entries.push_back(ExportEntry{
        "GetDesktopWindow", 8,
        [&mgr](Marshaler& m) {
            m.set_return(mgr.get_desktop_window());
        },
        0
    });
    // GetForegroundWindow (0 args, stdcall_bytes=0)
    exports.entries.push_back(ExportEntry{
        "GetForegroundWindow", 9,
        [&mgr](Marshaler& m) {
            const u32 hwnd = mgr.get_foreground_window();
            static u32 last_reported = 0xFFFFFFFFu;
            if (hwnd != last_reported && std::getenv("VITREO_FOCUS_DIAG")) {
                last_reported = hwnd;
                std::fprintf(stderr, "[focus] GetForegroundWindow -> 0x%x\n",
                             hwnd);
            }
            m.set_return(hwnd);
        },
        0
    });
    // SetForegroundWindow (1 arg, stdcall_bytes=4)
    exports.entries.push_back(ExportEntry{
        "SetForegroundWindow", 10,
        [&mgr](Marshaler& m) {
            // NULL or an invalid window: 0 / ERROR_INVALID_WINDOW_HANDLE
            // (s2msg_input_probe setforeground_null / _invalid).
            u32 hwnd = static_cast<u32>(m.arg(0));
            if (hwnd == 0 || !mgr.window_exists(hwnd)) {
                set_last_error(error_codes::ERROR_INVALID_WINDOW_HANDLE);
                m.set_return(0);
                return;
            }
            m.set_return(static_cast<u64>(mgr.set_foreground_window(hwnd)));
        },
        4
    });

    // CharUpperW/CharLowerW(LPWSTR lpsz): Learn -- a value whose high-order
    // bits are zero (IS_INTRESOURCE: the whole pointer on x64) is one
    // character and the mapped character comes back; otherwise the
    // NUL-terminated string is mapped in place and the pointer returned.
    // The mapping is USER's measured table (user32_case.hpp).
    const auto case_w = [](Marshaler& m, u16 (*map)(u16)) {
        const u64 arg = m.cpu_is_64bit() ? m.arg(0) : static_cast<u32>(m.arg(0));
        if (arg <= 0xFFFFu) {
            m.set_return(map(static_cast<u16>(arg)));
            return;
        }
        const GuestAddr ptr = static_cast<GuestAddr>(arg);
        for (GuestAddr p = ptr;; p += 2) {
            auto unit = m.read_u16(p);
            if (!unit.is_ok() || unit.value() == 0) break;
            const u16 mapped = map(unit.value());
            if (mapped != unit.value()) (void)m.write_u16(p, mapped);
        }
        m.set_return(arg);
    };
    // CharUpperA/CharLowerA: the same in the ANSI code page (the 1252 table).
    const auto case_a = [](Marshaler& m, u8 (*map)(u8)) {
        const u64 arg = m.cpu_is_64bit() ? m.arg(0) : static_cast<u32>(m.arg(0));
        if (arg <= 0xFFFFu) {
            m.set_return(map(static_cast<u8>(arg)));
            return;
        }
        const GuestAddr ptr = static_cast<GuestAddr>(arg);
        for (GuestAddr p = ptr;; ++p) {
            auto byte = m.read_u8(p);
            if (!byte.is_ok() || byte.value() == 0) break;
            const u8 mapped = map(byte.value());
            if (mapped != byte.value()) (void)m.write_u8(p, mapped);
        }
        m.set_return(arg);
    };
    // Char*BuffW/A(lpsz, cchLength): exactly cchLength units, NULs included;
    // returns cchLength (s2msg_input_probe charlowerbuffw_ret=5, _zero=0).
    const auto buff_w = [](Marshaler& m, u16 (*map)(u16)) {
        const GuestAddr p = m.arg_ptr(0);
        const u32 cch = static_cast<u32>(m.arg(1));
        for (u32 i = 0; i < cch; ++i) {
            auto unit = m.read_u16(p + static_cast<GuestAddr>(i) * 2);
            if (!unit.is_ok()) break;
            const u16 mapped = map(unit.value());
            if (mapped != unit.value()) (void)m.write_u16(p + static_cast<GuestAddr>(i) * 2, mapped);
        }
        m.set_return(cch);
    };
    const auto buff_a = [](Marshaler& m, u8 (*map)(u8)) {
        const GuestAddr p = m.arg_ptr(0);
        const u32 cch = static_cast<u32>(m.arg(1));
        for (u32 i = 0; i < cch; ++i) {
            auto byte = m.read_u8(p + i);
            if (!byte.is_ok()) break;
            const u8 mapped = map(byte.value());
            if (mapped != byte.value()) (void)m.write_u8(p + i, mapped);
        }
        m.set_return(cch);
    };
    exports.entries.push_back(ExportEntry{
        "CharLowerW", 11,
        [case_w](Marshaler& m) { case_w(m, &user32_case::lower); }, 4});
    exports.entries.push_back(ExportEntry{
        "CharUpperW", 12,
        [case_w](Marshaler& m) { case_w(m, &user32_case::upper); }, 4});
    exports.entries.push_back(ExportEntry{
        "CharLowerA", 0,
        [case_a](Marshaler& m) { case_a(m, &user32_case::lower_ansi); }, 4});
    exports.entries.push_back(ExportEntry{
        "CharUpperA", 0,
        [case_a](Marshaler& m) { case_a(m, &user32_case::upper_ansi); }, 4});
    exports.entries.push_back(ExportEntry{
        "CharUpperBuffA", 0,
        [buff_a](Marshaler& m) { buff_a(m, &user32_case::upper_ansi); }, 8});
    exports.entries.push_back(ExportEntry{
        "CharLowerBuffA", 0,
        [buff_a](Marshaler& m) { buff_a(m, &user32_case::lower_ansi); }, 8});
    exports.entries.push_back(ExportEntry{
        "CharUpperBuffW", 0,
        [buff_w](Marshaler& m) { buff_w(m, &user32_case::upper); }, 8});
    exports.entries.push_back(ExportEntry{
        "CharLowerBuffW", 0,
        [buff_w](Marshaler& m) { buff_w(m, &user32_case::lower); }, 8});

    // IsCharAlpha / IsCharAlphaNumeric / IsCharUpper / IsCharLower, A and W.
    // Learn: "determined by the semantics of the language selected by the
    // user". Windows answers from the GetStringTypeW CT_CTYPE1 table -- alpha
    // is C1_ALPHA, alphanumeric C1_ALPHA|C1_DIGIT, upper C1_UPPER, lower
    // C1_LOWER -- and the A forms first convert the byte through CP_ACP
    // (u32_char_dde_probe: ischarw == ctype1 at 48 code points, ischara ==
    // acpc1 for all 256 bytes). Only the low WCHAR / CHAR of the argument is
    // used (ischaralphaw_hiword, ischaralphaa_hibyte); TRUE is 1 and the last
    // error is untouched.
    exports.entries.push_back(ExportEntry{
        "IsCharAlphaA", 0,
        [](Marshaler& m) { m.set_return(user32_char_class_a(m.arg(0), kIsCharAlpha)); }, 4});
    exports.entries.push_back(ExportEntry{
        "IsCharAlphaNumericA", 0,
        [](Marshaler& m) { m.set_return(user32_char_class_a(m.arg(0), kIsCharAlphaNumeric)); }, 4});
    exports.entries.push_back(ExportEntry{
        "IsCharUpperA", 0,
        [](Marshaler& m) { m.set_return(user32_char_class_a(m.arg(0), kIsCharUpper)); }, 4});
    exports.entries.push_back(ExportEntry{
        "IsCharLowerA", 0,
        [](Marshaler& m) { m.set_return(user32_char_class_a(m.arg(0), kIsCharLower)); }, 4});
    exports.entries.push_back(ExportEntry{
        "IsCharAlphaW", 0,
        [](Marshaler& m) { m.set_return(user32_char_class_w(m.arg(0), kIsCharAlpha)); }, 4});
    exports.entries.push_back(ExportEntry{
        "IsCharAlphaNumericW", 0,
        [](Marshaler& m) { m.set_return(user32_char_class_w(m.arg(0), kIsCharAlphaNumeric)); }, 4});
    exports.entries.push_back(ExportEntry{
        "IsCharUpperW", 0,
        [](Marshaler& m) { m.set_return(user32_char_class_w(m.arg(0), kIsCharUpper)); }, 4});
    exports.entries.push_back(ExportEntry{
        "ExitWindowsEx", 13,
        [](Marshaler& m) {
            // BOOL ExitWindowsEx(UINT uFlags, DWORD dwReason). Learn: shutting
            // down or restarting needs SE_SHUTDOWN_NAME; EWX_LOGOFF ends every
            // process of the logon session. A guest may neither power off nor
            // log off the Mac, and ending the capsule's session is not
            // implemented, so the request fails where it is made instead of
            // reporting a shutdown that never happens.
            constexpr u32 kShutdownActions = 0x1u | 0x2u | 0x8u;  // SHUTDOWN|REBOOT|POWEROFF
            const u32 flags = m.arg32(0);
            (void)m.arg32(1);
            static std::atomic_flag reported = ATOMIC_FLAG_INIT;
            if (!reported.test_and_set()) {
                std::fprintf(stderr,
                             "[vitreo] UNIMPLEMENTED user32!ExitWindowsEx: flags 0x%x "
                             "(no guest-initiated logoff or host shutdown)\n", flags);
            }
            set_last_error((flags & kShutdownActions) != 0
                               ? 1314u   // ERROR_PRIVILEGE_NOT_HELD
                               : error_codes::ERROR_NOT_SUPPORTED);
            m.set_return(0);
        },
        8
    });
    // NOTE: GetDlgItemTextW, GetWindowTextW, SetWindowTextW, and SetDlgItemTextW
    // are now in dialog.cpp since they need CocoaBridge access for native control updates

    // IsDialogMessageA/W (2 args, stdcall_bytes=8).
    //
    // The implemented subset does not yet perform dialog keyboard navigation,
    // so it must report every message as unprocessed.  Returning TRUE for a
    // non-keyboard message would tell the caller not to dispatch it, even
    // though IsDialogMessage had performed none of the translation or dispatch
    // required by that return value.
    exports.entries.push_back(ExportEntry{
        "IsDialogMessageW", 18,
        [](Marshaler& m) { is_dialog_message_unprocessed(m); },
        8
    });

    exports.entries.push_back(ExportEntry{
        "IsDialogMessageA", 0,
        [](Marshaler& m) { is_dialog_message_unprocessed(m); },
        8
    });

    return exports;
}

} // namespace vitreo::winapi
