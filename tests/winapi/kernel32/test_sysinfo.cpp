/// @file test_sysinfo.cpp
/// @brief Tests for SysInfoManager (WIN32-064, WIN32-088)

#include <gtest/gtest.h>
#include <winapi/kernel32/system/time.hpp>
#include <vitreo/common.hpp>
#include <vitreo/virtual_tsc.hpp>
#include <winapi/kernel32/system/sysinfo.hpp>
#include <capsule/vfs/host_filesystem.hpp>
#include <capsule/vfs/vfs.hpp>
#include <winapi/kernel32/error.hpp>
#include <winapi/core/marshaler.hpp>
#include <core/memory/guest_memory.hpp>
#include <core/cpu/state/cpu_state.hpp>
#include <core/pe/loader/teb_setup.hpp>
#include <winapi/kernel32/system/kuser_shared_data.hpp>

#include <algorithm>
#include <array>
#include <string>
#include <thread>
#include <chrono>

using namespace vitreo;
using namespace vitreo::winapi;

// =============================================================================
// Test Fixture
// =============================================================================

class SysInfoTest : public ::testing::Test {
protected:
    SysInfoManager mgr;
};

class OpticalTestFileSystem final : public capsule::HostFileSystem {
public:
    explicit OpticalTestFileSystem(std::string root)
        : HostFileSystem(std::move(root)) {}
    bool is_optical() const noexcept override { return true; }
};

TEST_F(SysInfoTest, DriveQueriesReflectMountedFilesystems) {
    capsule::Vfs vfs;
    ASSERT_TRUE(vfs.mount_drive('C',
        std::make_unique<capsule::HostFileSystem>("/tmp")).is_ok());
    ASSERT_TRUE(vfs.mount_drive('E',
        std::make_unique<OpticalTestFileSystem>("/tmp")).is_ok());
    mgr.set_vfs(&vfs);

    EXPECT_EQ(mgr.get_logical_drives(), (1u << 2) | (1u << 4));
    EXPECT_EQ(mgr.get_drive_type_w("C:\\"), 3u);
    EXPECT_EQ(mgr.get_drive_type_w("E:\\"), 5u);
}

TEST_F(SysInfoTest, GlobalMemoryStatusExReportsRemainingUserVirtual) {
    // Learn MEMORYSTATUSEX: ullTotalVirtual is this process's user-mode VA;
    // ullAvailVirtual is the unreserved remainder. The old path wrote
    // 0xFFFE0000 for both (a 4 GB empty window) regardless of mappings.
    GuestMemory memory;
    constexpr GuestAddr kCeiling = 0x7FFEFFFFu;
    constexpr GuestAddr kBuf = 0x1000;
    memory.set_maximum_user_address(kCeiling);
    ASSERT_TRUE(memory.map_region(
        kBuf, 0x10000, MemoryProtection::ReadWrite).is_ok());
    SysInfoManager local{&memory};
    cpu::CpuState cpu;
    cpu.init_protected_mode();
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    ASSERT_TRUE(memory.write_u32(kBuf, 64).is_ok());

    ASSERT_TRUE(local.global_memory_status_ex(kBuf, marshaler));
    // A size, not the top address: the user region runs from 0x10000 to the
    // ceiling inclusive (0x7FFE0000 for a non-LAA 32-bit process).
    constexpr u64 kTotal = u64{kCeiling} - 0x10000u + 1u;
    EXPECT_EQ(memory.read_u64(kBuf + 40).value_or(0), kTotal);
    EXPECT_EQ(memory.read_u64(kBuf + 48).value_or(0),
              kTotal - memory.total_mapped_size());
    EXPECT_EQ(memory.read_u64(kBuf + 56).value_or(1), 0u);
    EXPECT_NE(memory.read_u64(kBuf + 40).value_or(0), 0xFFFE0000ull);
    EXPECT_NE(memory.read_u64(kBuf + 48).value_or(0), 0xFFFE0000ull);
}

TEST_F(SysInfoTest, VolumeAdvertisesPersistentAclMetadata) {
    // A NULL root means the current drive's root, which must be mounted:
    // an unmounted letter fails ERROR_PATH_NOT_FOUND (files_ns_probe).
    capsule::Vfs vfs;
    ASSERT_TRUE(vfs.mount_drive('C',
        std::make_unique<capsule::HostFileSystem>("/tmp")).is_ok());
    mgr.set_vfs(&vfs);
    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(
        0x1000, 0x1000, MemoryProtection::ReadWrite).is_ok());
    Marshaler marshaler(memory);

    ASSERT_TRUE(mgr.get_volume_information_w(
        0, 0, 0, 0, 0, 0x1000, 0, 0, marshaler));
    EXPECT_EQ(memory.read_u32(0x1000).value() & 0x8u, 0x8u);
}

