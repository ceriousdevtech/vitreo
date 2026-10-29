// SPDX-License-Identifier: LGPL-2.1-only
/// @file gdiplus_image_attributes.cpp
/// @brief GDI+ ImageAttributes: per-category colour adjustments (matrix,
/// gray matrix, threshold, gamma, colour keys, remap table, output
/// channel), wrap mode, and the adjustment pipeline DrawImage applies.
///
/// Contract: GdiPlusFlat.h, GdiPlusColorMatrix.h, the Learn ImageAttributes
/// pages, and tools/oracle_probe/gdiplus_image_probe.c ("attributes",
/// "extra") for the order of the stages, rounding and validation:
///  * a category set by any call replaces Default; Reset clears it;
///  * remap (exact ARGB) -> colour key (RGB inclusive, alpha ignored; a
///    keyed pixel gets alpha 0) -> matrix (gray rule) -> gamma -> threshold
///    -> output channel;
///  * the matrix works on 0..255 values, rounded to nearest; its fifth
///    column is ignored; AltGray applies the gray matrix with alpha 255 in;
///  * the threshold is round(t * 255) wrapped to a byte; a channel >= it
///    becomes 255, else 0;
///  * gamma <= 0, an unknown ColorMatrixFlags, a missing matrix, a colour
///    key whose low channel exceeds its high one, ColorAdjustTypeCount/Any
///    and an output channel above K are InvalidParameter.

#include "winapi/graphics/gdiplus/gdiplus_image_internal.hpp"
#include "winapi/graphics/gdiplus/gdiplus.hpp"
#include "winapi/core/marshaler.hpp"
#include "capsule/vfs/vfs.hpp"

#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iterator>

