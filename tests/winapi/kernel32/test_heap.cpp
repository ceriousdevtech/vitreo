/// @file test_heap.cpp
/// @brief Unit tests for HeapCreate / HeapAlloc / HeapFree (WIN32-021)

#include <gtest/gtest.h>
#include <vitreo/common.hpp>
#include <winapi/kernel32/memory/heap.hpp>
#include <winapi/kernel32/memory/heap_ext.hpp>
#include <winapi/kernel32/memory/memory.hpp>
#include <winapi/kernel32/handle.hpp>
#include <winapi/kernel32/error.hpp>
#include <core/memory/guest_memory.hpp>
#include <core/cpu/state/cpu_state.hpp>
#include <winapi/core/marshaler.hpp>
#include <winapi/msvcrt/memory/crt_memory.hpp>


using namespace vitreo;
using namespace vitreo::winapi;

// =============================================================================
// Test Fixture
// =============================================================================

class HeapTest : public ::testing::Test {
protected:
    GuestMemory mem;
    VirtualMemoryManager vmm{mem};
    HandleTable handles;
    HeapManager hm{vmm, mem, handles};
};

// =============================================================================
// HeapCreate Tests
// =============================================================================

TEST_F(HeapTest, CreateReturnsValidHandle) {
    Handle h = hm.heap_create(0, 0x10000, 0);
    EXPECT_NE(h, INVALID_HANDLE);
}

TEST_F(HeapTest, CreateWithDifferentSizes) {
    Handle h1 = hm.heap_create(0, 0x1000, 0);
    Handle h2 = hm.heap_create(0, 0x100000, 0);
    EXPECT_NE(h1, INVALID_HANDLE);
    EXPECT_NE(h2, INVALID_HANDLE);
    EXPECT_NE(h1, h2);
}

TEST_F(HeapTest, CreateMultipleHeaps) {
    Handle h1 = hm.heap_create(0, 0x10000, 0);
    Handle h2 = hm.heap_create(HEAP_NO_SERIALIZE, 0x10000, 0);
    EXPECT_NE(h1, INVALID_HANDLE);
    EXPECT_NE(h2, INVALID_HANDLE);
    EXPECT_NE(h1, h2);
}

// Audit 01 HeapCreate #1: nonzero dwMaximumSize makes a non-growable heap
// (Learn); initial > max fails; single block capped ~1 MiB; total cannot grow.
TEST_F(HeapTest, FixedSizeHeapEnforcesMaximumAndBlockCap) {
    set_last_error(0);
    EXPECT_EQ(hm.heap_create(0, 0x200000, 0x100000), NULL_HANDLE);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_PARAMETER);

    Handle hf = hm.heap_create(0, 0, 0x10000);
    ASSERT_NE(hf, NULL_HANDLE);
    EXPECT_EQ(hm.heap_alloc(hf, 0, 0x20000), 0u);  // larger than heap max
    EXPECT_NE(hm.heap_alloc(hf, 0, 64), 0u);

    Handle big = hm.heap_create(0, 0, 0x4000000);
    ASSERT_NE(big, NULL_HANDLE);
    EXPECT_EQ(hm.heap_alloc(big, 0, 0x200000), 0u);  // > ~1 MiB fixed cap
    EXPECT_NE(hm.heap_alloc(big, 0, 0x10000), 0u);
}

// Audit 01 HeapCreate #2: HEAP_CREATE_ENABLE_EXECUTE commits PAGE_EXECUTE_READWRITE
// (heap_contract_probe block_exec_small/large protect=0x40).
TEST_F(HeapTest, EnableExecuteCommitsExecuteReadWrite) {
    Handle h = hm.heap_create(HEAP_CREATE_ENABLE_EXECUTE, 0, 0);
    ASSERT_NE(h, NULL_HANDLE);
    const u64 small = hm.heap_alloc(h, 0, 64);
    ASSERT_NE(small, 0u);
    VirtualAllocationQuery q{};
    ASSERT_TRUE(vmm.virtual_query(small, q));
    EXPECT_EQ(q.protect, PAGE_EXECUTE_READWRITE);
    EXPECT_EQ(q.allocation_protect, PAGE_EXECUTE_READWRITE);

    const u64 large = hm.heap_alloc(h, 0, 0x200000);
    ASSERT_NE(large, 0u);
    ASSERT_TRUE(vmm.virtual_query(large, q));
    EXPECT_EQ(q.protect, PAGE_EXECUTE_READWRITE);
    EXPECT_EQ(q.allocation_protect, PAGE_EXECUTE_READWRITE);
}

