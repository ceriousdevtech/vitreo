/// @file test_kernel32_cpuz.cpp
/// @brief Unit tests for the CPU-Z driver device, host system-information and memory-status KERNEL32 contracts

#include "kernel32_cpuz_fixture.hpp"
#include <winapi/kernel32/error.hpp>
#include <winapi/kernel32/filesystem/file_pipe_internal.hpp>
#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <unistd.h>
#include <thread>
#include <sys/sysctl.h>

TEST_F(Kernel32CpuzTest, CpuzDeviceReportsDriverAndOsVersions) {
    constexpr GuestAddr output = DATA_BASE + 0x900;
    FileManager files{vfs, handles, memory};
    auto* device = new std::string("cpuz162");
    const Handle handle = handles.allocate(HandleType::Device, device);
    ASSERT_NE(handle, NULL_HANDLE);

    u32 bytes_returned = 0;
    set_last_error(0xDEADu);
    EXPECT_TRUE(files.device_io_control(
        handle, 0x9C402400, 0, 0, output, 4, bytes_returned));
    EXPECT_EQ(bytes_returned, 4u);
    EXPECT_EQ(memory.read_u32(output).value_or(0), 0x162u);
    EXPECT_EQ(get_last_error(), 0xDEADu);  // success leaves it untouched (last_error_on_success_probe.expected)

    set_last_error(0xDEADu);
    EXPECT_TRUE(files.device_io_control(
        handle, 0x9C402404, 0, 0, output, 12, bytes_returned));
    EXPECT_EQ(bytes_returned, 12u);
    EXPECT_EQ(memory.read_u32(output + 0).value_or(0), 10u);
    EXPECT_EQ(memory.read_u32(output + 4).value_or(1), 0u);
    EXPECT_EQ(memory.read_u32(output + 8).value_or(0), 19041u);
    EXPECT_EQ(get_last_error(), 0xDEADu);  // success leaves it untouched (last_error_on_success_probe.expected)

    EXPECT_TRUE(handles.close(handle));
    EXPECT_TRUE(files.on_handle_close(handle, HandleType::Device, device));
}

TEST_F(Kernel32CpuzTest, CpuzDeviceRejectsShortVersionBuffer) {
    constexpr GuestAddr output = DATA_BASE + 0x900;
    FileManager files{vfs, handles, memory};
    auto* device = new std::string("cpuz162");
    const Handle handle = handles.allocate(HandleType::Device, device);
    ASSERT_NE(handle, NULL_HANDLE);

    u32 bytes_returned = 99;
    EXPECT_FALSE(files.device_io_control(
        handle, 0x9C402404, 0, 0, output, 8, bytes_returned));
    EXPECT_EQ(bytes_returned, 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INSUFFICIENT_BUFFER);

    EXPECT_TRUE(handles.close(handle));
    EXPECT_TRUE(files.on_handle_close(handle, HandleType::Device, device));
}

