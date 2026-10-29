/// @file test_message_wait.cpp
/// @brief Blocking message waits, queue order, timers, hooks and the
///        asynchronous-message rules (Steam audit S2, 2026-09-30).
///
/// Expected values come from Microsoft Learn or from Windows 11 as measured
/// by tools/oracle_probe/s2msg_queue_probe (.expected / .x86.expected); each
/// test names the probe line it pins.

#include <gtest/gtest.h>
#include <vitreo/common.hpp>
#include <winapi/user32/user32_ext.hpp>
#include <winapi/user32/messages/message_loop.hpp>
#include <winapi/user32/messages/message_send.hpp>
#include <winapi/user32/painting/painting.hpp>
#include <winapi/user32/classes/window_class.hpp>
#include <winapi/core/marshaler.hpp>
#include <winapi/kernel32/error.hpp>
#include <winapi/kernel32/process/process.hpp>
#include <winapi/kernel32/thread/thread.hpp>
#include <core/memory/guest_memory.hpp>
#include <core/cpu/state/cpu_state.hpp>
#include <capsule/vfs/vfs.hpp>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

using namespace vitreo;
using namespace vitreo::winapi;
using namespace vitreo::windowing;
using namespace std::chrono_literals;

namespace {

constexpr u32 kWmUser = 0x0400;

/// Wake latency bound: far below GetMessage's 1 s safety cap, loose enough
/// for a loaded build machine.
constexpr auto kPrompt = 250ms;

class MessageWaitTest : public ::testing::Test {
protected:
    static constexpr GuestAddr MEM_BASE = 0x1000;
    static constexpr GuestAddr STR_BASE = 0x6000;
    static constexpr GuestAddr STACK_BASE = 0x80000;

    void SetUp() override {
        (void)memory_.map_region(MEM_BASE, 0x10000, MemoryProtection::ReadWrite);
        (void)memory_.map_region(STACK_BASE, 0x10000, MemoryProtection::ReadWrite);
        ext_ = std::make_unique<User32ExtManager>(
            memory_, vfs_, hwnd_mgr_, class_mgr_, bridge_, queue_);
        ext_->set_process_manager(&process_mgr_);
        mlm_ = std::make_unique<MessageLoopManager>(hwnd_mgr_, queue_, bridge_);
        mlm_->set_user32_ext_manager(ext_.get());
        mlm_->set_painting_manager(&paint_);
        ext_->set_message_loop_manager(mlm_.get());
        ext_->set_painting_manager(&paint_);
        // The same wiring RuntimeContext installs.
        paint_.set_invalidate_callback([this](u64 hwnd) {
            const auto owner = hwnd_mgr_.owner_thread_id(static_cast<HWND>(hwnd));
            if (owner) queue_.notify_thread(*owner, QS::PAINT);
        });
        msm_ = std::make_unique<MessageSendManager>(hwnd_mgr_, queue_, bridge_);
        cpu_.mode = cpu::CpuMode::Long;
        cpu_.gpr.rsp = STACK_BASE + 0x800;
        previous_tid_ = ThreadManager::get_current_thread_id_static();
    }

    void TearDown() override {
        queue_.begin_shutdown();
        for (auto& t : threads_) {
            if (t.joinable()) t.join();
        }
        ThreadManager::set_current_thread_id(previous_tid_);
    }

    HWND make_window(u32 owner, u32 style = WS::VISIBLE, HWND parent = HWND_DESKTOP) {
        WindowData data{};
        data.title = L"w";
        data.wndproc = 0x401000;
        data.parent = parent;
        data.owner_thread_id = owner;
        data.style = style;
        data.visible = (style & WS::VISIBLE) != 0;
        return hwnd_mgr_.create(data);
    }

    /// Call an x64 thunk with up to four register arguments.
    u64 call(const std::function<void(Marshaler&)>& fn, u64 a0 = 0, u64 a1 = 0,
             u64 a2 = 0, u64 a3 = 0) {
        cpu_.gpr.rcx = a0;
        cpu_.gpr.rdx = a1;
        cpu_.gpr.r8 = a2;
        cpu_.gpr.r9 = a3;
        Marshaler m(memory_);
        m.attach_cpu(cpu_);
        fn(m);
        return cpu_.gpr.rax;
    }

    ExportEntry ext_export(const std::string& name) {
        for (auto& e : create_user32_ext_thunks(*ext_).entries) {
            if (e.name == name) return e;
        }
        ADD_FAILURE() << name;
        return {};
    }

    ExportEntry send_export(const std::string& name) {
        for (auto& e : make_message_send_exports(*msm_).entries) {
            if (e.name == name) return e;
        }
        ADD_FAILURE() << name;
        return {};
    }

    struct Blocked {
        std::atomic<bool> returned{false};
        MSG msg{};
        bool result = true;
        std::chrono::steady_clock::time_point at{};
    };

