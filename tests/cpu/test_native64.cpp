// SPDX-License-Identifier: LGPL-2.1-only

/// @file test_native64.cpp
/// @brief Native Win64 guest execution: entry, thunk collection, resume, exit.
///
/// The guest program here is hand-assembled rather than loaded from a PE, for
/// the same reason the compat32 tests do it: this exercises the hardware
/// hand-off, and a PE would drag the loader, the thunk registry and the whole
/// WinAPI layer into a test whose subject is nine instructions.
///
/// What is actually being proved is that guest code runs on the CPU. The
/// arithmetic below is never interpreted -- no Vitreo code reads those opcodes
/// -- so a correct result can only come from the processor having executed
/// them.

#include <gtest/gtest.h>

#include "core/cpu/x64/native64.hpp"
#include "core/cpu/x64/segment_override_rewrite.hpp"
#include "core/cpu/x64/x64_guest_thread.hpp"

#include <sys/mman.h>

#include <cstring>
#include <vector>

using namespace vitreo;
using namespace vitreo::cpu::x64;

namespace {

TEST(Native64SegmentRewrite, TranslatesOnlyEffectiveFsAndGsPrefixes) {
    // First instruction has two segment prefixes: only the final/effective GS
    // prefix changes. The second is guest FS. The third has no segment prefix.
    std::vector<u8> code = {
        0x64, 0x65, 0x48, 0x8B, 0x04, 0x25, 0x58, 0x00, 0x00, 0x00,
        0x64, 0x48, 0x8B, 0x00,
        0x48, 0x8B, 0x00,
    };

    EXPECT_EQ(rewrite_segment_overrides(code.data(), code.size()), 2u);
    const std::vector<u8> expected = {
        0x64, 0x64, 0x48, 0x8B, 0x04, 0x25, 0x58, 0x00, 0x00, 0x00,
        0x3E, 0x48, 0x8B, 0x00,
        0x48, 0x8B, 0x00,
    };
    EXPECT_EQ(code, expected);
}

TEST(Native64SegmentRewrite, LeavesInertPrefixBytesInInlineDataUntouched) {
    // MSVC places switch RVAs inline in .text. This is the byte boundary from
    // a real table that was corrupted when 0x64 (the low byte of an RVA) was
    // decoded as an FS prefix on JNE. Segment prefixes have no effect on a
    // branch and therefore must not be translated.
    std::vector<u8> bytes = {
        0x64, 0x75, 0x11, 0x00, // RVA 0x00117564
        0x6E, 0x75, 0x11, 0x00, // next RVA
    };
    const auto original = bytes;

    EXPECT_EQ(rewrite_segment_overrides(bytes.data(), bytes.size()), 0u);
    EXPECT_EQ(bytes, original);
}

TEST(Native64SegmentRewrite, RangeFormLeavesDataOutsideFunctionsUntouched) {
    // The bytes 0x65 0x20 0x66 0x6F are the ASCII "e fo" of a literal that
    // real .text sections carry, and they decode as
    // `and byte ptr gs:[rsi + 0x6f], ah` -- a segment-prefixed MEMORY operand,
    // so the inert-prefix guard does not save them. A linear sweep rewrites
    // that 0x65 and silently corrupts the literal; steamclient64.dll alone has
    // 219 such sites. Confining the rewrite to .pdata function ranges is what
    // makes the difference.
    const std::vector<u8> original = {
        0x65, 0x48, 0x8B, 0x04, 0x25, 0x58, 0x00, 0x00, 0x00,  // mov rax,gs:[0x58]
        0xC3,                                                  // ret
        0x65, 0x20, 0x66, 0x6F,                                // literal "e fo"
    };

    std::vector<u8> swept = original;
    EXPECT_EQ(rewrite_segment_overrides(swept.data(), swept.size()), 2u);
    EXPECT_EQ(swept[10], 0x64) << "the linear sweep corrupts the literal";

    std::vector<u8> guided = original;
    const CodeRange function{0, 10};
    EXPECT_EQ(rewrite_segment_overrides_ranges(guided.data(), guided.size(),
                                               &function, 1),
              1u);
    EXPECT_EQ(guided[0], 0x64) << "the real instruction is still translated";
    EXPECT_EQ(guided[10], 0x65) << "the literal is left alone";
}

TEST(Native64SegmentRewrite, RangeFormResyncsPastBytesTheDecoderRejects) {
    // Every byte inside a .pdata function range is code, so a byte this
    // decoder does not know must not cost the rest of the function. It cost
    // SDL3.dll exactly that: two leaf functions reading `gs:[0x30]` stayed
    // untranslated, read a null TEB, and spun in a fault storm.
    std::vector<u8> bytes = {
        0xC3,                                                  // ret
        0x06,                                                  // invalid in 64-bit mode
        0x65, 0x48, 0x8B, 0x04, 0x25, 0x58, 0x00, 0x00, 0x00,  // mov rax,gs:[0x58]
    };
    const CodeRange function{0, bytes.size()};

    EXPECT_EQ(rewrite_segment_overrides_ranges(bytes.data(), bytes.size(),
                                               &function, 1),
              1u);
    EXPECT_EQ(bytes[2], 0x64);
}

TEST(Native64SegmentRewrite, GapSweepTakesTebReadsAndLeavesLiteralsAlone) {
    // Between the functions .pdata names sit both literals and the leaf
    // functions the ABI lets it omit. The gap pass accepts only the shape a
    // TEB access has -- no base, no index, small displacement -- which is
    // every real one measured across Steam's modules and none of the junk.
    std::vector<u8> bytes = {
        0xC3,                                                  // ret: the named function
        0xCC, 0xCC,                                            // padding
        0x65, 0x48, 0x8B, 0x04, 0x25, 0x30, 0x00, 0x00, 0x00,  // mov rax,gs:[0x30]
        0x65, 0x20, 0x66, 0x6F,                                // literal "e fo"
        0x65, 0x2A, 0x3E,                                      // literal: sub bh,gs:[rsi]
    };
    const CodeRange function{0, 1};

    EXPECT_EQ(rewrite_segment_overrides_ranges(bytes.data(), bytes.size(),
                                               &function, 1),
              1u);
    EXPECT_EQ(bytes[3], 0x64) << "the leaf function's TEB read is translated";
    EXPECT_EQ(bytes[12], 0x65) << "a literal with a base register is not";
    EXPECT_EQ(bytes[16], 0x65) << "nor is one that indexes off a register";
}

TEST(Native64SegmentRewrite, OverlappingRangesTranslateEachSiteOnlyOnce) {
    // .pdata carries several entries for one function (chained unwind
    // fragments), so the same bytes are seeded more than once. Translating a
    // site twice reads the host FS byte the first pass wrote (0x64) as a
    // guest FS access and turns it into DS, whose base is zero -- steam.exe
    // faulted reading absolute 0x58 that way.
    std::vector<u8> bytes = {
        0x65, 0x48, 0x8B, 0x04, 0x25, 0x58, 0x00, 0x00, 0x00,  // mov rax,gs:[0x58]
        0xC3,
    };
    const CodeRange ranges[2] = {{0, bytes.size()}, {0, bytes.size()}};

    EXPECT_EQ(rewrite_segment_overrides_ranges(bytes.data(), bytes.size(),
                                               ranges, 2),
              1u);
    EXPECT_EQ(bytes[0], 0x64);
}

TEST(Native64ThunkStub, GuardDispatchTailJumpsThroughRax) {
    std::vector<u8> stub(thunk_stub_size(), 0xCC);

    emit_guard_dispatch_stub(stub.data());

    const std::vector<u8> expected = {
        0xFF, 0xE0,
        0x90, 0x90, 0x90, 0x90, 0x90, 0x90,
    };
    EXPECT_EQ(stub, expected);
}

constexpr u64 kTebAddr    = 0x30000000ULL;  // must be < 4GB: descriptor base
constexpr u64 kCodeAddr   = 0x40000000ULL;
constexpr u64 kStackAddr  = 0x41000000ULL;
constexpr u64 kStackSize  = 0x10000ULL;
constexpr u64 kThunkSlot  = kCodeAddr + 0x100;
/// ThunkRegistry treats slot_base + STUB_SIZE as the dispatch gateway, and
/// that is the address the stub's own call pushes.
constexpr u64 kThunkGateway = kThunkSlot + 8;

constexpr u64 kMarkerRax = 0x1111;
constexpr u64 kMarkerRcx = 0x2222;
/// Value the host injects as the thunk's return, to prove the guest resumes
/// with what the host put there rather than with what it had before.
constexpr u64 kInjectedRax = 0x5555000;

void* map_fixed(u64 addr, u64 size, int prot) {
    void* p = mmap(reinterpret_cast<void*>(addr), size, prot,
                   MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
    return p == MAP_FAILED ? nullptr : p;
}

/// mov rax, 0x1111 / mov rcx, 0x2222 / call <slot> / add rax,1 / ret
///
/// The `ret` returns to whatever the host pushed as the guest's return
/// address, which is the sentinel stub.
void assemble_guest(u8* code, bool clear_fs_mid_guest = false) {
    usize n = 0;
    auto emit = [&](std::initializer_list<u8> bytes) {
        for (const u8 b : bytes) code[n++] = b;
    };
    if (clear_fs_mid_guest) {
        // xor eax,eax / mov fs,ax. The next carrier access is
        // the import thunk below, so only the Mach bridge can recover it.
        emit({0x31, 0xC0, 0x8E, 0xE0});
    }
    emit({0x48, 0xC7, 0xC0, 0x11, 0x11, 0x00, 0x00});  // mov rax, 0x1111
    emit({0x48, 0xC7, 0xC1, 0x22, 0x22, 0x00, 0x00});  // mov rcx, 0x2222

    // call rel32 -> kThunkSlot, relative to the next instruction
    const usize call_at = n;
    emit({0xE8, 0, 0, 0, 0});
    const i32 rel = static_cast<i32>(
        static_cast<i64>(kThunkSlot) -
        static_cast<i64>(kCodeAddr + call_at + 5));
    std::memcpy(code + call_at + 1, &rel, sizeof(rel));

    emit({0x48, 0x83, 0xC0, 0x01});                    // add rax, 1
    emit({0xC3});                                      // ret

    // The thunk slot itself, exactly as ThunkRegistry would fill it.
    emit_thunk_stub(code + 0x100);
}

/// Guest driver that deliberately nulls GS before every entry.
///
/// This is not a contrived state: an LDT-derived GS base does not survive a
/// kernel transition, and the kernel restores the null selector silently. The
/// entry stub must therefore reload GS itself. Nulling it on purpose is the
/// only way to prove it does -- with the reload removed, the stub reads its
/// register file out of low memory, loads all zeros and jumps to address 0.
void guest_body_null_gs(GuestHostHop& hop, void* user);

/// Shared between the guest thread and the test.
struct Ctl {
    volatile u64 entries{0};
    volatile u64 last_reason{0};
    volatile bool stop{false};
    u64 max_iters{8};
};

/// Guest-side driver. LIBC-FREE.
void guest_body(GuestHostHop& hop, void* user) {
    auto* ctl = static_cast<Ctl*>(user);
    u16 fs = 0;
    __asm__ volatile("movw %%fs, %0" : "=r"(fs));
    for (u64 i = 0; i < ctl->max_iters && !ctl->stop; ++i) {
        const Native64Exit why = enter(fs);
        ctl->entries = i + 1;
        ctl->last_reason = static_cast<u64>(why);
        hop.guest_request();   // host inspects and edits the transfer block
    }
}

void guest_body_null_gs(GuestHostHop& hop, void* user) {
    auto* ctl = static_cast<Ctl*>(user);
    u16 fs = 0;
    __asm__ volatile("movw %%fs, %0" : "=r"(fs));
    for (u64 i = 0; i < ctl->max_iters && !ctl->stop; ++i) {
        // Exactly what a kernel transition leaves behind.
        __asm__ volatile("xorl %%eax, %%eax\n\tmovw %%ax, %%fs"
                         ::: "eax", "memory");
        const Native64Exit why = enter(fs);
        ctl->entries = i + 1;
        ctl->last_reason = static_cast<u64>(why);
        hop.guest_request();
    }
}

} // namespace

class Native64Test : public ::testing::Test {
protected:
    void SetUp() override {
        teb_ = static_cast<u8*>(
            map_fixed(kTebAddr, kN64RequiredTebSize, PROT_READ | PROT_WRITE));
        ASSERT_NE(teb_, nullptr) << "no TEB mapping below 4GB";
        // NT_TIB.Self -- the stubs and enter() both recover the TEB from it.
        std::memcpy(teb_ + 0x30, &kTebAddr, sizeof(kTebAddr));

        code_ = static_cast<u8*>(map_fixed(kCodeAddr, 0x1000,
                                           PROT_READ | PROT_WRITE));
        ASSERT_NE(code_, nullptr);
        assemble_guest(code_);
        ASSERT_EQ(mprotect(code_, 0x1000, PROT_READ | PROT_EXEC), 0);

        stack_ = static_cast<u8*>(
            map_fixed(kStackAddr, kStackSize, PROT_READ | PROT_WRITE));
        ASSERT_NE(stack_, nullptr);

        publish_gs_hooks(teb_);
        xfer_ = Native64Transfer::at(teb_);
    }

