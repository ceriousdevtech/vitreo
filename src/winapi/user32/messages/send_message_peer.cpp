// SPDX-License-Identifier: LGPL-2.1-only

/// @file send_message_peer.cpp
/// @brief SendMessage to a window owned by another guest process in the capsule
///        (peer window lookup and the cross-process send helper).

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

// SendMessage to a window another capsule process owns (Learn: an HWND is
// valid system-wide and the system marshals system messages). True when
// `hwnd` was a peer window and the send was attempted.
bool send_message_to_peer_window(Marshaler& m, MessageSendManager& msm,
                                 bool ansi) {
    const auto hwnd = static_cast<windowing::HWND>(m.arg(0));
    if (hwnd == windowing::NULL_HWND || hwnd == 0xFFFF ||
        msm.window_data(hwnd) != nullptr) {
        return false;
    }
    const u32 owner = peer_owner_of(hwnd);
    if (owner == 0) return false;
    u64 lresult = 0;
    (void)send_to_peer(m, ansi, owner, hwnd, static_cast<u32>(m.arg(1)),
                       m.arg(2), m.arg(3), 0, 0xFFFFFFFFu, lresult);
    m.set_return(lresult);
    return true;
}

// SendMessage(Timeout) to a window owned by another guest process in the
// capsule (published in ipc::hwnd_registry). False with the last error set on
// failure; `lresult` holds the window procedure's result on success.
bool send_to_peer(Marshaler& m, bool ansi, u32 owner_pid,
                  windowing::HWND hwnd, u32 msg, u64 wParam, u64 lParam,
                  u32 flags, u32 timeout, u64& lresult) {
    if (xproc_send::classify(msg) == xproc_send::Kind::Unsupported) {
        std::fprintf(stderr,
            "[vitreo] UNIMPLEMENTED user32!SendMessage: cross-process "
            "marshaling of system message 0x%04x\n", msg);
        set_last_error(error_codes::ERROR_NOT_SUPPORTED);
        return false;
    }
    xproc_send::Marshaled marshaled;
    if (!xproc_send::marshal(m, ansi, msg, wParam, lParam, marshaled)) {
        return false;
    }
    ipc::RemoteSendStatus status{};
    std::vector<Byte> out;
    if (!ipc::remote_send_message(static_cast<pid_t>(owner_pid), hwnd, msg,
                                  wParam, lParam, flags, timeout,
                                  marshaled.payload, marshaled.out_capacity,
                                  status, lresult, out)) {
        // The owner exited between lookup and delivery.
        set_last_error(error_codes::ERROR_INVALID_WINDOW_HANDLE);
        return false;
    }
    switch (status) {
    case ipc::RemoteSendStatus::Completed:
        xproc_send::unmarshal_out(m, ansi, msg, wParam, lParam, out, lresult);
        return true;
    case ipc::RemoteSendStatus::TimedOut:
        set_last_error(error_codes::ERROR_TIMEOUT);
        return false;
    case ipc::RemoteSendStatus::TargetGone:
        set_last_error(error_codes::ERROR_INVALID_WINDOW_HANDLE);
        return false;
    case ipc::RemoteSendStatus::Unsupported:
        set_last_error(error_codes::ERROR_NOT_SUPPORTED);
        return false;
    }
    return false;
}

// The peer process that owns `hwnd`, or 0 when it is not a live peer window.
u32 peer_owner_of(windowing::HWND hwnd) {
    const auto entry = ipc::hwnd_registry_lookup(hwnd);
    if (!entry || entry->owner_host_pid == static_cast<u32>(::getpid())) return 0;
    return entry->owner_host_pid;
}

} // namespace vitreo::winapi
