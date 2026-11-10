// SPDX-License-Identifier: LGPL-2.1-only

/// @file d3d12_queue.cpp
/// @brief ID3D12CommandQueue and ID3D12Fence: submission, queue-ordered
/// signals and waits, CPU signals and completion events.
///
/// PLAN decision 7: every queue shares the backend's one command stream, and
/// ExecuteCommandLists replays the lists into it in submission order. One
/// stream makes a cross-queue wait a hazard D3D12 does not have: recording a
/// GPU wait for a value whose signal has not been submitted yet would park the
/// stream in front of the very signal that releases it. So a queue that waits
/// on a value no signal has been submitted for holds everything submitted to
/// it after the wait (CommandQueue::pending) until one is -- which is exactly
/// the D3D12 ordering: the queue's later work may not start before the wait
/// is satisfied, and other queues are unaffected.

#include "winapi/graphics/d3d12/d3d12_internal.hpp"
#include "winapi/graphics/d3d12/d3d12_layout.hpp"
#include "winapi/graphics/common/gpu_event_signaler.hpp"
#include "core/memory/guest_memory.hpp"
#include "vitreo/virtual_tsc.hpp"
#include <vitreo/ab_cycle.hpp>

#include <algorithm>
#include <cstdlib>

#include <mach/mach_time.h>

#include <chrono>
#include <thread>

