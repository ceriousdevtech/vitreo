/// @file dsound_capture_buffer.cpp
/// @brief IDirectSoundCaptureBuffer(8) and the host capture pump.

#include "winapi/media/dsound/dsound_internal.hpp"

namespace vitreo::winapi {
namespace {

/// dsound.h DSCCAPS / DSCBCAPS: four DWORDs each.
constexpr u32 DSCBCAPS_BYTES = 16;

} // namespace

// ---------------------------------------------------------------------------
// IDirectSoundCaptureBuffer8
// ---------------------------------------------------------------------------

void DirectSoundManager::destroy_cbuffer_locked(
    GuestAddr buffer, std::vector<std::shared_ptr<audio::IAudioCaptureStream>>& dead) {
    auto it = m_cbuffers.find(buffer);
    if (it == m_cbuffers.end()) return;
    auto& state = it->second;
    if (state.stream) dead.push_back(std::move(state.stream));
    if (auto owner = m_captures.find(state.owner);
        owner != m_captures.end() && owner->second.buffer == buffer) {
        owner->second.buffer = 0;
    }
    m_facets.erase(state.notify);
    m_facets.erase(state.unknown);
    if (m_memory && state.samples != 0) (void)m_memory->unmap_region(state.samples, state.mapped_bytes);
    m_cbuffers.erase(it);
    free_object(buffer, 3 * pointer_size());
}

u32 DirectSoundManager::cbuffer_query_interface(GuestAddr buffer, GuestAddr iid,
                                                GuestAddr output) {
    std::lock_guard lock(m_mutex);
    if (!m_memory || output == 0) return dsound::E_POINTER;
    (void)write_ptr(output, 0);
    GuestAddr primary = 0;
    CaptureBufferObject* state = find_cbuffer(buffer, &primary);
    if (!state) return dsound::E_NOINTERFACE;
    std::array<u8, 16> requested{};
    if (!read_guid(iid, requested)) return dsound::E_INVALIDARG;
    GuestAddr answer = 0;
    if (guid_equal(requested, IID_IUNKNOWN)) answer = state->unknown;
    else if (guid_equal(requested, IID_IDIRECTSOUNDCAPTUREBUFFER)) answer = primary;
    else if (guid_equal(requested, IID_IDIRECTSOUNDCAPTUREBUFFER8) && state->version8) answer = primary;
    else if (guid_equal(requested, IID_IDIRECTSOUNDNOTIFY)) answer = state->notify;
    if (answer == 0) return dsound::E_NOINTERFACE;
    (void)write_ptr(output, answer);
    ++state->refs;
    return dsound::DS_OK;
}

u32 DirectSoundManager::cbuffer_add_ref(GuestAddr buffer) {
    std::lock_guard lock(m_mutex);
    CaptureBufferObject* state = find_cbuffer(buffer);
    return state ? ++state->refs : 0;
}

u32 DirectSoundManager::cbuffer_release(GuestAddr buffer) {
    std::vector<std::shared_ptr<audio::IAudioCaptureStream>> dead;
    u32 refs = 0;
    {
        std::lock_guard lock(m_mutex);
        GuestAddr primary = 0;
        CaptureBufferObject* state = find_cbuffer(buffer, &primary);
        if (!state) return 0;
        refs = --state->refs;
        if (refs == 0) destroy_cbuffer_locked(primary, dead);
    }
    for (auto& stream : dead) stream->stop();
    return refs;
}

u32 DirectSoundManager::cbuffer_get_caps(GuestAddr buffer, GuestAddr caps) {
    std::lock_guard lock(m_mutex);
    CaptureBufferObject* state = find_cbuffer(buffer);
    if (!state || !m_memory || caps == 0) return dsound::E_INVALIDARG;
    if (m_memory->read_u32(caps).value_or(0) != DSCBCAPS_BYTES) return dsound::E_INVALIDARG;
    (void)m_memory->write_u32(caps + 4, state->flags);
    (void)m_memory->write_u32(caps + 8, state->bytes);
    (void)m_memory->write_u32(caps + 12, 0);
    return dsound::DS_OK;
}

u32 DirectSoundManager::cbuffer_get_current_position(GuestAddr buffer, GuestAddr capture,
                                                     GuestAddr read) {
    std::lock_guard lock(m_mutex);
    CaptureBufferObject* state = find_cbuffer(buffer);
    if (!state || !m_memory || (capture == 0 && read == 0)) return dsound::E_INVALIDARG;
    // Every captured frame is copied into the buffer as it arrives, so the
    // capture cursor never runs ahead of the read cursor (Learn allows equal).
    if (capture != 0) (void)m_memory->write_u32(capture, state->read_pos);
    if (read != 0) (void)m_memory->write_u32(read, state->read_pos);
    return dsound::DS_OK;
}

u32 DirectSoundManager::cbuffer_get_format(GuestAddr buffer, GuestAddr format, u32 size,
                                           GuestAddr written) {
    std::lock_guard lock(m_mutex);
    CaptureBufferObject* state = find_cbuffer(buffer);
    if (!state || !m_memory) return dsound::E_INVALIDARG;
    const u32 needed = static_cast<u32>(state->format.size());
    if (format == 0) {
        if (written == 0) return dsound::E_INVALIDARG;
        (void)m_memory->write_u32(written, needed);
        return dsound::DS_OK;
    }
    if (size < needed) return dsound::E_INVALIDARG;
    (void)m_memory->write_bytes(format, reinterpret_cast<const Byte*>(state->format.data()),
                                needed);
    if (written != 0) (void)m_memory->write_u32(written, needed);
    return dsound::DS_OK;
}

u32 DirectSoundManager::cbuffer_get_status(GuestAddr buffer, GuestAddr status) {
    std::lock_guard lock(m_mutex);
    CaptureBufferObject* state = find_cbuffer(buffer);
    if (!state || !m_memory || status == 0) return dsound::E_INVALIDARG;
    u32 value = 0;
    if (state->capturing) {
        value = dsound::DSCBSTATUS_CAPTURING |
                (state->looping ? dsound::DSCBSTATUS_LOOPING : 0u);
    }
    (void)m_memory->write_u32(status, value);
    return dsound::DS_OK;
}

u32 DirectSoundManager::cbuffer_initialize(GuestAddr buffer, GuestAddr capture,
                                           GuestAddr descriptor) {
    std::lock_guard lock(m_mutex);
    (void)capture;
    (void)descriptor;
    // Every buffer is initialized by CreateCaptureBuffer (Learn: "not used by
    // applications"; measured DSERR_ALREADYINITIALIZED).
    return find_cbuffer(buffer) ? dsound::DSERR_ALREADYINITIALIZED : dsound::E_INVALIDARG;
}

u32 DirectSoundManager::cbuffer_lock(GuestAddr buffer, u32 offset, u32 bytes,
                                     GuestAddr ptr1, GuestAddr bytes1, GuestAddr ptr2,
                                     GuestAddr bytes2, u32 flags) {
    std::lock_guard lock(m_mutex);
    CaptureBufferObject* state = find_cbuffer(buffer);
    if (!state || !m_memory) return dsound::E_INVALIDARG;
    // Measured out-parameter order: *ppvAudioPtr1 is cleared first, then the
    // byte counts and the second pointer, before anything else is checked.
    if (ptr1 == 0) return dsound::E_INVALIDARG;
    (void)write_ptr(ptr1, 0);
    if (bytes1 == 0) return dsound::E_INVALIDARG;
    (void)m_memory->write_u32(bytes1, 0);
    if (ptr2 != 0) (void)write_ptr(ptr2, 0);
    if (bytes2 != 0) (void)m_memory->write_u32(bytes2, 0);
    if (state->wedged || state->locked) return dsound::E_INVALIDARG;
    if ((flags & ~dsound::DSCBLOCK_ENTIREBUFFER) != 0) return dsound::E_INVALIDARG;
    // Measured: DSCBLOCK_ENTIREBUFFER replaces dwBytes, not dwOffset.
    if ((flags & dsound::DSCBLOCK_ENTIREBUFFER) != 0) bytes = state->bytes;
    if (offset >= state->bytes || bytes == 0 || bytes > state->bytes) return dsound::E_INVALIDARG;
    const u32 first = std::min(bytes, state->bytes - offset);
    u32 second = bytes - first;
    const bool truncated = second != 0 && (ptr2 == 0 || bytes2 == 0);
    if (truncated) second = 0;
    (void)write_ptr(ptr1, state->samples + offset);
    (void)m_memory->write_u32(bytes1, first);
    if (ptr2 != 0) (void)write_ptr(ptr2, second != 0 ? state->samples : 0);
    if (bytes2 != 0) (void)m_memory->write_u32(bytes2, second);
    state->locked = true;
    state->lock_ptr1 = state->samples + offset;
    state->lock_truncated = truncated;
    return dsound::DS_OK;
}

u32 DirectSoundManager::cbuffer_unlock(GuestAddr buffer, GuestAddr ptr1, u32 bytes1,
                                       GuestAddr ptr2, u32 bytes2) {
    std::lock_guard lock(m_mutex);
    CaptureBufferObject* state = find_cbuffer(buffer);
    if (!state) return dsound::E_INVALIDARG;
    (void)bytes1;
    (void)ptr2;
    (void)bytes2;
    // Measured: only the first pointer is matched; byte counts are not.
    if (state->wedged || !state->locked || ptr1 != state->lock_ptr1) return dsound::E_INVALIDARG;
    state->locked = false;
    if (state->lock_truncated) state->wedged = true;
    state->lock_truncated = false;
    return dsound::DS_OK;
}

u32 DirectSoundManager::cbuffer_start(GuestAddr buffer, u32 flags) {
    std::shared_ptr<audio::IAudioCaptureStream> stream;
    {
        std::lock_guard lock(m_mutex);
        CaptureBufferObject* state = find_cbuffer(buffer);
        if (!state) return dsound::E_INVALIDARG;
        if ((flags & ~dsound::DSCBSTART_LOOPING) != 0) return dsound::E_INVALIDARG;
        // Learn: no effect on a buffer that is capturing (measured: the
        // looping state does not change either).
        if (state->capturing) return dsound::DS_OK;
        if (!state->stream) return dsound::DSERR_NODRIVER;
        state->capturing = true;
        state->looping = (flags & dsound::DSCBSTART_LOOPING) != 0;
        stream = state->stream;
    }
    if (stream->start()) return dsound::DS_OK;
    std::lock_guard lock(m_mutex);
    if (CaptureBufferObject* state = find_cbuffer(buffer)) state->capturing = false;
    return dsound::DSERR_NODRIVER;
}

u32 DirectSoundManager::cbuffer_stop(GuestAddr buffer) {
    std::shared_ptr<audio::IAudioCaptureStream> stream;
    {
        std::lock_guard lock(m_mutex);
        CaptureBufferObject* state = find_cbuffer(buffer);
        if (!state) return dsound::E_INVALIDARG;
        // Measured: Stop on a stopped buffer succeeds and signals nothing.
        if (!state->capturing) return dsound::DS_OK;
        state->capturing = false;
        for (const auto& position : state->notifications) {
            if (position.offset == dsound::DSBPN_OFFSETSTOP) signal(position.event);
        }
        stream = state->stream;
    }
    if (stream) {
        stream->stop();
        // Frames the device delivered after the stop point are not part of
        // the capture; the next Start continues at the read cursor.
        stream->ring().clear(false);
    }
    return dsound::DS_OK;
}

void DirectSoundManager::capture_pump(GuestAddr buffer) {
    std::lock_guard lock(m_mutex);
    auto it = m_cbuffers.find(buffer);
    if (it == m_cbuffers.end() || !it->second.stream) return;
    CaptureBufferObject& state = it->second;
    audio::CaptureRing& ring = state.stream->ring();
    if (!state.capturing) {
        ring.consume(ring.available());
        return;
    }
    const u32 align = state.block_align;
    u32 available = ring.available();
    while (available != 0 && state.capturing) {
        const u32 room = (state.bytes - state.read_pos) / align;
        const u32 frames = std::min(available, room);
        if (frames == 0) break;
        state.scratch.resize(static_cast<usize>(frames) * align);
        const u32 copied = ring.read(state.scratch.data(), frames);
        (void)m_memory->write_bytes(state.samples + state.read_pos,
                                    reinterpret_cast<const Byte*>(state.scratch.data()),
                                    static_cast<usize>(copied) * align);
        const u32 from = state.read_pos;
        state.read_pos += copied * align;
        signal_crossed(state.notifications, from, state.read_pos);
        available -= copied;
        if (state.read_pos >= state.bytes) {
            state.read_pos = 0;
            if (!state.looping) {
                // Measured: a non-looping buffer stops when full, both cursors
                // return to 0, and DSBPN_OFFSETSTOP fires.
                state.capturing = false;
                for (const auto& position : state.notifications) {
                    if (position.offset == dsound::DSBPN_OFFSETSTOP) signal(position.event);
                }
                ring.consume(ring.available());
                state.stream->request_stop();
                break;
            }
        }
    }
}

u32 DirectSoundManager::cbuffer_get_object_in_path(GuestAddr buffer, GuestAddr object_guid,
                                                   u32 index, GuestAddr iid,
                                                   GuestAddr output) {
    std::lock_guard lock(m_mutex);
    CaptureBufferObject* state = find_cbuffer(buffer);
    if (!state || object_guid == 0 || iid == 0 || output == 0) return dsound::E_INVALIDARG;
    (void)index;
    // Measured: DSERR_CONTROLUNAVAIL without DSCBCAPS_CTRLFX, and
    // DSERR_OBJECTNOTFOUND with it (no effect object is instantiated, as on
    // Windows Vista and later); the output is not written.
    return (state->flags & dsound::DSCBCAPS_CTRLFX) == 0 ? dsound::DSERR_CONTROLUNAVAIL
                                                         : dsound::DSERR_OBJECTNOTFOUND;
}

u32 DirectSoundManager::cbuffer_get_fx_status(GuestAddr buffer, u32 count, GuestAddr status) {
    std::lock_guard lock(m_mutex);
    (void)buffer;
    (void)count;
    (void)status;
    // Measured: E_INVALIDARG for every call, with or without effects, and the
    // status array is not written.
    return dsound::DSERR_INVALIDPARAM;
}

} // namespace vitreo::winapi
