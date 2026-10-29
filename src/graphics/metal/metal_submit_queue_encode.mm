// SPDX-License-Identifier: LGPL-2.1-only

/// @file metal_submit_queue_encode.mm
/// @brief The list encoder: encode_draws, compute and encoder flushing.

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

void MetalSubmitQueue::encode_compute(const CsCompute& compute) {
    if (!m_recorder || compute.pipeline == nullptr) {
        return;
    }
    close_open_pass();
    m_recorder->ensure_begun();
    m_recorder->begin_compute();
    m_recorder->set_compute_pipeline(compute.pipeline);
    for (u32 i = 0; i < compute.buffers.size(); ++i) {
        if (compute.buffers[i] == nullptr) {
            continue;
        }
        m_recorder->set_compute_buffer(compute.buffers[i],
                                       compute.buffer_off[i], i);
    }
    for (u32 i = 0; i < compute.textures.size(); ++i) {
        if (compute.textures[i] == nullptr) {
            continue;
        }
        m_recorder->set_compute_texture(compute.textures[i], i);
    }
    for (u32 i = 0; i < compute.samplers.size(); ++i) {
        if (compute.samplers[i] == nullptr) {
            continue;
        }
        m_recorder->set_compute_sampler(compute.samplers[i], i);
    }
    if (!compute.constants.empty()) {
        m_recorder->set_compute_bytes(compute.constants.data(),
                                      compute.constants.size(),
                                      BACKEND_INLINE_CONSTANT_BUFFER_INDEX);
    }
    for (unsigned set = 0; set < 4; ++set) {
        if (compute.descriptor_tables[set] == nullptr) continue;
        m_recorder->set_compute_buffer(
            compute.descriptor_tables[set], 0,
            BACKEND_DESCRIPTOR_TABLE_BUFFER_BASE + set);
    }
    m_recorder->use_compute_resources(compute.resident);
    if (compute.indirect_args != nullptr) {
        m_recorder->dispatch_indirect(compute.indirect_args,
                                      compute.indirect_offset, compute.tx,
                                      compute.ty, compute.tz);
    } else {
        m_recorder->dispatch(compute.gx, compute.gy, compute.gz, compute.tx,
                             compute.ty, compute.tz);
    }
    m_recorder->end_compute();
    note_encoder_closed();
    // VITREO_GPU_WATCH_SPLIT=1: one command buffer per dispatch, so a slow or
    // stuck command buffer reported by gpu_watch names exactly one shader.
    // Diagnostic only -- it costs a commit per dispatch.
    static const bool split = std::getenv("VITREO_GPU_WATCH_SPLIT") != nullptr;
    if (split) flush_encoders();
}

void MetalSubmitQueue::flush_encoders() {
    if (m_recorder && m_recorder->has_command_buffer()) {
        m_recorder->commit();
        m_list_committed = true;
    }
    m_encoders_since_commit = 0;
}

void MetalSubmitQueue::note_encoder_closed() {
    ++m_encoders_since_commit;
    // Parks open thousands of encoders per frame. A single command buffer
    // that large plus waitUntilCompleted crashed the guest on scene load.
    constexpr u32 kMaxEncodersPerCommit = 256;
    if (m_encoders_since_commit >= kMaxEncodersPerCommit) {
        flush_encoders();
    }
}

