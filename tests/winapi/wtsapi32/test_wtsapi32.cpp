/// @file test_wtsapi32.cpp
/// @brief WTSAPI32 session information and allocation-lifetime tests.

#include <gtest/gtest.h>

#include <core/cpu/state/cpu_state.hpp>
#include <core/memory/guest_memory.hpp>
#include <winapi/kernel32/environment/environment.hpp>
#include <winapi/kernel32/memory/global_local.hpp>
#include <winapi/kernel32/handle.hpp>
#include <winapi/kernel32/memory/memory.hpp>
#include <winapi/core/marshaler.hpp>
#include <winapi/system/wtsapi32/wtsapi32.hpp>
#include <winapi/kernel32/error.hpp>

using namespace vitreo;
using namespace vitreo::winapi;

class Wtsapi32Test : public ::testing::Test {
protected:
    static constexpr GuestAddr STACK_BASE = 0x80000;
    static constexpr GuestAddr DATA_BASE = 0x1000;

    void SetUp() override {
        ASSERT_TRUE(memory.map_region(
            DATA_BASE, 0x10000, MemoryProtection::ReadWrite).is_ok());
        ASSERT_TRUE(memory.map_region(
            STACK_BASE, 0x1000, MemoryProtection::ReadWrite).is_ok());
        cpu.init_protected_mode();
        cpu.gpr.rsp = STACK_BASE + 0x800;
        marshaler.attach_cpu(cpu);
        exports = create_wtsapi32_thunks(allocations, environment);
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

    void query(u32 info_class) {
        push_arg(0, 0);
        push_arg(1, 0xFFFFFFFFu);
        push_arg(2, info_class);
        push_arg(3, DATA_BASE);
        push_arg(4, DATA_BASE + 8);
        auto* entry = find_export("WTSQuerySessionInformationW");
        ASSERT_NE(entry, nullptr);
        entry->handler(marshaler);
    }

    GuestMemory memory;
    cpu::CpuState cpu;
    Marshaler marshaler{memory};
    VirtualMemoryManager vmm{memory};
    HandleTable handles;
    HeapManager heap{vmm, memory, handles};
    GlobalLocalManager allocations{heap};
    EnvironmentManager environment{"test.exe", memory};
    DllExports exports;
};

TEST_F(Wtsapi32Test, QueryExportsDeclareDocumentedStdcallArity) {
    ASSERT_NE(find_export("WTSQuerySessionInformationW"), nullptr);
    ASSERT_NE(find_export("WTSQuerySessionInformationA"), nullptr);
    ASSERT_NE(find_export("WTSFreeMemory"), nullptr);
    EXPECT_EQ(find_export("WTSQuerySessionInformationW")->stdcall_bytes, 20);
    EXPECT_EQ(find_export("WTSQuerySessionInformationA")->stdcall_bytes, 20);
    EXPECT_EQ(find_export("WTSFreeMemory")->stdcall_bytes, 4);
}

TEST_F(Wtsapi32Test, CurrentSessionProtocolIsPhysicalConsole) {
    query(16); // WTSClientProtocolType
    ASSERT_EQ(cpu.gpr.rax, 1u);

    const GuestAddr buffer = memory.read_u32(DATA_BASE).value_or(0);
    ASSERT_NE(buffer, 0u);
    EXPECT_EQ(memory.read_u32(DATA_BASE + 8).value_or(0), 2u);
    EXPECT_EQ(memory.read_u16(buffer).value_or(0xFFFFu), 0u);

    push_arg(0, static_cast<u32>(buffer));
    find_export("WTSFreeMemory")->handler(marshaler);
    EXPECT_EQ(allocations.global_size(buffer), 0u);
}

TEST_F(Wtsapi32Test, X64QueryOverwritesFullOutputPointer) {
    cpu.init_long_mode();
    cpu.gpr.rsp = STACK_BASE + 0x800;
    ASSERT_TRUE(memory.write_u64(
        DATA_BASE, 0xDEADBEEFDEADBEEFull).is_ok());
    ASSERT_TRUE(memory.write_u32(DATA_BASE + 16, 0xAAAAAAAAu).is_ok());
    cpu.gpr.rcx = 0;
    cpu.gpr.rdx = 0xFFFFFFFFu;
    cpu.gpr.r8 = 16; // WTSClientProtocolType
    cpu.gpr.r9 = DATA_BASE;
    ASSERT_TRUE(memory.write_u64(
        cpu.gpr.rsp + 0x28, DATA_BASE + 16).is_ok());

    auto* entry = find_export("WTSQuerySessionInformationW");
    ASSERT_NE(entry, nullptr);
    entry->handler(marshaler);

    ASSERT_EQ(cpu.gpr.rax, 1u);
    const GuestAddr buffer = memory.read_u64(DATA_BASE).value_or(0);
    ASSERT_NE(buffer, 0u);
    EXPECT_EQ(buffer >> 32, 0u);
    EXPECT_EQ(memory.read_u32(DATA_BASE + 16).value_or(0), 2u);
    EXPECT_EQ(memory.read_u16(buffer).value_or(0xFFFFu), 0u);
    (void)allocations.global_free(buffer);
}

TEST_F(Wtsapi32Test, CurrentSessionUsernameUsesGuestEnvironment) {
    environment.set_variable("USERNAME", "Alice");
    query(5); // WTSUserName
    ASSERT_EQ(cpu.gpr.rax, 1u);

    const GuestAddr buffer = memory.read_u32(DATA_BASE).value_or(0);
    ASSERT_NE(buffer, 0u);
    EXPECT_EQ(memory.read_u32(DATA_BASE + 8).value_or(0), 12u);
    EXPECT_EQ(marshaler.read_wide_string(buffer).value_or(""), "Alice");
    (void)allocations.global_free(buffer);
}

TEST_F(Wtsapi32Test, SessionInfoExWReturnsActiveUnlockedConsoleSession) {
    environment.set_variable("USERNAME", "Alice");
    environment.set_variable("USERDOMAIN", "VITREOTEST");
    query(25); // WTSSessionInfoEx
    ASSERT_EQ(cpu.gpr.rax, 1u);

    const GuestAddr buffer = memory.read_u32(DATA_BASE).value_or(0);
    ASSERT_NE(buffer, 0u);
    EXPECT_EQ(memory.read_u32(DATA_BASE + 8).value_or(0), 232u);
    EXPECT_EQ(memory.read_u32(buffer).value_or(0), 1u);      // Level
    EXPECT_EQ(memory.read_u32(buffer + 8).value_or(0), 1u);  // SessionId
    EXPECT_EQ(memory.read_u32(buffer + 12).value_or(1), 0u); // WTSActive
    EXPECT_EQ(memory.read_u32(buffer + 16).value_or(0), 1u); // Unlocked
    EXPECT_EQ(marshaler.read_wide_string(buffer + 20).value_or(""),
              "Console");
    EXPECT_EQ(marshaler.read_wide_string(buffer + 86).value_or(""),
              "Alice");
    EXPECT_EQ(marshaler.read_wide_string(buffer + 128).value_or(""),
              "VITREOTEST");
    EXPECT_GT(memory.read_u64(buffer + 200).value_or(0), 0u); // CurrentTime
    EXPECT_EQ(memory.read_u32(buffer + 208).value_or(1), 0u);
    (void)allocations.global_free(buffer);
}

TEST_F(Wtsapi32Test, SessionInfoExAUsesAnsiLayout) {
    environment.set_variable("USERNAME", "Bob");
    push_arg(0, 0);
    push_arg(1, 0xFFFFFFFFu);
    push_arg(2, 25);
    push_arg(3, DATA_BASE);
    push_arg(4, DATA_BASE + 8);
    auto* entry = find_export("WTSQuerySessionInformationA");
    ASSERT_NE(entry, nullptr);
    entry->handler(marshaler);
    ASSERT_EQ(cpu.gpr.rax, 1u);

    const GuestAddr buffer = memory.read_u32(DATA_BASE).value_or(0);
    ASSERT_NE(buffer, 0u);
    EXPECT_EQ(memory.read_u32(DATA_BASE + 8).value_or(0), 160u);
    EXPECT_EQ(memory.read_u32(buffer).value_or(0), 1u);
    EXPECT_EQ(memory.read_u32(buffer + 8).value_or(0), 1u);
    EXPECT_EQ(marshaler.read_ansi_string(buffer + 20).value_or(""),
              "Console");
    EXPECT_EQ(marshaler.read_ansi_string(buffer + 53).value_or(""),
              "Bob");
    EXPECT_GT(memory.read_u64(buffer + 128).value_or(0), 0u); // CurrentTime
    EXPECT_EQ(memory.read_u32(buffer + 136).value_or(1), 0u);
    (void)allocations.global_free(buffer);
}

TEST_F(Wtsapi32Test, UnsupportedClassFailsAndLeavesOutputsUntouched) {
    // Measured (s7_misc_probe.expected): WTSIdleTime is ERROR_NOT_SUPPORTED
    // and a failure never writes *ppBuffer / *pBytesReturned.
    ASSERT_TRUE(memory.write_u32(DATA_BASE, 0xDEADBEEFu).is_ok());
    ASSERT_TRUE(memory.write_u32(DATA_BASE + 8, 0xAAAAAAAAu).is_ok());
    query(17);
    EXPECT_EQ(cpu.gpr.rax, 0u);
    EXPECT_EQ(get_last_error(), 50u);
    EXPECT_EQ(memory.read_u32(DATA_BASE).value_or(1), 0xDEADBEEFu);
    EXPECT_EQ(memory.read_u32(DATA_BASE + 8).value_or(1), 0xAAAAAAAAu);
    query(200);
    EXPECT_EQ(cpu.gpr.rax, 0u);
    EXPECT_EQ(get_last_error(), 87u);
    EXPECT_EQ(memory.read_u32(DATA_BASE).value_or(1), 0xDEADBEEFu);
}

TEST_F(Wtsapi32Test, UnknownSessionEchoesSessionIdAndFailsOtherClasses) {
    const auto query_session = [&](u32 session, u32 info_class) {
        push_arg(0, 0);
        push_arg(1, session);
        push_arg(2, info_class);
        push_arg(3, DATA_BASE);
        push_arg(4, DATA_BASE + 8);
        find_export("WTSQuerySessionInformationW")->handler(marshaler);
    };
    query_session(12345, 4);  // WTSSessionId
    ASSERT_EQ(cpu.gpr.rax, 1u);
    const GuestAddr buffer = memory.read_u32(DATA_BASE).value_or(0);
    EXPECT_EQ(memory.read_u32(DATA_BASE + 8).value_or(0), 4u);
    EXPECT_EQ(memory.read_u32(buffer).value_or(0), 12345u);
    (void)allocations.global_free(buffer);

    ASSERT_TRUE(memory.write_u32(DATA_BASE + 8, 0x77u).is_ok());
    query_session(12345, 8);  // WTSConnectState
    EXPECT_EQ(cpu.gpr.rax, 0u);
    EXPECT_EQ(get_last_error(), 2u);
    EXPECT_EQ(memory.read_u32(DATA_BASE + 8).value_or(0), 0x77u);

    query_session(0xFFFFFFFFu, 4);
    ASSERT_EQ(cpu.gpr.rax, 1u);
    const GuestAddr own = memory.read_u32(DATA_BASE).value_or(0);
    EXPECT_EQ(memory.read_u32(own).value_or(0), 1u);  // the console session
    (void)allocations.global_free(own);
}

TEST_F(Wtsapi32Test, MeasuredSizesAndLastErrors) {
    set_last_error(0xDEADu);
    query(29);  // WTSIsRemoteSession: a one-byte BOOLEAN, clears last error
    ASSERT_EQ(cpu.gpr.rax, 1u);
    EXPECT_EQ(memory.read_u32(DATA_BASE + 8).value_or(0), 1u);
    EXPECT_EQ(get_last_error(), 0u);
    (void)allocations.global_free(memory.read_u32(DATA_BASE).value_or(0));

    set_last_error(0xDEADu);
    query(8);  // WTSConnectState: success leaves ERROR_NO_TOKEN
    ASSERT_EQ(cpu.gpr.rax, 1u);
    EXPECT_EQ(get_last_error(), 1008u);
    (void)allocations.global_free(memory.read_u32(DATA_BASE).value_or(0));

    query(24);  // WTSSessionInfo (WTSINFOW): 216 bytes
    ASSERT_EQ(cpu.gpr.rax, 1u);
    EXPECT_EQ(memory.read_u32(DATA_BASE + 8).value_or(0), 216u);
    const GuestAddr info = memory.read_u32(DATA_BASE).value_or(0);
    EXPECT_EQ(memory.read_u32(info + 4).value_or(0), 1u);  // SessionId
    EXPECT_EQ(marshaler.read_wide_string(info + 32).value_or(""), "Console");
    (void)allocations.global_free(info);

    set_last_error(0xDEADu);
    query(4);  // WTSSessionId leaves the last error alone
    EXPECT_EQ(get_last_error(), 0xDEADu);
    (void)allocations.global_free(memory.read_u32(DATA_BASE).value_or(0));
}

TEST_F(Wtsapi32Test, InvalidOutputPointerFailsWithoutAllocation) {
    push_arg(0, 0);
    push_arg(1, 0xFFFFFFFFu);
    push_arg(2, 16);
    push_arg(3, 0);
    push_arg(4, DATA_BASE + 8);
    auto* entry = find_export("WTSQuerySessionInformationW");
    ASSERT_NE(entry, nullptr);
    entry->handler(marshaler);
    EXPECT_EQ(cpu.gpr.rax, 0u);
    EXPECT_EQ(get_last_error(), 1784u);  // ERROR_INVALID_USER_BUFFER
}
