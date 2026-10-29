// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// On-disk, section-aligned image layouts that back SEC_IMAGE mappings.

#pragma once

#include <vitreo/types.hpp>

#include <span>
#include <string>

namespace vitreo::pe {

/// One raw-data copy of the image layout: @p size bytes from file offset
/// @p file_offset land at image offset @p rva. Copies apply in order, so a
/// later one overwrites an earlier one where they overlap; every byte no copy
/// covers is zero.
struct ImageLayoutCopy {
    u32 rva;
    usize file_offset;
    usize size;
};

/// Open (creating on first use) a read-only file whose bytes [0, image_size)
/// are exactly the laid-out image described by @p copies over @p pe_data.
///
/// The file is content-addressed (SHA-256 of the layout version, image size
/// and @p pe_data) under the image layout cache directory, so every process
/// that maps the same PE maps the same file and shares its clean pages
/// through the host page cache. Returns -1 when caching is disabled, the
/// directory is unavailable, or any host I/O fails; the caller then lays the
/// image out in anonymous memory. The caller owns (and closes) the fd.
[[nodiscard]] int open_image_layout_file(std::span<const u8> pe_data,
                                         u32 image_size,
                                         std::span<const ImageLayoutCopy> copies);

/// Directory holding image layout files: $VITREO_IMAGE_LAYOUT_CACHE_DIR, else
/// $HOME/Library/Caches/Vitreo/image-layouts; empty when neither resolves or
/// VITREO_IMAGE_FILE_BACKING=0 disables file-backed images.
[[nodiscard]] std::string image_layout_cache_dir();

}  // namespace vitreo::pe
