#pragma once
/// @file message_send.hpp
/// @brief PostQuitMessage/PostMessageW/SendMessageW (WIN32-055)

#include "windowing/contracts/syslink_markup.hpp"
#include <vitreo/common.hpp>
#include "winapi/core/thunk_registry.hpp"
#include "windowing/model/hwnd_manager.hpp"
#include "windowing/messages/message_queue.hpp"
#include "windowing/messages/message_types.hpp"
#include "windowing/cocoa/cocoa_bridge.hpp"
#include <functional>
#include <optional>
#include <string>
#include <vector>
#include <mutex>

namespace vitreo::winapi {

// Progress bar / button / font message codes shared by message_send.cpp and
// the handle_control_message extract.
inline constexpr u32 PBM_SETRANGE   = 0x0401;
inline constexpr u32 PBM_SETPOS     = 0x0402;
inline constexpr u32 PBM_DELTAPOS   = 0x0403;
inline constexpr u32 PBM_SETSTEP    = 0x0404;
inline constexpr u32 PBM_STEPIT     = 0x0405;
inline constexpr u32 PBM_SETRANGE32 = 0x0406;
inline constexpr u32 PBM_GETPOS     = 0x0408;
inline constexpr u32 PBM_SETMARQUEE = 0x040A;
// CommCtrl.h (WM_USER + n, CCM_FIRST + 1). Several collide with RB_* values,
// so they are only interpreted for msctls_progress32.
inline constexpr u32 PBM_GETRANGE   = 0x0407;
inline constexpr u32 PBM_SETBARCOLOR = 0x0409;
inline constexpr u32 PBM_GETSTEP    = 0x040D;
inline constexpr u32 PBM_GETBKCOLOR = 0x040E;
inline constexpr u32 PBM_GETBARCOLOR = 0x040F;
inline constexpr u32 PBM_SETSTATE   = 0x0410;
inline constexpr u32 PBM_GETSTATE   = 0x0411;
inline constexpr u32 PBM_SETBKCOLOR = 0x2001;   // CCM_SETBKCOLOR
inline constexpr u32 PBST_NORMAL    = 0x0001;
inline constexpr u32 CLR_DEFAULT_COLOR = 0xFF000000u;
// Rebar messages (Commctrl.h) share the WM_USER+N space with PBM_* — callers
// must be discriminated by window class, not message id alone.
inline constexpr u32 RB_SETBARINFO    = 0x0404;
inline constexpr u32 RB_INSERTBANDW   = 0x040A;
inline constexpr u32 RB_SETBANDINFOW  = 0x040B;
inline constexpr u32 RB_GETBANDCOUNT  = 0x040C;
inline constexpr u32 RB_GETROWCOUNT   = 0x040D;
inline constexpr u32 RB_GETBARHEIGHT  = 0x041B;
inline constexpr u32 RB_SHOWBAND      = 0x0410;
// Toolbar messages used during host chrome layout.
inline constexpr u32 TB_INSERTBUTTONA   = 0x0413;
inline constexpr u32 TB_BUTTONSTRUCTSIZE = 0x041E;
inline constexpr u32 TB_SETBUTTONSIZE   = 0x041F;
inline constexpr u32 TB_SETBITMAPSIZE   = 0x0420;
inline constexpr u32 TB_AUTOSIZE        = 0x0421;
inline constexpr u32 TB_GETBUTTONSIZE   = 0x043A;
inline constexpr u32 TB_ADDBUTTONSW     = 0x0444;
inline constexpr u32 BM_GETCHECK    = 0x00F0;
inline constexpr u32 BM_SETCHECK    = 0x00F1;
inline constexpr u32 BM_GETSTATE    = 0x00F2;
inline constexpr u32 BM_SETSTATE    = 0x00F3;
inline constexpr u32 BM_SETSTYLE    = 0x00F4;
inline constexpr u32 BST_UNCHECKED  = 0x0000;
inline constexpr u32 BST_CHECKED    = 0x0001;
inline constexpr u32 BST_INDETERMINATE = 0x0002;
inline constexpr u32 WM_SETFONT     = 0x0030;
inline constexpr u32 WM_GETFONT     = 0x0031;
inline constexpr u32 EM_LIMITTEXT    = 0x00C5; // identical to EM_SETLIMITTEXT
inline constexpr u32 EM_SETLIMITTEXT = 0x00C5;
inline constexpr u32 EM_GETLIMITTEXT = 0x00D5;


class Marshaler;
class GdiManager;

/// Callback for synchronous message dispatch (SendMessage)
using SendWndProcCallback = std::function<u64(windowing::HWND, u32, u64, u64, u64)>;

class MessageSendManager {
public:
    using GuestCall = std::function<u64(GuestAddr, const u64*, usize, bool)>;

