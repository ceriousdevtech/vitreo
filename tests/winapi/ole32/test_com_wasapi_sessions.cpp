// SPDX-License-Identifier: LGPL-2.1-only

/// @file test_com_wasapi_sessions.cpp
/// @brief IAudioSessionControl / IAudioSessionControl2 tests (instantiated in
/// test_com_wasapi_services.cpp).

#include "com_test_common.hpp"
#include "com_test_wasapi_services_fixture.hpp"

TEST_P(WasapiServicesTest, SessionControlStateTextGroupingAndEvents) {
    const GuestAddr client = open_stream();
    const GuestAddr session = must_service(client, kIidAudioSessionControl);
    EXPECT_EQ(invoke(&ComCoreManager::audio_session_get_state, {session, 0}), kRpcNullRef);
    ASSERT_EQ(invoke(&ComCoreManager::audio_session_get_state, {session, OUT}), com::S_OK);
    EXPECT_EQ(memory_.read_u32(OUT).value(), 0u);
    EXPECT_EQ(invoke(&ComCoreManager::audio_session_get_display_name, {session, 0}), kEPointer);
    ASSERT_EQ(invoke(&ComCoreManager::audio_session_get_display_name, {session, OUT}), com::S_OK);
    ASSERT_NE(ptr(OUT), 0u);  // measured: an empty string, not NULL
    EXPECT_EQ(wstr(static_cast<GuestAddr>(ptr(OUT))), u"");
    ASSERT_EQ(invoke(&ComCoreManager::audio_session_get_icon_path, {session, OUT}), com::S_OK);
    EXPECT_EQ(wstr(static_cast<GuestAddr>(ptr(OUT))), u"");
    EXPECT_EQ(invoke(&ComCoreManager::audio_session_get_grouping_param, {session, 0}), kRpcNullRef);
    ASSERT_EQ(invoke(&ComCoreManager::audio_session_get_grouping_param, {session, OUT}), com::S_OK);
    EXPECT_FALSE(same_guid(read_guid(OUT), Guid{}));  // measured: a fresh GUID
    EXPECT_EQ(invoke(&ComCoreManager::audio_session_register_notification, {session, 0}), kEPointer);
    EXPECT_EQ(invoke(&ComCoreManager::audio_session_unregister_notification, {session, 0}), kEPointer);
    ASSERT_EQ(invoke(&ComCoreManager::audio_session_register_notification, {session, SINK}),
              com::S_OK);
    ASSERT_EQ(invoke(&ComCoreManager::audio_session_register_notification, {session, SINK}),
              com::S_OK);
    EXPECT_EQ(sink_refs_[SINK], 2u);  // Learn: AddRef per registration
    EXPECT_TRUE(drain().empty());

    put_wide(TEXT, u"Probe Name");
    ASSERT_EQ(invoke(&ComCoreManager::audio_session_set_display_name, {session, TEXT, GUID_A}),
              com::S_OK);
    EXPECT_TRUE(calls_.empty());  // delivered on the notification thread
    auto calls = drain();
    ASSERT_EQ(calls.size(), 2u);  // registered twice: called twice
    for (const auto& call : calls) {
        EXPECT_EQ(call.sink, SINK);
        EXPECT_EQ(call.slot, 3u);  // OnDisplayNameChanged
        EXPECT_EQ(call.text, u"Probe Name");
        EXPECT_FALSE(call.context_null);
        EXPECT_TRUE(same_guid(call.context, kContextA));
    }
    ASSERT_EQ(invoke(&ComCoreManager::audio_session_set_display_name, {session, TEXT, GUID_A}),
              com::S_OK);
    EXPECT_TRUE(launches_.empty());  // measured: the same name again is silent
    EXPECT_TRUE(drain().empty());
    EXPECT_EQ(invoke(&ComCoreManager::audio_session_set_display_name, {session, 0, GUID_A}),
              kRpcNullRef);
    EXPECT_TRUE(drain().empty());
    ASSERT_EQ(invoke(&ComCoreManager::audio_session_get_display_name, {session, OUT}), com::S_OK);
    EXPECT_EQ(wstr(static_cast<GuestAddr>(ptr(OUT))), u"Probe Name");
    put_wide(TEXT, u"");
    ASSERT_EQ(invoke(&ComCoreManager::audio_session_set_display_name, {session, TEXT, 0}), com::S_OK);
    calls = drain();
    ASSERT_EQ(calls.size(), 2u);
    EXPECT_FALSE(calls[0].context_null);  // measured: NULL arrives as &GUID_NULL
    EXPECT_TRUE(same_guid(calls[0].context, Guid{}));

    put_wide(TEXT, u"C:\\probe.dll,-1");
    ASSERT_EQ(invoke(&ComCoreManager::audio_session_set_icon_path, {session, TEXT, GUID_B}),
              com::S_OK);
    calls = drain();
    ASSERT_EQ(calls.size(), 2u);
    EXPECT_EQ(calls[0].slot, 4u);
    EXPECT_EQ(calls[0].text, u"C:\\probe.dll,-1");
    EXPECT_TRUE(same_guid(calls[0].context, kContextB));
    ASSERT_EQ(invoke(&ComCoreManager::audio_session_set_icon_path, {session, TEXT, GUID_A}),
              com::S_OK);
    EXPECT_TRUE(drain().empty());  // measured (supplement)
    EXPECT_EQ(invoke(&ComCoreManager::audio_session_set_icon_path, {session, 0, 0}), kRpcNullRef);
    ASSERT_EQ(invoke(&ComCoreManager::audio_session_get_icon_path, {session, OUT}), com::S_OK);
    EXPECT_EQ(wstr(static_cast<GuestAddr>(ptr(OUT))), u"C:\\probe.dll,-1");

    EXPECT_EQ(invoke(&ComCoreManager::audio_session_set_grouping_param, {session, 0, GUID_A}),
              kRpcNullRef);
    write_guid(PARAM_GUID, kContextB);
    ASSERT_EQ(invoke(&ComCoreManager::audio_session_set_grouping_param,
                     {session, PARAM_GUID, GUID_A}), com::S_OK);
    calls = drain();
    ASSERT_EQ(calls.size(), 2u);
    EXPECT_EQ(calls[0].slot, 7u);
    EXPECT_TRUE(same_guid(calls[0].param, kContextB));
    EXPECT_TRUE(same_guid(calls[0].context, kContextA));
    ASSERT_EQ(invoke(&ComCoreManager::audio_session_set_grouping_param,
                     {session, PARAM_GUID, GUID_B}), com::S_OK);
    EXPECT_TRUE(drain().empty());  // measured (supplement)
    ASSERT_EQ(invoke(&ComCoreManager::audio_session_get_grouping_param, {session, OUT}), com::S_OK);
    EXPECT_TRUE(same_guid(read_guid(OUT), kContextB));

    // The session is active while a stream runs (measured).
    ASSERT_EQ(invoke(&ComCoreManager::audio_client_start, {client}), com::S_OK);
    calls = drain();
    ASSERT_EQ(calls.size(), 2u);
    EXPECT_EQ(calls[0].slot, 8u);
    EXPECT_EQ(calls[0].args[1], 1u);  // AudioSessionStateActive
    ASSERT_EQ(invoke(&ComCoreManager::audio_session_get_state, {session, OUT}), com::S_OK);
    EXPECT_EQ(memory_.read_u32(OUT).value(), 1u);
    ASSERT_EQ(invoke(&ComCoreManager::audio_client_stop, {client}), com::S_OK);
    calls = drain();
    ASSERT_EQ(calls.size(), 2u);
    EXPECT_EQ(calls[0].args[1], 0u);
    ASSERT_EQ(invoke(&ComCoreManager::audio_session_get_state, {session, OUT}), com::S_OK);
    EXPECT_EQ(memory_.read_u32(OUT).value(), 0u);

    // One registration per Unregister, each Released (measured).
    ASSERT_EQ(invoke(&ComCoreManager::audio_session_unregister_notification, {session, SINK}),
              com::S_OK);
    EXPECT_EQ(sink_refs_[SINK], 1u);
    ASSERT_EQ(invoke(&ComCoreManager::audio_session_unregister_notification, {session, SINK}),
              com::S_OK);
    EXPECT_EQ(sink_refs_[SINK], 0u);
    EXPECT_EQ(invoke(&ComCoreManager::audio_session_unregister_notification, {session, SINK}),
              kNotFound);
    put_wide(TEXT, u"after");
    ASSERT_EQ(invoke(&ComCoreManager::audio_session_set_display_name, {session, TEXT, 0}), com::S_OK);
    EXPECT_TRUE(drain().empty());
}

