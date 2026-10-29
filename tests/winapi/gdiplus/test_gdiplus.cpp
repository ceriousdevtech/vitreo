// SPDX-License-Identifier: LGPL-2.1-only
/// @file test_gdiplus.cpp
/// @brief The gdiplus.dll export table: every export registered, with the
/// ordinals of each bitness and the x86 stdcall cleanup of its signature,
/// and every export implemented by an area (gdiplus_<area>.cpp). The areas'
/// contracts are tested in test_gdiplus_<area>.cpp.

#include "gdiplus_fixture.hpp"

#include <set>

namespace vitreo::winapi::gdip_test {
namespace {

class GdipExportTableTest : public GdipTest {};

const ExportEntry* find(const DllExports& e, const std::string& n) {
    for (const ExportEntry& entry : e.entries)
        if (entry.name == n) return &entry;
    return nullptr;
}

TEST_P(GdipExportTableTest, EveryGdiplusDllExportIsRegisteredWithItsOrdinalPerBitness) {
    // gdiplus.dll numbers the same 630 names differently per bitness
    // (gdiplus_export_table.inc, generated from both DLLs' export tables).
    const DllExports exports = create_gdiplus_thunks(mgr, x64());
    const std::vector<std::string> names = gdiplus_export_names();
    EXPECT_EQ(names.size(), 630u);
    for (const std::string& name : names) ASSERT_NE(find(exports, name), nullptr) << name;
    // Spot rows from the two export tables (llvm-objdump -p gdiplus.dll).
    EXPECT_EQ(find(exports, "GdipAddPathClosedCurve")->ordinal, x64() ? 7 : 9);
    // x86 cleanup comes from the SDK gdiplus.lib decorations
    // (_GdipAddPathArc@28, _GdipDrawImageRectRectI@56).
    EXPECT_EQ(find(exports, "GdipAddPathArc")->stdcall_bytes, 28);
    EXPECT_EQ(find(exports, "GdipDrawImageRectRectI")->stdcall_bytes, 56);
}

TEST_P(GdipExportTableTest, EveryExportHasAnAreaImplementation) {
    std::set<std::string> missing;
    for (const std::string& name : gdiplus_export_names())
        if (!handlers.contains(name)) missing.insert(name);
    EXPECT_TRUE(missing.empty()) << missing.size() << " exports without a handler, first: "
                                 << (missing.empty() ? "" : *missing.begin());
}

TEST_P(GdipExportTableTest, HandlersReachEveryArgumentKindInBothBitnesses) {
    // GdipCreateSolidFill(ARGB, GpSolidFill**): an INT and a pointer-width out.
    poison(OUT, 8);
    EXPECT_EQ(call("GdipCreateSolidFill", {I(0xFF102030), I(OUT)}), Status::Ok);
    const u64 brush = read_handle(OUT);
    EXPECT_NE(brush, 0u);
    if (x64()) EXPECT_EQ(brush >> 32, 0u) << "high dword of the out pointer left unwritten";
    u32 argb = 0;
    EXPECT_EQ(call("GdipGetSolidFillColor", {I(brush), I(OUT2)}), Status::Ok);
    argb = memory.read_u32(OUT2).value();
    EXPECT_EQ(argb, 0xFF102030u);
}
INSTANTIATE_GDIP_BITNESS(GdipExportTableTest);

}  // namespace
}  // namespace vitreo::winapi::gdip_test
