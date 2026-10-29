// SPDX-License-Identifier: LGPL-2.1-only

/// @file metal_submit_queue.mm
/// @brief Command-stream worker: encode recorded draws, then present.

#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#import <CoreFoundation/CoreFoundation.h>
#import <pthread.h>

#include <cmath>
#include "graphics/metal/metal_submit_queue.hpp"
#include "graphics/metal/metal_submit_queue_internal.hpp"
#include "graphics/metal/metal_tiled.hpp"
#include "graphics/metal/metal_shared_resource.hpp"
#include "graphics/metal/metal_command.hpp"
#include "graphics/metal/metal_device.hpp"
#include "graphics/metal/metal_swapchain.hpp"
#include "graphics/metal/metal_texture.hpp"

#include <vitreo/present_cpu.hpp>
#include "graphics/contracts/delta_record.hpp"
#include "graphics/contracts/slim_record.hpp"

#include <algorithm>
#include <string>
#include <vector>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <utility>

namespace vitreo::graphics {

thread_local std::vector<MetalSubmitQueue::Op>* MetalSubmitQueue::t_capture =
    nullptr;
thread_local MetalSubmitQueue::CommandListId MetalSubmitQueue::t_capture_id = 0;
thread_local u64 MetalSubmitQueue::t_recorded_generation = 0;

namespace {
/// MetalSubmitQueue::set_append_hook_for_test.
std::atomic<MetalSubmitQueue::AppendHook> g_append_hook{nullptr};
std::atomic<void*> g_append_hook_context{nullptr};
}  // namespace

void MetalSubmitQueue::append(Op&& op) {
    // Capture is per thread and needs no lock: begin_capture handed this thread a
    // stable vector address, and no other thread appends to it.
    if (t_capture != nullptr) {
        t_capture->push_back(std::move(op));
        t_recorded_generation = 0;  // runs when the list is executed
        return;
    }
    note_shared_access(op);
    const OpKind kind = op.kind;
    {
        std::lock_guard<std::mutex> lock(m_mu);
        // pass-fold: an op from outside the in-place path ends the candidate
        // (a reopened pass is split first, so the encoder sees the unfolded
        // stream). fold_settle_locked records an owed EndPass first
        // (pass-continue).
        fold_settle_locked();
        if (kind == OpKind::Present) m_recording_has_present = true;
        if (kind == OpKind::Draw) break_delta_chain_locked();
        m_recording.push_back(std::move(op));
        note_recorded_locked();
    }
    if (const auto hook = g_append_hook.load(std::memory_order_acquire))
        [[unlikely]] {
        hook(kind == OpKind::DownloadTexture ? TestAppendKind::DownloadTexture
             : kind == OpKind::CopyBuffer    ? TestAppendKind::CopyBuffer
                                             : TestAppendKind::Other,
             g_append_hook_context.load(std::memory_order_acquire));
    }
}

void MetalSubmitQueue::set_append_hook_for_test(AppendHook hook,
                                                void* context) noexcept {
    g_append_hook_context.store(context, std::memory_order_release);
    g_append_hook.store(hook, std::memory_order_release);
}

void MetalSubmitQueue::note_shared_access(const Op& op) {
    auto& shared = MetalSharedRegistry::instance();
    if (shared.empty()) return;
    // A Draw and its EndPass sit inside the pass their BeginPass opened; the
    // pull for them ran before that BeginPass. A debug marker touches no
    // resource.
    if (op.kind != OpKind::Draw && op.kind != OpKind::EndPass &&
        op.kind != OpKind::DebugMarker) {
        shared.acquire();
    }
    switch (op.kind) {
    case OpKind::BeginPass:
        if (!op.begin) break;
        for (u32 i = 0; i < op.begin->color_count && i < op.begin->colors.size();
             ++i) {
            shared.note_written_texture(op.begin->colors[i]);
        }
        shared.note_written_texture(op.begin->depth);
        shared.note_written_texture(op.begin->resolve);
        break;
    case OpKind::Draw:
        if (!op.draw) break;
        // Fragment UAVs: textures at BACKEND_UAV_TEXTURE_BASE and up (below
        // it are read-only SRVs), buffers in ps_uav_buffer; the stream-output
        // capture buffer; and whatever a descriptor table reaches (resident
        // resources say nothing about access, so they count as written).
        for (u32 i = BACKEND_UAV_TEXTURE_BASE; i < op.draw->textures.size();
             ++i) {
            shared.note_written_texture(op.draw->textures[i]);
        }
        for (void* buffer : op.draw->ps_uav_buffer) {
            shared.note_written_buffer(buffer);
        }
        for (u32 i = 0; i < op.draw->vertex_stream_count &&
                        i < op.draw->vertex_slots.size();
             ++i) {
            if (op.draw->vertex_slots[i] == BACKEND_VERTEX_CAPTURE_BUFFER_INDEX) {
                shared.note_written_buffer(op.draw->vertex_buffers[i]);
            }
        }
        for (void* resource : op.draw->resident) {
            shared.note_written_resource(resource);
        }
        break;
    case OpKind::Compute:
        if (!op.compute) break;
        for (u32 i = BACKEND_UAV_TEXTURE_BASE; i < op.compute->textures.size();
             ++i) {
            shared.note_written_texture(op.compute->textures[i]);
        }
        // Compute buffer slots do not say which are written (UAVs, stream
        // output targets, counters); a buffer's publication is only its
        // version, so marking a read one costs nothing but a version.
        for (void* buffer : op.compute->buffers) {
            shared.note_written_buffer(buffer);
        }
        for (void* resource : op.compute->resident) {
            shared.note_written_resource(resource);
        }
        break;
    case OpKind::Resolve:
    case OpKind::BlitRegion:
    case OpKind::UploadTexture:
        shared.note_written_texture(op.blit_dst);
        break;
    case OpKind::GenerateMips:
        shared.note_written_texture(op.blit_src);
        break;
    case OpKind::CopyBuffer:
    case OpKind::FillBuffer:
    case OpKind::DownloadTexture:
        shared.note_written_buffer(op.blit_dst);
        break;
    default:
        break;
    }
}

MetalSubmitQueue::MetalSubmitQueue() {
    m_thread = std::thread([this] { thread_main(); });
    std::fprintf(stderr, "[metal] command stream started\n");
}

MetalSubmitQueue::~MetalSubmitQueue() {
    shutdown();
    // The process-wide queue is leaked (instance()), so this runs only for
    // a queue that is really destroyed; its push chunks go with it.
    std::lock_guard<std::mutex> lock(m_mu);
    recycle_push_arena_locked(m_recording_arena);
    for (u8* chunk : m_free_push_chunks) delete[] chunk;
    m_free_push_chunks.clear();
}

void MetalSubmitQueue::signal_gpu_gen(u64 gen) {
    std::lock_guard<std::mutex> lock(m_mu);
    if (gen > m_gpu_gen) {
        m_gpu_gen = gen;
        m_gpu_gen_published.store(gen, std::memory_order_release);
    }
    m_cv.notify_all();
}

u64 MetalSubmitQueue::recording_generation() {
    // submit() pre-increments, so the list being recorded becomes gen+1.
    return m_submit_gen_published.load(std::memory_order_acquire) + 1;
}

u64 MetalSubmitQueue::completed_generation() {
    return m_gpu_gen_published.load(std::memory_order_acquire);
}

void MetalSubmitQueue::wait_until_idle_encode() {
    std::unique_lock<std::mutex> lock(m_mu);
    while (m_running.load(std::memory_order_acquire) &&
           (!m_pending.empty() || m_encode_busy || m_gpu_gen < m_submit_gen)) {
        if (pthread_main_np() != 0) {
            lock.unlock();
            CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.0, true);
            lock.lock();
            if (m_pending.empty() && !m_encode_busy &&
                m_gpu_gen >= m_submit_gen) {
                break;
            }
        }
        m_cv.wait_for(lock, std::chrono::milliseconds(1));
    }
}

