// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// PE import resolver

#include "import_resolver.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace vitreo::pe {

// VITREO_LOG_IMPORT_RESOLUTION: read once, not per call (getenv scans the
// environment block on every call).
static inline bool import_resolution_logging_enabled() {
    static const bool enabled =
        std::getenv("VITREO_LOG_IMPORT_RESOLUTION") != nullptr;
    return enabled;
}

// ============================================================================
// Error Messages
// ============================================================================

const char* import_error_message(ImportError err) noexcept {
    switch (err) {
        case ImportError::Success:
            return "Success";
        case ImportError::NoImports:
            return "No imports present";
        case ImportError::InvalidImportDirectory:
            return "Invalid import directory";
        case ImportError::InvalidDescriptor:
            return "Invalid import descriptor";
        case ImportError::InvalidThunkData:
            return "Invalid thunk data";
        case ImportError::MemoryReadFailed:
            return "Failed to read memory";
        case ImportError::MemoryWriteFailed:
            return "Failed to write memory";
        case ImportError::StringReadFailed:
            return "Failed to read string";
    }
    return "Unknown error";
}

// ============================================================================
// Static Helpers
// ============================================================================

bool ImportResolver::has_imports(const ParsedPe& pe) noexcept {
    return pe.has_imports();
}

// ============================================================================
// String Reading
// ============================================================================

Result<std::string, ImportError> ImportResolver::read_string(
    std::span<const u8> pe_data,
    u32 file_offset,
    usize max_length
) noexcept {
    if (file_offset >= pe_data.size()) {
        return Result<std::string, ImportError>::err(ImportError::StringReadFailed);
    }
    
    std::string result;
    result.reserve(64);
    
    for (usize i = 0; i < max_length && file_offset + i < pe_data.size(); ++i) {
        char c = static_cast<char>(pe_data[file_offset + i]);
        if (c == '\0') {
            return Result<std::string, ImportError>::ok(std::move(result));
        }
        result.push_back(c);
    }
    
    // No null terminator found within max_length
    return Result<std::string, ImportError>::err(ImportError::StringReadFailed);
}

// ============================================================================
// Module Parsing
// ============================================================================

