// SPDX-License-Identifier: LGPL-2.1-only

/// @file metal_backend_swap_chain.mm
/// @brief The backend swap chain

#import <Metal/Metal.h>
#import <IOSurface/IOSurface.h>
#import <QuartzCore/CAMetalLayer.h>
#include "metal_device.hpp"
#include "graphics/common/diagnostic_path.hpp"
#include "graphics/metal/metal_buffer.hpp"
#include "graphics/metal/metal_swapchain.hpp"
#include "graphics/metal/metal_submit_queue.hpp"
#include "graphics/metal/metal_tiled.hpp"
#include "graphics/metal/metal_gpu_profile.hpp"
#include "graphics/contracts/delta_record.hpp"
#include "graphics/contracts/slim_record.hpp"
#include <vitreo/ab_cycle.hpp>

#include <atomic>
#include <cmath>
#include <chrono>
#include "graphics/metal/metal_texture.hpp"
#include "graphics/metal/metal_shared_resource.hpp"
#include "core/memory/host_memory_floor.hpp"

#include <mach/mach.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <limits>
#include <memory>
#include <map>
#include <unordered_map>
#include <utility>
#include <mutex>
#include <thread>
#include <deque>
#include <unistd.h>
#include <condition_variable>
#include <vector>

#include "graphics/metal/metal_backend_texture_2d.hpp"
#include "graphics/metal/metal_backend_render_pass.hpp"

using namespace vitreo;

namespace vitreo::graphics {

namespace {

class MetalBackendSwapChain final : public BackendSwapChain {
public:
    MetalBackendSwapChain(void* device, void* command_queue,
                          BackendSwapChainDesc desc)
        : m_device(device)
        , m_command_queue(command_queue)
        , m_desc(desc) {
        rebuild();
    }

    ~MetalBackendSwapChain() {
        MetalSubmitQueue::instance().drain();
    }

    bool valid() const noexcept override {
        if (m_swapchain == nullptr || !m_swapchain->valid() ||
            m_back_buffer == nullptr || m_back_buffer->handle() == nullptr) {
            return false;
        }
        if (m_desc.sample_count > 1) {
            if (m_msaa_color == nullptr || m_msaa_color->handle() == nullptr) {
                return false;
            }
            // Pairing 1x depth with MSAA color is an invalid Metal pass.
            if (m_desc.depth_stencil &&
                (m_msaa_depth == nullptr ||
                 m_msaa_depth->handle() == nullptr)) {
                return false;
            }
        }
        return true;
    }

    const BackendSwapChainDesc& desc() const noexcept override {
        return m_desc;
    }

    bool clear_back_buffer(
        const std::array<float, 4>& color) override {
        if (!valid()) return false;
        CsBeginPass begin{};
        begin.colors[0] = color_attachment();
        begin.color_count = 1;
        begin.clear_color = true;
        begin.color_clear = color;
        if (m_desc.sample_count > 1 && m_back_buffer &&
            m_back_buffer->handle() != nullptr) {
            begin.resolve = m_back_buffer->handle();
        }
        const u64 pass_id = MetalSubmitQueue::instance().record_begin_pass(begin);
        MetalSubmitQueue::instance().record_end_pass(pass_id);
        m_color_initialized = true;
        return true;
    }

    bool clear_depth(float depth) override {
        if (!valid()) return false;
        void* depth_tex = depth_attachment();
        if (depth_tex == nullptr) return false;
        CsBeginPass begin{};
        begin.depth = depth_tex;
        begin.clear_depth = true;
        begin.depth_clear = depth;
        begin.stencil = true;
        const u64 pass_id = MetalSubmitQueue::instance().record_begin_pass(begin);
        MetalSubmitQueue::instance().record_end_pass(pass_id);
        m_depth_initialized = true;
        return true;
    }

    bool present(unsigned sync_interval, unsigned flags) override {
        return present_texture(nullptr, sync_interval, flags);
    }

    bool present_texture(BackendTexture2D* source, unsigned sync_interval,
                         unsigned flags) override {
        return present_texture_impl(source, nullptr, sync_interval, flags);
    }

