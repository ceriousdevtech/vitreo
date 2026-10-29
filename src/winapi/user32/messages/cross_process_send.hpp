// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file cross_process_send.hpp
/// @brief SendMessage/SendMessageTimeout to a window owned by another guest
/// process in the capsule.
///
/// Learn (nf-winuser-sendmessagew): "The system only does marshaling for
/// system messages (those in the range 0 to (WM_USER-1)). To send other
/// messages (those >= WM_USER) to another process, you must do custom
/// marshaling." So a message >= WM_USER travels with its raw wParam/lParam; a
/// system message travels raw only when neither parameter is a pointer, and
/// the pointer-bearing ones Vitreo marshals are listed in classify(). Any
/// other system message fails loudly (UNIMPLEMENTED) instead of handing the
/// peer a pointer into this process.

#include <vitreo/common.hpp>
#include "winapi/kernel32/memory/remote_memory.hpp"

#include <span>
#include <vector>

namespace vitreo { class GuestMemory; }

namespace vitreo::winapi {
class Marshaler;
class MessageSendManager;
class HeapManager;
}

namespace vitreo::winapi::xproc_send {

enum class Kind : u8 {
    Raw,          ///< no pointer parameters
    StringIn,     ///< lParam = LPCTSTR (may be NULL): WM_SETTEXT, WM_SETTINGCHANGE, WM_DEVMODECHANGE
    TextOut,      ///< WM_GETTEXT: wParam = buffer size in TCHARs, lParam = buffer
    CopyData,     ///< WM_COPYDATA: lParam = COPYDATASTRUCT*
    Unsupported,
};

[[nodiscard]] Kind classify(u32 msg);

struct Marshaled {
    std::vector<Byte> payload;
    u32 out_capacity = 0;
};

/// Sender: capture the pointed-to data. False (last error set) when the
/// guest pointer is unreadable.
[[nodiscard]] bool marshal(Marshaler& m, bool ansi, u32 msg, u64 wparam,
                           u64 lparam, Marshaled& out);

/// Sender: copy WM_GETTEXT results back into the caller's buffer; `lresult`
/// becomes the caller-encoding character count.
void unmarshal_out(Marshaler& m, bool ansi, u32 msg, u64 wparam, u64 lparam,
                   std::span<const Byte> out, u64& lresult);

/// Receiver: rebuild the pointed-to data in this process's guest memory,
/// deliver through `msm` with the SMTO flags/timeout, and capture output.
[[nodiscard]] ipc::RemoteSendStatus deliver(
    MessageSendManager& msm, GuestMemory& memory, HeapManager& heap,
    bool is64, u32 hwnd, u32 msg, u64 wparam, u64 lparam, u32 flags,
    u32 timeout_ms, std::span<const Byte> payload, u32 out_capacity,
    u64& lresult, std::vector<Byte>& out);

} // namespace vitreo::winapi::xproc_send
