// SPDX-License-Identifier: LGPL-2.1-only
// Stage 0 spike: GuestMemory::map_region_backed binds an externally-owned host
// buffer into the guest arena (no copy, no ownership) so cross-process shared
// memory works. Proves: (1) borrowed binding shares writes both directions,
// (2) destroying GuestMemory does NOT free the borrowed buffer, (3) a real
// POSIX shm region is visible across fork()ed processes through GuestMemory.

#include <gtest/gtest.h>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#include <cstring>
#include <string>

#include "core/memory/guest_memory.hpp"

using namespace vitreo;

TEST(Stage0BackedMemory, BorrowedBindingSharesBothWays) {
    const usize kSize = 4096;
    void* host = mmap(nullptr, kSize, PROT_READ | PROT_WRITE,
                      MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    ASSERT_NE(host, MAP_FAILED);

    constexpr GuestAddr kVa = 0x20000;
    GuestMemory mem_a;
    GuestMemory mem_b;
    ASSERT_TRUE(mem_a.map_region_backed(kVa, kSize, host, MemoryProtection::ReadWrite));
    ASSERT_TRUE(mem_b.map_region_backed(kVa, kSize, host, MemoryProtection::ReadWrite));

    // Write through A, read through B and via the raw host pointer.
    ASSERT_TRUE(mem_a.write_u32(kVa + 0x40, 0xCAFEBABEu));
    auto rb = mem_b.read_u32(kVa + 0x40);
    ASSERT_TRUE(rb.is_ok());
    EXPECT_EQ(rb.value(), 0xCAFEBABEu);
    EXPECT_EQ(*reinterpret_cast<u32*>(static_cast<u8*>(host) + 0x40), 0xCAFEBABEu);

    // Write through B, read through A (no copy — same backing).
    ASSERT_TRUE(mem_b.write_u32(kVa + 0x80, 0x12345678u));
    auto ra = mem_a.read_u32(kVa + 0x80);
    ASSERT_TRUE(ra.is_ok());
    EXPECT_EQ(ra.value(), 0x12345678u);

    munmap(host, kSize);
}

TEST(Stage0BackedMemory, DestroyDoesNotFreeBorrowedBuffer) {
    const usize kSize = 4096;
    void* host = mmap(nullptr, kSize, PROT_READ | PROT_WRITE,
                      MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    ASSERT_NE(host, MAP_FAILED);
    constexpr GuestAddr kVa = 0x30000;

    {
        GuestMemory mem;
        ASSERT_TRUE(mem.map_region_backed(kVa, kSize, host, MemoryProtection::ReadWrite));
        ASSERT_TRUE(mem.write_u32(kVa, 0xDEADBEEFu));
        ASSERT_TRUE(mem.unmap_region(kVa, kSize));
    }  // GuestMemory destroyed; borrowed buffer must remain valid.

    // If the borrowed region had been munmap'd, this would SIGSEGV. It must
    // still hold our value.
    EXPECT_EQ(*reinterpret_cast<u32*>(host), 0xDEADBEEFu);
    munmap(host, kSize);  // we own it
}

TEST(Stage0BackedMemory, CrossProcessSharedViaForkAndShm) {
    const usize kSize = 4096;
    const std::string name = "/vitreo_stage0_test_" + std::to_string(::getpid());
    shm_unlink(name.c_str());  // clean any stale
    int fd = shm_open(name.c_str(), O_CREAT | O_RDWR, 0600);
    ASSERT_GE(fd, 0);
    ASSERT_EQ(ftruncate(fd, static_cast<off_t>(kSize)), 0);

    constexpr GuestAddr kVa = 0x40000;
    constexpr u32 kParentMagic = 0xA1B2C3D4u;
    constexpr u32 kChildMagic  = 0x5E6F7081u;

    void* parent_map = mmap(nullptr, kSize, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    ASSERT_NE(parent_map, MAP_FAILED);
    GuestMemory pmem;
    ASSERT_TRUE(pmem.map_region_backed(kVa, kSize, parent_map, MemoryProtection::ReadWrite));
    ASSERT_TRUE(pmem.write_u32(kVa + 0x10, kParentMagic));

    pid_t pid = fork();
    ASSERT_GE(pid, 0);
    if (pid == 0) {
        // Child: open the SAME shm in a fresh GuestMemory and verify visibility.
        int cfd = shm_open(name.c_str(), O_RDWR, 0600);
        if (cfd < 0) _exit(10);
        void* cmap = mmap(nullptr, kSize, PROT_READ | PROT_WRITE, MAP_SHARED, cfd, 0);
        if (cmap == MAP_FAILED) _exit(11);
        GuestMemory cmem;
        if (!cmem.map_region_backed(kVa, kSize, cmap, MemoryProtection::ReadWrite)) _exit(12);
        auto v = cmem.read_u32(kVa + 0x10);
        if (!v.is_ok() || v.value() != kParentMagic) _exit(13);  // parent's write not seen
        if (!cmem.write_u32(kVa + 0x20, kChildMagic)) _exit(14);  // write back for parent
        _exit(0);
    }

    int status = 0;
    ASSERT_EQ(waitpid(pid, &status, 0), pid);
    EXPECT_TRUE(WIFEXITED(status));
    EXPECT_EQ(WEXITSTATUS(status), 0) << "child failed at step " << WEXITSTATUS(status);

    // Parent sees the child's write through the shared region.
    auto cv = pmem.read_u32(kVa + 0x20);
    ASSERT_TRUE(cv.is_ok());
    EXPECT_EQ(cv.value(), kChildMagic);

    munmap(parent_map, kSize);
    close(fd);
    shm_unlink(name.c_str());
}
