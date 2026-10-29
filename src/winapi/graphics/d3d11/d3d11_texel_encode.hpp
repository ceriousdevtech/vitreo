#pragma once
/// @file d3d11_texel_encode.hpp
/// @brief DXGI texel layouts and the D3D clear-value conversions into them.
///
/// Every D3D11 clear stores one texel value converted into the *view's* format
/// (D3D11.3 5.2.3.1: "converted to the fully qualified format type of the
/// View"). The conversions are the D3D11.3 functional spec's, measured on
/// Windows where the spec leaves room
/// (tools/oracle_probe/d3d11_a06_resource_ops_probe.expected, RTV/UAV/CLV):
///
///   * ClearUnorderedAccessViewUint (5.2.3.2): "copying the lower n_i bits from
///     each array element i to the corresponding channel", no conversion.
///   * ClearUnorderedAccessViewFloat (5.2.3.2): FLOAT, UNORM and SNORM views
///     only, with the 3.2.2/3.2.3 conversions; any other view is dropped.
///   * ClearView (5.2.3.3): as the Float form, except integer channels take the
///     colour "as integral floats ... fractions rounded to zero, out of
///     range/INF values clamped to target range, NaN to 0".
///
/// Conversions (3.2.2, 3.2.3.4/.6/.8). WARP rounds FLOAT -> *NORM/SRGB to the
/// nearest integer with ties to even -- the 3.2.2 default, inside the 0.6 ULP
/// tolerance the per-format sections allow (UNORM8 1.5/255 -> 2, 2.5/255 -> 2).
/// FLOAT -> smaller float rounds toward zero and a finite overflow saturates
/// at the largest finite value, "NOT including signed infinity"; NaN becomes
/// NaN (0x7E00 for float16, all ones for the unsigned 11/10-bit floats), and a
/// format without a sign stores 0 for any negative input.
///
/// Pure functions of their inputs, so the unit tests can hold them to the
/// oracle's bytes without a GPU.

#include <vitreo/types.hpp>

#include <array>
#include <cmath>
#include <cstring>
#include <limits>

