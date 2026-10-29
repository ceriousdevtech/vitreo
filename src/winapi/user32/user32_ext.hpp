// SPDX-License-Identifier: LGPL-2.1-only
#pragma once
/// @file user32_ext.hpp
/// @brief Extended user32 API surface.

#include <vitreo/common.hpp>
#include "winapi/core/thunk_registry.hpp"
#include "core/memory/guest_memory.hpp"
#include "windowing/model/window_types.hpp"
#include "windowing/contracts/menu_types.hpp"
#include "windowing/contracts/user32_host.hpp"
#include "windowing/messages/message_queue.hpp"
#include "windowing/messages/message_types.hpp"

#include "capsule/vfs/vfs.hpp"
#include "winapi/user32/windows/thread_scratch.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace vitreo::windowing {
class HwndManager;
}

namespace vitreo::winapi {

class Marshaler;
class WindowClassManager;
struct WindowClassData;
class GdiManager;
class PaintingManager;
class MessageLoopManager;
class MessageSendManager;
class DialogManager;
class ProcessManager;
class ModuleManager;
class GlobalLocalManager;
class ShowWindowManager;
class ResourceManager;

class User32ExtManager {
public:
    using GuestCall = std::function<u64(GuestAddr, const u64*, usize, bool)>;

    User32ExtManager(GuestMemory& memory,
                     capsule::Vfs& vfs,
                     windowing::HwndManager& hwnd_mgr,
                     WindowClassManager& class_mgr,
                     windowing::User32Host& host,
                     windowing::MessageQueue& queue,
                     std::function<std::string()> display_adapter_name_provider = {});
    User32ExtManager(const User32ExtManager&) = delete;
    User32ExtManager& operator=(const User32ExtManager&) = delete;
    ~User32ExtManager() = default;

    [[nodiscard]] windowing::CursorHost& cursor_host() noexcept {
        return m_host;
    }

    /// The guest virtual screen as a RECT: what GetSystemMetrics reports for
    /// SM_[XY]VIRTUALSCREEN / SM_C[XY]VIRTUALSCREEN, and what GetClipCursor
    /// reports when the cursor is unclipped. Virtual (exclusive) full-screen
    /// collapses it to the one (0,0)-(W,H) monitor the guest selected.
    [[nodiscard]] windowing::Rect guest_virtual_screen_rect() const;

    /// Clipboard GetClipboardData returns HGLOBALs callers GlobalLock/Free.
    void set_global_local_manager(GlobalLocalManager* mgr) {
        m_global_local = mgr;
    }
    /// The global-memory manager slot (DDEML data objects read it per call).
    [[nodiscard]] GlobalLocalManager* const& global_local_manager_slot() const noexcept {
        return m_global_local;
    }

    void impl_AppendMenuA(Marshaler& m);
    void impl_AppendMenuW(Marshaler& m);
    void append_menu_impl(Marshaler& m, bool ansi);
    void impl_CallWindowProcW(Marshaler& m);
    /// Body of the native-control wndproc, registered as a callable thunk so
    /// a subclassing guest can call the address GWLP_WNDPROC handed it.
    void impl_VitreoNativeControlProc(Marshaler& m);
    void impl_VitreoSystemControlProc(Marshaler& m, std::wstring_view base_class, bool ansi);
    using SystemControlInitializer = std::function<bool(Marshaler&, windowing::HWND,
                                                       std::wstring_view, u64, bool)>;
    void set_system_control_initializer(SystemControlInitializer callback) {
        m_system_control_initializer = std::move(callback);
    }


    /// Publish the callable thunk address that implements the native-control
    /// wndproc, once user32's thunks have been registered.
    static void set_native_control_wndproc(u64 address) noexcept;
    void impl_CallWindowProcA(Marshaler& m);
    /// Shared CallWindowProc{A,W} body. `ansi` selects string encoding for
    /// native control messages (LB_ADDSTRING etc.) when chaining to Vitreo's
    /// Def*Proc sentinel for Cocoa-backed dialog controls.
    void call_window_proc_impl(Marshaler& m, bool ansi);
    void call_window_proc_values(Marshaler& m, GuestAddr lpPrevWndFunc,
                                 windowing::HWND hwnd, u32 msg, u64 wParam,
                                 u64 lParam, bool ansi);
    void impl_CharNextA(Marshaler& m);
    void impl_CharNextW(Marshaler& m);
    void impl_CharNextExA(Marshaler& m);
    void impl_CharPrevA(Marshaler& m);
    void impl_CharPrevW(Marshaler& m);
    void impl_CheckDlgButton(Marshaler& m);
    void impl_IsDlgButtonChecked(Marshaler& m);
    void impl_SendDlgItemMessageW(Marshaler& m);
    void impl_SendDlgItemMessageA(Marshaler& m);
    // Shared body for SendDlgItemMessage{A,W}; `ansi` selects string encoding.
    void send_dlg_item_message_impl(Marshaler& m, bool ansi);
    void impl_ChangeClipboardChain(Marshaler& m);
    void impl_CloseClipboard(Marshaler& m);
    void impl_CreatePopupMenu(Marshaler& m);
    void impl_EmptyClipboard(Marshaler& m);
    void impl_EnableMenuItem(Marshaler& m);
    void impl_FindWindowExA(Marshaler& m);
    void impl_FindWindowExW(Marshaler& m);
    void find_window_ex_impl(Marshaler& m, bool ansi);
    /// FindWindow(A/W) (@p ex false) and FindWindowEx(A/W).
    void find_window_impl(Marshaler& m, bool ex, bool ansi);
    void impl_EnumWindows(Marshaler& m);
    /// EnumWindows with explicit arguments (EnumChildWindows(NULL, ...),
    /// EnumDesktopWindows of the thread's desktop).
    void impl_EnumWindows_values(Marshaler& m, GuestAddr enum_proc, u64 lparam);
    void impl_EnumThreadWindows(Marshaler& m);
    void impl_GetWindowThreadProcessId(Marshaler& m);
    void impl_GetDC(Marshaler& m);
    void impl_GetDCEx(Marshaler& m);
    void impl_WindowFromDC(Marshaler& m);
    void impl_GetMessagePos(Marshaler& m);
    void impl_GetCaretBlinkTime(Marshaler& m);
    void impl_SetCaretBlinkTime(Marshaler& m);
    void impl_GetSysColor(Marshaler& m);
    /// GetSysColor's value for `index` (SetSysColors overrides first).
    [[nodiscard]] u32 sys_color(u32 index) const;
    void impl_SetSysColors(Marshaler& m);
    void impl_GetSystemMenu(Marshaler& m);
    void impl_GetSystemMetrics(Marshaler& m);
    void impl_GetSystemMetricsForDpi(Marshaler& m);
    /// The guest's monitors, primary first (the virtual full-screen mode when
    /// one is set). HMONITOR i is 0x000D0001 + i and \\.\DISPLAY(i+1).
    [[nodiscard]] std::vector<windowing::MonitorInfo> guest_monitors() const;
    /// MonitorFromRect's rule: the monitor with the largest intersection,
    /// else by dwFlags. @p on_no_monitor skips the search (a NULL window).
    [[nodiscard]] u32 monitor_from_rect_rule(const windowing::Rect& r, u32 flags,
                                             bool on_no_monitor) const;
    /// Index of @p hmonitor in guest_monitors(), or -1.
    [[nodiscard]] int monitor_index(u64 hmonitor) const;
    void get_monitor_info(Marshaler& m, bool wide);
    /// GetSystemMetrics(@p index) (dpi 0) or GetSystemMetricsForDpi. @p valid
    /// is false for an index Windows rejects with ERROR_INVALID_PARAMETER.
    [[nodiscard]] i32 system_metric(i32 index, u32 dpi, bool& valid) const;
    /// The main image targets a subsystem older than 6.0: SM_CXFRAME folds
    /// in the padded border and SM_CXPADDEDBORDER is 0 (measured).
    void set_legacy_frame_metrics(bool legacy) noexcept;
    [[nodiscard]] bool legacy_frame_metrics() const noexcept { return m_legacy_frame_metrics; }
    void impl_GetWindowLongW(Marshaler& m);
    void impl_GetWindowLongPtrW(Marshaler& m);
    /// Get/SetWindowLong(Ptr)A/W. @p ptr_api selects the Ptr form (8-byte
    /// values and the pointer indices on x64); @p ansi the A form.
    void window_long_get(Marshaler& m, bool ptr_api, bool ansi);
    void window_long_set(Marshaler& m, bool ptr_api, bool ansi);
    /// Get/SetClassLong(Ptr)/ClassWord; @p width is 2, 4 or the pointer size.
    void class_long_get(Marshaler& m, u32 width);
    void class_long_set(Marshaler& m, u32 width);
    /// GetClassNameA/W.
    void get_class_name(Marshaler& m, bool ansi);
    /// The class record a window belongs to (by registration serial).
    WindowClassData* window_class(const windowing::WindowData& win);
    /// GetClassName's answer for a window.
    std::wstring window_class_name(windowing::HWND hwnd,
                                   const windowing::WindowData& win);
    /// SetWindowLong(GWL_STYLE / GWL_EXSTYLE): the stored bits, with
    /// WM_STYLECHANGING / WM_STYLECHANGED. False when the change is refused
    /// (last error set).
    bool change_window_style(windowing::HWND hwnd, bool ex, u32 requested,
                             u32& previous);
    [[nodiscard]] bool window_long_index_valid(i32 index, bool ptr_api) const;
    /// HMENU attached to a top-level window (GetMenu / GWLP_ID), or 0.
    [[nodiscard]] u64 window_menu(windowing::HWND hwnd) const;
    /// IsMenu: a live menu handle.
    [[nodiscard]] bool is_menu_handle(u32 hmenu) const;

