// SPDX-License-Identifier: LGPL-2.1-only

/// @file test_com_filter_graph_events.cpp
/// @brief DirectShow filter graph completion and event tests.

#include "com_test_common.hpp"
#include "com_test_filter_graph_fixture.hpp"

TEST_F(FilterGraphTest, WaitForCompletionDoesNotCompleteImmediatelyWithAudio) {
    // WMV intros have an audio stream. A voice that is not yet playing is
    // not EC_COMPLETE; the video clock must run for the probed duration.
    char temp_template[] = "/tmp/vitreo_avcomplete_test_XXXXXX";
    char* temp_dir = ::mkdtemp(temp_template);
    ASSERT_NE(temp_dir, nullptr);
    const std::filesystem::path root = temp_dir;
    const std::filesystem::path host_clip = root / "clip.mp4";
    const std::string command =
        "ffmpeg -y -hide_banner -loglevel error -f lavfi "
        "-i color=c=red:s=16x16:d=0.4:r=10 -f lavfi "
        "-i sine=frequency=440:duration=0.4 -pix_fmt yuv420p "
        "-shortest \"" +
        host_clip.string() + "\"";
    if (std::system(command.c_str()) != 0) {
        std::filesystem::remove_all(root);
        GTEST_SKIP() << "ffmpeg could not write a test A/V clip";
    }

    capsule::Vfs vfs;
    ASSERT_TRUE(vfs.mount_drive(
        'C', std::make_unique<capsule::HostFileSystem>(root.string())).is_ok());
    mgr_.set_vfs(&vfs);

    write_guid(CLSID_ADDR, CLSID_FILTERGRAPH);
    write_guid(IID_ADDR, IID_IGRAPHBUILDER);
    set_args({CLSID_ADDR, 0, 1, IID_ADDR, OUT_ADDR});
    auto create_marshaler = make_marshaler();
    ASSERT_EQ(mgr_.co_create_instance(create_marshaler), com::S_OK);
    const u32 graph = read_u32(OUT_ADDR);

    static constexpr GuestAddr PATH_ADDR = DATA_BASE + 0x200;
    static constexpr GuestAddr EV_CODE_ADDR = DATA_BASE + 0x280;
    auto path_marshaler = make_marshaler();
    ASSERT_TRUE(
        path_marshaler.write_wide_string(PATH_ADDR, "C:\\clip.mp4").is_ok());
    set_args({graph, PATH_ADDR, 0});
    auto render_marshaler = make_marshaler();
    ASSERT_EQ(mgr_.filter_graph_render_file(render_marshaler), com::S_OK);

    write_guid(QI_IID_ADDR, IID_IMEDIACONTROL);
    set_args({graph, QI_IID_ADDR, QI_OUT_ADDR});
    auto control_qi = make_marshaler();
    ASSERT_EQ(mgr_.filter_graph_query_interface(control_qi), com::S_OK);
    const u32 media = read_u32(QI_OUT_ADDR);

    write_guid(QI_IID_ADDR, IID_IMEDIAEVENT);
    set_args({graph, QI_IID_ADDR, QI_OUT_ADDR});
    auto event_qi = make_marshaler();
    ASSERT_EQ(mgr_.filter_graph_query_interface(event_qi), com::S_OK);
    const u32 media_event = read_u32(QI_OUT_ADDR);

    set_args({media});
    auto run_marshaler = make_marshaler();
    ASSERT_EQ(mgr_.filter_graph_media_control_run(run_marshaler), com::S_OK);

    set_args({media_event, 50, EV_CODE_ADDR});
    auto poll_marshaler = make_marshaler();
    EXPECT_EQ(mgr_.filter_graph_media_event_wait_for_completion(poll_marshaler),
              0x80004004u);

    set_args({media_event, 2000, EV_CODE_ADDR});
    auto wait_marshaler = make_marshaler();
    EXPECT_EQ(mgr_.filter_graph_media_event_wait_for_completion(wait_marshaler),
              com::S_OK);
    EXPECT_EQ(read_u32(EV_CODE_ADDR), 0x01u);

    std::filesystem::remove_all(root);
}

