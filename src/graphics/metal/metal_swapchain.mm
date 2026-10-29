// SPDX-License-Identifier: LGPL-2.1-only

/// @file metal_swapchain.mm
/// @brief CAMetalLayer swapchain — Objective-C++ implementation

#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#import <QuartzCore/CAShapeLayer.h>
#import <QuartzCore/CATransaction.h>
#import <AppKit/NSView.h>
#import <AppKit/NSWindow.h>
#import <AppKit/NSScreen.h>
#import <CoreFoundation/CoreFoundation.h>
#import <CoreGraphics/CoreGraphics.h>
#import <dispatch/dispatch.h>
#import <pthread.h>
#include <cstdlib>
#include "metal_swapchain.hpp"

#include "metal_submit_queue.hpp"
#include "metal_device.hpp"
#include "graphics/common/surface_presenter.hpp"
#include "graphics/common/present_rate.hpp"
#include "windowing/messages/input_latency.hpp"
#include <algorithm>
#include <chrono>
#include <cstdio>

using namespace vitreo;

namespace vitreo::graphics {
namespace {

void run_on_main_sync(dispatch_block_t block) {
    if (pthread_main_np() != 0) {
        block();
    } else {
        dispatch_sync(dispatch_get_main_queue(), block);
    }
}

} // namespace

namespace {

/// Display-sync override for diagnosing presentation tearing.
///
/// D3DPRESENT_INTERVAL_IMMEDIATE means "do not wait for vblank", and Vitreo
/// maps it straight to displaySyncEnabled=NO. That is a literal reading, but
/// Vitreo never gives a guest a true exclusive-fullscreen scanout: presentation
/// is always a composited CAMetalLayer. Presenting a half-drawn frame into a
/// compositor tears, and a torn frame reads as horizontal shearing whenever the
/// camera pans -- which no Windows user of the same title would see, because
/// modern Windows composites even "fullscreen" D3D9 titles through DWM.
///
/// VITREO_VSYNC=1 forces sync on, =0 forces it off, unset keeps the guest's
/// request. Diagnostic knob; see .docs/graphics-intel-vs-windows-oracle.md.
[[nodiscard]] bool resolve_vsync(bool requested) {
    const char* env = std::getenv("VITREO_VSYNC");
    if (env == nullptr || env[0] == '\0') return requested;
    return env[0] != '0';
}

} // namespace

bool metal_layer_supports_pixel_format(u32 pixel_format) {
    // CAMetalLayer.pixelFormat throws CAMetalLayerInvalid (an uncaught
    // NSException that takes the host process down) for anything but these.
    // Apple's documented set is the BGRA8 / RGBA16Float / 10-bit and XR
    // formats; RGBA8 (70, 71) is also accepted -- all measured on macOS 26
    // by assigning each MTLPixelFormat to a fresh layer.
    switch (static_cast<MTLPixelFormat>(pixel_format)) {
    case MTLPixelFormatBGRA8Unorm:
    case MTLPixelFormatBGRA8Unorm_sRGB:
    case MTLPixelFormatRGBA8Unorm:
    case MTLPixelFormatRGBA8Unorm_sRGB:
    case MTLPixelFormatRGBA16Float:
    case MTLPixelFormatRGB10A2Unorm:
    case MTLPixelFormatBGR10A2Unorm:
    case MTLPixelFormatBGR10_XR:
    case MTLPixelFormatBGR10_XR_sRGB:
    case MTLPixelFormatBGRA10_XR:
    case MTLPixelFormatBGRA10_XR_sRGB:
        return true;
    default:
        return false;
    }
}

MetalSwapchain::MetalSwapchain(const SwapchainDesc& desc)
    : m_vsync(desc.vsync)
    , m_width(desc.width)
    , m_height(desc.height)
    , m_pixel_format(desc.pixel_format) {
    m_acquire_queue = (__bridge_retained void*)dispatch_queue_create(
        "dev.vitreo.metal.drawable", DISPATCH_QUEUE_SERIAL);
    // Refuse, never throw: the caller sees an invalid swapchain.
    if (!metal_layer_supports_pixel_format(desc.pixel_format)) {
        std::fprintf(stderr,
                     "[swapchain] ERROR: MTLPixelFormat %u cannot back a "
                     "CAMetalLayer\n",
                     desc.pixel_format);
        return;
    }
    // CAMetalLayer silently drops a drawableSize past the device limit
    // (it reads back as 0x0) and every drawable is then empty.
    if (desc.width == 0 || desc.height == 0 || desc.width > 16384 ||
        desc.height > 16384) {
        std::fprintf(stderr,
                     "[swapchain] ERROR: drawable size %ux%u outside "
                     "1..16384\n",
                     desc.width, desc.height);
        return;
    }
    run_on_main_sync(^{
      @autoreleasepool {
        NSView* view = (__bridge NSView*)desc.ns_view;
        if (!view) {
            std::fprintf(stderr, "[swapchain] ERROR: null NSView\n");
            return;
        }

        id<MTLDevice> device = (__bridge id<MTLDevice>)desc.device;
        if (!device) {
            std::fprintf(stderr, "[swapchain] ERROR: null Metal device\n");
            return;
        }

        CAMetalLayer* layer = [CAMetalLayer layer];
        layer.device              = device;
        layer.pixelFormat         = static_cast<MTLPixelFormat>(desc.pixel_format);
        layer.framebufferOnly     = desc.framebuffer_only ? YES : NO;
        layer.maximumDrawableCount = desc.buffer_count;
        layer.drawableSize        = CGSizeMake(desc.width, desc.height);
        // Guest surfaces are addressed in logical pixels (e.g. 640x480). Keep
        // the layer's backing scale at 1 so drawableSize stays equal to the
        // guest mode and mouse mapping does not see a Retina-sized surface.
        layer.contentsScale       = 1.0;
        layer.displaySyncEnabled  = resolve_vsync(desc.vsync) ? YES : NO;
        // Learn DXGI_ALPHA_MODE_PREMULTIPLIED: colour is scaled by alpha and
        // composited with source-over. An opaque CAMetalLayer (the default)
        // plus a black host plate turns a transparent composition surface
        // into a solid black window — the same class of bug as creating a
        // WS_EX_LAYERED HWND opaque.
        layer.opaque              = desc.opaque ? YES : NO;
        // D3D9/DXGI 8-bit UNORM backbuffers are sRGB-encoded. The default
        // CAMetalLayer color space follows the display (often linear or P3),
        // so AppKit encodes those bytes again and the frame looks too dark.
        {
            CGColorSpaceRef srgb =
                CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
            layer.colorspace = srgb;
            if (srgb) CGColorSpaceRelease(srgb);
        }
        // Default linear; update_layer_frame switches to nearest only when
        // the aspect-fit scale is an exact integer (VITREO_INTEGER_SCALE=1).
        layer.magnificationFilter = kCAFilterLinear;
        layer.minificationFilter  = kCAFilterLinear;
        layer.actions             = @{ @"frame": [NSNull null],
                           @"bounds": [NSNull null],
                           @"position": [NSNull null] };

        [view setWantsLayer:YES];
        CALayer* host_layer = [view layer];
        // Reset / CreateDevice builds a new CAMetalLayer. A leftover layer
        // from the previous swapchain sits on top and presents as a black
        // (or stale-sized) plate.
        NSArray<CALayer*>* existing = [host_layer.sublayers copy];
        for (CALayer* sub in existing) {
            if ([sub isKindOfClass:[CAMetalLayer class]]) {
                [sub removeFromSuperlayer];
            }
        }
        // VtrMetalView is flipped (Win32 Y-down). Match the host layer so
        // sublayer frames share that space — otherwise aspect-fit Y origins
        // are interpreted from the bottom and mouse hit-tests skew vertically.
        host_layer.geometryFlipped = [view isFlipped];
        host_layer.opaque = desc.opaque ? YES : NO;
        host_layer.backgroundColor = desc.opaque
            ? CGColorGetConstantColor(kCGColorBlack)
            : CGColorGetConstantColor(kCGColorClear);
        host_layer.actions = @{ @"sublayers": [NSNull null],
                    @"backgroundColor": [NSNull null] };
        [host_layer addSublayer:layer];

        m_layer = (__bridge_retained void*)layer;

        // CAMetalLayer defaults to a zero frame. Aspect-fit into the host
        // view now; Present refreshes this again once AppKit has real bounds.
        {
            // A view with no NSWindow is a detached presentation target: the
            // out-of-process swapchain case, where CocoaBridge hands back an
            // orphan VtrMetalView because the HWND belongs to another process
            // and the presenter transports its pixels to the owner. AppKit
            // never lays such a view out, so the "Present refreshes this once
            // AppKit has real bounds" note below does not apply to it -- its
            // bounds keep whatever the orphan was constructed with (800x600)
            // and every frame is aspect-fit down into that. A 1280x800 client
            // client rendered into an 800x500 layer that way, which reaches
            // the owner as a black content area.
            //
            // An offscreen target has no layout authority, so the swapchain's
            // own size is the only meaningful bounds for it.
            if ([view window] == nil) {
                const NSRect want = NSMakeRect(0, 0,
                                               static_cast<CGFloat>(desc.width),
                                               static_cast<CGFloat>(desc.height));
                if (!NSEqualRects([view frame], want)) {
                    [view setFrame:want];
                }
            }
            const NSRect bounds = [view bounds];
            const PresentationRect fitted = aspect_fit_rect(
                desc.width, desc.height, bounds.size.width, bounds.size.height);
            layer.frame = CGRectMake(fitted.x, fitted.y,
                                     fitted.width, fitted.height);
            const bool integer = presentation_scale_is_integer(
                desc.width, desc.height, fitted);
            layer.magnificationFilter =
                integer ? kCAFilterNearest : kCAFilterLinear;
            layer.minificationFilter = layer.magnificationFilter;
            if (fitted.width >= 1.0 && fitted.height >= 1.0) {
                m_fitted_guest_w.store(desc.width, std::memory_order_relaxed);
                m_fitted_guest_h.store(desc.height, std::memory_order_relaxed);
                m_layer_frame_ready.store(true, std::memory_order_release);
            }
        }

        std::fprintf(stderr,
            "[swapchain] created %ux%u  fmt=%u  bufs=%u  vsync=%s  "
            "fb_only=%d  layer_frame=%.0fx%.0f view_bounds=%.0fx%.0f\n",
            desc.width, desc.height,
            desc.pixel_format, desc.buffer_count,
            resolve_vsync(desc.vsync) ? "on" : "off",
            desc.framebuffer_only ? 1 : 0,
            layer.frame.size.width, layer.frame.size.height,
            [view bounds].size.width, [view bounds].size.height);
      }
    });
}

MetalSwapchain::~MetalSwapchain() {
    // Present staging is owned per swapchain (DXGI: back buffers belong to the
    // swap chain). Release ours before the object goes away.
    MetalSubmitQueue::instance().forget_swapchain(this);
    if (m_acquire_queue) {
        dispatch_sync((__bridge dispatch_queue_t)m_acquire_queue, ^{});
    }
    run_on_main_sync(^{
      @autoreleasepool {
        if (m_current_drawable) {
            CFRelease(m_current_drawable);
            m_current_drawable = nullptr;
        }
        if (m_pending_drawable) {
            CFRelease(m_pending_drawable);
            m_pending_drawable = nullptr;
        }
        if (m_layer) {
            CAMetalLayer* layer = (__bridge CAMetalLayer*)m_layer;
            [layer removeFromSuperlayer];
            CFRelease(m_layer);
            m_layer = nullptr;
        }
      }
    });
    if (m_acquire_queue) {
        CFRelease(m_acquire_queue);
        m_acquire_queue = nullptr;
    }
}

void MetalSwapchain::apply_layer_frame_on_main() {
    CAMetalLayer* layer = (__bridge CAMetalLayer*)m_layer;
    if (!layer || !layer.superlayer) return;
    if (m_composition_mode) return;
    NSView* view = (NSView*)[layer.superlayer delegate];
    if (!view) return;
    const NSRect bounds = [view bounds];
    if (bounds.size.width < 1.0 || bounds.size.height < 1.0) return;
    const PresentationRect fitted = aspect_fit_rect(
        m_width, m_height, bounds.size.width, bounds.size.height);
    const CGRect frame = CGRectMake(fitted.x, fitted.y,
                                    fitted.width, fitted.height);
    const bool integer =
        presentation_scale_is_integer(m_width, m_height, fitted);
    const CALayerContentsFilter filter =
        integer ? kCAFilterNearest : kCAFilterLinear;
    const bool frame_changed = !CGRectEqualToRect(layer.frame, frame);
    const bool filter_changed = layer.magnificationFilter != filter;
    if (frame_changed || filter_changed) {
        [CATransaction begin];
        [CATransaction setDisableActions:YES];
        layer.frame = frame;
        layer.magnificationFilter = filter;
        layer.minificationFilter = filter;
        [CATransaction commit];
        static bool logged = false;
        if (!logged) {
            logged = true;
            std::fprintf(stderr,
                "[swapchain] layer_frame fitted to %.0fx%.0f+%.0f+%.0f "
                "(view %.0fx%.0f, guest %ux%u, filter=%s)\n",
                frame.size.width, frame.size.height, frame.origin.x,
                frame.origin.y, bounds.size.width, bounds.size.height,
                m_width, m_height, integer ? "nearest" : "linear");
        }
    }
    m_fitted_guest_w.store(m_width, std::memory_order_relaxed);
    m_fitted_guest_h.store(m_height, std::memory_order_relaxed);
    m_layer_frame_ready.store(true, std::memory_order_release);
}

void MetalSwapchain::queue_async_layer_refit() {
    // A vsync-off load loop can Present thousands of times per second.
    // Refitting every frame floods the main queue and the layer stays
    // correct after the first fit; a host resize / exclusive fullscreen
    // still applies within one 100 ms slice.
    const u64 now_ns = static_cast<u64>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
    const u64 last_ns = m_last_async_refit_ns.load(std::memory_order_relaxed);
    constexpr u64 kMinRefitNs = 100000000ull;
    if (last_ns != 0 && now_ns - last_ns < kMinRefitNs) {
        return;
    }
    bool expected = false;
    if (!m_layer_refit_queued.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel)) {
        return;
    }
    m_last_async_refit_ns.store(now_ns, std::memory_order_relaxed);
    dispatch_async(dispatch_get_main_queue(), ^{
      @autoreleasepool {
        apply_layer_frame_on_main();
        m_layer_refit_queued.store(false, std::memory_order_release);
      }
    });
}

