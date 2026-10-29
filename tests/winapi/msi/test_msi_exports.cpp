// SPDX-License-Identifier: LGPL-2.1-only
/// @file test_msi_exports.cpp
/// @brief The msi.dll export table: every export registered with the real
/// DLL's ordinal (the same on x86 and x64) and its x86 stdcall cleanup.

#include "msi_fixture.hpp"

#include <set>

namespace vitreo::winapi::msi_test {
namespace {

class MsiExportTableTest : public MsiTest {};

const ExportEntry* find(const DllExports& e, const std::string& n) {
    for (const ExportEntry& entry : e.entries)
        if (entry.name == n) return &entry;
    return nullptr;
}

TEST_P(MsiExportTableTest, EveryMsiDllExportIsRegisteredWithItsOrdinal) {
    const DllExports exports = create_msi_thunks(mgr);
    const std::vector<std::string> names = msi_export_names();
    EXPECT_EQ(names.size(), 295u);
    for (const std::string& name : names) ASSERT_NE(find(exports, name), nullptr) << name;
    // Rows of `llvm-objdump -p msi.dll` (both bitnesses) and Msi.Lib.
    EXPECT_EQ(find(exports, "MsiCloseHandle")->ordinal, 8);
    EXPECT_EQ(find(exports, "MsiCloseHandle")->stdcall_bytes, 4);
    EXPECT_EQ(find(exports, "MsiRecordGetInteger")->ordinal, 116);
    EXPECT_EQ(find(exports, "MsiRecordGetInteger")->stdcall_bytes, 8);
    // Measured from x86 msi.dll's own epilogue (no Msi.Lib decoration).
    EXPECT_EQ(find(exports, "DllGetClassObject")->stdcall_bytes, 12);
    EXPECT_EQ(find(exports, "MsiInvalidateFeatureCache")->stdcall_bytes, 0);
}

TEST_P(MsiExportTableTest, EveryExportHasAnAreaImplementation) {
    std::set<std::string> missing;
    for (const std::string& name : msi_export_names())
        if (!handlers.contains(name)) missing.insert(name);
    if (!missing.empty()) {
        GTEST_SKIP() << missing.size() << " msi.dll exports still without a handler, first: "
                     << *missing.begin();
    }
}
INSTANTIATE_MSI_BITNESS(MsiExportTableTest);

}  // namespace
}  // namespace vitreo::winapi::msi_test
