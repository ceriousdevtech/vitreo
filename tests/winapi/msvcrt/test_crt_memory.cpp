/// @file test_crt_memory.cpp
/// @brief Tests for CRT guest-memory operations

#include <gtest/gtest.h>
#include "winapi/kernel32/thread/thread.hpp"
#include <winapi/msvcrt/memory/crt_memory.hpp>
#include "winapi/msvcrt/text/crt_string.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/msvcrt/runtime/cxx_eh.hpp"
#include "core/cpu/state/cpu_state.hpp"
#include "core/memory/guest_memory.hpp"
#include "winapi/kernel32/error.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <mutex>
#include <thread>
#include <vector>

using namespace vitreo;
using namespace vitreo::winapi;

class CrtMemoryTest : public ::testing::Test {
protected:
    GuestMemory memory;
    CrtMemoryManager manager{memory, 0x20000000, 0x10000};
};

namespace {
#include "crt_errno_oracle.inc"
}

TEST_F(CrtMemoryTest, ErrnoAccessorsMatchWindowsBothAbisAndThreads) {
    const auto exports = create_crt_memory_thunks(manager);
    const char* names[] = {"_set_errno", "_set_doserrno", "_get_errno", "_get_doserrno"};
    const u32 inputs[] = {0, 1, 0x80000000u, 0xffffffffu};
    const u32 saved_tid = ThreadManager::get_current_thread_id_static();
    struct RestoreThread { u32 tid; ~RestoreThread() { ThreadManager::set_current_thread_id(tid); } } restore{saved_tid};
    ThreadManager::set_current_thread_id(3001);
    manager.set_errno(0xabcdefu);
    manager.set_doserrno(0x123456u);
    const GuestAddr first_thread = manager.errno_addr();
    ThreadManager::set_current_thread_id(3002);
    EXPECT_NE(manager.errno_addr(), first_thread);
    EXPECT_EQ(manager.get_errno(), 0u);
    EXPECT_EQ(memory.read_u32(manager.doserrno_addr()).value(), 0u);
    unsigned calls=0, seen_errno=0, seen_doserrno=0;
    manager.exchange_invalid_parameter_handler(0x12345678);
    manager.set_guest_caller([&](GuestAddr fn, const u64* args, usize count) -> u64 {
        EXPECT_EQ(fn, 0x12345678u);
        EXPECT_EQ(count, 5u);
        for (usize i=0; i<count; ++i) EXPECT_EQ(args[i], 0u);
        ++calls; seen_errno=manager.get_errno();
        seen_doserrno=memory.read_u32(manager.doserrno_addr()).value();
        return 0;
    });
    for (bool x64 : {false, true}) {
        cpu::CpuState cpu;
        if (x64) cpu.init_long_mode(); else cpu.init_protected_mode();
        cpu.set_sp(0x20003000);
        Marshaler m(memory); m.attach_cpu(cpu);
        for (const auto& row : kErrnoOracle) {
            SCOPED_TRACE(std::to_string(x64)+":"+std::to_string(row[0])+":"+std::to_string(row[1]));
            manager.set_errno(0x1234); manager.set_doserrno(0x5678);
            calls=seen_errno=seen_doserrno=0;
            const GuestAddr out=0x20002000;
            ASSERT_TRUE(memory.write_u32(out, inputs[row[1]]).is_ok());
            const u32 arg=row[0]<2 ? inputs[row[1]] : row[1] ? static_cast<u32>(out) : 0;
            if (x64) cpu.gpr.rcx=arg; else ASSERT_TRUE(memory.write_u32(cpu.sp()+4,arg).is_ok());
            set_last_error(0xdead);
            const auto found=std::find_if(exports.entries.begin(),exports.entries.end(),[&](const ExportEntry& e){return e.name==names[row[0]];});
            ASSERT_NE(found,exports.entries.end());
            EXPECT_EQ(found->stdcall_bytes,0u);
            found->handler(m);
            EXPECT_EQ(static_cast<u32>(cpu.gpr.rax),row[2]);
            EXPECT_EQ(memory.read_u32(out).value(),row[3]);
            EXPECT_EQ(manager.get_errno(),row[4]);
            EXPECT_EQ(memory.read_u32(manager.doserrno_addr()).value(),row[5]);
            EXPECT_EQ(calls,row[6]); EXPECT_EQ(seen_errno,row[7]); EXPECT_EQ(seen_doserrno,row[8]);
            EXPECT_EQ(get_last_error(),row[9]);
        }
    }
    ThreadManager::set_current_thread_id(3001);
    EXPECT_EQ(manager.get_errno(),0xabcdefu);
    EXPECT_EQ(memory.read_u32(manager.doserrno_addr()).value(),0x123456u);
}

