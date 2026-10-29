#pragma once

/// @file sysinfo.hpp
/// @brief System information APIs (WIN32-064)

#include <vitreo/common.hpp>
#include "winapi/core/thunk_registry.hpp"

#include <string>

namespace vitreo::capsule { class Vfs; }
namespace vitreo { class GuestMemory; }

namespace vitreo::winapi {

class Marshaler;

/// Number of active logical processors the guest is told the machine has.
/// Single source of truth: GetSystemInfo, GetProcessAffinityMask and the NT
/// SystemBasicInformation block must all agree, or a guest that sizes its
/// worker pool from one and its scheduling from another runs inconsistently.
[[nodiscard]] u32 host_active_processor_count();

/// Bit vector of those processors — bit N set means processor N is configured
/// into the system. This is the system affinity mask Windows reports.
[[nodiscard]] u64 host_active_processor_mask();

// =============================================================================
// SYSTEM_INFO structure (matches Windows x64 layout)
// =============================================================================
struct SystemInfo {
    u16 processor_architecture{9};  // PROCESSOR_ARCHITECTURE_AMD64
    u16 reserved{0};
    u32 page_size{4096};
    u64 min_app_address{0x10000};
    u64 max_app_address{0x7FFFFFFEFFFF};
    u64 active_processor_mask{0xF};
    u32 number_of_processors{4};
    u32 processor_type{8664};  // PROCESSOR_AMD_X8664
    u32 allocation_granularity{65536};
    u16 processor_level{6};
    u16 processor_revision{0x3C03};
};

// =============================================================================
// OSVERSIONINFOW (simplified)
// =============================================================================
struct OsVersionInfo {
    u32 os_version_info_size{0};
    u32 major_version{10};
    u32 minor_version{0};
    u32 build_number{19041};
    u32 platform_id{2};  // VER_PLATFORM_WIN32_NT
    // szCSDVersion is 128 wchar_t (256 bytes) — all zeros for no SP
};

// =============================================================================
// Processor Feature constants
// =============================================================================
namespace pf {
    constexpr u32 PF_COMPARE_EXCHANGE_DOUBLE = 2;
    constexpr u32 PF_MMX_INSTRUCTIONS_AVAILABLE = 3;
    constexpr u32 PF_XMMI_INSTRUCTIONS_AVAILABLE = 6;   // SSE
    constexpr u32 PF_RDTSC_INSTRUCTION_AVAILABLE = 8;
    constexpr u32 PF_PAE_ENABLED = 9;
    constexpr u32 PF_XMMI64_INSTRUCTIONS_AVAILABLE = 10; // SSE2
    constexpr u32 PF_NX_ENABLED = 12;
    constexpr u32 PF_SSE3_INSTRUCTIONS_AVAILABLE = 13;
    constexpr u32 PF_COMPARE_EXCHANGE128 = 14;
    constexpr u32 PF_SECOND_LEVEL_ADDRESS_TRANSLATION = 20;
    constexpr u32 PF_VIRT_FIRMWARE_ENABLED = 21;
    constexpr u32 PF_RDWRFSGSBASE_AVAILABLE = 22;
    constexpr u32 PF_FASTFAIL_AVAILABLE = 23;  // _fastfail / INT 0x29
    // winnt.h (SDK 10.0.26100.0) — the x86/x64 features derived from CPUID.
    constexpr u32 PF_FLOATING_POINT_PRECISION_ERRATA = 0;
    constexpr u32 PF_FLOATING_POINT_EMULATED = 1;
    constexpr u32 PF_3DNOW_INSTRUCTIONS_AVAILABLE = 7;
    constexpr u32 PF_SSE_DAZ_MODE_AVAILABLE = 11;
    constexpr u32 PF_XSAVE_ENABLED = 17;
    constexpr u32 PF_RDRAND_INSTRUCTION_AVAILABLE = 28;
    constexpr u32 PF_RDTSCP_INSTRUCTION_AVAILABLE = 32;
    constexpr u32 PF_RDPID_INSTRUCTION_AVAILABLE = 33;
    constexpr u32 PF_MONITORX_INSTRUCTION_AVAILABLE = 35;
    constexpr u32 PF_SSSE3_INSTRUCTIONS_AVAILABLE = 36;
    constexpr u32 PF_SSE4_1_INSTRUCTIONS_AVAILABLE = 37;
    constexpr u32 PF_SSE4_2_INSTRUCTIONS_AVAILABLE = 38;
    constexpr u32 PF_AVX_INSTRUCTIONS_AVAILABLE = 39;
    constexpr u32 PF_AVX2_INSTRUCTIONS_AVAILABLE = 40;
    constexpr u32 PF_AVX512F_INSTRUCTIONS_AVAILABLE = 41;
    constexpr u32 PF_ERMS_AVAILABLE = 42;
    constexpr u32 PF_BMI2_INSTRUCTIONS_AVAILABLE = 60;
    constexpr u32 PF_MOVDIR64B_INSTRUCTION_AVAILABLE = 61;
    /// Size of KUSER_SHARED_DATA.ProcessorFeatures (PROCESSOR_FEATURE_MAX).
    constexpr u32 PROCESSOR_FEATURE_MAX = 64;
}

/// IsProcessorFeaturePresent answered from the CPUID the virtual CPU
/// advertises in the given execution mode (core/cpu/cpuid_synth.hpp), with the
/// derivation rules measured on Windows by
/// tools/oracle_probe/processor_features_probe.c. Also feeds
/// KUSER_SHARED_DATA.ProcessorFeatures so both views agree.
[[nodiscard]] bool processor_feature_present_for_cpu(u32 feature,
                                                     bool long_mode) noexcept;

// =============================================================================
// Computer Name
// =============================================================================
constexpr u32 MAX_COMPUTERNAME_LENGTH = 15;

// =============================================================================
// SysInfoManager
// =============================================================================
class SysInfoManager {
public:
    explicit SysInfoManager(const GuestMemory* memory = nullptr);

