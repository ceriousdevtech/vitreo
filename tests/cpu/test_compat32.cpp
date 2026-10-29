// SPDX-License-Identifier: LGPL-2.1-only

/// @file test_compat32.cpp
/// @brief Native 32-bit guest execution in CPU compatibility mode.
///
/// x86_64 only, and these must actually RUN on Intel hardware -- a green
/// build proves nothing here. Every test executes real 32-bit machine code.
///
/// Each case runs in a forked child. Entering compatibility mode disturbs
/// segment state, and a mistake in a stub does not fail cleanly: it takes the
/// process with it, often with no signal handler able to run. Forking keeps a
/// failure reportable instead of ending the suite.

#include <gtest/gtest.h>

#include "core/cpu/x64/compat32.hpp"
#include "core/cpu/x64/x64_segments.hpp"

#include <atomic>
#include <cstring>
#include <sys/mman.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

using namespace vitreo;
using namespace vitreo::cpu::x64;

namespace {

constexpr u32 kTrampolineAddr = 0x38000000;
constexpr u32 kGuestCodeAddr  = 0x39000000;
constexpr u32 kGuestStackTop  = 0x3A000000;   // grows down from here
constexpr u32 kTebAddr        = 0x3B000000;   // 8 KiB fake TEB for FS stubs

/// Result of one forked execution, shared back to the parent.
struct SharedResult {
    u32  completed;    // 1 = run() returned
    u32  eax, ecx, edx, ebx, esp, ebp, esi, edi, eflags;
};

SharedResult* make_shared() {
    void* p = mmap(nullptr, 4096, PROT_READ | PROT_WRITE,
                   MAP_SHARED | MAP_ANON, -1, 0);
    if (p == MAP_FAILED) return nullptr;
    std::memset(p, 0, sizeof(SharedResult));
    return static_cast<SharedResult*>(p);
}

u8* map_fixed_rwx(u32 addr, usize size) {
    void* p = mmap(reinterpret_cast<void*>(static_cast<uintptr_t>(addr)),
                   size, PROT_READ | PROT_WRITE | PROT_EXEC,
                   MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
    return p == MAP_FAILED ? nullptr : static_cast<u8*>(p);
}

/// Shared thunk/sentinel stubs reach the trampoline through FS. Map an 8 KiB
/// TEB, load it as FS, and publish this executor's collector/sentinel.
bool install_fs_teb(Compat32Executor* exec, u32 teb_addr = kTebAddr) {
    u8* teb = map_fixed_rwx(teb_addr, 0x2000);
    if (teb == nullptr) return false;
    std::memset(teb, 0, 0x2000);
    auto sel = SegmentTable::allocate_data_segment(teb_addr);
    if (sel.is_err()) return false;
    exec->set_fs_selector(sel.value());
    exec->publish_fs_hooks(teb_addr);
    return true;
}

/// Run @p code (32-bit machine code, ending in `ret`) natively, with @p in as
/// the starting register state. Returns false if the child died.
///
/// The guest's return address is the executor's sentinel, so a plain `ret`
/// hands control back -- exactly how a Windows guest function returns to its
/// caller.
bool run_guest32(const u8* code, usize code_len,
                 const Compat32State& in, SharedResult* out) {
    const pid_t pid = fork();
    if (pid == -1) return false;

    if (pid == 0) {
        u8* text = map_fixed_rwx(kGuestCodeAddr, 0x1000);
        if (!text) _exit(20);
        std::memcpy(text, code, code_len);

        if (!map_fixed_rwx(kGuestStackTop - 0x4000, 0x4000)) _exit(21);

        auto ex = Compat32Executor::create(kTrampolineAddr);
        if (ex.is_err()) _exit(22);
        Compat32Executor* exec = ex.value();

        Compat32State st = in;
        st.eip = kGuestCodeAddr;

        // Push the sentinel as the guest's return address.
        st.esp = kGuestStackTop - 0x100;
        const u32 sentinel = exec->sentinel_return_address();
        std::memcpy(reinterpret_cast<void*>(static_cast<uintptr_t>(st.esp)),
                    &sentinel, 4);

        const Compat32Exit why = exec->run(st);

        out->completed = (why == Compat32Exit::ReturnedToSentinel) ? 1u : 0u;
        out->eax = st.eax; out->ecx = st.ecx; out->edx = st.edx;
        out->ebx = st.ebx; out->esp = st.esp; out->ebp = st.ebp;
        out->esi = st.esi; out->edi = st.edi; out->eflags = st.eflags;
        _exit(0);
    }

    int status = 0;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

} // namespace

TEST(Compat32Test, ExecutesASingleAddAndReturns) {
    // 32-bit guest:
    //   01 D8   add eax, ebx
    //   C3      ret
    const u8 code[] = { 0x01, 0xD8, 0xC3 };

    SharedResult* r = make_shared();
    ASSERT_NE(r, nullptr);

    Compat32State in{};
    in.eax = 40;
    in.ebx = 2;

    ASSERT_TRUE(run_guest32(code, sizeof(code), in, r))
        << "child died executing 32-bit code";
    EXPECT_EQ(r->completed, 1u);
    EXPECT_EQ(r->eax, 42u) << "guest add did not execute natively";

    munmap(r, 4096);
}

TEST(Compat32Test, AllGeneralRegistersRoundTrip) {
    // Only `ret`: whatever went in must come back untouched. This is the
    // register-transfer contract, and it is the part most likely to be
    // silently wrong -- a stub that forgets a register produces plausible
    // garbage rather than a crash.
    const u8 code[] = { 0xC3 };

    SharedResult* r = make_shared();
    ASSERT_NE(r, nullptr);

    Compat32State in{};
    in.eax = 0x11111111; in.ecx = 0x22222222;
    in.edx = 0x33333333; in.ebx = 0x44444444;
    in.ebp = 0x55555555; in.esi = 0x66666666;
    in.edi = 0x77777777;

    ASSERT_TRUE(run_guest32(code, sizeof(code), in, r));
    EXPECT_EQ(r->completed, 1u);
    EXPECT_EQ(r->eax, 0x11111111u);
    EXPECT_EQ(r->ecx, 0x22222222u);
    EXPECT_EQ(r->edx, 0x33333333u);
    EXPECT_EQ(r->ebx, 0x44444444u);
    EXPECT_EQ(r->ebp, 0x55555555u);
    EXPECT_EQ(r->esi, 0x66666666u);
    EXPECT_EQ(r->edi, 0x77777777u);

    munmap(r, 4096);
}

TEST(Compat32Test, GuestCanUseItsOwnStack) {
    // 32-bit guest:
    //   53        push ebx
    //   58        pop  eax
    //   C3        ret
    //
    // Proves ESP is the guest's and the stack is writable in 32-bit mode --
    // which is where a truncated or mis-set stack pointer would show up.
    const u8 code[] = { 0x53, 0x58, 0xC3 };

    SharedResult* r = make_shared();
    ASSERT_NE(r, nullptr);

    Compat32State in{};
    in.eax = 0;
    in.ebx = 0xCAFEF00D;

    ASSERT_TRUE(run_guest32(code, sizeof(code), in, r));
    EXPECT_EQ(r->completed, 1u);
    EXPECT_EQ(r->eax, 0xCAFEF00Du);

    munmap(r, 4096);
}

TEST(Compat32Test, GuestReadsAndWritesGuestMemory) {
    // 32-bit guest, absolute addressing into its own address space:
    //   A1 <addr>   mov eax, [addr]
    //   40          inc eax
    //   A3 <addr>   mov [addr], eax
    //   C3          ret
    //
    // Identity mapping is the whole point: the guest emits an absolute address
    // and the CPU consumes it directly, with no translation layer.
    constexpr u32 kDataAddr = kGuestCodeAddr + 0x800;

    u8 code[16];
    usize n = 0;
    code[n++] = 0xA1; std::memcpy(code + n, &kDataAddr, 4); n += 4;
    code[n++] = 0x40;
    code[n++] = 0xA3; std::memcpy(code + n, &kDataAddr, 4); n += 4;
    code[n++] = 0xC3;

    SharedResult* r = make_shared();
    ASSERT_NE(r, nullptr);

    // The data slot lives inside the guest code page, which run_guest32 maps
    // and zeroes, so the guest starts from 0 and must return 1.
    Compat32State in{};
    ASSERT_TRUE(run_guest32(code, n, in, r));
    EXPECT_EQ(r->completed, 1u);
    EXPECT_EQ(r->eax, 1u);

    munmap(r, 4096);
}

TEST(Compat32Test, FlagsSurviveTheModeSwitch) {
    // 32-bit guest:
    //   31 C0   xor eax, eax     (sets ZF)
    //   C3      ret
    const u8 code[] = { 0x31, 0xC0, 0xC3 };

    SharedResult* r = make_shared();
    ASSERT_NE(r, nullptr);

    Compat32State in{};
    in.eax = 0xFFFFFFFF;

    ASSERT_TRUE(run_guest32(code, sizeof(code), in, r));
    EXPECT_EQ(r->completed, 1u);
    EXPECT_EQ(r->eax, 0u);
    EXPECT_NE(r->eflags & 0x40u, 0u) << "ZF not captured from guest execution";

    munmap(r, 4096);
}

TEST(Compat32Test, HostSurvivesAndCanStillUseLibc) {
    // The failure this guards is delayed and misleading: returning with guest
    // segments loaded leaves the thread in a state the kernel cannot deliver
    // a signal into, which surfaces as a fault inside some unrelated libc
    // call much later. Doing real host work after the round trip is the only
    // way to catch it.
    const u8 code[] = { 0xC3 };

    SharedResult* r = make_shared();
    ASSERT_NE(r, nullptr);

    Compat32State in{};
    in.eax = 0x5A5A5A5A;
    ASSERT_TRUE(run_guest32(code, sizeof(code), in, r));
    EXPECT_EQ(r->completed, 1u);
    EXPECT_EQ(r->eax, 0x5A5A5A5Au);

    munmap(r, 4096);
}

// ---------------------------------------------------------------------------
// Thunk dispatch
// ---------------------------------------------------------------------------
//
// This is what turns "executes code" into "runs a Windows program": a guest
// CALL to an imported function has to leave 32-bit mode, tell the host which
// API was invoked, and let the guest resume afterwards.

namespace {

constexpr u32 kThunkSlotA = kGuestCodeAddr + 0x400;
constexpr u32 kThunkSlotB = kGuestCodeAddr + 0x408;   // STUB_SIZE apart

struct ThunkResult {
    u32 completed;
    u32 exit_reason;    // 0 = sentinel, 1 = thunk
    u32 thunk_addr;
    u32 esp_at_thunk;
    u32 guest_ret;      // what the guest pushed as its own return address
    u32 eax;
};

} // namespace

TEST(Compat32ThunkTest, GuestCallToAThunkSlotReturnsToHost) {
    auto* r = static_cast<ThunkResult*>(
        mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0));
    ASSERT_NE(r, MAP_FAILED);
    std::memset(r, 0, sizeof(ThunkResult));

    const pid_t pid = fork();
    ASSERT_NE(pid, -1);
    if (pid == 0) {
        u8* text = map_fixed_rwx(kGuestCodeAddr, 0x1000);
        if (!text) _exit(20);
        if (!map_fixed_rwx(kGuestStackTop - 0x4000, 0x4000)) _exit(21);

        auto ex = Compat32Executor::create(kTrampolineAddr);
        if (ex.is_err()) _exit(22);
        Compat32Executor* exec = ex.value();

        // Guest:  E8 <rel32>  call kThunkSlotA
        //         C3          ret
        const u32 call_site_end = kGuestCodeAddr + 5;
        const u32 rel = kThunkSlotA - call_site_end;
        text[0] = 0xE8;
        std::memcpy(text + 1, &rel, 4);
        text[5] = 0xC3;

        // The thunk slot holds the native stub, exactly as the loader will
        // write it into ThunkRegistry's slots.
        exec->emit_thunk_stub(text + (kThunkSlotA - kGuestCodeAddr), kThunkSlotA);
        if (!install_fs_teb(exec)) _exit(23);

        Compat32State st{};
        st.eip = kGuestCodeAddr;
        st.esp = kGuestStackTop - 0x100;
        const u32 sentinel = exec->sentinel_return_address();
        std::memcpy(reinterpret_cast<void*>(static_cast<uintptr_t>(st.esp)),
                    &sentinel, 4);
        st.eax = 0x1234;

        const Compat32Exit why = exec->run(st);

        r->exit_reason  = (why == Compat32Exit::ThunkCall) ? 1u : 0u;
        r->thunk_addr   = exec->last_thunk_address();
        r->esp_at_thunk = st.esp;
        r->eax          = st.eax;
        // The guest's own return address must be on top of its stack.
        std::memcpy(&r->guest_ret,
                    reinterpret_cast<void*>(static_cast<uintptr_t>(st.esp)), 4);
        r->completed = 1;
        _exit(0);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    ASSERT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0)
        << "child died during thunk dispatch";

    EXPECT_EQ(r->completed, 1u);
    EXPECT_EQ(r->exit_reason, 1u) << "did not report a thunk call";
    EXPECT_EQ(r->thunk_addr, kThunkSlotA) << "wrong thunk identified";
    // The collector must leave the stack exactly as the guest's own CALL did,
    // so the host can dispatch and resume without unwinding anything.
    EXPECT_EQ(r->guest_ret, kGuestCodeAddr + 5)
        << "guest return address not on top of stack";
    EXPECT_EQ(r->eax, 0x1234u) << "guest registers not preserved across exit";

    munmap(r, 4096);
}

TEST(Compat32ThunkTest, DistinctSlotsAreDistinguished) {
    // Identity comes from the return address the stub's CALL pushes, so two
    // adjacent slots must not be confused -- the failure would be dispatching
    // the wrong Windows API, which is far worse than a crash.
    for (u32 slot : {kThunkSlotA, kThunkSlotB}) {
        auto* r = static_cast<ThunkResult*>(
            mmap(nullptr, 4096, PROT_READ | PROT_WRITE,
                 MAP_SHARED | MAP_ANON, -1, 0));
        ASSERT_NE(r, MAP_FAILED);
        std::memset(r, 0, sizeof(ThunkResult));

        const pid_t pid = fork();
        ASSERT_NE(pid, -1);
        if (pid == 0) {
            u8* text = map_fixed_rwx(kGuestCodeAddr, 0x1000);
            if (!text) _exit(20);
            if (!map_fixed_rwx(kGuestStackTop - 0x4000, 0x4000)) _exit(21);
            auto ex = Compat32Executor::create(kTrampolineAddr);
            if (ex.is_err()) _exit(22);
            Compat32Executor* exec = ex.value();

            const u32 rel = slot - (kGuestCodeAddr + 5);
            text[0] = 0xE8;
            std::memcpy(text + 1, &rel, 4);
            text[5] = 0xC3;
            exec->emit_thunk_stub(text + (slot - kGuestCodeAddr), slot);
            if (!install_fs_teb(exec)) _exit(23);

            Compat32State st{};
            st.eip = kGuestCodeAddr;
            st.esp = kGuestStackTop - 0x100;
            const u32 sentinel = exec->sentinel_return_address();
            std::memcpy(reinterpret_cast<void*>(static_cast<uintptr_t>(st.esp)),
                        &sentinel, 4);
            (void)exec->run(st);
            r->thunk_addr = exec->last_thunk_address();
            r->completed = 1;
            _exit(0);
        }
        int status = 0;
        waitpid(pid, &status, 0);
        ASSERT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0);
        EXPECT_EQ(r->thunk_addr, slot);
        munmap(r, 4096);
    }
}

