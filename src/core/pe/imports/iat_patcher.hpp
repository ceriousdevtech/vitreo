// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// PE Import Address Table patcher

#pragma once

#include "import_resolver.hpp"
#include "core/memory/guest_memory.hpp"
#include <vitreo/result.hpp>

namespace vitreo::pe {

// ============================================================================
// Patch Results
// ============================================================================

/// Statistics from IAT patching
struct PatchStats {
    u32 modules_processed;      ///< Number of DLLs processed
    u32 imports_resolved;       ///< Number of imports successfully resolved
    u32 imports_unresolved;     ///< Number of imports that couldn't be resolved
};

/// Result of patching the IAT
struct PatchResult {
    PatchStats stats;
    std::vector<UnresolvedImport> unresolved;
};

// ============================================================================
// IAT Patcher
// ============================================================================

/// Patches Import Address Table with resolved addresses
class IatPatcher {
public:
    /// Construct a patcher with guest memory
    explicit IatPatcher(GuestMemory& memory) noexcept;
    
    /// Patch IAT using a callback to resolve imports
    /// @param imports Parsed import table
    /// @param resolver Callback that returns thunk address (0 if unresolved)
    /// @return Patch statistics and list of unresolved imports
    [[nodiscard]] Result<PatchResult, ImportError> patch(
        const ImportTable& imports,
        const ImportCallback& resolver
    ) noexcept;
    
    /// Patch a single IAT slot with an address
    /// @param iat_address Guest address of IAT slot
    /// @param thunk_address Address to write
    /// @param is_64bit True for 64-bit PE
    /// @return Success or error
    [[nodiscard]] Result<void, ImportError> patch_slot(
        u64 iat_address,
        u64 thunk_address,
        bool is_64bit
    ) noexcept;
    
private:
    GuestMemory& m_memory;
};

// ============================================================================
// Free Functions
// ============================================================================

/// Patch IAT with resolved thunk addresses
[[nodiscard]] Result<PatchResult, ImportError> patch_iat(
    GuestMemory& memory,
    const ImportTable& imports,
    const ImportCallback& resolver
) noexcept;

/// Parse imports and patch IAT in one step
[[nodiscard]] Result<PatchResult, ImportError> resolve_and_patch_imports(
    GuestMemory& memory,
    const ParsedPe& pe,
    std::span<const u8> pe_data,
    u64 base_address,
    const ImportCallback& resolver
) noexcept;

} // namespace vitreo::pe