    MessageSendManager(
        windowing::HwndManager& hwnd_mgr,
        windowing::MessageQueue& queue,
        windowing::CocoaBridge& bridge
    );

    /// Set callback for SendMessage dispatch
    void set_wndproc_callback(SendWndProcCallback callback);
    void set_gdi_manager(GdiManager* manager) { m_gdi_manager = manager; }
    /// @param ansi LB_GETTEXT writes the item text in the caller's encoding.
    bool dispatch_owner_draw_list(Marshaler& m, windowing::HWND hwnd, u32 msg,
                                 u64 wParam, u64 lParam, bool force_native,
                                 bool ansi = false);
    void set_guest_call(GuestCall call) { m_guest_call = std::move(call); }

    /// PostQuitMessage — posts WM_QUIT with exit code
    void post_quit_message(int exit_code);

    /// PostMessageW — post async message
    /// HWND_BROADCAST (0xFFFF) posts to all top-level windows
    /// NULL HWND posts a thread message
    bool post_message(windowing::HWND hwnd, u32 msg, u64 wParam, u64 lParam);

    /// PostThreadMessageW — post a thread-level message (hwnd=NULL)
    bool post_thread_message(u32 thread_id, u32 msg, u64 wParam, u64 lParam);

    /// The last error PostMessage, PostThreadMessage and SendNotifyMessage
    /// fail @p msg with (Learn: pointer-carrying messages below WM_USER are
    /// synchronous only), or 0 when it may be posted. The table is measured
    /// (post_message_policy.inc, generated from s2msg_queue_probe).
    [[nodiscard]] static u32 async_message_refusal(u32 msg) noexcept;

    /// PostThreadMessage's target check: a live guest thread of this process
    /// (ERROR_INVALID_THREAD_ID otherwise; thread id 0 never is), or a peer
    /// process's advertised primary thread.
    [[nodiscard]] bool thread_accepts_messages(u32 thread_id) const;

    /// GetInputState: new input of @p qs_mask classes queued for @p thread_id.
    [[nodiscard]] bool thread_has_new_input(u32 thread_id, u32 qs_mask) const {
        return m_queue.has_new_input_for_thread(thread_id, m_hwnd_mgr, qs_mask);
    }

    /// Liveness of a guest thread of this process, for PostThreadMessage's
    /// ERROR_INVALID_THREAD_ID. Unset: every thread is assumed alive.
    void set_thread_alive_query(std::function<bool(u32)> query) {
        m_thread_alive = std::move(query);
    }

    /// SendNotifyMessage (Learn): a window of the calling thread gets the
    /// message synchronously; another thread's window gets it queued as a
    /// sent message and the call returns at once. HWND_BROADCAST reaches
    /// every top-level window. False with the last error set on failure.
    bool send_notify_message(windowing::HWND hwnd, u32 msg, u64 wParam, u64 lParam);

    /// SendMessageW — synchronous dispatch to wndproc
    /// Returns the wndproc return value
    u64 send_message(windowing::HWND hwnd, u32 msg, u64 wParam, u64 lParam);

    /// WH_CALLWNDPROC (@p after false) / WH_CALLWNDPROCRET hook dispatch for
    /// a message SendMessage delivers on the receiving thread.
    using CallWndProcHook = std::function<void(windowing::HWND, u32, u64, u64,
                                               bool cross_thread, bool after,
                                               u64 lresult)>;
    void set_call_wndproc_hook(CallWndProcHook hook) {
        m_call_wndproc_hook = std::move(hook);
    }

    /// SendMessageTimeout's delivery: a window owned by another thread is
    /// sent through the timed wait (SMTO_* flags honoured); a same-thread
    /// window is called directly. `status` reports why a 0 came back.
    u64 send_message_timeout(windowing::HWND hwnd, u32 msg, u64 wParam, u64 lParam,
                             u32 flags, u32 timeout,
                             windowing::MessageQueue::SendWaitStatus& status);
    /// This process's top-level windows (HWND_BROADCAST targets).
    [[nodiscard]] std::vector<windowing::HWND> top_level_windows() const {
        return m_hwnd_mgr.enum_children(windowing::HWND_DESKTOP);
    }
    /// True when a send to `hwnd` crosses to another guest thread's wndproc.
    [[nodiscard]] bool is_cross_thread_wndproc(windowing::HWND hwnd) const;

