/// @file address_notification.cpp
/// @brief NotifyAddrChange / CancelIPChangeNotify over the host route socket.

#include "iphlpapi_internal.hpp"
#include "iphlpapi_state.hpp"

namespace vitreo::winapi {

void IphlpapiManager::impl_NotifyAddrChange(Marshaler& m) {
    auto& state = *m_notifications;
    const GuestAddr out = m.arg_ptr(0), ov = m.arg_ptr(1);
    const bool wide = m.cpu_is_64bit();
    const bool asynchronous = ov != 0;
    if ((out && !m.memory().guest_can_write(out, wide ? 8 : 4)) ||
        (ov && !m.memory().guest_can_write(ov, wide ? 32 : 20))) {
        m.set_return(ERROR_INVALID_PARAMETER);
        return;
    }
    if (!state.handles || !state.sync) {
        static std::once_flag once;
        std::call_once(once, [] {
            std::fprintf(stderr, "[vitreo] UNIMPLEMENTED iphlpapi!NotifyAddrChange: notification handle/sync managers are not configured\n");
        });
        winapi::set_last_error(ERROR_NOT_SUPPORTED);
        m.set_return(ERROR_NOT_SUPPORTED);
        return;
    }
    std::unique_lock lock(state.mutex);
    state.memory = &m.memory();
    if (!state.start()) {
        static std::once_flag once;
        std::call_once(once, [] {
            std::fprintf(stderr, "[vitreo] UNIMPLEMENTED iphlpapi!NotifyAddrChange: native IPv4 route notification backend unavailable\n");
        });
        winapi::set_last_error(ERROR_NOT_SUPPORTED);
        m.set_return(ERROR_NOT_SUPPORTED);
        return;
    }
    // Do not deliver a change predating this registration.
    const auto current = NotificationState::snapshot();
    if (!current) { m.set_return(ERROR_NOT_SUPPORTED); return; }
    if (*current != state.addresses) {
        for (auto& previous : state.pending) state.finish(previous, 0);
        state.pending.clear();
    }
    state.addresses = *current;
    if (!state.handle) {
        state.handle = state.handles->allocate(HandleType::NetworkNotification,
            static_cast<OverlappedNotificationFile*>(&state), HandleFlags::ProtectFromClose);
        if (!state.handle) { m.set_return(error_codes::ERROR_NOT_ENOUGH_MEMORY); return; }
    }
    if (out) {
        if (wide) (void)m.memory().write_u64(out, state.handle);
        else (void)m.memory().write_u32(out, state.handle);
    }
    if (asynchronous) {
        if (wide) (void)m.memory().write_u64(ov, kStatusNotificationPending);
        else (void)m.memory().write_u32(ov, kStatusNotificationPending);
        const u64 event = wide ? m.memory().read_u64(ov + 24).value_or(0)
                               : m.memory().read_u32(ov + 16).value_or(0);
        if (event && !state.sync->reset_event(static_cast<Handle>(event & ~u64{1}))) {
            winapi::set_last_error(error_codes::ERROR_INVALID_HANDLE);
            m.set_return(error_codes::ERROR_INVALID_HANDLE);
            return;
        }
    }
    std::shared_ptr<NotificationState::Pending> request;
    if (asynchronous) {
        const auto found = std::find_if(state.pending.begin(), state.pending.end(),
            [ov](const auto& p) { return p->overlapped == ov; });
        if (found != state.pending.end()) request = *found;
    }
    if (!request) {
        request = std::make_shared<NotificationState::Pending>();
        request->overlapped = ov;
        request->wide = wide;
        state.pending.push_back(request);
    }
    if (asynchronous) {
        // Windows preserves InternalHigh until completion.
        winapi::set_last_error(error_codes::ERROR_IO_PENDING);
        m.set_return(error_codes::ERROR_IO_PENDING);
        return;
    }
    state.changed.wait(lock, [&] { return request->done; });
    const u32 result = request->status ? error_codes::ERROR_CANCELLED : NO_ERROR;
    winapi::set_last_error(result);
    m.set_return(result);
}
void IphlpapiManager::impl_CancelIPChangeNotify(Marshaler& m) {
    auto& state = *m_notifications;
    const GuestAddr ov = m.arg_ptr(0);
    std::lock_guard lock(state.mutex);
    bool found = false;
    for (auto& request : state.pending) {
        if (ov && request->overlapped == ov) {
            state.finish(request, kStatusNotificationCancelled);
            found = true;
        }
    }
    std::erase_if(state.pending, [](const auto& p) { return p->done; });
    winapi::set_last_error(NO_ERROR);
    m.set_return(found ? 1 : 0);
}

} // namespace vitreo::winapi
