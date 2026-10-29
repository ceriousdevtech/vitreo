/// @file test_fiber_fls.cpp
/// @brief Tests for FlsManager (WIN32-072)

#include <gtest/gtest.h>
#include <vitreo/common.hpp>
#include <winapi/kernel32/thread/fiber_fls.hpp>
#include <winapi/kernel32/error.hpp>
#include <winapi/kernel32/thread/thread.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>
#include "core/memory/guest_memory.hpp"
#include "winapi/kernel32/memory/memory.hpp"
#include "winapi/core/marshaler.hpp"

using namespace vitreo;
using namespace vitreo::winapi;

// =============================================================================
// Test Fixture
// =============================================================================

class FlsTest : public ::testing::Test {
protected:
    GuestMemory memory;
    VirtualMemoryManager vmm{memory};
    FlsManager mgr;
    void SetUp() override {
        ThreadManager::set_current_thread_id(1);
        mgr.bind_runtime(memory, vmm, 0x70000008);
    }
};

// =============================================================================
// FLS Allocation Tests
// =============================================================================

TEST_F(FlsTest, FlsAlloc_ReturnsValidIndex) {
    u32 idx = mgr.fls_alloc();
    EXPECT_EQ(idx, 0u);
}

TEST_F(FlsTest, FlsAlloc_SequentialIndices) {
    u32 a = mgr.fls_alloc();
    u32 b = mgr.fls_alloc();
    u32 c = mgr.fls_alloc();
    EXPECT_EQ(a, 0u);
    EXPECT_EQ(b, 1u);
    EXPECT_EQ(c, 2u);
}

TEST_F(FlsTest, FlsAlloc_WithCallback) {
    u32 idx = mgr.fls_alloc(0xCAFE);
    EXPECT_EQ(idx, 0u);
}

// =============================================================================
// FLS Get/Set Tests
// =============================================================================

TEST_F(FlsTest, FlsSetGetValue_RoundTrip) {
    u32 idx = mgr.fls_alloc();
    ASSERT_NE(idx, FLS_OUT_OF_INDEXES);
    EXPECT_TRUE(mgr.fls_set_value(idx, 0xDEAD));
    EXPECT_EQ(mgr.fls_get_value(idx), 0xDEADu);
}

TEST_F(FlsTest, FlsGetValue_UnsetReturnsNull) {
    u32 idx = mgr.fls_alloc();
    ASSERT_NE(idx, FLS_OUT_OF_INDEXES);
    EXPECT_EQ(mgr.fls_get_value(idx), 0u);
}

TEST_F(FlsTest, FlsValuesAreIsolatedByGuestThread) {
    ThreadManager::set_current_thread_id(1);
    u32 idx = mgr.fls_alloc();
    ASSERT_NE(idx, FLS_OUT_OF_INDEXES);
    ASSERT_TRUE(mgr.fls_set_value(idx, 0x1111));

    std::thread other([&]() {
        ThreadManager::set_current_thread_id(2);
        EXPECT_EQ(mgr.fls_get_value(idx), 0u);
        EXPECT_TRUE(mgr.fls_set_value(idx, 0x2222));
        EXPECT_EQ(mgr.fls_get_value(idx), 0x2222u);
    });
    other.join();

    EXPECT_EQ(mgr.fls_get_value(idx), 0x1111u);
}

// Get/set memoise this thread's slot block to keep the process-wide lock off
// the hot path. The memo is keyed by guest thread id, so one host thread that
// runs two guest threads must still see two separate sets of values.
TEST_F(FlsTest, FlsValuesFollowGuestThreadIdOnOneHostThread) {
    ThreadManager::set_current_thread_id(1);
    const u32 idx = mgr.fls_alloc();
    ASSERT_NE(idx, FLS_OUT_OF_INDEXES);
    ASSERT_TRUE(mgr.fls_set_value(idx, 0x1111));

    ThreadManager::set_current_thread_id(2);
    EXPECT_EQ(mgr.fls_get_value(idx), 0u);
    ASSERT_TRUE(mgr.fls_set_value(idx, 0x2222));
    EXPECT_EQ(mgr.fls_get_value(idx), 0x2222u);

    ThreadManager::set_current_thread_id(1);
    EXPECT_EQ(mgr.fls_get_value(idx), 0x1111u);
}