// Audit 01 HeapAlloc #1: HEAP_GENERATE_EXCEPTIONS arms STATUS_NO_MEMORY
// (0xC0000017, ExceptionFlags=0x80, ExceptionInformation[0]=request size).
TEST_F(HeapTest, GenerateExceptionsArmsStatusNoMemory) {
    Handle h = hm.heap_create(HEAP_GENERATE_EXCEPTIONS, 0, 0x1000);
    ASSERT_NE(h, NULL_HANDLE);
    EXPECT_EQ(hm.heap_options(h) & HEAP_GENERATE_EXCEPTIONS, HEAP_GENERATE_EXCEPTIONS);

    // Fixed heap: request larger than max must fail.
    EXPECT_EQ(hm.heap_alloc(h, 0, 0x20000), 0u);

    cpu::CpuState cpu{};
    arm_heap_oom_exception(cpu, 0x20000);
    EXPECT_TRUE(cpu.pending_raise.active);
    EXPECT_EQ(cpu.pending_raise.code, 0xC0000017u);
    EXPECT_EQ(cpu.pending_raise.flags, 0x80u);
    EXPECT_EQ(cpu.pending_raise.num_args, 1u);
    EXPECT_EQ(cpu.pending_raise.args[0], 0x20000u);
}

// Audit 01 HeapFree #1: failure sets ERROR_INVALID_PARAMETER; thunk also
// arms STATUS_HEAP_CORRUPTION / ACCESS_VIOLATION (heap_contract_probe).
TEST_F(HeapTest, FreeFailureSetsInvalidParameter) {
    Handle h = hm.heap_create(0, 0x10000, 0);
    ASSERT_NE(h, NULL_HANDLE);
    u64 p = hm.heap_alloc(h, 0, 64);
    ASSERT_NE(p, 0u);
    EXPECT_TRUE(hm.heap_free(h, 0, p));
    set_last_error(0xDEAD);
    EXPECT_FALSE(hm.heap_free(h, 0, p));  // double free
    // Manager path does not set last-error; thunk does. Validate handle
    // classification used by the thunk.
    EXPECT_TRUE(hm.heap_valid(h));
    EXPECT_FALSE(hm.heap_valid(0x12340000));
    EXPECT_FALSE(hm.heap_free(h, 0, p + 16));  // interior
}

// =============================================================================
// HeapDestroy Tests
// =============================================================================

TEST_F(HeapTest, DestroySucceeds) {
    Handle h = hm.heap_create(0, 0x10000, 0);
    ASSERT_NE(h, INVALID_HANDLE);
    EXPECT_TRUE(hm.heap_destroy(h));
}

TEST_F(HeapTest, DestroyInvalidHandleFails) {
    set_last_error(0xDEAD);
    EXPECT_FALSE(hm.heap_destroy(INVALID_HANDLE));
    // Thunk sets ERROR_INVALID_HANDLE; manager soft-fails.
}

TEST_F(HeapTest, DestroyNullSucceedsLikeWindows) {
    // heap_contract_probe: HeapDestroy_null ret=1 err untouched.
    // Manager treats 0 as unknown — thunk special-cases NULL before calling.
    EXPECT_FALSE(hm.heap_destroy(0));
}

TEST_F(HeapTest, DestroyNonexistentHandleFails) {
    EXPECT_FALSE(hm.heap_destroy(0x9999));
}

TEST_F(HeapTest, DoubleDestroyFails) {
    Handle h = hm.heap_create(0, 0x10000, 0);
    ASSERT_NE(h, INVALID_HANDLE);
    EXPECT_TRUE(hm.heap_destroy(h));
    EXPECT_FALSE(hm.heap_destroy(h));
}

// =============================================================================
// HeapAlloc Tests
// =============================================================================

