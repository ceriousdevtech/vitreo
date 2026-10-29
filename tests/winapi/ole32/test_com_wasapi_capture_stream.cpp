// SPDX-License-Identifier: LGPL-2.1-only

/// @file test_com_wasapi_capture_stream.cpp
/// @brief IAudioCaptureClient packet, session, autoconvert and channel tests (instantiated
/// in test_com_wasapi_capture_devices.cpp).

#include "com_test_common.hpp"
#include "com_test_wasapi_capture_fixture.hpp"

namespace {
#include "wasapi_autoconvert_oracle.inc"
}  // namespace

TEST_P(WasapiCaptureTest, CaptureClientFormatsBuffersAndServices) {
    const GuestAddr client = capture_client();
    // GetMixFormat: "tag=fffe bits=32 cb=22", the 48 kHz engine rate (as on
    // Windows, whose 44.1 kHz devices mix at 48 kHz) and the device's
    // channels and speaker mask.
    ASSERT_EQ(invoke(&ComCoreManager::audio_client_get_mix_format, {client, OUT}), com::S_OK);
    const GuestAddr mix = static_cast<GuestAddr>(ptr(OUT));
    EXPECT_EQ(memory_.read_u16(mix).value(), 0xFFFEu);
    EXPECT_EQ(memory_.read_u16(mix + 2).value(), 2u);
    EXPECT_EQ(memory_.read_u32(mix + 4).value(), 48000u);
    EXPECT_EQ(memory_.read_u16(mix + 14).value(), 32u);
    EXPECT_EQ(memory_.read_u16(mix + 16).value(), 22u);
    EXPECT_EQ(memory_.read_u32(mix + 20).value(), 3u);
    EXPECT_TRUE(same_guid(read_guid(mix + 24), kSubtypeFloat));
    const GuestAddr usb = capture_client("uid-usb");
    ASSERT_EQ(invoke(&ComCoreManager::audio_client_get_mix_format, {usb, OUT}), com::S_OK);
    const GuestAddr usb_mix = static_cast<GuestAddr>(ptr(OUT));
    EXPECT_EQ(memory_.read_u16(usb_mix + 2).value(), 1u);
    EXPECT_EQ(memory_.read_u32(usb_mix + 4).value(), 48000u);
    EXPECT_EQ(memory_.read_u32(usb_mix + 20).value(), 4u);  // KSAUDIO_SPEAKER_MONO
    // PKEY_AudioEngine_DeviceFormat keeps the device's own rate ("devfmt
    // 44100 / mix 48000" on Windows).
    {
        auto pm = make_marshaler();
        const GuestAddr store = mgr_.create_audio_property_store(
            pm, ComCoreManager::AudioEndpointRef{true, "uid-usb"});
        write_guid(C_KEY, kPkeyAudioEngine);
        ASSERT_TRUE(memory_.write_u32(C_KEY + 16, 0).is_ok());
        ASSERT_EQ(invoke(&ComCoreManager::property_store_get_value, {store, C_KEY, C_VALUE}),
                  com::S_OK);
        const GuestAddr blob = static_cast<GuestAddr>(ptr(C_VALUE + (x64_ ? 16 : 12)));
        EXPECT_EQ(memory_.read_u32(blob + 4).value(), 44100u);
        EXPECT_EQ(memory_.read_u16(blob + 2).value(), 1u);
    }
    // "period def=100000 min=30000".
    ASSERT_EQ(invoke(&ComCoreManager::audio_client_get_device_period, {client, OUT, OUT2}),
              com::S_OK);
    EXPECT_EQ(r64(OUT), 100000u);
    EXPECT_EQ(r64(OUT2), 30000u);
    // "IsFormatSupported(mix) hr=0 closest=null", "(16k mono) hr=1 closest=set".
    wptr(OUT, 0x1234);
    EXPECT_EQ(invoke(&ComCoreManager::audio_client_is_format_supported, {client, 0, mix, OUT}),
              com::S_OK);
    EXPECT_EQ(ptr(OUT), 0u);
    write_format(1, 1, 16000, 16);
    ASSERT_EQ(invoke(&ComCoreManager::audio_client_is_format_supported,
                     {client, 0, FORMAT_ADDR, OUT}), com::S_FALSE);
    EXPECT_EQ(memory_.read_u32(static_cast<GuestAddr>(ptr(OUT)) + 4).value(), 48000u);
    // "Initialize(LOOPBACK on capture) hr=88890003", "(16k mono, no
    // autoconvert) hr=88890008"; exclusive mode is not offered (as for render).
    EXPECT_EQ(init(client, 0, kStreamFlagsLoopbackCapture, 0, mix), kAudclntWrongEndpointType);
    EXPECT_EQ(init(client, 0, 0, 0), kAudclntUnsupportedFormat);
    EXPECT_EQ(init(client, 1, 0, 100000, mix), kAudclntExclusiveModeNotAllowed);
    // "Initialize(pull, 0) buffer=1056 latency=0", "(event, mix) buffer=1056",
    // "(pull, 200ms) buffer=9600".
    ASSERT_EQ(init(client, 0, 0, 0, mix), com::S_OK);
    EXPECT_EQ(buffer_size(client), 1056u);
    ASSERT_EQ(invoke(&ComCoreManager::audio_client_get_stream_latency, {client, OUT}), com::S_OK);
    EXPECT_EQ(r64(OUT), 0u);
    const GuestAddr event_client = capture_client();
    ASSERT_EQ(init(event_client, 0, kStreamFlagsEventCallback, 0, mix), com::S_OK);
    EXPECT_EQ(buffer_size(event_client), 1056u);
    const GuestAddr long_client = capture_client();
    ASSERT_EQ(init(long_client, 0, 0, 2000000, mix), com::S_OK);
    EXPECT_EQ(buffer_size(long_client), 9600u);
    EXPECT_EQ(capture_.last_ring_frames, 9600u);
    // AUTOCONVERTPCM opens the host stream in the client's format;
    // "Initialize(16k mono, AUTOCONVERTPCM, 20ms) buffer=356".
    const GuestAddr convert = capture_client();
    write_format(1, 1, 16000, 16);
    ASSERT_EQ(init(convert, 0, kStreamFlagsAutoConvertPcm | kSrcDefaultQuality, 200000),
              com::S_OK);
    EXPECT_EQ(buffer_size(convert), 356u);
    EXPECT_EQ(capture_.last->format.sample_rate, 16000u);
    EXPECT_EQ(capture_.last->format.channels, 1u);
    EXPECT_EQ(capture_.last->device, "uid-builtin");
    // "GetService(IAudioRenderClient) on capture hr=88890003 out=null",
    // "GetService(IAudioCaptureClient) hr=0 second-same=1",
    // "capture IAudioClock freq-is-avgbytes=1".
    auto [rhr, rout] = service(client, kIidAudioRenderClient);
    EXPECT_EQ(rhr, kAudclntWrongEndpointType);
    EXPECT_EQ(rout, 0u);
    const GuestAddr cc = must_service(client, kIidAudioCaptureClient);
    EXPECT_EQ(must_service(client, kIidAudioCaptureClient), cc);
    const GuestAddr clock = must_service(client, kIidAudioClock);
    ASSERT_EQ(invoke(&ComCoreManager::audio_clock_get_frequency, {clock, OUT}), com::S_OK);
    EXPECT_EQ(r64(OUT), 384000u);
    // The render endpoint keeps refusing a capture client, and loopback of
    // the render endpoint fails with a documented Initialize error.
    const GuestAddr render = open_stream();
    EXPECT_EQ(service(render, kIidAudioCaptureClient).first, kAudclntWrongEndpointType);
    auto m = make_marshaler();
    const GuestAddr render2 = mgr_.create_audio_client(m);
    write_format(3, 2, 48000, 32);
    EXPECT_EQ(init(render2, 0, kStreamFlagsLoopbackCapture, 0), kAudclntEndpointCreateFailed);
}