// FlsFree zeroes one index across every registered thread. A thread that has
// already memoised its block must still observe the zero.
TEST_F(FlsTest, FlsFreeClearsValueInAnotherThreadsMemoisedBlock) {
    ThreadManager::set_current_thread_id(1);
    const u32 idx = mgr.fls_alloc();
    ASSERT_NE(idx, FLS_OUT_OF_INDEXES);

    std::atomic<bool> stored{false};
    std::atomic<bool> freed{false};
    u64 after_free = 0xFFFFu;
    std::thread other([&]() {
        ThreadManager::set_current_thread_id(2);
        // Warm this thread's memo before the main thread frees the index.
        EXPECT_TRUE(mgr.fls_set_value(idx, 0x2222));
        EXPECT_EQ(mgr.fls_get_value(idx), 0x2222u);
        stored.store(true);
        while (!freed.load()) std::this_thread::yield();
        after_free = mgr.fls_get_value(idx);
    });
    while (!stored.load()) std::this_thread::yield();
    ASSERT_TRUE(mgr.fls_free(idx));
    const u32 reused = mgr.fls_alloc();
    ASSERT_EQ(reused, idx);
    freed.store(true);
    other.join();

    EXPECT_EQ(after_free, 0u);
}

// The contended shape this path exists for: many guest threads doing leaf
// get/set at once. Each must only ever read back its own value.
TEST_F(FlsTest, FlsConcurrentGetSetKeepsPerThreadValues) {
    ThreadManager::set_current_thread_id(1);
    const u32 idx = mgr.fls_alloc();
    ASSERT_NE(idx, FLS_OUT_OF_INDEXES);

    constexpr u32 kThreads = 8;
    constexpr u32 kIterations = 2000;
    std::vector<std::thread> threads;
    std::atomic<u32> mismatches{0};
    for (u32 t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t]() {
            ThreadManager::set_current_thread_id(100 + t);
            const u64 mine = 0x1000u + t;
            for (u32 i = 0; i < kIterations; ++i) {
                if (!mgr.fls_set_value(idx, mine) ||
                    mgr.fls_get_value(idx) != mine) {
                    mismatches.fetch_add(1);
                }
            }
        });
    }
    for (auto& th : threads) th.join();
    EXPECT_EQ(mismatches.load(), 0u);
}

// =============================================================================
// FLS Contention Benchmark
// =============================================================================

// Replica of the shape FlsManager used before the per-thread block: one
// process-global mutex plus a map lookup per leaf call. Kept here so the cost
// of that shape can be measured against the current one in a single build,
// instead of inferring it from a CEF launch (which is not a deterministic
// workload and moves several times more than this change does).
class LegacyFlsStore {
public:
    u64 get(u32 index) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (index >= MAX_FLS_SLOTS || !m_allocated[index]) {
            set_last_error(error_codes::ERROR_INVALID_PARAMETER);
            return 0;
        }
        set_last_error(error_codes::ERROR_SUCCESS);
        return m_thread_values[ThreadManager::get_current_thread_id_static()][index];
    }
    bool set(u32 index, u64 value) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (index >= MAX_FLS_SLOTS || !m_allocated[index]) {
            set_last_error(error_codes::ERROR_INVALID_PARAMETER);
            return false;
        }
        m_thread_values[ThreadManager::get_current_thread_id_static()][index] = value;
        return true;
    }
    void allocate(u32 index) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_allocated[index] = true;
    }

private:
    std::mutex m_mutex;
    std::array<bool, MAX_FLS_SLOTS> m_allocated{};
    std::unordered_map<u32, std::array<u64, MAX_FLS_SLOTS>> m_thread_values;
};

