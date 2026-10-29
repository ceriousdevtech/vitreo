// SPDX-License-Identifier: LGPL-2.1-only
#pragma once
/// @file host_hardware.hpp
/// @brief The physical machine Vitreo runs on, read once from the host.
///
/// Every Windows surface that describes hardware derives from this one
/// profile: CPUID, GetLogicalProcessorInformation(Ex), CPU sets, processor
/// power information, the HARDWARE registry keys, SMBIOS and the display
/// adapter identity. A guest therefore sees one consistent machine, the real
/// one, instead of per-surface constants that contradict each other.
///
/// A value the host does not expose is 0 or empty ("unknown"), never a guess.
/// Each consumer encodes "unknown" the way its own Windows contract defines.
///
/// Sources (macOS):
/// - sysctl: machdep.cpu.brand_string, hw.model, hw.memsize, hw.cachelinesize,
///   hw.nperflevels and hw.perflevelN.* (Apple silicon), or hw.physicalcpu,
///   hw.logicalcpu, hw.l*cachesize, hw.cacheconfig and hw.cpufrequency_*
///   (Intel Macs).
/// - IODeviceTree:/cpus/*: logical-cpu-id, cluster-type, cluster-id.
/// - IODeviceTree:/chosen: dram-type, dram-vendor, dram-ecc,
///   system-firmware-version.
/// - IODeviceTree:/ and :/product: target-type, model-number, region-info,
///   time-stamp, product-name.
/// - pmgr voltage-states tables: the DVFS frequencies of each CPU class and
///   of the GPU.
/// - IOPlatformExpertDevice: IOPlatformSerialNumber, IOPlatformUUID.
/// - AGXAccelerator: model, gpu-core-count.

#include <vitreo/types.hpp>

#include <atomic>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace vitreo::platform {

/// One class of cores (a macOS "performance level"): on Apple silicon the
/// Performance and the Efficiency cores; on an Intel Mac the only class.
struct HostCoreClass {
    std::string name;              ///< hw.perflevelN.name, e.g. "Performance"
    bool efficiency = false;       ///< the lower-performance class of a hybrid CPU
    u32 physical_cores = 0;
    u32 logical_processors = 0;
    u64 l1i_bytes = 0;             ///< per core
    u64 l1d_bytes = 0;             ///< per core
    u64 l2_bytes = 0;              ///< per L2 instance
    u32 cores_per_l2 = 0;
    u32 min_mhz = 0;               ///< lowest DVFS state; 0 = unknown
    u32 max_mhz = 0;               ///< highest DVFS state; 0 = unknown
};

/// One logical processor, indexed by the host's logical CPU number (the
/// number pthread_cpu_number_np reports for a thread running on it).
struct HostLogicalProcessor {
    u32 core_class = 0;            ///< index into HostCpu::core_classes
    u32 cluster = 0;               ///< processors in one cluster share an L2
};

struct HostCpu {
    std::string brand;             ///< machdep.cpu.brand_string, e.g. "Apple M4 Pro"
    u32 cache_line_bytes = 0;      ///< hw.cachelinesize
    u64 timebase_hz = 0;           ///< hw.tbfrequency: the crystal the host clock counts
    u64 l3_bytes = 0;              ///< hw.l3cachesize; 0 on Apple silicon
    /// Ordered as macOS orders performance levels: index 0 is the highest
    /// performance class.
    std::vector<HostCoreClass> core_classes;
    std::vector<HostLogicalProcessor> processors;
};

struct HostMemory {
    u64 total_bytes = 0;           ///< hw.memsize
    std::string type;              ///< e.g. "LPDDR5"
    std::string manufacturer;      ///< e.g. "Micron"
    bool ecc = false;
};

struct HostGpu {
    std::string name;              ///< e.g. "Apple M4 Pro"
    u32 core_count = 0;
    u32 min_mhz = 0;               ///< lowest active DVFS state; 0 = unknown
    u32 max_mhz = 0;               ///< 0 = unknown
};

/// The enclosure class of the machine, as SMBIOS distinguishes them.
enum class HostChassis : u8 { Unknown, Notebook, AllInOne, MiniPc, Desktop, Tower };

