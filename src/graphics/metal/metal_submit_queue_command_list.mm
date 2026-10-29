// SPDX-License-Identifier: LGPL-2.1-only

/// @file metal_submit_queue_command_list.mm
/// @brief D3D11 command-list capture and replay.

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

MetalSubmitQueue::CommandListId MetalSubmitQueue::create_command_list() {
    std::lock_guard<std::mutex> lock(m_mu);
    const CommandListId id = m_next_command_list++;
    if (m_next_command_list == 0) m_next_command_list = 1;
    m_command_lists.emplace(id, std::make_unique<std::vector<Op>>());
    return id;
}

void MetalSubmitQueue::destroy_command_list(CommandListId id) {
    std::unique_ptr<std::vector<Op>> owned;
    {
        std::lock_guard<std::mutex> lock(m_mu);
        const auto found = m_command_lists.find(id);
        if (found == m_command_lists.end()) return;
        if (t_capture == found->second.get()) {
            // Destroying the list this thread is capturing into would leave the
            // thread appending through a dangling pointer.
            t_capture = nullptr;
            t_capture_id = 0;
        }
        owned = std::move(found->second);
        m_command_lists.erase(found);
    }
    for (Op& op : *owned) {
        release_op(op);
    }
}

bool MetalSubmitQueue::begin_capture(CommandListId id) {
    if (t_capture != nullptr) return false;  // nesting is refused
    std::lock_guard<std::mutex> lock(m_mu);
    const auto found = m_command_lists.find(id);
    if (found == m_command_lists.end()) return false;
    t_capture = found->second.get();
    t_capture_id = id;
    return true;
}

bool MetalSubmitQueue::end_capture(CommandListId id) {
    if (t_capture == nullptr || t_capture_id != id) return false;
    t_capture = nullptr;
    t_capture_id = 0;
    return true;
}

bool MetalSubmitQueue::submit_command_list(CommandListId id) {
    std::vector<Op> copies;
    {
        std::lock_guard<std::mutex> lock(m_mu);
        const auto found = m_command_lists.find(id);
        if (found == m_command_lists.end()) return false;
        copies = *found->second;
    }
    // The list stays replayable, so every handle the copies reference gains a
    // reference of its own. D3D11 permits executing one command list repeatedly.
    for (Op& op : copies) {
        retain_op(op);
    }
    // The list's writes happen now, at execution, not when it was recorded.
    for (const Op& op : copies) {
        note_shared_access(op);
    }
    {
        std::lock_guard<std::mutex> lock(m_mu);
        fold_settle_locked();
        break_delta_chain_locked();
        m_recording.insert(m_recording.end(),
                           std::make_move_iterator(copies.begin()),
                           std::make_move_iterator(copies.end()));
        if (!copies.empty()) note_recorded_locked();
    }
    return true;
}

usize MetalSubmitQueue::command_list_size(CommandListId id) {
    std::lock_guard<std::mutex> lock(m_mu);
    const auto found = m_command_lists.find(id);
    return found == m_command_lists.end() ? 0u : found->second->size();
}

void MetalSubmitQueue::reset_command_list(CommandListId id) {
    std::vector<Op> discarded;
    {
        std::lock_guard<std::mutex> lock(m_mu);
        const auto found = m_command_lists.find(id);
        if (found == m_command_lists.end()) return;
        discarded = std::move(*found->second);
        found->second->clear();
    }
    for (Op& op : discarded) {
        release_op(op);
    }
}

} // namespace vitreo::graphics
