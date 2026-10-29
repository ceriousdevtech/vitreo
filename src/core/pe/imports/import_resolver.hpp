// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// PE import resolver

#pragma once

#include "../parser/pe_parser.hpp"
#include <vitreo/result.hpp>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include <functional>

namespace vitreo::pe {

// ============================================================================
// Import Errors
// ============================================================================

/// Import resolution error codes
enum class ImportError : u8 {
    Success = 0,
    NoImports,
    InvalidImportDirectory,
    InvalidDescriptor,
    InvalidThunkData,
    MemoryReadFailed,
    MemoryWriteFailed,
    StringReadFailed,
};

/// Get human-readable error message
[[nodiscard]] const char* import_error_message(ImportError err) noexcept;

// ============================================================================
// Import Information
// ============================================================================

/// Represents a single import from a module
struct ImportEntry {
    std::string function_name;  ///< Function name (empty if by ordinal)
    u16 hint;                   ///< Hint index for named imports
    u16 ordinal;                ///< Ordinal number (0 if by name)
    u64 iat_address;            ///< Guest address of IAT slot to patch
    bool is_ordinal;            ///< True if import by ordinal
};

/// Represents all imports from a single DLL
struct ImportModule {
    std::string dll_name;                   ///< Name of the DLL
    u64 descriptor_va;                      ///< Guest address of descriptor
    std::vector<ImportEntry> imports;       ///< List of imports
};

/// Complete import information for a PE
struct ImportTable {
    std::vector<ImportModule> modules;      ///< All imported modules
    u32 total_imports;                      ///< Total number of import entries
    bool is_64bit;                          ///< True for PE32+
};

/// An unresolved import that couldn't be found
struct UnresolvedImport {
    std::string dll_name;
    std::string function_name;
    u16 ordinal;
};

// ============================================================================
// Import Resolver
// ============================================================================

/// Callback to resolve an import to a thunk address
/// Returns 0 if unresolved
using ImportCallback = std::function<u64(
    std::string_view dll_name,
    std::string_view function_name,
    u16 ordinal
)>;

/// Parses and resolves PE imports
class ImportResolver {
public:
    /// Construct a resolver
    ImportResolver() noexcept = default;
    
    /// Parse imports from a PE file
    /// @param pe Parsed PE information
    /// @param pe_data Raw PE file data
    /// @param base_address Guest address where PE is loaded
    /// @return Import table on success
    [[nodiscard]] Result<ImportTable, ImportError> parse(
        const ParsedPe& pe,
        std::span<const u8> pe_data,
        u64 base_address
    ) noexcept;
    
    /// Check if a PE has imports
    [[nodiscard]] static bool has_imports(const ParsedPe& pe) noexcept;
    
    /// Get ordinal flag mask for 32-bit PE
    static constexpr u32 ORDINAL_FLAG_32 = 0x80000000;
    
    /// Get ordinal flag mask for 64-bit PE
    static constexpr u64 ORDINAL_FLAG_64 = 0x8000000000000000ULL;
    
private:
    /// Parse a single import module
    [[nodiscard]] Result<ImportModule, ImportError> parse_module(
        const ImportDescriptor& desc,
        std::span<const u8> pe_data,
        const ParsedPe& pe,
        u64 base_address
    ) noexcept;
    
    /// Read a null-terminated string from PE data
    [[nodiscard]] Result<std::string, ImportError> read_string(
        std::span<const u8> pe_data,
        u32 file_offset,
        usize max_length = 64 * 1024
    ) noexcept;
};

// ============================================================================
// Free Functions
// ============================================================================

/// Parse imports from a PE file
[[nodiscard]] Result<ImportTable, ImportError> parse_imports(
    const ParsedPe& pe,
    std::span<const u8> pe_data,
    u64 base_address
) noexcept;

} // namespace vitreo::pe
