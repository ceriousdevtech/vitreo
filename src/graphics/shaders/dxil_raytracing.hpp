// SPDX-License-Identifier: LGPL-2.1-only
#pragma once
/// @file dxil_raytracing.hpp
/// @brief A DXR raytracing pipeline (DirectX-Specs Raytracing.md, DXIL.rst
/// shader model 6.3 libraries) lowered to ONE shader model 5.1 compute
/// program -- a megakernel -- that the DXBC translator compiles like any
/// other (dxil_lower_rtpipe.inc).
///
/// Design: one compute thread per DispatchRays index (8x8x1 groups). Every
/// raygen, miss, closest-hit, any-hit, intersection and callable function is
/// lowered as a DXBC subroutine (label / call); TraceRay is an inline ray
/// query (the Vitreo ray-query instruction, i.e. Metal intersection_query)
/// whose loop calls the any-hit and intersection subroutines its candidates
/// name through the hit-group shader record, then the closest-hit or miss
/// subroutine; CallShader calls a callable through its record. Shader
/// records are read from the four shader tables, bound as raw buffers.
/// DXBC (and SPIR-V) has no recursion, so each function a level can reach
/// is lowered once per level: level 0 is the raygen shader, a TraceRay or
/// CallShader at level k runs level k + 1, up to max_level. Every level has
/// its own payload / attribute arrays and ray query.
///
/// Reserved bindings, register space kDxilRtSpace: t0..t3 the raygen, miss,
/// hit-group and callable tables (raw, at each table's start), b0 the
/// dispatch constants (kRtConst*), t(kRtCompanionBase + ...) the raw views
/// of the acceleration-structure SRVs' own VAs, one per element in
/// declaration order (the Vitreo blob: its instance descriptions give
/// InstanceContributionToHitGroupIndex).
///
/// Local root arguments (Raytracing.md, "Shader record"; the local root
/// signature of a record's export or hit group lays them out after the
/// 32-byte identifier, each at its own alignment):
///  - 32-bit constants are read from the record itself;
///  - a root CBV / SRV / UAV (a GPU VA in the record) reads element j of a
///    reserved array of kRtLocalViewSlots views of the library resource it
///    binds, at register kRtLocalViewBase + k * kRtLocalViewSlots of its
///    class (k: DxilRtProgram::local_views). DispatchRays resolves each
///    record's VAs on the CPU, binds every distinct one as an element (0
///    stays null) and writes j into the translation buffer t4: word
///    (record * local_views.size() + k), record counting the raygen
///    record, then the miss, hit-group and callable records (bases in
///    kRtConstRecordBase*);
///  - a descriptor table (a GPU descriptor handle in the record) indexes the
///    bound shader-visible heap in place: the resource becomes an SM 6.6
///    heap-indexed one (graphics::kDxbcResourceHeapSpace /
///    kDxbcSamplerHeapSpace) at slot (handle - heap start) / increment +
///    the range's offset in the table + the register's offset in the range;
///  - a static sampler is s(kRtLocalSamplerBase + n), n the frontend's
///    index of that (root signature, sampler).
/// Shader identifiers: bytes 8..11 hold the index the megakernel switches
/// on (DxilRtProgram::indices).

#include "shader_types.hpp"

#include <map>
#include <string>
#include <vector>

