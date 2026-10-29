// SPDX-License-Identifier: LGPL-2.1-only
/// @file test_ntdll_ext.cpp
/// @brief Unit tests for ntdll extended functions (WIN32-086): NTSTATUS mapping, version, NtClose, delay, Rtl memory helpers, system information

#include "ntdll_ext_test_fixture.hpp"

// =============================================================================
// RtlNtStatusToDosError Tests
// =============================================================================

TEST_F(NtdllExtTest, StatusToDosError_Success) {
    EXPECT_EQ(mgr.nt_status_to_dos_error(nt_status::STATUS_SUCCESS), 0u);
}

TEST_F(NtdllExtTest, StatusToDosError_InvalidHandle) {
    EXPECT_EQ(mgr.nt_status_to_dos_error(nt_status::STATUS_INVALID_HANDLE), 6u);
}

TEST_F(NtdllExtTest, StatusToDosError_AccessDenied) {
    EXPECT_EQ(mgr.nt_status_to_dos_error(nt_status::STATUS_ACCESS_DENIED), 5u);
}

TEST_F(NtdllExtTest, StatusToDosError_Unknown) {
    EXPECT_EQ(mgr.nt_status_to_dos_error(0xDEADBEEF), 317u);
}

TEST_F(NtdllExtTest, StatusToDosError_BrokenPipe) {
    EXPECT_EQ(mgr.nt_status_to_dos_error(0xC000014Bu), 109u);
}

TEST_F(NtdllExtTest, StatusToDosError_UnwrapsWin32Facility) {
    EXPECT_EQ(mgr.nt_status_to_dos_error(0x8007006Du), 109u);
    EXPECT_EQ(mgr.nt_status_to_dos_error(0xC007006Du), 109u);
    EXPECT_EQ(mgr.nt_status_to_dos_error(0x800703E3u), 995u);
    EXPECT_EQ(mgr.nt_status_to_dos_error(0xC0070005u), 5u);
}

TEST_F(NtdllExtTest, StatusToDosError_Cancelled) {
    EXPECT_EQ(mgr.nt_status_to_dos_error(0xC0000120u), 995u);
}

TEST_F(NtdllExtTest, StatusToDosError_InvalidParameter) {
    EXPECT_EQ(mgr.nt_status_to_dos_error(nt_status::STATUS_INVALID_PARAMETER), 87u);
}

TEST_F(NtdllExtTest, StatusToDosError_ObjectNameNotFound) {
    EXPECT_EQ(mgr.nt_status_to_dos_error(nt_status::STATUS_OBJECT_NAME_NOT_FOUND), 2u);
}

TEST_F(NtdllExtTest, StatusToDosError_NoMemory) {
    EXPECT_EQ(mgr.nt_status_to_dos_error(nt_status::STATUS_NO_MEMORY), 8u);
}

// =============================================================================
// RtlGetVersion Tests
// =============================================================================

TEST_F(NtdllExtTest, GetVersion_Major) {
    auto ver = mgr.get_version();
    EXPECT_EQ(ver.major, 10u);
}

TEST_F(NtdllExtTest, GetVersion_Build) {
    auto ver = mgr.get_version();
    EXPECT_EQ(ver.build, 19041u);
}

TEST_F(NtdllExtTest, GetVersion_PlatformId) {
    auto ver = mgr.get_version();
    EXPECT_EQ(ver.platform_id, 2u);
}

TEST_F(NtdllExtTest, QueryElevationFlagsReportsUacBrokerEnabled) {
    GuestMemory memory;
    constexpr GuestAddr kData = 0x10000;
    constexpr GuestAddr kStack = 0x20000;
    ASSERT_TRUE(memory.map_region(
        kData, 0x1000, MemoryProtection::ReadWrite).is_ok());
    ASSERT_TRUE(memory.map_region(
        kStack, 0x1000, MemoryProtection::ReadWrite).is_ok());

    cpu::CpuState cpu;
    cpu.init_protected_mode();
    cpu.gpr.rsp = kStack + 0x800;
    ASSERT_TRUE(memory.write_u32(cpu.gpr.rsp + 4, kData).is_ok());

    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    auto exports = make_ntdll_ext_exports(mgr);
    auto entry = std::find_if(
        exports.entries.begin(), exports.entries.end(),
        [](const ExportEntry& candidate) {
            return candidate.name == "RtlQueryElevationFlags";
        });
    ASSERT_NE(entry, exports.entries.end());

    entry->handler(marshaler);

    EXPECT_EQ(cpu.gpr.rax, nt_status::STATUS_SUCCESS);
    EXPECT_EQ(memory.read_u32(kData).value(), 0x1u);
}

// =============================================================================
// NtClose Tests
// =============================================================================

