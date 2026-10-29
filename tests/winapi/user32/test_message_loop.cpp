/// @file test_message_loop.cpp
/// @brief Tests for MessageLoopManager (WIN32-053)

#include <gtest/gtest.h>
#include <vitreo/common.hpp>
#include <winapi/user32/messages/message_loop.hpp>
#include <winapi/user32/user32_internal.hpp>
#include <winapi/ui/comctl32/comctl32.hpp>
#include <winapi/core/marshaler.hpp>
#include <winapi/user32/painting/painting.hpp>
#include <winapi/kernel32/synchronization/sync.hpp>
#include <winapi/kernel32/thread/thread.hpp>
#include <winapi/kernel32/error.hpp>
#include <core/sync/ntsync.hpp>

#include <atomic>
#include <chrono>
#include <thread>
#include <utility>
#include <vector>

using namespace vitreo;
using namespace vitreo::winapi;
using namespace vitreo::windowing;

class MessageLoopManagerTest : public ::testing::Test {
protected:
    void SetUp() override {
        WindowData data;
        data.title = L"Test";
        data.wndproc = 0x401000; // fake guest wndproc
        data.parent = HWND_DESKTOP;
        // Visible: a hidden window keeps no update region and reports no
        // QS_PAINT (s3gdi_userpaint_probe), and the paint tests invalidate it.
        data.visible = true;
        test_hwnd = hwnd_mgr.create(data);
    }

    HwndManager    hwnd_mgr;
    MessageQueue   queue;
    CocoaBridge    bridge;
    HWND           test_hwnd = 0;

    MessageLoopManager make_mlm() {
        return MessageLoopManager(hwnd_mgr, queue, bridge);
    }
};

// ---------------------------------------------------------------------------
// GetMessageW
// ---------------------------------------------------------------------------

TEST_F(MessageLoopManagerTest, GetMessageRetrievesPostedMessage) {
    auto mlm = make_mlm();

    MSG msg_in{};
    msg_in.hwnd    = test_hwnd;
    msg_in.message = WM::PAINT;
    msg_in.wParam  = 0;
    msg_in.lParam  = 0;
    queue.post(msg_in);

    MSG msg_out{};
    bool result = mlm.get_message(msg_out);
    EXPECT_TRUE(result);
    EXPECT_EQ(msg_out.hwnd, test_hwnd);
    EXPECT_EQ(msg_out.message, WM::PAINT);
}

TEST_F(MessageLoopManagerTest, PostedMessageInvokesWndprocOnlyAfterDispatchMessage) {
    auto mlm = make_mlm();
    u32 dispatch_count = 0;
    mlm.set_wndproc_callback(
        [&](HWND, u32, u64, u64, u64) -> u64 {
            ++dispatch_count;
            return 0;
        });

    MSG posted{};
    posted.hwnd = test_hwnd;
    posted.message = WM::LBUTTONDOWN;
    ASSERT_TRUE(queue.post(posted));
    EXPECT_EQ(dispatch_count, 0u);

    MSG retrieved{};
    ASSERT_TRUE(mlm.get_message(retrieved));
    EXPECT_EQ(retrieved.message, WM::LBUTTONDOWN);
    EXPECT_EQ(dispatch_count, 0u);

    (void)mlm.dispatch_message(retrieved);
    EXPECT_EQ(dispatch_count, 1u);
}

TEST_F(MessageLoopManagerTest, HostInputPumpSkipsGuestFrameHitTest) {
    // Present/Wait host pumps must enqueue clicks without SendMessage(WM_NCHITTEST).
    bool hit_test_called = false;
    bridge.set_native_hit_test_callback(
        [&](HWND, i32, i32) -> i32 {
            hit_test_called = true;
            return 2; // HTCAPTION
        });
    bridge.begin_host_input_pump();
    EXPECT_TRUE(bridge.in_host_input_pump());
    EXPECT_FALSE(bridge.perform_guest_frame_mouse_down(nullptr, test_hwnd, 10, 10));
    EXPECT_FALSE(hit_test_called);
    bridge.end_host_input_pump();
    EXPECT_FALSE(bridge.in_host_input_pump());
}

// A key window USER32 never activated synchronously learns it is active at
// the first GetMessage. WM_ACTIVATEAPP and WM_SETFOCUS are sent messages
// (Learn), so the window procedure receives them inside GetMessage, which
// never returns them (tools/oracle_probe/s2msg_ansi_msg_probe retrieves only
// the posted WM_CHAR).
TEST_F(MessageLoopManagerTest, GetMessageSendsInitialActivationToKeyWindowWithoutReturningIt) {
    auto mlm = make_mlm();
    std::vector<std::pair<u32, u64>> delivered;
    mlm.set_wndproc_callback([&](HWND hwnd, u32 message, u64 wparam, u64, u64) -> u64 {
        EXPECT_EQ(hwnd, test_hwnd);
        delivered.emplace_back(message, wparam);
        return 0;
    });
    bridge.set_active_hwnd(test_hwnd);
    MSG posted{};
    posted.hwnd = test_hwnd;
    posted.message = WM::CHAR_MSG;
    posted.wParam = 'a';
    ASSERT_TRUE(queue.post(posted));

    MSG msg{};
    ASSERT_TRUE(mlm.get_message(msg));
    EXPECT_EQ(msg.message, WM::CHAR_MSG);
    EXPECT_EQ(delivered, (std::vector<std::pair<u32, u64>>{
                             {WM::ACTIVATEAPP, 1}, {WM::SETFOCUS, 0}}));

    // Once only.
    ASSERT_TRUE(queue.post(posted));
    ASSERT_TRUE(mlm.get_message(msg));
    EXPECT_EQ(delivered.size(), 2u);
}

// A window that was never made key -- here a hidden window passed as the
// GetMessage filter -- receives no activation at all.
TEST_F(MessageLoopManagerTest, GetMessageFilterWindowIsNotActivated) {
    auto mlm = make_mlm();
    u32 delivered = 0;
    mlm.set_wndproc_callback([&](HWND, u32, u64, u64, u64) -> u64 {
        ++delivered;
        return 0;
    });
    MSG posted{};
    posted.hwnd = test_hwnd;
    posted.message = WM::CHAR_MSG;
    ASSERT_TRUE(queue.post(posted));
    MSG msg{};
    ASSERT_TRUE(mlm.get_message(msg, test_hwnd));
    EXPECT_EQ(msg.message, WM::CHAR_MSG);
    EXPECT_EQ(delivered, 0u);
}

