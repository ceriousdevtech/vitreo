// SPDX-License-Identifier: LGPL-2.1-only
//
// HostProtectionSnapshot: patching guest code must leave every page's
// protection exactly as it found it. The guest's VirtualAlloc/VirtualProtect
// protection is the contract -- a PAGE_EXECUTE_READWRITE code buffer stays
// writable, read+execute code stays unwritable -- and host-side arming
// (write-watch read-only, guard no-access) must survive too.

#include "core/cpu/x64/host_protection_snapshot.hpp"

#include <gtest/gtest.h>

#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cstdint>

namespace {

using vitreo::cpu::x64::HostProtectionSnapshot;

vm_prot_t protection_at(std::uintptr_t addr) {
    mach_vm_address_t region = addr;
    mach_vm_size_t size = 0;
    vm_region_basic_info_data_64_t info{};
    mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
    mach_port_t object = MACH_PORT_NULL;
    EXPECT_EQ(mach_vm_region(mach_task_self(), &region, &size,
                             VM_REGION_BASIC_INFO_64,
                             reinterpret_cast<vm_region_info_t>(&info), &count,
                             &object),
              KERN_SUCCESS);
    EXPECT_LE(region, addr);
    return info.protection;
}

class HostProtectionSnapshotTest : public ::testing::Test {
protected:
    void SetUp() override {
        page_ = static_cast<std::uintptr_t>(getpagesize());
        base_ = mmap(nullptr, 4 * page_, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        ASSERT_NE(base_, MAP_FAILED);
    }
    void TearDown() override { munmap(base_, 4 * page_); }

    std::uintptr_t page(int i) const {
        return reinterpret_cast<std::uintptr_t>(base_) + i * page_;
    }
    void protect(int i, int prot) {
        ASSERT_EQ(mprotect(reinterpret_cast<void*>(page(i)), page_, prot), 0);
    }

    void* base_ = nullptr;
    std::uintptr_t page_ = 0;
};

// A span mixing writable, read-only, no-access and read+write pages comes
// back page-for-page after being opened wholesale, as a code patcher does.
TEST_F(HostProtectionSnapshotTest, RestoresMixedSpanExactly) {
    protect(0, PROT_READ | PROT_WRITE);   // writable buffer (RWX in the guest)
    protect(1, PROT_READ);                // read-only code or armed write-watch
    protect(2, PROT_NONE);                // guard / uncommitted
    protect(3, PROT_READ | PROT_WRITE);

    const HostProtectionSnapshot snapshot(page(0), page(4));
    ASSERT_EQ(mprotect(base_, 4 * page_, PROT_READ | PROT_WRITE), 0);
    snapshot.restore();

    EXPECT_EQ(protection_at(page(0)), VM_PROT_READ | VM_PROT_WRITE);
    EXPECT_EQ(protection_at(page(1)), VM_PROT_READ);
    EXPECT_EQ(protection_at(page(2)), VM_PROT_NONE);
    EXPECT_EQ(protection_at(page(3)), VM_PROT_READ | VM_PROT_WRITE);
}

// The regression: a writable page must not come back read-only. A blanket
// read+execute restore made a DLL's RWX code buffer fault on its next write.
TEST_F(HostProtectionSnapshotTest, WritablePageStaysWritable) {
    const HostProtectionSnapshot snapshot(page(0), page(1));
    ASSERT_EQ(mprotect(base_, page_, PROT_READ), 0);
    snapshot.restore();
    EXPECT_EQ(protection_at(page(0)), VM_PROT_READ | VM_PROT_WRITE);
    *reinterpret_cast<volatile int*>(page(0)) = 42;  // must not fault
}

// A span that starts or ends inside a larger region records only its own
// pages, so restoring it never touches the neighbours.
TEST_F(HostProtectionSnapshotTest, ClipsToTheRequestedSpan) {
    const HostProtectionSnapshot snapshot(page(1), page(2));
    ASSERT_EQ(mprotect(base_, 4 * page_, PROT_READ), 0);
    snapshot.restore();
    EXPECT_EQ(protection_at(page(0)), VM_PROT_READ);
    EXPECT_EQ(protection_at(page(1)), VM_PROT_READ | VM_PROT_WRITE);
    EXPECT_EQ(protection_at(page(2)), VM_PROT_READ);
}

}  // namespace
