// SPDX-License-Identifier: LGPL-2.1-only

/// @file dde_timer_msg.cpp
/// @brief DDE and Timer/Message leftover APIs.
///
/// Closed cut from user32_ext.cpp — verified with plan_extraction.py.

#include "winapi/user32/user32_ext.hpp"
#include "winapi/user32/user32_internal.hpp"
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
#include "winapi/user32/dialogs/dialog.hpp"
#include "winapi/user32/windows/show_window.hpp"
#include "winapi/user32/resources/resources.hpp"
#include "core/pe/parser/pe_structures.hpp"
#include "vitreo/result.hpp"

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

// ----- DDE APIs -----
//
// PackDDElParam / UnpackDDElParam / FreeDDElParam / ReuseDDElParam (Learn,
// dde.h). What Learn leaves open is measured in u32_char_dde_probe
// ("-- ddelparam", both bitnesses):
//  * WM_DDE_ACK, WM_DDE_ADVISE, WM_DDE_DATA and WM_DDE_POKE pack: the lParam
//    is a FIXED global memory object (GlobalLock returns the lParam itself)
//    of two UINT_PTRs, uiLo then uiHi (size 8 on x86, 16 on x64);
//  * WM_DDE_EXECUTE's lParam is uiHi itself; unpacking it gives lo 0;
//  * every other message (the other DDE messages and non-DDE ones alike) is
//    MAKELONG(LOWORD(uiLo), LOWORD(uiHi)), sign-extended to a 64-bit LPARAM
//    (pack_initiate_big ffffffff:def05678), and unpacks to its two words;
//  * unpacking a packed message never checks which packed message packed it;
//    a NULL lParam is FALSE with the outputs zeroed and the last error
//    untouched; an lParam that is not a global object is FALSE, outputs
//    zeroed, ERROR_INVALID_HANDLE; either output may be NULL;
//  * FreeDDElParam is TRUE for an unpacked message and for a NULL lParam;
//  * ReuseDDElParam keeps the object when both messages pack (and returns 0
//    when that lParam cannot be locked), frees it when only the incoming one
//    did, and otherwise packs afresh for the outgoing message.

namespace {

constexpr u32 kWmDdeAdvise = 0x03E2;
constexpr u32 kWmDdeAck = 0x03E4;
constexpr u32 kWmDdeData = 0x03E5;
constexpr u32 kWmDdePoke = 0x03E7;
constexpr u32 kWmDdeExecute = 0x03E8;

bool dde_message_packs(u32 msg) noexcept {
    return msg == kWmDdeAck || msg == kWmDdeAdvise || msg == kWmDdeData ||
           msg == kWmDdePoke;
}

u64 dde_make_lparam(u64 lo, u64 hi) noexcept {
    const u32 word = (static_cast<u32>(lo) & 0xFFFFu) |
                     ((static_cast<u32>(hi) & 0xFFFFu) << 16);
    return static_cast<u64>(static_cast<i64>(static_cast<i32>(word)));
}

/// Store the pair in a packed object; the slot width is the guest pointer.
bool dde_write_pair(Marshaler& m, GuestAddr at, u64 lo, u64 hi) {
    if (m.cpu_is_64bit()) {
        return m.write_u64(at, lo).is_ok() && m.write_u64(at + 8, hi).is_ok();
    }
    return m.write_u32(at, static_cast<u32>(lo)).is_ok() &&
           m.write_u32(at + 4, static_cast<u32>(hi)).is_ok();
}

} // namespace

u64 User32ExtManager::dde_pack_lparam(Marshaler& m, u32 msg, u64 lo, u64 hi) {
    if (msg == kWmDdeExecute) return m.cpu_is_64bit() ? hi : static_cast<u32>(hi);
    if (!dde_message_packs(msg)) {
        const u64 lp = dde_make_lparam(lo, hi);
        return m.cpu_is_64bit() ? lp : static_cast<u32>(lp);
    }
    if (m_global_local == nullptr) {
        static std::atomic_flag reported = ATOMIC_FLAG_INIT;
        if (!reported.test_and_set()) {
            std::fprintf(stderr, "[vitreo] UNIMPLEMENTED user32!PackDDElParam: "
                                 "no global memory manager wired\n");
        }
        set_last_error(error_codes::ERROR_NOT_ENOUGH_MEMORY);
        return 0;
    }
    constexpr u32 kGmemFixed = 0;
    const u64 slot = m.cpu_is_64bit() ? 8u : 4u;
    const u64 handle = m_global_local->global_alloc(kGmemFixed, slot * 2);
    if (handle == 0) return 0;
    if (!dde_write_pair(m, static_cast<GuestAddr>(handle), lo, hi)) {
        (void)m_global_local->global_free(handle);
        return 0;
    }
    return handle;
}

