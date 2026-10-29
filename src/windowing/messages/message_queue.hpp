#pragma once
/// @file message_queue.hpp
/// @brief Per-thread message queue (WINDOW-002)

#include "message_types.hpp"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>

namespace vitreo::windowing {

class HwndManager;

class MessageQueue {
public:
    MessageQueue();
    /// Configure once before producers start; tests use the initial ID 1.
    void set_primary_thread_id(u32 tid) noexcept { m_primary_thread_id = tid; }
    [[nodiscard]] u32 primary_thread_id() const noexcept { return m_primary_thread_id; }

    // Post a message (async, returns immediately)
    bool post(const MSG& msg);
    /// Enqueue host keyboard input behind posted messages (GetMessage order).
    bool post_input(const MSG& msg);
    /// Enqueue hardware input ahead of all other queued hardware input.
    bool post_input_front(const MSG& msg);

    /// Low-level input hooks (WH_KEYBOARD_LL / WH_MOUSE_LL) see every hardware
    /// input event before it is queued. The filter returns true when it took
    /// the event; it re-injects it with post_input_unfiltered once the hook
    /// chain let it through. Set once at startup.
    using InputFilter = std::function<bool(const MSG& msg, bool front)>;
    void set_input_filter(InputFilter filter);
    bool post_input_unfiltered(const MSG& msg, bool front);

    /// Wake the host UI runloop when a message is posted. MsgWait on the
    /// AppKit thread parks in nextEvent, not the queue CV, so PostMessage
    /// must poke CFRunLoop or the wait only returns at the slice timeout.
    void set_host_wake(std::function<void()> fn);

    /// A worker thread parked in MsgWaitForMultipleObjects waits on its
    /// handles in ntsync, not on the queue CV. Registering a per-thread wake
    /// makes a post signal that thread's wait object immediately instead of
    /// being discovered at the next timed re-check.
    void register_thread_wake(u32 thread_id, std::function<void()> fn);
    void unregister_thread_wake(u32 thread_id);
    /// Resolves an HWND to its owner thread so a post wakes only the thread
    /// the message belongs to (the rule in belongs_to_thread). Without a
    /// resolver, HWND-addressed posts wake every registered thread.
    using HwndOwnerResolver = std::function<std::optional<u32>(HWND)>;
    void set_hwnd_owner_resolver(HwndOwnerResolver fn);

    /// Synchronous, same-process sent messages execute on the receiver thread.
    /// While blocked, the sender services only incoming sent messages.
    u64 send_to_thread(u32 sender, u32 receiver, std::function<u64()> callback,
                       std::function<void(std::function<bool()>)> host_wait = {});

    /// SendMessageTimeout's wait (Learn, nf-winuser-sendmessagetimeoutw).
    enum class SendWaitStatus : u8 { Completed, TimedOut, Hung, TargetGone, Shutdown };
    struct SendWaitOptions {
        u32 timeout_ms = 0xFFFFFFFFu;        ///< INFINITE = no limit
        bool block = false;                  ///< SMTO_BLOCK: service no sends while waiting
        bool abort_if_hung = false;          ///< SMTO_ABORTIFHUNG
        bool no_timeout_if_not_hung = false; ///< SMTO_NOTIMEOUTIFNOTHUNG
        std::function<bool()> target_gone;   ///< SMTO_ERRORONEXIT
    };
    u64 send_to_thread(u32 sender, u32 receiver, std::function<u64()> callback,
                       std::function<void(std::function<bool()>)> host_wait,
                       const SendWaitOptions& options, SendWaitStatus& status);

    /// IsHungAppWindow's rule applied to a thread: a message sent to it has
    /// waited kHungThreshold without the thread checking its queue.
    static constexpr std::chrono::milliseconds kHungThreshold{5000};
    [[nodiscard]] bool thread_is_hung(u32 thread_id) const;

    /// Run every message other threads sent to @p thread_id. True when at
    /// least one ran.
    bool dispatch_sent_messages(u32 thread_id);

