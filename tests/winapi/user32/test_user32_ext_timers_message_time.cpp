/// @file test_user32_ext_timers_message_time.cpp
/// @brief Window timers, non-consuming message peeks and message time/position:
///        SetTimer/WM_TIMER delivery, PeekMessage, MsgWait, GetMessageTime, GetMessagePos.

#include "user32_ext_thunk_harness.hpp"

TEST_F(User32ExtThunkTest, WindowTimerExpiresOnlyOnItsOwnerThread) {
    const u32 previous_tid = ThreadManager::get_current_thread_id_static();
    ThreadManager::set_current_thread_id(5);

    WindowData data{};
    data.owner_thread_id = 5;
    const HWND hwnd = hwnd_mgr_.create(data);

    cpu_.mode = cpu::CpuMode::Long;
    cpu_.gpr.rcx = hwnd;
    cpu_.gpr.rdx = 124;
    cpu_.gpr.r8 = 1;
    cpu_.gpr.r9 = 0;
    Marshaler marshaler(memory_);
    marshaler.attach_cpu(cpu_);
    mgr_->impl_SetTimer(marshaler);
    EXPECT_EQ(cpu_.gpr.rax, 124u);

    std::this_thread::sleep_for(std::chrono::milliseconds(15));
    HWND timer_hwnd = 0;
    u64 timer_id = 0;
    GuestAddr callback = 0;
    EXPECT_FALSE(mgr_->find_expired_timer(timer_hwnd, timer_id, callback, 1));
    EXPECT_TRUE(mgr_->find_expired_timer(timer_hwnd, timer_id, callback, 5));
    EXPECT_EQ(timer_hwnd, hwnd);
    EXPECT_EQ(timer_id, 124u);
    EXPECT_EQ(callback, 0u);

    ThreadManager::set_current_thread_id(previous_tid);
}

TEST_F(User32ExtThunkTest, NonRemovingKeyboardPeekPreservesInputState) {
    MessageLoopManager mlm(hwnd_mgr_, queue_, bridge_);
    mlm.set_user32_ext_manager(mgr_.get());
    std::array<Byte, 256> initial{};
    ASSERT_TRUE(memory_.write_bytes(BUF_ADDR, initial.data(), initial.size()).is_ok());
    cpu_.mode = cpu::CpuMode::Long;
    cpu_.gpr.rcx = BUF_ADDR;
    Marshaler marshaler(memory_);
    marshaler.attach_cpu(cpu_);
    mgr_->impl_SetKeyboardState(marshaler);

    WindowData data{};
    data.owner_thread_id = ThreadManager::get_current_thread_id_static();
    MSG message{};
    message.hwnd = hwnd_mgr_.create(data);
    message.message = WM::KEYDOWN;
    message.wParam = 0x14; // VK_CAPITAL
    message.time = 123;
    queue_.post_input(message);
    MSG peeked{};
    for (int i = 0; i < 3; ++i) {
        ASSERT_TRUE(mlm.peek_message(peeked, 0, 0, 0, PM::NOREMOVE));
        EXPECT_EQ(mgr_->thread_key_state(0x14), 0);
        EXPECT_EQ(mgr_->last_message_time(), 123u);
    }
    ASSERT_TRUE(mlm.peek_message(peeked, 0, 0, 0, PM::REMOVE));
    EXPECT_EQ(static_cast<u16>(mgr_->thread_key_state(0x14)), 0xFF81u);

    message.wParam = 0x11; // VK_CONTROL
    queue_.post_input(message);
    ASSERT_TRUE(mlm.peek_message(peeked, 0, 0, 0, PM::REMOVE));
    ASSERT_TRUE(mlm.sync_modifier_down(0x11));
    message.message = WM::KEYUP;
    queue_.post_input(message);
    ASSERT_TRUE(mlm.peek_message(peeked, 0, 0, 0, PM::NOREMOVE));
    EXPECT_TRUE(mlm.sync_modifier_down(0x11));
    EXPECT_EQ(static_cast<u16>(mgr_->thread_key_state(0x11)), 0xFF80u);
    ASSERT_TRUE(mlm.peek_message(peeked, 0, 0, 0, PM::REMOVE));
    EXPECT_FALSE(mlm.sync_modifier_down(0x11));
    EXPECT_EQ(mgr_->thread_key_state(0x11), 0);
}