struct HostMachine {
    std::string manufacturer;      ///< "Apple Inc."
    std::string model_identifier;  ///< hw.model, e.g. "Mac16,8"
    std::string product_name;      ///< e.g. "MacBook Pro (14-inch, Nov 2024)"
    std::string product_family;    ///< e.g. "MacBook Pro"
    std::string part_number;       ///< model-number + region-info, e.g. "MX2J3LL/A"
    std::string board_id;          ///< target-type, e.g. "J614s"
    std::string firmware_vendor;   ///< "Apple Inc."
    std::string firmware_version;  ///< e.g. "13822.81.10"
    /// The firmware's build date as mm/dd/yyyy, from the device tree's root
    /// time-stamp (Apple firmware publishes no separate release date).
    std::string firmware_date;
    std::string serial_number;
    std::string hardware_uuid;     ///< canonical 8-4-4-4-12 text form
    HostChassis chassis = HostChassis::Unknown;
};

struct HostHardwareProfile {
    HostCpu cpu;
    HostMemory memory;
    HostGpu gpu;
    HostMachine machine;
};

/// The host's profile, read on first use and immutable afterwards.
[[nodiscard]] const HostHardwareProfile& host_hardware();

/// Makes host_hardware() return @p profile (nullptr restores the host's), so
/// tests do not depend on the machine they run on. Must not race readers.
void set_host_hardware_for_testing(const HostHardwareProfile* profile);

/// The highest clock of the host's fastest core class in Hz, or 0 when the
/// host does not expose it. Always the real host's value: the guest TSC rate
/// is fixed for the life of the process, so a test profile cannot change it.
[[nodiscard]] u64 host_max_core_hz() noexcept;

/// The associativity Vitreo reports for a host cache of @p level (CPUID leaf 4
/// and 0x80000006, Windows CACHE_DESCRIPTOR). macOS exposes each cache's size,
/// line size and sharing but not its associativity, and Apple does not publish
/// it, so this is the one cache property Vitreo chooses: 8 ways for L1 and 16
/// for L2 and beyond, the usual x86 shapes for those levels. When the cache's
/// line count is not a multiple of that, the next larger divisor is used, so
/// ways x sets x line is always the exact size. 0 for an invalid cache.
[[nodiscard]] u32 reported_cache_ways(u32 level, u64 size_bytes, u32 line_bytes) noexcept;

/// The logical processor the calling thread is running on, in the numbering
/// of HostCpu::processors (pthread_cpu_number_np). 0 when the host cannot say.
[[nodiscard]] u32 current_host_processor() noexcept;

/// A guest thread's processor affinity: bit N allows logical processor N of
/// HostCpu::processors (the single processor group). 0 = unrestricted. Owned
/// by the guest thread object and shared with the host thread running it.
using ProcessorAffinity = std::atomic<u64>;

/// Makes @p affinity the calling host thread's guest affinity (nullptr: none).
void bind_current_thread_affinity(std::shared_ptr<const ProcessorAffinity> affinity) noexcept;

/// The logical processor the calling guest thread runs on, as Windows reports
/// it (GetCurrentProcessorNumber, the CPUID APIC ID). That is the processor
/// macOS scheduled it on, unless the guest's affinity excludes it. macOS
/// cannot pin a thread, so then the thread is reported on the lowest
/// processor its affinity allows, where Windows would have run it.
[[nodiscard]] u32 current_processor() noexcept;

namespace host_hardware_detail {

/// One pmgr DVFS table (`voltage-statesN[-sram]`: little-endian u32 pairs of
/// frequency and voltage) as MHz, in table order, without the zero "off"
/// entries. Apple silicon before M4 stores Hz; M4 stores its CPU tables in
/// kHz. No DVFS state runs below 10 MHz, so a table whose largest value is
/// below 10^7 is in kHz.
[[nodiscard]] std::vector<u32> decode_dvfs_mhz(std::span<const u8> table);

/// "MacBook Pro (14-inch, Nov 2024)" -> "MacBook Pro".
[[nodiscard]] std::string product_family(std::string_view product_name);

/// The enclosure class of a Mac product family ("MacBook Pro" -> Notebook).
[[nodiscard]] HostChassis chassis_of(std::string_view product_family);

/// A device-tree string property (NUL-terminated or NUL-padded bytes) as text.
[[nodiscard]] std::string device_tree_string(std::span<const u8> bytes);

/// "Wed Jan 28 20:42:19 PST 2026" (the device tree's time-stamp) ->
/// "01/28/2026"; empty when it does not parse.
[[nodiscard]] std::string firmware_date(std::string_view time_stamp);

} // namespace host_hardware_detail

} // namespace vitreo::platform
