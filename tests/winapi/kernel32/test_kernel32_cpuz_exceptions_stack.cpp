/// @file test_kernel32_cpuz_exceptions_stack.cpp
/// @brief Unit tests for KERNEL32 x86 RtlUnwind, stack back-trace capture and MSVC thread-name exception contracts

#include "kernel32_cpuz_fixture.hpp"
#include <algorithm>
#include <pthread.h>
#include <string>

// x86 RtlUnwind contract, measured on Windows:
// tools/oracle_probe/rtl_unwind32_probe.{c,expected}.
TEST_F(Kernel32CpuzTest, RtlUnwindX86ReturnsToCallerWithMeasuredRegisters) {
    const GuestAddr stack = cpu.sp();
    constexpr GuestAddr teb = DATA_BASE + 0xA00;
    constexpr GuestAddr target = STACK_BASE + 0xF00;
    constexpr GuestAddr record = DATA_BASE + 0x600;
    cpu.seg.fs_desc.base = teb;
    ASSERT_TRUE(memory.write_u32(teb + 0x0, static_cast<u32>(target)).is_ok());
    ASSERT_TRUE(memory.write_u32(record + 4, 0x10).is_ok());  // caller's flags
    cpu.gpr.rbp = DATA_BASE + 0x800;
    cpu.gpr.rbx = 0x11111111;
    cpu.gpr.rsi = 0x22222222;
    cpu.gpr.rdi = 0x33333333;
    ASSERT_TRUE(memory.write_u32(stack + 0, 0x00405678).is_ok());  // return
    ASSERT_TRUE(memory.write_u32(stack + 4, static_cast<u32>(target)).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 8, 0x00401234).is_ok());  // TargetIp
    ASSERT_TRUE(memory.write_u32(stack + 12, static_cast<u32>(record)).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 16, 0xAABBCCDD).is_ok());

    mgr.impl_RtlUnwind(marshaler);

    // Returns to the caller -- TargetIp is ignored -- past the 4 arguments.
    EXPECT_FALSE(cpu.pending_unwind.active);
    EXPECT_TRUE(cpu.thunk_control_transfer);
    EXPECT_EQ(cpu.rip, 0x00405678u);
    EXPECT_EQ(cpu.sp(), stack + 20);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 0xAABBCCDDu);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rbp), DATA_BASE + 0x800);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rbx), 0u);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rsi), 0u);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rdi), 0u);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rcx), 0u);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rdx), 0u);
    EXPECT_EQ(memory.read_u32(teb).value_or(0), static_cast<u32>(target));
    // EXCEPTION_UNWINDING is OR-ed into the caller's record in place.
    EXPECT_EQ(memory.read_u32(record + 4).value_or(0), 0x12u);
}

