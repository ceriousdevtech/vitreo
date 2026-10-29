/// @file winmm.cpp
/// @brief winmm.dll API implementation (platform-agnostic)
///
/// waveOut/waveIn/midi/mixer/PlaySound follow Microsoft Learn plus the
/// measured Windows behaviour in tools/oracle_probe/s7_winmm_probe.expected
/// (x64) and .x86.expected. Lines of that probe are cited by their tag.
///
/// This file keeps the manager core: construction state, callback wiring,
/// output leases, teardown, multimedia timers and timeGetTime. The APIs are in
/// winmm_waveout.cpp, winmm_wavein.cpp, winmm_playsound.cpp, winmm_mmio.cpp and
/// winmm_midi_mixer.cpp; the notification machinery is in
/// winmm_wave_callbacks.cpp and the export table in winmm_thunks.cpp.

#include "winapi/media/winmm/winmm_internal.hpp"
#include "windowing/contracts/wave_output.hpp"
#include <vitreo/time_diag.hpp>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <mach/mach_time.h>
#include <mutex>

namespace vitreo::winapi {

namespace winmm_internal {

bool winmm_diag_enabled() {
    static const bool enabled = std::getenv("VITREO_WINMM_DIAG") != nullptr;
    return enabled;
}

void winmm_diag(const char* format, ...) {
    if (!winmm_diag_enabled()) return;
    static std::mutex diag_mutex;
    const auto timestamp_us = std::chrono::duration_cast<
        std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    std::lock_guard<std::mutex> guard(diag_mutex);
    std::fprintf(stderr, "[winmm-diag t=%lldus] ",
                 static_cast<long long>(timestamp_us));
    va_list args;
    va_start(args, format);
    std::vfprintf(stderr, format, args);
    va_end(args);
    std::fflush(stderr);
}

} // namespace winmm_internal

void WinmmManager::set_wave_callback_invoker(WaveCallbackInvoker invoker) {
    std::lock_guard<std::mutex> guard(m_wave_mutex);
    m_wave_invoker = std::move(invoker);
}
void WinmmManager::set_wave_sync_callback_invoker(WaveCallbackInvoker invoker) {
    std::lock_guard<std::mutex> guard(m_wave_mutex);
    m_wave_sync_invoker = std::move(invoker);
}
void WinmmManager::set_wave_window_notifier(WaveWindowNotifier notifier) {
    std::lock_guard<std::mutex> guard(m_wave_mutex);
    m_wave_window_notifier = std::move(notifier);
}
void WinmmManager::set_wave_thread_notifier(WaveThreadNotifier notifier) {
    std::lock_guard<std::mutex> guard(m_wave_mutex);
    m_wave_thread_notifier = std::move(notifier);
}
void WinmmManager::set_wave_event_signaler(WaveEventSignaler signaler) {
    std::lock_guard<std::mutex> guard(m_wave_mutex);
    m_wave_event_signaler = std::move(signaler);
}
void WinmmManager::set_window_validator(WindowValidator validator) {
    std::lock_guard<std::mutex> guard(m_wave_mutex);
    m_window_validator = std::move(validator);
}
void WinmmManager::set_wave_output_factory(WaveOutputFactory factory) {
    std::lock_guard<std::mutex> guard(m_wave_mutex);
    m_wave_output_factory = std::move(factory);
}

u32 WinmmManager::wave_device_count() const noexcept {
    return m_wave_output.device_count();
}

bool WinmmManager::acquire_output(OutputLease& lease) {
    std::lock_guard<std::mutex> guard(m_wave_mutex);
    if (!m_primary_output_in_use) {
        m_primary_output_in_use = true;
        lease.output = &m_wave_output;
        return true;
    }
    if (!m_wave_output_factory) return false;
    lease.owned = m_wave_output_factory();
    lease.output = lease.owned.get();
    return lease.output != nullptr;
}

void WinmmManager::release_output(OutputLease& lease) {
    if (lease.output == nullptr) return;
    lease.output->close();
    if (lease.owned) {
        lease.owned.reset();
    } else {
        std::lock_guard<std::mutex> guard(m_wave_mutex);
        m_primary_output_in_use = false;
    }
    lease.output = nullptr;
}

std::shared_ptr<WinmmManager::WaveStream> WinmmManager::find_stream(u64 handle) {
    std::lock_guard<std::mutex> guard(m_wave_mutex);
    const auto it = m_wave_streams.find(handle);
    return it == m_wave_streams.end() ? nullptr : it->second;
}

bool WinmmManager::audio_open() const noexcept {
    auto* self = const_cast<WinmmManager*>(this);
    std::lock_guard<std::mutex> guard(self->m_wave_mutex);
    return !m_wave_streams.empty();
}

WinmmManager::~WinmmManager() {
    stop_sound(false);
    close_all_wavein();
    std::vector<OutputLease> leases;
    {
        std::lock_guard<std::mutex> guard(m_wave_mutex);
        for (auto& [handle, stream] : m_wave_streams) {
            (void)handle;
            leases.push_back(std::move(stream->lease));
            stream->lease.output = nullptr;
        }
        m_wave_streams.clear();
    }
    for (auto& lease : leases) release_output(lease);
    stop_wave_callback_thread();
}

void WinmmManager::set_resource_info(GuestAddr image_base, u32 rsrc_rva, u32 rsrc_size) {
    m_image_base = image_base;
    m_rsrc_rva = rsrc_rva;
    m_rsrc_size = rsrc_size;
}

// ---------------------------------------------------------------------------
// Multimedia timers
// ---------------------------------------------------------------------------
// Timer IDs must be real: applications hold the ID returned by timeSetEvent and
// pass it to timeKillEvent during teardown. Issuing a fixed fake ID (or leaving
// timeKillEvent unregistered, so it resolves to the generic missing-export stub
// that does not clean up its stdcall argument) corrupts the guest stack and
// makes a later `ret` jump to garbage.
//
// Dispatch is delegated to MultimediaTimerScheduler, which owns the deadline
// queue and the single service thread. Timers previously had an exact ID
// lifecycle but never fired at all, which meant any caller pacing work off a
// multimedia timer - Smacker video decode being the motivating case - simply
// never advanced.

u32 WinmmManager::timer_create(u32 delay, u32 resolution, GuestAddr callback,
                               u64 user_data, u32 flags) {
    return m_timers.create(delay, resolution, callback, user_data, flags);
}

bool WinmmManager::timer_destroy(u32 timer_id) {
    return m_timers.destroy(timer_id);
}

u32 time_get_time() noexcept {
    static const mach_timebase_info_data_t timebase = []() {
        mach_timebase_info_data_t info{};
        mach_timebase_info(&info);
        return info;
    }();

    const auto nanoseconds =
        static_cast<unsigned __int128>(mach_continuous_time()) * timebase.numer /
        timebase.denom;
    const auto ms = static_cast<u64>(nanoseconds / 1'000'000U);
    // Observed before the DWORD truncation so the diagnostic sees the real
    // elapsed time and can attribute a wrap to the guest boundary.
    time_diag_observe(TimeApi::TimeGetTime, ms);
    return static_cast<u32>(ms);
}

} // namespace vitreo::winapi
