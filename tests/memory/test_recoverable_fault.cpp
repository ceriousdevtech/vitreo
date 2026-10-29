// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
//
// A linear guest→host mapping replaces per-access software bounds checks with
// hardware protection: a bad guest access becomes a real SIGSEGV/SIGBUS inside
// a JIT block, and a *recoverable* handler must turn it into a controlled guest
// access-violation exit rather than a crash. This test proves that mechanism in
// isolation, using exactly the pieces the real handler will use:
//
//   - a MAP_JIT executable stub shaped like a JIT block (STR X30,[SP,#-16]!
//     prologue; LDR X30,[SP],#16 / RET epilogue),
//   - a load through a PROT_NONE page (the "unmapped guest page"),
//   - a SA_SIGINFO handler that recognizes the fault address, records it, and
//     redirects uc_mcontext->__ss.__pc to a trampoline that unwinds the 16-byte
//     frame and returns a "raised" result to the caller.
//
// If this works and repeats, the block-restart recovery the design depends on
// is real. If it doesn't, the whole project stops here.

#include <gtest/gtest.h>

#include <csignal>
#include <cstring>
#include <cstdint>
#include <sys/mman.h>
#include <pthread.h>
#include <libkern/OSCacheControl.h>

#include <vitreo/common.hpp>

using namespace vitreo;