    // ----- Menu model (menus/menu.cpp) -----
    struct MenuItemRecord {
        u32 type = 0;      ///< MFT_*
        u32 state = 0;     ///< MFS_* / MF_GRAYED | MF_DISABLED | MF_CHECKED ...
        u32 id = 0;
        u32 submenu = 0;   ///< HMENU of the popup, 0 for a command item
        u64 checked_bitmap = 0, unchecked_bitmap = 0, data = 0, bitmap = 0, bitmap_item = 0;
        std::wstring text;
    };
    struct MenuRecord {
        std::vector<MenuItemRecord> items;
        bool popup = false;  ///< CreatePopupMenu (not a menu bar)
        u32 style = 0, max_height = 0, help_id = 0;
        u64 background = 0, menu_data = 0;
    };
    MenuRecord* menu_record(u32 hmenu);
    u32 new_menu(bool popup);
    bool find_menu_item(u32 hmenu, u32 item, bool by_position, u32& holder, usize& index,
                        u32 depth);
    void destroy_menu_tree(u32 hmenu, u32 depth);
    windowing::Win32MenuBar menu_to_bar(u32 hmenu, u32 depth);
    bool menu_contains(u32 root, u32 hmenu, u32 depth);
    void menu_changed(u32 hmenu);
    [[nodiscard]] u32 menu_item_state_flags(const MenuItemRecord& item) const;
    bool menu_legacy_edit(Marshaler& m, u32 hmenu, bool modify, bool append, u32 position,
                          u32 flags, u64 new_id, GuestAddr new_item, bool ansi);
    void remove_menu_item(Marshaler& m, bool destroy_submenu);
    bool menu_item_info(Marshaler& m, int op, bool ansi);
    u32 menu_string(Marshaler& m, bool ansi);
    bool menu_info(Marshaler& m, bool set);
    void track_popup_menu(Marshaler& m, bool ex);
    u32 load_menu_from_data(const u8* data, u32 size);
    void load_menu(Marshaler& m, bool ansi);
    /// DestroyWindow: the window's menu bar and window menu go with it.
    void window_menus_destroyed(windowing::HWND hwnd);
    void impl_InsertMenuA(Marshaler& m);
    void impl_InsertMenuItemA(Marshaler& m);
    void impl_SetMenuItemInfoA(Marshaler& m);
    void impl_GetMenuItemInfoA(Marshaler& m);
    void impl_GetMenuStringA(Marshaler& m);
    void impl_LoadMenuA(Marshaler& m);
    void impl_DeleteMenu(Marshaler& m);
    void impl_GetMenuInfo(Marshaler& m);
    void impl_SetMenuInfo(Marshaler& m);
    void impl_GetMenu(Marshaler& m);
    void impl_DrawMenuBar(Marshaler& m);
    void impl_ShutdownBlockReasonCreate(Marshaler& m);
    void impl_ShutdownBlockReasonQuery(Marshaler& m);
    void impl_ShutdownBlockReasonDestroy(Marshaler& m);
    std::mutex m_shutdown_reason_mutex;
    std::unordered_map<windowing::HWND, std::wstring> m_shutdown_reasons;
    std::map<u32, MenuRecord> m_menu_records;
    mutable std::recursive_mutex m_menu_mutex;
    /// SetMenu's model update (menu bar install, non-client height).
    bool set_window_menu(windowing::HWND hwnd, u32 hmenu);
    /// A window this process knows (local) or a peer published (registry).
    [[nodiscard]] bool window_exists(windowing::HWND hwnd) const;
    /// Per-window gesture configuration (want, block) for GID_ZOOM..
    /// GID_PRESSANDTAP (3..7), set by SetGestureConfig; absent = 0/0.
    struct GestureConfigEntry {
        u32 want = 0;
        u32 block = 0;
    };
    std::map<windowing::HWND, std::map<u32, GestureConfigEntry>> m_gesture_configs;
    std::mutex m_gesture_mutex;
    void impl_FlashWindow(Marshaler& m);
    void impl_PrintWindow(Marshaler& m);
    void impl_GetGuiResources(Marshaler& m);
    void impl_ChildWindowFromPoint(Marshaler& m);
    enum class PropOp { Set, Get, Remove };
    /// SetProp / GetProp / RemoveProp (A/W).
    void prop_call(Marshaler& m, PropOp op, bool ansi);
    /// DestroyWindow removes the window's property list.
    void drop_window_props(windowing::HWND hwnd);
    /// DestroyWindow: forget per-window state kept here (properties, the
    /// window's menu association, the pre-subclass procedure).
    void window_destroyed(windowing::HWND hwnd);
    /// Per-thread guest scratch for structures passed to window procedures.
    void set_thread_scratch(GuestAddr base, u32 size) {
        m_thread_scratch.configure(base, size);
    }
    void impl_IsWindow(Marshaler& m);
    void impl_IsWindowEnabled(Marshaler& m);
    void impl_LoadBitmapW(Marshaler& m);
    void impl_LoadImageW(Marshaler& m);
    void impl_GetPropA(Marshaler& m);
    void impl_GetPropW(Marshaler& m);
    void impl_SetPropA(Marshaler& m);
    void impl_SetPropW(Marshaler& m);
    void impl_RemovePropA(Marshaler& m);
    void impl_RemovePropW(Marshaler& m);
    void impl_GetDialogBaseUnits(Marshaler& m);
    void impl_MapDialogRect(Marshaler& m);
    void impl_MapWindowPoints(Marshaler& m);
    void impl_DrawFocusRect(Marshaler& m);
    void impl_InvertRect(Marshaler& m);
    [[nodiscard]] u32 bitmap_miss_error(GuestAddr module_base) const;
    void impl_KillTimer(Marshaler& m);
    void impl_MessageBoxIndirectW(Marshaler& m);
    void impl_OpenClipboard(Marshaler& m);
    void impl_ReleaseDC(Marshaler& m);
    void impl_ScreenToClient(Marshaler& m);
    void impl_SetClassLongW(Marshaler& m);
    void impl_SystemParametersInfoW(Marshaler& m);
    void impl_SystemParametersInfoForDpi(Marshaler& m);
    /// SystemParametersInfo{A,W,ForDpi} (dpi 0 with @p for_dpi false for the
    /// process metrics). Returns the BOOL result; sets the last error itself.
    bool spi_call(Marshaler& m, bool wide, u32 dpi, bool for_dpi);
    void impl_SetClipboardData(Marshaler& m);
    void impl_SetClipboardViewer(Marshaler& m);
    void impl_SetCursor(Marshaler& m);
    /// Apply an HCURSOR to host visibility state (DefWindowProc WM_SETCURSOR).
    void apply_cursor(u32 hcursor);
    /// Last HCURSOR passed to SetCursor; 0 means the guest asked to hide.
    u32 m_current_cursor{1};
    /// Last custom (non-stock) HCURSOR from CreateIconIndirect / CreateCursor.
    /// SetCursor(NULL) and SetCursor(IDC_ARROW) keep this shape — AppKit
    /// cannot hide reliably, and the loading-screen SETCURSOR/arrow path
    /// would otherwise replace the game pointer with the macOS arrow.
    u32 m_last_custom_cursor{0};
    /// HCURSOR whose pixels are currently installed as the host cursor, or 0
    /// for none/stock. Reinstalling an RGBA cursor is a synchronous hop onto
    /// the Cocoa main thread plus a fresh image build, and the software-cursor
    /// path drives SetCursor(NULL) once per frame -- so the shape is only
    /// pushed to the host when it actually differs from what is up.
    u32 m_installed_custom_cursor{0};
    void impl_SetTimer(Marshaler& m);
    void impl_SetWindowLongW(Marshaler& m);
    void impl_SetWindowLongPtrW(Marshaler& m);
    void impl_SetWindowPos(Marshaler& m);
    /// Apply SetWindowPos to a locally owned HWND. Used by the USER32 export
    /// and by the process-local RPC that peers use after SetParent.
    [[nodiscard]] bool apply_set_window_pos(windowing::HWND hwnd,
                                            windowing::HWND insert_after,
                                            i32 x, i32 y, i32 cx, i32 cy,
                                            u32 flags,
                                            bool send_messages = true,
                                            bool host_geometry_applied = false);
    /// SetWindowPos flag bits Windows accepts; any other bit fails with
    /// ERROR_INVALID_FLAGS (measured bit by bit, s1win_msgseq2_probe
    /// "-- swp flag bits": bits 0-10, 13, 14 and 17).
    static constexpr u32 kValidSetWindowPosFlags = 0x000267FFu;
    /// One SetWindowPos transaction, split so EndDeferWindowPos can send
    /// every WM_WINDOWPOSCHANGING before any geometry changes.
    struct SwpTransaction {
        windowing::HWND hwnd{0};
        windowing::HWND after{0};
        i32 x{0}, y{0}, cx{0}, cy{0};
        u32 flags{0};
        bool send{true};
        bool changing_sent{false};
        bool host_geometry_applied{false};
        bool dead{false};
        bool was_visible{false};
        bool was_effectively_visible{false};
        windowing::Rect old_rect{};
        windowing::Rect old_client{};
        windowing::Point old_client_offset{};
        u32 old_ex{0};
        std::vector<windowing::HWND> old_siblings;
        windowing::Rect new_rect{};
        bool outer_moved{false};
        bool outer_sized{false};
        bool z_changed{false};
        /// SetParent's placement: the window moved on screen with its new
        /// parent even if its parent-relative position is unchanged.
        bool reparented{false};
    };
    /// SetParent's SetWindowPos(SWP_NOSIZE | SWP_NOZORDER) at the window's
    /// parent-relative position in its new parent.
    void reposition_after_reparent(windowing::HWND hwnd);
    bool swp_changing(SwpTransaction& txn);
    void swp_read_back(SwpTransaction& txn, GuestAddr wp_addr);
    bool swp_apply(SwpTransaction& txn);
    void swp_finish(SwpTransaction& txn);
    void swp_notify(const SwpTransaction& txn, u32 msg, u64 wParam, u64 lParam);
    /// IsWindowVisible semantics: WS_VISIBLE on the window and its ancestors.
    [[nodiscard]] bool effectively_visible(windowing::HWND hwnd) const;
    /// ShowWindow: every SW_* command as Windows sequences it. Returns the
    /// ShowWindow return value.
    u64 show_window_cmd(windowing::HWND hwnd, int cmd);
    /// The rectangle SW_MAXIMIZE gives @p hwnd (after WM_GETMINMAXINFO).
    windowing::Rect maximized_rect(windowing::HWND hwnd);
    /// Owned windows hide when their owner is minimized and return when it
    /// is restored.
    /// CreateWindowEx's WS_MINIMIZE / WS_MAXIMIZE, applied after WM_CREATE.
    void apply_initial_state(windowing::HWND hwnd, bool minimize);
    void owned_windows_follow_owner(windowing::HWND owner, bool owner_minimized);
    void impl_CloseWindow(Marshaler& m);
    void impl_OpenIcon(Marshaler& m);
    /// Apply SetParent to a locally owned child HWND (owner-side of a
    /// cross-process SetParent). On success, @p previous receives the prior
    /// parent (Win32 SetParent return value).
    [[nodiscard]] bool apply_set_parent(windowing::HWND child,
                                        windowing::HWND parent,
                                        windowing::HWND& previous);
    void impl_MoveWindow(Marshaler& m);
    void impl_GetParent(Marshaler& m);
    void impl_GetAncestor(Marshaler& m);
    void impl_TrackPopupMenu(Marshaler& m);
    void impl_wsprintfA(Marshaler& m);
    void impl_wsprintfW(Marshaler& m);
    void impl_wvsprintfA(Marshaler& m);
    void impl_wvsprintfW(Marshaler& m);
    void impl_AllowSetForegroundWindow(Marshaler& m);
    void impl_EnumChildWindows(Marshaler& m);
    void impl_GetMonitorInfoW(Marshaler& m);
    void impl_GetProcessWindowStation(Marshaler& m);
    void impl_CreateWindowStationW(Marshaler& m);
    void impl_OpenWindowStationW(Marshaler& m);
    void impl_SetProcessWindowStation(Marshaler& m);
    void impl_CloseWindowStation(Marshaler& m);
    void impl_GetThreadDesktop(Marshaler& m);
    void impl_CreateDesktopW(Marshaler& m);
    void impl_SetThreadDesktop(Marshaler& m);
    void impl_SwitchDesktop(Marshaler& m);
    void impl_GetUserObjectSecurity(Marshaler& m);
    void impl_SetUserObjectSecurity(Marshaler& m);
    void impl_GetUserObjectInformationW(Marshaler& m);
    void impl_GetUserObjectInformationA(Marshaler& m);
    void impl_MonitorFromPoint(Marshaler& m);
    void impl_MonitorFromRect(Marshaler& m);
    void impl_MonitorFromWindow(Marshaler& m);
    void impl_EnumDisplayDevicesW(Marshaler& m);
    void impl_EnumDisplayMonitors(Marshaler& m);
    void impl_GetDisplayConfigBufferSizes(Marshaler& m);
    void impl_QueryDisplayConfig(Marshaler& m);
    void impl_DisplayConfigGetDeviceInfo(Marshaler& m);
    void impl_GetMonitorInfoA(Marshaler& m);
    void impl_PostThreadMessageW(Marshaler& m);
    void impl_RedrawWindow(Marshaler& m);
    void impl_SetDlgItemInt(Marshaler& m);

