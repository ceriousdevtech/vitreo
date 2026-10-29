/// @file dinput8.cpp
/// @brief DINPUT8.DLL entry-point contracts.

#include "winapi/media/dinput8/dinput8.hpp"

#include "winapi/core/marshaler.hpp"

namespace vitreo::winapi {
namespace {

constexpr u32 DIERR_INVALIDPARAM = 0x80070057u; // E_INVALIDARG

} // namespace

DllExports create_dinput8_thunks(DirectInputManager& manager) {
    DllExports exports;
    exports.entries.push_back(ExportEntry{
        "DirectInput8Create", 0,
        [&manager](Marshaler& m) {
            const GuestAddr instance = m.arg_ptr(0);
            const u32 version = static_cast<u32>(m.arg(1));
            const GuestAddr iid = m.arg_ptr(2);
            const GuestAddr output = m.arg_ptr(3);
            const GuestAddr outer = m.arg_ptr(4);

            if (output == 0) {
                VITREO_TRACE(
                    "[dinput8] DirectInput8Create instance=0x%llx "
                    "version=0x%x riid=0x%llx output=NULL outer=0x%llx -> "
                    "DIERR_INVALIDPARAM\n",
                    static_cast<unsigned long long>(instance), version,
                    static_cast<unsigned long long>(iid),
                    static_cast<unsigned long long>(outer));
                m.set_return(DIERR_INVALIDPARAM);
                return;
            }

            const u32 hr = manager.direct_input8_create(
                instance, version, iid, output, outer, m.cpu_is_64bit());
            VITREO_TRACE(
                "[dinput8] DirectInput8Create instance=0x%llx version=0x%x "
                "riid=0x%llx output=0x%llx outer=0x%llx -> 0x%08x\n",
                static_cast<unsigned long long>(instance), version,
                static_cast<unsigned long long>(iid),
                static_cast<unsigned long long>(output),
                static_cast<unsigned long long>(outer), hr);
            m.set_return(hr);
        },
        20,
    });
    return exports;
}

} // namespace vitreo::winapi
