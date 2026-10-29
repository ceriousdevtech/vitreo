// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// CLI-002: cross-process window, message and guest-call handlers of the process memory endpoint

#include "run_internal.hpp"
#include "runtime/process/runtime_context.hpp"
#include "runtime/execution/guest_run_loop.hpp"
#include "core/cpu/state/cpu_state.hpp"
#include "core/memory/guest_memory.hpp"
#include "ipc/hwnd_registry.hpp"
#include "winapi/kernel32/memory/remote_memory.hpp"
#include "winapi/user32/messages/cross_process_send.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <span>
#include <vector>

namespace vitreo::cli {

void install_remote_memory_handlers(
    ipc::RemoteMemoryServer& remote_memory_server, RuntimeContext& ctx,
    GuestMemory& memory, bool is_64bit, u64 peb_address,
    u64 pe_tls_data_start, u32 pe_tls_data_size,
    std::optional<GuestThreadContext>& com_rpc_thread_context,
    bool& com_rpc_jit_initialized) {
    remote_memory_server.set_window_present_handler(
        [&ctx](u32 hwnd, std::span<const vitreo::Byte> rgba, i32 width,
               i32 height) -> bool {
            if (rgba.empty() || width <= 0 || height <= 0) return false;
            ctx.cocoa_bridge.update_window_pixels(
                static_cast<windowing::HWND>(hwnd), rgba.data(), width, height,
                /*above_native_children=*/true);
            return true;
        });
    // Serve WM_NCHITTEST for peers. A process that does not own the window
    // under the cursor previously answered HTCLIENT on its behalf, which
    // reports "no caption anywhere" and silently disables dragging for every
    // cross-process browser window.
    remote_memory_server.set_window_hit_test_handler(
        [&ctx](u32 hwnd, i32 screen_x, i32 screen_y) -> i32 {
            return ctx.u32_ext_mgr.local_hit_test(
                static_cast<windowing::HWND>(hwnd), screen_x, screen_y);
        });
    remote_memory_server.set_window_surface_present_handler(
        [&ctx](u32 hwnd, u64 shared_handle, i32 width, i32 height,
               u32 sync_interval, u32 src_x, u32 src_y) -> bool {
            // Peer GPU children may present before SetParent IPC arrives.
            // Host the child under its registered parent so Metal binds to
            // that HWND instead of covering every sibling on the parent.
            const auto remote = ipc::hwnd_registry_lookup(hwnd);
            if (remote && remote->parent_hwnd != 0 &&
                (ctx.cocoa_bridge.has_control_view(
                     static_cast<windowing::HWND>(remote->parent_hwnd)) ||
                 ctx.cocoa_bridge.has_native_window(
                     static_cast<windowing::HWND>(remote->parent_hwnd)))) {
                const i32 host_w = remote->client_w > 1
                    ? remote->client_w
                    : 1;
                const i32 host_h = remote->client_h > 1
                    ? remote->client_h
                    : 1;
                if (!ctx.cocoa_bridge.has_control_view(
                        static_cast<windowing::HWND>(hwnd))) {
                    (void)ctx.cocoa_bridge.ensure_hosted_peer_child(
                        static_cast<windowing::HWND>(remote->parent_hwnd),
                        static_cast<windowing::HWND>(hwnd),
                        windowing::Rect{0, 0, host_w, host_h});
                } else {
                    // Present draws into the HWND client; it does not
                    // SetWindowPos (Learn: IDXGISwapChain::Present). Size
                    // placeholders from the registered window, never the
                    // DXGI surface — a 1×1 popup must not expand to the
                    // compositor buffer and cover sibling content.
                    //
                    // The registered client area and the DirectComposition
                    // clip BOTH bound what this HWND shows, so the visible
                    // extent is their INTERSECTION. `width`/`height` arrive
                    // already reduced to the visual's clip by the producer
                    // (D3D11Manager::present_swap_chain), so the registry
                    // alone sizes a pooled 512x512 composition buffer's view
                    // at 512x512 while the present fills only 64x64, leaving
                    // the uninitialised remainder on screen as a white box
                    // over sibling content. The present alone would re-open
                    // the 1x1-placeholder case the registry rule exists for.
                    const i32 clipped_w =
                        width > 0 ? std::min(host_w, width) : host_w;
                    const i32 clipped_h =
                        height > 0 ? std::min(host_h, height) : host_h;
                    (void)ctx.cocoa_bridge.grow_hosted_peer_child_to_present_size(
                        static_cast<windowing::HWND>(hwnd), clipped_w,
                        clipped_h);
                }
            }
            const bool presented = ctx.d3d11_mgr.present_shared_window_surface(
                static_cast<windowing::HWND>(hwnd), shared_handle,
                static_cast<u32>(width), static_cast<u32>(height),
                sync_interval, src_x, src_y);
            static const bool log_present_view =
                std::getenv("VITREO_LOG_PRESENT_VIEW") != nullptr;
            if (log_present_view) {
                std::fprintf(stderr,
                             "[present-recv] t=%.3f hwnd=0x%x %dx%d src=(%u,%u) "
                             "presented=%d\n",
                             windowing::present_view_timestamp(),
                             static_cast<unsigned>(hwnd), width, height, src_x,
                             src_y, presented ? 1 : 0);
            }
            if (presented) {
                // Only the TRANSITION to visible needs the z-order work below.
                // This handler runs once per hosted child per frame, and each
                // of these calls is a synchronous main-thread round trip: with
                // ~37 children that was over a hundred serialized hops per UI
                // frame to re-assert state that was already correct after the
                // first present.
                // IDXGISwapChain::Present draws into a window's client area
                // and says NOTHING about whether that window is shown
                // (Microsoft Learn); visibility is WS_VISIBLE / ShowWindow.
                // The guest creates popup HWNDs up front and presents into them
                // while they are still hidden, so inferring "presented =>
                // visible" put an uninitialised 512x512 compositor plate on
                // screen as a white box. Honour the owner's published state
                // instead; when no publisher has said otherwise the registry
                // reports visible, so this cannot hide a window that worked
                // before.
                const bool became_visible =
                    ctx.cocoa_bridge.set_presentation_view_hidden(
                        static_cast<windowing::HWND>(hwnd), false);
                ctx.cocoa_bridge.debug_describe_presentation_view(
                    static_cast<windowing::HWND>(hwnd),
                    "after-present");
                if (!became_visible) return presented;
                // Peer hosts start below local siblings so an empty DComp
                // plate cannot bury legacy widget windows or GDI. Once a real
                // surface presents, raise the child so GPU pixels are not
                // covered by an opaque white sibling. Also raise the peer's
                // parent control when present: the legacy widget is often a
                // sibling of the intermediate HWND, not of the DComp child
                // itself.
                (void)ctx.cocoa_bridge.set_control_z_order(
                    static_cast<windowing::HWND>(hwnd),
                    windowing::HWND_TOP);
                if (remote && remote->parent_hwnd != 0 &&
                    ctx.cocoa_bridge.has_control_view(
                        static_cast<windowing::HWND>(remote->parent_hwnd))) {
                    (void)ctx.cocoa_bridge.set_control_z_order(
                        static_cast<windowing::HWND>(remote->parent_hwnd),
                        windowing::HWND_TOP);
                }
            }
            return presented;
        });
    remote_memory_server.set_window_refresh_handler(
        [&ctx](u32 hwnd) -> bool {
            return ctx.d3d11_mgr.refresh_window_presentation(
                static_cast<GuestAddr>(hwnd));
        });
    remote_memory_server.set_window_set_window_pos_handler(
        [&ctx](u32 hwnd, u32 insert_after, i32 x, i32 y, i32 cx, i32 cy,
               u32 flags) -> bool {
            if (ctx.hwnd_mgr.find(static_cast<windowing::HWND>(hwnd)) !=
                nullptr) {
                return ctx.u32_ext_mgr.apply_set_window_pos(
                    static_cast<windowing::HWND>(hwnd),
                    static_cast<windowing::HWND>(insert_after), x, y, cx, cy,
                    flags, /*send_messages=*/false);
            }
            // Peer-owned child hosted here after cross-process SetParent.
            const auto remote = ipc::hwnd_registry_lookup(hwnd);
            if (!remote || remote->parent_hwnd == 0) return false;
            const windowing::Rect frame{x, y, x + std::max(cx, 1),
                                        y + std::max(cy, 1)};
            if (!ctx.cocoa_bridge.ensure_hosted_peer_child(
                    static_cast<windowing::HWND>(remote->parent_hwnd),
                    static_cast<windowing::HWND>(hwnd), frame)) {
                return false;
            }
            if ((flags & 0x0004u) == 0) { // !SWP_NOZORDER
                (void)ctx.cocoa_bridge.set_control_z_order(
                    static_cast<windowing::HWND>(hwnd),
                    static_cast<windowing::HWND>(insert_after));
            }
            return true;
        });
    // ShowWindow from a peer process (Learn: any HWND of the desktop). Like
    // ShowWindow on another thread's window, it runs on the owner thread,
    // which receives WM_SHOWWINDOW and the activation messages; the caller
    // waits for the result.
    remote_memory_server.set_window_show_handler(
        [&ctx](u32 hwnd, i32 cmd) -> std::optional<u64> {
            const auto target = static_cast<windowing::HWND>(hwnd);
            const auto owner = ctx.hwnd_mgr.owner_thread_id(target);
            if (!owner || ctx.hwnd_mgr.find(target) == nullptr) return std::nullopt;
            const u32 sender = winapi::ThreadManager::get_current_thread_id_static();
            return ctx.msg_queue.send_to_thread(sender, *owner, [&ctx, target, cmd]() -> u64 {
                if (ctx.hwnd_mgr.find(target) == nullptr) return 0;
                return ctx.u32_ext_mgr.show_window_cmd(target, cmd);
            });
        });
    remote_memory_server.set_window_set_parent_handler(
        [&ctx](u32 child, u32 parent) -> bool {
            windowing::HWND previous = windowing::NULL_HWND;
            if (ctx.hwnd_mgr.is_valid(static_cast<windowing::HWND>(child))) {
                return ctx.u32_ext_mgr.apply_set_parent(
                    static_cast<windowing::HWND>(child),
                    static_cast<windowing::HWND>(parent), previous);
            }
            // Parent-process half of a GPU→browser SetParent: create the
            // local host NSView for the peer child HWND.
            const auto remote = ipc::hwnd_registry_lookup(child);
            const i32 width =
                remote && remote->client_w > 0 ? remote->client_w : 1;
            const i32 height =
                remote && remote->client_h > 0 ? remote->client_h : 1;
            return ctx.cocoa_bridge.ensure_hosted_peer_child(
                static_cast<windowing::HWND>(parent),
                static_cast<windowing::HWND>(child),
                windowing::Rect{0, 0, width, height});
        });
    // SendMessage(Timeout) from a peer process to a window this process owns.
    remote_memory_server.set_window_send_message_handler(
        [&ctx, &memory, is_64bit](u32 hwnd, u32 message, u64 wparam, u64 lparam,
                                  u32 flags, u32 timeout_ms,
                                  std::span<const Byte> payload,
                                  u32 out_capacity, u64& lresult,
                                  std::vector<Byte>& out) {
            return winapi::xproc_send::deliver(
                ctx.msgsend_mgr, memory, ctx.heap_mgr, is_64bit, hwnd, message,
                wparam, lparam, flags, timeout_ms, payload, out_capacity,
                lresult, out);
        });
    // PostMessage from a peer process to a window this process owns.
    remote_memory_server.set_window_post_message_handler(
        [&ctx](u32 hwnd, u32 message, u64 wparam, u64 lparam) -> bool {
            const auto target = static_cast<windowing::HWND>(hwnd);
            if (!ctx.hwnd_mgr.is_valid(target)) return false;
            return ctx.msgsend_mgr.post_message(target, message, wparam, lparam);
        });
    remote_memory_server.set_thread_message_handler(
        [&ctx](u32 message, u64 wparam, u64 lparam) -> bool {
            windowing::MSG queued{
                windowing::NULL_HWND, message, wparam, lparam, 0, {}};
            // The advertised primary TID and the receiving queue share one identity.
            queued.target_thread_id = ctx.thread_mgr.main_thread_id();
            ctx.msg_queue.post(queued);
            return true;
        });
    remote_memory_server.set_guest_call_handler(
        [&memory, &ctx, is_64bit, peb_address, pe_tls_data_start,
         pe_tls_data_size, &com_rpc_thread_context,
         &com_rpc_jit_initialized](
            GuestAddr function,
            std::span<const u64> arguments,
            bool caller_elevated) -> std::optional<u64> {
            constexpr u32 COM_RPC_SERVICE_TID = 0xF0000003u;
            winapi::ThreadManager::set_current_thread_id(COM_RPC_SERVICE_TID);
            if (!com_rpc_jit_initialized) {
                // It runs guest code: a thread of this process to client-id
                // APIs such as NtAlertThreadByThreadId.
                ctx.thread_mgr.add_service_thread(COM_RPC_SERVICE_TID);
                init_thread_jit_state(memory);
                com_rpc_jit_initialized = true;
            }
            if (!com_rpc_thread_context.has_value()) {
                GuestThreadContextParams params{};
                params.is_64bit = is_64bit;
                params.peb_address = peb_address;
                params.pe_tls_data_start = pe_tls_data_start;
                params.pe_tls_data_size = pe_tls_data_size;
                auto created = create_guest_thread_context(
                    memory, ctx, params, COM_RPC_SERVICE_TID);
                if (!created.valid()) return std::nullopt;
                com_rpc_thread_context = created;
            }

            cpu::CpuState call_cpu = make_guest_cpu_state(
                memory, *com_rpc_thread_context, is_64bit);
            const auto previous_call_context =
                ctx.com_mgr.push_server_call_context(caller_elevated);
            bool completed_normally = false;
            const u64 result = run_guest_callback(
                call_cpu, memory, ctx.thunk_registry, ctx.process_mgr,
                function, arguments.data(), arguments.size(), is_64bit,
                &completed_normally);
            ctx.com_mgr.pop_server_call_context(previous_call_context);
            if (!completed_normally) return std::nullopt;
            return result;
        });
    remote_memory_server.set_com_interface_marshal_handler(
        [&memory, &ctx, is_64bit](
            u32 owner_pid, GuestAddr object, u32 owner_pointer_size,
            std::span<const Byte> iid_bytes) -> GuestAddr {
            if (iid_bytes.size() != sizeof(winapi::Guid)) return 0;
            winapi::Guid iid{};
            std::memcpy(&iid, iid_bytes.data(), sizeof(iid));
            cpu::CpuState marshal_cpu;
            if (is_64bit) {
                marshal_cpu.init_long_mode();
            } else {
                marshal_cpu.init_protected_mode();
            }
            winapi::Marshaler marshaler(memory);
            marshaler.attach_cpu(marshal_cpu);
            return ctx.com_mgr.create_marshaled_interface_proxy(
                marshaler, owner_pid, owner_pointer_size, object, iid);
        });
}

} // namespace vitreo::cli
