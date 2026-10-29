// SPDX-License-Identifier: LGPL-2.1-only
/// @file wasapi_endpoints.cpp
/// @brief MMDevice endpoints: IMMDeviceEnumerator / IMMDeviceCollection / IMMDevice /
/// IMMEndpoint, the endpoint IPropertyStore, and ActivateAudioInterfaceAsync with its
/// completion-handler worker. Split out of com_core.cpp.

#include "com_core.hpp"
#include "com_wasapi_internal.hpp"
#include "ole_guid_internal.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/com/ole32/task_memory.hpp"
#include "winapi/kernel32/synchronization/sync.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace vitreo::winapi {

namespace {

constexpr u32 E_NOINTERFACE = 0x80004002;
constexpr u32 E_OUTOFMEMORY = 0x8007000E;
constexpr u32 E_NOTFOUND = 0x80070490u;
#include "audio_async_contract.inc"

}  // namespace

u32 ComCoreManager::mmdevice_enumerator_query_interface(Marshaler& m) {
    const GuestAddr object = m.arg_ptr(0);
    const GuestAddr iid_ptr = m.arg_ptr(1);
    const GuestAddr out_ptr = m.arg_ptr(2);
    if (iid_ptr == 0 || out_ptr == 0) return com::E_POINTER;
    (void)m.write_ptr(out_ptr, 0);
    const auto iid = m.read_struct<Guid>(iid_ptr);
    if (iid.is_err()) return com::E_INVALIDARG;
    std::lock_guard lock(m_com_mutex);
    auto found = m_mmdevice_enumerators.find(object);
    if (found == m_mmdevice_enumerators.end() ||
        (!guid_equal(iid.value(), IID_IUNKNOWN) &&
         !guid_equal(iid.value(), IID_IMMDEVICEENUMERATOR))) {
        return E_NOINTERFACE;
    }
    ++found->second.refs;
    if (m.write_ptr(out_ptr, object).is_err()) {
        --found->second.refs;
        return com::E_INVALIDARG;
    }
    return com::S_OK;
}

u32 ComCoreManager::mmdevice_enumerator_add_ref(Marshaler& m) {
    std::lock_guard lock(m_com_mutex);
    auto found = m_mmdevice_enumerators.find(m.arg_ptr(0));
    if (found == m_mmdevice_enumerators.end()) return 0;
    return ++found->second.refs;
}

u32 ComCoreManager::mmdevice_enumerator_release(Marshaler& m) {
    std::lock_guard lock(m_com_mutex);
    auto found = m_mmdevice_enumerators.find(m.arg_ptr(0));
    if (found == m_mmdevice_enumerators.end()) return 0;
    const u32 refs = --found->second.refs;
    if (refs == 0) m_mmdevice_enumerators.erase(found);
    return refs;
}

std::vector<ComCoreManager::AudioEndpointRef> ComCoreManager::audio_endpoints(
    u32 data_flow) const {
    std::vector<AudioEndpointRef> out;
    if ((data_flow == 0u || data_flow == 2u) && m_audio_endpoint_available) {
        out.push_back(AudioEndpointRef{false, {}});
    }
    if ((data_flow == 1u || data_flow == 2u) && m_audio_capture != nullptr) {
        for (const auto& device : m_audio_capture->devices()) {
            out.push_back(AudioEndpointRef{true, device.uid});
        }
    }
    return out;
}