namespace vitreo::winapi::d3d12 {

// =============================================================================
// Creation
// =============================================================================

void Frontend::create_command_queue(Marshaler& m, Device& device,
                                    bool queue1) {
    // CreateCommandQueue(const D3D12_COMMAND_QUEUE_DESC *, REFIID, void **).
    // ID3D12Device9::CreateCommandQueue1 inserts REFIID CreatorID after the
    // description: an identity for tools, with no effect on the queue.
    const GuestAddr desc = m.arg_ptr(1);
    const usize riid_index = queue1 ? 3 : 2;
    const GuestAddr out = m.arg_ptr(riid_index + 1);
    if (out != 0) (void)write_pointer(out, 0);
    if (desc == 0) {
        m.set_return(E_INVALIDARG);
        return;
    }
    if (out == 0) {
        // Measured (a04 queue "ppQueue=NULL").
        m.set_return(E_UNEXPECTED);
        return;
    }
    const auto& l = layout::D3D12_COMMAND_QUEUE_DESC[m_is_64bit];
    auto queue = std::make_unique<CommandQueue>();
    queue->device = device.address;
    queue->type = read_u32(desc + l.Type);
    queue->priority = static_cast<i32>(read_u32(desc + l.Priority));
    queue->flags = read_u32(desc + l.Flags);
    queue->node_mask = read_u32(desc + l.NodeMask);
    // Learn (D3D12_COMMAND_LIST_TYPE): BUNDLE is not a queue type; the video
    // engines are not exposed here.
    if (queue->type >= D3D12_COMMAND_LIST_TYPE_VIDEO_DECODE &&
        queue->type <= D3D12_COMMAND_LIST_TYPE_VIDEO_ENCODE) {
        unimplemented("ID3D12Device::CreateCommandQueue", "video queues");
        m.set_return(E_INVALIDARG);
        return;
    }
    if ((queue->type != D3D12_COMMAND_LIST_TYPE_DIRECT &&
         queue->type != D3D12_COMMAND_LIST_TYPE_COMPUTE &&
         queue->type != D3D12_COMMAND_LIST_TYPE_COPY) ||
        (queue->flags & ~D3D12_COMMAND_QUEUE_FLAG_DISABLE_GPU_TIMEOUT) != 0 ||
        queue->node_mask > 1) {
        m.set_return(E_INVALIDARG);
        return;
    }
    if (queue->priority == D3D12_COMMAND_QUEUE_PRIORITY_GLOBAL_REALTIME) {
        // Learn: needs a privilege this process does not have; measured
        // (a04 queue): DXGI_ERROR_UNSUPPORTED.
        m.set_return(DXGI_ERROR_UNSUPPORTED);
        return;
    }
    if (queue->priority != D3D12_COMMAND_QUEUE_PRIORITY_NORMAL &&
        queue->priority != D3D12_COMMAND_QUEUE_PRIORITY_HIGH) {
        m.set_return(E_INVALIDARG);
        return;
    }
    // GetDesc reports a NodeMask of 0 as 1 (measured, a04 queue).
    if (queue->node_mask == 0) queue->node_mask = 1;
    if (!m_backend.supports_fences()) {
        m.set_return(E_FAIL);
        return;
    }
    m.set_return(
        return_new_object(std::move(queue), m.arg_ptr(riid_index), out));
}

void Frontend::create_fence(Marshaler& m, Device& device) {
    // CreateFence(UINT64 InitialValue, D3D12_FENCE_FLAGS Flags, REFIID riid,
    //     void **ppFence). InitialValue is two argument slots on x86.
    usize next = 0;
    const u64 initial = read_arg_u64(m, m_is_64bit, 1, next);
    const u32 flags = m.arg32(next);
    const GuestAddr iid = m.arg_ptr(next + 1);
    const GuestAddr out = m.arg_ptr(next + 2);
    if (out != 0) (void)write_pointer(out, 0);
    // Measured (a04 fence): SHARED and SHARED|SHARED_CROSS_ADAPTER create
    // (a shared fence is an ordinary one until CreateSharedHandle, which
    // fails loudly); NON_MONITORED is E_INVALIDARG, as is any other bit.
    if (out == 0 || (flags & ~(D3D12_FENCE_FLAG_SHARED |
                               D3D12_FENCE_FLAG_SHARED_CROSS_ADAPTER)) != 0 ||
        ((flags & D3D12_FENCE_FLAG_SHARED_CROSS_ADAPTER) != 0 &&
         (flags & D3D12_FENCE_FLAG_SHARED) == 0)) {
        m.set_return(E_INVALIDARG);
        return;
    }
    if (!m_backend.supports_fences()) {
        m.set_return(E_FAIL);
        return;
    }
    auto fence = std::make_unique<Fence>();
    fence->device = device.address;
    fence->flags = flags;
    fence->timeline->backend = m_backend.create_fence(initial);
    if (!fence->timeline->backend || !fence->timeline->backend->valid()) {
        m.set_return(E_OUTOFMEMORY);
        return;
    }
    fence->timeline->submitted = initial;
    m.set_return(return_new_object(std::move(fence), iid, out));
}

// =============================================================================
// Submission
// =============================================================================

FenceTimeline::~FenceTimeline() {
    if (backend) GpuEventSignaler::instance().cancel(backend.get());
}

bool Frontend::queue_signal(Fence& fence, u64 value) {
    if (value < std::max(fence.timeline->submitted, fence.timeline->backend->completed_value())) {
        // A queue signal lowers the fence once the queue's earlier work has
        // run (measured, a04 "fence queue Signal(45) (lower)": completed=45).
        // The host timeline cannot run backwards on the GPU, so the queue's
        // work is completed first and the fence set from the CPU.
        graphics::frame_profile::WaitScope profile_wait("queue:signal-lowers-fence");
        if (auto completion = m_backend.flush_completion()) {
            (void)completion->wait();
        } else {
            (void)m_backend.wait_idle();
        }
        return cpu_signal(fence, value);
    }
    if (!fence.timeline->backend->signal(value)) return false;
    fence.timeline->submitted = std::max(fence.timeline->submitted, value);
    return true;
}

bool Frontend::cpu_signal(Fence& fence, u64 value) {
    // A shared GetCompletedValue and device removal may observe the
    // timeline while a queue lowers it. Publish the replacement under the
    // same lock both readers use (in addition to replay/exclusive ownership).
    std::lock_guard<std::mutex> timeline(m_remove_mutex);
    const u64 completed = fence.timeline->backend->completed_value();
    if (value >= completed) {
        if (!fence.timeline->backend->signal_cpu(value)) return false;
        fence.timeline->submitted = std::max(fence.timeline->submitted, value);
        return true;
    }
    // Lower: a new timeline at @p value replaces the old one (the backend's
    // never runs backwards). Events waiting for a value are moved to it; a
    // signal already queued on the GPU for a higher value still raises the
    // fence when it runs, as it does on Windows, bridged from the old
    // timeline once the queued work completes.
    std::shared_ptr<graphics::BackendFence> next = m_backend.create_fence(value);
    if (!next || !next->valid()) return false;
    std::shared_ptr<graphics::BackendFence> old = fence.timeline->backend;
    GpuEventSignaler::instance().rebind(old.get(), next.get());
    const u64 in_flight = fence.timeline->submitted > completed ? fence.timeline->submitted : 0;
    fence.timeline->retired.push_back(old);
    fence.timeline->backend = next;
    // A thread blocked on the old timeline re-reads the fence (see
    // SetEventOnCompletion with a NULL event).
    fence.timeline->generation.fetch_add(1, std::memory_order_release);
    fence.timeline->submitted = value;
    if (in_flight != 0) {
        fence.timeline->submitted = in_flight;
        std::weak_ptr<graphics::BackendFence> target = next;
        GpuEventSignaler::instance().run_after(
            m_backend.flush_completion(), [target, in_flight] {
                if (auto live = target.lock();
                    live && live->completed_value() < in_flight) {
                    (void)live->signal_cpu(in_flight);
                }
            });
    }
    return true;
}

void Frontend::execute(
    CommandQueue& queue,
    const std::vector<std::shared_ptr<const Recording>>& lists) {
    namespace fp = graphics::frame_profile;
    const bool profile = fp::enabled();
    const u64 start = profile ? fp::now_ns() : 0;
    // Direct replay consumes resolved native draws now, while the frontend's
    // replay ownership and immutable recording references are still held.
    // Opt-in until the same-workload performance gate has passed.
    static const bool direct = [] {
        const char* value = std::getenv("VITREO_D3D12_DIRECT");
        return value != nullptr && value[0] == '1';
    }();
    const bool direct_this_batch = direct &&
        !diagnostics::ab_cycle_off("d3d12-direct");
    auto immediate = direct_this_batch ? m_backend.begin_immediate_execution() : nullptr;
    for (const auto& recording : lists) replay_list(queue, *recording);
    // Learn: a suspending/resuming pair is one ExecuteCommandLists call.
    // A suspend this call did not resume does not carry into the next.
    end_execute_batch();
    if (!profile) {
        if (!immediate) m_backend.flush();
        return;
    }
    const u64 replayed = fp::now_ns();
    if (!immediate) m_backend.flush();
    fp::add(fp::Time::Replay, replayed - start);
    fp::add(fp::Time::BackendFlush, fp::now_ns() - replayed);
    fp::add(fp::Counter::Executes);
    fp::add(fp::Counter::ListsReplayed, lists.size());
}

void Frontend::pump_queue(CommandQueue& queue) {
    while (!queue.pending.empty()) {
        CommandQueue::Pending& op = queue.pending.front();
        Fence* fence = op.fence != 0 ? find_serial<Fence>(op.fence,
                                                          op.fence_serial,
                                                          Kind::Fence)
                                     : nullptr;
        switch (op.op) {
        case CommandQueue::Pending::Op::Wait:
            if (fence == nullptr) break;  // released: nothing to wait for
            if (fence->timeline->submitted < op.value &&
                fence->timeline->backend->completed_value() < op.value) {
                return;  // still blocked
            }
            (void)fence->timeline->backend->wait_gpu(op.value);
            break;
        case CommandQueue::Pending::Op::Signal:
            if (fence != nullptr) (void)queue_signal(*fence, op.value);
            m_backend.flush();
            break;
        case CommandQueue::Pending::Op::Execute:
            execute(queue, op.recordings);
            break;
        case CommandQueue::Pending::Op::TileMappings:
            if (op.tile_mappings) op.tile_mappings();
            m_exclusive_queue_ops.fetch_sub(1);
            break;
        case CommandQueue::Pending::Op::Present:
            // IDXGISwapChain::Present held behind a queue wait: shown now,
            // after the work queued before it (a released chain shows
            // nothing).
            m_exclusive_queue_ops.fetch_sub(1);
            if (SwapChain* chain = find_serial<SwapChain>(
                    op.chain, op.chain_serial, Kind::SwapChain)) {
                show(*chain, op.buffer, op.buffer_serial, op.sync_interval,
                     op.present_flags);
            }
            break;
        }
        queue.pending.pop_front();
    }
}

void Frontend::pump_all_queues() {
    // A signal may release several queues, and a released queue's own
    // signals may release more; repeat until nothing moves.
    bool progress = true;
    while (progress) {
        progress = false;
        // A queued Present may release frontend ownership. Snapshot
        // identities, not object-table iterators or queue pointers, across
        // callbacks; a newly published queue is visited on the next pass.
        const auto queues = m_command_queues;
        for (const auto& [address, serial] : queues) {
            auto* queue = find_serial<CommandQueue>(address, serial, Kind::CommandQueue);
            if (queue == nullptr) continue;
            const usize before = queue->pending.size();
            if (before == 0) continue;
            pump_queue(*queue);
            queue = find_serial<CommandQueue>(address, serial, Kind::CommandQueue);
            if (queue == nullptr || queue->pending.size() != before) progress = true;
        }
        if (queues != m_command_queues) progress = true;
    }
}

// =============================================================================
// ID3D12CommandQueue
// =============================================================================

void Frontend::command_queue_method(Marshaler& m, CommandQueue& queue,
                                    usize slot) {
    using S = CommandQueueSlot;
    switch (slot) {
    case S::UpdateTileMappings:
        queue_update_tile_mappings(m, queue);
        return;
    case S::CopyTileMappings:
        queue_copy_tile_mappings(m, queue);
        return;
    case S::ExecuteCommandLists: {
        // ExecuteCommandLists(UINT NumCommandLists,
        //     ID3D12CommandList *const *ppCommandLists). Void: Learn says
        // the runtime drops the call for a bundle, a list not closed, or one
        // whose Close failed. The whole array is validated first so a bad
        // entry drops the batch rather than half of it.
        const u32 count = m.arg32(1);
        const GuestAddr array = m.arg_ptr(2);
        m.set_return(0);
        if (count == 0) return;
        if (array == 0) return;
        // Under the shared frontend lock (d3d12_lock.hpp): recording and
        // descriptor writes go on beside this. The replay state, the queue's
        // pending work and the one backend stream are the replay lock's
        // (an exclusive holder excludes every replay without it).
        std::optional<ReplayLock> replaying;
        if (m_lock.mode() == FrontendRwLock::Mode::Shared) replaying.emplace(*this);
        const usize pointer_size = m_is_64bit ? 8u : 4u;
        std::vector<std::shared_ptr<const Recording>> recordings;
        for (u32 index = 0; index < count; ++index) {
            GuestAddr address = 0;
            if (!read_pointer(array + index * pointer_size, address)) return;
            CommandList* list = find<CommandList>(address, Kind::CommandList);
            if (list == nullptr ||
                list->type == D3D12_COMMAND_LIST_TYPE_BUNDLE ||
                !list->closed || list->record_error != 0 ||
                // Learn: the list's type must match the queue's.
                list->type != queue.type) {
                // Measured (a04 execute_*): an open list, one whose Close
                // failed, a bundle, or a list of another type than the queue
                // is an invalid call that removes the device.
                remove_device(DXGI_ERROR_INVALID_CALL,
                              "ExecuteCommandLists of a list that cannot run "
                              "on this queue");
                return;
            }
            recordings.push_back(list->recording);
        }
        if (!queue.pending.empty()) {
            CommandQueue::Pending op{};
            op.op = CommandQueue::Pending::Op::Execute;
            op.recordings = std::move(recordings);
            queue.pending.push_back(std::move(op));
            return;
        }
        execute(queue, recordings);
        return;
    }
    case S::SetMarker:
    case S::BeginEvent:
    case S::EndEvent:
        // Debug annotations the GPU never sees.
        m.set_return(0);
        return;
    case S::Signal:
    case S::Wait: {
        // Signal / Wait(ID3D12Fence *pFence, UINT64 Value).
        usize next = 0;
        const u64 value = read_arg_u64(m, m_is_64bit, 2, next);
        Fence* fence = find<Fence>(m.arg_ptr(1), Kind::Fence);
        if (fence == nullptr) {
            m.set_return(E_INVALIDARG);
            return;
        }
        // Under the shared frontend lock (method(): no Present or tile
        // mapping is queued anywhere), the queues' pending work, the fence
        // submission and the replays a signal releases are the replay
        // lock's. Under the exclusive lock no replay runs beside this, and
        // the replay lock must not be held: a queued Present may release
        // the frontend lock for a full-screen transition.
        std::optional<ReplayLock> replaying;
        if (m_lock.mode() == FrontendRwLock::Mode::Shared) replaying.emplace(*this);
        CommandQueue::Pending op{};
        op.op = slot == S::Signal ? CommandQueue::Pending::Op::Signal
                                  : CommandQueue::Pending::Op::Wait;
        op.fence = fence->address;
        op.fence_serial = fence->serial;
        op.value = value;
        queue.pending.push_back(op);
        pump_queue(queue);
        if (slot == S::Signal) pump_all_queues();
        m.set_return(S_OK);
        return;
    }
    case S::GetTimestampFrequency: {
        // GetTimestampFrequency(UINT64 *pFrequency): the rate of TIMESTAMP
        // query ticks -- the backend's GPU completion times in host-clock
        // nanoseconds (d3d12_replay.cpp query), on every queue type
        // (measured, a06 timestamp: COPY's equals DIRECT's).
        const GuestAddr out = m.arg_ptr(1);
        if (out == 0) {
            m.set_return(E_INVALIDARG);
            return;
        }
        (void)m_memory.write_u64(out, TIMESTAMP_FREQUENCY_HZ);
        m.set_return(S_OK);
        return;
    }
    case S::GetClockCalibration: {
        // GetClockCalibration(UINT64 *pGpuTimestamp, UINT64 *pCpuTimestamp):
        // the GPU tick count and QueryPerformanceCounter at one moment. The
        // GPU clock is the host time base Metal reports completion times in
        // (mach_absolute_time), the CPU one the guest's QPC.
        const GuestAddr gpu = m.arg_ptr(1);
        const GuestAddr cpu = m.arg_ptr(2);
        if (gpu == 0 || cpu == 0) {
            m.set_return(E_INVALIDARG);
            return;
        }
        (void)m_memory.write_u64(gpu, host_gpu_clock_ns());
        (void)m_memory.write_u64(cpu, virtual_qpc_ticks());
        m.set_return(S_OK);
        return;
    }
    case S::GetDesc: {
        // Hidden return pointer (_WIN32 C interface): This, RetVal.
        const GuestAddr out = m.arg_ptr(1);
        if (out != 0) {
            const auto& l = layout::D3D12_COMMAND_QUEUE_DESC[m_is_64bit];
            (void)m_memory.write_u32(out + l.Type, queue.type);
            (void)m_memory.write_u32(out + l.Priority,
                                     static_cast<u32>(queue.priority));
            (void)m_memory.write_u32(out + l.Flags, queue.flags);
            (void)m_memory.write_u32(out + l.NodeMask, queue.node_mask);
        }
        m.set_return(out);
        return;
    }
    default:
        unimplemented("ID3D12CommandQueue", "slot");
        m.set_return(E_NOTIMPL);
        return;
    }
}

// =============================================================================
// ID3D12Fence
// =============================================================================

void Frontend::fence_method(Marshaler& m, Fence& fence, usize slot) {
    using S = FenceSlot;
    switch (slot) {
    case S::GetCompletedValue: {
        // Polling must not drain recording or wait for ordinary replay.
        // The shared frontend hold protects object lifetime; this lock
        // protects timeline replacement and the removal transition.
        std::lock_guard<std::mutex> timeline(m_remove_mutex);
        if (const auto* device = find<Device>(fence.device, Kind::Device);
            device != nullptr && device->removed_reason != S_OK) {
            m.set_return_i64(~u64{0});
            return;
        }
        // UINT64 by value: RAX on x64, EDX:EAX on x86.
        m.set_return_i64(fence.timeline->backend->completed_value());
        return;
    }
    case S::SetEventOnCompletion: {
        // SetEventOnCompletion(UINT64 Value, HANDLE hEvent).
        usize next = 0;
        const u64 value = read_arg_u64(m, m_is_64bit, 1, next);
        const GuestAddr event = m.arg_ptr(next);
        if (event == 0) {
            // Learn: a null event blocks until the fence reaches the value.
            // Bounded, so a value that is never signalled leaves a
            // diagnostic instead of a silent hang; the frontend lock is
            // released so other threads can submit the signal.
            // A Signal that lowers the fence meanwhile replaces its timeline
            // (cpu_signal); the wait then continues on the new one, so it
            // ends when the FENCE reaches the value, as on Windows. The
            // waiter looks for a replacement every kSlice.
            constexpr u64 kSlice = 20ull * 1000000ull;
            const std::shared_ptr<FenceTimeline> timeline = fence.timeline;
            std::shared_ptr<graphics::BackendFence> backend = timeline->backend;
            u64 generation = timeline->generation.load(std::memory_order_acquire);
            const u64 serial = fence.serial;
            const GuestAddr address = fence.address;
            bool reached = false;
            {
                // Released only when this thread holds it once
                // (FrontendUnlock): a wait re-entered from a guest callback
                // keeps it, as before.
                FrontendUnlock unlocked(*this);
                graphics::frame_profile::WaitScope profile_wait(
                    "fence:SetEventOnCompletion(NULL)");
                for (u64 waited = 0; waited < FENCE_BLOCKING_WAIT_TIMEOUT_NS;) {
                    const u64 slice =
                        std::min(kSlice, FENCE_BLOCKING_WAIT_TIMEOUT_NS - waited);
                    reached = backend->wait_cpu(value, slice);
                    waited += slice;
                    const u64 now = timeline->generation.load(std::memory_order_acquire);
                    if (now == generation) {
                        if (reached) break;
                        continue;
                    }
                    // Replaced: whatever the old timeline says no longer counts.
                    FrontendLock relock(*this);
                    backend = timeline->backend;
                    generation = timeline->generation.load(std::memory_order_acquire);
                    reached = backend->completed_value() >= value;
                    if (reached) break;
                }
            }
            if (find_serial<Fence>(address, serial, Kind::Fence) == nullptr) {
                m.set_return(E_FAIL);
                return;
            }
            if (!reached) {
                d3d12_diag("ID3D12Fence::SetEventOnCompletion(%llu) timed "
                           "out at %llu",
                           static_cast<unsigned long long>(value),
                           static_cast<unsigned long long>(
                               backend->completed_value()));
            }
            m.set_return(reached ? S_OK : E_FAIL);
            return;
        }
        auto& signaler = GpuEventSignaler::instance();
        if (!signaler.available()) {
            unimplemented("ID3D12Fence::SetEventOnCompletion",
                          "event handles without the GPU event bridge");
            m.set_return(E_NOTIMPL);
            return;
        }
        signaler.signal_when(fence.timeline->backend.get(), value, event);
        m.set_return(S_OK);
        return;
    }
    case S::Signal: {
        // Signal(UINT64 Value): the CPU sets the fence now -- even to a lower
        // value (measured, d12core_facts_probe fence_after_lower) -- which
        // may release queues waiting on it.
        usize next = 0;
        const u64 value = read_arg_u64(m, m_is_64bit, 1, next);
        if (!cpu_signal(fence, value)) {
            m.set_return(E_OUTOFMEMORY);
            return;
        }
        pump_all_queues();
        m.set_return(S_OK);
        return;
    }
    case S::GetCreationFlags:
        m.set_return(fence.flags);
        return;
    default:
        unimplemented("ID3D12Fence", "slot");
        m.set_return(E_NOTIMPL);
        return;
    }
}

}  // namespace vitreo::winapi::d3d12

