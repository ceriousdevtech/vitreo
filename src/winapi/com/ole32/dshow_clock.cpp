// SPDX-License-Identifier: LGPL-2.1-only
/// @file dshow_clock.cpp
/// @brief CLSID_SystemClock: IReferenceClock over the timeGetTime time base
/// (measured: the stock clock's GetTime equals timeGetTime() * 10000 within a
/// millisecond), with AdviseTime / AdvisePeriodic / Unadvise as measured in
/// tools/oracle_probe/dshow_asyncreader_probe{32,64}.expected.

#include "winapi/com/ole32/dshow_host.hpp"
#include "winapi/kernel32/synchronization/sync.hpp"

#include <mach/mach_time.h>

#include <chrono>
#include <cstring>
#include <thread>

namespace vitreo::winapi::dshow {

namespace {

namespace sdk {
#include "winapi/com/ole32/dshow_guids.inc"
}

constexpr u32 S_OK_ = 0;
constexpr u32 S_FALSE_ = 1;
constexpr u32 E_POINTER_ = 0x80004003u;
constexpr u32 E_INVALIDARG_ = 0x80070057u;
constexpr u32 E_NOTIMPL_ = 0x80004001u;

class SystemClock final : public HostObject {
public:
    struct Advise {
        u64 cookie;
        i64 due;
        i64 period;  // 0 = one shot (event), else periodic (semaphore)
        u32 handle;
    };

    std::mutex mutex;
    std::condition_variable cv;
    std::vector<Advise> advises;
    u64 next_cookie{1};
    bool stop{false};
    std::thread worker;

    ~SystemClock() override { shutdown(); }

    void shutdown() {
        {
            std::lock_guard<std::mutex> lock(mutex);
            stop = true;
        }
        cv.notify_all();
        if (worker.joinable() && worker.get_id() != std::this_thread::get_id()) worker.join();
    }

    bool interface_for(const Guid& iid, Iface& out) const override {
        if (guid_eq(iid, sdk::IID_IReferenceClock)) {
            out = Iface::ReferenceClock;
            return true;
        }
        return false;
    }

    void final_release(Marshaler& m) override {
        (void)m;
        shutdown();
    }

    static bool guid_eq(const Guid& a, const Guid& b) {
        return std::memcmp(&a, &b, sizeof(Guid)) == 0;
    }

    void ensure_worker() {
        if (worker.joinable()) return;
        worker = std::thread([this] { run(); });
    }

    void run() {
        SyncManager* sync = host->mgr().sync_manager();
        std::unique_lock<std::mutex> lock(mutex);
        while (!stop) {
            const i64 now = reference_clock_now();
            i64 next = 0;
            for (usize i = 0; i < advises.size();) {
                Advise& a = advises[i];
                if (a.due <= now) {
                    if (sync != nullptr) {
                        if (a.period == 0) {
                            (void)sync->set_event(a.handle);
                        } else {
                            (void)sync->release_semaphore(a.handle, 1, nullptr);
                        }
                    }
                    if (a.period == 0) {
                        advises.erase(advises.begin() + static_cast<std::ptrdiff_t>(i));
                        continue;
                    }
                    a.due += a.period;
                    continue;  // may be due again
                }
                if (next == 0 || a.due < next) next = a.due;
                ++i;
            }
            if (next == 0) {
                cv.wait(lock);
            } else {
                cv.wait_for(lock, std::chrono::microseconds((next - now) / 10 + 1));
            }
        }
    }

    u32 call(Marshaler& m, Iface iface, u32 slot, std::span<const u64> args) override {
        (void)iface;
        const auto arg = [&](usize i) { return i < args.size() ? args[i] : 0; };
        switch (slot) {
        case 3: {  // GetTime(REFERENCE_TIME*)
            const GuestAddr out = static_cast<GuestAddr>(arg(0));
            if (out == 0) return E_POINTER_;
            (void)m.write_u64(out, static_cast<u64>(reference_clock_now()));
            return S_OK_;
        }
        case 4:    // AdviseTime(baseTime, streamTime, HEVENT, DWORD_PTR* pdwAdviseCookie)
        case 5: {  // AdvisePeriodic(startTime, periodTime, HSEMAPHORE, DWORD_PTR*)
            const i64 a = static_cast<i64>(arg(0));
            const i64 b = static_cast<i64>(arg(1));
            const u32 handle = static_cast<u32>(arg(2));
            const GuestAddr cookie_out = static_cast<GuestAddr>(arg(3));
            if (cookie_out == 0) return E_POINTER_;
            // Measured: a NULL handle, base + stream <= 0 (AdviseTime) and a
            // zero period (AdvisePeriodic) are E_INVALIDARG.
            if (handle == 0) return E_INVALIDARG_;
            if (slot == 4 && a + b <= 0) return E_INVALIDARG_;
            if (slot == 5 && (b <= 0 || a < 0)) return E_INVALIDARG_;
            std::lock_guard<std::mutex> lock(mutex);
            const u64 cookie = next_cookie++;
            advises.push_back(Advise{cookie, slot == 4 ? a + b : a, slot == 5 ? b : 0, handle});
            (void)m.write_ptr(cookie_out, cookie);
            ensure_worker();
            cv.notify_all();
            return S_OK_;
        }
        case 6: {  // Unadvise(DWORD_PTR): S_FALSE once fired or unknown (measured)
            std::lock_guard<std::mutex> lock(mutex);
            for (auto it = advises.begin(); it != advises.end(); ++it) {
                if (it->cookie == arg(0)) {
                    advises.erase(it);
                    return S_OK_;
                }
            }
            return S_FALSE_;
        }
        default:
            return E_NOTIMPL_;
        }
    }
};

} // namespace

i64 reference_clock_now() {
    static const mach_timebase_info_data_t timebase = [] {
        mach_timebase_info_data_t info{};
        mach_timebase_info(&info);
        return info;
    }();
    const auto ns = static_cast<unsigned __int128>(mach_continuous_time()) * timebase.numer /
                    timebase.denom;
    return static_cast<i64>(ns / 100u);
}

GuestAddr create_system_clock(DShowHost& host, Marshaler& m) {
    auto clock = std::make_shared<SystemClock>();
    if (!host.publish(m, clock, {Iface::ReferenceClock})) return 0;
    return clock->pointer(Iface::ReferenceClock);
}

} // namespace vitreo::winapi::dshow
