/// @file test_wsock32.cpp
/// @brief Unit tests for WSOCK32.dll Winsock stubs

#include <gtest/gtest.h>
#include <vitreo/common.hpp>
#include <winapi/networking/wsock32/wsock32.hpp>
#include <winapi/core/marshaler.hpp>
#include <core/cpu/state/cpu_state.hpp>
#include <core/memory/guest_memory.hpp>

#include <algorithm>
#include <set>

using namespace vitreo;
using namespace vitreo::winapi;

// =============================================================================
// Test Fixture
// =============================================================================

class Wsock32ManagerTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto res = memory_.map_region(MEM_BASE, 0x10000, MemoryProtection::ReadWrite);
        ASSERT_TRUE(res.is_ok());

        auto stack_res = memory_.map_region(STACK_BASE, 0x1000, MemoryProtection::ReadWrite);
        ASSERT_TRUE(stack_res.is_ok());

        cpu_.gpr.rsp = STACK_BASE + 0x800;
    }

    static constexpr GuestAddr MEM_BASE   = 0x1000;
    static constexpr GuestAddr STACK_BASE = 0x80000;

    static constexpr GuestAddr WSADATA_ADDR = MEM_BASE + 0x100;

    GuestMemory memory_;
    cpu::CpuState cpu_;
    Wsock32Manager mgr_;
};

// =============================================================================
// WSAStartup Tests
// =============================================================================

TEST_F(Wsock32ManagerTest, WsaStartupReturnsZero) {
    Marshaler m(memory_);
    m.attach_cpu(cpu_);

    u32 rc = mgr_.wsa_startup(m, 0x0202, WSADATA_ADDR);
    EXPECT_EQ(rc, 0u);  // success
}

TEST_F(Wsock32ManagerTest, WsaStartupWritesVersionBack) {
    Marshaler m(memory_);
    m.attach_cpu(cpu_);

    u16 requested = 0x0202;
    mgr_.wsa_startup(m, requested, WSADATA_ADDR);

    u16 version = 0;
    version = memory_.read_u16(WSADATA_ADDR).value();
    EXPECT_EQ(version, requested);
}

TEST_F(Wsock32ManagerTest, WsaStartupWritesHighVersion) {
    Marshaler m(memory_);
    m.attach_cpu(cpu_);

    mgr_.wsa_startup(m, 0x0101, WSADATA_ADDR);

    u16 high_version = 0;
    high_version = memory_.read_u16(WSADATA_ADDR + 2).value();
    EXPECT_EQ(high_version, 0x0202u);
}

TEST_F(Wsock32ManagerTest, WsaStartupZeroesWsaData) {
    Marshaler m(memory_);
    m.attach_cpu(cpu_);

    // Pre-fill with 0xFF
    std::vector<u8> filler(400, 0xFF);
    (void)memory_.write_bytes(WSADATA_ADDR,
                               reinterpret_cast<const Byte*>(filler.data()), 400);

    mgr_.wsa_startup(m, 0x0202, WSADATA_ADDR);

    // Bytes beyond offset 4 should be zeroed (offsets 0-3 are version fields)
    u8 byte_at_10 = 0xFF;
    byte_at_10 = memory_.read_u8(WSADATA_ADDR + 10).value();
    EXPECT_EQ(byte_at_10, 0u);
}

TEST_F(Wsock32ManagerTest, WsaStartupWithNullPtr) {
    Marshaler m(memory_);
    m.attach_cpu(cpu_);

    u32 rc = mgr_.wsa_startup(m, 0x0202, 0);
    EXPECT_EQ(rc, 0u);  // still returns success
}

// =============================================================================
// Thunk Table Tests
// =============================================================================

TEST_F(Wsock32ManagerTest, ThunksContainStartupAndOrdinalFallback) {
    DllExports exports = create_wsock32_thunks(mgr_);
    EXPECT_EQ(exports.entries.size(), 2u);

    // A duplicate registration would let a later entry silently shadow an
    // earlier one, so assert on presence rather than only on the count.
    std::set<std::string> seen;
    for (const auto& e : exports.entries) {
        if (e.name.empty()) continue;  // ordinal-only fallbacks
        EXPECT_TRUE(seen.insert(e.name).second)
            << "duplicate export registration shadows an earlier one: "
            << e.name;
    }
    for (const char* required : {"WSAStartup", "WSACancelBlockingCall"}) {
        EXPECT_TRUE(seen.count(required) != 0)
            << "missing wsock32 export: " << required;
    }
}

TEST_F(Wsock32ManagerTest, ThunkWSAStartup) {
    DllExports exports = create_wsock32_thunks(mgr_);

    ASSERT_GE(exports.entries.size(), 1u);
    const auto& entry = exports.entries[0];

    EXPECT_EQ(entry.name, "WSAStartup");
    EXPECT_EQ(entry.ordinal, 115u);
    EXPECT_EQ(entry.stdcall_bytes, 8u);
}