TEST_F(User32ExtThunkTest, PeekAndWaitMessageDoNotConsumeDueTimer) {
    const u32 previous_tid = ThreadManager::get_current_thread_id_static();
    ThreadManager::set_current_thread_id(5);

    WindowData data{};
    data.owner_thread_id = 5;
    const HWND hwnd = hwnd_mgr_.create(data);

    cpu_.mode = cpu::CpuMode::Long;
    cpu_.gpr.rcx = hwnd;
    cpu_.gpr.rdx = 125;
    cpu_.gpr.r8 = 1;
    cpu_.gpr.r9 = 0;
    Marshaler marshaler(memory_);
    marshaler.attach_cpu(cpu_);
    mgr_->impl_SetTimer(marshaler);
    EXPECT_EQ(cpu_.gpr.rax, 125u);

    std::this_thread::sleep_for(std::chrono::milliseconds(15));

    HWND timer_hwnd = 0;
    u64 timer_id = 0;
    GuestAddr callback = 0;
    EXPECT_TRUE(mgr_->find_expired_timer(timer_hwnd, timer_id, callback, 5, false));
    EXPECT_EQ(timer_hwnd, hwnd);
    EXPECT_EQ(timer_id, 125u);
    EXPECT_TRUE(mgr_->find_expired_timer(timer_hwnd, timer_id, callback, 5, false));
    EXPECT_EQ(timer_id, 125u);

    MessageLoopManager mlm(hwnd_mgr_, queue_, bridge_);
    mlm.set_user32_ext_manager(mgr_.get());
    mgr_->set_message_loop_manager(&mlm);

    MSG peeked{};
    ASSERT_TRUE(mlm.peek_message(peeked, 0, 0, 0, PM::NOREMOVE));
    EXPECT_EQ(peeked.message, WM::TIMER);
    EXPECT_EQ(peeked.hwnd, hwnd);
    EXPECT_EQ(peeked.wParam, 125u);

    EXPECT_TRUE(mgr_->wait_message());

    peeked = {};
    ASSERT_TRUE(mlm.peek_message(peeked, 0, 0, 0, PM::REMOVE));
    EXPECT_EQ(peeked.message, WM::TIMER);
    EXPECT_EQ(peeked.hwnd, hwnd);
    EXPECT_EQ(peeked.wParam, 125u);

    peeked = {};
    EXPECT_FALSE(mlm.peek_message(peeked, 0, 0, 0, PM::REMOVE));

    ThreadManager::set_current_thread_id(previous_tid);
}

TEST_F(User32ExtThunkTest, MsgWaitWakesForDueTimerWithoutConsumingIt) {
    WindowData data{};
    data.owner_thread_id = ThreadManager::get_current_thread_id_static();
    const HWND hwnd = hwnd_mgr_.create(data);
    cpu_.mode = cpu::CpuMode::Long;
    cpu_.gpr.rcx = hwnd;
    cpu_.gpr.rdx = 126;
    cpu_.gpr.r8 = 10;
    cpu_.gpr.r9 = 0;
    Marshaler marshaler(memory_);
    marshaler.attach_cpu(cpu_);
    mgr_->impl_SetTimer(marshaler);
    ASSERT_EQ(cpu_.gpr.rax, 126u);

    MessageLoopManager mlm(hwnd_mgr_, queue_, bridge_);
    mlm.set_user32_ext_manager(mgr_.get());
    EXPECT_EQ(mlm.msg_wait_for_multiple_objects(nullptr, 0, false, 200,
                                               QS::TIMER), 0u);
    EXPECT_EQ(mlm.msg_wait_for_multiple_objects(nullptr, 0, false, 0,
                                               QS::MOUSE), 258u);
    const u32 owner = data.owner_thread_id;
    ThreadManager::set_current_thread_id(owner + 1);
    EXPECT_EQ(mlm.msg_wait_for_multiple_objects(nullptr, 0, false, 0,
                                               QS::TIMER), 258u);
    ThreadManager::set_current_thread_id(owner);
    MSG timer{};
    ASSERT_TRUE(mlm.peek_message(timer, 0, WM::TIMER, WM::TIMER, PM::REMOVE));
    EXPECT_EQ(timer.message, WM::TIMER);
    EXPECT_EQ(timer.hwnd, hwnd);
    EXPECT_EQ(timer.wParam, 126u);
    EXPECT_FALSE(mlm.peek_message(timer, 0, WM::TIMER, WM::TIMER, PM::REMOVE));
}

