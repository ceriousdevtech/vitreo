// SPDX-License-Identifier: LGPL-2.1-only

/// @file control_richedit.cpp
/// @brief Edit/RichEdit marshaled messages (EM_SETTEXTEX, EM_STREAMIN, EM_SETSEL,
///        EM_GETSCROLLPOS/EM_SETSCROLLPOS).

#include "winapi/user32/user32_internal.hpp"
#include "windowing/resources/resource_walker.hpp"

#include "winapi/user32/messages/message_send_internal.hpp"
#include "winapi/user32/messages/ansi_char_message.hpp"
#include "winapi/user32/messages/sdk_richedit_stream.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/graphics/gdi32/gdi32.hpp"
#include "winapi/kernel32/thread/thread.hpp"
#include "core/memory/guest_memory.hpp"
#include "windowing/model/diag_flags.hpp"
#include "ipc/ipc_gate.hpp"
#include "winapi/kernel32/memory/remote_memory.hpp"
#include "winapi/user32/messages/cross_process_send.hpp"
#include "ipc/hwnd_registry.hpp"
#include "ipc/process_registry.hpp"
#include <unistd.h>
#include <algorithm>
#include <pthread.h>
#include <cstdlib>
#include <optional>
#include <string>
#include <cstdio>
#include <unordered_map>
#include <vector>

