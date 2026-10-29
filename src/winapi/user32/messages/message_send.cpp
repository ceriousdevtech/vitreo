#include "winapi/user32/user32_internal.hpp"
#include "windowing/resources/resource_walker.hpp"
/// @file message_send.cpp
/// @brief PostQuitMessage/PostMessageW/SendMessageW implementation (WIN32-055)
///
/// MessageSendManager core, the post-side thunks and the export table. The
/// SendMessage thunks live in send_message_thunks.cpp, the cross-process and
/// timeout paths in send_message_peer.cpp / send_message_timeout.cpp, and the
/// native-control handlers in control_*.cpp.

#include "winapi/user32/messages/message_send_internal.hpp"
#include "winapi/user32/messages/ansi_char_message.hpp"
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

std::wstring message_send_utf8_to_wstring(const std::string& s) {
    std::wstring result;
    result.reserve(s.size());
    for (size_t i = 0; i < s.size(); ) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if (c < 0x80) {
            result.push_back(static_cast<wchar_t>(c));
            ++i;
        } else if ((c & 0xE0) == 0xC0 && i + 1 < s.size() &&
                   (static_cast<unsigned char>(s[i + 1]) & 0xC0) == 0x80) {
            wchar_t wc = static_cast<wchar_t>(((c & 0x1F) << 6) |
                         (static_cast<unsigned char>(s[i + 1]) & 0x3F));
            result.push_back(wc);
            i += 2;
        } else if ((c & 0xF0) == 0xE0 && i + 2 < s.size() &&
                   (static_cast<unsigned char>(s[i + 1]) & 0xC0) == 0x80 &&
                   (static_cast<unsigned char>(s[i + 2]) & 0xC0) == 0x80) {
            wchar_t wc = static_cast<wchar_t>(((c & 0x0F) << 12) |
                         ((static_cast<unsigned char>(s[i + 1]) & 0x3F) << 6) |
                         (static_cast<unsigned char>(s[i + 2]) & 0x3F));
            result.push_back(wc);
            i += 3;
        } else {
            // CP1252 fallback (e.g. ® = 0xAE) — not literal '?'
            if (c >= 0xA0) {
                result.push_back(static_cast<wchar_t>(c));
            } else if (c >= 0x80) {
                static constexpr wchar_t kCp1252_80_9F[32] = {
                    0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
                    0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x008D, 0x017D, 0x008F,
                    0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
                    0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178,
                };
                result.push_back(kCp1252_80_9F[c - 0x80]);
            } else {
                result.push_back(static_cast<wchar_t>(c));
            }
            ++i;
        }
    }
    return result;
}

// wstring_to_utf8 now lives in user32_ext.cpp and is declared in
// user32_internal.hpp. The copy that stood here was identical; two
// encoders that disagree on an edge case produce text that differs by
// path, which is why it is shared rather than re-derived.

// =============================================================================
// MessageSendManager
// =============================================================================

// Progress/button/font message codes: message_send.hpp

MessageSendManager::MessageSendManager(
    windowing::HwndManager& hwnd_mgr,
    windowing::MessageQueue& queue,
    windowing::CocoaBridge& bridge
)
    : m_hwnd_mgr{hwnd_mgr}
    , m_queue{queue}
    , m_bridge{bridge}
    , m_wndproc_callback{}
{}

void MessageSendManager::set_wndproc_callback(SendWndProcCallback callback) {
    m_wndproc_callback = std::move(callback);
}

u64 MessageSendManager::wndproc_for(windowing::HWND hwnd) const {
    const auto* win = m_hwnd_mgr.find(hwnd);
    if (!win) return 0;
    return win->wndproc;
}

bool MessageSendManager::set_control_text(windowing::HWND hwnd, const std::wstring& text) {
    return m_bridge.set_control_text(hwnd, text);
}

std::wstring MessageSendManager::get_control_text(windowing::HWND hwnd) const {
    return m_bridge.get_control_text(hwnd);
}

windowing::WindowData* MessageSendManager::window_data(windowing::HWND hwnd) {
    return m_hwnd_mgr.find(hwnd);
}