TEST_F(FlsTest, DISABLED_FlsLeafThroughputUnderThreadContention) {
    constexpr u32 kThreads = 8;
    constexpr u32 kIterations = 200000;

    ThreadManager::set_current_thread_id(1);
    const u32 idx = mgr.fls_alloc();
    ASSERT_NE(idx, FLS_OUT_OF_INDEXES);
    LegacyFlsStore legacy;
    legacy.allocate(idx);

    auto run = [&](const char* label, auto&& body) {
        std::vector<std::thread> threads;
        const auto start = std::chrono::steady_clock::now();
        for (u32 t = 0; t < kThreads; ++t) {
            threads.emplace_back([&, t]() {
                ThreadManager::set_current_thread_id(200 + t);
                body(t);
            });
        }
        for (auto& th : threads) th.join();
        const double ns = std::chrono::duration<double, std::nano>(
            std::chrono::steady_clock::now() - start).count();
        const double per_op = ns / (kThreads * kIterations * 2.0);
        std::printf("  %-22s %8.1f ms  %7.1f ns/op\n", label, ns / 1e6, per_op);
        return per_op;
    };

    std::printf("FLS leaf get+set, %u threads x %u iterations:\n",
                kThreads, kIterations);
    const double legacy_ns = run("global mutex + map", [&](u32 t) {
        for (u32 i = 0; i < kIterations; ++i) {
            legacy.set(idx, 0x1000u + t);
            (void)legacy.get(idx);
        }
    });
    const double current_ns = run("per-thread block", [&](u32 t) {
        for (u32 i = 0; i < kIterations; ++i) {
            (void)mgr.fls_set_value(idx, 0x1000u + t);
            (void)mgr.fls_get_value(idx);
        }
    });
    std::printf("  speedup: %.2fx\n", legacy_ns / current_ns);
    EXPECT_GT(legacy_ns, current_ns);
}

// =============================================================================
// FLS Free Tests
// =============================================================================

TEST_F(FlsTest, FlsFree_MakesReusable) {
    u32 first = mgr.fls_alloc();
    EXPECT_EQ(first, 0u);
    EXPECT_TRUE(mgr.fls_free(first));
    u32 reused = mgr.fls_alloc();
    EXPECT_EQ(reused, 0u);
}

TEST_F(FlsTest, FlsFree_InvalidIndex) {
    EXPECT_FALSE(mgr.fls_free(999));
}

TEST_F(FlsTest, FlsFree_UnallocatedIndex) {
    EXPECT_FALSE(mgr.fls_free(0));
}

TEST_F(FlsTest, FlsFree_ClearsValue) {
    u32 idx = mgr.fls_alloc();
    mgr.fls_set_value(idx, 0x1234);
    mgr.fls_free(idx);
    // Re-alloc same slot; value should be zero
    u32 idx2 = mgr.fls_alloc();
    EXPECT_EQ(idx, idx2);
    EXPECT_EQ(mgr.fls_get_value(idx2), 0u);
}

// =============================================================================
// FLS Exhaustion Tests
// =============================================================================

TEST_F(FlsTest, FlsAlloc_FullReturnsOutOfIndexes) {
    for (u32 i = 0; i < MAX_FLS_SLOTS; ++i) {
        u32 idx = mgr.fls_alloc();
        ASSERT_NE(idx, FLS_OUT_OF_INDEXES) << "Ran out at i=" << i;
    }
    u32 overflow = mgr.fls_alloc();
    EXPECT_EQ(overflow, FLS_OUT_OF_INDEXES);
}

// =============================================================================
// FLS Boundary / Error Tests
// =============================================================================

TEST_F(FlsTest, FlsGetValue_OutOfRange) {
    u64 val = mgr.fls_get_value(MAX_FLS_SLOTS);
    EXPECT_EQ(val, 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_PARAMETER);
}

TEST_F(FlsTest, FlsSetValue_OutOfRange) {
    EXPECT_FALSE(mgr.fls_set_value(MAX_FLS_SLOTS, 42));
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_PARAMETER);
}

// =============================================================================
// Fiber Conversion Tests
// =============================================================================

TEST_F(FlsTest, ConvertThreadToFiber_ReturnsNonNull) {
    u64 addr = mgr.convert_thread_to_fiber(0);
    EXPECT_NE(addr, 0u);
}

TEST_F(FlsTest, IsThreadAFiber_FalseBeforeConversion) {
    EXPECT_FALSE(mgr.is_thread_a_fiber());
}

TEST_F(FlsTest, IsThreadAFiber_TrueAfterConversion) {
    (void)mgr.convert_thread_to_fiber(0);
    EXPECT_TRUE(mgr.is_thread_a_fiber());
}

TEST_F(FlsTest, ConvertFiberToThread_Succeeds) {
    (void)mgr.convert_thread_to_fiber(0);
    EXPECT_TRUE(mgr.convert_fiber_to_thread());
    EXPECT_FALSE(mgr.is_thread_a_fiber());
}

