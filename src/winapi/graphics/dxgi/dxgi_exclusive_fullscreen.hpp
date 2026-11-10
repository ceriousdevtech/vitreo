// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file dxgi_exclusive_fullscreen.hpp
/// @brief DXGI swap-chain exclusive full-screen, shared by the D3D11 and the
///        D3D12 swap chains: IDXGISwapChain::SetFullscreenState,
///        GetFullscreenState and the full-screen half of ResizeTarget.
///
/// Sources: Learn IDXGISwapChain::SetFullscreenState, GetFullscreenState,
/// ResizeTarget; "DXGI Overview" (Choosing the DXGI Output and Size,
/// Switching Modes); "DXGI Best Practices" (Full-Screen Issues);
/// DXGI_SWAP_CHAIN_FLAG. Measured: tools/oracle_probe/
/// d3d12_a73_swapchain_fullscreen_probe (.winoracle-gpu.expected: RTX 4080,
/// the desktop user's session; .expected: WARP, a disconnected session) and
/// dxgi_exclusive_fullscreen_probe.
///
/// The guest display mode goes through the display-mode owner DirectDraw and
/// D3D9 exclusive mode share (user32's virtual display mode), and the host
/// presents it as a macOS Space, never as a covering window
/// (.docs/windowing-native-presentation.md rules 3 and 6).

#include <vitreo/common.hpp>

#include <array>
#include <atomic>
#include <functional>
#include <optional>
#include <utility>

namespace vitreo::winapi {
class DxgiManager;
}

