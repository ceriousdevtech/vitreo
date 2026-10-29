// SPDX-License-Identifier: LGPL-2.1-only

/// @file window_timer.cpp
/// @brief Window positioning helpers that remain, timers, and window props.
///
/// Closed cut from user32_ext.cpp — verified with plan_extraction.py.

#include "winapi/user32/user32_ext.hpp"
#include "winapi/user32/user32_internal.hpp"
#include "winapi/common/display_device_identity.hpp"
#include "winapi/user32/painting/painting.hpp"
#include "winapi/user32/messages/message_loop.hpp"
#include "winapi/user32/messages/message_time.hpp"
#include "winapi/user32/messages/message_send.hpp"
#include "winapi/kernel32/system/time.hpp"
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

// ---------- Window positioning / timer ----------





void User32ExtManager::impl_SetTimer(Marshaler& m) {
    // UINT_PTR SetTimer(HWND hWnd, UINT_PTR nIDEvent, UINT uElapse,
    //                   TIMERPROC lpTimerFunc)
    // Every rule below is Learn (nf-winuser-settimer) plus
    // tools/oracle_probe/s2msg_queue_probe.expected where Learn is silent.
    const auto hwnd = static_cast<windowing::HWND>(m.arg(0));
    // UINT_PTR: the whole register on x64 (settimer_wide_id).
    u64 id = m.cpu_is_64bit() ? m.arg(1) : static_cast<u32>(m.arg(1));
    u32 elapse = static_cast<u32>(m.arg(2));
    const GuestAddr lpTimerFunc = m.arg_ptr(3);

    // Store CPU bitness for callback invocation
    m_is_64bit = m.cpu_is_64bit();

    if (hwnd != windowing::NULL_HWND && !m_hwnd_mgr.is_valid(hwnd)) {
        set_last_error(error_codes::ERROR_INVALID_WINDOW_HANDLE);
        m.set_return(0);
        return;
    }
    const u32 caller = ThreadManager::get_current_thread_id_static();
    // WM_TIMER goes to the window's queue even when another thread armed it
    // (settimer_other_thread_window_armed / owner_thread_sees_timer).
    u32 owner = caller;
    if (hwnd != windowing::NULL_HWND) {
        owner = m_hwnd_mgr.owner_thread_id(hwnd).value_or(caller);
    }

    // USER_TIMER_MINIMUM / MAXIMUM (Learn): out-of-range elapses are raised
    // or lowered, not rejected.
    const u32 requested = elapse;
    elapse = std::clamp(elapse, USER_TIMER_MINIMUM, USER_TIMER_MAXIMUM);

    u64 result = 0;
    {
        std::lock_guard timer_lock(m_timer_mutex);
        if (hwnd == windowing::NULL_HWND) {
            // Learn: with a NULL hWnd an nIDEvent naming one of this thread's
            // timers replaces it; any other value is ignored and a new ID is
            // generated (settimer_null_replace / _unknown_id).
            const auto existing = m_timer_info.find({hwnd, id});
            if (id == 0 || existing == m_timer_info.end() ||
                existing->second.owner_thread_id != caller) {
                do {
                    id = m_next_timer_id++;
                } while (id == 0 || m_timer_info.contains({hwnd, id}));
            }
            result = id;
        } else {
            // A window timer is identified by the caller's nIDEvent, 0
            // included; the return is then "a nonzero integer": 1
            // (settimer_hwnd_id0).
            result = id != 0 ? id : 1u;
        }
        m_timer_info[{hwnd, id}] = TimerInfo{
            elapse, std::chrono::steady_clock::now(), lpTimerFunc, owner, false};
    }
    // A thread blocked in GetMessage sleeps until its earliest timer; a timer
    // armed for it by another thread must shorten that sleep.
    if (owner != caller) m_queue.notify_thread(owner);
    timer_diag("[timer-diag] SetTimer hwnd=0x%x id=%llu requested=%u effective=%u\n",
               hwnd, static_cast<unsigned long long>(id), requested, elapse);
    VITREO_TRACE("[user32_ext] SetTimer(hwnd=0x%x id=%llu elapse=%u callback=0x%llx)\n",
                 hwnd, static_cast<unsigned long long>(id), elapse,
                 static_cast<unsigned long long>(lpTimerFunc));
    m.set_return(result);
}

