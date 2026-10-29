/// @file test_mswsock.cpp
/// @brief Unit tests for mswsock.dll stubs

#include <gtest/gtest.h>
#include <vitreo/common.hpp>
#include <winapi/networking/mswsock/mswsock.hpp>
#include <winapi/networking/ws2_32/ws2_32.hpp>

#include <algorithm>

using namespace vitreo::winapi;

// =============================================================================
// Thunk Table Tests
// =============================================================================

TEST(MswsockTest, ThunksContainAcceptExExports) {
    MswsockManager mgr;
    Ws2Manager ws2;
    DllExports exports = create_mswsock_thunks(mgr, ws2);
    EXPECT_EQ(exports.entries.size(), 3u);

    auto has = [&](const char* name) {
        return std::any_of(exports.entries.begin(), exports.entries.end(),
                           [&](const ExportEntry& e) { return e.name == name; });
    };
    EXPECT_TRUE(has("GetSocketErrorMessageW"));
    EXPECT_TRUE(has("AcceptEx"));
    EXPECT_TRUE(has("GetAcceptExSockaddrs"));
}

TEST(MswsockTest, ThunkGetSocketErrorMessageW) {
    MswsockManager mgr;
    Ws2Manager ws2;
    DllExports exports = create_mswsock_thunks(mgr, ws2);

    auto entry = std::find_if(
        exports.entries.begin(), exports.entries.end(),
        [](const ExportEntry& e) { return e.name == "GetSocketErrorMessageW"; });
    ASSERT_NE(entry, exports.entries.end());
    EXPECT_EQ(entry->ordinal, 1u);
    EXPECT_EQ(entry->stdcall_bytes, 0u);
}

TEST(MswsockTest, AcceptExCleanupMatchesWs2) {
    MswsockManager mgr;
    Ws2Manager ws2;
    DllExports exports = create_mswsock_thunks(mgr, ws2);

    auto accept = std::find_if(
        exports.entries.begin(), exports.entries.end(),
        [](const ExportEntry& e) { return e.name == "AcceptEx"; });
    auto getaddrs = std::find_if(
        exports.entries.begin(), exports.entries.end(),
        [](const ExportEntry& e) { return e.name == "GetAcceptExSockaddrs"; });
    ASSERT_NE(accept, exports.entries.end());
    ASSERT_NE(getaddrs, exports.entries.end());
    EXPECT_EQ(accept->stdcall_bytes, 32u);
    EXPECT_EQ(getaddrs->stdcall_bytes, 32u);
    EXPECT_TRUE(accept->handler != nullptr);
    EXPECT_TRUE(getaddrs->handler != nullptr);
}
