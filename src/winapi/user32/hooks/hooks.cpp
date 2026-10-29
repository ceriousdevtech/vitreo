// SPDX-License-Identifier: LGPL-2.1-only

/// @file hooks.cpp
/// @brief SetWindowsHookEx and the WinEvent hooks beside it.
///
/// Windows hooks let a process intercept messages before their target sees
/// them, and a global hook injects the hook DLL into every process on the
/// desktop. Vitreo has no desktop to inject across, so these keep the
/// bookkeeping a caller can observe — a handle that unhooks, a chain that
/// forwards — without the cross-process half.
///
/// Gathered from user32_ext.cpp. Closed in both directions before moving.

#include "winapi/user32/user32_ext.hpp"
#include "winapi/user32/user32_internal.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/kernel32/process/process.hpp"
#include "winapi/kernel32/module/module.hpp"
#include "winapi/kernel32/thread/thread.hpp"
#include "winapi/user32/messages/message_send.hpp"
#include "winapi/user32/messages/message_time.hpp"
#include "windowing/model/hwnd_manager.hpp"
#include "windowing/model/diag_flags.hpp"
#include "core/memory/guest_memory.hpp"
#include "vitreo/result.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace vitreo::winapi {

namespace {
// winuser.h hook ids.
constexpr i32 WH_MSGFILTER = -1;
constexpr i32 WH_JOURNALRECORD = 0;
constexpr i32 WH_JOURNALPLAYBACK = 1;
constexpr i32 WH_KEYBOARD = 2;
constexpr i32 WH_GETMESSAGE = 3;
constexpr i32 WH_CALLWNDPROC = 4;
constexpr i32 WH_CBT = 5;
constexpr i32 WH_SYSMSGFILTER = 6;
constexpr i32 WH_MOUSE = 7;
constexpr i32 WH_HARDWARE = 8;
constexpr i32 WH_DEBUG = 9;
constexpr i32 WH_SHELL = 10;
constexpr i32 WH_FOREGROUNDIDLE = 11;
constexpr i32 WH_CALLWNDPROCRET = 12;
constexpr i32 WH_KEYBOARD_LL = 13;
constexpr i32 WH_MOUSE_LL = 14;
constexpr i32 HC_ACTION = 0;
constexpr i32 HC_NOREMOVE = 3;

// The chain the calling thread is walking, one frame per nested hook call,
// so CallNextHookEx (whose hhk argument Learn says is ignored) knows which
// procedure is next.
struct HookFrame {
    std::vector<User32ExtManager::HookRegistration> chain;
    usize index = 0;
};
thread_local std::vector<HookFrame> t_hook_frames;

// Guest memory for the structures a hook procedure receives (CWPSTRUCT,
// MOUSEHOOKSTRUCT, KBDLLHOOKSTRUCT, ...): one page per thread, used as a
// stack because hook calls nest.
struct HookScratch {
    GuestAddr base = 0;
    u32 used = 0;
};
thread_local HookScratch t_hook_scratch;
constexpr u32 kHookScratchBytes = 0x2000;

struct ScratchMark {
    u32 saved = t_hook_scratch.used;
    ~ScratchMark() { t_hook_scratch.used = saved; }
};

// A hook type Vitreo generates calls for. WH_HARDWARE is accepted and never
// called, as on Windows (no non-mouse, non-keyboard hardware messages
// exist); journal hooks are refused before this (ERROR_ACCESS_DENIED).
bool hook_type_dispatched(i32 id) noexcept {
    return id >= WH_MSGFILTER && id <= WH_MOUSE_LL &&
           id != WH_JOURNALRECORD && id != WH_JOURNALPLAYBACK;
}
} // namespace

GuestAddr User32ExtManager::hook_scratch(u32 bytes) {
    if (t_hook_scratch.base == 0) {
        t_hook_scratch.base = m_memory.map_first_fit(
            static_cast<GuestAddr>(0x3E000000), kHookScratchBytes,
            MemoryProtection::ReadWrite);
        if (t_hook_scratch.base == 0) return 0;
    }
    bytes = (bytes + 15u) & ~15u;
    if (t_hook_scratch.used + bytes > kHookScratchBytes) return 0;
    const GuestAddr at = t_hook_scratch.base + t_hook_scratch.used;
    t_hook_scratch.used += bytes;
    return at;
}

std::vector<User32ExtManager::HookRegistration>
User32ExtManager::hook_chain(i32 id_hook, u32 thread_id) const {
    // Learn (Hooks Overview): each chain is called most recently installed
    // first; a thread's own hooks run before the global ones.
    std::vector<HookRegistration> local;
    std::vector<HookRegistration> global;
    {
        std::lock_guard lock(m_hook_mutex);
        for (const auto& [handle, hook] : m_hooks) {
            (void)handle;
            if (hook.id_hook != id_hook) continue;
            if (hook.thread_id == thread_id) {
                local.push_back(hook);
            } else if (hook.thread_id == 0) {
                global.push_back(hook);
            }
        }
    }
    const auto newest_first = [](const HookRegistration& a,
                                 const HookRegistration& b) {
        return a.handle > b.handle;
    };
    std::sort(local.begin(), local.end(), newest_first);
    std::sort(global.begin(), global.end(), newest_first);
    local.insert(local.end(), global.begin(), global.end());
    return local;
}

