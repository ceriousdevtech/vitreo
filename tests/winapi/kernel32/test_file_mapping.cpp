/// @file test_file_mapping.cpp
/// @brief Unit tests for memory-mapped files (WIN32-066)

#include <gtest/gtest.h>
#include <vitreo/common.hpp>
#include <capsule/vfs/host_filesystem.hpp>
#include <capsule/vfs/vfs.hpp>
#include <core/cpu/state/cpu_state.hpp>
#include <ipc/ipc_gate.hpp>
#include <winapi/kernel32/memory/file_mapping.hpp>
#include <winapi/kernel32/memory/virtual_query.hpp>
#include <winapi/kernel32/memory/memory.hpp>
#include <winapi/kernel32/filesystem/file.hpp>
#include <winapi/kernel32/error.hpp>
#include <winapi/kernel32/handle.hpp>
#include <winapi/kernel32/kernel32_ext.hpp>
#include <winapi/kernel32/synchronization/sync.hpp>
#include <winapi/core/marshaler.hpp>
#include <core/memory/guest_memory.hpp>
#include <array>
#include <atomic>
#include <filesystem>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <optional>
#include <thread>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <unistd.h>
#include <vector>

using namespace vitreo;
using namespace vitreo::winapi;

// =============================================================================
// Test Fixture
// =============================================================================

class FileMappingTest : public ::testing::Test {
protected:
    void SetUp() override {
        mem     = std::make_unique<GuestMemory>();
        handles = std::make_unique<HandleTable>();
        // Occupy the historical preferred view base. MapViewOfFile must choose
        // a free range rather than treating unrelated mapped memory as its
        // section storage.
        auto res = mem->map_region(0x20000000, 0x100000, MemoryProtection::ReadWrite);
        ASSERT_FALSE(res.is_err()) << "failed to pre-map view arena";
        mgr = std::make_unique<FileMappingManager>(*handles, *mem);
    }

    std::unique_ptr<GuestMemory>       mem;
    std::unique_ptr<HandleTable>       handles;
    std::unique_ptr<FileMappingManager> mgr;
};

// =============================================================================
// Concurrency: the section tables are shared by every guest thread
// =============================================================================

// Learn: CreateFileMapping, OpenFileMapping, DuplicateHandle, MapViewOfFile and
// CloseHandle are kernel-object operations that any thread may issue at any
// time. Steam's CEF renderer and GPU process died with SIGSEGV on torn
// std::unordered_map nodes (map_view_impl / duplicate_handle reading a table
// another thread was inserting into or erasing from). Eight threads hammer
// the same tables; before the table mutex this crashed within a second.
TEST_F(FileMappingTest, TablesSurviveConcurrentCreateOpenDuplicateMapClose) {
    handles->set_close_callback([this](Handle h, HandleType type, void*) {
        return mgr->on_handle_close(h, type);
    });
    constexpr int kThreads = 8;
    constexpr int kRounds = 3000;
    std::atomic<int> failures{0};
    std::vector<std::thread> workers;
    for (int t = 0; t < kThreads; ++t) {
        workers.emplace_back([&, t] {
            const std::string name = "Local\\vitreo_mapping_race_" + std::to_string(t % 3);
            // Keep most handles open so the table keeps growing and
            // rehashing under the other threads' lookups, which is the
            // torn-node shape the crashes had.
            std::vector<Handle> kept;
            for (int i = 0; i < kRounds; ++i) {
                Handle h = mgr->create_file_mapping(
                    FILE_MAPPING_INVALID_FILE, 0x04, 0x1000,
                    (i % 2) ? name : std::string());
                if (h == NULL_HANDLE) { ++failures; continue; }
                Handle opened = (i % 2)
                    ? mgr->open_file_mapping(0x0006, name) : NULL_HANDLE;
                Handle dup = mgr->duplicate_handle(h, 0, HandleAccess::All);
                if (dup == NULL_HANDLE) ++failures;
                if (i % 16 == 0) {
                    GuestAddr view = mgr->map_view_of_file(h, 0x0006, 0, 0x1000);
                    if (view == 0) ++failures;
                    else if (!mgr->unmap_view_of_file(view)) ++failures;
                }
                if (dup != NULL_HANDLE && !handles->close(dup)) ++failures;
                if (opened != NULL_HANDLE && !handles->close(opened)) ++failures;
                if (i % 4 == 0) kept.push_back(h);
                else if (!handles->close(h)) ++failures;
            }
            for (Handle h : kept) {
                if (!handles->close(h)) ++failures;
            }
        });
    }
    for (auto& w : workers) w.join();
    EXPECT_EQ(failures.load(), 0);
}

// =============================================================================
// CreateFileMappingW Tests
// =============================================================================

TEST_F(FileMappingTest, CreateAnonymous_ReturnsValidHandle) {
    Handle h = mgr->create_file_mapping(FILE_MAPPING_INVALID_FILE, 0x04, 0x1000, "");
    EXPECT_NE(h, NULL_HANDLE);
}

TEST_F(FileMappingTest, CreateAnonymousZeroSize_ReturnsNull) {
    Handle h = mgr->create_file_mapping(FILE_MAPPING_INVALID_FILE, 0x04, 0, "");
    EXPECT_EQ(h, NULL_HANDLE);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_PARAMETER);
}

TEST_F(FileMappingTest, CreateNamed_ReturnsValidHandle) {
    Handle h = mgr->create_file_mapping(FILE_MAPPING_INVALID_FILE, 0x04, 0x1000,
                                         "Local\\TestMapping");
    EXPECT_NE(h, NULL_HANDLE);
}

// Audit 01 CreateFileMappingW #3: second Create with same name returns the
// existing object + ERROR_ALREADY_EXISTS; Local\X == X.
TEST_F(FileMappingTest, CreateNamedReuseReturnsExistingWithAlreadyExists) {
    Handle h1 = mgr->create_file_mapping(FILE_MAPPING_INVALID_FILE, 0x04, 0x2000,
                                         "Local\\ReuseMap");
    ASSERT_NE(h1, NULL_HANDLE);
    set_last_error(0xDEAD);
    Handle h2 = mgr->create_file_mapping(FILE_MAPPING_INVALID_FILE, 0x04, 0x8000,
                                          "ReuseMap");  // same after Local\ strip
    ASSERT_NE(h2, NULL_HANDLE);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_ALREADY_EXISTS);
    // Existing size wins (0x2000), not the second call's 0x8000.
    const GuestAddr v1 = mgr->map_view_of_file(h1, 0x0004, 0, 0);
    const GuestAddr v2 = mgr->map_view_of_file(h2, 0x0004, 0, 0);
    ASSERT_NE(v1, 0u);
    ASSERT_NE(v2, 0u);
    // Both handles refer to the same section object (same weak name entry).
    EXPECT_NE(h1, h2);
}

// =============================================================================
// MapViewOfFile Tests
// =============================================================================

TEST_F(FileMappingTest, MapViewStaysBelowNonLargeAddressAwareLimit) {
    // Learn GetSystemInfo lpMaximumApplicationAddress: 32-bit processes
    // without IMAGE_FILE_LARGE_ADDRESS_AWARE are limited to 0x7FFEFFFF.
    // MapViewOfFile must not return a pointer in the upper 2 GB.
    constexpr GuestAddr kHint = 0x79000000;
    constexpr usize kHintSpan = 0x07000000;
    ASSERT_TRUE(mem->map_region(kHint, kHintSpan, MemoryProtection::ReadWrite)
                    .is_ok());

    Handle h = mgr->create_file_mapping(FILE_MAPPING_INVALID_FILE, 0x04, 0x1000, "");
    ASSERT_NE(h, NULL_HANDLE);
    const GuestAddr addr = mgr->map_view_of_file(h, 0x0006, 0, 0);
    ASSERT_NE(addr, 0u);
    EXPECT_LE(addr, 0x7FFEFFFFull - 0x1000ull);
    EXPECT_LT(addr + 0x1000ull, 0x80000000ull);
}

