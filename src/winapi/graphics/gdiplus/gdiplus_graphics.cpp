// SPDX-License-Identifier: LGPL-2.1-only
/// @file gdiplus_graphics.cpp
/// @brief GDI+ area: startup/shutdown, GdipAlloc/Free, Graphics lifetime,
/// rendering state, world/page transforms, TransformPoints, Save/Restore,
/// containers, GetDC/ReleaseDC.
///
/// Contract: GdiPlusFlat.h, GdiPlusInit.h, GdiPlusEnums.h, the Learn
/// Graphics class pages, and every value Learn leaves open measured on
/// winoracle by tools/oracle_probe/gdiplus_graphics_probe.c (x64:
/// .expected, x86: .x86.expected). Quoted probe lines name the case.
/// Notes and gaps: .docs/contracts/gdiplus/graphics.md.

#include "winapi/graphics/gdiplus/gdiplus_graphics.hpp"
#include "winapi/graphics/gdiplus/gdiplus.hpp"
#include "winapi/graphics/gdiplus/gdiplus_geometry.hpp"
#include "winapi/graphics/gdiplus/gdiplus_image.hpp"
#include "winapi/graphics/gdiplus/gdiplus_region.hpp"
#include "winapi/graphics/gdi32/gdi32.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/core/thunk_registry.hpp"

#include <algorithm>
#include <cmath>
#include <mutex>
#include <unordered_map>

