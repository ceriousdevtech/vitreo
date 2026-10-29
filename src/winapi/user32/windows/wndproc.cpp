// SPDX-License-Identifier: LGPL-2.1-only

/// @file wndproc.cpp
/// @brief Window procedure / class long / CallWindowProc surface.
///
/// Closed cut from user32_ext.cpp — verified with plan_extraction.py.

#include "winapi/user32/painting/sys_colors.hpp"
#include "winapi/user32/ansi_text.hpp"
#include "winapi/user32/user32_ext.hpp"
#include "winapi/user32/user32_internal.hpp"
#include "winapi/user32/user32_errors.hpp"
#include "winapi/user32/classes/system_classes.hpp"
#include "winapi/common/display_device_identity.hpp"
#include "winapi/user32/painting/painting.hpp"
#include "winapi/user32/messages/message_loop.hpp"
#include "winapi/user32/messages/message_send.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/kernel32/memory/global_local.hpp"
#include "winapi/kernel32/module/module.hpp"
#include "winapi/kernel32/process/process.hpp"
#include "winapi/kernel32/thread/thread.hpp"
#include "winapi/graphics/gdi32/gdi32.hpp"
#include "graphics/contracts/backend_device.hpp"
#include "windowing/model/hwnd_manager.hpp"
#include "ipc/ipc_gate.hpp"
#include "ipc/hwnd_registry.hpp"
#include "windowing/resources/dialog_template.hpp"
#include "windowing/resources/resource_walker.hpp"
#include "winapi/user32/classes/window_class.hpp"
#include "winapi/user32/windows/window.hpp"
#include "winapi/user32/windows/draw_item_layout.hpp"
#include "winapi/user32/windows/sdk_creation_layouts.hpp"
#include "winapi/user32/dialogs/dialog.hpp"
#include "winapi/user32/windows/show_window.hpp"
#include "winapi/user32/resources/resources.hpp"
#include "core/pe/parser/pe_structures.hpp"
#include "vitreo/result.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace vitreo::winapi {

namespace {

// Owner-draw controls (MS Learn: BS_OWNERDRAW / SS_OWNERDRAW, DRAWITEMSTRUCT).
// The BUTTON/STATIC class proc paints nothing itself — it asks the parent to
// draw the control with WM_DRAWITEM. Skinned installers (EA's AutoRun menu)
// put all their button artwork and text there, so a control whose class proc
// ignores WM_PAINT stays blank while remaining clickable.
constexpr u32 kLowStyleMask  = 0x0000000F;
constexpr u32 kBsOwnerDraw   = 0x0000000B;
constexpr u32 kSsOwnerDraw   = 0x0000000D;
constexpr u16 kAtomButton    = 0x0080;
constexpr u16 kAtomStatic    = 0x0082;
constexpr u32 kOdtButton     = 4;
constexpr u32 kOdtStatic     = 5;
constexpr u32 kOdaDrawEntire = 0x0001;
constexpr u32 kOdsDisabled   = 0x0004;
constexpr u32 kWmDrawItem    = 0x002B;
// Each synchronous callback owns its storage. A global address aliases nested
// WM_DRAWITEM deliveries and outlives its GuestMemory when a runtime is replaced.
struct OwnerDrawScratch {
    GuestMemory& memory;
    GuestAddr address = 0;
    explicit OwnerDrawScratch(GuestMemory& guest_memory) : memory(guest_memory) {
        address = memory.map_first_fit(0x3E100000, 4096, MemoryProtection::ReadWrite);
    }
    ~OwnerDrawScratch() { if (address) (void)memory.unmap_region(address, 4096); }
    OwnerDrawScratch(const OwnerDrawScratch&) = delete;
    OwnerDrawScratch& operator=(const OwnerDrawScratch&) = delete;
};

/// Returns ODT_BUTTON / ODT_STATIC when @p win is owner-drawn, else 0.
u32 owner_draw_type(const windowing::WindowData& win) {
    const u32 low = win.style & kLowStyleMask;
    if ((win.class_atom == kAtomButton || win.native_control_class == L"Button") && low == kBsOwnerDraw) return kOdtButton;
    if ((win.class_atom == kAtomStatic || win.native_control_class == L"Static") && low == kSsOwnerDraw) return kOdtStatic;
    return 0;
}

} // namespace


// ---------- Window procedure / class ----------

void User32ExtManager::impl_CallWindowProcW(Marshaler& m) {
    call_window_proc_impl(m, /*ansi=*/false);
}

void User32ExtManager::impl_CallWindowProcA(Marshaler& m) {
    call_window_proc_impl(m, /*ansi=*/true);
}

void User32ExtManager::call_window_proc_impl(Marshaler& m, bool ansi) {
    // CallWindowProc's own signature: (prev, hwnd, msg, wParam, lParam).
    // Measured (s1win_api_probe callwindowproc*_null_proc): a NULL
    // procedure returns 0 without calling anything.
    if (m.arg_ptr(0) == 0) {
        m.set_return(0u);
        return;
    }
    call_window_proc_values(m, m.arg_ptr(0),
                            static_cast<windowing::HWND>(m.arg(1)),
                            static_cast<u32>(m.arg(2)), m.arg(3), m.arg(4),
                            ansi);
}

/// The native-control default proc, reachable as a real callable address.
///
/// A guest that subclasses a Cocoa-backed control gets the previous procedure
/// from SetWindowLongPtr(GWLP_WNDPROC) and is entitled to CALL it directly —
/// that is the documented Windows pattern, and Ark's server console does
/// exactly that. The address used to be a bare sentinel constant pointing at
/// unmapped memory, so the direct call landed on int3 padding and raised
/// STATUS_ILLEGAL_INSTRUCTION; the console's paint handler died on every
/// message and its text area stayed blank. Registering this as a thunk gives
/// the sentinel a real body with the same behaviour CallWindowProc gives it.
void User32ExtManager::impl_VitreoNativeControlProc(Marshaler& m) {
    // A wndproc's signature: (hwnd, msg, wParam, lParam).
    call_window_proc_values(m, static_cast<GuestAddr>(native_control_wndproc_address()),
                            static_cast<windowing::HWND>(m.arg(0)),
                            static_cast<u32>(m.arg(1)), m.arg(2), m.arg(3),
                            /*ansi=*/false);
}

void User32ExtManager::impl_VitreoSystemControlProc(Marshaler& m,
                                                  std::wstring_view base_class, bool ansi) {
    call_window_proc_values(m, system_class_wndproc(base_class, ansi),
        static_cast<windowing::HWND>(m.arg(0)), static_cast<u32>(m.arg(1)),
        m.arg(2), m.arg(3), ansi);
}