void User32ExtManager::impl_PackDDElParam(Marshaler& m) {
    // LPARAM PackDDElParam(UINT msg, UINT_PTR uiLo, UINT_PTR uiHi)
    m.set_return(dde_pack_lparam(m, m.arg32(0), m.arg(1), m.arg(2)));
}

void User32ExtManager::impl_UnpackDDElParam(Marshaler& m) {
    // BOOL UnpackDDElParam(UINT msg, LPARAM lParam, PUINT_PTR puiLo, PUINT_PTR puiHi)
    const u32 msg = m.arg32(0);
    const u64 lparam = m.cpu_is_64bit() ? m.arg(1) : static_cast<u32>(m.arg(1));
    const GuestAddr out_lo = m.arg_ptr(2);
    const GuestAddr out_hi = m.arg_ptr(3);
    const auto store = [&](GuestAddr at, u64 v) {
        if (at == 0) return;
        if (m.cpu_is_64bit()) (void)m.write_u64(at, v);
        else (void)m.write_u32(at, static_cast<u32>(v));
    };
    if (msg == kWmDdeExecute) {
        store(out_lo, 0);
        store(out_hi, lparam);
        m.set_return(1u);
        return;
    }
    if (!dde_message_packs(msg)) {
        store(out_lo, lparam & 0xFFFFu);
        store(out_hi, (lparam >> 16) & 0xFFFFu);
        m.set_return(1u);
        return;
    }
    u64 data = 0;
    if (lparam != 0 && m_global_local != nullptr) {
        data = m_global_local->global_lock(lparam);  // ERROR_INVALID_HANDLE
    }
    if (data == 0) {
        store(out_lo, 0);
        store(out_hi, 0);
        m.set_return(0u);
        return;
    }
    u64 lo = 0, hi = 0;
    if (m.cpu_is_64bit()) {
        lo = m.read_u64(static_cast<GuestAddr>(data)).value_or(0);
        hi = m.read_u64(static_cast<GuestAddr>(data) + 8).value_or(0);
    } else {
        lo = m.read_u32(static_cast<GuestAddr>(data)).value_or(0);
        hi = m.read_u32(static_cast<GuestAddr>(data) + 4).value_or(0);
    }
    (void)m_global_local->global_unlock(lparam);
    store(out_lo, lo);
    store(out_hi, hi);
    m.set_return(1u);
}

void User32ExtManager::impl_FreeDDElParam(Marshaler& m) {
    // BOOL FreeDDElParam(UINT msg, LPARAM lParam)
    const u32 msg = m.arg32(0);
    const u64 lparam = m.cpu_is_64bit() ? m.arg(1) : static_cast<u32>(m.arg(1));
    if (!dde_message_packs(msg) || lparam == 0 || m_global_local == nullptr) {
        m.set_return(1u);
        return;
    }
    m.set_return(m_global_local->global_free(lparam) == 0 ? 1u : 0u);
}

void User32ExtManager::impl_ReuseDDElParam(Marshaler& m) {
    // LPARAM ReuseDDElParam(LPARAM lParam, UINT msgIn, UINT msgOut,
    //                       UINT_PTR uiLo, UINT_PTR uiHi)
    const u64 lparam = m.cpu_is_64bit() ? m.arg(0) : static_cast<u32>(m.arg(0));
    const u32 msg_in = m.arg32(1);
    const u32 msg_out = m.arg32(2);
    const u64 lo = m.arg(3);
    const u64 hi = m.arg(4);
    if (dde_message_packs(msg_in) && m_global_local != nullptr) {
        if (dde_message_packs(msg_out)) {
            const u64 data = lparam != 0 ? m_global_local->global_lock(lparam) : 0;
            if (data == 0) {
                m.set_return(0u);
                return;
            }
            (void)dde_write_pair(m, static_cast<GuestAddr>(data), lo, hi);
            (void)m_global_local->global_unlock(lparam);
            m.set_return(lparam);
            return;
        }
        if (lparam != 0) (void)m_global_local->global_free(lparam);
    }
    m.set_return(dde_pack_lparam(m, msg_out, lo, hi));
}

// ----- Timer/Message APIs -----

void User32ExtManager::impl_GetDoubleClickTime(Marshaler& m) {
    VITREO_TRACE("[u32ext] GetDoubleClickTime -> 500\n");
    m.set_return(500u);
}

void User32ExtManager::impl_GetMessageTime(Marshaler& m) {
    // Learn GetMessageTime: time of the last message this thread retrieved,
    // not "now" from an unrelated host clock.
    const u32 tick = last_message_time();
    VITREO_TRACE("[u32ext] GetMessageTime -> %u\n", tick);
    m.set_return(tick);
}

} // namespace vitreo::winapi
