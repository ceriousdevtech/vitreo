// SPDX-License-Identifier: LGPL-2.1-only

/// @file metal_submit_queue_present.mm
/// @brief Present: recording, the per-chain staging textures and the
///        drawable blit.

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

void MetalSubmitQueue::record_present(MetalSwapchain* swapchain,
                                      void* src_texture, u32 width, u32 height,
                                      u32 sync_interval, u32 src_x,
                                      u32 src_y) {
    Op op{};
    op.kind = OpKind::Present;
    op.swapchain = swapchain;
    op.present_src = src_texture;
    op.present_src_x = src_x;
    op.present_src_y = src_y;
    op.width = width;
    op.height = height;
    op.vsync = sync_interval != 0;
    op.sync_interval = sync_interval;
    // VITREO_PRESENT_RATE also counts ATTEMPTS here. Completed presents alone
    // cannot tell "the guest only asked for 15 fps" from "the guest asked for
    // 60 and we dropped three quarters of them" -- and the drawable mailbox
    // does skip a frame when CoreAnimation has not recycled one. The ratio is
    // the answer.
    static const bool rate_on = std::getenv("VITREO_PRESENT_RATE") != nullptr;
    if (rate_on) {
        // [present-cpu]: CPU per frame of the presenting thread (this one --
        // the guest's own, the render thread) and of every named guest
        // thread, per [ab-cycle] arm (vitreo/present_cpu.hpp).
        diagnostics::present_cpu::note_present();
        static std::mutex amu;
        static u64 attempts = 0;
        static double last = 0.0;
        const double now = std::chrono::duration<double>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        std::lock_guard<std::mutex> lock(amu);
        ++attempts;
        if (last == 0.0) last = now;
        if (now - last >= 1.0) {
            std::fprintf(stderr, "[present-attempts] %.1f/s\n",
                         attempts / (now - last));
            attempts = 0;
            last = now;
        }
    }
    retain(op.present_src);
    append(std::move(op));
}

void MetalSubmitQueue::forget_swapchain(const MetalSwapchain* swapchain) {
    if (swapchain == nullptr) return;
    // Drain first: the worker may still be encoding a present that reads this
    // chain's staging textures.
    wait_until_present_idle();
    std::lock_guard<std::mutex> lock(m_mu);
    if (m_staging_current != nullptr) {
        const auto it = m_staging_by_chain.find(swapchain);
        if (it != m_staging_by_chain.end() && &it->second == m_staging_current) {
            m_staging_current = nullptr;
        }
    }
    m_staging_by_chain.erase(swapchain);
}

bool MetalSubmitQueue::ensure_staging(const MetalSwapchain* chain, void* src,
                                     u32 width, u32 height) {
    if (chain == nullptr || src == nullptr || width == 0 || height == 0) {
        return false;
    }
    id<MTLTexture> source = (__bridge id<MTLTexture>)src;
    if (source == nil) {
        return false;
    }
    const u32 format = static_cast<u32>(source.pixelFormat);

    // DXGI: the back buffers belong to the SWAP CHAIN (IDXGISwapChain
    // BufferCount), not to whatever presents them. Keying this cache on the
    // chain is that contract; keying it on the last presented size, as it used
    // to be, is what made a compositing UI reallocate two private MTLTextures
    // on nearly every present -- ~37 hosted child windows at different sizes
    // all funnel through this one queue, so consecutive presents rarely
    // matched.
    PresentStaging& staging = m_staging_by_chain[chain];
    m_staging_current = &staging;

    if (staging.textures[0] && staging.textures[1] &&
        staging.textures[0]->handle() != nullptr &&
        staging.textures[1]->handle() != nullptr &&
        staging.width == width && staging.height == height &&
        staging.format == format) {
        return true;
    }
    TextureDesc desc{};
    desc.width = width;
    desc.height = height;
    desc.pixel_format = format;
    desc.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
    desc.storage_mode = MTLStorageModePrivate;
    void* device = MetalDevice::instance().device_ptr();
    for (auto& texture : staging.textures) {
        texture = std::make_unique<MetalTexture>(device, desc);
        if (!texture || texture->handle() == nullptr) {
            staging.textures[0].reset();
            staging.textures[1].reset();
            return false;
        }
    }
    staging.width = width;
    staging.height = height;
    staging.format = format;
    staging.slot = 0;
    return true;
}

