// SPDX-License-Identifier: LGPL-2.1-only

/// @file test_com_filter_graph_interfaces.cpp
/// @brief DirectShow filter graph creation, QueryInterface, vtable and argument tests.

#include "com_test_common.hpp"
#include "com_test_filter_graph_fixture.hpp"

TEST_F(FilterGraphTest, CoCreateInstanceReturnsGraphBuilder) {
    write_guid(CLSID_ADDR, CLSID_FILTERGRAPH);
    write_guid(IID_ADDR, IID_IGRAPHBUILDER);
    (void)memory_.write_u32(OUT_ADDR, 0xCAFEBABEu);
    set_args({CLSID_ADDR, 0, 1, IID_ADDR, OUT_ADDR});
    auto marshaler = make_marshaler();
    ASSERT_EQ(mgr_.co_create_instance(marshaler), com::S_OK);
    const u32 graph = read_u32(OUT_ADDR);
    ASSERT_NE(graph, 0u);
    EXPECT_NE(graph, 0xCAFEBABEu);

    auto vtable = memory_.read_u32(graph);
    ASSERT_TRUE(vtable.is_ok());
    auto render_file = registry_.get_thunk_address(
        "ole32.dll", "Vitreo_FilterGraph_RenderFile");
    ASSERT_TRUE(render_file.is_ok());
    // IUnknown(3) + IFilterGraph(8) + Connect + Render + RenderFile = slot 13.
    EXPECT_EQ(memory_.read_u32(vtable.value() + 13 * 4).value(),
              static_cast<u32>(render_file.value()));
}

TEST_F(FilterGraphTest, QueryInterfaceSupportsDirectShowInterfaces) {
    write_guid(CLSID_ADDR, CLSID_FILTERGRAPH);
    write_guid(IID_ADDR, IID_IGRAPHBUILDER);
    set_args({CLSID_ADDR, 0, 1, IID_ADDR, OUT_ADDR});
    auto create_marshaler = make_marshaler();
    ASSERT_EQ(mgr_.co_create_instance(create_marshaler), com::S_OK);
    const u32 graph = read_u32(OUT_ADDR);

    const std::array<Guid, 9> iids{
        IID_IMEDIACONTROL, IID_IBASICAUDIO, IID_IMEDIAEVENT, IID_IMEDIAEVENTEX,
        IID_IMEDIASEEKING, IID_IVIDEOWINDOW, IID_IBASICVIDEO, IID_IMEDIAPOSITION,
        IID_IMEDIAEVENTSINK};
    for (const Guid& iid : iids) {
        write_guid(QI_IID_ADDR, iid);
        set_args({graph, QI_IID_ADDR, QI_OUT_ADDR});
        auto qi_marshaler = make_marshaler();
        ASSERT_EQ(mgr_.filter_graph_query_interface(qi_marshaler), com::S_OK)
            << "QI failed for iid Data1=0x" << std::hex << iid.Data1;
        EXPECT_NE(read_u32(QI_OUT_ADDR), 0u);
    }
}

TEST_F(FilterGraphTest, BasicAudioVtableMatchesSdkMethodOrder) {
    write_guid(CLSID_ADDR, CLSID_FILTERGRAPH);
    write_guid(IID_ADDR, IID_IGRAPHBUILDER);
    set_args({CLSID_ADDR, 0, 1, IID_ADDR, OUT_ADDR});
    auto create_marshaler = make_marshaler();
    ASSERT_EQ(mgr_.co_create_instance(create_marshaler), com::S_OK);
    const u32 graph = read_u32(OUT_ADDR);

    write_guid(QI_IID_ADDR, IID_IBASICAUDIO);
    set_args({graph, QI_IID_ADDR, QI_OUT_ADDR});
    auto audio_qi_marshaler = make_marshaler();
    ASSERT_EQ(mgr_.filter_graph_query_interface(audio_qi_marshaler), com::S_OK);
    const u32 audio = read_u32(QI_OUT_ADDR);
    const u32 vtable = read_u32(audio);

    static constexpr std::array<std::string_view, 4> method_names{{
        "Vitreo_FilterGraph_BasicAudio_put_Volume",
        "Vitreo_FilterGraph_BasicAudio_get_Volume",
        "Vitreo_FilterGraph_BasicAudio_put_Balance",
        "Vitreo_FilterGraph_BasicAudio_get_Balance",
    }};
    for (usize index = 0; index < method_names.size(); ++index) {
        auto thunk = registry_.get_thunk_address("ole32.dll", method_names[index]);
        ASSERT_TRUE(thunk.is_ok());
        EXPECT_EQ(read_u32(vtable + (7 + index) * 4),
                  static_cast<u32>(thunk.value()));
    }
}

