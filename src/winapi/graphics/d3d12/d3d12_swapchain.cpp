// SPDX-License-Identifier: LGPL-2.1-only

/// @file d3d12_swapchain.cpp
/// @brief IDXGISwapChain4 on an ID3D12CommandQueue (PLAN milestone M3).
///
/// DXGI creates a chain for D3D12 on a command queue, not on the device
/// (Learn, IDXGIFactory2::CreateSwapChainForHwnd: "For Direct3D 12 this is a
/// pointer to a direct command queue"). Only the flip model exists for D3D12
/// (Learn, "DXGI flip model": the bitblt effects are INVALID_CALL). The
/// buffers are ordinary committed ID3D12Resource textures of the queue's
/// device; GetCurrentBackBufferIndex names the one to render next, and
/// Present shows it on the window in the queue's order, after every command
/// list executed on the queue before it.
///
/// Every rule below is a line of tools/oracle_probe/d3d12_a08_swapchain_probe
/// .expected (Windows 11, WARP) unless it cites Learn.

#include "winapi/graphics/d3d12/d3d12_internal.hpp"
#include "winapi/graphics/common/gpu_event_signaler.hpp"
#include "winapi/graphics/dxgi/dxgi.hpp"
#include "core/memory/guest_memory.hpp"
#include "vitreo/virtual_tsc.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace vitreo::winapi::d3d12 {
namespace {

// DXGI_SWAP_EFFECT (dxgi.h).
constexpr u32 kSwapEffectFlipSequential = 3;
constexpr u32 kSwapEffectFlipDiscard = 4;
// DXGI_SCALING / DXGI_ALPHA_MODE (dxgi1_2.h).
constexpr u32 kScalingStretch = 0;
constexpr u32 kScalingNone = 1;
constexpr u32 kAlphaUnspecified = 0;
constexpr u32 kAlphaPremultiplied = 1;
constexpr u32 kAlphaIgnore = 3;
// DXGI_USAGE (dxgi.h).
constexpr u32 kUsageShaderInput = 0x10;
constexpr u32 kUsageRenderTargetOutput = 0x20;
constexpr u32 kUsageBackBuffer = 0x40;
// DXGI_SWAP_CHAIN_FLAG (dxgi.h).
constexpr u32 kFlagNonPrerotated = 0x1;
constexpr u32 kFlagAllowModeSwitch = 0x2;
constexpr u32 kFlagFrameLatencyWaitable = 0x40;
constexpr u32 kFlagAllowTearing = 0x800;
// DXGI_PRESENT (dxgi.h).
constexpr u32 kPresentTest = 0x1;
constexpr u32 kPresentDoNotSequence = 0x2;
constexpr u32 kPresentRestart = 0x4;
constexpr u32 kPresentDoNotWait = 0x8;
constexpr u32 kPresentStereoPreferRight = 0x10;
constexpr u32 kPresentStereoTemporaryMono = 0x20;
constexpr u32 kPresentRestrictToOutput = 0x40;
constexpr u32 kPresentUseDuration = 0x100;
constexpr u32 kPresentAllowTearing = 0x200;
constexpr u32 kPresentDefined =
    kPresentTest | kPresentDoNotSequence | kPresentRestart | kPresentDoNotWait |
    kPresentStereoPreferRight | kPresentStereoTemporaryMono |
    kPresentRestrictToOutput | kPresentUseDuration | kPresentAllowTearing;
// DXGI_COLOR_SPACE_TYPE (dxgicommon.h).
constexpr u32 kColorSpaceSrgb = 0;        // RGB_FULL_G22_NONE_P709
constexpr u32 kColorSpaceScRgb = 1;       // RGB_FULL_G10_NONE_P709
constexpr u32 kColorSpaceHdr10 = 12;      // RGB_FULL_G2084_NONE_P2020
constexpr u32 kColorSpaceBt2020G22 = 22;  // RGB_FULL_G22_NONE_P2020
// DXGI_HDR_METADATA_TYPE.
constexpr u32 kHdrMetadataNone = 0;
constexpr u32 kHdrMetadataHdr10 = 1;
constexpr u32 kHdr10MetadataBytes = 28;
// D3D12 heap flags a swap-chain buffer reports (measured: 0x8d =
// ALLOW_ONLY_RT_DS_TEXTURES | ALLOW_DISPLAY | SHARED).
constexpr u32 kBufferHeapFlags = D3D12_HEAP_FLAG_ALLOW_ONLY_RT_DS_TEXTURES |
                                 D3D12_HEAP_FLAG_ALLOW_DISPLAY |
                                 D3D12_HEAP_FLAG_SHARED;

/// The flip model's back-buffer formats (Learn, "DXGI flip model").
[[nodiscard]] bool flip_format(u32 format) {
    return format == DXGI_FORMAT_R16G16B16A16_FLOAT ||
           format == DXGI_FORMAT_R10G10B10A2_UNORM ||
           format == DXGI_FORMAT_R8G8B8A8_UNORM ||
           format == DXGI_FORMAT_B8G8R8A8_UNORM;
}

[[nodiscard]] graphics::BackendColorSpace backend_color_space(u32 space) {
    switch (space) {
    case kColorSpaceScRgb: return graphics::BackendColorSpace::ExtendedLinearSrgb;
    case kColorSpaceHdr10: return graphics::BackendColorSpace::Bt2100Pq;
    case kColorSpaceBt2020G22: return graphics::BackendColorSpace::Bt2020SrgbGamma;
    default: return graphics::BackendColorSpace::Srgb;
    }
}

/// The colour space, background, rotation and HDR metadata the guest set
/// belong to the chain, not to one presenter: a presenter created (or
/// re-created after a resize) takes them all.
void apply_presentation_state(SwapChain& chain) {
    if (!chain.backend) return;
    if (!chain.backend->set_color_space(backend_color_space(chain.color_space))) {
        unimplemented("IDXGISwapChain3::SetColorSpace1",
                      "the presenter cannot tag this colour space (shown as sRGB)");
    }
    if (chain.background != std::array<float, 4>{0.0f, 0.0f, 0.0f, 1.0f} &&
        !chain.backend->set_background_color(chain.background)) {
        unimplemented("IDXGISwapChain1::SetBackgroundColor",
                      "the presenter keeps its own background");
    }
    if (!chain.backend->set_rotation(chain.rotation - 1)) {
        unimplemented("IDXGISwapChain1::SetRotation",
                      "the presenter does not rotate the image");
    }
    if (chain.hdr_metadata_type == kHdrMetadataHdr10) {
        (void)chain.backend->set_hdr10_metadata(chain.hdr_metadata);
    }
}

}  // namespace

