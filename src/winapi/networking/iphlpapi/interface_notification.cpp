/// @file interface_notification.cpp
/// @brief NotifyIpInterfaceChange / CancelMibChangeNotify2 over the host route
///        socket.
///
/// Contract: Microsoft Learn (netioapi.h) plus
/// tools/oracle_probe/netio_notify_route_probe (x86 = x64):
///   - Family must be AF_UNSPEC, AF_INET or AF_INET6, else
///     ERROR_INVALID_PARAMETER with *NotificationHandle = NULL.
///   - InitialNotification calls the callback SYNCHRONOUSLY on the registering
///     thread, before the function returns, once per family (twice for
///     AF_UNSPEC), with Row = NULL and MibInitialNotification.
///   - A NULL Callback is accepted and never called. A NULL NotificationHandle
///     pointer is not validated (Windows faults writing through it).
///   - Later changes call back on a system thread, serialized per
///     registration, with a Row holding only Family, InterfaceLuid and
///     InterfaceIndex.
///   - CancelMibChangeNotify2(NULL) is ERROR_INVALID_HANDLE (Learn says
///     ERROR_INVALID_PARAMETER; Windows returns 6). Cancel waits for a callback
///     in progress, which is why Learn forbids calling it from the callback.
///
/// An IP interface is an (address family, interface) binding; it exists while
/// the host interface carries an address of that family. A change to its
/// operational state, MTU or addresses is MibParameterNotification.

#include "iphlpapi_internal.hpp"
#include "iphlpapi_state.hpp"

#include <deque>
#include <map>
#include <tuple>

namespace vitreo::winapi {

namespace {

constexpr u32 kMibParameterNotification = 0;
constexpr u32 kMibAddInstance = 1;
constexpr u32 kMibDeleteInstance = 2;
constexpr u32 kMibInitialNotification = 3;
constexpr u32 kAfUnspec = 0;
constexpr usize kIpInterfaceRowSize = 168;   // sizeof(MIB_IPINTERFACE_ROW), x86 = x64

struct IpInterfaceKey {
    u16 family;
    u32 index;
    auto operator<=>(const IpInterfaceKey&) const = default;
};

struct IpInterfaceState {
    u64 luid = 0;
    u32 oper_status = 0;
    u32 mtu = 0;
    std::vector<iphlp::NetAddress> addresses;
    bool operator==(const IpInterfaceState&) const = default;
};

using IpInterfaceTable = std::map<IpInterfaceKey, IpInterfaceState>;

IpInterfaceTable snapshot_ip_interfaces() {
    IpInterfaceTable table;
    const auto snapshot =
        iphlp::snapshot_host_interfaces(iphlp::SnapshotDetail::Identity);
    for (const auto& iface : snapshot.interfaces) {
        for (const auto& unicast : iface.unicast) {
            auto& state = table[IpInterfaceKey{unicast.address.family, iface.index}];
            state.luid = iface.luid;
            state.oper_status = iface.oper_status;
            state.mtu = iface.ip_mtu;
            state.addresses.push_back(unicast.address);
        }
    }
    for (auto& [key, state] : table) {
        std::sort(state.addresses.begin(), state.addresses.end(),
                  [](const auto& a, const auto& b) {
                      return std::tie(a.family, a.bytes, a.scope_id) <
                             std::tie(b.family, b.bytes, b.scope_id);
                  });
    }
    return table;
}

}  // namespace

struct InterfaceChangeState {
    struct Event {
        IpInterfaceKey key;
        u64 luid;
        u32 type;
    };
    struct Registration {
        Handle handle = NULL_HANDLE;
        u16 family = 0;
        GuestAddr callback = 0;
        GuestAddr context = 0;
        bool wide = true;
        std::mutex mutex;
        std::condition_variable wake;
        std::deque<Event> queue;
        bool cancelled = false;
        std::thread worker;
    };

    std::mutex mutex;
    std::unordered_map<Handle, std::shared_ptr<Registration>> registrations;
    IpInterfaceTable interfaces;
    int route_fd = -1;
    std::jthread watcher;

    /// Start the route-socket watcher once. Called with @ref mutex held.
    bool start() {
#ifdef __APPLE__
        if (route_fd >= 0) return true;
        route_fd = ::socket(PF_ROUTE, SOCK_RAW, AF_UNSPEC);
        if (route_fd < 0) return false;
        interfaces = snapshot_ip_interfaces();
        watcher = std::jthread([this](std::stop_token stop) { watch(stop); });
        return true;
#else
        return false;
#endif
    }