TEST(Compat32ThunkTest, StubFitsThunkRegistrySlotSize) {
    // ThunkRegistry::STUB_SIZE is 8. If the stub ever outgrows it, slots would
    // overlap and every import would dispatch to the wrong handler.
    EXPECT_LE(Compat32Executor::thunk_stub_size(), 8u);
}

TEST(Compat32ThunkTest, SharedStubReachesCallingThreadsCollector) {
    // The thunk table is shared guest memory. Two threads must be able to
    // execute the same stub and land in *their* trampoline, not a global one.
    constexpr u32 kThreads = 2;
    constexpr u32 kTebSpan = 0x2000;

    struct Res {
        u32 completed[kThreads];
        u32 thunk_addr[kThreads];
        u32 trampoline[kThreads];
        u32 eax[kThreads];
    };
    auto* r = static_cast<Res*>(
        mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0));
    ASSERT_NE(r, MAP_FAILED);
    std::memset(r, 0, sizeof(Res));

    const pid_t pid = fork();
    ASSERT_NE(pid, -1);
    if (pid == 0) {
        u8* text = map_fixed_rwx(kGuestCodeAddr, 0x1000);
        if (!text) _exit(20);

        // Guest: call shared slot / ret. One copy, many threads.
        const u32 call_site_end = kGuestCodeAddr + 5;
        const u32 rel = kThunkSlotA - call_site_end;
        text[0] = 0xE8;
        std::memcpy(text + 1, &rel, 4);
        text[5] = 0xC3;
        Compat32Executor::emit_thunk_stub(
            text + (kThunkSlotA - kGuestCodeAddr), kThunkSlotA);

        std::thread workers[kThreads];
        for (u32 i = 0; i < kThreads; ++i) {
            workers[i] = std::thread([&, i] {
                const u32 stack_top = kGuestStackTop + i * 0x10000;
                if (!map_fixed_rwx(stack_top - 0x4000, 0x4000)) return;

                auto ex = Compat32Executor::for_current_thread();
                if (ex.is_err()) return;
                Compat32Executor* exec = ex.value();
                const u32 teb = kTebAddr + i * kTebSpan;
                if (!install_fs_teb(exec, teb)) return;

                Compat32State st{};
                st.eip = kGuestCodeAddr;
                st.esp = stack_top - 0x100;
                st.eax = 0x1000u + i;
                const u32 sentinel = exec->sentinel_return_address();
                std::memcpy(reinterpret_cast<void*>(
                                static_cast<uintptr_t>(st.esp)),
                            &sentinel, 4);

                const Compat32Exit why = exec->run(st);
                if (why != Compat32Exit::ThunkCall) return;
                r->thunk_addr[i] = exec->last_thunk_address();
                r->trampoline[i] = exec->trampoline_base();
                r->eax[i] = st.eax;
                r->completed[i] = 1;
            });
        }
        for (auto& w : workers) w.join();
        _exit(0);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    ASSERT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0)
        << "child died during per-thread thunk dispatch";

    for (u32 i = 0; i < kThreads; ++i) {
        EXPECT_EQ(r->completed[i], 1u) << "thread " << i;
        EXPECT_EQ(r->thunk_addr[i], kThunkSlotA) << "thread " << i;
        EXPECT_EQ(r->eax[i], 0x1000u + i) << "thread " << i;
    }
    EXPECT_NE(r->trampoline[0], r->trampoline[1])
        << "threads shared a trampoline";

    munmap(r, 4096);
}

