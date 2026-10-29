/// @file uxtheme_properties.cpp
/// @brief Theme property getters and part metrics.
///
/// Measured on Windows 11 (uxtheme_aero_probe, "prop" and "api" lines):
///  - a getter reads the property's value whatever its declared type:
///    GetThemeInt on a colour is the COLORREF, GetThemeMargins on an int
///    reads the int and the bytes after it. Only GetThemeFont checks the
///    type (E_INVALIDARG), and GetThemeIntList takes the first int as the
///    count (more than MAX_INTLIST_COUNT is E_PROP_ID_UNSUPPORTED);
///  - lookup runs [part, state], [part], class; an unknown state therefore
///    finds the part's value, an unknown part E_PROP_ID_UNSUPPORTED;
///  - NULL or bogus handle E_HANDLE, then a NULL out pointer E_POINTER;
///    nothing is written and the last error is untouched on failure.

#include "winapi/ui/uxtheme/uxtheme_internal.hpp"

#include "winapi/kernel32/error.hpp"

#include <algorithm>
#include <cmath>

namespace vitreo::winapi::uxtheme {

namespace {

constexpr i32 MAX_INTLIST_COUNT = 402;

/// The value a property's bytes hold, as the getters read them.
std::array<i32, 4> raw_value(const UxThemeModule& mod, const ThemeObject& t, const PropertyRecord& p, i32 part,
                             i32 state) {
    std::array<i32, 4> v{p.value[0], p.value[1], p.value[2], p.value[3]};
    if (p.type == PropType::Color) {
        v = {static_cast<i32>(theme_colorref(t, mod.palette(t.dark), part, state, p.id)), 0, 0, 0};
    } else if (p.type == PropType::IntList) {
        const auto list = int_list(p.extra);
        v = {static_cast<i32>(list.size()), 0, 0, 0};
        for (usize i = 0; i < 3 && i < list.size(); ++i) v[i + 1] = list[i];
    }
    return v;
}

struct Lookup {
    std::optional<ThemeObject> theme;
    const PropertyRecord* prop = nullptr;
    u32 hr = S_OK;
};

/// Handle, out pointer and property, in Windows' order.
Lookup lookup(const UxThemeModule& mod, u64 handle, GuestAddr out, i32 part, i32 state, i32 id) {
    Lookup l;
    l.theme = mod.theme(handle);
    if (!l.theme) {
        l.hr = E_HANDLE;
        return l;
    }
    if (out == 0) {
        l.hr = E_POINTER;
        return l;
    }
    l.prop = find_property(l.theme->variant, part, state, id, nullptr);
    if (l.prop == nullptr) l.hr = E_PROP_ID_UNSUPPORTED;
    return l;
}

/// A size/content query's [part, state] section: E_FAIL when the class has none.
struct PartLookup {
    std::optional<ThemeObject> theme;
    const PartStateMetrics* ps = nullptr;
    u32 hr = S_OK;
};

PartLookup part_lookup(const UxThemeModule& mod, u64 handle, i32 part, i32 state) {
    PartLookup l;
    l.theme = mod.theme(handle);
    if (!l.theme) {
        l.hr = E_HANDLE;
        return l;
    }
    l.ps = find_part_state(l.theme->variant, part, state);
    if (l.ps == nullptr) l.hr = E_FAIL;
    return l;
}

}  // namespace

void add_property_exports(DllExports& exports, UxThemeModule& mod_ref) {
    UxThemeModule* mod = &mod_ref;
    auto add = [&](const char* name, u16 ordinal, u16 stdcall, std::function<void(Marshaler&)> fn) {
        exports.entries.push_back(ExportEntry{name, ordinal, std::move(fn), stdcall});
    };

    // Scalar getters: (HTHEME, int iPartId, int iStateId, int iPropId, T* out).
    auto scalar = [&](const char* name, u16 ordinal) {
        add(name, ordinal, 20, [mod](Marshaler& m) {
            const i32 part = static_cast<i32>(m.arg(1)), state = static_cast<i32>(m.arg(2));
            const GuestAddr out = m.arg_ptr(4);
            const Lookup l = lookup(*mod, m.arg(0), out, part, state, static_cast<i32>(m.arg(3)));
            if (l.hr != S_OK) {
                m.set_return(l.hr);
                return;
            }
            (void)m.write_u32(out, static_cast<u32>(raw_value(*mod, *l.theme, *l.prop, part, state)[0]));
            m.set_return(S_OK);
        });
    };
    scalar("GetThemeBool", 158);
    scalar("GetThemeColor", 159);
    scalar("GetThemeEnumValue", 161);
    scalar("GetThemeInt", 164);

    // HRESULT GetThemeMetric(HTHEME, HDC, int, int, int, int*): an int at the theme's DPI.
    add("GetThemeMetric", 167, 24, [mod](Marshaler& m) {
        const i32 part = static_cast<i32>(m.arg(2)), state = static_cast<i32>(m.arg(3));
        const GuestAddr out = m.arg_ptr(5);
        const Lookup l = lookup(*mod, m.arg(0), out, part, state, static_cast<i32>(m.arg(4)));
        if (l.hr != S_OK) {
            m.set_return(l.hr);
            return;
        }
        // Scaled to the theme's DPI like MulDiv (measured dprop: 6 -> 8 at 120 DPI).
        const i64 v = raw_value(*mod, *l.theme, *l.prop, part, state)[0];
        const i64 scaled = l.theme->dpi == 96 ? v : (v * l.theme->dpi + (v >= 0 ? 48 : -48)) / 96;
        (void)m.write_u32(out, static_cast<u32>(scaled));
        m.set_return(S_OK);
    });

    // HRESULT GetThemePosition(HTHEME, int, int, int, POINT*)
    add("GetThemePosition", 169, 20, [mod](Marshaler& m) {
        const i32 part = static_cast<i32>(m.arg(1)), state = static_cast<i32>(m.arg(2));
        const GuestAddr out = m.arg_ptr(4);
        const Lookup l = lookup(*mod, m.arg(0), out, part, state, static_cast<i32>(m.arg(3)));
        if (l.hr != S_OK) {
            m.set_return(l.hr);
            return;
        }
        const auto v = raw_value(*mod, *l.theme, *l.prop, part, state);
        (void)m.write_u32(out, static_cast<u32>(v[0]));
        (void)m.write_u32(out + 4, static_cast<u32>(v[1]));
        m.set_return(S_OK);
    });

    // HRESULT GetThemeRect(HTHEME, int, int, int, RECT*)
    add("GetThemeRect", 171, 20, [mod](Marshaler& m) {
        const i32 part = static_cast<i32>(m.arg(1)), state = static_cast<i32>(m.arg(2));
        const GuestAddr out = m.arg_ptr(4);
        const Lookup l = lookup(*mod, m.arg(0), out, part, state, static_cast<i32>(m.arg(3)));
        if (l.hr != S_OK) {
            m.set_return(l.hr);
            return;
        }
        const auto v = raw_value(*mod, *l.theme, *l.prop, part, state);
        (void)write_rect(m, out, v.data());
        m.set_return(S_OK);
    });

    // HRESULT GetThemeMargins(HTHEME, HDC, int, int, int, LPCRECT prc, MARGINS*)
    add("GetThemeMargins", 166, 28, [mod](Marshaler& m) {
        const i32 part = static_cast<i32>(m.arg(2)), state = static_cast<i32>(m.arg(3));
        const GuestAddr out = m.arg_ptr(6);
        const Lookup l = lookup(*mod, m.arg(0), out, part, state, static_cast<i32>(m.arg(4)));
        if (l.hr != S_OK) {
            m.set_return(l.hr);
            return;
        }
        const auto v = raw_value(*mod, *l.theme, *l.prop, part, state);
        (void)write_rect(m, out, v.data());
        m.set_return(S_OK);
    });

    // HRESULT GetThemeIntList(HTHEME, int, int, int, INTLIST*)
    add("GetThemeIntList", 165, 20, [mod](Marshaler& m) {
        const i32 part = static_cast<i32>(m.arg(1)), state = static_cast<i32>(m.arg(2));
        const GuestAddr out = m.arg_ptr(4);
        const Lookup l = lookup(*mod, m.arg(0), out, part, state, static_cast<i32>(m.arg(3)));
        if (l.hr != S_OK) {
            m.set_return(l.hr);
            return;
        }
        if (l.prop->type == PropType::IntList) {
            const auto list = int_list(l.prop->extra);
            (void)m.write_u32(out, static_cast<u32>(list.size()));
            for (usize i = 0; i < list.size(); ++i)
                (void)m.write_u32(out + 4 + static_cast<GuestAddr>(i) * 4, static_cast<u32>(list[i]));
            m.set_return(S_OK);
            return;
        }
        const auto v = raw_value(*mod, *l.theme, *l.prop, part, state);
        if (v[0] < 0 || v[0] > MAX_INTLIST_COUNT) {
            m.set_return(E_PROP_ID_UNSUPPORTED);
            return;
        }
        (void)m.write_u32(out, static_cast<u32>(v[0]));
        for (i32 i = 0; i < v[0]; ++i)
            (void)m.write_u32(out + 4 + static_cast<GuestAddr>(i) * 4, i < 3 ? static_cast<u32>(v[i + 1]) : 0u);
        m.set_return(S_OK);
    });

    // HRESULT GetThemeFont(HTHEME, HDC, int, int, int, LOGFONTW*). Measured:
    // a NULL pFont is E_POINTER before the handle is looked at.
    add("GetThemeFont", 163, 24, [mod](Marshaler& m) {
        const i32 part = static_cast<i32>(m.arg(2)), state = static_cast<i32>(m.arg(3));
        const GuestAddr out = m.arg_ptr(5);
        if (out == 0) {
            m.set_return(E_POINTER);
            return;
        }
        const Lookup l = lookup(*mod, m.arg(0), out, part, state, static_cast<i32>(m.arg(4)));
        if (l.hr != S_OK) {
            m.set_return(l.hr);
            return;
        }
        if (l.prop->type != PropType::Font) {
            m.set_return(E_INVALIDARG);
            return;
        }
        const auto& f = font_record(l.prop->extra);
        for (u32 i = 0; i < 92; i += 4) (void)m.write_u32(out + i, 0);
        (void)m.write_u32(out, static_cast<u32>(f.height));
        (void)m.write_u32(out + 4, static_cast<u32>(f.width));
        (void)m.write_u32(out + 16, static_cast<u32>(f.weight));
        (void)m.write_u8(out + 20, static_cast<u8>(f.italic));
        (void)m.write_u8(out + 23, static_cast<u8>(f.charset));
        (void)write_wide(m, out + 28, 32, f.face);
        m.set_return(S_OK);
    });

    // HRESULT GetThemeString/GetThemeFilename(HTHEME, int, int, int, LPWSTR, int cch):
    // a property's bytes as UTF-16 (Aero exposes no string-typed properties).
    auto string_getter = [&](const char* name, u16 ordinal) {
        add(name, ordinal, 24, [mod](Marshaler& m) {
            const i32 part = static_cast<i32>(m.arg(1)), state = static_cast<i32>(m.arg(2));
            const GuestAddr out = m.arg_ptr(4);
            const i32 cch = static_cast<i32>(m.arg(5));
            const Lookup l = lookup(*mod, m.arg(0), out, part, state, static_cast<i32>(m.arg(3)));
            if (l.hr != S_OK) {
                m.set_return(l.hr);
                return;
            }
            const auto v = raw_value(*mod, *l.theme, *l.prop, part, state);
            std::u16string s;
            for (i32 word : v) {
                for (int half = 0; half < 2; ++half) {
                    const char16_t c = static_cast<char16_t>((static_cast<u32>(word) >> (half * 16)) & 0xFFFFu);
                    if (c == 0) goto done;
                    s.push_back(c);
                }
            }
        done:
            if (cch > 0) (void)write_wide(m, out, static_cast<u32>(cch), s);
            m.set_return(S_OK);
        });
    };
    string_getter("GetThemeString", 173);
    string_getter("GetThemeFilename", 162);

    // HRESULT GetThemeStream(HTHEME, int, int, int, VOID** ppvStream, DWORD* pcbStream, HINSTANCE)
    add("GetThemeStream", 172, 28, [mod](Marshaler& m) {
        const i32 part = static_cast<i32>(m.arg(1)), state = static_cast<i32>(m.arg(2));
        const GuestAddr out = m.arg_ptr(4);
        const Lookup l = lookup(*mod, m.arg(0), out, part, state, static_cast<i32>(m.arg(3)));
        if (l.hr != S_OK) {
            m.set_return(l.hr);
            return;
        }
        // A stream points into the theme file's own data, which Vitreo does
        // not have.
        unimplemented_once("GetThemeStream: theme-file data streams");
        m.set_return(E_PROP_ID_UNSUPPORTED);
    });

    // HRESULT GetThemePropertyOrigin(HTHEME, int, int, int, PROPERTYORIGIN*)
    add("GetThemePropertyOrigin", 170, 20, [mod](Marshaler& m) {
        const auto t = mod->theme(m.arg(0));
        if (!t) {
            m.set_return(E_HANDLE);
            return;
        }
        const GuestAddr out = m.arg_ptr(4);
        if (out == 0) {
            m.set_return(E_POINTER);
            return;
        }
        Origin origin = Origin::NotFound;
        if (find_property(t->variant, static_cast<i32>(m.arg(1)), static_cast<i32>(m.arg(2)),
                          static_cast<i32>(m.arg(3)), &origin) == nullptr)
            origin = Origin::NotFound;
        (void)m.write_u32(out, static_cast<u32>(origin));
        m.set_return(S_OK);
    });

    // HRESULT GetThemeTransitionDuration(HTHEME, int iPartId, int iStateIdFrom,
    //     int iStateIdTo, int iPropId, DWORD* pdwDuration). The value is the
    // TMT_TRANSITIONDURATIONS matrix: list[0] = n, then n x n durations.
    add("GetThemeTransitionDuration", 184, 24, [mod](Marshaler& m) {
        const auto t = mod->theme(m.arg(0));
        if (!t) {
            m.set_return(E_HANDLE);
            return;
        }
        const i32 part = static_cast<i32>(m.arg(1));
        const i32 from = static_cast<i32>(m.arg(2)), to = static_cast<i32>(m.arg(3));
        const GuestAddr out = m.arg_ptr(5);
        if (out == 0) {
            m.set_return(E_INVALIDARG);
            return;
        }
        if (from <= 0 || to <= 0) {
            m.set_return(E_INVALIDARG);
            return;
        }
        (void)m.write_u32(out, 0);
        const PropertyRecord* p = find_property(t->variant, part, from, static_cast<i32>(m.arg(4)), nullptr);
        if (p == nullptr) {
            m.set_return(E_PROP_ID_UNSUPPORTED);
            return;
        }
        if (p->type != PropType::IntList) {
            m.set_return(E_INVALIDARG);
            return;
        }
        const auto list = int_list(p->extra);
        const i32 n = list.empty() ? 0 : list[0];
        const usize at = 1 + static_cast<usize>(from - 1) * static_cast<usize>(n) + static_cast<usize>(to - 1);
        if (from > n || to > n || at >= list.size()) {
            m.set_return(E_INVALIDARG);
            return;
        }
        (void)m.write_u32(out, static_cast<u32>(list[at]));
        m.set_return(S_OK);
    });

    // HRESULT GetThemeAnimationProperty(HTHEME, int iStoryboardId, int iTargetId,
    //     TA_PROPERTY, VOID*, DWORD cbSize, DWORD* pcbSizeOut). Animation
    // storyboards live in the "Animations" class only; every class here has
    // none. *pcbSizeOut is the property's size (each TA_PROPERTY is 4 bytes).
    add("GetThemeAnimationProperty", 151, 28, [mod](Marshaler& m) {
        const u64 h = m.arg(0);
        const GuestAddr out = m.arg_ptr(4), out_size = m.arg_ptr(6);
        if (h == 0 || out == 0) {
            m.set_return(E_INVALIDARG);
            return;
        }
        if (!mod->theme(h)) {
            m.set_return(E_HANDLE);
            return;
        }
        if (out_size != 0) (void)m.write_u32(out_size, 4);
        m.set_return(E_PROP_ID_UNSUPPORTED);
    });
    // HRESULT GetThemeAnimationTransform(HTHEME, int, int, DWORD, TA_TRANSFORM*, DWORD, DWORD*)
    add("GetThemeAnimationTransform", 152, 28, [mod](Marshaler& m) {
        const u64 h = m.arg(0);
        if (h == 0 || m.arg_ptr(4) == 0) {
            m.set_return(E_INVALIDARG);
            return;
        }
        m.set_return(mod->theme(h) ? E_PROP_ID_UNSUPPORTED : E_HANDLE);
    });
    // HRESULT GetThemeTimingFunction(HTHEME, int, TA_TIMINGFUNCTION*, DWORD, DWORD*)
    add("GetThemeTimingFunction", 183, 20, [mod](Marshaler& m) {
        const u64 h = m.arg(0);
        if (h == 0 || m.arg_ptr(2) == 0) {
            m.set_return(E_INVALIDARG);
            return;
        }
        m.set_return(mod->theme(h) ? E_PROP_ID_UNSUPPORTED : E_HANDLE);
    });

    // BOOL IsThemePartDefined(HTHEME, int, int): whether the class has a
    // [part, state] section. Last error E_HANDLE for a bad handle, else 0.
    add("IsThemePartDefined", 192, 12, [mod](Marshaler& m) {
        const auto t = mod->theme(m.arg(0));
        if (!t) {
            set_last_error(E_HANDLE);
            m.set_return(0);
            return;
        }
        set_last_error(0);
        const auto* ps = find_part_state(t->variant, static_cast<i32>(m.arg(1)), static_cast<i32>(m.arg(2)));
        m.set_return(ps != nullptr && (ps->flags & kDefined) != 0 ? 1 : 0);
    });
    // BOOL IsThemeBackgroundPartiallyTransparent(HTHEME, int, int)
    add("IsThemeBackgroundPartiallyTransparent", 190, 12, [mod](Marshaler& m) {
        const PartLookup l = part_lookup(*mod, m.arg(0), static_cast<i32>(m.arg(1)), static_cast<i32>(m.arg(2)));
        if (l.hr != S_OK) {
            set_last_error(l.hr);
            m.set_return(0);
            return;
        }
        set_last_error(0);
        m.set_return((l.ps->flags & kPartiallyTransparent) != 0 ? 1 : 0);
    });

    // HRESULT GetThemePartSize(HTHEME, HDC, int, int, LPCRECT prc, THEMESIZE, SIZE*)
    add("GetThemePartSize", 168, 28, [mod](Marshaler& m) {
        constexpr u32 TS_MIN = 0, TS_TRUE = 1, TS_DRAW = 2;
        const GuestAddr out = m.arg_ptr(6);
        const auto t = mod->theme(m.arg(0));
        if (!t) {
            m.set_return(E_HANDLE);
            return;
        }
        if (out == 0) {
            m.set_return(E_POINTER);
            return;
        }
        const auto* ps = find_part_state(t->variant, static_cast<i32>(m.arg(2)), static_cast<i32>(m.arg(3)));
        if (ps == nullptr) {
            m.set_return(E_FAIL);
            return;
        }
        const u32 kind = static_cast<u32>(m.arg(5));
        i32 r[4]{};
        const bool has_rect = read_rect(m, m.arg_ptr(4), r);
        i32 cx = 0, cy = 0;
        if (kind == TS_MIN) {
            cx = ps->min_cx;
            cy = ps->min_cy;
        } else if (kind == TS_TRUE) {
            cx = ps->true_cx;
            cy = ps->true_cy;
        } else if (kind == TS_DRAW) {
            const i32 rw = r[2] - r[0], rh = r[3] - r[1];
            // A true-size image with TRUESIZESCALINGTYPE = TSST_DPI grows with
            // the theme's DPI once the scale passes TRUESIZESTRETCHMARK percent
            // (measured "dpi" lines: a 13x13 check box is 13 at 120 DPI, 20 at
            // 144 and 26 at 192).
            i32 tcx = ps->true_cx, tcy = ps->true_cy;
            if (t->dpi != 96) {
                const auto* scaling = find_property(t->variant, ps->part, ps->state, tmt::TRUESIZESCALINGTYPE, nullptr);
                const auto* mark = find_property(t->variant, ps->part, ps->state, tmt::TRUESIZESTRETCHMARK, nullptr);
                const i64 threshold = mark ? mark->value[0] : 0;
                if (scaling && scaling->value[0] == 2 && (static_cast<i64>(t->dpi) - 96) * 100 >= threshold * 96) {
                    // Aero picks a per-DPI image: its measured size, else the scaled one.
                    if (const auto* d = find_dpi_draw_size(t->variant, ps->part, t->dpi)) {
                        tcx = d->cx;
                        tcy = d->cy;
                    } else {
                        tcx = static_cast<i32>((static_cast<i64>(tcx) * t->dpi + 48) / 96);
                        tcy = static_cast<i32>((static_cast<i64>(tcy) * t->dpi + 48) / 96);
                    }
                }
            }
            // Shrinking keeps the aspect ratio when the part has
            // TMT_UNIFORMSIZING, else each side is clamped on its own
            // (measured at 96 and 192 DPI).
            DrawKind kind = ps->draw;
            if (kind == DrawKind::TrueClamp || kind == DrawKind::TrueUniform) {
                const auto* uniform = find_property(t->variant, ps->part, ps->state, tmt::UNIFORMSIZING, nullptr);
                kind = uniform && uniform->value[0] != 0 ? DrawKind::TrueUniform : DrawKind::TrueClamp;
            }
            switch (kind) {
            case DrawKind::Invalid:
                m.set_return(E_INVALIDARG);
                return;
            case DrawKind::Zero:
                break;
            case DrawKind::Stretch:
                if (has_rect) {
                    cx = rw;
                    cy = rh;
                }
                break;
            case DrawKind::TrueFixed:
                cx = tcx;
                cy = tcy;
                break;
            case DrawKind::TrueClamp:
                cx = tcx;
                cy = tcy;
                if (has_rect) {
                    cx = std::min(cx, rw);
                    cy = std::min(cy, rh);
                }
                break;
            case DrawKind::TrueUniform:
                cx = tcx;
                cy = tcy;
                if (has_rect && (cx > rw || cy > rh) && cx > 0 && cy > 0) {
                    const double s = std::min(static_cast<double>(rw) / cx, static_cast<double>(rh) / cy);
                    cx = static_cast<i32>(cx * s);
                    cy = static_cast<i32>(cy * s);
                }
                break;
            }
        } else {
            m.set_return(E_INVALIDARG);
            return;
        }
        (void)m.write_u32(out, static_cast<u32>(cx));
        (void)m.write_u32(out + 4, static_cast<u32>(cy));
        m.set_return(S_OK);
    });

    // HRESULT GetThemeBackgroundContentRect(HTHEME, HDC, int, int, LPCRECT pBoundingRect, LPRECT)
    add("GetThemeBackgroundContentRect", 154, 24, [mod](Marshaler& m) {
        const GuestAddr out = m.arg_ptr(5);
        const auto t = mod->theme(m.arg(0));
        if (!t) { m.set_return(E_HANDLE); return; }
        i32 r[4];
        if (out == 0 || !read_rect(m, m.arg_ptr(4), r)) { m.set_return(E_POINTER); return; }
        const auto* ps = find_part_state(t->variant, static_cast<i32>(m.arg(2)), static_cast<i32>(m.arg(3)));
        if (ps == nullptr) { m.set_return(E_FAIL); return; }
        const i32 c[4] = {r[0] + ps->content[0], r[1] + ps->content[1], r[2] - ps->content[2], r[3] - ps->content[3]};
        (void)write_rect(m, out, c);
        m.set_return(S_OK);
    });
    // HRESULT GetThemeBackgroundExtent(HTHEME, HDC, int, int, LPCRECT pContentRect, LPRECT)
    add("GetThemeBackgroundExtent", 155, 24, [mod](Marshaler& m) {
        const GuestAddr out = m.arg_ptr(5);
        const auto t = mod->theme(m.arg(0));
        if (!t) { m.set_return(E_HANDLE); return; }
        i32 r[4];
        if (out == 0 || !read_rect(m, m.arg_ptr(4), r)) { m.set_return(E_POINTER); return; }
        const auto* ps = find_part_state(t->variant, static_cast<i32>(m.arg(2)), static_cast<i32>(m.arg(3)));
        if (ps == nullptr) { m.set_return(E_FAIL); return; }
        const i32 e[4] = {r[0] - ps->content[0], r[1] - ps->content[1], r[2] + ps->content[2], r[3] + ps->content[3]};
        (void)write_rect(m, out, e);
        m.set_return(S_OK);
    });
}

}  // namespace vitreo::winapi::uxtheme