TEST_F(MessageLoopManagerTest, ConstructionBindsQueueForEarlyNativeActivation) {
    auto mlm = make_mlm();

    bridge.on_native_window_became_key(test_hwnd);

    MSG msg{};
    ASSERT_TRUE(queue.peek(msg, 0, 0, 0, PM::REMOVE));
    EXPECT_EQ(msg.hwnd, test_hwnd);
    EXPECT_EQ(msg.message, WM::ACTIVATEAPP);
    EXPECT_EQ(msg.wParam, 1u);
}

TEST_F(MessageLoopManagerTest, GetMessageReturnsFalseForWmQuit) {
    auto mlm = make_mlm();

    queue.post_quit(0);

    MSG msg{};
    bool result = mlm.get_message(msg);
    EXPECT_FALSE(result);
}

TEST_F(MessageLoopManagerTest, GetMessageReturnsQuitExitCodeInWParam) {
    auto mlm = make_mlm();

    queue.post_quit(42);

    MSG msg{};
    mlm.get_message(msg);
    // After WM_QUIT, the exit code is in wParam
    EXPECT_EQ(msg.wParam, static_cast<u64>(42));
}

// ---------------------------------------------------------------------------
// PeekMessageW
// ---------------------------------------------------------------------------

TEST_F(MessageLoopManagerTest, PeekNoRemoveLeavesMessage) {
    auto mlm = make_mlm();

    MSG msg_in{};
    msg_in.hwnd    = test_hwnd;
    msg_in.message = WM::CLOSE;
    queue.post(msg_in);

    MSG msg_out{};
    bool found = mlm.peek_message(msg_out, 0, 0, 0, PM::NOREMOVE);
    EXPECT_TRUE(found);
    EXPECT_EQ(msg_out.message, WM::CLOSE);

    // Message should still be in the queue
    EXPECT_EQ(queue.size(), 1u);
}

TEST_F(MessageLoopManagerTest, PeekRemoveRemovesMessage) {
    auto mlm = make_mlm();

    MSG msg_in{};
    msg_in.hwnd    = test_hwnd;
    msg_in.message = WM::CLOSE;
    queue.post(msg_in);

    MSG msg_out{};
    bool found = mlm.peek_message(msg_out, 0, 0, 0, PM::REMOVE);
    EXPECT_TRUE(found);
    EXPECT_EQ(msg_out.message, WM::CLOSE);

    // Message should be gone
    EXPECT_TRUE(queue.empty());
}

TEST_F(MessageLoopManagerTest, PeekReturnsFalseOnEmptyQueue) {
    auto mlm = make_mlm();

    MSG msg{};
    bool found = mlm.peek_message(msg);
    EXPECT_FALSE(found);
}

TEST_F(MessageLoopManagerTest, WorkerGuiThreadReceivesOnlyItsWindowMessages) {
    auto mlm = make_mlm();

    WindowData worker_data{};
    worker_data.title = L"Worker GUI";
    worker_data.wndproc = 0x402000;
    worker_data.parent = HWND_DESKTOP;
    worker_data.owner_thread_id = 5;
    const HWND worker_hwnd = hwnd_mgr.create(worker_data);

    MSG main_msg{};
    main_msg.hwnd = test_hwnd;
    main_msg.message = WM::CLOSE;
    ASSERT_TRUE(queue.post(main_msg));

    MSG worker_msg{};
    worker_msg.hwnd = worker_hwnd;
    worker_msg.message = WM::TIMER;
    worker_msg.wParam = 124;
    ASSERT_TRUE(queue.post(worker_msg));

    const u32 previous_tid = ThreadManager::get_current_thread_id_static();
    ThreadManager::set_current_thread_id(5);
    MSG received{};
    EXPECT_TRUE(mlm.get_message(received));
    EXPECT_EQ(received.hwnd, worker_hwnd);
    EXPECT_EQ(received.message, WM::TIMER);
    EXPECT_EQ(received.wParam, 124u);

    ThreadManager::set_current_thread_id(previous_tid);
    EXPECT_TRUE(mlm.peek_message(received, 0, 0, 0, PM::REMOVE));
    EXPECT_EQ(received.hwnd, test_hwnd);
    EXPECT_EQ(received.message, WM::CLOSE);
}

TEST_F(MessageLoopManagerTest, QuitMessagesAreScopedToCallingThread) {
    auto mlm = make_mlm();
    queue.post_quit(77, 5);

    MSG msg{};
    EXPECT_FALSE(mlm.peek_message(msg));

    const u32 previous_tid = ThreadManager::get_current_thread_id_static();
    ThreadManager::set_current_thread_id(5);
    EXPECT_FALSE(mlm.get_message(msg));
    EXPECT_EQ(msg.message, WM::QUIT);
    EXPECT_EQ(msg.wParam, 77u);
    ThreadManager::set_current_thread_id(previous_tid);
}

// ---------------------------------------------------------------------------
// DispatchMessageW
// ---------------------------------------------------------------------------

TEST_F(MessageLoopManagerTest, DispatchCallsWndprocCallbackWithCorrectArgs) {
    auto mlm = make_mlm();

    HWND   captured_hwnd    = 0;
    u32    captured_msg     = 0;
    u64    captured_wparam  = 0;
    u64    captured_lparam  = 0;
    u64    captured_proc    = 0;

    mlm.set_wndproc_callback(
        [&](HWND h, u32 m, u64 wp, u64 lp, u64 proc) -> u64 {
            captured_hwnd   = h;
            captured_msg    = m;
            captured_wparam = wp;
            captured_lparam = lp;
            captured_proc   = proc;
            return 0;
        }
    );

    MSG msg{};
    msg.hwnd    = test_hwnd;
    msg.message = WM::PAINT;
    msg.wParam  = 100;
    msg.lParam  = 200;

    mlm.dispatch_message(msg);

    EXPECT_EQ(captured_hwnd, test_hwnd);
    EXPECT_EQ(captured_msg, WM::PAINT);
    EXPECT_EQ(captured_wparam, 100u);
    EXPECT_EQ(captured_lparam, 200u);
    EXPECT_EQ(captured_proc, 0x401000u);
}

TEST_F(MessageLoopManagerTest, DispatchReturnsCallbackResult) {
    auto mlm = make_mlm();

    mlm.set_wndproc_callback(
        [](HWND, u32, u64, u64, u64) -> u64 { return 0xDEAD; }
    );

    MSG msg{};
    msg.hwnd    = test_hwnd;
    msg.message = WM::CLOSE;

    u64 result = mlm.dispatch_message(msg);
    EXPECT_EQ(result, 0xDEADu);
    EXPECT_EQ(mlm.last_dispatch_result(), 0xDEADu);
}

