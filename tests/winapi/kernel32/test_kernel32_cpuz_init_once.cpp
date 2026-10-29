/// @file test_kernel32_cpuz_init_once.cpp
/// @brief Unit tests for KERNEL32 InitOnce, NtRunOnce and condition-variable initialisation contracts

#include "kernel32_cpuz_fixture.hpp"
#include <atomic>
#include <chrono>
#include <winapi/kernel32/error.hpp>
#include <thread>

TEST_F(Kernel32CpuzTest, InitializeConditionVariableWritesOnlyX86PointerWidth) {
    constexpr GuestAddr cv = DATA_BASE;
    constexpr u32 adjacent_sentinel = 0xA54AD0u;
    ASSERT_TRUE(memory.write_u32(cv, 0xFFFFFFFFu).is_ok());
    ASSERT_TRUE(memory.write_u32(cv + 4, adjacent_sentinel).is_ok());
    ASSERT_TRUE(memory.write_u32(cpu.sp() + 4, static_cast<u32>(cv)).is_ok());

    mgr.impl_InitializeConditionVariable(marshaler);

    EXPECT_EQ(memory.read_u32(cv).value(), 0u);
    EXPECT_EQ(memory.read_u32(cv + 4).value(), adjacent_sentinel);
}

TEST_F(Kernel32CpuzTest, InitializeConditionVariableWritesX64PointerWidth) {
    constexpr GuestAddr cv = DATA_BASE;
    cpu.init_long_mode();
    cpu.gpr.rcx = cv;
    ASSERT_TRUE(memory.write_u64(cv, 0xFFFFFFFFFFFFFFFFull).is_ok());

    mgr.impl_InitializeConditionVariable(marshaler);

    EXPECT_EQ(memory.read_u64(cv).value(), 0u);
}

TEST_F(Kernel32CpuzTest, InitOnceInvokesCallbackAndPersistsContext) {
    cpu.mode = cpu::CpuMode::Long;
    constexpr GuestAddr init_once = DATA_BASE;
    constexpr GuestAddr context_out = DATA_BASE + 0x20;
    constexpr GuestAddr callback = 0x140001000;
    constexpr u64 context = 0x2000;
    ASSERT_TRUE(memory.write_u64(init_once, 0).is_ok());
    ASSERT_TRUE(memory.write_u64(context_out, 0).is_ok());

    unsigned calls = 0;
    mgr.set_guest_call([&](GuestAddr function, const u64* args, usize count, bool is_64bit) {
        EXPECT_EQ(function, callback);
        EXPECT_EQ(count, 3u);
        EXPECT_TRUE(is_64bit);
        EXPECT_EQ(args[0], init_once);
        EXPECT_EQ(args[1], 0x1234u);
        EXPECT_EQ(args[2], context_out);
        EXPECT_TRUE(memory.write_u64(context_out, context).is_ok());
        ++calls;
        return 1u;
    });

    cpu.gpr.rcx = init_once;
    cpu.gpr.rdx = callback;
    cpu.gpr.r8 = 0x1234;
    cpu.gpr.r9 = context_out;
    mgr.impl_InitOnceExecuteOnce(marshaler);
    ASSERT_EQ(cpu.gpr.rax, 1u);
    EXPECT_EQ(calls, 1u);
    EXPECT_EQ(memory.read_u64(init_once).value(), context | 2u);

    ASSERT_TRUE(memory.write_u64(context_out, 0).is_ok());
    mgr.impl_InitOnceExecuteOnce(marshaler);
    EXPECT_EQ(cpu.gpr.rax, 1u);
    EXPECT_EQ(calls, 1u);
    EXPECT_EQ(memory.read_u64(context_out).value(), context);
}

