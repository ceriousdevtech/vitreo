// SPDX-License-Identifier: LGPL-2.1-only
/// @file hwnd_registry.hpp
/// @brief Capsule-scoped HWND discovery so peer Vitreo host processes can
///        honor IsWindow / GetDC / GDI present against windows they did not
///        create (required for a browser GPU process to present into a
///        browser-process HWND, including a software renderer).

#pragma once

#include <vitreo/common.hpp>

#include <array>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace vitreo::ipc {

struct HwndRegistryEntry {
    u32 hwnd{0};
    u32 owner_host_pid{0};
    u32 owner_guest_thread_id{0};
    u32 parent_hwnd{0};
    i32 client_w{0};
    i32 client_h{0};
    /// The guest window's WS_VISIBLE state, as the OWNING process knows it.
    ///
    /// A peer that presents into this window needs this: IDXGISwapChain::Present
    /// draws into a window's client area and says nothing about whether the
    /// window is shown (Microsoft Learn). The guest creates popup HWNDs up front and
    /// presents into them while they are still hidden, so treating "a frame
    /// arrived" as "show this window" puts an uninitialised plate on screen.
    ///
    /// Defaults to true so an unpublished or older record behaves exactly as
    /// before: a window is hidden only when a publisher SAYS it is hidden.
    bool visible{true};
    /// PID of the process that hosts an on-screen peer view for this HWND,
    /// or 0 when nobody does.
    ///
    /// A compositing child created by the GPU process is presented into by
    /// that same process, so neither the owner check nor the parent check can
    /// find a transport target -- the parent is GPU-local too. The process
    /// that actually shows the pixels is the one hosting a peer NSView for the
    /// child, and only it knows that. Without publishing it, the frames are
    /// rendered into a detached offscreen view and never reach the screen.
    /// A launcher content pane stayed black for exactly this reason while its
    /// loading window (whose child the browser happened to parent under a
    /// browser-owned HWND) rendered fine.
    u32 present_host_pid{0};
};

/// Allocate an HWND value unique within the capsule's interactive desktop.
/// Returns zero when no IPC scope is configured or allocation fails.
[[nodiscard]] u32 hwnd_registry_allocate_handle();

/// Allocate a guest thread ID unique within the capsule (Windows TIDs are
/// system-wide). Returns zero when no IPC scope is configured; callers then
/// keep a process-local counter in isolated tests. Runtime primary threads
/// and worker threads use the same allocator.
[[nodiscard]] u32 allocate_system_thread_id();

/// Publish or refresh a guest HWND owned by this host process.
/// No-op when VITREO_IPC_SCOPE is unset or arguments are invalid.
/// `class_name` and `title` are UTF-8 and are what makes a SYSTEM-WIDE
/// FindWindow possible; see hwnd_registry_find_window.
///
/// Publish the creator's actual thread identity without a PID remap.
void hwnd_registry_publish(u32 hwnd, i32 client_w, i32 client_h,
                           u32 owner_guest_thread_id = 0,
                           std::string_view class_name = {},
                           std::string_view title = {});

/// Find a published window by class and/or title across EVERY live guest
/// process in the capsule, which is what Win32 FindWindow does -- it searches
/// the desktop, not the caller's process. An empty field matches any value,
/// and both empty returns nullopt (Win32 returns NULL for FindWindow(NULL,
/// NULL)). Comparison is case-insensitive, as Microsoft Learn specifies.
[[nodiscard]] std::optional<HwndRegistryEntry> hwnd_registry_find_window(
    std::string_view class_name, std::string_view title);

/// Change a desktop HWND's parent, including when the caller and child are in
/// different guest processes. Returns the previous parent on success.
[[nodiscard]] std::optional<u32> hwnd_registry_set_parent(u32 hwnd,
                                                          u32 parent_hwnd);

/// Publish a desktop HWND's WS_VISIBLE state so peer processes presenting
/// into it can honour Win32 visibility instead of inferring it from presents.
/// No-op when the HWND is not published.
void hwnd_registry_set_visible(u32 hwnd, bool visible);

/// Record that @p host_pid shows an on-screen peer view for @p hwnd. Pass 0 to
/// clear. Published beside the record like visibility, so a geometry-only
/// publish cannot clobber it.
void hwnd_registry_set_present_host(u32 hwnd, u32 host_pid);

/// One value in a window's shell property store (SHGetPropertyStoreForWindow).
/// Windows keeps these values on the window as text, in insertion order;
/// removing one moves the last value into its slot
/// (tools/oracle_probe/window_propstore_probe64.expected).
struct WindowShellProperty {
    std::array<u8, 16> fmtid{};
    u32 pid{0};
    std::u16string text;
};

/// The shell properties stored on @p hwnd. They are per WINDOW, not per
/// process: a peer process that opens the store sees and changes the same
/// values (probe "peer-store"). Capsule-wide when VITREO_IPC_SCOPE is set,
/// process-local otherwise. Empty when nothing is stored.
[[nodiscard]] std::vector<WindowShellProperty> hwnd_registry_shell_properties(
    u32 hwnd);

/// Edit @p hwnd's shell properties under the store's lock. Returns false when
/// the values could not be read or written back.
bool hwnd_registry_update_shell_properties(
    u32 hwnd, const std::function<void(std::vector<WindowShellProperty>&)>& edit);

/// Remove a previously published HWND (best-effort). Also drops its shell
/// properties: a destroyed window's store holds nothing (probe
/// "after-destroy").
void hwnd_registry_unpublish(u32 hwnd);

/// Look up a published HWND. Returns nullopt when missing or owner is dead.
[[nodiscard]] std::optional<HwndRegistryEntry> hwnd_registry_lookup(u32 hwnd);

/// Live host pids that own at least one published window, excluding this
/// process. macOS activation is PER PROCESS: a Dock click activates only the
/// app-bundle process, so a capsule whose visible window belongs to a peer
/// (a browser/GPU process split) never comes forward. The caller uses this to
/// activate those peers too.
[[nodiscard]] std::vector<u32> hwnd_registry_live_peer_owners();

/// True when the HWND is published by a live peer (or this process).
[[nodiscard]] bool hwnd_registry_is_window(u32 hwnd);

/// Every live published window in the capsule (all processes, including the
/// caller's own), ordered by HWND. Dead owners' records are reaped.
[[nodiscard]] std::vector<HwndRegistryEntry> hwnd_registry_enumerate();

} // namespace vitreo::ipc
