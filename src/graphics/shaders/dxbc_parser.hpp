// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file dxbc_parser.hpp
/// @brief DXBC shader bytecode container parser.
///
/// Parses the DXBC container format used by D3D9 (SM1-3) and D3D11 (SM4-5)
/// compiled shaders. Extracts shader model version, input/output signatures,
/// resource bindings, and the shader bytecode chunks.

#include "shader_types.hpp"
#include <array>
#include <string>
#include <vector>

namespace vitreo::graphics {

// =============================================================================
// DXBC Chunk Types
// =============================================================================

enum class DxbcChunkType : u32 {
    RDEF = 0x46454452, // Resource definition
    ISGN = 0x4E475349, // Input signature
    /// Input signature with per-element minimum precision: fxc emits it
    /// instead of ISGN when an input uses a min16/min10 type (measured with
    /// vs_5_0 on Windows). Each element is 32 bytes: stream, the six ISGN
    /// fields, then the D3D_MIN_PRECISION value.
    ISG1 = 0x31475349,
    OSGN = 0x4E47534F, // Output signature
    /// SM5 geometry-shader output signature: each element is prefixed by
    /// its stream index (28 bytes; measured on fxc gs_5_0 output). fxc
    /// emits it for every gs_5_0 shader, even one using only stream 0.
    OSG5 = 0x3547534F,
    /// Patch-constant signature: a hull shader's patch-constant outputs, a
    /// domain shader's patch-constant inputs (same 24-byte elements).
    PCSG = 0x47534350,
    SHDR = 0x52444853, // SM4/5 shader bytecode
    SHEX = 0x58454853, // SM5 extended shader bytecode
    STAT = 0x54415453, // Statistics
    SFI0 = 0x30494653, // Feature info
    /// Class linkage (D3D11.3 7.19): the class types, the class instances
    /// with data and the interface slots of a program that declares
    /// interfaces (DxbcClassLinkage).
    IFCE = 0x45434649,
    /// Vitreo's own chunk, never produced by a compiler: the class instance
    /// bound at each interface slot of a program specialised for one
    /// *SetShader binding (dxbc_with_slot_bindings).
    VSLB = 0x424C5356,
};

// =============================================================================
// Parsed Signature Element
// =============================================================================

struct DxbcSignatureElement {
    std::string semantic_name;
    u32 semantic_index;
    u32 register_num;
    u8 component_mask;  // Bitmask of active components (xyzw)
    u8 component_type;  // 0=unknown, 1=uint, 2=int, 3=float
    u32 system_value_type{};  // D3D_NAME_*; 0 for an ordinary semantic
    /// Geometry-shader output stream (OSG5); 0 everywhere else.
    u32 stream{0};
};

// =============================================================================
// Resource Binding (from RDEF chunk)
// =============================================================================

struct DxbcResourceBinding {
    std::string name;
    u32 type;           // D3D_SIT_* values (0=CBV, 2=texture, 3=sampler, etc.)
    u32 bind_point;     // Register number (b0, t0, s0, etc.)
    u32 bind_count;     // Array size
    u32 return_type;    // For textures
    u32 dimension;      // Texture dimension
    /// SM 5.1 RDEF (D3D12_SHADER_INPUT_BIND_DESC Space / uID): the register
    /// space and the declaration's range ID (the T#/S#/U#/CB# the bytecode
    /// names it by). 0 before 5.1. An unbounded 5.1 range has bind_count 0
    /// -- 0xFFFFFFFF for a cbuffer (measured, fxc 10.1 on Windows,
    /// tools/oracle_probe/d3d12_sm51).
    u32 space{0};
    u32 range_id{0};
};

// =============================================================================
// Constant Buffer Layout (from RDEF chunk)
// =============================================================================

struct DxbcCBufferVariable {
    std::string name;
    u32 offset;
    u32 size;
    u32 type_class;     // Scalar, vector, matrix, etc.
    u32 type_type;      // Float, int, uint, bool
    u32 rows;
    u32 columns;
};

struct DxbcConstantBuffer {
    std::string name;
    u32 size_bytes;
    std::vector<DxbcCBufferVariable> variables;
};

// =============================================================================
// Class linkage (IFCE chunk, D3D11.3 7.19)
// =============================================================================
//
// The chunk is undocumented; its layout was measured against fxc 10.1's own
// /Fc listing ("Available Class Types", "Available Class Instances",
// "Interface slots") on Windows (tools/oracle_probe/d3d11_classlink.hlsl).
// Header, nine u32: instance count, type count, slot-run count, slot count
// (the this[] entries), then the byte offsets (from the chunk start) of the
// instance records, the type records and the slot runs, and two zero words.
// Names are NUL-terminated strings at chunk offsets.

/// "-" in the listing: the instance has no constant-buffer data, texture or
/// sampler.
inline constexpr u32 kDxbcClassNone = 0xFFFFu;

/// A class type: 12-byte record {name offset, u16 (0 in every shader
/// measured), u16 constant-buffer stride in 16-byte vectors, u16 texture
/// count, u16 sampler count}. Its type ID is the record index.
struct DxbcClassType {
    std::string name;
    u32 cb_stride{0};
    u32 texture_count{0};
    u32 sampler_count{0};
};

/// A class instance with data or resources (fxc lists no data-less
/// instance): 16-byte record {name offset, u16 type ID, u16 array count,
/// u16 constant buffer, u16 vector offset, u16 texture, u16 sampler}.
struct DxbcClassInstance {
    std::string name;
    u32 type{0};
    u32 count{1};
    u32 constant_buffer{kDxbcClassNone};
    u32 cb_offset{kDxbcClassNone};
    u32 texture{kDxbcClassNone};
    u32 sampler{kDxbcClassNone};
};

/// A run of consecutive interface slots (one interface variable or array):
/// 16-byte record {slot count, entry count, offset of the u16 type IDs,
/// offset of the u32 function-table IDs}. A class of type_ids[k] bound at
/// one of these slots runs function table table_ids[k].
struct DxbcInterfaceSlots {
    u32 count{0};
    std::vector<u32> type_ids;
    std::vector<u32> table_ids;
};

struct DxbcClassLinkage {
    std::vector<DxbcClassType> types;
    std::vector<DxbcClassInstance> instances;
    std::vector<DxbcInterfaceSlots> slot_runs;
    /// Interface slots (this[] entries) in all, in slot-run order.
    u32 slot_count{0};
};

/// No class instance at the slot: a program specialised with this binding
/// calls nothing through it.
inline constexpr u32 kDxbcUnboundTable = 0xFFFFFFFFu;

/// The class instance bound at one interface slot: the function table its
/// class runs there (dcl_function_table ft#) and its this[] data (D3D11.3
/// 22.7.20: constant buffer, vector offset, texture, sampler).
struct DxbcSlotBinding {
    u32 table{kDxbcUnboundTable};
    std::array<u32, 4> this_data{};