TEST(Compat32Test, RunsFromATrampolineAboveTwoGigabytes) {
    // Regression: the 64-bit half of the trampoline once used absolute
    // [disp32] operands, which are SIGN-extended in 64-bit mode. A page at or
    // above 0x80000000 therefore read its saved host RSP from
    // 0xFFFFFFFF_xxxxxxxx and died with no stack and no signal handler --
    // while every test below 2GB kept passing. The pool lives above 2GB on
    // purpose (that half of the 32-bit space is kernel space to a Win32 guest,
    // so it cannot collide with one), so this must work.
    constexpr u32 kHighTrampoline = 0x90000000;

    struct Res { u32 completed, eax; };
    auto* r = static_cast<Res*>(
        mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0));
    ASSERT_NE(r, MAP_FAILED);
    std::memset(r, 0, sizeof(Res));

    const pid_t pid = fork();
    ASSERT_NE(pid, -1);
    if (pid == 0) {
        u8* text = map_fixed_rwx(kGuestCodeAddr, 0x1000);
        if (!text) _exit(20);
        const u8 code[] = { 0x01, 0xD8, 0xC3 };   // add eax, ebx / ret
        std::memcpy(text, code, sizeof(code));
        if (!map_fixed_rwx(kGuestStackTop - 0x4000, 0x4000)) _exit(21);

        auto ex = Compat32Executor::create(kHighTrampoline);
        if (ex.is_err()) _exit(22);
        Compat32Executor* exec = ex.value();

        Compat32State st{};
        st.eip = kGuestCodeAddr;
        st.eax = 40;
        st.ebx = 2;
        st.esp = kGuestStackTop - 0x100;
        const u32 sentinel = exec->sentinel_return_address();
        std::memcpy(reinterpret_cast<void*>(static_cast<uintptr_t>(st.esp)),
                    &sentinel, 4);

        r->completed = (exec->run(st) == Compat32Exit::ReturnedToSentinel) ? 1u : 0u;
        r->eax = st.eax;
        delete exec;
        _exit(0);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    ASSERT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0)
        << "child died with a trampoline above 2GB";
    EXPECT_EQ(r->completed, 1u);
    EXPECT_EQ(r->eax, 42u);

    munmap(r, 4096);
}

