// SPDX-License-Identifier: LGPL-2.1-only
/// @file dshow_audio_renderer.cpp
/// @brief DirectSound Renderer (CLSID_DSoundRender): a renderer filter with
/// one PCM / IEEE-float input pin playing through the host push voice.
///
/// Contract: Learn "DirectSound Renderer Filter", CBaseRenderer state rules
/// (a paused renderer holds its first sample and reports
/// VFW_S_STATE_INTERMEDIATE until it has one; EC_COMPLETE once the data
/// after EndOfStream has played while running; Receive after EndOfStream is
/// VFW_E_SAMPLE_REJECTED_EOS), and the measured graph behaviour in
/// tools/oracle_probe/dshow_renderfile_probe64.*.expected (IAMFilterMiscFlags
/// IS_RENDERER is what makes the graph count its EC_COMPLETE). The filter
/// itself could not be measured: winoracle has no audio render endpoint.

#include "winapi/com/ole32/dshow_objects.hpp"

#include "audio/contracts/audio_renderer.hpp"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>

namespace vitreo::winapi::dshow {

namespace {

constexpr u32 VFW_E_SAMPLE_REJECTED_EOS = 0x8004022Cu;
constexpr u32 VFW_E_NO_AUDIO_HARDWARE = 0x80040256u;
constexpr u32 EC_COMPLETE = 0x01;
constexpr u16 WAVE_FORMAT_PCM = 1;
constexpr u16 WAVE_FORMAT_IEEE_FLOAT = 3;
constexpr u16 WAVE_FORMAT_EXTENSIBLE = 0xFFFE;

/// WAVEFORMATEX(TENSIBLE) in a media type's format block, as a PcmFormat.
bool pcm_format(const MediaType& type, audio::PcmFormat& out) {
    if (!guid_eq(type.major, sdk::MEDIATYPE_Audio) ||
        !guid_eq(type.format, sdk::FORMAT_WaveFormatEx) || type.format_bytes.size() < 16) {
        return false;
    }
    const u8* w = type.format_bytes.data();
    const auto u16_at = [&](usize o) { return static_cast<u16>(w[o] | (w[o + 1] << 8)); };
    const auto u32_at = [&](usize o) {
        return static_cast<u32>(w[o] | (w[o + 1] << 8) | (w[o + 2] << 16) |
                                (static_cast<u32>(w[o + 3]) << 24));
    };
    const u16 tag = u16_at(0);
    out.channels = u16_at(2);
    out.sample_rate = u32_at(4);
    out.bits_per_sample = u16_at(14);
    out.channel_mask = 0;
    if (tag == WAVE_FORMAT_PCM) {
        out.float_samples = false;
    } else if (tag == WAVE_FORMAT_IEEE_FLOAT) {
        out.float_samples = true;
    } else if (tag == WAVE_FORMAT_EXTENSIBLE && type.format_bytes.size() >= 40) {
        Guid sub{};
        std::memcpy(&sub, w + 24, sizeof(sub));
        out.channel_mask = u32_at(20);
        if (guid_eq(sub, sdk::KSDATAFORMAT_SUBTYPE_PCM)) {
            out.float_samples = false;
        } else if (guid_eq(sub, sdk::KSDATAFORMAT_SUBTYPE_IEEE_FLOAT)) {
            out.float_samples = true;
        } else {
            return false;
        }
    } else {
        return false;
    }
    return out.valid_stream();
}

class AudioRenderer;

class AudioInputPin final : public PinBase {
public:
    AudioRenderer* renderer{nullptr};
    GuestAddr allocator{0};