    // ----- Minesweeper / classic game stubs -----
    void impl_CheckMenuItem(Marshaler& m);
    void impl_CheckMenuRadioItem(Marshaler& m);
    void impl_GetSubMenu(Marshaler& m);
    void impl_SetMenu(Marshaler& m);
    void impl_LoadMenuW(Marshaler& m);
    void impl_ReleaseCapture(Marshaler& m);
    void impl_SetCapture(Marshaler& m);
    void impl_WinHelpW(Marshaler& m);
    void impl_GetMenuItemRect(Marshaler& m);
    void impl_TranslateAcceleratorW(Marshaler& m);
    void impl_LoadAcceleratorsW(Marshaler& m);
    void impl_LoadAcceleratorsA(Marshaler& m);
    /// Load an RT_ACCELERATOR table from the module's resources.
    /// @return the HACCEL handle, or 0 when the resource is absent/malformed
    [[nodiscard]] u32 load_accelerator_table(Marshaler& m, u32 hInstance,
                                            u32 lpTableName, bool wide);

    /// Resolve an HINSTANCE to the resource directory to search. Falls back to
    /// the main module for NULL or unknown handles, matching Windows' use of
    /// the process image when hInstance is NULL.
    void resolve_module_resources(u32 hInstance, GuestAddr& base_out,
                                  u32& rva_out, u32& size_out) const;

    /// Attach the module manager so per-module resources can be resolved.
    void set_module_manager(ModuleManager* mgr) noexcept { m_module_mgr = mgr; }

    /// Show/hide the native macOS cursor (used by ShowCursor to mirror the
    /// Win32 cursor-display count onto the host).
    void set_native_cursor_visible(bool visible);
    /// Match a keyboard message against a loaded table, posting WM_COMMAND on
    /// a hit. @return true when the message was translated
    [[nodiscard]] bool translate_accelerator(u32 hwnd, u32 haccel,
                                             const windowing::MSG& msg);

    // ----- Scroll APIs -----
    void impl_EnableScrollBar(Marshaler& m);
    void impl_ScrollWindow(Marshaler& m);
    void impl_SetScrollRange(Marshaler& m);
    void impl_GetScrollRange(Marshaler& m);
    void impl_ShowScrollBar(Marshaler& m);
    void impl_GetScrollInfo(Marshaler& m);
    void impl_SetScrollInfo(Marshaler& m);
    void impl_GetScrollPos(Marshaler& m);
    void impl_SetScrollPos(Marshaler& m);

    // ----- Menu APIs -----
    void impl_GetMenuDefaultItem(Marshaler& m);
    void impl_SetMenuDefaultItem(Marshaler& m);
    void impl_IsMenu(Marshaler& m);
    void impl_InsertMenuItemW(Marshaler& m);
    void impl_DestroyMenu(Marshaler& m);
    void impl_GetMenuItemInfoW(Marshaler& m);
    void impl_ShowOwnedPopups(Marshaler& m);
    void impl_GetMenuState(Marshaler& m);
    void impl_GetMenuStringW(Marshaler& m);
    void impl_GetMenuItemID(Marshaler& m);
    void impl_InsertMenuW(Marshaler& m);
    void impl_GetMenuItemCount(Marshaler& m);
    void impl_SetMenuItemBitmaps(Marshaler& m);
    void impl_GetMenuCheckMarkDimensions(Marshaler& m);
    void impl_CreateMenu(Marshaler& m);
    void impl_ModifyMenuA(Marshaler& m);
    void impl_ModifyMenuW(Marshaler& m);
    void impl_RemoveMenu(Marshaler& m);

    // ----- Keyboard/Input APIs -----
    void impl_GetKeyboardState(Marshaler& m);
    void impl_SetKeyboardState(Marshaler& m);
    void impl_GetKeyboardLayout(Marshaler& m);
    void impl_ToUnicodeEx(Marshaler& m);
    void impl_MapVirtualKeyW(Marshaler& m);
    void impl_GetKeyNameTextW(Marshaler& m);
    void impl_GetAsyncKeyState(Marshaler& m);
    void impl_MapVirtualKeyExW(Marshaler& m);
    void impl_GetFocus(Marshaler& m);
    /// Return GetKeyState's two observable bits from the calling thread's
    /// input-state table: bit 15 means down and bit 0 means toggled.
    [[nodiscard]] i16 thread_key_state(u32 virtual_key);
    /// True when threads @p a and @p b share an input queue: the same thread,
    /// or joined (transitively) by AttachThreadInput.
    [[nodiscard]] bool shares_input_queue(u32 a, u32 b) const;
    /// True when @p hwnd belongs to the calling thread's input queue. Focus,
    /// activation and capture are per queue: GetFocus and friends report
    /// another queue's window as NULL (s2msg_input_probe setfocus_other_*).
    [[nodiscard]] bool window_in_caller_queue(windowing::HWND hwnd) const;
    /// The calling thread's 256-byte key state (GetKeyboardState).
    [[nodiscard]] std::array<u8, 256> thread_keyboard_state();
    /// Advance the calling thread's input-state table when a keyboard message
    /// is removed from its queue.
    void note_retrieved_keyboard_message(
        const windowing::MSG& message) noexcept;
    /// Record the last retrieved message timestamp for GetMessageTime.
    /// Per guest thread: PeekMessage (including PM_NOREMOVE) and GetMessage
    /// both update this. Only removal advances keyboard state.
    void note_retrieved_queue_message(
        const windowing::MSG& message, bool removed = true) noexcept;
    /// GetMessageTime for the calling guest thread. 0 until that thread
    /// has retrieved a message.
    [[nodiscard]] u32 last_message_time() const noexcept;
    [[nodiscard]] u32 last_message_pos() const noexcept;

    // ----- Accelerator Table APIs -----
    void impl_CreateAcceleratorTableW(Marshaler& m);
    void impl_CopyAcceleratorTableW(Marshaler& m);
    void impl_DestroyAcceleratorTable(Marshaler& m);

    // ----- Icon/Cursor APIs -----
    void impl_CopyIcon(Marshaler& m);
    void impl_GetIconInfo(Marshaler& m);
    void impl_DrawIconEx(Marshaler& m);
    void impl_CopyImage(Marshaler& m);
    void impl_DrawIcon(Marshaler& m);
    void impl_DestroyIcon(Marshaler& m);
    void impl_CreateCursor(Marshaler& m);
    void impl_DestroyCursor(Marshaler& m);

    // ----- Clipboard APIs -----
    void impl_RegisterClipboardFormatW(Marshaler& m);
    void impl_RegisterClipboardFormatA(Marshaler& m);
    void impl_IsClipboardFormatAvailable(Marshaler& m);

    // ----- DDE APIs -----
    void impl_PackDDElParam(Marshaler& m);
    void impl_UnpackDDElParam(Marshaler& m);
    void impl_FreeDDElParam(Marshaler& m);
    void impl_ReuseDDElParam(Marshaler& m);
    /// PackDDElParam core (dde_timer_msg.cpp), shared with ReuseDDElParam.
    u64 dde_pack_lparam(Marshaler& m, u32 msg, u64 lo, u64 hi);
    void impl_GetComboBoxInfo(Marshaler& m);

