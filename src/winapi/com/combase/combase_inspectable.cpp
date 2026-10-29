// SPDX-License-Identifier: LGPL-2.1-only
/// @file combase_inspectable.cpp
/// @brief IUnknown/IInspectable/IWeakReference identity and lifetime for combase objects

#include "winapi/com/combase/combase.hpp"
#include "winapi/com/combase/combase_internal.hpp"

#include <cstdio>
#include <vector>

namespace vitreo::winapi {

bool CombaseManager::object_supports_iid(
    ObjectType type, GuestAddr iid_address) const {
    CombaseGuid iid{};
    if (iid_address == 0 ||
        m_memory.read_bytes(
            iid_address, reinterpret_cast<Byte*>(&iid), sizeof(iid)).is_err()) {
        return false;
    }
    if (guid_equal(iid, IID_IUNKNOWN) || guid_equal(iid, IID_IINSPECTABLE) ||
        guid_equal(iid, IID_IAGILEOBJECT)) {
        return true;
    }
    if (type == ObjectType::GlobalizationPreferences) {
        return guid_equal(iid, IID_IGLOBALIZATION_PREFERENCES_STATICS);
    }
    if (type == ObjectType::StringVectorView) {
        return guid_equal(iid, IID_IVECTOR_VIEW_HSTRING);
    }
    if (type == ObjectType::JsonObjectFactory) {
        return guid_equal(iid, IID_IACTIVATION_FACTORY);
    }
    // Every activation factory answers IActivationFactory on Windows, whatever
    // else it also implements. Declining it made a factory look half-built.
    switch (type) {
    case ObjectType::JsonObjectFactory:
    case ObjectType::JsonValueFactory:
    case ObjectType::BluetoothAdapterFactory:
    case ObjectType::ToastManagerFactory:
    case ObjectType::MediaTransportControlsFactory:
    case ObjectType::KeyCredentialManagerFactory:
    case ObjectType::UIViewSettingsFactory:
    case ObjectType::PenDeviceFactory:
    case ObjectType::AppCapabilityFactory:
    case ObjectType::LauncherFactory:
    case ObjectType::UriFactory:
    case ObjectType::TimelinePropertiesFactory:
        if (guid_equal(iid, IID_IACTIVATION_FACTORY)) return true;
        break;
    default:
        break;
    }
    if (type == ObjectType::ToastManagerFactory) {
        return guid_equal(iid, IID_ITOAST_NOTIFICATION_MANAGER_STATICS);
    }
    if (type == ObjectType::MediaTransportControlsInterop) {
        return guid_equal(iid, IID_ISMTC_INTEROP);
    }
    if (type == ObjectType::TimelineProperties) {
        return guid_equal(iid, IID_ISMTC_TIMELINE_PROPERTIES);
    }
    if (type == ObjectType::MediaTransportControls) {
        return guid_equal(iid, IID_ISYSTEM_MEDIA_TRANSPORT_CONTROLS);
    }
    if (type == ObjectType::MediaTransportControls2) {
        return guid_equal(iid, IID_ISYSTEM_MEDIA_TRANSPORT_CONTROLS2);
    }
    if (type == ObjectType::DisplayUpdater) {
        return guid_equal(iid, IID_IDISPLAY_UPDATER);
    }
    if (type == ObjectType::MusicDisplayProperties) {
        return guid_equal(iid, IID_IMUSIC_DISPLAY_PROPERTIES);
    }
    if (type == ObjectType::MediaTransportControlsFactory) {
        // The interop view is handed out from QueryInterface, since its single
        // method would otherwise collide with IActivationFactory's slot 6.
        return false;
    }
    if (type == ObjectType::KeyCredentialManagerFactory) {
        return guid_equal(iid, IID_IKEY_CREDENTIAL_MANAGER_STATICS);
    }
    if (type == ObjectType::UIViewSettingsFactory) {
        return guid_equal(iid, IID_IUI_VIEW_SETTINGS_INTEROP);
    }
    if (type == ObjectType::UIViewSettings) {
        return guid_equal(iid, IID_IUI_VIEW_SETTINGS);
    }
    if (type == ObjectType::PenDeviceFactory) {
        return guid_equal(iid, IID_IPEN_DEVICE_STATICS);
    }
    if (type == ObjectType::AppCapabilityFactory) {
        return guid_equal(iid, IID_IAPP_CAPABILITY_STATICS);
    }
    if (type == ObjectType::LauncherFactory) {
        return guid_equal(iid, IID_ILAUNCHER_STATICS);
    }
    if (type == ObjectType::UriFactory) {
        return guid_equal(iid, IID_IURI_RUNTIME_CLASS_FACTORY);
    }
    if (type == ObjectType::Uri) {
        return guid_equal(iid, IID_IURI_RUNTIME_CLASS);
    }
    if (type == ObjectType::JsonValueFactory) {
        return guid_equal(iid, IID_IJSON_VALUE_STATICS);
    }
    if (type == ObjectType::JsonValue) {
        return guid_equal(iid, IID_IJSON_VALUE);
    }
    if (type == ObjectType::JsonObject) {
        return guid_equal(iid, IID_IJSON_OBJECT) ||
               guid_equal(iid, IID_IJSON_VALUE);
    }
    if (type == ObjectType::BluetoothAdapterFactory) {
        return guid_equal(iid, IID_IBLUETOOTH_ADAPTER_STATICS);
    }
    if (type == ObjectType::BluetoothAdapter) {
        return guid_equal(iid, IID_IBLUETOOTH_ADAPTER);
    }
    if (type == ObjectType::WeakReferenceSource) {
        return guid_equal(iid, IID_IWEAK_REFERENCE_SOURCE);
    }
    if (type == ObjectType::WeakReference) {
        return guid_equal(iid, IID_IWEAK_REFERENCE);
    }
    if (type == ObjectType::Radio) {
        return guid_equal(iid, IID_IRADIO);
    }
    if (type == ObjectType::AsyncOperation ||
        type == ObjectType::AsyncInfo) {
        // The parameterised IAsyncOperation<T> IID is per-instantiation, so
        // the guest reaches the operation through its declared return type;
        // IAsyncInfo is the one view it QIs for by name.
        return guid_equal(iid, IID_IASYNC_INFO);
    }
    return false;
}

void CombaseManager::impl_QueryInterface(Marshaler& m) {
    const GuestAddr self = m.arg_ptr(0);
    const GuestAddr iid = m.arg_ptr(1);
    const GuestAddr object_out = m.arg_ptr(2);
    if (object_out == 0) {
        m.set_return(E_POINTER);
        return;
    }
    (void)m.write_ptr(object_out, 0);
    auto object = m_objects.find(self);
    if (object != m_objects.end()) {
        CombaseGuid requested{};
        const bool have_iid =
            iid != 0 && m_memory.read_bytes(
                iid, reinterpret_cast<Byte*>(&requested),
                sizeof(requested)).is_ok();
        if (have_iid && guid_equal(requested, IID_ISMTC_INTEROP) &&
            object->second.type == ObjectType::MediaTransportControlsFactory) {
            // Microsoft documents ISystemMediaTransportControlsInterop as
            // implemented by this activation factory; it gets its own view so
            // GetForWindow does not land on IActivationFactory's slot 6.
            Object view{};
            view.type = ObjectType::MediaTransportControlsInterop;
            view.ref_count = 1;
            const GuestAddr interop =
                allocate_object(std::move(view), m.cpu_is_64bit());
            if (interop == 0) {
                m.set_return(E_OUTOFMEMORY);
                return;
            }
            m.set_return(m.write_ptr(object_out, interop).is_ok() ? S_OK
                                                                  : E_POINTER);
            return;
        }
        // smtc_timeline_probe: the controls GetForWindow hands out answer
        // ISystemMediaTransportControls2 with a different interface pointer
        // of the same object (IUnknown identity equal), and that view leads
        // back to the original ISystemMediaTransportControls pointer.
        const bool want_v2 =
            have_iid && guid_equal(requested, IID_ISYSTEM_MEDIA_TRANSPORT_CONTROLS2) &&
            object->second.type == ObjectType::MediaTransportControls;
        const bool want_v1 =
            have_iid && guid_equal(requested, IID_ISYSTEM_MEDIA_TRANSPORT_CONTROLS) &&
            object->second.type == ObjectType::MediaTransportControls2;
        // COM identity: IUnknown is the same pointer from either view (probe
        // "IUnknown identity equal=1"), the ISystemMediaTransportControls one.
        if (have_iid && guid_equal(requested, IID_IUNKNOWN) &&
            object->second.type == ObjectType::MediaTransportControls2) {
            const auto state = smtc_state(self);
            if (state != m_objects.end() && state->first != self) {
                ++state->second.ref_count;
                m.set_return(m.write_ptr(object_out, state->first).is_ok() ? S_OK : E_POINTER);
                return;
            }
        }
        if (want_v1 || want_v2) {
            const auto state = smtc_state(self);
            const GuestAddr primary = state == m_objects.end() ? 0 : state->first;
            const GuestAddr view =
                primary == 0 ? 0 : smtc_view(primary, want_v2, m.cpu_is_64bit());
            if (view == 0) {
                m.set_return(E_NOINTERFACE);
                return;
            }
            ++m_objects.at(view).ref_count;
            m.set_return(m.write_ptr(object_out, view).is_ok() ? S_OK : E_POINTER);
            return;
        }
        if (have_iid && guid_equal(requested, IID_IWEAK_REFERENCE_SOURCE) &&
            object->second.type != ObjectType::WeakReferenceSource &&
            object->second.type != ObjectType::WeakReference) {
            // IWeakReferenceSource is IUnknown-derived, so it cannot share the
            // object's IInspectable vtable; hand out a view bound to it.
            Object view{};
            view.type = ObjectType::WeakReferenceSource;
            view.ref_count = 1;
            view.weak_target = self;
            const GuestAddr source =
                allocate_object(std::move(view), m.cpu_is_64bit());
            if (source == 0) {
                m.set_return(E_OUTOFMEMORY);
                return;
            }
            VITREO_TRACE(
                "[combase] QueryInterface IWeakReferenceSource "
                "target=0x%llx source=0x%llx out=0x%llx\n",
                static_cast<unsigned long long>(self),
                static_cast<unsigned long long>(source),
                static_cast<unsigned long long>(object_out));
            m.set_return(m.write_ptr(object_out, source).is_ok() ? S_OK
                                                                 : E_POINTER);
            return;
        }
    }
    if (object == m_objects.end() ||
        !object_supports_iid(object->second.type, iid)) {
        CombaseGuid wanted{};
        const bool have = iid != 0 &&
            m_memory.read_bytes(iid, reinterpret_cast<Byte*>(&wanted),
                                sizeof(wanted)).is_ok();
        std::fprintf(stderr,
            "[combase] QueryInterface this=0x%llx type=%d iid="
            "{%08x-%04x-%04x-%02x%02x%02x%02x%02x%02x%02x%02x} -> "
            "E_NOINTERFACE\n",
            static_cast<unsigned long long>(self),
            object == m_objects.end()
                ? -1
                : static_cast<int>(object->second.type),
            have ? wanted.data1 : 0u, have ? wanted.data2 : 0u,
            have ? wanted.data3 : 0u,
            have ? wanted.data4[0] : 0u, have ? wanted.data4[1] : 0u,
            have ? wanted.data4[2] : 0u, have ? wanted.data4[3] : 0u,
            have ? wanted.data4[4] : 0u, have ? wanted.data4[5] : 0u,
            have ? wanted.data4[6] : 0u, have ? wanted.data4[7] : 0u);
        m.set_return(E_NOINTERFACE);
        return;
    }
    ++object->second.ref_count;
    {
        CombaseGuid got{};
        const bool have_got = iid != 0 &&
            m_memory.read_bytes(iid, reinterpret_cast<Byte*>(&got),
                                sizeof(got)).is_ok();
        VITREO_TRACE(
            "[combase] QueryInterface this=0x%llx type=%d iid="
            "{%08x-%04x-%04x-%02x%02x%02x%02x%02x%02x%02x%02x} -> S_OK "
            "out=0x%llx\n",
            static_cast<unsigned long long>(self),
            static_cast<int>(object->second.type),
            have_got ? got.data1 : 0u, have_got ? got.data2 : 0u,
            have_got ? got.data3 : 0u,
            have_got ? got.data4[0] : 0u, have_got ? got.data4[1] : 0u,
            have_got ? got.data4[2] : 0u, have_got ? got.data4[3] : 0u,
            have_got ? got.data4[4] : 0u, have_got ? got.data4[5] : 0u,
            have_got ? got.data4[6] : 0u, have_got ? got.data4[7] : 0u,
            static_cast<unsigned long long>(object_out));
    }
    if (m.write_ptr(object_out, self).is_err()) {
        m.set_return(E_POINTER);
        return;
    }
    m.set_return(S_OK);
}

void CombaseManager::impl_AddRef(Marshaler& m) {
    auto object = m_objects.find(m.arg_ptr(0));
    if (object == m_objects.end()) {
        m.set_return(0);
        return;
    }
    m.set_return(++object->second.ref_count);
}

std::array<std::array<GuestAddr, 2>*, 13> CombaseManager::factory_caches() {
    return {&m_factories,        &m_json_factories,   &m_json_value_factories,
            &m_bt_factories,     &m_toast_factories,  &m_smtc_factories,
            &m_keycred_factories, &m_uiview_factories, &m_pen_factories,
            &m_appcap_factories, &m_launcher_factories, &m_uri_factories,
            &m_timeline_factories};
}

void CombaseManager::impl_Release(Marshaler& m) {
    auto object = m_objects.find(m.arg_ptr(0));
    if (object == m_objects.end()) {
        m.set_return(0);
        return;
    }
    const GuestAddr address = object->first;
    const u32 ref_count = --object->second.ref_count;
    if (ref_count == 0) {
        std::vector<GuestAddr> retained_handlers;
        retained_handlers.reserve(object->second.smtc_event_handlers.size());
        for (const auto& [token, handler] :
             object->second.smtc_event_handlers) {
            (void)token;
            retained_handlers.push_back(handler);
        }
        for (std::array<GuestAddr, 2>* cache : factory_caches()) {
            for (GuestAddr& factory : *cache) {
                if (address == factory) factory = 0;
            }
        }
        m_objects.erase(object);
        for (GuestAddr handler : retained_handlers) {
            (void)guest_iunknown_ref(m, handler, 2);
        }
    }
    m.set_return(ref_count);
}

void CombaseManager::impl_GetIids(Marshaler& m) {
    const GuestAddr self = m.arg_ptr(0);
    const GuestAddr count_out = m.arg_ptr(1);
    const GuestAddr iids_out = m.arg_ptr(2);
    auto object = m_objects.find(self);
    if (object == m_objects.end() || count_out == 0 || iids_out == 0) {
        m.set_return(E_INVALIDARG);
        return;
    }
    const CombaseGuid* iid = nullptr;
    switch (object->second.type) {
    case ObjectType::GlobalizationPreferences:
        iid = &IID_IGLOBALIZATION_PREFERENCES_STATICS;
        break;
    case ObjectType::StringVectorView:
        iid = &IID_IVECTOR_VIEW_HSTRING;
        break;
    case ObjectType::JsonObjectFactory:
        iid = &IID_IACTIVATION_FACTORY;
        break;
    case ObjectType::JsonObject:
        break;
    case ObjectType::LauncherFactory:
        iid = &IID_ILAUNCHER_STATICS;
        break;
    case ObjectType::UriFactory:
        iid = &IID_IURI_RUNTIME_CLASS_FACTORY;
        break;
    case ObjectType::Uri:
        iid = &IID_IURI_RUNTIME_CLASS;
        break;
    case ObjectType::TimelineProperties: {
        // smtc_timeline_probe "GetIids count=2": the interface, then
        // IWeakReferenceSource.
        const CombaseGuid iids[2] = {IID_ISMTC_TIMELINE_PROPERTIES, IID_IWEAK_REFERENCE_SOURCE};
        const GuestAddr storage = allocate_storage(sizeof(iids), alignof(u32));
        if (storage == 0 ||
            m_memory.write_bytes(storage, reinterpret_cast<const Byte*>(iids), sizeof(iids)).is_err() ||
            m.write_u32(count_out, 2).is_err() || m.write_ptr(iids_out, storage).is_err()) {
            m.set_return(E_OUTOFMEMORY);
            return;
        }
        m.set_return(S_OK);
        return;
    }
    }
    if (iid == nullptr) {
        if (m.write_u32(count_out, 0).is_err() ||
            m.write_ptr(iids_out, 0).is_err()) {
            m.set_return(E_INVALIDARG);
            return;
        }
        m.set_return(S_OK);
        return;
    }
    const GuestAddr iid_storage = allocate_storage(sizeof(CombaseGuid), alignof(u32));
    if (iid_storage == 0 ||
        m_memory.write_bytes(
            iid_storage, reinterpret_cast<const Byte*>(iid),
            sizeof(*iid)).is_err() ||
        m.write_u32(count_out, 1).is_err() ||
        m.write_ptr(iids_out, iid_storage).is_err()) {
        m.set_return(E_OUTOFMEMORY);
        return;
    }
    m.set_return(S_OK);
}

void CombaseManager::impl_GetRuntimeClassName(Marshaler& m) {
    auto object = m_objects.find(m.arg_ptr(0));
    const GuestAddr name_out = m.arg_ptr(1);
    if (object == m_objects.end() || name_out == 0) {
        m.set_return(E_INVALIDARG);
        return;
    }
    std::u16string_view class_name;
    switch (object->second.type) {
    case ObjectType::GlobalizationPreferences:
        class_name = GLOBALIZATION_PREFERENCES_CLASS;
        break;
    case ObjectType::StringVectorView:
        class_name = STRING_VECTOR_RUNTIME_CLASS;
        break;
    case ObjectType::JsonObjectFactory:
    case ObjectType::JsonObject:
        class_name = JSON_OBJECT_CLASS;
        break;
    case ObjectType::LauncherFactory:
        class_name = LAUNCHER_CLASS;
        break;
    case ObjectType::UriFactory:
    case ObjectType::Uri:
        class_name = URI_CLASS;
        break;
    case ObjectType::TimelinePropertiesFactory:
    case ObjectType::TimelineProperties:
        class_name = SMTC_TIMELINE_CLASS;
        break;
    }
    const GuestAddr name =
        create_hstring(class_name, m.cpu_is_64bit());
    if (name == 0 || m.write_ptr(name_out, name).is_err()) {
        m.set_return(E_OUTOFMEMORY);
        return;
    }
    m.set_return(S_OK);
}

void CombaseManager::impl_GetTrustLevel(Marshaler& m) {
    auto object = m_objects.find(m.arg_ptr(0));
    const GuestAddr trust_out = m.arg_ptr(1);
    if (object == m_objects.end() || trust_out == 0 ||
        m.write_u32(trust_out, 0).is_err()) {
        m.set_return(E_INVALIDARG);
        return;
    }
    m.set_return(S_OK);
}

u64 CombaseManager::guest_iunknown_ref(
    Marshaler& m, GuestAddr object, usize slot) {
    if (object == 0 || (slot != 1 && slot != 2) || !m_guest_call) return 0;
    const bool is_64bit = m.cpu_is_64bit();
    const GuestAddr pointer_size = is_64bit ? 8u : 4u;
    const auto vtable = is_64bit
        ? m_memory.read_u64(object)
        : m_memory.read_u32(object).map(
              [](u32 value) { return static_cast<u64>(value); });
    if (vtable.is_err() || vtable.value() == 0) return 0;
    const GuestAddr method_address =
        static_cast<GuestAddr>(vtable.value()) + slot * pointer_size;
    const auto method = is_64bit
        ? m_memory.read_u64(method_address)
        : m_memory.read_u32(method_address).map(
              [](u32 value) { return static_cast<u64>(value); });
    if (method.is_err() || method.value() == 0) return 0;
    const u64 args[1] = {object};
    return m_guest_call(
        static_cast<GuestAddr>(method.value()), args, 1, is_64bit);
}

// IWeakReferenceSource::GetWeakReference / IWeakReference::Resolve.
// Combase objects live for the lifetime of the manager, so a weak reference
// always resolves; Resolve's contract on a dead target is S_OK with null,
// which is why failure here is never an error code.
void CombaseManager::impl_GetWeakReference(Marshaler& m) {
    const GuestAddr out = m.arg_ptr(1);
    VITREO_TRACE(
        "[combase] IWeakReferenceSource.GetWeakReference self=0x%llx "
        "out=0x%llx\n",
        static_cast<unsigned long long>(m.arg_ptr(0)),
        static_cast<unsigned long long>(out));
    if (out == 0) {
        m.set_return(E_POINTER);
        return;
    }
    (void)m.write_ptr(out, 0);
    auto it = m_objects.find(m.arg_ptr(0));
    if (it == m_objects.end() ||
        it->second.type != ObjectType::WeakReferenceSource) {
        m.set_return(E_INVALIDARG);
        return;
    }
    Object weak{};
    weak.type = ObjectType::WeakReference;
    weak.ref_count = 1;
    weak.weak_target = it->second.weak_target;
    const GuestAddr reference =
        allocate_object(std::move(weak), m.cpu_is_64bit());
    if (reference == 0) {
        m.set_return(E_OUTOFMEMORY);
        return;
    }
    VITREO_TRACE(
        "[combase] IWeakReferenceSource.GetWeakReference target=0x%llx "
        "reference=0x%llx\n",
        static_cast<unsigned long long>(it->second.weak_target),
        static_cast<unsigned long long>(reference));
    m.set_return(m.write_ptr(out, reference).is_ok() ? S_OK : E_POINTER);
}

void CombaseManager::impl_WeakReferenceResolve(Marshaler& m) {
    const GuestAddr iid = m.arg_ptr(1);
    const GuestAddr out = m.arg_ptr(2);
    if (out == 0) {
        m.set_return(E_POINTER);
        return;
    }
    (void)m.write_ptr(out, 0);
    auto it = m_objects.find(m.arg_ptr(0));
    if (it == m_objects.end() ||
        it->second.type != ObjectType::WeakReference) {
        m.set_return(E_INVALIDARG);
        return;
    }
    auto target = m_objects.find(it->second.weak_target);
    if (target == m_objects.end()) {
        // Target gone: S_OK with a null out, per IWeakReference::Resolve.
        m.set_return(S_OK);
        return;
    }
    if (!object_supports_iid(target->second.type, iid)) {
        m.set_return(E_NOINTERFACE);
        return;
    }
    ++target->second.ref_count;
    m.set_return(m.write_ptr(out, it->second.weak_target).is_ok() ? S_OK
                                                                  : E_POINTER);
}

} // namespace vitreo::winapi