// =============================================================================
// Creation
// =============================================================================

u32 Frontend::validate_swap_chain(const SwapChain& c, SwapChainApi api) const {
    const bool composition = api == SwapChainApi::Composition;
    if (c.swap_effect != kSwapEffectFlipSequential &&
        c.swap_effect != kSwapEffectFlipDiscard) {
        return DXGI_ERROR_INVALID_CALL;
    }
    if (c.buffer_count < 2 || c.buffer_count > 16 || !flip_format(c.format)) {
        return DXGI_ERROR_INVALID_CALL;
    }
    if ((c.buffer_usage &
         ~(kUsageShaderInput | kUsageRenderTargetOutput | kUsageBackBuffer)) != 0) {
        return DXGI_ERROR_INVALID_CALL;
    }
    if ((c.flags & ~(kFlagNonPrerotated | kFlagAllowModeSwitch |
                     kFlagFrameLatencyWaitable | kFlagAllowTearing)) != 0) {
        return DXGI_ERROR_INVALID_CALL;
    }
    // A window chain stretches or does not scale; a composition chain only
    // stretches. Premultiplied / straight alpha needs composition.
    if (composition ? c.scaling != kScalingStretch
                    : c.scaling != kScalingStretch && c.scaling != kScalingNone) {
        return DXGI_ERROR_INVALID_CALL;
    }
    // Measured (a08 ALPHA_MODE_PREMULTIPLIED on a window: INVALID_CALL);
    // the D3D11 chains' measured rule: straight alpha nowhere, premultiplied
    // on composition chains only.
    if (!(c.alpha_mode == kAlphaUnspecified || c.alpha_mode == kAlphaIgnore ||
          (c.alpha_mode == kAlphaPremultiplied && composition))) {
        return DXGI_ERROR_INVALID_CALL;
    }
    if (c.stereo != 0) {
        // Windows accepts a stereo flip chain on an adapter with a stereo
        // path (WARP has one); the host has no stereo presentation and
        // IDXGIFactory2::IsWindowedStereoEnabled reports FALSE, as for the
        // D3D11 chains.
        unimplemented("IDXGIFactory2::CreateSwapChainForHwnd",
                      "Stereo = TRUE (no stereo presentation)");
        return DXGI_ERROR_INVALID_CALL;
    }
    return S_OK;
}

void Frontend::create_swap_chain(Marshaler& m, SwapChainApi api) {
    std::lock_guard lock(m_mutex);
    // CreateSwapChain(pDevice, DXGI_SWAP_CHAIN_DESC *, ppSwapChain);
    // CreateSwapChainForHwnd(pDevice, hWnd, DESC1 *, FULLSCREEN_DESC *,
    //     pRestrictToOutput, ppSwapChain);
    // CreateSwapChainForComposition(pDevice, DESC1 *, pRestrictToOutput,
    //     ppSwapChain). Argument 0 is the factory.
    const GuestAddr factory = m.arg_ptr(0);
    CommandQueue* queue = find<CommandQueue>(m.arg_ptr(1), Kind::CommandQueue);
    const usize out_index =
        api == SwapChainApi::Legacy ? 3 : api == SwapChainApi::Hwnd ? 6 : 4;
    const GuestAddr out = m.arg_ptr(out_index);
    // Measured (a08): every failure leaves *ppSwapChain NULL.
    if (out != 0) (void)write_pointer(out, 0);
    auto chain = std::make_unique<SwapChain>();
    chain->composition = api == SwapChainApi::Composition;
    GuestAddr restrict_output = 0;
    bool fullscreen_requested = false;
    if (api == SwapChainApi::Legacy) {
        // DXGI_SWAP_CHAIN_DESC: BufferDesc (DXGI_MODE_DESC, 28 bytes),
        // SampleDesc, BufferUsage, BufferCount, OutputWindow (pointer),
        // Windowed, SwapEffect, Flags.
        const GuestAddr desc = m.arg_ptr(2);
        if (desc == 0 || out == 0 || queue == nullptr) {
            m.set_return(DXGI_ERROR_INVALID_CALL);
            return;
        }
        const u32 window_offset = m_is_64bit ? 48u : 44u;
        const u32 after = window_offset + (m_is_64bit ? 8u : 4u);
        chain->width = read_u32(desc + 0);
        chain->height = read_u32(desc + 4);
        chain->refresh_numerator = read_u32(desc + 8);
        chain->refresh_denominator = read_u32(desc + 12);
        chain->format = read_u32(desc + 16);
        chain->scanline_ordering = read_u32(desc + 20);
        chain->fullscreen_scaling = read_u32(desc + 24);
        if (read_u32(desc + 28) != 1 || read_u32(desc + 32) != 0) {
            m.set_return(DXGI_ERROR_INVALID_CALL);
            return;
        }
        chain->buffer_usage = read_u32(desc + 36);
        chain->buffer_count = read_u32(desc + 40);
        GuestAddr window = 0;
        (void)read_pointer(desc + window_offset, window);
        chain->hwnd = window;
        fullscreen_requested = read_u32(desc + after) == 0;
        chain->swap_effect = read_u32(desc + after + 4);
        chain->flags = read_u32(desc + after + 8);
        chain->scaling = kScalingStretch;
        chain->alpha_mode = kAlphaUnspecified;
        if (chain->hwnd == 0) {
            m.set_return(DXGI_ERROR_INVALID_CALL);
            return;
        }
    } else {
        const GuestAddr desc = m.arg_ptr(api == SwapChainApi::Hwnd ? 3 : 2);
        if (api == SwapChainApi::Hwnd) {
            chain->hwnd = m.arg_ptr(2);
            restrict_output = m.arg_ptr(5);
        } else {
            restrict_output = m.arg_ptr(3);
        }
        if (desc == 0 || out == 0 || queue == nullptr ||
            (api == SwapChainApi::Hwnd && chain->hwnd == 0) ||
            (restrict_output != 0 && m_presentation.dxgi != nullptr &&
             !m_presentation.dxgi->is_output_object(restrict_output))) {
            m.set_return(DXGI_ERROR_INVALID_CALL);
            return;
        }
        // DXGI_SWAP_CHAIN_DESC1.
        chain->width = read_u32(desc + 0);
        chain->height = read_u32(desc + 4);
        chain->format = read_u32(desc + 8);
        chain->stereo = read_u32(desc + 12);
        if (read_u32(desc + 16) != 1 || read_u32(desc + 20) != 0) {
            m.set_return(DXGI_ERROR_INVALID_CALL);
            return;
        }
        chain->buffer_usage = read_u32(desc + 24);
        chain->buffer_count = read_u32(desc + 28);
        chain->scaling = read_u32(desc + 32);
        chain->swap_effect = read_u32(desc + 36);
        chain->alpha_mode = read_u32(desc + 40);
        chain->flags = read_u32(desc + 44);
        if (api == SwapChainApi::Hwnd) {
            if (const GuestAddr fs = m.arg_ptr(4); fs != 0) {
                // DXGI_SWAP_CHAIN_FULLSCREEN_DESC.
                chain->refresh_numerator = read_u32(fs + 0);
                chain->refresh_denominator = read_u32(fs + 4);
                chain->scanline_ordering = read_u32(fs + 8);
                chain->fullscreen_scaling = read_u32(fs + 12);
                fullscreen_requested = read_u32(fs + 16) == 0;
            }
        }
        if (chain->composition && (chain->width == 0 || chain->height == 0)) {
            m.set_return(DXGI_ERROR_INVALID_CALL);
            return;
        }
    }
    // Learn: a D3D12 chain is created on a direct command queue.
    if (queue->type != D3D12_COMMAND_LIST_TYPE_DIRECT) {
        m.set_return(DXGI_ERROR_INVALID_CALL);
        return;
    }
    if (u32 error = validate_swap_chain(*chain, api); error != S_OK) {
        m.set_return(error);
        return;
    }
    // Learn (DXGI_SWAP_CHAIN_DESC1): a zero Width or Height takes both from
    // the window's client area.
    if (!chain->composition) {
        const auto client = m_presentation.client_size
                                ? m_presentation.client_size(chain->hwnd)
                                : std::nullopt;
        if (!client) {
            d3d12_diag("CreateSwapChain: hwnd 0x%llx is not a window; every "
                       "Present is occluded",
                       static_cast<unsigned long long>(chain->hwnd));
        }
        if (chain->width == 0 || chain->height == 0) {
            if (!client) {
                m.set_return(DXGI_ERROR_INVALID_CALL);
                return;
            }
            chain->width = std::max(1u, client->first);
            chain->height = std::max(1u, client->second);
        }
        // A window has at most one flip-model chain (Learn; measured
        // E_ACCESSDENIED).
        for (const auto& [address, object] : m_objects) {
            if (object->kind != Kind::SwapChain) continue;
            const auto& other = static_cast<const SwapChain&>(*object);
            if (!other.composition && other.hwnd == chain->hwnd) {
                m.set_return(E_ACCESSDENIED);
                return;
            }
        }
    }
    chain->device = queue->device;
    chain->queue = queue->address;
    chain->windowed = true;
    // The chain holds the queue (measured: "swap chain holds the queue"),
    // its factory and restrict-to output.
    add_ref(*queue);
    if (m_presentation.dxgi != nullptr) {
        if (factory != 0 && m_presentation.dxgi->retain_object(factory)) {
            chain->factory = factory;
        }
        if (restrict_output != 0 &&
            m_presentation.dxgi->retain_object(restrict_output)) {
            chain->restrict_output = restrict_output;
        }
    }
    if ((chain->flags & kFlagFrameLatencyWaitable) != 0) {
        chain->latency = std::make_shared<SwapChain::Latency>();
    }
    SwapChain* raw = chain.get();
    const GuestAddr address = publish(std::move(chain));
    if (address == 0) {
        m.set_return(E_OUTOFMEMORY);
        return;
    }
    if (raw->device != 0) add_device_ref(raw->device);
    if (u32 error = create_swap_chain_buffers(*raw); error != S_OK) {
        (void)release(address);
        m.set_return(error);
        return;
    }
    (void)write_pointer(out, address);
    // Learn (CreateSwapChain): "If you attempt to create a swap chain in
    // full-screen mode, and full-screen mode is unavailable, the swap chain
    // will be created in windowed mode and DXGI_STATUS_OCCLUDED will be
    // returned." The host has no exclusive full-screen.
    m.set_return(fullscreen_requested ? DXGI_STATUS_OCCLUDED : S_OK);
}

