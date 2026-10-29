#pragma once

/// @file security.hpp
/// @brief advapi32 security stubs (WIN32-062)

#include <vitreo/common.hpp>
#include "winapi/common/elevation.hpp"
#include "winapi/common/object_access.hpp"
#include "winapi/kernel32/handle.hpp"
#include "winapi/core/thunk_registry.hpp"
#include "winapi/advapi32/security/named_security.hpp"

#include <string>
#include <memory>
#include <span>
#include <string_view>
#include <unordered_map>
#include <mutex>
#include <optional>
#include <unordered_set>
#include <vector>

namespace vitreo::capsule { class Vfs; }

namespace vitreo::winapi {

class ThreadManager;

class GlobalLocalManager;
class Kernel32ExtManager;

/// Security stubs returning plausible defaults
class SecurityStubs {
public:
    struct RestrictedSid {
        std::vector<u8> bytes;
        u32 attributes{0};
    };

    struct TokenPrivilege {
        u64 luid{0};
        u32 attributes{0};
    };

    explicit SecurityStubs(HandleTable& handles,
                           GlobalLocalManager* local_memory = nullptr,
                           const GuestElevationState* elevation = nullptr);

    // No copy / no move (reference member)
    SecurityStubs(const SecurityStubs&) = delete;
    SecurityStubs& operator=(const SecurityStubs&) = delete;

    /// GetUserNameW — returns macOS username
    bool get_user_name_w(std::wstring& name_out, u32& size_inout);

    /// GetUserNameA — returns macOS username
    bool get_user_name_a(std::string& name_out, u32& size_inout);

    /// OpenProcessToken — returns synthetic token handle
    /// OpenProcessToken: every handle to the current process's token names
    /// ONE token object (changes through one are seen through all). The
    /// process handle must be the current-process pseudo handle or a process
    /// handle with PROCESS_QUERY_(LIMITED_)INFORMATION; `access` (generic
    /// rights mapped) becomes the token handle's granted access.
    bool open_process_token(Handle process, u32 access, Handle& token_out,
                            u32& error);
    /// Map GENERIC_* / MAXIMUM_ALLOWED to token-specific rights. The token
    /// object's generic mapping as measured (advapi_token2_probe opt_access):
    /// GENERIC_READ 0x2001A, GENERIC_WRITE 0x201E0, GENERIC_EXECUTE 0x20005,
    /// GENERIC_ALL / MAXIMUM_ALLOWED 0xF01FF.
    [[nodiscard]] static u32 map_token_access(u32 access);
    /// Every specific, standard and system right a token handle can hold.
    static constexpr u32 kTokenValidAccess = 0x000F01FFu | 0x01000000u;
    /// Resolve thread handles to thread ids for the per-thread token table.
    void set_thread_manager(ThreadManager* threads) noexcept { m_threads = threads; }

    /// Associate an impersonation token with a concrete thread handle, or
    /// with the calling guest thread when `thread` is the current-thread
    /// pseudo handle. A NULL token removes the association.
    bool set_thread_token(Handle thread, Handle token, u32& error);

    /// Open the impersonation token currently associated with `thread`.
    bool open_thread_token(Handle thread, u32 access, Handle& token_out,
                           u32& error);

    /// Create/remove a calling-thread impersonation token derived from the
    /// process primary token.
    bool impersonate_self(u32 impersonation_level, u32& error);
    /// Install `token` as the calling thread's impersonation token (Learn:
    /// ImpersonateLoggedOnUser). Requires a live token handle.
    bool impersonate_logged_on_user(Handle token, u32& error);
    /// Install a synthetic impersonation token carrying the authenticated
    /// caller identity supplied by the COM/RPC transport.
    bool impersonate_com_caller(bool elevated);
    bool revert_to_self();

    /// True for a live synthetic token handle or an NT pseudo-token handle
    /// (GetCurrentProcessToken -4 / GetCurrentThreadToken -5 /
    /// GetCurrentThreadEffectiveToken -6).
    bool is_token_handle(Handle token) const;

    /// GetTokenInformation — returns synthetic data. `is64` selects the
    /// guest SID_AND_ATTRIBUTES layout (pointer width / attribute offset).
    bool get_token_information(Handle token, u32 info_class,
                               std::vector<u8>& data_out, u32& return_length,
                               bool is64 = false);

    /// DuplicateToken/DuplicateTokenEx — issue a new token handle that is a
    /// distinct, independently-closable duplicate of `existing`. Fails with
    /// `false` if `existing` is not a token or `impersonation_level` is outside
    /// SecurityAnonymous..SecurityDelegation (0..3); the caller must then leave
    /// the out handle NULL.
    bool duplicate_token(Handle existing, u32 impersonation_level,
                         Handle& token_out, u32 token_type = 2,
                         u32 access = HandleAccess::All);

