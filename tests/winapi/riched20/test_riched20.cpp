#include "winapi/ui/riched20/riched20.hpp"

#include <gtest/gtest.h>

using namespace vitreo::winapi;

TEST(Riched20Test, CreateThunksRegistersDllGetVersion) {
    auto exports = create_riched20_thunks();
    ASSERT_FALSE(exports.entries.empty());
    bool found = false;
    for (const auto& e : exports.entries) {
        if (e.name == "DllGetVersion") {
            found = true;
            break;
        }
    }
    EXPECT_TRUE(found);
}