TEST_F(Kernel32CpuzTest, RtlUnwindX86CallsAndUnlinksEachRegistrationAboveTarget) {
    const GuestAddr stack = cpu.sp();
    constexpr GuestAddr teb = DATA_BASE + 0xA00;
    constexpr GuestAddr target = STACK_BASE + 0xF00;
    constexpr GuestAddr above = STACK_BASE + 0xE00;
    constexpr u32 handler = 0x00402000;
    cpu.seg.fs_desc.base = teb;
    ASSERT_TRUE(memory.write_u32(teb + 0x0, static_cast<u32>(above)).is_ok());
    ASSERT_TRUE(memory.write_u32(teb + 0x4, STACK_BASE + 0x1000).is_ok());
    ASSERT_TRUE(memory.write_u32(teb + 0x8, STACK_BASE).is_ok());
    ASSERT_TRUE(memory.write_u32(above + 0, static_cast<u32>(target)).is_ok());
    ASSERT_TRUE(memory.write_u32(above + 4, handler).is_ok());
    ASSERT_TRUE(memory.write_u32(target + 0, 0xFFFFFFFFu).is_ok());
    ASSERT_TRUE(memory.write_u32(target + 4, handler).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 0, 0x00405678).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 4, static_cast<u32>(target)).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 8, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 12, 0).is_ok());  // no record
    ASSERT_TRUE(memory.write_u32(stack + 16, 7).is_ok());

    int calls = 0;
    mgr.set_guest_call([&](GuestAddr fn, const u64* args, usize count, bool) -> u64 {
        ++calls;
        EXPECT_EQ(fn, handler);
        EXPECT_EQ(count, 4u);
        EXPECT_EQ(args[1], above);  // EstablisherFrame
        const auto erec = static_cast<GuestAddr>(args[0]);
        EXPECT_EQ(memory.read_u32(erec + 0).value_or(0), 0xC0000027u);  // STATUS_UNWIND
        EXPECT_EQ(memory.read_u32(erec + 4).value_or(0), 0x2u);
        EXPECT_EQ(memory.read_u32(erec + 0xC).value_or(0), 0x00405678u);
        const auto ctx = static_cast<GuestAddr>(args[2]);
        EXPECT_EQ(memory.read_u32(ctx + 0x00).value_or(0), 0x10007u);  // ContextFlags
        EXPECT_EQ(memory.read_u32(ctx + 0xB8).value_or(0), 0x00405678u);  // Eip
        EXPECT_EQ(memory.read_u32(ctx + 0xC4).value_or(0), stack + 20);  // Esp
        EXPECT_EQ(memory.read_u32(ctx + 0xB0).value_or(0), 7u);          // Eax
        EXPECT_NE(args[3], 0u);  // DispatcherContext
        // FS:[0] is a protector whose Next is the registration being unwound.
        const u32 head = memory.read_u32(teb).value_or(0);
        EXPECT_NE(head, static_cast<u32>(above));
        EXPECT_EQ(memory.read_u32(head).value_or(0), static_cast<u32>(above));
        return 1;  // ExceptionContinueSearch
    });

    mgr.impl_RtlUnwind(marshaler);

    EXPECT_EQ(calls, 1);  // the target's own handler is not called
    EXPECT_EQ(memory.read_u32(teb).value_or(0), static_cast<u32>(target));
    EXPECT_EQ(cpu.rip, 0x00405678u);
    EXPECT_FALSE(cpu.pending_raise.active);
}

TEST_F(Kernel32CpuzTest, RtlUnwindExWithoutUnwindTablesRaisesBadStack) {
    // kernel32!RtlUnwindEx (and x64 RtlUnwind) walk frames from their caller
    // (winapi/kernel32/thread/unwind64.cpp, exercised on real guest code by
    // tools/check_rtl_unwind64_probe.py). With no module table to look
    // frames up in, the target cannot be reached: STATUS_BAD_STACK,
    // noncontinuable, raised from the caller's state -- never an unwind
    // request left behind for nothing to act on (that spun LOCKDOWN Protocol's
    // libpng longjmp at 100% CPU).
    constexpr u64 target_frame = STACK_BASE + 0xC00;
    constexpr u64 target_ip = 0x0000000140001234ull;
    constexpr u64 exception_record = DATA_BASE + 0x600;
    constexpr u64 return_value = 0xC0000005ull;

    for (const char* name : {"RtlUnwindEx", "RtlUnwind"}) {
        cpu.init_long_mode();
        cpu.pending_raise.clear();
        cpu.pending_unwind.clear();
        cpu.gpr.rsp = STACK_BASE + 0x800;
        ASSERT_TRUE(memory.write_u64(cpu.gpr.rsp, 0x0000000140005678ull).is_ok());
        cpu.gpr.rcx = target_frame;
        cpu.gpr.rdx = target_ip;
        cpu.gpr.r8 = exception_record;
        cpu.gpr.r9 = return_value;
        ASSERT_TRUE(memory.write_u64(cpu.gpr.rsp + 0x28, 0).is_ok());
        ASSERT_TRUE(memory.write_u64(cpu.gpr.rsp + 0x30, 0).is_ok());

        auto exports = create_kernel32_ext_thunks(mgr);
        const auto entry = std::find_if(
            exports.entries.begin(), exports.entries.end(),
            [name](const ExportEntry& e) { return e.name == name; });
        ASSERT_NE(entry, exports.entries.end()) << name;
        entry->handler(marshaler);

        EXPECT_FALSE(cpu.pending_unwind.active) << name;
        ASSERT_TRUE(cpu.pending_raise.active) << name;
        EXPECT_EQ(cpu.pending_raise.code, 0xC0000028u) << name;
        EXPECT_EQ(cpu.pending_raise.flags & 1u, 1u) << name;
        EXPECT_EQ(cpu.gpr.rsp, STACK_BASE + 0x800) << name;
        EXPECT_FALSE(cpu.thunk_control_transfer) << name;
    }
}