bool User32ExtManager::call_hook_chain(i32 id_hook, i32 code, u64 wparam,
                                       u64 lparam, u64& result) {
    if (!m_guest_call ||
        m_hook_type_count[static_cast<usize>(id_hook + 1)].load(
            std::memory_order_acquire) == 0) {
        return false;
    }
    auto chain = hook_chain(id_hook, ThreadManager::get_current_thread_id_static());
    if (chain.empty()) return false;
    const HookRegistration first = chain.front();
    t_hook_frames.push_back(HookFrame{std::move(chain), 0});
    struct Pop { ~Pop() { t_hook_frames.pop_back(); } } pop;
    result = 0;
    if (debug_hook_allows(first, code, wparam, lparam)) {
        const u64 args[3] = {static_cast<u64>(static_cast<u32>(code)), wparam, lparam};
        result = m_guest_call(first.proc, args, 3, first.is_64bit);
    }
    return true;
}

bool User32ExtManager::debug_hook_allows(const HookRegistration& target, i32 code,
                                         u64 wparam, u64 lparam) {
    // WH_DEBUG (Learn DebugProc) runs before any other hook procedure:
    // wParam the target's hook type, lParam a DEBUGHOOKINFO {idThread,
    // idThreadInstaller, lParam, wParam, code}. Nonzero keeps the target
    // from being called.
    if (target.id_hook == WH_DEBUG ||
        m_hook_type_count[static_cast<usize>(WH_DEBUG + 1)].load(
            std::memory_order_acquire) == 0) {
        return true;
    }
    ScratchMark mark;
    const bool is64 = m_is_64bit;
    const GuestAddr at = hook_scratch(is64 ? 32u : 20u);
    if (at == 0) return true;
    (void)m_memory.write_u32(at + 0, ThreadManager::get_current_thread_id_static());
    (void)m_memory.write_u32(at + 4, target.owner_thread);
    if (is64) {
        (void)m_memory.write_u64(at + 8, lparam);
        (void)m_memory.write_u64(at + 16, wparam);
        (void)m_memory.write_u32(at + 24, static_cast<u32>(code));
        (void)m_memory.write_u32(at + 28, 0);
    } else {
        (void)m_memory.write_u32(at + 8, static_cast<u32>(lparam));
        (void)m_memory.write_u32(at + 12, static_cast<u32>(wparam));
        (void)m_memory.write_u32(at + 16, static_cast<u32>(code));
    }
    u64 verdict = 0;
    (void)call_hook_chain(WH_DEBUG, HC_ACTION,
                          static_cast<u64>(static_cast<u32>(target.id_hook)),
                          at, verdict);
    return verdict == 0;
}

void User32ExtManager::dispatch_foreground_idle_hook() {
    // ForegroundIdleProc (Learn): called when the foreground thread is about
    // to become idle; the caller checked that this is that thread.
    u64 ignored = 0;
    (void)call_hook_chain(WH_FOREGROUNDIDLE, HC_ACTION, 0, 0, ignored);
}

bool User32ExtManager::call_msg_filter(windowing::MSG& msg, i32 code) {
    // CallMsgFilter (Learn): WH_SYSMSGFILTER first; if no procedure there
    // processed the message, WH_MSGFILTER. lParam is the MSG, which a hook
    // may change. True when a hook processed it (the caller then skips it).
    if (!m_guest_call ||
        (m_hook_type_count[static_cast<usize>(WH_SYSMSGFILTER + 1)].load(
             std::memory_order_acquire) == 0 &&
         m_hook_type_count[static_cast<usize>(WH_MSGFILTER + 1)].load(
             std::memory_order_acquire) == 0)) {
        return false;
    }
    ScratchMark mark;
    const bool is64 = m_is_64bit;
    const GuestAddr at = hook_scratch(is64 ? 48u : 28u);
    if (at == 0) return false;
    write_guest_msg(at, msg, is64);
    u64 result = 0;
    bool processed = call_hook_chain(WH_SYSMSGFILTER, code, 0, at, result) &&
                     result != 0;
    if (!processed) {
        processed = call_hook_chain(WH_MSGFILTER, code, 0, at, result) &&
                    result != 0;
    }
    msg = read_guest_msg(at, is64, msg);
    return processed;
}

void User32ExtManager::write_guest_msg(GuestAddr at, const windowing::MSG& msg,
                                       bool is64) {
    // MSG: x64 hwnd@0 message@8 wParam@16 lParam@24 time@32 pt@36 (48 bytes);
    //      x86 hwnd@0 message@4 wParam@8 lParam@12 time@16 pt@20 (28 bytes).
    if (is64) {
        (void)m_memory.write_u64(at + 0, static_cast<u64>(msg.hwnd));
        (void)m_memory.write_u32(at + 8, msg.message);
        (void)m_memory.write_u32(at + 12, 0);
        (void)m_memory.write_u64(at + 16, msg.wParam);
        (void)m_memory.write_u64(at + 24, msg.lParam);
        (void)m_memory.write_u32(at + 32, msg.time);
        (void)m_memory.write_u32(at + 36, static_cast<u32>(msg.pt.x));
        (void)m_memory.write_u32(at + 40, static_cast<u32>(msg.pt.y));
        (void)m_memory.write_u32(at + 44, 0);
    } else {
        (void)m_memory.write_u32(at + 0, static_cast<u32>(msg.hwnd));
        (void)m_memory.write_u32(at + 4, msg.message);
        (void)m_memory.write_u32(at + 8, static_cast<u32>(msg.wParam));
        (void)m_memory.write_u32(at + 12, static_cast<u32>(msg.lParam));
        (void)m_memory.write_u32(at + 16, msg.time);
        (void)m_memory.write_u32(at + 20, static_cast<u32>(msg.pt.x));
        (void)m_memory.write_u32(at + 24, static_cast<u32>(msg.pt.y));
    }
}