TEST_F(Kernel32CpuzTest, InitOnceCallbackCanInitializeDifferentObject) {
    cpu.mode = cpu::CpuMode::Long;
    constexpr GuestAddr outer_once = DATA_BASE;
    constexpr GuestAddr inner_once = DATA_BASE + 0x20;
    constexpr GuestAddr outer_callback = 0x140001000;
    constexpr GuestAddr inner_callback = 0x140002000;
    ASSERT_TRUE(memory.write_u64(outer_once, 0).is_ok());
    ASSERT_TRUE(memory.write_u64(inner_once, 0).is_ok());

    unsigned outer_calls = 0;
    unsigned inner_calls = 0;
    mgr.set_guest_call(
        [&](GuestAddr function, const u64* args, usize count, bool is_64bit) {
            EXPECT_EQ(count, 3u);
            EXPECT_TRUE(is_64bit);
            if (function == inner_callback) {
                EXPECT_EQ(args[0], inner_once);
                ++inner_calls;
                return 1u;
            }

            EXPECT_EQ(function, outer_callback);
            EXPECT_EQ(args[0], outer_once);
            ++outer_calls;

            cpu::CpuState inner_cpu;
            inner_cpu.init_long_mode();
            inner_cpu.gpr.rsp = STACK_BASE + 0x600;
            inner_cpu.gpr.rcx = inner_once;
            inner_cpu.gpr.rdx = inner_callback;
            inner_cpu.gpr.r8 = 0;
            inner_cpu.gpr.r9 = 0;
            Marshaler inner_marshaler{memory};
            inner_marshaler.attach_cpu(inner_cpu);
            mgr.impl_InitOnceExecuteOnce(inner_marshaler);
            EXPECT_EQ(inner_cpu.gpr.rax, 1u);
            return 1u;
        });

    cpu.gpr.rcx = outer_once;
    cpu.gpr.rdx = outer_callback;
    cpu.gpr.r8 = 0;
    cpu.gpr.r9 = 0;
    mgr.impl_InitOnceExecuteOnce(marshaler);

    EXPECT_EQ(cpu.gpr.rax, 1u);
    EXPECT_EQ(outer_calls, 1u);
    EXPECT_EQ(inner_calls, 1u);
    EXPECT_EQ(memory.read_u64(outer_once).value(), 2u);
    EXPECT_EQ(memory.read_u64(inner_once).value(), 2u);
}

TEST_F(Kernel32CpuzTest, InitOnceFailedCallbackLeavesObjectRetryable) {
    cpu.mode = cpu::CpuMode::Long;
    constexpr GuestAddr init_once = DATA_BASE;
    constexpr GuestAddr callback = 0x140001000;
    ASSERT_TRUE(memory.write_u64(init_once, 0).is_ok());

    unsigned calls = 0;
    mgr.set_guest_call(
        [&](GuestAddr, const u64*, usize, bool) { return ++calls == 1 ? 0u : 1u; });

    cpu.gpr.rcx = init_once;
    cpu.gpr.rdx = callback;
    cpu.gpr.r8 = 0;
    cpu.gpr.r9 = 0;
    mgr.impl_InitOnceExecuteOnce(marshaler);
    EXPECT_EQ(cpu.gpr.rax, 0u);
    EXPECT_EQ(memory.read_u64(init_once).value(), 0u);

    mgr.impl_InitOnceExecuteOnce(marshaler);
    EXPECT_EQ(cpu.gpr.rax, 1u);
    EXPECT_EQ(calls, 2u);
    EXPECT_EQ(memory.read_u64(init_once).value(), 2u);
}