namespace vitreo::graphics {

inline constexpr u32 kDxilRtSpace = 0xFFFFFFE0u;
inline constexpr u32 kRtCompanionBase = 16;
/// t4: the local root descriptor translation buffer (see above).
inline constexpr u32 kRtTranslationRegister = 4;
inline constexpr u32 kRtLocalViewBase = 0x100000u;
inline constexpr u32 kRtLocalViewSlots = 4096;
inline constexpr u32 kRtLocalSamplerBase = 0x100000u;
/// Dispatch constants (b0, space kDxilRtSpace), 32-bit words.
enum : u32 {
    kRtConstWidth = 0, kRtConstHeight = 1, kRtConstDepth = 2,
    kRtConstMissStride = 4, kRtConstHitStride = 5, kRtConstCallableStride = 6,
    kRtConstMissCount = 8, kRtConstHitCount = 9, kRtConstCallableCount = 10,
    /// The first record of each table in the translation buffer's order.
    kRtConstRecordBaseRaygen = 12, kRtConstRecordBaseMiss = 13, kRtConstRecordBaseHit = 14,
    kRtConstRecordBaseCallable = 15,
    /// Low words of the bound CBV/SRV/UAV and sampler heaps' GPU starts, and
    /// their handle increments.
    kRtConstViewHeap = 16, kRtConstSamplerHeap = 17, kRtConstViewIncrement = 18,
    kRtConstSamplerIncrement = 19,
    kRtConstWords = 32,
};

struct DxilRtProgramDesc {
    struct Library {
        const u8* data{nullptr};
        usize size{0};
        /// Exports to take (export name -> function name in the library);
        /// empty: every function.
        std::vector<std::pair<std::string, std::string>> exports;
    };
    std::vector<Library> libraries;
    struct HitGroup {
        std::string name;
        bool procedural{false};
        std::string any_hit, closest_hit, intersection;
    };
    std::vector<HitGroup> hit_groups;
    /// D3D12_RAYTRACING_PIPELINE_CONFIG(1).
    u32 max_recursion{1};
    u32 pipeline_flags{0};
    /// D3D12_RAYTRACING_SHADER_CONFIG.
    u32 max_payload_bytes{0};
    u32 max_attribute_bytes{8};
    /// A root signature as the pipeline uses it. Classes are DXIL's (0 SRV,
    /// 1 UAV, 2 CBV, 3 sampler).
    struct Root {
        enum Kind : u32 { Constants = 0, CBV = 1, SRV = 2, UAV = 3, Table = 4 };
        struct Range {
            u32 cls{0};
            u32 space{0};
            u32 base{0};
            u32 count{0};  ///< ~0u: unbounded
            u32 table_offset{0};
        };
        struct Param {
            u32 kind{Constants};
            u32 space{0};
            u32 reg{0};
            u32 words{0};  ///< Constants: the 32-bit values
            /// Local roots: byte offset in the shader record after the
            /// 32-byte identifier.
            u32 record_offset{0};
            std::vector<Range> ranges;  ///< Table
        };
        std::vector<Param> params;
        struct Sampler {
            u32 space{0};
            u32 reg{0};
            /// Local roots: n of s(kRtLocalSamplerBase + n).
            u32 reserved{0};
        };
        std::vector<Sampler> samplers;
    };
    /// The local root signature of each export (shader or hit group) that
    /// has one; a hit group's is its component shaders'.
    std::map<std::string, Root> local_roots;
    /// The global root signature of each shader export that has one.
    std::map<std::string, Root> global_roots;
    /// Every library function a shader export or hit group names (all
    /// exports, not only the ones a raygen reaches) must have each resource
    /// it reads bound by exactly one of its global and local root signatures
    /// (Raytracing.md, "Subobject association requirements"; measured:
    /// d3d12_a48 local_rules). False for a collection that allows local
    /// dependencies on external definitions.
    bool validate_bindings{true};
    /// Check every payload and attribute structure against the shader
    /// config (false when the state object has none).
    bool check_sizes{true};
    /// Identifier index of each raygen / miss / callable export and hit
    /// group (the frontend's, stable across AddToStateObject); one missing
    /// takes the next index past the largest.
    std::map<std::string, u32> indices;
};

struct DxilRtProgram {
    bool ok{false};
    bool unimplemented{false};
    std::string failure;
    /// The SM 5.1 compute container (numthreads 8, 8, 1).
    std::vector<u8> container;
    /// Identifier index of each raygen / miss / callable export and hit group.
    std::map<std::string, u32> indices;
    /// Shader kinds by export (DXIL ShaderKind: 7 raygen, 8 intersection,
    /// 9 any hit, 10 closest hit, 11 miss, 12 callable).
    std::map<std::string, u32> kinds;
    /// The reserved local view arrays, by k: the root parameter class
    /// (DxilRtProgramDesc::Root::Kind CBV / SRV / UAV) and whether its
    /// elements are acceleration structures.
    struct LocalView {
        u32 kind{0};
        bool acceleration{false};
    };
    std::vector<LocalView> local_views;
    /// By export or hit group: (k, record offset of the VA) for each root
    /// descriptor of its local root signature that binds a library resource.
    std::map<std::string, std::vector<std::pair<u32, u32>>> owner_views;
};

[[nodiscard]] DxilRtProgram dxil_raytracing_to_dxbc(const DxilRtProgramDesc& desc);

/// Only the creation-time checks of dxil_raytracing_to_dxbc (exports, hit
/// groups, root-signature coverage), for collections, which compile nothing.
[[nodiscard]] DxilRtProgram dxil_raytracing_validate(const DxilRtProgramDesc& desc);

/// A subobject a DXIL library declares (Raytracing.md, "Subobjects in DXIL
/// libraries"); `kind` is its D3D12_STATE_SUBOBJECT_TYPE.
struct DxilSubobject {
    u32 kind{0};
    std::string name;
    /// GLOBAL / LOCAL_ROOT_SIGNATURE: the serialized root signature.
    std::vector<u8> root_signature;
    /// STATE_OBJECT_CONFIG flags; RAYTRACING_PIPELINE_CONFIG1 flags.
    u32 flags{0};
    u32 max_payload{0}, max_attributes{0};
    u32 max_recursion{0};
    /// HIT_GROUP (D3D12_HIT_GROUP_TYPE) and its imports ("" for none).
    u32 hit_group_type{0};
    std::string any_hit, closest_hit, intersection;
    /// SUBOBJECT_TO_EXPORTS_ASSOCIATION: the subobject's name and the
    /// exports (empty: a default association).
    std::string subobject;
    std::vector<std::string> exports;
};

/// The subobjects of the DXIL library at @p data (none without an RDAT
/// part); false when its runtime data is malformed (dxil_subobjects.cpp).
[[nodiscard]] bool dxil_library_subobjects(const u8* data, usize size, std::vector<DxilSubobject>& out);

/// The export names and DXIL shader kinds of the library at @p data (for
/// CreateStateObject's validation), false when it is no DXIL library.
[[nodiscard]] bool dxil_library_exports(const u8* data, usize size,
                                        std::map<std::string, u32>& kinds);

}  // namespace vitreo::graphics