// =============================================================================
// GetSystemInfo Tests
// =============================================================================

TEST_F(SysInfoTest, GetSystemInfo_PageSize4096) {
    const auto& info = mgr.get_system_info();
    EXPECT_EQ(info.page_size, 4096u);
}

TEST_F(SysInfoTest, GetSystemInfo_AMD64Architecture) {
    const auto& info = mgr.get_system_info();
    EXPECT_EQ(info.processor_architecture, 9u);  // PROCESSOR_ARCHITECTURE_AMD64
}

TEST_F(SysInfoTest, GetSystemInfo_UsesHostProcessorCount) {
    const auto& info = mgr.get_system_info();
    const u32 host_count = std::thread::hardware_concurrency();
    EXPECT_EQ(info.number_of_processors, host_count == 0 ? 1u : host_count);
}

TEST_F(SysInfoTest, GetSystemInfo_AllocationGranularity65536) {
    const auto& info = mgr.get_system_info();
    EXPECT_EQ(info.allocation_granularity, 65536u);
}

TEST(SysInfoAddressLimitTest, GetSystemInfoHonorsGuestMemoryCeiling) {
    GuestMemory memory;
    constexpr GuestAddr kLinear64Ceiling = 0x0000007F'FFFFFFFFULL;
    memory.set_maximum_user_address(kLinear64Ceiling);
    SysInfoManager limited{&memory};

    EXPECT_EQ(limited.get_system_info().max_app_address, kLinear64Ceiling);

    constexpr GuestAddr kInfo = 0x1000;
    ASSERT_TRUE(memory.map_region(
        kInfo, 0x1000, MemoryProtection::ReadWrite).is_ok());
    cpu::CpuState cpu;
    cpu.init_long_mode();
    Marshaler marshaler{memory};
    marshaler.attach_cpu(cpu);
    limited.get_native_system_info(kInfo, marshaler);
    ASSERT_TRUE(memory.read_u64(kInfo + 16).is_ok());
    EXPECT_EQ(memory.read_u64(kInfo + 16).value(), kLinear64Ceiling);
}

// =============================================================================
// GetVersionEx Tests
// =============================================================================

TEST_F(SysInfoTest, GetVersionEx_UnmanifestedCallerReportsWindows8) {
    OsVersionInfo ver;
    bool ok = mgr.get_version_ex(148, ver);
    EXPECT_TRUE(ok);
    EXPECT_EQ(ver.major_version, 6u);
    EXPECT_EQ(ver.minor_version, 2u);
    EXPECT_EQ(ver.build_number, 9200u);
}

TEST_F(SysInfoTest, GetVersionEx_Windows10ManifestReportsWindows10) {
    mgr.set_windows_10_compatible(true);
    OsVersionInfo ver;
    bool ok = mgr.get_version_ex(148, ver);
    EXPECT_TRUE(ok);
    EXPECT_EQ(ver.major_version, 10u);
    EXPECT_EQ(ver.minor_version, 0u);
    EXPECT_EQ(ver.build_number, 19041u);
}

TEST_F(SysInfoTest, GetVersionExAInitializesExtendedVersionFields) {
    constexpr GuestAddr output = 0x1000;
    constexpr GuestAddr stack = 0x3000;
    constexpr u32 os_version_info_ex_a_size = 156;
    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(
        output, 0x1000, MemoryProtection::ReadWrite).is_ok());
    ASSERT_TRUE(memory.map_region(
        stack, 0x1000, MemoryProtection::ReadWrite).is_ok());
    cpu::CpuState cpu;
    cpu.init_protected_mode();
    cpu.gpr.rsp = stack + 0x800;
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);

    std::array<Byte, os_version_info_ex_a_size> fill;
    fill.fill(static_cast<Byte>(0xCC));
    ASSERT_TRUE(memory.write_bytes(output, fill.data(), fill.size()).is_ok());
    ASSERT_TRUE(memory.write_u32(output, os_version_info_ex_a_size).is_ok());
    ASSERT_TRUE(memory.write_u32(cpu.gpr.rsp + 4, output).is_ok());

    auto exports = create_sysinfo_thunks(mgr);
    const auto entry = std::find_if(
        exports.entries.begin(), exports.entries.end(),
        [](const ExportEntry& candidate) {
            return candidate.name == "GetVersionExA";
        });
    ASSERT_NE(entry, exports.entries.end());
    entry->handler(marshaler);

    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 1u);
    EXPECT_EQ(memory.read_u8(output + 20).value(), 0u);
    EXPECT_EQ(memory.read_u16(output + 148).value(), 0u);
    EXPECT_EQ(memory.read_u16(output + 150).value(), 0u);
    EXPECT_EQ(memory.read_u16(output + 152).value(), 0x0100u);
    EXPECT_EQ(memory.read_u8(output + 154).value(), 1u);
    EXPECT_EQ(memory.read_u8(output + 155).value(), 0u);
}