TEST_P(WasapiCaptureTest, CapturePacketsFollowTheEngineRules) {
    const GuestAddr client = capture_client();
    write_format(3, 2, 48000, 32);
    ASSERT_EQ(init(client, 0, kStreamFlagsEventCallback, 0), com::S_OK);
    const Handle event = sync_.create_event(false, false, "");
    ASSERT_EQ(invoke(&ComCoreManager::audio_client_set_event_handle, {client, event}), com::S_OK);
    const GuestAddr cc = must_service(client, kIidAudioCaptureClient);
    test::FakeCaptureStream* stream = capture_.last;
    ASSERT_NE(stream, nullptr);
    // Before Start: "GetCurrentPadding pad=0", "GetNextPacketSize n=0",
    // "GetNextPacketSize(NULL) 80004003", "GetBuffer hr=08890001 n=0
    // flags=99 data-null=1 dp=77 qp=77", "ReleaseBuffer(0) after empty 0",
    // "ReleaseBuffer(5) with nothing held 88890007", NULL out-pointers E_POINTER.
    EXPECT_EQ(padding(client), 0u);
    EXPECT_EQ(next_packet(cc), 0u);
    EXPECT_EQ(invoke(&ComCoreManager::audio_capture_client_get_next_packet_size, {cc, 0}), kEPointer);
    wptr(C_DATA, 1);
    ASSERT_TRUE(memory_.write_u32(C_FRAMES, 99).is_ok());
    ASSERT_TRUE(memory_.write_u32(C_FLAGS, 99).is_ok());
    ASSERT_TRUE(memory_.write_u64(C_POS, 77).is_ok());
    ASSERT_TRUE(memory_.write_u64(C_QPC, 77).is_ok());
    EXPECT_EQ(get_buffer(cc), kAudclntSBufferEmpty);
    EXPECT_EQ(ptr(C_DATA), 0u);
    EXPECT_EQ(memory_.read_u32(C_FRAMES).value(), 0u);
    EXPECT_EQ(memory_.read_u32(C_FLAGS).value(), 99u);
    EXPECT_EQ(r64(C_POS), 77u);
    EXPECT_EQ(r64(C_QPC), 77u);
    EXPECT_EQ(release(cc, 0), com::S_OK);
    EXPECT_EQ(release(cc, 5), kAudclntOutOfOrder);
    EXPECT_EQ(invoke(&ComCoreManager::audio_capture_client_get_buffer, {cc, 0, C_FRAMES, C_FLAGS, 0, 0}), kEPointer);
    EXPECT_EQ(invoke(&ComCoreManager::audio_capture_client_get_buffer, {cc, C_DATA, 0, C_FLAGS, 0, 0}), kEPointer);
    EXPECT_EQ(invoke(&ComCoreManager::audio_capture_client_get_buffer, {cc, C_DATA, C_FRAMES, 0, 0, 0}), kEPointer);
    // Start; the event is signaled once a whole 10 ms packet is waiting.
    ASSERT_EQ(invoke(&ComCoreManager::audio_client_start, {client}), com::S_OK);
    EXPECT_EQ(stream->starts, 1);
    stream->feed(float_frames(300, 0.25f), 5000);
    EXPECT_EQ(sync_.wait_for_single_object(event, 0), WAIT_TIMEOUT);
    EXPECT_EQ(next_packet(cc), 0u);
    stream->feed(float_frames(300, 0.5f), 5000 + 62500);
    EXPECT_EQ(sync_.wait_for_single_object(event, 0), WAIT_OBJECT_0);
    // "packet[0] next-eq-n=1 pad>=n=1 n=480 flags=1": the first packet of the
    // stream is discontinuous. Each packet is one 10 ms engine period k at
    // device position 480 * k, and the first delivered period is k = 1
    // ("wasapi positions first=480 period-aligned=1").
    EXPECT_EQ(next_packet(cc), 480u);
    EXPECT_EQ(padding(client), 600u);
    ASSERT_EQ(get_buffer(cc), com::S_OK);
    EXPECT_EQ(memory_.read_u32(C_FRAMES).value(), 480u);
    EXPECT_EQ(memory_.read_u32(C_FLAGS).value(), 1u);
    EXPECT_EQ(r64(C_POS), 480u);
    EXPECT_EQ(r64(C_QPC), 5000u);
    const GuestAddr data = static_cast<GuestAddr>(ptr(C_DATA));
    EXPECT_EQ(rf(data), 0.25f);
    EXPECT_EQ(rf(data + 300 * 8), 0.5f);
    // "GetBuffer while held 88890007", "ReleaseBuffer(n+1) / (1 partial)
    // 88890009"; ReleaseBuffer(0) hands the packet back (the same packet
    // returns); ReleaseBuffer(n) consumes it.
    EXPECT_EQ(get_buffer(cc), kAudclntOutOfOrder);
    EXPECT_EQ(release(cc, 481), kAudclntInvalidSize);
    EXPECT_EQ(release(cc, 1), kAudclntInvalidSize);
    EXPECT_EQ(release(cc, 0), com::S_OK);
    ASSERT_EQ(get_buffer(cc), com::S_OK);
    EXPECT_EQ(r64(C_POS), 480u);
    EXPECT_EQ(release(cc, 480), com::S_OK);
    // The 120 frames left are not a packet while the stream runs.
    EXPECT_EQ(next_packet(cc), 0u);
    stream->feed(float_frames(840, 0.75f));
    ASSERT_EQ(get_buffer(cc), com::S_OK);
    EXPECT_EQ(memory_.read_u32(C_FLAGS).value(), 0u);  // "flags=0 dp-step=480"
    EXPECT_EQ(r64(C_POS), 960u);
    EXPECT_EQ(release(cc, 480), com::S_OK);
    ASSERT_EQ(get_buffer(cc), com::S_OK);
    EXPECT_EQ(r64(C_POS), 1440u);
    EXPECT_EQ(release(cc, 480), com::S_OK);
    // "Reset(running) 88890005"; Stop keeps what was captured ("GetBuffer
    // after Stop hr=0 data-kept=1"), the remainder is the last packet;
    // "Stop again hr=1".
    EXPECT_EQ(invoke(&ComCoreManager::audio_client_reset, {client}), kAudclntNotStopped);
    stream->feed(float_frames(100, 0.1f));
    ASSERT_EQ(invoke(&ComCoreManager::audio_client_stop, {client}), com::S_OK);
    EXPECT_EQ(invoke(&ComCoreManager::audio_client_stop, {client}), com::S_FALSE);
    EXPECT_EQ(padding(client), 100u);
    EXPECT_EQ(next_packet(cc), 100u);
    ASSERT_EQ(get_buffer(cc), com::S_OK);
    EXPECT_EQ(r64(C_POS), 1920u);  // period 4, cut short by Stop
    EXPECT_EQ(release(cc, 0), com::S_OK);
    // The clock counts the frames delivered into the endpoint buffer.
    const GuestAddr clock = must_service(client, kIidAudioClock);
    ASSERT_EQ(invoke(&ComCoreManager::audio_clock_get_position, {clock, OUT, 0}), com::S_OK);
    EXPECT_EQ(r64(OUT), (600u + 840u + 100u) * 8u);
    // A packet held blocks Reset (AUDCLNT_E_BUFFER_OPERATION_PENDING); then
    // "Reset hr=0", "after Reset next=0 pad=0", "Reset again hr=0", "clock
    // after Reset pos=0".
    ASSERT_EQ(get_buffer(cc), com::S_OK);
    EXPECT_EQ(invoke(&ComCoreManager::audio_client_reset, {client}), kAudclntBufferOperationPending);
    EXPECT_EQ(release(cc, 100), com::S_OK);
    EXPECT_EQ(invoke(&ComCoreManager::audio_client_reset, {client}), com::S_OK);
    EXPECT_EQ(next_packet(cc), 0u);
    EXPECT_EQ(padding(client), 0u);
    EXPECT_EQ(invoke(&ComCoreManager::audio_client_reset, {client}), com::S_OK);
    ASSERT_EQ(invoke(&ComCoreManager::audio_clock_get_position, {clock, OUT, 0}), com::S_OK);
    EXPECT_EQ(r64(OUT), 0u);
    // "restart first packet flags=1"; the device position runs on through
    // Reset ("reset-keeps-position=1") and the restart resumes at the period
    // after the one Stop ended in ("restart-continues=1"): period 5.
    ASSERT_EQ(invoke(&ComCoreManager::audio_client_start, {client}), com::S_OK);
    stream->feed(float_frames(480, 0.2f));
    ASSERT_EQ(get_buffer(cc), com::S_OK);
    EXPECT_EQ(memory_.read_u32(C_FLAGS).value(), 1u);
    EXPECT_EQ(r64(C_POS), 2400u);
    EXPECT_EQ(release(cc, 480), com::S_OK);
    // Overrun: the buffer keeps its OLDEST 1056 frames ("padding-is-buffer=1
    // held-is-buffer=1 held-contiguous=1 first-dp-minus-expected=0
    // packets=480/0,480/0,96/1"), and the next packet after the gap is
    // discontinuous with a jump in position ("flags=1 dp-jumped=1").
    stream->feed(float_frames(3000, 0.3f));
    EXPECT_EQ(padding(client), 1056u);
    std::vector<std::pair<u32, u32>> shape;
    u64 expect = 2880;
    while (get_buffer(cc) == com::S_OK) {
        EXPECT_EQ(r64(C_POS), expect);
        const u32 n = memory_.read_u32(C_FRAMES).value();
        shape.emplace_back(n, memory_.read_u32(C_FLAGS).value());
        expect += n;
        ASSERT_EQ(release(cc, n), com::S_OK);
    }
    EXPECT_EQ(shape, (std::vector<std::pair<u32, u32>>{{480, 0}, {480, 0}, {96, 1}}));
    // The rest of the period the loss began in is lost too: delivery
    // resumes with a whole period (480 * 13; the 3000 frames ran to 5400).
    stream->feed(float_frames(480, 0.4f));
    EXPECT_EQ(next_packet(cc), 0u);
    stream->feed(float_frames(480, 0.4f));
    ASSERT_EQ(get_buffer(cc), com::S_OK);
    EXPECT_EQ(memory_.read_u32(C_FRAMES).value(), 480u);
    EXPECT_EQ(memory_.read_u32(C_FLAGS).value(), 1u);
    EXPECT_EQ(r64(C_POS), 6240u);
    EXPECT_EQ(release(cc, 480), com::S_OK);
    ASSERT_EQ(invoke(&ComCoreManager::audio_client_stop, {client}), com::S_OK);
    (void)sync_.close_handle(event);
}

