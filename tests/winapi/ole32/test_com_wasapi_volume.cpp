// SPDX-License-Identifier: LGPL-2.1-only

/// @file test_com_wasapi_volume.cpp
/// @brief ISimpleAudioVolume / IChannelAudioVolume / IAudioStreamVolume tests (instantiated
/// in test_com_wasapi_services.cpp).

#include "com_test_common.hpp"
#include "com_test_wasapi_services_fixture.hpp"

TEST_P(WasapiServicesTest, SimpleVolumeIsTheProcessVolume) {
    const GuestAddr a = open_stream();
    TestAudioPushVoice* voice_a = renderer_.last_push;
    write_guid(SESSION_GUID, kPrivateSession);
    const GuestAddr p = open_stream(0, SESSION_GUID);
    TestAudioPushVoice* voice_p = renderer_.last_push;
    const GuestAddr va = must_service(a, kIidSimpleAudioVolume);
    const GuestAddr vp = must_service(p, kIidSimpleAudioVolume);
    ASSERT_EQ(invoke(&ComCoreManager::audio_session_register_notification,
                     {qi(va, kIidAudioSessionControl).second, SINK}), com::S_OK);
    auto set_master = [&](GuestAddr v, float level, GuestAddr context) {
        return invoke(&ComCoreManager::simple_audio_volume_set_master_volume,
                      {v, fbits(level), context}, 1);
    };
    auto master = [&](GuestAddr v) {
        EXPECT_EQ(invoke(&ComCoreManager::simple_audio_volume_get_master_volume, {v, OUT}), com::S_OK);
        return rf(OUT);
    };
    auto mute = [&](GuestAddr v) {
        EXPECT_EQ(invoke(&ComCoreManager::simple_audio_volume_get_mute, {v, OUT}), com::S_OK);
        return memory_.read_u32(OUT).value();
    };
    auto delivered_volume = [&](const SinkCall& call) {
        // x64: the float is in XMM1 (its RDX slot is not read); x86: the stack.
        float value = 0;
        const u32 bits = x64_ ? call.xmm1 : static_cast<u32>(call.args[1]);
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    };
    EXPECT_EQ(invoke(&ComCoreManager::simple_audio_volume_get_master_volume, {va, 0}), kRpcNullRef);
    EXPECT_EQ(invoke(&ComCoreManager::simple_audio_volume_get_mute, {va, 0}), kRpcNullRef);
    EXPECT_EQ(master(va), 1.0f);
    EXPECT_EQ(mute(va), 0u);

    ASSERT_EQ(set_master(va, 0.5f, GUID_A), com::S_OK);
    EXPECT_TRUE(calls_.empty());
    auto calls = drain();
    ASSERT_EQ(calls.size(), 1u);
    EXPECT_EQ(calls[0].slot, 5u);
    EXPECT_EQ(delivered_volume(calls[0]), 0.5f);
    EXPECT_EQ(calls[0].args[2], 0u);
    EXPECT_TRUE(same_guid(calls[0].context, kContextA));
    EXPECT_EQ(master(va), 0.5f);
    EXPECT_EQ(voice_a->gains, (std::vector<float>{0.5f, 0.5f}));
    EXPECT_EQ(voice_p->gains, (std::vector<float>{0.5f, 0.5f}));  // the process's volume
    ASSERT_EQ(set_master(va, 0.5f, GUID_B), com::S_OK);  // measured: same level still notifies
    calls = drain();
    ASSERT_EQ(calls.size(), 1u);
    EXPECT_TRUE(same_guid(calls[0].context, kContextB));
    for (const float bad : {1.1f, -0.1f, std::numeric_limits<float>::quiet_NaN()}) {
        EXPECT_EQ(set_master(va, bad, 0), kEInvalidArg);
    }
    EXPECT_EQ(master(va), 0.5f);
    EXPECT_TRUE(drain().empty());

    ASSERT_EQ(invoke(&ComCoreManager::simple_audio_volume_set_mute, {va, 1, GUID_A}), com::S_OK);
    calls = drain();
    ASSERT_EQ(calls.size(), 1u);
    EXPECT_EQ(calls[0].args[2], 1u);
    EXPECT_EQ(delivered_volume(calls[0]), 0.5f);
    EXPECT_EQ(voice_a->gains, (std::vector<float>{0.0f, 0.0f}));
    ASSERT_EQ(invoke(&ComCoreManager::simple_audio_volume_set_mute, {va, 1, GUID_A}), com::S_OK);
    EXPECT_TRUE(drain().empty());  // measured: unchanged mute is silent
    ASSERT_EQ(invoke(&ComCoreManager::simple_audio_volume_set_mute, {va, 5, GUID_B}), com::S_OK);
    calls = drain();
    ASSERT_EQ(calls.size(), 1u);
    EXPECT_EQ(calls[0].args[2], 1u);  // measured: delivered as TRUE
    EXPECT_EQ(mute(va), 5u);          // measured: read back as stored
    ASSERT_EQ(invoke(&ComCoreManager::simple_audio_volume_set_mute, {va, 0, GUID_B}), com::S_OK);
    EXPECT_EQ(drain().size(), 1u);
    EXPECT_EQ(voice_a->gains, (std::vector<float>{0.5f, 0.5f}));

    // Through another session of the process (measured): a real change
    // reaches this session's sink with the system's own context.
    ASSERT_EQ(set_master(vp, 0.25f, GUID_B), com::S_OK);
    calls = drain();
    ASSERT_EQ(calls.size(), 1u);
    EXPECT_EQ(calls[0].sink, SINK);
    EXPECT_EQ(delivered_volume(calls[0]), 0.25f);
    EXPECT_FALSE(calls[0].context_null);
    EXPECT_FALSE(same_guid(calls[0].context, kContextA));
    EXPECT_FALSE(same_guid(calls[0].context, kContextB));
    EXPECT_FALSE(same_guid(calls[0].context, Guid{}));
    EXPECT_EQ(master(va), 0.25f);
    ASSERT_EQ(set_master(vp, 0.25f, GUID_B), com::S_OK);
    EXPECT_TRUE(drain().empty());
    ASSERT_EQ(invoke(&ComCoreManager::simple_audio_volume_set_mute, {vp, 1, GUID_B}), com::S_OK);
    EXPECT_TRUE(drain().empty());  // measured: mute is shared but not announced
    EXPECT_EQ(mute(va), 1u);
    EXPECT_EQ(voice_a->gains, (std::vector<float>{0.0f, 0.0f}));
}