windowing::MSG User32ExtManager::read_guest_msg(GuestAddr at, bool is64,
                                                const windowing::MSG& fallback) {
    windowing::MSG msg = fallback;
    if (is64) {
        msg.hwnd = static_cast<windowing::HWND>(m_memory.read_u64(at + 0).value_or(msg.hwnd));
        msg.message = m_memory.read_u32(at + 8).value_or(msg.message);
        msg.wParam = m_memory.read_u64(at + 16).value_or(msg.wParam);
        msg.lParam = m_memory.read_u64(at + 24).value_or(msg.lParam);
    } else {
        msg.hwnd = static_cast<windowing::HWND>(m_memory.read_u32(at + 0).value_or(msg.hwnd));
        msg.message = m_memory.read_u32(at + 4).value_or(msg.message);
        msg.wParam = m_memory.read_u32(at + 8).value_or(static_cast<u32>(msg.wParam));
        msg.lParam = m_memory.read_u32(at + 12).value_or(static_cast<u32>(msg.lParam));
    }
    return msg;
}

void User32ExtManager::impl_CallMsgFilterW(Marshaler& m) {
    // BOOL CallMsgFilter(LPMSG lpMsg, int nCode): the application's own
    // modal loop feeding WH_SYSMSGFILTER / WH_MSGFILTER.
    const GuestAddr msg_ptr = m.arg_ptr(0);
    const i32 code = static_cast<i32>(m.arg(1));
    if (msg_ptr == 0 || !m_guest_call) {
        m.set_return(0u);
        return;
    }
    u64 result = 0;
    bool processed = call_hook_chain(WH_SYSMSGFILTER, code, 0, msg_ptr, result) &&
                     result != 0;
    if (!processed) {
        processed = call_hook_chain(WH_MSGFILTER, code, 0, msg_ptr, result) &&
                    result != 0;
    }
    m.set_return(processed ? 1u : 0u);
}

void User32ExtManager::dispatch_shell_hook(i32 code, u64 wparam, u64 lparam) {
    // ShellProc (Learn): the HSHELL_* notifications Vitreo generates.
    u64 ignored = 0;
    (void)call_hook_chain(WH_SHELL, code, wparam, lparam, ignored);
}

void User32ExtManager::impl_NotifyWinEvent(Marshaler& m) {
    // void NotifyWinEvent(DWORD event, HWND hwnd, LONG idObject, LONG idChild)
    // Delivers to this process's WinEvent hooks (s2msg_queue_probe
    // winevent_after_notify / _after_peek: out-of-context hooks run at the
    // registering thread's next message retrieval, not inside the call).
    const u32 event = static_cast<u32>(m.arg(0));
    const auto hwnd = static_cast<windowing::HWND>(m.arg(1));
    const i32 object_id = static_cast<i32>(m.arg(2));
    const i32 child_id = static_cast<i32>(m.arg(3));
    notify_win_event(event, hwnd, object_id, child_id);
    m.set_return(0u);
}

void User32ExtManager::impl_SetWindowsHookExW(Marshaler& m) {
    // HHOOK SetWindowsHookEx(int idHook, HOOKPROC lpfn, HINSTANCE hmod,
    //                        DWORD dwThreadId). Errors are Windows 11's,
    // measured by s2msg_queue_probe (hook_id sweep, hook_* lines).
    const i32 idHook = static_cast<i32>(m.arg(0));
    const GuestAddr proc = m.arg_ptr(1);
    const u64 module = m.arg(2);
    const u32 thread_id = static_cast<u32>(m.arg(3));
    const auto fail = [&](u32 error) {
        set_last_error(error);
        VITREO_TRACE("[u32ext] SetWindowsHookExW hookId=%d proc=0x%llx "
                     "thread=%u -> NULL (error %u)\n",
                     idHook, static_cast<unsigned long long>(proc),
                     thread_id, error);
        m.set_return(0);
    };
    if (idHook < WH_MSGFILTER || idHook > WH_MOUSE_LL) {
        fail(error_codes::ERROR_INVALID_HOOK_FILTER);
        return;
    }
    if (idHook == WH_JOURNALRECORD || idHook == WH_JOURNALPLAYBACK) {
        fail(error_codes::ERROR_ACCESS_DENIED);
        return;
    }
    if (proc == 0) {
        fail(error_codes::ERROR_INVALID_FILTER_PROC);
        return;
    }
    const bool global_only = idHook == WH_SYSMSGFILTER ||
                             idHook == WH_KEYBOARD_LL || idHook == WH_MOUSE_LL;
    if (thread_id != 0) {
        if (m_thread_alive && !m_thread_alive(thread_id)) {
            fail(error_codes::ERROR_INVALID_PARAMETER);
            return;
        }
        if (global_only) {
            fail(error_codes::ERROR_GLOBAL_ONLY_HOOK);
            return;
        }
    } else if (idHook != WH_KEYBOARD_LL && idHook != WH_MOUSE_LL) {
        // A global hook's procedure must live in a DLL: NULL and the EXE's
        // own module are refused (hook_id N global_self err=594).
        const GuestAddr exe = m_module_mgr != nullptr
            ? m_module_mgr->main_module_base() : 0;
        if (module == 0 || static_cast<GuestAddr>(module) == exe) {
            fail(error_codes::ERROR_HOOK_NEEDS_HMOD);
            return;
        }
    }
    if (!hook_type_dispatched(idHook)) {
        fail(error_codes::ERROR_INVALID_HOOK_FILTER);
        return;
    }
    if (thread_id == 0 && idHook != WH_KEYBOARD_LL && idHook != WH_MOUSE_LL) {
        // A global hook on Windows is injected into every process on the
        // desktop; Vitreo runs it for this process's threads only.
        static std::atomic<bool> logged{false};
        if (!logged.exchange(true)) {
            std::fprintf(stderr,
                "[vitreo] UNIMPLEMENTED user32!SetWindowsHookExW: global hook "
                "type %d runs in this process only, not in other processes\n",
                idHook);
        }
    }
    // Partly generated types: the hook is installed and called for the
    // events Vitreo raises, and the rest is logged once.
    const char* partial = nullptr;
    if (idHook == WH_CBT) {
        partial = "WH_CBT is called for HCBT_CREATEWND of dialog windows only";
    } else if (idHook == WH_SHELL) {
        partial = "WH_SHELL is called for HSHELL_WINDOWACTIVATED only";
    } else if (idHook == WH_MSGFILTER || idHook == WH_SYSMSGFILTER) {
        partial = "message filter hooks are called by CallMsgFilter and modal "
                  "dialog boxes, not by menus or message boxes";
    }
    if (partial != nullptr) {
        static std::atomic<u32> logged{0};
        const u32 bit = 1u << static_cast<u32>(idHook + 1);
        if ((logged.fetch_or(bit) & bit) == 0) {
            std::fprintf(stderr,
                "[vitreo] UNIMPLEMENTED user32!SetWindowsHookExW: %s\n", partial);
        }
    }

    HookRegistration reg{};
    reg.id_hook = idHook;
    reg.proc = proc;
    reg.module = module;
    reg.thread_id = thread_id;
    reg.is_64bit = m.cpu_is_64bit();
    // LL hooks run on the thread that installed them (Learn LowLevelKeyboardProc).
    reg.owner_thread = ThreadManager::get_current_thread_id_static();
    {
        std::lock_guard lock(m_hook_mutex);
        reg.handle = m_next_hook_handle++;
        m_hooks.emplace(reg.handle, reg);
    }
    m_hook_type_count[static_cast<usize>(idHook + 1)].fetch_add(
        1, std::memory_order_acq_rel);
    VITREO_TRACE(
        "[u32ext] SetWindowsHookExW hookId=%d proc=0x%llx "
        "module=0x%llx thread=%u -> 0x%x\n",
        idHook, static_cast<unsigned long long>(proc),
        static_cast<unsigned long long>(module), thread_id, reg.handle);
    m.set_return(reg.handle);
}

