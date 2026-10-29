#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include "graphics/metal/metal_surface_presenter.hpp"

#include "graphics/contracts/cpu_surface.hpp"
#include "graphics/contracts/indexed_color_table.hpp"
#include "graphics/common/perf_diag.hpp"
#include "graphics/common/surface_presenter.hpp"
#include "graphics/metal/metal_device.hpp"
#include "graphics/metal/metal_gpu_watch.hpp"
#include "graphics/metal/metal_swapchain.hpp"
#include "graphics/metal/metal_texture.hpp"
#include "windowing/cocoa/cocoa_bridge.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace vitreo::graphics {
namespace {

constexpr u32 PIXEL_FORMAT_R8_UINT = 13;     // MTLPixelFormatR8Uint
constexpr u32 PIXEL_FORMAT_RGBA8_UNORM = 70; // MTLPixelFormatRGBA8Unorm
constexpr u32 PIXEL_FORMAT_BGRA8_UNORM = 80; // MTLPixelFormatBGRA8Unorm
constexpr u32 TEXTURE_USAGE_SHADER_READ = 0x01;
constexpr u32 STORAGE_MODE_SHARED = 0;

/// Palette resolution runs on the GPU: the guest's 8-bit indices upload
/// unmodified as an R8Uint texture and the 256-entry palette uploads as a
/// 256x1 RGBA8 texture. The fragment shader does the lookup per pixel. This
/// keeps palette animation, which some menus drive constantly, at a
/// 1 KB upload instead of re-expanding the whole framebuffer on the CPU.
///
/// `[[position]]` is in framebuffer pixels with a top-left origin, and the
/// drawable is sized to the guest surface, so the fragment coordinate indexes
/// the guest row/column directly. Aspect-fit scaling is applied by the layer
/// frame (nearest-filtered), not here.
constexpr char PRESENT_SHADER_SOURCE[] = R"(
#include <metal_stdlib>
using namespace metal;

struct VaryOut {
    float4 position [[position]];
};

vertex VaryOut vitreo_present_vs(uint vertex_id [[vertex_id]]) {
    const float2 corners[3] = {
        float2(-1.0, -1.0), float2(3.0, -1.0), float2(-1.0, 3.0)
    };
    VaryOut out;
    out.position = float4(corners[vertex_id], 0.0, 1.0);
    return out;
}

fragment float4 vitreo_present_direct_fs(
        VaryOut in [[stage_in]],
        texture2d<float, access::read> image [[texture(0)]]) {
    return float4(image.read(uint2(in.position.xy)).rgb, 1.0);
}

fragment float4 vitreo_present_fs(
        VaryOut in [[stage_in]],
        texture2d<uint, access::read> indices [[texture(0)]],
        texture2d<float, access::read> palette [[texture(1)]]) {
    const uint2 coord = uint2(in.position.xy);
    const uint index = indices.read(coord).r;
    return float4(palette.read(uint2(index, 0)).rgb, 1.0);
}
)";

/// Everything the present queue touches, owned by a shared_ptr so a queued or
/// chained present stays valid even if the presenter is destroyed underneath
/// it. The mutex covers the resources and the frame gate together: the guest
/// thread uploads and requests presents while the present queue consumes them.
struct PresentState {
    std::mutex mutex;
    std::unique_ptr<MetalSwapchain> swapchain;
    /// Two index textures so the guest can upload the next frame while the GPU
    /// still reads the previous one, without stalling and without dropping it.
    std::unique_ptr<MetalTexture> index_textures[2];
    std::unique_ptr<MetalTexture> palette_texture;
    void* pipeline{nullptr}; // id<MTLRenderPipelineState>, +1 retained
    u32 target{0};
    u32 width{0};
    u32 height{0};
    /// True while the resources describe a direct-colour (BGRA8) surface
    /// rather than the 8-bit indexed pair. A guest that switches display depth
    /// rebuilds, because the texture format and the pipeline both change.
    bool direct_color{false};
    bool in_flight{false};
    bool shutting_down{false};
    int flight_slot{-1};    // slot the GPU is currently reading, -1 if idle
    int ready_slot{-1};     // slot holding content not yet presented
    int resident_slot{-1};  // slot holding the most recent guest pixels

    // Telemetry only; all reads/writes happen under `mutex`.
    perf::TimePoint ready_time{};       // when ready_slot content was staged
    perf::TimePoint last_submit_time{}; // for the frame-interval histogram
    bool has_last_submit{false};