    // ----- Window Management APIs -----
    void impl_UpdateLayeredWindow(Marshaler& m);
    void impl_UpdateLayeredWindowIndirect(Marshaler& m);
    void impl_SetLayeredWindowAttributes(Marshaler& m);
    void impl_GetLayeredWindowAttributes(Marshaler& m);
    /// How a WS_EX_LAYERED window is being composed. Learn
    /// (SetLayeredWindowAttributes): once the attributes are set,
    /// UpdateLayeredWindow fails until WS_EX_LAYERED is cleared and set again;
    /// measured (s1win_dwm_probe "-- layered") both ways.
    struct LayeredState {
        enum class Mode : u8 { none, attributes, update };
        Mode mode = Mode::none;
        u32 key = 0;
        u8 alpha = 0;
        u32 flags = 0;
    };
    std::mutex m_layered_mutex;
    std::unordered_map<windowing::HWND, LayeredState> m_layered;
    /// WS_EX_LAYERED was cleared (or the window died): forget the layering
    /// and give the window back its full opacity.
    void layered_style_cleared(windowing::HWND hwnd);
    /// Shared body of UpdateLayeredWindow / UpdateLayeredWindowIndirect.
    bool update_layered_window(Marshaler& m, u32 hwnd, u32 hdc_src,
                               GuestAddr ppt_dst, GuestAddr psize,
                               GuestAddr ppt_src, GuestAddr pblend,
                               u32 flags, const char* api_tag);
    void impl_SetParent(Marshaler& m);
    void impl_LockWindowUpdate(Marshaler& m);
    void impl_BringWindowToTop(Marshaler& m);
    void impl_SetWindowRgn(Marshaler& m);
    void impl_GetWindowRgn(Marshaler& m);
    void impl_SetWindowPlacement(Marshaler& m);
    void impl_BeginDeferWindowPos(Marshaler& m);
    void impl_DeferWindowPos(Marshaler& m);
    void impl_EndDeferWindowPos(Marshaler& m);
    void impl_GetActiveWindow(Marshaler& m);
    /// Make @p next the active window, sending the activation messages in
    /// the order Windows does (tools/oracle_probe/activation_order_probe.c):
    /// WM_ACTIVATEAPP(TRUE) to every top-level when the application gains
    /// activation; WM_NCACTIVATE(FALSE) and WM_ACTIVATE(WA_INACTIVE) to the
    /// previous window; WM_NCACTIVATE(TRUE) and WM_ACTIVATE(@p state) to
    /// @p next, whose DefWindowProc then takes keyboard focus. A NULL
    /// @p next deactivates the application: the previous window is
    /// deactivated, WM_ACTIVATEAPP(FALSE) is sent and focus is killed.
    /// Returns false when nothing changed.
    bool change_active_window(windowing::HWND next, u32 state,
                              u32 other_thread_id = 0);
    /// Before @p hwnd is hidden, minimized or destroyed: if it is the active
    /// window, activate the next visible, enabled top-level window in z-order
    /// (its owner first), or clear activation and focus when none remains.
    /// Clears focus held by a window inside @p hwnd when @p destroying.
    void release_activation(windowing::HWND hwnd, bool destroying);
    /// DefDlgProc WM_ACTIVATE default (Learn DefDlgProc): on deactivation
    /// save the control inside @p dialog that has focus; on activation
    /// restore focus to the saved control.
    void dialog_activate_default(windowing::HWND dialog, bool activating);
    /// Activation caused by an activating USER32 operation (ShowWindow,
    /// visible creation).
    bool activate_application(windowing::HWND hwnd,
                              u32 other_thread_id = 0);
    /// While alive on the calling thread, the activation and focus messages
    /// this manager delivers to a window owned by ANOTHER thread are queued
    /// as sent messages (SendNotifyMessage semantics, Learn
    /// nf-winuser-sendnotifymessagew) instead of awaited. For changes AppKit
    /// originates on the host main thread, which is not a guest thread and
    /// must never block on one: Windows' own foreground switch does not wait
    /// for an application that is not pumping -- a GUI thread blocked in
    /// WaitForSingleObject receives WM_NCACTIVATE/WM_ACTIVATE/WM_ACTIVATEAPP/
    /// WM_KILLFOCUS when it next checks its queue, sent messages first. A
    /// window of the calling thread still gets them synchronously.
    class NativeDeliveryScope {
    public:
        NativeDeliveryScope() noexcept;
        ~NativeDeliveryScope();
        NativeDeliveryScope(const NativeDeliveryScope&) = delete;
        NativeDeliveryScope& operator=(const NativeDeliveryScope&) = delete;

    private:
        bool m_previous;
    };

    /// Apply a native activation (@p active) or application deactivation.
    /// @p by_mouse selects WA_CLICKACTIVE for the WM_ACTIVATE state.
    bool set_application_active(windowing::HWND hwnd, bool active,
                                u32 other_thread_id = 0,
                                bool by_mouse = false);
    /// A native click made top-level @p top key while another window is
    /// active. Learn WM_MOUSEACTIVATE: the window under the cursor is asked
    /// first, on its own thread. Returns its MA_* answer when the caller is
    /// that thread (the caller then activates unless it is MA_NOACTIVATE*);
    /// otherwise returns kMouseActivateDeferred and the owner thread decides,
    /// before the click's queued input: it activates @p top, or calls
    /// @p on_declined with the window that stays active.
    static constexpr u32 kMouseActivateDeferred = 0x10000;
    u32 native_mouse_activate(windowing::HWND top, u32 mouse_message,
                              std::function<void(windowing::HWND)> on_declined);
    /// Apply a native keyboard-focus transition and synchronously send the
    /// corresponding WM_SETFOCUS/WM_KILLFOCUS notification.
    bool set_keyboard_focus(windowing::HWND hwnd, bool focused);
    /// Translate an AppKit key-window transition into Win32 input-queue
    /// focus. Deactivation remembers the actual focused descendant; a later
    /// activation of the same top-level window restores that descendant.
    bool set_native_keyboard_focus(windowing::HWND top_level, bool focused);
    void impl_GetTopWindow(Marshaler& m);
    [[nodiscard]] bool application_active() const noexcept {
        return m_application_active;
    }
    void impl_GetWindow(Marshaler& m);
    void impl_GetLastActivePopup(Marshaler& m);
    void impl_SetActiveWindow(Marshaler& m);
    void impl_WindowFromPoint(Marshaler& m);
    void impl_ChildWindowFromPointEx(Marshaler& m);

    // ----- Timer/Message APIs -----
    void impl_GetDoubleClickTime(Marshaler& m);
    void impl_GetLastInputInfo(Marshaler& m);
    void impl_GetMessageTime(Marshaler& m);
    void impl_NotifyWinEvent(Marshaler& m);
    void impl_RegisterWindowMessageW(Marshaler& m);
    void impl_RegisterWindowMessageA(Marshaler& m);
    void impl_RegisterShellHookWindow(Marshaler& m);
    void impl_DeregisterShellHookWindow(Marshaler& m);

    /// Register a process-local representation of a system-wide registered
    /// message. Repeated registration of the same string returns the same ID.
    [[nodiscard]] u32 register_window_message(const std::string& name);

    /// Shell-hook registration and delivery used by window lifecycle code.
    [[nodiscard]] bool register_shell_hook_window(windowing::HWND hwnd);
    [[nodiscard]] bool deregister_shell_hook_window(windowing::HWND hwnd);
    void notify_shell_event(u32 event, windowing::HWND subject);

    /// Apply or clear a virtual exclusive display mode (DirectDraw
    /// SetDisplayMode / ChangeDisplaySettings). While active, GetSystemMetrics
    /// SM_CXSCREEN/SM_CYSCREEN, the desktop HWND, and the target window's
    /// client rect report the logical mode rather than the host display size.
    /// Pass width/height 0 to restore the physical desktop metrics.
    void apply_virtual_display_mode(windowing::HWND hwnd, u32 width, u32 height,
                                    u32 bits_per_pixel = 0);
    [[nodiscard]] bool is_virtual_fullscreen_window(windowing::HWND hwnd) const {
        return hwnd != windowing::NULL_HWND && hwnd == m_virtual_fullscreen_hwnd;
    }
    /// The logical display mode apply_virtual_display_mode published, or
    /// nullopt while the physical desktop mode is current.
    [[nodiscard]] std::optional<std::pair<u32, u32>> virtual_display_size() const {
        if (m_virtual_display_width <= 0 || m_virtual_display_height <= 0) {
            return std::nullopt;
        }
        return std::pair<u32, u32>{static_cast<u32>(m_virtual_display_width),
                                   static_cast<u32>(m_virtual_display_height)};
    }

    /// DXGI exclusive full-screen on the same owner DirectDraw and D3D9 use.
    /// With @p change_mode the logical mode width x height is published
    /// exactly as apply_virtual_display_mode does (metrics, WM_DISPLAYCHANGE,
    /// the window sized to the mode, WM_SIZE). Without it the display keeps
    /// its mode (no WM_DISPLAYCHANGE) and the window only covers @p output,
    /// with WM_SIZE for that size. False when @p hwnd is not a window.
    [[nodiscard]] bool enter_exclusive_display(windowing::HWND hwnd,
                                               const windowing::Rect& output,
                                               u32 width, u32 height,
                                               bool change_mode);
    /// Leave DXGI exclusive full-screen: when @p mode_changed, restore the
    /// desktop mode and broadcast its WM_DISPLAYCHANGE (Learn: sent to all
    /// windows when the display resolution has changed); then put the window
    /// back at @p restore (SetWindowPos, WM_SIZE for the client it gets).
    void leave_exclusive_display(windowing::HWND hwnd, bool mode_changed,
                                 const std::optional<windowing::Rect>& restore);
    /// The input-queue focus window (GetFocus of the active application).
    [[nodiscard]] windowing::HWND keyboard_focus_window() const noexcept {
        return m_focus_hwnd;
    }

    // ----- Rect Utility APIs -----
    void impl_GetUpdateRect(Marshaler& m);
    void impl_GetUpdateRgn(Marshaler& m);
    void impl_SetRectEmpty(Marshaler& m);
    void impl_AdjustWindowRectEx(Marshaler& m);
    void impl_EqualRect(Marshaler& m);
    void impl_SubtractRect(Marshaler& m);