namespace vitreo::winapi::d3d12 {

u64 host_gpu_clock_ns() {
    static const mach_timebase_info_data_t timebase = [] {
        mach_timebase_info_data_t info{};
        (void)mach_timebase_info(&info);
        return info;
    }();
    const unsigned __int128 ns =
        static_cast<unsigned __int128>(mach_absolute_time()) * timebase.numer /
        timebase.denom;
    return static_cast<u64>(ns);
}

void Frontend::set_event_on_multiple_fences(Marshaler& m) {
    // ID3D12Device1::SetEventOnMultipleFenceCompletion(
    //     ID3D12Fence *const *ppFences, const UINT64 *pFenceValues,
    //     UINT NumFences, D3D12_MULTIPLE_FENCE_WAIT_FLAGS Flags,
    //     HANDLE hEvent).
    const GuestAddr fences_address = m.arg_ptr(1);
    const GuestAddr values_address = m.arg_ptr(2);
    const u32 count = m.arg32(3);
    const u32 flags = m.arg32(4);
    const GuestAddr event = m.arg_ptr(5);
    if (count == 0 || fences_address == 0 || values_address == 0 ||
        (flags & ~D3D12_MULTIPLE_FENCE_WAIT_FLAG_ANY) != 0) {
        m.set_return(E_INVALIDARG);
        return;
    }
    const bool all = (flags & D3D12_MULTIPLE_FENCE_WAIT_FLAG_ANY) == 0;
    const usize pointer_size = m_is_64bit ? 8u : 4u;
    std::vector<std::pair<const graphics::BackendFence*, u64>> fences;
    std::vector<std::shared_ptr<graphics::BackendFence>> keep_alive;
    std::vector<std::pair<GuestAddr, u64>> identities;  // address, serial
    for (u32 index = 0; index < count; ++index) {
        GuestAddr address = 0;
        (void)read_pointer(fences_address + index * pointer_size, address);
        Fence* fence = find<Fence>(address, Kind::Fence);
        const auto value = m_memory.read_u64(values_address + index * 8u);
        if (fence == nullptr || !value.is_ok()) {
            m.set_return(E_INVALIDARG);
            return;
        }
        fences.emplace_back(fence->timeline->backend.get(), value.value());
        keep_alive.push_back(fence->timeline->backend);
        identities.emplace_back(fence->address, fence->serial);
    }
    if (event != 0) {
        auto& signaler = GpuEventSignaler::instance();
        if (!signaler.available()) {
            unimplemented("ID3D12Device1::SetEventOnMultipleFenceCompletion",
                          "event handles without the GPU event bridge");
            m.set_return(E_NOTIMPL);
            return;
        }
        signaler.signal_when_fences(std::move(fences), all, event);
        m.set_return(S_OK);
        return;
    }
    // Learn: a null event blocks until the condition holds. Bounded like
    // SetEventOnCompletion's, with the frontend lock released so other
    // threads can submit the signals.
    const auto done = [&] {
        usize reached = 0;
        for (const auto& [fence, value] : fences) {
            if (fence->completed_value() >= value) ++reached;
        }
        return all ? reached == fences.size() : reached != 0;
    };
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::nanoseconds(FENCE_BLOCKING_WAIT_TIMEOUT_NS);
    bool reached = false;
    {
        FrontendUnlock unlocked(*this);
        reached = done();
        while (!reached && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::microseconds(200));
            reached = done();
        }
    }
    for (const auto& [address, serial] : identities) {
        if (find_serial<Fence>(address, serial, Kind::Fence) == nullptr) {
            m.set_return(E_FAIL);
            return;
        }
    }
    if (!reached) {
        d3d12_diag("ID3D12Device1::SetEventOnMultipleFenceCompletion timed out");
    }
    m.set_return(reached ? S_OK : E_FAIL);
}