namespace vitreo::winapi::d3d11_texel {

enum class ChannelKind : u8 { Unorm, Snorm, Uint, Sint, Float, Srgb };

/// One stored channel: which colour component feeds it (0 R, 1 G, 2 B, 3 A),
/// where it sits in the little-endian texel, and how it is typed.
struct Channel {
    u8 component{0};
    u8 offset{0};
    u8 bits{0};
    ChannelKind kind{ChannelKind::Uint};
};

struct TexelLayout {
    u32 dxgi_format{0};
    u8 texel_bytes{0};
    u8 channel_count{0};
    std::array<Channel, 4> channels{};
};

namespace detail {
constexpr TexelLayout rgba(u32 format, u8 bits, ChannelKind kind,
                           u8 count = 4) {
    TexelLayout layout{format, static_cast<u8>(bits * count / 8), count, {}};
    for (u8 i = 0; i < count; ++i) {
        layout.channels[i] = {i, static_cast<u8>(i * bits), bits, kind};
    }
    return layout;
}
constexpr TexelLayout srgb_rgba8(u32 format) {
    TexelLayout layout = rgba(format, 8, ChannelKind::Srgb);
    layout.channels[3].kind = ChannelKind::Unorm;  // alpha is linear
    return layout;
}
constexpr TexelLayout bgra8(u32 format, ChannelKind rgb, bool alpha) {
    TexelLayout layout{format, 4, static_cast<u8>(alpha ? 4 : 3), {}};
    layout.channels[0] = {2, 0, 8, rgb};
    layout.channels[1] = {1, 8, 8, rgb};
    layout.channels[2] = {0, 16, 8, rgb};
    if (alpha) layout.channels[3] = {3, 24, 8, ChannelKind::Unorm};
    return layout;
}
constexpr TexelLayout packed(u32 format, u8 texel_bytes,
                             std::array<Channel, 4> channels, u8 count) {
    return TexelLayout{format, texel_bytes, count, channels};
}
using K = ChannelKind;
inline constexpr TexelLayout kLayouts[] = {
    rgba(2, 32, K::Float), rgba(3, 32, K::Uint), rgba(4, 32, K::Sint),
    rgba(6, 32, K::Float, 3), rgba(7, 32, K::Uint, 3), rgba(8, 32, K::Sint, 3),
    rgba(10, 16, K::Float), rgba(11, 16, K::Unorm), rgba(12, 16, K::Uint),
    rgba(13, 16, K::Snorm), rgba(14, 16, K::Sint),
    rgba(16, 32, K::Float, 2), rgba(17, 32, K::Uint, 2),
    rgba(18, 32, K::Sint, 2),
    packed(24, 4, {{{0, 0, 10, K::Unorm}, {1, 10, 10, K::Unorm},
                    {2, 20, 10, K::Unorm}, {3, 30, 2, K::Unorm}}}, 4),
    packed(25, 4, {{{0, 0, 10, K::Uint}, {1, 10, 10, K::Uint},
                    {2, 20, 10, K::Uint}, {3, 30, 2, K::Uint}}}, 4),
    packed(26, 4, {{{0, 0, 11, K::Float}, {1, 11, 11, K::Float},
                    {2, 22, 10, K::Float}, {}}}, 3),
    rgba(28, 8, K::Unorm), srgb_rgba8(29), rgba(30, 8, K::Uint),
    rgba(31, 8, K::Snorm), rgba(32, 8, K::Sint),
    rgba(34, 16, K::Float, 2), rgba(35, 16, K::Unorm, 2),
    rgba(36, 16, K::Uint, 2), rgba(37, 16, K::Snorm, 2),
    rgba(38, 16, K::Sint, 2),
    rgba(41, 32, K::Float, 1), rgba(42, 32, K::Uint, 1),
    rgba(43, 32, K::Sint, 1),
    rgba(49, 8, K::Unorm, 2), rgba(50, 8, K::Uint, 2),
    rgba(51, 8, K::Snorm, 2), rgba(52, 8, K::Sint, 2),
    rgba(54, 16, K::Float, 1), rgba(56, 16, K::Unorm, 1),
    rgba(57, 16, K::Uint, 1), rgba(58, 16, K::Snorm, 1),
    rgba(59, 16, K::Sint, 1),
    rgba(61, 8, K::Unorm, 1), rgba(62, 8, K::Uint, 1),
    rgba(63, 8, K::Snorm, 1), rgba(64, 8, K::Sint, 1),
    // A8_UNORM stores the A component (RTV oracle: Color[3]).
    packed(65, 1, {{{3, 0, 8, K::Unorm}, {}, {}, {}}}, 1),
    packed(85, 2, {{{2, 0, 5, K::Unorm}, {1, 5, 6, K::Unorm},
                    {0, 11, 5, K::Unorm}, {}}}, 3),
    packed(86, 2, {{{2, 0, 5, K::Unorm}, {1, 5, 5, K::Unorm},
                    {0, 10, 5, K::Unorm}, {3, 15, 1, K::Unorm}}}, 4),
    bgra8(87, K::Unorm, true), bgra8(88, K::Unorm, false),
    bgra8(91, K::Srgb, true), bgra8(93, K::Srgb, false),
    packed(115, 2, {{{2, 0, 4, K::Unorm}, {1, 4, 4, K::Unorm},
                     {0, 8, 4, K::Unorm}, {3, 12, 4, K::Unorm}}}, 4),
};
} // namespace detail

/// The stored layout of a typed, uncompressed, non-depth DXGI format; null for
/// anything else (typeless, block-compressed, depth, video, packed-shared-exp).
[[nodiscard]] constexpr const TexelLayout* layout_for(u32 dxgi_format) {
    for (const auto& layout : detail::kLayouts) {
        if (layout.dxgi_format == dxgi_format) return &layout;
    }
    return nullptr;
}

/// A typed colour format with no integer channel: every channel is FLOAT,
/// UNORM, SNORM or sRGB. These are the formats D3D11 filters -- the ones with
/// D3D11_FORMAT_SUPPORT_MIP_AUTOGEN (Learn, GenerateMips) and
/// MULTISAMPLE_RESOLVE (D3D11.3 5.6.6: integer formats do not resolve).
[[nodiscard]] constexpr bool is_filterable_color(u32 dxgi_format) {
    const TexelLayout* layout = layout_for(dxgi_format);
    if (layout == nullptr) return false;
    for (u8 i = 0; i < layout->channel_count; ++i) {
        if (layout->channels[i].kind == ChannelKind::Uint ||
            layout->channels[i].kind == ChannelKind::Sint) {
            return false;
        }
    }
    return true;
}

/// A texel's bytes, little-endian, `texel_bytes` of them meaningful.
using TexelBytes = std::array<u8, 16>;

namespace detail {
inline void put_bits(TexelBytes& texel, u32 offset, u32 bits, u32 value) {
    for (u32 i = 0; i < bits; ++i) {
        const u32 bit = offset + i;
        const u8 mask = static_cast<u8>(1u << (bit % 8u));
        if ((value >> i) & 1u) texel[bit / 8u] |= mask;
        else texel[bit / 8u] &= static_cast<u8>(~mask);
    }
}
inline u32 low_bits(u32 value, u32 bits) {
    return bits >= 32 ? value : value & ((1u << bits) - 1u);
}
inline u32 float_bits(float value) {
    u32 bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}
inline float bits_float(u32 bits) {
    float value = 0.0f;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}
} // namespace detail

/// D3D11.3 3.2.3.6 FLOAT -> UNORM (NaN 0, clamp, ties to even as WARP).
[[nodiscard]] inline u32 float_to_unorm(float value, u32 bits) {
    if (std::isnan(value) || value <= 0.0f) return 0;
    if (value >= 1.0f) return detail::low_bits(0xFFFFFFFFu, bits);
    const float scale = static_cast<float>((1ull << bits) - 1ull);
    const float scaled = value * scale;
    return static_cast<u32>(std::nearbyint(scaled));
}

/// D3D11.3 3.2.3.4 FLOAT -> SNORM, two's complement in the low `bits`.
[[nodiscard]] inline u32 float_to_snorm(float value, u32 bits) {
    if (std::isnan(value)) return 0;
    const float clamped = value < -1.0f ? -1.0f : (value > 1.0f ? 1.0f : value);
    const float scale = static_cast<float>((1ull << (bits - 1)) - 1ull);
    const float scaled = clamped * scale;
    const i32 rounded = static_cast<i32>(std::nearbyint(scaled));
    return detail::low_bits(static_cast<u32>(rounded), bits);
}

/// D3D11.3 3.2.3.8 FLOAT -> SRGB.
[[nodiscard]] inline u32 float_to_srgb(float value, u32 bits) {
    if (std::isnan(value) || value <= 0.0f) return 0;
    if (value >= 1.0f) return detail::low_bits(0xFFFFFFFFu, bits);
    const float encoded =
        value <= 0.0031308f
            ? 12.92f * value
            : 1.055f * std::pow(value, 1.0f / 2.4f) - 0.055f;
    return float_to_unorm(encoded, bits);
}

/// FLOAT -> a float with `exponent_bits` (bias 2^(e-1)-1) and
/// `mantissa_bits`, with a sign bit when `has_sign` (3.2.2).
[[nodiscard]] inline u32 float_to_small_float(float value, u32 exponent_bits,
                                              u32 mantissa_bits,
                                              bool has_sign) {
    const u32 bits = detail::float_bits(value);
    const u32 sign = bits >> 31;
    const u32 exponent_all = (1u << exponent_bits) - 1u;
    const u32 mantissa_mask = (1u << mantissa_bits) - 1u;
    const u32 sign_bit =
        has_sign ? sign << (exponent_bits + mantissa_bits) : 0u;
    if (std::isnan(value)) {
        // WARP: float16 NaN is 0x7E00; the unsigned 11/10-bit floats store
        // all ones (R11G11B10 RTV/UAV lines of the a06 probe).
        return has_sign
            ? (exponent_all << mantissa_bits) | (1u << (mantissa_bits - 1))
            : (exponent_all << mantissa_bits) | mantissa_mask;
    }
    if (!has_sign && sign != 0) return 0;  // no sign: negatives (and -0) are 0
    if (std::isinf(value)) return sign_bit | (exponent_all << mantissa_bits);
    const i32 exponent = static_cast<i32>((bits >> 23) & 0xFFu) - 127;
    const u32 mantissa = bits & 0x7FFFFFu;
    const i32 bias = static_cast<i32>((1u << (exponent_bits - 1)) - 1u);
    const i32 max_exponent = static_cast<i32>(exponent_all) - 1 - bias;
    if (exponent > max_exponent) {
        // Round toward zero saturates at the largest finite value.
        return sign_bit | ((exponent_all - 1u) << mantissa_bits) |
               mantissa_mask;
    }
    if (((bits >> 23) & 0xFFu) == 0) return sign_bit;  // float32 denorm/zero
    if (exponent >= 1 - bias) {
        return sign_bit |
               (static_cast<u32>(exponent + bias) << mantissa_bits) |
               (mantissa >> (23u - mantissa_bits));
    }
    // A denormal of the smaller format, truncated.
    const u32 shift = (23u - mantissa_bits) +
                      static_cast<u32>((1 - bias) - exponent);
    const u32 significand = mantissa | 0x800000u;
    return sign_bit | (shift >= 32 ? 0u : significand >> shift);
}

/// The value a small float encodes, exactly (every one fits in a float32).
[[nodiscard]] inline float small_float_value(u32 encoded, u32 exponent_bits,
                                             u32 mantissa_bits,
                                             bool has_sign) {
    const u32 mantissa = encoded & ((1u << mantissa_bits) - 1u);
    const u32 exponent =
        (encoded >> mantissa_bits) & ((1u << exponent_bits) - 1u);
    const bool negative =
        has_sign && ((encoded >> (exponent_bits + mantissa_bits)) & 1u) != 0;
    const i32 bias = static_cast<i32>((1u << (exponent_bits - 1)) - 1u);
    float magnitude = 0.0f;
    if (exponent == (1u << exponent_bits) - 1u) {
        magnitude = mantissa != 0 ? std::numeric_limits<float>::quiet_NaN()
                                  : std::numeric_limits<float>::infinity();
    } else if (exponent == 0) {
        magnitude = std::ldexp(static_cast<float>(mantissa),
                               1 - bias - static_cast<i32>(mantissa_bits));
    } else {
        magnitude = std::ldexp(
            static_cast<float>(mantissa | (1u << mantissa_bits)),
            static_cast<i32>(exponent) - bias -
                static_cast<i32>(mantissa_bits));
    }
    return negative ? -magnitude : magnitude;
}

/// FLOAT -> the float stored in a `bits`-wide channel (32, 16, 11, 10).
[[nodiscard]] inline u32 float_to_channel_float(float value, u32 bits) {
    switch (bits) {
    case 32: return detail::float_bits(value);
    case 16: return float_to_small_float(value, 5, 10, true);
    case 11: return float_to_small_float(value, 5, 6, false);
    case 10: return float_to_small_float(value, 5, 5, false);
    default: return 0;
    }
}

/// ClearView's rule for integer channels: an integral float, fraction toward
/// zero, clamped to the channel's range, NaN 0.
[[nodiscard]] inline u32 float_to_integer_channel(float value, u32 bits,
                                                  bool is_signed) {
    if (std::isnan(value)) return 0;
    const double truncated = std::trunc(static_cast<double>(value));
    if (is_signed) {
        const double lo = -std::ldexp(1.0, static_cast<int>(bits) - 1);
        const double hi = std::ldexp(1.0, static_cast<int>(bits) - 1) - 1.0;
        const double clamped = truncated < lo ? lo : (truncated > hi ? hi : truncated);
        return detail::low_bits(
            static_cast<u32>(static_cast<i64>(clamped)), bits);
    }
    const double hi = std::ldexp(1.0, static_cast<int>(bits)) - 1.0;
    const double clamped = truncated < 0.0 ? 0.0 : (truncated > hi ? hi : truncated);
    return static_cast<u32>(static_cast<u64>(clamped));
}

/// ClearUnorderedAccessViewUint: the low bits of Values[component] per channel.
[[nodiscard]] inline TexelBytes encode_uint(const TexelLayout& layout,
                                            const std::array<u32, 4>& values) {
    TexelBytes texel{};
    for (u8 i = 0; i < layout.channel_count; ++i) {
        const Channel& c = layout.channels[i];
        detail::put_bits(texel, c.offset, c.bits,
                         detail::low_bits(values[c.component], c.bits));
    }
    return texel;
}

enum class FloatClear : u8 {
    /// ClearUnorderedAccessViewFloat: integer channels make the call invalid.
    UnorderedAccessView,
    /// ClearView: integer channels take the integral float.
    ClearView,
};

/// Converts one colour into the view's texel. False when the rule refuses the
/// format (ClearUnorderedAccessViewFloat on an integer view).
[[nodiscard]] inline bool encode_float(const TexelLayout& layout,
                                       const std::array<float, 4>& color,
                                       FloatClear rule, TexelBytes& texel) {
    texel = {};
    for (u8 i = 0; i < layout.channel_count; ++i) {
        const Channel& c = layout.channels[i];
        const float value = color[c.component];
        u32 stored = 0;
        switch (c.kind) {
        case ChannelKind::Unorm: stored = float_to_unorm(value, c.bits); break;
        case ChannelKind::Snorm: stored = float_to_snorm(value, c.bits); break;
        case ChannelKind::Srgb: stored = float_to_srgb(value, c.bits); break;
        case ChannelKind::Float:
            stored = float_to_channel_float(value, c.bits);
            break;
        case ChannelKind::Uint:
        case ChannelKind::Sint:
            if (rule == FloatClear::UnorderedAccessView) return false;
            stored = float_to_integer_channel(value, c.bits,
                                              c.kind == ChannelKind::Sint);
            break;
        }
        detail::put_bits(texel, c.offset, c.bits, stored);
    }
    return true;
}

/// For a render-pass clear that the backend converts itself: replace each
/// normalized or small-float component with the exact value the D3D
/// conversion stores, so any correctly rounding conversion reproduces the D3D
/// bits (3.2.2 round-toward-zero for float16/11/10, ties-to-even for *NORM
/// and sRGB). Integer and float32 components pass through. The sRGB value is
/// the linear colour that encodes to the chosen code.
[[nodiscard]] inline std::array<float, 4> quantize_render_target_color(
    const TexelLayout& layout, const std::array<float, 4>& color) {
    std::array<float, 4> result = color;
    for (u8 i = 0; i < layout.channel_count; ++i) {
        const Channel& c = layout.channels[i];
        const float value = color[c.component];
        switch (c.kind) {
        case ChannelKind::Unorm: {
            const double max = static_cast<double>((1ull << c.bits) - 1ull);
            result[c.component] = static_cast<float>(
                float_to_unorm(value, c.bits) / max);
            break;
        }
        case ChannelKind::Snorm: {
            const u32 code = float_to_snorm(value, c.bits);
            const i32 signed_code =
                (code & (1u << (c.bits - 1)))
                    ? static_cast<i32>(code) - static_cast<i32>(1u << c.bits)
                    : static_cast<i32>(code);
            const double max = static_cast<double>((1ull << (c.bits - 1)) - 1ull);
            const double normalized = signed_code / max;
            result[c.component] =
                static_cast<float>(normalized < -1.0 ? -1.0 : normalized);
            break;
        }
        case ChannelKind::Srgb: {
            const double max = static_cast<double>((1ull << c.bits) - 1ull);
            const double encoded = float_to_srgb(value, c.bits) / max;
            const double linear =
                encoded <= 0.04045 ? encoded / 12.92
                                   : std::pow((encoded + 0.055) / 1.055, 2.4);
            result[c.component] = static_cast<float>(linear);
            break;
        }
        case ChannelKind::Float:
            if (c.bits == 16) {
                result[c.component] = small_float_value(
                    float_to_small_float(value, 5, 10, true), 5, 10, true);
            } else if (c.bits == 11 || c.bits == 10) {
                const u32 mantissa = c.bits == 11 ? 6u : 5u;
                result[c.component] = small_float_value(
                    float_to_small_float(value, 5, mantissa, false), 5,
                    mantissa, false);
            }
            break;
        case ChannelKind::Uint:
        case ChannelKind::Sint:
            break;
        }
    }
    return result;
}

} // namespace vitreo::winapi::d3d11_texel
