/// @file test_guest_crt_accel.cpp
/// @brief Executes the guest-side CRT accelerators and checks them against the
///        C semantics they claim to implement.
///
/// These functions are hand-written machine code injected into the guest's
/// address space, so "it looks right" is not a standard worth trusting. Each
/// case runs the real emitted bytes through Vitreo's interpreter and compares
/// the result against the host's own libc, including the parts that are easy
/// to get wrong: the returned pointer, the callee-saved registers, the
/// direction flag, and overlapping memmove in both directions.

#include <gtest/gtest.h>
#include <vitreo/common.hpp>
#include <winapi/msvcrt/guest_accel.hpp>
#include <winapi/msvcrt/io/crt_stdio_buffer.hpp>
#include <core/cpu/state/cpu_state.hpp>
#include <core/cpu/interpreter/interpreter.hpp>
#include <core/memory/guest_memory.hpp>

#include <cstring>
#include <string>
#include <vector>

using namespace vitreo;
using namespace vitreo::winapi;

namespace {

constexpr GuestAddr kCodeBase  = 0x00100000;
constexpr GuestAddr kDataBase  = 0x00200000;
constexpr GuestAddr kStackBase = 0x00300000;
constexpr GuestAddr kTebBase   = 0x00500000;
constexpr u32       kTestTid   = 0x00001234;
// Where an unresolved tail-jump lands. build_guest_crt_code leaves the slow-path
// rel32 slots empty (install_* fills them from the thunk arena), so the harness
// points them here: reaching it means the fast path declined, which is itself
// the behaviour under test.
constexpr u32       kSlowMagic = 0x00DEAD10;
constexpr u32       kRetMagic  = 0x00DEAD00;   // never mapped; a ret here stops us

class GuestCrtAccelTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(memory.map_region(kCodeBase, 0x1000,
                                      MemoryProtection::ReadWrite).is_ok());
        ASSERT_TRUE(memory.map_region(kDataBase, 0x4000,
                                      MemoryProtection::ReadWrite).is_ok());
        ASSERT_TRUE(memory.map_region(kStackBase, 0x2000,
                                      MemoryProtection::ReadWrite).is_ok());
        // The critical-section fast paths read the current thread id from
        // fs:[0x24] (TEB ClientId.UniqueThread), so the harness needs a TEB.
        ASSERT_TRUE(memory.map_region(kTebBase, 0x1000,
                                      MemoryProtection::ReadWrite).is_ok());
        ASSERT_TRUE(memory.write_u32(kTebBase + 0x24, kTestTid).is_ok());