// =============================================================================
// GetSystemTimeAsFileTime Tests
// =============================================================================

TEST_F(SysInfoTest, GetSystemTimeAsFileTime_NonZero) {
    u64 ft = mgr.get_system_time_as_filetime();
    EXPECT_GT(ft, 0u);
}

TEST_F(SysInfoTest, GetSystemTimeAsFileTime_Advances) {
    u64 ft1 = mgr.get_system_time_as_filetime();
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    u64 ft2 = mgr.get_system_time_as_filetime();
    EXPECT_GE(ft2, ft1);
}

// =============================================================================
// QueryPerformanceFrequency / Counter Tests
// =============================================================================

TEST_F(SysInfoTest, QueryPerformanceFrequency_NonZero) {
    u64 freq = mgr.query_performance_frequency();
    EXPECT_GT(freq, 0u);
}

// 10 MHz is what modern Windows reports, and matching it is a compatibility
// requirement rather than a cosmetic choice: at the previous 1 GHz a QPC delta
// crossed INT32_MAX in ~2.1 seconds, so a 32-bit guest holding a delta (or an
// intermediate such as `delta * 1000`) in a 32-bit integer overflowed here and
// never would on Windows.
TEST_F(SysInfoTest, QueryPerformanceFrequency_MatchesWindowsTenMegahertz) {
    EXPECT_EQ(mgr.query_performance_frequency(), 10'000'000ULL);
    EXPECT_EQ(mgr.query_performance_frequency(), kQpcHz);
}

// QPC and RDTSC must stay on one timebase while running at different rates,
// exactly as real hardware pairs a 10 MHz QPF with a GHz-class TSC.
TEST_F(SysInfoTest, VirtualTscRunsFasterThanQpcOnTheSameTimebase) {
    const u64 qpc_before = virtual_qpc_ticks();
    const u64 tsc_before = virtual_tsc_ticks();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    const u64 qpc_elapsed = virtual_qpc_ticks() - qpc_before;
    const u64 tsc_elapsed = virtual_tsc_ticks() - tsc_before;
    ASSERT_GT(qpc_elapsed, 0u);

    const double qpc_seconds =
        static_cast<double>(qpc_elapsed) / static_cast<double>(kQpcHz);
    const double tsc_seconds =
        static_cast<double>(tsc_elapsed) / static_cast<double>(guest_tsc_hz());
    // Same elapsed wall time measured through both scalings.
    EXPECT_NEAR(qpc_seconds, tsc_seconds, 0.005);
}

TEST_F(SysInfoTest, QueryPerformanceCounter_Advances) {
    u64 c1 = mgr.query_performance_counter();
    // Small busy-wait to ensure counter advances
    std::this_thread::sleep_for(std::chrono::microseconds(10));
    u64 c2 = mgr.query_performance_counter();
    EXPECT_GT(c2, c1);
}

TEST_F(SysInfoTest, QueryPerformanceCounter_RateMatchesFrequency) {
    const u64 freq = mgr.query_performance_frequency();
    const u64 c1 = mgr.query_performance_counter();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    const u64 c2 = mgr.query_performance_counter();
    ASSERT_GT(c2, c1);
    const double ms =
        static_cast<double>(c2 - c1) * 1000.0 / static_cast<double>(freq);
    EXPECT_GE(ms, 30.0);
    EXPECT_LE(ms, 200.0);
}

TEST_F(SysInfoTest, QueryUnbiasedInterruptTimePreciseUses100nsAndAdvances) {
    const u64 t1 = mgr.query_unbiased_interrupt_time_precise();
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    const u64 t2 = mgr.query_unbiased_interrupt_time_precise();
    EXPECT_GT(t2, t1);
    EXPECT_GE(t2 - t1, 1'000u);
}

TEST_F(SysInfoTest, QueryUnbiasedInterruptTimePreciseThunkWritesOutput) {
    constexpr GuestAddr output = 0x1000;
    constexpr GuestAddr stack = 0x3000;
    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(
        output, 0x1000, MemoryProtection::ReadWrite).is_ok());
    ASSERT_TRUE(memory.map_region(
        stack, 0x1000, MemoryProtection::ReadWrite).is_ok());
    cpu::CpuState cpu;
    cpu.init_protected_mode();
    cpu.gpr.rsp = stack + 0x800;
    ASSERT_TRUE(memory.write_u32(cpu.gpr.rsp + 4,
                                 static_cast<u32>(output)).is_ok());
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);

    auto exports = create_sysinfo_thunks(mgr);
    const auto entry = std::find_if(
        exports.entries.begin(), exports.entries.end(),
        [](const ExportEntry& candidate) {
            return candidate.name == "QueryUnbiasedInterruptTimePrecise";
        });
    ASSERT_NE(entry, exports.entries.end());
    entry->handler(marshaler);
    EXPECT_GT(memory.read_u64(output).value(), 0u);
}