    /// Dispatch a control message that needs a Marshaler (combobox/listbox
    /// population & queries, button-check, etc.). Used by SendDlgItemMessage so
    /// controls populate the same as via SendMessage. `ansi` selects string
    /// encoding. When `force_native` is set (CallWindowProc chaining to Vitreo's
    /// Def*Proc sentinel), handle even if the control is currently subclassed.
    /// Returns true if handled (and sets the Marshaler's return value).
    bool dispatch_marshaled_control(Marshaler& m, windowing::HWND hwnd, u32 msg,
                                    u64 wParam, u64 lParam, bool ansi,
                                    bool force_native = false);

    // --- Minimal helpers for thunk-level fallbacks (e.g. WM_SETTEXT/WM_GETTEXT on controls) ---
    [[nodiscard]] u64 wndproc_for(windowing::HWND hwnd) const;
    [[nodiscard]] bool set_control_text(windowing::HWND hwnd, const std::wstring& text);
    [[nodiscard]] std::wstring get_control_text(windowing::HWND hwnd) const;
    [[nodiscard]] windowing::WindowData* window_data(windowing::HWND hwnd);

    void adjust_common_control_size(windowing::HWND hwnd, i32& width, i32& height) const;
    bool dispatch_rebar_control(Marshaler& m, windowing::HWND hwnd, u32 msg, u64 wParam, u64 lParam);
    bool insert_rebar_child(windowing::HWND rebar, windowing::HWND child, i32 height);

    /// Access the bridge for listview operations
    windowing::CocoaBridge& bridge() { return m_bridge; }
    [[nodiscard]] const GuestCall& guest_call() const { return m_guest_call; }

private:
    /// The receiving-thread half of send_message: control intercepts, then
    /// the window procedure.
    u64 deliver_sent_message(windowing::HWND hwnd, u32 msg, u64 wParam, u64 lParam);
    CallWndProcHook m_call_wndproc_hook;
    std::function<bool(u32)> m_thread_alive;
    GdiManager* m_gdi_manager = nullptr;
    void paint_owner_draw_list(Marshaler& m, windowing::HWND hwnd, u64 supplied_dc = 0);
    windowing::HwndManager& m_hwnd_mgr;
    windowing::MessageQueue& m_queue;
    windowing::CocoaBridge& m_bridge;
    SendWndProcCallback m_wndproc_callback;
    GuestCall m_guest_call;

    // Protects only control tables. Never held across HWND/Cocoa/guest calls.
    mutable std::mutex m_control_state_mutex;

    /// Button check state tracking (for BM_SETCHECK / BM_GETCHECK)
    std::unordered_map<windowing::HWND, u32> m_button_check_state;

    struct StaticImageState {
        u32 bitmap = 0;
        u32 icon = 0;
    };

    /// Last image handles per static control, tracked by image type.
    std::unordered_map<windowing::HWND, StaticImageState> m_static_image_state;

    struct ProgressState {
        i32 minimum = 0;
        i32 maximum = 100;
        i32 position = 0;
        i32 step = 10;
        u32 bar_color = CLR_DEFAULT_COLOR;  // Learn PBM_GETBARCOLOR: default CLR_DEFAULT
        u32 bk_color = CLR_DEFAULT_COLOR;
        u32 state = PBST_NORMAL;
    };

    /// The common control owns range, position and step state. Cocoa is only
    /// the renderer for that state.
    std::unordered_map<windowing::HWND, ProgressState> m_progress_state;

    struct RebarState {
        u32 band_count = 0;
        i32 row_height = 30; // Learn: typical single-row rebar with toolbar
        u32 id = 0;
        u32 style = 0;
        windowing::HWND child = 0;
        u32 min_width = 0;
        u32 min_height = 0;
        u32 width = 0;
    };
    std::unordered_map<windowing::HWND, RebarState> m_rebar_state;

