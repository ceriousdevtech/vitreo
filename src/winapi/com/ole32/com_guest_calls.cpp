// SPDX-License-Identifier: LGPL-2.1-only
/// @file com_guest_calls.cpp
/// @brief Host-side helpers that call back into guest code: QueryInterface/AddRef on
/// a guest object, oleaut32 and arbitrary guest exports, and plain guest
/// functions.

#include "com_core.hpp"
#include "winapi/core/marshaler.hpp"
#include "com_core_internal.hpp"

#include <vector>

namespace vitreo::winapi {

u32 ComCoreManager::guest_query_interface(Marshaler& m, GuestAddr punk,
                                          GuestAddr iid, GuestAddr out) {
    if (punk == 0 || !m_guest_call) return E_NOINTERFACE;
    const GuestAddr vtbl = static_cast<GuestAddr>(m.read_ptr(punk).value_or(0));
    const GuestAddr qi = vtbl == 0 ? 0 : static_cast<GuestAddr>(m.read_ptr(vtbl).value_or(0));
    if (qi == 0) return E_NOINTERFACE;
    const u64 args[3] = {punk, iid, out};
    return static_cast<u32>(m_guest_call(qi, args, 3, m.cpu_is_64bit()));
}

void ComCoreManager::guest_iunknown_add_ref(Marshaler& m, GuestAddr punk) {
    if (punk == 0 || !m_guest_call) return;
    const GuestAddr ptr = m.cpu_is_64bit() ? 8u : 4u;
    const GuestAddr vtbl = static_cast<GuestAddr>(m.read_ptr(punk).value_or(0));
    const GuestAddr fn = vtbl == 0 ? 0 : static_cast<GuestAddr>(m.read_ptr(vtbl + ptr).value_or(0));
    if (fn == 0) return;
    const u64 args[1] = {punk};
    (void)m_guest_call(fn, args, 1, m.cpu_is_64bit());
}

u64 ComCoreManager::call_oleaut32(Marshaler& m, const char* name,
                                  std::initializer_list<u64> args) {
    if (m_thunks == nullptr || !m_guest_call) return kEOutOfMemory;
    auto fn = m_thunks->get_thunk_address("oleaut32.dll", name);
    if (fn.is_err()) return kEOutOfMemory;
    std::vector<u64> values(args);
    return m_guest_call(fn.value(), values.data(), values.size(), m.cpu_is_64bit());
}

u64 ComCoreManager::call_guest_export(Marshaler& m, const char* dll, const char* name,
                                      std::initializer_list<u64> args) {
    if (m_thunks == nullptr || !m_guest_call) return 0;
    auto fn = m_thunks->get_thunk_address(dll, name);
    if (fn.is_err()) return 0;
    std::vector<u64> values(args);
    return m_guest_call(fn.value(), values.data(), values.size(), m.cpu_is_64bit());
}

u64 ComCoreManager::call_guest_function(Marshaler& m, GuestAddr fn,
                                        std::initializer_list<u64> args) {
    if (fn == 0 || !m_guest_call) return 0x8000FFFFu;
    std::vector<u64> values(args);
    return m_guest_call(fn, values.data(), values.size(), m.cpu_is_64bit());
}

} // namespace vitreo::winapi