void User32ExtManager::impl_CallNextHookEx(Marshaler& m) {
    // LRESULT CallNextHookEx(HHOOK hhk, int nCode, WPARAM, LPARAM). Learn:
    // hhk is ignored; the next procedure is the one after the procedure
    // currently running in this thread's chain, and its result is returned
    // (s2msg_queue_probe getmsg_chain next_ret=aa). No hook running, or the
    // end of the chain: 0 (callnext_null_hook).
    (void)m.arg(0);
    const i32 code = static_cast<i32>(m.arg(1));
    const u64 wparam = m.arg(2);
    const u64 lparam = m.arg(3);
    if (t_hook_frames.empty() || !m_guest_call) {
        m.set_return(0);
        return;
    }
    const usize frame = t_hook_frames.size() - 1;
    const usize at = t_hook_frames[frame].index;
    usize next = at + 1;
    // A procedure unhooked while the chain runs is skipped.
    while (next < t_hook_frames[frame].chain.size()) {
        std::lock_guard lock(m_hook_mutex);
        if (m_hooks.contains(t_hook_frames[frame].chain[next].handle)) break;
        ++next;
    }
    if (next >= t_hook_frames[frame].chain.size()) {
        m.set_return(0);
        return;
    }
    const HookRegistration hook = t_hook_frames[frame].chain[next];
    t_hook_frames[frame].index = next;
    u64 result = 0;
    if (debug_hook_allows(hook, code, wparam, lparam)) {
        const u64 args[3] = {static_cast<u64>(static_cast<u32>(code)), wparam, lparam};
        result = m_guest_call(hook.proc, args, 3, hook.is_64bit);
    }
    t_hook_frames[frame].index = at;
    m.set_return(result);
}

void User32ExtManager::impl_UnhookWindowsHookEx(Marshaler& m) {
    // Unknown, NULL or already-removed handle: FALSE /
    // ERROR_INVALID_HOOK_HANDLE (unhook_null / unhook_bogus / unhook_b_again).
    const u32 hhk = static_cast<u32>(m.arg(0));
    i32 id_hook = 0;
    bool removed = false;
    {
        std::lock_guard lock(m_hook_mutex);
        const auto it = m_hooks.find(hhk);
        if (it != m_hooks.end()) {
            id_hook = it->second.id_hook;
            m_hooks.erase(it);
            removed = true;
        }
    }
    if (removed) {
        m_hook_type_count[static_cast<usize>(id_hook + 1)].fetch_sub(
            1, std::memory_order_acq_rel);
    } else {
        set_last_error(error_codes::ERROR_INVALID_HOOK_HANDLE);
    }
    VITREO_TRACE(
        "[u32ext] UnhookWindowsHookEx 0x%x -> %s\n",
        hhk, removed ? "TRUE" : "FALSE");
    m.set_return(removed ? 1u : 0u);
}

bool User32ExtManager::dispatch_cbt_create_hook(
    windowing::HWND hwnd, u32 style) {
    constexpr u64 HCBT_CREATEWND = 3;
    if (!m_guest_call ||
        m_hook_type_count[static_cast<usize>(WH_CBT + 1)].load(
            std::memory_order_acquire) == 0) {
        return true;
    }
    // CBT_CREATEWND {LPCREATESTRUCT lpcs; HWND hwndInsertAfter} and
    // CREATESTRUCT in the guest's layout (x64: 16 + 80 bytes, x86: 8 + 48).
    // The dialog creation path supplies only the style.
    ScratchMark mark;
    const bool is64 = m_is_64bit;
    const u32 cbt_size = is64 ? 16u : 8u;
    const u32 cs_size = is64 ? 80u : 48u;
    const GuestAddr cbt_addr = hook_scratch(cbt_size + cs_size);
    if (cbt_addr == 0) return true;
    const GuestAddr cs_addr = cbt_addr + cbt_size;
    std::array<u8, 96> zero{};
    (void)m_memory.write_bytes(cbt_addr, zero.data(), cbt_size + cs_size);
    (void)m_memory.write_u32(cs_addr + (is64 ? 48u : 32u), style);
    if (is64) {
        (void)m_memory.write_u64(cbt_addr, cs_addr);
    } else {
        (void)m_memory.write_u32(cbt_addr, static_cast<u32>(cs_addr));
    }
    u64 result = 0;
    if (!call_hook_chain(WH_CBT, static_cast<i32>(HCBT_CREATEWND),
                         static_cast<u64>(hwnd), cbt_addr, result)) {
        return true;
    }
    if (result != 0) {
        VITREO_TRACE("[u32ext] HCBT_CREATEWND vetoed hwnd=0x%x result=0x%llx\n",
                     hwnd, static_cast<unsigned long long>(result));
        return false;
    }
    return true;
}