    /// State the Button class keeps for BM_SETIMAGE and the comctl32 v6
    /// BCM_* messages (button_control_messages.cpp).
    struct ButtonExtState {
        i32 margin[4]{1, 1, 1, 1};     ///< BCM_SETTEXTMARGIN (probe default 1,1,1,1)
        u64 image_list{0};             ///< BUTTON_IMAGELIST.himl
        i32 image_margin[4]{0, 0, 0, 0};
        u32 image_align{0};
        u64 bitmap{0};                 ///< BM_SETIMAGE IMAGE_BITMAP
        u64 icon{0};                   ///< BM_SETIMAGE IMAGE_ICON
        u64 split_glyph{0};            ///< BUTTON_SPLITINFO
        u32 split_style{0x2};          ///< BCSS_STRETCH (probe default)
        i32 split_size[2]{15, 0};      ///< probe default
        bool split_glyph_set{false};
        std::u16string note;           ///< BCM_SETNOTE (command links)
        bool shield{false};
    };
    std::unordered_map<windowing::HWND, ButtonExtState> m_button_ext_state;
    /// The process default activation context names Common Controls 6.0
    /// (the exe manifest): Button windows answer the BCM_* messages.
    bool m_common_controls_v6{false};
    std::function<bool(u64, i32&, i32&)> m_image_list_size_query;
    std::function<bool(u64, i32&, i32&)> m_icon_size_query;
    std::function<bool(u64, u32&, u32&, std::vector<u8>&)> m_icon_rgba_query;

public:
    /// An icon's image as straight-alpha RGBA, top-down, for a native SS_ICON
    /// static to show (STM_SETICON / STM_SETIMAGE IMAGE_ICON). False for a
    /// handle the owner does not know.
    using IconRgbaQuery = std::function<bool(u64 icon, u32& width, u32& height, std::vector<u8>& rgba)>;
    void set_icon_rgba_query(IconRgbaQuery q) { m_icon_rgba_query = std::move(q); }
    /// Show @p icon in the native image view of static @p hwnd; false when
    /// the icon is unknown or the control has no image view.
    bool show_static_icon(windowing::HWND hwnd, u64 icon);
    /// Process setup: whether the exe manifest asks for comctl32 v6.
    void set_common_controls_v6(bool v6) { m_common_controls_v6 = v6; }
    /// Image sizes BCM_GETIDEALSIZE adds: a comctl32 image list's icon size
    /// and an icon's size. False for a handle the owner does not know.
    using ImageSizeQuery = std::function<bool(u64 handle, i32& width, i32& height)>;
    void set_image_list_size_query(ImageSizeQuery q) { m_image_list_size_query = std::move(q); }
    void set_icon_size_query(ImageSizeQuery q) { m_icon_size_query = std::move(q); }
    /// Button class messages that need guest memory: BM_SETIMAGE /
    /// BM_GETIMAGE and the comctl32 v6 BCM_* family. Handles a subclassed
    /// control only when @p force_native. button_control_messages.cpp.
    bool dispatch_button_control(Marshaler& m, windowing::HWND hwnd, u32 msg,
                                 u64 wParam, u64 lParam, bool force_native);

    /// Per-control state for tooltips_class32. The control owns its text,
    /// tracking position and bubble width; the bubble itself is presented
    /// natively by the windowing host.
    struct TooltipState {
        std::wstring text;
        i32 track_x{0};
        i32 track_y{0};
        i32 max_width{-1};     ///< Learn: no maximum width set
        u32 tool_count{0};
        u32 delay_ms{500};
        bool visible{false};
    };
    bool dispatch_tooltip_control(Marshaler& m, windowing::HWND hwnd,
                                  u32 msg, u64 wParam, u64 lParam);
    bool dispatch_tab_control(Marshaler& m, windowing::HWND hwnd, u32 msg,
                              u64 wParam, u64 lParam, bool ansi);
    /// SysLink (WC_LINK) class procedure: markup text, LM_* item messages,
    /// keyboard and mouse handling and NM_CLICK / NM_RETURN. Handles a
    /// subclassed control only when @p force_native (CallWindowProc of the
    /// class procedure). control_syslink.cpp.
    bool dispatch_syslink_control(Marshaler& m, windowing::HWND hwnd, u32 msg,
                                  u64 wParam, u64 lParam, bool ansi,
                                  bool force_native);
    /// PBM_GETRANGE with a PBRANGE* (needs guest memory). The value-only
    /// progress messages live in handle_control_message.
    bool dispatch_progress_range(Marshaler& m, windowing::HWND hwnd, u32 msg,
                                 u64 wParam, u64 lParam, bool force_native);
    /// EDIT class procedure: the Windows-visible text and every EM_* plus
    /// WM_SET/GETTEXT, WM_CUT/COPY/PASTE/CLEAR/UNDO (control_edit.cpp).
    /// Handles a subclassed control only when @p force_native.
    bool dispatch_edit_control(Marshaler& m, windowing::HWND hwnd, u32 msg,
                               u64 wParam, u64 lParam, bool ansi,
                               bool force_native);
    using EditClipboardCopy =
        std::function<bool(Marshaler&, windowing::HWND, const std::u16string&)>;
    using EditClipboardText =
        std::function<std::optional<std::u16string>(Marshaler&)>;
    /// The clipboard operations EDIT's WM_COPY / WM_CUT / WM_PASTE run.
    void set_edit_clipboard(EditClipboardCopy copy, EditClipboardText text) {
        m_edit_clipboard_copy = std::move(copy);
        m_edit_clipboard_text = std::move(text);
    }
    /// GetKeyState for the calling thread (SysLink Shift+Tab).
    void set_key_state_provider(std::function<i16(u32)> fn) {
        m_key_state = std::move(fn);
    }
    /// Drop per-control state when @p hwnd is destroyed, so a reused HWND
    /// value never inherits it.
    void control_destroyed(windowing::HWND hwnd);

private:
    std::unordered_map<windowing::HWND, TooltipState> m_tooltip_state;
    struct TabExtraState {
        u64 imagelist = 0;
        u32 ex_style = 0;
        u32 item_size = 0;
        u32 padding = 0;
        u64 tooltip_hwnd = 0;
        i32 min_tab_width = -1;
    };
    std::unordered_map<windowing::HWND, TabExtraState> m_tab_extra;

