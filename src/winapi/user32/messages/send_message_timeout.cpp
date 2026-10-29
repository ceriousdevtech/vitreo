// SPDX-License-Identifier: LGPL-2.1-only

/// @file send_message_timeout.cpp
/// @brief SendMessageTimeout: MessageSendManager::send_message_timeout and the
///        A/W thunks.

#include "winapi/user32/user32_internal.hpp"
#include "windowing/resources/resource_walker.hpp"

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

// -----------------------------------------------------------------------------
// send_message_timeout
// -----------------------------------------------------------------------------

bool MessageSendManager::is_cross_thread_wndproc(windowing::HWND hwnd) const {
    const auto* win = m_hwnd_mgr.find(hwnd);
    const u32 self = ThreadManager::get_current_thread_id_static();
    // Same ownership rule as send_message (owner thread != caller). A missing
    // wndproc still needs the timed wait: otherwise SendMessageTimeout falls
    // through to SendMessage and blocks forever on a hung receiver.
    return win != nullptr && win->owner_thread_id != 0 &&
           win->owner_thread_id != self;
}

u64 MessageSendManager::send_message_timeout(
    windowing::HWND hwnd, u32 msg, u64 wParam, u64 lParam,
    u32 flags, u32 timeout, windowing::MessageQueue::SendWaitStatus& status
) {
    // Learn (nf-winuser-sendmessagetimeoutw) flag values.
    constexpr u32 SMTO_BLOCK = 0x0001;
    constexpr u32 SMTO_ABORTIFHUNG = 0x0002;
    constexpr u32 SMTO_NOTIMEOUTIFNOTHUNG = 0x0008;
    constexpr u32 SMTO_ERRORONEXIT = 0x0020;
    status = windowing::MessageQueue::SendWaitStatus::Completed;
    const auto* win = m_hwnd_mgr.find(hwnd);
    if (!is_cross_thread_wndproc(hwnd)) {
        return send_message(hwnd, msg, wParam, lParam);
    }
    windowing::MessageQueue::SendWaitOptions options;
    options.timeout_ms = timeout;
    options.block = (flags & SMTO_BLOCK) != 0;
    options.abort_if_hung = (flags & SMTO_ABORTIFHUNG) != 0;
    options.no_timeout_if_not_hung = (flags & SMTO_NOTIMEOUTIFNOTHUNG) != 0;
    if ((flags & SMTO_ERRORONEXIT) != 0) {
        options.target_gone = [this, hwnd] { return m_hwnd_mgr.find(hwnd) == nullptr; };
    }
    std::function<void(std::function<bool()>)> host_wait;
    if (pthread_main_np() != 0 && m_bridge.is_app_launched()) {
        host_wait = [this](std::function<bool()> ready) {
            m_bridge.wait_events(m_queue, 16, std::move(ready));
        };
    }
    const u32 sender = ThreadManager::get_current_thread_id_static();
    return m_queue.send_to_thread(sender, win->owner_thread_id,
        [this, hwnd, msg, wParam, lParam] {
            return send_message(hwnd, msg, wParam, lParam);
        }, std::move(host_wait), options, status);
}