TEST_F(MessageLoopManagerTest, DispatchWithoutCallbackReturnsZero) {
    auto mlm = make_mlm();

    MSG msg{};
    msg.hwnd    = test_hwnd;
    msg.message = WM::PAINT;

    u64 result = mlm.dispatch_message(msg);
    EXPECT_EQ(result, 0u);
}

TEST_F(MessageLoopManagerTest, DispatchLooksUpCorrectWndproc) {
    auto mlm = make_mlm();

    mlm.set_wndproc_callback(
        [](HWND, u32, u64, u64, u64) -> u64 { return 1; }
    );

    MSG msg{};
    msg.hwnd    = test_hwnd;
    msg.message = WM::PAINT;

    mlm.dispatch_message(msg);
    EXPECT_EQ(mlm.last_dispatch_wndproc(), 0x401000u);
}

TEST_F(MessageLoopManagerTest, DispatchForInvalidHwndReturnsZero) {
    auto mlm = make_mlm();

    mlm.set_wndproc_callback(
        [](HWND, u32, u64, u64, u64) -> u64 { return 99; }
    );

    MSG msg{};
    msg.hwnd    = 0xBAD;  // invalid HWND
    msg.message = WM::PAINT;

    u64 result = mlm.dispatch_message(msg);
    EXPECT_EQ(result, 0u);
    EXPECT_EQ(mlm.last_dispatch_wndproc(), 0u);
}

// ---------------------------------------------------------------------------
// TranslateMessage
// ---------------------------------------------------------------------------

// Learn: TranslateMessage returns nonzero for any WM_KEYDOWN/WM_KEYUP/
// WM_SYSKEYDOWN/WM_SYSKEYUP "regardless of the translation", zero for other
// messages (tools/oracle_probe/s2msg_input_probe.expected
// translate_keydown_f1 ret=1 posted nothing; translate_char_msg ret=0).
TEST_F(MessageLoopManagerTest, TranslateMessageReturnsTrueForNonCharacterKey) {
    auto mlm = make_mlm();

    MSG msg{};
    msg.hwnd    = test_hwnd;
    msg.message = WM::KEYDOWN;
    msg.wParam  = 0x70; // VK_F1 — non-character key

    EXPECT_TRUE(mlm.translate_message(msg));
    MSG posted{};
    EXPECT_FALSE(queue.peek(posted, 0, 0, 0, PM::REMOVE));
    msg.message = WM::KEYUP;
    EXPECT_TRUE(mlm.translate_message(msg));
    msg.message = WM::CHAR_MSG;
    EXPECT_FALSE(mlm.translate_message(msg));
}

TEST_F(MessageLoopManagerTest, NativeLetterKeyTranslatesToWmChar) {
    auto mlm = make_mlm();

    MSG msg{};
    msg.hwnd = test_hwnd;
    msg.message = WM::KEYDOWN;
    msg.wParam = CocoaBridge::map_native_keycode_to_windows_vk(0x00);

    ASSERT_TRUE(mlm.translate_message(msg));
    MSG translated{};
    ASSERT_TRUE(queue.peek(translated, 0, 0, 0, PM::REMOVE));
    EXPECT_EQ(translated.hwnd, test_hwnd);
    EXPECT_EQ(translated.message, WM::CHAR_MSG);
    EXPECT_EQ(translated.wParam, static_cast<u64>('a'));
}

TEST_F(MessageLoopManagerTest, NativeKeyboardMappingUsesWindowsVirtualKeys) {
    EXPECT_EQ(CocoaBridge::map_native_keycode_to_windows_vk(0x00), 0x41u); // A
    EXPECT_EQ(CocoaBridge::map_native_keycode_to_windows_vk(0x12), 0x31u); // 1
    EXPECT_EQ(CocoaBridge::map_native_keycode_to_windows_vk(0x33), 0x08u); // Backspace
    EXPECT_EQ(CocoaBridge::map_native_keycode_to_windows_vk(0x7B), 0x25u); // Left
    EXPECT_EQ(CocoaBridge::map_native_keycode_to_windows_vk(0xFFFF), 0u);
}

// ---------------------------------------------------------------------------
// Export Registration
// ---------------------------------------------------------------------------

TEST_F(MessageLoopManagerTest, ExportsIncludeExtendedMessageWait) {
    auto mlm = make_mlm();
    auto exports = make_message_loop_exports(mlm);
    EXPECT_EQ(exports.entries.size(), 9u);
    EXPECT_EQ(exports.entries[0].name, "GetMessageW");
    EXPECT_EQ(exports.entries[1].name, "GetMessageA");
    EXPECT_EQ(exports.entries[2].name, "PeekMessageW");
    EXPECT_EQ(exports.entries[3].name, "PeekMessageA");
    EXPECT_EQ(exports.entries[4].name, "DispatchMessageW");
    EXPECT_EQ(exports.entries[5].name, "DispatchMessageA");
    EXPECT_EQ(exports.entries[6].name, "TranslateMessage");
    EXPECT_EQ(exports.entries[7].name, "MsgWaitForMultipleObjects");
    EXPECT_EQ(exports.entries[8].name, "MsgWaitForMultipleObjectsEx");
}

// ============================================================================
// TranslateMessage modifier handling
// ============================================================================
//
// The character a key produces depends on the modifier state as of the message
// being processed — GetKeyState's documented contract — not the live hardware
// state, which may already show the key released by the time a queued message
// is dispatched.

namespace {
constexpr u32 kVkShift   = 0x10;
constexpr u32 kVkControl = 0x11;

MSG key_msg(HWND hwnd, u32 message, u64 vk) {
    MSG m{};
    m.hwnd    = hwnd;
    m.message = message;
    m.wParam  = vk;
    return m;
}

/// Drain the queue and return the first WM_CHAR, if any.
bool take_char(MessageQueue& queue, MSG& out) {
    MSG m{};
    while (queue.peek(m, NULL_HWND, 0, 0, PM::REMOVE)) {
        if (m.message == WM::CHAR_MSG) { out = m; return true; }
    }
    return false;
}
} // namespace

TEST_F(MessageLoopManagerTest, TranslateMessageProducesLowercaseWithoutShift) {
    auto mlm = make_mlm();
    EXPECT_TRUE(mlm.translate_message(key_msg(test_hwnd, WM::KEYDOWN, 'A')));
    MSG ch{};
    ASSERT_TRUE(take_char(queue, ch));
    EXPECT_EQ(ch.wParam, static_cast<u64>('a'));
}