    bool present_texture_region(BackendTexture2D* source,
                                const PresentRegion& region,
                                unsigned sync_interval,
                                unsigned flags) override {
        return present_texture_impl(source, &region, sync_interval, flags);
    }

    bool present_texture_impl(BackendTexture2D* source,
                              const PresentRegion* region,
                              unsigned sync_interval, unsigned flags) {
        // SyncInterval 0..4 (DXGI / D3DPRESENT_INTERVAL_*): 0 is immediate,
        // n keeps the frame up for n refreshes (MetalSwapchain::present).
        // The frontend consumes its present flags; none reach the backend.
        if (!valid() || sync_interval > 4 || flags != 0) return false;
        @autoreleasepool {
            // Keep the Metal sublayer aspect-fitted into the NSView. Without
            // this, Present blits an 800×600 drawable into a zero-sized layer
            // and the user only sees the black host layer.
            m_swapchain->update_layer_frame();

            void* src_handle = m_back_buffer->handle();
            unsigned blit_w = m_desc.width;
            unsigned blit_h = m_desc.height;
            unsigned src_x = 0;
            unsigned src_y = 0;
            if (source != nullptr) {
                // D3D11 path: guest Clear/Draw target the Texture2D backend,
                // not MetalSwapChain's private m_back_buffer.
                if (!source->valid()) return false;
                const auto* metal_src =
                    static_cast<const MetalBackendTexture2D*>(source);
                src_handle = metal_src->native_handle();
                if (src_handle == nullptr) return false;
                blit_w = source->desc().width;
                blit_h = source->desc().height;
                // A composition clip: present only that rectangle.
                if (region != nullptr && region->width != 0 &&
                    region->height != 0 &&
                    region->x + region->width <= blit_w &&
                    region->y + region->height <= blit_h) {
                    src_x = region->x;
                    src_y = region->y;
                    blit_w = region->width;
                    blit_h = region->height;
                }
            }

            // Opt-in source capture. This reads the render target before
            // the drawable blit, separating an empty guest frame from a
            // CAMetalLayer/presentation failure. The synchronized readback is
            // intentionally absent from the normal present path.
            static const bool k_dump_d3d9 =
                std::getenv("VITREO_D3D9_DUMP_PRESENT") != nullptr;
            static const bool k_dump_d3d11 =
                std::getenv("VITREO_D3D11_DUMP_PRESENT") != nullptr;
            static bool source_dumped = false;
            static unsigned d3d11_present_count = 0;
            static unsigned d3d9_present_count = 0;
            if (k_dump_d3d9 || k_dump_d3d11) {
            if (source != nullptr) {
                ++d3d11_present_count;
            } else {
                ++d3d9_present_count;
            }
            const char* source_dump_path = source != nullptr
                ? std::getenv("VITREO_D3D11_DUMP_PRESENT")
                : std::getenv("VITREO_D3D9_DUMP_PRESENT");
            const char* d3d11_after_text =
                std::getenv("VITREO_D3D11_DUMP_PRESENT_AFTER");
            const unsigned d3d11_after = d3d11_after_text != nullptr
                ? static_cast<unsigned>(
                      std::strtoul(d3d11_after_text, nullptr, 0))
                : 1u;
            const char* d3d11_trigger =
                std::getenv("VITREO_D3D11_DUMP_PRESENT_TRIGGER");
            const auto trigger_path = diagnostic_path(
                d3d11_trigger ? d3d11_trigger : "", static_cast<unsigned long long>(::getpid()));
            d3d11_trigger = trigger_path ? trigger_path->c_str() : nullptr;
            bool d3d11_trigger_ready = trigger_path && trigger_path->empty();
            if (!d3d11_trigger_ready && d3d11_trigger) {
                if (FILE* trigger_file = std::fopen(d3d11_trigger, "rb")) {
                    std::fclose(trigger_file);
                    d3d11_trigger_ready = true;
                } else {
                    // Removing an opt-in trigger re-arms the one-shot capture
                    // so diagnostics can sample a later state in one process.
                    source_dumped = false;
                }
            }
            const char* d3d9_after_text =
                std::getenv("VITREO_D3D9_DUMP_PRESENT_AFTER");
            // The D3D9 capture arms when the environment sets the DUMP_READY
            // variable read below. VITREO_D3D9_DUMP_PRESENT_AFTER additionally
            // holds it back until that many presents have gone by, so a
            // capture can be aimed at a later scene instead of the first
            // armed frame.
            const unsigned d3d9_after = d3d9_after_text != nullptr
                ? static_cast<unsigned>(std::strtoul(d3d9_after_text, nullptr, 0))
                : 0u;
            const bool d3d9_ready =
                std::getenv("VITREO_D3D9_DUMP_READY") != nullptr;
            const bool capture_ready =
                (source != nullptr && d3d11_trigger_ready &&
                 d3d11_present_count >= d3d11_after) ||
                (d3d9_ready && d3d9_present_count >= d3d9_after);
            // A path containing "%u" captures every qualifying present into a
            // numbered file instead of stopping after the first, which is what
            // a draw-by-draw bisect needs.
            const bool dump_sequence = source_dump_path != nullptr &&
                std::strstr(source_dump_path, "%u") != nullptr;
            const auto dump_target = diagnostic_path(
                source_dump_path ? source_dump_path : "",
                static_cast<unsigned long long>(::getpid()),
                source != nullptr ? d3d11_present_count : d3d9_present_count);
            source_dump_path = dump_target ? dump_target->c_str() : nullptr;
            if ((!source_dumped || dump_sequence) && capture_ready &&
                source_dump_path != nullptr && source_dump_path[0] != '\0') {
                MetalSubmitQueue::instance().drain();
                source_dumped = true;
                const auto format = source != nullptr
                    ? source->desc().pixel_format
                    : PixelFormat::BGRA8Unorm;
                const bool bgra =
                    format == PixelFormat::BGRA8Unorm ||
                    format == PixelFormat::BGRA8Unorm_sRGB;
                const bool rgba =
                    format == PixelFormat::RGBA8Unorm ||
                    format == PixelFormat::RGBA8Unorm_sRGB;
                const usize row_bytes = static_cast<usize>(blit_w) * 4;
                const usize readback_pitch = (row_bytes + 255u) & ~255u;
                id<MTLDevice> device = (__bridge id<MTLDevice>)m_device;
                id<MTLCommandQueue> queue =
                    (__bridge id<MTLCommandQueue>)m_command_queue;
                id<MTLTexture> texture = (__bridge id<MTLTexture>)src_handle;
                id<MTLBuffer> readback = nil;
                if ((bgra || rgba) && device != nil && queue != nil &&
                    texture != nil) {
                    readback = [device
                        newBufferWithLength:readback_pitch * blit_h
                                   options:MTLResourceStorageModeShared];
                }
                if (readback != nil) {
                    id<MTLCommandBuffer> command = [queue commandBuffer];
                    id<MTLBlitCommandEncoder> blit =
                        [command blitCommandEncoder];
                    [blit copyFromTexture:texture
                              sourceSlice:0
                              sourceLevel:0
                             sourceOrigin:MTLOriginMake(0, 0, 0)
                               sourceSize:MTLSizeMake(blit_w, blit_h, 1)
                                 toBuffer:readback
                        destinationOffset:0
                   destinationBytesPerRow:readback_pitch
                 destinationBytesPerImage:readback_pitch * blit_h];
                    [blit endEncoding];
                    [command commit];
                    [command waitUntilCompleted];
                    if (command.status == MTLCommandBufferStatusCompleted) {
                        if (FILE* dump = std::fopen(source_dump_path, "wb")) {
                            std::fprintf(dump, "P6\n%u %u\n255\n", blit_w,
                                         blit_h);
                            const auto* bytes =
                                static_cast<const u8*>([readback contents]);
                            for (unsigned y = 0; y < blit_h; ++y) {
                                const u8* row = bytes + y * readback_pitch;
                                for (unsigned x = 0; x < blit_w; ++x) {
                                    const u8* pixel = row + x * 4;
                                    const u8 rgb[3] = {
                                        bgra ? pixel[2] : pixel[0], pixel[1],
                                        bgra ? pixel[0] : pixel[2]};
                                    std::fwrite(rgb, sizeof(rgb), 1, dump);
                                }
                            }
                            std::fclose(dump);
                            std::fprintf(
                                stderr,
                                "[metal-present-diag] dumped pre-present source %s "
                                "%ux%u pitch=%zu\n",
                                source_dump_path, blit_w, blit_h,
                                readback_pitch);
                        }
                    } else {
                        std::fprintf(
                            stderr,
                            "[metal-present-diag] pre-present readback failed: %s\n",
                            command.error != nil
                                ? [[command.error localizedDescription]
                                      UTF8String]
                                : "unknown Metal error");
                    }
                } else {
                    std::fprintf(
                        stderr,
                        "[metal-present-diag] pre-present readback unsupported "
                        "format=%u\n",
                        static_cast<unsigned>(format));
                }
            }
            }

            m_swapchain->set_vsync(sync_interval != 0);
            if (source == nullptr) {
                resolve_msaa_if_needed();
            }
            // Worker encodes and presents. submit() waits if a list is
            // still pending/encoding or the present two frames back has not
            // retired (MetalSubmitQueue::kPresentLatency). Do not GPU-wait
            // this frame on T1. The DISCARD pool reclaims storage on GPU
            // completion when it allocates; a Present only lets it release
            // slabs that have stayed idle.
            MetalSubmitQueue::instance().record_present(
                m_swapchain.get(), src_handle, blit_w, blit_h,
                sync_interval, src_x, src_y);
            (void)MetalSubmitQueue::instance().submit();
            metal_note_presented_buffers();
        }
        return true;
    }