    ~PresentState() {
        if (pipeline) {
            CFRelease(pipeline);
            pipeline = nullptr;
        }
    }
};

void* build_present_pipeline(id<MTLDevice> device, bool direct_color) {
    @autoreleasepool {
        NSError* error = nil;
        id<MTLLibrary> library = [device
            newLibraryWithSource:@(PRESENT_SHADER_SOURCE)
                         options:nil
                           error:&error];
        if (!library) {
            std::fprintf(stderr, "[present] shader compile failed: %s\n",
                         error ? [[error localizedDescription] UTF8String]
                               : "unknown error");
            return nullptr;
        }

        MTLRenderPipelineDescriptor* descriptor =
            [[MTLRenderPipelineDescriptor alloc] init];
        descriptor.vertexFunction =
            [library newFunctionWithName:@"vitreo_present_vs"];
        descriptor.fragmentFunction = [library
            newFunctionWithName:direct_color ? @"vitreo_present_direct_fs"
                                             : @"vitreo_present_fs"];
        descriptor.colorAttachments[0].pixelFormat =
            static_cast<MTLPixelFormat>(PIXEL_FORMAT_BGRA8_UNORM);
        if (!descriptor.vertexFunction || !descriptor.fragmentFunction) {
            std::fprintf(stderr, "[present] shader entry points missing\n");
            return nullptr;
        }

        id<MTLRenderPipelineState> pipeline =
            [device newRenderPipelineStateWithDescriptor:descriptor
                                                   error:&error];
        if (!pipeline) {
            std::fprintf(stderr, "[present] pipeline creation failed: %s\n",
                         error ? [[error localizedDescription] UTF8String]
                               : "unknown error");
            return nullptr;
        }
        return (__bridge_retained void*)pipeline;
    }
}

class MetalSurfacePresenter final : public ISurfacePresenter {
public:
    explicit MetalSurfacePresenter(windowing::CocoaBridge& cocoa_bridge)
        : m_cocoa_bridge(cocoa_bridge)
        , m_state(std::make_shared<PresentState>()) {
        m_present_queue = dispatch_queue_create("dev.vitreo.present",
                                                DISPATCH_QUEUE_SERIAL);
    }

    ~MetalSurfacePresenter() override {
        {
            std::lock_guard<std::mutex> guard(m_state->mutex);
            m_state->shutting_down = true;
        }
        // Drain the queue so no block is mid-flight; the shared state itself
        // outlives us if the GPU still holds a completion handler.
        if (m_present_queue) {
            dispatch_sync(m_present_queue, ^{});
        }
    }

    void invalidate_surface(const CpuSurface* surface) noexcept override {
        // Drain in-flight presents before clearing the identity so a completion
        // handler cannot observe a destroyed CpuSurface via m_surface.
        if (m_present_queue) {
            dispatch_sync(m_present_queue, ^{});
        }
        std::lock_guard<std::mutex> guard(m_state->mutex);
        if (m_surface == surface) {
            m_surface = nullptr;
            m_surface_generation = 0;
            m_palette_generation = 0;
        }
    }

