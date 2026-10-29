#pragma once
/// @file filter_graph.hpp
/// @brief DirectShow Filter Graph COM frontend over Vitreo's media pipeline.

#include "winapi/com/ole32/com_core.hpp"
#include "winapi/core/thunk_registry.hpp"

namespace vitreo::winapi {

/// CoCreateInstance path for CLSID_FilterGraph.
[[nodiscard]] u32 filter_graph_co_create_instance(
    ComCoreManager& mgr, Marshaler& m, const Guid& iid);

[[nodiscard]] u32 filter_graph_query_interface(ComCoreManager& mgr, Marshaler& m);
[[nodiscard]] u32 filter_graph_add_ref(ComCoreManager& mgr, Marshaler& m);
[[nodiscard]] u32 filter_graph_release(ComCoreManager& mgr, Marshaler& m);

[[nodiscard]] u32 filter_graph_render_file(ComCoreManager& mgr, Marshaler& m);
[[nodiscard]] u32 filter_graph_media_control_get_state(ComCoreManager& mgr, Marshaler& m);
[[nodiscard]] u32 filter_graph_media_control_pause(ComCoreManager& mgr, Marshaler& m);
[[nodiscard]] u32 filter_graph_media_control_stop(ComCoreManager& mgr, Marshaler& m);
[[nodiscard]] u32 filter_graph_basic_audio_get_volume(ComCoreManager& mgr, Marshaler& m);
[[nodiscard]] u32 filter_graph_basic_audio_put_volume(ComCoreManager& mgr, Marshaler& m);
[[nodiscard]] u32 filter_graph_basic_audio_get_balance(ComCoreManager& mgr, Marshaler& m);
[[nodiscard]] u32 filter_graph_basic_audio_put_balance(ComCoreManager& mgr, Marshaler& m);

void append_filter_graph_thunks(DllExports& exports, ComCoreManager& mgr);

} // namespace vitreo::winapi
