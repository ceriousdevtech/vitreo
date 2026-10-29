/// @file dsound_capture.cpp
/// @brief IDirectSoundCapture(8) device object and CreateCaptureBuffer.

#include "winapi/media/dsound/dsound_internal.hpp"

namespace vitreo::winapi {
namespace {

constexpr u16 WAVE_FORMAT_IEEE_FLOAT = 3;
constexpr u16 WAVE_FORMAT_EXTENSIBLE = 0xFFFE;
/// WAVEFORMATEXTENSIBLE: cbSize 22, sizeof 40.
constexpr u16 WAVEFORMATEXTENSIBLE_EXTRA = 22;

/// dsound.h DSCCAPS / DSCBCAPS: four DWORDs each.
constexpr u32 DSCCAPS_BYTES = 16;
/// DSCEFFECTDESC: dwSize, dwFlags, two GUIDs, two reserved DWORDs.
constexpr u32 DSCEFFECTDESC_BYTES = 48;

/// DSCBUFFERDESC1 / DSCBUFFERDESC sizes: four DWORDs, the format pointer and,
/// for DSCBUFFERDESC, dwFXCount and the effect array pointer (pointer-aligned).
constexpr u32 dscbufferdesc1_bytes(bool x64) { return x64 ? 24u : 20u; }
constexpr u32 dscbufferdesc_bytes(bool x64) { return x64 ? 40u : 28u; }

} // namespace

// ---------------------------------------------------------------------------
// IDirectSoundCapture8
// ---------------------------------------------------------------------------

u32 DirectSoundManager::create_capture_object(bool initialized, bool version8,
                                              const std::string& uid, GuestAddr& object) {
    object = new_object(Vtable::Capture, 2);
    if (object == 0) return dsound::E_OUTOFMEMORY;
    const GuestAddr unknown = object + pointer_size();
    if (!write_ptr(unknown, m_vtables[static_cast<usize>(Vtable::CaptureUnknown)])) {
        free_object(object, 2 * pointer_size());
        return dsound::E_OUTOFMEMORY;
    }
    CaptureObject state;
    state.initialized = initialized;
    state.version8 = version8;
    state.device_uid = uid;
    state.unknown = unknown;
    m_captures.emplace(object, std::move(state));
    m_facets[unknown] = object;
    return dsound::DS_OK;
}

u32 DirectSoundManager::capture_create(GuestAddr guid, GuestAddr output, GuestAddr outer,
                                       bool version8) {
    // Measured: a NULL ppDSC is E_INVALIDARG; aggregation and an unknown
    // device leave *ppDSC untouched.
    if (!m_memory || output == 0) return dsound::E_INVALIDARG;
    if (outer != 0) return dsound::CLASS_E_NOAGGREGATION;
    std::string uid;
    if (!resolve_capture_device(guid, uid)) return dsound::DSERR_NODRIVER;
    std::lock_guard lock(m_mutex);
    GuestAddr object = 0;
    const u32 hr = create_capture_object(true, version8, uid, object);
    if (hr != dsound::DS_OK) return hr;
    if (!write_ptr(output, object)) {
        std::vector<std::shared_ptr<audio::IAudioCaptureStream>> dead;
        destroy_capture_locked(object, dead);
        return dsound::E_INVALIDARG;
    }
    VITREO_TRACE("[dsound] DirectSoundCaptureCreate%s -> 0x%llx\n", version8 ? "8" : "",
                 static_cast<unsigned long long>(object));
    return dsound::DS_OK;
}

u32 DirectSoundManager::capture_query_interface(GuestAddr object, GuestAddr iid,
                                                GuestAddr output) {
    std::lock_guard lock(m_mutex);
    if (!m_memory || output == 0) return dsound::E_POINTER;
    (void)write_ptr(output, 0);
    GuestAddr primary = 0;
    CaptureObject* state = find_capture(object, &primary);
    if (!state) return dsound::E_NOINTERFACE;
    std::array<u8, 16> requested{};
    if (!read_guid(iid, requested)) return dsound::E_INVALIDARG;
    GuestAddr answer = 0;
    if (guid_equal(requested, IID_IUNKNOWN)) answer = state->unknown;
    else if (guid_equal(requested, IID_IDIRECTSOUNDCAPTURE)) answer = primary;
    if (answer == 0) return dsound::E_NOINTERFACE;
    (void)write_ptr(output, answer);
    ++state->refs;
    return dsound::DS_OK;
}

u32 DirectSoundManager::capture_add_ref(GuestAddr object) {
    std::lock_guard lock(m_mutex);
    CaptureObject* state = find_capture(object);
    return state ? ++state->refs : 0;
}

void DirectSoundManager::destroy_capture_locked(
    GuestAddr object, std::vector<std::shared_ptr<audio::IAudioCaptureStream>>& dead) {
    auto it = m_captures.find(object);
    if (it == m_captures.end()) return;
    // Measured: releasing the capture object destroys its buffer too (the
    // buffer holds no reference on it).
    if (it->second.buffer != 0) destroy_cbuffer_locked(it->second.buffer, dead);
    m_facets.erase(it->second.unknown);
    m_captures.erase(it);
    free_object(object, 2 * pointer_size());
}

u32 DirectSoundManager::capture_release(GuestAddr object) {
    std::vector<std::shared_ptr<audio::IAudioCaptureStream>> dead;
    u32 refs = 0;
    {
        std::lock_guard lock(m_mutex);
        GuestAddr primary = 0;
        CaptureObject* state = find_capture(object, &primary);
        if (!state) return 0;
        refs = --state->refs;
        if (refs == 0) destroy_capture_locked(primary, dead);
    }
    for (auto& stream : dead) stream->stop();
    return refs;
}

u32 DirectSoundManager::capture_get_caps(GuestAddr object, GuestAddr caps) {
    std::string uid;
    {
        std::lock_guard lock(m_mutex);
        CaptureObject* state = find_capture(object);
        if (!state) return dsound::E_INVALIDARG;
        if (!m_memory || caps == 0) return dsound::E_INVALIDARG;
        if (m_memory->read_u32(caps).value_or(0) != DSCCAPS_BYTES) return dsound::E_INVALIDARG;
        if (!state->initialized) return dsound::DSERR_UNINITIALIZED;
        uid = state->device_uid;
    }
    u16 channels = 0;
    if (m_capture) {
        for (const auto& device : m_capture->devices()) {
            if (device.uid == uid) channels = device.channels;
        }
    }
    if (channels == 0) return dsound::DSERR_NODRIVER;
    (void)m_memory->write_u32(caps + 4, dsound::DSCCAPS_MULTIPLECAPTURE);
    (void)m_memory->write_u32(caps + 8, dsound::kCaptureCapsFormats);
    (void)m_memory->write_u32(caps + 12, channels);
    return dsound::DS_OK;
}

u32 DirectSoundManager::capture_initialize(GuestAddr object, GuestAddr guid) {
    std::string uid;
    {
        std::lock_guard lock(m_mutex);
        CaptureObject* state = find_capture(object);
        if (!state) return dsound::E_INVALIDARG;
        if (state->initialized) return dsound::DSERR_ALREADYINITIALIZED;
    }
    if (!resolve_capture_device(guid, uid)) return dsound::DSERR_NODRIVER;
    std::lock_guard lock(m_mutex);
    CaptureObject* state = find_capture(object);
    if (!state) return dsound::E_INVALIDARG;
    if (state->initialized) return dsound::DSERR_ALREADYINITIALIZED;
    state->initialized = true;
    state->device_uid = uid;
    return dsound::DS_OK;
}

u32 DirectSoundManager::create_capture_buffer_locked(
    GuestAddr capture_ptr, GuestAddr descriptor, GuestAddr& buffer,
    std::shared_ptr<audio::IAudioCaptureStream>& opened) {
    GuestAddr capture_primary = 0;
    CaptureObject* owner = find_capture(capture_ptr, &capture_primary);
    if (!owner) return dsound::E_INVALIDARG;
    if (!owner->initialized) return dsound::DSERR_UNINITIALIZED;

    // DSCBUFFERDESC(1): dwSize, dwFlags, dwBufferBytes, dwReserved,
    // lpwfxFormat, then (DSCBUFFERDESC only) dwFXCount and lpDSCFXDesc.
    const u32 size = m_memory->read_u32(descriptor).value_or(0);
    const bool full = size == dscbufferdesc_bytes(m_x64);
    if (!full && size != dscbufferdesc1_bytes(m_x64)) return dsound::E_INVALIDARG;
    const u32 flags = m_memory->read_u32(descriptor + 4).value_or(0);
    const u32 bytes = m_memory->read_u32(descriptor + 8).value_or(0);
    const u32 reserved = m_memory->read_u32(descriptor + 12).value_or(0);
    const u32 ptr = pointer_size();
    const auto read_ptr = [&](GuestAddr at) -> GuestAddr {
        return m_x64 ? static_cast<GuestAddr>(m_memory->read_u64(at).value_or(0))
                     : m_memory->read_u32(at).value_or(0);
    };
    const GuestAddr format_ptr = read_ptr(descriptor + 16);
    const u32 fx_count = full ? m_memory->read_u32(descriptor + 16 + ptr).value_or(0) : 0;
    const GuestAddr fx_ptr = full ? read_ptr(descriptor + 16 + 2 * ptr) : 0;
    if (reserved != 0) return dsound::E_INVALIDARG;
    if ((flags & ~(dsound::DSCBCAPS_WAVEMAPPED | dsound::DSCBCAPS_CTRLFX)) != 0) {
        return dsound::E_INVALIDARG;
    }
    if (format_ptr == 0) return dsound::E_INVALIDARG;
    if (bytes < dsound::DSBSIZE_MIN || bytes > dsound::DSBSIZE_MAX) return dsound::E_INVALIDARG;
    // Effects (measured): an effect needs DSCBCAPS_CTRLFX, the array must be
    // present exactly when the count is non-zero, and each DSCEFFECTDESC
    // must be well formed.
    if (fx_count != 0 && (flags & dsound::DSCBCAPS_CTRLFX) == 0) return dsound::E_INVALIDARG;
    if ((fx_count == 0) != (fx_ptr == 0)) return dsound::E_INVALIDARG;
    for (u32 i = 0; i < fx_count; ++i) {
        const GuestAddr fx = fx_ptr + static_cast<GuestAddr>(i) * DSCEFFECTDESC_BYTES;
        const u32 fx_size = m_memory->read_u32(fx).value_or(0);
        const u32 fx_flags = m_memory->read_u32(fx + 4).value_or(0);
        std::array<u8, 16> cls{}, instance{};
        if (fx_size != DSCEFFECTDESC_BYTES || !read_guid(fx + 8, cls) ||
            !read_guid(fx + 24, instance) ||
            m_memory->read_u32(fx + 40).value_or(1) != 0 ||
            m_memory->read_u32(fx + 44).value_or(1) != 0) {
            return dsound::E_INVALIDARG;
        }
        if (fx_flags != dsound::DSCFX_LOCHARDWARE && fx_flags != dsound::DSCFX_LOCSOFTWARE) {
            return dsound::E_INVALIDARG;
        }
        const bool known_class = guid_equal(cls, GUID_DSCFX_CLASS_AEC) ||
                                 guid_equal(cls, GUID_DSCFX_CLASS_NS);
        const bool known_instance = guid_equal(instance, GUID_DSCFX_MS_AEC) ||
                                    guid_equal(instance, GUID_DSCFX_SYSTEM_AEC) ||
                                    guid_equal(instance, GUID_DSCFX_MS_NS) ||
                                    guid_equal(instance, GUID_DSCFX_SYSTEM_NS);
        if (!known_class || !known_instance) return dsound::E_INVALIDARG;
    }

    // The format: WAVEFORMATEX, or WAVEFORMATEXTENSIBLE with cbSize >= 22.
    const u16 tag = static_cast<u16>(m_memory->read_u16(format_ptr).value_or(0));
    const u16 channels = static_cast<u16>(m_memory->read_u16(format_ptr + 2).value_or(0));
    const u32 rate = m_memory->read_u32(format_ptr + 4).value_or(0);
    const u16 align = static_cast<u16>(m_memory->read_u16(format_ptr + 12).value_or(0));
    const u16 bits = static_cast<u16>(m_memory->read_u16(format_ptr + 14).value_or(0));
    if (channels == 0 || rate == 0 || bits == 0 || bits % 8u != 0 ||
        align != channels * (bits / 8u)) {
        return dsound::E_INVALIDARG;
    }
    std::vector<u8> format(WAVEFORMATEX_BYTES, 0);
    bool is_float = tag == WAVE_FORMAT_IEEE_FLOAT;
    bool supported = tag == WAVE_FORMAT_PCM || tag == WAVE_FORMAT_IEEE_FLOAT;
    if (tag == WAVE_FORMAT_EXTENSIBLE) {
        const u16 extra = static_cast<u16>(m_memory->read_u16(format_ptr + 16).value_or(0));
        if (extra < WAVEFORMATEXTENSIBLE_EXTRA) return dsound::E_INVALIDARG;
        const u16 valid = static_cast<u16>(m_memory->read_u16(format_ptr + 18).value_or(0));
        if (valid > bits) return dsound::E_INVALIDARG;
        format.assign(WAVEFORMATEX_BYTES + WAVEFORMATEXTENSIBLE_EXTRA, 0);
        (void)m_memory->read_bytes(format_ptr, reinterpret_cast<Byte*>(format.data()),
                                   format.size());
        format[16] = WAVEFORMATEXTENSIBLE_EXTRA;
        format[17] = 0;
        Guid subformat{};
        std::memcpy(&subformat, format.data() + 24, sizeof(subformat));
        // KSDATAFORMAT_SUBTYPE_PCM / _IEEE_FLOAT share the tail of
        // {0000000x-0000-0010-8000-00aa00389b71}.
        constexpr u8 tail[8] = {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71};
        const bool ks = subformat.data2 == 0 && subformat.data3 == 0x0010 &&
                        std::memcmp(subformat.data4, tail, 8) == 0;
        is_float = ks && subformat.data1 == WAVE_FORMAT_IEEE_FLOAT;
        // Measured: a PCM container whose valid bits are fewer than its size
        // is DSERR_BADFORMAT (24/20), the full container is accepted.
        supported = ks && (subformat.data1 == WAVE_FORMAT_PCM ? (valid == 0 || valid == bits)
                                                              : is_float);
    } else {
        (void)m_memory->read_bytes(format_ptr, reinterpret_cast<Byte*>(format.data()),
                                   WAVEFORMAT_FIELD_BYTES);
    }
    if (bytes % align != 0) return dsound::E_INVALIDARG;

    // Past argument validation, every failure answers *ppDSCBuffer = NULL.
    if (!supported || (is_float && bits != 32)) return dsound::DSERR_BADFORMAT;
    if (owner->buffer != 0) return dsound::DSERR_ALLOCATED;
    if (tag != WAVE_FORMAT_EXTENSIBLE && channels > 2) return dsound::DSERR_NODRIVER;
    if ((flags & dsound::DSCBCAPS_CTRLFX) != 0 && !owner->version8) {
        return dsound::DSERR_DS8_REQUIRED;
    }
    const audio::PcmFormat pcm{rate, channels, bits, is_float, 0};
    if (!pcm.valid_stream() || !m_capture) return dsound::DSERR_NODRIVER;
    if (fx_count != 0) {
        // Acoustic echo cancellation and noise suppression are processing
        // the host input does not perform; accepting them would capture
        // unprocessed audio under a success. dsound.h: "The effects
        // requested could not be found on the system".
        static std::once_flag once;
        unimplemented_once(once, "dsound!IDirectSoundCapture8::CreateCaptureBuffer: capture "
                                 "effects (DSCFX AEC / noise suppression)");
        return dsound::DSERR_FXUNAVAILABLE;
    }
    {
        // The host stream delivers the device's channels; a buffer of another
        // channel count needs a conversion the host can perform exactly.
        u16 device_channels = 0;
        for (const auto& device : m_capture->devices()) {
            if (device.uid == owner->device_uid) device_channels = device.channels;
        }
        if (device_channels == 0) return dsound::DSERR_NODRIVER;
        if (!audio::capture_channels_convertible(device_channels, channels)) {
            static std::once_flag once;
            unimplemented_once(once, "dsound!IDirectSoundCapture8::CreateCaptureBuffer: channel "
                                     "conversion other than mono<->stereo or none");
            return dsound::DSERR_BADFORMAT;
        }
    }
    if (!audio::capture_permitted(*m_capture, "IDirectSoundCapture8::CreateCaptureBuffer")) {
        return dsound::DSERR_ACCESSDENIED;
    }
    if (!ensure_storage()) return dsound::E_OUTOFMEMORY;

    const u32 mapped = (bytes + 0xFFFu) & ~0xFFFu;
    const GuestAddr samples = m_memory->find_free_gap(0x64000000, mapped);
    if (samples == 0 ||
        m_memory->map_region(samples, mapped, MemoryProtection::ReadWrite).is_err()) {
        return dsound::E_OUTOFMEMORY;
    }
    buffer = new_object(Vtable::CaptureBuffer, 3);
    if (buffer == 0) {
        (void)m_memory->unmap_region(samples, mapped);
        return dsound::E_OUTOFMEMORY;
    }
    const GuestAddr notify = buffer + ptr;
    const GuestAddr unknown = buffer + 2 * ptr;
    (void)write_ptr(notify, m_vtables[static_cast<usize>(Vtable::Notify)]);
    (void)write_ptr(unknown, m_vtables[static_cast<usize>(Vtable::CaptureBufferUnknown)]);

    // The host stream converts to the buffer format; half a second of slack
    // between the device and the guest ring.
    const GuestAddr key = buffer;
    auto stream = std::shared_ptr<audio::IAudioCaptureStream>(m_capture->open(
        owner->device_uid, pcm, std::max<u32>(rate / 2u, 1024u),
        [this, key] { capture_pump(key); }));
    if (!stream) {
        free_object(buffer, 3 * ptr);
        (void)m_memory->unmap_region(samples, mapped);
        buffer = 0;
        return dsound::DSERR_NODRIVER;
    }
    CaptureBufferObject state;
    state.owner = capture_primary;
    state.version8 = owner->version8;
    state.flags = flags;
    state.bytes = bytes;
    state.samples = samples;
    state.mapped_bytes = mapped;
    state.format = std::move(format);
    state.block_align = align;
    state.fx_count = fx_count;
    state.notify = notify;
    state.unknown = unknown;
    state.stream = stream;
    m_cbuffers.emplace(buffer, std::move(state));
    m_facets[notify] = buffer;
    m_facets[unknown] = buffer;
    owner->buffer = buffer;
    opened = std::move(stream);
    return dsound::DS_OK;
}

u32 DirectSoundManager::capture_create_buffer(GuestAddr object, GuestAddr descriptor,
                                              GuestAddr output, GuestAddr outer) {
    // Measured order: a NULL output, then a NULL descriptor, then
    // aggregation, all with the output untouched.
    if (!m_memory || output == 0 || descriptor == 0) return dsound::E_INVALIDARG;
    if (outer != 0) return dsound::CLASS_E_NOAGGREGATION;
    std::shared_ptr<audio::IAudioCaptureStream> opened;
    std::unique_lock lock(m_mutex);
    GuestAddr buffer = 0;
    const u32 hr = create_capture_buffer_locked(object, descriptor, buffer, opened);
    if (hr == dsound::DS_OK) {
        (void)write_ptr(output, buffer);
        VITREO_TRACE("[dsound] CreateCaptureBuffer -> 0x%llx\n",
                     static_cast<unsigned long long>(buffer));
        return hr;
    }
    // Argument errors leave the output untouched; the rest store NULL.
    if (hr != dsound::E_INVALIDARG && hr != dsound::DSERR_UNINITIALIZED) {
        (void)write_ptr(output, 0);
    }
    return hr;
}

} // namespace vitreo::winapi
