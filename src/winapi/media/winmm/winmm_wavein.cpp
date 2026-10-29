/// @file winmm_wavein.cpp
/// @brief waveIn (capture) device APIs.
///
/// Format check, caps, the WaveInStream and its pump, and the waveIn* entry
/// points. Split out of winmm.cpp.

#include "winapi/media/winmm/winmm_internal.hpp"
#include "audio/contracts/audio_capture.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/kernel32/locale/string_conv.hpp"
#include <algorithm>
#include <cstring>
#include <deque>
#include <mutex>

namespace vitreo::winapi {

// ---------------------------------------------------------------------------
// waveIn (capture)
//
// Sources: Learn (waveIn* and the waveInProc / WIM_* pages), mmeapi.h and
// mmsyscom.h (wavein_contract.inc, generated), and the Windows runs
// tools/oracle_probe/audio_capture_contract_probe{32,64}.expected for what
// Learn leaves open: argument-check order, out-parameters on failure, the
// device order (default first), the caps byte counts, header flag
// transitions, and the Stop/Reset buffer rules.
// ---------------------------------------------------------------------------
namespace {
namespace wic {
#include "winapi/media/winmm/wavein_contract.inc"
}  // namespace wic
static_assert(wic::WAVERR_BADFORMAT == WAVERR_BADFORMAT);
static_assert(wic::WAVERR_STILLPLAYING == WAVERR_STILLPLAYING);
static_assert(wic::WAVERR_UNPREPARED == WAVERR_UNPREPARED);
static_assert(wic::DRVM_MAPPER_PREFERRED_GET == DRVM_MAPPER_PREFERRED_GET);
static_assert(wic::DRVM_MAPPER_CONSOLEVOICECOM_GET == DRVM_MAPPER_CONSOLEVOICECOM_GET);

constexpr u32 TIME_MS_ = 0x0001;

/// The capture format a waveIn stream delivers, or the MMRESULT that refuses
/// it. `opening` adds the nBlockAlign check an open makes (measured: a wrong
/// nBlockAlign is MMSYSERR_INVALPARAM with *phwi = NULL).
struct WaveInFormat {
    u32 result{WAVERR_BADFORMAT};
    audio::PcmFormat pcm{};
};

[[nodiscard]] WaveInFormat check_wavein_format(const std::vector<u8>& fmt, bool opening) {
    WaveInFormat out;
    if (fmt.size() < 16) return out;
    const u16 tag = le16(fmt, 0);
    const u16 channels = le16(fmt, 2);
    const u32 rate = le32(fmt, 4);
    const u16 align = le16(fmt, 12);
    const u16 bits = le16(fmt, 14);
    if (channels == 0 || rate == 0) return out;
    const auto pcm_bits_ok = [](u16 b) { return b == 8 || b == 16 || b == 24 || b == 32; };
    bool is_float = false;
    switch (tag) {
    case WAVE_FORMAT_PCM:
        if (!pcm_bits_ok(bits) || channels > 2) return out;
        break;
    case WAVE_FORMAT_IEEE_FLOAT:
        if (bits != 32 || channels > 2) {
            if (bits == 64) WINMM_UNIMPLEMENTED("waveInOpen: 64-bit IEEE float capture");
            return out;
        }
        is_float = true;
        break;
    case WAVE_FORMAT_EXTENSIBLE: {
        if (fmt.size() < 40 || le16(fmt, 16) < 22) return out;
        const u16 valid = le16(fmt, 18);
        const u32 sub = le32(fmt, 24);
        if (std::memcmp(fmt.data() + 28, kSubtypeTail, 12) != 0) return out;
        if (valid > bits) return out;
        if (sub == WAVE_FORMAT_PCM) {
            if (!pcm_bits_ok(bits)) return out;
        } else if (sub == WAVE_FORMAT_IEEE_FLOAT) {
            if (bits != 32) return out;
            is_float = true;
        } else {
            return out;
        }
        if (channels > audio::kCaptureMaxChannels) return out;
        break;
    }
    default:
        WINMM_UNIMPLEMENTED("waveInOpen: ACM-converted (non-PCM) format tag");
        return out;
    }
    if (opening && align != channels * (bits / 8u)) {
        out.result = MMSYSERR_INVALPARAM;
        return out;
    }
    out.pcm = audio::PcmFormat{rate, channels, bits, is_float, 0};
    out.result = out.pcm.valid_stream(audio::kCaptureMaxChannels) ? MMSYSERR_NOERROR
                                                                  : WAVERR_BADFORMAT;
    return out;
}

/// WAVEINCAPS2A/W of a device or of the wave mapper.
[[nodiscard]] std::vector<u8> build_wavein_caps(const audio::CaptureDevice* device,
                                                bool wide) {
    const usize name_bytes = wide ? 64 : 32;
    const usize prefix = 8 + name_bytes + 8;
    std::vector<u8> b(prefix + 48, 0);
    put16(b, 0, static_cast<u16>(wic::kWaveInDeviceMid));
    if (device == nullptr) {
        put16(b, 2, static_cast<u16>(wic::kWaveInMapperPid));
        put32(b, 4, wic::kWaveInMapperVersion);
        put_name(b, 8, kWaveMapperName, wide);
        put32(b, 8 + name_bytes, wic::kWaveInFormats);
        put16(b, 12 + name_bytes, 2);
        return b;
    }
    put16(b, 2, static_cast<u16>(wic::MM_MSFT_WDMAUDIO_WAVEIN));
    put32(b, 4, wic::kWaveInDeviceVersion);
    // szPname: the endpoint friendly name cut to MAXPNAMELEN - 1 characters.
    std::vector<u16> units(device->name.begin(), device->name.end());
    units.push_back(0);
    if (wide) {
        const usize n = std::min<usize>(units.size() - 1, wic::MAXPNAMELEN - 1);
        for (usize i = 0; i < n; ++i) put16(b, 8 + i * 2, units[i]);
    } else {
        const std::string ansi = utf8_to_acp(utf16_to_utf8(units.data()));
        const usize n = std::min<usize>(ansi.size(), wic::MAXPNAMELEN - 1);
        std::memcpy(b.data() + 8, ansi.data(), n);
    }
    put32(b, 8 + name_bytes, wic::kWaveInFormats);
    // The input's channel count (the endpoint's; Learn names 1 and 2, and a
    // device of more channels reports them all).
    put16(b, 12 + name_bytes, device->channels);
    put_mmreg_guid(b, prefix, 0xd5a47fa7, 0x6d98, MM_MICROSOFT);
    put_mmreg_guid(b, prefix + 16, 0xe36dc2ac, 0x6d9a,
                   static_cast<u16>(wic::MM_MSFT_WDMAUDIO_WAVEIN));
    return b;
}

}  // namespace

struct WinmmManager::WaveInStream {
    u64 handle{0};
    u32 device_id{0};  ///< As passed to waveInOpen (WAVE_MAPPER kept).
    bool is_64bit{false};
    GuestAddr callback{0};
    u64 instance{0};
    u32 callback_type{CALLBACK_NULL};
    u16 block_align{0};
    std::shared_ptr<audio::IAudioCaptureStream> stream;
    /// Headers in AddBuffer order; the front one is being filled.
    std::deque<GuestAddr> queue;
    u32 head_recorded{0};
    bool started{false};
    /// Bytes captured since open or the last waveInReset (waveInGetPosition).
    u64 position_bytes{0};
    std::vector<u8> scratch;