    bool present_indexed(u32 target, const CpuSurface& surface,
                         const u8* pixels, usize pixel_bytes) override {
        const auto& palette = surface.palette();
        const usize required =
            static_cast<usize>(surface.pitch()) * surface.height();
        // 8bpp goes through the palette pair; 32bpp is uploaded as BGRA8 and
        // sampled straight through. A DirectDraw primary at 32bpp used to be
        // rejected here, which left the host window blank however faithfully
        // the guest rendered and flipped.
        const bool direct_color = surface.bits_per_pixel() == 32;
        if (!pixels || pixel_bytes < required) return false;
        if (!direct_color && (surface.bits_per_pixel() != 8 || !palette)) {
            return false;
        }

        if (!ensure_resources(target, surface.width(), surface.height(),
                              direct_color)) {
            return false;
        }
        // Aspect-fit the layer to the host view. Reads AppKit geometry, so do
        // it only on the main thread (the guest primary surface is unlocked on
        // its main thread; a rare off-main present just keeps the last frame).
        if ([NSThread isMainThread]) {
            std::lock_guard<std::mutex> guard(m_state->mutex);
            if (m_state->swapchain) m_state->swapchain->update_layer_frame();
        }

        const u64 palette_generation = palette ? palette->generation() : 0;
        const bool pixels_changed = m_surface != &surface ||
                                    m_surface_generation != surface.generation();
        const bool palette_changed = m_surface != &surface ||
                                     m_palette_generation != palette_generation;
        if (!pixels_changed && !palette_changed) {
            perf::count(perf::Counter::present_nochange_skip);
            perf::maybe_report();
            return true;
        }

        if (direct_color) {
            static u32 fno = 0;
            if (++fno == 300) {
                // Sample the middle row, where terrain actually is.
                const usize row = static_cast<usize>(surface.height() / 2) *
                                  surface.pitch();
                std::fprintf(stderr, "[px] mid-row:");
                for (usize i = 0; i < 6; ++i) {
                    const u8* q = pixels + row + (surface.width() / 2 + i) * 4;
                    std::fprintf(stderr, " %02x%02x%02x%02x",
                                 q[0], q[1], q[2], q[3]);
                }
                std::fprintf(stderr, "\n");
            }
        }
        emit_diagnostics(surface, pixels, palette_generation);

        std::lock_guard<std::mutex> guard(m_state->mutex);
        if (m_state->shutting_down) return true;

        if (pixels_changed) {
            // Never upload into the slot the GPU is reading.
            const int slot = m_state->flight_slot == 0 ? 1 : 0;
            // The guest pitch is passed straight through as bytesPerRow, so the
            // indices upload with no CPU pass over the framebuffer at all.
            m_state->index_textures[slot]->upload(pixels, surface.pitch());
            m_state->resident_slot = slot;
            perf::count(perf::Counter::present_upload_full);
        }
        if (palette_changed && palette) {
            std::array<u8, IndexedColorTable::ENTRY_COUNT * 4> colors{};
            for (u32 index = 0; index < IndexedColorTable::ENTRY_COUNT; ++index) {
                const PaletteEntry* color = palette->entries(index, 1);
                colors[index * 4 + 0] = color->red;
                colors[index * 4 + 1] = color->green;
                colors[index * 4 + 2] = color->blue;
                colors[index * 4 + 3] = 0xFF;
            }
            m_state->palette_texture->upload(colors.data(), colors.size());
            perf::count(perf::Counter::present_upload_palette);
        }

        // A frame already staged but never submitted is about to be replaced.
        // That is the coalescing the design intends, but it is also the signal
        // that the guest is outrunning the display - worth counting separately.
        if (m_state->ready_slot >= 0) {
            perf::count(perf::Counter::present_coalesced);
        }

        // A palette-only change re-presents the pixels already resident on the
        // GPU: 1 KB of palette upload instead of re-expanding the framebuffer.
        m_state->ready_slot = m_state->resident_slot;
        m_state->ready_time = perf::now();

        m_surface = &surface;
        m_surface_generation = surface.generation();
        m_palette_generation = palette_generation;

        start_present_locked();
        perf::maybe_report();
        return true;
    }

private:
    /// Begin a present if the GPU is idle. If one is already in flight the
    /// frame stays in `ready_slot` and the completion handler picks it up, so
    /// the newest frame is always presented rather than dropped.
    void start_present_locked() {
        if (m_state->in_flight || m_state->ready_slot < 0 ||
            m_state->shutting_down) {
            return;
        }
        const int slot = m_state->ready_slot;
        m_state->in_flight = true;
        m_state->flight_slot = slot;
        m_state->ready_slot = -1;
        note_submit_locked(*m_state);

        auto state = m_state;
        dispatch_queue_t queue = m_present_queue;
        dispatch_async(queue, ^{
            present_slot(state, queue, slot);
        });
    }

    /// Records submit-side telemetry. Caller must hold `state.mutex`.
    static void note_submit_locked(PresentState& state) {
        if (!perf::enabled()) return;
        perf::count(perf::Counter::present_submit);
        perf::record(perf::Hist::present_update_to_submit,
                     perf::since(state.ready_time));
        const auto now = perf::now();
        if (state.has_last_submit) {
            perf::record(perf::Hist::frame_interval,
                         static_cast<u64>(
                             std::chrono::duration_cast<std::chrono::nanoseconds>(
                                 now - state.last_submit_time).count()));
        }
        state.last_submit_time = now;
        state.has_last_submit = true;
    }

