// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file metal_swapchain.hpp
/// @brief CAMetalLayer-based swapchain for window presentation

#include <vitreo/common.hpp>
#include <atomic>
#include <mutex>
#include <vector>

namespace vitreo::graphics {

struct BackendCompositionLayerDesc;

struct SwapchainDesc {
    void* ns_view = nullptr;     // NSView* to attach CAMetalLayer to
    void* device = nullptr;      // id<MTLDevice>
    u32 width = 0;
    u32 height = 0;
    u32 pixel_format = 80;       // MTLPixelFormatBGRA8Unorm = 80
    u32 buffer_count = 3;
    bool vsync = true;
    bool framebuffer_only = true;
    /// False for DXGI_ALPHA_MODE_PREMULTIPLIED / STRAIGHT composition.
    bool opaque = true;
};

/// True when a CAMetalLayer accepts @p pixel_format (an MTLPixelFormat); a
/// layer assigned any other format throws and aborts the process.
[[nodiscard]] bool metal_layer_supports_pixel_format(u32 pixel_format);

class MetalSwapchain {
public:
    explicit MetalSwapchain(const SwapchainDesc& desc);
    ~MetalSwapchain();

    /// Recompute the layer's aspect-fit frame from the host view bounds.
    /// Safe from any thread: the first successful fit (and a guest-mode
    /// resize) waits for AppKit. Later calls do not; a window resize is
    /// applied asynchronously so D3D9 Present cannot stall the guest.
    void update_layer_frame();

    /// Acquire the next drawable. Returns id<CAMetalDrawable> as void*.
    /// Safe to call off the main thread (does not touch view geometry). On the
    /// main thread this must not block inside nextDrawable: D3D9 Present runs
    /// on the AppKit thread, and a 1s drawable wait starves the guest load.
    /// Main-thread callers start acquire on a side queue. Learn D3DPRESENT:
    /// INTERVAL_IMMEDIATE must not wait for refresh — drain pending sources
    /// and skip the frame (nullptr) if no drawable is ready. INTERVAL_ONE
    /// may wait up to one refresh for the recycle.
    void* next_drawable();

    /// Kick off an asynchronous `nextDrawable` unless one is already in
    /// flight. Called at PRESENT time so the drawable for frame N+1 is being
    /// recycled while the guest renders it, rather than being requested only
    /// when the guest arrives and finding nothing ready.
    void start_drawable_acquire();

    /// Off-guest present thread: block in `nextDrawable` until CA has one.
    /// Must not run on the AppKit thread (that starves drawable recycle).
    void* next_drawable_blocking();

    /// Async layer refit only. Safe from the present thread, which must not
    /// dispatch_sync to main (the guest is often that thread).
    void request_layer_refit();

    /// Present the current drawable on the given command buffer.
    /// cmd_buf is id<MTLCommandBuffer>. A vsynced frame stays on screen for
    /// at least @p sync_interval refresh periods (DXGI SyncInterval 1..4).
    void present(void* cmd_buf, u32 sync_interval = 1);

    /// Tag the layer's content (BackendColorSpace numbering) and switch
    /// extended-dynamic-range on for the linear / PQ spaces.
    void set_color_space(u32 space);
    /// DXGI_HDR_METADATA_HDR10 bytes (28) as the layer's EDR metadata;
    /// anything else clears it.
    void set_hdr10_metadata(const std::vector<u8>& hdr10);
    /// Colour of the host view area the image does not cover.
    void set_background_color(float r, float g, float b, float a);

    /// Resize the drawable surface.
    void resize(u32 width, u32 height);
    void set_vsync(bool enabled);
    [[nodiscard]] bool configure_composition_layer(
        const BackendCompositionLayerDesc& desc);

    u32 width() const  { return m_width; }
    u32 height() const { return m_height; }
    u32 pixel_format() const { return m_pixel_format; }
    bool valid() const { return m_layer != nullptr; }

    MetalSwapchain(const MetalSwapchain&) = delete;
    MetalSwapchain& operator=(const MetalSwapchain&) = delete;

private:
    void* m_layer = nullptr;     // CAMetalLayer*
    void* m_current_drawable = nullptr; // id<CAMetalDrawable>
    void* m_acquire_queue = nullptr;    // dispatch_queue_t
    void* m_pending_drawable = nullptr; // id<CAMetalDrawable>, side-queue result
    std::mutex m_drawable_mu;
    bool m_acquire_in_flight = false;
    bool m_vsync = true;
    u32 m_width = 0;
    u32 m_height = 0;
    u32 m_pixel_format = 80;
    bool m_composition_mode = false;
    std::atomic<bool> m_layer_frame_ready{false};
    std::atomic<bool> m_layer_refit_queued{false};
    std::atomic<u32> m_fitted_guest_w{0};
    std::atomic<u32> m_fitted_guest_h{0};
    std::atomic<u64> m_last_async_refit_ns{0};

    void apply_layer_frame_on_main();
    void queue_async_layer_refit();
};

} // namespace vitreo::graphics
