/// @file dsound_buffer.cpp
/// @brief IDirectSoundBuffer(8) creation, identity and lifetime.

#include "winapi/media/dsound/dsound_internal.hpp"

namespace vitreo::winapi {
namespace {

/// Pre-DX7 DSBUFFERDESC (no guid3DAlgorithm).
constexpr u32 DSBUFFERDESC_BYTES = 20;
/// DX7+ / DSBUFFERDESC with trailing GUID (DirectX SDK sizeof == 36).
constexpr u32 DSBUFFERDESC_DX7_BYTES = 36;

} // namespace

// ---------------------------------------------------------------------------
// Playback (IDirectSound / IDirectSoundBuffer)
// ---------------------------------------------------------------------------

u32 DirectSoundManager::ensure_listener(BufferObject& buffer, GuestAddr buffer_addr,
                                        GuestAddr output) {
    if (!m_memory || output == 0) return dsound::E_INVALIDARG;
    if ((buffer.flags & dsound::DSBCAPS_CTRL3D) == 0) {
        (void)write_ptr(output, 0);
        return dsound::E_NOINTERFACE;
    }
    if (buffer.listener != 0) {
        (void)write_ptr(output, buffer.listener);
        (void)listener_add_ref(buffer.listener);
        return dsound::DS_OK;
    }
    const GuestAddr listener = new_object(Vtable::Listener);
    if (listener == 0 || !write_ptr(output, listener)) return dsound::E_OUTOFMEMORY;
    ListenerObject state;
    state.buffer = buffer_addr;
    m_listeners.emplace(listener, state);
    buffer.listener = listener;
    VITREO_TRACE("[dsound] IDirectSound3DListener created on buffer=0x%llx -> "
                 "listener=0x%llx\n",
                 static_cast<unsigned long long>(buffer_addr),
                 static_cast<unsigned long long>(listener));
    return dsound::DS_OK;
}

bool DirectSoundManager::read_wave_format(GuestAddr address, WaveFormat& out) const {
    if (!m_memory || address == 0) return false;
    out.format_tag = static_cast<u16>(m_memory->read_u16(address).value_or(0));
    out.channels = static_cast<u16>(m_memory->read_u16(address + 2).value_or(0));
    out.samples_per_second = m_memory->read_u32(address + 4).value_or(0);
    out.average_bytes_per_second = m_memory->read_u32(address + 8).value_or(0);
    out.block_align = static_cast<u16>(m_memory->read_u16(address + 12).value_or(0));
    out.bits_per_sample = static_cast<u16>(m_memory->read_u16(address + 14).value_or(0));
    return true;
}

void DirectSoundManager::write_wave_format(GuestAddr address,
                                           const WaveFormat& format) const {
    if (!m_memory || address == 0) return;
    (void)m_memory->write_u16(address, format.format_tag);
    (void)m_memory->write_u16(address + 2, format.channels);
    (void)m_memory->write_u32(address + 4, format.samples_per_second);
    (void)m_memory->write_u32(address + 8, format.average_bytes_per_second);
    (void)m_memory->write_u16(address + 12, format.block_align);
    (void)m_memory->write_u16(address + 14, format.bits_per_sample);
}

bool DirectSoundManager::create_voice(BufferObject& state) {
    if (!m_renderer || !m_memory || state.primary || state.samples == 0) return false;
    const Byte* ring = m_memory->host_ptr(state.samples, state.bytes);
    if (!ring) return false;
    const audio::PcmFormat format{state.format.samples_per_second,
                                  state.format.channels,
                                  state.format.bits_per_sample};
    state.voice = m_renderer->create_voice(
        format, reinterpret_cast<const u8*>(ring), state.bytes);
    if (!state.voice) {
        VITREO_TRACE("[dsound] voice unavailable %uHz %uch %ubit bytes=%u\n",
                     format.sample_rate, format.channels, format.bits_per_sample,
                     state.bytes);
        return false;
    }
    state.voice->set_volume(state.volume);
    state.voice->set_pan(state.pan);
    return true;
}

u32 DirectSoundManager::create_sound_buffer(GuestAddr object, GuestAddr descriptor,
                                            GuestAddr output, GuestAddr outer) {
    std::lock_guard lock(m_mutex);
    if (!m_memory || output == 0) return dsound::E_INVALIDARG;
    (void)write_ptr(output, 0);
    Object* owner = find_object(object);
    if (!owner) return dsound::DSERR_UNINITIALIZED;
    if (outer != 0) return dsound::CLASS_E_NOAGGREGATION;
    if (descriptor == 0) return dsound::E_INVALIDARG;

    const u32 desc_size = m_memory->read_u32(descriptor).value_or(0);
    if (desc_size != DSBUFFERDESC_BYTES && desc_size != DSBUFFERDESC_DX7_BYTES) {
        VITREO_TRACE("[dsound] CreateSoundBuffer rejected dwSize=%u\n", desc_size);
        return dsound::E_INVALIDARG;
    }
    const u32 flags = m_memory->read_u32(descriptor + 4).value_or(0);
    const u32 bytes = m_memory->read_u32(descriptor + 8).value_or(0);
    // DSBUFFERDESC: dwSize, dwFlags, dwBufferBytes, dwReserved, then the
    // pointer-aligned lpwfxFormat (+16 on both: 16 is 8-aligned).
    const GuestAddr format_ptr = m_x64
        ? static_cast<GuestAddr>(m_memory->read_u64(descriptor + 16).value_or(0))
        : m_memory->read_u32(descriptor + 16).value_or(0);
    const bool primary = (flags & dsound::DSBCAPS_PRIMARYBUFFER) != 0;

    BufferObject state;
    state.flags = flags;
    state.primary = primary;
    state.version8 = owner->version8;

    if (primary) {
        // A primary buffer describes the mix destination: no size, no format.
        if (bytes != 0 || format_ptr != 0) return dsound::E_INVALIDARG;
        state.format = WaveFormat{WAVE_FORMAT_PCM, 2, 22050, 22050 * 4, 4, 16};
        state.bytes = 0;
    } else {
        if (bytes < dsound::DSBSIZE_MIN || format_ptr == 0) return dsound::E_INVALIDARG;
        if (!read_wave_format(format_ptr, state.format)) return dsound::E_INVALIDARG;
        if (state.format.format_tag != WAVE_FORMAT_PCM ||
            (state.format.channels != 1 && state.format.channels != 2) ||
            (state.format.bits_per_sample != 8 && state.format.bits_per_sample != 16) ||
            state.format.samples_per_second == 0) {
            return dsound::DSERR_BADFORMAT;
        }
        state.bytes = bytes;
    }
    state.frequency = state.format.samples_per_second;
    if (!ensure_storage()) return dsound::E_OUTOFMEMORY;

    // Secondary buffers get guest-visible sample memory. Lock hands the guest
    // pointers straight into it, so writes land where the device reads.
    if (!primary) {
        const u32 mapped = (state.bytes + 0xFFFu) & ~0xFFFu;
        const GuestAddr samples = m_memory->find_free_gap(0x64000000, mapped);
        if (samples == 0 || m_memory->map_region(
                samples, mapped, MemoryProtection::ReadWrite).is_err()) {
            return dsound::E_OUTOFMEMORY;
        }
        std::vector<Byte> silence(state.bytes,
                                  static_cast<Byte>(state.format.bits_per_sample == 8 ? 0x80 : 0));
        (void)m_memory->write_bytes(samples, silence.data(), silence.size());
        state.samples = samples;
        state.mapped_bytes = mapped;
        state.owns_samples = true;
    }

    const GuestAddr buffer = new_object(Vtable::Buffer);
    if (buffer == 0 || !write_ptr(output, buffer)) return dsound::E_OUTOFMEMORY;

    auto [it, inserted] = m_buffers.emplace(buffer, std::move(state));
    if (!inserted) return dsound::E_OUTOFMEMORY;
    if (!it->second.primary) (void)create_voice(it->second);

    VITREO_TRACE("[dsound] CreateSoundBuffer %s flags=0x%x bytes=%u %uHz %uch %ubit"
                 " samples=0x%llx -> buffer=0x%llx\n",
                 primary ? "primary" : "secondary", flags, it->second.bytes,
                 it->second.format.samples_per_second, it->second.format.channels,
                 it->second.format.bits_per_sample,
                 static_cast<unsigned long long>(it->second.samples),
                 static_cast<unsigned long long>(buffer));
    dsound_diag("create buffer=0x%llx primary=%u flags=0x%x bytes=%u "
                "rate=%u channels=%u bits=%u\n",
                static_cast<unsigned long long>(buffer), primary ? 1u : 0u,
                flags, it->second.bytes,
                it->second.format.samples_per_second,
                it->second.format.channels, it->second.format.bits_per_sample);
    return dsound::DS_OK;
}

u32 DirectSoundManager::duplicate_sound_buffer(GuestAddr object, GuestAddr source,
                                               GuestAddr output) {
    std::lock_guard lock(m_mutex);
    if (!m_memory || output == 0) return dsound::E_INVALIDARG;
    (void)write_ptr(output, 0);
    if (!find_object(object)) return dsound::DSERR_UNINITIALIZED;
    BufferObject* original = find_buffer(source);
    if (!original) return dsound::E_INVALIDARG;
    // Windows refuses to duplicate the primary buffer.
    if (original->primary) return dsound::E_INVALIDARG;
    if (!ensure_storage()) return dsound::E_OUTOFMEMORY;

    // A duplicate shares the original's sample memory but owns its own
    // position, volume, pan and playback state.
    BufferObject state;
    state.flags = original->flags;
    state.bytes = original->bytes;
    state.samples = original->samples;
    state.format = original->format;
    state.frequency = original->frequency;
    state.volume = original->volume;
    state.pan = original->pan;
    state.version8 = original->version8;
    state.owns_samples = false;

    const GuestAddr buffer = new_object(Vtable::Buffer);
    if (buffer == 0 || !write_ptr(output, buffer)) return dsound::E_OUTOFMEMORY;
    auto [it, inserted] = m_buffers.emplace(buffer, std::move(state));
    if (!inserted) return dsound::E_OUTOFMEMORY;
    (void)create_voice(it->second);

    VITREO_TRACE("[dsound] DuplicateSoundBuffer source=0x%llx -> buffer=0x%llx\n",
                 static_cast<unsigned long long>(source),
                 static_cast<unsigned long long>(buffer));
    return dsound::DS_OK;
}

u32 DirectSoundManager::buffer_query_interface(GuestAddr buffer, GuestAddr iid,
                                               GuestAddr output) {
    std::lock_guard lock(m_mutex);
    if (!m_memory || output == 0) return dsound::E_INVALIDARG;
    (void)write_ptr(output, 0);
    auto found = m_buffers.find(buffer);
    if (found == m_buffers.end()) {
        auto facet = m_facets.find(buffer);
        if (facet != m_facets.end()) found = m_buffers.find(facet->second);
    }
    if (found == m_buffers.end()) return dsound::DSERR_UNINITIALIZED;
    BufferObject* state = &found->second;
    const GuestAddr primary_ptr = found->first;
    Guid requested{};
    if (iid == 0 || m_memory->read_bytes(
            iid, reinterpret_cast<Byte*>(&requested), sizeof(requested)).is_err()) {
        return dsound::E_INVALIDARG;
    }
    if (guid_equal(requested, IID_IDIRECTSOUND3DLISTENER)) {
        return ensure_listener(*state, primary_ptr, output);
    }
    if (guid_equal(requested, IID_IDIRECTSOUNDNOTIFY)) {
        // Learn (IDirectSoundNotify8): only buffers created with
        // DSBCAPS_CTRLPOSITIONNOTIFY support notifications.
        if (state->primary || (state->flags & dsound::DSBCAPS_CTRLPOSITIONNOTIFY) == 0) {
            return dsound::E_NOINTERFACE;
        }
        if (state->notify == 0) {
            state->notify = new_object(Vtable::Notify);
            if (state->notify == 0) return dsound::E_OUTOFMEMORY;
            m_facets[state->notify] = primary_ptr;
        }
        (void)write_ptr(output, state->notify);
        ++state->ref_count;
        return dsound::DS_OK;
    }
    const bool buffer8 = guid_equal(requested, IID_IDIRECTSOUNDBUFFER8);
    if (!guid_equal(requested, IID_IUNKNOWN) &&
        !guid_equal(requested, IID_IDIRECTSOUNDBUFFER) &&
        !(buffer8 && state->version8 && !state->primary)) {
        return dsound::E_NOINTERFACE;
    }
    (void)write_ptr(output, primary_ptr);
    ++state->ref_count;
    return dsound::DS_OK;
}

u32 DirectSoundManager::buffer_add_ref(GuestAddr buffer) {
    std::lock_guard lock(m_mutex);
    BufferObject* state = find_buffer(buffer);
    return state ? ++state->ref_count : 0;
}

u32 DirectSoundManager::buffer_release_locked(
    GuestAddr buffer, std::vector<std::unique_ptr<audio::IAudioVoice>>& dead) {
    auto found = m_buffers.find(buffer);
    if (found == m_buffers.end()) {
        auto facet = m_facets.find(buffer);
        if (facet != m_facets.end()) found = m_buffers.find(facet->second);
    }
    if (found == m_buffers.end()) return 0;
    BufferObject* state = &found->second;
    const u32 result = --state->ref_count;
    if (result != 0) return result;
    const GuestAddr primary_ptr = found->first;

    // Drop any 3D listener attached to this buffer before freeing samples.
    if (state->listener != 0) {
        const GuestAddr listener = state->listener;
        state->listener = 0;
        if (auto* listener_state = find_listener(listener)) {
            listener_state->buffer = 0;
            listener_state->ref_count = 0;
            m_listeners.erase(listener);
            free_object(listener, pointer_size());
        }
    }
    if (state->notify != 0) {
        m_facets.erase(state->notify);
        free_object(state->notify, pointer_size());
    }
    // Stop the device before the sample memory goes away.
    if (state->voice) dead.push_back(std::move(state->voice));
    dead.clear();
    if (state->owns_samples && state->samples != 0 && m_memory) {
        (void)m_memory->unmap_region(state->samples, state->mapped_bytes);
    }
    m_buffers.erase(found);
    free_object(primary_ptr, pointer_size());
    return 0;
}

u32 DirectSoundManager::buffer_release(GuestAddr buffer) {
    std::vector<std::unique_ptr<audio::IAudioVoice>> dead;
    std::lock_guard lock(m_mutex);
    return buffer_release_locked(buffer, dead);
}

} // namespace vitreo::winapi
