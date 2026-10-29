// SPDX-License-Identifier: LGPL-2.1-only
/// @file d3d9_cs.cpp
/// @brief Serial D3D9 draw/clear worker. Does not touch the Metal CS wait
///        path: T1 opens the pass; the worker records CsDraw / EndPass /
///        GPU clear only.

#include "winapi/graphics/d3d9/d3d9_cs.hpp"

#include <CoreFoundation/CoreFoundation.h>
#include <pthread.h>

#include <chrono>
#include <cstdio>
#include <utility>

namespace vitreo::winapi {

D3d9SubmitQueue& D3d9SubmitQueue::instance() {
    // Leaked: same lifetime as MetalSubmitQueue. A worker or T1 flush
    // during process teardown must not lock a destroyed mutex.
    static D3d9SubmitQueue* queue = new D3d9SubmitQueue();
    return *queue;
}

D3d9SubmitQueue::D3d9SubmitQueue() {
    m_thread = std::thread([this] { thread_main(); });
    std::fprintf(stderr, "[d3d9] command stream started\n");
}

D3d9SubmitQueue::~D3d9SubmitQueue() {
    shutdown();
}

void D3d9SubmitQueue::enqueue_op(D3d9CsOp&& op) {
    std::lock_guard<std::mutex> lock(m_mu);
    if (!m_running.load(std::memory_order_acquire)) {
        return;
    }
    const bool wake = m_queue.empty();
    m_queue.push_back(std::move(op));
    ++m_submit_gen;
    if (wake) {
        m_cv.notify_one();
    }
}

void D3d9SubmitQueue::enqueue_draw(D3d9DrawSnapshot&& snap) {
    enqueue_op(D3d9CsOp{std::move(snap)});
}

void D3d9SubmitQueue::enqueue_clear(D3d9ClearSnapshot&& snap) {
    enqueue_op(D3d9CsOp{std::move(snap)});
}

void D3d9SubmitQueue::enqueue_color_fill(D3d9ColorFillSnapshot&& snap) {
    enqueue_op(D3d9CsOp{std::move(snap)});
}

void D3d9SubmitQueue::wait_until_idle() {
    std::unique_lock<std::mutex> lock(m_mu);
    while (m_running.load(std::memory_order_acquire) &&
           (m_busy || !m_queue.empty() || m_done_gen < m_submit_gen)) {
        if (pthread_main_np() != 0) {
            lock.unlock();
            CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.0, true);
            lock.lock();
            if (!m_busy && m_queue.empty() && m_done_gen >= m_submit_gen) {
                break;
            }
        }
        m_cv.wait_for(lock, std::chrono::milliseconds(1));
    }
}

void D3d9SubmitQueue::flush() {
    wait_until_idle();
}

void D3d9SubmitQueue::drain() {
    wait_until_idle();
}

void D3d9SubmitQueue::shutdown() {
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

void D3d9SubmitQueue::clear_pipeline_cache() {
    std::lock_guard<std::mutex> lock(m_mu);
    m_pipelines.clear();
}

graphics::BackendPipeline* D3d9SubmitQueue::cached_pipeline(u64 key) {
    std::lock_guard<std::mutex> lock(m_mu);
    const auto found = m_pipelines.find(key);
    if (found == m_pipelines.end()) {
        return nullptr;
    }
    return found->second.get();
}

void D3d9SubmitQueue::store_pipeline(
    u64 key, std::unique_ptr<graphics::BackendPipeline> pipeline) {
    std::lock_guard<std::mutex> lock(m_mu);
    m_pipelines[key] = std::move(pipeline);
}

std::vector<u32> D3d9SubmitQueue::take_completed_retains() {
    std::lock_guard<std::mutex> lock(m_mu);
    std::vector<u32> out;
    out.swap(m_completed_retains);
    return out;
}

void D3d9SubmitQueue::thread_main() {
    for (;;) {
        D3d9CsOp op;
        {
            std::unique_lock<std::mutex> lock(m_mu);
            m_cv.wait(lock, [&] {
                return !m_queue.empty() ||
                       !m_running.load(std::memory_order_acquire);
            });
            if (m_queue.empty()) {
                return;
            }
            op = std::move(m_queue.front());
            m_queue.erase(m_queue.begin());
            m_busy = true;
        }

        std::vector<u32> retained;
        if (auto* draw = std::get_if<D3d9DrawSnapshot>(&op)) {
            if (draw->manager != nullptr && draw->render_pass != nullptr) {
                draw->manager->execute_recorded_draw(*draw);
            }
            retained = std::move(draw->retained);
        } else if (auto* clear = std::get_if<D3d9ClearSnapshot>(&op)) {
            if (clear->manager != nullptr) {
                clear->manager->execute_recorded_clear(*clear);
            }
            retained = std::move(clear->retained);
        } else if (auto* fill = std::get_if<D3d9ColorFillSnapshot>(&op)) {
            if (fill->manager != nullptr) {
                fill->manager->execute_recorded_color_fill(*fill);
            }
            retained = std::move(fill->retained);
        }

        {
            std::lock_guard<std::mutex> lock(m_mu);
            m_completed_retains.insert(m_completed_retains.end(),
                                       retained.begin(), retained.end());
            m_busy = false;
            ++m_done_gen;
            m_cv.notify_all();
        }
    }
}

void D3D9Manager::execute_recorded_clear(D3d9ClearSnapshot& snap) {
    if (snap.pass_to_end) {
        (void)snap.pass_to_end->end();
        snap.pass_to_end.reset();
    }
    if ((snap.flags & D3DCLEAR_ZBUFFER) != 0 && snap.to_back_buffer &&
        snap.swap_chain != nullptr) {
        (void)snap.swap_chain->clear_depth(snap.depth);
    }
    if ((snap.flags & D3DCLEAR_TARGET) != 0) {
        if (snap.to_back_buffer) {
            if (snap.swap_chain != nullptr) {
                (void)snap.swap_chain->clear_back_buffer(snap.color);
            }
        }
        for (size_t slot = 0; slot < snap.rt_colors.size(); ++slot) {
            graphics::BackendTexture2D* target = snap.rt_colors[slot];
            if (target == nullptr) continue;
            const unsigned slice = snap.rt_color_slices[slot];
            if (target->desc().cube || target->desc().array_size > 1) {
                (void)target->clear_render_target_level(snap.color, 0, slice,
                                                        1);
            } else {
                (void)target->clear_render_target(snap.color);
            }
        }
    }
}

void D3D9Manager::execute_recorded_color_fill(D3d9ColorFillSnapshot& snap) {
    if (snap.pass_to_end) {
        (void)snap.pass_to_end->end();
        snap.pass_to_end.reset();
    }
    if (snap.backend != nullptr) {
        if (snap.backend->desc().cube || snap.backend->desc().array_size > 1) {
            (void)snap.backend->clear_render_target_level(
                snap.color, snap.mip_level, snap.slice, 1);
        } else {
            (void)snap.backend->clear_render_target(snap.color);
        }
    }
}

} // namespace vitreo::winapi
