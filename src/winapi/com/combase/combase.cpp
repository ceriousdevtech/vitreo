// SPDX-License-Identifier: LGPL-2.1-only
/// @file combase.cpp
/// @brief combase manager core: storage region, vtable layout, object allocation

#include "winapi/com/combase/combase.hpp"
#include "winapi/com/combase/combase_internal.hpp"

#include <algorithm>

namespace vitreo::winapi {

namespace {

constexpr usize STORAGE_SIZE = 0x40000;
constexpr u32 FACTORY_VTABLE_SLOTS = 12;
constexpr u32 VECTOR_VTABLE_SLOTS = 10;
constexpr u32 ACTIVATION_FACTORY_VTABLE_SLOTS = 7;
constexpr u32 INSPECTABLE_VTABLE_SLOTS = 6;
// IJsonValueStatics: IInspectable + Parse/TryParse/CreateBoolean/CreateNumber/
// CreateString/CreateNull. IJsonValue: IInspectable + ValueType/Stringify/
// GetString/GetNumber/GetBoolean/GetArray/GetObject (Windows.Data.Json.idl).
constexpr u32 JSON_VALUE_STATICS_VTABLE_SLOTS = 12;
constexpr u32 JSON_VALUE_VTABLE_SLOTS = 13;
// IJsonObject: IInspectable + GetNamedValue/SetNamedValue/GetNamedObject/
// GetNamedArray/GetNamedString/GetNamedNumber/GetNamedBoolean.
constexpr u32 JSON_OBJECT_VTABLE_SLOTS = 13;
// Windows.Devices.Bluetooth.idl / Windows.Foundation.idl.
constexpr u32 BT_STATICS_VTABLE_SLOTS = 9;    // + GetDeviceSelector/FromId/GetDefault
constexpr u32 BT_ADAPTER_VTABLE_SLOTS = 14;   // + 7 properties + GetRadioAsync
constexpr u32 ASYNC_OP_VTABLE_SLOTS = 9;      // + put/get_Completed, GetResults
constexpr u32 ASYNC_INFO_VTABLE_SLOTS = 11;   // + Id/Status/ErrorCode/Cancel/Close
// IRadio: IInspectable + SetStateAsync, add/remove_StateChanged, State, Name,
// Kind (Windows.Devices.Radios.idl).
constexpr u32 RADIO_VTABLE_SLOTS = 12;
// IWeakReferenceSource and IWeakReference derive from IUnknown: three
// IUnknown slots then the single method.
constexpr u32 WEAK_VTABLE_SLOTS = 4;
// Statics interfaces from their respective Windows IDLs.
constexpr u32 TOAST_VTABLE_SLOTS = 9;      // + CreateToastNotifier{,WithId}, GetTemplateContent
constexpr u32 SMTC_VTABLE_SLOTS = 7;       // IActivationFactory only
constexpr u32 KEYCRED_VTABLE_SLOTS = 11;   // + IsSupported/Renew/RequestCreate/Open/Delete
// IUIViewSettingsInterop: IInspectable + GetForWindow.
constexpr u32 UIVIEW_INTEROP_VTABLE_SLOTS = 7;
constexpr u32 UIVIEW_VTABLE_SLOTS = 7;     // + get_UserInteractionMode
constexpr u32 PEN_VTABLE_SLOTS = 7;        // + GetFromPointerId
// IAppCapabilityStatics: IInspectable + RequestAccessForCapabilitiesAsync,
// RequestAccessForCapabilitiesForUserAsync, Create, CreateWithProcessIdForUser.
constexpr u32 APPCAP_VTABLE_SLOTS = 10;
// ISystemMediaTransportControlsInterop: IInspectable + GetForWindow.
constexpr u32 SMTC_INTEROP_VTABLE_SLOTS = 7;
// ISystemMediaTransportControls: IInspectable + the 30 members below, in the
// declaration order MIDL emits (properties then events, get before put).
constexpr u32 SMTC_OBJECT_VTABLE_SLOTS = 36;
// ISystemMediaTransportControls2: IInspectable + AutoRepeatMode,
// ShuffleEnabled, PlaybackRate, UpdateTimelineProperties, and four event
// add/remove pairs (Windows.Media.idl declaration order).
constexpr u32 SMTC2_OBJECT_VTABLE_SLOTS = 21;
// ISystemMediaTransportControlsDisplayUpdater: IInspectable + 12 members.
// ClearAll precedes Update in MIDL order — the docs list them alphabetically.
constexpr u32 DU_VTABLE_SLOTS = 18;
// IMusicDisplayProperties: IInspectable + Title/AlbumArtist/Artist get+put.
constexpr u32 MUSIC_VTABLE_SLOTS = 12;     // + Create, CreateWithProcessIdForUser
// ILauncherStatics: LaunchFile, LaunchFileWithOptions, LaunchUri,
// LaunchUriWithOptions. windows.system.h, SDK 10.0.26100.0.
constexpr u32 LAUNCHER_VTABLE_SLOTS = 10;
// IUriRuntimeClassFactory: CreateUri, CreateWithRelativeUri.
constexpr u32 URI_FACTORY_VTABLE_SLOTS = 8;
// IUriRuntimeClass: AbsoluteUri through CombineUri (17 methods).
constexpr u32 URI_VTABLE_SLOTS = 23;
// SystemMediaTransportControlsTimelineProperties: its activation factory
// (IActivationFactory) and ISystemMediaTransportControlsTimelineProperties,
// IInspectable + get/put StartTime, EndTime, MinSeekTime, MaxSeekTime,
// Position (windows.media.h order).
constexpr u32 TIMELINE_FACTORY_VTABLE_SLOTS = 7;
constexpr u32 TIMELINE_VTABLE_SLOTS = 16;
constexpr u32 TIMELINE_FIRST_THUNK = 135;
// ISystemMediaTransportControls2 get/put AutoRepeatMode and ShuffleEnabled.
constexpr u32 SMTC2_REPEAT_SHUFFLE_FIRST_THUNK = 145;

} // namespace

CombaseManager::CombaseManager(
    GuestMemory& memory,
    HeapManager& heap,
    WinrtGlobalizationPreferences preferences)
    : m_memory(memory),
      m_heap(heap),
      m_preferences(std::move(preferences)) {}

void CombaseManager::set_method_thunks(
    const std::array<GuestAddr, kMethodThunks>& thunks) {
    std::copy(thunks.begin(), thunks.end(), m_method_thunks.begin());
}

bool CombaseManager::write_pointer(
    GuestAddr address, GuestAddr value, bool is_64bit) {
    if (is_64bit) return m_memory.write_u64(address, value).is_ok();
    return m_memory.write_u32(address, static_cast<u32>(value)).is_ok();
}

bool CombaseManager::ensure_storage() {
    if (m_region_base != 0) return true;
    if (std::any_of(m_method_thunks.begin(), m_method_thunks.end(),
                    [](GuestAddr thunk) { return thunk == 0; })) {
        return false;
    }

    GuestAddr minimum = 0x62000000ull;
    while (minimum != 0) {
        const GuestAddr base = m_memory.find_free_gap(minimum, STORAGE_SIZE);
        if (base == 0) return false;
        if (m_memory.map_region(
                base, STORAGE_SIZE, MemoryProtection::ReadWrite).is_ok()) {
            m_region_base = base;
            break;
        }
        minimum = base + STORAGE_SIZE;
    }
    if (m_region_base == 0) return false;

    m_factory_vtables = {m_region_base, m_region_base + 0x400};
    m_vector_vtables = {m_region_base + 0x200, m_region_base + 0x600};
    m_json_factory_vtables = {
        m_region_base + 0x800, m_region_base + 0xA00};
    m_json_object_vtables = {
        m_region_base + 0xC00, m_region_base + 0xE00};
    m_json_value_statics_vtables = {
        m_region_base + 0x1000, m_region_base + 0x1200};
    m_json_value_vtables = {
        m_region_base + 0x1400, m_region_base + 0x1600};
    m_bt_statics_vtables = {
        m_region_base + 0x1800, m_region_base + 0x1A00};
    m_bt_adapter_vtables = {
        m_region_base + 0x1C00, m_region_base + 0x1E00};
    m_async_op_vtables = {
        m_region_base + 0x2000, m_region_base + 0x2200};
    m_async_info_vtables = {
        m_region_base + 0x2400, m_region_base + 0x2600};
    m_radio_vtables = {
        m_region_base + 0x2800, m_region_base + 0x2A00};
    m_weak_source_vtables = {
        m_region_base + 0x2C00, m_region_base + 0x2D00};
    m_weak_ref_vtables = {
        m_region_base + 0x2E00, m_region_base + 0x2F00};
    m_toast_vtables = {m_region_base + 0x3000, m_region_base + 0x3200};
    m_smtc_vtables = {m_region_base + 0x3400, m_region_base + 0x3600};
    m_keycred_vtables = {m_region_base + 0x3800, m_region_base + 0x3A00};
    m_uiview_statics_vtables = {m_region_base + 0x3C00, m_region_base + 0x3E00};
    m_uiview_vtables = {m_region_base + 0x4000, m_region_base + 0x4200};
    m_pen_vtables = {m_region_base + 0x4400, m_region_base + 0x4600};
    m_appcap_vtables = {m_region_base + 0x4800, m_region_base + 0x4A00};
    m_smtc_interop_vtables = {m_region_base + 0x4C00, m_region_base + 0x4E00};
    m_smtc_object_vtables = {m_region_base + 0x5000, m_region_base + 0x5400};
    m_du_vtables = {m_region_base + 0x5800, m_region_base + 0x5A00};
    m_music_vtables = {m_region_base + 0x5C00, m_region_base + 0x5E00};
    m_smtc2_object_vtables = {
        m_region_base + 0x6000, m_region_base + 0x6400};
    m_launcher_vtables = {m_region_base + 0x6800, m_region_base + 0x6A00};
    m_uri_factory_vtables = {m_region_base + 0x6C00, m_region_base + 0x6E00};
    m_uri_vtables = {m_region_base + 0x7000, m_region_base + 0x7200};
    m_timeline_factory_vtables = {m_region_base + 0x7400, m_region_base + 0x7600};
    m_timeline_vtables = {m_region_base + 0x7800, m_region_base + 0x7A00};
    m_next_object = m_region_base + 0x7C00;
    m_next_storage = m_region_base + 0x10000;
    m_empty_buffer = m_next_storage;
    if (m_memory.write_u16(m_empty_buffer, 0).is_err()) return false;
    m_next_storage += 2;

    for (usize mode = 0; mode < 2; ++mode) {
        const bool is_64bit = mode != 0;
        const usize pointer_size = is_64bit ? 8 : 4;
        for (u32 slot = 0; slot < FACTORY_VTABLE_SLOTS; ++slot) {
            if (!write_pointer(
                    m_factory_vtables[mode] + slot * pointer_size,
                    m_method_thunks[slot], is_64bit)) {
                return false;
            }
        }
        for (u32 slot = 0; slot < VECTOR_VTABLE_SLOTS; ++slot) {
            const GuestAddr thunk =
                slot < 6 ? m_method_thunks[slot]
                         : m_method_thunks[12 + (slot - 6)];
            if (!write_pointer(
                    m_vector_vtables[mode] + slot * pointer_size,
                    thunk, is_64bit)) {
                return false;
            }
        }
        for (u32 slot = 0; slot < ACTIVATION_FACTORY_VTABLE_SLOTS; ++slot) {
            const GuestAddr thunk =
                slot < INSPECTABLE_VTABLE_SLOTS
                    ? m_method_thunks[slot]
                    : m_method_thunks[16];
            if (!write_pointer(
                    m_json_factory_vtables[mode] + slot * pointer_size,
                    thunk, is_64bit)) {
                return false;
            }
        }
        struct DerivedVtable {
            const std::array<GuestAddr, 2>& table;
            u32 slots;
            u32 first_thunk;
        };
        const DerivedVtable derived[] = {
            {m_bt_statics_vtables, BT_STATICS_VTABLE_SLOTS, 37},
            {m_async_op_vtables, ASYNC_OP_VTABLE_SLOTS, 40},
            {m_async_info_vtables, ASYNC_INFO_VTABLE_SLOTS, 43},
            {m_bt_adapter_vtables, BT_ADAPTER_VTABLE_SLOTS, 48},
            {m_radio_vtables, RADIO_VTABLE_SLOTS, 56},
            {m_toast_vtables, TOAST_VTABLE_SLOTS, 64},
            {m_keycred_vtables, KEYCRED_VTABLE_SLOTS, 67},
            {m_uiview_statics_vtables, UIVIEW_INTEROP_VTABLE_SLOTS, 72},
            {m_uiview_vtables, UIVIEW_VTABLE_SLOTS, 73},
            {m_pen_vtables, PEN_VTABLE_SLOTS, 74},
            {m_smtc_interop_vtables, SMTC_INTEROP_VTABLE_SLOTS, 77},
            {m_smtc_object_vtables, SMTC_OBJECT_VTABLE_SLOTS, 78},
            {m_du_vtables, DU_VTABLE_SLOTS, 108},
            {m_music_vtables, MUSIC_VTABLE_SLOTS, 120},
        };
        // ISystemMediaTransportControls2: AutoRepeatMode and ShuffleEnabled
        // are its own state (smtc_timeline_probe: setting them leaves v1's
        // PlaybackStatus and IsEnabled alone), then PlaybackRate and
        // UpdateTimelineProperties; its four events share the controls'
        // registration accessors.
        constexpr std::array<u32, 15> smtc2_thunks = {
            SMTC2_REPEAT_SHUFFLE_FIRST_THUNK, SMTC2_REPEAT_SHUFFLE_FIRST_THUNK + 1,
            SMTC2_REPEAT_SHUFFLE_FIRST_THUNK + 2, SMTC2_REPEAT_SHUFFLE_FIRST_THUNK + 3,
            126, 127, 128,
            104, 105, 104, 105, 104, 105, 104, 105,
        };
        for (u32 slot = 0; slot < SMTC2_OBJECT_VTABLE_SLOTS; ++slot) {
            const GuestAddr thunk =
                slot < INSPECTABLE_VTABLE_SLOTS
                    ? m_method_thunks[slot]
                    : m_method_thunks[smtc2_thunks[
                          slot - INSPECTABLE_VTABLE_SLOTS]];
            if (!write_pointer(
                    m_smtc2_object_vtables[mode] + slot * pointer_size,
                    thunk, is_64bit)) {
                return false;
            }
        }
        for (u32 slot = 0; slot < TIMELINE_FACTORY_VTABLE_SLOTS; ++slot) {
            const GuestAddr thunk = slot < INSPECTABLE_VTABLE_SLOTS
                                        ? m_method_thunks[slot]
                                        : m_method_thunks[16];  // ActivateInstance
            if (!write_pointer(m_timeline_factory_vtables[mode] + slot * pointer_size,
                               thunk, is_64bit)) {
                return false;
            }
        }
        for (u32 slot = 0; slot < TIMELINE_VTABLE_SLOTS; ++slot) {
            const GuestAddr thunk =
                slot < INSPECTABLE_VTABLE_SLOTS
                    ? m_method_thunks[slot]
                    : m_method_thunks[TIMELINE_FIRST_THUNK + (slot - INSPECTABLE_VTABLE_SLOTS)];
            if (!write_pointer(m_timeline_vtables[mode] + slot * pointer_size, thunk,
                               is_64bit)) {
                return false;
            }
        }
        // SystemMediaTransportControls is not activatable; its factory only
        // answers IActivationFactory, whose ActivateInstance is thunk 16.
        for (u32 slot = 0; slot < SMTC_VTABLE_SLOTS; ++slot) {
            const GuestAddr thunk = slot < INSPECTABLE_VTABLE_SLOTS
                                        ? m_method_thunks[slot]
                                        : m_method_thunks[16];
            if (!write_pointer(m_smtc_vtables[mode] + slot * pointer_size,
                               thunk, is_64bit)) {
                return false;
            }
        }
        // The weak-reference vtables are IUnknown-based, so slot 3 is the
        // interface's own method rather than IInspectable's GetIids.
        const std::pair<const std::array<GuestAddr, 2>&, u32> weak[] = {
            {m_weak_source_vtables, 62},
            {m_weak_ref_vtables, 63},
        };
        for (const auto& entry : weak) {
            for (u32 slot = 0; slot < WEAK_VTABLE_SLOTS; ++slot) {
                const GuestAddr thunk = slot < 3 ? m_method_thunks[slot]
                                                 : m_method_thunks[entry.second];
                if (!write_pointer(entry.first[mode] + slot * pointer_size,
                                   thunk, is_64bit)) {
                    return false;
                }
            }
        }
        for (const auto& entry : derived) {
            for (u32 slot = 0; slot < entry.slots; ++slot) {
                const GuestAddr thunk =
                    slot < INSPECTABLE_VTABLE_SLOTS
                        ? m_method_thunks[slot]
                        : m_method_thunks[entry.first_thunk +
                                          (slot - INSPECTABLE_VTABLE_SLOTS)];
                if (!write_pointer(entry.table[mode] + slot * pointer_size,
                                   thunk, is_64bit)) {
                    return false;
                }
            }
        }
        // Preserve existing thunk indices while matching the Windows metadata
        // order of IAppCapabilityStatics exactly.
        constexpr std::array<u32, 4> appcap_thunks = {129, 130, 75, 76};
        for (u32 slot = 0; slot < APPCAP_VTABLE_SLOTS; ++slot) {
            const GuestAddr thunk =
                slot < INSPECTABLE_VTABLE_SLOTS
                    ? m_method_thunks[slot]
                    : m_method_thunks[appcap_thunks[
                          slot - INSPECTABLE_VTABLE_SLOTS]];
            if (!write_pointer(m_appcap_vtables[mode] + slot * pointer_size,
                               thunk, is_64bit)) {
                return false;
            }
        }
        for (u32 slot = 0; slot < LAUNCHER_VTABLE_SLOTS; ++slot) {
            GuestAddr thunk = slot < INSPECTABLE_VTABLE_SLOTS
                                  ? m_method_thunks[slot]
                                  : m_method_thunks[131];
            if (slot == 8) thunk = m_method_thunks[132];
            if (!write_pointer(m_launcher_vtables[mode] + slot * pointer_size,
                               thunk, is_64bit)) {
                return false;
            }
        }
        for (u32 slot = 0; slot < URI_FACTORY_VTABLE_SLOTS; ++slot) {
            GuestAddr thunk = slot < INSPECTABLE_VTABLE_SLOTS
                                  ? m_method_thunks[slot]
                                  : m_method_thunks[131];
            if (slot == 6) thunk = m_method_thunks[133];
            if (!write_pointer(
                    m_uri_factory_vtables[mode] + slot * pointer_size,
                    thunk, is_64bit)) {
                return false;
            }
        }
        for (u32 slot = 0; slot < URI_VTABLE_SLOTS; ++slot) {
            // get_AbsoluteUri is slot 6 and get_RawUri is slot 16.
            const bool text = slot == 6 || slot == 16;
            const GuestAddr thunk =
                slot < INSPECTABLE_VTABLE_SLOTS ? m_method_thunks[slot]
                : text                          ? m_method_thunks[134]
                                                : m_method_thunks[131];
            if (!write_pointer(m_uri_vtables[mode] + slot * pointer_size,
                               thunk, is_64bit)) {
                return false;
            }
        }
        for (u32 slot = 0; slot < JSON_OBJECT_VTABLE_SLOTS; ++slot) {
            const GuestAddr thunk =
                slot < INSPECTABLE_VTABLE_SLOTS
                    ? m_method_thunks[slot]
                    : m_method_thunks[30 + (slot - INSPECTABLE_VTABLE_SLOTS)];
            if (!write_pointer(
                    m_json_object_vtables[mode] + slot * pointer_size,
                    thunk, is_64bit)) {
                return false;
            }
        }
        for (u32 slot = 0; slot < JSON_VALUE_STATICS_VTABLE_SLOTS; ++slot) {
            const GuestAddr thunk =
                slot < INSPECTABLE_VTABLE_SLOTS
                    ? m_method_thunks[slot]
                    : m_method_thunks[17 + (slot - INSPECTABLE_VTABLE_SLOTS)];
            if (!write_pointer(
                    m_json_value_statics_vtables[mode] + slot * pointer_size,
                    thunk, is_64bit)) {
                return false;
            }
        }
        for (u32 slot = 0; slot < JSON_VALUE_VTABLE_SLOTS; ++slot) {
            const GuestAddr thunk =
                slot < INSPECTABLE_VTABLE_SLOTS
                    ? m_method_thunks[slot]
                    : m_method_thunks[23 + (slot - INSPECTABLE_VTABLE_SLOTS)];
            if (!write_pointer(
                    m_json_value_vtables[mode] + slot * pointer_size,
                    thunk, is_64bit)) {
                return false;
            }
        }
    }
    return true;
}

GuestAddr CombaseManager::allocate_storage(usize bytes, usize alignment) {
    if (!ensure_storage() || alignment == 0) return 0;
    const GuestAddr aligned =
        (m_next_storage + alignment - 1) & ~(static_cast<GuestAddr>(alignment) - 1);
    if (aligned > m_region_base + STORAGE_SIZE ||
        bytes > m_region_base + STORAGE_SIZE - aligned) {
        return 0;
    }
    m_next_storage = aligned + bytes;
    return aligned;
}

GuestAddr CombaseManager::allocate_object(
    Object object, bool is_64bit) {
    if (!ensure_storage()) return 0;
    if (m_next_object + 0x20 > m_region_base + 0x10000) return 0;
    const GuestAddr address = m_next_object;
    m_next_object += 0x20;
    const usize mode = is_64bit ? 1 : 0;
    GuestAddr vtable = 0;
    switch (object.type) {
    case ObjectType::GlobalizationPreferences:
        vtable = m_factory_vtables[mode];
        break;
    case ObjectType::StringVectorView:
        vtable = m_vector_vtables[mode];
        break;
    case ObjectType::JsonObjectFactory:
        vtable = m_json_factory_vtables[mode];
        break;
    case ObjectType::JsonObject:
        vtable = m_json_object_vtables[mode];
        break;
    case ObjectType::JsonValueFactory:
        vtable = m_json_value_statics_vtables[mode];
        break;
    case ObjectType::JsonValue:
        vtable = m_json_value_vtables[mode];
        break;
    case ObjectType::BluetoothAdapterFactory:
        vtable = m_bt_statics_vtables[mode];
        break;
    case ObjectType::BluetoothAdapter:
        vtable = m_bt_adapter_vtables[mode];
        break;
    case ObjectType::AsyncOperation:
        vtable = m_async_op_vtables[mode];
        break;
    case ObjectType::AsyncInfo:
        vtable = m_async_info_vtables[mode];
        break;
    case ObjectType::Radio:
        vtable = m_radio_vtables[mode];
        break;
    case ObjectType::WeakReferenceSource:
        vtable = m_weak_source_vtables[mode];
        break;
    case ObjectType::WeakReference:
        vtable = m_weak_ref_vtables[mode];
        break;
    case ObjectType::ToastManagerFactory:
        vtable = m_toast_vtables[mode];
        break;
    case ObjectType::MediaTransportControlsFactory:
        vtable = m_smtc_vtables[mode];
        break;
    case ObjectType::KeyCredentialManagerFactory:
        vtable = m_keycred_vtables[mode];
        break;
    case ObjectType::UIViewSettingsFactory:
        vtable = m_uiview_statics_vtables[mode];
        break;
    case ObjectType::UIViewSettings:
        vtable = m_uiview_vtables[mode];
        break;
    case ObjectType::PenDeviceFactory:
        vtable = m_pen_vtables[mode];
        break;
    case ObjectType::AppCapabilityFactory:
        vtable = m_appcap_vtables[mode];
        break;
    case ObjectType::MediaTransportControlsInterop:
        vtable = m_smtc_interop_vtables[mode];
        break;
    case ObjectType::MediaTransportControls:
        vtable = m_smtc_object_vtables[mode];
        break;
    case ObjectType::MediaTransportControls2:
        vtable = m_smtc2_object_vtables[mode];
        break;
    case ObjectType::DisplayUpdater:
        vtable = m_du_vtables[mode];
        break;
    case ObjectType::MusicDisplayProperties:
        vtable = m_music_vtables[mode];
        break;
    case ObjectType::LauncherFactory:
        vtable = m_launcher_vtables[mode];
        break;
    case ObjectType::UriFactory:
        vtable = m_uri_factory_vtables[mode];
        break;
    case ObjectType::Uri:
        vtable = m_uri_vtables[mode];
        break;
    case ObjectType::TimelinePropertiesFactory:
        vtable = m_timeline_factory_vtables[mode];
        break;
    case ObjectType::TimelineProperties:
        vtable = m_timeline_vtables[mode];
        break;
    }
    if (!write_pointer(address, vtable, is_64bit)) return 0;
    m_objects.emplace(address, std::move(object));
    return address;
}

GuestAddr CombaseManager::create_vector_view(
    const std::vector<std::u16string>& values, bool is_64bit) {
    return allocate_object(
        Object{ObjectType::StringVectorView, 1, values}, is_64bit);
}

} // namespace vitreo::winapi
