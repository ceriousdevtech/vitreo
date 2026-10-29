// SPDX-License-Identifier: LGPL-2.1-only

/// @file cross_process_send.cpp
/// @brief See cross_process_send.hpp.

#include "winapi/user32/messages/cross_process_send.hpp"

#include "winapi/user32/messages/message_send.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/kernel32/memory/heap.hpp"
#include "winapi/kernel32/locale/codepage_convert.hpp"
#include "winapi/kernel32/locale/string_conv.hpp"
#include "core/memory/guest_memory.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <set>

namespace vitreo::winapi::xproc_send {
namespace {

// winuser.h (10.0.26100) message numbers used below.
constexpr u32 WM_SETTEXT = 0x000C;
constexpr u32 WM_GETTEXT = 0x000D;
constexpr u32 WM_SETTINGCHANGE = 0x001A;  // == WM_WININICHANGE
constexpr u32 WM_DEVMODECHANGE = 0x001B;
constexpr u32 WM_COPYDATA = 0x004A;
constexpr u32 WM_USER = 0x0400;
constexpr u32 kMaxTextChars = 256 * 1024;

void append(std::vector<Byte>& v, const void* p, usize n) {
    const auto* b = static_cast<const Byte*>(p);
    v.insert(v.end(), b, b + n);
}

std::vector<u16> read_text(Marshaler& m, bool ansi, GuestAddr p, bool& ok) {
    ok = true;
    if (ansi) {
        auto r = m.read_ansi_string(p, kMaxTextChars);
        if (r.is_err()) { ok = false; return {}; }
        const std::string& bytes = r.value();
        std::vector<u16> w(bytes.size() + 1);
        const auto res = codepage::multi_byte_to_wide(
            0 /*CP_ACP*/, 0, reinterpret_cast<const u8*>(bytes.data()),
            bytes.size(), w.data(), w.size());
        w.resize(res.count);
        return w;
    }
    auto r = m.read_wide_string(p, kMaxTextChars);
    if (r.is_err()) { ok = false; return {}; }
    return utf8_to_utf16(r.value());
}

} // namespace

Kind classify(u32 msg) {
    if (msg >= WM_USER) return Kind::Raw;
    switch (msg) {
    case WM_SETTEXT:
    case WM_SETTINGCHANGE:
    case WM_DEVMODECHANGE:
        return Kind::StringIn;
    case WM_GETTEXT:
        return Kind::TextOut;
    case WM_COPYDATA:
        return Kind::CopyData;
    // System messages whose wParam and lParam are both plain values
    // (winuser.h numbers; parameter meanings from each message's Learn page).
    case 0x0000: // WM_NULL
    case 0x0002: // WM_DESTROY
    case 0x0003: // WM_MOVE
    case 0x0005: // WM_SIZE
    case 0x0006: // WM_ACTIVATE
    case 0x0007: // WM_SETFOCUS
    case 0x0008: // WM_KILLFOCUS
    case 0x000A: // WM_ENABLE
    case 0x000B: // WM_SETREDRAW
    case 0x000E: // WM_GETTEXTLENGTH
    case 0x0010: // WM_CLOSE
    case 0x0011: // WM_QUERYENDSESSION
    case 0x0012: // WM_QUIT
    case 0x0013: // WM_QUERYOPEN
    case 0x0015: // WM_SYSCOLORCHANGE
    case 0x0016: // WM_ENDSESSION
    case 0x0018: // WM_SHOWWINDOW
    case 0x001C: // WM_ACTIVATEAPP
    case 0x001D: // WM_FONTCHANGE
    case 0x001E: // WM_TIMECHANGE
    case 0x001F: // WM_CANCELMODE
    case 0x0022: // WM_CHILDACTIVATE
    case 0x0050: // WM_INPUTLANGCHANGEREQUEST
    case 0x0051: // WM_INPUTLANGCHANGE
    case 0x0054: // WM_USERCHANGED
    case 0x007E: // WM_DISPLAYCHANGE
    case 0x0086: // WM_NCACTIVATE
    case 0x0111: // WM_COMMAND
    case 0x0112: // WM_SYSCOMMAND
    case 0x0218: // WM_POWERBROADCAST
    case 0x02A1: // WM_MOUSEHOVER
    case 0x02A3: // WM_MOUSELEAVE
    case 0x02B1: // WM_WTSSESSION_CHANGE
    case 0x030F: // WM_QUERYNEWPALETTE
    case 0x0312: // WM_HOTKEY
    case 0x0319: // WM_APPCOMMAND
    case 0x031A: // WM_THEMECHANGED
    case 0x031D: // WM_CLIPBOARDUPDATE
    case 0x031E: // WM_DWMCOMPOSITIONCHANGED
        return Kind::Raw;
    default:
        break;
    }
    if (msg >= 0x0100 && msg <= 0x0109) return Kind::Raw;  // WM_KEYFIRST..WM_KEYLAST
    if (msg >= 0x0200 && msg <= 0x020E) return Kind::Raw;  // WM_MOUSEFIRST..WM_MOUSELAST
    return Kind::Unsupported;
}

bool marshal(Marshaler& m, bool ansi, u32 msg, u64 wparam, u64 lparam,
             Marshaled& out) {
    out = {};
    switch (classify(msg)) {
    case Kind::StringIn: {
        const u32 present = lparam != 0 ? 1u : 0u;
        append(out.payload, &present, sizeof(present));
        if (present) {
            bool ok = false;
            std::vector<u16> text = read_text(m, ansi, lparam, ok);
            if (!ok) {
                set_last_error(error_codes::ERROR_NOACCESS);
                return false;
            }
            text.push_back(0);
            append(out.payload, text.data(), text.size() * sizeof(u16));
        }
        return true;
    }
    case Kind::TextOut: {
        const u64 chars = std::min<u64>(wparam, kMaxTextChars);
        out.out_capacity = static_cast<u32>(chars * sizeof(u16));
        return true;
    }
    case Kind::CopyData: {
        // COPYDATASTRUCT { ULONG_PTR dwData; DWORD cbData; PVOID lpData; }
        const bool is64 = m.cpu_is_64bit();
        u64 dw = 0, data = 0;
        u32 cb = 0;
        bool ok = lparam != 0;
        if (ok && is64) {
            auto a = m.read_u64(lparam); auto b = m.read_u32(lparam + 8);
            auto c = m.read_u64(lparam + 16);
            ok = a.is_ok() && b.is_ok() && c.is_ok();
            if (ok) { dw = a.value(); cb = b.value(); data = c.value(); }
        } else if (ok) {
            auto a = m.read_u32(lparam); auto b = m.read_u32(lparam + 4);
            auto c = m.read_u32(lparam + 8);
            ok = a.is_ok() && b.is_ok() && c.is_ok();
            if (ok) { dw = a.value(); cb = b.value(); data = c.value(); }
        }
        if (!ok) {
            set_last_error(error_codes::ERROR_NOACCESS);
            return false;
        }
        if (cb > 1000 * 1024) {
            std::fprintf(stderr,
                "[vitreo] UNIMPLEMENTED user32!SendMessage: cross-process "
                "WM_COPYDATA of %u bytes (transport limit 1000 KiB)\n", cb);
            set_last_error(error_codes::ERROR_NOT_ENOUGH_MEMORY);
            return false;
        }
        const u32 pad = 0;
        append(out.payload, &dw, sizeof(dw));
        append(out.payload, &cb, sizeof(cb));
        append(out.payload, &pad, sizeof(pad));
        if (cb != 0) {
            std::vector<Byte> bytes(cb);
            if (data == 0 || m.memory().read_bytes(data, bytes.data(), cb).is_err()) {
                set_last_error(error_codes::ERROR_NOACCESS);
                return false;
            }
            append(out.payload, bytes.data(), bytes.size());
        }
        return true;
    }
    case Kind::Raw:
        return true;
    case Kind::Unsupported:
        break;
    }
    return false;
}

void unmarshal_out(Marshaler& m, bool ansi, u32 msg, u64 wparam, u64 lparam,
                   std::span<const Byte> out, u64& lresult) {
    if (classify(msg) != Kind::TextOut || lparam == 0 || wparam == 0) return;
    std::vector<u16> text(out.size() / sizeof(u16));
    if (!text.empty()) std::memcpy(text.data(), out.data(), text.size() * 2);
    while (!text.empty() && text.back() == 0) text.pop_back();
    if (!ansi) {
        const usize n = std::min<usize>(text.size(), wparam - 1);
        text.resize(n);
        text.push_back(0);
        (void)m.memory().write_bytes(lparam, reinterpret_cast<const Byte*>(text.data()),
                                     text.size() * sizeof(u16));
        lresult = n;
        return;
    }
    std::vector<u8> bytes(text.size() * 4 + 1);
    const auto r = codepage::wide_to_multi_byte(
        0 /*CP_ACP*/, 0, text.data(), text.size(), bytes.data(), bytes.size(),
        nullptr, false);
    const usize n = std::min<usize>(r.count, wparam - 1);
    bytes.resize(n);
    bytes.push_back(0);
    (void)m.memory().write_bytes(lparam, bytes.data(), bytes.size());
    lresult = n;
}

ipc::RemoteSendStatus deliver(
    MessageSendManager& msm, GuestMemory& memory, HeapManager& heap,
    bool is64, u32 hwnd, u32 msg, u64 wparam, u64 lparam, u32 flags,
    u32 timeout_ms, std::span<const Byte> payload, u32 out_capacity,
    u64& lresult, std::vector<Byte>& out) {
    using S = ipc::RemoteSendStatus;
    const Kind kind = classify(msg);
    if (kind == Kind::Unsupported) return S::Unsupported;
    if (msm.window_data(static_cast<windowing::HWND>(hwnd)) == nullptr) {
        return S::TargetGone;
    }
    const GuestAddr process_heap = heap.get_process_heap();
    GuestAddr block = 0;
    u64 l = lparam;
    auto alloc = [&](usize bytes) -> bool {
        block = static_cast<GuestAddr>(heap.heap_alloc(process_heap, 0x8 /*HEAP_ZERO_MEMORY*/,
                                                       bytes == 0 ? 1 : bytes));
        return block != 0;
    };
    switch (kind) {
    case Kind::StringIn: {
        u32 present = 0;
        if (payload.size() >= 4) std::memcpy(&present, payload.data(), 4);
        l = 0;
        if (present) {
            const auto text = payload.subspan(4);
            if (!alloc(text.size() + 2)) return S::TimedOut;
            (void)memory.write_bytes(block, text.data(), text.size());
            l = block;
        }
        break;
    }
    case Kind::TextOut:
        if (!alloc(static_cast<usize>(out_capacity) + 2)) return S::TimedOut;
        l = block;
        break;
    case Kind::CopyData: {
        if (payload.size() < 16) return S::Unsupported;
        u64 dw = 0; u32 cb = 0;
        std::memcpy(&dw, payload.data(), 8);
        std::memcpy(&cb, payload.data() + 8, 4);
        const auto data = payload.subspan(16);
        if (data.size() < cb) return S::Unsupported;
        const usize header = is64 ? 24 : 12;
        if (!alloc(header + cb)) return S::TimedOut;
        const GuestAddr bytes_at = cb != 0 ? block + header : 0;
        if (cb != 0) (void)memory.write_bytes(bytes_at, data.data(), cb);
        if (is64) {
            (void)memory.write_u64(block, dw);
            (void)memory.write_u32(block + 8, cb);
            (void)memory.write_u64(block + 16, bytes_at);
        } else {
            (void)memory.write_u32(block, static_cast<u32>(dw));
            (void)memory.write_u32(block + 4, cb);
            (void)memory.write_u32(block + 8, static_cast<u32>(bytes_at));
        }
        l = block;
        break;
    }
    case Kind::Raw:
    case Kind::Unsupported:
        break;
    }
    windowing::MessageQueue::SendWaitStatus status{};
    lresult = msm.send_message_timeout(static_cast<windowing::HWND>(hwnd), msg,
                                       wparam, l, flags, timeout_ms, status);
    using W = windowing::MessageQueue::SendWaitStatus;
    if (status != W::Completed) {
        // The timed-out message is still delivered later (it stays queued),
        // so its marshaled block must outlive this call: it is not freed.
        return status == W::TargetGone ? S::TargetGone : S::TimedOut;
    }
    if (kind == Kind::TextOut && block != 0 && out_capacity >= 2) {
        const usize chars = std::min<usize>(static_cast<usize>(lresult),
                                            out_capacity / 2 - 1);
        out.resize((chars + 1) * 2);
        (void)memory.read_bytes(block, out.data(), chars * 2);
        out[chars * 2] = 0;
        out[chars * 2 + 1] = 0;
    }
    if (block != 0) (void)heap.heap_free(process_heap, 0, block);
    return S::Completed;
}

} // namespace vitreo::winapi::xproc_send