    bool interface_for(const Guid& iid, Iface& out) const override {
        if (guid_eq(iid, sdk::IID_IMemInputPin)) {
            out = Iface::MemInputPin;
            return true;
        }
        return PinBase::interface_for(iid, out);
    }

protected:
    bool accept(Marshaler& m, const MediaType& type) override {
        (void)m;
        audio::PcmFormat format;
        return pcm_format(type, format);
    }
    u32 complete_connect(Marshaler& m) override;
    void on_disconnect(Marshaler& m) override;
    u32 on_end_of_stream(Marshaler& m) override;
    u32 on_begin_flush(Marshaler& m) override;
    u32 on_end_flush(Marshaler& m) override;
    u32 on_new_segment(Marshaler& m, i64 start, i64 stop, double rate) override {
        (void)m; (void)start; (void)stop; (void)rate;
        return hr::S_OK;
    }
    u32 call_other(Marshaler& m, Iface iface, u32 slot, std::span<const u64> args) override;
};

class AudioRenderer final : public FilterBase {
public:
    std::shared_ptr<AudioInputPin> input;
    std::mutex audio_mutex;
    std::condition_variable audio_cv;
    std::unique_ptr<audio::IAudioPushVoice> voice;
    audio::PcmFormat format;
    bool have_sample{false};
    bool eos{false};
    bool complete_sent{false};
    bool flushing{false};
    i32 volume{0};
    i32 balance{0};
    std::thread engine;
    bool engine_stop{false};
    u64 samples{0};
    u64 bytes{0};

    ~AudioRenderer() override { stop_engine(); }

    bool interface_for(const Guid& iid, Iface& out) const override {
        if (guid_eq(iid, sdk::IID_IAMFilterMiscFlags)) {
            out = Iface::MiscFlags;
            return true;
        }
        if (guid_eq(iid, sdk::IID_IBasicAudio) || guid_eq(iid, sdk::IID_IDispatch)) {
            out = Iface::BasicAudio;
            return true;
        }
        if (guid_eq(iid, sdk::IID_IMediaSeeking)) {
            out = Iface::MediaSeeking;
            return true;
        }
        if (guid_eq(iid, sdk::IID_IReferenceClock)) {
            static std::atomic<bool> logged{false};
            if (!logged.exchange(true)) {
                std::fprintf(stderr,
                             "[vitreo] UNIMPLEMENTED quartz!DirectSound Renderer IReferenceClock: "
                             "the graph runs on the system clock\n");
            }
            return false;
        }
        return FilterBase::interface_for(iid, out);
    }

    void apply_gains() {
        if (voice == nullptr) return;
        // IBasicAudio: volume in hundredths of a dB (0 .. -10000), balance
        // attenuates the other side (-10000 .. 10000).
        const float master = volume <= -10000 ? 0.0f : std::pow(10.0f, volume / 2000.0f);
        const float left = balance > 0 ? std::pow(10.0f, -balance / 2000.0f) : 1.0f;
        const float right = balance < 0 ? std::pow(10.0f, balance / 2000.0f) : 1.0f;
        std::vector<float> gains(format.channels, master);
        if (format.channels >= 2) {
            gains[0] = master * left;
            gains[1] = master * right;
        }
        voice->set_channel_gains(gains.data(), static_cast<u32>(gains.size()));
    }

    void start_engine() {
        if (engine.joinable()) return;
        engine_stop = false;
        engine = std::thread([this] { engine_loop(); });
    }
    void stop_engine() {
        {
            std::lock_guard<std::mutex> lock(audio_mutex);
            engine_stop = true;
        }
        audio_cv.notify_all();
        if (engine.joinable() && engine.get_id() != std::this_thread::get_id()) engine.join();
    }

    void engine_loop() {
        auto last = std::chrono::steady_clock::now();
        std::unique_lock<std::mutex> lock(audio_mutex);
        while (!engine_stop) {
            audio_cv.wait_for(lock, std::chrono::milliseconds(10));
            const auto now = std::chrono::steady_clock::now();
            const u64 elapsed = static_cast<u64>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(now - last).count());
            last = now;
            if (voice != nullptr) voice->engine_pass(elapsed);
            if (eos && !complete_sent && running_locked() &&
                (voice == nullptr || voice->padding_frames() == 0)) {
                complete_sent = true;
                std::fprintf(stderr,
                             "[quartz] DirectSound Renderer: audio after EndOfStream played "
                             "(samples=%llu bytes=%llu); EC_COMPLETE\n",
                             static_cast<unsigned long long>(samples),
                             static_cast<unsigned long long>(bytes));
                const GuestAddr g = graph_iface();
                lock.unlock();
                if (g != 0) host->mgr().filter_graph_post_event(g, EC_COMPLETE, 0, identity());
                lock.lock();
            }
            audio_cv.notify_all();
        }
    }

