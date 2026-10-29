// SPDX-License-Identifier: LGPL-2.1-only
// Tests for MSVC `_except_handler3` scopetable dispatch.

#include <gtest/gtest.h>

#include "core/memory/guest_memory.hpp"
#include "core/cpu/seh/seh32.hpp"
#include "winapi/msvcrt/runtime/except_handler3.hpp"

using namespace vitreo;
using namespace vitreo::winapi;

namespace {

constexpr GuestAddr k_base = 0x10000;
constexpr u32 k_size = 0x4000;
constexpr u32 k_frame = 0x200;
constexpr u32 k_erec = 0x300;
constexpr u32 k_ctx = 0x400;
constexpr u32 k_table = 0x800;
constexpr u32 k_handler = 0x00401000u;
constexpr u32 k_filter = 0x00402000u;

void map(GuestMemory& mem) {
    ASSERT_TRUE(mem.map_region(k_base, k_size, MemoryProtection::ReadWrite));
}

void write_registration(GuestMemory& mem, i32 trylevel) {
    (void)mem.write_u32(k_base + k_frame + 0, 0xFFFFFFFFu);
    (void)mem.write_u32(k_base + k_frame + 4, 0x00c8c45au);
    (void)mem.write_u32(k_base + k_frame + 8, static_cast<u32>(k_base + k_table));
    (void)mem.write_u32(k_base + k_frame + 12, static_cast<u32>(trylevel));
}

void write_scope(GuestMemory& mem, i32 enclosing, u32 filter, u32 handler) {
    (void)mem.write_u32(k_base + k_table + 0, static_cast<u32>(enclosing));
    (void)mem.write_u32(k_base + k_table + 4, filter);
    (void)mem.write_u32(k_base + k_table + 8, handler);
}

void write_av(GuestMemory& mem) {
    (void)mem.write_u32(k_base + k_erec + cpu::EREC32_CODE, cpu::STATUS_ACCESS_VIOLATION);
    (void)mem.write_u32(k_base + k_erec + cpu::EREC32_FLAGS, 0);
    (void)mem.write_u32(k_base + k_erec + cpu::EREC32_ADDRESS, 0x290);
}

} // namespace

TEST(ExceptHandler3Test, ExecuteHandlerTransfersToExceptBlock) {
    GuestMemory mem;
    map(mem);
    write_registration(mem, 0);
    write_scope(mem, -1, k_filter, k_handler);
    write_av(mem);

    const Eh3DispatchResult r = dispatch_except_handler3_32(
        mem, static_cast<u32>(k_base + k_frame),
        static_cast<u32>(k_base + k_erec),
        [](GuestAddr filter, u32 parent_ebp) -> i32 {
            EXPECT_EQ(filter, k_filter);
            EXPECT_EQ(parent_ebp, static_cast<u32>(k_base + k_frame + 16));
            return 1; // EXCEPTION_EXECUTE_HANDLER
        });

    ASSERT_TRUE(r.handled);
    EXPECT_FALSE(r.continue_execution);
    EXPECT_EQ(r.handler_addr, k_handler);
    EXPECT_EQ(r.enclosing_level, -1);

    patch_context_for_eh3_handler(
        mem, static_cast<u32>(k_base + k_frame),
        static_cast<u32>(k_base + k_ctx), r);
    EXPECT_EQ(mem.read_u32(k_base + k_ctx + cpu::CTX32_EIP).value(), k_handler);
    EXPECT_EQ(mem.read_u32(k_base + k_ctx + cpu::CTX32_EBP).value(),
              static_cast<u32>(k_base + k_frame + 16));
    EXPECT_EQ(mem.read_u32(k_base + k_frame + 12).value(), 0xFFFFFFFFu);
}

TEST(ExceptHandler3Test, ContinueSearchWhenFilterDeclines) {
    GuestMemory mem;
    map(mem);
    write_registration(mem, 0);
    write_scope(mem, -1, k_filter, k_handler);
    write_av(mem);

    const Eh3DispatchResult r = dispatch_except_handler3_32(
        mem, static_cast<u32>(k_base + k_frame),
        static_cast<u32>(k_base + k_erec),
        [](GuestAddr, u32) -> i32 { return 0; });

    EXPECT_FALSE(r.handled);
    EXPECT_FALSE(r.continue_execution);
}

TEST(ExceptHandler3Test, UnwindingSkipsSearch) {
    GuestMemory mem;
    map(mem);
    write_registration(mem, 0);
    write_scope(mem, -1, k_filter, k_handler);
    (void)mem.write_u32(k_base + k_erec + cpu::EREC32_CODE, cpu::STATUS_ACCESS_VIOLATION);
    (void)mem.write_u32(k_base + k_erec + cpu::EREC32_FLAGS, 0x2); // EXCEPTION_UNWINDING

    bool called = false;
    const Eh3DispatchResult r = dispatch_except_handler3_32(
        mem, static_cast<u32>(k_base + k_frame),
        static_cast<u32>(k_base + k_erec),
        [&](GuestAddr, u32) -> i32 {
            called = true;
            return 1;
        });

    EXPECT_FALSE(r.handled);
    EXPECT_FALSE(called);
}

TEST(ExceptHandler3Test, ContinueExecutionWhenFilterReturnsMinusOne) {
    GuestMemory mem;
    map(mem);
    write_registration(mem, 0);
    write_scope(mem, -1, k_filter, k_handler);
    write_av(mem);

    const Eh3DispatchResult r = dispatch_except_handler3_32(
        mem, static_cast<u32>(k_base + k_frame),
        static_cast<u32>(k_base + k_erec),
        [](GuestAddr, u32) -> i32 { return -1; }); // EXCEPTION_CONTINUE_EXECUTION

    EXPECT_FALSE(r.handled);
    EXPECT_TRUE(r.continue_execution);
    EXPECT_EQ(r.handler_addr, 0u);
}

TEST(ExceptHandler3Test, ExceptionPointersDoNotClobberSavedEsp) {
    GuestMemory mem;
    map(mem);
    write_registration(mem, 0);

    constexpr u32 k_saved_esp = 0x00ABCDEF;
    constexpr u32 k_pointers = 0x500;
    (void)mem.write_u32(k_base + k_frame - 8, k_saved_esp);
    (void)mem.write_u32(k_base + k_frame - 4, 0);

    install_eh3_exception_pointers(
        mem, static_cast<u32>(k_base + k_frame),
        static_cast<u32>(k_base + k_erec),
        static_cast<u32>(k_base + k_ctx),
        static_cast<u32>(k_base + k_pointers));

    EXPECT_EQ(mem.read_u32(k_base + k_frame - 8).value(), k_saved_esp);
    EXPECT_EQ(mem.read_u32(k_base + k_frame - 4).value(),
              static_cast<u32>(k_base + k_pointers));
    EXPECT_EQ(mem.read_u32(k_base + k_pointers).value(),
              static_cast<u32>(k_base + k_erec));
    EXPECT_EQ(mem.read_u32(k_base + k_pointers + 4).value(),
              static_cast<u32>(k_base + k_ctx));
}

TEST(ExceptHandler3Test, FinallyOnlyScopeContinuesSearch) {
    GuestMemory mem;
    map(mem);
    write_registration(mem, 0);
    write_scope(mem, -1, 0, k_handler); // filter NULL → __finally
    write_av(mem);

    const Eh3DispatchResult r = dispatch_except_handler3_32(
        mem, static_cast<u32>(k_base + k_frame),
        static_cast<u32>(k_base + k_erec),
        [](GuestAddr, u32) -> i32 { return 1; });

    EXPECT_FALSE(r.handled);
}