TEST_F(Kernel32CpuzTest, CpuzDeviceExposesVirtualSmbiosFirmwareWindow) {
    constexpr GuestAddr input = DATA_BASE + 0x900;
    constexpr GuestAddr output = DATA_BASE + 0x1000;
    FileManager files{vfs, handles, memory};
    auto* device = new std::string("cpuz162");
    const Handle handle = handles.allocate(HandleType::Device, device);
    ASSERT_NE(handle, NULL_HANDLE);

    ASSERT_TRUE(memory.write_u32(input + 0, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(input + 4, 0x000F0000).is_ok());
    ASSERT_TRUE(memory.write_u32(input + 8, 2048).is_ok());

    u32 bytes_returned = 0;
    ASSERT_TRUE(files.device_io_control(
        handle, 0x9C402540, input, 12, output, 2052, bytes_returned));
    EXPECT_EQ(bytes_returned, 2052u);
    EXPECT_EQ(memory.read_u32(output).value_or(1), 0u);

    // The window starts with the SMBIOS 3.0 (64-bit) entry point (DSP0134
    // 5.2.2), then carries the 2.1-style 32-bit one at 20h.
    std::array<u8, 0x18> entry3{};
    ASSERT_TRUE(memory.read_bytes(
        output + 4, entry3.data(), entry3.size()).is_ok());
    EXPECT_EQ(std::string(entry3.begin(), entry3.begin() + 5), "_SM3_");
    EXPECT_EQ(entry3[6], 0x18u);
    EXPECT_EQ(entry3[7], 3u);
    EXPECT_EQ(entry3[8], 6u);

    std::array<u8, 0x1F> entry{};
    ASSERT_TRUE(memory.read_bytes(
        output + 4 + 0x20, entry.data(), entry.size()).is_ok());
    EXPECT_EQ(std::string(entry.begin(), entry.begin() + 4), "_SM_");
    EXPECT_EQ(std::string(entry.begin() + 16, entry.begin() + 21), "_DMI_");
    EXPECT_EQ(entry[5], 0x1Fu);
    EXPECT_EQ(entry[6], 3u);
    EXPECT_EQ(entry[7], 6u);

    const auto checksum = [](const u8* data, usize size) {
        u8 sum = 0;
        for (usize i = 0; i < size; ++i) {
            sum = static_cast<u8>(sum + data[i]);
        }
        return sum;
    };
    EXPECT_EQ(checksum(entry3.data(), entry3.size()), 0u);
    EXPECT_EQ(checksum(entry.data(), entry.size()), 0u);
    EXPECT_EQ(checksum(entry.data() + 16, entry.size() - 16), 0u);
    const u32 table_address =
        static_cast<u32>(entry[24]) |
        (static_cast<u32>(entry[25]) << 8) |
        (static_cast<u32>(entry[26]) << 16) |
        (static_cast<u32>(entry[27]) << 24);
    EXPECT_EQ(table_address, 0x000F0100u);

    std::array<char, 1024> table{};
    ASSERT_TRUE(memory.read_bytes(
        output + 4 + 0x100, reinterpret_cast<u8*>(table.data()),
        table.size()).is_ok());
    const std::string table_bytes(table.data(), table.size());
    EXPECT_TRUE(
        table_bytes.find("Apple Inc.") != std::string::npos ||
        table_bytes.find("Vitreo") != std::string::npos);
    EXPECT_EQ(table_bytes.find("VITREO-0001"), std::string::npos);
    EXPECT_EQ(table_bytes.find("VITREO-MB-0001"), std::string::npos);

    EXPECT_TRUE(handles.close(handle));
    EXPECT_TRUE(files.on_handle_close(handle, HandleType::Device, device));
}

TEST_F(Kernel32CpuzTest, CpuzDeviceRejectsPhysicalReadsOutsideVirtualBios) {
    constexpr GuestAddr input = DATA_BASE + 0x900;
    constexpr GuestAddr output = DATA_BASE + 0x1000;
    FileManager files{vfs, handles, memory};
    auto* device = new std::string("cpuz162");
    const Handle handle = handles.allocate(HandleType::Device, device);
    ASSERT_NE(handle, NULL_HANDLE);

    ASSERT_TRUE(memory.write_u32(input + 0, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(input + 4, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(input + 8, 1024).is_ok());

    u32 bytes_returned = 99;
    EXPECT_FALSE(files.device_io_control(
        handle, 0x9C402540, input, 12, output, 1028, bytes_returned));
    EXPECT_EQ(bytes_returned, 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_PARAMETER);

    EXPECT_TRUE(handles.close(handle));
    EXPECT_TRUE(files.on_handle_close(handle, HandleType::Device, device));
}

TEST_F(Kernel32CpuzTest, KernelDriverRunningReflectsProcessLocalProvider) {
    FileManager files{vfs, handles, memory};
    EXPECT_FALSE(files.kernel_driver_running("CPUZ162"));

    // The file contents exercise only the provider identity check; the test
    // image is not executed as native kernel code.
    const auto image = std::filesystem::temp_directory_path() /
                       "vitreo_cpuz162_provider_test.sys";
    {
        std::ofstream out(image, std::ios::binary);
        ASSERT_TRUE(out.good());
        const std::array<u8, 18> bytes{
            'M', 'Z', 'C', 0, 'P', 0, 'U', 0, 'Z', 0,
            '1', 0, '6', 0, '2', 0, 0, 0};
        out.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    }

    u32 error = 0;
    ASSERT_TRUE(files.start_kernel_driver("CPUZ162", image.string(), error));
    EXPECT_EQ(error, error_codes::ERROR_SUCCESS);
    EXPECT_TRUE(files.kernel_driver_running("cpuz162"));
    files.stop_kernel_driver("CPUZ162");
    EXPECT_FALSE(files.kernel_driver_running("cpuz162"));

    std::error_code ec;
    std::filesystem::remove(image, ec);
}

TEST_F(Kernel32CpuzTest, GetLogicalProcessorInformationExReturnsRequestedPackage) {
    constexpr GuestAddr length = DATA_BASE;
    constexpr GuestAddr buffer = DATA_BASE + 0x100;
    const GuestAddr stack = cpu.sp();
    ASSERT_TRUE(memory.write_u32(length, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 4, 3).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 8, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 12, length).is_ok());

    mgr.impl_GetLogicalProcessorInformationEx(marshaler);
    EXPECT_EQ(cpu.gpr.rax, 0u);
    EXPECT_EQ(memory.read_u32(length).value_or(0), 44u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INSUFFICIENT_BUFFER);

    ASSERT_TRUE(memory.write_u32(stack + 8, buffer).is_ok());
    ASSERT_TRUE(memory.write_u32(length, 44).is_ok());
    mgr.impl_GetLogicalProcessorInformationEx(marshaler);
    EXPECT_EQ(cpu.gpr.rax, 1u);
    EXPECT_EQ(memory.read_u32(buffer + 0).value_or(0), 3u);
    EXPECT_EQ(memory.read_u32(buffer + 4).value_or(0), 44u);
    EXPECT_EQ(memory.read_u16(buffer + 30).value_or(0), 1u);
    const u32 host_count = std::clamp(
        std::thread::hardware_concurrency(), 1u, 32u);
    const u32 host_mask =
        host_count == 32 ? ~0u : ((1u << host_count) - 1);
    EXPECT_EQ(memory.read_u32(buffer + 32).value_or(0), host_mask);
}

TEST_F(Kernel32CpuzTest, GetLogicalProcessorInformationExReturnsX86GroupLayout) {
    constexpr GuestAddr length = DATA_BASE;
    constexpr GuestAddr buffer = DATA_BASE + 0x100;
    const GuestAddr stack = cpu.sp();
    ASSERT_TRUE(memory.write_u32(length, 76).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 4, 4).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 8, buffer).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 12, length).is_ok());

    mgr.impl_GetLogicalProcessorInformationEx(marshaler);
    EXPECT_EQ(cpu.gpr.rax, 1u);
    EXPECT_EQ(memory.read_u32(buffer + 0).value_or(0), 4u);
    EXPECT_EQ(memory.read_u32(buffer + 4).value_or(0), 76u);
    EXPECT_EQ(memory.read_u16(buffer + 8).value_or(0), 1u);
    EXPECT_EQ(memory.read_u16(buffer + 10).value_or(0), 1u);
    const u32 host_count = std::clamp(
        std::thread::hardware_concurrency(), 1u, 32u);
    const u32 host_mask =
        host_count == 32 ? ~0u : ((1u << host_count) - 1);
    EXPECT_EQ(memory.read_u8(buffer + 32).value_or(0), host_count);
    EXPECT_EQ(memory.read_u8(buffer + 33).value_or(0), host_count);
    EXPECT_EQ(memory.read_u32(buffer + 72).value_or(0), host_mask);
}