    /// GetMessage on a guest thread @p tid, parked before this returns.
    void start_get_message(Blocked& b, u32 tid) {
        const u64 parked_before = queue_.thread_wait_count();
        threads_.emplace_back([this, &b, tid] {
            ThreadManager::set_current_thread_id(tid);
            b.result = mlm_->get_message(b.msg);
            b.at = std::chrono::steady_clock::now();
            b.returned.store(true, std::memory_order_release);
        });
        const auto deadline = std::chrono::steady_clock::now() + 2s;
        while (queue_.thread_wait_count() == parked_before &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(1ms);
        }
        std::this_thread::sleep_for(20ms);
        ASSERT_FALSE(b.returned.load());
    }

    static bool wait_returned(Blocked& b, std::chrono::milliseconds limit) {
        const auto deadline = std::chrono::steady_clock::now() + limit;
        while (!b.returned.load(std::memory_order_acquire)) {
            if (std::chrono::steady_clock::now() >= deadline) return false;
            std::this_thread::sleep_for(1ms);
        }
        return true;
    }

    GuestAddr wide(const char* s, GuestAddr at) {
        Marshaler m(memory_);
        (void)m.write_wide_string(at, s);
        return at;
    }
    GuestAddr ansi(const char* s, GuestAddr at) {
        Marshaler m(memory_);
        (void)m.write_ansi_string(at, s);
        return at;
    }

