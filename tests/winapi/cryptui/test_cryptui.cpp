/// @file test_cryptui.cpp
/// @brief cryptui.dll CryptUIDlgViewCertificateA/W (no viewer UI).

#include <gtest/gtest.h>

#include <core/cpu/state/cpu_state.hpp>
#include <core/memory/guest_memory.hpp>
#include <winapi/core/marshaler.hpp>
#include <winapi/kernel32/error.hpp>
#include <winapi/system/cryptui/cryptui.hpp>

using namespace vitreo;
using namespace vitreo::winapi;

class CryptUiTest : public ::testing::Test {
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
        exports = create_cryptui_thunks();
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

    GuestMemory memory;
    cpu::CpuState cpu;
    Marshaler marshaler{memory};
    DllExports exports;
};

TEST_F(CryptUiTest, StdcallBytesAreEight) {
    auto* wide = find_export("CryptUIDlgViewCertificateW");
    auto* ansi = find_export("CryptUIDlgViewCertificateA");
    ASSERT_NE(wide, nullptr);
    ASSERT_NE(ansi, nullptr);
    EXPECT_EQ(wide->stdcall_bytes, 8);
    EXPECT_EQ(ansi->stdcall_bytes, 8);
}

TEST_F(CryptUiTest, NullViewInfoIsInvalidParameter) {
    auto* entry = find_export("CryptUIDlgViewCertificateW");
    ASSERT_NE(entry, nullptr);

    push_arg(0, 0);
    push_arg(1, 0);
    entry->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_PARAMETER);
}

TEST_F(CryptUiTest, DummyViewInfoWritesUnchangedAndCancels) {
    auto* entry = find_export("CryptUIDlgViewCertificateW");
    ASSERT_NE(entry, nullptr);

    ASSERT_TRUE(memory.write_u32(DATA_BASE + 0x100, 1).is_ok());

    push_arg(0, static_cast<u32>(DATA_BASE));
    push_arg(1, static_cast<u32>(DATA_BASE + 0x100));
    entry->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 0u);
    EXPECT_EQ(memory.read_u32(DATA_BASE + 0x100).value(), 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_CANCELLED);
}
