// SPDX-License-Identifier: LGPL-2.1-only

/// @file test_x64_exception_bridge.cpp
/// @brief Mach exception bridge: guest faults become Compat32Exit::Fault.
///
/// Must run on Intel hardware. A POSIX signal cannot be delivered while guest
/// segments are loaded, so this is the only way a native 32-bit AV is
/// recoverable.

#include <gtest/gtest.h>

#include "core/cpu/x64/compat32.hpp"
#include "core/cpu/x64/x64_exception_bridge.hpp"

#include <cstring>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace vitreo;
using namespace vitreo::cpu::x64;

namespace {

constexpr u32 kTrampolineAddr = 0x38000000;
constexpr u32 kGuestCodeAddr  = 0x39000000;
constexpr u32 kGuestStackTop  = 0x3A000000;

struct FaultResult {
    u32 completed;
    u32 exit_reason;   // 0 sentinel, 1 thunk, 2 fault, 3 other
    u32 eip;
    u32 status;
    u32 address;
    u32 eax;
};

u8* map_fixed_rwx(u32 addr, usize size) {
    void* p = mmap(reinterpret_cast<void*>(static_cast<uintptr_t>(addr)),
                   size, PROT_READ | PROT_WRITE | PROT_EXEC,
                   MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
    return p == MAP_FAILED ? nullptr : static_cast<u8*>(p);
}

} // namespace

TEST(X64ExceptionBridgeTest, NullDerefSurfacesAsAccessViolation) {
    auto* r = static_cast<FaultResult*>(
        mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0));
    ASSERT_NE(r, MAP_FAILED);
    std::memset(r, 0, sizeof(FaultResult));

    const pid_t pid = fork();
    ASSERT_NE(pid, -1);
    if (pid == 0) {
        u8* text = map_fixed_rwx(kGuestCodeAddr, 0x1000);
        if (!text) _exit(20);
        if (!map_fixed_rwx(kGuestStackTop - 0x4000, 0x4000)) _exit(21);

        // 31 C0   xor eax, eax
        // 8B 00   mov eax, [eax]    ; null deref
        // C3      ret               ; not reached
        const u8 code[] = {0x31, 0xC0, 0x8B, 0x00, 0xC3};
        std::memcpy(text, code, sizeof(code));

        auto ex = Compat32Executor::create(kTrampolineAddr);
        if (ex.is_err()) _exit(22);
        Compat32Executor* exec = ex.value();

        if (X64ExceptionBridge::start().is_err()) _exit(23);
        X64ExceptionBridge::bind_executor(exec);
        if (X64ExceptionBridge::install_on_current_thread().is_err()) _exit(24);

        Compat32State st{};
        st.eip = kGuestCodeAddr;
        st.esp = kGuestStackTop - 0x100;
        const u32 sentinel = exec->sentinel_return_address();
        std::memcpy(reinterpret_cast<void*>(static_cast<uintptr_t>(st.esp)),
                    &sentinel, 4);
        st.eax = 0xDEAD;

        const Compat32Exit why = exec->run(st);
        r->exit_reason = static_cast<u32>(why);
        r->eip = st.eip;
        r->status = exec->last_fault_status();
        r->address = exec->last_fault_address();
        r->eax = st.eax;
        r->completed = 1;
        _exit(0);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    ASSERT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0)
        << "child died instead of reporting the guest fault";

    EXPECT_EQ(r->completed, 1u);
    EXPECT_EQ(r->exit_reason, static_cast<u32>(Compat32Exit::Fault));
    EXPECT_EQ(r->status, 0xC0000005u);
    // xor eax,eax is 2 bytes; the faulting mov is at entry+2.
    EXPECT_EQ(r->eip, kGuestCodeAddr + 2);
    EXPECT_EQ(r->address, 0u);
    EXPECT_EQ(r->eax, 0u) << "eax should be the guest's zero after xor";
    munmap(r, 4096);
}

TEST(X64ExceptionBridgeTest, SentinelReturnStillWorksWithBridgeInstalled) {
    // Installing the port must not change the no-fault path.
    auto* r = static_cast<FaultResult*>(
        mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0));
    ASSERT_NE(r, MAP_FAILED);
    std::memset(r, 0, sizeof(FaultResult));

    const pid_t pid = fork();
    ASSERT_NE(pid, -1);
    if (pid == 0) {
        u8* text = map_fixed_rwx(kGuestCodeAddr, 0x1000);
        if (!text) _exit(20);
        if (!map_fixed_rwx(kGuestStackTop - 0x4000, 0x4000)) _exit(21);
        const u8 code[] = {0x40, 0xC3};  // inc eax; ret
        std::memcpy(text, code, sizeof(code));

        auto ex = Compat32Executor::create(kTrampolineAddr);
        if (ex.is_err()) _exit(22);
        Compat32Executor* exec = ex.value();
        if (X64ExceptionBridge::start().is_err()) _exit(23);
        X64ExceptionBridge::bind_executor(exec);
        if (X64ExceptionBridge::install_on_current_thread().is_err()) _exit(24);

        Compat32State st{};
        st.eip = kGuestCodeAddr;
        st.esp = kGuestStackTop - 0x100;
        st.eax = 41;
        const u32 sentinel = exec->sentinel_return_address();
        std::memcpy(reinterpret_cast<void*>(static_cast<uintptr_t>(st.esp)),
                    &sentinel, 4);
        const Compat32Exit why = exec->run(st);
        r->exit_reason = static_cast<u32>(why);
        r->eax = st.eax;
        r->completed = 1;
        _exit(0);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    ASSERT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    EXPECT_EQ(r->exit_reason, static_cast<u32>(Compat32Exit::ReturnedToSentinel));
    EXPECT_EQ(r->eax, 42u);
    munmap(r, 4096);
}