    /// Acquire a drawable, draw the indexed surface through the palette shader
    /// and present - all on the background queue so the (potentially blocking)
    /// drawable acquisition never stalls the guest thread.
    static void present_slot(std::shared_ptr<PresentState> state,
                             dispatch_queue_t queue, int slot) {
        @autoreleasepool {
            MetalSwapchain* swapchain = nullptr;
            id<MTLTexture> indices = nil;
            id<MTLTexture> palette = nil;
            id<MTLRenderPipelineState> pipeline = nil;
            {
                std::lock_guard<std::mutex> guard(state->mutex);
                if (state->shutting_down || !state->swapchain ||
                    !state->index_textures[slot] || !state->palette_texture ||
                    !state->pipeline) {
                    state->in_flight = false;
                    state->flight_slot = -1;
                    return;
                }
                swapchain = state->swapchain.get();
                indices = (__bridge id<MTLTexture>)
                    state->index_textures[slot]->handle();
                palette = (__bridge id<MTLTexture>)
                    state->palette_texture->handle();
                pipeline = (__bridge id<MTLRenderPipelineState>)state->pipeline;
            }

            // Drawable acquisition is where vsync back-pressure actually lands.
            // Timing it separately is what distinguishes "the display is pacing
            // us" from "we are too slow to fill the interval".
            const auto acquire_start = perf::now();
            void* drawable_ptr = swapchain->next_drawable();
            perf::record(perf::Hist::present_drawable_wait,
                         perf::since(acquire_start));
            if (!drawable_ptr) {
                perf::count(perf::Counter::present_drawable_fail);
                finish_present(state, queue);
                return;
            }
            id<CAMetalDrawable> drawable =
                (__bridge id<CAMetalDrawable>)drawable_ptr;
            id<MTLCommandQueue> command_queue = (__bridge id<MTLCommandQueue>)
                MetalDevice::instance().command_queue_ptr();
            id<MTLCommandBuffer> command_buffer = [command_queue commandBuffer];

            MTLRenderPassDescriptor* pass =
                [MTLRenderPassDescriptor renderPassDescriptor];
            pass.colorAttachments[0].texture = drawable.texture;
            pass.colorAttachments[0].loadAction = MTLLoadActionDontCare;
            pass.colorAttachments[0].storeAction = MTLStoreActionStore;

            id<MTLRenderCommandEncoder> encoder =
                [command_buffer renderCommandEncoderWithDescriptor:pass];
            [encoder setRenderPipelineState:pipeline];
            [encoder setFragmentTexture:indices atIndex:0];
            [encoder setFragmentTexture:palette atIndex:1];
            [encoder drawPrimitives:MTLPrimitiveTypeTriangle
                        vertexStart:0
                        vertexCount:3];
            [encoder endEncoding];

            // Release the gate only once the GPU has finished reading the
            // index texture, so the guest can't overwrite the slot mid-draw.
            const auto commit_time = perf::now();
            [command_buffer addCompletedHandler:^(id<MTLCommandBuffer>) {
                perf::record(perf::Hist::present_submit_to_done,
                             perf::since(commit_time));
                finish_present(state, queue);
            }];
            swapchain->present((__bridge void*)command_buffer);
            gpu_watch::time_busy((__bridge void*)command_buffer);
            [command_buffer commit];
        }
    }

    /// Clear the gate and immediately present whatever the guest queued while
    /// the GPU was busy. Without this the final frame of a sequence is never
    /// shown: on a static screen the guest stops calling and the last update
    /// would sit un-presented indefinitely.
    static void finish_present(std::shared_ptr<PresentState> state,
                               dispatch_queue_t queue) {
        int next_slot = -1;
        {
            std::lock_guard<std::mutex> guard(state->mutex);
            state->in_flight = false;
            state->flight_slot = -1;
            if (state->shutting_down || state->ready_slot < 0) return;
            next_slot = state->ready_slot;
            state->ready_slot = -1;
            state->in_flight = true;
            state->flight_slot = next_slot;
            note_submit_locked(*state);
        }
        dispatch_async(queue, ^{
            present_slot(state, queue, next_slot);
        });
    }

