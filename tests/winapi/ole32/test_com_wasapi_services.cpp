// SPDX-License-Identifier: LGPL-2.1-only

/// @file test_com_wasapi_services.cpp
/// @brief IAudioClient::GetService, IAudioClock and activation tests. Owns the
/// Bitness/WasapiServicesTest instantiation.

#include "com_test_common.hpp"
#include "com_test_wasapi_services_fixture.hpp"

INSTANTIATE_TEST_SUITE_P(Bitness, WasapiServicesTest, ::testing::Values(false, true),
                         [](const ::testing::TestParamInfo<bool>& info) {
                             return info.param ? "x64" : "x86";
                         });

TEST_P(WasapiServicesTest, GetServiceAnswersEveryRiidAndSharesObjects) {
    auto m = make_marshaler();
    const GuestAddr raw = mgr_.create_audio_client(m);
    // Measured: before Initialize the service IIDs say NOT_INITIALIZED and
    // every other IID E_NOINTERFACE; *ppv is NULL either way.
    for (const Guid& iid : {kIidAudioClock, kIidSimpleAudioVolume, kIidAudioRenderClient,
                            kIidAudioClockAdjustment}) {
        auto [hr, out] = service(raw, iid);
        EXPECT_EQ(hr, kAudclntNotInitialized);
        EXPECT_EQ(out, 0u);
    }
    for (const Guid& iid : {kIidAudioClock2, kIidAudioSessionControl2, kIidUnknown, kIidMMDevice}) {
        auto [hr, out] = service(raw, iid);
        EXPECT_EQ(hr, kENoInterface);
        EXPECT_EQ(out, 0u);
    }
    write_guid(IID_ADDR, kIidAudioClock);
    EXPECT_EQ(invoke(&ComCoreManager::audio_client_get_service, {raw, IID_ADDR, 0}), kEPointer);

    const GuestAddr client = open_stream();
    EXPECT_EQ(invoke(&ComCoreManager::audio_client_get_service, {client, IID_ADDR, 0}), kEPointer);
    const u32 base = client_refs(client);
    const GuestAddr render = must_service(client, kIidAudioRenderClient);
    EXPECT_EQ(must_service(client, kIidAudioRenderClient), render);
    {
        auto [hr, out] = service(client, kIidAudioCaptureClient);
        EXPECT_EQ(hr, kAudclntWrongEndpointType);  // Learn: capture client on a render stream
        EXPECT_EQ(out, 0u);
    }
    const GuestAddr clock = must_service(client, kIidAudioClock);
    const GuestAddr session = must_service(client, kIidAudioSessionControl);
    const GuestAddr simple = must_service(client, kIidSimpleAudioVolume);
    const GuestAddr channel = must_service(client, kIidChannelAudioVolume);
    const GuestAddr stream = must_service(client, kIidAudioStreamVolume);
    {
        auto [hr, out] = service(client, kIidAudioClockAdjustment);  // no RATEADJUST
        EXPECT_EQ(hr, kEInvalidArg);
        EXPECT_EQ(out, 0u);
    }
    for (const Guid& iid : {kIidAudioClock2, kIidAudioSessionControl2, kIidUnknown,
                            kIidAudioClient, kIidAudioSessionManager, kIidAudioSessionManager2,
                            kIidAudioClient2, kIidAudioClientDuckingControl, kIidMFTrustedOutput}) {
        auto [hr, out] = service(client, iid);
        EXPECT_EQ(hr, kENoInterface);
        EXPECT_EQ(out, 0u);
    }
    // One client reference per live object: render, clock, session (with its
    // simple and channel volume facets), stream volume.
    EXPECT_EQ(client_refs(client), base + 4u);
    // A second GetService answers the same objects (measured).
    EXPECT_EQ(must_service(client, kIidAudioClock), clock);
    EXPECT_EQ(must_service(client, kIidAudioSessionControl), session);
    EXPECT_EQ(must_service(client, kIidSimpleAudioVolume), simple);
    EXPECT_EQ(must_service(client, kIidAudioStreamVolume), stream);
    EXPECT_EQ(client_refs(client), base + 4u);

    // Measured QueryInterface graphs.
    const std::array<Guid, 13> every = {
        kIidAudioClient, kIidAudioRenderClient, kIidAudioCaptureClient, kIidAudioClock,
        kIidAudioClock2, kIidAudioClockAdjustment, kIidAudioSessionControl,
        kIidAudioSessionControl2, kIidAudioStreamVolume, kIidChannelAudioVolume,
        kIidSimpleAudioVolume, kIidAudioSessionManager, kIidAudioClient2};
    auto expect_graph = [&](GuestAddr object, std::initializer_list<std::pair<Guid, GuestAddr>> yes) {
        for (const Guid& iid : every) {
            GuestAddr expected = 0;
            for (const auto& [g, p] : yes) if (same_guid(g, iid)) expected = p;
            auto [hr, out] = qi(object, iid);
            if (expected == 0) {
                EXPECT_EQ(hr, kENoInterface);
                EXPECT_EQ(out, 0u);
            } else {
                EXPECT_EQ(hr, com::S_OK);
                EXPECT_EQ(out, expected);
            }
        }
    };
    auto [h1, clock_unknown] = qi(clock, kIidUnknown);
    ASSERT_EQ(h1, com::S_OK);
    EXPECT_NE(clock_unknown, clock);
    EXPECT_NE(clock_unknown, client);
    auto [h2, clock2] = qi(clock, kIidAudioClock2);
    ASSERT_EQ(h2, com::S_OK);
    EXPECT_NE(clock2, clock);
    EXPECT_NE(clock2, clock_unknown);
    EXPECT_EQ(qi(static_cast<GuestAddr>(clock2), kIidUnknown).second, clock_unknown);
    expect_graph(clock, {{kIidAudioClock, clock}, {kIidAudioClock2, static_cast<GuestAddr>(clock2)}});
    auto [h3, session_unknown] = qi(session, kIidUnknown);
    ASSERT_EQ(h3, com::S_OK);
    EXPECT_NE(session_unknown, session);
    EXPECT_EQ(qi(simple, kIidUnknown).second, session_unknown);
    EXPECT_EQ(qi(channel, kIidUnknown).second, session_unknown);
    expect_graph(session, {{kIidAudioSessionControl, session}, {kIidAudioSessionControl2, session},
                           {kIidChannelAudioVolume, channel}, {kIidSimpleAudioVolume, simple}});
    expect_graph(simple, {{kIidAudioSessionControl, session}, {kIidAudioSessionControl2, session},
                          {kIidChannelAudioVolume, channel}, {kIidSimpleAudioVolume, simple}});
    auto [h4, stream_unknown] = qi(stream, kIidUnknown);
    ASSERT_EQ(h4, com::S_OK);
    EXPECT_NE(stream_unknown, stream);
    EXPECT_NE(stream_unknown, session_unknown);
    expect_graph(stream, {{kIidAudioStreamVolume, stream}});
    write_guid(IID_ADDR, kIidUnknown);
    EXPECT_EQ(invoke(&ComCoreManager::audio_service_query_interface, {clock, IID_ADDR, 0}),
              kEPointer);
    // One count per object across its facets.
    const u32 before = invoke(&ComCoreManager::audio_service_add_ref, {simple});
    EXPECT_EQ(invoke(&ComCoreManager::audio_service_add_ref, {session}), before + 1u);
    EXPECT_EQ(invoke(&ComCoreManager::audio_service_release, {channel}), before);
    EXPECT_EQ(invoke(&ComCoreManager::audio_service_release, {session_unknown}), before - 1u);
}

