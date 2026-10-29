// SPDX-License-Identifier: LGPL-2.1-only

/// @file d3d12_root_signature.cpp
/// @brief Root-signature serialization, parsing, validation and conversion,
/// and the d3d12.dll exports and COM objects built on them: ID3DBlob,
/// D3D12Serialize(Versioned)RootSignature, and the two deserializers.
///
/// Source for every rule: tools/oracle_probe/d3d12_rootsig_probe.c and its
/// .expected files (Windows, 26100, x64 and x86). Comments cite the probe
/// case that pins a rule.

#include "winapi/graphics/d3d12/d3d12_root_signature.hpp"
#include "winapi/graphics/d3d12/d3d12.hpp"
#include "winapi/graphics/d3d12/d3d12_internal.hpp"
#include "graphics/shaders/dxbc_parser.hpp"
#include "winapi/core/marshaler.hpp"
#include "core/memory/guest_memory.hpp"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>

namespace vitreo::winapi::d3d12 {
namespace {

constexpr u32 kDxbcMagic = 0x43425844u;  // "DXBC"
constexpr u32 kRts0 = 0x30535452u;       // "RTS0"
constexpr u32 kDxbcHeaderBytes = 32;     // magic, hash, version, size, count

/// Part sizes per version.
constexpr u32 range_bytes(u32 version) { return version >= 2 ? 24u : 20u; }
constexpr u32 root_descriptor_bytes(u32 version) {
    return version >= 2 ? 12u : 8u;
}
constexpr u32 sampler_bytes(u32 version) { return version >= 3 ? 56u : 52u; }

u32 f32_bits(f32 value) {
    u32 bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

f32 bits_f32(u32 bits) {
    f32 value = 0.0f;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

u32 load_u32(const u8* data) {
    u32 value = 0;
    std::memcpy(&value, data, sizeof(value));
    return value;
}

void append_u32(std::vector<u8>& out, u32 value) {
    const usize at = out.size();
    out.resize(at + 4);
    std::memcpy(out.data() + at, &value, 4);
}

std::string format(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
std::string format(const char* fmt, ...) {
    char buffer[512];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    return buffer;
}

/// "%f" of a float the way d3d12.dll prints one: exact digits for finite
/// values (float_bias_7149F2CA: 1e30f prints all 31 digits; ties round to
/// even, float_bias_41801000: 16.0078125 -> "16.007812"), and the legacy
/// MSVCRT spellings for the rest: "1.#INF00", "-1.#INF00", "-1.#IND00" for
/// the negative default NaN, "1.#QNAN0" / "-1.#QNAN0" for every other NaN
/// (float_bias_7FC00000 / FFC00000 / FF800001 / FFFFFFFF).
std::string format_legacy_f(f32 value) {
    const u32 bits = f32_bits(value);
    const bool negative = (bits >> 31) != 0;
    if ((bits & 0x7F800000u) == 0x7F800000u) {
        const u32 mantissa = bits & 0x007FFFFFu;
        if (mantissa == 0) return negative ? "-1.#INF00" : "1.#INF00";
        if (negative && mantissa == 0x00400000u) return "-1.#IND00";
        return negative ? "-1.#QNAN0" : "1.#QNAN0";
    }
    return format("%f", static_cast<double>(value));
}

bool is_nan(f32 value) {
    return (f32_bits(value) & 0x7FFFFFFFu) > 0x7F800000u;
}

const char* visibility_name(u32 visibility) {
    static constexpr const char* kNames[] = {
        "ALL", "VERTEX", "HULL", "DOMAIN", "GEOMETRY", "PIXEL",
        "AMPLIFICATION", "MESH"};
    return visibility <= SHADER_VISIBILITY_MESH ? kNames[visibility] : "?";
}

const char* range_class_name(u32 range_type) {
    static constexpr const char* kNames[] = {"SRV", "UAV", "CBV", "SAMPLER"};
    return range_type <= DESCRIPTOR_RANGE_TYPE_SAMPLER ? kNames[range_type]
                                                       : "?";
}

/// The D3D12_FILTER values a static sampler accepts (probe [scan] filter):
/// the eight point/linear combinations and ANISOTROPIC for each of the four
/// reductions. MIN_MAG_ANISOTROPIC_MIP_POINT (0x54 etc.), although in the SDK
/// enum, is refused.
bool static_filter_recognized(u32 filter) {
    if (filter > 0x1FFu) return false;
    const u32 low = filter & 0x7Fu;
    if (low == 0x55u) return true;
    return (low & ~0x15u) == 0;
}

bool is_comparison_filter(u32 filter) { return ((filter >> 7) & 3u) == 1u; }

/// Error texts, verbatim from the probe's error blobs. The trailing newline
/// differs between messages exactly as it does on Windows.
namespace msg {
std::string unsupported_root_flags(u32 flags) {
    return format("Unsupported bit-flag set (root signature flags %x).\n",
                  flags);
}
std::string local_root_signature(u32 flags) {
    return format("D3D12_ROOT_SIGNATURE_FLAG_LOCAL_ROOT_SIGNATURE combined "
                  "with other flag(s) that don't make sense (root signature "
                  "flags %x).\n",
                  flags);
}
std::string parameter_visibility(u32 visibility, u32 parameter) {
    return format("Unsupported ShaderVisibility value %u (root parameter "
                  "[%u]).\n",
                  visibility, parameter);
}
std::string sampler_visibility(u32 visibility, u32 sampler) {
    return format("Unsupported ShaderVisibility value %u (static sampler "
                  "[%u]).\n",
                  visibility, sampler);
}
std::string parameter_type(u32 type, u32 parameter) {
    return format("Unsupported ParameterType value %u (root parameter %u)\n",
                  type, parameter);
}
std::string range_type(u32 type, u32 slot, u32 parameter) {
    return format("Unsupported RangeType value %u (descriptor table slot "
                  "[%u], root parameter [%u]).\n",
                  type, slot, parameter);
}
std::string mixed_samplers(u32 parameter) {
    return format("Samplers cannot be mixed with other resource types in a "
                  "descriptor table (root parameter [%u]).\n",
                  parameter);
}
std::string zero_descriptors(u32 slot, u32 parameter) {
    return format("NumDescriptors cannot be 0 (descriptor table slot [%u], "
                  "root parameter [%u]).\n",
                  slot, parameter);
}
std::string append_after_unbounded(u32 slot, u32 parameter) {
    return format("Cannot append range with implicit lower bound after an "
                  "unbounded range (descriptor table slot [%u], root "
                  "parameter [%u]).\n",
                  slot, parameter);
}
std::string register_overflow(u32 base, u32 count, u32 slot, u32 parameter) {
    return format("Overflow for shader register range: BaseShaderRegister=%u, "
                  "NumDescriptor=%u; (descriptor table slot [%u], root "
                  "parameter [%u]).\n",
                  base, count, slot, parameter);
}
std::string offset_overflow(u32 slot, u32 parameter) {
    return format("Overflow for descriptor range (descriptor table slot [%u], "
                  "root parameter [%u])\n",
                  slot, parameter);
}
std::string unsupported_range_flags(u32 flags) {
    return format("Unsupported bit-flag set (descriptor range flags %x).\n",
                  flags);
}
std::string range_two_descriptor_flags(u32 flags) {
    return format("Descriptor range flags cannot specify more than one "
                  "DESCRIPTOR_* flag at a time (descriptor range flags "
                  "%x).\n",
                  flags);
}
std::string sampler_range_data_flags(u32 flags) {
    return format("Sampler descriptor ranges can't specify DATA_* flags since "
                  "there is no data pointed to by samplers (descriptor range "
                  "flags %x).\n",
                  flags);
}
std::string sampler_range_bounds_checks(u32 flags) {
    return format("Sampler descriptor ranges can't specify "
                  "DESCRIPTORS_STATIC_KEEPING_BUFFER_BOUNDS_CHECKS, since "
                  "there are no bounds to check (descriptor range flags "
                  "%x).\n",
                  flags);
}
std::string range_two_data_flags(u32 flags) {
    return format("Descriptor range flags cannot specify more than one DATA_* "
                  "flag at a time (descriptor range flags %x).\n",
                  flags);
}
std::string range_volatile_static(u32 flags) {
    return format("Descriptor range flags cannot specify DESCRIPTORS_VOLATILE "
                  "with the DATA_STATIC flag at the same time (descriptor "
                  "range flags %x). DATA_STATIC_WHILE_SET_AT_EXECUTE is fine "
                  "to combine with DESCRIPTORS_VOLATILE, since "
                  "DESCRIPTORS_VOLATILE still requires descriptors don't "
                  "change during execution. \n",
                  flags);
}
std::string unsupported_root_descriptor_flags(u32 flags) {
    return format("Unsupported bit-flag set (root descriptor flags %x).\n",
                  flags);
}
std::string root_descriptor_two_data_flags(u32 flags) {
    return format("Root descriptor flags cannot specify more than one DATA_* "
                  "flag at a time (root descriptor flags %x).\n",
                  flags);
}
constexpr const char* kZeroConstants =
    "Root constants Num32BitValues should be > 0.\n";
constexpr const char* kFilter = "Static sampler: Filter unrecognized.";
constexpr const char* kAddressU = "Static sampler: AddressU unrecognized.";
constexpr const char* kAddressV = "Static sampler: AddressV unrecognized.";
constexpr const char* kAddressW = "Static sampler: AddressW unrecognized.";
constexpr const char* kComparison =
    "Static sampler: ComparisonFunc unrecognized.";
std::string bias(f32 value) {
    return "Static sampler: MipLODBias must be in the range [-16.000000 to "
           "15.990000].  " +
           format_legacy_f(value) + " specified.";
}
std::string anisotropy(u32 value) {
    return format("Static sampler: MaxAnisotropy must be in the range [0 to "
                  "16].  %u specified.",
                  value);
}
std::string min_lod(f32 value) {
    return "Static sampler: MinLOD be in the range [-INF to +INF].  " +
           format_legacy_f(value) + " specified.";
}
// No "Static sampler: " prefix on this one (float_maxlod_7FC00000).
std::string max_lod(f32 value) {
    return "MaxLOD be in the range [-INF to +INF].  " + format_legacy_f(value) +
           " specified.";
}
constexpr const char* kSamplerFlags =
    "Sampler flags contain invalid value. See D3D12_SAMPLER_FLAGS.";
constexpr const char* kNonNormalizedFilter =
    "Filter is invalid with non-normalized coordinate sampling. MIN and MAG "
    "must be equal, MIP must be point, and Filter cannot be comparison or "
    "anisotropy.";
constexpr const char* kNonNormalizedLod =
    "MinLOD, MaxLOD, and MipLODBias must be 0 when sampling with "
    "non-normalized coordinates.";
constexpr const char* kNonNormalizedAddressU =
    "AddressU is invalid with non-normalized coordinate sampling. AddressU "
    "and AddressV must be either CLAMP or BORDER.";
constexpr const char* kNonNormalizedAddressV =
    "AddressV is invalid with non-normalized coordinate sampling. AddressU "
    "and AddressV must be either CLAMP or BORDER.";
} // namespace msg

// =============================================================================
// Validation
// =============================================================================

/// Which validator: the serializer's (error-blob text, E_INVALIDARG) or
/// CreateRootSignature's (E_FAIL / E_INVALIDARG, its own flag masks).
enum class Mode { Serializer, Device };

/// One register range registered for overlap checks.
struct Occupant {
    enum Kind : u8 { TableRange, RootParameter, StaticSampler } kind;
    u32 index{0};  // root parameter or static sampler
    u32 slot{0};   // descriptor table slot
    u32 visibility{0};
    u32 last{0};   // last register, inclusive
};

/// Overlap detection, reproducing which conflicting range Windows names.
///
/// A range lives in one bucket per (class, space, stage): a stage-specific
/// range in its stage's bucket, an ALL range in all seven. A new range is
/// checked bucket by bucket in stage order VERTEX..MESH, and within a bucket
/// against the range with the lowest base register that overlaps it
/// (overlap_stage_order_vs_register_order, overlap_mesh_then_amp,
/// overlap_two_in_bucket_lowest_base).
class OverlapTracker {
public:
    /// Registers @p occupant at [base, occupant.last] and returns an empty
    /// string, or the overlap message and registers nothing.
    std::string insert(u32 range_class, u32 space, u32 base,
                       const Occupant& occupant) {
        const u32 first_stage = occupant.visibility == 0 ? 1 : occupant.visibility;
        const u32 last_stage = occupant.visibility == 0 ? 7 : occupant.visibility;
        for (u32 stage = first_stage; stage <= last_stage; ++stage) {
            auto& bucket = m_buckets[key(range_class, space, stage)];
            const Occupant* hit = nullptr;
            auto next = bucket.upper_bound(base);
            if (next != bucket.begin()) {
                auto previous = std::prev(next);
                if (previous->second.last >= base) hit = &previous->second;
            }
            if (hit == nullptr && next != bucket.end() &&
                next->first <= occupant.last) {
                hit = &next->second;
            }
            if (hit != nullptr) {
                return overlap_message(range_class, occupant, *hit);
            }
        }
        for (u32 stage = first_stage; stage <= last_stage; ++stage) {
            m_buckets[key(range_class, space, stage)].emplace(base, occupant);
        }
        return {};
    }

private:
    static u64 key(u32 range_class, u32 space, u32 stage) {
        return (static_cast<u64>(space) << 8) | (range_class << 4) | stage;
    }

    /// The other range's visibility prints as ALL when either side is ALL
    /// (overlap_names_vis{1..7}_uav: a PIXEL range hit by an ALL range reads
    /// "visibility ALL").
    static std::string overlap_message(u32 range_class, const Occupant& added,
                                       const Occupant& other) {
        const u32 other_visibility =
            added.visibility == 0 || other.visibility == 0 ? 0
                                                           : other.visibility;
        std::string text = "Shader register range of type ";
        text += range_class_name(range_class);
        text += " (";
        text += describe(added, added.visibility, true);
        text += ") overlaps with another shader register range (";
        text += describe(other, other_visibility, false);
        text += ").\n";
        return text;
    }

    /// The table-range form of the second operand has no space before its
    /// bracket ("root parameter[0]"); every other form has one.
    static std::string describe(const Occupant& o, u32 visibility, bool first) {
        switch (o.kind) {
        case Occupant::TableRange:
            return format(first ? "root parameter [%u], visibility %s, "
                                  "descriptor table slot [%u]"
                                : "root parameter[%u], visibility %s, "
                                  "descriptor table slot [%u]",
                          o.index, visibility_name(visibility), o.slot);
        case Occupant::RootParameter:
            return format("root parameter [%u], visibility %s", o.index,
                          visibility_name(visibility));
        case Occupant::StaticSampler:
            break;
        }
        return format("static sampler [%u], visibility %s", o.index,
                      visibility_name(visibility));
    }

    std::map<u64, std::map<u32, Occupant>> m_buckets;
};

u32 popcount(u32 value) { return static_cast<u32>(__builtin_popcount(value)); }

class Validator {
public:
    Validator(const RootSignatureDesc& desc, u32 version, Mode mode)
        : m_desc(desc), m_version(version), m_mode(mode) {}

    /// S_OK, or the failure HRESULT with message() naming the first fault.
    u32 run() {
        if (!root_flags()) return fail_hr();
        for (u32 i = 0; i < m_desc.parameters.size(); ++i) {
            if (!parameter(i)) return fail_hr();
        }
        for (u32 i = 0; i < m_desc.static_samplers.size(); ++i) {
            if (!static_sampler(i)) return fail_hr();
        }
        return S_OK;
    }

    [[nodiscard]] const std::string& message() const { return m_message; }

private:
    [[nodiscard]] u32 fail_hr() const {
        return m_mode == Mode::Serializer ? E_INVALIDARG : E_FAIL;
    }

    bool fail(std::string text) {
        m_message = std::move(text);
        return false;
    }

    /// Register spaces 0xFFFFFFF0 and up are reserved. Only CreateRootSignature
    /// rejects them (space_reserved_*: the serializer accepts, the device
    /// returns E_FAIL).
    [[nodiscard]] bool space_ok(u32 space) const {
        return m_mode == Mode::Serializer || space < 0xFFFFFFF0u;
    }

    bool root_flags() {
        const u32 flags = m_desc.flags;
        // Serializer: the D3D12_ROOT_SIGNATURE_FLAGS bits (0x1-0x800) and bit
        // 31 (scan rootsig_flags). The device also takes bits 12-15 (devscan
        // rootsig_flags).
        const u32 unknown = m_mode == Mode::Serializer ? flags & ~0x80000FFFu
                                                       : flags & 0x7FFF0000u;
        if (unknown != 0) return fail(msg::unsupported_root_flags(flags));
        // LOCAL_ROOT_SIGNATURE with any other of those bits -- bit 31 counts,
        // the device's extra bits 12-15 do not (flags_local_plus_*, devscan
        // rootsig_flags 0x1080 / 0x80000080).
        if ((flags & ROOT_SIGNATURE_FLAG_LOCAL_ROOT_SIGNATURE) != 0 &&
            (flags & 0x80000F7Fu) != 0) {
            return fail(msg::local_root_signature(flags));
        }
        return true;
    }

    bool parameter(u32 index) {
        const RootParameter& p = m_desc.parameters[index];
        if (p.shader_visibility > SHADER_VISIBILITY_MESH) {
            return fail(msg::parameter_visibility(p.shader_visibility, index));
        }
        switch (p.parameter_type) {
        case ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE:
            return table(index, p);
        case ROOT_PARAMETER_TYPE_32BIT_CONSTANTS: {
            if (!space_ok(p.register_space)) return fail({});
            if (p.num_32bit_values == 0 && m_mode == Mode::Serializer) {
                // A warning: the call succeeds and the error blob carries it,
                // unless a later error replaces it (zeroconsts_*).
                m_message = msg::kZeroConstants;
            }
            return insert(DESCRIPTOR_RANGE_TYPE_CBV, p.register_space,
                          p.shader_register, p.shader_register,
                          {Occupant::RootParameter, index, 0,
                           p.shader_visibility, p.shader_register});
        }
        case ROOT_PARAMETER_TYPE_CBV:
        case ROOT_PARAMETER_TYPE_SRV:
        case ROOT_PARAMETER_TYPE_UAV: {
            if (!space_ok(p.register_space)) return fail({});
            if (m_version >= ROOT_SIGNATURE_VERSION_1_1 &&
                !root_descriptor_flags(p.flags)) {
                return false;
            }
            const u32 range_class =
                p.parameter_type == ROOT_PARAMETER_TYPE_CBV
                    ? DESCRIPTOR_RANGE_TYPE_CBV
                    : p.parameter_type == ROOT_PARAMETER_TYPE_SRV
                          ? DESCRIPTOR_RANGE_TYPE_SRV
                          : DESCRIPTOR_RANGE_TYPE_UAV;
            return insert(range_class, p.register_space, p.shader_register,
                          p.shader_register,
                          {Occupant::RootParameter, index, 0,
                           p.shader_visibility, p.shader_register});
        }
        default:
            // Reached only at 1.2: below it the conversion to the 1.2 form
            // fails first, with no message (bad_param_type).
            return fail(msg::parameter_type(p.parameter_type, index));
        }
    }

    /// A descriptor table, in Windows' three passes: every range's shape
    /// (order_rangetype_r0_vs_zero_r1, order_offset_vs_register_overflow,
    /// order_append_vs_regoverflow_same ...), then every range's flags
    /// (order_rangeflags_r0_vs_offset_r1), then the overlap checks
    /// (order_rangeflags_vs_overlap_same).
    bool table(u32 index, const RootParameter& p) {
        const std::vector<RootDescriptorRange>& ranges = p.ranges;
        // The next APPEND offset. 2^32 or more -- after an unbounded range,
        // or after a range ending at the top of the table -- cannot be
        // appended to (bounds_append_after_end_of_space).
        u64 next_offset = 0;
        for (u32 slot = 0; slot < ranges.size(); ++slot) {
            const RootDescriptorRange& r = ranges[slot];
            if (r.range_type > DESCRIPTOR_RANGE_TYPE_SAMPLER) {
                return fail(msg::range_type(r.range_type, slot, index));
            }
            if ((r.range_type == DESCRIPTOR_RANGE_TYPE_SAMPLER) !=
                (ranges[0].range_type == DESCRIPTOR_RANGE_TYPE_SAMPLER)) {
                return fail(msg::mixed_samplers(index));
            }
            if (r.num_descriptors == 0) {
                return fail(msg::zero_descriptors(slot, index));
            }
            const bool append =
                r.offset_in_descriptors_from_table_start ==
                DESCRIPTOR_RANGE_OFFSET_APPEND;
            if (append && next_offset > 0xFFFFFFFFull) {
                return fail(msg::append_after_unbounded(slot, index));
            }
            const bool unbounded =
                r.num_descriptors == DESCRIPTOR_RANGE_UNBOUNDED;
            if (!unbounded && static_cast<u64>(r.base_shader_register) +
                                      r.num_descriptors >
                                  0x100000000ull) {
                return fail(msg::register_overflow(r.base_shader_register,
                                                   r.num_descriptors, slot,
                                                   index));
            }
            const u64 start =
                append ? next_offset : r.offset_in_descriptors_from_table_start;
            if (!unbounded && start + r.num_descriptors > 0x100000000ull) {
                return fail(msg::offset_overflow(slot, index));
            }
            next_offset = unbounded ? ~0ull : start + r.num_descriptors;
            if (!space_ok(r.register_space)) return fail({});
        }
        if (m_version >= ROOT_SIGNATURE_VERSION_1_1) {
            for (const RootDescriptorRange& r : ranges) {
                if (!range_flags(r)) return false;
            }
        }
        for (u32 slot = 0; slot < ranges.size(); ++slot) {
            const RootDescriptorRange& r = ranges[slot];
            const u32 last = r.num_descriptors == DESCRIPTOR_RANGE_UNBOUNDED
                                 ? 0xFFFFFFFFu
                                 : r.base_shader_register + r.num_descriptors - 1;
            if (!insert(r.range_type, r.register_space, r.base_shader_register,
                        last,
                        {Occupant::TableRange, index, slot, p.shader_visibility,
                         last})) {
                return false;
            }
        }
        return true;
    }

    /// D3D12_DESCRIPTOR_RANGE_FLAGS (rangeflags_t{0..3}_*). The device accepts
    /// every bit below 0x20000 (devscan range_flags_*).
    bool range_flags(const RootDescriptorRange& r) {
        const u32 flags = r.flags;
        const u32 unknown =
            m_mode == Mode::Serializer ? flags & ~0x1000Fu : flags & ~0x1FFFFu;
        if (unknown != 0) return fail(msg::unsupported_range_flags(flags));
        if ((flags & 0x1u) != 0 && (flags & 0x10000u) != 0) {
            return fail(msg::range_two_descriptor_flags(flags));
        }
        if (r.range_type == DESCRIPTOR_RANGE_TYPE_SAMPLER) {
            if ((flags & 0xEu) != 0) {
                return fail(msg::sampler_range_data_flags(flags));
            }
            if ((flags & 0x10000u) != 0) {
                return fail(msg::sampler_range_bounds_checks(flags));
            }
            return true;
        }
        if (popcount(flags & 0xEu) > 1) {
            return fail(msg::range_two_data_flags(flags));
        }
        if ((flags & 0x1u) != 0 && (flags & 0x8u) != 0) {
            return fail(msg::range_volatile_static(flags));
        }
        return true;
    }

    /// D3D12_ROOT_DESCRIPTOR_FLAGS (rootflags_t{2..4}_*). The device accepts
    /// bits 0-15, DESCRIPTORS_VOLATILE included (devscan
    /// root_descriptor_flags).
    bool root_descriptor_flags(u32 flags) {
        const u32 unknown =
            m_mode == Mode::Serializer ? flags & ~0xEu : flags & 0xFFFF0000u;
        if (unknown != 0) {
            return fail(msg::unsupported_root_descriptor_flags(flags));
        }
        if (popcount(flags & 0xEu) > 1) {
            return fail(msg::root_descriptor_two_data_flags(flags));
        }
        return true;
    }

    /// One static sampler, fully, before the next (order_sampler*): the
    /// visibility, each field in declaration order, the 1.2 flags, the
    /// non-normalized-coordinate rules, then the overlap check.
    bool static_sampler(u32 index) {
        const StaticSampler& s = m_desc.static_samplers[index];
        if (s.shader_visibility > SHADER_VISIBILITY_MESH) {
            return fail(msg::sampler_visibility(s.shader_visibility, index));
        }
        if (!static_filter_recognized(s.filter)) return fail(msg::kFilter);
        const auto address_ok = [](u32 mode) { return mode >= 1 && mode <= 5; };
        if (!address_ok(s.address_u)) return fail(msg::kAddressU);
        if (!address_ok(s.address_v)) return fail(msg::kAddressV);
        if (!address_ok(s.address_w)) return fail(msg::kAddressW);
        // NaN fails too: the comparison is false (float_bias_7FC00000).
        if (!(s.mip_lod_bias >= -16.0f && s.mip_lod_bias <= 15.99f)) {
            return fail(msg::bias(s.mip_lod_bias));
        }
        if (s.max_anisotropy > 16) return fail(msg::anisotropy(s.max_anisotropy));
        if (is_comparison_filter(s.filter) &&
            (s.comparison_func < 1 || s.comparison_func > 8)) {
            return fail(msg::kComparison);
        }
        // Infinities are legal LODs; only NaN is refused.
        if (is_nan(s.min_lod)) return fail(msg::min_lod(s.min_lod));
        if (is_nan(s.max_lod)) return fail(msg::max_lod(s.max_lod));
        if (m_version >= ROOT_SIGNATURE_VERSION_1_2) {
            if ((s.flags & ~0x3u) != 0) return fail(msg::kSamplerFlags);
            if ((s.flags & 0x2u) != 0 && !non_normalized(s)) return false;
        }
        if (!space_ok(s.register_space)) return fail({});
        return insert(DESCRIPTOR_RANGE_TYPE_SAMPLER, s.register_space,
                      s.shader_register, s.shader_register,
                      {Occupant::StaticSampler, index, 0, s.shader_visibility,
                       s.shader_register});
    }

    /// D3D12_SAMPLER_FLAG_NON_NORMALIZED_COORDINATES (nn_*): MIN == MAG, MIP
    /// point, neither comparison nor anisotropic; zero LODs and bias (-0
    /// counts as zero, nn_negzero_lods); U and V CLAMP or BORDER, W free.
    bool non_normalized(const StaticSampler& s) {
        const u32 f = s.filter;
        const bool filter_ok = !is_comparison_filter(f) && (f & 0x40u) == 0 &&
                               ((f >> 4) & 3u) == ((f >> 2) & 3u) &&
                               (f & 3u) == 0;
        if (!filter_ok) return fail(msg::kNonNormalizedFilter);
        if (s.min_lod != 0.0f || s.max_lod != 0.0f || s.mip_lod_bias != 0.0f) {
            return fail(msg::kNonNormalizedLod);
        }
        const auto clamp_or_border = [](u32 mode) {
            return mode == 3 || mode == 4;
        };
        if (!clamp_or_border(s.address_u)) {
            return fail(msg::kNonNormalizedAddressU);
        }
        if (!clamp_or_border(s.address_v)) {
            return fail(msg::kNonNormalizedAddressV);
        }
        return true;
    }

    bool insert(u32 range_class, u32 space, u32 base, u32 last,
                Occupant occupant) {
        occupant.last = last;
        std::string text = m_overlaps.insert(range_class, space, base, occupant);
        if (!text.empty()) return fail(std::move(text));
        return true;
    }

    const RootSignatureDesc& m_desc;
    u32 m_version;
    Mode m_mode;
    std::string m_message;
    OverlapTracker m_overlaps;
};

// =============================================================================
// Blob writing
// =============================================================================

std::vector<u8> write_part(const RootSignatureDesc& desc, u32 version) {
    std::vector<u8> part;
    append_u32(part, version);
    append_u32(part, static_cast<u32>(desc.parameters.size()));
    append_u32(part, 24);
    append_u32(part, static_cast<u32>(desc.static_samplers.size()));
    append_u32(part, 0);  // static samplers offset, patched below
    append_u32(part, desc.flags);
    const usize records = part.size();
    part.resize(records + 12 * desc.parameters.size());
    for (usize i = 0; i < desc.parameters.size(); ++i) {
        const RootParameter& p = desc.parameters[i];
        const u32 payload = static_cast<u32>(part.size());
        u8* record = part.data() + records + 12 * i;
        std::memcpy(record, &p.parameter_type, 4);
        std::memcpy(record + 4, &p.shader_visibility, 4);
        std::memcpy(record + 8, &payload, 4);
        switch (p.parameter_type) {
        case ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE:
            append_u32(part, static_cast<u32>(p.ranges.size()));
            append_u32(part, payload + 8);
            for (const RootDescriptorRange& r : p.ranges) {
                append_u32(part, r.range_type);
                append_u32(part, r.num_descriptors);
                append_u32(part, r.base_shader_register);
                append_u32(part, r.register_space);
                if (version >= ROOT_SIGNATURE_VERSION_1_1) {
                    append_u32(part, r.flags);
                }
                append_u32(part, r.offset_in_descriptors_from_table_start);
            }
            break;
        case ROOT_PARAMETER_TYPE_32BIT_CONSTANTS:
            append_u32(part, p.shader_register);
            append_u32(part, p.register_space);
            append_u32(part, p.num_32bit_values);
            break;
        default:
            append_u32(part, p.shader_register);
            append_u32(part, p.register_space);
            if (version >= ROOT_SIGNATURE_VERSION_1_1) append_u32(part, p.flags);
            break;
        }
    }
    const u32 samplers = static_cast<u32>(part.size());
    std::memcpy(part.data() + 16, &samplers, 4);
    for (const StaticSampler& s : desc.static_samplers) {
        append_u32(part, s.filter);
        append_u32(part, s.address_u);
        append_u32(part, s.address_v);
        append_u32(part, s.address_w);
        append_u32(part, f32_bits(s.mip_lod_bias));
        append_u32(part, s.max_anisotropy);
        append_u32(part, s.comparison_func);
        append_u32(part, s.border_color);
        append_u32(part, f32_bits(s.min_lod));
        append_u32(part, f32_bits(s.max_lod));
        append_u32(part, s.shader_register);
        append_u32(part, s.register_space);
        append_u32(part, s.shader_visibility);
        if (version >= ROOT_SIGNATURE_VERSION_1_2) append_u32(part, s.flags);
    }
    return part;
}

/// A one-part DXBC container around @p part, hashed.
std::vector<u8> wrap_container(const std::vector<u8>& part) {
    std::vector<u8> blob;
    append_u32(blob, kDxbcMagic);
    blob.resize(20, 0);  // hash, filled last
    append_u32(blob, 1);
    const u32 total = static_cast<u32>(kDxbcHeaderBytes + 4 + 8 + part.size());
    append_u32(blob, total);
    append_u32(blob, 1);
    append_u32(blob, kDxbcHeaderBytes + 4);
    append_u32(blob, kRts0);
    append_u32(blob, static_cast<u32>(part.size()));
    blob.insert(blob.end(), part.begin(), part.end());
    graphics::dxbc_container_checksum(blob.data(), total, blob.data() + 4);
    return blob;
}

// =============================================================================
// Blob parsing
// =============================================================================

/// Read a u32 at @p offset + @p at of a part of @p size bytes.
bool part_u32(const u8* part, usize size, u64 offset, u32& out) {
    if (offset + 4 > size) return false;
    out = load_u32(part + offset);
    return true;
}

/// Parse an RTS0 part. False on anything out of bounds, an unknown version or
/// an unknown parameter type (rts_version_*, rts_param0_type_5,
/// rts_*_big).
bool parse_part(const u8* part, usize size, RootSignatureDesc& desc) {
    if (size < 24) return false;
    const u32 version = load_u32(part);
    if (!is_root_signature_version(version)) return false;
    const u32 num_parameters = load_u32(part + 4);
    const u32 parameters_offset = load_u32(part + 8);
    const u32 num_samplers = load_u32(part + 12);
    const u32 samplers_offset = load_u32(part + 16);
    desc = RootSignatureDesc{};
    desc.version = version;
    desc.flags = load_u32(part + 20);
    if (static_cast<u64>(parameters_offset) + 12ull * num_parameters > size) {
        return false;
    }
    desc.parameters.resize(num_parameters);
    for (u32 i = 0; i < num_parameters; ++i) {
        const u8* record = part + parameters_offset + 12ull * i;
        RootParameter& p = desc.parameters[i];
        p.parameter_type = load_u32(record);
        p.shader_visibility = load_u32(record + 4);
        const u64 payload = load_u32(record + 8);
        switch (p.parameter_type) {
        case ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE: {
            u32 count = 0;
            u32 offset = 0;
            if (!part_u32(part, size, payload, count) ||
                !part_u32(part, size, payload + 4, offset)) {
                return false;
            }
            const u64 stride = range_bytes(version);
            if (static_cast<u64>(offset) + stride * count > size) return false;
            p.ranges.resize(count);
            for (u32 r = 0; r < count; ++r) {
                const u8* at = part + offset + stride * r;
                RootDescriptorRange& range = p.ranges[r];
                range.range_type = load_u32(at);
                range.num_descriptors = load_u32(at + 4);
                range.base_shader_register = load_u32(at + 8);
                range.register_space = load_u32(at + 12);
                if (version >= ROOT_SIGNATURE_VERSION_1_1) {
                    range.flags = load_u32(at + 16);
                    range.offset_in_descriptors_from_table_start =
                        load_u32(at + 20);
                } else {
                    range.offset_in_descriptors_from_table_start =
                        load_u32(at + 16);
                }
            }
            break;
        }
        case ROOT_PARAMETER_TYPE_32BIT_CONSTANTS:
            if (payload + 12 > size) return false;
            p.shader_register = load_u32(part + payload);
            p.register_space = load_u32(part + payload + 4);
            p.num_32bit_values = load_u32(part + payload + 8);
            break;
        case ROOT_PARAMETER_TYPE_CBV:
        case ROOT_PARAMETER_TYPE_SRV:
        case ROOT_PARAMETER_TYPE_UAV:
            if (payload + root_descriptor_bytes(version) > size) return false;
            p.shader_register = load_u32(part + payload);
            p.register_space = load_u32(part + payload + 4);
            if (version >= ROOT_SIGNATURE_VERSION_1_1) {
                p.flags = load_u32(part + payload + 8);
            }
            break;
        default:
            return false;
        }
    }
    const u64 stride = sampler_bytes(version);
    if (static_cast<u64>(samplers_offset) + stride * num_samplers > size) {
        return false;
    }
    desc.static_samplers.resize(num_samplers);
    for (u32 i = 0; i < num_samplers; ++i) {
        const u8* at = part + samplers_offset + stride * i;
        StaticSampler& s = desc.static_samplers[i];
        s.filter = load_u32(at);
        s.address_u = load_u32(at + 4);
        s.address_v = load_u32(at + 8);
        s.address_w = load_u32(at + 12);
        s.mip_lod_bias = bits_f32(load_u32(at + 16));
        s.max_anisotropy = load_u32(at + 20);
        s.comparison_func = load_u32(at + 24);
        s.border_color = load_u32(at + 28);
        s.min_lod = bits_f32(load_u32(at + 32));
        s.max_lod = bits_f32(load_u32(at + 36));
        s.shader_register = load_u32(at + 40);
        s.register_space = load_u32(at + 44);
        s.shader_visibility = load_u32(at + 48);
        if (version >= ROOT_SIGNATURE_VERSION_1_2) s.flags = load_u32(at + 52);
    }
    return true;
}

/// The container checks shared by both callers. @p structural is the failure
/// for a malformed part table: E_INVALIDARG for the deserializers, E_FAIL for
/// CreateRootSignature (magic, container_version_*, part_count_*,
/// part_offset_*, fourcc_*, part_size_big / _plus_4). Sets @p part and
/// @p part_size to the first RTS0 part.
u32 find_rts0(const u8* data, usize size, u32 structural,
              bool check_hash, const u8*& part, usize& part_size) {
    if (data == nullptr || size < kDxbcHeaderBytes) return E_INVALIDARG;
    // The size handed in must be the container's own (size_minus_*,
    // size_plus_16, total_size_*): E_INVALIDARG on both paths, and checked
    // before anything else on the device path (bare_rts0_part).
    if (load_u32(data + 24) != size) return E_INVALIDARG;
    if (load_u32(data) != kDxbcMagic) return structural;
    if (check_hash) {
        u8 hash[16];
        graphics::dxbc_container_checksum(data, static_cast<u32>(size), hash);
        if (std::memcmp(hash, data + 4, sizeof(hash)) != 0) return E_INVALIDARG;
    }
    if (load_u32(data + 20) != 1) return structural;
    const u32 count = load_u32(data + 28);
    if (count == 0 ||
        kDxbcHeaderBytes + 4ull * count > static_cast<u64>(size)) {
        return structural;
    }
    part = nullptr;
    for (u32 i = 0; i < count; ++i) {
        const u64 offset = load_u32(data + kDxbcHeaderBytes + 4 * i);
        if (offset + 8 > size) return structural;
        const u64 bytes = load_u32(data + offset + 4);
        if (offset + 8 + bytes > size) return structural;
        if (part == nullptr && load_u32(data + offset) == kRts0) {
            part = data + offset + 8;
            part_size = static_cast<usize>(bytes);
        }
    }
    return part != nullptr ? S_OK : structural;
}

/// 32-bit values the root arguments take (Learn, "Root Signature Limits":
/// 64 DWORDs; a table costs 1, a root descriptor 2, constants 1 each).
u64 root_argument_cost(const RootSignatureDesc& desc) {
    u64 cost = 0;
    for (const RootParameter& p : desc.parameters) {
        switch (p.parameter_type) {
        case ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE: cost += 1; break;
        case ROOT_PARAMETER_TYPE_32BIT_CONSTANTS: cost += p.num_32bit_values; break;
        default: cost += 2; break;
        }
    }
    return cost;
}

} // namespace

bool is_root_signature_version(u32 version) {
    return version >= ROOT_SIGNATURE_VERSION_1_0 &&
           version <= ROOT_SIGNATURE_VERSION_1_2;
}

u32 parse_root_signature_blob(const u8* data, usize size,
                              RootSignatureDesc& desc) {
    const u8* part = nullptr;
    usize part_size = 0;
    const u32 found = find_rts0(data, size, E_INVALIDARG, true, part, part_size);
    if (found != S_OK) return found;
    if (!parse_part(part, part_size, desc)) return E_INVALIDARG;
    // The deserializers drop bit 31 of the root-signature flags, and only that
    // bit (rts_flags_0x80000000 / 0x40000000 / 0xFFFFFFFF -> 0x7FFFFFFF). The
    // serializer writes it and CreateRootSignature validates it.
    desc.flags &= 0x7FFFFFFFu;
    return S_OK;
}

u32 parse_root_signature_blob_for_device(const u8* data, usize size,
                                         RootSignatureDesc& desc) {
    const u8* part = nullptr;
    usize part_size = 0;
    const u32 found = find_rts0(data, size, E_FAIL, false, part, part_size);
    if (found != S_OK) return found;
    if (!parse_part(part, part_size, desc)) return E_INVALIDARG;
    return validate_root_signature_for_device(desc);
}

u32 validate_root_signature_for_device(const RootSignatureDesc& desc) {
    Validator validator(desc, desc.version, Mode::Device);
    const u32 hr = validator.run();
    if (hr != S_OK) return hr;
    // Checked after every E_FAIL rule (devscan cost65_vs_*), and not for a
    // local root signature (devscan cost65_local).
    if ((desc.flags & ROOT_SIGNATURE_FLAG_LOCAL_ROOT_SIGNATURE) == 0 &&
        root_argument_cost(desc) > 64) {
        return E_INVALIDARG;
    }
    return S_OK;
}

u32 serialize_root_signature(const RootSignatureDesc& desc, u32 version,
                             std::vector<u8>& blob,
                             std::string& error_message) {
    blob.clear();
    error_message.clear();
    if (!is_root_signature_version(version)) return E_INVALIDARG;
    // Below 1.2 the desc is first converted to the 1.2 form, and an unknown
    // parameter type fails that conversion with no message -- whatever else
    // is wrong with the desc (order_flags_vs_paramtype, bad_param_type).
    if (version < ROOT_SIGNATURE_VERSION_1_2) {
        for (const RootParameter& p : desc.parameters) {
            if (p.parameter_type > ROOT_PARAMETER_TYPE_UAV) return E_INVALIDARG;
        }
    }
    // Fields the version's structures do not have are not read.
    const RootSignatureDesc view = convert_root_signature(
        [&] {
            RootSignatureDesc copy = desc;
            copy.version = version;
            return copy;
        }(),
        version);
    Validator validator(view, version, Mode::Serializer);
    const u32 hr = validator.run();
    error_message = validator.message();
    if (hr != S_OK) return hr;
    blob = wrap_container(write_part(view, version));
    return S_OK;
}

RootSignatureDesc convert_root_signature(const RootSignatureDesc& desc,
                                         u32 version) {
    RootSignatureDesc out = desc;
    out.version = version;
    const bool from_1_0 = desc.version == ROOT_SIGNATURE_VERSION_1_0;
    for (RootParameter& p : out.parameters) {
        const bool descriptor = p.parameter_type == ROOT_PARAMETER_TYPE_CBV ||
                                p.parameter_type == ROOT_PARAMETER_TYPE_SRV ||
                                p.parameter_type == ROOT_PARAMETER_TYPE_UAV;
        if (version == ROOT_SIGNATURE_VERSION_1_0) {
            p.flags = 0;
        } else if (from_1_0 && descriptor) {
            p.flags = 0x2u;  // DATA_VOLATILE
        }
        if (p.parameter_type != ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE) {
            if (!descriptor) p.flags = 0;
            continue;
        }
        for (RootDescriptorRange& r : p.ranges) {
            if (version == ROOT_SIGNATURE_VERSION_1_0) {
                r.flags = 0;
            } else if (from_1_0) {
                // DESCRIPTORS_VOLATILE (| DATA_VOLATILE unless a sampler
                // range) -- rts_version_1_of_11_data: every range type but 3
                // takes 0x3, invalid types included.
                r.flags = r.range_type == DESCRIPTOR_RANGE_TYPE_SAMPLER ? 0x1u
                                                                        : 0x3u;
            }
        }
    }
    if (version < ROOT_SIGNATURE_VERSION_1_2) {
        for (StaticSampler& s : out.static_samplers) s.flags = 0;
    }
    return out;
}

// =============================================================================
// Guest structures
// =============================================================================

namespace {

/// Offsets in the SDK structures (probe [layout]: x64 then x86).
struct Layout {
    bool is_64bit;
    [[nodiscard]] u32 ptr() const { return is_64bit ? 8u : 4u; }
    // D3D12_ROOT_SIGNATURE_DESC{,1,2}
    [[nodiscard]] u32 desc_size() const { return is_64bit ? 40u : 20u; }
    [[nodiscard]] u32 desc_parameters() const { return is_64bit ? 8u : 4u; }
    [[nodiscard]] u32 desc_num_samplers() const { return is_64bit ? 16u : 8u; }
    [[nodiscard]] u32 desc_samplers() const { return is_64bit ? 24u : 12u; }
    [[nodiscard]] u32 desc_flags() const { return is_64bit ? 32u : 16u; }
    // D3D12_VERSIONED_ROOT_SIGNATURE_DESC
    [[nodiscard]] u32 versioned_size() const { return is_64bit ? 48u : 24u; }
    [[nodiscard]] u32 versioned_desc() const { return is_64bit ? 8u : 4u; }
    // D3D12_ROOT_PARAMETER{,1}: the union starts at 8 / 4; a table's range
    // pointer sits one pointer-aligned slot after its count.
    [[nodiscard]] u32 parameter_size() const { return is_64bit ? 32u : 20u; }
    [[nodiscard]] u32 parameter_union() const { return is_64bit ? 8u : 4u; }
    [[nodiscard]] u32 parameter_ranges() const { return is_64bit ? 16u : 8u; }
    [[nodiscard]] u32 parameter_visibility() const {
        return is_64bit ? 24u : 16u;
    }
};

bool read_u32_at(const GuestMemory& memory, GuestAddr address, u32& out) {
    auto value = memory.read_u32(address);
    if (!value.is_ok()) return false;
    out = value.value();
    return true;
}

bool read_ptr_at(const GuestMemory& memory, GuestAddr address, bool is_64bit,
                 GuestAddr& out) {
    if (is_64bit) {
        auto value = memory.read_u64(address);
        if (!value.is_ok()) return false;
        out = value.value();
        return true;
    }
    u32 value = 0;
    if (!read_u32_at(memory, address, value)) return false;
    out = value;
    return true;
}

bool read_words(const GuestMemory& memory, GuestAddr address, u32* words,
                usize count) {
    return memory
        .read_bytes(address, reinterpret_cast<Byte*>(words), count * 4)
        .is_ok();
}

usize align_up(usize value, usize alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

} // namespace

bool read_guest_root_signature_desc(const GuestMemory& memory,
                                    GuestAddr address, u32 version,
                                    bool is_64bit, RootSignatureDesc& desc) {
    const Layout l{is_64bit};
    desc = RootSignatureDesc{};
    desc.version = version;
    u32 num_parameters = 0;
    u32 num_samplers = 0;
    GuestAddr parameters = 0;
    GuestAddr samplers = 0;
    if (address == 0 || !read_u32_at(memory, address, num_parameters) ||
        !read_ptr_at(memory, address + l.desc_parameters(), is_64bit,
                     parameters) ||
        !read_u32_at(memory, address + l.desc_num_samplers(), num_samplers) ||
        !read_ptr_at(memory, address + l.desc_samplers(), is_64bit, samplers) ||
        !read_u32_at(memory, address + l.desc_flags(), desc.flags)) {
        return false;
    }
    if ((num_parameters != 0 && parameters == 0) ||
        (num_samplers != 0 && samplers == 0)) {
        return false;
    }
    desc.parameters.resize(num_parameters);
    for (u32 i = 0; i < num_parameters; ++i) {
        const GuestAddr at = parameters + static_cast<u64>(i) * l.parameter_size();
        RootParameter& p = desc.parameters[i];
        u32 fields[3] = {};
        if (!read_u32_at(memory, at, p.parameter_type) ||
            !read_u32_at(memory, at + l.parameter_visibility(),
                         p.shader_visibility) ||
            !read_words(memory, at + l.parameter_union(), fields, 3)) {
            return false;
        }
        switch (p.parameter_type) {
        case ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE: {
            const u32 count = fields[0];
            GuestAddr ranges = 0;
            if (!read_ptr_at(memory, at + l.parameter_ranges(), is_64bit,
                             ranges) ||
                (count != 0 && ranges == 0)) {
                return false;
            }
            const u32 stride = range_bytes(version);
            std::vector<u32> words(static_cast<usize>(count) * (stride / 4));
            if (count != 0 &&
                !read_words(memory, ranges, words.data(), words.size())) {
                return false;
            }
            p.ranges.resize(count);
            for (u32 r = 0; r < count; ++r) {
                const u32* w = words.data() + static_cast<usize>(r) * (stride / 4);
                RootDescriptorRange& range = p.ranges[r];
                range.range_type = w[0];
                range.num_descriptors = w[1];
                range.base_shader_register = w[2];
                range.register_space = w[3];
                if (version >= ROOT_SIGNATURE_VERSION_1_1) {
                    range.flags = w[4];
                    range.offset_in_descriptors_from_table_start = w[5];
                } else {
                    range.offset_in_descriptors_from_table_start = w[4];
                }
            }
            break;
        }
        case ROOT_PARAMETER_TYPE_32BIT_CONSTANTS:
            p.shader_register = fields[0];
            p.register_space = fields[1];
            p.num_32bit_values = fields[2];
            break;
        case ROOT_PARAMETER_TYPE_CBV:
        case ROOT_PARAMETER_TYPE_SRV:
        case ROOT_PARAMETER_TYPE_UAV:
            p.shader_register = fields[0];
            p.register_space = fields[1];
            if (version >= ROOT_SIGNATURE_VERSION_1_1) p.flags = fields[2];
            break;
        default:
            break;
        }
    }
    const u32 stride = sampler_bytes(version);
    std::vector<u32> words(static_cast<usize>(num_samplers) * (stride / 4));
    if (num_samplers != 0 &&
        !read_words(memory, samplers, words.data(), words.size())) {
        return false;
    }
    desc.static_samplers.resize(num_samplers);
    for (u32 i = 0; i < num_samplers; ++i) {
        const u32* w = words.data() + static_cast<usize>(i) * (stride / 4);
        StaticSampler& s = desc.static_samplers[i];
        s.filter = w[0];
        s.address_u = w[1];
        s.address_v = w[2];
        s.address_w = w[3];
        s.mip_lod_bias = bits_f32(w[4]);
        s.max_anisotropy = w[5];
        s.comparison_func = w[6];
        s.border_color = w[7];
        s.min_lod = bits_f32(w[8]);
        s.max_lod = bits_f32(w[9]);
        s.shader_register = w[10];
        s.register_space = w[11];
        s.shader_visibility = w[12];
        if (version >= ROOT_SIGNATURE_VERSION_1_2) s.flags = w[13];
    }
    return true;
}

bool read_guest_versioned_root_signature_desc(const GuestMemory& memory,
                                              GuestAddr address, bool is_64bit,
                                              RootSignatureDesc& desc) {
    const Layout l{is_64bit};
    u32 version = 0;
    if (address == 0 || !read_u32_at(memory, address, version)) return false;
    if (!is_root_signature_version(version)) {
        desc = RootSignatureDesc{};
        desc.version = version;
        return true;
    }
    return read_guest_root_signature_desc(memory, address + l.versioned_desc(),
                                          version, is_64bit, desc);
}

usize guest_root_signature_desc_size(const RootSignatureDesc& desc,
                                     u32 version, bool is_64bit,
                                     bool versioned) {
    const Layout l{is_64bit};
    usize size = versioned ? l.versioned_size() : l.desc_size();
    size = align_up(size, 8) + desc.parameters.size() * l.parameter_size();
    for (const RootParameter& p : desc.parameters) {
        if (p.parameter_type == ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE) {
            size += p.ranges.size() * range_bytes(version);
        }
    }
    return size + desc.static_samplers.size() * sampler_bytes(version);
}

bool write_guest_root_signature_desc(GuestMemory& memory, GuestAddr address,
                                     const RootSignatureDesc& desc, u32 version,
                                     bool is_64bit, bool versioned) {
    const Layout l{is_64bit};
    const usize total =
        guest_root_signature_desc_size(desc, version, is_64bit, versioned);
    std::vector<u8> block(total, 0);
    const auto put32 = [&](usize at, u32 value) {
        std::memcpy(block.data() + at, &value, 4);
    };
    const auto put_ptr = [&](usize at, GuestAddr value) {
        if (is_64bit) {
            std::memcpy(block.data() + at, &value, 8);
        } else {
            put32(at, static_cast<u32>(value));
        }
    };
    const usize base = versioned ? l.versioned_desc() : 0;
    if (versioned) put32(0, version);
    usize cursor = align_up(versioned ? l.versioned_size() : l.desc_size(), 8);
    const usize parameters = cursor;
    cursor += desc.parameters.size() * l.parameter_size();
    put32(base, static_cast<u32>(desc.parameters.size()));
    put_ptr(base + l.desc_parameters(),
            desc.parameters.empty() ? 0 : address + parameters);
    put32(base + l.desc_num_samplers(),
          static_cast<u32>(desc.static_samplers.size()));
    put32(base + l.desc_flags(), desc.flags);
    for (usize i = 0; i < desc.parameters.size(); ++i) {
        const RootParameter& p = desc.parameters[i];
        const usize at = parameters + i * l.parameter_size();
        put32(at, p.parameter_type);
        put32(at + l.parameter_visibility(), p.shader_visibility);
        const usize u = at + l.parameter_union();
        switch (p.parameter_type) {
        case ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE:
            put32(u, static_cast<u32>(p.ranges.size()));
            put_ptr(at + l.parameter_ranges(),
                    p.ranges.empty() ? 0 : address + cursor);
            for (const RootDescriptorRange& r : p.ranges) {
                put32(cursor, r.range_type);
                put32(cursor + 4, r.num_descriptors);
                put32(cursor + 8, r.base_shader_register);
                put32(cursor + 12, r.register_space);
                if (version >= ROOT_SIGNATURE_VERSION_1_1) {
                    put32(cursor + 16, r.flags);
                    put32(cursor + 20, r.offset_in_descriptors_from_table_start);
                } else {
                    put32(cursor + 16, r.offset_in_descriptors_from_table_start);
                }
                cursor += range_bytes(version);
            }
            break;
        case ROOT_PARAMETER_TYPE_32BIT_CONSTANTS:
            put32(u, p.shader_register);
            put32(u + 4, p.register_space);
            put32(u + 8, p.num_32bit_values);
            break;
        default:
            put32(u, p.shader_register);
            put32(u + 4, p.register_space);
            if (version >= ROOT_SIGNATURE_VERSION_1_1) put32(u + 8, p.flags);
            break;
        }
    }
    put_ptr(base + l.desc_samplers(),
            desc.static_samplers.empty() ? 0 : address + cursor);
    for (const StaticSampler& s : desc.static_samplers) {
        const u32 words[14] = {
            s.filter, s.address_u, s.address_v, s.address_w,
            f32_bits(s.mip_lod_bias), s.max_anisotropy, s.comparison_func,
            s.border_color, f32_bits(s.min_lod), f32_bits(s.max_lod),
            s.shader_register, s.register_space, s.shader_visibility, s.flags};
        std::memcpy(block.data() + cursor, words, sampler_bytes(version));
        cursor += sampler_bytes(version);
    }
    return memory
        .write_bytes(address, reinterpret_cast<const Byte*>(block.data()),
                     block.size())
        .is_ok();
}

} // namespace vitreo::winapi::d3d12

// =============================================================================
// d3d12.dll: exports and COM objects
// =============================================================================

namespace vitreo::winapi {
namespace {

using d3d12::GuidBytes;
using d3d12::guid_equal;
using d3d12::d3d12_diag;

/// Guest memory for blob bytes and deserializer descs, page-granular like the
/// D3D10 blobs: a root signature is created a handful of times per pipeline,
/// not per frame.
constexpr GuestAddr kDataFloor = 0x10000000ull;

usize page_round(usize size) {
    return std::max<usize>((size + 0xFFFu) & ~usize{0xFFF}, 0x1000u);
}

/// A serialized root signature -- or shader bytecode carrying one -- larger
/// than this is refused before it is copied out of the guest.
constexpr u64 kMaxSourceBytes = 256ull << 20;

} // namespace

GuestAddr D3D12Manager::create_blob(const u8* bytes, usize size) {
    const std::lock_guard<std::recursive_mutex> lock(m_root_signature_mutex);
    const usize mapped = page_round(size);
    const GuestAddr data =
        m_memory.map_first_fit(kDataFloor, mapped, MemoryProtection::ReadWrite);
    if (data == 0) return 0;
    if (size != 0 &&
        m_memory.write_bytes(data, reinterpret_cast<const Byte*>(bytes), size)
            .is_err()) {
        (void)m_memory.unmap_region(data, mapped);
        return 0;
    }
    const GuestAddr address = allocate_object(&D3D12Manager::m_blob_vtable);
    if (address == 0) {
        (void)m_memory.unmap_region(data, mapped);
        return 0;
    }
    m_blobs.emplace(address, Blob{1, data, size, mapped});
    return address;
}

u32 D3D12Manager::release_blob(GuestAddr address) {
    const std::lock_guard<std::recursive_mutex> lock(m_root_signature_mutex);
    auto blob = m_blobs.find(address);
    if (blob == m_blobs.end() || blob->second.ref_count == 0) return 0;
    const u32 count = --blob->second.ref_count;
    if (count == 0) {
        const Blob dead = blob->second;
        m_blobs.erase(blob);
        (void)m_memory.unmap_region(dead.data, dead.mapped);
        free_object(address);
    }
    return count;
}

u32 D3D12Manager::release_root_signature_deserializer(GuestAddr address) {
    const std::lock_guard<std::recursive_mutex> lock(m_root_signature_mutex);
    auto object = m_root_signature_deserializers.find(address);
    if (object == m_root_signature_deserializers.end() ||
        object->second.ref_count == 0) {
        return 0;
    }
    const u32 count = --object->second.ref_count;
    if (count == 0) {
        const RootSignatureDeserializer dead = std::move(object->second);
        m_root_signature_deserializers.erase(object);
        for (usize v = 0; v < dead.descs.size(); ++v) {
            if (dead.descs[v] != 0) {
                (void)m_memory.unmap_region(dead.descs[v], dead.mapped[v]);
            }
        }
        free_object(address);
    }
    return count;
}

GuestAddr D3D12Manager::root_signature_desc_at(
    RootSignatureDeserializer& object, u32 version) {
    if (object.descs[version] != 0) return object.descs[version];
    const d3d12::RootSignatureDesc desc =
        version == object.desc.version
            ? object.desc
            : d3d12::convert_root_signature(object.desc, version);
    const usize size = d3d12::guest_root_signature_desc_size(
        desc, version, m_is_64bit, object.versioned);
    const usize mapped = page_round(size);
    const GuestAddr address =
        m_memory.map_first_fit(kDataFloor, mapped, MemoryProtection::ReadWrite);
    if (address == 0) return 0;
    if (!d3d12::write_guest_root_signature_desc(m_memory, address, desc,
                                                version, m_is_64bit,
                                                object.versioned)) {
        (void)m_memory.unmap_region(address, mapped);
        return 0;
    }
    object.descs[version] = address;
    object.mapped[version] = mapped;
    return address;
}

// -----------------------------------------------------------------------------
// D3D12SerializeRootSignature / D3D12SerializeVersionedRootSignature
// -----------------------------------------------------------------------------

void D3D12Manager::impl_D3D12SerializeRootSignature(Marshaler& m) {
    serialize_root_signature_export(m, false);
}

void D3D12Manager::impl_D3D12SerializeVersionedRootSignature(Marshaler& m) {
    serialize_root_signature_export(m, true);
}

void D3D12Manager::serialize_root_signature_export(Marshaler& m,
                                                   bool versioned) {
    // (pRootSignature, [Version,] ppBlob, ppErrorBlob)
    const GuestAddr desc_address = m.arg_ptr(0);
    const u32 requested = versioned ? 0 : m.arg32(1);
    const GuestAddr blob_out = m.arg_ptr(versioned ? 1 : 2);
    const GuestAddr error_out = m.arg_ptr(versioned ? 2 : 3);
    // A NULL desc or ppBlob touches neither out-parameter ([null],
    // ok_null_blob_out*).
    if (desc_address == 0 || blob_out == 0) {
        m.set_return(d3d12::E_INVALIDARG);
        return;
    }
    // The versioned export clears both out-parameters first; the 1.0 one
    // clears only the error blob and never writes *ppBlob on failure (every
    // S10 failure in the probe reads "blob=untouched").
    if (versioned) (void)write_pointer(blob_out, 0);
    if (error_out != 0) (void)write_pointer(error_out, 0);

    d3d12::RootSignatureDesc desc;
    std::vector<u8> bytes;
    std::string message;
    u32 hr = d3d12::E_INVALIDARG;
    if (!versioned && requested != d3d12::ROOT_SIGNATURE_VERSION_1_0) {
        // version_*_s10.
        message = d3d12::format(
            "D3DSerializeRootSignature: unsupported root signature version "
            "(%u)\n",
            requested);
    } else if (!(versioned
                     ? d3d12::read_guest_versioned_root_signature_desc(
                           m_memory, desc_address, m_is_64bit, desc)
                     : d3d12::read_guest_root_signature_desc(
                           m_memory, desc_address,
                           d3d12::ROOT_SIGNATURE_VERSION_1_0, m_is_64bit,
                           desc))) {
        // A NULL array with a non-zero count: Windows dereferences it and
        // faults inside d3d12.dll (probe "crash 0-2"). Refuse instead.
        d3d12_diag("D3D12Serialize%sRootSignature: unreadable desc -> "
                   "E_INVALIDARG",
                   versioned ? "Versioned" : "");
    } else {
        hr = d3d12::serialize_root_signature(desc, desc.version, bytes,
                                             message);
    }
    if (hr == d3d12::S_OK) {
        const GuestAddr blob = create_blob(bytes.data(), bytes.size());
        if (blob == 0) {
            m.set_return(d3d12::E_OUTOFMEMORY);
            return;
        }
        (void)write_pointer(blob_out, blob);
    }
    if (!message.empty() && error_out != 0) {
        // The error blob is the text and its terminating NUL.
        const GuestAddr error = create_blob(
            reinterpret_cast<const u8*>(message.c_str()), message.size() + 1);
        if (error != 0) (void)write_pointer(error_out, error);
    }
    d3d12_diag("D3D12Serialize%sRootSignature -> 0x%08x%s%s",
               versioned ? "Versioned" : "", hr, message.empty() ? "" : ": ",
               message.c_str());
    m.set_return(hr);
}

// -----------------------------------------------------------------------------
// D3D12Create(Versioned)RootSignatureDeserializer
// -----------------------------------------------------------------------------

void D3D12Manager::impl_D3D12CreateRootSignatureDeserializer(Marshaler& m) {
    create_root_signature_deserializer(m, false);
}

void D3D12Manager::impl_D3D12CreateVersionedRootSignatureDeserializer(
    Marshaler& m) {
    create_root_signature_deserializer(m, true);
}

void D3D12Manager::create_root_signature_deserializer(Marshaler& m,
                                                      bool versioned) {
    const std::lock_guard<std::recursive_mutex> lock(m_root_signature_mutex);
    // (pSrcData, SrcDataSizeInBytes, riid, ppRootSignatureDeserializer)
    const GuestAddr data = m.arg_ptr(0);
    const u64 size = m.arg(1);  // SIZE_T
    const GuestAddr iid_address = m.arg_ptr(2);
    const GuestAddr out = m.arg_ptr(3);
    // Measured order ([ident]): a NULL out-pointer first, then the data, and
    // only then the interface; *ppv is NULL on every failure.
    if (out == 0) {
        m.set_return(d3d12::E_INVALIDARG);
        return;
    }
    (void)write_pointer(out, 0);
    std::vector<u8> bytes;
    if (data == 0 || size == 0 || size > kMaxSourceBytes) {
        m.set_return(d3d12::E_INVALIDARG);
        return;
    }
    bytes.resize(static_cast<usize>(size));
    if (m_memory.read_bytes(data, reinterpret_cast<Byte*>(bytes.data()),
                            bytes.size())
            .is_err()) {
        m.set_return(d3d12::E_INVALIDARG);
        return;
    }
    d3d12::RootSignatureDesc desc;
    const u32 parsed =
        d3d12::parse_root_signature_blob(bytes.data(), bytes.size(), desc);
    if (parsed != d3d12::S_OK) {
        d3d12_diag("D3D12Create%sRootSignatureDeserializer: blob refused",
                   versioned ? "Versioned" : "");
        m.set_return(parsed);
        return;
    }
    // Each creator hands out its own interface only; IUnknown is refused too.
    GuidBytes iid{};
    const GuidBytes& own =
        versioned ? d3d12::IID_ID3D12VERSIONEDROOTSIGNATUREDESERIALIZER
                  : d3d12::IID_ID3D12ROOTSIGNATUREDESERIALIZER;
    if (!read_guid(iid_address, iid) || !guid_equal(iid, own)) {
        m.set_return(d3d12::E_NOINTERFACE);
        return;
    }
    const GuestAddr address = allocate_object(
        versioned ? &D3D12Manager::m_versioned_root_signature_deserializer_vtable
                  : &D3D12Manager::m_root_signature_deserializer_vtable);
    if (address == 0) {
        m.set_return(d3d12::E_OUTOFMEMORY);
        return;
    }
    RootSignatureDeserializer object;
    object.versioned = versioned;
    object.desc = std::move(desc);
    auto& stored =
        m_root_signature_deserializers.emplace(address, std::move(object))
            .first->second;
    // The desc a caller first asks for exists before the object is handed
    // out, so GetRootSignatureDesc / GetUnconvertedRootSignatureDesc, which
    // have no way to fail, never need to allocate.
    const u32 first = versioned ? stored.desc.version
                                : d3d12::ROOT_SIGNATURE_VERSION_1_0;
    if (root_signature_desc_at(stored, first) == 0 ||
        !write_pointer(out, address)) {
        (void)release_root_signature_deserializer(address);
        (void)write_pointer(out, 0);
        m.set_return(d3d12::E_OUTOFMEMORY);
        return;
    }
    m.set_return(d3d12::S_OK);
}

// -----------------------------------------------------------------------------
// COM methods
// -----------------------------------------------------------------------------

void D3D12Manager::impl_BlobMethod(Marshaler& m, usize slot) {
    const std::lock_guard<std::recursive_mutex> lock(m_root_signature_mutex);
    const GuestAddr self = m.arg_ptr(0);
    auto blob = m_blobs.find(self);
    if (blob == m_blobs.end()) {
        m.set_return(slot == 1 || slot == 2 ? 0 : d3d12::E_INVALIDARG);
        return;
    }
    switch (slot) {
    case 0: {
        // IUnknown and ID3D10Blob (= ID3DBlob), both answering the object
        // itself ([blob]: out=self for a serialized blob and an error blob).
        // Windows dereferences a NULL out-pointer (probe "crash 13"); refuse.
        const GuestAddr out = m.arg_ptr(2);
        if (out == 0) {
            m.set_return(d3d12::E_POINTER);
            return;
        }
        GuidBytes iid{};
        if (!read_guid(m.arg_ptr(1), iid) ||
            (!guid_equal(iid, d3d12::IID_IUNKNOWN) &&
             !guid_equal(iid, d3d12::IID_ID3D10BLOB))) {
            (void)write_pointer(out, 0);
            m.set_return(d3d12::E_NOINTERFACE);
            return;
        }
        ++blob->second.ref_count;
        (void)write_pointer(out, self);
        m.set_return(d3d12::S_OK);
        return;
    }
    case 1:
        m.set_return(++blob->second.ref_count);
        return;
    case 2:
        m.set_return(release_blob(self));
        return;
    case 3:  // GetBufferPointer
        m.set_return(blob->second.data);
        return;
    case 4:  // GetBufferSize (SIZE_T)
        m.set_return(blob->second.size);
        return;
    default:
        m.set_return(0);
        return;
    }
}

void D3D12Manager::impl_RootSignatureDeserializerMethod(Marshaler& m,
                                                        usize slot) {
    const std::lock_guard<std::recursive_mutex> lock(m_root_signature_mutex);
    const GuestAddr self = m.arg_ptr(0);
    auto object = m_root_signature_deserializers.find(self);
    if (object == m_root_signature_deserializers.end() ||
        object->second.versioned) {
        m.set_return(slot == 1 || slot == 2 ? 0 : d3d12::E_INVALIDARG);
        return;
    }
    switch (slot) {
    case 0: {
        // Only ID3D12RootSignatureDeserializer: IUnknown is E_NOINTERFACE,
        // and a NULL out-pointer is E_FAIL whatever the interface ([ident]
        // qi(own) / qi(Bogus) ppv=null).
        const GuestAddr out = m.arg_ptr(2);
        if (out == 0) {
            m.set_return(d3d12::E_FAIL);
            return;
        }
        GuidBytes iid{};
        if (!read_guid(m.arg_ptr(1), iid) ||
            !guid_equal(iid, d3d12::IID_ID3D12ROOTSIGNATUREDESERIALIZER)) {
            (void)write_pointer(out, 0);
            m.set_return(d3d12::E_NOINTERFACE);
            return;
        }
        ++object->second.ref_count;
        (void)write_pointer(out, self);
        m.set_return(d3d12::S_OK);
        return;
    }
    case 1:
        m.set_return(++object->second.ref_count);
        return;
    case 2:
        m.set_return(release_root_signature_deserializer(self));
        return;
    case 3:  // GetRootSignatureDesc: the desc at 1.0, the same pointer each call
        m.set_return(object->second.descs[d3d12::ROOT_SIGNATURE_VERSION_1_0]);
        return;
    default:
        m.set_return(0);
        return;
    }
}

void D3D12Manager::impl_VersionedRootSignatureDeserializerMethod(Marshaler& m,
                                                                 usize slot) {
    const std::lock_guard<std::recursive_mutex> lock(m_root_signature_mutex);
    const GuestAddr self = m.arg_ptr(0);
    auto object = m_root_signature_deserializers.find(self);
    if (object == m_root_signature_deserializers.end() ||
        !object->second.versioned) {
        m.set_return(slot == 1 || slot == 2 ? 0 : d3d12::E_INVALIDARG);
        return;
    }
    switch (slot) {
    case 0: {
        const GuestAddr out = m.arg_ptr(2);
        if (out == 0) {
            m.set_return(d3d12::E_FAIL);  // as the 1.0 deserializer ([ident])
            return;
        }
        GuidBytes iid{};
        if (!read_guid(m.arg_ptr(1), iid) ||
            !guid_equal(iid,
                        d3d12::IID_ID3D12VERSIONEDROOTSIGNATUREDESERIALIZER)) {
            (void)write_pointer(out, 0);
            m.set_return(d3d12::E_NOINTERFACE);
            return;
        }
        ++object->second.ref_count;
        (void)write_pointer(out, self);
        m.set_return(d3d12::S_OK);
        return;
    }
    case 1:
        m.set_return(++object->second.ref_count);
        return;
    case 2:
        m.set_return(release_root_signature_deserializer(self));
        return;
    case 3: {
        // GetRootSignatureDescAtVersion(convertToVersion, ppDesc). An unknown
        // version is E_FAIL with *ppDesc untouched (AtVersion 0 / 4). Every
        // version asked for stays valid until the final Release (Learn), and
        // the version the blob holds answers the unconverted desc itself.
        const u32 version = m.arg32(1);
        const GuestAddr out = m.arg_ptr(2);
        if (!d3d12::is_root_signature_version(version)) {
            m.set_return(d3d12::E_FAIL);
            return;
        }
        if (out == 0) {
            // Windows writes through it and faults (probe "crash 15").
            m.set_return(d3d12::E_POINTER);
            return;
        }
        const GuestAddr desc = root_signature_desc_at(object->second, version);
        if (desc == 0) {
            m.set_return(d3d12::E_OUTOFMEMORY);
            return;
        }
        (void)write_pointer(out, desc);
        m.set_return(d3d12::S_OK);
        return;
    }
    case 4:  // GetUnconvertedRootSignatureDesc
        m.set_return(object->second.descs[object->second.desc.version]);
        return;
    default:
        m.set_return(0);
        return;
    }
}

void append_d3d12_root_signature_thunks(DllExports& exports,
                                        D3D12Manager& manager) {
    const auto add_vtable = [&](const char* interface_name, usize count,
                                const u16* stdcall_bytes,
                                void (D3D12Manager::*method)(Marshaler&,
                                                             usize)) {
        for (usize slot = 0; slot < count; ++slot) {
            exports.entries.push_back({
                d3d12_method_export_name(interface_name, slot), 0,
                [&manager, method, slot](Marshaler& m) {
                    (manager.*method)(m, slot);
                }, stdcall_bytes[slot]});
        }
    };
    add_vtable("Blob", d3d12::BLOB_METHOD_COUNT,
               d3d12::BLOB_STDCALL_BYTES.data(), &D3D12Manager::impl_BlobMethod);
    add_vtable("RootSignatureDeserializer",
               d3d12::ROOT_SIGNATURE_DESERIALIZER_METHOD_COUNT,
               d3d12::ROOT_SIGNATURE_DESERIALIZER_STDCALL_BYTES.data(),
               &D3D12Manager::impl_RootSignatureDeserializerMethod);
    add_vtable("VersionedRootSignatureDeserializer",
               d3d12::VERSIONED_ROOT_SIGNATURE_DESERIALIZER_METHOD_COUNT,
               d3d12::VERSIONED_ROOT_SIGNATURE_DESERIALIZER_STDCALL_BYTES.data(),
               &D3D12Manager::impl_VersionedRootSignatureDeserializerMethod);
}

} // namespace vitreo::winapi
