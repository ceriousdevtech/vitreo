/// @file host_memory_floor.cpp
/// @brief See host_memory_floor.hpp. System headers stay here: <sys/sysctl.h>
///        defines macros (FSCALE, ...) that collide with Vitreo identifiers.

#include "core/memory/host_memory_floor.hpp"


#include <dispatch/dispatch.h>
#include <mach/mach.h>
#include <sys/sysctl.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>

namespace vitreo::memory {

namespace host_memory_floor_detail {

inline u64 now_ns() noexcept {
    return static_cast<u64>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

inline u64 query_obtainable_bytes() noexcept {
    vm_statistics64_data_t vm{};
    mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
    if (host_statistics64(mach_host_self(), HOST_VM_INFO64,
                          reinterpret_cast<host_info64_t>(&vm),
                          &count) != KERN_SUCCESS) {
        return ~u64{0};  // unknown: never refuse on a failed query
    }
    // vm_statistics.h: free_count already includes speculative pages;
    // external pages are file-backed (evictable), internal are anonymous and
    // can only be compressed. Compressing them frees what the compressor's
    // current ratio (compressor_page_count physical pages holding
    // total_uncompressed_pages_in_compressor) saves; with nothing compressed
    // yet there is no measured ratio, so they count for nothing. Speculative
    // pages are file-backed reads too, so they are taken out of external
    // rather than counted twice.
    const u64 external = vm.external_page_count > vm.speculative_count
        ? vm.external_page_count - vm.speculative_count : 0;
    const u64 purgeable = vm.purgeable_count;
    const u64 internal = vm.internal_page_count > purgeable
        ? vm.internal_page_count - purgeable : 0;
    const u64 stored = vm.total_uncompressed_pages_in_compressor;
    const u64 occupied = vm.compressor_page_count;
    const u64 compressible = stored > occupied
        ? internal - internal * occupied / stored : 0;
    const u64 pages = static_cast<u64>(vm.free_count) + external + purgeable +
                      compressible;
    return pages * static_cast<u64>(vm_page_size);
}

inline HostMemoryPressure query_pressure() noexcept {
    // kern.memorystatus_vm_pressure_level reports the level that
    // DISPATCH_SOURCE_TYPE_MEMORYPRESSURE delivers, in the
    // DISPATCH_MEMORYPRESSURE_* encoding (dispatch/source.h).
    int level = 0;
    size_t size = sizeof(level);
    if (sysctlbyname("kern.memorystatus_vm_pressure_level", &level, &size,
                     nullptr, 0) != 0) {
        return HostMemoryPressure::normal;  // unknown: never refuse
    }
    if (level == DISPATCH_MEMORYPRESSURE_CRITICAL) {
        return HostMemoryPressure::critical;
    }
    if (level == DISPATCH_MEMORYPRESSURE_WARN) return HostMemoryPressure::warn;
    return HostMemoryPressure::normal;
}

inline u64 physical_bytes() noexcept {
    u64 bytes = 0;
    size_t size = sizeof(bytes);
    if (sysctlbyname("hw.memsize", &bytes, &size, nullptr, 0) != 0) return 0;
    return bytes;
}

struct ReadingsOverride {
    std::mutex mutex;
    bool active{false};
    HostMemoryReadings readings{};
};

inline ReadingsOverride& readings_override() noexcept {
    static ReadingsOverride value;
    return value;
}

inline std::atomic<bool>& readings_overridden() noexcept {
    static std::atomic<bool> value{false};
    return value;
}

inline std::atomic<u64>& floor_override() noexcept {
    static std::atomic<u64> value{~u64{0}};  // ~0: not overridden
    return value;
}

}  // namespace host_memory_floor_detail

HostMemoryReadings host_memory_readings() noexcept {
    using namespace host_memory_floor_detail;
    if (readings_overridden().load(std::memory_order_acquire)) {
        auto& pinned = readings_override();
        std::lock_guard<std::mutex> lock(pinned.mutex);
        if (pinned.active) return pinned.readings;
    }
    static std::atomic<u64> cached_bytes{0};
    static std::atomic<u8> cached_pressure{0};
    static std::atomic<u64> cached_at{0};
    const u64 now = now_ns();
    const u64 at = cached_at.load(std::memory_order_acquire);
    if (at != 0 && now - at < 20'000'000ull) {
        return {cached_bytes.load(std::memory_order_relaxed),
                static_cast<HostMemoryPressure>(
                    cached_pressure.load(std::memory_order_relaxed))};
    }
    const HostMemoryReadings readings{query_obtainable_bytes(),
                                      query_pressure()};
    cached_bytes.store(readings.obtainable_bytes, std::memory_order_relaxed);
    cached_pressure.store(static_cast<u8>(readings.pressure),
                          std::memory_order_relaxed);
    cached_at.store(now, std::memory_order_release);
    return readings;
}

void set_host_memory_readings_override(
    const HostMemoryReadings* readings) noexcept {
    using namespace host_memory_floor_detail;
    auto& pinned = readings_override();
    std::lock_guard<std::mutex> lock(pinned.mutex);
    pinned.active = readings != nullptr;
    if (readings != nullptr) pinned.readings = *readings;
    readings_overridden().store(pinned.active, std::memory_order_release);
}

u64 host_available_bytes() noexcept {
    return host_memory_readings().obtainable_bytes;
}

u64 host_memory_floor_bytes() noexcept {
    using namespace host_memory_floor_detail;
    const u64 pinned = floor_override().load(std::memory_order_relaxed);
    if (pinned != ~u64{0}) return pinned;
    static const u64 floor = [] {
        if (const char* text = std::getenv("VITREO_MEMORY_FLOOR_MB")) {
            return static_cast<u64>(std::strtoull(text, nullptr, 10)) << 20;
        }
        return std::max<u64>(u64{2} << 30, physical_bytes() / 8);
    }();
    return floor;
}

void set_host_memory_floor_bytes(u64 bytes) noexcept {
    host_memory_floor_detail::floor_override().store(
        bytes, std::memory_order_relaxed);
}

namespace {

thread_local bool t_refused = false;

u64 saturating_add(u64 a, u64 b) noexcept {
    return b > ~u64{0} - a ? ~u64{0} : a + b;
}

/// GPU budget under explicit readings; ~0 means unlimited.
u64 gpu_budget(GpuWorkingSet device, u64 floor,
               const HostMemoryReadings& host) noexcept {
    if (floor == 0) return device.recommended != 0 ? device.recommended : ~u64{0};
    if (host.pressure == HostMemoryPressure::critical) return device.allocated;
    const u64 headroom = host.obtainable_bytes > floor
        ? host.obtainable_bytes - floor : 0;
    const u64 host_limit = saturating_add(device.allocated, headroom);
    return device.recommended != 0 ? std::min(device.recommended, host_limit)
                                   : host_limit;
}

bool refuse(const char* what, u64 bytes, const char* rule,
            const HostMemoryReadings& host, u64 floor,
            GpuWorkingSet device) noexcept {
    t_refused = true;
    static std::atomic<u64> refusals{0};
    const u64 n = refusals.fetch_add(1, std::memory_order_relaxed);
    if (n < 16 || (n & (n - 1)) == 0) {
        static constexpr const char* kPressure[] = {"normal", "warn",
                                                    "critical"};
        std::fprintf(stderr,
                     "[vitreo] memory floor: refused %s of %llu MiB by %s "
                     "(host obtainable %llu MiB, floor %llu MiB, pressure %s, "
                     "gpu allocated %llu MiB of recommended %llu MiB, "
                     "refusal #%llu)\n",
                     what, static_cast<unsigned long long>(bytes >> 20), rule,
                     static_cast<unsigned long long>(host.obtainable_bytes >> 20),
                     static_cast<unsigned long long>(floor >> 20),
                     kPressure[static_cast<u8>(host.pressure)],
                     static_cast<unsigned long long>(device.allocated >> 20),
                     static_cast<unsigned long long>(device.recommended >> 20),
                     static_cast<unsigned long long>(n + 1));
    }
    return false;
}

}  // namespace

bool take_host_memory_refusal() noexcept {
    const bool refused = t_refused;
    t_refused = false;
    return refused;
}

bool host_can_commit(u64 bytes, const char* what) noexcept {
    t_refused = false;
    const u64 floor = host_memory_floor_bytes();
    if (floor == 0) return true;
    const HostMemoryReadings host = host_memory_readings();
    if (host.pressure != HostMemoryPressure::critical) return true;
    return refuse(what, bytes, "critical memory pressure", host, floor, {});
}

bool host_can_allocate_gpu(u64 bytes, GpuWorkingSet device,
                           const char* what) noexcept {
    t_refused = false;
    const u64 floor = host_memory_floor_bytes();
    if (floor == 0 || bytes == 0) return true;
    const HostMemoryReadings host = host_memory_readings();
    if (host.pressure == HostMemoryPressure::critical) {
        return refuse(what, bytes, "critical memory pressure", host, floor,
                      device);
    }
    if (host.obtainable_bytes < floor ||
        bytes > host.obtainable_bytes - floor) {
        return refuse(what, bytes, "host floor", host, floor, device);
    }
    return true;
}

u64 host_gpu_budget(GpuWorkingSet device) noexcept {
    return gpu_budget(device, host_memory_floor_bytes(), host_memory_readings());
}

}  // namespace vitreo::memory
