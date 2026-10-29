// SPDX-License-Identifier: LGPL-2.1-only

/// @file metal_submit_queue_record_pass.mm
/// @brief Render-pass and draw recording, with pass-fold at record time.

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

namespace {
void retain_begin_attachments(CsBeginPass& pass) {
    for (u32 i = 0; i < pass.color_count && i < pass.colors.size(); ++i) {
        if (pass.colors[i] != nullptr) CFRetain(pass.colors[i]);
    }
    if (pass.depth != nullptr) CFRetain(pass.depth);
    if (pass.resolve != nullptr) CFRetain(pass.resolve);
    if (pass.visibility_buffer != nullptr) CFRetain(pass.visibility_buffer);
}
}  // namespace

u64 MetalSubmitQueue::record_begin_pass(CsBeginPass pass) {
    if (appends_in_place()) {
        std::lock_guard<std::mutex> lock(m_mu);
        // Folded or not, the pass joins this recording (a fold candidate
        // never outlives the recording it was recorded in: submit settles
        // it).
        note_recorded_locked();
        const usize recorded = m_recording.size();
        // pass-continue: an owed EndPass is the candidate's, after a draw
        // (record_end_pass defers no other), so the trailing clears below
        // see a draw at the tail either way and fold nothing.
        fold_trailing_clears_locked(pass);
        // Folded clears were the tail, the candidate among them.
        if (m_recording.size() != recorded) fold_reset_locked();
        // pass-fold: the encoder would carry this pass on in the encoder of
        // the pass recorded last, so reopen that one instead. Its EndPass is
        // dropped; this pass's attachments are that pass's, which its own
        // BeginPass holds.
        const CsBeginPass* const open = m_fold.begin;
        if (m_fold.pass_id != 0 && m_fold.ended && !m_fold.wrote &&
            open != nullptr && pass.visibility_buffer == nullptr &&
            open->visibility_buffer == nullptr && pass_merge_enabled() &&
            !gpu_watch_split() && descriptors_continue(*open, pass) &&
            (m_fold.end_pending ||
             (!m_recording.empty() &&
              m_recording.back().kind == OpKind::EndPass &&
              m_recording.back().pass_id == m_fold.pass_id)) &&
            slim_record::active(slim_record::Part::PassFold)) {
            if (m_fold.end_pending) {
                // pass-continue: the EndPass was never recorded.
                m_fold.end_pending = false;
                if (delta_record::count_tick(++m_ends_elided)) {
                    std::fprintf(stderr,
                                 "[r3] pass-continue ends-elided=%llu "
                                 "materialized=%llu\n",
                                 static_cast<unsigned long long>(m_ends_elided),
                                 static_cast<unsigned long long>(
                                     m_ends_materialized));
                }
            } else {
                m_recording.pop_back();  // an EndPass holds no reference
            }
            if (!m_fold.reopened) {
                // draw_needs_own_encoder's roots: the attachments and the
                // textures they are views of.
                m_fold.root_count = 0;
                const auto add = [&](void* texture) {
                    if (texture == nullptr) return;
                    m_fold.roots[m_fold.root_count++] = texture;
                    void* const root = texture_root(texture);
                    if (root != texture) {
                        m_fold.roots[m_fold.root_count++] = root;
                    }
                };
                for (u32 i = 0; i < open->color_count && i < open->colors.size();
                     ++i) {
                    add(open->colors[i]);
                }
                add(open->depth);
            }
            m_fold.reopened = true;
            m_fold.ended = false;
            m_fold.replaced = std::move(pass);
            const u64 folded = ++m_passes_folded;
            if (slim_record::count_tick(folded)) {
                std::fprintf(stderr, "[wpr2] pass-fold folded=%llu split=%llu\n",
                             static_cast<unsigned long long>(folded),
                             static_cast<unsigned long long>(m_folds_split));
            }
            return m_fold.pass_id;
        }
        materialize_end_locked();
        // Retained under m_mu, unlike before pass-fold: whether to retain is
        // only known here, and retaining after unlocking would race a submit
        // on another thread handing the op to the encoder, which releases
        // it. A folded pass takes no retain at all; an unfolded one holds
        // the lock for its 1-6 CFRetains.
        retain_begin_attachments(pass);
        ++m_record_pass_id;
        if (m_record_pass_id == 0) {
            ++m_record_pass_id;
        }
        Op& op = m_recording.emplace_back();
        op.kind = OpKind::BeginPass;
        op.begin = std::move(pass);
        op.pass_id = m_record_pass_id;
        // The new candidate. A pass reopened before it needs no split: this
        // BeginPass ends that pass's encoder in the folded and the unfolded
        // stream alike (a pass that has not ended is never continued).
        fold_reset_locked();
        m_fold.pass_id = m_record_pass_id;
        m_fold.begin = &*op.begin;
        return m_record_pass_id;
    }
    retain_begin_attachments(pass);
    Op op{};
    op.kind = OpKind::BeginPass;
    op.begin = std::move(pass);
    u64 pass_id = 0;
    {
        std::lock_guard<std::mutex> lock(m_mu);
        ++m_record_pass_id;
        if (m_record_pass_id == 0) {
            ++m_record_pass_id;
        }
        pass_id = m_record_pass_id;
    }
    op.pass_id = pass_id;
    append(std::move(op));
    return pass_id;
}

