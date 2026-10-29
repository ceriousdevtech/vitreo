// SPDX-License-Identifier: LGPL-2.1-only
/// @file wasapi_audio_client.cpp
/// @brief IAudioClient (render and capture) with its IAudioRenderClient /
/// IAudioCaptureClient: Initialize, formats, buffers, Start/Stop/Reset. GetService lives in
/// wasapi_services.cpp. Split out of com_core.cpp.

#include "com_core.hpp"
#include "com_wasapi_internal.hpp"
#include "ole_guid_internal.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/com/ole32/task_memory.hpp"
#include "winapi/kernel32/synchronization/sync.hpp"

#include <vitreo/present_cpu.hpp>

#include <algorithm>
#include <atomic>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace vitreo::winapi {

namespace {

constexpr u32 E_NOINTERFACE = 0x80004002;
constexpr u32 E_OUTOFMEMORY = 0x8007000E;

/// Frames written to VITREO_WASAPI_DUMP so far (all streams), so a
/// `[wasapi-client]` line anchors a dump frame index to host time.
std::atomic<u64> g_dump_frames{0};

/// A log line formatted under a lock and printed on destruction.
struct DeferredLine {
    char text[512]{};
    DeferredLine() = default;
    DeferredLine(const DeferredLine&) = delete;
    DeferredLine& operator=(const DeferredLine&) = delete;
    ~DeferredLine() {
        if (text[0] != '\0') std::fputs(text, stderr);
    }
};

/// Account one IAudioRenderClient::ReleaseBuffer and close the per-second
/// `[wasapi-client]` window. A line is produced when a lock wait exceeded
/// 1 ms, a zero-frame release happened, or padding fell below 10 ms while
/// started -- or every tenth window, so a quiet stream shows its baseline.
template <class State>
void note_render_release(DeferredLine& line, GuestAddr client, State& state,
                         double now_ms, double wait_us, u32 frames, bool silent) {
    auto& d = state.diag;
    d.release_lock_max_us = std::max(d.release_lock_max_us, wait_us);
    ++d.releases;
    if (frames == 0) ++d.release_zero;
    if (silent && frames != 0) ++d.release_silent;
    d.frames += frames;
    if (d.window_start_ms == 0.0) {
        d.window_start_ms = now_ms;
        std::snprintf(line.text, sizeof(line.text),
            "[wasapi-client] c=0x%llx format rate=%u ch=%u bits=%u float=%d "
            "buffer_frames=%u event=%d (*_lock_max_us=COM mutex wait; "
            "pad_min=min padding while started; dump_frames=frames dumped so far)\n",
            static_cast<unsigned long long>(client), state.format.sample_rate,
            state.format.channels, state.format.bits_per_sample,
            state.format.float_samples ? 1 : 0, state.buffer_frames,
            state.event_driven ? 1 : 0);
        return;
    }
    if (now_ms - d.window_start_ms < 1000.0) return;
    const u32 low = state.format.sample_rate / 100u;
    const bool anomaly = d.padding_lock_max_us > 1000.0 ||
        d.get_buffer_lock_max_us > 1000.0 || d.release_lock_max_us > 1000.0 ||
        d.release_zero != 0 || (d.padding_min != UINT32_MAX && d.padding_min < low);
    if (anomaly || d.windows % 10 == 0) {
        std::snprintf(line.text, sizeof(line.text),
            "[wasapi-client] c=0x%llx t_ms=%.1f dt_ms=%.0f anom=%d pad_calls=%u "
            "pad_min=%u rel=%u rel_zero=%u rel_silent=%u frames=%llu "
            "pad_lock_max_us=%.0f getbuf_lock_max_us=%.0f rel_lock_max_us=%.0f "
            "dump_frames=%llu\n",
            static_cast<unsigned long long>(client), now_ms,
            now_ms - d.window_start_ms, anomaly ? 1 : 0, d.padding_calls,
            d.padding_min == UINT32_MAX ? 0u : d.padding_min, d.releases,
            d.release_zero, d.release_silent,
            static_cast<unsigned long long>(d.frames), d.padding_lock_max_us,
            d.get_buffer_lock_max_us, d.release_lock_max_us,
            static_cast<unsigned long long>(
                g_dump_frames.load(std::memory_order_relaxed)));
    }
    const u32 windows = d.windows + 1u;
    d = {};
    d.window_start_ms = now_ms;
    d.windows = windows;
}

}  // namespace

GuestAddr ComCoreManager::create_audio_client(Marshaler& m) {
    return create_audio_client(m, AudioEndpointRef{});
}

GuestAddr ComCoreManager::create_audio_client(Marshaler& m,
                                              const AudioEndpointRef& endpoint) {
    const std::string_view names[] = {
        "Vitreo_AudioClient_QueryInterface", "Vitreo_AudioClient_AddRef",
        "Vitreo_AudioClient_Release", "Vitreo_AudioClient_Initialize",
        "Vitreo_AudioClient_GetBufferSize", "Vitreo_AudioClient_GetStreamLatency",
        "Vitreo_AudioClient_GetCurrentPadding", "Vitreo_AudioClient_IsFormatSupported",
        "Vitreo_AudioClient_GetMixFormat", "Vitreo_AudioClient_GetDevicePeriod",
        "Vitreo_AudioClient_Start", "Vitreo_AudioClient_Stop",
        "Vitreo_AudioClient_Reset", "Vitreo_AudioClient_SetEventHandle",
        "Vitreo_AudioClient_GetService"};
    const GuestAddr object = allocate_builtin_object(m, names);
    if (!object) return 0;
    std::lock_guard lock(m_com_mutex);
    AudioClientState state{};
    state.capture = endpoint.capture;
    state.capture_uid = endpoint.uid;
    m_audio_clients.emplace(object, std::move(state));
    return object;
}

u32 ComCoreManager::audio_client_query_interface(Marshaler& m) {
    const GuestAddr object = m.arg_ptr(0), iid_ptr = m.arg_ptr(1), out_ptr = m.arg_ptr(2);
    if (iid_ptr == 0 || out_ptr == 0) return com::E_POINTER;
    (void)m.write_ptr(out_ptr, 0);
    const auto iid = m.read_struct<Guid>(iid_ptr);
    if (iid.is_err()) return com::E_INVALIDARG;
    note_unimplemented_audio_client_version(iid.value(), "IAudioClient::QueryInterface");
    std::lock_guard lock(m_com_mutex);
    auto found = m_audio_clients.find(object);
    if (found == m_audio_clients.end() ||
        (!guid_equal(iid.value(), IID_IUNKNOWN) &&
         !guid_equal(iid.value(), IID_IAUDIOCLIENT))) return E_NOINTERFACE;
    ++found->second.refs;
    return m.write_ptr(out_ptr, object).is_ok() ? com::S_OK : com::E_INVALIDARG;
}

u32 ComCoreManager::audio_client_add_ref(Marshaler& m) {
    std::lock_guard lock(m_com_mutex);
    auto found = m_audio_clients.find(m.arg_ptr(0));
    return found == m_audio_clients.end() ? 0u : ++found->second.refs;
}