TEST_F(FilterGraphTest, MediaControlAndBasicAudioMethodsAreSafe) {
    write_guid(CLSID_ADDR, CLSID_FILTERGRAPH);
    write_guid(IID_ADDR, IID_IGRAPHBUILDER);
    set_args({CLSID_ADDR, 0, 1, IID_ADDR, OUT_ADDR});
    auto create_marshaler = make_marshaler();
    ASSERT_EQ(mgr_.co_create_instance(create_marshaler), com::S_OK);
    const u32 graph = read_u32(OUT_ADDR);

    write_guid(QI_IID_ADDR, IID_IMEDIACONTROL);
    set_args({graph, QI_IID_ADDR, QI_OUT_ADDR});
    auto media_qi_marshaler = make_marshaler();
    ASSERT_EQ(mgr_.filter_graph_query_interface(media_qi_marshaler), com::S_OK);
    const u32 media = read_u32(QI_OUT_ADDR);

    set_args({media, 0, STATE_ADDR});
    auto state_marshaler = make_marshaler();
    ASSERT_EQ(mgr_.filter_graph_media_control_get_state(state_marshaler),
              com::S_OK);
    EXPECT_EQ(read_u32(STATE_ADDR), 0u);

    // Learn GetState(msTimeout, *pfs): timeout 0 is a poll, not a null pfs.
    set_args({media, 0, 0});
    auto null_state = make_marshaler();
    EXPECT_EQ(mgr_.filter_graph_media_control_get_state(null_state),
              0x80004003u);

    write_guid(QI_IID_ADDR, IID_IBASICAUDIO);
    set_args({graph, QI_IID_ADDR, QI_OUT_ADDR});
    auto audio_qi_marshaler = make_marshaler();
    ASSERT_EQ(mgr_.filter_graph_query_interface(audio_qi_marshaler), com::S_OK);
    const u32 audio = read_u32(QI_OUT_ADDR);

    set_args({audio, static_cast<u32>(-5000)});
    auto put_volume_marshaler = make_marshaler();
    ASSERT_EQ(mgr_.filter_graph_basic_audio_put_volume(put_volume_marshaler),
              com::S_OK);
    set_args({audio, VOLUME_ADDR});
    auto get_volume_marshaler = make_marshaler();
    ASSERT_EQ(mgr_.filter_graph_basic_audio_get_volume(get_volume_marshaler),
              com::S_OK);
    EXPECT_EQ(static_cast<i32>(read_u32(VOLUME_ADDR)), -5000);
}

TEST_F(FilterGraphTest, VideoWindowVtableMatchesSdkMethodOrder) {
    write_guid(CLSID_ADDR, CLSID_FILTERGRAPH);
    write_guid(IID_ADDR, IID_IGRAPHBUILDER);
    set_args({CLSID_ADDR, 0, 1, IID_ADDR, OUT_ADDR});
    auto create_marshaler = make_marshaler();
    ASSERT_EQ(mgr_.co_create_instance(create_marshaler), com::S_OK);
    const u32 graph = read_u32(OUT_ADDR);

    write_guid(QI_IID_ADDR, IID_IVIDEOWINDOW);
    set_args({graph, QI_IID_ADDR, QI_OUT_ADDR});
    auto video_qi = make_marshaler();
    ASSERT_EQ(mgr_.filter_graph_query_interface(video_qi), com::S_OK);
    const u32 video = read_u32(QI_OUT_ADDR);
    ASSERT_NE(video, 0u);
    const u32 vtable = read_u32(video);

    auto put_owner = registry_.get_thunk_address(
        "ole32.dll", "Vitreo_FilterGraph_VideoWindow_put_Owner");
    ASSERT_TRUE(put_owner.is_ok());
    // IUnknown(3) + IDispatch(4) + 22 IVideoWindow methods before put_Owner.
    EXPECT_EQ(read_u32(vtable + 29 * 4), static_cast<u32>(put_owner.value()));

    static constexpr GuestAddr OWNER_ADDR = DATA_BASE + 0x180;
    set_args({video, 0x12340000u});
    auto put_marshaler = make_marshaler();
    EXPECT_EQ(mgr_.filter_graph_video_window(put_marshaler, 22),
              com::E_NOINTERFACE);

    (void)memory_.write_u32(OWNER_ADDR, 0xFFFFFFFFu);
    set_args({video, OWNER_ADDR});
    auto get_marshaler = make_marshaler();
    EXPECT_EQ(mgr_.filter_graph_video_window(get_marshaler, 23),
              com::E_NOINTERFACE);
    EXPECT_EQ(read_u32(OWNER_ADDR), 0xFFFFFFFFu);
}

TEST_F(FilterGraphTest, CoCreateInstanceAcceptsFilterGraphNoThread) {
    write_guid(CLSID_ADDR, CLSID_FILTERGRAPH_NOTHREAD);
    write_guid(IID_ADDR, IID_IGRAPHBUILDER);
    set_args({CLSID_ADDR, 0, 1, IID_ADDR, OUT_ADDR});
    auto create_marshaler = make_marshaler();
    ASSERT_EQ(mgr_.co_create_instance(create_marshaler), com::S_OK);
    EXPECT_NE(read_u32(OUT_ADDR), 0u);
}

