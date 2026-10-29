#pragma once

/// @file pe_loader.hpp
/// @brief PE Loader Integration (PE-005)
///
/// Orchestrates the full PE loading pipeline: parse, map sections,
/// apply relocations, and resolve imports.

#include <vitreo/common.hpp>
#include "core/pe/mapper/section_mapper.hpp"
#include "core/pe/imports/import_resolver.hpp"

#include <span>
#include <functional>
#include <string>
#include <vector>

namespace vitreo::winapi {
class ThunkRegistry;
}

namespace vitreo::pe {

// ============================================================================
// Loaded Image
// ============================================================================

/// Result of successfully loading a PE into guest memory
struct LoadedImage {
    GuestAddr entry_point;              ///< Guest address of entry point
    GuestAddr image_base;               ///< Guest base address of loaded image
    GuestAddr image_end;                ///< First address past the image
    u32 bitness;                        ///< 32 or 64
    std::vector<MappedSection> sections; ///< Mapped section descriptors
    /// Page-aligned size of the separately mapped header region. Retained so
    /// the image can be unmapped later: unmap_region requires the exact base
    /// and size each region was mapped with.
    usize header_size{0};
};

// ============================================================================
// PE Loader
// ============================================================================

/// Orchestrates the complete PE loading pipeline
class PeLoader {
public:
    using PreImportCallback =
        std::function<void(GuestAddr image_base, usize image_size,
                           bool is_64bit)>;

    /// Construct a loader with guest memory and thunk registry
    /// @param memory Guest memory to load into
    /// @param thunks Thunk registry for import resolution
    PeLoader(GuestMemory& memory, winapi::ThunkRegistry& thunks);

    /// Load a PE file into guest memory
    /// @param pe_data Raw PE file bytes
    /// @param preferred_base Preferred load address (0 = use PE's ImageBase)
    /// @param extra_resolver Optional callback consulted FIRST for each import.
    ///        If it returns 0, the loader falls back to the ThunkRegistry.
    ///        Use this to add companion-DLL / on-disk loading semantics
    ///        without coupling the PE loader to higher-level subsystems.
    /// @param resolve_imports When false (DONT_RESOLVE_DLL_REFERENCES / LoadLibrary
    ///        of a non-DLL executable), map+relocate only; leave the IAT untouched
    ///        and do not load referenced modules (Learn LoadLibraryExW).
    /// @return LoadedImage on success, Error on failure
    [[nodiscard]] Result<LoadedImage, Error> load(
        std::span<const u8> pe_data,
        GuestAddr preferred_base = 0,
        ImportCallback extra_resolver = {},
        PreImportCallback pre_import_callback = {},
        bool resolve_imports = true);

    /// Unmap an image previously produced by load().
    ///
    /// Reverses the section and header mappings so the same guest range can be
    /// mapped again. This is what makes a fresh load possible after Windows
    /// semantics say a DLL was unloaded: the replacement image must start from
    /// its on-disk initial data, not from whatever the previous instance's
    /// DLL_PROCESS_DETACH left behind.
    ///
    /// Best-effort: regions that are already gone are skipped.
    void unload(const LoadedImage& image);

private:
    GuestMemory& m_memory;
    winapi::ThunkRegistry& m_thunks;
};

} // namespace vitreo::pe