u32 ComCoreManager::audio_client_release(Marshaler& m) {
    u32 refs = 0;
    {
        std::lock_guard lock(m_com_mutex);
        auto found = m_audio_clients.find(m.arg_ptr(0));
        if (found == m_audio_clients.end()) return 0;
        refs = --found->second.refs;
        if (refs == 0) destroy_audio_client_locked(found);
    }
    // A stream that goes away can end its session (releasing its sinks) or
    // make it inactive.
    if (refs == 0) finish_audio_change(m);
    return refs;
}

void ComCoreManager::destroy_audio_client_locked(
    std::unordered_map<GuestAddr, AudioClientState>::iterator client) {
    auto& state = client->second;
    // Synchronous: after remove() the clock never signals this handle again,
    // so it is safe to close it.
    if (m_audio_event_clock) m_audio_event_clock->remove(client->first);
    if (state.event_handle != 0 && m_sync != nullptr) {
        (void)m_sync->close_handle(static_cast<Handle>(state.event_handle));
    }
    if (state.session != 0) leave_audio_session_locked(state);
    state.voice.reset();
    // The data callback never takes m_com_mutex, so the stream can be
    // stopped (it waits for an in-flight callback) with the lock held.
    if (state.capture_stream) state.capture_stream->stop();
    state.capture_stream.reset();
    if (state.staging != 0 && m_guest_task_memory != nullptr)
        m_guest_task_memory->free(state.staging);
    m_audio_clients.erase(client);
}

namespace {
/// The next packet the capture engine hands a stream (measured: one packet
/// per 10 ms engine period): the stream frames of the period the ring's
/// front frame belongs to, once the ring holds all of them; a shorter run
/// only when nothing can extend it (a gap follows it, the endpoint buffer is
/// full, or the stream is stopped: "wasapi overrun" 480/480/96 packets).
/// Returns {frames, period}; frames 0 means no packet is ready.
std::pair<u32, u64> next_capture_packet(const audio::CaptureRing& ring,
                                        const audio::CapturePeriods& periods, bool started) {
    const audio::CaptureRing::Span span = ring.next(ring.capacity());
    if (span.frames == 0) return {0u, 0u};
    const u64 period = periods.period_of(span.position);
    const u64 rest = periods.end(period) - span.position;
    if (span.frames >= rest) return {static_cast<u32>(rest), period};
    if (ring.front_span_closed() || ring.available() == ring.capacity() || !started) {
        return {span.frames, period};
    }
    return {0u, period};
}

void log_unimplemented_capture_once(std::once_flag& once, const char* what) {
    std::call_once(once, [what] { std::fprintf(stderr, "[vitreo] UNIMPLEMENTED %s\n", what); });
}
}  // namespace

u32 ComCoreManager::capture_minimum_buffer(u32 rate) {
    return capture_minimum_buffer_frames(rate);
}

u32 ComCoreManager::initialize_capture_locked(GuestAddr object, AudioClientState& state,
                                              u32 flags, u64 duration,
                                              const audio::PcmFormat& format,
                                              const Guid& session_guid,
                                              u32& buffer_frames) {
    // The capture endpoint's device, its mix format, and the rules of a
    // shared-mode capture stream on the 48 kHz engine (measured on Windows):
    // the mix rate and channels unless AUTOCONVERTPCM; a zero-duration buffer
    // of 1056 frames (22 ms), or the converter's (capture_minimum_buffer_frames),
    // and never less; one packet per 10 ms engine period.
    const audio::CaptureDevice* device = nullptr;
    std::vector<audio::CaptureDevice> devices;
    if (m_audio_capture != nullptr) devices = m_audio_capture->devices();
    for (const auto& d : devices) {
        if (d.uid == state.capture_uid) device = &d;
    }
    if (device == nullptr) return AUDCLNT_E_DEVICE_INVALIDATED;
    const CaptureMixFormat mix = capture_mix_format(*device);
    const bool autoconvert = (flags & AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM) != 0;
    if (!autoconvert && (format.sample_rate != mix.rate || format.channels != mix.channels)) {
        return AUDCLNT_E_UNSUPPORTED_FORMAT;
    }
    // Measured: the converter takes client rates 10..384000 Hz.
    if (format.sample_rate < capture_contract::kAutoconvertMinimumRate ||
        format.sample_rate > capture_contract::kAutoconvertMaximumRate) {
        return AUDCLNT_E_UNSUPPORTED_FORMAT;
    }
    if (!audio::capture_channels_convertible(device->channels, format.channels)) {
        static std::once_flag once;
        log_unimplemented_capture_once(
            once, "IAudioClient::Initialize: AUTOCONVERTPCM channel conversion other than "
                  "mono<->stereo or none");
        return AUDCLNT_E_UNSUPPORTED_FORMAT;
    }
    const bool default_quality =
        (flags & capture_contract::AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY) != 0;
    const std::optional<audio::CapturePeriods> periods =
        capture_periods_for(format.sample_rate, default_quality);
    if (!periods) {
        // Windows' converter packetizes each rate by a delay that is only
        // known where measured (tools/oracle_probe "wasapi src"); another
        // rate's packets cannot be reproduced.
        static std::once_flag once;
        log_unimplemented_capture_once(
            once, "IAudioClient::Initialize: AUTOCONVERTPCM to a client rate whose "
                  "converter schedule is unmeasured");
        return AUDCLNT_E_UNSUPPORTED_FORMAT;
    }
    if (m_guest_task_memory == nullptr) return AUDCLNT_E_ENDPOINT_CREATE_FAILED;
    if (!audio::capture_permitted(*m_audio_capture, "IAudioClient::Initialize")) {
        // Unmeasured: the result Windows gives a desktop app whose
        // microphone access is off. E_ACCESSDENIED is what Learn documents
        // for capture without consent (ActivateAudioInterfaceAsync).
        return capture_contract::E_ACCESSDENIED;
    }
    const u32 rate = format.sample_rate;
    const u64 minimum = capture_minimum_buffer_frames(rate);
    const u64 requested = duration == 0 ? 0 : (duration * rate + 9999999u) / 10000000u;
    state.buffer_frames = static_cast<u32>(std::clamp<u64>(std::max(requested, minimum),
                                                           1u, rate * 2ull));
    state.capture_periods = *periods;
    const u32 bytes = state.buffer_frames * format.block_align();
    state.staging = m_guest_task_memory->allocate(bytes);
    if (state.staging == 0) return E_OUTOFMEMORY;
    auto signal = std::make_shared<CaptureSignal>();
    signal->periods = *periods;
    SyncManager* sync = m_sync;
    auto stream = std::shared_ptr<audio::IAudioCaptureStream>(m_audio_capture->open(
        state.capture_uid, format, state.buffer_frames, [signal, sync] {
            // Host audio thread: signal the stream's event once a whole
            // packet is waiting. Never m_com_mutex.
            std::lock_guard lock(signal->mutex);
            if (!signal->started || signal->event == 0 || signal->ring == nullptr ||
                sync == nullptr) {
                return;
            }
            if (next_capture_packet(*signal->ring, signal->periods, true).first != 0) {
                (void)sync->set_event(static_cast<Handle>(signal->event));
            }
        }));
    if (!stream) {
        m_guest_task_memory->free(state.staging);
        state.staging = 0;
        return AUDCLNT_E_ENDPOINT_CREATE_FAILED;
    }
    stream->ring().set_periods(*periods);
    {
        std::lock_guard lock(signal->mutex);
        signal->ring = &stream->ring();
    }
    state.capture_stream = std::move(stream);
    state.capture_signal = std::move(signal);
    state.format = format;
    state.event_driven = (flags & AUDCLNT_STREAMFLAGS_EVENTCALLBACK) != 0;
    state.stream_flags = flags;
    state.stream_volumes.assign(format.channels, 1.0f);
    state.initialized = true;
    buffer_frames = state.buffer_frames;
    join_audio_session_locked(state, session_guid);
    (void)object;
    return com::S_OK;
}

