// SPDX-License-Identifier: LGPL-2.1-only
#pragma once
/// @file dxil_container.hpp
/// @brief The DXIL container: its parts and the DXIL program header.
///
/// DXIL.rst "DXIL Container Format": a DxilContainerHeader ("DXBC", the
/// 16-byte digest, version, size, part count, part offsets) followed by
/// DxilPartHeader {FourCC, size} parts. The program part ("DXIL") starts
/// with DxilProgramHeader {ProgramVersion, SizeInUint32, "DXIL", DxilVersion,
/// BitcodeOffset (from DxilMagic), BitcodeSize}. The other parts a compiler
/// emits are PSV0 (runtime information), ISG1 / OSG1 / PSG1 (signatures in
/// the DXBC v1 element layout dxbc_parser.hpp reads), RTS0 (a root
/// signature), SFI0 (feature flags), HASH, ILDB / ILDN / STAT (debug and
/// statistics); the translation needs only DXIL and the signatures.

#include "shader_types.hpp"

#include <string>
#include <vector>

namespace vitreo::graphics {

struct DxilContainerPart {
    u32 fourcc{0};
    u32 offset{0};  ///< Offset of the part data in the container
    u32 size{0};
};

struct DxilContainer {
    bool ok{false};
    std::string error;
    std::vector<DxilContainerPart> parts;
    /// DxilProgramHeader::ProgramVersion: bits 16..31 the program kind
    /// (D3D12_SHADER_VERSION_TYPE: 0 pixel, 1 vertex, 2 geometry, 3 hull,
    /// 4 domain, 5 compute, 6 library, 13 mesh, 14 amplification), bits 4..7
    /// the major and 0..3 the minor shader model.
    u32 program_version{0};
    u32 dxil_version{0};
    const u8* bitcode{nullptr};
    u32 bitcode_size{0};
    /// SFI0's feature-flag word (D3D_SHADER_REQUIRES_*), 0 without one.
    u64 feature_flags{0};

    [[nodiscard]] u32 program_kind() const { return program_version >> 16; }
    [[nodiscard]] u32 major() const { return (program_version >> 4) & 0xFu; }
    [[nodiscard]] u32 minor() const { return program_version & 0xFu; }
    [[nodiscard]] const DxilContainerPart* part(const char fourcc[4]) const;
};

[[nodiscard]] constexpr u32 dxil_fourcc(const char c[5]) {
    return static_cast<u32>(static_cast<u8>(c[0])) |
           static_cast<u32>(static_cast<u8>(c[1])) << 8 |
           static_cast<u32>(static_cast<u8>(c[2])) << 16 |
           static_cast<u32>(static_cast<u8>(c[3])) << 24;
}

/// Parses the container at @p data. ok is false when it is not a whole
/// container (bad magic, a part past ContainerSizeInBytes), when there is no
/// DXIL program part, or when the program header is malformed.
[[nodiscard]] DxilContainer parse_dxil_container(const u8* data, usize size);

/// True when @p data is a DXBC-format container holding a "DXIL" part.
[[nodiscard]] bool dxil_container_present(const u8* data, usize size);

}  // namespace vitreo::graphics