    /// DuplicateTokenEx (tools/oracle_probe/advapi_token2_probe dte_*):
    /// level > SecurityDelegation -> ERROR_BAD_IMPERSONATION_LEVEL, TokenType
    /// not primary/impersonation -> ERROR_INVALID_PARAMETER (both before the
    /// handle is examined); a non-token or pseudo handle -> ERROR_INVALID_HANDLE;
    /// no TOKEN_DUPLICATE -> ERROR_ACCESS_DENIED; from an impersonation token a
    /// higher level, or a primary token from below SecurityImpersonation ->
    /// ERROR_BAD_IMPERSONATION_LEVEL. `access` 0 copies the source handle's
    /// access; otherwise it is generic-mapped and bits that are not token
    /// rights are dropped. Returns ERROR_SUCCESS or the error.
    [[nodiscard]] u32 duplicate_token_ex(Handle existing, u32 access, u32 level,
                                         u32 token_type, Handle& token_out);

    /// CheckTokenMembership (advapi_token2_probe ctm_*). NULL, -4 and -6 use
    /// the effective token; -5 the thread token (ERROR_NO_TOKEN when the
    /// thread is not impersonating). A real handle needs TOKEN_QUERY, must be
    /// an impersonation token (ERROR_NO_IMPERSONATION_TOKEN) above
    /// SecurityAnonymous (ERROR_BAD_IMPERSONATION_LEVEL). The SID counts when
    /// it is the user or an enabled, not deny-only group, and — for a
    /// restricted token — also a restricting SID.
    [[nodiscard]] u32 check_token_membership(Handle token, std::span<const u8> sid,
                                             bool& member);

    /// ImpersonateAnonymousToken: the calling or given thread impersonates the
    /// ANONYMOUS LOGON token (S-1-5-7, one group S-1-16-0, no privileges,
    /// untrusted integrity, SecurityImpersonation — advapi_token2_probe
    /// anon_user). Returns ERROR_SUCCESS or ERROR_INVALID_HANDLE.
    [[nodiscard]] u32 impersonate_anonymous(Handle thread);

    /// The identity a token carries, as access checks need it.
    struct TokenIdentity {
        std::vector<u8> user;
        bool user_deny_only{false};
        struct Group {
            std::vector<u8> sid;
            u32 attributes{0};
        };
        std::vector<Group> groups;
        std::vector<std::vector<u8>> restricting;
        std::vector<TokenPrivilege> privileges;
        u32 integrity_rid{0x2000};
        u32 type{1};
        u32 impersonation_level{2};
    };
    [[nodiscard]] TokenIdentity token_identity(Handle token);
    /// The calling thread's effective token (impersonation token, else the
    /// process token). `impersonating` reports which.
    [[nodiscard]] TokenIdentity effective_identity(bool& impersonating);

    /// DuplicateHandle creates another handle to the same token object. Token
    /// mutations through either handle therefore remain mutually visible.
    bool duplicate_token_handle(Handle existing, u32 access, u32 flags,
                                Handle& token_out);

    /// Create a distinct token carrying the observable restrictions requested
    /// by CreateRestrictedToken. Access checks remain capsule-local, but token
    /// type, filtering state, SANDBOX_INERT, and restricting SIDs are retained
    /// for subsequent token queries and sandbox process creation.
    bool create_restricted_token(
        Handle existing, u32 flags,
        std::vector<RestrictedSid> disabled_sids,
        std::vector<u64> deleted_privileges,
        std::vector<RestrictedSid> restricting_sids,
        Handle& token_out);

    bool set_token_default_dacl(Handle token, std::vector<u8> acl);
    bool set_token_integrity_level(Handle token, u32 integrity_rid);

    /// Observable claim attribute stored for TokenSecurityAttributes.
    /// Add and query round-trip string values used for process-isolation
    /// conditional ACEs.
    struct SecurityAttribute {
        std::wstring name;
        std::wstring value;
        u32 flags{0};
        u16 value_type{3};  // TOKEN_SECURITY_ATTRIBUTE_TYPE_STRING
    };

    /// Replace-all / add / delete for TokenSecurityAttributes (class 39).
    bool set_token_security_attributes(
        Handle token, std::vector<SecurityAttribute> attributes,
        bool replace_all);