TEST_F(User32ExtThunkTest, SynthesizedTimerMessageCarriesGetTickCountTime) {
    const u32 previous_tid = ThreadManager::get_current_thread_id_static();
    ThreadManager::set_current_thread_id(5);

    WindowData data{};
    data.owner_thread_id = 5;
    const HWND hwnd = hwnd_mgr_.create(data);

    cpu_.mode = cpu::CpuMode::Long;
    cpu_.gpr.rcx = hwnd;
    cpu_.gpr.rdx = 200;
    cpu_.gpr.r8 = 10;
    cpu_.gpr.r9 = 0;
    Marshaler marshaler(memory_);
    marshaler.attach_cpu(cpu_);
    mgr_->impl_SetTimer(marshaler);
    EXPECT_EQ(cpu_.gpr.rax, 200u);

    std::this_thread::sleep_for(std::chrono::milliseconds(15));

    MessageLoopManager mlm(hwnd_mgr_, queue_, bridge_);
    mlm.set_user32_ext_manager(mgr_.get());
    mgr_->set_message_loop_manager(&mlm);

    const u32 before = tick_count32();
    MSG peeked{};
    ASSERT_TRUE(mlm.peek_message(peeked, 0, 0, 0, PM::REMOVE));
    const u32 after = tick_count32();
    EXPECT_EQ(peeked.message, WM::TIMER);
    EXPECT_EQ(peeked.wParam, 200u);
    EXPECT_TRUE(tick_in_closed_range32(peeked.time, before, after));

    ThreadManager::set_current_thread_id(previous_tid);
}

TEST_F(User32ExtThunkTest, PeekMessageNoremoveUpdatesGetMessageTime) {
    const u32 previous_tid = ThreadManager::get_current_thread_id_static();
    ThreadManager::set_current_thread_id(5);

    WindowData data{};
    data.owner_thread_id = 5;
    const HWND hwnd = hwnd_mgr_.create(data);

    cpu_.mode = cpu::CpuMode::Long;
    cpu_.gpr.rcx = hwnd;
    cpu_.gpr.rdx = 201;
    cpu_.gpr.r8 = 10;
    cpu_.gpr.r9 = 0;
    Marshaler marshaler(memory_);
    marshaler.attach_cpu(cpu_);
    mgr_->impl_SetTimer(marshaler);

    std::this_thread::sleep_for(std::chrono::milliseconds(15));

    MessageLoopManager mlm(hwnd_mgr_, queue_, bridge_);
    mlm.set_user32_ext_manager(mgr_.get());

    MSG peeked{};
    ASSERT_TRUE(mlm.peek_message(peeked, 0, 0, 0, PM::NOREMOVE));
    EXPECT_EQ(peeked.message, WM::TIMER);
    mgr_->impl_GetMessageTime(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu_.gpr.rax), peeked.time);

    ThreadManager::set_current_thread_id(previous_tid);
}

TEST_F(User32ExtThunkTest, GetMessageTimeRetainsRetrievedTimestamp) {
    const u32 previous_tid = ThreadManager::get_current_thread_id_static();
    ThreadManager::set_current_thread_id(5);

    WindowData data{};
    data.owner_thread_id = 5;
    const HWND hwnd = hwnd_mgr_.create(data);

    MSG posted{};
    posted.hwnd = hwnd;
    posted.message = 0x0400; // WM_USER
    posted.time = 0x11223344u;
    posted.target_thread_id = 5;
    ASSERT_TRUE(queue_.post(posted));

    MessageLoopManager mlm(hwnd_mgr_, queue_, bridge_);
    mlm.set_user32_ext_manager(mgr_.get());

    MSG retrieved{};
    ASSERT_TRUE(mlm.peek_message(retrieved, 0, 0, 0, PM::REMOVE));
    EXPECT_EQ(retrieved.time, 0x11223344u);

    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    cpu_.mode = cpu::CpuMode::Long;
    Marshaler marshaler(memory_);
    marshaler.attach_cpu(cpu_);
    mgr_->impl_GetMessageTime(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu_.gpr.rax), 0x11223344u);
    EXPECT_NE(tick_count32(), 0x11223344u);

    ThreadManager::set_current_thread_id(previous_tid);
}