    bool present_would_block() const override {
        return MetalSubmitQueue::instance().submit_would_block();
    }

    bool set_background_color(const std::array<float, 4>& rgba) override {
        m_background = rgba;
        m_background_set = true;
        if (m_swapchain) {
            m_swapchain->set_background_color(rgba[0], rgba[1], rgba[2],
                                              rgba[3]);
        }
        return m_swapchain != nullptr;
    }

    bool set_color_space(BackendColorSpace space) override {
        m_color_space = space;
        if (m_swapchain) {
            m_swapchain->set_color_space(static_cast<u32>(space));
        }
        return m_swapchain != nullptr;
    }

    bool set_hdr10_metadata(const std::vector<u8>& hdr10) override {
        m_hdr10 = hdr10;
        if (m_swapchain) m_swapchain->set_hdr10_metadata(hdr10);
        return m_swapchain != nullptr;
    }

    bool readback_texture_rgba(BackendTexture2D* source,
                               std::vector<u8>& rgba) override {
        rgba.clear();

        // Null source: D3D9 windowed Present has no distinct readable front
        // buffer on CAMetalLayer — the private back buffer holds the last
        // Clear/Draw contents that Present blits to the drawable.
        PixelFormat format = PixelFormat::Unknown;
        unsigned width = 0;
        unsigned height = 0;
        void* texture_handle = nullptr;
        if (source != nullptr) {
            if (!source->valid()) return false;
            format = source->desc().pixel_format;
            width = source->desc().width;
            height = source->desc().height;
            const auto* metal_source =
                static_cast<const MetalBackendTexture2D*>(source);
            texture_handle = metal_source->native_handle();
        } else {
            if (!valid() || m_back_buffer == nullptr ||
                m_back_buffer->handle() == nullptr) {
                return false;
            }
            resolve_msaa_if_needed();
            format = m_desc.pixel_format;
            width = m_desc.width;
            height = m_desc.height;
            texture_handle = m_back_buffer->handle();
        }

        const bool bgra =
            format == PixelFormat::BGRA8Unorm ||
            format == PixelFormat::BGRA8Unorm_sRGB;
        const bool source_rgba =
            format == PixelFormat::RGBA8Unorm ||
            format == PixelFormat::RGBA8Unorm_sRGB;
        if ((!bgra && !source_rgba) || width == 0 || height == 0) {
            return false;
        }

        id<MTLTexture> texture = (__bridge id<MTLTexture>)texture_handle;
        id<MTLDevice> device = (__bridge id<MTLDevice>)m_device;
        id<MTLCommandQueue> queue =
            (__bridge id<MTLCommandQueue>)m_command_queue;
        if (texture == nil || device == nil || queue == nil) return false;

        // Foreign-HWND presentation reads the rendered swap-chain image back
        // instead of blitting it to a local CAMetalDrawable. Drain the submit
        // queue first so this readback is ordered after every Clear/Draw
        // recorded for the frame, just as the local Present path is.
        MetalSubmitQueue::instance().drain();

        // Apple documents 256-byte texture-to-buffer row alignment for the
        // Mac2 Metal family. The shared buffer makes the completed blit
        // visible to the CPU after waitUntilCompleted.
        const usize row_bytes = static_cast<usize>(width) * 4u;
        const usize readback_pitch = (row_bytes + 255u) & ~255u;
        if (readback_pitch > SIZE_MAX / height ||
            row_bytes > SIZE_MAX / height) {
            return false;
        }

        @autoreleasepool {
            id<MTLBuffer> readback =
                [device newBufferWithLength:readback_pitch * height
                                    options:MTLResourceStorageModeShared];
            if (readback == nil) return false;
            id<MTLCommandBuffer> command = [queue commandBuffer];
            id<MTLBlitCommandEncoder> blit =
                [command blitCommandEncoder];
            if (command == nil || blit == nil) return false;
            [blit copyFromTexture:texture
                      sourceSlice:0
                      sourceLevel:0
                     sourceOrigin:MTLOriginMake(0, 0, 0)
                       sourceSize:MTLSizeMake(width, height, 1)
                         toBuffer:readback
                destinationOffset:0
           destinationBytesPerRow:readback_pitch
         destinationBytesPerImage:readback_pitch * height];
            [blit endEncoding];
            [command commit];
            [command waitUntilCompleted];
            if (command.status != MTLCommandBufferStatusCompleted) {
                std::fprintf(
                    stderr, "[metal] texture readback failed: %s\n",
                    command.error != nil
                        ? [[command.error localizedDescription] UTF8String]
                        : "unknown Metal error");
                return false;
            }

            rgba.resize(row_bytes * height);
            const auto* input =
                static_cast<const u8*>([readback contents]);
            for (unsigned y = 0; y < height; ++y) {
                const u8* src_row = input + y * readback_pitch;
                u8* dst_row = rgba.data() + y * row_bytes;
                if (source_rgba) {
                    std::memcpy(dst_row, src_row, row_bytes);
                    continue;
                }
                for (unsigned x = 0; x < width; ++x) {
                    const u8* src = src_row + x * 4u;
                    u8* dst = dst_row + x * 4u;
                    dst[0] = src[2];
                    dst[1] = src[1];
                    dst[2] = src[0];
                    dst[3] = src[3];
                }
            }
        }
        return true;
    }