void User32ExtManager::impl_KillTimer(Marshaler& m) {
    // BOOL KillTimer(HWND hWnd, UINT_PTR uIDEvent)
    const auto hwnd = static_cast<windowing::HWND>(m.arg(0));
    const u64 id = m.cpu_is_64bit() ? m.arg(1) : static_cast<u32>(m.arg(1));
    if (hwnd != windowing::NULL_HWND && !m_hwnd_mgr.is_valid(hwnd)) {
        set_last_error(error_codes::ERROR_INVALID_WINDOW_HANDLE);
        m.set_return(0);
        return;
    }
    const u32 caller = ThreadManager::get_current_thread_id_static();
    bool removed = false;
    {
        std::lock_guard timer_lock(m_timer_mutex);
        const auto it = m_timer_info.find({hwnd, id});
        // Window timers may be killed from any thread
        // (killtimer_other_thread_window); a NULL-hWnd timer belongs to the
        // thread that armed it. An unknown timer fails with the last error
        // untouched (killtimer_7_again / killtimer_null_bogus).
        if (it != m_timer_info.end() &&
            (hwnd != windowing::NULL_HWND || it->second.owner_thread_id == caller)) {
            m_timer_info.erase(it);
            removed = true;
        }
    }
    VITREO_TRACE("[user32_ext] KillTimer(hwnd=0x%x id=%llu) → %s\n",
                 hwnd, static_cast<unsigned long long>(id),
                 removed ? "TRUE" : "FALSE");
    m.set_return(removed ? 1u : 0u);
}

namespace {
// Timers a Get/PeekMessage hWnd filter can return: NULL takes any, (HWND)-1
// takes none (thread_timer_minus1_filter), a window takes its own and its
// child windows' timers -- never a thread timer (thread_timer_hwnd_filter).
bool timer_passes_hwnd_filter(const windowing::HwndManager& hwnd_mgr,
                              windowing::HWND timer_hwnd,
                              windowing::HWND filter_hwnd) noexcept {
    if (filter_hwnd == windowing::NULL_HWND) return true;
    if (filter_hwnd == static_cast<windowing::HWND>(-1)) return false;
    if (timer_hwnd == windowing::NULL_HWND) return false;
    return windowing::MessageQueue::hwnd_filter_selects(hwnd_mgr, timer_hwnd,
                                                        filter_hwnd);
}
} // namespace

u32 User32ExtManager::next_timer_due_ms(u32 owner_thread_id,
                                        windowing::HWND filter_hwnd) const {
    const auto now = std::chrono::steady_clock::now();
    std::lock_guard timer_lock(m_timer_mutex);
    u32 best = 0xFFFFFFFFu;
    for (const auto& [key, info] : m_timer_info) {
        if (owner_thread_id != 0 && info.owner_thread_id != owner_thread_id) {
            continue;
        }
        if (!timer_passes_hwnd_filter(m_hwnd_mgr, key.first, filter_hwnd)) continue;
        // A destroyed window's timer is dead (find_expired_timer prunes it);
        // counting it as due would spin a waiter.
        if (key.first != windowing::NULL_HWND && !m_hwnd_mgr.is_valid(key.first)) {
            continue;
        }
        const auto elapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                now - info.last_fire).count();
        const u64 remaining = static_cast<u64>(elapsed) >= info.elapse_ms
            ? 0u : info.elapse_ms - static_cast<u64>(elapsed);
        if (remaining < best) best = static_cast<u32>(remaining);
        if (best == 0) return 0;
    }
    return best;
}

bool User32ExtManager::timer_due_for_thread(u32 owner_thread_id, bool& newly) {
    newly = false;
    const auto now = std::chrono::steady_clock::now();
    bool due = false;
    std::lock_guard timer_lock(m_timer_mutex);
    for (auto& [key, info] : m_timer_info) {
        if (info.owner_thread_id != owner_thread_id) continue;
        if (key.first != windowing::NULL_HWND && !m_hwnd_mgr.is_valid(key.first)) {
            continue;
        }
        if (now - info.last_fire < std::chrono::milliseconds(info.elapse_ms)) {
            continue;
        }
        due = true;
        if (!info.change_reported) {
            info.change_reported = true;
            newly = true;
        }
    }
    return due;
}

bool User32ExtManager::is_registered_timer_proc(GuestAddr proc) const {
    if (proc == 0) return false;
    std::lock_guard timer_lock(m_timer_mutex);
    for (const auto& [key, info] : m_timer_info) {
        (void)key;
        if (info.callback == proc) return true;
    }
    return false;
}

