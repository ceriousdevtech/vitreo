/// @file test_mpr.cpp
/// @brief mpr.dll WNetGetUniversalName for local (non-redirected) paths.

#include <gtest/gtest.h>

#include <core/cpu/state/cpu_state.hpp>
#include <core/memory/guest_memory.hpp>
#include <winapi/core/marshaler.hpp>
#include <winapi/networking/mpr/mpr.hpp>

#include <cstring>

using namespace vitreo;
using namespace vitreo::winapi;

class MprTest : public ::testing::Test {
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
        exports = create_mpr_thunks();
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

TEST_F(MprTest, LocalDrivePathIsNotRedirected) {
    auto* entry = find_export("WNetGetUniversalNameW");
    ASSERT_NE(entry, nullptr);
    EXPECT_EQ(entry->stdcall_bytes, 16);

    const char16_t path[] = u"C:\\Windows\\Fonts\\arial.ttf";
    ASSERT_TRUE(memory.write_bytes(
        DATA_BASE + 0x100, reinterpret_cast<const Byte*>(path),
        sizeof(path)).is_ok());
    ASSERT_TRUE(memory.write_u32(DATA_BASE + 0x200, 64).is_ok());

    push_arg(0, static_cast<u32>(DATA_BASE + 0x100));
    push_arg(1, 1);  // UNIVERSAL_NAME_INFO_LEVEL
    push_arg(2, static_cast<u32>(DATA_BASE + 0x300));
    push_arg(3, static_cast<u32>(DATA_BASE + 0x200));
    entry->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 2250u);  // ERROR_NOT_CONNECTED
}

TEST_F(MprTest, RejectsNonDrivePathAndBadInfoLevel) {
    auto* entry = find_export("WNetGetUniversalNameA");
    ASSERT_NE(entry, nullptr);

    const char unc[] = "\\\\server\\share\\file.txt";
    ASSERT_TRUE(memory.write_bytes(
        DATA_BASE + 0x100, reinterpret_cast<const Byte*>(unc),
        sizeof(unc)).is_ok());
    ASSERT_TRUE(memory.write_u32(DATA_BASE + 0x200, 64).is_ok());

    push_arg(0, static_cast<u32>(DATA_BASE + 0x100));
    push_arg(1, 1);
    push_arg(2, static_cast<u32>(DATA_BASE + 0x300));
    push_arg(3, static_cast<u32>(DATA_BASE + 0x200));
    entry->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 1200u);  // ERROR_BAD_DEVICE

    push_arg(1, 99);
    entry->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 87u);  // ERROR_INVALID_PARAMETER
}
