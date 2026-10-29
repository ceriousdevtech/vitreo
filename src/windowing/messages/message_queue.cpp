/// @file message_queue.cpp
/// @brief Per-thread message queue implementation (WINDOW-002)

#include "windowing/messages/message_queue.hpp"
#include "windowing/messages/input_latency.hpp"
#include "windowing/model/hwnd_manager.hpp"
#include <atomic>
#include <algorithm>
#include <cstdlib>
#include <cstdio>
#include <unistd.h>

namespace vitreo::windowing {

static inline bool diag_logs_enabled() noexcept {
    static const bool enabled = (std::getenv("VITREO_ENABLE_DIAG_LOGS") != nullptr);
    return enabled;
}

/// The QS_* class a queued message reports (GetQueueStatus, MsgWait wake
/// masks, PM_QS_*). A message a thread POSTED is QS_POSTMESSAGE whatever its
/// number (a PostMessage(WM_KEYDOWN) is posted, not input: s2msg_queue_probe
/// status_posted_keydown); only the hardware input stream reports input
/// classes.
[[nodiscard]] static u32 classify_qs_bits(const MSG& msg) noexcept {
    if (!msg.hardware_input) return QS::POSTMESSAGE | QS::ALLPOSTMESSAGE;
    const u32 message = msg.message;
    if (message == WM::INPUT) return QS::RAWINPUT;
    if (message >= WM::KEYDOWN && message <= 0x0109u) return QS::KEY;
    if (message == WM::MOUSEMOVE || message == 0x00A0u /* WM_NCMOUSEMOVE */) {
        return QS::MOUSEMOVE;
    }
    if ((message > WM::MOUSEMOVE && message <= 0x020Eu /* WM_MOUSEHWHEEL */) ||
        (message > 0x00A0u && message <= 0x00ADu /* WM_NCXBUTTONDBLCLK */)) {
        return QS::MOUSEBUTTON;
    }
    if (message == 0x0240u /* WM_TOUCH */) return QS::TOUCH;
    if (message >= 0x0241u && message <= 0x0257u) return QS::POINTER;
    return QS::POSTMESSAGE | QS::ALLPOSTMESSAGE;
}

namespace {
// The message the calling thread is processing because another thread sent
// it (InSendMessageEx / ReplyMessage). A stack: a sent-message handler can
// itself receive a nested cross-thread send while it waits on one.
struct SendContext {
    std::shared_ptr<MessageQueue::SentMessage> request;
    MessageQueue* queue;
};
thread_local std::vector<SendContext> t_send_stack;
constexpr u32 kIsmexReplied = 0x00000008u;
} // namespace

MessageQueue::MessageQueue() = default;

void MessageQueue::set_host_wake(std::function<void()> fn) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_host_wake = std::move(fn);
}

void MessageQueue::register_thread_wake(u32 thread_id, std::function<void()> fn) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_thread_wakes[thread_id] = std::move(fn);
}

void MessageQueue::unregister_thread_wake(u32 thread_id) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_thread_wakes.erase(thread_id);
}

void MessageQueue::set_hwnd_owner_resolver(HwndOwnerResolver fn) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_hwnd_owner_resolver = std::move(fn);
}

std::optional<u32> MessageQueue::owner_locked(const MSG& msg) const {
    if (msg.target_thread_id != 0) return msg.target_thread_id;
    if (msg.hwnd == NULL_HWND) return m_primary_thread_id;
    if (m_hwnd_owner_resolver) return m_hwnd_owner_resolver(msg.hwnd);
    return std::nullopt;
}

u64 MessageQueue::generation_locked(u32 thread_id) const {
    const auto it = m_wake_generation.find(thread_id);
    return m_global_wake_generation + (it != m_wake_generation.end() ? it->second : 0u);
}

void MessageQueue::wake_waiters(const MSG* msg) {
    std::optional<u32> owner;
    if (msg != nullptr) {
        std::lock_guard<std::mutex> lock(m_mutex);
        owner = owner_locked(*msg);
    }
    wake_owner(owner);
}