    GuestMemory memory_;
    capsule::Vfs vfs_;
    HwndManager hwnd_mgr_;
    WindowClassManager class_mgr_;
    CocoaBridge bridge_;
    MessageQueue queue_;
    ProcessManager process_mgr_;
    PaintingManager paint_{&hwnd_mgr_};
    std::unique_ptr<User32ExtManager> ext_;
    std::unique_ptr<MessageLoopManager> mlm_;
    std::unique_ptr<MessageSendManager> msm_;
    cpu::CpuState cpu_;
    u32 previous_tid_ = 0;
    std::vector<std::thread> threads_;
};

// -----------------------------------------------------------------------------
// GetMessage is a blocking wait, woken by each of its sources
// -----------------------------------------------------------------------------

TEST_F(MessageWaitTest, PostedMessageWakesBlockedGetMessage) {
    const HWND w = make_window(7);
    Blocked b;
    start_get_message(b, 7);
    const auto t0 = std::chrono::steady_clock::now();
    MSG m{};
    m.hwnd = w;
    m.message = kWmUser + 1;
    m.target_thread_id = 7;
    ASSERT_TRUE(queue_.post(m));
    ASSERT_TRUE(wait_returned(b, 2s));
    EXPECT_LT(b.at - t0, kPrompt);
    EXPECT_TRUE(b.result);
    EXPECT_EQ(b.msg.message, kWmUser + 1);
}

TEST_F(MessageWaitTest, TimerArmedByAnotherThreadWakesBlockedGetMessage) {
    const HWND w = make_window(7);
    Blocked b;
    start_get_message(b, 7);
    // Any thread may arm a window's timer; WM_TIMER goes to the window's
    // thread (s2msg_queue_probe settimer_other_thread_window_armed,
    // owner_thread_sees_timer).
    ThreadManager::set_current_thread_id(3);
    const auto t0 = std::chrono::steady_clock::now();
    EXPECT_EQ(call([&](Marshaler& m) { ext_->impl_SetTimer(m); }, w, 5, 60, 0), 5u);
    ASSERT_TRUE(wait_returned(b, 2s));
    const auto waited = b.at - t0;
    EXPECT_GE(waited, 40ms);
    EXPECT_LT(waited, 60ms + kPrompt);
    EXPECT_EQ(b.msg.message, WM::TIMER);
    EXPECT_EQ(b.msg.hwnd, w);
    EXPECT_EQ(b.msg.wParam, 5u);
}

TEST_F(MessageWaitTest, CrossThreadSendRunsInsideBlockedGetMessage) {
    const HWND w = make_window(7);
    std::atomic<u32> ran_on{0};
    msm_->set_wndproc_callback([&](HWND, u32 msg, u64, u64, u64) -> u64 {
        ran_on = ThreadManager::get_current_thread_id_static();
        return msg == kWmUser + 5 ? 77u : 0u;
    });
    Blocked b;
    start_get_message(b, 7);
    std::atomic<u64> result{0};
    std::atomic<bool> sent{false};
    const auto t0 = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point t1{};
    std::thread sender([&] {
        ThreadManager::set_current_thread_id(3);
        result = msm_->send_message(w, kWmUser + 5, 0, 0);
        t1 = std::chrono::steady_clock::now();
        sent = true;
    });
    sender.join();
    EXPECT_TRUE(sent);
    EXPECT_EQ(result.load(), 77u);
    EXPECT_EQ(ran_on.load(), 7u);
    EXPECT_LT(t1 - t0, kPrompt);
    // A sent message is dispatched inside GetMessage, never returned by it.
    EXPECT_FALSE(b.returned.load());
    queue_.post_quit(0, 7);
    ASSERT_TRUE(wait_returned(b, 2s));
}

TEST_F(MessageWaitTest, InvalidateRectWakesBlockedGetMessage) {
    const HWND w = make_window(7);
    Blocked b;
    start_get_message(b, 7);
    const auto t0 = std::chrono::steady_clock::now();
    ASSERT_TRUE(paint_.invalidate_rect(w, nullptr, false));
    ASSERT_TRUE(wait_returned(b, 2s));
    EXPECT_LT(b.at - t0, kPrompt);
    EXPECT_EQ(b.msg.message, WM::PAINT);
    EXPECT_EQ(b.msg.hwnd, w);
}

TEST_F(MessageWaitTest, QuitWakesBlockedGetMessage) {
    make_window(7);
    Blocked b;
    start_get_message(b, 7);
    const auto t0 = std::chrono::steady_clock::now();
    queue_.post_quit(3, 7);
    ASSERT_TRUE(wait_returned(b, 2s));
    EXPECT_LT(b.at - t0, kPrompt);
    EXPECT_FALSE(b.result);
    EXPECT_EQ(b.msg.message, WM::QUIT);
    EXPECT_EQ(b.msg.wParam, 3u);
}

TEST_F(MessageWaitTest, IdleGetMessageParksInsteadOfPolling) {
    // The old loop re-checked every 1 ms (~2000 wakeups a second per idle GUI
    // thread). Idle, GetMessage parks once and stays parked.
    make_window(7);
    Blocked b;
    start_get_message(b, 7);
    const u64 before = queue_.thread_wait_count();
    std::this_thread::sleep_for(300ms);
    EXPECT_LE(queue_.thread_wait_count() - before, 1u);
    queue_.post_quit(0, 7);
    ASSERT_TRUE(wait_returned(b, 2s));
}

TEST_F(MessageWaitTest, WaitMessageIgnoresAlreadySeenInput) {
    // s2msg_queue_probe waitmessage: a post already looked at by PeekMessage
    // does not end WaitMessage; a new one does.
    ThreadManager::set_current_thread_id(7);
    const HWND w = make_window(7);
    MSG old{};
    old.hwnd = w;
    old.message = kWmUser + 31;
    ASSERT_TRUE(queue_.post(old));
    MSG seen{};
    EXPECT_FALSE(mlm_->peek_message(seen, 0, kWmUser + 100, kWmUser + 100, PM::NOREMOVE));
    std::thread poster([&] {
        std::this_thread::sleep_for(150ms);
        MSG fresh{};
        fresh.hwnd = w;
        fresh.message = kWmUser + 30;
        (void)queue_.post(fresh);
    });
    const auto t0 = std::chrono::steady_clock::now();
    EXPECT_TRUE(ext_->wait_message());
    EXPECT_GE(std::chrono::steady_clock::now() - t0, 100ms);
    poster.join();
}

// -----------------------------------------------------------------------------
// Retrieval order and filters
// -----------------------------------------------------------------------------

TEST_F(MessageWaitTest, RetrievalOrderPostedThenQuitThenInputThenPaintThenTimer) {
    // s2msg_queue_probe "order: S U1 U2 U3(thread) QUIT PAINT TIMER"; the
    // synthesized WM_MOUSEMOVE Windows generated came after WM_QUIT.
    ThreadManager::set_current_thread_id(7);
    const HWND w = make_window(7);
    EXPECT_EQ(call([&](Marshaler& m) { ext_->impl_SetTimer(m); }, w, 1, 10, 0), 1u);
    std::this_thread::sleep_for(30ms);
    ASSERT_TRUE(paint_.invalidate_rect(w, nullptr, false));
    MSG input{};
    input.hwnd = w;
    input.message = WM::MOUSEMOVE;
    ASSERT_TRUE(queue_.post_input(input));
    MSG posted{};
    posted.hwnd = w;
    posted.message = kWmUser + 1;
    ASSERT_TRUE(queue_.post(posted));
    queue_.post_quit(5, 7);
    posted.message = kWmUser + 2;
    ASSERT_TRUE(queue_.post(posted));

    std::vector<u32> order;
    MSG m{};
    for (int i = 0; i < 6 && mlm_->peek_message(m, 0, 0, 0, PM::REMOVE); ++i) {
        order.push_back(m.message);
        if (m.message == WM::PAINT) (void)paint_.validate_rect(w, nullptr);
    }
    const std::vector<u32> expected{kWmUser + 1, kWmUser + 2, WM::QUIT,
                                    WM::MOUSEMOVE, WM::PAINT, WM::TIMER};
    EXPECT_EQ(order, expected);
}

TEST_F(MessageWaitTest, FiltersFollowWindows) {
    ThreadManager::set_current_thread_id(7);
    const HWND parent = make_window(7);
    const HWND child = make_window(7, WS::CHILD | WS::VISIBLE, parent);
    MSG m{};
    MSG out{};
    // A parent hWnd filter also retrieves its children's messages
    // (parent_filter_child_msg = 0x405).
    m.hwnd = child;
    m.message = kWmUser + 5;
    ASSERT_TRUE(queue_.post(m));
    ASSERT_TRUE(mlm_->peek_message(out, parent, kWmUser + 5, kWmUser + 5, PM::REMOVE));
    EXPECT_EQ(out.hwnd, child);
    // (HWND)-1 selects thread messages only (filter_hwnd_minus1_thread_msg).
    m.hwnd = parent;
    m.message = kWmUser + 1;
    ASSERT_TRUE(queue_.post(m));
    MSG thread_msg{};
    thread_msg.message = kWmUser + 3;
    thread_msg.target_thread_id = 7;
    ASSERT_TRUE(queue_.post(thread_msg));
    ASSERT_TRUE(mlm_->peek_message(out, static_cast<HWND>(-1), 0, 0, PM::REMOVE));
    EXPECT_EQ(out.message, kWmUser + 3u);
    // An inverted range matches no posted message (inverted_range_1..3) ...
    EXPECT_FALSE(mlm_->peek_message(out, 0, kWmUser + 2, kWmUser, PM::NOREMOVE));
    // ... but still yields WM_PAINT (inverted_range_paint).
    ASSERT_TRUE(paint_.invalidate_rect(parent, nullptr, false));
    ASSERT_TRUE(mlm_->peek_message(out, 0, kWmUser + 2, kWmUser, PM::NOREMOVE));
    EXPECT_EQ(out.message, WM::PAINT);
}

TEST_F(MessageWaitTest, PmQsPaintRunsPendingSendAndReturnsFalse) {
    // pm_qs_paint_with_pending_send: FALSE, send_dispatched=1; then
    // pm_qs_paint_after_send finds the paint. PM_QS_POSTMESSAGE leaves the
    // send queued (pm_qs_postmessage_with_pending_send).
    const HWND w = make_window(7);
    std::atomic<int> sends{0};
    msm_->set_wndproc_callback([&](HWND, u32, u64, u64, u64) -> u64 {
        ++sends;
        return 0;
    });
    std::thread sender([&] {
        ThreadManager::set_current_thread_id(3);
        (void)msm_->send_message(w, kWmUser + 50, 0, 0);
    });
    ThreadManager::set_current_thread_id(7);
    while (queue_.get_queue_status(QS::SENDMESSAGE, 7, hwnd_mgr_) == 0) {
        std::this_thread::sleep_for(1ms);
    }
    ASSERT_TRUE(paint_.invalidate_rect(w, nullptr, false));
    MSG out{};
    constexpr u32 PM_QS_POSTMESSAGE = (QS::POSTMESSAGE | QS::HOTKEY | QS::TIMER) << 16;
    constexpr u32 PM_QS_PAINT = QS::PAINT << 16;
    EXPECT_FALSE(mlm_->peek_message(out, 0, 0, 0, PM::NOREMOVE | PM_QS_POSTMESSAGE));
    EXPECT_EQ(sends.load(), 0);
    EXPECT_FALSE(mlm_->peek_message(out, 0, 0, 0, PM::NOREMOVE | PM_QS_PAINT));
    EXPECT_EQ(sends.load(), 1);
    ASSERT_TRUE(mlm_->peek_message(out, 0, 0, 0, PM::NOREMOVE | PM_QS_PAINT));
    EXPECT_EQ(out.message, WM::PAINT);
    sender.join();
}

TEST_F(MessageWaitTest, QueueStatusChangeBitsArePerThreadAndReportSynthesized) {
    ThreadManager::set_current_thread_id(7);
    const HWND w = make_window(7);
    make_window(8);
    const auto status = [&](u32 flags) {
        return static_cast<u32>(call([&](Marshaler& m) { ext_->impl_GetQueueStatus(m); }, flags));
    };
    EXPECT_EQ(status(QS::ALLINPUT), 0u);
    // Another thread's post changes nothing here (status_other_thread_post).
    MSG other{};
    other.message = kWmUser;
    other.target_thread_id = 8;
    ASSERT_TRUE(queue_.post(other));
    EXPECT_EQ(status(QS::ALLINPUT), 0u);
    ASSERT_TRUE(paint_.invalidate_rect(w, nullptr, false));
    EXPECT_EQ(status(QS::ALLINPUT), 0x00200020u);  // status_paint
    (void)paint_.validate_rect(w, nullptr);
    EXPECT_EQ(call([&](Marshaler& m) { ext_->impl_SetTimer(m); }, w, 4, 10, 0), 4u);
    std::this_thread::sleep_for(30ms);
    EXPECT_EQ(status(QS::ALLINPUT), 0x00100010u);  // status_timer
    EXPECT_EQ(call([&](Marshaler& m) { ext_->impl_KillTimer(m); }, w, 4), 1u);
    EXPECT_EQ(status(QS::ALLINPUT), 0u);           // status_after_killtimer
    set_last_error(0xDEAD);
    EXPECT_EQ(status(0x80000000u), 0u);            // status_invalid_flags
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_FLAGS);
    set_last_error(0xDEAD);
    EXPECT_EQ(status(0x200u), 0u);                 // status_bit_200_rejected
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_FLAGS);
}

