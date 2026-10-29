#pragma once
/// @file iphlpapi_state.hpp
/// @brief The private state types of IphlpapiManager (notification, ICMP,
///        compartment), defined as the nested members iphlpapi.hpp declares.
///
/// Included by iphlpapi.cpp (constructor/destructor need the complete types),
/// address_notification.cpp and icmp.cpp.

#include "iphlpapi_internal.hpp"

#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace vitreo::winapi {

// IP Helper notification ownership is process-local. The route socket observes
// host changes; only a change in the IPv4 address/interface table completes
// requests. All writes happen before signaling the caller's event.
struct IphlpapiManager::NotificationState final : OverlappedNotificationFile {
    struct Pending { GuestAddr overlapped; bool wide; bool done = false; u32 status = 0; };
    HandleTable* handles = nullptr;
    SyncManager* sync = nullptr;
    GuestMemory* memory = nullptr;
    Handle handle = NULL_HANDLE;
    std::mutex mutex;
    std::condition_variable changed;
    std::vector<std::shared_ptr<Pending>> pending;
    std::vector<std::array<u32, 3>> addresses;
    int route_fd = -1;
    bool backend_failed = false;
    std::jthread watcher;

    static std::optional<std::vector<std::array<u32, 3>>> snapshot() {
        std::vector<std::array<u32, 3>> result;
        ifaddrs* entries = nullptr;
        if (::getifaddrs(&entries) != 0) return std::nullopt;
        for (auto* p = entries; p; p = p->ifa_next) {
            if (!p->ifa_addr || p->ifa_addr->sa_family != AF_INET) continue;
            const auto* address = reinterpret_cast<const sockaddr_in*>(p->ifa_addr);
            const auto* mask = reinterpret_cast<const sockaddr_in*>(p->ifa_netmask);
            result.push_back({::if_nametoindex(p->ifa_name), address->sin_addr.s_addr,
                              mask ? mask->sin_addr.s_addr : 0});
        }
        ::freeifaddrs(entries);
        std::sort(result.begin(), result.end());
        return result;
    }
    void finish(const std::shared_ptr<Pending>& request, u32 status) {
        if (request->done) return;
        request->status = status;
        if (request->overlapped && memory) {
            const GuestAddr ov = request->overlapped;
            const auto event = request->wide ? memory->read_u64(ov + 24).value_or(0)
                                             : memory->read_u32(ov + 16).value_or(0);
            if (request->wide) {
                (void)memory->write_u64(ov + 8, 0);
                (void)memory->write_u64(ov, status);
            } else {
                (void)memory->write_u32(ov + 4, 0);
                (void)memory->write_u32(ov, status);
            }
            if (event && sync) (void)sync->set_event(static_cast<Handle>(event & ~u64{1}));
        }
        request->done = true;
        changed.notify_all();
    }
    bool start() {
#ifdef __APPLE__
        if (backend_failed) return false;
        if (route_fd >= 0) return true;
        route_fd = ::socket(PF_ROUTE, SOCK_RAW, AF_INET);
        if (route_fd < 0) return false;
        const auto initial = snapshot();
        if (!initial) { ::close(route_fd); route_fd = -1; return false; }
        addresses = *initial;
        watcher = std::jthread([this](std::stop_token stop) {
            while (!stop.stop_requested()) {
                pollfd descriptor{route_fd, POLLIN, 0};
                const int ready = ::poll(&descriptor, 1, 100);
                if (ready < 0 && errno == EINTR) continue;
                if (ready < 0 || (ready > 0 && (descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)))) {
                    std::lock_guard lock(mutex);
                    backend_failed = true;
                    for (auto& request : pending) finish(request, kStatusNotificationCancelled);
                    pending.clear();
                    return;
                }
                if (ready == 0) continue;
                std::array<u8, 8192> message;
                if (::read(route_fd, message.data(), message.size()) <= 0) continue;
                std::lock_guard lock(mutex);
                const auto current = snapshot();
                if (!current || *current == addresses) continue;
                addresses = *current;
                for (auto& request : pending) finish(request, 0);
                pending.clear();
            }
        });
        return true;
#else
        return false;
#endif
    }
    void wait_for_completion(GuestAddr overlapped) override {
        std::unique_lock lock(mutex);
        changed.wait(lock, [&] {
            return std::none_of(pending.begin(), pending.end(), [&](const auto& p) {
                return p->overlapped == overlapped && !p->done;
            });
        });
    }
    ~NotificationState() override {
        watcher.request_stop();
        if (watcher.joinable()) watcher.join();
        std::lock_guard lock(mutex);
        for (auto& request : pending) finish(request, kStatusNotificationCancelled);
        pending.clear();
        if (route_fd >= 0) ::close(route_fd);
        // The provider, not the caller, owns the shared notification file.
        if (handle && handles) {
            (void)handles->set_flags(handle, HandleFlags::ProtectFromClose, 0);
            (void)handles->close(handle);
        }
    }
};

struct IphlpapiManager::IcmpState {
    struct Open {
        int socket = -1;
        std::mutex mutex;
        std::condition_variable idle;
        u32 active = 0;
        ~Open() { if (socket >= 0) ::close(socket); }
    };
    std::mutex mutex;
    u64 next = 0x40000000u;
    std::unordered_map<u64, std::shared_ptr<Open>> handles;
};

// Capsule networking currently owns one real compartment: the default host
// namespace. State belongs to guest thread IDs, never the dispatching host TLS.
struct IphlpapiManager::CompartmentState {
    std::mutex mutex;
    std::unordered_map<u32, u32> thread_ids;
};

} // namespace vitreo::winapi