void User32ExtManager::dispatch_get_message_hook(
    GuestAddr msg_ptr, u32 remove_flags) {
    // GetMsgProc (Learn): nCode HC_ACTION, wParam PM_REMOVE / PM_NOREMOVE,
    // lParam the MSG the caller is about to receive (hooks may edit it:
    // getmsg_chain msg_wparam=a). Called for PM_NOREMOVE too
    // (getmsg_noremove), never when nothing was retrieved.
    if (msg_ptr == 0) return;
    u64 ignored = 0;
    (void)call_hook_chain(WH_GETMESSAGE, HC_ACTION,
                          (remove_flags & windowing::PM::REMOVE) != 0 ? 1u : 0u,
                          msg_ptr, ignored);
}

void User32ExtManager::dispatch_call_wndproc_hooks(
    windowing::HWND hwnd, u32 message, u64 wparam, u64 lparam,
    bool cross_thread, bool after, u64 lresult) {
    const i32 id = after ? WH_CALLWNDPROCRET : WH_CALLWNDPROC;
    if (!m_guest_call ||
        m_hook_type_count[static_cast<usize>(id + 1)].load(
            std::memory_order_acquire) == 0) {
        return;
    }
    // CWPSTRUCT {lParam, wParam, message, hwnd} / CWPRETSTRUCT {lResult,
    // lParam, wParam, message, hwnd}. The hook's wParam is 1 for a message
    // another thread sent and 0 for the thread's own SendMessage
    // (s2msg_queue_probe callwndproc "cr" / callwndproc_cross_thread_wparam=1).
    ScratchMark mark;
    const bool is64 = m_is_64bit;
    const u32 size = after ? (is64 ? 40u : 20u) : (is64 ? 32u : 16u);
    const GuestAddr at = hook_scratch(size);
    if (at == 0) return;
    const u32 ptr = is64 ? 8u : 4u;
    u32 off = 0;
    const auto put_ptr = [&](u64 value) {
        if (is64) (void)m_memory.write_u64(at + off, value);
        else (void)m_memory.write_u32(at + off, static_cast<u32>(value));
        off += ptr;
    };
    if (after) put_ptr(lresult);
    put_ptr(lparam);
    put_ptr(wparam);
    (void)m_memory.write_u32(at + off, message);
    if (is64) (void)m_memory.write_u32(at + off + 4, 0);
    off += ptr;
    put_ptr(static_cast<u64>(hwnd));
    u64 ignored = 0;
    (void)call_hook_chain(id, HC_ACTION, cross_thread ? 1u : 0u, at, ignored);
}

bool User32ExtManager::dispatch_input_hooks(const windowing::MSG& msg,
                                            bool removed) {
    // WH_KEYBOARD / WH_MOUSE run when GetMessage/PeekMessage retrieve
    // HARDWARE keyboard or mouse input -- never for a posted WM_KEYDOWN
    // (posted_input_hooks kbd=0 mouse=0). Nonzero discards the message.
    // nCode HC_ACTION when it is removed, HC_NOREMOVE for PM_NOREMOVE.
    if (!msg.hardware_input || !m_guest_call) return false;
    const i32 code = removed ? HC_ACTION : HC_NOREMOVE;
    const u32 id = msg.message;
    u64 result = 0;
    if (id >= windowing::WM::KEYDOWN && id <= 0x0109u) {
        if (id != windowing::WM::KEYDOWN && id != windowing::WM::KEYUP &&
            id != windowing::WM::SYSKEYDOWN && id != windowing::WM::SYSKEYUP) {
            return false;
        }
        // KeyboardProc: wParam virtual key, lParam the keystroke flags.
        return call_hook_chain(WH_KEYBOARD, code, msg.wParam,
                               msg.lParam & 0xFFFFFFFFu, result) &&
               result != 0;
    }
    const bool mouse = (id >= windowing::WM::MOUSEMOVE && id <= 0x020Eu) ||
                       (id >= 0x00A0u && id <= 0x00ADu);
    if (!mouse ||
        m_hook_type_count[static_cast<usize>(WH_MOUSE + 1)].load(
            std::memory_order_acquire) == 0) {
        return false;
    }
    // MouseProc: wParam the message, lParam MOUSEHOOKSTRUCTEX {pt (screen),
    // hwnd, wHitTestCode, dwExtraInfo, mouseData}.
    ScratchMark mark;
    const bool is64 = m_is_64bit;
    const GuestAddr at = hook_scratch(is64 ? 40u : 24u);
    if (at == 0) return false;
    (void)m_memory.write_u32(at + 0, static_cast<u32>(msg.pt.x));
    (void)m_memory.write_u32(at + 4, static_cast<u32>(msg.pt.y));
    const bool client = id >= windowing::WM::MOUSEMOVE;
    const u32 hit = client ? 1u /* HTCLIENT */ : static_cast<u32>(msg.wParam);
    if (is64) {
        (void)m_memory.write_u64(at + 8, static_cast<u64>(msg.hwnd));
        (void)m_memory.write_u32(at + 16, hit);
        (void)m_memory.write_u32(at + 20, 0);
        (void)m_memory.write_u64(at + 24, 0);
        (void)m_memory.write_u32(at + 32, static_cast<u32>(
            (id == 0x020Au || id == 0x020Eu || (id >= 0x020Bu && id <= 0x020Du))
                ? (msg.wParam & 0xFFFF0000u) >> 16 << 16 : 0u));
    } else {
        (void)m_memory.write_u32(at + 8, static_cast<u32>(msg.hwnd));
        (void)m_memory.write_u32(at + 12, hit);
        (void)m_memory.write_u32(at + 16, 0);
        (void)m_memory.write_u32(at + 20, static_cast<u32>(
            (id == 0x020Au || id == 0x020Eu || (id >= 0x020Bu && id <= 0x020Du))
                ? (msg.wParam & 0xFFFF0000u) >> 16 << 16 : 0u));
    }
    return call_hook_chain(WH_MOUSE, code, id, at, result) && result != 0;
}

