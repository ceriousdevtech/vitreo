#pragma once
/// @file d3d12_root_signature.hpp
/// @brief Direct3D 12 root signatures: the version-neutral description, the
/// serialized blob, the serializer's and CreateRootSignature's validation,
/// version conversion, and the guest D3D12_*ROOT_SIGNATURE_DESC* layouts.
///
/// Every rule here was measured on Windows 11 26100 with
/// tools/oracle_probe/d3d12_rootsig_probe.c (x64 and x86, byte-exact output in
/// d3d12_rootsig_probe{64,32}.expected; CreateRootSignature on the default
/// adapter in d3d12_rootsig_probe_device64.expected). Learn documents the
/// structures and the 1.0 <-> 1.1 conversion; the blob layout, the validation
/// order, every error-blob text and the acceptance rules are not documented
/// and come from the probe.
///
/// **Blob.** A DXBC container (magic, 16-byte hash, version 1, total size,
/// part count, part offsets) whose first "RTS0" part holds: a 24-byte header
/// {Version, NumParameters, ParametersOffset, NumStaticSamplers,
/// StaticSamplersOffset, Flags}; 12-byte parameter records {Type, Visibility,
/// PayloadOffset}; each parameter's payload in parameter order (a table's
/// {NumRanges, RangesOffset} immediately followed by its ranges); then the
/// static samplers. Offsets are relative to the part data. 1.1 adds Flags to
/// ranges (before the offset) and root descriptors; 1.2 adds Flags to static
/// samplers. A shader's bytecode carries the same part.

#include <vitreo/common.hpp>

#include <array>
#include <string>
#include <vector>

namespace vitreo {
class GuestMemory;
}

