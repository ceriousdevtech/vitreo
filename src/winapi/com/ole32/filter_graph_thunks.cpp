// SPDX-License-Identifier: LGPL-2.1-only
/// @file filter_graph_thunks.cpp
/// @brief Guest-visible ole32 export table for the Filter Graph Manager interfaces
/// (append_filter_graph_thunks).

#include "filter_graph_internal.hpp"
#include "winapi/com/ole32/dshow_host.hpp"

#include <array>
#include <string>
#include <string_view>

namespace vitreo::winapi {

void append_filter_graph_thunks(DllExports& exports, ComCoreManager& mgr) {
    // Host DirectShow objects (File Source, DirectSound Renderer, allocator,
    // clock): internal thunks for every interface slot.
    mgr.dshow_host().append_thunks(exports);
    exports.entries.push_back(ExportEntry{
        "Vitreo_FilterGraph_QueryInterface", 0,
        [&mgr](Marshaler& m) {
            m.set_return(mgr.filter_graph_query_interface(m));
        },
        12});
    exports.entries.push_back(ExportEntry{
        "Vitreo_FilterGraph_AddRef", 0,
        [&mgr](Marshaler& m) { m.set_return(mgr.filter_graph_add_ref(m)); },
        4});
    exports.entries.push_back(ExportEntry{
        "Vitreo_FilterGraph_Release", 0,
        [&mgr](Marshaler& m) { m.set_return(mgr.filter_graph_release(m)); },
        4});

    // IGraphBuilder / IFilterGraph methods (IUnknown slots handled above).
    exports.entries.push_back(ExportEntry{
        "Vitreo_FilterGraph_AddFilter", 0,
        [&mgr](Marshaler& m) {
            m.set_return(mgr.filter_graph_add_filter(m));
        }, 12});
    exports.entries.push_back(ExportEntry{
        "Vitreo_FilterGraph_RemoveFilter", 0,
        [&mgr](Marshaler& m) {
            m.set_return(mgr.filter_graph_remove_filter(m));
        }, 8});
    exports.entries.push_back(ExportEntry{
        "Vitreo_FilterGraph_EnumFilters", 0,
        [&mgr](Marshaler& m) {
            m.set_return(mgr.filter_graph_enum_filters(m));
        },
        8});
    exports.entries.push_back(ExportEntry{
        "Vitreo_FilterGraph_FindFilterByName", 0,
        [&mgr](Marshaler& m) {
            m.set_return(mgr.filter_graph_find_filter_by_name(m));
        },
        12});
    exports.entries.push_back(ExportEntry{
        "Vitreo_FilterGraph_ConnectDirect", 0,
        [&mgr](Marshaler& m) { m.set_return(mgr.filter_graph_connect_direct(m)); }, 16});
    exports.entries.push_back(ExportEntry{
        "Vitreo_FilterGraph_Reconnect", 0,
        // Reconnect(IPin*): Learn calls it obsolete (IFilterGraph2::ReconnectEx);
        // it schedules a reconnection on a worker thread Vitreo does not run.
        [](Marshaler& m) {
            (void)m;
            VITREO_QUARTZ_UNIMPLEMENTED_ONCE("IFilterGraph::Reconnect: deferred reconnection");
            m.set_return(E_NOTIMPL);
        }, 8});
    exports.entries.push_back(ExportEntry{
        "Vitreo_FilterGraph_Disconnect", 0,
        [&mgr](Marshaler& m) { m.set_return(mgr.filter_graph_disconnect(m)); }, 8});
    exports.entries.push_back(ExportEntry{
        "Vitreo_FilterGraph_SetDefaultSyncSource", 0,
        [&mgr](Marshaler& m) { m.set_return(mgr.filter_graph_set_default_sync_source(m)); }, 4});
    exports.entries.push_back(ExportEntry{
        "Vitreo_FilterGraph_Connect", 0,
        [&mgr](Marshaler& m) { m.set_return(mgr.filter_graph_connect(m)); }, 12});
    exports.entries.push_back(ExportEntry{
        "Vitreo_FilterGraph_Render", 0,
        [&mgr](Marshaler& m) { m.set_return(mgr.filter_graph_render(m)); }, 8});
    exports.entries.push_back(ExportEntry{
        "Vitreo_FilterGraph_RenderFile", 0,
        [&mgr](Marshaler& m) {
            m.set_return(mgr.filter_graph_render_file(m));
        },
        12});
    exports.entries.push_back(ExportEntry{
        "Vitreo_FilterGraph_AddSourceFilter", 0,
        // AddSourceFilter(LPCWSTR, LPCWSTR, IBaseFilter** ppFilter): a File
        // Source (Async) for the file.
        [&mgr](Marshaler& m) { m.set_return(mgr.filter_graph_add_source_filter(m)); },
        16});
    exports.entries.push_back(ExportEntry{
        "Vitreo_FilterGraph_SetLogFile", 0,
        [](Marshaler& m) { m.set_return(com::S_OK); }, 8});
    exports.entries.push_back(ExportEntry{
        "Vitreo_FilterGraph_Abort", 0,
        [](Marshaler& m) { m.set_return(com::S_OK); }, 4});
    exports.entries.push_back(ExportEntry{
        "Vitreo_FilterGraph_ShouldOperationContinue", 0,
        [](Marshaler& m) { m.set_return(com::S_OK); }, 4});

    // IDispatch stubs shared by IMediaControl / IBasicAudio / IMediaEvent.
    exports.entries.push_back(ExportEntry{
        "Vitreo_FilterGraph_Dispatch_GetTypeInfoCount", 0,
        [](Marshaler& m) {
            if (m.arg_ptr(1) != 0) (void)m.write_u32(m.arg_ptr(1), 0);
            m.set_return(com::S_OK);
        },
        8});
    exports.entries.push_back(ExportEntry{
        "Vitreo_FilterGraph_Dispatch_GetTypeInfo", 0,
        [](Marshaler& m) { m.set_return(E_NOTIMPL); }, 16});
    exports.entries.push_back(ExportEntry{
        "Vitreo_FilterGraph_Dispatch_GetIDsOfNames", 0,
        // IDispatch stdcall sizes from the SDK (dshow_method_table.inc:
        // GetIDsOfNames 24, Invoke 36).
        [](Marshaler& m) { m.set_return(E_NOTIMPL); }, 24});
    exports.entries.push_back(ExportEntry{
        "Vitreo_FilterGraph_Dispatch_Invoke", 0,
        [](Marshaler& m) { m.set_return(E_NOTIMPL); }, 36});

    exports.entries.push_back(ExportEntry{
        "Vitreo_FilterGraph_MediaControl_Run", 0,
        [&mgr](Marshaler& m) {
            m.set_return(mgr.filter_graph_media_control_run(m));
        },
        4});
    exports.entries.push_back(ExportEntry{
        "Vitreo_FilterGraph_MediaControl_Pause", 0,
        [&mgr](Marshaler& m) {
            m.set_return(mgr.filter_graph_media_control_pause(m));
        }, 4});
    exports.entries.push_back(ExportEntry{
        "Vitreo_FilterGraph_MediaControl_Stop", 0,
        [&mgr](Marshaler& m) {
            m.set_return(mgr.filter_graph_media_control_stop(m));
        }, 4});
    exports.entries.push_back(ExportEntry{
        "Vitreo_FilterGraph_MediaControl_GetState", 0,
        [&mgr](Marshaler& m) {
            m.set_return(mgr.filter_graph_media_control_get_state(m));
        },
        12});
    exports.entries.push_back(ExportEntry{
        "Vitreo_FilterGraph_MediaControl_RenderFile", 0,
        [&mgr](Marshaler& m) {
            m.set_return(mgr.filter_graph_render_file(m));
        },
        12});
    exports.entries.push_back(ExportEntry{
        "Vitreo_FilterGraph_MediaControl_AddSourceFilter", 0,
        // AddSourceFilter(BSTR strFilename, IDispatch** ppUnk).
        [](Marshaler& m) {
            if (m.arg_ptr(2) != 0) (void)m.write_ptr(m.arg_ptr(2), 0);
            VITREO_QUARTZ_UNIMPLEMENTED_ONCE(
                "IMediaControl::AddSourceFilter: no IFilterInfo automation object");
            m.set_return(E_NOTIMPL);
        },
        12});
    exports.entries.push_back(ExportEntry{
        "Vitreo_FilterGraph_MediaControl_get_FilterCollection", 0,
        // get_FilterCollection(IDispatch** ppUnk).
        [](Marshaler& m) {
            if (m.arg_ptr(1) != 0) (void)m.write_ptr(m.arg_ptr(1), 0);
            VITREO_QUARTZ_UNIMPLEMENTED_ONCE(
                "IMediaControl::get_FilterCollection: no automation collection");
            m.set_return(E_NOTIMPL);
        },
        8});
    exports.entries.push_back(ExportEntry{
        "Vitreo_FilterGraph_MediaControl_get_RegFilterCollection", 0,
        // get_RegFilterCollection(IDispatch** ppUnk).
        [](Marshaler& m) {
            if (m.arg_ptr(1) != 0) (void)m.write_ptr(m.arg_ptr(1), 0);
            VITREO_QUARTZ_UNIMPLEMENTED_ONCE(
                "IMediaControl::get_RegFilterCollection: no automation collection");
            m.set_return(E_NOTIMPL);
        },
        8});
    exports.entries.push_back(ExportEntry{
        "Vitreo_FilterGraph_MediaControl_StopWhenReady", 0,
        [](Marshaler& m) { m.set_return(com::S_OK); }, 4});

    exports.entries.push_back(ExportEntry{
        "Vitreo_FilterGraph_BasicAudio_get_Balance", 0,
        [&mgr](Marshaler& m) {
            m.set_return(mgr.filter_graph_basic_audio_get_balance(m));
        },
        8});
    exports.entries.push_back(ExportEntry{
        "Vitreo_FilterGraph_BasicAudio_put_Balance", 0,
        [&mgr](Marshaler& m) {
            m.set_return(mgr.filter_graph_basic_audio_put_balance(m));
        },
        8});
    exports.entries.push_back(ExportEntry{
        "Vitreo_FilterGraph_BasicAudio_get_Volume", 0,
        [&mgr](Marshaler& m) {
            m.set_return(mgr.filter_graph_basic_audio_get_volume(m));
        },
        8});
    exports.entries.push_back(ExportEntry{
        "Vitreo_FilterGraph_BasicAudio_put_Volume", 0,
        [&mgr](Marshaler& m) {
            m.set_return(mgr.filter_graph_basic_audio_put_volume(m));
        },
        8});

    exports.entries.push_back(ExportEntry{
        "Vitreo_FilterGraph_MediaEvent_GetEventHandle", 0,
        [&mgr](Marshaler& m) {
            m.set_return(mgr.filter_graph_media_event_get_event_handle(m));
        },
        8});
    exports.entries.push_back(ExportEntry{
        "Vitreo_FilterGraph_MediaEvent_GetEvent", 0,
        [&mgr](Marshaler& m) {
            m.set_return(mgr.filter_graph_media_event_get_event(m));
        },
        20});
    exports.entries.push_back(ExportEntry{
        "Vitreo_FilterGraph_MediaEvent_WaitForCompletion", 0,
        [&mgr](Marshaler& m) {
            m.set_return(mgr.filter_graph_media_event_wait_for_completion(m));
        },
        12});
    exports.entries.push_back(ExportEntry{
        "Vitreo_FilterGraph_MediaEvent_CancelDefaultHandling", 0,
        [&mgr](Marshaler& m) {
            m.set_return(mgr.filter_graph_media_event_default_handling(m, false));
        }, 8});
    exports.entries.push_back(ExportEntry{
        "Vitreo_FilterGraph_MediaEvent_RestoreDefaultHandling", 0,
        [&mgr](Marshaler& m) {
            m.set_return(mgr.filter_graph_media_event_default_handling(m, true));
        }, 8});
    exports.entries.push_back(ExportEntry{
        "Vitreo_FilterGraph_MediaEvent_FreeEventParams", 0,
        [](Marshaler& m) { m.set_return(com::S_OK); }, 16});
    exports.entries.push_back(ExportEntry{
        "Vitreo_FilterGraph_MediaEventEx_SetNotifyWindow", 0,
        [&mgr](Marshaler& m) {
            m.set_return(mgr.filter_graph_media_event_set_notify_window(m));
        },
        16});
    exports.entries.push_back(ExportEntry{
        "Vitreo_FilterGraph_MediaEventEx_SetNotifyFlags", 0,
        [&mgr](Marshaler& m) {
            m.set_return(mgr.filter_graph_media_event_set_notify_flags(m));
        },
        8});
    exports.entries.push_back(ExportEntry{
        "Vitreo_FilterGraph_MediaEventEx_GetNotifyFlags", 0,
        [&mgr](Marshaler& m) {
            m.set_return(mgr.filter_graph_media_event_get_notify_flags(m));
        },
        8});
    exports.entries.push_back(ExportEntry{
        "Vitreo_FilterGraph_MediaEventSink_Notify", 0,
        [&mgr](Marshaler& m) {
            m.set_return(mgr.filter_graph_media_event_sink_notify(m));
        },
        16});

    exports.entries.push_back(ExportEntry{
        "Vitreo_FilterGraph_MediaSeeking_GetCapabilities", 0,
        [](Marshaler& m) {
            // strmif.h AM_SEEKING_SEEKING_CAPABILITIES: duration, position,
            // and absolute/forward/back seek.
            constexpr u32 kCaps = 0x01u | 0x02u | 0x04u | 0x08u | 0x10u | 0x20u;
            if (m.arg_ptr(1) != 0) (void)m.write_u32(m.arg_ptr(1), kCaps);
            m.set_return(com::S_OK);
        },
        8});
    exports.entries.push_back(ExportEntry{
        "Vitreo_FilterGraph_MediaSeeking_CheckCapabilities", 0,
        [](Marshaler& m) { m.set_return(com::S_OK); }, 8});
    exports.entries.push_back(ExportEntry{
        "Vitreo_FilterGraph_MediaSeeking_IsFormatSupported", 0,
        [](Marshaler& m) {
            const GuestAddr guid = m.arg_ptr(1);
            if (guid == 0) {
                m.set_return(E_POINTER);
                return;
            }
            const u32 d1 = m.memory().read_u32(guid).value_or(0);
            const u16 d2 = m.memory().read_u16(guid + 4).value_or(0);
            const u16 d3 = m.memory().read_u16(guid + 6).value_or(0);
            // TIME_FORMAT_MEDIA_TIME {7B785574-8C82-11CF-BC0C-00AA00AC74F6}
            const bool media_time = d1 == 0x7B785574u && d2 == 0x8C82u &&
                                    d3 == 0x11CFu;
            m.set_return(media_time ? com::S_OK : com::S_FALSE);
        },
        8});
    exports.entries.push_back(ExportEntry{
        "Vitreo_FilterGraph_MediaSeeking_QueryPreferredFormat", 0,
        // QueryPreferredFormat(GUID*): the graph's only format is
        // TIME_FORMAT_MEDIA_TIME (16-byte GUID out).
        [](Marshaler& m) {
            if (m.arg_ptr(1) == 0) {
                m.set_return(E_POINTER);
                return;
            }
            (void)m.write_struct<Guid>(m.arg_ptr(1), TIME_FORMAT_MEDIA_TIME);
            m.set_return(com::S_OK);
        },
        8});
    exports.entries.push_back(ExportEntry{
        "Vitreo_FilterGraph_MediaSeeking_GetTimeFormat", 0,
        // GetTimeFormat(GUID*).
        [](Marshaler& m) {
            if (m.arg_ptr(1) == 0) {
                m.set_return(E_POINTER);
                return;
            }
            (void)m.write_struct<Guid>(m.arg_ptr(1), TIME_FORMAT_MEDIA_TIME);
            m.set_return(com::S_OK);
        },
        8});
    exports.entries.push_back(ExportEntry{
        "Vitreo_FilterGraph_MediaSeeking_IsUsingTimeFormat", 0,
        // IsUsingTimeFormat(const GUID*): this + 1 pointer = 8 stdcall bytes.
        [](Marshaler& m) {
            const GuestAddr guid = m.arg_ptr(1);
            if (guid == 0) {
                m.set_return(E_POINTER);
                return;
            }
            auto format = m.read_struct<Guid>(guid);
            m.set_return(format.is_ok() &&
                                 guid_equal(format.value(), TIME_FORMAT_MEDIA_TIME)
                             ? com::S_OK
                             : com::S_FALSE);
        },
        8});
    exports.entries.push_back(ExportEntry{
        "Vitreo_FilterGraph_MediaSeeking_SetTimeFormat", 0,
        [](Marshaler& m) { m.set_return(com::S_OK); }, 8});
    exports.entries.push_back(ExportEntry{
        "Vitreo_FilterGraph_MediaSeeking_GetDuration", 0,
        [&mgr](Marshaler& m) {
            m.set_return(mgr.filter_graph_media_seeking_get_duration(m));
        },
        8});
    exports.entries.push_back(ExportEntry{
        "Vitreo_FilterGraph_MediaSeeking_GetStopPosition", 0,
        [&mgr](Marshaler& m) {
            m.set_return(mgr.filter_graph_media_seeking_get_stop_position(m));
        },
        8});
    exports.entries.push_back(ExportEntry{
        "Vitreo_FilterGraph_MediaSeeking_GetCurrentPosition", 0,
        [&mgr](Marshaler& m) {
            m.set_return(
                mgr.filter_graph_media_seeking_get_current_position(m));
        },
        8});
    exports.entries.push_back(ExportEntry{
        "Vitreo_FilterGraph_MediaSeeking_ConvertTimeFormat", 0,
        // ConvertTimeFormat(LONGLONG* pTarget, const GUID* pTargetFormat,
        // LONGLONG Source, const GUID* pSourceFormat). Source is one slot on
        // x64 and two stack dwords (low, high) on x86. NULL formats mean the
        // current one, which is always TIME_FORMAT_MEDIA_TIME here.
        [](Marshaler& m) {
            const bool x64 = m.cpu_is_64bit();
            const GuestAddr target = m.arg_ptr(1);
            const GuestAddr target_format = m.arg_ptr(2);
            const u64 source = x64 ? m.arg(3)
                                   : ((m.arg(3) & 0xFFFFFFFFull) | (m.arg(4) << 32));
            const GuestAddr source_format = m.arg_ptr(x64 ? 4 : 5);
            if (target == 0) {
                m.set_return(E_POINTER);
                return;
            }
            const auto is_media_time = [&](GuestAddr format) {
                if (format == 0) return true;
                auto guid = m.read_struct<Guid>(format);
                return guid.is_ok() && guid_equal(guid.value(), TIME_FORMAT_MEDIA_TIME);
            };
            if (!is_media_time(target_format) || !is_media_time(source_format)) {
                VITREO_QUARTZ_UNIMPLEMENTED_ONCE(
                    "IMediaSeeking::ConvertTimeFormat: formats other than "
                    "TIME_FORMAT_MEDIA_TIME");
                m.set_return(E_NOTIMPL);
                return;
            }
            (void)m.write_u64(target, source);
            m.set_return(com::S_OK);
        },
        24});
    exports.entries.push_back(ExportEntry{
        "Vitreo_FilterGraph_MediaSeeking_SetPositions", 0,
        // this + pCurrent + dwCurrentFlags + pStop + dwStopFlags
        [&mgr](Marshaler& m) {
            m.set_return(mgr.filter_graph_media_seeking_set_positions(m));
        }, 20});
    exports.entries.push_back(ExportEntry{
        "Vitreo_FilterGraph_MediaSeeking_GetPositions", 0,
        [](Marshaler& m) { m.set_return(com::S_OK); }, 12});
    exports.entries.push_back(ExportEntry{
        "Vitreo_FilterGraph_MediaSeeking_GetAvailable", 0,
        [](Marshaler& m) { m.set_return(com::S_OK); }, 12});
    exports.entries.push_back(ExportEntry{
        "Vitreo_FilterGraph_MediaSeeking_SetRate", 0,
        // SetRate(double): this + an 8-byte double = 12 stdcall bytes.
        [](Marshaler& m) { m.set_return(com::S_OK); }, 12});
    exports.entries.push_back(ExportEntry{
        "Vitreo_FilterGraph_MediaSeeking_GetRate", 0,
        [](Marshaler& m) {
            // GetRate(double* pdRate): an 8-byte IEEE double.
            if (m.arg_ptr(1) == 0) {
                m.set_return(E_POINTER);
                return;
            }
            const double rate = 1.0;
            u64 bits = 0;
            std::memcpy(&bits, &rate, sizeof(bits));
            (void)m.write_u64(m.arg_ptr(1), bits);
            m.set_return(com::S_OK);
        },
        8});
    exports.entries.push_back(ExportEntry{
        "Vitreo_FilterGraph_MediaSeeking_GetPreroll", 0,
        [](Marshaler& m) {
            if (m.arg_ptr(1) != 0) (void)m.write_u64(m.arg_ptr(1), 0);
            m.set_return(com::S_OK);
        },
        8});

    // On a graph built from its filters every IMediaSeeking method goes to
    // the renderers (filter_graph_seeking.cpp); the handlers above serve the
    // host-decode graphs.
    {
        static constexpr std::string_view kSeekingPrefix = "Vitreo_FilterGraph_MediaSeeking_";
        static constexpr std::array<std::string_view, 17> kSeekingOrder{{
            "GetCapabilities", "CheckCapabilities", "IsFormatSupported", "QueryPreferredFormat",
            "GetTimeFormat", "IsUsingTimeFormat", "SetTimeFormat", "GetDuration",
            "GetStopPosition", "GetCurrentPosition", "ConvertTimeFormat", "SetPositions",
            "GetPositions", "GetAvailable", "SetRate", "GetRate", "GetPreroll"}};
        for (auto& entry : exports.entries) {
            const std::string_view name = entry.name;
            if (name.substr(0, kSeekingPrefix.size()) != kSeekingPrefix) continue;
            const std::string_view method = name.substr(kSeekingPrefix.size());
            for (u32 i = 0; i < kSeekingOrder.size(); ++i) {
                if (kSeekingOrder[i] != method) continue;
                entry.handler = [inner = entry.handler, &mgr, slot = 3u + i](Marshaler& m) {
                    u32 result = 0;
                    if (mgr.intelligent_seeking(m, slot, result)) {
                        m.set_return(result);
                        return;
                    }
                    inner(m);
                };
            }
        }
    }

    for (u32 i = 0; i < kVideoWindowMethodNames.size(); ++i) {
        const u32 method = i;
        exports.entries.push_back(ExportEntry{
            std::string(kVideoWindowMethodNames[i]), 0,
            [&mgr, method](Marshaler& m) {
                m.set_return(mgr.filter_graph_video_window(m, method));
            },
            kVideoWindowStdcallBytes[i]});
    }

    for (u32 i = 0; i < kBasicVideoMethodNames.size(); ++i) {
        const u32 method = i;
        exports.entries.push_back(ExportEntry{
            std::string(kBasicVideoMethodNames[i]), 0,
            [&mgr, method](Marshaler& m) {
                m.set_return(mgr.filter_graph_basic_video(m, method));
            },
            kBasicVideoStdcallBytes[i]});
    }

    for (u32 i = 0; i < kMediaPositionMethodNames.size(); ++i) {
        const u32 method = i;
        exports.entries.push_back(ExportEntry{
            std::string(kMediaPositionMethodNames[i]), 0,
            [&mgr, method](Marshaler& m) {
                m.set_return(mgr.filter_graph_media_position(m, method));
            },
            kMediaPositionStdcallBytes[i]});
    }

    for (u32 i = 0; i < kOutputPinMethodNames.size(); ++i) {
        const u32 method = i;
        exports.entries.push_back(ExportEntry{
            std::string(kOutputPinMethodNames[i]), 0,
            [&mgr, method](Marshaler& m) {
                m.set_return(mgr.filter_graph_output_pin(m, method));
            },
            kOutputPinStdcallBytes[i]});
    }

    // Guest thread bodies the graph starts itself (internal: not exported
    // by name to the guest). ThreadProc(LPVOID) = 4 stdcall bytes.
    {
        ExportEntry stream{"Vitreo_FilterGraph_StreamThread", 0,
                           [&mgr](Marshaler& m) {
                               m.set_return(mgr.filter_graph_stream_thread(m));
                           },
                           4};
        stream.internal = true;
        exports.entries.push_back(std::move(stream));
        ExportEntry run{"Vitreo_FilterGraph_RunWorker", 0,
                        [&mgr](Marshaler& m) {
                            m.set_return(mgr.filter_graph_run_worker(m));
                        },
                        4};
        run.internal = true;
        exports.entries.push_back(std::move(run));
    }

    for (u32 i = 0; i < kEnumFiltersMethodNames.size(); ++i) {
        const u32 method = i;
        exports.entries.push_back(ExportEntry{
            std::string(kEnumFiltersMethodNames[i]), 0,
            [&mgr, method](Marshaler& m) {
                m.set_return(mgr.filter_graph_enum_filters_method(m, method));
            },
            kEnumFiltersStdcallBytes[i]});
    }

    for (u32 i = 0; i < kMediaSampleMethodNames.size(); ++i) {
        const u32 method = i;
        exports.entries.push_back(ExportEntry{
            std::string(kMediaSampleMethodNames[i]), 0,
            [&mgr, method](Marshaler& m) {
                m.set_return(mgr.filter_graph_media_sample(m, method));
            },
            kMediaSampleStdcallBytes[i]});
    }
}

} // namespace vitreo::winapi
