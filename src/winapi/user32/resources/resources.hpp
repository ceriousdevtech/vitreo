#pragma once
/// @file resources.hpp
/// @brief user32 dialog, cursor, icon, resource stubs (WIN32-070)

#include <vitreo/common.hpp>
#include "winapi/core/thunk_registry.hpp"
#include "windowing/model/window_types.hpp"
#include "winapi/user32/resources/icon_planes.hpp"

#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace vitreo::winapi {

class Marshaler;
class ModuleManager;

} // namespace vitreo::winapi

namespace vitreo { class GuestMemory; }
namespace vitreo::windowing {
class CocoaBridge;
class HwndManager;
}

namespace vitreo::winapi {

// MessageBox types
namespace mb {
    constexpr u32 MB_OK = 0x0000;
    constexpr u32 MB_OKCANCEL = 0x0001;
    constexpr u32 MB_ABORTRETRYIGNORE = 0x0002;
    constexpr u32 MB_YESNOCANCEL = 0x0003;
    constexpr u32 MB_YESNO = 0x0004;
    constexpr u32 MB_RETRYCANCEL = 0x0005;
    constexpr u32 MB_TYPE_MASK = 0x000F;
}

// MessageBox return values
namespace id_btn {
    constexpr i32 IDOK = 1;
    constexpr i32 IDCANCEL = 2;
    constexpr i32 IDABORT = 3;
    constexpr i32 IDRETRY = 4;
    constexpr i32 IDIGNORE = 5;
    constexpr i32 IDYES = 6;
    constexpr i32 IDNO = 7;
}

// Standard cursor IDs
namespace idc {
    constexpr u32 IDC_ARROW = 32512;
    constexpr u32 IDC_IBEAM = 32513;
    constexpr u32 IDC_WAIT = 32514;
    constexpr u32 IDC_CROSS = 32515;
    constexpr u32 IDC_UPARROW = 32516;
    constexpr u32 IDC_SIZENWSE = 32642;
    constexpr u32 IDC_SIZENESW = 32643;
    constexpr u32 IDC_SIZEWE = 32644;
    constexpr u32 IDC_SIZENS = 32645;
    constexpr u32 IDC_SIZEALL = 32646;
    constexpr u32 IDC_NO = 32648;
    constexpr u32 IDC_HAND = 32649;
    constexpr u32 IDC_APPSTARTING = 32650;
    constexpr u32 IDC_HELP = 32651;

    [[nodiscard]] inline bool is_system_cursor(u32 id) {
        switch (id) {
            case IDC_ARROW:
            case IDC_IBEAM:
            case IDC_WAIT:
            case IDC_CROSS:
            case IDC_UPARROW:
            case IDC_SIZENWSE:
            case IDC_SIZENESW:
            case IDC_SIZEWE:
            case IDC_SIZENS:
            case IDC_SIZEALL:
            case IDC_NO:
            case IDC_HAND:
            case IDC_APPSTARTING:
            case IDC_HELP:
                return true;
            default:
                return false;
        }
    }
}

// Standard icon IDs
namespace idi {
    constexpr u32 IDI_APPLICATION = 32512;
    constexpr u32 IDI_ERROR = 32513;
    constexpr u32 IDI_WARNING = 32515;
}

/// Manages user32 resource/dialog stubs
class ResourceManager {
public:
    explicit ResourceManager(::vitreo::GuestMemory& memory) : m_memory{memory} {}

    /// Wire the Cocoa bridge so MessageBox can show a real modal alert.
    /// Without it, message_box() falls back to answering with the default
    /// button, which silently dismisses dialogs the user should have seen.
    void set_cocoa_bridge(windowing::CocoaBridge* bridge) { m_cocoa_bridge = bridge; }

    /// HWND table used to disable the MessageBox owner (and MB_TASKMODAL
    /// top-levels) for the duration of the box (Learn About Dialog Boxes).
    void set_hwnd_manager(windowing::HwndManager* mgr) { m_hwnd_mgr = mgr; }
    /// A window of this process (SetForegroundWindow's validation).
    [[nodiscard]] bool window_exists(u32 hwnd) const;

    /// Set PE resource directory info (called after PE load)
    void set_resource_info(GuestAddr image_base, u32 rsrc_rva, u32 rsrc_size);

    /// Optional ModuleManager so LoadStringW can resolve AS_DATAFILE /
    /// LoadLibrary HINSTANCE values (Learn LoadStringW).
    void set_module_manager(class ModuleManager* mgr) { m_module_mgr = mgr; }

