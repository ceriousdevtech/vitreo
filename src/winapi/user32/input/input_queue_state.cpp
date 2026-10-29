// SPDX-License-Identifier: LGPL-2.1-only

/// @file input_queue_state.cpp
/// @brief Per-thread input-queue state: AttachThreadInput and the attachment
///        graph, Get/SetMessageExtraInfo, GetQueueStatus and GetLastInputInfo.
///        Split out of the former mixed USER32 stubs file
///        as verbatim code motion.

#include "winapi/user32/user32_ext.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/kernel32/thread/thread.hpp"
#include "windowing/model/hwnd_manager.hpp"
#include "winapi/user32/messages/message_loop.hpp"
#include "winapi/user32/painting/painting.hpp"
#include "ipc/ipc_gate.hpp"

#include <algorithm>
#include <mutex>
#include <vector>

namespace vitreo::winapi {

bool User32ExtManager::shares_input_queue(u32 a, u32 b) const {
    if (a == b) return true;
    std::lock_guard lock(m_attach_mutex);
    if (m_attached_inputs.empty()) return false;
    // Attachments are transitive: walk the attachment graph from a.
    std::vector<u32> frontier{a};
    std::vector<u32> seen{a};
    while (!frontier.empty()) {
        const u32 cur = frontier.back();
        frontier.pop_back();
        for (const auto& [from, to] : m_attached_inputs) {
            u32 next = 0;
            if (from == cur) next = to;
            else if (to == cur) next = from;
            else continue;
            if (next == b) return true;
            if (std::find(seen.begin(), seen.end(), next) == seen.end()) {
                seen.push_back(next);
                frontier.push_back(next);
            }
        }
    }
    return false;
}

bool User32ExtManager::window_in_caller_queue(windowing::HWND hwnd) const {
    if (hwnd == windowing::NULL_HWND) return true;
    const auto owner = m_hwnd_mgr.owner_thread_id(hwnd);
    // A window with no recorded owner (host-created) is not refused.
    if (!owner || *owner == 0) return true;
    return shares_input_queue(*owner, ThreadManager::get_current_thread_id_static());
}

void User32ExtManager::impl_AttachThreadInput(Marshaler& m) {
    // BOOL AttachThreadInput(DWORD idAttach, DWORD idAttachTo, BOOL fAttach)
    // s2msg_input_probe: a thread cannot attach to itself, and an unknown
    // thread, thread 0, or detaching a pair that is not attached fail with
    // ERROR_INVALID_PARAMETER (attach_self_self, attach_bogus, attach_zero,
    // detach_not_attached, detach_again).
    const u32 idAttach = static_cast<u32>(m.arg(0));
    const u32 idAttachTo = static_cast<u32>(m.arg(1));
    const bool fAttach = static_cast<u32>(m.arg(2)) != 0;
    const auto alive = [this](u32 tid) {
        return tid != 0 && (!m_thread_alive || m_thread_alive(tid));
    };
    bool ok = idAttach != idAttachTo && alive(idAttach) && alive(idAttachTo);
    if (ok) {
        std::lock_guard lock(m_attach_mutex);
        const auto it = std::find_if(
            m_attached_inputs.begin(), m_attached_inputs.end(),
            [&](const auto& pair) {
                return (pair.first == idAttach && pair.second == idAttachTo) ||
                       (pair.first == idAttachTo && pair.second == idAttach);
            });
        if (fAttach) {
            if (it == m_attached_inputs.end()) {
                m_attached_inputs.emplace_back(idAttach, idAttachTo);
            }
        } else if (it != m_attached_inputs.end()) {
            m_attached_inputs.erase(it);
        } else {
            ok = false;
        }
    }
    if (!ok) set_last_error(error_codes::ERROR_INVALID_PARAMETER);
    VITREO_TRACE("[u32ext] AttachThreadInput from=0x%x to=0x%x attach=%u -> %s\n",
                 idAttach, idAttachTo, fAttach ? 1u : 0u, ok ? "TRUE" : "FALSE");
    m.set_return(ok ? 1u : 0u);
}

void User32ExtManager::impl_GetMessageExtraInfo(Marshaler& m) {
    // Learn: the extra message information of the calling thread -- the
    // dwExtraInfo of the input event behind the message it last retrieved
    // (0 for posted messages), or what SetMessageExtraInfo stored since.
    const u64 value = MessageLoopManager::message_extra_info();
    m.set_return(m.cpu_is_64bit() ? value : static_cast<u32>(value));
}

void User32ExtManager::impl_SetMessageExtraInfo(Marshaler& m) {
    // LPARAM SetMessageExtraInfo(LPARAM): returns the previous value, last
    // error untouched (s2msg_extrainfo_probe set_55).
    const u64 value = m.cpu_is_64bit() ? m.arg(0) : static_cast<u32>(m.arg(0));
    const u64 previous = MessageLoopManager::exchange_message_extra_info(value);
    m.set_return(m.cpu_is_64bit() ? previous : static_cast<u32>(previous));
}

void User32ExtManager::impl_GetQueueStatus(Marshaler& m) {
    // DWORD GetQueueStatus(UINT flags) — Learn: high word = types currently in
    // the calling thread's queue; low word = types added since last
    // GetQueueStatus/GetMessage/PeekMessage that are still present. Posted
    // thread messages count in that occupancy. Returning 0 hides work that
    // is still queued.
    const u32 flags = static_cast<u32>(m.arg(0));
    // s2msg_queue_probe status_invalid_flags / status_bit_*: bits outside
    // the accepted set fail with 0 / ERROR_INVALID_FLAGS.
    if ((flags & ~windowing::QS::VALID_STATUS_FLAGS) != 0) {
        set_last_error(error_codes::ERROR_INVALID_FLAGS);
        m.set_return(0u);
        return;
    }
    const u32 tid = ThreadManager::get_current_thread_id_static();
    // WM_PAINT and WM_TIMER are synthesized, not queued: report them from
    // their sources (status_paint 0x00200020, status_timer 0x00100010,
    // status_after_killtimer 0). Invalidation raises QS_PAINT's change bit
    // (MessageQueue::notify_thread); a timer raises QS_TIMER's once per
    // expiry.
    u32 synthesized = 0;
    if (m_paint_mgr != nullptr &&
        m_paint_mgr->find_window_needing_paint(tid, 0) != 0) {
        synthesized |= windowing::QS::PAINT;
    }
    bool timer_newly_due = false;
    if (timer_due_for_thread(tid, timer_newly_due)) {
        synthesized |= windowing::QS::TIMER;
        if (timer_newly_due) m_queue.mark_changed(tid, windowing::QS::TIMER);
    }
    const u32 status = m_queue.get_queue_status(flags, tid, m_hwnd_mgr, synthesized);
    VITREO_TRACE("[u32ext] GetQueueStatus flags=0x%x tid=%u -> 0x%x\n",
                 flags, tid, status);
    if (status != 0) {
        ipc::ipc_log("GetQueueStatus flags=0x%x tid=%u -> 0x%x",
                     flags, tid, status);
    }
    m.set_return(status);
}

void User32ExtManager::impl_GetLastInputInfo(Marshaler& m) {
    constexpr u32 kLastInputInfoSize = 8;
    const GuestAddr info = m.arg_ptr(0);
    if (info == 0) {
        VITREO_TRACE("[user32_ext] GetLastInputInfo(NULL) -> FALSE\n");
        m.set_return(0);
        return;
    }

    const auto cb_size = m.read_u32(info);
    if (cb_size.is_err() || cb_size.value() != kLastInputInfoSize) {
        // lastinputinfo_bad_size: FALSE / ERROR_INVALID_PARAMETER.
        VITREO_TRACE(
            "[user32_ext] GetLastInputInfo(0x%llx, cbSize=%u) -> FALSE\n",
            static_cast<unsigned long long>(info),
            cb_size.value_or(0));
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return;
    }

    const u32 last_input_tick = m_host.last_input_tick_count();
    if (m.write_u32(info + 4, last_input_tick).is_err()) {
        VITREO_TRACE(
            "[user32_ext] GetLastInputInfo(0x%llx) write failed -> FALSE\n",
            static_cast<unsigned long long>(info));
        m.set_return(0);
        return;
    }

    VITREO_TRACE(
        "[user32_ext] GetLastInputInfo(0x%llx) -> TRUE dwTime=%u\n",
        static_cast<unsigned long long>(info), last_input_tick);
    m.set_return(1);
}

} // namespace vitreo::winapi
