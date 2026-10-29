// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file combase.hpp
/// @brief Windows Runtime activation, HSTRING, and system preference contracts

#include "winapi/core/thunk_registry.hpp"
#include "hardware/contracts/bluetooth_host.hpp"
#include "hardware/contracts/now_playing.hpp"

#include <array>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace vitreo {
class GuestMemory;
}

namespace vitreo::winapi {

class HeapManager;
class Marshaler;

struct WinrtGlobalizationPreferences {
    std::vector<std::u16string> calendars;
    std::vector<std::u16string> clocks;
    std::vector<std::u16string> currencies;
    std::vector<std::u16string> languages;
    std::u16string home_geographic_region;
    u32 week_starts_on{0};
};

[[nodiscard]] WinrtGlobalizationPreferences detect_host_globalization_preferences();

#include <functional>

class ComCoreManager;

class CombaseManager {
public:
    CombaseManager(GuestMemory& memory,
                   HeapManager& heap,
                   WinrtGlobalizationPreferences preferences);

    /// How many "_Combase_*" method thunks the registry resolves, in the
    /// order create_combase_thunks exports them.
    static constexpr usize kMethodThunks = 149;
    void set_method_thunks(const std::array<GuestAddr, kMethodThunks>& thunks);

    /// One lock for every guest entry (create_combase_thunks wraps each
    /// export in it, the way D3D11Manager::m_api_mutex does): WinRT objects
    /// and HSTRINGs are process-wide and agile, so WindowsCreateString /
    /// AddRef / Release / RoGetActivationFactory run on every guest thread,
    /// and all of them live in m_objects / m_strings. Recursive because a
    /// handler re-enters through guest code (RoActivateInstance calls the
    /// guest factory, put_Completed invokes the guest delegate inline).
    /// Lock order: m_api_mutex -> ComCoreManager::m_com_mutex (handlers call
    /// into ComCore; ComCore never calls into this manager).
    [[nodiscard]] std::recursive_mutex& api_mutex() noexcept { return m_api_mutex; }

    /// Synchronous guest call, used to invoke a WinRT completion delegate.
    /// Same contract the loader and ddraw enumeration callbacks use.
    using GuestCall = std::function<u64(GuestAddr, const u64*, usize, bool)>;
    void set_guest_call(GuestCall call) { m_guest_call = std::move(call); }

    /// Host radio behind Windows.Devices.Bluetooth. Null means the platform
    /// has no bridge, which reads to the guest as "no adapter".
    void set_bluetooth_host(hardware::BluetoothHost* host) {
        m_bluetooth_host = host;
    }

    /// Host now-playing surface behind SystemMediaTransportControls.
    void set_now_playing_host(hardware::NowPlayingHost* host) {
        m_now_playing_host = host;
    }

    void impl_RoGetActivationFactory(Marshaler& m);
    void impl_WindowsCreateString(Marshaler& m);
    void impl_WindowsCreateStringReference(Marshaler& m);
    void impl_WindowsCompareStringOrdinal(Marshaler& m);
    void impl_WindowsDeleteString(Marshaler& m);
    void impl_WindowsGetStringRawBuffer(Marshaler& m);
    void impl_WindowsDuplicateString(Marshaler& m);
    void impl_WindowsGetStringLen(Marshaler& m);
    void impl_WindowsStringHasEmbeddedNull(Marshaler& m);
    void impl_WindowsIsStringEmpty(Marshaler& m);

    void impl_QueryInterface(Marshaler& m);
    void impl_AddRef(Marshaler& m);
    void impl_Release(Marshaler& m);
    void impl_GetIids(Marshaler& m);
    void impl_GetRuntimeClassName(Marshaler& m);
    void impl_GetTrustLevel(Marshaler& m);
    void impl_GetCalendars(Marshaler& m);
    void impl_GetClocks(Marshaler& m);
    void impl_GetCurrencies(Marshaler& m);
    void impl_GetLanguages(Marshaler& m);
    void impl_GetHomeGeographicRegion(Marshaler& m);
    void impl_GetWeekStartsOn(Marshaler& m);
    void impl_VectorGetAt(Marshaler& m);
    void impl_VectorGetSize(Marshaler& m);
    void impl_VectorIndexOf(Marshaler& m);
    void impl_VectorGetMany(Marshaler& m);
    void impl_ActivateInstance(Marshaler& m);