TEST_F(NtdllExtTest, NtClose_ReturnsSuccess) {
    EXPECT_EQ(mgr.nt_close(0x1234), nt_status::STATUS_SUCCESS);
}

TEST_F(NtdllExtTest, NtClose_ZeroHandle) {
    EXPECT_EQ(mgr.nt_close(0), nt_status::STATUS_INVALID_HANDLE);
}

// =============================================================================
// NtDelayExecution Tests
// =============================================================================

// nt_alert_wait_probe.expected "delay_zero status=40000024": with no other
// thread ready, a zero delay reports STATUS_NO_YIELD_PERFORMED.
TEST_F(NtdllExtTest, DelayExecution_ZeroDelay) {
    u32 status = mgr.nt_delay_execution(false, 0);
    EXPECT_EQ(status, nt_status::STATUS_NO_YIELD_PERFORMED);
}

TEST_F(NtdllExtTest, DelayExecution_NegativeRelative) {
    // Negative = relative time, -10000 = 1ms
    u32 status = mgr.nt_delay_execution(false, -10000);
    EXPECT_EQ(status, nt_status::STATUS_SUCCESS);
}

// =============================================================================
// Rtl Memory Operation Tests
// =============================================================================

TEST_F(NtdllExtTest, RtlCopyMemory_CopiesCorrectly) {
    const u8 src[] = {0xDE, 0xAD, 0xBE, 0xEF};
    u8 dst[4] = {};

    NtdllExtManager::rtl_copy_memory(dst, src, sizeof(src));
    EXPECT_EQ(std::memcmp(dst, src, sizeof(src)), 0);
}

TEST_F(NtdllExtTest, RtlZeroMemory_Zeros) {
    u8 buf[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    NtdllExtManager::rtl_zero_memory(buf, sizeof(buf));

    for (auto b : buf) {
        EXPECT_EQ(b, 0u);
    }
}

TEST_F(NtdllExtTest, RtlMoveMemory_HandlesOverlap) {
    u8 buf[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    // Overlapping move: copy buf[0..4] to buf[2..6]
    NtdllExtManager::rtl_move_memory(buf + 2, buf, 4);

    EXPECT_EQ(buf[2], 1u);
    EXPECT_EQ(buf[3], 2u);
    EXPECT_EQ(buf[4], 3u);
    EXPECT_EQ(buf[5], 4u);
}

TEST_F(NtdllExtTest, RtlFillMemory_Fills) {
    u8 buf[6] = {};
    NtdllExtManager::rtl_fill_memory(buf, sizeof(buf), 0xAB);

    for (auto b : buf) {
        EXPECT_EQ(b, 0xABu);
    }
}

// =============================================================================
// NtQuerySystemInformation Tests
// =============================================================================

TEST_F(NtdllExtTest, QuerySystemInfo_BasicInformation) {
    NtdllExtManager::SystemBasicInfo info{};
    u32 return_len = 0;

    u32 status = mgr.query_system_info(0, &info, sizeof(info), &return_len);

    EXPECT_EQ(status, nt_status::STATUS_SUCCESS);
    EXPECT_EQ(info.page_size, 4096u);
    EXPECT_EQ(info.allocation_granularity, 65536u);
    EXPECT_EQ(info.number_of_processors, 4u);
    EXPECT_GT(return_len, 0u);
}

TEST_F(NtdllExtTest, QuerySystemInfo_BasicInformation_BufferTooSmall) {
    u8 buf[4] = {};
    u32 return_len = 0;

    u32 status = mgr.query_system_info(0, buf, sizeof(buf), &return_len);
    EXPECT_EQ(status, nt_status::STATUS_INFO_LENGTH_MISMATCH);
}

TEST_F(NtdllExtTest, QuerySystemInfo_InvalidClass) {
    u8 buf[256] = {};
    u32 return_len = 0;

    u32 status = mgr.query_system_info(9999, buf, sizeof(buf), &return_len);
    EXPECT_EQ(status, nt_status::STATUS_INVALID_INFO_CLASS);
}

// =============================================================================
// NTSTATUS Extended Constants Tests
// =============================================================================

TEST_F(NtdllExtTest, NtStatusExtConstants) {
    EXPECT_EQ(nt_status::STATUS_INVALID_PARAMETER,     0xC000000Du);
    EXPECT_EQ(nt_status::STATUS_ACCESS_DENIED,          0xC0000022u);
    EXPECT_EQ(nt_status::STATUS_OBJECT_NAME_NOT_FOUND,  0xC0000034u);
    EXPECT_EQ(nt_status::STATUS_NO_MEMORY,              0xC0000017u);
    EXPECT_EQ(nt_status::STATUS_NOT_IMPLEMENTED,        0xC0000002u);
}