// QueryInterruptTime / QueryInterruptTimePrecise were missing KernelBase
// exports (Media Foundation imports them). Measured on Windows 11
// (s4_interrupt_time_probe): VOID fn(PULONGLONG) in 100 ns units; the coarse
// value / 10000 tracks GetTickCount64; precise - coarse stays under a tick;
// interrupt time >= unbiased interrupt time; precise advances across Sleep.
TEST_F(SysInfoTest, QueryInterruptTimeAndPreciseWrite100nsInterruptTime) {
    constexpr GuestAddr output = 0x1000;
    constexpr GuestAddr stack = 0x3000;
    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(output, 0x1000, MemoryProtection::ReadWrite).is_ok());
    ASSERT_TRUE(memory.map_region(stack, 0x1000, MemoryProtection::ReadWrite).is_ok());
    cpu::CpuState cpu;
    cpu.init_protected_mode();
    cpu.gpr.rsp = stack + 0x800;
    ASSERT_TRUE(memory.write_u32(cpu.gpr.rsp + 4, static_cast<u32>(output)).is_ok());
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    auto exports = create_sysinfo_thunks(mgr);
    const auto find = [&](std::string_view name) {
        auto it = std::find_if(exports.entries.begin(), exports.entries.end(),
                               [&](const ExportEntry& e) { return e.name == name; });
        EXPECT_NE(it, exports.entries.end()) << name;
        return it;
    };
    const auto coarse = find("QueryInterruptTime");
    const auto precise = find("QueryInterruptTimePrecise");
    ASSERT_NE(coarse, exports.entries.end());
    ASSERT_NE(precise, exports.entries.end());
    EXPECT_EQ(coarse->stdcall_bytes, 4u);
    EXPECT_EQ(precise->stdcall_bytes, 4u);

    const u64 tick_before = tick_count_ms();
    coarse->handler(marshaler);
    const u64 c = memory.read_u64(output).value();
    precise->handler(marshaler);
    const u64 p1 = memory.read_u64(output).value();
    const u64 unbiased = mgr.query_unbiased_interrupt_time_precise();
    EXPECT_GE(c / 10000, tick_before);
    EXPECT_LE(c / 10000, tick_count_ms());
    EXPECT_GE(p1, c);
    EXPECT_LT(p1 - c, 200'000u);   // under one 15.6 ms tick (and 1 ms here)
    EXPECT_GE(p1 + 10, unbiased);  // interrupt time also counts sleep
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    precise->handler(marshaler);
    const u64 p2 = memory.read_u64(output).value();
    EXPECT_GE(p2 - p1, 290'000u);
}

TEST_F(SysInfoTest, QueryUnbiasedInterruptTimeThunkWritesOutputAndReturnsTrue) {
    constexpr GuestAddr output = 0x1000;
    constexpr GuestAddr stack = 0x3000;
    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(
        output, 0x1000, MemoryProtection::ReadWrite).is_ok());
    ASSERT_TRUE(memory.map_region(
        stack, 0x1000, MemoryProtection::ReadWrite).is_ok());
    cpu::CpuState cpu;
    cpu.init_protected_mode();
    cpu.gpr.rsp = stack + 0x800;
    ASSERT_TRUE(memory.write_u32(cpu.gpr.rsp + 4,
                                 static_cast<u32>(output)).is_ok());
    ASSERT_TRUE(memory.write_u64(output, 0).is_ok());
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);

    auto exports = create_sysinfo_thunks(mgr);
    const auto entry = std::find_if(
        exports.entries.begin(), exports.entries.end(),
        [](const ExportEntry& candidate) {
            return candidate.name == "QueryUnbiasedInterruptTime";
        });
    ASSERT_NE(entry, exports.entries.end());
    entry->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 1u);
    EXPECT_GT(memory.read_u64(output).value(), 0u);
}