    bool ensure_resources(u32 target, u32 width, u32 height,
                          bool direct_color) {
        if (target == 0 || width == 0 || height == 0) return false;
        {
            std::lock_guard<std::mutex> guard(m_state->mutex);
            if (m_state->target == target && m_state->width == width &&
                m_state->height == height && m_state->index_textures[0] &&
                m_state->direct_color == direct_color &&
                m_state->swapchain && m_state->pipeline) {
                return true;
            }
            // A resize while the GPU is mid-present would free textures it is
            // still reading. Skip this frame; the guest's next update rebuilds.
            if (m_state->in_flight) {
                perf::count(perf::Counter::present_resize_drop);
                return false;
            }
        }

        void* view = m_cocoa_bridge.native_presentation_view(target);
        if (!view) return false;

        id<MTLDevice> device =
            (__bridge id<MTLDevice>)MetalDevice::instance().device_ptr();

        TextureDesc index_desc{};
        index_desc.width = width;
        index_desc.height = height;
        // A direct-colour guest surface uploads as BGRA8 and is sampled
        // straight through; the indexed path keeps its R8 index plane.
        index_desc.pixel_format = direct_color ? PIXEL_FORMAT_BGRA8_UNORM
                                               : PIXEL_FORMAT_R8_UINT;
        index_desc.usage = TEXTURE_USAGE_SHADER_READ;
        index_desc.storage_mode = STORAGE_MODE_SHARED;
        std::unique_ptr<MetalTexture> index_textures[2];
        for (auto& texture : index_textures) {
            texture = std::make_unique<MetalTexture>(
                MetalDevice::instance().device_ptr(), index_desc);
            if (!texture->handle()) return false;
        }

        TextureDesc palette_desc{};
        palette_desc.width = IndexedColorTable::ENTRY_COUNT;
        palette_desc.height = 1;
        palette_desc.pixel_format = PIXEL_FORMAT_RGBA8_UNORM;
        palette_desc.usage = TEXTURE_USAGE_SHADER_READ;
        palette_desc.storage_mode = STORAGE_MODE_SHARED;
        auto palette_texture = std::make_unique<MetalTexture>(
            MetalDevice::instance().device_ptr(), palette_desc);
        if (!palette_texture->handle()) return false;

        void* pipeline = build_present_pipeline(device, direct_color);
        if (!pipeline) return false;

        SwapchainDesc swapchain_desc{};
        swapchain_desc.ns_view = view;
        swapchain_desc.device = MetalDevice::instance().device_ptr();
        swapchain_desc.width = width;
        swapchain_desc.height = height;
        swapchain_desc.pixel_format = PIXEL_FORMAT_BGRA8_UNORM;
        swapchain_desc.framebuffer_only = true;
        auto swapchain = std::make_unique<MetalSwapchain>(swapchain_desc);

        std::lock_guard<std::mutex> guard(m_state->mutex);
        if (m_state->pipeline) CFRelease(m_state->pipeline);
        m_state->target = target;
        m_state->direct_color = direct_color;
        m_state->width = width;
        m_state->height = height;
        m_state->index_textures[0] = std::move(index_textures[0]);
        m_state->index_textures[1] = std::move(index_textures[1]);
        m_state->palette_texture = std::move(palette_texture);
        m_state->pipeline = pipeline;
        m_state->swapchain = std::move(swapchain);
        m_state->ready_slot = -1;
        m_state->flight_slot = -1;
        m_state->resident_slot = -1;
        m_surface = nullptr;
        m_surface_generation = 0;
        m_palette_generation = 0;
        return true;
    }