TEST_F(FilterGraphTest, WaitForCompletionDeliversEcCompleteForVideoOnly) {
    // Learn IMediaEvent::WaitForCompletion: running graph, timeout 0 is a
    // poll (E_ABORT until EC_COMPLETE). Video-only clips have no audio voice;
    // completion must follow the video clock, not a missing renderer.
    char temp_template[] = "/tmp/vitreo_waitcomplete_test_XXXXXX";
    char* temp_dir = ::mkdtemp(temp_template);
    ASSERT_NE(temp_dir, nullptr);
    const std::filesystem::path root = temp_dir;
    const std::filesystem::path host_clip = root / "clip.mp4";
    const std::string command =
        "ffmpeg -y -hide_banner -loglevel error -f lavfi "
        "-i color=c=red:s=16x16:d=0.4:r=10 -an -pix_fmt yuv420p \"" +
        host_clip.string() + "\"";
    if (std::system(command.c_str()) != 0) {
        std::filesystem::remove_all(root);
        GTEST_SKIP() << "ffmpeg could not write a test video";
    }

    capsule::Vfs vfs;
    ASSERT_TRUE(vfs.mount_drive(
        'C', std::make_unique<capsule::HostFileSystem>(root.string())).is_ok());
    mgr_.set_vfs(&vfs);

    write_guid(CLSID_ADDR, CLSID_FILTERGRAPH);
    write_guid(IID_ADDR, IID_IGRAPHBUILDER);
    set_args({CLSID_ADDR, 0, 1, IID_ADDR, OUT_ADDR});
    auto create_marshaler = make_marshaler();
    ASSERT_EQ(mgr_.co_create_instance(create_marshaler), com::S_OK);
    const u32 graph = read_u32(OUT_ADDR);

    static constexpr GuestAddr PATH_ADDR = DATA_BASE + 0x200;
    static constexpr GuestAddr EV_CODE_ADDR = DATA_BASE + 0x280;
    auto path_marshaler = make_marshaler();
    ASSERT_TRUE(
        path_marshaler.write_wide_string(PATH_ADDR, "C:\\clip.mp4").is_ok());
    set_args({graph, PATH_ADDR, 0});
    auto render_marshaler = make_marshaler();
    ASSERT_EQ(mgr_.filter_graph_render_file(render_marshaler), com::S_OK);

    write_guid(QI_IID_ADDR, IID_IMEDIACONTROL);
    set_args({graph, QI_IID_ADDR, QI_OUT_ADDR});
    auto control_qi = make_marshaler();
    ASSERT_EQ(mgr_.filter_graph_query_interface(control_qi), com::S_OK);
    const u32 media = read_u32(QI_OUT_ADDR);

    write_guid(QI_IID_ADDR, IID_IMEDIAEVENT);
    set_args({graph, QI_IID_ADDR, QI_OUT_ADDR});
    auto event_qi = make_marshaler();
    ASSERT_EQ(mgr_.filter_graph_query_interface(event_qi), com::S_OK);
    const u32 media_event = read_u32(QI_OUT_ADDR);

    set_args({media});
    auto run_marshaler = make_marshaler();
    ASSERT_EQ(mgr_.filter_graph_media_control_run(run_marshaler), com::S_OK);

    set_args({media_event, 0, EV_CODE_ADDR});
    auto poll_marshaler = make_marshaler();
    EXPECT_EQ(mgr_.filter_graph_media_event_wait_for_completion(poll_marshaler),
              0x80004004u);

    set_args({media_event, 2000, EV_CODE_ADDR});
    auto wait_marshaler = make_marshaler();
    EXPECT_EQ(mgr_.filter_graph_media_event_wait_for_completion(wait_marshaler),
              com::S_OK);
    EXPECT_EQ(read_u32(EV_CODE_ADDR), 0x01u);

    std::filesystem::remove_all(root);
}