    /// SendNotifyMessage to a window of another thread (Learn,
    /// nf-winuser-sendnotifymessagew): queue the message as a sent message
    /// for @p receiver and return at once. The receiver runs @p callback the
    /// next time it checks its queue, with InSendMessageEx == ISMEX_NOTIFY.
    bool notify_thread_send(u32 sender, u32 receiver, std::function<u64()> callback);

    /// InSendMessageEx flags (winuser.h) for the message the CALLING thread is
    /// processing: ISMEX_NOSEND (0) unless it is running a message another
    /// thread sent (ISMEX_SEND 1 / ISMEX_NOTIFY 2), | ISMEX_REPLIED (8) once
    /// ReplyMessage answered it.
    [[nodiscard]] static u32 current_send_flags() noexcept;
    /// ReplyMessage (Learn): release the thread that sent the message being
    /// processed, with @p result as its SendMessage return. FALSE when the
    /// caller is not processing a cross-thread SendMessage (none, a notify,
    /// or already replied).
    static bool reply_message(u64 result);

    /// Wake the waiters of @p thread_id (GetMessage, WaitMessage, MsgWait)
    /// after state they synthesize from changed without a message being
    /// queued: a window of the thread was invalidated, a timer was armed for
    /// it, a WinEvent was queued for it. @p qs_bits are recorded as new input
    /// for GetQueueStatus's low word and MsgWait without MWMO_INPUTAVAILABLE.
    void notify_thread(u32 thread_id, u32 qs_bits = 0);
    /// Same, for state that belongs to no single thread.
    void notify_all_threads();
    /// Count of wakes addressed to @p thread_id, process-wide ones included.
    /// A waiter reads it BEFORE checking its wake sources and hands it to
    /// wait_for_thread_wake, so a wake that lands between the check and the
    /// wait is never lost.
    [[nodiscard]] u64 wake_generation(u32 thread_id) const;
    /// Block until wake_generation(@p thread_id) differs from @p generation,
    /// shutdown begins, or @p timeout elapses. True when woken.
    bool wait_for_thread_wake(u32 thread_id, u64 generation,
                              std::chrono::milliseconds timeout);
    /// How many times wait_for_thread_wake has parked (all threads): an idle
    /// GetMessage must park once and stay parked, not re-park on a clock.
    [[nodiscard]] u64 thread_wait_count() const noexcept {
        return m_thread_wait_count.load(std::memory_order_relaxed);
    }
    /// Record @p qs_bits as new input for @p thread_id without waking it
    /// (a synthesized source the caller just observed becoming ready).
    void mark_changed(u32 thread_id, u32 qs_bits);

    // Post WM_QUIT with exit code
    void post_quit(int exit_code, u32 thread_id = 0);

    // Get next message (non-blocking).
    // Returns true if a message was produced (including WM_QUIT).
    // Returns false only if the queue is empty and WM_QUIT was not posted.
    bool get(MSG& msg, HWND filter_hwnd = NULL_HWND,
             u32 filter_min = 0, u32 filter_max = 0);

    // Get the next message belonging to a guest thread. Window messages are
    // routed by their HWND creator; hwnd-less messages use target_thread_id.
    // Retrieval order (s2msg_queue_probe "order"): sent messages are
    // dispatched first, then posted messages, then WM_QUIT, then hardware
    // input. WM_PAINT and WM_TIMER are synthesized by the caller after these.
    bool get_for_thread(MSG& msg, u32 thread_id, const HwndManager& hwnd_mgr,
                        HWND filter_hwnd = NULL_HWND,
                        u32 filter_min = 0, u32 filter_max = 0);

    // Peek without removing (PM_NOREMOVE) or with removing (PM_REMOVE)
    bool peek(MSG& msg, HWND filter_hwnd = NULL_HWND,
              u32 filter_min = 0, u32 filter_max = 0,
              u32 remove_flags = PM::NOREMOVE);