    // Windows.Data.Json.JsonValue — IJsonValueStatics then IJsonValue.
    void impl_JsonValueParse(Marshaler& m);
    void impl_JsonValueTryParse(Marshaler& m);
    void impl_JsonValueCreateBoolean(Marshaler& m);
    void impl_JsonValueCreateNumber(Marshaler& m);
    void impl_JsonValueCreateString(Marshaler& m);
    void impl_JsonValueCreateNull(Marshaler& m);
    void impl_JsonValueGetValueType(Marshaler& m);
    void impl_JsonValueStringify(Marshaler& m);
    void impl_JsonValueGetString(Marshaler& m);
    void impl_JsonValueGetNumber(Marshaler& m);
    void impl_JsonValueGetBoolean(Marshaler& m);
    void impl_JsonValueGetArray(Marshaler& m);
    void impl_JsonValueGetObject(Marshaler& m);

    // Windows.Data.Json.JsonObject — IJsonObject.
    void impl_JsonObjectGetNamedValue(Marshaler& m);
    void impl_JsonObjectSetNamedValue(Marshaler& m);
    void impl_JsonObjectGetNamedObject(Marshaler& m);
    void impl_JsonObjectGetNamedArray(Marshaler& m);
    void impl_JsonObjectGetNamedString(Marshaler& m);
    void impl_JsonObjectGetNamedNumber(Marshaler& m);
    void impl_JsonObjectGetNamedBoolean(Marshaler& m);

    // Windows.Devices.Bluetooth.BluetoothAdapter — IBluetoothAdapterStatics,
    // the IAsyncOperation it returns, IAsyncInfo, and IBluetoothAdapter.
    void impl_BluetoothStaticsGetDeviceSelector(Marshaler& m);
    void impl_BluetoothStaticsFromIdAsync(Marshaler& m);
    void impl_BluetoothStaticsGetDefaultAsync(Marshaler& m);
    void impl_AsyncPutCompleted(Marshaler& m);
    void impl_AsyncGetCompleted(Marshaler& m);
    void impl_AsyncGetResults(Marshaler& m);
    void impl_AsyncInfoGetId(Marshaler& m);
    void impl_AsyncInfoGetStatus(Marshaler& m);
    void impl_AsyncInfoGetErrorCode(Marshaler& m);
    void impl_AsyncInfoCancel(Marshaler& m);
    void impl_AsyncInfoClose(Marshaler& m);
    void impl_BluetoothAdapterGetDeviceId(Marshaler& m);
    void impl_BluetoothAdapterGetAddress(Marshaler& m);
    void impl_BluetoothAdapterGetClassicSupported(Marshaler& m);
    void impl_BluetoothAdapterGetLowEnergySupported(Marshaler& m);
    void impl_BluetoothAdapterGetPeripheralRoleSupported(Marshaler& m);
    void impl_BluetoothAdapterGetCentralRoleSupported(Marshaler& m);
    void impl_BluetoothAdapterGetAdvertisementOffloadSupported(Marshaler& m);
    void impl_BluetoothAdapterGetRadioAsync(Marshaler& m);

    // Windows.Devices.Radios.Radio — the radio behind the adapter.
    void impl_RadioSetStateAsync(Marshaler& m);
    void impl_RadioAddStateChanged(Marshaler& m);
    void impl_RadioRemoveStateChanged(Marshaler& m);
    void impl_RadioGetState(Marshaler& m);
    void impl_RadioGetName(Marshaler& m);
    void impl_RadioGetKind(Marshaler& m);

    // IWeakReferenceSource / IWeakReference. cppwinrt asks every WinRT object
    // for a weak reference; both interfaces derive from IUnknown, so each
    // needs its own vtable rather than the object's IInspectable one.
    void impl_GetWeakReference(Marshaler& m);
    void impl_WeakReferenceResolve(Marshaler& m);