TEST_F(FileMappingTest, LargeMapViewPrefersAddressBelowTebIsland) {
    // Occupy 0x79000000..0x7F000000 so an 8 MiB view cannot fit under the
    // TEB island floor from the default high hint. Leave 0x7F000000+ free so
    // a naive finder would place above the island; MapViewOfFile must instead
    // fall back below 0x7F000000.
    constexpr GuestAddr kHint = 0x79000000;
    constexpr usize kHintSpan = 0x06000000; // through 0x7F000000
    ASSERT_TRUE(mem->map_region(kHint, kHintSpan, MemoryProtection::ReadWrite)
                    .is_ok());

    FileMappingManager x64_mgr(*handles, *mem, 0x00007FFF'FFFEFFFFULL);
    constexpr u64 kViewSize = 8ull * 1024ull * 1024ull;
    Handle h =
        x64_mgr.create_file_mapping(FILE_MAPPING_INVALID_FILE, 0x04, kViewSize, "");
    ASSERT_NE(h, NULL_HANDLE);
    const GuestAddr addr = x64_mgr.map_view_of_file(h, 0x0004, 0, 0);
    ASSERT_NE(addr, 0u);
    EXPECT_LT(addr, 0x7F000000ull);
    EXPECT_LE(addr + kViewSize, 0x7F000000ull);
}

TEST_F(FileMappingTest, MapViewMayUseUpper2GbWhenLargeAddressAware) {
    FileMappingManager laa(*handles, *mem, 0xFFFE'FFFFULL);
    // Exhaust the preferred low arenas and the 0x79000000 hint so the only
    // remaining fit under the 4 GB LAA ceiling is in the upper 2 GB.
    // Fixture already owns 0x20000000..0x20100000.
    ASSERT_TRUE(mem->map_region(0x10000000, 0x10000000, MemoryProtection::ReadWrite)
                    .is_ok());
    ASSERT_TRUE(mem->map_region(0x20100000, 0x58F00000, MemoryProtection::ReadWrite)
                    .is_ok());
    ASSERT_TRUE(mem->map_region(0x79000000, 0x07000000, MemoryProtection::ReadWrite)
                    .is_ok());

    Handle h = laa.create_file_mapping(FILE_MAPPING_INVALID_FILE, 0x04, 0x1000, "");
    ASSERT_NE(h, NULL_HANDLE);
    const GuestAddr addr = laa.map_view_of_file(h, 0x0006, 0, 0);
    ASSERT_NE(addr, 0u);
    EXPECT_GE(addr, 0x80000000ull);
    EXPECT_LE(addr, 0xFFFEFFFFull - 0x1000ull);
}

TEST_F(FileMappingTest, MapView_ReturnsAllocationGranularityAlignedBase) {
    Handle h = mgr->create_file_mapping(FILE_MAPPING_INVALID_FILE, 0x04, 0x1000, "");
    ASSERT_NE(h, NULL_HANDLE);

    GuestAddr addr = mgr->map_view_of_file(h, 0x0006, 0, 0);
    ASSERT_NE(addr, 0u);
    EXPECT_EQ(addr % 0x10000u, 0u);

    Handle h2 = mgr->create_file_mapping(FILE_MAPPING_INVALID_FILE, 0x04, 0x100000, "");
    ASSERT_NE(h2, NULL_HANDLE);
    GuestAddr addr2 = mgr->map_view_of_file(h2, 0x0006, 0, 0);
    ASSERT_NE(addr2, 0u);
    EXPECT_EQ(addr2 % 0x10000u, 0u);
    // Must not pack inside the prior 64 KiB window at a non-granular base;
    // Chromium UnmapViewOfFile aligns down by dwAllocationGranularity.
    EXPECT_GE(addr2, addr + 0x10000u);
}

TEST_F(FileMappingTest, MapView_MisalignedOffset_FailsMappedAlignment) {
    Handle h = mgr->create_file_mapping(FILE_MAPPING_INVALID_FILE, 0x04, 0x20000, "");
    ASSERT_NE(h, NULL_HANDLE);

    GuestAddr addr = mgr->map_view_of_file(h, 0x0006, 0x1000, 0x1000);
    EXPECT_EQ(addr, 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_MAPPED_ALIGNMENT);
}

TEST_F(FileMappingTest, MapView_ReturnsNonNull) {
    Handle h = mgr->create_file_mapping(FILE_MAPPING_INVALID_FILE, 0x04, 0x1000, "");
    ASSERT_NE(h, NULL_HANDLE);

    GuestAddr addr = mgr->map_view_of_file(h, 0x0006, 0, 0);
    EXPECT_NE(addr, 0u);
}

TEST_F(FileMappingTest, MapViewExUsesExactAvailableBase) {
    constexpr GuestAddr kRequestedBase = 0x51000000;
    Handle h = mgr->create_file_mapping(
        FILE_MAPPING_INVALID_FILE, 0x04, 0x20000, "");
    ASSERT_NE(h, NULL_HANDLE);

    const GuestAddr addr = mgr->map_view_of_file_ex(
        h, 0x0006, 0, 0x20000, kRequestedBase);
    ASSERT_EQ(addr, kRequestedBase);
    EXPECT_TRUE(mem->write_u32(addr + 0x1234, 0xC0DEC0DEu).is_ok());
    EXPECT_EQ(mem->read_u32(addr + 0x1234).value_or(0), 0xC0DEC0DEu);
}

TEST_F(FileMappingTest, MapViewExFailsInsteadOfRelocatingOccupiedBase) {
    constexpr GuestAddr kRequestedBase = 0x52000000;
    ASSERT_TRUE(mem->map_region(
        kRequestedBase, 0x10000, MemoryProtection::ReadWrite).is_ok());
    Handle h = mgr->create_file_mapping(
        FILE_MAPPING_INVALID_FILE, 0x04, 0x10000, "");
    ASSERT_NE(h, NULL_HANDLE);

    EXPECT_EQ(mgr->map_view_of_file_ex(
                  h, 0x0006, 0, 0x10000, kRequestedBase),
              0u);
}

TEST_F(FileMappingTest, MapViewExRejectsMisalignedBase) {
    Handle h = mgr->create_file_mapping(
        FILE_MAPPING_INVALID_FILE, 0x04, 0x10000, "");
    ASSERT_NE(h, NULL_HANDLE);

    EXPECT_EQ(mgr->map_view_of_file_ex(
                  h, 0x0006, 0, 0x10000, 0x53001000),
              0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_MAPPED_ALIGNMENT);
}

TEST_F(FileMappingTest, SecCommitReadWriteSectionMapsWritableView) {
    constexpr u32 PAGE_READWRITE = 0x04;
    constexpr u32 SEC_COMMIT = 0x08000000;
    constexpr u32 FILE_MAP_WRITE = 0x00000002;
    constexpr u32 FILE_MAP_READ = 0x00000004;

    Handle h = mgr->create_file_mapping(
        FILE_MAPPING_INVALID_FILE,
        PAGE_READWRITE | SEC_COMMIT,
        0x2000,
        "");
    ASSERT_NE(h, NULL_HANDLE);

    const GuestAddr addr = mgr->map_view_of_file(
        h, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0);
    ASSERT_NE(addr, 0u);
    EXPECT_TRUE(mem->write_u32(addr, 0xC0DEC0DEu).is_ok());
    EXPECT_EQ(mem->read_u32(addr).value(), 0xC0DEC0DEu);
}

TEST_F(FileMappingTest, WriteReadThroughView) {
    Handle h = mgr->create_file_mapping(FILE_MAPPING_INVALID_FILE, 0x04, 0x1000, "");
    ASSERT_NE(h, NULL_HANDLE);

    GuestAddr addr = mgr->map_view_of_file(h, 0x0006, 0, 0);
    ASSERT_NE(addr, 0u);

    // Write a u32 into the mapped view
    auto wr = mem->write_u32(addr, 0xDEADBEEF);
    EXPECT_FALSE(wr.is_err());

    // Read it back
    auto rd = mem->read_u32(addr);
    EXPECT_FALSE(rd.is_err());
    EXPECT_EQ(rd.value(), 0xDEADBEEFu);
}

TEST_F(FileMappingTest, AnonymousZeroInitialized) {
    Handle h = mgr->create_file_mapping(FILE_MAPPING_INVALID_FILE, 0x04, 0x100, "");
    ASSERT_NE(h, NULL_HANDLE);

    GuestAddr addr = mgr->map_view_of_file(h, 0x0006, 0, 0);
    ASSERT_NE(addr, 0u);

    // All bytes should be zero for an anonymous mapping
    for (usize i = 0; i < 16; ++i) {
        auto rd = mem->read_u8(addr + static_cast<GuestAddr>(i));
        ASSERT_FALSE(rd.is_err());
        EXPECT_EQ(rd.value(), 0u) << "byte at offset " << i << " not zero";
    }
}

TEST(FileMappingAddressTest, PartialCollisionRetriesAtFreeGap) {
    GuestMemory memory;
    HandleTable handles;
    FileMappingManager manager(handles, memory);

    Handle first = manager.create_file_mapping(
        FILE_MAPPING_INVALID_FILE, 0x04, 0x1800, "");
    ASSERT_NE(first, NULL_HANDLE);
    const GuestAddr first_view =
        manager.map_view_of_file(first, 0x0006, 0, 0);
    ASSERT_EQ(first_view, 0x79000000u);

    const usize host_page_size = static_cast<usize>(::getpagesize());
    const GuestAddr collision =
        first_view + host_page_size + GUEST_PAGE_SIZE;
    ASSERT_TRUE(memory.map_region(collision, GUEST_PAGE_SIZE,
                                  MemoryProtection::ReadWrite).is_ok());
    Handle second = manager.create_file_mapping(
        FILE_MAPPING_INVALID_FILE, 0x04, 0x2000, "");
    ASSERT_NE(second, NULL_HANDLE);

    GuestAddr second_view = manager.map_view_of_file(second, 0x0006, 0, 0);
    EXPECT_NE(second_view, 0u);
    EXPECT_EQ(second_view % 0x10000u, 0u);
    EXPECT_GE(
        second_view,
        align_up(
            collision + GUEST_PAGE_SIZE,
            static_cast<GuestAddr>(0x10000)));
}

TEST(FileMappingAddressTest,
     OccupiedPreferredRangeSurvivesCrossProcessPromotion) {
    if (!ipc::ipc_enabled()) {
        GTEST_SKIP() << "disabled by VITREO_IPC=0";
    }

    GuestMemory memory;
    HandleTable handles;
    FileMappingManager manager(handles, memory);
    constexpr GuestAddr occupied_base = 0x20000000;
    const usize host_page_size = static_cast<usize>(::getpagesize());
    const usize occupied_size = host_page_size + GUEST_PAGE_SIZE;
    constexpr u32 sentinel = 0xA5C31E7Bu;

    ASSERT_TRUE(memory.map_region(
        occupied_base, occupied_size,
        MemoryProtection::ReadWrite).is_ok());
    ASSERT_TRUE(memory.write_u32(occupied_base, sentinel).is_ok());

    const Handle mapping = manager.create_file_mapping(
        FILE_MAPPING_INVALID_FILE, 0x08000004, 0x2000, "");
    ASSERT_NE(mapping, NULL_HANDLE);
    const GuestAddr view =
        manager.map_view_of_file(mapping, 0x0006, 0, 0);
    ASSERT_NE(view, 0u);
    EXPECT_GE(view, occupied_base + occupied_size);
    EXPECT_EQ(view % 0x10000u, 0u);
    ASSERT_TRUE(memory.write_u32(view, 0x12345678u).is_ok());

    const auto descriptor = manager.prepare_transferred_handle(
        mapping, 0x0007, /*inheritable=*/false);
    EXPECT_TRUE(descriptor.has_value());
    EXPECT_EQ(memory.read_u32(occupied_base).value_or(0), sentinel);
    EXPECT_EQ(memory.read_u32(view).value_or(0), 0x12345678u);
}

// =============================================================================
// UnmapViewOfFile Tests
// =============================================================================

TEST_F(FileMappingTest, UnmapSucceeds) {
    Handle h = mgr->create_file_mapping(FILE_MAPPING_INVALID_FILE, 0x04, 0x1000, "");
    ASSERT_NE(h, NULL_HANDLE);

    GuestAddr addr = mgr->map_view_of_file(h, 0x0006, 0, 0);
    ASSERT_NE(addr, 0u);

    EXPECT_TRUE(mgr->unmap_view_of_file(addr));
}

TEST_F(FileMappingTest, UnmapInvalidAddress_ReturnsFalse) {
    EXPECT_FALSE(mgr->unmap_view_of_file(0xBAADCAFE));
}

TEST_F(FileMappingTest, FileMapCopyUnmapDoesNotPersistPrivateWrites) {
    // Learn MapViewOfFile FILE_MAP_COPY: private pages are never written
    // back to the section / file and are discarded on UnmapViewOfFile.
    constexpr u32 FILE_MAP_COPY = 0x0001;
    constexpr u32 FILE_MAP_WRITE = 0x0002;
    constexpr u32 FILE_MAP_READ = 0x0004;

    Handle h = mgr->create_file_mapping(
        FILE_MAPPING_INVALID_FILE, PAGE_READWRITE, 0x1000, "");
    ASSERT_NE(h, NULL_HANDLE);

    GuestAddr writable = mgr->map_view_of_file(
        h, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0);
    ASSERT_NE(writable, 0u);
    ASSERT_TRUE(mem->write_u32(writable, 0xA1B2C3D4u).is_ok());
    ASSERT_TRUE(mgr->unmap_view_of_file(writable));

    GuestAddr cow = mgr->map_view_of_file(h, FILE_MAP_COPY, 0, 0);
    ASSERT_NE(cow, 0u);
    EXPECT_EQ(mem->read_u32(cow).value_or(0), 0xA1B2C3D4u);
    ASSERT_TRUE(mem->write_u32(cow, 0xDEADBEEFu).is_ok());
    ASSERT_TRUE(mgr->unmap_view_of_file(cow));

    GuestAddr again = mgr->map_view_of_file(h, FILE_MAP_READ, 0, 0);
    ASSERT_NE(again, 0u);
    EXPECT_EQ(mem->read_u32(again).value_or(0), 0xA1B2C3D4u)
        << "FILE_MAP_COPY modifications must not poison section backing";
    EXPECT_TRUE(mgr->unmap_view_of_file(again));
}

TEST_F(FileMappingTest, PageWriteCopyRejectsFileMapWrite) {
    constexpr u32 FILE_MAP_WRITE = 0x0002;
    constexpr u32 FILE_MAP_COPY = 0x0001;

    Handle h = mgr->create_file_mapping(
        FILE_MAPPING_INVALID_FILE, PAGE_WRITECOPY, 0x1000, "");
    ASSERT_NE(h, NULL_HANDLE);

    EXPECT_EQ(mgr->map_view_of_file(h, FILE_MAP_WRITE, 0, 0), 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_PARAMETER);

    GuestAddr cow = mgr->map_view_of_file(h, FILE_MAP_COPY, 0, 0);
    ASSERT_NE(cow, 0u);
    EXPECT_TRUE(mgr->unmap_view_of_file(cow));
}

TEST_F(FileMappingTest, DiscardVirtualMemoryReadWriteViewPreservesMapping) {
    constexpr u32 FILE_MAP_WRITE = 0x0002;
    constexpr u32 FILE_MAP_READ = 0x0004;
    const Handle section = mgr->create_file_mapping(
        FILE_MAPPING_INVALID_FILE, PAGE_READWRITE, 0x2000, "");
    ASSERT_NE(section, NULL_HANDLE);
    const GuestAddr view = mgr->map_view_of_file(
        section, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0);
    ASSERT_NE(view, 0u);
    ASSERT_TRUE(mem->write_u32(view, 0xA5A5A5A5u).is_ok());

    EXPECT_EQ(mgr->discard_virtual_memory(view, 0x1000),
              ViewDiscardResult::Success);
    GuestAddr queried_base = 0;
    u64 queried_size = 0;
    EXPECT_TRUE(mgr->query_view(view, queried_base, queried_size));
    EXPECT_EQ(queried_base, view);
    EXPECT_EQ(queried_size, 0x2000u);
    EXPECT_TRUE(mem->write_u32(view, 0x5A5A5A5Au).is_ok());
    EXPECT_EQ(mem->read_u32(view).value_or(0), 0x5A5A5A5Au);
}

TEST_F(FileMappingTest, DiscardVirtualMemoryReadOnlyViewFailsUnchanged) {
    constexpr u32 FILE_MAP_READ = 0x0004;
    const Handle section = mgr->create_file_mapping(
        FILE_MAPPING_INVALID_FILE, PAGE_READONLY, 0x1000, "");
    ASSERT_NE(section, NULL_HANDLE);
    const GuestAddr view = mgr->map_view_of_file(
        section, FILE_MAP_READ, 0, 0);
    ASSERT_NE(view, 0u);
    EXPECT_TRUE(mem->write_u32(view, 0xBAD0BAD0u).is_err());
    const u32 before = mem->read_u32(view).value_or(0xFFFFFFFFu);

    EXPECT_EQ(mgr->discard_virtual_memory(view, 0x1000),
              ViewDiscardResult::InvalidRange);
    EXPECT_EQ(mem->read_u32(view).value_or(0xFFFFFFFFu), before);
    GuestAddr queried_base = 0;
    u64 queried_size = 0;
    EXPECT_TRUE(mgr->query_view(view, queried_base, queried_size));
}

TEST_F(FileMappingTest, DiscardVirtualMemoryCopyViewFailsUnchanged) {
    constexpr u32 FILE_MAP_COPY = 0x0001;
    const Handle section = mgr->create_file_mapping(
        FILE_MAPPING_INVALID_FILE, PAGE_WRITECOPY, 0x1000, "");
    ASSERT_NE(section, NULL_HANDLE);
    const GuestAddr view = mgr->map_view_of_file(
        section, FILE_MAP_COPY, 0, 0);
    ASSERT_NE(view, 0u);
    ASSERT_TRUE(mem->write_u32(view, 0xC0DEC0DEu).is_ok());

    EXPECT_EQ(mgr->discard_virtual_memory(view, 0x1000),
              ViewDiscardResult::InvalidRange);
    EXPECT_EQ(mem->read_u32(view).value_or(0), 0xC0DEC0DEu);
    GuestAddr queried_base = 0;
    u64 queried_size = 0;
    EXPECT_TRUE(mgr->query_view(view, queried_base, queried_size));
    VirtualMemoryManager vmm(*mem);
    VirtualQueryManager vqm(*mem, &vmm, mgr.get());
    MemoryBasicInformation info{};
    ASSERT_EQ(vqm.virtual_query(view, info), sizeof(info));
    EXPECT_EQ(info.allocation_protect, PAGE_WRITECOPY);
    // The page was privately written (writecopy_protect_probe
    // "map rocopy written q prot=0x4").
    EXPECT_EQ(info.protect, PAGE_READWRITE);
}

TEST_F(FileMappingTest, DiscardVirtualMemoryThunkUsesMappedViewOwner) {
    constexpr u32 FILE_MAP_WRITE = 0x0002;
    constexpr u32 FILE_MAP_READ = 0x0004;
    const Handle section = mgr->create_file_mapping(
        FILE_MAPPING_INVALID_FILE, PAGE_READWRITE, 0x1000, "");
    ASSERT_NE(section, NULL_HANDLE);
    const GuestAddr view = mgr->map_view_of_file(
        section, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0);
    ASSERT_NE(view, 0u);

    VirtualMemoryManager vmm(*mem);
    capsule::Vfs vfs;
    Kernel32ExtManager kernel32(*mem, vfs);
    kernel32.set_virtual_memory_manager(&vmm);
    kernel32.set_file_mapping_manager(mgr.get());
    cpu::CpuState cpu;
    cpu.init_long_mode();
    Marshaler marshaler{*mem};
    marshaler.attach_cpu(cpu);
    set_last_error(0x1234u);
    cpu.gpr.rcx = view;
    cpu.gpr.rdx = 0x1000;

    kernel32.impl_DiscardVirtualMemory(marshaler);
    EXPECT_EQ(cpu.gpr.rax, error_codes::ERROR_SUCCESS);
    EXPECT_EQ(get_last_error(), 0x1234u);
}

TEST_F(FileMappingTest, DiscardRejectsHostOnlyViewPadding) {
    constexpr u32 FILE_MAP_WRITE = 0x0002;
    constexpr u32 FILE_MAP_READ = 0x0004;
    const Handle section = mgr->create_file_mapping(
        FILE_MAPPING_INVALID_FILE, PAGE_READWRITE, 0x4000, "");
    ASSERT_NE(section, NULL_HANDLE);
    const GuestAddr view = mgr->map_view_of_file(
        section, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0x1001);
    ASSERT_NE(view, 0u);

    EXPECT_NE(mgr->discard_virtual_memory(view + 0x2000, 0x1000),
              ViewDiscardResult::Success);
}

TEST_F(FileMappingTest,
       DiscardTracksVirtualProtectSubrangesExactly) {
    constexpr u32 FILE_MAP_WRITE = 0x0002;
    constexpr u32 FILE_MAP_READ = 0x0004;
    const Handle section = mgr->create_file_mapping(
        FILE_MAPPING_INVALID_FILE, PAGE_READWRITE, 0x2000, "");
    ASSERT_NE(section, NULL_HANDLE);
    const GuestAddr view = mgr->map_view_of_file(
        section, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0);
    ASSERT_NE(view, 0u);

    VirtualMemoryManager vmm(*mem);
    VirtualQueryManager vqm(*mem, &vmm, mgr.get());
    u32 old_protect = 0;
    ASSERT_TRUE(vqm.virtual_protect(
        view, 0x1000, PAGE_READONLY, &old_protect));
    EXPECT_EQ(old_protect, PAGE_READWRITE);
    EXPECT_EQ(mgr->discard_virtual_memory(view, 0x1000),
              ViewDiscardResult::InvalidRange);
    EXPECT_EQ(mgr->discard_virtual_memory(view + 0x1000, 0x1000),
              ViewDiscardResult::Success);

    ASSERT_TRUE(vqm.virtual_protect(
        view, 0x1000, PAGE_READWRITE, &old_protect));
    EXPECT_EQ(old_protect, PAGE_READONLY);
    EXPECT_EQ(mgr->discard_virtual_memory(view, 0x1000),
              ViewDiscardResult::Success);

    // Query-only helpers used by VirtualQueryEx/IPC must not detach the
    // process-wide mapped-view owner from the shared VMM.
    { VirtualQueryManager query_only(*mem, &vmm); }

    // VirtualProtectEx and NtProtectVirtualMemory both call the shared VMM
    // owner directly; the VQM constructor wires mapped-view routing into it.
    ASSERT_TRUE(vmm.virtual_protect(
        view, 0x1000, PAGE_READONLY, &old_protect));
    EXPECT_EQ(old_protect, PAGE_READWRITE);
    EXPECT_EQ(mgr->discard_virtual_memory(view, 0x1000),
              ViewDiscardResult::InvalidRange);

    MemoryBasicInformation info{};
    ASSERT_EQ(vqm.virtual_query(view, info), sizeof(info));
    EXPECT_EQ(info.type, MEM_MAPPED);
    EXPECT_EQ(info.allocation_protect, PAGE_READWRITE);
    EXPECT_EQ(info.protect, PAGE_READONLY);
    EXPECT_EQ(info.region_size, 0x1000u);
}

TEST_F(FileMappingTest, VirtualProtectCannotEscalateReadOnlyView) {
    constexpr u32 FILE_MAP_READ = 0x0004;
    const Handle section = mgr->create_file_mapping(
        FILE_MAPPING_INVALID_FILE, PAGE_READWRITE, 0x1000, "");
    ASSERT_NE(section, NULL_HANDLE);
    const GuestAddr view = mgr->map_view_of_file(
        section, FILE_MAP_READ, 0, 0);
    ASSERT_NE(view, 0u);

    VirtualMemoryManager vmm(*mem);
    VirtualQueryManager vqm(*mem, &vmm, mgr.get());
    u32 old_protect = 0;
    EXPECT_FALSE(vqm.virtual_protect(
        view, 0x1000, PAGE_READWRITE, &old_protect));
    EXPECT_TRUE(mem->write_u32(view, 0xBAD0BAD0u).is_err());
}

// VirtualProtect PAGE_*WRITECOPY on mapped views. Every expected value is a
// "map ..." line of tools/oracle_probe/writecopy_protect_probe{,32}.expected
// (identical on x86 and x64): copy views accept WRITECOPY and reject the
// READWRITE forms; a protection the view's access cannot take fails
// ERROR_INVALID_PARAMETER with old protect PAGE_NOACCESS; a privately
// written WRITECOPY page reports PAGE_READWRITE.
namespace {
struct ViewProtect {
    bool ok;
    u32 old;
    u32 error;
    bool operator==(const ViewProtect&) const = default;
};
}  // namespace

class FileMappingWritecopyTest : public FileMappingTest {
protected:
    static constexpr u32 kMapCopy = 0x0001;
    static constexpr u32 kMapWrite = 0x0002;
    static constexpr u32 kMapRead = 0x0004;
    static constexpr u32 kMapExecute = 0x0020;
    static constexpr u64 kPg = 0x1000;

    void SetUp() override {
        FileMappingTest::SetUp();
        vmm = std::make_unique<VirtualMemoryManager>(*mem);
        vqm = std::make_unique<VirtualQueryManager>(*mem, vmm.get(), mgr.get());
    }

    ViewProtect protect(GuestAddr addr, u64 size, u32 prot) {
        u32 old = 0xBEEF;
        const bool ok = vqm->virtual_protect(addr, size, prot, &old);
        return {ok, old, ok ? 0u : vqm->take_last_protect_error()};
    }

    std::pair<u32, u64> query(GuestAddr addr, u32 allocation_protect) {
        MemoryBasicInformation info{};
        EXPECT_EQ(vqm->virtual_query(addr, info), sizeof(info));
        EXPECT_EQ(info.type, MEM_MAPPED);
        EXPECT_EQ(info.allocation_protect, allocation_protect);
        return {info.protect, info.region_size};
    }

    std::unique_ptr<VirtualMemoryManager> vmm;
    std::unique_ptr<VirtualQueryManager> vqm;
};

namespace {
constexpr u32 kInvalid = error_codes::ERROR_INVALID_PARAMETER;
using Q = std::pair<u32, u64>;
}  // namespace

TEST_F(FileMappingWritecopyTest, CopyViewOfReadWriteSection) {
    const Handle section = mgr->create_file_mapping(
        FILE_MAPPING_INVALID_FILE, PAGE_READWRITE, 4 * kPg, "");
    ASSERT_NE(section, NULL_HANDLE);
    const GuestAddr w = mgr->map_view_of_file(section, kMapWrite, 0, 4 * kPg);
    ASSERT_NE(w, 0u);
    for (u64 i = 0; i < 4; ++i) {
        ASSERT_TRUE(mem->write_u8(w + i * kPg, static_cast<u8>(i + 1)).is_ok());
    }
    const GuestAddr c = mgr->map_view_of_file(section, kMapCopy, 0, 4 * kPg);
    ASSERT_NE(c, 0u);

    EXPECT_EQ(query(c, PAGE_WRITECOPY), Q(PAGE_WRITECOPY, 0x4000));
    EXPECT_EQ(protect(c, kPg, PAGE_WRITECOPY),
              (ViewProtect{true, PAGE_WRITECOPY, 0}));
    EXPECT_EQ(protect(c, kPg, PAGE_EXECUTE_WRITECOPY),
              (ViewProtect{false, PAGE_NOACCESS, kInvalid}));
    EXPECT_EQ(protect(c, kPg, PAGE_EXECUTE_READWRITE),
              (ViewProtect{false, PAGE_NOACCESS, kInvalid}));
    EXPECT_EQ(protect(c + kPg, 0x38, PAGE_READWRITE),
              (ViewProtect{false, PAGE_NOACCESS, kInvalid}));
    EXPECT_EQ(query(c + kPg, PAGE_WRITECOPY), Q(PAGE_WRITECOPY, 0x3000));

    ASSERT_TRUE(mem->write_u8(c + kPg, 0x42).is_ok());
    EXPECT_EQ(query(c + kPg, PAGE_WRITECOPY), Q(PAGE_READWRITE, 0x1000));
    EXPECT_EQ(protect(c + kPg, 0x38, PAGE_WRITECOPY),
              (ViewProtect{true, PAGE_READWRITE, 0}));
    EXPECT_EQ(query(c + kPg, PAGE_WRITECOPY), Q(PAGE_READWRITE, 0x1000));
    EXPECT_EQ(mem->read_u8(c + kPg).value_or(0), 0x42);
    EXPECT_EQ(mem->read_u8(w + kPg).value_or(0), 2)
        << "a WRITECOPY write must stay private to the copy view";

    EXPECT_EQ(protect(c, 3 * kPg, PAGE_READONLY),
              (ViewProtect{true, PAGE_WRITECOPY, 0}));
    EXPECT_EQ(protect(c, 3 * kPg, PAGE_WRITECOPY),
              (ViewProtect{true, PAGE_READONLY, 0}));
    EXPECT_EQ(query(c, PAGE_WRITECOPY), Q(PAGE_WRITECOPY, 0x1000));
    EXPECT_EQ(query(c + kPg, PAGE_WRITECOPY), Q(PAGE_READWRITE, 0x1000));
    EXPECT_EQ(query(c + 2 * kPg, PAGE_WRITECOPY), Q(PAGE_WRITECOPY, 0x2000));
    EXPECT_EQ(protect(c, 3 * kPg, PAGE_READWRITE),
              (ViewProtect{false, PAGE_NOACCESS, kInvalid}));

    EXPECT_EQ(protect(c + 3 * kPg, kPg, PAGE_NOACCESS),
              (ViewProtect{true, PAGE_WRITECOPY, 0}));
    EXPECT_EQ(protect(c + 3 * kPg, kPg, PAGE_WRITECOPY),
              (ViewProtect{true, PAGE_NOACCESS, 0}));
    EXPECT_EQ(query(c + 3 * kPg, PAGE_WRITECOPY), Q(PAGE_WRITECOPY, 0x1000));
}

TEST_F(FileMappingWritecopyTest, CopyViewOfReadOnlySection) {
    const Handle section = mgr->create_file_mapping(
        FILE_MAPPING_INVALID_FILE, PAGE_READONLY, 2 * kPg, "");
    ASSERT_NE(section, NULL_HANDLE);
    const GuestAddr r = mgr->map_view_of_file(section, kMapRead, 0, 2 * kPg);
    const GuestAddr rc = mgr->map_view_of_file(section, kMapCopy, 0, 2 * kPg);
    ASSERT_NE(r, 0u);
    ASSERT_NE(rc, 0u);

    EXPECT_EQ(protect(r, kPg, PAGE_READWRITE),
              (ViewProtect{false, PAGE_NOACCESS, kInvalid}));
    EXPECT_EQ(protect(r, kPg, PAGE_EXECUTE_WRITECOPY),
              (ViewProtect{false, PAGE_NOACCESS, kInvalid}));

    EXPECT_EQ(query(rc, PAGE_WRITECOPY), Q(PAGE_WRITECOPY, 0x2000));
    EXPECT_EQ(protect(rc, kPg, PAGE_WRITECOPY),
              (ViewProtect{true, PAGE_WRITECOPY, 0}));
    EXPECT_EQ(protect(rc, kPg, PAGE_READWRITE),
              (ViewProtect{false, PAGE_NOACCESS, kInvalid}));
    ASSERT_TRUE(mem->write_u8(rc, 0x31).is_ok());
    EXPECT_EQ(query(rc, PAGE_WRITECOPY), Q(PAGE_READWRITE, 0x1000));
    EXPECT_EQ(mem->read_u8(r).value_or(0xFF), 0);
}

TEST_F(FileMappingWritecopyTest, ExecutableCopyView) {
    const Handle section = mgr->create_file_mapping(
        FILE_MAPPING_INVALID_FILE, PAGE_EXECUTE_READWRITE, 2 * kPg, "");
    ASSERT_NE(section, NULL_HANDLE);
    const GuestAddr xc =
        mgr->map_view_of_file(section, kMapCopy | kMapExecute, 0, 2 * kPg);
    ASSERT_NE(xc, 0u);

    EXPECT_EQ(query(xc, PAGE_EXECUTE_WRITECOPY),
              Q(PAGE_EXECUTE_WRITECOPY, 0x2000));
    EXPECT_EQ(protect(xc, kPg, PAGE_WRITECOPY),
              (ViewProtect{true, PAGE_EXECUTE_WRITECOPY, 0}));
    EXPECT_EQ(query(xc, PAGE_EXECUTE_WRITECOPY), Q(PAGE_WRITECOPY, 0x1000));
    EXPECT_EQ(protect(xc, kPg, PAGE_EXECUTE_WRITECOPY),
              (ViewProtect{true, PAGE_WRITECOPY, 0}));
    EXPECT_EQ(protect(xc, kPg, PAGE_EXECUTE_READWRITE),
              (ViewProtect{false, PAGE_NOACCESS, kInvalid}));
    EXPECT_EQ(query(xc, PAGE_EXECUTE_WRITECOPY),
              Q(PAGE_EXECUTE_WRITECOPY, 0x2000));
    ASSERT_TRUE(mem->write_u8(xc, 0x21).is_ok());
    EXPECT_EQ(query(xc, PAGE_EXECUTE_WRITECOPY),
              Q(PAGE_EXECUTE_READWRITE, 0x1000));
    EXPECT_EQ(protect(xc, kPg, PAGE_EXECUTE_WRITECOPY),
              (ViewProtect{true, PAGE_EXECUTE_READWRITE, 0}));
    EXPECT_EQ(query(xc, PAGE_EXECUTE_WRITECOPY),
              Q(PAGE_EXECUTE_READWRITE, 0x1000));
}

// Windows accepts WRITECOPY on FILE_MAP_WRITE / FILE_MAP_READ views and makes
// each later write a private page (probe "map write wc isolation other=0x0").
// Vitreo's shared views alias section pages, so it fails loudly instead of
// reporting success while writes still reach the section. The execute check
// still matches Windows ("map write ewc ok=0x0 err=0x57 old=0x1").
TEST_F(FileMappingWritecopyTest, SharedViewWritecopyFailsLoudly) {
    const Handle section = mgr->create_file_mapping(
        FILE_MAPPING_INVALID_FILE, PAGE_READWRITE, 2 * kPg, "");
    ASSERT_NE(section, NULL_HANDLE);
    const GuestAddr w = mgr->map_view_of_file(section, kMapWrite, 0, 2 * kPg);
    ASSERT_NE(w, 0u);

    EXPECT_EQ(protect(w, kPg, PAGE_EXECUTE_WRITECOPY),
              (ViewProtect{false, PAGE_NOACCESS, kInvalid}));
    EXPECT_EQ(protect(w, kPg, PAGE_WRITECOPY),
              (ViewProtect{false, 0xBEEF, kInvalid}));
    EXPECT_EQ(query(w, PAGE_READWRITE), Q(PAGE_READWRITE, 0x2000));
}

TEST_F(FileMappingTest, NonExecutableSectionRejectsExecutableView) {
    constexpr u32 FILE_MAP_READ = 0x0004;
    constexpr u32 FILE_MAP_EXECUTE = 0x0020;
    const Handle section = mgr->create_file_mapping(
        FILE_MAPPING_INVALID_FILE, PAGE_READWRITE, 0x1000, "");
    ASSERT_NE(section, NULL_HANDLE);

    EXPECT_EQ(mgr->map_view_of_file(
                  section, FILE_MAP_READ | FILE_MAP_EXECUTE, 0, 0),
              0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_PARAMETER);
}

// mapview_access_zero_probe (tests/oracle_data/mapview_access_zero_probe
// {,.x86}.expected): dwDesiredAccess 0 maps every section kind; a data view
// without FILE_MAP_READ/WRITE/COPY (0, or FILE_MAP_EXECUTE alone) is
// PAGE_NOACCESS ("pagefile_rw access=0x0 view=ok err=0 prot=0x1",
// "data_rwx access=0x20 view=ok err=0 prot=0x1"). The CLR's fusion maps a
// SEC_IMAGE assembly with access 0 and failed with ERROR_INVALID_PARAMETER.
TEST_F(FileMappingTest, ZeroAccessViewMapsNoAccess) {
    constexpr u32 FILE_MAP_EXECUTE = 0x0020;
    const Handle rw = mgr->create_file_mapping(FILE_MAPPING_INVALID_FILE, PAGE_READWRITE, 0x10000, "");
    ASSERT_NE(rw, NULL_HANDLE);
    set_last_error(0xDEAD);
    const GuestAddr view = mgr->map_view_of_file(rw, 0, 0, 0);
    ASSERT_NE(view, 0u);
    EXPECT_EQ(get_last_error(), 0xDEADu);
    GuestAddr base = 0;
    u64 size = 0;
    u32 protection = 0;
    ASSERT_TRUE(mgr->query_view(view, base, size, &protection));
    EXPECT_EQ(protection, PAGE_NOACCESS);
    EXPECT_TRUE(mgr->unmap_view_of_file(view));

    const Handle rwx = mgr->create_file_mapping(FILE_MAPPING_INVALID_FILE, PAGE_EXECUTE_READWRITE, 0x10000, "");
    ASSERT_NE(rwx, NULL_HANDLE);
    const GuestAddr exec_only = mgr->map_view_of_file(rwx, FILE_MAP_EXECUTE, 0, 0);
    ASSERT_NE(exec_only, 0u);
    ASSERT_TRUE(mgr->query_view(exec_only, base, size, &protection));
    EXPECT_EQ(protection, PAGE_NOACCESS);
    EXPECT_TRUE(mgr->unmap_view_of_file(exec_only));
}

TEST_F(FileMappingTest, PlaceholderViewRoundTripsThroughPreservingUnmap) {
    VirtualMemoryManager vmm(*mem);
    constexpr u64 kViewSize = 0x10000;
    const u64 placeholder = vmm.virtual_alloc2(
        0, kViewSize,
        MEM_RESERVE | MEM_RESERVE_PLACEHOLDER,
        PAGE_NOACCESS);
    ASSERT_NE(placeholder, 0u);

    const Handle mapping = mgr->create_file_mapping(
        FILE_MAPPING_INVALID_FILE, PAGE_READWRITE,
        kViewSize, "");
    ASSERT_NE(mapping, NULL_HANDLE);

    const GuestAddr first = mgr->map_view_of_file3(
        mapping, static_cast<GuestAddr>(placeholder), 0, kViewSize,
        MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, vmm);
    ASSERT_EQ(first, placeholder);
    ASSERT_TRUE(mem->write_u32(first + 0x120, 0x13579BDFu).is_ok());

    ASSERT_TRUE(mgr->unmap_view_of_file2(
        first, MEM_PRESERVE_PLACEHOLDER, vmm));
    EXPECT_TRUE(mem->read_u32(first + 0x120).is_err());
    VirtualAllocationQuery restored{};
    ASSERT_TRUE(vmm.virtual_query(first, restored));
    EXPECT_EQ(restored.state, MEM_RESERVE);
    EXPECT_EQ(restored.allocation_base, first);

    const GuestAddr second = mgr->map_view_of_file3(
        mapping, first, 0, kViewSize,
        MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, vmm);
    ASSERT_EQ(second, first);
    EXPECT_EQ(mem->read_u32(second + 0x120).value_or(0), 0x13579BDFu);
    EXPECT_TRUE(mgr->unmap_view_of_file2(second, 0, vmm));
}

TEST_F(FileMappingTest, PreservingUnmapRejectsOrdinaryView) {
    VirtualMemoryManager vmm(*mem);
    const Handle mapping = mgr->create_file_mapping(
        FILE_MAPPING_INVALID_FILE, PAGE_READWRITE, 0x10000, "");
    ASSERT_NE(mapping, NULL_HANDLE);
    const GuestAddr view = mgr->map_view_of_file(mapping, 0x0006, 0, 0);
    ASSERT_NE(view, 0u);

    EXPECT_FALSE(mgr->unmap_view_of_file2(
        view, MEM_PRESERVE_PLACEHOLDER, vmm));
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_ADDRESS);
    EXPECT_TRUE(mgr->unmap_view_of_file(view));
}

TEST_F(FileMappingTest, PlaceholderThunkTableExportsApiFamily) {
    VirtualMemoryManager vmm(*mem);
    const DllExports exports =
        create_placeholder_mapping_thunks(*mgr, vmm);
    ASSERT_EQ(exports.entries.size(), 2u);
    EXPECT_EQ(exports.entries[0].name, "MapViewOfFile3");
    EXPECT_EQ(exports.entries[0].stdcall_bytes, 40u);
    EXPECT_EQ(exports.entries[1].name, "UnmapViewOfFile2");
    EXPECT_EQ(exports.entries[1].stdcall_bytes, 12u);
}

// =============================================================================
// OpenFileMappingW Tests
// =============================================================================

TEST_F(FileMappingTest, NamedMapping_OpenByName) {
    Handle h = mgr->create_file_mapping(FILE_MAPPING_INVALID_FILE, 0x04, 0x1000,
                                         "Local\\SharedMem");
    ASSERT_NE(h, NULL_HANDLE);

    Handle h2 = mgr->open_file_mapping(0x0006, "Local\\SharedMem");
    ASSERT_NE(h2, NULL_HANDLE);
    EXPECT_NE(h, h2);
    EXPECT_EQ(handles->get_access(h2), 0x0006u);

    handles->set_close_callback(
        [this](Handle handle, HandleType type, void*) {
            return mgr->on_handle_close(handle, type);
        });
    EXPECT_TRUE(handles->close(h));

    GuestAddr addr = mgr->map_view_of_file(h2, 0x0006, 0, 0);
    EXPECT_NE(addr, 0u);
    EXPECT_TRUE(handles->close(h2));
    EXPECT_TRUE(mgr->unmap_view_of_file(addr));
    handles->set_close_callback(nullptr);
}

TEST_F(FileMappingTest, OpenUnknownName_ReturnsNull) {
    Handle h = mgr->open_file_mapping(0x0006, "NoSuchMapping");
    EXPECT_EQ(h, NULL_HANDLE);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_FILE_NOT_FOUND);
}