TEST_F(FilterGraphTest, BasicVideoVtableMatchesSdkMethodOrder) {
    write_guid(CLSID_ADDR, CLSID_FILTERGRAPH);
    write_guid(IID_ADDR, IID_IGRAPHBUILDER);
    set_args({CLSID_ADDR, 0, 1, IID_ADDR, OUT_ADDR});
    auto create_marshaler = make_marshaler();
    ASSERT_EQ(mgr_.co_create_instance(create_marshaler), com::S_OK);
    const u32 graph = read_u32(OUT_ADDR);

    write_guid(QI_IID_ADDR, IID_IBASICVIDEO);
    set_args({graph, QI_IID_ADDR, QI_OUT_ADDR});
    auto video_qi = make_marshaler();
    ASSERT_EQ(mgr_.filter_graph_query_interface(video_qi), com::S_OK);
    const u32 video = read_u32(QI_OUT_ADDR);
    ASSERT_NE(video, 0u);
    const u32 vtable = read_u32(video);

    auto get_size = registry_.get_thunk_address(
        "ole32.dll", "Vitreo_FilterGraph_BasicVideo_GetVideoSize");
    ASSERT_TRUE(get_size.is_ok());
    // IUnknown(3) + IDispatch(4) + 27 IBasicVideo methods before GetVideoSize.
    EXPECT_EQ(read_u32(vtable + 34 * 4), static_cast<u32>(get_size.value()));

    static constexpr GuestAddr WIDTH_ADDR = DATA_BASE + 0x180;
    static constexpr GuestAddr HEIGHT_ADDR = DATA_BASE + 0x184;
    (void)memory_.write_u32(WIDTH_ADDR, 0xFFFFFFFFu);
    (void)memory_.write_u32(HEIGHT_ADDR, 0xFFFFFFFFu);
    set_args({video, WIDTH_ADDR, HEIGHT_ADDR});
    auto size_marshaler = make_marshaler();
    EXPECT_EQ(mgr_.filter_graph_basic_video(size_marshaler, 27),
              com::E_NOINTERFACE);
    EXPECT_EQ(read_u32(WIDTH_ADDR), 0xFFFFFFFFu);
    EXPECT_EQ(read_u32(HEIGHT_ADDR), 0xFFFFFFFFu);
}

TEST_F(FilterGraphTest, MediaPositionVtableMatchesSdkMethodOrder) {
    write_guid(CLSID_ADDR, CLSID_FILTERGRAPH);
    write_guid(IID_ADDR, IID_IGRAPHBUILDER);
    set_args({CLSID_ADDR, 0, 1, IID_ADDR, OUT_ADDR});
    auto create_marshaler = make_marshaler();
    ASSERT_EQ(mgr_.co_create_instance(create_marshaler), com::S_OK);
    const u32 graph = read_u32(OUT_ADDR);

    write_guid(QI_IID_ADDR, IID_IMEDIAPOSITION);
    set_args({graph, QI_IID_ADDR, QI_OUT_ADDR});
    auto position_qi = make_marshaler();
    ASSERT_EQ(mgr_.filter_graph_query_interface(position_qi), com::S_OK);
    const u32 position = read_u32(QI_OUT_ADDR);
    ASSERT_NE(position, 0u);
    const u32 vtable = read_u32(position);

    auto get_duration = registry_.get_thunk_address(
        "ole32.dll", "Vitreo_FilterGraph_MediaPosition_get_Duration");
    ASSERT_TRUE(get_duration.is_ok());
    // IUnknown(3) + IDispatch(4) + get_Duration is the first IMediaPosition method.
    EXPECT_EQ(read_u32(vtable + 7 * 4),
              static_cast<u32>(get_duration.value()));

    static constexpr GuestAddr DURATION_ADDR = DATA_BASE + 0x180;
    (void)memory_.write_u32(DURATION_ADDR, 0xFFFFFFFFu);
    (void)memory_.write_u32(DURATION_ADDR + 4, 0xFFFFFFFFu);
    set_args({position, DURATION_ADDR});
    auto duration_marshaler = make_marshaler();
    EXPECT_EQ(mgr_.filter_graph_media_position(duration_marshaler, 0),
              com::S_OK);
    EXPECT_EQ(read_u32(DURATION_ADDR), 0u);
    EXPECT_EQ(read_u32(DURATION_ADDR + 4), 0u);
}