// -----------------------------------------------------------------------------
// post_quit_message
// -----------------------------------------------------------------------------

void MessageSendManager::post_quit_message(int exit_code) {
    m_queue.post_quit(exit_code, ThreadManager::get_current_thread_id_static());
}


// -----------------------------------------------------------------------------
// post_thread_message
// -----------------------------------------------------------------------------

bool MessageSendManager::thread_accepts_messages(u32 thread_id) const {
    if (thread_id == 0) return false;
    const auto owner = ipc::primary_thread_owner(thread_id);
    if (owner && *owner != static_cast<u32>(::getpid()) &&
        ipc::remote_process_endpoint_available(static_cast<pid_t>(*owner))) {
        return true;
    }
    return !m_thread_alive || m_thread_alive(thread_id);
}

bool MessageSendManager::post_thread_message(
    u32 thread_id, u32 msg, u64 wParam, u64 lParam
) {
    // A Windows thread ID is not a host process ID. Resolve the published
    // primary identity before contacting its owning USER32 endpoint.
    const auto owner = ipc::primary_thread_owner(thread_id);
    if (owner && *owner != static_cast<u32>(::getpid()) &&
        ipc::remote_process_endpoint_available(static_cast<pid_t>(*owner))) {
        const bool posted = ipc::remote_post_thread_message(
            static_cast<pid_t>(*owner), msg, wParam, lParam);
        ipc::ipc_log(
            "PostThreadMessage remote tid=%u msg=0x%x wParam=0x%llx "
            "lParam=0x%llx result=%d",
            thread_id, msg, static_cast<unsigned long long>(wParam),
            static_cast<unsigned long long>(lParam), posted ? 1 : 0);
        VITREO_TRACE(
            "[send] PostThreadMessageW remote tid=%u msg=0x%x -> %d\n",
            thread_id, msg, posted ? 1 : 0);
        return posted;
    }

    windowing::MSG m{windowing::NULL_HWND, msg, wParam, lParam, 0, {}};
    m.target_thread_id = thread_id;
    m_queue.post(m);
    VITREO_TRACE( "[send] PostThreadMessageW tid=%u msg=0x%x wParam=0x%llx lParam=0x%llx\n",
                 thread_id, msg, static_cast<unsigned long long>(wParam),
                 static_cast<unsigned long long>(lParam));
    return true;
}

// =============================================================================
// Thunk Implementations
// =============================================================================

void impl_PostQuitMessage(Marshaler& m, MessageSendManager& msm) {
    u32 tid = winapi::ThreadManager::get_current_thread_id_static();
    int exitCode = static_cast<int>(m.arg(0));
    if (message_send_diag_logs_enabled()) {
        std::fprintf(stderr, "[btn-diag] PostQuitMessage(exitCode=%d) tid=%u\n",
                    exitCode, tid);
    }
    // Win32 posts WM_QUIT to the calling thread's queue, including GUI worker
    // threads. MessageQueue keeps quit state per guest thread.
    msm.post_quit_message(exitCode);
}