TEST_F(Kernel32CpuzTest, InitOnceBeginCompletePersistsContext) {
    constexpr GuestAddr init_once = DATA_BASE;
    constexpr GuestAddr pending_out = DATA_BASE + 0x20;
    constexpr GuestAddr context_out = DATA_BASE + 0x30;
    constexpr u32 context = 0x2000;
    const GuestAddr stack = cpu.sp();
    ASSERT_TRUE(memory.write_u32(init_once, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(pending_out, 0xFFFFFFFFu).is_ok());
    ASSERT_TRUE(memory.write_u32(context_out, 0).is_ok());

    // CHECK_ONLY before any init must fail.
    ASSERT_TRUE(memory.write_u32(stack + 4, init_once).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 8, 0x1u).is_ok());  // INIT_ONCE_CHECK_ONLY
    ASSERT_TRUE(memory.write_u32(stack + 12, pending_out).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 16, context_out).is_ok());
    mgr.impl_InitOnceBeginInitialize(marshaler);
    EXPECT_EQ(cpu.gpr.rax, 0u);
    EXPECT_EQ(memory.read_u32(pending_out).value_or(0), 1u);

    // First Begin claims initialization.
    ASSERT_TRUE(memory.write_u32(stack + 8, 0).is_ok());
    mgr.impl_InitOnceBeginInitialize(marshaler);
    ASSERT_EQ(cpu.gpr.rax, 1u);
    EXPECT_EQ(memory.read_u32(pending_out).value_or(0), 1u);
    EXPECT_EQ(memory.read_u32(init_once).value_or(0), 1u);

    // Complete persists context with the completion bit.
    ASSERT_TRUE(memory.write_u32(stack + 4, init_once).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 8, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 12, context).is_ok());
    mgr.impl_InitOnceComplete(marshaler);
    ASSERT_EQ(cpu.gpr.rax, 1u);
    EXPECT_EQ(memory.read_u32(init_once).value_or(0), context | 2u);

    // Subsequent Begin returns the stored context without re-running init.
    ASSERT_TRUE(memory.write_u32(pending_out, 0xFFFFFFFFu).is_ok());
    ASSERT_TRUE(memory.write_u32(context_out, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 4, init_once).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 8, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 12, pending_out).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 16, context_out).is_ok());
    mgr.impl_InitOnceBeginInitialize(marshaler);
    ASSERT_EQ(cpu.gpr.rax, 1u);
    EXPECT_EQ(memory.read_u32(pending_out).value_or(1), 0u);
    EXPECT_EQ(memory.read_u32(context_out).value_or(0), context);

    // CHECK_ONLY after complete succeeds.
    ASSERT_TRUE(memory.write_u32(pending_out, 0xFFFFFFFFu).is_ok());
    ASSERT_TRUE(memory.write_u32(context_out, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 8, 0x1u).is_ok());
    mgr.impl_InitOnceBeginInitialize(marshaler);
    ASSERT_EQ(cpu.gpr.rax, 1u);
    EXPECT_EQ(memory.read_u32(pending_out).value_or(1), 0u);
    EXPECT_EQ(memory.read_u32(context_out).value_or(0), context);
}

TEST_F(Kernel32CpuzTest, InitOnceAsyncLoserCompleteFailsAndCheckOnlySeesWinner) {
    constexpr GuestAddr init_once = DATA_BASE;
    constexpr GuestAddr pending_out = DATA_BASE + 0x20;
    constexpr GuestAddr context_out = DATA_BASE + 0x30;
    constexpr u32 winner_context = 0x3000;
    constexpr u32 loser_context = 0x4000;
    constexpr u32 init_once_async = 0x2u;
    const GuestAddr stack = cpu.sp();
    ASSERT_TRUE(memory.write_u32(init_once, 0).is_ok());

    auto begin_async = [&] {
        ASSERT_TRUE(memory.write_u32(stack + 4, init_once).is_ok());
        ASSERT_TRUE(memory.write_u32(stack + 8, init_once_async).is_ok());
        ASSERT_TRUE(memory.write_u32(stack + 12, pending_out).is_ok());
        ASSERT_TRUE(memory.write_u32(stack + 16, context_out).is_ok());
        mgr.impl_InitOnceBeginInitialize(marshaler);
    };

    begin_async();
    ASSERT_EQ(cpu.gpr.rax, 1u);
    EXPECT_EQ(memory.read_u32(pending_out).value_or(0), 1u);

    // Parallel async Begin is also pending.
    begin_async();
    ASSERT_EQ(cpu.gpr.rax, 1u);
    EXPECT_EQ(memory.read_u32(pending_out).value_or(0), 1u);

    ASSERT_TRUE(memory.write_u32(stack + 4, init_once).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 8, init_once_async).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 12, winner_context).is_ok());
    mgr.impl_InitOnceComplete(marshaler);
    ASSERT_EQ(cpu.gpr.rax, 1u);

    // Second completer loses the race.
    ASSERT_TRUE(memory.write_u32(stack + 12, loser_context).is_ok());
    mgr.impl_InitOnceComplete(marshaler);
    EXPECT_EQ(cpu.gpr.rax, 0u);

    ASSERT_TRUE(memory.write_u32(pending_out, 0xFFFFFFFFu).is_ok());
    ASSERT_TRUE(memory.write_u32(context_out, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 4, init_once).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 8, 0x1u).is_ok());  // CHECK_ONLY
    ASSERT_TRUE(memory.write_u32(stack + 12, pending_out).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 16, context_out).is_ok());
    mgr.impl_InitOnceBeginInitialize(marshaler);
    ASSERT_EQ(cpu.gpr.rax, 1u);
    EXPECT_EQ(memory.read_u32(pending_out).value_or(1), 0u);
    EXPECT_EQ(memory.read_u32(context_out).value_or(0), winner_context);
}