TEST_F(FileMappingTest, DuplicateHandleHasIndependentLifetime) {
    Handle source = mgr->create_file_mapping(
        FILE_MAPPING_INVALID_FILE, 0x04, 0x1000, "");
    ASSERT_NE(source, NULL_HANDLE);

    Handle duplicate = mgr->duplicate_handle(
        source, HandleFlags::Inherit, handles->get_access(source));
    ASSERT_NE(duplicate, NULL_HANDLE);
    EXPECT_NE(duplicate, source);
    EXPECT_EQ(handles->get_type(duplicate), HandleType::FileMapping);
    EXPECT_EQ(handles->get_flags(duplicate), HandleFlags::Inherit);

    handles->set_close_callback(
        [this](Handle handle, HandleType type, void*) {
            return mgr->on_handle_close(handle, type);
        });
    EXPECT_TRUE(handles->close(source));
    EXPECT_NE(mgr->map_view_of_file(duplicate, 0x0006, 0, 0), 0u);
    EXPECT_TRUE(handles->close(duplicate));
    handles->set_close_callback(nullptr);
}

TEST(FileMappingInheritanceTest,
     AnonymousSectionKeepsHandleAndSharedContentsAcrossManagers) {
    if (!ipc::ipc_enabled()) {
        GTEST_SKIP() << "disabled by VITREO_IPC=0";
    }

    GuestMemory parent_memory;
    HandleTable parent_handles;
    FileMappingManager parent_mappings(parent_handles, parent_memory);

    const Handle source = parent_mappings.create_file_mapping(
        FILE_MAPPING_INVALID_FILE, 0x04, 0x4000, "");
    ASSERT_NE(source, NULL_HANDLE);
    const Handle inherited = parent_mappings.duplicate_handle(
        source, HandleFlags::Inherit, 0x0006);
    ASSERT_NE(inherited, NULL_HANDLE);

    const GuestAddr parent_view =
        parent_mappings.map_view_of_file(inherited, 0x0006, 0, 0);
    ASSERT_NE(parent_view, 0u);
    ASSERT_TRUE(parent_memory.write_u32(
        parent_view + 0x120, 0x13579BDFu).is_ok());

    const std::array<Handle, 1> selected{inherited};
    const auto descriptors =
        parent_mappings.prepare_inherited_handles(selected);
    ASSERT_TRUE(descriptors.has_value());
    ASSERT_FALSE(descriptors->empty());

    GuestMemory child_memory;
    const std::array<Handle, 1> inherited_values{inherited};
    HandleTable child_handles(inherited_values);
    for (int i = 0; i < 32; ++i) {
        ASSERT_NE(child_handles.allocate(HandleType::Event, nullptr), inherited);
    }
    FileMappingManager child_mappings(child_handles, child_memory);
    ASSERT_TRUE(child_mappings.import_inherited_handles(*descriptors));
    EXPECT_EQ(child_handles.get_type(inherited), HandleType::FileMapping);
    EXPECT_EQ(child_handles.get_flags(inherited), HandleFlags::Inherit);
    EXPECT_EQ(child_handles.get_access(inherited), 0x0006u);

    const GuestAddr child_view =
        child_mappings.map_view_of_file(inherited, 0x0006, 0, 0);
    ASSERT_NE(child_view, 0u);
    EXPECT_EQ(child_memory.read_u32(child_view + 0x120).value_or(0),
              0x13579BDFu);

    ASSERT_TRUE(child_memory.write_u32(
        child_view + 0x124, 0x2468ACE0u).is_ok());
    EXPECT_EQ(parent_memory.read_u32(parent_view + 0x124).value_or(0),
              0x2468ACE0u);
}