TEST_P(WasapiServicesTest, ChannelVolumeIsPerChannelProcessVolume) {
    const GuestAddr a = open_stream();
    TestAudioPushVoice* voice_a = renderer_.last_push;
    write_guid(SESSION_GUID, kPrivateSession);
    const GuestAddr p = open_stream(0, SESSION_GUID);
    const GuestAddr ca = must_service(a, kIidChannelAudioVolume);
    const GuestAddr cp = must_service(p, kIidChannelAudioVolume);
    ASSERT_EQ(invoke(&ComCoreManager::audio_session_register_notification,
                     {qi(ca, kIidAudioSessionControl).second, SINK}), com::S_OK);
    auto set_channel = [&](GuestAddr c, u32 index, float level, GuestAddr context) {
        return invoke(&ComCoreManager::channel_audio_volume_set_channel_volume,
                      {c, index, fbits(level), context}, 2);
    };
    EXPECT_EQ(invoke(&ComCoreManager::channel_audio_volume_get_channel_count, {ca, 0}), kRpcNullRef);
    ASSERT_EQ(invoke(&ComCoreManager::channel_audio_volume_get_channel_count, {ca, OUT}), com::S_OK);
    EXPECT_EQ(memory_.read_u32(OUT).value(), 2u);
    EXPECT_EQ(invoke(&ComCoreManager::channel_audio_volume_get_channel_volume, {ca, 0, 0}),
              kRpcNullRef);
    ASSERT_EQ(invoke(&ComCoreManager::channel_audio_volume_get_channel_volume, {ca, 0, OUT}),
              com::S_OK);
    EXPECT_EQ(rf(OUT), 1.0f);
    wf(OUT, -7.0f);
    EXPECT_EQ(invoke(&ComCoreManager::channel_audio_volume_get_channel_volume, {ca, 2, OUT}),
              kEInvalidArg);
    EXPECT_EQ(memory_.read_u32(OUT).value(), 0u);  // measured: zeroed

    ASSERT_EQ(set_channel(ca, 0, 0.25f, GUID_A), com::S_OK);
    EXPECT_TRUE(calls_.empty());
    auto calls = drain();
    ASSERT_EQ(calls.size(), 1u);
    EXPECT_EQ(calls[0].slot, 6u);
    EXPECT_EQ(calls[0].args[1], 2u);
    EXPECT_EQ(calls[0].volumes, (std::vector<float>{0.25f, 1.0f}));
    EXPECT_EQ(calls[0].args[3], 0u);
    EXPECT_TRUE(same_guid(calls[0].context, kContextA));
    ASSERT_EQ(set_channel(ca, 0, 0.25f, GUID_B), com::S_OK);  // measured: notifies again
    calls = drain();
    ASSERT_EQ(calls.size(), 1u);
    EXPECT_TRUE(same_guid(calls[0].context, kContextB));
    EXPECT_EQ(set_channel(ca, 2, 0.5f, 0), kEInvalidArg);
    EXPECT_EQ(set_channel(ca, 0, 1.1f, 0), kEInvalidArg);
    EXPECT_EQ(set_channel(ca, 0, -0.1f, 0), kEInvalidArg);
    EXPECT_TRUE(drain().empty());

    wf(ARRAY, 0.1f);
    wf(ARRAY + 4, 0.2f);
    ASSERT_EQ(invoke(&ComCoreManager::channel_audio_volume_set_all_volumes, {ca, 2, ARRAY, GUID_B}),
              com::S_OK);
    calls = drain();
    ASSERT_EQ(calls.size(), 1u);
    EXPECT_EQ(calls[0].args[3], 0xFFFFFFFFu);  // Learn: (DWORD)-1, several channels
    EXPECT_EQ(calls[0].volumes, (std::vector<float>{0.1f, 0.2f}));
    for (const u32 count : {3u, 1u, 0u}) {
        EXPECT_EQ(invoke(&ComCoreManager::channel_audio_volume_set_all_volumes,
                         {ca, count, ARRAY, 0}), kEInvalidArg);
    }
    EXPECT_EQ(invoke(&ComCoreManager::channel_audio_volume_set_all_volumes, {ca, 2, 0, 0}),
              kRpcNullRef);
    wf(ARRAY + 4, 1.5f);
    EXPECT_EQ(invoke(&ComCoreManager::channel_audio_volume_set_all_volumes, {ca, 2, ARRAY, 0}),
              kEInvalidArg);
    EXPECT_TRUE(drain().empty());
    ASSERT_EQ(invoke(&ComCoreManager::channel_audio_volume_get_all_volumes, {ca, 2, OUT}), com::S_OK);
    EXPECT_EQ(rf(OUT), 0.1f);
    EXPECT_EQ(rf(OUT + 4), 0.2f);
    for (u32 i = 0; i < 3; ++i) wf(OUT + i * 4, -7.0f);
    EXPECT_EQ(invoke(&ComCoreManager::channel_audio_volume_get_all_volumes, {ca, 3, OUT}),
              kEInvalidArg);
    for (u32 i = 0; i < 3; ++i) EXPECT_EQ(memory_.read_u32(OUT + i * 4).value(), 0u);
    for (u32 i = 0; i < 2; ++i) wf(OUT + i * 4, -7.0f);
    EXPECT_EQ(invoke(&ComCoreManager::channel_audio_volume_get_all_volumes, {ca, 1, OUT}),
              kEInvalidArg);
    EXPECT_EQ(memory_.read_u32(OUT).value(), 0u);
    EXPECT_EQ(rf(OUT + 4), -7.0f);
    EXPECT_EQ(invoke(&ComCoreManager::channel_audio_volume_get_all_volumes, {ca, 2, 0}), kRpcNullRef);
    EXPECT_EQ(voice_a->gains, (std::vector<float>{0.1f, 0.2f}));

    // Set through another session of the process (measured, supplement).
    ASSERT_EQ(set_channel(cp, 0, 0.5f, GUID_B), com::S_OK);
    calls = drain();
    ASSERT_EQ(calls.size(), 1u);
    EXPECT_EQ(calls[0].sink, SINK);
    EXPECT_EQ(calls[0].args[3], 0u);
    EXPECT_EQ(calls[0].volumes, (std::vector<float>{0.5f, 0.2f}));
    EXPECT_FALSE(same_guid(calls[0].context, kContextB));
    EXPECT_FALSE(same_guid(calls[0].context, Guid{}));
    ASSERT_EQ(invoke(&ComCoreManager::channel_audio_volume_get_channel_volume, {ca, 0, OUT}),
              com::S_OK);
    EXPECT_EQ(rf(OUT), 0.5f);
}