    bool running_locked() {
        std::lock_guard<std::mutex> lock(filter_mutex);
        return state == 2;
    }
    GuestAddr graph_iface() {
        std::lock_guard<std::mutex> lock(filter_mutex);
        return graph;
    }
    u32 current_state() {
        std::lock_guard<std::mutex> lock(filter_mutex);
        return state;
    }

protected:
    u32 on_stop(Marshaler& m) override {
        (void)FilterBase::on_stop(m);
        {
            std::lock_guard<std::mutex> lock(audio_mutex);
            if (samples != 0) {
                std::fprintf(stderr, "[quartz] DirectSound Renderer Stop (samples=%llu bytes=%llu eos=%d)\n",
                             static_cast<unsigned long long>(samples),
                             static_cast<unsigned long long>(bytes), eos ? 1 : 0);
            }
            samples = 0;
            bytes = 0;
            have_sample = false;
            eos = false;
            complete_sent = false;
            if (voice != nullptr) {
                voice->pause();
                voice->reset();
            }
        }
        audio_cv.notify_all();
        stop_engine();
        // CBaseInputPin::Inactive: the input pin decommits its allocator, so
        // an upstream streaming thread blocked in GetBuffer returns
        // VFW_E_NOT_COMMITTED and can leave before its filter's Stop joins it.
        GuestAddr allocator = 0;
        {
            std::lock_guard<std::mutex> lock(audio_mutex);
            allocator = input != nullptr ? input->allocator : 0;
        }
        if (allocator != 0) {
            const u32 decommit = host->call(m, allocator, Iface::MemAllocator, 6, {});
            std::fprintf(stderr, "[quartz] DirectSound Renderer Stop: allocator Decommit -> 0x%x\n",
                         decommit);
        }
        return hr::S_OK;
    }
    u32 on_pause(Marshaler& m) override {
        const u32 previous = current_state();
        (void)FilterBase::on_pause(m);
        std::lock_guard<std::mutex> lock(audio_mutex);
        if (previous == 2 && voice != nullptr) voice->pause();
        audio_cv.notify_all();
        // CBaseRenderer::Pause: S_FALSE while there is no sample to show
        // (and the stream has not already ended), on a connected pin.
        const bool connected = input != nullptr && input->pointer(Iface::Pin) != 0 &&
                               voice != nullptr;
        return (connected && !have_sample && !eos) ? hr::S_FALSE : hr::S_OK;
    }
    u32 on_run(Marshaler& m, i64 start) override {
        (void)FilterBase::on_run(m, start);
        {
            std::lock_guard<std::mutex> lock(audio_mutex);
            if (voice != nullptr) (void)voice->start();
        }
        audio_cv.notify_all();
        start_engine();
        return hr::S_OK;
    }
    u32 on_get_state(Marshaler& m, u32 timeout_ms, u32& state_out) override {
        (void)m;
        state_out = current_state();
        std::unique_lock<std::mutex> lock(audio_mutex);
        const auto cueing = [&] {
            return current_state() == 1 && voice != nullptr && !have_sample && !eos;
        };
        if (cueing() && timeout_ms != 0) {
            const auto ready = [&] { return !cueing(); };
            if (timeout_ms == 0xFFFFFFFFu) {
                audio_cv.wait(lock, ready);
            } else {
                audio_cv.wait_for(lock, std::chrono::milliseconds(timeout_ms), ready);
            }
        }
        return cueing() ? hr::VFW_S_STATE_INTERMEDIATE : hr::S_OK;
    }
    u32 call_other(Marshaler& m, Iface iface, u32 slot, std::span<const u64> args) override {
        const auto arg = [&](usize i) { return i < args.size() ? args[i] : 0; };
        if (iface == Iface::MiscFlags) {
            return slot == 3 ? 1u : hr::E_NOTIMPL;  // AM_FILTER_MISC_FLAGS_IS_RENDERER
        }
        if (iface == Iface::MediaSeeking) {
            // CRendererPosPassThru: the renderer's IMediaSeeking is the
            // upstream output pin's (E_NOTIMPL while unconnected).
            GuestAddr upstream = 0;
            {
                std::lock_guard<std::mutex> lock(input->pin_mutex);
                upstream = input->peer;
            }
            if (upstream == 0) return hr::E_NOTIMPL;
            const GuestAddr seeking = host->query(m, upstream, sdk::IID_IMediaSeeking);
            if (seeking == 0) return hr::E_NOTIMPL;
            const u32 result = host->call(m, seeking, Iface::MediaSeeking, slot, args);
            (void)host->release(m, seeking);
            return result;
        }
        if (iface != Iface::BasicAudio) return hr::E_NOTIMPL;
        switch (slot) {
        case 3: {  // GetTypeInfoCount(UINT*)
            const GuestAddr out = static_cast<GuestAddr>(arg(0));
            if (out == 0) return hr::E_POINTER;
            (void)m.write_u32(out, 0);
            return hr::S_OK;
        }
        case 4: case 5: case 6:  // GetTypeInfo / GetIDsOfNames / Invoke
            return hr::E_NOTIMPL;
        case 7: {  // put_Volume(long)
            const i32 v = static_cast<i32>(static_cast<u32>(arg(0)));
            if (v < -10000 || v > 0) return hr::E_INVALIDARG;
            std::lock_guard<std::mutex> lock(audio_mutex);
            volume = v;
            apply_gains();
            return hr::S_OK;
        }
        case 8: case 10: {  // get_Volume / get_Balance(long*)
            const GuestAddr out = static_cast<GuestAddr>(arg(0));
            if (out == 0) return hr::E_POINTER;
            std::lock_guard<std::mutex> lock(audio_mutex);
            (void)m.write_u32(out, static_cast<u32>(slot == 8 ? volume : balance));
            return hr::S_OK;
        }
        case 9: {  // put_Balance(long)
            const i32 v = static_cast<i32>(static_cast<u32>(arg(0)));
            if (v < -10000 || v > 10000) return hr::E_INVALIDARG;
            std::lock_guard<std::mutex> lock(audio_mutex);
            balance = v;
            apply_gains();
            return hr::S_OK;
        }
        default:
            return hr::E_NOTIMPL;
        }
    }