void MetalSubmitQueue::wait_until_present_idle() {
    std::unique_lock<std::mutex> lock(m_mu);
    while (m_running.load(std::memory_order_acquire) &&
           (!m_pending.empty() || m_encode_busy || m_presents_inflight != 0 ||
            m_gpu_gen < m_submit_gen)) {
        if (pthread_main_np() != 0) {
            lock.unlock();
            CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.0, true);
            lock.lock();
            if (m_pending.empty() && !m_encode_busy &&
                m_presents_inflight == 0 && m_gpu_gen >= m_submit_gen) {
                break;
            }
        }
        m_cv.wait_for(lock, std::chrono::milliseconds(1));
    }
}

void MetalSubmitQueue::wait_encoded() {
    wait_until_idle_encode();
}

bool MetalSubmitQueue::submit_blocked_locked(bool present) const noexcept {
    if (m_pending.size() >= kMaxPendingLists) return true;
    if (m_submit_gen - m_gpu_gen >= kMaxListsInFlight) return true;
    return present && m_present_gens[m_present_slot] > m_gpu_gen;
}

bool MetalSubmitQueue::submit_would_block() {
    std::lock_guard<std::mutex> lock(m_mu);
    return m_running.load(std::memory_order_acquire) &&
           submit_blocked_locked(true);
}