    // ----- Class APIs -----
    void impl_GetClassLongW(Marshaler& m);
    void impl_GetClassLongPtrW(Marshaler& m);
    void impl_GetClassNameW(Marshaler& m);
    void impl_IsChild(Marshaler& m);
    void impl_GetDlgCtrlID(Marshaler& m);
    void impl_SetClassLongPtrW(Marshaler& m);

    // ----- Dialog APIs -----
    void impl_CreateDialogIndirectParamA(Marshaler& m);
    void impl_CreateDialogIndirectParamW(Marshaler& m);
    void impl_GetNextDlgTabItem(Marshaler& m);
    void impl_GetNextDlgGroupItem(Marshaler& m);

    // ----- Hook APIs -----
    void impl_SetWindowsHookExW(Marshaler& m);
    void impl_CallNextHookEx(Marshaler& m);
    void impl_UnhookWindowsHookEx(Marshaler& m);

    // ----- Drawing APIs -----
    void impl_GetSysColorBrush(Marshaler& m);
    void impl_DrawStateW(Marshaler& m);
    void impl_DrawStateA(Marshaler& m);
    /// DrawStateW/A (drawing_ext.cpp); `wide` selects the DST_TEXT encoding.
    void draw_state_impl(Marshaler& m, bool wide);
    void impl_DrawTextExA(Marshaler& m);
    void impl_DrawTextExW(Marshaler& m);
    void impl_TabbedTextOutA(Marshaler& m);
    void impl_TabbedTextOutW(Marshaler& m);
    void impl_GrayStringA(Marshaler& m);
    void impl_GrayStringW(Marshaler& m);
    void draw_text_ex_impl(Marshaler& m, bool wide);
    void gray_string_impl(Marshaler& m, bool wide);
    void impl_DrawEdge(Marshaler& m);
    void impl_DrawFrameControl(Marshaler& m);
    void impl_GetWindowDC(Marshaler& m);

    // ----- MDI APIs -----
    void impl_DefFrameProcW(Marshaler& m);
    void impl_DefMDIChildProcW(Marshaler& m);
    void impl_TranslateMDISysAccel(Marshaler& m);

    // ----- Character APIs -----
    void impl_CharUpperBuffW(Marshaler& m);
    void impl_IsCharLowerW(Marshaler& m);

    // ----- System Parameters -----
    void impl_SystemParametersInfoA(Marshaler& m);

    // ===== Additional Win32 stubs =====
    void impl_TrackMouseEvent(Marshaler& m);
    void impl_AttachThreadInput(Marshaler& m);
    void impl_ClipCursor(Marshaler& m);
    void impl_GetClipCursor(Marshaler& m);
    void impl_SetMenuItemInfoW(Marshaler& m);
    void impl_RegisterDeviceNotificationW(Marshaler& m);
    void impl_UnregisterDeviceNotification(Marshaler& m);
    void impl_RegisterPowerSettingNotification(Marshaler& m);
    void impl_UnregisterPowerSettingNotification(Marshaler& m);
    void impl_RegisterSuspendResumeNotification(Marshaler& m);
    void impl_UnregisterSuspendResumeNotification(Marshaler& m);
    void impl_SetProcessDpiAwarenessContext(Marshaler& m);
    void impl_SetProcessDPIAware(Marshaler& m);
    void impl_SetWinEventHook(Marshaler& m);
    void impl_UnhookWinEvent(Marshaler& m);
    void notify_win_event(u32 event, windowing::HWND hwnd,
                          i32 object_id = 0, i32 child_id = 0);
    /// Deliver queued WINEVENT_OUTOFCONTEXT callbacks for @p thread_id.
    /// Microsoft Learn: out-of-context events arrive on the SetWinEventHook
    /// thread via its message loop.
    void drain_pending_win_events(u32 thread_id);
    void drain_pending_native_resizes(u32 thread_id);
    /// Microsoft Learn GetMsgProc: after GetMessage/PeekMessage retrieves a
    /// message, call matching WH_GETMESSAGE hooks with HC_ACTION, wParam =
    /// PM_REMOVE or PM_NOREMOVE, and lParam pointing at the guest MSG.
    void dispatch_get_message_hook(GuestAddr msg_ptr, u32 remove_flags);
    /// WH_CALLWNDPROC (@p after false) / WH_CALLWNDPROCRET around a message
    /// SendMessage delivers to a window procedure on the calling thread.
    void dispatch_call_wndproc_hooks(windowing::HWND hwnd, u32 message,
                                     u64 wparam, u64 lparam, bool cross_thread,
                                     bool after, u64 lresult);
    /// WH_KEYBOARD / WH_MOUSE for hardware input GetMessage/PeekMessage is
    /// about to return. True when a hook discarded the message.
    bool dispatch_input_hooks(const windowing::MSG& msg, bool removed);
    /// WH_KEYBOARD_LL (@p keyboard) / WH_MOUSE_LL installed?
    [[nodiscard]] bool low_level_hooks_installed(bool keyboard) const;
    /// Thread that installed the newest low-level hook of the kind.
    [[nodiscard]] std::optional<u32> low_level_hook_thread(bool keyboard) const;
    /// Run the low-level chain for one input event on the calling thread.
    /// True when a hook swallowed it.
    bool run_low_level_hooks(const windowing::MSG& msg);
    /// WH_FOREGROUNDIDLE, called by GetMessage on the foreground thread
    /// before it blocks.
    void dispatch_foreground_idle_hook();
    /// CallMsgFilter's chains (WH_SYSMSGFILTER, then WH_MSGFILTER) for a
    /// modal loop Vitreo runs; @p msg is updated with the hook's edits.
    /// True when a hook processed the message.
    bool call_msg_filter(windowing::MSG& msg, i32 code);
    /// WH_SHELL notification.
    void dispatch_shell_hook(i32 code, u64 wparam, u64 lparam);
    void impl_CallMsgFilterW(Marshaler& m);
    /// Route hardware input through WH_KEYBOARD_LL / WH_MOUSE_LL (Learn: the
    /// hook runs on its installing thread, before the input reaches any
    /// queue; nonzero swallows it). Installed once at startup; costs one
    /// atomic load per input event while no low-level hook exists.
    void install_low_level_input_filter();
    /// Thread liveness for dwThreadId validation (SetWindowsHookEx).
    void set_thread_alive_query(std::function<bool(u32)> query) {
        m_thread_alive = std::move(query);
    }
    void impl_FlashWindowEx(Marshaler& m);
    void impl_OpenInputDesktop(Marshaler& m);
    void impl_OpenDesktopA(Marshaler& m);
    void impl_OpenDesktopW(Marshaler& m);
    /// Open an existing desktop by name (case-insensitive). Returns 0 if missing.
    u64 open_desktop_by_name(const std::string& name);
    /// Open an existing window station by name (case-insensitive). Returns 0 if missing.
    u64 open_window_station_by_name(const std::string& name);
    [[nodiscard]] bool has_desktop_handle(u64 handle) const;
    [[nodiscard]] bool has_window_station_handle(u64 handle) const;
    /// Apply STARTUPINFO.lpDesktop (windowstation\\desktop or desktop).
    /// Ensures named objects exist in this process and attaches to them —
    /// Microsoft Learn CreateProcessAsUserW lpDesktop contract.
    void apply_startup_desktop(std::string_view spec);
    void impl_CloseDesktop(Marshaler& m);
    void impl_EnumDesktopWindows(Marshaler& m);
    void impl_GetShellWindow(Marshaler& m);
    void impl_ChangeDisplaySettingsA(Marshaler& m);
    /// Shared body of EnumDisplaySettingsA/W; @p wide selects the DEVMODEW
    /// field offsets.
    void enum_display_settings(Marshaler& m, bool wide);
    void impl_EnumDisplaySettingsA(Marshaler& m);
    void impl_EnumDisplaySettingsW(Marshaler& m);
    void impl_EnumDisplaySettingsExA(Marshaler& m);
    void impl_EnumDisplaySettingsExW(Marshaler& m);
    void impl_EnumDisplayDevicesA(Marshaler& m);
    void impl_ChangeDisplaySettingsExA(Marshaler& m);
    void impl_ChangeDisplaySettingsExW(Marshaler& m);
    void impl_ChangeDisplaySettingsW(Marshaler& m);
    void impl_DisplayConfigSetDeviceInfo(Marshaler& m);
    /// ChangeDisplaySettingsEx core; returns the DISP_CHANGE_* code.
    i32 change_display_settings(Marshaler& m, GuestAddr device_ptr, GuestAddr devmode,
                                bool wide, u32 flags);
    void enum_display_devices(Marshaler& m, bool wide);
    bool read_device_name(Marshaler& m, GuestAddr ptr, bool wide, std::string& out);
    /// The current mode (device pixels) of guest monitor @p index.
    void monitor_mode(usize index, u32& width, u32& height) const;
    void impl_CreateIconFromResource(Marshaler& m);
    void impl_CreateIconFromResourceEx(Marshaler& m);
    void impl_CreateIconIndirect(Marshaler& m);
    void impl_GetIconInfoEx(Marshaler& m, bool wide);
    /// CreateIconFromResource(Ex) core with the Ex size and flags.
    void create_icon_from_resource(Marshaler& m, i32 cx, i32 cy, u32 flags);
    void impl_DialogBoxIndirectParamA(Marshaler& m);
    void impl_DialogBoxIndirectParamW(Marshaler& m);
    void impl_RegisterRawInputDevices(Marshaler& m);
    void impl_GetRawInputData(Marshaler& m);
    /// Publish one physical keyboard transition to a registered raw-input
    /// consumer. The HRAWINPUT stored in WM_INPUT remains valid until the
    /// consumer removes the next message on that guest thread.
    void note_raw_keyboard_transition(u32 virtual_key, bool down);
    void note_removed_raw_input_message(const windowing::MSG& message);
    void discard_raw_input_for_thread(u32 tid);
    /// Same path for the mouse usage (Generic Desktop / Mouse). Unity's input
    /// system reads clicks from these packets, not from WM_LBUTTON*.
    void note_raw_mouse_button(u32 button, bool down, windowing::HWND event_hwnd);
    /// RAWMOUSE wheel packet: RI_MOUSE_WHEEL (vertical) or RI_MOUSE_HWHEEL
    /// with the signed WHEEL_DELTA-unit delta in usButtonData. Clients that
    /// read the mouse only through raw input never see WM_MOUSEWHEEL.
    void note_raw_mouse_wheel(i32 delta, bool horizontal,
                              windowing::HWND event_hwnd);
    /// Learn (RAWMOUSE): with MOUSE_MOVE_RELATIVE, lLastX/lLastY are the
    /// device's motion since the last event, "not subject to the effects of
    /// the mouse speed" and independent of the cursor. The host passes the
    /// event's device deltas; differencing cursor positions here would turn
    /// every SetCursorPos warp, ClipCursor clamp and screen edge into motion.
    void note_raw_mouse_motion(double device_dx, double device_dy,
                               windowing::HWND event_hwnd);
    void impl_RegisterPointerDeviceNotifications(Marshaler& m);
    void impl_GetPointerDevices(Marshaler& m);
    void impl_GetRawInputDeviceList(Marshaler& m);
    void impl_GetRawInputBuffer(Marshaler& m);
    void impl_GetRawInputDeviceInfoA(Marshaler& m);
    void impl_GetRawInputDeviceInfoW(Marshaler& m);
    void impl_GetMessageExtraInfo(Marshaler& m);
    void impl_SetMessageExtraInfo(Marshaler& m);
    void impl_GetQueueStatus(Marshaler& m);
    void impl_EnumClipboardFormats(Marshaler& m);
    void impl_GetUpdatedClipboardFormats(Marshaler& m);
    /// SetGestureConfig / GetGestureConfig (clipformats_gesture_probe).
    void impl_SetGestureConfig(Marshaler& m);
    void impl_GetGestureConfig(Marshaler& m);
    void impl_GetClipboardData(Marshaler& m);
    void impl_GetClipboardFormatNameA(Marshaler& m);
    void impl_GetClipboardFormatNameW(Marshaler& m);
    void impl_GetClipboardSequenceNumber(Marshaler& m);
    void impl_SendInput(Marshaler& m);
    void impl_GetCursorInfo(Marshaler& m);
    void impl_IsWindowUnicode(Marshaler& m);
    void impl_GetWindowInfo(Marshaler& m);
    void impl_GetClassLongA(Marshaler& m);
    void impl_GetClassNameA(Marshaler& m);
    void impl_ToUnicode(Marshaler& m);
    void impl_RealGetWindowClassW(Marshaler& m);
    void impl_RealGetWindowClassA(Marshaler& m);
    void impl_IsHungAppWindow(Marshaler& m);
    void impl_RegisterTouchWindow(Marshaler& m);
    void impl_UnregisterTouchWindow(Marshaler& m);
    void impl_IsTouchWindow(Marshaler& m);
    void impl_GetTouchInputInfo(Marshaler& m);
    void impl_WaitForInputIdle(Marshaler& m);
    bool wait_message();