// Windows CreateFileMapping(INVALID_HANDLE_VALUE, unnamed) creates a section
// whose pages are shareable immediately. Writes through one process's view must
// be visible to a peer after DuplicateHandle/transfer without requiring the
// peer's write to race a private→shared promote on the creator.
TEST(FileMappingTransferTest,
     UnnamedAnonymousSectionLiveSharesAcrossPrepareTransfer) {
    if (!ipc::ipc_enabled()) {
        GTEST_SKIP() << "disabled by VITREO_IPC=0";
    }

    GuestMemory parent_memory;
    HandleTable parent_handles;
    FileMappingManager parent_mappings(parent_handles, parent_memory);

    const Handle source = parent_mappings.create_file_mapping(
        FILE_MAPPING_INVALID_FILE, 0x04, 0x3000, "");
    ASSERT_NE(source, NULL_HANDLE);
    const GuestAddr parent_view =
        parent_mappings.map_view_of_file(source, 0x0006, 0, 0);
    ASSERT_NE(parent_view, 0u);
    ASSERT_TRUE(parent_memory.write_u32(parent_view, 0x11111111u).is_ok());

    const auto descriptor = parent_mappings.prepare_transferred_handle(
        source, 0x0007, /*inheritable=*/false);
    ASSERT_TRUE(descriptor.has_value());

    // Post-transfer write on the creator must still land in shared pages.
    ASSERT_TRUE(parent_memory.write_u32(
        parent_view + 4, 0x22222222u).is_ok());

    GuestMemory child_memory;
    HandleTable child_handles;
    FileMappingManager child_mappings(child_handles, child_memory);
    const Handle imported =
        child_mappings.import_transferred_handle(*descriptor);
    ASSERT_NE(imported, NULL_HANDLE);
    const GuestAddr child_view =
        child_mappings.map_view_of_file(imported, 0x0006, 0, 0);
    ASSERT_NE(child_view, 0u);
    EXPECT_EQ(child_memory.read_u32(child_view).value_or(0), 0x11111111u);
    EXPECT_EQ(child_memory.read_u32(child_view + 4).value_or(0), 0x22222222u);

    ASSERT_TRUE(child_memory.write_u32(
        child_view + 8, 0x33333333u).is_ok());
    EXPECT_EQ(parent_memory.read_u32(parent_view + 8).value_or(0),
              0x33333333u);
}

