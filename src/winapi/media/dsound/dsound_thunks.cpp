/// @file dsound_thunks.cpp
/// @brief dsound.dll export registration: DirectSoundCreate* and the COM method thunks.

#include "winapi/media/dsound/dsound_internal.hpp"

namespace vitreo::winapi {
namespace {

constexpr u32 DS_UNCERTIFIED = 1;

} // namespace

// ---------------------------------------------------------------------------
// Exports
// ---------------------------------------------------------------------------

DllExports create_dsound_thunks(DirectSoundManager& manager) {
    DllExports exports;
    auto ordinal_of = [](std::string_view name) -> u16 {
        for (const auto& e : dsound::kDsoundExports) {
            if (name == e.name) return e.ordinal;
        }
        return 0;
    };
    auto add_export = [&](const char* name, u16 stdcall_bytes, ApiHandler handler) {
        exports.entries.push_back({name, ordinal_of(name), std::move(handler), stdcall_bytes});
    };
    auto out = [](Marshaler& m, u32 hr) { m.set_return(hr); };

    add_export("DirectSoundCreate", 12, [&manager, out](Marshaler& m) {
        manager.note_guest_bitness(m.cpu_is_64bit());
        out(m, manager.direct_sound_create(m.arg_ptr(0), m.arg_ptr(1), m.arg_ptr(2), false));
    });
    add_export("DirectSoundCreate8", 12, [&manager, out](Marshaler& m) {
        manager.note_guest_bitness(m.cpu_is_64bit());
        out(m, manager.direct_sound_create(m.arg_ptr(0), m.arg_ptr(1), m.arg_ptr(2), true));
    });
    for (bool wide : {false, true}) {
        add_export(wide ? "DirectSoundEnumerateW" : "DirectSoundEnumerateA", 8,
            [&manager, wide](Marshaler& m) {
                m.set_return(manager.enumerate(m.arg_ptr(0), m.arg_ptr(1), wide,
                                               false, m.cpu_is_64bit()));
            });
        add_export(wide ? "DirectSoundCaptureEnumerateW" : "DirectSoundCaptureEnumerateA", 8,
            [&manager, wide](Marshaler& m) {
                m.set_return(manager.enumerate(m.arg_ptr(0), m.arg_ptr(1), wide,
                                               true, m.cpu_is_64bit()));
            });
    }
    for (bool version8 : {false, true}) {
        add_export(version8 ? "DirectSoundCaptureCreate8" : "DirectSoundCaptureCreate", 12,
            [&manager, version8](Marshaler& m) {
                manager.note_guest_bitness(m.cpu_is_64bit());
                m.set_return(manager.capture_create(m.arg_ptr(0), m.arg_ptr(1), m.arg_ptr(2),
                                                    version8));
            });
    }
    add_export("GetDeviceID", 8, [&manager](Marshaler& m) {
        m.set_return(manager.get_device_id(m.arg_ptr(0), m.arg_ptr(1)));
    });
    // HRESULT DirectSoundFullDuplexCreate(LPCGUID, LPCGUID, LPCDSCBUFFERDESC,
    //   LPCDSBUFFERDESC, HWND, DWORD, LPDIRECTSOUNDFULLDUPLEX*,
    //   LPDIRECTSOUNDCAPTUREBUFFER8*, LPDIRECTSOUNDBUFFER8*, LPUNKNOWN)
    add_export("DirectSoundFullDuplexCreate", 40, [&manager](Marshaler& m) {
        manager.note_guest_bitness(m.cpu_is_64bit());
        m.set_return(manager.full_duplex_create(
            m.arg_ptr(0), m.arg_ptr(1), m.arg_ptr(2), m.arg_ptr(3), m.arg(4),
            m.arg32(5), m.arg_ptr(6), m.arg_ptr(7), m.arg_ptr(8), m.arg_ptr(9)));
    });
    add_export("DllGetClassObject", 12, [&manager](Marshaler& m) {
        manager.note_guest_bitness(m.cpu_is_64bit());
        m.set_return(manager.dll_get_class_object(m.arg_ptr(0), m.arg_ptr(1), m.arg_ptr(2)));
    });
    // S_FALSE: class objects may still be in use (the module stays loaded).
    add_export("DllCanUnloadNow", 0, [](Marshaler& m) { m.set_return(1u); });

    auto method = [&](const char* name, u16 stdcall_bytes, ApiHandler handler) {
        exports.entries.push_back({name, 0, std::move(handler), stdcall_bytes});
    };

    // -- IDirectSound8 --
    method("Vitreo_DSound_QueryInterface", 12, [&manager](Marshaler& m) {
        m.set_return(manager.query_interface(m.arg_ptr(0), m.arg_ptr(1), m.arg_ptr(2))); });
    method("Vitreo_DSound_AddRef", 4, [&manager](Marshaler& m) {
        m.set_return(manager.add_ref(m.arg_ptr(0))); });
    method("Vitreo_DSound_Release", 4, [&manager](Marshaler& m) {
        m.set_return(manager.release(m.arg_ptr(0))); });
    method("Vitreo_DSound_CreateSoundBuffer", 16, [&manager](Marshaler& m) {
        m.set_return(manager.create_sound_buffer(m.arg_ptr(0), m.arg_ptr(1), m.arg_ptr(2),
                                                 m.arg_ptr(3))); });
    method("Vitreo_DSound_GetCaps", 8, [&manager](Marshaler& m) {
        m.set_return(manager.get_caps(m.arg_ptr(0), m.arg_ptr(1))); });
    method("Vitreo_DSound_DuplicateSoundBuffer", 12, [&manager](Marshaler& m) {
        m.set_return(manager.duplicate_sound_buffer(m.arg_ptr(0), m.arg_ptr(1), m.arg_ptr(2))); });
    method("Vitreo_DSound_SetCooperativeLevel", 12, [&manager](Marshaler& m) {
        m.set_return(manager.set_cooperative_level(m.arg_ptr(0), m.arg(1), m.arg32(2))); });
    // Compact only matters for hardware buffer memory, which Vitreo has none of.
    method("Vitreo_DSound_Compact", 4, [](Marshaler& m) { m.set_return(dsound::DS_OK); });
    method("Vitreo_DSound_GetSpeakerConfig", 8, [](Marshaler& m) {
        // DSSPEAKER_STEREO (4) with DSSPEAKER_GEOMETRY_WIDE (20) in the high
        // word: DSSPEAKER_COMBINED(DSSPEAKER_STEREO, DSSPEAKER_GEOMETRY_WIDE).
        constexpr u32 DSSPEAKER_STEREO_WIDE = 4u | (20u << 16);
        const GuestAddr out_ptr = m.arg_ptr(1);
        if (out_ptr == 0) { m.set_return(dsound::E_INVALIDARG); return; }
        (void)m.write_u32(out_ptr, DSSPEAKER_STEREO_WIDE);
        m.set_return(dsound::DS_OK);
    });
    method("Vitreo_DSound_SetSpeakerConfig", 8, [](Marshaler& m) {
        // Accepted and recorded by the driver on Windows; Vitreo always
        // renders through the host's own speaker configuration.
        m.set_return(dsound::DS_OK);
    });
    method("Vitreo_DSound_Initialize", 8, [](Marshaler& m) {
        // DirectSoundCreate already initialized the object.
        m.set_return(dsound::DSERR_ALREADYINITIALIZED);
    });
    method("Vitreo_DSound_VerifyCertification", 8, [](Marshaler& m) {
        // Learn: DS_CERTIFIED or DS_UNCERTIFIED; the software mixer is not a
        // certified driver.
        const GuestAddr out_ptr = m.arg_ptr(1);
        if (out_ptr == 0) { m.set_return(dsound::E_INVALIDARG); return; }
        (void)m.write_u32(out_ptr, DS_UNCERTIFIED);
        m.set_return(dsound::DS_OK);
    });

    // -- IDirectSoundBuffer8 --
    method("Vitreo_DSBuffer_QueryInterface", 12, [&manager](Marshaler& m) {
        m.set_return(manager.buffer_query_interface(m.arg_ptr(0), m.arg_ptr(1), m.arg_ptr(2))); });
    method("Vitreo_DSBuffer_AddRef", 4, [&manager](Marshaler& m) {
        m.set_return(manager.buffer_add_ref(m.arg_ptr(0))); });
    method("Vitreo_DSBuffer_Release", 4, [&manager](Marshaler& m) {
        m.set_return(manager.buffer_release(m.arg_ptr(0))); });
    method("Vitreo_DSBuffer_GetCaps", 8, [&manager](Marshaler& m) {
        m.set_return(manager.buffer_get_caps(m.arg_ptr(0), m.arg_ptr(1))); });
    method("Vitreo_DSBuffer_GetCurrentPosition", 12, [&manager](Marshaler& m) {
        m.set_return(manager.buffer_get_current_position(m.arg_ptr(0), m.arg_ptr(1),
                                                         m.arg_ptr(2))); });
    method("Vitreo_DSBuffer_GetFormat", 16, [&manager](Marshaler& m) {
        m.set_return(manager.buffer_get_format(m.arg_ptr(0), m.arg_ptr(1), m.arg32(2),
                                               m.arg_ptr(3))); });
    method("Vitreo_DSBuffer_GetVolume", 8, [&manager](Marshaler& m) {
        m.set_return(manager.buffer_get_volume(m.arg_ptr(0), m.arg_ptr(1))); });
    method("Vitreo_DSBuffer_GetPan", 8, [&manager](Marshaler& m) {
        m.set_return(manager.buffer_get_pan(m.arg_ptr(0), m.arg_ptr(1))); });
    method("Vitreo_DSBuffer_GetFrequency", 8, [&manager](Marshaler& m) {
        m.set_return(manager.buffer_get_frequency(m.arg_ptr(0), m.arg_ptr(1))); });
    method("Vitreo_DSBuffer_GetStatus", 8, [&manager](Marshaler& m) {
        m.set_return(manager.buffer_get_status(m.arg_ptr(0), m.arg_ptr(1))); });
    method("Vitreo_DSBuffer_Initialize", 12, [](Marshaler& m) {
        m.set_return(dsound::DSERR_ALREADYINITIALIZED); });
    method("Vitreo_DSBuffer_Lock", 32, [&manager](Marshaler& m) {
        m.set_return(manager.buffer_lock(m.arg_ptr(0), m.arg32(1), m.arg32(2), m.arg_ptr(3),
                                         m.arg_ptr(4), m.arg_ptr(5), m.arg_ptr(6),
                                         m.arg32(7))); });
    method("Vitreo_DSBuffer_Play", 16, [&manager](Marshaler& m) {
        m.set_return(manager.buffer_play(m.arg_ptr(0), m.arg32(1), m.arg32(2), m.arg32(3))); });
    method("Vitreo_DSBuffer_SetCurrentPosition", 8, [&manager](Marshaler& m) {
        m.set_return(manager.buffer_set_current_position(m.arg_ptr(0), m.arg32(1))); });
    method("Vitreo_DSBuffer_SetFormat", 8, [&manager](Marshaler& m) {
        m.set_return(manager.buffer_set_format(m.arg_ptr(0), m.arg_ptr(1))); });
    method("Vitreo_DSBuffer_SetVolume", 8, [&manager](Marshaler& m) {
        m.set_return(manager.buffer_set_volume(m.arg_ptr(0), static_cast<i32>(m.arg32(1)))); });
    method("Vitreo_DSBuffer_SetPan", 8, [&manager](Marshaler& m) {
        m.set_return(manager.buffer_set_pan(m.arg_ptr(0), static_cast<i32>(m.arg32(1)))); });
    method("Vitreo_DSBuffer_SetFrequency", 8, [&manager](Marshaler& m) {
        m.set_return(manager.buffer_set_frequency(m.arg_ptr(0), m.arg32(1))); });
    method("Vitreo_DSBuffer_Stop", 4, [&manager](Marshaler& m) {
        m.set_return(manager.buffer_stop(m.arg_ptr(0))); });
    method("Vitreo_DSBuffer_Unlock", 20, [&manager](Marshaler& m) {
        m.set_return(manager.buffer_unlock(m.arg_ptr(0), m.arg_ptr(1), m.arg32(2),
                                           m.arg_ptr(3), m.arg32(4))); });
    method("Vitreo_DSBuffer_Restore", 4, [&manager](Marshaler& m) {
        m.set_return(manager.buffer_restore(m.arg_ptr(0))); });
    method("Vitreo_DSBuffer_SetFX", 16, [&manager](Marshaler& m) {
        m.set_return(manager.buffer_set_fx(m.arg_ptr(0), m.arg32(1), m.arg_ptr(2),
                                           m.arg_ptr(3))); });
    method("Vitreo_DSBuffer_AcquireResources", 16, [&manager](Marshaler& m) {
        m.set_return(manager.buffer_acquire_resources(m.arg_ptr(0), m.arg32(1), m.arg32(2),
                                                      m.arg_ptr(3))); });
    method("Vitreo_DSBuffer_GetObjectInPath", 20, [&manager](Marshaler& m) {
        m.set_return(manager.buffer_get_object_in_path(m.arg_ptr(0), m.arg_ptr(1), m.arg32(2),
                                                       m.arg_ptr(3), m.arg_ptr(4))); });

    // -- IDirectSound3DListener (D3DVALUE arguments are floats: XMM on x64) --
    auto fbits = [](Marshaler& m, usize index) { return float_bits(m.arg_f32(index)); };
    method("Vitreo_DS3DListener_QueryInterface", 12, [&manager](Marshaler& m) {
        m.set_return(manager.listener_query_interface(m.arg_ptr(0), m.arg_ptr(1),
                                                      m.arg_ptr(2))); });
    method("Vitreo_DS3DListener_AddRef", 4, [&manager](Marshaler& m) {
        m.set_return(manager.listener_add_ref(m.arg_ptr(0))); });
    method("Vitreo_DS3DListener_Release", 4, [&manager](Marshaler& m) {
        m.set_return(manager.listener_release(m.arg_ptr(0))); });
    method("Vitreo_DS3DListener_GetAllParameters", 8, [&manager](Marshaler& m) {
        m.set_return(manager.listener_get_all_parameters(m.arg_ptr(0), m.arg_ptr(1))); });
    method("Vitreo_DS3DListener_GetDistanceFactor", 8, [&manager](Marshaler& m) {
        m.set_return(manager.listener_get_distance_factor(m.arg_ptr(0), m.arg_ptr(1))); });
    method("Vitreo_DS3DListener_GetDopplerFactor", 8, [&manager](Marshaler& m) {
        m.set_return(manager.listener_get_doppler_factor(m.arg_ptr(0), m.arg_ptr(1))); });
    method("Vitreo_DS3DListener_GetOrientation", 12, [&manager](Marshaler& m) {
        m.set_return(manager.listener_get_orientation(m.arg_ptr(0), m.arg_ptr(1),
                                                      m.arg_ptr(2))); });
    method("Vitreo_DS3DListener_GetPosition", 8, [&manager](Marshaler& m) {
        m.set_return(manager.listener_get_position(m.arg_ptr(0), m.arg_ptr(1))); });
    method("Vitreo_DS3DListener_GetRolloffFactor", 8, [&manager](Marshaler& m) {
        m.set_return(manager.listener_get_rolloff_factor(m.arg_ptr(0), m.arg_ptr(1))); });
    method("Vitreo_DS3DListener_GetVelocity", 8, [&manager](Marshaler& m) {
        m.set_return(manager.listener_get_velocity(m.arg_ptr(0), m.arg_ptr(1))); });
    method("Vitreo_DS3DListener_SetAllParameters", 12, [&manager](Marshaler& m) {
        m.set_return(manager.listener_set_all_parameters(m.arg_ptr(0), m.arg_ptr(1),
                                                         m.arg32(2))); });
    method("Vitreo_DS3DListener_SetDistanceFactor", 12, [&manager, fbits](Marshaler& m) {
        m.set_return(manager.listener_set_distance_factor(m.arg_ptr(0), fbits(m, 1),
                                                          m.arg32(2))); });
    method("Vitreo_DS3DListener_SetDopplerFactor", 12, [&manager, fbits](Marshaler& m) {
        m.set_return(manager.listener_set_doppler_factor(m.arg_ptr(0), fbits(m, 1),
                                                         m.arg32(2))); });
    method("Vitreo_DS3DListener_SetOrientation", 32, [&manager, fbits](Marshaler& m) {
        m.set_return(manager.listener_set_orientation(
            m.arg_ptr(0), fbits(m, 1), fbits(m, 2), fbits(m, 3), fbits(m, 4), fbits(m, 5),
            fbits(m, 6), m.arg32(7))); });
    method("Vitreo_DS3DListener_SetPosition", 20, [&manager, fbits](Marshaler& m) {
        m.set_return(manager.listener_set_position(m.arg_ptr(0), fbits(m, 1), fbits(m, 2),
                                                   fbits(m, 3), m.arg32(4))); });
    method("Vitreo_DS3DListener_SetRolloffFactor", 12, [&manager, fbits](Marshaler& m) {
        m.set_return(manager.listener_set_rolloff_factor(m.arg_ptr(0), fbits(m, 1),
                                                         m.arg32(2))); });
    method("Vitreo_DS3DListener_SetVelocity", 20, [&manager, fbits](Marshaler& m) {
        m.set_return(manager.listener_set_velocity(m.arg_ptr(0), fbits(m, 1), fbits(m, 2),
                                                   fbits(m, 3), m.arg32(4))); });
    method("Vitreo_DS3DListener_CommitDeferredSettings", 4, [&manager](Marshaler& m) {
        m.set_return(manager.listener_commit_deferred(m.arg_ptr(0))); });

    // -- IDirectSoundCapture8 (and its IUnknown facet) --
    method("Vitreo_DSCapture_QueryInterface", 12, [&manager](Marshaler& m) {
        m.set_return(manager.capture_query_interface(m.arg_ptr(0), m.arg_ptr(1),
                                                     m.arg_ptr(2))); });
    method("Vitreo_DSCapture_AddRef", 4, [&manager](Marshaler& m) {
        m.set_return(manager.capture_add_ref(m.arg_ptr(0))); });
    method("Vitreo_DSCapture_Release", 4, [&manager](Marshaler& m) {
        m.set_return(manager.capture_release(m.arg_ptr(0))); });
    method("Vitreo_DSCapture_CreateCaptureBuffer", 16, [&manager](Marshaler& m) {
        m.set_return(manager.capture_create_buffer(m.arg_ptr(0), m.arg_ptr(1), m.arg_ptr(2),
                                                   m.arg_ptr(3))); });
    method("Vitreo_DSCapture_GetCaps", 8, [&manager](Marshaler& m) {
        m.set_return(manager.capture_get_caps(m.arg_ptr(0), m.arg_ptr(1))); });
    method("Vitreo_DSCapture_Initialize", 8, [&manager](Marshaler& m) {
        m.set_return(manager.capture_initialize(m.arg_ptr(0), m.arg_ptr(1))); });

    // -- IDirectSoundCaptureBuffer8 (and its IUnknown facet) --
    method("Vitreo_DSCBuffer_QueryInterface", 12, [&manager](Marshaler& m) {
        m.set_return(manager.cbuffer_query_interface(m.arg_ptr(0), m.arg_ptr(1),
                                                     m.arg_ptr(2))); });
    method("Vitreo_DSCBuffer_AddRef", 4, [&manager](Marshaler& m) {
        m.set_return(manager.cbuffer_add_ref(m.arg_ptr(0))); });
    method("Vitreo_DSCBuffer_Release", 4, [&manager](Marshaler& m) {
        m.set_return(manager.cbuffer_release(m.arg_ptr(0))); });
    method("Vitreo_DSCBuffer_GetCaps", 8, [&manager](Marshaler& m) {
        m.set_return(manager.cbuffer_get_caps(m.arg_ptr(0), m.arg_ptr(1))); });
    method("Vitreo_DSCBuffer_GetCurrentPosition", 12, [&manager](Marshaler& m) {
        m.set_return(manager.cbuffer_get_current_position(m.arg_ptr(0), m.arg_ptr(1),
                                                          m.arg_ptr(2))); });
    method("Vitreo_DSCBuffer_GetFormat", 16, [&manager](Marshaler& m) {
        m.set_return(manager.cbuffer_get_format(m.arg_ptr(0), m.arg_ptr(1), m.arg32(2),
                                                m.arg_ptr(3))); });
    method("Vitreo_DSCBuffer_GetStatus", 8, [&manager](Marshaler& m) {
        m.set_return(manager.cbuffer_get_status(m.arg_ptr(0), m.arg_ptr(1))); });
    method("Vitreo_DSCBuffer_Initialize", 12, [&manager](Marshaler& m) {
        m.set_return(manager.cbuffer_initialize(m.arg_ptr(0), m.arg_ptr(1), m.arg_ptr(2))); });
    method("Vitreo_DSCBuffer_Lock", 32, [&manager](Marshaler& m) {
        m.set_return(manager.cbuffer_lock(m.arg_ptr(0), m.arg32(1), m.arg32(2), m.arg_ptr(3),
                                          m.arg_ptr(4), m.arg_ptr(5), m.arg_ptr(6),
                                          m.arg32(7))); });
    method("Vitreo_DSCBuffer_Start", 8, [&manager](Marshaler& m) {
        m.set_return(manager.cbuffer_start(m.arg_ptr(0), m.arg32(1))); });
    method("Vitreo_DSCBuffer_Stop", 4, [&manager](Marshaler& m) {
        m.set_return(manager.cbuffer_stop(m.arg_ptr(0))); });
    method("Vitreo_DSCBuffer_Unlock", 20, [&manager](Marshaler& m) {
        m.set_return(manager.cbuffer_unlock(m.arg_ptr(0), m.arg_ptr(1), m.arg32(2),
                                            m.arg_ptr(3), m.arg32(4))); });
    method("Vitreo_DSCBuffer_GetObjectInPath", 20, [&manager](Marshaler& m) {
        m.set_return(manager.cbuffer_get_object_in_path(m.arg_ptr(0), m.arg_ptr(1),
                                                        m.arg32(2), m.arg_ptr(3),
                                                        m.arg_ptr(4))); });
    method("Vitreo_DSCBuffer_GetFXStatus", 12, [&manager](Marshaler& m) {
        m.set_return(manager.cbuffer_get_fx_status(m.arg_ptr(0), m.arg32(1), m.arg_ptr(2))); });

    // -- IDirectSoundNotify --
    method("Vitreo_DSNotify_QueryInterface", 12, [&manager](Marshaler& m) {
        m.set_return(manager.notify_query_interface(m.arg_ptr(0), m.arg_ptr(1),
                                                    m.arg_ptr(2))); });
    method("Vitreo_DSNotify_AddRef", 4, [&manager](Marshaler& m) {
        m.set_return(manager.notify_add_ref(m.arg_ptr(0))); });
    method("Vitreo_DSNotify_Release", 4, [&manager](Marshaler& m) {
        m.set_return(manager.notify_release(m.arg_ptr(0))); });
    method("Vitreo_DSNotify_SetNotificationPositions", 12, [&manager](Marshaler& m) {
        m.set_return(manager.notify_set_positions(m.arg_ptr(0), m.arg32(1), m.arg_ptr(2))); });

    // -- IDirectSoundFullDuplex --
    method("Vitreo_DSFullDuplex_QueryInterface", 12, [&manager](Marshaler& m) {
        m.set_return(manager.full_duplex_query_interface(m.arg_ptr(0), m.arg_ptr(1),
                                                         m.arg_ptr(2))); });
    method("Vitreo_DSFullDuplex_AddRef", 4, [&manager](Marshaler& m) {
        m.set_return(manager.full_duplex_add_ref(m.arg_ptr(0))); });
    method("Vitreo_DSFullDuplex_Release", 4, [&manager](Marshaler& m) {
        m.set_return(manager.full_duplex_release(m.arg_ptr(0))); });
    method("Vitreo_DSFullDuplex_Initialize", 36, [&manager](Marshaler& m) {
        m.set_return(manager.full_duplex_initialize(
            m.arg_ptr(0), m.arg_ptr(1), m.arg_ptr(2), m.arg_ptr(3), m.arg_ptr(4), m.arg(5),
            m.arg32(6), m.arg_ptr(7), m.arg_ptr(8))); });

    // -- IClassFactory --
    method("Vitreo_DSClassFactory_QueryInterface", 12, [&manager](Marshaler& m) {
        m.set_return(manager.factory_query_interface(m.arg_ptr(0), m.arg_ptr(1),
                                                     m.arg_ptr(2))); });
    method("Vitreo_DSClassFactory_AddRef", 4, [&manager](Marshaler& m) {
        m.set_return(manager.factory_add_ref(m.arg_ptr(0))); });
    method("Vitreo_DSClassFactory_Release", 4, [&manager](Marshaler& m) {
        m.set_return(manager.factory_release(m.arg_ptr(0))); });
    method("Vitreo_DSClassFactory_CreateInstance", 16, [&manager](Marshaler& m) {
        m.set_return(manager.factory_create_instance(m.arg_ptr(0), m.arg_ptr(1), m.arg_ptr(2),
                                                     m.arg_ptr(3))); });
    method("Vitreo_DSClassFactory_LockServer", 8, [&manager](Marshaler& m) {
        m.set_return(manager.factory_lock_server(m.arg_ptr(0), m.arg32(1))); });

    return exports;
}

} // namespace vitreo::winapi
