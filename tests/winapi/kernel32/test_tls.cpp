/// @file test_tls.cpp
/// @brief Tests for TlsManager (WIN32-039)

#include <gtest/gtest.h>
#include <vitreo/common.hpp>
#include <winapi/kernel32/thread/tls.hpp>
#include <winapi/kernel32/error.hpp>
#include <winapi/core/marshaler.hpp>
#include <core/memory/guest_memory.hpp>
#include <core/cpu/state/cpu_state.hpp>

#include <atomic>
#include <thread>
#include <vector>

using namespace vitreo;
using namespace vitreo::winapi;

// =============================================================================
// Test Fixture
// =============================================================================

class TlsManagerTest : public ::testing::Test {
protected:
    TlsManager mgr;
};

// =============================================================================
// TlsAlloc Tests
// =============================================================================

TEST_F(TlsManagerTest, TlsAllocReturnsZeroFirst) {
    u32 idx = mgr.tls_alloc();
    EXPECT_EQ(idx, 0u);
}

TEST_F(TlsManagerTest, TlsAllocReturnsSequentialIndices) {
    u32 idx0 = mgr.tls_alloc();
    u32 idx1 = mgr.tls_alloc();
    u32 idx2 = mgr.tls_alloc();
    EXPECT_EQ(idx0, 0u);
    EXPECT_EQ(idx1, 1u);
    EXPECT_EQ(idx2, 2u);
}

// =============================================================================
// TlsSetValue / TlsGetValue Round-Trip
// =============================================================================