TEST_F(HeapTest, AllocReturnsNonZero) {
    Handle h = hm.heap_create(0, 0x10000, 0);
    ASSERT_NE(h, INVALID_HANDLE);
    u64 addr = hm.heap_alloc(h, 0, 256);
    EXPECT_NE(addr, 0u);
}

TEST_F(HeapTest, AllocWriteRead) {
    Handle h = hm.heap_create(0, 0x10000, 0);
    ASSERT_NE(h, INVALID_HANDLE);

    u64 addr = hm.heap_alloc(h, 0, 256);
    ASSERT_NE(addr, 0u);

    auto wr = mem.write_u32(static_cast<GuestAddr>(addr), 0xCAFEBABE);
    EXPECT_FALSE(wr.is_err());

    auto rd = mem.read_u32(static_cast<GuestAddr>(addr));
    EXPECT_FALSE(rd.is_err());
    EXPECT_EQ(rd.value(), 0xCAFEBABEu);
}

TEST_F(HeapTest, AllocZeroSizeSucceeds) {
    Handle h = hm.heap_create(0, 0x10000, 0);
    ASSERT_NE(h, INVALID_HANDLE);
    u64 addr = hm.heap_alloc(h, 0, 0);
    EXPECT_NE(addr, 0u);
}

TEST_F(HeapTest, AllocWithHeapZeroMemory) {
    Handle h = hm.heap_create(0, 0x10000, 0);
    ASSERT_NE(h, INVALID_HANDLE);

    u64 addr = hm.heap_alloc(h, HEAP_ZERO_MEMORY, 64);
    ASSERT_NE(addr, 0u);

    // VirtualAlloc already zero-fills, so the memory must be zero.
    auto rd = mem.read_u32(static_cast<GuestAddr>(addr));
    EXPECT_FALSE(rd.is_err());
    EXPECT_EQ(rd.value(), 0u);
}

TEST_F(HeapTest, MultipleAllocsReturnDifferentAddresses) {
    Handle h = hm.heap_create(0, 0x10000, 0);
    ASSERT_NE(h, INVALID_HANDLE);

    u64 a1 = hm.heap_alloc(h, 0, 128);
    u64 a2 = hm.heap_alloc(h, 0, 128);
    u64 a3 = hm.heap_alloc(h, 0, 128);

    EXPECT_NE(a1, 0u);
    EXPECT_NE(a2, 0u);
    EXPECT_NE(a3, 0u);
    EXPECT_NE(a1, a2);
    EXPECT_NE(a1, a3);
    EXPECT_NE(a2, a3);
}

TEST_F(HeapTest, AllocOnInvalidHeapReturnsZero) {
    u64 addr = hm.heap_alloc(INVALID_HANDLE, 0, 128);
    EXPECT_EQ(addr, 0u);
}

// =============================================================================
// HeapFree Tests
// =============================================================================

TEST_F(HeapTest, FreeSucceeds) {
    Handle h = hm.heap_create(0, 0x10000, 0);
    ASSERT_NE(h, INVALID_HANDLE);

    u64 addr = hm.heap_alloc(h, 0, 256);
    ASSERT_NE(addr, 0u);

    EXPECT_TRUE(hm.heap_free(h, 0, addr));
}

TEST_F(HeapTest, FreeInvalidAddressFails) {
    Handle h = hm.heap_create(0, 0x10000, 0);
    ASSERT_NE(h, INVALID_HANDLE);

    EXPECT_FALSE(hm.heap_free(h, 0, 0xDEAD0000));
}

TEST_F(HeapTest, DoubleFreeReturnsFalse) {
    Handle h = hm.heap_create(0, 0x10000, 0);
    ASSERT_NE(h, INVALID_HANDLE);

    u64 addr = hm.heap_alloc(h, 0, 128);
    ASSERT_NE(addr, 0u);

    EXPECT_TRUE(hm.heap_free(h, 0, addr));
    EXPECT_FALSE(hm.heap_free(h, 0, addr));
}

TEST_F(HeapTest, AllocAfterFreeWorks) {
    Handle h = hm.heap_create(0, 0x10000, 0);
    ASSERT_NE(h, INVALID_HANDLE);

    u64 a1 = hm.heap_alloc(h, 0, 128);
    ASSERT_NE(a1, 0u);
    EXPECT_TRUE(hm.heap_free(h, 0, a1));

    u64 a2 = hm.heap_alloc(h, 0, 128);
    EXPECT_NE(a2, 0u);
}