namespace vitreo::winapi {

// RichEdit messages: EM_SETTEXTEX, EM_STREAMIN, EM_SETSEL, and scroll position.
constexpr u32 EM_STREAMIN_MSG = 0x0449;   // WM_USER+73
constexpr u32 EM_SETTEXTEX_MSG = 0x0461;  // WM_USER+97
constexpr u32 EM_GETSCROLLPOS_MSG = 0x04DD; // WM_USER+221
constexpr u32 EM_SETSCROLLPOS_MSG = 0x04DE; // WM_USER+222
constexpr u32 EM_SETSEL_MSG = 0x00B1;       // EDIT selection (wParam=start, lParam=end)
constexpr u32 SF_TEXT_FLAG = 0x0001;
constexpr u32 SF_RTF_FLAG = 0x0002;
constexpr u32 SF_UNICODE_FLAG = 0x0010;
constexpr u32 CP_UNICODE_TEXTEX = 1200;

// Per-invocation storage remains valid during reentrant callbacks and belongs
// to this GuestMemory only. map_first_fit reserves the range atomically.
struct RichEditStreamScratch {
    GuestMemory& memory;
    GuestAddr address;
    static constexpr u32 bytes = 8192;
    explicit RichEditStreamScratch(GuestMemory& mem) : memory(mem),
        address(mem.map_first_fit(0x3F001000, bytes, MemoryProtection::ReadWrite)) {}
    ~RichEditStreamScratch() { if (address) (void)memory.unmap_region(address, bytes); }
    RichEditStreamScratch(const RichEditStreamScratch&) = delete;
    RichEditStreamScratch& operator=(const RichEditStreamScratch&) = delete;
};

static bool text_looks_like_rtf_utf8(const std::string& s) {
    return s.size() >= 5 && s.compare(0, 5, "{\\rtf") == 0;
}

/// True when guest memory at @p ptr looks like 8-bit text (e.g. ANSI RTF
/// `{\rtf`) rather than UTF-16LE (`{\0\\\0r\0t\0f\0`). EM_SETTEXTEX codepage
/// 1200 (UTF-16) is sometimes paired with an ANSI buffer; decoding that as
/// UTF-16 produces mojibake.
static bool guest_text_looks_ansi(Marshaler& m, GuestAddr ptr) {
    auto b0 = m.memory().read_u8(ptr);
    auto b1 = m.memory().read_u8(ptr + 1);
    auto b2 = m.memory().read_u8(ptr + 2);
    auto b3 = m.memory().read_u8(ptr + 3);
    if (!b0.is_ok() || !b1.is_ok() || !b2.is_ok() || !b3.is_ok()) {
        return false;
    }
    const u8 c0 = b0.value();
    const u8 c1 = b1.value();
    const u8 c2 = b2.value();
    const u8 c3 = b3.value();
    // ANSI RTF / ASCII: "{ \ r t"
    if (c0 == '{' && c1 == '\\' && c2 == 'r' && c3 == 't') {
        return true;
    }
    // UTF-16LE RTF: "{ \0 \ \0"
    if (c0 == '{' && c1 == 0 && c2 == '\\' && c3 == 0) {
        return false;
    }
    // Heuristic: printable ASCII followed by another printable (not NUL) → ANSI.
    if (c0 >= 0x20 && c0 < 0x7F && c1 >= 0x20 && c1 < 0x7F) {
        return true;
    }
    // ASCII + NUL → likely UTF-16LE BMP character.
    if (c0 >= 0x20 && c0 < 0x7F && c1 == 0) {
        return false;
    }
    return false;
}

bool try_handle_richedit_settextex(Marshaler& m, MessageSendManager& msm,
                                   windowing::HWND hwnd, u32 msg,
                                   u64 wParam, u64 lParam) {
    if (msg != EM_SETTEXTEX_MSG || msm.wndproc_for(hwnd) != 0) {
        return false;
    }
    // NULL lParam clears the control (Windows).
    if (lParam == 0) {
        (void)msm.set_control_text(hwnd, L"");
        m.set_return(1u);
        return true;
    }
    if (wParam == 0) {
        m.set_return(0u);
        return true;
    }

    const GuestAddr settextex = static_cast<GuestAddr>(wParam);
    auto flags_res = m.read_u32(settextex);
    auto cp_res = m.read_u32(settextex + 4);
    const u32 flags = flags_res.is_ok() ? flags_res.value() : 0;
    const u32 codepage = cp_res.is_ok() ? cp_res.value() : 0;
    (void)flags;

    const GuestAddr text_ptr = static_cast<GuestAddr>(lParam);
    // Prefer the buffer's actual encoding over SETTEXTEX.codepage when they
    // disagree. Codepage 1200 (UTF-16) is sometimes paired with an ANSI
    // `{\rtf1\ansi...` buffer.
    const bool ansi_buffer = guest_text_looks_ansi(m, text_ptr);
    const bool read_as_utf16 =
        (codepage == CP_UNICODE_TEXTEX) && !ansi_buffer;

    bool ok = false;
    usize char_count = 0;
    if (read_as_utf16) {
        // Marshaler returns UTF-8 converted from guest UTF-16LE.
        auto str_res = m.read_wide_string(text_ptr);
        if (!str_res.is_ok()) {
            m.set_return(0u);
            return true;
        }
        std::string utf8 = str_res.value();
        std::wstring ws = message_send_utf8_to_wstring(utf8);
        char_count = ws.size();
        if (text_looks_like_rtf_utf8(utf8)) {
            ok = msm.bridge().set_control_rich_content(
                hwnd, reinterpret_cast<const u8*>(utf8.data()), utf8.size(), true);
        } else {
            ok = msm.set_control_text(hwnd, ws);
        }
    } else {
        auto str_res = m.read_ansi_string(text_ptr);
        if (!str_res.is_ok()) {
            m.set_return(0u);
            return true;
        }
        std::string ansi = str_res.value();
        char_count = ansi.size();
        if (text_looks_like_rtf_utf8(ansi)) {
            ok = msm.bridge().set_control_rich_content(
                hwnd, reinterpret_cast<const u8*>(ansi.data()), ansi.size(), true);
        } else {
            ok = msm.set_control_text(hwnd, message_send_utf8_to_wstring(ansi));
        }
    }

    VITREO_TRACE(
        "[send] EM_SETTEXTEX hwnd=0x%x flags=0x%x cp=%u ansi_buf=%d chars=%zu -> %s\n",
        hwnd, flags, codepage, ansi_buffer ? 1 : 0, char_count, ok ? "ok" : "fail");
    // Windows returns the number of TCHARs set on success.
    m.set_return(ok ? static_cast<u64>(char_count) : 0u);
    return true;
}

bool try_handle_richedit_streamin(Marshaler& m, MessageSendManager& msm,
                                  windowing::HWND hwnd, u32 msg,
                                  u64 wParam, u64 lParam, bool force_native) {
    // SendMessage delivers EM_STREAMIN to the current wndproc first. A
    // subclass that CallWindowProc's the rich-edit/edit base procedure is
    // asking that procedure to read the stream; the guest wndproc still
    // being installed must not skip the base implementation.
    if (msg != EM_STREAMIN_MSG || lParam == 0 ||
        (!force_native && msm.wndproc_for(hwnd) != 0)) {
        return false;
    }
    if (!msm.guest_call()) {
        VITREO_TRACE("[send] EM_STREAMIN hwnd=0x%x — no guest_call\n", hwnd);
        m.set_return(0u);
        return true;
    }

    const bool is64 = m.cpu_is_64bit();
    const GuestAddr stream = static_cast<GuestAddr>(lParam);
    GuestAddr cookie = 0;
    GuestAddr callback = 0;
    if (is64) {
        auto c = m.read_u64(stream);
        auto cb = m.read_u64(stream + offsetof(sdk_richedit_stream::EDITSTREAM<u64>, pfnCallback));
        if (!c.is_ok() || !cb.is_ok()) {
            m.set_return(0u);
            return true;
        }
        cookie = static_cast<GuestAddr>(c.value());
        callback = static_cast<GuestAddr>(cb.value());
    } else {
        auto c = m.read_u32(stream);
        auto cb = m.read_u32(stream + offsetof(sdk_richedit_stream::EDITSTREAM<u32>, pfnCallback));
        if (!c.is_ok() || !cb.is_ok()) {
            m.set_return(0u);
            return true;
        }
        cookie = static_cast<GuestAddr>(c.value());
        callback = static_cast<GuestAddr>(cb.value());
    }
    if (callback == 0) {
        m.set_return(0u);
        return true;
    }

    constexpr u32 kChunk = 4096;
    RichEditStreamScratch storage(m.memory());
    const GuestAddr scratch = storage.address;
    if (scratch == 0) {
        VITREO_TRACE("[send] EM_STREAMIN hwnd=0x%x — no guest scratch\n", hwnd);
        m.set_return(0u);
        return true;
    }
    const GuestAddr guest_buf = scratch;
    const GuestAddr guest_pcb = scratch + kChunk;

    std::vector<u8> collected;
    collected.reserve(64 * 1024);
    u32 stream_error = 0;
    for (;;) {
        (void)m.write_u32(guest_pcb, 0);
        const u64 args[4] = {
            static_cast<u64>(cookie),
            static_cast<u64>(guest_buf),
            static_cast<u64>(kChunk),
            static_cast<u64>(guest_pcb),
        };
        const u32 cb_ret = static_cast<u32>(msm.guest_call()(callback, args, 4, is64));
        // A failing callback's buffer is discarded (EDITSTREAMCALLBACK).
        if (cb_ret != 0) { stream_error = cb_ret; break; }
        auto pcb_res = m.read_u32(guest_pcb);
        const u32 got = pcb_res.is_ok() ? pcb_res.value() : 0;
        if (got > 0 && got <= kChunk) {
            std::vector<u8> chunk(got);
            auto rd = m.memory().read_bytes(guest_buf, chunk.data(), got);
            if (rd.is_ok()) {
                collected.insert(collected.end(), chunk.begin(), chunk.end());
            }
        }
        if (got == 0) break;
        if (collected.size() > 8u * 1024u * 1024u) {
            stream_error = 8;
            break;
        }
    }

    if (is64) {
        (void)m.write_u32(stream + offsetof(sdk_richedit_stream::EDITSTREAM<u64>, dwError), stream_error);
    } else {
        (void)m.write_u32(stream + offsetof(sdk_richedit_stream::EDITSTREAM<u32>, dwError), stream_error);
    }

    const u32 flags = static_cast<u32>(wParam);
    const bool is_rtf = (flags & SF_RTF_FLAG) != 0;
    bool ok = false;
    if (!collected.empty()) {
        if ((flags & SF_UNICODE_FLAG) != 0 && !is_rtf && collected.size() >= 2) {
            std::wstring wide;
            wide.resize(collected.size() / 2);
            for (size_t i = 0; i + 1 < collected.size(); i += 2) {
                wide[i / 2] = static_cast<wchar_t>(
                    collected[i] | (static_cast<u16>(collected[i + 1]) << 8));
            }
            while (!wide.empty() && wide.back() == L'\0') wide.pop_back();
            std::string utf8 = wstring_to_utf8(wide);
            ok = msm.bridge().set_control_rich_content(
                hwnd, reinterpret_cast<const u8*>(utf8.data()), utf8.size(), false);
        } else {
            // SF_RTF, and a stream that is not SF_TEXT, is RTF.
            const bool treat_rtf = is_rtf || (flags & SF_TEXT_FLAG) == 0;
            ok = msm.bridge().set_control_rich_content(
                hwnd, collected.data(), collected.size(), treat_rtf);
        }
    }

    VITREO_TRACE(
        "[send] EM_STREAMIN hwnd=0x%x flags=0x%x bytes=%zu err=%u -> %s\n",
        hwnd, flags, collected.size(), stream_error, ok ? "ok" : "fail");
    m.set_return(ok ? static_cast<u64>(collected.size()) : 0u);
    return true;
}

bool try_handle_edit_setsel(Marshaler& m, MessageSendManager& msm,
                            windowing::HWND hwnd, u32 msg,
                            u64 wParam, u64 lParam) {
    if (msg != EM_SETSEL_MSG) {
        return false;
    }
    // Win32: wParam = start, lParam = end; end == -1 means "to end of text".
    const i32 start = static_cast<i32>(static_cast<u32>(wParam));
    const i32 end = static_cast<i32>(lParam);
    if (!msm.bridge().set_edit_selection(hwnd, start, end)) {
        return false;
    }
    VITREO_TRACE("[send] EM_SETSEL hwnd=0x%x start=%d end=%d\n", hwnd, start, end);
    m.set_return(1u);
    return true;
}

bool try_handle_richedit_scrollpos(Marshaler& m, MessageSendManager& msm,
                                   windowing::HWND hwnd, u32 msg,
                                   u64 wParam, u64 lParam) {
    (void)wParam;
    if (msg != EM_SETSCROLLPOS_MSG && msg != EM_GETSCROLLPOS_MSG) {
        return false;
    }
    // Handle even when the control is subclassed. The owner may
    // SetWindowLong(GWL_WNDPROC) and then SendMessage EM_SETSCROLLPOS.
    if (lParam == 0) {
        return false;
    }
    const GuestAddr pt = static_cast<GuestAddr>(lParam);
    if (msg == EM_GETSCROLLPOS_MSG) {
        i32 x = 0;
        i32 y = 0;
        if (!msm.bridge().get_control_scroll_pos(hwnd, x, y)) {
            return false; // not an NSScrollView-backed control
        }
        (void)m.write_u32(pt, static_cast<u32>(x));
        (void)m.write_u32(pt + 4, static_cast<u32>(y));
        VITREO_TRACE("[send] EM_GETSCROLLPOS hwnd=0x%x -> (%d,%d)\n", hwnd, x, y);
        m.set_return(1u);
        return true;
    }
    auto x_res = m.read_u32(pt);
    auto y_res = m.read_u32(pt + 4);
    const i32 x = static_cast<i32>(x_res.is_ok() ? x_res.value() : 0);
    const i32 y = static_cast<i32>(y_res.is_ok() ? y_res.value() : 0);
    if (!msm.bridge().set_control_scroll_pos(hwnd, x, y)) {
        return false;
    }
    VITREO_TRACE("[send] EM_SETSCROLLPOS hwnd=0x%x (%d,%d) -> ok\n", hwnd, x, y);
    m.set_return(1u);
    return true;
}

} // namespace vitreo::winapi
