/// @file winmm_waveout.cpp
/// @brief waveOut* device APIs.
///
/// Caps, open/close, header prepare/write, reset/pause/restart, position,
/// message and volume. Split out of winmm.cpp.

#include "winapi/media/winmm/winmm_internal.hpp"
#include "winapi/kernel32/error.hpp"
#include "windowing/contracts/wave_output.hpp"
#include <algorithm>
#include <cstring>
#include <mutex>

namespace vitreo::winapi {

namespace {

// ---------------------------------------------------------------------------
// Capability structures
// ---------------------------------------------------------------------------
/// WAVEOUTCAPS2A/W. The prefix is WAVEOUTCAPSA (52 bytes) / WAVEOUTCAPSW
/// (84). Every modern Windows endpoint reports the WDM audio identity
/// (s7_winmm_probe info caps_a: mid=1 pid=0x64 ver=0xa00, mapper mid=1
/// pid=2 ver=0x500) and the manufacturer/product GUIDs derived from it
/// (info caps2_*). dwSupport lists only what Vitreo implements: one gain
/// (WAVECAPS_VOLUME), no separate left/right channel, pitch, playback rate,
/// or sample-accurate position.
[[nodiscard]] std::vector<u8> build_waveout_caps(bool mapper, bool wide) {
    const usize name_bytes = wide ? 64 : 32;
    const usize prefix = 8 + name_bytes + 12;
    std::vector<u8> b(prefix + 48, 0);
    put16(b, 0, MM_MICROSOFT);
    put16(b, 2, mapper ? MM_WAVE_MAPPER : MM_MSFT_WDMAUDIO_WAVEOUT);
    put32(b, 4, mapper ? 0x0500 : 0x0A00);
    put_name(b, 8, mapper ? kWaveMapperName : kWaveDeviceName, wide);
    put32(b, 8 + name_bytes, 0x000FFFFF); // WAVE_FORMAT_1M08 .. 96S16
    put16(b, 12 + name_bytes, 2);
    put32(b, 16 + name_bytes, WAVECAPS_VOLUME);
    if (!mapper) {
        put_mmreg_guid(b, prefix, 0xd5a47fa7, 0x6d98, MM_MICROSOFT);
        put_mmreg_guid(b, prefix + 16, 0xe36dc2ac, 0x6d9a,
                       MM_MSFT_WDMAUDIO_WAVEOUT);
    }
    return b;
}


} // namespace

// ---------------------------------------------------------------------------
// waveOut
// ---------------------------------------------------------------------------
void WinmmManager::impl_waveOutGetNumDevs(Marshaler& m) {
    const u32 count = wave_device_count();
    winmm_diag("get-num-devs -> %u\n", count);
    m.set_return(count);
}

// waveOutGetDevCapsA/W(UINT_PTR uDeviceID, LPWAVEOUTCAPS pwoc, UINT cbwoc).
// uDeviceID is a device id, WAVE_MAPPER, or an open HWAVEOUT. Order and
// copy sizes measured by s7_winmm_probe caps_*:
//  - cbwoc == 0 copies nothing and succeeds, even for a bad id (cb0_baddev);
//  - a NULL buffer is MMSYSERR_INVALPARAM, then the id is validated;
//  - A copies min(cbwoc, sizeof(WAVEOUTCAPS2A) = 100) bytes (caps_a_cb);
//  - W needs cbwoc >= sizeof(WAVEOUTCAPSW) = 84 (caps_w_cb cb<84 -> 11), then
//    copies min(cbwoc, 132) for a device and exactly 84 for the mapper;
//  - the mapper answers even with no devices (zero-device run).
static void wave_out_get_dev_caps(Marshaler& m, bool wide, u32 device_count,
                                  bool is_handle, u32 handle_device) {
    const u64 raw_id = m.arg(0);
    const GuestAddr pwoc = m.arg_ptr(1);
    const u32 cbwoc = m.arg32(2);
    if (cbwoc == 0) {
        m.set_return(MMSYSERR_NOERROR);
        return;
    }
    if (pwoc == 0) {
        m.set_return(MMSYSERR_INVALPARAM);
        return;
    }
    const u32 id = is_handle ? handle_device : static_cast<u32>(raw_id);
    const bool mapper = is_handle ? handle_device == WAVE_MAPPER
                                  : is_mapper_id(raw_id);
    if (!mapper && ((!is_handle && (raw_id >> 32) != 0) || id >= device_count)) {
        m.set_return(MMSYSERR_BADDEVICEID);
        return;
    }
    if (wide && cbwoc < 84) {
        m.set_return(MMSYSERR_INVALPARAM);
        return;
    }
    const std::vector<u8> caps = build_waveout_caps(mapper, wide);
    const usize limit = wide && mapper ? 84 : caps.size();
    const usize copy = std::min<usize>(cbwoc, limit);
    (void)m.memory().write_bytes(
        pwoc, reinterpret_cast<const Byte*>(caps.data()), copy);
    m.set_return(MMSYSERR_NOERROR);
}

void WinmmManager::impl_waveOutGetDevCapsA(Marshaler& m) {
    const auto stream = find_stream(m.arg(0));
    wave_out_get_dev_caps(m, false, wave_device_count(),
                          stream != nullptr, stream ? stream->device_id : 0);
}

void WinmmManager::impl_waveOutGetDevCapsW(Marshaler& m) {
    const auto stream = find_stream(m.arg(0));
    wave_out_get_dev_caps(m, true, wave_device_count(),
                          stream != nullptr, stream ? stream->device_id : 0);
}

// waveOutOpen(LPHWAVEOUT phwo, UINT uDeviceID, LPCWAVEFORMATEX pwfx,
//             DWORD_PTR dwCallback, DWORD_PTR dwInstance, DWORD fdwOpen)
void WinmmManager::impl_waveOutOpen(Marshaler& m) {
    const GuestAddr phwo = m.arg_ptr(0);
    const u32 device_id = m.arg32(1);
    const GuestAddr pwfx = m.arg_ptr(2);
    const u64 callback = m.arg(3);
    const u64 instance = m.arg(4);
    const u32 fdwOpen = m.arg32(5);
    const u32 callback_type = fdwOpen & CALLBACK_TYPEMASK;
    const bool query = (fdwOpen & WAVE_FORMAT_QUERY) != 0;

    // s7_winmm_probe o_null_fmt / o_null_phwo / o_window_bad: argument
    // checks come before the device id, and leave *phwo untouched.
    if (pwfx == 0 || (!query && phwo == 0)) {
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
    // A failed open stores NULL in *phwo (o_baddev h=0); a query never
    // writes it (q_pcm_phwo).
    const auto fail = [&](u32 code) {
        if (!query && phwo != 0) (void)m.write_ptr(phwo, 0);
        m.set_return(code);
    };
    const u32 devices = wave_device_count();
    if (devices == 0 || (device_id != WAVE_MAPPER && device_id >= devices)) {
        fail(MMSYSERR_BADDEVICEID);
        return;
    }
    const std::vector<u8> fmt = read_guest_wave_format(m_memory, pwfx);
    if (fmt.empty()) {
        fail(MMSYSERR_INVALPARAM);
        return;
    }
    const WaveFormatCheck check = check_wave_format(fmt, !query);
    winmm_diag("open fmt=%u ch=%u rate=%u align=%u bits=%u callback=0x%llx "
               "flags=0x%x -> %u\n",
               le16(fmt, 0), le16(fmt, 2), le32(fmt, 4), le16(fmt, 12),
               le16(fmt, 14), static_cast<unsigned long long>(callback),
               fdwOpen, check.result);
    if (check.result != MMSYSERR_NOERROR) {
        fail(check.result);
        return;
    }
    if (query) {
        m.set_return(MMSYSERR_NOERROR);
        return;
    }

    auto stream = std::make_shared<WaveStream>();
    stream->device_id = device_id;
    stream->is_64bit = m.cpu_is_64bit();
    stream->callback = callback;
    stream->instance = instance;
    stream->callback_type = callback_type;
    stream->conversion = check.conversion;
    stream->bytes_per_second = check.guest_bytes_per_second;
    stream->block_align = check.guest_block_align;
    if (!acquire_output(stream->lease)) {
        WINMM_UNIMPLEMENTED(
            "waveOutOpen: concurrent stream without a host output factory");
        fail(MMSYSERR_ALLOCATED);
        return;
    }
    {
        std::lock_guard<std::mutex> guard(m_wave_mutex);
        stream->handle = m_next_wave_handle++;
    }
    const u64 handle = stream->handle;
    if (!stream->lease.output->open(
            check.host, [this, handle](u64 token, u32) {
                complete_wave_block(handle, token);
            })) {
        release_output(stream->lease);
        fail(MMSYSERR_ERROR);
        return;
    }
    {
        std::lock_guard<std::mutex> guard(m_wave_mutex);
        stream->volume = m_device_volume;
        m_wave_streams.emplace(handle, stream);
    }
    (void)stream->lease.output->set_volume(
        static_cast<float>(stream->volume & 0xFFFFu) / 65535.0f);
    (void)m.write_ptr(phwo, handle);
    // WOM_OPEN before waveOutOpen returns, on this thread (fn_open_cbs,
    // th_open_msg, wn_open_msg, ev_open signaled_at_return).
    notify_sync(*stream, WOM_OPEN);
    m.set_return(MMSYSERR_NOERROR);
}

void WinmmManager::impl_waveOutClose(Marshaler& m) {
    const auto stream = find_stream(m.arg(0));
    if (!stream) {
        m.set_return(MMSYSERR_INVALHANDLE); // close_again / close_null
        return;
    }
    u64 sequence = 0;
    {
        std::lock_guard<std::mutex> guard(m_wave_mutex);
        if (!stream->pending.empty()) {
            m.set_return(WAVERR_STILLPLAYING); // close_inqueue
            return;
        }
        sequence = m_wave_event_sequence;
    }
    // Every WOM_DONE precedes WOM_CLOSE.
    wait_wave_events(sequence);
    OutputLease lease;
    {
        // Detach the host stream under the lock: a racing call on this
        // handle then sees no output instead of a closed one.
        std::lock_guard<std::mutex> guard(m_wave_mutex);
        m_wave_streams.erase(stream->handle);
        lease = std::move(stream->lease);
        stream->lease.output = nullptr;
    }
    release_output(lease);
    VITREO_TRACE("[winmm] waveOutClose hwo=0x%llx\n",
                 static_cast<unsigned long long>(stream->handle));
    // WOM_CLOSE before waveOutClose returns (close_cbs, th_close_msg).
    notify_sync(*stream, WOM_CLOSE);
    m.set_return(MMSYSERR_NOERROR);
}

void WinmmManager::impl_waveOutPrepareHeader(Marshaler& m) {
    const u64 handle = m.arg(0);
    const GuestAddr pwh = m.arg_ptr(1);
    const u32 cbwh = m.arg32(2);
    const WaveHdrLayout& layout = wavehdr_layout(m.cpu_is_64bit());
    if (pwh == 0 || cbwh < layout.size) { // prep_small / prep_null
        m.set_return(MMSYSERR_INVALPARAM);
        return;
    }
    if (!find_stream(handle)) { // prep_badh
        m.set_return(MMSYSERR_INVALHANDLE);
        return;
    }
    const u32 flags = m.read_u32(pwh + layout.flags_offset).value_or(0);
    // Preparing twice succeeds (prep_again).
    (void)m.write_u32(pwh + layout.flags_offset, flags | WHDR_PREPARED);
    winmm_diag("prepare header=0x%llx\n", static_cast<unsigned long long>(pwh));
    m.set_return(MMSYSERR_NOERROR);
}

void WinmmManager::impl_waveOutUnprepareHeader(Marshaler& m) {
    const u64 handle = m.arg(0);
    const GuestAddr pwh = m.arg_ptr(1);
    const u32 cbwh = m.arg32(2);
    const WaveHdrLayout& layout = wavehdr_layout(m.cpu_is_64bit());
    if (pwh == 0 || cbwh < layout.size) { // unprep_null / unprep_small_cb
        m.set_return(MMSYSERR_INVALPARAM);
        return;
    }
    if (!find_stream(handle)) {
        m.set_return(MMSYSERR_INVALHANDLE);
        return;
    }
    const u32 flags = m.read_u32(pwh + layout.flags_offset).value_or(0);
    if ((flags & WHDR_INQUEUE) != 0) { // unprep_inqueue
        m.set_return(WAVERR_STILLPLAYING);
        return;
    }
    // WHDR_DONE stays set (unprep f=1); an unprepared header is a no-op
    // success (unprep_unprepared).
    (void)m.write_u32(pwh + layout.flags_offset, flags & ~WHDR_PREPARED);
    m.set_return(MMSYSERR_NOERROR);
}

void WinmmManager::impl_waveOutWrite(Marshaler& m) {
    const u64 handle = m.arg(0);
    const GuestAddr pwh = m.arg_ptr(1);
    const u32 cbwh = m.arg32(2);
    const WaveHdrLayout& layout = wavehdr_layout(m.cpu_is_64bit());

    // Header checks precede the handle check: writing an unprepared header
    // to a closed handle is WAVERR_UNPREPARED (write_closed).
    if (pwh == 0 || cbwh < layout.size) { // write_null_hdr / write_small_cb
        m.set_return(MMSYSERR_INVALPARAM);
        return;
    }
    const u32 old_flags = m.read_u32(pwh + layout.flags_offset).value_or(0);
    if ((old_flags & WHDR_PREPARED) == 0) {
        m.set_return(WAVERR_UNPREPARED);
        return;
    }
    if ((old_flags & WHDR_INQUEUE) != 0) { // write_again
        m.set_return(WAVERR_STILLPLAYING);
        return;
    }
    const auto stream = find_stream(handle);
    if (!stream) {
        m.set_return(MMSYSERR_INVALHANDLE);
        return;
    }

    const GuestAddr data = m.read_ptr(pwh + layout.data_offset).value_or(0);
    const u32 length = m.read_u32(pwh + layout.buffer_length_offset).value_or(0);
    if ((old_flags & WHDR_BEGINLOOP) != 0 &&
        m.read_u32(pwh + layout.loops_offset).value_or(1) > 1) {
        WINMM_UNIMPLEMENTED("waveOutWrite: WHDR_BEGINLOOP/dwLoops looping "
                            "(the block plays once)");
    }
    std::vector<Byte> pcm;
    if (length != 0) {
        pcm.resize(length);
        if (data == 0 ||
            m_memory.read_bytes(data, pcm.data(), length).is_err()) {
            VITREO_TRACE("[winmm] waveOutWrite: failed to read %u bytes from "
                         "0x%llx\n", length,
                         static_cast<unsigned long long>(data));
            m.set_return(MMSYSERR_ERROR);
            return;
        }
        pcm = convert_samples(pcm, stream->conversion);
    }

    // Queued: WHDR_DONE cleared, WHDR_INQUEUE set (write f=0x12). Set before
    // the block can complete, so the completion's DONE is never overwritten.
    (void)m.write_u32(pwh + layout.flags_offset,
                      (old_flags & ~WHDR_DONE) | WHDR_INQUEUE);
    {
        std::lock_guard<std::mutex> guard(m_wave_mutex);
        if (length == 0) {
            // An empty buffer is accepted and returned in order
            // (write_empty_hdr r=0 done=1).
            if (stream->pending.empty()) {
                (void)enqueue_wave_event_locked(*stream, WOM_DONE, pwh, true);
            } else {
                stream->pending.push_back({pwh, 0, 0});
            }
            m.set_return(MMSYSERR_NOERROR);
            return;
        }
        const u64 token = stream->next_token++;
        stream->pending.push_back({pwh, length, token});
        if (stream->lease.output == nullptr ||
            !stream->lease.output->submit(token, pcm)) {
            stream->pending.pop_back();
            (void)m.write_u32(pwh + layout.flags_offset, old_flags);
            m.set_return(MMSYSERR_ERROR);
            return;
        }
        if (winmm_diag_enabled()) {
            const auto now = std::chrono::steady_clock::now();
            if (stream->diag_first_write.time_since_epoch().count() == 0) {
                stream->diag_first_write = now;
            }
            stream->diag_bytes_written += length;
            const double elapsed = std::chrono::duration<double>(
                now - stream->diag_first_write).count();
            const double supplied = stream->bytes_per_second
                ? static_cast<double>(stream->diag_bytes_written) /
                      stream->bytes_per_second
                : 0.0;
            winmm_diag("write hwo=0x%llx header=0x%llx bytes=%u queued=%zu "
                       "supplied=%.2fs elapsed=%.2fs\n",
                       static_cast<unsigned long long>(handle),
                       static_cast<unsigned long long>(pwh), length,
                       stream->pending.size(), supplied, elapsed);
        }
    }
    VITREO_TRACE("[winmm] waveOutWrite header=0x%llx bytes=%u\n",
                 static_cast<unsigned long long>(pwh), length);
    m.set_return(MMSYSERR_NOERROR);
}

// waveOutReset: every pending buffer is marked done and returned, WOM_DONE
// callbacks included, before the call returns (reset cb_at_return=3,
// f=3 f=3 f=3); the position restarts at 0 (pos_after_reset) and a paused
// stream plays again (after_reset_write done=1).
void WinmmManager::impl_waveOutReset(Marshaler& m) {
    const auto stream = find_stream(m.arg(0));
    if (!stream) {
        m.set_return(MMSYSERR_INVALHANDLE);
        return;
    }
    u64 sequence = 0;
    usize returned = 0;
    {
        std::lock_guard<std::mutex> guard(m_wave_mutex);
        if (stream->lease.output != nullptr) {
            stream->lease.output->reset();
            stream->lease.output->set_paused(false);
        }
        returned = stream->pending.size();
        for (const auto& p : stream->pending) {
            (void)enqueue_wave_event_locked(*stream, WOM_DONE, p.header, true);
        }
        stream->pending.clear();
        stream->bytes_completed = 0;
        sequence = m_wave_event_sequence;
    }
    wait_wave_events(sequence);
    winmm_diag("reset hwo=0x%llx returned=%zu\n",
               static_cast<unsigned long long>(stream->handle), returned);
    m.set_return(MMSYSERR_NOERROR);
}

void WinmmManager::impl_waveOutPause(Marshaler& m) {
    const auto stream = find_stream(m.arg(0));
    if (!stream) {
        m.set_return(MMSYSERR_INVALHANDLE);
        return;
    }
    {
        std::lock_guard<std::mutex> guard(m_wave_mutex);
        if (stream->lease.output != nullptr) {
            stream->lease.output->set_paused(true); // pause_twice: both succeed
        }
    }
    m.set_return(MMSYSERR_NOERROR);
}

void WinmmManager::impl_waveOutRestart(Marshaler& m) {
    const auto stream = find_stream(m.arg(0));
    if (!stream) {
        m.set_return(MMSYSERR_INVALHANDLE);
        return;
    }
    {
        std::lock_guard<std::mutex> guard(m_wave_mutex);
        if (stream->lease.output != nullptr) stream->lease.output->set_paused(false);
    }
    m.set_return(MMSYSERR_NOERROR);
}

// waveOutGetPosition(hwo, pmmt, cbmmt): TIME_SAMPLES and TIME_BYTES are
// answered as asked; every other format comes back as TIME_BYTES, the
// driver's choice Learn allows (pos_ms / pos_smpte / pos_type_junk
// type=4). A NULL MMTIME is MMSYSERR_INVALPARAM, then the handle is
// checked, then a short MMTIME is MMSYSERR_ERROR (pos_cb4 / pos_cb8 /
// pos_cb11 r=1, pos_cb16 r=0).
void WinmmManager::impl_waveOutGetPosition(Marshaler& m) {
    const u64 handle = m.arg(0);
    const GuestAddr pmmt = m.arg_ptr(1);
    const u32 cbmmt = m.arg32(2);
    if (pmmt == 0) {
        m.set_return(MMSYSERR_INVALPARAM);
        return;
    }
    // The handle is checked before the size (x_wi_getpos_cb4 r=5).
    const auto stream = find_stream(handle);
    if (!stream) {
        m.set_return(MMSYSERR_INVALHANDLE);
        return;
    }
    if (cbmmt < MMTIME_SIZE) {
        m.set_return(MMSYSERR_ERROR);
        return;
    }
    u64 bytes = 0;
    u16 block_align = 0;
    {
        std::lock_guard<std::mutex> guard(m_wave_mutex);
        bytes = stream->bytes_completed;
        block_align = stream->block_align;
    }
    const u32 requested = m.read_u32(pmmt).value_or(TIME_BYTES);
    const u32 type = requested == TIME_SAMPLES ? TIME_SAMPLES : TIME_BYTES;
    const u64 position = type == TIME_SAMPLES && block_align != 0
        ? bytes / block_align : bytes;
    (void)m.write_u32(pmmt, type);
    (void)m.write_u32(pmmt + 4, static_cast<u32>(position));
    m.set_return(MMSYSERR_NOERROR);
}

// waveOutGetID: the id the handle was opened with, WAVE_MAPPER included
// (getid id=ffffffff for a mapper handle, getid_dev0 id=0).
void WinmmManager::impl_waveOutGetID(Marshaler& m) {
    const u64 handle = m.arg(0);
    const GuestAddr id_ptr = m.arg_ptr(1);
    if (id_ptr == 0) { // getid_null
        m.set_return(MMSYSERR_INVALPARAM);
        return;
    }
    const auto stream = find_stream(handle);
    if (!stream) { // getid_bad / getid_zero / getid_closed: id untouched
        m.set_return(MMSYSERR_INVALHANDLE);
        return;
    }
    (void)m.write_u32(id_ptr, stream->device_id);
    m.set_return(MMSYSERR_NOERROR);
}

// waveOutMessage(hwo, uMsg, dw1, dw2). hwo is WAVE_MAPPER, a device id, or
// an open handle (s7_winmm_probe msg_*):
//  - WAVE_MAPPER answers DRVM_MAPPER_PREFERRED_GET and
//    DRVM_MAPPER_CONSOLEVOICECOM_GET: DWORD device id (0, or -1 with no
//    device) into *dw1, DWORD status flags 0 into *dw2; either pointer NULL
//    is MMSYSERR_INVALPARAM; any other message MMSYSERR_NOTSUPPORTED;
//  - a device id rejects the mapper messages with MMSYSERR_INVALPARAM and
//    has no other driver messages; an out-of-range id is BADDEVICEID;
//  - an open handle supports no messages.
void WinmmManager::impl_waveOutMessage(Marshaler& m) {
    const u64 target = m.arg(0);
    const u32 message = m.arg32(1);
    const GuestAddr dw1 = m.arg_ptr(2);
    const GuestAddr dw2 = m.arg_ptr(3);
    const bool mapper_message = message == DRVM_MAPPER_PREFERRED_GET ||
                                message == DRVM_MAPPER_CONSOLEVOICECOM_GET;
    const u32 devices = wave_device_count();
    if (is_mapper_id(target)) {
        if (!mapper_message) {
            if (message == 0x080C || message == 0x080D) {
                // DRV_QUERYDEVICEINTERFACE(SIZE): Vitreo's output has no
                // PnP device interface path.
                WINMM_UNIMPLEMENTED(
                    "waveOutMessage: DRV_QUERYDEVICEINTERFACE on WAVE_MAPPER");
            }
            m.set_return(MMSYSERR_NOTSUPPORTED);
            return;
        }
        if (dw1 == 0 || dw2 == 0) {
            m.set_return(MMSYSERR_INVALPARAM);
            return;
        }
        (void)m.write_u32(dw1, devices != 0 ? 0u : 0xFFFFFFFFu);
        (void)m.write_u32(dw2, 0);
        m.set_return(MMSYSERR_NOERROR);
        return;
    }
    if (find_stream(target)) {
        m.set_return(MMSYSERR_NOTSUPPORTED);
        return;
    }
    if (target < devices) {
        m.set_return(mapper_message ? MMSYSERR_INVALPARAM
                                    : MMSYSERR_NOTSUPPORTED);
        return;
    }
    m.set_return(MMSYSERR_BADDEVICEID);
}

// waveOutSetVolume/GetVolume(hwo, ...): hwo is a handle or a device id.
// Vitreo's caps do not include WAVECAPS_LRVOLUME, so (Learn) the low word
// is the mono level and the high word is ignored; the backend has one gain.
void WinmmManager::impl_waveOutSetVolume(Marshaler& m) {
    const u64 target = m.arg(0);
    const u32 volume = m.arg32(1) & 0xFFFFu;
    const float gain = static_cast<float>(volume) / 65535.0f;
    if (const auto stream = find_stream(target)) {
        std::lock_guard<std::mutex> guard(m_wave_mutex);
        stream->volume = volume;
        if (stream->lease.output != nullptr) {
            (void)stream->lease.output->set_volume(gain);
        }
        m.set_return(MMSYSERR_NOERROR);
        return;
    }
    if (!is_mapper_id(target) && target >= wave_device_count()) {
        m.set_return(MMSYSERR_BADDEVICEID);
        return;
    }
    // A device volume applies to every stream of this process.
    {
        std::lock_guard<std::mutex> guard(m_wave_mutex);
        m_device_volume = volume;
        for (auto& [handle, stream] : m_wave_streams) {
            (void)handle;
            stream->volume = volume;
            if (stream->lease.output != nullptr) {
                (void)stream->lease.output->set_volume(gain);
            }
        }
    }
    m.set_return(MMSYSERR_NOERROR);
}

void WinmmManager::impl_waveOutGetVolume(Marshaler& m) {
    const u64 target = m.arg(0);
    const GuestAddr pdw = m.arg_ptr(1);
    if (pdw == 0) {
        m.set_return(MMSYSERR_INVALPARAM);
        return;
    }
    u32 volume = 0;
    if (const auto stream = find_stream(target)) {
        std::lock_guard<std::mutex> guard(m_wave_mutex);
        volume = stream->volume;
    } else if (is_mapper_id(target) || target < wave_device_count()) {
        std::lock_guard<std::mutex> guard(m_wave_mutex);
        volume = m_device_volume;
    } else {
        m.set_return(MMSYSERR_BADDEVICEID);
        return;
    }
    (void)m.write_u32(pdw, volume);
    m.set_return(MMSYSERR_NOERROR);
}

void WinmmManager::impl_waveOutBreakLoop(Marshaler& m) {
    // No loop is ever active (see waveOutWrite), so breaking one is a no-op.
    m.set_return(find_stream(m.arg(0)) ? MMSYSERR_NOERROR
                                       : MMSYSERR_INVALHANDLE);
}

// waveOutGet/SetPitch, waveOutGet/SetPlaybackRate: Vitreo's caps do not
// advertise WAVECAPS_PITCH or WAVECAPS_PLAYBACKRATE, so a valid handle gets
// MMSYSERR_NOTSUPPORTED (Learn).
void WinmmManager::impl_waveOutUnsupportedControl(Marshaler& m) {
    m.set_return(find_stream(m.arg(0)) ? MMSYSERR_NOTSUPPORTED
                                       : MMSYSERR_INVALHANDLE);
}

} // namespace vitreo::winapi
