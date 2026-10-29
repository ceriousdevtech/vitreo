/// @file winmm_playsound.cpp
/// @brief PlaySound / sndPlaySound.
///
/// The single PlaySound voice (SoundVoice), the RIFF/WAVE parser it plays
/// from, and the resource lookup for SND_RESOURCE. Split out of winmm.cpp.

#include "winapi/media/winmm/winmm_internal.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/kernel32/filesystem/file.hpp"
#include "winapi/kernel32/locale/string_conv.hpp"
#include "windowing/resources/resource_walker.hpp"
#include "windowing/contracts/wave_output.hpp"
#include "capsule/vfs/vfs.hpp"
#include <algorithm>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <string>

namespace vitreo::winapi {

namespace {

/// A RIFF WAVE image split into its format and sample data.
struct ParsedWav {
    std::vector<u8> fmt;
    std::vector<u8> data;
};

[[nodiscard]] bool parse_wav(const std::vector<u8>& wav, ParsedWav& out) {
    if (wav.size() < 12 || std::memcmp(wav.data(), "RIFF", 4) != 0 ||
        std::memcmp(wav.data() + 8, "WAVE", 4) != 0) {
        return false;
    }
    const usize end = std::min<usize>(wav.size(),
                                      static_cast<usize>(le32(wav, 4)) + 8);
    bool have_fmt = false;
    bool have_data = false;
    usize pos = 12;
    while (pos + 8 <= end) {
        const u32 size = le32(wav, pos + 4);
        const usize body = pos + 8;
        if (body + size > end) break;
        if (std::memcmp(wav.data() + pos, "fmt ", 4) == 0 && size >= 16) {
            out.fmt.assign(wav.begin() + static_cast<std::ptrdiff_t>(body),
                           wav.begin() + static_cast<std::ptrdiff_t>(body + size));
            have_fmt = true;
        } else if (std::memcmp(wav.data() + pos, "data", 4) == 0) {
            out.data.assign(wav.begin() + static_cast<std::ptrdiff_t>(body),
                            wav.begin() + static_cast<std::ptrdiff_t>(body + size));
            have_data = true;
        }
        pos = body + size + (size & 1U);
    }
    return have_fmt && have_data;
}

// ---------------------------------------------------------------------------
// Resources
// ---------------------------------------------------------------------------
/// The resource directory of a loaded image, from its PE optional header.
[[nodiscard]] bool module_resource_dir(GuestMemory& memory, GuestAddr base,
                                       u32& rva, u32& size) {
    if (base == 0) return false;
    if (memory.read_u16(base).value_or(0) != 0x5A4D) return false;
    const u32 lfanew = memory.read_u32(base + 0x3C).value_or(0);
    if (memory.read_u32(base + lfanew).value_or(0) != 0x00004550) return false;
    const GuestAddr opt = base + lfanew + 24;
    const u16 magic = memory.read_u16(opt).value_or(0);
    // IMAGE_DIRECTORY_ENTRY_RESOURCE (2); the data directories start at 96
    // (PE32) or 112 (PE32+) and each entry is 8 bytes.
    const u32 dirs = magic == 0x20B ? 112u : magic == 0x10B ? 96u : 0u;
    if (dirs == 0) return false;
    const u32 count = memory.read_u32(opt + dirs - 4).value_or(0);
    if (count <= 2) {
        rva = 0;
        size = 0;
        return true;
    }
    rva = memory.read_u32(opt + dirs + 16).value_or(0);
    size = memory.read_u32(opt + dirs + 20).value_or(0);
    return true;
}

[[nodiscard]] std::u16string ascii_upper_utf16(std::string_view utf8) {
    std::vector<u16> w = utf8_to_utf16(utf8);
    std::u16string out;
    out.reserve(w.size());
    for (u16 c : w) {
        out.push_back(static_cast<char16_t>(
            c >= u'a' && c <= u'z' ? c - (u'a' - u'A') : c));
    }
    return out;
}

} // namespace

struct WinmmManager::SoundVoice {
    OutputLease lease;
    std::vector<Byte> pcm;
    bool loop{false};
    std::mutex mutex;
    std::condition_variable cv;
    bool finished{false};
    bool stopped{false};
};

// ---------------------------------------------------------------------------
// PlaySound / sndPlaySound
// ---------------------------------------------------------------------------
void WinmmManager::stop_sound(bool set_last_error_on_stop) {
    std::shared_ptr<SoundVoice> voice;
    bool had_async = false;
    {
        std::lock_guard<std::mutex> guard(m_sound_mutex);
        voice = std::move(m_sound);
        m_sound.reset();
        had_async = m_sound_async_pending;
        m_sound_async_pending = false;
    }
    if (voice) {
        {
            std::lock_guard<std::mutex> guard(voice->mutex);
            voice->stopped = true;
        }
        voice->cv.notify_all();
        release_output(voice->lease);
    }
    // Tearing down a previously started async sound leaves the last error 0;
    // stopping when nothing was started leaves it untouched (s7_winmm_probe
    // ps_stop_idle le=0, ps_stop_idle2 le=dead, ps_purge_busy le=0).
    if (had_async && set_last_error_on_stop) set_last_error(0);
}

bool WinmmManager::play_sound(u32 flags, std::vector<u8> wav) {
    ParsedWav parsed;
    // Not a RIFF WAVE with fmt and data: FALSE, last error untouched
    // (ps_mem_junk / ps_mem_notwave / ps_mem_nodata).
    if (!parse_wav(wav, parsed)) return false;
    const WaveFormatCheck check = check_wave_format(parsed.fmt, true);
    if (check.result != MMSYSERR_NOERROR) {
        set_last_error(0); // ps_mem_badtag: FALSE, le=0
        return false;
    }

    {
        std::lock_guard<std::mutex> guard(m_sound_mutex);
        if ((flags & SND_NOSTOP) != 0 && m_sound) {
            std::lock_guard<std::mutex> voice_guard(m_sound->mutex);
            // ps_nostop_busy: FALSE with the last error untouched.
            if (!m_sound->finished && !m_sound->stopped) return false;
        }
    }
    stop_sound(false);

    auto voice = std::make_shared<SoundVoice>();
    voice->pcm = convert_samples(parsed.data, check.conversion);
    // SND_LOOP needs SND_ASYNC; without it the sound plays once,
    // synchronously (ps_loop_sync r=1, waited).
    voice->loop = (flags & SND_LOOP) != 0 && (flags & SND_ASYNC) != 0;
    if (!acquire_output(voice->lease)) {
        WINMM_UNIMPLEMENTED("PlaySound: no host output stream available");
        set_last_error(0);
        return false;
    }
    std::weak_ptr<SoundVoice> weak = voice;
    const bool opened = voice->lease.output->open(
        check.host, [weak](u64, u32) {
            auto v = weak.lock();
            if (!v) return;
            {
                // stop_sound sets `stopped` under this mutex before it
                // releases the output, so a looping resubmit never touches
                // a closed stream.
                std::lock_guard<std::mutex> guard(v->mutex);
                if (v->stopped) return;
                if (v->loop && v->lease.output != nullptr) {
                    (void)v->lease.output->submit(1, v->pcm);
                    return;
                }
                v->finished = true;
            }
            v->cv.notify_all();
        });
    if (!opened) {
        release_output(voice->lease);
        set_last_error(0);
        return false;
    }
    if (voice->pcm.empty()) {
        voice->finished = true;
    } else if (!voice->lease.output->submit(1, voice->pcm)) {
        release_output(voice->lease);
        set_last_error(0);
        return false;
    }

    const bool async = (flags & SND_ASYNC) != 0;
    bool first_async = false;
    {
        std::lock_guard<std::mutex> guard(m_sound_mutex);
        m_sound = voice;
        m_sound_async_pending = async;
        first_async = async && !m_sound_async_started;
        if (async) m_sound_async_started = true;
    }
    if (async) {
        // The process's first async sound starts Windows' PlaySound worker
        // and leaves the last error 0 (ps_file_async le=0); later ones leave
        // it untouched (ps_file_async2 / ps_mem_async le=dead).
        if (first_async) set_last_error(0);
        return true;
    }

    {
        std::unique_lock<std::mutex> lock(voice->mutex);
        voice->cv.wait(lock, [&] { return voice->finished || voice->stopped; });
    }
    {
        std::lock_guard<std::mutex> guard(m_sound_mutex);
        if (m_sound == voice) m_sound.reset();
    }
    release_output(voice->lease);
    set_last_error(0); // ps_file_sync / ps_mem / ps_res_id: le=0
    return true;
}

void WinmmManager::play_sound_entry(Marshaler& m, GuestAddr sound,
                                    GuestAddr hmod, u32 flags, bool wide) {
    // SND_PURGE and a NULL name stop the current sound (ps_purge,
    // ps_stop_null).
    if ((flags & SND_PURGE) != 0 || sound == 0) {
        stop_sound(true);
        m.set_return(1);
        return;
    }

    const auto read_name = [&](std::string& out) -> bool {
        if (wide) {
            auto r = m.read_wide_string(sound, 32768);
            if (r.is_err()) return false;
            out = r.value();
            return true;
        }
        auto r = m.read_ansi_string(sound, 32768);
        if (r.is_err()) return false;
        // Learn: the A entry point takes ANSI (CP_ACP) text.
        const std::string& ansi = r.value();
        if (ansi.empty()) {
            out.clear();
            return true;
        }
        std::vector<u16> w(ansi.size() + 1);
        const int n = StringConverter::multi_byte_to_wide_char(
            CP_ACP, 0, ansi.data(), static_cast<int>(ansi.size()), w.data(),
            static_cast<int>(w.size()));
        out = utf16_to_utf8(w.data(), n);
        return true;
    };

    std::vector<u8> wav;
    if ((flags & SND_RESOURCE_BIT) != 0) {
        // SND_RESOURCE: hmod NULL means the executable (ps_res_null_hmod=1).
        const GuestAddr module = hmod != 0 ? hmod : m_image_base;
        u32 rva = 0;
        u32 size = 0;
        if (module == m_image_base && m_rsrc_rva != 0) {
            rva = m_rsrc_rva;
            size = m_rsrc_size;
        } else if (!module_resource_dir(m_memory, module, rva, size)) {
            set_last_error(ERROR_RESOURCE_DATA_NOT_FOUND);
            m.set_return(0);
            return;
        }
        u16 id = 0;
        bool by_id = sound < 0x10000; // IS_INTRESOURCE
        std::u16string name;
        if (by_id) {
            id = static_cast<u16>(sound);
        } else {
            std::string text;
            if (!read_name(text)) {
                m.set_return(0);
                return;
            }
            // "#123" names integer id 123 (ps_res_hash=1).
            if (text.size() > 1 && text[0] == '#') {
                u32 value = 0;
                bool digits = true;
                for (usize i = 1; i < text.size(); ++i) {
                    if (text[i] < '0' || text[i] > '9') { digits = false; break; }
                    value = value * 10 + static_cast<u32>(text[i] - '0');
                }
                if (digits && value < 0x10000) {
                    by_id = true;
                    id = static_cast<u16>(value);
                }
            }
            // Resource names are stored upper case (FindResource).
            if (!by_id) name = ascii_upper_utf16(text);
        }
        static const char16_t wave_type[] = u"WAVE";
        const auto langs = windowing::enumerate_resource_languages(
            m_memory, module, rva, size, 0, true, wave_type, 4, id, !by_id,
            by_id ? nullptr : name.c_str(),
            static_cast<u16>(name.size()));
        if (size == 0 ||
            langs.status == windowing::ResourceLanguageEnum::Status::TypeNotFound) {
            set_last_error(ERROR_RESOURCE_TYPE_NOT_FOUND); // ps_res_k32
            m.set_return(0);
            return;
        }
        if (langs.status == windowing::ResourceLanguageEnum::Status::NameNotFound) {
            set_last_error(ERROR_RESOURCE_NAME_NOT_FOUND); // ps_res_missing
            m.set_return(0);
            return;
        }
        const auto entry = by_id
            ? windowing::find_resource_by_named_type(
                  m_memory, module, rva, size, wave_type, 4, id)
            : windowing::find_resource_by_named_type_and_name(
                  m_memory, module, rva, size, wave_type, 4, name.c_str(),
                  static_cast<u16>(name.size()));
        if (entry.data == nullptr || entry.size == 0) {
            set_last_error(ERROR_RESOURCE_NAME_NOT_FOUND);
            m.set_return(0);
            return;
        }
        wav.assign(entry.data, entry.data + entry.size);
    } else if ((flags & SND_MEMORY) != 0) {
        // SND_MEMORY: a RIFF image in guest memory, sized by its RIFF header.
        std::vector<u8> head(12);
        if (m_memory.read_bytes(sound, reinterpret_cast<Byte*>(head.data()), 12)
                .is_err() ||
            std::memcmp(head.data(), "RIFF", 4) != 0) {
            m.set_return(0); // ps_mem_junk: le untouched
            return;
        }
        const u64 total = static_cast<u64>(le32(head, 4)) + 8;
        if (total > 256ULL * 1024ULL * 1024ULL) {
            m.set_return(0);
            return;
        }
        wav.resize(static_cast<usize>(total));
        if (m_memory.read_bytes(sound, reinterpret_cast<Byte*>(wav.data()),
                                wav.size()).is_err()) {
            m.set_return(0);
            return;
        }
    } else if ((flags & SND_ALIAS_ID) == SND_ALIAS_ID) {
        // SND_ALIAS_SYSTEM*: sndAlias('S', c) = 'S' | c << 8.
        const u32 alias = static_cast<u32>(sound);
        static constexpr char kSystemAliases[] = "*?HESW!D";
        const bool known = (alias & 0xFF) == 'S' && (alias >> 16) == 0 &&
            std::strchr(kSystemAliases, static_cast<int>(alias >> 8)) != nullptr &&
            (alias >> 8) != 0;
        if (!known) {
            set_last_error(ERROR_RESOURCE_NAME_NOT_FOUND); // ps_aliasid_bad
            m.set_return(0);
            return;
        }
        // System event sounds come from the HKCU\AppEvents scheme, which
        // Vitreo does not read: nothing is associated, and there is no
        // default sound to fall back to.
        WINMM_UNIMPLEMENTED("PlaySound: registry sound associations (HKCU\\AppEvents)");
        set_last_error(ERROR_FILE_NOT_FOUND);
        m.set_return(0);
        return;
    } else {
        std::string name;
        if (!read_name(name)) {
            m.set_return(0);
            return;
        }
        // An empty name is a stop request (ps_file_empty / ps_noflag_empty).
        if (name.empty()) {
            stop_sound(true);
            m.set_return(1);
            return;
        }
        if ((flags & SND_FILENAME) == 0) {
            // SND_ALIAS (0x10000) and flagless names consult the registry sound
            // associations first; an unassociated name is a file name
            // (ps_alias_file_path=1, ps_alias_missing le=2).
            WINMM_UNIMPLEMENTED("PlaySound: registry sound associations (HKCU\\AppEvents)");
        }
        // Learn search order: current directory, Windows directory, system
        // directory, PATH. A name with a directory part is opened as given.
        std::vector<std::string> candidates;
        const bool has_dir = name.find_first_of("\\/:") != std::string::npos;
        candidates.push_back(name);
        if (!has_dir) {
            candidates.push_back("C:\\Windows\\" + name);
            candidates.push_back("C:\\Windows\\System32\\" + name);
        }
        u32 error = ERROR_FILE_NOT_FOUND;
        bool found = false;
        std::string path;
        for (const auto& candidate : candidates) {
            auto st = m_vfs.stat(candidate);
            if (st.is_ok()) {
                if (st.value().size > 256ULL * 1024ULL * 1024ULL) break;
                path = candidate;
                found = true;
                break;
            }
            if (!has_dir) continue; // a bare name reports ERROR_FILE_NOT_FOUND
            error = vfs_error_to_win32(st.error()); // ps_missing_w_nodefault le=3
        }
        if (!found) {
            if (!has_dir) {
                WINMM_UNIMPLEMENTED("PlaySound: PATH directory search for a bare sound name");
            }
            set_last_error(error);
            m.set_return(0);
            return;
        }
        auto open = m_vfs.open(path, capsule::AccessMode::Read,
                               capsule::ShareMode::Read,
                               capsule::CreationDisposition::OpenExisting);
        if (open.is_err()) {
            set_last_error(vfs_error_to_win32(open.error()));
            m.set_return(0);
            return;
        }
        const capsule::FileHandle file = open.value();
        wav.resize(static_cast<usize>(m_vfs.stat(path).value().size));
        usize total = 0;
        while (total < wav.size()) {
            auto r = m_vfs.read(file, std::span<u8>(wav).subspan(total));
            if (r.is_err() || r.value() == 0) break;
            total += r.value();
        }
        (void)m_vfs.close(file);
        wav.resize(total);
    }

    // SND_NODEFAULT only matters when the sound is missing: Windows would
    // then play the default system sound, which Vitreo does not have, so
    // the call fails the same way (Learn: "If the function can find neither
    // the system default entry nor the default sound, it makes no sound and
    // returns FALSE").
    m.set_return(play_sound(flags, std::move(wav)) ? 1u : 0u);
}

void WinmmManager::impl_PlaySoundW(Marshaler& m) {
    play_sound_entry(m, m.arg_ptr(0), m.arg_ptr(1), m.arg32(2), true);
}

void WinmmManager::impl_PlaySoundA(Marshaler& m) {
    winmm_diag("PlaySoundA sound=0x%llx hmod=0x%llx flags=0x%x\n",
               static_cast<unsigned long long>(m.arg(0)),
               static_cast<unsigned long long>(m.arg(1)), m.arg32(2));
    play_sound_entry(m, m.arg_ptr(0), m.arg_ptr(1), m.arg32(2), false);
}

// sndPlaySound(lpszSound, fuSound) == PlaySound(lpszSound, NULL, fuSound)
// restricted to the flags sndPlaySound defines (snd_file / snd_missing /
// snd_mem / snd_null match the PlaySound results).
void WinmmManager::impl_sndPlaySoundA(Marshaler& m) {
    play_sound_entry(m, m.arg_ptr(0), 0,
                     m.arg32(1) & (SND_ASYNC | SND_NODEFAULT | SND_MEMORY |
                                   SND_LOOP | SND_NOSTOP), false);
}

void WinmmManager::impl_sndPlaySoundW(Marshaler& m) {
    play_sound_entry(m, m.arg_ptr(0), 0,
                     m.arg32(1) & (SND_ASYNC | SND_NODEFAULT | SND_MEMORY |
                                   SND_LOOP | SND_NOSTOP), true);
}

} // namespace vitreo::winapi