// The UCRT exports `malloc`/`free` as thin wrappers over `_malloc_base` /
// `_free_base`, and code compiled against it imports either spelling. They must
// share one heap: a pointer from `malloc` has to be accepted by `_free_base`.
//
// Missing one is worse than it looks. Vitreo derives every `_o_`-prefixed
// downlevel forwarder from an existing export, so no `_free_base` also means no
// `_o__free_base` — and a DLL importing that gets the missing-export stub,
// whose 0xFFFFFFFF is then used as a heap pointer.
TEST_F(CrtMemoryTest, BaseHeapEntryPointsShareTheWrapperHeap) {
    auto exports = create_crt_memory_thunks(manager);
    const auto find = [&](const char* name) {
        return std::find_if(exports.entries.begin(), exports.entries.end(),
                            [&](const ExportEntry& e) { return e.name == name; });
    };
    for (const char* name : {"_malloc_base", "_free_base", "_calloc_base",
                             "_realloc_base", "_msize_base"}) {
        EXPECT_NE(find(name), exports.entries.end())
            << name << " is not exported";
    }

    cpu::CpuState cpu;
    cpu.init_long_mode();
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);

    const auto malloc_wrapper = find("malloc");
    const auto free_base = find("_free_base");
    const auto malloc_base = find("_malloc_base");
    const auto msize_base = find("_msize_base");
    ASSERT_NE(malloc_wrapper, exports.entries.end());
    ASSERT_NE(free_base, exports.entries.end());
    ASSERT_NE(malloc_base, exports.entries.end());
    ASSERT_NE(msize_base, exports.entries.end());

    // A block from the public wrapper must be measurable and freeable through
    // the base entry points, or the two are not the same allocator.
    cpu.gpr.rcx = 0x80;
    cpu.gpr.rax = 0;
    malloc_wrapper->handler(marshaler);
    const GuestAddr from_wrapper = static_cast<GuestAddr>(cpu.gpr.rax);
    ASSERT_NE(from_wrapper, 0u);

    cpu.gpr.rcx = from_wrapper;
    cpu.gpr.rax = 0;
    msize_base->handler(marshaler);
    EXPECT_GE(cpu.gpr.rax, 0x80u);

    cpu.gpr.rcx = from_wrapper;
    free_base->handler(marshaler);

    // And the reverse direction: _malloc_base yields a usable block.
    cpu.gpr.rcx = 0x40;
    cpu.gpr.rax = 0;
    malloc_base->handler(marshaler);
    const GuestAddr from_base = static_cast<GuestAddr>(cpu.gpr.rax);
    ASSERT_NE(from_base, 0u);
    EXPECT_TRUE(memory.write_u32(from_base, 0x5A5A5A5Au).is_ok());
    cpu.gpr.rcx = from_base;
    free_base->handler(marshaler);
}