void MetalSwapchain::update_layer_frame() {
    // AppKit geometry must be touched on the main thread. D3D9 Present often
    // arrives from the guest/JIT thread; DirectDraw's presenter already gated
    // on [NSThread isMainThread], which left D3D9's CAMetalLayer at {0,0,0,0}
    // over a black host layer (black window with Present still succeeding).
    //
    // Learn D3DPRESENT: a loading loop presents every frame with a static
    // window. dispatch_sync to AppKit on each Present parks the guest behind
    // CFRunLoop. After one successful fit, only a guest-mode change waits;
    // a host resize is applied asynchronously.
    if (m_composition_mode) return;
    if (m_layer_frame_ready.load(std::memory_order_acquire) &&
        m_fitted_guest_w.load(std::memory_order_relaxed) == m_width &&
        m_fitted_guest_h.load(std::memory_order_relaxed) == m_height) {
        // Guest mode is unchanged, but exclusive fullscreen / a host
        // resize still grows the NSView. Refit off the Present thread,
        // at most every 100 ms, so a vsync-off load loop cannot flood
        // the main queue.
        queue_async_layer_refit();
        return;
    }
    run_on_main_sync(^{
      @autoreleasepool {
        apply_layer_frame_on_main();
      }
    });
}

bool MetalSwapchain::configure_composition_layer(
    const BackendCompositionLayerDesc& desc) {
    if (m_layer == nullptr) return false;
    __block bool configured = false;
    run_on_main_sync(^{
      @autoreleasepool {
        CAMetalLayer* layer = (__bridge CAMetalLayer*)m_layer;
        if (layer == nil || layer.superlayer == nil) return;
        NSView* view = (NSView*)[layer.superlayer delegate];
        NSView* parent = [view superview];
        if (view != nil && parent != nil) {
            [parent addSubview:view
                    positioned:desc.above_window_children
                        ? NSWindowAbove
                        : NSWindowBelow
                    relativeTo:nil];
        }
        const auto& m = desc.transform;
        [CATransaction begin];
        [CATransaction setDisableActions:YES];
        m_composition_mode = true;
        layer.anchorPoint = CGPointMake(0.0, 0.0);
        layer.bounds = CGRectMake(0.0, 0.0, m_width, m_height);
        layer.position = CGPointZero;
        layer.affineTransform = CGAffineTransformMake(
            m[0], m[1], m[2], m[3], m[4], m[5]);
        layer.zPosition = desc.z_order;
        const CALayerContentsFilter filter =
            desc.interpolation == BackendFilter::Nearest
                ? kCAFilterNearest : kCAFilterLinear;
        layer.magnificationFilter = filter;
        layer.minificationFilter = filter;
        if (desc.has_clip_rect) {
            const auto& clip = desc.clip_rect;
            CAShapeLayer* mask = [CAShapeLayer layer];
            mask.frame = layer.bounds;
            const CGRect rect = CGRectMake(
                clip[0], clip[1],
                std::max(0.0f, clip[2] - clip[0]),
                std::max(0.0f, clip[3] - clip[1]));
            CGPathRef path = CGPathCreateWithRect(rect, nullptr);
            mask.path = path;
            CGPathRelease(path);
            mask.fillColor = CGColorGetConstantColor(kCGColorBlack);
            layer.mask = mask;
        } else {
            layer.mask = nil;
        }
        [CATransaction commit];
        configured = true;
      }
    });
    return configured;
}