static void post_message_common(Marshaler& m, MessageSendManager& msm,
                                bool ansi) {
    auto hwnd   = static_cast<windowing::HWND>(m.arg(0));
    auto msg    = static_cast<u32>(m.arg(1));
    auto wParam = m.arg(2);
    auto lParam = m.arg(3);
    // PostMessageA queues a character message in its Unicode form
    // (s2msg_ansi_msg_probe posta_getw_*).
    if (ansi && ansi_char_message::is_char_message(msg)) {
        wParam = ansi_char_message::to_unicode(wParam);
    }
    // s2msg_queue_probe: an invalid window fails with
    // ERROR_INVALID_WINDOW_HANDLE before the message is examined
    // (post_invalid_hwnd_settext); then synchronous-only messages fail
    // (post_settext_ptr / post_broadcast_settext: ERROR_MESSAGE_SYNC_ONLY).
    // A window another capsule process owns is valid too (Learn: an HWND is
    // system-wide); its message goes to that process's queue.
    u32 peer_owner = 0;
    if (hwnd != windowing::NULL_HWND && hwnd != windowing::HWND_BROADCAST &&
        msm.window_data(hwnd) == nullptr) {
        peer_owner = peer_owner_of(hwnd);
        if (peer_owner == 0) {
            set_last_error(error_codes::ERROR_INVALID_WINDOW_HANDLE);
            m.set_return(0u);
            return;
        }
    }
    if (const u32 refusal = MessageSendManager::async_message_refusal(msg)) {
        set_last_error(refusal);
        m.set_return(0u);
        return;
    }
    if (peer_owner != 0) {
        // Posted values cross unmarshaled, as on Windows: every message whose
        // parameters are pointers is synchronous-only and was refused above.
        if (!ipc::remote_post_message(static_cast<pid_t>(peer_owner), hwnd, msg,
                                      wParam, lParam)) {
            set_last_error(error_codes::ERROR_INVALID_WINDOW_HANDLE);
            m.set_return(0u);
            return;
        }
        m.set_return(1u);
        return;
    }
    bool result = msm.post_message(hwnd, msg, wParam, lParam);
    if (result && hwnd == windowing::HWND_BROADCAST) {
        // Learn PostMessage: HWND_BROADCAST posts to every top-level window
        // in the system -- the peers' too.
        for (const auto& entry : ipc::hwnd_registry_enumerate()) {
            if (entry.parent_hwnd != 0 ||
                entry.owner_host_pid == static_cast<u32>(::getpid())) {
                continue;
            }
            (void)ipc::remote_post_message(static_cast<pid_t>(entry.owner_host_pid),
                                           entry.hwnd, msg, wParam, lParam);
        }
    }
    m.set_return(result ? 1u : 0u);
}

void impl_PostMessageW(Marshaler& m, MessageSendManager& msm) {
    post_message_common(m, msm, /*ansi=*/false);
}

static void post_thread_message_common(Marshaler& m, MessageSendManager& msm,
                                       bool ansi) {
    auto thread_id = static_cast<u32>(m.arg(0));
    auto msg       = static_cast<u32>(m.arg(1));
    auto wParam    = m.arg(2);
    auto lParam    = m.arg(3);
    // s2msg_ansi_msg_probe postthreada_getw_char_80: the A form queues the
    // Unicode character.
    if (ansi && ansi_char_message::is_char_message(msg)) {
        wParam = ansi_char_message::to_unicode(wParam);
    }
    // Thread first (post_thread_bogus / post_thread_zero:
    // ERROR_INVALID_THREAD_ID), then the synchronous-only refusal
    // (post_thread_settext).
    if (!msm.thread_accepts_messages(thread_id)) {
        set_last_error(error_codes::ERROR_INVALID_THREAD_ID);
        m.set_return(0u);
        return;
    }
    if (const u32 refusal = MessageSendManager::async_message_refusal(msg)) {
        set_last_error(refusal);
        m.set_return(0u);
        return;
    }
    bool result = msm.post_thread_message(thread_id, msg, wParam, lParam);
    m.set_return(result ? 1u : 0u);
}

void impl_PostThreadMessageW(Marshaler& m, MessageSendManager& msm) {
    post_thread_message_common(m, msm, /*ansi=*/false);
}

void impl_SendNotifyMessageW(Marshaler& m, MessageSendManager& msm) {
    // BOOL SendNotifyMessage(HWND, UINT, WPARAM, LPARAM). The messages it
    // accepts carry no pointers, so A and W deliver the same payload.
    const auto hwnd = static_cast<windowing::HWND>(m.arg(0));
    const auto msg = static_cast<u32>(m.arg(1));
    m.set_return(msm.send_notify_message(hwnd, msg, m.arg(2), m.arg(3)) ? 1u : 0u);
}

// =============================================================================
// ANSI (A-suffix) Wrappers
// =============================================================================

void impl_PostMessageA(Marshaler& m, MessageSendManager& msm) {
    post_message_common(m, msm, /*ansi=*/true);
}

// =============================================================================
// Export Registration
// =============================================================================