TEST_P(WasapiServicesTest, SessionControl2IdentifiersAndProcess) {
    const GuestAddr client = open_stream();
    const GuestAddr session = must_service(client, kIidAudioSessionControl);
    EXPECT_EQ(qi(session, kIidAudioSessionControl2).second, session);
    const std::u16string id =
        u"Vitreo.CoreAudio.Render|\\Device\\HarddiskVolume1\\Games\\probe.exe"
        u"%b{00000000-0000-0000-0000-000000000000}";
    EXPECT_EQ(invoke(&ComCoreManager::audio_session_get_session_identifier, {session, 0}), kEPointer);
    ASSERT_EQ(invoke(&ComCoreManager::audio_session_get_session_identifier, {session, OUT}),
              com::S_OK);
    EXPECT_EQ(wstr(static_cast<GuestAddr>(ptr(OUT))), id);
    std::u16string pid;
    for (const char c : std::to_string(static_cast<u32>(::getpid()))) pid.push_back(c);
    EXPECT_EQ(invoke(&ComCoreManager::audio_session_get_session_instance_identifier, {session, 0}),
              kEPointer);
    ASSERT_EQ(invoke(&ComCoreManager::audio_session_get_session_instance_identifier, {session, OUT}),
              com::S_OK);
    EXPECT_EQ(wstr(static_cast<GuestAddr>(ptr(OUT))), id + u"|0%b" + pid);
    EXPECT_EQ(invoke(&ComCoreManager::audio_session_get_process_id, {session, 0}), kEPointer);
    ASSERT_EQ(invoke(&ComCoreManager::audio_session_get_process_id, {session, OUT}), com::S_OK);
    EXPECT_EQ(memory_.read_u32(OUT).value(), static_cast<u32>(::getpid()));
    EXPECT_EQ(invoke(&ComCoreManager::audio_session_is_system_sounds_session, {session}),
              com::S_FALSE);
    EXPECT_EQ(invoke(&ComCoreManager::audio_session_set_ducking_preference, {session, 1}), com::S_OK);
    EXPECT_EQ(invoke(&ComCoreManager::audio_session_set_ducking_preference, {session, 0}), com::S_OK);

    // Cross-process sessions have no process in their identifiers (measured).
    write_guid(SESSION_GUID, kPrivateSession);
    const GuestAddr cross = open_stream(kStreamFlagsCrossProcess, SESSION_GUID);
    const GuestAddr cross_session = must_service(cross, kIidAudioSessionControl);
    const std::u16string cross_id =
        u"Vitreo.CoreAudio.Render|#%b{5E551012-1234-4ABC-8001-020304050608}";
    ASSERT_EQ(invoke(&ComCoreManager::audio_session_get_session_identifier, {cross_session, OUT}),
              com::S_OK);
    EXPECT_EQ(wstr(static_cast<GuestAddr>(ptr(OUT))), cross_id);
    ASSERT_EQ(invoke(&ComCoreManager::audio_session_get_session_instance_identifier,
                     {cross_session, OUT}), com::S_OK);
    EXPECT_EQ(wstr(static_cast<GuestAddr>(ptr(OUT))), cross_id + u"|0%b#");
    EXPECT_EQ(invoke(&ComCoreManager::audio_session_get_process_id, {cross_session, OUT}),
              kAudclntNoSingleProcess);
    EXPECT_EQ(memory_.read_u32(OUT).value(), static_cast<u32>(::getpid()));
    // The same GUID without CROSSPROCESS is another session.
    const GuestAddr local = open_stream(0, SESSION_GUID);
    const GuestAddr local_session = must_service(local, kIidAudioSessionControl);
    ASSERT_EQ(invoke(&ComCoreManager::audio_session_get_session_identifier, {local_session, OUT}),
              com::S_OK);
    EXPECT_EQ(wstr(static_cast<GuestAddr>(ptr(OUT))),
              u"Vitreo.CoreAudio.Render|\\Device\\HarddiskVolume1\\Games\\probe.exe"
              u"%b{5E551012-1234-4ABC-8001-020304050608}");
}