namespace {

/// Host pages of [p, p+len) that hold data (resident or compressed/swapped).
usize host_pages_with_data(const void* p, usize len) {
    const usize page = static_cast<usize>(::getpagesize());
    const auto lo = reinterpret_cast<std::uintptr_t>(p) & ~(page - 1);
    const auto hi = (reinterpret_cast<std::uintptr_t>(p) + len + page - 1) &
                    ~(page - 1);
    std::vector<char> state((hi - lo) / page);
    if (::mincore(reinterpret_cast<void*>(lo), hi - lo, state.data()) != 0) {
        return state.size();
    }
    usize count = 0;
    for (char s : state) {
        if ((s & (MINCORE_INCORE | MINCORE_PAGED_OUT)) != 0) ++count;
    }
    return count;
}

}  // namespace

// Learn CreateFileMapping: pages of a pagefile-backed section are initially
// zero; SEC_COMMIT charges commit but does not make pages resident. Learn
// MapViewOfFile: FILE_MAP_COPY writes go to private pages, never the section.
// Learn DuplicateHandle / handle inheritance: every handle names the same
// section object. None of these operations may write section pages up front.
TEST(FileMappingFootprintTest,
     LargeSectionInheritTransferAndViewsDoNotMaterializePages) {
    if (!ipc::ipc_enabled()) {
        GTEST_SKIP() << "disabled by VITREO_IPC=0";
    }
    constexpr u64 kSize = 128ull << 20;
    constexpr u32 FILE_MAP_COPY = 0x0001;
    constexpr u32 FILE_MAP_WRITE = 0x0002;
    constexpr u32 FILE_MAP_READ = 0x0004;
    constexpr u32 SEC_COMMIT = 0x08000000u;
    constexpr usize kBudget = 64;  // host pages (1 MiB) of incidental touches

    GuestMemory memory;
    HandleTable handles;
    FileMappingManager mappings(handles, memory);

    const Handle section = mappings.create_file_mapping(
        FILE_MAPPING_INVALID_FILE, PAGE_READWRITE | SEC_COMMIT, kSize, "");
    ASSERT_NE(section, NULL_HANDLE);
    const Handle inherited = mappings.duplicate_handle(
        section, HandleFlags::Inherit, FILE_MAP_READ | FILE_MAP_WRITE);
    ASSERT_NE(inherited, NULL_HANDLE);
    const std::array<Handle, 1> selected{inherited};
    ASSERT_TRUE(mappings.prepare_inherited_handles(selected).has_value());
    const auto transfer = mappings.prepare_transferred_handle(
        section, FILE_MAP_READ | FILE_MAP_COPY, /*inheritable=*/false);
    ASSERT_TRUE(transfer.has_value());

    const GuestAddr ro = mappings.map_view_of_file(section, FILE_MAP_READ, 0, 0);
    ASSERT_NE(ro, 0u);
    const GuestAddr cow = mappings.map_view_of_file(section, FILE_MAP_COPY, 0, 0);
    ASSERT_NE(cow, 0u);
    const Byte* ro_host = memory.host_ptr(ro, kSize);
    const Byte* cow_host = memory.host_ptr(cow, kSize);
    ASSERT_NE(ro_host, nullptr);
    ASSERT_NE(cow_host, nullptr);
    EXPECT_LE(host_pages_with_data(ro_host, kSize), kBudget);
    EXPECT_LE(host_pages_with_data(cow_host, kSize), kBudget);

    for (u64 off : {u64{0}, kSize / 2, kSize - 4}) {
        EXPECT_EQ(memory.read_u32(ro + off).value_or(1), 0u);
        EXPECT_EQ(memory.read_u32(cow + off).value_or(1), 0u);
    }
    constexpr u64 kWrite = 64ull << 20;
    ASSERT_TRUE(memory.write_u32(cow + kWrite, 0xC0FFEE11u).is_ok());
    EXPECT_EQ(memory.read_u32(cow + kWrite).value_or(0), 0xC0FFEE11u);
    EXPECT_EQ(memory.read_u32(ro + kWrite).value_or(1), 0u)
        << "FILE_MAP_COPY write must stay private to its view";

    GuestMemory child_memory;
    HandleTable child_handles;
    FileMappingManager child_mappings(child_handles, child_memory);
    const Handle imported = child_mappings.import_transferred_handle(*transfer);
    ASSERT_NE(imported, NULL_HANDLE);
    const GuestAddr child_view =
        child_mappings.map_view_of_file(imported, FILE_MAP_READ, 0, 0);
    ASSERT_NE(child_view, 0u);
    EXPECT_EQ(child_memory.read_u32(child_view + kWrite).value_or(1), 0u);
    const Byte* child_host = child_memory.host_ptr(child_view, kSize);
    ASSERT_NE(child_host, nullptr);
    EXPECT_LE(host_pages_with_data(child_host, kSize), kBudget);
    EXPECT_LE(host_pages_with_data(ro_host, kSize), kBudget);
    EXPECT_LE(host_pages_with_data(cow_host, kSize), kBudget);
}

// A SEC_RESERVE section has no shm until it is transferred. Promotion must
// carry only pages the guest committed and wrote, not a zero-filled copy of
// the whole section.
TEST(FileMappingFootprintTest, SecReserveTransferCopiesOnlyWrittenPages) {
    if (!ipc::ipc_enabled()) {
        GTEST_SKIP() << "disabled by VITREO_IPC=0";
    }
    constexpr u64 kSize = 32ull << 20;
    constexpr u32 FILE_MAP_WRITE = 0x0002;
    constexpr u32 FILE_MAP_READ = 0x0004;
    constexpr u32 SEC_RESERVE = 0x04000000u;
    constexpr usize kBudget = 64;

    GuestMemory memory;
    HandleTable handles;
    FileMappingManager mappings(handles, memory);
    const Handle section = mappings.create_file_mapping(
        FILE_MAPPING_INVALID_FILE, PAGE_READWRITE | SEC_RESERVE, kSize, "");
    ASSERT_NE(section, NULL_HANDLE);
    const GuestAddr view = mappings.map_view_of_file(
        section, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0);
    ASSERT_NE(view, 0u);
    constexpr u64 kWrite = 16ull << 20;
    ASSERT_TRUE(mappings.commit_reserved_view_pages(
        view + kWrite, 0x1000, PAGE_READWRITE));
    ASSERT_TRUE(memory.write_u32(view + kWrite + 8, 0x5EC0FFEEu).is_ok());

    const auto transfer = mappings.prepare_transferred_handle(
        section, FILE_MAP_READ, /*inheritable=*/false);
    ASSERT_TRUE(transfer.has_value());

    GuestMemory child_memory;
    HandleTable child_handles;
    FileMappingManager child_mappings(child_handles, child_memory);
    const Handle imported = child_mappings.import_transferred_handle(*transfer);
    ASSERT_NE(imported, NULL_HANDLE);
    const GuestAddr child_view =
        child_mappings.map_view_of_file(imported, FILE_MAP_READ, 0, 0);
    ASSERT_NE(child_view, 0u);
    const Byte* child_host = child_memory.host_ptr(child_view, kSize);
    ASSERT_NE(child_host, nullptr);
    EXPECT_LE(host_pages_with_data(child_host, kSize), kBudget);
    EXPECT_EQ(child_memory.read_u32(child_view + kWrite + 8).value_or(0),
              0x5EC0FFEEu);
    EXPECT_EQ(child_memory.read_u32(child_view).value_or(1), 0u);
    EXPECT_EQ(child_memory.read_u32(child_view + kSize - 4).value_or(1), 0u);
}

TEST(FileMappingVirtualQueryTest, MapViewReportsMemMapped) {
    // Learn VirtualQuery: MapViewOfFile regions report Type=MEM_MAPPED.
    GuestMemory memory;
    HandleTable handles;
    FileMappingManager mappings(handles, memory);
    VirtualMemoryManager vmm(memory);
    VirtualQueryManager vqm(memory, &vmm, &mappings);

    const Handle section = mappings.create_file_mapping(
        FILE_MAPPING_INVALID_FILE, 0x04, 0x2000, "");
    ASSERT_NE(section, NULL_HANDLE);
    const GuestAddr view = mappings.map_view_of_file(section, 0x0006, 0, 0);
    ASSERT_NE(view, 0u);

    MemoryBasicInformation info{};
    ASSERT_EQ(vqm.virtual_query(view + 0x100, info),
              sizeof(MemoryBasicInformation));
    EXPECT_EQ(info.state, MEM_COMMIT);
    EXPECT_EQ(info.type, MEM_MAPPED);
    EXPECT_EQ(info.allocation_base, view);
    EXPECT_GE(info.region_size, 0x2000u - 0x100u);
}

TEST(FileMappingDuplicateApiTest,
     HonorsRequestedAccessForDistinctX64Handle) {
    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(
        0x1000, 0x10000, MemoryProtection::ReadWrite).is_ok());

    HandleTable handles;
    FileMappingManager mappings(handles, memory);
    capsule::Vfs vfs;
    Kernel32ExtManager kernel32(memory, vfs);
    kernel32.set_handle_table(&handles);
    kernel32.set_file_mapping_manager(&mappings);

    const Handle source = mappings.create_file_mapping(
        FILE_MAPPING_INVALID_FILE, 0x04, 0x1000, "");
    ASSERT_NE(source, NULL_HANDLE);

    cpu::CpuState cpu;
    cpu.init_long_mode();
    cpu.gpr.rsp = 0x8000;
    cpu.gpr.rcx = INVALID_HANDLE;  // current source process
    cpu.gpr.rdx = source;
    cpu.gpr.r8 = INVALID_HANDLE;   // current target process
    cpu.gpr.r9 = 0x1800;           // output HANDLE*
    // Chromium duplicates writable shared-memory handles with explicit
    // FILE_MAP_COPY | FILE_MAP_WRITE | FILE_MAP_READ access and no
    // DUPLICATE_SAME_ACCESS option.
    ASSERT_TRUE(memory.write_u64(cpu.gpr.rsp + 0x28, 0x0007).is_ok());
    ASSERT_TRUE(memory.write_u64(cpu.gpr.rsp + 0x30, 1).is_ok());
    ASSERT_TRUE(memory.write_u64(cpu.gpr.rsp + 0x38, 0).is_ok());

    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    kernel32.impl_DuplicateHandle(marshaler);

    ASSERT_EQ(cpu.gpr.rax, 1u);
    const Handle duplicate = static_cast<Handle>(
        memory.read_u64(0x1800).value_or(0));
    ASSERT_NE(duplicate, NULL_HANDLE);
    EXPECT_NE(duplicate, source);
    EXPECT_EQ(handles.get_flags(duplicate), HandleFlags::Inherit);
    EXPECT_EQ(handles.get_access(duplicate), 0x0007u);

    handles.set_close_callback(
        [&mappings](Handle handle, HandleType type, void*) {
            return mappings.on_handle_close(handle, type);
        });
    EXPECT_TRUE(handles.close(source));
    EXPECT_NE(mappings.map_view_of_file(duplicate, 0x0006, 0, 0), 0u);
    EXPECT_EQ(mappings.map_view_of_file(duplicate, 0x0024, 0, 0), 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_ACCESS_DENIED);
    EXPECT_TRUE(handles.close(duplicate));
    handles.set_close_callback(nullptr);
}

TEST(FileMappingDuplicateApiTest,
     CannotEscalateReadOnlySectionHandleToFileMapWrite) {
    constexpr u32 SECTION_QUERY = 0x00000001;
    constexpr u32 FILE_MAP_WRITE = 0x00000002;
    constexpr u32 FILE_MAP_READ = 0x00000004;

    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(
        0x1000, 0x10000, MemoryProtection::ReadWrite).is_ok());

    HandleTable handles;
    FileMappingManager mappings(handles, memory);
    capsule::Vfs vfs;
    Kernel32ExtManager kernel32(memory, vfs);
    kernel32.set_handle_table(&handles);
    kernel32.set_file_mapping_manager(&mappings);

    const Handle unrestricted = mappings.create_file_mapping(
        FILE_MAPPING_INVALID_FILE, 0x04, 0x1000, "");
    ASSERT_NE(unrestricted, NULL_HANDLE);
    const Handle read_only = mappings.duplicate_handle(
        unrestricted, 0, FILE_MAP_READ | SECTION_QUERY);
    ASSERT_NE(read_only, NULL_HANDLE);

    cpu::CpuState cpu;
    cpu.init_long_mode();
    cpu.gpr.rsp = 0x8000;
    cpu.gpr.rcx = INVALID_HANDLE;
    cpu.gpr.rdx = read_only;
    cpu.gpr.r8 = INVALID_HANDLE;
    cpu.gpr.r9 = 0x1800;
    ASSERT_TRUE(memory.write_u64(cpu.gpr.rsp + 0x28, FILE_MAP_WRITE).is_ok());
    ASSERT_TRUE(memory.write_u64(cpu.gpr.rsp + 0x30, 0).is_ok());
    ASSERT_TRUE(memory.write_u64(cpu.gpr.rsp + 0x38, 0).is_ok());

    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    kernel32.impl_DuplicateHandle(marshaler);

    EXPECT_EQ(cpu.gpr.rax, 0u);
    EXPECT_EQ(memory.read_u64(0x1800).value_or(0), 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_ACCESS_DENIED);
}