u32 Frontend::create_swap_chain_buffers(SwapChain& chain) {
    release_swap_chain_buffers(chain);
    for (u32 index = 0; index < chain.buffer_count; ++index) {
        auto resource = std::make_unique<Resource>();
        resource->device = chain.device;
        ResourceDescription& d = resource->desc;
        d.dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        d.alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
        d.width = chain.width;
        d.height = chain.height;
        d.depth_or_array_size = 1;
        d.mip_levels = 1;
        d.format = chain.format;
        d.sample_count = 1;
        d.sample_quality = 0;
        d.layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        d.flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        resource->committed = true;
        resource->heap_properties.type = D3D12_HEAP_TYPE_DEFAULT;
        resource->heap_properties.creation_node_mask = 1;
        resource->heap_properties.visible_node_mask = 1;
        resource->heap_flags = kBufferHeapFlags;
        resource->initial_state = D3D12_RESOURCE_STATE_COMMON;  // PRESENT
        resource->swap_chain = chain.address;
        if (u32 error = create_resource_backend(*resource, nullptr, 0, false);
            error != S_OK) {
            release_swap_chain_buffers(chain);
            return error;
        }
        const GuestAddr device = resource->device;
        const GuestAddr address = publish(std::move(resource));
        if (address == 0) {
            release_swap_chain_buffers(chain);
            return E_OUTOFMEMORY;
        }
        if (device != 0) add_device_ref(device);
        chain.buffers.push_back(address);
    }
    chain.current = 0;
    return S_OK;
}

void Frontend::release_swap_chain_buffers(SwapChain& chain) {
    std::vector<GuestAddr> buffers = std::move(chain.buffers);
    chain.buffers.clear();
    for (const GuestAddr buffer : buffers) (void)release(buffer);
}

// =============================================================================
// Present
// =============================================================================