TEST_F(FilterGraphTest, UnknownClsidClearsOutputPointer) {
    static constexpr Guid CLSID_UNKNOWN{
        0xDEADBEEFu, 0x0000u, 0x0000u,
        {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
    write_guid(CLSID_ADDR, CLSID_UNKNOWN);
    write_guid(IID_ADDR, IID_IGRAPHBUILDER);
    (void)memory_.write_u32(OUT_ADDR, 0xCAFEBABEu);
    set_args({CLSID_ADDR, 0, 1, IID_ADDR, OUT_ADDR});
    auto marshaler = make_marshaler();
    EXPECT_EQ(mgr_.co_create_instance(marshaler), com::REGDB_E_CLASSNOTREG);
    EXPECT_EQ(read_u32(OUT_ADDR), 0u);
}

TEST_F(FilterGraphTest, BasicAudioRejectsOutOfRangeVolume) {
    write_guid(CLSID_ADDR, CLSID_FILTERGRAPH);
    write_guid(IID_ADDR, IID_IGRAPHBUILDER);
    set_args({CLSID_ADDR, 0, 1, IID_ADDR, OUT_ADDR});
    auto create_marshaler = make_marshaler();
    ASSERT_EQ(mgr_.co_create_instance(create_marshaler), com::S_OK);
    const u32 graph = read_u32(OUT_ADDR);

    write_guid(QI_IID_ADDR, IID_IBASICAUDIO);
    set_args({graph, QI_IID_ADDR, QI_OUT_ADDR});
    auto audio_qi = make_marshaler();
    ASSERT_EQ(mgr_.filter_graph_query_interface(audio_qi), com::S_OK);
    const u32 audio = read_u32(QI_OUT_ADDR);

    set_args({audio, static_cast<u32>(-10001)});
    auto below_minimum = make_marshaler();
    EXPECT_EQ(mgr_.filter_graph_basic_audio_put_volume(below_minimum),
              com::E_INVALIDARG);

    set_args({audio, 1});
    auto above_maximum = make_marshaler();
    EXPECT_EQ(mgr_.filter_graph_basic_audio_put_volume(above_maximum),
              com::E_INVALIDARG);
}

// ---------------------------------------------------------------------------
// Both bitnesses. Expected values come from Windows 11 (quartz.dll, MSVC
// builds against SDK 10.0.26100 on winoracle, x86 and x64):
//   tools/oracle_probe/dshow_layout_probe{32,64}.expected  - slot counts
//   tools/oracle_probe/dshow_graph_probe{32,64}.expected   - AddFilter/
//     EnumFilters/FindFilterByName/RemoveFilter/GetEvent behaviour
//   tools/oracle_probe/dshow_graph_names_probe{32,64}.expected - naming
// ---------------------------------------------------------------------------

namespace {
constexpr u32 kVfwSDuplicateName = 0x0004022Du;
constexpr u32 kVfwENotFound = 0x80040216u;
constexpr u32 kVfwEEnumOutOfSync = 0x80040203u;
constexpr u32 kEPointer = 0x80004003u;
constexpr u32 kEInvalidArg = 0x80070057u;
constexpr u32 kEAbort = 0x80004004u;
constexpr Guid kIidEnumFilters{0x56A86893u, 0x0AD4u, 0x11CEu,
                               {0xB0, 0x3A, 0x00, 0x20, 0xAF, 0x0B, 0xA7, 0x70}};
}  // namespace

TEST_P(FilterGraphBitnessTest, InterfaceObjectsUsePointerWidthVtables) {
    const GuestAddr graph = create_graph();
    ASSERT_NE(graph, 0u);
    // {vtable*} then pointer-width slots; IGraphBuilder has 18 slots
    // (AddFilter 3, EnumFilters 5, FindFilterByName 6, RenderFile 13,
    // ShouldOperationContinue 17).
    if (x64_) {
        EXPECT_EQ(graph % 8u, 0u);
    }
    const GuestAddr vtable = static_cast<GuestAddr>(ptr(graph));
    EXPECT_EQ(vtable, graph + ps());
    EXPECT_EQ(ptr(vtable + 0 * ps()), thunk("Vitreo_FilterGraph_QueryInterface"));
    EXPECT_EQ(ptr(vtable + 1 * ps()), thunk("Vitreo_FilterGraph_AddRef"));
    EXPECT_EQ(ptr(vtable + 2 * ps()), thunk("Vitreo_FilterGraph_Release"));
    EXPECT_EQ(ptr(vtable + 3 * ps()), thunk("Vitreo_FilterGraph_AddFilter"));
    EXPECT_EQ(ptr(vtable + 5 * ps()), thunk("Vitreo_FilterGraph_EnumFilters"));
    EXPECT_EQ(ptr(vtable + 6 * ps()), thunk("Vitreo_FilterGraph_FindFilterByName"));
    EXPECT_EQ(ptr(vtable + 13 * ps()), thunk("Vitreo_FilterGraph_RenderFile"));
    EXPECT_EQ(ptr(vtable + 17 * ps()),
              thunk("Vitreo_FilterGraph_ShouldOperationContinue"));

    // Every interface QI reaches: (iid, SDK slot count, name of the last slot).
    struct Expect {
        Guid iid;
        u32 slots;
        std::string_view last;
    };
    const Expect expects[] = {
        {IID_IMEDIACONTROL, 16, "Vitreo_FilterGraph_MediaControl_StopWhenReady"},
        {IID_IBASICAUDIO, 11, "Vitreo_FilterGraph_BasicAudio_get_Balance"},
        {IID_IMEDIAEVENT, 16, "Vitreo_FilterGraph_MediaEventEx_GetNotifyFlags"},
        {IID_IMEDIAEVENTEX, 16, "Vitreo_FilterGraph_MediaEventEx_GetNotifyFlags"},
        {IID_IMEDIASEEKING, 20, "Vitreo_FilterGraph_MediaSeeking_GetPreroll"},
        {IID_IVIDEOWINDOW, 46, "Vitreo_FilterGraph_VideoWindow_IsCursorHidden"},
        {IID_IBASICVIDEO, 39, "Vitreo_FilterGraph_BasicVideo_IsUsingDefaultDestination"},
        {IID_IMEDIAPOSITION, 18, "Vitreo_FilterGraph_MediaPosition_CanSeekBackward"},
        {IID_IMEDIAEVENTSINK, 4, "Vitreo_FilterGraph_MediaEventSink_Notify"},
        {IID_IGRAPHBUILDER, 18, "Vitreo_FilterGraph_ShouldOperationContinue"},
    };
    for (const Expect& e : expects) {
        const GuestAddr object = qi(graph, e.iid);
        ASSERT_NE(object, 0u) << std::hex << e.iid.Data1;
        if (x64_) {
            EXPECT_EQ(object % 8u, 0u);
        }
        const GuestAddr vt = static_cast<GuestAddr>(ptr(object));
        EXPECT_EQ(vt, object + ps());
        EXPECT_EQ(ptr(vt), thunk("Vitreo_FilterGraph_QueryInterface"));
        EXPECT_EQ(ptr(vt + 2 * ps()), thunk("Vitreo_FilterGraph_Release"));
        EXPECT_EQ(ptr(vt + (e.slots - 1) * ps()), thunk(e.last))
            << std::hex << e.iid.Data1;
        // The returned pointer is itself a working interface: QI back to
        // IGraphBuilder through it.
        EXPECT_EQ(qi(object, IID_IGRAPHBUILDER), graph);
    }
}

TEST_P(FilterGraphBitnessTest, AddFilterEnumFiltersFindFilterByNameMatchWindows) {
    const GuestAddr graph = create_graph();
    const GuestAddr A = filter_obj(0), B = filter_obj(1), C = filter_obj(2);

    put_wide(NAME_ADDR, u"Frontier Texture Renderer");
    EXPECT_EQ(invoke(&ComCoreManager::filter_graph_add_filter, {graph, A, NAME_ADDR}),
              com::S_OK);
    EXPECT_EQ(filters_[0].refs, 2);
    EXPECT_EQ(filters_[0].joins, 1);
    EXPECT_EQ(filters_[0].graph, graph);
    EXPECT_EQ(filters_[0].name, u"Frontier Texture Renderer");
    EXPECT_EQ(invoke(&ComCoreManager::filter_graph_add_filter, {graph, B, NAME_ADDR}),
              kVfwSDuplicateName);
    EXPECT_EQ(filters_[1].name, u"Frontier Texture Renderer 0001");
    EXPECT_EQ(invoke(&ComCoreManager::filter_graph_add_filter, {graph, C, 0}),
              com::S_OK);
    EXPECT_EQ(filters_[2].name, u"0002");
    put_wide(NAME_ADDR + 0x100, u"again");
    EXPECT_EQ(invoke(&ComCoreManager::filter_graph_add_filter,
                     {graph, A, NAME_ADDR + 0x100}),
              com::S_OK);
    EXPECT_EQ(filters_[0].refs, 3);
    EXPECT_EQ(filters_[0].joins, 2);
    EXPECT_EQ(invoke(&ComCoreManager::filter_graph_add_filter, {graph, 0, NAME_ADDR}),
              kEPointer);

    // EnumFilters: most recent first -> A C B A.
    (void)memory_.write_u64(OUT2_ADDR, 0xCAFEBABECAFEBABEull);
    ASSERT_EQ(invoke(&ComCoreManager::filter_graph_enum_filters, {graph, OUT2_ADDR}),
              com::S_OK);
    if (x64_) EXPECT_EQ(memory_.read_u32(OUT2_ADDR + 4).value_or(1), 0u);
    const GuestAddr en = static_cast<GuestAddr>(ptr(OUT2_ADDR));
    ASSERT_NE(en, 0u);
    // IEnumFilters vtable: 7 pointer-width slots.
    const GuestAddr en_vt = static_cast<GuestAddr>(ptr(en));
    EXPECT_EQ(ptr(en_vt + 3 * ps()), thunk("Vitreo_FilterGraph_EnumFilters_Next"));
    EXPECT_EQ(ptr(en_vt + 6 * ps()), thunk("Vitreo_FilterGraph_EnumFilters_Clone"));

    const auto method = &ComCoreManager::filter_graph_enum_filters_method;
    const GuestAddr expected_order[] = {A, C, B, A};
    for (const GuestAddr want : expected_order) {
        (void)memory_.write_u64(ARRAY_ADDR, 0x1234);
        (void)memory_.write_u32(OUT3_ADDR, 77);
        EXPECT_EQ(invoke_op(method, 3, {en, 1, ARRAY_ADDR, OUT3_ADDR}), com::S_OK);
        EXPECT_EQ(memory_.read_u32(OUT3_ADDR).value_or(0), 1u);
        EXPECT_EQ(ptr(ARRAY_ADDR), want);
        if (x64_) EXPECT_EQ(memory_.read_u32(ARRAY_ADDR + 4).value_or(1), 0u);
        filters_[(want - FILTER_OBJS) / 0x100].refs--;  // caller's Release
    }
    EXPECT_EQ(filters_[0].refs, 3);
    EXPECT_EQ(filters_[1].refs, 2);
    EXPECT_EQ(filters_[2].refs, 2);
    EXPECT_EQ(invoke_op(method, 3, {en, 1, ARRAY_ADDR, OUT3_ADDR}), com::S_FALSE);
    EXPECT_EQ(memory_.read_u32(OUT3_ADDR).value_or(9), 0u);

    // Next(5) after Reset: S_FALSE, 4 fetched, pointer-width array.
    EXPECT_EQ(invoke_op(method, 5, {en}), com::S_OK);
    EXPECT_EQ(invoke_op(method, 3, {en, 5, ARRAY_ADDR, OUT3_ADDR}), com::S_FALSE);
    EXPECT_EQ(memory_.read_u32(OUT3_ADDR).value_or(0), 4u);
    for (u32 i = 0; i < 4; ++i) {
        EXPECT_EQ(ptr(ARRAY_ADDR + i * ps()), expected_order[i]);
    }
    EXPECT_EQ(filters_[0].refs, 5);
    EXPECT_EQ(filters_[1].refs, 3);
    EXPECT_EQ(filters_[2].refs, 3);
    filters_[0].refs -= 2;
    filters_[1].refs -= 1;
    filters_[2].refs -= 1;

    EXPECT_EQ(invoke_op(method, 5, {en}), com::S_OK);
    EXPECT_EQ(invoke_op(method, 3, {en, 2, ARRAY_ADDR, 0}), kEInvalidArg);
    EXPECT_EQ(invoke_op(method, 3, {en, 0, 0, 0}), kEPointer);
    EXPECT_EQ(invoke_op(method, 4, {en, 2}), com::S_OK);
    EXPECT_EQ(invoke_op(method, 4, {en, 5}), com::S_FALSE);
    EXPECT_EQ(invoke_op(method, 3, {en, 1, ARRAY_ADDR, OUT3_ADDR}), com::S_FALSE);
    EXPECT_EQ(memory_.read_u32(OUT3_ADDR).value_or(9), 0u);

    // Clone copies the position: Reset, Skip(1), Clone -> C next.
    EXPECT_EQ(invoke_op(method, 5, {en}), com::S_OK);
    EXPECT_EQ(invoke_op(method, 4, {en, 1}), com::S_OK);
    EXPECT_EQ(invoke_op(method, 6, {en, OUT2_ADDR + 8}), com::S_OK);
    const GuestAddr clone = static_cast<GuestAddr>(ptr(OUT2_ADDR + 8));
    ASSERT_NE(clone, 0u);
    EXPECT_NE(clone, en);
    EXPECT_EQ(invoke_op(method, 3, {clone, 1, ARRAY_ADDR, OUT3_ADDR}), com::S_OK);
    EXPECT_EQ(ptr(ARRAY_ADDR), C);
    filters_[2].refs--;
    EXPECT_EQ(invoke_op(method, 6, {en, 0}), kEPointer);
    write_guid(QI_IID_ADDR, kIidEnumFilters);
    EXPECT_EQ(invoke_op(method, 0, {en, QI_IID_ADDR, OUT2_ADDR + 8}), com::S_OK);
    EXPECT_EQ(ptr(OUT2_ADDR + 8), en);
    EXPECT_EQ(invoke_op(method, 2, {en}), 1u);

    // FindFilterByName: exact, case-sensitive, AddRef'd; a miss is
    // VFW_E_NOT_FOUND with *ppFilter = NULL; NULLs are E_POINTER.
    (void)memory_.write_u64(OUT3_ADDR, 0x1234);
    EXPECT_EQ(invoke(&ComCoreManager::filter_graph_find_filter_by_name,
                     {graph, NAME_ADDR, OUT3_ADDR}),
              com::S_OK);
    EXPECT_EQ(ptr(OUT3_ADDR), A);
    EXPECT_EQ(filters_[0].refs, 4);
    filters_[0].refs--;
    put_wide(NAME_ADDR + 0x200, u"Frontier Texture Renderer 0001");
    EXPECT_EQ(invoke(&ComCoreManager::filter_graph_find_filter_by_name,
                     {graph, NAME_ADDR + 0x200, OUT3_ADDR}),
              com::S_OK);
    EXPECT_EQ(ptr(OUT3_ADDR), B);
    filters_[1].refs--;
    put_wide(NAME_ADDR + 0x200, u"frontier texture renderer");
    (void)memory_.write_u64(OUT3_ADDR, 0x1234);
    EXPECT_EQ(invoke(&ComCoreManager::filter_graph_find_filter_by_name,
                     {graph, NAME_ADDR + 0x200, OUT3_ADDR}),
              kVfwENotFound);
    EXPECT_EQ(memory_.read_u64(OUT3_ADDR).value_or(1) & (x64_ ? ~0ull : 0xFFFFFFFFull),
              0u);
    (void)memory_.write_u64(OUT3_ADDR, 0x1234);
    EXPECT_EQ(invoke(&ComCoreManager::filter_graph_find_filter_by_name,
                     {graph, 0, OUT3_ADDR}),
              kEPointer);
    EXPECT_EQ(memory_.read_u64(OUT3_ADDR).value_or(0), 0x1234u);
    EXPECT_EQ(invoke(&ComCoreManager::filter_graph_find_filter_by_name,
                     {graph, NAME_ADDR, 0}),
              kEPointer);

    // RemoveFilter(B): JoinFilterGraph(NULL, NULL) + Release; the open
    // enumerator is now out of sync until Reset.
    EXPECT_EQ(invoke(&ComCoreManager::filter_graph_remove_filter, {graph, B}), com::S_OK);
    EXPECT_EQ(filters_[1].refs, 1);
    EXPECT_EQ(filters_[1].joins, 2);
    EXPECT_EQ(filters_[1].graph, 0u);
    EXPECT_EQ(filters_[1].name, u"<null>");
    EXPECT_EQ(invoke_op(method, 3, {en, 1, ARRAY_ADDR, OUT3_ADDR}), kVfwEEnumOutOfSync);
    EXPECT_EQ(invoke_op(method, 4, {en, 1}), kVfwEEnumOutOfSync);
    EXPECT_EQ(invoke_op(method, 5, {en}), com::S_OK);
    EXPECT_EQ(invoke_op(method, 3, {en, 1, ARRAY_ADDR, OUT3_ADDR}), com::S_OK);
    EXPECT_EQ(ptr(ARRAY_ADDR), A);
    filters_[0].refs--;
    // Not in the graph any more: still S_OK, joined to NULL, not released.
    EXPECT_EQ(invoke(&ComCoreManager::filter_graph_remove_filter, {graph, B}), com::S_OK);
    EXPECT_EQ(filters_[1].refs, 1);
    EXPECT_EQ(filters_[1].joins, 3);
    EXPECT_EQ(invoke(&ComCoreManager::filter_graph_remove_filter, {graph, 0}), kEPointer);

    // Enumerators hold graph references; the last graph Release joins and
    // releases every remaining entry (A twice, C once).
    EXPECT_EQ(invoke_op(method, 2, {clone}), 0u);
    EXPECT_EQ(invoke_op(method, 2, {en}), 0u);
    EXPECT_EQ(invoke(&ComCoreManager::filter_graph_release, {graph}), 0u);
    EXPECT_EQ(filters_[0].refs, 1);
    EXPECT_EQ(filters_[2].refs, 1);
    EXPECT_EQ(filters_[0].joins, 4);
    EXPECT_EQ(filters_[0].graph, 0u);
}

TEST_P(FilterGraphBitnessTest, AddFilterGeneratedNamesFollowTheGraphCounter) {
    const GuestAddr graph = create_graph();
    // dshow_graph_names2 probe: "0001" by name, NULL -> "0002" (0001 taken),
    // "Z", "Z 0003", "Z" -> "Z 0004", NULL -> "0005".
    const std::u16string_view names[] = {u"0001", u"", u"Z", u"Z 0003", u"Z", u""};
    const char16_t* expected[] = {u"0001", u"0002", u"Z", u"Z 0003", u"Z 0004", u"0005"};
    const u32 hrs[] = {com::S_OK, com::S_OK, com::S_OK, com::S_OK, kVfwSDuplicateName,
                       com::S_OK};
    for (u32 i = 0; i < 6; ++i) {
        GuestAddr name = 0;
        if (!names[i].empty()) {
            name = NAME_ADDR + i * 0x40;
            put_wide(name, names[i]);
        }
        EXPECT_EQ(invoke(&ComCoreManager::filter_graph_add_filter,
                         {graph, filter_obj(i), name}),
                  hrs[i])
            << i;
        EXPECT_EQ(filters_[i].name, std::u16string(expected[i])) << i;
    }
    // A JoinFilterGraph failure is AddFilter's result; the filter is
    // released and not in the graph.
    filters_[6].join_hr = 0x80004005u;
    put_wide(NAME_ADDR + 0x300, u"bad");
    EXPECT_EQ(invoke(&ComCoreManager::filter_graph_add_filter,
                     {graph, filter_obj(6), NAME_ADDR + 0x300}),
              0x80004005u);
    EXPECT_EQ(filters_[6].refs, 1);
    EXPECT_EQ(invoke(&ComCoreManager::filter_graph_find_filter_by_name,
                     {graph, NAME_ADDR + 0x300, OUT3_ADDR}),
              kVfwENotFound);
}

TEST_P(FilterGraphBitnessTest, MediaEventParamsArePointerWidth) {
    const GuestAddr graph = create_graph();
    const GuestAddr sink = qi(graph, IID_IMEDIAEVENTSINK);
    const GuestAddr events = qi(graph, IID_IMEDIAEVENTEX);
    // Notify(EC_USER + 1, LONG_PTR, LONG_PTR); on x86 the LONG_PTRs are the
    // low dwords (measured: p1=55667788 p2=5060708).
    const u64 p1 = x64_ ? 0x1122334455667788ull : 0x55667788ull;
    const u64 p2 = x64_ ? 0x0102030405060708ull : 0x05060708ull;
    EXPECT_EQ(invoke(&ComCoreManager::filter_graph_media_event_sink_notify,
                     {sink, 0x8001, p1, p2}),
              com::S_OK);
    constexpr GuestAddr CODE = DATA_BASE + 0x400;
    constexpr GuestAddr P1 = DATA_BASE + 0x408;
    constexpr GuestAddr P2 = DATA_BASE + 0x410;
    (void)memory_.write_u64(P1, 0xCAFEBABECAFEBABEull);
    (void)memory_.write_u64(P2, 0xCAFEBABECAFEBABEull);
    ASSERT_EQ(invoke(&ComCoreManager::filter_graph_media_event_get_event,
                     {events, CODE, P1, P2, 0}),
              com::S_OK);
    EXPECT_EQ(memory_.read_u32(CODE).value_or(0), 0x8001u);
    EXPECT_EQ(ptr(P1), p1);
    EXPECT_EQ(ptr(P2), p2);
    if (!x64_) {
        // A 4-byte LONG_PTR: the neighbouring dword is untouched.
        EXPECT_EQ(memory_.read_u32(P1 + 4).value_or(0), 0xCAFEBABEu);
    }
    // Empty queue: E_ABORT, all three outputs zeroed.
    (void)memory_.write_u32(CODE, 7);
    (void)memory_.write_u64(P1, ~0ull);
    (void)memory_.write_u64(P2, ~0ull);
    EXPECT_EQ(invoke(&ComCoreManager::filter_graph_media_event_get_event,
                     {events, CODE, P1, P2, 0}),
              kEAbort);
    EXPECT_EQ(memory_.read_u32(CODE).value_or(1), 0u);
    EXPECT_EQ(ptr(P1), 0u);
    EXPECT_EQ(ptr(P2), 0u);

    // GetEventHandle(OAEVENT*): OAEVENT is a LONG_PTR.
    (void)memory_.write_u64(P1, 0xCAFEBABECAFEBABEull);
    EXPECT_EQ(invoke(&ComCoreManager::filter_graph_media_event_get_event_handle,
                     {events, P1}),
              com::S_OK);
    EXPECT_NE(ptr(P1), 0u);
    EXPECT_EQ(memory_.read_u32(P1 + 4).value_or(1), x64_ ? 0u : 0xCAFEBABEu);
}

TEST_P(FilterGraphBitnessTest, MediaPositionReftimeArgumentFollowsTheAbi) {
    // put_Rate(REFTIME): a double by value - XMM1 on x64 (RDX is junk),
    // two stack dwords on x86. get_Rate(REFTIME*) writes 8 bytes.
    const GuestAddr graph = create_graph();
    const GuestAddr position = qi(graph, IID_IMEDIAPOSITION);
    const double rate = 1.5;
    u64 bits = 0;
    std::memcpy(&bits, &rate, sizeof(bits));
    if (x64_) {
        put_args({position, bits}, 1);
    } else {
        put_args({position, bits & 0xFFFFFFFFull, bits >> 32});
    }
    {
        auto m = make_marshaler();
        EXPECT_EQ(mgr_.filter_graph_media_position(m, 7), com::S_OK);
    }
    constexpr GuestAddr RATE = DATA_BASE + 0x500;
    (void)memory_.write_u64(RATE, 0);
    EXPECT_EQ(invoke_op(&ComCoreManager::filter_graph_media_position, 8, {position, RATE}),
              com::S_OK);
    EXPECT_EQ(memory_.read_u64(RATE).value_or(0), bits);
}

INSTANTIATE_TEST_SUITE_P(Bitness, FilterGraphBitnessTest, ::testing::Values(false, true),
                         [](const ::testing::TestParamInfo<bool>& info) {
                             return info.param ? "x64" : "x86";
                         });