void MetalSwapchain::start_drawable_acquire() {
    CAMetalLayer* layer = (__bridge CAMetalLayer*)m_layer;
    if (layer == nullptr) return;
    {
        std::lock_guard<std::mutex> lock(m_drawable_mu);
        // Already have one waiting, or already asking for one.
        if (m_pending_drawable != nullptr || m_acquire_in_flight) return;
        m_acquire_in_flight = true;
    }
    dispatch_queue_t queue = (__bridge dispatch_queue_t)m_acquire_queue;
    if (queue == nullptr) {
        queue = dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0);
    }
    dispatch_async(queue, ^{
        id<CAMetalDrawable> drawable = [layer nextDrawable];
        std::lock_guard<std::mutex> lock(m_drawable_mu);
        if (m_pending_drawable) {
            CFRelease(m_pending_drawable);
            m_pending_drawable = nullptr;
        }
        if (drawable) {
            m_pending_drawable = (__bridge_retained void*)drawable;
        }
        m_acquire_in_flight = false;
    });
}

void* MetalSwapchain::next_drawable() {
    @autoreleasepool {
        if (m_current_drawable) {
            CFRelease(m_current_drawable);
            m_current_drawable = nullptr;
        }

        CAMetalLayer* layer = (__bridge CAMetalLayer*)m_layer;
        if (!layer) {
            return nullptr;
        }

        auto take_pending = [this]() -> void* {
            std::lock_guard<std::mutex> lock(m_drawable_mu);
            void* pending = m_pending_drawable;
            m_pending_drawable = nullptr;
            return pending;
        };

        // nextDrawable waits for CA to recycle a drawable. That recycle needs
        // the main CFRunLoop. D3D9 Present often runs on the AppKit thread, so
        // blocking inside nextDrawable starves the guest. Acquire off-main.
        // INTERVAL_IMMEDIATE: drain pending sources (timeout 0) and skip the
        // frame if no drawable is ready. INTERVAL_ONE may wait one refresh.
        // True mailbox off the main thread: take a drawable ONLY if the async
        // acquisition has already produced one, otherwise start one and skip
        // this frame immediately. No semaphore wait, and no CFRunLoop spin
        // either -- the recycle is driven by the MAIN run loop, so draining
        // the worker's own run loop cannot produce a drawable and only burns
        // the time the guest is waiting on.
        if (pthread_main_np() == 0) {
            if (void* pending = take_pending()) {
                m_current_drawable = pending;
                return pending;
            }
            bool start = false;
            {
                std::lock_guard<std::mutex> lock(m_drawable_mu);
                if (!m_acquire_in_flight) {
                    m_acquire_in_flight = true;
                    start = true;
                }
            }
            if (start) {
                dispatch_queue_t queue =
                    (__bridge dispatch_queue_t)m_acquire_queue;
                if (queue == nullptr) {
                    queue = dispatch_get_global_queue(
                        QOS_CLASS_USER_INITIATED, 0);
                }
                dispatch_async(queue, ^{
                    id<CAMetalDrawable> drawable = [layer nextDrawable];
                    std::lock_guard<std::mutex> lock(m_drawable_mu);
                    if (m_pending_drawable) {
                        CFRelease(m_pending_drawable);
                        m_pending_drawable = nullptr;
                    }
                    if (drawable) {
                        m_pending_drawable =
                            (__bridge_retained void*)drawable;
                    }
                    m_acquire_in_flight = false;
                });
            }
            vitreo::graphics::note_drawable_skip();
            return nullptr;   // skipped frame; caller still returns D3D_OK
        }
        if (pthread_main_np() != 0) {
            if (void* pending = take_pending()) {
                m_current_drawable = pending;
                return pending;
            }

            bool start_acquire = false;
            {
                std::lock_guard<std::mutex> lock(m_drawable_mu);
                if (!m_acquire_in_flight) {
                    m_acquire_in_flight = true;
                    start_acquire = true;
                }
            }
            if (start_acquire) {
                dispatch_queue_t queue =
                    (__bridge dispatch_queue_t)m_acquire_queue;
                if (queue == nullptr) {
                    // Not Present-skip; scheduling only so guest CPU can
                    // compete during vsync-off load storms.
                    queue = dispatch_get_global_queue(
                        QOS_CLASS_USER_INITIATED, 0);
                }
                dispatch_async(queue, ^{
                    id<CAMetalDrawable> drawable = [layer nextDrawable];
                    std::lock_guard<std::mutex> lock(m_drawable_mu);
                    if (m_pending_drawable) {
                        CFRelease(m_pending_drawable);
                        m_pending_drawable = nullptr;
                    }
                    if (drawable) {
                        m_pending_drawable =
                            (__bridge_retained void*)drawable;
                    }
                    m_acquire_in_flight = false;
                });
            }

            // Drain already-queued sources so CA can recycle a drawable.
            // Timeout 0: do not park the guest here. Learn D3DPRESENT
            // INTERVAL_IMMEDIATE presents without waiting for refresh;
            // a missed drawable is a skipped frame (caller returns D3D_OK).
            CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0, true);
            if (void* pending = take_pending()) {
                m_current_drawable = pending;
                return pending;
            }
            if (layer.displaySyncEnabled) {
                // INTERVAL_ONE: block at most one refresh for the recycle.
                const CFTimeInterval deadline =
                    CFAbsoluteTimeGetCurrent() + 0.016;
                while (CFAbsoluteTimeGetCurrent() < deadline) {
                    if (void* pending = take_pending()) {
                        m_current_drawable = pending;
                        return pending;
                    }
                    CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.001, true);
                }
                if (void* pending = take_pending()) {
                    m_current_drawable = pending;
                    return pending;
                }
            }
            return nullptr;
        }

        // Off-main Present: INTERVAL_IMMEDIATE must not block in
        // nextDrawable (Learn D3DPRESENT_INTERVAL_IMMEDIATE). Take a
        // pending drawable or skip the frame. INTERVAL_ONE may wait.
        if (!layer.displaySyncEnabled) {
            if (void* pending = take_pending()) {
                m_current_drawable = pending;
                return pending;
            }
            bool start_acquire = false;
            {
                std::lock_guard<std::mutex> lock(m_drawable_mu);
                if (!m_acquire_in_flight) {
                    m_acquire_in_flight = true;
                    start_acquire = true;
                }
            }
            if (start_acquire) {
                dispatch_queue_t queue =
                    (__bridge dispatch_queue_t)m_acquire_queue;
                if (queue == nullptr) {
                    // Not Present-skip; scheduling only so guest CPU can
                    // compete during vsync-off load storms.
                    queue = dispatch_get_global_queue(
                        QOS_CLASS_USER_INITIATED, 0);
                }
                dispatch_async(queue, ^{
                    id<CAMetalDrawable> drawable = [layer nextDrawable];
                    std::lock_guard<std::mutex> lock(m_drawable_mu);
                    if (m_pending_drawable) {
                        CFRelease(m_pending_drawable);
                        m_pending_drawable = nullptr;
                    }
                    if (drawable) {
                        m_pending_drawable =
                            (__bridge_retained void*)drawable;
                    }
                    m_acquire_in_flight = false;
                });
            }
            return nullptr;
        }

        id<CAMetalDrawable> drawable = [layer nextDrawable];
        if (!drawable) {
            return nullptr;
        }

        m_current_drawable = (__bridge_retained void*)drawable;
        return m_current_drawable;
    }
}

