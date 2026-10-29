// SPDX-License-Identifier: LGPL-2.1-only

/// @file d3d10_device.cpp
/// @brief ID3D10Device1 and the D3D10 facets of D3D11Manager's objects.
///
/// The contract, measured on Windows (tools/oracle_probe/
/// d3d10_contract_probe, x86 and x64) and read from Learn:
///
///  - D3D10CreateDevice(1) creates a D3D11 device whose immediate context
///    runs a D3D10 context state. The ID3D10Device1 pointer is one facet of
///    that device: it answers the device's whole QueryInterface graph
///    (ID3D11Device..5, IDXGIDevice..4, ID3D10Multithread) and shares its
///    count.
///  - Every object a D3D10 or D3D11 device makes answers the D3D10
///    interfaces of its kind through one facet (a D3D11 texture answers
///    ID3D10Texture2D; a D3D10 buffer answers ID3D11Buffer), one COM
///    identity, one count, one private-data store.
///  - The D3D10 methods are the D3D11 ones with D3D10 structures: a
///    D3D10_BUFFER_DESC has no stride, the misc flags are renumbered, a DSV
///    description has no Flags, a D3D10_BLEND_DESC has per-target enables and
///    masks over one blend function, viewports are integers, Map lives on the
///    resource, and a D3D10 PIPELINE_STATISTICS query returns 64 bytes.
///  - While a D3D11 state is active on a device's immediate context, the
///    D3D10 context methods are off (setters do nothing, getters report an
///    empty state) and vice versa (Learn, CreateDeviceContextState remarks).
///
/// Every D3D10 method below runs the D3D11 implementation through
/// d3d10_forward: the call the D3D11 thunk would have received, `this` the
/// D3D11 object, each D3D10 interface pointer translated to its object and
/// each D3D10-only structure converted, in this thread's scratch arena.
///
/// The implementation is split by role, as verbatim code motion out of
/// this one file; d3d10_device_internal.hpp holds the file-local constants
/// and the misc-flag conversions they share:
///
///  - d3d10_device.cpp: the ID3D10Device1 dispatcher
///    (impl_D3D10DeviceMethod) and the well-known counter table.
///  - d3d10_create_device.cpp: D3D10CreateDevice(1) and
///    D3D10CreateDeviceAndSwapChain(1).
///  - d3d10_device_create_methods.cpp: the ID3D10Device Create* methods
///    (d3d10_create_method).
///  - d3d10_forwarding.cpp: the facet lookups, d3d10_forward and its
///    scratch arena, the immediate-context methods (d3d10_context_method),
///    d3d10_child_method, and the buffer and texture facets with Map.
///  - d3d10_facets.cpp: the vtable specifications, the QueryInterface graph
///    (d3d10_interface_for) and the view, state, device-child and query
///    facets.
///  - d3d10_blob.cpp: ID3D10Blob, D3D10Get*SignatureBlob and
///    D3D10Get*ShaderProfile.
///  - d3d10_state_block.cpp: ID3D10StateBlock.

#include "winapi/graphics/d3d10/d3d10.hpp"
#include "winapi/graphics/d3d11/d3d11.hpp"
#include "winapi/graphics/d3d11/d3d11_internal.hpp"
#include "winapi/graphics/d3d10/d3d10_device_internal.hpp"
#include "graphics/shaders/dxbc_parser.hpp"
#include "winapi/core/marshaler.hpp"
#include "core/memory/guest_memory.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

