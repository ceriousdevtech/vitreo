// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS
//
// MEM_WRITE_WATCH hardware tracking (write_watch::enable_hardware_traps).
//
// The contract these tests pin is the one that broke The Sims 3 on native32:
// GetWriteWatch with WRITE_WATCH_FLAG_RESET must SHRINK its answer. A caller
// that drains the buffer with `do { ... } while (count == capacity)` -- which
// is what Boehm's GC_gww_read_dirty does -- loops forever against a reporter
// that answers with the whole committed region every time. So "report a
// conservative superset because the stores are untracked" is not a safe
// fallback; the stores have to actually be tracked.

#include "core/memory/write_watch_signal.hpp"
#include "core/memory/write_watch_traps.hpp"

#include <gtest/gtest.h>

#include <sys/mman.h>
#include <unistd.h>

#include <cstring>
#include <vector>

namespace {

using namespace vitreo;

constexpr u64 kPages = 8;
constexpr u64 kRegionSize = kPages * write_watch::kGuestPage;

// The arming is per HOST page. Only a host page that is exactly the guest page
// gives exact tracking: on a 16 KiB host page one trap would unprotect four
// guest pages and silently lose the stores to the other three. That is why
// native32 (x86_64, 4 KiB pages) arms and Apple Silicon does not.
bool host_page_is_guest_page() {
    return write_watch::host_page_size() == write_watch::kGuestPage;
}

/// Owns the mmap that stands in for the guest's linear window, plus the dirty
/// and armed bit arrays the VirtualMemoryManager would normally own.
class WriteWatchRegion {
  public:
    WriteWatchRegion() {
        m_map = static_cast<u8*>(::mmap(nullptr, kRegionSize,
                                        PROT_READ | PROT_WRITE,
                                        MAP_PRIVATE | MAP_ANON, -1, 0));
        EXPECT_NE(m_map, MAP_FAILED);
        // native32 maps the guest identically, so the guest address IS the
        // host address and the base offset is zero. Model that here rather
        // than inventing a base: it is the only configuration that arms.
        m_base = reinterpret_cast<u64>(m_map);
        m_dirty.assign((kPages + 7) / 8, 0);
        m_armed.assign((kPages + 7) / 8, 0);
        write_watch::enable_hardware_traps(0);
        memory::install_write_watch_signal_handler();
        write_watch::register_region(m_base, kRegionSize, m_dirty.data(),
                                     m_armed.data(), /*executable=*/false);
    }

    ~WriteWatchRegion() {
        write_watch::unregister_region(m_base);
        if (m_map != MAP_FAILED) {
            (void)::mprotect(m_map, kRegionSize, PROT_READ | PROT_WRITE);
            (void)::munmap(m_map, kRegionSize);
        }
    }

    u8* page(u64 index) { return m_map + index * write_watch::kGuestPage; }

    /// Arm every clean page, as ResetWriteWatch/GetWriteWatch(RESET) does.
    void arm_all() {
        write_watch::arm_range(m_base, m_base, m_base + kRegionSize,
                               m_dirty.data(), m_armed.data(),
                               /*executable=*/false);
    }

    /// The reporting half of GetWriteWatch(WRITE_WATCH_FLAG_RESET): collect the
    /// dirty pages, clear them, and re-arm. Mirrors
    /// VirtualMemoryManager::get_write_watch for a fully committed region.
    std::vector<u64> drain() {
        std::vector<u64> out;
        for (u64 i = 0; i < kPages; ++i) {
            if (write_watch::test_bit(m_dirty.data(), i) ||
                write_watch::test_linear_page(m_base +
                                              i * write_watch::kGuestPage)) {
                out.push_back(i);
            }
        }
        for (u64 i : out) {
            write_watch::clear_bit(m_dirty.data(), i);
        }
        write_watch::clear_linear_pages(m_base, m_base + kRegionSize);
        arm_all();
        return out;
    }

    [[nodiscard]] u64 guest_base() const { return m_base; }

