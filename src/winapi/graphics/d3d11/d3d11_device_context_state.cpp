// SPDX-License-Identifier: LGPL-2.1-only

/// @file d3d11_device_context_state.cpp
/// @brief ID3DDeviceContextState and SwapDeviceContextState.
///
/// ID3D11Device1::CreateDeviceContextState, the state object's methods
/// and release (which drops the bindings a detached state still holds),
/// and ID3D11DeviceContext1::SwapDeviceContextState.
///
/// Split out of d3d11_device_ext.cpp.

#include "winapi/graphics/d3d11/d3d11.hpp"
#include "winapi/graphics/d3d11/d3d11_internal.hpp"
#include "winapi/core/marshaler.hpp"
#include "graphics/contracts/backend_device.hpp"
#include "core/memory/guest_memory.hpp"

namespace vitreo::winapi {

std::string d3d11_device_context_state_method_export_name(usize slot) {
    char name[64];
    std::snprintf(name, sizeof(name), "_D3D11_DeviceContextState_%03zu", slot);
    return name;
}

// -----------------------------------------------------------------------------
// ID3DDeviceContextState / SwapDeviceContextState
// -----------------------------------------------------------------------------

void D3D11Manager::create_device_context_state(Marshaler& m, GuestAddr self) {
    // (Flags, pFeatureLevels, FeatureLevels, SDKVersion, EmulatedInterface,
    //  pChosenFeatureLevel, ppContextState)
    const u32 flags = static_cast<u32>(m.arg(1));
    const GuestAddr levels = m.arg_ptr(2);
    const u32 level_count = static_cast<u32>(m.arg(3));
    const u32 sdk_version = static_cast<u32>(m.arg(4));
    const GuestAddr iid_address = m.arg_ptr(5);
    const GuestAddr chosen_out = m.arg_ptr(6);
    const GuestAddr state_out = m.arg_ptr(7);
    if (state_out != 0) (void)m.write_ptr(state_out, 0);
    // Learn: *pChosenFeatureLevel is 0 on failure (measured on every
    // refusal below).
    const auto fail = [&](u32 hr) {
        if (chosen_out != 0) (void)m_memory.write_u32(chosen_out, 0);
        m.set_return(hr);
    };
    const auto owner_it = m_interfaces.find(self);
    const GuestAddr owner =
        owner_it != m_interfaces.end() ? owner_it->second.owner : 0;
    const auto device = m_devices.find(owner);
    // From its first CreateDeviceContextState on -- whatever the arguments,
    // a refused call included -- the device answers ID3D10Device and
    // ID3D10Device1 (measured, d3d10_contract_probe [emu]: E_NOINTERFACE
    // before, S_OK after a call that failed with E_INVALIDARG).
    if (device != m_devices.end()) device->second.d3d10_interfaces = true;
    GuidBytes iid{};
    // D3D11_1_CREATE_DEVICE_CONTEXT_STATE_SINGLETHREADED is the only flag,
    // and it must say what the device's creation flags say (Learn: a
    // single-threaded device's states must be single-threaded; measured, the
    // flag on a free-threaded device is E_INVALIDARG). Other bits are
    // ignored (measured: flags 2 succeeds).
    const bool device_single_threaded =
        device != m_devices.end() &&
        (device->second.creation_flags &
         d3d11::CREATE_DEVICE_SINGLETHREADED) != 0;
    if (device == m_devices.end() ||
        ((flags & 1u) != 0) != device_single_threaded ||
        sdk_version != d3d11::SDK_VERSION || levels == 0 ||
        level_count == 0 || !read_guid(iid_address, iid)) {
        fail(d3d11::E_INVALIDARG);
        return;
    }
    const bool d3d10 = guid_equal(iid, IID_ID3D10DEVICE) ||
                       guid_equal(iid, IID_ID3D10DEVICE1);
    const bool d3d11 =
        guid_equal(iid, IID_ID3D11DEVICE) || guid_equal(iid, IID_ID3D11DEVICE1) ||
        guid_equal(iid, IID_ID3D11DEVICE2) || guid_equal(iid, IID_ID3D11DEVICE3) ||
        guid_equal(iid, IID_ID3D11DEVICE4) || guid_equal(iid, IID_ID3D11DEVICE5);
    if (!d3d10 && !d3d11) {
        fail(d3d11::E_INVALIDARG);
        return;
    }
    // Every entry must be a D3D_FEATURE_LEVEL (measured: 0x1234 is
    // E_INVALIDARG), the same rule D3D11CreateDevice applies. The chosen
    // level is the first one the ADAPTER supports, not capped at the
    // device's own level (measured: {11_1, 11_0} on an 11_0 device of an
    // 11_1 adapter chooses 11_1), and at most 10_1 for a D3D10 emulated
    // interface (Windows refuses D3D10 emulation at 11_0 -- measured).
    bool invalid = false;
    (void)select_feature_level(levels, level_count, invalid);
    if (invalid) {
        fail(d3d11::E_INVALIDARG);
        return;
    }
    u32 chosen = 0;
    for (u32 i = 0; i < level_count; ++i) {
        const auto level = m_memory.read_u32(levels + i * 4);
        if (level.is_err()) break;
        const u32 value = level.value();
        if (value <= d3d11::FEATURE_LEVEL_11_0 &&
            (!d3d10 || value <= 0xa100u)) {
            chosen = value;
            break;
        }
    }
    if (chosen == 0) {
        fail(d3d11::E_INVALIDARG);
        return;
    }
    if (chosen_out != 0) (void)m_memory.write_u32(chosen_out, chosen);
    if (state_out == 0) {
        m.set_return(1u);  // S_FALSE, measured
        return;
    }
    if (!ensure_storage()) {
        m.set_return(d3d11::E_FAIL);
        return;
    }
    const GuestAddr address = allocate_object(m_device_context_state_vtable);
    if (address == 0) {
        m.set_return(0x8007000Eu);  // E_OUTOFMEMORY
        return;
    }
    DeviceContextStateObject object{};
    object.interface_address = address;
    object.device_owner = owner;
    object.feature_level = chosen;
    // While it is active the D3D10 device methods are live and the
    // immediate context's are off (swap_device_context_state).
    object.d3d10 = d3d10;
    m_device_context_states.emplace(address, std::move(object));
    acquire_parent(owner);
    (void)m.write_ptr(state_out, address);
    m.set_return(d3d11::S_OK);
}

void D3D11Manager::release_bindings(ContextState&& state) {
    // The output-merger views are released by the dispatcher's own path for a
    // live context; a detached state has to do it here.
    for (const GuestAddr view : state.render_targets) {
        if (view != 0) (void)release_render_target_view(view);
    }
    if (state.depth_stencil_view != 0) {
        (void)release_depth_stencil_view(state.depth_stencil_view);
    }
    state.render_targets.fill(0);
    state.depth_stencil_view.set(0);
    // Everything else goes through clear_context_state, under a key no real
    // context uses.
    constexpr GuestAddr kDetachedState = 1;
    m_context_states[kDetachedState] = std::move(state);
    clear_context_state(kDetachedState);
    invalidate_context_lookups();  // an m_context_states erase (context memo)
    m_context_states.erase(kDetachedState);
}

u32 D3D11Manager::release_device_context_state(GuestAddr address) {
    auto it = m_device_context_states.find(address);
    if (it == m_device_context_states.end() || it->second.ref_count == 0) {
        return 0;
    }
    const u32 count = --it->second.ref_count;
    if (count == 0) {
        ContextState stored = std::move(it->second.stored);
        m_device_context_states.erase(it);
        release_bindings(std::move(stored));
        free_object(address);
    }
    return count;
}

void D3D11Manager::swap_device_context_state(Marshaler& m, GuestAddr self) {
    // SwapDeviceContextState(pState, ppPreviousState): the context's bindings
    // become pState's, and the state they belonged to is handed back. A fresh
    // state holds the default (empty) bindings -- measured.
    const GuestAddr state = m.arg_ptr(1);
    const GuestAddr previous_out = m.arg_ptr(2);
    if (previous_out != 0) (void)m.write_ptr(previous_out, 0);
    if (state == 0 || is_deferred_context(self) ||
        m_device_context_states.find(state) == m_device_context_states.end()) {
        // A deferred context cannot swap, and an unknown state is ignored.
        m.set_return(0);
        return;
    }
    const GuestAddr owner = m_interfaces.find(self)->second.owner;
    GuestAddr previous = m_active_context_state[self];
    if (previous == 0) {
        // The first swap: the bindings the context has had all along become a
        // state object of their own, owned by the context.
        if (!ensure_storage()) {
            m.set_return(0);
            return;
        }
        previous = allocate_object(m_device_context_state_vtable);
        if (previous == 0) {
            m.set_return(0);
            return;
        }
        // Held by the context alone until it is handed out below.
        DeviceContextStateObject implicit{};
        implicit.interface_address = previous;
        implicit.device_owner = owner;
        implicit.feature_level = m_devices[owner].feature_level;
        implicit.d3d10 = m_devices[owner].d3d10_active;
        implicit.public_refs = 0;
        m_device_context_states.emplace(previous, std::move(implicit));
    }
    if (previous != state) {
        auto& incoming_object = m_device_context_states.at(state);
        ContextState incoming = std::move(incoming_object.stored);
        ContextState outgoing = std::move(m_context_states[self]);
        // Translation caches belong to the context, not to a state.
        incoming.pipelines = std::move(outgoing.pipelines);
        m_context_states[self] = std::move(incoming);
        m_device_context_states.at(previous).stored = std::move(outgoing);
        // The context's own reference: the application's count does not
        // move (measured "swap in: state count 1->1").
        (void)hold_object(state);
        m_active_context_state[self] = state;
        // The state's feature level and emulated interface become the
        // device's (Learn: "the device behaviors that are associated with
        // both the context state object's feature level and its compatible
        // interface are activated"; measured, GetFeatureLevel of both the
        // D3D11 and the D3D10 interface reads the state's level).
        {
            Device& active_device = m_devices[owner];
            const auto& incoming_state = m_device_context_states.at(state);
            active_device.feature_level = incoming_state.feature_level;
            active_device.d3d10_active = incoming_state.d3d10;
        }
        // The context's reference on the previous state becomes the
        // caller's.
        if (previous_out != 0) {
            (void)retain_object(previous);
            (void)m.write_ptr(previous_out, previous);
        }
        release_internal(previous);
    } else if (previous_out != 0) {
        // Swapping in the live state: the context keeps it, and the caller
        // gets it back with a reference of its own.
        (void)retain_object(state);
        (void)m.write_ptr(previous_out, state);
    }
    m.set_return(0);
}

void D3D11Manager::impl_DeviceContextStateMethod(Marshaler& m, usize slot) {
    const GuestAddr self = m.arg_ptr(0);
    auto it = m_device_context_states.find(self);
    if (it == m_device_context_states.end()) {
        m.set_return(slot == 1 || slot == 2 ? 0 : d3d11::E_INVALIDARG);
        return;
    }
    switch (slot) {
    case 0: {
        const GuestAddr out = m.arg_ptr(2);
        if (out == 0) {
            m.set_return(d3d11::E_POINTER);
            return;
        }
        (void)m.write_ptr(out, 0);
        GuidBytes iid{};
        if (!read_guid(m.arg_ptr(1), iid) ||
            (!guid_equal(iid, IID_IUNKNOWN) &&
             !guid_equal(iid, IID_ID3D11DEVICECHILD) &&
             !guid_equal(iid, IID_ID3DDEVICECONTEXTSTATE))) {
            m.set_return(d3d11::E_NOINTERFACE);
            return;
        }
        (void)retain_object(self);
        (void)m.write_ptr(out, self);
        m.set_return(d3d11::S_OK);
        return;
    }
    case 1:
        m.set_return(retain_object(self));
        return;
    case 2:
        m.set_return(release_object(self));
        return;
    case 3: {
        const GuestAddr device_out = m.arg_ptr(1);
        auto device = m_devices.find(it->second.device_owner);
        if (device_out != 0 && device != m_devices.end()) {
            (void)add_ref(device->second.device_interface);
            (void)m.write_ptr(device_out, device->second.device_interface);
        }
        m.set_return(0);
        return;
    }
    case 4:
        m.set_return(get_private_data(m, it->second.private_data));
        return;
    case 5:
        m.set_return(set_private_data(m, it->second.private_data));
        return;
    case 6:
        m.set_return(set_private_data_interface(m, it->second.private_data));
        return;
    default:
        m.set_return(d3d11::E_NOTIMPL);
        return;
    }
}

}  // namespace vitreo::winapi