    void set_vfs(capsule::Vfs* vfs) noexcept { m_vfs = vfs; }

    void set_windows_10_compatible(bool compatible) noexcept {
        m_windows_10_compatible = compatible;
    }

    void set_windows_81_compatible(bool compatible) noexcept {
        m_windows_81_compatible = compatible;
    }

    /// Get system info
    [[nodiscard]] SystemInfo get_system_info() const noexcept;

    /// Get OS version info
    /// @param info_size The dwOSVersionInfoSize from caller
    /// @param out Output structure
    /// @return true if valid, false if size mismatch
    [[nodiscard]] bool get_version_ex(u32 info_size, OsVersionInfo& out) const;

    /// Get system time as FILETIME (100-ns intervals since 1601-01-01)
    [[nodiscard]] u64 get_system_time_as_filetime() const;

    /// Query performance counter (1 GHz virtual TSC, same as RDTSC)
    [[nodiscard]] u64 query_performance_counter() const;

    /// Query performance frequency (1e9, matching the virtual TSC)
    [[nodiscard]] u64 query_performance_frequency() const;

    /// Unbiased interrupt time in 100-nanosecond units. mach_absolute_time is
    /// monotonic and, unlike the continuous clock, excludes host sleep.
    [[nodiscard]] u64 query_unbiased_interrupt_time_precise() const;

    /// Is processor feature present, for a guest in @p long_mode (x64) or
    /// 32-bit mode — the virtual CPUID differs between the two.
    [[nodiscard]] bool is_processor_feature_present(u32 feature,
                                                    bool long_mode = true) const;

    /// Get computer name
    [[nodiscard]] const std::string& get_computer_name() const noexcept;

    /// Set computer name (truncated to MAX_COMPUTERNAME_LENGTH)
    void set_computer_name(std::string name);

    /// GetComputerNameA — writes ANSI name to guest buffer
    bool get_computer_name_a(GuestAddr buffer, GuestAddr size_ptr, Marshaler& m) const;

    /// GetComputerNameW — writes wide name to guest buffer
    bool get_computer_name_w(GuestAddr buffer, GuestAddr size_ptr, Marshaler& m) const;

    /// GlobalMemoryStatusEx — fill MEMORYSTATUSEX struct
    bool global_memory_status_ex(GuestAddr buffer, Marshaler& m) const;

    /// GetNativeSystemInfo — like GetSystemInfo but with real CPU count
    void get_native_system_info(GuestAddr info_ptr, Marshaler& m) const;

    /// GetDiskFreeSpaceExW — query disk space
    bool get_disk_free_space_ex_w(const std::string& dir, GuestAddr free_caller,
                                  GuestAddr total_bytes, GuestAddr total_free,
                                  Marshaler& m) const;

    /// GetVolumeInformationW — query volume info
    bool get_volume_information_w(GuestAddr root, GuestAddr vol_name_buf, u32 vol_name_size,
                                  GuestAddr serial, GuestAddr max_comp_len,
                                  GuestAddr flags, GuestAddr fs_name_buf, u32 fs_name_size,
                                  Marshaler& m) const;

    /// GetDriveTypeW — return drive type for path
    [[nodiscard]] u32 get_drive_type_w(const std::string& path) const;

    /// GetLogicalDrives — return bitmask of available drives
    [[nodiscard]] u32 get_logical_drives() const;

    /// GetLogicalDriveStringsW — write drive strings to buffer
    [[nodiscard]] u32 get_logical_drive_strings_w(u32 buf_len, GuestAddr buffer, Marshaler& m) const;

private:
    SystemInfo m_info{};
    const GuestMemory* m_memory{nullptr};
    std::string m_computer_name{"VITREO"};
    capsule::Vfs* m_vfs{nullptr};
    bool m_windows_10_compatible{false};
    bool m_windows_81_compatible{false};
};

/// Register sysinfo thunks
DllExports create_sysinfo_thunks(SysInfoManager& mgr);

} // namespace vitreo::winapi