u32 ComCoreManager::audio_client_initialize(Marshaler& m) {
    // HRESULT Initialize(AUDCLNT_SHAREMODE ShareMode, DWORD StreamFlags,
    //     REFERENCE_TIME hnsBufferDuration, REFERENCE_TIME hnsPeriodicity,
    //     const WAVEFORMATEX* pFormat, LPCGUID AudioSessionGuid)
    // On x86 each REFERENCE_TIME occupies two stack slots.
    const bool x64 = m.cpu_is_64bit();
    const GuestAddr object = m.arg_ptr(0);
    const u32 share_mode = static_cast<u32>(m.arg(1));
    const u32 flags = static_cast<u32>(m.arg(2));
    const u64 duration = x64 ? m.arg(3) : ((m.arg(3) & 0xFFFFFFFFull) | (m.arg(4) << 32u));
    const u64 periodicity = x64 ? m.arg(4) : ((m.arg(5) & 0xFFFFFFFFull) | (m.arg(6) << 32u));
    const GuestAddr format_ptr = m.arg_ptr(x64 ? 5u : 7u);
    const GuestAddr session_ptr = m.arg_ptr(x64 ? 6u : 8u);

    StreamWaveFormat raw{};
    u32 buffer_frames = 0;
    const u32 hr = [&]() -> u32 {
        if (format_ptr == 0) return com::E_POINTER;
        // Learn (Initialize): a NULL AudioSessionGuid is GUID_NULL.
        Guid session_guid{};
        if (session_ptr != 0) {
            const auto read = m.read_struct<Guid>(session_ptr);
            if (read.is_err()) return com::E_POINTER;
            session_guid = read.value();
        }
        std::lock_guard lock(m_com_mutex);
        auto found = m_audio_clients.find(object);
        if (found == m_audio_clients.end()) return E_NOINTERFACE;
        auto& state = found->second;
        if (state.initialized) return AUDCLNT_E_ALREADY_INITIALIZED;
        if (share_mode != AUDCLNT_SHAREMODE_SHARED &&
            share_mode != AUDCLNT_SHAREMODE_EXCLUSIVE) return com::E_INVALIDARG;
        if ((flags & ~AUDCLNT_KNOWN_INITIALIZE_FLAGS) != 0) return com::E_INVALIDARG;
        // Learn: E_INVALIDARG when LOOPBACK is set outside shared mode, or
        // CROSSPROCESS is set in exclusive mode.
        if (share_mode == AUDCLNT_SHAREMODE_EXCLUSIVE &&
            (flags & (AUDCLNT_STREAMFLAGS_LOOPBACK |
                      AUDCLNT_STREAMFLAGS_CROSSPROCESS)) != 0) {
            return com::E_INVALIDARG;
        }
        // The host endpoint is shared by every macOS process; there is no
        // exclusive-mode path to it. This is the documented "user has
        // disabled exclusive-mode use" answer, which clients handle by
        // falling back to shared mode.
        if (share_mode == AUDCLNT_SHAREMODE_EXCLUSIVE) {
            return AUDCLNT_E_EXCLUSIVE_MODE_NOT_ALLOWED;
        }
        if ((flags & AUDCLNT_STREAMFLAGS_LOOPBACK) != 0) {
            // Learn and measured: loopback on a capture endpoint is
            // AUDCLNT_E_WRONG_ENDPOINT_TYPE.
            if (state.capture) return capture_contract::AUDCLNT_E_WRONG_ENDPOINT_TYPE;
            // Loopback of a render endpoint records the whole host output
            // mix, which no host API Vitreo uses provides.
            static std::once_flag once;
            std::call_once(once, [] {
                std::fprintf(stderr, "[vitreo] UNIMPLEMENTED IAudioClient::Initialize: "
                                     "AUDCLNT_STREAMFLAGS_LOOPBACK on the render endpoint\n");
            });
            return capture_contract::AUDCLNT_E_ENDPOINT_CREATE_FAILED;
        }
        audio::PcmFormat format{};
        const auto verdict = state.capture
            ? read_stream_format(m, format_ptr, raw, format, audio::kCaptureMaxChannels,
                                 capture_contract::kAutoconvertMinimumRate)
            : read_stream_format(m, format_ptr, raw, format);
        switch (verdict) {
        case StreamFormatVerdict::Invalid: return com::E_INVALIDARG;
        case StreamFormatVerdict::Unsupported: return AUDCLNT_E_UNSUPPORTED_FORMAT;
        case StreamFormatVerdict::Supported: break;
        }
        if (state.capture) {
            return initialize_capture_locked(object, state, flags, duration, format,
                                             session_guid, buffer_frames);
        }
        if ((flags & AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM) == 0 &&
            !matches_mix_rate_and_channels(format)) {
            return AUDCLNT_E_UNSUPPORTED_FORMAT;
        }
        if (m_audio_renderer == nullptr || m_guest_task_memory == nullptr) {
            return AUDCLNT_E_ENDPOINT_CREATE_FAILED;
        }
        // Shared mode: the buffer is at least the requested duration, rounded
        // up to whole frames, and never below the engine's minimum.
        const u32 rate = format.sample_rate;
        const u64 requested_frames = duration == 0 ? rate / 10u
            : (duration * rate + 9999999u) / 10000000u;
        state.buffer_frames = static_cast<u32>(std::clamp<u64>(
            requested_frames, std::max<u64>(rate / 100u, 1u), rate * 2ull));
        const u32 bytes = state.buffer_frames * format.block_align();
        state.staging = m_guest_task_memory->allocate(bytes);
        if (state.staging == 0) return E_OUTOFMEMORY;
        state.voice = m_audio_renderer->create_push_voice(
            format, state.buffer_frames, (flags & AUDCLNT_STREAMFLAGS_RATEADJUST) != 0);
        if (!state.voice) {
            m_guest_task_memory->free(state.staging);
            state.staging = 0;
            return AUDCLNT_E_ENDPOINT_CREATE_FAILED;
        }
        state.format = format;
        state.event_driven = (flags & AUDCLNT_STREAMFLAGS_EVENTCALLBACK) != 0;
        state.stream_flags = flags;
        state.stream_volumes.assign(format.channels, 1.0f);
        state.initialized = true;
        buffer_frames = state.buffer_frames;
        // The stream joins its session, whose volume applies to it at once.
        join_audio_session_locked(state, session_guid);
        apply_audio_gains_locked(object);
        return com::S_OK;
    }();

    if (hr == com::S_OK) hold_audio_engine_mta_usage();

    // Always on, but bounded: one line per stream open is the evidence a
    // silent guest needs, and a client that retries forever must not flood
    // the log.
    static std::atomic<u32> logged{0};
    if (logged.fetch_add(1, std::memory_order_relaxed) < 32u) {
        std::fprintf(stderr,
            "[wasapi] IAudioClient::Initialize mode=%u flags=0x%08x duration=%lld "
            "period=%lld tag=0x%04x ch=%u rate=%u bits=%u valid=%u align=%u "
            "mask=0x%x sub=%08x-%04x -> 0x%08x buffer=%u frames\n",
            share_mode, flags, static_cast<long long>(duration),
            static_cast<long long>(periodicity), raw.tag, raw.channels, raw.rate,
            raw.bits, raw.valid_bits, raw.block_align, raw.channel_mask,
            raw.subformat.Data1, raw.subformat.Data2, hr, buffer_frames);
    }
    return hr;
}

