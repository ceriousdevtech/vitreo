#pragma once

/// @file host_memory_floor.hpp
/// @brief Refuse new large allocations before the host runs out of memory.
///
/// macOS does not stop a process that outgrows RAM: it compresses and swaps,
/// and GPU allocations can do neither. One title's world load drove a 24 GB
/// Mac from 4 GB available to a frozen WindowServer (and a forced power-off)
/// in under two seconds, with no jetsam kill and nothing logged.
///
/// Windows has a documented failure for this situation instead: allocations
/// fail with E_OUTOFMEMORY / ERROR_NOT_ENOUGH_MEMORY / STATUS_NO_MEMORY when
/// the commit limit is reached, and guests handle that by shrinking pools or
/// reporting out of video memory. Vitreo returns that documented failure
/// rather than let the whole machine lock up, with two admission rules:
///
/// - Pageable memory (guest MEM_COMMIT): Windows charges commit against
///   RAM + pagefile, not free RAM, and macOS backs it with the compressor and
///   swap. It is refused only while the kernel reports critical memory
///   pressure.
/// - Non-pageable GPU memory (Metal resources): refused when the memory the
///   kernel can still reclaim minus the request falls below the floor, or
///   while pressure is critical. Metal's recommended working set is only the
///   reported budget: Learn's QueryVideoMemoryInfo treats exceeding the budget
///   as a performance hazard, not an allocation failure.
///
/// Obtainable = free + file-backed + purgeable pages, plus the anonymous pages
/// the compressor would free at its current ratio. The floor defaults to
/// max(2 GiB, RAM / 8); VITREO_MEMORY_FLOOR_MB overrides it and 0 disables
/// both rules.

#include <vitreo/common.hpp>

namespace vitreo::memory {

enum class HostMemoryPressure : u8 { normal, warn, critical };

struct HostMemoryReadings {
    /// Bytes the kernel can hand out by evicting, purging, or compressing,
    /// without jetsam. ~0 when unknown (never refuse on a failed query).
    u64 obtainable_bytes{~u64{0}};
    HostMemoryPressure pressure{HostMemoryPressure::normal};
};

/// Current host readings. Cached for 20 ms: allocation paths call this per
/// resource.
[[nodiscard]] HostMemoryReadings host_memory_readings() noexcept;

/// Pin the readings (tests); nullptr restores the live host readings.
void set_host_memory_readings_override(const HostMemoryReadings* readings) noexcept;

/// host_memory_readings().obtainable_bytes.
[[nodiscard]] u64 host_available_bytes() noexcept;

/// The floor below which new GPU allocations are refused; 0 disables
/// admission.
[[nodiscard]] u64 host_memory_floor_bytes() noexcept;

/// Pin the floor (tests). ~0 restores the configured default.
void set_host_memory_floor_bytes(u64 bytes) noexcept;

/// The GPU device's own accounting. recommended == 0 means unknown.
struct GpuWorkingSet {
    u64 allocated{0};
    u64 recommended{0};
};

/// Pageable admission: whether a commit of @p bytes may proceed. A refusal
/// is logged (rate-limited) naming @p what, so a guest's failure is never
/// silent.
[[nodiscard]] bool host_can_commit(u64 bytes, const char* what) noexcept;

/// Non-pageable GPU admission for @p bytes on a device in state @p device.
[[nodiscard]] bool host_can_allocate_gpu(u64 bytes, GpuWorkingSet device,
                                         const char* what) noexcept;

/// The most GPU memory a device in state @p device may hold under
/// host_can_allocate_gpu: allocated + the largest request it would admit.
[[nodiscard]] u64 host_gpu_budget(GpuWorkingSet device) noexcept;

/// Whether the most recent admission check on this thread refused, and
/// clears it. Lets a frontend that only sees "creation failed" report the
/// documented out-of-memory result (E_OUTOFMEMORY) instead of a generic one.
[[nodiscard]] bool take_host_memory_refusal() noexcept;

}  // namespace vitreo::memory
