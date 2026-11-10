/// @file winmm_mci.cpp
/// @brief MCI and the cdaudio device (see winmm_mci.hpp).

#include "winapi/media/winmm/winmm_mci.hpp"

#include "capsule/vfs/cd_image.hpp"
#include "capsule/vfs/vfs.hpp"
#include "winapi/core/marshaler.hpp"
#include "windowing/contracts/wave_output.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>

namespace vitreo::winapi {

namespace mci {
#include "winapi/media/winmm/mci_constants.inc"
} // namespace mci

namespace {

using namespace mci;

constexpr u32 kFps = capsule::CdImage::kFramesPerSecond;
constexpr u32 kMsfOffset = capsule::CdImage::kLbaToMsfOffset;
/// mcicda's lengths stop 2 frames short of the next address
/// (mci_cdaudio_probe: 1491- and 4420-sector discs report 1489 and 4418).
constexpr u32 kLengthShortfall = 2;
constexpr u32 kOutOfRangeReturn = 0xFF000000u;

struct ErrorText {
    u32 code;
    const char* text;
};
constexpr ErrorText kErrorTexts[] = {
#include "winapi/media/winmm/mci_error_text.inc"
};

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

u32 make_msf(u32 frames) {
    const u32 m = frames / (60 * kFps);
    const u32 s = (frames / kFps) % 60;
    const u32 f = frames % kFps;
    return (m & 0xFF) | ((s & 0xFF) << 8) | ((f & 0xFF) << 16);
}
u32 msf_frames(u32 msf) {
    return (msf & 0xFF) * 60 * kFps + ((msf >> 8) & 0xFF) * kFps + ((msf >> 16) & 0xFF);
}
u32 frames_to_ms(u32 frames) { return static_cast<u32>(static_cast<u64>(frames) * 1000 / kFps + 1); }
u32 ms_to_frames(u32 ms) { return static_cast<u32>(static_cast<u64>(ms) * kFps / 1000); }

const capsule::CdTrack* track_at(const capsule::CdImage& disc, u32 number) {
    for (const auto& t : disc.tracks()) {
        if (t.number == number) return &t;
    }
    return nullptr;
}
const capsule::CdTrack* track_containing(const capsule::CdImage& disc, u32 lba) {
    const capsule::CdTrack* found = nullptr;
    for (const auto& t : disc.tracks()) {
        if (t.start_lba <= lba) found = &t;
    }
    return found ? found : (disc.tracks().empty() ? nullptr : &disc.tracks().front());
}
u32 disc_start(const capsule::CdImage& disc) {
    return disc.tracks().empty() ? 0 : disc.tracks().front().start_lba;
}
/// Track length as mcicda reports it.
u32 track_length(const capsule::CdImage& disc, const capsule::CdTrack& track) {
    const auto& tracks = disc.tracks();
    const bool last = &track == &tracks.back();
    if (!last) {
        // A real drive's non-final track length could not be measured (the
        // oracle has no mixed-mode disc): the TOC distance to the next track.
        return (&track + 1)->start_lba - track.start_lba;
    }
    const u32 span = disc.leadout_lba() - track.start_lba;
    return span > kLengthShortfall ? span - kLengthShortfall : 0;
}
u32 disc_length(const capsule::CdImage& disc) {
    const u32 span = disc.leadout_lba() - disc_start(disc);
    return span > kLengthShortfall ? span - kLengthShortfall : 0;
}
bool has_audio(const capsule::CdImage& disc) {
    return std::any_of(disc.tracks().begin(), disc.tracks().end(),
                       [](const capsule::CdTrack& t) { return t.is_audio(); });
}

/// A position (LBA) in @p format.
u32 position_value(const capsule::CdImage& disc, u32 lba, u32 format) {
    switch (format) {
    case MCI_FORMAT_MILLISECONDS: return frames_to_ms(lba + kMsfOffset);
    case MCI_FORMAT_TMSF: {
        const capsule::CdTrack* t = track_containing(disc, lba);
        const u32 rel = t ? lba - t->start_lba : lba;
        return (t ? t->number : 1u) | (make_msf(rel) << 8);
    }
    default: return make_msf(lba + kMsfOffset);
    }
}
/// A length (frames) in @p format; TMSF lengths are packed as MSF.
u32 length_value(u32 frames, u32 format) {
    return format == MCI_FORMAT_MILLISECONDS ? frames_to_ms(frames) : make_msf(frames);
}

std::string two(u32 v) {
    char b[4];
    std::snprintf(b, sizeof b, "%02u", v % 100);
    return b;
}
std::string format_msf(u32 msf) {
    return two(msf & 0xFF) + ":" + two((msf >> 8) & 0xFF) + ":" + two((msf >> 16) & 0xFF);
}

const char* mode_name(u32 mode) {
    switch (mode) {
    case MCI_MODE_NOT_READY: return "not ready";
    case MCI_MODE_STOP: return "stopped";
    case MCI_MODE_PLAY: return "playing";
    case MCI_MODE_RECORD: return "recording";
    case MCI_MODE_SEEK: return "seeking";
    case MCI_MODE_PAUSE: return "paused";
    case MCI_MODE_OPEN: return "open";
    default: return "";
    }
}

/// MCI device types Windows ships that Vitreo has no driver for.
bool is_unimplemented_type(const std::string& name) {
    static const char* const kTypes[] = {"waveaudio", "sequencer", "avivideo", "mpegvideo",
                                         "videodisc", "vcr", "overlay", "scanner", "dat",
                                         "digitalvideo", "animation", "other", "mmmovie"};
    return std::any_of(std::begin(kTypes), std::end(kTypes),
                       [&](const char* t) { return name == t; });
}
const char* type_name_for_id(u32 id) {
    switch (id) {
    case MCI_DEVTYPE_VCR: return "vcr";
    case MCI_DEVTYPE_VIDEODISC: return "videodisc";
    case MCI_DEVTYPE_OVERLAY: return "overlay";
    case MCI_DEVTYPE_CD_AUDIO: return "cdaudio";
    case MCI_DEVTYPE_DAT: return "dat";
    case MCI_DEVTYPE_SCANNER: return "scanner";
    case MCI_DEVTYPE_ANIMATION: return "animation";
    case MCI_DEVTYPE_DIGITAL_VIDEO: return "digitalvideo";
    case MCI_DEVTYPE_OTHER: return "other";
    case MCI_DEVTYPE_WAVEFORM_AUDIO: return "waveaudio";
    case MCI_DEVTYPE_SEQUENCER: return "sequencer";
    default: return nullptr;
    }
}

void log_unimplemented_once(const std::string& what) {
    static std::mutex lock;
    static std::vector<std::string> seen;
    std::lock_guard g(lock);
    if (std::find(seen.begin(), seen.end(), what) != seen.end()) return;
    seen.push_back(what);
    std::fprintf(stderr, "[vitreo] UNIMPLEMENTED winmm!%s\n", what.c_str());
}

/// "D:", "d:\" -> 'D'; 0 when @p element is no drive.
char drive_of(const std::string& element) {
    if (element.size() < 2 || element[1] != ':' || !std::isalpha(static_cast<unsigned char>(element[0]))) {
        return 0;
    }
    if (element.size() > 3 || (element.size() == 3 && element[2] != '\\' && element[2] != '/')) {
        return 0;
    }
    return static_cast<char>(std::toupper(static_cast<unsigned char>(element[0])));
}

} // namespace

// ---------------------------------------------------------------------------
// Playback
// ---------------------------------------------------------------------------

/// Streams CD-DA frames [from, to) into a host output.
struct MciManager::Player {
    std::unique_ptr<windowing::WaveOutput> output;
    std::thread thread;
    std::atomic<bool> stop{false};
    std::mutex queue_mutex;
    std::condition_variable queue_cv;
    u32 queued = 0;
    ~Player() {
        stop = true;
        queue_cv.notify_all();
        if (output) output->reset();
        if (thread.joinable()) thread.join();
        if (output) output->close();
    }
};

MciManager::MciManager(capsule::Vfs& vfs, const OutputFactory& output_factory,
                       const WindowNotifier& notifier)
    : m_vfs(vfs), m_output_factory(output_factory), m_notifier(notifier) {}

MciManager::~MciManager() {
    {
        std::lock_guard lock(m_mutex);
        for (auto& [id, device] : m_devices) stop_player(device);
        m_devices.clear();
    }
    release_retired();
}

void MciManager::player_progress(u32 device_id, u64 generation, u32 lba, bool finished) {
    std::vector<std::pair<u64, u64>> notices;
    {
        std::lock_guard lock(m_mutex);
        Device* device = find(device_id);
        if (device == nullptr || device->play_generation != generation) return;
        device->position = lba;
        if (finished) {
            device->mode = Mode::Stopped;
            finish_pending_notify(*device, MCI_NOTIFY_SUCCESSFUL, notices);
            m_play_done.notify_all();
        }
    }
    post_notices(notices);
}

u32 MciManager::start_play(Device& device, u32 from, u32 to) {
    capsule::CdImage* disc = disc_for(device);
    if (disc == nullptr || !disc->has_raw_frames()) return MCIERR_HARDWARE;
    // A drive refuses to play audio from a data track (the oracle's
    // emulated ATAPI drive answers MCIERR_HARDWARE).
    const capsule::CdTrack* first = track_containing(*disc, from);
    if (first == nullptr || !first->is_audio()) return MCIERR_HARDWARE;
    if (!m_output_factory) return MCIERR_HARDWARE;
    auto output = m_output_factory();
    if (!output) return MCIERR_HARDWARE;
    stop_player(device);

    auto player = std::make_unique<Player>();
    Player* p = player.get();
    windowing::WaveOutputFormat format{2, 44100, 44100 * 4, 4, 16};
    const u32 id = device.id;
    const u64 generation = ++device.play_generation;
    const bool ok = output->open(format, [this, p, id, generation](u64 token, u32) {
        {
            std::lock_guard g(p->queue_mutex);
            if (p->queued > 0) --p->queued;
        }
        p->queue_cv.notify_all();
        const bool finished = (token >> 63) != 0;
        player_progress(id, generation, static_cast<u32>(token & 0xFFFFFFFFu), finished);
    });
    if (!ok) return MCIERR_HARDWARE;
    p->output = std::move(output);
    const bool mute_left = device.mute_left, mute_right = device.mute_right;
    device.mode = Mode::Playing;
    device.positioned = true;
    device.position = from;
    device.play_to = to;
    p->thread = std::thread([p, disc, from, to, mute_left, mute_right] {
        constexpr u32 kChunkFrames = 15;  // 0.2 s
        constexpr u32 kMaxQueued = 4;
        std::vector<u8> pcm(kChunkFrames * capsule::CdImage::kRawSectorSize);
        for (u32 lba = from; lba < to && !p->stop;) {
            const u32 count = std::min(kChunkFrames, to - lba);
            for (u32 i = 0; i < count; ++i) {
                std::span<u8> frame(pcm.data() + i * capsule::CdImage::kRawSectorSize,
                                    capsule::CdImage::kRawSectorSize);
                if (!disc->read_raw(lba + i, frame)) std::fill(frame.begin(), frame.end(), u8{0});
                if (mute_left || mute_right) {
                    for (u32 s = 0; s + 3 < frame.size(); s += 4) {
                        if (mute_left) { frame[s] = 0; frame[s + 1] = 0; }
                        if (mute_right) { frame[s + 2] = 0; frame[s + 3] = 0; }
                    }
                }
            }
            {
                std::unique_lock g(p->queue_mutex);
                p->queue_cv.wait(g, [&] { return p->stop || p->queued < kMaxQueued; });
                if (p->stop) break;
                ++p->queued;
            }
            lba += count;
            const u64 token = static_cast<u64>(lba) | (lba >= to ? (u64{1} << 63) : 0);
            if (!p->output->submit(token, std::span<const Byte>(
                    reinterpret_cast<const Byte*>(pcm.data()),
                    count * capsule::CdImage::kRawSectorSize))) {
                break;
            }
        }
    });
    device.player = std::move(player);
    return 0;
}

void MciManager::stop_player(Device& device) {
    if (!device.player) return;
    std::unique_ptr<Player> player = std::move(device.player);
    ++device.play_generation;  // late completions are ignored
    // The feeder stops at once; the stream is drained and joined after the
    // MCI lock is released, because its completion callback takes that lock.
    player->stop = true;
    player->queue_cv.notify_all();
    m_retired.push_back(std::move(player));
    m_play_done.notify_all();
}

void MciManager::release_retired() {
    std::vector<std::unique_ptr<Player>> retired;
    {
        std::lock_guard lock(m_mutex);
        retired.swap(m_retired);
    }
    retired.clear();
}

void MciManager::finish_pending_notify(Device& device, u32 flag,
                                       std::vector<std::pair<u64, u64>>& notices) {
    if (!device.notify_pending) return;
    device.notify_pending = false;
    notices.emplace_back(device.notify_hwnd, (static_cast<u64>(flag) << 32) | device.id);
}

void MciManager::post_notices(const std::vector<std::pair<u64, u64>>& notices) {
    if (!m_notifier) return;
    for (const auto& [hwnd, packed] : notices) {
        (void)m_notifier(hwnd, MM_MCINOTIFY, packed >> 32, packed & 0xFFFFFFFFu);
    }
}

// ---------------------------------------------------------------------------
// Devices
// ---------------------------------------------------------------------------

MciManager::Device* MciManager::find(u32 id) {
    const auto it = m_devices.find(id);
    return it == m_devices.end() ? nullptr : &it->second;
}

capsule::CdImage* MciManager::disc_for(const Device& device) {
    if (device.door_open) return nullptr;
    auto resolved = m_vfs.resolve_components(std::string(1, device.drive) + ":\\");
    if (resolved.is_err() || resolved.value().first == nullptr ||
        !resolved.value().first->is_optical()) {
        return nullptr;
    }
    return resolved.value().first->cd_image();
}

void MciManager::close_device(u32 id, std::vector<std::pair<u64, u64>>& notices) {
    Device* device = find(id);
    if (device == nullptr) return;
    finish_pending_notify(*device, MCI_NOTIFY_ABORTED, notices);
    stop_player(*device);
    m_devices.erase(id);
    m_play_done.notify_all();
}

u32 MciManager::open_device(Request& request, Reply& reply) {
    std::string type = lower(request.device_type);
    if ((request.flags & MCI_OPEN_TYPE) != 0 && (request.flags & MCI_OPEN_TYPE_ID) != 0) {
        const char* name = type_name_for_id(request.device_type_id & 0xFFFF);
        if (name == nullptr) return MCIERR_INVALID_DEVICE_NAME;
        type = name;
    }
    const std::string element = (request.flags & MCI_OPEN_ELEMENT) != 0 ? request.element : "";
    if (type.empty()) {
        if (element.empty()) return MCIERR_MISSING_PARAMETER;
        // An element alone picks its type from the [mci extensions] list; a
        // drive has no extension (mci_cdaudio_probe "open F: alias f").
        const auto dot = element.find_last_of('.');
        if (dot == std::string::npos || drive_of(element) != 0) return MCIERR_INVALID_DEVICE_NAME;
        const std::string ext = lower(element.substr(dot + 1));
        if (ext == "wav" || ext == "mid" || ext == "rmi" || ext == "avi" || ext == "mpg" ||
            ext == "mpeg" || ext == "mp3" || ext == "wma") {
            log_unimplemented_once("mciSendCommand: MCI device for ." + ext + " files");
            return MCIERR_CANNOT_LOAD_DRIVER;
        }
        return MCIERR_EXTENSION_NOT_FOUND;
    }
    if (type != "cdaudio") {
        if (is_unimplemented_type(type)) {
            log_unimplemented_once("mciSendCommand: MCI device type " + type);
            return MCIERR_CANNOT_LOAD_DRIVER;
        }
        return MCIERR_INVALID_DEVICE_NAME;
    }
    char drive = 0;
    if (!element.empty()) {
        drive = drive_of(element);
        if (drive == 0) return MCIERR_INVALID_FILE;
        auto resolved = m_vfs.resolve_components(std::string(1, drive) + ":\\");
        if (resolved.is_err() || resolved.value().first == nullptr ||
            !resolved.value().first->is_optical()) {
            return MCIERR_INVALID_FILE;  // mci_cdaudio_probe "open Q: type cdaudio"
        }
    } else {
        for (const char d : m_vfs.mounted_drives()) {
            auto resolved = m_vfs.resolve_components(std::string(1, d) + ":\\");
            if (resolved.is_ok() && resolved.value().first != nullptr &&
                resolved.value().first->is_optical()) {
                drive = static_cast<char>(std::toupper(static_cast<unsigned char>(d)));
                break;
            }
        }
        if (drive == 0) return MCIERR_DEVICE_NOT_INSTALLED;
    }
    const bool shareable = (request.flags & MCI_OPEN_SHAREABLE) != 0;
    std::string name = (request.flags & MCI_OPEN_ALIAS) != 0 ? request.alias
                       : !element.empty()                    ? element
                                                             : type;
    for (const auto& [id, other] : m_devices) {
        if (lower(other.name) == lower(name)) {
            return (request.flags & MCI_OPEN_ALIAS) != 0 ? MCIERR_DUPLICATE_ALIAS
                                                         : MCIERR_DEVICE_OPEN;
        }
        if (other.drive == drive && (!other.shareable || !shareable)) {
            return MCIERR_MUST_USE_SHAREABLE;
        }
    }
    u32 id = 1;
    while (m_devices.contains(id)) ++id;
    Device device;
    device.id = id;
    device.name = name;
    device.element = element;
    device.drive = drive;
    device.shareable = shareable;
    device.time_format = MCI_FORMAT_MSF;
    m_devices.emplace(id, std::move(device));
    Device& opened = m_devices.at(id);
    if (capsule::CdImage* disc = disc_for(opened)) {
        opened.position = disc_start(*disc);
        opened.mode = has_audio(*disc) ? Mode::Stopped : Mode::NotReady;
    } else {
        opened.mode = Mode::NotReady;
    }
    reply.kind = Reply::Kind::Integer;
    reply.value = id;
    return 0;
}

u32 MciManager::status(Device& device, const Request& request, Reply& reply) {
    if ((request.flags & MCI_STATUS_ITEM) == 0) return MCIERR_MISSING_PARAMETER;
    const bool with_track = (request.flags & MCI_TRACK) != 0;
    switch (request.item) {
    case MCI_STATUS_MODE: {
        reply.kind = Reply::Kind::Mode;
        const bool present = disc_for(device) != nullptr;
        u32 mode = MCI_MODE_NOT_READY;
        if (device.door_open || !present) mode = MCI_MODE_OPEN;
        else if (device.mode == Mode::Playing) mode = MCI_MODE_PLAY;
        else if (device.mode == Mode::Paused) mode = MCI_MODE_PAUSE;
        else if (device.mode == Mode::Stopped) mode = MCI_MODE_STOP;
        reply.value = mode;
        return 0;
    }
    case MCI_STATUS_MEDIA_PRESENT:
        reply.kind = Reply::Kind::Bool;
        reply.value = disc_for(device) != nullptr ? 1 : 0;
        return 0;
    case MCI_STATUS_TIME_FORMAT:
        reply.kind = Reply::Kind::TimeFormat;
        reply.value = device.time_format;
        return 0;
    case MCI_STATUS_READY:
        reply.kind = Reply::Kind::Bool;
        reply.value = disc_for(device) != nullptr &&
                      (device.mode == Mode::Stopped || device.mode == Mode::Playing ||
                       device.mode == Mode::Paused);
        return 0;
    default: break;
    }
    capsule::CdImage* disc = disc_for(device);
    if (disc == nullptr) return MCIERR_HARDWARE;
    const capsule::CdTrack* track = nullptr;
    const auto out_of_range = [&]() {
        reply.kind = Reply::Kind::Integer;
        reply.value = kOutOfRangeReturn;
        return MCIERR_OUTOFRANGE;
    };
    if (with_track) {
        track = track_at(*disc, request.track);
    }
    switch (request.item) {
    case MCI_STATUS_NUMBER_OF_TRACKS:
        reply.kind = Reply::Kind::Integer;
        reply.value = disc->tracks().size();
        return 0;
    case MCI_STATUS_CURRENT_TRACK: {
        const capsule::CdTrack* t = track_containing(*disc, device.position);
        reply.kind = Reply::Kind::Integer;
        reply.value = t ? t->number : 1;
        return 0;
    }
    case MCI_STATUS_LENGTH:
        if (with_track && track == nullptr) return out_of_range();
        reply.kind = Reply::Kind::Length;
        reply.time_format = device.time_format;
        reply.value = length_value(track ? track_length(*disc, *track) : disc_length(*disc),
                                   device.time_format);
        return 0;
    case MCI_STATUS_POSITION: {
        if (with_track && track == nullptr) return out_of_range();
        u32 lba = device.position;
        if (track != nullptr) lba = track->start_lba;
        else if ((request.flags & MCI_STATUS_START) != 0) lba = disc_start(*disc);
        reply.kind = Reply::Kind::Position;
        reply.time_format = device.time_format;
        reply.value = position_value(*disc, lba, device.time_format);
        return 0;
    }
    case MCI_CDA_STATUS_TYPE_TRACK:
        if (!with_track) {
            // Windows answers success without storing a value
            // (mci_cdaudio_probe status_type_notrack).
            reply.store_return = false;
            return 0;
        }
        if (track == nullptr) return out_of_range();
        reply.kind = Reply::Kind::TrackType;
        reply.value = track->is_audio() ? MCI_CDA_TRACK_AUDIO : MCI_CDA_TRACK_OTHER;
        return 0;
    default:
        return MCIERR_UNSUPPORTED_FUNCTION;
    }
}

u32 MciManager::device_command(Device& device, Request& request, Reply& reply,
                               std::vector<std::pair<u64, u64>>& notices) {
    const auto interrupt = [&] { finish_pending_notify(device, MCI_NOTIFY_ABORTED, notices); };
    switch (request.message) {
    case MCI_STATUS:
        return status(device, request, reply);
    case MCI_SET: {
        bool did = false;
        if ((request.flags & MCI_SET_TIME_FORMAT) != 0) {
            if (request.time_format != MCI_FORMAT_MILLISECONDS &&
                request.time_format != MCI_FORMAT_MSF && request.time_format != MCI_FORMAT_TMSF) {
                return MCIERR_BAD_TIME_FORMAT;
            }
            device.time_format = request.time_format;
            did = true;
        }
        if ((request.flags & MCI_SET_DOOR_OPEN) != 0) {
            interrupt();
            stop_player(device);
            device.door_open = true;
            device.mode = Mode::Open;
            did = true;
        }
        if ((request.flags & MCI_SET_DOOR_CLOSED) != 0) {
            device.door_open = false;
            capsule::CdImage* disc = disc_for(device);
            device.mode = disc && has_audio(*disc) ? Mode::Stopped : Mode::NotReady;
            device.positioned = false;
            if (disc) device.position = disc_start(*disc);
            did = true;
        }
        if ((request.flags & MCI_SET_AUDIO) != 0) {
            const bool on = (request.flags & MCI_SET_ON) != 0;
            const bool off = (request.flags & MCI_SET_OFF) != 0;
            if (on == off) return MCIERR_MISSING_PARAMETER;
            if (request.audio == MCI_SET_AUDIO_ALL || request.audio == MCI_SET_AUDIO_LEFT) device.mute_left = off;
            if (request.audio == MCI_SET_AUDIO_ALL || request.audio == MCI_SET_AUDIO_RIGHT) device.mute_right = off;
            if (request.audio > MCI_SET_AUDIO_RIGHT) return MCIERR_OUTOFRANGE;
            did = true;
        }
        if ((request.flags & MCI_SET_VIDEO) != 0) return MCIERR_UNSUPPORTED_FUNCTION;
        return did ? 0 : MCIERR_MISSING_PARAMETER;
    }
    case MCI_PLAY: {
        capsule::CdImage* disc = disc_for(device);
        if (disc == nullptr) return MCIERR_HARDWARE;
        const u32 start = disc_start(*disc);
        const u32 end = disc->leadout_lba();
        u32 from = device.position;
        u32 to = end;
        if ((request.flags & MCI_FROM) != 0 && parse_time(device, std::to_string(request.from), from) != 0) {
            return MCIERR_OUTOFRANGE;
        }
        if ((request.flags & MCI_TO) != 0 && parse_time(device, std::to_string(request.to), to) != 0) {
            return MCIERR_OUTOFRANGE;
        }
        if (from < start || from >= end || to > end || to <= from) return MCIERR_OUTOFRANGE;
        interrupt();
        const u32 error = start_play(device, from, to);
        if (error != 0) return error;
        if ((request.flags & MCI_NOTIFY) != 0) {
            device.notify_pending = true;
            device.notify_hwnd = request.callback;
            request.flags &= ~MCI_NOTIFY;  // posted when playback ends
        }
        if ((request.flags & MCI_WAIT) != 0) {
            const u64 generation = device.play_generation;
            const u32 id = device.id;
            m_play_done.wait(m_mutex, [&] {
                Device* d = find(id);
                return d == nullptr || d->play_generation != generation || d->mode != Mode::Playing;
            });
        }
        return 0;
    }
    case MCI_SEEK: {
        capsule::CdImage* disc = disc_for(device);
        if (disc == nullptr) return MCIERR_HARDWARE;
        u32 target = 0;
        if ((request.flags & MCI_SEEK_TO_START) != 0) target = disc_start(*disc);
        else if ((request.flags & MCI_SEEK_TO_END) != 0) target = disc->leadout_lba() - 1;
        else if ((request.flags & MCI_TO) != 0) {
            if (parse_time(device, std::to_string(request.to), target) != 0 ||
                target < disc_start(*disc) || target >= disc->leadout_lba()) {
                return MCIERR_OUTOFRANGE;
            }
        } else {
            return MCIERR_MISSING_PARAMETER;
        }
        interrupt();
        stop_player(device);
        device.position = target;
        device.positioned = true;
        device.mode = Mode::Stopped;
        return 0;
    }
    case MCI_STOP:
        if (disc_for(device) == nullptr) return MCIERR_HARDWARE;
        interrupt();
        stop_player(device);
        if (device.mode == Mode::Playing || device.mode == Mode::Paused) device.mode = Mode::Stopped;
        return 0;
    case MCI_PAUSE:
        if (disc_for(device) == nullptr) return MCIERR_HARDWARE;
        interrupt();
        stop_player(device);
        if (device.mode == Mode::Playing) device.mode = Mode::Paused;
        return 0;
    case MCI_RESUME: {
        capsule::CdImage* disc = disc_for(device);
        if (disc == nullptr) return MCIERR_HARDWARE;
        // Resume plays on from the current position, also after a stop
        // (mci_cdaudio_probe: stop, pause, resume -> "playing").
        const u32 to = device.play_to > device.position ? device.play_to : disc->leadout_lba();
        if (device.position >= to) return 0;
        return start_play(device, device.position, to);
    }
    case MCI_GETDEVCAPS: {
        if ((request.flags & MCI_GETDEVCAPS_ITEM) == 0) return MCIERR_MISSING_PARAMETER;
        reply.kind = Reply::Kind::Bool;
        switch (request.item) {
        case MCI_GETDEVCAPS_CAN_RECORD: reply.value = 0; return 0;
        case MCI_GETDEVCAPS_HAS_AUDIO: reply.value = 1; return 0;
        case MCI_GETDEVCAPS_HAS_VIDEO: reply.value = 0; return 0;
        case MCI_GETDEVCAPS_DEVICE_TYPE:
            reply.kind = Reply::Kind::DeviceType;
            reply.value = MCI_DEVTYPE_CD_AUDIO;
            return 0;
        case MCI_GETDEVCAPS_USES_FILES: reply.value = 0; return 0;
        case MCI_GETDEVCAPS_COMPOUND_DEVICE: reply.value = 0; return 0;
        case MCI_GETDEVCAPS_CAN_EJECT: reply.value = 1; return 0;
        case MCI_GETDEVCAPS_CAN_PLAY: reply.value = 1; return 0;
        case MCI_GETDEVCAPS_CAN_SAVE: reply.value = 0; return 0;
        default: return MCIERR_UNSUPPORTED_FUNCTION;
        }
    }
    case MCI_INFO: {
        reply.kind = Reply::Kind::Text;
        if ((request.flags & MCI_INFO_PRODUCT) != 0) {
            reply.text = "CD Audio";
            return 0;
        }
        if ((request.flags & (MCI_INFO_MEDIA_IDENTITY | MCI_INFO_MEDIA_UPC)) != 0) {
            capsule::CdImage* disc = disc_for(device);
            if (disc == nullptr) return MCIERR_HARDWARE;
            if ((request.flags & MCI_INFO_MEDIA_UPC) != 0) return MCIERR_NO_IDENTITY;
            if (disc->tracks().size() != 1) {
                // Measured only for one-track discs (start MSF packed plus the
                // lead-out LBA - 1: 2002 / 4931).
                log_unimplemented_once("mciSendCommand: MCI_INFO_MEDIA_IDENTITY of a multi-track disc");
                return MCIERR_NO_IDENTITY;
            }
            const auto& t = disc->tracks().front();
            const u32 packed = ((t.start_lba + kMsfOffset) / (60 * kFps)) << 16 |
                               (((t.start_lba + kMsfOffset) / kFps) % 60) << 8 |
                               ((t.start_lba + kMsfOffset) % kFps);
            reply.text = std::to_string(packed + disc->leadout_lba() - 1);
            return 0;
        }
        return MCIERR_MISSING_PARAMETER;
    }
    default:
        return MCIERR_UNSUPPORTED_FUNCTION;
    }
}

u32 MciManager::parse_time(const Device& device, const std::string& text, u32& lba) const {
    // Command-block values arrive as numbers; command strings as text.
    auto* self = const_cast<MciManager*>(this);
    capsule::CdImage* disc = self->disc_for(device);
    if (disc == nullptr) return MCIERR_HARDWARE;
    const bool numeric = !text.empty() &&
        std::all_of(text.begin(), text.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)); });
    u32 value = 0;
    if (numeric) {
        value = static_cast<u32>(std::stoul(text));
    }
    switch (device.time_format) {
    case MCI_FORMAT_MILLISECONDS: {
        if (!numeric) return MCIERR_BAD_INTEGER;
        const u32 frames = ms_to_frames(value);
        if (frames < kMsfOffset) return MCIERR_OUTOFRANGE;
        lba = frames - kMsfOffset;
        return 0;
    }
    case MCI_FORMAT_TMSF: {
        const u32 t = value & 0xFF;
        const capsule::CdTrack* track = track_at(*disc, t);
        if (track == nullptr) return MCIERR_OUTOFRANGE;
        lba = track->start_lba + msf_frames(value >> 8);
        return 0;
    }
    default: {
        const u32 frames = msf_frames(value);
        if (frames < kMsfOffset) return MCIERR_OUTOFRANGE;
        lba = frames - kMsfOffset;
        return 0;
    }
    }
}