// =============================================================================
// Reserved-resource tile mappings
// =============================================================================
//
// Learn ID3D12CommandQueue::UpdateTileMappings / CopyTileMappings. Both are
// void. D3D12_TILED_RESOURCE_COORDINATE and D3D12_TILE_REGION_SIZE are four
// UINTs and (UINT, BOOL, UINT, UINT16, UINT16) -- 16 bytes on x86 and x64,
// the same layout as the D3D11.2 structs (d3d12.h; not in d3d12_layouts.hpp,
// which has no pointer-free tile structs). A reserved resource may map tiles
// from several heaps at once (Learn, D3D12_TILED_RESOURCES_TIER remarks);
// one UpdateTileMappings call names a single heap. NULL mappings need no
// heap. The backend records the update in the submit stream, ordered with
// the queue's work.
//
// Tile indexes are the ones GetResourceTiling reports (reserved_tiled_layout):
// the standard tile shape for every non-packed mip, then the backend's
// packed tail per array slice. D3D11.3 5.9.2.8.5: packing is the IHV's, a mip
// that fills a standard tile in every dimension is never packed (the tier-2
// guarantee build_reserved_tiles enforces), and the packed tiles of a slice
// are addressed with the subresource of any packed mip and X the tile within
// the tail (Learn D3D12_TILED_RESOURCE_COORDINATE). A standard mip translates
// to the same level and tile x,y; a packed tile to the slice's tail tile x.