void* MetalSwapchain::next_drawable_blocking() {
    @autoreleasepool {
        if (m_current_drawable) {
            CFRelease(m_current_drawable);
            m_current_drawable = nullptr;
        }
        CAMetalLayer* layer = (__bridge CAMetalLayer*)m_layer;
        if (!layer || pthread_main_np() != 0) {
            return next_drawable();
        }
        void* pending = nullptr;
        {
            std::lock_guard<std::mutex> lock(m_drawable_mu);
            pending = m_pending_drawable;
            m_pending_drawable = nullptr;
        }
        if (pending) {
            m_current_drawable = pending;
            return pending;
        }
        id<CAMetalDrawable> drawable = [layer nextDrawable];
        if (!drawable) {
            return nullptr;
        }
        m_current_drawable = (__bridge_retained void*)drawable;
        return m_current_drawable;
    }
}

void MetalSwapchain::request_layer_refit() {
    queue_async_layer_refit();
}

void MetalSwapchain::present(void* cmd_buf, u32 sync_interval) {
    @autoreleasepool {
        if (!m_current_drawable || !cmd_buf) {
            std::fprintf(stderr, "[swapchain] WARNING: present with null drawable or command buffer\n");
            return;
        }

        id<MTLCommandBuffer> buffer = (__bridge id<MTLCommandBuffer>)cmd_buf;
        id<CAMetalDrawable> drawable = (__bridge id<CAMetalDrawable>)m_current_drawable;

        // Cap presentation by RATE, not by the panel.
        //
        // Vsync means display refresh: a 60Hz iMac is a 60fps cap, a 240Hz
        // monitor is effectively none. Each present costs a full-screen
        // staging blit plus a submit, and in this guest the MAIN thread is
        // also the loader -- sampled on the 240Hz panel it sat 90% blocked
        // inside MetalSubmitQueue::submit, leaving ~10% of its time to make
        // load progress against ~78% at 60 presents/sec.
        //
        // afterMinimumDuration paces at the API level and still presents EVERY
        // frame. That distinction matters: skipping presents to cap the rate
        // was tried twice on this title and broke it both times (it flickered
        // once and hung the frame loop once), because the guest's loop
        // desynchronises when a Present does not land.
        //
        // The DEFAULT follows the display, it is not 60.
        //
        // A hardcoded 60 was a workaround for the guest described above, and
        // it silently became everyone's ceiling. On a 240 Hz panel that is not
        // just three quarters of the frames thrown away: afterMinimumDuration
        // asks CA to hold the drawable on screen for at least that long, so a
        // frame finished 4 ms after the last one still waits until 16.7 ms.
        // That is ~12 ms of added click-to-photon latency on every frame, on a
        // display fast enough to make it obvious.
        //
        // The cap's real job is to stop an unbounded present rate from
        // starving a guest whose main thread is also its loader; the panel's
        // own refresh is the honest bound for that. VITREO_PRESENT_MAX_FPS
        // still overrides, and 0 disables the cap entirely.
        static const double min_duration = [] {
            if (const char* env = std::getenv("VITREO_PRESENT_MAX_FPS")) {
                const double fps = std::atof(env);
                return fps > 0.0 ? 1.0 / fps : 0.0;
            }
            // Highest refresh among attached screens: a window can be dragged
            // to any of them, and capping a 240 Hz panel at a 60 Hz sibling's
            // rate is the bug this replaces.
            //
            // Queried DIRECTLY, never through run_on_main_sync. This runs
            // inside present(), which the guest calls from its own threads; a
            // main-thread hop here deadlocks whenever the main thread is
            // waiting on that guest. Trying it that way produced a run with
            // nine frames in six seconds. CocoaBridge::get_all_monitor_info
            // already reads NSScreen off-main for the same reason.
            double hz = 0.0;
            @autoreleasepool {
                for (NSScreen* screen in [NSScreen screens]) {
                    if ([screen respondsToSelector:
                                    @selector(maximumFramesPerSecond)]) {
                        hz = std::max(hz,
                            static_cast<double>([screen maximumFramesPerSecond]));
                    }
                }
            }
            if (hz <= 0.0) hz = 60.0;   // headless / pre-AppKit
            std::fprintf(stderr,
                         "[swapchain] present pacing: %.0f fps (display)\n", hz);
            return 1.0 / hz;
        }();
        // DXGI SyncInterval n (1..4): the frame stays up for at least n
        // refresh periods (Learn IDXGISwapChain::Present).
        const double interval_duration =
            min_duration * static_cast<double>(std::max<u32>(sync_interval, 1));
        if (m_vsync && interval_duration > 0.0 &&
            [buffer respondsToSelector:@selector(presentDrawable:
                                                 afterMinimumDuration:)]) {
            [buffer presentDrawable:drawable
                afterMinimumDuration:interval_duration];
        } else {
            [buffer presentDrawable:drawable];
        }

        // One frame reached the compositor. See present_rate.hpp: this is the
        // only place an out-of-process swapchain's frame rate is observable.
        vitreo::graphics::note_present(m_vsync);
        vitreo::graphics::note_present_size(
            static_cast<u32>(drawable.texture.width),
            static_cast<u32>(drawable.texture.height));
        // Stops the click-to-pixels clock started when the action was
        // dequeued: this frame is the guest's visible answer to it.
        (void)vitreo::windowing::input_latency::take_pending_action_age();

        // Start recycling the NEXT drawable now, while the guest is busy
        // producing frame N+1.
        //
        // next_drawable() is a mailbox that returns null — dropping the frame —
        // whenever nothing is already pending, and it only *started* an
        // acquisition at that point. So the steady state was
        // present, skip, present, skip: the guest asked, found nothing, kicked
        // off an acquire, and lost the frame; the drawable then sat ready until
        // the guest came back a whole frame later. That is exactly half rate,
        // and it is what put 71% of this app's frames in the 30 fps bucket
        // while they were being vsync-paced for 60.
        //
        // Priming here keeps the never-block-the-guest property that motivated
        // the mailbox (nextDrawable waits on the main CFRunLoop to recycle, so
        // blocking in it starves the guest) while removing the dropped frame.
        // maximumDrawableCount is the swapchain's buffer count, so holding one
        // pending plus one current still leaves the pool a spare.
        start_drawable_acquire();

        CFRelease(m_current_drawable);
        m_current_drawable = nullptr;
    }
}