// -----------------------------------------------------------------------------
// Timers
// -----------------------------------------------------------------------------

TEST_F(MessageWaitTest, SetTimerIdsAndErrorsMatchWindows) {
    ThreadManager::set_current_thread_id(7);
    const HWND w = make_window(7);
    const auto set_timer = [&](u64 hwnd, u64 id, u32 elapse, u64 proc = 0) {
        return call([&](Marshaler& m) { ext_->impl_SetTimer(m); }, hwnd, id, elapse, proc);
    };
    const auto kill_timer = [&](u64 hwnd, u64 id) {
        return call([&](Marshaler& m) { ext_->impl_KillTimer(m); }, hwnd, id);
    };
    EXPECT_EQ(set_timer(w, 0, 50), 1u);                   // settimer_hwnd_id0
    EXPECT_EQ(set_timer(w, 7, 50), 7u);                   // settimer_hwnd_id7
    EXPECT_EQ(kill_timer(w, 7), 1u);
    set_last_error(0xDEAD);
    EXPECT_EQ(kill_timer(w, 7), 0u);                      // killtimer_7_again
    EXPECT_EQ(get_last_error(), 0xDEADu);
    EXPECT_EQ(kill_timer(0x1234, 1), 0u);                 // killtimer_invalid_hwnd
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_WINDOW_HANDLE);
    set_last_error(0xDEAD);
    EXPECT_EQ(set_timer(0x1234, 1, 50), 0u);              // settimer_invalid_hwnd
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_WINDOW_HANDLE);
    // NULL window, NULL TIMERPROC: a thread timer (settimer_null_null).
    const u64 t1 = set_timer(0, 0, 50);
    EXPECT_NE(t1, 0u);
    EXPECT_EQ(set_timer(0, t1, 60), t1);                  // settimer_null_replace
    const u64 t3 = set_timer(0, 0x5555, 60);
    EXPECT_NE(t3, 0x5555u);                               // settimer_null_unknown_id
    EXPECT_NE(t3, 0u);
    EXPECT_EQ(kill_timer(w, t1), 0u);                     // killtimer_hwnd_t1
    EXPECT_EQ(kill_timer(0, t1), 1u);
    EXPECT_EQ(kill_timer(0, t3), 1u);
    // UINT_PTR ids survive whole on x64 (settimer_wide_id, wide_id_matches).
    const u64 wide = 0x123456789abcull;
    EXPECT_EQ(set_timer(w, wide, 10), wide);
    std::this_thread::sleep_for(30ms);
    MSG out{};
    ASSERT_TRUE(mlm_->peek_message(out, w, WM::TIMER, WM::TIMER, PM::REMOVE));
    EXPECT_EQ(out.wParam, wide);
    EXPECT_EQ(kill_timer(w, wide), 1u);
    EXPECT_EQ(kill_timer(w, 0), 1u);
}