    /// MessageBoxW/A/IndirectW: returns button ID based on uType. @p owner is
    /// disabled for the duration (MB_TASKMODAL disables every top-level of the
    /// thread). 0 with the last error set for an invalid owner or button set.
    [[nodiscard]] i32 message_box(windowing::HWND owner,
                                   const std::string& caption,
                                   const std::string& text, u32 type) const;

    /// TaskDialog/TaskDialogIndirect: show a modal alert with caller-supplied
    /// button labels and return the id of the button pressed. Shares
    /// MessageBox's headless fallback, so an automated run answers with
    /// `default_button`'s id instead of blocking on a modal.
    /// @param buttons Left-to-right; must not be empty.
    /// @param default_button Index into `buttons`.
    /// @param[in,out] verification_checked Checkbox state in and out.
    [[nodiscard]] i32 task_dialog(
        const std::string& title,
        const std::string& instruction,
        const std::string& content,
        const std::vector<windowing::TaskDialogButton>& buttons,
        int default_button,
        const std::string& verification_text,
        bool& verification_checked,
        bool warning) const;

private:
    windowing::CocoaBridge* m_cocoa_bridge = nullptr;
    windowing::HwndManager* m_hwnd_mgr = nullptr;
public:

    /// LoadCursorW/A: system IDC_* when instance is 0; otherwise the module's
    /// RT_GROUP_CURSOR. Shared: the same module+id returns the same HCURSOR.
    [[nodiscard]] u32 load_cursor(GuestAddr instance, u32 cursor_id);
    /// LoadCursor / LoadIcon with a string name, and the Win32 error of a
    /// failed load: ERROR_RESOURCE_TYPE_NOT_FOUND (1813) when the module has
    /// no cursor/icon groups, ERROR_RESOURCE_NAME_NOT_FOUND (1814) otherwise
    /// (s3gdi_usermisc_probe).
    [[nodiscard]] u32 load_cursor_named(GuestAddr instance, const std::u16string& name);
    [[nodiscard]] u32 load_icon_named(GuestAddr instance, const std::u16string& name);
    [[nodiscard]] u32 resource_miss_error(GuestAddr instance, bool icon) const;

    /// CreateCursor: retain AND/XOR bits as a top-down RGBA image.
    [[nodiscard]] u32 create_cursor(u32 hotspot_x, u32 hotspot_y,
                                    u32 width, u32 height,
                                    const u8* and_plane, const u8* xor_plane);

    /// CreateIconIndirect / CreateIconFromResource cursor: keep a top-down
    /// RGBA8 image on the handle so SetCursor can install it.
    [[nodiscard]] u32 create_cursor_rgba(u32 hotspot_x, u32 hotspot_y,
                                         u32 width, u32 height,
                                         std::vector<u8> rgba);

    /// Animated cursor (RIFF/ACON). @p frames is in display-step order and
    /// @p delays_ms is parallel to it; frame 0 also becomes the still image,
    /// so every consumer that ignores animation still gets the right shape.
    [[nodiscard]] u32 create_animated_cursor_rgba(
        u32 hotspot_x, u32 hotspot_y, u32 width, u32 height,
        std::vector<std::vector<u8>> frames, std::vector<u32> delays_ms);
    [[nodiscard]] bool destroy_cursor(u32 cursor);

    struct CursorImage {
        bool is_system = false;
        u32 system_id = 0;
        u32 width = 0;
        u32 height = 0;
        u32 hotspot_x = 0;
        u32 hotspot_y = 0;
        std::vector<u8> rgba;
        /// Display-step order, including frame 0. Empty for a static cursor;
        /// never of size 1 (a one-frame animation is just a static cursor).
        std::vector<std::vector<u8>> frames;
        std::vector<u32> frame_delays_ms;
    };

    /// Shape for SetCursor. System IDC_* handles have is_system set; custom
    /// handles carry tightly packed top-down RGBA8.
    [[nodiscard]] std::optional<CursorImage> cursor_image(u32 handle) const;

    /// LoadIconW/A: stock IDI_* when instance is NULL; otherwise MAKEINTRESOURCE
    /// RT_GROUP_ICON from the module (shared, like Windows LoadIcon).
    [[nodiscard]] u32 load_icon(GuestAddr instance, u32 icon_id);

    /// Load a module icon group for LoadIconMetric.
    ///
    /// The retained ICO data is the backing state for the returned HICON.
    /// metric is LIM_SMALL (0) or LIM_LARGE (1).
    [[nodiscard]] u32 load_icon_metric(GuestAddr instance, u16 icon_id, u32 metric);

    struct IconSourceKey {
        GuestAddr module_base{0};
        u32 resource_id{0};

        bool operator==(const IconSourceKey&) const = default;
    };