void MetalSubmitQueue::encode_draws(OpList& ops, u64 gpu_gen) {
    @autoreleasepool {
        if (!m_recorder) {
            m_recorder = std::make_unique<MetalCommandRecorder>(
                MetalDevice::instance().command_queue_ptr());
        }
        m_list_committed = false;
        // delta-record: every list starts from its first keyframe.
        m_delta_valid = false;
        const Op* present = nullptr;
        static const bool pass_break_diag =
            std::getenv("VITREO_METAL_PASS_BREAK_DIAG") != nullptr;
        // Counted here, published once after the list (begin_passes_encoded).
        u64 begins = 0;
        u64 ends = 0;
        for (Op& op : ops) {
            if (pass_break_diag) note_pass_break(op);
            switch (op.kind) {
            case OpKind::BeginPass:
                ++begins;
                note_live_begin(op.pass_id, *op.begin);
                if (can_continue_pass(*op.begin)) {
                    // Same attachments, loaded: carry on in the open encoder.
                    if (!m_open_pass_merged) {
                        const CsBeginPass& open = *m_open_begin;
                        u32 n = 0;
                        const auto add = [&](void* texture) {
                            if (texture == nullptr) return;
                            m_open_attachment_roots[n++] = texture;
                            void* root = texture_root(texture);
                            if (root != texture) {
                                m_open_attachment_roots[n++] = root;
                            }
                        };
                        for (u32 i = 0;
                             i < open.color_count && i < open.colors.size();
                             ++i) {
                            add(open.colors[i]);
                        }
                        add(open.depth);
                        m_open_attachment_root_count = n;
                        m_open_pass_merged = true;
                    }
                    m_open_pass = op.pass_id;
                    m_open_pass_ended = false;
                    m_open_begin = &*op.begin;
                    m_passes_continued.fetch_add(1, std::memory_order_relaxed);
                    if (op.begin->visibility_buffer != nullptr) {
                        m_recorder->set_visibility_counting(
                            op.begin->visibility_offset);
                    }
                    break;
                }
                if (m_open_pass != 0) {
                    close_open_pass();
                }
                encode_begin_pass(*op.begin);
                m_open_pass =
                    m_recorder && m_recorder->has_render_encoder() ? op.pass_id
                                                                   : 0;
                m_open_begin = m_open_pass != 0 ? &*op.begin : nullptr;
                // A cleared or discarded attachment is not what memory holds.
                m_open_depth_dirty = op.begin->clear_depth ||
                                     op.begin->clear_stencil ||
                                     !op.begin->preserve;
                break;
            case OpKind::Draw:
                // A full draw is state the DrawRuns after it do not know:
                // the next one starts from a keyframe (break_delta_chain).
                m_delta_valid = false;
                encode_draw_in_pass(*op.draw, op.pass_id);
                break;
            case OpKind::DrawRun: {
                const u8* record = op.run;
                const u8* const end = op.run + op.run_bytes;
                for (u32 i = 0; i < op.run_draws && record < end; ++i) {
                    const auto* head = reinterpret_cast<const DeltaHead*>(record);
                    if (!apply_delta(record, m_delta_state, m_delta_valid)) {
                        static std::atomic<bool> told{false};
                        if (!told.exchange(true)) {
                            std::fprintf(stderr,
                                         "[r3] ERROR: a delta draw with no "
                                         "keyframe before it was dropped\n");
                        }
                    } else {
                        if (delta_record::verify() &&
                            cs_draw_state_hash(m_delta_state) != head->hash) {
                            std::fprintf(stderr,
                                         "[r3] VERIFY FAILED: the applied "
                                         "delta draw state differs from the "
                                         "recorded draw (pass %llu)\n",
                                         static_cast<unsigned long long>(
                                             op.pass_id));
                            std::abort();
                        }
                        encode_draw_in_pass(m_delta_state, op.pass_id);
                    }
                    record += head->bytes;
                }
                break;
            }
            case OpKind::EndPass:
                ++ends;
                note_live_end(op.pass_id);
                if (op.pass_id != 0 && op.pass_id == m_open_pass) {
                    // VITREO_GPU_WATCH_SPLIT=1 also isolates each render
                    // pass, so a stuck buffer names one pass's draws.
                    const bool split = gpu_watch_split();
                    if (split || !pass_merge_enabled()) {
                        close_open_pass();
                        if (split) flush_encoders();
                    } else {
                        // Kept open for a following pass on the same
                        // attachments; any other op closes it.
                        m_open_pass_ended = true;
                    }
                }
                break;
            case OpKind::Resolve:
                close_open_pass();
                if (m_recorder && op.blit_src && op.blit_dst) {
                    m_recorder->resolve_msaa_texture(
                        op.blit_src, op.blit_dst, op.blit_src_level,
                        op.blit_src_slice, op.blit_dst_level,
                        op.blit_dst_slice);
                    note_encoder_closed();
                }
                break;
            case OpKind::BlitRegion:
                close_open_pass();
                if (m_recorder && op.blit_src && op.blit_dst) {
                    m_recorder->ensure_begun();
                    m_recorder->blit_copy_texture_region(
                        op.blit_src, op.blit_src_x, op.blit_src_y, op.blit_dst,
                        op.blit_dst_x, op.blit_dst_y, op.width, op.height,
                        op.blit_src_level, op.blit_src_slice,
                        op.blit_dst_level, op.blit_dst_slice, op.blit_depth,
                        op.blit_src_z, op.blit_dst_z);
                    note_encoder_closed();
                }
                break;
            case OpKind::DownloadTexture:
                close_open_pass();
                if (m_recorder && op.blit_src && op.blit_dst) {
                    m_recorder->ensure_begun();
                    m_recorder->blit_copy_texture_to_buffer(
                        op.blit_src, op.blit_src_level, op.blit_src_slice,
                        op.blit_src_x, op.blit_src_y, op.blit_src_z,
                        op.upload_width, op.upload_height, op.upload_depth,
                        op.blit_dst, op.upload_bytes_per_row,
                        op.upload_bytes_per_image, op.blit_dst_offset,
                        op.upload_options);
                    note_encoder_closed();
                }
                break;
            case OpKind::CopyBuffer:
                close_open_pass();
                if (m_recorder && op.blit_src && op.blit_dst &&
                    op.blit_size != 0) {
                    m_recorder->ensure_begun();
                    m_recorder->blit_copy_buffer(
                        op.blit_src, op.blit_src_offset, op.blit_dst,
                        op.blit_dst_offset, op.blit_size);
                    note_encoder_closed();
                }
                break;
            case OpKind::FillBuffer:
                close_open_pass();
                if (m_recorder && op.blit_dst && op.blit_size != 0) {
                    m_recorder->ensure_begun();
                    m_recorder->blit_fill_buffer(op.blit_dst,
                                                 op.blit_dst_offset,
                                                 op.blit_size, op.fill_value);
                    note_encoder_closed();
                }
                break;
            case OpKind::UploadTexture:
                close_open_pass();
                if (m_recorder && op.blit_src && op.blit_dst) {
                    m_recorder->ensure_begun();
                    m_recorder->blit_copy_buffer_to_texture(
                        op.blit_src, op.upload_bytes_per_row,
                        op.upload_bytes_per_image, op.upload_width,
                        op.upload_height, op.upload_depth, op.blit_dst,
                        op.blit_dst_level, op.blit_dst_slice, op.blit_dst_x,
                        op.blit_dst_y, op.upload_dst_z, op.upload_options,
                        op.blit_src_offset);
                    note_encoder_closed();
                }
                break;
            case OpKind::GenerateMips:
                close_open_pass();
                if (m_recorder && op.blit_src) {
                    m_recorder->ensure_begun();
                    m_recorder->blit_generate_mipmaps(op.blit_src);
                    note_encoder_closed();
                }
                break;
            case OpKind::Compute:
                encode_compute(*op.compute);
                break;
            case OpKind::Present:
                present = &op;
                break;
            case OpKind::SignalFence:
                // Close the open pass first: a signal means "everything recorded
                // before this is done", and an open pass is not done.
                // A fence op can be the first thing in a list; with no
                // command buffer begun the recorder dropped it silently.
                close_open_pass();
                m_recorder->ensure_begun();
                m_recorder->signal_event(op.fence_event, op.fence_value);
                break;
            case OpKind::WaitFence:
                close_open_pass();
                m_recorder->ensure_begun();
                m_recorder->wait_for_event(op.fence_event, op.fence_value);
                break;
            case OpKind::Timestamp:
                // A timestamp is "the GPU has finished everything before
                // this", so it ends the command buffer here and takes that
                // buffer's GPUEndTime. Command buffers on one queue retire in
                // order, and an empty one still reports a GPUEndTime after
                // its predecessors (measured), so a timestamp with no work
                // since the last split is ordered correctly too.
                close_open_pass();
                if (m_recorder && op.timestamp) {
                    m_recorder->ensure_begun();
                    m_recorder->commit_with_end_time(
                        [](void* context, double gpu_end_seconds) {
                            auto* cell = static_cast<
                                std::shared_ptr<MetalTimestampCell>*>(context);
                            (*cell)->gpu_ns.store(
                                static_cast<u64>(std::llround(
                                    gpu_end_seconds * 1e9)),
                                std::memory_order_relaxed);
                            (*cell)->done.store(true,
                                                std::memory_order_release);
                            delete cell;
                        },
                        new std::shared_ptr<MetalTimestampCell>(op.timestamp));
                    m_list_committed = true;
                    m_encoders_since_commit = 0;
                }
                break;
            case OpKind::DebugMarker:
                // Applied where it stands: inside an open pass it marks that
                // pass's encoder (MetalCommandRecorder mirrors open groups
                // onto every encoder and balances them), so a marker never
                // splits a pass.
                if (!m_recorder) break;
                switch (op.debug_marker) {
                case DebugMarker::Push:
                    m_recorder->push_debug_group(op.debug_label);
                    break;
                case DebugMarker::Pop:
                    m_recorder->pop_debug_group();
                    break;
                case DebugMarker::Signpost:
                    m_recorder->insert_debug_signpost(op.debug_label);
                    break;
                }
                break;
            case OpKind::TileMapping: {
                // The command buffer so far signals the shared event, the
                // Metal 4 queue waits for it and updates the mappings, and
                // the next command buffer waits for the update (Metal orders
                // events against command-buffer boundaries).
                close_open_pass();
                auto& tiled = MetalTiled::instance();
                if (m_recorder && op.tiles && tiled.available()) {
                    const u64 before = tiled.take_event_values();
                    m_recorder->ensure_begun();
                    m_recorder->signal_event(tiled.event(), before);
                    flush_encoders();
                    tiled.encode(op.tiles, before, before + 1, gpu_gen);
                    m_recorder->ensure_begun();
                    m_recorder->wait_for_event(tiled.event(), before + 1);
                }
                break;
            }
            case OpKind::TiledBarrier: {
                // A signal completes when everything before it has; a wait
                // holds everything after it: together, a full barrier.
                close_open_pass();
                auto& tiled = MetalTiled::instance();
                if (m_recorder && tiled.available()) {
                    const u64 value = tiled.take_event_values();
                    m_recorder->ensure_begun();
                    m_recorder->signal_event(tiled.event(), value);
                    m_recorder->wait_for_event(tiled.event(), value);
                }
                break;
            }
            case OpKind::Exit:
                break;
            }
        }
        close_open_pass();
        // The list's ops are released once it is encoded: a pass still open
        // (its EndPass is in a later list) keeps its descriptor here.
        if (!m_live_passes.empty()) [[unlikely]] carry_live_passes();
        if (!m_list_carries.empty()) [[unlikely]] {
            std::lock_guard<std::mutex> lock(m_carry_mu);
            m_carries.emplace_back(gpu_gen, std::move(m_list_carries));
            m_list_carries.clear();
            m_carry_lists.fetch_add(1, std::memory_order_release);
        }
        if (begins != 0) {
            m_begin_passes_encoded.fetch_add(begins, std::memory_order_relaxed);
        }
        if (ends != 0) {
            m_end_passes_encoded.fetch_add(ends, std::memory_order_relaxed);
        }
        if (present != nullptr && present->present_src != nullptr &&
            ensure_staging(present->swapchain, present->present_src,
                           present->width, present->height)) {
            m_recorder->ensure_begun();
            m_recorder->blit_copy_texture_region(
                present->present_src, present->present_src_x,
                present->present_src_y,
                m_staging_current->textures[m_staging_current->slot]->handle(),
                0, 0, present->width, present->height, 0, 0, 0, 0);
        }
        if (m_recorder && !m_recorder->has_command_buffer()) {
            // A generation with no newly encoded GPU commands can still
            // follow an earlier GPU-incomplete generation. Its completion
            // must be ordered on the Metal queue: advancing m_gpu_gen from
            // the CPU would make waits for the earlier generation succeed
            // before its IOSurface writes are visible in another process.
            // This also covers mid-frame flushes committed above.
            m_recorder->ensure_begun();
        }
        if (m_recorder && m_recorder->has_command_buffer()) {
            // Worker must not waitUntilCompleted here: a park-load CB is
            // huge, and blocking this thread on the GPU deadlocks Present
            // behind nextDrawable and has aborted the process.
            m_recorder->commit_with_handler(
                [](u64 gen) { MetalSubmitQueue::instance().list_completed(gen); },
                gpu_gen);
        } else {
            list_completed(gpu_gen);
        }
        m_encoders_since_commit = 0;
        m_list_committed = false;
        if (m_recorder) {
            m_recorder->recycle_constant_slabs_after_present();
        }
    }
}

} // namespace vitreo::graphics