TEST_P(WasapiServicesTest, StreamVolumeIsPerStreamAndSilent) {
    const GuestAddr a = open_stream();
    TestAudioPushVoice* voice = renderer_.last_push;
    const GuestAddr sv = must_service(a, kIidAudioStreamVolume);
    ASSERT_EQ(invoke(&ComCoreManager::audio_session_register_notification,
                     {must_service(a, kIidAudioSessionControl), SINK}), com::S_OK);
    auto set_stream = [&](u32 index, float level) {
        return invoke(&ComCoreManager::stream_audio_volume_set_channel_volume,
                      {sv, index, fbits(level)}, 2);
    };
    EXPECT_EQ(invoke(&ComCoreManager::stream_audio_volume_get_channel_count, {sv, 0}), kEPointer);
    ASSERT_EQ(invoke(&ComCoreManager::stream_audio_volume_get_channel_count, {sv, OUT}), com::S_OK);
    EXPECT_EQ(memory_.read_u32(OUT).value(), 2u);
    EXPECT_EQ(invoke(&ComCoreManager::stream_audio_volume_get_channel_volume, {sv, 0, 0}), kEPointer);
    wf(OUT, -7.0f);
    EXPECT_EQ(invoke(&ComCoreManager::stream_audio_volume_get_channel_volume, {sv, 2, OUT}),
              kEInvalidArg);
    EXPECT_EQ(memory_.read_u32(OUT).value(), 0u);
    ASSERT_EQ(set_stream(0, 0.25f), com::S_OK);
    ASSERT_EQ(invoke(&ComCoreManager::stream_audio_volume_get_channel_volume, {sv, 0, OUT}),
              com::S_OK);
    EXPECT_EQ(rf(OUT), 0.25f);
    EXPECT_TRUE(launches_.empty());  // measured: no session events
    EXPECT_EQ(set_stream(2, 0.5f), kEInvalidArg);
    EXPECT_EQ(set_stream(0, 1.1f), kEInvalidArg);
    EXPECT_EQ(set_stream(0, -0.1f), kEInvalidArg);
    wf(ARRAY, 0.1f);
    wf(ARRAY + 4, 0.2f);
    ASSERT_EQ(invoke(&ComCoreManager::stream_audio_volume_set_all_volumes, {sv, 2, ARRAY}), com::S_OK);
    for (const u32 count : {3u, 1u, 0u}) {
        EXPECT_EQ(invoke(&ComCoreManager::stream_audio_volume_set_all_volumes, {sv, count, ARRAY}),
                  kEInvalidArg);
    }
    EXPECT_EQ(invoke(&ComCoreManager::stream_audio_volume_set_all_volumes, {sv, 2, 0}), kEPointer);
    wf(ARRAY, 1.5f);
    EXPECT_EQ(invoke(&ComCoreManager::stream_audio_volume_set_all_volumes, {sv, 2, ARRAY}),
              kEInvalidArg);
    ASSERT_EQ(invoke(&ComCoreManager::stream_audio_volume_get_all_volumes, {sv, 2, OUT}), com::S_OK);
    EXPECT_EQ(rf(OUT), 0.1f);
    EXPECT_EQ(rf(OUT + 4), 0.2f);
    for (u32 i = 0; i < 3; ++i) wf(OUT + i * 4, -7.0f);
    EXPECT_EQ(invoke(&ComCoreManager::stream_audio_volume_get_all_volumes, {sv, 3, OUT}),
              kEInvalidArg);
    for (u32 i = 0; i < 3; ++i) EXPECT_EQ(memory_.read_u32(OUT + i * 4).value(), 0u);
    wf(OUT + 4, -7.0f);
    EXPECT_EQ(invoke(&ComCoreManager::stream_audio_volume_get_all_volumes, {sv, 1, OUT}),
              kEInvalidArg);
    EXPECT_EQ(rf(OUT + 4), -7.0f);
    EXPECT_EQ(invoke(&ComCoreManager::stream_audio_volume_get_all_volumes, {sv, 2, 0}), kEPointer);
    EXPECT_TRUE(drain().empty());

    // Independent of the session channel volume (measured); the device gain
    // is their product with the master volume.
    const GuestAddr channel = must_service(a, kIidChannelAudioVolume);
    ASSERT_EQ(invoke(&ComCoreManager::channel_audio_volume_get_channel_volume, {channel, 0, OUT}),
              com::S_OK);
    EXPECT_EQ(rf(OUT), 1.0f);
    ASSERT_EQ(invoke(&ComCoreManager::channel_audio_volume_set_channel_volume,
                     {channel, 0, fbits(0.5f), 0}, 2), com::S_OK);
    ASSERT_EQ(invoke(&ComCoreManager::simple_audio_volume_set_master_volume,
                     {must_service(a, kIidSimpleAudioVolume), fbits(0.5f), 0}, 1), com::S_OK);
    EXPECT_EQ(voice->gains, (std::vector<float>{0.1f * 0.5f * 0.5f, 0.2f * 0.5f}));
    ASSERT_EQ(invoke(&ComCoreManager::stream_audio_volume_get_channel_volume, {sv, 0, OUT}),
              com::S_OK);
    EXPECT_EQ(rf(OUT), 0.1f);
    (void)drain();
}