        blob = build_guest_crt_code(kCodeBase, /*include_critical_sections=*/true);
        ASSERT_FALSE(blob.code.empty());
        for (const auto& [slot, name] : blob.extern_fixups) {
            const u32 site = static_cast<u32>(kCodeBase + slot);
            const u32 rel = kSlowMagic - (site + 4u);
            std::memcpy(blob.code.data() + slot, &rel, 4);
        }
        ASSERT_TRUE(memory.write_bytes(kCodeBase, blob.code.data(),
                                       blob.code.size()).is_ok());
    }

    [[nodiscard]] u32 entry(const std::string& name) const {
        for (const auto& [n, addr] : blob.entries) {
            if (n == name || n.substr(n.find('!') + 1) == name) return addr;
        }
        return 0;
    }

    /// cdecl-call one accelerator and return EAX. Arguments are pushed
    /// right-to-left, and a sentinel return address stops the run.
    u32 call(const std::string& name, std::vector<u32> args) {
        const u32 fn = entry(name);
        EXPECT_NE(fn, 0u) << name << " was not emitted";
        took_slow_path = false;

        cpu::CpuState cpu;
        cpu.init_protected_mode();
        cpu.seg.fs_desc.base = kTebBase;
        u32 sp = kStackBase + 0x1000;
        for (auto it = args.rbegin(); it != args.rend(); ++it) {
            sp -= 4;
            EXPECT_TRUE(memory.write_u32(sp, *it).is_ok());
        }
        sp -= 4;
        EXPECT_TRUE(memory.write_u32(sp, kRetMagic).is_ok());
        cpu.set_sp(sp);
        cpu.rip = fn;

        // Callee-saved registers get recognisable values so a clobber shows up
        // as a failed expectation rather than as corruption somewhere later.
        cpu.gpr.set32(3, 0xB1B1B1B1u);   // ebx
        cpu.gpr.set32(5, 0xB5B5B5B5u);   // ebp
        cpu.gpr.set32(6, 0xB6B6B6B6u);   // esi
        cpu.gpr.set32(7, 0xB7B7B7B7u);   // edi

        cpu::Interpreter interp(cpu, memory);
        for (int steps = 0; steps < 2'000'000; ++steps) {
            if (static_cast<u32>(cpu.rip) == kSlowMagic) {
                took_slow_path = true;
                return 0;
            }
            if (static_cast<u32>(cpu.rip) == kRetMagic) {
                saved_ebx = cpu.gpr.get32(3);
                saved_ebp = cpu.gpr.get32(5);
                saved_esi = cpu.gpr.get32(6);
                saved_edi = cpu.gpr.get32(7);
                df_set = cpu.flags.df();
                return cpu.gpr.get32(0);
            }
            const cpu::ExecResult r = interp.step();
            if (r != cpu::ExecResult::Continue &&
                r != cpu::ExecResult::Branch) {
                ADD_FAILURE() << name << " stopped with ExecResult="
                              << static_cast<int>(r) << " at rip=0x"
                              << std::hex << cpu.rip;
                return 0;
            }
        }
        ADD_FAILURE() << name << " did not return";
        return 0;
    }

    void write_data(GuestAddr addr, const void* src, usize n) {
        ASSERT_TRUE(memory.write_bytes(addr, static_cast<const u8*>(src), n).is_ok());
    }
    std::vector<u8> read_data(GuestAddr addr, usize n) {
        std::vector<u8> out(n);
        EXPECT_TRUE(memory.read_bytes(addr, out.data(), n).is_ok());
        return out;
    }

    GuestMemory memory;
    CrtAccelBlob blob;
    u32 saved_ebx = 0, saved_ebp = 0, saved_esi = 0, saved_edi = 0;
    bool took_slow_path = false;
    bool df_set = false;
};

} // namespace

TEST_F(GuestCrtAccelTest, MemsetFillsAndReturnsDestination) {
    std::vector<u8> seed(64, 0xAA);
    write_data(kDataBase, seed.data(), seed.size());

    const u32 ret = call("memset", {kDataBase + 8, 0x5A, 16});

    EXPECT_EQ(ret, kDataBase + 8) << "memset must return its destination";
    auto got = read_data(kDataBase, 64);
    std::vector<u8> want(64, 0xAA);
    std::memset(want.data() + 8, 0x5A, 16);
    EXPECT_EQ(got, want);
    EXPECT_EQ(saved_ebx, 0xB1B1B1B1u);
    EXPECT_EQ(saved_esi, 0xB6B6B6B6u);
    EXPECT_EQ(saved_edi, 0xB7B7B7B7u) << "edi is callee-saved";
    EXPECT_FALSE(df_set) << "the ABI requires DF=0 on return";
}

// A zero count must touch nothing: rep with ECX=0 does not store, and this is
// the case a hand-written loop most easily gets wrong.
TEST_F(GuestCrtAccelTest, MemsetZeroCountWritesNothing) {
    std::vector<u8> seed(16, 0x11);
    write_data(kDataBase, seed.data(), seed.size());

    const u32 ret = call("memset", {kDataBase, 0xFF, 0});

    EXPECT_EQ(ret, kDataBase);
    EXPECT_EQ(read_data(kDataBase, 16), seed);
}

// Only the low byte of the int argument is stored.
TEST_F(GuestCrtAccelTest, MemsetUsesLowByteOfValue) {
    call("memset", {kDataBase, 0x1234AB, 4});
    const auto got = read_data(kDataBase, 4);
    EXPECT_EQ(got, (std::vector<u8>{0xAB, 0xAB, 0xAB, 0xAB}));
}

