/// @file test_runtime_function_lookup.cpp
/// @brief RtlLookupFunctionEntry's .pdata search: sorted binary search, and a
///        ControlPc no entry covers reported as a miss.
///
/// Contract source: Microsoft PE/COFF ("The .pdata Section" — an x64 image's
/// RUNTIME_FUNCTION records are sorted by BeginAddress so the OS can binary
/// search them) and Microsoft Learn, RtlLookupFunctionEntry (searches the
/// function table of the image containing ControlPc; returns NULL for a leaf
/// function that has no entry).

#include <gtest/gtest.h>
#include <vitreo/common.hpp>
#include <winapi/kernel32/module/module.hpp>
#include <winapi/core/thunk_registry.hpp>
#include <core/memory/guest_memory.hpp>
#include <capsule/vfs/host_filesystem.hpp>

using namespace vitreo;
using namespace vitreo::winapi;

namespace {

constexpr GuestAddr kImageBase = 0x140000000ull;
constexpr u32 kPdataRva = 0x1000;
constexpr u32 kEntryCount = 4096;

/// Function N occupies [0x10000 + N*0x40, +0x20) — so every other 0x20 window
/// is a gap that no RUNTIME_FUNCTION covers.
constexpr u32 func_begin(u32 n) { return 0x10000u + n * 0x40u; }
constexpr u32 func_end(u32 n) { return func_begin(n) + 0x20u; }

class RuntimeFunctionLookupTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(m_memory.map_region(kImageBase, 0x400000,
                                        MemoryProtection::ReadWrite).is_ok());
        for (u32 i = 0; i < kEntryCount; ++i) {
            const GuestAddr e =
                kImageBase + kPdataRva + static_cast<GuestAddr>(i) * 12;
            ASSERT_TRUE(m_memory.write_u32(e + 0, func_begin(i)).is_ok());
            ASSERT_TRUE(m_memory.write_u32(e + 4, func_end(i)).is_ok());
            ASSERT_TRUE(m_memory.write_u32(e + 8, 0x200000u + i).is_ok());
        }
        m_modules = std::make_unique<ModuleManager>(m_registry);
        m_modules->attach_dynamic_loader(m_memory, m_vfs, true);
        m_modules->set_main_module(kImageBase, "C:\\app.exe", kPdataRva,
                                   kEntryCount * 12);
    }

    static GuestAddr entry_addr(u32 index) {
        return kImageBase + kPdataRva + static_cast<GuestAddr>(index) * 12;
    }

    GuestMemory m_memory;
    capsule::Vfs m_vfs;
    ThunkRegistry m_registry{0x70000000};
    std::unique_ptr<ModuleManager> m_modules;
};

}  // namespace

TEST_F(RuntimeFunctionLookupTest, FindsFirstMiddleAndLastEntries) {
    for (u32 i : {0u, 1u, kEntryCount / 2, kEntryCount - 2, kEntryCount - 1}) {
        GuestAddr image_base = 0;
        EXPECT_EQ(m_modules->lookup_runtime_function_entry(
                      kImageBase + func_begin(i), &image_base),
                  entry_addr(i)) << "entry " << i;
        EXPECT_EQ(image_base, kImageBase);
    }
}

TEST_F(RuntimeFunctionLookupTest, FindsInteriorAndLastByteOfARange) {
    const u32 i = 1234;
    EXPECT_EQ(m_modules->lookup_runtime_function_entry(
                  kImageBase + func_begin(i) + 0x10),
              entry_addr(i));
    EXPECT_EQ(m_modules->lookup_runtime_function_entry(
                  kImageBase + func_end(i) - 1),
              entry_addr(i));
}

TEST_F(RuntimeFunctionLookupTest, EndAddressIsExclusive) {
    const u32 i = 77;
    EXPECT_EQ(m_modules->lookup_runtime_function_entry(
                  kImageBase + func_end(i)),
              0u);
}

TEST_F(RuntimeFunctionLookupTest, LeafPcInAGapHasNoEntry) {
    // Between function 10 and function 11 — covered by neither.
    EXPECT_EQ(m_modules->lookup_runtime_function_entry(
                  kImageBase + func_end(10) + 4),
              0u);
}