bool User32ExtManager::low_level_hooks_installed(bool keyboard) const {
    const i32 id = keyboard ? WH_KEYBOARD_LL : WH_MOUSE_LL;
    return m_hook_type_count[static_cast<usize>(id + 1)].load(
               std::memory_order_acquire) != 0;
}

std::optional<u32> User32ExtManager::low_level_hook_thread(bool keyboard) const {
    const i32 id = keyboard ? WH_KEYBOARD_LL : WH_MOUSE_LL;
    std::lock_guard lock(m_hook_mutex);
    std::optional<u32> owner;
    u32 newest = 0;
    for (const auto& [handle, hook] : m_hooks) {
        if (hook.id_hook == id && handle >= newest) {
            newest = handle;
            owner = hook.owner_thread;
        }
    }
    return owner;
}

void User32ExtManager::install_low_level_input_filter() {
    m_queue.set_input_filter([this](const windowing::MSG& msg, bool front) {
        const u32 id = msg.message;
        const bool keyboard =
            id == windowing::WM::KEYDOWN || id == windowing::WM::KEYUP ||
            id == windowing::WM::SYSKEYDOWN || id == windowing::WM::SYSKEYUP;
        const bool mouse = (id >= windowing::WM::MOUSEMOVE && id <= 0x020Eu) ||
                           (id >= 0x00A0u && id <= 0x00ADu);
        if (!keyboard && !mouse) return false;
        if (!low_level_hooks_installed(keyboard)) {
            // A registered hot key consumes its key-down (RegisterHotKey).
            return keyboard && dispatch_hotkey(msg);
        }
        const auto owner = low_level_hook_thread(keyboard);
        if (!owner) return false;
        windowing::MSG event = msg;
        if (mouse && id != 0x020Au && id != 0x020Eu) {
            // MSLLHOOKSTRUCT.pt is in screen coordinates; the queued lParam
            // is client-relative (non-client and wheel lParams already are
            // screen coordinates).
            i32 x = static_cast<i16>(msg.lParam & 0xFFFF);
            i32 y = static_cast<i16>((msg.lParam >> 16) & 0xFFFF);
            if (id >= windowing::WM::MOUSEMOVE) {
                windowing::HWND cur = msg.hwnd;
                while (cur != windowing::HWND_DESKTOP && cur != windowing::NULL_HWND) {
                    const auto* w = m_hwnd_mgr.find(cur);
                    if (w == nullptr) break;
                    x += w->rect.left;
                    y += w->rect.top;
                    cur = w->parent;
                }
            }
            event.pt = {x, y};
        } else if (mouse) {
            event.pt = {static_cast<i16>(msg.lParam & 0xFFFF),
                        static_cast<i16>((msg.lParam >> 16) & 0xFFFF)};
        }
        // Asynchronous: the input source never waits on a guest thread. The
        // event reaches its queue (in order) once the hook thread ran the
        // chain; Windows' LowLevelHooksTimeout fallback is not modelled.
        return m_queue.notify_thread_send(0, *owner, [this, event, front]() -> u64 {
            if (!run_low_level_hooks(event) && !dispatch_hotkey(event)) {
                (void)m_queue.post_input_unfiltered(event, front);
            }
            return 0;
        });
    });
}