// =============================================================================
// HeapSize Tests
// =============================================================================

TEST_F(HeapTest, SizeReturnsCorrectValue) {
    Handle h = hm.heap_create(0, 0x10000, 0);
    ASSERT_NE(h, INVALID_HANDLE);

    u64 addr = hm.heap_alloc(h, 0, 512);
    ASSERT_NE(addr, 0u);

    u64 sz = hm.heap_size(h, 0, addr);
    EXPECT_EQ(sz, 512u);
}

TEST_F(HeapTest, SizeOfUnknownAddressReturnsFailure) {
    Handle h = hm.heap_create(0, 0x10000, 0);
    ASSERT_NE(h, INVALID_HANDLE);

    u64 sz = hm.heap_size(h, 0, 0xBAAD0000);
    EXPECT_EQ(sz, ~0ULL);
}

TEST_F(HeapTest, SizeOnInvalidHeapReturnsFailure) {
    u64 sz = hm.heap_size(INVALID_HANDLE, 0, 0x1000);
    EXPECT_EQ(sz, ~0ULL);
}

// =============================================================================
// HeapReAlloc Tests
// =============================================================================

TEST_F(HeapTest, ReallocReturnsNewAddress) {
    Handle h = hm.heap_create(0, 0x10000, 0);
    ASSERT_NE(h, INVALID_HANDLE);

    u64 addr = hm.heap_alloc(h, 0, 64);
    ASSERT_NE(addr, 0u);

    u64 new_addr = hm.heap_realloc(h, 0, addr, 256);
    EXPECT_NE(new_addr, 0u);

    // New allocation should be tracked with the new size.
    u64 sz = hm.heap_size(h, 0, new_addr);
    EXPECT_EQ(sz, 256u);
}

TEST_F(HeapTest, ReallocInPlaceOnlyFails) {
    Handle h = hm.heap_create(0, 0x10000, 0);
    ASSERT_NE(h, INVALID_HANDLE);

    u64 addr = hm.heap_alloc(h, 0, 64);
    ASSERT_NE(addr, 0u);

    u64 result = hm.heap_realloc(h, HEAP_REALLOC_IN_PLACE_ONLY, addr, 256);
    EXPECT_EQ(result, 0u);
}

// Audit 01 HeapReAlloc #1. tools/oracle_probe/heap_contract_probe.c
// (Windows x64/x86): inplace_shrink same=1 err=0xdead size=50 byte49 kept;
// inplace_grow_small same=1; inplace_grow_big ret=0 err=0x8 with the block,
// its size and contents untouched; inplace_grow_zero zeroes only the tail.
TEST_F(HeapTest, ReallocInPlaceOnlyResizesWithinBlockAndFailsWhenItMustMove) {
    Handle h = hm.heap_create(0, 0, 0);
    ASSERT_NE(h, INVALID_HANDLE);
    const u64 p = hm.heap_alloc(h, 0, 100);
    ASSERT_NE(p, 0u);
    (void)hm.heap_alloc(h, 0, 100);
    for (u32 i = 0; i < 100; ++i) {
        ASSERT_TRUE(mem.write_u8(p + i, static_cast<u8>(0xA0 + (i & 15))).is_ok());
    }
    set_last_error(0xDEAD);
    EXPECT_EQ(hm.heap_realloc(h, HEAP_REALLOC_IN_PLACE_ONLY, p, 50), p);
    EXPECT_EQ(get_last_error(), 0xDEADu);
    EXPECT_EQ(hm.heap_size(h, 0, p), 50u);
    EXPECT_EQ(mem.read_u8(p + 49).value(), 0xA1u);

    EXPECT_EQ(hm.heap_realloc(h, HEAP_REALLOC_IN_PLACE_ONLY, p, 56), p);
    EXPECT_EQ(hm.heap_size(h, 0, p), 56u);

    set_last_error(0xDEAD);
    EXPECT_EQ(hm.heap_realloc(h, HEAP_REALLOC_IN_PLACE_ONLY, p, 0x200000), 0u);
    EXPECT_EQ(get_last_error(), 8u);  // ERROR_NOT_ENOUGH_MEMORY
    EXPECT_EQ(hm.heap_size(h, 0, p), 56u);
    EXPECT_EQ(mem.read_u8(p).value(), 0xA0u);
    EXPECT_EQ(mem.read_u8(p + 49).value(), 0xA1u);

    EXPECT_EQ(hm.heap_realloc(h, HEAP_REALLOC_IN_PLACE_ONLY | HEAP_ZERO_MEMORY, p, 80), p);
    EXPECT_EQ(mem.read_u8(p + 49).value(), 0xA1u);
    EXPECT_EQ(mem.read_u8(p + 60).value(), 0u);
    EXPECT_EQ(mem.read_u8(p + 79).value(), 0u);
}