DllExports make_message_send_exports(MessageSendManager& msm) {
    DllExports exports;
    exports.entries.push_back(ExportEntry{
        "PostQuitMessage", 1,
        [&msm](Marshaler& m) { impl_PostQuitMessage(m, msm); },
        4
    });
    exports.entries.push_back(ExportEntry{
        "PostMessageW", 2,
        [&msm](Marshaler& m) { impl_PostMessageW(m, msm); },
        16
    });
    exports.entries.push_back(ExportEntry{
        "PostMessageA", 0,
        [&msm](Marshaler& m) { impl_PostMessageA(m, msm); },
        16
    });
    // PostThreadMessageA queues character messages in their Unicode form,
    // like PostMessageA. The A export is 4 arguments, stdcall 16 bytes.
    // A missing-export stub returns 0xFFFFFFFF and pops no bytes, which
    // imbalances the caller.
    exports.entries.push_back(ExportEntry{
        "PostThreadMessageW", 0,
        [&msm](Marshaler& m) { post_thread_message_common(m, msm, false); }, 16
    });
    exports.entries.push_back(ExportEntry{
        "PostThreadMessageA", 0,
        [&msm](Marshaler& m) { post_thread_message_common(m, msm, true); }, 16
    });
    exports.entries.push_back(ExportEntry{
        "SendMessageW", 3,
        [&msm](Marshaler& m) { impl_SendMessageW(m, msm); },
        16
    });
    exports.entries.push_back(ExportEntry{
        "SendMessageA", 0,
        [&msm](Marshaler& m) { impl_SendMessageA(m, msm); },
        16
    });
    exports.entries.push_back(ExportEntry{
        "SendMessageTimeoutW", 4,
        [&msm](Marshaler& m) { impl_SendMessageTimeoutW(m, msm); },
        28
    });
    exports.entries.push_back(ExportEntry{
        "SendMessageTimeoutA", 0,
        [&msm](Marshaler& m) { impl_SendMessageTimeoutA(m, msm); },
        28
    });
    exports.entries.push_back(ExportEntry{
        "SendNotifyMessageW", 0,
        [&msm](Marshaler& m) { impl_SendNotifyMessageW(m, msm); },
        16
    });
    exports.entries.push_back(ExportEntry{
        "SendNotifyMessageA", 0,
        [&msm](Marshaler& m) { impl_SendNotifyMessageW(m, msm); },
        16
    });
    // BOOL InSendMessage(void) / DWORD InSendMessageEx(LPVOID lpReserved) /
    // BOOL ReplyMessage(LRESULT) (Learn; s2msg_queue_probe same_thread,
    // cross mode=0..2: ISMEX_SEND 1, ISMEX_NOTIFY 2, | ISMEX_REPLIED 8).
    // Only a message sent by ANOTHER thread counts; the thread's own
    // SendMessage calls its window procedure directly.
    exports.entries.push_back(ExportEntry{
        "InSendMessage", 0,
        [](Marshaler& m) {
            m.set_return(windowing::MessageQueue::current_send_flags() != 0 ? 1u : 0u);
        },
        0
    });
    exports.entries.push_back(ExportEntry{
        "InSendMessageEx", 0,
        [](Marshaler& m) {
            (void)m.arg_ptr(0); // lpReserved: must be NULL, not examined
            m.set_return(windowing::MessageQueue::current_send_flags());
        },
        4
    });
    exports.entries.push_back(ExportEntry{
        "ReplyMessage", 0,
        [](Marshaler& m) {
            // Not processing a cross-thread send: FALSE, last error untouched
            // (reply_not_in_send).
            m.set_return(windowing::MessageQueue::reply_message(m.arg(0)) ? 1u : 0u);
        },
        4
    });
    // BOOL GetInputState(void): nonzero when the calling thread's queue holds
    // NEW mouse-button or keyboard input (Learn). A PostMessage(WM_KEYDOWN) is
    // posted, not input (input_state_posted_keydown = 0).
    exports.entries.push_back(ExportEntry{
        "GetInputState", 0,
        [&msm](Marshaler& m) {
            m.set_return(msm.thread_has_new_input(
                ThreadManager::get_current_thread_id_static(),
                windowing::QS::KEY | windowing::QS::MOUSEBUTTON) ? 1u : 0u);
        },
        0
    });
    return exports;
}

} // namespace vitreo::winapi