bool User32ExtManager::run_low_level_hooks(const windowing::MSG& msg) {
    // LowLevelKeyboardProc / LowLevelMouseProc (Learn): nCode HC_ACTION,
    // wParam the input message, lParam KBDLLHOOKSTRUCT / MSLLHOOKSTRUCT.
    // Nonzero keeps the input from reaching any window. Runs on the thread
    // that installed the hook (see MessageQueue input filter).
    const u32 id = msg.message;
    const bool keyboard = id == windowing::WM::KEYDOWN || id == windowing::WM::KEYUP ||
                          id == windowing::WM::SYSKEYDOWN || id == windowing::WM::SYSKEYUP;
    ScratchMark mark;
    const bool is64 = m_is_64bit;
    u64 result = 0;
    if (keyboard) {
        // KBDLLHOOKSTRUCT {vkCode, scanCode, flags, time, dwExtraInfo}.
        const GuestAddr at = hook_scratch(is64 ? 24u : 20u);
        if (at == 0) return false;
        const u32 lp = static_cast<u32>(msg.lParam);
        u32 flags = 0;
        if ((lp & (1u << 24)) != 0) flags |= 0x01;  // LLKHF_EXTENDED
        if ((lp & (1u << 29)) != 0) flags |= 0x20;  // LLKHF_ALTDOWN
        if ((lp & (1u << 31)) != 0) flags |= 0x80;  // LLKHF_UP
        (void)m_memory.write_u32(at + 0, static_cast<u32>(msg.wParam & 0xFF));
        (void)m_memory.write_u32(at + 4, (lp >> 16) & 0xFFu);
        (void)m_memory.write_u32(at + 8, flags);
        (void)m_memory.write_u32(at + 12, msg.time);
        if (is64) {
            (void)m_memory.write_u32(at + 16, 0);
            (void)m_memory.write_u32(at + 20, 0);
        } else {
            (void)m_memory.write_u32(at + 16, 0);
        }
        return call_hook_chain(WH_KEYBOARD_LL, HC_ACTION, id, at, result) &&
               result != 0;
    }
    // MSLLHOOKSTRUCT {pt (screen), mouseData, flags, time, dwExtraInfo}.
    // Non-client button messages reach the hook as their client forms.
    u32 hook_message = id;
    if (id >= 0x00A0u && id <= 0x00ADu) hook_message = id - 0x00A0u + 0x0200u;
    const GuestAddr at = hook_scratch(is64 ? 32u : 24u);
    if (at == 0) return false;
    u32 mouse_data = 0;
    if (hook_message == 0x020Au || hook_message == 0x020Eu ||
        (hook_message >= 0x020Bu && hook_message <= 0x020Du)) {
        mouse_data = static_cast<u32>(msg.wParam & 0xFFFF0000u);
    }
    (void)m_memory.write_u32(at + 0, static_cast<u32>(msg.pt.x));
    (void)m_memory.write_u32(at + 4, static_cast<u32>(msg.pt.y));
    (void)m_memory.write_u32(at + 8, mouse_data);
    (void)m_memory.write_u32(at + 12, 0);
    (void)m_memory.write_u32(at + 16, msg.time);
    (void)m_memory.write_u32(at + 20, 0);
    if (is64) (void)m_memory.write_u64(at + 24, 0);
    return call_hook_chain(WH_MOUSE_LL, HC_ACTION, hook_message, at, result) &&
           result != 0;
}

void User32ExtManager::impl_SetWinEventHook(Marshaler& m) {
    // HWINEVENTHOOK SetWinEventHook(DWORD eventMin, DWORD eventMax,
    //     HMODULE hmodWinEventProc, WINEVENTPROC pfn, DWORD idProcess,
    //     DWORD idThread, DWORD dwFlags). Errors measured by
    // s2msg_queue_probe winevent_*.
    const u32 event_min = static_cast<u32>(m.arg(0));
    const u32 event_max = static_cast<u32>(m.arg(1));
    const u64 module = m.arg(2);
    const GuestAddr proc = m.arg_ptr(3);
    const u32 process_id = m.arg32(4);
    const u32 thread_id = m.arg32(5);
    const u32 flags = m.arg32(6);
    constexpr u32 kValidFlags = 0x7u;
    constexpr u32 WINEVENT_INCONTEXT_FLAG = 0x4u;
    u32 error = 0;
    if (event_min > event_max) {
        error = error_codes::ERROR_INVALID_HOOK_FILTER;
    } else if (proc == 0) {
        error = error_codes::ERROR_INVALID_FILTER_PROC;
    } else if ((flags & ~kValidFlags) != 0) {
        error = error_codes::ERROR_INVALID_FLAGS;
    } else if ((flags & WINEVENT_INCONTEXT_FLAG) != 0 && module == 0) {
        error = error_codes::ERROR_HOOK_NEEDS_HMOD;
    }
    if (error != 0) {
        set_last_error(error);
        m.set_return(0);
        return;
    }

    const u32 handle = m_next_win_event_hook++;
    const u32 registering_process =
        m_process_mgr != nullptr ? m_process_mgr->get_current_process_id() : 0;
    {
        std::lock_guard lock(m_win_event_hook_mutex);
        m_win_event_hooks.emplace(
            handle,
            WinEventHookRegistration{
                event_min,
                event_max,
                proc,
                process_id,
                thread_id,
                flags,
                ThreadManager::get_current_thread_id_static(),
                registering_process,
                m.cpu_is_64bit(),
            });
    }
    VITREO_TRACE(
        "[u32ext] SetWinEventHook events=0x%x..0x%x proc=0x%llx "
        "pid=%u tid=%u flags=0x%x -> 0x%x\n",
        event_min, event_max, static_cast<unsigned long long>(proc),
        process_id, thread_id, flags, handle);
    if (windowing::diag::window_diag()) {
        std::fprintf(
            stderr,
            "[window-diag] SetWinEventHook events=0x%x..0x%x "
            "proc=0x%llx tid=%u -> 0x%x\n",
            event_min, event_max, static_cast<unsigned long long>(proc),
            ThreadManager::get_current_thread_id_static(), handle);
    }
    m.set_return(handle);
}

void User32ExtManager::impl_UnhookWinEvent(Marshaler& m) {
    const u32 handle = static_cast<u32>(m.arg(0));
    bool removed = false;
    {
        std::lock_guard lock(m_win_event_hook_mutex);
        removed = m_win_event_hooks.erase(handle) != 0;
    }
    // Success leaves the last error untouched (measured on Windows 11:
    // last_error_on_success_user_probe.expected).
    if (!removed) set_last_error(error_codes::ERROR_INVALID_HANDLE);
    VITREO_TRACE("[u32ext] UnhookWinEvent hook=0x%x -> %s\n", handle,
                 removed ? "TRUE" : "FALSE");
    m.set_return(removed ? 1u : 0u);
}