namespace vitreo::winapi {
namespace {

/// ID3D10Device vtable slots that are immediate-context methods, and so are
/// off while a D3D11 state is active (Learn's table: every Set/Get, draw,
/// copy, clear, ClearState, Flush, GenerateMips, predication, SO and the
/// text-filter pair).
[[nodiscard]] constexpr bool d3d10_context_slot(usize slot) {
    return (slot >= 3 && slot <= 62) || slot == 69 || slot == 70 ||
           slot == 96 || slot == 97;
}

/// D3D10_COUNTER_GPU_IDLE .. D3D10_COUNTER_TEXTURE_CACHE_HIT_RATE: the
/// well-known counters. CheckCounter answers each with its name, units and
/// description (FLOAT32, no active-counter cost); CreateCounter refuses them
/// with DXGI_ERROR_UNSUPPORTED. The strings are Windows' own, typos
/// included, read back through CheckCounter on Windows
/// (d3d10_contract_probe [fmt]).
struct WellKnownCounter {
    const char* name;
    const char* description;
};
constexpr const char* kCounterUnits = "fraction";
constexpr std::array<WellKnownCounter, 18> kWellKnownCounters{{
    {"GPU Idle",
     "A well-known counter to estimate the fractional amount of time for "
     "which the GPU was idle."},
    {"Vertex Processing",
     "A well-known counter to estimate the fractional amount of time for "
     "which the GPU was workingon vertex processing."},
    {"Geometry Processing",
     "A well-known counter to estimate the fractional amount of time for "
     "which the GPU was workingon geometry processing."},
    {"Pixel Processing",
     "A well-known counter to estimate the fractional amount of time for "
     "which the GPU was workingon pixel processing."},
    {"Other GPU Processing",
     "A well-known counter to estimate the fractional amount of time for "
     "which the GPU was workingon other processing, not directly related to "
     "vertex, geometry, or pixel processing."},
    {"Host Adapter Bandwidth Utilization",
     "A well-known counter to estimate the fractional amount of host adapter "
     "bandwidth utilized."},
    {"Local Video Memory Bandwidth Utilization",
     "A well-known counter to estimate the fractional amount of local video "
     "memory bandwidth utilized."},
    {"Vertex Throughput Utilization",
     "A well-known counter to estimate the fractional amount of vertex "
     "throughput utilized."},
    {"Triangle Setup Throughput Utilization",
     "A well-known counter to estimate the fractional amount of triangle "
     "setup throughput utilized."},
    {"Fill Rate Utilization",
     "A well-known counter to estimate the fractional amount of fill rate "
     "throughput utilized."},
    {"Vertex Shader Memory Limited",
     "A well-known counter to estimate the fractional amount of time the "
     "vertex shader was working onmemory accesses."},
    {"Vertex Shader Computation Limited",
     "A well-known counter to estimate the fractional amount of time the "
     "vertex shader was working oncomputation."},
    {"Geometry Shader Memory Limited",
     "A well-known counter to estimate the fractional amount of time the "
     "geometry shader was working onmemory accesses."},
    {"Geometry Shader Computation Limited",
     "A well-known counter to estimate the fractional amount of time the "
     "geometry shader was working oncomputation."},
    {"Pixel Shader Memory Limited",
     "A well-known counter to estimate the fractional amount of time the "
     "pixel shader was working onmemory accesses."},
    {"Pixel Shader Computation Limited",
     "A well-known counter to estimate the fractional amount of time the "
     "pixel shader was working oncomputation."},
    {"Post Vertex-Shader Vertex Cache Hit Rate",
     "A well-known counter to estimate the fractional amount of post "
     "vertex-shader vertex requests that weresatisfied by a cache, instead of "
     "running the vertex shader again."},
    {"Texture Cache Hit Rate",
     "A well-known counter to estimate the fractional amount of texture "
     "memory requests that weresatisfied by a cache, instead of going "
     "further out to video memory."},
}};
/// D3D10_COUNTER_DEVICE_DEPENDENT_0.
constexpr u32 kCounterDeviceDependent0 = 0x40000000u;

} // namespace

// -----------------------------------------------------------------------------
// ID3D10Device1
// -----------------------------------------------------------------------------

void D3D11Manager::impl_D3D10DeviceMethod(Marshaler& m, usize slot) {
    const GuestAddr self = m.arg_ptr(0);
    InterfaceType type{};
    Device* device = find_device(self, &type);
    if (device == nullptr || type != InterfaceType::D3D10Device) {
        m.set_return(slot == 1 || slot == 2 ? 0 : kEInvalidArg);
        return;
    }
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
    default:
        break;
    }
    if (d3d10_context_slot(slot)) {
        (void)d3d10_context_method(m, slot, *device);
        return;
    }
    const GuestAddr d3d11_device = device->device_interface;
    const auto device_call = [&](usize d3d11_slot,
                                 std::initializer_list<u64> args) {
        return d3d10_forward(&D3D11Manager::impl_DeviceMethod, d3d11_slot,
                             d3d11_device, args);
    };
    switch (slot) {
    case 63:  // GetDeviceRemovedReason
        m.set_return(device_call(39, {}));
        return;
    case 64:  // SetExceptionMode(RaiseFlags)
        m.set_return(device_call(41, {m.arg(1)}));
        return;
    case 65:  // GetExceptionMode
        m.set_return(device_call(42, {}));
        return;
    // Private data: one store with the ID3D11Device and the IDXGIDevice
    // (measured).
    case 66:
        m.set_return(device_call(34, {m.arg(1), m.arg(2), m.arg(3)}));
        return;
    case 67:
        m.set_return(device_call(35, {m.arg(1), m.arg(2), m.arg(3)}));
        return;
    case 68:
        m.set_return(device_call(36, {m.arg(1), m.arg(2)}));
        return;
    case 89: {
        // CreateCounter(pCounterDesc, ppCounter). The well-known counters
        // exist in the D3D10 API but no device measures them
        // (DXGI_ERROR_UNSUPPORTED); there are no device-dependent ones
        // (CheckCounterInfo) -- E_INVALIDARG. The out-parameter is left
        // untouched either way (measured).
        const GuestAddr desc = m.arg_ptr(1);
        const auto counter =
            desc != 0 ? m_memory.read_u32(desc) : VResult<u32>::ok(~0u);
        m.set_return(counter.is_ok() &&
                             counter.value() < kWellKnownCounters.size()
                         ? kDxgiErrorUnsupported
                         : kEInvalidArg);
        return;
    }
    case 90:  // CheckFormatSupport(Format, pFormatSupport)
        m.set_return(device_call(29, {m.arg(1), m.arg(2)}));
        return;
    case 91:  // CheckMultisampleQualityLevels(Format, SampleCount, pLevels)
        m.set_return(device_call(30, {m.arg(1), m.arg(2), m.arg(3)}));
        return;
    case 92:  // CheckCounterInfo(pCounterInfo): {0, 0, 1}, as D3D11's.
        m.set_return(device_call(31, {m.arg(1)}));
        return;
    case 93: {
        // CheckCounter(pDesc, pType, pActiveCounters, szName, pNameLength,
        // szUnits, pUnitsLength, szDescription, pDescriptionLength).
        const GuestAddr desc = m.arg_ptr(1);
        const auto counter =
            desc != 0 ? m_memory.read_u32(desc) : VResult<u32>::ok(~0u);
        if (counter.is_ok() && counter.value() < kWellKnownCounters.size()) {
            const WellKnownCounter& known =
                kWellKnownCounters[counter.value()];
            if (m.arg_ptr(2) != 0) (void)m_memory.write_u32(m.arg_ptr(2), 0);
            if (m.arg_ptr(3) != 0) (void)m_memory.write_u32(m.arg_ptr(3), 0);
            // Each string: with a buffer, as much as fits (NUL-terminated);
            // the length always becomes the full length, NUL included
            // (measured: a 4-byte name buffer gets "GPU\0", length 9).
            const auto string_out = [&](GuestAddr text, GuestAddr length,
                                        const char* value) {
                if (length == 0) return;
                const u32 full = static_cast<u32>(std::strlen(value)) + 1;
                const auto capacity = m_memory.read_u32(length);
                if (text != 0 && capacity.is_ok() && capacity.value() > 0) {
                    const u32 copy = std::min(capacity.value(), full);
                    std::vector<u8> bytes(value, value + copy);
                    bytes[copy - 1] = 0;
                    (void)m_memory.write_bytes(text, bytes.data(), copy);
                }
                (void)m_memory.write_u32(length, full);
            };
            string_out(m.arg_ptr(4), m.arg_ptr(5), known.name);
            string_out(m.arg_ptr(6), m.arg_ptr(7), kCounterUnits);
            string_out(m.arg_ptr(8), m.arg_ptr(9), known.description);
            m.set_return(kSOk);
            return;
        }
        if (counter.is_ok() && counter.value() < kCounterDeviceDependent0) {
            // An undefined counter: E_INVALIDARG after emptying each string
            // buffer given (measured: first byte 0, lengths untouched).
            for (const usize text : {4u, 6u, 8u}) {
                const GuestAddr buffer = m.arg_ptr(text);
                const auto length = m.arg_ptr(text + 1) != 0
                                        ? m_memory.read_u32(m.arg_ptr(text + 1))
                                        : VResult<u32>::ok(0);
                if (buffer != 0 && length.is_ok() && length.value() > 0) {
                    (void)m_memory.write_u8(buffer, 0);
                }
            }
            m.set_return(kEInvalidArg);
            return;
        }
        // Device-dependent counters: D3D11's answer (none exist).
        m.set_return(device_call(
            32, {m.arg(1), m.arg(2), m.arg(3), m.arg(4), m.arg(5), m.arg(6),
                 m.arg(7), m.arg(8), m.arg(9)}));
        return;
    }
    case 94:  // GetCreationFlags
        m.set_return(device->creation_flags);
        return;
    case 95: {
        // OpenSharedResource(hResource, ReturnedInterface, ppResource): the
        // D3D11 open answers the D3D10 IIDs through the facet
        // (texture_interface_for).
        m.set_return(device_call(28, {m.arg(1), m.arg(2), m.arg(3)}));
        return;
    }
    case 100:  // GetFeatureLevel: the active state's level.
        m.set_return(device->feature_level);
        return;
    default:
        d3d10_create_method(m, slot, *device);
        return;
    }
}

} // namespace vitreo::winapi