    void set_guest_call(GuestCall call) { m_guest_call = std::move(call); }
    void set_gdi_manager(GdiManager* mgr) { m_gdi_mgr = mgr; }
    void set_painting_manager(PaintingManager* mgr) { m_paint_mgr = mgr; }
    void set_message_loop_manager(MessageLoopManager* mgr) { m_msgloop_mgr = mgr; }
    void set_message_send_manager(MessageSendManager* mgr) { m_msgsend_mgr = mgr; }
    /// EDIT WM_COPY / WM_CUT: the clipboard sequence an edit control runs
    /// (open as @p owner, empty, CF_UNICODETEXT, close). False when another
    /// thread holds the clipboard open.
    bool edit_clipboard_copy(Marshaler& m, windowing::HWND owner, const std::u16string& text);
    /// EDIT WM_PASTE: the clipboard's text, or nullopt when there is none or
    /// another thread holds it open.
    [[nodiscard]] std::optional<std::u16string> edit_clipboard_text(Marshaler& m);
    void set_dialog_manager(DialogManager* mgr);
    void set_process_manager(ProcessManager* mgr) { m_process_mgr = mgr; }
    void set_show_window_manager(ShowWindowManager* mgr) { m_show_mgr = mgr; }
    void set_resource_manager(ResourceManager* mgr) { m_resource_mgr = mgr; }
    void set_windowpos_scratch(GuestAddr address) { m_windowpos_scratch = address; }
    ThreadScratch& thread_scratch() noexcept { return m_thread_scratch; }

    /// Auto-load and install a menu bar from a WNDCLASS lpszMenuName.
    /// Called by WindowManager::create_window when hMenu==0 and the class
    /// has a menu resource name.  The name may be a numeric string (from
    /// IS_INTRESOURCE) or a named resource.
    void load_and_install_class_menu(windowing::HWND hwnd, const std::wstring& menu_name);

    /// Accessors for use by inline thunk lambdas
    GuestMemory& memory() { return m_memory; }
    capsule::Vfs& vfs() { return m_vfs; }
    windowing::HwndManager& hwnd_mgr() { return m_hwnd_mgr; }

    /// WM_NCHITTEST against a window THIS process owns. Returns HTCLIENT when
    /// the window has no wndproc here. Callers that may be asking about a
    /// peer-owned HWND must route through the registry instead -- answering
    /// HTCLIENT on another process's behalf reports "no caption anywhere",
    /// which silently disables dragging.
    [[nodiscard]] i32 local_hit_test(windowing::HWND hwnd, i32 screen_x,
                                     i32 screen_y);
    windowing::BitmapHost& bitmap_host() { return m_host; }
    windowing::ControlHost& control_host() { return m_host; }

    void set_resource_info(GuestAddr image_base, u32 rsrc_rva, u32 rsrc_size) {
        m_rsrc_image_base = image_base;
        m_rsrc_rva = rsrc_rva;
        m_rsrc_size = rsrc_size;
    }

    /// One entry of a loaded RT_ACCELERATOR table.
    struct AcceleratorEntry {
        u16 flags;  ///< FVIRTKEY/FSHIFT/FCONTROL/FALT
        u16 key;    ///< virtual key code, or character for non-FVIRTKEY
        u16 cmd;    ///< command id delivered in WM_COMMAND
    };

    GuestAddr rsrc_image_base() const noexcept { return m_rsrc_image_base; }
    u32 rsrc_rva() const noexcept { return m_rsrc_rva; }
    u32 rsrc_size() const noexcept { return m_rsrc_size; }

    /// Current logical-monitor DPI policy, shared with graphics frontends.
    [[nodiscard]] u32 system_dpi() const noexcept;
    bool set_process_dpi_awareness_context(i64 context);
    /// The manifest's <dpiAwareness> / <dpiAware>: the process starts with
    /// that awareness already set.
    void set_manifest_dpi_awareness_context(i64 context);
    void impl_SetProcessDpiAwareness(Marshaler& m);
    void impl_GetProcessDpiAwareness(Marshaler& m);
    void impl_GetDpiForMonitor(Marshaler& m);
    void impl_GetDpiForWindow(Marshaler& m);
    [[nodiscard]] i64 process_dpi_awareness_context() const noexcept {
        return m_process_dpi_context;
    }
    [[nodiscard]] i64 thread_dpi_awareness_context() const;
    [[nodiscard]] i64 set_thread_dpi_awareness_context(i64 context);
    [[nodiscard]] windowing::HWND pointer_notification_window() const noexcept {
        return m_pointer_notification_window;
    }
    [[nodiscard]] bool pointer_notification_in_range_enabled() const noexcept {
        return m_pointer_notification_in_range;
    }

private:
    void recalculate_client_area(windowing::HWND hwnd,
                                 const windowing::Rect& new_outer,
                                 const windowing::Rect& old_outer,
                                 windowing::HWND insert_after,
                                 u32 windowpos_flags);

    GuestMemory& m_memory;
    capsule::Vfs& m_vfs;
    windowing::HwndManager& m_hwnd_mgr;
    WindowClassManager& m_class_mgr;
    windowing::User32Host& m_host;

    windowing::MessageQueue& m_queue;
    GuestCall m_guest_call;
    SystemControlInitializer m_system_control_initializer;
    GdiManager* m_gdi_mgr{nullptr};
    PaintingManager* m_paint_mgr{nullptr};
    MessageLoopManager* m_msgloop_mgr{nullptr};
    MessageSendManager* m_msgsend_mgr{nullptr};
    DialogManager* m_dialog_mgr{nullptr};
    ProcessManager* m_process_mgr{nullptr};
    ShowWindowManager* m_show_mgr{nullptr};
    ResourceManager* m_resource_mgr{nullptr};
    windowing::HWND m_virtual_fullscreen_hwnd{windowing::NULL_HWND};
    bool m_legacy_frame_metrics{false};
    /// SPI_SETSTICKYKEYS / TOGGLEKEYS / FILTERKEYS state (keyed by the GET
    /// action), created from the measured defaults on first use.
    std::mutex m_spi_mutex;
    std::map<u32, std::vector<u8>> m_spi_structs;
    i32 m_virtual_display_width{0};
    i32 m_virtual_display_height{0};
    /// Pieces of an exclusive display-mode change, shared by
    /// apply_virtual_display_mode and enter/leave_exclusive_display.
    void size_window_to_display(windowing::HWND hwnd,
                                const windowing::Rect& rect);
    void broadcast_display_change(u32 bits_per_pixel, u32 width, u32 height);
    void post_display_size(windowing::HWND hwnd, u32 width, u32 height);

    // Resource directory info for menu parsing
    ModuleManager* m_module_mgr{nullptr};
    std::unordered_map<u32, std::vector<AcceleratorEntry>> m_accel_tables;
    u32 m_next_accel_handle{0xACC00001};

    GuestAddr m_rsrc_image_base{0};
    u32 m_rsrc_rva{0};
    u32 m_rsrc_size{0};

