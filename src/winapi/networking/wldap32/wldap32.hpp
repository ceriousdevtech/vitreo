// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file wldap32.hpp
/// @brief Wldap32.dll: the synchronous LDAP client (init, options, simple
///        bind, search, and walking the entries/attributes/values of the
///        result), backed by the host's OpenLDAP libldap.
///
/// Contract sources: Microsoft Learn winldap.h pages and "Session Options";
/// export ordinals, struct layouts, error strings and no-server results from
/// tools/oracle_probe/wldap32_contract_probe.{x86,x64}.expected.

#include <vitreo/common.hpp>
#include "winapi/core/thunk_registry.hpp"

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace vitreo::winapi {

class Marshaler;
class HeapManager;

class Wldap32Manager {
public:
    Wldap32Manager() = default;
    ~Wldap32Manager();

    Wldap32Manager(const Wldap32Manager&) = delete;
    Wldap32Manager& operator=(const Wldap32Manager&) = delete;

    /// Every buffer handed to the guest (the LDAP and LDAPMessage blocks,
    /// DN and attribute strings, berval arrays) lives on the process heap.
    void set_heap_manager(HeapManager* heap) noexcept { m_heap = heap; }

    struct Session;
    struct Chain;

    // Exposed for DllExports; `wide` selects the W string flavour.
    void init(Marshaler& m, bool wide, bool ssl_entry);
    void unbind(Marshaler& m);
    void set_option(Marshaler& m, bool wide);
    void get_option(Marshaler& m, bool wide);
    void simple_bind_s(Marshaler& m, bool wide);
    void search_s(Marshaler& m, bool wide);
    void msgfree(Marshaler& m);
    void first_entry(Marshaler& m);
    void next_entry(Marshaler& m);
    void get_dn(Marshaler& m, bool wide);
    void first_attribute(Marshaler& m, bool wide);
    void next_attribute(Marshaler& m, bool wide);
    void get_values_len(Marshaler& m, bool wide);
    void value_free_len(Marshaler& m);
    void memfree(Marshaler& m);
    void free_ber(Marshaler& m);
    void err2string(Marshaler& m, bool wide);

private:
    [[nodiscard]] GuestAddr alloc(u64 bytes);
    void free_block(GuestAddr addr);
    [[nodiscard]] GuestAddr alloc_string(Marshaler& m, const std::string& s, bool wide);
    [[nodiscard]] std::shared_ptr<Session> find_session(GuestAddr ld);
    void set_session_error(Marshaler& m, Session& s, u32 code);
    [[nodiscard]] u32 connect(Session& s);
    void finish_operation(Session& s, u32 rc);
    [[nodiscard]] GuestAddr guest_message_for(void* host_msg);
    [[nodiscard]] void* host_message(GuestAddr msg, GuestAddr* ld_out = nullptr);

    HeapManager* m_heap = nullptr;
    std::mutex m_mu;
    std::unordered_map<GuestAddr, std::shared_ptr<Session>> m_sessions;
    /// Guest LDAPMessage block -> owning chain; every message of a chain maps.
    std::unordered_map<GuestAddr, std::shared_ptr<Chain>> m_messages;
    /// Guest BerElement token -> host BerElement*.
    std::unordered_map<GuestAddr, void*> m_bers;
    /// Strings and berval arrays freed by ldap_memfree / ldap_value_free_len.
    std::unordered_set<GuestAddr> m_buffers;
    /// ldap_err2string tables (A, W): one static string per code, allocated
    /// once so repeated calls return the same pointer.
    std::unordered_map<u32, GuestAddr> m_err_strings[2];
};

/// Wldap32.dll exports at their Windows ordinals.
DllExports create_wldap32_thunks(Wldap32Manager& mgr);

} // namespace vitreo::winapi
