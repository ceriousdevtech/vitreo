#include <gtest/gtest.h>
#include <vitreo/common.hpp>
#include <windowing/messages/message_queue.hpp>
#include <windowing/model/hwnd_manager.hpp>
#include <atomic>
#include <future>
#include <thread>

using namespace vitreo;
using namespace vitreo::windowing;

class MessageQueueTest : public ::testing::Test {
protected:
    MessageQueue queue;
};

// 1. Newly created queue is empty
TEST_F(MessageQueueTest, NewQueueIsEmpty) {
    EXPECT_TRUE(queue.empty());
    EXPECT_EQ(queue.size(), 0u);
}

TEST_F(MessageQueueTest, TranslatedCharacterPrecedesLaterHardwareEnter) {
    MSG key{};
    key.hwnd = 1;
    key.message = WM::KEYDOWN;
    key.wParam = 'A';
    ASSERT_TRUE(queue.post_input(key));
    key.wParam = 0x0D;
    ASSERT_TRUE(queue.post_input(key));
    MSG received{};
    ASSERT_TRUE(queue.get(received));
    ASSERT_EQ(received.wParam, 'A');
    MSG character{};
    character.hwnd = 1;
    character.message = WM::CHAR_MSG;
    character.wParam = 'a';
    ASSERT_TRUE(queue.post(character));
    ASSERT_TRUE(queue.get(received));
    EXPECT_EQ(received.message, WM::CHAR_MSG);
    ASSERT_TRUE(queue.get(received));
    EXPECT_EQ(received.wParam, 0x0Du);
}

TEST_F(MessageQueueTest, PostedKeyboardMessagesKeepPostedPriorityAndFifo) {
    MSG message{};
    message.hwnd = 1;
    message.message = WM::KEYUP;
    message.wParam = 'A';
    ASSERT_TRUE(queue.post_input(message));
    message.wParam = 'B';
    ASSERT_TRUE(queue.post(message));
    message.wParam = 'C';
    ASSERT_TRUE(queue.post(message));
    MSG received{};
    for (const auto expected : {'B', 'C', 'A'}) {
        ASSERT_TRUE(queue.get(received));
        EXPECT_EQ(received.wParam, static_cast<u64>(expected));
    }
}

// 2. Post and get round-trip
TEST_F(MessageQueueTest, PostAndGetRoundTrip) {
    MSG msg_in{};
    msg_in.hwnd = 1;
    msg_in.message = WM::PAINT;
    msg_in.wParam = 42;
    msg_in.lParam = 99;

    EXPECT_TRUE(queue.post(msg_in));

    MSG msg_out{};
    EXPECT_TRUE(queue.get(msg_out));
    EXPECT_EQ(msg_out.hwnd, 1u);
    EXPECT_EQ(msg_out.message, WM::PAINT);
    EXPECT_EQ(msg_out.wParam, 42u);
    EXPECT_EQ(msg_out.lParam, 99u);
}

// 3. FIFO ordering
TEST_F(MessageQueueTest, FIFOOrdering) {
    MSG a{};
    a.message = WM::CREATE;
    MSG b{};
    b.message = WM::DESTROY;

    queue.post(a);
    queue.post(b);

    MSG out{};
    EXPECT_TRUE(queue.get(out));
    EXPECT_EQ(out.message, WM::CREATE);

    EXPECT_TRUE(queue.get(out));
    EXPECT_EQ(out.message, WM::DESTROY);
}

// 4. Get returns false on empty queue
TEST_F(MessageQueueTest, GetReturnsFalseOnEmpty) {
    MSG msg{};
    EXPECT_FALSE(queue.get(msg));
}

// 5. post_quit sets quit flag
TEST_F(MessageQueueTest, PostQuitSetsFlag) {
    EXPECT_FALSE(queue.quit_posted());
    queue.post_quit(0);
    EXPECT_TRUE(queue.quit_posted());
}

// 6. Get synthesizes WM_QUIT after post_quit
TEST_F(MessageQueueTest, GetSynthesizesQuitMessage) {
    queue.post_quit(0);

    MSG msg{};
    EXPECT_TRUE(queue.get(msg));
    EXPECT_EQ(msg.message, WM::QUIT);
}

// 7. WM_QUIT has correct exit code in wParam
TEST_F(MessageQueueTest, QuitExitCodeInWParam) {
    queue.post_quit(42);

    MSG msg{};
    EXPECT_TRUE(queue.get(msg));
    EXPECT_EQ(msg.wParam, 42u);
    EXPECT_EQ(queue.quit_exit_code(), 42);
}

