// SPDX-License-Identifier: LGPL-2.1-only
/// @file test_crt_stdio_ucrt_abi.cpp
/// @brief UCRT x86 thunk-ABI tests for vsprintf_s, vsscanf and the standard-stream buffer cells

#include "crt_stdio_test_fixture.hpp"

// The printf engine's tests (format semantics per the UCRT source and the
// crt_printf_probe / crt_msvcrt_printf_probe oracles) live in
// test_crt_printf.cpp. The earlier expectation here that "%a" of 1.5 prints
// 0x1.8p+0 was the host libc's answer; Windows prints 0x1.8000000000000p+0.

TEST_F(CrtStdioTest, UcrtVsprintfSFormatsX86VaListAndRejectsTruncation) {
    constexpr GuestAddr base = 0x10000;
    constexpr GuestAddr stack = base;
    constexpr GuestAddr destination = base + 0x200;
    constexpr GuestAddr format = base + 0x300;
    constexpr GuestAddr text = base + 0x400;
    constexpr GuestAddr va_list = base + 0x500;

    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(
        base, 0x10000, MemoryProtection::ReadWrite).is_ok());
    capsule::Vfs vfs;
    CrtDataBlock crt_data;
    ASSERT_TRUE(crt_data.initialize(memory));
    CrtMemoryManager memory_manager(memory);
    // The string-target printf backends are registered by crt_printf.
    DllExports exports = create_crt_printf_thunks(memory_manager);
    auto entry = std::find_if(
        exports.entries.begin(), exports.entries.end(),
        [](const ExportEntry& candidate) {
            return candidate.name == "__stdio_common_vsprintf_s";
        });
    ASSERT_NE(entry, exports.entries.end());

    cpu::CpuState cpu;
    cpu.init_protected_mode();
    cpu.gpr.rsp = stack;
    Marshaler marshaler{memory};
    marshaler.attach_cpu(cpu);
    ASSERT_TRUE(marshaler.write_ansi_string(format, "value=%d:%s").is_ok());
    ASSERT_TRUE(marshaler.write_ansi_string(text, "ready").is_ok());
    ASSERT_TRUE(memory.write_u32(va_list, 7).is_ok());
    ASSERT_TRUE(memory.write_u32(va_list + 4, text).is_ok());
    // _Options is `unsigned __int64`, so on x86 it occupies two stack slots
    // and the buffer pointer starts at stack + 12.
    ASSERT_TRUE(memory.write_u32(stack + 4, 0).is_ok());   // options low
    ASSERT_TRUE(memory.write_u32(stack + 8, 0).is_ok());   // options high
    ASSERT_TRUE(memory.write_u32(stack + 12, destination).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 16, 32).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 20, format).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 24, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 28, va_list).is_ok());

    entry->handler(marshaler);
    EXPECT_EQ(cpu.gpr.rax, 13u);
    EXPECT_EQ(marshaler.read_ansi_string(destination).value(),
              "value=7:ready");

    ASSERT_TRUE(marshaler.write_ansi_string(destination, "old").is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 16, 5).is_ok());
    entry->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), std::numeric_limits<u32>::max());
    EXPECT_EQ(memory.read_u8(destination).value(), 0u);
}

TEST_F(CrtStdioTest, UcrtVsscanfParsesX86VaList) {
    constexpr GuestAddr base = 0x10000;
    constexpr GuestAddr stack = base;
    constexpr GuestAddr input = base + 0x200;
    constexpr GuestAddr format = base + 0x300;
    constexpr GuestAddr out_int = base + 0x400;
    constexpr GuestAddr out_str = base + 0x500;
    constexpr GuestAddr va_list = base + 0x600;

    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(
        base, 0x10000, MemoryProtection::ReadWrite).is_ok());
    capsule::Vfs vfs;
    CrtDataBlock crt_data;
    ASSERT_TRUE(crt_data.initialize(memory));
    CrtMemoryManager memory_manager(memory);
    DllExports exports = create_crt_stdio_thunks(mgr, vfs, memory_manager, crt_data);
    auto entry = std::find_if(
        exports.entries.begin(), exports.entries.end(),
        [](const ExportEntry& candidate) {
            return candidate.name == "__stdio_common_vsscanf";
        });
    ASSERT_NE(entry, exports.entries.end());

    cpu::CpuState cpu;
    cpu.init_protected_mode();
    cpu.gpr.rsp = stack;
    Marshaler marshaler{memory};
    marshaler.attach_cpu(cpu);
    ASSERT_TRUE(marshaler.write_ansi_string(input, "42 ready").is_ok());
    ASSERT_TRUE(marshaler.write_ansi_string(format, "%d %s").is_ok());
    ASSERT_TRUE(memory.write_u32(va_list, out_int).is_ok());
    ASSERT_TRUE(memory.write_u32(va_list + 4, out_str).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 4, 0).is_ok());   // options low
    ASSERT_TRUE(memory.write_u32(stack + 8, 0).is_ok());   // options high
    ASSERT_TRUE(memory.write_u32(stack + 12, input).is_ok());
    // Non-secure x86 vsscanf passes the 32-bit SIZE_MAX sentinel. The host is
    // 64-bit, so the thunk must interpret this using the guest ABI width.
    ASSERT_TRUE(memory.write_u32(
        stack + 16, std::numeric_limits<u32>::max()).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 20, format).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 24, 0).is_ok());   // locale
    ASSERT_TRUE(memory.write_u32(stack + 28, va_list).is_ok());

    entry->handler(marshaler);
    EXPECT_EQ(cpu.gpr.rax, 2u);
    EXPECT_EQ(memory.read_u32(out_int).value(), 42u);
    EXPECT_EQ(marshaler.read_ansi_string(out_str).value(), "ready");
}

