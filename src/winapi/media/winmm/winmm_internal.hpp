#pragma once

/// @file winmm_internal.hpp
/// @brief Private declarations shared by the winmm*.cpp translation units.
///
/// PRIVATE: include from src/winapi/media/winmm/winmm*.cpp only. winmm.cpp
/// was one 3,400-line translation unit whose constants and helpers sat in
/// anonymous namespaces; they are here so the per-API files that were split
/// out of it (winmm_waveout.cpp, winmm_wavein.cpp, winmm_playsound.cpp, ...)
/// still see them. The constants keep internal linkage, so each including
/// TU has its own copy exactly as before. The helpers shared by more than one
/// TU live in `winmm_internal` (the tiny ones inline here, the rest defined
/// once in the TU named next to them), and the trailing using-directive
/// keeps their call sites unqualified.

#include "winapi/media/winmm/winmm.hpp"
#include "winapi/core/marshaler.hpp"
#include "core/memory/guest_memory.hpp"
#include "windowing/contracts/wave_output.hpp"
#include <vitreo/common.hpp>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <span>
#include <string_view>
#include <vector>

namespace vitreo::winapi {

/// One-time `[vitreo] UNIMPLEMENTED winmm!<api>: <case>` report.
#define WINMM_UNIMPLEMENTED(api_case)                                       \
    do {                                                                    \
        static std::atomic_flag reported_ = ATOMIC_FLAG_INIT;               \
        if (!reported_.test_and_set()) {                                    \
            std::fprintf(stderr, "[vitreo] UNIMPLEMENTED winmm!%s\n",       \
                         api_case);                                         \
        }                                                                   \
    } while (0)

// playsoundapi.h
static constexpr u32 SND_ASYNC       = 0x00000001;
static constexpr u32 SND_NODEFAULT   = 0x00000002;
static constexpr u32 SND_MEMORY      = 0x00000004;
static constexpr u32 SND_LOOP        = 0x00000008;
static constexpr u32 SND_NOSTOP      = 0x00000010;
static constexpr u32 SND_PURGE       = 0x00000040;
static constexpr u32 SND_FILENAME    = 0x00020000;
static constexpr u32 SND_RESOURCE_BIT = 0x00040000; // SND_RESOURCE = 0x40004
static constexpr u32 SND_ALIAS_ID    = 0x00110000;

// mmsyscom.h
static constexpr u32 MMSYSERR_NOERROR     = 0;
static constexpr u32 MMSYSERR_ERROR       = 1;
static constexpr u32 MMSYSERR_BADDEVICEID = 2;
static constexpr u32 MMSYSERR_ALLOCATED   = 4;
static constexpr u32 MMSYSERR_INVALHANDLE = 5;
static constexpr u32 MMSYSERR_NODRIVER    = 6;
static constexpr u32 MMSYSERR_NOTSUPPORTED = 8;
static constexpr u32 MMSYSERR_BADERRNUM   = 9;
static constexpr u32 MMSYSERR_INVALFLAG   = 10;
static constexpr u32 MMSYSERR_INVALPARAM  = 11;
// mmeapi.h
static constexpr u32 WAVERR_BADFORMAT     = 32;
static constexpr u32 WAVERR_STILLPLAYING  = 33;
static constexpr u32 WAVERR_UNPREPARED    = 34;
static constexpr u32 MIDIERR_NODEVICE     = 68;
static constexpr u32 TIMERR_NOERROR    = 0;
/// Returned when a requested resolution is outside the advertised range, or
/// when timeEndPeriod is called without a matching timeBeginPeriod.
static constexpr u32 TIMERR_NOCANDO    = 97;
static constexpr u32 WHDR_DONE         = 0x00000001;
static constexpr u32 WHDR_PREPARED     = 0x00000002;
static constexpr u32 WHDR_BEGINLOOP    = 0x00000004;
static constexpr u32 WHDR_INQUEUE      = 0x00000010;
static constexpr u32 WAVE_FORMAT_QUERY = 0x00000001;
static constexpr u32 WAVE_MAPPER       = 0xFFFFFFFFu;
static constexpr u32 MIDI_MAPPER       = 0xFFFFFFFFu;

static constexpr u32 CALLBACK_TYPEMASK = 0x00070000;
static constexpr u32 CALLBACK_NULL     = 0x00000000;
static constexpr u32 CALLBACK_WINDOW   = 0x00010000;
static constexpr u32 CALLBACK_THREAD   = 0x00020000;
static constexpr u32 CALLBACK_FUNCTION = 0x00030000;
static constexpr u32 CALLBACK_EVENT    = 0x00050000;

static constexpr u32 WOM_OPEN  = 0x03BB;
static constexpr u32 WOM_CLOSE = 0x03BC;
static constexpr u32 WOM_DONE  = 0x03BD;

static constexpr u32 TIME_SAMPLES = 0x0002;
static constexpr u32 TIME_BYTES   = 0x0004;
/// sizeof(MMTIME): 12 on x86 and x64 (s7_winmm_probe `sizes`).
static constexpr u32 MMTIME_SIZE  = 12;

// mmddk.h: DRVM_MAPPER (0x2000) + 21 / + 23.
static constexpr u32 DRVM_MAPPER_PREFERRED_GET       = 0x2015;
static constexpr u32 DRVM_MAPPER_CONSOLEVOICECOM_GET = 0x2017;

// mmreg.h / ksmedia.h
static constexpr u16 WAVE_FORMAT_PCM        = 0x0001;
static constexpr u16 WAVE_FORMAT_IEEE_FLOAT = 0x0003;
static constexpr u16 WAVE_FORMAT_EXTENSIBLE = 0xFFFE;
static constexpr u16 MM_MICROSOFT             = 1;
static constexpr u16 MM_MIDI_MAPPER           = 1;
static constexpr u16 MM_WAVE_MAPPER           = 2;
static constexpr u16 MM_MSFT_WDMAUDIO_WAVEOUT = 100;
static constexpr u32 WAVECAPS_VOLUME = 0x0004;
static constexpr u16 MOD_MAPPER       = 5;
static constexpr u32 MIDICAPS_VOLUME  = 0x0001;
static constexpr u32 MIDICAPS_STREAM  = 0x0008;

// winerror.h
static constexpr u32 ERROR_FILE_NOT_FOUND            = 2;
static constexpr u32 ERROR_RESOURCE_DATA_NOT_FOUND   = 1812;
static constexpr u32 ERROR_RESOURCE_TYPE_NOT_FOUND   = 1813;
static constexpr u32 ERROR_RESOURCE_NAME_NOT_FOUND   = 1814;

// MIXER_OBJECTF_*: read by the mixer TU and by the thunk table.
static constexpr u32 MIXER_OBJECTF_HANDLE   = 0x80000000u;
static constexpr u32 MIXER_OBJECTF_TYPEMASK = 0x70000000u;
static constexpr u32 MIXER_OBJECTF_MIXER    = 0x00000000u;
static constexpr u32 MIXER_OBJECTF_WAVEOUT  = 0x10000000u;
static constexpr u32 MIXER_OBJECTF_WAVEIN   = 0x20000000u;
static constexpr u32 MIXER_OBJECTF_AUX      = 0x50000000u;

namespace winmm_internal {

/// Every waveOut*/midi*/mixer* API that takes a UINT device id also accepts
/// the id zero-extended into a UINT_PTR/HANDLE slot; WAVE_MAPPER may arrive
/// either zero- or sign-extended on x64.
[[nodiscard]] inline bool is_mapper_id(u64 value) noexcept {
    return value == 0xFFFFFFFFull || value == ~0ull;
}

enum class WaveConversion : u8 { None, Float32ToInt32, Float64ToInt32 };

struct WaveFormatCheck {
    u32 result{WAVERR_BADFORMAT};
    windowing::WaveOutputFormat host{};
    WaveConversion conversion{WaveConversion::None};
    u16 guest_block_align{0};
    u32 guest_bytes_per_second{0};
};

// KSDATAFORMAT_SUBTYPE_PCM / _IEEE_FLOAT (ksmedia.h): {0000000X-0000-0010-
// 8000-00aa00389b71}. Only Data1 differs.
inline constexpr u8 kSubtypeTail[12] = {0x00, 0x00, 0x10, 0x00, 0x80, 0x00,
                                 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71};

[[nodiscard]] inline u16 le16(const std::vector<u8>& b, usize o) {
    return static_cast<u16>(b[o] | (b[o + 1] << 8));
}
[[nodiscard]] inline u32 le32(const std::vector<u8>& b, usize o) {
    return static_cast<u32>(b[o]) | (static_cast<u32>(b[o + 1]) << 8) |
           (static_cast<u32>(b[o + 2]) << 16) |
           (static_cast<u32>(b[o + 3]) << 24);
}

inline void put16(std::vector<u8>& b, usize o, u16 v) {
    b[o] = static_cast<u8>(v);
    b[o + 1] = static_cast<u8>(v >> 8);
}
inline void put32(std::vector<u8>& b, usize o, u32 v) {
    for (usize i = 0; i < 4; ++i) b[o + i] = static_cast<u8>(v >> (8 * i));
}
/// szPname: 32 CHARs (A) or 32 WCHARs (W), NUL-terminated and truncated.
inline void put_name(std::vector<u8>& b, usize o, std::string_view name, bool wide) {
    const usize n = std::min<usize>(name.size(), 31);
    for (usize i = 0; i < n; ++i) {
        const u8 c = static_cast<u8>(name[i]);
        if (wide) put16(b, o + i * 2, c); else b[o + i] = c;
    }
}
/// INIT_MMREG_MID / INIT_MMREG_PID (ksmedia.h).
inline void put_mmreg_guid(std::vector<u8>& b, usize o, u32 base, u16 d2, u16 id) {
    static constexpr u8 tail[8] = {0xa2, 0x1a, 0x00, 0xa0,
                                   0xc9, 0x22, 0x31, 0x96};
    put32(b, o, base + id);
    put16(b, o + 4, d2);
    put16(b, o + 6, 0x11d1);
    std::memcpy(b.data() + o + 8, tail, 8);
}

inline constexpr char kWaveDeviceName[] = "Vitreo Audio";
inline constexpr char kWaveMapperName[] = "Microsoft Sound Mapper";
inline constexpr char kMidiMapperName[] = "Microsoft MIDI Mapper";

/// stderr diagnostics gated on VITREO_WINMM_DIAG (defined in winmm.cpp, so
/// the env lookup and the mutex stay single process-wide instances).
bool winmm_diag_enabled();
void winmm_diag(const char* format, ...);

/// WAVEFORMATEX contract helpers (winmm_wave_format.cpp).
[[nodiscard]] WaveFormatCheck check_wave_format(const std::vector<u8>& fmt,
                                                bool opening);
[[nodiscard]] std::vector<u8> read_guest_wave_format(GuestMemory& memory,
                                                     GuestAddr pwfx);
[[nodiscard]] std::vector<Byte> convert_samples(std::span<const Byte> in,
                                                WaveConversion conversion);

/// MM error text (winmm_errors.cpp).
void error_text(Marshaler& m, bool wide, bool midi);

/// MIDI and mixer with no device (winmm_midi_mixer.cpp).
void no_device_caps(Marshaler& m);
void midi_out_get_dev_caps(Marshaler& m, bool wide);
void midi_open(Marshaler& m, bool output, u32 id);
void mixer_get_line_info(WinmmManager& mgr, Marshaler& m, bool wide);
void mixer_get_line_controls(WinmmManager& mgr, Marshaler& m, bool wide);
void mixer_control_details(WinmmManager& mgr, Marshaler& m, bool wide,
                           bool set);

} // namespace winmm_internal

using namespace winmm_internal;

// ---------------------------------------------------------------------------
// Streams
// ---------------------------------------------------------------------------
struct WinmmManager::WaveStream {
    u64 handle{0};
    u32 device_id{0}; ///< As passed to waveOutOpen (WAVE_MAPPER kept).
    bool is_64bit{false};
    GuestAddr callback{0};
    u64 instance{0};
    u32 callback_type{CALLBACK_NULL};
    OutputLease lease;
    WaveConversion conversion{WaveConversion::None};
    struct Pending {
        GuestAddr header{0};
        u32 bytes{0};
        /// Backend token; 0 for an empty buffer, which completes together
        /// with the buffer queued before it (write_empty_hdr: done, f=3).
        u64 token{0};
    };
    std::deque<Pending> pending;
    u64 next_token{1};
    u64 bytes_completed{0};
    u32 bytes_per_second{0};
    u16 block_align{0};
    u32 volume{0x0000FFFF};
    // Diagnostics (VITREO_WINMM_DIAG).
    u64 diag_bytes_written{0};
    std::chrono::steady_clock::time_point diag_first_write{};
};

} // namespace vitreo::winapi
