// SPDX-License-Identifier: LGPL-2.1-only
/// @file wasapi_services.cpp
/// @brief IAudioClient::GetService and the service objects it hands out:
/// IAudioClock / IAudioClock2, IAudioClockAdjustment, IAudioSessionControl(2)
/// with ISimpleAudioVolume and IChannelAudioVolume, IAudioStreamVolume, and
/// IAudioSessionEvents delivery.
///
/// Sources: Learn (IAudioClient::GetService and each interface's method
/// pages), the SDK headers audioclient.h / audiopolicy.h (vtable order,
/// IIDs, HRESULTs: wasapi_services_contract.inc, generated), and the Windows
/// runs tools/oracle_probe/wasapi_services_probe{32,64}.expected for what
/// Learn leaves undocumented (object identity, clock units, NULL-pointer
/// HRESULTs, event and volume scope). .docs/contracts/ole32.md § WASAPI.

#include "com_core.hpp"
#include "ole_guid_internal.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/com/ole32/task_memory.hpp"
#include "winapi/com/rpcrt4/rpcrt4.hpp"
#include "winapi/kernel32/locale/string_conv.hpp"
#include "winapi/kernel32/module/module.hpp"

#include <vitreo/virtual_tsc.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <string_view>

#include <unistd.h>

namespace vitreo::winapi {

namespace {

#include "wasapi_services_contract.inc"

constexpr u32 E_NOINTERFACE = 0x80004002u;
constexpr u32 E_OUTOFMEMORY = 0x8007000Eu;
constexpr u32 COINIT_MULTITHREADED = 0x0u;
/// IChannelAudioVolume::SetAllVolumes reports ChangedChannel as (DWORD)-1.
constexpr u32 kAllChannelsChanged = 0xFFFFFFFFu;

// IAudioSessionEvents vtable slots (audiopolicy.h IAudioSessionEventsVtbl).
constexpr u32 kOnDisplayNameChanged = 3;
constexpr u32 kOnIconPathChanged = 4;
constexpr u32 kOnSimpleVolumeChanged = 5;
constexpr u32 kOnChannelVolumeChanged = 6;
constexpr u32 kOnGroupingParamChanged = 7;
constexpr u32 kOnStateChanged = 8;

/// The services Learn lists for IAudioClient::GetService (plus
/// IAudioClockAdjustment, which its own page says is obtained there).
/// Measured: an uninitialized client answers AUDCLNT_E_NOT_INITIALIZED for
/// these and E_NOINTERFACE for every other IID (IAudioClock2,
/// IAudioSessionControl2 and IUnknown included).
enum class ServiceIid : u8 {
    None, Render, Capture, Clock, ClockAdjustment, SessionControl, SimpleVolume,
    ChannelVolume, StreamVolume, DuckingControl, TrustedOutput
};

[[nodiscard]] ServiceIid classify_service_iid(const Guid& iid) {
    if (guid_equal(iid, IID_IAUDIORENDERCLIENT)) return ServiceIid::Render;
    if (guid_equal(iid, IID_IAUDIOCAPTURECLIENT)) return ServiceIid::Capture;
    if (guid_equal(iid, IID_IAUDIOCLOCK)) return ServiceIid::Clock;
    if (guid_equal(iid, IID_IAUDIOCLOCKADJUSTMENT)) return ServiceIid::ClockAdjustment;
    if (guid_equal(iid, IID_IAUDIOSESSIONCONTROL)) return ServiceIid::SessionControl;
    if (guid_equal(iid, IID_ISIMPLEAUDIOVOLUME)) return ServiceIid::SimpleVolume;
    if (guid_equal(iid, IID_ICHANNELAUDIOVOLUME)) return ServiceIid::ChannelVolume;
    if (guid_equal(iid, IID_IAUDIOSTREAMVOLUME)) return ServiceIid::StreamVolume;
    if (guid_equal(iid, IID_IAUDIOCLIENTDUCKINGCONTROL)) return ServiceIid::DuckingControl;
    if (guid_equal(iid, IID_IMFTRUSTEDOUTPUT)) return ServiceIid::TrustedOutput;
    return ServiceIid::None;
}

[[nodiscard]] bool valid_level(float level) {
    // NaN fails both comparisons.
    return level >= 0.0f && level <= 1.0f;
}

[[nodiscard]] u32 float_bits(float value) {
    u32 bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

/// Read a NUL-terminated UTF-16 string exactly (no code-page round trip).
[[nodiscard]] bool read_utf16(Marshaler& m, GuestAddr address, std::u16string& out) {
    out.clear();
    for (u32 index = 0; index < (1u << 20); ++index) {
        const auto unit = m.memory().read_u16(address + static_cast<GuestAddr>(index) * 2u);
        if (unit.is_err()) return false;
        if (unit.value() == 0) return true;
        out.push_back(static_cast<char16_t>(unit.value()));
    }
    return false;
}

[[nodiscard]] bool write_utf16(Marshaler& m, GuestAddr address, std::u16string_view text) {
    for (usize index = 0; index < text.size(); ++index) {
        if (m.memory().write_u16(address + static_cast<GuestAddr>(index * 2u),
                                 static_cast<u16>(text[index])).is_err()) return false;
    }
    return m.memory().write_u16(address + static_cast<GuestAddr>(text.size() * 2u), 0).is_ok();
}

void append_utf8(std::u16string& out, std::string_view utf8) {
    for (const u16 unit : utf8_to_utf16(utf8)) {
        if (unit == 0) break;
        out.push_back(static_cast<char16_t>(unit));
    }
}

[[nodiscard]] Guid fresh_guid() {
    const auto bytes = generate_uuid_v4();
    Guid guid{};
    static_assert(sizeof(guid) == 16);
    std::memcpy(&guid, bytes.data(), sizeof(guid));
    return guid;
}

/// The process image as GetProcessImageFileNameW reports it (psapi.cpp):
/// the capsule's drive is \Device\HarddiskVolume1.
[[nodiscard]] std::string nt_image_path(std::string_view dos_path) {
    std::string nt = "\\Device\\HarddiskVolume1";
    if (dos_path.size() > 2 && dos_path[1] == ':') nt += dos_path.substr(2);
    else nt += dos_path;
    std::replace(nt.begin(), nt.end(), '/', '\\');
    return nt;
}

void log_unimplemented_once(std::once_flag& once, const char* what) {
    std::call_once(once, [what] {
        std::fprintf(stderr, "[vitreo] UNIMPLEMENTED %s\n", what);
    });
}

}  // namespace

void note_unimplemented_audio_client_version(const Guid& iid, const char* api) {
    if (guid_equal(iid, IID_IAUDIOCLIENT2)) {
        static std::once_flag once;
        std::call_once(once, [api] {
            std::fprintf(stderr, "[vitreo] UNIMPLEMENTED %s: IID_IAudioClient2\n", api);
        });
    } else if (guid_equal(iid, IID_IAUDIOCLIENT3)) {
        static std::once_flag once;
        std::call_once(once, [api] {
            std::fprintf(stderr, "[vitreo] UNIMPLEMENTED %s: IID_IAudioClient3\n", api);
        });
    }
}

u64 audio_qpc_now_hns() noexcept {
    // kQpcHz is 10 MHz: guest QPC ticks already are 100 ns units.
    static_assert(kQpcHz == 10'000'000ULL);
    return virtual_qpc_ticks();
}

u64 ComCoreManager::audio_played_frames(const AudioClientState& state) {
    // A capture stream's position counts the frames the device delivered
    // into the endpoint buffer (measured: it stops at the buffer size while
    // nothing is read, and Reset returns it to 0).
    if (state.capture) {
        return state.initialized && state.capture_stream
            ? state.capture_stream->ring().stored_total() : 0;
    }
    // Learn (IAudioClock::GetPosition): a render stream's position is the
    // sample "currently playing through the speakers", so it counts the
    // client frames the device has finished playing -- not frames the engine
    // has read but the host pipeline still holds, and not the silence an
    // underrun plays.
    if (!state.initialized || !state.voice) return 0;
    return state.voice->played_frames();
}

namespace {

/// IAudioClock2 device position: frames at the engine (device) rate. The
/// stream's frames play at format rate x rate ratio, so each played stream
/// frame spans engine_rate / (format_rate x ratio) device frames.
template <typename State>
[[nodiscard]] u64 audio_device_frames(const State& state, u64 played) {
    const double per_frame = static_cast<double>(ComCoreManager::AUDIO_ENGINE_RATE) /
        (static_cast<double>(state.format.sample_rate) * state.rate_ratio);
    const double frames = state.device_frames_at_checkpoint +
        static_cast<double>(played - std::min(played, state.played_frames_at_checkpoint)) *
            per_frame;
    return static_cast<u64>(frames);
}

}  // namespace

// ---------------------------------------------------------------------------
// IAudioClient::GetService
// ---------------------------------------------------------------------------

u32 ComCoreManager::audio_client_get_service(Marshaler& m) {
    const GuestAddr object = m.arg_ptr(0), iid_ptr = m.arg_ptr(1), out = m.arg_ptr(2);
    // Measured: a NULL ppv is E_POINTER whether or not the stream is
    // initialized; every failure leaves *ppv NULL (Learn).
    if (out == 0 || m.write_ptr(out, 0).is_err()) return com::E_POINTER;
    if (iid_ptr == 0) return com::E_POINTER;
    const auto iid = m.read_struct<Guid>(iid_ptr);
    if (iid.is_err()) return com::E_INVALIDARG;
    const ServiceIid service = classify_service_iid(iid.value());
    u32 flags = 0;
    bool capture = false;
    {
        std::lock_guard lock(m_com_mutex);
        auto found = m_audio_clients.find(object);
        if (found == m_audio_clients.end()) return E_NOINTERFACE;
        if (service == ServiceIid::None) return E_NOINTERFACE;
        if (!found->second.initialized) return AUDCLNT_E_NOT_INITIALIZED;
        flags = found->second.stream_flags;
        capture = found->second.capture;
    }
    GuestAddr result = 0;
    switch (service) {
    case ServiceIid::None: return E_NOINTERFACE;
    case ServiceIid::Capture:
        // Learn: IAudioCaptureClient on a rendering endpoint (loopback, the
        // exception, is refused at Initialize).
        if (!capture) return AUDCLNT_E_WRONG_ENDPOINT_TYPE;
        result = create_audio_capture_client(m, object);
        break;
    case ServiceIid::DuckingControl: {
        static std::once_flag once;
        log_unimplemented_once(once, "IAudioClient::GetService: IID_IAudioClientDuckingControl");
        return E_NOINTERFACE;
    }
    case ServiceIid::TrustedOutput: {
        static std::once_flag once;
        log_unimplemented_once(once, "IAudioClient::GetService: IID_IMFTrustedOutput "
                                     "(output trust authority)");
        return E_NOINTERFACE;
    }
    case ServiceIid::Render:
        // Learn and measured: IAudioRenderClient on a capture endpoint.
        if (capture) return AUDCLNT_E_WRONG_ENDPOINT_TYPE;
        result = create_audio_render_client(m, object);
        break;
    case ServiceIid::Clock:
        result = get_audio_service(m, object, AudioServiceKind::Clock, AudioFacet::Clock);
        break;
    case ServiceIid::ClockAdjustment:
        // Measured: E_INVALIDARG unless Initialize had RATEADJUST.
        if ((flags & AUDCLNT_STREAMFLAGS_RATEADJUST) == 0) return com::E_INVALIDARG;
        result = get_audio_service(m, object, AudioServiceKind::ClockAdjustment,
                                   AudioFacet::ClockAdjustment);
        break;
    case ServiceIid::SessionControl:
        result = get_audio_service(m, object, AudioServiceKind::Session,
                                   AudioFacet::SessionControl);
        break;
    case ServiceIid::SimpleVolume:
        result = get_audio_service(m, object, AudioServiceKind::Session,
                                   AudioFacet::SimpleVolume);
        break;
    case ServiceIid::ChannelVolume:
        result = get_audio_service(m, object, AudioServiceKind::Session,
                                   AudioFacet::ChannelVolume);
        break;
    case ServiceIid::StreamVolume:
        result = get_audio_service(m, object, AudioServiceKind::StreamVolume,
                                   AudioFacet::StreamVolume);
        break;
    }
    if (result == 0) return E_OUTOFMEMORY;
    // *ppv was writable a moment ago (the NULL above).
    (void)m.write_ptr(out, result);
    return com::S_OK;
}

// ---------------------------------------------------------------------------
// Service objects
// ---------------------------------------------------------------------------

GuestAddr ComCoreManager::get_audio_service(Marshaler& m, GuestAddr client,
                                            AudioServiceKind kind, AudioFacet facet) {
    const usize kind_index = static_cast<usize>(kind);
    auto facet_pointer = [](const AudioServiceState& service, AudioFacet wanted) -> GuestAddr {
        for (const auto& [f, pointer] : service.facets) {
            if (f == wanted) return pointer;
        }
        return 0;
    };
    // A live object is shared by every GetService on this stream (measured:
    // the second GetService answers the same pointer).
    {
        std::lock_guard lock(m_com_mutex);
        auto found = m_audio_clients.find(client);
        if (found == m_audio_clients.end()) return 0;
        const GuestAddr block = found->second.services[kind_index];
        if (block != 0) {
            auto service = m_audio_services.find(block);
            if (service != m_audio_services.end()) {
                const GuestAddr pointer = facet_pointer(service->second, facet);
                if (pointer == 0) return 0;
                ++service->second.refs;
                return pointer;
            }
        }
    }
    if (m_thunks == nullptr || m_guest_task_memory == nullptr) return 0;

    static constexpr std::string_view kQi = "Vitreo_AudioService_QueryInterface";
    static constexpr std::string_view kAddRef = "Vitreo_AudioService_AddRef";
    static constexpr std::string_view kRelease = "Vitreo_AudioService_Release";
    static constexpr std::string_view kIdentity[] = {kQi, kAddRef, kRelease};
    static constexpr std::string_view kClock[] = {
        kQi, kAddRef, kRelease, "Vitreo_AudioClock_GetFrequency",
        "Vitreo_AudioClock_GetPosition", "Vitreo_AudioClock_GetCharacteristics"};
    static constexpr std::string_view kClock2[] = {
        kQi, kAddRef, kRelease, "Vitreo_AudioClock2_GetDevicePosition"};
    static constexpr std::string_view kClockAdjustment[] = {
        kQi, kAddRef, kRelease, "Vitreo_AudioClockAdjustment_SetSampleRate"};
    static constexpr std::string_view kSessionControl[] = {
        kQi, kAddRef, kRelease, "Vitreo_AudioSession_GetState",
        "Vitreo_AudioSession_GetDisplayName", "Vitreo_AudioSession_SetDisplayName",
        "Vitreo_AudioSession_GetIconPath", "Vitreo_AudioSession_SetIconPath",
        "Vitreo_AudioSession_GetGroupingParam", "Vitreo_AudioSession_SetGroupingParam",
        "Vitreo_AudioSession_RegisterAudioSessionNotification",
        "Vitreo_AudioSession_UnregisterAudioSessionNotification",
        "Vitreo_AudioSession_GetSessionIdentifier",
        "Vitreo_AudioSession_GetSessionInstanceIdentifier",
        "Vitreo_AudioSession_GetProcessId", "Vitreo_AudioSession_IsSystemSoundsSession",
        "Vitreo_AudioSession_SetDuckingPreference"};
    static constexpr std::string_view kSimpleVolume[] = {
        kQi, kAddRef, kRelease, "Vitreo_SimpleAudioVolume_SetMasterVolume",
        "Vitreo_SimpleAudioVolume_GetMasterVolume", "Vitreo_SimpleAudioVolume_SetMute",
        "Vitreo_SimpleAudioVolume_GetMute"};
    static constexpr std::string_view kChannelVolume[] = {
        kQi, kAddRef, kRelease, "Vitreo_ChannelAudioVolume_GetChannelCount",
        "Vitreo_ChannelAudioVolume_SetChannelVolume",
        "Vitreo_ChannelAudioVolume_GetChannelVolume",
        "Vitreo_ChannelAudioVolume_SetAllVolumes",
        "Vitreo_ChannelAudioVolume_GetAllVolumes"};
    static constexpr std::string_view kStreamVolume[] = {
        kQi, kAddRef, kRelease, "Vitreo_AudioStreamVolume_GetChannelCount",
        "Vitreo_AudioStreamVolume_SetChannelVolume",
        "Vitreo_AudioStreamVolume_GetChannelVolume",
        "Vitreo_AudioStreamVolume_SetAllVolumes",
        "Vitreo_AudioStreamVolume_GetAllVolumes"};
    auto methods_of = [&](AudioFacet f) -> std::span<const std::string_view> {
        switch (f) {
        case AudioFacet::Identity: return kIdentity;
        case AudioFacet::Clock: return kClock;
        case AudioFacet::Clock2: return kClock2;
        case AudioFacet::ClockAdjustment: return kClockAdjustment;
        case AudioFacet::SessionControl: return kSessionControl;
        case AudioFacet::SimpleVolume: return kSimpleVolume;
        case AudioFacet::ChannelVolume: return kChannelVolume;
        case AudioFacet::StreamVolume: return kStreamVolume;
        }
        return kIdentity;
    };
    // Measured QueryInterface graphs: IUnknown is a facet of its own, and the
    // session control, simple volume and channel volume are one object.
    std::vector<AudioFacet> facets;
    switch (kind) {
    case AudioServiceKind::Clock:
        facets = {AudioFacet::Identity, AudioFacet::Clock, AudioFacet::Clock2};
        break;
    case AudioServiceKind::ClockAdjustment:
        facets = {AudioFacet::Identity, AudioFacet::ClockAdjustment};
        break;
    case AudioServiceKind::Session:
        facets = {AudioFacet::Identity, AudioFacet::SessionControl,
                  AudioFacet::ChannelVolume, AudioFacet::SimpleVolume};
        break;
    case AudioServiceKind::StreamVolume:
        facets = {AudioFacet::Identity, AudioFacet::StreamVolume};
        break;
    }

    const bool x64 = m.cpu_is_64bit();
    const GuestAddr pointer_size = x64 ? 8u : 4u;
    std::vector<GuestAddr> vtables;
    {
        std::lock_guard lock(m_com_mutex);
        for (const AudioFacet f : facets) {
            const u32 key = (static_cast<u32>(f) << 1u) | (x64 ? 1u : 0u);
            auto cached = m_audio_service_vtables.find(key);
            if (cached == m_audio_service_vtables.end()) {
                // allocate_builtin_object lays out {vtable*}{methods}; the
                // vtable is shared by every object of this facet.
                const GuestAddr holder = allocate_builtin_object(m, methods_of(f));
                if (holder == 0) return 0;
                cached = m_audio_service_vtables.emplace(key, holder + pointer_size).first;
            }
            vtables.push_back(cached->second);
        }
    }
    const GuestAddr block = m_guest_task_memory->allocate(
        static_cast<u64>(facets.size()) * pointer_size);
    if (block == 0) return 0;
    for (usize index = 0; index < facets.size(); ++index) {
        if (m.write_ptr(block + static_cast<GuestAddr>(index) * pointer_size,
                        vtables[index]).is_err()) {
            m_guest_task_memory->free(block);
            return 0;
        }
    }

    std::lock_guard lock(m_com_mutex);
    auto found = m_audio_clients.find(client);
    if (found == m_audio_clients.end()) {
        m_guest_task_memory->free(block);
        return 0;
    }
    // Another thread may have created the object meanwhile: share it.
    if (const GuestAddr existing = found->second.services[kind_index]; existing != 0) {
        m_guest_task_memory->free(block);
        auto service = m_audio_services.find(existing);
        if (service == m_audio_services.end()) return 0;
        const GuestAddr pointer = facet_pointer(service->second, facet);
        if (pointer == 0) return 0;
        ++service->second.refs;
        return pointer;
    }
    AudioServiceState state{};
    state.kind = kind;
    state.refs = 1;
    state.client = client;
    GuestAddr result = 0;
    for (usize index = 0; index < facets.size(); ++index) {
        const GuestAddr pointer = block + static_cast<GuestAddr>(index) * pointer_size;
        state.facets.emplace_back(facets[index], pointer);
        m_audio_service_facets[pointer] = {block, facets[index]};
        if (facets[index] == facet) result = pointer;
    }
    // Measured: each service object holds one reference on its client.
    ++found->second.refs;
    found->second.services[kind_index] = block;
    m_audio_services.emplace(block, std::move(state));
    return result;
}

ComCoreManager::AudioServiceState* ComCoreManager::find_audio_service_locked(
    GuestAddr facet, AudioFacet expected) {
    auto found = m_audio_service_facets.find(facet);
    if (found == m_audio_service_facets.end() || found->second.second != expected) {
        return nullptr;
    }
    auto service = m_audio_services.find(found->second.first);
    return service == m_audio_services.end() ? nullptr : &service->second;
}

ComCoreManager::AudioClientState* ComCoreManager::audio_service_client_locked(
    GuestAddr facet, AudioFacet expected) {
    AudioServiceState* service = find_audio_service_locked(facet, expected);
    if (service == nullptr) return nullptr;
    auto client = m_audio_clients.find(service->client);
    return client == m_audio_clients.end() ? nullptr : &client->second;
}

void ComCoreManager::destroy_audio_service_locked(
    std::unordered_map<GuestAddr, AudioServiceState>::iterator service) {
    const GuestAddr block = service->first;
    const GuestAddr client = service->second.client;
    const usize kind_index = static_cast<usize>(service->second.kind);
    for (const auto& [facet, pointer] : service->second.facets) {
        (void)facet;
        m_audio_service_facets.erase(pointer);
    }
    m_audio_services.erase(service);
    if (m_guest_task_memory != nullptr) m_guest_task_memory->free(block);
    auto found = m_audio_clients.find(client);
    if (found == m_audio_clients.end()) return;
    if (found->second.services[kind_index] == block) found->second.services[kind_index] = 0;
    // The object's reference on its IAudioClient.
    if (--found->second.refs == 0) destroy_audio_client_locked(found);
}

u32 ComCoreManager::audio_service_query_interface(Marshaler& m) {
    const GuestAddr object = m.arg_ptr(0), iid_ptr = m.arg_ptr(1), out = m.arg_ptr(2);
    if (out == 0) return com::E_POINTER;
    (void)m.write_ptr(out, 0);
    if (iid_ptr == 0) return com::E_POINTER;
    const auto iid = m.read_struct<Guid>(iid_ptr);
    if (iid.is_err()) return com::E_INVALIDARG;
    const Guid& wanted = iid.value();
    std::lock_guard lock(m_com_mutex);
    auto facet = m_audio_service_facets.find(object);
    if (facet == m_audio_service_facets.end()) return E_NOINTERFACE;
    auto service = m_audio_services.find(facet->second.first);
    if (service == m_audio_services.end()) return E_NOINTERFACE;
    AudioFacet target = AudioFacet::Identity;
    bool known = guid_equal(wanted, IID_IUNKNOWN);
    switch (service->second.kind) {
    case AudioServiceKind::Clock:
        if (guid_equal(wanted, IID_IAUDIOCLOCK)) { target = AudioFacet::Clock; known = true; }
        if (guid_equal(wanted, IID_IAUDIOCLOCK2)) { target = AudioFacet::Clock2; known = true; }
        break;
    case AudioServiceKind::ClockAdjustment:
        if (guid_equal(wanted, IID_IAUDIOCLOCKADJUSTMENT)) {
            target = AudioFacet::ClockAdjustment;
            known = true;
        }
        break;
    case AudioServiceKind::Session:
        if (guid_equal(wanted, IID_IAUDIOSESSIONCONTROL) ||
            guid_equal(wanted, IID_IAUDIOSESSIONCONTROL2)) {
            target = AudioFacet::SessionControl;
            known = true;
        }
        if (guid_equal(wanted, IID_ICHANNELAUDIOVOLUME)) {
            target = AudioFacet::ChannelVolume;
            known = true;
        }
        if (guid_equal(wanted, IID_ISIMPLEAUDIOVOLUME)) {
            target = AudioFacet::SimpleVolume;
            known = true;
        }
        break;
    case AudioServiceKind::StreamVolume:
        if (guid_equal(wanted, IID_IAUDIOSTREAMVOLUME)) {
            target = AudioFacet::StreamVolume;
            known = true;
        }
        break;
    }
    if (!known) return E_NOINTERFACE;
    for (const auto& [f, pointer] : service->second.facets) {
        if (f != target) continue;
        if (m.write_ptr(out, pointer).is_err()) return com::E_POINTER;
        ++service->second.refs;
        return com::S_OK;
    }
    return E_NOINTERFACE;
}

u32 ComCoreManager::audio_service_add_ref(Marshaler& m) {
    std::lock_guard lock(m_com_mutex);
    auto facet = m_audio_service_facets.find(m.arg_ptr(0));
    if (facet == m_audio_service_facets.end()) return 0;
    auto service = m_audio_services.find(facet->second.first);
    return service == m_audio_services.end() ? 0u : ++service->second.refs;
}

u32 ComCoreManager::audio_service_release(Marshaler& m) {
    u32 refs = 0;
    {
        std::lock_guard lock(m_com_mutex);
        auto facet = m_audio_service_facets.find(m.arg_ptr(0));
        if (facet == m_audio_service_facets.end()) return 0;
        auto service = m_audio_services.find(facet->second.first);
        if (service == m_audio_services.end()) return 0;
        refs = --service->second.refs;
        if (refs == 0) destroy_audio_service_locked(service);
    }
    if (refs == 0) finish_audio_change(m);
    return refs;
}

// ---------------------------------------------------------------------------
// IAudioClock / IAudioClock2 / IAudioClockAdjustment
// ---------------------------------------------------------------------------

namespace {
/// The QPC value paired with a position reading: now while the stream runs;
/// 0 before the first Start; frozen at the last Stop (measured, Reset keeps
/// it).
template <typename State>
[[nodiscard]] u64 position_qpc(const State& state) {
    return state.started ? audio_qpc_now_hns() : state.position_qpc;
}
}  // namespace

u32 ComCoreManager::audio_clock_get_frequency(Marshaler& m) {
    const GuestAddr out = m.arg_ptr(1);
    if (out == 0) return com::E_POINTER;
    std::lock_guard lock(m_com_mutex);
    const AudioClientState* state = audio_service_client_locked(m.arg_ptr(0), AudioFacet::Clock);
    if (state == nullptr) return E_NOINTERFACE;
    // Measured: shared-mode positions are stream bytes, so the frequency is
    // the stream format's nAvgBytesPerSec (192000 for 16-bit stereo 48 kHz,
    // 384000 for the float mix format).
    const u64 frequency = static_cast<u64>(state->format.sample_rate) *
                          state->format.block_align();
    return m.write_u64(out, frequency).is_ok() ? com::S_OK : com::E_POINTER;
}

u32 ComCoreManager::audio_clock_get_position(Marshaler& m) {
    const GuestAddr position = m.arg_ptr(1), qpc = m.arg_ptr(2);
    // Learn: E_POINTER for a NULL pu64Position (measured: *qpc untouched).
    if (position == 0) return com::E_POINTER;
    std::lock_guard lock(m_com_mutex);
    const AudioClientState* state = audio_service_client_locked(m.arg_ptr(0), AudioFacet::Clock);
    if (state == nullptr) return E_NOINTERFACE;
    const u64 bytes = audio_played_frames(*state) * state->format.block_align();
    if (m.write_u64(position, bytes).is_err()) return com::E_POINTER;
    if (qpc != 0 && m.write_u64(qpc, position_qpc(*state)).is_err()) return com::E_POINTER;
    return com::S_OK;
}

u32 ComCoreManager::audio_clock_get_characteristics(Marshaler& m) {
    const GuestAddr out = m.arg_ptr(1);
    if (out == 0) return com::E_POINTER;
    std::lock_guard lock(m_com_mutex);
    if (audio_service_client_locked(m.arg_ptr(0), AudioFacet::Clock) == nullptr) {
        return E_NOINTERFACE;
    }
    return m.write_u32(out, kAudioClockCharacteristics).is_ok() ? com::S_OK : com::E_POINTER;
}

u32 ComCoreManager::audio_clock2_get_device_position(Marshaler& m) {
    const GuestAddr position = m.arg_ptr(1), qpc = m.arg_ptr(2);
    if (position == 0) return com::E_POINTER;
    std::lock_guard lock(m_com_mutex);
    const AudioClientState* state = audio_service_client_locked(m.arg_ptr(0), AudioFacet::Clock2);
    if (state == nullptr) return E_NOINTERFACE;
    const u64 frames = audio_device_frames(*state, audio_played_frames(*state));
    if (m.write_u64(position, frames).is_err()) return com::E_POINTER;
    if (qpc != 0 && m.write_u64(qpc, position_qpc(*state)).is_err()) return com::E_POINTER;
    // Learn: AUDCLNT_S_POSITION_STALLED until Start has been called.
    return state->ever_started ? com::S_OK : AUDCLNT_S_POSITION_STALLED;
}

u32 ComCoreManager::audio_clock_adjustment_set_sample_rate(Marshaler& m) {
    const float rate = m.arg_f32(1);
    std::lock_guard lock(m_com_mutex);
    AudioClientState* state = audio_service_client_locked(m.arg_ptr(0),
                                                          AudioFacet::ClockAdjustment);
    if (state == nullptr) return E_NOINTERFACE;
    if (!state->initialized) return AUDCLNT_E_NOT_INITIALIZED;
    // Learn: E_INVALIDARG when the rate is outside what the resampler
    // supports (here, the host voice's play-rate range).
    if (!std::isfinite(rate) || rate <= 0.0f) return com::E_INVALIDARG;
    if (state->capture) {
        // The host input stream runs at its device rate; no resampler is
        // inserted for capture rate adjustment.
        static std::once_flag once;
        log_unimplemented_once(once, "IAudioClockAdjustment::SetSampleRate: capture stream");
        return com::E_INVALIDARG;
    }
    const double ratio = static_cast<double>(rate) /
                         static_cast<double>(state->format.sample_rate);
    const u64 played = audio_played_frames(*state);
    const u64 device = audio_device_frames(*state, played);
    if (!state->voice->set_rate_ratio(ratio)) return com::E_INVALIDARG;
    state->device_frames_at_checkpoint = static_cast<double>(device);
    state->played_frames_at_checkpoint = played;
    state->rate_ratio = ratio;
    return com::S_OK;
}

// ---------------------------------------------------------------------------
// Sessions
// ---------------------------------------------------------------------------

void ComCoreManager::join_audio_session_locked(AudioClientState& state,
                                               const Guid& session_guid) {
    const bool cross = (state.stream_flags & AUDCLNT_STREAMFLAGS_CROSSPROCESS) != 0;
    u32 id = 0;
    // Sessions are per endpoint: a capture stream never joins a render
    // session or another device's session.
    for (auto& [key, session] : m_audio_sessions) {
        if (session.cross_process == cross && guid_equal(session.guid, session_guid) &&
            session.capture == state.capture && session.capture_uid == state.capture_uid) {
            id = key;
            break;
        }
    }
    if (id == 0) {
        id = m_next_audio_session++;
        AudioSessionState session{};
        session.guid = session_guid;
        session.cross_process = cross;
        session.capture = state.capture;
        session.capture_uid = state.capture_uid;
        session.channels = state.format.channels;
        session.grouping = fresh_guid();
        m_audio_sessions.emplace(id, std::move(session));
    }
    ++m_audio_sessions[id].streams;
    state.session = id;
    auto& volume = audio_endpoint_volume_locked(state.capture, state.capture_uid);
    if (volume.channels.size() < state.format.channels) {
        volume.channels.resize(state.format.channels, 1.0f);
    }
}

void ComCoreManager::leave_audio_session_locked(AudioClientState& state) {
    const u32 id = state.session;
    state.session = 0;
    auto found = m_audio_sessions.find(id);
    if (found == m_audio_sessions.end()) return;
    if (state.started) update_audio_session_state_locked(id, -1);
    auto& session = found->second;
    if (session.streams > 0) --session.streams;
    if (session.streams != 0) return;
    // The session goes with its last stream, and with it every registration
    // (measured: the sink is Released, with no OnStateChanged first).
    for (auto& registration : session.sinks) {
        if (registration->pins != 0) registration->release_pending = true;
        else m_audio_sink_releases.push_back(registration->sink);
    }
    m_audio_sessions.erase(found);
}

std::vector<float> ComCoreManager::audio_stream_gains_locked(const AudioClientState& state) {
    const AudioEndpointVolume& volume =
        audio_endpoint_volume_locked(state.capture, state.capture_uid);
    const float master = volume.mute != 0 ? 0.0f : volume.master;
    std::vector<float> gains(state.format.channels, 1.0f);
    for (u32 channel = 0; channel < gains.size(); ++channel) {
        const float stream = channel < state.stream_volumes.size()
            ? state.stream_volumes[channel] : 1.0f;
        const float session = channel < volume.channels.size() ? volume.channels[channel] : 1.0f;
        gains[channel] = stream * session * master;
    }
    return gains;
}

void ComCoreManager::apply_audio_gains_locked(GuestAddr only) {
    // Render streams apply the gain as the voice dequeues; capture streams
    // apply it as GetBuffer copies a packet out (audio_stream_gains_locked).
    auto apply = [&](AudioClientState& state) {
        if (!state.initialized || !state.voice) return;
        const std::vector<float> gains = audio_stream_gains_locked(state);
        state.voice->set_channel_gains(gains.data(), static_cast<u32>(gains.size()));
    };
    if (only != 0) {
        auto found = m_audio_clients.find(only);
        if (found != m_audio_clients.end()) apply(found->second);
        return;
    }
    for (auto& [address, state] : m_audio_clients) {
        (void)address;
        apply(state);
    }
}

void ComCoreManager::update_audio_session_state_locked(u32 session, int running_delta) {
    auto found = m_audio_sessions.find(session);
    if (found == m_audio_sessions.end()) return;
    auto& state = found->second;
    if (running_delta > 0) ++state.running_streams;
    else if (running_delta < 0 && state.running_streams > 0) --state.running_streams;
    const u32 next = state.running_streams != 0 ? kAudioSessionStateActive
                                                : kAudioSessionStateInactive;
    if (next == state.state) return;
    state.state = next;
    AudioSessionEvent event{};
    event.slot = kOnStateChanged;
    event.session = session;
    event.state = next;
    queue_audio_session_event_locked(std::move(event));
}

void ComCoreManager::queue_audio_session_event_locked(AudioSessionEvent event) {
    auto found = m_audio_sessions.find(event.session);
    if (found == m_audio_sessions.end() || found->second.sinks.empty()) return;
    m_audio_session_events.push_back(std::move(event));
}

void ComCoreManager::finish_audio_change(Marshaler& m) {
    std::vector<GuestAddr> releases;
    bool launch = false;
    {
        std::lock_guard lock(m_com_mutex);
        releases.swap(m_audio_sink_releases);
        if (!m_audio_session_events.empty() && !m_audio_session_worker_running) {
            m_audio_session_worker_running = true;
            launch = true;
        }
    }
    for (const GuestAddr sink : releases) guest_iunknown_release(m, sink);
    if (!launch) return;
    // Learn: the session manager calls IAudioSessionEvents on its own thread
    // (measured: never the thread that made the change).
    bool started = false;
    if (m_thunks != nullptr && m_audio_activation_thread) {
        auto worker = m_thunks->get_thunk_address("ole32.dll", "Vitreo_AudioSession_EventWorker");
        started = worker.is_ok() && m_audio_activation_thread(worker.value(), 0);
    }
    if (started) return;
    {
        std::lock_guard lock(m_com_mutex);
        m_audio_session_worker_running = false;
        m_audio_session_events.clear();
    }
    static std::once_flag once;
    log_unimplemented_once(once, "audiopolicy IAudioSessionEvents delivery: "
                                 "no guest thread could be started");
}

u32 ComCoreManager::audio_session_event_worker(Marshaler& m) {
    const u32 init = co_initialize_ex(m, COINIT_MULTITHREADED);
    const bool x64 = m.cpu_is_64bit();
    const GuestAddr pointer_size = x64 ? 8u : 4u;
    for (;;) {
        AudioSessionEvent event{};
        std::vector<std::shared_ptr<AudioSinkRegistration>> sinks;
        {
            std::lock_guard lock(m_com_mutex);
            if (m_audio_session_events.empty()) {
                m_audio_session_worker_running = false;
                break;
            }
            event = std::move(m_audio_session_events.front());
            m_audio_session_events.pop_front();
            auto session = m_audio_sessions.find(event.session);
            if (session == m_audio_sessions.end()) continue;
            sinks = session->second.sinks;
            for (auto& registration : sinks) ++registration->pins;
        }
        // Arguments that are pointers live in guest memory for the calls.
        const u64 text_bytes = (event.text.size() + 1u) * 2u;
        const u64 volume_bytes = event.channel_volumes.size() * sizeof(float);
        const u64 bytes = 32u + text_bytes + volume_bytes;
        const GuestAddr payload = (m_guest_task_memory != nullptr && m_guest_call)
            ? m_guest_task_memory->allocate(bytes) : 0;
        const GuestAddr context = payload, grouping = payload + 16u;
        const GuestAddr text = payload + 32u;
        const GuestAddr volumes = text + static_cast<GuestAddr>(text_bytes);
        bool ready = payload != 0 && m.write_struct(context, event.context).is_ok() &&
                     m.write_struct(grouping, event.grouping).is_ok() &&
                     write_utf16(m, text, event.text);
        for (usize index = 0; ready && index < event.channel_volumes.size(); ++index) {
            ready = m.write_u32(volumes + static_cast<GuestAddr>(index * 4u),
                                float_bits(event.channel_volumes[index])).is_ok();
        }
        if (ready) {
            for (const auto& registration : sinks) {
                const GuestAddr sink = registration->sink;
                const GuestAddr vtable = static_cast<GuestAddr>(m.read_ptr(sink).value_or(0));
                const GuestAddr method = vtable == 0 ? 0 : static_cast<GuestAddr>(
                    m.read_ptr(vtable + event.slot * pointer_size).value_or(0));
                if (method == 0) continue;
                switch (event.slot) {
                case kOnDisplayNameChanged:
                case kOnIconPathChanged: {
                    const u64 args[] = {sink, text, context};
                    (void)m_guest_call(method, args, 3, x64);
                    break;
                }
                case kOnSimpleVolumeChanged: {
                    // HRESULT OnSimpleVolumeChanged(float, BOOL, LPCGUID): the
                    // float travels in XMM1 on x64 and in its stack slot on x86.
                    const u32 bits = float_bits(event.volume);
                    if (x64 && m.cpu() != nullptr) {
                        m.cpu()->sse.xmm[1].u64_[0] = bits;
                        m.cpu()->sse.xmm[1].u64_[1] = 0;
                    }
                    const u64 args[] = {sink, bits, event.mute != 0 ? 1u : 0u, context};
                    (void)m_guest_call(method, args, 4, x64);
                    break;
                }
                case kOnChannelVolumeChanged: {
                    const u64 args[] = {sink, event.channel_volumes.size(), volumes,
                                        event.changed_channel, context};
                    (void)m_guest_call(method, args, 5, x64);
                    break;
                }
                case kOnGroupingParamChanged: {
                    const u64 args[] = {sink, grouping, context};
                    (void)m_guest_call(method, args, 3, x64);
                    break;
                }
                case kOnStateChanged: {
                    const u64 args[] = {sink, event.state};
                    (void)m_guest_call(method, args, 2, x64);
                    break;
                }
                default: break;
                }
            }
        } else {
            static std::once_flag once;
            log_unimplemented_once(once, "audiopolicy IAudioSessionEvents delivery: "
                                         "no guest memory or guest caller for the arguments");
        }
        if (payload != 0) m_guest_task_memory->free(payload);
        {
            std::lock_guard lock(m_com_mutex);
            for (auto& registration : sinks) {
                if (--registration->pins == 0 && registration->release_pending) {
                    registration->release_pending = false;
                    m_audio_sink_releases.push_back(registration->sink);
                }
            }
        }
        finish_audio_change(m);
    }
    if ((init & 0x80000000u) == 0) co_uninitialize(m);
    return 0;
}

// ---------------------------------------------------------------------------
// IAudioSessionControl / IAudioSessionControl2
// ---------------------------------------------------------------------------

namespace {
/// Read an LPCGUID event context. Measured: a NULL context reaches the
/// notification as a pointer to GUID_NULL (Learn's "NULL context pointer"
/// does not survive the trip through the audio service).
[[nodiscard]] Guid read_event_context(Marshaler& m, GuestAddr address) {
    if (address == 0) return Guid{};
    return m.read_struct<Guid>(address).value_or(Guid{});
}
}  // namespace

u32 ComCoreManager::audio_session_get_state(Marshaler& m) {
    const GuestAddr out = m.arg_ptr(1);
    // Measured: NULL out-pointers on the session interfaces are rejected by
    // the RPC layer, HRESULT_FROM_WIN32(RPC_X_NULL_REF_POINTER).
    if (out == 0) return kHresultRpcNullRefPointer;
    std::lock_guard lock(m_com_mutex);
    const AudioClientState* client = audio_service_client_locked(m.arg_ptr(0),
                                                                 AudioFacet::SessionControl);
    if (client == nullptr) return E_NOINTERFACE;
    auto session = m_audio_sessions.find(client->session);
    if (session == m_audio_sessions.end()) return E_NOINTERFACE;
    return m.write_u32(out, session->second.state).is_ok() ? com::S_OK : com::E_POINTER;
}

u32 ComCoreManager::audio_session_get_display_name(Marshaler& m) {
    const GuestAddr out = m.arg_ptr(1);
    if (out == 0) return com::E_POINTER;
    (void)m.write_ptr(out, 0);
    std::u16string text;
    {
        std::lock_guard lock(m_com_mutex);
        const AudioClientState* client = audio_service_client_locked(
            m.arg_ptr(0), AudioFacet::SessionControl);
        if (client == nullptr) return E_NOINTERFACE;
        auto session = m_audio_sessions.find(client->session);
        if (session == m_audio_sessions.end()) return E_NOINTERFACE;
        text = session->second.display_name;
    }
    // Measured: an unset name is an allocated empty string, not NULL.
    if (m_guest_task_memory == nullptr) return E_OUTOFMEMORY;
    const GuestAddr copy = m_guest_task_memory->allocate((text.size() + 1u) * 2u);
    if (copy == 0) return E_OUTOFMEMORY;
    if (!write_utf16(m, copy, text) || m.write_ptr(out, copy).is_err()) {
        m_guest_task_memory->free(copy);
        return com::E_POINTER;
    }
    return com::S_OK;
}

u32 ComCoreManager::audio_session_get_icon_path(Marshaler& m) {
    const GuestAddr out = m.arg_ptr(1);
    if (out == 0) return com::E_POINTER;
    (void)m.write_ptr(out, 0);
    std::u16string text;
    {
        std::lock_guard lock(m_com_mutex);
        const AudioClientState* client = audio_service_client_locked(
            m.arg_ptr(0), AudioFacet::SessionControl);
        if (client == nullptr) return E_NOINTERFACE;
        auto session = m_audio_sessions.find(client->session);
        if (session == m_audio_sessions.end()) return E_NOINTERFACE;
        text = session->second.icon_path;
    }
    if (m_guest_task_memory == nullptr) return E_OUTOFMEMORY;
    const GuestAddr copy = m_guest_task_memory->allocate((text.size() + 1u) * 2u);
    if (copy == 0) return E_OUTOFMEMORY;
    if (!write_utf16(m, copy, text) || m.write_ptr(out, copy).is_err()) {
        m_guest_task_memory->free(copy);
        return com::E_POINTER;
    }
    return com::S_OK;
}

u32 ComCoreManager::audio_session_set_display_name(Marshaler& m) {
    const GuestAddr value = m.arg_ptr(1);
    if (value == 0) return kHresultRpcNullRefPointer;
    std::u16string text;
    if (!read_utf16(m, value, text)) return kHresultRpcNullRefPointer;
    const Guid context = read_event_context(m, m.arg_ptr(2));
    {
        std::lock_guard lock(m_com_mutex);
        const AudioClientState* client = audio_service_client_locked(
            m.arg_ptr(0), AudioFacet::SessionControl);
        if (client == nullptr) return E_NOINTERFACE;
        auto session = m_audio_sessions.find(client->session);
        if (session == m_audio_sessions.end()) return E_NOINTERFACE;
        // Measured: setting the current name again raises no event.
        if (session->second.display_name != text) {
            session->second.display_name = text;
            AudioSessionEvent event{};
            event.slot = kOnDisplayNameChanged;
            event.session = session->first;
            event.text = std::move(text);
            event.context = context;
            queue_audio_session_event_locked(std::move(event));
        }
    }
    finish_audio_change(m);
    return com::S_OK;
}

u32 ComCoreManager::audio_session_set_icon_path(Marshaler& m) {
    const GuestAddr value = m.arg_ptr(1);
    if (value == 0) return kHresultRpcNullRefPointer;
    std::u16string text;
    if (!read_utf16(m, value, text)) return kHresultRpcNullRefPointer;
    const Guid context = read_event_context(m, m.arg_ptr(2));
    {
        std::lock_guard lock(m_com_mutex);
        const AudioClientState* client = audio_service_client_locked(
            m.arg_ptr(0), AudioFacet::SessionControl);
        if (client == nullptr) return E_NOINTERFACE;
        auto session = m_audio_sessions.find(client->session);
        if (session == m_audio_sessions.end()) return E_NOINTERFACE;
        // Measured (supplement): the current path again raises no event.
        if (session->second.icon_path != text) {
            session->second.icon_path = text;
            AudioSessionEvent event{};
            event.slot = kOnIconPathChanged;
            event.session = session->first;
            event.text = std::move(text);
            event.context = context;
            queue_audio_session_event_locked(std::move(event));
        }
    }
    finish_audio_change(m);
    return com::S_OK;
}

u32 ComCoreManager::audio_session_get_grouping_param(Marshaler& m) {
    const GuestAddr out = m.arg_ptr(1);
    if (out == 0) return kHresultRpcNullRefPointer;
    std::lock_guard lock(m_com_mutex);
    const AudioClientState* client = audio_service_client_locked(m.arg_ptr(0),
                                                                 AudioFacet::SessionControl);
    if (client == nullptr) return E_NOINTERFACE;
    auto session = m_audio_sessions.find(client->session);
    if (session == m_audio_sessions.end()) return E_NOINTERFACE;
    return m.write_struct(out, session->second.grouping).is_ok() ? com::S_OK : com::E_POINTER;
}

u32 ComCoreManager::audio_session_set_grouping_param(Marshaler& m) {
    const GuestAddr value = m.arg_ptr(1);
    if (value == 0) return kHresultRpcNullRefPointer;
    const auto grouping = m.read_struct<Guid>(value);
    if (grouping.is_err()) return kHresultRpcNullRefPointer;
    const Guid context = read_event_context(m, m.arg_ptr(2));
    {
        std::lock_guard lock(m_com_mutex);
        const AudioClientState* client = audio_service_client_locked(
            m.arg_ptr(0), AudioFacet::SessionControl);
        if (client == nullptr) return E_NOINTERFACE;
        auto session = m_audio_sessions.find(client->session);
        if (session == m_audio_sessions.end()) return E_NOINTERFACE;
        // Measured (supplement): the current parameter again raises no event.
        if (!guid_equal(session->second.grouping, grouping.value())) {
            session->second.grouping = grouping.value();
            AudioSessionEvent event{};
            event.slot = kOnGroupingParamChanged;
            event.session = session->first;
            event.grouping = grouping.value();
            event.context = context;
            queue_audio_session_event_locked(std::move(event));
        }
    }
    finish_audio_change(m);
    return com::S_OK;
}

u32 ComCoreManager::audio_session_register_notification(Marshaler& m) {
    const GuestAddr sink = m.arg_ptr(1);
    if (sink == 0) return com::E_POINTER;
    {
        std::lock_guard lock(m_com_mutex);
        const AudioClientState* client = audio_service_client_locked(
            m.arg_ptr(0), AudioFacet::SessionControl);
        if (client == nullptr || !m_audio_sessions.contains(client->session)) {
            return E_NOINTERFACE;
        }
    }
    // Notifications are calls into the guest on a thread of their own. A
    // registration that could never be called back would be a false success.
    if (!m_guest_call || !m_audio_activation_thread || m_thunks == nullptr) {
        static std::once_flag once;
        log_unimplemented_once(once, "IAudioSessionControl::RegisterAudioSessionNotification: "
                                     "no guest callback thread");
        return com::E_NOTIMPL;
    }
    // Learn: success AddRefs the sink (measured: once per registration).
    guest_iunknown_add_ref(m, sink);
    bool added = false;
    {
        std::lock_guard lock(m_com_mutex);
        const AudioClientState* client = audio_service_client_locked(
            m.arg_ptr(0), AudioFacet::SessionControl);
        if (client != nullptr) {
            auto session = m_audio_sessions.find(client->session);
            if (session != m_audio_sessions.end()) {
                auto registration = std::make_shared<AudioSinkRegistration>();
                registration->sink = sink;
                session->second.sinks.push_back(std::move(registration));
                added = true;
            }
        }
    }
    if (!added) {
        guest_iunknown_release(m, sink);
        return E_NOINTERFACE;
    }
    return com::S_OK;
}

u32 ComCoreManager::audio_session_unregister_notification(Marshaler& m) {
    const GuestAddr sink = m.arg_ptr(1);
    if (sink == 0) return com::E_POINTER;
    bool release = false;
    {
        std::lock_guard lock(m_com_mutex);
        const AudioClientState* client = audio_service_client_locked(
            m.arg_ptr(0), AudioFacet::SessionControl);
        if (client == nullptr) return E_NOINTERFACE;
        auto session = m_audio_sessions.find(client->session);
        if (session == m_audio_sessions.end()) return E_NOINTERFACE;
        auto& sinks = session->second.sinks;
        auto found = std::find_if(sinks.begin(), sinks.end(),
            [sink](const auto& registration) { return registration->sink == sink; });
        // Measured: one registration per call; none left is ERROR_NOT_FOUND.
        if (found == sinks.end()) return kHresultNotFound;
        if ((*found)->pins != 0) (*found)->release_pending = true;
        else release = true;
        sinks.erase(found);
    }
    // Learn: Unregister Releases the sink.
    if (release) guest_iunknown_release(m, sink);
    return com::S_OK;
}

namespace {
/// IAudioSessionControl2 identifiers (measured layout):
///   session:  <endpoint id>|<process image NT path>%b<session GUID>
///             <endpoint id>|#%b<session GUID>          (cross-process)
///   instance: <session id>|0%b<pid>, or <session id>|0%b# (cross-process)
[[nodiscard]] std::u16string audio_session_identifier(std::u16string_view endpoint,
                                                      std::string_view image,
                                                      const Guid& guid, bool cross) {
    std::u16string id(endpoint);
    id += u"|";
    if (cross) id += u"#";
    else append_utf8(id, nt_image_path(image));
    id += u"%b";
    append_utf8(id, format_guid(guid));
    return id;
}
}  // namespace

u32 ComCoreManager::audio_session_get_session_identifier(Marshaler& m) {
    const GuestAddr out = m.arg_ptr(1);
    if (out == 0) return com::E_POINTER;
    (void)m.write_ptr(out, 0);
    Guid guid{};
    bool cross = false;
    std::u16string endpoint = AUDIO_RENDER_ENDPOINT_ID;
    {
        std::lock_guard lock(m_com_mutex);
        const AudioClientState* client = audio_service_client_locked(
            m.arg_ptr(0), AudioFacet::SessionControl);
        if (client == nullptr) return E_NOINTERFACE;
        auto session = m_audio_sessions.find(client->session);
        if (session == m_audio_sessions.end()) return E_NOINTERFACE;
        guid = session->second.guid;
        cross = session->second.cross_process;
        if (session->second.capture) {
            const std::string id = audio::capture_endpoint_id(session->second.capture_uid);
            endpoint.assign(id.begin(), id.end());
        }
    }
    const std::string_view image = m_modules != nullptr ? m_modules->main_module_path()
                                                        : std::string_view{};
    if (!cross && image.empty()) {
        static std::once_flag once;
        log_unimplemented_once(once, "IAudioSessionControl2::GetSessionIdentifier: "
                                     "no process image path");
        return com::E_NOTIMPL;
    }
    const std::u16string id = audio_session_identifier(endpoint, image, guid, cross);
    if (m_guest_task_memory == nullptr) return E_OUTOFMEMORY;
    const GuestAddr copy = m_guest_task_memory->allocate((id.size() + 1u) * 2u);
    if (copy == 0) return E_OUTOFMEMORY;
    if (!write_utf16(m, copy, id) || m.write_ptr(out, copy).is_err()) {
        m_guest_task_memory->free(copy);
        return com::E_POINTER;
    }
    return com::S_OK;
}

u32 ComCoreManager::audio_session_get_session_instance_identifier(Marshaler& m) {
    const GuestAddr out = m.arg_ptr(1);
    if (out == 0) return com::E_POINTER;
    (void)m.write_ptr(out, 0);
    Guid guid{};
    bool cross = false;
    std::u16string endpoint = AUDIO_RENDER_ENDPOINT_ID;
    {
        std::lock_guard lock(m_com_mutex);
        const AudioClientState* client = audio_service_client_locked(
            m.arg_ptr(0), AudioFacet::SessionControl);
        if (client == nullptr) return E_NOINTERFACE;
        auto session = m_audio_sessions.find(client->session);
        if (session == m_audio_sessions.end()) return E_NOINTERFACE;
        guid = session->second.guid;
        cross = session->second.cross_process;
        if (session->second.capture) {
            const std::string id = audio::capture_endpoint_id(session->second.capture_uid);
            endpoint.assign(id.begin(), id.end());
        }
    }
    const std::string_view image = m_modules != nullptr ? m_modules->main_module_path()
                                                        : std::string_view{};
    if (!cross && image.empty()) {
        static std::once_flag once;
        log_unimplemented_once(once, "IAudioSessionControl2::GetSessionInstanceIdentifier: "
                                     "no process image path");
        return com::E_NOTIMPL;
    }
    std::u16string id = audio_session_identifier(endpoint, image, guid, cross);
    id += u"|0%b";
    if (cross) id += u"#";
    else append_utf8(id, std::to_string(static_cast<u32>(::getpid())));
    if (m_guest_task_memory == nullptr) return E_OUTOFMEMORY;
    const GuestAddr copy = m_guest_task_memory->allocate((id.size() + 1u) * 2u);
    if (copy == 0) return E_OUTOFMEMORY;
    if (!write_utf16(m, copy, id) || m.write_ptr(out, copy).is_err()) {
        m_guest_task_memory->free(copy);
        return com::E_POINTER;
    }
    return com::S_OK;
}

u32 ComCoreManager::audio_session_get_process_id(Marshaler& m) {
    const GuestAddr out = m.arg_ptr(1);
    if (out == 0) return com::E_POINTER;
    bool cross = false;
    {
        std::lock_guard lock(m_com_mutex);
        const AudioClientState* client = audio_service_client_locked(
            m.arg_ptr(0), AudioFacet::SessionControl);
        if (client == nullptr) return E_NOINTERFACE;
        auto session = m_audio_sessions.find(client->session);
        if (session == m_audio_sessions.end()) return E_NOINTERFACE;
        cross = session->second.cross_process;
    }
    // GetCurrentProcessId is the host pid. Learn + measured: a cross-process
    // session answers AUDCLNT_S_NO_SINGLE_PROCESS with its creator's pid.
    if (m.write_u32(out, static_cast<u32>(::getpid())).is_err()) return com::E_POINTER;
    return cross ? AUDCLNT_S_NO_SINGLE_PROCESS : com::S_OK;
}

u32 ComCoreManager::audio_session_is_system_sounds_session(Marshaler& m) {
    std::lock_guard lock(m_com_mutex);
    if (audio_service_client_locked(m.arg_ptr(0), AudioFacet::SessionControl) == nullptr) {
        return E_NOINTERFACE;
    }
    // A stream a process opens is never the system-sounds session (measured).
    return com::S_FALSE;
}

u32 ComCoreManager::audio_session_set_ducking_preference(Marshaler& m) {
    const u32 opt_out = static_cast<u32>(m.arg(1));
    std::lock_guard lock(m_com_mutex);
    const AudioClientState* client = audio_service_client_locked(m.arg_ptr(0),
                                                                 AudioFacet::SessionControl);
    if (client == nullptr) return E_NOINTERFACE;
    auto session = m_audio_sessions.find(client->session);
    if (session == m_audio_sessions.end()) return E_NOINTERFACE;
    // The preference governs the system's communications ducking. The host
    // has no communications device that ducks other streams, so there is no
    // ducking to opt out of; the preference is recorded as given.
    session->second.ducking_opt_out = opt_out != 0;
    return com::S_OK;
}

// ---------------------------------------------------------------------------
// ISimpleAudioVolume / IChannelAudioVolume (session) — the process's volume
// on the stream's endpoint
// ---------------------------------------------------------------------------

u32 ComCoreManager::simple_audio_volume_set_master_volume(Marshaler& m) {
    const float level = m.arg_f32(1);
    const Guid context = read_event_context(m, m.arg_ptr(2));
    {
        std::lock_guard lock(m_com_mutex);
        const AudioClientState* client = audio_service_client_locked(
            m.arg_ptr(0), AudioFacet::SimpleVolume);
        if (client == nullptr) return E_NOINTERFACE;
        if (!valid_level(level)) return com::E_INVALIDARG;
        const u32 own = client->session;
        AudioEndpointVolume& volume = audio_endpoint_volume_locked(client->capture,
                                                                   client->capture_uid);
        const bool changed = volume.master != level;
        volume.master = level;
        apply_audio_gains_locked();
        // Measured: the caller's session hears every set (even of the same
        // level) with the caller's context; the process's other sessions on
        // the endpoint hear only a real change, with a context of the
        // system's own.
        const Guid propagated = fresh_guid();
        for (const auto& [id, session] : m_audio_sessions) {
            if (session.capture != client->capture || session.capture_uid != client->capture_uid) {
                continue;
            }
            if (id != own && !changed) continue;
            AudioSessionEvent event{};
            event.slot = kOnSimpleVolumeChanged;
            event.session = id;
            event.volume = level;
            event.mute = volume.mute;
            event.context = id == own ? context : propagated;
            queue_audio_session_event_locked(std::move(event));
        }
    }
    finish_audio_change(m);
    return com::S_OK;
}

u32 ComCoreManager::simple_audio_volume_get_master_volume(Marshaler& m) {
    const GuestAddr out = m.arg_ptr(1);
    if (out == 0) return kHresultRpcNullRefPointer;
    std::lock_guard lock(m_com_mutex);
    const AudioClientState* client = audio_service_client_locked(m.arg_ptr(0),
                                                                 AudioFacet::SimpleVolume);
    if (client == nullptr) return E_NOINTERFACE;
    const float level = audio_endpoint_volume_locked(client->capture, client->capture_uid).master;
    return m.write_u32(out, float_bits(level)).is_ok() ? com::S_OK : com::E_POINTER;
}

u32 ComCoreManager::simple_audio_volume_set_mute(Marshaler& m) {
    const u32 mute = static_cast<u32>(m.arg(1));
    const Guid context = read_event_context(m, m.arg_ptr(2));
    {
        std::lock_guard lock(m_com_mutex);
        const AudioClientState* client = audio_service_client_locked(
            m.arg_ptr(0), AudioFacet::SimpleVolume);
        if (client == nullptr) return E_NOINTERFACE;
        // Measured: the BOOL is kept as given (SetMute(5) reads back 5); a
        // set to the stored value raises no event; a change notifies the
        // caller's session only, with mute normalised to TRUE/FALSE.
        AudioEndpointVolume& volume = audio_endpoint_volume_locked(client->capture,
                                                                   client->capture_uid);
        if (volume.mute != mute) {
            volume.mute = mute;
            apply_audio_gains_locked();
            AudioSessionEvent event{};
            event.slot = kOnSimpleVolumeChanged;
            event.session = client->session;
            event.volume = volume.master;
            event.mute = mute;
            event.context = context;
            queue_audio_session_event_locked(std::move(event));
        }
    }
    finish_audio_change(m);
    return com::S_OK;
}

u32 ComCoreManager::simple_audio_volume_get_mute(Marshaler& m) {
    const GuestAddr out = m.arg_ptr(1);
    if (out == 0) return kHresultRpcNullRefPointer;
    std::lock_guard lock(m_com_mutex);
    const AudioClientState* client = audio_service_client_locked(m.arg_ptr(0),
                                                                 AudioFacet::SimpleVolume);
    if (client == nullptr) return E_NOINTERFACE;
    const u32 mute = audio_endpoint_volume_locked(client->capture, client->capture_uid).mute;
    return m.write_u32(out, mute).is_ok() ? com::S_OK : com::E_POINTER;
}

namespace {
/// The session's view of the process channel volumes.
[[nodiscard]] std::vector<float> session_channel_view(const std::vector<float>& volumes,
                                                      u32 channels) {
    std::vector<float> view(channels, 1.0f);
    for (u32 channel = 0; channel < channels && channel < volumes.size(); ++channel) {
        view[channel] = volumes[channel];
    }
    return view;
}
}  // namespace

u32 ComCoreManager::channel_audio_volume_get_channel_count(Marshaler& m) {
    const GuestAddr out = m.arg_ptr(1);
    if (out == 0) return kHresultRpcNullRefPointer;
    std::lock_guard lock(m_com_mutex);
    const AudioClientState* client = audio_service_client_locked(m.arg_ptr(0),
                                                                 AudioFacet::ChannelVolume);
    if (client == nullptr) return E_NOINTERFACE;
    auto session = m_audio_sessions.find(client->session);
    if (session == m_audio_sessions.end()) return E_NOINTERFACE;
    return m.write_u32(out, session->second.channels).is_ok() ? com::S_OK : com::E_POINTER;
}

u32 ComCoreManager::channel_audio_volume_set_channel_volume(Marshaler& m) {
    const u32 index = static_cast<u32>(m.arg(1));
    const float level = m.arg_f32(2);
    const Guid context = read_event_context(m, m.arg_ptr(3));
    {
        std::lock_guard lock(m_com_mutex);
        const AudioClientState* client = audio_service_client_locked(
            m.arg_ptr(0), AudioFacet::ChannelVolume);
        if (client == nullptr) return E_NOINTERFACE;
        auto own = m_audio_sessions.find(client->session);
        if (own == m_audio_sessions.end()) return E_NOINTERFACE;
        if (index >= own->second.channels || !valid_level(level)) return com::E_INVALIDARG;
        AudioEndpointVolume& volume = audio_endpoint_volume_locked(client->capture,
                                                                   client->capture_uid);
        if (volume.channels.size() <= index) volume.channels.resize(index + 1u, 1.0f);
        const bool changed = volume.channels[index] != level;
        volume.channels[index] = level;
        apply_audio_gains_locked();
        // Measured: every set notifies the caller's session with its context
        // (even an unchanged level); a change also reaches the process's
        // other sessions on the endpoint, with a context of the system's own.
        const Guid propagated = fresh_guid();
        for (const auto& [id, session] : m_audio_sessions) {
            if (session.capture != client->capture || session.capture_uid != client->capture_uid) {
                continue;
            }
            if (id != own->first && (!changed || index >= session.channels)) continue;
            AudioSessionEvent event{};
            event.slot = kOnChannelVolumeChanged;
            event.session = id;
            event.channel_volumes = session_channel_view(volume.channels, session.channels);
            event.changed_channel = index;
            event.context = id == own->first ? context : propagated;
            queue_audio_session_event_locked(std::move(event));
        }
    }
    finish_audio_change(m);
    return com::S_OK;
}

u32 ComCoreManager::channel_audio_volume_get_channel_volume(Marshaler& m) {
    const u32 index = static_cast<u32>(m.arg(1));
    const GuestAddr out = m.arg_ptr(2);
    if (out == 0) return kHresultRpcNullRefPointer;
    std::lock_guard lock(m_com_mutex);
    const AudioClientState* client = audio_service_client_locked(m.arg_ptr(0),
                                                                 AudioFacet::ChannelVolume);
    if (client == nullptr) return E_NOINTERFACE;
    auto session = m_audio_sessions.find(client->session);
    if (session == m_audio_sessions.end()) return E_NOINTERFACE;
    if (index >= session->second.channels) {
        // Measured: the out-parameter is zeroed on the range failure.
        (void)m.write_u32(out, 0u);
        return com::E_INVALIDARG;
    }
    const auto& channels = audio_endpoint_volume_locked(client->capture,
                                                        client->capture_uid).channels;
    const float level = index < channels.size() ? channels[index] : 1.0f;
    return m.write_u32(out, float_bits(level)).is_ok() ? com::S_OK : com::E_POINTER;
}

u32 ComCoreManager::channel_audio_volume_set_all_volumes(Marshaler& m) {
    const u32 count = static_cast<u32>(m.arg(1));
    const GuestAddr levels = m.arg_ptr(2);
    if (levels == 0) return kHresultRpcNullRefPointer;
    const Guid context = read_event_context(m, m.arg_ptr(3));
    {
        std::lock_guard lock(m_com_mutex);
        const AudioClientState* client = audio_service_client_locked(
            m.arg_ptr(0), AudioFacet::ChannelVolume);
        if (client == nullptr) return E_NOINTERFACE;
        auto own = m_audio_sessions.find(client->session);
        if (own == m_audio_sessions.end()) return E_NOINTERFACE;
        if (count != own->second.channels) return com::E_INVALIDARG;
        std::vector<float> values(count);
        for (u32 channel = 0; channel < count; ++channel) {
            const auto bits = m.read_u32(levels + channel * 4u);
            if (bits.is_err()) return kHresultRpcNullRefPointer;
            std::memcpy(&values[channel], &bits.value(), sizeof(float));
            // Measured: one level out of range rejects the whole call.
            if (!valid_level(values[channel])) return com::E_INVALIDARG;
        }
        AudioEndpointVolume& volume = audio_endpoint_volume_locked(client->capture,
                                                                   client->capture_uid);
        if (volume.channels.size() < count) volume.channels.resize(count, 1.0f);
        bool changed = false;
        for (u32 channel = 0; channel < count; ++channel) {
            changed = changed || volume.channels[channel] != values[channel];
            volume.channels[channel] = values[channel];
        }
        apply_audio_gains_locked();
        const Guid propagated = fresh_guid();
        for (const auto& [id, session] : m_audio_sessions) {
            if (session.capture != client->capture || session.capture_uid != client->capture_uid) {
                continue;
            }
            if (id != own->first && !changed) continue;
            AudioSessionEvent event{};
            event.slot = kOnChannelVolumeChanged;
            event.session = id;
            event.channel_volumes = session_channel_view(volume.channels, session.channels);
            // Learn: more than one channel changed is (DWORD)-1.
            event.changed_channel = kAllChannelsChanged;
            event.context = id == own->first ? context : propagated;
            queue_audio_session_event_locked(std::move(event));
        }
    }
    finish_audio_change(m);
    return com::S_OK;
}

u32 ComCoreManager::channel_audio_volume_get_all_volumes(Marshaler& m) {
    const u32 count = static_cast<u32>(m.arg(1));
    const GuestAddr out = m.arg_ptr(2);
    if (out == 0) return kHresultRpcNullRefPointer;
    std::lock_guard lock(m_com_mutex);
    const AudioClientState* client = audio_service_client_locked(m.arg_ptr(0),
                                                                 AudioFacet::ChannelVolume);
    if (client == nullptr) return E_NOINTERFACE;
    auto session = m_audio_sessions.find(client->session);
    if (session == m_audio_sessions.end()) return E_NOINTERFACE;
    if (count != session->second.channels) {
        // Measured: the caller's dwCount entries are zeroed.
        for (u32 channel = 0; channel < count; ++channel) {
            (void)m.write_u32(out + channel * 4u, 0u);
        }
        return com::E_INVALIDARG;
    }
    const auto view = session_channel_view(
        audio_endpoint_volume_locked(client->capture, client->capture_uid).channels, count);
    for (u32 channel = 0; channel < count; ++channel) {
        if (m.write_u32(out + channel * 4u, float_bits(view[channel])).is_err()) {
            return com::E_POINTER;
        }
    }
    return com::S_OK;
}

// ---------------------------------------------------------------------------
// IAudioStreamVolume — the stream's own per-channel levels (no events)
// ---------------------------------------------------------------------------

u32 ComCoreManager::stream_audio_volume_get_channel_count(Marshaler& m) {
    const GuestAddr out = m.arg_ptr(1);
    if (out == 0) return com::E_POINTER;
    std::lock_guard lock(m_com_mutex);
    const AudioClientState* client = audio_service_client_locked(m.arg_ptr(0),
                                                                 AudioFacet::StreamVolume);
    if (client == nullptr) return E_NOINTERFACE;
    // Measured: the stream format's channels (2 for a 16-bit stereo stream).
    return m.write_u32(out, client->format.channels).is_ok() ? com::S_OK : com::E_POINTER;
}

u32 ComCoreManager::stream_audio_volume_set_channel_volume(Marshaler& m) {
    const u32 index = static_cast<u32>(m.arg(1));
    const float level = m.arg_f32(2);
    std::lock_guard lock(m_com_mutex);
    const AudioServiceState* service = find_audio_service_locked(m.arg_ptr(0),
                                                                 AudioFacet::StreamVolume);
    if (service == nullptr) return E_NOINTERFACE;
    auto client = m_audio_clients.find(service->client);
    if (client == m_audio_clients.end()) return E_NOINTERFACE;
    auto& volumes = client->second.stream_volumes;
    if (index >= volumes.size() || !valid_level(level)) return com::E_INVALIDARG;
    volumes[index] = level;
    apply_audio_gains_locked(client->first);
    return com::S_OK;
}

u32 ComCoreManager::stream_audio_volume_get_channel_volume(Marshaler& m) {
    const u32 index = static_cast<u32>(m.arg(1));
    const GuestAddr out = m.arg_ptr(2);
    if (out == 0) return com::E_POINTER;
    std::lock_guard lock(m_com_mutex);
    const AudioClientState* client = audio_service_client_locked(m.arg_ptr(0),
                                                                 AudioFacet::StreamVolume);
    if (client == nullptr) return E_NOINTERFACE;
    if (index >= client->stream_volumes.size()) {
        (void)m.write_u32(out, 0u);
        return com::E_INVALIDARG;
    }
    return m.write_u32(out, float_bits(client->stream_volumes[index])).is_ok()
        ? com::S_OK : com::E_POINTER;
}

u32 ComCoreManager::stream_audio_volume_set_all_volumes(Marshaler& m) {
    const u32 count = static_cast<u32>(m.arg(1));
    const GuestAddr levels = m.arg_ptr(2);
    if (levels == 0) return com::E_POINTER;
    std::lock_guard lock(m_com_mutex);
    AudioServiceState* service = find_audio_service_locked(m.arg_ptr(0),
                                                           AudioFacet::StreamVolume);
    if (service == nullptr) return E_NOINTERFACE;
    auto client = m_audio_clients.find(service->client);
    if (client == m_audio_clients.end()) return E_NOINTERFACE;
    auto& volumes = client->second.stream_volumes;
    if (count != volumes.size()) return com::E_INVALIDARG;
    std::vector<float> values(count);
    for (u32 channel = 0; channel < count; ++channel) {
        const auto bits = m.read_u32(levels + channel * 4u);
        if (bits.is_err()) return com::E_POINTER;
        std::memcpy(&values[channel], &bits.value(), sizeof(float));
        if (!valid_level(values[channel])) return com::E_INVALIDARG;
    }
    volumes = std::move(values);
    apply_audio_gains_locked(client->first);
    return com::S_OK;
}

u32 ComCoreManager::stream_audio_volume_get_all_volumes(Marshaler& m) {
    const u32 count = static_cast<u32>(m.arg(1));
    const GuestAddr out = m.arg_ptr(2);
    if (out == 0) return com::E_POINTER;
    std::lock_guard lock(m_com_mutex);
    const AudioClientState* client = audio_service_client_locked(m.arg_ptr(0),
                                                                 AudioFacet::StreamVolume);
    if (client == nullptr) return E_NOINTERFACE;
    if (count != client->stream_volumes.size()) {
        for (u32 channel = 0; channel < count; ++channel) {
            (void)m.write_u32(out + channel * 4u, 0u);
        }
        return com::E_INVALIDARG;
    }
    for (u32 channel = 0; channel < count; ++channel) {
        if (m.write_u32(out + channel * 4u,
                        float_bits(client->stream_volumes[channel])).is_err()) {
            return com::E_POINTER;
        }
    }
    return com::S_OK;
}

// ---------------------------------------------------------------------------
// Exports
// ---------------------------------------------------------------------------

void append_wasapi_service_exports(DllExports& exports, ComCoreManager& mgr) {
    struct Method {
        const char* name;
        u32 (ComCoreManager::*fn)(Marshaler&);
        u16 stdcall_bytes;
    };
    static constexpr Method kMethods[] = {
        {"Vitreo_AudioService_QueryInterface", &ComCoreManager::audio_service_query_interface, 12},
        {"Vitreo_AudioService_AddRef", &ComCoreManager::audio_service_add_ref, 4},
        {"Vitreo_AudioService_Release", &ComCoreManager::audio_service_release, 4},
        {"Vitreo_AudioClock_GetFrequency", &ComCoreManager::audio_clock_get_frequency, 8},
        {"Vitreo_AudioClock_GetPosition", &ComCoreManager::audio_clock_get_position, 12},
        {"Vitreo_AudioClock_GetCharacteristics",
         &ComCoreManager::audio_clock_get_characteristics, 8},
        {"Vitreo_AudioClock2_GetDevicePosition",
         &ComCoreManager::audio_clock2_get_device_position, 12},
        {"Vitreo_AudioClockAdjustment_SetSampleRate",
         &ComCoreManager::audio_clock_adjustment_set_sample_rate, 8},
        {"Vitreo_AudioSession_GetState", &ComCoreManager::audio_session_get_state, 8},
        {"Vitreo_AudioSession_GetDisplayName", &ComCoreManager::audio_session_get_display_name, 8},
        {"Vitreo_AudioSession_SetDisplayName", &ComCoreManager::audio_session_set_display_name, 12},
        {"Vitreo_AudioSession_GetIconPath", &ComCoreManager::audio_session_get_icon_path, 8},
        {"Vitreo_AudioSession_SetIconPath", &ComCoreManager::audio_session_set_icon_path, 12},
        {"Vitreo_AudioSession_GetGroupingParam",
         &ComCoreManager::audio_session_get_grouping_param, 8},
        {"Vitreo_AudioSession_SetGroupingParam",
         &ComCoreManager::audio_session_set_grouping_param, 12},
        {"Vitreo_AudioSession_RegisterAudioSessionNotification",
         &ComCoreManager::audio_session_register_notification, 8},
        {"Vitreo_AudioSession_UnregisterAudioSessionNotification",
         &ComCoreManager::audio_session_unregister_notification, 8},
        {"Vitreo_AudioSession_GetSessionIdentifier",
         &ComCoreManager::audio_session_get_session_identifier, 8},
        {"Vitreo_AudioSession_GetSessionInstanceIdentifier",
         &ComCoreManager::audio_session_get_session_instance_identifier, 8},
        {"Vitreo_AudioSession_GetProcessId", &ComCoreManager::audio_session_get_process_id, 8},
        {"Vitreo_AudioSession_IsSystemSoundsSession",
         &ComCoreManager::audio_session_is_system_sounds_session, 4},
        {"Vitreo_AudioSession_SetDuckingPreference",
         &ComCoreManager::audio_session_set_ducking_preference, 8},
        {"Vitreo_SimpleAudioVolume_SetMasterVolume",
         &ComCoreManager::simple_audio_volume_set_master_volume, 12},
        {"Vitreo_SimpleAudioVolume_GetMasterVolume",
         &ComCoreManager::simple_audio_volume_get_master_volume, 8},
        {"Vitreo_SimpleAudioVolume_SetMute", &ComCoreManager::simple_audio_volume_set_mute, 12},
        {"Vitreo_SimpleAudioVolume_GetMute", &ComCoreManager::simple_audio_volume_get_mute, 8},
        {"Vitreo_ChannelAudioVolume_GetChannelCount",
         &ComCoreManager::channel_audio_volume_get_channel_count, 8},
        {"Vitreo_ChannelAudioVolume_SetChannelVolume",
         &ComCoreManager::channel_audio_volume_set_channel_volume, 16},
        {"Vitreo_ChannelAudioVolume_GetChannelVolume",
         &ComCoreManager::channel_audio_volume_get_channel_volume, 12},
        {"Vitreo_ChannelAudioVolume_SetAllVolumes",
         &ComCoreManager::channel_audio_volume_set_all_volumes, 16},
        {"Vitreo_ChannelAudioVolume_GetAllVolumes",
         &ComCoreManager::channel_audio_volume_get_all_volumes, 12},
        {"Vitreo_AudioStreamVolume_GetChannelCount",
         &ComCoreManager::stream_audio_volume_get_channel_count, 8},
        {"Vitreo_AudioStreamVolume_SetChannelVolume",
         &ComCoreManager::stream_audio_volume_set_channel_volume, 12},
        {"Vitreo_AudioStreamVolume_GetChannelVolume",
         &ComCoreManager::stream_audio_volume_get_channel_volume, 12},
        {"Vitreo_AudioStreamVolume_SetAllVolumes",
         &ComCoreManager::stream_audio_volume_set_all_volumes, 12},
        {"Vitreo_AudioStreamVolume_GetAllVolumes",
         &ComCoreManager::stream_audio_volume_get_all_volumes, 12},
        {"Vitreo_AudioSession_EventWorker", &ComCoreManager::audio_session_event_worker, 4},
    };
    for (const Method& method : kMethods) {
        exports.entries.push_back(ExportEntry{
            method.name, 0,
            [&mgr, fn = method.fn](Marshaler& m) { m.set_return((mgr.*fn)(m)); },
            method.stdcall_bytes});
    }
}

}  // namespace vitreo::winapi
