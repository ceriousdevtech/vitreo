// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS
// Image base selection against the guest address space's reserved holes

#pragma once

#include "../parser/pe_parser.hpp"
#include "core/memory/guest_memory.hpp"

namespace vitreo::pe {

// ============================================================================
// Image Placement
// ============================================================================

/// Outcome of choosing where an image may be mapped.
///
/// Under identity mapping (the Intel native backend) the guest shares its
/// address space with the host process, so a preferred ImageBase can still
/// belong to the host (dyld, leftover stack if relocation failed). Windows
/// already permits a relocatable image to load elsewhere, so the honest
/// response is to rebase, not to fail. An image with no relocation table has
/// no such freedom, and pretending otherwise would map it where its absolute
/// addresses do not point.
struct ImagePlacement {
    /// Chosen guest base; 0 when the image cannot be placed.
    GuestAddr base = 0;
    /// True when @ref base differs from the requested base.
    bool relocated = false;
    /// nullptr on success; otherwise why no base exists. Static storage.
    const char* failure = nullptr;

    [[nodiscard]] bool ok() const noexcept { return failure == nullptr; }
};

/// Pick the guest base for @p pe.
///
/// @param memory      Guest memory whose reserved holes constrain placement.
/// @param pe          Parsed image (SizeOfImage, ImageBase, reloc table).
/// @param preferred   Caller's requested base, or 0 to use `pe.image_base`.
///
/// With no reserved holes recorded — every translated (ARM) run — this returns
/// the requested base unchanged, so the existing linear-reservation path is
/// unaffected.
[[nodiscard]] ImagePlacement select_image_base(
    const GuestMemory& memory,
    const ParsedPe& pe,
    GuestAddr preferred = 0) noexcept;

} // namespace vitreo::pe