TEST_F(Kernel32CpuzTest, RtlCaptureStackBackTraceWalksSkipsAndHashesFrames) {
    constexpr GuestAddr back_trace = DATA_BASE + 0x400;
    constexpr GuestAddr hash = DATA_BASE + 0x440;
    constexpr GuestAddr frame0 = STACK_BASE + 0x900;
    constexpr GuestAddr frame1 = STACK_BASE + 0x920;
    constexpr u32 caller = 0x401234;
    constexpr u32 parent0 = 0x402345;
    constexpr u32 parent1 = 0x403456;
    const GuestAddr stack = cpu.sp();

    ASSERT_TRUE(memory.write_u32(stack, caller).is_ok());
    ASSERT_TRUE(memory.write_u32(frame0, frame1).is_ok());
    ASSERT_TRUE(memory.write_u32(frame0 + 4, parent0).is_ok());
    ASSERT_TRUE(memory.write_u32(frame1, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(frame1 + 4, parent1).is_ok());
    cpu.gpr.rbp = frame0;

    ASSERT_TRUE(memory.write_u32(stack + 4, 1).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 8, 2).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 12, back_trace).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 16, hash).is_ok());

    auto exports = create_kernel32_ext_thunks(mgr);
    const auto entry = std::find_if(
        exports.entries.begin(), exports.entries.end(),
        [](const ExportEntry& candidate) {
            return candidate.name == "RtlCaptureStackBackTrace";
        });
    ASSERT_NE(entry, exports.entries.end());
    entry->handler(marshaler);

    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 2u);
    EXPECT_EQ(memory.read_u32(back_trace).value_or(0), parent0);
    EXPECT_EQ(memory.read_u32(back_trace + 4).value_or(0), parent1);
    EXPECT_EQ(memory.read_u32(hash).value_or(0), parent0 + parent1);
    EXPECT_EQ(entry->stdcall_bytes, 16u);
}

TEST_F(Kernel32CpuzTest, RtlCaptureStackBackTraceStopsAtMalformedFrame) {
    constexpr GuestAddr back_trace = DATA_BASE + 0x480;
    constexpr GuestAddr hash = DATA_BASE + 0x4c0;
    constexpr GuestAddr frame = STACK_BASE + 0x900;
    constexpr u32 caller = 0x501234;
    constexpr u32 parent = 0x502345;
    const GuestAddr stack = cpu.sp();

    ASSERT_TRUE(memory.write_u32(stack, caller).is_ok());
    ASSERT_TRUE(memory.write_u32(frame, frame).is_ok());
    ASSERT_TRUE(memory.write_u32(frame + 4, parent).is_ok());
    cpu.gpr.rbp = frame;
    ASSERT_TRUE(memory.write_u32(stack + 4, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 8, 8).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 12, back_trace).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 16, hash).is_ok());

    auto exports = create_kernel32_ext_thunks(mgr);
    const auto entry = std::find_if(
        exports.entries.begin(), exports.entries.end(),
        [](const ExportEntry& candidate) {
            return candidate.name == "RtlCaptureStackBackTrace";
        });
    ASSERT_NE(entry, exports.entries.end());
    entry->handler(marshaler);

    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 2u);
    EXPECT_EQ(memory.read_u32(back_trace).value_or(0), caller);
    EXPECT_EQ(memory.read_u32(back_trace + 4).value_or(0), parent);
    EXPECT_EQ(memory.read_u32(hash).value_or(0), caller + parent);
}