TEST_F(FilterGraphTest, FailedRenderDoesNotFabricateCompletion) {
    static constexpr GuestAddr EVENT_CODE_ADDR = DATA_BASE + 0x200;
    static constexpr GuestAddr EVENT_P1_ADDR = DATA_BASE + 0x204;
    static constexpr GuestAddr EVENT_P2_ADDR = DATA_BASE + 0x208;
    static constexpr u32 E_POINTER = 0x80004003u;
    static constexpr u32 E_ABORT = 0x80004004u;
    static constexpr u32 VFW_E_WRONG_STATE = 0x80040227u;

    write_guid(CLSID_ADDR, CLSID_FILTERGRAPH);
    write_guid(IID_ADDR, IID_IGRAPHBUILDER);
    set_args({CLSID_ADDR, 0, 1, IID_ADDR, OUT_ADDR});
    auto create_marshaler = make_marshaler();
    ASSERT_EQ(mgr_.co_create_instance(create_marshaler), com::S_OK);
    const u32 graph = read_u32(OUT_ADDR);

    write_guid(QI_IID_ADDR, IID_IMEDIAEVENT);
    set_args({graph, QI_IID_ADDR, QI_OUT_ADDR});
    auto event_qi = make_marshaler();
    ASSERT_EQ(mgr_.filter_graph_query_interface(event_qi), com::S_OK);
    const u32 media_event = read_u32(QI_OUT_ADDR);

    write_guid(QI_IID_ADDR, IID_IMEDIACONTROL);
    set_args({graph, QI_IID_ADDR, QI_OUT_ADDR});
    auto control_qi = make_marshaler();
    ASSERT_EQ(mgr_.filter_graph_query_interface(control_qi), com::S_OK);
    const u32 media_control = read_u32(QI_OUT_ADDR);

    // A null file name is an invalid graph request and must not fabricate a
    // successful end-of-stream event.
    set_args({graph, 0, 0});
    auto render_marshaler = make_marshaler();
    ASSERT_EQ(mgr_.filter_graph_render_file(render_marshaler), E_POINTER);

    set_args({media_event, EVENT_CODE_ADDR, EVENT_P1_ADDR, EVENT_P2_ADDR, 0});
    auto get_event = make_marshaler();
    EXPECT_EQ(mgr_.filter_graph_media_event_get_event(get_event), E_ABORT);

    // Run without a rendered graph fails honestly.
    set_args({media_control});
    auto run_marshaler = make_marshaler();
    EXPECT_EQ(mgr_.filter_graph_media_control_run(run_marshaler),
              VFW_E_WRONG_STATE);
}

TEST_F(FilterGraphTest, WaitForCompletionTimesOutWithoutRenderedMedia) {
    static constexpr GuestAddr EV_CODE_ADDR = DATA_BASE + 0x210;
    static constexpr u32 VFW_E_WRONG_STATE = 0x80040227u;

    write_guid(CLSID_ADDR, CLSID_FILTERGRAPH);
    write_guid(IID_ADDR, IID_IGRAPHBUILDER);
    set_args({CLSID_ADDR, 0, 1, IID_ADDR, OUT_ADDR});
    auto create_marshaler = make_marshaler();
    ASSERT_EQ(mgr_.co_create_instance(create_marshaler), com::S_OK);
    const u32 graph = read_u32(OUT_ADDR);

    write_guid(QI_IID_ADDR, IID_IMEDIAEVENT);
    set_args({graph, QI_IID_ADDR, QI_OUT_ADDR});
    auto event_qi = make_marshaler();
    ASSERT_EQ(mgr_.filter_graph_query_interface(event_qi), com::S_OK);
    const u32 media_event = read_u32(QI_OUT_ADDR);

    // Learn: WaitForCompletion fails unless the graph is running.
    set_args({media_event, 0, EV_CODE_ADDR});
    auto wait_empty = make_marshaler();
    EXPECT_EQ(mgr_.filter_graph_media_event_wait_for_completion(wait_empty),
              VFW_E_WRONG_STATE);

    set_args({media_event, 0, EV_CODE_ADDR});
    auto wait_again = make_marshaler();
    EXPECT_EQ(mgr_.filter_graph_media_event_wait_for_completion(wait_again),
              VFW_E_WRONG_STATE);
}