TEST_F(SysInfoTest, GetSystemFirmwareTableExposesRawVirtualSmbios) {
    constexpr GuestAddr output = 0x1000;
    constexpr GuestAddr stack = 0x3000;
    constexpr u32 rsmb = 0x52534D42;  // 'RSMB'
    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(
        output, 0x1000, MemoryProtection::ReadWrite).is_ok());
    ASSERT_TRUE(memory.map_region(
        stack, 0x1000, MemoryProtection::ReadWrite).is_ok());
    cpu::CpuState cpu;
    cpu.init_protected_mode();
    cpu.gpr.rsp = stack + 0x800;
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);

    auto exports = create_sysinfo_thunks(mgr);
    const auto entry = std::find_if(
        exports.entries.begin(), exports.entries.end(),
        [](const ExportEntry& candidate) {
            return candidate.name == "GetSystemFirmwareTable";
        });
    ASSERT_NE(entry, exports.entries.end());

    ASSERT_TRUE(memory.write_u32(cpu.gpr.rsp + 4, rsmb).is_ok());
    ASSERT_TRUE(memory.write_u32(cpu.gpr.rsp + 8, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(cpu.gpr.rsp + 12, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(cpu.gpr.rsp + 16, 0).is_ok());
    entry->handler(marshaler);
    const u32 required = static_cast<u32>(cpu.gpr.rax);
    ASSERT_GT(required, 8u);

    ASSERT_TRUE(memory.write_u32(
        cpu.gpr.rsp + 12, static_cast<u32>(output)).is_ok());
    ASSERT_TRUE(memory.write_u32(cpu.gpr.rsp + 16, required).is_ok());
    entry->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), required);

    std::array<u8, 8> header{};
    ASSERT_TRUE(memory.read_bytes(
        output, header.data(), header.size()).is_ok());
    EXPECT_EQ(header[0], 0u);
    EXPECT_EQ(header[1], 3u);   // SMBIOS 3.6
    EXPECT_EQ(header[2], 6u);
    EXPECT_EQ(header[3], 0u);   // DmiRevision, as Windows reports it
    const u32 table_length =
        static_cast<u32>(header[4]) |
        (static_cast<u32>(header[5]) << 8) |
        (static_cast<u32>(header[6]) << 16) |
        (static_cast<u32>(header[7]) << 24);
    EXPECT_EQ(table_length + header.size(), required);

    std::array<char, 4096> table{};
    ASSERT_LE(table_length, table.size());
    ASSERT_TRUE(memory.read_bytes(
        output + header.size(), reinterpret_cast<u8*>(table.data()),
        table_length).is_ok());
    const std::string table_bytes(table.data(), table_length);
    EXPECT_TRUE(
        table_bytes.find("Apple Inc.") != std::string::npos ||
        table_bytes.find("Vitreo") != std::string::npos);
    EXPECT_EQ(table_bytes.find("VITREO-0001"), std::string::npos);
    EXPECT_EQ(table_bytes.find("VITREO-MB-0001"), std::string::npos);
}

TEST_F(SysInfoTest, GetSystemFirmwareTableReportsShortBuffer) {
    constexpr GuestAddr output = 0x1000;
    constexpr GuestAddr stack = 0x3000;
    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(
        output, 0x1000, MemoryProtection::ReadWrite).is_ok());
    ASSERT_TRUE(memory.map_region(
        stack, 0x1000, MemoryProtection::ReadWrite).is_ok());
    cpu::CpuState cpu;
    cpu.init_protected_mode();
    cpu.gpr.rsp = stack + 0x800;
    ASSERT_TRUE(memory.write_u32(
        cpu.gpr.rsp + 4, 0x52534D42).is_ok());
    ASSERT_TRUE(memory.write_u32(cpu.gpr.rsp + 8, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(
        cpu.gpr.rsp + 12, static_cast<u32>(output)).is_ok());
    ASSERT_TRUE(memory.write_u32(cpu.gpr.rsp + 16, 4).is_ok());
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);

    auto exports = create_sysinfo_thunks(mgr);
    const auto entry = std::find_if(
        exports.entries.begin(), exports.entries.end(),
        [](const ExportEntry& candidate) {
            return candidate.name == "GetSystemFirmwareTable";
        });
    ASSERT_NE(entry, exports.entries.end());
    entry->handler(marshaler);
    EXPECT_GT(static_cast<u32>(cpu.gpr.rax), 4u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INSUFFICIENT_BUFFER);
}

// =============================================================================
// IsProcessorFeaturePresent Tests
// =============================================================================

TEST_F(SysInfoTest, IsProcessorFeaturePresent_SSE_True) {
    EXPECT_TRUE(mgr.is_processor_feature_present(pf::PF_XMMI_INSTRUCTIONS_AVAILABLE));
}

TEST_F(SysInfoTest, IsProcessorFeaturePresent_SSE2_True) {
    EXPECT_TRUE(mgr.is_processor_feature_present(pf::PF_XMMI64_INSTRUCTIONS_AVAILABLE));
}

TEST_F(SysInfoTest, IsProcessorFeaturePresent_Unknown_False) {
    EXPECT_FALSE(mgr.is_processor_feature_present(9999));
}

TEST_F(SysInfoTest, IsProcessorFeaturePresent_Fastfail_True) {
    EXPECT_TRUE(mgr.is_processor_feature_present(pf::PF_FASTFAIL_AVAILABLE));
}

TEST_F(SysInfoTest, IsProcessorFeaturePresent_RdtscAndPae_True) {
    EXPECT_TRUE(mgr.is_processor_feature_present(pf::PF_RDTSC_INSTRUCTION_AVAILABLE));
    EXPECT_TRUE(mgr.is_processor_feature_present(pf::PF_PAE_ENABLED));
}

