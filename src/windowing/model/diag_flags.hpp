#pragma once
/// @file diag_flags.hpp
/// @brief Opt-in windowing/input diagnostic switches, read from the environment once.
///
/// Each switch is process-immutable: the environment variable is read on first
/// use and the answer is cached for the life of the process. Presence of the
/// variable (any value, including empty) enables the diagnostic, exactly as the
/// scattered `std::getenv(NAME) != nullptr` checks this replaces did. Function-
/// local statics keep the read independent of static-initialization order, and
/// the accessors are cheap enough for per-message hot paths.

#include <cstdlib>

#include <unistd.h>

namespace vitreo::windowing::diag {

/// VITREO_INPUT_DIAG: trace native input translation, queueing and dispatch.
/// The marker file /tmp/vitreo-input-diag enables it too: a guest launched
/// by a store client is several re-execs from the shell and does not
/// reliably inherit the environment (same reason as /tmp/vitreo-d3d11-diag).
[[nodiscard]] inline bool input_diag() noexcept {
    static const bool v = (std::getenv("VITREO_INPUT_DIAG") != nullptr) ||
                          ::access("/tmp/vitreo-input-diag", F_OK) == 0;
    return v;
}

/// VITREO_FOCUS_DIAG: trace key/active window and focus transitions.
[[nodiscard]] inline bool focus_diag() noexcept {
    static const bool v = (std::getenv("VITREO_FOCUS_DIAG") != nullptr);
    return v;
}

/// VITREO_WIN_DIAG: trace window and control lifecycle and notifications.
[[nodiscard]] inline bool win_diag() noexcept {
    static const bool v = (std::getenv("VITREO_WIN_DIAG") != nullptr);
    return v;
}

/// VITREO_WINDOW_DIAG: trace SetWinEventHook registration and WinEvent delivery.
[[nodiscard]] inline bool window_diag() noexcept {
    static const bool v = (std::getenv("VITREO_WINDOW_DIAG") != nullptr);
    return v;
}

/// VITREO_MSG_PUMP_DIAG: trace MsgWaitForMultipleObjectsEx waits and pump wakeups.
[[nodiscard]] inline bool msg_pump_diag() noexcept {
    static const bool v = (std::getenv("VITREO_MSG_PUMP_DIAG") != nullptr);
    return v;
}

/// VITREO_ENABLE_DIAG_LOGS: enable the verbose button/window-show bridge logs.
[[nodiscard]] inline bool diag_logs() noexcept {
    static const bool v = (std::getenv("VITREO_ENABLE_DIAG_LOGS") != nullptr);
    return v;
}

}  // namespace vitreo::windowing::diag