TEST_P(WasapiCaptureTest, CaptureSessionsAndRateAdjustment) {
    const GuestAddr client = capture_client();
    write_format(3, 2, 48000, 32);
    ASSERT_EQ(init(client, 0, 0, 0), com::S_OK);
    // Capture sessions are the capture endpoint's own: the identifier starts
    // with the capture endpoint id.
    const GuestAddr session = must_service(client, kIidAudioSessionControl);
    auto [hr, control2] = qi(session, kIidAudioSessionControl2);
    ASSERT_EQ(hr, com::S_OK);
    ASSERT_EQ(invoke(&ComCoreManager::audio_session_get_session_identifier,
                     {static_cast<GuestAddr>(control2), OUT}), com::S_OK);
    EXPECT_EQ(wstr(static_cast<GuestAddr>(ptr(OUT))).substr(0, endpoint("uid-builtin").size()),
              endpoint("uid-builtin"));
    // IAudioClockAdjustment cannot change a capture stream's rate.
    const GuestAddr adjust_client = capture_client();
    ASSERT_EQ(init(adjust_client, 0, kStreamFlagsRateAdjust, 0), com::S_OK);
    const GuestAddr adjust = must_service(adjust_client, kIidAudioClockAdjustment);
    EXPECT_EQ(invoke(&ComCoreManager::audio_clock_adjustment_set_sample_rate,
                     {adjust, fbits(44100.0f)}, 1), kEInvalidArg);
}