void MessageQueue::wake_owner(std::optional<u32> owner) {
    // Three waiter kinds: the queue CV (GetMessage; predicates filter, so it
    // is broadcast), the host run loop (the AppKit thread, guest tid 0, and
    // legacy tid 1 park there), and the per-thread ntsync wake of a worker
    // MsgWait parked on its handles. The last is targeted: on a
    // this queue, posts arrive thousands of times a second, and
    // waking every parked MsgWait on each one made each of them rescan the
    // queue. Measured cost: +30 CPU-s during a launcher login. The owner is
    // the thread belongs_to_thread would pick. Hooks are copied out so a
    // wake never runs under m_mutex.
    std::vector<std::function<void()>> hooks;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (owner) {
            ++m_wake_generation[*owner];
        } else {
            ++m_global_wake_generation;
        }
        hooks.reserve(m_thread_wakes.size() + 1);
        if (m_host_wake && (!owner || *owner == 0 || *owner == m_primary_thread_id)) {
            hooks.push_back(m_host_wake);
        }
        if (owner) {
            auto it = m_thread_wakes.find(*owner);
            if (it != m_thread_wakes.end() && it->second) hooks.push_back(it->second);
        } else {
            for (const auto& [tid, fn] : m_thread_wakes) {
                (void)tid;
                if (fn) hooks.push_back(fn);
            }
        }
    }
    m_cv.notify_all();
    for (auto& fn : hooks) fn();
}

void MessageQueue::notify_thread(u32 thread_id, u32 qs_bits) {
    if (qs_bits != 0) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_changed_flags[thread_id] |= qs_bits;
    }
    wake_owner(thread_id);
}

void MessageQueue::notify_all_threads() {
    wake_owner(std::nullopt);
}

void MessageQueue::mark_changed(u32 thread_id, u32 qs_bits) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_changed_flags[thread_id] |= qs_bits;
}

u64 MessageQueue::wake_generation(u32 thread_id) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return generation_locked(thread_id);
}

bool MessageQueue::wait_for_thread_wake(u32 thread_id, u64 generation,
                                        std::chrono::milliseconds timeout) {
    m_thread_wait_count.fetch_add(1, std::memory_order_relaxed);
    std::unique_lock<std::mutex> lock(m_mutex);
    const auto woken = [&] {
        return m_shutdown.load(std::memory_order_acquire) ||
               generation_locked(thread_id) != generation;
    };
    if (timeout == std::chrono::milliseconds::max()) {
        m_cv.wait(lock, woken);
        return true;
    }
    return m_cv.wait_for(lock, timeout, woken);
}

bool MessageQueue::post(const MSG& msg) {
    return post_impl(msg, false);
}

bool MessageQueue::post_input(const MSG& msg) {
    if (m_input_filter && m_input_filter(msg, false)) return true;
    return post_impl(msg, true);
}

bool MessageQueue::post_input_front(const MSG& msg) {
    if (m_input_filter && m_input_filter(msg, true)) return true;
    return post_impl(msg, true, true);
}

void MessageQueue::set_input_filter(InputFilter filter) {
    m_input_filter = std::move(filter);
}

bool MessageQueue::post_input_unfiltered(const MSG& msg, bool front) {
    return post_impl(msg, true, front);
}

bool MessageQueue::post_impl(const MSG& msg, bool hardware_input, bool front) {
    {
    std::lock_guard<std::mutex> lock(m_mutex);

        // Coalesce mouse-move spam so modal dialogs don't get starved by a
        // backlog of stale cursor positions while long-running work is active.
        // A front post precedes the queued moves, so they are newer than it.
        if (msg.message == WM::MOUSEMOVE && !front) {
            auto it = m_queue.begin();
            while (it != m_queue.end()) {
                if (it->hwnd == msg.hwnd && it->message == WM::MOUSEMOVE) {
                    it = m_queue.erase(it);
                } else {
                    ++it;
                }
            }
        }

        QueuedMessage queued;
        static_cast<MSG&>(queued) = msg;
        // Stamp the arrival time once, here, so every producer is covered
        // without each having to remember. A caller that already stamped it
        // (a message re-posted after coalescing, say) keeps its original time,
        // which is the one the latency probe wants. Diagnostic only.
        if (input_latency::enabled() && queued.host_post_ns == 0) {
            queued.host_post_ns = input_latency::now_ns();
        }
        queued.hardware_input = hardware_input;
        // Posted messages take precedence over hardware input even when
        // posted later. Keep FIFO ordering within each source, independent
        // of the message number (PostMessage may itself post WM_KEYDOWN).
        auto position = m_queue.end();
        if (!hardware_input || front) {
            position = std::find_if(m_queue.begin(), m_queue.end(),
                [](const auto& entry) { return entry.hardware_input; });
        }
        const auto inserted = m_queue.insert(position, queued);
        if (const auto owner = owner_locked(*inserted)) {
            m_changed_flags[*owner] |= classify_qs_bits(*inserted);
        }
    }
    // Waiters apply guest-thread predicates to this process-wide container.
    // Wake all of them so the matching guest thread always observes the post;
    // non-matching waiters immediately go back to sleep.
    wake_waiters(&msg);
    return true;
}

