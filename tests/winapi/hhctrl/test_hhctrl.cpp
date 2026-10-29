/// @file test_hhctrl.cpp
/// @brief Unit tests for HHCTRL.OCX HTML Help stubs

#include <gtest/gtest.h>
#include <vitreo/common.hpp>
#include <winapi/ui/hhctrl/hhctrl.hpp>

#include <algorithm>

using namespace vitreo::winapi;

// =============================================================================
// Thunk Table Tests
// =============================================================================

TEST(HhctrlTest, ThunksContainTwoEntries) {
    DllExports exports = create_hhctrl_thunks();
    EXPECT_EQ(exports.entries.size(), 2u);
}

TEST(HhctrlTest, ThunkHtmlHelpA) {
    DllExports exports = create_hhctrl_thunks();
    auto it = std::find_if(exports.entries.begin(), exports.entries.end(),
        [](const ExportEntry& e) { return e.name == "HtmlHelpA"; });
    ASSERT_NE(it, exports.entries.end());
    EXPECT_EQ(it->ordinal, 14u);
    EXPECT_EQ(it->stdcall_bytes, 16u);
}

TEST(HhctrlTest, ThunkHtmlHelpW) {
    DllExports exports = create_hhctrl_thunks();
    auto it = std::find_if(exports.entries.begin(), exports.entries.end(),
        [](const ExportEntry& e) { return e.name == "HtmlHelpW"; });
    ASSERT_NE(it, exports.entries.end());
    EXPECT_EQ(it->ordinal, 15u);
    EXPECT_EQ(it->stdcall_bytes, 16u);
}

TEST(HhctrlTest, AllHandlersAreCallable) {
    DllExports exports = create_hhctrl_thunks();
    for (const auto& entry : exports.entries) {
        EXPECT_TRUE(entry.handler != nullptr) << "Handler for " << entry.name << " is null";
    }
}