void User32ExtManager::call_window_proc_values(
    Marshaler& m, GuestAddr lpPrevWndFunc, windowing::HWND hwnd, u32 msg,
    u64 wParam, u64 lParam, bool ansi) {

    // Trace CallWindowProc for WM_COMMAND
    if (msg == 0x0111 && diag_logs_enabled()) { // WM_COMMAND
        std::fprintf(stderr,
            "[btn-diag] CallWindowProc: proc=0x%llx hwnd=0x%x WM_COMMAND id=%u ansi=%d\n",
            static_cast<unsigned long long>(lpPrevWndFunc), hwnd,
            static_cast<unsigned>(wParam & 0xFFFF), ansi ? 1 : 0);
    }
    // Trace CallWindowProc for the WM_USER+ range (application-defined
    // messages).
    if (msg >= 0x0400 && diag_logs_enabled()) {
        std::fprintf(stderr,
            "[cwp-diag] CallWindowProc: proc=0x%llx hwnd=0x%x msg=0x%x wParam=0x%llx\n",
            static_cast<unsigned long long>(lpPrevWndFunc), hwnd, msg,
            static_cast<unsigned long long>(wParam));
    }

    // Guard against infinite recursion. A subclass chain can store its own
    // wrapper as the previous wndproc when an earlier subclass never
    // restored the original.
    constexpr int MAX_CWP_DEPTH = 8;
    if (m_cwp_depth >= MAX_CWP_DEPTH) {
        VITREO_TRACE( "[user32_ext] CallWindowProc: recursion depth %d, returning 0\n",
                     m_cwp_depth);
        m.set_return(0);
        return;
    }

    // Self-call detection: if lpPrevWndFunc == the current wndproc of the
    // target window, calling it would recurse through the same subclass
    // wrapper.  Instead of dropping the message, chain to the original
    // (pre-subclass) wndproc if we have one.
    const windowing::WindowData* win = m_hwnd_mgr.find(hwnd);
    if (win && !find_system_class_by_wndproc(lpPrevWndFunc) &&
        static_cast<GuestAddr>(win->wndproc) == lpPrevWndFunc) {
        bool has_original = false;
        u64 original = 0;
        {
            std::lock_guard<std::mutex> procs(m_wndproc_mutex);
            auto orig_it = m_original_wndproc.find(hwnd);
            if (orig_it != m_original_wndproc.end()) {
                has_original = true;
                original = orig_it->second;
            }
        }
        if (has_original
            && static_cast<GuestAddr>(original) != lpPrevWndFunc
            && (original != 0 || is_native_host_control(*win))) {
            const u64 chain_to = (original != 0)
                ? original
                : kNativeControlWndProc;
            VITREO_TRACE( "[user32_ext] CallWindowProc: self-call detected "
                         "(proc=0x%llx == wndproc), chaining to original 0x%llx\n",
                         static_cast<unsigned long long>(lpPrevWndFunc),
                         static_cast<unsigned long long>(chain_to));
            lpPrevWndFunc = static_cast<GuestAddr>(chain_to);
        } else {
            // No original to chain to — return 0 as DefWindowProc
            VITREO_TRACE( "[user32_ext] CallWindowProc: self-call detected "
                         "(proc=0x%llx == wndproc), no original, returning 0\n",
                         static_cast<unsigned long long>(lpPrevWndFunc));
            m.set_return(0);
            return;
        }
    }

    const auto* base_class = find_system_class_by_wndproc(lpPrevWndFunc);
    if (base_class && msg == windowing::WM::NCCREATE) {
        // Mixed caller/base encodings need Windows code-page conversion of
        // the CREATESTRUCT strings. Do not claim successful initialization
        // while that conversion path is unsupported.
        if (lpPrevWndFunc == system_class_wndproc(base_class->name, !ansi)) {
            static std::once_flag once;
            std::call_once(once, [] {
                std::fprintf(stderr, "[vitreo] UNIMPLEMENTED user32!CallWindowProc: mixed A/W superclass creation\n");
            });
            m.set_return(0u);
            return;
        }
        const bool initialized = m_system_control_initializer &&
            m_system_control_initializer(m, hwnd, base_class->name, lParam, ansi);
        m.set_return(initialized ? 1u : 0u);
        return;
    }

    if (!base_class && msg == windowing::WM::NCCREATE &&
        is_native_control_wndproc(static_cast<u64>(lpPrevWndFunc))) {
        // Generic hosted controls already have their real backing before
        // synchronous creation delivery. Acknowledge only that actual state
        // and a readable CREATESTRUCT of the caller's Windows ABI.
        std::array<u8, sizeof(sdk_creation::CREATESTRUCTW<u64>)> create{};
        const usize bytes = m.cpu_is_64bit()
            ? sizeof(sdk_creation::CREATESTRUCTW<u64>)
            : sizeof(sdk_creation::CREATESTRUCTW<u32>);
        const bool native_owner = win &&
            (is_native_host_control(*win) || !win->native_control_class.empty());
        const bool initialized = native_owner && lParam != 0 &&
            m.memory().read_bytes(static_cast<GuestAddr>(lParam), create.data(), bytes).is_ok() &&
            m_host.has_control_view(hwnd);
        m.set_return(initialized ? 1u : 0u);
        return;
    }

    // Native Cocoa control Def*Proc, and a system class procedure the guest
    // forwarded to. The system procedure is a host thunk: invoking it as
    // guest code re-enters this function and drops the message. Run the
    // base control behavior here, including EM_STREAMIN on a subclassed edit.
    if (base_class || is_native_control_wndproc(static_cast<u64>(lpPrevWndFunc))) {
        if (m_msgsend_mgr &&
            m_msgsend_mgr->dispatch_marshaled_control(
                m, hwnd, msg, wParam, lParam, ansi, /*force_native=*/true)) {
            VITREO_TRACE(
                "[user32_ext] CallWindowProc: native control hwnd=0x%x msg=0x%x "
                "ansi=%d -> handled\n",
                hwnd, msg, ansi ? 1 : 0);
            return;
        }
        if (m_msgsend_mgr && msg == windowing::WM::SETTEXT && lParam != 0) {
            std::wstring ws;
            if (ansi) {
                auto str_res =
                    m.read_ansi_string(static_cast<GuestAddr>(lParam));
                if (str_res.is_ok()) {
                    ws = utf8_to_wstring(str_res.value());
                }
            } else {
                auto str_res =
                    m.read_wide_string(static_cast<GuestAddr>(lParam));
                if (str_res.is_ok()) {
                    ws = utf8_to_wstring(str_res.value());
                }
            }
            if (auto* w = m_hwnd_mgr.find(hwnd)) {
                w->title = ws;
            }
            (void)m_msgsend_mgr->set_control_text(hwnd, ws);
            m.set_return(1u);
            return;
        }
        // WM_GETTEXT / WM_GETTEXTLENGTH through the class procedure (VCL's
        // TEdit.Text reaches the EDIT proc this way). Learn WM_GETTEXT:
        // wParam = buffer size in characters including the NUL; copies at
        // most wParam-1 characters, NUL-terminates, returns the count copied.
        // WM_GETTEXTLENGTH: the length in characters (bytes for the A form).
        if (m_msgsend_mgr &&
            (msg == windowing::WM::GETTEXT ||
             msg == windowing::WM::GETTEXTLENGTH)) {
            std::wstring text;
            if (m_msgsend_mgr->bridge().has_control_view(hwnd)) {
                text = m_msgsend_mgr->get_control_text(hwnd);
            } else if (const auto* w = m_hwnd_mgr.find(hwnd)) {
                text = w->title;
            }
            const std::string ansi_text_bytes =
                ansi ? ansi_text::to_ansi(text) : std::string{};
            const u32 len = ansi ? static_cast<u32>(ansi_text_bytes.size())
                                 : static_cast<u32>(text.size());
            if (msg == windowing::WM::GETTEXTLENGTH) {
                m.set_return(static_cast<u64>(len));
                return;
            }
            const u32 max_count = static_cast<u32>(wParam);
            if (max_count == 0 || lParam == 0) {
                m.set_return(0u);
                return;
            }
            const GuestAddr buf = static_cast<GuestAddr>(lParam);
            const u32 n = std::min(len, max_count - 1);
            for (u32 i = 0; i < n; ++i) {
                if (ansi) {
                    (void)m.write_u8(buf + i, static_cast<u8>(ansi_text_bytes[i]));
                } else {
                    (void)m.write_u16(buf + i * 2, static_cast<u16>(text[i]));
                }
            }
            if (ansi) {
                (void)m.write_u8(buf + n, 0);
            } else {
                (void)m.write_u16(buf + n * 2, 0);
            }
            m.set_return(static_cast<u64>(n));
            return;
        }
        // BS_OWNERDRAW / SS_OWNERDRAW: the class proc's WM_PAINT sends
        // WM_DRAWITEM to the parent, which does all of the drawing.
        if (msg == windowing::WM::PAINT && m_paint_mgr && m_msgsend_mgr) {
            auto* win = m_hwnd_mgr.find(hwnd);
            const u32 ctl_type = win ? owner_draw_type(*win) : 0;
            if (win && ctl_type != 0 && win->parent != windowing::NULL_HWND) {
                OwnerDrawScratch scratch(m.memory());
                const GuestAddr dis = scratch.address;
                if (dis == 0) {
                    m.set_return(0);
                    return;
                }
                PaintStruct ps{};
                const u64 hdc =
                    m_paint_mgr->begin_paint(static_cast<u64>(hwnd), ps);
                if (hdc != 0) {
                    const i32 cw =
                        win->client_rect.right - win->client_rect.left;
                    const i32 ch =
                        win->client_rect.bottom - win->client_rect.top;
                    const bool wide = m.cpu_is_64bit();
                    const auto& layout = wide ? detail::draw_item_layout64 : detail::draw_item_layout32;
                    for (u32 off = 0; off < layout.size; off += 4) {
                        (void)m.write_u32(dis + off, 0);
                    }
                    (void)m.write_u32(dis + layout.CtlType, ctl_type);
                    (void)m.write_u32(dis + layout.CtlID, win->ctrl_id);
                    (void)m.write_u32(dis + layout.itemAction, kOdaDrawEntire);
                    (void)m.write_u32(dis + layout.itemState, win->enabled ? 0u : kOdsDisabled);
                    if (wide) {
                        (void)m.write_u64(dis + layout.hwndItem, hwnd);
                        (void)m.write_u64(dis + layout.hDC, hdc);
                    } else {
                        (void)m.write_u32(dis + layout.hwndItem, static_cast<u32>(hwnd));
                        (void)m.write_u32(dis + layout.hDC, static_cast<u32>(hdc));
                    }
                    (void)m.write_u32(dis + layout.rcItem_right, static_cast<u32>(cw));
                    (void)m.write_u32(dis + layout.rcItem_bottom, static_cast<u32>(ch));
                    VITREO_TRACE(
                        "[ownerdraw] WM_DRAWITEM hwnd=0x%x parent=0x%x id=%u "
                        "type=%u hdc=0x%llx rc=%dx%d\n",
                        hwnd, win->parent, win->ctrl_id, ctl_type,
                        static_cast<unsigned long long>(hdc), cw, ch);
                    // On Windows an owner-draw control paints into the
                    // dialog's own surface, so whatever the parent's handler
                    // leaves untouched keeps showing the dialog background —
                    // for a skinned installer, its artwork. Vitreo gives the
                    // control a private framebuffer that SoftBitmap::resize
                    // fills with opaque white, which would punch a white
                    // rectangle through that artwork wherever the handler only
                    // draws a label. Start it fully transparent instead so the
                    // untouched pixels composite away.
                    if (m_gdi_mgr) {
                        m_gdi_mgr->clear_window_framebuffer(
                            static_cast<u64>(hwnd));
                    }
                    m_msgsend_mgr->send_message(
                        win->parent, kWmDrawItem,
                        static_cast<u64>(win->ctrl_id),
                        static_cast<u64>(dis));
                    // EndPaint flushes the control's framebuffer to its Cocoa
                    // overlay, which is what makes the parent's drawing show.
                    (void)m_paint_mgr->end_paint(static_cast<u64>(hwnd), ps);
                    m.set_return(0);
                    return;
                }
            }
        }
        VITREO_TRACE(
            "[user32_ext] CallWindowProc: native control hwnd=0x%x msg=0x%x "
            "unhandled -> 0\n",
            hwnd, msg);
        m.set_return(0);
        return;
    }

    if (!m_guest_call) {
        VITREO_TRACE( "[user32_ext] CallWindowProc: no guest_call wired\n");
        m.set_return(0);
        return;
    }

    bool is_64bit = m.cpu_is_64bit();
    u64 args[4] = {
        static_cast<u64>(hwnd),
        static_cast<u64>(msg),
        wParam,
        lParam
    };

    ++m_cwp_depth;
    u64 result = m_guest_call(lpPrevWndFunc, args, 4, is_64bit);
    --m_cwp_depth;

    // Trace CallWindowProc return value for WM_COMMAND
    if (msg == 0x0111 && diag_logs_enabled()) { // WM_COMMAND
        std::fprintf(stderr,
            "[btn-diag] CallWindowProc result: proc=0x%llx hwnd=0x%x id=%u -> 0x%llx\n",
            static_cast<unsigned long long>(lpPrevWndFunc), hwnd,
            static_cast<unsigned>(wParam & 0xFFFF),
            static_cast<unsigned long long>(result));
    }
    // Trace CallWindowProc return for WM_USER+ range
    if (msg >= 0x0400 && diag_logs_enabled()) {
        std::fprintf(stderr,
            "[cwp-diag] CallWindowProc result: proc=0x%llx hwnd=0x%x msg=0x%x -> 0x%llx\n",
            static_cast<unsigned long long>(lpPrevWndFunc), hwnd, msg,
            static_cast<unsigned long long>(result));
    }

    VITREO_TRACE( "[user32_ext] CallWindowProc(proc=0x%llx, hwnd=0x%x, msg=0x%x) → 0x%llx\n",
                 static_cast<unsigned long long>(lpPrevWndFunc), hwnd, msg,
                 static_cast<unsigned long long>(result));
    m.set_return(result);
}