namespace vitreo::winapi::gdip {

// ---- Transform math --------------------------------------------------------

float unit_to_device(i32 unit, float dpi) {
    switch (unit) {
    case UnitPoint: return dpi / 72.0f;
    case UnitInch: return dpi;
    case UnitDocument: return dpi / 300.0f;
    case UnitMillimeter: return dpi / 25.4f;
    default: return 1.0f;  // World, Display (a display/image target), Pixel
    }
}

Affine page_to_container(const GpGraphics& g) {
    const GraphicsState& s = g.state;
    const float sx = unit_to_device(s.page_unit, g.dpi_x) * s.page_scale;
    const float sy = unit_to_device(s.page_unit, g.dpi_y) * s.page_scale;
    Affine page{sx, 0, 0, sy, 0, 0};
    // PixelOffsetModeHighQuality (2) / Half (4) sample pixel centres at
    // .5: device = page - 0.5 (probe "pom half TP": (1,1) -> (0.5,0.5)).
    if (s.pixel_offset_mode == 2 || s.pixel_offset_mode == 4) {
        page.dx = -0.5f;
        page.dy = -0.5f;
    }
    return page;
}

Affine world_to_device(const GpGraphics& g) {
    return g.state.world.then(page_to_container(g)).then(g.state.container_world);
}

bool affine_invert(const Affine& a, Affine& out) {
    const double det = static_cast<double>(a.m11) * a.m22 - static_cast<double>(a.m12) * a.m21;
    if (!(det < 0.0 || det > 0.0) || !std::isfinite(det)) return false;
    const double i11 = a.m22 / det, i12 = -a.m12 / det, i21 = -a.m21 / det, i22 = a.m11 / det;
    out.m11 = static_cast<float>(i11);
    out.m12 = static_cast<float>(i12);
    out.m21 = static_cast<float>(i21);
    out.m22 = static_cast<float>(i22);
    out.dx = static_cast<float>(-(a.dx * i11 + a.dy * i21));
    out.dy = static_cast<float>(-(a.dx * i12 + a.dy * i22));
    return true;
}

Status graphics_usable(const GpGraphics* g) {
    if (g == nullptr) return Status::InvalidParameter;
    if (g->hdc_out) return Status::ObjectBusy;
    return Status::Ok;
}

namespace {

/// GDI+ accepts a world matrix only when it is invertible: a zero
/// determinant (or NaN) is InvalidParameter, an overflowing one is not
/// (probe "SetWorld det0" 2, "SetWorld tiny"/"huge" 0, "Rotate(nan)" 2).
bool invertible(const Affine& a) {
    const double det = static_cast<double>(a.m11) * a.m22 - static_cast<double>(a.m12) * a.m21;
    return det < 0.0 || det > 0.0;  // false for zero and NaN
}

/// MatrixOrderPrepend (0): the operation applies first; Append (1): last.
bool combine(Affine& world, const Affine& op, i32 order) {
    if (order != 0 && order != 1) return false;
    world = order == 0 ? op.then(world) : world.then(op);
    return true;
}

Affine rotation(float degrees) {
    const double r = static_cast<double>(degrees) * (3.14159265358979323846 / 180.0);
    const float c = static_cast<float>(std::cos(r)), s = static_cast<float>(std::sin(r));
    return Affine{c, s, -s, c, 0, 0};
}

// ---- Startup state ---------------------------------------------------------

struct StartupState {
    std::unordered_map<u64, bool> tokens;  ///< live startup token -> SuppressBackgroundThread
    u64 next_token{0x6D10};
    u64 next_hook_token{0x6E10};
};

std::mutex& startup_mutex() {
    static std::mutex mu;
    return mu;
}
std::unordered_map<const GdiplusManager*, StartupState>& startup_states() {
    static std::unordered_map<const GdiplusManager*, StartupState> states;
    return states;
}

bool background_suppressed(const GdiplusManager& mgr) {
    std::lock_guard lock(startup_mutex());
    auto it = startup_states().find(&mgr);
    if (it == startup_states().end()) return false;
    for (const auto& [token, suppressed] : it->second.tokens) {
        if (suppressed) return true;
    }
    return false;
}

constexpr const char* kHookThunk = "Vitreo_GdiplusNotificationHook";
constexpr const char* kUnhookThunk = "Vitreo_GdiplusNotificationUnhook";

// ---- GDI DC helpers ----------------------------------------------------------

constexpr u32 OBJ_DC = 3, OBJ_METADC = 4, OBJ_MEMDC = 10, OBJ_ENHMETADC = 12;
constexpr i32 LOGPIXELSX = 88, LOGPIXELSY = 90;

/// GDI+ draws into the DC's device pixels through the DC's logical ->
/// device transform as it stood when the graphics was created: viewport
/// and window origin, mapping-mode extents and the GM_ADVANCED world
/// transform all apply, and a later change to the DC does not move GDI+
/// drawing (probe "vporg10,5", "vporg changed after create", "winorg3,4",
/// "aniso x2", "gdi world 7,2").
Status init_hdc_graphics(GdiplusManager& mgr, u64 hdc, GpGraphics& g) {
    GdiManager* gdi = mgr.gdi();
    if (gdi == nullptr) return Status::GenericError;
    // NULL or a handle that is no DC: OutOfMemory (probe "CreateFromHDC(NULL)",
    // "CreateFromHDC(bogus)").
    if (hdc == 0 || hdc > 0xFFFFFFFFull) return Status::OutOfMemory;
    const Handle h = static_cast<Handle>(hdc);
    const u32 type = gdi->object_type(h);
    if (type == OBJ_METADC || type == OBJ_ENHMETADC) {
        log_unimplemented("GdipCreateFromHDC", "graphics on a metafile DC (recording GDI+ drawing as EMF)");
        return Status::NotImplemented;
    }
    if (type != OBJ_DC && type != OBJ_MEMDC) return Status::OutOfMemory;
    Affine xform;
    (void)gdi->with_device_surface(h, [&](GdiManager::DeviceSurface& s) {
        xform = Affine{static_cast<float>(s.xform[0]), static_cast<float>(s.xform[1]),
                       static_cast<float>(s.xform[2]), static_cast<float>(s.xform[3]),
                       static_cast<float>(s.xform[4]), static_cast<float>(s.xform[5])};
    });
    g.hdc = hdc;
    g.state.container_world = xform;
    const i32 dx = gdi->get_device_caps(h, LOGPIXELSX);
    const i32 dy = gdi->get_device_caps(h, LOGPIXELSY);
    g.dpi_x = dx > 0 ? static_cast<float>(dx) : 96.0f;
    g.dpi_y = dy > 0 ? static_cast<float>(dy) : 96.0f;
    return Status::Ok;
}

/// GdipGetDC on an image: GDI+ hands out a memory DC holding a 32bpp
/// bottom-up DIB section of the image's size, every pixel the sentinel
/// 0x000D0B0C, with no transform or clip (probe "img GetDC raw[0]",
/// "img... dc vporg"). GdipReleaseDC stores each pixel whose RGB is no
/// longer the sentinel into the image as opaque, alpha ignored in the
/// compare (probe "img sentinel"); other pixels keep the image's value.
constexpr u8 kSentinelR = 0x0D, kSentinelG = 0x0B, kSentinelB = 0x0C;

Status open_image_dc(GdiplusManager& mgr, Marshaler& m, GpGraphics& g, u64& out_dc) {
    GdiManager* gdi = mgr.gdi();
    GpImage* image = mgr.objects().get<GpImage>(g.image);
    if (gdi == nullptr) return Status::GenericError;
    if (image == nullptr) return Status::InvalidParameter;
    if (image->type != ImageTypeBitmap) {
        log_unimplemented("GdipGetDC", "an HDC over a metafile-recording graphics");
        return Status::NotImplemented;
    }
    HeapManager& heap = mgr.heap();
    const GuestAddr process_heap = heap.get_process_heap();
    const GuestAddr scratch = heap.heap_alloc(process_heap, 0x8 /*HEAP_ZERO_MEMORY*/, 64);
    if (scratch == 0) return Status::OutOfMemory;
    // BITMAPINFOHEADER: 32bpp BI_RGB, positive height (bottom-up, as GDI+).
    (void)m.write_u32(scratch + 0, 40);
    (void)m.write_u32(scratch + 4, static_cast<u32>(image->width));
    (void)m.write_u32(scratch + 8, static_cast<u32>(image->height));
    (void)m.write_u16(scratch + 12, 1);
    (void)m.write_u16(scratch + 14, 32);
    const Handle dc = gdi->create_compatible_dc(0);
    const Handle dib = dc == 0 ? 0
                               : gdi->create_dib_section(dc, scratch, 0, scratch + 48,
                                                         m.cpu_is_64bit());
    (void)heap.heap_free(process_heap, 0, scratch);
    if (dib == 0) {
        if (dc != 0) (void)gdi->delete_dc(dc);
        return Status::OutOfMemory;
    }
    (void)gdi->select_object(dc, dib);
    (void)gdi->with_device_surface(dc, [](GdiManager::DeviceSurface& s) {
        const usize n = static_cast<usize>(s.width) * static_cast<usize>(s.height);
        for (usize i = 0; i < n; ++i) {
            s.rgba[i * 4] = kSentinelR;
            s.rgba[i * 4 + 1] = kSentinelG;
            s.rgba[i * 4 + 2] = kSentinelB;
            s.rgba[i * 4 + 3] = 0;
        }
    });
    g.image_dc = dc;
    g.image_dc_bitmap = dib;
    out_dc = dc;
    return Status::Ok;
}

Status close_image_dc(GdiplusManager& mgr, Marshaler& m, GpGraphics& g) {
    GdiManager* gdi = mgr.gdi();
    GpImage* image = mgr.objects().get<GpImage>(g.image);
    Status result = Status::Ok;
    if (gdi != nullptr && image != nullptr) {
        RenderPixels px;
        if (image_to_render(m, *image, px)) {
            bool changed = false;
            (void)gdi->with_device_surface(
                static_cast<Handle>(g.image_dc), [&](GdiManager::DeviceSurface& s) {
                    const i32 w = std::min(s.width, px.width), h = std::min(s.height, px.height);
                    for (i32 y = 0; y < h; ++y) {
                        for (i32 x = 0; x < w; ++x) {
                            const u8* src = s.rgba + (static_cast<usize>(y) * static_cast<usize>(s.width) +
                                                      static_cast<usize>(x)) * 4u;
                            if (src[0] == kSentinelR && src[1] == kSentinelG && src[2] == kSentinelB) {
                                continue;
                            }
                            u8* dst = px.bgra.data() + (static_cast<usize>(y) * static_cast<usize>(px.width) +
                                                         static_cast<usize>(x)) * 4u;
                            dst[0] = src[2];
                            dst[1] = src[1];
                            dst[2] = src[0];
                            dst[3] = 0xFF;
                            changed = true;
                        }
                    }
                });
            if (changed && !render_to_image(m, *image, px)) result = Status::NotImplemented;
        } else {
            result = Status::NotImplemented;
        }
    }
    if (gdi != nullptr) {
        (void)gdi->delete_dc(static_cast<Handle>(g.image_dc));
        (void)gdi->delete_object(static_cast<Handle>(g.image_dc_bitmap));
    }
    g.image_dc = 0;
    g.image_dc_bitmap = 0;
    return result;
}

// ---- Container math ----------------------------------------------------------

/// The state a new container level starts from: every property at its
/// fresh-graphics value except the rendering origin, which carries over
/// (probe "in container2": origin=3,4, all else default; clip infinite).
GraphicsState container_state(const GpGraphics& g, const Affine& base) {
    const GraphicsState& outer = g.state;
    GraphicsState inner;
    inner.rendering_origin_x = outer.rendering_origin_x;
    inner.rendering_origin_y = outer.rendering_origin_y;
    inner.container_world = base;
    // The enclosing levels keep clipping: the new level's container clip is
    // the enclosing container clip intersected with the enclosing clip.
    if (outer.container_clip && outer.clip) {
        auto both = region_clone(*outer.container_clip);
        region_combine(*both, *outer.clip, 1 /*CombineModeIntersect*/);
        inner.container_clip = std::move(both);
    } else {
        inner.container_clip = outer.clip ? outer.clip : outer.container_clip;
    }
    return inner;
}

/// GdipBeginContainer2: container coordinates take the enclosing world
/// transform's (and enclosing containers') linear part but NOT the page
/// unit/scale, anchored at the enclosing world origin's device point,
/// pixel offset included (probe "c2 outer scale2,3", "c2 outer inch",
/// "c2 outer inch T5,6", "c2 outer pom half", "nested c2").
Affine container2_base(const GpGraphics& g) {
    Affine base = g.state.world.then(g.state.container_world);
    const PointF origin = world_to_device(g).apply({0, 0});
    base.dx = origin.X;
    base.dy = origin.Y;
    return base;
}

/// GdipBeginContainer(dst, src, unit): a container point (in its page
/// pixels) is converted to @p unit, mapped src -> dst, then through the
/// enclosing world AND page transform (probe "rc outer pagescale2",
/// "rc outer inch", "rc src offset point", "BeginContainer(unit N)").
/// A zero-width or zero-height src maps identically (probe "src w0").
Affine container_rect_base(const GpGraphics& g, const RectF& dst, const RectF& src, i32 unit) {
    Affine rect;
    if (src.Width != 0 && src.Height != 0) {
        const float kx = dst.Width / src.Width, ky = dst.Height / src.Height;
        rect.m11 = kx / unit_to_device(unit, g.dpi_x);
        rect.m22 = ky / unit_to_device(unit, g.dpi_y);
        rect.dx = dst.X - src.X * kx;
        rect.dy = dst.Y - src.Y * ky;
    }
    return rect.then(world_to_device(g));
}

/// GdipTransformPoints matrix from @p src_space to @p dst_space
/// (CoordinateSpaceWorld 0, Page 1, Device 2).
bool space_matrix(const GpGraphics& g, i32 dst_space, i32 src_space, Affine& out) {
    auto to_device = [&](i32 space) {
        switch (space) {
        case 0: return world_to_device(g);
        case 1: return page_to_container(g).then(g.state.container_world);
        default: return Affine{};
        }
    };
    Affine inverse;
    if (!affine_invert(to_device(dst_space), inverse)) return false;
    out = to_device(src_space).then(inverse);
    return true;
}

}  // namespace

bool gdiplus_started(const GdiplusManager& mgr) {
    std::lock_guard lock(startup_mutex());
    auto it = startup_states().find(&mgr);
    return it != startup_states().end() && !it->second.tokens.empty();
}

void register_graphics(GdiplusManager& mgr, HandlerMap& map) {
    {
        // A fresh handler table is a fresh gdiplus.dll instance.
        std::lock_guard lock(startup_mutex());
        startup_states()[&mgr] = StartupState{};
    }
    auto graphics = [&mgr](Marshaler& m, usize index) {
        return mgr.objects().get<GpGraphics>(handle_arg(m, index));
    };
    auto arg_i32 = [](Marshaler& m, usize index) { return static_cast<i32>(m.arg(index)); };

    // ---- Startup / shutdown (GdiPlusInit.h) ----

    // Status GdiplusStartup(ULONG_PTR* token, const GdiplusStartupInput* input,
    //                       GdiplusStartupOutput* output)
    map["GdiplusStartup"] = [&mgr](Marshaler& m) {
        const GuestAddr token_out = m.arg_ptr(0);
        const GuestAddr input = m.arg_ptr(1);
        const GuestAddr output = m.arg_ptr(2);
        // probe "startup null-token" / "null-input": InvalidParameter.
        if (token_out == 0 || input == 0) { write_status(m, Status::InvalidParameter); return; }
        const auto version = m.read_u32(input);
        if (version.is_err()) { write_status(m, Status::InvalidParameter); return; }
        // GdiplusVersion 1, 2 (GdiplusStartupInputEx) and 3 (Ex V3) start;
        // anything else is UnsupportedGdiplusVersion (probe "startup version=").
        if (version.value() < 1 || version.value() > 3) {
            write_status(m, Status::UnsupportedGdiplusVersion);
            return;
        }
        // SuppressBackgroundThread follows the DebugEventCallback pointer:
        // +8 on x86, +16 on x64. DebugEventCallback is ignored (free build),
        // SuppressExternalCodecs changes nothing (no external codecs), and
        // StartupParameters (v2+) only governs GDI+ setting the x87 rounding
        // mode, which it leaves at the default round-to-nearest.
        const auto suppress = m.read_u32(input + (m.cpu_is_64bit() ? 16u : 8u));
        const bool suppress_bg = suppress.is_ok() && suppress.value() != 0;
        if (suppress_bg) {
            // The caller must receive the hooks (probe "startup suppress
            // null-output": InvalidParameter).
            if (output == 0) { write_status(m, Status::InvalidParameter); return; }
            GuestAddr hook = 0, unhook = 0;
            if (ThunkRegistry* thunks = mgr.thunks(); thunks != nullptr) {
                if (auto a = thunks->get_thunk_address("gdiplus.dll", kHookThunk); a.is_ok()) hook = a.value();
                if (auto a = thunks->get_thunk_address("gdiplus.dll", kUnhookThunk); a.is_ok()) unhook = a.value();
            }
            if (hook == 0 || unhook == 0) {
                log_unimplemented("GdiplusStartup", "notification hooks without a thunk registry");
                write_status(m, Status::GenericError);
                return;
            }
            // GdiplusStartupOutput { NotificationHook; NotificationUnhook; }:
            // GDI+'s internal entry points, not the same-named exports
            // (probe "hook_is_export=0").
            (void)m.write_ptr(output, hook);
            (void)m.write_ptr(output + (m.cpu_is_64bit() ? 8u : 4u), unhook);
        }
        // Without suppression the output is left untouched (probe
        // "startup no-suppress ... untouched").
        u64 token = 0;
        {
            std::lock_guard lock(startup_mutex());
            StartupState& st = startup_states()[&mgr];
            token = st.next_token;
            st.next_token += 0x10;
            st.tokens[token] = suppress_bg;
        }
        (void)m.write_ptr(token_out, token);
        write_status(m, Status::Ok);
    };

    // VOID GdiplusShutdown(ULONG_PTR token): GDI+ is reference counted; it
    // stays usable until the last startup's token is shut down (probe
    // "after one of two shutdowns", "after shutdown").
    map["GdiplusShutdown"] = [&mgr](Marshaler& m) {
        const u64 token = m.arg(0);
        std::lock_guard lock(startup_mutex());
        startup_states()[&mgr].tokens.erase(token);
    };

    // Status NotificationHook(ULONG_PTR* token): starts the work GDI+'s
    // background thread would do. Only meaningful while a startup suppressed
    // that thread: otherwise GenericError, token untouched (probe "export
    // hook without suppress" 1, "under suppress" 0). Vitreo's GDI+ has no
    // deferred background work, so there is nothing to run; each call hands
    // out a distinct token (probe "hook again ... same=0").
    auto hook = [&mgr](Marshaler& m) {
        const GuestAddr token_out = m.arg_ptr(0);
        if (!background_suppressed(mgr)) { write_status(m, Status::GenericError); return; }
        // Windows dereferences a NULL token pointer; refuse it instead.
        if (token_out == 0) { write_status(m, Status::InvalidParameter); return; }
        u64 token = 0;
        {
            std::lock_guard lock(startup_mutex());
            StartupState& st = startup_states()[&mgr];
            token = st.next_hook_token;
            st.next_hook_token += 0x10;
        }
        (void)m.write_ptr(token_out, token);
        write_status(m, Status::Ok);
    };
    // VOID NotificationUnhook(ULONG_PTR token): ends what the hook started.
    auto unhook = [](Marshaler& m) { (void)m.arg(0); };
    map["GdiplusNotificationHook"] = hook;
    map["GdiplusNotificationUnhook"] = unhook;
    map[kHookThunk] = hook;
    map[kUnhookThunk] = unhook;

    // ---- Memory (GdiPlusMem.h) ----

    // void* GdipAlloc(size_t): NULL before GdiplusStartup (probe
    // "pre-startup GdipAlloc nonnull=0"); a zero size still allocates
    // (probe "GdipAlloc(0) nonnull=1").
    map["GdipAlloc"] = [&mgr](Marshaler& m) {
        const u64 size = m.arg(0);
        if (!gdiplus_started(mgr)) { m.set_return(0); return; }
        HeapManager& heap = mgr.heap();
        m.set_return(heap.heap_alloc(heap.get_process_heap(), 0, size == 0 ? 1 : size));
    };
    // void GdipFree(void*): NULL is a no-op.
    map["GdipFree"] = [&mgr](Marshaler& m) {
        const u64 ptr = m.arg(0);
        if (ptr == 0) return;
        HeapManager& heap = mgr.heap();
        (void)heap.heap_free(heap.get_process_heap(), 0, ptr);
    };

    // Status GdipCreateStreamOnFile(const WCHAR*, UINT access, IStream**):
    // Windows' gdiplus.dll answers NotImplemented for every argument,
    // NULLs included, and writes nothing (probe "stream ... st=6").
    map["GdipCreateStreamOnFile"] = [](Marshaler& m) {
        write_status(m, Status::NotImplemented);
    };

    // ---- Graphics lifetime ----

    auto create_from_hdc = [&mgr](Marshaler& m, u64 hdc, GuestAddr out) {
        if (out == 0) { write_status(m, Status::InvalidParameter); return; }
        GpGraphics g;
        g.kind = TargetKind::Hdc;
        const Status s = init_hdc_graphics(mgr, hdc, g);
        if (s != Status::Ok) { write_status(m, s); return; }
        write_handle(m, out, mgr.objects().add(std::move(g)));
        write_status(m, Status::Ok);
    };
    // GdipCreateFromHDC(HDC, GpGraphics**)
    map["GdipCreateFromHDC"] = [create_from_hdc](Marshaler& m) {
        create_from_hdc(m, m.arg(0), m.arg_ptr(1));
    };
    // GdipCreateFromHDC2(HDC, HANDLE hDevice, GpGraphics**): hDevice names
    // a printer whose capabilities refine printing; a NULL one is plain
    // CreateFromHDC (probe "CreateFromHDC2(dc,NULL)").
    map["GdipCreateFromHDC2"] = [create_from_hdc](Marshaler& m) {
        if (m.arg(1) != 0) {
            log_unimplemented("GdipCreateFromHDC2", "printer device handle (printer capabilities)");
        }
        create_from_hdc(m, m.arg(0), m.arg_ptr(2));
    };
    // GdipCreateFromHWND[ICM](HWND, GpGraphics**): a graphics on GetDC(hwnd)
    // (NULL = the screen, probe "CreateFromHWND(NULL)" 0), released when the
    // graphics is deleted.
    auto create_from_hwnd = [&mgr](Marshaler& m) {
        const u64 hwnd = m.arg(0);
        const GuestAddr out = m.arg_ptr(1);
        if (out == 0) { write_status(m, Status::InvalidParameter); return; }
        GdiManager* gdi = mgr.gdi();
        if (gdi == nullptr) { write_status(m, Status::GenericError); return; }
        if (hwnd > 0xFFFFFFFFull) { write_status(m, Status::OutOfMemory); return; }
        const Handle hdc = gdi->get_dc(static_cast<Handle>(hwnd));
        if (hdc == 0) { write_status(m, Status::OutOfMemory); return; }
        GpGraphics g;
        g.kind = TargetKind::Hwnd;
        g.hwnd = hwnd;
        g.owns_hdc = true;
        const Status s = init_hdc_graphics(mgr, hdc, g);
        if (s != Status::Ok) {
            (void)gdi->release_dc(static_cast<Handle>(hwnd), hdc);
            write_status(m, s);
            return;
        }
        write_handle(m, out, mgr.objects().add(std::move(g)));
        write_status(m, Status::Ok);
    };
    map["GdipCreateFromHWND"] = create_from_hwnd;
    map["GdipCreateFromHWNDICM"] = [create_from_hwnd](Marshaler& m) {
        log_unimplemented("GdipCreateFromHWNDICM", "ICM colour correction of drawn images");
        create_from_hwnd(m);
    };

    // GdipDeleteGraphics(GpGraphics*): NULL InvalidParameter, ObjectBusy
    // while a GetDC is out (probe "Delete while out" 4).
    map["GdipDeleteGraphics"] = [&mgr, graphics](Marshaler& m) {
        const u64 h = handle_arg(m, 0);
        GpGraphics* g = graphics(m, 0);
        const Status s = graphics_usable(g);
        if (s != Status::Ok) { write_status(m, s); return; }
        if (g->owns_hdc && mgr.gdi() != nullptr) {
            (void)mgr.gdi()->release_dc(static_cast<Handle>(g->hwnd), static_cast<Handle>(g->hdc));
        }
        (void)mgr.objects().remove<GpGraphics>(h);
        write_status(m, Status::Ok);
    };

    // ---- GetDC / ReleaseDC ----

    // GdipGetDC(GpGraphics*, HDC*): the graphics is locked (every other
    // graphics export answers ObjectBusy) until GdipReleaseDC.
    map["GdipGetDC"] = [&mgr, graphics](Marshaler& m) {
        GpGraphics* g = graphics(m, 0);
        const GuestAddr out = m.arg_ptr(1);
        const Status s = graphics_usable(g);
        if (s != Status::Ok) { write_status(m, s); return; }
        if (out == 0) { write_status(m, Status::InvalidParameter); return; }
        u64 dc = g->hdc;
        if (g->kind == TargetKind::Image) {
            const Status os = open_image_dc(mgr, m, *g, dc);
            if (os != Status::Ok) { write_status(m, os); return; }
        }
        write_handle(m, out, dc);
        g->hdc_out = true;
        write_status(m, Status::Ok);
    };
    // GdipReleaseDC(GpGraphics*, HDC): any non-NULL HDC ends the lock
    // (probe "ReleaseDC(wrong)" 0); NULL, or no DC out, is InvalidParameter.
    map["GdipReleaseDC"] = [&mgr, graphics](Marshaler& m) {
        GpGraphics* g = graphics(m, 0);
        const u64 hdc = m.arg(1);
        if (g == nullptr || hdc == 0 || !g->hdc_out) {
            write_status(m, Status::InvalidParameter);
            return;
        }
        Status s = Status::Ok;
        if (g->kind == TargetKind::Image) s = close_image_dc(mgr, m, *g);
        g->hdc_out = false;
        write_status(m, s);
    };

    // ---- Rendering state ----

    // A getter: validate, then write the value through the out pointer at
    // @p out_index (NULL out = InvalidParameter, probe "null-out Get*").
    auto getter = [graphics](auto read) {
        return [graphics, read](Marshaler& m) {
            GpGraphics* g = graphics(m, 0);
            const Status s = graphics_usable(g);
            if (s != Status::Ok) { write_status(m, s); return; }
            const GuestAddr out = m.arg_ptr(1);
            if (out == 0) { write_status(m, Status::InvalidParameter); return; }
            (void)m.write_u32(out, read(*g));
            write_status(m, Status::Ok);
        };
    };
    // A setter whose INT argument passes @p accept (returning the value to
    // store) or is InvalidParameter.
    auto setter = [graphics, arg_i32](auto apply) {
        return [graphics, arg_i32, apply](Marshaler& m) {
            GpGraphics* g = graphics(m, 0);
            const Status s = graphics_usable(g);
            if (s != Status::Ok) { write_status(m, s); return; }
            write_status(m, apply(*g, arg_i32(m, 1)) ? Status::Ok : Status::InvalidParameter);
        };
    };

    // CompositingMode / CompositingQuality: GDI+ stores any value and
    // reports it back (probe "set CompositingMode(-1)/(100) st=0").
    map["GdipGetCompositingMode"] = getter([](GpGraphics& g) { return static_cast<u32>(g.state.compositing_mode); });
    map["GdipSetCompositingMode"] = setter([](GpGraphics& g, i32 v) { g.state.compositing_mode = v; return true; });
    map["GdipGetCompositingQuality"] = getter([](GpGraphics& g) { return static_cast<u32>(g.state.compositing_quality); });
    map["GdipSetCompositingQuality"] = setter([](GpGraphics& g, i32 v) { g.state.compositing_quality = v; return true; });
    // SmoothingMode 0..5; Default/HighSpeed store None (3), HighQuality
    // stores AntiAlias (4) (probe "set SmoothingMode(...)").
    map["GdipGetSmoothingMode"] = getter([](GpGraphics& g) { return static_cast<u32>(g.state.smoothing_mode); });
    map["GdipSetSmoothingMode"] = setter([](GpGraphics& g, i32 v) {
        if (v < 0 || v > 5) return false;
        g.state.smoothing_mode = v <= 1 ? 3 : v == 2 ? 4 : v;
        return true;
    });
    // InterpolationMode 0..7; Default/LowQuality store Bilinear (3),
    // HighQuality stores HighQualityBicubic (7).
    map["GdipGetInterpolationMode"] = getter([](GpGraphics& g) { return static_cast<u32>(g.state.interpolation_mode); });
    map["GdipSetInterpolationMode"] = setter([](GpGraphics& g, i32 v) {
        if (v < 0 || v > 7) return false;
        g.state.interpolation_mode = v <= 1 ? 3 : v == 2 ? 7 : v;
        return true;
    });
    // PixelOffsetMode 0..4, stored as given.
    map["GdipGetPixelOffsetMode"] = getter([](GpGraphics& g) { return static_cast<u32>(g.state.pixel_offset_mode); });
    map["GdipSetPixelOffsetMode"] = setter([](GpGraphics& g, i32 v) {
        if (v < 0 || v > 4) return false;
        g.state.pixel_offset_mode = v;
        return true;
    });
    // TextRenderingHint 0..5 (SystemDefault..ClearTypeGridFit).
    map["GdipGetTextRenderingHint"] = getter([](GpGraphics& g) { return static_cast<u32>(g.state.text_rendering_hint); });
    map["GdipSetTextRenderingHint"] = setter([](GpGraphics& g, i32 v) {
        if (v < 0 || v > 5) return false;
        g.state.text_rendering_hint = v;
        return true;
    });
    // TextContrast 0..12 (UINT; probe "set TextContrast(13)" 2).
    map["GdipGetTextContrast"] = getter([](GpGraphics& g) { return g.state.text_contrast; });
    map["GdipSetTextContrast"] = setter([](GpGraphics& g, i32 v) {
        if (static_cast<u32>(v) > 12) return false;
        g.state.text_contrast = static_cast<u32>(v);
        return true;
    });
    // PageUnit Display..Millimeter; UnitWorld is InvalidParameter.
    map["GdipGetPageUnit"] = getter([](GpGraphics& g) { return static_cast<u32>(g.state.page_unit); });
    map["GdipSetPageUnit"] = setter([](GpGraphics& g, i32 v) {
        if (v < UnitDisplay || v > UnitMillimeter) return false;
        g.state.page_unit = v;
        return true;
    });

    // GdipGetRenderingOrigin(GpGraphics*, INT* x, INT* y)
    map["GdipGetRenderingOrigin"] = [graphics](Marshaler& m) {
        GpGraphics* g = graphics(m, 0);
        const Status s = graphics_usable(g);
        if (s != Status::Ok) { write_status(m, s); return; }
        const GuestAddr x = m.arg_ptr(1), y = m.arg_ptr(2);
        if (x == 0 || y == 0) { write_status(m, Status::InvalidParameter); return; }
        (void)m.write_u32(x, static_cast<u32>(g->state.rendering_origin_x));
        (void)m.write_u32(y, static_cast<u32>(g->state.rendering_origin_y));
        write_status(m, Status::Ok);
    };
    // GdipSetRenderingOrigin(GpGraphics*, INT x, INT y): any value.
    map["GdipSetRenderingOrigin"] = [graphics, arg_i32](Marshaler& m) {
        GpGraphics* g = graphics(m, 0);
        const Status s = graphics_usable(g);
        if (s != Status::Ok) { write_status(m, s); return; }
        g->state.rendering_origin_x = arg_i32(m, 1);
        g->state.rendering_origin_y = arg_i32(m, 2);
        write_status(m, Status::Ok);
    };

    // REAL getters: PageScale, DpiX, DpiY.
    auto real_getter = [graphics](auto read) {
        return [graphics, read](Marshaler& m) {
            GpGraphics* g = graphics(m, 0);
            const Status s = graphics_usable(g);
            if (s != Status::Ok) { write_status(m, s); return; }
            const GuestAddr out = m.arg_ptr(1);
            if (out == 0) { write_status(m, Status::InvalidParameter); return; }
            (void)write_f32(m, out, read(*g));
            write_status(m, Status::Ok);
        };
    };
    map["GdipGetPageScale"] = real_getter([](GpGraphics& g) { return g.state.page_scale; });
    // DPI: the HDC's LOGPIXELSX/Y, or the image's resolution (probe "img
    // 72x150 ... dpi=72,150").
    map["GdipGetDpiX"] = real_getter([](GpGraphics& g) { return g.dpi_x; });
    map["GdipGetDpiY"] = real_getter([](GpGraphics& g) { return g.dpi_y; });
    // GdipSetPageScale(GpGraphics*, REAL): accepted when 1e-9 < scale <= 1e9;
    // the test is two ordered compares, so NaN passes (probe "pagescale").
    map["GdipSetPageScale"] = [graphics](Marshaler& m) {
        GpGraphics* g = graphics(m, 0);
        const Status s = graphics_usable(g);
        if (s != Status::Ok) { write_status(m, s); return; }
        const float scale = real_arg(m, 1);
        if (scale <= 1e-9f || scale > 1e9f) { write_status(m, Status::InvalidParameter); return; }
        g->state.page_scale = scale;
        write_status(m, Status::Ok);
    };
    // GdipResetPageTransform: UnitDisplay, scale 1.
    map["GdipResetPageTransform"] = [graphics](Marshaler& m) {
        GpGraphics* g = graphics(m, 0);
        const Status s = graphics_usable(g);
        if (s != Status::Ok) { write_status(m, s); return; }
        g->state.page_unit = UnitDisplay;
        g->state.page_scale = 1.0f;
        write_status(m, Status::Ok);
    };

    // ---- World transform ----

    auto matrix_arg = [&mgr](Marshaler& m, usize index) {
        return mgr.objects().get<GpMatrix>(handle_arg(m, index));
    };
    // GdipSetWorldTransform(GpGraphics*, GpMatrix*)
    map["GdipSetWorldTransform"] = [graphics, matrix_arg](Marshaler& m) {
        GpGraphics* g = graphics(m, 0);
        const Status s = graphics_usable(g);
        if (s != Status::Ok) { write_status(m, s); return; }
        GpMatrix* mx = matrix_arg(m, 1);
        if (mx == nullptr || !invertible(mx->m)) { write_status(m, Status::InvalidParameter); return; }
        g->state.world = mx->m;
        write_status(m, Status::Ok);
    };
    // GdipGetWorldTransform(GpGraphics*, GpMatrix*): copies into the matrix.
    map["GdipGetWorldTransform"] = [graphics, matrix_arg](Marshaler& m) {
        GpGraphics* g = graphics(m, 0);
        const Status s = graphics_usable(g);
        if (s != Status::Ok) { write_status(m, s); return; }
        GpMatrix* mx = matrix_arg(m, 1);
        if (mx == nullptr) { write_status(m, Status::InvalidParameter); return; }
        mx->m = g->state.world;
        write_status(m, Status::Ok);
    };
    map["GdipResetWorldTransform"] = [graphics](Marshaler& m) {
        GpGraphics* g = graphics(m, 0);
        const Status s = graphics_usable(g);
        if (s != Status::Ok) { write_status(m, s); return; }
        g->state.world = Affine{};
        write_status(m, Status::Ok);
    };
    // Apply @p op to the world with @p order; the result must stay
    // invertible, else InvalidParameter and the world is unchanged (probe
    // "Scale(0,0)", "Multiply(singular)", "Rotate(nan)").
    auto apply_world = [](Marshaler& m, GpGraphics& g, const Affine& op, i32 order) {
        Affine next = g.state.world;
        if (!combine(next, op, order) || !invertible(next)) {
            write_status(m, Status::InvalidParameter);
            return;
        }
        g.state.world = next;
        write_status(m, Status::Ok);
    };
    // GdipMultiplyWorldTransform(GpGraphics*, const GpMatrix*, MatrixOrder):
    // a NULL matrix is a successful no-op (probe "Mul(NULL m)=0").
    map["GdipMultiplyWorldTransform"] = [graphics, matrix_arg, apply_world, arg_i32](Marshaler& m) {
        GpGraphics* g = graphics(m, 0);
        const Status s = graphics_usable(g);
        if (s != Status::Ok) { write_status(m, s); return; }
        const i32 order = arg_i32(m, 2);
        if (order != 0 && order != 1) { write_status(m, Status::InvalidParameter); return; }
        if (handle_arg(m, 1) == 0) { write_status(m, Status::Ok); return; }
        GpMatrix* mx = matrix_arg(m, 1);
        if (mx == nullptr) { write_status(m, Status::InvalidParameter); return; }
        apply_world(m, *g, mx->m, order);
    };
    // GdipTranslateWorldTransform(GpGraphics*, REAL dx, REAL dy, MatrixOrder)
    map["GdipTranslateWorldTransform"] = [graphics, apply_world, arg_i32](Marshaler& m) {
        GpGraphics* g = graphics(m, 0);
        const Status s = graphics_usable(g);
        if (s != Status::Ok) { write_status(m, s); return; }
        apply_world(m, *g, Affine{1, 0, 0, 1, real_arg(m, 1), real_arg(m, 2)}, arg_i32(m, 3));
    };
    // GdipScaleWorldTransform(GpGraphics*, REAL sx, REAL sy, MatrixOrder)
    map["GdipScaleWorldTransform"] = [graphics, apply_world, arg_i32](Marshaler& m) {
        GpGraphics* g = graphics(m, 0);
        const Status s = graphics_usable(g);
        if (s != Status::Ok) { write_status(m, s); return; }
        apply_world(m, *g, Affine{real_arg(m, 1), 0, 0, real_arg(m, 2), 0, 0}, arg_i32(m, 3));
    };
    // GdipRotateWorldTransform(GpGraphics*, REAL degrees, MatrixOrder):
    // clockwise in GDI+'s y-down space, [cos sin; -sin cos].
    map["GdipRotateWorldTransform"] = [graphics, apply_world, arg_i32](Marshaler& m) {
        GpGraphics* g = graphics(m, 0);
        const Status s = graphics_usable(g);
        if (s != Status::Ok) { write_status(m, s); return; }
        apply_world(m, *g, rotation(real_arg(m, 1)), arg_i32(m, 2));
    };

    // GdipTransformPoints[I](GpGraphics*, CoordinateSpace dst, CoordinateSpace
    // src, points, INT count): spaces World/Page/Device only, count >= 1,
    // points non-NULL (probe "TP count0", "TP(3<-0)"). Page -> Device is
    // the page transform, pixel offset and container base (DC transform
    // included, probe "vporg10,5 TP").
    auto transform_points = [graphics, arg_i32](bool ints) {
        return [graphics, arg_i32, ints](Marshaler& m) {
            GpGraphics* g = graphics(m, 0);
            const Status s = graphics_usable(g);
            if (s != Status::Ok) { write_status(m, s); return; }
            const i32 dst = arg_i32(m, 1), src = arg_i32(m, 2);
            const GuestAddr pts = m.arg_ptr(3);
            const i32 count = arg_i32(m, 4);
            if (dst < 0 || dst > 2 || src < 0 || src > 2 || pts == 0 || count <= 0) {
                write_status(m, Status::InvalidParameter);
                return;
            }
            std::vector<PointF> p;
            if (!(ints ? read_points_i(m, pts, count, p) : read_points(m, pts, count, p))) {
                write_status(m, Status::InvalidParameter);
                return;
            }
            Affine x;
            if (!space_matrix(*g, dst, src, x)) { write_status(m, Status::InvalidParameter); return; }
            for (PointF& q : p) q = x.apply(q);
            if (ints) {
                // gp_round (gdiplus_geometry.hpp): floor(x + 0.5), as the
                // probe measures ("TPI scale.5": 0.5->1, -0.5->0, -1.5->-1).
                for (usize i = 0; i < p.size(); ++i) {
                    (void)m.write_u32(pts + i * 8, static_cast<u32>(gp_round(p[i].X)));
                    (void)m.write_u32(pts + i * 8 + 4, static_cast<u32>(gp_round(p[i].Y)));
                }
            } else {
                (void)write_points(m, pts, p);
            }
            write_status(m, Status::Ok);
        };
    };
    map["GdipTransformPoints"] = transform_points(false);
    map["GdipTransformPointsI"] = transform_points(true);

    // ---- Save / Restore / containers ----

    // GdipSaveGraphics(GpGraphics*, GraphicsState*): the token is opaque.
    map["GdipSaveGraphics"] = [graphics](Marshaler& m) {
        GpGraphics* g = graphics(m, 0);
        const Status s = graphics_usable(g);
        if (s != Status::Ok) { write_status(m, s); return; }
        const GuestAddr out = m.arg_ptr(1);
        if (out == 0) { write_status(m, Status::InvalidParameter); return; }
        const u32 token = g->next_state_token++;
        g->saved.push_back(SavedGraphicsState{token, false, g->state});
        (void)m.write_u32(out, token);
        write_status(m, Status::Ok);
    };
    // Pop to the entry with @p token of the given kind, restoring its
    // state; a token that matches nothing (popped, unknown, other kind) is
    // a successful no-op (probe "Restore(12345)", "mix ...").
    auto pop_to = [](GpGraphics& g, u32 token, bool container) {
        for (usize i = g.saved.size(); i-- > 0;) {
            if (g.saved[i].token == token && g.saved[i].container == container) {
                g.state = std::move(g.saved[i].state);
                g.saved.resize(i);
                return;
            }
        }
    };
    // GdipRestoreGraphics(GpGraphics*, GraphicsState)
    map["GdipRestoreGraphics"] = [graphics, pop_to](Marshaler& m) {
        GpGraphics* g = graphics(m, 0);
        const Status s = graphics_usable(g);
        if (s != Status::Ok) { write_status(m, s); return; }
        pop_to(*g, static_cast<u32>(m.arg(1)), false);
        write_status(m, Status::Ok);
    };
    // GdipEndContainer(GpGraphics*, GraphicsContainer)
    map["GdipEndContainer"] = [graphics, pop_to](Marshaler& m) {
        GpGraphics* g = graphics(m, 0);
        const Status s = graphics_usable(g);
        if (s != Status::Ok) { write_status(m, s); return; }
        pop_to(*g, static_cast<u32>(m.arg(1)), true);
        write_status(m, Status::Ok);
    };
    auto begin_container = [](Marshaler& m, GpGraphics& g, const Affine& base, GuestAddr out) {
        const u32 token = g.next_state_token++;
        g.saved.push_back(SavedGraphicsState{token, true, g.state});
        g.state = container_state(g, base);
        (void)m.write_u32(out, token);
        write_status(m, Status::Ok);
    };
    // GdipBeginContainer2(GpGraphics*, GraphicsContainer*)
    map["GdipBeginContainer2"] = [graphics, begin_container](Marshaler& m) {
        GpGraphics* g = graphics(m, 0);
        const Status s = graphics_usable(g);
        if (s != Status::Ok) { write_status(m, s); return; }
        const GuestAddr out = m.arg_ptr(1);
        if (out == 0) { write_status(m, Status::InvalidParameter); return; }
        begin_container(m, *g, container2_base(*g), out);
    };
    // GdipBeginContainer[I](GpGraphics*, const RectF* dst, const RectF* src,
    // Unit, GraphicsContainer*): unit Pixel..Millimeter (World and Display
    // are InvalidParameter, probe "BeginContainer(unit N)").
    auto begin_container_rect = [graphics, begin_container, arg_i32](bool ints) {
        return [graphics, begin_container, arg_i32, ints](Marshaler& m) {
            GpGraphics* g = graphics(m, 0);
            const Status s = graphics_usable(g);
            if (s != Status::Ok) { write_status(m, s); return; }
            const GuestAddr dst_at = m.arg_ptr(1), src_at = m.arg_ptr(2), out = m.arg_ptr(4);
            const i32 unit = arg_i32(m, 3);
            RectF dst, src;
            if (dst_at == 0 || src_at == 0 || out == 0 || unit < UnitPixel || unit > UnitMillimeter ||
                !(ints ? read_rect_i(m, dst_at, dst) : read_rect(m, dst_at, dst)) ||
                !(ints ? read_rect_i(m, src_at, src) : read_rect(m, src_at, src))) {
                write_status(m, Status::InvalidParameter);
                return;
            }
            begin_container(m, *g, container_rect_base(*g, dst, src, unit), out);
        };
    };
    map["GdipBeginContainer"] = begin_container_rect(false);
    map["GdipBeginContainerI"] = begin_container_rect(true);

    // ---- Miscellaneous ----

    // GdipGetNearestColor(GpGraphics*, ARGB*): GDI+ only changes the colour
    // for an 8bpp palettized display device; every other target (and
    // Vitreo's display is 32bpp) leaves it as given, alpha included (probe
    // "... nearest").
    map["GdipGetNearestColor"] = [graphics](Marshaler& m) {
        GpGraphics* g = graphics(m, 0);
        const Status s = graphics_usable(g);
        if (s != Status::Ok) { write_status(m, s); return; }
        write_status(m, m.arg_ptr(1) == 0 ? Status::InvalidParameter : Status::Ok);
    };
    // GdipFlush(GpGraphics*, FlushIntention): every draw already reached its
    // target, so both intentions (and any value, probe "Flush(-1)") are Ok.
    map["GdipFlush"] = [graphics](Marshaler& m) {
        write_status(m, graphics_usable(graphics(m, 0)));
    };
    // GdipComment(GpGraphics*, UINT size, const BYTE*): only a graphics
    // recording a metafile takes a comment; others are InvalidParameter
    // (probe "img Comment", "hdc Comment").
    map["GdipComment"] = [&mgr, graphics](Marshaler& m) {
        GpGraphics* g = graphics(m, 0);
        const Status s = graphics_usable(g);
        if (s != Status::Ok) { write_status(m, s); return; }
        const GpImage* image = g->kind == TargetKind::Image ? mgr.objects().get<GpImage>(g->image) : nullptr;
        if (image != nullptr && image->type == ImageTypeMetafile) {
            log_unimplemented("GdipComment", "EMF+ comment record on a metafile-recording graphics");
            write_status(m, Status::NotImplemented);
            return;
        }
        write_status(m, Status::InvalidParameter);
    };
    // GdipGraphicsSetAbort(GpGraphics*, GdiplusAbort*): stores the callback
    // object (NULL clears it). GDI+ polls it only inside long operations at
    // its discretion; Vitreo's draws run to completion and never poll.
    map["GdipGraphicsSetAbort"] = [graphics](Marshaler& m) {
        GpGraphics* g = graphics(m, 0);
        const Status s = graphics_usable(g);
        if (s != Status::Ok) { write_status(m, s); return; }
        g->abort_object = m.arg(1);
        write_status(m, Status::Ok);
    };
    // GdipPlayTSClientRecord: undocumented (Remote Desktop client EMF+
    // playback). With a NULL record argument Windows answers 10011 before
    // looking at the graphics (probe "PlayTSClientRecord": 10011 for (g,
    // NULL...) and (NULL, NULL...); GenericError for garbage).
    map["GdipPlayTSClientRecord"] = [](Marshaler& m) {
        if (m.arg(1) == 0) { m.set_return(10011); return; }
        log_unimplemented("GdipPlayTSClientRecord", "Terminal Services client record playback");
        write_status(m, Status::NotImplemented);
    };
}

}  // namespace vitreo::winapi::gdip