TEST_P(WasapiCaptureTest, CaptureSessionVolumeScalesCapturedSamples) {
    // Learn (Session Volume Controls): the engine multiplies a stream's
    // samples by stream x session-channel x session-master levels. A capture
    // device's sessions keep their own levels (sessions belong to an
    // endpoint); render sessions' levels do not reach the microphone.
    // Windows' scaling of captured samples is unmeasured (Windows's inputs
    // are silent: "flags-or=1 peak=0").
    const GuestAddr client = capture_client();
    write_format(3, 2, 48000, 32);
    ASSERT_EQ(init(client, 0, 0, 0), com::S_OK);
    const GuestAddr cc = must_service(client, kIidAudioCaptureClient);
    const GuestAddr simple = must_service(client, kIidSimpleAudioVolume);
    const GuestAddr channel = must_service(client, kIidChannelAudioVolume);
    const GuestAddr render = open_stream();
    const GuestAddr render_simple = must_service(render, kIidSimpleAudioVolume);
    ASSERT_EQ(invoke(&ComCoreManager::simple_audio_volume_set_master_volume,
                     {render_simple, fbits(0.25f), 0}, 1), com::S_OK);
    ASSERT_EQ(invoke(&ComCoreManager::simple_audio_volume_set_master_volume,
                     {simple, fbits(0.5f), 0}, 1), com::S_OK);
    ASSERT_EQ(invoke(&ComCoreManager::channel_audio_volume_set_channel_volume,
                     {channel, 1, fbits(0.5f), 0}, 2), com::S_OK);
    ASSERT_EQ(invoke(&ComCoreManager::simple_audio_volume_get_master_volume, {simple, OUT}),
              com::S_OK);
    EXPECT_EQ(rf(OUT), 0.5f);
    ASSERT_EQ(invoke(&ComCoreManager::simple_audio_volume_get_master_volume, {render_simple, OUT}),
              com::S_OK);
    EXPECT_EQ(rf(OUT), 0.25f);
    ASSERT_EQ(invoke(&ComCoreManager::audio_client_start, {client}), com::S_OK);
    capture_.last->feed(float_frames(480, 0.8f));
    ASSERT_EQ(get_buffer(cc), com::S_OK);
    const GuestAddr data = static_cast<GuestAddr>(ptr(C_DATA));
    EXPECT_FLOAT_EQ(rf(data), 0.4f);      // left: master 0.5
    EXPECT_FLOAT_EQ(rf(data + 4), 0.2f);  // right: master 0.5 x channel 0.5
    EXPECT_EQ(release(cc, 480), com::S_OK);
    ASSERT_EQ(invoke(&ComCoreManager::simple_audio_volume_set_mute, {simple, 1, 0}), com::S_OK);
    capture_.last->feed(float_frames(480, 0.8f));
    ASSERT_EQ(get_buffer(cc), com::S_OK);
    EXPECT_EQ(rf(static_cast<GuestAddr>(ptr(C_DATA))), 0.0f);
    EXPECT_EQ(release(cc, 480), com::S_OK);
}