    /// @p remove_flags is PeekMessage's wRemoveMsg: PM_REMOVE plus the
    /// PM_QS_* class mask in its high word.
    /// @p sent_dispatched (optional) reports whether sent messages ran: with a
    /// PM_QS_* mask that ends the PeekMessage call with FALSE
    /// (s2msg_queue_probe pm_qs_paint_with_pending_send).
    bool peek_for_thread(MSG& msg, u32 thread_id, const HwndManager& hwnd_mgr,
                         HWND filter_hwnd = NULL_HWND,
                         u32 filter_min = 0, u32 filter_max = 0,
                         u32 remove_flags = PM::NOREMOVE,
                         bool* sent_dispatched = nullptr);

    /// The GetMessage/PeekMessage filter rule for one message (Learn plus
    /// s2msg_queue_probe): hWnd (HWND)-1 selects only messages whose hwnd is
    /// NULL; any other non-NULL hWnd selects that window; both bounds 0 is no
    /// range filter; an inverted range (min > max) matches no posted message.
    [[nodiscard]] static bool filter_matches(HWND msg_hwnd, u32 message,
                                             HWND filter_hwnd, u32 filter_min,
                                             u32 filter_max,
                                             const HwndManager* hwnd_mgr = nullptr) noexcept;
    /// The GetMessage/PeekMessage hWnd filter selects that window AND its
    /// child windows (s2msg_queue_probe parent_filter_child_msg): true when
    /// @p hwnd is @p filter or reaches it through WS_CHILD parents.
    [[nodiscard]] static bool hwnd_filter_selects(const HwndManager& hwnd_mgr,
                                                  HWND hwnd, HWND filter) noexcept;

    // Check if WM_QUIT has been posted
    bool quit_posted(u32 thread_id = 0) const;

    // Get quit exit code (only valid if quit_posted())
    int quit_exit_code(u32 thread_id = 0) const;

    // Check if queue is empty
    bool empty() const;

    // Get number of pending messages
    usize size() const;

    // Clear all messages
    void clear();

    // Remove all messages of a given type for a given hwnd.
    // Used to coalesce WM_SIZE during live resize so only the latest is kept.
    void remove_by_type(HWND hwnd, u32 message_type);

    /// Remove one queued message identified by its complete Win32 routing
    /// tuple. Raw-input buffering uses this to consume the WM_INPUT whose
    /// HRAWINPUT packet it copied.
    bool remove_exact(HWND hwnd, u32 message_type, u64 lparam);

    // Block until a message is posted (or quit) or timeout expires.
    // Returns true if woken by a post, false on timeout.
    bool wait_for_post(std::chrono::milliseconds timeout);

    /// Block until input matching wake_mask is available for thread_id.
    ///
    /// Vitreo stores process messages in one host container, while Windows
    /// exposes a distinct input queue to each GUI thread. Callers that model
    /// Win32 thread waits must use this filtered form so work queued for one
    /// guest thread cannot make every other guest thread runnable.
    bool wait_for_input_for_thread(std::chrono::milliseconds timeout,
                                   u32 thread_id,
                                   const HwndManager& hwnd_mgr,
                                   u32 wake_mask);

    /// Non-consuming input readiness check for a guest thread.
    [[nodiscard]] bool has_input_for_thread(u32 thread_id,
                                            const HwndManager& hwnd_mgr,
                                            u32 wake_mask) const;

    /// Like has_input_for_thread, but only messages posted since the last
    /// Peek/Get/GetQueueStatus (Learn MsgWaitForMultipleObjectsEx without
    /// MWMO_INPUTAVAILABLE ignores already-seen unread input).
    [[nodiscard]] bool has_new_input_for_thread(u32 thread_id,
                                                const HwndManager& hwnd_mgr,
                                                u32 wake_mask) const;

    /// GetQueueStatus (Learn): high word = types currently in this thread's
    /// queue & flags; low word = types added since the last GetQueueStatus /
    /// GetMessage / PeekMessage that are still present. Clears the reported
    /// change bits. A permanent 0 breaks a UI-thread message pump that treats
    /// a zero queue status as idle.
    /// @p synthesized_present: QS_PAINT / QS_TIMER the caller found ready for
    /// the thread (those messages are synthesized, not stored here); their
    /// change bits arrive through notify_thread / mark_changed.
    [[nodiscard]] u32 get_queue_status(u32 flags, u32 thread_id,
                                       const HwndManager& hwnd_mgr,
                                       u32 synthesized_present = 0);

