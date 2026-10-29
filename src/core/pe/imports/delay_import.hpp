// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// PE delayed import processor

#pragma once

#include <vitreo/common.hpp>
#include <vitreo/result.hpp>
#include "core/pe/parser/pe_parser.hpp"
#include "core/memory/guest_memory.hpp"
#include "winapi/core/thunk_registry.hpp"
#include <span>
#include <string>
#include <vector>

namespace vitreo::pe {

// ============================================================================
// Delay Import Structures
// ============================================================================

/// IMAGE_DELAYLOAD_DESCRIPTOR (Windows SDK ImgDelayDescr)
struct ImgDelayDescr {
    u32 grAttrs;        ///< 0 = VA-based, 1 = RVA-based
    u32 rvaDLLName;     ///< RVA (or VA) of DLL name
    u32 rvaHmod;        ///< RVA (or VA) of module handle location
    u32 rvaIAT;         ///< RVA (or VA) of delay IAT
    u32 rvaINT;         ///< RVA (or VA) of delay INT (name table)
    u32 rvaBoundIAT;    ///< RVA (or VA) of bound delay IAT
    u32 rvaUnloadIAT;   ///< RVA (or VA) of unload delay IAT
    u32 dwTimeStamp;    ///< Timestamp of bound image
};

static_assert(sizeof(ImgDelayDescr) == 32, "ImgDelayDescr must be 32 bytes");

// ============================================================================
// Delay Import Information
// ============================================================================

/// A single delay-imported function
struct DelayImportEntry {
    std::string function_name;  ///< Function name (empty if by ordinal)
    u16 ordinal;                ///< Ordinal number (0 if by name)
    bool is_ordinal;            ///< True if import by ordinal
    u64 iat_address;            ///< Guest address where to write resolved thunk
};

/// All delay imports from one DLL
struct DelayImportModule {
    std::string dll_name;                       ///< Name of the DLL
    std::vector<DelayImportEntry> entries;       ///< List of imports
    u64 hmod_address;                           ///< Guest address for module handle
};

// ============================================================================
// Delay Import Errors
// ============================================================================

/// Delay import processing error codes
enum class DelayImportError : u8 {
    Success = 0,
    NoDelayImports,
    InvalidDirectory,
    InvalidDescriptor,
    InvalidThunkData,
    MemoryReadFailed,
    MemoryWriteFailed,
    StringReadFailed,
};

/// Get human-readable error message
[[nodiscard]] const char* delay_import_error_message(DelayImportError err) noexcept;

// ============================================================================
// Delay Import Processor
// ============================================================================

/// Parses and resolves PE delay imports
class DelayImportProcessor {
public:
    /// Construct a processor with guest memory and thunk registry
    DelayImportProcessor(GuestMemory& memory, winapi::ThunkRegistry& thunks);

    /// Check if PE has delay imports (data directory entry 13)
    [[nodiscard]] static bool has_delay_imports(const ParsedPe& pe) noexcept;

    /// Parse all delay import descriptors
    /// @param pe_data Raw PE file data
    /// @param pe Parsed PE information
    /// @param base_address Guest address where PE is loaded
    /// @return List of delay import modules or error
    [[nodiscard]] Result<std::vector<DelayImportModule>, DelayImportError>
    parse_delay_imports(std::span<const u8> pe_data, const ParsedPe& pe, u64 base_address) noexcept;

    /// Resolve and patch all delay imports into guest memory
    /// @param pe_data Raw PE file data
    /// @param pe Parsed PE information
    /// @param base_address Guest address where PE is loaded
    /// @return Count of resolved imports or error
    [[nodiscard]] Result<usize, DelayImportError>
    resolve_delay_imports(std::span<const u8> pe_data, const ParsedPe& pe, u64 base_address) noexcept;

private:
    GuestMemory& m_memory;
    winapi::ThunkRegistry& m_thunks;

    static constexpr u64 ORDINAL_FLAG_64 = 0x8000000000000000ULL;
    static constexpr u32 ORDINAL_FLAG_32 = 0x80000000;
    static constexpr u32 TRAP_THUNK_ADDR = 0xDEAD0000;

    /// Read a null-terminated string from PE data
    [[nodiscard]] static Result<std::string, DelayImportError>
    read_string(std::span<const u8> pe_data, u32 file_offset, usize max_length = 256) noexcept;
};

} // namespace vitreo::pe
