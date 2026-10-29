#pragma once
/// @file lcmap.hpp
/// @brief Shared LCMapStringW / LCMapStringEx mapping and sort-key logic.

#include <vitreo/common.hpp>
#include <vector>

namespace vitreo {
class GuestMemory;
}

namespace vitreo::winapi {

/// Map or sort-key a UTF-16 guest string per Learn LCMapStringEx dwMapFlags.
[[nodiscard]] i32 lcmap_string_w(GuestMemory& memory, u32 flags, GuestAddr src,
                                 i32 cch_src, GuestAddr dst, i32 cch_dest);

/// Host-buffer form used by LCMapStringA after ACP widen / before narrow.
[[nodiscard]] i32 lcmap_units(u32 flags, const u16* src, usize src_len,
                              bool cch_src_was_minus_one,
                              std::vector<u16>* mapped,
                              std::vector<u8>* sort_key);

} // namespace vitreo::winapi
