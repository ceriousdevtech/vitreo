// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file combase_internal.hpp
/// @brief Private to the combase implementation; include from
/// src/winapi/com/combase/*.cpp only. HRESULTs, runtime-class names and
/// interface IDs shared by the combase translation units. Everything sits in
/// an anonymous namespace: each including TU gets its own internal-linkage
/// copy.

#include "winapi/com/combase/combase.hpp"

#include "core/memory/guest_memory.hpp"
#include "winapi/kernel32/memory/heap.hpp"
#include "winapi/core/marshaler.hpp"

#include <array>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace vitreo::winapi {
namespace {

constexpr u32 S_OK = 0x00000000u;
constexpr u32 S_FALSE = 0x00000001u;
constexpr u32 E_NOTIMPL = 0x80004001u;
constexpr u32 E_NOINTERFACE = 0x80004002u;
constexpr u32 E_POINTER = 0x80004003u;
constexpr u32 E_OUTOFMEMORY = 0x8007000Eu;
constexpr u32 E_INVALIDARG = 0x80070057u;
constexpr u32 E_BOUNDS = 0x8000000Bu;
constexpr u32 REGDB_E_CLASSNOTREG = 0x80040154u;
constexpr u32 E_ILLEGAL_METHOD_CALL = 0x8000000Eu;

constexpr std::u16string_view GLOBALIZATION_PREFERENCES_CLASS =
    u"Windows.System.UserProfile.GlobalizationPreferences";
constexpr std::u16string_view JSON_OBJECT_CLASS =
    u"Windows.Data.Json.JsonObject";
constexpr std::u16string_view JSON_VALUE_CLASS =
    u"Windows.Data.Json.JsonValue";
constexpr std::u16string_view BLUETOOTH_ADAPTER_CLASS =
    u"Windows.Devices.Bluetooth.BluetoothAdapter";
constexpr std::u16string_view TOAST_MANAGER_CLASS =
    u"Windows.UI.Notifications.ToastNotificationManager";
constexpr std::u16string_view SMTC_CLASS =
    u"Windows.Media.SystemMediaTransportControls";
constexpr std::u16string_view SMTC_TIMELINE_CLASS =
    u"Windows.Media.SystemMediaTransportControlsTimelineProperties";
constexpr std::u16string_view KEY_CREDENTIAL_MANAGER_CLASS =
    u"Windows.Security.Credentials.KeyCredentialManager";
constexpr std::u16string_view UI_VIEW_SETTINGS_CLASS =
    u"Windows.UI.ViewManagement.UIViewSettings";
constexpr std::u16string_view PEN_DEVICE_CLASS =
    u"Windows.Devices.Input.PenDevice";
constexpr std::u16string_view APP_CAPABILITY_CLASS =
    u"Windows.Security.Authorization.AppCapabilityAccess.AppCapability";
constexpr std::u16string_view LAUNCHER_CLASS = u"Windows.System.Launcher";
constexpr std::u16string_view URI_CLASS = u"Windows.Foundation.Uri";
constexpr std::u16string_view STRING_VECTOR_RUNTIME_CLASS =
    u"Windows.Foundation.Collections.IVectorView`1<String>";

struct CombaseGuid {
    u32 data1;
    u16 data2;
    u16 data3;
    u8 data4[8];
};
static_assert(sizeof(CombaseGuid) == 16);

constexpr CombaseGuid IID_IUNKNOWN{
    0x00000000u, 0x0000u, 0x0000u,
    {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
constexpr CombaseGuid IID_IINSPECTABLE{
    0xAF86E2E0u, 0xB12Du, 0x4C6Au,
    {0x9C, 0x5A, 0xD7, 0xAA, 0x65, 0x10, 0x1E, 0x90}};
constexpr CombaseGuid IID_IACTIVATION_FACTORY{
    0x00000035u, 0x0000u, 0x0000u,
    {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
// IAgileObject is a marker interface: no methods, and supporting it says the
// object may be called from any apartment. Every object combase hands out is
// pure guest-visible state with no apartment affinity, so all of them qualify.
constexpr CombaseGuid IID_IAGILEOBJECT{
    0x94EA2B94u, 0xE9CCu, 0x49E0u,
    {0xC0, 0xFF, 0xEE, 0x64, 0xCA, 0x8F, 0x5B, 0x90}};
constexpr CombaseGuid IID_IJSON_VALUE_STATICS{
    0x5F6B544Au, 0x2F53u, 0x48E1u,
    {0x91, 0xA3, 0xF7, 0x8B, 0x50, 0xA6, 0x34, 0x5C}};
constexpr CombaseGuid IID_IBLUETOOTH_ADAPTER_STATICS{
    0x8B02FB6Au, 0xAC4Cu, 0x4741u,
    {0x86, 0x61, 0x8E, 0xAB, 0x7D, 0x17, 0xEA, 0x9F}};
constexpr CombaseGuid IID_IBLUETOOTH_ADAPTER{
    0x7974F984u, 0x1E8Cu, 0x4D5Bu,
    {0xA2, 0xD2, 0x1A, 0x09, 0xCB, 0xDD, 0xA1, 0xA1}};
constexpr CombaseGuid IID_ITOAST_NOTIFICATION_MANAGER_STATICS{
    0x50AC103Fu, 0xD235u, 0x4598u,
    {0xBB, 0xEF, 0x98, 0xFE, 0x4D, 0x1A, 0x3A, 0xD4}};
constexpr CombaseGuid IID_IKEY_CREDENTIAL_MANAGER_STATICS{
    0x6AAC468Bu, 0x0EF1u, 0x4CE0u,
    {0x82, 0x90, 0x41, 0x06, 0xDA, 0x6A, 0x63, 0xB5}};
constexpr CombaseGuid IID_IUI_VIEW_SETTINGS_INTEROP{
    0x3694DBF9u, 0x8F68u, 0x44BEu,
    {0x8F, 0xF5, 0x19, 0x5C, 0x98, 0xED, 0xE8, 0xA6}};
constexpr CombaseGuid IID_IUI_VIEW_SETTINGS{
    0xC63657F6u, 0x8850u, 0x470Du,
    {0x88, 0xF8, 0x45, 0x5E, 0x16, 0xEA, 0x2C, 0x26}};
constexpr CombaseGuid IID_IPEN_DEVICE_STATICS{
    0x9DFBBE01u, 0x0966u, 0x5180u,
    {0xBC, 0xB4, 0xB8, 0x50, 0x60, 0xE3, 0x94, 0x79}};
constexpr CombaseGuid IID_IAPP_CAPABILITY_STATICS{
    0x7C353E2Au, 0x46EEu, 0x44E5u,
    {0xAF, 0x3D, 0x6A, 0xD3, 0xFC, 0x49, 0xBD, 0x22}};
// windows.system.h / windows.foundation.h, SDK 10.0.26100.0.
constexpr CombaseGuid IID_ILAUNCHER_STATICS{
    0x277151C3u, 0x9E3Eu, 0x42F6u,
    {0x91, 0xA4, 0x5D, 0xFD, 0xEB, 0x23, 0x24, 0x51}};
constexpr CombaseGuid IID_IURI_RUNTIME_CLASS_FACTORY{
    0x44A9796Fu, 0x723Eu, 0x4FDFu,
    {0xA2, 0x18, 0x03, 0x3E, 0x75, 0xB0, 0xC0, 0x84}};
constexpr CombaseGuid IID_IURI_RUNTIME_CLASS{
    0x9E365E57u, 0x48B2u, 0x4160u,
    {0x95, 0x6F, 0xC7, 0x38, 0x51, 0x20, 0xBB, 0xFC}};
constexpr CombaseGuid IID_ISMTC_INTEROP{
    0xDDB0472Du, 0xC911u, 0x4A1Fu,
    {0x86, 0xD9, 0xDC, 0x3D, 0x71, 0xA9, 0x5F, 0x5A}};
constexpr CombaseGuid IID_ISYSTEM_MEDIA_TRANSPORT_CONTROLS{
    0x99FA3FF4u, 0x1742u, 0x42A6u,
    {0x90, 0x2E, 0x08, 0x7D, 0x41, 0xF9, 0x65, 0xEC}};
constexpr CombaseGuid IID_ISYSTEM_MEDIA_TRANSPORT_CONTROLS2{
    0xEA98D2F6u, 0x7F3Cu, 0x4AF2u,
    {0xA5, 0x86, 0x72, 0x88, 0x98, 0x08, 0xEF, 0xB1}};
// windows.media.h: ISystemMediaTransportControlsTimelineProperties.
constexpr CombaseGuid IID_ISMTC_TIMELINE_PROPERTIES{
    0x5125316Au, 0xC3A2u, 0x475Bu,
    {0x85, 0x07, 0x93, 0x53, 0x4D, 0xC8, 0x8F, 0x15}};
constexpr CombaseGuid IID_IDISPLAY_UPDATER{
    0x8ABBC53Eu, 0xFA55u, 0x4ECFu,
    {0xAD, 0x8E, 0xC9, 0x84, 0xE5, 0xDD, 0x15, 0x50}};
constexpr CombaseGuid IID_IMUSIC_DISPLAY_PROPERTIES{
    0x6BBF0C59u, 0xD0A0u, 0x4D26u,
    {0x92, 0xA0, 0xF9, 0x78, 0xE1, 0xD1, 0x8E, 0x7B}};
constexpr CombaseGuid IID_IWEAK_REFERENCE_SOURCE{
    0x00000038u, 0x0000u, 0x0000u,
    {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
constexpr CombaseGuid IID_IWEAK_REFERENCE{
    0x00000037u, 0x0000u, 0x0000u,
    {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
constexpr CombaseGuid IID_IRADIO{
    0x252118DFu, 0xB33Eu, 0x416Au,
    {0x87, 0x5F, 0x1C, 0xF3, 0x8A, 0xE2, 0xD8, 0x3E}};
constexpr CombaseGuid IID_IASYNC_INFO{
    0x00000036u, 0x0000u, 0x0000u,
    {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
constexpr CombaseGuid IID_IJSON_OBJECT{
    0x064E24DDu, 0x29C2u, 0x4F83u,
    {0x9A, 0xC1, 0x9E, 0xE1, 0x15, 0x78, 0xBE, 0xB3}};
constexpr CombaseGuid IID_IJSON_VALUE{
    0xA3219ECBu, 0xF0B3u, 0x4DCDu,
    {0xBE, 0xEE, 0x19, 0xD4, 0x8C, 0xD3, 0xED, 0x1E}};
constexpr CombaseGuid IID_IGLOBALIZATION_PREFERENCES_STATICS{
    0x01BF4326u, 0xED37u, 0x4E96u,
    {0xB0, 0xE9, 0xC1, 0x34, 0x0D, 0x1E, 0xA1, 0x58}};
constexpr CombaseGuid IID_IVECTOR_VIEW_HSTRING{
    0x2F13C006u, 0xA03Au, 0x5F69u,
    {0xB0, 0x90, 0x75, 0xA4, 0x3E, 0x33, 0x42, 0x3E}};

inline bool guid_equal(const CombaseGuid& left, const CombaseGuid& right) {
    return std::memcmp(&left, &right, sizeof(CombaseGuid)) == 0;
}

} // namespace
} // namespace vitreo::winapi