TEST_F(MessageWaitTest, ThreadTimerMatchesOnlyNullWindowFilter) {
    // thread_timer_minus1_filter / thread_timer_hwnd_filter: none;
    // thread_timer_null_filter: WM_TIMER.
    ThreadManager::set_current_thread_id(7);
    const HWND w = make_window(7);
    const u64 id = call([&](Marshaler& m) { ext_->impl_SetTimer(m); }, 0, 0, 10, 0);
    ASSERT_NE(id, 0u);
    std::this_thread::sleep_for(30ms);
    MSG out{};
    EXPECT_FALSE(mlm_->peek_message(out, static_cast<HWND>(-1), WM::TIMER, WM::TIMER, PM::REMOVE));
    EXPECT_FALSE(mlm_->peek_message(out, w, WM::TIMER, WM::TIMER, PM::REMOVE));
    ASSERT_TRUE(mlm_->peek_message(out, 0, WM::TIMER, WM::TIMER, PM::REMOVE));
    EXPECT_EQ(out.hwnd, NULL_HWND);
    EXPECT_EQ(out.wParam, id);
    EXPECT_EQ(out.lParam, 0u);
}

TEST_F(MessageWaitTest, DispatchDropsWmTimerNamingNoRegisteredTimerProc) {
    // fake_timerproc_msg: DispatchMessage returns 0; neither the named
    // procedure nor the window procedure runs.
    const HWND w = make_window(0);
    int guest_calls = 0;
    int wndproc_calls = 0;
    ext_->set_guest_call([&](GuestAddr, const u64*, usize, bool) -> u64 {
        ++guest_calls;
        return 0;
    });
    mlm_->set_wndproc_callback([&](HWND, u32, u64, u64, u64) -> u64 {
        ++wndproc_calls;
        return 5;
    });
    MSG fake{};
    fake.hwnd = w;
    fake.message = WM::TIMER;
    fake.wParam = 77;
    fake.lParam = 0x405000;
    EXPECT_EQ(mlm_->dispatch_message(fake), 0u);
    EXPECT_EQ(guest_calls, 0);
    EXPECT_EQ(wndproc_calls, 0);
    // A registered TIMERPROC is called instead of the window procedure.
    EXPECT_EQ(call([&](Marshaler& m) { ext_->impl_SetTimer(m); }, w, 10, 50, 0x405000), 10u);
    EXPECT_EQ(mlm_->dispatch_message(fake), 0u);
    EXPECT_EQ(guest_calls, 1);
    EXPECT_EQ(wndproc_calls, 0);
}

