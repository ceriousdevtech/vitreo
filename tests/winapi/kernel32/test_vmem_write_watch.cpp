/// @file test_vmem_write_watch.cpp
/// @brief Unit tests for MEM_WRITE_WATCH, GetWriteWatch and ResetWriteWatch, including JIT and atomic stores (WIN32-020)

#include "test_vmem_fixture.hpp"
#include <core/cpu/decode/decoder.hpp>
#include <core/cpu/jit/block_cache.hpp>
#include <core/cpu/jit/jit_compiler.hpp>
#include <core/cpu/jit/jit_helpers.hpp>
#include <core/cpu/jit/linear_fault.hpp>
#include <core/cpu/jit/stencils.hpp>
#include <core/memory/write_watch_traps.hpp>
#include <array>
#include <atomic>
#include <cstring>
#include <vector>

TEST_F(VirtualMemoryTest, GetWriteWatchRequiresWriteWatchAllocation) {
    u64 plain = vmm.virtual_alloc(0, 0x1000, MEM_COMMIT | MEM_RESERVE,
                                  PAGE_READWRITE);
    ASSERT_NE(plain, 0u);
    u64 count = 8;
    u32 granularity = 0;
    std::vector<u64> pages;
    EXPECT_NE(vmm.get_write_watch(plain, 0x1000, count, granularity, pages),
              0u);

    u64 watched = vmm.virtual_alloc(0, 0x1000,
                                    MEM_COMMIT | MEM_RESERVE | MEM_WRITE_WATCH,
                                    PAGE_READWRITE);
    ASSERT_NE(watched, 0u);
    count = 8;
    granularity = 0;
    EXPECT_EQ(vmm.get_write_watch(watched, 0x1000, count, granularity, pages),
              0u);
    // Learn: a committed watch page is not dirty until a store.
    EXPECT_EQ(count, 0u);
    EXPECT_TRUE(pages.empty());
    EXPECT_EQ(granularity, 4096u);
    EXPECT_EQ(vmm.reset_write_watch(watched, 0x1000), 0u);
}