namespace vitreo::winapi::gdip {

namespace {

void finish(Marshaler& m, Status s) { (void)write_status(m, s); }

bool valid_type(i32 type) { return type >= ColorAdjustTypeDefault && type <= ColorAdjustTypeText; }

inline u32 clamp_round(double v) {
    const double r = std::floor(v + 0.5);
    return static_cast<u32>(std::clamp(r, 0.0, 255.0));
}

bool read_matrix(Marshaler& m, GuestAddr at, ColorMatrix5& out) {
    for (int i = 0; i < 5; ++i)
        for (int j = 0; j < 5; ++j)
            if (!read_f32(m, at + static_cast<u64>((i * 5 + j) * 4), out.m[i][j])) return false;
    return true;
}

ColorMatrix5 identity_matrix() {
    ColorMatrix5 c;
    for (int i = 0; i < 5; ++i) c.m[i][i] = 1.0f;
    return c;
}

u32 apply_matrix(const ColorMatrix5& cm, u32 px, bool opaque_in) {
    const double in[4] = {static_cast<double>((px >> 16) & 0xFF), static_cast<double>((px >> 8) & 0xFF),
                          static_cast<double>(px & 0xFF), opaque_in ? 255.0 : static_cast<double>(px >> 24)};
    u32 out[4];
    for (int j = 0; j < 4; ++j) {
        double v = 255.0 * static_cast<double>(cm.m[4][j]);
        for (int i = 0; i < 4; ++i) v += in[i] * static_cast<double>(cm.m[i][j]);
        out[j] = clamp_round(v);
    }
    return (out[3] << 24) | (out[0] << 16) | (out[1] << 8) | out[2];
}

/// GdipSetImageAttributesOutputChannel: each pixel becomes the gray
/// 255 - ink of one CMYK separation (alpha kept, probe "chan0".."chan3").
/// GDI+ separates through the system's CMYK profile; this separates through
/// the given output profile or ColorSync's generic CMYK, so the ink values
/// are a colour-managed separation but not Windows' byte-for-byte.
void separate_channel(u32* argb, usize count, i32 channel, const std::vector<u8>& profile) {
    if (count == 0) return;
    CGColorSpaceRef cmyk = nullptr;
    if (!profile.empty()) {
        CFDataRef data = CFDataCreate(nullptr, profile.data(), static_cast<CFIndex>(profile.size()));
        if (data != nullptr) {
            cmyk = CGColorSpaceCreateWithICCData(data);
            CFRelease(data);
        }
        if (cmyk != nullptr && CGColorSpaceGetModel(cmyk) != kCGColorSpaceModelCMYK) {
            CGColorSpaceRelease(cmyk);
            cmyk = nullptr;
        }
    }
    if (cmyk == nullptr) cmyk = CGColorSpaceCreateWithName(kCGColorSpaceGenericCMYK);
    CGColorSpaceRef rgb = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    if (cmyk == nullptr || rgb == nullptr) {
        if (cmyk != nullptr) CGColorSpaceRelease(cmyk);
        if (rgb != nullptr) CGColorSpaceRelease(rgb);
        log_unimplemented("GdipSetImageAttributesOutputChannel", "no CMYK colour space available");
        return;
    }
    const usize w = std::min<usize>(count, 4096);
    const usize h = (count + w - 1) / w;
    std::vector<u8> src(w * h * 4, 0xFF), dst(w * h * 4, 0);
    for (usize i = 0; i < count; ++i) {
        src[i * 4] = static_cast<u8>(argb[i] >> 16);
        src[i * 4 + 1] = static_cast<u8>(argb[i] >> 8);
        src[i * 4 + 2] = static_cast<u8>(argb[i]);
    }
    CGDataProviderRef dp = CGDataProviderCreateWithData(nullptr, src.data(), src.size(), nullptr);
    CGImageRef img = CGImageCreate(w, h, 8, 32, w * 4, rgb, static_cast<CGBitmapInfo>(kCGImageAlphaNoneSkipLast), dp,
                                   nullptr, false, kCGRenderingIntentDefault);
    CGContextRef ctx = CGBitmapContextCreate(dst.data(), w, h, 8, w * 4, cmyk, static_cast<u32>(kCGImageAlphaNone));
    if (img != nullptr && ctx != nullptr) {
        CGContextDrawImage(ctx, CGRectMake(0, 0, static_cast<CGFloat>(w), static_cast<CGFloat>(h)), img);
        CGContextFlush(ctx);
        // Bitmap contexts store rows top-down in memory; CGContextDrawImage
        // maps the image's first row to the top, so row order matches.
        for (usize i = 0; i < count; ++i) {
            const u32 gray = 255u - dst[i * 4 + static_cast<usize>(std::clamp(channel, 0, 3))];
            argb[i] = (argb[i] & 0xFF000000u) | (gray << 16) | (gray << 8) | gray;
        }
    }
    if (ctx != nullptr) CGContextRelease(ctx);
    if (img != nullptr) CGImageRelease(img);
    CGDataProviderRelease(dp);
    CGColorSpaceRelease(rgb);
    CGColorSpaceRelease(cmyk);
}

/// The ICC bytes of a colour profile named as GDI+ resolves it: a bare
/// name in the system colour directory, otherwise a path.
bool load_profile(GdiplusManager& mgr, const std::u16string& name, std::vector<u8>& out) {
    capsule::Vfs* vfs = mgr.vfs();
    if (vfs == nullptr || name.empty()) return false;
    std::string utf8;
    for (char16_t c : name) {
        if (c < 0x80) utf8.push_back(static_cast<char>(c));
        else if (c < 0x800) {
            utf8.push_back(static_cast<char>(0xC0 | (c >> 6)));
            utf8.push_back(static_cast<char>(0x80 | (c & 0x3F)));
        } else {
            utf8.push_back(static_cast<char>(0xE0 | (c >> 12)));
            utf8.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
            utf8.push_back(static_cast<char>(0x80 | (c & 0x3F)));
        }
    }
    const bool bare = utf8.find('\\') == std::string::npos && utf8.find('/') == std::string::npos &&
                      utf8.find(':') == std::string::npos;
    const std::string path = bare ? "C:\\Windows\\System32\\spool\\drivers\\color\\" + utf8 : utf8;
    auto host = vfs->resolve_host_path(path);
    if (host.is_err()) return false;
    std::ifstream f(host.value(), std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return !out.empty();
}

}  // namespace

const ColorAdjust& effective_adjust(const GpImageAttributes& ia, i32 type) {
    if (type > ColorAdjustTypeDefault && type < ColorAdjustTypeCount && ia.adjust[static_cast<usize>(type)].set)
        return ia.adjust[static_cast<usize>(type)];
    return ia.adjust[ColorAdjustTypeDefault];
}

void apply_color_adjust(const ColorAdjust& a, u32* argb, usize count) {
    if (a.noop) return;
    const bool any = a.remap_enabled || a.key_enabled || a.matrix_enabled || a.gamma_enabled ||
                     a.threshold_enabled || a.channel_enabled;
    if (!any) return;
    const u32 threshold = static_cast<u32>(static_cast<u8>(static_cast<i32>(std::floor(a.threshold * 255.0f + 0.5f))));
    u32 gamma_lut[256];
    if (a.gamma_enabled) {
        for (u32 v = 0; v < 256; ++v)
            gamma_lut[v] = clamp_round(255.0 * std::pow(v / 255.0, static_cast<double>(a.gamma)));
    }
    for (usize i = 0; i < count; ++i) {
        u32 px = argb[i];
        if (a.remap_enabled) {
            for (const auto& [from, to] : a.remap) {
                if (px == from) { px = to; break; }
            }
        }
        if (a.key_enabled) {
            bool keyed = true;
            for (int s = 0; s <= 16; s += 8) {
                const u32 c = (px >> s) & 0xFF;
                if (c < ((a.key_low >> s) & 0xFF) || c > ((a.key_high >> s) & 0xFF)) keyed = false;
            }
            if (keyed) px &= 0x00FFFFFFu;
        }
        if (a.matrix_enabled) {
            const u32 r = (px >> 16) & 0xFF, g = (px >> 8) & 0xFF, b = px & 0xFF;
            const bool gray = r == g && g == b;
            if (gray && a.matrix_flags == 1) {
                // ColorMatrixFlagsSkipGrays: grays untouched.
            } else if (gray && a.matrix_flags == 2) {
                px = apply_matrix(a.gray_matrix, px, true);
            } else {
                px = apply_matrix(a.matrix, px, false);
            }
        }
        if (a.gamma_enabled) {
            px = (px & 0xFF000000u) | (gamma_lut[(px >> 16) & 0xFF] << 16) | (gamma_lut[(px >> 8) & 0xFF] << 8) |
                 gamma_lut[px & 0xFF];
        }
        if (a.threshold_enabled) {
            u32 out = px & 0xFF000000u;
            for (int s = 0; s <= 16; s += 8)
                if (((px >> s) & 0xFF) >= threshold) out |= 0xFFu << s;
            px = out;
        }
        argb[i] = px;
    }
    if (a.channel_enabled) separate_channel(argb, count, a.channel, a.profile_enabled ? a.profile : std::vector<u8>{});
}

void register_image_attributes(GdiplusManager& mgr, HandlerMap& map) {
    auto attrs = [&mgr](Marshaler& m, usize i) { return mgr.objects().get<GpImageAttributes>(handle_arg(m, i)); };

    map["GdipCreateImageAttributes"] = [&mgr](Marshaler& m) {
        const GuestAddr out = m.arg_ptr(0);
        if (out == 0) return finish(m, Status::InvalidParameter);
        const u64 h = mgr.objects().add(GpImageAttributes{});
        if (!write_handle(m, out, h)) {
            (void)mgr.objects().remove<GpImageAttributes>(h);
            return finish(m, Status::InvalidParameter);
        }
        finish(m, Status::Ok);
    };
    map["GdipCloneImageAttributes"] = [&mgr, attrs](Marshaler& m) {
        const GpImageAttributes* ia = attrs(m, 0);
        if (ia == nullptr) return finish(m, missing_object(mgr, handle_arg(m, 0)));
        const GuestAddr out = m.arg_ptr(1);
        if (ia == nullptr || out == 0) return finish(m, Status::InvalidParameter);
        const u64 h = mgr.objects().add(GpImageAttributes{*ia});
        if (!write_handle(m, out, h)) {
            (void)mgr.objects().remove<GpImageAttributes>(h);
            return finish(m, Status::InvalidParameter);
        }
        finish(m, Status::Ok);
    };
    map["GdipDisposeImageAttributes"] = [&mgr](Marshaler& m) {
        const u64 h = handle_arg(m, 0);
        finish(m, mgr.objects().remove<GpImageAttributes>(h) ? Status::Ok : missing_object(mgr, h));
    };

    // Each category setter: (GpImageAttributes*, ColorAdjustType, ...).
    // @p body runs on the category and answers the status; a successful
    // call marks the category as set.
    using Body = Status (*)(GdiplusManager&, Marshaler&, ColorAdjust&);
    auto setter = [&mgr, attrs](Body body) {
        return [&mgr, attrs, body](Marshaler& m) {
            GpImageAttributes* ia = attrs(m, 0);
            if (ia == nullptr) return finish(m, missing_object(mgr, handle_arg(m, 0)));
            const i32 type = static_cast<i32>(m.arg32(1));
            if (!valid_type(type)) return finish(m, Status::InvalidParameter);
            ColorAdjust& c = ia->adjust[static_cast<usize>(type)];
            ColorAdjust trial = c;
            const Status s = body(mgr, m, trial);
            if (s == Status::Ok) {
                c = std::move(trial);
                c.set = true;
            }
            finish(m, s);
        };
    };

    map["GdipResetImageAttributes"] = [&mgr, attrs](Marshaler& m) {
        GpImageAttributes* ia = attrs(m, 0);
        if (ia == nullptr) return finish(m, missing_object(mgr, handle_arg(m, 0)));
        const i32 type = static_cast<i32>(m.arg32(1));
        if (ia == nullptr || !valid_type(type)) return finish(m, Status::InvalidParameter);
        ia->adjust[static_cast<usize>(type)] = ColorAdjust{};
        finish(m, Status::Ok);
    };
    map["GdipSetImageAttributesToIdentity"] = setter([](GdiplusManager&, Marshaler&, ColorAdjust& c) {
        c = ColorAdjust{};
        c.matrix = identity_matrix();
        return Status::Ok;
    });
    // (ia, type, BOOL enable, const ColorMatrix*, const ColorMatrix* gray, ColorMatrixFlags)
    map["GdipSetImageAttributesColorMatrix"] = setter([](GdiplusManager&, Marshaler& m, ColorAdjust& c) {
        const bool enable = m.arg32(2) != 0;
        if (!enable) {
            c.matrix_enabled = false;
            return Status::Ok;
        }
        const GuestAddr cm = m.arg_ptr(3), gray = m.arg_ptr(4);
        const i32 flags = static_cast<i32>(m.arg32(5));
        if (cm == 0 || flags < 0 || flags > 2 || (flags == 2 && gray == 0)) return Status::InvalidParameter;
        ColorMatrix5 mat, gm = identity_matrix();
        if (!read_matrix(m, cm, mat)) return Status::InvalidParameter;
        if (flags == 2 && !read_matrix(m, gray, gm)) return Status::InvalidParameter;
        c.matrix_enabled = true;
        c.matrix = mat;
        c.gray_matrix = gm;
        c.matrix_flags = flags;
        return Status::Ok;
    });
    // (ia, type, BOOL enable, REAL threshold): any value (probe "threshold-0.1(0)").
    map["GdipSetImageAttributesThreshold"] = setter([](GdiplusManager&, Marshaler& m, ColorAdjust& c) {
        c.threshold_enabled = m.arg32(2) != 0;
        if (c.threshold_enabled) c.threshold = real_arg(m, 3);
        return Status::Ok;
    });
    // (ia, type, BOOL enable, REAL gamma): gamma > 0 (probe "gamma0(2)").
    map["GdipSetImageAttributesGamma"] = setter([](GdiplusManager&, Marshaler& m, ColorAdjust& c) {
        const bool enable = m.arg32(2) != 0;
        const float g = real_arg(m, 3);
        if (enable && !(g > 0.0f)) return Status::InvalidParameter;
        c.gamma_enabled = enable;
        if (enable) c.gamma = g;
        return Status::Ok;
    });
    map["GdipSetImageAttributesNoOp"] = setter([](GdiplusManager&, Marshaler& m, ColorAdjust& c) {
        c.noop = m.arg32(2) != 0;
        return Status::Ok;
    });
    // (ia, type, BOOL enable, ARGB low, ARGB high)
    map["GdipSetImageAttributesColorKeys"] = setter([](GdiplusManager&, Marshaler& m, ColorAdjust& c) {
        const bool enable = m.arg32(2) != 0;
        const u32 lo = m.arg32(3), hi = m.arg32(4);
        if (enable) {
            for (int s = 0; s <= 16; s += 8)
                if (((lo >> s) & 0xFF) > ((hi >> s) & 0xFF)) return Status::InvalidParameter;  // probe "ck_r=2", "ck_a=0"
            c.key_low = lo;
            c.key_high = hi;
        }
        c.key_enabled = enable;
        return Status::Ok;
    });
    // (ia, type, BOOL enable, ColorChannelFlags C=0 .. K=3)
    map["GdipSetImageAttributesOutputChannel"] = setter([](GdiplusManager&, Marshaler& m, ColorAdjust& c) {
        const bool enable = m.arg32(2) != 0;
        const i32 ch = static_cast<i32>(m.arg32(3));
        if (enable && (ch < 0 || ch > 3)) return Status::InvalidParameter;
        c.channel_enabled = enable;
        if (enable) c.channel = ch;
        return Status::Ok;
    });
    // (ia, type, BOOL enable, const WCHAR* profile): NULL is Win32Error, a
    // profile that cannot be opened OutOfMemory (probe "profile_null=7
    // profile_missing=3").
    map["GdipSetImageAttributesOutputChannelColorProfile"] =
        setter([](GdiplusManager& mgr, Marshaler& m, ColorAdjust& c) {
            const bool enable = m.arg32(2) != 0;
            if (!enable) {
                c.profile_enabled = false;
                c.profile.clear();
                return Status::Ok;
            }
            const GuestAddr name = m.arg_ptr(3);
            if (name == 0) return Status::Win32Error;
            std::vector<u8> bytes;
            if (!load_profile(mgr, read_wstr(m, name), bytes)) return Status::OutOfMemory;
            c.profile_enabled = true;
            c.profile = std::move(bytes);
            return Status::Ok;
        });
    // (ia, type, BOOL enable, UINT mapSize, const ColorMap*)
    map["GdipSetImageAttributesRemapTable"] = setter([](GdiplusManager&, Marshaler& m, ColorAdjust& c) {
        const bool enable = m.arg32(2) != 0;
        if (!enable) {
            c.remap_enabled = false;
            c.remap.clear();
            return Status::Ok;
        }
        const u32 n = m.arg32(3);
        const GuestAddr map_at = m.arg_ptr(4);
        if (n == 0 || map_at == 0) return Status::InvalidParameter;
        std::vector<std::pair<u32, u32>> pairs(n);
        for (u32 i = 0; i < n; ++i) {
            const auto from = m.read_u32(map_at + 8ull * i), to = m.read_u32(map_at + 8ull * i + 4);
            if (from.is_err() || to.is_err()) return Status::InvalidParameter;
            pairs[i] = {from.value(), to.value()};
        }
        c.remap_enabled = true;
        c.remap = std::move(pairs);
        return Status::Ok;
    });
    // GdipSetImageAttributesWrapMode(ia, WrapMode, ARGB, BOOL clamp): not
    // validated (probe "wrap_bad=0").
    map["GdipSetImageAttributesWrapMode"] = [&mgr, attrs](Marshaler& m) {
        GpImageAttributes* ia = attrs(m, 0);
        if (ia == nullptr) return finish(m, missing_object(mgr, handle_arg(m, 0)));
        if (ia == nullptr) return finish(m, Status::InvalidParameter);
        ia->wrap_mode = static_cast<i32>(m.arg32(1));
        ia->wrap_color = m.arg32(2);
        ia->wrap_clamp = m.arg32(3) != 0;
        finish(m, Status::Ok);
    };
    map["GdipSetImageAttributesCachedBackground"] = [&mgr, attrs](Marshaler& m) {
        GpImageAttributes* ia = attrs(m, 0);
        if (ia == nullptr) return finish(m, missing_object(mgr, handle_arg(m, 0)));
        if (ia == nullptr) return finish(m, Status::InvalidParameter);
        ia->cached_background = m.arg32(1) != 0;
        finish(m, Status::Ok);
    };
    // GdipGetImageAttributesAdjustedPalette(ia, ColorPalette*, ColorAdjustType):
    // Bitmap..Text only (probe "adjpal_default=2"), Count 1..256.
    map["GdipGetImageAttributesAdjustedPalette"] = [&mgr, attrs](Marshaler& m) {
        GpImageAttributes* ia = attrs(m, 0);
        if (ia == nullptr) return finish(m, missing_object(mgr, handle_arg(m, 0)));
        const GuestAddr pal = m.arg_ptr(1);
        const i32 type = static_cast<i32>(m.arg32(2));
        if (ia == nullptr || pal == 0 || type <= ColorAdjustTypeDefault || type >= ColorAdjustTypeCount)
            return finish(m, Status::InvalidParameter);
        const auto count = m.read_u32(pal + 4);
        if (count.is_err() || count.value() == 0 || count.value() > 256) return finish(m, Status::InvalidParameter);
        std::vector<u32> entries(count.value());
        for (u32 i = 0; i < count.value(); ++i) {
            const auto e = m.read_u32(pal + 8 + 4ull * i);
            if (e.is_err()) return finish(m, Status::InvalidParameter);
            entries[i] = e.value();
        }
        apply_color_adjust(effective_adjust(*ia, type), entries.data(), entries.size());
        for (u32 i = 0; i < count.value(); ++i)
            if (m.write_u32(pal + 8 + 4ull * i, entries[i]).is_err()) return finish(m, Status::InvalidParameter);
        finish(m, Status::Ok);
    };
}

}  // namespace vitreo::winapi::gdip