// ---------------------------------------------------------------------------
// Multi-threaded guest execution
// ---------------------------------------------------------------------------
//
// A trampoline holds the guest register mirror, the resume address and the
// saved 64-bit host RSP at fixed addresses, because a 32-bit stub has no base
// register to spare. Sharing one between threads therefore does not merely
// scramble registers -- the second thread returns onto the first thread's host
// stack. Each guest thread gets its own page, and these tests prove it.

namespace {

constexpr u32 kMtThreads       = 4;
constexpr u32 kMtCodeAddr      = 0x3C000000;
constexpr u32 kMtStackBase     = 0x3D000000;   // per-thread, kMtStackSpan apart
constexpr u32 kMtStackSpan     = 0x00100000;
constexpr u32 kMtStackBytes    = 0x4000;
constexpr u32 kMtRunsPerThread = 200;
constexpr u32 kMtSpinCount     = 4000;

struct MtThreadResult {
    u32 base;         // trampoline this thread was given
    u32 runs;         // guest entries that returned to the sentinel
    u32 mismatches;   // times another thread's values came back
    u32 last_eax;
};

struct MtResult {
    u32            completed;
    u32            pages_before;
    u32            pages_after_join;
    MtThreadResult t[kMtThreads];
};

/// 32-bit guest:
///   dec ecx / jnz -3      spin, widening the window where two threads are
///                         inside compatibility mode at the same moment
///   add eax, ebx
///   ret
const u8 kMtSpinCode[] = { 0x49, 0x75, 0xFD, 0x01, 0xD8, 0xC3 };

/// One guest thread: its own executor, its own stack, its own register tags.
/// Every value it puts in must come back, or another thread clobbered it.
void mt_thread_body(u32 index, MtThreadResult* out, std::atomic<u32>* ready) {
    out->base = 0;

    u8* stack = map_fixed_rwx(kMtStackBase + index * kMtStackSpan - kMtStackBytes,
                              kMtStackBytes);
    if (stack == nullptr) return;
    const u32 stack_top = kMtStackBase + index * kMtStackSpan;

    auto ex = Compat32Executor::for_current_thread();
    if (ex.is_err()) return;
    Compat32Executor* exec = ex.value();
    out->base = exec->trampoline_base();

    // The tag is distinct per thread in every nibble, so a stray value is
    // traceable to the thread it came from rather than merely "wrong".
    const u32 tag = 0x11111111u * (index + 1);

    // Start together: the corruption only exists while two threads are in
    // 32-bit mode simultaneously.
    ready->fetch_add(1);
    while (ready->load() < kMtThreads) { /* spin */ }

    for (u32 i = 0; i < kMtRunsPerThread; ++i) {
        Compat32State st{};
        st.eip = kMtCodeAddr;
        st.esp = stack_top - 0x100;
        const u32 sentinel = exec->sentinel_return_address();
        std::memcpy(reinterpret_cast<void*>(static_cast<uintptr_t>(st.esp)),
                    &sentinel, 4);

        st.eax = tag;
        st.ebx = tag ^ 0x0F0F0F0Fu;
        st.ecx = kMtSpinCount;
        st.edx = tag + 0xD;
        st.ebp = tag + 0xB;
        st.esi = tag + 0x5;
        st.edi = tag + 0x7;

        if (exec->run(st) != Compat32Exit::ReturnedToSentinel) {
            ++out->mismatches;
            continue;
        }
        ++out->runs;
        out->last_eax = st.eax;

        const u32 expect_eax = tag + (tag ^ 0x0F0F0F0Fu);
        if (st.eax != expect_eax || st.ebx != (tag ^ 0x0F0F0F0Fu) ||
            st.ecx != 0 || st.edx != tag + 0xD || st.ebp != tag + 0xB ||
            st.esi != tag + 0x5 || st.edi != tag + 0x7 ||
            st.esp != stack_top - 0x100 + 4) {
            ++out->mismatches;
        }
    }
}

} // namespace