    // Windows.UI.Notifications.ToastNotificationManager
    void impl_ToastCreateNotifier(Marshaler& m);
    void impl_ToastCreateNotifierWithId(Marshaler& m);
    void impl_ToastGetTemplateContent(Marshaler& m);
    // Windows.Security.Credentials.KeyCredentialManager
    void impl_KeyCredIsSupportedAsync(Marshaler& m);
    void impl_KeyCredRenewAttestationAsync(Marshaler& m);
    void impl_KeyCredRequestCreateAsync(Marshaler& m);
    void impl_KeyCredOpenAsync(Marshaler& m);
    void impl_KeyCredDeleteAsync(Marshaler& m);
    // Windows.UI.ViewManagement.UIViewSettings
    void impl_UIViewSettingsGetForWindow(Marshaler& m);
    void impl_UIViewSettingsGetUserInteractionMode(Marshaler& m);
    // Windows.Devices.Input.PenDevice
    void impl_PenDeviceGetFromPointerId(Marshaler& m);
    // Windows.Security.Authorization.AppCapabilityAccess.AppCapability
    void impl_AppCapabilityRequestAccessForCapabilitiesAsync(Marshaler& m);
    void impl_AppCapabilityRequestAccessForCapabilitiesForUserAsync(Marshaler& m);
    void impl_AppCapabilityCreate(Marshaler& m);
    void impl_AppCapabilityCreateForUser(Marshaler& m);
    // Windows.Foundation.Uri and Windows.System.Launcher. Desktop login opens
    // the default browser through ILauncherStatics::LaunchUriAsync.
    void impl_UriCreate(Marshaler& m);
    void impl_UriGetText(Marshaler& m);
    void impl_LauncherLaunchUri(Marshaler& m);
    void impl_WinrtNotImplemented(Marshaler& m);

    // Windows.Media.SystemMediaTransportControls. A desktop app reaches it
    // through ISystemMediaTransportControlsInterop::GetForWindow, which
    // Microsoft documents as being implemented by the activation factory.
    void impl_SmtcGetForWindow(Marshaler& m);
    void impl_SmtcGetPlaybackStatus(Marshaler& m);
    void impl_SmtcPutPlaybackStatus(Marshaler& m);
    void impl_SmtcGetDisplayUpdater(Marshaler& m);
    void impl_SmtcGetSoundLevel(Marshaler& m);
    void impl_SmtcAddEvent(Marshaler& m);
    void impl_SmtcRemoveEvent(Marshaler& m);
    void impl_Smtc2GetPlaybackRate(Marshaler& m);
    void impl_Smtc2PutPlaybackRate(Marshaler& m);
    void impl_Smtc2UpdateTimelineProperties(Marshaler& m);
    void impl_Smtc2GetAutoRepeatMode(Marshaler& m);
    void impl_Smtc2PutAutoRepeatMode(Marshaler& m);
    void impl_Smtc2GetShuffleEnabled(Marshaler& m);
    void impl_Smtc2PutShuffleEnabled(Marshaler& m);
    /// SystemMediaTransportControlsTimelineProperties: @p field is StartTime,
    /// EndTime, MinSeekTime, MaxSeekTime, Position (0..4, vtable order).
    [[nodiscard]] u32 timeline_get(Marshaler& m, u32 field);
    [[nodiscard]] u32 timeline_put(Marshaler& m, u32 field);
    /// The fourteen boolean button/enable properties share one pair of
    /// accessors; @p bit selects which flag the slot refers to.
    [[nodiscard]] u32 smtc_get_flag(Marshaler& m, u32 bit);
    [[nodiscard]] u32 smtc_put_flag(Marshaler& m, u32 bit);

    // SystemMediaTransportControlsDisplayUpdater and MusicDisplayProperties.
    void impl_DuGetType(Marshaler& m);
    void impl_DuPutType(Marshaler& m);
    void impl_DuGetAppMediaId(Marshaler& m);
    void impl_DuPutAppMediaId(Marshaler& m);
    void impl_DuGetThumbnail(Marshaler& m);
    void impl_DuPutThumbnail(Marshaler& m);
    void impl_DuGetMusicProperties(Marshaler& m);
    void impl_DuGetVideoProperties(Marshaler& m);
    void impl_DuGetImageProperties(Marshaler& m);
    void impl_DuCopyFromFileAsync(Marshaler& m);
    void impl_DuClearAll(Marshaler& m);
    void impl_DuUpdate(Marshaler& m);
    /// The three MusicDisplayProperties strings share one pair of accessors;
    /// @p field selects Title / AlbumArtist / Artist.
    [[nodiscard]] u32 music_get_string(Marshaler& m, u32 field);
    [[nodiscard]] u32 music_put_string(Marshaler& m, u32 field);