TEST_F(User32ExtThunkTest, GetMessageTimeIsPerGuestThread) {
    const u32 previous_tid = ThreadManager::get_current_thread_id_static();

    ThreadManager::set_current_thread_id(5);
    MSG first{};
    first.message = WM::TIMER;
    first.time = 1000;
    mgr_->note_retrieved_queue_message(first);
    cpu_.mode = cpu::CpuMode::Long;
    Marshaler marshaler(memory_);
    marshaler.attach_cpu(cpu_);
    mgr_->impl_GetMessageTime(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu_.gpr.rax), 1000u);

    ThreadManager::set_current_thread_id(7);
    MSG second{};
    second.message = WM::TIMER;
    second.time = 2000;
    mgr_->note_retrieved_queue_message(second);
    mgr_->impl_GetMessageTime(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu_.gpr.rax), 2000u);

    ThreadManager::set_current_thread_id(5);
    mgr_->impl_GetMessageTime(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu_.gpr.rax), 1000u);

    ThreadManager::set_current_thread_id(previous_tid);
}

TEST_F(User32ExtThunkTest, GetMessagePosRetainsSignedPerThreadMessagePosition) {
    const u32 previous_tid = ThreadManager::get_current_thread_id_static();
    cpu_.mode = cpu::CpuMode::Long;
    Marshaler marshaler(memory_);
    marshaler.attach_cpu(cpu_);
    ThreadManager::set_current_thread_id(5);
    MSG first{};
    first.message = WM::MOUSEMOVE;
    first.pt = {-200, 350};
    mgr_->note_retrieved_queue_message(first, false);
    mgr_->impl_GetMessagePos(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu_.gpr.rax), (350u << 16) | 0xff38u);
    ThreadManager::set_current_thread_id(7);
    MSG second{};
    second.pt = {70, -90};
    mgr_->note_retrieved_queue_message(second);
    mgr_->impl_GetMessagePos(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu_.gpr.rax), (0xffa6u << 16) | 70u);
    ThreadManager::set_current_thread_id(5);
    mgr_->impl_GetMessagePos(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu_.gpr.rax), (350u << 16) | 0xff38u);
    ThreadManager::set_current_thread_id(previous_tid);
}

TEST_F(User32ExtThunkTest, TimerProcDwTimeMatchesGetTickCount) {
    u32 observed_dw_time = 0;
    u32 observed_gettick = 0;
    mgr_->set_guest_call([&](GuestAddr, const u64* args, usize count, bool) -> u64 {
        EXPECT_EQ(count, 4u);
        observed_dw_time = static_cast<u32>(args[3]);
        observed_gettick = tick_count32();
        return 0;
    });
    mgr_->set_is_64bit(true);

    const u32 before = tick_count32();
    mgr_->invoke_timer_callback(0x1000, 9, 0x401000);
    const u32 after = tick_count32();
    EXPECT_TRUE(tick_in_closed_range32(observed_dw_time, before, after));
    EXPECT_TRUE(tick_in_closed_range32(observed_gettick, before, after));
    EXPECT_LE(tick_delta32(observed_gettick, observed_dw_time), 5u);
}