TEST_F(FlsTest, ConvertFiberToThread_FailsWhenNotFiber) {
    EXPECT_FALSE(mgr.convert_fiber_to_thread());
}

TEST_F(FlsTest, GetFiberData_ReturnsParameter) {
    (void)mgr.convert_thread_to_fiber(42);
    EXPECT_EQ(mgr.get_fiber_data(), 42u);
}

TEST_F(FlsTest, GetFiberData_ZeroWhenNotFiber) {
    EXPECT_EQ(mgr.get_fiber_data(), 0u);
}

TEST_F(FlsTest, FiberStateIsIsolatedByGuestThread) {
    ThreadManager::set_current_thread_id(1);
    (void)mgr.convert_thread_to_fiber(0x1111);

    std::thread other([&]() {
        ThreadManager::set_current_thread_id(2);
        EXPECT_FALSE(mgr.is_thread_a_fiber());
        EXPECT_EQ(mgr.get_fiber_data(), 0u);
        (void)mgr.convert_thread_to_fiber(0x2222);
        EXPECT_EQ(mgr.get_fiber_data(), 0x2222u);
    });
    other.join();

    EXPECT_TRUE(mgr.is_thread_a_fiber());
    EXPECT_EQ(mgr.get_fiber_data(), 0x1111u);
}

TEST_F(FlsTest, ConvertThreadToFiber_RejectsAlreadyConverted) {
    u64 a = mgr.convert_thread_to_fiber(10);
    u64 b = mgr.convert_thread_to_fiber(20);
    EXPECT_NE(a, 0u);
    EXPECT_EQ(b, 0u);
    EXPECT_EQ(get_last_error(), 1280u);
}


