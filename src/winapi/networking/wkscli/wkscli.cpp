// SPDX-License-Identifier: LGPL-2.1-only
/// @file wkscli.cpp
/// @brief wkscli.dll / netapi32 join and DS role APIs

#include "wkscli.hpp"
#include "winapi/advapi32/security/security.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/kernel32/memory/heap.hpp"

#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

namespace vitreo::winapi {

namespace {

constexpr u32 kErrorInvalidParameter = 87;
constexpr u32 kErrorNotEnoughMemory  = 8;
constexpr u32 kErrorInvalidLevel     = 124;
constexpr u32 kNerrSuccess           = 0;
constexpr u32 kNerrUserNotFound      = 2221;
constexpr u32 kNerrBadUsername       = 2202;
constexpr u32 kErrorNotSupported     = 50;
// Windows' RPC-backed join/DS role calls leave ERROR_IO_PENDING as the last
// error on every path (tools/oracle_probe/iphlp_netapi_probe join/dsrole le=0x3e5).
constexpr u32 kErrorIoPending        = 997;
constexpr u32 kDsRoleUpgradeStatus   = 2;
constexpr u32 kDsRoleOperationState  = 3;
constexpr u32 kNetSetupWorkgroupName = 2;
constexpr u32 kDsRolePrimaryDomainInfoBasic = 1;
constexpr u32 kDsRoleStandaloneWorkstation  = 0;
constexpr u32 kSOk = 0;
constexpr u32 kUserPrivUser = 1;
constexpr u32 kUfNormalAccount = 0x0200;
constexpr u32 kUfScript = 0x0001;
constexpr u32 kUfDontExpirePasswd = 0x10000;

// Capsule local account (matches TEB/username used elsewhere).
static constexpr char16_t kLocalUser[] = u"VitUser";
constexpr usize kLocalUserBytes = sizeof(kLocalUser);

[[nodiscard]] GuestAddr heap_alloc_bytes(HeapManager* heap, usize n) {
    if (!heap || n == 0) {
        return 0;
    }
    return static_cast<GuestAddr>(
        heap->heap_alloc(heap->get_process_heap(), 0, n));
}

void heap_free_ptr(HeapManager* heap, GuestAddr p) {
    if (!heap || p == 0) {
        return;
    }
    (void)heap->heap_free(heap->get_process_heap(), 0, p);
}

[[nodiscard]] usize ds_role_basic_fixed_size(bool is_64bit) {
    // MachineRole(4) + Flags(4) + 3 pointers + GUID(16)
    return 8 + (is_64bit ? 24 : 12) + 16;
}

} // namespace

// =============================================================================
// WksCliManager Implementation
// =============================================================================

void WksCliManager::net_get_join_information(Marshaler& m) {
    // NET_API_STATUS NetGetJoinInformation(
    //   LPCWSTR lpServer, LPWSTR *lpNameBuffer, PNETSETUP_JOIN_STATUS BufferType)
    // https://learn.microsoft.com/en-us/windows/win32/api/lmjoin/nf-lmjoin-netgetjoininformation
    //
    // A capsule is a workgroup member: NetSetupWorkgroupName with the
    // workgroup name in a buffer the caller frees with NetApiBufferFree
    // (iphlp_netapi_probe join: status=2 name="WORKGROUP").
    const GuestAddr server = m.arg_ptr(0);
    const GuestAddr name_buf_ptr = m.arg_ptr(1);
    const GuestAddr join_status = m.arg_ptr(2);
    if (name_buf_ptr == 0 || join_status == 0) {
        m.set_return(kErrorInvalidParameter);  // join-null-name / join-null-status
        return;
    }
    if (server != 0 && m.read_u16(server).value_or(0) != 0) {
        static std::once_flag once;
        std::call_once(once, [] {
            std::fprintf(stderr, "[vitreo] UNIMPLEMENTED netapi32!NetGetJoinInformation: remote lpServer\n");
        });
        m.set_return(kErrorNotSupported);
        return;
    }
    static constexpr char16_t kWorkgroup[] = u"WORKGROUP";
    const GuestAddr name = heap_alloc_bytes(m_heap, sizeof(kWorkgroup));
    if (name == 0 || m.memory().write_bytes(name, reinterpret_cast<const u8*>(kWorkgroup), sizeof(kWorkgroup)).is_err()) {
        heap_free_ptr(m_heap, name);
        m.set_return(kErrorNotEnoughMemory);
        return;
    }
    (void)m.write_ptr(name_buf_ptr, name);
    (void)m.write_u32(join_status, kNetSetupWorkgroupName);
    set_last_error(kErrorIoPending);
    m.set_return(kNerrSuccess);
}

void WksCliManager::ds_role_get_primary_domain_information(Marshaler& m) {
    // DWORD DsRoleGetPrimaryDomainInformation(
    //   LPCWSTR lpServer, DSROLE_PRIMARY_DOMAIN_INFO_LEVEL InfoLevel, PBYTE *Buffer)
    // https://learn.microsoft.com/en-us/windows/win32/api/dsrole/nf-dsrole-dsrolegetprimarydomaininformation
    (void)m.arg_ptr(0); // lpServer
    const u32 info_level = static_cast<u32>(m.arg(1));
    GuestAddr buffer_out = m.arg_ptr(2);
    const bool is_64 = m.cpu_is_64bit();
    const usize fixed = ds_role_basic_fixed_size(is_64);

    if (buffer_out == 0) {
        m.set_return(kErrorInvalidParameter);  // dsrole-null-buffer
        return;
    }
    set_last_error(kErrorIoPending);  // every measured level, success or not

    if (info_level == kDsRoleUpgradeStatus || info_level == kDsRoleOperationState) {
        // DSROLE_UPGRADE_STATUS_INFO {OperationState, PreviousServerState} and
        // DSROLE_OPERATION_STATE_INFO {OperationState}: no upgrade and no role
        // change in progress (DsRoleServerUnknown / DsRoleOperationIdle, both 0;
        // iphlp_netapi_probe dsrole level=2/3).
        const usize size = info_level == kDsRoleUpgradeStatus ? 8 : 4;
        const GuestAddr info = heap_alloc_bytes(m_heap, size);
        const std::vector<u8> zeros(size, 0);
        if (info == 0 || m.memory().write_bytes(info, zeros.data(), size).is_err()) {
            heap_free_ptr(m_heap, info);
            (void)m.write_ptr(buffer_out, 0);
            m.set_return(kErrorNotEnoughMemory);
            return;
        }
        (void)m.write_ptr(buffer_out, info);
        m.set_return(0);
        return;
    }
    if (info_level != kDsRolePrimaryDomainInfoBasic) {
        (void)m.write_ptr(buffer_out, 0);  // dsrole level=0/4 buffer_is_null=1
        m.set_return(kErrorInvalidParameter);
        return;
    }

    // L"WORKGROUP" including NUL
    static constexpr char16_t kWorkgroup[] = u"WORKGROUP";
    constexpr usize kWorkgroupBytes = sizeof(kWorkgroup);

    const usize total = fixed + kWorkgroupBytes;
    GuestAddr buf = heap_alloc_bytes(m_heap, total);
    if (buf == 0) {
        (void)m.write_ptr(buffer_out, 0);
        m.set_return(kErrorNotEnoughMemory);
        return;
    }

    std::vector<u8> blob(total, 0);
    // MachineRole = DsRole_RoleStandaloneWorkstation; Flags = 0
    std::memcpy(blob.data() + 0, &kDsRoleStandaloneWorkstation, sizeof(u32));
    const GuestAddr name_ga = buf + fixed;
    if (is_64) {
        const u64 name_ptr = static_cast<u64>(name_ga);
        std::memcpy(blob.data() + 8, &name_ptr, sizeof(u64));
    } else {
        const u32 name_ptr = static_cast<u32>(name_ga);
        std::memcpy(blob.data() + 8, &name_ptr, sizeof(u32));
    }
    std::memcpy(blob.data() + fixed, kWorkgroup, kWorkgroupBytes);

    if (m.memory().write_bytes(buf, blob.data(), total).is_err()) {
        heap_free_ptr(m_heap, buf);
        (void)m.write_ptr(buffer_out, 0);
        m.set_return(kErrorNotEnoughMemory);
        return;
    }

    (void)m.write_ptr(buffer_out, buf);
    VITREO_TRACE("[wkscli] DsRoleGetPrimaryDomainInformation -> "
                 "StandaloneWorkstation WORKGROUP buf=0x%llx\n",
                 static_cast<unsigned long long>(buf));
    m.set_return(0); // ERROR_SUCCESS
}

void WksCliManager::ds_role_free_memory(Marshaler& m) {
    // VOID DsRoleFreeMemory(PVOID Buffer)
    // https://learn.microsoft.com/en-us/windows/win32/api/dsrole/nf-dsrole-dsrolefreememory
    GuestAddr buf = m.arg_ptr(0);
    heap_free_ptr(m_heap, buf);
    m.set_return(0);
}

void WksCliManager::net_get_aad_join_information(Marshaler& m) {
    // HRESULT NetGetAadJoinInformation(LPCWSTR pcszTenantId, PDSREG_JOIN_INFO *ppJoinInfo)
    // https://learn.microsoft.com/en-us/windows/win32/api/lmjoin/nf-lmjoin-netgetaadjoininformation
    //
    // Capsules are not Entra-joined and have no work accounts. Docs: on that
    // path *ppJoinInfo is set to NULL. Return S_OK so callers that check
    // SUCCEEDED(hr) && ppJoinInfo treat the device as not joined.
    (void)m.arg_ptr(0); // pcszTenantId
    GuestAddr pp_join = m.arg_ptr(1);
    if (pp_join != 0) {
        (void)m.write_ptr(pp_join, 0);
    }
    VITREO_TRACE("[wkscli] NetGetAadJoinInformation -> not joined (NULL)\n");
    m.set_return(kSOk);
}

void WksCliManager::net_free_aad_join_information(Marshaler& m) {
    // VOID NetFreeAadJoinInformation(PDSREG_JOIN_INFO pJoinInfo)
    GuestAddr p = m.arg_ptr(0);
    heap_free_ptr(m_heap, p);
    m.set_return(0);
}

void WksCliManager::net_user_get_info(Marshaler& m) {
    // NET_API_STATUS NetUserGetInfo(LPCWSTR servername, LPCWSTR username,
    //                               DWORD level, LPBYTE *bufptr)
    // https://learn.microsoft.com/en-us/windows/win32/api/lmaccess/nf-lmaccess-netusergetinfo
    // Measured by tools/oracle_probe/iphlp_netapi_probe (user*): the last
    // error is cleared on every path, *bufptr is NULL on every failure, a NULL
    // name is NERR_BadUsername, levels other than 0/1/2/3/4/10/11/20/23/24 are
    // ERROR_INVALID_LEVEL.
    (void)m.arg_ptr(0); // servername — local when NULL
    const GuestAddr username_ptr = m.arg_ptr(1);
    const u32 level = m.arg32(2);
    const GuestAddr bufptr = m.arg_ptr(3);
    const bool is_64 = m.cpu_is_64bit();
    set_last_error(0);

    if (bufptr == 0) {
        m.set_return(kErrorInvalidParameter);
        return;
    }
    (void)m.write_ptr(bufptr, 0);
    if (username_ptr == 0) {
        m.set_return(kNerrBadUsername);  // user-null-name
        return;
    }
    // The capsule has one local account, VitUser (advapi32 SecurityStubs).
    std::u16string guest_name;
    for (u32 i = 0; i < 257; ++i) {
        const u16 ch = m.read_u16(username_ptr + i * 2).value_or(0);
        if (ch == 0) break;
        guest_name.push_back(static_cast<char16_t>(ch >= 'a' && ch <= 'z' ? ch - 32 : ch));
    }
    std::u16string account(kLocalUser, kLocalUser + (kLocalUserBytes / 2 - 1));
    for (auto& c : account) if (c >= u'a' && c <= u'z') c = static_cast<char16_t>(c - 32);
    if (guest_name != account) {
        m.set_return(kNerrUserNotFound);
        return;
    }
    switch (level) {
    case 0: case 1: case 10: case 20: case 23: case 24: break;
    case 2: case 3: case 4: case 11: {
        // These levels report account history (password age, last logon and
        // logoff, logon and bad-password counts, logon hours) the capsule
        // account does not keep.
        static std::mutex mutex;
        static std::unordered_set<u32> reported;
        std::lock_guard lock(mutex);
        if (reported.insert(level).second)
            std::fprintf(stderr, "[vitreo] UNIMPLEMENTED netapi32!NetUserGetInfo: level %u (account logon history)\n", level);
        m.set_return(kErrorNotSupported);
        return;
    }
    default:
        m.set_return(kErrorInvalidLevel);
        return;
    }

    // One process-heap block: fixed structure, then the strings and the SID
    // it points to (the caller frees it with NetApiBufferFree).
    const usize ptr = is_64 ? 8 : 4;
    const auto& sid = SecurityStubs::synthetic_sid();
    const usize fixed = level == 0 ? ptr
                      : level == 1 ? (is_64 ? 56u : 32u)
                      : level == 10 ? 4 * ptr
                      : level == 20 ? (is_64 ? 32u : 20u)
                      : level == 23 ? (is_64 ? 40u : 20u)
                      : (is_64 ? 32u : 20u);  // level 24
    constexpr char16_t kEmpty[] = u"";
    const usize name_off = fixed;
    const usize empty_off = name_off + kLocalUserBytes;
    const usize sid_off = (empty_off + sizeof(kEmpty) + 3) & ~usize{3};
    const usize total = sid_off + sid.size();
    const GuestAddr buf = heap_alloc_bytes(m_heap, total);
    if (buf == 0) {
        m.set_return(kErrorNotEnoughMemory);
        return;
    }
    std::vector<u8> blob(total, 0);
    auto put_ptr = [&](usize off, GuestAddr p) {
        if (is_64) { const u64 v = p; std::memcpy(blob.data() + off, &v, 8); }
        else { const u32 v = static_cast<u32>(p); std::memcpy(blob.data() + off, &v, 4); }
    };
    auto put32 = [&](usize off, u32 v) { std::memcpy(blob.data() + off, &v, 4); };
    std::memcpy(blob.data() + name_off, kLocalUser, kLocalUserBytes);
    std::memcpy(blob.data() + sid_off, sid.data(), sid.size());
    const GuestAddr name = buf + name_off, empty = buf + empty_off, user_sid = buf + sid_off;
    const u32 flags = kUfNormalAccount | kUfScript | kUfDontExpirePasswd;
    const u32 rid = 1001;  // last subauthority of the account SID
    switch (level) {
    case 0:
        put_ptr(0, name);
        break;
    case 1:
        // name, password (NULL), password_age, priv, home_dir, comment, flags, script_path
        put_ptr(0, name);
        put32(is_64 ? 16 : 8, 0);
        put32(is_64 ? 20 : 12, kUserPrivUser);
        put32(is_64 ? 40 : 24, flags);
        break;
    case 10:
        // name, comment, usr_comment, full_name
        put_ptr(0, name);
        put_ptr(ptr, empty);
        put_ptr(2 * ptr, empty);
        put_ptr(3 * ptr, empty);
        break;
    case 20:
        // name, full_name, comment, flags, user_id
        put_ptr(0, name);
        put_ptr(ptr, empty);
        put_ptr(2 * ptr, empty);
        put32(3 * ptr, flags);
        put32(3 * ptr + 4, rid);
        break;
    case 23:
        // name, full_name, comment, flags, user_sid
        put_ptr(0, name);
        put_ptr(ptr, empty);
        put_ptr(2 * ptr, empty);
        put32(3 * ptr, flags);
        put_ptr(is_64 ? 32 : 16, user_sid);
        break;
    default:
        // USER_INFO_24: a local account is not an Internet identity
        // (iphlp_netapi_probe user level=24 internet_identity=0).
        put32(0, 0);
        put32(4, 0);
        put_ptr(is_64 ? 24 : 16, user_sid);
        break;
    }
    if (m.memory().write_bytes(buf, blob.data(), total).is_err() || m.write_ptr(bufptr, buf).is_err()) {
        heap_free_ptr(m_heap, buf);
        (void)m.write_ptr(bufptr, 0);
        m.set_return(kErrorNotEnoughMemory);
        return;
    }
    VITREO_TRACE("[wkscli] NetUserGetInfo level=%u -> VitUser buf=0x%llx\n",
                 level, static_cast<unsigned long long>(buf));
    m.set_return(kNerrSuccess);
}

void WksCliManager::net_share_enum(Marshaler& m) {
    // NET_API_STATUS NetShareEnum(server, level, bufptr, prefmaxlen,
    //   entriesread, totalentries, resume_handle)
    // https://learn.microsoft.com/en-us/windows/win32/api/lmshare/nf-lmshare-netshareenum
    (void)m.arg_ptr(0);
    (void)m.arg(1);
    GuestAddr bufptr = m.arg_ptr(2);
    (void)m.arg(3);
    GuestAddr entriesread = m.arg_ptr(4);
    GuestAddr totalentries = m.arg_ptr(5);
    GuestAddr resume = m.arg_ptr(6);

    if (bufptr != 0) {
        (void)m.write_ptr(bufptr, 0);
    }
    if (entriesread != 0) {
        (void)m.write_u32(entriesread, 0);
    }
    if (totalentries != 0) {
        (void)m.write_u32(totalentries, 0);
    }
    if (resume != 0) {
        (void)m.write_u32(resume, 0);
    }
    VITREO_TRACE("[wkscli] NetShareEnum -> empty share list\n");
    m.set_return(kNerrSuccess);
}

// =============================================================================
// Win32 Thunk Implementations
// =============================================================================

static void impl_NetGetJoinInformation(Marshaler& m, WksCliManager& mgr) {
    mgr.net_get_join_information(m);
}

static void impl_DsRoleGetPrimaryDomainInformation(Marshaler& m, WksCliManager& mgr) {
    mgr.ds_role_get_primary_domain_information(m);
}

static void impl_DsRoleFreeMemory(Marshaler& m, WksCliManager& mgr) {
    mgr.ds_role_free_memory(m);
}

static void impl_NetGetAadJoinInformation(Marshaler& m, WksCliManager& mgr) {
    mgr.net_get_aad_join_information(m);
}

static void impl_NetFreeAadJoinInformation(Marshaler& m, WksCliManager& mgr) {
    mgr.net_free_aad_join_information(m);
}

static void impl_NetUserGetInfo(Marshaler& m, WksCliManager& mgr) {
    mgr.net_user_get_info(m);
}

static void impl_NetShareEnum(Marshaler& m, WksCliManager& mgr) {
    mgr.net_share_enum(m);
}

// =============================================================================
// Export Table Factory
// =============================================================================

DllExports create_wkscli_thunks(WksCliManager& mgr) {
    DllExports exports;
    exports.entries.reserve(7);

    exports.entries.push_back(ExportEntry{
        "NetGetJoinInformation", 1,
        [&mgr](Marshaler& m) { impl_NetGetJoinInformation(m, mgr); },
        12
    });
    exports.entries.push_back(ExportEntry{
        "DsRoleGetPrimaryDomainInformation", 2,
        [&mgr](Marshaler& m) { impl_DsRoleGetPrimaryDomainInformation(m, mgr); },
        12
    });
    exports.entries.push_back(ExportEntry{
        "DsRoleFreeMemory", 3,
        [&mgr](Marshaler& m) { impl_DsRoleFreeMemory(m, mgr); },
        4
    });
    exports.entries.push_back(ExportEntry{
        "NetGetAadJoinInformation", 4,
        [&mgr](Marshaler& m) { impl_NetGetAadJoinInformation(m, mgr); },
        8
    });
    exports.entries.push_back(ExportEntry{
        "NetFreeAadJoinInformation", 5,
        [&mgr](Marshaler& m) { impl_NetFreeAadJoinInformation(m, mgr); },
        4
    });
    exports.entries.push_back(ExportEntry{
        "NetUserGetInfo", 6,
        [&mgr](Marshaler& m) { impl_NetUserGetInfo(m, mgr); },
        16
    });
    exports.entries.push_back(ExportEntry{
        "NetShareEnum", 7,
        [&mgr](Marshaler& m) { impl_NetShareEnum(m, mgr); },
        28
    });

    return exports;
}

} // namespace vitreo::winapi