Result<ImportModule, ImportError> ImportResolver::parse_module(
    const ImportDescriptor& desc,
    std::span<const u8> pe_data,
    const ParsedPe& pe,
    u64 base_address
) noexcept {
    ImportModule module{};
    
    // Get DLL name
    auto name_offset_result = pe.rva_to_file_offset(desc.Name);
    if (!name_offset_result) {
        return Result<ImportModule, ImportError>::err(ImportError::InvalidDescriptor);
    }
    
    auto name_result = read_string(pe_data, name_offset_result.value());
    if (!name_result) {
        if (import_resolution_logging_enabled()) {
            std::fprintf(stderr,
                         "[pe] import-parse DLL name read failed: "
                         "name_rva=0x%x file_offset=0x%x\n",
                         desc.Name, name_offset_result.value());
        }
        return Result<ImportModule, ImportError>::err(ImportError::StringReadFailed);
    }
    module.dll_name = std::move(name_result.value());
    
    // Get IAT RVA (FirstThunk is the IAT, OriginalFirstThunk is the INT)
    u32 iat_rva = desc.FirstThunk;
    u32 int_rva = desc.OriginalFirstThunk;
    
    // If OriginalFirstThunk is 0, use FirstThunk for both
    if (int_rva == 0) {
        int_rva = iat_rva;
    }
    
    // Convert to file offsets
    auto int_offset_result = pe.rva_to_file_offset(int_rva);
    if (!int_offset_result) {
        return Result<ImportModule, ImportError>::err(ImportError::InvalidThunkData);
    }
    u32 int_offset = int_offset_result.value();
    
    // Walk the thunk array
    usize thunk_index = 0;
    while (true) {
        ImportEntry entry{};
        
        if (pe.is_64bit) {
            // 64-bit thunk
            usize offset = int_offset + thunk_index * sizeof(u64);
            if (offset + sizeof(u64) > pe_data.size()) {
                break;
            }
            
            u64 thunk;
            std::memcpy(&thunk, pe_data.data() + offset, sizeof(thunk));
            
            if (thunk == 0) {
                break;  // End of imports
            }
            
            entry.iat_address = base_address + iat_rva + thunk_index * sizeof(u64);
            
            if (thunk & ORDINAL_FLAG_64) {
                // Import by ordinal
                entry.is_ordinal = true;
                entry.ordinal = static_cast<u16>(thunk & 0xFFFF);
            } else {
                // Import by name
                entry.is_ordinal = false;
                u32 hint_name_rva = static_cast<u32>(thunk);
                
                auto hint_offset_result = pe.rva_to_file_offset(hint_name_rva);
                if (!hint_offset_result) {
                    return Result<ImportModule, ImportError>::err(ImportError::InvalidThunkData);
                }
                
                u32 hint_offset = hint_offset_result.value();
                if (hint_offset + sizeof(u16) > pe_data.size()) {
                    return Result<ImportModule, ImportError>::err(ImportError::InvalidThunkData);
                }
                
                std::memcpy(&entry.hint, pe_data.data() + hint_offset, sizeof(u16));
                
                auto func_name_result = read_string(pe_data, hint_offset + sizeof(u16));
                if (!func_name_result) {
                    if (import_resolution_logging_enabled()) {
                        std::fprintf(stderr,
                                     "[pe] import-parse function read failed: "
                                     "dll=\"%s\" thunk=%zu hint_rva=0x%x "
                                     "file_offset=0x%x bitness=64\n",
                                     module.dll_name.c_str(), thunk_index,
                                     hint_name_rva, hint_offset);
                    }
                    return Result<ImportModule, ImportError>::err(ImportError::StringReadFailed);
                }
                entry.function_name = std::move(func_name_result.value());
            }
        } else {
            // 32-bit thunk
            usize offset = int_offset + thunk_index * sizeof(u32);
            if (offset + sizeof(u32) > pe_data.size()) {
                break;
            }
            
            u32 thunk;
            std::memcpy(&thunk, pe_data.data() + offset, sizeof(thunk));
            
            if (thunk == 0) {
                break;  // End of imports
            }
            
            entry.iat_address = base_address + iat_rva + thunk_index * sizeof(u32);
            
            if (thunk & ORDINAL_FLAG_32) {
                // Import by ordinal
                entry.is_ordinal = true;
                entry.ordinal = static_cast<u16>(thunk & 0xFFFF);
            } else {
                // Import by name
                entry.is_ordinal = false;
                
                auto hint_offset_result = pe.rva_to_file_offset(thunk);
                if (!hint_offset_result) {
                    return Result<ImportModule, ImportError>::err(ImportError::InvalidThunkData);
                }
                
                u32 hint_offset = hint_offset_result.value();
                if (hint_offset + sizeof(u16) > pe_data.size()) {
                    return Result<ImportModule, ImportError>::err(ImportError::InvalidThunkData);
                }
                
                std::memcpy(&entry.hint, pe_data.data() + hint_offset, sizeof(u16));
                
                auto func_name_result = read_string(pe_data, hint_offset + sizeof(u16));
                if (!func_name_result) {
                    if (import_resolution_logging_enabled()) {
                        std::fprintf(stderr,
                                     "[pe] import-parse function read failed: "
                                     "dll=\"%s\" thunk=%zu hint_rva=0x%x "
                                     "file_offset=0x%x bitness=32\n",
                                     module.dll_name.c_str(), thunk_index,
                                     thunk, hint_offset);
                    }
                    return Result<ImportModule, ImportError>::err(ImportError::StringReadFailed);
                }
                entry.function_name = std::move(func_name_result.value());
            }
        }
        
        module.imports.push_back(std::move(entry));
        thunk_index++;
    }
    
    return Result<ImportModule, ImportError>::ok(std::move(module));
}

// ============================================================================
// Main Parse Function
// ============================================================================

Result<ImportTable, ImportError> ImportResolver::parse(
    const ParsedPe& pe,
    std::span<const u8> pe_data,
    u64 base_address
) noexcept {
    ImportTable table{};
    table.is_64bit = pe.is_64bit;
    table.total_imports = 0;
    
    if (!pe.has_imports()) {
        return Result<ImportTable, ImportError>::err(ImportError::NoImports);
    }
    
    const auto& import_dir = pe.data_directories[DIR_IMPORT];
    
    auto import_offset_result = pe.rva_to_file_offset(import_dir.VirtualAddress);
    if (!import_offset_result) {
        return Result<ImportTable, ImportError>::err(ImportError::InvalidImportDirectory);
    }
    
    u32 import_offset = import_offset_result.value();
    u32 current_offset = 0;
    
    // Walk import descriptors
    while (true) {
        if (import_offset + current_offset + sizeof(ImportDescriptor) > pe_data.size()) {
            break;
        }
        
        ImportDescriptor desc;
        std::memcpy(&desc, pe_data.data() + import_offset + current_offset, sizeof(desc));
        
        // Check for null terminator
        if (desc.OriginalFirstThunk == 0 && desc.FirstThunk == 0) {
            break;
        }
        
        auto module_result = parse_module(desc, pe_data, pe, base_address);
        if (!module_result) {
            return Result<ImportTable, ImportError>::err(module_result.error());
        }
        
        auto& module = module_result.value();
        table.total_imports += static_cast<u32>(module.imports.size());
        table.modules.push_back(std::move(module));
        
        current_offset += sizeof(ImportDescriptor);
    }
    
    return Result<ImportTable, ImportError>::ok(std::move(table));
}

// ============================================================================
// Free Function
// ============================================================================

Result<ImportTable, ImportError> parse_imports(
    const ParsedPe& pe,
    std::span<const u8> pe_data,
    u64 base_address
) noexcept {
    ImportResolver resolver;
    return resolver.parse(pe, pe_data, base_address);
}

} // namespace vitreo::pe