    void watch(const std::stop_token& stop) {
        while (!stop.stop_requested()) {
            pollfd descriptor{route_fd, POLLIN, 0};
            const int ready = ::poll(&descriptor, 1, 100);
            if (ready <= 0) continue;
            std::array<u8, 8192> message;
            if (::read(route_fd, message.data(), message.size()) <= 0) continue;
            // A configuration change arrives as a burst of routing messages;
            // let it settle so one change is one notification.
            for (;;) {
                pollfd more{route_fd, POLLIN, 0};
                if (::poll(&more, 1, 50) <= 0) break;
                (void)::read(route_fd, message.data(), message.size());
            }
            publish(snapshot_ip_interfaces());
        }
    }

    void publish(IpInterfaceTable current) {
        std::vector<Event> events;
        std::lock_guard lock(mutex);
        for (const auto& [key, state] : current) {
            const auto previous = interfaces.find(key);
            if (previous == interfaces.end()) {
                events.push_back({key, state.luid, kMibAddInstance});
            } else if (!(previous->second == state)) {
                events.push_back({key, state.luid, kMibParameterNotification});
            }
        }
        for (const auto& [key, state] : interfaces) {
            if (!current.contains(key)) {
                events.push_back({key, state.luid, kMibDeleteInstance});
            }
        }
        interfaces = std::move(current);
        if (events.empty()) return;
        for (auto& [handle, registration] : registrations) {
            std::lock_guard registration_lock(registration->mutex);
            for (const Event& event : events) {
                if (registration->family == kAfUnspec ||
                    registration->family == event.key.family) {
                    registration->queue.push_back(event);
                }
            }
            registration->wake.notify_all();
        }
    }