namespace vitreo::winapi::dxgi {

/// What exclusive full-screen needs of the window system. Unset members mean
/// no window system is attached (unit tests without one): entering then
/// fails with DXGI_ERROR_NOT_CURRENTLY_AVAILABLE.
struct ExclusiveWindowServices {
    /// Client-area size; nullopt when @p hwnd is not a window.
    std::function<std::optional<std::pair<u32, u32>>(u64 hwnd)> client_size;
    /// Window rectangle in desktop coordinates (left, top, right, bottom).
    std::function<std::optional<std::array<i32, 4>>(u64 hwnd)> window_rect;
    /// The shared exclusive owner (User32ExtManager::enter_exclusive_display):
    /// with @p change_mode publish width x height as the display mode and
    /// size the window to it; otherwise the window covers @p output.
    std::function<bool(u64 hwnd, std::array<i32, 4> output, u32 width,
                       u32 height, bool change_mode)>
        enter_exclusive;
    /// Leave it: the desktop mode when @p mode_changed, then the window
    /// rectangle when @p restore is set.
    std::function<void(u64 hwnd, bool mode_changed,
                       std::optional<std::array<i32, 4>> restore)>
        leave_exclusive;
    /// The host presentation: a macOS Space (CocoaBridge::
    /// set_window_fullscreen).
    std::function<void(u64 hwnd, bool enable)> host_fullscreen;
    /// Whether the guest application is the foreground one.
    std::function<bool()> application_active;
    /// Whether the host reports the window entirely occluded.
    std::function<bool(u64 hwnd)> window_occluded;
    /// Whether @p hwnd or a descendant has keyboard focus.
    std::function<bool(u64 hwnd)> has_keyboard_focus;
};

/// One swap chain's exclusive full-screen state.
struct ExclusiveState {
    /// GetFullscreenState's answer.
    bool fullscreen{false};
    /// While full-screen: the IDXGIOutput GetFullscreenState returns, held
    /// with one reference. It is pTarget itself when SetFullscreenState was
    /// given one (measured a73: "passed=1"), else a new output object.
    GuestAddr output{0};
    /// The mode the output is in, and whether it was switched to
    /// (ALLOW_MODE_SWITCH) or is the desktop's.
    u32 mode_width{0};
    u32 mode_height{0};
    bool mode_changed{false};
    /// The window rectangle before full-screen, restored on leaving.
    std::optional<std::array<i32, 4>> windowed_rect;
    /// The host window is in a macOS Space for this chain.
    bool host_space{false};
    /// D3D11's focus model: full-screen was relinquished because the
    /// application lost focus; Present reports DXGI_STATUS_OCCLUDED until
    /// focus returns.
    bool occluded_by_focus_loss{false};
    /// A flip-model chain's buffers were made for the other state: it went
    /// through a full-screen / windowed transition since its last
    /// ResizeBuffers, and Present (TEST included) is DXGI_ERROR_INVALID_CALL
    /// until the next (Learn SetFullscreenState: "you must call ResizeBuffers
    /// to ensure that your call to IDXGISwapChain1::Present1 succeeds").
    /// Each transition toggles it: measured a73 on D3D11 and D3D12 flip
    /// chains, TRUE -> ResizeBuffers -> FALSE asks again, while TRUE -> FALSE
    /// with no ResizeBuffers between presents windowed again (present12
    /// resize=0). A chain created full-screen (Windowed=FALSE) has buffers
    /// for that state; its creation clears it (not measured).
    bool needs_resize_buffers{false};
};

/// What a transition needs to know about the chain.
struct ExclusiveRequest {
    u64 hwnd{0};
    /// The IDXGIAdapter whose outputs the chain's device presents to.
    GuestAddr adapter{0};
    u32 buffer_width{0};
    u32 buffer_height{0};
    u32 flags{0};  // DXGI_SWAP_CHAIN_FLAG
    bool flip_model{false};
    /// The HWND is not a window (every Present is occluded).
    bool window_missing{false};
};

/// Runs @p transition with the caller's API locks released: the transition
/// sends the window messages (WM_DISPLAYCHANGE, WM_SIZE) the application
/// handles, possibly on another thread (Learn, Multithreading and DXGI).
using UnlockedScope = std::function<void(const std::function<void()>& transition)>;
/// Re-finds the chain's state after an unlocked transition: nullptr when the
/// chain is gone.
using StateLookup = std::function<ExclusiveState*()>;
using DiagFn = void (*)(const char* format, ...);

/// The process-wide part: one chain of the process holds the display mode
/// (the guest has one desktop), and whether a transition is running. Lives
/// in DxgiManager so the D3D11 and D3D12 chains share it.
/// Atomic: the D3D11 chains use it under the DXGI API lock, the D3D12 chains
/// under the D3D12 frontend lock.
struct ExclusiveRegistry {
    std::atomic<GuestAddr> owner{0};
    std::atomic<bool> transition{false};
};

/// SetFullscreenState(TRUE, @p target): choose the output and mode, publish
/// the mode through the shared exclusive owner and enter the host Space.
/// The caller has refused composition chains and the D3D11 waitable rule.
/// Returns S_OK, DXGI_ERROR_INVALID_CALL, DXGI_ERROR_NOT_CURRENTLY_AVAILABLE
/// or DXGI_STATUS_MODE_CHANGE_IN_PROGRESS.
[[nodiscard]] u32 enter_exclusive(DxgiManager& dxgi,
                                  const ExclusiveWindowServices& window,
                                  GuestAddr self, const ExclusiveRequest& request,
                                  GuestAddr target, const StateLookup& state,
                                  const UnlockedScope& unlocked, DiagFn diag);

/// Leave exclusive full-screen: the desktop mode and window rectangle come
/// back; @p leave_host_space also leaves the macOS Space.
void leave_exclusive(DxgiManager& dxgi, const ExclusiveWindowServices& window,
                     GuestAddr self, u64 hwnd, bool flip_model,
                     const StateLookup& state, bool leave_host_space,
                     const UnlockedScope& unlocked, DiagFn diag);

/// ResizeTarget on a full-screen chain: the output switches to the supported
/// mode closest to width x height (with ALLOW_MODE_SWITCH; without it the
/// desktop mode stays and the call succeeds).
[[nodiscard]] u32 resize_exclusive_target(DxgiManager& dxgi,
                                          const ExclusiveWindowServices& window,
                                          GuestAddr self, u64 hwnd, u32 flags,
                                          u32 width, u32 height,
                                          const StateLookup& state,
                                          const UnlockedScope& unlocked,
                                          DiagFn diag);

/// SetFullscreenState(FALSE, pTarget) validation: Learn says pTarget "must"
/// be NULL; measured (a73) a non-NULL pTarget is DXGI_ERROR_INVALID_CALL on
/// a full-screen chain and accepted (S_OK, nothing to do) on a windowed one.
[[nodiscard]] inline bool windowed_target_refused(const ExclusiveState& state,
                                                  GuestAddr target) {
    return target != 0 && state.fullscreen;
}

/// The GetFullscreenState out-parameters: TRUE and the output (one new
/// reference) while full-screen, else FALSE and NULL. @p write_bool and
/// @p write_pointer write guest memory; a failed pointer write gives the
/// reference back.
void report_fullscreen_state(DxgiManager& dxgi, const ExclusiveState& state,
                             const std::function<void(u32)>& write_bool,
                             const std::function<bool(GuestAddr)>& write_pointer);

}  // namespace vitreo::winapi::dxgi