    struct ToolbarState {
        i32 bitmap_cx = 16;
        i32 bitmap_cy = 15;
        i32 padding_cx = 4;
        i32 padding_cy = 3;
        i32 button_cx = 24;
        i32 button_cy = 22;
        u32 button_count = 0;
    };
    std::unordered_map<windowing::HWND, ToolbarState> m_toolbar_state;

    struct SysLinkState {
        std::wstring markup;               ///< WM_GETTEXT returns the markup.
        windowing::syslink::Markup parsed; ///< Display text, items, id/url.
        std::vector<u32> states;           ///< LIS_* per item.
        i32 pressed = -1;                  ///< Item under WM_LBUTTONDOWN.
    };
    std::unordered_map<windowing::HWND, SysLinkState> m_syslink_state;

public:
    /// One EDIT control's Windows-visible state (control_edit.cpp).
    struct EditState {
        std::u16string text;        ///< Windows-visible text (CRLF line breaks)
        std::u16string shown;       ///< display string last given to the view
        u32 anchor = 0;             ///< EM_SETSEL start side
        u32 caret = 0;              ///< EM_SETSEL end side (the caret)
        bool modified = false;
        bool can_undo = false;
        std::u16string undo_text;   ///< text EM_UNDO restores
        u32 undo_start = 0;         ///< selection EM_UNDO leaves: start..+len
        u32 undo_len = 0;
        u32 redo_len = 0;           ///< length to select after the next undo
        u32 limit = 30000;          ///< edit_messages_probe "limit=30000"
        u16 password_char = 0;
        i32 left_margin = 0;
        i32 right_margin = 0;
        std::u16string cue;
        bool has_cue = false;
        bool fmtlines = false;
        u64 wordbreak_proc = 0;
        u32 ex_style = 0;
        u32 end_of_line = 0;
        bool initialized = false;
    };

private:
    std::unordered_map<windowing::HWND, EditState> m_edit_state;
    EditClipboardCopy m_edit_clipboard_copy;
    EditClipboardText m_edit_clipboard_text;
    std::function<i16(u32)> m_key_state;

    /// Handle common control messages (e.g., progress bar)
    /// @return true if handled, false to dispatch to wndproc
    bool handle_control_message(windowing::HWND hwnd, u32 msg, u64 wParam, u64 lParam, u64& result);
};

// Thunks
void impl_PostQuitMessage(Marshaler& m, MessageSendManager& msm);
void impl_PostMessageW(Marshaler& m, MessageSendManager& msm);
void impl_PostThreadMessageW(Marshaler& m, MessageSendManager& msm);
void impl_SendNotifyMessageW(Marshaler& m, MessageSendManager& msm);
void impl_SendMessageW(Marshaler& m, MessageSendManager& msm);
void impl_SendMessageA(Marshaler& m, MessageSendManager& msm);
void impl_SendMessageTimeoutW(Marshaler& m, MessageSendManager& msm);

[[nodiscard]] DllExports make_message_send_exports(MessageSendManager& msm);

} // namespace vitreo::winapi
