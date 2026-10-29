// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// PE delayed import processor

#include "delay_import.hpp"
#include <cstring>

namespace vitreo::pe {

// ============================================================================
// Error Messages
// ============================================================================

const char* delay_import_error_message(DelayImportError err) noexcept {
    switch (err) {
        case DelayImportError::Success:
            return "Success";
        case DelayImportError::NoDelayImports:
            return "No delay imports present";
        case DelayImportError::InvalidDirectory:
            return "Invalid delay import directory";
        case DelayImportError::InvalidDescriptor:
            return "Invalid delay import descriptor";
        case DelayImportError::InvalidThunkData:
            return "Invalid delay import thunk data";
        case DelayImportError::MemoryReadFailed:
            return "Failed to read memory";
        case DelayImportError::MemoryWriteFailed:
            return "Failed to write memory";
        case DelayImportError::StringReadFailed:
            return "Failed to read string";
    }
    return "Unknown error";
}

// ============================================================================
// Construction
// ============================================================================

DelayImportProcessor::DelayImportProcessor(GuestMemory& memory, winapi::ThunkRegistry& thunks)
    : m_memory(memory)
    , m_thunks(thunks)
{}

// ============================================================================
// Static Helpers
// ============================================================================

bool DelayImportProcessor::has_delay_imports(const ParsedPe& pe) noexcept {
    if (pe.num_data_directories <= DIR_DELAY_IMPORT) {
        return false;
    }
    return pe.data_directories[DIR_DELAY_IMPORT].VirtualAddress != 0 &&
           pe.data_directories[DIR_DELAY_IMPORT].Size != 0;
}

Result<std::string, DelayImportError> DelayImportProcessor::read_string(
    std::span<const u8> pe_data,
    u32 file_offset,
    usize max_length
) noexcept {
    if (file_offset >= pe_data.size()) {
        return Result<std::string, DelayImportError>::err(DelayImportError::StringReadFailed);
    }

    std::string result;
    result.reserve(64);

    for (usize i = 0; i < max_length && file_offset + i < pe_data.size(); ++i) {
        char c = static_cast<char>(pe_data[file_offset + i]);
        if (c == '\0') {
            return Result<std::string, DelayImportError>::ok(std::move(result));
        }
        result.push_back(c);
    }

    // No null terminator found within max_length
    return Result<std::string, DelayImportError>::err(DelayImportError::StringReadFailed);
}

// ============================================================================
// Parsing
// ============================================================================

Result<std::vector<DelayImportModule>, DelayImportError>
DelayImportProcessor::parse_delay_imports(
    std::span<const u8> pe_data,
    const ParsedPe& pe,
    u64 base_address
) noexcept {
    std::vector<DelayImportModule> modules;

    if (!has_delay_imports(pe)) {
        return Result<std::vector<DelayImportModule>, DelayImportError>::err(
            DelayImportError::NoDelayImports);
    }

    const auto& dir = pe.data_directories[DIR_DELAY_IMPORT];

    auto dir_offset_result = pe.rva_to_file_offset(dir.VirtualAddress);
    if (!dir_offset_result) {
        return Result<std::vector<DelayImportModule>, DelayImportError>::err(
            DelayImportError::InvalidDirectory);
    }

    u32 dir_offset = dir_offset_result.value();
    u32 current_offset = 0;

    // Walk delay import descriptors
    while (true) {
        if (dir_offset + current_offset + sizeof(ImgDelayDescr) > pe_data.size()) {
            break;
        }

        ImgDelayDescr desc{};
        std::memcpy(&desc, pe_data.data() + dir_offset + current_offset, sizeof(desc));

        // All-zero descriptor terminates the array
        if (desc.rvaDLLName == 0 && desc.rvaIAT == 0 && desc.rvaINT == 0) {
            break;
        }

        DelayImportModule module{};

        // Determine if addresses are VA-based (grAttrs == 0) or RVA-based (grAttrs == 1)
        bool is_va_based = (desc.grAttrs == 0);
        u64 image_base = pe.image_base;

        // Convert VA to RVA if needed
        u32 dll_name_rva = is_va_based
            ? static_cast<u32>(desc.rvaDLLName - image_base)
            : desc.rvaDLLName;
        u32 int_rva = is_va_based
            ? static_cast<u32>(desc.rvaINT - image_base)
            : desc.rvaINT;
        u32 iat_rva = is_va_based
            ? static_cast<u32>(desc.rvaIAT - image_base)
            : desc.rvaIAT;
        u32 hmod_rva = is_va_based
            ? static_cast<u32>(desc.rvaHmod - image_base)
            : desc.rvaHmod;

        // Read DLL name
        auto name_offset_result = pe.rva_to_file_offset(dll_name_rva);
        if (!name_offset_result) {
            return Result<std::vector<DelayImportModule>, DelayImportError>::err(
                DelayImportError::InvalidDescriptor);
        }

        auto name_result = read_string(pe_data, name_offset_result.value());
        if (!name_result) {
            return Result<std::vector<DelayImportModule>, DelayImportError>::err(
                DelayImportError::StringReadFailed);
        }
        module.dll_name = std::move(name_result.value());

        // Module handle guest address
        module.hmod_address = base_address + hmod_rva;

        // Convert INT RVA to file offset
        auto int_offset_result = pe.rva_to_file_offset(int_rva);
        if (!int_offset_result) {
            return Result<std::vector<DelayImportModule>, DelayImportError>::err(
                DelayImportError::InvalidThunkData);
        }
        u32 int_offset = int_offset_result.value();

        // Walk the INT (Import Name Table) array
        usize thunk_index = 0;
        while (true) {
            DelayImportEntry entry{};

            if (pe.is_64bit) {
                // 64-bit thunk
                usize offset = int_offset + thunk_index * sizeof(u64);
                if (offset + sizeof(u64) > pe_data.size()) {
                    break;
                }

                u64 thunk = 0;
                std::memcpy(&thunk, pe_data.data() + offset, sizeof(thunk));

                if (thunk == 0) {
                    break;  // End of imports
                }

                entry.iat_address = base_address + iat_rva + thunk_index * sizeof(u64);

                if (thunk & ORDINAL_FLAG_64) {
                    entry.is_ordinal = true;
                    entry.ordinal = static_cast<u16>(thunk & 0xFFFF);
                } else {
                    entry.is_ordinal = false;
                    u32 hint_name_rva = static_cast<u32>(thunk);

                    auto hint_offset_result = pe.rva_to_file_offset(hint_name_rva);
                    if (!hint_offset_result) {
                        return Result<std::vector<DelayImportModule>, DelayImportError>::err(
                            DelayImportError::InvalidThunkData);
                    }

                    u32 hint_offset = hint_offset_result.value();
                    if (hint_offset + sizeof(u16) > pe_data.size()) {
                        return Result<std::vector<DelayImportModule>, DelayImportError>::err(
                            DelayImportError::InvalidThunkData);
                    }

                    // Skip hint (2 bytes), read name
                    auto func_name_result = read_string(pe_data, hint_offset + sizeof(u16));
                    if (!func_name_result) {
                        return Result<std::vector<DelayImportModule>, DelayImportError>::err(
                            DelayImportError::StringReadFailed);
                    }
                    entry.function_name = std::move(func_name_result.value());
                    entry.ordinal = 0;
                }
            } else {
                // 32-bit thunk
                usize offset = int_offset + thunk_index * sizeof(u32);
                if (offset + sizeof(u32) > pe_data.size()) {
                    break;
                }

                u32 thunk = 0;
                std::memcpy(&thunk, pe_data.data() + offset, sizeof(thunk));

                if (thunk == 0) {
                    break;  // End of imports
                }

                entry.iat_address = base_address + iat_rva + thunk_index * sizeof(u32);

                if (thunk & ORDINAL_FLAG_32) {
                    entry.is_ordinal = true;
                    entry.ordinal = static_cast<u16>(thunk & 0xFFFF);
                } else {
                    entry.is_ordinal = false;

                    auto hint_offset_result = pe.rva_to_file_offset(thunk);
                    if (!hint_offset_result) {
                        return Result<std::vector<DelayImportModule>, DelayImportError>::err(
                            DelayImportError::InvalidThunkData);
                    }

                    u32 hint_offset = hint_offset_result.value();
                    if (hint_offset + sizeof(u16) > pe_data.size()) {
                        return Result<std::vector<DelayImportModule>, DelayImportError>::err(
                            DelayImportError::InvalidThunkData);
                    }

                    // Skip hint (2 bytes), read name
                    auto func_name_result = read_string(pe_data, hint_offset + sizeof(u16));
                    if (!func_name_result) {
                        return Result<std::vector<DelayImportModule>, DelayImportError>::err(
                            DelayImportError::StringReadFailed);
                    }
                    entry.function_name = std::move(func_name_result.value());
                    entry.ordinal = 0;
                }
            }

            module.entries.push_back(std::move(entry));
            thunk_index++;
        }

        modules.push_back(std::move(module));
        current_offset += sizeof(ImgDelayDescr);
    }

    return Result<std::vector<DelayImportModule>, DelayImportError>::ok(std::move(modules));
}

// ============================================================================
// Resolution
// ============================================================================

Result<usize, DelayImportError>
DelayImportProcessor::resolve_delay_imports(
    std::span<const u8> pe_data,
    const ParsedPe& pe,
    u64 base_address
) noexcept {
    auto parse_result = parse_delay_imports(pe_data, pe, base_address);
    if (!parse_result) {
        return Result<usize, DelayImportError>::err(parse_result.error());
    }

    auto& modules = parse_result.value();
    usize resolved_count = 0;

    for (auto& module : modules) {
        bool dll_known = m_thunks.has_dll(module.dll_name);

        for (auto& entry : module.entries) {
            u64 thunk_addr = TRAP_THUNK_ADDR;

            if (dll_known) {
                if (entry.is_ordinal) {
                    auto result = m_thunks.get_thunk_address(module.dll_name, entry.ordinal);
                    if (result) {
                        thunk_addr = m_thunks.guest_entry(result.value());
                    }
                } else {
                    auto result = m_thunks.get_thunk_address(module.dll_name, entry.function_name);
                    if (result) {
                        thunk_addr = m_thunks.guest_entry(result.value());
                    }
                }
            }

            // Write resolved address to IAT in guest memory
            VoidResult write_result = pe.is_64bit
                ? m_memory.write_u64(entry.iat_address, thunk_addr)
                : m_memory.write_u32(entry.iat_address, static_cast<u32>(thunk_addr));

            if (!write_result) {
                return Result<usize, DelayImportError>::err(DelayImportError::MemoryWriteFailed);
            }

            resolved_count++;
        }

        // Write a synthetic module handle to hmod_address
        // Use a non-zero sentinel so __HrLoadAllImportsForDll sees the DLL as loaded
        u64 synthetic_handle = dll_known ? (base_address + 0x10000) : 0;
        VoidResult hmod_result = pe.is_64bit
            ? m_memory.write_u64(module.hmod_address, synthetic_handle)
            : m_memory.write_u32(module.hmod_address, static_cast<u32>(synthetic_handle));

        if (!hmod_result) {
            return Result<usize, DelayImportError>::err(DelayImportError::MemoryWriteFailed);
        }
    }

    return Result<usize, DelayImportError>::ok(resolved_count);
}

} // namespace vitreo::pe
