// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// ModR/M and SIB byte decoding

#pragma once

#include "decoder.hpp"

namespace vitreo::cpu {

/// Decode ModR/M byte and optional SIB byte
/// @param reader Byte reader for fetching additional bytes
/// @param mode Current decode mode (16/32/64-bit)
/// @param prefix Active prefixes
/// @param is_64bit_operand Whether operand is 64-bit
/// @param modrm The ModR/M byte value
/// @param rm_operand [out] Decoded r/m operand
/// @param reg_field [out] The reg field value (with REX.R applied)
/// @return Number of additional bytes consumed, or -1 on error
[[nodiscard]] i8 decode_modrm(
    ByteReader& reader,
    DecodeMode mode,
    const Prefixes& prefix,
    bool is_64bit_operand,
    u8 modrm,
    Operand& rm_operand,
    u8& reg_field,
    bool vsib = false
) noexcept;

/// Get the default operand size for the current mode
[[nodiscard]] u8 default_operand_size(DecodeMode mode, const Prefixes& prefix) noexcept;

} // namespace vitreo::cpu