    friend class AudioInputPin;
};

u32 AudioInputPin::complete_connect(Marshaler& m) {
    (void)m;
    audio::PcmFormat format;
    if (!pcm_format(connected_type, format)) return hr::VFW_E_TYPE_NOT_ACCEPTED;
    audio::IAudioRenderer* device = host->mgr().audio_renderer();
    if (device == nullptr) return VFW_E_NO_AUDIO_HARDWARE;
    // One second of endpoint buffer, as DirectSound's default secondary.
    auto voice = device->create_push_voice(format, format.sample_rate, false);
    if (voice == nullptr) return VFW_E_NO_AUDIO_HARDWARE;
    std::lock_guard<std::mutex> lock(renderer->audio_mutex);
    renderer->format = format;
    renderer->voice = std::move(voice);
    renderer->apply_gains();
    return hr::S_OK;
}

void AudioInputPin::on_disconnect(Marshaler& m) {
    renderer->stop_engine();
    GuestAddr old = 0;
    {
        std::lock_guard<std::mutex> lock(renderer->audio_mutex);
        renderer->voice.reset();
        old = allocator;
        allocator = 0;
    }
    if (old != 0) (void)host->release(m, old);
}

u32 AudioInputPin::on_end_of_stream(Marshaler& m) {
    (void)m;
    std::lock_guard<std::mutex> lock(renderer->audio_mutex);
    if (renderer->current_state() == 0) return hr::VFW_E_WRONG_STATE;
    if (flushing) return hr::S_FALSE;
    renderer->eos = true;
    std::fprintf(stderr, "[quartz] DirectSound Renderer EndOfStream state=%u samples=%llu bytes=%llu\n",
                 renderer->current_state(), static_cast<unsigned long long>(renderer->samples),
                 static_cast<unsigned long long>(renderer->bytes));
    renderer->audio_cv.notify_all();
    return hr::S_OK;
}

u32 AudioInputPin::on_begin_flush(Marshaler& m) {
    (void)m;
    std::lock_guard<std::mutex> lock(renderer->audio_mutex);
    flushing = true;
    renderer->have_sample = false;
    renderer->eos = false;
    renderer->complete_sent = false;
    if (renderer->voice != nullptr) renderer->voice->reset();
    renderer->audio_cv.notify_all();
    return hr::S_OK;
}

u32 AudioInputPin::on_end_flush(Marshaler& m) {
    (void)m;
    std::lock_guard<std::mutex> lock(renderer->audio_mutex);
    flushing = false;
    return hr::S_OK;
}

u32 AudioInputPin::call_other(Marshaler& m, Iface iface, u32 slot, std::span<const u64> args) {
    if (iface != Iface::MemInputPin) return hr::E_NOTIMPL;
    const auto arg = [&](usize i) { return i < args.size() ? args[i] : 0; };
    switch (slot) {
    case 3: {  // GetAllocator(IMemAllocator**): CBaseInputPin makes a CMemAllocator
        const GuestAddr out = static_cast<GuestAddr>(arg(0));
        if (out == 0) return hr::E_POINTER;
        GuestAddr a = 0;
        {
            std::lock_guard<std::mutex> lock(renderer->audio_mutex);
            a = allocator;
        }
        if (a == 0) {
            a = create_memory_allocator(*host, m);
            if (a == 0) return hr::E_OUTOFMEMORY;
            std::lock_guard<std::mutex> lock(renderer->audio_mutex);
            allocator = a;
        }
        (void)host->add_ref(m, a);
        (void)m.write_ptr(out, a);
        return hr::S_OK;
    }
    case 4: {  // NotifyAllocator(IMemAllocator*, BOOL bReadOnly)
        const GuestAddr a = static_cast<GuestAddr>(arg(0));
        if (a == 0) return hr::E_POINTER;
        (void)host->add_ref(m, a);
        GuestAddr old = 0;
        {
            std::lock_guard<std::mutex> lock(renderer->audio_mutex);
            old = allocator;
            allocator = a;
        }
        if (old != 0) (void)host->release(m, old);
        return hr::S_OK;
    }
    case 5:  // GetAllocatorRequirements
        return hr::E_NOTIMPL;
    case 6:  // Receive(IMediaSample*)
    case 7: {  // ReceiveMultiple(IMediaSample**, long, long*)
        std::vector<GuestAddr> samples;
        GuestAddr processed = 0;
        if (slot == 6) {
            samples.push_back(static_cast<GuestAddr>(arg(0)));
        } else {
            const GuestAddr list = static_cast<GuestAddr>(arg(0));
            const i32 n = static_cast<i32>(static_cast<u32>(arg(1)));
            processed = static_cast<GuestAddr>(arg(2));
            if (list == 0 || processed == 0) return hr::E_POINTER;
            const GuestAddr ps = m.cpu_is_64bit() ? 8u : 4u;
            for (i32 i = 0; i < n; ++i) {
                samples.push_back(static_cast<GuestAddr>(m.read_ptr(list + static_cast<GuestAddr>(i) * ps).value_or(0)));
            }
        }
        u32 result = hr::S_OK;
        u32 done = 0;
        for (const GuestAddr sample : samples) {
            if (sample == 0) {
                result = hr::E_POINTER;
                break;
            }
            {
                std::lock_guard<std::mutex> lock(renderer->audio_mutex);
                if (renderer->current_state() == 0) { result = hr::VFW_E_WRONG_STATE; break; }
                if (flushing) { result = hr::S_FALSE; break; }
                if (renderer->eos) { result = VFW_E_SAMPLE_REJECTED_EOS; break; }
            }
            const GuestAddr buf = host->scratch(8);
            (void)m.write_ptr(buf, 0);
            (void)host->call(m, sample, Iface::MediaSample, 3, {buf});
            const GuestAddr data = static_cast<GuestAddr>(m.read_ptr(buf).value_or(0));
            host->free_scratch(buf);
            const u32 length = host->call(m, sample, Iface::MediaSample, 11, {});
            std::vector<u8> bytes(length);
            if (data != 0 && length != 0) (void)m.memory().read_bytes(data, bytes.data(), length);
            std::unique_lock<std::mutex> lock(renderer->audio_mutex);
            const u32 block = renderer->format.block_align();
            if (renderer->voice != nullptr && block != 0 && !bytes.empty()) {
                // Flow control: a running renderer keeps about half a second
                // queued and makes the sender wait beyond that.
                const u32 limit = renderer->format.sample_rate / 2u;
                bool waited = false;
                while (renderer->current_state() == 2 && !flushing &&
                       renderer->voice->padding_frames() > limit) {
                    waited = true;
                    renderer->audio_cv.wait_for(lock, std::chrono::milliseconds(5));
                }
                if (flushing || renderer->current_state() == 0) {
                    if (waited) {
                        std::fprintf(stderr,
                                     "[quartz] DirectSound Renderer: Receive waiting for buffer "
                                     "room released by %s\n",
                                     flushing ? "BeginFlush" : "Stop");
                    }
                    // CBaseRenderer: a stopped renderer refuses samples.
                    result = flushing ? hr::S_FALSE : hr::VFW_E_WRONG_STATE;
                    break;
                }
                (void)renderer->voice->submit(bytes.data(), static_cast<u32>(bytes.size()) / block, false);
            }
            const bool first = !renderer->have_sample;
            if (renderer->samples == 0) {
                std::fprintf(stderr, "[quartz] DirectSound Renderer first sample state=%u bytes=%zu\n",
                             renderer->current_state(), bytes.size());
            }
            ++renderer->samples;
            renderer->bytes += bytes.size();
            renderer->have_sample = true;
            renderer->audio_cv.notify_all();
            // CBaseRenderer: a paused renderer holds its preroll and blocks
            // the sender until Run, Stop or a flush.
            if (first && renderer->current_state() == 1) {
                renderer->audio_cv.wait(lock, [&] {
                    return renderer->current_state() != 1 || flushing;
                });
                std::fprintf(stderr,
                             "[quartz] DirectSound Renderer: preroll Receive released by %s\n",
                             flushing ? "BeginFlush"
                                      : renderer->current_state() == 2 ? "Run" : "Stop");
            }
            ++done;
        }
        if (processed != 0) (void)m.write_u32(processed, done);
        return result;
    }
    case 8:  // ReceiveCanBlock: S_OK, Receive can block (flow control, preroll)
        return hr::S_OK;
    default:
        return hr::E_NOTIMPL;
    }
}

} // namespace