    /// Text of an HSTRING (NULL = empty) for callers outside the manager.
    [[nodiscard]] std::optional<std::u16string> hstring_value(GuestAddr string,
                                                              bool is_64bit) const {
        return read_hstring(string, is_64bit);
    }

private:
    enum class ObjectType {
        GlobalizationPreferences,
        StringVectorView,
        // Windows.Data.Json.JsonObject activation factory and its instances.
        JsonObjectFactory,
        JsonObject,
        // Windows.Data.Json.JsonValue: the IJsonValueStatics factory and the
        // IJsonValue instances it hands out.
        JsonValueFactory,
        JsonValue,
        // Windows.Devices.Bluetooth.
        BluetoothAdapterFactory,
        BluetoothAdapter,
        AsyncOperation,
        AsyncInfo,
        Radio,
        WeakReferenceSource,
        WeakReference,
        ToastManagerFactory,
        MediaTransportControlsFactory,
        KeyCredentialManagerFactory,
        UIViewSettingsFactory,
        UIViewSettings,
        PenDeviceFactory,
        AppCapabilityFactory,
        MediaTransportControlsInterop,
        MediaTransportControls,
        MediaTransportControls2,
        DisplayUpdater,
        MusicDisplayProperties,
        LauncherFactory,
        UriFactory,
        Uri,
        // Windows.Media.SystemMediaTransportControlsTimelineProperties.
        TimelinePropertiesFactory,
        TimelineProperties,
    };

    /// Windows.Data.Json.JsonValueType (Windows.Data.Json.idl).
    enum class JsonType : u32 {
        Null = 0,
        Boolean = 1,
        Number = 2,
        String = 3,
        Array = 4,
        Object = 5,
    };

    struct Object {
        ObjectType type{ObjectType::GlobalizationPreferences};
        u32 ref_count{1};
        std::vector<std::u16string> values;
        // JsonValue payload. `values[0]` carries the string form.
        JsonType json_type{JsonType::Null};
        double json_number{0.0};
        bool json_boolean{false};
        /// JsonObject members, in insertion order (IJsonObject is an ordered
        /// IMap<String, IJsonValue> as far as guests can observe).
        std::vector<std::pair<std::u16string, GuestAddr>> json_members;
        /// AsyncOperation: the completed result (0 = null result) and the
        /// AsyncStatus the guest observes. Vitreo resolves every operation
        /// before handing it out, so the status is never Started.
        GuestAddr async_result{0};
        u32 async_status{1};   // AsyncStatus::Completed
        GuestAddr async_peer{0};  // operation <-> info view
        /// WeakReferenceSource / WeakReference: the object they refer to.
        GuestAddr weak_target{0};
        /// AsyncOperation with a by-value result (IAsyncOperation<bool> and
        /// friends): GetResults writes @p async_scalar of this width instead
        /// of an interface pointer. 0 means the result is an interface.
        u32 async_scalar_size{0};
        u64 async_scalar{0};
        /// SystemMediaTransportControls: the fourteen boolean properties as a
        /// bitmask, plus the playback status. The object IS its state — every
        /// one of these is a setter/getter pair the guest reads back.
        u32 smtc_flags{0};
        u32 smtc_playback_status{0};
        /// ISystemMediaTransportControls2.PlaybackRate starts at 0
        /// (smtc_timeline_probe "PlaybackRate rate=0").
        double smtc_playback_rate{0.0};
        /// ISystemMediaTransportControls2's own state (smtc_timeline_probe):
        /// AutoRepeatMode (None), ShuffleEnabled, and the timeline last passed
        /// to UpdateTimelineProperties.
        u32 smtc_auto_repeat{0};
        bool smtc_shuffle{false};
        bool smtc_has_timeline{false};
        std::array<i64, 5> smtc_timeline{};
        /// The window's controls: the v1 object holds the state, and its v2
        /// view points back at it (smtc_peer both ways).
        GuestAddr smtc_peer{0};
        /// TimelineProperties: StartTime, EndTime, MinSeekTime, MaxSeekTime,
        /// Position, in TimeSpan ticks (100 ns).
        std::array<i64, 5> timeline{};
        /// WinRT event subscriptions owned by this SMTC instance. A
        /// successful add accessor returns a nonzero EventRegistrationToken
        /// identifying the stored delegate; remove consumes that token.
        u64 smtc_next_event_token{1};
        std::vector<std::pair<u64, GuestAddr>> smtc_event_handlers;
        /// DisplayUpdater: the owning controls, its MusicDisplayProperties
        /// child, the MediaPlaybackType and the AppMediaId.
        GuestAddr du_owner{0};
        GuestAddr du_music{0};
        u32 du_type{0};
        std::u16string du_app_media_id;
        /// MusicDisplayProperties: Title, AlbumArtist, Artist.
        std::array<std::u16string, 3> music_fields{};
        /// BluetoothAdapter: the host radio snapshot this object reports.
        hardware::BluetoothAdapterState adapter{};
    };