    bool operator==(const DxbcSlotBinding&) const = default;
};

// =============================================================================
// Binding declarations (SHEX dcl_*)
// =============================================================================
//
// Every descriptor the program declares, from its declaration opcodes -- the
// authoritative binding list, present in stripped bytecode too (RDEF is
// optional). Shader model 5.1 (D3D12) declares a RANGE of registers in a
// register space under a range ID, and names it by that ID in instructions;
// before 5.1 each declaration is one register of space 0 and its "range ID"
// is the register itself. Encodings measured with fxc 10.1 (Windows SDK
// 10.0.26100) on Windows, tools/oracle_probe/d3d12_sm51/:
//
//   5.1 operand: index dimension 3, all immediate: [range ID][lower][upper],
//   upper 0xFFFFFFFF for an unbounded range ([lower:*]), then the
//   declaration's own DWORDs, then the register space as the LAST DWORD:
//     dcl_constantbuffer (89)      [id][lo][hi], size in vec4s, space
//     dcl_sampler (90)             [id][lo][hi], space
//     dcl_resource (88)            [id][lo][hi], return-type token, space
//     dcl_resource_raw (161)       [id][lo][hi], space
//     dcl_resource_structured(162) [id][lo][hi], stride, space
//     dcl_uav_typed (156)          [id][lo][hi], return-type token, space
//     dcl_uav_raw (157)            [id][lo][hi], space
//     dcl_uav_structured (158)     [id][lo][hi], stride, space
//   Before 5.1 the operand is 1-D [register] (2-D [register][size] for a
//   constant buffer) and no space DWORD follows.

enum class DxbcRegisterClass : u8 {
    ConstantBuffer,   // b#
    ShaderResource,   // t#
    UnorderedAccess,  // u#
    Sampler,          // s#
};

/// DxbcBindingDeclaration::upper of an unbounded range (HLSL `T a[]`).
inline constexpr u32 kDxbcUnboundedRange = 0xFFFFFFFFu;

struct DxbcBindingDeclaration {
    DxbcRegisterClass register_class{DxbcRegisterClass::ShaderResource};
    /// 5.1: the declaration's ID (CB#/T#/U#/S#), unique per class; before
    /// 5.1 the register.
    u32 range_id{0};
    u32 space{0};
    /// First and last register; upper == kDxbcUnboundedRange when unbounded.
    u32 lower{0};
    u32 upper{0};
    /// The declaring opcode (see above).
    u32 opcode{0};
    /// dcl_resource / dcl_uav_typed: D3D10_SB_RESOURCE_DIMENSION, OpcodeToken0
    /// bits 11-15 (1 buffer, 2 texture1d, 3 texture2d, 4 texture2dms, 5
    /// texture3d, 6 texturecube, 7 texture1darray, 8 texture2darray, 9
    /// texture2dmsarray, 10 texturecubearray). 0 for the other opcodes.
    u32 dimension{0};
    /// The packed return-type token, 4 bits per component
    /// (D3D10_SB_RESOURCE_RETURN_TYPE: 1 unorm, 2 snorm, 3 sint, 4 uint,
    /// 5 float, ...); 0 when the opcode has none.
    u32 return_type{0};
    /// dcl_resource_texture2dms[array]: the declared sample count,
    /// OpcodeToken0 bits 16-22 (0 = not specified: Texture2DMS<T>).
    u32 sample_count{0};
    /// Structured: the structure stride in bytes.
    u32 stride{0};
    /// Constant buffer: the declared size in 16-byte vectors.
    u32 cb_vectors{0};
    /// Constant buffer: `dynamicIndexed` (OpcodeToken0 bit 11).
    bool dynamically_indexed{false};
    /// Sampler: D3D10_SB_SAMPLER_MODE, OpcodeToken0 bits 11-14 (0 default,
    /// 1 comparison, 2 mono).
    u32 sampler_mode{0};
    /// UAV: `_glc` (bit 16), `_rov` (bit 17), `_opc` (bit 23: the structured
    /// UAV has a counter -- which a 5.1 ARRAY range moves without the flag).
    bool globally_coherent{false};
    bool rasterizer_ordered{false};
    bool has_counter{false};
    /// The RDEF name, when reflection is present.
    std::string name;