u64 MessageQueue::send_to_thread(u32 sender, u32 receiver,
                                  std::function<u64()> callback,
                                  std::function<void(std::function<bool()>)> host_wait) {
    SendWaitStatus status = SendWaitStatus::Completed;
    return send_to_thread(sender, receiver, std::move(callback),
                          std::move(host_wait), SendWaitOptions{}, status);
}

bool MessageQueue::thread_is_hung(u32 thread_id) const {
    std::lock_guard lock(m_mutex);
    const auto it = m_sent.find(thread_id);
    if (it == m_sent.end() || it->second.empty()) return false;
    return std::chrono::steady_clock::now() - it->second.front()->queued_at >=
           kHungThreshold;
}

u64 MessageQueue::send_to_thread(u32 sender, u32 receiver,
                                  std::function<u64()> callback,
                                  std::function<void(std::function<bool()>)> host_wait,
                                  const SendWaitOptions& options,
                                  SendWaitStatus& status) {
    status = SendWaitStatus::Completed;
    // SMTO_ABORTIFHUNG: "does not wait for the time-out period to elapse if
    // the receiving process appears to not respond or hangs".
    if (options.abort_if_hung && thread_is_hung(receiver)) {
        status = SendWaitStatus::Hung;
        return 0;
    }
    auto request = std::make_shared<SentMessage>();
    request->callback = std::move(callback);
    request->queued_at = std::chrono::steady_clock::now();
    request->sender = sender;
    {
        std::lock_guard lock(m_mutex);
        if (m_shutdown.load(std::memory_order_acquire)) {
            status = SendWaitStatus::Shutdown;
            return 0;
        }
        m_sent[receiver].push_back(request);
        m_changed_flags[receiver] |= QS::SENDMESSAGE;
    }
    wake_owner(receiver);

    const bool timed = options.timeout_ms != 0xFFFFFFFFu;
    const auto deadline = request->queued_at +
        std::chrono::milliseconds(timed ? options.timeout_ms : 0);
    const bool polled = timed || options.abort_if_hung ||
                        static_cast<bool>(options.target_gone);
    constexpr auto kPoll = std::chrono::milliseconds(20);
    for (;;) {
        // Do not hold the queue lock across guest code. A reciprocal send
        // must be able to reenter the original sender's window procedure.
        // SMTO_BLOCK: the sender services no incoming sends while it waits.
        if (!options.block) {
            dispatch_sent_messages(sender);
        }
        std::unique_lock lock(m_mutex);
        const auto ready = [&] {
            return request->complete || m_shutdown.load(std::memory_order_acquire) ||
                   (!options.block && m_sent.contains(sender));
        };
        if (!request->complete && !m_shutdown.load(std::memory_order_acquire) &&
            polled) {
            const auto now = std::chrono::steady_clock::now();
            if (options.target_gone) {
                lock.unlock();
                const bool gone = options.target_gone();
                lock.lock();
                if (gone && !request->complete) {
                    // Leave the request queued: the receiver may still run it.
                    status = SendWaitStatus::TargetGone;
                    return 0;
                }
            }
            if (options.abort_if_hung && now - request->queued_at >= kHungThreshold) {
                status = SendWaitStatus::Hung;
                return 0;
            }
            if (timed && now >= deadline && !request->complete) {
                // SMTO_NOTIMEOUTIFNOTHUNG: the time-out is not enforced while
                // the receiver keeps processing messages.
                const bool hung = now - request->queued_at >= kHungThreshold;
                if (!options.no_timeout_if_not_hung || hung) {
                    // Leave the request queued for a later receiver pump
                    // (Learn: timeout ends the wait; delivery is separate).
                    status = SendWaitStatus::TimedOut;
                    return 0;
                }
            }
        }
        if (host_wait && !ready()) {
            // The AppKit thread must service native work requested by the
            // receiving thread. The host callback never retrieves guest MSGs.
            lock.unlock();
            host_wait([this, request, sender, block = options.block] {
                std::lock_guard check(m_mutex);
                return request->complete ||
                    m_shutdown.load(std::memory_order_acquire) ||
                    (!block && m_sent.contains(sender));
            });
            continue;
        }
        if (polled) {
            auto until = std::chrono::steady_clock::now() + kPoll;
            if (timed && deadline < until) until = deadline;
            m_cv.wait_until(lock, until, ready);
        } else {
            m_cv.wait(lock, ready);
        }
        if (request->complete) {
            return request->result;
        }
        if (m_shutdown.load(std::memory_order_acquire)) {
            status = SendWaitStatus::Shutdown;
            return 0;
        }
    }
}