u32 Frontend::present(SwapChain& chain, u32 sync_interval, u32 flags,
                      GuestAddr dirty_rects, u32 dirty_count) {
    // Measured (a08 present): SyncInterval 0..4; ALLOW_TEARING needs the
    // chain flag and interval 0; undefined bits, STEREO_* on a mono chain
    // and RESTRICT_TO_OUTPUT without an output are refused; TEST stands
    // alone and DO_NOT_SEQUENCE takes neither RESTART nor DO_NOT_WAIT
    // (Learn, DXGI_PRESENT).
    if (sync_interval > 4 || (flags & ~kPresentDefined) != 0 ||
        (flags & (kPresentStereoPreferRight | kPresentStereoTemporaryMono)) != 0 ||
        ((flags & kPresentRestrictToOutput) != 0 && chain.restrict_output == 0) ||
        ((flags & kPresentAllowTearing) != 0 &&
         ((chain.flags & kFlagAllowTearing) == 0 || sync_interval != 0)) ||
        ((flags & kPresentTest) != 0 && flags != kPresentTest) ||
        ((flags & kPresentDoNotSequence) != 0 &&
         (flags & (kPresentRestart | kPresentDoNotWait)) != 0) ||
        chain.buffers.empty()) {
        return DXGI_ERROR_INVALID_CALL;
    }
    if (dirty_count != 0) {
        // Learn (DXGI_PRESENT_PARAMETERS): dirty rectangles are for the
        // sequential flip model (measured INVALID_CALL on FLIP_DISCARD), and
        // lie inside the buffer.
        if (chain.swap_effect != kSwapEffectFlipSequential || dirty_rects == 0) {
            return DXGI_ERROR_INVALID_CALL;
        }
        for (u32 index = 0; index < dirty_count; ++index) {
            const GuestAddr r = dirty_rects + index * 16u;
            const i32 left = static_cast<i32>(read_u32(r));
            const i32 top = static_cast<i32>(read_u32(r + 4));
            const i32 right = static_cast<i32>(read_u32(r + 8));
            const i32 bottom = static_cast<i32>(read_u32(r + 12));
            if (left < 0 || top < 0 || right < left || bottom < top ||
                right > static_cast<i32>(chain.width) ||
                bottom > static_cast<i32>(chain.height)) {
                return DXGI_ERROR_INVALID_CALL;
            }
        }
    }
    const bool window_missing =
        !chain.composition &&
        !(m_presentation.client_size &&
          m_presentation.client_size(chain.hwnd).has_value());
    if ((flags & kPresentTest) != 0) {
        return window_missing ? DXGI_STATUS_OCCLUDED : S_OK;
    }
    // DXGI_PRESENT_DO_NOT_WAIT: WAS_STILL_DRAWING when the present queue is
    // full, i.e. the GPU has not finished as many frames as the latency
    // allows (Learn, IDXGIDevice1::SetMaximumFrameLatency: 3 by default; a
    // waitable chain's is its own).
    while (!chain.frames_in_flight.empty() &&
           chain.frames_in_flight.front()->ready()) {
        chain.frames_in_flight.pop_front();
    }
    const u32 latency_limit =
        (chain.flags & kFlagFrameLatencyWaitable) != 0 ? chain.max_frame_latency
                                                       : 3u;
    if ((flags & kPresentDoNotWait) != 0 &&
        chain.frames_in_flight.size() >= latency_limit) {
        return DXGI_ERROR_WAS_STILL_DRAWING;
    }
    // DO_NOT_SEQUENCE: the frame already shown stays; the ring does not
    // move (as the D3D11 chains apply it).
    if ((flags & kPresentDoNotSequence) != 0) {
        if (chain.present_count == 0) return S_OK;
        ++chain.present_count;
        chain.last_present_qpc = virtual_qpc_ticks();
        return S_OK;
    }
    const GuestAddr buffer = chain.buffers[chain.current];
    const Resource* resource = find<Resource>(buffer, Kind::Resource);
    const u64 buffer_serial = resource != nullptr ? resource->serial : 0;
    // The present runs in the queue's order: after the lists executed on it
    // before, which may still be held behind a fence wait.
    CommandQueue* queue = find<CommandQueue>(chain.queue, Kind::CommandQueue);
    if (queue != nullptr && !queue->pending.empty()) {
        CommandQueue::Pending op{};
        op.op = CommandQueue::Pending::Op::Present;
        op.chain = chain.address;
        op.chain_serial = chain.serial;
        op.buffer = buffer;
        op.buffer_serial = buffer_serial;
        op.sync_interval = sync_interval;
        op.present_flags = flags;
        queue->pending.push_back(op);
    } else {
        show(chain, buffer, buffer_serial, sync_interval, flags);
    }
    chain.current = (chain.current + 1) % static_cast<u32>(chain.buffers.size());
    ++chain.present_count;
    chain.last_present_qpc = virtual_qpc_ticks();
    return window_missing ? DXGI_STATUS_OCCLUDED : S_OK;
}