void User32ExtManager::impl_GetDC(Marshaler& m) {
    auto hwnd = static_cast<u64>(m.arg(0));
    // A handle that is not a window: NULL, error untouched ("GetDC(bad)=NULL").
    if (hwnd != 0 && m_paint_mgr != nullptr && !m_paint_mgr->is_window(hwnd)) {
        m.set_return(0u);
        return;
    }
    if (m_gdi_mgr) {
        auto h = m_gdi_mgr->get_dc(static_cast<Handle>(hwnd));
        // Attribute the call to guest code. GetDC is a common-DC cache on
        // Windows, so an unbalanced caller is a leak that must eventually fail
        // rather than hand out unlimited DCs; the caller RVA names the loop.
        if (::vitreo::detail::g_trace_enabled) {
            u64 caller_ret = 0;
            if (auto* cpu = m.cpu()) {
                auto r = m.memory().read_u32(cpu->sp());
                if (r.is_ok()) caller_ret = static_cast<u64>(r.value());
            }
            VITREO_TRACE( "[user32_ext] GetDC(0x%llx) → GdiManager hdc=0x%x caller_ret=0x%llx\n",
                         hwnd, h, static_cast<unsigned long long>(caller_ret));
        }
        m.set_return(static_cast<u64>(h));
    } else {
        VITREO_TRACE( "[user32_ext] GetDC stub → 0xDC000001\n");
        m.set_return(0xDC000001);
    }
}

