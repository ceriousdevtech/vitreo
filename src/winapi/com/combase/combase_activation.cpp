// SPDX-License-Identifier: LGPL-2.1-only
/// @file combase_activation.cpp
/// @brief RoGetActivationFactory and IActivationFactory::ActivateInstance

#include "winapi/com/combase/combase.hpp"
#include "winapi/com/combase/combase_internal.hpp"

#include <cstdlib>
#include <string>

namespace vitreo::winapi {

namespace {

std::string winrt_class_name_for_log(std::u16string_view value) {
    std::string result;
    result.reserve(value.size());
    for (const char16_t code_unit : value) {
        result.push_back(
            code_unit >= 0x20 && code_unit <= 0x7E
                ? static_cast<char>(code_unit)
                : '?');
    }
    return result;
}

} // namespace

void CombaseManager::impl_RoGetActivationFactory(Marshaler& m) {
    const GuestAddr class_id = m.arg_ptr(0);
    const GuestAddr iid = m.arg_ptr(1);
    const GuestAddr factory_out = m.arg_ptr(2);
    if (factory_out != 0) (void)m.write_ptr(factory_out, 0);
    if (class_id == 0 || iid == 0 || factory_out == 0) {
        m.set_return(E_INVALIDARG);
        return;
    }

    const bool is_64bit = m.cpu_is_64bit();
    auto class_name = read_hstring(class_id, is_64bit);
    if (!class_name.has_value()) {
        const auto flags = m_memory.read_u32(class_id);
        const auto length = m_memory.read_u32(class_id + 4);
        u64 buffer = 0;
        if (is_64bit) {
            const auto pointer = m_memory.read_u64(class_id + 16);
            if (pointer.is_ok()) buffer = pointer.value();
        } else {
            const auto pointer = m_memory.read_u32(class_id + 16);
            if (pointer.is_ok()) buffer = pointer.value();
        }
        VITREO_TRACE(
            "[combase] RoGetActivationFactory invalid HSTRING=0x%llx "
            "mode=%s flags=0x%x length=%u buffer=0x%llx\n",
            static_cast<unsigned long long>(class_id),
            is_64bit ? "x64" : "x86",
            flags.is_ok() ? flags.value() : 0,
            length.is_ok() ? length.value() : 0,
            static_cast<unsigned long long>(buffer));
        m.set_return(E_INVALIDARG);
        return;
    }
    const bool globalization =
        *class_name == GLOBALIZATION_PREFERENCES_CLASS;
    const bool json_object = *class_name == JSON_OBJECT_CLASS;
    const bool json_value = *class_name == JSON_VALUE_CLASS;
    const bool bluetooth = *class_name == BLUETOOTH_ADAPTER_CLASS;
    const bool toast = *class_name == TOAST_MANAGER_CLASS;
    const bool smtc = *class_name == SMTC_CLASS;
    const bool keycred = *class_name == KEY_CREDENTIAL_MANAGER_CLASS;
    const bool uiview = *class_name == UI_VIEW_SETTINGS_CLASS;
    const bool pen = *class_name == PEN_DEVICE_CLASS;
    const bool appcap = *class_name == APP_CAPABILITY_CLASS;
    const bool launcher = *class_name == LAUNCHER_CLASS;
    const bool uri = *class_name == URI_CLASS;
    const bool timeline = *class_name == SMTC_TIMELINE_CLASS;
    bool known_class = globalization || json_object || json_value ||
                       bluetooth || toast || smtc || keycred || uiview ||
                       pen || appcap || launcher || uri || timeline;
    // Bisection aid: VITREO_DISABLE_WINRT_CLASS=<substring> makes any matching
    // runtime class activate as REGDB_E_CLASSNOTREG. That is a real Windows
    // outcome (the class genuinely is not registered on some SKUs), so a guest
    // that handles it correctly keeps running -- which is exactly the question
    // you are asking when a WinRT bring-up path is a suspect.
    if (known_class) {
        static const std::string disabled_substring = []() {
            const char* disabled = std::getenv("VITREO_DISABLE_WINRT_CLASS");
            return disabled != nullptr ? std::string(disabled) : std::string();
        }();
        if (!disabled_substring.empty()) {
            const std::string name = winrt_class_name_for_log(*class_name);
            if (name.find(disabled_substring) != std::string::npos) {
                known_class = false;
            }
        }
    }
    if (!known_class) {
        const std::string printable_class =
            winrt_class_name_for_log(*class_name);
        CombaseGuid wanted{};
        const bool have_iid =
            iid != 0 &&
            m_memory.read_bytes(iid, reinterpret_cast<Byte*>(&wanted),
                                sizeof(wanted)).is_ok();
        VITREO_TRACE(
            "[combase] RoGetActivationFactory unsupported class "
            "'%s' length=%zu iid={%08x-%04x-%04x-%02x%02x%02x%02x%02x%02x%02x%02x} "
            "-> REGDB_E_CLASSNOTREG\n",
            printable_class.c_str(), class_name->size(),
            have_iid ? wanted.data1 : 0u,
            have_iid ? wanted.data2 : 0u,
            have_iid ? wanted.data3 : 0u,
            have_iid ? wanted.data4[0] : 0u, have_iid ? wanted.data4[1] : 0u,
            have_iid ? wanted.data4[2] : 0u, have_iid ? wanted.data4[3] : 0u,
            have_iid ? wanted.data4[4] : 0u, have_iid ? wanted.data4[5] : 0u,
            have_iid ? wanted.data4[6] : 0u, have_iid ? wanted.data4[7] : 0u);
        m.set_return(REGDB_E_CLASSNOTREG);
        return;
    }
    const ObjectType factory_type =
        globalization ? ObjectType::GlobalizationPreferences
        : json_value  ? ObjectType::JsonValueFactory
        : bluetooth   ? ObjectType::BluetoothAdapterFactory
        : toast       ? ObjectType::ToastManagerFactory
        : smtc        ? ObjectType::MediaTransportControlsFactory
        : keycred     ? ObjectType::KeyCredentialManagerFactory
        : uiview      ? ObjectType::UIViewSettingsFactory
        : pen         ? ObjectType::PenDeviceFactory
        : appcap      ? ObjectType::AppCapabilityFactory
        : launcher    ? ObjectType::LauncherFactory
        : uri         ? ObjectType::UriFactory
        : timeline    ? ObjectType::TimelinePropertiesFactory
                      : ObjectType::JsonObjectFactory;
    // The SystemMediaTransportControls factory implements
    // ISystemMediaTransportControlsInterop (smtc_timeline_probe "interop
    // hr=00000000": asked for directly, as C++/WinRT's
    // get_activation_factory<..., ISystemMediaTransportControlsInterop> does).
    // It is a separate view so GetForWindow does not land on
    // IActivationFactory's slot 6.
    if (smtc && iid != 0) {
        CombaseGuid wanted{};
        if (m_memory.read_bytes(iid, reinterpret_cast<Byte*>(&wanted), sizeof(wanted)).is_ok() &&
            guid_equal(wanted, IID_ISMTC_INTEROP)) {
            Object view{};
            view.type = ObjectType::MediaTransportControlsInterop;
            view.ref_count = 1;
            const GuestAddr interop = allocate_object(std::move(view), is_64bit);
            if (interop == 0) {
                m.set_return(E_OUTOFMEMORY);
                return;
            }
            m.set_return(m.write_ptr(factory_out, interop).is_ok() ? S_OK : E_POINTER);
            return;
        }
    }
    if (!object_supports_iid(factory_type, iid)) {
        VITREO_TRACE(
            "[combase] RoGetActivationFactory "
            "class='%s' unsupported IID -> E_NOINTERFACE\n",
            winrt_class_name_for_log(*class_name).c_str());
        m.set_return(E_NOINTERFACE);
        return;
    }

    const usize mode = is_64bit ? 1 : 0;
    GuestAddr& factory = globalization ? m_factories[mode]
                         : json_value  ? m_json_value_factories[mode]
                         : bluetooth   ? m_bt_factories[mode]
                         : toast       ? m_toast_factories[mode]
                         : smtc        ? m_smtc_factories[mode]
                         : keycred     ? m_keycred_factories[mode]
                         : uiview      ? m_uiview_factories[mode]
        : pen         ? m_pen_factories[mode]
        : appcap      ? m_appcap_factories[mode]
        : launcher    ? m_launcher_factories[mode]
        : uri         ? m_uri_factories[mode]
        : timeline    ? m_timeline_factories[mode]
                      : m_json_factories[mode];
    if (factory == 0) {
        factory = allocate_object(
            Object{factory_type, 1, {}},
            is_64bit);
    } else if (auto cached = m_objects.find(factory);
               cached != m_objects.end()) {
        ++cached->second.ref_count;
    } else {
        // The cache slot outlived the object it named; re-activate rather
        // than dereference a freed address.
        factory = allocate_object(Object{factory_type, 1, {}}, is_64bit);
    }
    if (factory == 0) {
        m.set_return(E_OUTOFMEMORY);
        return;
    }
    if (m.write_ptr(factory_out, factory).is_err()) {
        m.set_return(E_POINTER);
        return;
    }
    VITREO_TRACE(
        "[combase] RoGetActivationFactory "
        "class='%s' "
        "factory=0x%llx -> S_OK\n",
        winrt_class_name_for_log(*class_name).c_str(),
        static_cast<unsigned long long>(factory));
    m.set_return(S_OK);
}

void CombaseManager::impl_ActivateInstance(Marshaler& m) {
    auto factory = m_objects.find(m.arg_ptr(0));
    const GuestAddr instance_out = m.arg_ptr(1);
    if (instance_out == 0) {
        m.set_return(E_INVALIDARG);
        return;
    }
    (void)m.write_ptr(instance_out, 0);
    if (factory == m_objects.end() ||
        (factory->second.type != ObjectType::JsonObjectFactory &&
         factory->second.type != ObjectType::TimelinePropertiesFactory)) {
        m.set_return(E_INVALIDARG);
        return;
    }
    // smtc_timeline_probe: a new TimelineProperties has every TimeSpan at 0,
    // and each activation is a distinct object.
    const ObjectType instance_type =
        factory->second.type == ObjectType::TimelinePropertiesFactory
            ? ObjectType::TimelineProperties
            : ObjectType::JsonObject;
    const GuestAddr instance = allocate_object(
        Object{instance_type, 1, {}}, m.cpu_is_64bit());
    if (instance == 0) {
        m.set_return(E_OUTOFMEMORY);
        return;
    }
    if (m.write_ptr(instance_out, instance).is_err()) {
        m.set_return(E_INVALIDARG);
        return;
    }
    m.set_return(S_OK);
}

} // namespace vitreo::winapi