  private:
    u8* m_map{nullptr};
    u64 m_base{0};
    std::vector<u8> m_dirty;
    std::vector<u8> m_armed;
};

class WriteWatchHardware : public ::testing::Test {
  protected:
    void SetUp() override {
        if (!host_page_is_guest_page()) {
            GTEST_SKIP() << "host page " << write_watch::host_page_size()
                         << " != guest page; arming is only exact at 4 KiB";
        }
    }
};

TEST_F(WriteWatchHardware, StoreThroughArmedPageIsRecovered) {
    WriteWatchRegion region;
    region.arm_all();
    // A plain store to an armed page takes a protection fault, which the
    // handler must turn into a dirty bit and a retry -- not a crash.
    *region.page(3) = 0x5a;
    EXPECT_EQ(*region.page(3), 0x5a);

    const std::vector<u64> dirty = region.drain();
    ASSERT_EQ(dirty.size(), 1u);
    EXPECT_EQ(dirty[0], 3u);
}

TEST_F(WriteWatchHardware, ResetShrinksTheAnswer) {
    WriteWatchRegion region;
    region.arm_all();
    *region.page(1) = 1;
    *region.page(6) = 1;

    const std::vector<u64> first = region.drain();
    EXPECT_EQ(first, (std::vector<u64>{1, 6}));

    // The drain loop's termination condition: with nothing written since the
    // reset, the second call reports nothing. A superset reporter answers
    // "all 8 pages" here forever and the caller never leaves the loop.
    EXPECT_TRUE(region.drain().empty());
}

TEST_F(WriteWatchHardware, PageIsRearmedAfterEveryReset) {
    WriteWatchRegion region;
    region.arm_all();
    for (int round = 0; round < 3; ++round) {
        *region.page(2) = static_cast<u8>(round);
        const std::vector<u64> dirty = region.drain();
        ASSERT_EQ(dirty.size(), 1u) << "round " << round;
        EXPECT_EQ(dirty[0], 2u) << "round " << round;
    }
}

TEST_F(WriteWatchHardware, UnwrittenPagesAreNeverReported) {
    WriteWatchRegion region;
    region.arm_all();
    *region.page(0) = 1;
    // Reads must not arm anything dirty.
    volatile u8 sink = 0;
    for (u64 i = 0; i < kPages; ++i) {
        sink = static_cast<u8>(sink + *region.page(i));
    }
    (void)sink;
    EXPECT_EQ(region.drain(), (std::vector<u64>{0}));
}

TEST_F(WriteWatchHardware, HostSideRecordStoreMarksAndUnprotects) {
    WriteWatchRegion region;
    region.arm_all();
    // Vitreo's own writes into guest memory go through record_store, which has
    // to lift the arming itself -- a memcpy spanning several pages must not
    // fault part-way through.
    const u64 addr = region.guest_base() + 4 * write_watch::kGuestPage;
    write_watch::record_store(static_cast<GuestAddr>(addr),
                              2 * write_watch::kGuestPage);
    std::memset(region.page(4), 0xcc, 2 * write_watch::kGuestPage);
    EXPECT_EQ(region.drain(), (std::vector<u64>{4, 5}));
}

TEST_F(WriteWatchHardware, DiscardPreservesArmingAndDirtyState) {
    WriteWatchRegion region;
    region.arm_all();
    ASSERT_TRUE(write_watch::discard_armed_pages(
        region.page(4), write_watch::kGuestPage));
    EXPECT_TRUE(region.drain().empty())
        << "maintenance discard falsely dirtied a write-watch page";

    *region.page(4) = 0x5A;
    EXPECT_EQ(region.drain(), (std::vector<u64>{4}))
        << "maintenance discard failed to restore the write trap";
}


// Microsoft Learn GetWriteWatch: every page written since reset is reported.
// https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-getwritewatch
class WriteWatchSpan : public ::testing::TestWithParam<u64> {};

TEST_P(WriteWatchSpan, BulkStoreTracksInteriorSlotsAndOnlyTouchedPages) {
    constexpr u64 page = write_watch::kGuestPage;
    const u64 base = GetParam();
    u8 dirty[3] = {}, armed[3] = {};
    const u64 starts[] = {base + page, base + 4 * page, base + 8 * page};
    for (int i = 0; i < 3; ++i)
        write_watch::register_region(starts[i], 2 * page, &dirty[i], &armed[i], false);
    struct Cleanup {
        const u64* starts;
        u64 base;
        ~Cleanup() {
            for (int i = 0; i < 3; ++i) write_watch::unregister_region(starts[i]);
            write_watch::clear_linear_pages(base, base + 10 * page);
        }
    } cleanup{starts, base};
    write_watch::clear_linear_pages(base, base + 10 * page);

    // Both endpoints unwatched; two entire watched slots lie inside the span.
    write_watch::record_store(base, 7 * page);
    EXPECT_EQ(dirty[0], 3);
    EXPECT_EQ(dirty[1], 3);
    EXPECT_EQ(dirty[2], 0);
    std::memset(dirty, 0, sizeof(dirty));
    write_watch::clear_linear_pages(base, base + 10 * page);

    // Distinct watched endpoints, including an interior slot, with untouched
    // sibling pages at either end. Above 4 GiB only slot bits can track these.
    write_watch::record_store(base + 2 * page + 7, 6 * page);
    EXPECT_EQ(dirty[0], 2);
    EXPECT_EQ(dirty[1], 3);
    EXPECT_EQ(dirty[2], 1);
    EXPECT_FALSE(write_watch::test_linear_page(base + page));
    EXPECT_FALSE(write_watch::test_linear_page(base + 9 * page));

    // Scalar cached-hit and same-page-miss fast paths retain exact coverage.
    std::memset(dirty, 0, sizeof(dirty));
    write_watch::clear_linear_pages(base, base + 10 * page);
    write_watch::record_store(base + page + 9, sizeof(u32));
    write_watch::record_store(base + 6 * page + 9, sizeof(u32));
    EXPECT_EQ(dirty[0], 1);
    EXPECT_EQ(dirty[1], 0);
    EXPECT_EQ(dirty[2], 0);
    EXPECT_FALSE(write_watch::test_linear_page(base + 6 * page));
}

INSTANTIATE_TEST_SUITE_P(AddressWidths, WriteWatchSpan,
    ::testing::Values(u64{0x18000000}, u64{0x118000000}));

TEST_F(WriteWatchHardware, BulkStoreUnprotectsEveryOverlappedSlot) {
    constexpr u64 page = write_watch::kGuestPage;
    auto* mapping = static_cast<u8*>(::mmap(nullptr, 8 * page,
        PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0));
    ASSERT_NE(mapping, MAP_FAILED);
    const u64 base = reinterpret_cast<u64>(mapping);
    u8 dirty[2] = {}, armed[2] = {};
    write_watch::enable_hardware_traps(0);
    for (int i = 0; i < 2; ++i) {
        const u64 start = base + (1 + 3 * i) * page;
        write_watch::register_region(start, 2 * page, &dirty[i], &armed[i], false);
        write_watch::arm_range(start, start, start + 2 * page,
            &dirty[i], &armed[i], false);
    }
    write_watch::record_store(base, 8 * page);
    // Check before memcpy: fault recovery must not hide missed unprotection.
    EXPECT_EQ(dirty[0], 3);
    EXPECT_EQ(dirty[1], 3);
    EXPECT_EQ(armed[0], 0);
    EXPECT_EQ(armed[1], 0);
    std::memset(mapping, 0x5a, 8 * page);
    for (int i = 0; i < 2; ++i)
        write_watch::unregister_region(base + (1 + 3 * i) * page);
    write_watch::clear_linear_pages(base, base + 8 * page);
    ::munmap(mapping, 8 * page);
}

} // namespace