u32 MciManager::dispatch(u32 device_id, Request& request, Reply& reply) {
    std::vector<std::pair<u64, u64>> notices;
    u32 error = 0;
    {
        std::lock_guard lock(m_mutex);
        if (request.message == MCI_OPEN) {
            error = open_device(request, reply);
        } else if (request.message == MCI_CLOSE) {
            if (device_id == MCI_ALL_DEVICE_ID || device_id == 0xFFFF) {
                std::vector<u32> ids;
                for (const auto& [id, d] : m_devices) ids.push_back(id);
                for (const u32 id : ids) close_device(id, notices);
            } else if (find(device_id) == nullptr) {
                error = MCIERR_UNSUPPORTED_FUNCTION;  // measured: close_again
            } else {
                close_device(device_id, notices);
            }
        } else if (request.message == MCI_SYSINFO) {
            const bool all_types = request.sysinfo_type == 0;
            const bool cd_type = all_types || request.sysinfo_type == MCI_DEVTYPE_CD_AUDIO;
            if ((request.flags & MCI_SYSINFO_QUANTITY) != 0) {
                reply.kind = Reply::Kind::Integer;
                reply.value = (request.flags & MCI_SYSINFO_OPEN) != 0
                                  ? (cd_type ? m_devices.size() : 0)
                                  : (cd_type ? 1 : 0);
            } else if ((request.flags & MCI_SYSINFO_NAME) != 0) {
                reply.kind = Reply::Kind::Text;
                if ((request.flags & MCI_SYSINFO_OPEN) != 0) {
                    if (request.number == 0 || request.number > m_devices.size()) {
                        error = MCIERR_OUTOFRANGE;
                    } else {
                        reply.text = std::next(m_devices.begin(), request.number - 1)->second.name;
                    }
                } else if (!cd_type || request.number != 1) {
                    error = MCIERR_OUTOFRANGE;
                } else {
                    reply.text = "CDAudio";
                }
            } else if ((request.flags & MCI_SYSINFO_INSTALLNAME) != 0) {
                if (find(device_id) == nullptr) error = MCIERR_INVALID_DEVICE_ID;
                reply.kind = Reply::Kind::Text;
                reply.text = "cdaudio";
            } else {
                error = MCIERR_MISSING_PARAMETER;
            }
        } else {
            Device* device = find(device_id);
            if (device == nullptr) {
                error = MCIERR_UNSUPPORTED_FUNCTION;  // measured: status_badid
            } else {
                error = device_command(*device, request, reply, notices);
                if (error == 0 && (request.flags & MCI_NOTIFY) != 0) {
                    // A command that does not interrupt playback supersedes
                    // the pending play notification.
                    if (request.message == MCI_STATUS || request.message == MCI_SET ||
                        request.message == MCI_INFO || request.message == MCI_GETDEVCAPS) {
                        finish_pending_notify(*device, MCI_NOTIFY_SUPERSEDED, notices);
                    }
                    notices.emplace_back(request.callback,
                                         (static_cast<u64>(MCI_NOTIFY_SUCCESSFUL) << 32) | device_id);
                }
            }
        }
        if (request.message == MCI_OPEN && error == 0 && (request.flags & MCI_NOTIFY) != 0) {
            notices.emplace_back(request.callback,
                                 (static_cast<u64>(MCI_NOTIFY_SUCCESSFUL) << 32) | reply.value);
        }
    }
    release_retired();
    post_notices(notices);
    return error;
}