namespace {

constexpr u32 kOutsideTile = 0xFFFFFFFFu;
constexpr u32 kCoordinateBytes = 16;
constexpr u32 kRegionBytes = 16;

struct Coordinate {
    u32 x{0};
    u32 y{0};
    u32 z{0};
    u32 subresource{0};
};

struct RegionSize {
    u32 num_tiles{0};
    bool use_box{false};
    u32 width{0};
    u32 height{0};
    u32 depth{0};
};

bool read_coordinate(GuestMemory& memory, GuestAddr address, Coordinate& out) {
    std::array<u32, 4> words{};
    for (usize i = 0; i < words.size(); ++i) {
        const auto value = memory.read_u32(address + i * 4u);
        if (value.is_err()) return false;
        words[i] = value.value();
    }
    out = {words[0], words[1], words[2], words[3]};
    return true;
}

bool read_region(GuestMemory& memory, GuestAddr address, RegionSize& out) {
    const auto tiles = memory.read_u32(address);
    const auto box = memory.read_u32(address + 4);
    const auto width = memory.read_u32(address + 8);
    const auto extents = memory.read_u32(address + 12);
    if (tiles.is_err() || box.is_err() || width.is_err() || extents.is_err()) {
        return false;
    }
    out.num_tiles = tiles.value();
    out.use_box = box.value() != 0;
    out.width = width.value();
    out.height = extents.value() & 0xFFFFu;
    out.depth = extents.value() >> 16;
    return true;
}

bool region_count(const RegionSize& region, u64& count) {
    if (region.use_box) {
        if (region.width == 0 || region.height == 0 || region.depth == 0) {
            return false;
        }
        if (region.width > ~0ull / region.height) return false;
        const u64 plane = static_cast<u64>(region.width) * region.height;
        if (region.depth != 0 && plane > ~0ull / region.depth) return false;
        const u64 box = plane * region.depth;
        if (box != region.num_tiles) return false;
        count = box;
        return true;
    }
    if (region.num_tiles == 0) return false;
    count = region.num_tiles;
    return true;
}

ResourceDesc layout_desc_from(const ResourceDescription& d) {
    ResourceDesc out{};
    out.dimension = d.dimension;
    out.alignment = d.alignment;
    out.width = d.width;
    out.height = d.height;
    out.depth_or_array_size = static_cast<u16>(d.depth_or_array_size);
    out.mip_levels = static_cast<u16>(d.mip_levels);
    out.format = d.format;
    out.sample_count = d.sample_count;
    out.sample_quality = d.sample_quality;
    out.layout = d.layout;
    out.flags = d.flags;
    out.mip_region_width = d.mip_region[0];
    out.mip_region_height = d.mip_region[1];
    out.mip_region_depth = d.mip_region[2];
    return out;
}

}  // namespace

bool reserved_tiled_layout(const Resource& resource, TiledLayout& layout) {
    layout = {};
    if (!resource.reserved || resource.tiles == nullptr) return false;
    const ResourceDesc desc = layout_desc_from(resource.desc);
    if (!standard_tiled_layout(desc, layout)) return false;
    if (desc.dimension == RESOURCE_DIMENSION_BUFFER) return true;
    const ReservedTiles& tiles = *resource.tiles;
    // The backend's tile is the standard one (the format's TILED support
    // compares them), and its tail is what the device really has: report it.
    if (desc.dimension != RESOURCE_DIMENSION_TEXTURE2D || tiles.buffer ||
        tiles.tile_width != layout.tile_width || tiles.tile_height != layout.tile_height ||
        layout.tile_depth != 1) {
        layout = {};
        return false;
    }
    layout.standard_mips = std::min(tiles.standard_mips, resolved_mip_levels(desc));
    layout.packed_tiles = layout.standard_mips < resolved_mip_levels(desc) ? tiles.packed_tiles : 0;
    return true;
}

