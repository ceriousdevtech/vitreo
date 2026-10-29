// SPDX-License-Identifier: LGPL-2.1-only
/// @file com_core_thunks_audio.cpp
/// @brief ole32 thunks for the MMDevice enumerator/endpoint and the WASAPI audio
/// client, render/capture and property-store objects.

#include "com_core.hpp"
#include "winapi/core/marshaler.hpp"
#include "com_core_thunks_internal.hpp"

namespace vitreo::winapi {

/// MMDevice enumerator, endpoint and audio-client vtable methods plus the
/// WASAPI service exports.
void append_com_audio_thunks(DllExports& exports, ComCoreManager& mgr) {
    exports.entries.push_back(ExportEntry{
        "Vitreo_MMDeviceEnumerator_QueryInterface", 0,
        [&mgr](Marshaler& m) {
            m.set_return(mgr.mmdevice_enumerator_query_interface(m));
        }, 12});
    exports.entries.push_back(ExportEntry{
        "Vitreo_MMDeviceEnumerator_AddRef", 0,
        [&mgr](Marshaler& m) {
            m.set_return(mgr.mmdevice_enumerator_add_ref(m));
        }, 4});
    exports.entries.push_back(ExportEntry{
        "Vitreo_MMDeviceEnumerator_Release", 0,
        [&mgr](Marshaler& m) {
            m.set_return(mgr.mmdevice_enumerator_release(m));
        }, 4});
    exports.entries.push_back(ExportEntry{
        "Vitreo_MMDeviceEnumerator_EnumAudioEndpoints", 0,
        [&mgr](Marshaler& m) {
            m.set_return(mgr.mmdevice_enumerator_enum_audio_endpoints(m));
        }, 16});
    exports.entries.push_back(ExportEntry{
        "Vitreo_MMDeviceEnumerator_GetDefaultAudioEndpoint", 0,
        [&mgr](Marshaler& m) {
            m.set_return(mgr.mmdevice_enumerator_get_default_audio_endpoint(m));
        }, 16});
    exports.entries.push_back(ExportEntry{
        "Vitreo_MMDeviceEnumerator_GetDevice", 0,
        [&mgr](Marshaler& m) {
            m.set_return(mgr.mmdevice_enumerator_get_device(m));
        }, 12});
    exports.entries.push_back(ExportEntry{
        "Vitreo_MMDeviceEnumerator_RegisterEndpointNotificationCallback", 0,
        [&mgr](Marshaler& m) {
            m.set_return(mgr.mmdevice_enumerator_register_notification(m));
        }, 8});
    exports.entries.push_back(ExportEntry{
        "Vitreo_MMDeviceEnumerator_UnregisterEndpointNotificationCallback", 0,
        [&mgr](Marshaler& m) {
            m.set_return(mgr.mmdevice_enumerator_unregister_notification(m));
        }, 8});
    exports.entries.push_back(ExportEntry{
        "Vitreo_MMDeviceCollection_QueryInterface", 0,
        [&mgr](Marshaler& m) {
            m.set_return(mgr.mmdevice_collection_query_interface(m));
        }, 12});
    exports.entries.push_back(ExportEntry{
        "Vitreo_MMDeviceCollection_AddRef", 0,
        [&mgr](Marshaler& m) {
            m.set_return(mgr.mmdevice_collection_add_ref(m));
        }, 4});
    exports.entries.push_back(ExportEntry{
        "Vitreo_MMDeviceCollection_Release", 0,
        [&mgr](Marshaler& m) {
            m.set_return(mgr.mmdevice_collection_release(m));
        }, 4});
    exports.entries.push_back(ExportEntry{
        "Vitreo_MMDeviceCollection_GetCount", 0,
        [&mgr](Marshaler& m) {
            m.set_return(mgr.mmdevice_collection_get_count(m));
        }, 8});
    exports.entries.push_back(ExportEntry{
        "Vitreo_MMDeviceCollection_Item", 0,
        [&mgr](Marshaler& m) {
            m.set_return(mgr.mmdevice_collection_item(m));
        }, 12});
    exports.entries.push_back(ExportEntry{
        "Vitreo_MMDevice_QueryInterface", 0,
        [&mgr](Marshaler& m) { m.set_return(mgr.mmdevice_query_interface(m)); }, 12});
    exports.entries.push_back(ExportEntry{
        "Vitreo_MMDevice_AddRef", 0,
        [&mgr](Marshaler& m) { m.set_return(mgr.mmdevice_add_ref(m)); }, 4});
    exports.entries.push_back(ExportEntry{
        "Vitreo_MMDevice_Release", 0,
        [&mgr](Marshaler& m) { m.set_return(mgr.mmdevice_release(m)); }, 4});
    exports.entries.push_back(ExportEntry{
        "Vitreo_MMDevice_Activate", 0,
        [&mgr](Marshaler& m) { m.set_return(mgr.mmdevice_activate(m)); }, 20});
    exports.entries.push_back(ExportEntry{
        "Vitreo_MMDevice_OpenPropertyStore", 0,
        [&mgr](Marshaler& m) { m.set_return(mgr.mmdevice_open_property_store(m)); }, 12});
    exports.entries.push_back(ExportEntry{
        "Vitreo_MMDevice_GetId", 0,
        [&mgr](Marshaler& m) { m.set_return(mgr.mmdevice_get_id(m)); }, 8});
    exports.entries.push_back(ExportEntry{
        "Vitreo_MMDevice_GetState", 0,
        [&mgr](Marshaler& m) { m.set_return(mgr.mmdevice_get_state(m)); }, 8});
    exports.entries.push_back(ExportEntry{
        "Vitreo_MMEndpoint_QueryInterface", 0,
        [&mgr](Marshaler& m) { m.set_return(mgr.mmendpoint_query_interface(m)); }, 12});
    exports.entries.push_back(ExportEntry{
        "Vitreo_MMEndpoint_AddRef", 0,
        [&mgr](Marshaler& m) { m.set_return(mgr.mmendpoint_add_ref(m)); }, 4});
    exports.entries.push_back(ExportEntry{
        "Vitreo_MMEndpoint_Release", 0,
        [&mgr](Marshaler& m) { m.set_return(mgr.mmendpoint_release(m)); }, 4});
    exports.entries.push_back(ExportEntry{
        "Vitreo_MMEndpoint_GetDataFlow", 0,
        [&mgr](Marshaler& m) { m.set_return(mgr.mmendpoint_get_data_flow(m)); }, 8});
    exports.entries.push_back({"Vitreo_AudioActivation_Worker", 0,
        [&mgr](Marshaler& m) { m.set_return(mgr.audio_activation_worker(m)); }, 4});
    exports.entries.push_back({"Vitreo_AudioActivation_QueryInterface", 0,
        [&mgr](Marshaler& m) { m.set_return(mgr.audio_activation_query_interface(m)); }, 12});
    exports.entries.push_back({"Vitreo_AudioActivation_AddRef", 0,
        [&mgr](Marshaler& m) { m.set_return(mgr.audio_activation_add_ref(m)); }, 4});
    exports.entries.push_back({"Vitreo_AudioActivation_Release", 0,
        [&mgr](Marshaler& m) { m.set_return(mgr.audio_activation_release(m)); }, 4});
    exports.entries.push_back({"Vitreo_AudioActivation_GetResult", 0,
        [&mgr](Marshaler& m) { m.set_return(mgr.audio_activation_get_result(m)); }, 12});
    exports.entries.push_back(ExportEntry{"Vitreo_AudioClient_QueryInterface", 0,
        [&mgr](Marshaler& m) { m.set_return(mgr.audio_client_query_interface(m)); }, 12});
    exports.entries.push_back(ExportEntry{"Vitreo_AudioClient_AddRef", 0,
        [&mgr](Marshaler& m) { m.set_return(mgr.audio_client_add_ref(m)); }, 4});
    exports.entries.push_back(ExportEntry{"Vitreo_AudioClient_Release", 0,
        [&mgr](Marshaler& m) { m.set_return(mgr.audio_client_release(m)); }, 4});
    exports.entries.push_back(ExportEntry{"Vitreo_AudioClient_Initialize", 0,
        [&mgr](Marshaler& m) { m.set_return(mgr.audio_client_initialize(m)); }, 36});
    exports.entries.push_back(ExportEntry{"Vitreo_AudioClient_GetBufferSize", 0,
        [&mgr](Marshaler& m) { m.set_return(mgr.audio_client_get_buffer_size(m)); }, 8});
    exports.entries.push_back(ExportEntry{"Vitreo_AudioClient_GetStreamLatency", 0,
        [&mgr](Marshaler& m) { m.set_return(mgr.audio_client_get_stream_latency(m)); }, 8});
    exports.entries.push_back(ExportEntry{"Vitreo_AudioClient_GetCurrentPadding", 0,
        [&mgr](Marshaler& m) { m.set_return(mgr.audio_client_get_current_padding(m)); }, 8});
    exports.entries.push_back(ExportEntry{"Vitreo_AudioClient_IsFormatSupported", 0,
        [&mgr](Marshaler& m) { m.set_return(mgr.audio_client_is_format_supported(m)); }, 16});
    exports.entries.push_back(ExportEntry{"Vitreo_AudioClient_GetMixFormat", 0,
        [&mgr](Marshaler& m) { m.set_return(mgr.audio_client_get_mix_format(m)); }, 8});
    exports.entries.push_back(ExportEntry{"Vitreo_AudioClient_GetDevicePeriod", 0,
        [&mgr](Marshaler& m) { m.set_return(mgr.audio_client_get_device_period(m)); }, 12});
    exports.entries.push_back(ExportEntry{"Vitreo_AudioClient_Start", 0,
        [&mgr](Marshaler& m) { m.set_return(mgr.audio_client_start(m)); }, 4});
    exports.entries.push_back(ExportEntry{"Vitreo_AudioClient_Stop", 0,
        [&mgr](Marshaler& m) { m.set_return(mgr.audio_client_stop(m)); }, 4});
    exports.entries.push_back(ExportEntry{"Vitreo_AudioClient_Reset", 0,
        [&mgr](Marshaler& m) { m.set_return(mgr.audio_client_reset(m)); }, 4});
    exports.entries.push_back(ExportEntry{"Vitreo_AudioClient_SetEventHandle", 0,
        [&mgr](Marshaler& m) { m.set_return(mgr.audio_client_set_event_handle(m)); }, 8});
    exports.entries.push_back(ExportEntry{"Vitreo_AudioClient_GetService", 0,
        [&mgr](Marshaler& m) { m.set_return(mgr.audio_client_get_service(m)); }, 12});
    exports.entries.push_back(ExportEntry{"Vitreo_AudioRenderClient_QueryInterface", 0,
        [&mgr](Marshaler& m) { m.set_return(mgr.audio_render_client_query_interface(m)); }, 12});
    exports.entries.push_back(ExportEntry{"Vitreo_AudioRenderClient_AddRef", 0,
        [&mgr](Marshaler& m) { m.set_return(mgr.audio_render_client_add_ref(m)); }, 4});
    exports.entries.push_back(ExportEntry{"Vitreo_AudioRenderClient_Release", 0,
        [&mgr](Marshaler& m) { m.set_return(mgr.audio_render_client_release(m)); }, 4});
    exports.entries.push_back(ExportEntry{"Vitreo_AudioRenderClient_GetBuffer", 0,
        [&mgr](Marshaler& m) { m.set_return(mgr.audio_render_client_get_buffer(m)); }, 12});
    exports.entries.push_back(ExportEntry{"Vitreo_AudioRenderClient_ReleaseBuffer", 0,
        [&mgr](Marshaler& m) { m.set_return(mgr.audio_render_client_release_buffer(m)); }, 12});
    exports.entries.push_back(ExportEntry{"Vitreo_AudioCaptureClient_QueryInterface", 0,
        [&mgr](Marshaler& m) { m.set_return(mgr.audio_capture_client_query_interface(m)); }, 12});
    exports.entries.push_back(ExportEntry{"Vitreo_AudioCaptureClient_AddRef", 0,
        [&mgr](Marshaler& m) { m.set_return(mgr.audio_capture_client_add_ref(m)); }, 4});
    exports.entries.push_back(ExportEntry{"Vitreo_AudioCaptureClient_Release", 0,
        [&mgr](Marshaler& m) { m.set_return(mgr.audio_capture_client_release(m)); }, 4});
    exports.entries.push_back(ExportEntry{"Vitreo_AudioCaptureClient_GetBuffer", 0,
        [&mgr](Marshaler& m) { m.set_return(mgr.audio_capture_client_get_buffer(m)); }, 24});
    exports.entries.push_back(ExportEntry{"Vitreo_AudioCaptureClient_ReleaseBuffer", 0,
        [&mgr](Marshaler& m) { m.set_return(mgr.audio_capture_client_release_buffer(m)); }, 8});
    exports.entries.push_back(ExportEntry{"Vitreo_AudioCaptureClient_GetNextPacketSize", 0,
        [&mgr](Marshaler& m) {
            m.set_return(mgr.audio_capture_client_get_next_packet_size(m)); }, 8});
    append_wasapi_service_exports(exports, mgr);
    exports.entries.push_back(ExportEntry{
        "Vitreo_AudioPropertyStore_QueryInterface", 0,
        [&mgr](Marshaler& m) { m.set_return(mgr.property_store_query_interface(m)); }, 12});
    exports.entries.push_back(ExportEntry{
        "Vitreo_AudioPropertyStore_AddRef", 0,
        [&mgr](Marshaler& m) { m.set_return(mgr.property_store_add_ref(m)); }, 4});
    exports.entries.push_back(ExportEntry{
        "Vitreo_AudioPropertyStore_Release", 0,
        [&mgr](Marshaler& m) { m.set_return(mgr.property_store_release(m)); }, 4});
    exports.entries.push_back(ExportEntry{
        "Vitreo_AudioPropertyStore_GetCount", 0,
        [&mgr](Marshaler& m) { m.set_return(mgr.property_store_get_count(m)); }, 8});
    exports.entries.push_back(ExportEntry{
        "Vitreo_AudioPropertyStore_GetAt", 0,
        [&mgr](Marshaler& m) { m.set_return(mgr.property_store_get_at(m)); }, 12});
    exports.entries.push_back(ExportEntry{
        "Vitreo_AudioPropertyStore_GetValue", 0,
        [&mgr](Marshaler& m) { m.set_return(mgr.property_store_get_value(m)); }, 12});
    exports.entries.push_back(ExportEntry{
        "Vitreo_AudioPropertyStore_SetValue", 0,
        [&mgr](Marshaler& m) { m.set_return(mgr.property_store_set_value(m)); }, 12});
    exports.entries.push_back(ExportEntry{
        "Vitreo_AudioPropertyStore_Commit", 0,
        [&mgr](Marshaler& m) { m.set_return(mgr.property_store_commit(m)); }, 4});
}

} // namespace vitreo::winapi