u32 ComCoreManager::audio_client_get_buffer_size(Marshaler& m) {
    const GuestAddr out = m.arg_ptr(1);
    if (out == 0) return com::E_POINTER;
    std::lock_guard lock(m_com_mutex);
    auto found = m_audio_clients.find(m.arg_ptr(0));
    if (found == m_audio_clients.end()) return E_NOINTERFACE;
    if (!found->second.initialized) return AUDCLNT_E_NOT_INITIALIZED;
    return m.write_u32(out, found->second.buffer_frames).is_ok()
        ? com::S_OK : com::E_INVALIDARG;
}

u32 ComCoreManager::audio_client_get_stream_latency(Marshaler& m) {
    const GuestAddr out = m.arg_ptr(1);
    if (out == 0) return com::E_POINTER;
    std::lock_guard lock(m_com_mutex);
    auto found = m_audio_clients.find(m.arg_ptr(0));
    if (found == m_audio_clients.end()) return E_NOINTERFACE;
    if (!found->second.initialized) return AUDCLNT_E_NOT_INITIALIZED;
    // Measured: a shared-mode capture stream reports 0. Render (Learn: the
    // maximum latency, constant for the stream's life): a frame waits at
    // most one engine period in the endpoint buffer for the pass that reads
    // it, then crosses the host pipeline and device (the voice's latency).
    const auto& state = found->second;
    const u64 latency = state.capture ? 0u
        : kEngineDefaultPeriodHns +
              (static_cast<u64>(state.voice->latency_frames()) * 10000000u +
               state.format.sample_rate - 1u) / state.format.sample_rate;
    return m.memory().write_u64(out, latency).is_ok() ? com::S_OK : com::E_INVALIDARG;
}

u32 ComCoreManager::audio_client_get_current_padding(Marshaler& m) {
    const GuestAddr out = m.arg_ptr(1);
    if (out == 0) return com::E_POINTER;
    const double wait_start = diagnostics::present_cpu::host_mono_ms();
    std::lock_guard lock(m_com_mutex);
    const double wait_us = (diagnostics::present_cpu::host_mono_ms() - wait_start) * 1000.0;
    auto found = m_audio_clients.find(m.arg_ptr(0));
    if (found == m_audio_clients.end()) return E_NOINTERFACE;
    auto& state = found->second;
    if (!state.initialized) return AUDCLNT_E_NOT_INITIALIZED;
    // Capture: every frame waiting in the endpoint buffer (measured: the
    // whole buffer once it is full, and at least the next packet).
    const u32 padding = state.capture ? state.capture_stream->ring().available()
                                      : state.voice->padding_frames();
    if (!state.capture) {
        auto& diag = state.diag;
        diag.padding_lock_max_us = std::max(diag.padding_lock_max_us, wait_us);
        ++diag.padding_calls;
        if (state.started) diag.padding_min = std::min(diag.padding_min, padding);
    }
    return m.write_u32(out, padding).is_ok() ? com::S_OK : com::E_INVALIDARG;
}

std::optional<audio::CaptureDevice> ComCoreManager::capture_device_of(GuestAddr client) {
    std::string uid;
    {
        std::lock_guard lock(m_com_mutex);
        auto found = m_audio_clients.find(client);
        if (found == m_audio_clients.end() || !found->second.capture) return std::nullopt;
        uid = found->second.capture_uid;
    }
    if (m_audio_capture == nullptr) return std::nullopt;
    for (const auto& device : m_audio_capture->devices()) {
        if (device.uid == uid) return device;
    }
    return std::nullopt;
}

u32 ComCoreManager::audio_client_is_format_supported(Marshaler& m) {
    // HRESULT IsFormatSupported(AUDCLNT_SHAREMODE ShareMode,
    //     const WAVEFORMATEX* pFormat, WAVEFORMATEX** ppClosestMatch)
    const u32 share_mode = static_cast<u32>(m.arg(1));
    const GuestAddr format = m.arg_ptr(2), closest = m.arg_ptr(3);
    if (share_mode != AUDCLNT_SHAREMODE_SHARED &&
        share_mode != AUDCLNT_SHAREMODE_EXCLUSIVE) return com::E_INVALIDARG;
    if (format == 0) return com::E_POINTER;
    if (share_mode == AUDCLNT_SHAREMODE_SHARED && closest == 0) return com::E_POINTER;
    // Every outcome but S_FALSE leaves *ppClosestMatch NULL.
    if (closest != 0 && m.write_ptr(closest, 0).is_err()) return com::E_POINTER;
    // No exclusive-mode path exists to the host endpoint (see Initialize).
    if (share_mode == AUDCLNT_SHAREMODE_EXCLUSIVE) return AUDCLNT_E_UNSUPPORTED_FORMAT;
    StreamWaveFormat raw{};
    audio::PcmFormat parsed{};
    bool capture = false;
    {
        std::lock_guard lock(m_com_mutex);
        auto found = m_audio_clients.find(m.arg_ptr(0));
        capture = found != m_audio_clients.end() && found->second.capture;
    }
    const auto verdict = capture
        ? read_stream_format(m, format, raw, parsed, audio::kCaptureMaxChannels)
        : read_stream_format(m, format, raw, parsed);
    const std::optional<audio::CaptureDevice> capture_device =
        capture ? capture_device_of(m.arg_ptr(0)) : std::nullopt;
    const std::optional<CaptureMixFormat> capture_mix =
        capture_device ? std::optional<CaptureMixFormat>(capture_mix_format(*capture_device))
                       : std::nullopt;
    if (capture && !capture_mix) return AUDCLNT_E_DEVICE_INVALIDATED;
    const bool same_rate_and_channels = capture
        ? parsed.sample_rate == capture_mix->rate && parsed.channels == capture_mix->channels
        : matches_mix_rate_and_channels(parsed);
    if (verdict == StreamFormatVerdict::Supported && same_rate_and_channels) {
        return com::S_OK;
    }
    // A linear PCM/float format the engine cannot take as-is: the similar
    // format it does support is the mix format. Anything else (compressed,
    // malformed) has no similar format.
    if (verdict == StreamFormatVerdict::Invalid ||
        (raw.tag != WAVE_FORMAT_PCM && raw.tag != WAVE_FORMAT_IEEE_FLOAT &&
         raw.tag != WAVE_FORMAT_EXTENSIBLE) ||
        (raw.tag == WAVE_FORMAT_EXTENSIBLE &&
         !guid_equal(raw.subformat, KSDATAFORMAT_SUBTYPE_PCM) &&
         !guid_equal(raw.subformat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT))) {
        return AUDCLNT_E_UNSUPPORTED_FORMAT;
    }
    if (m_guest_task_memory == nullptr) return AUDCLNT_E_UNSUPPORTED_FORMAT;
    const GuestAddr mix = m_guest_task_memory->allocate(MixWaveFormat::BYTES);
    if (mix == 0) return E_OUTOFMEMORY;
    const bool written = capture ? write_capture_mix_format(m, mix, *capture_mix)
                                 : write_mix_wave_format(m, mix);
    if (!written || m.write_ptr(closest, mix).is_err()) {
        m_guest_task_memory->free(mix);
        (void)m.write_ptr(closest, 0);
        return com::E_POINTER;
    }
    return com::S_FALSE;
}