TEST_F(MessageLoopManagerTest, TranslateMessageProducesCapitalWhileShiftHeld) {
    auto mlm = make_mlm();
    // Shift goes down as its message is retrieved, before the letter arrives.
    mlm.note_retrieved_message(key_msg(test_hwnd, WM::KEYDOWN, kVkShift));
    EXPECT_TRUE(mlm.sync_modifier_down(kVkShift));

    EXPECT_TRUE(mlm.translate_message(key_msg(test_hwnd, WM::KEYDOWN, 'A')));
    MSG ch{};
    ASSERT_TRUE(take_char(queue, ch));
    EXPECT_EQ(ch.wParam, static_cast<u64>('A'));

    // Releasing shift returns to lowercase.
    mlm.note_retrieved_message(key_msg(test_hwnd, WM::KEYUP, kVkShift));
    EXPECT_FALSE(mlm.sync_modifier_down(kVkShift));
    EXPECT_TRUE(mlm.translate_message(key_msg(test_hwnd, WM::KEYDOWN, 'A')));
    ASSERT_TRUE(take_char(queue, ch));
    EXPECT_EQ(ch.wParam, static_cast<u64>('a'));
}

TEST_F(MessageLoopManagerTest, TranslateMessageShiftsDigitRow) {
    auto mlm = make_mlm();
    mlm.note_retrieved_message(key_msg(test_hwnd, WM::KEYDOWN, kVkShift));
    EXPECT_TRUE(mlm.translate_message(key_msg(test_hwnd, WM::KEYDOWN, '1')));
    MSG ch{};
    ASSERT_TRUE(take_char(queue, ch));
    EXPECT_EQ(ch.wParam, static_cast<u64>('!'));
}

TEST_F(MessageLoopManagerTest, TranslateMessageSuppressesCharForCtrlDigit) {
    auto mlm = make_mlm();
    mlm.note_retrieved_message(key_msg(test_hwnd, WM::KEYDOWN, kVkControl));
    // Windows produces no character for Ctrl+digit. Emitting one makes the
    // chord look like a plain digit to the application. The call still
    // returns nonzero (s2msg_input_probe translate_ctrl_2 ret=1).
    EXPECT_TRUE(mlm.translate_message(key_msg(test_hwnd, WM::KEYDOWN, '1')));
    MSG ch{};
    EXPECT_FALSE(take_char(queue, ch));
}

TEST_F(MessageLoopManagerTest, TranslateMessageProducesControlCharForCtrlLetter) {
    auto mlm = make_mlm();
    mlm.note_retrieved_message(key_msg(test_hwnd, WM::KEYDOWN, kVkControl));
    EXPECT_TRUE(mlm.translate_message(key_msg(test_hwnd, WM::KEYDOWN, 'A')));
    MSG ch{};
    ASSERT_TRUE(take_char(queue, ch));
    EXPECT_EQ(ch.wParam, 0x01u);  // Ctrl+A
}

// ============================================================================
// TranslateMessage control characters
// ============================================================================
//
// Windows produces a WM_CHAR carrying the ASCII control code for Escape,
// Backspace, Tab and Return. Escape was previously absent, so applications
// that cancel on WM_CHAR 0x1B — StarCraft's in-game menu among them — saw the
// key as dead.

TEST_F(MessageLoopManagerTest, TranslateMessageProducesCharForEscape) {
    auto mlm = make_mlm();
    EXPECT_TRUE(mlm.translate_message(key_msg(test_hwnd, WM::KEYDOWN, 0x1B)));
    MSG ch{};
    ASSERT_TRUE(take_char(queue, ch));
    EXPECT_EQ(ch.wParam, 0x1Bu);
}

TEST_F(MessageLoopManagerTest, TranslateMessageEscapeIsUnaffectedByShift) {
    auto mlm = make_mlm();
    mlm.note_retrieved_message(key_msg(test_hwnd, WM::KEYDOWN, kVkShift));
    EXPECT_TRUE(mlm.translate_message(key_msg(test_hwnd, WM::KEYDOWN, 0x1B)));
    MSG ch{};
    ASSERT_TRUE(take_char(queue, ch));
    EXPECT_EQ(ch.wParam, 0x1Bu);
}

TEST_F(MessageLoopManagerTest, TranslateMessageProducesControlCharsForEditingKeys) {
    struct { u32 vk; u64 expected; } cases[] = {
        {0x08, 0x08},  // VK_BACK
        {0x09, 0x09},  // VK_TAB
        {0x0D, 0x0D},  // VK_RETURN
        {0x1B, 0x1B},  // VK_ESCAPE
    };
    for (const auto& c : cases) {
        auto mlm = make_mlm();
        EXPECT_TRUE(mlm.translate_message(key_msg(test_hwnd, WM::KEYDOWN, c.vk)))
            << "vk=0x" << std::hex << c.vk;
        MSG ch{};
        ASSERT_TRUE(take_char(queue, ch)) << "vk=0x" << std::hex << c.vk;
        EXPECT_EQ(ch.wParam, c.expected) << "vk=0x" << std::hex << c.vk;
    }
}

// ---------------------------------------------------------------------------
// MsgWaitForMultipleObjects on worker threads
// ---------------------------------------------------------------------------
//
// Workers own per-thread queues (HWND owner_thread_id / PostThreadMessage).
// MsgWait must still consult object handles (StarCraft Modem,
// .docs/starcraft-bringup.md §33) *and* wake when a matching message is posted
// (Chromium MessagePumpForUI on a non-tid=1 UI thread).

class WorkerMsgWaitTest : public MessageLoopManagerTest {
protected:
    void SetUp() override {
        MessageLoopManagerTest::SetUp();
        prev_tid_ = ThreadManager::get_current_thread_id_static();
        ThreadManager::set_current_thread_id(5);  // pose as a worker thread
    }
    void TearDown() override {
        ThreadManager::set_current_thread_id(prev_tid_);
    }
    u32          prev_tid_ = 1;
    HandleTable  handles_;
    SyncManager  sync_{handles_};
};

TEST_F(WorkerMsgWaitTest, SignaledHandleSatisfiesWaitInsteadOfTimingOut) {
    auto mlm = make_mlm();
    mlm.set_sync_manager(&sync_);

    const Handle ev = sync_.create_event(/*manual_reset=*/true,
                                         /*initial_state=*/true, "");
    ASSERT_NE(ev, 0u);

    const u32 h = static_cast<u32>(ev);
    const u32 result = mlm.msg_wait_for_multiple_objects(
        &h, 1, /*wait_all=*/false, 0xFFFFFFFF, 0xFF);

    EXPECT_EQ(result, 0u /* WAIT_OBJECT_0 */);
}

