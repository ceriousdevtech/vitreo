// SPDX-License-Identifier: LGPL-2.1-only
/// @file d3d11_swapchain_present.cpp
/// @brief IDXGISwapChain::Present and IDXGISwapChain1::Present1: frame
///        presentation, present-parameter validation and the
///        VITREO_D3D11_DUMP_PRESENT source dump.
///        Split out of d3d11_swapchain.cpp as verbatim code motion.
#include "winapi/graphics/d3d11/d3d11.hpp"
#include "winapi/core/guest_com.hpp"
#include "winapi/graphics/d3d11/d3d11_caps_log.hpp"
#include "winapi/graphics/d3d11/d3d11_in_chain.hpp"
#include "winapi/graphics/d3d11/d3d11_internal.hpp"
#include "winapi/graphics/d3d11/d3d11_swapchain_internal.hpp"
#include "winapi/graphics/d3d11/d3d11_state_census.hpp"
#include "winapi/graphics/d3d11/d3d11_stdcall.hpp"
#include "winapi/graphics/common/gpu_event_signaler.hpp"
#include "winapi/graphics/common/shared_keyed_mutex.hpp"
#include "winapi/common/virtual_display_modes.hpp"

#include "graphics/contracts/backend_device.hpp"
#include "graphics/common/diagnostic_path.hpp"
#include "core/memory/guest_memory.hpp"
#include "winapi/graphics/dxgi/dxgi.hpp"
#include "winapi/core/marshaler.hpp"
#include <vitreo/diagnostics.hpp>
#include <vitreo/virtual_tsc.hpp>

#include <algorithm>
#include <cmath>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <unistd.h>
#include <cstring>
#include <limits>
#include <map>
#include <mutex>
#include <string>
#include <optional>
#include <unordered_map>
#include <thread>
#include <utility>
#include <vector>