TEST_F(RuntimeFunctionLookupTest, PcBeforeAndAfterTheTableHasNoEntry) {
    EXPECT_EQ(m_modules->lookup_runtime_function_entry(
                  kImageBase + func_begin(0) - 4),
              0u);
    EXPECT_EQ(m_modules->lookup_runtime_function_entry(
                  kImageBase + func_end(kEntryCount - 1) + 0x1000),
              0u);
}

TEST_F(RuntimeFunctionLookupTest, PcBelowTheImageBaseHasNoEntry) {
    EXPECT_EQ(m_modules->lookup_runtime_function_entry(kImageBase - 0x1000), 0u);
}

// Precedence against dynamic tables, measured on Windows by
// tools/oracle_probe/function_table_precedence_probe.c: a PC inside a loaded
// image is answered by that image's table alone -- a callback whose range also
// covers the image is not called, even for a PC the image has no entry for.
// PCs outside every image go to the covering callback.
class RuntimeFunctionPrecedenceTest : public RuntimeFunctionLookupTest {
protected:
    static constexpr GuestAddr kHeap = kImageBase + 0x400000;
    static constexpr GuestAddr kCallbackEntry = kHeap + 0x800;

    void SetUp() override {
        RuntimeFunctionLookupTest::SetUp();
        m_modules->register_main_module_exports(kImageBase, "C:\\app.exe",
                                                kImageBase + 0x400000, 64, {});
        ASSERT_TRUE(m_memory.map_region(kHeap, 0x10000,
                                        MemoryProtection::ReadWrite).is_ok());
        m_modules->set_runtime_function_callback_runner(
            [this](GuestAddr, GuestAddr, GuestAddr) -> GuestAddr {
                ++m_calls;
                return kCallbackEntry;
            });
        ASSERT_TRUE(m_modules->add_dynamic_function_table_callback(
            kImageBase | 3, kImageBase, 0x410000, 0x7000, 0));
    }

    int m_calls = 0;
};

TEST_F(RuntimeFunctionPrecedenceTest, ImageTableWinsOverCoveringCallback) {
    GuestAddr image_base = 0;
    EXPECT_EQ(m_modules->lookup_runtime_function_entry(
                  kImageBase + func_begin(5) + 4, &image_base),
              entry_addr(5));
    EXPECT_EQ(image_base, kImageBase);
    EXPECT_EQ(m_calls, 0);
}

TEST_F(RuntimeFunctionPrecedenceTest, LeafPcInsideImageDoesNotConsultCallback) {
    EXPECT_EQ(m_modules->lookup_runtime_function_entry(kImageBase + 0x10), 0u);
    EXPECT_EQ(m_modules->lookup_runtime_function_entry(
                  kImageBase + func_end(10) + 4),
              0u);
    EXPECT_EQ(m_calls, 0);
}

TEST_F(RuntimeFunctionPrecedenceTest, PcOutsideImagesUsesCallback) {
    GuestAddr image_base = 0;
    EXPECT_EQ(m_modules->lookup_runtime_function_entry(kHeap + 0x100, &image_base),
              kCallbackEntry);
    EXPECT_EQ(image_base, kImageBase);
    EXPECT_EQ(m_calls, 1);
}

TEST_F(RuntimeFunctionPrecedenceTest, ThreadRunnerOverridesDefault) {
    int thread_calls = 0;
    const ModuleManager::RuntimeFunctionCallbackRunner runner =
        [&](GuestAddr, GuestAddr, GuestAddr) -> GuestAddr {
            ++thread_calls;
            return 0;
        };
    {
        const ModuleManager::ScopedThreadCallbackRunner scoped{&runner};
        EXPECT_EQ(m_modules->lookup_runtime_function_entry(kHeap + 0x100), 0u);
    }
    EXPECT_EQ(thread_calls, 1);
    EXPECT_EQ(m_calls, 0);
    EXPECT_EQ(m_modules->lookup_runtime_function_entry(kHeap + 0x100),
              kCallbackEntry);
    EXPECT_EQ(m_calls, 1);
}