TEST_F(CrtMemoryTest, QsortUsesGuestComparatorAndPreservesRecords) {
    constexpr GuestAddr base = 0x20001000;
    const u32 records[] = {0xAAAA0003, 0xBBBB0001, 0xCCCC0002};
    ASSERT_TRUE(memory.write_bytes(base, reinterpret_cast<const Byte*>(records),
                                   sizeof(records)).is_ok());

    usize comparisons = 0;
    manager.set_compare_callback([&](GuestAddr compare, GuestAddr lhs, GuestAddr rhs) {
        EXPECT_EQ(compare, 0x12345678u);
        ++comparisons;
        const u32 lhs_value = memory.read_u32(lhs).value() & 0xFFFF;
        const u32 rhs_value = memory.read_u32(rhs).value() & 0xFFFF;
        return static_cast<i32>(lhs_value) - static_cast<i32>(rhs_value);
    });

    manager.crt_qsort(base, 3, sizeof(u32), 0x12345678);

    EXPECT_GT(comparisons, 0u);
    EXPECT_EQ(memory.read_u32(base).value(), 0xBBBB0001u);
    EXPECT_EQ(memory.read_u32(base + 4).value(), 0xCCCC0002u);
    EXPECT_EQ(memory.read_u32(base + 8).value(), 0xAAAA0003u);
}

TEST_F(CrtMemoryTest, QsortDoesNotCallComparatorForSingleElement) {
    bool called = false;
    manager.set_compare_callback([&](GuestAddr, GuestAddr, GuestAddr) {
        called = true;
        return 0;
    });

    manager.crt_qsort(0x20001000, 1, sizeof(u32), 0x12345678);

    EXPECT_FALSE(called);
}

TEST_F(CrtMemoryTest, BsearchFindsMatchingElementViaGuestComparator) {
    constexpr GuestAddr base = 0x20001000;
    constexpr GuestAddr key = 0x20001040;
    const u32 records[] = {1, 3, 5, 7, 9};
    ASSERT_TRUE(memory.write_bytes(base, reinterpret_cast<const Byte*>(records),
                                   sizeof(records)).is_ok());
    ASSERT_TRUE(memory.write_u32(key, 7).is_ok());

    usize comparisons = 0;
    manager.set_compare_callback([&](GuestAddr compare, GuestAddr lhs, GuestAddr rhs) {
        EXPECT_EQ(compare, 0xABCDEF00u);
        ++comparisons;
        const u32 a = memory.read_u32(lhs).value();
        const u32 b = memory.read_u32(rhs).value();
        return static_cast<i32>(a) - static_cast<i32>(b);
    });

    const GuestAddr found =
        manager.crt_bsearch(key, base, 5, sizeof(u32), 0xABCDEF00);
    EXPECT_EQ(found, base + 3 * sizeof(u32));
    EXPECT_GT(comparisons, 0u);

    ASSERT_TRUE(memory.write_u32(key, 4).is_ok());
    EXPECT_EQ(manager.crt_bsearch(key, base, 5, sizeof(u32), 0xABCDEF00), 0u);
}

TEST_F(CrtMemoryTest, OnexitTableInitializesRegistersAndExecutesInReverseOrder) {
    constexpr GuestAddr table = 0x20002000;
    ASSERT_EQ(manager.initialize_onexit_table(table, false), 0);
    EXPECT_EQ(memory.read_u32(table).value(), 0u);
    EXPECT_EQ(memory.read_u32(table + 4).value(), 0u);
    EXPECT_EQ(memory.read_u32(table + 8).value(), 0u);

    ASSERT_EQ(manager.register_onexit_function(table, 0x11111111, false), 0);
    ASSERT_EQ(manager.register_onexit_function(table, 0x22222222, false), 0);

    std::vector<GuestAddr> calls;
    manager.set_void_callback([&](GuestAddr callback) {
        calls.push_back(callback);
    });
    ASSERT_EQ(manager.execute_onexit_table(table, false), 0);

    EXPECT_EQ(calls, (std::vector<GuestAddr>{0x22222222, 0x11111111}));
    EXPECT_EQ(memory.read_u32(table).value(), 0u);
    EXPECT_EQ(memory.read_u32(table + 4).value(), 0u);
    EXPECT_EQ(memory.read_u32(table + 8).value(), 0u);
}

