// SPDX-License-Identifier: LGPL-2.1-only
/// @file dxil_container.cpp
/// @brief DXIL container parts (DXIL.rst, "DXIL Container Format").

#include "dxil_container.hpp"

#include <cstring>

namespace vitreo::graphics {

namespace {

u32 rd32(const u8* p) {
    u32 v = 0;
    std::memcpy(&v, p, 4);
    return v;
}

// DxilContainerHeader: FourCC 4, Digest 16, Major 2, Minor 2, Size 4,
// PartCount 4, then PartOffset[PartCount].
constexpr usize kHeaderSize = 32;

}  // namespace

const DxilContainerPart* DxilContainer::part(const char fourcc[4]) const {
    const u32 code = static_cast<u32>(static_cast<u8>(fourcc[0])) |
                     static_cast<u32>(static_cast<u8>(fourcc[1])) << 8 |
                     static_cast<u32>(static_cast<u8>(fourcc[2])) << 16 |
                     static_cast<u32>(static_cast<u8>(fourcc[3])) << 24;
    for (const auto& p : parts) {
        if (p.fourcc == code) return &p;
    }
    return nullptr;
}

DxilContainer parse_dxil_container(const u8* data, usize size) {
    DxilContainer out;
    if (data == nullptr || size < kHeaderSize || std::memcmp(data, "DXBC", 4) != 0) {
        out.error = "not a DXBC/DXIL container";
        return out;
    }
    const u32 total = rd32(data + 24);
    const u32 count = rd32(data + 28);
    if (total > size || total < kHeaderSize || count > (total - kHeaderSize) / 4u) {
        out.error = "container size or part count past the data";
        return out;
    }
    for (u32 i = 0; i < count; ++i) {
        const u32 at = rd32(data + kHeaderSize + i * 4u);
        if (at > total || total - at < 8u) {
            out.error = "part header past the container";
            return out;
        }
        DxilContainerPart part;
        part.fourcc = rd32(data + at);
        part.size = rd32(data + at + 4);
        part.offset = at + 8u;
        if (part.size > total - part.offset) {
            out.error = "part data past the container";
            return out;
        }
        out.parts.push_back(part);
    }
    const DxilContainerPart* program = out.part("DXIL");
    if (program == nullptr) {
        out.error = "container has no DXIL part";
        return out;
    }
    // DxilProgramHeader: ProgramVersion, SizeInUint32, DxilMagic,
    // DxilVersion, BitcodeOffset (from DxilMagic), BitcodeSize.
    if (program->size < 24) {
        out.error = "DXIL part shorter than its program header";
        return out;
    }
    const u8* p = data + program->offset;
    out.program_version = rd32(p);
    const u32 words = rd32(p + 4);
    if (rd32(p + 8) != dxil_fourcc("DXIL")) {
        out.error = "DXIL program header magic is not DXIL";
        return out;
    }
    out.dxil_version = rd32(p + 12);
    const u32 bitcode_offset = rd32(p + 16);
    const u32 bitcode_size = rd32(p + 20);
    if (static_cast<u64>(words) * 4u > program->size ||
        8ull + bitcode_offset + bitcode_size > program->size) {
        out.error = "DXIL bitcode past its part";
        return out;
    }
    out.bitcode = p + 8 + bitcode_offset;
    out.bitcode_size = bitcode_size;
    if (const DxilContainerPart* sfi0 = out.part("SFI0"); sfi0 != nullptr && sfi0->size >= 8) {
        std::memcpy(&out.feature_flags, data + sfi0->offset, 8);
    }
    out.ok = true;
    return out;
}

bool dxil_container_present(const u8* data, usize size) {
    if (data == nullptr || size < kHeaderSize || std::memcmp(data, "DXBC", 4) != 0) return false;
    const u32 total = rd32(data + 24);
    const u32 count = rd32(data + 28);
    if (total > size || count > (total - kHeaderSize) / 4u) return false;
    for (u32 i = 0; i < count; ++i) {
        const u32 at = rd32(data + kHeaderSize + i * 4u);
        if (at <= total - 8u && rd32(data + at) == dxil_fourcc("DXIL")) return true;
    }
    return false;
}

}  // namespace vitreo::graphics