void MetalSubmitQueue::fold_trailing_clears_locked(CsBeginPass& next) {
    // ClearRenderTargetView records a pass of its own (clear, store, no
    // draws), and the pass that then renders to that view loads it straight
    // back: on a tile GPU, a full store and a full load of the target for
    // nothing. One frame spent ~1 ms of GPU per 1440p frame in clear-only
    // passes. When the clear's EndPass is the last op recorded, nothing --
    // no draw, dispatch, copy, readback or present -- can observe the target
    // between the two, so clearing it as this pass's load action is the same
    // result. A run of such clears folds back to front; when one view was
    // cleared twice, the later clear is the one that reaches this pass.
    // VITREO_METAL_NO_CLEAR_FOLD=1 records them as separate passes again.
    static const bool off = std::getenv("VITREO_METAL_NO_CLEAR_FOLD") != nullptr;
    if (off || next.clear_color || !next.preserve) return;
    while (m_recording.size() >= 2) {
        Op& end = m_recording.back();
        Op& begin = m_recording[m_recording.size() - 2];
        if (end.kind != OpKind::EndPass || begin.kind != OpKind::BeginPass ||
            !begin.begin || end.pass_id != begin.pass_id) {
            return;
        }
        const CsBeginPass& clear = *begin.begin;
        if (!clear.clear_color || clear.color_count != 1 ||
            clear.colors[0] == nullptr || clear.depth != nullptr ||
            clear.resolve != nullptr || clear.visibility_buffer != nullptr ||
            clear.stencil || clear.clear_depth || clear.clear_stencil ||
            clear.center_samples || clear.default_width != 0 ||
            clear.color_clear_mask != 0 ||
            clear.render_target_array_length != next.render_target_array_length) {
            return;
        }
        u32 slot = static_cast<u32>(next.colors.size());
        for (u32 i = 0; i < next.color_count && i < next.colors.size(); ++i) {
            if (next.colors[i] == clear.colors[0] &&
                next.color_levels[i] == clear.color_levels[0] &&
                next.color_slices[i] == clear.color_slices[0] &&
                next.color_depth_planes[i] == clear.color_depth_planes[0]) {
                slot = i;
                break;
            }
        }
        if (slot == next.colors.size()) return;
        const u8 bit = static_cast<u8>(1u << slot);
        if ((next.color_clear_mask & bit) == 0) {
            next.color_clear_mask |= bit;
            next.color_clears[slot] = clear.color_clear;
        }
        release_op(end);
        release_op(begin);
        m_recording.pop_back();
        m_recording.pop_back();
        m_clears_folded.fetch_add(1, std::memory_order_relaxed);
    }
}

bool MetalSubmitQueue::appends_in_place() const noexcept {
    // What append() would do for these kinds, minus the stack Op: no capture
    // to divert to, and no shared resource for note_shared_access to note
    // (it returns at once when the registry is empty).
    return t_capture == nullptr && MetalSharedRegistry::instance().empty();
}

void MetalSubmitQueue::materialize_end_locked() {
    if (!m_fold.end_pending) return;
    m_fold.end_pending = false;
    Op& end = m_recording.emplace_back();
    end.kind = OpKind::EndPass;
    end.pass_id = m_fold.pass_id;
    ++m_ends_materialized;
}

void MetalSubmitQueue::fold_reset_locked() noexcept {
    // Callers record an owed EndPass first (materialize_end_locked); only a
    // recording dropped unencoded (shutdown) still has one here.
    m_fold.end_pending = false;
    m_fold.pass_id = 0;
    m_fold.begin = nullptr;
    m_fold.ended = false;
    m_fold.wrote = false;
    m_fold.reopened = false;
    m_fold.root_count = 0;
}