TEST_F(CrtMemoryTest, OnexitTableRejectsInvalidPointers) {
    EXPECT_EQ(manager.initialize_onexit_table(0, false), -1);
    EXPECT_EQ(manager.register_onexit_function(0, 0x11111111, false), -1);
    EXPECT_EQ(manager.register_onexit_function(0x20002000, 0, false), -1);
    EXPECT_EQ(manager.execute_onexit_table(0, false), -1);
}

TEST_F(CrtMemoryTest, CrtAtexitRegistersNonNullCallback) {
    EXPECT_EQ(manager.crt_atexit(0x12345678), 0);
    EXPECT_EQ(manager.crt_atexit(0), -1);
}

TEST(CrtMemoryGrowthTest, AddsArenaWhenInitialArenaIsFull) {
    GuestMemory memory;
    CrtMemoryManager manager{memory, 0x20000000, 0x1000};

    const GuestAddr first = manager.crt_malloc(0x800);
    const GuestAddr second = manager.crt_malloc(0x800);

    ASSERT_NE(first, 0u);
    ASSERT_NE(second, 0u);
    EXPECT_LT(first, 0x20001000u);
    EXPECT_GE(second, 0x20001000u);
    EXPECT_EQ(memory.region_count(), 2u);
    EXPECT_TRUE(memory.write_u32(second, 0xA5A5A5A5u).is_ok());
    EXPECT_EQ(memory.read_u32(second).value(), 0xA5A5A5A5u);
}

TEST(CrtMemoryGrowthTest, GrowthSkipsOccupiedGuestRanges) {
    GuestMemory memory;
    CrtMemoryManager manager{memory, 0x20000000, 0x1000};
    ASSERT_TRUE(memory.map_region(
        0x20001000, 0x1000, MemoryProtection::ReadWrite).is_ok());

    ASSERT_NE(manager.crt_malloc(0x800), 0u);
    const GuestAddr second = manager.crt_malloc(0x800);

    EXPECT_GE(second, 0x20002000u);
    EXPECT_LT(second, 0x20003000u);
}

// Arenas are placed by an upward search from the newest one. Once that search
// reaches the top of the user address range, malloc must still use the free
// space left below it instead of reporting ENOMEM.
TEST(CrtMemoryGrowthTest, GrowthReusesFreeSpaceBelowTheNewestArena) {
    GuestMemory memory;
    memory.set_maximum_user_address(0x000FFFFF);
    CrtMemoryManager manager{memory, 0x00080000, 0x1000};
    // Occupy everything above the initial arena, leaving [0x10000, 0x80000).
    ASSERT_TRUE(memory.map_region(
        0x00081000, 0x7F000, MemoryProtection::ReadWrite).is_ok());

    ASSERT_NE(manager.crt_malloc(0x800), 0u);
    const GuestAddr second = manager.crt_malloc(0x800);

    ASSERT_NE(second, 0u);
    EXPECT_GE(second, 0x10000u);
    EXPECT_LT(second, 0x80000u);
    EXPECT_TRUE(memory.write_u32(second, 0xA5A5A5A5u).is_ok());
    EXPECT_EQ(memory.read_u32(second).value(), 0xA5A5A5A5u);
}

TEST(CrtMemoryGrowthTest, InitialArenaMovesPastOccupiedPreferredRange) {
    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(
        0x20000000, 0x1000, MemoryProtection::ReadWrite).is_ok());
    CrtMemoryManager manager{memory, 0x20000000, 0x1000};
    // errno is per guest thread; the reserved block at the arena start is the
    // one used by code running with no guest thread (tid 0).
    const u32 saved_tid = ThreadManager::get_current_thread_id_static();
    ThreadManager::set_current_thread_id(0);

    const GuestAddr allocation = manager.crt_malloc(64);

    EXPECT_GE(manager.errno_addr(), 0x20001000u);
    EXPECT_LT(manager.errno_addr(), 0x20002000u);
    EXPECT_GT(allocation, manager.errno_addr());
    EXPECT_TRUE(memory.write_u32(allocation, 0xA5A5A5A5u).is_ok());
    ThreadManager::set_current_thread_id(saved_tid);
}