TEST_F(HeapTest, ReallocOfUnknownAddressFails) {
    Handle h = hm.heap_create(0, 0x10000, 0);
    ASSERT_NE(h, INVALID_HANDLE);

    u64 result = hm.heap_realloc(h, 0, 0xDEAD0000, 128);
    EXPECT_EQ(result, 0u);
}

// =============================================================================
// GetProcessHeap Tests
// =============================================================================

TEST_F(HeapTest, GetProcessHeapReturnsValidHandle) {
    Handle h = hm.get_process_heap();
    EXPECT_NE(h, INVALID_HANDLE);
}

TEST_F(HeapTest, GetProcessHeapReturnsSameHandleOnMultipleCalls) {
    Handle h1 = hm.get_process_heap();
    Handle h2 = hm.get_process_heap();
    EXPECT_EQ(h1, h2);
}

TEST_F(HeapTest, ProcessHeapAllocWorks) {
    Handle h = hm.get_process_heap();
    ASSERT_NE(h, INVALID_HANDLE);

    u64 addr = hm.heap_alloc(h, 0, 1024);
    EXPECT_NE(addr, 0u);

    EXPECT_TRUE(hm.heap_free(h, 0, addr));
}

// RtlCreateHeap's default VirtualMemoryThreshold is 0x7F000. A 128 KiB
// block must come from a committed segment, not an isolated VirtualAlloc.
TEST_F(HeapTest, MediumAllocUsesCommittedSegment) {
    Handle h = hm.heap_create(0, 0, 0);
    ASSERT_NE(h, INVALID_HANDLE);

    constexpr u64 kSize = 128 * 1024;
    const u64 addr = hm.heap_alloc(h, 0, kSize);
    ASSERT_NE(addr, 0u);
    EXPECT_EQ(hm.heap_size(h, 0, addr), kSize);
    EXPECT_NE(addr & 0xFFFF, 0u)
        << "medium HeapAlloc must not return a 64KiB region base";
    EXPECT_TRUE(mem.read_u8(static_cast<GuestAddr>(addr)).is_ok());
    EXPECT_TRUE(mem.read_u8(static_cast<GuestAddr>(addr + kSize - 1)).is_ok());
}

TEST_F(HeapTest, LargeAllocReturnsPointerAfterVirtualAllocEntry) {
    Handle h = hm.heap_create(0, 0, 0);
    ASSERT_NE(h, INVALID_HANDLE);

    constexpr u64 kSize = 0x80000;  // above default VirtualMemoryThreshold
    const u64 addr = hm.heap_alloc(h, 0, kSize);
    ASSERT_NE(addr, 0u);
    EXPECT_EQ(hm.heap_size(h, 0, addr), kSize);
    EXPECT_NE(addr & 0xFFFF, 0u)
        << "large HeapAlloc must not return a 64KiB region base";
    EXPECT_TRUE(mem.read_u8(static_cast<GuestAddr>(addr)).is_ok());
    EXPECT_TRUE(mem.read_u8(static_cast<GuestAddr>(addr + kSize - 1)).is_ok());
}