TEST_F(Kernel32CpuzTest, RtlCaptureStackBackTracePreservesX64Addresses) {
    constexpr GuestAddr back_trace = DATA_BASE + 0x500;
    constexpr GuestAddr hash = DATA_BASE + 0x540;
    constexpr GuestAddr frame = STACK_BASE + 0x900;
    constexpr u64 caller = 0x0000000140123456ull;
    constexpr u64 parent = 0x0000000140234567ull;
    const GuestAddr stack = STACK_BASE + 0x800;

    cpu.init_long_mode();
    cpu.gpr.rsp = stack;
    cpu.gpr.rbp = frame;
    ASSERT_TRUE(memory.write_u64(stack, caller).is_ok());
    ASSERT_TRUE(memory.write_u64(frame, 0).is_ok());
    ASSERT_TRUE(memory.write_u64(frame + 8, parent).is_ok());
    cpu.gpr.rcx = 0;
    cpu.gpr.rdx = 2;
    cpu.gpr.r8 = back_trace;
    cpu.gpr.r9 = hash;

    auto exports = create_kernel32_ext_thunks(mgr);
    const auto entry = std::find_if(
        exports.entries.begin(), exports.entries.end(),
        [](const ExportEntry& candidate) {
            return candidate.name == "RtlCaptureStackBackTrace";
        });
    ASSERT_NE(entry, exports.entries.end());
    entry->handler(marshaler);

    EXPECT_EQ(cpu.gpr.rax, 2u);
    EXPECT_EQ(memory.read_u64(back_trace).value_or(0), caller);
    EXPECT_EQ(memory.read_u64(back_trace + 8).value_or(0), parent);
    EXPECT_EQ(
        memory.read_u32(hash).value_or(0),
        static_cast<u32>(caller) + static_cast<u32>(parent));
}

TEST_F(Kernel32CpuzTest, RtlCaptureStackBackTraceZeroCaptureClearsHash) {
    constexpr GuestAddr hash = DATA_BASE + 0x500;
    const GuestAddr stack = cpu.sp();
    ASSERT_TRUE(memory.write_u32(hash, 0xdeadbeef).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 4, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 8, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 12, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 16, hash).is_ok());

    auto exports = create_kernel32_ext_thunks(mgr);
    const auto entry = std::find_if(
        exports.entries.begin(), exports.entries.end(),
        [](const ExportEntry& candidate) {
            return candidate.name == "RtlCaptureStackBackTrace";
        });
    ASSERT_NE(entry, exports.entries.end());
    entry->handler(marshaler);

    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 0u);
    EXPECT_EQ(memory.read_u32(hash).value_or(1), 0u);
}

TEST_F(Kernel32CpuzTest, RaiseExceptionMsVcSetThreadNameContinues) {
    // Learn: Set a thread name in native code — RaiseException(0x406D1388)
    // with THREADNAME_INFO. Without a debugger Windows still dispatches it to
    // VEH / __except (thread_lifecycle_probe: RAISE ms_vc_exception veh_saw=1).
    ThreadManager::set_current_thread_id(1);
    constexpr GuestAddr name_addr = DATA_BASE + 0x500;
    constexpr GuestAddr args_addr = DATA_BASE + 0x600;
    const char* name = "JobThread0";
    for (u32 i = 0; name[i] != 0; ++i) {
        ASSERT_TRUE(memory.write_u8(name_addr + i,
                                    static_cast<u8>(name[i])).is_ok());
    }
    ASSERT_TRUE(memory.write_u8(name_addr + 10, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(args_addr, 0x1000).is_ok());
    ASSERT_TRUE(memory.write_u32(args_addr + 4, static_cast<u32>(name_addr)).is_ok());
    ASSERT_TRUE(memory.write_u32(args_addr + 8, 0xFFFFFFFFu).is_ok());
    ASSERT_TRUE(memory.write_u32(args_addr + 12, 0).is_ok());

    const GuestAddr stack = cpu.sp();
    ASSERT_TRUE(memory.write_u32(stack + 4, 0x406D1388u).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 8, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 12, 4).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 16, static_cast<u32>(args_addr)).is_ok());
    char saved_name[64] = {};
    ASSERT_EQ(pthread_getname_np(pthread_self(), saved_name, sizeof(saved_name)), 0);
    cpu.pending_raise.active = false;
    mgr.impl_RaiseException(marshaler);
    EXPECT_TRUE(cpu.pending_raise.active);
    cpu.pending_raise.active = false;

    std::u16string description;
    ASSERT_TRUE(threads.get_thread_description(
        ThreadManager::get_current_thread(), description));
    ASSERT_EQ(description.size(), 10u);
    EXPECT_EQ(description[0], u'J');
    EXPECT_EQ(description[3], u'T');
    EXPECT_EQ(description[9], u'0');

    // THREADNAME_INFO with dwThreadID -1 names the calling thread: the host
    // thread carries the name too (through the same path as
    // SetThreadDescription).
    char host_name[64] = {};
    EXPECT_EQ(pthread_getname_np(pthread_self(), host_name, sizeof(host_name)), 0);
    EXPECT_EQ(std::string(host_name), "JobThread0");
    (void)pthread_setname_np(saved_name);
}
