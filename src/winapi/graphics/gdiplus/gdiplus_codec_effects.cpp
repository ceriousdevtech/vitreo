// SPDX-License-Identifier: LGPL-2.1-only
/// @file gdiplus_codec_effects.cpp
/// @brief GDI+ 1.1 effects (GdiPlusEffects.h) and bitmap histograms
/// (GdiPlusColorMatrix.h HistogramFormat).
///
/// Contract: GdiPlusEffects.h (GUIDs, parameter structs, the C++ wrappers'
/// size rules), Learn (Effect / Bitmap::ApplyEffect / Bitmap::GetHistogram
/// and the *Params pages for value ranges), and the winoracle probe
/// tools/oracle_probe/gdiplus_codec_probe.c ("effect.*", "hist*", "r2.*"
/// lines) for statuses, defaults, ROI rules, output formats and the exact
/// lookup tables GDI+ hands back as aux data.
///
/// Every effect works on non-premultiplied ARGB (probe: a ColorMatrix on a
/// 50%-alpha white keeps its colour, 8099ffff). Effects that GDI+ builds as
/// per-channel lookup tables (ColorLUT, BrightnessContrast, Levels,
/// ColorBalance, ColorCurve, HueSaturationLightness) produce the same
/// 1024-byte B,G,R,A table as aux data.

#include "winapi/graphics/gdiplus/gdiplus_codec.hpp"
#include "winapi/graphics/gdiplus/gdiplus.hpp"
#include "winapi/core/marshaler.hpp"
#include "core/memory/guest_memory.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

