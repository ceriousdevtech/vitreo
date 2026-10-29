/// @file powrprof.cpp
/// @brief POWRPROF.DLL power-information contracts.

#include <algorithm>
#include <array>
#include <cstdio>
#include <iterator>
#include <mutex>
#include "powrprof.hpp"

#include "winapi/kernel32/system/sysinfo.hpp"
#include "winapi/kernel32/system/processor_topology.hpp"
#include <vitreo/virtual_tsc.hpp>
#include "winapi/core/marshaler.hpp"

namespace vitreo::winapi {

namespace {

// POWER_INFORMATION_LEVEL
constexpr u32 PROCESSOR_INFORMATION = 11;
constexpr u32 SYSTEM_POWER_INFORMATION = 12;

// NTSTATUS values returned directly by CallNtPowerInformation.
constexpr u32 STATUS_SUCCESS            = 0x00000000u;
constexpr u32 STATUS_ACCESS_VIOLATION   = 0xC0000005u;
constexpr u32 STATUS_INVALID_PARAMETER  = 0xC000000Du;
constexpr u32 STATUS_BUFFER_TOO_SMALL   = 0xC0000023u;
constexpr u32 STATUS_NOT_SUPPORTED      = 0xC00000BBu;

// PROCESSOR_POWER_INFORMATION contains six ULONG fields.
constexpr u32 PROCESSOR_POWER_INFORMATION_SIZE = 24;
constexpr u32 SYSTEM_POWER_INFORMATION_SIZE = 16;

// Vitreo exposes a synthetic x86-64 processor to the guest. Power information
// must describe that processor rather than leak an unrelated ARM host
// frequency. Keep the three Windows frequency fields internally consistent.
/// A processor's clock for PROCESSOR_POWER_INFORMATION: its core class's
/// highest clock (P and E cores differ), or the guest TSC rate when the host
/// does not expose one. MaxMhz, CurrentMhz and MhzLimit all report it: macOS
/// gives an unprivileged process no per-core current clock.
u32 processor_mhz(u32 index) {
    const auto& topology = processor_topology::topology();
    const u32 mhz = index < topology.max_mhz.size() ? topology.max_mhz[index] : 0;
    return mhz != 0 ? mhz : static_cast<u32>(guest_tsc_hz() / 1'000'000);
}

void impl_call_nt_power_information(const SysInfoManager& sysinfo,
                                    Marshaler& m) {
    const u32 information_level = static_cast<u32>(m.arg(0));
    const GuestAddr input_buffer = m.arg_ptr(1);
    const u32 input_length = static_cast<u32>(m.arg(2));
    const GuestAddr output_buffer = m.arg_ptr(3);
    const u32 output_length = static_cast<u32>(m.arg(4));

    // Measured (tools/oracle_probe/s7_misc_probe.expected "levels:"): with no
    // input buffer only these levels are accepted; every other level --
    // including undefined ones -- is STATUS_INVALID_PARAMETER.
    constexpr u32 SYSTEM_BATTERY_STATE = 5;
    static constexpr u32 kQueryableLevels[] = {
        4, 5, 8, 9, 0x0B, 0x0C, 0x0E, 0x0F, 0x10, 0x12, 0x13, 0x14, 0x15, 0x16, 0x41};
    const bool queryable = std::find(std::begin(kQueryableLevels),
                                     std::end(kQueryableLevels),
                                     information_level) != std::end(kQueryableLevels);
    if (!queryable) {
        m.set_return(STATUS_INVALID_PARAMETER);
        return;
    }
    if (information_level != PROCESSOR_INFORMATION &&
        information_level != SYSTEM_POWER_INFORMATION &&
        information_level != SYSTEM_BATTERY_STATE) {
        static std::once_flag once;
        std::call_once(once, [information_level] {
            std::fprintf(stderr, "[vitreo] UNIMPLEMENTED powrprof!CallNtPowerInformation: "
                                 "level %u\n", information_level);
        });
        m.set_return(STATUS_NOT_SUPPORTED);
        return;
    }

    // Both supported information levels are query-only.
    if (input_buffer != 0 || input_length != 0) {
        VITREO_TRACE(
            "[powrprof] CallNtPowerInformation(level=%u) "
            "input=0x%llx input_len=%u -> STATUS_INVALID_PARAMETER\n",
            information_level,
            static_cast<unsigned long long>(input_buffer), input_length);
        m.set_return(STATUS_INVALID_PARAMETER);
        return;
    }

    if (information_level == SYSTEM_BATTERY_STATE) {
        // SYSTEM_BATTERY_STATE (32 bytes): AcOnLine, BatteryPresent,
        // Charging, Discharging, Spare1[3], Tag, then six ULONG/LONG
        // capacity/rate/time fields. Vitreo presents a machine on AC power
        // with no battery (GetSystemPowerStatus says the same); measured on
        // such a Windows machine: AcOnLine 1, everything else 0.
        constexpr u32 SYSTEM_BATTERY_STATE_SIZE = 32;
        if (output_buffer == 0 || output_length < SYSTEM_BATTERY_STATE_SIZE) {
            m.set_return(STATUS_BUFFER_TOO_SMALL);
            return;
        }
        std::array<u8, SYSTEM_BATTERY_STATE_SIZE> state{};
        state[0] = 1;  // AcOnLine
        if (m.memory().write_bytes(output_buffer, state.data(), state.size()).is_err()) {
            m.set_return(STATUS_ACCESS_VIOLATION);
            return;
        }
        m.set_return(STATUS_SUCCESS);
        return;
    }

    if (information_level == SYSTEM_POWER_INFORMATION) {
        if (output_buffer == 0 ||
            output_length < SYSTEM_POWER_INFORMATION_SIZE) {
            VITREO_TRACE(
                "[powrprof] CallNtPowerInformation(SystemPowerInformation) "
                "output=0x%llx output_len=%u required=%u -> "
                "STATUS_BUFFER_TOO_SMALL\n",
                static_cast<unsigned long long>(output_buffer), output_length,
                SYSTEM_POWER_INFORMATION_SIZE);
            m.set_return(STATUS_BUFFER_TOO_SMALL);
            return;
        }

        // SYSTEM_POWER_INFORMATION describes guest-observable idle state.
        // Vitreo currently has no guest idle-policy timer or x86 thermal zone:
        // report the process as active, no pending idle timeout, and the
        // documented invalid/no-thermal-zone cooling mode.
        const bool wrote =
            m.write_u32(output_buffer + 0, 90).is_ok() &&
            m.write_u32(output_buffer + 4, 0).is_ok() &&
            m.write_u32(output_buffer + 8, 0).is_ok() &&
            m.write_u8(output_buffer + 12, 2).is_ok() &&
            m.write_u8(output_buffer + 13, 0).is_ok() &&
            m.write_u8(output_buffer + 14, 0).is_ok() &&
            m.write_u8(output_buffer + 15, 0).is_ok();
        if (!wrote) {
            VITREO_TRACE(
                "[powrprof] CallNtPowerInformation(SystemPowerInformation) "
                "write failed at output=0x%llx -> STATUS_ACCESS_VIOLATION\n",
                static_cast<unsigned long long>(output_buffer));
            m.set_return(STATUS_ACCESS_VIOLATION);
            return;
        }

        VITREO_TRACE(
            "[powrprof] CallNtPowerInformation(SystemPowerInformation) "
            "idle=0%% threshold=90%% cooling=invalid -> STATUS_SUCCESS\n");
        m.set_return(STATUS_SUCCESS);
        return;
    }

    const u32 processor_count =
        sysinfo.get_system_info().number_of_processors;
    const u64 required_length =
        static_cast<u64>(processor_count) *
        PROCESSOR_POWER_INFORMATION_SIZE;
    if (output_buffer == 0 || output_length < required_length) {
        VITREO_TRACE(
            "[powrprof] CallNtPowerInformation(ProcessorInformation) "
            "output=0x%llx output_len=%u required=%llu -> "
            "STATUS_BUFFER_TOO_SMALL\n",
            static_cast<unsigned long long>(output_buffer), output_length,
            static_cast<unsigned long long>(required_length));
        m.set_return(STATUS_BUFFER_TOO_SMALL);
        return;
    }

    for (u32 index = 0; index < processor_count; ++index) {
        const GuestAddr entry =
            output_buffer +
            static_cast<u64>(index) * PROCESSOR_POWER_INFORMATION_SIZE;
        const bool wrote =
            m.write_u32(entry + 0, index).is_ok() &&
            m.write_u32(entry + 4, processor_mhz(index)).is_ok() &&
            m.write_u32(entry + 8, processor_mhz(index)).is_ok() &&
            m.write_u32(entry + 12, processor_mhz(index)).is_ok() &&
            m.write_u32(entry + 16, 0).is_ok() &&
            m.write_u32(entry + 20, 0).is_ok();
        if (!wrote) {
            VITREO_TRACE(
                "[powrprof] CallNtPowerInformation(ProcessorInformation) "
                "write failed at processor=%u output=0x%llx -> "
                "STATUS_ACCESS_VIOLATION\n",
                index, static_cast<unsigned long long>(entry));
            m.set_return(STATUS_ACCESS_VIOLATION);
            return;
        }
    }

    VITREO_TRACE(
        "[powrprof] CallNtPowerInformation(ProcessorInformation) "
        "processors=%u mhz=%u -> STATUS_SUCCESS\n",
        processor_count, processor_mhz(0));
    m.set_return(STATUS_SUCCESS);
}

} // namespace

DllExports create_powrprof_thunks(const SysInfoManager& sysinfo) {
    DllExports exports;
    exports.entries.push_back(ExportEntry{
        "CallNtPowerInformation", 0,
        [&sysinfo](Marshaler& m) {
            impl_call_nt_power_information(sysinfo, m);
        },
        20
    });
    return exports;
}

} // namespace vitreo::winapi