u64 MetalSubmitQueue::submit() {
    std::unique_lock<std::mutex> lock(m_mu);
    // A plain flush (GetData, Flush, a readback's hand-off) used to wait until
    // fewer than two lists were GPU-incomplete. A D3D11 renderer polls queries
    // between draws, each poll a flush, so the render thread ran in lockstep
    // with a GPU that was idle ~60% of the time. The DISCARD and constant pools recycle
    // on GPU completion (completed_generation()), not on list counts, so
    // only the Present list needs to throttle -- by frames.
    const bool present = m_recording_has_present;
    while (m_running.load(std::memory_order_acquire) &&
           submit_blocked_locked(present)) {
        if (pthread_main_np() != 0) {
            lock.unlock();
            CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.0, true);
            lock.lock();
            if (!submit_blocked_locked(present)) break;
        }
        if (submit_blocked_locked(present)) {
            m_cv.wait_for(lock, std::chrono::milliseconds(1));
        }
    }
    // pass-fold: the list ends here, so does the candidate (split first when
    // it is a reopened pass still open, as the unfolded list would end).
    fold_settle_locked();
    if (!m_running.load(std::memory_order_acquire)) {
        for (Op& op : m_recording) {
            release_op(op);
        }
        m_recording.clear();
        recycle_push_arena_locked(m_recording_arena);
        m_recording_has_present = false;
        return m_submit_gen;
    }
    if (m_recording.empty()) {
        return m_submit_gen;
    }
    ++m_submit_gen;
    m_submit_gen_published.store(m_submit_gen, std::memory_order_release);
    // delta-record: the next recording starts from a keyframe (its RED test
    // mutation drops this and the producer's generation check together).
    if (g_delta_mutation.load(std::memory_order_relaxed) !=
        static_cast<int>(DeltaMutation::NoRecordingKeyframe)) {
        break_delta_chain_locked();
    }
    // The push blocks go with the ops that point into them.
    m_pending.push_back(PendingList{std::move(m_recording), m_submit_gen,
                                    std::move(m_recording_arena)});
    m_recording.clear();  // moved from: valid, now explicitly empty
    m_recording_arena = PushArena{};
    if (present) {
        m_present_gens[m_present_slot] = m_submit_gen;
        m_present_slot = (m_present_slot + 1) % kPresentLatency;
        m_recording_has_present = false;
    }
    m_cv.notify_all();
    return m_submit_gen;
}

bool MetalSubmitQueue::wait_for_generation(u64 generation) {
    std::unique_lock<std::mutex> lock(m_mu);
    m_cv.wait(lock, [this, generation] {
        return m_gpu_gen >= generation ||
               !m_running.load(std::memory_order_acquire);
    });
    return m_gpu_gen >= generation;
}

void MetalSubmitQueue::drain() {
    // Synchronous readers (blit_to_host, readback_w_slice, ...) drain and
    // then read a texture on a command buffer of their own: newer shared
    // contents another process published must be pulled in first.
    MetalSharedRegistry::instance().acquire();
    (void)submit();
    wait_until_present_idle();
}

void MetalSubmitQueue::shutdown() {
    {
        std::lock_guard<std::mutex> lock(m_mu);
        if (!m_running.exchange(false, std::memory_order_acq_rel)) {
            return;
        }
        m_cv.notify_one();
    }
    if (m_thread.joinable()) {
        m_thread.join();
    }
}

void MetalSubmitQueue::thread_main() {
    // Not Present-skip; scheduling only so guest CPU can compete during
    // vsync-off load storms.
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INITIATED, 0);
    for (;;) {
        OpList ops;
        u64 gpu_gen = 0;
        // push-intern: the list's push blocks, given back once it is encoded.
        PushArena arena;
        {
            std::unique_lock<std::mutex> lock(m_mu);
            m_cv.wait(lock, [&] {
                return !m_pending.empty() ||
                       !m_running.load(std::memory_order_acquire);
            });
            if (m_pending.empty()) {
                for (Op& op : m_recording) {
                    release_op(op);
                }
                m_recording.clear();
                recycle_push_arena_locked(m_recording_arena);
                fold_reset_locked();
                return;
            }
            ops = std::move(m_pending.front().ops);
            gpu_gen = m_pending.front().gen;
            arena = std::move(m_pending.front().arena);
            m_pending.pop_front();
            m_encode_busy = true;
            m_cv.notify_all();   // a submit() waiting for queue room
        }

        encode_draws(ops, gpu_gen);
        // delta-record: the references the list's DeltaDraws carry, released
        // with the ops below, outside the lock.
        std::vector<void*> releases = std::move(arena.releases);
        arena.releases.clear();

        {
            std::lock_guard<std::mutex> lock(m_mu);
            // The encoder copied every block where it bound it.
            recycle_push_arena_locked(arena);
            m_encode_busy = false;
            m_cv.notify_all();
        }

        if (present_from_list(ops)) {
            std::lock_guard<std::mutex> lock(m_mu);
            if (m_presents_inflight > 0) {
                --m_presents_inflight;
            }
            m_cv.notify_all();
        }

        for (Op& op : ops) {
            release_op(op);
        }
        ops.clear();
        for (void* handle : releases) release_draw_handle(handle);
    }
}

} // namespace vitreo::graphics