TEST_F(Kernel32CpuzTest, InitOnceSyncWaiterObservesCompletion) {
    constexpr GuestAddr init_once = DATA_BASE;
    constexpr GuestAddr pending_out = DATA_BASE + 0x20;
    constexpr GuestAddr context_out = DATA_BASE + 0x30;
    constexpr GuestAddr waiter_pending_out = DATA_BASE + 0x40;
    constexpr GuestAddr waiter_context_out = DATA_BASE + 0x50;
    constexpr u32 context = 0x5000;
    ASSERT_TRUE(memory.write_u32(init_once, 0).is_ok());

    const GuestAddr stack = cpu.sp();
    ASSERT_TRUE(memory.write_u32(stack + 4, init_once).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 8, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 12, pending_out).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 16, context_out).is_ok());
    mgr.impl_InitOnceBeginInitialize(marshaler);
    ASSERT_EQ(cpu.gpr.rax, 1u);
    EXPECT_EQ(memory.read_u32(pending_out).value_or(0), 1u);
    EXPECT_EQ(memory.read_u32(init_once).value_or(0), 1u);

    GuestMemory* mem = &memory;
    Kernel32ExtManager* ext = &mgr;
    std::atomic<u32> waiter_ret{0xFFFFFFFFu};
    std::atomic<u32> waiter_pending{0xFFFFFFFFu};
    std::atomic<u32> waiter_context{0};

    std::thread waiter([&] {
        cpu::CpuState local_cpu;
        local_cpu.init_protected_mode();
        local_cpu.gpr.rsp = STACK_BASE + 0x700;
        Marshaler local_m{*mem};
        local_m.attach_cpu(local_cpu);
        const GuestAddr local_stack = local_cpu.sp();
        ASSERT_TRUE(mem->write_u32(local_stack + 4, init_once).is_ok());
        ASSERT_TRUE(mem->write_u32(local_stack + 8, 0).is_ok());
        ASSERT_TRUE(mem->write_u32(local_stack + 12, waiter_pending_out).is_ok());
        ASSERT_TRUE(mem->write_u32(local_stack + 16, waiter_context_out).is_ok());
        ext->impl_InitOnceBeginInitialize(local_m);
        waiter_ret.store(static_cast<u32>(local_cpu.gpr.rax),
                         std::memory_order_release);
        waiter_pending.store(
            mem->read_u32(waiter_pending_out).value_or(0xFFFFFFFFu),
            std::memory_order_release);
        waiter_context.store(mem->read_u32(waiter_context_out).value_or(0),
                             std::memory_order_release);
    });

    // Waiter should block in Begin until Complete publishes context.
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    EXPECT_EQ(waiter_ret.load(std::memory_order_acquire), 0xFFFFFFFFu);

    ASSERT_TRUE(memory.write_u32(stack + 4, init_once).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 8, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 12, context).is_ok());
    mgr.impl_InitOnceComplete(marshaler);
    ASSERT_EQ(cpu.gpr.rax, 1u);

    waiter.join();
    EXPECT_EQ(waiter_ret.load(), 1u);
    EXPECT_EQ(waiter_pending.load(), 0u);
    EXPECT_EQ(waiter_context.load(), context);
}