namespace vitreo::winapi::d3d12 {

/// D3D_ROOT_SIGNATURE_VERSION (d3d12.h, 26100 SDK).
inline constexpr u32 ROOT_SIGNATURE_VERSION_1_0 = 1;
inline constexpr u32 ROOT_SIGNATURE_VERSION_1_1 = 2;
inline constexpr u32 ROOT_SIGNATURE_VERSION_1_2 = 3;

/// D3D12_ROOT_PARAMETER_TYPE.
inline constexpr u32 ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE = 0;
inline constexpr u32 ROOT_PARAMETER_TYPE_32BIT_CONSTANTS = 1;
inline constexpr u32 ROOT_PARAMETER_TYPE_CBV = 2;
inline constexpr u32 ROOT_PARAMETER_TYPE_SRV = 3;
inline constexpr u32 ROOT_PARAMETER_TYPE_UAV = 4;

/// D3D12_DESCRIPTOR_RANGE_TYPE.
inline constexpr u32 DESCRIPTOR_RANGE_TYPE_SRV = 0;
inline constexpr u32 DESCRIPTOR_RANGE_TYPE_UAV = 1;
inline constexpr u32 DESCRIPTOR_RANGE_TYPE_CBV = 2;
inline constexpr u32 DESCRIPTOR_RANGE_TYPE_SAMPLER = 3;

/// D3D12_SHADER_VISIBILITY: ALL (0) through MESH (7).
inline constexpr u32 SHADER_VISIBILITY_ALL = 0;
inline constexpr u32 SHADER_VISIBILITY_MESH = 7;

/// D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND, and the NumDescriptors value
/// (UINT_MAX) that makes a range unbounded.
inline constexpr u32 DESCRIPTOR_RANGE_OFFSET_APPEND = 0xFFFFFFFFu;
inline constexpr u32 DESCRIPTOR_RANGE_UNBOUNDED = 0xFFFFFFFFu;

/// D3D12_ROOT_SIGNATURE_FLAG_LOCAL_ROOT_SIGNATURE.
inline constexpr u32 ROOT_SIGNATURE_FLAG_LOCAL_ROOT_SIGNATURE = 0x80u;

/// One D3D12_DESCRIPTOR_RANGE / D3D12_DESCRIPTOR_RANGE1. `flags` is 0 for 1.0.
struct RootDescriptorRange {
    u32 range_type{0};
    u32 num_descriptors{0};
    u32 base_shader_register{0};
    u32 register_space{0};
    u32 flags{0};
    u32 offset_in_descriptors_from_table_start{0};
};

/// One D3D12_ROOT_PARAMETER / D3D12_ROOT_PARAMETER1. Which fields mean
/// anything depends on `parameter_type`: `ranges` for a descriptor table;
/// `shader_register`, `register_space` and `num_32bit_values` for root
/// constants; `shader_register`, `register_space` and `flags` (1.1+) for a
/// root CBV/SRV/UAV.
struct RootParameter {
    u32 parameter_type{0};
    u32 shader_visibility{0};
    std::vector<RootDescriptorRange> ranges;
    u32 shader_register{0};
    u32 register_space{0};
    u32 num_32bit_values{0};
    u32 flags{0};
};

/// One D3D12_STATIC_SAMPLER_DESC / D3D12_STATIC_SAMPLER_DESC1. `flags`
/// (D3D12_SAMPLER_FLAGS) is 0 below 1.2. The floats are carried bit for bit:
/// nothing here does arithmetic on them.
struct StaticSampler {
    u32 filter{0};
    u32 address_u{0};
    u32 address_v{0};
    u32 address_w{0};
    f32 mip_lod_bias{0.0f};
    u32 max_anisotropy{0};
    u32 comparison_func{0};
    u32 border_color{0};
    f32 min_lod{0.0f};
    f32 max_lod{0.0f};
    u32 shader_register{0};
    u32 register_space{0};
    u32 shader_visibility{0};
    u32 flags{0};
};

/// A root signature of any version: the 1.2 superset. `version` is the
/// D3D_ROOT_SIGNATURE_VERSION the desc was read or parsed at, which decides
/// which of the flags fields exist.
struct RootSignatureDesc {
    u32 version{ROOT_SIGNATURE_VERSION_1_0};
    u32 flags{0};
    std::vector<RootParameter> parameters;
    std::vector<StaticSampler> static_samplers;
};

/// True for D3D_ROOT_SIGNATURE_VERSION_1_0, _1_1 and _1_2.
[[nodiscard]] bool is_root_signature_version(u32 version);

/// Parse a serialized root signature -- a DXBC container, a root signature
/// compiled by fxc, or shader bytecode carrying one -- with the acceptance
/// rules of D3D12Create(Versioned)RootSignatureDeserializer: the size must be
/// the container's total size, the container hash must match, and the first
/// RTS0 part must be in bounds with a known version and parameter types. The
/// contents are otherwise not validated (a visibility of 8 parses). Returns
/// S_OK or E_INVALIDARG; @p desc receives the unconverted desc.
///
/// Windows reads a parameter array whose count or offset runs past the part
/// without checking (an access violation in d3d12.dll); this returns
/// E_INVALIDARG instead.
[[nodiscard]] u32 parse_root_signature_blob(const u8* data, usize size,
                                            RootSignatureDesc& desc);

/// The HRESULT ID3D12Device::CreateRootSignature returns for a blob, and the
/// desc it describes. Measured, and different from the deserializers: the
/// container hash is not checked; a size that is not the container's total
/// size, an RTS0 part that is out of bounds, has an unknown version or an
/// unknown parameter type is E_INVALIDARG; any other malformed container (magic,
/// version, part table, no RTS0 part) is E_FAIL; then
/// validate_root_signature_for_device.
[[nodiscard]] u32 parse_root_signature_blob_for_device(const u8* data,
                                                       usize size,
                                                       RootSignatureDesc& desc);

/// CreateRootSignature's validation of a parsed desc: S_OK, E_FAIL for every
/// rule the serializer enforces (with the device's own, wider flag masks) and
/// for register spaces 0xFFFFFFF0 and up, then E_INVALIDARG when the root
/// arguments cost more than 64 DWORDs (tables 1, root descriptors 2, constants
/// one per value; not checked for a local root signature).
[[nodiscard]] u32 validate_root_signature_for_device(
    const RootSignatureDesc& desc);

/// Serialize @p desc at @p version (1, 2 or 3), the way
/// D3D12SerializeVersionedRootSignature does: validate (first error wins, in
/// Windows' order), then write the blob. Fields @p version's structures do
/// not have are not read. Returns S_OK or E_INVALIDARG. @p error_message
/// receives the text Windows puts in the error blob, without the terminating
/// NUL: the error on failure, empty or a warning ("Root constants
/// Num32BitValues should be > 0.") on success. An invalid @p version is
/// E_INVALIDARG with no message.
[[nodiscard]] u32 serialize_root_signature(const RootSignatureDesc& desc,
                                           u32 version, std::vector<u8>& blob,
                                           std::string& error_message);

/// @p desc converted to @p version, as the deserializers convert it. Learn
/// (GetRootSignatureDescAtVersion): 1.1 -> 1.0 drops every range and root
/// descriptor flag; 1.0 -> 1.1 adds the flags that match 1.0 semantics
/// (measured: DESCRIPTORS_VOLATILE | DATA_VOLATILE on every non-sampler range,
/// DESCRIPTORS_VOLATILE on sampler ranges, DATA_VOLATILE on root
/// descriptors). Below 1.2 the static-sampler flags are dropped.
[[nodiscard]] RootSignatureDesc convert_root_signature(
    const RootSignatureDesc& desc, u32 version);

// -- Guest structures ---------------------------------------------------------
//
// D3D12_ROOT_SIGNATURE_DESC (version 1), _DESC1 (2) and _DESC2 (3), and
// D3D12_VERSIONED_ROOT_SIGNATURE_DESC, in the guest's bitness. Layouts from
// the 26100 SDK declarations, checked against the probe's [layout] lines.

/// Read a D3D12_ROOT_SIGNATURE_DESC{,1,2} (@p version 1, 2, 3) and every
/// array it points to. False when any read faults: a NULL array with a
/// non-zero count (Windows dereferences it and faults).
[[nodiscard]] bool read_guest_root_signature_desc(const GuestMemory& memory,
                                                  GuestAddr address,
                                                  u32 version, bool is_64bit,
                                                  RootSignatureDesc& desc);

/// Read a D3D12_VERSIONED_ROOT_SIGNATURE_DESC. @p desc.version receives its
/// Version; the union is read only when that is a known version.
[[nodiscard]] bool read_guest_versioned_root_signature_desc(
    const GuestMemory& memory, GuestAddr address, bool is_64bit,
    RootSignatureDesc& desc);

/// Bytes write_guest_root_signature_desc needs.
[[nodiscard]] usize guest_root_signature_desc_size(
    const RootSignatureDesc& desc, u32 version, bool is_64bit, bool versioned);

/// Lay @p desc out at @p address as a D3D12_ROOT_SIGNATURE_DESC{,1,2} for
/// @p version -- or, when @p versioned, a D3D12_VERSIONED_ROOT_SIGNATURE_DESC --
/// with every array after it in the same block. An empty array is a NULL
/// pointer, as the deserializers return it (measured).
[[nodiscard]] bool write_guest_root_signature_desc(
    GuestMemory& memory, GuestAddr address, const RootSignatureDesc& desc,
    u32 version, bool is_64bit, bool versioned);

// -- COM tables -----------------------------------------------------------------

// ID3D10BlobVtbl, 5 slots, generated by
// tools/audit_d3d11_vtables.py --emit BLOB_STDCALL_BYTES@d3d12
constexpr usize BLOB_METHOD_COUNT = 5;

inline constexpr std::array<u16, d3d12::BLOB_METHOD_COUNT>
    BLOB_STDCALL_BYTES{{
    12, 4, 4, 4, 4
}};

inline constexpr std::array<const char*, d3d12::BLOB_METHOD_COUNT>
    BLOB_METHOD_NAMES{{
    "QueryInterface", "AddRef", "Release", "GetBufferPointer",
    "GetBufferSize"
}};

// ID3D12RootSignatureDeserializerVtbl, 4 slots, generated by
// tools/audit_d3d11_vtables.py --emit ROOT_SIGNATURE_DESERIALIZER_STDCALL_BYTES
constexpr usize ROOT_SIGNATURE_DESERIALIZER_METHOD_COUNT = 4;

inline constexpr std::array<u16,
                            d3d12::ROOT_SIGNATURE_DESERIALIZER_METHOD_COUNT>
    ROOT_SIGNATURE_DESERIALIZER_STDCALL_BYTES{{
    12, 4, 4, 4
}};

// ID3D12VersionedRootSignatureDeserializerVtbl, 5 slots, generated by
// tools/audit_d3d11_vtables.py
//     --emit VERSIONED_ROOT_SIGNATURE_DESERIALIZER_STDCALL_BYTES
constexpr usize VERSIONED_ROOT_SIGNATURE_DESERIALIZER_METHOD_COUNT = 5;

inline constexpr std::array<
    u16, d3d12::VERSIONED_ROOT_SIGNATURE_DESERIALIZER_METHOD_COUNT>
    VERSIONED_ROOT_SIGNATURE_DESERIALIZER_STDCALL_BYTES{{
    12, 4, 4, 12, 4
}};

/// Interface IDs, checked against the SDK by tools/audit_guid_constants.py.
using GuidBytes = std::array<u8, 16>;

/// ID3D10Blob (= ID3DBlob) {8ba5fb08-5195-40e2-ac58-0d989c3a0102}.
inline constexpr GuidBytes IID_ID3D10BLOB{
    0x08, 0xFB, 0xA5, 0x8B, 0x95, 0x51, 0xE2, 0x40,
    0xAC, 0x58, 0x0D, 0x98, 0x9C, 0x3A, 0x01, 0x02};
/// ID3D12RootSignatureDeserializer {34AB647B-3CC8-46AC-841B-C0965645C046}.
inline constexpr GuidBytes IID_ID3D12ROOTSIGNATUREDESERIALIZER{
    0x7B, 0x64, 0xAB, 0x34, 0xC8, 0x3C, 0xAC, 0x46,
    0x84, 0x1B, 0xC0, 0x96, 0x56, 0x45, 0xC0, 0x46};
/// ID3D12VersionedRootSignatureDeserializer
/// {7F91CE67-090C-4BB7-B78E-ED8FF2E31DA0}.
inline constexpr GuidBytes IID_ID3D12VERSIONEDROOTSIGNATUREDESERIALIZER{
    0x67, 0xCE, 0x91, 0x7F, 0x0C, 0x09, 0xB7, 0x4B,
    0xB7, 0x8E, 0xED, 0x8F, 0xF2, 0xE3, 0x1D, 0xA0};

} // namespace vitreo::winapi::d3d12