bool MetalSubmitQueue::present_from_list(OpList& ops) {
    const Op* present = nullptr;
    for (const Op& op : ops) {
        if (op.kind == OpKind::Present) {
            present = &op;
        }
    }
    if (present == nullptr || present->swapchain == nullptr ||
        present->present_src == nullptr) {
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(m_mu);
        ++m_presents_inflight;
        m_cv.notify_all();
    }
    @autoreleasepool {
        if (!m_recorder) {
            m_recorder = std::make_unique<MetalCommandRecorder>(
                MetalDevice::instance().command_queue_ptr());
        }
        if (!ensure_staging(present->swapchain, present->present_src,
                            present->width, present->height)) {
            return true;
        }
        PresentStaging& staging_set = *m_staging_current;
        void* staging = staging_set.textures[staging_set.slot]->handle();
        void* present_src = staging;
        u32 present_src_x{0};
        u32 present_src_y{0};
        auto& metal = MetalDevice::instance();
        if (metal.present_gamma_active() && metal.present_gamma_lut() != nullptr &&
            metal.present_gamma_compute_pso() != nullptr) {
            void* dest = staging_set.textures[staging_set.slot ^ 1u]->handle();
            m_recorder->ensure_begun();
            m_recorder->begin_compute();
            m_recorder->set_compute_pipeline(metal.present_gamma_compute_pso());
            m_recorder->set_compute_texture(staging, 0);
            m_recorder->set_compute_texture(dest, 1);
            m_recorder->set_compute_texture(metal.present_gamma_lut(), 2);
            constexpr u32 kTx = 16;
            constexpr u32 kTy = 16;
            const u32 gx = (present->width + kTx - 1u) / kTx;
            const u32 gy = (present->height + kTy - 1u) / kTy;
            m_recorder->dispatch(gx, gy, 1, kTx, kTy, 1);
            m_recorder->end_compute();
            present_src = dest;
        }
        staging_set.slot ^= 1u;
        // Ready Present still refits. INTERVAL_IMMEDIATE must not park this
        // worker in nextDrawable — that backs up submit() via presents_inflight
        // and turns a vsync-off load loop into a ~1.5 ms Present tax. A missed
        // drawable skips only the layer blit; draws and the staging copy already
        // ran. INTERVAL_ONE still waits. This is not Present-skip (that dropped
        // Draw/Clear for 8 ms and flickered).
        // VITREO_PRESENT_RATE=1: presents actually completed, per second,
        // total and for the busiest chain. This is the number that separates
        // "each surface is correctly capped at the display refresh" from
        // "N surfaces are sharing one refresh between them" -- the two look
        // identical from CPU usage alone, and only the second is a pacing bug.
        static const bool rate_on = std::getenv("VITREO_PRESENT_RATE") != nullptr;
        if (rate_on) {
            static std::mutex rate_mu;
            static std::unordered_map<const MetalSwapchain*, u64> per_chain;
            static std::unordered_map<const MetalSwapchain*, double> chain_last_ms;
            static u64 total = 0;
            static double last = 0.0;
            static double max_gap_ms = 0.0;
            static double max_gap_at_ms = 0.0;
            const double now_ms = diagnostics::present_cpu::host_mono_ms();
            const double now = now_ms / 1000.0;
            std::lock_guard<std::mutex> lock(rate_mu);
            ++per_chain[present->swapchain];
            ++total;
            // Longest interval between two presents of one chain in this
            // window, and the host time it ended: a long frame to correlate
            // with other meters' t_ms.
            double& chain_last = chain_last_ms[present->swapchain];
            if (chain_last != 0.0 && now_ms - chain_last > max_gap_ms) {
                max_gap_ms = now_ms - chain_last;
                max_gap_at_ms = now_ms;
            }
            chain_last = now_ms;
            if (last == 0.0) last = now;
            if (now - last >= 1.0) {
                u64 best = 0;
                for (const auto& [chain, n] : per_chain) best = std::max(best, n);
                std::fprintf(stderr,
                    "[present-rate] %.1f/s total over %zu chains, busiest %.1f/s "
                    "t_ms=%.1f max_gap_ms=%.2f max_gap_at_ms=%.1f\n",
                    static_cast<double>(total) / (now - last), per_chain.size(),
                    static_cast<double>(best) / (now - last), now_ms, max_gap_ms,
                    max_gap_at_ms);
                per_chain.clear();
                if (chain_last_ms.size() > 64) chain_last_ms.clear();
                total = 0;
                last = now;
                max_gap_ms = 0.0;
                max_gap_at_ms = 0.0;
            }
        }
        present->swapchain->request_layer_refit();
        // The submit WORKER never parks in drawable acquisition, whatever
        // interval the guest asked for.
        //
        // `next_drawable_blocking()` waits for CoreAnimation to recycle a
        // drawable, and that recycle is driven by the MAIN run loop at display
        // refresh. Parking the worker there backs up `submit()` through the
        // worker handoff, and `submit()` runs on the GUEST thread -- so a
        // guest that presents from the same thread it loads on gets throttled
        // to refresh rate. Sampling one load: the worker sat in
        // nextDrawable for 9,855 of 9,877 samples and the guest thread sat in
        // submit() for 92% of the window, while the actual translated work is
        // ~1.5 s. Skipping the park took the guest's blocked samples from
        // 9,565 to 434.
        //
        // Dropping the frame is the right thing and not a semantic loss:
        // `ensure_staging` above has ALREADY copied the backbuffer, so the
        // guest's content is safe and only the layer blit is skipped -- the
        // case the code below and `next_drawable()` were written for. A frame
        // that cannot get a drawable is a frame the compositor was never going
        // to display, because drawables are recycled at refresh rate.
        // INTERVAL_ONE promises at most one displayed frame per refresh, which
        // this still honours; what it stops promising is that Present STALLS
        // the caller; the presentation interval governs display cadence.
        //
        // The main-thread path keeps the blocking form: there CFRunLoop can
        // actually drive the recycle, so waiting is productive rather than
        // dead time.
        static const bool worker_may_park =
            std::getenv("VITREO_PRESENT_BLOCKING") != nullptr;
        const bool on_main = pthread_main_np() != 0;
        void* drawable = (present->vsync && (on_main || worker_may_park))
            ? present->swapchain->next_drawable_blocking()
            : present->swapchain->next_drawable();
        if (drawable) {
            m_recorder->ensure_begun();
            id<CAMetalDrawable> metal_drawable =
                (__bridge id<CAMetalDrawable>)drawable;
            const u32 dest_w = static_cast<u32>(metal_drawable.texture.width);
            const u32 dest_h = static_cast<u32>(metal_drawable.texture.height);
            const u32 blit_w = std::min(present->width, dest_w);
            const u32 blit_h = std::min(present->height, dest_h);
            if (blit_w > 0 && blit_h > 0) {
                m_recorder->blit_copy_texture(
                    present_src, (__bridge void*)metal_drawable.texture, blit_w,
                    blit_h);
                present->swapchain->present(m_recorder->command_buffer(),
                                            present->sync_interval);
                m_recorder->commit();
            }
        }
    }
    return true;
}

} // namespace vitreo::graphics