bool MessageQueue::dispatch_sent_messages(u32 thread_id) {
    bool ran = false;
    for (;;) {
        std::shared_ptr<SentMessage> request;
        {
            std::lock_guard lock(m_mutex);
            auto it = m_sent.find(thread_id);
            if (it == m_sent.end()) return ran;
            ran = true;
            request = std::move(it->second.front());
            it->second.pop_front();
            if (it->second.empty()) m_sent.erase(it);
        }
        t_send_stack.push_back(SendContext{request, this});
        struct Pop { ~Pop() { t_send_stack.pop_back(); } } pop;
        const u64 result = request->callback();
        {
            std::lock_guard lock(m_mutex);
            // ReplyMessage already gave the sender its result; the window
            // procedure's own return value is then discarded (Learn).
            if (!request->replied) request->result = result;
            request->complete = true;
        }
        if (request->kind == 1u /* ISMEX_SEND */) wake_owner(request->sender);
    }
}

bool MessageQueue::notify_thread_send(u32 sender, u32 receiver,
                                      std::function<u64()> callback) {
    auto request = std::make_shared<SentMessage>();
    request->callback = std::move(callback);
    request->queued_at = std::chrono::steady_clock::now();
    request->sender = sender;
    request->kind = 2u; // ISMEX_NOTIFY
    {
        std::lock_guard lock(m_mutex);
        if (m_shutdown.load(std::memory_order_acquire)) return false;
        m_sent[receiver].push_back(request);
        m_changed_flags[receiver] |= QS::SENDMESSAGE;
    }
    wake_owner(receiver);
    return true;
}

u32 MessageQueue::current_send_flags() noexcept {
    if (t_send_stack.empty()) return 0; // ISMEX_NOSEND
    const SendContext& ctx = t_send_stack.back();
    std::lock_guard lock(ctx.queue->m_mutex);
    return ctx.request->kind | (ctx.request->replied ? kIsmexReplied : 0u);
}

bool MessageQueue::reply_message(u64 result) {
    if (t_send_stack.empty()) return false;
    const SendContext& ctx = t_send_stack.back();
    u32 sender = 0;
    {
        std::lock_guard lock(ctx.queue->m_mutex);
        SentMessage& request = *ctx.request;
        // A notification has no waiting sender to release.
        if (request.kind != 1u || request.replied) return false;
        request.replied = true;
        request.result = result;
        request.complete = true;
        sender = request.sender;
    }
    ctx.queue->wake_owner(sender);
    return true;
}

void MessageQueue::post_quit(int exit_code, u32 thread_id) {
    if (thread_id == 0) thread_id = m_primary_thread_id;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_quit_codes[thread_id] = exit_code;
        m_last_quit_codes[thread_id] = exit_code;
    }
    if (diag_logs_enabled()) {
        fprintf(stderr, "[quit-diag] post_quit(exit_code=%d tid=%u)\n",
                exit_code, thread_id);
    }
    wake_owner(thread_id);
}