TEST_F(FlsTest, FiberSwitchRestoresStackRegistersAndSeparateFlsOnBothArchitectures) {
    for (bool x64 : {false, true}) {
        SCOPED_TRACE(x64);
        GuestMemory mem;
        VirtualMemoryManager vm{mem};
        FlsManager fibers;
        fibers.bind_runtime(mem, vm, 0x70000008);
        ASSERT_TRUE(mem.map_region(0x700000, 0x20000, MemoryProtection::ReadWrite).is_ok());
        cpu::CpuState state;
        if (x64) state.init_long_mode(); else state.init_protected_mode();
        const u64 teb = 0x710000;
        if (x64) {
            state.seg.gs_desc.base = teb;
            ASSERT_TRUE(mem.write_u64(teb + 8, 0x710000).is_ok());
            ASSERT_TRUE(mem.write_u64(teb + 16, 0x700000).is_ok());
        } else {
            state.seg.fs_desc.base = teb;
            ASSERT_TRUE(mem.write_u32(teb, 0xffffffff).is_ok());
            ASSERT_TRUE(mem.write_u32(teb + 4, 0x710000).is_ok());
            ASSERT_TRUE(mem.write_u32(teb + 8, 0x700000).is_ok());
        }
        state.set_sp(0x70ff00);
        Marshaler m(mem);
        m.attach_cpu(state);
        std::vector<u64> cleaned;
        fibers.set_cleanup_callback([&](GuestAddr callback, u64 value) {
            EXPECT_EQ(callback, 0x404040u);
            cleaned.push_back(value);
        });
        const u32 slot = fibers.fls_alloc(0x404040);
        ASSERT_TRUE(fibers.fls_set_value(slot, 0x1111));
        const u64 main = fibers.convert_thread_to_fiber(0xabc, &state, 1);
        ASSERT_NE(main, 0u);
        const u64 parameter = x64 ? 0x1234567887654321ull : 0x87654321;
        const u64 start = x64 ? 0x140001000ull : 0x401000;
        if (x64) {
            state.gpr.rcx = 0;
            state.gpr.rdx = 0x20000;
            state.gpr.r8 = 1;
            state.gpr.r9 = start;
            ASSERT_TRUE(mem.write_u64(state.sp() + 0x28, parameter).is_ok());
        } else {
            const u32 args[] = {0, 0x20000, 1, static_cast<u32>(start), static_cast<u32>(parameter)};
            for (u32 i = 0; i < 5; ++i)
                ASSERT_TRUE(mem.write_u32(state.sp() + 4 + i*4, args[i]).is_ok());
        }
        fibers.create_fiber(m, true);
        const u64 child = state.gpr.rax;
        ASSERT_NE(child, 0u);
        EXPECT_EQ(fibers.get_fiber_data(), 0xabcu);
        EXPECT_EQ(fibers.fls_get_value(slot), 0x1111u);

        auto switch_fiber = [&](u64 address, u64 continuation) {
            // Model CALL plus the x86 stack argument before entering a thunk.
            state.set_sp(state.sp() - 8);
            if (x64) {
                state.gpr.rcx = address;
                EXPECT_TRUE(mem.write_u64(state.sp(), continuation).is_ok());
            } else {
                EXPECT_TRUE(mem.write_u32(state.sp(), static_cast<u32>(continuation)).is_ok());
                EXPECT_TRUE(mem.write_u32(state.sp()+4, static_cast<u32>(address)).is_ok());
            }
            state.thunk_control_transfer = false;
            fibers.switch_to_fiber(m);
            EXPECT_TRUE(state.thunk_control_transfer);
        };
        state.gpr.rbx = 0x11112222;
        switch_fiber(child, 0x402000);
        EXPECT_EQ(state.rip, start);
        EXPECT_NE(state.sp(), 0x70ff00u);
        EXPECT_EQ(fibers.get_fiber_data(), parameter);
        if (x64) EXPECT_EQ(state.gpr.rcx, parameter);
        else EXPECT_EQ(mem.read_u32(state.sp()+4).value(), parameter);
        EXPECT_EQ(fibers.fls_get_value(slot), 0u);
        EXPECT_TRUE(fibers.fls_set_value(slot, 0x2222));
        const u64 child_sp = state.sp();
        state.gpr.rbx = 0x33334444;
        switch_fiber(main, 0x403000);
        EXPECT_EQ(state.rip, 0x402000u);
        EXPECT_EQ(state.sp(), 0x70ff00u);
        EXPECT_EQ(state.gpr.rbx, 0x11112222u);
        EXPECT_EQ(fibers.fls_get_value(slot), 0x1111u);
        const u64 current_field = x64 ? mem.read_u64(teb+0x20).value() : mem.read_u32(teb+0x10).value();
        EXPECT_EQ(current_field, main);
        switch_fiber(child, 0x404000);
        EXPECT_EQ(state.rip, 0x403000u);
        EXPECT_EQ(state.sp(), child_sp);
        EXPECT_EQ(state.gpr.rbx, 0x33334444u);
        EXPECT_EQ(fibers.fls_get_value(slot), 0x2222u);
        switch_fiber(main, 0x405000);
        if (x64) state.gpr.rcx = child;
        else ASSERT_TRUE(mem.write_u32(state.sp()+4, static_cast<u32>(child)).is_ok());
        fibers.delete_fiber(m);
        EXPECT_TRUE(mem.read_u32(child).is_err());
        EXPECT_EQ(cleaned, std::vector<u64>{0x2222});
        if (x64) state.gpr.rcx = main;
        else ASSERT_TRUE(mem.write_u32(state.sp()+4, static_cast<u32>(main)).is_ok());
        fibers.delete_fiber(m);
        EXPECT_EQ(state.rip, 0x70000008u);
        if (x64) EXPECT_EQ(state.gpr.rcx, 0u);
        else EXPECT_EQ(mem.read_u32(state.sp()+4).value(), 0u);
        EXPECT_TRUE(fibers.convert_fiber_to_thread(&state));
    }
}

