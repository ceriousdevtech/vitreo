/// @file test_authz.cpp
/// @brief Unit tests for AUTHZ.dll authorization stubs

#include <gtest/gtest.h>
#include <vitreo/common.hpp>
#include <winapi/system/authz/authz.hpp>
#include <winapi/core/marshaler.hpp>
#include <core/cpu/state/cpu_state.hpp>
#include <core/memory/guest_memory.hpp>

#include <algorithm>

using namespace vitreo;
using namespace vitreo::winapi;

// =============================================================================
// Test Fixture
// =============================================================================

class AuthzManagerTest : public ::testing::Test {
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

    GuestMemory memory_;
    cpu::CpuState cpu_;
    AuthzManager mgr_;
};

// =============================================================================
// Manager Method Tests
// =============================================================================

TEST_F(AuthzManagerTest, InitializeClaimDictionaryReturnsTrue) {
    u32 rc = mgr_.initialize_claim_dictionary(0x1000);
    EXPECT_EQ(rc, 1u);  // TRUE
}

TEST_F(AuthzManagerTest, GetClaimDefinitionsReturnsZero) {
    u32 rc = mgr_.get_claim_definitions(0, 0, 0);
    EXPECT_EQ(rc, 0u);
}

TEST_F(AuthzManagerTest, FreeClaimDefinitionsDoesNotCrash) {
    mgr_.free_claim_definitions(0x1234);
    SUCCEED();
}

TEST_F(AuthzManagerTest, FreeClaimDictionaryDoesNotCrash) {
    mgr_.free_claim_dictionary(0x1234);
    SUCCEED();
}

// =============================================================================
// Thunk Table Tests
// =============================================================================

TEST_F(AuthzManagerTest, ThunksContainFourEntries) {
    DllExports exports = create_authz_thunks(mgr_);
    EXPECT_EQ(exports.entries.size(), 4u);
}

TEST_F(AuthzManagerTest, ThunkFreeClaimDefinitions) {
    DllExports exports = create_authz_thunks(mgr_);
    auto it = std::find_if(exports.entries.begin(), exports.entries.end(),
        [](const ExportEntry& e) { return e.name == "FreeClaimDefinitions"; });
    ASSERT_NE(it, exports.entries.end());
    EXPECT_EQ(it->ordinal, 1u);
    EXPECT_EQ(it->stdcall_bytes, 4u);
}

TEST_F(AuthzManagerTest, ThunkInitializeClaimDictionary) {
    DllExports exports = create_authz_thunks(mgr_);
    auto it = std::find_if(exports.entries.begin(), exports.entries.end(),
        [](const ExportEntry& e) { return e.name == "InitializeClaimDictionary"; });
    ASSERT_NE(it, exports.entries.end());
    EXPECT_EQ(it->ordinal, 2u);
    EXPECT_EQ(it->stdcall_bytes, 4u);
}

TEST_F(AuthzManagerTest, ThunkGetClaimDefinitions) {
    DllExports exports = create_authz_thunks(mgr_);
    auto it = std::find_if(exports.entries.begin(), exports.entries.end(),
        [](const ExportEntry& e) { return e.name == "GetClaimDefinitions"; });
    ASSERT_NE(it, exports.entries.end());
    EXPECT_EQ(it->ordinal, 3u);
    EXPECT_EQ(it->stdcall_bytes, 12u);
}

TEST_F(AuthzManagerTest, ThunkFreeClaimDictionary) {
    DllExports exports = create_authz_thunks(mgr_);
    auto it = std::find_if(exports.entries.begin(), exports.entries.end(),
        [](const ExportEntry& e) { return e.name == "FreeClaimDictionary"; });
    ASSERT_NE(it, exports.entries.end());
    EXPECT_EQ(it->ordinal, 4u);
    EXPECT_EQ(it->stdcall_bytes, 4u);
}
