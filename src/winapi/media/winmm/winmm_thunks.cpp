/// @file winmm_thunks.cpp
/// @brief The winmm.dll export table.
///
/// Split out of winmm.cpp.

#include "winapi/media/winmm/winmm_internal.hpp"
#include "graphics/common/perf_diag.hpp"
#include "winapi/common/timer_resolution.hpp"
#include <cstdio>
#include <cstdlib>

namespace vitreo::winapi {

// timeGetTime — returns milliseconds since system start
static void impl_timeGetTime(Marshaler& m) {
    // Counted because a guest that drives its simulation from this clock will
    // poll it far more often than it renders; the call rate relative to the
    // frame rate says whether the game loop is time-driven or frame-driven.
    perf::count(perf::Counter::mm_time_get_time);
    m.set_return(time_get_time());
}

DllExports create_winmm_thunks(WinmmManager& mgr) {
    DllExports exports;

    // ---- PlaySound ---------------------------------------------------------
    exports.entries.push_back(ExportEntry{"PlaySoundW", 0, [&mgr](Marshaler& m) { mgr.impl_PlaySoundW(m); }, 12});
    exports.entries.push_back(ExportEntry{"PlaySoundA", 0, [&mgr](Marshaler& m) { mgr.impl_PlaySoundA(m); }, 12});
    // Undecorated PlaySound is the ANSI entry point (winmm.lib PlaySound@12).
    exports.entries.push_back(ExportEntry{"PlaySound", 0, [&mgr](Marshaler& m) { mgr.impl_PlaySoundA(m); }, 12});
    exports.entries.push_back(ExportEntry{"sndPlaySoundA", 0, [&mgr](Marshaler& m) { mgr.impl_sndPlaySoundA(m); }, 8});
    exports.entries.push_back(ExportEntry{"sndPlaySoundW", 0, [&mgr](Marshaler& m) { mgr.impl_sndPlaySoundW(m); }, 8});

    // timeGetTime — 0 args, returns DWORD ms
    exports.entries.push_back(ExportEntry{"timeGetTime", 0, impl_timeGetTime, 0});

    // ---- mmio --------------------------------------------------------------
    exports.entries.push_back(ExportEntry{"mmioOpenA", 0, [&mgr](Marshaler& m) { mgr.impl_mmioOpenA(m); }, 12});
    exports.entries.push_back(ExportEntry{"mmioClose", 0, [&mgr](Marshaler& m) { mgr.impl_mmioClose(m); }, 8});
    exports.entries.push_back(ExportEntry{"mmioDescend", 0, [&mgr](Marshaler& m) { mgr.impl_mmioDescend(m); }, 16});
    exports.entries.push_back(ExportEntry{"mmioRead", 0, [&mgr](Marshaler& m) { mgr.impl_mmioRead(m); }, 12});
    exports.entries.push_back(ExportEntry{"mmioAscend", 0, [&mgr](Marshaler& m) { mgr.impl_mmioAscend(m); }, 12});

    // ---- waveOut -----------------------------------------------------------
    exports.entries.push_back(ExportEntry{"waveOutGetNumDevs", 0, [&mgr](Marshaler& m) { mgr.impl_waveOutGetNumDevs(m); }, 0});
    exports.entries.push_back(ExportEntry{"waveOutGetDevCapsA", 0, [&mgr](Marshaler& m) { mgr.impl_waveOutGetDevCapsA(m); }, 12});
    exports.entries.push_back(ExportEntry{"waveOutGetDevCapsW", 0, [&mgr](Marshaler& m) { mgr.impl_waveOutGetDevCapsW(m); }, 12});
    exports.entries.push_back(ExportEntry{"waveOutOpen", 0, [&mgr](Marshaler& m) { mgr.impl_waveOutOpen(m); }, 24});
    exports.entries.push_back(ExportEntry{"waveOutClose", 0, [&mgr](Marshaler& m) { mgr.impl_waveOutClose(m); }, 4});
    exports.entries.push_back(ExportEntry{"waveOutPrepareHeader", 0, [&mgr](Marshaler& m) { mgr.impl_waveOutPrepareHeader(m); }, 12});
    exports.entries.push_back(ExportEntry{"waveOutUnprepareHeader", 0, [&mgr](Marshaler& m) { mgr.impl_waveOutUnprepareHeader(m); }, 12});
    exports.entries.push_back(ExportEntry{"waveOutWrite", 0, [&mgr](Marshaler& m) { mgr.impl_waveOutWrite(m); }, 12});
    exports.entries.push_back(ExportEntry{"waveOutReset", 0, [&mgr](Marshaler& m) { mgr.impl_waveOutReset(m); }, 4});
    exports.entries.push_back(ExportEntry{"waveOutPause", 0, [&mgr](Marshaler& m) { mgr.impl_waveOutPause(m); }, 4});
    exports.entries.push_back(ExportEntry{"waveOutRestart", 0, [&mgr](Marshaler& m) { mgr.impl_waveOutRestart(m); }, 4});
    exports.entries.push_back(ExportEntry{"waveOutGetPosition", 0, [&mgr](Marshaler& m) { mgr.impl_waveOutGetPosition(m); }, 12});
    exports.entries.push_back(ExportEntry{"waveOutGetID", 0, [&mgr](Marshaler& m) { mgr.impl_waveOutGetID(m); }, 8});
    exports.entries.push_back(ExportEntry{"waveOutMessage", 0, [&mgr](Marshaler& m) { mgr.impl_waveOutMessage(m); }, 16});
    exports.entries.push_back(ExportEntry{"waveOutSetVolume", 0, [&mgr](Marshaler& m) { mgr.impl_waveOutSetVolume(m); }, 8});
    exports.entries.push_back(ExportEntry{"waveOutGetVolume", 0, [&mgr](Marshaler& m) { mgr.impl_waveOutGetVolume(m); }, 8});
    exports.entries.push_back(ExportEntry{"waveOutBreakLoop", 0, [&mgr](Marshaler& m) { mgr.impl_waveOutBreakLoop(m); }, 4});
    for (const char* name : {"waveOutGetPitch", "waveOutSetPitch",
                             "waveOutGetPlaybackRate", "waveOutSetPlaybackRate"}) {
        exports.entries.push_back(ExportEntry{name, 0, [&mgr](Marshaler& m) { mgr.impl_waveOutUnsupportedControl(m); }, 8});
    }
    exports.entries.push_back(ExportEntry{"waveOutGetErrorTextA", 0, [](Marshaler& m) { error_text(m, false, false); }, 12});
    exports.entries.push_back(ExportEntry{"waveOutGetErrorTextW", 0, [](Marshaler& m) { error_text(m, true, false); }, 12});

    // mciSendCommandA(wDeviceID, uMsg, dwParam1, dwParam2) → MCIERROR
    exports.entries.push_back(ExportEntry{"mciSendCommandA", 0, [](Marshaler& m) {
        (void)m.arg(0); (void)m.arg(1); (void)m.arg(2); (void)m.arg(3);
        m.set_return(0u); // success
    }, 16});

    // timeBeginPeriod(uPeriod) -> TIMERR_NOERROR / TIMERR_NOCANDO
    //
    // Reference-counted process-wide, matching Windows: the effective
    // resolution is the smallest period any caller holds, and a period outside
    // the range timeGetDevCaps advertises is rejected rather than accepted and
    // ignored. Sleep and the kernel32 wait paths read the effective value.
    exports.entries.push_back({"timeBeginPeriod", 0, [](Marshaler& m) {
        const u32 period = static_cast<u32>(m.arg(0));
        perf::count(perf::Counter::mm_begin_period);
        const bool accepted = timer_resolution_acquire(period);
        VITREO_TRACE("[winmm] timeBeginPeriod period=%u -> %s (effective=%u)\n",
                     period, accepted ? "TIMERR_NOERROR" : "TIMERR_NOCANDO",
                     timer_resolution_effective());
        m.set_return(accepted ? TIMERR_NOERROR : TIMERR_NOCANDO);
    }, 4});

    // timeEndPeriod(uPeriod) -> TIMERR_NOERROR / TIMERR_NOCANDO
    exports.entries.push_back({"timeEndPeriod", 0, [](Marshaler& m) {
        const u32 period = static_cast<u32>(m.arg(0));
        perf::count(perf::Counter::mm_end_period);
        const bool released = timer_resolution_release(period);
        VITREO_TRACE("[winmm] timeEndPeriod period=%u -> %s (effective=%u)\n",
                     period, released ? "TIMERR_NOERROR" : "TIMERR_NOCANDO",
                     timer_resolution_effective());
        m.set_return(released ? TIMERR_NOERROR : TIMERR_NOCANDO);
    }, 4});

    // timeSetEvent(uDelay, uResolution, lpTimeProc, dwUser, fuEvent) -> timer id
    exports.entries.push_back({"timeSetEvent", 0, [&mgr](Marshaler& m) {
        const u32 delay = static_cast<u32>(m.arg(0));
        const u32 res = static_cast<u32>(m.arg(1));
        const auto proc = m.arg_ptr(2);
        // dwUser is DWORD_PTR: keep all 64 bits on x64 (Learn timeSetEvent).
        const u64 user = m.arg(3);
        const u32 flags = static_cast<u32>(m.arg(4));
        const u32 id = mgr.timer_create(delay, res, proc, user, flags);
        perf::count(perf::Counter::mm_timer_set);
        static const bool mm_timer_diag =
            std::getenv("VITREO_MM_TIMER_DIAG") != nullptr;
        if (mm_timer_diag) {
            std::fprintf(stderr,
                         "[mm-timer-diag] create delay=%u resolution=%u "
                         "proc=0x%08x user=0x%llx flags=0x%x -> id=%u\n",
                         delay, res, static_cast<u32>(proc),
                         static_cast<unsigned long long>(user), flags, id);
            std::fflush(stderr);
        }
        VITREO_TRACE("[winmm] timeSetEvent delay=%u res=%u proc=0x%08x user=0x%llx "
                     "fuEvent=0x%x (type=%s periodic=%d) -> id=%u\n",
                     delay, res, static_cast<u32>(proc),
                     static_cast<unsigned long long>(user), flags,
                     (flags & 0x30) == 0x10 ? "EVENT_SET" :
                     (flags & 0x30) == 0x20 ? "EVENT_PULSE" : "FUNCTION",
                     (flags & 0x1) ? 1 : 0, id);
        m.set_return(id);
    }, 20});

    // timeKillEvent(uTimerID) -> TIMERR_NOERROR / MMSYSERR_INVALPARAM
    exports.entries.push_back({"timeKillEvent", 0, [&mgr](Marshaler& m) {
        const u32 id = static_cast<u32>(m.arg(0));
        const bool killed = mgr.timer_destroy(id);
        perf::count(perf::Counter::mm_timer_kill);
        // An unknown id is TIMERR_NOCANDO on Windows 10 (winmm_timer_probe
        // kill_bad), not the MMSYSERR_INVALPARAM Learn lists.
        VITREO_TRACE("[winmm] timeKillEvent id=%u -> %s\n", id,
                     killed ? "TIMERR_NOERROR" : "TIMERR_NOCANDO");
        m.set_return(killed ? TIMERR_NOERROR : TIMERR_NOCANDO);
    }, 4});

    // timeGetDevCaps(ptc, cbtc) -> TIMERR_NOERROR with the supported period range
    exports.entries.push_back({"timeGetDevCaps", 0, [](Marshaler& m) {
        const auto caps = m.arg_ptr(0);
        const u32 size = static_cast<u32>(m.arg(1));
        // TIMECAPS { UINT wPeriodMin; UINT wPeriodMax; }
        // Learn: TIMERR_NOCANDO when ptc is NULL or cbtc is invalid
        // (winmm_timer_probe devcaps_null / devcaps_4 -> 0x61).
        if (caps == 0 || size < 8) {
            m.set_return(TIMERR_NOCANDO);
            return;
        }
        (void)m.write_u32(caps, 1);            // wPeriodMin: 1 ms
        (void)m.write_u32(caps + 4, 1000000);  // wPeriodMax: Windows' documented max
        m.set_return(TIMERR_NOERROR);
    }, 8});

    // ---- waveIn: the host capture devices (WinmmManager::impl_waveIn*) ------
    exports.entries.push_back(ExportEntry{"waveInGetNumDevs", 0,
        [&mgr](Marshaler& m) { mgr.impl_waveInGetNumDevs(m); }, 0});
    exports.entries.push_back(ExportEntry{"waveInGetDevCapsA", 0,
        [&mgr](Marshaler& m) { mgr.impl_waveInGetDevCaps(m, false); }, 12});
    exports.entries.push_back(ExportEntry{"waveInGetDevCapsW", 0,
        [&mgr](Marshaler& m) { mgr.impl_waveInGetDevCaps(m, true); }, 12});
    exports.entries.push_back(ExportEntry{"waveInOpen", 0,
        [&mgr](Marshaler& m) { mgr.impl_waveInOpen(m); }, 24});
    exports.entries.push_back(ExportEntry{"waveInClose", 0,
        [&mgr](Marshaler& m) { mgr.impl_waveInClose(m); }, 4});
    exports.entries.push_back(ExportEntry{"waveInPrepareHeader", 0,
        [&mgr](Marshaler& m) { mgr.impl_waveInPrepareHeader(m); }, 12});
    exports.entries.push_back(ExportEntry{"waveInUnprepareHeader", 0,
        [&mgr](Marshaler& m) { mgr.impl_waveInUnprepareHeader(m); }, 12});
    exports.entries.push_back(ExportEntry{"waveInAddBuffer", 0,
        [&mgr](Marshaler& m) { mgr.impl_waveInAddBuffer(m); }, 12});
    exports.entries.push_back(ExportEntry{"waveInStart", 0,
        [&mgr](Marshaler& m) { mgr.impl_waveInStart(m); }, 4});
    exports.entries.push_back(ExportEntry{"waveInStop", 0,
        [&mgr](Marshaler& m) { mgr.impl_waveInStop(m); }, 4});
    exports.entries.push_back(ExportEntry{"waveInReset", 0,
        [&mgr](Marshaler& m) { mgr.impl_waveInReset(m); }, 4});
    exports.entries.push_back(ExportEntry{"waveInGetPosition", 0,
        [&mgr](Marshaler& m) { mgr.impl_waveInGetPosition(m); }, 12});
    exports.entries.push_back(ExportEntry{"waveInGetID", 0,
        [&mgr](Marshaler& m) { mgr.impl_waveInGetID(m); }, 8});
    exports.entries.push_back(ExportEntry{"waveInMessage", 0,
        [&mgr](Marshaler& m) { mgr.impl_waveInMessage(m); }, 16});
    exports.entries.push_back(ExportEntry{"waveInGetErrorTextA", 0,
        [](Marshaler& m) { error_text(m, false, false); }, 12});
    exports.entries.push_back(ExportEntry{"waveInGetErrorTextW", 0,
        [](Marshaler& m) { error_text(m, true, false); }, 12});

    // ---- MIDI: no port; midiOut has only the MIDI mapper --------------------
    exports.entries.push_back(ExportEntry{"midiOutGetNumDevs", 0,
        [](Marshaler& m) { m.set_return(0u); }, 0});
    exports.entries.push_back(ExportEntry{"midiInGetNumDevs", 0,
        [](Marshaler& m) { m.set_return(0u); }, 0});
    exports.entries.push_back(ExportEntry{"midiOutGetDevCapsA", 0,
        [](Marshaler& m) { midi_out_get_dev_caps(m, false); }, 12});
    exports.entries.push_back(ExportEntry{"midiOutGetDevCapsW", 0,
        [](Marshaler& m) { midi_out_get_dev_caps(m, true); }, 12});
    for (const char* name : {"midiInGetDevCapsA", "midiInGetDevCapsW"}) {
        exports.entries.push_back(ExportEntry{name, 0, no_device_caps, 12});
    }
    exports.entries.push_back(ExportEntry{"midiOutOpen", 0,
        [](Marshaler& m) { midi_open(m, true, m.arg32(1)); }, 20});
    exports.entries.push_back(ExportEntry{"midiInOpen", 0,
        [](Marshaler& m) { midi_open(m, false, m.arg32(1)); }, 20});
    // midiStreamOpen(LPHMIDISTRM, LPUINT puDeviceID, DWORD cMidi, ...).
    exports.entries.push_back(ExportEntry{"midiStreamOpen", 0,
        [](Marshaler& m) {
            const GuestAddr id_ptr = m.arg_ptr(1);
            if (id_ptr == 0) {
                m.set_return(MMSYSERR_INVALPARAM);
                return;
            }
            midi_open(m, true, m.read_u32(id_ptr).value_or(0));
        }, 24});
    // Every MIDI handle is invalid: no open ever succeeds (mo_close_bad,
    // mo_short_bad, mo_reset_bad, mi_close_bad).
    for (const auto& [name, bytes] : std::initializer_list<std::pair<const char*, u16>>{
             {"midiOutClose", 4}, {"midiOutReset", 4}, {"midiOutShortMsg", 8},
             {"midiOutLongMsg", 12}, {"midiOutPrepareHeader", 12},
             {"midiOutUnprepareHeader", 12}, {"midiOutGetID", 8},
             {"midiOutCachePatches", 16}, {"midiOutCacheDrumPatches", 16},
             {"midiInClose", 4}, {"midiInReset", 4}, {"midiInStart", 4},
             {"midiInStop", 4}, {"midiInAddBuffer", 12},
             {"midiInPrepareHeader", 12}, {"midiInUnprepareHeader", 12},
             {"midiInGetID", 8}, {"midiStreamClose", 4},
             {"midiStreamOut", 12}, {"midiStreamPause", 4},
             {"midiStreamRestart", 4}, {"midiStreamStop", 4},
             {"midiStreamPosition", 12}, {"midiStreamProperty", 12},
             {"midiConnect", 12}, {"midiDisconnect", 12}}) {
        exports.entries.push_back(ExportEntry{name, 0,
            [](Marshaler& m) { m.set_return(MMSYSERR_INVALHANDLE); }, bytes});
    }
    // midiOutMessage / midiInMessage / midiOutGetVolume / midiOutSetVolume
    // take a handle or a device id; neither exists.
    for (const auto& [name, bytes] : std::initializer_list<std::pair<const char*, u16>>{
             {"midiOutMessage", 16}, {"midiInMessage", 16},
             {"midiOutGetVolume", 8}, {"midiOutSetVolume", 8}}) {
        exports.entries.push_back(ExportEntry{name, 0,
            [](Marshaler& m) { m.set_return(MMSYSERR_BADDEVICEID); }, bytes});
    }
    exports.entries.push_back(ExportEntry{"midiOutGetErrorTextA", 0,
        [](Marshaler& m) { error_text(m, false, true); }, 12});
    exports.entries.push_back(ExportEntry{"midiOutGetErrorTextW", 0,
        [](Marshaler& m) { error_text(m, true, true); }, 12});
    exports.entries.push_back(ExportEntry{"midiInGetErrorTextA", 0,
        [](Marshaler& m) { error_text(m, false, true); }, 12});
    exports.entries.push_back(ExportEntry{"midiInGetErrorTextW", 0,
        [](Marshaler& m) { error_text(m, true, true); }, 12});

    // ---- Mixer: no mixer device ---------------------------------------------
    exports.entries.push_back(ExportEntry{"mixerGetNumDevs", 0,
        [](Marshaler& m) { m.set_return(0u); }, 0});
    exports.entries.push_back(ExportEntry{"mixerOpen", 0,
        [&mgr](Marshaler& m) { mgr.impl_mixerOpen(m); }, 20});
    exports.entries.push_back(ExportEntry{"mixerClose", 0,
        [](Marshaler& m) { m.set_return(MMSYSERR_INVALHANDLE); }, 4});
    // mixerGetID(HMIXEROBJ, UINT* puMxId, DWORD fdwId): *puMxId = -1 on
    // every failure (mx_getid_badh / badid / badflag id=ffffffff).
    exports.entries.push_back(ExportEntry{"mixerGetID", 0,
        [&mgr](Marshaler& m) {
            const u64 object = m.arg(0);
            const GuestAddr id = m.arg_ptr(1);
            const u32 flags = m.arg32(2);
            if (id == 0) { // mx_getid_null
                m.set_return(MMSYSERR_INVALPARAM);
                return;
            }
            (void)m.write_u32(id, 0xFFFFFFFFu);
            if ((flags & ~(MIXER_OBJECTF_HANDLE | MIXER_OBJECTF_TYPEMASK)) != 0) {
                m.set_return(MMSYSERR_INVALFLAG);
                return;
            }
            m.set_return(mgr.mixer_object_status(object, flags));
        }, 12});
    for (const char* name : {"mixerGetDevCapsA", "mixerGetDevCapsW"}) {
        exports.entries.push_back(ExportEntry{name, 0, no_device_caps, 12});
    }
    exports.entries.push_back(ExportEntry{"mixerGetLineInfoA", 0,
        [&mgr](Marshaler& m) { mixer_get_line_info(mgr, m, false); }, 12});
    exports.entries.push_back(ExportEntry{"mixerGetLineInfoW", 0,
        [&mgr](Marshaler& m) { mixer_get_line_info(mgr, m, true); }, 12});
    exports.entries.push_back(ExportEntry{"mixerGetLineControlsA", 0,
        [&mgr](Marshaler& m) { mixer_get_line_controls(mgr, m, false); }, 12});
    exports.entries.push_back(ExportEntry{"mixerGetLineControlsW", 0,
        [&mgr](Marshaler& m) { mixer_get_line_controls(mgr, m, true); }, 12});
    exports.entries.push_back(ExportEntry{"mixerGetControlDetailsA", 0,
        [&mgr](Marshaler& m) { mixer_control_details(mgr, m, false, false); }, 12});
    exports.entries.push_back(ExportEntry{"mixerGetControlDetailsW", 0,
        [&mgr](Marshaler& m) { mixer_control_details(mgr, m, true, false); }, 12});
    exports.entries.push_back(ExportEntry{"mixerSetControlDetails", 0,
        [&mgr](Marshaler& m) { mixer_control_details(mgr, m, false, true); }, 12});
    // mixerMessage(HMIXER, UINT, DWORD_PTR, DWORD_PTR): an unknown handle is
    // taken as a mixer id, and there is none (x_mx_msg_badh r=2).
    exports.entries.push_back(ExportEntry{"mixerMessage", 0,
        [](Marshaler& m) { m.set_return(MMSYSERR_BADDEVICEID); }, 16});

    // ---- Joystick -----------------------------------------------------------
    //
    // Vitreo has no joystick backend, so the honest report is zero devices.
    // A guest that sees joyGetNumDevs() == 0 stops there; inventing a device
    // and then failing joyGetPosEx would leave it polling a phantom stick.

    // UINT joyGetNumDevs(void)
    exports.entries.push_back(ExportEntry{ "joyGetNumDevs", 0,
        [](Marshaler& m) { m.set_return(0u); }, 0 });

    // MMRESULT joyGetDevCapsA/W(UINT_PTR uJoyID, LPJOYCAPS pjc, UINT cbjc)
    const auto joy_get_dev_caps = [](Marshaler& m) {
        (void)m.arg(0);
        (void)m.arg(1);
        (void)m.arg(2);
        m.set_return(MMSYSERR_NODRIVER);
    };
    exports.entries.push_back(ExportEntry{ "joyGetDevCapsA", 0,
        joy_get_dev_caps, 12 });
    exports.entries.push_back(ExportEntry{ "joyGetDevCapsW", 0,
        joy_get_dev_caps, 12 });

    // MMRESULT joyGetPos(UINT uJoyID, LPJOYINFO pji)
    exports.entries.push_back(ExportEntry{ "joyGetPos", 0,
        [](Marshaler& m) {
            (void)m.arg(0); (void)m.arg(1);
            m.set_return(MMSYSERR_NODRIVER);
        }, 8 });

    // MMRESULT joyGetPosEx(UINT uJoyID, LPJOYINFOEX pji)
    exports.entries.push_back(ExportEntry{ "joyGetPosEx", 0,
        [](Marshaler& m) {
            (void)m.arg(0);
            (void)m.arg(1);
            m.set_return(MMSYSERR_NODRIVER);
        }, 8 });

    // MMRESULT joyGetThreshold / joySetThreshold / joySetCapture / joyReleaseCapture
    exports.entries.push_back(ExportEntry{ "joyGetThreshold", 0,
        [](Marshaler& m) {
            (void)m.arg(0); (void)m.arg(1);
            m.set_return(MMSYSERR_NODRIVER);
        }, 8 });
    exports.entries.push_back(ExportEntry{ "joySetThreshold", 0,
        [](Marshaler& m) {
            (void)m.arg(0); (void)m.arg(1);
            m.set_return(MMSYSERR_NODRIVER);
        }, 8 });
    exports.entries.push_back(ExportEntry{ "joyReleaseCapture", 0,
        [](Marshaler& m) {
            (void)m.arg(0);
            m.set_return(MMSYSERR_NODRIVER);
        }, 4 });

    // ---- Error text ---------------------------------------------------------
    // BOOL mciGetErrorStringA(MCIERROR mcierr, LPSTR pszText, UINT cchText)
    //
    // Vitreo implements no MCI commands, so every MCI error code the guest can
    // be holding is one Vitreo never produced. Returning FALSE is what Windows
    // does for an unrecognized code.
    exports.entries.push_back(ExportEntry{ "mciGetErrorStringA", 0,
        [](Marshaler& m) {
            (void)m.arg(0);
            const GuestAddr text = m.arg_ptr(1);
            if (text != 0 && m.arg(2) != 0) (void)m.write_u8(text, 0);
            m.set_return(0u);
        }, 12 });
    exports.entries.push_back(ExportEntry{ "mciGetErrorStringW", 0,
        [](Marshaler& m) {
            (void)m.arg(0);
            const GuestAddr text = m.arg_ptr(1);
            if (text != 0 && m.arg(2) != 0) (void)m.write_u16(text, 0);
            m.set_return(0u);
        }, 12 });

    return exports;
}

} // namespace vitreo::winapi