void User32ExtManager::impl_WindowFromDC(Marshaler& m) {
    const Handle hdc = static_cast<Handle>(m.arg(0));
    const Handle hwnd = m_gdi_mgr ? m_gdi_mgr->window_from_dc(hdc) : 0;
    VITREO_TRACE("[user32_ext] WindowFromDC(0x%x) -> 0x%x\n", hdc, hwnd);
    m.set_return(static_cast<u64>(hwnd));
}

void User32ExtManager::impl_GetDCEx(Marshaler& m) {
    // HDC GetDCEx(HWND hWnd, HRGN hrgnClip, DWORD flags)
    // Vitreo's GDI does not implement per-DC clip regions or the DCX_* caching
    // flags, so this returns the same window DC as GetDC. The stdcall arity
    // is 12 bytes: leaving the three arguments on the stack corrupts the
    // caller.
    auto hwnd = static_cast<u64>(m.arg(0));
    (void)m.arg(1); // hrgnClip — clip region not modeled
    u32 flags = static_cast<u32>(m.arg(2));
    if (hwnd != 0 && m_paint_mgr != nullptr && !m_paint_mgr->is_window(hwnd)) {
        m.set_return(0u);
        return;
    }
    if (m_gdi_mgr) {
        auto h = m_gdi_mgr->get_dc(static_cast<Handle>(hwnd));
        VITREO_TRACE("[user32_ext] GetDCEx(0x%llx, flags=0x%x) → hdc=0x%x\n",
                     hwnd, flags, h);
        m.set_return(static_cast<u64>(h));
    } else {
        m.set_return(0xDC000001);
    }
}

void User32ExtManager::impl_SetSysColors(Marshaler& m) {
    // BOOL SetSysColors(int cElements, const INT* lpaElements,
    //                   const COLORREF* lpaRgbValues)
    // Record the overrides so GetSysColor reflects them. Prior to this the
    // missing-export stub returned 0xFFFFFFFF without popping its 12 bytes of
    // arguments, corrupting the caller's stack.
    i32 count = static_cast<i32>(m.arg(0));
    GuestAddr elements = m.arg_ptr(1);
    GuestAddr rgb_values = m.arg_ptr(2);
    if (count < 0) count = 0;
    if (count > 64) count = 64; // sanity clamp
    u32 applied = 0;
    if (elements && rgb_values) {
        for (i32 i = 0; i < count; ++i) {
            auto idx = m.read_u32(elements + static_cast<u32>(i) * 4);
            auto rgb = m.read_u32(rgb_values + static_cast<u32>(i) * 4);
            if (idx.is_ok() && rgb.is_ok()) {
                sys_colors::set(idx.value(), rgb.value());
                ++applied;
            }
        }
    }
    VITREO_TRACE("[user32_ext] SetSysColors(count=%d) → applied %u overrides\n",
                 count, applied);
    m.set_return(1); // TRUE
}

windowing::Rect User32ExtManager::guest_virtual_screen_rect() const {
    // Exclusive / virtual fullscreen collapses the guest desktop to one
    // monitor at (0,0)-(W,H). Match GetMonitorInfo / EnumDisplaySettings
    // CURRENT so layout code does not mix host multi-monitor bounds with
    // the remapped SM_CX/CYSCREEN values.
    if (m_virtual_display_width > 0 && m_virtual_display_height > 0) {
        return windowing::Rect{0, 0, m_virtual_display_width,
                               m_virtual_display_height};
    }
    const auto desk = m_host.get_virtual_desktop_info();
    return windowing::Rect{desk.origin_x, desk.origin_y,
                           desk.origin_x + desk.width,
                           desk.origin_y + desk.height};
}

// GetSystemMetrics: desktop/system_metrics.cpp.

void User32ExtManager::impl_IsWindow(Marshaler& m) {
    auto hwnd = static_cast<windowing::HWND>(m.arg(0));
    // Windows IsWindow is process-global for HWNDs. Guest processes are
    // separate host processes, so a published HWND owned by a peer still
    // answers TRUE. Callers gate StretchDIBits present on IsWindow.
    bool valid = m_hwnd_mgr.is_valid(hwnd) ||
                 ipc::hwnd_registry_is_window(static_cast<u32>(hwnd));
    m.set_return(valid ? 1u : 0u);
}