    [[nodiscard]] u32 token_type(Handle token) const;
    [[nodiscard]] bool token_has_restrictions(Handle token) const;
    /// Learn IsTokenRestricted: nonzero only when restricting SIDs exist.
    [[nodiscard]] bool token_is_restricted(Handle token) const;
    [[nodiscard]] bool token_has_access(Handle token, u32 access) const;
    /// True for a token handle from the handle table (not a -4/-5/-6
    /// pseudo handle, which carries TOKEN_QUERY | TOKEN_QUERY_SOURCE).
    [[nodiscard]] bool is_real_token_handle(Handle token) const;
    /// The effective token (thread impersonation token, else the process
    /// token) as the named-security code needs it.
    [[nodiscard]] named_sd::Principal principal();
    [[nodiscard]] object_access::Token effective_object_token();
    /// True when `token` holds privilege `luid` and it is enabled.
    [[nodiscard]] bool privilege_enabled(Handle token, u64 luid) const;
    /// TokenLinkedToken for an elevated (split) token: a new
    /// SecurityIdentification impersonation token of the filtered half.
    bool open_linked_token(Handle token, Handle& linked_out, u32& error);
    [[nodiscard]] std::vector<std::string>
    token_launch_environment(Handle token) const;

    /// Privileges present on a newly opened process token. Matches a typical
    /// elevated local Administrators token: backup/restore/take-ownership exist
    /// but start disabled. SeChangeNotify, SeImpersonate, and SeCreateGlobal
    /// start enabled, as on Windows.
    [[nodiscard]] static std::vector<TokenPrivilege>
    default_process_token_privileges();

    /// Snapshot the previous LUID_AND_ATTRIBUTES of privileges that
    /// AdjustTokenPrivileges would modify. Does not mutate the token.
    /// Removed privileges are omitted from @p previous_out (Learn).
    bool describe_privilege_adjustment(
        Handle token, bool disable_all,
        const std::vector<TokenPrivilege>& requested,
        std::vector<TokenPrivilege>& previous_out,
        bool& all_assigned) const;

    /// Apply AdjustTokenPrivileges. The return value reports API success;
    /// `all_assigned` mirrors ERROR_NOT_ALL_ASSIGNED for unknown LUIDs.
    bool adjust_token_privileges(
        Handle token, bool disable_all,
        const std::vector<TokenPrivilege>& requested,
        bool& all_assigned);

    /// Release token metadata when HandleTable closes a token handle.
    bool on_handle_close(Handle token);

    /// Release an impersonation association when a concrete thread handle is
    /// closed. Current-thread associations are keyed by guest thread id.
    void on_thread_handle_close(Handle thread);

    /// Preserve a concrete thread's impersonation-token association when
    /// DuplicateHandle creates another handle to that thread object.
    void duplicate_thread_token_association(Handle source, Handle duplicate);

    /// LookupPrivilegeValue: the LSA privilege table (advapi_wksid_probe
    /// "lpv"); false for a name LSA does not know.
    bool lookup_privilege_value_w(const std::wstring& name, u64& luid_out);
    /// LookupPrivilegeName: the inverse; empty for an unknown LUID.
    [[nodiscard]] static std::string privilege_name(u64 luid);

    /// LookupAccountSidW — well-known SIDs plus the synthetic local user.
    /// lpSystemName names this machine (NULL, "", ".", "\\\\VITREO").
    [[nodiscard]] static bool is_local_system_name(std::wstring_view system);
    bool lookup_account_sid_w(std::span<const u8> sid,
                              std::wstring& name_out, std::wstring& domain_out,
                              u32& use_out);

    /// Resolve the capsule's synthetic local user to the same SID exposed by
    /// TokenUser and LookupAccountSidW. Unknown or remote identities fail.
    bool lookup_account_name_w(const std::wstring& system_name,
                               const std::wstring& account_name,
                               std::vector<u8>& sid_out,
                               std::wstring& domain_out,
                               u32& use_out);

    /// Get the synthetic SID bytes (S-1-5-21-1000000000-2000000000-3000000000-1001)
    static const std::vector<u8>& synthetic_sid();

    // -- SID helpers (guest-memory based, used by thunks) --

    /// GetSidIdentifierAuthority — return guest ptr to offset 2 of SID
    static GuestAddr get_sid_identifier_authority(GuestAddr sid);

    /// GetLengthSid — 8 + 4 * SubAuthorityCount
    static u32 get_length_sid(Marshaler& m, GuestAddr sid);

    /// GetSidLengthRequired — size needed for a SID with the given count
    static u32 get_sid_length_required(u8 sub_auth_count);

    /// IsValidSid — revision==1 && sub_auth_count<=15
    static u32 is_valid_sid(Marshaler& m, GuestAddr sid);

    /// CopySid — copy src SID bytes to dest
    /// CopySid: returns ERROR_SUCCESS or the Win32 error to report.
    static u32 copy_sid(Marshaler& m, u32 dest_len, GuestAddr dest, GuestAddr src);

    /// GetSidSubAuthority — guest addr of sid+8+index*4
    static GuestAddr get_sid_sub_authority(GuestAddr sid, u32 index);

    /// GetSidSubAuthorityCount — guest addr of sid+1
    static GuestAddr get_sid_sub_authority_count(GuestAddr sid);

    /// InitializeSid — write revision, authority, sub_auth_count
    static u32 initialize_sid(Marshaler& m, GuestAddr sid,
                              GuestAddr authority_ptr, u8 sub_auth_count);