    [[nodiscard]] bool unbounded() const {
        return upper == kDxbcUnboundedRange;
    }
    /// Registers in the range; kDxbcUnboundedRange when unbounded.
    [[nodiscard]] u32 count() const {
        return unbounded() ? kDxbcUnboundedRange : upper - lower + 1u;
    }
    /// A 5.1 range of more than one register (or unbounded): translated as
    /// a descriptor array.
    [[nodiscard]] bool is_array() const { return unbounded() || upper > lower; }
};

// =============================================================================
// Parsed DXBC Container
// =============================================================================

struct DxbcContainer {
    u32 shader_model_major;
    u32 shader_model_minor;
    ShaderStage stage;
    std::vector<u8> bytecode;       // Raw SHDR/SHEX bytecode
    std::vector<DxbcSignatureElement> input_signature;
    /// An ISGN or ISG1 chunk was present (possibly with no elements). A blob
    /// from D3DGetInputSignatureBlob carries only this chunk and no bytecode,
    /// so it is not valid() yet is a valid input signature.
    bool has_input_signature{false};
    std::vector<DxbcSignatureElement> output_signature;
    std::vector<DxbcSignatureElement> patch_constant_signature;
    std::vector<DxbcResourceBinding> resource_bindings;
    std::vector<DxbcConstantBuffer> constant_buffers;
    /// Every binding the program declares, in declaration order (see
    /// DxbcBindingDeclaration). Empty when the bytecode is malformed.
    std::vector<DxbcBindingDeclaration> binding_declarations;
    /// The SFI0 chunk's feature flags: the D3D_SHADER_REQUIRES_* bits of
    /// d3d11shader.h (ID3D11ShaderReflection::GetRequiresFlags), which the
    /// runtime checks against the device at shader creation. 0 without the
    /// chunk.
    u64 required_features{0};
    /// IFCE: present only in a program that declares interfaces.
    bool has_class_linkage{false};
    DxbcClassLinkage class_linkage;
    /// VSLB: one entry per interface slot when this is a program specialised
    /// for a class-instance binding; empty otherwise, and the translator
    /// then treats every slot as unbound.
    std::vector<DxbcSlotBinding> slot_bindings;
    u8 checksum[16];

    [[nodiscard]] bool valid() const {
        return !bytecode.empty();
    }

    [[nodiscard]] bool is_sm4_plus() const {
        return shader_model_major >= 4;
    }