// ---------------------------------------------------------------------------
// Window longs: GetWindowLong(Ptr)A/W and SetWindowLong(Ptr)A/W.
//
// Contract: Microsoft Learn GetWindowLongPtrW / SetWindowLongPtrW and the
// winuser.h index constants. What Learn leaves open is measured in
// tools/oracle_probe/s1win_api_probe.c ("-- window longs") and
// s1win_style_probe.c: invalid indices fail with ERROR_INVALID_INDEX, a
// positive index is valid while index + size <= cbWndExtra (unaligned is
// fine), the 32-bit functions on x64 reject GWL_WNDPROC/GWL_HINSTANCE/
// GWL_HWNDPARENT, and success never touches the last error.
// ---------------------------------------------------------------------------

namespace {

constexpr i32 kGwlWndProc    = -4;
constexpr i32 kGwlHInstance  = -6;
constexpr i32 kGwlHwndParent = -8;
constexpr i32 kGwlId         = -12;
constexpr i32 kGwlStyle      = -16;
constexpr i32 kGwlExStyle    = -20;
constexpr i32 kGwlUserData   = -21;

constexpr u32 kWsExDlgModalFrame = 0x00000001u;
constexpr u32 kWsExTopmost       = 0x00000008u;
constexpr u32 kWsExWindowEdge    = 0x00000100u;
constexpr u32 kWsExStaticEdge    = 0x00020000u;
constexpr u32 kWsExNoRedirectionBitmap = 0x00200000u;
/// Extended-style bits SetWindowLong(GWL_EXSTYLE) drops without an error
/// (measured bit by bit, s1win_style_probe "-- exstyle bits": the bits
/// CreateWindowEx rejects, plus 0x800).
constexpr u32 kWsExDroppedOnSet  = 0xF5808000u | 0x00000800u;

constexpr u32 kWmStyleChanging = 0x007C;
constexpr u32 kWmStyleChanged  = 0x007D;

bool top_level_window(const windowing::WindowData& win) {
    return win.parent == windowing::HWND_DESKTOP ||
           win.parent == windowing::NULL_HWND ||
           win.parent == windowing::HWND_MESSAGE || win.is_message_only;
}

/// WS_EX_WINDOWEDGE is derived from the frame, never stored as asked
/// (measured, s1win_style_probe "-- setwindowlong style transitions" and
/// "-- setwindowlong exstyle").
u32 derive_window_edge(u32 style, u32 ex_style) {
    const bool edge = (ex_style & kWsExDlgModalFrame) != 0 ||
        ((style & (windowing::WS::DLGFRAME | windowing::WS::THICKFRAME)) != 0 &&
         (ex_style & kWsExStaticEdge) == 0);
    return edge ? (ex_style | kWsExWindowEdge) : (ex_style & ~kWsExWindowEdge);
}

u64 read_extra(const windowing::WindowData& win, i32 off, u32 size) {
    u64 v = 0;
    for (u32 i = 0; i < size; ++i) {
        v |= static_cast<u64>(win.extra_bytes[static_cast<usize>(off) + i]) << (8 * i);
    }
    return v;
}

u64 write_extra(windowing::WindowData& win, i32 off, u64 value, u32 size) {
    u64 prev = 0;
    for (u32 i = 0; i < size; ++i) {
        const usize at = static_cast<usize>(off) + i;
        prev |= static_cast<u64>(win.extra_bytes[at]) << (8 * i);
        win.extra_bytes[at] = static_cast<u8>((value >> (8 * i)) & 0xFFu);
    }
    return prev;
}

bool extra_in_range(const windowing::WindowData& win, i32 index, u32 size) {
    return index >= 0 &&
           static_cast<u64>(index) + size <= static_cast<u64>(win.extra_bytes.size());
}

} // namespace

bool User32ExtManager::window_long_index_valid(i32 index, bool ptr_api) const {
    if (index == kGwlStyle || index == kGwlExStyle || index == kGwlUserData ||
        index == kGwlId) {
        return true;
    }
    if (index == kGwlWndProc || index == kGwlHInstance || index == kGwlHwndParent) {
        // winuser.h defines GWL_WNDPROC/GWL_HINSTANCE/GWL_HWNDPARENT only for
        // !_WIN64; the 32-bit functions reject them on x64 (measured,
        // gwl_wndproc_minus4_nonzero et al.).
        return ptr_api || !m_is_64bit;
    }
    return false;
}

u64 User32ExtManager::window_menu(windowing::HWND hwnd) const {
    auto it = m_window_menus.find(hwnd);
    return it == m_window_menus.end() ? 0 : it->second;
}

void User32ExtManager::window_long_get(Marshaler& m, bool ptr_api, bool ansi) {
    (void)ansi;  // A/W wndproc handle translation is not modelled (see report)
    const auto hwnd = static_cast<windowing::HWND>(m.arg(0));
    const i32 index = static_cast<i32>(m.arg(1));
    const u32 size = (ptr_api && m_is_64bit) ? 8u : 4u;
    const windowing::WindowData* win = m_hwnd_mgr.find(hwnd);
    if (!win) {
        set_last_error(error_codes::ERROR_INVALID_WINDOW_HANDLE);
        m.set_return(0u);
        return;
    }
    u64 value = 0;
    if (index >= 0) {
        const u32 ptr = m_is_64bit ? 8u : 4u;
        if (win->is_dialog && index == 0) {
            value = win->msg_result;                    // DWLP_MSGRESULT
        } else if (win->is_dialog && static_cast<u32>(index) == ptr) {
            value = win->wndproc;                       // DWLP_DLGPROC
        } else if (extra_in_range(*win, index, size)) {
            value = read_extra(*win, index, size);
        } else {
            set_last_error(user32_errors::ERROR_INVALID_INDEX);
            m.set_return(0u);
            return;
        }
    } else if (!window_long_index_valid(index, ptr_api)) {
        set_last_error(user32_errors::ERROR_INVALID_INDEX);
        m.set_return(0u);
        return;
    } else {
        switch (index) {
        case kGwlStyle:    value = win->style; break;
        case kGwlExStyle:  value = win->ex_style; break;
        case kGwlUserData: value = win->user_data; break;
        case kGwlWndProc:  value = effective_wndproc(*win); break;
        case kGwlHInstance: value = win->hinstance; break;
        case kGwlHwndParent:
            // A child reports its parent, a top-level window its owner
            // (Learn: GetWindowLongPtrW, GWLP_HWNDPARENT; measured
            // gwlp_hwndparent_is_owner / gwlp_hwndparent_child_is_parent).
            value = (win->style & windowing::WS::CHILD) != 0 && !top_level_window(*win)
                ? static_cast<u64>(win->parent)
                : (win->is_message_only ? static_cast<u64>(win->parent)
                                        : static_cast<u64>(win->owner));
            break;
        case kGwlId:
            // A top-level window's GWL_ID is its menu handle (measured,
            // gwlp_id_toplevel_with_menu).
            value = top_level_window(*win) ? window_menu(hwnd)
                                           : static_cast<u64>(win->ctrl_id);
            break;
        default: break;
        }
    }
    if (size == 4) value &= 0xFFFFFFFFull;
    VITREO_TRACE("[user32_ext] GetWindowLong%s%c(hwnd=0x%x, %d) -> 0x%llx\n",
                 ptr_api ? "Ptr" : "", ansi ? 'A' : 'W', hwnd, index,
                 static_cast<unsigned long long>(value));
    m.set_return(value);
}