bool MessageQueue::get(MSG& msg, HWND filter_hwnd, u32 filter_min, u32 filter_max) {
    std::lock_guard<std::mutex> lock(m_mutex);
    // Search for a matching message in the queue
    for (auto it = m_queue.begin(); it != m_queue.end(); ++it) {
        if (matches_filter(*it, filter_hwnd, filter_min, filter_max)) {
            msg = *it;
            m_queue.erase(it);
            // Learn: GetMessage clears QS_POSTMESSAGE change tracking; unfiltered
            // also clears QS_ALLPOSTMESSAGE.
            m_changed_flags[m_primary_thread_id] &= ~QS::POSTMESSAGE;
            if (filter_min == 0 && filter_max == 0) {
                m_changed_flags[m_primary_thread_id] &= ~QS::ALLPOSTMESSAGE;
            }
            return true;
        }
    }

    // No matching message found — if quit was posted, synthesize WM_QUIT.
    // Clear the flag so that nested message loops (a plugin dialog running
    // inside a DialogBoxParamW callback) can enter GetMessage
    // normally after an outer loop has consumed the WM_QUIT.
    // This matches Win32 semantics where GetMessage removes WM_QUIT.
    auto quit = m_quit_codes.find(m_primary_thread_id);
    if (quit != m_quit_codes.end()) {
        msg = {};
        msg.message = WM::QUIT;
        msg.wParam = static_cast<u64>(quit->second);
        msg.target_thread_id = m_primary_thread_id;
        m_quit_codes.erase(quit);
        return true;
    }

    // Empty queue, no quit
    return false;
}

bool MessageQueue::get_for_thread(MSG& msg, u32 thread_id,
                                  const HwndManager& hwnd_mgr,
                                  HWND filter_hwnd, u32 filter_min,
                                  u32 filter_max) {
    return retrieve_for_thread(msg, thread_id, hwnd_mgr, filter_hwnd,
                               filter_min, filter_max, /*remove=*/true,
                               /*qs_mask=*/0);
}

bool MessageQueue::retrieve_for_thread(MSG& msg, u32 thread_id,
                                       const HwndManager& hwnd_mgr,
                                       HWND filter_hwnd, u32 filter_min,
                                       u32 filter_max, bool remove,
                                       u32 qs_mask, bool* sent_dispatched) {
    // Sent messages are dispatched first, inside the call, and never
    // returned (Learn PeekMessage remarks). Under a PM_QS_* mask Windows 11
    // runs them for PM_QS_SENDMESSAGE and PM_QS_PAINT, not for
    // PM_QS_POSTMESSAGE, and a call that ran any returns FALSE
    // (s2msg_queue_probe pm_qs_paint_with_pending_send, pm_qs_sendmessage,
    // pm_qs_postmessage_with_pending_send).
    const bool class_filtered = qs_mask != 0;
    bool ran = false;
    if (!class_filtered || (qs_mask & (QS::SENDMESSAGE | QS::PAINT)) != 0) {
        ran = dispatch_sent_messages(thread_id);
    }
    if (sent_dispatched != nullptr) *sent_dispatched = ran;
    if (class_filtered && ran) return false;
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto quit = m_quit_codes.find(thread_id);
    const bool quit_allowed =
        !class_filtered || (qs_mask & QS::POSTMESSAGE) != 0;
    const auto take_quit = [&]() {
        msg = {};
        msg.message = WM::QUIT;
        msg.wParam = static_cast<u64>(quit->second);
        msg.target_thread_id = thread_id;
        if (remove) m_quit_codes.erase(quit);
    };
    for (auto it = m_queue.begin(); it != m_queue.end(); ++it) {
        // WM_QUIT comes after every posted message and before hardware input
        // (s2msg_queue_probe "order": posted, QUIT, input, PAINT, TIMER).
        // Posted entries always precede hardware entries in m_queue.
        if (it->hardware_input && quit != m_quit_codes.end() && quit_allowed) {
            take_quit();
            return true;
        }
        if (!filter_matches(it->hwnd, it->message, filter_hwnd, filter_min,
                            filter_max, &hwnd_mgr) ||
            !belongs_to_thread(*it, thread_id, hwnd_mgr)) {
            continue;
        }
        if (class_filtered && (classify_qs_bits(*it) & qs_mask) == 0) continue;
        msg = *it;
        if (remove) {
            m_queue.erase(it);
        }
        // Learn GetQueueStatus: Get/PeekMessage clear the QS_POSTMESSAGE
        // change bit; an unfiltered call also clears QS_ALLPOSTMESSAGE.
        u32& changed = m_changed_flags[thread_id];
        changed &= ~QS::POSTMESSAGE;
        if (filter_min == 0 && filter_max == 0) changed &= ~QS::ALLPOSTMESSAGE;
        return true;
    }

    // Learn: Get/PeekMessage always retrieve WM_QUIT, whatever the filter.
    if (quit != m_quit_codes.end() && quit_allowed) {
        take_quit();
        return true;
    }

    // Learn MsgWait remarks: any Peek/Get that inspects the queue marks
    // existing unread input as old, even when the filter matches nothing
    // (user32_windows_probe Peek(WM_USER+100) then MsgWait without
    // MWMO_INPUTAVAILABLE times out while WM_USER+7 is still queued).
    u32 present = m_sent.contains(thread_id) ? QS::SENDMESSAGE : 0;
    for (const MSG& queued : m_queue) {
        if (belongs_to_thread(queued, thread_id, hwnd_mgr)) {
            present |= classify_qs_bits(queued);
        }
    }
    m_changed_flags[thread_id] &= ~present;
    return false;
}