    /// Shader model 5.1 (D3D12): register ranges, spaces and 2-D/3-D
    /// resource operands.
    [[nodiscard]] bool is_sm51_plus() const {
        return shader_model_major > 5 ||
               (shader_model_major == 5 && shader_model_minor >= 1);
    }
};

// =============================================================================
// API
// =============================================================================

/// Parse a DXBC container from raw bytecode.
/// @param data     Pointer to the DXBC blob (starts with "DXBC" magic)
/// @param size     Size of the blob in bytes
/// @return Parsed container; check valid() for success
[[nodiscard]] DxbcContainer parse_dxbc(const u8* data, usize size);

/// True when @p data is a well-formed DXBC container whose checksum matches:
/// "DXBC" magic, a TotalSize no larger than @p size, every chunk inside it,
/// and the container hash the compiler stores at bytes 4-19. The hash is MD5
/// over bytes 20..TotalSize with the container's own final-block padding
/// (DirectXShaderCompiler, lib/DxilHash/DxilHash.cpp, ComputeHashRetail).
/// The D3D11 runtime rejects a blob whose hash does not match (measured,
/// d3d11_a05b_bind_rules_probe: CreateInputLayout -> E_INVALIDARG).
[[nodiscard]] bool dxbc_container_verified(const u8* data, usize size);

/// The container hash of the first @p total_size bytes at @p data (bytes
/// 20..total_size), as the compiler stores it at bytes 4-19 -- the rule
/// dxbc_container_verified checks. For a container built at runtime (the
/// D3D10Get*SignatureBlob blobs).
void dxbc_container_checksum(const u8* data, u32 total_size, u8 out[16]);

/// True when the program declares any unordered-access view (dcl_uav_typed,
/// dcl_uav_raw or dcl_uav_structured). Feature level 11_0 allows these in
/// the pixel and compute stages only; the D3D11 runtime refuses the others.
[[nodiscard]] bool dxbc_declares_uavs(const DxbcContainer& container);

/// True when the program writes memory: store_uav_typed (164), store_raw
/// (166), store_structured (168) or any atomic_* / imm_atomic_* (169-189,
/// imm_atomic_alloc/consume included). A pixel shader has no groupshared
/// memory, so in that stage every one of these is a u# side effect.
[[nodiscard]] bool dxbc_writes_memory(const DxbcContainer& container);

/// D3D11.3 16.3: the pixel shader runs once per covered sample rather than
/// once per pixel -- it reads SV_SampleIndex, or declares an input
/// (SV_Position included) with `sample` interpolation (dcl_input_ps* mode 6
/// linear sample / 7 noperspective sample).
[[nodiscard]] bool dxbc_pixel_runs_per_sample(const DxbcContainer& container);

/// dcl_globalFlags forceEarlyDepthStencil (HLSL [earlydepthstencil]):
/// OpcodeToken0 bit 13, measured with fxc.
[[nodiscard]] bool dxbc_forces_early_depth_stencil(
    const DxbcContainer& container);

/// d3d11shader.h D3D_SHADER_REQUIRES_* bits (DxbcContainer::required_features).
inline constexpr u64 kShaderRequiresDoubles = 0x1;
inline constexpr u64 kShaderRequiresUavsAtEveryStage = 0x4;
inline constexpr u64 kShaderRequires64Uavs = 0x8;
inline constexpr u64 kShaderRequires11_1DoubleExtensions = 0x20;
inline constexpr u64 kShaderRequires11_1ShaderExtensions = 0x40;

/// True when the program uses class linkage: dcl_function_body/_table,
/// dcl_interface or fcall. Such a shader needs an ID3D11ClassLinkage at
/// creation (WARP: E_INVALIDARG without one).
[[nodiscard]] bool dxbc_uses_class_linkage(const DxbcContainer& container);

/// A copy of the DXBC blob @p data with a VSLB chunk carrying @p bindings
/// appended (every other chunk moves 4 bytes for the new offset entry; the
/// container hash is not recomputed -- the copy never leaves Vitreo). Empty
/// when @p data is not a well-formed container.
[[nodiscard]] std::vector<u8> dxbc_with_slot_bindings(
    const u8* data, usize size, const std::vector<DxbcSlotBinding>& bindings);

/// True when a pixel shader's output signature has SV_StencilRef.
[[nodiscard]] bool dxbc_writes_stencil_ref(const DxbcContainer& container);

/// Detect shader stage from SM4/5 SHDR/SHEX program type token.
[[nodiscard]] ShaderStage dxbc_program_type_to_stage(u32 program_type);

/// Detect shader model version from a D3D9 shader bytecode blob.
/// D3D9 shaders don't use the DXBC container — they start directly
/// with a version token: 0xFFFE (VS) or 0xFFFF (PS) followed by major.minor.
/// @param data     Raw D3D9 shader bytecode
/// @param size     Size in bytes
/// @param out_major Output: shader model major version (1-3)
/// @param out_minor Output: shader model minor version
/// @param out_stage Output: detected stage
/// @return true if the data appears to be valid D3D9 shader bytecode
[[nodiscard]] bool detect_d3d9_shader(
    const u8* data, usize size,
    u32& out_major, u32& out_minor, ShaderStage& out_stage);

} // namespace vitreo::graphics