TEST_F(TlsManagerTest, SetValueGetValueRoundTrip) {
    u32 idx = mgr.tls_alloc();
    ASSERT_NE(idx, TLS_OUT_OF_INDEXES);

    EXPECT_TRUE(mgr.tls_set_value(idx, 0xDEADBEEF'CAFEBABE));
    EXPECT_EQ(mgr.tls_get_value(idx), 0xDEADBEEF'CAFEBABE);
}

TEST_F(TlsManagerTest, GetValueOnUnsetSlotReturnsZero) {
    u32 idx = mgr.tls_alloc();
    ASSERT_NE(idx, TLS_OUT_OF_INDEXES);

    EXPECT_EQ(mgr.tls_get_value(idx), 0u);
}

// =============================================================================
// TlsFree Tests
// =============================================================================

TEST_F(TlsManagerTest, TlsFreeMarksSlotForReuse) {
    u32 idx = mgr.tls_alloc();
    ASSERT_NE(idx, TLS_OUT_OF_INDEXES);
    EXPECT_TRUE(mgr.tls_free(idx));
}

TEST_F(TlsManagerTest, TlsAllocAfterFreeReusesFreedSlot) {
    u32 idx0 = mgr.tls_alloc();
    u32 idx1 = mgr.tls_alloc();
    ASSERT_NE(idx0, TLS_OUT_OF_INDEXES);
    ASSERT_NE(idx1, TLS_OUT_OF_INDEXES);

    // Free slot 0, next alloc should reuse it
    EXPECT_TRUE(mgr.tls_free(idx0));
    u32 idx_reused = mgr.tls_alloc();
    EXPECT_EQ(idx_reused, idx0);
}

TEST(TlsTebReuseTest, ReallocatedIndexIsNullInEveryRegisteredThread) {
    // Microsoft Learn (TlsAlloc): when an index is allocated, its storage
    // slots are initialized to NULL in every thread of the process.
    constexpr GuestAddr kTeb1 = 0x20000;
    constexpr GuestAddr kTeb2 = 0x22000;
    constexpr GuestAddr kStack = 0x30000;

    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(
        kTeb1, 0x4000, MemoryProtection::ReadWrite).is_ok());
    ASSERT_TRUE(memory.map_region(
        kStack, 0x1000, MemoryProtection::ReadWrite).is_ok());

    cpu::CpuState cpu;
    cpu.init_protected_mode();
    cpu.seg.fs_desc.base = kTeb1;
    cpu.set_sp(kStack);
    ASSERT_TRUE(memory.write_u32(kStack, 0xDEADBEEF).is_ok());

    Marshaler m(memory);
    m.attach_cpu(cpu);
    TlsManager tm;
    // The thread lifecycle registers every TEB when it is created.
    tm.register_teb(memory, kTeb1, /*is_64bit=*/false);
    tm.register_teb(memory, kTeb2, /*is_64bit=*/false);

    impl_TlsAlloc(m, tm);
    const u32 index = static_cast<u32>(cpu.gpr.rax);
    ASSERT_NE(index, TLS_OUT_OF_INDEXES);

    auto set_args = [&](u32 slot, u32 value) {
        ASSERT_TRUE(memory.write_u32(kStack + 4, slot).is_ok());
        ASSERT_TRUE(memory.write_u32(kStack + 8, value).is_ok());
    };

    set_args(index, 0x11111111);
    impl_TlsSetValue(m, tm);

    cpu.seg.fs_desc.base = kTeb2;
    set_args(index, 0x22222222);
    impl_TlsSetValue(m, tm);

    cpu.seg.fs_desc.base = kTeb1;
    ASSERT_TRUE(memory.write_u32(kStack + 4, index).is_ok());
    impl_TlsFree(m, tm);
    ASSERT_EQ(cpu.gpr.rax, 1u);

    impl_TlsAlloc(m, tm);
    ASSERT_EQ(static_cast<u32>(cpu.gpr.rax), index);

    cpu.seg.fs_desc.base = kTeb2;
    ASSERT_TRUE(memory.write_u32(kStack + 4, index).is_ok());
    impl_TlsGetValue(m, tm);
    EXPECT_EQ(cpu.gpr.rax, 0u);
}

// A slot written without any Tls* call on that thread (the JIT fast path
// stores straight into TEB.TlsSlots) is still reset by TlsFree, and a TEB whose
// thread has exited is never written again.
TEST(TlsTebReuseTest, TlsFreeResetsLiveTebsOnlyAndIgnoresReleasedOnes) {
    constexpr GuestAddr kLive = 0x20000;
    constexpr GuestAddr kReleased = 0x22000;
    constexpr GuestAddr kStack = 0x30000;
    constexpr GuestAddr kSlots32 = 0xE10;  // TEB.TlsSlots (x86)

    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(kLive, 0x4000, MemoryProtection::ReadWrite).is_ok());
    ASSERT_TRUE(memory.map_region(kStack, 0x1000, MemoryProtection::ReadWrite).is_ok());

    cpu::CpuState cpu;
    cpu.init_protected_mode();
    cpu.seg.fs_desc.base = kLive;
    cpu.set_sp(kStack);
    Marshaler m(memory);
    m.attach_cpu(cpu);
    TlsManager tm;
    tm.register_teb(memory, kLive, false);
    tm.register_teb(memory, kReleased, false);

    impl_TlsAlloc(m, tm);
    const u32 index = static_cast<u32>(cpu.gpr.rax);
    ASSERT_LT(index, 64u);
    // Both threads store through the fast path: no Tls* call records them.
    ASSERT_TRUE(memory.write_u32(kLive + kSlots32 + index * 4, 0x11111111).is_ok());
    ASSERT_TRUE(memory.write_u32(kReleased + kSlots32 + index * 4, 0x22222222).is_ok());
    // The second thread exits: its TEB is unregistered before being freed
    // (here the memory stays mapped so a stray write would be visible).
    tm.unregister_teb(kReleased);

    ASSERT_TRUE(memory.write_u32(kStack + 4, index).is_ok());
    impl_TlsFree(m, tm);
    ASSERT_EQ(cpu.gpr.rax, 1u);
    EXPECT_EQ(memory.read_u32(kLive + kSlots32 + index * 4).value_or(1), 0u);
    EXPECT_EQ(memory.read_u32(kReleased + kSlots32 + index * 4).value_or(0), 0x22222222u);
}

TEST_F(TlsManagerTest, TlsFreeClearsStoredValue) {
    u32 idx = mgr.tls_alloc();
    ASSERT_NE(idx, TLS_OUT_OF_INDEXES);
    mgr.tls_set_value(idx, 42);
    mgr.tls_free(idx);

    // Re-allocate should get the same slot with value reset to 0
    u32 idx2 = mgr.tls_alloc();
    EXPECT_EQ(idx2, idx);
    EXPECT_EQ(mgr.tls_get_value(idx2), 0u);
}

TEST_F(TlsManagerTest, TlsFreeInvalidIndexReturnsFalse) {
    EXPECT_FALSE(mgr.tls_free(TLS_TOTAL_SLOTS));
    EXPECT_FALSE(mgr.tls_free(TLS_OUT_OF_INDEXES));
}

TEST_F(TlsManagerTest, TlsFreeUnallocatedIndexReturnsFalse) {
    // Slot 0 was never allocated
    EXPECT_FALSE(mgr.tls_free(0));
}

// =============================================================================
// Error Code Tests
// =============================================================================

TEST_F(TlsManagerTest, TlsGetValueSetsLastErrorToSuccess) {
    u32 idx = mgr.tls_alloc();
    ASSERT_NE(idx, TLS_OUT_OF_INDEXES);

    // Set last error to something else first
    set_last_error(error_codes::ERROR_INVALID_HANDLE);
    mgr.tls_get_value(idx);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_SUCCESS);
}