void User32ExtManager::notify_win_event(
    u32 event, windowing::HWND hwnd, i32 object_id, i32 child_id) {
    if (!m_guest_call) return;

    constexpr u32 WINEVENT_SKIPOWNTHREAD = 0x1u;
    constexpr u32 WINEVENT_SKIPOWNPROCESS = 0x2u;
    constexpr u32 WINEVENT_INCONTEXT = 0x4u;

    const u32 event_thread = ThreadManager::get_current_thread_id_static();
    const u32 event_process =
        m_process_mgr != nullptr ? m_process_mgr->get_current_process_id() : 0;
    // dwmsEventTime is GetTickCount's clock (Learn WinEventProc).
    const u32 event_time = tick_count32();

    std::vector<PendingWinEvent> deliver_now;
    std::unordered_map<u32, std::vector<PendingWinEvent>> deliver_queued;
    {
        std::lock_guard lock(m_win_event_hook_mutex);
        for (const auto& entry : m_win_event_hooks) {
            const auto& hook = entry.second;
            if (event < hook.event_min || event > hook.event_max) continue;
            if (hook.process_id != 0 && hook.process_id != event_process) {
                continue;
            }
            if (hook.thread_id != 0 && hook.thread_id != event_thread) {
                continue;
            }
            if ((hook.flags & WINEVENT_SKIPOWNPROCESS) != 0 &&
                hook.registering_process_id != 0 &&
                hook.registering_process_id == event_process) {
                continue;
            }
            if ((hook.flags & WINEVENT_SKIPOWNTHREAD) != 0 &&
                hook.registering_thread_id == event_thread) {
                continue;
            }

            PendingWinEvent pending{
                entry.first,
                hook.proc,
                hook.is_64bit,
                event,
                hwnd,
                object_id,
                child_id,
                event_thread,
                event_time,
            };
            // Learn: WINEVENT_OUTOFCONTEXT (and the default) deliver on the
            // registering thread's message loop. INCONTEXT runs immediately.
            if ((hook.flags & WINEVENT_INCONTEXT) != 0) {
                deliver_now.push_back(pending);
            } else {
                deliver_queued[hook.registering_thread_id].push_back(pending);
            }
        }
        for (auto& [tid, events] : deliver_queued) {
            auto& queue = m_pending_win_events[tid];
            queue.insert(queue.end(), events.begin(), events.end());
        }
    }

    if (windowing::diag::window_diag()) {
        usize queued = 0;
        for (const auto& [tid, events] : deliver_queued) {
            (void)tid;
            queued += events.size();
        }
        std::fprintf(stderr,
                     "[window-diag] WinEvent event=0x%x hwnd=0x%x "
                     "tid=%u incontext=%zu queued=%zu\n",
                     event, hwnd, event_thread, deliver_now.size(), queued);
    }

    for (const auto& pending : deliver_now) {
        const u64 args[7] = {
            pending.hook,
            pending.event,
            static_cast<u64>(pending.hwnd),
            static_cast<u32>(pending.object_id),
            static_cast<u32>(pending.child_id),
            pending.event_thread,
            pending.event_time,
        };
        (void)m_guest_call(pending.proc, args, 7, pending.is_64bit);
    }

    // Wake each registering thread so its GetMessage/PeekMessage/MsgWait
    // drains the events. Windows queues no message for this: a posted WM_NULL
    // would reach the guest's own loop.
    for (const auto& [tid, events] : deliver_queued) {
        if (events.empty()) continue;
        m_queue.notify_thread(tid);
    }
}

void User32ExtManager::drain_pending_win_events(u32 thread_id) {
    if (!m_guest_call || thread_id == 0) return;

    std::vector<PendingWinEvent> pending;
    {
        std::lock_guard lock(m_win_event_hook_mutex);
        auto it = m_pending_win_events.find(thread_id);
        if (it == m_pending_win_events.end() || it->second.empty()) {
            return;
        }
        pending.swap(it->second);
    }

    if (windowing::diag::window_diag()) {
        std::fprintf(stderr,
                     "[window-diag] drain WinEvents tid=%u count=%zu\n",
                     thread_id, pending.size());
    }
    for (const auto& event : pending) {
        const u64 args[7] = {
            event.hook,
            event.event,
            static_cast<u64>(event.hwnd),
            static_cast<u32>(event.object_id),
            static_cast<u32>(event.child_id),
            event.event_thread,
            event.event_time,
        };
        (void)m_guest_call(event.proc, args, 7, event.is_64bit);
    }
}

bool User32ExtManager::register_shell_hook_window(windowing::HWND hwnd) {
    if (!m_hwnd_mgr.is_valid(hwnd) ||
        hwnd == windowing::HWND_DESKTOP ||
        hwnd == windowing::NULL_HWND) {
        return false;
    }
    std::lock_guard lock(m_shell_hook_mutex);
    m_shell_hook_windows.insert(hwnd);
    return true;
}

bool User32ExtManager::deregister_shell_hook_window(windowing::HWND hwnd) {
    std::lock_guard lock(m_shell_hook_mutex);
    return m_shell_hook_windows.erase(hwnd) != 0;
}

void User32ExtManager::impl_RegisterShellHookWindow(Marshaler& m) {
    const auto hwnd = static_cast<windowing::HWND>(m.arg(0));
    const bool registered = register_shell_hook_window(hwnd);
    VITREO_TRACE(
        "[user32_ext] RegisterShellHookWindow(hwnd=0x%x) -> %s\n",
        hwnd, registered ? "TRUE" : "FALSE");
    m.set_return(registered ? 1u : 0u);
}

void User32ExtManager::impl_DeregisterShellHookWindow(Marshaler& m) {
    const auto hwnd = static_cast<windowing::HWND>(m.arg(0));
    const bool removed = deregister_shell_hook_window(hwnd);
    VITREO_TRACE(
        "[user32_ext] DeregisterShellHookWindow(hwnd=0x%x) -> %s\n",
        hwnd, removed ? "TRUE" : "FALSE");
    m.set_return(removed ? 1u : 0u);
}

} // namespace vitreo::winapi