TEST_P(WasapiServicesTest, ServicesHoldTheirClient) {
    const GuestAddr client = open_stream();
    const GuestAddr clock = must_service(client, kIidAudioClock);
    const GuestAddr simple = must_service(client, kIidSimpleAudioVolume);
    // Measured: the client's last Release leaves one count per service.
    EXPECT_EQ(invoke(&ComCoreManager::audio_client_release, {client}), 2u);
    EXPECT_EQ(invoke(&ComCoreManager::audio_clock_get_frequency, {clock, OUT}), com::S_OK);
    EXPECT_EQ(invoke(&ComCoreManager::simple_audio_volume_get_master_volume, {simple, OUT}),
              com::S_OK);
    EXPECT_EQ(invoke(&ComCoreManager::audio_service_release, {clock}), 0u);
    EXPECT_EQ(invoke(&ComCoreManager::audio_service_release, {simple}), 0u);
    // The last service took the stream with it.
    EXPECT_EQ(invoke(&ComCoreManager::audio_client_add_ref, {client}), 0u);
    EXPECT_EQ(invoke(&ComCoreManager::audio_clock_get_frequency, {clock, OUT}), kENoInterface);
}

TEST_P(WasapiServicesTest, ClockReportsPlayedStreamBytesWithQpc) {
    const GuestAddr client = open_stream();  // 16-bit stereo: 4-byte frames
    const GuestAddr clock = must_service(client, kIidAudioClock);
    EXPECT_EQ(invoke(&ComCoreManager::audio_clock_get_frequency, {clock, 0}), kEPointer);
    ASSERT_EQ(invoke(&ComCoreManager::audio_clock_get_frequency, {clock, OUT}), com::S_OK);
    EXPECT_EQ(r64(OUT), 192000u);  // measured: the stream's nAvgBytesPerSec
    EXPECT_EQ(invoke(&ComCoreManager::audio_clock_get_characteristics, {clock, 0}), kEPointer);
    ASSERT_EQ(invoke(&ComCoreManager::audio_clock_get_characteristics, {clock, OUT}), com::S_OK);
    EXPECT_EQ(memory_.read_u32(OUT).value(), 1u);  // AUDIOCLOCK_CHARACTERISTIC_FIXED_FREQ
    ASSERT_TRUE(memory_.write_u64(OUT2, 0xDEADu).is_ok());
    EXPECT_EQ(invoke(&ComCoreManager::audio_clock_get_position, {clock, 0, OUT2}), kEPointer);
    EXPECT_EQ(r64(OUT2), 0xDEADu);
    ASSERT_EQ(invoke(&ComCoreManager::audio_clock_get_position, {clock, OUT, OUT2}), com::S_OK);
    EXPECT_EQ(r64(OUT), 0u);
    EXPECT_EQ(r64(OUT2), 0u);  // measured: a new stream's QPC position is 0
    EXPECT_EQ(invoke(&ComCoreManager::audio_clock_get_position, {clock, OUT, 0}), com::S_OK);

    auto [hq, clock2] = qi(clock, kIidAudioClock2);
    ASSERT_EQ(hq, com::S_OK);
    EXPECT_EQ(invoke(&ComCoreManager::audio_clock2_get_device_position, {clock2, 0, OUT2}),
              kEPointer);
    EXPECT_EQ(invoke(&ComCoreManager::audio_clock2_get_device_position, {clock2, OUT, OUT2}),
              kAudclntPositionStalled);  // Learn: Start not yet called
    EXPECT_EQ(r64(OUT), 0u);

    submit(client, {1, 2, 3, 4, 5, 6, 7, 8});  // 4 frames
    TestAudioPushVoice* voice = voice_of(client);
    ASSERT_NE(voice, nullptr);
    ASSERT_EQ(voice->padding, 4u);
    ASSERT_EQ(invoke(&ComCoreManager::audio_clock_get_position, {clock, OUT, 0}), com::S_OK);
    EXPECT_EQ(r64(OUT), 0u);  // queued, not played
    ASSERT_EQ(invoke(&ComCoreManager::audio_client_start, {client}), com::S_OK);
    const u64 before = audio_qpc_now_hns();
    voice->padding = 1;  // the device has played 3 of the 4 frames
    ASSERT_EQ(invoke(&ComCoreManager::audio_clock_get_position, {clock, OUT, OUT2}), com::S_OK);
    EXPECT_EQ(r64(OUT), 12u);  // played frames x nBlockAlign
    EXPECT_GE(r64(OUT2), before);
    EXPECT_LE(r64(OUT2), audio_qpc_now_hns());
    EXPECT_EQ(invoke(&ComCoreManager::audio_clock2_get_device_position, {clock2, OUT, OUT2}),
              com::S_OK);
    EXPECT_EQ(r64(OUT), 3u);  // frames at the 48 kHz engine rate
    EXPECT_GE(r64(OUT2), before);
    voice->padding = 0;
    ASSERT_EQ(invoke(&ComCoreManager::audio_clock_get_position, {clock, OUT, 0}), com::S_OK);
    EXPECT_EQ(r64(OUT), 16u);  // monotonic

    // Stop freezes the position and the QPC reading (measured).
    ASSERT_EQ(invoke(&ComCoreManager::audio_client_stop, {client}), com::S_OK);
    ASSERT_EQ(invoke(&ComCoreManager::audio_clock_get_position, {clock, OUT, OUT2}), com::S_OK);
    const u64 stop_qpc = r64(OUT2);
    EXPECT_NE(stop_qpc, 0u);
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    ASSERT_EQ(invoke(&ComCoreManager::audio_clock_get_position, {clock, OUT, OUT2}), com::S_OK);
    EXPECT_EQ(r64(OUT), 16u);
    EXPECT_EQ(r64(OUT2), stop_qpc);
    // Reset: position 0 (Learn); the QPC stays the Stop reading (measured).
    ASSERT_EQ(invoke(&ComCoreManager::audio_client_reset, {client}), com::S_OK);
    ASSERT_EQ(invoke(&ComCoreManager::audio_clock_get_position, {clock, OUT, OUT2}), com::S_OK);
    EXPECT_EQ(r64(OUT), 0u);
    EXPECT_EQ(r64(OUT2), stop_qpc);
    EXPECT_EQ(invoke(&ComCoreManager::audio_clock2_get_device_position, {clock2, OUT, 0}),
              com::S_OK);
    EXPECT_EQ(r64(OUT), 0u);

    // Float mix format: 8-byte frames.
    const GuestAddr mix = open_stream(0, 0, true);
    const GuestAddr mix_clock = must_service(mix, kIidAudioClock);
    ASSERT_EQ(invoke(&ComCoreManager::audio_clock_get_frequency, {mix_clock, OUT}), com::S_OK);
    EXPECT_EQ(r64(OUT), 384000u);  // measured
    // A 44.1 kHz stream (AUTOCONVERTPCM): device frames run at 48 kHz.
    const GuestAddr cd = open_stream(kStreamFlagsAutoConvertPcm, 0, false, 44100);
    const GuestAddr cd_clock = must_service(cd, kIidAudioClock);
    ASSERT_EQ(invoke(&ComCoreManager::audio_clock_get_frequency, {cd_clock, OUT}), com::S_OK);
    EXPECT_EQ(r64(OUT), 176400u);
    auto [hc, cd_clock2] = qi(cd_clock, kIidAudioClock2);
    ASSERT_EQ(hc, com::S_OK);
    {
        const GuestAddr render = must_service(cd, kIidAudioRenderClient);
        ASSERT_EQ(invoke(&ComCoreManager::audio_render_client_get_buffer, {render, 441, OUT2}),
                  com::S_OK);
        ASSERT_EQ(invoke(&ComCoreManager::audio_render_client_release_buffer,
                         {render, 441, kBufferFlagsSilent}), com::S_OK);
    }
    renderer_.last_push->padding = 0;
    ASSERT_EQ(invoke(&ComCoreManager::audio_client_start, {cd}), com::S_OK);
    ASSERT_EQ(invoke(&ComCoreManager::audio_clock2_get_device_position, {cd_clock2, OUT, 0}),
              com::S_OK);
    EXPECT_EQ(r64(OUT), 480u);
}

