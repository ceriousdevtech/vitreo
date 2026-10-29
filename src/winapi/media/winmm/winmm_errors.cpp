/// @file winmm_errors.cpp
/// @brief waveOutGetErrorText / midiOutGetErrorText message tables.
///
/// Split out of winmm.cpp.

#include "winapi/media/winmm/winmm_internal.hpp"
#include <algorithm>
#include <iterator>
#include <string_view>

namespace vitreo::winapi {

namespace {

// Generated from tools/oracle_probe/s7_winmm_probe.expected (wot_a/mot_a lines
// measured on Windows). MMSYSERR_* 0..20 are shared; waveOut/waveIn add
// WAVERR_* 32..35, midiOut/midiIn add MIDIERR_* 64..71. Every other code is
// MMSYSERR_BADERRNUM (21 MMSYSERR_MOREDATA included).
struct MmErrorText { u32 code; const char* text; };
constexpr MmErrorText kMmsyserrText[] = {
    {0, "The specified command was carried out."},
    {1, "Undefined external error."},
    {2, "A device ID has been used that is out of range for your system."},
    {3, "The driver was not enabled."},
    {4, "The specified device is already in use.  Wait until it is free, and then try again."},
    {5, "The specified device handle is invalid."},
    {6, "There is no driver installed on your system."},
    {7, "There is not enough memory available for this task.  Quit one or more applications to increase available memory, and then try again."},
    {8, "This function is not supported.  Use the Capabilities function to determine which functions and messages the driver supports."},
    {9, "An error number was specified that is not defined in the system."},
    {10, "An invalid flag was passed to a system function."},
    {11, "An invalid parameter was passed to a system function."},
    {12, "Handle being used simultaneously on another thread (eg callback)."},
    {13, "Specified alias not found in WIN.INI."},
    {14, "The registry database is corrupt."},
    {15, "The specified registry key was not found."},
    {16, "The registry could not be opened or could not be read."},
    {17, "The registry could not be written to."},
    {18, "The specified registry key could not be deleted."},
    {19, "The specified registry key value could not be found."},
    {20, "The driver did not generate a valid OPEN callback."},
};
constexpr MmErrorText kWaveerrText[] = {
    {32, "The specified format is not supported or cannot be translated.  Use the Capabilities function to determine the supported formats."},
    {33, "Cannot perform this operation while media data is still playing.  Reset the device, or wait until the data is finished playing."},
    {34, "The wave header was not prepared.  Use the Prepare function to prepare the header, and then try again."},
    {35, "Cannot open the device without using the WAVE_ALLOWSYNC flag.  Use the flag, and then try again."},
};
constexpr MmErrorText kMidierrText[] = {
    {64, "The MIDI header was not prepared.  Use the Prepare function to prepare the header, and then try again."},
    {65, "Cannot perform this operation while media data is still playing.  Reset the device, or wait until the data is finished playing."},
    {66, "A MIDI map was not found.  There may be a problem with the driver, or the MIDIMAP.CFG file may be corrupt or missing."},
    {67, "The port is transmitting data to the device.  Wait until the data has been transmitted, and then try again."},
    {68, "The current MIDI Mapper setup refers to a MIDI device that is not installed on the system.  Use MIDI Mapper to edit the setup."},
    {69, "The current MIDI setup is damaged.  Copy the original MIDIMAP.CFG file to the Windows SYSTEM directory, and then try again."},
    {70, "A MIDI call was made which is invalid with the current open mode. Reopen the device with the correct mode."},
    {71, "Driver condition - do not callback this input event"},
};

/// The text for `code`, or nullptr (MMSYSERR_BADERRNUM). waveOut/waveIn know
/// MMSYSERR + WAVERR, midiOut/midiIn MMSYSERR + MIDIERR (s7_winmm_probe
/// wot_a e=40 r=9, mot_a e=20 r=9).
[[nodiscard]] const char* mm_error_text(u32 code, bool midi) {
    const auto find = [code](const MmErrorText* first, usize count) -> const char* {
        for (usize i = 0; i < count; ++i) {
            if (first[i].code == code) return first[i].text;
        }
        return nullptr;
    };
    if (const char* t = find(kMmsyserrText, std::size(kMmsyserrText))) return t;
    return midi ? find(kMidierrText, std::size(kMidierrText))
                : find(kWaveerrText, std::size(kWaveerrText));
}

} // namespace

namespace winmm_internal {

void error_text(Marshaler& m, bool wide, bool midi) {
    const u32 error = m.arg32(0);
    const GuestAddr text = m.arg_ptr(1);
    const u32 chars = m.arg32(2);
    // cchText 0 succeeds and writes nothing, before the code is looked up
    // (wot_a_cch0 / wot_a_bad_cch0 / wot_a_null0).
    if (chars == 0) {
        m.set_return(MMSYSERR_NOERROR);
        return;
    }
    if (text == 0) { // wot_a_null
        m.set_return(MMSYSERR_INVALPARAM);
        return;
    }
    const char* message = mm_error_text(error, midi);
    // An unknown code returns an empty string (wot_a e=15 r=9 buf0=0).
    const std::string_view full = message ? message : "";
    const usize copy = std::min<usize>(full.size(), chars - 1);
    for (usize i = 0; i < copy; ++i) {
        if (wide) {
            (void)m.write_u16(text + i * 2, static_cast<u8>(full[i]));
        } else {
            (void)m.write_u8(text + i, static_cast<u8>(full[i]));
        }
    }
    if (wide) (void)m.write_u16(text + copy * 2, 0);
    else (void)m.write_u8(text + copy, 0);
    m.set_return(message ? MMSYSERR_NOERROR : MMSYSERR_BADERRNUM);
}

} // namespace winmm_internal

} // namespace vitreo::winapi