bool MessageQueue::peek(MSG& msg, HWND filter_hwnd, u32 filter_min,
                        u32 filter_max, u32 remove_flags) {
    std::lock_guard<std::mutex> lock(m_mutex);
    // Search for a matching message in the queue
    for (auto it = m_queue.begin(); it != m_queue.end(); ++it) {
        if (matches_filter(*it, filter_hwnd, filter_min, filter_max)) {
            msg = *it;
            if (remove_flags & PM::REMOVE) {
                m_queue.erase(it);
            }
            // PeekMessage clears change tracking like GetMessage (Learn).
            m_changed_flags[m_primary_thread_id] &= ~QS::POSTMESSAGE;
            if (filter_min == 0 && filter_max == 0) {
                m_changed_flags[m_primary_thread_id] &= ~QS::ALLPOSTMESSAGE;
            }
            return true;
        }
    }

    // If quit posted and no other messages match, synthesize WM_QUIT
    auto quit = m_quit_codes.find(m_primary_thread_id);
    if (quit != m_quit_codes.end()) {
        msg = {};
        msg.message = WM::QUIT;
        msg.wParam = static_cast<u64>(quit->second);
        msg.target_thread_id = m_primary_thread_id;
        if (remove_flags & PM::REMOVE) {
            m_quit_codes.erase(quit);
        }
        return true;
    }

    // Learn MsgWaitRemarks: any Peek/Get that inspects the queue marks
    // existing unread input as old, even when the filter matches nothing
    // (user32_windows_probe Peek(WM_USER+100) then MsgWait without
    // MWMO_INPUTAVAILABLE times out while WM_USER+7 is still queued).
    u32 present = 0;
    for (const MSG& queued : m_queue) {
        present |= classify_qs_bits(queued);
    }
    if (m_quit_codes.contains(m_primary_thread_id)) {
        present |= QS::POSTMESSAGE | QS::ALLPOSTMESSAGE;
    }
    m_changed_flags[m_primary_thread_id] &= ~present;
    return false;
}

bool MessageQueue::peek_for_thread(MSG& msg, u32 thread_id,
                                   const HwndManager& hwnd_mgr,
                                   HWND filter_hwnd, u32 filter_min,
                                   u32 filter_max, u32 remove_flags,
                                   bool* sent_dispatched) {
    return retrieve_for_thread(msg, thread_id, hwnd_mgr, filter_hwnd,
                               filter_min, filter_max,
                               (remove_flags & PM::REMOVE) != 0,
                               remove_flags >> PM::QS_SHIFT, sent_dispatched);
}

bool MessageQueue::quit_posted(u32 thread_id) const {
    if (thread_id == 0) thread_id = m_primary_thread_id;
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_quit_codes.contains(thread_id);
}

int MessageQueue::quit_exit_code(u32 thread_id) const {
    if (thread_id == 0) thread_id = m_primary_thread_id;
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_last_quit_codes.find(thread_id);
    return it != m_last_quit_codes.end() ? it->second : 0;
}

bool MessageQueue::empty() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_queue.empty();
}

usize MessageQueue::size() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_queue.size();
}

void MessageQueue::clear() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_queue.clear();
    // Don't reset quit flag
}

void MessageQueue::remove_by_type(HWND hwnd, u32 message_type) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_queue.begin();
    while (it != m_queue.end()) {
        if (it->hwnd == hwnd && it->message == message_type) {
            it = m_queue.erase(it);
        } else {
            ++it;
        }
    }
}

bool MessageQueue::remove_exact(HWND hwnd, u32 message_type, u64 lparam) {
    std::lock_guard<std::mutex> lock(m_mutex);
    for (auto it = m_queue.begin(); it != m_queue.end(); ++it) {
        if (it->hwnd == hwnd && it->message == message_type &&
            it->lParam == lparam) {
            m_queue.erase(it);
            return true;
        }
    }
    return false;
}