GuestAddr create_audio_renderer(DShowHost& host, Marshaler& m, u32* hr_out) {
    if (host.mgr().audio_renderer() == nullptr) {
        if (hr_out != nullptr) *hr_out = VFW_E_NO_AUDIO_HARDWARE;  // measured on winoracle
        return 0;
    }
    auto filter = std::make_shared<AudioRenderer>();
    filter->clsid = sdk::CLSID_DSoundRender;
    if (!host.publish(m, filter, {Iface::BaseFilter, Iface::MiscFlags, Iface::BasicAudio,
                                  Iface::MediaSeeking})) {
        if (hr_out != nullptr) *hr_out = hr::E_OUTOFMEMORY;
        return 0;
    }
    auto pin = std::make_shared<AudioInputPin>();
    pin->renderer = filter.get();
    pin->output = false;
    // Pin name per the stock filter (not measurable on winoracle: no device).
    pin->name = u"Audio Input pin (rendered)";
    if (!publish_pin(host, m, *filter, pin, {Iface::Pin, Iface::MemInputPin})) {
        if (hr_out != nullptr) *hr_out = hr::E_OUTOFMEMORY;
        return 0;
    }
    filter->input = pin;
    if (hr_out != nullptr) *hr_out = hr::S_OK;
    return filter->pointer(Iface::BaseFilter);
}

} // namespace vitreo::winapi::dshow