namespace {

// The stub is called as: int stub(void* addr).  Returns 0 if the load at addr
// succeeded, 1 if it faulted and was recovered via the trampoline.
using StubFn = int (*)(void*);

constexpr usize kPageSize = 4096;

// Shared state the recoverable handler communicates through. Process-global
// because signal handlers are; guarded by only ever having one test touch the
// fault at a time.
u8*  g_bad_page = nullptr;        // PROT_NONE region — the "unmapped" page
void* g_trampoline = nullptr;      // where the handler resumes execution
volatile sig_atomic_t g_fault_seen = 0;
volatile uintptr_t    g_fault_addr = 0;

struct sigaction g_old_segv{};
struct sigaction g_old_bus{};

void recoverable_handler(int sig, siginfo_t* info, void* uc_raw) {
    const uintptr_t fault = reinterpret_cast<uintptr_t>(info->si_addr);
    const uintptr_t lo = reinterpret_cast<uintptr_t>(g_bad_page);
    if (g_bad_page && fault >= lo && fault < lo + kPageSize && g_trampoline) {
        g_fault_seen = 1;
        g_fault_addr = fault;
        // Redirect the faulting thread to the trampoline. On macOS ARM64,
        // mutating __ss.__pc and returning resumes there via sigreturn. X19/X20,
        // SP and LR are untouched, so the trampoline can undo the prologue.
        auto* uc = static_cast<ucontext_t*>(uc_raw);
        uc->uc_mcontext->__ss.__pc = reinterpret_cast<uintptr_t>(g_trampoline);
        return;
    }
    // Not our fault — restore the previous disposition and let it re-raise so a
    // genuine bug still crashes loudly rather than being swallowed.
    sigaction(SIGSEGV, &g_old_segv, nullptr);
    sigaction(SIGBUS, &g_old_bus, nullptr);
    (void)sig;
}

class RecoverableFaultTest : public ::testing::Test {
protected:
    void SetUp() override {
        // PROT_NONE "unmapped guest page".
        g_bad_page = static_cast<u8*>(mmap(nullptr, kPageSize, PROT_NONE,
                                           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
        ASSERT_NE(g_bad_page, MAP_FAILED);

        // A normal RW page for the success path.
        m_good_page = static_cast<u8*>(mmap(nullptr, kPageSize,
                                            PROT_READ | PROT_WRITE,
                                            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
        ASSERT_NE(m_good_page, MAP_FAILED);
        m_good_page[0] = 0x42;

        // Executable stub in a MAP_JIT page.
        m_stub_mem = mmap(nullptr, kPageSize, PROT_READ | PROT_WRITE | PROT_EXEC,
                          MAP_PRIVATE | MAP_ANONYMOUS | MAP_JIT, -1, 0);
        ASSERT_NE(m_stub_mem, MAP_FAILED);

        // Block-shaped stub. Word offsets:
        //   0: STR X30,[SP,#-16]!      prologue (save LR)
        //   1: LDR W1,[X0]             may fault
        //   2: MOVZ W0,#0              ok: result = 0
        //   3: LDR X30,[SP],#16        \ ok epilogue
        //   4: RET                     /
        //   5: MOVZ W0,#1              trampoline: result = 1  <-- handler lands here
        //   6: LDR X30,[SP],#16        \ trampoline epilogue (undo prologue)
        //   7: RET                     /
        static const uint32_t code[] = {
            0xF81F0FFEu,  // STR X30, [SP, #-16]!
            0xB9400001u,  // LDR W1, [X0]
            0x52800000u,  // MOVZ W0, #0
            0xF84107FEu,  // LDR X30, [SP], #16
            0xD65F03C0u,  // RET
            0x52800020u,  // MOVZ W0, #1
            0xF84107FEu,  // LDR X30, [SP], #16
            0xD65F03C0u,  // RET
        };
        pthread_jit_write_protect_np(0);          // make writable
        std::memcpy(m_stub_mem, code, sizeof(code));
        pthread_jit_write_protect_np(1);          // back to executable
        sys_icache_invalidate(m_stub_mem, sizeof(code));

        g_trampoline = static_cast<u8*>(m_stub_mem) + 5 * 4;  // word 5
        m_stub = reinterpret_cast<StubFn>(m_stub_mem);

        g_fault_seen = 0;
        g_fault_addr = 0;

        struct sigaction sa{};
        sa.sa_sigaction = recoverable_handler;
        sa.sa_flags = SA_SIGINFO;
        sigemptyset(&sa.sa_mask);
        ASSERT_EQ(sigaction(SIGSEGV, &sa, &g_old_segv), 0);
        ASSERT_EQ(sigaction(SIGBUS, &sa, &g_old_bus), 0);
    }

    void TearDown() override {
        sigaction(SIGSEGV, &g_old_segv, nullptr);
        sigaction(SIGBUS, &g_old_bus, nullptr);
        if (g_bad_page && g_bad_page != MAP_FAILED) munmap(g_bad_page, kPageSize);
        if (m_good_page && m_good_page != MAP_FAILED) munmap(m_good_page, kPageSize);
        if (m_stub_mem && m_stub_mem != MAP_FAILED) munmap(m_stub_mem, kPageSize);
        g_bad_page = nullptr;
        g_trampoline = nullptr;
    }

    u8* m_good_page = nullptr;
    void* m_stub_mem = nullptr;
    StubFn m_stub = nullptr;
};

// A valid access takes the ok path and returns 0, no fault.
TEST_F(RecoverableFaultTest, ValidAccessReturnsZero) {
    const int r = m_stub(m_good_page);
    EXPECT_EQ(r, 0);
    EXPECT_EQ(g_fault_seen, 0);
}

// The crux: a load through a PROT_NONE page faults in hardware, the handler
// redirects to the trampoline, and the stub returns the "raised" result to its
// caller — no crash. Proves recoverable block-restart.
TEST_F(RecoverableFaultTest, FaultRecoversToTrampoline) {
    const int r = m_stub(g_bad_page);
    EXPECT_EQ(r, 1) << "stub did not return via the trampoline";
    EXPECT_EQ(g_fault_seen, 1) << "handler never saw the fault";
    EXPECT_EQ(g_fault_addr, reinterpret_cast<uintptr_t>(g_bad_page))
        << "fault address did not match the bad page";
}

// Recovery must be repeatable and must not corrupt normal control flow: a fault
// followed by a valid call must both behave correctly.
TEST_F(RecoverableFaultTest, RecoveryIsRepeatable) {
    EXPECT_EQ(m_stub(g_bad_page), 1);
    EXPECT_EQ(g_fault_seen, 1);
    g_fault_seen = 0;
    EXPECT_EQ(m_stub(m_good_page), 0);
    EXPECT_EQ(g_fault_seen, 0);
    EXPECT_EQ(m_stub(g_bad_page), 1);
    EXPECT_EQ(g_fault_seen, 1);
}

// The fault address the handler recovers is the exact guest-page byte touched —
// this is what a real handler turns into guest_addr = fault - linear_base.
TEST_F(RecoverableFaultTest, FaultAddressIsPrecise) {
    // Point the stub at an offset within the bad page.
    const int r = m_stub(g_bad_page + 0x40);
    EXPECT_EQ(r, 1);
    EXPECT_EQ(g_fault_addr, reinterpret_cast<uintptr_t>(g_bad_page) + 0x40);
}

}  // namespace