namespace vitreo::winapi::gdip {

namespace {

// GdiPlusEffects.h
constexpr Guid kBlurEffect = make_guid(0x633c80a4, 0x1843, 0x482b, 0x9e, 0xf2, 0xbe, 0x28, 0x34, 0xc5, 0xfd, 0xd4);
constexpr Guid kSharpenEffect = make_guid(0x63cbf3ee, 0xc526, 0x402c, 0x8f, 0x71, 0x62, 0xc5, 0x40, 0xbf, 0x51, 0x42);
constexpr Guid kColorMatrixEffect = make_guid(0x718f2615, 0x7933, 0x40e3, 0xa5, 0x11, 0x5f, 0x68, 0xfe, 0x14, 0xdd, 0x74);
constexpr Guid kColorLUTEffect = make_guid(0xa7ce72a9, 0x0f7f, 0x40d7, 0xb3, 0xcc, 0xd0, 0xc0, 0x2d, 0x5c, 0x32, 0x12);
constexpr Guid kBrightnessContrastEffect = make_guid(0xd3a1dbe1, 0x8ec4, 0x4c17, 0x9f, 0x4c, 0xea, 0x97, 0xad, 0x1c, 0x34, 0x3d);
constexpr Guid kHueSaturationLightnessEffect = make_guid(0x8b2dd6c3, 0xeb07, 0x4d87, 0xa5, 0xf0, 0x71, 0x08, 0xe2, 0x6a, 0x9c, 0x5f);
constexpr Guid kLevelsEffect = make_guid(0x99c354ec, 0x2a31, 0x4f3a, 0x8c, 0x34, 0x17, 0xa8, 0x03, 0xb3, 0x3a, 0x25);
constexpr Guid kTintEffect = make_guid(0x1077af00, 0x2848, 0x4441, 0x94, 0x89, 0x44, 0xad, 0x4c, 0x2d, 0x7a, 0x2c);
constexpr Guid kColorBalanceEffect = make_guid(0x537e597d, 0x251e, 0x48da, 0x96, 0x64, 0x29, 0xca, 0x49, 0x6b, 0x70, 0xf8);
constexpr Guid kRedEyeCorrectionEffect = make_guid(0x74d29d05, 0x69a4, 0x4266, 0x95, 0x49, 0x3c, 0xc5, 0x28, 0x36, 0xb6, 0x32);
constexpr Guid kColorCurveEffect = make_guid(0xdd6a0022, 0x58e4, 0x4a67, 0x9d, 0x9b, 0xd4, 0x8e, 0xb8, 0x81, 0xa5, 0x3d);

enum class Kind { Blur, Sharpen, ColorMatrix, ColorLUT, BrightnessContrast, HSL, Levels, Tint, ColorBalance, RedEye, ColorCurve };

/// A CGpEffect. Parameters are kept as the raw bytes the guest set (BOOL
/// and enum fields read back exactly as written, probe "effect.*.get").
struct GpEffect {
    Kind kind{Kind::Blur};
    bool is_64{false};
    bool has_params{false};       ///< ColorMatrix/ColorLUT/RedEye start without any
    std::vector<u8> params;       ///< fixed part (RedEye: count + pointer slot)
    std::vector<std::array<i32, 4>> areas;  ///< RedEye RECTs (copied at Set)
};

/// sizeof(*Params) (GdiPlusEffects.h); RedEye is {UINT; RECT*} = 2 pointers.
u32 param_size(const GpEffect& e) {
    switch (e.kind) {
        case Kind::Blur: case Kind::Sharpen: case Kind::BrightnessContrast: case Kind::Tint: return 8;
        case Kind::ColorMatrix: return 100;
        case Kind::ColorLUT: return 1024;
        case Kind::HSL: case Kind::Levels: case Kind::ColorBalance: case Kind::ColorCurve: return 12;
        case Kind::RedEye: return (e.is_64 ? 16u : 8u) + static_cast<u32>(e.areas.size()) * 16u;
    }
    return 0;
}

i32 int_at(const std::vector<u8>& p, usize off) {
    i32 v = 0;
    std::memcpy(&v, p.data() + off, 4);
    return v;
}
float float_at(const std::vector<u8>& p, usize off) {
    float v = 0;
    std::memcpy(&v, p.data() + off, 4);
    return v;
}
void put_i32(std::vector<u8>& p, usize off, i32 v) { std::memcpy(p.data() + off, &v, 4); }
void put_f32(std::vector<u8>& p, usize off, float v) { std::memcpy(p.data() + off, &v, 4); }

/// Defaults a fresh effect reports (probe "effect.*.getdefault").
void set_defaults(GpEffect& e) {
    e.params.assign(e.kind == Kind::RedEye ? (e.is_64 ? 16 : 8) : param_size(e), 0);
    switch (e.kind) {
        case Kind::Blur: put_f32(e.params, 0, 10.0f); e.has_params = true; break;          // radius 10, expandEdge FALSE
        case Kind::Sharpen: put_f32(e.params, 0, 10.0f); put_f32(e.params, 4, 50.0f); e.has_params = true; break;
        case Kind::Levels: put_i32(e.params, 0, 100); e.has_params = true; break;           // highlight 100
        case Kind::ColorCurve: put_i32(e.params, 0, 2); e.has_params = true; break;         // AdjustContrast
        case Kind::BrightnessContrast: case Kind::HSL: case Kind::Tint: case Kind::ColorBalance:
            e.has_params = true; break;
        case Kind::RedEye: e.has_params = true; break;  // zero areas (probe "effect.redeye get" after a refused Set)
        case Kind::ColorMatrix: case Kind::ColorLUT: e.has_params = false; break;
    }
}

bool in(i32 v, i32 lo, i32 hi) { return v >= lo && v <= hi; }

/// Value ranges (Learn *Params pages; probe "r2.range"). A refused Set keeps
/// the previous parameters.
bool params_valid(Kind kind, const std::vector<u8>& p) {
    switch (kind) {
        case Kind::Blur: {
            const float r = float_at(p, 0);
            return r >= 0.0f && r <= 256.0f;   // probe: 256 accepted, 300 refused
        }
        case Kind::Sharpen: {
            const float r = float_at(p, 0), a = float_at(p, 4);
            return r >= 0.0f && r <= 256.0f && a >= 0.0f && a <= 100.0f;
        }
        case Kind::BrightnessContrast: return in(int_at(p, 0), -255, 255) && in(int_at(p, 4), -100, 100);
        case Kind::HSL: return in(int_at(p, 0), -180, 180) && in(int_at(p, 4), -100, 100) && in(int_at(p, 8), -100, 100);
        case Kind::Levels: return in(int_at(p, 0), 0, 100) && in(int_at(p, 4), -100, 100) && in(int_at(p, 8), 0, 100);
        case Kind::Tint: return in(int_at(p, 0), -180, 180) && in(int_at(p, 4), -100, 100);
        case Kind::ColorBalance: return in(int_at(p, 0), -100, 100) && in(int_at(p, 4), -100, 100) && in(int_at(p, 8), -100, 100);
        case Kind::ColorCurve: {
            const i32 adj = int_at(p, 0), ch = int_at(p, 4), v = int_at(p, 8);
            if (!in(adj, 0, 7) || !in(ch, 0, 3)) return false;
            if (adj <= 1) return in(v, -255, 255);      // Exposure, Density
            if (adj <= 5) return in(v, -100, 100);      // Contrast, Highlight, Shadow, Midtone
            return in(v, 0, 255);                       // White / Black saturation
        }
        default: return true;
    }
}

u8 clamp8(double v) {
    if (!(v > 0)) return 0;
    if (v >= 255) return 255;
    return static_cast<u8>(std::floor(v + 0.5));
}

using Lut = std::array<u8, 256>;
struct Luts { Lut b, g, r, a; };

Lut identity() {
    Lut l{};
    for (int i = 0; i < 256; ++i) l[static_cast<usize>(i)] = static_cast<u8>(i);
    return l;
}

/// Brightness/contrast (probe "r2.brightness/contrast", exact): contrast
/// scales about 127.5 by k = 100/(100-c) (c > 0) or (100+c)/100; brightness
/// adds b, scaled by (1+k)/2 when both are set (probe "effect.brightness").
Lut brightness_contrast(i32 b, i32 c) {
    Lut l{};
    for (int i = 0; i < 256; ++i) {
        double v;
        if (c >= 100) {
            v = i >= 128 ? 255.0 : 0.0;
        } else {
            const double k = c > 0 ? 100.0 / (100.0 - c) : (100.0 + c) / 100.0;
            v = (i - 127.5) * k + 127.5 + b * (1.0 + k) / 2.0;
        }
        l[static_cast<usize>(i)] = clamp8(v);
    }
    return l;
}

/// Levels' / midtone's gamma (probe "r2.levels", exact): g = 1/(1+m/50)
/// for m > 0, 1 - m/50 for m < 0.
double midtone_gamma(i32 m) { return m > 0 ? 1.0 / (1.0 + m / 50.0) : 1.0 - m / 50.0; }

Lut levels(i32 highlight, i32 midtone, i32 shadow) {
    Lut l{};
    const double wp = 255.0 * highlight / 100.0, bp = 255.0 * shadow / 100.0;
    const double g = midtone_gamma(midtone);
    for (int i = 0; i < 256; ++i) {
        double x;
        if (wp <= bp) x = i >= wp ? 1.0 : 0.0;
        else x = std::clamp((i - bp) / (wp - bp), 0.0, 1.0);
        l[static_cast<usize>(i)] = clamp8(255.0 * std::pow(x, g));
    }
    return l;
}

Lut scale_lut(double factor) {
    Lut l{};
    for (int i = 0; i < 256; ++i) l[static_cast<usize>(i)] = clamp8(i * factor);
    return l;
}

/// ColorCurve tables. Exact against the probe: Density (+v), Midtone
/// (levels gamma) and BlackSaturation ((i-v)*255/(255-v)). The S-curves
/// (Exposure, Contrast, Highlight, Shadow) and WhiteSaturation follow the
/// shape GDI+ produces but are not oracle-exact (see the contract doc).
Lut color_curve(i32 adj, i32 v) {
    Lut l{};
    const double pi = 3.14159265358979323846;
    for (int i = 0; i < 256; ++i) {
        const double x = i / 255.0;
        double y = x;
        switch (adj) {
            case 0:  // Exposure: a stop-like lift of the shadows/midtones
                y = 1.0 - std::pow(1.0 - x, std::pow(2.0, v / 100.0));
                break;
            case 1:  // Density
                y = (i + v) / 255.0;
                break;
            case 2: {  // Contrast: symmetric S-curve about the midpoint
                const double p = v >= 0 ? 1.0 + 2.0 * v / 100.0 : 1.0 / (1.0 - v / 200.0);
                y = x < 0.5 ? 0.5 * std::pow(2.0 * x, p) : 1.0 - 0.5 * std::pow(2.0 * (1.0 - x), p);
                break;
            }
            case 3:  // Highlight: bends only the upper half
                if (x > 0.5) y = x + v / 100.0 * 0.28 * std::sin(pi * (x - 0.5) * 2.0);
                break;
            case 4:  // Shadow: bends only the lower half
                if (x < 0.5) y = x + v / 100.0 * 0.28 * std::sin(pi * x * 2.0);
                break;
            case 5:  // Midtone
                y = std::pow(x, midtone_gamma(v));
                break;
            case 6:  // White saturation: v becomes white
                y = v <= 0 ? (i > 0 ? 1.0 : 0.0) : i / static_cast<double>(v);
                break;
            case 7:  // Black saturation: v becomes black
                y = v >= 255 ? (i >= 255 ? 1.0 : 0.0) : (i - v) / (255.0 - v);
                break;
            default: break;
        }
        l[static_cast<usize>(i)] = clamp8(y * 255.0);
    }
    return l;
}

/// The per-channel tables for a LUT-based effect; false for the others.
bool effect_luts(const GpEffect& e, Luts& out) {
    out = {identity(), identity(), identity(), identity()};
    const auto& p = e.params;
    switch (e.kind) {
        case Kind::ColorLUT:
            std::memcpy(out.b.data(), p.data(), 256);
            std::memcpy(out.g.data(), p.data() + 256, 256);
            std::memcpy(out.r.data(), p.data() + 512, 256);
            std::memcpy(out.a.data(), p.data() + 768, 256);
            return true;
        case Kind::BrightnessContrast:
            out.b = out.g = out.r = brightness_contrast(int_at(p, 0), int_at(p, 4));
            return true;
        case Kind::Levels:
            out.b = out.g = out.r = levels(int_at(p, 0), int_at(p, 4), int_at(p, 8));
            return true;
        case Kind::ColorBalance:  // cyanRed, magentaGreen, yellowBlue (probe exact)
            out.r = scale_lut(1.0 + int_at(p, 0) / 100.0);
            out.g = scale_lut(1.0 + int_at(p, 4) / 100.0);
            out.b = scale_lut(1.0 + int_at(p, 8) / 100.0);
            return true;
        case Kind::ColorCurve: {
            const Lut l = color_curve(int_at(p, 0), int_at(p, 8));
            switch (int_at(p, 4)) {
                case 1: out.r = l; break;
                case 2: out.g = l; break;
                case 3: out.b = l; break;
                default: out.r = out.g = out.b = l; break;
            }
            return true;
        }
        case Kind::HSL: {
            // On 8-bit H,S,L: the R table is hue (shift by h*256/360, wrapping),
            // G is saturation (x(1+s/100)), B is lightness (+l*2.55). Probe exact.
            const i32 h = int_at(p, 0), s = int_at(p, 4), lv = int_at(p, 8);
            const i32 shift = static_cast<i32>(std::lround(h * 256.0 / 360.0));
            for (int i = 0; i < 256; ++i) {
                out.r[static_cast<usize>(i)] = static_cast<u8>(((i + shift) % 256 + 256) % 256);
                out.g[static_cast<usize>(i)] = clamp8(i * (1.0 + s / 100.0));
                out.b[static_cast<usize>(i)] = clamp8(i + lv * 255.0 / 100.0);
            }
            return true;
        }
        default:
            return false;
    }
}

// ---- Pixel helpers ---------------------------------------------------------------

struct Px { double a, r, g, b; };
Px unpack(u32 c) {
    return {static_cast<double>(c >> 24), static_cast<double>((c >> 16) & 0xFF), static_cast<double>((c >> 8) & 0xFF),
            static_cast<double>(c & 0xFF)};
}
u32 pack(double a, double r, double g, double b) {
    return (u32{clamp8(a)} << 24) | (u32{clamp8(r)} << 16) | (u32{clamp8(g)} << 8) | clamp8(b);
}

/// RGB <-> HSL on 0..255 channels with H in 0..256 (one turn).
void rgb_to_hsl(double r, double g, double b, double& h, double& s, double& l) {
    const double mx = std::max({r, g, b}), mn = std::min({r, g, b});
    l = (mx + mn) / 2.0;
    const double d = mx - mn;
    if (d == 0) { h = 0; s = 0; return; }
    s = l <= 127.5 ? d / (mx + mn) * 255.0 : d / (510.0 - mx - mn) * 255.0;
    double hh;
    if (mx == r) hh = (g - b) / d;
    else if (mx == g) hh = 2.0 + (b - r) / d;
    else hh = 4.0 + (r - g) / d;
    hh /= 6.0;
    if (hh < 0) hh += 1.0;
    h = hh * 256.0;
}
double hue_channel(double p, double q, double t) {
    if (t < 0) t += 1;
    if (t > 1) t -= 1;
    if (t < 1.0 / 6) return p + (q - p) * 6 * t;
    if (t < 0.5) return q;
    if (t < 2.0 / 3) return p + (q - p) * (2.0 / 3 - t) * 6;
    return p;
}
void hsl_to_rgb(double h, double s, double l, double& r, double& g, double& b) {
    const double S = s / 255.0, L = l / 255.0, H = h / 256.0;
    if (S == 0) { r = g = b = l; return; }
    const double q = L < 0.5 ? L * (1 + S) : L + S - L * S;
    const double p = 2 * L - q;
    r = hue_channel(p, q, H + 1.0 / 3) * 255.0;
    g = hue_channel(p, q, H) * 255.0;
    b = hue_channel(p, q, H - 1.0 / 3) * 255.0;
}

/// Separable Gaussian blur, sigma = radius/2. expandEdge TRUE treats the
/// outside as transparent (edges lose alpha, probe "r2.blur r=1 expand");
/// FALSE renormalises over the pixels inside (edges stay opaque).
/// Premultiplied while filtering so transparent pixels carry no colour.
void blur(std::vector<u32>& px, i32 w, i32 h, i32 x0, i32 y0, i32 x1, i32 y1, float radius, bool expand) {
    if (radius <= 0.0f) return;
    const double sigma = radius / 2.0;
    const i32 k = std::max(1, static_cast<i32>(std::ceil(radius)));
    std::vector<double> wts(static_cast<usize>(2 * k + 1));
    for (i32 i = -k; i <= k; ++i) wts[static_cast<usize>(i + k)] = std::exp(-(i * i) / (2 * sigma * sigma));
    double sum = 0;
    for (double v : wts) sum += v;
    for (double& v : wts) v /= sum;
    const i32 rw = x1 - x0, rh = y1 - y0;
    std::vector<std::array<double, 4>> a(static_cast<usize>(rw) * static_cast<usize>(rh)), t(a.size());
    for (i32 y = 0; y < rh; ++y)
        for (i32 x = 0; x < rw; ++x) {
            const Px p = unpack(px[static_cast<usize>(y + y0) * static_cast<usize>(w) + static_cast<usize>(x + x0)]);
            const double f = p.a / 255.0;
            a[static_cast<usize>(y * rw + x)] = {p.a, p.r * f, p.g * f, p.b * f};
        }
    auto pass = [&](const std::vector<std::array<double, 4>>& src, std::vector<std::array<double, 4>>& dst, bool horiz) {
        for (i32 y = 0; y < rh; ++y)
            for (i32 x = 0; x < rw; ++x) {
                std::array<double, 4> acc{};
                double wsum = 0;
                for (i32 i = -k; i <= k; ++i) {
                    const i32 sx = horiz ? x + i : x, sy = horiz ? y : y + i;
                    if (sx < 0 || sy < 0 || sx >= rw || sy >= rh) continue;
                    const double wt = wts[static_cast<usize>(i + k)];
                    const auto& s = src[static_cast<usize>(sy * rw + sx)];
                    for (int c = 0; c < 4; ++c) acc[static_cast<usize>(c)] += s[static_cast<usize>(c)] * wt;
                    wsum += wt;
                }
                if (!expand && wsum > 0)
                    for (double& v : acc) v /= wsum;
                dst[static_cast<usize>(y * rw + x)] = acc;
            }
    };
    pass(a, t, true);
    pass(t, a, false);
    for (i32 y = 0; y < rh; ++y)
        for (i32 x = 0; x < rw; ++x) {
            const auto& s = a[static_cast<usize>(y * rw + x)];
            const double f = s[0] > 0 ? 255.0 / s[0] : 0.0;
            px[static_cast<usize>(y + y0) * static_cast<usize>(w) + static_cast<usize>(x + x0)] =
                pack(s[0], s[1] * f, s[2] * f, s[3] * f);
        }
}

/// Red-eye correction inside each area: a red-dominant blob that does not
/// touch the area's edge (a pupil) has its red replaced by the mean of green
/// and blue. Probe "r2.redeye": a red pixel on the area's edge is left alone.
void red_eye(std::vector<u32>& px, i32 w, i32 h, const std::vector<std::array<i32, 4>>& areas) {
    for (const auto& ar : areas) {
        const i32 l = std::max(0, ar[0]), t = std::max(0, ar[1]), r = std::min(w, ar[2]), b = std::min(h, ar[3]);
        if (l >= r || t >= b) continue;
        const i32 aw = r - l, ah = b - t;
        std::vector<int> mark(static_cast<usize>(aw) * static_cast<usize>(ah), 0);
        auto red = [&](i32 x, i32 y) {
            const Px p = unpack(px[static_cast<usize>(y) * static_cast<usize>(w) + static_cast<usize>(x)]);
            return p.r > 1.5 * std::max(p.g, p.b) && p.r > 64;
        };
        int label = 0;
        for (i32 y = 0; y < ah; ++y)
            for (i32 x = 0; x < aw; ++x) {
                if (mark[static_cast<usize>(y * aw + x)] != 0 || !red(x + l, y + t)) continue;
                ++label;
                std::vector<std::pair<i32, i32>> stack{{x, y}}, blob;
                bool touches = false;
                mark[static_cast<usize>(y * aw + x)] = label;
                while (!stack.empty()) {
                    auto [cx, cy] = stack.back();
                    stack.pop_back();
                    blob.emplace_back(cx, cy);
                    if (cx == 0 || cy == 0 || cx == aw - 1 || cy == ah - 1) touches = true;
                    const i32 d[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
                    for (const auto& dd : d) {
                        const i32 nx = cx + dd[0], ny = cy + dd[1];
                        if (nx < 0 || ny < 0 || nx >= aw || ny >= ah) continue;
                        if (mark[static_cast<usize>(ny * aw + nx)] != 0 || !red(nx + l, ny + t)) continue;
                        mark[static_cast<usize>(ny * aw + nx)] = label;
                        stack.emplace_back(nx, ny);
                    }
                }
                if (touches) continue;
                for (auto [bx, by] : blob) {
                    u32& c = px[static_cast<usize>(by + t) * static_cast<usize>(w) + static_cast<usize>(bx + l)];
                    const Px p = unpack(c);
                    c = pack(p.a, (p.g + p.b) / 2.0, p.g, p.b);
                }
            }
    }
}

/// Apply @p e to the rectangle [x0,x1)x[y0,y1) of @p px.
void run_effect(const GpEffect& e, std::vector<u32>& px, i32 w, i32 h, i32 x0, i32 y0, i32 x1, i32 y1) {
    const auto& p = e.params;
    Luts luts;
    if (effect_luts(e, luts)) {
        for (i32 y = y0; y < y1; ++y)
            for (i32 x = x0; x < x1; ++x) {
                u32& c = px[static_cast<usize>(y) * static_cast<usize>(w) + static_cast<usize>(x)];
                const u8 a = static_cast<u8>(c >> 24), r = static_cast<u8>(c >> 16), g = static_cast<u8>(c >> 8),
                         b = static_cast<u8>(c);
                if (e.kind == Kind::HSL) {
                    double hh, ss, ll;
                    rgb_to_hsl(r, g, b, hh, ss, ll);
                    const u8 H = static_cast<u8>(static_cast<i32>(std::floor(hh + 0.5)) & 0xFF);
                    double nr, ng, nb;
                    hsl_to_rgb(luts.r[H], luts.g[clamp8(ss)], luts.b[clamp8(ll)], nr, ng, nb);
                    c = pack(a, nr, ng, nb);
                } else {
                    c = (u32{luts.a[a]} << 24) | (u32{luts.r[r]} << 16) | (u32{luts.g[g]} << 8) | luts.b[b];
                }
            }
        return;
    }
    switch (e.kind) {
        case Kind::ColorMatrix: {
            // out_c = sum_r in_r * m[r][c] + m[4][c] * 255, vector [R G B A 1] (probe exact).
            float mtx[25];
            std::memcpy(mtx, p.data(), 100);
            for (i32 y = y0; y < y1; ++y)
                for (i32 x = x0; x < x1; ++x) {
                    u32& c = px[static_cast<usize>(y) * static_cast<usize>(w) + static_cast<usize>(x)];
                    const Px s = unpack(c);
                    const double v[4] = {s.r, s.g, s.b, s.a};
                    double o[4];
                    for (int col = 0; col < 4; ++col) {
                        double acc = mtx[20 + col] * 255.0;
                        for (int row = 0; row < 4; ++row) acc += v[row] * mtx[row * 5 + col];
                        o[col] = acc;
                    }
                    c = pack(o[3], o[0], o[1], o[2]);
                }
            return;
        }
        case Kind::Blur: {
            i32 ex = 0;
            std::memcpy(&ex, p.data() + 4, 4);
            blur(px, w, h, x0, y0, x1, y1, float_at(p, 0), ex != 0);
            return;
        }
        case Kind::Sharpen: {
            // Unsharp mask: in + amount/100 * (in - blur(in)).
            std::vector<u32> soft = px;
            blur(soft, w, h, x0, y0, x1, y1, float_at(p, 0), false);
            const double amt = float_at(p, 4) / 100.0;
            for (i32 y = y0; y < y1; ++y)
                for (i32 x = x0; x < x1; ++x) {
                    const usize i = static_cast<usize>(y) * static_cast<usize>(w) + static_cast<usize>(x);
                    const Px s = unpack(px[i]), b = unpack(soft[i]);
                    px[i] = pack(s.a, s.r + amt * (s.r - b.r), s.g + amt * (s.g - b.g), s.b + amt * (s.b - b.b));
                }
            return;
        }
        case Kind::Tint: {
            // Pull each pixel toward the tint hue at its own luminance; a
            // negative amount tints toward the complementary hue.
            const i32 hue = int_at(p, 0), amount = int_at(p, 4);
            const double t = std::abs(amount) / 100.0;
            double hr, hg, hb;
            const double hh = ((amount < 0 ? hue + 180 : hue) % 360 + 360) % 360;
            hsl_to_rgb(hh * 256.0 / 360.0, 255.0, 127.5, hr, hg, hb);
            const double hl = 0.2126 * hr + 0.7152 * hg + 0.0722 * hb;
            for (i32 y = y0; y < y1; ++y)
                for (i32 x = x0; x < x1; ++x) {
                    u32& c = px[static_cast<usize>(y) * static_cast<usize>(w) + static_cast<usize>(x)];
                    const Px s = unpack(c);
                    const double l = 0.2126 * s.r + 0.7152 * s.g + 0.0722 * s.b;
                    const double f = hl > 0 ? l / hl : 0;
                    c = pack(s.a, s.r + t * (hr * f - s.r), s.g + t * (hg * f - s.g), s.b + t * (hb * f - s.b));
                }
            return;
        }
        case Kind::RedEye: {
            std::vector<std::array<i32, 4>> clipped;
            for (const auto& a : e.areas)
                clipped.push_back({std::max(a[0], x0), std::max(a[1], y0), std::min(a[2], x1), std::min(a[3], y1)});
            red_eye(px, w, h, clipped);
            return;
        }
        default:
            return;
    }
}

bool has_aux(Kind k) {
    return k == Kind::ColorLUT || k == Kind::BrightnessContrast || k == Kind::HSL || k == Kind::Levels ||
           k == Kind::ColorBalance || k == Kind::ColorCurve;
}

/// Write the aux outputs: a 1024-byte B,G,R,A table in GdipAlloc memory for
/// a LUT effect when asked for, else NULL / 0.
void write_aux(GdiplusManager& mgr, Marshaler& m, const GpEffect& e, bool use_aux, GuestAddr aux_out, GuestAddr size_out) {
    GuestAddr block = 0;
    i32 size = 0;
    Luts luts;
    if (use_aux && aux_out != 0 && has_aux(e.kind) && effect_luts(e, luts)) {
        GuestBlock b(mgr, 1024);
        if (b.addr() != 0) {
            u8 all[1024];
            std::memcpy(all, luts.b.data(), 256);
            std::memcpy(all + 256, luts.g.data(), 256);
            std::memcpy(all + 512, luts.r.data(), 256);
            std::memcpy(all + 768, luts.a.data(), 256);
            if (m.memory().write_bytes(b.addr(), all, sizeof all).is_ok()) {
                block = b.release();
                size = 1024;
            }
        }
    }
    if (aux_out != 0) (void)m.write_ptr(aux_out, block);
    if (size_out != 0) (void)m.write_u32(size_out, static_cast<u32>(size));
}

/// Read a guest RECT* ROI into [x0,x1)x[y0,y1) clipped to the bitmap.
bool read_roi(Marshaler& m, GuestAddr roi, i32 w, i32 h, i32& x0, i32& y0, i32& x1, i32& y1) {
    x0 = 0; y0 = 0; x1 = w; y1 = h;
    if (roi == 0) return true;
    i32 r[4];
    for (int i = 0; i < 4; ++i) {
        const auto v = m.read_u32(roi + static_cast<u64>(i) * 4);
        if (v.is_err()) return false;
        r[i] = static_cast<i32>(v.value());
    }
    x0 = std::clamp(r[0], 0, w); y0 = std::clamp(r[1], 0, h);
    x1 = std::clamp(r[2], 0, w); y1 = std::clamp(r[3], 0, h);
    return true;
}

}  // namespace

void register_codec_effects(GdiplusManager& mgr, HandlerMap& map) {
    // GdipCreateEffect(const GUID guid, CGpEffect** effect): the GUID is
    // passed by value — four stack dwords on x86, a pointer on x64.
    map["GdipCreateEffect"] = [&mgr](Marshaler& m) {
        Guid g{};
        GuestAddr out;
        if (m.cpu_is_64bit()) {
            if (!read_guid(m, m.arg_ptr(0), g)) { write_status(m, Status::InvalidParameter); return; }
            out = m.arg_ptr(1);
        } else {
            for (usize i = 0; i < 4; ++i) {
                const u32 d = m.arg32(i);
                std::memcpy(g.data() + i * 4, &d, 4);
            }
            out = m.arg_ptr(4);
        }
        if (out == 0) { write_status(m, Status::InvalidParameter); return; }
        static const std::pair<Guid, Kind> kinds[] = {
            {kBlurEffect, Kind::Blur}, {kSharpenEffect, Kind::Sharpen}, {kColorMatrixEffect, Kind::ColorMatrix},
            {kColorLUTEffect, Kind::ColorLUT}, {kBrightnessContrastEffect, Kind::BrightnessContrast},
            {kHueSaturationLightnessEffect, Kind::HSL}, {kLevelsEffect, Kind::Levels}, {kTintEffect, Kind::Tint},
            {kColorBalanceEffect, Kind::ColorBalance}, {kRedEyeCorrectionEffect, Kind::RedEye},
            {kColorCurveEffect, Kind::ColorCurve}};
        for (const auto& [guid, kind] : kinds) {
            if (guid != g) continue;
            GpEffect e;
            e.kind = kind;
            e.is_64 = m.cpu_is_64bit();
            set_defaults(e);
            write_handle(m, out, mgr.objects().add(std::move(e)));
            write_status(m, Status::Ok);
            return;
        }
        // Probe "effect.bogus": Win32Error, *effect = NULL.
        write_handle(m, out, 0);
        write_status(m, Status::Win32Error);
    };

    map["GdipDeleteEffect"] = [&mgr](Marshaler& m) {
        write_status(m, mgr.objects().remove<GpEffect>(handle_arg(m, 0)) ? Status::Ok : Status::InvalidParameter);
    };

    map["GdipGetEffectParameterSize"] = [&mgr](Marshaler& m) {
        GpEffect* e = mgr.objects().get<GpEffect>(handle_arg(m, 0));
        const GuestAddr out = m.arg_ptr(1);
        if (e == nullptr || out == 0) { write_status(m, Status::InvalidParameter); return; }
        (void)m.write_u32(out, param_size(*e));
        write_status(m, Status::Ok);
    };

    // GdipGetEffectParameters(effect, UINT* size, VOID* params): *size must
    // hold the parameter size (probe "getsmall": InvalidParameter, *size kept).
    map["GdipGetEffectParameters"] = [&mgr](Marshaler& m) {
        GpEffect* e = mgr.objects().get<GpEffect>(handle_arg(m, 0));
        const GuestAddr size_at = m.arg_ptr(1), out = m.arg_ptr(2);
        if (e == nullptr || size_at == 0 || out == 0) { write_status(m, Status::InvalidParameter); return; }
        const auto have = m.read_u32(size_at);
        const u32 need = param_size(*e);
        if (have.is_err() || have.value() < need) { write_status(m, Status::InvalidParameter); return; }
        if (!e->has_params) {
            // GDI+ has no block to copy before the first Set (the real DLL
            // faults reading it); refuse instead.
            write_status(m, Status::InvalidParameter);
            return;
        }
        std::vector<u8> bytes = e->params;
        if (e->kind == Kind::RedEye) {
            // {UINT count; RECT* areas}: the areas follow the header in the
            // caller's buffer and the pointer refers to them (probe "r2.redeye get").
            const usize ptr = e->is_64 ? 8 : 4;
            bytes.assign(2 * ptr, 0);
            const u32 n = static_cast<u32>(e->areas.size());
            std::memcpy(bytes.data(), &n, 4);
            const u64 tail = n != 0 ? out + 2 * ptr : 0;
            std::memcpy(bytes.data() + ptr, &tail, ptr);
            for (const auto& a : e->areas) {
                const usize at = bytes.size();
                bytes.resize(at + 16);
                std::memcpy(bytes.data() + at, a.data(), 16);
            }
        }
        if (m.memory().write_bytes(out, bytes.data(), bytes.size()).is_err()) {
            write_status(m, Status::InvalidParameter);
            return;
        }
        (void)m.write_u32(size_at, need);
        write_status(m, Status::Ok);
    };

    // GdipSetEffectParameters(effect, const VOID* params, UINT size): size
    // must equal sizeof(*Params) exactly (probe "set+1"/"set-1"); RedEye's is
    // sizeof(RedEyeCorrectionParams) + n * sizeof(RECT) (the C++ wrapper).
    map["GdipSetEffectParameters"] = [&mgr](Marshaler& m) {
        GpEffect* e = mgr.objects().get<GpEffect>(handle_arg(m, 0));
        const GuestAddr in = m.arg_ptr(1);
        const u32 size = m.arg32(2);
        if (e == nullptr || in == 0) { write_status(m, Status::InvalidParameter); return; }
        if (e->kind == Kind::RedEye) {
            const usize ptr = e->is_64 ? 8 : 4;
            const auto n = m.read_u32(in);
            const auto areas = m.read_ptr(in + ptr);
            if (n.is_err() || areas.is_err() || n.value() == 0 || n.value() > (1u << 20) ||
                size != 2 * ptr + static_cast<u64>(n.value()) * 16 || areas.value() == 0) {
                write_status(m, Status::InvalidParameter);
                return;
            }
            std::vector<std::array<i32, 4>> rects(n.value());
            if (m.memory().read_bytes(areas.value(), reinterpret_cast<u8*>(rects.data()), rects.size() * 16).is_err()) {
                write_status(m, Status::InvalidParameter);
                return;
            }
            e->areas = std::move(rects);
            e->params.assign(2 * ptr, 0);
            e->has_params = true;
            write_status(m, Status::Ok);
            return;
        }
        if (size != param_size(*e)) { write_status(m, Status::InvalidParameter); return; }
        std::vector<u8> bytes(size);
        if (m.memory().read_bytes(in, bytes.data(), size).is_err() || !params_valid(e->kind, bytes)) {
            write_status(m, Status::InvalidParameter);
            return;
        }
        e->params = std::move(bytes);
        e->has_params = true;
        write_status(m, Status::Ok);
    };

    // GdipBitmapApplyEffect(bitmap, effect, RECT* roi, BOOL useAuxData,
    // VOID** auxData, INT* auxDataSize). ROI right/bottom are exclusive and
    // clipped to the bitmap; an empty or outside ROI is a no-op success.
    map["GdipBitmapApplyEffect"] = [&mgr](Marshaler& m) {
        GpImage* img = mgr.objects().get<GpImage>(handle_arg(m, 0));
        GpEffect* e = mgr.objects().get<GpEffect>(handle_arg(m, 1));
        const GuestAddr roi = m.arg_ptr(2);
        const bool use_aux = m.arg32(3) != 0;
        const GuestAddr aux_out = m.arg_ptr(4), size_out = m.arg_ptr(5);
        if (img == nullptr || e == nullptr || img->type != ImageTypeBitmap) {
            write_status(m, Status::InvalidParameter);
            return;
        }
        if (!e->has_params || (e->kind == Kind::RedEye && e->areas.empty())) {
            write_status(m, Status::InvalidParameter);   // probe "effect.redeye apply"
            return;
        }
        if (img->locked != 0) { write_status(m, Status::WrongState); return; }
        i32 x0, y0, x1, y1;
        if (!read_roi(m, roi, img->width, img->height, x0, y0, x1, y1)) {
            write_status(m, Status::InvalidParameter);
            return;
        }
        if (x0 < x1 && y0 < y1) {
            std::vector<u32> px;
            if (!read_argb(m, *img, px)) { write_status(m, Status::InvalidParameter); return; }
            run_effect(*e, px, img->width, img->height, x0, y0, x1, y1);
            if (!write_argb(m, *img, px)) { write_status(m, Status::InvalidParameter); return; }
        }
        write_aux(mgr, m, *e, use_aux, aux_out, size_out);
        write_status(m, Status::Ok);
    };

    // GdipBitmapCreateApplyEffect(GpBitmap** inputs, INT numInputs, effect,
    // RECT* roi, RECT* outputRect, GpBitmap** output, BOOL useAuxData,
    // VOID** auxData, INT* auxDataSize). One input (Learn). The output is the
    // ROI's pixels after the effect, 32bppARGB for Blur/Sharpen/ColorMatrix
    // and the input's format otherwise (probe "createapply").
    map["GdipBitmapCreateApplyEffect"] = [&mgr](Marshaler& m) {
        const GuestAddr inputs = m.arg_ptr(0);
        const i32 count = static_cast<i32>(m.arg32(1));
        GpEffect* e = mgr.objects().get<GpEffect>(handle_arg(m, 2));
        const GuestAddr roi = m.arg_ptr(3), out_rect = m.arg_ptr(4), out = m.arg_ptr(5);
        const bool use_aux = m.arg32(6) != 0;
        const GuestAddr aux_out = m.arg_ptr(7), size_out = m.arg_ptr(8);
        if (inputs == 0 || count != 1 || e == nullptr || out == 0) { write_status(m, Status::InvalidParameter); return; }
        const auto first = m.read_ptr(inputs);
        GpImage* img = first.is_ok() ? mgr.objects().get<GpImage>(first.value()) : nullptr;
        if (img == nullptr || img->type != ImageTypeBitmap) { write_status(m, Status::InvalidParameter); return; }
        if (!e->has_params || (e->kind == Kind::RedEye && e->areas.empty())) {
            write_status(m, Status::InvalidParameter);
            return;
        }
        i32 x0, y0, x1, y1;
        if (!read_roi(m, roi, img->width, img->height, x0, y0, x1, y1) || x0 >= x1 || y0 >= y1) {
            write_status(m, Status::InvalidParameter);
            return;
        }
        std::vector<u32> px;
        if (!read_argb(m, *img, px)) { write_status(m, Status::InvalidParameter); return; }
        run_effect(*e, px, img->width, img->height, x0, y0, x1, y1);
        const i32 ow = x1 - x0, oh = y1 - y0;
        std::vector<u32> crop(static_cast<usize>(ow) * static_cast<usize>(oh));
        for (i32 y = 0; y < oh; ++y)
            for (i32 x = 0; x < ow; ++x)
                crop[static_cast<usize>(y * ow + x)] =
                    px[static_cast<usize>(y + y0) * static_cast<usize>(img->width) + static_cast<usize>(x + x0)];
        const bool argb_out = e->kind == Kind::Blur || e->kind == Kind::Sharpen || e->kind == Kind::ColorMatrix;
        const i32 fmt = argb_out ? PixelFormat32bppARGB : img->pixel_format;
        GpImage result = make_bitmap(ow, oh, fmt);
        if (pixel_format_indexed(fmt)) result.palette = img->palette;
        result.dpi_x = img->dpi_x;
        result.dpi_y = img->dpi_y;
        if (!write_argb(m, result, crop)) { write_status(m, Status::InvalidParameter); return; }
        if (out_rect != 0) {
            const i32 r[4] = {x0, y0, x1, y1};
            (void)m.memory().write_bytes(out_rect, reinterpret_cast<const u8*>(r), sizeof r);
        }
        write_handle(m, out, mgr.objects().add(std::move(result)));
        write_aux(mgr, m, *e, use_aux, aux_out, size_out);
        write_status(m, Status::Ok);
    };

    // GdipBitmapGetHistogram(bitmap, format, NumberOfEntries, ch0..ch3):
    // 256 entries; ARGB/PARGB fill A,R,G,B into ch0..3 (all four required),
    // RGB fills R,G,B (ch3 must be NULL), the single-channel formats fill
    // ch0 only (the others must be NULL). Gray = round(Rec.709 luma). Probe
    // "hist" / "r2.hist".
    map["GdipBitmapGetHistogram"] = [&mgr](Marshaler& m) {
        GpImage* img = mgr.objects().get<GpImage>(handle_arg(m, 0));
        const i32 format = static_cast<i32>(m.arg32(1));
        const u32 n = m.arg32(2);
        const GuestAddr ch[4] = {m.arg_ptr(3), m.arg_ptr(4), m.arg_ptr(5), m.arg_ptr(6)};
        if (img == nullptr || img->type != ImageTypeBitmap || n != 256 || format < 0 || format > 7) {
            write_status(m, Status::InvalidParameter);
            return;
        }
        const int needed = format <= 1 ? 4 : format == 2 ? 3 : 1;
        for (int i = 0; i < 4; ++i) {
            if ((i < needed) != (ch[i] != 0)) { write_status(m, Status::InvalidParameter); return; }
        }
        std::vector<u32> px;
        if (!read_argb(m, *img, px)) { write_status(m, Status::InvalidParameter); return; }
        std::vector<u32> counts(4 * 256, 0);
        for (u32 c : px) {
            u32 a = c >> 24, r = (c >> 16) & 0xFF, g = (c >> 8) & 0xFF, b = c & 0xFF;
            if (format == 1) {
                r = (r * a + 127) / 255;
                g = (g * a + 127) / 255;
                b = (b * a + 127) / 255;
            }
            switch (format) {
                case 0: case 1:
                    ++counts[a]; ++counts[256 + r]; ++counts[512 + g]; ++counts[768 + b];
                    break;
                case 2: ++counts[r]; ++counts[256 + g]; ++counts[512 + b]; break;
                case 3: ++counts[clamp8(0.2126 * r + 0.7152 * g + 0.0722 * b)]; break;
                case 4: ++counts[b]; break;
                case 5: ++counts[g]; break;
                case 6: ++counts[r]; break;
                case 7: ++counts[a]; break;
                default: break;
            }
        }
        for (int i = 0; i < needed; ++i) {
            if (m.memory().write_bytes(ch[i], reinterpret_cast<const u8*>(counts.data() + i * 256), 256 * 4).is_err()) {
                write_status(m, Status::InvalidParameter);
                return;
            }
        }
        write_status(m, Status::Ok);
    };

    // GdipBitmapGetHistogramSize(format, UINT* n): 256 for any format value
    // (probe "hist.size f=-1..8").
    map["GdipBitmapGetHistogramSize"] = [](Marshaler& m) {
        (void)m.arg32(0);
        const GuestAddr out = m.arg_ptr(1);
        if (out == 0) { write_status(m, Status::InvalidParameter); return; }
        (void)m.write_u32(out, 256);
        write_status(m, Status::Ok);
    };
}

}  // namespace vitreo::winapi::gdip
