/// @file wait_for_multiple_events.cpp
/// @brief Ws2Manager::wait_for_multiple_events
///
/// Closed method cut from ws2_32.cpp.

#include "ws2_32.hpp"
#include "winapi/networking/ws2_32/ws2_32_internal.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/kernel32/thread/thread.hpp"
#include "winapi/core/marshaler.hpp"
#include "ipc/ipc_gate.hpp"
#include "core/memory/guest_memory.hpp"

#include <algorithm>
#include <chrono>
#include <span>
#include <vector>

namespace vitreo::winapi {

u32 Ws2Manager::wait_for_multiple_events(
    std::span<const Handle> handles, bool wait_all, u32 milliseconds,
    bool alertable) {
    if (handles.empty() || handles.size() > 64 || m_sync_manager == nullptr) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        return WAIT_FAILED;
    }

    const bool infinite = milliseconds == INFINITE_WAIT;
    const auto deadline =
        infinite
            ? std::chrono::steady_clock::time_point::max()
            : std::chrono::steady_clock::now() +
                  std::chrono::milliseconds(milliseconds);

    // Learn WSAWaitForMultipleEvents: fAlertable TRUE performs an alertable
    // wait; pending user APCs (QueueUserAPC / WSA completion routines) end the
    // wait with WSA_WAIT_IO_COMPLETION (= WAIT_IO_COMPLETION).
    if (alertable && m_sync_manager->try_drain_alertable_apcs()) {
        return WAIT_IO_COMPLETION;
    }

    for (;;) {
        if (alertable && m_sync_manager->try_drain_alertable_apcs()) {
            return WAIT_IO_COMPLETION;
        }

        const bool has_socket_events = refresh_socket_events(handles);

        u32 wait_slice = milliseconds;
        if (has_socket_events || alertable) {
            constexpr u32 kPollSliceMs = 10;
            if (infinite) {
                wait_slice = kPollSliceMs;
            } else {
                const auto now = std::chrono::steady_clock::now();
                if (now >= deadline) {
                    wait_slice = 0;
                } else {
                    const auto remaining =
                        std::chrono::duration_cast<std::chrono::milliseconds>(
                            deadline - now);
                    const u32 remaining_ms =
                        static_cast<u32>(std::max<i64>(1, remaining.count()));
                    wait_slice = std::min(remaining_ms, kPollSliceMs);
                }
            }
        }

        u64 apc_ev = 0;
        if (alertable && !wait_all) {
            apc_ev = m_sync_manager->apc_wait_object_for_current_thread();
        }

        const u32 result = m_sync_manager->wait_for_multiple_objects(
            static_cast<u32>(handles.size()), handles.data(), wait_all,
            wait_slice, apc_ev);
        if (apc_ev != 0 &&
            result == WAIT_OBJECT_0 + static_cast<u32>(handles.size())) {
            // Woken by APC queue — drain and report IO_COMPLETION.
            if (m_sync_manager->try_drain_alertable_apcs()) {
                return WAIT_IO_COMPLETION;
            }
            continue;
        }
        if (result != WAIT_TIMEOUT) {
            if (result == WAIT_FAILED) {
                set_last_error(get_last_error());
            }
            return result;
        }
        if (alertable && m_sync_manager->try_drain_alertable_apcs()) {
            return WAIT_IO_COMPLETION;
        }
        if ((!has_socket_events && !alertable) || milliseconds == 0) {
            return WAIT_TIMEOUT;
        }
        if (!infinite && std::chrono::steady_clock::now() >= deadline) {
            return WAIT_TIMEOUT;
        }
    }
}

} // namespace vitreo::winapi