// CreateFiber / CreateFiberEx stack size: the reservation each (commit,
// reserve) pair gets on Windows 11, from tests/oracle_data/
// fiber_stack_size_probe{,.x86}.expected (the probe image's header says
// SizeOfStackReserve 0x200000, SizeOfStackCommit 0x1000). A job system that
// asks for commit == reserve == 128 KB gets 1 MB, and code with a 129 KB
// frame relies on it.
TEST_F(FlsTest, FiberStackSizeFollowsWindowsReserveRules) {
    struct Case {
        bool extended;
        u64 commit;
        u64 reserve;
        u64 reservation;
    };
    const Case cases[] = {
        {false, 0, 0, 0x200000},           {false, 0x20000, 0, 0x200000},
        {false, 0x200000, 0, 0x200000},    {true, 0x20000, 0x20000, 0x100000},
        {true, 0x100000, 0x100000, 0x100000}, {true, 0x100001, 0x100001, 0x200000},
        {true, 0, 0x1000, 0x100000},       {true, 0, 0x8000, 0x10000},
        {true, 0, 0x20000, 0x20000},       {true, 0, 0x100001, 0x110000},
        {true, 0x1000, 0x20000, 0x20000},  {true, 0x20000, 0x10000, 0x100000},
        {true, 0x30000, 0x200000, 0x200000},
    };
    for (bool x64 : {false, true}) {
        for (const Case& c : cases) {
            SCOPED_TRACE(::testing::Message() << (x64 ? "x64" : "x86") << " ex=" << c.extended
                                              << " commit=" << std::hex << c.commit
                                              << " reserve=" << c.reserve);
            GuestMemory mem;
            VirtualMemoryManager vm{mem};
            FlsManager fibers;
            fibers.bind_runtime(mem, vm, 0x70000008);
            ASSERT_TRUE(mem.map_region(0x700000, 0x30000, MemoryProtection::ReadWrite).is_ok());
            cpu::CpuState state;
            if (x64) state.init_long_mode(); else state.init_protected_mode();
            const u64 teb = 0x710000, peb = 0x711000, image = 0x720000;
            const u64 optional = image + 0x80 + 24;
            ASSERT_TRUE(mem.write_u32(image + 0x3c, 0x80).is_ok());
            if (x64) {
                state.seg.gs_desc.base = teb;
                ASSERT_TRUE(mem.write_u64(teb + 0x60, peb).is_ok());
                ASSERT_TRUE(mem.write_u64(peb + 0x10, image).is_ok());
                ASSERT_TRUE(mem.write_u64(optional + 72, 0x200000).is_ok());
                ASSERT_TRUE(mem.write_u64(optional + 80, 0x1000).is_ok());
            } else {
                state.seg.fs_desc.base = teb;
                ASSERT_TRUE(mem.write_u32(teb + 0x30, static_cast<u32>(peb)).is_ok());
                ASSERT_TRUE(mem.write_u32(peb + 0x08, static_cast<u32>(image)).is_ok());
                ASSERT_TRUE(mem.write_u32(optional + 72, 0x200000).is_ok());
                ASSERT_TRUE(mem.write_u32(optional + 76, 0x1000).is_ok());
            }
            state.set_sp(0x70ff00);
            Marshaler m(mem);
            m.attach_cpu(state);
            ASSERT_NE(fibers.convert_thread_to_fiber(0, &state, 0), 0u);
            const u64 start = x64 ? 0x140001000ull : 0x401000;
            const std::vector<u64> args = c.extended
                ? std::vector<u64>{c.commit, c.reserve, 0, start, 0}
                : std::vector<u64>{c.commit, start, 0};
            if (x64) {
                state.gpr.rcx = args[0];
                state.gpr.rdx = args[1];
                state.gpr.r8 = args[2];
                if (args.size() > 3) state.gpr.r9 = args[3];
                if (args.size() > 4) ASSERT_TRUE(mem.write_u64(state.sp() + 0x28, args[4]).is_ok());
            } else {
                for (u32 i = 0; i < args.size(); ++i)
                    ASSERT_TRUE(mem.write_u32(state.sp() + 4 + i * 4, static_cast<u32>(args[i])).is_ok());
            }
            fibers.create_fiber(m, c.extended);
            const u64 child = state.gpr.rax;
            ASSERT_NE(child, 0u);
            // Enter the fiber: its TEB stack bounds span the reservation
            // (Vitreo backs the whole reservation, so StackLimit is its bottom).
            state.set_sp(state.sp() - 8);
            if (x64) {
                state.gpr.rcx = child;
                ASSERT_TRUE(mem.write_u64(state.sp(), 0x402000).is_ok());
            } else {
                ASSERT_TRUE(mem.write_u32(state.sp(), 0x402000).is_ok());
                ASSERT_TRUE(mem.write_u32(state.sp() + 4, static_cast<u32>(child)).is_ok());
            }
            fibers.switch_to_fiber(m);
            const u64 base = x64 ? mem.read_u64(teb + 8).value() : mem.read_u32(teb + 4).value();
            const u64 limit = x64 ? mem.read_u64(teb + 16).value() : mem.read_u32(teb + 8).value();
            EXPECT_EQ(base - limit, c.reservation);
        }
    }
}