u32 ComCoreManager::audio_client_get_mix_format(Marshaler& m) {
    const GuestAddr out_ptr = m.arg_ptr(1);
    if (out_ptr == 0) return com::E_POINTER;
    if (m.write_ptr(out_ptr, 0).is_err()) return com::E_POINTER;
    if (m_guest_task_memory == nullptr) return com::E_NOTIMPL;
    bool capture = false;
    {
        std::lock_guard lock(m_com_mutex);
        auto found = m_audio_clients.find(m.arg_ptr(0));
        capture = found != m_audio_clients.end() && found->second.capture;
    }
    const std::optional<audio::CaptureDevice> capture_device =
        capture ? capture_device_of(m.arg_ptr(0)) : std::nullopt;
    const std::optional<CaptureMixFormat> capture_mix =
        capture_device ? std::optional<CaptureMixFormat>(capture_mix_format(*capture_device))
                       : std::nullopt;
    if (capture && !capture_mix) return AUDCLNT_E_DEVICE_INVALIDATED;
    const GuestAddr mix = m_guest_task_memory->allocate(MixWaveFormat::BYTES);
    if (mix == 0) return E_OUTOFMEMORY;
    const bool written = capture ? write_capture_mix_format(m, mix, *capture_mix)
                                 : write_mix_wave_format(m, mix);
    if (!written || m.write_ptr(out_ptr, mix).is_err()) {
        m_guest_task_memory->free(mix);
        (void)m.write_ptr(out_ptr, 0);
        return com::E_POINTER;
    }
    return com::S_OK;
}

u32 ComCoreManager::audio_client_get_device_period(Marshaler& m) {
    const GuestAddr default_period = m.arg_ptr(1), minimum_period = m.arg_ptr(2);
    if (default_period == 0 && minimum_period == 0) return com::E_POINTER;
    if (default_period != 0 &&
        m.memory().write_u64(default_period, kEngineDefaultPeriodHns).is_err())
        return com::E_POINTER;
    if (minimum_period != 0 &&
        m.memory().write_u64(minimum_period, kEngineMinimumPeriodHns).is_err())
        return com::E_POINTER;
    return com::S_OK;
}

u32 ComCoreManager::audio_client_start(Marshaler& m) {
    const GuestAddr object = m.arg_ptr(0);
    {
        std::lock_guard lock(m_com_mutex);
        auto found = m_audio_clients.find(object);
        if (found == m_audio_clients.end()) return E_NOINTERFACE;
        auto& state = found->second;
        if (!state.initialized) return AUDCLNT_E_NOT_INITIALIZED;
        if (state.started) return AUDCLNT_E_NOT_STOPPED;
        if (state.event_driven && state.event_handle == 0) {
            return AUDCLNT_E_EVENTHANDLE_NOT_SET;
        }
        if (state.capture) {
            // The data callback takes only the signal's mutex, so starting
            // (which may wait for nothing) is safe under m_com_mutex.
            if (!state.capture_stream->start()) return AUDCLNT_E_DEVICE_INVALIDATED;
            std::lock_guard signal_lock(state.capture_signal->mutex);
            state.capture_signal->started = true;
            state.capture_signal->event = state.event_handle;
        } else if (!state.voice->start()) {
            return AUDCLNT_E_DEVICE_INVALIDATED;
        }
        state.started = true;
        state.ever_started = true;
        state.needs_reset = true;
        update_audio_session_state_locked(state.session, +1);
        if (!state.capture) {
            // The engine reads every running render stream once per period,
            // event-driven or polled; only event-driven streams are signalled.
            if (!m_audio_event_clock) {
                SyncManager* sync = m_sync;
                m_audio_event_clock = std::make_unique<AudioEventClock>(
                    std::chrono::nanoseconds(kEngineDefaultPeriodHns * 100u),
                    [sync](u64 handle) {
                        if (sync != nullptr) (void)sync->set_event(static_cast<Handle>(handle));
                    });
            }
            m_audio_event_clock->add(object, state.event_driven ? state.event_handle : 0u,
                                     state.voice.get());
        }
    }
    finish_audio_change(m);
    return com::S_OK;
}

u32 ComCoreManager::audio_client_stop(Marshaler& m) {
    const GuestAddr object = m.arg_ptr(0);
    {
        std::lock_guard lock(m_com_mutex);
        auto found = m_audio_clients.find(object);
        if (found == m_audio_clients.end()) return E_NOINTERFACE;
        auto& state = found->second;
        if (!state.initialized) return AUDCLNT_E_NOT_INITIALIZED;
        if (!state.started) return com::S_FALSE;
        if (m_audio_event_clock) m_audio_event_clock->remove(object);
        if (state.capture) {
            {
                std::lock_guard signal_lock(state.capture_signal->mutex);
                state.capture_signal->started = false;
            }
            // Measured: the captured frames stay readable after Stop.
            state.capture_stream->stop();
        } else {
            state.voice->pause();
        }
        state.started = false;
        // The position freezes, and so does the time it was read at
        // (measured: GetPosition's QPC value stops advancing at Stop).
        state.position_qpc = audio_qpc_now_hns();
        update_audio_session_state_locked(state.session, -1);
    }
    finish_audio_change(m);
    return com::S_OK;
}