TEST_P(WasapiCaptureTest, AutoconvertFollowsTheMeasuredEngine) {
    // AUTOCONVERTPCM on the 48 kHz engine, measured on Windows
    // (wasapi_autoconvert_oracle.inc, generated from the .expected files).
    // The zero-duration buffer rule holds at every probed rate:
    for (const auto& m : kAutoconvertBuffers) {
        SCOPED_TRACE(m.rate);
        if (m.hr == com::S_OK) EXPECT_EQ(ComCoreManager::capture_minimum_buffer(m.rate), m.buffer);
    }
    // Initialize: rates outside 10..384000 Hz are AUDCLNT_E_UNSUPPORTED_FORMAT
    // ("rate=9", "rate=384001"); a rate whose converter schedule was not
    // measured is refused the same way (UNIMPLEMENTED: its packets cannot be
    // reproduced), and opens no host stream.
    for (const u32 rate : {9u, 384001u, 12345u, 7999u}) {
        SCOPED_TRACE(rate);
        const int opens = capture_.opens;
        write_format(1, 1, rate, 16);
        EXPECT_EQ(init(capture_client(), 0, kStreamFlagsAutoConvertPcm | kSrcDefaultQuality, 0),
                  kAudclntUnsupportedFormat);
        EXPECT_EQ(capture_.opens, opens);
    }
    // Every measured schedule, both converter qualities ("wasapi src"): the
    // buffer, then the first 12 packets as frames arrive (each one engine
    // period k at device position 480 * k).
    for (const auto& s : kAutoconvertSchedules) {
        SCOPED_TRACE(::testing::Message() << s.rate << (s.default_quality ? " default" : " fast"));
        const GuestAddr client = capture_client();
        write_format(1, 1, s.rate, 16);
        ASSERT_EQ(init(client, 0,
                       kStreamFlagsAutoConvertPcm | (s.default_quality ? kSrcDefaultQuality : 0u),
                       0),
                  com::S_OK);
        EXPECT_EQ(buffer_size(client), s.buffer);
        const GuestAddr cc = must_service(client, kIidAudioCaptureClient);
        ASSERT_EQ(invoke(&ComCoreManager::audio_client_start, {client}), com::S_OK);
        test::FakeCaptureStream* stream = capture_.last;
        std::vector<u32> sizes;
        std::vector<u64> positions;
        const u32 chunk = std::max<u32>(s.rate / 400u, 1u);  // 2.5 ms of frames
        for (int step = 0; sizes.size() < s.sizes.size() && step < 4000; ++step) {
            stream->feed_frames(chunk, 0x11);
            while (sizes.size() < s.sizes.size() && get_buffer(cc) == com::S_OK) {
                const u32 n = memory_.read_u32(C_FRAMES).value();
                sizes.push_back(n);
                positions.push_back(r64(C_POS));
                ASSERT_EQ(release(cc, n), com::S_OK);
            }
        }
        EXPECT_EQ(sizes, std::vector<u32>(s.sizes.begin(), s.sizes.end()));
        for (usize i = 0; i < positions.size(); ++i) EXPECT_EQ(positions[i], 480u * (i + 1));
        ASSERT_EQ(invoke(&ComCoreManager::audio_client_stop, {client}), com::S_OK);
    }
}