// crt_free() really frees when VITREO_CRT_HEAP_REUSE=1: a block goes back on a
// free list keyed by the capacity it physically owns, and the next request of
// that capacity reuses it.
//
// Default is monotonic (no reuse): recycling addresses exposed live-object
// UAFs in guests (Battle.net locale facets; Sims 3 EIP=0 shortly after
// first_present). Opt in for the RSS win measured on tools/stress sc_alloc
// (4078 MB -> 352 MB peak, checksums identical).
TEST(CrtMemoryGrowthTest, FreeReturnsTheBlockForReuse) {
    struct Guard {
        Guard() { setenv("VITREO_CRT_HEAP_REUSE", "1", 1); }
        ~Guard() { unsetenv("VITREO_CRT_HEAP_REUSE"); }
    } guard;
    GuestMemory memory;
    CrtMemoryManager manager{memory, 0x20000000, 0x1000};

    const GuestAddr first = manager.crt_malloc(64);
    ASSERT_NE(first, 0u);
    manager.crt_free(first);
    const GuestAddr second = manager.crt_malloc(64);
    ASSERT_NE(second, 0u);
    EXPECT_EQ(second, first) << "a freed block of the same capacity must be "
                                "reused rather than growing the arena";

    // A larger request must NOT take that block: the free lists are keyed on
    // exact capacity precisely so a recycled block can never be too small.
    manager.crt_free(second);
    const GuestAddr bigger = manager.crt_malloc(4096);
    ASSERT_NE(bigger, 0u);
    EXPECT_NE(bigger, first);
}

// Shipping default reuses freed blocks, as the Windows heap does; the
// monotonic arena is an opt-in A/B mode (VITREO_CRT_HEAP_REUSE=0).
TEST(CrtMemoryGrowthTest, DefaultReusesAndOptOutIsMonotonic) {
    unsetenv("VITREO_CRT_HEAP_REUSE");
    {
        GuestMemory memory;
        CrtMemoryManager manager{memory, 0x20000000, 0x1000};
        const GuestAddr first = manager.crt_malloc(64);
        manager.crt_free(first);
        const GuestAddr second = manager.crt_malloc(64);
        ASSERT_NE(first, 0u);
        EXPECT_EQ(second, first);
    }
    setenv("VITREO_CRT_HEAP_REUSE", "0", 1);
    {
        GuestMemory memory;
        CrtMemoryManager manager{memory, 0x20000000, 0x1000};
        const GuestAddr first = manager.crt_malloc(64);
        manager.crt_free(first);
        const GuestAddr second = manager.crt_malloc(64);
        ASSERT_NE(first, 0u);
        ASSERT_NE(second, 0u);
        EXPECT_GT(second, first);
    }
    unsetenv("VITREO_CRT_HEAP_REUSE");
}

TEST(CrtMemoryGrowthTest, OversizedRequestFailsAndSetsErrno) {
    GuestMemory memory;
    CrtMemoryManager manager{memory, 0x20000000, 0x1000};

    // Learn malloc: fail only when memory is unavailable or size exceeds
    // _HEAP_MAXREQ (≈ SIZE_MAX). A request of 4 GiB+1 must not be rejected by
    // an artificial u32 ceiling — it may still fail with ENOMEM if the guest
    // address space cannot map it.
    EXPECT_EQ(
        manager.crt_malloc(static_cast<usize>(
            std::numeric_limits<u32>::max()) + 1),
        0u);
    EXPECT_EQ(memory.read_u32(manager.errno_addr()).value(), 12u);

    // SIZE_MAX itself is above _HEAP_MAXREQ (header/alignment overhead).
    EXPECT_EQ(manager.crt_malloc(std::numeric_limits<usize>::max()), 0u);
    EXPECT_EQ(memory.read_u32(manager.errno_addr()).value(), 12u);
}

