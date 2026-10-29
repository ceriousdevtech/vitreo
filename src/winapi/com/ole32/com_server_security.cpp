// SPDX-License-Identifier: LGPL-2.1-only
/// @file com_server_security.cpp
/// @brief Server-call context and IServerSecurity (CoGetCallContext).

#include "com_core.hpp"
#include "ole_guid_internal.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/com/ole32/task_memory.hpp"
#include "com_core_internal.hpp"

#include <array>

namespace vitreo::winapi {

namespace {

constexpr Guid IID_ISERVERSECURITY{0x0000013Eu, 0x0000u, 0x0000u, {0xC0,0x00,0x00,0x00,0x00,0x00,0x00,0x46}};

} // namespace

thread_local ComCoreManager::ServerCallContext
    ComCoreManager::s_server_call_context{};

ComCoreManager::ServerCallContext ComCoreManager::push_server_call_context(
    bool caller_elevated) noexcept {
    const ServerCallContext previous = s_server_call_context;
    s_server_call_context = {true, caller_elevated, false};
    return previous;
}

void ComCoreManager::pop_server_call_context(
    ServerCallContext previous) noexcept {
    if (s_server_call_context.impersonating && m_caller_revert) {
        m_caller_revert();
    }
    s_server_call_context = previous;
}

GuestAddr ComCoreManager::create_server_security(Marshaler& m) {
    if (m_guest_task_memory == nullptr || m_thunks == nullptr) return 0;
    constexpr std::array<const char*, 7> names{
        "Vitreo_ServerSecurity_QueryInterface",
        "Vitreo_ServerSecurity_AddRef",
        "Vitreo_ServerSecurity_Release",
        "Vitreo_ServerSecurity_QueryBlanket",
        "Vitreo_ServerSecurity_ImpersonateClient",
        "Vitreo_ServerSecurity_RevertToSelf",
        "Vitreo_ServerSecurity_IsImpersonating",
    };
    std::array<GuestAddr, names.size()> methods{};
    for (usize index = 0; index < names.size(); ++index) {
        auto thunk = m_thunks->get_thunk_address("ole32.dll", names[index]);
        if (thunk.is_err()) return 0;
        methods[index] = thunk.value();
    }
    const GuestAddr pointer_size = m.cpu_is_64bit() ? 8u : 4u;
    const GuestAddr allocation = m_guest_task_memory->allocate(
        pointer_size * (1u + methods.size()));
    if (allocation == 0) return 0;
    const GuestAddr vtable = allocation + pointer_size;
    if (m.write_ptr(allocation, vtable).is_err()) {
        m_guest_task_memory->free(allocation);
        return 0;
    }
    for (usize index = 0; index < methods.size(); ++index) {
        if (m.write_ptr(vtable + index * pointer_size,
                        methods[index]).is_err()) {
            m_guest_task_memory->free(allocation);
            return 0;
        }
    }
    std::lock_guard lock(m_server_security_mutex);
    m_server_security_refs.emplace(allocation, 1u);
    return allocation;
}

u32 ComCoreManager::co_get_call_context(Marshaler& m) {
    const GuestAddr iid_address = m.arg_ptr(0);
    const GuestAddr output = m.arg_ptr(1);
    if (output == 0) return com::E_POINTER;
    if (m.write_ptr(output, 0).is_err()) return com::E_INVALIDARG;
    if (iid_address == 0) return com::E_INVALIDARG;
    auto iid = m.read_struct<Guid>(iid_address);
    if (iid.is_err()) return com::E_INVALIDARG;
    if (!s_server_call_context.active ||
        !guid_equal(iid.value(), IID_ISERVERSECURITY)) {
        VITREO_TRACE(
            "[ole32] CoGetCallContext IID={%08X-%04X-%04X-...} "
            "-> E_NOINTERFACE\n",
            iid.value().Data1, iid.value().Data2, iid.value().Data3);
        return com::E_NOINTERFACE;
    }
    const GuestAddr object = create_server_security(m);
    if (object == 0 || m.write_ptr(output, object).is_err()) {
        if (object != 0) co_task_mem_free_guest(object);
        return E_OUTOFMEMORY;
    }
    VITREO_TRACE(
        "[ole32] CoGetCallContext(IID_IServerSecurity) -> 0x%llx "
        "caller_elevated=%u\n",
        static_cast<unsigned long long>(object),
        s_server_call_context.caller_elevated ? 1u : 0u);
    return com::S_OK;
}

u32 ComCoreManager::server_security_query_interface(Marshaler& m) {
    const GuestAddr object = m.arg_ptr(0);
    const GuestAddr iid_address = m.arg_ptr(1);
    const GuestAddr output = m.arg_ptr(2);
    if (output == 0) return com::E_POINTER;
    if (m.write_ptr(output, 0).is_err() || iid_address == 0) {
        return com::E_INVALIDARG;
    }
    auto iid = m.read_struct<Guid>(iid_address);
    if (iid.is_err() ||
        (!guid_equal(iid.value(), IID_IUNKNOWN) &&
         !guid_equal(iid.value(), IID_ISERVERSECURITY))) {
        return com::E_NOINTERFACE;
    }
    if (m.write_ptr(output, object).is_err()) return com::E_INVALIDARG;
    (void)server_security_add_ref(m);
    return com::S_OK;
}

u32 ComCoreManager::server_security_add_ref(Marshaler& m) {
    std::lock_guard lock(m_server_security_mutex);
    auto found = m_server_security_refs.find(m.arg_ptr(0));
    if (found == m_server_security_refs.end()) return 0;
    return ++found->second;
}

u32 ComCoreManager::server_security_release(Marshaler& m) {
    const GuestAddr object = m.arg_ptr(0);
    u32 refs = 0;
    {
        std::lock_guard lock(m_server_security_mutex);
        auto found = m_server_security_refs.find(object);
        if (found == m_server_security_refs.end()) return 0;
        refs = --found->second;
        if (refs == 0) m_server_security_refs.erase(found);
    }
    if (refs == 0) co_task_mem_free_guest(object);
    return refs;
}

u32 ComCoreManager::server_security_query_blanket(Marshaler& m) {
    if (m.arg_ptr(1)) (void)m.write_u32(m.arg_ptr(1), 10u);
    if (m.arg_ptr(2)) (void)m.write_u32(m.arg_ptr(2), 0u);
    if (m.arg_ptr(3)) (void)m.write_ptr(m.arg_ptr(3), 0);
    if (m.arg_ptr(4)) (void)m.write_u32(m.arg_ptr(4), 0u);
    if (m.arg_ptr(5)) (void)m.write_u32(m.arg_ptr(5), 3u);
    if (m.arg_ptr(6)) (void)m.write_ptr(m.arg_ptr(6), 0);
    if (m.arg_ptr(7)) (void)m.write_u32(m.arg_ptr(7), 0u);
    return com::S_OK;
}

u32 ComCoreManager::server_security_impersonate_client(Marshaler& /*m*/) {
    if (!s_server_call_context.active || !m_caller_impersonation ||
        !m_caller_impersonation(s_server_call_context.caller_elevated)) {
        return E_FAIL;
    }
    s_server_call_context.impersonating = true;
    return com::S_OK;
}

u32 ComCoreManager::server_security_revert_to_self(Marshaler& /*m*/) {
    if (s_server_call_context.impersonating && m_caller_revert) {
        m_caller_revert();
    }
    s_server_call_context.impersonating = false;
    return com::S_OK;
}

u32 ComCoreManager::server_security_is_impersonating(Marshaler& /*m*/) {
    return s_server_call_context.impersonating ? 1u : 0u;
}

} // namespace vitreo::winapi