TEST_P(WasapiCaptureTest, CaptureChannelsFollowTheDevice) {
    // The mix format carries the input's own channel count and speaker mask
    // (here KSAUDIO_SPEAKER_QUAD); a client format of other channels needs
    // AUTOCONVERTPCM and an exact conversion (mono<->stereo), otherwise it
    // is AUDCLNT_E_UNSUPPORTED_FORMAT (UNIMPLEMENTED: no speaker matrix).
    capture_.list.push_back(audio::CaptureDevice{"uid-quad", u"Quad Array", 48000, 4, 0x33u});
    const GuestAddr quad = capture_client("uid-quad");
    ASSERT_EQ(invoke(&ComCoreManager::audio_client_get_mix_format, {quad, OUT}), com::S_OK);
    const GuestAddr mix = static_cast<GuestAddr>(ptr(OUT));
    EXPECT_EQ(memory_.read_u16(mix + 2).value(), 4u);
    EXPECT_EQ(memory_.read_u16(mix + 12).value(), 16u);
    EXPECT_EQ(memory_.read_u32(mix + 20).value(), 0x33u);
    ASSERT_EQ(init(quad, 0, 0, 0, mix), com::S_OK);
    EXPECT_EQ(capture_.last->format.channels, 4u);
    write_format(1, 2, 48000, 16);
    EXPECT_EQ(init(capture_client("uid-quad"), 0, kStreamFlagsAutoConvertPcm | kSrcDefaultQuality, 0),
              kAudclntUnsupportedFormat);
    write_format(1, 1, 48000, 16);
    EXPECT_EQ(init(capture_client("uid-quad"), 0, kStreamFlagsAutoConvertPcm | kSrcDefaultQuality, 0),
              kAudclntUnsupportedFormat);
    // The mono USB input: a stereo client is a duplicated channel.
    write_format(1, 2, 16000, 16);
    EXPECT_EQ(init(capture_client("uid-usb"), 0, kStreamFlagsAutoConvertPcm | kSrcDefaultQuality, 0),
              com::S_OK);
    EXPECT_EQ(capture_.last->format.channels, 2u);
    EXPECT_EQ(capture_.last->device, "uid-usb");
    // An input whose host layout names no positions: KSAUDIO_SPEAKER_DIRECTOUT.
    capture_.list.push_back(audio::CaptureDevice{"uid-array", u"Array", 48000, 6, 0x0u});
    ASSERT_EQ(invoke(&ComCoreManager::audio_client_get_mix_format,
                     {capture_client("uid-array"), OUT}), com::S_OK);
    EXPECT_EQ(memory_.read_u16(static_cast<GuestAddr>(ptr(OUT)) + 2).value(), 6u);
    EXPECT_EQ(memory_.read_u32(static_cast<GuestAddr>(ptr(OUT)) + 20).value(), 0u);
}
