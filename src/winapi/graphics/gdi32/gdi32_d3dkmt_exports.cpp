// SPDX-License-Identifier: LGPL-2.1-only

/// @file gdi32_d3dkmt_exports.cpp
/// @brief D3DKMT adapter exports.
///
/// D3DKMTOpenAdapterFromGdiDisplayName .. D3DKMTEnumAdapters: the
/// kernel-mode thunk entry points that identify the Metal adapter to the
/// guest and report no further WDDM adapters.
///
/// One of the section registrars split out of make_gdi32_exports
/// (gdi32_registration.cpp); see gdi32_registration_internal.hpp. The entries
/// are pushed in their original order.

#include "winapi/graphics/gdi32/gdi32_registration_internal.hpp"
#include "winapi/common/display_device_identity.hpp"

namespace vitreo::winapi {

void add_gdi32_d3dkmt_exports(
    DllExports& exports,
    std::function<std::string()> display_adapter_name_provider) {
    // D3DKMT adapter handles identify the real Metal adapter inside Vitreo's
    // guest-visible Windows graphics stack. Unsupported KMT queries fail with
    // STATUS_NOT_SUPPORTED instead of returning invented driver statistics.
    constexpr u32 D3DKMT_ADAPTER_HANDLE = 0x4D544C01u;
    constexpr u32 ADAPTER_LUID_LOW = 0x5654524Du;
    constexpr u32 ADAPTER_LUID_HIGH = 1u;
    constexpr u32 STATUS_SUCCESS = 0;
    constexpr u32 STATUS_BUFFER_TOO_SMALL = 0xC0000023u;
    constexpr u32 STATUS_INVALID_PARAMETER = 0xC000000Du;
    constexpr u32 STATUS_NOT_SUPPORTED = 0xC00000BBu;
    constexpr u32 KMTQAITYPE_ADAPTERADDRESS = 6;
    const auto write_adapter_identity =
        [display_adapter_name_provider](
            Marshaler& m, GuestAddr structure, GuestAddr handle_offset,
            GuestAddr luid_offset, GuestAddr source_offset) {
            if (structure == 0 || display_adapter_name_provider().empty() ||
                m.write_u32(structure + handle_offset,
                            D3DKMT_ADAPTER_HANDLE).is_err() ||
                m.write_u32(structure + luid_offset,
                            ADAPTER_LUID_LOW).is_err() ||
                m.write_u32(structure + luid_offset + 4,
                            ADAPTER_LUID_HIGH).is_err() ||
                m.write_u32(structure + source_offset, 0).is_err()) {
                m.set_return(STATUS_INVALID_PARAMETER);
                return;
            }
            VITREO_TRACE("[gdi32] D3DKMT open -> adapter=0x%x \"%s\"\n",
                         D3DKMT_ADAPTER_HANDLE,
                         display_adapter_name_provider().c_str());
            m.set_return(STATUS_SUCCESS);
        };
    exports.entries.push_back({
        "D3DKMTOpenAdapterFromGdiDisplayName", 0,
        [write_adapter_identity](Marshaler& m) {
            write_adapter_identity(m, m.arg_ptr(0), 64, 68, 76);
        }, 4});
    exports.entries.push_back({
        "D3DKMTOpenAdapterFromDeviceName", 0,
        [write_adapter_identity](Marshaler& m) {
            const GuestAddr structure = m.arg_ptr(0);
            const GuestAddr handle_offset = m.cpu_is_64bit() ? 8 : 4;
            write_adapter_identity(
                m, structure, handle_offset, handle_offset + 4,
                handle_offset + 12);
        }, 4});
    exports.entries.push_back({
        "D3DKMTOpenAdapterFromHdc", 0,
        [write_adapter_identity](Marshaler& m) {
            const GuestAddr structure = m.arg_ptr(0);
            const GuestAddr handle_offset = m.cpu_is_64bit() ? 8 : 4;
            write_adapter_identity(
                m, structure, handle_offset, handle_offset + 4,
                handle_offset + 12);
        }, 4});
    exports.entries.push_back({"D3DKMTCloseAdapter", 0,
        [](Marshaler& m) {
            const GuestAddr close = m.arg_ptr(0);
            if (close == 0) {
                m.set_return(STATUS_INVALID_PARAMETER);
                return;
            }
            const auto handle = m.memory().read_u32(close);
            m.set_return(
                handle.is_ok() &&
                handle.value() == D3DKMT_ADAPTER_HANDLE
                    ? STATUS_SUCCESS
                    : STATUS_INVALID_PARAMETER);
        }, 4});
    exports.entries.push_back({"D3DKMTQueryAdapterInfo", 0,
        [](Marshaler& m) {
            const GuestAddr query = m.arg_ptr(0);
            if (query == 0) {
                m.set_return(STATUS_INVALID_PARAMETER);
                return;
            }
            const auto adapter = m.memory().read_u32(query);
            const auto type = m.memory().read_u32(query + 4);
            const auto data = m.read_ptr(query + 8);
            const auto data_size = m.memory().read_u32(
                query + (m.cpu_is_64bit() ? 16u : 12u));
            if (adapter.is_err() || type.is_err() || data.is_err() ||
                data_size.is_err() ||
                adapter.value() != D3DKMT_ADAPTER_HANDLE) {
                m.set_return(STATUS_INVALID_PARAMETER);
                return;
            }
            if (type.value() == KMTQAITYPE_ADAPTERADDRESS) {
                constexpr u32 ADDRESS_SIZE = 12;
                if (data_size.value() < ADDRESS_SIZE) {
                    m.set_return(STATUS_BUFFER_TOO_SMALL);
                    return;
                }
                if (data.value() == 0 ||
                    m.write_u32(
                         data.value(),
                         display_device_identity::VIRTUAL_BUS_NUMBER).is_err() ||
                    m.write_u32(
                         data.value() + 4,
                         display_device_identity::VIRTUAL_DEVICE_NUMBER).is_err() ||
                    m.write_u32(
                         data.value() + 8,
                         display_device_identity::VIRTUAL_FUNCTION_NUMBER).is_err()) {
                    m.set_return(STATUS_INVALID_PARAMETER);
                    return;
                }
                VITREO_TRACE(
                    "[gdi32] D3DKMTQueryAdapterInfo adapter=0x%x "
                    "type=ADAPTERADDRESS -> virtual-root 0:0:0\n",
                    adapter.value());
                m.set_return(STATUS_SUCCESS);
                return;
            }
            VITREO_TRACE(
                "[gdi32] D3DKMTQueryAdapterInfo adapter=0x%x "
                "type=%u data_size=%u -> STATUS_NOT_SUPPORTED\n",
                adapter.value(), type.value(), data_size.value());
            m.set_return(STATUS_NOT_SUPPORTED);
        }, 4});
    exports.entries.push_back({"D3DKMTQueryStatistics", 0,
        [](Marshaler& m) {
            const GuestAddr query = m.arg_ptr(0);
            const auto type = m.memory().read_u32(query);
            const auto adapter = m.memory().read_u32(query + 4);
            VITREO_TRACE(
                "[gdi32] D3DKMTQueryStatistics type=%u adapter=0x%x "
                "-> STATUS_NOT_SUPPORTED\n",
                type.is_ok() ? type.value() : 0u,
                adapter.is_ok() ? adapter.value() : 0u);
            m.set_return(STATUS_NOT_SUPPORTED);
        }, 4});
    // D3DKMTEnumAdapters2(D3DKMT_ENUMADAPTERS2*): report zero WDDM adapters.
    // The Vulkan loader probes this before falling back to ICD enumeration
    // (VK_ICD_FILENAMES / Khronos registry). Returning SUCCESS with
    // NumAdapters=0 is honest on a software-only path and avoids the loader
    // treating a missing export as a crashy function pointer.
    exports.entries.push_back({"D3DKMTEnumAdapters2", 0,
        [](Marshaler& m) {
            const GuestAddr info = m.arg_ptr(0);
            if (info == 0) {
                m.set_return(STATUS_INVALID_PARAMETER);
                return;
            }
            // struct { ULONG NumAdapters; D3DKMT_ADAPTERINFO* pAdapters; }
            if (m.write_u32(info, 0).is_err()) {
                m.set_return(STATUS_INVALID_PARAMETER);
                return;
            }
            VITREO_TRACE("[gdi32] D3DKMTEnumAdapters2 -> 0 adapters\n");
            m.set_return(STATUS_SUCCESS);
        }, 4});
    exports.entries.push_back({"D3DKMTEnumAdapters", 0,
        [](Marshaler& m) {
            const GuestAddr info = m.arg_ptr(0);
            if (info == 0) {
                m.set_return(STATUS_INVALID_PARAMETER);
                return;
            }
            // D3DKMT_ENUMADAPTERS: ULONG NumAdapters; D3DKMT_ADAPTERINFO Adapters[16]
            if (m.write_u32(info, 0).is_err()) {
                m.set_return(STATUS_INVALID_PARAMETER);
                return;
            }
            VITREO_TRACE("[gdi32] D3DKMTEnumAdapters -> 0 adapters\n");
            m.set_return(STATUS_SUCCESS);
        }, 4});
}

}  // namespace vitreo::winapi