TEST_F(VirtualMemoryTest, GetWriteWatchReportsX64JitStoreAboveFourGiB) {
    constexpr GuestAddr code_base = 0x140000000ull;
    constexpr GuestAddr watched = 0x240000000ull;
    constexpr GuestAddr stack = 0x340000000ull;
    constexpr usize reservation = usize{16} * 1024 * 1024 * 1024;
    GuestMemory linear_memory;
    ASSERT_TRUE(linear_memory.enable_linear_reservation(reservation));
    cpu::jit::LinearReservationRegistration fault_registration;
    ASSERT_TRUE(fault_registration.acquire(linear_memory.linear_base(),
                                           reservation));
    VirtualMemoryManager linear_vmm{linear_memory, reservation - 1};
    ASSERT_TRUE(linear_memory.map_region(
        code_base, 0x1000, MemoryProtection::ReadWriteExecute));
    ASSERT_EQ(linear_vmm.virtual_alloc(
                  watched, 0x2000,
                  MEM_COMMIT | MEM_RESERVE | MEM_WRITE_WATCH,
                  PAGE_READWRITE),
              watched);
    ASSERT_TRUE(linear_memory.map_region(
        stack, 0x2000, MemoryProtection::ReadWrite));

    // mov dword ptr [rbx], eax
    const std::array<u8, 2> code{0x89, 0x03};
    ASSERT_TRUE(linear_memory.write_bytes(code_base, code.data(), code.size()));
    cpu::MemoryByteReader reader(code.data(), code.size());
    cpu::DecodedInstruction inst;
    ASSERT_EQ(cpu::decode_instruction(reader, cpu::DecodeMode::Mode64, inst),
              cpu::DecodeError::Success);
    cpu::jit::BasicBlock block;
    block.start_addr = code_base;
    block.is_64bit = true;
    block.instructions.push_back(inst);
    block.byte_length = static_cast<u32>(code.size());
    block.instruction_count = 1;

    setenv("VITREO_JIT_LINEAR64", "1", 1);
    cpu::jit::JitCompiler jit(linear_memory);
    cpu::jit::CompiledBlock* compiled = jit.compile(block);
    ASSERT_NE(compiled, nullptr);

    const usize word_count = compiled->code_size / sizeof(u32);
    std::vector<u32> emitted(word_count);
    std::memcpy(emitted.data(), compiled->code,
                word_count * sizeof(u32));
    // The direct linear64 store is `ADD X8, linear_base, Xaddr` followed by
    // the ordered store through X8 -- NOT the single base+index
    // `STR Wn, [X14, W9, UXTW]` this scanned for before. Two separate changes
    // moved it: the store now goes through a computed address so the TSO
    // lowering can decorate it, and TSO fault patching (default since
    // 2026-08-27) emits a patch marker plus one ordered store instead of an
    // inline ordered/plain diamond. The old matcher therefore reported "test
    // did not compile the direct linear64 store path" while that path was
    // exactly what had been compiled -- a reached-ness guard that had gone
    // blind rather than a regression it caught.
    bool saw_direct_store = false;
    for (usize i = 0; i + 2 < emitted.size(); ++i) {
        if (emitted[i] != cpu::jit::arm64::add_x(
                8, cpu::jit::REG_LINEAR_BASE, 9)) {
            continue;
        }
        // Either TSO form is the direct path; only the helper call is not.
        const u32 op = emitted[i + 1] & ~0x1Fu;
        const u32 op2 = emitted[i + 2] & ~0x1Fu;
        if (op == (cpu::jit::arm64::stlr_w(0, 8) & ~0x1Fu) ||
            op == (cpu::jit::arm64::str_w_uoff(0, 8, 0) & ~0x1Fu) ||
            (emitted[i + 1] == cpu::jit::kTsoPatchMarker &&
             (op2 == (cpu::jit::arm64::stlr_w(0, 8) & ~0x1Fu) ||
              op2 == (cpu::jit::arm64::str_w_uoff(0, 8, 0) & ~0x1Fu)))) {
            saw_direct_store = true;
            break;
        }
    }
    EXPECT_TRUE(saw_direct_store)
        << "test did not compile the direct linear64 store path";

    // A watched page routes the store to the checked path through
    // kLinearCheckWatched in the per-host-page check byte, not through a
    // process-global slot load. The byte must actually carry the bit, and the
    // store must actually test the byte and branch on it.
    const u8* check = linear_memory.linear_check_page_map();
    ASSERT_NE(check, nullptr);
    EXPECT_NE(check[watched >> kLinearHostPageShift] & kLinearCheckWatched, 0u)
        << "publishing a MEM_WRITE_WATCH allocation did not set the check bit";
    bool saw_live_watch_guard = false;
    const u32 check_byte_load = cpu::jit::arm64::ldrb_reg(
        3, cpu::jit::REG_LINEAR64_CHECK_MAP, 4);
    // A multi-byte operand may straddle a 16 KiB host page, so the store loads
    // BOTH endpoints' check bytes (into X8 and X3) and ORs them together
    // before the branch -- see emit_linear64_check_page_test. The guard word is
    // therefore at i+1 for a byte store and at i+2 for this 4-byte one. The
    // matcher previously only accepted i+1, so it went blind on exactly the
    // width this test compiles; it is the reached-ness check that must follow
    // the emitter, not the emitter that owes it a fixed shape. The branch
    // itself is always CBNZ X3 for a store (TBNZ's imm14 form is load-only),
    // so the encoding assertion below stays exact.
    const u32 merge_both_endpoints = cpu::jit::arm64::orr_x(3, 3, 8);
    const auto is_cbnz_x3 = [](u32 word) {
        return (word & 0xFF00001Fu) == 0xB5000003u;
    };
    for (usize i = 0; i + 1 < emitted.size(); ++i) {
        if (emitted[i] != check_byte_load) continue;
        if (is_cbnz_x3(emitted[i + 1]) ||
            (i + 2 < emitted.size() &&
             emitted[i + 1] == merge_both_endpoints &&
             is_cbnz_x3(emitted[i + 2]))) {
            saw_live_watch_guard = true;
            break;
        }
    }
    EXPECT_TRUE(saw_live_watch_guard)
        << "linear64 store does not branch to the checked write-watch path";
    ASSERT_EQ(linear_vmm.reset_write_watch(watched, 0x2000), 0u);

    cpu::CpuState state;
    state.init_long_mode();
    state.rip = code_base;
    state.gpr.rsp = stack + 0x1f00;
    state.gpr.rbx = watched + 0x1040;
    state.gpr.rax = 0xA5A5A5A5u;
    (void)jit.execute(*compiled, state);
    unsetenv("VITREO_JIT_LINEAR64");
    ASSERT_FALSE(state.pending_raise.active);

    u64 count = 4;
    u32 granularity = 0;
    std::vector<u64> pages;
    ASSERT_EQ(linear_vmm.get_write_watch(
                  watched, 0x2000, count, granularity, pages),
              0u);
    ASSERT_EQ(pages.size(), 1u);
    EXPECT_EQ(pages[0], watched + 0x1000);
    EXPECT_EQ(granularity, 4096u);
}