TEST_F(GuestCrtAccelTest, MemcpyCopiesAndReturnsDestination) {
    std::vector<u8> src(100);
    for (usize i = 0; i < src.size(); ++i) src[i] = static_cast<u8>(i * 7 + 1);
    write_data(kDataBase, src.data(), src.size());

    const u32 ret = call("memcpy",
                         {kDataBase + 0x800, kDataBase,
                          static_cast<u32>(src.size())});

    EXPECT_EQ(ret, kDataBase + 0x800);
    EXPECT_EQ(read_data(kDataBase + 0x800, src.size()), src);
    EXPECT_EQ(saved_esi, 0xB6B6B6B6u) << "esi is callee-saved";
    EXPECT_EQ(saved_edi, 0xB7B7B7B7u);
    EXPECT_FALSE(df_set);
}

// dst > src and the ranges overlap: the copy must run backwards or it smears
// the first byte across the whole range.
TEST_F(GuestCrtAccelTest, MemmoveHandlesForwardOverlap) {
    std::vector<u8> seed(64);
    for (usize i = 0; i < seed.size(); ++i) seed[i] = static_cast<u8>(i);
    write_data(kDataBase, seed.data(), seed.size());

    const u32 ret = call("memmove", {kDataBase + 4, kDataBase, 32});

    std::vector<u8> want = seed;
    std::memmove(want.data() + 4, want.data(), 32);
    EXPECT_EQ(ret, kDataBase + 4);
    EXPECT_EQ(read_data(kDataBase, 64), want);
    EXPECT_FALSE(df_set) << "the backward path must restore DF=0";
}

// dst < src and overlapping: the forward path is the correct one here.
TEST_F(GuestCrtAccelTest, MemmoveHandlesBackwardOverlap) {
    std::vector<u8> seed(64);
    for (usize i = 0; i < seed.size(); ++i) seed[i] = static_cast<u8>(i);
    write_data(kDataBase, seed.data(), seed.size());

    const u32 ret = call("memmove", {kDataBase, kDataBase + 4, 32});

    std::vector<u8> want = seed;
    std::memmove(want.data(), want.data() + 4, 32);
    EXPECT_EQ(ret, kDataBase);
    EXPECT_EQ(read_data(kDataBase, 64), want);
    EXPECT_FALSE(df_set);
}

TEST_F(GuestCrtAccelTest, MemmoveDisjointStillCopies) {
    std::vector<u8> seed(32);
    for (usize i = 0; i < seed.size(); ++i) seed[i] = static_cast<u8>(0xF0 + i);
    write_data(kDataBase, seed.data(), seed.size());

    const u32 ret = call("memmove",
                         {kDataBase + 0x400, kDataBase,
                          static_cast<u32>(seed.size())});

    EXPECT_EQ(ret, kDataBase + 0x400);
    EXPECT_EQ(read_data(kDataBase + 0x400, seed.size()), seed);
}

TEST_F(GuestCrtAccelTest, StrlenMatchesLibc) {
    for (const char* s : {"", "a", "hello", "0123456789abcdef"}) {
        const usize n = std::strlen(s);
        write_data(kDataBase, s, n + 1);
        EXPECT_EQ(call("strlen", {kDataBase}), static_cast<u32>(n))
            << "strlen(\"" << s << "\")";
        EXPECT_EQ(saved_edi, 0xB7B7B7B7u);
        EXPECT_FALSE(df_set);
    }
}