TEST_P(WasapiServicesTest, VolumeScalesTheRenderedSamples) {
    const GuestAddr a = open_stream();  // 16-bit stereo
    TestAudioPushVoice* voice = renderer_.last_push;
    submit(a, {1000, -2000, 3000, -4000});
    auto rendered = [&] {
        const auto bytes = voice->rendered();
        std::vector<i16> samples(bytes.size() / 2);
        std::memcpy(samples.data(), bytes.data(), bytes.size());
        return samples;
    };
    EXPECT_EQ(rendered(), (std::vector<i16>{1000, -2000, 3000, -4000}));
    const GuestAddr sv = must_service(a, kIidAudioStreamVolume);
    ASSERT_EQ(invoke(&ComCoreManager::stream_audio_volume_set_channel_volume,
                     {sv, 0, fbits(0.5f)}, 2), com::S_OK);
    const GuestAddr simple = must_service(a, kIidSimpleAudioVolume);
    ASSERT_EQ(invoke(&ComCoreManager::simple_audio_volume_set_master_volume,
                     {simple, fbits(0.5f), 0}, 1), com::S_OK);
    // Applied to frames already queued, as the device takes them.
    EXPECT_EQ(rendered(), (std::vector<i16>{250, -1000, 750, -2000}));
    ASSERT_EQ(invoke(&ComCoreManager::simple_audio_volume_set_mute, {simple, 1, 0}), com::S_OK);
    EXPECT_EQ(rendered(), (std::vector<i16>{0, 0, 0, 0}));
    (void)drain();
}