TEST_F(TlsManagerTest, TlsGetValueInvalidIndexReturnsZero) {
    u64 val = mgr.tls_get_value(TLS_TOTAL_SLOTS + 100);
    EXPECT_EQ(val, 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_PARAMETER);
}

TEST_F(TlsManagerTest, TlsSetValueInvalidIndexReturnsFalse) {
    EXPECT_FALSE(mgr.tls_set_value(TLS_TOTAL_SLOTS, 42));
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_PARAMETER);
}

// =============================================================================
// FLS Alias Tests
// =============================================================================

TEST_F(TlsManagerTest, FlsAllocGetSetRoundTrip) {
    u32 idx = mgr.fls_alloc();
    ASSERT_NE(idx, TLS_OUT_OF_INDEXES);

    EXPECT_TRUE(mgr.fls_set_value(idx, 0x1234));
    EXPECT_EQ(mgr.fls_get_value(idx), 0x1234u);
}

TEST_F(TlsManagerTest, FlsSharesSlotSpaceWithTls) {
    // Allocate via TLS, then via FLS — they share the same pool
    u32 tls_idx = mgr.tls_alloc();
    u32 fls_idx = mgr.fls_alloc();
    EXPECT_NE(tls_idx, fls_idx);
    EXPECT_EQ(fls_idx, tls_idx + 1);
}

TEST_F(TlsManagerTest, FlsFreeWorks) {
    u32 idx = mgr.fls_alloc();
    ASSERT_NE(idx, TLS_OUT_OF_INDEXES);
    EXPECT_TRUE(mgr.fls_free(idx));
    // Slot is reusable
    u32 idx2 = mgr.fls_alloc();
    EXPECT_EQ(idx2, idx);
}

// =============================================================================
// Exhaustion Test
// =============================================================================

TEST_F(TlsManagerTest, ExhaustAllSlotsReturnsTlsOutOfIndexes) {
    for (u32 i = 0; i < TLS_TOTAL_SLOTS; ++i) {
        u32 idx = mgr.tls_alloc();
        ASSERT_NE(idx, TLS_OUT_OF_INDEXES) << "Failed at slot " << i;
    }
    // Next alloc should fail
    u32 idx = mgr.tls_alloc();
    EXPECT_EQ(idx, TLS_OUT_OF_INDEXES);
}

// =============================================================================
// TEB-backed expansion slots (index >= 64)
// =============================================================================

TEST(TlsTebExpansionTest, SetValueDemandAllocatesExpansionArray) {
    // Windows stores expansion TLS in TEB.TlsExpansionSlots (x86 +0xF94),
    // demand-allocated on TlsSetValue. Guest code may inline that read.
    constexpr GuestAddr kTeb = 0x20000;
    constexpr usize kTebSize = 0x2000;

    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(kTeb, kTebSize, MemoryProtection::ReadWrite).is_ok());
    for (usize i = 0; i < kTebSize; ++i) {
        ASSERT_TRUE(memory.write_u8(kTeb + i, 0).is_ok());
    }

    cpu::CpuState cpu;
    cpu.init_protected_mode();
    cpu.seg.fs_desc.base = kTeb;

    TlsManager tm;
    // Burn the first 64 slots so the next alloc is an expansion index.
    for (u32 i = 0; i < TLS_MINIMUM_AVAILABLE; ++i) {
        ASSERT_NE(tm.tls_alloc(), TLS_OUT_OF_INDEXES);
    }
    const u32 idx = tm.tls_alloc();
    ASSERT_GE(idx, TLS_MINIMUM_AVAILABLE);
    ASSERT_NE(idx, TLS_OUT_OF_INDEXES);

    constexpr GuestAddr kStack = 0x30000;
    ASSERT_TRUE(memory.map_region(kStack, 0x1000, MemoryProtection::ReadWrite).is_ok());
    cpu.set_sp(kStack);
    ASSERT_TRUE(memory.write_u32(kStack, 0xDEADBEEF).is_ok());  // retaddr

    Marshaler m(memory);
    m.attach_cpu(cpu);

    auto set_args = [&](std::initializer_list<u32> args) {
        usize i = 0;
        for (u32 a : args) {
            ASSERT_TRUE(memory.write_u32(kStack + 4 + i * 4, a).is_ok());
            ++i;
        }
    };

    // Get before Set: expansion array absent ⇒ 0 (Windows contract).
    set_args({idx});
    impl_TlsGetValue(m, tm);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 0u);
    EXPECT_EQ(memory.read_u32(kTeb + 0xF94).value_or(1), 0u);

    // Set must allocate TEB.TlsExpansionSlots and store the value there.
    set_args({idx, 0xCAFEBABEu});
    impl_TlsSetValue(m, tm);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 1u);

    const u32 expansion = memory.read_u32(kTeb + 0xF94).value_or(0);
    ASSERT_NE(expansion, 0u);
    const u32 slot = expansion + (idx - TLS_MINIMUM_AVAILABLE) * 4;
    EXPECT_EQ(memory.read_u32(slot).value_or(0), 0xCAFEBABEu);

    // Get via thunk agrees with the TEB-resident slot.
    set_args({idx});
    impl_TlsGetValue(m, tm);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 0xCAFEBABEu);
}