TEST_F(VirtualMemoryTest, GetWriteWatchReportsOnlyCommittedPages) {
    // Reserve four pages but commit only the second: a page that cannot have
    // been written must not be reported.
    u64 watched = vmm.virtual_alloc(0, 0x4000,
                                    MEM_RESERVE | MEM_WRITE_WATCH,
                                    PAGE_READWRITE);
    ASSERT_NE(watched, 0u);
    ASSERT_NE(vmm.virtual_alloc(watched + 0x1000, 0x1000, MEM_COMMIT,
                                PAGE_READWRITE),
              0u);

    ASSERT_TRUE(mem.write_u8(static_cast<GuestAddr>(watched + 0x1000), 0x5A)
                    .is_ok());

    u64 count = 16;
    u32 granularity = 0;
    std::vector<u64> pages;
    EXPECT_EQ(vmm.get_write_watch(watched, 0x4000, count, granularity, pages),
              0u);
    EXPECT_EQ(count, 1u);
    ASSERT_EQ(pages.size(), 1u);
    EXPECT_EQ(pages[0], watched + 0x1000);
}

// A LOCK-prefixed read-modify-write is a store, and both the interpreter and
// the JIT execute it by writing straight through a host pointer rather than
// through GuestMemory. Nothing else on that path marks the page, so unless the
// atomic helpers record it themselves the page is reported clean. A managed GC
// reading GetWriteWatch then concludes the object was untouched and can free
// something still live — the corruption is silent and arbitrarily delayed,
// which is why this is pinned by a test rather than left to inspection.
TEST_F(VirtualMemoryTest, GetWriteWatchReportsLockedAtomicStores) {
    u64 watched = vmm.virtual_alloc(0, 0x2000,
                                    MEM_RESERVE | MEM_COMMIT | MEM_WRITE_WATCH,
                                    PAGE_READWRITE);
    ASSERT_NE(watched, 0u);

    // Clear anything the allocation itself recorded.
    ASSERT_EQ(vmm.reset_write_watch(watched, 0x2000), 0u);
    {
        u64 count = 16;
        u32 granularity = 0;
        std::vector<u64> pages;
        ASSERT_EQ(
            vmm.get_write_watch(watched, 0x2000, count, granularity, pages),
            0u);
        ASSERT_EQ(pages.size(), 0u) << "baseline should start clean";
    }

    // Exactly what the locked helpers do: an atomic RMW through the host
    // mapping, with the write recorded explicitly.
    const GuestAddr target = static_cast<GuestAddr>(watched + 0x1000);
    Byte* host = mem.host_ptr_mut(target, sizeof(u32));
    ASSERT_NE(host, nullptr);
    std::atomic_ref<u32> cell(*reinterpret_cast<u32*>(host));
    cell.fetch_add(1u, std::memory_order_seq_cst);
    write_watch::record_store(target, sizeof(u32));

    u64 count = 16;
    u32 granularity = 0;
    std::vector<u64> pages;
    EXPECT_EQ(vmm.get_write_watch(watched, 0x2000, count, granularity, pages),
              0u);
    ASSERT_EQ(pages.size(), 1u)
        << "an interlocked store must appear in the dirty set";
    EXPECT_EQ(pages[0], watched + 0x1000);
}