bool MessageQueue::hwnd_filter_selects(const HwndManager& hwnd_mgr, HWND hwnd,
                                       HWND filter) noexcept {
    if (hwnd == filter) return true;
    HWND cur = hwnd;
    for (u32 guard = 0; guard < 256 && cur != NULL_HWND; ++guard) {
        const WindowData* win = hwnd_mgr.find(cur);
        if (win == nullptr || (win->style & WS::CHILD) == 0) return false;
        cur = win->parent;
        if (cur == filter) return true;
    }
    return false;
}

bool MessageQueue::filter_matches(HWND msg_hwnd, u32 message,
                                  HWND filter_hwnd, u32 filter_min,
                                  u32 filter_max,
                                  const HwndManager* hwnd_mgr) noexcept {
    // (HWND)-1: only messages whose hwnd is NULL (PostThreadMessage and
    // PostMessage(NULL)); s2msg_queue_probe filter_hwnd_minus1*.
    if (filter_hwnd == static_cast<HWND>(-1)) {
        if (msg_hwnd != NULL_HWND) return false;
    } else if (filter_hwnd != NULL_HWND && msg_hwnd != filter_hwnd &&
               (hwnd_mgr == nullptr ||
                !hwnd_filter_selects(*hwnd_mgr, msg_hwnd, filter_hwnd))) {
        return false;
    }
    if (filter_min == 0 && filter_max == 0) return true;
    // An inverted range matches no posted message (inverted_range_1..3).
    return message >= filter_min && message <= filter_max;
}

bool MessageQueue::matches_filter(const MSG& msg, HWND filter_hwnd,
                                  u32 filter_min, u32 filter_max) const {
    return filter_matches(msg.hwnd, msg.message, filter_hwnd, filter_min,
                          filter_max);
}

bool MessageQueue::wait_for_post(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(m_mutex);
    return m_cv.wait_for(lock, timeout, [this] {
        return m_shutdown.load(std::memory_order_acquire) ||
               !m_queue.empty() || !m_quit_codes.empty() || !m_sent.empty();
    });
}

bool MessageQueue::wait_for_input_for_thread(
    std::chrono::milliseconds timeout,
    u32 thread_id,
    const HwndManager& hwnd_mgr,
    u32 wake_mask
) {
    std::unique_lock<std::mutex> lock(m_mutex);
    return m_cv.wait_for(lock, timeout, [this, thread_id, &hwnd_mgr, wake_mask] {
        if (m_shutdown.load(std::memory_order_acquire)) return true;
        if ((wake_mask & QS::SENDMESSAGE) && m_sent.contains(thread_id)) return true;
        if ((wake_mask & (QS::POSTMESSAGE | QS::ALLPOSTMESSAGE)) != 0 &&
            m_quit_codes.contains(thread_id)) {
            return true;
        }
        for (const MSG& msg : m_queue) {
            if (belongs_to_thread(msg, thread_id, hwnd_mgr) &&
                (classify_qs_bits(msg) & wake_mask) != 0) {
                return true;
            }
        }
        return false;
    });
}

bool MessageQueue::has_input_for_thread(u32 thread_id,
                                        const HwndManager& hwnd_mgr,
                                        u32 wake_mask) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    if ((wake_mask & QS::SENDMESSAGE) && m_sent.contains(thread_id)) return true;
    if ((wake_mask & (QS::POSTMESSAGE | QS::ALLPOSTMESSAGE)) != 0 &&
        m_quit_codes.contains(thread_id)) {
        return true;
    }
    for (const MSG& msg : m_queue) {
        if (belongs_to_thread(msg, thread_id, hwnd_mgr) &&
            (classify_qs_bits(msg) & wake_mask) != 0) {
            return true;
        }
    }
    return false;
}

bool MessageQueue::has_new_input_for_thread(u32 thread_id,
                                            const HwndManager& hwnd_mgr,
                                            u32 wake_mask) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto changed_it = m_changed_flags.find(thread_id);
    const u32 changed = changed_it != m_changed_flags.end() ? changed_it->second : 0u;
    if ((changed & wake_mask) == 0) return false;
    if ((wake_mask & QS::SENDMESSAGE) && m_sent.contains(thread_id) &&
        (changed & QS::SENDMESSAGE) != 0) {
        return true;
    }
    if ((wake_mask & (QS::POSTMESSAGE | QS::ALLPOSTMESSAGE)) != 0 &&
        m_quit_codes.contains(thread_id) &&
        (changed & (QS::POSTMESSAGE | QS::ALLPOSTMESSAGE)) != 0) {
        return true;
    }
    for (const MSG& msg : m_queue) {
        const u32 bits = classify_qs_bits(msg);
        if (belongs_to_thread(msg, thread_id, hwnd_mgr) &&
            (bits & wake_mask) != 0 && (bits & changed) != 0) {
            return true;
        }
    }
    return false;
}

