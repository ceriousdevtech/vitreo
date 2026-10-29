// SPDX-License-Identifier: LGPL-2.1-only
/// @file gpu_event_signaler.cpp

#include "winapi/graphics/common/gpu_event_signaler.hpp"

#include "graphics/contracts/backend_device.hpp"

#include <chrono>

namespace vitreo::winapi {

GpuEventSignaler& GpuEventSignaler::instance() {
    static GpuEventSignaler signaler;
    return signaler;
}

GpuEventSignaler::~GpuEventSignaler() {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_stop = true;
    }
    m_wake.notify_all();
    if (m_thread.joinable()) m_thread.join();
}

void GpuEventSignaler::set_signal_function(SignalFn fn) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_signal = std::move(fn);
}

bool GpuEventSignaler::available() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return static_cast<bool>(m_signal);
}

void GpuEventSignaler::signal_now(u64 event_handle) {
    SignalFn signal;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        signal = m_signal;
    }
    if (signal && event_handle != 0) signal(event_handle);
}

void GpuEventSignaler::signal_when(const graphics::BackendFence* fence,
                                   u64 value, u64 event_handle) {
    if (fence == nullptr || event_handle == 0) return;
    if (fence->completed_value() >= value) {
        signal_now(event_handle);
        return;
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    m_pending.push_back({fence, value, nullptr, event_handle});
    ensure_thread_locked();
    m_wake.notify_all();
}

bool GpuEventSignaler::Request::fences_done() const {
    usize reached = 0;
    for (const auto& [fence, value] : fences) {
        if (fence->completed_value() >= value) ++reached;
    }
    return all ? reached == fences.size() : reached != 0;
}

void GpuEventSignaler::signal_when_fences(
    std::vector<std::pair<const graphics::BackendFence*, u64>> fences,
    bool all, u64 event_handle) {
    if (event_handle == 0 || fences.empty()) return;
    Request request{};
    request.event_handle = event_handle;
    request.fences = std::move(fences);
    request.all = all;
    if (request.fences_done()) {
        signal_now(event_handle);
        return;
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    m_pending.push_back(std::move(request));
    ensure_thread_locked();
    m_wake.notify_all();
}

void GpuEventSignaler::signal_after(
    std::shared_ptr<graphics::GpuCompletion> completion, u64 event_handle) {
    if (event_handle == 0) return;
    if (!completion) {
        signal_now(event_handle);
        return;
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    m_pending.push_back({nullptr, 0, std::move(completion), event_handle});
    ensure_thread_locked();
    m_wake.notify_all();
}

void GpuEventSignaler::run_after(
    std::shared_ptr<graphics::GpuCompletion> completion,
    std::function<void()> action) {
    if (!action) return;
    if (!completion) {
        action();
        return;
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    m_pending.push_back(
        {nullptr, 0, std::move(completion), 0, std::move(action)});
    ensure_thread_locked();
    m_wake.notify_all();
}

void GpuEventSignaler::cancel(const graphics::BackendFence* fence) {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::erase_if(m_pending, [fence](const Request& r) {
        if (r.fence == fence) return true;
        for (const auto& entry : r.fences) {
            if (entry.first == fence) return true;
        }
        return false;
    });
}

void GpuEventSignaler::rebind(const graphics::BackendFence* from,
                              const graphics::BackendFence* to) {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (Request& r : m_pending) {
            if (r.fence == from) r.fence = to;
            for (auto& entry : r.fences) {
                if (entry.first == from) entry.first = to;
            }
        }
    }
    m_wake.notify_all();
}

usize GpuEventSignaler::pending() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_pending.size();
}

void GpuEventSignaler::ensure_thread_locked() {
    if (m_thread.joinable()) return;
    m_thread = std::thread([this] { run(); });
}

void GpuEventSignaler::run() {
    std::unique_lock<std::mutex> lock(m_mutex);
    while (!m_stop) {
        if (m_pending.empty()) {
            m_wake.wait(lock, [this] { return m_stop || !m_pending.empty(); });
            continue;
        }
        // Collect what is ready without holding the lock across a wait.
        std::vector<u64> ready;
        std::vector<std::function<void()>> actions;
        std::vector<std::shared_ptr<graphics::GpuCompletion>> completions;
        for (auto it = m_pending.begin(); it != m_pending.end();) {
            if (!it->fences.empty()) {
                if (it->fences_done()) {
                    ready.push_back(it->event_handle);
                    it = m_pending.erase(it);
                    continue;
                }
                ++it;
                continue;
            }
            if (it->fence != nullptr) {
                if (it->fence->completed_value() >= it->value) {
                    ready.push_back(it->event_handle);
                    it = m_pending.erase(it);
                    continue;
                }
                ++it;
                continue;
            }
            // A submission completion: wait for it outside the lock below.
            completions.push_back(it->completion);
            if (it->action) {
                actions.push_back(std::move(it->action));
            } else {
                ready.push_back(it->event_handle);
            }
            it = m_pending.erase(it);
        }
        SignalFn signal = m_signal;
        lock.unlock();
        for (auto& completion : completions) (void)completion->wait();
        if (signal) {
            for (const u64 handle : ready) signal(handle);
        }
        for (auto& action : actions) action();
        lock.lock();
        if (!m_pending.empty() && !m_stop) {
            // Fence timelines have no completion callback in the backend
            // contract; poll briefly while something is outstanding.
            m_wake.wait_for(lock, std::chrono::microseconds(250));
        }
    }
}

}  // namespace vitreo::winapi
