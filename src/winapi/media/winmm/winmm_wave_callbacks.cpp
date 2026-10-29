/// @file winmm_wave_callbacks.cpp
/// @brief waveOut/waveIn notification machinery of WinmmManager.
///
/// WOM_*/WIM_* delivery: the synchronous open/close notifications, the
/// queue the service thread drains, and block completion. Split out of
/// winmm.cpp.

#include "winapi/media/winmm/winmm_internal.hpp"
#include <algorithm>
#include <mutex>
#include <thread>

namespace vitreo::winapi {

// ---------------------------------------------------------------------------
// waveOut notifications
// ---------------------------------------------------------------------------
void WinmmManager::notify_sync(const WaveStream& stream, u32 message) {
    notify_sync(WaveCallbackTarget{stream.handle, stream.callback, stream.callback_type,
                                   stream.instance, stream.is_64bit},
                message);
}

void WinmmManager::notify_sync(const WaveCallbackTarget& stream, u32 message) {
    WaveCallbackInvoker invoker;
    WaveWindowNotifier window;
    WaveThreadNotifier thread;
    WaveEventSignaler event;
    {
        std::lock_guard<std::mutex> guard(m_wave_mutex);
        invoker = m_wave_sync_invoker;
        window = m_wave_window_notifier;
        thread = m_wave_thread_notifier;
        event = m_wave_event_signaler;
    }
    switch (stream.callback_type) {
    case CALLBACK_FUNCTION:
        if (stream.callback != 0 && invoker) {
            invoker(stream.callback, stream.handle, message, stream.instance,
                    0, 0);
        }
        break;
    case CALLBACK_WINDOW:
        if (stream.callback != 0 && window) {
            (void)window(stream.callback, message, stream.handle, 0);
        }
        break;
    case CALLBACK_THREAD:
        // Thread id 0 names no thread: PostThreadMessage cannot deliver it.
        if (thread && static_cast<u32>(stream.callback) != 0) {
            (void)thread(static_cast<u32>(stream.callback), message,
                         stream.handle, 0);
        }
        break;
    case CALLBACK_EVENT:
        if (event) (void)event(stream.callback);
        break;
    default:
        // Unknown callback kinds are accepted by waveOutOpen and never
        // notified (s7_winmm_probe o_cbtype t=40000/60000/70000 r=0).
        break;
    }
}

u64 WinmmManager::enqueue_wave_event_locked(const WaveStream& stream,
                                            u32 message, u64 param1,
                                            bool complete_header) {
    return enqueue_wave_event_locked(
        WaveCallbackTarget{stream.handle, stream.callback, stream.callback_type,
                           stream.instance, stream.is_64bit},
        message, param1, complete_header);
}

u64 WinmmManager::enqueue_wave_event_locked(const WaveCallbackTarget& stream,
                                            u32 message, u64 param1,
                                            bool complete_header) {
    WaveCallbackEvent event;
    event.stream_handle = stream.handle;
    event.callback = stream.callback;
    event.callback_type = stream.callback_type;
    event.instance = stream.instance;
    event.message = message;
    event.param1 = param1;
    event.complete_header = complete_header;
    event.is_64bit = stream.is_64bit;
    event.sequence = ++m_wave_event_sequence;
    m_wave_events.push_back(event);
    if (!m_wave_thread.joinable() && !m_wave_stopping) {
        m_wave_thread = std::thread([this] { wave_callback_loop(); });
    }
    m_wave_wake.notify_one();
    return event.sequence;
}

void WinmmManager::wait_wave_events(u64 sequence) {
    std::unique_lock<std::mutex> lock(m_wave_mutex);
    if (m_wave_thread.joinable() &&
        m_wave_thread.get_id() == std::this_thread::get_id()) {
        return; // a waveOutProc calling back in; it cannot wait on itself
    }
    m_wave_delivered.wait(lock, [&] {
        return m_wave_delivered_sequence >= sequence || m_wave_stopping;
    });
}

void WinmmManager::deliver_wave_event(const WaveCallbackEvent& event) {
    // WHDR_DONE is set and WHDR_INQUEUE cleared before the notification
    // (s7_winmm_probe fn_done_cbs: f=3 inside the callback).
    if (event.complete_header && event.param1 != 0) {
        const WaveHdrLayout& layout = wavehdr_layout(event.is_64bit);
        const GuestAddr flags_addr = event.param1 + layout.flags_offset;
        const u32 flags = m_memory.read_u32(flags_addr).value_or(0);
        (void)m_memory.write_u32(flags_addr,
                                 (flags & ~WHDR_INQUEUE) | WHDR_DONE);
    }

    WaveCallbackInvoker invoker;
    WaveWindowNotifier window;
    WaveThreadNotifier thread;
    WaveEventSignaler signal;
    {
        std::lock_guard<std::mutex> guard(m_wave_mutex);
        invoker = m_wave_invoker;
        window = m_wave_window_notifier;
        thread = m_wave_thread_notifier;
        signal = m_wave_event_signaler;
    }
    switch (event.callback_type) {
    case CALLBACK_FUNCTION:
        if (event.callback == 0) break;
        VITREO_TRACE("[winmm] wave callback proc=0x%llx msg=0x%x "
                     "instance=0x%llx param1=0x%llx\n",
                     static_cast<unsigned long long>(event.callback),
                     event.message,
                     static_cast<unsigned long long>(event.instance),
                     static_cast<unsigned long long>(event.param1));
        if (invoker) {
            invoker(event.callback, event.stream_handle, event.message,
                    event.instance, event.param1, event.param2);
        } else {
            VITREO_TRACE("[winmm] wave callback dropped: no guest invoker\n");
        }
        break;
    case CALLBACK_WINDOW:
        if (event.callback == 0) break;
        winmm_diag("window hwnd=0x%llx msg=0x%x hwo=0x%llx header=0x%llx\n",
                   static_cast<unsigned long long>(event.callback),
                   event.message,
                   static_cast<unsigned long long>(event.stream_handle),
                   static_cast<unsigned long long>(event.param1));
        if (!window || !window(event.callback, event.message,
                               event.stream_handle, event.param1)) {
            VITREO_TRACE("[winmm] wave window callback dropped: invalid HWND "
                         "or no notifier wired\n");
        }
        break;
    case CALLBACK_THREAD:
        if (thread && static_cast<u32>(event.callback) != 0) {
            (void)thread(static_cast<u32>(event.callback), event.message,
                         event.stream_handle, event.param1);
        }
        break;
    case CALLBACK_EVENT:
        if (signal) (void)signal(event.callback);
        break;
    default:
        break;
    }
}

void WinmmManager::wave_callback_loop() {
    for (;;) {
        WaveCallbackEvent event;
        {
            std::unique_lock<std::mutex> lock(m_wave_mutex);
            m_wave_wake.wait(lock, [this] {
                return m_wave_stopping || !m_wave_events.empty();
            });
            if (m_wave_events.empty() && m_wave_stopping) return;
            event = m_wave_events.front();
            m_wave_events.pop_front();
        }
        deliver_wave_event(event);
        {
            std::lock_guard<std::mutex> guard(m_wave_mutex);
            m_wave_delivered_sequence = event.sequence;
        }
        m_wave_delivered.notify_all();
    }
}

void WinmmManager::stop_wave_callback_thread() {
    std::thread thread;
    {
        std::lock_guard<std::mutex> guard(m_wave_mutex);
        m_wave_stopping = true;
        thread = std::move(m_wave_thread);
    }
    m_wave_wake.notify_all();
    m_wave_delivered.notify_all();
    if (thread.joinable()) thread.join();
}

void WinmmManager::complete_wave_block(u64 stream_handle, u64 token) {
    std::lock_guard<std::mutex> guard(m_wave_mutex);
    const auto it = m_wave_streams.find(stream_handle);
    if (it == m_wave_streams.end()) return;
    WaveStream& stream = *it->second;
    const auto match = std::find_if(
        stream.pending.begin(), stream.pending.end(),
        [token](const WaveStream::Pending& p) { return p.token == token; });
    if (token == 0 || match == stream.pending.end()) return; // reset raced
    // The backend completes in submission order: everything up to this
    // block is done, and so is any empty buffer queued right behind it.
    auto last = std::next(match);
    while (last != stream.pending.end() && last->token == 0) ++last;
    for (auto p = stream.pending.begin(); p != last; ++p) {
        stream.bytes_completed += p->bytes;
        (void)enqueue_wave_event_locked(stream, WOM_DONE, p->header, true);
    }
    stream.pending.erase(stream.pending.begin(), last);
}

} // namespace vitreo::winapi