void Frontend::show(SwapChain& chain, GuestAddr buffer, u64 buffer_serial,
                    u32 sync_interval, u32 flags) {
    // The frame is the GPU work queued before this present: a point after
    // it (the present queue DO_NOT_WAIT tests), submitted now.
    if (auto frame = m_backend.record_timestamp()) {
        chain.frames_in_flight.push_back(std::move(frame));
    }
    std::shared_ptr<graphics::GpuCompletion> frame_done =
        m_backend.flush_completion();
    // FRAME_LATENCY_WAITABLE_OBJECT: the object gains a frame once that work
    // has retired, never past the latency.
    if (chain.latency && m_presentation.release_semaphore) {
        auto latency = chain.latency;
        auto release_semaphore = m_presentation.release_semaphore;
        GpuEventSignaler::instance().run_after(
            std::move(frame_done), [latency, release_semaphore] {
                std::lock_guard<std::mutex> hold(latency->mutex);
                if (latency->alive && latency->handle != 0) {
                    release_semaphore(latency->handle, 1,
                                      static_cast<i32>(latency->cap));
                }
            });
    }
    // An HWND that is not a window shows nothing (Present reported
    // DXGI_STATUS_OCCLUDED).
    if (!chain.composition &&
        !(m_presentation.client_size &&
          m_presentation.client_size(chain.hwnd).has_value())) {
        return;
    }
    Resource* resource = find_serial<Resource>(buffer, buffer_serial, Kind::Resource);
    if (resource == nullptr || !resource->texture) return;
    if (chain.composition) {
        // A composition chain is shown by the DirectComposition visual whose
        // content it is; that binding exists for D3D11 chains only.
        unimplemented("IDXGISwapChain::Present",
                      "showing a D3D12 composition swap chain");
        return;
    }
    if (!chain.backend) {
        void* view = m_presentation.presentation_view
                         ? m_presentation.presentation_view(chain.hwnd)
                         : nullptr;
        if (view == nullptr) {
            unimplemented("IDXGISwapChain::Present",
                          "a window with no presentation view in this process");
            return;
        }
        graphics::BackendSwapChainDesc desc{};
        desc.native_view = view;
        desc.width = chain.width;
        desc.height = chain.height;
        desc.pixel_format = texture_format_for(chain.format, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
        desc.buffer_count = chain.buffer_count;
        desc.vsync = true;
        std::unique_ptr<graphics::BackendSwapChain> backend =
            m_backend.create_swap_chain(desc);
        if (!backend || !backend->valid()) {
            unimplemented("IDXGISwapChain::Present",
                          "a presenter the backend refused");
            return;
        }
        chain.backend = std::move(backend);
        apply_presentation_state(chain);
    }
    // IDXGISwapChain2::SetSourceSize: only [0, 0, w, h] is shown.
    const bool region = chain.source_width != 0 && chain.source_height != 0 &&
                        (chain.source_width < chain.width ||
                         chain.source_height < chain.height);
    bool shown = false;
    if (region) {
        graphics::BackendSwapChain::PresentRegion r{};
        r.width = chain.source_width;
        r.height = chain.source_height;
        shown = chain.backend->present_texture_region(resource->texture.get(), r,
                                                      sync_interval, flags);
    } else {
        shown = chain.backend->present_texture(resource->texture.get(),
                                               sync_interval, flags);
    }
    if (!shown) d3d12_diag("Present: the presenter refused the frame");
}

// =============================================================================
// IDXGISwapChain4
// =============================================================================

void Frontend::swap_chain_method(Marshaler& m, SwapChain& chain, usize slot) {
    using S = SwapChainSlot;
    const bool waitable = (chain.flags & kFlagFrameLatencyWaitable) != 0;
    switch (slot) {
    case S::QueryInterface: {
        const GuestAddr out = m.arg_ptr(2);
        if (out == 0) {
            m.set_return(E_POINTER);
            return;
        }
        GuidBytes iid{};
        const bool known =
            read_guid(m.arg_ptr(1), iid) &&
            (guid_equal(iid, IID_IUNKNOWN) || guid_equal(iid, IID_IDXGIOBJECT) ||
             guid_equal(iid, IID_IDXGIDEVICESUBOBJECT) ||
             guid_equal(iid, IID_IDXGISWAPCHAIN) ||
             guid_equal(iid, IID_IDXGISWAPCHAIN1) ||
             guid_equal(iid, IID_IDXGISWAPCHAIN2) ||
             guid_equal(iid, IID_IDXGISWAPCHAIN3) ||
             guid_equal(iid, IID_IDXGISWAPCHAIN4));
        (void)write_pointer(out, known ? chain.address : 0);
        if (known) add_ref(chain);
        m.set_return(known ? S_OK : E_NOINTERFACE);
        return;
    }
    case S::AddRef:
        add_ref(chain);
        m.set_return(chain.ref_count);
        return;
    case S::Release:
        m.set_return(release(chain.address));
        return;
    case S::SetPrivateData:
        m.set_return(set_private_data(m, chain));
        return;
    case S::SetPrivateDataInterface:
        m.set_return(set_private_data_interface(m, chain));
        return;
    case S::GetPrivateData:
        m.set_return(get_private_data(m, chain));
        return;
    case S::GetParent: {
        // GetParent(riid, ppParent): the factory that created the chain.
        const GuestAddr out = m.arg_ptr(2);
        if (out == 0) {
            m.set_return(DXGI_ERROR_INVALID_CALL);
            return;
        }
        if (chain.factory == 0 || m_presentation.dxgi == nullptr) {
            (void)write_pointer(out, 0);
            m.set_return(E_NOINTERFACE);
            return;
        }
        m.set_return(m_presentation.dxgi->query_object_interface(
            chain.factory, m.arg_ptr(1), out));
        return;
    }
    case S::GetDevice: {
        // IDXGIDeviceSubObject::GetDevice(riid, ppDevice): the queue's device
        // (measured: ID3D12Device yes, the queue and IDXGIDevice no).
        const GuestAddr out = m.arg_ptr(2);
        if (out == 0) {
            m.set_return(E_POINTER);
            return;
        }
        Device* device = find<Device>(chain.device, Kind::Device);
        GuidBytes iid{};
        const GuestAddr answer =
            device != nullptr && read_guid(m.arg_ptr(1), iid)
                ? query_interface(*device, iid)
                : 0;
        (void)write_pointer(out, answer);
        m.set_return(answer != 0 ? S_OK : E_NOINTERFACE);
        return;
    }
    case S::Present:
        m.set_return(present(chain, m.arg32(1), m.arg32(2), 0, 0));
        return;
    case S::Present1: {
        // Present1(SyncInterval, PresentFlags,
        //     const DXGI_PRESENT_PARAMETERS *: {UINT DirtyRectsCount;
        //     RECT *pDirtyRects; RECT *pScrollRect; POINT *pScrollOffset}).
        const GuestAddr params = m.arg_ptr(3);
        u32 count = 0;
        GuestAddr rects = 0;
        if (params != 0) {
            count = read_u32(params);
            (void)read_pointer(params + (m_is_64bit ? 8u : 4u), rects);
        }
        m.set_return(present(chain, m.arg32(1), m.arg32(2), rects, count));
        return;
    }
    case S::GetBuffer: {
        // GetBuffer(UINT Buffer, REFIID riid, void **ppSurface): an
        // ID3D12Resource (measured: IUnknown / Resource1 too; IDXGISurface,
        // IDXGIResource and ID3D12Heap are E_NOINTERFACE; a NULL
        // ppSurface or an index past the count is INVALID_CALL).
        const u32 index = m.arg32(1);
        const GuestAddr out = m.arg_ptr(3);
        if (out == 0) {
            m.set_return(DXGI_ERROR_INVALID_CALL);
            return;
        }
        (void)write_pointer(out, 0);
        Resource* resource =
            index < chain.buffers.size()
                ? find<Resource>(chain.buffers[index], Kind::Resource)
                : nullptr;
        GuidBytes iid{};
        if (resource == nullptr || !read_guid(m.arg_ptr(2), iid)) {
            m.set_return(DXGI_ERROR_INVALID_CALL);
            return;
        }
        const GuestAddr answer = query_interface(*resource, iid);
        (void)write_pointer(out, answer);
        m.set_return(answer != 0 ? S_OK : E_NOINTERFACE);
        return;
    }
    case S::SetFullscreenState: {
        // Learn: S_OK, DXGI_ERROR_NOT_CURRENTLY_AVAILABLE or
        // DXGI_STATUS_MODE_CHANGE_IN_PROGRESS; a waitable or composition
        // chain refuses the call (as measured for D3D11 chains).
        if (waitable || chain.composition) {
            m.set_return(DXGI_ERROR_INVALID_CALL);
            return;
        }
        if (m.arg32(1) != 0) {
            unimplemented("IDXGISwapChain::SetFullscreenState",
                          "exclusive full-screen (the chain stays windowed)");
            m.set_return(DXGI_ERROR_NOT_CURRENTLY_AVAILABLE);
            return;
        }
        m.set_return(S_OK);
        return;
    }
    case S::GetFullscreenState: {
        if (const GuestAddr state = m.arg_ptr(1); state != 0) {
            (void)m_memory.write_u32(state, 0);
        }
        if (const GuestAddr output = m.arg_ptr(2); output != 0) {
            (void)write_pointer(output, 0);
        }
        m.set_return(S_OK);
        return;
    }
    case S::GetDesc: {
        // DXGI_SWAP_CHAIN_DESC.
        const GuestAddr desc = m.arg_ptr(1);
        if (desc == 0) {
            m.set_return(E_INVALIDARG);
            return;
        }
        const u32 window_offset = m_is_64bit ? 48u : 44u;
        const u32 after = window_offset + (m_is_64bit ? 8u : 4u);
        std::vector<u8> zero(m_is_64bit ? 72u : 60u, 0u);
        (void)m_memory.write_bytes(desc, zero.data(), zero.size());
        (void)m_memory.write_u32(desc + 0, chain.width);
        (void)m_memory.write_u32(desc + 4, chain.height);
        (void)m_memory.write_u32(desc + 8, chain.refresh_numerator);
        (void)m_memory.write_u32(desc + 12, chain.refresh_denominator);
        (void)m_memory.write_u32(desc + 16, chain.format);
        (void)m_memory.write_u32(desc + 20, chain.scanline_ordering);
        (void)m_memory.write_u32(desc + 24, chain.fullscreen_scaling);
        (void)m_memory.write_u32(desc + 28, 1);
        (void)m_memory.write_u32(desc + 32, 0);
        (void)m_memory.write_u32(desc + 36, chain.buffer_usage);
        (void)m_memory.write_u32(desc + 40, chain.buffer_count);
        (void)write_pointer(desc + window_offset, chain.hwnd);
        (void)m_memory.write_u32(desc + after, 1);  // Windowed
        (void)m_memory.write_u32(desc + after + 4, chain.swap_effect);
        (void)m_memory.write_u32(desc + after + 8, chain.flags);
        m.set_return(S_OK);
        return;
    }
    case S::GetDesc1: {
        const GuestAddr desc = m.arg_ptr(1);
        if (desc == 0) {
            m.set_return(E_INVALIDARG);
            return;
        }
        const u32 values[12] = {chain.width,       chain.height,
                                chain.format,      chain.stereo,
                                1,                 0,
                                chain.buffer_usage, chain.buffer_count,
                                chain.scaling,     chain.swap_effect,
                                chain.alpha_mode,  chain.flags};
        for (u32 i = 0; i < 12; ++i) (void)m_memory.write_u32(desc + i * 4u, values[i]);
        m.set_return(S_OK);
        return;
    }
    case S::GetFullscreenDesc: {
        const GuestAddr desc = m.arg_ptr(1);
        if (desc == 0) {
            m.set_return(E_INVALIDARG);
            return;
        }
        if (chain.composition) {
            m.set_return(DXGI_ERROR_INVALID_CALL);
            return;
        }
        (void)m_memory.write_u32(desc + 0, chain.refresh_numerator);
        (void)m_memory.write_u32(desc + 4, chain.refresh_denominator);
        (void)m_memory.write_u32(desc + 8, chain.scanline_ordering);
        (void)m_memory.write_u32(desc + 12, chain.fullscreen_scaling);
        (void)m_memory.write_u32(desc + 16, 1);
        m.set_return(S_OK);
        return;
    }
    case S::GetHwnd: {
        const GuestAddr out = m.arg_ptr(1);
        if (out == 0) {
            m.set_return(E_INVALIDARG);
            return;
        }
        if (chain.composition) {
            (void)write_pointer(out, 0);
            m.set_return(DXGI_ERROR_INVALID_CALL);
            return;
        }
        (void)write_pointer(out, chain.hwnd);
        m.set_return(S_OK);
        return;
    }
    case S::GetCoreWindow: {
        // Not a CoreWindow chain (Learn: DXGI_ERROR_INVALID_CALL).
        if (const GuestAddr out = m.arg_ptr(2); out != 0) {
            (void)write_pointer(out, 0);
        }
        m.set_return(DXGI_ERROR_INVALID_CALL);
        return;
    }
    case S::ResizeBuffers:
    case S::ResizeBuffers1: {
        // ResizeBuffers(BufferCount, Width, Height, NewFormat,
        //     SwapChainFlags); ResizeBuffers1 adds const UINT
        //     *pCreationNodeMask and IUnknown *const *ppPresentQueue.
        u32 count = m.arg32(1);
        u32 width = m.arg32(2);
        u32 height = m.arg32(3);
        u32 format = m.arg32(4);
        const u32 flags = m.arg32(5);
        // Measured: while the application holds a buffer, INVALID_CALL.
        for (const GuestAddr buffer : chain.buffers) {
            const Resource* r = find<Resource>(buffer, Kind::Resource);
            if (r != nullptr && r->ref_count > 1) {
                m.set_return(DXGI_ERROR_INVALID_CALL);
                return;
            }
        }
        if (count == 0) count = chain.buffer_count;
        if (format == DXGI_FORMAT_UNKNOWN) format = chain.format;
        // Measured: one buffer, an sRGB format, or adding ALLOW_TEARING /
        // FRAME_LATENCY_WAITABLE_OBJECT is E_INVALIDARG.
        if (count < 2 || count > 16 || !flip_format(format) ||
            ((flags ^ chain.flags) &
             (kFlagAllowTearing | kFlagFrameLatencyWaitable)) != 0) {
            m.set_return(E_INVALIDARG);
            return;
        }
        if (slot == S::ResizeBuffers1) {
            const GuestAddr masks = m.arg_ptr(6);
            const GuestAddr queues = m.arg_ptr(7);
            if (masks == 0 || queues == 0) {
                m.set_return(DXGI_ERROR_INVALID_CALL);
                return;
            }
            const usize pointer_size = m_is_64bit ? 8u : 4u;
            for (u32 index = 0; index < count; ++index) {
                GuestAddr q = 0;
                (void)read_pointer(queues + index * pointer_size, q);
                const CommandQueue* present_queue =
                    find<CommandQueue>(q, Kind::CommandQueue);
                if (read_u32(masks + index * 4u) != 1 || present_queue == nullptr ||
                    present_queue->device != chain.device) {
                    m.set_return(DXGI_ERROR_INVALID_CALL);
                    return;
                }
            }
        }
        if ((width == 0 || height == 0) && !chain.composition) {
            const auto client = m_presentation.client_size
                                    ? m_presentation.client_size(chain.hwnd)
                                    : std::nullopt;
            if (client) {
                width = std::max(1u, client->first);
                height = std::max(1u, client->second);
            }
        }
        if (width == 0 || height == 0) {
            m.set_return(DXGI_ERROR_INVALID_CALL);
            return;
        }
        chain.width = width;
        chain.height = height;
        chain.format = format;
        chain.buffer_count = count;
        chain.flags = flags;
        chain.source_width = 0;
        chain.source_height = 0;
        if (u32 error = create_swap_chain_buffers(chain); error != S_OK) {
            m.set_return(error);
            return;
        }
        if (chain.backend &&
            !chain.backend->resize(width, height,
                                   texture_format_for(format, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET),
                                   count)) {
            chain.backend.reset();  // re-created at the next Present
        }
        m.set_return(S_OK);
        return;
    }
    case S::ResizeTarget: {
        const GuestAddr mode = m.arg_ptr(1);
        if (mode == 0 || chain.composition || !m_presentation.resize_client) {
            m.set_return(DXGI_ERROR_INVALID_CALL);
            return;
        }
        m.set_return(m_presentation.resize_client(chain.hwnd, read_u32(mode),
                                                  read_u32(mode + 4))
                         ? S_OK
                         : DXGI_ERROR_INVALID_CALL);
        return;
    }
    case S::GetContainingOutput: {
        const GuestAddr out = m.arg_ptr(1);
        if (out == 0) {
            m.set_return(DXGI_ERROR_INVALID_CALL);
            return;
        }
        (void)write_pointer(out, 0);
        std::optional<std::array<i32, 4>> rect;
        if (!chain.composition && m_presentation.window_rect) {
            rect = m_presentation.window_rect(chain.hwnd);
        }
        DxgiManager* dxgi = m_presentation.dxgi;
        GuestAddr output = 0;
        if (dxgi != nullptr && rect) {
            const GuestAddr adapter = dxgi->create_default_adapter();
            if (adapter != 0) {
                output = dxgi->create_containing_output(
                    adapter, (*rect)[0], (*rect)[1], (*rect)[2], (*rect)[3]);
                dxgi->release_adapter(adapter);
            }
        }
        if (output == 0) {
            m.set_return(DXGI_ERROR_UNSUPPORTED);
            return;
        }
        (void)write_pointer(out, output);
        m.set_return(S_OK);
        return;
    }
    case S::GetFrameStatistics: {
        // DXGI_FRAME_STATISTICS: PresentCount, PresentRefreshCount,
        // SyncRefreshCount (UINTs), SyncQPCTime, SyncGPUTime (8-aligned).
        const GuestAddr stats = m.arg_ptr(1);
        if (stats == 0) {
            m.set_return(DXGI_ERROR_INVALID_CALL);
            return;
        }
        const std::array<u8, 32> zero{};
        (void)m_memory.write_bytes(stats, zero.data(), zero.size());
        if (chain.present_count == 0) {
            m.set_return(DXGI_ERROR_FRAME_STATISTICS_DISJOINT);
            return;
        }
        double hz = 0.0;
        if (m_presentation.dxgi != nullptr) {
            for (const auto& output : m_presentation.dxgi->output_infos()) {
                hz = std::max(hz, output.refresh_rate_hz);
            }
        }
        if (!std::isfinite(hz) || hz < 1.0) hz = 60.0;
        const u64 period =
            std::max<u64>(1, static_cast<u64>(static_cast<double>(kQpcHz) / hz));
        const u64 now = virtual_qpc_ticks();
        const u64 sync_refresh = now / period;
        const u64 present_refresh =
            std::min(sync_refresh, (chain.last_present_qpc + period - 1) / period);
        (void)m_memory.write_u32(stats + 0, chain.present_count);
        (void)m_memory.write_u32(stats + 4, static_cast<u32>(present_refresh));
        (void)m_memory.write_u32(stats + 8, static_cast<u32>(sync_refresh));
        (void)m_memory.write_u64(stats + 16, sync_refresh * period);
        m.set_return(S_OK);
        return;
    }
    case S::GetLastPresentCount: {
        const GuestAddr out = m.arg_ptr(1);
        if (out == 0) {
            m.set_return(DXGI_ERROR_INVALID_CALL);
            return;
        }
        (void)m_memory.write_u32(out, chain.present_count);
        m.set_return(S_OK);
        return;
    }
    case S::IsTemporaryMonoSupported:
        m.set_return(0);
        return;
    case S::GetRestrictToOutput: {
        const GuestAddr out = m.arg_ptr(1);
        if (out == 0) {
            m.set_return(E_INVALIDARG);
            return;
        }
        const bool held = chain.restrict_output != 0 &&
                          m_presentation.dxgi != nullptr &&
                          m_presentation.dxgi->retain_object(chain.restrict_output);
        (void)write_pointer(out, held ? chain.restrict_output : 0);
        m.set_return(S_OK);
        return;
    }
    case S::SetBackgroundColor:
    case S::GetBackgroundColor: {
        // DXGI_RGBA: four floats.
        const GuestAddr rgba = m.arg_ptr(1);
        if (rgba == 0) {
            m.set_return(E_INVALIDARG);
            return;
        }
        if (slot == S::SetBackgroundColor) {
            for (u32 c = 0; c < 4; ++c) chain.background[c] = read_f32(rgba + c * 4u);
            if (chain.backend) (void)chain.backend->set_background_color(chain.background);
        } else {
            for (u32 c = 0; c < 4; ++c) {
                u32 bits = 0;
                std::memcpy(&bits, &chain.background[c], 4);
                (void)m_memory.write_u32(rgba + c * 4u, bits);
            }
        }
        m.set_return(S_OK);
        return;
    }
    case S::SetRotation: {
        // DXGI_MODE_ROTATION: IDENTITY 1 .. ROTATE270 4.
        const u32 rotation = m.arg32(1);
        if (rotation < 1 || rotation > 4) {
            m.set_return(DXGI_ERROR_INVALID_CALL);
            return;
        }
        chain.rotation = rotation;
        if (chain.backend && !chain.backend->set_rotation(rotation - 1)) {
            unimplemented("IDXGISwapChain1::SetRotation",
                          "the presenter does not rotate the image");
        }
        m.set_return(S_OK);
        return;
    }
    case S::GetRotation: {
        const GuestAddr out = m.arg_ptr(1);
        if (out == 0) {
            m.set_return(E_INVALIDARG);
            return;
        }
        (void)m_memory.write_u32(out, chain.rotation);
        m.set_return(S_OK);
        return;
    }
    case S::SetSourceSize: {
        const u32 width = m.arg32(1);
        const u32 height = m.arg32(2);
        if (width == 0 || height == 0 || width > chain.width ||
            height > chain.height) {
            m.set_return(DXGI_ERROR_INVALID_CALL);
            return;
        }
        chain.source_width = width;
        chain.source_height = height;
        m.set_return(S_OK);
        return;
    }
    case S::GetSourceSize: {
        const GuestAddr w = m.arg_ptr(1);
        const GuestAddr h = m.arg_ptr(2);
        if (w == 0 || h == 0) {
            m.set_return(E_INVALIDARG);
            return;
        }
        (void)m_memory.write_u32(w, chain.source_width != 0 ? chain.source_width
                                                            : chain.width);
        (void)m_memory.write_u32(h, chain.source_height != 0 ? chain.source_height
                                                             : chain.height);
        m.set_return(S_OK);
        return;
    }
    case S::SetMaximumFrameLatency: {
        const u32 latency = m.arg32(1);
        if (!waitable || latency == 0 || latency > 16) {
            m.set_return(DXGI_ERROR_INVALID_CALL);
            return;
        }
        chain.max_frame_latency = latency;
        if (chain.latency) {
            std::lock_guard<std::mutex> hold(chain.latency->mutex);
            chain.latency->cap = latency;
        }
        m.set_return(S_OK);
        return;
    }
    case S::GetMaximumFrameLatency: {
        const GuestAddr out = m.arg_ptr(1);
        if (out == 0 || !waitable) {
            m.set_return(DXGI_ERROR_INVALID_CALL);
            return;
        }
        (void)m_memory.write_u32(out, chain.max_frame_latency);
        m.set_return(S_OK);
        return;
    }
    case S::GetFrameLatencyWaitableObject: {
        // HANDLE: a new handle to the chain's semaphore, which starts full
        // (Learn: one count per frame of latency).
        if (!waitable || !chain.latency || !m_presentation.create_semaphore ||
            !m_presentation.duplicate_handle) {
            m.set_return(0);
            return;
        }
        std::lock_guard<std::mutex> hold(chain.latency->mutex);
        if (chain.latency->handle == 0) {
            chain.latency->cap = chain.max_frame_latency;
            chain.latency->handle = m_presentation.create_semaphore(
                static_cast<i32>(chain.max_frame_latency), 16);
        }
        m.set_return(chain.latency->handle != 0
                         ? m_presentation.duplicate_handle(chain.latency->handle)
                         : 0);
        return;
    }
    case S::SetMatrixTransform:
    case S::GetMatrixTransform: {
        // DXGI_MATRIX_3X2_F: composition chains only (Learn).
        const GuestAddr matrix = m.arg_ptr(1);
        if (!chain.composition || matrix == 0) {
            m.set_return(DXGI_ERROR_INVALID_CALL);
            return;
        }
        for (u32 i = 0; i < 6; ++i) {
            if (slot == S::SetMatrixTransform) {
                chain.matrix[i] = read_f32(matrix + i * 4u);
            } else {
                u32 bits = 0;
                std::memcpy(&bits, &chain.matrix[i], 4);
                (void)m_memory.write_u32(matrix + i * 4u, bits);
            }
        }
        m.set_return(S_OK);
        return;
    }
    case S::GetCurrentBackBufferIndex:
        m.set_return(chain.current);
        return;
    case S::CheckColorSpaceSupport: {
        // CheckColorSpaceSupport(DXGI_COLOR_SPACE_TYPE, UINT *pSupport):
        // PRESENT (1) for what the presenter can tag with this buffer format.
        const u32 space = m.arg32(1);
        const GuestAddr out = m.arg_ptr(2);
        if (out == 0) {
            m.set_return(E_INVALIDARG);
            return;
        }
        const bool present =
            space == kColorSpaceSrgb ||
            (space == kColorSpaceScRgb &&
             chain.format == DXGI_FORMAT_R16G16B16A16_FLOAT) ||
            ((space == kColorSpaceHdr10 || space == kColorSpaceBt2020G22) &&
             chain.format == DXGI_FORMAT_R10G10B10A2_UNORM);
        (void)m_memory.write_u32(out, present ? 1u : 0u);
        m.set_return(S_OK);
        return;
    }
    case S::SetColorSpace1: {
        // Learn (SetColorSpace1): E_INVALIDARG for a space
        // CheckColorSpaceSupport does not report PRESENT for.
        const u32 space = m.arg32(1);
        const bool known =
            space == kColorSpaceSrgb ||
            (space == kColorSpaceScRgb &&
             chain.format == DXGI_FORMAT_R16G16B16A16_FLOAT) ||
            ((space == kColorSpaceHdr10 || space == kColorSpaceBt2020G22) &&
             chain.format == DXGI_FORMAT_R10G10B10A2_UNORM);
        if (!known) {
            m.set_return(E_INVALIDARG);
            return;
        }
        chain.color_space = space;
        apply_presentation_state(chain);
        m.set_return(S_OK);
        return;
    }
    case S::SetHDRMetaData: {
        // SetHDRMetaData(DXGI_HDR_METADATA_TYPE, UINT Size, void *pMetaData).
        const u32 type = m.arg32(1);
        const u32 size = m.arg32(2);
        const GuestAddr data = m.arg_ptr(3);
        if (type == kHdrMetadataNone) {
            chain.hdr_metadata_type = type;
            chain.hdr_metadata.clear();
            if (chain.backend) (void)chain.backend->set_hdr10_metadata({});
            m.set_return(S_OK);
            return;
        }
        if (type != kHdrMetadataHdr10 || size != kHdr10MetadataBytes || data == 0) {
            m.set_return(E_INVALIDARG);
            return;
        }
        chain.hdr_metadata.resize(size);
        if (m_memory.read_bytes(data, chain.hdr_metadata.data(), size).is_err()) {
            m.set_return(E_INVALIDARG);
            return;
        }
        chain.hdr_metadata_type = type;
        if (chain.backend) (void)chain.backend->set_hdr10_metadata(chain.hdr_metadata);
        m.set_return(S_OK);
        return;
    }
    default:
        break;
    }
    unimplemented("IDXGISwapChain4", "slot");
    m.set_return(E_NOTIMPL);
}

}  // namespace vitreo::winapi::d3d12

// =============================================================================
// D3D12Manager
// =============================================================================

namespace vitreo::winapi {

void D3D12Manager::set_presentation_services(PresentationServices services) {
    m_frontend->m_presentation = std::move(services);
}

void D3D12Manager::impl_CreateSwapChain(Marshaler& m) {
    m_frontend->create_swap_chain(m, d3d12::Frontend::SwapChainApi::Legacy);
}

void D3D12Manager::impl_CreateSwapChainForHwnd(Marshaler& m) {
    m_frontend->create_swap_chain(m, d3d12::Frontend::SwapChainApi::Hwnd);
}

void D3D12Manager::impl_CreateSwapChainForComposition(Marshaler& m) {
    m_frontend->create_swap_chain(m, d3d12::Frontend::SwapChainApi::Composition);
}

}  // namespace vitreo::winapi