    bool configure_composition_layer(
        const BackendCompositionLayerDesc& desc) override {
        return valid() && m_swapchain->configure_composition_layer(desc);
    }

    std::unique_ptr<BackendRenderPass> begin_render_pass(
        bool preserve_contents, bool clear_depth,
        float depth_clear_value,
        const std::array<BackendTexture2D*, BACKEND_MAX_RENDER_TARGETS>&
            color_targets) override {
        if (!valid()) return nullptr;
        // Apple GPUs read uninitialised MSAA memory as magenta and depth 0.
        // The first pass after Reset must not Load that garbage. A Z-tested
        // scene with an untested HUD then shows a solid magenta world while
        // the UI stays visible.
        const bool preserve = preserve_contents && m_color_initialized;
        const bool clear_z = clear_depth || !m_depth_initialized;
        void* resolve = nullptr;
        if (m_desc.sample_count > 1 && m_back_buffer &&
            m_back_buffer->handle() != nullptr) {
            resolve = m_back_buffer->handle();
        }
        auto pass = std::make_unique<MetalBackendRenderPass>(
            m_device, color_attachment(), depth_attachment(),
            preserve, clear_z, depth_clear_value, resolve, color_targets);
        if (!pass->valid()) return nullptr;
        m_color_initialized = true;
        m_depth_initialized = true;
        return pass;
    }