void MetalSubmitQueue::fold_split_locked(bool end_first) {
    if (end_first) {
        Op& end = m_recording.emplace_back();
        end.kind = OpKind::EndPass;
        end.pass_id = m_fold.pass_id;
    }
    // The BeginPass the fold left out, under the pass's id: the encoder
    // continues it exactly as it would have continued the original.
    CsBeginPass again = m_fold.replaced;
    retain_begin_attachments(again);
    Op& begin = m_recording.emplace_back();
    begin.kind = OpKind::BeginPass;
    begin.begin = std::move(again);
    begin.pass_id = m_fold.pass_id;
    m_fold.begin = &*begin.begin;
    m_fold.ended = false;
    m_fold.wrote = false;
    m_fold.reopened = false;
    ++m_folds_split;
}

void MetalSubmitQueue::fold_settle_locked() {
    materialize_end_locked();
    if (m_fold.pass_id != 0 && m_fold.reopened && !m_fold.ended) {
        fold_split_locked(/*end_first=*/true);
    }
    fold_reset_locked();
}

void MetalSubmitQueue::record_draw(CsDraw&& draw, u64 pass_id) {
    // ---- P0 state census (VITREO_D3D11_STATE_CENSUS=1, DIAGNOSTIC).
    if (g_draw_census) [[unlikely]] census_record_draw(draw);
    // ---- end P0 state census
    // Built in the list itself: an Op is ~3.4 KB, and a stack Op moved in
    // wrote every draw's bytes twice (three ops per draw).
    if (appends_in_place()) {
        std::lock_guard<std::mutex> lock(m_mu);
        note_recorded_locked();
        materialize_end_locked();
        break_delta_chain_locked();
        settle_push_locked(draw);
        if (m_fold.pass_id != 0 && pass_id == m_fold.pass_id) {
            if (m_fold.ended) {
                // A draw after its pass ended (a pass object refuses one, so
                // no frontend records this; kept exact regardless). Unfolded,
                // the encoder would meet it after the folded pass's EndPass:
                // in the continued encoder, ended. So when a pass was folded
                // in, record its BeginPass before the draw and an EndPass
                // after it; the encoder then continues, draws (restarting if
                // its own rule says so) and ends exactly as it would have.
                const bool reopened = m_fold.reopened;
                if (reopened) fold_split_locked(/*end_first=*/false);
                const bool writes = draw_writes(draw);
                Op& op = m_recording.emplace_back();
                op.kind = OpKind::Draw;
                op.draw = std::move(draw);
                op.pass_id = pass_id;
                if (!reopened) {
                    fold_reset_locked();  // the tail is no longer the pass's
                    return;
                }
                Op& end = m_recording.emplace_back();
                end.kind = OpKind::EndPass;
                end.pass_id = pass_id;
                // Still the candidate: BeginPass, a draw, EndPass.
                m_fold.ended = true;
                m_fold.wrote = writes;
                return;
            } else {
                if (m_fold.reopened && fold_draw_needs_split_locked(draw)) {
                    fold_split_locked(/*end_first=*/true);
                }
                if (draw_writes(draw)) m_fold.wrote = true;
            }
        } else {
            fold_settle_locked();
        }
        Op& op = m_recording.emplace_back();
        op.kind = OpKind::Draw;
        op.draw = std::move(draw);
        op.pass_id = pass_id;
        return;
    }
    // A captured list is replayed after this recording's arena is gone, and
    // the shared-resource path appends without it: the draw owns its bytes.
    settle_push_into_vectors(draw);
    Op op{};
    op.kind = OpKind::Draw;
    op.draw = std::move(draw);
    op.pass_id = pass_id;
    append(std::move(op));
}

void MetalSubmitQueue::record_end_pass(u64 pass_id) {
    if (appends_in_place()) {
        std::lock_guard<std::mutex> lock(m_mu);
        materialize_end_locked();
        if (m_fold.pass_id != 0 && pass_id == m_fold.pass_id &&
            !m_fold.ended) {
            m_fold.ended = true;
            // pass-continue: owe the EndPass of a pass that drew, so the
            // next pass can fold into it without recording and dropping it.
            // A pass with no draw keeps its EndPass in place: the trailing
            // clears fold reads it.
            const Op* tail = m_recording.empty() ? nullptr : &m_recording.back();
            if (tail != nullptr &&
                (tail->kind == OpKind::Draw || tail->kind == OpKind::DrawRun) &&
                tail->pass_id == pass_id &&
                delta_record::active(delta_record::Part::PassContinue)) {
                // Owed, not recorded: last_recorded_generation stays
                // that of the op this thread recorded last.
                m_fold.end_pending = true;
                return;
            }
        } else {
            fold_settle_locked();
        }
        Op& op = m_recording.emplace_back();
        op.kind = OpKind::EndPass;
        op.pass_id = pass_id;
        note_recorded_locked();
        return;
    }
    Op op{};
    op.kind = OpKind::EndPass;
    op.pass_id = pass_id;
    append(std::move(op));
}

} // namespace vitreo::graphics