TEST_F(Kernel32CpuzTest, GetLogicalProcessorInformationExRelationAllIsSequence) {
    // The export hands out exactly the records processor_topology builds
    // (their layout and order: test_processor_topology.cpp), behind the
    // measured length protocol.
    test::ScopedHostHardware host(test::m4_pro_profile());
    constexpr GuestAddr length = DATA_BASE;
    constexpr GuestAddr buffer = DATA_BASE + 0x1000;
    const GuestAddr stack = cpu.sp();
    const auto records = processor_topology::logical_processor_information_ex(0xFFFF, false);
    ASSERT_TRUE(records.has_value());
    const u32 required = static_cast<u32>(records->size());
    ASSERT_TRUE(memory.write_u32(length, 76).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 4, 0xFFFFu).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 8, buffer).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 12, length).is_ok());

    mgr.impl_GetLogicalProcessorInformationEx(marshaler);
    EXPECT_EQ(cpu.gpr.rax, 0u);
    EXPECT_EQ(memory.read_u32(length).value_or(0), required);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INSUFFICIENT_BUFFER);

    set_last_error(0x1234);
    mgr.impl_GetLogicalProcessorInformationEx(marshaler);
    ASSERT_EQ(cpu.gpr.rax, 1u);
    EXPECT_EQ(memory.read_u32(length).value_or(0), required);
    EXPECT_EQ(get_last_error(), 0x1234u);
    std::vector<u8> written(required);
    ASSERT_TRUE(memory.read_bytes(buffer, reinterpret_cast<Byte*>(written.data()), written.size()).is_ok());
    EXPECT_EQ(written, *records);

    // An unknown relationship is ERROR_GEN_FAILURE (measured).
    ASSERT_TRUE(memory.write_u32(stack + 4, 8).is_ok());
    mgr.impl_GetLogicalProcessorInformationEx(marshaler);
    EXPECT_EQ(cpu.gpr.rax, 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_GEN_FAILURE);
}

