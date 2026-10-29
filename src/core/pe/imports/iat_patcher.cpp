// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// PE Import Address Table patcher

#include "iat_patcher.hpp"

namespace vitreo::pe {

// ============================================================================
// Construction
// ============================================================================

IatPatcher::IatPatcher(GuestMemory& memory) noexcept
    : m_memory(memory) {}

// ============================================================================
// Slot Patching
// ============================================================================

Result<void, ImportError> IatPatcher::patch_slot(
    u64 iat_address,
    u64 thunk_address,
    bool is_64bit
) noexcept {
    if (is_64bit) {
        auto result = m_memory.write_u64(iat_address, thunk_address);
        if (!result) {
            return Result<void, ImportError>::err(ImportError::MemoryWriteFailed);
        }
    } else {
        auto result = m_memory.write_u32(iat_address, static_cast<u32>(thunk_address));
        if (!result) {
            return Result<void, ImportError>::err(ImportError::MemoryWriteFailed);
        }
    }
    
    return Result<void, ImportError>::ok();
}

// ============================================================================
// Main Patch Function
// ============================================================================

Result<PatchResult, ImportError> IatPatcher::patch(
    const ImportTable& imports,
    const ImportCallback& resolver
) noexcept {
    PatchResult result{};
    result.stats.modules_processed = 0;
    result.stats.imports_resolved = 0;
    result.stats.imports_unresolved = 0;
    
    for (const auto& module : imports.modules) {
        result.stats.modules_processed++;
        
        for (const auto& entry : module.imports) {
            // Resolve the import
            u64 thunk_address = 0;
            
            if (entry.is_ordinal) {
                thunk_address = resolver(module.dll_name, "", entry.ordinal);
            } else {
                thunk_address = resolver(module.dll_name, entry.function_name, 0);
            }
            
            if (thunk_address == 0) {
                // Unresolved import — zero out the IAT slot so that stale
                // "bound import" addresses from the original PE don't cause
                // the guest to jump into unmapped memory.
                (void)patch_slot(entry.iat_address, 0, imports.is_64bit);

                UnresolvedImport unresolved{};
                unresolved.dll_name = module.dll_name;
                unresolved.function_name = entry.function_name;
                unresolved.ordinal = entry.ordinal;
                result.unresolved.push_back(std::move(unresolved));
                result.stats.imports_unresolved++;
            } else {
                // Patch the IAT slot
                auto patch_result = patch_slot(
                    entry.iat_address,
                    thunk_address,
                    imports.is_64bit
                );
                
                if (!patch_result) {
                    return Result<PatchResult, ImportError>::err(patch_result.error());
                }
                
                result.stats.imports_resolved++;
            }
        }
    }
    
    return Result<PatchResult, ImportError>::ok(std::move(result));
}

// ============================================================================
// Free Functions
// ============================================================================

Result<PatchResult, ImportError> patch_iat(
    GuestMemory& memory,
    const ImportTable& imports,
    const ImportCallback& resolver
) noexcept {
    IatPatcher patcher(memory);
    return patcher.patch(imports, resolver);
}

Result<PatchResult, ImportError> resolve_and_patch_imports(
    GuestMemory& memory,
    const ParsedPe& pe,
    std::span<const u8> pe_data,
    u64 base_address,
    const ImportCallback& resolver
) noexcept {
    // Parse imports
    auto import_result = parse_imports(pe, pe_data, base_address);
    if (!import_result) {
        // No imports is okay - return empty result
        if (import_result.error() == ImportError::NoImports) {
            PatchResult result{};
            return Result<PatchResult, ImportError>::ok(std::move(result));
        }
        return Result<PatchResult, ImportError>::err(import_result.error());
    }
    
    // Patch IAT
    return patch_iat(memory, import_result.value(), resolver);
}

} // namespace vitreo::pe