void MetalSwapchain::resize(u32 width, u32 height) {
    m_layer_frame_ready.store(false, std::memory_order_release);
    run_on_main_sync(^{
      @autoreleasepool {
        m_width  = width;
        m_height = height;

        CAMetalLayer* layer = (__bridge CAMetalLayer*)m_layer;
        layer.drawableSize = CGSizeMake(width, height);

        std::fprintf(stderr, "[swapchain] resized to %ux%u\n", width, height);
      }
    });
    update_layer_frame();
}

void MetalSwapchain::set_color_space(u32 space) {
    run_on_main_sync(^{
      @autoreleasepool {
        CAMetalLayer* layer = (__bridge CAMetalLayer*)m_layer;
        if (!layer) return;
        // BackendColorSpace: 0 sRGB, 1 extended linear sRGB (scRGB),
        // 2 BT.2100 PQ, 3 BT.2020 with the sRGB curve.
        CFStringRef name = kCGColorSpaceSRGB;
        bool extended = false;
        switch (space) {
        case 1:
            name = kCGColorSpaceExtendedLinearSRGB;
            extended = true;
            break;
        case 2:
            name = kCGColorSpaceITUR_2100_PQ;
            extended = true;
            break;
        case 3:
            name = kCGColorSpaceITUR_2020_sRGBGamma;
            break;
        default:
            break;
        }
        CGColorSpaceRef color_space = CGColorSpaceCreateWithName(name);
        layer.colorspace = color_space;
        if (color_space) CGColorSpaceRelease(color_space);
        layer.wantsExtendedDynamicRangeContent = extended ? YES : NO;
      }
    });
}

