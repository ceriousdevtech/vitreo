#pragma once

/// @file environment.hpp
/// @brief Environment variables and command line management (WIN32-035)
///
/// Implements GetEnvironmentVariable, SetEnvironmentVariable,
/// GetCommandLine, GetEnvironmentStrings, and FreeEnvironmentStrings
/// for the emulated Win32 environment.

#include <vitreo/common.hpp>
#include "winapi/core/thunk_registry.hpp"

#include <algorithm>
#include <map>
#include <utility>
#include <vector>
#include <string>
#include <string_view>

namespace vitreo {
class GuestMemory;
}

namespace vitreo::winapi {

// Forward declaration
class Marshaler;

// =============================================================================
// Case-Insensitive Key Helper
// =============================================================================

/// Convert a string to uppercase for case-insensitive environment variable keys
[[nodiscard]] inline std::string uppercase_key(std::string_view key) {
    std::string result(key);
    std::transform(result.begin(), result.end(), result.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return result;
}

// =============================================================================
// Environment Manager
// =============================================================================

/// Manages environment variables and the command line for a guest process
///
/// Environment variable names are stored in uppercase for case-insensitive
/// lookup, matching Windows behavior. Values are stored as-is.
class EnvironmentManager {
public:
    /// Base address for environment data region in guest memory
    static constexpr u64 ENV_DATA_BASE = 0x6FE00000;
    /// Command-line A occupies [0, 0x8000), W occupies [0x8000, 0x18000).
    /// Windows allows up to 32,767 characters; these slots no longer overlap.
    /// GetEnvironmentStrings snapshots allocate above ENV_DATA_BASE+ENV_DATA_SIZE.
    static constexpr u64 ENV_DATA_SIZE = 0x18000;
    static constexpr u64 CMDLINE_A_MAX = 0x8000;   // includes NUL
    static constexpr u64 CMDLINE_W_OFF = 0x8000;
    static constexpr u64 CMDLINE_W_MAX = 0x10000;  // UTF-16 bytes incl. NUL

    /// Create an EnvironmentManager with the given command line
    /// @param command_line The command line string for the process
    /// @param memory Guest memory for command line allocation
    explicit EnvironmentManager(std::string_view command_line, GuestMemory& memory);

    /// No copy
    EnvironmentManager(const EnvironmentManager&) = delete;
    EnvironmentManager& operator=(const EnvironmentManager&) = delete;

    /// No move (owns a mutex for concurrent Get/Set/GetEnvironmentStrings)
    EnvironmentManager(EnvironmentManager&&) = delete;
    EnvironmentManager& operator=(EnvironmentManager&&) = delete;
    ~EnvironmentManager() = default;

    /// Get the guest address of the ANSI command line string
    [[nodiscard]] GuestAddr cmdline_a_addr() const noexcept { return m_cmdline_a_addr; }

    /// Get the guest address of the wide command line string
    [[nodiscard]] GuestAddr cmdline_w_addr() const noexcept { return m_cmdline_w_addr; }

    /// UTF-16 command-line length in bytes, excluding the terminating NUL.
    /// RTL_USER_PROCESS_PARAMETERS.CommandLine.Length uses this unit.
    [[nodiscard]] u16 cmdline_w_bytes() const noexcept { return m_cmdline_w_bytes; }

    // -------------------------------------------------------------------------
    // Variable Management
    // -------------------------------------------------------------------------

    /// Set an environment variable (case-insensitive key)
    /// @param name  Variable name
    /// @param value Variable value
    void set_variable(std::string_view name, std::string_view value);

    /// Delete an environment variable
    /// @param name Variable name (case-insensitive)
    void delete_variable(std::string_view name);

    /// Get an environment variable value
    /// @param name Variable name (case-insensitive)
    /// @return Variable value, or Error::NotFound if not set
    [[nodiscard]] VResult<std::string> get_variable(std::string_view name) const;

    /// Get a copy of all environment variables
    /// @return Map of uppercase key → value
    [[nodiscard]] std::map<std::string, std::string> get_all_variables() const;

    /// Every variable as NAME/value with NAME in the spelling it was set with,
    /// in environment-block order. This is what a child process inherits and
    /// what the CRT's _environ/_wenviron expose: Windows environment names are
    /// case-insensitive for lookup but case-PRESERVING in the block, so
    /// A mixed-case name must reach a child with that casing, not folded
    /// to uppercase.
    [[nodiscard]] std::vector<std::pair<std::string, std::string>>
    get_all_variables_as_named() const;

    // -------------------------------------------------------------------------
    // Command Line
    // -------------------------------------------------------------------------

    /// Get the ANSI command line
    /// @return Command line string
    [[nodiscard]] std::string get_command_line_a() const;

    /// Get the command line in UTF-8 (for wide string conversion)
    /// @return Command line string
    [[nodiscard]] std::string get_command_line_w_utf8() const;

    // -------------------------------------------------------------------------
    // Environment Block
    // -------------------------------------------------------------------------

    /// Build a double-null-terminated environment block
    /// Format: "KEY=VALUE\0KEY=VALUE\0\0"
    /// @return The environment block as a string (with embedded nulls)
    [[nodiscard]] std::string build_environment_block() const;

    /// Allocate a fresh GetEnvironmentStrings snapshot in guest memory.
    /// Free with free_environment_strings_snapshot.
    [[nodiscard]] GuestAddr allocate_environment_strings_snapshot(
        GuestMemory& memory, bool wide);

    /// Release a snapshot previously returned by allocate_*.
    [[nodiscard]] bool free_environment_strings_snapshot(
        GuestMemory& memory, GuestAddr block, bool wide);

    // -------------------------------------------------------------------------
    // Default Population
    // -------------------------------------------------------------------------

    /// Populate default environment variables (PATH, TEMP, etc.)
    void populate_defaults();

    /// Set the names WOW64 gives each process its own view of:
    /// PROCESSOR_ARCHITECTURE (AMD64 / x86), PROCESSOR_ARCHITEW6432 (32-bit
    /// only), ProgramFiles and CommonProgramFiles. Call once the process
    /// bitness is known and after any inherited block has been applied.
    void apply_process_bitness(bool is_64bit);

private:
    std::string m_command_line;
    GuestAddr m_cmdline_a_addr{0};
    GuestAddr m_cmdline_w_addr{0};
    u16 m_cmdline_w_bytes{0};

    /// Case-insensitive map: uppercase key → {display name casing, value}.
    /// Display names are what GetEnvironmentStrings emits; Windows preserves
    /// the casing supplied to SetEnvironmentVariable / the initial seed.
    struct EnvEntry {
        std::string display_name;
        std::string value;
    };
    std::map<std::string, EnvEntry> m_env_vars;

    /// Live GetEnvironmentStrings(A/W) snapshots. Each call allocates a fresh
    /// guest block; FreeEnvironmentStrings* releases the matching pointer.
    struct EnvSnapshot {
        GuestAddr addr{0};
        u64 bytes{0};
        bool wide{false};
    };
    std::vector<EnvSnapshot> m_env_snapshots;
    mutable std::mutex m_lock;
};

// =============================================================================
// Win32 Thunk Implementations
// =============================================================================

void impl_GetCommandLineA(Marshaler& m, EnvironmentManager& em);
void impl_GetCommandLineW(Marshaler& m, EnvironmentManager& em);
void impl_GetEnvironmentVariableA(Marshaler& m, EnvironmentManager& em);
void impl_GetEnvironmentVariableW(Marshaler& m, EnvironmentManager& em);
void impl_SetEnvironmentVariableA(Marshaler& m, EnvironmentManager& em);
void impl_SetEnvironmentVariableW(Marshaler& m, EnvironmentManager& em);
void impl_GetEnvironmentStringsW(Marshaler& m, EnvironmentManager& em);
void impl_FreeEnvironmentStringsW(Marshaler& m, EnvironmentManager& em);

// =============================================================================
// Export Table Factory
// =============================================================================

/// Build DllExports containing the 8 environment / command-line functions.
/// The returned ApiHandlers capture @p em by reference.
[[nodiscard]] DllExports make_environment_exports(EnvironmentManager& em);

} // namespace vitreo::winapi