// 8. Peek with PM_NOREMOVE leaves message in queue
TEST_F(MessageQueueTest, PeekNoRemoveLeavesMessage) {
    MSG msg_in{};
    msg_in.message = WM::PAINT;
    queue.post(msg_in);

    MSG msg_out{};
    EXPECT_TRUE(queue.peek(msg_out, NULL_HWND, 0, 0, PM::NOREMOVE));
    EXPECT_EQ(msg_out.message, WM::PAINT);
    EXPECT_EQ(queue.size(), 1u);
}

// 9. Peek with PM_REMOVE removes message
TEST_F(MessageQueueTest, PeekRemoveRemovesMessage) {
    MSG msg_in{};
    msg_in.message = WM::PAINT;
    queue.post(msg_in);

    MSG msg_out{};
    EXPECT_TRUE(queue.peek(msg_out, NULL_HWND, 0, 0, PM::REMOVE));
    EXPECT_EQ(msg_out.message, WM::PAINT);
    EXPECT_TRUE(queue.empty());
}

// 10. Message filtering by HWND
TEST_F(MessageQueueTest, FilterByHWND) {
    MSG a{};
    a.hwnd = 1;
    a.message = WM::PAINT;

    MSG b{};
    b.hwnd = 2;
    b.message = WM::CLOSE;

    queue.post(a);
    queue.post(b);

    MSG out{};
    EXPECT_TRUE(queue.get(out, 2));
    EXPECT_EQ(out.hwnd, 2u);
    EXPECT_EQ(out.message, WM::CLOSE);

    // First message should still be in the queue
    EXPECT_EQ(queue.size(), 1u);
    EXPECT_TRUE(queue.get(out));
    EXPECT_EQ(out.hwnd, 1u);
}

// 11. Message filtering by range
TEST_F(MessageQueueTest, FilterByMessageRange) {
    MSG a{};
    a.message = WM::KEYDOWN;  // 0x0100

    MSG b{};
    b.message = WM::PAINT;    // 0x000F

    queue.post(a);
    queue.post(b);

    MSG out{};
    // Filter for keyboard messages only (0x0100 - 0x0101)
    EXPECT_TRUE(queue.get(out, NULL_HWND, 0x0100, 0x0101));
    EXPECT_EQ(out.message, WM::KEYDOWN);

    // Paint message remains
    EXPECT_EQ(queue.size(), 1u);
}

// 12. Clear empties the queue
TEST_F(MessageQueueTest, ClearEmptiesQueue) {
    MSG msg{};
    msg.message = WM::PAINT;
    queue.post(msg);
    queue.post(msg);
    queue.post(msg);

    EXPECT_EQ(queue.size(), 3u);
    queue.clear();
    EXPECT_TRUE(queue.empty());
}

TEST_F(MessageQueueTest, RemoveByTypeOnlyCoalescesMatchingWindowMessages) {
    MSG old_move{};
    old_move.hwnd = 1;
    old_move.message = WM::MOUSEMOVE;
    old_move.lParam = 0x00010001;
    queue.post(old_move);

    MSG other_window_move = old_move;
    other_window_move.hwnd = 2;
    queue.post(other_window_move);

    MSG button{};
    button.hwnd = 1;
    button.message = WM::LBUTTONUP;
    queue.post(button);

    queue.remove_by_type(1, WM::MOUSEMOVE);

    ASSERT_EQ(queue.size(), 2u);
    MSG out{};
    ASSERT_TRUE(queue.get(out));
    EXPECT_EQ(out.hwnd, 2u);
    EXPECT_EQ(out.message, WM::MOUSEMOVE);
    ASSERT_TRUE(queue.get(out));
    EXPECT_EQ(out.hwnd, 1u);
    EXPECT_EQ(out.message, WM::LBUTTONUP);
}

// 13. Size tracks correctly
TEST_F(MessageQueueTest, SizeTracksCorrectly) {
    MSG msg{};
    EXPECT_EQ(queue.size(), 0u);

    queue.post(msg);
    EXPECT_EQ(queue.size(), 1u);

    queue.post(msg);
    EXPECT_EQ(queue.size(), 2u);

    MSG out{};
    queue.get(out);
    EXPECT_EQ(queue.size(), 1u);
}