    u32 m_next_menu_handle{0xDE000000};
    u32 m_next_accel_table{0xACC00001};
    u32 m_next_cursor_handle{0xCCCC0001};
    u32 m_next_hook_handle{0xFEED0001};
public:
    struct HookRegistration {
        u32 handle{0};
        i32 id_hook{0};
        GuestAddr proc{0};
        u64 module{0};
        u32 thread_id{0};     ///< 0 = global
        u32 owner_thread{0};  ///< the thread that installed it
        bool is_64bit{false};
    };
private:
    /// Installed hooks per type (index idHook + 1), so a dispatch site with
    /// no hook of its type costs one atomic load.
    std::array<std::atomic<u32>, 16> m_hook_type_count{};
    std::function<bool(u32)> m_thread_alive;
    enum class NativeActionKind { Resize, Move, EnterMoveSize, ExitMoveSize, SysCommand };
    struct PendingNativeAction {
        windowing::HWND hwnd;
        u32 thread_id;
        NativeActionKind kind;
        i32 first;
        i32 second;
    };
    void queue_native_action(windowing::HWND hwnd, u32 thread_id,
                             NativeActionKind kind, i32 first = 0, i32 second = 0);
    // Consecutive geometry updates may coalesce, but never across a gesture
    // boundary. Preserve host order for each owning guest thread.
    std::deque<PendingNativeAction> m_pending_native_actions;
    /// The window being placed is where the host already put it (a native
    /// move): the transaction must not push that frame back to the host,
    /// which mid-drag would snap the window to an older position.
    windowing::HWND m_applying_native_move{windowing::NULL_HWND};
    std::mutex m_native_resize_mutex;
    mutable std::mutex m_hook_mutex;
    std::unordered_map<u32, HookRegistration> m_hooks;
    struct WinEventHookRegistration {
        u32 event_min{0};
        u32 event_max{0};
        GuestAddr proc{0};
        u32 process_id{0};
        u32 thread_id{0};
        u32 flags{0};
        u32 registering_thread_id{0};
        u32 registering_process_id{0};
        bool is_64bit{false};
    };
    struct PendingWinEvent {
        u32 hook{0};
        GuestAddr proc{0};
        bool is_64bit{false};
        u32 event{0};
        windowing::HWND hwnd{0};
        i32 object_id{0};
        i32 child_id{0};
        u32 event_thread{0};
        u32 event_time{0};
    };
    std::mutex m_win_event_hook_mutex;
    std::unordered_map<u32, WinEventHookRegistration> m_win_event_hooks;
    std::unordered_map<u32, std::vector<PendingWinEvent>> m_pending_win_events;
    u32 m_next_win_event_hook{0xFEED8001u};
    u32 m_next_clip_format{0xC000};
    // Registered clipboard format names. Windows keys these on the global
    // atom table: registering the same name twice returns the *same* id, and
    // GetClipboardFormatName maps an id back to its name. Allocating a fresh
    // id per call breaks every format two components agree on by name.
    std::unordered_map<u32, std::string> m_clip_format_names;
    std::unordered_map<std::string, u32> m_clip_format_ids;
    /// Intern one format name (atom-table case-insensitive) and return its id.
    [[nodiscard]] u32 intern_clipboard_format(const std::string& name);
    /// Shared body of GetClipboardFormatNameA/W. `wide` selects the character
    /// size of the output buffer. Returns characters copied, 0 on failure.
    [[nodiscard]] u32 clipboard_format_name(Marshaler& m, bool wide);
    // Windows registers touch input per window; without a digitizer the flag
    // is still remembered and reported by IsTouchWindow.
    std::unordered_map<u32, u32> m_touch_windows;
    [[nodiscard]] bool touch_window_access(u32 hwnd, bool require_owner,
                                           const char* api_name);
    u32 m_next_deferred_pos{0xDF000001};
    struct DeferredPos {
        windowing::HWND hwnd{0};
        windowing::HWND after{0};
        i32 x{0}, y{0}, cx{0}, cy{0};
        u32 flags{0};
    };
    std::mutex m_defer_mutex;
    std::unordered_map<u32, std::vector<DeferredPos>> m_deferred;
    u32 m_next_dialog_handle{0xD1A00000};
    u32 m_active_window{0};
    bool m_application_active{false};
    std::mutex m_activation_mutex;
    std::atomic<u32> m_caret_blink_time_ms{530};
    std::function<std::string()> m_display_adapter_name_provider;
    std::mutex m_registered_message_mutex;
    std::unordered_map<std::string, u32> m_registered_messages;
    u32 m_next_registered_message{0xC000};
    std::mutex m_shell_hook_mutex;
    std::unordered_set<windowing::HWND> m_shell_hook_windows;
    std::mutex m_keyboard_state_mutex;
    std::unordered_map<u32, std::array<u8, 256>> m_keyboard_states;

    struct RawInputRegistration {
        u32 flags{0};
        windowing::HWND target{windowing::NULL_HWND};
        bool is_64bit{false};
    };
    struct RawKeyboardPacket {
        bool mouse{false};
        u16 make_code{0};
        u16 flags{0};
        u16 virtual_key{0};
        u32 message{0};
        u16 button_flags{0};
        u16 button_data{0};
        i32 last_x{0};
        i32 last_y{0};
        bool is_64bit{false};
    };
    std::mutex m_raw_input_mutex;
    std::optional<RawInputRegistration> m_raw_keyboard_registration;
    std::optional<RawInputRegistration> m_raw_mouse_registration;
    std::unordered_map<u64, RawKeyboardPacket> m_raw_keyboard_packets;
    std::deque<u64> m_raw_keyboard_order;
    std::unordered_map<u32, u64> m_current_raw_input_by_thread;
    u64 m_next_raw_input_handle{0x524157000001ull};
    /// Sub-count device motion carried to the next WM_INPUT packet.
    double m_raw_mouse_frac_x{0.0};
    double m_raw_mouse_frac_y{0.0};
    /// Queue one RAWMOUSE packet carrying only usButtonFlags/usButtonData.
    void post_raw_mouse_buttons(u16 button_flags, u16 button_data,
                                windowing::HWND event_hwnd);

    std::unordered_map<u64, std::string> m_window_stations{
        {0xABCD0001u, "WinSta0"}
    };
    u64 m_process_window_station{0xABCD0001u};
    u64 m_next_window_station{0xABCD0100u};
    static constexpr u64 kDefaultDesktopHandle = 0xABCD1001u;
    std::unordered_map<u64, std::string> m_desktops{
        {kDefaultDesktopHandle, "Default"}
    };
    u64 m_thread_desktop{kDefaultDesktopHandle};
    /// Desktop currently receiving user input (OpenInputDesktop / SwitchDesktop).
    u64 m_input_desktop{kDefaultDesktopHandle};
    u64 m_next_desktop{0xABCD1100u};
    /// Self-relative security descriptors for window stations / desktops.
    std::unordered_map<u64, std::vector<u8>> m_user_object_sds;
    /// USEROBJECTFLAGS.dwFlags per station/desktop handle (Learn: WSF_VISIBLE /
    /// DF_ALLOWOTHERACCOUNTHOOK). Interactive WinSta0 starts visible.
    std::unordered_map<u64, u32> m_user_object_dw_flags{
        {0xABCD0001u, 0x0001u} // WSF_VISIBLE
    };
    /// Window stations and desktops are kernel objects: GetUserObjectInformation /
    /// OpenInputDesktop / GetThreadDesktop run on every thread at startup
    /// (sandbox and crash-reporter probes) while CreateDesktop / CloseDesktop
    /// insert and erase. m_window_stations / m_desktops / m_user_object_sds /
    /// m_user_object_dw_flags are touched only under this lock, held for the
    /// table operation only (never across a guest callback: EnumDesktopWindows
    /// releases it before enumerating). The *_by_name / has_* helpers are
    /// caller-holds.
    mutable std::mutex m_user_object_mutex;

    struct SuspendResumeRegistration {
        u64 recipient;
        u32 flags;
    };
    std::unordered_map<u64, SuspendResumeRegistration> m_suspend_resume_registrations;
    u64 m_next_suspend_resume_handle{0xB0070001u};
    struct PowerSettingRegistration {
        u64 recipient;
        std::array<u8, 16> setting_guid;
        u32 flags;
    };
    std::mutex m_power_setting_mutex;
    std::unordered_map<u64, PowerSettingRegistration>
        m_power_setting_registrations;
    u64 m_next_power_setting_handle{0xB0080001u};
    i64 m_process_dpi_context{-1};
    bool m_process_dpi_context_set{false};
    mutable std::mutex m_dpi_context_mutex;
    std::unordered_map<u32, i64> m_thread_dpi_contexts;
    windowing::HWND m_pointer_notification_window{windowing::NULL_HWND};
    bool m_pointer_notification_in_range{false};
    GuestAddr m_windowpos_scratch{0};
    ThreadScratch m_thread_scratch;

    /// LockWindowUpdate: the one locked window (NULL when none).
    std::mutex m_lock_update_mutex;
    windowing::HWND m_lock_update_hwnd{0};
    /// GetGuiResources GR_*_PEAK.
    std::atomic<u32> m_gdi_peak{0};
    std::atomic<u32> m_user_peak{0};

    u64 m_next_timer_id{1};
    mutable std::mutex m_timer_mutex;
    int m_cwp_depth{0};  // CallWindowProcW recursion depth guard
    // Wndproc before the first subclass of this window. A self-call chains
    // to that original rather than returning 0, including a second subclass
    // that never restored the first.
    std::unordered_map<windowing::HWND, u64> m_original_wndproc;
    /// CallWindowProc reads it on the window's thread while SetWindowLongPtr
    /// on another thread's window inserts; held for the lookup only (the
    /// value is copied out before any wndproc is called).
    mutable std::mutex m_wndproc_mutex;
    /// SetTimer timers keyed by (hWnd, nIDEvent). nIDEvent is a UINT_PTR:
    /// 64 bits on x64, 32 bits on x86. WM_TIMER's wParam is that same id, so
    /// the key keeps the full pointer-sized value.
    struct TimerInfo {
        u64 elapse_ms;
        std::chrono::steady_clock::time_point last_fire;
        GuestAddr callback;  // TIMERPROC callback, 0 if none
        /// Thread whose queue gets WM_TIMER: the window's owner (any thread
        /// may arm a window's timer: s2msg_queue_probe
        /// settimer_other_thread_window), else the thread that armed it.
        u32 owner_thread_id;
        /// QS_TIMER change bit already raised for the current expiry.
        bool change_reported = false;
    };
    std::map<std::pair<windowing::HWND, u64>, TimerInfo> m_timer_info;
    bool m_is_64bit{false};  // CPU bitness for callback invocation
    mutable std::mutex m_message_time_mutex;
    std::unordered_map<u32, u32> m_last_message_time_by_thread;
    std::unordered_map<u32, u32> m_last_message_pos_by_thread;

