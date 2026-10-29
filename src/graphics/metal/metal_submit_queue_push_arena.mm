// SPDX-License-Identifier: LGPL-2.1-only

/// @file metal_submit_queue_push_arena.mm
/// @brief The recording arena: push-intern blocks and delta chunks.

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

const u8* MetalSubmitQueue::intern_push_locked(int stage, const u8* src,
                                               u32 size) {
    PushArena& arena = m_recording_arena;
    const usize s = static_cast<usize>(stage);
    // Consecutive draws very often carry the same block: share its copy.
    if (arena.last[s] != nullptr && arena.last_size[s] == size &&
        std::memcmp(arena.last[s], src, size) == 0) {
        ++m_push_blocks_shared;
        return arena.last[s];
    }
    const usize aligned = (static_cast<usize>(size) + 15u) & ~usize{15};
    if (aligned > PushArena::kChunkBytes) return nullptr;
    if (arena.chunks.empty() || arena.used + aligned > PushArena::kChunkBytes) {
        u8* chunk = nullptr;
        if (!m_free_push_chunks.empty()) {
            chunk = m_free_push_chunks.back();
            m_free_push_chunks.pop_back();
        } else {
            chunk = new u8[PushArena::kChunkBytes];
        }
        arena.chunks.push_back(chunk);
        arena.used = 0;
    }
    u8* const dst = arena.chunks.back() + arena.used;
    std::memcpy(dst, src, size);
    arena.used += aligned;
    arena.last[s] = dst;
    arena.last_size[s] = size;
    ++m_push_blocks_copied;
    return dst;
}

void MetalSubmitQueue::settle_push_into_vectors(CsDraw& draw) {
    if (!draw.push_pending) return;
    draw.vs_const.clear();
    draw.ps_const.clear();
    if (draw.vs_push != nullptr && draw.vs_push_size != 0) {
        draw.vs_const.assign(draw.vs_push, draw.vs_push + draw.vs_push_size);
    }
    if (draw.ps_push != nullptr && draw.ps_push_size != 0) {
        draw.ps_const.assign(draw.ps_push, draw.ps_push + draw.ps_push_size);
    }
    draw.vs_push = nullptr;
    draw.ps_push = nullptr;
    draw.vs_push_size = 0;
    draw.ps_push_size = 0;
    draw.push_pending = false;
}

void MetalSubmitQueue::settle_push_locked(CsDraw& draw) {
    if (!draw.push_pending) return;
    const u8* const vs =
        draw.vs_push_size != 0
            ? intern_push_locked(0, draw.vs_push, draw.vs_push_size)
            : nullptr;
    const u8* const ps =
        draw.ps_push_size != 0
            ? intern_push_locked(1, draw.ps_push, draw.ps_push_size)
            : nullptr;
    if ((draw.vs_push_size != 0 && vs == nullptr) ||
        (draw.ps_push_size != 0 && ps == nullptr)) {
        settle_push_into_vectors(draw);  // a block larger than a chunk
        return;
    }
    draw.vs_push = vs;
    draw.ps_push = ps;
    draw.push_pending = false;
    if (slim_record::count_tick(++m_push_draws)) {
        std::fprintf(
            stderr, "[wpr2] push-intern draws=%llu copied=%llu shared=%llu\n",
            static_cast<unsigned long long>(m_push_draws),
            static_cast<unsigned long long>(m_push_blocks_copied),
            static_cast<unsigned long long>(m_push_blocks_shared));
    }
}

void MetalSubmitQueue::recycle_push_arena_locked(PushArena& arena) noexcept {
    for (u8* chunk : arena.chunks) {
        if (m_free_push_chunks.size() < kMaxFreePushChunks) {
            m_free_push_chunks.push_back(chunk);
        } else {
            delete[] chunk;
        }
    }
    arena.chunks.clear();
    // delta-record: the same chunk size and free list.
    for (u8* chunk : arena.delta_chunks) {
        if (m_free_push_chunks.size() < kMaxFreePushChunks) {
            m_free_push_chunks.push_back(chunk);
        } else {
            delete[] chunk;
        }
    }
    arena.delta_chunks.clear();
    arena.delta_used = PushArena::kChunkBytes;
    // The encode thread releases a list's references outside the lock before
    // it gets here; what is left belongs to a recording dropped unencoded
    // (shutdown).
    for (void* handle : arena.releases) release_draw_handle(handle);
    arena.releases.clear();
    arena.used = PushArena::kChunkBytes;
    arena.last = {};
    arena.last_size = {};
}

void MetalSubmitQueue::append_delta_locked(const u8* bytes, usize size,
                                           u64 pass_id) {
    PushArena& arena = m_recording_arena;
    Op* tail = m_recording.empty() ? nullptr : &m_recording.back();
    const bool room = !arena.delta_chunks.empty() &&
                      arena.delta_used + size <= PushArena::kChunkBytes;
    const bool extend =
        room && tail != nullptr && tail->kind == OpKind::DrawRun &&
        tail->pass_id == pass_id &&
        tail->run + tail->run_bytes ==
            arena.delta_chunks.back() + arena.delta_used;
    if (!room) {
        u8* chunk = nullptr;
        if (!m_free_push_chunks.empty()) {
            chunk = m_free_push_chunks.back();
            m_free_push_chunks.pop_back();
        } else {
            chunk = new u8[PushArena::kChunkBytes];
        }
        arena.delta_chunks.push_back(chunk);
        arena.delta_used = 0;
    }
    u8* const dst = arena.delta_chunks.back() + arena.delta_used;
    std::memcpy(dst, bytes, size);
    arena.delta_used += size;
    if (extend) {
        tail->run_bytes += static_cast<u32>(size);
        ++tail->run_draws;
        return;
    }
    Op& op = m_recording.emplace_back();
    op.kind = OpKind::DrawRun;
    op.pass_id = pass_id;
    op.run = dst;
    op.run_bytes = static_cast<u32>(size);
    op.run_draws = 1;
    ++m_delta.runs;
}

} // namespace vitreo::graphics
