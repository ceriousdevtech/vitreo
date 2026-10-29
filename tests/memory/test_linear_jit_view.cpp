// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
//
// The JIT view: a second mapping of the linear reservation's pages, used only by
// translated code, whose per-16-KiB protection ENFORCES what the guest allows.
// The host view keeps today's permissive protections for host C++ code.
//
// Every test here checks one of the two promises the JIT relies on:
//   * the views alias the same pages, after every operation that can replace
//     the host view's backing (a missed one is silent corruption);
//   * the JIT view never grants an access the guest denies, and denies a store
//     whenever one must reach a helper (translated code, write watch).

#include <gtest/gtest.h>

#include <atomic>
#include <cstring>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <sys/mman.h>
#include <unistd.h>

#include "core/memory/guest_memory.hpp"

using namespace vitreo;

namespace {

constexpr usize kReservation = usize{1} << 37;  // 128 GiB, under the 2^45 cap
constexpr GuestAddr kBase = 0x10'0000'0000ull;  // 64 GiB, inside the window
constexpr usize kHostPage = usize{1} << kLinearHostPageShift;  // 16 KiB

/// Current protection of the JIT view's page holding guest address @p a, as the
/// kernel reports it (VM_PROT_* bits), or -1 when nothing is mapped there.
int view_prot(const GuestMemory& m, GuestAddr a) {
    const mach_vm_address_t want = reinterpret_cast<mach_vm_address_t>(
        m.linear_jit_view_base() + a);
    mach_vm_address_t addr = want;
    mach_vm_size_t size = 0;
    vm_region_basic_info_data_64_t info{};
    mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
    mach_port_t object = MACH_PORT_NULL;
    if (mach_vm_region(mach_task_self(), &addr, &size, VM_REGION_BASIC_INFO_64,
                       reinterpret_cast<vm_region_info_t>(&info), &count,
                       &object) != KERN_SUCCESS ||
        addr > want) {
        return -1;
    }
    return info.protection &
           (VM_PROT_READ | VM_PROT_WRITE | VM_PROT_EXECUTE);
}

class JitViewTest : public ::testing::Test {
protected:
    GuestMemory m;
    void SetUp() override {
        ASSERT_TRUE(m.enable_linear_reservation(kReservation));
        ASSERT_TRUE(m.enable_linear_hardware_protection());
        ASSERT_TRUE(m.enable_linear_jit_view());
        ASSERT_NE(m.linear_jit_view_base(), nullptr);
    }
    u8* host(GuestAddr a) { return m.linear_base() + a; }
    u8* view(GuestAddr a) { return m.linear_jit_view_base() + a; }
};

TEST_F(JitViewTest, JitViewAliasesHostWritesBothWays) {
    ASSERT_TRUE(m.map_region(kBase, kHostPage, MemoryProtection::ReadWrite));
    host(kBase)[0] = 0x11;
    EXPECT_EQ(view(kBase)[0], 0x11u) << "a host-view write is not visible to the JIT";
    view(kBase)[8] = 0x22;
    EXPECT_EQ(host(kBase)[8], 0x22u) << "a JIT-view write is not visible to the host";
    EXPECT_TRUE(m.linear_jit_view_coherent(kBase, kBase + kHostPage));
}

TEST_F(JitViewTest, UniformPageCarriesExactGuestProtection) {
    ASSERT_TRUE(m.map_region(kBase, kHostPage, MemoryProtection::Read));
    EXPECT_EQ(view_prot(m, kBase), VM_PROT_READ);
    ASSERT_TRUE(m.protect_region(kBase, kHostPage, MemoryProtection::ReadWrite));
    EXPECT_EQ(view_prot(m, kBase), VM_PROT_READ | VM_PROT_WRITE);
    ASSERT_TRUE(m.protect_region(kBase, kHostPage, MemoryProtection::None));
    EXPECT_EQ(view_prot(m, kBase), VM_PROT_NONE);
}

TEST_F(JitViewTest, MixedPageIsMostRestrictive) {
    // .rdata beside .data: the host view must stay RW for the RW part, the JIT
    // view must deny the store the R part forbids.
    ASSERT_TRUE(m.map_region(kBase, GUEST_PAGE_SIZE, MemoryProtection::Read));
    ASSERT_TRUE(m.map_region(kBase + GUEST_PAGE_SIZE, kHostPage - GUEST_PAGE_SIZE,
                             MemoryProtection::ReadWrite));
    EXPECT_EQ(view_prot(m, kBase), VM_PROT_READ);
    host(kBase + GUEST_PAGE_SIZE)[0] = 0x5A;  // the host view still permits it
    EXPECT_EQ(view(kBase + GUEST_PAGE_SIZE)[0], 0x5Au);
}

TEST_F(JitViewTest, UncommittedSubpageMakesViewNone) {
    ASSERT_TRUE(m.map_region(kBase, kHostPage - GUEST_PAGE_SIZE,
                             MemoryProtection::ReadWrite));
    EXPECT_EQ(view_prot(m, kBase), VM_PROT_NONE)
        << "a read of the unmapped remainder would succeed through the JIT view";
}

TEST_F(JitViewTest, ExecuteOnlyPageIsNone) {
    // PAGE_EXECUTE without read: the host view must grant READ so the decoder
    // can fetch, but a guest DATA read must fault.
    ASSERT_TRUE(m.map_region(kBase, kHostPage, MemoryProtection::Execute));
    EXPECT_EQ(view_prot(m, kBase), VM_PROT_NONE);
}

TEST_F(JitViewTest, CodePageIsReadOnlyUntilReleased) {
    ASSERT_TRUE(m.map_region(kBase, kHostPage, MemoryProtection::ReadWrite));
    m.retain_jit_code_page(kBase);
    EXPECT_EQ(view_prot(m, kBase), VM_PROT_READ)
        << "a lean store into translated code would skip SMC invalidation";
    m.release_jit_code_page(kBase);
    EXPECT_EQ(view_prot(m, kBase), VM_PROT_READ | VM_PROT_WRITE);
}

TEST_F(JitViewTest, WatchedRangeIsReadOnly) {
    ASSERT_TRUE(m.map_region(kBase, kHostPage, MemoryProtection::ReadWrite));
    m.linear_watch_range_retain(kBase, kHostPage);
    EXPECT_EQ(view_prot(m, kBase), VM_PROT_READ)
        << "a lean store into a write-watched page would not be recorded";
    m.linear_watch_range_release(kBase, kHostPage);
    EXPECT_EQ(view_prot(m, kBase), VM_PROT_READ | VM_PROT_WRITE);
}

TEST_F(JitViewTest, DecommitDropKeepsViewsCoherent) {
    // MEM_RELEASE drops the host view's pages with a fresh MAP_FIXED mapping.
    // Unless the JIT view is re-aliased, it keeps the OLD pages: the JIT would
    // read stale data and its writes would vanish from the host's sight.
    ASSERT_TRUE(m.map_region(kBase, kHostPage, MemoryProtection::ReadWrite));
    std::memset(host(kBase), 0x5A, kHostPage);
    ASSERT_TRUE(m.unmap_region(kBase, kHostPage).is_ok());
    ASSERT_TRUE(m.map_region(kBase, kHostPage, MemoryProtection::ReadWrite));
    EXPECT_EQ(host(kBase)[0], 0u);
    EXPECT_EQ(view(kBase)[0], 0u) << "the JIT view still maps the released pages";
    host(kBase)[0] = 0x77;
    EXPECT_EQ(view(kBase)[0], 0x77u);
    EXPECT_TRUE(m.linear_jit_view_coherent(kBase, kBase + kHostPage));
}

TEST_F(JitViewTest, SharedMemoryRemapIsVisibleInView) {
    const usize sz = 2 * kHostPage;
    void* shared = mmap(nullptr, sz, PROT_READ | PROT_WRITE,
                        MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    ASSERT_NE(shared, MAP_FAILED);
    std::memset(shared, 0xAB, sz);
    ASSERT_TRUE(m.map_region_backed(kBase, sz, shared, MemoryProtection::ReadWrite));
    EXPECT_EQ(view(kBase)[0x10], 0xABu);
    static_cast<u8*>(shared)[0x20] = 0xCD;
    EXPECT_EQ(view(kBase)[0x20], 0xCDu) << "a peer's write is not visible to the JIT";
    view(kBase)[0x30] = 0xEF;
    EXPECT_EQ(static_cast<u8*>(shared)[0x30], 0xEFu);
    EXPECT_TRUE(m.linear_jit_view_coherent(kBase, kBase + sz));
    ASSERT_TRUE(m.unmap_region(kBase, sz).is_ok());
    munmap(shared, sz);
}

TEST_F(JitViewTest, PrivateFileBackingIsVisibleInViewAndStaysPrivate) {
    // SectionMapper backs SEC_IMAGE pages with a MAP_PRIVATE file mapping.
    // Both views must see the file bytes and each other's copy-on-write
    // stores, the file itself must never change, and unmapping must hand the
    // reservation back as zero pages.
    const usize sz = 2 * kHostPage;
    char path[] = "/tmp/vitreo_jit_view_fileXXXXXX";
    const int fd = mkstemp(path);
    ASSERT_GE(fd, 0);
    std::vector<u8> bytes(sz, 0x5A);
    ASSERT_EQ(pwrite(fd, bytes.data(), sz, 0), static_cast<ssize_t>(sz));
    ASSERT_TRUE(m.map_region(kBase, sz, MemoryProtection::ReadWrite));
    ASSERT_TRUE(m.back_region_with_file(kBase, sz, fd, 0));
    EXPECT_EQ(host(kBase)[0x10], 0x5Au);
    EXPECT_EQ(view(kBase + kHostPage)[0x10], 0x5Au);
    host(kBase)[0x20] = 0xCD;
    EXPECT_EQ(view(kBase)[0x20], 0xCDu) << "a host-view COW store split the views";
    view(kBase)[0x30] = 0xEF;
    EXPECT_EQ(host(kBase)[0x30], 0xEFu) << "a JIT-view COW store split the views";
    EXPECT_TRUE(m.linear_jit_view_coherent(kBase, kBase + sz));
    u8 on_disk = 0;
    ASSERT_EQ(pread(fd, &on_disk, 1, 0x20), 1);
    EXPECT_EQ(on_disk, 0x5Au) << "a MAP_PRIVATE store reached the file";
    ASSERT_TRUE(m.unmap_region(kBase, sz).is_ok());
    ASSERT_TRUE(m.map_region(kBase, sz, MemoryProtection::ReadWrite));
    EXPECT_EQ(host(kBase)[0x10], 0u) << "released file pages leaked into a new commit";
    EXPECT_EQ(view(kBase)[0x10], 0u);
    ASSERT_TRUE(m.unmap_region(kBase, sz).is_ok());
    close(fd);
    unlink(path);
}

TEST_F(JitViewTest, FileBackingRefusesUnalignedOrUncommittedRanges) {
    char path[] = "/tmp/vitreo_jit_view_fileXXXXXX";
    const int fd = mkstemp(path);
    ASSERT_GE(fd, 0);
    ASSERT_EQ(ftruncate(fd, 4 * kHostPage), 0);
    EXPECT_FALSE(m.back_region_with_file(kBase, kHostPage, fd, 0))
        << "no region is mapped there";
    ASSERT_TRUE(m.map_region(kBase, 2 * kHostPage, MemoryProtection::ReadWrite));
    EXPECT_FALSE(m.back_region_with_file(kBase + 0x1000, kHostPage, fd, 0));
    EXPECT_FALSE(m.back_region_with_file(kBase, kHostPage, fd, 0x1000));
    EXPECT_FALSE(m.back_region_with_file(kBase, 3 * kHostPage, fd, 0))
        << "the range runs past its region";
    ASSERT_TRUE(m.unmap_region(kBase, 2 * kHostPage).is_ok());
    ASSERT_TRUE(m.map_region(kBase, kHostPage, MemoryProtection::ReadWrite,
                             /*committed=*/false));
    EXPECT_FALSE(m.back_region_with_file(kBase, kHostPage, fd, 0))
        << "a reservation has no committed pages to back";
    ASSERT_TRUE(m.unmap_region(kBase, kHostPage).is_ok());
    close(fd);
    unlink(path);
}

TEST_F(JitViewTest, CowSnapshotOfTheHostViewKeepsViewsAliased) {
    // remap_detached_image.cpp takes a copy=TRUE (copy-on-write) snapshot of a
    // mapped image straight out of the host view. A later write through either
    // view must still reach the other -- the snapshot, not the JIT view, is
    // what diverges.
    ASSERT_TRUE(m.map_region(kBase, kHostPage, MemoryProtection::ReadWrite));
    host(kBase)[0] = 0x41;
    vm_address_t snap = 0;
    vm_prot_t cur = VM_PROT_NONE, max = VM_PROT_NONE;
    ASSERT_EQ(vm_remap(mach_task_self(), &snap, kHostPage, 0, VM_FLAGS_ANYWHERE,
                       mach_task_self(),
                       reinterpret_cast<vm_address_t>(host(kBase)),
                       /*copy=*/TRUE, &cur, &max, VM_INHERIT_NONE),
              KERN_SUCCESS);
    host(kBase)[0] = 0x42;
    EXPECT_EQ(view(kBase)[0], 0x42u) << "the COW snapshot split the two views";
    view(kBase)[1] = 0x43;
    EXPECT_EQ(host(kBase)[1], 0x43u);
    EXPECT_EQ(reinterpret_cast<const u8*>(snap)[0], 0x41u)
        << "the snapshot must keep the bytes it was taken with";
    EXPECT_TRUE(m.linear_jit_view_coherent(kBase, kBase + kHostPage));
    (void)vm_deallocate(mach_task_self(), snap, kHostPage);
}

TEST(JitViewLifecycle, EnableRefusedWhenPreconditionsFail) {
    {
        GuestMemory big;  // a window too large for two views
        ASSERT_TRUE(big.enable_linear_reservation(usize{1} << 46));
        ASSERT_TRUE(big.enable_linear_hardware_protection());
        EXPECT_FALSE(big.enable_linear_jit_view());
        EXPECT_EQ(big.linear_jit_view_base(), nullptr);
    }
    {
        GuestMemory soft;  // no hardware enforcement to mirror
        ASSERT_TRUE(soft.enable_linear_reservation(kReservation));
        EXPECT_FALSE(soft.enable_linear_jit_view());
    }
    {
        GuestMemory late;  // must precede any mapping
        ASSERT_TRUE(late.enable_linear_reservation(kReservation));
        ASSERT_TRUE(late.enable_linear_hardware_protection());
        ASSERT_TRUE(late.map_region(kBase, kHostPage, MemoryProtection::ReadWrite));
        EXPECT_FALSE(late.enable_linear_jit_view());
    }
    {
        GuestMemory twice;
        ASSERT_TRUE(twice.enable_linear_reservation(kReservation));
        ASSERT_TRUE(twice.enable_linear_hardware_protection());
        ASSERT_TRUE(twice.enable_linear_jit_view());
        EXPECT_FALSE(twice.enable_linear_jit_view());
    }
}

TEST_F(JitViewTest, JitViewStaysReadOnlyWhenCodeRetainRacesReconcile) {
    // End-state check under real concurrency: whatever order a reconcile and
    // a code-page retain interleave in, the JIT view ends read-only over the
    // translated code. Each trial stops reconciling as soon as the retain
    // returns, so the last reconcile is one that overlapped it.
    //
    // What this CANNOT force: the one fatal interleaving, a reconcile
    // PREEMPTED between reading the check byte and its mprotect while the
    // retain completes entirely. That window is sub-microsecond, so the
    // m_jit_view_mutex the reconcile holds is justified by that ordering
    // argument, not by this test (removing it still passes here).
    ASSERT_TRUE(m.map_region(kBase, kHostPage, MemoryProtection::ReadWrite));
    int writable_after_retain = 0;
    for (int trial = 0; trial < 400; ++trial) {
        std::atomic<bool> started{false};
        std::atomic<bool> stop{false};
        std::thread protector([&] {
            // Alternate, so every call really reconciles (an unchanged
            // protection may not), and end each iteration on RW: the state a
            // stale decision would leave over translated code.
            while (!stop.load(std::memory_order_acquire)) {
                (void)m.protect_region(kBase, kHostPage, MemoryProtection::Read);
                (void)m.protect_region(kBase, kHostPage,
                                       MemoryProtection::ReadWrite);
                started.store(true, std::memory_order_release);
            }
        });
        while (!started.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        m.retain_jit_code_page(kBase);
        stop.store(true, std::memory_order_release);
        protector.join();
        if (view_prot(m, kBase) != VM_PROT_READ) ++writable_after_retain;
        m.release_jit_code_page(kBase);
    }
    EXPECT_EQ(writable_after_retain, 0)
        << "a reconcile raced the code-page retain and left the JIT view "
           "writable over translated code";
}

// Many threads retaining and releasing guest code pages that share host pages
// (the four 4 KiB pages of one 16 KiB host page share one check bit and one
// JIT-view protection). While any retain is outstanding on a host page, its
// view must stay read-only and its check bit set; once the last one is gone,
// the view must be writable again and the bit clear. Run at a low (< 4 GiB)
// and a high base, which keep their counts in different structures.
TEST_F(JitViewTest, ConcurrentCodePageRetainsKeepProtectionExact) {
    constexpr usize kHostPages = 8;
    constexpr usize kGuestPages = kHostPages * (kHostPage / 0x1000);
    constexpr u32 kThreads = 8;
    constexpr u32 kIterations = 4000;
    for (const GuestAddr base : {GuestAddr{0x4000'0000}, kBase}) {
        SCOPED_TRACE(::testing::Message() << "base 0x" << std::hex << base);
        ASSERT_TRUE(m.map_region(base, kHostPages * kHostPage,
                                 MemoryProtection::ReadWrite));
        const auto guest_page = [&](usize i) { return base + i * 0x1000; };
        const auto check_byte = [&](usize host) {
            return __atomic_load_n(
                &m.linear_check_page_map()[(base >> kLinearHostPageShift) + host],
                __ATOMIC_ACQUIRE);
        };
        // Pins on guest page 1 of host page 0 and guest page 6 of host page 1,
        // held for the whole churn: those two host pages must never become
        // writable, whatever the churn on their other guest pages does.
        m.retain_jit_code_page(guest_page(1));
        m.retain_jit_code_page(guest_page(6));

        std::atomic<bool> stop{false};
        std::atomic<int> writable_while_pinned{0};
        std::thread checker([&] {
            while (!stop.load(std::memory_order_acquire)) {
                for (usize host : {usize{0}, usize{1}}) {
                    if (view_prot(m, base + host * kHostPage) != VM_PROT_READ ||
                        (check_byte(host) & kLinearCheckHoldsCode) == 0) {
                        writable_while_pinned.fetch_add(1);
                    }
                }
            }
        });
        std::vector<std::thread> churn;
        for (u32 t = 0; t < kThreads; ++t) {
            churn.emplace_back([&, t] {
                u32 x = 0x9E3779B9u * (t + 1);
                for (u32 i = 0; i < kIterations; ++i) {
                    x ^= x << 13;
                    x ^= x >> 17;
                    x ^= x << 5;
                    const GuestAddr a = guest_page(x % kGuestPages);
                    const GuestAddr b = guest_page((x >> 8) % kGuestPages);
                    m.retain_jit_code_page(a);
                    m.retain_jit_code_page(b);
                    m.release_jit_code_page(a);
                    m.release_jit_code_page(b);
                }
            });
        }
        for (auto& t : churn) t.join();
        stop.store(true, std::memory_order_release);
        checker.join();
        EXPECT_EQ(writable_while_pinned.load(), 0)
            << "a host page with a retained code page became writable in the "
               "JIT view (or lost its check bit) while another thread released "
               "a sibling page";

        for (usize host = 0; host < kHostPages; ++host) {
            const bool pinned = host < 2;
            EXPECT_EQ(view_prot(m, base + host * kHostPage),
                      pinned ? VM_PROT_READ : (VM_PROT_READ | VM_PROT_WRITE))
                << "host page " << host;
            EXPECT_EQ((check_byte(host) & kLinearCheckHoldsCode) != 0, pinned)
                << "host page " << host;
        }
        for (usize i = 0; i < kGuestPages; ++i) {
            EXPECT_EQ(m.jit_code_pages_overlap(guest_page(i), 1), i == 1 || i == 6)
                << "guest page " << i;
        }

        m.release_jit_code_page(guest_page(1));
        m.release_jit_code_page(guest_page(6));
        for (usize host = 0; host < kHostPages; ++host) {
            EXPECT_EQ(view_prot(m, base + host * kHostPage),
                      VM_PROT_READ | VM_PROT_WRITE)
                << "host page " << host << " stayed read-only after its last "
                   "code page was released";
            EXPECT_EQ(check_byte(host) & kLinearCheckHoldsCode, 0) << "host page " << host;
        }
        EXPECT_FALSE(m.jit_code_pages_overlap(base, kHostPages * kHostPage));
    }
}

}  // namespace
