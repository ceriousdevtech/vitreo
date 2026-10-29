// SPDX-License-Identifier: LGPL-2.1-only
#pragma once
#include <vitreo/common.hpp>
#include <span>
#include <vector>
namespace vitreo::graphics::png_decode {
// File-format samples, not any Windows API's converted pixel format.
struct Header {u32 width{},height{};u8 depth{},color{},interlace{};};
struct Pixels {usize stride{};std::vector<u8> rows;};
bool inflate_all(std::span<const u8>,std::vector<u8>&,usize expected=0);
bool samples(const Header&,std::span<const u8> compressed,Pixels&);
}
