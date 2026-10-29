// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// CLI-002: guest-call boundaries for multimedia timers, waveOut and FLS cleanup

#include "run_internal.hpp"
#include "runtime/process/runtime_context.hpp"
#include "runtime/execution/guest_run_loop.hpp"
#include "core/cpu/state/cpu_state.hpp"
#include "core/memory/guest_memory.hpp"
#include "ipc/hwnd_registry.hpp"
#include "winapi/graphics/common/gpu_event_signaler.hpp"
#include "audio/contracts/media_player.hpp"
#include "winapi/media/mfplay/mfplay.hpp"
#include <vitreo/diagnostics.hpp>

#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <optional>

namespace vitreo::cli {

namespace {

/// mfplay.dll (MfPlayManager) host hooks: guest calls on the calling thread
/// and on a Vitreo service thread (MFP_OPTION_FREE_THREADED_CALLBACK), the
/// player's hidden event window through user32's own exports, message
/// posting, Windows path resolution, the host media backend and video
/// presentation into the hWnd.
void install_mfplay_hooks(cpu::CpuState& cpu, GuestMemory& memory,
                          RuntimeContext& ctx, bool is_64bit, u64 peb_address,
                          u64 pe_tls_data_start, u32 pe_tls_data_size) {
    ctx.mfplay_mgr.set_memory(&memory);
    winapi::MfPlayManager::Host host;
    const auto call_guest = [&cpu, &memory, &ctx, is_64bit](
                                GuestAddr function, const u64* args,
                                usize count) -> u64 {
        return run_guest_callback(active_guest_cpu_or(cpu), memory,
                                  ctx.thunk_registry, ctx.process_mgr,
                                  function, args, count, is_64bit);
    };
    host.call_guest = call_guest;
    host.call_guest_service =
        [&memory, &ctx, is_64bit, peb_address, pe_tls_data_start,
         pe_tls_data_size](GuestAddr function, const u64* args,
                           usize count) -> u64 {
            // The free-threaded callback thread is a real thread of its own
            // to the guest, with a persistent stack and TEB (as waveOut's).
            constexpr u32 MFPLAY_SERVICE_TID = 0xF0000003u;
            static thread_local bool initialised = false;
            static thread_local GuestThreadContext gtc{};
            if (!initialised) {
                initialised = true;
                winapi::ThreadManager::set_current_thread_id(MFPLAY_SERVICE_TID);
                ctx.thread_mgr.add_service_thread(MFPLAY_SERVICE_TID);
                init_thread_jit_state(memory);
                GuestThreadContextParams params{};
                params.is_64bit = is_64bit;
                params.peb_address = peb_address;
                params.pe_tls_data_start = pe_tls_data_start;
                params.pe_tls_data_size = pe_tls_data_size;
                gtc = create_guest_thread_context(memory, ctx, params,
                                                  MFPLAY_SERVICE_TID);
            }
            if (!gtc.valid()) return 0;
            cpu::CpuState service_cpu = make_guest_cpu_state(memory, gtc, is_64bit);
            return run_guest_callback(service_cpu, memory, ctx.thunk_registry,
                                      ctx.process_mgr, function, args, count,
                                      is_64bit);
        };
    const auto user32 = [&ctx](const char* name) -> GuestAddr {
        auto address = ctx.thunk_registry.get_thunk_address("user32.dll", name);
        return address.is_ok() ? address.value() : GuestAddr{0};
    };
    host.create_event_window = [call_guest, user32, &ctx, &memory, is_64bit](
                                   GuestAddr wndproc) -> u64 {
        static std::mutex mutex;
        static GuestAddr scratch = 0;
        std::lock_guard<std::mutex> lock(mutex);
        constexpr u32 kCommitReserve = 0x3000;  // MEM_COMMIT | MEM_RESERVE
        constexpr u32 kReadWrite = 0x04;        // PAGE_READWRITE
        if (scratch == 0) {
            scratch = static_cast<GuestAddr>(
                ctx.vmm.virtual_alloc(0, 0x1000, kCommitReserve, kReadWrite));
            if (scratch == 0) return 0;
            // RegisterClassExW(WNDCLASSEXW) once per process: cbSize, style,
            // lpfnWndProc, cbClsExtra, cbWndExtra, hInstance, hIcon, hCursor,
            // hbrBackground, lpszMenuName, lpszClassName, hIconSm.
            const char16_t name[] = u"VitreoMFPlayEventWindow";
            const GuestAddr class_name = scratch + 0x100;
            for (usize i = 0; i < sizeof(name) / sizeof(name[0]); ++i) {
                (void)memory.write_u16(class_name + i * 2, static_cast<u16>(name[i]));
            }
            const u32 ptr = is_64bit ? 8u : 4u;
            const u32 size = is_64bit ? 80u : 48u;
            const GuestAddr wc = scratch;
            const auto put_ptr = [&](GuestAddr at, u64 v) {
                if (is_64bit) (void)memory.write_u64(at, v);
                else (void)memory.write_u32(at, static_cast<u32>(v));
            };
            (void)memory.write_u32(wc, size);
            (void)memory.write_u32(wc + 4, 0);
            put_ptr(wc + 8, wndproc);
            const GuestAddr after_proc = wc + 8 + ptr;
            (void)memory.write_u32(after_proc, 0);
            (void)memory.write_u32(after_proc + 4, 0);
            for (u32 i = 0; i < 6; ++i) put_ptr(after_proc + 8 + i * ptr, 0);
            put_ptr(after_proc + 8 + 5 * ptr, class_name);  // lpszClassName
            put_ptr(after_proc + 8 + 6 * ptr, 0);           // hIconSm
            const u64 args[1] = {wc};
            if (call_guest(user32("RegisterClassExW"), args, 1) == 0) {
                scratch = 0;
                return 0;
            }
        }
        // A message-only window (parent HWND_MESSAGE) owned by this thread.
        const u64 hwnd_message = is_64bit ? ~u64{2} : u64{0xFFFFFFFDu};
        const u64 args[12] = {0, scratch + 0x100, 0, 0, 0, 0, 0, 0,
                              hwnd_message, 0, 0, 0};
        return call_guest(user32("CreateWindowExW"), args, 12);
    };
    host.destroy_window = [call_guest, user32](u64 hwnd) {
        const u64 args[1] = {hwnd};
        (void)call_guest(user32("DestroyWindow"), args, 1);
    };
    host.def_window_proc = [call_guest, user32](u64 hwnd, u32 msg, u64 wparam,
                                                u64 lparam) -> u64 {
        const u64 args[4] = {hwnd, msg, wparam, lparam};
        return call_guest(user32("DefWindowProcW"), args, 4);
    };
    host.post_message = [&ctx](u64 hwnd, u32 msg, u64 wparam, u64 lparam) {
        return ctx.msgsend_mgr.post_message(static_cast<windowing::HWND>(hwnd),
                                            msg, wparam, lparam);
    };
    host.current_thread_id = [] {
        return winapi::ThreadManager::get_current_thread_id_static();
    };
    host.resolve_path = [&ctx](const std::string& path) -> std::optional<std::string> {
        auto resolved = ctx.vfs.resolve_host_path(path);
        if (resolved.is_err()) return std::nullopt;
        return resolved.value();
    };
    host.create_media = [] { return audio::create_media_player(); };
    host.present_video = [&ctx](u64 hwnd, const u8* rgba, i32 width, i32 height) {
        ctx.cocoa_bridge.present_video_overlay(static_cast<windowing::HWND>(hwnd),
                                               rgba, width, height);
    };
    host.hide_video = [&ctx](u64 hwnd) {
        ctx.cocoa_bridge.hide_video_overlay(static_cast<windowing::HWND>(hwnd));
    };
    host.client_size = [&ctx](u64 hwnd, i32& width, i32& height) {
        const auto rect = ctx.hwnd_mgr.client_rect(static_cast<windowing::HWND>(hwnd));
        if (!rect) return false;
        width = rect->width();
        height = rect->height();
        return true;
    };
    host.alloc_guest = [&ctx](u64 size) -> GuestAddr {
        return static_cast<GuestAddr>(ctx.vmm.virtual_alloc(0, size, 0x3000, 0x04));
    };
    host.task_alloc = [&ctx](u64 size) -> GuestAddr {
        return ctx.com_mgr.co_task_mem_alloc_guest(size);
    };
    ctx.mfplay_mgr.set_host(std::move(host));
}

} // namespace

void install_media_callbacks(
    cpu::CpuState& cpu, GuestMemory& memory, RuntimeContext& ctx,
    bool is_64bit, u64 peb_address, u64 pe_tls_data_start,
    u32 pe_tls_data_size) {
    // Wire up multimedia timer callbacks. The scheduler owns *when* a timer
    // fires; this owns *how* guest code runs, because a TIMEPROC needs a guest
    // stack and TEB that only this layer can build.
    //
    // The context is built once, on first fire, and reused for every later
    // callback. Building one per callback would leak a stack and a TEB per
    // tick - guest allocations are owned by the address space and are not
    // reclaimed individually - which at a video decoder's tick rate exhausts
    // the guest address space in minutes.
    ctx.winmm_mgr.timers().set_proc_invoker(
        [&memory, &ctx, is_64bit, peb_address, pe_tls_data_start,
         pe_tls_data_size](GuestAddr proc, u32 timer_id, u64 user_data) {
            static const bool timer_diag =
                std::getenv("VITREO_MM_TIMER_DIAG") != nullptr;
            if (timer_diag) {
                std::fprintf(stderr,
                             "[mm-timer-diag] callback-enter id=%u "
                             "proc=0x%08llx user=0x%llx\n",
                             timer_id,
                             static_cast<unsigned long long>(proc),
                             static_cast<unsigned long long>(user_data));
                std::fflush(stderr);
            }
            // Vitreo-owned service thread: it is a real distinct thread from
            // the guest's point of view, so it gets its own id rather than
            // impersonating the main thread (tid 1), which would make the
            // message-loop code treat it as the UI thread.
            constexpr u32 TIMER_SERVICE_TID = 0xF0000001u;

            static thread_local bool initialised = false;
            static thread_local GuestThreadContext gtc{};
            if (!initialised) {
                initialised = true;
                winapi::ThreadManager::set_current_thread_id(TIMER_SERVICE_TID);
                ctx.thread_mgr.add_service_thread(TIMER_SERVICE_TID);
                init_thread_jit_state(memory);

                GuestThreadContextParams params{};
                params.is_64bit = is_64bit;
                params.peb_address = peb_address;
                params.pe_tls_data_start = pe_tls_data_start;
                params.pe_tls_data_size = pe_tls_data_size;
                gtc = create_guest_thread_context(memory, ctx, params,
                                                  TIMER_SERVICE_TID);
                if (!gtc.valid()) {
                    VITREO_TRACE("[winmm] timer thread context allocation failed; "
                                 "TIMEPROC dispatch disabled\n");
                }
            }
            if (!gtc.valid()) return;

            // Rebuild the CpuState per callback so the stack pointer resets:
            // a TIMEPROC that leaves the stack unbalanced must not bleed into
            // the next tick.
            cpu::CpuState timer_cpu = make_guest_cpu_state(memory, gtc, is_64bit);

            // void CALLBACK TimeProc(UINT uTimerID, UINT uMsg, DWORD_PTR dwUser,
            //                        DWORD_PTR dw1, DWORD_PTR dw2)
            const u64 args[5] = {timer_id, 0, user_data, 0, 0};
            const u64 result = run_guest_callback(
                timer_cpu, memory, ctx.thunk_registry, ctx.process_mgr, proc,
                args, 5, is_64bit, nullptr,
                GuestCallbackKind::TimerProcedure);
            if (timer_diag) {
                std::fprintf(stderr,
                             "[mm-timer-diag] callback-exit id=%u result=0x%llx\n",
                             timer_id,
                             static_cast<unsigned long long>(result));
                std::fflush(stderr);
            }
        }
    );

    // GPU completion events (ID3D11Fence/ID3D12Fence::SetEventOnCompletion,
    // ID3D11DeviceContext3::Flush1): the graphics frontends ask, the kernel
    // object manager sets the guest event.
    winapi::GpuEventSignaler::instance().set_signal_function(
        [&ctx](u64 handle) {
            (void)ctx.sync_mgr.set_event(static_cast<winapi::Handle>(handle));
        });

    // Event-mode timers carry a HANDLE in place of a function pointer and need
    // no guest context at all.
    ctx.winmm_mgr.timers().set_event_signaler(
        [&ctx](u32 handle, bool pulse) {
            // TIME_CALLBACK_EVENT_PULSE calls PulseEvent (Learn
            // timeSetEvent; winmm_timer_probe pulse_manual_after).
            if (pulse) {
                (void)ctx.sync_mgr.pulse_event(static_cast<winapi::Handle>(handle));
            } else {
                (void)ctx.sync_mgr.set_event(static_cast<winapi::Handle>(handle));
            }
        }
    );

    // waveOut callbacks are completed by a WinmmManager-owned service thread.
    // Give that thread a persistent guest stack/TEB, just like multimedia
    // timers, and never execute guest code on CoreAudio's real-time thread.
    // HWAVEOUT and the DWORD_PTR arguments keep all 64 bits on x64.
    ctx.winmm_mgr.set_wave_callback_invoker(
        [&memory, &ctx, is_64bit, peb_address, pe_tls_data_start,
         pe_tls_data_size](GuestAddr proc, u64 wave_handle, u32 message,
                           u64 instance, u64 param1, u64 param2) {
            constexpr u32 WAVE_SERVICE_TID = 0xF0000002u;

            static thread_local bool initialised = false;
            static thread_local GuestThreadContext gtc{};
            if (!initialised) {
                initialised = true;
                winapi::ThreadManager::set_current_thread_id(WAVE_SERVICE_TID);
                ctx.thread_mgr.add_service_thread(WAVE_SERVICE_TID);
                init_thread_jit_state(memory);

                GuestThreadContextParams params{};
                params.is_64bit = is_64bit;
                params.peb_address = peb_address;
                params.pe_tls_data_start = pe_tls_data_start;
                params.pe_tls_data_size = pe_tls_data_size;
                gtc = create_guest_thread_context(memory, ctx, params,
                                                  WAVE_SERVICE_TID);
                if (!gtc.valid()) {
                    VITREO_TRACE(
                        "[winmm] wave callback thread context allocation "
                        "failed; waveOutProc dispatch disabled\n");
                }
            }
            if (!gtc.valid()) return;

            cpu::CpuState wave_cpu =
                make_guest_cpu_state(memory, gtc, is_64bit);
            // void CALLBACK waveOutProc(HWAVEOUT, UINT, DWORD_PTR,
            //                           DWORD_PTR, DWORD_PTR)
            const u64 args[5] = {
                wave_handle, message, instance, param1, param2
            };
            (void)run_guest_callback(wave_cpu, memory, ctx.thunk_registry,
                                     ctx.process_mgr, proc, args, 5, is_64bit);
        }
    );
    // WOM_OPEN / WOM_CLOSE run on the guest thread inside waveOutOpen /
    // waveOutClose, before they return (s7_winmm_probe fn_open_cbs).
    ctx.winmm_mgr.set_wave_sync_callback_invoker(
        [&cpu, &memory, &ctx, is_64bit](GuestAddr proc, u64 wave_handle,
                                        u32 message, u64 instance, u64 param1,
                                        u64 param2) {
            const u64 args[5] = {
                wave_handle, message, instance, param1, param2
            };
            (void)run_guest_callback(active_guest_cpu_or(cpu), memory,
                                     ctx.thunk_registry, ctx.process_mgr,
                                     proc, args, 5, is_64bit);
        }
    );
    ctx.winmm_mgr.set_wave_window_notifier(
        [&ctx](u64 hwnd, u32 message, u64 wave_handle, u64 param1) {
            return ctx.msgsend_mgr.post_message(
                static_cast<windowing::HWND>(hwnd), message,
                wave_handle, param1);
        }
    );
    ctx.winmm_mgr.set_wave_thread_notifier(
        [&ctx](u32 thread_id, u32 message, u64 wave_handle, u64 param1) {
            return ctx.msgsend_mgr.post_thread_message(
                thread_id, message, wave_handle, param1);
        }
    );
    ctx.winmm_mgr.set_wave_event_signaler([&ctx](u64 handle) {
        return ctx.sync_mgr.set_event(static_cast<winapi::Handle>(handle));
    });
    ctx.winmm_mgr.set_window_validator([&ctx](u64 hwnd) {
        return ctx.hwnd_mgr.is_valid(static_cast<windowing::HWND>(hwnd)) ||
               ipc::hwnd_registry_is_window(static_cast<u32>(hwnd));
    });
    // Each concurrently open HWAVEOUT (and the PlaySound voice) gets its own
    // host stream; RuntimeContext's wave_output backs the first.
    ctx.winmm_mgr.set_wave_output_factory(
        [] { return windowing::create_wave_output(); });

    install_mfplay_hooks(cpu, memory, ctx, is_64bit, peb_address,
                         pe_tls_data_start, pe_tls_data_size);

    ctx.fls_mgr.set_cleanup_callback(
        [&cpu, &memory, &ctx, is_64bit](GuestAddr callback, u64 value) {
            auto& active_cpu = active_guest_cpu_or(cpu);
            (void)run_guest_callback(active_cpu, memory, ctx.thunk_registry,
                ctx.process_mgr, callback, &value, 1, is_64bit);
        });
}

} // namespace vitreo::cli