// =============================================================================
// FlushViewOfFile Tests
// =============================================================================

TEST_F(FileMappingTest, FlushAnonymous_ReturnsTrue) {
    Handle h = mgr->create_file_mapping(FILE_MAPPING_INVALID_FILE, 0x04, 0x1000, "");
    ASSERT_NE(h, NULL_HANDLE);

    GuestAddr addr = mgr->map_view_of_file(h, 0x0006, 0, 0);
    ASSERT_NE(addr, 0u);

    EXPECT_TRUE(mgr->flush_view_of_file(addr, 0));
}

// =============================================================================
// Error-path Tests
// =============================================================================

TEST_F(FileMappingTest, InvalidHandle_MapReturnsNull) {
    GuestAddr addr = mgr->map_view_of_file(0x9999, 0x0006, 0, 0);
    EXPECT_EQ(addr, 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_HANDLE);
}

// =============================================================================
// Multi-view Test
// =============================================================================

TEST_F(FileMappingTest, MultipleViewsShareData) {
    Handle h = mgr->create_file_mapping(FILE_MAPPING_INVALID_FILE, 0x04, 0x1000, "");
    ASSERT_NE(h, NULL_HANDLE);

    // Map two views of the same mapping
    GuestAddr v1 = mgr->map_view_of_file(h, 0x0006, 0, 0);
    ASSERT_NE(v1, 0u);

    // Write a value into the first view
    auto wr = mem->write_u32(v1, 0x12345678);
    EXPECT_FALSE(wr.is_err());

    // Unmap the first view — this syncs data back to the backing store
    EXPECT_TRUE(mgr->unmap_view_of_file(v1));

    // Map a second view — backing data should contain the written value
    GuestAddr v2 = mgr->map_view_of_file(h, 0x0006, 0, 0);
    ASSERT_NE(v2, 0u);

    auto rd = mem->read_u32(v2);
    EXPECT_FALSE(rd.is_err());
    EXPECT_EQ(rd.value(), 0x12345678u);
}

TEST(FileMappingFileBacked, MapViewExposesFileBytes) {
    // Learn CreateFileMappingW / MapViewOfFile: creating the mapping does not
    // read the file, and the view exposes section pages (not a second zeroed
    // private copy of the whole file).
    char path_template[] = "/tmp/vitreo_fmap_XXXXXX";
    char* root = ::mkdtemp(path_template);
    ASSERT_NE(root, nullptr);
    const std::string host_file = std::string(root) + "/payload.bin";
    std::vector<u8> payload(96u * 1024u);
    for (usize i = 0; i < payload.size(); ++i) {
        payload[i] = static_cast<u8>(i * 3u);
    }
    {
        std::ofstream out(host_file, std::ios::binary);
        out.write(reinterpret_cast<const char*>(payload.data()),
                  static_cast<std::streamsize>(payload.size()));
    }

    capsule::Vfs vfs;
    ASSERT_TRUE(vfs.mount_drive(
                    'C', std::make_unique<capsule::HostFileSystem>(root))
                    .is_ok());
    GuestMemory memory;
    if (!memory.enable_linear_reservation(0x80000000ull)) {
        GTEST_SKIP() << "linear reservation unavailable";
    }
    HandleTable handles;
    FileManager files(vfs, handles);
    FileMappingManager mappings(handles, memory);
    mappings.set_file_manager(&files);

    const Handle file = files.create_file(
        "C:\\payload.bin", GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING, 0);
    ASSERT_NE(file, INVALID_HANDLE);

    const Handle mapping =
        mappings.create_file_mapping(file, PAGE_READONLY, 0, "");
    ASSERT_NE(mapping, NULL_HANDLE);

    constexpr u32 kFileMapRead = 0x0004;
    const GuestAddr view =
        mappings.map_view_of_file(mapping, kFileMapRead, 0, 0);
    ASSERT_NE(view, 0u);

    for (usize i : {usize{0}, usize{1}, usize{255}, usize{4096}, usize{65535},
                    usize{65536}, payload.size() - 1}) {
        auto rd = memory.read_u8(view + static_cast<GuestAddr>(i));
        ASSERT_TRUE(rd.is_ok()) << "offset " << i;
        EXPECT_EQ(rd.value(), payload[i]) << "offset " << i;
    }

    EXPECT_EQ(mappings.mapped_file_path(view + 4096), "C:\\payload.bin");
    EXPECT_EQ(mappings.mem_reset_view_disposition(view, 0x1000),
              error_codes::ERROR_USER_MAPPED_FILE);
    EXPECT_TRUE(handles.close(file));
    EXPECT_EQ(mappings.mapped_file_path(view + 4096), "C:\\payload.bin");
    EXPECT_TRUE(mappings.unmap_view_of_file(view));
    std::filesystem::remove_all(root);
}

// Audit 01 CreateFileMappingW #1: PAGE_READWRITE on a read-only file handle
// must fail with ERROR_ACCESS_DENIED (not a silent private snapshot).
TEST(FileMappingFileBacked, ReadWriteProtectOnReadOnlyHandleDenied) {
    char path_template[] = "/tmp/vitreo_fmap_ro_XXXXXX";
    char* root = ::mkdtemp(path_template);
    ASSERT_NE(root, nullptr);
    const std::string host_file = std::string(root) + "/ro.bin";
    {
        std::ofstream out(host_file, std::ios::binary);
        out.write("abcd", 4);
    }
    capsule::Vfs vfs;
    ASSERT_TRUE(vfs.mount_drive(
                    'C', std::make_unique<capsule::HostFileSystem>(root))
                    .is_ok());
    GuestMemory memory;
    if (!memory.enable_linear_reservation(0x80000000ull)) {
        GTEST_SKIP() << "linear reservation unavailable";
    }
    HandleTable handles;
    FileManager files(vfs, handles);
    FileMappingManager mappings(handles, memory);
    mappings.set_file_manager(&files);

    const Handle file = files.create_file(
        "C:\\ro.bin", GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING, 0);
    ASSERT_NE(file, INVALID_HANDLE);
    set_last_error(0xDEAD);
    EXPECT_EQ(mappings.create_file_mapping(file, PAGE_READWRITE, 0, ""),
              NULL_HANDLE);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_ACCESS_DENIED);
    EXPECT_NE(mappings.create_file_mapping(file, PAGE_READONLY, 0, ""),
              NULL_HANDLE);
    std::filesystem::remove_all(root);
}

// Audit 01 CreateFileMappingW #4: SEC_RESERVE views are MEM_RESERVE until
// VirtualAlloc MEM_COMMIT (Windows sec_attrs_probe).
TEST_F(FileMappingTest, SecReserveViewIsReservedUntilVirtualAllocCommit) {
    constexpr u32 SEC_RESERVE = 0x04000000u;
    constexpr u32 FILE_MAP_WRITE = 0x0002;
    Handle h = mgr->create_file_mapping(
        FILE_MAPPING_INVALID_FILE, PAGE_READWRITE | SEC_RESERVE, 0x10000, "");
    ASSERT_NE(h, NULL_HANDLE);
    GuestAddr view = mgr->map_view_of_file(h, FILE_MAP_WRITE, 0, 0);
    ASSERT_NE(view, 0u);

    VirtualMemoryManager vmm(*mem);
    vmm.set_file_mapping_manager(mgr.get());
    vmm.set_mapped_view_probe(
        [this](GuestAddr a, u64 n) {
            return mgr->range_within_mapped_view(a, n);
        });
    VirtualQueryManager vqm(*mem, &vmm, mgr.get());

    MemoryBasicInformation info{};
    ASSERT_EQ(vqm.virtual_query(view, info), sizeof(info));
    EXPECT_EQ(info.state, MEM_RESERVE);
    EXPECT_EQ(info.protect, 0u);
    EXPECT_EQ(info.allocation_protect, PAGE_READWRITE);
    EXPECT_EQ(info.type, MEM_MAPPED);
    EXPECT_EQ(info.region_size, 0x10000u);

    ASSERT_EQ(vmm.virtual_alloc(view, 0x1000, MEM_COMMIT, PAGE_READWRITE),
              view);
    ASSERT_EQ(vqm.virtual_query(view, info), sizeof(info));
    EXPECT_EQ(info.state, MEM_COMMIT);
    EXPECT_EQ(info.protect, PAGE_READWRITE);
    EXPECT_EQ(info.region_size, 0x1000u);

    ASSERT_EQ(vqm.virtual_query(view + 0x1000, info), sizeof(info));
    EXPECT_EQ(info.state, MEM_RESERVE);
    EXPECT_EQ(info.protect, 0u);
    EXPECT_EQ(info.allocation_base, view);
}

// Audit 01: SEC_LARGE_PAGES without SeLockMemoryPrivilege → ERROR_PRIVILEGE_NOT_HELD.
TEST_F(FileMappingTest, SecLargePagesRequiresPrivilege) {
    constexpr u32 SEC_COMMIT = 0x08000000u;
    constexpr u32 SEC_LARGE_PAGES = 0x80000000u;
    set_last_error(0);
    EXPECT_EQ(mgr->create_file_mapping(
                  FILE_MAPPING_INVALID_FILE,
                  PAGE_READWRITE | SEC_COMMIT | SEC_LARGE_PAGES, 0x200000, ""),
              NULL_HANDLE);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_PRIVILEGE_NOT_HELD);
    set_last_error(0);
    EXPECT_EQ(mgr->create_file_mapping(
                  FILE_MAPPING_INVALID_FILE,
                  PAGE_READWRITE | SEC_LARGE_PAGES, 0x200000, ""),
              NULL_HANDLE);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_PARAMETER);
}

// Audit 01 CreateFileMappingW #4: SEC_IMAGE maps section-aligned PE as MEM_IMAGE
// with per-section PAGE_* (Windows sec_attrs_probe / tiny_sec_image.dll).
TEST(FileMappingImageTest, SecImageMapsPeWithPerSectionProtection) {
    const auto src =
        std::filesystem::path(__FILE__).parent_path()  // kernel32
            .parent_path()                             // winapi
            .parent_path()                             // tests
            .parent_path() /                           // repo
        "tests" / "oracle_data" / "tiny_sec_image.dll";
    if (!std::filesystem::exists(src)) {
        GTEST_SKIP() << "missing tiny_sec_image.dll fixture at " << src;
    }
    const auto root =
        std::filesystem::temp_directory_path() / "vitreo_sec_image_test";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    std::filesystem::copy_file(src, root / "tiny.dll");

    capsule::Vfs vfs;
    ASSERT_TRUE(vfs.mount_drive(
                    'C', std::make_unique<capsule::HostFileSystem>(root))
                    .is_ok());
    GuestMemory memory;
    if (!memory.enable_linear_reservation(0x200000000ull)) {
        GTEST_SKIP() << "linear reservation unavailable for ImageBase";
    }

    HandleTable handles;
    FileManager files(vfs, handles);
    FileMappingManager mappings(handles, memory);
    mappings.set_file_manager(&files);
    constexpr u32 SEC_IMAGE = 0x01000000u;
    constexpr u32 GENERIC_READ = 0x80000000u;
    constexpr u32 GENERIC_EXECUTE = 0x20000000u;
    constexpr u32 FILE_MAP_READ = 0x0004;
    const Handle file = files.create_file(
        "C:\\tiny.dll", GENERIC_READ | GENERIC_EXECUTE, FILE_SHARE_READ,
        OPEN_EXISTING, 0);
    ASSERT_NE(file, INVALID_HANDLE);
    EXPECT_EQ(handles.get_type(file), HandleType::File)
        << "file handle type mismatch";
    set_last_error(0);
    Handle section = mappings.create_file_mapping(
        file, PAGE_READONLY | SEC_IMAGE, 0, "");
    ASSERT_NE(section, NULL_HANDLE)
        << "err=" << get_last_error()
        << " file=0x" << std::hex << file
        << " type=" << static_cast<int>(handles.get_type(file));
    GuestAddr view = mappings.map_view_of_file(section, FILE_MAP_READ, 0, 0);
    ASSERT_NE(view, 0u);

    VirtualMemoryManager vmm(memory);
    VirtualQueryManager vqm(memory, &vmm, &mappings);
    MemoryBasicInformation info{};
    ASSERT_EQ(vqm.virtual_query(view, info), sizeof(info));
    EXPECT_EQ(info.type, MEM_IMAGE);
    EXPECT_EQ(info.state, MEM_COMMIT);
    EXPECT_EQ(info.protect, PAGE_READONLY);
    EXPECT_EQ(info.allocation_protect, PAGE_EXECUTE_WRITECOPY);

    ASSERT_EQ(vqm.virtual_query(view + 0x1000, info), sizeof(info));
    EXPECT_EQ(info.type, MEM_IMAGE);
    EXPECT_EQ(info.protect, PAGE_EXECUTE_READ);

    ASSERT_EQ(vqm.virtual_query(view + 0x2000, info), sizeof(info));
    EXPECT_EQ(info.type, MEM_IMAGE);
    EXPECT_EQ(info.protect, PAGE_WRITECOPY);

    u8 b = 0;
    ASSERT_TRUE(memory.read_bytes(view + 0x1000, &b, 1).is_ok());
    EXPECT_EQ(b, 0xC3);
    ASSERT_TRUE(memory.read_bytes(view + 0x2000, &b, 1).is_ok());
    EXPECT_EQ(b, 0x42);
    std::filesystem::remove_all(root);
}