// 14. quit_posted returns correct state
TEST_F(MessageQueueTest, QuitPostedReturnsCorrectState) {
    EXPECT_FALSE(queue.quit_posted());
    queue.post_quit(0);
    EXPECT_TRUE(queue.quit_posted());
}

// 15. Post after quit still adds messages
TEST_F(MessageQueueTest, PostAfterQuitStillAddsMessages) {
    queue.post_quit(0);

    MSG msg{};
    msg.message = WM::PAINT;
    EXPECT_TRUE(queue.post(msg));
    EXPECT_EQ(queue.size(), 1u);

    // Get should return the posted message first, not WM_QUIT
    MSG out{};
    EXPECT_TRUE(queue.get(out));
    EXPECT_EQ(out.message, WM::PAINT);

    // Now with queue empty, get returns WM_QUIT
    EXPECT_TRUE(queue.get(out));
    EXPECT_EQ(out.message, WM::QUIT);
}

TEST_F(MessageQueueTest, ShutdownWakesAllWaiters) {
    auto first = std::async(std::launch::async, [this] {
        return queue.wait_for_post(std::chrono::seconds(5));
    });
    auto second = std::async(std::launch::async, [this] {
        return queue.wait_for_post(std::chrono::seconds(5));
    });

    queue.begin_shutdown();

    EXPECT_TRUE(first.get());
    EXPECT_TRUE(second.get());
    EXPECT_TRUE(queue.shutdown_requested());
}