    /// Opt-in tracing and frame capture. Both walk the whole surface, so they
    /// stay entirely off the normal present path.
    ///
    /// The pixel histogram is deliberately NOT on VITREO_TRACE. It is an O(w*h)
    /// pass plus a 256-entry palette walk, and running it per present made every
    /// traced run unrepresentative of the untraced one - which is exactly the
    /// measurement it was there to inform. It now needs its own opt-in and is
    /// sampled at most once a second even then.
    void emit_diagnostics(const CpuSurface& surface, const u8* pixels,
                          u64 palette_generation) {
        static const bool pixel_diag_on =
            std::getenv("VITREO_PRESENT_PIXEL_DIAG") != nullptr;
        bool scan_this_frame = false;
        if (pixel_diag_on) {
            using Clock = std::chrono::steady_clock;
            static Clock::time_point last_scan{};
            const auto now = Clock::now();
            constexpr auto period = std::chrono::seconds(1);
            if (now - last_scan >= period) {
                last_scan = now;
                scan_this_frame = true;
            }
        }
        if (scan_this_frame) {
            std::array<bool, IndexedColorTable::ENTRY_COUNT> used{};
            usize nonzero_pixels = 0;
            u8 minimum_index = 0xFF;
            u8 maximum_index = 0;
            for (u32 y = 0; y < surface.height(); ++y) {
                const u8* row = pixels + static_cast<usize>(y) * surface.pitch();
                for (u32 x = 0; x < surface.width(); ++x) {
                    const u8 index = row[x];
                    used[index] = true;
                    nonzero_pixels += index != 0;
                    minimum_index = std::min(minimum_index, index);
                    maximum_index = std::max(maximum_index, index);
                }
            }
            usize used_colors = 0;
            usize nonblack_colors = 0;
            for (u32 index = 0; index < IndexedColorTable::ENTRY_COUNT; ++index) {
                if (!used[index]) continue;
                ++used_colors;
                const PaletteEntry* color = surface.palette()->entries(index, 1);
                nonblack_colors +=
                    color->red != 0 || color->green != 0 || color->blue != 0;
            }
            // Row occupancy: which rows carry any non-zero index, so a frame
            // that only fills a strip is distinguishable from a full-field one.
            u32 first_nz_row = surface.height();
            u32 last_nz_row = 0;
            u32 nz_rows = 0;
            for (u32 y = 0; y < surface.height(); ++y) {
                const u8* row = pixels + static_cast<usize>(y) * surface.pitch();
                bool row_nz = false;
                for (u32 x = 0; x < surface.width(); ++x) {
                    if (row[x] != 0) {
                        row_nz = true;
                        break;
                    }
                }
                if (!row_nz) continue;
                ++nz_rows;
                first_nz_row = std::min(first_nz_row, y);
                last_nz_row = y;
            }
            // Printed directly rather than through VITREO_TRACE so that
            // VITREO_PRESENT_PIXEL_DIAG is self-sufficient: the scan and its
            // output are gated by the same switch.
            std::fprintf(stderr,
                         "[present] indexed %ux%u generation=%llu palette=%llu "
                         "range=%u..%u nonzero=%zu used_colors=%zu nonblack=%zu "
                         "nz_rows=%u y=%u..%u\n",
                         surface.width(), surface.height(),
                         static_cast<unsigned long long>(surface.generation()),
                         static_cast<unsigned long long>(palette_generation),
                         minimum_index, maximum_index, nonzero_pixels,
                         used_colors, nonblack_colors, nz_rows,
                         first_nz_row, last_nz_row);
        }

        const char* dump_path = std::getenv("VITREO_DUMP_PRESENT_FRAME");
        if (!dump_path) return;

        // The GPU resolves the palette now, so reuse the CPU expansion purely
        // to render the capture - it stays the reference for what should be
        // on screen.
        std::vector<u8> bgra;
        const usize pixel_bytes =
            static_cast<usize>(surface.pitch()) * surface.height();
        if (expand_indexed_bgra(surface, pixels, pixel_bytes, bgra)) {
            if (FILE* dump = std::fopen(dump_path, "wb")) {
                std::fprintf(dump, "P6\n%u %u\n255\n",
                             surface.width(), surface.height());
                for (usize offset = 0; offset < bgra.size(); offset += 4) {
                    const u8 rgb[] = {bgra[offset + 2], bgra[offset + 1],
                                      bgra[offset]};
                    std::fwrite(rgb, sizeof(rgb), 1, dump);
                }
                std::fclose(dump);
            }
        }
        // Companion diagnostics: raw guest palette indices (.idx) and the
        // 256-entry RGB palette (.pal), so a debugger can distinguish a
        // never-drawn (index 0) region from a solid-block fill (one nonzero
        // index) from real content mapped by an incorrect palette.
        std::string idx_path = std::string(dump_path) + ".idx";
        if (FILE* idx = std::fopen(idx_path.c_str(), "wb")) {
            for (u32 y = 0; y < surface.height(); ++y) {
                std::fwrite(pixels + static_cast<usize>(y) * surface.pitch(),
                            1, surface.width(), idx);
            }
            std::fclose(idx);
        }
        std::string pal_path = std::string(dump_path) + ".pal";
        if (FILE* pal = std::fopen(pal_path.c_str(), "wb")) {
            for (u32 index = 0; index < IndexedColorTable::ENTRY_COUNT; ++index) {
                const PaletteEntry* color = surface.palette()->entries(index, 1);
                const u8 rgb[] = {color->red, color->green, color->blue};
                std::fwrite(rgb, sizeof(rgb), 1, pal);
            }
            std::fclose(pal);
        }
    }

    windowing::CocoaBridge& m_cocoa_bridge;
    dispatch_queue_t m_present_queue{nullptr};
    std::shared_ptr<PresentState> m_state;
    // Guest-thread-only change detection.
    const CpuSurface* m_surface{nullptr};
    u64 m_surface_generation{0};
    u64 m_palette_generation{0};
};

} // namespace

std::unique_ptr<ISurfacePresenter> create_metal_surface_presenter(
    windowing::CocoaBridge& cocoa_bridge) {
    return std::make_unique<MetalSurfacePresenter>(cocoa_bridge);
}

} // namespace vitreo::graphics
