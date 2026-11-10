// SPDX-License-Identifier: LGPL-2.1-only

/// @file dxil_subobjects.cpp
/// @brief The subobjects a DXIL library declares (DirectX-Specs
/// Raytracing.md, "Subobjects in DXIL libraries"; HLSL GlobalRootSignature,
/// LocalRootSignature, StateObjectConfig, SubobjectToExportsAssociation,
/// RaytracingShaderConfig, RaytracingPipelineConfig(1), TriangleHitGroup,
/// ProceduralPrimitiveHitGroup), read from the container's RDAT part.
///
/// The bitcode does not carry them (dxc 1.6 writes no dx.subobjects
/// metadata); RDAT does. Its layout, as read from the dxc 1.6 output of
/// tools/oracle_probe/d3d12_dxr_local (whose -Fc listings print each
/// subobject's fields) and a CONFIG1 / empty-association sample:
///   header: u32 version (0x10), u32 part count, u32 part offsets (from the
///           part data's start);
///   part:   u32 type, u32 size, then size bytes. Type 1 is the string
///           buffer (NUL-terminated, by byte offset), 2 the index arrays
///           (u32 words; an array at word w is a count then its elements),
///           5 raw bytes, 6 the subobject table; tables are u32 count,
///           u32 record stride, then the records;
///   subobject record (24 bytes or more): u32 D3D12_STATE_SUBOBJECT_TYPE,
///           u32 name, then by type: STATE_OBJECT_CONFIG flags; GLOBAL /
///           LOCAL_ROOT_SIGNATURE raw-bytes offset and size (a serialized
///           root signature); SUBOBJECT_TO_EXPORTS_ASSOCIATION the
///           subobject's name and an index array of export names;
///           RAYTRACING_SHADER_CONFIG payload and attribute bytes;
///           RAYTRACING_PIPELINE_CONFIG depth; CONFIG1 depth and flags;
///           HIT_GROUP type and the any-hit, closest-hit and intersection
///           names.

#include "dxil_container.hpp"
#include "dxil_raytracing.hpp"

#include <cstring>

namespace vitreo::graphics {

namespace {

constexpr u32 kRdatStrings = 1, kRdatIndices = 2, kRdatRawBytes = 5, kRdatSubobjects = 6;

struct Rdat {
    const u8* data{nullptr};
    usize size{0};
    const u8* strings{nullptr};
    usize strings_size{0};
    const u8* indices{nullptr};
    usize indices_size{0};
    const u8* raw{nullptr};
    usize raw_size{0};
    const u8* subobjects{nullptr};
    usize subobjects_size{0};

    [[nodiscard]] static bool word(const u8* p, usize size, usize at, u32& out) {
        if (at + 4 > size) return false;
        std::memcpy(&out, p + at, 4);
        return true;
    }
    [[nodiscard]] bool string(u32 offset, std::string& out) const {
        out.clear();
        if (offset >= strings_size) return false;
        for (usize k = offset; k < strings_size; ++k) {
            if (strings[k] == 0) return true;
            out.push_back(static_cast<char>(strings[k]));
        }
        return false;
    }
    [[nodiscard]] bool index_array(u32 at, std::vector<u32>& out) const {
        out.clear();
        u32 count = 0;
        if (!word(indices, indices_size, usize{at} * 4, count)) return false;
        for (u32 k = 0; k < count; ++k) {
            u32 v = 0;
            if (!word(indices, indices_size, (usize{at} + 1 + k) * 4, v)) return false;
            out.push_back(v);
        }
        return true;
    }
};

}  // namespace

bool dxil_library_subobjects(const u8* data, usize size, std::vector<DxilSubobject>& out) {
    out.clear();
    const DxilContainer container = parse_dxil_container(data, size);
    if (!container.ok) return false;
    const DxilContainerPart* part = container.part("RDAT");
    if (part == nullptr) return true;  // no runtime data: no subobjects
    Rdat r;
    r.data = data + part->offset;
    r.size = part->size;
    u32 version = 0, parts = 0;
    if (!Rdat::word(r.data, r.size, 0, version) || !Rdat::word(r.data, r.size, 4, parts)) return false;
    for (u32 k = 0; k < parts; ++k) {
        u32 offset = 0, type = 0, bytes = 0;
        if (!Rdat::word(r.data, r.size, 8 + usize{k} * 4, offset) || !Rdat::word(r.data, r.size, offset, type) ||
            !Rdat::word(r.data, r.size, usize{offset} + 4, bytes) || usize{offset} + 8 + bytes > r.size) {
            return false;
        }
        const u8* p = r.data + offset + 8;
        switch (type) {
        case kRdatStrings: r.strings = p, r.strings_size = bytes; break;
        case kRdatIndices: r.indices = p, r.indices_size = bytes; break;
        case kRdatRawBytes: r.raw = p, r.raw_size = bytes; break;
        case kRdatSubobjects: r.subobjects = p, r.subobjects_size = bytes; break;
        default: break;
        }
    }
    if (r.subobjects == nullptr) return true;
    u32 count = 0, stride = 0;
    if (!Rdat::word(r.subobjects, r.subobjects_size, 0, count) ||
        !Rdat::word(r.subobjects, r.subobjects_size, 4, stride) || stride < 24 ||
        8 + usize{count} * stride > r.subobjects_size) {
        return false;
    }
    for (u32 k = 0; k < count; ++k) {
        const u8* rec = r.subobjects + 8 + usize{k} * stride;
        u32 w[6];
        std::memcpy(w, rec, sizeof(w));
        DxilSubobject s;
        s.kind = w[0];
        if (!r.string(w[1], s.name)) return false;
        switch (s.kind) {
        case 0:  // STATE_OBJECT_CONFIG
            s.flags = w[2];
            break;
        case 1:  // GLOBAL_ROOT_SIGNATURE
        case 2:  // LOCAL_ROOT_SIGNATURE
            if (r.raw == nullptr || usize{w[2]} + w[3] > r.raw_size) return false;
            s.root_signature.assign(r.raw + w[2], r.raw + w[2] + w[3]);
            break;
        case 8: {  // SUBOBJECT_TO_EXPORTS_ASSOCIATION
            if (!r.string(w[2], s.subobject)) return false;
            std::vector<u32> names;
            if (!r.index_array(w[3], names)) return false;
            for (const u32 n : names) {
                std::string e;
                if (!r.string(n, e)) return false;
                s.exports.push_back(std::move(e));
            }
            break;
        }
        case 9:  // RAYTRACING_SHADER_CONFIG
            s.max_payload = w[2];
            s.max_attributes = w[3];
            break;
        case 10:  // RAYTRACING_PIPELINE_CONFIG
            s.max_recursion = w[2];
            break;
        case 12:  // RAYTRACING_PIPELINE_CONFIG1
            s.max_recursion = w[2];
            s.flags = w[3];
            break;
        case 11:  // HIT_GROUP
            s.hit_group_type = w[2];
            if (!r.string(w[3], s.any_hit) || !r.string(w[4], s.closest_hit) || !r.string(w[5], s.intersection)) {
                return false;
            }
            break;
        default:
            return false;  // not a subobject a library can declare
        }
        out.push_back(std::move(s));
    }
    return true;
}

}  // namespace vitreo::graphics