const char* MciManager::error_text(u32 code) {
    for (const auto& e : kErrorTexts) {
        if (e.code == code) return e.text;
    }
    return nullptr;
}

u32 MciManager::device_id_for_name(const std::string& name) {
    std::lock_guard lock(m_mutex);
    if (lower(name) == "all") return MCI_ALL_DEVICE_ID;
    for (const auto& [id, device] : m_devices) {
        if (lower(device.name) == lower(name)) return id;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// mciSendString
// ---------------------------------------------------------------------------

namespace {

std::vector<std::string> tokenize(const std::string& s, bool& unterminated) {
    std::vector<std::string> out;
    unterminated = false;
    usize i = 0;
    while (i < s.size()) {
        while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
        if (i >= s.size()) break;
        std::string tok;
        if (s[i] == '"') {
            ++i;
            while (i < s.size() && s[i] != '"') tok += s[i++];
            if (i >= s.size()) {
                unterminated = true;
                return out;
            }
            ++i;
        } else {
            while (i < s.size() && !std::isspace(static_cast<unsigned char>(s[i]))) tok += s[i++];
        }
        out.push_back(tok);
    }
    return out;
}

} // namespace

u32 MciManager::send_string(const std::string& command, std::string& result, u64 hwnd_callback) {
    result.clear();
    bool unterminated = false;
    std::vector<std::string> tok = tokenize(command, unterminated);
    if (unterminated) return MCIERR_NO_CLOSING_QUOTE;
    if (tok.empty()) return MCIERR_MISSING_COMMAND_STRING;
    const std::string verb = lower(tok[0]);
    if (tok.size() < 2) return MCIERR_MISSING_DEVICE_NAME;
    const std::string device_name = tok[1];
    std::vector<std::string> args;
    for (usize i = 2; i < tok.size(); ++i) args.push_back(lower(tok[i]));

    Request request;
    request.callback = hwnd_callback;
    // Trailing "wait" / "notify" flags.
    std::vector<std::string> words;
    for (const auto& a : args) {
        if (a == "wait") request.flags |= MCI_WAIT;
        else if (a == "notify") request.flags |= MCI_NOTIFY;
        else words.push_back(a);
    }
    const auto take = [&](usize& i, std::initializer_list<const char*> phrase) {
        usize j = i;
        for (const char* w : phrase) {
            if (j >= words.size() || words[j] != w) return false;
            ++j;
        }
        i = j;
        return true;
    };

    if (verb == "open") {
        request.message = MCI_OPEN;
        const std::string lname = lower(device_name);
        if (lname == "cdaudio" || is_unimplemented_type(lname)) {
            request.flags |= MCI_OPEN_TYPE;
            request.device_type = lname;
        } else {
            request.flags |= MCI_OPEN_ELEMENT;
            request.element = device_name;
        }
        for (usize i = 0; i < words.size();) {
            if (words[i] == "alias" && i + 1 < words.size()) {
                request.flags |= MCI_OPEN_ALIAS;
                request.alias = tok[2 + (std::find(args.begin(), args.end(), words[i + 1]) - args.begin())];
                i += 2;
            } else if (words[i] == "type" && i + 1 < words.size()) {
                request.flags |= MCI_OPEN_TYPE;
                request.device_type = words[i + 1];
                i += 2;
            } else if (words[i] == "shareable") {
                request.flags |= MCI_OPEN_SHAREABLE;
                ++i;
            } else {
                return MCIERR_UNRECOGNIZED_KEYWORD;
            }
        }
        if ((request.flags & MCI_NOTIFY) == 0) request.callback = 0;
        Reply reply;
        const u32 error = dispatch(0, request, reply);
        if (error == 0) result = std::to_string(reply.value);
        return error;
    }

    static const char* const kVerbs[] = {"close", "status", "set", "play", "seek", "stop", "pause",
                                         "resume", "info", "capability", "sysinfo"};
    if (std::find(std::begin(kVerbs), std::end(kVerbs), verb) == std::end(kVerbs)) {
        return MCIERR_UNRECOGNIZED_COMMAND;
    }

    // sysinfo addresses a device type, "all" or an open device.
    if (verb == "sysinfo") {
        request.message = MCI_SYSINFO;
        const std::string lname = lower(device_name);
        u32 id = 0;
        if (lname == "all") {
            request.sysinfo_type = 0;
        } else if (lname == "cdaudio") {
            request.sysinfo_type = MCI_DEVTYPE_CD_AUDIO;
        } else if ((id = device_id_for_name(device_name)) == 0) {
            return MCIERR_INVALID_DEVICE_NAME;
        }
        usize i = 0;
        if (take(i, {"quantity"})) {
            request.flags |= MCI_SYSINFO_QUANTITY;
            if (take(i, {"open"})) request.flags |= MCI_SYSINFO_OPEN;
        } else if (take(i, {"name"}) && i < words.size()) {
            request.flags |= MCI_SYSINFO_NAME;
            request.number = static_cast<u32>(std::strtoul(words[i].c_str(), nullptr, 10));
            ++i;
            if (take(i, {"open"})) request.flags |= MCI_SYSINFO_OPEN;
        } else if (take(i, {"installname"})) {
            request.flags |= MCI_SYSINFO_INSTALLNAME;
        } else {
            return words.empty() ? MCIERR_MISSING_PARAMETER : MCIERR_UNRECOGNIZED_KEYWORD;
        }
        if (i != words.size()) return MCIERR_UNRECOGNIZED_KEYWORD;
        Reply reply;
        const u32 error = dispatch(id, request, reply);
        if (error == 0) {
            result = reply.kind == Reply::Kind::Text ? reply.text : std::to_string(reply.value);
        }
        return error;
    }

    // Resolve the device: an open name, "all" for close, or an auto-opened
    // device type.
    u32 id = 0;
    bool auto_opened = false;
    if (lower(device_name) == "all") {
        if (verb != "close") return MCIERR_CANNOT_USE_ALL;
        id = MCI_ALL_DEVICE_ID;
    } else {
        id = device_id_for_name(device_name);
        if (id == 0) {
            const std::string lname = lower(device_name);
            if (lname != "cdaudio" || verb == "close") {
                return is_unimplemented_type(lname) ? MCIERR_CANNOT_LOAD_DRIVER
                                                    : MCIERR_INVALID_DEVICE_NAME;
            }
            if ((request.flags & MCI_NOTIFY) != 0) return MCIERR_NOTIFY_ON_AUTO_OPEN;
            Request open;
            open.message = MCI_OPEN;
            open.flags = MCI_OPEN_TYPE;
            open.device_type = lname;
            Reply opened;
            const u32 error = dispatch(0, open, opened);
            if (error != 0) return error;
            id = static_cast<u32>(opened.value);
            auto_opened = true;
        }
    }
    const auto finish = [&](u32 error) {
        if (auto_opened) {
            Request close;
            close.message = MCI_CLOSE;
            Reply ignored;
            (void)dispatch(id, close, ignored);
        }
        return error;
    };
    if ((request.flags & MCI_NOTIFY) == 0) request.callback = 0;

    Reply reply;
    std::string time_text_from, time_text_to;
    usize i = 0;
    if (verb == "close") {
        request.message = MCI_CLOSE;
    } else if (verb == "status") {
        request.message = MCI_STATUS;
        request.flags |= MCI_STATUS_ITEM;
        if (words.empty()) return finish(MCIERR_MISSING_PARAMETER);
        if (take(i, {"length"})) request.item = MCI_STATUS_LENGTH;
        else if (take(i, {"position"})) request.item = MCI_STATUS_POSITION;
        else if (take(i, {"number", "of", "tracks"})) request.item = MCI_STATUS_NUMBER_OF_TRACKS;
        else if (take(i, {"mode"})) request.item = MCI_STATUS_MODE;
        else if (take(i, {"media", "present"})) request.item = MCI_STATUS_MEDIA_PRESENT;
        else if (take(i, {"ready"})) request.item = MCI_STATUS_READY;
        else if (take(i, {"current", "track"})) request.item = MCI_STATUS_CURRENT_TRACK;
        else if (take(i, {"start", "position"})) { request.item = MCI_STATUS_POSITION; request.flags |= MCI_STATUS_START; }
        else if (take(i, {"time", "format"})) request.item = MCI_STATUS_TIME_FORMAT;
        else if (take(i, {"type"})) request.item = MCI_CDA_STATUS_TYPE_TRACK;
        else return finish(MCIERR_BAD_CONSTANT);
        if (take(i, {"track"})) {
            if (i >= words.size()) return finish(MCIERR_MISSING_PARAMETER);
            char* end = nullptr;
            const unsigned long n = std::strtoul(words[i].c_str(), &end, 10);
            if (end == nullptr || *end != '\0') return finish(MCIERR_BAD_INTEGER);
            request.flags |= MCI_TRACK;
            request.track = static_cast<u32>(n);
            ++i;
        }
    } else if (verb == "set") {
        request.message = MCI_SET;
        while (i < words.size()) {
            if (take(i, {"time", "format"})) {
                if (i >= words.size()) return finish(MCIERR_MISSING_PARAMETER);
                const std::string& f = words[i++];
                request.flags |= MCI_SET_TIME_FORMAT;
                if (f == "milliseconds" || f == "ms") request.time_format = MCI_FORMAT_MILLISECONDS;
                else if (f == "msf") request.time_format = MCI_FORMAT_MSF;
                else if (f == "tmsf") request.time_format = MCI_FORMAT_TMSF;
                else return finish(MCIERR_BAD_CONSTANT);
            } else if (take(i, {"door", "open"})) {
                request.flags |= MCI_SET_DOOR_OPEN;
            } else if (take(i, {"door", "closed"})) {
                request.flags |= MCI_SET_DOOR_CLOSED;
            } else if (take(i, {"audio"})) {
                request.flags |= MCI_SET_AUDIO;
                if (take(i, {"all"})) request.audio = MCI_SET_AUDIO_ALL;
                else if (take(i, {"left"})) request.audio = MCI_SET_AUDIO_LEFT;
                else if (take(i, {"right"})) request.audio = MCI_SET_AUDIO_RIGHT;
                else return finish(MCIERR_BAD_CONSTANT);
                if (take(i, {"on"})) request.flags |= MCI_SET_ON;
                else if (take(i, {"off"})) request.flags |= MCI_SET_OFF;
                else return finish(MCIERR_MISSING_PARAMETER);
            } else {
                return finish(MCIERR_UNRECOGNIZED_KEYWORD);
            }
        }
    } else if (verb == "play") {
        request.message = MCI_PLAY;
        while (i < words.size()) {
            if (take(i, {"from"}) && i < words.size()) { time_text_from = words[i++]; request.flags |= MCI_FROM; }
            else if (take(i, {"to"}) && i < words.size()) { time_text_to = words[i++]; request.flags |= MCI_TO; }
            else return finish(MCIERR_UNRECOGNIZED_KEYWORD);
        }
    } else if (verb == "seek") {
        request.message = MCI_SEEK;
        if (take(i, {"to", "start"})) request.flags |= MCI_SEEK_TO_START;
        else if (take(i, {"to", "end"})) request.flags |= MCI_SEEK_TO_END;
        else if (take(i, {"to"}) && i < words.size()) { time_text_to = words[i++]; request.flags |= MCI_TO; }
        else return finish(MCIERR_MISSING_PARAMETER);
    } else if (verb == "stop") {
        request.message = MCI_STOP;
    } else if (verb == "pause") {
        request.message = MCI_PAUSE;
    } else if (verb == "resume") {
        request.message = MCI_RESUME;
    } else if (verb == "info") {
        request.message = MCI_INFO;
        if (take(i, {"product"})) request.flags |= MCI_INFO_PRODUCT;
        else if (take(i, {"identity"})) request.flags |= MCI_INFO_MEDIA_IDENTITY;
        else if (take(i, {"upc"})) request.flags |= MCI_INFO_MEDIA_UPC;
        else return finish(words.empty() ? MCIERR_MISSING_PARAMETER : MCIERR_UNRECOGNIZED_KEYWORD);
    } else if (verb == "capability") {
        request.message = MCI_GETDEVCAPS;
        request.flags |= MCI_GETDEVCAPS_ITEM;
        if (take(i, {"can", "play"})) request.item = MCI_GETDEVCAPS_CAN_PLAY;
        else if (take(i, {"can", "record"})) request.item = MCI_GETDEVCAPS_CAN_RECORD;
        else if (take(i, {"can", "save"})) request.item = MCI_GETDEVCAPS_CAN_SAVE;
        else if (take(i, {"can", "eject"})) request.item = MCI_GETDEVCAPS_CAN_EJECT;
        else if (take(i, {"device", "type"})) request.item = MCI_GETDEVCAPS_DEVICE_TYPE;
        else if (take(i, {"has", "audio"})) request.item = MCI_GETDEVCAPS_HAS_AUDIO;
        else if (take(i, {"has", "video"})) request.item = MCI_GETDEVCAPS_HAS_VIDEO;
        else if (take(i, {"uses", "files"})) request.item = MCI_GETDEVCAPS_USES_FILES;
        else if (take(i, {"compound", "device"})) request.item = MCI_GETDEVCAPS_COMPOUND_DEVICE;
        else return finish(words.empty() ? MCIERR_MISSING_PARAMETER : MCIERR_BAD_CONSTANT);
    }
    if (i != words.size()) return finish(MCIERR_UNRECOGNIZED_KEYWORD);

    // Times in a command string follow the device's time format.
    if (!time_text_from.empty() || !time_text_to.empty()) {
        std::lock_guard lock(m_mutex);
        Device* device = find(id);
        if (device == nullptr) return finish(MCIERR_INVALID_DEVICE_NAME);
        const auto parse = [&](const std::string& text, u32& out) -> u32 {
            // "tt:mm:ss:ff", "mm:ss:ff" and shorter forms pack into the
            // format's DWORD; milliseconds are a plain integer.
            std::vector<u32> parts;
            usize start = 0;
            while (true) {
                const auto colon = text.find(':', start);
                const std::string part = text.substr(start, colon - start);
                if (part.empty() || !std::all_of(part.begin(), part.end(),
                        [](char c) { return std::isdigit(static_cast<unsigned char>(c)); })) {
                    return MCIERR_BAD_INTEGER;
                }
                parts.push_back(static_cast<u32>(std::stoul(part)));
                if (colon == std::string::npos) break;
                start = colon + 1;
            }
            if (device->time_format == MCI_FORMAT_MILLISECONDS) {
                if (parts.size() != 1) return MCIERR_BAD_INTEGER;
                out = parts[0];
                return 0;
            }
            const usize max = device->time_format == MCI_FORMAT_TMSF ? 4 : 3;
            if (parts.size() > max) return MCIERR_BAD_INTEGER;
            u32 packed = 0;
            for (usize k = 0; k < parts.size(); ++k) {
                if (parts[k] > 0xFF) return MCIERR_OUTOFRANGE;
                packed |= parts[k] << (8 * k);
            }
            out = packed;
            return 0;
        };
        if (!time_text_from.empty()) {
            if (const u32 e = parse(time_text_from, request.from); e != 0) return finish(e);
        }
        if (!time_text_to.empty()) {
            if (const u32 e = parse(time_text_to, request.to); e != 0) return finish(e);
        }
    }

    const u32 error = dispatch(id, request, reply);
    if (error == 0) {
        switch (reply.kind) {
        case Reply::Kind::Integer: result = std::to_string(reply.value); break;
        case Reply::Kind::Bool: result = reply.value ? "true" : "false"; break;
        case Reply::Kind::Mode: result = mode_name(static_cast<u32>(reply.value)); break;
        case Reply::Kind::TimeFormat:
            result = reply.value == MCI_FORMAT_MILLISECONDS ? "milliseconds"
                     : reply.value == MCI_FORMAT_TMSF        ? "tmsf"
                                                             : "msf";
            break;
        case Reply::Kind::Position:
        case Reply::Kind::Length:
            if (reply.time_format == MCI_FORMAT_MILLISECONDS) {
                result = std::to_string(reply.value);
            } else if (reply.time_format == MCI_FORMAT_TMSF && reply.kind == Reply::Kind::Position) {
                const u32 v = static_cast<u32>(reply.value);
                result = two(v & 0xFF) + ":" + format_msf(v >> 8);
            } else {
                result = format_msf(static_cast<u32>(reply.value));
            }
            break;
        case Reply::Kind::TrackType:
            result = reply.value == MCI_CDA_TRACK_AUDIO ? "audio" : "other";
            break;
        case Reply::Kind::DeviceType: result = "cdaudio"; break;
        case Reply::Kind::Text: result = reply.text; break;
        case Reply::Kind::None: break;
        }
    }
    return finish(error);
}

// ---------------------------------------------------------------------------
// Exports
// ---------------------------------------------------------------------------

namespace {

/// Field offsets of the MCI parameter blocks (mmsystem.h, compiled for each
/// bitness; the x64 blocks are byte-packed).
struct Layout {
    bool x64;
    u32 ptr() const { return x64 ? 8 : 4; }
    // MCI_OPEN_PARMS
    u32 open_id() const { return x64 ? 8 : 4; }
    u32 open_type() const { return x64 ? 12 : 8; }
    u32 open_element() const { return x64 ? 20 : 12; }
    u32 open_alias() const { return x64 ? 28 : 16; }
    // MCI_STATUS_PARMS
    u32 status_return() const { return x64 ? 8 : 4; }
    u32 status_item() const { return x64 ? 16 : 8; }
    u32 status_track() const { return x64 ? 20 : 12; }
    // MCI_SET_PARMS
    u32 set_format() const { return x64 ? 8 : 4; }
    u32 set_audio() const { return x64 ? 12 : 8; }
    // MCI_PLAY_PARMS / MCI_SEEK_PARMS
    u32 play_from() const { return x64 ? 8 : 4; }
    u32 play_to() const { return x64 ? 12 : 8; }
    u32 seek_to() const { return x64 ? 8 : 4; }
    // MCI_GETDEVCAPS_PARMS
    u32 caps_return() const { return x64 ? 8 : 4; }
    u32 caps_item() const { return x64 ? 12 : 8; }
    // MCI_INFO_PARMS / MCI_SYSINFO_PARMS
    u32 info_return() const { return x64 ? 8 : 4; }
    u32 info_size() const { return x64 ? 16 : 8; }
    u32 sysinfo_number() const { return x64 ? 20 : 12; }
    u32 sysinfo_type() const { return x64 ? 24 : 16; }
};

std::optional<std::string> read_text(Marshaler& m, GuestAddr p, bool wide) {
    if (p == 0) return std::string();
    auto r = wide ? m.read_wide_string(p) : m.read_ansi_string(p);
    if (r.is_err()) return std::nullopt;
    return r.value();
}

/// Copy @p text into a cch-sized guest buffer; false when it does not fit.
bool write_text(Marshaler& m, GuestAddr p, u32 cch, const std::string& text, bool wide) {
    if (p == 0 || cch == 0) return text.empty();
    if (wide) {
        std::vector<u16> units;
        for (unsigned char c : text) units.push_back(c);
        if (units.size() + 1 > cch) return false;
        for (usize i = 0; i < units.size(); ++i) (void)m.write_u16(p + i * 2, units[i]);
        (void)m.write_u16(p + units.size() * 2, 0);
        return true;
    }
    if (text.size() + 1 > cch) return false;
    return m.write_ansi_string(p, text).is_ok();
}

} // namespace

void MciManager::impl_mciSendCommand(Marshaler& m, bool wide) {
    const u32 device_id = m.arg32(0);
    const u32 message = m.arg32(1);
    const u32 flags = m.arg32(2);
    const GuestAddr parms = m.arg_ptr(3);
    const Layout L{m.cpu_is_64bit()};
    Request request;
    request.message = message;
    request.flags = flags;

    // Messages that take no parameter block tolerate NULL; the rest need it.
    const bool needs_block = message == MCI_OPEN || message == MCI_STATUS ||
                             message == MCI_GETDEVCAPS || message == MCI_INFO ||
                             message == MCI_SYSINFO ||
                             (message == MCI_SET && flags != 0) ||
                             (message == MCI_PLAY && (flags & (MCI_FROM | MCI_TO)) != 0) ||
                             (message == MCI_SEEK && (flags & MCI_TO) != 0);
    if (parms == 0 && needs_block) {
        m.set_return(MCIERR_NULL_PARAMETER_BLOCK);
        return;
    }
    if (parms != 0) request.callback = m.read_ptr(parms).value_or(0);
    switch (message) {
    case MCI_OPEN: {
        const GuestAddr type = m.read_ptr(parms + L.open_type()).value_or(0);
        if ((flags & MCI_OPEN_TYPE) != 0) {
            if ((flags & MCI_OPEN_TYPE_ID) != 0) {
                request.device_type_id = static_cast<u32>(type);
            } else {
                auto t = read_text(m, type, wide);
                if (!t) { m.set_return(MCIERR_NULL_PARAMETER_BLOCK); return; }
                request.device_type = *t;
            }
        }
        if ((flags & MCI_OPEN_ELEMENT) != 0) {
            auto e = read_text(m, m.read_ptr(parms + L.open_element()).value_or(0), wide);
            request.element = e.value_or("");
        }
        if ((flags & MCI_OPEN_ALIAS) != 0) {
            auto a = read_text(m, m.read_ptr(parms + L.open_alias()).value_or(0), wide);
            request.alias = a.value_or("");
        }
        break;
    }
    case MCI_STATUS:
        request.item = m.read_u32(parms + L.status_item()).value_or(0);
        request.track = m.read_u32(parms + L.status_track()).value_or(0);
        break;
    case MCI_SET:
        if (parms != 0) {
            request.time_format = m.read_u32(parms + L.set_format()).value_or(0);
            request.audio = m.read_u32(parms + L.set_audio()).value_or(0);
        }
        break;
    case MCI_PLAY:
        if (parms != 0) {
            request.from = m.read_u32(parms + L.play_from()).value_or(0);
            request.to = m.read_u32(parms + L.play_to()).value_or(0);
        }
        break;
    case MCI_SEEK:
        if (parms != 0) request.to = m.read_u32(parms + L.seek_to()).value_or(0);
        break;
    case MCI_GETDEVCAPS:
        request.item = m.read_u32(parms + L.caps_item()).value_or(0);
        break;
    case MCI_SYSINFO:
        request.number = m.read_u32(parms + L.sysinfo_number()).value_or(0);
        request.sysinfo_type = m.read_u32(parms + L.sysinfo_type()).value_or(0);
        break;
    default:
        break;
    }

    Reply reply;
    const u32 error = dispatch(device_id, request, reply);
    VITREO_TRACE("[winmm] mciSendCommand(id=%u msg=0x%x flags=0x%x type='%s'/0x%x element='%s' "
                 "item=0x%x track=%u fmt=%u from=0x%x to=0x%x) -> 0x%x value=0x%llx '%s'\n",
                 device_id, message, flags, request.device_type.c_str(), request.device_type_id,
                 request.element.c_str(), request.item, request.track, request.time_format,
                 request.from, request.to, error, static_cast<unsigned long long>(reply.value),
                 reply.text.c_str());
    switch (message) {
    case MCI_OPEN:
        if (error == 0) (void)m.write_u32(parms + L.open_id(), static_cast<u32>(reply.value));
        break;
    case MCI_STATUS:
        if ((error == 0 && reply.store_return && reply.kind != Reply::Kind::None) ||
            (error == MCIERR_OUTOFRANGE && reply.kind == Reply::Kind::Integer)) {
            (void)(L.x64 ? m.write_u64(parms + L.status_return(), reply.value)
                         : m.write_u32(parms + L.status_return(), static_cast<u32>(reply.value)));
        }
        break;
    case MCI_GETDEVCAPS:
        if (error == 0) (void)m.write_u32(parms + L.caps_return(), static_cast<u32>(reply.value));
        break;
    case MCI_INFO:
    case MCI_SYSINFO:
        if (error == 0) {
            const GuestAddr buffer = m.read_ptr(parms + L.info_return()).value_or(0);
            const u32 cch = m.read_u32(parms + L.info_size()).value_or(0);
            if (message == MCI_SYSINFO && (flags & MCI_SYSINFO_QUANTITY) != 0) {
                // Learn: the quantity is returned in lpstrReturn as a DWORD.
                if (buffer != 0) (void)m.write_u32(buffer, static_cast<u32>(reply.value));
            } else if (!write_text(m, buffer, cch, reply.text, wide)) {
                m.set_return(MCIERR_PARAM_OVERFLOW);
                return;
            }
        }
        break;
    default:
        break;
    }
    m.set_return(error);
}

void MciManager::impl_mciSendString(Marshaler& m, bool wide) {
    const GuestAddr command = m.arg_ptr(0);
    const GuestAddr ret = m.arg_ptr(1);
    const u32 cch = m.arg32(2);
    const u64 hwnd = m.arg(3);
    auto text = read_text(m, command, wide);
    if (!text || command == 0) {
        m.set_return(MCIERR_MISSING_COMMAND_STRING);
        return;
    }
    std::string result;
    const u32 error = send_string(*text, result, hwnd);
    VITREO_TRACE("[winmm] mciSendString(\"%s\") -> 0x%x \"%s\"\n", text->c_str(), error,
                 result.c_str());
    if (ret != 0 && cch != 0) {
        if (!write_text(m, ret, cch, error == 0 ? result : std::string(), wide)) {
            m.set_return(MCIERR_PARAM_OVERFLOW);
            return;
        }
    }
    m.set_return(error);
}

void MciManager::impl_mciGetErrorString(Marshaler& m, bool wide) {
    const u32 code = m.arg32(0);
    const GuestAddr buffer = m.arg_ptr(1);
    const u32 cch = m.arg32(2);
    const char* text = error_text(code);
    // Measured: a code without a message stores an empty string and returns
    // FALSE; a short buffer gets the truncated text and TRUE.
    if (buffer != 0 && cch != 0) {
        std::string s = text ? text : "";
        if (s.size() + 1 > cch) s.resize(cch - 1);
        (void)write_text(m, buffer, cch, s, wide);
    }
    m.set_return(text != nullptr ? 1u : 0u);
}

void MciManager::impl_mciGetDeviceID(Marshaler& m, bool wide) {
    auto name = read_text(m, m.arg_ptr(0), wide);
    m.set_return(name ? device_id_for_name(*name) : 0u);
}

void MciManager::impl_mciGetDeviceIDFromElementID(Marshaler& m, bool wide) {
    // cdaudio devices are opened by element name, never by element id, so no
    // open device matches (Learn: 0 when none is found).
    (void)m.arg32(0);
    (void)read_text(m, m.arg_ptr(1), wide);
    m.set_return(0u);
}

void MciManager::impl_mciExecute(Marshaler& m) {
    // BOOL mciExecute(LPCSTR): mciSendString, and an error message box on
    // failure. Vitreo has no message box path from winmm yet.
    auto text = read_text(m, m.arg_ptr(0), false);
    std::string result;
    const u32 error = text ? send_string(*text, result, 0) : MCIERR_MISSING_COMMAND_STRING;
    if (error != 0) log_unimplemented_once("mciExecute: error message box");
    m.set_return(error == 0 ? 1u : 0u);
}

void MciManager::impl_mciGetCreatorTask(Marshaler& m) {
    (void)m.arg32(0);
    log_unimplemented_once("mciGetCreatorTask");
    m.set_return(0u);
}

void MciManager::impl_mciGetYieldProc(Marshaler& m) {
    (void)m.arg32(0);
    (void)m.arg_ptr(1);
    log_unimplemented_once("mciGetYieldProc");
    m.set_return(0u);
}

void MciManager::impl_mciSetYieldProc(Marshaler& m) {
    (void)m.arg32(0);
    (void)m.arg_ptr(1);
    (void)m.arg32(2);
    log_unimplemented_once("mciSetYieldProc");
    m.set_return(0u);
}

} // namespace vitreo::winapi