    struct OwnedHString {
        std::u16string value;
        GuestAddr buffer{0};
        bool is_64bit{false};
    };

    [[nodiscard]] bool ensure_storage();
    [[nodiscard]] bool write_pointer(
        GuestAddr address, GuestAddr value, bool is_64bit);
    [[nodiscard]] GuestAddr allocate_object(
        Object object, bool is_64bit);
    [[nodiscard]] GuestAddr create_json_value(
        JsonType type, double number, bool boolean,
        std::u16string text, bool is_64bit);
    /// Shared tail of every IJsonValue accessor: resolves `this`, checks it is
    /// a JsonValue and that the caller passed an out parameter.
    [[nodiscard]] Object* json_value_arg(Marshaler& m, GuestAddr* out_param);
    /// Resolves `this` as a JsonObject plus the HSTRING name argument.
    [[nodiscard]] Object* json_object_arg(Marshaler& m,
                                          std::u16string* name,
                                          GuestAddr* out_param);
    [[nodiscard]] GuestAddr create_completed_async(GuestAddr result,
                                                   bool is_64bit);
    [[nodiscard]] Object* async_arg(Marshaler& m, ObjectType type);
    [[nodiscard]] Object* adapter_arg(Marshaler& m, GuestAddr* out_param);
    [[nodiscard]] Object* json_member(Object& owner,
                                      const std::u16string& name);
    [[nodiscard]] GuestAddr create_vector_view(
        const std::vector<std::u16string>& values, bool is_64bit);
    [[nodiscard]] GuestAddr create_hstring(
        std::u16string_view value, bool is_64bit);
    /// Invoke IUnknown::AddRef (slot 1) or Release (slot 2) on a guest-owned
    /// interface through the nested guest-call bridge.
    [[nodiscard]] u64 guest_iunknown_ref(
        Marshaler& m, GuestAddr object, usize slot);
    [[nodiscard]] std::optional<std::u16string> read_hstring(
        GuestAddr string, bool is_64bit) const;
    [[nodiscard]] std::optional<GuestAddr> hstring_buffer(
        GuestAddr string, u32* length, bool is_64bit) const;
    [[nodiscard]] bool object_supports_iid(
        ObjectType type, GuestAddr iid_address) const;
    [[nodiscard]] u32 return_vector(Marshaler& m,
                                    const std::vector<std::u16string>& values);
    [[nodiscard]] GuestAddr allocate_storage(usize bytes, usize alignment);

