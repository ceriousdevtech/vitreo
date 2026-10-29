/// @file winmm_midi_mixer.cpp
/// @brief MIDI (no ports) and mixer (no device) contracts.
///
/// Split out of winmm.cpp.

#include "winapi/media/winmm/winmm_internal.hpp"
#include "audio/contracts/audio_capture.hpp"
#include <algorithm>
#include <mutex>

namespace vitreo::winapi {

// ---------------------------------------------------------------------------
// Mixer objects. Vitreo exposes no mixer device (mixerGetNumDevs 0).
// ---------------------------------------------------------------------------

namespace {

/// MIDIOUTCAPSA/W of the MIDI mapper (s7_winmm_probe info mo_caps_mapper).
[[nodiscard]] std::vector<u8> build_midi_mapper_caps(bool wide) {
    const usize name_bytes = wide ? 64 : 32;
    std::vector<u8> b(8 + name_bytes + 12, 0);
    put16(b, 0, MM_MICROSOFT);
    put16(b, 2, MM_MIDI_MAPPER);
    put32(b, 4, 0x0500);
    put_name(b, 8, kMidiMapperName, wide);
    put16(b, 8 + name_bytes, MOD_MAPPER);
    put16(b, 10 + name_bytes, 0);        // wVoices
    put16(b, 12 + name_bytes, 0);        // wNotes
    put16(b, 14 + name_bytes, 0xFFFF);   // wChannelMask
    put32(b, 16 + name_bytes, MIDICAPS_VOLUME | MIDICAPS_STREAM);
    return b;
}

/// Packed (pshpack1) mixer structure sizes, s7_winmm_probe `sizes3`.
struct MixerSizes {
    u32 line_a, line_w, line_controls, control_a, control_w, details;
};
constexpr MixerSizes kMixer32{168, 280, 24, 148, 228, 24};
constexpr MixerSizes kMixer64{172, 284, 28, 148, 228, 32};
constexpr u32 kListTextA = 72;
constexpr u32 kListTextW = 136;

} // namespace

// The status a mixer call reports for the object `hmxobj` + MIXER_OBJECTF_*
// names (s7_winmm_probe mx_*): an unknown object type is INVALPARAM
// (mx_open_badflag / mx_getid_badflag), a bad id BADDEVICEID, a bad handle
// INVALHANDLE. A valid waveOut device or handle has no mixer:
// MMSYSERR_NODRIVER (Learn mixerOpen / mixerGetID).
u32 WinmmManager::mixer_object_status(u64 object, u32 flags) {
    const u32 type = flags & MIXER_OBJECTF_TYPEMASK;
    const bool handle = (flags & MIXER_OBJECTF_HANDLE) != 0;
    if (type > MIXER_OBJECTF_AUX) return MMSYSERR_INVALPARAM;
    if (type == MIXER_OBJECTF_WAVEOUT) {
        if (handle) {
            return find_stream(object) ? MMSYSERR_NODRIVER
                                       : MMSYSERR_INVALHANDLE;
        }
        return object < wave_device_count() ? MMSYSERR_NODRIVER
                                            : MMSYSERR_BADDEVICEID;
    }
    if (type == MIXER_OBJECTF_WAVEIN) {
        if (handle) {
            return find_wavein(object) ? MMSYSERR_NODRIVER : MMSYSERR_INVALHANDLE;
        }
        const u64 count = m_capture ? m_capture->devices().size() : 0;
        return object < count ? MMSYSERR_NODRIVER : MMSYSERR_BADDEVICEID;
    }
    if (type == MIXER_OBJECTF_AUX && handle) return MMSYSERR_INVALPARAM;
    // Mixers, midiOut, midiIn and aux: no devices, no handles.
    (void)MIXER_OBJECTF_MIXER;
    return handle ? MMSYSERR_INVALHANDLE : MMSYSERR_BADDEVICEID;
}

// mixerOpen(LPHMIXER phmx, UINT uMxId, DWORD_PTR dwCallback,
//           DWORD_PTR dwInstance, DWORD fdwOpen)
void WinmmManager::impl_mixerOpen(Marshaler& m) {
    const GuestAddr phmx = m.arg_ptr(0);
    const u64 object = m.arg(1) & 0xFFFFFFFFull;
    const u64 callback = m.arg(2);
    const u32 flags = m.arg32(4);
    if (phmx == 0) { // mx_open_null
        m.set_return(MMSYSERR_INVALPARAM);
        return;
    }
    // Every failure stores a NULL HMIXER, pointer width (mx_open_bad /
    // badwaveout / badwaveid / badflag / badwnd: h=0 for five different
    // codes). Learn's "-1" note for MMSYSERR_NODRIVER names "the location
    // referenced by uMxId", which only exists for mixerGetID.
    const auto finish = [&](u32 code) {
        (void)m.write_ptr(phmx, 0);
        m.set_return(code);
    };
    // Only MIXER_OBJECTF_* and CALLBACK_WINDOW are defined (mx_open_flag1
    // and mx_open_cbfunc: MMSYSERR_INVALFLAG).
    if ((flags & ~(MIXER_OBJECTF_HANDLE | MIXER_OBJECTF_TYPEMASK |
                   CALLBACK_WINDOW)) != 0) {
        finish(MMSYSERR_INVALFLAG);
        return;
    }
    if ((flags & CALLBACK_WINDOW) != 0 && callback != 0) {
        WindowValidator validator;
        {
            std::lock_guard<std::mutex> guard(m_wave_mutex);
            validator = m_window_validator;
        }
        if (validator && !validator(callback)) { // mx_open_badwnd
            finish(MMSYSERR_INVALPARAM);
            return;
        }
    }
    finish(mixer_object_status(object, flags));
}

namespace winmm_internal {

void mixer_get_line_info(WinmmManager& mgr, Marshaler& m, bool wide) {
    const u64 object = m.arg(0) & 0xFFFFFFFFull;
    const GuestAddr pmxl = m.arg_ptr(1);
    const u32 flags = m.arg32(2);
    const MixerSizes& sizes = m.cpu_is_64bit() ? kMixer64 : kMixer32;
    // mx_line_a_null / mx_line_a_cb0 / mx_line_w_cb_a: cbStruct >= sizeof.
    if (pmxl == 0 ||
        m.read_u32(pmxl).value_or(0) < (wide ? sizes.line_w : sizes.line_a)) {
        m.set_return(MMSYSERR_INVALPARAM);
        return;
    }
    // MIXER_GETLINEINFOF_DESTINATION..TARGETTYPE are 0..4 (mx_line_a_badquery).
    if ((flags & 0xF) > 4 ||
        (flags & ~(0xFu | MIXER_OBJECTF_HANDLE | MIXER_OBJECTF_TYPEMASK)) != 0) {
        m.set_return(MMSYSERR_INVALFLAG);
        return;
    }
    m.set_return(mgr.mixer_object_status(object, flags & 0xF0000000u));
}

void mixer_get_line_controls(WinmmManager& mgr, Marshaler& m, bool wide) {
    const u64 object = m.arg(0) & 0xFFFFFFFFull;
    const GuestAddr p = m.arg_ptr(1);
    const u32 flags = m.arg32(2);
    const MixerSizes& sizes = m.cpu_is_64bit() ? kMixer64 : kMixer32;
    // MIXERLINECONTROLS: cbStruct@0 dwLineID@4 dwControlID/Type@8
    // cControls@12 cbmxctrl@16 pamxctrl@20. A NULL pamxctrl is not checked
    // before the object (mx_ctl_a_nullarr r=2).
    if (p == 0 || m.read_u32(p).value_or(0) < sizes.line_controls ||
        m.read_u32(p + 12).value_or(0) == 0 ||
        m.read_u32(p + 16).value_or(0) <
            (wide ? sizes.control_w : sizes.control_a)) {
        m.set_return(MMSYSERR_INVALPARAM);
        return;
    }
    // MIXER_GETLINECONTROLSF_ALL / ONEBYID / ONEBYTYPE (mx_ctl_a_badquery).
    if ((flags & 0xF) > 2 ||
        (flags & ~(0xFu | MIXER_OBJECTF_HANDLE | MIXER_OBJECTF_TYPEMASK)) != 0) {
        m.set_return(MMSYSERR_INVALFLAG);
        return;
    }
    m.set_return(mgr.mixer_object_status(object, flags & 0xF0000000u));
}

void mixer_control_details(WinmmManager& mgr, Marshaler& m, bool wide,
                           bool set) {
    const u64 object = m.arg(0) & 0xFFFFFFFFull;
    const GuestAddr p = m.arg_ptr(1);
    const u32 flags = m.arg32(2);
    const bool x64 = m.cpu_is_64bit();
    const MixerSizes& sizes = x64 ? kMixer64 : kMixer32;
    // MIXERCONTROLDETAILS: cbStruct@0 dwControlID@4 cChannels@8
    // {HWND hwndOwner | DWORD cMultipleItems}@12, then cbDetails and
    // paDetails (@16/@20 on x86, @20/@24 on x64).
    if (p == 0 || m.read_u32(p).value_or(0) < sizes.details) {
        m.set_return(MMSYSERR_INVALPARAM);
        return;
    }
    const u32 query = flags & 0xF;
    // Get: VALUE 0 / LISTTEXT 1. Set: VALUE 0 / CUSTOM 1.
    if (query > 1 ||
        (flags & ~(0xFu | MIXER_OBJECTF_HANDLE | MIXER_OBJECTF_TYPEMASK)) != 0) {
        m.set_return(MMSYSERR_INVALFLAG);
        return;
    }
    const u32 cb_details = m.read_u32(p + (x64 ? 20 : 16)).value_or(0);
    const u64 details = m.read_ptr(p + (x64 ? 24 : 20)).value_or(0);
    // mx_det_nullarr / cbDetails 0 / mx_det_listtext with a 4-byte element.
    const u32 minimum = (!set && query == 1) ? (wide ? kListTextW : kListTextA)
                                             : 4u;
    if (details == 0 || cb_details < minimum) {
        m.set_return(MMSYSERR_INVALPARAM);
        return;
    }
    m.set_return(mgr.mixer_object_status(object, flags & 0xF0000000u));
}

/// midiOut/midiIn/mixer GetDevCaps with no device: cbSize 0 succeeds without
/// writing, a NULL buffer is INVALPARAM, every id BADDEVICEID
/// (mo_caps_a_cb0 / mo_caps_a_null / mo_caps_a_bad, mx_caps_*, mi_caps_*).
void no_device_caps(Marshaler& m) {
    if (m.arg32(2) == 0) {
        m.set_return(MMSYSERR_NOERROR);
        return;
    }
    if (m.arg_ptr(1) == 0) {
        m.set_return(MMSYSERR_INVALPARAM);
        return;
    }
    m.set_return(MMSYSERR_BADDEVICEID);
}

/// midiOutGetDevCapsA/W: no MIDI output port, only the MIDI mapper, whose
/// caps are copied min(cbmoc, sizeof) (info mo_caps_*_cb8 copies 8 bytes).
void midi_out_get_dev_caps(Marshaler& m, bool wide) {
    const u64 id = m.arg(0);
    const GuestAddr p = m.arg_ptr(1);
    const u32 cb = m.arg32(2);
    if (cb == 0) {
        m.set_return(MMSYSERR_NOERROR);
        return;
    }
    if (p == 0) {
        m.set_return(MMSYSERR_INVALPARAM);
        return;
    }
    if (!is_mapper_id(id)) {
        m.set_return(MMSYSERR_BADDEVICEID);
        return;
    }
    const std::vector<u8> caps = build_midi_mapper_caps(wide);
    (void)m.memory().write_bytes(p, reinterpret_cast<const Byte*>(caps.data()),
                                 std::min<usize>(cb, caps.size()));
    m.set_return(MMSYSERR_NOERROR);
}

/// midiOutOpen / midiInOpen / midiStreamOpen with no port: a NULL handle
/// pointer is INVALPARAM; otherwise *phandle = NULL and the id is rejected.
/// Only output has a mapper, which reports MIDIERR_NODEVICE (Learn
/// midiOutOpen: "No MIDI port was found. This error occurs only when the
/// mapper is opened."); input ids are all BADDEVICEID (mi_open_mapper).
void midi_open(Marshaler& m, bool output, u32 id) {
    const GuestAddr ph = m.arg_ptr(0);
    if (ph == 0) { // mo_open_null
        m.set_return(MMSYSERR_INVALPARAM);
        return;
    }
    (void)m.write_ptr(ph, 0); // mo_open_bad / mi_open_bad h=0
    m.set_return(output && id == MIDI_MAPPER ? MIDIERR_NODEVICE
                                             : MMSYSERR_BADDEVICEID);
}

} // namespace winmm_internal

} // namespace vitreo::winapi