TEST_P(WasapiServicesTest, ClockAdjustmentChangesThePlayRate) {
    const GuestAddr plain = open_stream();
    EXPECT_FALSE(renderer_.last_push->rate_adjust);
    const GuestAddr client = open_stream(kStreamFlagsRateAdjust);
    TestAudioPushVoice* voice = renderer_.last_push;
    EXPECT_TRUE(voice->rate_adjust);
    (void)plain;
    const GuestAddr adjust = must_service(client, kIidAudioClockAdjustment);
    EXPECT_EQ(must_service(client, kIidAudioClockAdjustment), adjust);
    auto [hu, unknown] = qi(adjust, kIidUnknown);
    ASSERT_EQ(hu, com::S_OK);
    EXPECT_NE(unknown, adjust);
    EXPECT_EQ(qi(adjust, kIidAudioClockAdjustment).second, adjust);
    EXPECT_EQ(qi(adjust, kIidAudioClock).first, kENoInterface);
    auto set_rate = [&](float rate) {
        return invoke(&ComCoreManager::audio_clock_adjustment_set_sample_rate,
                      {adjust, fbits(rate)}, 1);
    };
    ASSERT_EQ(set_rate(48480.0f), com::S_OK);
    EXPECT_NEAR(voice->rate_ratio, 1.01, 1e-9);
    for (const float bad : {0.0f, -48000.0f, std::numeric_limits<float>::quiet_NaN(),
                            std::numeric_limits<float>::infinity(), 48000.0f * 4.0f}) {
        EXPECT_EQ(set_rate(bad), kEInvalidArg) << bad;
    }
    EXPECT_NEAR(voice->rate_ratio, 1.01, 1e-9);

    // IAudioClock2 frames: 100 played at 1.0, then 125 at 1.25 -> 100 + 100.
    ASSERT_EQ(set_rate(48000.0f), com::S_OK);
    const GuestAddr clock = must_service(client, kIidAudioClock);
    auto [hq, clock2] = qi(clock, kIidAudioClock2);
    ASSERT_EQ(hq, com::S_OK);
    {
        const GuestAddr render = must_service(client, kIidAudioRenderClient);
        ASSERT_EQ(invoke(&ComCoreManager::audio_render_client_get_buffer, {render, 400, OUT2}),
                  com::S_OK);
        ASSERT_EQ(invoke(&ComCoreManager::audio_render_client_release_buffer,
                         {render, 400, kBufferFlagsSilent}), com::S_OK);
    }
    ASSERT_EQ(invoke(&ComCoreManager::audio_client_start, {client}), com::S_OK);
    voice->padding = 300;
    ASSERT_EQ(set_rate(60000.0f), com::S_OK);
    EXPECT_NEAR(voice->rate_ratio, 1.25, 1e-9);
    voice->padding = 175;
    ASSERT_EQ(invoke(&ComCoreManager::audio_clock2_get_device_position, {clock2, OUT, 0}),
              com::S_OK);
    EXPECT_EQ(r64(OUT), 200u);
    ASSERT_EQ(invoke(&ComCoreManager::audio_clock_get_position, {clock, OUT, 0}), com::S_OK);
    EXPECT_EQ(r64(OUT), 225u * 4u);  // stream bytes played
    ASSERT_EQ(invoke(&ComCoreManager::audio_clock_get_frequency, {clock, OUT}), com::S_OK);
    EXPECT_EQ(r64(OUT), 192000u);  // FIXED_FREQ
}

