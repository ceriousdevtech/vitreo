// SPDX-License-Identifier: LGPL-2.1-only
/// @file gpu_event_signaler.hpp
/// @brief Signal a guest event when GPU work completes.
///
/// ID3D11Fence / ID3D12Fence::SetEventOnCompletion and
/// ID3D11DeviceContext3::Flush1 take a Win32 event handle that the runtime must
/// set once the GPU reaches a point. The graphics backends report completion
/// (a fence's completed value, a submission's GpuCompletion) but know nothing
/// of guest handles, and the frontends know nothing of the kernel object
/// manager. This is the bridge: the process wires one function that sets a
/// guest event, and the frontends ask for it to be called at the right time.
///
/// Completion is observed by one background thread, started on first use,
/// that sleeps while nothing is pending. Without a wired function the
/// frontends must keep refusing -- an S_OK on an event that never fires hangs
/// the guest with no diagnostic.

#pragma once

#include <vitreo/common.hpp>

#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace vitreo::graphics {
class BackendFence;
class GpuCompletion;
}  // namespace vitreo::graphics

namespace vitreo::winapi {

class GpuEventSignaler {
public:
    using SignalFn = std::function<void(u64 event_handle)>;

    static GpuEventSignaler& instance();

    /// Wired once by the process (sets a guest event object).
    void set_signal_function(SignalFn fn);
    [[nodiscard]] bool available() const;

    /// Set @p event now.
    void signal_now(u64 event_handle);
    /// Set @p event once @p fence's completed value reaches @p value. The
    /// fence must outlive the request or be passed to cancel() first.
    void signal_when(const graphics::BackendFence* fence, u64 value,
                     u64 event_handle);
    /// Set @p event once all (@p all) or any of @p fences reach their value
    /// (ID3D12Device1::SetEventOnMultipleFenceCompletion). One set, however
    /// many fences complete. Each fence must outlive the request or be
    /// passed to cancel() first, which drops the whole request.
    void signal_when_fences(
        std::vector<std::pair<const graphics::BackendFence*, u64>> fences,
        bool all, u64 event_handle);
    /// Set @p event once @p completion finishes (null means: already done).
    void signal_after(std::shared_ptr<graphics::GpuCompletion> completion,
                      u64 event_handle);
    /// Run @p action on the signaler thread once @p completion finishes
    /// (null means: already done, so run it now). For the kernel objects
    /// that are not events, such as DXGI's frame-latency semaphore.
    void run_after(std::shared_ptr<graphics::GpuCompletion> completion,
                   std::function<void()> action);
    /// Drop every pending request on @p fence (it is being destroyed). Windows
    /// never sets those events either: the fence that would have is gone.
    void cancel(const graphics::BackendFence* fence);
    /// Move every pending request on @p from to @p to (a frontend replaced
    /// the fence's timeline: ID3D12Fence::Signal to a lower value).
    void rebind(const graphics::BackendFence* from,
                const graphics::BackendFence* to);

    /// Requests not yet satisfied; for tests.
    [[nodiscard]] usize pending() const;

    ~GpuEventSignaler();

private:
    GpuEventSignaler() = default;
    void ensure_thread_locked();
    void run();

    struct Request {
        const graphics::BackendFence* fence{nullptr};
        u64 value{0};
        std::shared_ptr<graphics::GpuCompletion> completion;
        u64 event_handle{0};
        std::function<void()> action;
        /// signal_when_fences: the fences and whether all must complete.
        std::vector<std::pair<const graphics::BackendFence*, u64>> fences;
        bool all{false};
        [[nodiscard]] bool fences_done() const;
    };

    mutable std::mutex m_mutex;
    std::condition_variable m_wake;
    SignalFn m_signal;
    std::vector<Request> m_pending;
    std::thread m_thread;
    bool m_stop{false};
};

}  // namespace vitreo::winapi