u32 ComCoreManager::audio_client_reset(Marshaler& m) {
    std::lock_guard lock(m_com_mutex);
    auto found = m_audio_clients.find(m.arg_ptr(0));
    if (found == m_audio_clients.end()) return E_NOINTERFACE;
    auto& state = found->second;
    if (!state.initialized) return AUDCLNT_E_NOT_INITIALIZED;
    if (state.started) return AUDCLNT_E_NOT_STOPPED;
    if (state.buffer_pending || state.capture_held) return AUDCLNT_E_BUFFER_OPERATION_PENDING;
    if (state.capture) {
        // Measured: Reset of a stopped capture stream always succeeds (a
        // second Reset too), empties the buffer and zeroes the clock; the
        // device position runs on ("wasapi positions reset-keeps-position=1")
        // and the next Start resumes at the next engine period.
        state.capture_stream->ring().clear(false);
        state.capture_stream->ring().reset_stored_total();
        state.needs_reset = false;
        return com::S_OK;
    }
    if (!state.needs_reset) return com::S_FALSE;
    state.voice->reset();
    state.needs_reset = false;
    // Learn (IAudioClock::GetPosition): Reset sets the device position to 0.
    state.submitted_frames = 0;
    state.device_frames_at_checkpoint = 0.0;
    state.played_frames_at_checkpoint = 0;
    return com::S_OK;
}

u32 ComCoreManager::audio_client_set_event_handle(Marshaler& m) {
    const GuestAddr object = m.arg_ptr(0);
    const u64 event = m.arg(1);
    std::lock_guard lock(m_com_mutex);
    auto found = m_audio_clients.find(object);
    if (found == m_audio_clients.end()) return E_NOINTERFACE;
    auto& state = found->second;
    if (!state.initialized) return AUDCLNT_E_NOT_INITIALIZED;
    if (!state.event_driven) return AUDCLNT_E_EVENTHANDLE_NOT_EXPECTED;
    if (event == 0 || m_sync == nullptr) return com::E_INVALIDARG;
    // Hold our own reference, as the audio service does: the stream keeps
    // signalling even if the guest closes its handle first. Anything but an
    // event (or a stale value) fails to duplicate, which is the documented
    // "invalid handle" E_INVALIDARG.
    const Handle duplicate = m_sync->duplicate_handle(
        static_cast<Handle>(event), 0u, HandleAccess::All);
    if (duplicate == NULL_HANDLE) return com::E_INVALIDARG;
    const u64 previous = state.event_handle;
    state.event_handle = duplicate;
    if (state.capture) {
        std::lock_guard signal_lock(state.capture_signal->mutex);
        if (state.capture_signal->started) state.capture_signal->event = duplicate;
    } else if (state.started && m_audio_event_clock) {
        m_audio_event_clock->add(object, duplicate, state.voice.get());
    }
    if (previous != 0) {
        // A tick that copied the previous handle may still be signalling it.
        if (m_audio_event_clock) m_audio_event_clock->quiesce();
        (void)m_sync->close_handle(static_cast<Handle>(previous));
    }
    return com::S_OK;
}

// IAudioClient::GetService and its service objects: wasapi_services.cpp.

GuestAddr ComCoreManager::create_audio_render_client(Marshaler& m,
                                                      GuestAddr client) {
    // One IAudioRenderClient per stream while it lives: a second GetService
    // answers the same object (measured on the capture-client twin).
    auto share_live = [&]() -> GuestAddr {
        auto found = m_audio_clients.find(client);
        if (found == m_audio_clients.end()) return 0;
        auto live = m_audio_render_clients.find(found->second.render_client);
        if (found->second.render_client == 0 || live == m_audio_render_clients.end()) return 0;
        ++live->second.refs;
        return live->first;
    };
    {
        std::lock_guard lock(m_com_mutex);
        if (const GuestAddr live = share_live(); live != 0) return live;
    }
    const char* names[] = {"Vitreo_AudioRenderClient_QueryInterface",
        "Vitreo_AudioRenderClient_AddRef", "Vitreo_AudioRenderClient_Release",
        "Vitreo_AudioRenderClient_GetBuffer", "Vitreo_AudioRenderClient_ReleaseBuffer"};
    const GuestAddr pointer_size = m.cpu_is_64bit() ? 8u : 4u;
    const GuestAddr object = reserve_com_scratch((1 + std::size(names)) * pointer_size);
    if (object == 0) return 0;
    const GuestAddr vtable = object + pointer_size;
    if (m.write_ptr(object, vtable).is_err()) return 0;
    for (usize slot = 0; slot < std::size(names); ++slot) {
        auto method = m_thunks->get_thunk_address("ole32.dll", names[slot]);
        if (method.is_err() ||
            m.write_ptr(vtable + slot * pointer_size, method.value()).is_err()) return 0;
    }
    std::lock_guard lock(m_com_mutex);
    if (const GuestAddr live = share_live(); live != 0) return live;
    auto found = m_audio_clients.find(client);
    if (found == m_audio_clients.end()) return 0;
    ++found->second.refs;
    found->second.render_client = object;
    m_audio_render_clients.emplace(object, AudioRenderClientState{1u, client});
    return object;
}

GuestAddr ComCoreManager::create_audio_capture_client(Marshaler& m, GuestAddr client) {
    // One IAudioCaptureClient per stream while it lives (measured: a second
    // GetService answers the same object).
    auto share_live = [&]() -> GuestAddr {
        auto found = m_audio_clients.find(client);
        if (found == m_audio_clients.end()) return 0;
        auto live = m_audio_capture_clients.find(found->second.capture_client);
        if (found->second.capture_client == 0 || live == m_audio_capture_clients.end()) return 0;
        ++live->second.refs;
        return live->first;
    };
    {
        std::lock_guard lock(m_com_mutex);
        if (const GuestAddr live = share_live(); live != 0) return live;
    }
    static constexpr std::string_view names[] = {
        "Vitreo_AudioCaptureClient_QueryInterface", "Vitreo_AudioCaptureClient_AddRef",
        "Vitreo_AudioCaptureClient_Release", "Vitreo_AudioCaptureClient_GetBuffer",
        "Vitreo_AudioCaptureClient_ReleaseBuffer",
        "Vitreo_AudioCaptureClient_GetNextPacketSize"};
    const GuestAddr object = allocate_builtin_object(m, names);
    if (object == 0) return 0;
    std::lock_guard lock(m_com_mutex);
    if (const GuestAddr live = share_live(); live != 0) return live;
    auto found = m_audio_clients.find(client);
    if (found == m_audio_clients.end()) return 0;
    ++found->second.refs;
    found->second.capture_client = object;
    m_audio_capture_clients.emplace(object, AudioRenderClientState{1u, client});
    return object;
}

ComCoreManager::CapturePacket ComCoreManager::capture_packet(const AudioClientState& state) {
    if (!state.initialized || !state.capture_stream) return {};
    const auto [frames, period] = next_capture_packet(state.capture_stream->ring(),
                                                      state.capture_periods, state.started);
    return CapturePacket{frames, period};
}

u32 ComCoreManager::audio_capture_client_query_interface(Marshaler& m) {
    const GuestAddr object = m.arg_ptr(0), iid_ptr = m.arg_ptr(1), out = m.arg_ptr(2);
    if (iid_ptr == 0 || out == 0) return com::E_POINTER;
    (void)m.write_ptr(out, 0);
    const auto iid = m.read_struct<Guid>(iid_ptr);
    if (iid.is_err()) return com::E_INVALIDARG;
    std::lock_guard lock(m_com_mutex);
    auto found = m_audio_capture_clients.find(object);
    if (found == m_audio_capture_clients.end() ||
        (!guid_equal(iid.value(), IID_IUNKNOWN) &&
         !guid_equal(iid.value(), capture_contract::IID_IAUDIOCAPTURECLIENT))) return E_NOINTERFACE;
    ++found->second.refs;
    return m.write_ptr(out, object).is_ok() ? com::S_OK : com::E_INVALIDARG;
}