TEST_P(WasapiServicesTest, SessionsAreSharedByGuidAndEndWithTheirLastStream) {
    write_guid(SESSION_GUID, Guid{});
    const GuestAddr a = open_stream(0, 0);
    const GuestAddr b = open_stream(0, SESSION_GUID);  // GUID_NULL: same as NULL
    write_guid(PARAM_GUID, kPrivateSession);
    const GuestAddr p = open_stream(0, PARAM_GUID);
    const GuestAddr xa = must_service(a, kIidAudioSessionControl);
    const GuestAddr xb = must_service(b, kIidAudioSessionControl);
    const GuestAddr xp = must_service(p, kIidAudioSessionControl);
    EXPECT_NE(xa, xb);  // measured: one control object per stream
    put_wide(TEXT, u"Shared");
    ASSERT_EQ(invoke(&ComCoreManager::audio_session_set_display_name, {xa, TEXT, 0}), com::S_OK);
    ASSERT_EQ(invoke(&ComCoreManager::audio_session_get_display_name, {xb, OUT}), com::S_OK);
    EXPECT_EQ(wstr(static_cast<GuestAddr>(ptr(OUT))), u"Shared");
    ASSERT_EQ(invoke(&ComCoreManager::audio_session_get_display_name, {xp, OUT}), com::S_OK);
    EXPECT_EQ(wstr(static_cast<GuestAddr>(ptr(OUT))), u"");
    auto state = [&](GuestAddr control) {
        EXPECT_EQ(invoke(&ComCoreManager::audio_session_get_state, {control, OUT}), com::S_OK);
        return memory_.read_u32(OUT).value();
    };
    ASSERT_EQ(invoke(&ComCoreManager::audio_client_start, {a}), com::S_OK);
    ASSERT_EQ(invoke(&ComCoreManager::audio_client_start, {b}), com::S_OK);
    ASSERT_EQ(invoke(&ComCoreManager::audio_client_stop, {a}), com::S_OK);
    EXPECT_EQ(state(xa), 1u);  // measured: one of two still running
    ASSERT_EQ(invoke(&ComCoreManager::audio_client_stop, {b}), com::S_OK);
    EXPECT_EQ(state(xa), 0u);
    EXPECT_EQ(state(xp), 0u);
    (void)drain();

    // A running stream torn down with a sink registered: the sink is Released
    // with the session, and hears nothing (measured, supplement).
    ASSERT_EQ(invoke(&ComCoreManager::audio_session_register_notification, {xp, SINK2}), com::S_OK);
    ASSERT_EQ(invoke(&ComCoreManager::audio_client_start, {p}), com::S_OK);
    auto calls = drain();
    ASSERT_EQ(calls.size(), 1u);
    EXPECT_EQ(calls[0].slot, 8u);
    EXPECT_EQ(sink_refs_[SINK2], 1u);
    EXPECT_EQ(invoke(&ComCoreManager::audio_service_release, {xp}), 0u);
    EXPECT_EQ(invoke(&ComCoreManager::audio_client_release, {p}), 0u);
    EXPECT_EQ(sink_refs_[SINK2], 0u);
    EXPECT_TRUE(drain().empty());
}