    /// EqualSid — compare two SIDs byte-by-byte
    static u32 equal_sid(Marshaler& m, GuestAddr sid1, GuestAddr sid2);

    /// Allocate guest memory for a SID (bump allocator)
    GuestAddr alloc_sid_memory(u32 size);

    /// Allocate fixed local memory that the guest can release with LocalFree.
    GuestAddr alloc_local_memory(u32 size);

    /// Release memory returned by alloc_local_memory when backed by LocalAlloc.
    void free_local_memory(GuestAddr address);

    /// Classic ETW consumer handles (OpenTrace on a real-time session that is
    /// not running succeeds; ProcessTrace then reports
    /// ERROR_WMI_INSTANCE_NOT_FOUND — tools/oracle_probe/advapi_etw_probe).
    u64 open_trace_consumer();
    bool close_trace_consumer(u64 handle);
    [[nodiscard]] bool is_trace_consumer(u64 handle) const;

    /// Register/unregister a legacy ETW provider in this process.
    u64 register_trace_provider();
    bool unregister_trace_provider(u64 handle);
    bool is_trace_provider_registered(u64 handle) const;

    [[nodiscard]] bool is_elevated() const noexcept {
        return m_elevation != nullptr && m_elevation->is_elevated();
    }
    [[nodiscard]] bool token_is_elevated(Handle token) const;

    [[nodiscard]] HandleTable& handle_table() noexcept { return m_handles; }
    [[nodiscard]] const HandleTable& handle_table() const noexcept {
        return m_handles;
    }

private:
    struct TokenState {
        u64 token_id{0x1001};
        u64 authentication_id{0x3E7};
        u64 modified_id{0x1002};
        u32 type{1};  // TokenPrimary
        u32 impersonation_level{2};  // SecurityImpersonation
        bool elevated{false};
        /// The ANONYMOUS LOGON token (ImpersonateAnonymousToken).
        bool anonymous{false};
        bool has_restrictions{false};
        bool sandbox_inert{false};
        std::vector<RestrictedSid> disabled_sids;
        std::vector<RestrictedSid> restricting_sids;
        std::vector<SecurityAttribute> security_attributes;
        std::vector<TokenPrivilege> privileges{
            default_process_token_privileges()};
        std::vector<u8> default_dacl{2, 0, 8, 0, 0, 0, 0, 0};
        std::optional<u32> integrity_rid;
    };

    [[nodiscard]] TokenState token_state(Handle token) const;
    [[nodiscard]] u64 thread_token_key(Handle thread) const;
    /// The process primary token object shared by every OpenProcessToken
    /// handle (created from m_primary_token_state on first use).
    [[nodiscard]] std::shared_ptr<TokenState> primary_token_object_locked();

    HandleTable& m_handles;
    ThreadManager* m_threads{nullptr};
    std::shared_ptr<TokenState> m_primary_token_object;
    GlobalLocalManager* m_local_memory;
    const GuestElevationState* m_elevation;
    TokenState m_primary_token_state;
    mutable std::mutex m_token_mutex;
    std::unordered_map<Handle, std::shared_ptr<TokenState>> m_token_states;
    std::unordered_map<u64, std::shared_ptr<TokenState>> m_thread_token_states;
    u64 m_next_token_luid{0x2000};
    u64 m_next_trace_handle{0xE700000000000001ull};
    mutable std::mutex m_trace_mutex;
    std::unordered_set<u64> m_trace_handles;
    std::unordered_set<u64> m_trace_consumers;
    u64 m_next_trace_consumer{0x100};
    u32 m_sid_alloc_offset{0};
    static constexpr GuestAddr SID_SCRATCH_BASE = 0x5D000000;

    static const std::wstring USERNAME;
    static const std::string USERNAME_A;
    static const std::wstring DOMAIN_NAME;
};

class RegistryThunks;

/// advapi32!RegGetKeySecurity / RegSetKeySecurity. A key's descriptor is the
/// one GetNamedSecurityInfoW / SetNamedSecurityInfoW see for its
/// "CURRENT_USER\\..." name, so it belongs to the key, not to the handle.
void add_registry_key_security_exports(DllExports& exports, SecurityStubs& stubs,
                                       RegistryThunks& registry);

/// Create advapi32 security thunks.
/// `vfs` (optional) backs the object-existence checks that file security APIs
/// such as GetFileSecurityW must perform; without it those APIs cannot
/// distinguish a missing file and fail with ERROR_CALL_NOT_IMPLEMENTED.
DllExports create_advapi32_security_thunks(SecurityStubs& stubs,
                                           capsule::Vfs* vfs = nullptr,
                                           Kernel32ExtManager* process_launcher =
                                               nullptr);

} // namespace vitreo::winapi