u32 ComCoreManager::audio_capture_client_add_ref(Marshaler& m) {
    std::lock_guard lock(m_com_mutex);
    auto found = m_audio_capture_clients.find(m.arg_ptr(0));
    return found == m_audio_capture_clients.end() ? 0u : ++found->second.refs;
}

u32 ComCoreManager::audio_capture_client_release(Marshaler& m) {
    u32 refs = 0;
    {
        std::lock_guard lock(m_com_mutex);
        auto found = m_audio_capture_clients.find(m.arg_ptr(0));
        if (found == m_audio_capture_clients.end()) return 0;
        refs = --found->second.refs;
        if (refs == 0) {
            auto client = m_audio_clients.find(found->second.client);
            m_audio_capture_clients.erase(found);
            if (client != m_audio_clients.end()) {
                if (client->second.capture_client == m.arg_ptr(0)) {
                    client->second.capture_client = 0;
                }
                if (--client->second.refs == 0) destroy_audio_client_locked(client);
            }
        }
    }
    if (refs == 0) finish_audio_change(m);
    return refs;
}

u32 ComCoreManager::audio_capture_client_get_buffer(Marshaler& m) {
    // HRESULT GetBuffer(BYTE** ppData, UINT32* pNumFramesToRead, DWORD* pdwFlags,
    //                   UINT64* pu64DevicePosition, UINT64* pu64QPCPosition)
    const GuestAddr out_data = m.arg_ptr(1), out_frames = m.arg_ptr(2),
                    out_flags = m.arg_ptr(3), out_position = m.arg_ptr(4),
                    out_qpc = m.arg_ptr(5);
    if (out_data == 0 || out_frames == 0 || out_flags == 0) return com::E_POINTER;
    std::lock_guard lock(m_com_mutex);
    auto service = m_audio_capture_clients.find(m.arg_ptr(0));
    if (service == m_audio_capture_clients.end()) return E_NOINTERFACE;
    auto client = m_audio_clients.find(service->second.client);
    if (client == m_audio_clients.end()) return E_NOINTERFACE;
    auto& state = client->second;
    if (state.capture_held) return AUDCLNT_E_OUT_OF_ORDER;
    const CapturePacket packet = capture_packet(state);
    const u32 frames = packet.frames;
    if (frames == 0) {
        // Measured: *ppData NULL and *pNumFramesToRead 0; the flags and both
        // positions are left as they were.
        (void)m.write_ptr(out_data, 0);
        (void)m.write_u32(out_frames, 0);
        return capture_contract::AUDCLNT_S_BUFFER_EMPTY;
    }
    audio::CaptureRing& ring = state.capture_stream->ring();
    const audio::CaptureRing::Span span = ring.next(frames);
    const u32 align = state.format.block_align();
    std::vector<u8> copy(static_cast<usize>(frames) * align);
    (void)ring.peek(copy.data(), frames);
    // Learn (Session Volume Controls): the engine multiplies a stream's
    // samples by the product of its stream, session-channel and session
    // master levels. The capture endpoint's sessions have their own levels
    // (audio_endpoint_volume_locked); Windows' effect on captured samples is
    // unmeasured (Windows's inputs are silent).
    const std::vector<float> gains = audio_stream_gains_locked(state);
    if (std::any_of(gains.begin(), gains.end(), [](float g) { return g != 1.0f; })) {
        audio::apply_channel_gains(state.format, copy.data(), frames, gains.data());
    }
    if (m.memory().write_bytes(state.staging, reinterpret_cast<const Byte*>(copy.data()),
                               copy.size()).is_err() ||
        m.write_ptr(out_data, state.staging).is_err() ||
        m.write_u32(out_frames, frames).is_err() ||
        m.write_u32(out_flags, span.discontinuity
                                   ? capture_contract::AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY
                                   : 0u).is_err()) {
        return com::E_POINTER;
    }
    // Device position: the engine frame where the packet's period starts
    // (measured: 480 * k, engine frames even when AUTOCONVERTPCM converts).
    if (out_position != 0) {
        (void)m.memory().write_u64(out_position, state.capture_periods.position(packet.period));
    }
    if (out_qpc != 0) (void)m.memory().write_u64(out_qpc, span.qpc_hns);
    state.capture_held = true;
    state.capture_held_frames = frames;
    return com::S_OK;
}

u32 ComCoreManager::audio_capture_client_release_buffer(Marshaler& m) {
    // HRESULT ReleaseBuffer(UINT32 NumFramesRead)
    const u32 frames = static_cast<u32>(m.arg(1));
    std::lock_guard lock(m_com_mutex);
    auto service = m_audio_capture_clients.find(m.arg_ptr(0));
    if (service == m_audio_capture_clients.end()) return E_NOINTERFACE;
    auto client = m_audio_clients.find(service->second.client);
    if (client == m_audio_clients.end()) return E_NOINTERFACE;
    auto& state = client->second;
    // Measured: releasing 0 frames with nothing held succeeds (the optional
    // release after an empty GetBuffer); anything else then is out of order.
    if (!state.capture_held) {
        return frames == 0 ? com::S_OK : capture_contract::AUDCLNT_E_OUT_OF_ORDER;
    }
    // Learn: all or nothing. 0 keeps the packet for the next GetBuffer.
    if (frames == 0) {
        state.capture_held = false;
        return com::S_OK;
    }
    if (frames != state.capture_held_frames) return capture_contract::AUDCLNT_E_INVALID_SIZE;
    state.capture_stream->ring().consume(frames);
    state.capture_held = false;
    state.capture_held_frames = 0;
    state.needs_reset = true;
    return com::S_OK;
}

u32 ComCoreManager::audio_capture_client_get_next_packet_size(Marshaler& m) {
    const GuestAddr out = m.arg_ptr(1);
    if (out == 0) return com::E_POINTER;
    std::lock_guard lock(m_com_mutex);
    auto service = m_audio_capture_clients.find(m.arg_ptr(0));
    if (service == m_audio_capture_clients.end()) return E_NOINTERFACE;
    auto client = m_audio_clients.find(service->second.client);
    if (client == m_audio_clients.end()) return E_NOINTERFACE;
    return m.write_u32(out, capture_packet(client->second).frames).is_ok()
        ? com::S_OK : com::E_POINTER;
}