namespace {

// tools/oracle_probe/view_region_lasterror_probe.c's image: .text RX 0x1000,
// .rdata R 0x2000, .rsrc R 0x3000, .data RW 0x4000; SizeOfImage 0x5000 from
// a 0xC00-byte file, so the view is larger than the file.
std::vector<u8> build_region_probe_image(bool pe64) {
    std::vector<u8> f(0xC00, 0);
    auto put16 = [&](usize at, u16 v) { std::memcpy(&f[at], &v, 2); };
    auto put32 = [&](usize at, u32 v) { std::memcpy(&f[at], &v, 4); };
    auto put64 = [&](usize at, u64 v) { std::memcpy(&f[at], &v, 8); };
    put16(0, 0x5A4D);
    put32(0x3C, 0x80);
    put32(0x80, 0x4550);
    const usize fh = 0x84;
    put16(fh, pe64 ? 0x8664 : 0x14C);
    put16(fh + 2, 4);
    const u16 opt_size = pe64 ? 0xF0 : 0xE0;
    put16(fh + 16, opt_size);
    put16(fh + 18, pe64 ? 0x2022 : 0x2102);
    const usize oh = fh + 20;
    put16(oh, pe64 ? 0x20B : 0x10B);
    if (pe64) put64(oh + 24, 0x20000000ull); else put32(oh + 28, 0x20000000u);
    put32(oh + 32, 0x1000);
    put32(oh + 36, 0x200);
    put16(oh + 40, 6);
    put16(oh + 48, 6);
    put32(oh + 56, 0x5000);
    put32(oh + 60, 0x400);
    put16(oh + 68, 2);
    if (pe64) {
        put64(oh + 72, 0x100000); put64(oh + 80, 0x1000);
        put64(oh + 88, 0x100000); put64(oh + 96, 0x1000);
        put32(oh + 108, 16);
    } else {
        put32(oh + 72, 0x100000); put32(oh + 76, 0x1000);
        put32(oh + 80, 0x100000); put32(oh + 84, 0x1000);
        put32(oh + 92, 16);
    }
    const char* names[4] = {".text", ".rdata", ".rsrc", ".data"};
    const u32 chars[4] = {0x60000020u, 0x40000040u, 0x40000040u, 0xC0000040u};
    for (int i = 0; i < 4; ++i) {
        const usize sh = oh + opt_size + static_cast<usize>(i) * 40;
        std::memcpy(&f[sh], names[i], std::strlen(names[i]));
        put32(sh + 8, 0x100);
        put32(sh + 12, 0x1000u * static_cast<u32>(i + 1));
        put32(sh + 16, 0x200);
        put32(sh + 20, 0x400u + 0x200u * static_cast<u32>(i));
        put32(sh + 36, chars[i]);
        f[0x400 + 0x200 * static_cast<usize>(i)] = static_cast<u8>(0xA0 + i);
    }
    return f;
}

}  // namespace

// view_region_lasterror_probe (x86 == x64, PE32 == PE32+): VirtualQuery
// reports adjacent same-protection image pages as one region, a mapped view
// leaves the last error alone, and UnmapViewOfFile of an image view frees its
// whole SizeOfImage range so the same base can be mapped again. Unmapping by
// the file size left the image mapped, which made the x86 CLR's
// ConvertedImageLayout miss the assembly's preferred base.
TEST(FileMappingImageTest, ImageViewRegionsLastErrorAndUnmap) {
    for (bool pe64 : {false, true}) {
        SCOPED_TRACE(pe64 ? "PE32+" : "PE32");
        const auto root = std::filesystem::temp_directory_path() /
                          "vitreo_view_region_test";
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root);
        {
            const auto image = build_region_probe_image(pe64);
            std::ofstream out(root / "img.dll", std::ios::binary);
            out.write(reinterpret_cast<const char*>(image.data()),
                      static_cast<std::streamsize>(image.size()));
        }
        capsule::Vfs vfs;
        ASSERT_TRUE(vfs.mount_drive(
                        'C', std::make_unique<capsule::HostFileSystem>(root))
                        .is_ok());
        GuestMemory memory;
        HandleTable handles;
        FileManager files(vfs, handles);
        FileMappingManager mappings(handles, memory);
        mappings.set_file_manager(&files);
        constexpr u32 SEC_IMAGE = 0x01000000u;
        constexpr u32 GENERIC_READ = 0x80000000u;
        constexpr u32 FILE_MAP_READ = 0x0004;
        constexpr u32 FILE_MAP_ALL_ACCESS = 0x000F001Fu;
        const Handle file = files.create_file(
            "C:\\img.dll", GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING, 0);
        ASSERT_NE(file, INVALID_HANDLE);
        const Handle section =
            mappings.create_file_mapping(file, PAGE_READONLY | SEC_IMAGE, 0, "");
        ASSERT_NE(section, NULL_HANDLE) << get_last_error();

        set_last_error(0xDEAD);
        const GuestAddr view = mappings.map_view_of_file_ex(section, 0, 0, 0, 0);
        ASSERT_NE(view, 0u);
        EXPECT_EQ(get_last_error(), 0xDEADu);

        VirtualMemoryManager vmm(memory);
        VirtualQueryManager vqm(memory, &vmm, &mappings);
        struct Region { u64 off, size; u32 prot; };
        const Region expected[] = {
            {0x0, 0x1000, PAGE_READONLY},
            {0x1000, 0x1000, PAGE_EXECUTE_READ},
            {0x2000, 0x2000, PAGE_READONLY},
            {0x4000, 0x1000, PAGE_WRITECOPY},
        };
        u64 off = 0;
        for (const Region& r : expected) {
            MemoryBasicInformation info{};
            ASSERT_EQ(vqm.virtual_query(view + off, info), sizeof(info));
            EXPECT_EQ(off, r.off);
            EXPECT_EQ(info.region_size, r.size) << "off=0x" << std::hex << off;
            EXPECT_EQ(info.protect, r.prot) << "off=0x" << std::hex << off;
            EXPECT_EQ(info.allocation_protect, PAGE_EXECUTE_WRITECOPY);
            EXPECT_EQ(info.state, MEM_COMMIT);
            EXPECT_EQ(info.type, MEM_IMAGE);
            off += info.region_size;
        }

        set_last_error(0xDEAD);
        const GuestAddr second = mappings.map_view_of_file(section, FILE_MAP_READ, 0, 0);
        ASSERT_NE(second, 0u);
        EXPECT_NE(second, view);
        EXPECT_EQ(get_last_error(), 0xDEADu);

        set_last_error(0xDEAD);
        ASSERT_TRUE(mappings.unmap_view_of_file(view));
        EXPECT_EQ(get_last_error(), 0xDEADu);
        MemoryBasicInformation tail{};
        ASSERT_EQ(vqm.virtual_query(view + 0x4000, tail), sizeof(tail));
        EXPECT_EQ(tail.state, MEM_FREE);

        const Handle pagefile = mappings.create_file_mapping(
            FILE_MAPPING_INVALID_FILE, PAGE_READWRITE, 0x5000, "");
        ASSERT_NE(pagefile, NULL_HANDLE);
        set_last_error(0xDEAD);
        EXPECT_EQ(mappings.map_view_of_file_ex(pagefile, FILE_MAP_ALL_ACCESS, 0,
                                               0, view),
                  view);
        EXPECT_EQ(get_last_error(), 0xDEADu);
        std::filesystem::remove_all(root);
    }
}

// view_region_lasterror_probe "data_*" / "copy_dirty": a protection change
// splits a data view's region and changing it back rejoins it; a privately
// written FILE_MAP_COPY page is its own region.
TEST_F(FileMappingTest, DataViewRegionsSplitAndRejoin) {
    constexpr u32 FILE_MAP_COPY = 0x0001;
    constexpr u32 FILE_MAP_ALL_ACCESS = 0x000F001Fu;
    const Handle section = mgr->create_file_mapping(
        FILE_MAPPING_INVALID_FILE, PAGE_READWRITE, 0x4000, "");
    ASSERT_NE(section, NULL_HANDLE);
    set_last_error(0xDEAD);
    const GuestAddr view = mgr->map_view_of_file(section, FILE_MAP_ALL_ACCESS, 0, 0);
    ASSERT_NE(view, 0u);
    EXPECT_EQ(get_last_error(), 0xDEADu);

    VirtualMemoryManager vmm(*mem);
    VirtualQueryManager vqm(*mem, &vmm, mgr.get());
    MemoryBasicInformation info{};
    ASSERT_EQ(vqm.virtual_query(view, info), sizeof(info));
    EXPECT_EQ(info.region_size, 0x4000u);
    u32 old = 0;
    ASSERT_TRUE(vqm.virtual_protect(view + 0x1000, 0x1000, PAGE_READONLY, &old));
    ASSERT_EQ(vqm.virtual_query(view, info), sizeof(info));
    EXPECT_EQ(info.region_size, 0x1000u);
    ASSERT_EQ(vqm.virtual_query(view + 0x1000, info), sizeof(info));
    EXPECT_EQ(info.region_size, 0x1000u);
    EXPECT_EQ(info.protect, PAGE_READONLY);
    ASSERT_EQ(vqm.virtual_query(view + 0x2000, info), sizeof(info));
    EXPECT_EQ(info.region_size, 0x2000u);
    ASSERT_TRUE(vqm.virtual_protect(view + 0x1000, 0x1000, PAGE_READWRITE, &old));
    ASSERT_EQ(vqm.virtual_query(view, info), sizeof(info));
    EXPECT_EQ(info.region_size, 0x4000u);
    EXPECT_EQ(info.protect, PAGE_READWRITE);

    set_last_error(0xDEAD);
    const GuestAddr copy = mgr->map_view_of_file(section, FILE_MAP_COPY, 0, 0);
    ASSERT_NE(copy, 0u);
    EXPECT_EQ(get_last_error(), 0xDEADu);
    ASSERT_TRUE(mem->write_u8(copy + 0x1000, 1).is_ok());
    const struct { u64 off, size; u32 prot; } regions[] = {
        {0x0, 0x1000, PAGE_WRITECOPY},
        {0x1000, 0x1000, PAGE_READWRITE},
        {0x2000, 0x2000, PAGE_WRITECOPY},
    };
    for (const auto& r : regions) {
        ASSERT_EQ(vqm.virtual_query(copy + r.off, info), sizeof(info));
        EXPECT_EQ(info.region_size, r.size) << "off=0x" << std::hex << r.off;
        EXPECT_EQ(info.protect, r.prot) << "off=0x" << std::hex << r.off;
        EXPECT_EQ(info.allocation_protect, PAGE_WRITECOPY);
    }
}

namespace {

struct LargeImageLayout {
    u32 rva;
    u32 raw_offset;
    u32 raw_size;
    u32 virtual_size;
    u32 characteristics;
    u32 win32_protect;  // Learn "Memory Protection Constants", unwritten page
};

// PE/COFF: FileAlignment 0x200, SectionAlignment 0x1000, so no section is
// 16 KiB-aligned in the file. SizeOfImage 0x801000 leaves the last host page
// partly outside the image. .data has VirtualSize > SizeOfRawData.
constexpr std::array<LargeImageLayout, 3> kLargeImage{{
    {0x1000, 0x400, 0x400000, 0x400000, 0x60000020u, 0x20},       // RX code
    {0x401000, 0x400400, 0x300000, 0x300000, 0x40000040u, 0x02},   // R data
    {0x701000, 0x700400, 0x200, 0x100000, 0xC0000040u, 0x08},      // RW data
}};
constexpr u32 kLargeImageSize = 0x801000;

std::vector<u8> build_large_image(bool x64) {
    std::vector<u8> f(0x700600, 0);
    for (usize i = 0x400; i < f.size(); ++i) {
        f[i] = static_cast<u8>((i * 2654435761u) >> 24);
    }
    auto put16 = [&](usize o, u16 v) { std::memcpy(&f[o], &v, 2); };
    auto put32 = [&](usize o, u32 v) { std::memcpy(&f[o], &v, 4); };
    auto put64 = [&](usize o, u64 v) { std::memcpy(&f[o], &v, 8); };
    std::memset(f.data(), 0, 0x400);
    put16(0, 0x5A4D);       // MZ
    put32(0x3C, 0x80);      // e_lfanew
    put32(0x80, 0x00004550);
    put16(0x84, x64 ? 0x8664 : 0x014C);
    put16(0x86, static_cast<u16>(kLargeImage.size()));
    const u16 opt_size = x64 ? 0xF0 : 0xE0;
    put16(0x94, opt_size);
    // EXECUTABLE_IMAGE | DLL | (32BIT_MACHINE or LARGE_ADDRESS_AWARE)
    put16(0x96, x64 ? 0x2022 : 0x2102);
    const usize opt = 0x98;
    put16(opt, x64 ? 0x20B : 0x10B);
    if (x64) {
        put64(opt + 24, 0x180000000ull);
    } else {
        put32(opt + 28, 0x10000000u);
    }
    put32(opt + 32, 0x1000);    // SectionAlignment
    put32(opt + 36, 0x200);     // FileAlignment
    put16(opt + 40, 6);         // MajorOperatingSystemVersion
    put16(opt + 48, 6);         // MajorSubsystemVersion
    put32(opt + 56, kLargeImageSize);
    put32(opt + 60, 0x400);     // SizeOfHeaders
    put16(opt + 68, 2);         // IMAGE_SUBSYSTEM_WINDOWS_GUI
    put32(opt + (x64 ? 108 : 92), 16);  // NumberOfRvaAndSizes
    usize sh = opt + opt_size;
    const char* names[] = {".text", ".rdata", ".data"};
    for (usize i = 0; i < kLargeImage.size(); ++i, sh += 40) {
        std::memcpy(&f[sh], names[i], std::strlen(names[i]));
        put32(sh + 8, kLargeImage[i].virtual_size);
        put32(sh + 12, kLargeImage[i].rva);
        put32(sh + 16, kLargeImage[i].raw_size);
        put32(sh + 20, kLargeImage[i].raw_offset);
        put32(sh + 36, kLargeImage[i].characteristics);
    }
    return f;
}

/// Host pages of [p, p+len) holding data that belongs to this process alone
/// (anonymous or copy-on-written), as opposed to clean pages of a file.
usize private_host_pages(const void* p, usize len) {
    const usize page = static_cast<usize>(::getpagesize());
    const auto lo = reinterpret_cast<mach_vm_address_t>(p) & ~(page - 1);
    const auto hi = (reinterpret_cast<mach_vm_address_t>(p) + len + page - 1) &
                    ~(page - 1);
    std::vector<int> disp((hi - lo) / page);
    mach_vm_size_t count = disp.size();
    if (mach_vm_page_range_query(
            mach_task_self(), lo, hi - lo,
            reinterpret_cast<mach_vm_address_t>(disp.data()), &count) !=
        KERN_SUCCESS) {
        return disp.size();
    }
    usize n = 0;
    for (mach_vm_size_t i = 0; i < count; ++i) {
        const int d = disp[i];
        const bool data =
            (d & (VM_PAGE_QUERY_PAGE_PRESENT | VM_PAGE_QUERY_PAGE_PAGED_OUT)) != 0;
        if (data && (d & VM_PAGE_QUERY_PAGE_EXTERNAL) == 0) ++n;
    }
    return n;
}

class ScopedEnv {
public:
    ScopedEnv(const char* name, const std::string& value) : m_name(name) {
        if (const char* old = std::getenv(name)) m_old = old;
        ::setenv(name, value.c_str(), 1);
    }
    ~ScopedEnv() {
        if (m_old) ::setenv(m_name, m_old->c_str(), 1);
        else ::unsetenv(m_name);
    }
private:
    const char* m_name;
    std::optional<std::string> m_old;
};

}  // namespace