// Audit 01 IsProcessorFeaturePresent #1-#3. Windows derives each PF from
// CPUID (tools/oracle_probe/processor_features_probe.expected: every rule
// line reads eq-cpuid on x64 and WOW64). Vitreo's virtual CPU advertises
// XSAVE/OSXSAVE, SSSE3, SSE4.1, SSE4.2, AVX, BMI2 in both modes and AVX2 in
// long mode only (core/cpu/cpuid_synth.hpp), and no FSGSBASE / VMX.
TEST_F(SysInfoTest, IsProcessorFeaturePresent_FollowsVirtualCpuid) {
    for (const bool long_mode : {true, false}) {
        SCOPED_TRACE(long_mode ? "x64" : "x86");
        for (const u32 f : {pf::PF_XSAVE_ENABLED, pf::PF_SSSE3_INSTRUCTIONS_AVAILABLE,
                            pf::PF_SSE4_1_INSTRUCTIONS_AVAILABLE,
                            pf::PF_SSE4_2_INSTRUCTIONS_AVAILABLE,
                            pf::PF_AVX_INSTRUCTIONS_AVAILABLE,
                            pf::PF_BMI2_INSTRUCTIONS_AVAILABLE,
                            pf::PF_COMPARE_EXCHANGE128, pf::PF_SSE3_INSTRUCTIONS_AVAILABLE,
                            pf::PF_MMX_INSTRUCTIONS_AVAILABLE}) {
            EXPECT_TRUE(mgr.is_processor_feature_present(f, long_mode)) << f;
        }
        EXPECT_EQ(mgr.is_processor_feature_present(
                      pf::PF_AVX2_INSTRUCTIONS_AVAILABLE, long_mode),
                  long_mode);
        // Not advertised by CPUID: FSGSBASE (7.EBX[0]), VMX (1.ECX[5]),
        // RDRAND, RDTSCP, ERMS, AVX-512F, 3DNow!.
        for (const u32 f : {pf::PF_RDWRFSGSBASE_AVAILABLE,
                            pf::PF_SECOND_LEVEL_ADDRESS_TRANSLATION,
                            pf::PF_VIRT_FIRMWARE_ENABLED,
                            pf::PF_RDRAND_INSTRUCTION_AVAILABLE,
                            pf::PF_RDTSCP_INSTRUCTION_AVAILABLE,
                            pf::PF_ERMS_AVAILABLE,
                            pf::PF_AVX512F_INSTRUCTIONS_AVAILABLE,
                            pf::PF_3DNOW_INSTRUCTIONS_AVAILABLE}) {
            EXPECT_FALSE(mgr.is_processor_feature_present(f, long_mode)) << f;
        }
        // Constant on Windows 10/11 (probe: PF 9/12/23 value=1, PF 11 value=0,
        // every ARM/PPC/Alpha index value=0).
        EXPECT_TRUE(mgr.is_processor_feature_present(pf::PF_NX_ENABLED, long_mode));
        EXPECT_TRUE(mgr.is_processor_feature_present(pf::PF_FASTFAIL_AVAILABLE, long_mode));
        EXPECT_FALSE(mgr.is_processor_feature_present(pf::PF_SSE_DAZ_MODE_AVAILABLE, long_mode));
        for (const u32 f : {0u, 1u, 4u, 5u, 15u, 16u, 18u, 19u, 24u, 29u, 34u, 43u,
                            59u, 62u, 63u, 64u, 88u, 127u}) {
            EXPECT_FALSE(mgr.is_processor_feature_present(f, long_mode)) << f;
        }
    }
}

// KUSER_SHARED_DATA.ProcessorFeatures[64] must equal the API for every index
// (probe .expected: "ksd-eq" on every line below 64).
TEST(KuserSharedDataTest, ProcessorFeaturesMatchIsProcessorFeaturePresent) {
    for (const bool long_mode : {true, false}) {
        GuestMemory mem;
        ASSERT_TRUE(install_kuser_shared_data(mem, long_mode));
        for (u32 f = 0; f < pf::PROCESSOR_FEATURE_MAX; ++f) {
            const u8 v = mem.read_u8(pe::SHARED_USER_DATA_ADDRESS + 0x274 + f).value_or(0xFF);
            EXPECT_EQ(v, processor_feature_present_for_cpu(f, long_mode) ? 1 : 0)
                << "PF " << f << (long_mode ? " x64" : " x86");
        }
    }
}

// =============================================================================
// GetComputerName Tests (WIN32-088)
// =============================================================================

class ComputerNameTest : public ::testing::Test {
protected:
    static constexpr GuestAddr BUF_ADDR  = 0x1000;
    static constexpr GuestAddr SIZE_ADDR = 0x2000;