u32 ComCoreManager::audio_render_client_query_interface(Marshaler& m) {
    const GuestAddr object = m.arg_ptr(0), iid_ptr = m.arg_ptr(1), out = m.arg_ptr(2);
    if (iid_ptr == 0 || out == 0) return com::E_POINTER;
    (void)m.write_ptr(out, 0);
    const auto iid = m.read_struct<Guid>(iid_ptr);
    if (iid.is_err()) return com::E_INVALIDARG;
    std::lock_guard lock(m_com_mutex);
    auto found = m_audio_render_clients.find(object);
    if (found == m_audio_render_clients.end() ||
        (!guid_equal(iid.value(), IID_IUNKNOWN) &&
         !guid_equal(iid.value(), IID_IAUDIORENDERCLIENT))) return E_NOINTERFACE;
    ++found->second.refs;
    return m.write_ptr(out, object).is_ok() ? com::S_OK : com::E_INVALIDARG;
}

u32 ComCoreManager::audio_render_client_add_ref(Marshaler& m) {
    std::lock_guard lock(m_com_mutex);
    auto found = m_audio_render_clients.find(m.arg_ptr(0));
    return found == m_audio_render_clients.end() ? 0u : ++found->second.refs;
}

u32 ComCoreManager::audio_render_client_release(Marshaler& m) {
    u32 refs = 0;
    {
        std::lock_guard lock(m_com_mutex);
        auto found = m_audio_render_clients.find(m.arg_ptr(0));
        if (found == m_audio_render_clients.end()) return 0;
        refs = --found->second.refs;
        if (refs == 0) {
            // The service holds a reference on its IAudioClient.
            auto client = m_audio_clients.find(found->second.client);
            m_audio_render_clients.erase(found);
            if (client != m_audio_clients.end()) {
                if (client->second.render_client == m.arg_ptr(0)) {
                    client->second.render_client = 0;
                }
                if (--client->second.refs == 0) destroy_audio_client_locked(client);
            }
        }
    }
    if (refs == 0) finish_audio_change(m);
    return refs;
}

u32 ComCoreManager::audio_render_client_get_buffer(Marshaler& m) {
    // HRESULT GetBuffer(UINT32 NumFramesRequested, BYTE** ppData)
    const u32 frames = static_cast<u32>(m.arg(1));
    const GuestAddr out = m.arg_ptr(2);
    if (out == 0) return com::E_POINTER;
    const double wait_start = diagnostics::present_cpu::host_mono_ms();
    std::lock_guard lock(m_com_mutex);
    const double wait_us = (diagnostics::present_cpu::host_mono_ms() - wait_start) * 1000.0;
    auto service = m_audio_render_clients.find(m.arg_ptr(0));
    if (service == m_audio_render_clients.end()) return E_NOINTERFACE;
    auto client = m_audio_clients.find(service->second.client);
    if (client == m_audio_clients.end()) return E_NOINTERFACE;
    auto& state = client->second;
    state.diag.get_buffer_lock_max_us =
        std::max(state.diag.get_buffer_lock_max_us, wait_us);
    if (state.buffer_pending) return AUDCLNT_E_OUT_OF_ORDER;
    // Learn: a request for 0 frames succeeds without writing *ppData, and
    // needs no ReleaseBuffer.
    if (frames == 0) return com::S_OK;
    const u32 padding = state.voice->padding_frames();
    if (frames > state.buffer_frames - std::min(padding, state.buffer_frames)) {
        return AUDCLNT_E_BUFFER_TOO_LARGE;
    }
    if (m.write_ptr(out, state.staging).is_err()) return com::E_POINTER;
    state.buffer_pending = true;
    state.pending_frames = frames;
    return com::S_OK;
}

u32 ComCoreManager::audio_render_client_release_buffer(Marshaler& m) {
    // HRESULT ReleaseBuffer(UINT32 NumFramesWritten, DWORD dwFlags)
    const u32 frames = static_cast<u32>(m.arg(1));
    const u32 flags = static_cast<u32>(m.arg(2));
    // Destroyed after `lock`, so the summary prints outside m_com_mutex.
    DeferredLine line;
    const double wait_start = diagnostics::present_cpu::host_mono_ms();
    std::lock_guard lock(m_com_mutex);
    const double locked_at = diagnostics::present_cpu::host_mono_ms();
    auto service = m_audio_render_clients.find(m.arg_ptr(0));
    if (service == m_audio_render_clients.end()) return E_NOINTERFACE;
    auto client = m_audio_clients.find(service->second.client);
    if (client == m_audio_clients.end()) return E_NOINTERFACE;
    auto& state = client->second;
    note_render_release(line, client->first, state, locked_at,
                        (locked_at - wait_start) * 1000.0, frames,
                        (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0);
    if ((flags & ~AUDCLNT_BUFFERFLAGS_SILENT) != 0) return com::E_INVALIDARG;
    // Learn: releasing 0 frames is always permitted — it hands back an
    // unused packet (or is a no-op with none outstanding).
    if (frames == 0) {
        state.buffer_pending = false;
        state.pending_frames = 0;
        return com::S_OK;
    }
    if (!state.buffer_pending) return AUDCLNT_E_OUT_OF_ORDER;
    if (frames > state.pending_frames) return AUDCLNT_E_INVALID_SIZE;
    const bool silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
    const u32 align = state.format.block_align();
    const Byte* source = silent ? nullptr
                                : m.memory().host_ptr(state.staging, frames * align);
    if (!silent && source == nullptr) return com::E_POINTER;
    // Diagnostic capture of the exact committed guest stream, before the
    // host backend can alter timing or representation.
    static const std::optional<std::string> dump_path =
        []() -> std::optional<std::string> {
        const char* path = std::getenv("VITREO_WASAPI_DUMP");
        if (path == nullptr) return std::nullopt;
        return std::string(path);
    }();
    if (dump_path.has_value()) {
        static std::mutex dump_mutex;
        static FILE* dump = nullptr;
        std::lock_guard dump_lock(dump_mutex);
        if (dump == nullptr) dump = std::fopen(dump_path->c_str(), "ab");
        if (dump != nullptr) {
            // A SILENT packet is written as zeros so the dump's frame index
            // stays the committed stream's timeline.
            if (silent) {
                static const std::vector<Byte> zeros(4096, 0);
                for (usize left = static_cast<usize>(frames) * align; left != 0;) {
                    const usize chunk = std::min(left, zeros.size());
                    (void)std::fwrite(zeros.data(), 1, chunk, dump);
                    left -= chunk;
                }
            } else {
                (void)std::fwrite(source, align, frames, dump);
            }
            (void)std::fflush(dump);
            if (g_dump_frames.fetch_add(frames, std::memory_order_relaxed) == 0) {
                std::fprintf(stderr,
                    "[wasapi-dump] first_frame t_ms=%.1f c=0x%llx rate=%u ch=%u "
                    "bits=%u float=%d path=%s\n",
                    diagnostics::present_cpu::host_mono_ms(),
                    static_cast<unsigned long long>(client->first),
                    state.format.sample_rate, state.format.channels,
                    state.format.bits_per_sample, state.format.float_samples ? 1 : 0,
                    dump_path->c_str());
            }
        }
    }
    if (!state.voice->submit(source, frames, silent)) {
        return AUDCLNT_E_DEVICE_INVALIDATED;
    }
    state.submitted_frames += frames;
    state.buffer_pending = false;
    state.pending_frames = 0;
    state.needs_reset = true;
    return com::S_OK;
}

}  // namespace vitreo::winapi
