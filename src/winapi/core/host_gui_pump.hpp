// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file host_gui_pump.hpp
/// @brief Process-wide throttled AppKit event pump for the UI thread.
///
/// The callback keeps native AppKit state responsive and translates native
/// input into queued Win32 messages. It must never retrieve or dispatch those
/// messages; the guest owns that part of the Win32 message-loop contract.

#include <functional>

namespace vitreo::winapi {

using HostGuiPump = std::function<void()>;

/// Install the native event pump (typically Cocoa pump and Win32 enqueue).
void set_host_gui_pump(HostGuiPump pump);

/// Clear the pump (tests / shutdown).
void clear_host_gui_pump();

/// On the main thread, invoke the pump at most once per ~16 ms.
void pump_host_gui();

/// True while `pump_host_gui` is running on this thread.
///
/// Cocoa must not `SendMessage` / run guest wndprocs from that path (including
/// sync `WM_NCHITTEST` for guest-frame caption drag). Host pumps only enqueue
/// translated AppKit input; the guest message loop owns dispatch.
[[nodiscard]] bool in_host_gui_pump() noexcept;

} // namespace vitreo::winapi