TEST(CrtMemoryGrowthTest, SizeHeaderStoresFullUsize) {
    GuestMemory memory;
    CrtMemoryManager manager{memory, 0x20000000, 0x10000};
    const GuestAddr p = manager.crt_malloc(100);
    ASSERT_NE(p, 0u);
    ASSERT_GE(p, 8u);
    EXPECT_EQ(memory.read_u64(p - 8).value(), 100u);
}

TEST(CrtMemoryGrowthTest, ConcurrentAllocationsReturnUniqueBlocks) {
    GuestMemory memory;
    CrtMemoryManager manager{memory, 0x20000000, 0x1000};
    std::mutex results_lock;
    std::vector<GuestAddr> results;

    constexpr usize THREAD_COUNT = 4;
    constexpr usize ALLOCATIONS_PER_THREAD = 128;
    std::vector<std::thread> threads;
    threads.reserve(THREAD_COUNT);
    for (usize thread = 0; thread < THREAD_COUNT; ++thread) {
        threads.emplace_back([&] {
            for (usize allocation = 0;
                 allocation < ALLOCATIONS_PER_THREAD; ++allocation) {
                const GuestAddr address = manager.crt_malloc(32);
                std::lock_guard lock(results_lock);
                results.push_back(address);
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }

    ASSERT_EQ(results.size(), THREAD_COUNT * ALLOCATIONS_PER_THREAD);
    EXPECT_EQ(
        std::count(results.begin(), results.end(), GuestAddr{0}), 0);
    std::sort(results.begin(), results.end());
    EXPECT_EQ(std::adjacent_find(results.begin(), results.end()), results.end());
}

TEST(CrtMemoryAbiTest, WcstodReturnsViaX87St0OnX86) {
    // MSVC x86: double returns in ST(0). A prior bug put bits in EAX, so
    // callers' fstp consumed a stale ST0 (e.g. 768.0) instead of the parse.
    constexpr GuestAddr base = 0x10000;
    GuestMemory memory;
    ASSERT_TRUE(memory
                    .map_region(base, 0x1000, MemoryProtection::ReadWrite)
                    .is_ok());
    CrtMemoryManager memory_manager(memory);
    // wcstod is registered with the strtod family (crt_strtod.cpp).
    DllExports exports;
    add_crt_strtod_exports(exports, memory_manager);
    auto entry = std::find_if(
        exports.entries.begin(), exports.entries.end(),
        [](const ExportEntry& e) { return e.name == "wcstod"; });
    ASSERT_NE(entry, exports.entries.end());

    cpu::CpuState cpu;
    cpu.init_protected_mode();
    // Poison ST0 so a wrong ABI (leave ST0 untouched) fails loudly.
    cpu.fpu.push(cpu::Float80(768.0));

    constexpr GuestAddr stack = base + 0x800;
    constexpr GuestAddr str = base;
    constexpr GuestAddr endptr = base + 0x100;
    Marshaler marshaler{memory};
    marshaler.attach_cpu(cpu);
    ASSERT_TRUE(marshaler.write_wide_string(str, "0.5").is_ok());
    ASSERT_TRUE(memory.write_u32(endptr, 0).is_ok());
    cpu.gpr.set32(static_cast<u8>(cpu::RegIndex::ESP),
                  static_cast<u32>(stack));
    ASSERT_TRUE(memory.write_u32(stack, 0xDEADBEEFu).is_ok());       // ret
    ASSERT_TRUE(memory.write_u32(stack + 4, static_cast<u32>(str)).is_ok());
    ASSERT_TRUE(
        memory.write_u32(stack + 8, static_cast<u32>(endptr)).is_ok());

    entry->handler(marshaler);

    EXPECT_NEAR(cpu.fpu.get_st(0).to_double(), 0.5, 1e-12);
    EXPECT_EQ(memory.read_u32(endptr).value(),
              static_cast<u32>(str + 3 * 2));  // consumed "0.5"
}

namespace {

ExportEntry* find_crt_export(DllExports& exports, const char* name) {
    for (auto& entry : exports.entries) {
        if (entry.name == name) return &entry;
    }
    return nullptr;
}

class VcruntimeExceptionAbiTest : public ::testing::Test {
protected:
    static constexpr GuestAddr kBase = 0x10000;

    void SetUp() override {
        ASSERT_TRUE(memory.map_region(
            kBase, 0x2000, MemoryProtection::ReadWrite).is_ok());
        cpu.init_protected_mode();
        cpu.gpr.set32(static_cast<u8>(cpu::RegIndex::ESP),
                      static_cast<u32>(kBase + 0x1800));
        marshaler.attach_cpu(cpu);
        exports = create_crt_memory_thunks(manager);
    }

    void push_arg(usize index, u32 value) {
        const GuestAddr sp = static_cast<GuestAddr>(cpu.gpr.rsp);
        ASSERT_TRUE(memory.write_u32(sp + 4 + index * 4, value).is_ok());
    }

    GuestMemory memory;
    CrtMemoryManager manager{memory, 0x20000000, 0x10000};
    cpu::CpuState cpu;
    Marshaler marshaler{memory};
    DllExports exports;
};

}  // namespace

TEST_F(VcruntimeExceptionAbiTest, StdExceptionCopySharesStaticWhat) {
    auto* entry = find_crt_export(exports, "__std_exception_copy");
    ASSERT_NE(entry, nullptr);

    const GuestAddr what = kBase;
    const GuestAddr from = kBase + 0x40;
    const GuestAddr to = kBase + 0x80;
    ASSERT_TRUE(marshaler.write_ansi_string(what, "bad allocation").is_ok());
    ASSERT_TRUE(memory.write_u32(from, static_cast<u32>(what)).is_ok());
    ASSERT_TRUE(memory.write_u8(from + 4, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(to, 0x11111111u).is_ok());
    ASSERT_TRUE(memory.write_u8(to + 4, 1).is_ok());

    push_arg(0, static_cast<u32>(from));
    push_arg(1, static_cast<u32>(to));
    entry->handler(marshaler);

    EXPECT_EQ(memory.read_u32(to).value(), static_cast<u32>(what));
    EXPECT_EQ(memory.read_u8(to + 4).value(), 0);
}

TEST_F(VcruntimeExceptionAbiTest, StdExceptionCopyOwnsDuplicateWhenDoFree) {
    auto* copy = find_crt_export(exports, "__std_exception_copy");
    auto* destroy = find_crt_export(exports, "__std_exception_destroy");
    ASSERT_NE(copy, nullptr);
    ASSERT_NE(destroy, nullptr);

    const GuestAddr what = kBase;
    const GuestAddr from = kBase + 0x40;
    const GuestAddr to = kBase + 0x80;
    ASSERT_TRUE(marshaler.write_ansi_string(what, "owned").is_ok());
    ASSERT_TRUE(memory.write_u32(from, static_cast<u32>(what)).is_ok());
    ASSERT_TRUE(memory.write_u8(from + 4, 1).is_ok());
    ASSERT_TRUE(memory.write_u32(to, 0).is_ok());
    ASSERT_TRUE(memory.write_u8(to + 4, 0).is_ok());

    push_arg(0, static_cast<u32>(from));
    push_arg(1, static_cast<u32>(to));
    copy->handler(marshaler);

    const u32 copied = memory.read_u32(to).value();
    EXPECT_NE(copied, 0u);
    EXPECT_NE(copied, static_cast<u32>(what));
    EXPECT_EQ(memory.read_u8(to + 4).value(), 1);
    EXPECT_EQ(marshaler.read_ansi_string(copied).value(), "owned");

    push_arg(0, static_cast<u32>(to));
    destroy->handler(marshaler);
    EXPECT_EQ(memory.read_u32(to).value(), 0u);
    EXPECT_EQ(memory.read_u8(to + 4).value(), 0);
}

TEST_F(VcruntimeExceptionAbiTest, UncaughtCountTracksThrowAndCatch) {
    auto* uncaught = find_crt_export(exports, "__uncaught_exception");
    auto* count = find_crt_export(exports, "__uncaught_exceptions");
    ASSERT_NE(uncaught, nullptr);
    ASSERT_NE(count, nullptr);

    const int before = cxx_uncaught_exceptions();
    uncaught->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), before > 0 ? 1u : 0u);

    note_cxx_exception_thrown();
    count->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax),
              static_cast<u32>(before + 1));
    uncaught->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 1u);

    note_cxx_exception_caught();
    count->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), static_cast<u32>(before));
}