TEST_F(CrtStdioTest, UcrtStandardStreamsExposeStableBufferCells) {
    constexpr GuestAddr stack = 0x10000;
    constexpr GuestAddr base_out = 0x10200;
    constexpr GuestAddr pointer_out = 0x10204;
    constexpr GuestAddr count_out = 0x10208;

    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(
        stack, 0x10000, MemoryProtection::ReadWrite).is_ok());
    CrtDataBlock crt_data;
    ASSERT_TRUE(crt_data.initialize(memory));
    capsule::Vfs vfs;
    CrtMemoryManager memory_manager(memory);
    DllExports exports = create_crt_stdio_thunks(mgr, vfs, memory_manager, crt_data);

    const auto find_export = [&exports](std::string_view name) {
        return std::find_if(
            exports.entries.begin(), exports.entries.end(),
            [name](const ExportEntry& candidate) {
                return candidate.name == name;
            });
    };
    const auto iob = find_export("__acrt_iob_func");
    const auto pointers = find_export("_get_stream_buffer_pointers");
    ASSERT_NE(iob, exports.entries.end());
    ASSERT_NE(pointers, exports.entries.end());

    cpu::CpuState cpu;
    cpu.init_protected_mode();
    cpu.gpr.rsp = stack;
    Marshaler marshaler{memory};
    marshaler.attach_cpu(cpu);

    ASSERT_TRUE(memory.write_u32(stack + 4, 1).is_ok());
    iob->handler(marshaler);
    const GuestAddr stdout_stream = crt_data.acrt_stream_addr(1);
    ASSERT_EQ(cpu.gpr.rax, stdout_stream);

    ASSERT_TRUE(memory.write_u32(
        stack + 4, static_cast<u32>(stdout_stream)).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 8, base_out).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 12, pointer_out).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 16, count_out).is_ok());
    pointers->handler(marshaler);
    EXPECT_EQ(cpu.gpr.rax, 0u);
    EXPECT_EQ(memory.read_u32(base_out).value(),
              stdout_stream + CrtDataBlock::ACRT_BASE_OFFSET);
    EXPECT_EQ(memory.read_u32(pointer_out).value(),
              stdout_stream + CrtDataBlock::ACRT_PTR_OFFSET);
    EXPECT_EQ(memory.read_u32(count_out).value(),
              stdout_stream + CrtDataBlock::ACRT_COUNT_OFFSET);
    EXPECT_EQ(memory.read_u32(
                  stdout_stream + CrtDataBlock::ACRT_PTR_OFFSET).value(),
              0u);
    EXPECT_EQ(memory.read_u32(
                  stdout_stream + CrtDataBlock::ACRT_COUNT_OFFSET).value(),
              0u);
    EXPECT_EQ(memory.read_u32(
                  stdout_stream + CrtDataBlock::ACRT_BASE_OFFSET).value(),
              0u);

    ASSERT_TRUE(memory.write_u32(stack + 4, 3).is_ok());
    iob->handler(marshaler);
    EXPECT_EQ(cpu.gpr.rax, 0u);

    ASSERT_TRUE(memory.write_u32(stack + 4, 0x12345678).is_ok());
    pointers->handler(marshaler);
    EXPECT_EQ(cpu.gpr.rax, 22u);
}