TEST_F(FilterGraphTest, GetEventHandleSignalsEcCompleteForVideoOnly) {
    // Learn IMediaEvent::GetEventHandle: manual-reset event, signaled while
    // notifications are queued. After a video-only clip finishes, WaitForSingleObject
    // and GetEvent must see EC_COMPLETE (0x01).
    char temp_template[] = "/tmp/vitreo_eventhandle_test_XXXXXX";
    char* temp_dir = ::mkdtemp(temp_template);
    ASSERT_NE(temp_dir, nullptr);
    const std::filesystem::path root = temp_dir;
    const std::filesystem::path host_clip = root / "clip.mp4";
    const std::string command =
        "ffmpeg -y -hide_banner -loglevel error -f lavfi "
        "-i color=c=red:s=16x16:d=0.4:r=10 -an -pix_fmt yuv420p \"" +
        host_clip.string() + "\"";
    if (std::system(command.c_str()) != 0) {
        std::filesystem::remove_all(root);
        GTEST_SKIP() << "ffmpeg could not write a test video";
    }

    capsule::Vfs vfs;
    ASSERT_TRUE(vfs.mount_drive(
        'C', std::make_unique<capsule::HostFileSystem>(root.string())).is_ok());
    mgr_.set_vfs(&vfs);

    write_guid(CLSID_ADDR, CLSID_FILTERGRAPH);
    write_guid(IID_ADDR, IID_IGRAPHBUILDER);
    set_args({CLSID_ADDR, 0, 1, IID_ADDR, OUT_ADDR});
    auto create_marshaler = make_marshaler();
    ASSERT_EQ(mgr_.co_create_instance(create_marshaler), com::S_OK);
    const u32 graph = read_u32(OUT_ADDR);

    static constexpr GuestAddr PATH_ADDR = DATA_BASE + 0x200;
    static constexpr GuestAddr HANDLE_ADDR = DATA_BASE + 0x280;
    static constexpr GuestAddr CODE_ADDR = DATA_BASE + 0x284;
    static constexpr GuestAddr P1_ADDR = DATA_BASE + 0x288;
    static constexpr GuestAddr P2_ADDR = DATA_BASE + 0x28C;
    auto path_marshaler = make_marshaler();
    ASSERT_TRUE(
        path_marshaler.write_wide_string(PATH_ADDR, "C:\\clip.mp4").is_ok());
    set_args({graph, PATH_ADDR, 0});
    auto render_marshaler = make_marshaler();
    ASSERT_EQ(mgr_.filter_graph_render_file(render_marshaler), com::S_OK);

    write_guid(QI_IID_ADDR, IID_IMEDIACONTROL);
    set_args({graph, QI_IID_ADDR, QI_OUT_ADDR});
    auto control_qi = make_marshaler();
    ASSERT_EQ(mgr_.filter_graph_query_interface(control_qi), com::S_OK);
    const u32 media = read_u32(QI_OUT_ADDR);

    write_guid(QI_IID_ADDR, IID_IMEDIAEVENT);
    set_args({graph, QI_IID_ADDR, QI_OUT_ADDR});
    auto event_qi = make_marshaler();
    ASSERT_EQ(mgr_.filter_graph_query_interface(event_qi), com::S_OK);
    const u32 media_event = read_u32(QI_OUT_ADDR);

    set_args({media});
    auto run_marshaler = make_marshaler();
    ASSERT_EQ(mgr_.filter_graph_media_control_run(run_marshaler), com::S_OK);

    set_args({media_event, HANDLE_ADDR});
    auto handle_marshaler = make_marshaler();
    EXPECT_EQ(mgr_.filter_graph_media_event_get_event_handle(handle_marshaler),
              com::S_OK);
    const u32 event_handle = read_u32(HANDLE_ADDR);
    EXPECT_NE(event_handle, 0u);

    EXPECT_EQ(sync_.wait_for_single_object(event_handle, 2000), WAIT_OBJECT_0);

    set_args({media_event, CODE_ADDR, P1_ADDR, P2_ADDR, 0});
    auto get_event = make_marshaler();
    EXPECT_EQ(mgr_.filter_graph_media_event_get_event(get_event), com::S_OK);
    EXPECT_EQ(read_u32(CODE_ADDR), 0x01u);

    std::filesystem::remove_all(root);
}

