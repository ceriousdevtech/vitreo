// SPDX-License-Identifier: LGPL-2.1-only

/// @file message_send.cpp
/// @brief PostQuitMessage/PostMessageW/SendMessageW implementation (WIN32-055)

#include "message_send.hpp"
#include "winapi/user32/messages/message_time.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/graphics/gdi32/gdi32.hpp"
#include "winapi/kernel32/thread/thread.hpp"
#include "core/memory/guest_memory.hpp"
#include <cstdlib>
#include <string>
#include <cstdio>
#include <vector>

/// @file post_message.cpp
/// @brief post_message
///
/// Closed cut from message_send.cpp — verified with plan_extraction.py.

#include <algorithm>
#include <iterator>

namespace vitreo::winapi {

namespace {
struct AsyncRefusal {
    u32 message;
    u32 error;
};
constexpr AsyncRefusal kAsyncRefusals[] = {
#include "post_message_policy.inc"
};
} // namespace

u32 MessageSendManager::async_message_refusal(u32 msg) noexcept {
    const auto* end = std::end(kAsyncRefusals);
    const auto* it = std::lower_bound(
        std::begin(kAsyncRefusals), end, msg,
        [](const AsyncRefusal& row, u32 value) { return row.message < value; });
    return (it != end && it->message == msg) ? it->error : 0u;
}

bool MessageSendManager::send_notify_message(windowing::HWND hwnd, u32 msg,
                                             u64 wParam, u64 lParam) {
    // s2msg_queue_probe: an invalid window fails first (send_notify_invalid,
    // 1400), then the synchronous-only refusal -- even when the window is the
    // caller's own (sendnotify_settext_same_thread, 1159).
    if (hwnd != windowing::HWND_BROADCAST &&
        (hwnd == windowing::NULL_HWND || !m_hwnd_mgr.is_valid(hwnd))) {
        set_last_error(error_codes::ERROR_INVALID_WINDOW_HANDLE);
        return false;
    }
    if (const u32 refusal = async_message_refusal(msg)) {
        set_last_error(refusal);
        return false;
    }
    const u32 sender = ThreadManager::get_current_thread_id_static();
    const auto deliver = [&](windowing::HWND target) {
        const auto owner = m_hwnd_mgr.owner_thread_id(target);
        if (!owner || *owner == sender) {
            (void)send_message(target, msg, wParam, lParam);
            return;
        }
        (void)m_queue.notify_thread_send(sender, *owner,
            [this, target, msg, wParam, lParam] {
                return send_message(target, msg, wParam, lParam);
            });
    };
    if (hwnd == windowing::HWND_BROADCAST) {
        for (const auto target : m_hwnd_mgr.enum_children(windowing::HWND_DESKTOP)) {
            deliver(target);
        }
        return true;
    }
    deliver(hwnd);
    return true;
}

// -----------------------------------------------------------------------------
// post_message
// -----------------------------------------------------------------------------

bool MessageSendManager::post_message(
    windowing::HWND hwnd, u32 msg, u64 wParam, u64 lParam
) {
    // Build the MSG. Always record target_thread_id so belongs_to_thread can
    // still attribute the message if the HWND is later destroyed (Windows keeps
    // posted messages on the creating thread's queue until retrieved).
    auto build_msg = [&](windowing::HWND target) -> windowing::MSG {
        windowing::MSG queued{target, msg, wParam, lParam, tick_count32(), {}};
        if (target == windowing::NULL_HWND) {
            queued.target_thread_id =
                ThreadManager::get_current_thread_id_static();
        } else if (const auto owner = m_hwnd_mgr.owner_thread_id(target)) {
            queued.target_thread_id = *owner;
        } else {
            queued.target_thread_id =
                ThreadManager::get_current_thread_id_static();
        }
        return queued;
    };

    const bool diag = std::getenv("VITREO_POSTMSG_DIAG") != nullptr;
    const u32 tid = ThreadManager::get_current_thread_id_static();

    // HWND_BROADCAST: post to all top-level windows
    if (hwnd == windowing::HWND_BROADCAST) {
        auto children = m_hwnd_mgr.enum_children(windowing::HWND_DESKTOP);
        for (auto child : children) {
            m_queue.post(build_msg(child));
        }
        if (diag) {
            std::fprintf(stderr,
                         "[postmsg-diag] tid=%u HWND_BROADCAST msg=0x%x "
                         "wParam=0x%llx lParam=0x%llx targets=%zu\n",
                         tid, msg,
                         static_cast<unsigned long long>(wParam),
                         static_cast<unsigned long long>(lParam),
                         children.size());
            std::fflush(stderr);
        }
        return true;
    }

    // NULL HWND: thread message
    if (hwnd == windowing::NULL_HWND) {
        m_queue.post(build_msg(windowing::NULL_HWND));
        if (diag) {
            std::fprintf(stderr,
                         "[postmsg-diag] tid=%u HWND_NULL msg=0x%x "
                         "wParam=0x%llx lParam=0x%llx\n",
                         tid, msg,
                         static_cast<unsigned long long>(wParam),
                         static_cast<unsigned long long>(lParam));
            std::fflush(stderr);
        }
        return true;
    }

    // Normal window: validate HWND exists
    if (!m_hwnd_mgr.is_valid(hwnd)) {
        set_last_error(error_codes::ERROR_INVALID_WINDOW_HANDLE);
        if (diag) {
            std::fprintf(stderr,
                         "[postmsg-diag] tid=%u INVALID hwnd=0x%x msg=0x%x\n",
                         tid, hwnd, msg);
            std::fflush(stderr);
        }
        return false;
    }

    const windowing::MSG queued = build_msg(hwnd);
    m_queue.post(queued);
    if (diag) {
        const auto owner = m_hwnd_mgr.owner_thread_id(hwnd);
        std::fprintf(stderr,
                     "[postmsg-diag] tid=%u hwnd=0x%x owner_tid=%u "
                     "target_tid=%u msg=0x%x wParam=0x%llx lParam=0x%llx\n",
                     tid, hwnd, owner.value_or(0), queued.target_thread_id, msg,
                     static_cast<unsigned long long>(wParam),
                     static_cast<unsigned long long>(lParam));
        std::fflush(stderr);
    }
    return true;
}

} // namespace vitreo::winapi