TEST_F(VcruntimeExceptionAbiTest, TypeInfoNameCachesDecoratedWithoutDot) {
    auto* entry = find_crt_export(exports, "__std_type_info_name");
    ASSERT_NE(entry, nullptr);

    const GuestAddr data = kBase + 0x100;
    ASSERT_TRUE(memory.write_u32(data, 0).is_ok());
    ASSERT_TRUE(marshaler.write_ansi_string(
        data + 4, ".?AVexception@std@@").is_ok());

    push_arg(0, static_cast<u32>(data));
    push_arg(1, 0);
    entry->handler(marshaler);

    const u32 first = static_cast<u32>(cpu.gpr.rax);
    ASSERT_NE(first, 0u);
    EXPECT_EQ(marshaler.read_ansi_string(first).value(), "?AVexception@std@@");
    EXPECT_EQ(memory.read_u32(data).value(), first);

    entry->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), first);
}

TEST_F(VcruntimeExceptionAbiTest, TypeInfoCompareMatchesDecoratedNames) {
    auto* entry = find_crt_export(exports, "__std_type_info_compare");
    ASSERT_NE(entry, nullptr);

    const GuestAddr lhs = kBase + 0x200;
    const GuestAddr rhs_same = kBase + 0x280;
    const GuestAddr rhs_other = kBase + 0x300;
    ASSERT_TRUE(memory.write_u32(lhs, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(rhs_same, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(rhs_other, 0).is_ok());
    ASSERT_TRUE(marshaler.write_ansi_string(lhs + 4, ".?AVexception@std@@").is_ok());
    ASSERT_TRUE(marshaler.write_ansi_string(
        rhs_same + 4, ".?AVexception@std@@").is_ok());
    ASSERT_TRUE(marshaler.write_ansi_string(rhs_other + 4, ".?AVbad_alloc@std@@").is_ok());

    push_arg(0, static_cast<u32>(lhs));
    push_arg(1, static_cast<u32>(lhs));
    entry->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 0u);

    push_arg(1, static_cast<u32>(rhs_same));
    entry->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 0u);

    push_arg(1, static_cast<u32>(rhs_other));
    entry->handler(marshaler);
    EXPECT_NE(static_cast<u32>(cpu.gpr.rax), 0u);
}

TEST(CrtAllocationExportTest, X64NewReturnsWritableHighGuestPointer) {
    GuestMemory memory;
    CrtMemoryManager manager(memory, 0x140000000ull, 0x10000);
    cpu::CpuState cpu;
    cpu.init_long_mode();
    cpu.set_sp(0x14000f000);
    Marshaler m(memory);
    m.attach_cpu(cpu);
    const auto exports = create_crt_memory_thunks(manager);
    u32 checked = 0;
    for (const auto& entry : exports.entries) {
        if (entry.name != "??2@YAPEAX_K@Z" && entry.name != "??_U@YAPEAX_K@Z") continue;
        cpu.gpr.rcx = 96;
        entry.handler(m);
        const GuestAddr pointer = cpu.gpr.rax;
        EXPECT_GE(pointer, 0x140000000ull);
        EXPECT_EQ(pointer % 16, 0u);
        ASSERT_TRUE(memory.write_u64(pointer+88, 0x1234567887654321).is_ok());
        EXPECT_EQ(memory.read_u64(pointer+88).value(), 0x1234567887654321ull);
        ++checked;
    }
    EXPECT_EQ(checked, 2u);
}