TEST_F(User32ExtThunkTest, SetTimerClampsBelowUserMinimumAndKeepsHwndIds) {
    const u32 previous_tid = ThreadManager::get_current_thread_id_static();
    ThreadManager::set_current_thread_id(5);

    WindowData data{};
    data.owner_thread_id = 5;
    const HWND hwnd = hwnd_mgr_.create(data);

    cpu_.mode = cpu::CpuMode::Long;
    Marshaler marshaler(memory_);
    marshaler.attach_cpu(cpu_);

    cpu_.gpr.rcx = hwnd;
    cpu_.gpr.rdx = 0;
    cpu_.gpr.r8 = 1;
    cpu_.gpr.r9 = 0;
    mgr_->impl_SetTimer(marshaler);
    // Learn: a window timer returns "a nonzero integer" -- 1 for nIDEvent 0
    // (tools/oracle_probe/s2msg_queue_probe.expected settimer_hwnd_id0).
    EXPECT_EQ(cpu_.gpr.rax, 1u);

    cpu_.gpr.rcx = hwnd;
    cpu_.gpr.rdx = 1;
    cpu_.gpr.r8 = 16;
    cpu_.gpr.r9 = 0;
    mgr_->impl_SetTimer(marshaler);
    EXPECT_EQ(cpu_.gpr.rax, 1u);

    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    HWND timer_hwnd = 0;
    u64 timer_id = 0;
    GuestAddr callback = 0;
    EXPECT_FALSE(mgr_->find_expired_timer(timer_hwnd, timer_id, callback, 5));

    std::this_thread::sleep_for(std::chrono::milliseconds(15));
    EXPECT_TRUE(mgr_->find_expired_timer(timer_hwnd, timer_id, callback, 5));
    EXPECT_TRUE(timer_id == 0u || timer_id == 1u);

    cpu_.gpr.rcx = hwnd;
    cpu_.gpr.rdx = 0;
    mgr_->impl_KillTimer(marshaler);
    EXPECT_EQ(cpu_.gpr.rax, 1u);
    cpu_.gpr.rdx = 1;
    mgr_->impl_KillTimer(marshaler);
    EXPECT_EQ(cpu_.gpr.rax, 1u);

    ThreadManager::set_current_thread_id(previous_tid);
}

TEST_F(User32ExtThunkTest, DelayedPumpDoesNotBurstMissedTimerIntervals) {
    const u32 previous_tid = ThreadManager::get_current_thread_id_static();
    ThreadManager::set_current_thread_id(5);

    WindowData data{};
    data.owner_thread_id = 5;
    const HWND hwnd = hwnd_mgr_.create(data);

    cpu_.mode = cpu::CpuMode::Long;
    cpu_.gpr.rcx = hwnd;
    cpu_.gpr.rdx = 202;
    cpu_.gpr.r8 = 10;
    cpu_.gpr.r9 = 0;
    Marshaler marshaler(memory_);
    marshaler.attach_cpu(cpu_);
    mgr_->impl_SetTimer(marshaler);

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    MessageLoopManager mlm(hwnd_mgr_, queue_, bridge_);
    mlm.set_user32_ext_manager(mgr_.get());

    MSG first{};
    ASSERT_TRUE(mlm.peek_message(first, 0, 0, 0, PM::REMOVE));
    EXPECT_EQ(first.message, WM::TIMER);
    EXPECT_EQ(first.wParam, 202u);

    MSG second{};
    EXPECT_FALSE(mlm.peek_message(second, 0, 0, 0, PM::REMOVE));

    ThreadManager::set_current_thread_id(previous_tid);
}

TEST_F(User32ExtThunkTest, TimerMutationIsSafeWhileAnotherThreadPolls) {
    const u32 previous_tid = ThreadManager::get_current_thread_id_static();
    ThreadManager::set_current_thread_id(5);

    WindowData data{};
    data.owner_thread_id = 5;
    const HWND hwnd = hwnd_mgr_.create(data);

    cpu_.mode = cpu::CpuMode::Long;
    Marshaler marshaler(memory_);
    marshaler.attach_cpu(cpu_);

    std::atomic<bool> running{true};
    std::thread poller([&] {
        HWND timer_hwnd = 0;
        u64 timer_id = 0;
        GuestAddr callback = 0;
        while (running.load(std::memory_order_acquire)) {
            (void)mgr_->find_expired_timer(
                timer_hwnd, timer_id, callback, 5, false);
        }
    });

    for (u32 id = 1; id <= 2000; ++id) {
        cpu_.gpr.rcx = hwnd;
        cpu_.gpr.rdx = id;
        cpu_.gpr.r8 = USER_TIMER_MINIMUM;
        cpu_.gpr.r9 = 0;
        mgr_->impl_SetTimer(marshaler);

        cpu_.gpr.rcx = hwnd;
        cpu_.gpr.rdx = id;
        mgr_->impl_KillTimer(marshaler);
        ASSERT_EQ(cpu_.gpr.rax, 1u);
    }

    running.store(false, std::memory_order_release);
    poller.join();
    ThreadManager::set_current_thread_id(previous_tid);
}