    ~InterfaceChangeState() {
        watcher.request_stop();
        if (watcher.joinable()) watcher.join();
        std::vector<std::shared_ptr<Registration>> remaining;
        {
            std::lock_guard lock(mutex);
            for (auto& [handle, registration] : registrations) {
                remaining.push_back(registration);
            }
            registrations.clear();
        }
        for (auto& registration : remaining) {
            {
                std::lock_guard registration_lock(registration->mutex);
                registration->cancelled = true;
            }
            registration->wake.notify_all();
            if (registration->worker.joinable()) registration->worker.join();
        }
        if (route_fd >= 0) ::close(route_fd);
    }
};

void InterfaceChangeStateDeleter::operator()(InterfaceChangeState* state) const {
    delete state;
}

std::unique_ptr<InterfaceChangeState, InterfaceChangeStateDeleter>
make_interface_change_state() {
    return std::unique_ptr<InterfaceChangeState, InterfaceChangeStateDeleter>(
        new InterfaceChangeState());
}

void IphlpapiManager::set_guest_callbacks(GuestCall guest_call,
                                          ThreadManager* threads) {
    m_guest_call = std::move(guest_call);
    m_threads = threads;
}

namespace {

/// Delivers one registration's change notifications, serialized, each on a new
/// guest thread as Windows uses a system worker thread.
void run_interface_change_worker(
        const std::shared_ptr<InterfaceChangeState::Registration>& registration,
        ThreadManager* threads, HeapManager* heap, GuestMemory* memory,
        SyncManager* sync) {
    // A stable tid that is never a real guest thread, as RegisterWait's
    // waiter thread uses, so sync waits are not attributed to the UI thread.
    const u32 service_tid = 0xF1000000u | (registration->handle & 0x00FFFFFFu);
    ThreadManager::set_current_thread_id(service_tid);
    ThreadManager::ServiceThreadScope service_thread(threads, service_tid);
    for (;;) {
        InterfaceChangeState::Event event{};
        {
            std::unique_lock lock(registration->mutex);
            registration->wake.wait(lock, [&] {
                return registration->cancelled || !registration->queue.empty();
            });
            if (registration->cancelled) return;
            event = registration->queue.front();
            registration->queue.pop_front();
        }
        if (registration->callback == 0 || threads == nullptr || heap == nullptr ||
            memory == nullptr || sync == nullptr) {
            continue;
        }
        const u64 row = heap->heap_alloc(heap->get_process_heap(), 0x8 /*HEAP_ZERO_MEMORY*/,
                                         kIpInterfaceRowSize);
        if (row == 0) continue;
        (void)memory->write_u16(row + 0, event.key.family);
        (void)memory->write_u64(row + 8, event.luid);
        (void)memory->write_u32(row + 16, event.key.index);
        u32 tid = 0;
        const Handle thread = threads->create_thread(
            0, registration->callback, registration->context, 0, &tid, row);
        if (thread != INVALID_HANDLE && thread != 0) {
            if (auto* guest_thread = threads->get_thread(tid)) guest_thread->launched = true;
            threads->run_async(tid, registration->callback, registration->context,
                               row, event.type, 3);
            // Serialized: the next notification waits for this callback.
            (void)sync->wait_for_single_object(thread, INFINITE_WAIT);
            threads->close_thread_handle(thread);
        }
        (void)heap->heap_free(heap->get_process_heap(), 0, row);
    }
}

}  // namespace

void IphlpapiManager::impl_NotifyIpInterfaceChange(Marshaler& m) {
    // NETIO_STATUS NotifyIpInterfaceChange(ADDRESS_FAMILY Family,
    //     PIPINTERFACE_CHANGE_CALLBACK Callback, PVOID CallerContext,
    //     BOOLEAN InitialNotification, HANDLE* NotificationHandle)
    const u16 family = static_cast<u16>(m.arg(0));
    const GuestAddr callback = m.arg_ptr(1);
    const GuestAddr context = m.arg_ptr(2);
    const bool initial = (m.arg(3) & 0xFFu) != 0;
    const GuestAddr handle_out = m.arg_ptr(4);
    const bool wide = m.cpu_is_64bit();
    const auto write_handle = [&](u64 value) {
        if (handle_out == 0) return;
        if (wide) (void)m.memory().write_u64(handle_out, value);
        else (void)m.memory().write_u32(handle_out, static_cast<u32>(value));
    };
    if (family != kAfUnspec && family != iphlp::kGuestAfInet &&
        family != iphlp::kGuestAfInet6) {
        write_handle(0);
        m.set_return(ERROR_INVALID_PARAMETER);
        return;
    }
    auto& notifications = *m_notifications;
    if (!notifications.handles || !notifications.sync || !m_threads || !m_guest_call) {
        static std::once_flag once;
        std::call_once(once, [] {
            std::fprintf(stderr, "[vitreo] UNIMPLEMENTED iphlpapi!NotifyIpInterfaceChange: "
                                 "guest callback or handle managers are not configured\n");
        });
        write_handle(0);
        m.set_return(ERROR_NOT_SUPPORTED);
        return;
    }
    {
        std::lock_guard lock(notifications.mutex);
        notifications.memory = &m.memory();
    }
    auto& state = *m_interface_changes;
    auto registration = std::make_shared<InterfaceChangeState::Registration>();
    registration->family = family;
    registration->callback = callback;
    registration->context = context;
    registration->wide = wide;
    {
        std::lock_guard lock(state.mutex);
        if (!state.start()) {
            static std::once_flag once;
            std::call_once(once, [] {
                std::fprintf(stderr, "[vitreo] UNIMPLEMENTED iphlpapi!NotifyIpInterfaceChange: "
                                     "host route notification backend unavailable\n");
            });
            write_handle(0);
            m.set_return(ERROR_NOT_SUPPORTED);
            return;
        }
        registration->handle = notifications.handles->allocate(
            HandleType::NetworkNotification, nullptr, HandleFlags::ProtectFromClose);
        if (registration->handle == NULL_HANDLE) {
            write_handle(0);
            m.set_return(error_codes::ERROR_NOT_ENOUGH_MEMORY);
            return;
        }
        state.registrations[registration->handle] = registration;
    }
    registration->worker = std::thread(
        [registration, threads = m_threads, heap = m_heap, memory = &m.memory(),
         sync = notifications.sync] {
            run_interface_change_worker(registration, threads, heap, memory, sync);
        });
    write_handle(registration->handle);

    if (initial && callback != 0) {
        const auto notify_initial = [&] {
            const u64 args[3] = {context, 0, kMibInitialNotification};
            (void)m_guest_call(callback, args, 3, wide);
        };
        if (family == kAfUnspec) {
            notify_initial();   // IPv4
            notify_initial();   // IPv6
        } else {
            notify_initial();
        }
    }
    m.set_return(NO_ERROR);
}

void IphlpapiManager::impl_CancelMibChangeNotify2(Marshaler& m) {
    // NETIO_STATUS CancelMibChangeNotify2(HANDLE NotificationHandle)
    const Handle handle = static_cast<Handle>(m.arg(0));
    std::shared_ptr<InterfaceChangeState::Registration> registration;
    {
        auto& state = *m_interface_changes;
        std::lock_guard lock(state.mutex);
        const auto found = state.registrations.find(handle);
        if (handle != NULL_HANDLE && found != state.registrations.end()) {
            registration = found->second;
            state.registrations.erase(found);
        }
    }
    if (!registration) {
        m.set_return(error_codes::ERROR_INVALID_HANDLE);
        return;
    }
    {
        std::lock_guard lock(registration->mutex);
        registration->cancelled = true;
        registration->queue.clear();
    }
    registration->wake.notify_all();
    if (registration->worker.joinable()) registration->worker.join();
    auto& notifications = *m_notifications;
    if (notifications.handles) {
        (void)notifications.handles->set_flags(handle, HandleFlags::ProtectFromClose, 0);
        (void)notifications.handles->close(handle);
    }
    m.set_return(NO_ERROR);
}

} // namespace vitreo::winapi