namespace {

/// GetResourceTiling's tile space for a resource this device created
/// (reserved_tiled_layout): standard mips, then the packed tail of each slice.
struct ReportedTiles {
    bool ok{false};
    bool buffer{false};
    u32 tile_w{1};
    u32 tile_h{1};
    u32 mips{1};
    u32 standard_mips{1};
    u32 packed_tiles{0};
    u32 packed_start{0};  ///< first packed tile within a slice
    u32 arrays{1};
    u32 per_slice{0};
    u32 total{0};
    std::vector<u32> mip_tx;
    std::vector<u32> mip_ty;
    std::vector<u32> mip_first;
};


ReportedTiles reported_tiles(const Resource& resource) {
    ReportedTiles space;
    const ResourceDesc desc = layout_desc_from(resource.desc);
    TiledLayout layout{};
    if (!reserved_tiled_layout(resource, layout)) return space;
    const ResourceTiling tiling = resource_tiling(desc, layout);
    space.ok = true;
    space.buffer = desc.dimension == RESOURCE_DIMENSION_BUFFER;
    space.tile_w = layout.tile_width;
    space.tile_h = layout.tile_height;
    space.total = tiling.total_tiles;
    space.mips = space.buffer ? 1u : resolved_mip_levels(desc);
    space.standard_mips = space.buffer ? 1u : std::min(layout.standard_mips, space.mips);
    space.packed_tiles = space.standard_mips < space.mips ? layout.packed_tiles : 0u;
    if (desc.dimension == RESOURCE_DIMENSION_TEXTURE2D ||
        desc.dimension == RESOURCE_DIMENSION_TEXTURE1D) {
        space.arrays = std::max(1u, static_cast<u32>(desc.depth_or_array_size));
    }
    space.per_slice = space.arrays == 0 ? 0u : space.total / space.arrays;
    space.packed_start = space.per_slice - space.packed_tiles;
    space.mip_tx.resize(space.standard_mips);
    space.mip_ty.resize(space.standard_mips);
    space.mip_first.resize(space.standard_mips);
    for (u32 mip = 0; mip < space.standard_mips; ++mip) {
        const SubresourceTiling sub = subresource_tiling(desc, layout, mip);
        space.mip_tx[mip] = sub.width_in_tiles;
        space.mip_ty[mip] = sub.height_in_tiles;
        space.mip_first[mip] = sub.start_tile_index;
    }
    return space;
}

u32 reported_tile_count(const Resource& resource) {
    const ReportedTiles space = reported_tiles(resource);
    return space.ok ? space.total : 0u;
}

/// The mip, slice and tile x,y of reported tile @p index. A tile of a slice's
/// packed tail sets @p packed, @p mip to the first packed mip and @p x to the
/// tile within the tail.
bool locate_reported_tile(const ReportedTiles& space, u32 index, u32& mip, u32& slice,
                          u32& x, u32& y, bool& packed) {
    packed = false;
    if (!space.ok || index >= space.total) return false;
    if (space.buffer) {
        mip = 0;
        slice = 0;
        x = index;
        y = 0;
        return true;
    }
    if (space.per_slice == 0) return false;
    slice = index / space.per_slice;
    const u32 within = index % space.per_slice;
    if (space.packed_tiles != 0 && within >= space.packed_start) {
        packed = true;
        mip = space.standard_mips;
        x = within - space.packed_start;
        y = 0;
        return true;
    }
    for (u32 m = 0; m < space.standard_mips && m < space.mip_tx.size() &&
                    m < space.mip_ty.size() && m < space.mip_first.size();
         ++m) {
        const u32 span = space.mip_tx[m] * space.mip_ty[m];
        const u32 first = space.mip_first[m];
        if (static_cast<u64>(within) < static_cast<u64>(first) + span) {
            const u32 local = within - first;
            mip = m;
            x = space.mip_tx[m] == 0 ? 0 : local % space.mip_tx[m];
            y = space.mip_tx[m] == 0 ? 0 : local / space.mip_tx[m];
            return true;
        }
    }
    return false;
}

/// Backend coordinate for a guest tile. False when the backend's tile shape
/// or tail is not the one GetResourceTiling reports.
bool backend_coord(const ReservedTiles& backend, const ReportedTiles& space, u32 index,
                   graphics::BackendTileCoord& coord) {
    coord = {};
    u32 mip = 0;
    u32 slice = 0;
    u32 x = 0;
    u32 y = 0;
    bool packed = false;
    if (!locate_reported_tile(space, index, mip, slice, x, y, packed)) return false;
    if (space.buffer || backend.buffer) {
        coord.x = index;
        return true;
    }
    if (backend.tile_width != space.tile_w || backend.tile_height != space.tile_h) {
        return false;
    }
    if (packed) {
        if (backend.standard_mips != space.standard_mips ||
            backend.packed_tiles != space.packed_tiles || x >= backend.packed_tiles) {
            return false;
        }
        coord.level = mip;
        coord.slice = slice;
        coord.x = x;
        coord.packed = true;
        return true;
    }
    if (mip >= backend.standard_mips || mip >= backend.mip_tiles_x.size() ||
        mip >= backend.mip_tiles_y.size()) {
        return false;
    }
    if (x >= backend.mip_tiles_x[mip] || y >= backend.mip_tiles_y[mip]) return false;
    coord.level = mip;
    coord.slice = slice;
    coord.x = x;
    coord.y = y;
    coord.packed = false;
    return true;
}

/// Tiles a region names, in the reported index space, x-then-y-then-z. A box
/// tile outside its subresource is kOutsideTile unless @p strict, which
/// refuses the region (CopyTileMappings: the regions must fit or the call is
/// undefined).
bool expand_guest_region(const Resource& resource, u32 x, u32 y, u32 z, u32 subresource,
                         u32 num_tiles, bool use_box, u32 width, u32 height, u32 depth,
                         bool strict, std::vector<u32>& out) {
    RegionSize region{};
    region.num_tiles = num_tiles;
    region.use_box = use_box;
    region.width = width;
    region.height = height;
    region.depth = depth;
    u64 count = 0;
    if (!region_count(region, count)) return false;
    const ReportedTiles space = reported_tiles(resource);
    if (!space.ok || space.total == 0) return false;
    if (space.buffer) {
        if (subresource != 0) return false;
        const u64 run = use_box ? width : count;
        if (static_cast<u64>(x) + run > space.total) return false;
        if (strict && run != count) return false;
        for (u64 i = 0; i < count; ++i) {
            out.push_back(!use_box || i < run ? x + static_cast<u32>(i) : kOutsideTile);
        }
        return true;
    }
    if (space.mips == 0) return false;
    const u32 subresources = space.mips * space.arrays;
    if (subresource >= subresources) return false;
    const u32 mip = subresource % space.mips;
    const u32 slice = subresource / space.mips;
    if (mip >= space.standard_mips) {
        // Learn D3D12_TILED_RESOURCE_COORDINATE: any packed mip's subresource
        // names the slice's packed tiles, X is the tile within them, Y and Z
        // are 0. A box spans X only.
        if (space.packed_tiles == 0 || y != 0 || z != 0 || x >= space.packed_tiles) {
            return false;
        }
        const u64 start = static_cast<u64>(slice) * space.per_slice + space.packed_start + x;
        if (!use_box) {
            // A run walks the resource's tile order, as from a standard mip.
            if (start + count > space.total) return false;
            for (u64 i = 0; i < count; ++i) out.push_back(static_cast<u32>(start + i));
            return true;
        }
        if (height != 1 || depth != 1) return false;
        for (u64 i = 0; i < count; ++i) {
            const bool inside = static_cast<u64>(x) + i < space.packed_tiles;
            if (!inside && strict) return false;
            out.push_back(inside ? static_cast<u32>(start + i) : kOutsideTile);
        }
        return true;
    }
    if (mip >= space.mip_tx.size() || mip >= space.mip_ty.size() ||
        mip >= space.mip_first.size()) {
        return false;
    }
    const u32 tx = space.mip_tx[mip];
    const u32 ty = space.mip_ty[mip];
    if (!use_box) {
        const u64 start = static_cast<u64>(slice) * space.per_slice + space.mip_first[mip] +
                          static_cast<u64>(y) * tx + x;
        if (start + count > space.total) return false;
        for (u64 i = 0; i < count; ++i) {
            out.push_back(static_cast<u32>(start + i));
        }
        return true;
    }
    for (u32 dz = 0; dz < depth; ++dz) {
        for (u32 dy = 0; dy < height; ++dy) {
            for (u32 dx = 0; dx < width; ++dx) {
                const u64 px = static_cast<u64>(x) + dx;
                const u64 py = static_cast<u64>(y) + dy;
                const u64 ps = static_cast<u64>(slice) + dz;
                if (z != 0 || px >= tx || py >= ty || ps >= space.arrays) {
                    if (strict) return false;
                    out.push_back(kOutsideTile);
                    continue;
                }
                out.push_back(static_cast<u32>(ps * space.per_slice + space.mip_first[mip] +
                                               py * tx + px));
            }
        }
    }
    return true;
}

bool describe_guest_copy_tile(const Resource& resource, u32 index, u32& level, u32& slice,
                              u32& texel_x, u32& texel_y, u32& tile_width, u32& tile_height) {
    const ReportedTiles space = reported_tiles(resource);
    u32 mip = 0;
    u32 x = 0;
    u32 y = 0;
    bool packed = false;
    // D3D11.3 5.9.2.8.5: CopyTiles cannot reach packed mips.
    if (!locate_reported_tile(space, index, mip, slice, x, y, packed) || space.buffer ||
        packed) {
        return false;
    }
    level = mip;
    tile_width = space.tile_w;
    tile_height = space.tile_h;
    texel_x = x * space.tile_w;
    texel_y = y * space.tile_h;
    return true;
}

bool heap_accepts(const Heap& heap, const Resource& resource) {
    const bool rt_ds =
        (resource.desc.flags & (D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET |
                                D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL)) != 0;
    if (resource.is_buffer()) {
        return (heap.flags & D3D12_HEAP_FLAG_DENY_BUFFERS) == 0;
    }
    if (rt_ds) return (heap.flags & D3D12_HEAP_FLAG_DENY_RT_DS_TEXTURES) == 0;
    return (heap.flags & D3D12_HEAP_FLAG_DENY_NON_RT_DS_TEXTURES) == 0;
}

}  // namespace

bool guest_tile_region(const Resource& resource, u32 x, u32 y, u32 z, u32 subresource,
                       u32 num_tiles, bool use_box, u32 width, u32 height, u32 depth,
                       bool strict, std::vector<u32>& out) {
    return expand_guest_region(resource, x, y, z, subresource, num_tiles, use_box, width,
                               height, depth, strict, out);
}

bool guest_copy_tile(const Resource& resource, u32 index, u32& level, u32& slice,
                     u32& texel_x, u32& texel_y, u32& tile_width, u32& tile_height) {
    return describe_guest_copy_tile(resource, index, level, slice, texel_x, texel_y,
                                    tile_width, tile_height);
}

