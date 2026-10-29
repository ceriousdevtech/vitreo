// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// INFRA-004: Thunk Dispatch Bridge tests

#include <gtest/gtest.h>

#include "core/memory/guest_memory.hpp"
#include "core/cpu/state/cpu_state.hpp"
#include "winapi/core/thunk_registry.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/process/process.hpp"

#include <cstdio>

using namespace vitreo;
using namespace vitreo::winapi;

// =============================================================================
// ThunkRegistry region query tests
// =============================================================================

TEST(ThunkDispatchTest, IsInThunkRegionEmpty) {
    ThunkRegistry registry(0x7000'0000);
    // No thunks registered — next_thunk_ == thunk_base_, region is empty
    EXPECT_FALSE(registry.is_in_thunk_region(0x7000'0000));
    EXPECT_FALSE(registry.is_in_thunk_region(0x7000'0010));
    EXPECT_FALSE(registry.is_in_thunk_region(0x0));
}

TEST(ThunkDispatchTest, IsInThunkRegionAfterRegistration) {
    ThunkRegistry registry(0x7000'0000);

    DllExports exports;
    exports.entries.push_back(ExportEntry{
        "Func1", 1, [](Marshaler&) {}});
    exports.entries.push_back(ExportEntry{
        "Func2", 2, [](Marshaler&) {}});
    (void)registry.register_dll("test.dll", std::move(exports));

    // Thunks at 0x7000'0000 and 0x7000'0010 (spacing=16)
    EXPECT_TRUE(registry.is_in_thunk_region(0x7000'0000));
    EXPECT_TRUE(registry.is_in_thunk_region(0x7000'0010));
    // Just past last allocated thunk
    EXPECT_FALSE(registry.is_in_thunk_region(0x7000'0020));
    // Below base
    EXPECT_FALSE(registry.is_in_thunk_region(0x6FFF'FFFF));
}

TEST(ThunkDispatchTest, ThunkBaseAndEnd) {
    ThunkRegistry registry(0x7000'0000);

    DllExports exports;
    exports.entries.push_back(ExportEntry{"A", 1, [](Marshaler&) {}});
    (void)registry.register_dll("test.dll", std::move(exports));

    EXPECT_EQ(registry.thunk_base(), 0x7000'0000u);
    EXPECT_EQ(registry.thunk_end(), 0x7000'0010u);  // base + 1*16
}

// =============================================================================
// Thunk dispatch logic (isolated, no interpreter)
// =============================================================================

TEST(ThunkDispatchTest, ThunkAddressTriggersHandler) {
    GuestMemory memory;
    (void)memory.map_region(0x7000'0000, 0x1000, MemoryProtection::ReadWrite);
    (void)memory.map_region(0x0000'1000, 0x1000, MemoryProtection::ReadWrite);

    cpu::CpuState cpu;
    cpu.init_long_mode();

    ThunkRegistry registry(0x7000'0000);

    bool handler_called = false;
    DllExports exports;
    exports.entries.push_back(ExportEntry{
        "TestFunc", 1,
        [&handler_called](Marshaler& m) {
            handler_called = true;
            m.set_return(42);
        }
    });
    (void)registry.register_dll("test.dll", std::move(exports));

    auto thunk_addr = registry.get_thunk_address("test.dll", "TestFunc");
    ASSERT_TRUE(thunk_addr.is_ok());

    // Set up CPU: RIP at thunk, RSP pointing to return address on stack
    cpu.rip = thunk_addr.value();
    cpu.gpr.rsp = 0x1800;
    u64 return_addr = 0x4000;
    (void)memory.write_u64(cpu.gpr.rsp, return_addr);

    // --- Thunk dispatch logic (mirrors run.cpp) ---
    ASSERT_TRUE(registry.is_in_thunk_region(cpu.rip));
    auto resolved = registry.resolve_handler(cpu.rip);
    ASSERT_TRUE(resolved.is_ok());

    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    (*resolved.value())(marshaler);

    // Simulate RET: pop return address from stack
    auto ret = memory.read_u64(cpu.gpr.rsp);
    ASSERT_TRUE(ret.is_ok());
    cpu.rip = ret.value();
    cpu.gpr.rsp += 8;

    EXPECT_TRUE(handler_called);
    EXPECT_EQ(cpu.gpr.rax, 42u);
    EXPECT_EQ(cpu.rip, return_addr);
    EXPECT_EQ(cpu.gpr.rsp, 0x1808u);
}

TEST(ThunkDispatchTest, HandlerPointerSurvivesMissingExportGrowth) {
    // resolve_handler returns a pointer into thunks_. Appending missing-export
    // stubs used to reallocate that vector and destroy neighboring
    // std::function objects while a CRT handler was still running.
    GuestMemory memory;
    (void)memory.map_region(0x7000'0000, 0x20000, MemoryProtection::ReadWrite);

    ThunkRegistry registry(0x7000'0000);
    bool called = false;
    DllExports exports;
    exports.entries.push_back(ExportEntry{
        "KeepAlive", 1,
        [&called](Marshaler& m) {
            called = true;
            m.set_return(1);
        }});
    ASSERT_TRUE(registry.register_dll("test.dll", std::move(exports)).is_ok());

    const auto thunk = registry.get_thunk_address("test.dll", "KeepAlive");
    ASSERT_TRUE(thunk.is_ok());
    const auto resolved = registry.resolve_handler(thunk.value());
    ASSERT_TRUE(resolved.is_ok());
    const ApiHandler* handler = resolved.value();

    for (u32 i = 0; i < 512; ++i) {
        char name[32];
        std::snprintf(name, sizeof(name), "Missing%u", i);
        (void)registry.register_missing_export("kernel32.dll", name, 0);
    }

    cpu::CpuState cpu;
    cpu.init_long_mode();
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    (*handler)(marshaler);
    EXPECT_TRUE(called);
    EXPECT_EQ(cpu.gpr.rax, 1u);
}

TEST(ThunkDispatchTest, ExitProcessStopsDispatchLoop) {
    GuestMemory memory;
    (void)memory.map_region(0x7000'0000, 0x1000, MemoryProtection::ReadWrite);
    (void)memory.map_region(0x0000'1000, 0x1000, MemoryProtection::ReadWrite);

    cpu::CpuState cpu;
    cpu.init_long_mode();

    ProcessManager process_mgr;
    ThunkRegistry registry(0x7000'0000);

    // Register kernel32 with real process exports
    auto k32_exports = make_process_exports(process_mgr);
    (void)registry.register_dll("kernel32.dll", std::move(k32_exports));

    auto thunk_addr = registry.get_thunk_address("kernel32.dll", "ExitProcess");
    ASSERT_TRUE(thunk_addr.is_ok());

    // ExitProcess(7): first arg in RCX
    cpu.rip = thunk_addr.value();
    cpu.gpr.rcx = 7;
    cpu.gpr.rsp = 0x1800;
    (void)memory.write_u64(cpu.gpr.rsp, 0xDEAD);

    // Dispatch
    ASSERT_TRUE(registry.is_in_thunk_region(cpu.rip));
    auto resolved = registry.resolve_handler(cpu.rip);
    ASSERT_TRUE(resolved.is_ok());

    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    (*resolved.value())(marshaler);

    EXPECT_TRUE(process_mgr.has_exited());
    EXPECT_EQ(process_mgr.exit_code(), 7u);
}

TEST(ThunkDispatchTest, UnresolvedThunkAddressFails) {
    ThunkRegistry registry(0x7000'0000);

    DllExports exports;
    exports.entries.push_back(ExportEntry{"Func", 1, [](Marshaler&) {}});
    (void)registry.register_dll("test.dll", std::move(exports));

    // Address is in the thunk region but was never assigned to a function
    // (only 0x7000'0000 is valid)
    GuestAddr bad_addr = 0x7000'0009;  // neither entry stub nor dispatch gateway
    EXPECT_TRUE(registry.is_in_thunk_region(bad_addr));
    auto resolved = registry.resolve_handler(bad_addr);
    EXPECT_TRUE(resolved.is_err());
}

TEST(ThunkDispatchTest, MultipleHandlersDispatchCorrectly) {
    GuestMemory memory;
    (void)memory.map_region(0x7000'0000, 0x1000, MemoryProtection::ReadWrite);
    (void)memory.map_region(0x0000'1000, 0x1000, MemoryProtection::ReadWrite);

    cpu::CpuState cpu;
    cpu.init_long_mode();

    ThunkRegistry registry(0x7000'0000);

    int called_id = -1;
    DllExports exports;
    exports.entries.push_back(ExportEntry{
        "FuncA", 1,
        [&called_id](Marshaler& m) { called_id = 0; m.set_return(100); }
    });
    exports.entries.push_back(ExportEntry{
        "FuncB", 2,
        [&called_id](Marshaler& m) { called_id = 1; m.set_return(200); }
    });
    (void)registry.register_dll("test.dll", std::move(exports));

    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);

    // Dispatch FuncB
    auto addr_b = registry.get_thunk_address("test.dll", "FuncB");
    ASSERT_TRUE(addr_b.is_ok());

    cpu.rip = addr_b.value();
    cpu.gpr.rsp = 0x1800;
    (void)memory.write_u64(cpu.gpr.rsp, 0x5000);

    auto handler = registry.resolve_handler(cpu.rip);
    ASSERT_TRUE(handler.is_ok());
    (*handler.value())(marshaler);

    auto ret = memory.read_u64(cpu.gpr.rsp);
    ASSERT_TRUE(ret.is_ok());
    cpu.rip = ret.value();
    cpu.gpr.rsp += 8;

    EXPECT_EQ(called_id, 1);
    EXPECT_EQ(cpu.gpr.rax, 200u);
    EXPECT_EQ(cpu.rip, 0x5000u);
}

// =============================================================================
// PE32 (32-bit) thunk dispatch tests — INFRA-005
// =============================================================================

TEST(ThunkDispatchTest, ThunkDispatch32BitRetSimulation) {
    GuestMemory memory;
    (void)memory.map_region(0x7000'0000, 0x1000, MemoryProtection::ReadWrite);
    (void)memory.map_region(0x0000'1000, 0x1000, MemoryProtection::ReadWrite);

    cpu::CpuState cpu;
    cpu.init_protected_mode();  // 32-bit

    ThunkRegistry registry(0x7000'0000);

    bool handler_called = false;
    DllExports exports;
    exports.entries.push_back(ExportEntry{
        "TestFunc32", 1,
        [&handler_called](Marshaler& m) {
            handler_called = true;
            m.set_return(99);
        }
    });
    (void)registry.register_dll("test.dll", std::move(exports));

    auto thunk_addr = registry.get_thunk_address("test.dll", "TestFunc32");
    ASSERT_TRUE(thunk_addr.is_ok());

    cpu.rip = thunk_addr.value();
    cpu.set_sp(0x1800);
    u32 return_addr = 0x4000;
    // Write 4-byte return address (32-bit)
    (void)memory.write_u32(cpu.sp(), return_addr);

    // --- Thunk dispatch logic (32-bit path) ---
    ASSERT_TRUE(registry.is_in_thunk_region(cpu.rip));
    auto resolved = registry.resolve_handler(cpu.rip);
    ASSERT_TRUE(resolved.is_ok());

    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    (*resolved.value())(marshaler);

    // Simulate 32-bit RET: pop 4-byte return address
    ASSERT_FALSE(cpu.is_64bit());
    auto ret = memory.read_u32(cpu.sp());
    ASSERT_TRUE(ret.is_ok());
    cpu.rip = static_cast<u64>(ret.value());
    cpu.set_sp(cpu.sp() + 4);

    EXPECT_TRUE(handler_called);
    EXPECT_EQ(cpu.gpr.rax, 99u);       // masked to 32 bits
    EXPECT_EQ(cpu.rip, 0x4000u);
    EXPECT_EQ(cpu.sp(), 0x1804u);       // ESP advanced by 4, not 8
}

TEST(ThunkDispatchTest, ThunkDispatch32BitStdcallArgs) {
    GuestMemory memory;
    (void)memory.map_region(0x7000'0000, 0x1000, MemoryProtection::ReadWrite);
    (void)memory.map_region(0x0000'1000, 0x1000, MemoryProtection::ReadWrite);

    cpu::CpuState cpu;
    cpu.init_protected_mode();  // 32-bit

    ThunkRegistry registry(0x7000'0000);

    u64 captured_arg0 = 0, captured_arg1 = 0;
    DllExports exports;
    exports.entries.push_back(ExportEntry{
        "TwoArgFunc", 1,
        [&captured_arg0, &captured_arg1](Marshaler& m) {
            captured_arg0 = m.arg(0);
            captured_arg1 = m.arg(1);
            m.set_return(captured_arg0 + captured_arg1);
        }
    });
    (void)registry.register_dll("test.dll", std::move(exports));

    auto thunk_addr = registry.get_thunk_address("test.dll", "TwoArgFunc");
    ASSERT_TRUE(thunk_addr.is_ok());

    cpu.rip = thunk_addr.value();
    cpu.set_sp(0x1800);
    // stdcall stack layout: [ESP] = return addr, [ESP+4] = arg0, [ESP+8] = arg1
    (void)memory.write_u32(cpu.sp(),     0xDEAD);  // return address
    (void)memory.write_u32(cpu.sp() + 4, 100);     // arg0
    (void)memory.write_u32(cpu.sp() + 8, 200);     // arg1

    auto resolved = registry.resolve_handler(cpu.rip);
    ASSERT_TRUE(resolved.is_ok());

    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    (*resolved.value())(marshaler);

    EXPECT_EQ(captured_arg0, 100u);
    EXPECT_EQ(captured_arg1, 200u);
    EXPECT_EQ(cpu.gpr.rax, 300u);
}