    void TearDown() override {
        if (teb_ != nullptr)   munmap(teb_, kN64RequiredTebSize);
        if (code_ != nullptr)  munmap(code_, 0x1000);
        if (stack_ != nullptr) munmap(stack_, kStackSize);
    }

    /// Guest stack primed with the sentinel as the outermost return address.
    u64 prime_stack() {
        const u64 top = kStackAddr + kStackSize - 64;
        const u64 sp = top - 8;
        const u64 sentinel = sentinel_return_address();
        std::memcpy(stack_ + (sp - kStackAddr), &sentinel, sizeof(sentinel));
        return sp;
    }

    u8* teb_{nullptr};
    u8* code_{nullptr};
    u8* stack_{nullptr};
    Native64Transfer* xfer_{nullptr};
};

/// The full round trip: guest executes, calls an import, the host services it
/// and injects a return value, the guest resumes and exits through the
/// sentinel.
TEST_F(Native64Test, GuestRunsNativelyThroughThunkAndSentinel) {
    const u64 initial_sp = prime_stack();

    std::memset(xfer_, 0, sizeof(*xfer_));
    publish_gs_hooks(teb_);
    xfer_->rip = kCodeAddr;
    xfer_->rsp = initial_sp;
    xfer_->flags = 0x202;

    Ctl ctl{};
    GuestTebThread thread;
    ASSERT_TRUE(thread.start(kTebAddr, &guest_body, &ctl).is_ok());

    std::vector<u64> reasons;
    std::vector<u64> thunk_gateways;
    u64 rax_at_thunk = 0;
    u64 rcx_at_thunk = 0;
    u64 guest_return_addr = 0;

    const bool clean = thread.serve([&] {
        const auto why = static_cast<Native64Exit>(xfer_->reason);
        reasons.push_back(xfer_->reason);

        if (why == Native64Exit::ThunkCall) {
            thunk_gateways.push_back(xfer_->rip);
            rax_at_thunk = xfer_->rax;
            rcx_at_thunk = xfer_->rcx;

            // The guest stack is as the guest's own `call` left it: its return
            // address on top. Consume it exactly as the shared dispatch path
            // does, and hand back a result in RAX.
            std::memcpy(&guest_return_addr,
                        stack_ + (xfer_->rsp - kStackAddr),
                        sizeof(guest_return_addr));
            xfer_->rip = guest_return_addr;
            xfer_->rsp += 8;
            xfer_->rax = kInjectedRax;
        } else {
            ctl.stop = true;
        }
    }, /*timeout_ns=*/5'000'000'000ULL);

    ASSERT_TRUE(clean) << "guest thread wedged";

    ASSERT_EQ(reasons.size(), 2u) << "expected one thunk call then the sentinel";
    EXPECT_EQ(reasons[0], static_cast<u64>(Native64Exit::ThunkCall));
    EXPECT_EQ(reasons[1], static_cast<u64>(Native64Exit::ReturnedToSentinel));

    // Registers the guest set with instructions no Vitreo code ever read.
    EXPECT_EQ(rax_at_thunk, kMarkerRax);
    EXPECT_EQ(rcx_at_thunk, kMarkerRcx);

    // The collector identified the slot from the return address its own call
    // pushed, with no register spent on carrying it.
    ASSERT_EQ(thunk_gateways.size(), 1u);
    EXPECT_EQ(thunk_gateways[0], kThunkGateway);

    // The guest resumed at its own instruction after the call...
    EXPECT_EQ(guest_return_addr, kCodeAddr + 19);
    // ...and ran `add rax, 1` on the value the host injected.
    EXPECT_EQ(xfer_->rax, kInjectedRax + 1);

    // Stack discipline: the sentinel's `ret` consumed exactly the one return
    // address that was primed, leaving nothing behind.
    EXPECT_EQ(xfer_->rsp, initial_sp + 8);

    // The TEB holds the transfer block, and TearDown unmaps it.
    EXPECT_TRUE(thread.wait_until_exited());
}

/// The entry stub reloads GS itself, so a guest thread whose selector was
/// reset -- which any kernel transition does -- still executes correctly.
TEST_F(Native64Test, EntryReloadsGsThatAKernelTransitionWouldHaveReset) {
    const u64 initial_sp = prime_stack();

    std::memset(xfer_, 0, sizeof(*xfer_));
    publish_gs_hooks(teb_);
    xfer_->rip = kCodeAddr;
    xfer_->rsp = initial_sp;
    xfer_->flags = 0x202;

    Ctl ctl{};
    GuestTebThread thread;
    ASSERT_TRUE(thread.start(kTebAddr, &guest_body_null_gs, &ctl).is_ok());

    std::vector<u64> reasons;
    u64 guest_return_addr = 0;
    const bool clean = thread.serve([&] {
        reasons.push_back(xfer_->reason);
        if (static_cast<Native64Exit>(xfer_->reason) ==
            Native64Exit::ThunkCall) {
            std::memcpy(&guest_return_addr,
                        stack_ + (xfer_->rsp - kStackAddr),
                        sizeof(guest_return_addr));
            xfer_->rip = guest_return_addr;
            xfer_->rsp += 8;
            xfer_->rax = kInjectedRax;
        } else {
            ctl.stop = true;
        }
    }, 5'000'000'000ULL);

    ASSERT_TRUE(clean) << "guest wedged with a reset GS selector";
    ASSERT_EQ(reasons.size(), 2u);
    EXPECT_EQ(reasons[0], static_cast<u64>(Native64Exit::ThunkCall));
    EXPECT_EQ(reasons[1], static_cast<u64>(Native64Exit::ReturnedToSentinel));
    EXPECT_EQ(xfer_->rax, kInjectedRax + 1);
    EXPECT_TRUE(thread.wait_until_exited());
}

/// The stub is exactly ThunkRegistry::STUB_SIZE and encodes the offset the
/// assembler stubs read. A mismatch here would send the first import call to
/// a wild address.
TEST_F(Native64Test, ThunkStubEncodingMatchesTheLayout) {
    u8 stub[16]{};
    emit_thunk_stub(stub);

    EXPECT_EQ(thunk_stub_size(), 8u);
    EXPECT_EQ(stub[0], 0x64);  // FS carrier prefix
    EXPECT_EQ(stub[1], 0xFF);  // CALL r/m64
    EXPECT_EQ(stub[2], 0x14);  // mod=00 reg=/2 rm=SIB
    EXPECT_EQ(stub[3], 0x25);  // SIB: disp32, no base, no index

    u32 disp = 0;
    std::memcpy(&disp, stub + 4, sizeof(disp));
    EXPECT_EQ(disp, static_cast<u32>(kN64Collect));
    EXPECT_EQ(stub[8], 0u) << "stub wrote past ThunkRegistry::STUB_SIZE";
}

/// publish_gs_hooks writes the two addresses the guest-side stubs read; an
/// unpublished TEB would call through zero on the first import.
TEST_F(Native64Test, LayoutMatchesStubs) {
    std::memset(xfer_, 0, sizeof(*xfer_));
    publish_gs_hooks(teb_);

    u64 collect = 0;
    u64 sentinel = 0;
    std::memcpy(&collect, teb_ + kN64Collect, sizeof(collect));
    std::memcpy(&sentinel, teb_ + kN64Sentinel, sizeof(sentinel));

    EXPECT_EQ(collect, collect_address());
    EXPECT_EQ(sentinel, sentinel_return_address());
    EXPECT_NE(collect, 0u);
    EXPECT_NE(sentinel, 0u);
    EXPECT_NE(collect, sentinel);
}