    GuestMemory& m_memory;
    HeapManager& m_heap;
    WinrtGlobalizationPreferences m_preferences;
    std::array<GuestAddr, kMethodThunks> m_method_thunks{};
    GuestAddr m_region_base{0};
    std::array<GuestAddr, 2> m_factory_vtables{};
    std::array<GuestAddr, 2> m_vector_vtables{};
    // IActivationFactory (IInspectable + ActivateInstance) and the activated
    // JsonObject instance, which currently publishes IInspectable only.
    std::array<GuestAddr, 2> m_json_factory_vtables{};
    std::array<GuestAddr, 2> m_json_object_vtables{};
    std::array<GuestAddr, 2> m_json_factories{};
    std::array<GuestAddr, 2> m_json_value_statics_vtables{};
    std::array<GuestAddr, 2> m_json_value_vtables{};
    std::array<GuestAddr, 2> m_json_value_factories{};
    std::array<GuestAddr, 2> m_bt_statics_vtables{};
    std::array<GuestAddr, 2> m_bt_adapter_vtables{};
    std::array<GuestAddr, 2> m_async_op_vtables{};
    std::array<GuestAddr, 2> m_async_info_vtables{};
    std::array<GuestAddr, 2> m_radio_vtables{};
    std::array<GuestAddr, 2> m_weak_source_vtables{};
    std::array<GuestAddr, 2> m_weak_ref_vtables{};
    std::array<GuestAddr, 2> m_toast_vtables{};
    std::array<GuestAddr, 2> m_smtc_vtables{};
    std::array<GuestAddr, 2> m_keycred_vtables{};
    std::array<GuestAddr, 2> m_uiview_statics_vtables{};
    std::array<GuestAddr, 2> m_uiview_vtables{};
    std::array<GuestAddr, 2> m_pen_vtables{};
    std::array<GuestAddr, 2> m_appcap_vtables{};
    std::array<GuestAddr, 2> m_smtc_interop_vtables{};
    std::array<GuestAddr, 2> m_smtc_object_vtables{};
    std::array<GuestAddr, 2> m_smtc2_object_vtables{};
    std::array<GuestAddr, 2> m_du_vtables{};
    std::array<GuestAddr, 2> m_music_vtables{};
    std::array<GuestAddr, 2> m_launcher_vtables{};
    std::array<GuestAddr, 2> m_uri_factory_vtables{};
    std::array<GuestAddr, 2> m_uri_vtables{};
    std::array<GuestAddr, 2> m_timeline_factory_vtables{};
    std::array<GuestAddr, 2> m_timeline_vtables{};
    hardware::NowPlayingHost* m_now_playing_host{nullptr};
    /// Pushes @p updater's accumulated metadata to the host surface.
    void publish_now_playing(const Object& updater);
    std::array<GuestAddr, 2> m_toast_factories{};
    std::array<GuestAddr, 2> m_smtc_factories{};
    std::array<GuestAddr, 2> m_keycred_factories{};
    std::array<GuestAddr, 2> m_uiview_factories{};
    std::array<GuestAddr, 2> m_pen_factories{};
    std::array<GuestAddr, 2> m_appcap_factories{};
    std::array<GuestAddr, 2> m_launcher_factories{};
    std::array<GuestAddr, 2> m_uri_factories{};
    std::array<GuestAddr, 2> m_timeline_factories{};
    /// GetForWindow's controls, one per (window, bitness): the v1 object.
    std::unordered_map<u64, GuestAddr> m_smtc_by_window;
    /// The object that holds an SMTC view's state (the v1 controls).
    [[nodiscard]] std::unordered_map<GuestAddr, Object>::iterator smtc_state(GuestAddr self);
    /// The v1 or v2 view of the controls @p primary, made on first use.
    [[nodiscard]] GuestAddr smtc_view(GuestAddr primary, bool v2, bool is_64bit);
    /// Re-publish now playing when the controls have a display updater.
    void republish_now_playing(GuestAddr controls);
    /// The ISystemMediaTransportControls2 accessors run on that view only; the
    /// state they touch lives in the controls it belongs to.
    [[nodiscard]] bool is_controls2(GuestAddr self) const;
    /// Completed IAsyncOperation whose result is a by-value scalar.
    [[nodiscard]] GuestAddr create_completed_async_scalar(
        u64 value, u32 width, bool is_64bit);
    std::array<GuestAddr, 2> m_bt_factories{};
    GuestCall m_guest_call;
    hardware::BluetoothHost* m_bluetooth_host{nullptr};
    GuestAddr m_next_object{0};
    GuestAddr m_next_storage{0};
    GuestAddr m_empty_buffer{0};
    std::array<GuestAddr, 2> m_factories{};
    /// Every cached activation-factory slot. A factory released by the guest
    /// must be evicted from all of them: a slot left holding a freed address
    /// would later be handed back, or reused by an unrelated object allocated
    /// at the same guest address.
    [[nodiscard]] std::array<std::array<GuestAddr, 2>*, 13> factory_caches();
    /// Touched only under m_api_mutex (see api_mutex()). Unit tests that call
    /// impl_* directly bypass the lock; the thunk-level test goes through
    /// create_combase_thunks.
    std::unordered_map<GuestAddr, Object> m_objects;
    std::unordered_map<GuestAddr, OwnedHString> m_strings;
    std::recursive_mutex m_api_mutex;
};

[[nodiscard]] DllExports create_combase_thunks(CombaseManager& manager,
                                               ComCoreManager& com);

} // namespace vitreo::winapi