    /// Dispatch HCBT_CREATEWND to matching thread hooks. Returns false when a
    /// hook vetoes creation by returning nonzero.
    /// Per-thread guest bytes for a structure handed to a hook procedure;
    /// freed LIFO by the caller's scratch mark. 0 when exhausted.
    GuestAddr hook_scratch(u32 bytes);
    /// The calling thread's chain for @p id_hook, in call order.
    [[nodiscard]] std::vector<HookRegistration> hook_chain(i32 id_hook,
                                                           u32 thread_id) const;
    /// WH_DEBUG's verdict before @p target runs (true: call it).
    bool debug_hook_allows(const HookRegistration& target, i32 code, u64 wparam,
                           u64 lparam);
    void write_guest_msg(GuestAddr at, const windowing::MSG& msg, bool is64);
    windowing::MSG read_guest_msg(GuestAddr at, bool is64,
                                  const windowing::MSG& fallback);
    /// Call the first procedure of the chain (CallNextHookEx walks the rest).
    /// False when the chain is empty.
    bool call_hook_chain(i32 id_hook, i32 code, u64 wparam, u64 lparam,
                         u64& result);
    [[nodiscard]] bool dispatch_cbt_create_hook(
        windowing::HWND hwnd, u32 style);
    u64 send_clipboard_viewer_message(windowing::HWND hwnd, u32 message,
                                      u64 wParam, u64 lParam);
    void notify_clipboard_viewer();

public:
    /// Called by the message loop to check for expired timers.
    /// Returns true and fills hwnd/timer_id/callback if a timer has expired.
    /// `consume` must be true only when the caller is actually retrieving
    /// WM_TIMER (GetMessage / PeekMessage(PM_REMOVE)). PeekMessage(PM_NOREMOVE)
    /// and WaitMessage observe a due timer without resetting last_fire.
    /// Milliseconds until the earliest timer owned by @p owner_thread_id is
    /// due (0 if one is already due, UINT32_MAX if the thread has none). Lets
    /// a blocking wait sleep exactly until WM_TIMER work exists instead of
    /// re-checking on a fixed slice.
    [[nodiscard]] u32 next_timer_due_ms(
        u32 owner_thread_id,
        windowing::HWND filter_hwnd = windowing::NULL_HWND) const;
    /// GetQueueStatus's QS_TIMER: true when a timer of @p owner_thread_id is
    /// due. @p newly is set when this expiry has not been reported yet.
    [[nodiscard]] bool timer_due_for_thread(u32 owner_thread_id, bool& newly);
    /// DispatchMessage calls a WM_TIMER lParam only when it is the TIMERPROC
    /// of a live timer of this process (s2msg_queue_probe fake_timerproc).
    [[nodiscard]] bool is_registered_timer_proc(GuestAddr proc) const;
    bool find_expired_timer(windowing::HWND& hwnd, u64& timer_id,
                            GuestAddr& callback, u32 owner_thread_id = 0,
                            bool consume = true,
                            windowing::HWND filter_hwnd = windowing::NULL_HWND);

    /// Invoke a TIMERPROC callback. Called by MessageLoopManager when dispatching
    /// WM_TIMER with a callback address in lParam.
    /// @param hwnd The window handle (or 0 for callback-only timers)
    /// @param timer_id The timer ID (wParam)
    /// @param callback The TIMERPROC address (lParam)
    void invoke_timer_callback(windowing::HWND hwnd, u64 timer_id, GuestAddr callback);

    /// Set CPU bitness for callback invocation
    void set_is_64bit(bool is_64bit) { m_is_64bit = is_64bit; }
    std::mutex m_props_mutex;
    std::unordered_map<windowing::HWND, std::unordered_map<std::string, u64>> m_props;
    /// Every string property name ever set: the names that have an atom.
    std::unordered_set<std::string> m_prop_names;

    /// Cursor display counter (ShowCursor increments/decrements)
    i32 m_cursor_display_count{0};
    /// SetWindowRgn: the region each window owns (window coordinates).
    struct WindowRegion {
        std::array<i32, 4> bounds{};
        std::vector<std::array<i32, 4>> rects;  ///< empty: the bounds alone
    };
    std::unordered_map<u32, WindowRegion> m_window_regions;
    // Region/touch snapshots belong to the process; HWND APIs can be called
    // concurrently. Never carry this lock into GDI, Cocoa or guest dispatch.
    mutable std::mutex m_window_state_mutex;

    /// Clipboard state (Learn "Clipboard Operations"; measured by
    /// s2msg_input_probe). Text interoperates with the host pasteboard; the
    /// other formats live in this process.
    bool m_clipboard_open{false};
    std::unordered_map<u32, std::vector<u8>> m_clipboard_data;
    struct ClipboardEntry {
        u32 format;
        u64 handle;  ///< HGLOBAL the system owns; 0 = delayed rendering
    };
    std::recursive_mutex m_clipboard_mutex;
    u32 m_clipboard_open_thread{0};
    windowing::HWND m_clipboard_open_hwnd{windowing::NULL_HWND};
    windowing::HWND m_clipboard_owner{windowing::NULL_HWND};
    /// This process's contents, in SetClipboardData order.
    std::vector<ClipboardEntry> m_clipboard_entries;
    /// The contents were closed since they were set: CF_TEXT / CF_OEMTEXT /
    /// CF_UNICODETEXT / CF_LOCALE synthesis is available.
    bool m_clipboard_closed_since_set{true};
    bool m_clipboard_changed{false};
    /// Host pasteboard change count this process's contents correspond to;
    /// a different count means another application replaced them.
    i64 m_clipboard_host_count{-1};
    i64 m_clipboard_host_seen{-1};
    u32 m_clipboard_sequence{1};
    /// HGLOBALs rendered for synthesized or host-supplied formats.
    std::unordered_map<u32, u64> m_clipboard_rendered;
    /// Formats available now, in EnumClipboardFormats order. Caller holds
    /// m_clipboard_mutex.
    [[nodiscard]] std::vector<u32> clipboard_formats_locked();
    [[nodiscard]] std::string clipboard_text_utf8_locked(Marshaler& m);
    /// Drop this process's contents if the host pasteboard moved on.
    void clipboard_sync_host_locked();
    void clipboard_free_rendered_locked();
    [[nodiscard]] bool clipboard_open_by_caller_locked() const;
    std::mutex m_clipboard_viewer_mutex;
    windowing::HWND m_clipboard_viewer_head{windowing::NULL_HWND};
    GlobalLocalManager* m_global_local{nullptr};
    /// Last HGLOBAL returned from GetClipboardData (freed on next get/empty).
    std::vector<u64> m_clipboard_owned_handles;

    /// Window-to-menu mapping for GetMenu
    std::unordered_map<windowing::HWND, u32> m_window_menus;
    /// Window menus (GetSystemMenu copies), per window.
    std::unordered_map<windowing::HWND, u32> m_system_menus;

    /// Focus tracking
    windowing::HWND m_focus_hwnd{0};
    /// Keys held down by SendInput / keybd_event, merged into
    /// GetAsyncKeyState's host state.
    std::array<std::atomic<u8>, 256> m_injected_keys{};
    /// Inject one keyboard event (KEYBDINPUT semantics) into this process's
    /// focus window as hardware input. False when there is no window to
    /// receive it.
    bool inject_keyboard(u16 vk, u16 scan, u32 flags, u32 time, u64 extra_info = 0);
    /// Inject one mouse event (MOUSEINPUT semantics).
    bool inject_mouse(i32 dx, i32 dy, u32 mouse_data, u32 flags, u32 time,
                      u64 extra_info = 0);
public:
    void impl_keybd_event(Marshaler& m);
    void impl_RegisterHotKey(Marshaler& m);
    void impl_UnregisterHotKey(Marshaler& m);
    /// A key-down matching a registered hot key posts WM_HOTKEY instead of
    /// reaching the focus window. True when @p msg was consumed.
    bool dispatch_hotkey(const windowing::MSG& msg);
private:
    struct HotKey {
        windowing::HWND hwnd;
        i32 id;
        u32 modifiers;  ///< MOD_ALT/CONTROL/SHIFT/WIN (MOD_NOREPEAT kept apart)
        u32 vk;
        bool no_repeat;
        u32 thread_id;  ///< poster for a NULL-window hot key
    };
    std::mutex m_hotkey_mutex;
    std::vector<HotKey> m_hotkeys;
    /// The system caret, one per thread (Learn "Carets": owned by a window
    /// of the thread that created it).
    struct CaretState {
        windowing::HWND hwnd{windowing::NULL_HWND};
        i32 x{0}, y{0}, width{0}, height{0};
        i32 hide_count{1};  ///< CreateCaret leaves it hidden
        u64 bitmap{0};
    };
    std::mutex m_caret_mutex;
    std::unordered_map<u32, CaretState> m_carets;
public:
    void impl_CreateCaret(Marshaler& m);
    void impl_DestroyCaret(Marshaler& m);
    void impl_SetCaretPos(Marshaler& m);
    void impl_GetCaretPos(Marshaler& m);
    void impl_ShowHideCaret(Marshaler& m, bool show);
private:
    /// AttachThreadInput pairs (idAttach, idAttachTo). Attached threads share
    /// one input state: focus, active window and capture.
    mutable std::mutex m_attach_mutex;
    std::vector<std::pair<u32, u32>> m_attached_inputs;
    std::unordered_map<windowing::HWND, windowing::HWND>
        m_saved_focus_by_top_level;
};

[[nodiscard]] DllExports create_user32_ext_thunks(User32ExtManager& mgr);

} // namespace vitreo::winapi