// Learn, TlsSetValue: a value stored at an expansion index (64..1087) lives in
// the thread's TlsExpansionSlots array, allocated on that thread's first use;
// the only failure is lack of memory. Placement is first-fit, so every
// thread's first expansion write searches up from the same address: a search
// that loses the gap to another thread must retry, not fail. When it failed,
// the losing thread's TlsSetValue silently dropped its value -- a guest
// recursion count kept in a slot (Chaos FPhysicsRwLock) read back 0 and the
// matching ReleaseSRWLockShared never ran.
TEST(TlsExpansionSlotsTest, ConcurrentFirstUseGivesEveryThreadItsOwnArray) {
    constexpr int kThreads = 16;
    constexpr GuestAddr kTebBase = 0x100000;
    constexpr GuestAddr kTebStride = 0x2000;   // x64 TEB reaches 0x1788
    constexpr u32 kExpansionField = 0x1780;    // x64 TEB.TlsExpansionSlots
    GuestMemory memory;
    for (int i = 0; i < kThreads; ++i) {
        ASSERT_TRUE(memory.map_region(kTebBase + i * kTebStride, kTebStride,
                                      MemoryProtection::ReadWrite).is_ok());
    }
    std::atomic<int> ready{0};
    std::atomic<bool> go{false};
    std::vector<GuestAddr> arrays(kThreads, 0);
    std::vector<std::thread> threads;
    for (int i = 0; i < kThreads; ++i) {
        threads.emplace_back([&, i] {
            ready.fetch_add(1);
            while (!go.load()) std::this_thread::yield();
            arrays[i] = ensure_tls_expansion_slots(
                memory, kTebBase + i * kTebStride, /*is_64bit=*/true);
        });
    }
    while (ready.load() != kThreads) std::this_thread::yield();
    go.store(true);
    for (auto& t : threads) t.join();

    for (int i = 0; i < kThreads; ++i) {
        const GuestAddr teb = kTebBase + i * kTebStride;
        ASSERT_NE(arrays[i], 0u) << "thread " << i << " lost its first-fit race";
        EXPECT_EQ(memory.read_u64(teb + kExpansionField).value_or(0), arrays[i]);
        EXPECT_EQ(tls_expansion_slots(memory, teb, true), arrays[i]);
        // Zeroed: every expansion index starts NULL (Learn, TlsAlloc).
        EXPECT_EQ(memory.read_u64(arrays[i]).value_or(1), 0u);
        EXPECT_EQ(memory.read_u64(arrays[i] + (TLS_EXPANSION_SLOTS - 1) * 8)
                      .value_or(1), 0u);
        for (int j = 0; j < i; ++j) EXPECT_NE(arrays[i], arrays[j]);
        ASSERT_TRUE(memory.write_u64(arrays[i] + 8, 0x1000 + i).is_ok());
    }
    for (int i = 0; i < kThreads; ++i) {
        EXPECT_EQ(memory.read_u64(arrays[i] + 8).value_or(0), 0x1000u + i);
        // A second use finds the published array.
        EXPECT_EQ(ensure_tls_expansion_slots(memory, kTebBase + i * kTebStride,
                                             true),
                  arrays[i]);
    }
    // Thread exit frees the array and clears the field.
    for (int i = 0; i < kThreads; ++i) {
        const GuestAddr teb = kTebBase + i * kTebStride;
        release_tls_expansion_slots(memory, teb, true);
        EXPECT_EQ(tls_expansion_slots(memory, teb, true), 0u);
        EXPECT_TRUE(memory.read_u64(arrays[i]).is_err());
    }
}