TEST_F(WorkerMsgWaitTest, UnsignaledHandleStillHonorsZeroTimeout) {
    auto mlm = make_mlm();
    mlm.set_sync_manager(&sync_);

    const Handle ev = sync_.create_event(true, /*initial_state=*/false, "");
    ASSERT_NE(ev, 0u);

    const u32 h = static_cast<u32>(ev);
    const u32 result = mlm.msg_wait_for_multiple_objects(
        &h, 1, false, /*timeout_ms=*/0, 0xFF);

    EXPECT_EQ(result, 0x102u /* WAIT_TIMEOUT */);
}

// MWMO_ALERTABLE (Learn, nf-winuser-msgwaitformultipleobjectsex): a pending
// QueueUserAPC ends the wait with WAIT_IO_COMPLETION after the APC runs
// (user32_windows_probe mwmo_alertable_apc).
TEST_F(WorkerMsgWaitTest, AlertableReturnsIoCompletionWhenApcPending) {
    auto mlm = make_mlm();
    mlm.set_sync_manager(&sync_);
    std::atomic<bool> delivered{false};
    sync_.set_alertable_apc_drain([&] {
        if (delivered.exchange(true)) return false;
        return true;
    });
    EXPECT_EQ(mlm.msg_wait_for_multiple_objects(
                  nullptr, 0, false, 1000, 0xFF, /*alertable=*/true),
              WAIT_IO_COMPLETION);
    EXPECT_TRUE(delivered.load());
}

TEST_F(WorkerMsgWaitTest, AlertableWaitWakesWhenApcQueued) {
    auto mlm = make_mlm();
    mlm.set_sync_manager(&sync_);
    std::atomic<bool> has_apc{false};
    sync_.set_alertable_apc_drain([&] { return has_apc.exchange(false); });
    std::thread apc([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        has_apc.store(true);
        sync_.note_apc_queued(5);  // WorkerMsgWaitTest poses as tid 5
    });
    const auto t0 = std::chrono::steady_clock::now();
    EXPECT_EQ(mlm.msg_wait_for_multiple_objects(
                  nullptr, 0, false, 2000, 0xFF, /*alertable=*/true),
              WAIT_IO_COMPLETION);
    apc.join();
    EXPECT_LT(std::chrono::steady_clock::now() - t0, std::chrono::seconds(1));
}

TEST_F(WorkerMsgWaitTest, NonAlertableIgnoresPendingApc) {
    auto mlm = make_mlm();
    mlm.set_sync_manager(&sync_);
    sync_.set_alertable_apc_drain([] { return true; });
    EXPECT_EQ(mlm.msg_wait_for_multiple_objects(
                  nullptr, 0, false, /*timeout_ms=*/20, 0xFF, /*alertable=*/false),
              WAIT_TIMEOUT);
}

// WaitForInputIdle (Learn; user32_windows_probe wfii_*): invalid → WAIT_FAILED
// + ERROR_INVALID_HANDLE; GetCurrentProcess → WAIT_FAILED leaving last-error.
TEST_F(WorkerMsgWaitTest, WaitForInputIdleRejectsInvalidAndCurrentProcess) {
    auto mlm = make_mlm();
    mlm.set_sync_manager(&sync_);
    set_last_error(0xDEAD);
    EXPECT_EQ(mlm.wait_for_input_idle(0x7FFFFFF0ull, 0), WAIT_FAILED);
    EXPECT_EQ(get_last_error(), 6u);  // ERROR_INVALID_HANDLE
    set_last_error(0xDEAD);
    EXPECT_EQ(mlm.wait_for_input_idle(0xFFFFFFFFFFFFFFFFull, 0), WAIT_FAILED);
    EXPECT_EQ(get_last_error(), 0xDEADu);
}

TEST_F(WorkerMsgWaitTest, DelayedSetEventWakesBlockingWait) {
    auto mlm = make_mlm();
    mlm.set_sync_manager(&sync_);

    const Handle ev = sync_.create_event(true, /*initial_state=*/false, "");
    ASSERT_NE(ev, 0u);
    const u32 h = static_cast<u32>(ev);

    std::thread signaler([&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        ASSERT_TRUE(sync_.set_event(ev));
    });

    const u32 result = mlm.msg_wait_for_multiple_objects(
        &h, 1, /*wait_all=*/false, /*timeout_ms=*/2000, 0xFF);
    signaler.join();

    EXPECT_EQ(result, 0u /* WAIT_OBJECT_0 */);
}

