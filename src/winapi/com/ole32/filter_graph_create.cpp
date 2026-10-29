// SPDX-License-Identifier: LGPL-2.1-only
/// @file filter_graph_create.cpp
/// @brief CoCreateInstance(CLSID_FilterGraph): builds the nine interface objects and
/// their vtables and registers the FilterGraphState.

#include "filter_graph_internal.hpp"

#include <array>
#include <mutex>
#include <string_view>

namespace vitreo::winapi {

namespace {

[[nodiscard]] bool filter_graph_iid_supported(const Guid& iid) {
    return guid_equal(iid, IID_IUNKNOWN) ||
           guid_equal(iid, IID_IGRAPHBUILDER) ||
           guid_equal(iid, IID_IFILTERGRAPH) ||
           guid_equal(iid, IID_IMEDIACONTROL) ||
           guid_equal(iid, IID_IBASICAUDIO) ||
           guid_equal(iid, IID_IMEDIAEVENT) ||
           guid_equal(iid, IID_IMEDIAEVENTEX) ||
           guid_equal(iid, IID_IMEDIASEEKING) ||
           guid_equal(iid, IID_IVIDEOWINDOW) ||
           guid_equal(iid, IID_IBASICVIDEO) ||
           guid_equal(iid, IID_IMEDIAPOSITION) ||
           guid_equal(iid, IID_IMEDIAEVENTSINK);
}

} // namespace

u32 filter_graph_co_create_instance(
    ComCoreManager& mgr, Marshaler& m, const Guid& iid) {
    if (mgr.m_thunks == nullptr) {
        return com::CLASS_E_CLASSNOTAVAILABLE;
    }
    if (!filter_graph_iid_supported(iid)) {
        return E_NOINTERFACE;
    }

    static constexpr std::array<std::string_view, 3> core_names{{
        "Vitreo_FilterGraph_QueryInterface",
        "Vitreo_FilterGraph_AddRef",
        "Vitreo_FilterGraph_Release",
    }};
    std::array<GuestAddr, 3> core{};
    if (!load_filter_graph_thunks(mgr.m_thunks, core_names, core)) {
        return com::CLASS_E_CLASSNOTAVAILABLE;
    }

    // IFilterGraph (8) + IGraphBuilder extras (7) after IUnknown — 18 slots.
    // Do not invent methods here: a shifted vtable makes RenderFile call the
    // wrong stub with the wrong stdcall pop count and corrupts the stack.
    static constexpr std::array<std::string_view, 15> graph_builder_names{{
        "Vitreo_FilterGraph_AddFilter",
        "Vitreo_FilterGraph_RemoveFilter",
        "Vitreo_FilterGraph_EnumFilters",
        "Vitreo_FilterGraph_FindFilterByName",
        "Vitreo_FilterGraph_ConnectDirect",
        "Vitreo_FilterGraph_Reconnect",
        "Vitreo_FilterGraph_Disconnect",
        "Vitreo_FilterGraph_SetDefaultSyncSource",
        "Vitreo_FilterGraph_Connect",
        "Vitreo_FilterGraph_Render",
        "Vitreo_FilterGraph_RenderFile",
        "Vitreo_FilterGraph_AddSourceFilter",
        "Vitreo_FilterGraph_SetLogFile",
        "Vitreo_FilterGraph_Abort",
        "Vitreo_FilterGraph_ShouldOperationContinue",
    }};
    std::array<GuestAddr, 15> graph_builder{};
    if (!load_filter_graph_thunks(mgr.m_thunks, graph_builder_names, graph_builder)) {
        return com::CLASS_E_CLASSNOTAVAILABLE;
    }

    static constexpr std::array<std::string_view, 4> dispatch_names{{
        "Vitreo_FilterGraph_Dispatch_GetTypeInfoCount",
        "Vitreo_FilterGraph_Dispatch_GetTypeInfo",
        "Vitreo_FilterGraph_Dispatch_GetIDsOfNames",
        "Vitreo_FilterGraph_Dispatch_Invoke",
    }};
    std::array<GuestAddr, 4> dispatch{};
    if (!load_filter_graph_thunks(mgr.m_thunks, dispatch_names, dispatch)) {
        return com::CLASS_E_CLASSNOTAVAILABLE;
    }

    static constexpr std::array<std::string_view, 9> media_control_names{{
        "Vitreo_FilterGraph_MediaControl_Run",
        "Vitreo_FilterGraph_MediaControl_Pause",
        "Vitreo_FilterGraph_MediaControl_Stop",
        "Vitreo_FilterGraph_MediaControl_GetState",
        "Vitreo_FilterGraph_MediaControl_RenderFile",
        "Vitreo_FilterGraph_MediaControl_AddSourceFilter",
        "Vitreo_FilterGraph_MediaControl_get_FilterCollection",
        "Vitreo_FilterGraph_MediaControl_get_RegFilterCollection",
        "Vitreo_FilterGraph_MediaControl_StopWhenReady",
    }};
    std::array<GuestAddr, 9> media_control{};
    if (!load_filter_graph_thunks(mgr.m_thunks, media_control_names, media_control)) {
        return com::CLASS_E_CLASSNOTAVAILABLE;
    }

    static constexpr std::array<std::string_view, 4> basic_audio_names{{
        "Vitreo_FilterGraph_BasicAudio_put_Volume",
        "Vitreo_FilterGraph_BasicAudio_get_Volume",
        "Vitreo_FilterGraph_BasicAudio_put_Balance",
        "Vitreo_FilterGraph_BasicAudio_get_Balance",
    }};
    std::array<GuestAddr, 4> basic_audio{};
    if (!load_filter_graph_thunks(mgr.m_thunks, basic_audio_names, basic_audio)) {
        return com::CLASS_E_CLASSNOTAVAILABLE;
    }

    static constexpr std::array<std::string_view, 9> media_event_names{{
        "Vitreo_FilterGraph_MediaEvent_GetEventHandle",
        "Vitreo_FilterGraph_MediaEvent_GetEvent",
        "Vitreo_FilterGraph_MediaEvent_WaitForCompletion",
        "Vitreo_FilterGraph_MediaEvent_CancelDefaultHandling",
        "Vitreo_FilterGraph_MediaEvent_RestoreDefaultHandling",
        "Vitreo_FilterGraph_MediaEvent_FreeEventParams",
        "Vitreo_FilterGraph_MediaEventEx_SetNotifyWindow",
        "Vitreo_FilterGraph_MediaEventEx_SetNotifyFlags",
        "Vitreo_FilterGraph_MediaEventEx_GetNotifyFlags",
    }};
    std::array<GuestAddr, 9> media_event{};
    if (!load_filter_graph_thunks(mgr.m_thunks, media_event_names, media_event)) {
        return com::CLASS_E_CLASSNOTAVAILABLE;
    }

    static constexpr std::array<std::string_view, 17> media_seeking_names{{
        "Vitreo_FilterGraph_MediaSeeking_GetCapabilities",
        "Vitreo_FilterGraph_MediaSeeking_CheckCapabilities",
        "Vitreo_FilterGraph_MediaSeeking_IsFormatSupported",
        "Vitreo_FilterGraph_MediaSeeking_QueryPreferredFormat",
        "Vitreo_FilterGraph_MediaSeeking_GetTimeFormat",
        "Vitreo_FilterGraph_MediaSeeking_IsUsingTimeFormat",
        "Vitreo_FilterGraph_MediaSeeking_SetTimeFormat",
        "Vitreo_FilterGraph_MediaSeeking_GetDuration",
        "Vitreo_FilterGraph_MediaSeeking_GetStopPosition",
        "Vitreo_FilterGraph_MediaSeeking_GetCurrentPosition",
        "Vitreo_FilterGraph_MediaSeeking_ConvertTimeFormat",
        "Vitreo_FilterGraph_MediaSeeking_SetPositions",
        "Vitreo_FilterGraph_MediaSeeking_GetPositions",
        "Vitreo_FilterGraph_MediaSeeking_GetAvailable",
        "Vitreo_FilterGraph_MediaSeeking_SetRate",
        "Vitreo_FilterGraph_MediaSeeking_GetRate",
        "Vitreo_FilterGraph_MediaSeeking_GetPreroll",
    }};
    std::array<GuestAddr, 17> media_seeking{};
    if (!load_filter_graph_thunks(mgr.m_thunks, media_seeking_names, media_seeking)) {
        return com::CLASS_E_CLASSNOTAVAILABLE;
    }

    std::array<GuestAddr, 39> video_window{};
    if (!load_filter_graph_thunks(mgr.m_thunks, kVideoWindowMethodNames,
                                 video_window)) {
        return com::CLASS_E_CLASSNOTAVAILABLE;
    }

    std::array<GuestAddr, 32> basic_video{};
    if (!load_filter_graph_thunks(mgr.m_thunks, kBasicVideoMethodNames,
                                 basic_video)) {
        return com::CLASS_E_CLASSNOTAVAILABLE;
    }

    std::array<GuestAddr, 11> media_position{};
    if (!load_filter_graph_thunks(mgr.m_thunks, kMediaPositionMethodNames,
                                 media_position)) {
        return com::CLASS_E_CLASSNOTAVAILABLE;
    }
    auto media_event_sink_notify = get_filter_graph_thunk(
        mgr.m_thunks, "Vitreo_FilterGraph_MediaEventSink_Notify");
    if (media_event_sink_notify.is_err()) {
        return com::CLASS_E_CLASSNOTAVAILABLE;
    }

    // Nine interface objects, each {vtable*} immediately followed by its
    // vtable, all pointer-width (8-byte slots on x64). Slot counts are the
    // SDK's (dshow_layout_probe: IGraphBuilder 18, IMediaControl 16,
    // IBasicAudio 11, IMediaEventEx 16, IMediaSeeking 20, IVideoWindow 46,
    // IBasicVideo 39, IMediaPosition 18, IMediaEventSink 4).
    const GuestAddr ps = guest_pointer_size(m);
    constexpr std::array<GuestAddr, 9> kSlots{18, 16, 11, 16, 20, 46, 39, 18, 4};
    GuestAddr total_slots = 0;
    for (const GuestAddr slots : kSlots) total_slots += 1 + slots;
    const GuestAddr block = mgr.reserve_com_scratch(total_slots * ps + 16u);
    if (block == 0) return E_OUTOFMEMORY;
    std::array<GuestAddr, 9> objects{};
    GuestAddr cursor = (block + 15u) & ~GuestAddr{15u};
    for (usize i = 0; i < kSlots.size(); ++i) {
        objects[i] = cursor;
        cursor += (1 + kSlots[i]) * ps;
    }
    const GuestAddr graph_builder_obj = objects[0];
    const GuestAddr graph_builder_vtbl = graph_builder_obj + ps;
    const GuestAddr media_control_obj = objects[1];
    const GuestAddr media_control_vtbl = media_control_obj + ps;
    const GuestAddr basic_audio_obj = objects[2];
    const GuestAddr basic_audio_vtbl = basic_audio_obj + ps;
    const GuestAddr media_event_obj = objects[3];
    const GuestAddr media_event_vtbl = media_event_obj + ps;
    const GuestAddr media_seeking_obj = objects[4];
    const GuestAddr media_seeking_vtbl = media_seeking_obj + ps;
    const GuestAddr video_window_obj = objects[5];
    const GuestAddr video_window_vtbl = video_window_obj + ps;
    const GuestAddr basic_video_obj = objects[6];
    const GuestAddr basic_video_vtbl = basic_video_obj + ps;
    const GuestAddr media_position_obj = objects[7];
    const GuestAddr media_position_vtbl = media_position_obj + ps;
    const GuestAddr media_event_sink_obj = objects[8];
    const GuestAddr media_event_sink_vtbl = media_event_sink_obj + ps;

    const u32 sid = mgr.m_next_filter_graph_id++;
    ComCoreManager::FilterGraphState st{};
    st.graph_builder_iface = graph_builder_obj;
    st.media_control_iface = media_control_obj;
    st.basic_audio_iface = basic_audio_obj;
    st.media_event_iface = media_event_obj;
    st.media_seeking_iface = media_seeking_obj;
    st.video_window_iface = video_window_obj;
    st.basic_video_iface = basic_video_obj;
    st.media_position_iface = media_position_obj;
    st.media_event_sink_iface = media_event_sink_obj;
    {
        std::lock_guard<std::mutex> lock(mgr.m_filter_graph_clock_mutex);
        std::lock_guard<std::mutex> map_lock(mgr.m_filter_graph_map_mutex);
        mgr.m_filter_graphs[sid] = st;
    }
    graph_diag("create sid=%u iface=0x%llx\n", sid,
               static_cast<unsigned long long>(graph_builder_obj));
    {
    std::lock_guard<std::mutex> map_lock(mgr.m_filter_graph_map_mutex);
    mgr.m_iface_to_filter_graph[graph_builder_obj] = {
        sid, ComCoreManager::FilterGraphInterfaceKind::GraphBuilder};
    mgr.m_iface_to_filter_graph[media_control_obj] = {
        sid, ComCoreManager::FilterGraphInterfaceKind::MediaControl};
    mgr.m_iface_to_filter_graph[basic_audio_obj] = {
        sid, ComCoreManager::FilterGraphInterfaceKind::BasicAudio};
    mgr.m_iface_to_filter_graph[media_event_obj] = {
        sid, ComCoreManager::FilterGraphInterfaceKind::MediaEvent};
    mgr.m_iface_to_filter_graph[media_seeking_obj] = {
        sid, ComCoreManager::FilterGraphInterfaceKind::MediaSeeking};
    mgr.m_iface_to_filter_graph[video_window_obj] = {
        sid, ComCoreManager::FilterGraphInterfaceKind::VideoWindow};
    mgr.m_iface_to_filter_graph[basic_video_obj] = {
        sid, ComCoreManager::FilterGraphInterfaceKind::BasicVideo};
    mgr.m_iface_to_filter_graph[media_position_obj] = {
        sid, ComCoreManager::FilterGraphInterfaceKind::MediaPosition};
    mgr.m_iface_to_filter_graph[media_event_sink_obj] = {
        sid, ComCoreManager::FilterGraphInterfaceKind::MediaEventSink};
    }
    mgr.ensure_filter_graph_event(mgr.m_filter_graphs[sid]);

    for (const GuestAddr object : objects) {
        (void)m.write_ptr(object, object + ps);
    }

    std::array<GuestAddr, 18> graph_builder_vtable{};
    graph_builder_vtable[0] = core[0];
    graph_builder_vtable[1] = core[1];
    graph_builder_vtable[2] = core[2];
    for (usize i = 0; i < graph_builder.size(); ++i) {
        graph_builder_vtable[3 + i] = graph_builder[i];
    }
    write_vtable(m, graph_builder_vtbl, graph_builder_vtable.data(),
                 graph_builder_vtable.size());

    std::array<GuestAddr, 16> media_control_vtable{};
    media_control_vtable[0] = core[0];
    media_control_vtable[1] = core[1];
    media_control_vtable[2] = core[2];
    for (usize i = 0; i < dispatch.size(); ++i) {
        media_control_vtable[3 + i] = dispatch[i];
    }
    for (usize i = 0; i < media_control.size(); ++i) {
        media_control_vtable[7 + i] = media_control[i];
    }
    write_vtable(m, media_control_vtbl, media_control_vtable.data(),
                 media_control_vtable.size());

    std::array<GuestAddr, 11> basic_audio_vtable{};
    basic_audio_vtable[0] = core[0];
    basic_audio_vtable[1] = core[1];
    basic_audio_vtable[2] = core[2];
    for (usize i = 0; i < dispatch.size(); ++i) {
        basic_audio_vtable[3 + i] = dispatch[i];
    }
    for (usize i = 0; i < basic_audio.size(); ++i) {
        basic_audio_vtable[7 + i] = basic_audio[i];
    }
    write_vtable(m, basic_audio_vtbl, basic_audio_vtable.data(),
                 basic_audio_vtable.size());

    std::array<GuestAddr, 16> media_event_vtable{};
    media_event_vtable[0] = core[0];
    media_event_vtable[1] = core[1];
    media_event_vtable[2] = core[2];
    for (usize i = 0; i < dispatch.size(); ++i) {
        media_event_vtable[3 + i] = dispatch[i];
    }
    for (usize i = 0; i < media_event.size(); ++i) {
        media_event_vtable[7 + i] = media_event[i];
    }
    write_vtable(m, media_event_vtbl, media_event_vtable.data(),
                 media_event_vtable.size());

    std::array<GuestAddr, 20> media_seeking_vtable{};
    media_seeking_vtable[0] = core[0];
    media_seeking_vtable[1] = core[1];
    media_seeking_vtable[2] = core[2];
    for (usize i = 0; i < media_seeking.size(); ++i) {
        media_seeking_vtable[3 + i] = media_seeking[i];
    }
    write_vtable(m, media_seeking_vtbl, media_seeking_vtable.data(),
                 media_seeking_vtable.size());

    std::array<GuestAddr, 46> video_window_vtable{};
    video_window_vtable[0] = core[0];
    video_window_vtable[1] = core[1];
    video_window_vtable[2] = core[2];
    for (usize i = 0; i < dispatch.size(); ++i) {
        video_window_vtable[3 + i] = dispatch[i];
    }
    for (usize i = 0; i < video_window.size(); ++i) {
        video_window_vtable[7 + i] = video_window[i];
    }
    write_vtable(m, video_window_vtbl, video_window_vtable.data(),
                 video_window_vtable.size());

    std::array<GuestAddr, 39> basic_video_vtable{};
    basic_video_vtable[0] = core[0];
    basic_video_vtable[1] = core[1];
    basic_video_vtable[2] = core[2];
    for (usize i = 0; i < dispatch.size(); ++i) {
        basic_video_vtable[3 + i] = dispatch[i];
    }
    for (usize i = 0; i < basic_video.size(); ++i) {
        basic_video_vtable[7 + i] = basic_video[i];
    }
    write_vtable(m, basic_video_vtbl, basic_video_vtable.data(),
                 basic_video_vtable.size());

    std::array<GuestAddr, 18> media_position_vtable{};
    media_position_vtable[0] = core[0];
    media_position_vtable[1] = core[1];
    media_position_vtable[2] = core[2];
    for (usize i = 0; i < dispatch.size(); ++i) {
        media_position_vtable[3 + i] = dispatch[i];
    }
    for (usize i = 0; i < media_position.size(); ++i) {
        media_position_vtable[7 + i] = media_position[i];
    }
    write_vtable(m, media_position_vtbl, media_position_vtable.data(),
                 media_position_vtable.size());

    std::array<GuestAddr, 4> media_event_sink_vtable{};
    media_event_sink_vtable[0] = core[0];
    media_event_sink_vtable[1] = core[1];
    media_event_sink_vtable[2] = core[2];
    media_event_sink_vtable[3] = media_event_sink_notify.value();
    write_vtable(m, media_event_sink_vtbl, media_event_sink_vtable.data(),
                 media_event_sink_vtable.size());

    const GuestAddr out_iface = mgr.resolve_filter_graph_iface(st, iid);
    const GuestAddr ppv_ptr = m.arg_ptr(4);
    (void)m.write_ptr(ppv_ptr, out_iface);
    VITREO_TRACE(
        "[ole32] CoCreateInstance: FilterGraph graph=0x%llx media=0x%llx "
        "audio=0x%llx out=0x%llx\n",
        static_cast<unsigned long long>(graph_builder_obj),
        static_cast<unsigned long long>(media_control_obj),
        static_cast<unsigned long long>(basic_audio_obj),
        static_cast<unsigned long long>(out_iface));
    return com::S_OK;
}

} // namespace vitreo::winapi
