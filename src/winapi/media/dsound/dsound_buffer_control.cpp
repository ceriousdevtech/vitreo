/// @file dsound_buffer_control.cpp
/// @brief IDirectSoundBuffer(8) control surface: Lock, Unlock, Play, Stop, positions and format.

#include "winapi/media/dsound/dsound_internal.hpp"

namespace vitreo::winapi {
namespace {

constexpr u32 DSBCAPS_BYTES = 20;

} // namespace

u32 DirectSoundManager::buffer_get_caps(GuestAddr buffer, GuestAddr caps) {
    std::lock_guard lock(m_mutex);
    BufferObject* state = find_buffer(buffer);
    if (!state) return dsound::DSERR_UNINITIALIZED;
    if (!m_memory || caps == 0) return dsound::E_INVALIDARG;
    if (m_memory->read_u32(caps).value_or(0) != DSBCAPS_BYTES) {
        return dsound::E_INVALIDARG;
    }
    // Vitreo mixes in software, so report DSBCAPS_LOCSOFTWARE regardless of
    // any location hint the caller requested.
    const u32 reported =
        (state->flags & ~dsound::DSBCAPS_LOCHARDWARE) | dsound::DSBCAPS_LOCSOFTWARE;
    (void)m_memory->write_u32(caps + 4, reported);
    (void)m_memory->write_u32(caps + 8, state->bytes);
    (void)m_memory->write_u32(caps + 12, 0);  // dwUnlockTransferRate (software)
    (void)m_memory->write_u32(caps + 16, 0);  // dwPlayCpuOverhead
    return dsound::DS_OK;
}

u32 DirectSoundManager::buffer_get_current_position(GuestAddr buffer, GuestAddr play,
                                                    GuestAddr write) {
    std::lock_guard lock(m_mutex);
    BufferObject* state = find_buffer(buffer);
    if (!state) return dsound::DSERR_UNINITIALIZED;
    if (!m_memory) return dsound::E_INVALIDARG;
    if (state->primary) return dsound::DSERR_INVALIDCALL;

    const u32 play_cursor = state->voice ? state->voice->play_cursor() : 0;
    const u32 write_cursor = state->voice ? state->voice->write_cursor() : play_cursor;
    if (play != 0) (void)m_memory->write_u32(play, play_cursor);
    if (write != 0) (void)m_memory->write_u32(write, write_cursor);
    VITREO_TRACE("[dsound] Buffer::GetCurrentPosition buffer=0x%llx play=%u write=%u"
                 " playing=%d\n",
                 static_cast<unsigned long long>(buffer), play_cursor, write_cursor,
                 (state->voice && state->voice->is_playing()) ? 1 : 0);
    dsound_diag("position buffer=0x%llx play=%u write=%u playing=%u\n",
                static_cast<unsigned long long>(buffer), play_cursor,
                write_cursor,
                (state->voice && state->voice->is_playing()) ? 1u : 0u);
    return dsound::DS_OK;
}

u32 DirectSoundManager::buffer_get_format(GuestAddr buffer, GuestAddr format,
                                          u32 size, GuestAddr written) {
    std::lock_guard lock(m_mutex);
    BufferObject* state = find_buffer(buffer);
    if (!state) return dsound::DSERR_UNINITIALIZED;
    if (!m_memory) return dsound::E_INVALIDARG;

    // A null format pointer is the size query; it requires the size output.
    if (format == 0) {
        if (written == 0) return dsound::E_INVALIDARG;
        (void)m_memory->write_u32(written, WAVEFORMATEX_BYTES);
        return dsound::DS_OK;
    }
    if (size < WAVEFORMAT_FIELD_BYTES) return dsound::E_INVALIDARG;
    write_wave_format(format, state->format);
    u32 copied = WAVEFORMAT_FIELD_BYTES;
    if (size >= WAVEFORMATEX_BYTES) {
        (void)m_memory->write_u16(format + 16, 0);  // cbSize
        copied = WAVEFORMATEX_BYTES;
    }
    if (written != 0) (void)m_memory->write_u32(written, copied);
    return dsound::DS_OK;
}

u32 DirectSoundManager::buffer_get_volume(GuestAddr buffer, GuestAddr volume) {
    std::lock_guard lock(m_mutex);
    BufferObject* state = find_buffer(buffer);
    if (!state) return dsound::DSERR_UNINITIALIZED;
    if ((state->flags & dsound::DSBCAPS_CTRLVOLUME) == 0) {
        return dsound::DSERR_CONTROLUNAVAIL;
    }
    if (!m_memory || volume == 0) return dsound::E_INVALIDARG;
    (void)m_memory->write_u32(volume, static_cast<u32>(state->volume));
    return dsound::DS_OK;
}

u32 DirectSoundManager::buffer_get_pan(GuestAddr buffer, GuestAddr pan) {
    std::lock_guard lock(m_mutex);
    BufferObject* state = find_buffer(buffer);
    if (!state) return dsound::DSERR_UNINITIALIZED;
    if ((state->flags & dsound::DSBCAPS_CTRLPAN) == 0) {
        return dsound::DSERR_CONTROLUNAVAIL;
    }
    if (!m_memory || pan == 0) return dsound::E_INVALIDARG;
    (void)m_memory->write_u32(pan, static_cast<u32>(state->pan));
    return dsound::DS_OK;
}

u32 DirectSoundManager::buffer_get_frequency(GuestAddr buffer, GuestAddr frequency) {
    std::lock_guard lock(m_mutex);
    BufferObject* state = find_buffer(buffer);
    if (!state) return dsound::DSERR_UNINITIALIZED;
    if ((state->flags & dsound::DSBCAPS_CTRLFREQUENCY) == 0) {
        return dsound::DSERR_CONTROLUNAVAIL;
    }
    if (!m_memory || frequency == 0) return dsound::E_INVALIDARG;
    (void)m_memory->write_u32(frequency, state->frequency);
    return dsound::DS_OK;
}

u32 DirectSoundManager::buffer_get_status(GuestAddr buffer, GuestAddr status) {
    std::lock_guard lock(m_mutex);
    BufferObject* state = find_buffer(buffer);
    if (!state) return dsound::DSERR_UNINITIALIZED;
    if (!m_memory || status == 0) return dsound::E_INVALIDARG;
    u32 value = 0;
    if (state->voice && state->voice->is_playing()) {
        value |= dsound::DSBSTATUS_PLAYING;
        if (state->looping) value |= dsound::DSBSTATUS_LOOPING;
    }
    (void)m_memory->write_u32(status, value);
    VITREO_TRACE("[dsound] Buffer::GetStatus buffer=0x%llx -> 0x%x\n",
                 static_cast<unsigned long long>(buffer), value);
    return dsound::DS_OK;
}

u32 DirectSoundManager::buffer_lock(GuestAddr buffer, u32 offset, u32 bytes,
                                    GuestAddr ptr1, GuestAddr bytes1,
                                    GuestAddr ptr2, GuestAddr bytes2, u32 flags) {
    std::lock_guard lock(m_mutex);
    BufferObject* state = find_buffer(buffer);
    if (!state) return dsound::DSERR_UNINITIALIZED;
    if (!m_memory || ptr1 == 0 || bytes1 == 0) return dsound::E_INVALIDARG;
    // Writing the primary buffer directly requires the write-primary level,
    // which Vitreo does not grant; secondary buffers are the mix inputs.
    if (state->primary) return dsound::DSERR_PRIOLEVELNEEDED;
    if (state->locked) return dsound::DSERR_INVALIDCALL;
    if (state->bytes == 0) return dsound::E_INVALIDARG;

    if ((flags & dsound::DSBLOCK_ENTIREBUFFER) != 0) {
        offset = 0;
        bytes = state->bytes;
    } else if ((flags & dsound::DSBLOCK_FROMWRITECURSOR) != 0) {
        offset = state->voice ? state->voice->write_cursor() : 0;
    }
    if (offset >= state->bytes || bytes > state->bytes) return dsound::E_INVALIDARG;

    // The lock wraps around the ring when it runs past the end.
    const u32 first = std::min(bytes, state->bytes - offset);
    const u32 second = bytes - first;

    (void)write_ptr(ptr1, state->samples + offset);
    (void)m_memory->write_u32(bytes1, first);
    if (ptr2 != 0) (void)write_ptr(ptr2, second != 0 ? state->samples : 0);
    if (bytes2 != 0) (void)m_memory->write_u32(bytes2, second);

    state->locked = true;
    state->lock_offset = offset;
    state->lock_bytes = bytes;
    VITREO_TRACE("[dsound] Buffer::Lock buffer=0x%llx offset=%u bytes=%u flags=0x%x"
                 " -> ptr1=0x%llx(%u) ptr2=%u\n",
                 static_cast<unsigned long long>(buffer), offset, bytes, flags,
                 static_cast<unsigned long long>(state->samples + offset), first, second);
    dsound_diag("lock buffer=0x%llx offset=%u bytes=%u flags=0x%x "
                "first=%u second=%u\n",
                static_cast<unsigned long long>(buffer), offset, bytes, flags,
                first, second);
    return dsound::DS_OK;
}

u32 DirectSoundManager::buffer_unlock(GuestAddr buffer, GuestAddr ptr1, u32 bytes1,
                                      GuestAddr ptr2, u32 bytes2) {
    std::lock_guard lock(m_mutex);
    BufferObject* state = find_buffer(buffer);
    if (!state) return dsound::DSERR_UNINITIALIZED;
    if (!state->locked) return dsound::DSERR_INVALIDCALL;
    if (bytes1 + bytes2 > state->lock_bytes) return dsound::E_INVALIDARG;
    (void)ptr1;
    (void)ptr2;
    // The guest wrote straight into the sample ring, so there is nothing to
    // copy back; the device reads the new bytes on its next chunk.
    state->locked = false;
    return dsound::DS_OK;
}

u32 DirectSoundManager::buffer_play(GuestAddr buffer, u32 reserved, u32 priority,
                                    u32 flags) {
    std::lock_guard lock(m_mutex);
    BufferObject* state = find_buffer(buffer);
    if (!state) return dsound::DSERR_UNINITIALIZED;
    if (reserved != 0) return dsound::E_INVALIDARG;
    (void)priority;
    // Playing the primary buffer only means "keep the mixer running", which is
    // always true here: the device stays open for the secondary voices.
    if (state->primary) return dsound::DS_OK;
    if (!state->voice && !create_voice(*state)) return dsound::DSERR_NODRIVER;

    state->looping = (flags & dsound::DSBPLAY_LOOPING) != 0;
    if (!state->voice->play(state->looping)) return dsound::DSERR_NODRIVER;
    state->last_cursor = state->voice->play_cursor();
    state->was_playing = true;
    if (!state->notifications.empty()) wake_notify_thread_locked();
    VITREO_TRACE("[dsound] Buffer::Play buffer=0x%llx flags=0x%x looping=%d\n",
                 static_cast<unsigned long long>(buffer), flags,
                 state->looping ? 1 : 0);
    dsound_diag("play buffer=0x%llx flags=0x%x looping=%u\n",
                static_cast<unsigned long long>(buffer), flags,
                state->looping ? 1u : 0u);
    return dsound::DS_OK;
}

u32 DirectSoundManager::buffer_stop(GuestAddr buffer) {
    std::lock_guard lock(m_mutex);
    BufferObject* state = find_buffer(buffer);
    if (!state) return dsound::DSERR_UNINITIALIZED;
    if (state->primary) return dsound::DS_OK;
    const bool was_playing = state->voice && state->voice->is_playing();
    if (state->voice) state->voice->stop();
    state->looping = false;
    if (was_playing) {
        // Learn (DSBPOSITIONNOTIFY): DSBPN_OFFSETSTOP fires on Stop.
        for (const auto& position : state->notifications) {
            if (position.offset == dsound::DSBPN_OFFSETSTOP) signal(position.event);
        }
    }
    state->was_playing = false;
    VITREO_TRACE("[dsound] Buffer::Stop buffer=0x%llx\n",
                 static_cast<unsigned long long>(buffer));
    dsound_diag("stop buffer=0x%llx\n",
                static_cast<unsigned long long>(buffer));
    return dsound::DS_OK;
}

u32 DirectSoundManager::buffer_set_current_position(GuestAddr buffer, u32 position) {
    std::lock_guard lock(m_mutex);
    BufferObject* state = find_buffer(buffer);
    if (!state) return dsound::DSERR_UNINITIALIZED;
    if (state->primary) return dsound::DSERR_INVALIDCALL;
    if (state->bytes == 0 || position >= state->bytes) return dsound::E_INVALIDARG;
    if (state->voice) state->voice->set_position(position);
    state->last_cursor = position;
    return dsound::DS_OK;
}

u32 DirectSoundManager::buffer_set_format(GuestAddr buffer, GuestAddr format) {
    std::lock_guard lock(m_mutex);
    BufferObject* state = find_buffer(buffer);
    if (!state) return dsound::DSERR_UNINITIALIZED;
    // Windows only allows SetFormat on the primary buffer, and only at
    // DSSCL_PRIORITY or higher.
    if (!state->primary) return dsound::DSERR_INVALIDCALL;
    if (format == 0) return dsound::E_INVALIDARG;
    WaveFormat requested{};
    if (!read_wave_format(format, requested)) return dsound::E_INVALIDARG;
    if (requested.format_tag != WAVE_FORMAT_PCM ||
        (requested.channels != 1 && requested.channels != 2) ||
        (requested.bits_per_sample != 8 && requested.bits_per_sample != 16) ||
        requested.samples_per_second == 0) {
        return dsound::DSERR_BADFORMAT;
    }
    // Secondary voices are rendered at their own rates and mixed by the
    // device, so the primary format is recorded rather than imposed.
    state->format = requested;
    VITREO_TRACE("[dsound] Buffer::SetFormat primary %uHz %uch %ubit\n",
                 requested.samples_per_second, requested.channels,
                 requested.bits_per_sample);
    return dsound::DS_OK;
}

u32 DirectSoundManager::buffer_set_volume(GuestAddr buffer, i32 volume) {
    std::lock_guard lock(m_mutex);
    BufferObject* state = find_buffer(buffer);
    if (!state) return dsound::DSERR_UNINITIALIZED;
    if ((state->flags & dsound::DSBCAPS_CTRLVOLUME) == 0) {
        return dsound::DSERR_CONTROLUNAVAIL;
    }
    if (volume < dsound::DSBVOLUME_MIN || volume > dsound::DSBVOLUME_MAX) {
        return dsound::E_INVALIDARG;
    }
    state->volume = volume;
    if (state->voice) state->voice->set_volume(volume);
    return dsound::DS_OK;
}

u32 DirectSoundManager::buffer_set_pan(GuestAddr buffer, i32 pan) {
    std::lock_guard lock(m_mutex);
    BufferObject* state = find_buffer(buffer);
    if (!state) return dsound::DSERR_UNINITIALIZED;
    if ((state->flags & dsound::DSBCAPS_CTRLPAN) == 0) {
        return dsound::DSERR_CONTROLUNAVAIL;
    }
    if (pan < dsound::DSBPAN_LEFT || pan > dsound::DSBPAN_RIGHT) {
        return dsound::E_INVALIDARG;
    }
    state->pan = pan;
    if (state->voice) state->voice->set_pan(pan);
    return dsound::DS_OK;
}

u32 DirectSoundManager::buffer_set_frequency(GuestAddr buffer, u32 frequency) {
    std::lock_guard lock(m_mutex);
    BufferObject* state = find_buffer(buffer);
    if (!state) return dsound::DSERR_UNINITIALIZED;
    if ((state->flags & dsound::DSBCAPS_CTRLFREQUENCY) == 0) {
        return dsound::DSERR_CONTROLUNAVAIL;
    }
    // DSBFREQUENCY_ORIGINAL (0) restores the buffer's own sample rate.
    if (frequency != 0 && (frequency < 100 || frequency > 200000)) {
        return dsound::E_INVALIDARG;
    }
    state->frequency = frequency == 0 ? state->format.samples_per_second : frequency;
    if (state->voice) state->voice->set_frequency(frequency);
    return dsound::DS_OK;
}

u32 DirectSoundManager::buffer_restore(GuestAddr buffer) {
    std::lock_guard lock(m_mutex);
    // Vitreo never loses buffer memory, so a restore is always a no-op success.
    return find_buffer(buffer) ? dsound::DS_OK : dsound::DSERR_UNINITIALIZED;
}

u32 DirectSoundManager::buffer_set_fx(GuestAddr buffer, u32 count, GuestAddr descs,
                                      GuestAddr results) {
    std::lock_guard lock(m_mutex);
    BufferObject* state = find_buffer(buffer);
    if (!state) return dsound::DSERR_UNINITIALIZED;
    // Learn (IDirectSoundBuffer8::SetFX): the buffer needs DSBCAPS_CTRLFX and
    // must be stopped and unlocked.
    if ((state->flags & dsound::DSBCAPS_CTRLFX) == 0) return dsound::DSERR_CONTROLUNAVAIL;
    if (state->locked || (state->voice && state->voice->is_playing())) {
        return dsound::DSERR_INVALIDCALL;
    }
    if (count == 0) return descs == 0 && results == 0 ? dsound::DS_OK : dsound::E_INVALIDARG;
    if (descs == 0) return dsound::E_INVALIDARG;
    // No DMO effect is available to the software mixer.
    static std::once_flag once;
    unimplemented_once(once, "dsound!IDirectSoundBuffer8::SetFX: DMO effects");
    if (results != 0 && m_memory) {
        constexpr u32 DSFXR_UNKNOWN = 5;  // dsound.h DSFXR_*
        for (u32 i = 0; i < count; ++i) (void)m_memory->write_u32(results + i * 4u, DSFXR_UNKNOWN);
    }
    return dsound::DSERR_FXUNAVAILABLE;
}

u32 DirectSoundManager::buffer_acquire_resources(GuestAddr buffer, u32 flags, u32 count,
                                                 GuestAddr results) {
    std::lock_guard lock(m_mutex);
    BufferObject* state = find_buffer(buffer);
    if (!state) return dsound::DSERR_UNINITIALIZED;
    (void)flags;
    (void)count;
    (void)results;
    // Learn (AcquireResources): only DSBCAPS_LOCDEFER buffers defer their
    // resources; a software buffer always has them.
    if ((state->flags & dsound::DSBCAPS_LOCDEFER) == 0) return dsound::DSERR_INVALIDCALL;
    return dsound::DS_OK;
}

u32 DirectSoundManager::buffer_get_object_in_path(GuestAddr buffer, GuestAddr object_guid,
                                                  u32 index, GuestAddr iid,
                                                  GuestAddr output) {
    std::lock_guard lock(m_mutex);
    BufferObject* state = find_buffer(buffer);
    if (!state) return dsound::DSERR_UNINITIALIZED;
    (void)index;
    if (object_guid == 0 || iid == 0 || output == 0) return dsound::E_INVALIDARG;
    // No effect is ever placed in the path (SetFX refuses every effect).
    return (state->flags & dsound::DSBCAPS_CTRLFX) == 0 ? dsound::DSERR_CONTROLUNAVAIL
                                                        : dsound::DSERR_OBJECTNOTFOUND;
}

} // namespace vitreo::winapi