void Frontend::apply_tile_changes(CommandQueue& queue, Resource& resource,
                                  const std::map<u32, TileAssignment>& changes) {
    if (resource.tiles == nullptr || changes.empty()) return;
    ReservedTiles& tiles = *resource.tiles;
    const ReportedTiles space = reported_tiles(resource);
    if (!space.ok) {
        unimplemented("ID3D12CommandQueue::UpdateTileMappings", "tiled layout");
        return;
    }
    // Every reported tile has a backend coordinate: GetResourceTiling reports
    // the backend's own shape and tail (reserved_tiled_layout).
    for (const auto& change : changes) {
        graphics::BackendTileCoord coord{};
        if (backend_coord(tiles, space, change.first, coord)) continue;
        unimplemented("ID3D12CommandQueue::UpdateTileMappings",
                      "backend tile layout differs from GetResourceTiling");
        return;
    }
    struct Taken {
        std::shared_ptr<graphics::BackendTilePool> pool;
        u64 pool_tile{0};
    };
    std::vector<Taken> referenced;
    struct Delta {
        u32 index{0};
        TileAssignment desired{};
    };
    std::vector<Delta> deltas;
    for (const auto& [index, desired] : changes) {
        auto old = tiles.mapped.find(index);
        const bool was = old != tiles.mapped.end();
        if (!desired.pool) {
            if (!was) continue;
        } else if (was && old->second.pool.get() == desired.pool.get() &&
                   old->second.pool_tile == desired.pool_tile) {
            continue;
        }
        if (desired.pool) {
            if (!desired.pool->reference(desired.pool_tile)) {
                for (const Taken& taken : referenced) {
                    taken.pool->unreference(taken.pool_tile);
                }
                // The call is void, so a failed allocation cannot return
                // E_OUTOFMEMORY. Leaving the mappings unchanged and removing
                // the device keeps that failure visible.
                remove_device(DXGI_ERROR_DEVICE_REMOVED,
                              "UpdateTileMappings could not allocate tile memory");
                return;
            }
            referenced.push_back({desired.pool, desired.pool_tile});
        }
        deltas.push_back({index, desired});
    }
    if (deltas.empty()) return;

    struct Group {
        std::shared_ptr<graphics::BackendTilePool> pool;
        std::vector<graphics::BackendTileMapping> mappings;
    };
    std::vector<Group> groups;
    auto group_for = [&](graphics::BackendTilePool* pool,
                         const std::shared_ptr<graphics::BackendTilePool>& keep)
        -> Group& {
        for (Group& group : groups) {
            if (group.pool.get() == pool) return group;
        }
        groups.push_back(Group{keep, {}});
        return groups.back();
    };
    auto note_remove = [&](graphics::BackendTilePool* pool, u64 pool_tile) {
        auto uses = tiles.uses.find({pool, pool_tile});
        if (uses == tiles.uses.end()) return;
        if (uses->second == 2 && tiles.shared_tiles > 0) --tiles.shared_tiles;
        if (--uses->second == 0) tiles.uses.erase(uses);
    };
    auto note_add = [&](graphics::BackendTilePool* pool, u64 pool_tile) {
        if (++tiles.uses[{pool, pool_tile}] == 2) ++tiles.shared_tiles;
    };
    for (const Delta& delta : deltas) {
        graphics::BackendTileCoord coord{};
        if (!backend_coord(tiles, space, delta.index, coord)) {
            // The check above accepted every index.
            if (delta.desired.pool) delta.desired.pool->unreference(delta.desired.pool_tile);
            continue;
        }
        auto old = tiles.mapped.find(delta.index);
        if (old != tiles.mapped.end()) {
            if (old->second.pool) {
                old->second.pool->unreference(old->second.pool_tile);
                note_remove(old->second.pool.get(), old->second.pool_tile);
            }
            tiles.mapped.erase(old);
        }
        const u64 pool_tile = delta.desired.pool
                                  ? delta.desired.pool_tile
                                  : graphics::BACKEND_TILE_UNMAPPED;
        group_for(delta.desired.pool.get(), delta.desired.pool)
            .mappings.push_back({coord, pool_tile});
        if (delta.desired.pool) {
            note_add(delta.desired.pool.get(), delta.desired.pool_tile);
            tiles.mapped[delta.index] =
                ReservedTiles::Binding{delta.desired.pool, delta.desired.pool_tile};
        }
    }
    const bool self_aliased = tiles.shared_tiles != 0;
    const GuestAddr address = resource.address;
    const u64 serial = resource.serial;
    // The residency a shader's status reads changes in queue order with the
    // mappings (ReservedTiles::resident).
    std::vector<std::pair<u32, bool>> residency;
    residency.reserve(deltas.size());
    for (const Delta& delta : deltas) {
        residency.emplace_back(delta.index, delta.desired.pool != nullptr);
    }
    auto run = [this, address, serial, groups = std::move(groups), self_aliased,
                residency = std::move(residency), bits = tiles.resident,
                total = tiles.total_tiles]() {
        if (bits) {
            bits->resize((static_cast<usize>(total) + 31u) / 32u, 0u);
            for (const auto& [index, mapped] : residency) {
                if (index >= total) continue;
                const u32 bit = 1u << (index % 32u);
                if (mapped) (*bits)[index / 32u] |= bit;
                else (*bits)[index / 32u] &= ~bit;
            }
        }
        Resource* live = find_serial<Resource>(address, serial, Kind::Resource);
        if (live == nullptr) return;
        graphics::BackendTexture2D* texture = live->texture.get();
        graphics::BackendBuffer* buffer = live->tiled_buffer.get();
        for (const Group& group : groups) {
            if (group.mappings.empty()) continue;
            const bool recorded =
                texture != nullptr
                    ? m_backend.update_tile_mappings(
                          texture, group.pool.get(), group.mappings.data(),
                          group.mappings.size(), self_aliased)
                    : m_backend.update_tile_mappings(
                          buffer, group.pool.get(), group.mappings.data(),
                          group.mappings.size(), self_aliased);
            if (!recorded) {
                unimplemented("ID3D12CommandQueue::UpdateTileMappings",
                              "backend refused a tile-mapping update");
            }
        }
    };
    if (queue.pending.empty()) {
        run();
        return;
    }
    CommandQueue::Pending pending{};
    pending.op = CommandQueue::Pending::Op::TileMappings;
    pending.tile_mappings = std::move(run);
    queue.pending.push_back(std::move(pending));
    m_exclusive_queue_ops.fetch_add(1);
}

