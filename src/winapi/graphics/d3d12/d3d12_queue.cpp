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
#include "winapi/graphics/common/gpu_event_signaler.hpp"
#include "core/memory/guest_memory.hpp"
#include "vitreo/virtual_tsc.hpp"

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
    fence->backend = m_backend.create_fence(initial);
    if (!fence->backend || !fence->backend->valid()) {
        m.set_return(E_OUTOFMEMORY);
        return;
    }
    fence->submitted = initial;
    m.set_return(return_new_object(std::move(fence), iid, out));
}

// =============================================================================
// Submission
// =============================================================================

bool Frontend::queue_signal(Fence& fence, u64 value) {
    if (value < std::max(fence.submitted, fence.backend->completed_value())) {
        // A queue signal lowers the fence once the queue's earlier work has
        // run (measured, a04 "fence queue Signal(45) (lower)": completed=45).
        // The host timeline cannot run backwards on the GPU, so the queue's
        // work is completed first and the fence set from the CPU.
        if (auto completion = m_backend.flush_completion()) {
            (void)completion->wait();
        } else {
            (void)m_backend.wait_idle();
        }
        return cpu_signal(fence, value);
    }
    if (!fence.backend->signal(value)) return false;
    fence.submitted = std::max(fence.submitted, value);
    return true;
}

bool Frontend::cpu_signal(Fence& fence, u64 value) {
    const u64 completed = fence.backend->completed_value();
    if (value >= completed) {
        if (!fence.backend->signal_cpu(value)) return false;
        fence.submitted = std::max(fence.submitted, value);
        return true;
    }
    // Lower: a new timeline at @p value replaces the old one (the backend's
    // never runs backwards). Events waiting for a value are moved to it; a
    // signal already queued on the GPU for a higher value still raises the
    // fence when it runs, as it does on Windows, bridged from the old
    // timeline once the queued work completes.
    std::shared_ptr<graphics::BackendFence> next = m_backend.create_fence(value);
    if (!next || !next->valid()) return false;
    std::shared_ptr<graphics::BackendFence> old = fence.backend;
    GpuEventSignaler::instance().rebind(old.get(), next.get());
    const u64 in_flight = fence.submitted > completed ? fence.submitted : 0;
    fence.retired.push_back(old);
    fence.backend = next;
    fence.submitted = value;
    if (in_flight != 0) {
        fence.submitted = in_flight;
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
    for (const auto& recording : lists) replay_list(queue, *recording);
    m_backend.flush();
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
            if (fence->submitted < op.value &&
                fence->backend->completed_value() < op.value) {
                return;  // still blocked
            }
            (void)fence->backend->wait_gpu(op.value);
            break;
        case CommandQueue::Pending::Op::Signal:
            if (fence != nullptr) (void)queue_signal(*fence, op.value);
            m_backend.flush();
            break;
        case CommandQueue::Pending::Op::Execute:
            execute(queue, op.recordings);
            break;
        case CommandQueue::Pending::Op::Present:
            // IDXGISwapChain::Present held behind a queue wait: shown now,
            // after the work queued before it (a released chain shows
            // nothing).
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
        for (auto& [address, object] : m_objects) {
            if (object->kind != Kind::CommandQueue) continue;
            auto& queue = static_cast<CommandQueue&>(*object);
            const usize before = queue.pending.size();
            if (before == 0) continue;
            pump_queue(queue);
            if (queue.pending.size() != before) progress = true;
        }
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
    case S::CopyTileMappings:
        // No reserved resource can exist (tier NOT_SUPPORTED), so there are
        // no tiles to map.
        unimplemented(slot == S::UpdateTileMappings
                          ? "ID3D12CommandQueue::UpdateTileMappings"
                          : "ID3D12CommandQueue::CopyTileMappings",
                      "tiled resources");
        m.set_return(0);
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
    case S::GetCompletedValue:
        // UINT64 by value: RAX on x64, EDX:EAX on x86.
        m.set_return_i64(fence.backend->completed_value());
        return;
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
            const std::shared_ptr<graphics::BackendFence> backend =
                fence.backend;
            const u64 serial = fence.serial;
            const GuestAddr address = fence.address;
            m_mutex.unlock();
            const bool reached =
                backend->wait_cpu(value, FENCE_BLOCKING_WAIT_TIMEOUT_NS);
            m_mutex.lock();
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
        signaler.signal_when(fence.backend.get(), value, event);
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
        fences.emplace_back(fence->backend.get(), value.value());
        keep_alive.push_back(fence->backend);
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
    m_mutex.unlock();
    bool reached = done();
    while (!reached && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::microseconds(200));
        reached = done();
    }
    m_mutex.lock();
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

}  // namespace vitreo::winapi::d3d12
