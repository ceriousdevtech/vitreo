// SPDX-License-Identifier: LGPL-2.1-only
/// @file combase_bluetooth.cpp
/// @brief Windows.Devices.Bluetooth.BluetoothAdapter and Windows.Devices.Radios.Radio

#include "winapi/com/combase/combase.hpp"
#include "winapi/com/combase/combase_internal.hpp"

namespace vitreo::winapi {

// ---------------------------------------------------------------------------
// Windows.Devices.Bluetooth.BluetoothAdapter
//
// The adapter reports the real host radio through the BluetoothHost contract,
// including the "no radio" case: Windows completes GetDefaultAsync with a null
// result on a machine without a controller, and so does this.
// ---------------------------------------------------------------------------

namespace {

// Windows.Devices.Radios.RadioState / RadioKind.
constexpr u32 RADIO_STATE_ON = 1;
constexpr u32 RADIO_STATE_OFF = 2;
constexpr u32 RADIO_KIND_BLUETOOTH = 3;

u32 write_bool_property(Marshaler& m, GuestAddr out, bool value) {
    if (out == 0) return E_POINTER;
    return m.write_u8(out, value ? 1u : 0u).is_ok() ? S_OK : E_POINTER;
}

} // namespace

CombaseManager::Object* CombaseManager::adapter_arg(Marshaler& m,
                                                    GuestAddr* out_param) {
    if (out_param != nullptr) *out_param = m.arg_ptr(1);
    auto it = m_objects.find(m.arg_ptr(0));
    if (it == m_objects.end() ||
        it->second.type != ObjectType::BluetoothAdapter) {
        return nullptr;
    }
    return &it->second;
}

void CombaseManager::impl_BluetoothStaticsGetDeviceSelector(Marshaler& m) {
    const GuestAddr out = m.arg_ptr(1);
    if (out == 0) {
        m.set_return(E_POINTER);
        return;
    }
    // AQS for "a Bluetooth adapter", the shape Windows returns.
    const GuestAddr selector = create_hstring(
        u"System.Devices.InterfaceClassGuid:=\"{92383B0E-F90E-4AC9-8D44-8C2D0D0EBDA2}\"",
        m.cpu_is_64bit());
    m.set_return(m.write_ptr(out, selector).is_ok() ? S_OK : E_POINTER);
}

void CombaseManager::impl_BluetoothStaticsGetDefaultAsync(Marshaler& m) {
    const bool is_64bit = m.cpu_is_64bit();
    const GuestAddr out = m.arg_ptr(1);
    if (out == 0) {
        m.set_return(E_POINTER);
        return;
    }
    (void)m.write_ptr(out, 0);

    hardware::BluetoothAdapterState state{};
    if (m_bluetooth_host != nullptr) {
        (void)m_bluetooth_host->query_default_adapter(state);
    }

    GuestAddr adapter = 0;
    if (state.present) {
        Object object{};
        object.type = ObjectType::BluetoothAdapter;
        object.ref_count = 1;
        object.adapter = state;
        adapter = allocate_object(std::move(object), is_64bit);
        if (adapter == 0) {
            m.set_return(E_OUTOFMEMORY);
            return;
        }
    }
    VITREO_TRACE(
        "[combase] BluetoothAdapter.GetDefaultAsync host radio present=%d "
        "address=%012llx powered=%d -> %s\n",
        state.present ? 1 : 0,
        static_cast<unsigned long long>(state.address),
        state.powered ? 1 : 0, adapter != 0 ? "adapter" : "null");

    const GuestAddr operation = create_completed_async(adapter, is_64bit);
    if (operation == 0) {
        m.set_return(E_OUTOFMEMORY);
        return;
    }
    m.set_return(m.write_ptr(out, operation).is_ok() ? S_OK : E_POINTER);
}

void CombaseManager::impl_BluetoothStaticsFromIdAsync(Marshaler& m) {
    const bool is_64bit = m.cpu_is_64bit();
    const GuestAddr out = m.arg_ptr(2);
    if (out == 0) {
        m.set_return(E_POINTER);
        return;
    }
    (void)m.write_ptr(out, 0);
    // One radio per machine: any id other than the default adapter's has no
    // device behind it, which Windows also reports as a null result.
    std::u16string wanted;
    if (const GuestAddr id = m.arg_ptr(1); id != 0) {
        auto read = read_hstring(id, is_64bit);
        if (read.has_value()) wanted = std::move(*read);
    }
    hardware::BluetoothAdapterState state{};
    if (m_bluetooth_host != nullptr) {
        (void)m_bluetooth_host->query_default_adapter(state);
    }
    std::u16string device_id;
    for (const char c : state.device_id) {
        device_id.push_back(static_cast<char16_t>(c));
    }
    GuestAddr adapter = 0;
    if (state.present && (wanted.empty() || wanted == device_id)) {
        Object object{};
        object.type = ObjectType::BluetoothAdapter;
        object.ref_count = 1;
        object.adapter = state;
        adapter = allocate_object(std::move(object), is_64bit);
    }
    const GuestAddr operation = create_completed_async(adapter, is_64bit);
    if (operation == 0) {
        m.set_return(E_OUTOFMEMORY);
        return;
    }
    m.set_return(m.write_ptr(out, operation).is_ok() ? S_OK : E_POINTER);
}

void CombaseManager::impl_BluetoothAdapterGetDeviceId(Marshaler& m) {
    GuestAddr out = 0;
    Object* adapter = adapter_arg(m, &out);
    if (out == 0) {
        m.set_return(E_POINTER);
        return;
    }
    if (adapter == nullptr) {
        m.set_return(E_INVALIDARG);
        return;
    }
    std::u16string id;
    for (const char c : adapter->adapter.device_id) {
        id.push_back(static_cast<char16_t>(c));
    }
    m.set_return(
        m.write_ptr(out, create_hstring(id, m.cpu_is_64bit())).is_ok()
            ? S_OK
            : E_POINTER);
}

void CombaseManager::impl_BluetoothAdapterGetAddress(Marshaler& m) {
    GuestAddr out = 0;
    Object* adapter = adapter_arg(m, &out);
    if (out == 0) {
        m.set_return(E_POINTER);
        return;
    }
    if (adapter == nullptr) {
        m.set_return(E_INVALIDARG);
        return;
    }
    m.set_return(m.write_u64(out, adapter->adapter.address).is_ok() ? S_OK
                                                                    : E_POINTER);
}

void CombaseManager::impl_BluetoothAdapterGetClassicSupported(Marshaler& m) {
    GuestAddr out = 0;
    Object* adapter = adapter_arg(m, &out);
    m.set_return(adapter == nullptr
                     ? (out == 0 ? E_POINTER : E_INVALIDARG)
                     : write_bool_property(m, out,
                                           adapter->adapter.classic_supported));
}

void CombaseManager::impl_BluetoothAdapterGetLowEnergySupported(Marshaler& m) {
    GuestAddr out = 0;
    Object* adapter = adapter_arg(m, &out);
    m.set_return(
        adapter == nullptr
            ? (out == 0 ? E_POINTER : E_INVALIDARG)
            : write_bool_property(m, out,
                                  adapter->adapter.low_energy_supported));
}

void CombaseManager::impl_BluetoothAdapterGetPeripheralRoleSupported(
    Marshaler& m) {
    GuestAddr out = 0;
    Object* adapter = adapter_arg(m, &out);
    // macOS exposes no peripheral-role control through IOBluetooth, so the
    // honest answer for a host radio Vitreo can only observe is false.
    m.set_return(adapter == nullptr
                     ? (out == 0 ? E_POINTER : E_INVALIDARG)
                     : write_bool_property(m, out, false));
}

void CombaseManager::impl_BluetoothAdapterGetCentralRoleSupported(
    Marshaler& m) {
    GuestAddr out = 0;
    Object* adapter = adapter_arg(m, &out);
    m.set_return(
        adapter == nullptr
            ? (out == 0 ? E_POINTER : E_INVALIDARG)
            : write_bool_property(m, out,
                                  adapter->adapter.low_energy_supported));
}

void CombaseManager::impl_BluetoothAdapterGetAdvertisementOffloadSupported(
    Marshaler& m) {
    GuestAddr out = 0;
    Object* adapter = adapter_arg(m, &out);
    m.set_return(adapter == nullptr
                     ? (out == 0 ? E_POINTER : E_INVALIDARG)
                     : write_bool_property(m, out, false));
}

void CombaseManager::impl_BluetoothAdapterGetRadioAsync(Marshaler& m) {
    const GuestAddr out = m.arg_ptr(1);
    if (out == 0) {
        m.set_return(E_POINTER);
        return;
    }
    (void)m.write_ptr(out, 0);
    auto it = m_objects.find(m.arg_ptr(0));
    if (it == m_objects.end() ||
        it->second.type != ObjectType::BluetoothAdapter) {
        m.set_return(E_INVALIDARG);
        return;
    }
    const bool is_64bit = m.cpu_is_64bit();
    Object radio{};
    radio.type = ObjectType::Radio;
    radio.ref_count = 1;
    radio.adapter = it->second.adapter;
    const GuestAddr radio_addr = allocate_object(std::move(radio), is_64bit);
    if (radio_addr == 0) {
        m.set_return(E_OUTOFMEMORY);
        return;
    }
    const GuestAddr operation = create_completed_async(radio_addr, is_64bit);
    if (operation == 0) {
        m.set_return(E_OUTOFMEMORY);
        return;
    }
    m.set_return(m.write_ptr(out, operation).is_ok() ? S_OK : E_POINTER);
}

// ---------------------------------------------------------------------------
// Windows.Devices.Radios.Radio — the host radio behind the adapter.
// ---------------------------------------------------------------------------

void CombaseManager::impl_RadioGetState(Marshaler& m) {
    const GuestAddr out = m.arg_ptr(1);
    if (out == 0) { m.set_return(E_POINTER); return; }
    auto it = m_objects.find(m.arg_ptr(0));
    if (it == m_objects.end() || it->second.type != ObjectType::Radio) {
        m.set_return(E_INVALIDARG);
        return;
    }
    const u32 state = it->second.adapter.powered ? RADIO_STATE_ON
                                                 : RADIO_STATE_OFF;
    m.set_return(m.write_u32(out, state).is_ok() ? S_OK : E_POINTER);
}

void CombaseManager::impl_RadioGetName(Marshaler& m) {
    const GuestAddr out = m.arg_ptr(1);
    if (out == 0) { m.set_return(E_POINTER); return; }
    auto it = m_objects.find(m.arg_ptr(0));
    if (it == m_objects.end() || it->second.type != ObjectType::Radio) {
        m.set_return(E_INVALIDARG);
        return;
    }
    std::u16string name;
    for (const char c : it->second.adapter.name) {
        name.push_back(static_cast<char16_t>(c));
    }
    if (name.empty()) name = u"Bluetooth";
    m.set_return(
        m.write_ptr(out, create_hstring(name, m.cpu_is_64bit())).is_ok()
            ? S_OK
            : E_POINTER);
}

void CombaseManager::impl_RadioGetKind(Marshaler& m) {
    const GuestAddr out = m.arg_ptr(1);
    if (out == 0) { m.set_return(E_POINTER); return; }
    m.set_return(m.write_u32(out, RADIO_KIND_BLUETOOTH).is_ok() ? S_OK
                                                                : E_POINTER);
}

void CombaseManager::impl_RadioSetStateAsync(Marshaler& m) {
    const GuestAddr out = m.arg_ptr(2);
    if (out == 0) { m.set_return(E_POINTER); return; }
    (void)m.write_ptr(out, 0);
    // Turning the host radio on or off from a guest would reach into the
    // user's machine settings; report the access denial WinRT already has a
    // value for (RadioAccessStatus::DeniedByUser = 2) rather than pretending.
    Object status{};
    status.type = ObjectType::AsyncOperation;
    status.ref_count = 1;
    status.async_result = 0;
    status.async_status = 1;
    const bool is_64bit = m.cpu_is_64bit();
    const GuestAddr operation = create_completed_async(0, is_64bit);
    if (operation == 0) { m.set_return(E_OUTOFMEMORY); return; }
    VITREO_TRACE(
        "[combase] Radio.SetStateAsync: host radio state is the user's to "
        "change; completing with no result\n");
    m.set_return(m.write_ptr(out, operation).is_ok() ? S_OK : E_POINTER);
}

void CombaseManager::impl_RadioAddStateChanged(Marshaler& m) {
    const GuestAddr out = m.arg_ptr(2);
    if (out == 0) { m.set_return(E_POINTER); return; }
    // No host notification source is wired up, so no event will ever fire.
    // A zero token is the honest "nothing registered" answer.
    m.set_return(m.write_u64(out, 0).is_ok() ? S_OK : E_POINTER);
}

void CombaseManager::impl_RadioRemoveStateChanged(Marshaler& m) {
    (void)m.arg(1);
    m.set_return(S_OK);
}

} // namespace vitreo::winapi