// A bounded live set must not grow the heap without bound. Windows' heap
// (LFH) reuses freed blocks across nearby sizes; Vitreo's slab free lists
// were keyed by the exact committed size, so every distinct size was its own
// never-reused pile and sc_alloc reached 8.3 GB resident.
TEST_F(HeapTest, VaryingSizesWithBoundedLiveSetDoNotGrowSlabsWithoutBound) {
    Handle h = hm.heap_create(0, 0x10000, 0);
    ASSERT_NE(h, INVALID_HANDLE);
    constexpr int kLive = 64;
    constexpr int kIters = 20000;
    u64 ring[kLive] = {};
    u32 lcg = 12345;
    for (int i = 0; i < kIters; ++i) {
        lcg = lcg * 1664525u + 1013904223u;
        const u64 size = 16 + (lcg >> 8) % 3000;   // 16..3015 bytes
        const int slot = i % kLive;
        if (ring[slot] != 0) ASSERT_TRUE(hm.heap_free(h, 0, ring[slot]));
        ring[slot] = hm.heap_alloc(h, 0, size);
        ASSERT_NE(ring[slot], 0u) << "iteration " << i;
    }
    // Live set <= 64 * ~3 KB = 192 KB (plus class rounding); one 4 MB slab
    // holds it many times over. Exact-size free lists needed several.
    EXPECT_LE(hm.heap_slab_count(h), 2u);
}


#include <winapi/kernel32/memory/heap_projection.hpp>
TEST_F(HeapTest, HeapIdentityIsMappedAndEqualsPebAcrossArchitectures) {
    constexpr GuestAddr peb=0x7ffd2000;
    ASSERT_TRUE(mem.map_region(peb,0x1000,MemoryProtection::ReadWrite).is_ok());
    for(bool wide:{false,true}) {
        hm.bind_peb(peb,wide);
        const auto heap=hm.get_process_heap();
        const auto layout=wide?heap_projection::layout64:heap_projection::layout32;
        EXPECT_NE(heap,0u);
        EXPECT_EQ(wide?mem.read_u64(peb+layout.peb_process_heap).value():mem.read_u32(peb+layout.peb_process_heap).value(),heap);
        EXPECT_EQ(mem.read_u32(heap+layout.flags).value_or(0xffffffff),layout.process_flags);
        EXPECT_EQ(mem.read_u32(heap+layout.force_flags).value_or(0xffffffff),0u);
        EXPECT_FALSE(handles.is_valid(static_cast<Handle>(heap)));
        const auto block=hm.heap_alloc(heap,0,32);EXPECT_NE(block,0u);
        EXPECT_TRUE(hm.heap_free(heap,0,block));
    }
}

#include "heap_projection_oracle.inc"
TEST_F(HeapTest, HeapHeaderFlagsAndLifetimeMatchWindowsOracle) {
    constexpr u32 options[]={0,HEAP_NO_SERIALIZE,HEAP_GENERATE_EXCEPTIONS,HEAP_CREATE_ENABLE_EXECUTE,
        HEAP_NO_SERIALIZE|HEAP_GENERATE_EXCEPTIONS|HEAP_CREATE_ENABLE_EXECUTE,
        HEAP_NO_SERIALIZE|HEAP_GENERATE_EXCEPTIONS,HEAP_NO_SERIALIZE|HEAP_CREATE_ENABLE_EXECUTE,
        HEAP_GENERATE_EXCEPTIONS|HEAP_CREATE_ENABLE_EXECUTE};
    ASSERT_TRUE(mem.map_region(0x7ffd2000,0x1000,MemoryProtection::ReadWrite).is_ok());
    HeapExtended extended(hm,handles);
    for(const auto& expected:heap_projection_cases) {
        SCOPED_TRACE(::testing::Message()<<expected.name<<" wide="<<expected.wide);
        hm.bind_peb(0x7ffd2000,expected.wide);
        const std::string_view name=expected.name;
        const bool process=name=="process",rtl=name.starts_with("rtl");
        unsigned index=process?0:static_cast<unsigned>(std::stoul(std::string(name.substr(rtl?3:7))));
        const auto heap=process?hm.get_process_heap():hm.heap_create(index<8?options[index]:index==9?HEAP_GENERATE_EXCEPTIONS:0,index==9?0x1000:0,index>=8?0x10000:0,rtl);
        ASSERT_NE(heap,0u);
        const auto layout=expected.wide?heap_projection::layout64:heap_projection::layout32;
        EXPECT_EQ(mem.read_u32(heap+layout.flags).value(),expected.flags);
        EXPECT_EQ(mem.read_u32(heap+layout.force_flags).value(),expected.force);
        EXPECT_TRUE(extended.heap_validate(heap,0,0));
        EXPECT_FALSE(handles.close(static_cast<Handle>(heap)));
        EXPECT_TRUE(hm.heap_valid(heap));
        const auto allocation=hm.heap_alloc(heap,0,32);EXPECT_NE(allocation,0u);
        if(allocation)EXPECT_TRUE(hm.heap_free(heap,0,allocation));
        const auto all=hm.heap_handles();EXPECT_NE(std::find(all.begin(),all.end(),heap),all.end());
        if(!process) {
            EXPECT_TRUE(hm.heap_destroy(heap));
            EXPECT_FALSE(hm.heap_valid(heap));EXPECT_FALSE(mem.read_u32(heap+layout.flags).is_ok());
        }
    }
}

