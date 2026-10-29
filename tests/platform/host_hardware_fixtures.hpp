#pragma once
/// @file host_hardware_fixtures.hpp
/// @brief Host hardware profiles for tests that must not depend on the Mac
/// running them.

#include "platform/host_hardware.hpp"

namespace vitreo::test {

/// An Apple M4 Pro (14 cores) as macOS reports it (sysctl hw.perflevel*,
/// IODeviceTree:/cpus, pmgr), read from a real Mac16,8:
/// - CPUs 0-3: Efficiency, one cluster (64K L1D, 128K L1I, 4 MB L2), 1020-2592 MHz;
/// - CPUs 4-8 and 9-13: Performance, two clusters of 5 (128K L1D, 192K L1I,
///   16 MB L2 each), 1260-4512 MHz.
inline platform::HostHardwareProfile m4_pro_profile() {
    platform::HostHardwareProfile host;
    host.cpu.brand = "Apple M4 Pro";
    host.cpu.cache_line_bytes = 128;
    host.cpu.timebase_hz = 24'000'000;
    platform::HostCoreClass performance;
    performance.name = "Performance";
    performance.physical_cores = performance.logical_processors = 10;
    performance.l1i_bytes = 196608;
    performance.l1d_bytes = 131072;
    performance.l2_bytes = 16777216;
    performance.cores_per_l2 = 5;
    performance.min_mhz = 1260;
    performance.max_mhz = 4512;
    platform::HostCoreClass efficiency;
    efficiency.name = "Efficiency";
    efficiency.efficiency = true;
    efficiency.physical_cores = efficiency.logical_processors = 4;
    efficiency.l1i_bytes = 131072;
    efficiency.l1d_bytes = 65536;
    efficiency.l2_bytes = 4194304;
    efficiency.cores_per_l2 = 4;
    efficiency.min_mhz = 1020;
    efficiency.max_mhz = 2592;
    host.cpu.core_classes = {performance, efficiency};
    for (u32 cpu = 0; cpu < 14; ++cpu) {
        host.cpu.processors.push_back(cpu < 4 ? platform::HostLogicalProcessor{1, 0}
                                              : platform::HostLogicalProcessor{0, cpu < 9 ? 1u : 2u});
    }
    host.memory.total_bytes = 24ull << 30;
    host.memory.type = "LPDDR5";
    host.memory.manufacturer = "Micron";
    host.memory.ecc = true;
    host.gpu.name = "Apple M4 Pro";
    host.gpu.core_count = 20;
    host.gpu.min_mhz = 338;
    host.gpu.max_mhz = 1578;
    host.machine.manufacturer = "Apple Inc.";
    host.machine.model_identifier = "Mac16,8";
    host.machine.product_name = "MacBook Pro (14-inch, Nov 2024)";
    host.machine.product_family = "MacBook Pro";
    host.machine.part_number = "MX2J3LL/A";
    host.machine.board_id = "J614s";
    host.machine.firmware_vendor = "Apple Inc.";
    host.machine.firmware_version = "13822.81.10";
    host.machine.firmware_date = "01/28/2026";
    host.machine.serial_number = "TESTSERIAL";
    host.machine.hardware_uuid = "01234567-89AB-CDEF-0123-456789ABCDEF";
    host.machine.chassis = platform::HostChassis::Notebook;
    return host;
}

/// Installs @p profile for one test's lifetime.
class ScopedHostHardware {
public:
    explicit ScopedHostHardware(platform::HostHardwareProfile profile) : m_profile(std::move(profile)) {
        platform::set_host_hardware_for_testing(&m_profile);
    }
    ~ScopedHostHardware() { platform::set_host_hardware_for_testing(nullptr); }
    ScopedHostHardware(const ScopedHostHardware&) = delete;
    ScopedHostHardware& operator=(const ScopedHostHardware&) = delete;
    const platform::HostHardwareProfile& profile() const { return m_profile; }

private:
    platform::HostHardwareProfile m_profile;
};

} // namespace vitreo::test
