// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file metal_transcode.hpp
/// @brief GPU-timeline transcoding of guest texel encodings to backend
/// storage (metal_transcode.mm).

#include <vitreo/common.hpp>

#include "graphics/contracts/format_converter.hpp"

namespace vitreo::graphics {

/// A box of pixels: guest rows / slices at in_* (decode) or out_* (encode),
/// storage texels (4 bytes each, tightly placed from pixel 0) at the other.
struct Transcode422Region {
    u32 width{0};
    u32 height{0};
    u32 depth{1};
    u64 in_row{0};
    u64 in_slice{0};
    u64 out_row{0};
    u64 out_slice{0};
};

/// Records a kernel that decodes guest 4:2:2 blocks from @p src into RGBA8
/// storage texels at @p dst (@p decode), or encodes storage texels back into
/// blocks. @p src / @p dst are id<MTLBuffer> handles and byte offsets.
[[nodiscard]] bool record_transcode_422(PixelFormat format, bool decode, void* src,
                                        usize src_offset, void* dst, usize dst_offset,
                                        const Transcode422Region& region);

}  // namespace vitreo::graphics
