// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file message_send_internal.hpp
/// @brief Private declarations shared by the message_send*.cpp,
///        send_message_*.cpp and control_*.cpp translation units.
///
/// Not a public interface: only the translation units under
/// winapi/user32/messages/ that implement SendMessage/PostMessage and the
/// native-control handlers they dispatch to may include it.

#include "winapi/user32/messages/message_send.hpp"

#include <cstdlib>
#include <string>

namespace vitreo::winapi {

static inline bool message_send_diag_logs_enabled() noexcept {
    static const bool enabled = (std::getenv("VITREO_ENABLE_DIAG_LOGS") != nullptr);
    return enabled;
}

// message_send.cpp
std::wstring message_send_utf8_to_wstring(const std::string& s);

// send_message_peer.cpp
bool send_to_peer(Marshaler& m, bool ansi, u32 owner_pid,
                  windowing::HWND hwnd, u32 msg, u64 wParam, u64 lParam,
                  u32 flags, u32 timeout, u64& lresult);
u32 peer_owner_of(windowing::HWND hwnd);
bool send_message_to_peer_window(Marshaler& m, MessageSendManager& msm,
                                 bool ansi);

// send_message_timeout.cpp
void impl_SendMessageTimeoutA(Marshaler& m, MessageSendManager& msm);

// control_treeview.cpp
bool try_handle_treeview(Marshaler& m, MessageSendManager& msm,
                         windowing::HWND hwnd, u32 msg, u64 wParam, u64 lParam);

// control_combolist.cpp
bool try_handle_combolist(Marshaler& m, MessageSendManager& msm,
                          windowing::HWND hwnd, u32 msg, u64 wParam, u64 lParam,
                          bool ansi, bool force_native = false);

// control_bars.cpp
bool try_handle_toolbar(Marshaler& m, MessageSendManager& msm,
                        windowing::HWND hwnd, u32 msg, u64 wParam, u64 lParam);
bool try_handle_statusbar(Marshaler& m, MessageSendManager& msm,
                          windowing::HWND hwnd, u32 msg, u64 wParam, u64 lParam);

// control_richedit.cpp
bool try_handle_richedit_settextex(Marshaler& m, MessageSendManager& msm,
                                   windowing::HWND hwnd, u32 msg,
                                   u64 wParam, u64 lParam);
bool try_handle_richedit_streamin(Marshaler& m, MessageSendManager& msm,
                                  windowing::HWND hwnd, u32 msg,
                                  u64 wParam, u64 lParam,
                                  bool force_native = false);
bool try_handle_edit_setsel(Marshaler& m, MessageSendManager& msm,
                            windowing::HWND hwnd, u32 msg,
                            u64 wParam, u64 lParam);
bool try_handle_richedit_scrollpos(Marshaler& m, MessageSendManager& msm,
                                   windowing::HWND hwnd, u32 msg,
                                   u64 wParam, u64 lParam);

// ComboBox/ListBox message codes shared by control_combolist.cpp and the
// inline ANSI switch in send_message_thunks.cpp.
// ComboBox messages
constexpr u32 CB_GETCOUNT       = 0x0146;
constexpr u32 CB_GETCURSEL      = 0x0147;
constexpr u32 CB_GETLBTEXT      = 0x0148;
constexpr u32 CB_GETLBTEXTLEN   = 0x0149;
constexpr u32 CB_INSERTSTRING   = 0x014A;
constexpr u32 CB_RESETCONTENT   = 0x014B;
constexpr u32 CB_ADDSTRING      = 0x0143;
constexpr u32 CB_SETCURSEL      = 0x014E;
constexpr u32 CB_GETITEMDATA    = 0x0150;
constexpr u32 CB_SETITEMDATA    = 0x0151;

// ListBox messages
constexpr u32 LB_ADDSTRING      = 0x0180;
constexpr u32 LB_INSERTSTRING   = 0x0181;
constexpr u32 LB_DELETESTRING   = 0x0182;
constexpr u32 LB_RESETCONTENT   = 0x0184;
constexpr u32 LB_SETCURSEL      = 0x0186;
constexpr u32 LB_GETCURSEL      = 0x0188;
constexpr u32 LB_GETTEXT        = 0x0189;
constexpr u32 LB_GETTEXTLEN     = 0x018A;
constexpr u32 LB_GETCOUNT       = 0x018B;
constexpr u32 LB_GETITEMDATA    = 0x0199;
constexpr u32 LB_SETITEMDATA    = 0x019A;

constexpr i32 CB_ERR = -1;
constexpr i32 CB_OKAY = 0;

} // namespace vitreo::winapi