TEST_F(GuestCrtAccelTest, FgetcConsumesGuestBufferWithoutHostTransition) {
    using namespace crt_stdio_buffer;
    const GuestAddr stream = kDataBase + 0x100;
    const GuestAddr buffer = kDataBase + 0x800;
    const char bytes[] = "ABC";
    write_data(buffer, bytes, 3);
    ASSERT_TRUE(memory.write_u32(stream + kPtr, buffer).is_ok());
    ASSERT_TRUE(memory.write_u32(stream + kCount, 3).is_ok());
    ASSERT_TRUE(memory.write_u32(stream + kBase, buffer).is_ok());
    ASSERT_TRUE(memory.write_u32(stream + kLock, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(stream + kMagic, kMagicValue).is_ok());

    EXPECT_EQ(call("fgetc", {static_cast<u32>(stream)}), 'A');
    EXPECT_FALSE(took_slow_path);
    EXPECT_EQ(memory.read_u32(stream + kPtr).value(), buffer + 1);
    EXPECT_EQ(memory.read_u32(stream + kCount).value(), 2u);
    EXPECT_EQ(memory.read_u32(stream + kHasRead).value(), 1u);
    EXPECT_EQ(memory.read_u32(stream + kLock).value(), 0u);
}

TEST_F(GuestCrtAccelTest, UcrtFgetcUsesOpaqueBufferCellOffsets) {
    using namespace crt_stdio_buffer;
    const auto public_cells = cells(Layout::UcrtOpaque32);
    const GuestAddr stream = kDataBase + 0x100;
    const GuestAddr buffer = kDataBase + 0x800;
    const char bytes[] = "XYZ";
    write_data(buffer, bytes, 3);
    ASSERT_TRUE(memory.write_u32(stream + public_cells.ptr, buffer).is_ok());
    ASSERT_TRUE(memory.write_u32(stream + public_cells.count, 3).is_ok());
    ASSERT_TRUE(memory.write_u32(stream + public_cells.base, buffer).is_ok());
    ASSERT_TRUE(memory.write_u32(stream + kLock, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(stream + kMagic, kMagicValue).is_ok());

    EXPECT_EQ(call("ucrtbase.dll!fgetc", {static_cast<u32>(stream)}), 'X');
    EXPECT_FALSE(took_slow_path);
    EXPECT_EQ(memory.read_u32(stream + public_cells.ptr).value(), buffer + 1);
    EXPECT_EQ(memory.read_u32(stream + public_cells.count).value(), 2u);
    // UCRT count at +8 must not be mistaken for legacy base at +8.
    EXPECT_EQ(memory.read_u32(stream + public_cells.base).value(), buffer);
    EXPECT_EQ(memory.read_u32(stream + kLock).value(), 0u);
}

TEST_F(GuestCrtAccelTest, FgetcEmptyBufferTakesRefillTailPath) {
    using namespace crt_stdio_buffer;
    const GuestAddr stream = kDataBase + 0x100;
    ASSERT_TRUE(memory.write_u32(stream + kCount, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(stream + kLock, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(stream + kMagic, kMagicValue).is_ok());

    (void)call("fgetc", {static_cast<u32>(stream)});
    EXPECT_TRUE(took_slow_path);
    // The host refill target inherits ownership and is responsible for the
    // release after publishing the new buffer cells.
    EXPECT_EQ(memory.read_u32(stream + kLock).value(), kTestTid);
}

TEST_F(GuestCrtAccelTest, UngetcPushesExactlyOneCharacterAndClearsEof) {
    using namespace crt_stdio_buffer;
    const GuestAddr stream = kDataBase + 0x100;
    const GuestAddr buffer = kDataBase + 0x800;
    const char byte = 'A';
    write_data(buffer, &byte, 1);
    ASSERT_TRUE(memory.write_u32(stream + kPtr, buffer + 1).is_ok());
    ASSERT_TRUE(memory.write_u32(stream + kCount, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(stream + kBase, buffer).is_ok());
    ASSERT_TRUE(memory.write_u32(stream + kLock, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(stream + kMagic, kMagicValue).is_ok());
    ASSERT_TRUE(memory.write_u32(stream + kHasRead, 1).is_ok());
    ASSERT_TRUE(memory.write_u32(stream + kPushback, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(stream + kEof, 1).is_ok());

    EXPECT_EQ(call("ungetc", {'Z', static_cast<u32>(stream)}), 'Z');
    EXPECT_FALSE(took_slow_path);
    EXPECT_EQ(memory.read_u32(stream + kPtr).value(), buffer);
    EXPECT_EQ(memory.read_u32(stream + kCount).value(), 1u);
    EXPECT_EQ(memory.read_u32(stream + kPushback).value(), 1u);
    EXPECT_EQ(memory.read_u32(stream + kEof).value(), 0u);
    EXPECT_EQ(memory.read_u8(buffer).value(), static_cast<u8>('Z'));
    EXPECT_EQ(call("ungetc", {'Y', static_cast<u32>(stream)}), 0xFFFFFFFFu);
    EXPECT_EQ(memory.read_u32(stream + kCount).value(), 1u);
    EXPECT_EQ(memory.read_u32(stream + kLock).value(), 0u);
}

// ---------------------------------------------------------------------------
// Critical-section fast paths. The emitted code owns the guest's LockCount, so
// these check the exact word transitions the host half in critical_section.cpp
// contends for: SP1+ encoding, -1 unlocked, -2 locked with no waiters.
// ---------------------------------------------------------------------------

namespace {
constexpr u32 kUnlocked = 0xFFFFFFFFu;
constexpr u32 kLockedNoWaiters = 0xFFFFFFFEu;
constexpr GuestAddr kCs = kDataBase + 0x1000;

void init_cs(GuestMemory& m, GuestAddr cs) {
    EXPECT_TRUE(m.write_u32(cs + 0x04, kUnlocked).is_ok());  // LockCount
    EXPECT_TRUE(m.write_u32(cs + 0x08, 0).is_ok());          // RecursionCount
    EXPECT_TRUE(m.write_u32(cs + 0x0C, 0).is_ok());          // OwningThread
}
} // namespace

TEST_F(GuestCrtAccelTest, EnterCriticalSectionTakesUncontendedLock) {
    init_cs(memory, kCs);
    call("EnterCriticalSection", {kCs});

    EXPECT_EQ(memory.read_u32(kCs + 0x04).value_or(9u), kLockedNoWaiters)
        << "LockCount must go -1 -> -2 (SP1+: locked, no waiters)";
    EXPECT_EQ(memory.read_u32(kCs + 0x08).value_or(0u), 1u) << "RecursionCount";
    EXPECT_EQ(memory.read_u32(kCs + 0x0C).value_or(0u), kTestTid)
        << "OwningThread must be the current thread id from the TEB";
    EXPECT_EQ(saved_ebx, 0xB1B1B1B1u);
    EXPECT_EQ(saved_esi, 0xB6B6B6B6u);
    EXPECT_EQ(saved_edi, 0xB7B7B7B7u);
}

TEST_F(GuestCrtAccelTest, LeaveCriticalSectionReleasesUncontendedLock) {
    init_cs(memory, kCs);
    call("EnterCriticalSection", {kCs});
    call("LeaveCriticalSection", {kCs});

    EXPECT_EQ(memory.read_u32(kCs + 0x04).value_or(0u), kUnlocked)
        << "LockCount must return to -1";
    EXPECT_EQ(memory.read_u32(kCs + 0x08).value_or(9u), 0u) << "RecursionCount";
    EXPECT_EQ(memory.read_u32(kCs + 0x0C).value_or(9u), 0u)
        << "OwningThread must be cleared, and before the release";
}

// Recursion is decided by OwningThread, so a second Enter from the same thread
// must nest instead of touching the lock word.
TEST_F(GuestCrtAccelTest, EnterCriticalSectionRecursesWithoutRelocking) {
    init_cs(memory, kCs);
    call("EnterCriticalSection", {kCs});
    const u32 owner = memory.read_u32(kCs + 0x0C).value_or(0u);

    call("EnterCriticalSection", {kCs});
    EXPECT_EQ(memory.read_u32(kCs + 0x08).value_or(0u), 2u);
    EXPECT_EQ(memory.read_u32(kCs + 0x04).value_or(9u), kLockedNoWaiters)
        << "recursive enter must not re-take the lock word";
    EXPECT_EQ(memory.read_u32(kCs + 0x0C).value_or(0u), owner);

    call("LeaveCriticalSection", {kCs});
    EXPECT_EQ(memory.read_u32(kCs + 0x08).value_or(0u), 1u);
    EXPECT_EQ(memory.read_u32(kCs + 0x04).value_or(9u), kLockedNoWaiters)
        << "still held after the inner leave";

    call("LeaveCriticalSection", {kCs});
    EXPECT_EQ(memory.read_u32(kCs + 0x04).value_or(0u), kUnlocked);
}

// A section owned by someone else must not be taken by the fast path; it falls
// through to the slow export instead, which in this harness is an unmapped
// tail-jump, so the run faults rather than silently acquiring.
TEST_F(GuestCrtAccelTest, EnterCriticalSectionDoesNotStealHeldLock) {
    init_cs(memory, kCs);
    ASSERT_TRUE(memory.write_u32(kCs + 0x04, kLockedNoWaiters).is_ok());  // locked
    ASSERT_TRUE(memory.write_u32(kCs + 0x0C, 0xDEAD1234u).is_ok());  // by another thread
    ASSERT_TRUE(memory.write_u32(kCs + 0x08, 1u).is_ok());

    call("EnterCriticalSection", {kCs});

    EXPECT_TRUE(took_slow_path)
        << "a held section must fall through to the blocking half";
    EXPECT_EQ(memory.read_u32(kCs + 0x0C).value_or(0u), 0xDEAD1234u)
        << "the fast path must not overwrite another thread's ownership";
    EXPECT_EQ(memory.read_u32(kCs + 0x08).value_or(0u), 1u)
        << "and must not bump the other thread's recursion count";
}

// ---------------------------------------------------------------------------
// setjmp. The host longjmp reads these exact jmp_buf slots, so the emitted
// save must match JmpBuf32 in crt_init.cpp field for field.
// ---------------------------------------------------------------------------

TEST_F(GuestCrtAccelTest, SetjmpSavesCalleeStateAndReturnsZero) {
    constexpr GuestAddr kBuf = kDataBase + 0x1400;
    constexpr u32 kSehHead = 0x0012FF40u;
    ASSERT_TRUE(memory.write_u32(kTebBase + 0x00, kSehHead).is_ok());  // fs:[0]

    const u32 ret = call("_setjmp3", {kBuf});

    EXPECT_EQ(ret, 0u) << "the direct path must return 0";
    EXPECT_EQ(memory.read_u32(kBuf + 0).value_or(1u), 0xB5B5B5B5u)  << "EBP";
    EXPECT_EQ(memory.read_u32(kBuf + 4).value_or(1u), 0xB1B1B1B1u)  << "EBX";
    EXPECT_EQ(memory.read_u32(kBuf + 8).value_or(1u), 0xB7B7B7B7u)  << "EDI";
    EXPECT_EQ(memory.read_u32(kBuf + 12).value_or(1u), 0xB6B6B6B6u) << "ESI";
    EXPECT_EQ(memory.read_u32(kBuf + 20).value_or(0u), kRetMagic)
        << "EIP must be the address setjmp returns to";
    EXPECT_EQ(memory.read_u32(kBuf + 24).value_or(0u), kSehHead)
        << "Registration must capture the SEH chain head from fs:[0]";
    EXPECT_EQ(memory.read_u32(kBuf + 28).value_or(0u), 0xFFFFFFFFu)
        << "TryLevel";
    EXPECT_FALSE(df_set);
}

// The saved ESP must be the value the caller will have *after* setjmp returns,
// i.e. just past the return address -- longjmp restores it verbatim.
TEST_F(GuestCrtAccelTest, SetjmpSavesResumedStackPointer) {
    constexpr GuestAddr kBuf = kDataBase + 0x1500;
    call("_setjmp3", {kBuf});

    const u32 saved_esp = memory.read_u32(kBuf + 16).value_or(0u);
    // call() pushes one argument then the sentinel return address, so the
    // resumed esp points at the argument slot.
    EXPECT_EQ(saved_esp, kStackBase + 0x1000 - 4)
        << "ESP must skip the return address but not the argument (cdecl)";
}

// All three exported spellings share one implementation.
TEST_F(GuestCrtAccelTest, SetjmpAliasesAllResolve) {
    for (const char* name : {"_setjmp", "setjmp", "_setjmp3"}) {
        constexpr GuestAddr kBuf = kDataBase + 0x1600;
        EXPECT_EQ(call(name, {kBuf}), 0u) << name;
        EXPECT_EQ(memory.read_u32(kBuf + 28).value_or(0u), 0xFFFFFFFFu) << name;
    }
}

// ---------------------------------------------------------------------------
// bsearch. The interesting part is the callback: the emitted code calls a
// comparator that lives in guest memory, so these tests plant a real one and
// let the interpreter run it.
// ---------------------------------------------------------------------------

namespace {
constexpr GuestAddr kArray = kDataBase + 0x2000;
constexpr GuestAddr kKeyBuf = kDataBase + 0x2400;
constexpr GuestAddr kCmpFn = kCodeBase + 0x800;

/// int __cdecl cmp_i32(const int* a, const int* b) { return *a - *b; }
///   mov eax,[esp+4]; mov ecx,[esp+8]; mov eax,[eax]; sub eax,[ecx]; ret
void plant_comparator(GuestMemory& m) {
    static const u8 code[] = {
        0x8B, 0x44, 0x24, 0x04,   // mov eax, [esp+4]   (key)
        0x8B, 0x4C, 0x24, 0x08,   // mov ecx, [esp+8]   (element)
        0x8B, 0x00,               // mov eax, [eax]
        0x2B, 0x01,               // sub eax, [ecx]
        0xC3,                     // ret
    };
    ASSERT_TRUE(m.write_bytes(kCmpFn, code, sizeof(code)).is_ok());
}

void fill_sorted(GuestMemory& m, int count) {
    for (int i = 0; i < count; ++i) {
        ASSERT_TRUE(m.write_u32(kArray + static_cast<GuestAddr>(i) * 4,
                                static_cast<u32>(i * 10)).is_ok());
    }
}
} // namespace

TEST_F(GuestCrtAccelTest, BsearchFindsEveryPresentElement) {
    plant_comparator(memory);
    constexpr int kCount = 17;      // odd, so the midpoint is never exact
    fill_sorted(memory, kCount);

    for (int i = 0; i < kCount; ++i) {
        ASSERT_TRUE(memory.write_u32(kKeyBuf, static_cast<u32>(i * 10)).is_ok());
        const u32 got = call("bsearch", {kKeyBuf, kArray, kCount, 4, kCmpFn});
        EXPECT_EQ(got, kArray + static_cast<u32>(i) * 4)
            << "bsearch(" << i * 10 << ") returned the wrong element";
    }
}

TEST_F(GuestCrtAccelTest, BsearchReturnsNullWhenAbsent) {
    plant_comparator(memory);
    fill_sorted(memory, 16);

    for (u32 missing : {5u, 155u, 1000u}) {
        ASSERT_TRUE(memory.write_u32(kKeyBuf, missing).is_ok());
        EXPECT_EQ(call("bsearch", {kKeyBuf, kArray, 16, 4, kCmpFn}), 0u)
            << "missing key " << missing << " must yield NULL";
    }
    // Below the whole range, which drives hi towards 0.
    ASSERT_TRUE(memory.write_u32(kKeyBuf, 0xFFFFFFFFu).is_ok());
    EXPECT_EQ(call("bsearch", {kKeyBuf, kArray, 16, 4, kCmpFn}), 0u);
}

// num == 0 must not read the array at all, and must not loop.
TEST_F(GuestCrtAccelTest, BsearchEmptyRangeReturnsNull) {
    plant_comparator(memory);
    ASSERT_TRUE(memory.write_u32(kKeyBuf, 0u).is_ok());
    EXPECT_EQ(call("bsearch", {kKeyBuf, kArray, 0, 4, kCmpFn}), 0u);
    EXPECT_EQ(saved_ebx, 0xB1B1B1B1u);
    EXPECT_EQ(saved_esi, 0xB6B6B6B6u);
    EXPECT_EQ(saved_edi, 0xB7B7B7B7u);
    EXPECT_EQ(saved_ebp, 0xB5B5B5B5u) << "ebp is callee-saved";
}

TEST_F(GuestCrtAccelTest, BsearchSingleElement) {
    plant_comparator(memory);
    fill_sorted(memory, 1);
    ASSERT_TRUE(memory.write_u32(kKeyBuf, 0u).is_ok());
    EXPECT_EQ(call("bsearch", {kKeyBuf, kArray, 1, 4, kCmpFn}), kArray);
    ASSERT_TRUE(memory.write_u32(kKeyBuf, 10u).is_ok());
    EXPECT_EQ(call("bsearch", {kKeyBuf, kArray, 1, 4, kCmpFn}), 0u);
}

// Elements wider than a pointer exercise the mid*width multiply.
TEST_F(GuestCrtAccelTest, BsearchHonoursElementWidth) {
    plant_comparator(memory);
    constexpr int kCount = 9;
    constexpr u32 kWidth = 16;      // key is the first dword of each record
    for (int i = 0; i < kCount; ++i) {
        ASSERT_TRUE(memory.write_u32(kArray + static_cast<GuestAddr>(i) * kWidth,
                                     static_cast<u32>(i * 3)).is_ok());
    }
    for (int i = 0; i < kCount; ++i) {
        ASSERT_TRUE(memory.write_u32(kKeyBuf, static_cast<u32>(i * 3)).is_ok());
        EXPECT_EQ(call("bsearch", {kKeyBuf, kArray, kCount, kWidth, kCmpFn}),
                  kArray + static_cast<u32>(i) * kWidth);
    }
}