void MetalSwapchain::set_hdr10_metadata(const std::vector<u8>& hdr10) {
    // DXGI_HDR_METADATA_HDR10 (dxgi1_5.h, little-endian): Red, Green, Blue
    // primaries and WhitePoint as UINT16 x/y in 0.00002 units, then UINT
    // MaxMasteringLuminance (nits) and MinMasteringLuminance (0.0001 nits),
    // then UINT16 MaxContentLightLevel and MaxFrameAverageLightLevel.
    // CAEDRMetadata takes the SEI payloads: the mastering display colour
    // volume (big-endian G, B, R primaries, white point, max and min
    // luminance in 0.0001 cd/m2) and the content light level.
    std::vector<u8> display;
    std::vector<u8> content;
    if (hdr10.size() == 28) {
        const auto u16_at = [&](usize offset) {
            return static_cast<u16>(hdr10[offset] | (hdr10[offset + 1] << 8));
        };
        const auto u32_at = [&](usize offset) {
            return static_cast<u32>(hdr10[offset]) |
                   (static_cast<u32>(hdr10[offset + 1]) << 8) |
                   (static_cast<u32>(hdr10[offset + 2]) << 16) |
                   (static_cast<u32>(hdr10[offset + 3]) << 24);
        };
        const auto put16 = [](std::vector<u8>& out, u16 value) {
            out.push_back(static_cast<u8>(value >> 8));
            out.push_back(static_cast<u8>(value));
        };
        const auto put32 = [](std::vector<u8>& out, u32 value) {
            out.push_back(static_cast<u8>(value >> 24));
            out.push_back(static_cast<u8>(value >> 16));
            out.push_back(static_cast<u8>(value >> 8));
            out.push_back(static_cast<u8>(value));
        };
        // Green, blue, red (SEI order) from red@0, green@4, blue@8.
        for (const usize primary : {usize{4}, usize{8}, usize{0}}) {
            put16(display, u16_at(primary));
            put16(display, u16_at(primary + 2));
        }
        put16(display, u16_at(12));
        put16(display, u16_at(14));
        const u64 max_nits = u32_at(16);
        put32(display, static_cast<u32>(std::min<u64>(max_nits * 10000u,
                                                       0xFFFFFFFFu)));
        put32(display, u32_at(20));
        put16(content, u16_at(24));
        put16(content, u16_at(26));
    }
    run_on_main_sync(^{
      @autoreleasepool {
        CAMetalLayer* layer = (__bridge CAMetalLayer*)m_layer;
        if (!layer) return;
        if (display.empty()) {
            layer.EDRMetadata = nil;
            return;
        }
        NSData* display_info = [NSData dataWithBytes:display.data()
                                              length:display.size()];
        NSData* content_info = [NSData dataWithBytes:content.data()
                                              length:content.size()];
        layer.EDRMetadata =
            [CAEDRMetadata HDR10MetadataWithDisplayInfo:display_info
                                            contentInfo:content_info
                                     opticalOutputScale:100.0f];
      }
    });
}

void MetalSwapchain::set_background_color(float r, float g, float b,
                                          float a) {
    run_on_main_sync(^{
      @autoreleasepool {
        CAMetalLayer* layer = (__bridge CAMetalLayer*)m_layer;
        CALayer* host = layer != nil ? layer.superlayer : nil;
        if (!host) return;
        CGColorSpaceRef srgb = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
        const CGFloat components[4] = {r, g, b, a};
        CGColorRef color = CGColorCreate(srgb, components);
        host.backgroundColor = color;
        if (color) CGColorRelease(color);
        if (srgb) CGColorSpaceRelease(srgb);
      }
    });
}

void MetalSwapchain::set_vsync(bool enabled) {
    if (m_vsync == enabled) {
        return;
    }
    m_vsync = enabled;
    run_on_main_sync(^{
      @autoreleasepool {
        CAMetalLayer* layer = (__bridge CAMetalLayer*)m_layer;
        if (layer) layer.displaySyncEnabled = resolve_vsync(enabled) ? YES : NO;
      }
    });
}

} // namespace vitreo::graphics
