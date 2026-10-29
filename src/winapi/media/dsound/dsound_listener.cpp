/// @file dsound_listener.cpp
/// @brief IDirectSound3DListener.

#include "winapi/media/dsound/dsound_internal.hpp"

namespace vitreo::winapi {
namespace {

constexpr u32 DS3DLISTENER_BYTES = 64;

} // namespace

// ---------------------------------------------------------------------------
// IDirectSound3DListener
// ---------------------------------------------------------------------------

u32 DirectSoundManager::listener_query_interface(GuestAddr listener, GuestAddr iid,
                                                 GuestAddr output) {
    std::lock_guard lock(m_mutex);
    if (!m_memory || output == 0) return dsound::E_INVALIDARG;
    (void)write_ptr(output, 0);
    if (!find_listener(listener)) return dsound::DSERR_UNINITIALIZED;
    Guid requested{};
    if (iid == 0 || m_memory->read_bytes(
            iid, reinterpret_cast<Byte*>(&requested), sizeof(requested)).is_err()) {
        return dsound::E_INVALIDARG;
    }
    if (!guid_equal(requested, IID_IUNKNOWN) &&
        !guid_equal(requested, IID_IDIRECTSOUND3DLISTENER)) {
        return dsound::E_NOINTERFACE;
    }
    (void)write_ptr(output, listener);
    ++find_listener(listener)->ref_count;
    return dsound::DS_OK;
}

u32 DirectSoundManager::listener_add_ref(GuestAddr listener) {
    std::lock_guard lock(m_mutex);
    ListenerObject* state = find_listener(listener);
    return state ? ++state->ref_count : 0;
}

u32 DirectSoundManager::listener_release(GuestAddr listener) {
    std::lock_guard lock(m_mutex);
    ListenerObject* state = find_listener(listener);
    if (!state) return 0;
    const u32 result = --state->ref_count;
    if (result != 0) return result;
    if (state->buffer != 0) {
        if (auto* buffer = find_buffer(state->buffer)) {
            if (buffer->listener == listener) buffer->listener = 0;
        }
    }
    m_listeners.erase(listener);
    free_object(listener, pointer_size());
    return 0;
}

u32 DirectSoundManager::listener_get_all_parameters(GuestAddr listener,
                                                   GuestAddr params) {
    std::lock_guard lock(m_mutex);
    ListenerObject* state = find_listener(listener);
    if (!state) return dsound::DSERR_UNINITIALIZED;
    if (!m_memory || params == 0) return dsound::E_INVALIDARG;
    if (m_memory->read_u32(params).value_or(0) != DS3DLISTENER_BYTES) {
        return dsound::E_INVALIDARG;
    }
    auto write_vec = [&](GuestAddr base, const float* v) {
        for (u32 i = 0; i < 3; ++i) {
            (void)m_memory->write_u32(base + i * 4, float_bits(v[i]));
        }
    };
    write_vec(params + 4, state->position);
    write_vec(params + 16, state->velocity);
    write_vec(params + 28, state->orient_front);
    write_vec(params + 40, state->orient_top);
    (void)m_memory->write_u32(params + 52, float_bits(state->distance_factor));
    (void)m_memory->write_u32(params + 56, float_bits(state->rolloff_factor));
    (void)m_memory->write_u32(params + 60, float_bits(state->doppler_factor));
    return dsound::DS_OK;
}

u32 DirectSoundManager::listener_set_all_parameters(GuestAddr listener,
                                                   GuestAddr params, u32 apply) {
    std::lock_guard lock(m_mutex);
    (void)apply;
    ListenerObject* state = find_listener(listener);
    if (!state) return dsound::DSERR_UNINITIALIZED;
    if (!m_memory || params == 0) return dsound::E_INVALIDARG;
    if (m_memory->read_u32(params).value_or(0) != DS3DLISTENER_BYTES) {
        return dsound::E_INVALIDARG;
    }
    auto read_vec = [&](GuestAddr base, float* v) {
        for (u32 i = 0; i < 3; ++i) {
            v[i] = bits_float(m_memory->read_u32(base + i * 4).value_or(0));
        }
    };
    read_vec(params + 4, state->position);
    read_vec(params + 16, state->velocity);
    read_vec(params + 28, state->orient_front);
    read_vec(params + 40, state->orient_top);
    state->distance_factor =
        bits_float(m_memory->read_u32(params + 52).value_or(float_bits(1.0f)));
    state->rolloff_factor =
        bits_float(m_memory->read_u32(params + 56).value_or(float_bits(1.0f)));
    state->doppler_factor =
        bits_float(m_memory->read_u32(params + 60).value_or(float_bits(1.0f)));
    VITREO_TRACE("[dsound] 3DListener::SetAllParameters listener=0x%llx\n",
                 static_cast<unsigned long long>(listener));
    return dsound::DS_OK;
}

u32 DirectSoundManager::listener_get_distance_factor(GuestAddr listener,
                                                     GuestAddr out) {
    std::lock_guard lock(m_mutex);
    ListenerObject* state = find_listener(listener);
    if (!state) return dsound::DSERR_UNINITIALIZED;
    if (!m_memory || out == 0) return dsound::E_INVALIDARG;
    (void)m_memory->write_u32(out, float_bits(state->distance_factor));
    return dsound::DS_OK;
}

u32 DirectSoundManager::listener_set_distance_factor(GuestAddr listener,
                                                     u32 factor_bits, u32 apply) {
    std::lock_guard lock(m_mutex);
    (void)apply;
    ListenerObject* state = find_listener(listener);
    if (!state) return dsound::DSERR_UNINITIALIZED;
    state->distance_factor = bits_float(factor_bits);
    return dsound::DS_OK;
}

u32 DirectSoundManager::listener_get_doppler_factor(GuestAddr listener,
                                                    GuestAddr out) {
    std::lock_guard lock(m_mutex);
    ListenerObject* state = find_listener(listener);
    if (!state) return dsound::DSERR_UNINITIALIZED;
    if (!m_memory || out == 0) return dsound::E_INVALIDARG;
    (void)m_memory->write_u32(out, float_bits(state->doppler_factor));
    return dsound::DS_OK;
}

u32 DirectSoundManager::listener_set_doppler_factor(GuestAddr listener,
                                                    u32 factor_bits, u32 apply) {
    std::lock_guard lock(m_mutex);
    (void)apply;
    ListenerObject* state = find_listener(listener);
    if (!state) return dsound::DSERR_UNINITIALIZED;
    state->doppler_factor = bits_float(factor_bits);
    return dsound::DS_OK;
}

u32 DirectSoundManager::listener_get_rolloff_factor(GuestAddr listener,
                                                    GuestAddr out) {
    std::lock_guard lock(m_mutex);
    ListenerObject* state = find_listener(listener);
    if (!state) return dsound::DSERR_UNINITIALIZED;
    if (!m_memory || out == 0) return dsound::E_INVALIDARG;
    (void)m_memory->write_u32(out, float_bits(state->rolloff_factor));
    return dsound::DS_OK;
}

u32 DirectSoundManager::listener_set_rolloff_factor(GuestAddr listener,
                                                    u32 factor_bits, u32 apply) {
    std::lock_guard lock(m_mutex);
    (void)apply;
    ListenerObject* state = find_listener(listener);
    if (!state) return dsound::DSERR_UNINITIALIZED;
    state->rolloff_factor = bits_float(factor_bits);
    return dsound::DS_OK;
}

u32 DirectSoundManager::listener_get_orientation(GuestAddr listener,
                                                 GuestAddr front, GuestAddr top) {
    std::lock_guard lock(m_mutex);
    ListenerObject* state = find_listener(listener);
    if (!state) return dsound::DSERR_UNINITIALIZED;
    if (!m_memory || front == 0 || top == 0) return dsound::E_INVALIDARG;
    for (u32 i = 0; i < 3; ++i) {
        (void)m_memory->write_u32(front + i * 4, float_bits(state->orient_front[i]));
        (void)m_memory->write_u32(top + i * 4, float_bits(state->orient_top[i]));
    }
    return dsound::DS_OK;
}

u32 DirectSoundManager::listener_set_orientation(GuestAddr listener,
                                                 u32 fx, u32 fy, u32 fz,
                                                 u32 tx, u32 ty, u32 tz,
                                                 u32 apply) {
    std::lock_guard lock(m_mutex);
    (void)apply;
    ListenerObject* state = find_listener(listener);
    if (!state) return dsound::DSERR_UNINITIALIZED;
    state->orient_front[0] = bits_float(fx);
    state->orient_front[1] = bits_float(fy);
    state->orient_front[2] = bits_float(fz);
    state->orient_top[0] = bits_float(tx);
    state->orient_top[1] = bits_float(ty);
    state->orient_top[2] = bits_float(tz);
    return dsound::DS_OK;
}

u32 DirectSoundManager::listener_get_position(GuestAddr listener, GuestAddr out) {
    std::lock_guard lock(m_mutex);
    ListenerObject* state = find_listener(listener);
    if (!state) return dsound::DSERR_UNINITIALIZED;
    if (!m_memory || out == 0) return dsound::E_INVALIDARG;
    for (u32 i = 0; i < 3; ++i) {
        (void)m_memory->write_u32(out + i * 4, float_bits(state->position[i]));
    }
    return dsound::DS_OK;
}

u32 DirectSoundManager::listener_set_position(GuestAddr listener, u32 x, u32 y,
                                              u32 z, u32 apply) {
    std::lock_guard lock(m_mutex);
    (void)apply;
    ListenerObject* state = find_listener(listener);
    if (!state) return dsound::DSERR_UNINITIALIZED;
    state->position[0] = bits_float(x);
    state->position[1] = bits_float(y);
    state->position[2] = bits_float(z);
    return dsound::DS_OK;
}

u32 DirectSoundManager::listener_get_velocity(GuestAddr listener, GuestAddr out) {
    std::lock_guard lock(m_mutex);
    ListenerObject* state = find_listener(listener);
    if (!state) return dsound::DSERR_UNINITIALIZED;
    if (!m_memory || out == 0) return dsound::E_INVALIDARG;
    for (u32 i = 0; i < 3; ++i) {
        (void)m_memory->write_u32(out + i * 4, float_bits(state->velocity[i]));
    }
    return dsound::DS_OK;
}

u32 DirectSoundManager::listener_set_velocity(GuestAddr listener, u32 x, u32 y,
                                              u32 z, u32 apply) {
    std::lock_guard lock(m_mutex);
    (void)apply;
    ListenerObject* state = find_listener(listener);
    if (!state) return dsound::DSERR_UNINITIALIZED;
    state->velocity[0] = bits_float(x);
    state->velocity[1] = bits_float(y);
    state->velocity[2] = bits_float(z);
    return dsound::DS_OK;
}

u32 DirectSoundManager::listener_commit_deferred(GuestAddr listener) {
    std::lock_guard lock(m_mutex);
    if (!find_listener(listener)) return dsound::DSERR_UNINITIALIZED;
    return dsound::DS_OK;
}

} // namespace vitreo::winapi