    [[nodiscard]] WaveCallbackTarget target() const {
        return WaveCallbackTarget{handle, callback, callback_type, instance, is_64bit};
    }
};

void WinmmManager::close_all_wavein() {
    std::vector<std::shared_ptr<audio::IAudioCaptureStream>> hosts;
    {
        std::lock_guard<std::mutex> guard(m_wave_mutex);
        for (auto& [handle, stream] : m_wavein_streams) {
            (void)handle;
            stream->started = false;
            if (stream->stream) hosts.push_back(std::move(stream->stream));
        }
        m_wavein_streams.clear();
    }
    for (auto& host : hosts) host->stop();
}

std::shared_ptr<WinmmManager::WaveInStream> WinmmManager::find_wavein(u64 handle) {
    std::lock_guard<std::mutex> guard(m_wave_mutex);
    const auto it = m_wavein_streams.find(handle);
    return it == m_wavein_streams.end() ? nullptr : it->second;
}

void WinmmManager::complete_wavein_head_locked(WaveInStream& stream) {
    if (stream.queue.empty()) return;
    const GuestAddr header = stream.queue.front();
    stream.queue.pop_front();
    const WaveHdrLayout& layout = wavehdr_layout(stream.is_64bit);
    (void)m_memory.write_u32(header + layout.bytes_recorded_offset, stream.head_recorded);
    stream.head_recorded = 0;
    // WHDR_DONE set and WHDR_INQUEUE cleared before WIM_DATA is delivered.
    (void)enqueue_wave_event_locked(stream.target(), wic::WIM_DATA, header, true);
}

void WinmmManager::wavein_pump(u64 handle) {
    std::lock_guard<std::mutex> guard(m_wave_mutex);
    const auto it = m_wavein_streams.find(handle);
    if (it == m_wavein_streams.end() || !it->second->stream) return;
    WaveInStream& stream = *it->second;
    audio::CaptureRing& ring = stream.stream->ring();
    const u32 frames = ring.available();
    if (frames == 0) return;
    stream.scratch.resize(static_cast<usize>(frames) * stream.block_align);
    const u32 got = ring.read(stream.scratch.data(), frames);
    if (!stream.started) return;
    const usize bytes = static_cast<usize>(got) * stream.block_align;
    stream.position_bytes += bytes;
    const WaveHdrLayout& layout = wavehdr_layout(stream.is_64bit);
    usize offset = 0;
    // Buffers are filled to the byte (measured: a 3201-byte buffer records
    // 3201 bytes); with no buffer queued the data is thrown away (Learn
    // waveInStart).
    while (!stream.queue.empty()) {
        const GuestAddr header = stream.queue.front();
        const u32 length = m_memory.read_u32(header + layout.buffer_length_offset).value_or(0);
        const GuestAddr data = stream.is_64bit
            ? static_cast<GuestAddr>(m_memory.read_u64(header + layout.data_offset).value_or(0))
            : m_memory.read_u32(header + layout.data_offset).value_or(0);
        const usize room = length > stream.head_recorded ? length - stream.head_recorded : 0;
        const usize n = std::min(room, bytes - offset);
        if (n != 0 && data != 0) {
            (void)m_memory.write_bytes(data + stream.head_recorded,
                                       reinterpret_cast<const Byte*>(stream.scratch.data() + offset),
                                       n);
        }
        stream.head_recorded += static_cast<u32>(n);
        offset += n;
        if (stream.head_recorded < length) {
            (void)m_memory.write_u32(header + layout.bytes_recorded_offset, stream.head_recorded);
            break;
        }
        complete_wavein_head_locked(stream);
        if (offset == bytes) break;
    }
}

void WinmmManager::impl_waveInGetNumDevs(Marshaler& m) {
    m.set_return(m_capture ? static_cast<u32>(m_capture->devices().size()) : 0u);
}

// waveInGetDevCapsA/W(UINT_PTR uDeviceID, LPWAVEINCAPS pwic, UINT cbwic).
// Measured (wi GetDevCaps*): cbwic == 0 succeeds and copies nothing; a NULL
// buffer is MMSYSERR_INVALPARAM; an id that is neither a device, the mapper
// nor an open waveIn handle is MMSYSERR_BADDEVICEID; W needs a whole
// WAVEINCAPSW (80 bytes). A device W query zero-fills the caller's buffer
// and copies WAVEINCAPS2W; the mapper W copies WAVEINCAPSW; A copies
// min(cbwic, sizeof(WAVEINCAPS2A)).
void WinmmManager::impl_waveInGetDevCaps(Marshaler& m, bool wide) {
    const u64 raw_id = m.arg(0);
    const GuestAddr pwic = m.arg_ptr(1);
    const u32 cbwic = m.arg32(2);
    if (cbwic == 0) {
        m.set_return(MMSYSERR_NOERROR);
        return;
    }
    if (pwic == 0) {
        m.set_return(MMSYSERR_INVALPARAM);
        return;
    }
    std::vector<audio::CaptureDevice> devices;
    if (m_capture) devices = audio::capture_devices_in_id_order(*m_capture);
    bool mapper = is_mapper_id(raw_id);
    u64 id = raw_id;
    if (!mapper) {
        if (const auto stream = find_wavein(raw_id)) {
            id = stream->device_id;
            mapper = id == WAVE_MAPPER;
        }
    }
    if (!mapper && id >= devices.size()) {
        m.set_return(MMSYSERR_BADDEVICEID);
        return;
    }
    if (wide && cbwic < wic::kWaveInCapsWBytes) {
        m.set_return(MMSYSERR_INVALPARAM);
        return;
    }
    const std::vector<u8> caps =
        build_wavein_caps(mapper ? nullptr : &devices[static_cast<usize>(id)], wide);
    usize copy = 0;
    if (!wide) {
        copy = std::min<usize>(cbwic, wic::kWaveInCaps2ABytes);
    } else if (mapper) {
        copy = wic::kWaveInCapsMapperWBytes;
    } else {
        std::vector<u8> zeros(cbwic, 0);
        (void)m.memory().write_bytes(pwic, reinterpret_cast<const Byte*>(zeros.data()),
                                     zeros.size());
        copy = std::min<usize>(cbwic, wic::kWaveInCaps2WBytes);
    }
    (void)m.memory().write_bytes(pwic, reinterpret_cast<const Byte*>(caps.data()),
                                 std::min(copy, caps.size()));
    m.set_return(MMSYSERR_NOERROR);
}

// waveInOpen(LPHWAVEIN phwi, UINT uDeviceID, LPCWAVEFORMATEX pwfx,
//            DWORD_PTR dwCallback, DWORD_PTR dwInstance, DWORD fdwOpen)
void WinmmManager::impl_waveInOpen(Marshaler& m) {
    const GuestAddr phwi = m.arg_ptr(0);
    const u32 device_id = m.arg32(1);
    const GuestAddr pwfx = m.arg_ptr(2);
    const u64 callback = m.arg(3);
    const u64 instance = m.arg(4);
    const u32 flags = m.arg32(5);
    const u32 callback_type = flags & CALLBACK_TYPEMASK;
    const bool query = (flags & WAVE_FORMAT_QUERY) != 0;

    // Measured: pointer and window checks leave *phwi untouched.
    if (pwfx == 0 || (!query && phwi == 0)) {
        m.set_return(MMSYSERR_INVALPARAM);
        return;
    }
    if (callback_type == CALLBACK_WINDOW && callback != 0) {
        WindowValidator validator;
        {
            std::lock_guard<std::mutex> guard(m_wave_mutex);
            validator = m_window_validator;
        }
        if (validator && !validator(callback)) {
            m.set_return(MMSYSERR_INVALPARAM);
            return;
        }
    }
    const auto fail = [&](u32 code) {
        if (!query && phwi != 0) (void)m.write_ptr(phwi, 0);
        m.set_return(code);
    };
    // Learn: WAVE_MAPPED_DEFAULT_COMMUNICATION_DEVICE only with WAVE_MAPPER
    // (measured MMSYSERR_INVALFLAG otherwise).
    if ((flags & wic::WAVE_MAPPED_DEFAULT_COMMUNICATION_DEVICE) != 0 &&
        device_id != WAVE_MAPPER) {
        fail(wic::MMSYSERR_INVALFLAG);
        return;
    }
    std::vector<audio::CaptureDevice> devices;
    if (m_capture) devices = audio::capture_devices_in_id_order(*m_capture);
    if (devices.empty() || (device_id != WAVE_MAPPER && device_id >= devices.size())) {
        fail(MMSYSERR_BADDEVICEID);
        return;
    }
    const std::vector<u8> fmt = read_guest_wave_format(m_memory, pwfx);
    if (fmt.empty()) {
        fail(MMSYSERR_INVALPARAM);
        return;
    }
    const WaveInFormat check = check_wavein_format(fmt, !query);
    if (check.result != MMSYSERR_NOERROR) {
        fail(check.result);
        return;
    }
    // The mapper (and the default communication device, which Vitreo maps to
    // the same host default) is device id 0.
    const audio::CaptureDevice& device =
        devices[device_id == WAVE_MAPPER ? 0u : device_id];
    if (!audio::capture_channels_convertible(device.channels, check.pcm.channels)) {
        // The host converts mono<->stereo exactly; another channel mix would
        // need a speaker matrix it does not make.
        WINMM_UNIMPLEMENTED("waveInOpen: channel conversion other than mono<->stereo or none");
        fail(WAVERR_BADFORMAT);
        return;
    }
    if (query) {
        m.set_return(MMSYSERR_NOERROR);
        return;
    }
    if (!audio::capture_permitted(*m_capture, "waveInOpen")) {
        fail(MMSYSERR_NODRIVER);
        return;
    }
    auto stream = std::make_shared<WaveInStream>();
    {
        std::lock_guard<std::mutex> guard(m_wave_mutex);
        stream->handle = m_next_wave_handle++;
    }
    stream->device_id = device_id;
    stream->is_64bit = m.cpu_is_64bit();
    stream->callback = callback;
    stream->instance = instance;
    stream->callback_type = callback_type;
    stream->block_align = static_cast<u16>(check.pcm.block_align());
    const u64 handle = stream->handle;
    stream->stream = std::shared_ptr<audio::IAudioCaptureStream>(m_capture->open(
        device.uid, check.pcm, std::max<u32>(check.pcm.sample_rate / 2u, 1024u),
        [this, handle] { wavein_pump(handle); }));
    if (!stream->stream) {
        fail(MMSYSERR_NODRIVER);
        return;
    }
    {
        std::lock_guard<std::mutex> guard(m_wave_mutex);
        m_wavein_streams.emplace(handle, stream);
    }
    (void)m.write_ptr(phwi, handle);
    // WIM_OPEN before waveInOpen returns, on this thread (measured).
    notify_sync(stream->target(), wic::WIM_OPEN);
    m.set_return(MMSYSERR_NOERROR);
}

void WinmmManager::impl_waveInClose(Marshaler& m) {
    const auto stream = find_wavein(m.arg(0));
    if (!stream) {
        m.set_return(MMSYSERR_INVALHANDLE);
        return;
    }
    u64 sequence = 0;
    std::shared_ptr<audio::IAudioCaptureStream> host;
    {
        std::lock_guard<std::mutex> guard(m_wave_mutex);
        // Learn: every buffer must have been returned (measured
        // WAVERR_STILLPLAYING with one queued).
        if (!stream->queue.empty()) {
            m.set_return(WAVERR_STILLPLAYING);
            return;
        }
        m_wavein_streams.erase(stream->handle);
        stream->started = false;
        host = std::move(stream->stream);
        sequence = m_wave_event_sequence;
    }
    if (host) host->stop();
    host.reset();
    // Every WIM_DATA precedes WIM_CLOSE, which arrives before waveInClose
    // returns (measured).
    wait_wave_events(sequence);
    notify_sync(stream->target(), wic::WIM_CLOSE);
    m.set_return(MMSYSERR_NOERROR);
}

void WinmmManager::impl_waveInPrepareHeader(Marshaler& m) {
    const u64 handle = m.arg(0);
    const GuestAddr pwh = m.arg_ptr(1);
    const u32 cbwh = m.arg32(2);
    const WaveHdrLayout& layout = wavehdr_layout(m.cpu_is_64bit());
    if (pwh == 0 || cbwh < layout.size) {
        m.set_return(MMSYSERR_INVALPARAM);
        return;
    }
    if (!find_wavein(handle)) {
        m.set_return(MMSYSERR_INVALHANDLE);
        return;
    }
    // Measured: a header handed in with WHDR_DONE comes back WHDR_PREPARED
    // only; preparing twice succeeds.
    const u32 flags = m.read_u32(pwh + layout.flags_offset).value_or(0);
    (void)m.write_u32(pwh + layout.flags_offset, (flags & ~WHDR_DONE) | WHDR_PREPARED);
    m.set_return(MMSYSERR_NOERROR);
}

void WinmmManager::impl_waveInUnprepareHeader(Marshaler& m) {
    const u64 handle = m.arg(0);
    const GuestAddr pwh = m.arg_ptr(1);
    const u32 cbwh = m.arg32(2);
    const WaveHdrLayout& layout = wavehdr_layout(m.cpu_is_64bit());
    if (pwh == 0 || cbwh < layout.size) {
        m.set_return(MMSYSERR_INVALPARAM);
        return;
    }
    const u32 flags = m.read_u32(pwh + layout.flags_offset).value_or(0);
    // Measured: the queue state is read from dwFlags, before the handle is
    // checked (WAVERR_STILLPLAYING even for a handle that names no stream).
    if ((flags & WHDR_INQUEUE) != 0) {
        m.set_return(WAVERR_STILLPLAYING);
        return;
    }
    if (!find_wavein(handle)) {
        m.set_return(MMSYSERR_INVALHANDLE);
        return;
    }
    // WHDR_DONE stays; an unprepared header is a no-op success (Learn).
    (void)m.write_u32(pwh + layout.flags_offset, flags & ~WHDR_PREPARED);
    m.set_return(MMSYSERR_NOERROR);
}

void WinmmManager::impl_waveInAddBuffer(Marshaler& m) {
    const u64 handle = m.arg(0);
    const GuestAddr pwh = m.arg_ptr(1);
    const u32 cbwh = m.arg32(2);
    const WaveHdrLayout& layout = wavehdr_layout(m.cpu_is_64bit());
    if (pwh == 0 || cbwh < layout.size) {
        m.set_return(MMSYSERR_INVALPARAM);
        return;
    }
    const u32 flags = m.read_u32(pwh + layout.flags_offset).value_or(0);
    // Header checks before the handle, as for waveOutWrite.
    if ((flags & WHDR_PREPARED) == 0) {
        m.set_return(WAVERR_UNPREPARED);
        return;
    }
    const auto stream = find_wavein(handle);
    if (!stream) {
        m.set_return(MMSYSERR_INVALHANDLE);
        return;
    }
    if ((flags & WHDR_INQUEUE) != 0) {
        m.set_return(WAVERR_STILLPLAYING);
        return;
    }
    std::lock_guard<std::mutex> guard(m_wave_mutex);
    // Measured: dwBytesRecorded restarts at 0 and the flags become
    // WHDR_PREPARED | WHDR_INQUEUE.
    (void)m.write_u32(pwh + layout.bytes_recorded_offset, 0);
    (void)m.write_u32(pwh + layout.flags_offset, (flags & ~WHDR_DONE) | WHDR_INQUEUE);
    if (stream->queue.empty()) stream->head_recorded = 0;
    stream->queue.push_back(pwh);
    m.set_return(MMSYSERR_NOERROR);
}

void WinmmManager::impl_waveInStart(Marshaler& m) {
    const auto stream = find_wavein(m.arg(0));
    if (!stream) {
        m.set_return(MMSYSERR_INVALHANDLE);
        return;
    }
    std::shared_ptr<audio::IAudioCaptureStream> host;
    {
        std::lock_guard<std::mutex> guard(m_wave_mutex);
        // Learn: starting a started device has no effect.
        if (stream->started) {
            m.set_return(MMSYSERR_NOERROR);
            return;
        }
        stream->started = true;
        host = stream->stream;
    }
    if (host && host->start()) {
        m.set_return(MMSYSERR_NOERROR);
        return;
    }
    std::lock_guard<std::mutex> guard(m_wave_mutex);
    stream->started = false;
    m.set_return(MMSYSERR_NODRIVER);
}

void WinmmManager::impl_waveInStop(Marshaler& m) {
    const auto stream = find_wavein(m.arg(0));
    if (!stream) {
        m.set_return(MMSYSERR_INVALHANDLE);
        return;
    }
    std::shared_ptr<audio::IAudioCaptureStream> host;
    {
        std::lock_guard<std::mutex> guard(m_wave_mutex);
        if (!stream->started) {
            m.set_return(MMSYSERR_NOERROR);
            return;
        }
        stream->started = false;
        host = stream->stream;
    }
    if (host) {
        host->stop();
        host->ring().clear(false);
    }
    std::lock_guard<std::mutex> guard(m_wave_mutex);
    // Learn: the current buffer is returned with what it holds; empty buffers
    // stay queued (measured: the partial one returned, the next still
    // WHDR_INQUEUE).
    if (!stream->queue.empty() && stream->head_recorded != 0) {
        complete_wavein_head_locked(*stream);
    }
    m.set_return(MMSYSERR_NOERROR);
}

void WinmmManager::impl_waveInReset(Marshaler& m) {
    const auto stream = find_wavein(m.arg(0));
    if (!stream) {
        m.set_return(MMSYSERR_INVALHANDLE);
        return;
    }
    std::shared_ptr<audio::IAudioCaptureStream> host;
    {
        std::lock_guard<std::mutex> guard(m_wave_mutex);
        stream->started = false;
        host = stream->stream;
    }
    if (host) {
        host->stop();
        host->ring().clear(true);
    }
    std::lock_guard<std::mutex> guard(m_wave_mutex);
    // Learn: every pending buffer is marked done and returned, and the
    // position goes back to zero.
    while (!stream->queue.empty()) complete_wavein_head_locked(*stream);
    stream->position_bytes = 0;
    m.set_return(MMSYSERR_NOERROR);
}

// waveInGetPosition(HWAVEIN, LPMMTIME, UINT cbmmt). Measured: NULL is
// MMSYSERR_INVALPARAM, cbmmt is not checked, TIME_SAMPLES and TIME_BYTES are
// answered as asked and every other wType is changed to TIME_BYTES.
void WinmmManager::impl_waveInGetPosition(Marshaler& m) {
    const GuestAddr pmmt = m.arg_ptr(1);
    if (pmmt == 0) {
        m.set_return(MMSYSERR_INVALPARAM);
        return;
    }
    const auto stream = find_wavein(m.arg(0));
    if (!stream) {
        m.set_return(MMSYSERR_INVALHANDLE);
        return;
    }
    u64 bytes = 0;
    {
        std::lock_guard<std::mutex> guard(m_wave_mutex);
        bytes = stream->position_bytes;
    }
    const u32 type = m.read_u32(pmmt).value_or(0);
    if (type == TIME_SAMPLES) {
        (void)m.write_u32(pmmt + 4, static_cast<u32>(bytes / std::max<u16>(stream->block_align, 1)));
    } else {
        (void)m.write_u32(pmmt, TIME_BYTES);
        (void)m.write_u32(pmmt + 4, static_cast<u32>(bytes));
    }
    (void)TIME_MS_;
    m.set_return(MMSYSERR_NOERROR);
}

void WinmmManager::impl_waveInGetID(Marshaler& m) {
    const GuestAddr out = m.arg_ptr(1);
    if (out == 0) {
        m.set_return(MMSYSERR_INVALPARAM);
        return;
    }
    const auto stream = find_wavein(m.arg(0));
    if (!stream) {
        m.set_return(MMSYSERR_INVALHANDLE);
        return;
    }
    (void)m.write_u32(out, stream->device_id);
    m.set_return(MMSYSERR_NOERROR);
}

// waveInMessage(HWAVEIN hwi, UINT uMsg, DWORD_PTR dw1, DWORD_PTR dw2): hwi is
// a device id (Learn). The wave mapper answers the preferred-device queries;
// a device answers the queries the system intercepts.
void WinmmManager::impl_waveInMessage(Marshaler& m) {
    const u64 target = m.arg(0);
    const u32 msg = m.arg32(1);
    const GuestAddr dw1 = m.arg_ptr(2);
    const GuestAddr dw2 = m.arg_ptr(3);
    std::vector<audio::CaptureDevice> devices;
    if (m_capture) devices = audio::capture_devices_in_id_order(*m_capture);
    // DRV_QUERYDEVICEINTERFACE(SIZE) of an id with no device interface: an
    // empty name, 2 bytes with its NUL (measured on WAVE_MAPPER: size 2; a
    // 4-byte buffer gets one NUL; a 1-byte buffer MMSYSERR_MOREDATA,
    // untouched; NULL MMSYSERR_INVALPARAM. Learn: "If the device has no
    // device interface, the string length is zero").
    const auto no_device_interface = [&] {
        if (dw1 == 0) {
            m.set_return(MMSYSERR_INVALPARAM);
            return;
        }
        if (msg == wic::DRV_QUERYDEVICEINTERFACESIZE) {
            (void)m.write_u32(dw1, 2u);
            m.set_return(MMSYSERR_NOERROR);
            return;
        }
        if (static_cast<u32>(dw2) < 2u) {
            m.set_return(wic::MMSYSERR_MOREDATA);
            return;
        }
        (void)m.write_u16(dw1, 0);
        m.set_return(MMSYSERR_NOERROR);
    };
    if (is_mapper_id(target)) {
        if (msg == wic::DRV_QUERYDEVICEINTERFACESIZE || msg == wic::DRV_QUERYDEVICEINTERFACE) {
            no_device_interface();
            return;
        }
        if (msg != DRVM_MAPPER_PREFERRED_GET && msg != DRVM_MAPPER_CONSOLEVOICECOM_GET) {
            m.set_return(MMSYSERR_NOTSUPPORTED);
            return;
        }
        if (dw1 == 0 || dw2 == 0) {
            m.set_return(MMSYSERR_INVALPARAM);
            return;
        }
        // Measured: the default (and default communications) device is id 0,
        // flags 0; -1 when there is no device.
        (void)m.write_u32(dw1, devices.empty() ? 0xFFFFFFFFu : 0u);
        (void)m.write_u32(dw2, 0);
        m.set_return(MMSYSERR_NOERROR);
        return;
    }
    if (target >= devices.size()) {
        m.set_return(MMSYSERR_BADDEVICEID);
        return;
    }
    const audio::CaptureDevice& device = devices[static_cast<usize>(target)];
    switch (msg) {
    case wic::DRV_QUERYFUNCTIONINSTANCEIDSIZE:
    case wic::DRV_QUERYFUNCTIONINSTANCEID: {
        // The MMDevice endpoint id (measured: device 0's is the default
        // capture endpoint's id), with its terminating NUL.
        const std::string id = audio::capture_endpoint_id(device.uid);
        const u32 size = static_cast<u32>((id.size() + 1) * 2);
        if (dw1 == 0) {
            m.set_return(MMSYSERR_INVALPARAM);
            return;
        }
        if (msg == wic::DRV_QUERYFUNCTIONINSTANCEIDSIZE) {
            (void)m.write_u32(dw1, size);
            m.set_return(MMSYSERR_NOERROR);
            return;
        }
        if (static_cast<u32>(dw2) < size) {
            m.set_return(wic::MMSYSERR_MOREDATA);
            return;
        }
        for (usize i = 0; i <= id.size(); ++i) {
            (void)m.write_u16(dw1 + i * 2, i < id.size() ? static_cast<u8>(id[i]) : 0);
        }
        m.set_return(MMSYSERR_NOERROR);
        return;
    }
    case wic::DRV_QUERYDEVICEINTERFACESIZE:
    case wic::DRV_QUERYDEVICEINTERFACE:
        // Windows answers the kernel-streaming filter's PnP interface path
        // (measured: "\\?\...#{6994ad04-...}\<ref>", KSCATEGORY_AUDIO).
        // Vitreo's inputs have no PnP device node or KS filter a path could
        // name, so the device reports no device interface, the answer
        // Windows gives for an id without one (WAVE_MAPPER).
        WINMM_UNIMPLEMENTED("waveInMessage: DRV_QUERYDEVICEINTERFACE (no kernel-streaming "
                            "device interface; reported as none)");
        no_device_interface();
        return;
    case wic::DRV_QUERYMAPPABLE:
        m.set_return(MMSYSERR_NOERROR);
        return;
    default:
        // DRV_QUERYDEVNODE included (Learn: not supported since Windows 2000);
        // measured MMSYSERR_NOTSUPPORTED for an unknown message.
        m.set_return(MMSYSERR_NOTSUPPORTED);
        return;
    }
}

} // namespace vitreo::winapi