void Frontend::queue_update_tile_mappings(Marshaler& m, CommandQueue& queue) {
    // UpdateTileMappings(pResource, NumResourceRegions,
    //     pResourceRegionStartCoordinates, pResourceRegionSizes, pHeap,
    //     NumRanges, pRangeFlags, pHeapRangeStartOffsets, pRangeTileCounts,
    //     Flags). void.
    m.set_return(0);
    const GuestAddr resource_address = m.arg_ptr(1);
    const u32 region_count_arg = m.arg32(2);
    const GuestAddr coordinates = m.arg_ptr(3);
    const GuestAddr sizes = m.arg_ptr(4);
    const GuestAddr heap_address = m.arg_ptr(5);
    const u32 range_count = m.arg32(6);
    const GuestAddr range_flags = m.arg_ptr(7);
    const GuestAddr heap_offsets = m.arg_ptr(8);
    const GuestAddr range_tile_counts = m.arg_ptr(9);
    const u32 flags = m.arg32(10);
    constexpr const char* kApi = "ID3D12CommandQueue::UpdateTileMappings";
    Resource* resource = find<Resource>(resource_address, Kind::Resource);
    if (resource == nullptr || !resource->reserved || resource->tiles == nullptr ||
        (flags & ~D3D12_TILE_MAPPING_FLAG_NO_HAZARD) != 0 || region_count_arg == 0 ||
        range_count == 0 || (region_count_arg > 1 && coordinates == 0) ||
        region_count_arg > 0x100000u || range_count > 0x100000u) {
        d3d12_diag("%s: invalid resource, flags, or region/range count", kApi);
        return;
    }
    const u32 guest_tiles = reported_tile_count(*resource);
    if (guest_tiles == 0) {
        unimplemented(kApi, "tiled layout");
        return;
    }
    std::vector<u32> region_tiles_out;
    for (u32 r = 0; r < region_count_arg; ++r) {
        Coordinate at{};
        if (coordinates != 0 &&
            !read_coordinate(m_memory, coordinates + r * kCoordinateBytes, at)) {
            d3d12_diag("%s: unreadable region coordinate", kApi);
            return;
        }
        RegionSize region{};
        if (sizes != 0) {
            if (!read_region(m_memory, sizes + r * kRegionBytes, region)) {
                d3d12_diag("%s: unreadable region size", kApi);
                return;
            }
        } else {
            // Learn: NULL sizes with NULL coordinates are the whole resource;
            // NULL sizes with coordinates are one tile each. The count is the
            // one GetResourceTiling reports, not the backend's packed tail.
            region.num_tiles = coordinates == 0 ? guest_tiles : 1u;
        }
        if (!expand_guest_region(*resource, at.x, at.y, at.z, at.subresource,
                                 region.num_tiles, region.use_box, region.width,
                                 region.height, region.depth, false, region_tiles_out)) {
            d3d12_diag("%s: region does not describe tiles of the resource", kApi);
            return;
        }
    }
    struct Range {
        u32 flag{0};
        u32 offset{0};
        u64 count{0};
    };
    std::vector<Range> ranges(range_count);
    u64 range_total = 0;
    bool needs_heap = false;
    for (u32 i = 0; i < range_count; ++i) {
        Range& range = ranges[i];
        if (range_flags != 0) {
            const auto flag = m_memory.read_u32(range_flags + i * 4u);
            if (flag.is_err()) {
                d3d12_diag("%s: unreadable range flags", kApi);
                return;
            }
            range.flag = flag.value();
        }
        if (range.flag != D3D12_TILE_RANGE_FLAG_NONE &&
            range.flag != D3D12_TILE_RANGE_FLAG_NULL &&
            range.flag != D3D12_TILE_RANGE_FLAG_SKIP &&
            range.flag != D3D12_TILE_RANGE_FLAG_REUSE_SINGLE_TILE) {
            d3d12_diag("%s: range flag 0x%x", kApi, range.flag);
            return;
        }
        const bool maps = range.flag == D3D12_TILE_RANGE_FLAG_NONE ||
                          range.flag == D3D12_TILE_RANGE_FLAG_REUSE_SINGLE_TILE;
        if (maps) {
            needs_heap = true;
            if (heap_offsets == 0) {
                d3d12_diag("%s: mapped range without a heap offset", kApi);
                return;
            }
            const auto offset = m_memory.read_u32(heap_offsets + i * 4u);
            if (offset.is_err()) {
                d3d12_diag("%s: unreadable heap offset", kApi);
                return;
            }
            range.offset = offset.value();
        }
        if (range_tile_counts != 0) {
            const auto count = m_memory.read_u32(range_tile_counts + i * 4u);
            if (count.is_err()) {
                d3d12_diag("%s: unreadable range tile count", kApi);
                return;
            }
            range.count = count.value();
        } else {
            range.count = range_count == 1 ? region_tiles_out.size() : 1u;
        }
        if (range.count > ~0ull - range_total) {
            d3d12_diag("%s: range tile count overflows", kApi);
            return;
        }
        range_total += range.count;
    }
    if (range_total != region_tiles_out.size()) {
        d3d12_diag("%s: range tiles (%llu) != region tiles (%zu)", kApi,
                   static_cast<unsigned long long>(range_total),
                   region_tiles_out.size());
        return;
    }
    Heap* heap = nullptr;
    std::shared_ptr<graphics::BackendTilePool> pool;
    if (heap_address != 0 || needs_heap) {
        heap = find<Heap>(heap_address, Kind::Heap);
        if (heap == nullptr || !heap_accepts(*heap, *resource)) {
            d3d12_diag("%s: heap cannot back this resource", kApi);
            return;
        }
        pool = heap_tile_pool(*heap);
        if (!pool) {
            unimplemented(kApi, "backend tile pool");
            return;
        }
        for (const Range& range : ranges) {
            if (range.count == 0) continue;
            if (range.flag == D3D12_TILE_RANGE_FLAG_NONE &&
                (static_cast<u64>(range.offset) + range.count > heap->tile_count)) {
                d3d12_diag("%s: heap tile range past the heap", kApi);
                return;
            }
            if (range.flag == D3D12_TILE_RANGE_FLAG_REUSE_SINGLE_TILE &&
                range.offset >= heap->tile_count) {
                d3d12_diag("%s: reused heap tile past the heap", kApi);
                return;
            }
        }
    }
    std::map<u32, TileAssignment> changes;
    usize next = 0;
    for (const Range& range : ranges) {
        for (u64 j = 0; j < range.count; ++j, ++next) {
            const u32 tile = region_tiles_out[next];
            if (tile == kOutsideTile || range.flag == D3D12_TILE_RANGE_FLAG_SKIP) {
                continue;
            }
            TileAssignment assignment{};
            if (range.flag == D3D12_TILE_RANGE_FLAG_NONE) {
                assignment.pool = pool;
                assignment.pool_tile = static_cast<u64>(range.offset) + j;
            } else if (range.flag == D3D12_TILE_RANGE_FLAG_REUSE_SINGLE_TILE) {
                assignment.pool = pool;
                assignment.pool_tile = range.offset;
            }
            changes[tile] = std::move(assignment);
        }
    }
    apply_tile_changes(queue, *resource, changes);
}

void Frontend::queue_copy_tile_mappings(Marshaler& m, CommandQueue& queue) {
    // CopyTileMappings(pDstResource, pDstRegionStartCoordinate, pSrcResource,
    //     pSrcRegionStartCoordinate, pRegionSize, Flags). void. Learn: the
    // copy is as if the source was saved to a temporary, so overlap is well
    // defined; a region that does not fit the resource is undefined and is
    // refused here.
    m.set_return(0);
    constexpr const char* kApi = "ID3D12CommandQueue::CopyTileMappings";
    const u32 flags = m.arg32(6);
    Resource* dst = find<Resource>(m.arg_ptr(1), Kind::Resource);
    Resource* src = find<Resource>(m.arg_ptr(3), Kind::Resource);
    const GuestAddr dst_at_address = m.arg_ptr(2);
    const GuestAddr src_at_address = m.arg_ptr(4);
    const GuestAddr size_address = m.arg_ptr(5);
    Coordinate dst_at{};
    Coordinate src_at{};
    RegionSize region{};
    if (dst == nullptr || src == nullptr || !dst->reserved || !src->reserved ||
        dst->tiles == nullptr || src->tiles == nullptr ||
        (flags & ~D3D12_TILE_MAPPING_FLAG_NO_HAZARD) != 0 ||
        dst_at_address == 0 || src_at_address == 0 || size_address == 0 ||
        !read_coordinate(m_memory, dst_at_address, dst_at) ||
        !read_coordinate(m_memory, src_at_address, src_at) ||
        !read_region(m_memory, size_address, region)) {
        d3d12_diag("%s: invalid resources, flags, or region", kApi);
        return;
    }
    if (reported_tile_count(*dst) == 0 || reported_tile_count(*src) == 0) {
        unimplemented(kApi, "tiled layout");
        return;
    }
    std::vector<u32> dst_tiles;
    std::vector<u32> src_tiles;
    if (!expand_guest_region(*dst, dst_at.x, dst_at.y, dst_at.z, dst_at.subresource,
                             region.num_tiles, region.use_box, region.width, region.height,
                             region.depth, true, dst_tiles) ||
        !expand_guest_region(*src, src_at.x, src_at.y, src_at.z, src_at.subresource,
                             region.num_tiles, region.use_box, region.width, region.height,
                             region.depth, true, src_tiles) ||
        dst_tiles.size() != src_tiles.size()) {
        d3d12_diag("%s: region does not fit both resources", kApi);
        return;
    }
    std::map<u32, TileAssignment> changes;
    for (usize i = 0; i < dst_tiles.size(); ++i) {
        if (dst_tiles[i] == kOutsideTile) continue;
        TileAssignment assignment{};
        if (src_tiles[i] != kOutsideTile) {
            auto found = src->tiles->mapped.find(src_tiles[i]);
            if (found != src->tiles->mapped.end()) {
                assignment.pool = found->second.pool;
                assignment.pool_tile = found->second.pool_tile;
            }
        }
        changes[dst_tiles[i]] = std::move(assignment);
    }
    apply_tile_changes(queue, *dst, changes);
}

}  // namespace vitreo::winapi::d3d12
