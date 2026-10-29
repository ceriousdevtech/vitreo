/// @file winmm_wave_format.cpp
/// @brief WAVEFORMATEX validation, guest reads and sample conversion.
///
/// Shared by waveOutOpen, waveInOpen and PlaySound; declared in
/// winmm_internal.hpp. Split out of winmm.cpp.

#include "winapi/media/winmm/winmm_internal.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace vitreo::winapi {

namespace winmm_internal {

// ---------------------------------------------------------------------------
// Wave formats
// ---------------------------------------------------------------------------

/// Validate a WAVEFORMATEX(TENSIBLE) image. `opening` adds the checks only
/// an actual open makes: Windows' WAVE_FORMAT_QUERY accepts a wrong
/// nBlockAlign and 16-bit IEEE float, the open then rejects them
/// (s7_winmm_probe q_bad_align=0 vs o_real_bad_align=20, q_float16=0 vs
/// o_real_float16=20). nAvgBytesPerSec is never checked (o_real_bad_avg=0).
[[nodiscard]] WaveFormatCheck check_wave_format(const std::vector<u8>& fmt,
                                                bool opening) {
    WaveFormatCheck check;
    if (fmt.size() < 16) return check;
    const u16 tag = le16(fmt, 0);
    const u16 channels = le16(fmt, 2);
    const u32 rate = le32(fmt, 4);
    const u16 align = le16(fmt, 12);
    const u16 bits = le16(fmt, 14);
    // q_0ch / q_0rate
    if (channels == 0 || rate == 0) return check;

    const auto pcm_bits_ok = [](u16 b) {
        return b == 8 || b == 16 || b == 24 || b == 32;
    };
    bool is_float = false;
    switch (tag) {
    case WAVE_FORMAT_PCM:
        // q_12bit=20; a non-extensible PCM format is at most stereo (q_6ch=20).
        if (!pcm_bits_ok(bits) || channels > 2) return check;
        break;
    case WAVE_FORMAT_IEEE_FLOAT:
        // q_float16 / q_float32 / q_float64 = 0.
        if ((bits != 16 && bits != 32 && bits != 64) || channels > 2) {
            return check;
        }
        is_float = true;
        break;
    case WAVE_FORMAT_EXTENSIBLE: {
        // WAVEFORMATEXTENSIBLE needs cbSize >= 22 (q_ext_cb0=20).
        if (fmt.size() < 40 || le16(fmt, 16) < 22) return check;
        const u32 sub = le32(fmt, 24);
        if (std::memcmp(fmt.data() + 28, kSubtypeTail, 12) != 0) return check;
        if (sub == WAVE_FORMAT_PCM) {
            if (!pcm_bits_ok(bits)) return check;
        } else if (sub == WAVE_FORMAT_IEEE_FLOAT) {
            if (bits != 32 && bits != 64) return check;
            is_float = true;
        } else {
            return check; // q_ext_sub_unknown=20
        }
        if (channels > 2) {
            // Windows accepts multichannel extensible formats and downmixes
            // them (q_ext_pcm_6ch=0); the host stream is at most stereo.
            WINMM_UNIMPLEMENTED(
                "waveOutOpen: WAVE_FORMAT_EXTENSIBLE with more than 2 channels");
            return check;
        }
        break;
    }
    default:
        // Windows' mapper converts formats of installed ACM codecs; Vitreo
        // has no ACM, so only PCM and IEEE float reach the device.
        WINMM_UNIMPLEMENTED("waveOutOpen: ACM-converted (non-PCM) format tag");
        return check;
    }

    if (opening) {
        if (align != channels * (bits / 8)) return check;
        if (is_float && bits == 16) return check;
    }

    const u16 host_bits = is_float ? 32 : bits;
    check.host.channels = channels;
    check.host.samples_per_second = rate;
    check.host.bits_per_sample = host_bits;
    check.host.block_align = static_cast<u16>(channels * (host_bits / 8));
    check.host.average_bytes_per_second = rate * check.host.block_align;
    check.conversion = !is_float ? WaveConversion::None
                     : bits == 32 ? WaveConversion::Float32ToInt32
                                  : WaveConversion::Float64ToInt32;
    check.guest_block_align = static_cast<u16>(channels * (bits / 8));
    check.guest_bytes_per_second = rate * check.guest_block_align;
    check.result = MMSYSERR_NOERROR;
    return check;
}

/// Read a guest WAVEFORMATEX: 16 bytes of PCMWAVEFORMAT, then cbSize and the
/// extension only for tags that carry one (a PCM caller may pass a 16-byte
/// PCMWAVEFORMAT; q_pcm_cbsize_junk shows cbSize is ignored for PCM).
[[nodiscard]] std::vector<u8> read_guest_wave_format(GuestMemory& memory,
                                                     GuestAddr pwfx) {
    std::vector<u8> fmt(16);
    if (memory.read_bytes(pwfx, reinterpret_cast<Byte*>(fmt.data()), 16)
            .is_err()) {
        return {};
    }
    if (le16(fmt, 0) != WAVE_FORMAT_EXTENSIBLE) return fmt;
    fmt.resize(40);
    if (memory.read_bytes(pwfx + 16, reinterpret_cast<Byte*>(fmt.data() + 16),
                          24).is_err()) {
        fmt.resize(18);
        if (memory.read_bytes(pwfx + 16,
                              reinterpret_cast<Byte*>(fmt.data() + 16), 2)
                .is_err()) {
            return {};
        }
    }
    return fmt;
}

/// IEEE float samples to 32-bit signed PCM, the host stream's format.
[[nodiscard]] std::vector<Byte> convert_samples(std::span<const Byte> in,
                                                WaveConversion conversion) {
    const auto to_i32 = [](double v) -> i32 {
        if (!(v == v)) return 0; // NaN
        if (v >= 1.0) return std::numeric_limits<i32>::max();
        if (v <= -1.0) return std::numeric_limits<i32>::min();
        return static_cast<i32>(v * 2147483648.0);
    };
    std::vector<Byte> out;
    if (conversion == WaveConversion::Float32ToInt32) {
        out.resize(in.size() / 4 * 4);
        for (usize i = 0; i + 4 <= in.size(); i += 4) {
            float f;
            std::memcpy(&f, in.data() + i, 4);
            const i32 s = to_i32(static_cast<double>(f));
            std::memcpy(out.data() + i, &s, 4);
        }
    } else if (conversion == WaveConversion::Float64ToInt32) {
        out.resize(in.size() / 8 * 4);
        for (usize i = 0, o = 0; i + 8 <= in.size(); i += 8, o += 4) {
            double d;
            std::memcpy(&d, in.data() + i, 8);
            const i32 s = to_i32(d);
            std::memcpy(out.data() + o, &s, 4);
        }
    } else {
        out.assign(in.begin(), in.end());
    }
    return out;
}

} // namespace winmm_internal

} // namespace vitreo::winapi