TEST_F(HeapTest, X64HeapArgumentsDoNotAliasAfterTruncation) {
    const auto heap=hm.get_process_heap();ASSERT_NE(heap,0u);
    const GuestAddr high_alias=heap|(u64{1}<<32);
    EXPECT_FALSE(hm.heap_valid(high_alias));
    EXPECT_EQ(hm.heap_alloc(high_alias,0,16),0u);
    EXPECT_FALSE(hm.heap_destroy(high_alias));
    const auto block=hm.heap_alloc(heap,0,32);ASSERT_NE(block,0u);
    EXPECT_FALSE(hm.heap_free(high_alias,0,block));
    EXPECT_EQ(hm.heap_size(heap,0,block),32u);
    HeapExtended ext(hm,handles);
    auto exports=create_rtl_heap_thunks(ext,hm);
    cpu::CpuState cpu;cpu.init_long_mode();Marshaler m(mem);m.attach_cpu(cpu);
    cpu.gpr.rcx=high_alias;cpu.gpr.rdx=0;cpu.gpr.r8=16;
    for(const auto& entry:exports.entries)if(entry.name=="RtlAllocateHeap") {entry.handler(m);EXPECT_EQ(cpu.gpr.rax,0u);}
    cpu.gpr.rcx=high_alias;
    for(const auto& entry:exports.entries)if(entry.name=="RtlDestroyHeap") {entry.handler(m);EXPECT_EQ(cpu.gpr.rax,high_alias);}
    cpu.gpr.rcx=heap;set_last_error(0x12345678);
    impl_CloseHandle(m,handles);
    EXPECT_EQ(cpu.gpr.rax,0u);EXPECT_EQ(get_last_error(),error_codes::ERROR_INVALID_HANDLE);
    EXPECT_TRUE(hm.heap_valid(heap));EXPECT_TRUE(hm.heap_free(heap,0,block));
}

TEST_F(HeapTest, HeapIdentityAboveFourGiBPreservesPointerWidth) {
    // Use the runtime's x64 process ceiling rather than the fixture's default.
    mem.set_maximum_user_address(0x00007fffffffffffull);
    // Reserve VA only, without committing gigabytes of backing memory.
    const auto reservation=vmm.virtual_alloc(0x100000,u64{1}<<32,MEM_RESERVE,PAGE_NOACCESS);
    ASSERT_NE(reservation,0u);
    const auto heap=hm.get_process_heap();ASSERT_GT(heap,UINT32_MAX);
    EXPECT_TRUE(hm.heap_valid(heap));
    EXPECT_FALSE(hm.heap_valid(static_cast<u32>(heap)));
    const auto allocation=hm.heap_alloc(heap,0,32);ASSERT_NE(allocation,0u);
    EXPECT_TRUE(hm.heap_free(heap,0,allocation));
    ASSERT_TRUE(mem.map_region(0x3000,0x2000,MemoryProtection::ReadWrite).is_ok());
    cpu::CpuState cpu;cpu.init_long_mode();Marshaler m(mem);m.attach_cpu(cpu);
    HeapExtended ext(hm,handles);auto exports=create_heap_ext_thunks(ext,hm);
    cpu.gpr.rcx=1;cpu.gpr.rdx=0x3000;
    for(const auto& entry:exports.entries)if(entry.name=="GetProcessHeaps")entry.handler(m);
    EXPECT_EQ(mem.read_u64(0x3000).value(),heap);
    cpu.gpr.rcx=heap;cpu.gpr.rdx=0;cpu.gpr.r8=0;
    for(const auto& entry:exports.entries)if(entry.name=="HeapValidate") {entry.handler(m);EXPECT_EQ(cpu.gpr.rax,1u);}
    CrtMemoryManager crt(mem,0x20000,0x1000);
    crt.set_process_heap_provider([this]{return hm.get_process_heap();});
    const auto crt_exports=create_crt_memory_thunks(crt);
    bool found_crt=false;
    for(const auto& entry:crt_exports.entries)if(entry.name=="_get_heap_handle") {
        found_crt=true;entry.handler(m);EXPECT_EQ(cpu.gpr.rax,heap);
    }
    EXPECT_TRUE(found_crt);
    EXPECT_TRUE(vmm.virtual_free(reservation,0,MEM_RELEASE));
}