bool User32ExtManager::find_expired_timer(windowing::HWND& out_hwnd, u64& out_id,
                                          GuestAddr& out_callback,
                                          u32 owner_thread_id, bool consume,
                                          windowing::HWND filter_hwnd) {
    auto now = std::chrono::steady_clock::now();
    std::lock_guard timer_lock(m_timer_mutex);

    // Lazily prune timers for destroyed windows. Win32 contract: when a
    // window is destroyed, all its associated timers are killed. Without
    // this, the bootstrap window's marquee timer (hwnd=0x10000 id=1)
    // keeps firing forever after DestroyWindow, causing the main thread
    // to be stuck pumping stale WM_TIMERs instead of advancing.
    auto chosen = m_timer_info.end();
    auto chosen_due = std::chrono::steady_clock::time_point::max();
    for (auto it = m_timer_info.begin(); it != m_timer_info.end(); ) {
        const windowing::HWND hwnd = it->first.first;
        // hwnd == 0 is a thread-bound timer (no window); always valid.
        if (hwnd != windowing::NULL_HWND && !m_hwnd_mgr.is_valid(hwnd)) {
            VITREO_TRACE("[user32_ext] pruning timer id=%llu for destroyed hwnd=0x%x\n",
                         static_cast<unsigned long long>(it->first.second), hwnd);
            it = m_timer_info.erase(it);
            continue;
        }
        const TimerInfo& info = it->second;
        if ((owner_thread_id == 0 || info.owner_thread_id == owner_thread_id) &&
            timer_passes_hwnd_filter(m_hwnd_mgr, hwnd, filter_hwnd)) {
            const auto due = info.last_fire + std::chrono::milliseconds(info.elapse_ms);
            // The most overdue timer first, so a short timer cannot starve a
            // long one that expired earlier.
            if (due <= now && due < chosen_due) {
                chosen = it;
                chosen_due = due;
            }
        }
        ++it;
    }
    if (chosen == m_timer_info.end()) return false;
    if (consume) {
        chosen->second.last_fire = now;
        chosen->second.change_reported = false;
    }
    out_hwnd = chosen->first.first;
    out_id = chosen->first.second;
    out_callback = chosen->second.callback;
    return true;
}

bool User32ExtManager::wait_message() {
    if (!m_msgloop_mgr) return false;
    // Learn WaitMessage: it "does not return until a new message is placed in
    // the thread's message queue", and "does not return if there is unread
    // input in the message queue after the thread has called a function to
    // check the queue" -- MsgWaitForMultipleObjects(0, NULL, FALSE, INFINITE,
    // QS_ALLINPUT) without MWMO_INPUTAVAILABLE (s2msg_queue_probe
    // waitmessage: an already-peeked post does not end the wait).
    constexpr u32 kInfinite = 0xFFFFFFFFu;
    const u32 result = m_msgloop_mgr->msg_wait_for_multiple_objects(
        nullptr, 0, false, kInfinite, windowing::QS::ALLINPUT);
    return result != 0xFFFFFFFFu; // WAIT_FAILED only at process teardown
}

void User32ExtManager::invoke_timer_callback(windowing::HWND hwnd, u64 timer_id, GuestAddr callback) {
    if (callback == 0 || !m_guest_call) {
        VITREO_TRACE("[user32_ext] invoke_timer_callback: no callback or guest_call\n");
        return;
    }

    // TIMERPROC signature: void CALLBACK TimerProc(HWND, UINT, UINT_PTR, DWORD)
    // The fourth parameter is GetTickCount, not a host steady_clock epoch.
    const u32 tick_count = tick_count32();

    u64 args[4] = {
        static_cast<u64>(hwnd),
        static_cast<u64>(windowing::WM::TIMER),
        timer_id,
        static_cast<u64>(tick_count)
    };
    timer_diag("[timer-diag] TIMERPROC hwnd=0x%x id=%llu dwTime=%u gettick=%u "
               "delta=%u\n",
               hwnd, static_cast<unsigned long long>(timer_id), tick_count,
               tick_count32(), tick_delta32(tick_count32(), tick_count));

    VITREO_TRACE("[user32_ext] invoke_timer_callback(hwnd=0x%x, id=%llu, callback=0x%llx)\n",
                 hwnd, timer_id, static_cast<unsigned long long>(callback));

    // TIMERPROC returns void, but we call through the same mechanism
    (void)m_guest_call(callback, args, 4, m_is_64bit);
}