TEST_F(Kernel32CpuzTest, GetLogicalProcessorInformationUsesHostCoreCount) {
    test::ScopedHostHardware host(test::m4_pro_profile());
    constexpr GuestAddr length = DATA_BASE;
    constexpr GuestAddr buffer = DATA_BASE + 0x1000;
    const GuestAddr stack = cpu.sp();
    const auto records = processor_topology::logical_processor_information(false);
    const u32 required = static_cast<u32>(records.size());
    EXPECT_EQ(required, (14u + 31u + 2u) * 24u);  // x86 records are 24 bytes
    ASSERT_TRUE(memory.write_u32(length, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 4, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 8, length).is_ok());

    mgr.impl_GetLogicalProcessorInformation(marshaler);
    EXPECT_EQ(cpu.gpr.rax, 0u);
    EXPECT_EQ(memory.read_u32(length).value_or(0), required);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INSUFFICIENT_BUFFER);

    ASSERT_TRUE(memory.write_u32(stack + 4, buffer).is_ok());
    ASSERT_TRUE(memory.write_u32(length, required).is_ok());
    mgr.impl_GetLogicalProcessorInformation(marshaler);
    EXPECT_EQ(cpu.gpr.rax, 1u);
    EXPECT_EQ(memory.read_u32(length).value_or(0), required);
    std::vector<u8> written(required);
    ASSERT_TRUE(memory.read_bytes(buffer, reinterpret_cast<Byte*>(written.data()), written.size()).is_ok());
    EXPECT_EQ(written, records);
}

TEST_F(Kernel32CpuzTest, GlobalMemoryStatusExUsesHostPhysicalMemory) {
    constexpr GuestAddr status = DATA_BASE + 0x400;
    const GuestAddr stack = cpu.sp();
    ASSERT_TRUE(memory.write_u32(status, 64).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 4, status).is_ok());

    mgr.impl_GlobalMemoryStatusEx(marshaler);
    ASSERT_EQ(cpu.gpr.rax, 1u);
    EXPECT_EQ(memory.read_u32(status).value_or(0), 64u);
    u64 host_memory = 0;
    size_t size = sizeof(host_memory);
    ASSERT_EQ(sysctlbyname(
        "hw.memsize", &host_memory, &size, nullptr, 0), 0);
    EXPECT_EQ(memory.read_u64(status + 8).value_or(0), host_memory);
    EXPECT_LE(memory.read_u64(status + 16).value_or(host_memory + 1),
              host_memory);
}

TEST_F(Kernel32CpuzTest, ActiveProcessorApisUseHostExecutionCapacity) {
    const u32 expected = std::clamp(
        std::thread::hardware_concurrency(), 1u, 32u);
    (void)memory.write_u32(cpu.sp() + 4, 0xFFFFu);
    mgr.impl_GetActiveProcessorCount(marshaler);
    EXPECT_EQ(cpu.gpr.rax, expected);

    mgr.impl_GetActiveProcessorGroupCount(marshaler);
    EXPECT_EQ(cpu.gpr.rax, 1u);
}

TEST_F(Kernel32CpuzTest, PhysicallyInstalledMemoryUsesHostCapacity) {
    constexpr GuestAddr installed_kb = DATA_BASE + 0xB00;
    (void)memory.write_u32(cpu.sp() + 4, installed_kb);
    mgr.impl_GetPhysicallyInstalledSystemMemory(marshaler);
    ASSERT_EQ(cpu.gpr.rax, 1u);

    u64 host_bytes = 0;
    size_t host_size = sizeof(host_bytes);
    ASSERT_EQ(sysctlbyname(
        "hw.memsize", &host_bytes, &host_size, nullptr, 0), 0);
    EXPECT_EQ(memory.read_u64(installed_kb).value(), host_bytes / 1024);
}