TEST_F(WorkerMsgWaitTest, PostedMessageWakesHandleWaitWithoutSliceDelay) {
    // Chromium's UI thread (not tid 1) waits in MsgWaitForMultipleObjectsEx
    // with one event handle and QS_ALLINPUT. The wait parks on ntsync, which
    // a PostMessage cannot see, so before the per-thread message event it
    // was only discovered at the next 16 ms re-check. Every wake must now be
    // event-driven: the post has to end the wait within a few milliseconds.
    auto mlm = make_mlm();
    mlm.set_sync_manager(&sync_);

    const Handle ev = sync_.create_event(true, /*initial_state=*/false, "");
    ASSERT_NE(ev, 0u);
    const u32 h = static_cast<u32>(ev);

    // The waiter runs on a worker pthread: on the process main thread
    // MsgWait takes the AppKit run-loop path, whose wake is wired by the
    // runtime, not by this fixture.
    std::chrono::steady_clock::time_point posted_at{};
    std::chrono::steady_clock::time_point woke_at{};
    u32 result = 0xFFFFFFFFu;
    std::thread waiter([&]() {
        ThreadManager::set_current_thread_id(5);
        result = mlm.msg_wait_for_multiple_objects(
            &h, 1, /*wait_all=*/false, /*timeout_ms=*/2000, 0xFF);
        woke_at = std::chrono::steady_clock::now();
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    windowing::MSG work{};
    work.hwnd = NULL_HWND;
    work.message = 0x0401; // kMsgHaveWork
    work.target_thread_id = 5;
    posted_at = std::chrono::steady_clock::now();
    ASSERT_TRUE(queue.post(work));
    waiter.join();

    EXPECT_EQ(result, 1u /* WAIT_OBJECT_0 + nCount: message ready */);
    const auto after_post =
        std::chrono::duration_cast<std::chrono::milliseconds>(woke_at - posted_at);
    // A slice-based re-check would show up here as ~16 ms; the cap on a
    // missed wake is 100 ms. An event-driven wake is well under 8 ms even
    // on a loaded machine.
    EXPECT_LT(after_post.count(), 8) << "wake lagged the post by "
                                     << after_post.count() << " ms";
}

TEST_F(WorkerMsgWaitTest, PostedThreadMessageWakesMsgWait) {
    // Chromium MessagePumpForUI WaitForWork: nCount can be 0 with QS_ALLINPUT.
    auto mlm = make_mlm();
    mlm.set_sync_manager(&sync_);

    windowing::MSG work{};
    work.hwnd = NULL_HWND;
    work.message = 0x0401; // kMsgHaveWork
    work.target_thread_id = 5;
    ASSERT_TRUE(queue.post(work));

    const u32 result = mlm.msg_wait_for_multiple_objects(
        nullptr, 0, /*wait_all=*/false, /*timeout_ms=*/0, 0xFF);
    EXPECT_EQ(result, 0u /* WAIT_OBJECT_0 — message ready */);
}

// Learn MsgWaitForMultipleObjectsEx without MWMO_INPUTAVAILABLE: Peek/Get
// that inspects the queue marks existing input as seen, so a later wait
// times out until new input arrives (user32_windows_probe mwmo_seen_input_*).
TEST_F(WorkerMsgWaitTest, SeenInputIgnoredUnlessInputAvailable) {
    auto mlm = make_mlm();
    mlm.set_sync_manager(&sync_);

    windowing::MSG work{};
    work.hwnd = NULL_HWND;
    work.message = 0x0401;
    work.target_thread_id = 5;
    ASSERT_TRUE(queue.post(work));

    windowing::MSG peeked{};
    EXPECT_FALSE(queue.peek_for_thread(
        peeked, 5, hwnd_mgr, 0, 0x500, 0x500, 0 /* PM_NOREMOVE */));

    EXPECT_EQ(mlm.msg_wait_for_multiple_objects(
                  nullptr, 0, false, 30, QS::ALLINPUT,
                  /*alertable=*/false, /*input_available=*/false),
              0x102u /* WAIT_TIMEOUT */);
    EXPECT_EQ(mlm.msg_wait_for_multiple_objects(
                  nullptr, 0, false, 30, QS::ALLINPUT,
                  /*alertable=*/false, /*input_available=*/true),
              0u /* WAIT_OBJECT_0 — message still queued */);
}

// MWMO_WAITALL must surface WAIT_ABANDONED_0 after acquiring an abandoned
// mutex (08c M7); swallowing it leaves ownership taken and the wait stuck.
TEST_F(WorkerMsgWaitTest, WaitAllReportsAbandonedMutex) {
    auto mlm = make_mlm();
    mlm.set_sync_manager(&sync_);

    const Handle mtx = sync_.create_mutex(/*initial_owner=*/true, "");
    const Handle ev = sync_.create_event(true, /*initial_state=*/true, "");
    ASSERT_NE(mtx, 0u);
    ASSERT_NE(ev, 0u);
    ntsync::global().abandon_for_thread(5);

    windowing::MSG work{};
    work.hwnd = NULL_HWND;
    work.message = 0x0401;
    work.target_thread_id = 5;
    ASSERT_TRUE(queue.post(work));

    const u32 handles[2] = {static_cast<u32>(mtx), static_cast<u32>(ev)};
    const u32 result = mlm.msg_wait_for_multiple_objects(
        handles, 2, /*wait_all=*/true, /*timeout_ms=*/0, QS::ALLINPUT,
        /*alertable=*/false, /*input_available=*/true);
    EXPECT_EQ(result, 0x80u /* WAIT_ABANDONED_0 — mutex at index 0 */);
}

TEST_F(WorkerMsgWaitTest, AnotherThreadsMessageDoesNotWakeMsgWait) {
    auto mlm = make_mlm();
    mlm.set_sync_manager(&sync_);

    windowing::MSG other_work{};
    other_work.hwnd = NULL_HWND;
    other_work.message = 0x0401;
    other_work.target_thread_id = 1;
    ASSERT_TRUE(queue.post(other_work));

    const u32 result = mlm.msg_wait_for_multiple_objects(
        nullptr, 0, /*wait_all=*/false, /*timeout_ms=*/10, QS::ALLINPUT);
    EXPECT_EQ(result, 0x102u /* WAIT_TIMEOUT */);
}

TEST_F(WorkerMsgWaitTest, PostedMessageWakesAlongsideUnsignaledHandle) {
    auto mlm = make_mlm();
    mlm.set_sync_manager(&sync_);

    const Handle ev = sync_.create_event(true, /*initial_state=*/false, "");
    ASSERT_NE(ev, 0u);
    const u32 h = static_cast<u32>(ev);

    windowing::MSG work{};
    work.hwnd = NULL_HWND;
    work.message = 0x0401;
    work.target_thread_id = 5;
    ASSERT_TRUE(queue.post(work));

    const u32 result = mlm.msg_wait_for_multiple_objects(
        &h, 1, /*wait_all=*/false, /*timeout_ms=*/0, 0xFF);
    EXPECT_EQ(result, 1u /* WAIT_OBJECT_0 + nCount */);
}

TEST_F(MessageLoopManagerTest, MainThreadMsgWaitWakesOnSetEventWithoutMessage) {
    // tid=1 path must park on ntsync (not message-CV alone) so Mojo trap
    // SetEvent wakes MsgWait even when no WM_* is posted.
    auto mlm = make_mlm();
    HandleTable handles;
    SyncManager sync{handles};
    mlm.set_sync_manager(&sync);
    ThreadManager::set_current_thread_id(1);

    const Handle ev = sync.create_event(true, /*initial_state=*/false, "");
    ASSERT_NE(ev, 0u);
    const u32 h = static_cast<u32>(ev);

    std::atomic<bool> started{false};
    std::atomic<u32> result{0x102u};
    std::thread waiter([&]() {
        ThreadManager::set_current_thread_id(1);
        started.store(true, std::memory_order_release);
        result.store(
            mlm.msg_wait_for_multiple_objects(
                &h, 1, /*wait_all=*/false, /*timeout_ms=*/2000, 0xFF),
            std::memory_order_release);
    });
    while (!started.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    ASSERT_TRUE(sync.set_event(ev));
    waiter.join();

    EXPECT_EQ(result.load(std::memory_order_acquire), 0u /* WAIT_OBJECT_0 */);
}

// ---------------------------------------------------------------------------
// Synthesized-message filtering
// ---------------------------------------------------------------------------
//
// WM_PAINT and WM_TIMER are generated from a window's update region and from
// expired timers rather than dequeued, but Windows still applies the caller's
// hWnd and message-range filters to them. Reporting a synthesized message the
// filter excludes turns a filtered drain such as
// `while (PeekMessage(&m, hwnd, min, max, PM_REMOVE))` into an infinite spin,
// which is exactly what a Chromium message pump does while asking whether a
// character message is pending.

TEST_F(MessageLoopManagerTest, MsgWaitSeesPendingPaintWithoutValidatingIt) {
    PaintingManager paint_mgr{&hwnd_mgr};
    paint_mgr.invalidate_rect(test_hwnd, nullptr, false);
    auto mlm = make_mlm();
    mlm.set_painting_manager(&paint_mgr);

    EXPECT_EQ(mlm.msg_wait_for_multiple_objects(nullptr, 0, false, 0,
                                               QS::TIMER), 258u);
    EXPECT_EQ(mlm.msg_wait_for_multiple_objects(nullptr, 0, false, 0,
                                               QS::PAINT), 0u);
    EXPECT_TRUE(paint_mgr.needs_paint(test_hwnd));
    MSG msg{};
    ASSERT_TRUE(mlm.peek_message(msg, 0, WM::PAINT, WM::PAINT, PM::REMOVE));
    EXPECT_EQ(msg.hwnd, test_hwnd);
    EXPECT_EQ(msg.message, WM::PAINT);
    EXPECT_TRUE(paint_mgr.needs_paint(test_hwnd));
}

TEST_F(MessageLoopManagerTest, MsgWaitIgnoresAnotherThreadsPendingPaint) {
    hwnd_mgr.find(test_hwnd)->owner_thread_id =
        ThreadManager::get_current_thread_id_static() + 1;
    PaintingManager paint_mgr{&hwnd_mgr};
    paint_mgr.invalidate_rect(test_hwnd, nullptr, false);
    auto mlm = make_mlm();
    mlm.set_painting_manager(&paint_mgr);
    EXPECT_EQ(mlm.msg_wait_for_multiple_objects(nullptr, 0, false, 0,
                                               QS::PAINT), 258u);
    EXPECT_TRUE(paint_mgr.needs_paint(test_hwnd));
}

TEST_F(MessageLoopManagerTest, SynthesizedPaintHonoursTheMessageRangeFilter) {
    constexpr u32 WM_CHAR_FIRST = 0x0102;   // WM_CHAR
    constexpr u32 WM_CHAR_LAST  = 0x0103;   // WM_DEADCHAR

    PaintingManager paint_mgr{&hwnd_mgr};
    paint_mgr.invalidate_rect(static_cast<u64>(test_hwnd), nullptr, false);
    ASSERT_TRUE(paint_mgr.needs_paint(static_cast<u64>(test_hwnd)));

    auto mlm = make_mlm();
    mlm.set_painting_manager(&paint_mgr);

    // A range that excludes WM_PAINT (0x000F) must report nothing, however
    // dirty the window is.
    MSG msg{};
    EXPECT_FALSE(mlm.peek_message(msg, 0, WM_CHAR_FIRST, WM_CHAR_LAST,
                                  PM::REMOVE));

    // The window is still dirty, so an unfiltered peek still sees the paint.
    msg = {};
    EXPECT_TRUE(mlm.peek_message(msg, 0, 0, 0, PM::REMOVE));
    EXPECT_EQ(msg.message, WM::PAINT);
    EXPECT_EQ(msg.hwnd, test_hwnd);
}

TEST_F(MessageLoopManagerTest, SynthesizedPaintHonoursTheWindowFilter) {
    WindowData other{};
    other.title = L"Other";
    other.wndproc = 0x402000;
    other.parent = HWND_DESKTOP;
    other.visible = true;
    const HWND other_hwnd = hwnd_mgr.create(other);
    ASSERT_NE(other_hwnd, test_hwnd);

    PaintingManager paint_mgr{&hwnd_mgr};
    paint_mgr.invalidate_rect(static_cast<u64>(test_hwnd), nullptr, false);

    auto mlm = make_mlm();
    mlm.set_painting_manager(&paint_mgr);

    // Filtering on a different window must not report the dirty one's paint.
    MSG msg{};
    EXPECT_FALSE(mlm.peek_message(msg, other_hwnd, 0, 0, PM::REMOVE));

    // Filtering on the dirty window itself does report it.
    msg = {};
    EXPECT_TRUE(mlm.peek_message(msg, test_hwnd, 0, 0, PM::REMOVE));
    EXPECT_EQ(msg.message, WM::PAINT);
    EXPECT_EQ(msg.hwnd, test_hwnd);
}

TEST_F(MessageLoopManagerTest, RepeatedFilteredPeekTerminates) {
    // The storm this guards against: a dirty window plus a filtered drain.
    // On Windows the loop below ends immediately; before the filter was
    // honoured it ran forever.
    constexpr u32 WM_CHAR_FIRST = 0x0102;
    constexpr u32 WM_CHAR_LAST  = 0x0103;

    PaintingManager paint_mgr{&hwnd_mgr};
    paint_mgr.invalidate_rect(static_cast<u64>(test_hwnd), nullptr, true);

    auto mlm = make_mlm();
    mlm.set_painting_manager(&paint_mgr);

    MSG msg{};
    unsigned iterations = 0;
    while (mlm.peek_message(msg, test_hwnd, WM_CHAR_FIRST, WM_CHAR_LAST,
                            PM::REMOVE)) {
        if (++iterations > 16) break;   // bounded so a regression fails fast
    }
    EXPECT_EQ(iterations, 0u);
}

TEST_F(MessageLoopManagerTest, WheelPreservesEventScreenCoordinates) {
    auto mlm = make_mlm();
    hwnd_mgr.find(test_hwnd)->rect = {100, 200, 500, 500};
    for (u32 kind : {0x020au, 0x020eu}) {
        MSG input{};
        input.hwnd = test_hwnd; input.message = kind;
        input.lParam = (static_cast<u32>(static_cast<u16>(-40)) << 16) | static_cast<u16>(-20);
        queue.post_input(input);
        MSG output{};
        ASSERT_TRUE(mlm.get_message(output));
        EXPECT_EQ(output.hwnd, test_hwnd);
        EXPECT_EQ(output.pt.x, -20); EXPECT_EQ(output.pt.y, -40);
        EXPECT_EQ(output.lParam, input.lParam);
    }
}

// Learn DispatchMessageW returns the actual procedure result; a native system
// control still has an effective callable procedure when WindowData uses zero
// as its internal host-control marker. No SendMessage hook path is involved.
TEST_F(MessageLoopManagerTest, DispatchNativeControlUsesItsEffectiveProcedure) {
    auto mlm = make_mlm();
    WindowData native{};
    native.class_name = L"Button";
    native.parent = test_hwnd;
    native.wndproc = 0;
    const HWND hwnd = hwnd_mgr.create(native);
    const auto* window = hwnd_mgr.find(hwnd);
    ASSERT_NE(window, nullptr);
    const GuestAddr procedure = effective_wndproc(*window);
    ASSERT_NE(procedure, 0u);
    unsigned calls = 0;
    mlm.set_wndproc_callback([&](HWND h, u32 message, u64 wp, u64 lp, u64 proc) -> u64 {
        ++calls;
        EXPECT_EQ(h, hwnd); EXPECT_EQ(message, WM::PAINT);
        EXPECT_EQ(wp, 123u); EXPECT_EQ(lp, 456u); EXPECT_EQ(proc, procedure);
        return 0x8765432187654321ull;
    });
    MSG message{};
    message.hwnd = hwnd; message.message = WM::PAINT;
    message.wParam = 123; message.lParam = 456;
    EXPECT_EQ(mlm.dispatch_message(message), 0x8765432187654321ull);
    EXPECT_EQ(calls, 1u);
    EXPECT_EQ(mlm.last_dispatch_wndproc(), procedure);
    EXPECT_EQ(hwnd_mgr.find(hwnd)->wndproc, 0u);
}

// SetWindowSubclass callbacks may consume the message or call DefSubclassProc,
// which must reach the real native class procedure exactly once. Exercise both
// guest MSG ABIs and DispatchMessageA/W through the actual thunk marshaler.
TEST_F(MessageLoopManagerTest, DispatchNativeSubclassPreservesBothAbisAndDefaultChain) {
    const u32 previous_tid = ThreadManager::get_current_thread_id_static();
    struct RestoreThread { u32 tid; ~RestoreThread() { ThreadManager::set_current_thread_id(tid); } } restore{previous_tid};
    ThreadManager::set_current_thread_id(1);
    for (bool x64 : {false, true}) for (bool ansi : {false, true})
    for (const wchar_t* class_name : {L"Button", L"SysLink"}) {
        auto mlm = make_mlm();
        GuestMemory memory;
        ASSERT_TRUE(memory.map_region(0x10000, 0x10000, MemoryProtection::ReadWrite).is_ok());
        cpu::CpuState cpu;
        if (x64) cpu.init_long_mode(); else cpu.init_protected_mode();
        cpu.gpr.rsp = 0x18000;
        Marshaler marshaler(memory); marshaler.attach_cpu(cpu);
        WindowData native{};
        native.class_name = class_name; native.parent = test_hwnd;
        native.owner_thread_id = 1; native.wndproc = 0;
        const HWND hwnd = hwnd_mgr.create(native);
        const GuestAddr original = effective_wndproc(*hwnd_mgr.find(hwnd));
        ASSERT_NE(original, 0u);
        constexpr GuestAddr subclass = 0x402000;
        ComCtl32Manager common;
        common.set_hwnd_manager(&hwnd_mgr);
        ASSERT_TRUE(common.set_window_subclass(hwnd, subclass, 7, 0x1234));
        std::vector<GuestAddr> calls;
        const u64 result = x64 ? 0xfedcba9887654321ull : 0x87654321ull;
        const u64 wp = x64 ? 0x1234567887654321ull : 0x87654321ull;
        const u64 lp = x64 ? 0x8877665544332211ull : 0x44332211ull;
        common.set_guest_call([&](GuestAddr proc, const u64* args, usize count, bool actual_x64) -> u64 {
            calls.push_back(proc);
            EXPECT_EQ(actual_x64, x64); EXPECT_EQ(args[0], hwnd);
            EXPECT_EQ(args[2], wp); EXPECT_EQ(args[3], lp);
            if (proc == original) { EXPECT_EQ(count, 4u); return result; }
            EXPECT_EQ(proc, subclass); EXPECT_EQ(count, 6u);
            EXPECT_EQ(args[4], 7u); EXPECT_EQ(args[5], 0x1234u);
            if (args[1] == WM::PAINT) return result;
            if (x64) {
                cpu.gpr.rcx = args[0]; cpu.gpr.rdx = args[1];
                cpu.gpr.r8 = args[2]; cpu.gpr.r9 = args[3];
            } else {
                for (usize i = 0; i < 4; ++i)
                    (void)memory.write_u32(cpu.gpr.rsp + 4 + i * 4, static_cast<u32>(args[i]));
            }
            common.impl_DefSubclassProc(marshaler);
            return cpu.gpr.rax;
        });
        unsigned shortcuts = 0;
        mlm.set_native_control_dispatch([&](Marshaler& m, const MSG&, bool) {
            if (std::wstring_view(class_name) != L"SysLink") return false;
            ++shortcuts; m.set_return(0xbeef); return true;
        });
        mlm.set_wndproc_callback([&](HWND h, u32 message, u64 w, u64 l, u64 proc) {
            return common.dispatch_window_message(h, message, w, l, proc, x64);
        });
        for (u32 message : {WM::PAINT, WM::CLOSE}) {
            constexpr GuestAddr msg = 0x10000;
            if (x64) {
                (void)memory.write_u64(msg, hwnd); (void)memory.write_u32(msg + 8, message);
                (void)memory.write_u64(msg + 16, wp); (void)memory.write_u64(msg + 24, lp);
                cpu.gpr.rcx = msg;
            } else {
                (void)memory.write_u32(msg, hwnd); (void)memory.write_u32(msg + 4, message);
                (void)memory.write_u32(msg + 8, static_cast<u32>(wp));
                (void)memory.write_u32(msg + 12, static_cast<u32>(lp));
                (void)memory.write_u32(cpu.gpr.rsp + 4, static_cast<u32>(msg));
            }
            calls.clear();
            impl_DispatchMessageW(marshaler, mlm, ansi);
            EXPECT_EQ(cpu.gpr.rax, result);
            if (message == WM::PAINT) EXPECT_EQ(calls, (std::vector<GuestAddr>{subclass}));
            else EXPECT_EQ(calls, (std::vector<GuestAddr>{subclass, original}));
            EXPECT_EQ(hwnd_mgr.find(hwnd)->wndproc, 0u);
            EXPECT_EQ(shortcuts, 0u);
        }
    }
}