TEST_F(FilterGraphTest, MediaEventExSetNotifyWindowIsVtableSlot) {
    write_guid(CLSID_ADDR, CLSID_FILTERGRAPH);
    write_guid(IID_ADDR, IID_IGRAPHBUILDER);
    set_args({CLSID_ADDR, 0, 1, IID_ADDR, OUT_ADDR});
    auto create_marshaler = make_marshaler();
    ASSERT_EQ(mgr_.co_create_instance(create_marshaler), com::S_OK);
    const u32 graph = read_u32(OUT_ADDR);

    write_guid(QI_IID_ADDR, IID_IMEDIAEVENTEX);
    set_args({graph, QI_IID_ADDR, QI_OUT_ADDR});
    auto event_qi = make_marshaler();
    ASSERT_EQ(mgr_.filter_graph_query_interface(event_qi), com::S_OK);
    const u32 media_event = read_u32(QI_OUT_ADDR);
    ASSERT_NE(media_event, 0u);

    auto vtable = memory_.read_u32(media_event);
    ASSERT_TRUE(vtable.is_ok());
    auto set_notify = registry_.get_thunk_address(
        "ole32.dll", "Vitreo_FilterGraph_MediaEventEx_SetNotifyWindow");
    ASSERT_TRUE(set_notify.is_ok());
    // IUnknown(3) + IDispatch(4) + IMediaEvent(6) = slot 13.
    EXPECT_EQ(memory_.read_u32(vtable.value() + 13 * 4).value(),
              static_cast<u32>(set_notify.value()));

    static constexpr GuestAddr FLAGS_ADDR = DATA_BASE + 0x200;
    set_args({media_event, 0x1234, 0x8001, 0xABCD});
    auto notify_marshaler = make_marshaler();
    EXPECT_EQ(mgr_.filter_graph_media_event_set_notify_window(notify_marshaler),
              com::S_OK);

    set_args({media_event, 0});
    auto flags_set = make_marshaler();
    EXPECT_EQ(mgr_.filter_graph_media_event_set_notify_flags(flags_set),
              com::S_OK);
    set_args({media_event, FLAGS_ADDR});
    auto flags_get = make_marshaler();
    EXPECT_EQ(mgr_.filter_graph_media_event_get_notify_flags(flags_get),
              com::S_OK);
    EXPECT_EQ(read_u32(FLAGS_ADDR), 0u);
}