bool User32ExtManager::change_window_style(windowing::HWND hwnd, bool ex,
                                           u32 requested, u32& previous) {
    windowing::WindowData* win = m_hwnd_mgr.find(hwnd);
    if (!win) return false;
    previous = ex ? win->ex_style : win->style;
    u32 proposed = requested;
    if (ex) {
        // WS_EX_NOREDIRECTIONBITMAP can only be given at creation (measured:
        // ERROR_INVALID_PARAMETER, nothing changes, return 0).
        if ((requested & kWsExNoRedirectionBitmap) != 0 &&
            (previous & kWsExNoRedirectionBitmap) == 0) {
            set_last_error(error_codes::ERROR_INVALID_PARAMETER);
            return false;
        }
        proposed &= ~kWsExDroppedOnSet;
        // WS_EX_TOPMOST follows the Z order; only SetWindowPos changes it.
        proposed = (proposed & ~kWsExTopmost) | (previous & kWsExTopmost);
        proposed = derive_window_edge(win->style, proposed);
    } else if (top_level_window(*win)) {
        // Every top-level window keeps WS_CLIPSIBLINGS (measured).
        proposed |= windowing::WS::CLIPSIBLINGS;
    }

    // WM_STYLECHANGING / WM_STYLECHANGED carry a STYLESTRUCT the procedure
    // may edit (Learn: WM_STYLECHANGING, "styleNew ... can be changed").
    const u64 which = static_cast<u64>(static_cast<i64>(ex ? kGwlExStyle : kGwlStyle));
    auto frame = m_thread_scratch.reserve(8);
    const GuestAddr ss = frame.address();
    const bool send = ss != 0 && m_msgsend_mgr != nullptr && win->wndproc != 0;
    if (send) {
        // WM_STYLECHANGING shows the caller's value as given; USER's own
        // adjustments (WS_CLIPSIBLINGS, WS_EX_TOPMOST, WS_EX_WINDOWEDGE) are
        // applied afterwards and appear in WM_STYLECHANGED (measured,
        // s1win_msgseq_probe setwindowlong_style_hidden: CHANGING 80800000,
        // CHANGED 84800000; setwindowlong_exstyle_topmost_bit: 88 then 80).
        (void)m_memory.write_u32(ss + 0, previous);
        (void)m_memory.write_u32(ss + 4, requested);
        (void)m_msgsend_mgr->send_message(hwnd, kWmStyleChanging, which, ss);
        const u32 edited = m_memory.read_u32(ss + 4).value_or(requested);
        win = m_hwnd_mgr.find(hwnd);
        if (!win) return true;
        if (edited != requested) {
            proposed = edited;
            if (ex) proposed &= ~kWsExDroppedOnSet;
        }
    }
    u32 final_value = proposed;
    if (ex) {
        final_value = (final_value & ~kWsExTopmost) | (win->ex_style & kWsExTopmost);
        final_value = derive_window_edge(win->style, final_value);
        win->ex_style = final_value;
    } else {
        if (top_level_window(*win)) final_value |= windowing::WS::CLIPSIBLINGS;
        win->style = final_value;
        // Learn IsWindowVisible / IsWindowEnabled / IsIconic / IsZoomed: these
        // read the style bits, so the shared state follows them.
        const bool now_visible = (final_value & windowing::WS::VISIBLE) != 0;
        if (win->visible != now_visible) {
            win->visible = now_visible;
            ipc::hwnd_registry_set_visible(static_cast<u32>(hwnd), now_visible);
        }
        win->enabled = (final_value & windowing::WS::DISABLED) == 0;
        win->minimized = (final_value & windowing::WS::MINIMIZE) != 0;
        win->maximized = (final_value & windowing::WS::MAXIMIZE) != 0;
        win->ex_style = derive_window_edge(final_value, win->ex_style);
    }
    // Learn (SetLayeredWindowAttributes): clearing WS_EX_LAYERED ends the
    // window's layering; setting it again starts from nothing.
    constexpr u32 kWsExLayered = 0x00080000u;
    if (ex && (previous & kWsExLayered) != 0 && (final_value & kWsExLayered) == 0) {
        layered_style_cleared(hwnd);
    }
    if (send) {
        (void)m_memory.write_u32(ss + 0, previous);
        (void)m_memory.write_u32(ss + 4, final_value);
        (void)m_msgsend_mgr->send_message(hwnd, kWmStyleChanged, which, ss);
    }
    return true;
}