TEST_F(VirtualMemoryTest, GetWriteWatchIgnoresLinearMarkOnReservedPage) {
    // A JIT-style page mark on a reserved page is not a Windows write.
    // Dirty-first still has to consult the committed run.
    u64 watched = vmm.virtual_alloc(0, 0x4000,
                                    MEM_RESERVE | MEM_WRITE_WATCH,
                                    PAGE_READWRITE);
    ASSERT_NE(watched, 0u);
    ASSERT_NE(vmm.virtual_alloc(watched + 0x1000, 0x1000, MEM_COMMIT,
                                PAGE_READWRITE),
              0u);

    write_watch::mark_linear_pages(watched, 1);

    u64 count = 16;
    u32 granularity = 0;
    std::vector<u64> pages;
    EXPECT_EQ(vmm.get_write_watch(watched, 0x4000, count, granularity, pages),
              0u);
    // Reserved pages stay excluded. An unwritten committed page is clean.
    EXPECT_EQ(count, 0u);
    EXPECT_TRUE(pages.empty());
}

TEST_F(VirtualMemoryTest, GetWriteWatchFailsWhenCallerArrayIsTooSmall) {
    u64 watched = vmm.virtual_alloc(0, 0x4000,
                                    MEM_COMMIT | MEM_RESERVE | MEM_WRITE_WATCH,
                                    PAGE_READWRITE);
    ASSERT_NE(watched, 0u);
    for (u32 i = 0; i < 4; ++i) {
        ASSERT_TRUE(mem.write_u8(static_cast<GuestAddr>(watched + i * 0x1000),
                                 static_cast<u8>(0x10 + i))
                        .is_ok());
    }

    // Four dirty pages reported into a two-element array: truncating would
    // hide written pages, so the call has to fail instead.
    u64 count = 2;
    u32 granularity = 0;
    std::vector<u64> pages;
    EXPECT_NE(vmm.get_write_watch(watched, 0x4000, count, granularity, pages),
              0u);
    EXPECT_TRUE(pages.empty());

    count = 4;
    EXPECT_EQ(vmm.get_write_watch(watched, 0x4000, count, granularity, pages),
              0u);
    EXPECT_EQ(count, 4u);
}

TEST_F(VirtualMemoryTest, GetWriteWatchFlagResetClearsDirtySet) {
    u64 watched = vmm.virtual_alloc(0, 0x2000,
                                    MEM_COMMIT | MEM_RESERVE | MEM_WRITE_WATCH,
                                    PAGE_READWRITE);
    ASSERT_NE(watched, 0u);
    ASSERT_TRUE(mem.write_u8(static_cast<GuestAddr>(watched), 0xA1).is_ok());

    u64 count = 8;
    u32 granularity = 0;
    std::vector<u64> pages;
    EXPECT_EQ(vmm.get_write_watch(watched, 0x2000, count, granularity, pages,
                                  WRITE_WATCH_FLAG_RESET),
              0u);
    EXPECT_EQ(count, 1u);
    ASSERT_EQ(pages.size(), 1u);
    EXPECT_EQ(pages[0], watched);

    count = 8;
    pages.clear();
    EXPECT_EQ(vmm.get_write_watch(watched, 0x2000, count, granularity, pages),
              0u);
    EXPECT_EQ(count, 0u);
    EXPECT_TRUE(pages.empty());

    ASSERT_TRUE(mem.write_u8(static_cast<GuestAddr>(watched + 0x1000), 0xA2)
                    .is_ok());
    count = 8;
    EXPECT_EQ(vmm.get_write_watch(watched, 0x2000, count, granularity, pages),
              0u);
    EXPECT_EQ(count, 1u);
    ASSERT_EQ(pages.size(), 1u);
    EXPECT_EQ(pages[0], watched + 0x1000);

    EXPECT_EQ(vmm.reset_write_watch(watched, 0x2000), 0u);
    count = 8;
    pages.clear();
    EXPECT_EQ(vmm.get_write_watch(watched, 0x2000, count, granularity, pages),
              0u);
    EXPECT_EQ(count, 0u);
    EXPECT_TRUE(pages.empty());
}

TEST_F(VirtualMemoryTest, GetWriteWatchOmitsUnwrittenSiblingPage) {
    u64 watched = vmm.virtual_alloc(0, 0x2000,
                                    MEM_COMMIT | MEM_RESERVE | MEM_WRITE_WATCH,
                                    PAGE_READWRITE);
    ASSERT_NE(watched, 0u);
    ASSERT_TRUE(mem.write_u8(static_cast<GuestAddr>(watched), 0x11).is_ok());

    u64 count = 8;
    u32 granularity = 0;
    std::vector<u64> pages;
    EXPECT_EQ(vmm.get_write_watch(watched, 0x2000, count, granularity, pages),
              0u);
    EXPECT_EQ(count, 1u);
    ASSERT_EQ(pages.size(), 1u);
    EXPECT_EQ(pages[0], watched);
}