// Learn (GetStreamLatency): the stream's maximum latency, unchanged for the
// life of the IAudioClient. Learn (GetCurrentPadding): padding is what the
// engine has not read, and the engine reads every running shared-mode
// stream each period whether or not it is event-driven.
TEST_P(WasapiServicesTest, StreamLatencyIsConstantAndTheEngineReadsPolledStreams) {
    const GuestAddr client = open_stream();
    EXPECT_EQ(invoke(&ComCoreManager::audio_client_get_stream_latency, {client, 0}), kEPointer);
    ASSERT_EQ(invoke(&ComCoreManager::audio_client_get_stream_latency, {client, OUT}),
              com::S_OK);
    const u64 latency = r64(OUT);
    EXPECT_GE(latency, 100000u);  // at least the 10 ms default device period
    TestAudioPushVoice* voice = voice_of(client);
    ASSERT_NE(voice, nullptr);
    ASSERT_EQ(invoke(&ComCoreManager::audio_client_start, {client}), com::S_OK);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (voice->passes.load() < 3 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    EXPECT_GE(voice->passes.load(), 3u);
    ASSERT_EQ(invoke(&ComCoreManager::audio_client_get_stream_latency, {client, OUT}),
              com::S_OK);
    EXPECT_EQ(r64(OUT), latency);
    ASSERT_EQ(invoke(&ComCoreManager::audio_client_stop, {client}), com::S_OK);
    const u64 stopped = voice->passes.load();
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    EXPECT_EQ(voice->passes.load(), stopped);  // Stop: the engine no longer reads it
    ASSERT_EQ(invoke(&ComCoreManager::audio_client_get_stream_latency, {client, OUT}),
              com::S_OK);
    EXPECT_EQ(r64(OUT), latency);
}

// ActivateAudioInterfaceAsync must complete through the runtime's real
// thread start: ThreadManager::start_detached_thread creates the thread and
// hands it to run_async, which is what runs the guest thread body.
TEST_P(WasapiServicesTest, ActivateAudioInterfaceAsyncCompletesOnAStartedThread) {
    HandleTable handles;
    ThreadManager threads{handles};
    struct Run { u32 tid; GuestAddr start; GuestAddr parameter; };
    std::vector<Run> runs;
    threads.set_run_callback([&](u32 tid, GuestAddr start, GuestAddr parameter, u64, u64, usize) {
        runs.push_back({tid, start, parameter});
    });
    mgr_.set_audio_activation_thread([&](GuestAddr entry, GuestAddr parameter) {
        return threads.start_detached_thread(entry, parameter);
    });
    constexpr GuestAddr HANDLER = DATA_BASE + 0x1300, VTABLE = DATA_BASE + 0x1340;
    const GuestAddr stride = x64_ ? 8u : 4u;
    wptr(HANDLER, VTABLE);
    wptr(VTABLE + stride, 0x100);
    wptr(VTABLE + 2 * stride, 0x200);
    wptr(VTABLE + 3 * stride, 0x300);
    u32 handler_refs = 1, completions = 0, result = 0xFFFFFFFFu;
    u64 activated = 0;
    GuestAddr operation = 0;
    mgr_.set_guest_call([&](GuestAddr fn, const u64* args, usize, bool) -> u64 {
        if (fn == 0x100) return ++handler_refs;
        if (fn == 0x200) return --handler_refs;
        if (fn == 0x300) {
            ++completions;
            EXPECT_EQ(args[1], operation);
            put_args({operation, OUT2, OUT2 + 0x10}, -1);
            auto m = make_marshaler();
            EXPECT_EQ(mgr_.audio_activation_get_result(m), com::S_OK);
            result = memory_.read_u32(OUT2).value_or(1);
            activated = ptr(OUT2 + 0x10);
            return 0;
        }
        auto handler = registry_.resolve_handler(fn);  // Release of the client
        EXPECT_TRUE(handler.is_ok());
        if (handler.is_ok()) {
            put_args({args[0]}, -1);
            auto m = make_marshaler();
            (*handler.value())(m);
        }
        return cpu_.gpr.rax;
    });
    put_wide(TEXT, u"{E6327CAD-DCEC-4949-AE8A-991E976A79D2}");
    write_guid(IID_ADDR, kIidAudioClient);
    ASSERT_EQ(invoke(&ComCoreManager::activate_audio_interface_async,
                     {TEXT, IID_ADDR, 0, HANDLER, OUT}), com::S_OK);
    operation = static_cast<GuestAddr>(ptr(OUT));
    ASSERT_NE(operation, 0u);
    // The completion thread really started: created, marked launched, run.
    ASSERT_EQ(runs.size(), 1u);
    auto worker = registry_.get_thunk_address("ole32.dll", "Vitreo_AudioActivation_Worker");
    ASSERT_TRUE(worker.is_ok());
    EXPECT_EQ(runs[0].start, worker.value());
    EXPECT_EQ(runs[0].parameter, operation);
    const GuestThread* thread = threads.get_thread(runs[0].tid);
    ASSERT_NE(thread, nullptr);
    EXPECT_TRUE(thread->launched);
    EXPECT_EQ(completions, 0u);
    // The thread body, as the run callback's host thread executes it.
    auto body = registry_.resolve_handler(runs[0].start);
    ASSERT_TRUE(body.is_ok());
    put_args({runs[0].parameter}, -1);
    auto m = make_marshaler();
    (*body.value())(m);
    EXPECT_EQ(completions, 1u);
    EXPECT_EQ(result, com::S_OK);
    EXPECT_NE(activated, 0u);
    // A launcher that cannot start the thread fails the call outright.
    mgr_.set_audio_activation_thread([](GuestAddr, GuestAddr) { return false; });
    EXPECT_EQ(invoke(&ComCoreManager::activate_audio_interface_async,
                     {TEXT, IID_ADDR, 0, HANDLER, OUT}), 0x8007000Eu);
    EXPECT_EQ(ptr(OUT), 0u);
}