void User32ExtManager::window_long_set(Marshaler& m, bool ptr_api, bool ansi) {
    const auto hwnd = static_cast<windowing::HWND>(m.arg(0));
    const i32 index = static_cast<i32>(m.arg(1));
    const u32 size = (ptr_api && m_is_64bit) ? 8u : 4u;
    u64 value = m.arg(2);
    if (size == 4) value &= 0xFFFFFFFFull;
    windowing::WindowData* win = m_hwnd_mgr.find(hwnd);
    if (!win) {
        set_last_error(error_codes::ERROR_INVALID_WINDOW_HANDLE);
        m.set_return(0u);
        return;
    }
    u64 previous = 0;
    if (index >= 0) {
        const u32 ptr = m_is_64bit ? 8u : 4u;
        if (win->is_dialog && index == 0) {
            previous = win->msg_result;                 // DWLP_MSGRESULT
            win->msg_result = value;
        } else if (win->is_dialog && static_cast<u32>(index) == ptr) {
            previous = win->wndproc;                    // DWLP_DLGPROC
            win->wndproc = value;
        } else if (extra_in_range(*win, index, size)) {
            previous = write_extra(*win, index, value, size);
        } else {
            set_last_error(user32_errors::ERROR_INVALID_INDEX);
            m.set_return(0u);
            return;
        }
    } else if (!window_long_index_valid(index, ptr_api)) {
        set_last_error(user32_errors::ERROR_INVALID_INDEX);
        m.set_return(0u);
        return;
    } else {
        switch (index) {
        case kGwlStyle:
        case kGwlExStyle: {
            u32 prev32 = 0;
            if (!change_window_style(hwnd, index == kGwlExStyle,
                                     static_cast<u32>(value), prev32)) {
                m.set_return(0u);
                return;
            }
            previous = prev32;
            break;
        }
        case kGwlUserData:
            previous = win->user_data;
            win->user_data = value;
            break;
        case kGwlWndProc: {
            previous = effective_wndproc(*win);
            const bool begins_guest_painting = win->wndproc == 0 && value != 0;
            // Track the original (pre-subclass) wndproc so CallWindowProc can
            // chain to the real base proc when the window is subclassed
            // twice without a restore. Native controls start at wndproc==0;
            // remember the sentinel so CallWindowProc(old, LB_ADDSTRING, …)
            // still fills the Cocoa list.
            {
                std::lock_guard<std::mutex> procs(m_wndproc_mutex);
                if (m_original_wndproc.find(hwnd) == m_original_wndproc.end() &&
                    value != previous) {
                    m_original_wndproc[hwnd] = previous;
                }
            }
            win->wndproc = value;
            // The window's charset follows the function that installed the
            // procedure (measured: iswindowunicode_after_a_subclass = 0,
            // iswindowunicode_after_w_subclass = 1).
            win->unicode = !ansi;
            if (begins_guest_painting && win->visible && m_paint_mgr) {
                m_paint_mgr->invalidate_rect(static_cast<u64>(hwnd), nullptr, true);
            }
            break;
        }
        case kGwlHInstance:
            previous = win->hinstance;
            win->hinstance = value;
            break;
        case kGwlHwndParent: {
            const auto target = static_cast<windowing::HWND>(value);
            if ((win->style & windowing::WS::CHILD) != 0 && !top_level_window(*win)) {
                // A child is re-parented and the old parent returned (measured,
                // hwndparent_child_set_returns_parent / child_getparent_is_o2).
                windowing::HWND old_parent = windowing::NULL_HWND;
                previous = win->parent;
                if (!apply_set_parent(hwnd, target == windowing::NULL_HWND
                                                ? windowing::HWND_DESKTOP
                                                : target,
                                      old_parent)) {
                    set_last_error(error_codes::ERROR_INVALID_PARAMETER);
                    m.set_return(0u);
                    return;
                }
                break;
            }
            // A top-level window's owner. Measured (s1win_style_probe):
            // itself or a non-window is ERROR_INVALID_PARAMETER; NULL clears
            // the owner and reports ERROR_INVALID_WINDOW_HANDLE; any window
            // (even a child) becomes the owner as given.
            if (target == hwnd ||
                (target != windowing::NULL_HWND && !m_hwnd_mgr.is_valid(target) &&
                 !ipc::hwnd_registry_is_window(static_cast<u32>(target)))) {
                set_last_error(error_codes::ERROR_INVALID_PARAMETER);
                m.set_return(0u);
                return;
            }
            previous = win->owner;
            win->owner = target;
            if (target == windowing::NULL_HWND) {
                set_last_error(error_codes::ERROR_INVALID_WINDOW_HANDLE);
            }
            break;
        }
        case kGwlId:
            if (top_level_window(*win)) {
                // A top-level window's identifier is its menu: the value must
                // be a menu or NULL (measured, swl_id_popup:
                // ERROR_INVALID_MENU_HANDLE; id_toplevel_valid_menu attaches it).
                if (value != 0 && !is_menu_handle(static_cast<u32>(value))) {
                    set_last_error(error_codes::ERROR_INVALID_MENU_HANDLE);
                    m.set_return(0u);
                    return;
                }
                previous = window_menu(hwnd);
                (void)set_window_menu(hwnd, static_cast<u32>(value));
            } else {
                previous = win->ctrl_id;
                win->ctrl_id = static_cast<u32>(value);
            }
            break;
        default: break;
        }
    }
    if (size == 4) previous &= 0xFFFFFFFFull;
    VITREO_TRACE("[user32_ext] SetWindowLong%s%c(hwnd=0x%x, %d, 0x%llx) -> prev 0x%llx\n",
                 ptr_api ? "Ptr" : "", ansi ? 'A' : 'W', hwnd, index,
                 static_cast<unsigned long long>(value),
                 static_cast<unsigned long long>(previous));
    m.set_return(previous);
}

void User32ExtManager::impl_GetWindowLongW(Marshaler& m) { window_long_get(m, false, false); }
void User32ExtManager::impl_GetWindowLongPtrW(Marshaler& m) { window_long_get(m, true, false); }
void User32ExtManager::impl_SetWindowLongW(Marshaler& m) { window_long_set(m, false, false); }
void User32ExtManager::impl_SetWindowLongPtrW(Marshaler& m) { window_long_set(m, true, false); }

// ---------------------------------------------------------------------------
// Class longs: GetClassLong(Ptr)A/W, SetClassLong(Ptr)A/W, Get/SetClassWord.
//
// Contract: Learn GetClassLongPtrW / SetClassLongPtrW / GetClassWord and the
// winuser.h GCL_* / GCLP_* / GCW_ATOM indices; measured in s1win_api_probe
// ("-- classes") and s1win_class_probe: invalid indices and extra-memory
// offsets past cbClsExtra fail with ERROR_INVALID_INDEX; on x64 the 32-bit
// functions reject the pointer-valued indices; GCW_ATOM cannot be set
// (SetClassLong: ERROR_INVALID_PARAMETER, SetClassWord: ERROR_INVALID_INDEX).
// ---------------------------------------------------------------------------

namespace {

constexpr i32 kGclMenuName      = -8;
constexpr i32 kGclHbrBackground = -10;
constexpr i32 kGclHCursor       = -12;
constexpr i32 kGclHIcon         = -14;
constexpr i32 kGclHModule       = -16;
constexpr i32 kGclCbWndExtra    = -18;
constexpr i32 kGclCbClsExtra    = -20;
constexpr i32 kGclWndProc       = -24;
constexpr i32 kGclStyle         = -26;
constexpr i32 kGcwAtom          = -32;
constexpr i32 kGclHIconSm       = -34;

bool pointer_class_index(i32 index) {
    return index == kGclMenuName || index == kGclHbrBackground ||
           index == kGclHCursor || index == kGclHIcon || index == kGclHModule ||
           index == kGclWndProc || index == kGclHIconSm;
}

} // namespace