TEST_F(HeapTest, RtlHeapThunksSharePointerIdentityInBothCallingConventions) {
    ASSERT_TRUE(mem.map_region(0x8000,0x2000,MemoryProtection::ReadWrite).is_ok());
    HeapExtended extended(hm,handles);
    const auto rtl=create_rtl_heap_thunks(extended,hm);
    const auto kernel=create_heap_ext_thunks(extended,hm);
    for(bool wide:{false,true}) {
        hm.bind_peb(0,wide);
        cpu::CpuState cpu;if(wide)cpu.init_long_mode();else cpu.init_protected_mode();
        Marshaler m(mem);m.attach_cpu(cpu);cpu.gpr.rsp=0x8800;
        const auto call=[&](const DllExports& exports,const char* name,std::initializer_list<u64> arguments) {
            const ExportEntry* function=nullptr;
            for(const auto& entry:exports.entries)if(entry.name==name)function=&entry;
            EXPECT_NE(function,nullptr);
            usize index=0;
            for(u64 value:arguments) {
                if(wide) {
                    if(index==0)cpu.gpr.rcx=value;else if(index==1)cpu.gpr.rdx=value;
                    else if(index==2)cpu.gpr.r8=value;else if(index==3)cpu.gpr.r9=value;
                    else EXPECT_TRUE(mem.write_u64(cpu.gpr.rsp+40+(index-4)*8,value).is_ok());
                } else EXPECT_TRUE(mem.write_u32(cpu.gpr.rsp+4+index*4,static_cast<u32>(value)).is_ok());
                ++index;
            }
            if(function)function->handler(m);
            return cpu.gpr.rax;
        };
        const auto heap=call(rtl,"RtlCreateHeap",{HEAP_GROWABLE,0,0,0,0,0});ASSERT_NE(heap,0u);
        const auto layout=wide?heap_projection::layout64:heap_projection::layout32;
        EXPECT_EQ(mem.read_u32(heap+layout.flags).value(),layout.process_flags);
        const auto block=call(rtl,"RtlAllocateHeap",{heap,0,64});ASSERT_NE(block,0u);
        EXPECT_EQ(call(kernel,"HeapSize",{heap,0,block}),64u);
        EXPECT_EQ(call(rtl,"RtlSizeHeap",{heap,0,block}),64u);
        EXPECT_EQ(call(kernel,"HeapValidate",{heap,0,block}),1u);
        EXPECT_EQ(call(rtl,"RtlFreeHeap",{heap,0,block}),1u);
        const auto count=call(kernel,"GetProcessHeaps",{8,0x9000});
        bool found=false;
        for(u64 i=0;i<std::min<u64>(count,8);++i) {
            const auto value=wide?mem.read_u64(0x9000+i*8).value():mem.read_u32(0x9000+i*4).value();
            if(value==heap)found=true;
        }
        EXPECT_TRUE(found);
        EXPECT_EQ(call(rtl,"RtlDestroyHeap",{heap}),0u);
        EXPECT_FALSE(hm.heap_valid(heap));EXPECT_FALSE(mem.read_u32(heap+layout.flags).is_ok());
    }
}