    void SetUp() override {
        auto r1 = mem.map_region(0x1000, 0x1000, MemoryProtection::ReadWrite);
        ASSERT_TRUE(r1.is_ok());
        auto r2 = mem.map_region(0x2000, 0x100, MemoryProtection::ReadWrite);
        ASSERT_TRUE(r2.is_ok());
    }

    SysInfoManager mgr;
    GuestMemory mem;
};

TEST_F(ComputerNameTest, GetComputerNameA_Default) {
    Marshaler m(mem);
    (void)m.write_u32(SIZE_ADDR, 64);
    bool ok = mgr.get_computer_name_a(BUF_ADDR, SIZE_ADDR, m);
    EXPECT_TRUE(ok);

    auto name = m.read_ansi_string(BUF_ADDR);
    ASSERT_TRUE(name.is_ok());
    EXPECT_EQ(name.value(), "VITREO");

    auto sz = m.read_u32(SIZE_ADDR);
    ASSERT_TRUE(sz.is_ok());
    EXPECT_EQ(sz.value(), 6u);  // strlen("VITREO")
}

TEST_F(ComputerNameTest, GetComputerNameW_Default) {
    Marshaler m(mem);
    (void)m.write_u32(SIZE_ADDR, 64);
    bool ok = mgr.get_computer_name_w(BUF_ADDR, SIZE_ADDR, m);
    EXPECT_TRUE(ok);

    auto name = m.read_wide_string(BUF_ADDR);
    ASSERT_TRUE(name.is_ok());
    EXPECT_EQ(name.value(), "VITREO");

    auto sz = m.read_u32(SIZE_ADDR);
    ASSERT_TRUE(sz.is_ok());
    EXPECT_EQ(sz.value(), 6u);
}

TEST_F(ComputerNameTest, GetComputerNameA_BufferTooSmall) {
    Marshaler m(mem);
    (void)m.write_u32(SIZE_ADDR, 3);  // too small for "VITREO" + null
    bool ok = mgr.get_computer_name_a(BUF_ADDR, SIZE_ADDR, m);
    EXPECT_FALSE(ok);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_BUFFER_OVERFLOW);

    // size_ptr updated to required size
    auto sz = m.read_u32(SIZE_ADDR);
    ASSERT_TRUE(sz.is_ok());
    EXPECT_EQ(sz.value(), 7u);  // strlen("VITREO") + 1
}

TEST_F(ComputerNameTest, GetComputerNameA_ExactFit) {
    Marshaler m(mem);
    (void)m.write_u32(SIZE_ADDR, 7);  // exactly name_len + 1
    bool ok = mgr.get_computer_name_a(BUF_ADDR, SIZE_ADDR, m);
    EXPECT_TRUE(ok);

    auto name = m.read_ansi_string(BUF_ADDR);
    ASSERT_TRUE(name.is_ok());
    EXPECT_EQ(name.value(), "VITREO");

    auto sz = m.read_u32(SIZE_ADDR);
    ASSERT_TRUE(sz.is_ok());
    EXPECT_EQ(sz.value(), 6u);
}

TEST_F(ComputerNameTest, GetComputerNameA_CustomName) {
    mgr.set_computer_name("MYPC");
    Marshaler m(mem);
    (void)m.write_u32(SIZE_ADDR, 64);
    bool ok = mgr.get_computer_name_a(BUF_ADDR, SIZE_ADDR, m);
    EXPECT_TRUE(ok);

    auto name = m.read_ansi_string(BUF_ADDR);
    ASSERT_TRUE(name.is_ok());
    EXPECT_EQ(name.value(), "MYPC");

    auto sz = m.read_u32(SIZE_ADDR);
    ASSERT_TRUE(sz.is_ok());
    EXPECT_EQ(sz.value(), 4u);
}

TEST_F(ComputerNameTest, GetComputerNameA_NullBuffer) {
    Marshaler m(mem);
    bool ok = mgr.get_computer_name_a(0, SIZE_ADDR, m);
    EXPECT_FALSE(ok);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_PARAMETER);
}

TEST_F(ComputerNameTest, GetComputerNameW_BufferTooSmall) {
    Marshaler m(mem);
    (void)m.write_u32(SIZE_ADDR, 2);  // too small
    bool ok = mgr.get_computer_name_w(BUF_ADDR, SIZE_ADDR, m);
    EXPECT_FALSE(ok);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_BUFFER_OVERFLOW);

    auto sz = m.read_u32(SIZE_ADDR);
    ASSERT_TRUE(sz.is_ok());
    EXPECT_EQ(sz.value(), 7u);
}

namespace {

const ExportEntry* find_export(const DllExports& exports, const char* name) {
    const auto it = std::find_if(
        exports.entries.begin(), exports.entries.end(),
        [name](const ExportEntry& candidate) {
            return candidate.name == name;
        });
    return it == exports.entries.end() ? nullptr : &*it;
}

} // namespace