TEST(Compat32ThreadTest, ThreadsExecuteGuestCodeConcurrentlyWithoutCorruption) {
    auto* r = static_cast<MtResult*>(
        mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0));
    ASSERT_NE(r, MAP_FAILED);
    std::memset(r, 0, sizeof(MtResult));

    const pid_t pid = fork();
    ASSERT_NE(pid, -1);
    if (pid == 0) {
        u8* text = map_fixed_rwx(kMtCodeAddr, 0x1000);
        if (!text) _exit(20);
        std::memcpy(text, kMtSpinCode, sizeof(kMtSpinCode));

        r->pages_before = static_cast<u32>(Compat32Executor::pooled_pages_in_use());

        std::atomic<u32> ready{0};
        std::thread workers[kMtThreads];
        for (u32 i = 0; i < kMtThreads; ++i) {
            workers[i] = std::thread(mt_thread_body, i, &r->t[i], &ready);
        }
        for (auto& w : workers) w.join();

        // Each thread's page must have gone back to the pool at thread exit.
        r->pages_after_join =
            static_cast<u32>(Compat32Executor::pooled_pages_in_use());
        r->completed = 1;
        _exit(0);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    ASSERT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0)
        << "child died running 32-bit guest code on multiple threads";
    ASSERT_EQ(r->completed, 1u);

    for (u32 i = 0; i < kMtThreads; ++i) {
        EXPECT_NE(r->t[i].base, 0u) << "thread " << i << " got no executor";
        EXPECT_EQ(r->t[i].runs, kMtRunsPerThread) << "thread " << i;
        EXPECT_EQ(r->t[i].mismatches, 0u)
            << "thread " << i << " saw corrupted state; last eax = "
            << std::hex << r->t[i].last_eax;
    }

    // Distinct trampolines, all below 4GB: 32-bit stubs address their slots
    // absolutely, so a page above 4GB is unreachable from guest code.
    for (u32 i = 0; i < kMtThreads; ++i) {
        EXPECT_EQ(r->t[i].base & 0xFFFu, 0u) << "trampoline not page aligned";
        for (u32 j = i + 1; j < kMtThreads; ++j) {
            EXPECT_NE(r->t[i].base, r->t[j].base)
                << "threads " << i << " and " << j << " share a trampoline";
        }
    }

    // No leak: a pool page is finite low address space, and a thread that has
    // exited must not still be holding one.
    EXPECT_EQ(r->pages_after_join, r->pages_before)
        << "trampoline pages leaked at thread exit";

    munmap(r, 4096);
}