// ---------------------------------------------------------------------------
// Window properties: SetProp / GetProp / RemoveProp (A/W).
//
// Contract: Learn SetPropW / GetPropW / RemovePropW. A property name is an
// atom: string names compare case-insensitively and MAKEINTATOM names are
// their own key. Measured (s1win_api_probe "-- props"): an invalid window is
// ERROR_INVALID_WINDOW_HANDLE for all three, a NULL name is
// ERROR_INVALID_PARAMETER for SetProp, and looking up a string name no
// window ever used fails with ERROR_FILE_NOT_FOUND (the atom lookup fails).
// ---------------------------------------------------------------------------

void User32ExtManager::prop_call(Marshaler& m, PropOp op, bool ansi) {
    const auto hwnd = static_cast<windowing::HWND>(m.arg(0));
    const GuestAddr name = m.arg_ptr(1);
    if (!m_hwnd_mgr.is_valid(hwnd)) {
        set_last_error(error_codes::ERROR_INVALID_WINDOW_HANDLE);
        m.set_return(0u);
        return;
    }
    const std::string key = make_prop_key(m, name, ansi);
    if (key.empty()) {
        if (op == PropOp::Set) set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0u);
        return;
    }
    std::lock_guard property_lock(m_props_mutex);
    if (op == PropOp::Set) {
        m_props[hwnd][key] = m.arg(2);
        if (key[0] == 'S') m_prop_names.insert(key);
        m.set_return(1u);
        return;
    }
    if (key[0] == 'S' && m_prop_names.find(key) == m_prop_names.end()) {
        // No atom exists for a name nobody has used.
        set_last_error(error_codes::ERROR_FILE_NOT_FOUND);
        m.set_return(0u);
        return;
    }
    if (key[0] == 'A' && name >= 0xC000) {
        // A string atom from GlobalAddAtom names the property of that string;
        // USER's property list and kernel32's global atom table are separate
        // in Vitreo, so such a lookup cannot be resolved.
        static std::once_flag once;
        std::call_once(once, [] {
            std::fprintf(stderr, "[vitreo] UNIMPLEMENTED user32!GetProp/RemoveProp: "
                                 "global string atom as the property name\n");
        });
    }
    auto hit = m_props.find(hwnd);
    if (hit == m_props.end()) {
        m.set_return(0u);
        return;
    }
    auto kit = hit->second.find(key);
    if (kit == hit->second.end()) {
        m.set_return(0u);
        return;
    }
    const u64 data = kit->second;
    if (op == PropOp::Remove) {
        hit->second.erase(kit);
        if (hit->second.empty()) m_props.erase(hit);
    }
    m.set_return(data);
}

void User32ExtManager::impl_SetPropA(Marshaler& m) { prop_call(m, PropOp::Set, true); }
void User32ExtManager::impl_SetPropW(Marshaler& m) { prop_call(m, PropOp::Set, false); }
void User32ExtManager::impl_GetPropA(Marshaler& m) { prop_call(m, PropOp::Get, true); }
void User32ExtManager::impl_GetPropW(Marshaler& m) { prop_call(m, PropOp::Get, false); }
void User32ExtManager::impl_RemovePropA(Marshaler& m) { prop_call(m, PropOp::Remove, true); }
void User32ExtManager::impl_RemovePropW(Marshaler& m) { prop_call(m, PropOp::Remove, false); }

void User32ExtManager::drop_window_props(windowing::HWND hwnd) {
    std::lock_guard property_lock(m_props_mutex);
    m_props.erase(hwnd);
}

void User32ExtManager::window_destroyed(windowing::HWND hwnd) {
    {
        std::lock_guard lock(m_window_state_mutex);
        m_window_regions.erase(hwnd);
        m_touch_windows.erase(hwnd);
    }
    drop_window_props(hwnd);
    window_menus_destroyed(hwnd);
    if (m_msgsend_mgr != nullptr) m_msgsend_mgr->control_destroyed(hwnd);
    {
        std::lock_guard lock(m_shutdown_reason_mutex);
        m_shutdown_reasons.erase(hwnd);
    }
    {
        std::lock_guard<std::mutex> procs(m_wndproc_mutex);
        m_original_wndproc.erase(hwnd);
    }
    {
        std::lock_guard lock(m_layered_mutex);
        m_layered.erase(hwnd);
    }
}

} // namespace vitreo::winapi
