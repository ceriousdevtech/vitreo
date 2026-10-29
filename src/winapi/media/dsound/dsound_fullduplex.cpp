/// @file dsound_fullduplex.cpp
/// @brief DirectSoundFullDuplexCreate and IDirectSoundFullDuplex.

#include "winapi/media/dsound/dsound_internal.hpp"

namespace vitreo::winapi {
// ---------------------------------------------------------------------------
// DirectSoundFullDuplexCreate / IDirectSoundFullDuplex
// ---------------------------------------------------------------------------

u32 DirectSoundManager::full_duplex_build(GuestAddr capture_guid, GuestAddr render_guid,
                                          GuestAddr capture_desc, GuestAddr render_desc,
                                          u64 hwnd, u32 level, GuestAddr& capture,
                                          GuestAddr& render, GuestAddr capture_buffer,
                                          GuestAddr render_buffer) {
    capture = 0;
    render = 0;
    // The capture and playback objects live in guest scratch at the end of
    // the region while their outputs are collected.
    GuestAddr slot = 0;
    {
        std::lock_guard lock(m_mutex);
        if (!ensure_storage()) return dsound::E_OUTOFMEMORY;
        slot = m_region_base + m_region_size - 0x1000 + 0xC00;
    }
    u32 hr = capture_create(capture_guid, slot, 0, true);
    if (hr != dsound::DS_OK) return hr;
    capture = m_x64 ? static_cast<GuestAddr>(m_memory->read_u64(slot).value_or(0))
                    : m_memory->read_u32(slot).value_or(0);
    hr = direct_sound_create(render_guid, slot + 8, 0, true);
    if (hr == dsound::DS_OK) {
        render = m_x64 ? static_cast<GuestAddr>(m_memory->read_u64(slot + 8).value_or(0))
                       : m_memory->read_u32(slot + 8).value_or(0);
        hr = set_cooperative_level(render, hwnd, level);
    }
    if (hr == dsound::DS_OK) hr = capture_create_buffer(capture, capture_desc, slot + 16, 0);
    GuestAddr cbuf = 0;
    if (hr == dsound::DS_OK) {
        cbuf = m_x64 ? static_cast<GuestAddr>(m_memory->read_u64(slot + 16).value_or(0))
                     : m_memory->read_u32(slot + 16).value_or(0);
        hr = create_sound_buffer(render, render_desc, slot + 24, 0);
    }
    GuestAddr rbuf = 0;
    if (hr == dsound::DS_OK) {
        rbuf = m_x64 ? static_cast<GuestAddr>(m_memory->read_u64(slot + 24).value_or(0))
                     : m_memory->read_u32(slot + 24).value_or(0);
        // IDirectSoundBuffer8: the primary buffer has none.
        BufferObject* rstate = nullptr;
        {
            std::lock_guard lock(m_mutex);
            rstate = find_buffer(rbuf);
            if (rstate == nullptr || rstate->primary) hr = dsound::E_INVALIDARG;
        }
    }
    if (hr != dsound::DS_OK) {
        if (rbuf != 0) (void)buffer_release(rbuf);
        if (cbuf != 0) (void)cbuffer_release(cbuf);
        if (render != 0) (void)release(render);
        if (capture != 0) (void)capture_release(capture);
        capture = render = 0;
        return hr;
    }
    (void)write_ptr(capture_buffer, cbuf);
    (void)write_ptr(render_buffer, rbuf);
    return dsound::DS_OK;
}

u32 DirectSoundManager::full_duplex_create(GuestAddr capture_guid, GuestAddr render_guid,
                                           GuestAddr capture_desc, GuestAddr render_desc,
                                           u64 hwnd, u32 level, GuestAddr output,
                                           GuestAddr capture_buffer, GuestAddr render_buffer,
                                           GuestAddr outer) {
    // Measured: a NULL output or a NULL descriptor is E_INVALIDARG and
    // aggregation CLASS_E_NOAGGREGATION, every output untouched.
    if (!m_memory || output == 0 || capture_desc == 0 || render_desc == 0 ||
        capture_buffer == 0 || render_buffer == 0) {
        return dsound::E_INVALIDARG;
    }
    if (outer != 0) return dsound::CLASS_E_NOAGGREGATION;
    GuestAddr capture = 0, render = 0;
    const u32 hr = full_duplex_build(capture_guid, render_guid, capture_desc, render_desc,
                                     hwnd, level, capture, render, capture_buffer,
                                     render_buffer);
    if (hr != dsound::DS_OK) return hr;
    std::lock_guard lock(m_mutex);
    const GuestAddr object = new_object(Vtable::FullDuplex);
    if (object == 0) return dsound::E_OUTOFMEMORY;
    m_full_duplex.emplace(object, FullDuplexObject{1, true, capture, render});
    (void)write_ptr(output, object);
    return dsound::DS_OK;
}

u32 DirectSoundManager::full_duplex_query_interface(GuestAddr object, GuestAddr iid,
                                                    GuestAddr output) {
    std::array<u8, 16> requested{};
    GuestAddr capture = 0, render = 0;
    {
        std::lock_guard lock(m_mutex);
        if (!m_memory || output == 0) return dsound::E_POINTER;
        (void)write_ptr(output, 0);
        auto it = m_full_duplex.find(object);
        if (it == m_full_duplex.end()) return dsound::E_NOINTERFACE;
        if (!read_guid(iid, requested)) return dsound::E_INVALIDARG;
        if (guid_equal(requested, IID_IUNKNOWN) ||
            guid_equal(requested, IID_IDIRECTSOUNDFULLDUPLEX)) {
            ++it->second.refs;
            (void)write_ptr(output, object);
            return dsound::DS_OK;
        }
        capture = it->second.capture;
        render = it->second.render;
    }
    // Learn (IDirectSoundFullDuplex8): the object also answers the capture
    // and playback device interfaces.
    if (capture != 0 && guid_equal(requested, IID_IDIRECTSOUNDCAPTURE)) {
        return capture_query_interface(capture, iid, output);
    }
    if (render != 0 && (guid_equal(requested, IID_IDIRECTSOUND) ||
                        guid_equal(requested, IID_IDIRECTSOUND8))) {
        return query_interface(render, iid, output);
    }
    return dsound::E_NOINTERFACE;
}

u32 DirectSoundManager::full_duplex_add_ref(GuestAddr object) {
    std::lock_guard lock(m_mutex);
    auto it = m_full_duplex.find(object);
    return it == m_full_duplex.end() ? 0u : ++it->second.refs;
}

u32 DirectSoundManager::full_duplex_release(GuestAddr object) {
    GuestAddr capture = 0, render = 0;
    {
        std::lock_guard lock(m_mutex);
        auto it = m_full_duplex.find(object);
        if (it == m_full_duplex.end()) return 0;
        const u32 refs = --it->second.refs;
        if (refs != 0) return refs;
        capture = it->second.capture;
        render = it->second.render;
        m_full_duplex.erase(it);
        free_object(object, pointer_size());
    }
    if (capture != 0) (void)capture_release(capture);
    if (render != 0) (void)release(render);
    return 0;
}

u32 DirectSoundManager::full_duplex_initialize(GuestAddr object, GuestAddr capture_guid,
                                               GuestAddr render_guid, GuestAddr capture_desc,
                                               GuestAddr render_desc, u64 hwnd, u32 level,
                                               GuestAddr capture_buffer,
                                               GuestAddr render_buffer) {
    {
        std::lock_guard lock(m_mutex);
        auto it = m_full_duplex.find(object);
        if (it == m_full_duplex.end()) return dsound::E_INVALIDARG;
        if (it->second.initialized) return dsound::DSERR_ALREADYINITIALIZED;
    }
    if (capture_desc == 0 || render_desc == 0 || capture_buffer == 0 || render_buffer == 0) {
        return dsound::E_INVALIDARG;
    }
    GuestAddr capture = 0, render = 0;
    const u32 hr = full_duplex_build(capture_guid, render_guid, capture_desc, render_desc,
                                     hwnd, level, capture, render, capture_buffer,
                                     render_buffer);
    if (hr != dsound::DS_OK) return hr;
    std::lock_guard lock(m_mutex);
    auto it = m_full_duplex.find(object);
    if (it == m_full_duplex.end()) return dsound::E_INVALIDARG;
    it->second.initialized = true;
    it->second.capture = capture;
    it->second.render = render;
    return dsound::DS_OK;
}

} // namespace vitreo::winapi