    /// Retain a valid ICO payload and return an opaque HICON.
    ///
    /// A source key enables the LR_SHARED cache. Without one, each call owns
    /// a distinct icon that DestroyIcon can release.
    [[nodiscard]] u32 register_icon_image(
        std::vector<u8> ico_data,
        std::optional<IconSourceKey> shared_source = std::nullopt,
        i32 requested_width = 0,
        i32 requested_height = 0);
    [[nodiscard]] bool destroy_icon(u32 icon);

    /// CreateIconIndirect / CreateIconFromResourceEx / CopyIcon / CopyImage:
    /// register an icon (fIcon) or cursor built from @p planes; a cursor is
    /// also installable by SetCursor. 0 when the planes are malformed.
    [[nodiscard]] u32 create_icon_object(IconPlanes planes);

    /// The planes behind an icon or cursor handle, decoding a module image
    /// on first use. nullopt for a handle that is no live icon/cursor, and
    /// for the stock system images (IDI_*, IDC_*), whose Windows artwork
    /// Vitreo does not carry.
    [[nodiscard]] std::optional<IconPlanes> icon_planes(u32 handle);

    enum class IconKind { None, Created, Shared, StockSystem };
    /// What kind of icon/cursor object @p handle is.
    [[nodiscard]] IconKind icon_kind(u32 handle) const;

    /// DestroyIcon / DestroyCursor (either destroys either kind,
    /// s3gdi_icon_probe "DestroyCursor(icon)=1"). True for a live object;
    /// shared and stock objects answer true and survive.
    [[nodiscard]] bool destroy_icon_object(u32 handle);
    /// True for a handle DestroyIcon/DestroyCursor already destroyed: a
    /// second destroy sets ERROR_INVALID_CURSOR_HANDLE, a value that never
    /// was a handle leaves the last error alone (s3gdi_icon_probe).
    [[nodiscard]] bool icon_was_destroyed(u32 handle) const;

    /// LoadStringW: returns number of wide chars written (excluding null).
    /// @p instance is pointer-sized (HINSTANCE / AS_DATAFILE tagged handle).
    [[nodiscard]] i32 load_string(GuestAddr instance, u32 id, GuestAddr buf,
                                  u32 cch_max, bool is_64bit = false) const;

    /// GetDesktopWindow: returns synthetic HWND
    [[nodiscard]] u32 get_desktop_window() const;

    /// GetForegroundWindow: returns the tracked native foreground guest HWND.
    [[nodiscard]] u32 get_foreground_window() const;

    /// SetForegroundWindow: activates a real top-level guest window.
    [[nodiscard]] i32 set_foreground_window(u32 hwnd) const;

private:
    static constexpr u32 CURSOR_HANDLE_BASE = 0x20000;
    static constexpr u32 ICON_HANDLE_BASE = 0x30000;
    static constexpr u32 DYNAMIC_ICON_HANDLE_BASE = 0x30000000;
    static constexpr u32 DYNAMIC_CURSOR_HANDLE_BASE = 0x40000000;

    struct IconState {
        std::vector<u8> ico_data;
        i32 requested_width = 0;
        i32 requested_height = 0;
        bool shared = false;
    };

    struct IconSourceKeyHash {
        usize operator()(const IconSourceKey& key) const noexcept {
            const usize module = static_cast<usize>(key.module_base);
            return module ^ (static_cast<usize>(key.resource_id) << 1);
        }
    };

    struct CursorState {
        CursorImage image;
        bool shared = false;
    };

    ::vitreo::GuestMemory& m_memory;
    GuestAddr m_image_base = 0;
    u32 m_rsrc_rva = 0;
    u32 m_rsrc_size = 0;
    ModuleManager* m_module_mgr = nullptr;
    mutable std::mutex m_icon_mutex;
    std::unordered_map<u32, IconState> m_icons;
    /// The image planes of every created icon/cursor, and the decoded planes
    /// of module icons/cursors once asked for (guarded by m_icon_mutex).
    std::unordered_map<u32, IconPlanes> m_icon_planes;
    /// Handles DestroyIcon/DestroyCursor freed (handle numbers never recur).
    std::unordered_set<u32> m_destroyed_icons;
    std::unordered_map<IconSourceKey, u32, IconSourceKeyHash>
        m_shared_icons;
    u32 m_next_icon_handle = DYNAMIC_ICON_HANDLE_BASE;
    mutable std::mutex m_cursor_mutex;
    std::unordered_map<u32, CursorState> m_cursors;
    std::unordered_map<IconSourceKey, u32, IconSourceKeyHash>
        m_shared_cursors;
    u32 m_next_cursor_handle = DYNAMIC_CURSOR_HANDLE_BASE;
};

DllExports create_resource_thunks(ResourceManager& mgr);

} // namespace vitreo::winapi