TEST_F(MessageWaitTest, CoalescableTimerToleranceRange) {
    // coalescable_default / _no / _max succeed; _over / _ffff_fffe fail with
    // ERROR_INVALID_PARAMETER.
    const HWND w = make_window(0);
    auto e = ext_export("SetCoalescableTimer");
    const auto set = [&](u64 id, u32 tolerance) {
        (void)memory_.write_u64(cpu_.gpr.rsp + 0x28, tolerance);
        return call(e.handler, w, id, 50, 0);
    };
    EXPECT_EQ(set(12, 0), 12u);
    EXPECT_EQ(set(13, 0xFFFFFFFFu), 13u);
    EXPECT_EQ(set(14, 0x7FFFFFF5u), 14u);
    set_last_error(0xDEAD);
    EXPECT_EQ(set(15, 0x7FFFFFF6u), 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_PARAMETER);
    EXPECT_EQ(set(16, 0xFFFFFFFEu), 0u);
}

// -----------------------------------------------------------------------------
// Asynchronous messages, sends
// -----------------------------------------------------------------------------

TEST_F(MessageWaitTest, AsynchronousFunctionsRefuseSyncOnlyMessages) {
    const HWND w = make_window(0);
    auto post = send_export("PostMessageW");
    auto post_thread = send_export("PostThreadMessageW");
    auto notify = send_export("SendNotifyMessageW");
    msm_->set_thread_alive_query([](u32 tid) { return tid == 7; });
    set_last_error(0xDEAD);
    EXPECT_EQ(call(post.handler, w, 0x000C /* WM_SETTEXT */, 0, STR_BASE), 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_MESSAGE_SYNC_ONLY);   // post_settext_ptr
    EXPECT_EQ(call(post.handler, w, 0x004A /* WM_COPYDATA */, 0, 0), 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_MESSAGE_SYNC_ONLY);   // post_copydata
    EXPECT_EQ(call(post.handler, 0x1234, 0x000C, 0, 0), 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_WINDOW_HANDLE);  // post_invalid_hwnd_settext
    EXPECT_EQ(call(notify.handler, w, 0x000C, 0, 0), 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_MESSAGE_SYNC_ONLY);   // sendnotify_settext_same_thread
    EXPECT_EQ(call(post.handler, w, 0x10005, 0, 0), 1u);                 // post_msg_over_ffff
    EXPECT_EQ(call(post_thread.handler, 9, kWmUser, 0, 0), 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_THREAD_ID);   // post_thread_bogus
    EXPECT_EQ(call(post_thread.handler, 0, kWmUser, 0, 0), 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_THREAD_ID);   // post_thread_zero
    EXPECT_EQ(call(post_thread.handler, 7, 0x000C, 0, 0), 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_MESSAGE_SYNC_ONLY);   // post_thread_settext
    EXPECT_EQ(call(post_thread.handler, 7, kWmUser, 0, 0), 1u);
}

TEST_F(MessageWaitTest, InSendMessageExReportsTheSendKind) {
    // same_thread insendex=0; cross mode=0 insendex=1; mode=1 ReplyMessage
    // TRUE, 0x109 (SEND then SEND|REPLIED), sender gets 42; mode=2 (notify) 2.
    EXPECT_EQ(MessageQueue::current_send_flags(), 0u);
    EXPECT_FALSE(MessageQueue::reply_message(1));  // reply_not_in_send
    std::atomic<u32> seen{0xFFFF};
    std::atomic<u32> after_reply{0xFFFF};
    std::thread receiver([&] {
        ThreadManager::set_current_thread_id(7);
        while (!queue_.shutdown_requested()) {
            queue_.dispatch_sent_messages(7);
            std::this_thread::sleep_for(1ms);
        }
    });
    u64 r = queue_.send_to_thread(3, 7, [&]() -> u64 {
        seen = MessageQueue::current_send_flags();
        return 61;
    });
    EXPECT_EQ(r, 61u);
    EXPECT_EQ(seen.load(), 1u);
    r = queue_.send_to_thread(3, 7, [&]() -> u64 {
        EXPECT_TRUE(MessageQueue::reply_message(42));
        after_reply = MessageQueue::current_send_flags();
        return 61;
    });
    EXPECT_EQ(r, 42u);
    EXPECT_EQ(after_reply.load(), 9u);
    std::atomic<bool> notified{false};
    ASSERT_TRUE(queue_.notify_thread_send(3, 7, [&]() -> u64 {
        seen = MessageQueue::current_send_flags();
        EXPECT_FALSE(MessageQueue::reply_message(1));
        notified = true;
        return 0;
    }));
    while (!notified) std::this_thread::sleep_for(1ms);
    EXPECT_EQ(seen.load(), 2u);
    queue_.begin_shutdown();
    receiver.join();
}

