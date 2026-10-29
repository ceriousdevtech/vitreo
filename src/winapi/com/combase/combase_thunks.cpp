// SPDX-License-Identifier: LGPL-2.1-only
/// @file combase_thunks.cpp
/// @brief combase.dll export table (create_combase_thunks)

#include "winapi/com/combase/combase.hpp"
#include "winapi/com/combase/combase_internal.hpp"
#include "winapi/com/ole32/com_core.hpp"

#include <mutex>

namespace vitreo::winapi {

DllExports create_combase_thunks(CombaseManager& manager, ComCoreManager& com) {
    DllExports exports;
    exports.entries.reserve(34);

    // HRESULT RoInitialize(RO_INIT_TYPE initType)
    // Learn: initializes the Windows Runtime *and* the calling thread's COM
    // apartment — RO_INIT_SINGLETHREADED an STA, RO_INIT_MULTITHREADED an MTA.
    // It shares the apartment and the initialize count with CoInitializeEx, so
    // it must go through the same per-thread state: a guest that enters its MTA
    // only through RoInitialize otherwise reads "no apartment" out of its TEB.
    exports.entries.push_back({"RoInitialize", 0, [&com](Marshaler& m) {
        m.set_return(com.ro_initialize(m, static_cast<u32>(m.arg(0))));
    }, 4});
    exports.entries.push_back({"RoUninitialize", 0, [&com](Marshaler& m) {
        com.co_uninitialize(m);
        m.set_return(S_OK);
    }, 0});
    exports.entries.push_back({"RoGetActivationFactory", 0,
        [&manager](Marshaler& m) {
            manager.impl_RoGetActivationFactory(m);
        }, 12});
    // HRESULT RoActivateInstance(HSTRING activatableClassId, IInspectable**)
    // = RoGetActivationFactory(IID_IActivationFactory) + ActivateInstance
    // (slot 6, after IInspectable's six) + Release. Measured: an unknown
    // class is REGDB_E_CLASSNOTREG with *instance NULL.
    exports.entries.push_back({"RoActivateInstance", 0, [&com](Marshaler& m) {
        const GuestAddr class_id = m.arg_ptr(0);
        const GuestAddr out = m.arg_ptr(1);
        if (out != 0) (void)m.write_ptr(out, 0);
        if (out == 0) {
            m.set_return(E_INVALIDARG);
            return;
        }
        const GuestAddr scratch = com.co_task_mem_alloc_guest(32);
        if (scratch == 0) {
            m.set_return(0x8007000Eu);
            return;
        }
        // IID_IActivationFactory {00000035-0000-0000-C000-000000000046}.
        static constexpr u8 kIid[16] = {0x35, 0, 0, 0, 0, 0, 0, 0,
                                        0xC0, 0, 0, 0, 0, 0, 0, 0x46};
        (void)m.memory().write_bytes(scratch, kIid, sizeof(kIid));
        (void)m.write_ptr(scratch + 16, 0);
        u32 hr = static_cast<u32>(com.call_guest_export(
            m, "combase.dll", "RoGetActivationFactory", {class_id, scratch, scratch + 16}));
        const GuestAddr factory = static_cast<GuestAddr>(m.read_ptr(scratch + 16).value_or(0));
        com.co_task_mem_free_guest(scratch);
        if ((hr & 0x80000000u) != 0 || factory == 0) {
            m.set_return((hr & 0x80000000u) != 0 ? hr : 0x80040154u);
            return;
        }
        const u32 ptr = m.cpu_is_64bit() ? 8u : 4u;
        const GuestAddr vtable = static_cast<GuestAddr>(m.read_ptr(factory).value_or(0));
        const GuestAddr activate = vtable == 0 ? 0
            : static_cast<GuestAddr>(m.read_ptr(vtable + 6u * ptr).value_or(0));
        hr = activate == 0 ? E_NOTIMPL
                           : static_cast<u32>(com.call_guest_function(m, activate, {factory, out}));
        com.guest_iunknown_release(m, factory);
        m.set_return(hr);
    }, 8});
    exports.entries.push_back({"RoGetApartmentIdentifier", 0, [&com](Marshaler& m) {
        m.set_return(com.ro_get_apartment_identifier(m, m.arg_ptr(0)));
    }, 4});
    exports.entries.push_back({"RoRegisterForApartmentShutdown", 0, [&com](Marshaler& m) {
        m.set_return(com.ro_register_for_apartment_shutdown(
            m, m.arg_ptr(0), m.arg_ptr(1), m.arg_ptr(2)));
    }, 12});
    exports.entries.push_back({"RoUnregisterForApartmentShutdown", 0, [&com](Marshaler& m) {
        m.set_return(com.ro_unregister_for_apartment_shutdown(m, m.arg_ptr(0)));
    }, 4});
    exports.entries.push_back({"WindowsCompareStringOrdinal", 0,
        [&manager](Marshaler& m) {
            manager.impl_WindowsCompareStringOrdinal(m);
        }, 12});

    exports.entries.push_back({"WindowsCreateString", 0,
        [&manager](Marshaler& m) {
            manager.impl_WindowsCreateString(m);
        }, 12});
    exports.entries.push_back({"WindowsCreateStringReference", 0,
        [&manager](Marshaler& m) {
            manager.impl_WindowsCreateStringReference(m);
        }, 16});
    exports.entries.push_back({"WindowsDeleteString", 0,
        [&manager](Marshaler& m) {
            manager.impl_WindowsDeleteString(m);
        }, 4});
    exports.entries.push_back({"WindowsGetStringRawBuffer", 0,
        [&manager](Marshaler& m) {
            manager.impl_WindowsGetStringRawBuffer(m);
        }, 8});
    exports.entries.push_back({"WindowsDuplicateString", 0,
        [&manager](Marshaler& m) {
            manager.impl_WindowsDuplicateString(m);
        }, 8});
    exports.entries.push_back({"WindowsGetStringLen", 0,
        [&manager](Marshaler& m) {
            manager.impl_WindowsGetStringLen(m);
        }, 4});
    exports.entries.push_back({"WindowsStringHasEmbeddedNull", 0,
        [&manager](Marshaler& m) {
            manager.impl_WindowsStringHasEmbeddedNull(m);
        }, 8});
    exports.entries.push_back({"WindowsIsStringEmpty", 0,
        [&manager](Marshaler& m) {
            manager.impl_WindowsIsStringEmpty(m);
        }, 4});

    exports.entries.push_back({"RoOriginateLanguageException", 0,
        [](Marshaler& m) {
            (void)m.arg(0);
            (void)m.arg_ptr(1);
            (void)m.arg_ptr(2);
            m.set_return(0u);
        }, 12});
    exports.entries.push_back({"RoOriginateErrorW", 0,
        [](Marshaler& m) {
            (void)m.arg(0);
            (void)m.arg(1);
            (void)m.arg_ptr(2);
            m.set_return(0u);
        }, 12});
    exports.entries.push_back({"SetRestrictedErrorInfo", 0,
        [](Marshaler& m) {
            (void)m.arg_ptr(0);
            m.set_return(S_OK);
        }, 4});
    exports.entries.push_back({"GetRestrictedErrorInfo", 0,
        [](Marshaler& m) {
            const GuestAddr out = m.arg_ptr(0);
            if (out != 0) (void)m.write_ptr(out, 0);
            m.set_return(S_FALSE);
        }, 4});
    exports.entries.push_back({"_Combase_QueryInterface", 0,
        [&manager](Marshaler& m) { manager.impl_QueryInterface(m); }, 12});
    exports.entries.push_back({"_Combase_AddRef", 0,
        [&manager](Marshaler& m) { manager.impl_AddRef(m); }, 4});
    exports.entries.push_back({"_Combase_Release", 0,
        [&manager](Marshaler& m) { manager.impl_Release(m); }, 4});
    exports.entries.push_back({"_Combase_GetIids", 0,
        [&manager](Marshaler& m) { manager.impl_GetIids(m); }, 12});
    exports.entries.push_back({"_Combase_GetRuntimeClassName", 0,
        [&manager](Marshaler& m) {
            manager.impl_GetRuntimeClassName(m);
        }, 8});
    exports.entries.push_back({"_Combase_GetTrustLevel", 0,
        [&manager](Marshaler& m) { manager.impl_GetTrustLevel(m); }, 8});
    exports.entries.push_back({"_Combase_GetCalendars", 0,
        [&manager](Marshaler& m) { manager.impl_GetCalendars(m); }, 8});
    exports.entries.push_back({"_Combase_GetClocks", 0,
        [&manager](Marshaler& m) { manager.impl_GetClocks(m); }, 8});
    exports.entries.push_back({"_Combase_GetCurrencies", 0,
        [&manager](Marshaler& m) { manager.impl_GetCurrencies(m); }, 8});
    exports.entries.push_back({"_Combase_GetLanguages", 0,
        [&manager](Marshaler& m) { manager.impl_GetLanguages(m); }, 8});
    exports.entries.push_back({"_Combase_GetHomeGeographicRegion", 0,
        [&manager](Marshaler& m) {
            manager.impl_GetHomeGeographicRegion(m);
        }, 8});
    exports.entries.push_back({"_Combase_GetWeekStartsOn", 0,
        [&manager](Marshaler& m) { manager.impl_GetWeekStartsOn(m); }, 8});
    exports.entries.push_back({"_Combase_VectorGetAt", 0,
        [&manager](Marshaler& m) { manager.impl_VectorGetAt(m); }, 12});
    exports.entries.push_back({"_Combase_VectorGetSize", 0,
        [&manager](Marshaler& m) { manager.impl_VectorGetSize(m); }, 8});
    exports.entries.push_back({"_Combase_VectorIndexOf", 0,
        [&manager](Marshaler& m) { manager.impl_VectorIndexOf(m); }, 16});
    exports.entries.push_back({"_Combase_VectorGetMany", 0,
        [&manager](Marshaler& m) { manager.impl_VectorGetMany(m); }, 20});
    exports.entries.push_back({"_Combase_ActivateInstance", 0,
        [&manager](Marshaler& m) { manager.impl_ActivateInstance(m); }, 8});
    exports.entries.push_back({"_Combase_JsonValueStatics_Parse", 0,
        [&manager](Marshaler& m) { manager.impl_JsonValueParse(m); }, 12});
    exports.entries.push_back({"_Combase_JsonValueStatics_TryParse", 0,
        [&manager](Marshaler& m) { manager.impl_JsonValueTryParse(m); }, 16});
    exports.entries.push_back({"_Combase_JsonValueStatics_CreateBooleanValue", 0,
        [&manager](Marshaler& m) { manager.impl_JsonValueCreateBoolean(m); }, 12});
    exports.entries.push_back({"_Combase_JsonValueStatics_CreateNumberValue", 0,
        [&manager](Marshaler& m) { manager.impl_JsonValueCreateNumber(m); }, 16});
    exports.entries.push_back({"_Combase_JsonValueStatics_CreateStringValue", 0,
        [&manager](Marshaler& m) { manager.impl_JsonValueCreateString(m); }, 12});
    exports.entries.push_back({"_Combase_JsonValueStatics_CreateNullValue", 0,
        [&manager](Marshaler& m) { manager.impl_JsonValueCreateNull(m); }, 8});
    exports.entries.push_back({"_Combase_JsonValue_get_ValueType", 0,
        [&manager](Marshaler& m) { manager.impl_JsonValueGetValueType(m); }, 8});
    exports.entries.push_back({"_Combase_JsonValue_Stringify", 0,
        [&manager](Marshaler& m) { manager.impl_JsonValueStringify(m); }, 8});
    exports.entries.push_back({"_Combase_JsonValue_GetString", 0,
        [&manager](Marshaler& m) { manager.impl_JsonValueGetString(m); }, 8});
    exports.entries.push_back({"_Combase_JsonValue_GetNumber", 0,
        [&manager](Marshaler& m) { manager.impl_JsonValueGetNumber(m); }, 8});
    exports.entries.push_back({"_Combase_JsonValue_GetBoolean", 0,
        [&manager](Marshaler& m) { manager.impl_JsonValueGetBoolean(m); }, 8});
    exports.entries.push_back({"_Combase_JsonValue_GetArray", 0,
        [&manager](Marshaler& m) { manager.impl_JsonValueGetArray(m); }, 8});
    exports.entries.push_back({"_Combase_JsonValue_GetObject", 0,
        [&manager](Marshaler& m) { manager.impl_JsonValueGetObject(m); }, 8});
    exports.entries.push_back({"_Combase_JsonObject_GetNamedValue", 0,
        [&manager](Marshaler& m) { manager.impl_JsonObjectGetNamedValue(m); }, 12});
    exports.entries.push_back({"_Combase_JsonObject_SetNamedValue", 0,
        [&manager](Marshaler& m) { manager.impl_JsonObjectSetNamedValue(m); }, 12});
    exports.entries.push_back({"_Combase_JsonObject_GetNamedObject", 0,
        [&manager](Marshaler& m) { manager.impl_JsonObjectGetNamedObject(m); }, 12});
    exports.entries.push_back({"_Combase_JsonObject_GetNamedArray", 0,
        [&manager](Marshaler& m) { manager.impl_JsonObjectGetNamedArray(m); }, 12});
    exports.entries.push_back({"_Combase_JsonObject_GetNamedString", 0,
        [&manager](Marshaler& m) { manager.impl_JsonObjectGetNamedString(m); }, 12});
    exports.entries.push_back({"_Combase_JsonObject_GetNamedNumber", 0,
        [&manager](Marshaler& m) { manager.impl_JsonObjectGetNamedNumber(m); }, 12});
    exports.entries.push_back({"_Combase_JsonObject_GetNamedBoolean", 0,
        [&manager](Marshaler& m) { manager.impl_JsonObjectGetNamedBoolean(m); }, 12});
    exports.entries.push_back({"_Combase_BT_GetDeviceSelector", 0,
        [&manager](Marshaler& m) { manager.impl_BluetoothStaticsGetDeviceSelector(m); }, 8});
    exports.entries.push_back({"_Combase_BT_FromIdAsync", 0,
        [&manager](Marshaler& m) { manager.impl_BluetoothStaticsFromIdAsync(m); }, 12});
    exports.entries.push_back({"_Combase_BT_GetDefaultAsync", 0,
        [&manager](Marshaler& m) { manager.impl_BluetoothStaticsGetDefaultAsync(m); }, 8});
    exports.entries.push_back({"_Combase_Async_put_Completed", 0,
        [&manager](Marshaler& m) { manager.impl_AsyncPutCompleted(m); }, 8});
    exports.entries.push_back({"_Combase_Async_get_Completed", 0,
        [&manager](Marshaler& m) { manager.impl_AsyncGetCompleted(m); }, 8});
    exports.entries.push_back({"_Combase_Async_GetResults", 0,
        [&manager](Marshaler& m) { manager.impl_AsyncGetResults(m); }, 8});
    exports.entries.push_back({"_Combase_AsyncInfo_get_Id", 0,
        [&manager](Marshaler& m) { manager.impl_AsyncInfoGetId(m); }, 8});
    exports.entries.push_back({"_Combase_AsyncInfo_get_Status", 0,
        [&manager](Marshaler& m) { manager.impl_AsyncInfoGetStatus(m); }, 8});
    exports.entries.push_back({"_Combase_AsyncInfo_get_ErrorCode", 0,
        [&manager](Marshaler& m) { manager.impl_AsyncInfoGetErrorCode(m); }, 8});
    exports.entries.push_back({"_Combase_AsyncInfo_Cancel", 0,
        [&manager](Marshaler& m) { manager.impl_AsyncInfoCancel(m); }, 4});
    exports.entries.push_back({"_Combase_AsyncInfo_Close", 0,
        [&manager](Marshaler& m) { manager.impl_AsyncInfoClose(m); }, 4});
    exports.entries.push_back({"_Combase_BTAdapter_get_DeviceId", 0,
        [&manager](Marshaler& m) { manager.impl_BluetoothAdapterGetDeviceId(m); }, 8});
    exports.entries.push_back({"_Combase_BTAdapter_get_BluetoothAddress", 0,
        [&manager](Marshaler& m) { manager.impl_BluetoothAdapterGetAddress(m); }, 8});
    exports.entries.push_back({"_Combase_BTAdapter_get_IsClassicSupported", 0,
        [&manager](Marshaler& m) { manager.impl_BluetoothAdapterGetClassicSupported(m); }, 8});
    exports.entries.push_back({"_Combase_BTAdapter_get_IsLowEnergySupported", 0,
        [&manager](Marshaler& m) { manager.impl_BluetoothAdapterGetLowEnergySupported(m); }, 8});
    exports.entries.push_back({"_Combase_BTAdapter_get_IsPeripheralRoleSupported", 0,
        [&manager](Marshaler& m) { manager.impl_BluetoothAdapterGetPeripheralRoleSupported(m); }, 8});
    exports.entries.push_back({"_Combase_BTAdapter_get_IsCentralRoleSupported", 0,
        [&manager](Marshaler& m) { manager.impl_BluetoothAdapterGetCentralRoleSupported(m); }, 8});
    exports.entries.push_back({"_Combase_BTAdapter_get_IsAdvertisementOffloadSupported", 0,
        [&manager](Marshaler& m) { manager.impl_BluetoothAdapterGetAdvertisementOffloadSupported(m); }, 8});
    exports.entries.push_back({"_Combase_BTAdapter_GetRadioAsync", 0,
        [&manager](Marshaler& m) { manager.impl_BluetoothAdapterGetRadioAsync(m); }, 8});
    exports.entries.push_back({"_Combase_Radio_SetStateAsync", 0,
        [&manager](Marshaler& m) { manager.impl_RadioSetStateAsync(m); }, 12});
    exports.entries.push_back({"_Combase_Radio_add_StateChanged", 0,
        [&manager](Marshaler& m) { manager.impl_RadioAddStateChanged(m); }, 12});
    exports.entries.push_back({"_Combase_Radio_remove_StateChanged", 0,
        [&manager](Marshaler& m) { manager.impl_RadioRemoveStateChanged(m); }, 12});
    exports.entries.push_back({"_Combase_Radio_get_State", 0,
        [&manager](Marshaler& m) { manager.impl_RadioGetState(m); }, 8});
    exports.entries.push_back({"_Combase_Radio_get_Name", 0,
        [&manager](Marshaler& m) { manager.impl_RadioGetName(m); }, 8});
    exports.entries.push_back({"_Combase_Radio_get_Kind", 0,
        [&manager](Marshaler& m) { manager.impl_RadioGetKind(m); }, 8});
    exports.entries.push_back({"_Combase_GetWeakReference", 0,
        [&manager](Marshaler& m) { manager.impl_GetWeakReference(m); }, 8});
    exports.entries.push_back({"_Combase_WeakReference_Resolve", 0,
        [&manager](Marshaler& m) { manager.impl_WeakReferenceResolve(m); }, 12});
    exports.entries.push_back({"_Combase_Toast_CreateToastNotifier", 0,
        [&manager](Marshaler& m) { manager.impl_ToastCreateNotifier(m); }, 8});
    exports.entries.push_back({"_Combase_Toast_CreateToastNotifierWithId", 0,
        [&manager](Marshaler& m) { manager.impl_ToastCreateNotifierWithId(m); }, 12});
    exports.entries.push_back({"_Combase_Toast_GetTemplateContent", 0,
        [&manager](Marshaler& m) { manager.impl_ToastGetTemplateContent(m); }, 12});
    exports.entries.push_back({"_Combase_KeyCred_IsSupportedAsync", 0,
        [&manager](Marshaler& m) { manager.impl_KeyCredIsSupportedAsync(m); }, 8});
    exports.entries.push_back({"_Combase_KeyCred_RenewAttestationAsync", 0,
        [&manager](Marshaler& m) { manager.impl_KeyCredRenewAttestationAsync(m); }, 8});
    exports.entries.push_back({"_Combase_KeyCred_RequestCreateAsync", 0,
        [&manager](Marshaler& m) { manager.impl_KeyCredRequestCreateAsync(m); }, 16});
    exports.entries.push_back({"_Combase_KeyCred_OpenAsync", 0,
        [&manager](Marshaler& m) { manager.impl_KeyCredOpenAsync(m); }, 12});
    exports.entries.push_back({"_Combase_KeyCred_DeleteAsync", 0,
        [&manager](Marshaler& m) { manager.impl_KeyCredDeleteAsync(m); }, 12});
    exports.entries.push_back({"_Combase_UIViewSettings_GetForWindow", 0,
        [&manager](Marshaler& m) { manager.impl_UIViewSettingsGetForWindow(m); }, 16});
    exports.entries.push_back({"_Combase_UIViewSettings_get_UserInteractionMode", 0,
        [&manager](Marshaler& m) { manager.impl_UIViewSettingsGetUserInteractionMode(m); }, 8});
    exports.entries.push_back({"_Combase_PenDevice_GetFromPointerId", 0,
        [&manager](Marshaler& m) { manager.impl_PenDeviceGetFromPointerId(m); }, 12});
    exports.entries.push_back({"_Combase_AppCapability_Create", 0,
        [&manager](Marshaler& m) { manager.impl_AppCapabilityCreate(m); }, 12});
    exports.entries.push_back({"_Combase_AppCapability_CreateForUser", 0,
        [&manager](Marshaler& m) { manager.impl_AppCapabilityCreateForUser(m); }, 20});
    exports.entries.push_back({"_Combase_SMTC_GetForWindow", 0,
        [&manager](Marshaler& m) { manager.impl_SmtcGetForWindow(m); }, 16});
    exports.entries.push_back({"_Combase_SMTC_get_PlaybackStatus", 0,
        [&manager](Marshaler& m) { manager.impl_SmtcGetPlaybackStatus(m); }, 8});
    exports.entries.push_back({"_Combase_SMTC_put_PlaybackStatus", 0,
        [&manager](Marshaler& m) { manager.impl_SmtcPutPlaybackStatus(m); }, 8});
    exports.entries.push_back({"_Combase_SMTC_get_DisplayUpdater", 0,
        [&manager](Marshaler& m) { manager.impl_SmtcGetDisplayUpdater(m); }, 8});
    exports.entries.push_back({"_Combase_SMTC_get_SoundLevel", 0,
        [&manager](Marshaler& m) { manager.impl_SmtcGetSoundLevel(m); }, 8});
    exports.entries.push_back({"_Combase_SMTC_get_IsEnabled", 0,
        [&manager](Marshaler& m) { m.set_return(manager.smtc_get_flag(m, 0)); }, 8});
    exports.entries.push_back({"_Combase_SMTC_put_IsEnabled", 0,
        [&manager](Marshaler& m) { m.set_return(manager.smtc_put_flag(m, 0)); }, 8});
    exports.entries.push_back({"_Combase_SMTC_get_IsPlayEnabled", 0,
        [&manager](Marshaler& m) { m.set_return(manager.smtc_get_flag(m, 1)); }, 8});
    exports.entries.push_back({"_Combase_SMTC_put_IsPlayEnabled", 0,
        [&manager](Marshaler& m) { m.set_return(manager.smtc_put_flag(m, 1)); }, 8});
    exports.entries.push_back({"_Combase_SMTC_get_IsStopEnabled", 0,
        [&manager](Marshaler& m) { m.set_return(manager.smtc_get_flag(m, 2)); }, 8});
    exports.entries.push_back({"_Combase_SMTC_put_IsStopEnabled", 0,
        [&manager](Marshaler& m) { m.set_return(manager.smtc_put_flag(m, 2)); }, 8});
    exports.entries.push_back({"_Combase_SMTC_get_IsPauseEnabled", 0,
        [&manager](Marshaler& m) { m.set_return(manager.smtc_get_flag(m, 3)); }, 8});
    exports.entries.push_back({"_Combase_SMTC_put_IsPauseEnabled", 0,
        [&manager](Marshaler& m) { m.set_return(manager.smtc_put_flag(m, 3)); }, 8});
    exports.entries.push_back({"_Combase_SMTC_get_IsRecordEnabled", 0,
        [&manager](Marshaler& m) { m.set_return(manager.smtc_get_flag(m, 4)); }, 8});
    exports.entries.push_back({"_Combase_SMTC_put_IsRecordEnabled", 0,
        [&manager](Marshaler& m) { m.set_return(manager.smtc_put_flag(m, 4)); }, 8});
    exports.entries.push_back({"_Combase_SMTC_get_IsFastForwardEnabled", 0,
        [&manager](Marshaler& m) { m.set_return(manager.smtc_get_flag(m, 5)); }, 8});
    exports.entries.push_back({"_Combase_SMTC_put_IsFastForwardEnabled", 0,
        [&manager](Marshaler& m) { m.set_return(manager.smtc_put_flag(m, 5)); }, 8});
    exports.entries.push_back({"_Combase_SMTC_get_IsRewindEnabled", 0,
        [&manager](Marshaler& m) { m.set_return(manager.smtc_get_flag(m, 6)); }, 8});
    exports.entries.push_back({"_Combase_SMTC_put_IsRewindEnabled", 0,
        [&manager](Marshaler& m) { m.set_return(manager.smtc_put_flag(m, 6)); }, 8});
    exports.entries.push_back({"_Combase_SMTC_get_IsPreviousEnabled", 0,
        [&manager](Marshaler& m) { m.set_return(manager.smtc_get_flag(m, 7)); }, 8});
    exports.entries.push_back({"_Combase_SMTC_put_IsPreviousEnabled", 0,
        [&manager](Marshaler& m) { m.set_return(manager.smtc_put_flag(m, 7)); }, 8});
    exports.entries.push_back({"_Combase_SMTC_get_IsNextEnabled", 0,
        [&manager](Marshaler& m) { m.set_return(manager.smtc_get_flag(m, 8)); }, 8});
    exports.entries.push_back({"_Combase_SMTC_put_IsNextEnabled", 0,
        [&manager](Marshaler& m) { m.set_return(manager.smtc_put_flag(m, 8)); }, 8});
    exports.entries.push_back({"_Combase_SMTC_get_IsChannelUpEnabled", 0,
        [&manager](Marshaler& m) { m.set_return(manager.smtc_get_flag(m, 9)); }, 8});
    exports.entries.push_back({"_Combase_SMTC_put_IsChannelUpEnabled", 0,
        [&manager](Marshaler& m) { m.set_return(manager.smtc_put_flag(m, 9)); }, 8});
    exports.entries.push_back({"_Combase_SMTC_get_IsChannelDownEnabled", 0,
        [&manager](Marshaler& m) { m.set_return(manager.smtc_get_flag(m, 10)); }, 8});
    exports.entries.push_back({"_Combase_SMTC_put_IsChannelDownEnabled", 0,
        [&manager](Marshaler& m) { m.set_return(manager.smtc_put_flag(m, 10)); }, 8});
    exports.entries.push_back({"_Combase_SMTC_add_ButtonPressed", 0,
        [&manager](Marshaler& m) { manager.impl_SmtcAddEvent(m); }, 12});
    exports.entries.push_back({"_Combase_SMTC_remove_ButtonPressed", 0,
        [&manager](Marshaler& m) { manager.impl_SmtcRemoveEvent(m); }, 12});
    exports.entries.push_back({"_Combase_SMTC_add_PropertyChanged", 0,
        [&manager](Marshaler& m) { manager.impl_SmtcAddEvent(m); }, 12});
    exports.entries.push_back({"_Combase_SMTC_remove_PropertyChanged", 0,
        [&manager](Marshaler& m) { manager.impl_SmtcRemoveEvent(m); }, 12});
    exports.entries.push_back({"_Combase_DU_get_Type", 0,
        [&manager](Marshaler& m) { manager.impl_DuGetType(m); }, 8});
    exports.entries.push_back({"_Combase_DU_put_Type", 0,
        [&manager](Marshaler& m) { manager.impl_DuPutType(m); }, 8});
    exports.entries.push_back({"_Combase_DU_get_AppMediaId", 0,
        [&manager](Marshaler& m) { manager.impl_DuGetAppMediaId(m); }, 8});
    exports.entries.push_back({"_Combase_DU_put_AppMediaId", 0,
        [&manager](Marshaler& m) { manager.impl_DuPutAppMediaId(m); }, 8});
    exports.entries.push_back({"_Combase_DU_get_Thumbnail", 0,
        [&manager](Marshaler& m) { manager.impl_DuGetThumbnail(m); }, 8});
    exports.entries.push_back({"_Combase_DU_put_Thumbnail", 0,
        [&manager](Marshaler& m) { manager.impl_DuPutThumbnail(m); }, 8});
    exports.entries.push_back({"_Combase_DU_get_MusicProperties", 0,
        [&manager](Marshaler& m) { manager.impl_DuGetMusicProperties(m); }, 8});
    exports.entries.push_back({"_Combase_DU_get_VideoProperties", 0,
        [&manager](Marshaler& m) { manager.impl_DuGetVideoProperties(m); }, 8});
    exports.entries.push_back({"_Combase_DU_get_ImageProperties", 0,
        [&manager](Marshaler& m) { manager.impl_DuGetImageProperties(m); }, 8});
    exports.entries.push_back({"_Combase_DU_CopyFromFileAsync", 0,
        [&manager](Marshaler& m) { manager.impl_DuCopyFromFileAsync(m); }, 16});
    exports.entries.push_back({"_Combase_DU_ClearAll", 0,
        [&manager](Marshaler& m) { manager.impl_DuClearAll(m); }, 4});
    exports.entries.push_back({"_Combase_DU_Update", 0,
        [&manager](Marshaler& m) { manager.impl_DuUpdate(m); }, 4});
    exports.entries.push_back({"_Combase_Music_get_Title", 0,
        [&manager](Marshaler& m) { m.set_return(manager.music_get_string(m, 0)); }, 8});
    exports.entries.push_back({"_Combase_Music_put_Title", 0,
        [&manager](Marshaler& m) { m.set_return(manager.music_put_string(m, 0)); }, 8});
    exports.entries.push_back({"_Combase_Music_get_AlbumArtist", 0,
        [&manager](Marshaler& m) { m.set_return(manager.music_get_string(m, 1)); }, 8});
    exports.entries.push_back({"_Combase_Music_put_AlbumArtist", 0,
        [&manager](Marshaler& m) { m.set_return(manager.music_put_string(m, 1)); }, 8});
    exports.entries.push_back({"_Combase_Music_get_Artist", 0,
        [&manager](Marshaler& m) { m.set_return(manager.music_get_string(m, 2)); }, 8});
    exports.entries.push_back({"_Combase_Music_put_Artist", 0,
        [&manager](Marshaler& m) { m.set_return(manager.music_put_string(m, 2)); }, 8});
    exports.entries.push_back({"_Combase_SMTC2_get_PlaybackRate", 0,
        [&manager](Marshaler& m) { manager.impl_Smtc2GetPlaybackRate(m); }, 8});
    exports.entries.push_back({"_Combase_SMTC2_put_PlaybackRate", 0,
        [&manager](Marshaler& m) { manager.impl_Smtc2PutPlaybackRate(m); }, 12});
    exports.entries.push_back({"_Combase_SMTC2_UpdateTimelineProperties", 0,
        [&manager](Marshaler& m) {
            manager.impl_Smtc2UpdateTimelineProperties(m);
        }, 8});
    exports.entries.push_back({"_Combase_AppCapability_RequestAccessForCapabilitiesAsync", 0,
        [&manager](Marshaler& m) {
            manager.impl_AppCapabilityRequestAccessForCapabilitiesAsync(m);
        }, 12});
    exports.entries.push_back({"_Combase_AppCapability_RequestAccessForCapabilitiesForUserAsync", 0,
        [&manager](Marshaler& m) {
            manager.impl_AppCapabilityRequestAccessForCapabilitiesForUserAsync(m);
        }, 16});
    exports.entries.push_back({"_Combase_NotImplemented", 0,
        [&manager](Marshaler& m) { manager.impl_WinrtNotImplemented(m); }, 16});
    exports.entries.push_back({"_Combase_Launcher_LaunchUriAsync", 0,
        [&manager](Marshaler& m) { manager.impl_LauncherLaunchUri(m); }, 12});
    exports.entries.push_back({"_Combase_UriFactory_CreateUri", 0,
        [&manager](Marshaler& m) { manager.impl_UriCreate(m); }, 12});
    exports.entries.push_back({"_Combase_Uri_GetText", 0,
        [&manager](Marshaler& m) { manager.impl_UriGetText(m); }, 8});
    // 135-144: ISystemMediaTransportControlsTimelineProperties, vtable order.
    // 145-148: ISystemMediaTransportControls2 AutoRepeatMode / ShuffleEnabled.
    exports.entries.push_back({"_Combase_Timeline_get_StartTime", 0,
        [&manager](Marshaler& m) { m.set_return(manager.timeline_get(m, 0)); }, 8});
    exports.entries.push_back({"_Combase_Timeline_put_StartTime", 0,
        [&manager](Marshaler& m) { m.set_return(manager.timeline_put(m, 0)); }, 12});
    exports.entries.push_back({"_Combase_Timeline_get_EndTime", 0,
        [&manager](Marshaler& m) { m.set_return(manager.timeline_get(m, 1)); }, 8});
    exports.entries.push_back({"_Combase_Timeline_put_EndTime", 0,
        [&manager](Marshaler& m) { m.set_return(manager.timeline_put(m, 1)); }, 12});
    exports.entries.push_back({"_Combase_Timeline_get_MinSeekTime", 0,
        [&manager](Marshaler& m) { m.set_return(manager.timeline_get(m, 2)); }, 8});
    exports.entries.push_back({"_Combase_Timeline_put_MinSeekTime", 0,
        [&manager](Marshaler& m) { m.set_return(manager.timeline_put(m, 2)); }, 12});
    exports.entries.push_back({"_Combase_Timeline_get_MaxSeekTime", 0,
        [&manager](Marshaler& m) { m.set_return(manager.timeline_get(m, 3)); }, 8});
    exports.entries.push_back({"_Combase_Timeline_put_MaxSeekTime", 0,
        [&manager](Marshaler& m) { m.set_return(manager.timeline_put(m, 3)); }, 12});
    exports.entries.push_back({"_Combase_Timeline_get_Position", 0,
        [&manager](Marshaler& m) { m.set_return(manager.timeline_get(m, 4)); }, 8});
    exports.entries.push_back({"_Combase_Timeline_put_Position", 0,
        [&manager](Marshaler& m) { m.set_return(manager.timeline_put(m, 4)); }, 12});
    exports.entries.push_back({"_Combase_SMTC2_get_AutoRepeatMode", 0,
        [&manager](Marshaler& m) { manager.impl_Smtc2GetAutoRepeatMode(m); }, 8});
    exports.entries.push_back({"_Combase_SMTC2_put_AutoRepeatMode", 0,
        [&manager](Marshaler& m) { manager.impl_Smtc2PutAutoRepeatMode(m); }, 8});
    exports.entries.push_back({"_Combase_SMTC2_get_ShuffleEnabled", 0,
        [&manager](Marshaler& m) { manager.impl_Smtc2GetShuffleEnabled(m); }, 8});
    exports.entries.push_back({"_Combase_SMTC2_put_ShuffleEnabled", 0,
        [&manager](Marshaler& m) { manager.impl_Smtc2PutShuffleEnabled(m); }, 8});
    // One manager lock for every guest entry (CombaseManager::api_mutex()):
    // WinRT activation, HSTRING and object-lifetime calls are free-threaded by
    // contract, and every object here lives in the manager's shared maps.
    for (ExportEntry& entry : exports.entries) {
        entry.handler = [&manager, inner = std::move(entry.handler)](
                            Marshaler& m) {
            std::scoped_lock lock(manager.api_mutex());
            inner(m);
        };
    }
    return exports;
}

} // namespace vitreo::winapi
