#pragma once

/// @file ntdll.hpp
/// @brief ntdll.dll core function stubs (WIN32-038)

#include <vitreo/common.hpp>
#include "winapi/core/thunk_registry.hpp"

#include <optional>
#include <vector>
#include <utility>

namespace vitreo::winapi {

class Marshaler;
class HandleTable;

// =============================================================================
// NTSTATUS Constants
// =============================================================================

namespace nt_status {

constexpr u32 STATUS_SUCCESS              = 0x00000000;
constexpr u32 STATUS_INVALID_HANDLE       = 0xC0000008;
constexpr u32 STATUS_INVALID_INFO_CLASS   = 0xC0000003;
constexpr u32 STATUS_BUFFER_TOO_SMALL     = 0xC0000023;
constexpr u32 STATUS_INFO_LENGTH_MISMATCH = 0xC0000004;

} // namespace nt_status

// =============================================================================
// NtdllManager Class
// =============================================================================

class NtdllManager {
public:
    NtdllManager();
    ~NtdllManager() = default;

    // No copy
    NtdllManager(const NtdllManager&) = delete;
    NtdllManager& operator=(const NtdllManager&) = delete;

    // -------------------------------------------------------------------------
    // RtlInitUnicodeString
    // -------------------------------------------------------------------------

    /// Guest-side UNICODE_STRING layout (32-bit):
    ///   u16 Length        (byte count, excluding NUL)
    ///   u16 MaximumLength (byte count, including NUL)
    ///   u32 Buffer        (guest pointer to wide chars)
    struct UnicodeString {
        u16 length;
        u16 max_length;
        u32 buffer;
    };

    /// Compute UNICODE_STRING fields for a wide string.
    /// @param source     Host pointer to NUL-terminated char16_t data (may be null)
    /// @param source_addr Guest address of the wide string (stored in Buffer field)
    /// @return Populated UnicodeString descriptor
    [[nodiscard]] UnicodeString init_unicode_string(
        const char16_t* source,
        u32 source_addr) const;

    // -------------------------------------------------------------------------
    // NtQueryInformationProcess
    // -------------------------------------------------------------------------

    /// Return the synthetic PEB guest address.
    [[nodiscard]] u32 get_peb_address() const noexcept;

    /// Fill a PROCESS_BASIC_INFORMATION buffer (24 bytes on 32-bit).
    /// Returns NTSTATUS.
    [[nodiscard]] u32 query_basic_info(u32 buffer_size) const;

    /// Set the capsule-visible parent process identifier for the current
    /// process. Zero means the host parent is not a guest in this capsule.
    void set_current_parent_process_id(u64 process_id) noexcept {
        m_current_parent_process_id = process_id;
    }

    [[nodiscard]] u64 current_parent_process_id() const noexcept {
        return m_current_parent_process_id;
    }

    struct ProcessIdentity {
        u64 peb_address;
        u64 process_id;
    };

    /// Resolve a real process handle to the startup identity published when
    /// CreateProcess launched its independent guest address space.
    [[nodiscard]] std::optional<ProcessIdentity> process_identity(
        u64 process_handle) const;

    void set_handle_table(HandleTable* table) noexcept {
        m_handle_table = table;
    }

    [[nodiscard]] HandleTable* handle_table() const noexcept {
        return m_handle_table;
    }

    /// Size of PROCESS_BASIC_INFORMATION on 32-bit Windows:
    ///   NTSTATUS  ExitStatus          (4)
    ///   PVOID     PebBaseAddress      (4)
    ///   ULONG_PTR AffinityMask        (4)
    ///   KPRIORITY BasePriority        (4)
    ///   ULONG_PTR UniqueProcessId     (4)
    ///   ULONG_PTR InheritedFromUnique (4)
    static constexpr u32 PROCESS_BASIC_INFO_SIZE = 24;

private:
    /// Synthetic PEB address seen by guest code.
    static constexpr u32 SYNTHETIC_PEB = 0x7FFD0000;

    HandleTable* m_handle_table{nullptr};
    u64 m_current_parent_process_id{0};
};

// =============================================================================
// Thunk Implementations
// =============================================================================

void impl_RtlInitUnicodeString(Marshaler& m, NtdllManager& nm);
void impl_NtQueryInformationProcess(Marshaler& m, NtdllManager& nm);
void impl_NtQuerySystemInformation(Marshaler& m, NtdllManager& nm);

// =============================================================================
// Export Factory
// =============================================================================

[[nodiscard]] DllExports make_ntdll_exports(NtdllManager& nm);

} // namespace vitreo::winapi