namespace vitreo::winapi {

namespace {

// VITREO_D3D11_DUMP_PRESENT=<path> (%p = pid, %u = that call site's present
// count) with optional VITREO_D3D11_DUMP_PRESENT_AFTER=<n> (default 1): one
// binary PPM of the pre-present source per call site, at its first present
// numbered >= n. The frame oracle of .docs/bopl-battle-bringup.md; the local
// Metal swap-chain path has its own copy in metal_device.mm. Read once.
struct PresentSourceDumpConfig {
    bool set{false};
    std::string path;
    unsigned after{1};
};

const PresentSourceDumpConfig& present_source_dump_config() {
    static const PresentSourceDumpConfig config = [] {
        PresentSourceDumpConfig c;
        if (const char* path = std::getenv("VITREO_D3D11_DUMP_PRESENT")) {
            c.set = true;
            c.path = path;
        }
        if (const char* after =
                std::getenv("VITREO_D3D11_DUMP_PRESENT_AFTER")) {
            c.after = static_cast<unsigned>(std::strtoul(after, nullptr, 0));
        }
        return c;
    }();
    return config;
}

// The resolved path when a call site's `count`th present is due its one dump.
std::optional<std::string> due_present_source_dump(unsigned count,
                                                   bool dumped) {
    const PresentSourceDumpConfig& c = present_source_dump_config();
    if (dumped || !c.set || count < c.after) return std::nullopt;
    auto path = graphics::diagnostic_path(
        c.path, static_cast<unsigned long long>(::getpid()), count);
    if (!path || path->empty()) return std::nullopt;
    return path;
}

void write_present_source_ppm(const std::string& path,
                              const std::vector<u8>& rgba, u32 width,
                              u32 height, const char* what) {
    if (FILE* dump = std::fopen(path.c_str(), "wb")) {
        std::fprintf(dump, "P6\n%u %u\n255\n", width, height);
        for (usize offset = 0;
             offset + 3 < static_cast<usize>(width) * height * 4u;
             offset += 4) {
            std::fwrite(rgba.data() + offset, 3, 1, dump);
        }
        std::fclose(dump);
        d3d11_diag("Dumped %s source %s %ux%u", what, path.c_str(), width,
                   height);
    }
}

}  // namespace

bool D3D11Manager::present_swap_chain_frame(GuestAddr address,
                                            u32 sync_interval, u32 flags,
                                            bool rotate) {
    ++m_diag_frame;
    dump_render_targets_if_requested();
    auto swap_chain = m_swap_chains.find(address);
    if (swap_chain == m_swap_chains.end()) {
        return false;
    }
    if (!swap_chain->second.backend) {
        // CreateSwapChainForComposition is intentionally windowless. DXGI
        // does not require IDCompositionVisual::SetContent to precede
        // Present/Present1, so retain the latest valid frame until Commit
        // binds the chain to a composition target.
        if (!swap_chain->second.composition ||
            swap_chain->second.desc.output_window != 0) {
            return false;
        }
        swap_chain->second.pending_sync_interval = sync_interval;
        swap_chain->second.pending_present_flags = flags;
        swap_chain->second.pending_present = true;
        d3d11_diag(
            "Queue deferred composition present chain=0x%llx sync=%u "
            "flags=0x%x",
            static_cast<unsigned long long>(address), sync_interval, flags);
        return true;
    }
    graphics::BackendTexture2D* source = nullptr;
    auto texture = m_textures.find(swap_chain->second.back_buffer);
    if (texture != m_textures.end() && texture->second.backend &&
        texture->second.backend->valid()) {
        source = texture->second.backend.get();
    }
    // A multisampled (bitblt) back buffer is shown through its resolve.
    if (source != nullptr && swap_chain->second.msaa_resolve) {
        if (!m_backend.resolve_texture_2d(
                swap_chain->second.msaa_resolve.get(), 0, 0, source, 0, 0,
                source->desc().pixel_format)) {
            d3d11_diag("Present chain=0x%llx MSAA resolve failed",
                       static_cast<unsigned long long>(address));
            return false;
        }
        source = swap_chain->second.msaa_resolve.get();
    }
    // IDXGISwapChain2::SetSourceSize: only [0, 0, w, h] of the buffer is
    // presented (a zero size presents nothing).
    const u32 source_width = swap_chain->second.source_width;
    const u32 source_height = swap_chain->second.source_height;
    if (source_width == 0 || source_height == 0) {
        return !rotate ||
               texture == m_textures.end() ||
               advance_flip_back_buffer(swap_chain->second, texture->second);
    }
    const bool source_region =
        source != nullptr && (source_width < source->desc().width ||
                              source_height < source->desc().height);

    // Windows permits a device in one process to present to an HWND owned by
    // another. Vitreo maps guest processes to separate macOS host processes,
    // so a CAMetalLayer created here cannot be attached to the owner's
    // NSWindow. Read back only that foreign-HWND path and deliver the real
    // frame to the owner through the configured process transport.
    const u64 hwnd = swap_chain->second.desc.output_window;
    if (source != nullptr && m_foreign_window_resolver &&
        m_foreign_window_resolver(hwnd)) {
        // Learn DirectComposition: a composition swap chain becomes visible
        // once it is the content of a visual in a committed target tree, and
        // every later Present publishes the new back buffer regardless of what
        // was drawn into it. Deferral is handled by pending_present until the
        // chain is bound; the frame's content must not gate publication.
        // The owner presents a shared surface only as 32-bit BGRA (its
        // CAMetalLayer format, present_shared_window_surface); any other
        // back-buffer format goes through the pixel transport below.
        const u64 shared_handle =
            source->desc().pixel_format == graphics::PixelFormat::BGRA8Unorm
                ? source->shared_handle()
                : 0u;
        if (shared_handle != 0 && m_window_surface_presenter) {
            // IDXGISwapChain::Present queues the frame and returns; SyncInterval
            // 0 must not wait (Microsoft Learn). GPU writes still have to finish
            // before the HWND owner reads the IOSurface, so wait_idle runs on a
            // worker. Flip-model interval 0 also discards superseded frames —
            // keep only the latest surface per HWND.
            // With a two-buffer flip chain, buffer N can be recycled only
            // after the owner has consumed buffer N-1. The first call returns
            // immediately; later calls wait only when the finite DXGI ring is
            // full, matching flip-model back-pressure.
            // The owner presents the whole surface unless told otherwise, and
            // a DirectComposition clip is the only thing that scopes a pooled
            // chain to its content; carry it as a source rectangle.
            u32 present_w = std::min(source->desc().width, source_width);
            u32 present_h = std::min(source->desc().height, source_height);
            u32 src_x = 0;
            u32 src_y = 0;
            const auto& clip = swap_chain->second.composition_clip;
            if (clip.valid) {
                const float left = std::max(0.0f, clip.left);
                const float top = std::max(0.0f, clip.top);
                const float right = std::min(static_cast<float>(present_w), clip.right);
                const float bottom = std::min(static_cast<float>(present_h), clip.bottom);
                if (right > left && bottom > top) {
                    src_x = static_cast<u32>(left);
                    src_y = static_cast<u32>(top);
                    present_w = static_cast<u32>(right - left);
                    present_h = static_cast<u32>(bottom - top);
                }
            }
            wait_for_foreign_surface_reuse(shared_handle);
            enqueue_foreign_surface_present(
                hwnd, shared_handle, present_w, present_h, sync_interval,
                src_x, src_y);
            // Keep the producer/transport boundary observable without
            // replacing the zero-copy IOSurface path. A capture here answers
            // whether corruption was already present in the composition back
            // buffer or was introduced while the owner process presented it.
            static unsigned shared_present_count = 0;
            static bool shared_source_dumped = false;
            ++shared_present_count;
            if (const auto dump_path = due_present_source_dump(
                    shared_present_count, shared_source_dumped)) {
                std::vector<u8> rgba;
                if (swap_chain->second.backend->readback_texture_rgba(
                        source, rgba) &&
                    rgba.size() >= static_cast<usize>(source->desc().width) *
                                       source->desc().height * 4u) {
                    shared_source_dumped = true;
                    write_present_source_ppm(*dump_path, rgba,
                                             source->desc().width,
                                             source->desc().height,
                                             "shared pre-handoff");
                }
            }
            // Report the SURFACE extent and the rect actually presented.
            // Logging only the surface size cannot distinguish "this chain
            // had no clip" from "the clip covered the whole surface", which
            // is the exact question a white pooled-popup remainder asks.
            d3d11_diag(
                "Queue foreign surface HWND=0x%llx handle=0x%llx surface=%ux%u "
                "clip=%d present=%ux%u@%u,%u",
                static_cast<unsigned long long>(hwnd),
                static_cast<unsigned long long>(shared_handle),
                source->desc().width, source->desc().height,
                clip.valid ? 1 : 0, present_w, present_h, src_x, src_y);
            return !rotate || advance_flip_back_buffer(
                swap_chain->second, texture->second);
        }
        if (!m_window_pixels_presenter) return false;
        std::vector<u8> rgba;
        if (!swap_chain->second.backend->readback_texture_rgba(source, rgba)) {
            d3d11_diag(
                "Present foreign HWND=0x%llx readback failed",
                static_cast<unsigned long long>(hwnd));
            return false;
        }
        const u32 width = source->desc().width;
        const u32 height = source->desc().height;
        if (std::getenv("VITREO_D3D11_DIAG") != nullptr &&
            rgba.size() >= 4) {
            const std::array<u8, 4> first{
                rgba[0], rgba[1], rgba[2], rgba[3]};
            usize pixels_different = 0;
            for (usize offset = 4; offset + 3 < rgba.size(); offset += 4) {
                pixels_different +=
                    rgba[offset] != first[0] ||
                    rgba[offset + 1] != first[1] ||
                    rgba[offset + 2] != first[2] ||
                    rgba[offset + 3] != first[3];
            }
            d3d11_diag(
                "Present foreign pixels first=(%u,%u,%u,%u) "
                "different=%zu/%zu",
                first[0], first[1], first[2], first[3], pixels_different,
                rgba.size() / 4u);
        }
        // The local Metal swap-chain path owns an equivalent opt-in capture,
        // but foreign HWND presentation bypasses that path. Keep the same
        // diagnostic available here so cross-process compositing can be
        // inspected without changing normal presentation behavior.
        static bool foreign_source_dumped = false;
        static unsigned foreign_present_count = 0;
        ++foreign_present_count;
        if (const auto dump_path = due_present_source_dump(
                foreign_present_count, foreign_source_dumped);
            dump_path &&
            rgba.size() >= static_cast<usize>(width) * height * 4u) {
            foreign_source_dumped = true;
            write_present_source_ppm(*dump_path, rgba, width, height,
                                     "foreign pre-present");
        }
        const bool presented = m_window_pixels_presenter(
            hwnd, rgba, width, height);
        d3d11_diag(
            "Present foreign HWND=0x%llx %ux%u bytes=%zu -> %s",
            static_cast<unsigned long long>(hwnd), width, height, rgba.size(),
            presented ? "ok" : "failed");
        if (presented) {
            return !rotate || advance_flip_back_buffer(
                swap_chain->second, texture->second);
        }
        return false;
    }
    d3d11_diag("Present chain=0x%llx source=%p backbuf=0x%llx %ux%u "
               "samples=%u fmt=%u",
               static_cast<unsigned long long>(swap_chain->first),
               static_cast<const void*>(source),
               static_cast<unsigned long long>(swap_chain->second.back_buffer),
               source != nullptr ? source->desc().width : 0u,
               source != nullptr ? source->desc().height : 0u,
               source != nullptr ? source->desc().sample_count : 0u,
               source != nullptr
                   ? static_cast<unsigned>(source->desc().pixel_format)
                   : 0u);
    // VITREO_D3D11_PRESENT_TEST_PATTERN: overwrite the back buffer with a
    // solid colour immediately before presenting. Diagnostic only. A black
    // window has two very different causes — the guest rendered nothing, or
    // what it rendered never reaches the screen — and this splits them in one
    // run: if the colour appears, presentation works and the content is the
    // problem; if the window stays black, the presentation path is.
    // The switch is a *file*, not an environment variable: a guest launched
    // by a store client is several re-execs removed from the shell, and env
    // vars do not survive that reliably — which silently turned earlier
    // diagnostic runs into no-ops. A path anyone can touch always arrives.
    static const bool present_test_pattern = [] {
        if (std::getenv("VITREO_D3D11_PRESENT_TEST_PATTERN") != nullptr) {
            return true;
        }
        return ::access("/tmp/vitreo-present-test-pattern", F_OK) == 0;
    }();
    if (source != nullptr && present_test_pattern) {
        auto* mutable_source = const_cast<graphics::BackendTexture2D*>(source);
        const bool cleared = mutable_source->clear_render_target(
            {1.0f, 0.0f, 1.0f, 1.0f});
        d3d11_diag("Present test pattern -> %s",
                   cleared ? "cleared" : "failed");
    }
    const bool presented = source_region
        ? swap_chain->second.backend->present_texture_region(
              source,
              graphics::BackendSwapChain::PresentRegion{
                  0, 0, source_width, source_height},
              sync_interval, flags)
        : swap_chain->second.backend->present_texture(
              source, sync_interval, flags);
    if (presented && texture != m_textures.end()) {
        return !rotate || advance_flip_back_buffer(
            swap_chain->second, texture->second);
    }
    return presented;
}

u32 D3D11Manager::validate_present_parameters(const SwapChain& chain,
                                              GuestAddr params) const {
    // DXGI_PRESENT_PARAMETERS {UINT DirtyRectsCount; RECT* pDirtyRects;
    // RECT* pScrollRect; POINT* pScrollOffset;}. Windows dereferences a NULL
    // structure (measured: an access violation); here it means what an
    // all-zero one does -- the whole frame, nothing scrolled. A NULL rect
    // array with a count (also a Windows crash) is refused below.
    if (params == 0) return d3d11::S_OK;
    const usize ptr = m_is_64bit ? 8u : 4u;
    const auto count = m_memory.read_u32(params);
    GuestAddr dirty = 0;
    GuestAddr scroll = 0;
    GuestAddr offset = 0;
    if (count.is_err() || !read_pointer(params + ptr, dirty) ||
        !read_pointer(params + 2 * ptr, scroll) ||
        !read_pointer(params + 3 * ptr, offset)) {
        return DXGI_ERROR_INVALID_CALL;
    }
    if (count.value() == 0 && scroll == 0) return d3d11::S_OK;
    // Learn DXGI_PRESENT_PARAMETERS: dirty and scroll rectangles are not
    // supported for multisampled chains, scroll not for the bitblt effects;
    // measured, DISCARD and FLIP_DISCARD refuse dirty rectangles too (their
    // buffers are not preserved, so "only these pixels changed" cannot hold).
    if (chain.desc.sample_count > 1 ||
        (count.value() != 0 &&
         !dxgi_sequential_effect(chain.desc.swap_effect)) ||
        (scroll != 0 && !dxgi_flip_effect(chain.desc.swap_effect))) {
        return DXGI_ERROR_INVALID_CALL;
    }
    const auto rect_inside = [&](i32 left, i32 top, i32 right, i32 bottom) {
        return left >= 0 && top >= 0 && left <= right && top <= bottom &&
               static_cast<u32>(right) <= chain.desc.width &&
               static_cast<u32>(bottom) <= chain.desc.height;
    };
    const auto read_rect = [&](GuestAddr at, std::array<i32, 4>& rect) {
        return m_memory.read_bytes(at, reinterpret_cast<Byte*>(rect.data()),
                                   sizeof(rect)).is_ok();
    };
    if (count.value() != 0) {
        if (dirty == 0) return DXGI_ERROR_INVALID_CALL;
        for (u32 i = 0; i < count.value(); ++i) {
            std::array<i32, 4> rect{};
            if (!read_rect(dirty + i * 16u, rect) ||
                !rect_inside(rect[0], rect[1], rect[2], rect[3])) {
                return DXGI_ERROR_INVALID_CALL;
            }
        }
    }
    if (scroll != 0) {
        // The scroll rectangle is the destination; the source is it moved
        // back by the offset. Both must lie in the buffer (measured: a
        // 16x16 scroll by (4,4) from the origin reads outside it).
        std::array<i32, 4> rect{};
        std::array<i32, 2> delta{};
        if (!read_rect(scroll, rect) ||
            (offset != 0 &&
             m_memory.read_bytes(offset, reinterpret_cast<Byte*>(delta.data()),
                                 sizeof(delta)).is_err())) {
            return DXGI_ERROR_INVALID_CALL;
        }
        if (!rect_inside(rect[0], rect[1], rect[2], rect[3]) ||
            !rect_inside(rect[0] - delta[0], rect[1] - delta[1],
                         rect[2] - delta[0], rect[3] - delta[1])) {
            return DXGI_ERROR_INVALID_CALL;
        }
    }
    // The whole back buffer is presented: every pixel outside the dirty
    // rectangles is, by contract, unchanged from the previous frame (the
    // sequential and flip buffers are preserved), and a scrolled region is
    // already in the buffer the app drew.
    return d3d11::S_OK;
}

u32 D3D11Manager::present_swap_chain(GuestAddr address, u32 sync_interval,
                                     u32 flags) {
    // Learn (DXGI Overview, Debugging in Full-Screen Mode): "The occlusion
    // check is performed during Present" -- a chain whose application lost
    // focus relinquishes full-screen here.
    relinquish_fullscreen_if_inactive(address);
    auto found = m_swap_chains.find(address);
    if (found == m_swap_chains.end()) return DXGI_ERROR_INVALID_CALL;
    SwapChain& chain = found->second;
    const bool flip = dxgi_flip_effect(chain.desc.swap_effect);
    // Validation, measured on both models (dxgi_a11_contracts_probe
    // swapchain.methods, dxgi_a11_swapchain_rules_probe present.combos):
    //  - SyncInterval 0..4 (Learn: "1 through 4" vblanks);
    //  - STEREO_* need a stereo chain (none exist here), RESTRICT_TO_OUTPUT
    //    a restrict-to output, ALLOW_TEARING the chain flag and interval 0;
    //  - bits outside DXGI_PRESENT are ignored by a bitblt chain and refused
    //    by a flip chain, which also refuses TEST with any other flag and
    //    DO_NOT_SEQUENCE with RESTART or DO_NOT_WAIT;
    //  - a chain whose ResizeBuffers failed has no buffers to present.
    if (chain.buffers_lost || sync_interval > 4 ||
        (flags & (kPresentStereoPreferRight | kPresentStereoTemporaryMono)) !=
            0 ||
        ((flags & kPresentRestrictToOutput) != 0 &&
         chain.restrict_output == 0) ||
        ((flags & kPresentAllowTearing) != 0 &&
         ((chain.desc.flags & kSwapChainFlagAllowTearing) == 0 ||
          sync_interval != 0))) {
        return DXGI_ERROR_INVALID_CALL;
    }
    if (flip) {
        if ((flags & ~kPresentFlagsDefined) != 0 ||
            ((flags & kPresentTest) != 0 && flags != kPresentTest) ||
            ((flags & kPresentDoNotSequence) != 0 &&
             (flags & (kPresentRestart | kPresentDoNotWait)) != 0)) {
            return DXGI_ERROR_INVALID_CALL;
        }
    }
    // A window that did not exist at creation may have appeared since (a
    // peer process publishing its HWND late): attach to it then.
    if (chain.window_missing && window_exists(chain.desc.output_window) &&
        create_swap_chain_backend(chain)) {
        chain.window_missing = false;
    }
    // DXGI_PRESENT_TEST: "Do not present the frame to the output. The status
    // of the swap chain will be tested" -- nothing is shown or counted.
    // A full-screen chain that lost it to another application is occluded
    // until its application is foreground again ("Present ... will inform
    // you if your output window is entirely occluded via
    // DXGI_STATUS_OCCLUDED").
    // Only while it really is: once the host shows the window again (the
    // user came back to the Space) the frame is displayed even before the
    // guest's activation bookkeeping catches up.
    const bool occluded =
        chain.window_missing ||
        (chain.occluded_by_focus_loss &&
         (!m_window_services.window_occluded ||
          m_window_services.window_occluded(chain.desc.output_window)));
    if ((flags & kPresentTest) != 0) {
        return occluded ? d3d11::DXGI_STATUS_OCCLUDED : d3d11::S_OK;
    }
    // A present submits the device's work: shared resources it wrote are
    // published like at Flush.
    (void)publish_shared_writes();
    if (occluded) {
        // Nothing can be shown for an HWND that is not a window, but the
        // frame's commands are sent all the same (see the flush below).
        m_backend.flush();
        // Measured with every Present occluded: the flip model still counts
        // the present, the bitblt model does not.
        if (flip) {
            ++chain.present_count;
            chain.last_present_qpc = virtual_qpc_ticks();
        }
        return d3d11::DXGI_STATUS_OCCLUDED;
    }
    // DXGI_PRESENT_DO_NOT_WAIT: WAS_STILL_DRAWING instead of blocking when
    // the present queue is full.
    if ((flags & kPresentDoNotWait) != 0 && chain.backend &&
        chain.backend->present_would_block()) {
        return kDxgiErrorWasStillDrawing;
    }
    // DXGI_PRESENT_DO_NOT_SEQUENCE: "Present a frame from the current buffer
    // to the output", synchronised to the vblank, without sequencing the
    // buffers. The current buffer is the frame already on screen, so nothing
    // new is shown and the ring does not move; on the very first present
    // there is no current buffer and the runtime ignores the call (Learn).
    const bool sequence = (flags & kPresentDoNotSequence) == 0;
    if (!sequence) {
        m_backend.flush();  // the commands queued before it are sent
        if (chain.present_count == 0) return d3d11::S_OK;
        ++chain.present_count;
        chain.last_present_qpc = virtual_qpc_ticks();
        return d3d11::S_OK;
    }
    // DXGI_PRESENT_RESTART discards queued presents; this frontend never
    // holds more than the newest frame per chain (a newer interval-0 frame
    // replaces a queued one), so there is nothing further to discard.
    // DXGI_PRESENT_USE_DURATION applies an IDXGISwapChainMedia present
    // duration, which is never set here, so the frame presents normally.
    if (!present_swap_chain_frame(address, sync_interval, 0, true)) {
        return DXGI_ERROR_INVALID_CALL;
    }
    SwapChain& presented = m_swap_chains.at(address);
    // Learn (ID3D11DeviceContext::Flush): present commands are queued with
    // the rest, and the runtime sends them "whenever [it] determines that
    // they need to be sent"; a Present is such a point -- an application
    // polling an EVENT query with D3D11_ASYNC_GETDATA_DONOTFLUSH ("the
    // commands in the command buffer are not flushed") relies on its frame's
    // Present to send the work the query waits on. A chain with a backend
    // swap chain submitted with its present; a composition chain not yet
    // bound to a visual retains the frame and has sent nothing.
    if (!presented.backend) m_backend.flush();
    ++presented.present_count;
    presented.last_present_qpc = virtual_qpc_ticks();
    diagnostics::phase_note_present();
    // Learn Present remarks: a successful flip-model present unbinds back
    // buffer 0 from the pipeline (measured: the immediate context's render
    // target is gone; a bitblt chain's stays bound).
    if (flip) {
        auto device = m_devices.find(presented.device_owner);
        if (device != m_devices.end()) {
            unbind_texture_views(presented.back_buffer,
                                 device->second.context_interface, true);
        }
    }
    // FRAME_LATENCY_WAITABLE_OBJECT: the waitable object gains a frame when
    // this present's GPU work has retired, never beyond the latency.
    if (presented.latency && m_sync_services.release_semaphore) {
        auto latency = presented.latency;
        auto release = m_sync_services.release_semaphore;
        GpuEventSignaler::instance().run_after(
            m_backend.flush_completion(), [latency, release] {
                std::lock_guard<std::mutex> lock(latency->mutex);
                if (latency->alive && latency->handle != 0) {
                    release(latency->handle, 1,
                            static_cast<i32>(latency->cap));
                }
            });
    }
    return d3d11::S_OK;
}

} // namespace vitreo::winapi