// Learn CreateFileMapping SEC_IMAGE / PE-COFF section table: an image view
// lays the file out by section and every page the process has not written is
// shared, file-backed memory. Mapping a 8 MiB image must not leave a private
// copy of its read-only sections behind, for x86 and x64 alike, while the
// bytes, MEM_IMAGE type and per-section protections stay exactly Windows'.
TEST(FileMappingImageTest, LargeSecImageReadOnlySectionsStayFileBacked) {
    const auto root =
        std::filesystem::temp_directory_path() / "vitreo_sec_image_large";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "drive");
    const ScopedEnv cache_dir("VITREO_IMAGE_LAYOUT_CACHE_DIR",
                              (root / "layouts").string());
    for (const bool x64 : {false, true}) {
        SCOPED_TRACE(x64 ? "x64" : "x86");
        const std::vector<u8> file = build_large_image(x64);
        {
            std::ofstream out(root / "drive" / "big.dll", std::ios::binary);
            out.write(reinterpret_cast<const char*>(file.data()),
                      static_cast<std::streamsize>(file.size()));
        }
        const auto map_image = [&](bool file_backing, auto&& check) {
            const ScopedEnv backing("VITREO_IMAGE_FILE_BACKING",
                                    file_backing ? "1" : "0");
            capsule::Vfs vfs;
            ASSERT_TRUE(vfs.mount_drive('C', std::make_unique<capsule::HostFileSystem>(
                                                 root / "drive"))
                            .is_ok());
            GuestMemory memory;
            ASSERT_TRUE(memory.enable_linear_reservation(0x200000000ull));
            HandleTable handles;
            FileManager files(vfs, handles);
            FileMappingManager mappings(handles, memory);
            mappings.set_file_manager(&files);
            const Handle fh = files.create_file(
                "C:\\big.dll", 0x80000000u | 0x20000000u, FILE_SHARE_READ,
                OPEN_EXISTING, 0);
            ASSERT_NE(fh, INVALID_HANDLE);
            const Handle section = mappings.create_file_mapping(
                fh, PAGE_READONLY | 0x01000000u /*SEC_IMAGE*/, 0, "");
            ASSERT_NE(section, NULL_HANDLE) << "err=" << get_last_error();
            const GuestAddr view =
                mappings.map_view_of_file(section, 0x0004 /*FILE_MAP_READ*/, 0, 0);
            ASSERT_NE(view, 0u);
            ASSERT_EQ(view % 0x10000, 0u);
            check(memory, mappings, view);
        };

        map_image(true, [&](GuestMemory& memory, FileMappingManager& mappings,
                            GuestAddr view) {
            VirtualMemoryManager vmm(memory);
            VirtualQueryManager vqm(memory, &vmm, &mappings);
            MemoryBasicInformation info{};
            ASSERT_EQ(vqm.virtual_query(view, info), sizeof(info));
            EXPECT_EQ(info.type, MEM_IMAGE);
            EXPECT_EQ(info.allocation_base, view);
            EXPECT_EQ(info.protect, PAGE_READONLY);
            const Byte* host = memory.host_ptr(view, kLargeImageSize);
            ASSERT_NE(host, nullptr);
            EXPECT_EQ(std::memcmp(host, file.data(), 0x400), 0) << "headers";
            for (const auto& s : kLargeImage) {
                ASSERT_EQ(vqm.virtual_query(view + s.rva, info), sizeof(info));
                EXPECT_EQ(info.type, MEM_IMAGE);
                EXPECT_EQ(info.allocation_base, view);
                EXPECT_EQ(info.protect, s.win32_protect) << std::hex << s.rva;
                EXPECT_EQ(std::memcmp(host + s.rva, file.data() + s.raw_offset,
                                      s.raw_size), 0)
                    << "section bytes at rva 0x" << std::hex << s.rva;
                for (u32 off = s.raw_size; off < s.virtual_size; off += 0x1000) {
                    ASSERT_EQ(host[s.rva + off], 0u) << "zero tail";
                }
            }
            // Read-only .text + .rdata: every byte was just read, so all
            // their pages are resident -- and none may be private.
            const usize ro_len = 0x700000;
            EXPECT_LE(private_host_pages(host + 0x1000, ro_len), 2u);

            // A guest store to .data lands in a private page, never the file.
            const GuestAddr data = view + kLargeImage[2].rva + 0x40000;
            ASSERT_TRUE(memory.protect_region(data, 0x1000,
                                              MemoryProtection::ReadWrite)
                            .is_ok());
            ASSERT_TRUE(memory.write_u32(data, 0xC0FFEE11u).is_ok());
            EXPECT_EQ(memory.read_u32(data).value_or(0), 0xC0FFEE11u);
            EXPECT_GE(private_host_pages(host + kLargeImage[2].rva + 0x40000, 4),
                      1u);
        });

        // The same measurement without file backing proves it detects the
        // anonymous copy the mapper used to make.
        map_image(false, [&](GuestMemory& memory, FileMappingManager&,
                             GuestAddr view) {
            const Byte* host = memory.host_ptr(view, kLargeImageSize);
            ASSERT_NE(host, nullptr);
            EXPECT_EQ(std::memcmp(host + 0x1000, file.data() + 0x400, 0x400000), 0);
            EXPECT_GE(private_host_pages(host + 0x1000, 0x700000),
                      0x700000 / static_cast<usize>(::getpagesize()) - 2);
        });
    }
    // One content-addressed layout per distinct image, shared by every mapper.
    usize layouts = 0;
    for (const auto& e : std::filesystem::directory_iterator(root / "layouts")) {
        if (e.path().extension() == ".img") ++layouts;
    }
    EXPECT_EQ(layouts, 2u);
    std::filesystem::remove_all(root);
}

// Audit 01 CreateFileMappingW #3: cross-type BaseNamedObjects → ERROR_INVALID_HANDLE.
TEST(FileMappingNameTest, CrossTypeNameCollisionFailsInvalidHandle) {
    GuestMemory mem;
    HandleTable handles;
    FileMappingManager fmap(handles, mem);
    SyncManager sync(handles);
    const std::string name = "VitreoSecAttrsNameProbeGTest";

    Handle e = sync.create_event(false, false, name);
    ASSERT_NE(e, NULL_HANDLE);
    set_last_error(0xDEAD);
    EXPECT_EQ(fmap.create_file_mapping(FILE_MAPPING_INVALID_FILE,
                                       PAGE_READWRITE, 0x1000, name),
              NULL_HANDLE);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_HANDLE);
    sync.on_handle_close(e, HandleType::Event);
    handles.close(e);

    Handle m = fmap.create_file_mapping(FILE_MAPPING_INVALID_FILE,
                                        PAGE_READWRITE, 0x1000, name);
    ASSERT_NE(m, NULL_HANDLE);
    set_last_error(0xDEAD);
    EXPECT_EQ(sync.create_event(false, false, name), NULL_HANDLE);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_HANDLE);
    fmap.on_handle_close(m, HandleType::FileMapping);
    handles.close(m);

    Handle mx = sync.create_mutex(false, name);
    ASSERT_NE(mx, NULL_HANDLE);
    set_last_error(0xDEAD);
    EXPECT_EQ(fmap.create_file_mapping(FILE_MAPPING_INVALID_FILE,
                                       PAGE_READWRITE, 0x1000, name),
              NULL_HANDLE);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_HANDLE);
}

TEST_F(FileMappingTest, AllAccessViewUsesWriteRightWithoutRequestingObjectAdministrationRights) {
    const std::string name="VitreoMapNormalize_"+std::to_string(::getpid());
    const Handle creator=mgr->create_file_mapping(FILE_MAPPING_INVALID_FILE,PAGE_READWRITE,0x200000,name);
    ASSERT_NE(creator,0u);
    const Handle writer=mgr->open_file_mapping(0x2,name);
    ASSERT_NE(writer,0u);ASSERT_EQ(handles->get_access(writer),0x2u);
    set_last_error(0x12345678);
    const auto view=mgr->map_view_of_file(writer,0xf001f,0,0x200000);
    EXPECT_NE(view,0u);EXPECT_EQ(get_last_error(),0x12345678u);
    if(view) { EXPECT_TRUE(mem->write_u32(view,0x12345678).is_ok());EXPECT_TRUE(mgr->unmap_view_of_file(view)); }
    const Handle reader=mgr->open_file_mapping(0x4,name);ASSERT_NE(reader,0u);
    EXPECT_EQ(mgr->map_view_of_file(reader,0xf001f,0,0x1000),0u);
    EXPECT_EQ(get_last_error(),error_codes::ERROR_ACCESS_DENIED);
}

#include "mapview_oracle.inc"
TEST_F(FileMappingTest, WindowsMeasuredViewRightsAndProtectionMatrix) {
    handles->set_close_callback([this](Handle h, HandleType type, void*) {
        return mgr->on_handle_close(h,type);
    });
    unsigned count=0;
    for (const auto& row:mapview_cases) {
        // Image page protections depend on a real PE fixture, covered by the
        // separate SEC_IMAGE suite. This matrix exercises data sections.
        if (row.protection & 0x1000000u) continue;
        SCOPED_TRACE(::testing::Message()<<"row "<<count++<<" bitness "<<row.x64
            <<" protection "<<row.protection<<" access "<<row.access<<" view "<<row.view);
        const std::string name="VitreoMapOracle_"+std::to_string(::getpid())+"_"+std::to_string(count);
        const Handle creator=mgr->create_file_mapping(FILE_MAPPING_INVALID_FILE,row.protection,0x30000,name);
        ASSERT_NE(creator,0u);
        const Handle handle=row.duplicate ? mgr->duplicate_handle(creator,0,row.access)
                                          : mgr->open_file_mapping(row.access,name);
        ASSERT_NE(handle,0u);
        EXPECT_EQ(handles->get_access(handle),row.grant);
        set_last_error(0x12345678);
        const auto view=mgr->map_view_of_file(handle,row.view,row.offset,row.bytes);
        EXPECT_EQ(view!=0,row.ok);EXPECT_EQ(get_last_error(),row.last);
        if(view) {
            GuestAddr base;u64 size;u32 protect,allocation;
            EXPECT_TRUE(mgr->query_view(view,base,size,&protect));
            EXPECT_EQ(protect,row.page);
            EXPECT_TRUE(mgr->unmap_view_of_file(view));
        }
        EXPECT_TRUE(handles->close(handle));EXPECT_TRUE(handles->close(creator));
    }
}

TEST_F(FileMappingTest, RemoteSectionUsesImmutableCreatorProtection) {
    HandleTable peer_handles;GuestMemory peer_memory;
    FileMappingManager peer(peer_handles,peer_memory);
    const std::string name="VitreoMapReadonly_"+std::to_string(::getpid());
    const auto creator=mgr->create_file_mapping(FILE_MAPPING_INVALID_FILE,PAGE_READONLY,0x20000,name);
    ASSERT_NE(creator,0u);
    const auto writer=peer.open_file_mapping(0x2,name);ASSERT_NE(writer,0u);
    EXPECT_EQ(peer.map_view_of_file(writer,0xf001f,0,0x1000),0u);
    EXPECT_EQ(get_last_error(),error_codes::ERROR_INVALID_PARAMETER);
    // A second CreateFileMapping cannot replace the creator's protection.
    const auto existing=peer.create_file_mapping(FILE_MAPPING_INVALID_FILE,PAGE_READWRITE,0x1000,name);
    ASSERT_NE(existing,0u);
    EXPECT_EQ(peer.map_view_of_file(existing,0xf001f,0,0x1000),0u);
    EXPECT_EQ(get_last_error(),error_codes::ERROR_INVALID_PARAMETER);
    const auto reader=peer.open_file_mapping(0x4,name);ASSERT_NE(reader,0u);
    const auto view=peer.map_view_of_file(reader,0x4,0,0x1000);
    ASSERT_NE(view,0u);EXPECT_TRUE(peer.unmap_view_of_file(view));
}