WindowClassData* User32ExtManager::window_class(const windowing::WindowData& win) {
    if (WindowClassData* cls = m_class_mgr.find_by_id_mut(win.class_id)) return cls;
    if (win.class_atom >= 0xC000) return m_class_mgr.find_by_atom_mut(win.class_atom);
    if (win.is_dialog) {
        return const_cast<WindowClassData*>(m_class_mgr.find_by_name(L"#32770"));
    }
    if (!win.class_name.empty()) {
        return const_cast<WindowClassData*>(m_class_mgr.find_by_name(win.class_name));
    }
    return nullptr;
}

void User32ExtManager::class_long_get(Marshaler& m, u32 width) {
    const auto hwnd = static_cast<windowing::HWND>(m.arg(0));
    const i32 index = static_cast<i32>(m.arg(1));
    const windowing::WindowData* win = m_hwnd_mgr.find(hwnd);
    if (!win) {
        set_last_error(error_codes::ERROR_INVALID_WINDOW_HANDLE);
        m.set_return(0u);
        return;
    }
    const WindowClassData* cls = window_class(*win);
    u64 value = 0;
    bool valid = cls != nullptr;
    if (valid && index >= 0) {
        valid = static_cast<u64>(index) + width <= cls->extra.size();
        if (valid) {
            for (u32 i = 0; i < width; ++i) {
                value |= static_cast<u64>(cls->extra[static_cast<usize>(index) + i]) << (8 * i);
            }
        }
    } else if (valid) {
        if (width == 2) {
            valid = index == kGcwAtom;
        } else if (width == 4 && m_is_64bit && pointer_class_index(index)) {
            valid = false;
        }
        if (valid) {
            switch (index) {
            case kGclMenuName:      value = cls->menu_name_ptr; break;
            case kGclHbrBackground: value = cls->hbr_background; break;
            case kGclHCursor:       value = m_class_mgr.class_cursor(*cls); break;
            case kGclHIcon:         value = cls->hicon; break;
            case kGclHModule:
                value = cls->system ? m_class_mgr.system_module() : cls->hinstance;
                break;
            case kGclCbWndExtra:    value = static_cast<u32>(cls->wnd_extra); break;
            case kGclCbClsExtra:    value = static_cast<u32>(cls->cls_extra); break;
            case kGclWndProc:
                value = resolve_system_class_wndproc(cls->wndproc);
                break;
            case kGclStyle:         value = cls->style; break;
            case kGcwAtom:          value = cls->atom; break;
            case kGclHIconSm:       value = cls->hicon_sm; break;
            default:                valid = false; break;
            }
        }
    }
    if (!valid) {
        set_last_error(user32_errors::ERROR_INVALID_INDEX);
        m.set_return(0u);
        return;
    }
    if (width == 2) value &= 0xFFFFull;
    if (width == 4) value &= 0xFFFFFFFFull;
    m.set_return(value);
}

void User32ExtManager::class_long_set(Marshaler& m, u32 width) {
    const auto hwnd = static_cast<windowing::HWND>(m.arg(0));
    const i32 index = static_cast<i32>(m.arg(1));
    u64 value = m.arg(2);
    if (width == 2) value &= 0xFFFFull;
    if (width == 4) value &= 0xFFFFFFFFull;
    const windowing::WindowData* win = m_hwnd_mgr.find(hwnd);
    if (!win) {
        set_last_error(error_codes::ERROR_INVALID_WINDOW_HANDLE);
        m.set_return(0u);
        return;
    }
    WindowClassData* cls = window_class(*win);
    if (!cls) {
        set_last_error(user32_errors::ERROR_INVALID_INDEX);
        m.set_return(0u);
        return;
    }
    u64 previous = 0;
    if (index >= 0) {
        if (static_cast<u64>(index) + width > cls->extra.size()) {
            set_last_error(user32_errors::ERROR_INVALID_INDEX);
            m.set_return(0u);
            return;
        }
        for (u32 i = 0; i < width; ++i) {
            const usize at = static_cast<usize>(index) + i;
            previous |= static_cast<u64>(cls->extra[at]) << (8 * i);
            cls->extra[at] = static_cast<u8>((value >> (8 * i)) & 0xFFu);
        }
        m.set_return(previous);
        return;
    }
    if (width == 2 || (width == 4 && m_is_64bit && pointer_class_index(index))) {
        set_last_error(user32_errors::ERROR_INVALID_INDEX);
        m.set_return(0u);
        return;
    }
    switch (index) {
    case kGclMenuName:      previous = cls->menu_name_ptr; cls->menu_name_ptr = value; break;
    case kGclHbrBackground: previous = cls->hbr_background; cls->hbr_background = value; break;
    case kGclHCursor:
        previous = m_class_mgr.class_cursor(*cls);
        cls->hcursor = value;
        cls->arrow_cursor = false;
        break;
    case kGclHIcon:         previous = cls->hicon; cls->hicon = value; break;
    case kGclHModule:
        previous = cls->system ? m_class_mgr.system_module() : cls->hinstance;
        cls->hinstance = value;
        break;
    // Learn SetClassLongPtrW: GCL_CBWNDEXTRA / GCL_CBCLSEXTRA set the size
    // "but do not change the number of extra bytes already allocated".
    case kGclCbWndExtra:
        previous = static_cast<u32>(cls->wnd_extra);
        cls->wnd_extra = static_cast<i32>(value);
        break;
    case kGclCbClsExtra:
        previous = static_cast<u32>(cls->cls_extra);
        cls->cls_extra = static_cast<i32>(value);
        break;
    case kGclWndProc:
        // Existing windows keep their procedure; new ones get this one
        // (measured, existing_window_keeps_proc / new_window_uses_new_proc).
        previous = resolve_system_class_wndproc(cls->wndproc);
        cls->wndproc = value;
        break;
    case kGclStyle:         previous = cls->style; cls->style = static_cast<u32>(value); break;
    case kGclHIconSm:       previous = cls->hicon_sm; cls->hicon_sm = value; break;
    case kGcwAtom:
        // Measured (setclasslong_atom): ERROR_INVALID_PARAMETER.
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0u);
        return;
    default:
        set_last_error(user32_errors::ERROR_INVALID_INDEX);
        m.set_return(0u);
        return;
    }
    if (width == 4) previous &= 0xFFFFFFFFull;
    m.set_return(previous);
}

void User32ExtManager::impl_SetClassLongW(Marshaler& m) { class_long_set(m, 4); }
} // namespace vitreo::winapi
