// SPDX-License-Identifier: LGPL-2.1-only

/// @file gdi32.cpp
/// @brief GDI32 device context stubs (WIN32-063)

#include "winapi/graphics/gdi32/gdi32.hpp"
#include "winapi/graphics/gdi32/gdi32_internal.hpp"
#include "winapi/common/display_device_identity.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/core/marshaler.hpp"
#include "graphics/contracts/backend_device.hpp"
#include "core/memory/guest_memory.hpp"
#include "windowing/cocoa/cocoa_bridge.hpp"
#include "windowing/model/hwnd_manager.hpp"
#include "ipc/hwnd_registry.hpp"
#include "winapi/kernel32/memory/remote_memory.hpp"
#include <cmath>
#include <cstring>
#include <algorithm>
#include <array>
#include <cstdio>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include <unistd.h>

/// @file flush_window_framebuffer_cocoa.cpp
/// @brief Flush window framebuffer to Cocoa
///
/// Closed cut from gdi32.cpp — verified with plan_extraction.py.

namespace vitreo::winapi {

// =============================================================================
// Flush window framebuffer to Cocoa
// =============================================================================

void GdiManager::debug_report_dc_alpha(Handle hdc) {
    ApiLock api_lock(*this);
    auto it = m_dc_states.find(hdc);
    if (it == m_dc_states.end() || !it->second.bitmap) {
        std::fprintf(stderr, "[dcalpha] hdc=0x%x: no bitmap\n", (unsigned)hdc);
        return;
    }
    const SoftBitmap& b = *it->second.bitmap;
    usize a0 = 0, a255 = 0, amid = 0, rgb_nonzero = 0;
    for (usize i = 0; i + 3 < b.pixels.size(); i += 4) {
        const u8 a = b.pixels[i + 3];
        if (a == 0) ++a0; else if (a == 255) ++a255; else ++amid;
        if (b.pixels[i] || b.pixels[i+1] || b.pixels[i+2]) ++rgb_nonzero;
    }
    std::fprintf(stderr,
        "[dcalpha] hdc=0x%x bmp=0x%x %dx%d alpha0=%zu alpha255=%zu alphamid=%zu "
        "rgb_nonzero=%zu of %zu\n",
        (unsigned)hdc, (unsigned)it->second.selected_bitmap_handle,
        b.width, b.height, a0, a255, amid, rgb_nonzero, b.pixels.size() / 4);
}

void GdiManager::clear_window_framebuffer(u64 hwnd) {
    ApiLock api_lock(*this);
    auto it = m_window_framebuffers.find(hwnd);
    if (it == m_window_framebuffers.end() || !it->second) return;
    auto& fb = it->second;
    std::fill(fb->pixels.begin(), fb->pixels.end(), u8{0});
    fb->dirty = true;
}

void GdiManager::set_presentation_scheduler(
    std::function<void(std::function<void()>)> scheduler) {
    ApiLock lock(*this);
    m_presentation = std::make_shared<PresentationState>();
    m_presentation->owner = this;
    m_presentation->schedule = std::move(scheduler);
}

void GdiManager::stop_presentation() {
    // Called after guest execution stops, before the host bridge is destroyed.
    if (!m_presentation) return;
    std::lock_guard lifetime(m_presentation->lifetime_mutex);
    m_presentation->owner = nullptr;
    m_presentation->schedule = {};
}

void GdiManager::request_presentation_locked() const {
    if (!m_presentation || !m_presentation->schedule || m_presentation_pending) return;
    const bool dirty = std::any_of(m_window_framebuffers.begin(), m_window_framebuffers.end(),
        [](const auto& item) { return item.second && item.second->dirty; });
    if (!dirty) return;
    m_presentation_pending = true;
    std::weak_ptr<PresentationState> weak = m_presentation;
    m_presentation->schedule([weak] { present_scheduled(weak); });
}

void GdiManager::present_scheduled(std::weak_ptr<PresentationState> weak) {
    const auto state = weak.lock();
    if (!state) return;
    std::lock_guard lifetime(state->lifetime_mutex);
    auto* owner = state->owner;
    if (!owner) return;
    // Never block the host thread behind a worker that may need AppKit.
    ApiLock lock(*owner, std::try_to_lock);
    if (!lock) {
        state->schedule([weak] { present_scheduled(weak); });
        return;
    }
    owner->flush_pending_pixels();
    owner->m_presentation_pending = false;
    // The outer ApiLock rearms if drawing arrived while presentation had
    // released the GDI lock. Clean surfaces do not keep a timer running.
}

bool GdiManager::flush_pending_pixels() {
    ApiLock api_lock(*this);
    // GDI raster operations execute synchronously and report errors at their
    // call sites: there is no deferred guest command batch or deferred error.
    // Host presentation is separate from the calling thread's command batch.
    // This drains completed pixels; it never executes deferred guest calls.
    std::vector<u64> pending;
    for (const auto& [hwnd, bitmap] : m_window_framebuffers) {
        if (bitmap && bitmap->dirty) pending.push_back(hwnd);
    }
    // flush_window temporarily releases ApiLock around host presentation, so
    // iterate a snapshot rather than keeping map iterators across that call.
    for (const auto hwnd : pending) flush_window(hwnd);
    return true;
}

void GdiManager::flush_window(u64 hwnd) {
    ApiLock api_lock(*this);
    auto fb_it = m_window_framebuffers.find(hwnd);
    if (fb_it == m_window_framebuffers.end() || !fb_it->second) {
        VITREO_TRACE( "[gdi] flush_window: no framebuffer for hwnd=0x%llx\n", hwnd);
        return;
    }

    const auto live = fb_it->second;
    if (live->width <= 0 || live->height <= 0 || live->pixels.empty()) return;

    // Skip the (expensive, full-window) blit when no GDI drawing happened since
    // the last flush. dispatch_message flushes on every mouse/paint message; for
    // a DirectDraw title whose window framebuffer is never drawn to, that would
    // otherwise re-present an unchanged buffer on every pointer move.
    if (!live->dirty || m_presenting_windows.contains(hwnd)) return;
    // Only one snapshot of this window may be in flight. A newer draw stays
    // dirty until that submission finishes, so a delayed worker cannot paint
    // an old snapshot over a newer host tick. Never wait while holding GDI.
    m_presenting_windows.insert(hwnd);
    struct SubmissionGuard {
        std::unordered_set<u64>& active;
        u64 hwnd;
        ~SubmissionGuard() { active.erase(hwnd); }
    } submission{m_presenting_windows, hwnd};
    live->dirty = false;
    // Presentation owns its pixels while another independent DC may draw.
    const auto fb = std::make_shared<SoftBitmap>(*live);
    const auto bridge = m_cocoa_bridge;
    i32 send_w = fb->width;
    i32 send_h = fb->height;
    if (const auto size = m_window_client_size.find(hwnd); size != m_window_client_size.end()) {
        send_w = std::min(send_w, size->second.first);
        send_h = std::min(send_h, size->second.second);
    }
    ApiUnlock unlocked(*this);

    // VITREO_DUMP_WINDOW_FB=<path> writes the window framebuffer as a PPM on
    // each flush (overwriting), plus a content summary. %p expands to the host
    // process ID and %h to the hexadecimal HWND so multi-process renderers can
    // retain one dump per surface. This answers "is the surface blank, or is it
    // drawn but not presented?" — the question a blank window cannot otherwise
    // distinguish.
    {
        static const char* fb_dump = std::getenv("VITREO_DUMP_WINDOW_FB");
        if (fb_dump) {
            const SoftBitmap& b = *fb;
            usize nonwhite = 0, nonzero_alpha = 0;
            for (usize i = 0; i + 3 < b.pixels.size(); i += 4) {
                if (b.pixels[i] != 0xFF || b.pixels[i + 1] != 0xFF ||
                    b.pixels[i + 2] != 0xFF) ++nonwhite;
                if (b.pixels[i + 3] != 0) ++nonzero_alpha;
            }
            VITREO_TRACE("[gdi] flush_window hwnd=0x%llx %dx%d nonwhite=%zu/%zu"
                         " alpha!=0=%zu\n",
                         hwnd, b.width, b.height, nonwhite,
                         b.pixels.size() / 4, nonzero_alpha);
            std::string dump_path(fb_dump);
            const auto replace_token = [&dump_path](std::string_view token,
                                                     std::string_view value) {
                usize pos = 0;
                while ((pos = dump_path.find(token, pos)) != std::string::npos) {
                    dump_path.replace(pos, token.size(), value);
                    pos += value.size();
                }
            };
            replace_token("%p", std::to_string(::getpid()));
            char hwnd_text[32]{};
            std::snprintf(hwnd_text, sizeof(hwnd_text), "%llx",
                          static_cast<unsigned long long>(hwnd));
            replace_token("%h", hwnd_text);

            if (FILE* f = std::fopen(dump_path.c_str(), "wb")) {
                std::fprintf(f, "P6\n%d %d\n255\n", b.width, b.height);
                for (usize i = 0; i + 3 < b.pixels.size(); i += 4) {
                    const u8 rgb[3] = {b.pixels[i + 2], b.pixels[i + 1],
                                       b.pixels[i]};
                    std::fwrite(rgb, 3, 1, f);
                }
                std::fclose(f);
            }
            // The alpha plane as a PGM beside it: the overlay presents child
            // framebuffers premultiplied, so an opaque-looking pixel with A=0
            // composites away — invisible in the RGB dump.
            if (FILE* f = std::fopen((dump_path + ".alpha.pgm").c_str(), "wb")) {
                std::fprintf(f, "P5\n%d %d\n255\n", b.width, b.height);
                for (usize i = 0; i + 3 < b.pixels.size(); i += 4) {
                    std::fwrite(&b.pixels[i + 3], 1, 1, f);
                }
                std::fclose(f);
            }
        }
    }

    // Determine the actual client area to send (crop oversized framebuffers)

    const u8* send_pixels = fb->pixels.data();
    std::vector<u8> cropped;
    if (send_w != fb->width || send_h != fb->height) {
        const SoftBitmap& cfb = *fb;
        cropped.resize(static_cast<size_t>(send_w) * send_h * 4);
        for (i32 row = 0; row < send_h; ++row) {
            const u8* src_row = cfb.pixel_at(0, row);
            u8* dst_row = cropped.data() + row * send_w * 4;
            std::memcpy(dst_row, src_row, static_cast<size_t>(send_w) * 4);
        }
        send_pixels = cropped.data();
        VITREO_TRACE( "[gdi] flush_window hwnd=0x%llx %dx%d (cropped from %dx%d)\n",
                     hwnd, send_w, send_h, fb->width, fb->height);
    } else {
        VITREO_TRACE( "[gdi] flush_window hwnd=0x%llx %dx%d (direct)\n",
                     hwnd, send_w, send_h);
    }

    // Skin / billboard bring-up: confirm CreateDIBitmap→BitBlt left real
    // pixels without a full PPM dump.
    if (std::getenv("VITREO_GDI_DIAG")) {
        usize nonwhite = 0;
        const usize npx = static_cast<usize>(send_w) * static_cast<usize>(send_h);
        for (usize i = 0; i < npx; ++i) {
            const u8* p = send_pixels + i * 4;
            if (p[0] != 0xFF || p[1] != 0xFF || p[2] != 0xFF) ++nonwhite;
        }
        std::fprintf(stderr,
                     "[gdi-diag] flush_window hwnd=0x%llx %dx%d nonwhite=%zu/%zu\n",
                     static_cast<unsigned long long>(hwnd), send_w, send_h,
                     nonwhite, npx);
    }

    // Peer-owned HWND: ship pixels to the owning Vitreo host process
    // (StretchDIBits from a GPU process into the browser window).
    if (auto remote = ipc::hwnd_registry_lookup(static_cast<u32>(hwnd))) {
        if (remote->owner_host_pid != 0 &&
            remote->owner_host_pid != static_cast<u32>(::getpid())) {
            const bool ok = ipc::remote_present_window_pixels(
                static_cast<pid_t>(remote->owner_host_pid),
                static_cast<u32>(hwnd),
                std::span<const Byte>(send_pixels,
                                      static_cast<usize>(send_w) *
                                          static_cast<usize>(send_h) * 4u),
                send_w, send_h);
            VITREO_TRACE(
                "[gdi] flush_window remote hwnd=0x%llx -> pid=%u %dx%d %s\n",
                hwnd, remote->owner_host_pid, send_w, send_h,
                ok ? "ok" : "FAIL");
            if (std::getenv("VITREO_GDI_DIAG")) {
                std::fprintf(stderr,
                             "[gdi-diag] flush remote hwnd=0x%llx -> pid=%u "
                             "%dx%d %s\n",
                             hwnd, remote->owner_host_pid, send_w, send_h,
                             ok ? "ok" : "FAIL");
            }
            return;
        }
    }

    if (!bridge) {
        VITREO_TRACE( "[gdi] flush_window: no cocoa_bridge!\n");
        return;
    }
    bridge->update_window_pixels(
        static_cast<windowing::HWND>(hwnd), send_pixels, send_w, send_h);
}

} // namespace vitreo::winapi