TEST_F(SysInfoTest, Wow64ProcessReportsTrueFor32BitOnAmd64) {
    constexpr GuestAddr out = 0x1000;
    constexpr GuestAddr stack = 0x3000;
    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(
        out, 0x1000, MemoryProtection::ReadWrite).is_ok());
    ASSERT_TRUE(memory.map_region(
        stack, 0x1000, MemoryProtection::ReadWrite).is_ok());
    cpu::CpuState cpu;
    cpu.init_protected_mode();
    cpu.gpr.rsp = stack + 0x800;
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    ASSERT_TRUE(memory.write_u32(cpu.gpr.rsp + 4, 0xFFFFFFFFu).is_ok());
    ASSERT_TRUE(memory.write_u32(cpu.gpr.rsp + 8, out).is_ok());

    const auto exports = create_sysinfo_thunks(mgr);
    const auto* entry = find_export(exports, "IsWow64Process");
    ASSERT_NE(entry, nullptr);
    entry->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 1u);
    EXPECT_EQ(memory.read_u32(out).value(), 1u);
}

TEST_F(SysInfoTest, Wow64ProcessReportsFalseFor64Bit) {
    constexpr GuestAddr out = 0x1000;
    constexpr GuestAddr stack = 0x3000;
    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(
        out, 0x1000, MemoryProtection::ReadWrite).is_ok());
    ASSERT_TRUE(memory.map_region(
        stack, 0x1000, MemoryProtection::ReadWrite).is_ok());
    cpu::CpuState cpu;
    cpu.init_long_mode();
    cpu.gpr.rsp = stack + 0x800;
    cpu.gpr.rcx = 0xFFFFFFFFFFFFFFFFull;
    cpu.gpr.rdx = out;
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);

    const auto exports = create_sysinfo_thunks(mgr);
    const auto* entry = find_export(exports, "IsWow64Process");
    ASSERT_NE(entry, nullptr);
    entry->handler(marshaler);
    EXPECT_EQ(cpu.gpr.rax, 1u);
    EXPECT_EQ(memory.read_u32(out).value(), 0u);
}

TEST_F(SysInfoTest, Wow64Process2ReportsI386GuestOnAmd64Host) {
    constexpr GuestAddr proc = 0x1000;
    constexpr GuestAddr native = 0x1004;
    constexpr GuestAddr stack = 0x3000;
    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(
        proc, 0x1000, MemoryProtection::ReadWrite).is_ok());
    ASSERT_TRUE(memory.map_region(
        stack, 0x1000, MemoryProtection::ReadWrite).is_ok());
    cpu::CpuState cpu;
    cpu.init_protected_mode();
    cpu.gpr.rsp = stack + 0x800;
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    ASSERT_TRUE(memory.write_u32(cpu.gpr.rsp + 4, 0xFFFFFFFFu).is_ok());
    ASSERT_TRUE(memory.write_u32(cpu.gpr.rsp + 8, proc).is_ok());
    ASSERT_TRUE(memory.write_u32(cpu.gpr.rsp + 12, native).is_ok());

    const auto exports = create_sysinfo_thunks(mgr);
    const auto* entry = find_export(exports, "IsWow64Process2");
    ASSERT_NE(entry, nullptr);
    entry->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 1u);
    EXPECT_EQ(memory.read_u16(proc).value(), 0x014cu);
    EXPECT_EQ(memory.read_u16(native).value(), 0x8664u);
}

TEST_F(SysInfoTest, GetSystemInfoWow64ViewIsIntelNativeIsAmd64) {
    constexpr GuestAddr wow_info = 0x1000;
    constexpr GuestAddr native_info = 0x1100;
    constexpr GuestAddr stack = 0x3000;
    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(
        wow_info, 0x1000, MemoryProtection::ReadWrite).is_ok());
    ASSERT_TRUE(memory.map_region(
        stack, 0x1000, MemoryProtection::ReadWrite).is_ok());
    cpu::CpuState cpu;
    cpu.init_protected_mode();
    cpu.gpr.rsp = stack + 0x800;
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    ASSERT_TRUE(memory.write_u32(cpu.gpr.rsp + 4, wow_info).is_ok());

    const auto exports = create_sysinfo_thunks(mgr);
    const auto* get_si = find_export(exports, "GetSystemInfo");
    ASSERT_NE(get_si, nullptr);
    get_si->handler(marshaler);
    EXPECT_EQ(memory.read_u16(wow_info).value(), 0u);

    ASSERT_TRUE(memory.write_u32(cpu.gpr.rsp + 4, native_info).is_ok());
    const auto* get_native = find_export(exports, "GetNativeSystemInfo");
    ASSERT_NE(get_native, nullptr);
    get_native->handler(marshaler);
    EXPECT_EQ(memory.read_u16(native_info).value(), 9u);
}