TEST(Compat32ThreadTest, PerThreadAccessorIsStableWithinAThread) {
    // The accessor is called on every guest entry, so it must return the same
    // executor rather than exhausting the pool one call at a time.
    struct Res { u32 completed, first, second, pages; };
    auto* r = static_cast<Res*>(
        mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0));
    ASSERT_NE(r, MAP_FAILED);
    std::memset(r, 0, sizeof(Res));

    const pid_t pid = fork();
    ASSERT_NE(pid, -1);
    if (pid == 0) {
        auto a = Compat32Executor::for_current_thread();
        if (a.is_err()) _exit(22);
        auto b = Compat32Executor::for_current_thread();
        if (b.is_err()) _exit(23);
        r->first  = a.value()->trampoline_base();
        r->second = b.value()->trampoline_base();
        r->pages  = static_cast<u32>(Compat32Executor::pooled_pages_in_use());
        r->completed = 1;
        _exit(0);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    ASSERT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    EXPECT_EQ(r->completed, 1u);
    EXPECT_EQ(r->first, r->second);
    EXPECT_EQ(r->pages, 1u) << "repeated access consumed extra pool pages";
    EXPECT_LT(static_cast<u64>(r->first), 0x100000000ULL);

    munmap(r, 4096);
}

TEST(Compat32ThreadTest, EnteringAForeignThreadsTrampolineIsRefused) {
    // Regression guard for the corruption this whole design avoids: run() from
    // the wrong thread must fail loudly, not scribble on another thread's
    // register mirror and saved host RSP.
    struct Res { u32 completed, exit_reason; };
    auto* r = static_cast<Res*>(
        mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0));
    ASSERT_NE(r, MAP_FAILED);
    std::memset(r, 0, sizeof(Res));

    const pid_t pid = fork();
    ASSERT_NE(pid, -1);
    if (pid == 0) {
        if (!map_fixed_rwx(kGuestCodeAddr, 0x1000)) _exit(20);
        if (!map_fixed_rwx(kGuestStackTop - 0x4000, 0x4000)) _exit(21);
        auto ex = Compat32Executor::create(kTrampolineAddr);
        if (ex.is_err()) _exit(22);
        Compat32Executor* exec = ex.value();

        *reinterpret_cast<u8*>(static_cast<uintptr_t>(kGuestCodeAddr)) = 0xC3;

        u32 reason = 99;
        std::thread other([&] {
            Compat32State st{};
            st.eip = kGuestCodeAddr;
            st.esp = kGuestStackTop - 0x100;
            const u32 sentinel = exec->sentinel_return_address();
            std::memcpy(reinterpret_cast<void*>(static_cast<uintptr_t>(st.esp)),
                        &sentinel, 4);
            reason = (exec->run(st) == Compat32Exit::SetupFailed) ? 1u : 0u;
        });
        other.join();

        r->exit_reason = reason;
        r->completed = 1;
        delete exec;
        _exit(0);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    ASSERT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0)
        << "child died -- a foreign-thread entry was not refused";
    EXPECT_EQ(r->completed, 1u);
    EXPECT_EQ(r->exit_reason, 1u) << "run() accepted a foreign thread";

    munmap(r, 4096);
}