TEST_F(Kernel32CpuzTest, PerformanceInfoFailsInsteadOfInventingCounters) {
    constexpr GuestAddr info = DATA_BASE + 0xC00;
    constexpr u32 x86_performance_info_size = 56;
    constexpr u8 sentinel = 0xA5;
    for (u32 offset = 0; offset < x86_performance_info_size; ++offset) {
        ASSERT_TRUE(memory.write_u8(info + offset, sentinel).is_ok());
    }
    ASSERT_TRUE(memory.write_u32(cpu.sp() + 4, info).is_ok());
    ASSERT_TRUE(memory.write_u32(
        cpu.sp() + 8, x86_performance_info_size).is_ok());

    mgr.impl_K32GetPerformanceInfo(marshaler);

    EXPECT_EQ(cpu.gpr.rax, 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_CALL_NOT_IMPLEMENTED);
    for (u32 offset = 0; offset < x86_performance_info_size; ++offset) {
        EXPECT_EQ(memory.read_u8(info + offset).value_or(0), sentinel);
    }
}

TEST_F(Kernel32CpuzTest, GlobalMemoryStatus_WritesNonZeroTotalPhys) {
    constexpr GuestAddr buf = DATA_BASE + 0xA00;
    // Zero the buffer (32 bytes for MEMORYSTATUS)
    for (u32 i = 0; i < 32; i += 4) {
        (void)memory.write_u32(buf + i, 0);
    }

    (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 4), buf);  // lpBuffer
    mgr.impl_GlobalMemoryStatus(marshaler);

    // dwTotalPhys at offset 8
    auto total = memory.read_u32(buf + 8);
    ASSERT_TRUE(total.is_ok());
    EXPECT_NE(total.value(), 0u);
}

// Learn / KB 274558: non-LAA 32-bit GlobalMemoryStatus caps physical
// memory at 2 GB. Reporting 0xFFFFFFFF (-1) makes a signed "enough RAM"
// check fail as OOM on hosts with more than 4 GB.
TEST_F(Kernel32CpuzTest, GlobalMemoryStatus_NonLaa32BitCapsPhysAt2Gb) {
    constexpr GuestAddr buf = DATA_BASE + 0xA00;
    for (u32 i = 0; i < 32; i += 4) {
        (void)memory.write_u32(buf + i, 0);
    }

    (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 4), buf);
    mgr.impl_GlobalMemoryStatus(marshaler);

    const u32 total = memory.read_u32(buf + 8).value_or(0);
    const u32 avail = memory.read_u32(buf + 12).value_or(0);
    const u32 load = memory.read_u32(buf + 4).value_or(1000);
    EXPECT_NE(total, 0u);
    EXPECT_LE(total, 0x80000000u);
    EXPECT_NE(total, 0xFFFFFFFFu);
    EXPECT_LE(avail, total);
    EXPECT_NE(avail, 0xFFFFFFFFu);
    EXPECT_LE(load, 100u);
}

TEST_F(Kernel32CpuzTest, GlobalMemoryStatus_AvailVirtualIsRemainingUserVa) {
    // Learn MEMORYSTATUS: dwAvailVirtual is unreserved/uncommitted user VA,
    // not a hardcoded 1.5 GB. dwTotalVirtual follows the process ceiling.
    constexpr GuestAddr buf = DATA_BASE + 0xA00;
    constexpr GuestAddr kCeiling = 0x7FFEFFFFu;
    memory.set_maximum_user_address(kCeiling);
    for (u32 i = 0; i < 32; i += 4) {
        (void)memory.write_u32(buf + i, 0);
    }

    (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 4), buf);
    mgr.impl_GlobalMemoryStatus(marshaler);

    const u32 total_virtual = memory.read_u32(buf + 24).value_or(0);
    const u32 avail_virtual = memory.read_u32(buf + 28).value_or(0);
    const u32 mapped = static_cast<u32>(memory.total_mapped_size());
    EXPECT_EQ(total_virtual, kCeiling);
    EXPECT_EQ(avail_virtual, kCeiling - mapped);
    EXPECT_NE(avail_virtual, 0x60000000u);
}