u32 MessageQueue::get_queue_status(u32 flags, u32 thread_id,
                                   const HwndManager& hwnd_mgr,
                                   u32 synthesized_present) {
    std::lock_guard<std::mutex> lock(m_mutex);
    u32 present = (m_sent.contains(thread_id) ? QS::SENDMESSAGE : 0) |
                  synthesized_present;
    for (const MSG& msg : m_queue) {
        if (belongs_to_thread(msg, thread_id, hwnd_mgr)) {
            present |= classify_qs_bits(msg);
        }
    }
    // A pending WM_QUIT is present but never "new" (s2msg_queue_probe
    // quit_status: 0x01080000).
    if (m_quit_codes.contains(thread_id)) {
        present |= QS::POSTMESSAGE | QS::ALLPOSTMESSAGE;
    }

    u32& changed = m_changed_flags[thread_id];
    const u32 present_masked = present & flags;
    const u32 changed_masked = (changed & present) & flags;
    // Consume the reported change bits (Learn: low word is since last call).
    // Only the asked-for classes: GetQueueStatus(QS_TIMER) leaves a new post
    // reported by the next GetQueueStatus(QS_ALLINPUT)
    // (status_post_after_mask_timer).
    changed &= ~flags;
    return (present_masked << 16) | changed_masked;
}

void MessageQueue::begin_shutdown() noexcept {
    {
        std::lock_guard lock(m_mutex);
        m_shutdown.store(true, std::memory_order_release);
        m_sent.clear();
    }
    wake_waiters();
}

void MessageQueue::dump_queue(const char* tag, usize max_messages) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::fprintf(stderr, "[msgqueue-dump] %s size=%zu\n",
                 tag ? tag : "?", m_queue.size());
    usize n = 0;
    for (const MSG& m : m_queue) {
        if (n >= max_messages) break;
        std::fprintf(stderr,
                     "[msgqueue-dump]   [%zu] hwnd=0x%x msg=0x%x "
                     "target_tid=%u wParam=0x%llx lParam=0x%llx\n",
                     n, m.hwnd, m.message, m.target_thread_id,
                     static_cast<unsigned long long>(m.wParam),
                     static_cast<unsigned long long>(m.lParam));
        ++n;
    }
    std::fflush(stderr);
}

usize MessageQueue::diag_head(const HwndManager& hwnd_mgr, u32& hwnd,
                              u32& message, u32& target_tid,
                              int& owner_tid) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    hwnd = 0; message = 0; target_tid = 0; owner_tid = -1;
    if (m_queue.empty()) return 0;
    const MSG& head = m_queue.front();
    hwnd = head.hwnd;
    message = head.message;
    target_tid = head.target_thread_id;
    if (head.hwnd != NULL_HWND) {
        const auto owner = hwnd_mgr.owner_thread_id(head.hwnd);
        owner_tid = owner.has_value() ? static_cast<int>(*owner) : -1;
    }
    return m_queue.size();
}

bool MessageQueue::belongs_to_thread(const MSG& msg, u32 thread_id,
                                     const HwndManager& hwnd_mgr) const {
    // Windows attributes a posted message to the thread that owned the HWND
    // (or the explicit PostThreadMessage target) at PostMessage time. Prefer
    // the recorded target so HWND reuse cannot re-home a thread message
    // posted at WM_USER (0x401) away from the posting thread while other
    // work for that thread is still queued.
    if (msg.target_thread_id != 0) {
        return msg.target_thread_id == thread_id;
    }
    if (msg.hwnd == NULL_HWND) {
        // Legacy producers that do not specify a target belong to the initial
        // GUI thread.
        return thread_id == m_primary_thread_id;
    }
    const auto owner = hwnd_mgr.owner_thread_id(msg.hwnd);
    return owner.has_value() && *owner == thread_id;
}

} // namespace vitreo::windowing
