/// @file test_enum_resource_names_exports.cpp
/// @brief EnumResourceNamesA/W must be registered (not the missing-export stub).

#include <gtest/gtest.h>
#include <vitreo/common.hpp>
#include <winapi/kernel32/kernel32_ext.hpp>
#include <core/memory/guest_memory.hpp>
#include <capsule/vfs/vfs.hpp>

#include <algorithm>

using namespace vitreo;
using namespace vitreo::winapi;

namespace {

[[nodiscard]] const ExportEntry* find_export(const DllExports& e,
                                             std::string_view name) {
    auto it = std::find_if(e.entries.begin(), e.entries.end(),
                           [&](const ExportEntry& x) { return x.name == name; });
    return (it == e.entries.end()) ? nullptr : &*it;
}

} // namespace

TEST(EnumResourceNamesExportsTest, AnsiAndWideVariantsAreRegistered) {
    GuestMemory memory;
    capsule::Vfs vfs;
    Kernel32ExtManager mgr(memory, vfs);
    auto exports = create_kernel32_ext_thunks(mgr);

    const auto* a = find_export(exports, "EnumResourceNamesA");
    const auto* w = find_export(exports, "EnumResourceNamesW");

    ASSERT_NE(a, nullptr) << "EnumResourceNamesA missing → ANSI callers hit "
                             "the missing-export stub";
    ASSERT_NE(w, nullptr);
    EXPECT_TRUE(static_cast<bool>(a->handler));
    EXPECT_TRUE(static_cast<bool>(w->handler));
    EXPECT_EQ(a->stdcall_bytes, 16u);
    EXPECT_EQ(w->stdcall_bytes, 16u);
}

TEST(EnumResourceNamesExportsTest, EnumResourceLanguagesAnsiAndWideAreRegistered) {
    GuestMemory memory;
    capsule::Vfs vfs;
    Kernel32ExtManager mgr(memory, vfs);
    auto exports = create_kernel32_ext_thunks(mgr);

    const auto* a = find_export(exports, "EnumResourceLanguagesA");
    const auto* w = find_export(exports, "EnumResourceLanguagesW");

    ASSERT_NE(a, nullptr) << "EnumResourceLanguagesA missing → ANSI callers hit "
                             "the missing-export stub";
    ASSERT_NE(w, nullptr);
    EXPECT_TRUE(static_cast<bool>(a->handler));
    EXPECT_TRUE(static_cast<bool>(w->handler));
    EXPECT_EQ(a->stdcall_bytes, 20u);
    EXPECT_EQ(w->stdcall_bytes, 20u);
}