    BackendTexture2D* color_target() noexcept override {
        return m_color_view.get();
    }

    bool resize(unsigned width, unsigned height,
                PixelFormat pixel_format,
                unsigned buffer_count) override {
        if (width == 0 || height == 0 || buffer_count == 0) return false;
        BackendSwapChainDesc replacement = m_desc;
        replacement.width = width;
        replacement.height = height;
        if (pixel_format != PixelFormat::Unknown) {
            replacement.pixel_format = pixel_format;
        }
        replacement.buffer_count = buffer_count;
        m_desc = replacement;
        rebuild();
        return valid();
    }

private:
    void rebuild() {
        MetalSubmitQueue::instance().drain();
        m_swapchain.reset();
        m_back_buffer.reset();
        m_depth_buffer.reset();
        m_msaa_color.reset();
        m_msaa_depth.reset();
        m_color_view.reset();
        m_color_initialized = false;
        m_depth_initialized = false;
        const FormatInfo format = format_table(m_desc.pixel_format);
        if (m_desc.native_view == nullptr || m_desc.width == 0 ||
            m_desc.height == 0 || format.mtl_pixel_format == 0) {
            return;
        }

        SwapchainDesc swapchain_desc{};
        swapchain_desc.ns_view = m_desc.native_view;
        swapchain_desc.device = m_device;
        swapchain_desc.width = m_desc.width;
        swapchain_desc.height = m_desc.height;
        swapchain_desc.pixel_format = format.mtl_pixel_format;
        // CAMetalLayer stalls when the caller holds the last drawable on the
        // AppKit thread. Keep the Metal pool at 3 even if the guest asked for
        // a single D3D9 back buffer (mapped to 2).
        swapchain_desc.buffer_count = 3;
        swapchain_desc.vsync = m_desc.vsync;
        // Present blits the private back buffer into the drawable texture.
        // framebufferOnly=YES (SwapchainDesc default, used by the indexed
        // DirectDraw presenter that renders *into* the drawable) forbids
        // blit destinations — Metal then leaves the layer black while
        // Present still returns success.
        swapchain_desc.framebuffer_only = false;
        swapchain_desc.opaque = !m_desc.premultiplied_alpha;
        m_swapchain =
            std::make_unique<MetalSwapchain>(swapchain_desc);
        if (!m_swapchain->valid()) {
            // An unsupported layer format or size: no layer was built.
            m_swapchain.reset();
            return;
        }
        // Presentation state survives a rebuild (ResizeBuffers).
        if (m_color_space != BackendColorSpace::Srgb) {
            m_swapchain->set_color_space(static_cast<u32>(m_color_space));
        }
        if (!m_hdr10.empty()) m_swapchain->set_hdr10_metadata(m_hdr10);
        if (m_background_set) {
            m_swapchain->set_background_color(m_background[0], m_background[1],
                                              m_background[2], m_background[3]);
        }

        TextureDesc texture_desc{};
        texture_desc.width = m_desc.width;
        texture_desc.height = m_desc.height;
        texture_desc.pixel_format = format.mtl_pixel_format;
        texture_desc.usage =
            MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        texture_desc.storage_mode = MTLStorageModePrivate;
        texture_desc.render_target = true;
        m_back_buffer =
            std::make_unique<MetalTexture>(m_device, texture_desc);

        if (m_desc.sample_count > 1) {
            TextureDesc msaa_color = texture_desc;
            msaa_color.sample_count = m_desc.sample_count;
            msaa_color.texture_type = 4u; // MTLTextureType2DMultisample
            msaa_color.usage = MTLTextureUsageRenderTarget;
            msaa_color.storage_mode = MTLStorageModePrivate;
            m_msaa_color = std::make_unique<MetalTexture>(m_device, msaa_color);
            if (m_msaa_color->handle() == nullptr) {
                std::fprintf(stderr,
                             "[metal] MSAA color create failed samples=%u "
                             "%ux%u\n",
                             m_desc.sample_count, m_desc.width, m_desc.height);
                m_msaa_color.reset();
            }
        }

        if (m_desc.depth_stencil) {
            TextureDesc depth_desc{};
            depth_desc.width = m_desc.width;
            depth_desc.height = m_desc.height;
            depth_desc.pixel_format = MTLPixelFormatDepth32Float_Stencil8;
            depth_desc.usage = MTLTextureUsageRenderTarget;
            depth_desc.storage_mode = MTLStorageModePrivate;
            depth_desc.render_target = true;
            m_depth_buffer =
                std::make_unique<MetalTexture>(m_device, depth_desc);
            if (m_depth_buffer->handle() == nullptr) m_depth_buffer.reset();
            if (m_desc.sample_count > 1) {
                TextureDesc msaa_depth = depth_desc;
                msaa_depth.sample_count = m_desc.sample_count;
                msaa_depth.texture_type = 4u;
                m_msaa_depth =
                    std::make_unique<MetalTexture>(m_device, msaa_depth);
                if (m_msaa_depth->handle() == nullptr) {
                    std::fprintf(stderr,
                                 "[metal] MSAA depth create failed samples=%u "
                                 "%ux%u\n",
                                 m_desc.sample_count, m_desc.width,
                                 m_desc.height);
                    m_msaa_depth.reset();
                }
            }
        }

        if (valid()) {
            BackendTexture2DDesc view{};
            view.width = m_desc.width;
            view.height = m_desc.height;
            view.mip_levels = 1;
            view.array_size = 1;
            view.pixel_format = m_desc.pixel_format;
            view.render_target = true;
            view.sample_count =
                m_desc.sample_count > 1 ? m_desc.sample_count : 1;
            // MSAA attachments cannot be sampled; the 1x resolve can.
            view.shader_resource = view.sample_count <= 1;
            m_color_view = std::make_unique<MetalBackendTexture2D>(
                color_attachment(), m_command_queue, view);

            // Private MSAA colour is magenta and depth is 0 until stored.
            // A modulate composite onto black stays black; onto magenta
            // stays magenta. White is the D3D dest that lets a first-frame
            // lightmap/scene multiply show the park. Depth 1.0 lets Z-tested
            // park draws through.
            (void)clear_back_buffer({1.0f, 1.0f, 1.0f, 1.0f});
            if (depth_attachment() != nullptr) {
                (void)clear_depth(1.0f);
            }
        }
    }