std::optional<ComCoreManager::AudioEndpointRef> ComCoreManager::capture_endpoint_by_id(
    std::string_view id) const {
    if (m_audio_capture == nullptr) return std::nullopt;
    auto lower = [](std::string text) {
        std::transform(text.begin(), text.end(), text.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return text;
    };
    const std::string wanted = lower(std::string(id));
    for (const auto& device : m_audio_capture->devices()) {
        if (lower(audio::capture_endpoint_id(device.uid)) == wanted) {
            return AudioEndpointRef{true, device.uid};
        }
    }
    return std::nullopt;
}

u32 ComCoreManager::mmdevice_enumerator_enum_audio_endpoints(Marshaler& m) {
    const GuestAddr object = m.arg_ptr(0);
    const u32 data_flow = static_cast<u32>(m.arg(1));
    const u32 state_mask = static_cast<u32>(m.arg(2));
    const GuestAddr out_ptr = m.arg_ptr(3);
    if (out_ptr == 0) return com::E_POINTER;
    (void)m.write_ptr(out_ptr, 0);
    if (data_flow > 2u || state_mask == 0 || (state_mask & ~0xFu) != 0) {
        return com::E_INVALIDARG;
    }
    {
        std::lock_guard lock(m_com_mutex);
        if (!m_mmdevice_enumerators.contains(object)) return E_NOINTERFACE;
    }

    const char* names[] = {
        "Vitreo_MMDeviceCollection_QueryInterface",
        "Vitreo_MMDeviceCollection_AddRef",
        "Vitreo_MMDeviceCollection_Release",
        "Vitreo_MMDeviceCollection_GetCount",
        "Vitreo_MMDeviceCollection_Item",
    };
    const GuestAddr pointer_size = m.cpu_is_64bit() ? 8u : 4u;
    const GuestAddr collection = reserve_com_scratch(
        pointer_size + static_cast<GuestAddr>(std::size(names)) * pointer_size);
    if (collection == 0) return E_OUTOFMEMORY;
    const GuestAddr vtable = collection + pointer_size;
    if (m.write_ptr(collection, vtable).is_err()) return com::E_INVALIDARG;
    for (usize slot = 0; slot < std::size(names); ++slot) {
        auto method = m_thunks->get_thunk_address("ole32.dll", names[slot]);
        if (method.is_err() ||
            m.write_ptr(vtable + slot * pointer_size, method.value()).is_err()) {
            return com::E_NOTIMPL;
        }
    }
    // Every endpoint Vitreo exposes is DEVICE_STATE_ACTIVE: the render
    // endpoint, then each host input device in host order.
    std::vector<AudioEndpointRef> endpoints;
    if ((state_mask & 0x1u) != 0) endpoints = audio_endpoints(data_flow);
    const u32 count = static_cast<u32>(endpoints.size());
    {
        std::lock_guard lock(m_com_mutex);
        m_mmdevice_collections.emplace(
            collection, MmDeviceCollectionState{.refs = 1u, .count = count,
                                                .endpoints = std::move(endpoints)});
    }
    if (m.write_ptr(out_ptr, collection).is_err()) {
        std::lock_guard lock(m_com_mutex);
        m_mmdevice_collections.erase(collection);
        return com::E_INVALIDARG;
    }
    return com::S_OK;
}

u32 ComCoreManager::mmdevice_enumerator_get_default_audio_endpoint(
    Marshaler& m) {
    const GuestAddr object = m.arg_ptr(0);
    const u32 data_flow = static_cast<u32>(m.arg(1));
    const u32 role = static_cast<u32>(m.arg(2));
    const GuestAddr out_ptr = m.arg_ptr(3);
    if (out_ptr == 0) return com::E_POINTER;
    (void)m.write_ptr(out_ptr, 0);
    if (data_flow > 1u || role > 2u) return com::E_INVALIDARG;
    {
        std::lock_guard lock(m_com_mutex);
        if (!m_mmdevice_enumerators.contains(object)) return E_NOINTERFACE;
    }
    // Every role maps to the host default (render: the one output endpoint;
    // capture: the host default input, else the first input). No device of
    // the flow is E_NOTFOUND (Learn).
    AudioEndpointRef wanted{};
    if (data_flow == 0u) {
        if (!m_audio_endpoint_available) return E_NOTFOUND;
    } else {
        if (m_audio_capture == nullptr) return E_NOTFOUND;
        const auto devices = audio::capture_devices_in_id_order(*m_audio_capture);
        if (devices.empty()) return E_NOTFOUND;
        wanted = AudioEndpointRef{true, devices.front().uid};
    }
    const GuestAddr endpoint = create_mmdevice_endpoint(m, wanted);
    if (endpoint == 0 || m.write_ptr(out_ptr, endpoint).is_err()) {
        return com::E_INVALIDARG;
    }
    return com::S_OK;
}

u32 ComCoreManager::mmdevice_enumerator_get_device(Marshaler& m) {
    const GuestAddr object = m.arg_ptr(0);
    const GuestAddr endpoint_id = m.arg_ptr(1);
    const GuestAddr out_ptr = m.arg_ptr(2);
    if (endpoint_id == 0 || out_ptr == 0) return com::E_POINTER;
    (void)m.write_ptr(out_ptr, 0);
    {
        std::lock_guard lock(m_com_mutex);
        if (!m_mmdevice_enumerators.contains(object)) return E_NOINTERFACE;
    }
    const auto id = m.read_wide_string(endpoint_id);
    if (id.is_err()) return com::E_INVALIDARG;
    AudioEndpointRef wanted{};
    if (m_audio_endpoint_available && id.value() == "Vitreo.CoreAudio.Render") {
        wanted = AudioEndpointRef{false, {}};
    } else if (auto capture = capture_endpoint_by_id(id.value())) {
        wanted = *capture;
    } else {
        // Measured (mm GetDevice): an id of the endpoint form that names no
        // endpoint is E_NOTFOUND; a string that is no endpoint id at all is
        // E_INVALIDARG. *ppDevice stays NULL.
        return id.value().rfind("{0.0.", 0) == 0 || id.value() == "Vitreo.CoreAudio.Render"
            ? E_NOTFOUND : com::E_INVALIDARG;
    }
    const GuestAddr endpoint = create_mmdevice_endpoint(m, wanted);
    if (endpoint == 0 || m.write_ptr(out_ptr, endpoint).is_err()) {
        return com::E_INVALIDARG;
    }
    return com::S_OK;
}

u32 ComCoreManager::mmdevice_enumerator_register_notification(Marshaler& m) {
    const GuestAddr object = m.arg_ptr(0);
    const GuestAddr client = m.arg_ptr(1);
    if (client == 0) return com::E_POINTER;
    std::lock_guard lock(m_com_mutex);
    auto found = m_mmdevice_enumerators.find(object);
    if (found == m_mmdevice_enumerators.end()) return E_NOINTERFACE;
    // Per IMMDeviceEnumerator, registration deliberately does not AddRef the
    // client's IMMNotificationClient interface.
    found->second.notification_clients.insert(client);
    return com::S_OK;
}

u32 ComCoreManager::mmdevice_enumerator_unregister_notification(Marshaler& m) {
    const GuestAddr object = m.arg_ptr(0);
    const GuestAddr client = m.arg_ptr(1);
    if (client == 0) return com::E_POINTER;
    std::lock_guard lock(m_com_mutex);
    auto found = m_mmdevice_enumerators.find(object);
    if (found == m_mmdevice_enumerators.end()) return E_NOINTERFACE;
    return found->second.notification_clients.erase(client) != 0
        ? com::S_OK : E_NOTFOUND;
}

u32 ComCoreManager::mmdevice_collection_query_interface(Marshaler& m) {
    const GuestAddr object = m.arg_ptr(0);
    const GuestAddr iid_ptr = m.arg_ptr(1);
    const GuestAddr out_ptr = m.arg_ptr(2);
    if (iid_ptr == 0 || out_ptr == 0) return com::E_POINTER;
    (void)m.write_ptr(out_ptr, 0);
    const auto iid = m.read_struct<Guid>(iid_ptr);
    if (iid.is_err()) return com::E_INVALIDARG;
    std::lock_guard lock(m_com_mutex);
    auto found = m_mmdevice_collections.find(object);
    if (found == m_mmdevice_collections.end() ||
        (!guid_equal(iid.value(), IID_IUNKNOWN) &&
         !guid_equal(iid.value(), IID_IMMDEVICECOLLECTION))) {
        return E_NOINTERFACE;
    }
    ++found->second.refs;
    if (m.write_ptr(out_ptr, object).is_err()) {
        --found->second.refs;
        return com::E_INVALIDARG;
    }
    return com::S_OK;
}

u32 ComCoreManager::mmdevice_collection_add_ref(Marshaler& m) {
    std::lock_guard lock(m_com_mutex);
    auto found = m_mmdevice_collections.find(m.arg_ptr(0));
    if (found == m_mmdevice_collections.end()) return 0;
    return ++found->second.refs;
}

u32 ComCoreManager::mmdevice_collection_release(Marshaler& m) {
    std::lock_guard lock(m_com_mutex);
    auto found = m_mmdevice_collections.find(m.arg_ptr(0));
    if (found == m_mmdevice_collections.end()) return 0;
    const u32 refs = --found->second.refs;
    if (refs == 0) m_mmdevice_collections.erase(found);
    return refs;
}

u32 ComCoreManager::mmdevice_collection_get_count(Marshaler& m) {
    const GuestAddr object = m.arg_ptr(0);
    const GuestAddr out_count = m.arg_ptr(1);
    if (out_count == 0) return com::E_POINTER;
    std::lock_guard lock(m_com_mutex);
    auto found = m_mmdevice_collections.find(object);
    if (found == m_mmdevice_collections.end()) return E_NOINTERFACE;
    return m.write_u32(out_count, found->second.count).is_ok()
        ? com::S_OK : com::E_INVALIDARG;
}

u32 ComCoreManager::mmdevice_collection_item(Marshaler& m) {
    const GuestAddr object = m.arg_ptr(0);
    const u32 index = static_cast<u32>(m.arg(1));
    const GuestAddr out_device = m.arg_ptr(2);
    if (out_device == 0) return com::E_POINTER;
    (void)m.write_ptr(out_device, 0);
    AudioEndpointRef wanted{};
    {
        std::lock_guard lock(m_com_mutex);
        auto found = m_mmdevice_collections.find(object);
        if (found == m_mmdevice_collections.end()) return E_NOINTERFACE;
        if (index >= found->second.count) return com::E_INVALIDARG;
        wanted = found->second.endpoints[index];
    }
    const GuestAddr endpoint = create_mmdevice_endpoint(m, wanted);
    if (endpoint == 0 || m.write_ptr(out_device, endpoint).is_err()) {
        return com::E_INVALIDARG;
    }
    return com::S_OK;
}

GuestAddr ComCoreManager::create_mmdevice_endpoint(Marshaler& m,
                                                   const AudioEndpointRef& endpoint) {
    const char* names[] = {
        "Vitreo_MMDevice_QueryInterface",
        "Vitreo_MMDevice_AddRef",
        "Vitreo_MMDevice_Release",
        "Vitreo_MMDevice_Activate",
        "Vitreo_MMDevice_OpenPropertyStore",
        "Vitreo_MMDevice_GetId",
        "Vitreo_MMDevice_GetState",
    };
    const GuestAddr pointer_size = m.cpu_is_64bit() ? 8u : 4u;
    const GuestAddr object = reserve_com_scratch(
        pointer_size + static_cast<GuestAddr>(std::size(names)) * pointer_size);
    if (object == 0) return 0;
    const GuestAddr vtable = object + pointer_size;
    if (m.write_ptr(object, vtable).is_err()) return 0;
    for (usize slot = 0; slot < std::size(names); ++slot) {
        auto method = m_thunks->get_thunk_address("ole32.dll", names[slot]);
        if (method.is_err() ||
            m.write_ptr(vtable + slot * pointer_size, method.value()).is_err()) {
            return 0;
        }
    }
    std::lock_guard lock(m_com_mutex);
    m_mmdevice_refs.emplace(object, MmDeviceState{1u, endpoint});
    return object;
}

u32 ComCoreManager::mmdevice_query_interface(Marshaler& m) {
    const GuestAddr object = m.arg_ptr(0);
    const GuestAddr iid_ptr = m.arg_ptr(1);
    const GuestAddr out_ptr = m.arg_ptr(2);
    if (iid_ptr == 0 || out_ptr == 0) return com::E_POINTER;
    (void)m.write_ptr(out_ptr, 0);
    const auto iid = m.read_struct<Guid>(iid_ptr);
    if (iid.is_err()) return com::E_INVALIDARG;
    std::lock_guard lock(m_com_mutex);
    auto found = m_mmdevice_refs.find(object);
    if (found == m_mmdevice_refs.end()) {
        return E_NOINTERFACE;
    }
    if (guid_equal(iid.value(), IID_IMMENDPOINT)) {
        const GuestAddr endpoint = create_mmendpoint(m, found->second.endpoint.capture);
        return endpoint != 0 && m.write_ptr(out_ptr, endpoint).is_ok()
            ? com::S_OK : com::E_INVALIDARG;
    }
    if (!guid_equal(iid.value(), IID_IUNKNOWN) &&
        !guid_equal(iid.value(), IID_IMMDEVICE)) return E_NOINTERFACE;
    ++found->second.refs;
    return m.write_ptr(out_ptr, object).is_ok() ? com::S_OK : com::E_INVALIDARG;
}

u32 ComCoreManager::mmdevice_add_ref(Marshaler& m) {
    std::lock_guard lock(m_com_mutex);
    auto found = m_mmdevice_refs.find(m.arg_ptr(0));
    return found == m_mmdevice_refs.end() ? 0u : ++found->second.refs;
}

// Caller holds m_com_mutex for the endpoint reference table.
GuestAddr ComCoreManager::create_mmendpoint(Marshaler& m, bool capture) {
    const char* names[] = {
        "Vitreo_MMEndpoint_QueryInterface", "Vitreo_MMEndpoint_AddRef",
        "Vitreo_MMEndpoint_Release", "Vitreo_MMEndpoint_GetDataFlow"};
    const GuestAddr pointer_size = m.cpu_is_64bit() ? 8u : 4u;
    const GuestAddr object = reserve_com_scratch(
        pointer_size + std::size(names) * pointer_size);
    if (object == 0) return 0;
    const GuestAddr vtable = object + pointer_size;
    if (m.write_ptr(object, vtable).is_err()) return 0;
    for (usize slot = 0; slot < std::size(names); ++slot) {
        auto method = m_thunks->get_thunk_address("ole32.dll", names[slot]);
        if (method.is_err() ||
            m.write_ptr(vtable + slot * pointer_size, method.value()).is_err()) return 0;
    }
    m_mmendpoint_refs.emplace(object, MmEndpointState{1u, capture});
    return object;
}

u32 ComCoreManager::mmendpoint_query_interface(Marshaler& m) {
    const GuestAddr object = m.arg_ptr(0);
    const GuestAddr iid_ptr = m.arg_ptr(1);
    const GuestAddr out_ptr = m.arg_ptr(2);
    if (iid_ptr == 0 || out_ptr == 0) return com::E_POINTER;
    (void)m.write_ptr(out_ptr, 0);
    const auto iid = m.read_struct<Guid>(iid_ptr);
    if (iid.is_err()) return com::E_INVALIDARG;
    std::lock_guard lock(m_com_mutex);
    auto found = m_mmendpoint_refs.find(object);
    if (found == m_mmendpoint_refs.end() ||
        (!guid_equal(iid.value(), IID_IUNKNOWN) &&
         !guid_equal(iid.value(), IID_IMMENDPOINT))) return E_NOINTERFACE;
    ++found->second.refs;
    return m.write_ptr(out_ptr, object).is_ok() ? com::S_OK : com::E_INVALIDARG;
}

u32 ComCoreManager::mmendpoint_add_ref(Marshaler& m) {
    std::lock_guard lock(m_com_mutex);
    auto found = m_mmendpoint_refs.find(m.arg_ptr(0));
    return found == m_mmendpoint_refs.end() ? 0u : ++found->second.refs;
}

u32 ComCoreManager::mmendpoint_release(Marshaler& m) {
    std::lock_guard lock(m_com_mutex);
    auto found = m_mmendpoint_refs.find(m.arg_ptr(0));
    if (found == m_mmendpoint_refs.end()) return 0;
    const u32 refs = --found->second.refs;
    if (refs == 0) m_mmendpoint_refs.erase(found);
    return refs;
}

u32 ComCoreManager::mmendpoint_get_data_flow(Marshaler& m) {
    const GuestAddr out_flow = m.arg_ptr(1);
    if (out_flow == 0) return com::E_POINTER;
    std::lock_guard lock(m_com_mutex);
    auto found = m_mmendpoint_refs.find(m.arg_ptr(0));
    if (found == m_mmendpoint_refs.end()) return E_NOINTERFACE;
    // EDataFlow: eRender 0, eCapture 1.
    return m.write_u32(out_flow, found->second.capture ? 1u : 0u).is_ok()
        ? com::S_OK : com::E_INVALIDARG;
}

u32 ComCoreManager::mmdevice_release(Marshaler& m) {
    std::lock_guard lock(m_com_mutex);
    auto found = m_mmdevice_refs.find(m.arg_ptr(0));
    if (found == m_mmdevice_refs.end()) return 0;
    const u32 refs = --found->second.refs;
    if (refs == 0) m_mmdevice_refs.erase(found);
    return refs;
}

u32 ComCoreManager::activate_audio_interface_async(Marshaler& m) {
    const GuestAddr path = m.arg_ptr(0), iid_ptr = m.arg_ptr(1);
    const GuestAddr params = m.arg_ptr(2), handler = m.arg_ptr(3), out = m.arg_ptr(4);
    if (!out || !m.memory().guest_can_write(out, m.cpu_is_64bit() ? 8 : 4)) return com::E_POINTER;
    if (!path || !iid_ptr || !handler) return com::E_INVALIDARG;
    const auto name = m.read_wide_string(path);
    const auto iid = m.read_struct<Guid>(iid_ptr);
    if (name.is_err() || iid.is_err()) return com::E_INVALIDARG;
    const GuestAddr stride = m.cpu_is_64bit() ? 8 : 4;
    const auto vtable = m.read_ptr(handler).value_or(0);
    const auto callback = vtable ? m.read_ptr(vtable + 3 * stride).value_or(0) : 0;
    if (!callback) return com::E_INVALIDARG;
    if (!m_guest_call || !m_audio_activation_thread || !m_thunks) {
        static std::once_flag once;
        std::call_once(once, [] { std::fprintf(stderr, "[vitreo] UNIMPLEMENTED mmdevapi!ActivateAudioInterfaceAsync: guest MTA completion dispatcher unavailable\n"); });
        return com::E_NOTIMPL;
    }
    auto worker = m_thunks->get_thunk_address("ole32.dll", "Vitreo_AudioActivation_Worker");
    if (worker.is_err()) return com::E_NOTIMPL;
    std::string device = name.value();
    std::transform(device.begin(), device.end(), device.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    // A capture device path: DEVINTERFACE_AUDIO_CAPTURE for the default
    // capture device, or \\?\SWD#MMDEVAPI#<endpoint id>#<capture class> for
    // one endpoint (measured: both activate; a bare endpoint id, or an endpoint
    // under the render class, is HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)).
    constexpr std::string_view kSwdPrefix = "\\\\?\\swd#mmdevapi#";
    std::optional<AudioEndpointRef> capture_target;
    bool capture_path = false;
    if (device == kDefaultAudioCapture) {
        capture_path = true;
        if (m_audio_capture != nullptr) {
            const auto devices = audio::capture_devices_in_id_order(*m_audio_capture);
            if (!devices.empty()) capture_target = AudioEndpointRef{true, devices.front().uid};
        }
    } else if (device.rfind(kSwdPrefix, 0) == 0) {
        const std::string rest = device.substr(kSwdPrefix.size());
        const std::string suffix = std::string("#") + std::string(kDefaultAudioCapture);
        if (rest.size() > suffix.size() &&
            rest.compare(rest.size() - suffix.size(), suffix.size(), suffix) == 0) {
            capture_target = capture_endpoint_by_id(rest.substr(0, rest.size() - suffix.size()));
            capture_path = capture_target.has_value();
        }
    }
    u32 result = com::S_OK;
    GuestAddr activated = 0;
    if (!guid_equal(iid.value(), IID_IAUDIOCLIENT)) {
        note_unimplemented_audio_client_version(iid.value(),
                                                "mmdevapi!ActivateAudioInterfaceAsync");
        result = E_NOINTERFACE;
    } else if (params || device == kProcessAudioLoopback) {
        static std::once_flag once;
        std::call_once(once, [] { std::fprintf(stderr, "[vitreo] UNIMPLEMENTED mmdevapi!ActivateAudioInterfaceAsync: process loopback or activation parameters\n"); });
        result = com::E_NOTIMPL;
    } else if (capture_path) {
        if (!capture_target) {
            result = kAudioEndpointNotFound;
        } else if (!audio::capture_permitted(*m_audio_capture,
                                             "mmdevapi!ActivateAudioInterfaceAsync")) {
            // Learn (GetActivateResult): E_ACCESSDENIED when the user has not
            // given consent.
            result = capture_contract::E_ACCESSDENIED;
        } else {
            activated = create_audio_client(m, *capture_target);
            if (!activated) result = E_OUTOFMEMORY;
        }
    } else if (device != kDefaultAudioRender &&
               device != "vitreo.coreaudio.render") {
        result = kAudioPathNotFound;
    } else if (!m_audio_endpoint_available) {
        result = kAudioEndpointNotFound;
    } else {
        activated = create_audio_client(m, AudioEndpointRef{false, {}});
        if (!activated) result = E_OUTOFMEMORY;
    }
    static constexpr std::string_view methods[] = {
        "Vitreo_AudioActivation_QueryInterface", "Vitreo_AudioActivation_AddRef",
        "Vitreo_AudioActivation_Release", "Vitreo_AudioActivation_GetResult"};
    const GuestAddr operation = allocate_builtin_object(m, methods);
    if (!operation) {
        if (activated) guest_iunknown_release(m, activated);
        return E_OUTOFMEMORY;
    }
    guest_iunknown_add_ref(m, handler);
    {
        std::lock_guard lock(m_com_mutex);
        m_audio_activations.emplace(operation, AudioActivationState{2, handler, callback, activated, result, false});
    }
    (void)m.write_ptr(out, operation);
    if (!m_audio_activation_thread(worker.value(), operation)) {
        (void)release_audio_activation(m, operation);
        (void)release_audio_activation(m, operation);
        (void)m.write_ptr(out, 0);
        return E_OUTOFMEMORY;
    }
    set_last_error(0);
    return com::S_OK;
}

u32 ComCoreManager::audio_activation_worker(Marshaler& m) {
    const GuestAddr operation = m.arg_ptr(0);
    const u32 init = co_initialize_ex(m, com::COINIT_MULTITHREADED);
    GuestAddr handler = 0, callback = 0;
    {
        std::lock_guard lock(m_com_mutex);
        auto it = m_audio_activations.find(operation);
        if (it == m_audio_activations.end()) return 0;
        it->second.complete = true;
        handler = it->second.handler;
        callback = it->second.callback;
    }
    const u64 args[] = {handler, operation};
    (void)m_guest_call(callback, args, 2, m.cpu_is_64bit());
    (void)release_audio_activation(m, operation);
    if ((init & 0x80000000u) == 0) co_uninitialize(m);
    return 0;
}

u32 ComCoreManager::audio_activation_query_interface(Marshaler& m) {
    const auto iid = m.read_struct<Guid>(m.arg_ptr(1));
    const auto out = m.arg_ptr(2);
    if (!out || !m.memory().guest_can_write(out, m.cpu_is_64bit() ? 8 : 4)) return com::E_POINTER;
    (void)m.write_ptr(out, 0);
    if (iid.is_err()) return com::E_POINTER;
    std::lock_guard lock(m_com_mutex);
    auto it = m_audio_activations.find(m.arg_ptr(0));
    if (it == m_audio_activations.end() ||
        (!guid_equal(iid.value(), IID_IUNKNOWN) && !guid_equal(iid.value(), kAudioActivateAsyncIid))) return E_NOINTERFACE;
    ++it->second.refs;
    (void)m.write_ptr(out, it->first);
    return com::S_OK;
}

u32 ComCoreManager::audio_activation_add_ref(Marshaler& m) {
    std::lock_guard lock(m_com_mutex);
    auto it = m_audio_activations.find(m.arg_ptr(0));
    return it == m_audio_activations.end() ? 0u : ++it->second.refs;
}

u32 ComCoreManager::release_audio_activation(Marshaler& m, GuestAddr operation) {
    GuestAddr handler = 0, activated = 0;
    {
        std::lock_guard lock(m_com_mutex);
        auto it = m_audio_activations.find(operation);
        if (it == m_audio_activations.end()) return 0;
        const auto refs = --it->second.refs;
        if (refs) return refs;
        handler = it->second.handler;
        activated = it->second.activated;
        m_audio_activations.erase(it);
    }
    if (activated) guest_iunknown_release(m, activated);
    guest_iunknown_release(m, handler);
    return 0;
}

u32 ComCoreManager::audio_activation_release(Marshaler& m) {
    return release_audio_activation(m, m.arg_ptr(0));
}

u32 ComCoreManager::audio_activation_get_result(Marshaler& m) {
    const auto out_result = m.arg_ptr(1), out_interface = m.arg_ptr(2);
    if (!out_result || !out_interface || !m.memory().guest_can_write(out_result, 4) ||
        !m.memory().guest_can_write(out_interface, m.cpu_is_64bit() ? 8 : 4)) return com::E_POINTER;
    GuestAddr activated = 0;
    u32 result = 0;
    {
        std::lock_guard lock(m_com_mutex);
        auto it = m_audio_activations.find(m.arg_ptr(0));
        if (it == m_audio_activations.end()) return com::E_INVALIDARG;
        if (!it->second.complete) return kAudioActivationPending;
        activated = it->second.activated;
        result = it->second.result;
    }
    if (activated) guest_iunknown_add_ref(m, activated);
    (void)m.write_u32(out_result, result);
    (void)m.write_ptr(out_interface, activated);
    return com::S_OK;
}

u32 ComCoreManager::mmdevice_activate(Marshaler& m) {
    const GuestAddr object = m.arg_ptr(0);
    const GuestAddr iid_ptr = m.arg_ptr(1);
    const GuestAddr out_ptr = m.arg_ptr(4);
    if (iid_ptr == 0 || out_ptr == 0) return com::E_POINTER;
    (void)m.write_ptr(out_ptr, 0);
    AudioEndpointRef endpoint{};
    {
        std::lock_guard lock(m_com_mutex);
        auto found = m_mmdevice_refs.find(object);
        if (found == m_mmdevice_refs.end()) return E_NOINTERFACE;
        endpoint = found->second.endpoint;
    }
    const auto iid = m.read_struct<Guid>(iid_ptr);
    if (iid.is_err()) return com::E_INVALIDARG;
    if (!guid_equal(iid.value(), IID_IAUDIOCLIENT)) {
        note_unimplemented_audio_client_version(iid.value(), "IMMDevice::Activate");
        // IDirectSoundCapture(8) on a capture endpoint (Learn): the
        // DirectSound frontend is a separate module.
        constexpr Guid kIidDirectSoundCapture{
            0xb0210781u, 0x89cdu, 0x11d0u, {0xaf, 0x08, 0x00, 0xa0, 0xc9, 0x25, 0xcd, 0x16}};
        if (endpoint.capture && guid_equal(iid.value(), kIidDirectSoundCapture)) {
            static std::once_flag once;
            std::call_once(once, [] {
                std::fprintf(stderr, "[vitreo] UNIMPLEMENTED IMMDevice::Activate: "
                                     "IID_IDirectSoundCapture8 (use DirectSoundCaptureCreate8)\n");
            });
        }
        return E_NOINTERFACE;
    }
    const GuestAddr client = create_audio_client(m, endpoint);
    if (client == 0 || m.write_ptr(out_ptr, client).is_err()) return com::E_INVALIDARG;
    return com::S_OK;
}

u32 ComCoreManager::mmdevice_open_property_store(Marshaler& m) {
    const GuestAddr object = m.arg_ptr(0);
    const u32 access = static_cast<u32>(m.arg(1));
    const GuestAddr out_ptr = m.arg_ptr(2);
    if (out_ptr == 0) return com::E_POINTER;
    (void)m.write_ptr(out_ptr, 0);
    if (access != 0u && access != 1u && access != 2u) return com::E_INVALIDARG;
    AudioEndpointRef endpoint{};
    {
        std::lock_guard lock(m_com_mutex);
        auto found = m_mmdevice_refs.find(object);
        if (found == m_mmdevice_refs.end()) return E_NOINTERFACE;
        endpoint = found->second.endpoint;
    }
    const GuestAddr store = create_audio_property_store(m, endpoint);
    if (store == 0 || m.write_ptr(out_ptr, store).is_err()) {
        return com::E_INVALIDARG;
    }
    return com::S_OK;
}

GuestAddr ComCoreManager::create_audio_property_store(Marshaler& m,
                                                      const AudioEndpointRef& endpoint) {
    const char* names[] = {
        "Vitreo_AudioPropertyStore_QueryInterface",
        "Vitreo_AudioPropertyStore_AddRef",
        "Vitreo_AudioPropertyStore_Release",
        "Vitreo_AudioPropertyStore_GetCount",
        "Vitreo_AudioPropertyStore_GetAt",
        "Vitreo_AudioPropertyStore_GetValue",
        "Vitreo_AudioPropertyStore_SetValue",
        "Vitreo_AudioPropertyStore_Commit",
    };
    const GuestAddr pointer_size = m.cpu_is_64bit() ? 8u : 4u;
    const GuestAddr object = reserve_com_scratch(
        pointer_size + std::size(names) * pointer_size);
    if (object == 0) return 0;
    const GuestAddr vtable = object + pointer_size;
    if (m.write_ptr(object, vtable).is_err()) return 0;
    for (usize slot = 0; slot < std::size(names); ++slot) {
        auto method = m_thunks->get_thunk_address("ole32.dll", names[slot]);
        if (method.is_err() ||
            m.write_ptr(vtable + slot * pointer_size, method.value()).is_err()) {
            return 0;
        }
    }
    std::lock_guard lock(m_com_mutex);
    m_audio_property_store_refs.emplace(object, MmDeviceState{1u, endpoint});
    return object;
}

u32 ComCoreManager::property_store_query_interface(Marshaler& m) {
    const GuestAddr object = m.arg_ptr(0);
    const GuestAddr iid_ptr = m.arg_ptr(1);
    const GuestAddr out_ptr = m.arg_ptr(2);
    if (iid_ptr == 0 || out_ptr == 0) return com::E_POINTER;
    (void)m.write_ptr(out_ptr, 0);
    const auto iid = m.read_struct<Guid>(iid_ptr);
    if (iid.is_err()) return com::E_INVALIDARG;
    std::lock_guard lock(m_com_mutex);
    auto found = m_audio_property_store_refs.find(object);
    if (found == m_audio_property_store_refs.end() ||
        (!guid_equal(iid.value(), IID_IUNKNOWN) &&
         !guid_equal(iid.value(), IID_IPROPERTYSTORE))) return E_NOINTERFACE;
    ++found->second.refs;
    return m.write_ptr(out_ptr, object).is_ok() ? com::S_OK : com::E_INVALIDARG;
}

u32 ComCoreManager::property_store_add_ref(Marshaler& m) {
    std::lock_guard lock(m_com_mutex);
    auto found = m_audio_property_store_refs.find(m.arg_ptr(0));
    return found == m_audio_property_store_refs.end() ? 0u : ++found->second.refs;
}

u32 ComCoreManager::property_store_release(Marshaler& m) {
    std::lock_guard lock(m_com_mutex);
    auto found = m_audio_property_store_refs.find(m.arg_ptr(0));
    if (found == m_audio_property_store_refs.end()) return 0;
    const u32 refs = --found->second.refs;
    if (refs == 0) m_audio_property_store_refs.erase(found);
    return refs;
}

u32 ComCoreManager::property_store_get_count(Marshaler& m) {
    const GuestAddr out_count = m.arg_ptr(1);
    if (out_count == 0) return com::E_POINTER;
    std::lock_guard lock(m_com_mutex);
    if (!m_audio_property_store_refs.contains(m.arg_ptr(0))) return E_NOINTERFACE;
    return m.write_u32(out_count, 2u).is_ok() ? com::S_OK : com::E_INVALIDARG;
}

u32 ComCoreManager::property_store_get_at(Marshaler& m) {
    const u32 index = static_cast<u32>(m.arg(1));
    const GuestAddr out_key = m.arg_ptr(2);
    if (out_key == 0) return com::E_POINTER;
    if (index >= 2u) return com::E_INVALIDARG;
    {
        std::lock_guard lock(m_com_mutex);
        if (!m_audio_property_store_refs.contains(m.arg_ptr(0))) return E_NOINTERFACE;
    }
    if (m.write_struct(out_key, PKEY_DEVICE_NAMES_FMTID).is_err() ||
        m.write_u32(out_key + sizeof(Guid), index == 0 ? 2u : 14u).is_err()) {
        return com::E_INVALIDARG;
    }
    return com::S_OK;
}

u32 ComCoreManager::property_store_get_value(Marshaler& m) {
    const GuestAddr key_ptr = m.arg_ptr(1);
    const GuestAddr value_ptr = m.arg_ptr(2);
    if (key_ptr == 0 || value_ptr == 0) return com::E_POINTER;
    AudioEndpointRef endpoint{};
    {
        std::lock_guard lock(m_com_mutex);
        auto found = m_audio_property_store_refs.find(m.arg_ptr(0));
        if (found == m_audio_property_store_refs.end()) return E_NOINTERFACE;
        endpoint = found->second.endpoint;
    }
    // A capture endpoint's device (gone: its properties read as VT_EMPTY).
    std::optional<audio::CaptureDevice> capture_device;
    if (endpoint.capture && m_audio_capture != nullptr) {
        for (const auto& device : m_audio_capture->devices()) {
            if (device.uid == endpoint.uid) capture_device = device;
        }
    }
    // A PROPVARIANT is 16 bytes on x86 and 24 on x64; the value union starts
    // at +8 in both. Clear the whole structure so an untouched key reads back
    // as VT_EMPTY, which is what Windows returns for a property the store does
    // not carry.
    std::array<Byte, 24> empty{};
    const usize propvariant_bytes = m.cpu_is_64bit() ? 24u : 16u;
    if (m.memory().write_bytes(value_ptr, empty.data(),
                               propvariant_bytes).is_err()) {
        return com::E_INVALIDARG;
    }
    const auto fmtid = m.read_struct<Guid>(key_ptr);
    const auto pid = m.memory().read_u32(key_ptr + sizeof(Guid));
    if (fmtid.is_err() || pid.is_err()) return com::E_INVALIDARG;

    // PKEY_AudioEngine_DeviceFormat. Returning VT_EMPTY here is not harmless:
    // a client reads no channel count and no sample rate, and then fails to
    // create its render pipeline. UE's FWindowsMMDeviceCache logs "Failed to
    // get Format for active device" and XAudio2's CreateMasteringVoice fails
    // with E_NOINTERFACE on (NumChannels=0, SampleRate=0), after which the
    // engine falls back to a simulated null audio device.
    if (guid_equal(fmtid.value(), PKEY_AUDIOENGINE_FMTID) &&
        pid.value() == 0u && endpoint.capture) {
        // A capture endpoint's device format: its rate and channels as a
        // WAVEFORMATEXTENSIBLE (measured: a 40-byte VT_BLOB).
        if (!capture_device) return com::S_OK;
        if (m_guest_task_memory == nullptr) return com::E_NOTIMPL;
        const GuestAddr blob = m_guest_task_memory->allocate(MixWaveFormat::BYTES);
        if (blob == 0) return E_OUTOFMEMORY;
        const GuestAddr blob_ptr_field = value_ptr + (m.cpu_is_64bit() ? 16u : 12u);
        if (!write_capture_mix_format(m, blob, capture_device_format(*capture_device)) ||
            m.memory().write_u16(value_ptr, 65u).is_err() ||
            m.memory().write_u32(value_ptr + 8u, MixWaveFormat::BYTES).is_err() ||
            m.write_ptr(blob_ptr_field, blob).is_err()) {
            m_guest_task_memory->free(blob);
            return com::E_INVALIDARG;
        }
        return com::S_OK;
    }
    if (guid_equal(fmtid.value(), PKEY_AUDIOENGINE_FMTID) &&
        pid.value() == 0u) {
        if (m_guest_task_memory == nullptr) return com::E_NOTIMPL;
        const GuestAddr blob =
            m_guest_task_memory->allocate(EndpointWaveFormat::BYTES);
        if (blob == 0) return E_OUTOFMEMORY;
        // VT_BLOB (65): BLOB{ ULONG cbSize; BYTE* pBlobData; } at the union,
        // so the size is at +8 and the pointer at the next pointer-aligned
        // slot — +12 on x86, +16 on x64.
        const GuestAddr blob_ptr_field =
            value_ptr + (m.cpu_is_64bit() ? 16u : 12u);
        if (!write_endpoint_wave_format(m, blob) ||
            m.memory().write_u16(value_ptr, 65u).is_err() ||
            m.memory().write_u32(value_ptr + 8u,
                                 EndpointWaveFormat::BYTES).is_err() ||
            m.write_ptr(blob_ptr_field, blob).is_err()) {
            m_guest_task_memory->free(blob);
            return com::E_INVALIDARG;
        }
        return com::S_OK;
    }

    if (!guid_equal(fmtid.value(), PKEY_DEVICE_NAMES_FMTID) ||
        (pid.value() != 2u && pid.value() != 14u)) return com::S_OK;
    if (m_guest_task_memory == nullptr) return com::E_NOTIMPL;
    // PKEY_Device_DeviceDesc (2) and PKEY_Device_FriendlyName (14): the host
    // device name (the name DirectSound enumerates and waveIn reports).
    std::u16string name = u"Vitreo CoreAudio Output";
    if (endpoint.capture) {
        if (!capture_device) return com::S_OK;
        name = capture_device->name;
    }
    const usize name_bytes = (name.size() + 1) * sizeof(char16_t);
    const GuestAddr allocation = m_guest_task_memory->allocate(name_bytes);
    if (allocation == 0 ||
        m.memory().write_bytes(allocation,
            reinterpret_cast<const Byte*>(name.c_str()), name_bytes).is_err() ||
        m.memory().write_u16(value_ptr, 31u).is_err() ||
        m.write_ptr(value_ptr + 8u, allocation).is_err()) {
        if (allocation != 0) m_guest_task_memory->free(allocation);
        return com::E_INVALIDARG;
    }
    return com::S_OK;
}

u32 ComCoreManager::property_store_set_value(Marshaler&) {
    return 0x80030005u;
}

u32 ComCoreManager::property_store_commit(Marshaler&) {
    return 0x80030005u;
}

u32 ComCoreManager::mmdevice_get_id(Marshaler& m) {
    const GuestAddr out_ptr = m.arg_ptr(1);
    if (out_ptr == 0) return com::E_POINTER;
    (void)m.write_ptr(out_ptr, 0);
    if (m_guest_task_memory == nullptr) return com::E_NOTIMPL;
    AudioEndpointRef endpoint{};
    {
        std::lock_guard lock(m_com_mutex);
        auto found = m_mmdevice_refs.find(m.arg_ptr(0));
        if (found == m_mmdevice_refs.end()) return E_NOINTERFACE;
        endpoint = found->second.endpoint;
    }
    std::u16string id = AUDIO_RENDER_ENDPOINT_ID;
    if (endpoint.capture) {
        const std::string text = audio::capture_endpoint_id(endpoint.uid);
        id.assign(text.begin(), text.end());
    }
    const usize bytes = (id.size() + 1) * sizeof(char16_t);
    const GuestAddr allocation = m_guest_task_memory->allocate(bytes);
    if (allocation == 0 ||
        m.memory().write_bytes(
            allocation, reinterpret_cast<const Byte*>(id.c_str()), bytes).is_err() ||
        m.write_ptr(out_ptr, allocation).is_err()) {
        if (allocation != 0) m_guest_task_memory->free(allocation);
        return com::E_INVALIDARG;
    }
    return com::S_OK;
}

u32 ComCoreManager::mmdevice_get_state(Marshaler& m) {
    const GuestAddr out_ptr = m.arg_ptr(1);
    if (out_ptr == 0) return com::E_POINTER;
    AudioEndpointRef endpoint{};
    {
        std::lock_guard lock(m_com_mutex);
        auto found = m_mmdevice_refs.find(m.arg_ptr(0));
        if (found == m_mmdevice_refs.end()) return E_NOINTERFACE;
        endpoint = found->second.endpoint;
    }
    // DEVICE_STATE_ACTIVE (1); a capture device that has left the host is
    // DEVICE_STATE_NOTPRESENT (4).
    u32 state = 0x1u;
    if (endpoint.capture) {
        state = 0x4u;
        if (m_audio_capture != nullptr) {
            for (const auto& device : m_audio_capture->devices()) {
                if (device.uid == endpoint.uid) state = 0x1u;
            }
        }
    }
    return m.write_u32(out_ptr, state).is_ok() ? com::S_OK : com::E_INVALIDARG;
}

}  // namespace vitreo::winapi
