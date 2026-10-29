/// @file test_credui.cpp
/// @brief credui.dll CredUIPromptForWindowsCredentialsW/A contracts.

#include <gtest/gtest.h>

#include <core/cpu/state/cpu_state.hpp>
#include <core/memory/guest_memory.hpp>
#include <winapi/core/marshaler.hpp>
#include <winapi/system/credui/credui.hpp>

using namespace vitreo;
using namespace vitreo::winapi;

class CreduiTest : public ::testing::Test {
protected:
    static constexpr GuestAddr STACK_BASE = 0x80000;
    static constexpr GuestAddr DATA_BASE = 0x1000;

    void SetUp() override {
        ASSERT_TRUE(memory.map_region(
            DATA_BASE, 0x1000, MemoryProtection::ReadWrite).is_ok());
        ASSERT_TRUE(memory.map_region(
            STACK_BASE, 0x1000, MemoryProtection::ReadWrite).is_ok());
        cpu.init_protected_mode();
        cpu.gpr.rsp = STACK_BASE + 0x800;
        marshaler.attach_cpu(cpu);
        exports = create_credui_thunks();
    }

    ExportEntry* find_export(const char* name) {
        for (auto& entry : exports.entries) {
            if (entry.name == name) return &entry;
        }
        return nullptr;
    }

    void push_arg(usize index, u32 value) {
        ASSERT_TRUE(memory.write_u32(
            cpu.gpr.rsp + 4 + index * 4, value).is_ok());
    }

    void push_prompt_args(u32 auth_package, u32 in_buffer, u32 out_buffer,
                          u32 out_size, u32 flags) {
        push_arg(0, 0);
        push_arg(1, 0);
        push_arg(2, auth_package);
        push_arg(3, in_buffer);
        push_arg(4, 0);
        push_arg(5, out_buffer);
        push_arg(6, out_size);
        push_arg(7, 0);
        push_arg(8, flags);
    }

    GuestMemory memory;
    cpu::CpuState cpu;
    Marshaler marshaler{memory};
    DllExports exports;
};

TEST_F(CreduiTest, WideStdcallBytesIs36) {
    auto* entry = find_export("CredUIPromptForWindowsCredentialsW");
    ASSERT_NE(entry, nullptr);
    EXPECT_EQ(entry->stdcall_bytes, 36);
}

TEST_F(CreduiTest, MissingOutPointersReturnInvalidParameter) {
    auto* entry = find_export("CredUIPromptForWindowsCredentialsW");
    ASSERT_NE(entry, nullptr);

    const GuestAddr package = DATA_BASE;
    const GuestAddr out_ptr = DATA_BASE + 0x10;
    const GuestAddr out_size = DATA_BASE + 0x20;
    ASSERT_TRUE(memory.write_u32(package, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(out_ptr, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(out_size, 0).is_ok());

    push_prompt_args(0, 0, static_cast<u32>(out_ptr),
                     static_cast<u32>(out_size), 0);
    entry->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 87u);

    push_prompt_args(static_cast<u32>(package), 0, 0,
                     static_cast<u32>(out_size), 0);
    entry->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 87u);

    push_prompt_args(static_cast<u32>(package), 0,
                     static_cast<u32>(out_ptr), 0, 0);
    entry->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 87u);
}

TEST_F(CreduiTest, ValidArgsReturnCancelledWithNullOutputs) {
    auto* entry = find_export("CredUIPromptForWindowsCredentialsW");
    ASSERT_NE(entry, nullptr);

    const GuestAddr package = DATA_BASE;
    const GuestAddr out_ptr = DATA_BASE + 0x10;
    const GuestAddr out_size = DATA_BASE + 0x20;
    ASSERT_TRUE(memory.write_u32(package, 1).is_ok());
    ASSERT_TRUE(memory.write_u32(out_ptr, 0xDEADBEEF).is_ok());
    ASSERT_TRUE(memory.write_u32(out_size, 0xCAFEBABE).is_ok());

    push_prompt_args(static_cast<u32>(package), 0,
                     static_cast<u32>(out_ptr),
                     static_cast<u32>(out_size), 0);
    entry->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 1223u);

    auto buffer = memory.read_u32(out_ptr);
    ASSERT_TRUE(buffer.is_ok());
    EXPECT_EQ(buffer.value(), 0u);
    auto size = memory.read_u32(out_size);
    ASSERT_TRUE(size.is_ok());
    EXPECT_EQ(size.value(), 0u);
}

TEST_F(CreduiTest, InCredOnlyWithNullBufferReturnsInvalidParameter) {
    auto* entry = find_export("CredUIPromptForWindowsCredentialsW");
    ASSERT_NE(entry, nullptr);

    const GuestAddr package = DATA_BASE;
    const GuestAddr out_ptr = DATA_BASE + 0x10;
    const GuestAddr out_size = DATA_BASE + 0x20;
    ASSERT_TRUE(memory.write_u32(package, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(out_ptr, 0xDEADBEEF).is_ok());
    ASSERT_TRUE(memory.write_u32(out_size, 0xCAFEBABE).is_ok());

    push_prompt_args(static_cast<u32>(package), 0,
                     static_cast<u32>(out_ptr),
                     static_cast<u32>(out_size), 0x20);
    entry->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 87u);
}