TEST_F(MessageWaitTest, RegisterWindowMessageSharesTheClipboardAtomTable) {
    // regmsg range_ok=1 same_ci=1 same_as_clipfmt=1; regmsg_empty 0 with the
    // last error untouched; regmsg_299_chars 0 / ERROR_MORE_DATA.
    const u64 a = call(ext_export("RegisterWindowMessageW").handler,
                       wide("VitreoAtomS2", STR_BASE));
    const u64 b = call(ext_export("RegisterWindowMessageA").handler,
                       ansi("vitreoatoms2", STR_BASE + 0x100));
    const u64 c = call(ext_export("RegisterClipboardFormatW").handler,
                       wide("VITREOATOMS2", STR_BASE + 0x200));
    EXPECT_GE(a, 0xC000u);
    EXPECT_LE(a, 0xFFFFu);
    EXPECT_EQ(a, b);
    EXPECT_EQ(a, c);
    set_last_error(0xDEAD);
    EXPECT_EQ(call(ext_export("RegisterWindowMessageW").handler, wide("", STR_BASE + 0x300)), 0u);
    EXPECT_EQ(get_last_error(), 0xDEADu);
    const std::string long_name(299, 'a');
    EXPECT_EQ(call(ext_export("RegisterWindowMessageA").handler,
                   ansi(long_name.c_str(), STR_BASE + 0x400)), 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_MORE_DATA);
}

// -----------------------------------------------------------------------------
// Hooks
// -----------------------------------------------------------------------------

TEST_F(MessageWaitTest, SetWindowsHookExErrorsMatchWindows) {
    ThreadManager::set_current_thread_id(7);
    ext_->set_thread_alive_query([](u32 tid) { return tid == 7; });
    const auto hook = [&](i64 id, u64 proc, u64 hmod, u64 tid) {
        set_last_error(0xDEAD);
        return call([&](Marshaler& m) { ext_->impl_SetWindowsHookExW(m); },
                    static_cast<u64>(id), proc, hmod, tid);
    };
    EXPECT_EQ(hook(99, 0x401000, 0, 7), 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_HOOK_FILTER);  // hook_invalid_id
    EXPECT_EQ(hook(3, 0, 0, 7), 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_FILTER_PROC);  // hook_null_proc
    EXPECT_EQ(hook(3, 0x401000, 0, 0x7ffffff0), 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_PARAMETER);    // hook_bogus_thread
    EXPECT_EQ(hook(3, 0x401000, 0, 0), 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_HOOK_NEEDS_HMOD);      // hook_global_no_hmod
    EXPECT_EQ(hook(0, 0x401000, 0x10000000, 0), 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_ACCESS_DENIED);        // hook_journalrecord
    EXPECT_EQ(hook(13, 0x401000, 0, 7), 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_GLOBAL_ONLY_HOOK);     // hook_keyboard_ll_thread
    const u64 ll = hook(13, 0x401000, 0, 0);                              // hook_keyboard_ll_null_hmod
    EXPECT_NE(ll, 0u);
    EXPECT_EQ(hook(8, 0x401000, 0, 7) != 0, true);                        // hook_id 8 thread ok=1
    set_last_error(0xDEAD);
    EXPECT_EQ(call([&](Marshaler& m) { ext_->impl_UnhookWindowsHookEx(m); }, ll), 1u);
    EXPECT_EQ(get_last_error(), 0xDEADu);
    EXPECT_EQ(call([&](Marshaler& m) { ext_->impl_UnhookWindowsHookEx(m); }, ll), 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_HOOK_HANDLE);  // unhook_b_again
}

TEST_F(MessageWaitTest, CallNextHookExWalksTheChainNewestFirst) {
    // getmsg_chain n=2 order=BA next_ret=aa msg_wparam=a.
    ThreadManager::set_current_thread_id(7);
    const HWND w = make_window(7);
    constexpr GuestAddr kHookA = 0x40A000;
    constexpr GuestAddr kHookB = 0x40B000;
    std::vector<char> order;
    u64 next_ret = 0;
    ext_->set_guest_call([&](GuestAddr proc, const u64* args, usize, bool) -> u64 {
        if (proc == kHookB) {
            order.push_back('B');
            cpu::CpuState saved = cpu_;
            next_ret = call([&](Marshaler& m) { ext_->impl_CallNextHookEx(m); },
                            0, args[0], args[1], args[2]);
            cpu_ = saved;
            return 0xBB;
        }
        if (proc == kHookA) {
            order.push_back('A');
            // The MSG the caller is about to receive may be edited.
            (void)memory_.write_u64(args[2] + 16, 0xA);
            return 0xAA;
        }
        return 0;
    });
    ASSERT_NE(call([&](Marshaler& m) { ext_->impl_SetWindowsHookExW(m); }, 3, kHookA, 0, 7), 0u);
    ASSERT_NE(call([&](Marshaler& m) { ext_->impl_SetWindowsHookExW(m); }, 3, kHookB, 0, 7), 0u);
    MSG posted{};
    posted.hwnd = w;
    posted.message = kWmUser + 20;
    posted.wParam = 1;
    ASSERT_TRUE(queue_.post(posted));
    constexpr GuestAddr kMsg = 0x5000;
    auto peek = make_message_loop_exports(*mlm_);
    ExportEntry peek_w{};
    for (auto& e : peek.entries) if (e.name == "PeekMessageW") peek_w = e;
    (void)memory_.write_u64(cpu_.gpr.rsp + 0x28, PM::REMOVE);
    EXPECT_EQ(call(peek_w.handler, kMsg, 0, kWmUser + 20, kWmUser + 20), 1u);
    EXPECT_EQ(order, (std::vector<char>{'B', 'A'}));
    EXPECT_EQ(next_ret, 0xAAu);
    EXPECT_EQ(memory_.read_u64(kMsg + 16).value_or(0), 0xAu);
    // Outside any hook CallNextHookEx returns 0 (callnext_null_hook).
    EXPECT_EQ(call([&](Marshaler& m) { ext_->impl_CallNextHookEx(m); }, 0, 0, 0, 0), 0u);
}