// LRESULT-returning SendMessageTimeout core shared by the A and W exports.
// Learn (nf-winuser-sendmessagetimeoutw): the return value is nonzero on
// success and 0 on failure or time-out (GetLastError == ERROR_TIMEOUT for a
// time-out); the message's result goes to *lpdwResult (a DWORD_PTR).
static void send_message_timeout_common(Marshaler& m, MessageSendManager& msm,
                                        bool ansi) {
    const auto hwnd = static_cast<windowing::HWND>(m.arg(0));
    const auto msg = static_cast<u32>(m.arg(1));
    const u64 wParam = m.arg(2);
    const u64 lParam = m.arg(3);
    // UINT fuFlags / uTimeout sit in the x64 stack slots: low dword only.
    const u32 flags = m.arg32(4);
    const u32 timeout = m.arg32(5);
    const GuestAddr result_ptr = m.arg_ptr(6);
    // Measured (user32_windows_probe smto_invalid_hwnd / smto_busy_timeout):
    // a failed or timed-out call stores 0 in *lpdwResult.
    auto store_result = [&](u64 v) {
        if (result_ptr == 0) return;
        if (m.cpu_is_64bit()) (void)m.memory().write_u64(result_ptr, v);
        else (void)m.memory().write_u32(result_ptr, static_cast<u32>(v));
    };
    auto fail = [&](u32 error) {
        if (error != 0) set_last_error(error);
        store_result(0);
        m.set_return(0);
    };

    u64 lresult = 0;
    constexpr windowing::HWND HWND_BROADCAST = 0xFFFF;
    if (hwnd == HWND_BROADCAST) {
        // Learn: "the message is sent to all top-level windows in the
        // system" -- this process's and every peer's in the capsule -- and a
        // successful broadcast returns nonzero. Each window gets its own
        // uTimeout.
        for (const auto top : msm.top_level_windows()) {
            if (msm.is_cross_thread_wndproc(top)) {
                windowing::MessageQueue::SendWaitStatus st{};
                (void)msm.send_message_timeout(top, msg, wParam, lParam, flags,
                                               timeout, st);
            } else {
                (void)msm.send_message(top, msg, wParam, lParam);
            }
        }
        for (const auto& entry : ipc::hwnd_registry_enumerate()) {
            if (entry.parent_hwnd != 0 ||
                entry.owner_host_pid == static_cast<u32>(::getpid())) {
                continue;
            }
            u64 ignored = 0;
            (void)send_to_peer(m, ansi, entry.owner_host_pid, entry.hwnd, msg,
                               wParam, lParam, flags, timeout, ignored);
        }
        m.set_return(1);
        return;
    }
    if (hwnd == windowing::NULL_HWND) {
        fail(error_codes::ERROR_INVALID_WINDOW_HANDLE);
        return;
    }
    if (msm.window_data(hwnd) == nullptr) {
        const u32 owner = peer_owner_of(hwnd);
        if (owner == 0) {
            fail(error_codes::ERROR_INVALID_WINDOW_HANDLE);
            return;
        }
        if (!send_to_peer(m, ansi, owner, hwnd, msg, wParam, lParam, flags,
                          timeout, lresult)) {
            fail(0);  // send_to_peer set the error
            return;
        }
        store_result(lresult);
        m.set_return(1);
        return;
    }
    if (msm.is_cross_thread_wndproc(hwnd)) {
        windowing::MessageQueue::SendWaitStatus status{};
        lresult = msm.send_message_timeout(hwnd, msg, wParam, lParam, flags,
                                           timeout, status);
        using S = windowing::MessageQueue::SendWaitStatus;
        if (status != S::Completed) {
            fail(status == S::TargetGone
                     ? error_codes::ERROR_INVALID_WINDOW_HANDLE
                     : error_codes::ERROR_TIMEOUT);
            return;
        }
    } else {
        // Same-thread (or native control) target: exactly SendMessage,
        // including the native-control marshaling SendMessageW performs.
        if (ansi) impl_SendMessageA(m, msm); else impl_SendMessageW(m, msm);
        lresult = m.has_cpu() ? m.cpu()->gpr.rax : 0;
        if (m.has_cpu() && !m.cpu_is_64bit()) lresult &= 0xFFFFFFFFull;
    }
    store_result(lresult);
    m.set_return(1);
}

void impl_SendMessageTimeoutW(Marshaler& m, MessageSendManager& msm) {
    send_message_timeout_common(m, msm, /*ansi=*/false);
}

void impl_SendMessageTimeoutA(Marshaler& m, MessageSendManager& msm) {
    send_message_timeout_common(m, msm, /*ansi=*/true);
}

} // namespace vitreo::winapi