TEST_F(FilterGraphTest, GetEventAfterRunIsEmptyUntilComplete) {
    // Learn IMediaEvent::GetEvent(0) is a poll. A running graph with no
    // EC_COMPLETE / EC_ERRORABORT yet must return E_ABORT, not a startup
    // notification that callers treat as end-of-stream.
    char temp_template[] = "/tmp/vitreo_clockchanged_test_XXXXXX";
    char* temp_dir = ::mkdtemp(temp_template);
    ASSERT_NE(temp_dir, nullptr);
    const std::filesystem::path root = temp_dir;
    const std::filesystem::path host_clip = root / "clip.mp4";
    const std::string command =
        "ffmpeg -y -hide_banner -loglevel error -f lavfi "
        "-i color=c=red:s=16x16:d=0.2:r=10 -an -pix_fmt yuv420p \"" +
        host_clip.string() + "\"";
    if (std::system(command.c_str()) != 0) {
        std::filesystem::remove_all(root);
        GTEST_SKIP() << "ffmpeg could not write a test video";
    }

    capsule::Vfs vfs;
    ASSERT_TRUE(vfs.mount_drive(
        'C', std::make_unique<capsule::HostFileSystem>(root.string())).is_ok());
    mgr_.set_vfs(&vfs);

    write_guid(CLSID_ADDR, CLSID_FILTERGRAPH);
    write_guid(IID_ADDR, IID_IGRAPHBUILDER);
    set_args({CLSID_ADDR, 0, 1, IID_ADDR, OUT_ADDR});
    auto create_marshaler = make_marshaler();
    ASSERT_EQ(mgr_.co_create_instance(create_marshaler), com::S_OK);
    const u32 graph = read_u32(OUT_ADDR);

    static constexpr GuestAddr PATH_ADDR = DATA_BASE + 0x200;
    auto path_marshaler = make_marshaler();
    ASSERT_TRUE(
        path_marshaler.write_wide_string(PATH_ADDR, "C:\\clip.mp4").is_ok());
    set_args({graph, PATH_ADDR, 0});
    auto render_marshaler = make_marshaler();
    ASSERT_EQ(mgr_.filter_graph_render_file(render_marshaler), com::S_OK);

    write_guid(QI_IID_ADDR, IID_IMEDIACONTROL);
    set_args({graph, QI_IID_ADDR, QI_OUT_ADDR});
    auto control_qi = make_marshaler();
    ASSERT_EQ(mgr_.filter_graph_query_interface(control_qi), com::S_OK);
    const u32 media = read_u32(QI_OUT_ADDR);

    write_guid(QI_IID_ADDR, IID_IMEDIAEVENT);
    set_args({graph, QI_IID_ADDR, QI_OUT_ADDR});
    auto event_qi = make_marshaler();
    ASSERT_EQ(mgr_.filter_graph_query_interface(event_qi), com::S_OK);
    const u32 media_event = read_u32(QI_OUT_ADDR);

    set_args({media});
    auto run_marshaler = make_marshaler();
    ASSERT_EQ(mgr_.filter_graph_media_control_run(run_marshaler), com::S_OK);

    static constexpr GuestAddr CODE_ADDR = DATA_BASE + 0x280;
    static constexpr GuestAddr P1_ADDR = DATA_BASE + 0x284;
    static constexpr GuestAddr P2_ADDR = DATA_BASE + 0x288;
    static constexpr u32 E_ABORT = 0x80004004u;
    set_args({media_event, CODE_ADDR, P1_ADDR, P2_ADDR, 0});
    auto get_event = make_marshaler();
    EXPECT_EQ(mgr_.filter_graph_media_event_get_event(get_event), E_ABORT);

    std::filesystem::remove_all(root);
}

TEST_F(FilterGraphTest, GetEventHonorsTimeoutPoll) {
    write_guid(CLSID_ADDR, CLSID_FILTERGRAPH);
    write_guid(IID_ADDR, IID_IGRAPHBUILDER);
    set_args({CLSID_ADDR, 0, 1, IID_ADDR, OUT_ADDR});
    auto create_marshaler = make_marshaler();
    ASSERT_EQ(mgr_.co_create_instance(create_marshaler), com::S_OK);
    const u32 graph = read_u32(OUT_ADDR);

    write_guid(QI_IID_ADDR, IID_IMEDIAEVENT);
    set_args({graph, QI_IID_ADDR, QI_OUT_ADDR});
    auto event_qi = make_marshaler();
    ASSERT_EQ(mgr_.filter_graph_query_interface(event_qi), com::S_OK);
    const u32 media_event = read_u32(QI_OUT_ADDR);

    static constexpr GuestAddr CODE_ADDR = DATA_BASE + 0x200;
    static constexpr GuestAddr P1_ADDR = DATA_BASE + 0x204;
    static constexpr GuestAddr P2_ADDR = DATA_BASE + 0x208;
    static constexpr u32 E_ABORT = 0x80004004u;
    set_args({media_event, CODE_ADDR, P1_ADDR, P2_ADDR, 0});
    auto get_event = make_marshaler();
    EXPECT_EQ(mgr_.filter_graph_media_event_get_event(get_event), E_ABORT);
}
