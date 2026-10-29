// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// DLL registry part: combase.dll

#include "dll_registry_internal.hpp"

#include "winapi/com/combase/combase.hpp"

#include <array>
#include <cstdio>
#include <mutex>
#include <string_view>

namespace vitreo::cli {

void register_combase_dlls(RuntimeContext& ctx) {
    auto& reg = ctx.thunk_registry;

    // -- combase.dll --
    {
        auto combase_exports =
            winapi::create_combase_thunks(ctx.combase_mgr, ctx.com_mgr);
        {
            // Audit completions for the WinRT error and activation-registration surfaces.
            winapi::DllExports audit;
            audit.entries.push_back({"RoOriginateError", 0,
                [](winapi::Marshaler& m) {
                    (void)m.arg(0);
                    (void)m.arg(1);
                    // No restricted-error-info store: report "not captured".
                    m.set_return(0u);
                }, 8});
            audit.entries.push_back({"RoTransformError", 0,
                [](winapi::Marshaler& m) {
                    (void)m.arg(0);
                    (void)m.arg(1);
                    (void)m.arg(2);
                    // No restricted-error-info store: report "not captured".
                    m.set_return(0u);
                }, 12});
            audit.entries.push_back({"RoFailFastWithErrorContext", 0,
                [](winapi::Marshaler& m) {
                    const u32 hr = static_cast<u32>(m.arg(0));
                    std::fprintf(stderr,
                        "[combase] RoFailFastWithErrorContext(0x%08x)\n", hr);
                    m.set_return(0u);
                }, 4});
            // HRESULT RoRegisterActivationFactories(HSTRING*,
            //   PFNGETACTIVATIONFACTORY*, UINT32 count, RO_REGISTRATION_COOKIE*)
            // x86 stdcall 16 (libruntimeobject.a
            // _RoRegisterActivationFactories@16). In-process factory
            // registration would need a table the activation path consults;
            // it fails loudly with the cookie cleared.
            audit.entries.push_back({"RoRegisterActivationFactories", 0,
                [](winapi::Marshaler& m) {
                    if (const GuestAddr cookie = m.arg_ptr(3); cookie != 0) {
                        (void)m.write_ptr(cookie, 0);
                    }
                    static std::once_flag once;
                    std::call_once(once, [] {
                        std::fprintf(stderr, "[vitreo] UNIMPLEMENTED combase!"
                                             "RoRegisterActivationFactories\n");
                    });
                    m.set_return(0x80004001u);
                }, 16});
            audit.entries.push_back({"RoRevokeActivationFactories", 0,
                [](winapi::Marshaler& m) {
                    (void)m.arg(0);
                    // In-process factory registration would need a table the
                    // activation path does not consult.
                    m.set_return(0x80004001u);
                }, 4});
            merge_exports(combase_exports, std::move(audit));
        }
        (void)reg.register_dll("combase.dll", std::move(combase_exports));
        constexpr std::array<std::string_view, winapi::CombaseManager::kMethodThunks> method_names = {
            "_Combase_QueryInterface",
            "_Combase_AddRef",
            "_Combase_Release",
            "_Combase_GetIids",
            "_Combase_GetRuntimeClassName",
            "_Combase_GetTrustLevel",
            "_Combase_GetCalendars",
            "_Combase_GetClocks",
            "_Combase_GetCurrencies",
            "_Combase_GetLanguages",
            "_Combase_GetHomeGeographicRegion",
            "_Combase_GetWeekStartsOn",
            "_Combase_VectorGetAt",
            "_Combase_VectorGetSize",
            "_Combase_VectorIndexOf",
            "_Combase_VectorGetMany",
            "_Combase_ActivateInstance",
            "_Combase_JsonValueStatics_Parse",
            "_Combase_JsonValueStatics_TryParse",
            "_Combase_JsonValueStatics_CreateBooleanValue",
            "_Combase_JsonValueStatics_CreateNumberValue",
            "_Combase_JsonValueStatics_CreateStringValue",
            "_Combase_JsonValueStatics_CreateNullValue",
            "_Combase_JsonValue_get_ValueType",
            "_Combase_JsonValue_Stringify",
            "_Combase_JsonValue_GetString",
            "_Combase_JsonValue_GetNumber",
            "_Combase_JsonValue_GetBoolean",
            "_Combase_JsonValue_GetArray",
            "_Combase_JsonValue_GetObject",
            "_Combase_JsonObject_GetNamedValue",
            "_Combase_JsonObject_SetNamedValue",
            "_Combase_JsonObject_GetNamedObject",
            "_Combase_JsonObject_GetNamedArray",
            "_Combase_JsonObject_GetNamedString",
            "_Combase_JsonObject_GetNamedNumber",
            "_Combase_JsonObject_GetNamedBoolean",
            "_Combase_BT_GetDeviceSelector",
            "_Combase_BT_FromIdAsync",
            "_Combase_BT_GetDefaultAsync",
            "_Combase_Async_put_Completed",
            "_Combase_Async_get_Completed",
            "_Combase_Async_GetResults",
            "_Combase_AsyncInfo_get_Id",
            "_Combase_AsyncInfo_get_Status",
            "_Combase_AsyncInfo_get_ErrorCode",
            "_Combase_AsyncInfo_Cancel",
            "_Combase_AsyncInfo_Close",
            "_Combase_BTAdapter_get_DeviceId",
            "_Combase_BTAdapter_get_BluetoothAddress",
            "_Combase_BTAdapter_get_IsClassicSupported",
            "_Combase_BTAdapter_get_IsLowEnergySupported",
            "_Combase_BTAdapter_get_IsPeripheralRoleSupported",
            "_Combase_BTAdapter_get_IsCentralRoleSupported",
            "_Combase_BTAdapter_get_IsAdvertisementOffloadSupported",
            "_Combase_BTAdapter_GetRadioAsync",
            "_Combase_Radio_SetStateAsync",
            "_Combase_Radio_add_StateChanged",
            "_Combase_Radio_remove_StateChanged",
            "_Combase_Radio_get_State",
            "_Combase_Radio_get_Name",
            "_Combase_Radio_get_Kind",
            "_Combase_GetWeakReference",
            "_Combase_WeakReference_Resolve",
            "_Combase_Toast_CreateToastNotifier",
            "_Combase_Toast_CreateToastNotifierWithId",
            "_Combase_Toast_GetTemplateContent",
            "_Combase_KeyCred_IsSupportedAsync",
            "_Combase_KeyCred_RenewAttestationAsync",
            "_Combase_KeyCred_RequestCreateAsync",
            "_Combase_KeyCred_OpenAsync",
            "_Combase_KeyCred_DeleteAsync",
            "_Combase_UIViewSettings_GetForWindow",
            "_Combase_UIViewSettings_get_UserInteractionMode",
            "_Combase_PenDevice_GetFromPointerId",
            "_Combase_AppCapability_Create",
            "_Combase_AppCapability_CreateForUser",
            "_Combase_SMTC_GetForWindow",
            "_Combase_SMTC_get_PlaybackStatus",
            "_Combase_SMTC_put_PlaybackStatus",
            "_Combase_SMTC_get_DisplayUpdater",
            "_Combase_SMTC_get_SoundLevel",
            "_Combase_SMTC_get_IsEnabled",
            "_Combase_SMTC_put_IsEnabled",
            "_Combase_SMTC_get_IsPlayEnabled",
            "_Combase_SMTC_put_IsPlayEnabled",
            "_Combase_SMTC_get_IsStopEnabled",
            "_Combase_SMTC_put_IsStopEnabled",
            "_Combase_SMTC_get_IsPauseEnabled",
            "_Combase_SMTC_put_IsPauseEnabled",
            "_Combase_SMTC_get_IsRecordEnabled",
            "_Combase_SMTC_put_IsRecordEnabled",
            "_Combase_SMTC_get_IsFastForwardEnabled",
            "_Combase_SMTC_put_IsFastForwardEnabled",
            "_Combase_SMTC_get_IsRewindEnabled",
            "_Combase_SMTC_put_IsRewindEnabled",
            "_Combase_SMTC_get_IsPreviousEnabled",
            "_Combase_SMTC_put_IsPreviousEnabled",
            "_Combase_SMTC_get_IsNextEnabled",
            "_Combase_SMTC_put_IsNextEnabled",
            "_Combase_SMTC_get_IsChannelUpEnabled",
            "_Combase_SMTC_put_IsChannelUpEnabled",
            "_Combase_SMTC_get_IsChannelDownEnabled",
            "_Combase_SMTC_put_IsChannelDownEnabled",
            "_Combase_SMTC_add_ButtonPressed",
            "_Combase_SMTC_remove_ButtonPressed",
            "_Combase_SMTC_add_PropertyChanged",
            "_Combase_SMTC_remove_PropertyChanged",
            "_Combase_DU_get_Type",
            "_Combase_DU_put_Type",
            "_Combase_DU_get_AppMediaId",
            "_Combase_DU_put_AppMediaId",
            "_Combase_DU_get_Thumbnail",
            "_Combase_DU_put_Thumbnail",
            "_Combase_DU_get_MusicProperties",
            "_Combase_DU_get_VideoProperties",
            "_Combase_DU_get_ImageProperties",
            "_Combase_DU_CopyFromFileAsync",
            "_Combase_DU_ClearAll",
            "_Combase_DU_Update",
            "_Combase_Music_get_Title",
            "_Combase_Music_put_Title",
            "_Combase_Music_get_AlbumArtist",
            "_Combase_Music_put_AlbumArtist",
            "_Combase_Music_get_Artist",
            "_Combase_Music_put_Artist",
            "_Combase_SMTC2_get_PlaybackRate",
            "_Combase_SMTC2_put_PlaybackRate",
            "_Combase_SMTC2_UpdateTimelineProperties",
            "_Combase_AppCapability_RequestAccessForCapabilitiesAsync",
            "_Combase_AppCapability_RequestAccessForCapabilitiesForUserAsync",
            "_Combase_NotImplemented",
            "_Combase_Launcher_LaunchUriAsync",
            "_Combase_UriFactory_CreateUri",
            "_Combase_Uri_GetText",
            "_Combase_Timeline_get_StartTime",
            "_Combase_Timeline_put_StartTime",
            "_Combase_Timeline_get_EndTime",
            "_Combase_Timeline_put_EndTime",
            "_Combase_Timeline_get_MinSeekTime",
            "_Combase_Timeline_put_MinSeekTime",
            "_Combase_Timeline_get_MaxSeekTime",
            "_Combase_Timeline_put_MaxSeekTime",
            "_Combase_Timeline_get_Position",
            "_Combase_Timeline_put_Position",
            "_Combase_SMTC2_get_AutoRepeatMode",
            "_Combase_SMTC2_put_AutoRepeatMode",
            "_Combase_SMTC2_get_ShuffleEnabled",
            "_Combase_SMTC2_put_ShuffleEnabled",
        };
        std::array<GuestAddr, method_names.size()> method_thunks{};
        bool complete = true;
        for (usize i = 0; i < method_names.size(); ++i) {
            auto thunk = reg.get_thunk_address(
                "combase.dll", method_names[i]);
            if (thunk.is_err()) {
                complete = false;
                break;
            }
            method_thunks[i] = thunk.value();
        }
        if (complete) ctx.combase_mgr.set_method_thunks(method_thunks);
    }
}

} // namespace vitreo::cli