TEST_F(MessageWaitTest, CallWndProcHooksWrapSameThreadSendWithWParamZero) {
    // callwndproc n=2 order=cr lresult=0: WH_CALLWNDPROC then
    // WH_CALLWNDPROCRET, both with wParam 0 for the thread's own send, and
    // none for DispatchMessage (callwndproc_dispatch n=0).
    ThreadManager::set_current_thread_id(7);
    const HWND w = make_window(7);
    ext_->set_is_64bit(true);
    msm_->set_wndproc_callback([](HWND, u32, u64, u64, u64) -> u64 { return 0x33; });
    msm_->set_call_wndproc_hook([&](HWND h, u32 msg, u64 wp, u64 lp, bool cross,
                                    bool after, u64 lr) {
        ext_->dispatch_call_wndproc_hooks(h, msg, wp, lp, cross, after, lr);
    });
    constexpr GuestAddr kCwp = 0x40C000;
    constexpr GuestAddr kCwpRet = 0x40D000;
    std::vector<std::pair<char, u64>> calls;
    u64 lresult = 0;
    ext_->set_guest_call([&](GuestAddr proc, const u64* args, usize, bool) -> u64 {
        if (proc == kCwp) {
            calls.emplace_back('c', args[1]);
            EXPECT_EQ(memory_.read_u32(args[2] + 16).value_or(0), kWmUser + 21);
        }
        if (proc == kCwpRet) {
            calls.emplace_back('r', args[1]);
            lresult = memory_.read_u64(args[2]).value_or(0);
        }
        return 0;
    });
    ASSERT_NE(call([&](Marshaler& m) { ext_->impl_SetWindowsHookExW(m); }, 4, kCwp, 0, 7), 0u);
    ASSERT_NE(call([&](Marshaler& m) { ext_->impl_SetWindowsHookExW(m); }, 12, kCwpRet, 0, 7), 0u);
    EXPECT_EQ(msm_->send_message(w, kWmUser + 21, 0, 0), 0x33u);
    ASSERT_EQ(calls.size(), 2u);
    EXPECT_EQ(calls[0], std::make_pair('c', u64{0}));
    EXPECT_EQ(calls[1], std::make_pair('r', u64{0}));
    EXPECT_EQ(lresult, 0x33u);
}

TEST_F(MessageWaitTest, NotifyWinEventRunsOutOfContextHookAtNextRetrieval) {
    // winevent_after_notify=0, winevent_after_peek=1, outside range not
    // delivered; validation errors winevent_*.
    ThreadManager::set_current_thread_id(7);
    const HWND w = make_window(7);
    int delivered = 0;
    ext_->set_guest_call([&](GuestAddr, const u64* args, usize count, bool) -> u64 {
        if (count == 7 && args[1] == 0x8005) ++delivered;
        return 0;
    });
    auto set_hook = ext_export("SetWinEventHook");
    const auto hook = [&](u64 min, u64 max, u64 hmod, u64 proc, u32 pid, u32 tid, u32 flags) {
        (void)memory_.write_u64(cpu_.gpr.rsp + 0x28, pid);
        (void)memory_.write_u64(cpu_.gpr.rsp + 0x30, tid);
        (void)memory_.write_u64(cpu_.gpr.rsp + 0x38, flags);
        set_last_error(0xDEAD);
        return call(set_hook.handler, min, max, hmod, proc);
    };
    EXPECT_EQ(hook(0x8005, 0x8004, 0, 0x401000, 0, 0, 0), 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_HOOK_FILTER);
    EXPECT_EQ(hook(0x8005, 0x8005, 0, 0, 0, 0, 0), 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_FILTER_PROC);
    EXPECT_EQ(hook(0x8005, 0x8005, 0, 0x401000, 0, 0, 4 /* INCONTEXT */), 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_HOOK_NEEDS_HMOD);
    EXPECT_EQ(hook(0x8005, 0x8005, 0, 0x401000, 0, 0, 0x100), 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_FLAGS);
    ASSERT_NE(hook(0x8005, 0x8005, 0, 0x401000, 0, 0, 0), 0u);
    auto notify = ext_export("NotifyWinEvent");
    (void)call(notify.handler, 0x8005, w, static_cast<u32>(-4), 0);
    EXPECT_EQ(delivered, 0);
    MSG out{};
    (void)mlm_->peek_message(out, 0, 0, 0, PM::NOREMOVE);
    EXPECT_EQ(delivered, 1);
    (void)call(notify.handler, 0x8006, w, static_cast<u32>(-4), 0);
    (void)mlm_->peek_message(out, 0, 0, 0, PM::NOREMOVE);
    EXPECT_EQ(delivered, 1);
    // Delivery queues no message of its own.
    EXPECT_FALSE(mlm_->peek_message(out, 0, 0, 0, PM::NOREMOVE));
}

} // namespace
