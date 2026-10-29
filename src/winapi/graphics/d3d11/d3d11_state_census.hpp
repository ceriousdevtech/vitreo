// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file d3d11_state_census.hpp
/// @brief VITREO_D3D11_STATE_CENSUS=1: the D3D11 state census.
///
/// DIAGNOSTIC, not a lever and not a probe: it changes no behaviour. It counts
/// what the frontend sees -- per guest draw, which draw-state groups the
/// guest's setters touched and which actually changed value since the
/// context's previous draw; per frame, crossings by interface class,
/// AddRef/Release, CreateBuffer by thread, the device lock's wait and hold
/// times, ID3D11Multithread use -- and prints `[r3-census]` lines every 2^16
/// guest draws. The backend's half (retains and changed CsDraw fields per
/// recorded draw) is metal_submit_queue_census.mm's `[r3-census] backend` line.
///
/// Off (the default) it costs one predictable branch per guest COM entry and
/// two per draw: `enabled` is a constant read once at load.

#include <vitreo/types.hpp>

namespace vitreo::winapi {

class D3D11Manager;
class Marshaler;

namespace d3d11_census {

/// VITREO_D3D11_STATE_CENSUS set to anything but "0". Read once, at load.
extern const bool enabled;

/// The interface a COM entry belongs to (one per D3D11Manager::impl_*Method).
enum class Iface : u8 {
    Context,
    Annotation,
    VideoContext,
    Device,
    Multithread,
    DxgiDevice,
    DxgiResource,
    DxgiSurface,
    SwapChain,
    SwapChainMedia,
    KeyedMutex,
    Buffer,
    Texture,
    ShaderResourceView,
    UnorderedAccessView,
    RenderTargetView,
    DepthStencilView,
    Query,
    Fence,
    CommandList,
    Shader,
    InputLayout,
    StateObject,
    ClassLinkage,
    DeviceContextState,
    Other,
    Count
};

/// One guest COM entry, timed around the device lock: d3d11_com_method
/// fills `t_enter` before it takes the lock and `t_locked` once it holds
/// it, then calls before_method, the method, and after_method.
struct Crossing {
    Iface iface{Iface::Other};
    u32 slot{0};
    u64 t_enter{0};
    u64 t_locked{0};
    /// The entry took the lock first on this thread (depth was 0): only
    /// these holds go into the hold histogram, a nested entry runs inside
    /// one.
    bool outer{false};
};

[[nodiscard]] u64 now_ns() noexcept;
/// Lock held, before the method: per-call counters keyed by the method.
void before_method(const Crossing& crossing, const Marshaler& m) noexcept;
/// Lock still held, after the method: hold/wait histograms, frame
/// boundaries (IDXGISwapChain::Present / Present1).
void after_method(const Crossing& crossing) noexcept;

}  // namespace d3d11_census
}  // namespace vitreo::winapi