TEST_F(VirtualMemoryTest, GetWriteWatchReportsStoreOfIdenticalBytes) {
    // Learn: GetWriteWatch reports pages that were written, not pages whose
    // contents changed. A store of the same value still dirties the page.
    u64 watched = vmm.virtual_alloc(0, 0x1000,
                                    MEM_COMMIT | MEM_RESERVE | MEM_WRITE_WATCH,
                                    PAGE_READWRITE);
    ASSERT_NE(watched, 0u);
    ASSERT_TRUE(mem.write_u8(static_cast<GuestAddr>(watched), 0).is_ok());

    u64 count = 8;
    u32 granularity = 0;
    std::vector<u64> pages;
    EXPECT_EQ(vmm.get_write_watch(watched, 0x1000, count, granularity, pages),
              0u);
    EXPECT_EQ(count, 1u);
    ASSERT_EQ(pages.size(), 1u);
    EXPECT_EQ(pages[0], watched);
}

TEST(VirtualMemoryLinearTest, GetWriteWatchHostPtrMutSpansHostPage) {
    // CRT memset goes through host_ptr_mut then libc memset. Apple Silicon
    // host pages are 16 KiB; a store that spans two host pages must restore
    // write permission on both or the second page SIGBUS-es (Learn:
    // GetWriteWatch still has to report every written guest page).
    GuestMemory memory;
    ASSERT_TRUE(memory.enable_linear_reservation(0x01000000));
    VirtualMemoryManager manager{memory, 0x00FF'FFFF};

    const u64 watched = manager.virtual_alloc(
        0, 0x10000, MEM_COMMIT | MEM_RESERVE | MEM_WRITE_WATCH,
        PAGE_READWRITE);
    ASSERT_NE(watched, 0u);
    EXPECT_EQ(manager.reset_write_watch(watched, 0x10000), 0u);

    constexpr usize kSpan = 0x8000;
    Byte* ptr = memory.host_ptr_mut(static_cast<GuestAddr>(watched), kSpan);
    ASSERT_NE(ptr, nullptr);
    std::memset(ptr, 0xCD, kSpan);

    u64 count = 32;
    u32 granularity = 0;
    std::vector<u64> pages;
    EXPECT_EQ(manager.get_write_watch(watched, 0x10000, count, granularity,
                                      pages),
              0u);
    // host_ptr_mut records the memset span. Unwritten committed pages stay
    // clean. The memset still has to complete without SIGBUS.
    EXPECT_EQ(count, kSpan / 0x1000);
    ASSERT_EQ(pages.size(), kSpan / 0x1000);
    EXPECT_EQ(pages.front(), watched);
    EXPECT_EQ(pages.back(), watched + kSpan - 0x1000);
}

TEST(VirtualMemoryLinearTest, GetWriteWatchReportsJitStylePageMark) {
    // JIT linear stores mark the 4 KiB page map instead of taking a
    // PROT_READ SIGBUS. GetWriteWatch must report that mark (Learn:
    // pages written since alloc/reset).
    GuestMemory memory;
    ASSERT_TRUE(memory.enable_linear_reservation(0x01000000));
    VirtualMemoryManager manager{memory, 0x00FF'FFFF};

    const u64 watched = manager.virtual_alloc(
        0, 0x1000, MEM_COMMIT | MEM_RESERVE | MEM_WRITE_WATCH,
        PAGE_READWRITE);
    ASSERT_NE(watched, 0u);
    EXPECT_EQ(manager.reset_write_watch(watched, 0x1000), 0u);

    write_watch::mark_linear_pages(watched, 1);

    u64 count = 8;
    u32 granularity = 0;
    std::vector<u64> pages;
    EXPECT_EQ(manager.get_write_watch(watched, 0x1000, count, granularity,
                                      pages),
              0u);
    EXPECT_EQ(count, 1u);
    ASSERT_EQ(pages.size(), 1u);
    EXPECT_EQ(pages[0], watched);
}