TEST_F(Kernel32CpuzTest, NtRunOnceMatchesWindowsReturnContextAndLastErrorBothAbis) {
    // tools/oracle_probe/rtl_runonce_probe{32,64}.expected.
    for (bool x64 : {false,true}) for (bool native : {false,true}) {
        if(x64)cpu.init_long_mode();else cpu.init_protected_mode();
        cpu.gpr.rsp=STACK_BASE+0x800;
        const GuestAddr once=DATA_BASE, out=DATA_BASE+0x20, callback=0x123400;
        for(unsigned scenario=0;scenario<6;++scenario){
            (void)marshaler.write_ptr(once,scenario>=4?0x1002:0);
            (void)marshaler.write_ptr(out,0xdead);
            unsigned calls=0;
            mgr.set_guest_call([&](GuestAddr function,const u64* args,usize count,bool is64)->u64{
                EXPECT_EQ(function,callback);EXPECT_EQ(count,3u);EXPECT_EQ(is64,x64);
                EXPECT_EQ(args[0],once);EXPECT_EQ(args[1],0x2000u);
                EXPECT_EQ(args[2],scenario==2?0:out);
                if(args[2])(void)marshaler.write_ptr(args[2],0x2000);
                set_last_error(0xbeef);++calls;return scenario==1?0:1;
            });
            const u64 args[]={once,scenario==5?0:callback,0x2000,scenario==2?0:out};
            if(x64){cpu.gpr.rcx=args[0];cpu.gpr.rdx=args[1];cpu.gpr.r8=args[2];cpu.gpr.r9=args[3];}
            else for(usize i=0;i<4;++i)(void)memory.write_u32(cpu.gpr.rsp+4+i*4,static_cast<u32>(args[i]));
            set_last_error(0x1234);
            if(native)mgr.impl_RtlRunOnceExecuteOnce(marshaler);else mgr.impl_InitOnceExecuteOnce(marshaler);
            EXPECT_EQ(static_cast<u32>(cpu.gpr.rax),scenario==1?(native?0xc0000001u:0u):(native?0u:1u));
            EXPECT_EQ(get_last_error(),scenario>=4?0x1234u:0xbeefu);
            EXPECT_EQ(marshaler.read_ptr(once).value(),scenario>=4?0x1002u:scenario==1?0u:scenario==2?2u:0x2002u);
            EXPECT_EQ(marshaler.read_ptr(out).value(),scenario>=4?0x1000u:scenario==2?0xdeadu:0x2000u);
            EXPECT_EQ(calls,scenario>=4?0u:1u);
        }
    }
}

TEST_F(Kernel32CpuzTest, NtRunOnceInvalidContextRaisesWindowsExceptionWithoutReset) {
    for(bool x64:{false,true}){
        if(x64)cpu.init_long_mode();else cpu.init_protected_mode();
        cpu.gpr.rsp=STACK_BASE+0x800;
        (void)marshaler.write_ptr(DATA_BASE,0);
        mgr.set_guest_call([&](GuestAddr,const u64* args,usize,bool)->u64{
            (void)marshaler.write_ptr(args[2],0x2001);return 1;
        });
        if(x64){cpu.gpr.rcx=DATA_BASE;cpu.gpr.rdx=0x1234;cpu.gpr.r8=0;cpu.gpr.r9=DATA_BASE+0x20;}
        else{(void)memory.write_u32(cpu.gpr.rsp+4,DATA_BASE);(void)memory.write_u32(cpu.gpr.rsp+8,0x1234);(void)memory.write_u32(cpu.gpr.rsp+12,0);(void)memory.write_u32(cpu.gpr.rsp+16,DATA_BASE+0x20);}
        mgr.impl_RtlRunOnceExecuteOnce(marshaler);
        EXPECT_TRUE(cpu.pending_raise.active);
        EXPECT_EQ(cpu.pending_raise.code,0xc00000f1u);
        EXPECT_EQ(cpu.pending_raise.flags,0x81u);
        EXPECT_EQ(marshaler.read_ptr(DATA_BASE).value(),1u);
    }
}