    [[nodiscard]] void* color_attachment() const {
        if (m_msaa_color && m_msaa_color->handle() != nullptr) {
            return m_msaa_color->handle();
        }
        return m_back_buffer != nullptr ? m_back_buffer->handle() : nullptr;
    }

    [[nodiscard]] void* depth_attachment() const {
        if (m_desc.sample_count > 1) {
            return m_msaa_depth != nullptr ? m_msaa_depth->handle() : nullptr;
        }
        return m_depth_buffer != nullptr ? m_depth_buffer->handle() : nullptr;
    }

    void resolve_msaa_if_needed() {
        if (m_msaa_color == nullptr || m_msaa_color->handle() == nullptr ||
            m_back_buffer == nullptr || m_back_buffer->handle() == nullptr) {
            return;
        }
        MetalSubmitQueue::instance().record_resolve(m_msaa_color->handle(),
                                                    m_back_buffer->handle());
    }

    void* m_device{nullptr};
    void* m_command_queue{nullptr};
    BackendSwapChainDesc m_desc;
    std::unique_ptr<MetalSwapchain> m_swapchain;
    std::unique_ptr<MetalTexture> m_back_buffer;
    std::unique_ptr<MetalTexture> m_depth_buffer;
    std::unique_ptr<MetalTexture> m_msaa_color;
    std::unique_ptr<MetalTexture> m_msaa_depth;
    std::unique_ptr<BackendTexture2D> m_color_view;
    bool m_color_initialized{false};
    bool m_depth_initialized{false};
    BackendColorSpace m_color_space{BackendColorSpace::Srgb};
    std::vector<u8> m_hdr10;
    std::array<float, 4> m_background{0.0f, 0.0f, 0.0f, 1.0f};
    bool m_background_set{false};
};

} // namespace

std::unique_ptr<BackendSwapChain> MetalDevice::create_swap_chain(
    const BackendSwapChainDesc& desc) {
    auto swapchain = std::make_unique<MetalBackendSwapChain>(
        m_device, m_command_queue, desc);
    if (!swapchain->valid()) return nullptr;
    return swapchain;
}

} // namespace vitreo::graphics