    // Wake every waiter during process teardown. Unlike WM_QUIT, this is a
    // host-lifetime signal and is visible to all guest message threads.
    void begin_shutdown() noexcept;
    [[nodiscard]] bool shutdown_requested() const noexcept {
        return m_shutdown.load(std::memory_order_acquire);
    }

    /// Debug: print up to max_messages queued entries to stderr.
    void dump_queue(const char* tag, usize max_messages = 16) const;

private:
    bool post_impl(const MSG& msg, bool hardware_input, bool front = false);
    bool matches_filter(const MSG& msg, HWND filter_hwnd,
                        u32 filter_min, u32 filter_max) const;
    /// Shared body of get_for_thread / peek_for_thread.
    bool retrieve_for_thread(MSG& msg, u32 thread_id, const HwndManager& hwnd_mgr,
                             HWND filter_hwnd, u32 filter_min, u32 filter_max,
                             bool remove, u32 qs_mask,
                             bool* sent_dispatched = nullptr);
public:
    /// Diagnostic snapshot: queue depth plus the head message's hwnd, message
    /// id, recorded target thread and the thread HwndManager says owns its
    /// window. Used to tell "queue is empty" apart from "queue holds input
    /// that is no longer attributed to the waiting thread".
    usize diag_head(const HwndManager& hwnd_mgr, u32& hwnd, u32& message,
                    u32& target_tid, int& owner_tid) const;

private:
    bool belongs_to_thread(const MSG& msg, u32 thread_id,
                           const HwndManager& hwnd_mgr) const;

    struct QueuedMessage : MSG {};
public:
    struct SentMessage {
        std::function<u64()> callback;
        u64 result = 0;
        bool complete = false; // protected by m_mutex
        std::chrono::steady_clock::time_point queued_at{};
        u32 sender = 0;
        /// ISMEX_SEND or ISMEX_NOTIFY.
        u32 kind = 1;
        /// ReplyMessage already answered the sender (protected by m_mutex).
        bool replied = false;
    };
private:
    std::unordered_map<u32, std::deque<std::shared_ptr<SentMessage>>> m_sent;
    std::deque<QueuedMessage> m_queue;
    u32 m_primary_thread_id{1};
    mutable std::mutex m_mutex;
    std::condition_variable m_cv;
    std::function<void()> m_host_wake;
    InputFilter m_input_filter;
    std::unordered_map<u32, std::function<void()>> m_thread_wakes;
    HwndOwnerResolver m_hwnd_owner_resolver;
    /// Notify the queue CV, the host run loop, and the registered per-thread
    /// wake of the thread @p msg belongs to (every registered thread when
    /// @p msg is null or its owner cannot be resolved).
    void wake_waiters(const MSG* msg = nullptr);
    /// Wake the waiters of one thread (nullopt = every thread).
    void wake_owner(std::optional<u32> owner);
    /// Which thread a queued message belongs to (belongs_to_thread's rule);
    /// nullopt when its window's owner cannot be resolved. Caller holds m_mutex.
    [[nodiscard]] std::optional<u32> owner_locked(const MSG& msg) const;
    [[nodiscard]] u64 generation_locked(u32 thread_id) const;
    std::unordered_map<u32, int> m_quit_codes;
    std::unordered_map<u32, int> m_last_quit_codes;
    /// Per thread: QS_* bits added since that thread's last GetQueueStatus /
    /// GetMessage / PeekMessage (Learn GetQueueStatus low word). Windows keeps
    /// these per thread queue; a process-wide word let one thread's post make
    /// another thread's already-seen input look new.
    std::unordered_map<u32, u32> m_changed_flags;
    std::unordered_map<u32, u64> m_wake_generation;
    u64 m_global_wake_generation{0};
    std::atomic<u64> m_thread_wait_count{0};
    std::atomic<bool> m_shutdown{false};
};

} // namespace vitreo::windowing
