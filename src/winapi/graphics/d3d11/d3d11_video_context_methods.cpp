// SPDX-License-Identifier: LGPL-2.1-only

/// @file d3d11_video_context_methods.cpp
/// @brief The immediate context's ID3D11VideoContext facet.
///
/// Split out of d3d11_device_methods.cpp.

#include "core/memory/host_memory_floor.hpp"
#include "winapi/graphics/d3d11/d3d11.hpp"
#include "winapi/graphics/d3d11/d3d11_caps_log.hpp"
#include "winapi/graphics/common/gpu_event_signaler.hpp"
#include "winapi/graphics/d3d11/d3d11_internal.hpp"
#include "winapi/graphics/d3d11/d3d11_vertex_formats.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/error.hpp"
#include "graphics/contracts/backend_device.hpp"
#include "graphics/contracts/gpu_resource.hpp"
#include "graphics/shaders/dxbc_parser.hpp"
#include "core/memory/guest_memory.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <array>
#include <cstring>
#include <cstdio>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace vitreo::winapi {

void D3D11Manager::impl_VideoContextMethod(Marshaler& m, usize slot) {
    // ID3D11VideoContext (1-3), a facet of the immediate context: measured
    // on Windows (d3d11_video_formats_probe [video]), the context answers
    // every tier, is one COM object with it (QI back to
    // ID3D11DeviceContext returns the context) and GetDevice returns the
    // device -- while the device answers no ID3D11VideoDevice, with or
    // without D3D11_CREATE_DEVICE_VIDEO_SUPPORT. Every video method takes a
    // decoder, processor, crypto session or authenticated channel, which
    // only an ID3D11VideoDevice of this device creates, so no call can name
    // a valid one: the HRESULT methods fail with E_INVALIDARG and the void
    // ones do nothing (Learn: these objects come from ID3D11VideoDevice;
    // this runtime implements no video decode, processing or content
    // protection).
    const GuestAddr self = m.arg_ptr(0);
    InterfaceType type{};
    Device* device = find_device(self, &type);
    if (device == nullptr || type != InterfaceType::VideoContext) {
        m.set_return(slot == 1 || slot == 2 ? 0 : d3d11::E_INVALIDARG);
        return;
    }
    const char* name = slot < VIDEO_CONTEXT_METHOD_NAMES.size()
        ? VIDEO_CONTEXT_METHOD_NAMES[slot] : "unknown";
    d3d11_diag("ID3D11VideoContext::%s self=0x%llx", name,
               static_cast<unsigned long long>(self));
    switch (slot) {
    case 0:
        m.set_return(query_interface(m, self));
        return;
    case 1:
        m.set_return(add_ref(self));
        return;
    case 2:
        m.set_return(release(self));
        return;
    case 3: {
        // ID3D11DeviceChild::GetDevice, as the context's.
        const GuestAddr device_out = m.arg_ptr(1);
        if (device_out != 0) {
            (void)add_ref(device->device_interface);
            if (m.write_ptr(device_out, device->device_interface).is_err()) {
                (void)release(device->device_interface);
            }
        }
        m.set_return(0);
        return;
    }
    // The context's own private-data store: one object.
    case 4:
        m.set_return(get_private_data(m, device->context_private_data,
                                      device->context_interface));
        return;
    case 5:
        m.set_return(set_private_data(m, device->context_private_data,
                                      device->context_interface));
        return;
    case 6:
        m.set_return(set_private_data_interface(
            m, device->context_private_data, device->context_interface));
        return;
    default:
        break;
    }
    if (slot >= d3d11::VIDEO_CONTEXT_METHOD_COUNT) {
        unsupported(m, "ID3D11VideoContext", slot);
        return;
    }
    // The ID3D11VideoContext3 methods that return an HRESULT (the
    // ID3D11VideoContext3Vtbl return types, d3d11_4.h); the rest are void.
    bool returns_hresult = false;
    switch (slot) {
    case 7: case 8: case 9: case 10: case 11: case 12:  // decoder
    case 19: case 26: case 39: case 52:     // processor extensions
    case 53:                                // VideoProcessorBlt
    case 54: case 59: case 60: case 61: case 62:  // crypto, channel
    case 65: case 66: case 67: case 68: case 69:  // VideoContext1 decoder
    case 78:                                // VideoProcessorGetBehaviorHints
    case 83: case 84:                       // VideoContext3 decoder
        returns_hresult = true;
        break;
    default:
        break;
    }
    d3d11_unimplemented_once(
        (std::string("ID3D11VideoContext::") + name).c_str(),
        "no video decoder, processor, crypto session or authenticated "
        "channel exists (no ID3D11VideoDevice)");
    m.set_return(returns_hresult ? d3d11::E_INVALIDARG : 0u);
}

} // namespace vitreo::winapi