TEST_F(MessageQueueTest, ShutdownReleasesNativeSendToStoppedGuest) {
    std::promise<void> waiting;
    bool signaled = false;
    auto sender = std::async(std::launch::async, [&] {
        return queue.send_to_thread(0, 1, [] { return 42u; }, [&](std::function<bool()>) {
            if (!signaled) {
                signaled = true;
                waiting.set_value();
            }
            std::this_thread::yield();
        });
    });
    waiting.get_future().wait();
    queue.begin_shutdown();
    EXPECT_EQ(sender.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    EXPECT_EQ(sender.get(), 0u);
}

TEST_F(MessageQueueTest, ThreadWaitIgnoresAnotherThreadsInput) {
    HwndManager hwnd_mgr;
    MSG other{};
    other.message = 0x0401;
    other.target_thread_id = 7;
    ASSERT_TRUE(queue.post(other));

    EXPECT_FALSE(queue.wait_for_input_for_thread(
        std::chrono::milliseconds(10), 1, hwnd_mgr, QS::ALLINPUT));

    MSG own = other;
    own.target_thread_id = 1;
    ASSERT_TRUE(queue.post(own));
    EXPECT_TRUE(queue.wait_for_input_for_thread(
        std::chrono::milliseconds(10), 1, hwnd_mgr, QS::ALLINPUT));
}

TEST_F(MessageQueueTest, ThreadWaitHonorsWakeMask) {
    HwndManager hwnd_mgr;
    MSG mouse{};
    mouse.message = WM::MOUSEMOVE;
    mouse.target_thread_id = 1;
    ASSERT_TRUE(queue.post_input(mouse));

    EXPECT_FALSE(queue.has_input_for_thread(1, hwnd_mgr, QS::POSTMESSAGE));
    EXPECT_TRUE(queue.has_input_for_thread(1, hwnd_mgr, QS::MOUSEMOVE));
}

// A PostMessage(WM_KEYDOWN / WM_MOUSEMOVE) is a POSTED message, not input:
// Windows reports it as QS_POSTMESSAGE (tools/oracle_probe/
// s2msg_queue_probe.expected status_posted_keydown = 0x00080008).
TEST_F(MessageQueueTest, PostedInputMessageIsPostedClass) {
    HwndManager hwnd_mgr;
    MSG key{};
    key.message = WM::KEYDOWN;
    key.target_thread_id = 1;
    ASSERT_TRUE(queue.post(key));
    EXPECT_FALSE(queue.has_input_for_thread(1, hwnd_mgr, QS::KEY));
    EXPECT_EQ(queue.get_queue_status(QS::ALLINPUT, 1, hwnd_mgr), 0x00080008u);
}

TEST_F(MessageQueueTest, GetQueueStatusReportsPostedWork) {
    // Learn: high word = present types; low word = changed-since-last-call.
    // Chromium MessagePumpForUI relies on a non-zero hint for kMsgHaveWork.
    HwndManager hwnd_mgr;
    EXPECT_EQ(queue.get_queue_status(QS::ALLINPUT, 1, hwnd_mgr), 0u);

    MSG work{};
    work.hwnd = NULL_HWND;
    work.message = 0x0401; // WM_USER+1 style posted work
    work.target_thread_id = 1;
    ASSERT_TRUE(queue.post(work));

    const u32 status = queue.get_queue_status(QS::ALLINPUT, 1, hwnd_mgr);
    EXPECT_NE(status & 0xFFFF0000u, 0u); // present
    EXPECT_NE(status & 0x0000FFFFu, 0u); // changed
    EXPECT_EQ(status & QS::POSTMESSAGE, QS::POSTMESSAGE);
    EXPECT_EQ((status >> 16) & QS::POSTMESSAGE, QS::POSTMESSAGE);

    // Second call: still present, but change bits were consumed.
    const u32 again = queue.get_queue_status(QS::ALLINPUT, 1, hwnd_mgr);
    EXPECT_EQ((again >> 16) & QS::POSTMESSAGE, QS::POSTMESSAGE);
    EXPECT_EQ(again & 0x0000FFFFu, 0u);
}

TEST_F(MessageQueueTest, GetQueueStatusIgnoresOtherThreads) {
    HwndManager hwnd_mgr;
    MSG work{};
    work.hwnd = NULL_HWND;
    work.message = 0x0401;
    work.target_thread_id = 7;
    ASSERT_TRUE(queue.post(work));
    EXPECT_EQ(queue.get_queue_status(QS::ALLINPUT, 1, hwnd_mgr), 0u);
    EXPECT_NE(queue.get_queue_status(QS::ALLINPUT, 7, hwnd_mgr), 0u);
}

// Posted work for a destroyed HWND must still belong to the posting thread
// when target_thread_id was recorded at PostMessage time.
TEST_F(MessageQueueTest, OrphanedHwndMessageUsesTargetThreadId) {
    HwndManager hwnd_mgr;
    MSG work{};
    work.hwnd = 0x10000; // never registered → owner_thread_id empty
    work.message = 0x0401;
    work.target_thread_id = 1;
    ASSERT_TRUE(queue.post(work));

    MSG out{};
    EXPECT_TRUE(queue.peek_for_thread(out, 1, hwnd_mgr, 0, 0, 0, 0x0001u));
    EXPECT_EQ(out.message, 0x0401u);
    EXPECT_EQ(out.hwnd, 0x10000u);

    EXPECT_FALSE(queue.peek_for_thread(out, 2, hwnd_mgr, 0, 0, 0, 0x0001u));
}

// target_thread_id wins over the HWND's current owner (HWND value reuse).
TEST_F(MessageQueueTest, TargetThreadIdWinsOverCurrentHwndOwner) {
    HwndManager hwnd_mgr;
    WindowData data{};
    data.owner_thread_id = 2;
    const HWND hwnd = hwnd_mgr.create(data);
    ASSERT_NE(hwnd, NULL_HWND);

    MSG work{};
    work.hwnd = hwnd;
    work.message = 0x0401;
    work.target_thread_id = 1; // posted when owned by tid=1
    ASSERT_TRUE(queue.post(work));

    MSG out{};
    EXPECT_TRUE(queue.peek_for_thread(out, 1, hwnd_mgr, 0, 0, 0, 0));
    EXPECT_EQ(out.message, 0x0401u);
    EXPECT_FALSE(queue.peek_for_thread(out, 2, hwnd_mgr, 0, 0, 0, 0));
}

TEST_F(MessageQueueTest, PostInvokesHostWake) {
    std::atomic<int> wakes{0};
    queue.set_host_wake([&] { wakes.fetch_add(1, std::memory_order_relaxed); });
    MSG msg{};
    msg.message = 0x0401;
    ASSERT_TRUE(queue.post(msg));
    EXPECT_EQ(wakes.load(std::memory_order_relaxed), 1);
}

// SendMessageTimeout (Learn, nf-winuser-sendmessagetimeoutw): a receiver
// that never checks its queue does not block the sender past uTimeout.
TEST_F(MessageQueueTest, TimedSendReturnsAfterTimeoutWhenReceiverIsBusy) {
    MessageQueue::SendWaitOptions options;
    options.timeout_ms = 50;
    MessageQueue::SendWaitStatus status{};
    const auto t0 = std::chrono::steady_clock::now();
    const u64 r = queue.send_to_thread(2, 9, [] { return 7u; }, {}, options, status);
    const auto elapsed = std::chrono::steady_clock::now() - t0;
    EXPECT_EQ(r, 0u);
    EXPECT_EQ(status, MessageQueue::SendWaitStatus::TimedOut);
    EXPECT_GE(elapsed, std::chrono::milliseconds(45));
    EXPECT_LT(elapsed, std::chrono::seconds(2));
    // The message stays queued: the receiver still processes it later.
    queue.dispatch_sent_messages(9);
}

TEST_F(MessageQueueTest, TimedSendCompletesWithTheReceiversResult) {
    MessageQueue::SendWaitOptions options;
    options.timeout_ms = 5000;
    MessageQueue::SendWaitStatus status{};
    std::atomic<bool> stop{false};
    std::thread receiver([&] {
        while (!stop) { queue.dispatch_sent_messages(9); std::this_thread::yield(); }
    });
    const u64 r = queue.send_to_thread(2, 9, [] { return 0x1234u; }, {}, options, status);
    stop = true;
    receiver.join();
    EXPECT_EQ(r, 0x1234u);
    EXPECT_EQ(status, MessageQueue::SendWaitStatus::Completed);
}

// SMTO_ERRORONEXIT: the target going away ends the wait with a failure.
TEST_F(MessageQueueTest, TimedSendErrorOnExitWhenTargetGone) {
    MessageQueue::SendWaitOptions options;
    std::atomic<int> polls{0};
    options.target_gone = [&] { return ++polls > 2; };
    MessageQueue::SendWaitStatus status{};
    EXPECT_EQ(queue.send_to_thread(2, 9, [] { return 1u; }, {}, options, status), 0u);
    EXPECT_EQ(status, MessageQueue::SendWaitStatus::TargetGone);
    queue.dispatch_sent_messages(9);
}

// SMTO_BLOCK: while waiting, the sender does not service sends aimed at it.
TEST_F(MessageQueueTest, TimedSendBlockDoesNotServiceIncomingSends) {
    std::atomic<bool> incoming_ran{false};
    std::thread other([&] {
        MessageQueue::SendWaitOptions o;
        o.timeout_ms = 200;
        MessageQueue::SendWaitStatus st{};
        (void)queue.send_to_thread(5, 2, [&] { incoming_ran = true; return 0u; }, {}, o, st);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    MessageQueue::SendWaitOptions options;
    options.timeout_ms = 60;
    options.block = true;
    MessageQueue::SendWaitStatus status{};
    (void)queue.send_to_thread(2, 9, [] { return 1u; }, {}, options, status);
    EXPECT_EQ(status, MessageQueue::SendWaitStatus::TimedOut);
    EXPECT_FALSE(incoming_ran.load());
    other.join();
    queue.dispatch_sent_messages(2);
    queue.dispatch_sent_messages(9);
}

TEST_F(MessageQueueTest, ThreadIsHungOnlyAfterAPendingSendAges) {
    EXPECT_FALSE(queue.thread_is_hung(9));
    MessageQueue::SendWaitOptions options;
    options.timeout_ms = 0;
    MessageQueue::SendWaitStatus status{};
    (void)queue.send_to_thread(2, 9, [] { return 1u; }, {}, options, status);
    EXPECT_EQ(status, MessageQueue::SendWaitStatus::TimedOut);
    EXPECT_FALSE(queue.thread_is_hung(9));  // pending, but not for 5 s
    queue.dispatch_sent_messages(9);
}

TEST_F(MessageQueueTest, ConfiguredPrimaryIdentityControlsHostWakeAndDefaultQuit) {
    queue.set_primary_thread_id(101);
    unsigned wakes = 0;
    queue.set_host_wake([&] { ++wakes; });
    MSG primary{};
    primary.message = WM::PAINT;
    primary.target_thread_id = 101;
    queue.post(primary);
    EXPECT_EQ(wakes, 1u);
    MSG worker = primary;
    worker.target_thread_id = 102;
    queue.post(worker);
    EXPECT_EQ(wakes, 1u);
    queue.clear();
    queue.post_quit(7);
    EXPECT_TRUE(queue.quit_posted());
    MSG quit{};
    ASSERT_TRUE(queue.peek(quit, NULL_HWND, 0, 0, PM::REMOVE));
    EXPECT_EQ(quit.message, WM::QUIT);
    EXPECT_EQ(quit.target_thread_id, 101u);
    EXPECT_EQ(queue.quit_exit_code(), 7);
}
