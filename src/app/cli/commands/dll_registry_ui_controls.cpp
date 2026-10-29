// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// DLL registry part: propsys, comctl32, riched20, uxtheme, dwmapi, Media Foundation and dcomp

#include "dll_registry_internal.hpp"

#include "windowing/cocoa/cocoa_bridge.hpp"
#include "winapi/com/propsys/propsys.hpp"
#include "winapi/graphics/dcomp/dcomp.hpp"
#include "winapi/graphics/dwmapi/dwmapi.hpp"
#include "winapi/user32/windows/nc_metrics.hpp"
#include "winapi/media/mfplat/mfplat.hpp"
#include "winapi/media/mfplay/mfplay.hpp"
#include "winapi/graphics/gdiplus/gdiplus.hpp"
#include "winapi/ui/msctf/msctf.hpp"
#include "winapi/ui/uxtheme/uxtheme.hpp"
#include "winapi/user32/user32_internal.hpp"
#include "winapi/ui/riched20/riched20.hpp"

#include <string>

namespace vitreo::cli {

void register_ui_controls_dlls(RuntimeContext& ctx, bool guest_is_64bit) {
    auto& reg = ctx.thunk_registry;

    // -- propsys.dll --
    {
        auto propsys_exports = winapi::make_propsys_exports(ctx.com_mgr, ctx.com_task_memory_mgr);
        {
            // Audit completions. The property system is not modelled; each entry nulls its out parameter and reports E_NOTIMPL rather than an empty store.
            winapi::DllExports audit;
            audit.entries.push_back({"PSCreateMemoryPropertyStore", 0,
                [](winapi::Marshaler& m) {
                    (void)m.arg(0);
                    if (const GuestAddr o = m.arg_ptr(1); o != 0) (void)m.write_ptr(o, 0);
                    m.set_return(0x80004001u);
                }, 8});
            audit.entries.push_back({"InitPropVariantFromCLSID", 0,
                [](winapi::Marshaler& m) {
                    (void)m.arg_ptr(0);
                    const GuestAddr out = m.arg_ptr(1);
                    if (out == 0) { m.set_return(0x80070057u); return; }
                    // A PROPVARIANT holding a CLSID needs an allocated copy of
                    // the GUID; without CoTaskMemAlloc ownership modelled here
                    // the honest answer is a cleared VT_EMPTY and a failure.
                    (void)m.write_u16(out, 0);
                    m.set_return(0x80004001u);
                }, 8});
            // HRESULT PropVariantToBSTR(REFPROPVARIANT, BSTR*)
            audit.entries.push_back({"PropVariantToBSTR", 0,
                [](winapi::Marshaler& m) {
                    (void)m.arg_ptr(0);
                    const GuestAddr out = m.arg_ptr(1);
                    if (out == 0) { m.set_return(0x80070057u); return; }
                    // A BSTR result needs SysAllocString ownership over a
                    // converted PROPVARIANT. The store is not modelled, so
                    // null the out parameter and report it rather than hand
                    // back a pointer the caller would SysFreeString.
                    (void)m.write_ptr(out, 0);
                    m.set_return(0x80004001u);
                }, 8});
            audit.entries.push_back({"VariantCompare", 0,
                [](winapi::Marshaler& m) {
                    (void)m.arg_ptr(0);
                    (void)m.arg_ptr(1);
                    // Ordering needs full VARIANT coercion; 0 would claim the
                    // two are equal, so report the comparison as unavailable.
                    m.set_return(0x80004001u);
                }, 8});
            merge_exports(propsys_exports, std::move(audit));
        }
        (void)reg.register_dll("propsys.dll", std::move(propsys_exports));
    }

    // -- comctl32.dll --
    {
        auto comctl_exports = winapi::create_comctl32_thunks(ctx.comctl32_mgr,
            [&ctx](winapi::Marshaler& m) { ctx.u32_ext_mgr.impl_TrackMouseEvent(m); },
            [&ctx](winapi::Marshaler& m) { ctx.u32_ext_mgr.impl_SetScrollInfo(m); },
            winapi::FlatScrollHandlers{
                [&ctx](winapi::Marshaler& m) { ctx.u32_ext_mgr.impl_GetScrollPos(m); },
                [&ctx](winapi::Marshaler& m) { ctx.u32_ext_mgr.impl_SetScrollPos(m); },
                [&ctx](winapi::Marshaler& m) { ctx.u32_ext_mgr.impl_GetScrollInfo(m); },
                [&ctx](winapi::Marshaler& m) { ctx.u32_ext_mgr.impl_GetScrollRange(m); },
                [&ctx](winapi::Marshaler& m) { ctx.u32_ext_mgr.impl_SetScrollRange(m); },
                [&ctx](winapi::Marshaler& m) { ctx.u32_ext_mgr.impl_EnableScrollBar(m); },
                [&ctx](winapi::Marshaler& m) { ctx.u32_ext_mgr.impl_ShowScrollBar(m); }});
        (void)reg.register_dll("comctl32.dll", std::move(comctl_exports));

        ctx.gdiplus_mgr.set_thunk_registry(&ctx.thunk_registry);
        ctx.gdiplus_mgr.set_vfs(&ctx.vfs);
        ctx.gdiplus_mgr.set_gdi_manager(&ctx.gdi_mgr);
        ctx.gdiplus_mgr.set_resource_manager(&ctx.resource_mgr);
        auto gdip_exports = winapi::create_gdiplus_thunks(ctx.gdiplus_mgr, guest_is_64bit);
        (void)reg.register_dll("gdiplus.dll", std::move(gdip_exports));
    }

    // -- riched20.dll / RichEd20.dll (EULA / license RichEdit controls) --
    {
        auto riched_exports = winapi::create_riched20_thunks();
        (void)reg.register_dll("riched20.dll", std::move(riched_exports));
    }
    // -- riched32.dll — Win95-era RichEdit 1.0 host; LoadLibrary target for
    // Older installers LoadLibrary this (Learn: Rich Edit Controls). Same surface
    // as riched20 for version probes; class registration is via user32.
    {
        auto riched32_exports = winapi::create_riched20_thunks();
        (void)reg.register_dll("riched32.dll", std::move(riched32_exports));
    }

    // -- uxtheme.dll: Aero's metrics, macOS artwork (AppKit palette) --
    {
        winapi::UxThemeServices ux;
        ux.hwnd_mgr = &ctx.hwnd_mgr;
        ux.gdi = &ctx.gdi_mgr;
        ux.host = &ctx.cocoa_bridge;
        ux.send_message = [&ctx](u32 hwnd, u32 msg, u64 wparam, u64 lparam) {
            return ctx.msgsend_mgr.send_message(static_cast<windowing::HWND>(hwnd), msg, wparam, lparam);
        };
        ux.system_metric = [&ctx](i32 index) {
            bool valid = false;
            return ctx.u32_ext_mgr.system_metric(index, 0, valid);
        };
        ux.parent = [&ctx](u32 hwnd) -> u32 {
            constexpr u32 WS_CHILD = 0x40000000u;
            const auto* win = ctx.hwnd_mgr.find(static_cast<windowing::HWND>(hwnd));
            if (win == nullptr || (win->style & WS_CHILD) == 0) return 0;
            return static_cast<u32>(win->parent);
        };
        ux.map_point = [&ctx](u32 from, u32 to, i32& x, i32& y) {
            const auto a = winapi::client_origin_to_desktop(ctx.hwnd_mgr, static_cast<windowing::HWND>(from));
            const auto b = winapi::client_origin_to_desktop(ctx.hwnd_mgr, static_cast<windowing::HWND>(to));
            x += a.x - b.x;
            y += a.y - b.y;
            return true;
        };
        auto ux_exports = winapi::create_uxtheme_thunks(ux);
        (void)reg.register_dll("uxtheme.dll", std::move(ux_exports));
    }

    // -- msctf.dll (Learn: inputscope.h, SetInputScope and siblings) --
    {
        auto msctf_exports = winapi::create_msctf_thunks(&ctx.hwnd_mgr);
        (void)reg.register_dll("msctf.dll", std::move(msctf_exports));
    }

    // -- dwmapi.dll --
    {
        auto dwm_exports = winapi::create_dwmapi_thunks(
            [&ctx](u32 hwnd, bool client_drawn) {
                if (auto* win = ctx.hwnd_mgr.find(
                        static_cast<windowing::HWND>(hwnd))) {
                    win->client_drawn_frame = client_drawn;
                    if (client_drawn) {
                        win->client_offset = {};
                    }
                }
                if (ctx.cocoa_bridge.has_native_window(
                        static_cast<windowing::HWND>(hwnd))) {
                    ctx.cocoa_bridge.set_client_drawn_frame(
                        static_cast<windowing::HWND>(hwnd), client_drawn);
                }
            }, ctx.cocoa_bridge.get_primary_monitor_info().refresh_rate_hz,
            winapi::DwmWindowServices{
                [&ctx](u32 hwnd) {
                    const auto* win = ctx.hwnd_mgr.find(
                        static_cast<windowing::HWND>(hwnd));
                    if (win == nullptr) return winapi::DwmWindowKind::Invalid;
                    constexpr u32 WS_CHILD = 0x40000000u;
                    return (win->style & WS_CHILD) != 0
                        ? winapi::DwmWindowKind::Child
                        : winapi::DwmWindowKind::TopLevel;
                },
                [&ctx](u64 hrgn) {
                    winapi::ClipRect rect{};
                    return ctx.gdi_mgr.get_region_rect(
                        static_cast<winapi::Handle>(hrgn), rect);
                },
                // DWMWA_EXTENDED_FRAME_BOUNDS, measured (s1win_dwm_probe):
                // the window rect without the invisible part of the frame
                // (frame thickness - 1 on the left, right and bottom; the top
                // edge is visible); a maximized window loses the whole frame
                // on every side, a minimized one only on the left and right.
                [&ctx](u32 hwnd, i32* rect) {
                    const auto* win = ctx.hwnd_mgr.find(
                        static_cast<windowing::HWND>(hwnd));
                    if (win == nullptr) return false;
                    const i32 f = winapi::nc_metrics::frame_thickness(win->style, 0);
                    const i32 inv = f > 0 ? f - 1 : 0;
                    rect[0] = win->rect.left;
                    rect[1] = win->rect.top;
                    rect[2] = win->rect.right;
                    rect[3] = win->rect.bottom;
                    if (win->maximized) {
                        rect[0] += f; rect[1] += f; rect[2] -= f; rect[3] -= f;
                    } else if (win->minimized) {
                        rect[0] += inv; rect[2] -= inv;
                    } else {
                        rect[0] += inv; rect[2] -= inv; rect[3] -= inv;
                    }
                    return true;
                },
                [&ctx](u32 hwnd) {
                    const auto* win = ctx.hwnd_mgr.find(
                        static_cast<windowing::HWND>(hwnd));
                    if (win == nullptr) return false;
                    constexpr u32 kCaption = 0x00C00000u, kDlgFrame = 0x00400000u,
                                  kThickFrame = 0x00040000u;
                    return (win->style & kCaption) == kCaption ||
                           (win->style & (kDlgFrame | kThickFrame)) != 0;
                }});
        (void)reg.register_dll("dwmapi.dll", std::move(dwm_exports));
    }
}

void register_media_foundation_dcomp_dlls(RuntimeContext& ctx) {
    auto& reg = ctx.thunk_registry;

    // -- Media Foundation: mfplat / mf / mfreadwrite / evr / msmpeg2vdec / dxva2 --
    // One consistent model (Learn "Media Feature Pack"): every DLL of the
    // pack exists, the platform starts up, attribute and media-type objects
    // are real, and nothing that needs a pipeline (transforms, sources,
    // sinks, renderers, decoders) exists -- enumerations are empty and every
    // factory fails loudly with its documented HRESULT. A Windows where
    // mfplat loads but evr/msmpeg2vdec do not is a state no edition has.
    {
        ctx.mfplat_mgr.set_thunk_registry(&ctx.thunk_registry);
        const GuestAddr mf_scratch = reserve_scratch_region(
            ctx.vmm, "mfplat COM",
            winapi::MfPlatManager::MFPLAT_SCRATCH_BASE,
            winapi::MfPlatManager::MFPLAT_SCRATCH_SIZE);
        if (mf_scratch != 0) ctx.mfplat_mgr.set_scratch_base(mf_scratch);
        // Buffers handed to the guest (GetItem, GetAllocated*) are freed by
        // PropVariantClear / CoTaskMemFree, so they come from ole32's task
        // allocator; stored IUnknowns are AddRef'd/Released/queried through
        // the guest's own vtables.
        ctx.mfplat_mgr.set_task_allocator(
            [&com = ctx.com_mgr](u64 size) -> GuestAddr {
                return com.co_task_mem_alloc_guest(size);
            },
            [&com = ctx.com_mgr](GuestAddr address) {
                com.co_task_mem_free_guest(address);
            });
        ctx.mfplat_mgr.set_guest_vtable_call(
            [&com = ctx.com_mgr](winapi::Marshaler& m, GuestAddr punk, u32 slot,
                                 const u64* args, usize count) -> u32 {
                return com.call_guest_slot(m, punk, slot, args, count);
            });
        (void)reg.register_dll("mfplat.dll",
                               winapi::create_mfplat_thunks(ctx.mfplat_mgr));
        (void)reg.register_dll("mfreadwrite.dll", winapi::create_mfreadwrite_thunks());
        (void)reg.register_dll("mf.dll", winapi::create_mf_thunks());
        (void)reg.register_dll("evr.dll", winapi::create_evr_thunks());
        (void)reg.register_dll("msmpeg2vdec.dll", winapi::create_msmpeg2vdec_thunks());
        (void)reg.register_dll("dxva2.dll", winapi::create_dxva2_thunks());
    }

    // -- mfplay.dll: the Media Foundation simple player, on the host media
    // backend. Its guest-call, window and presentation hooks are installed
    // with the run loop (install_media_callbacks).
    {
        ctx.mfplay_mgr.set_thunk_registry(&ctx.thunk_registry);
        (void)reg.register_dll("mfplay.dll", winapi::create_mfplay_thunks(ctx.mfplay_mgr));
    }

    // -- dcomp.dll --
    {
        auto dcomp_exports = winapi::create_dcomp_thunks(ctx.dcomp_mgr);
        (void)reg.register_dll("dcomp.dll", std::move(dcomp_exports));
        winapi::DcompManager::MethodThunks methods{};
        const auto thunk = [&](const std::string& name) {
            const auto address = reg.get_thunk_address("dcomp.dll", name);
            return address.is_ok() ? address.value() : GuestAddr{0};
        };
        for (usize slot = 0; slot < winapi::dcomp::DEVICE_METHOD_COUNT;
             ++slot) {
            methods.device[slot] = thunk(
                winapi::dcomp_device_method_export_name(slot));
        }
        for (usize slot = 0; slot < winapi::dcomp::DEVICE2_METHOD_COUNT;
             ++slot) {
            methods.device2[slot] = thunk(
                winapi::dcomp_device2_method_export_name(slot));
        }
        for (usize slot = 0;
             slot < winapi::dcomp::DESKTOP_DEVICE_METHOD_COUNT; ++slot) {
            methods.desktop_device[slot] = thunk(
                winapi::dcomp_desktop_method_export_name(slot));
        }
        for (usize slot = 0; slot < winapi::dcomp::DEVICE3_METHOD_COUNT;
             ++slot) {
            methods.device3[slot] = thunk(
                winapi::dcomp_device3_method_export_name(slot));
        }
        for (usize slot = 0; slot < winapi::dcomp::TARGET_METHOD_COUNT;
             ++slot) {
            methods.target[slot] = thunk(
                winapi::dcomp_target_method_export_name(slot));
        }
        for (usize slot = 0; slot < winapi::dcomp::VISUAL2_METHOD_COUNT;
             ++slot) {
            methods.visual2[slot] = thunk(
                winapi::dcomp_visual_method_export_name(slot));
        }
        for (usize slot = 0;
             slot < winapi::dcomp::TRANSLATE_TRANSFORM_METHOD_COUNT; ++slot) {
            methods.translate_transform[slot] = thunk(
                winapi::dcomp_translate_transform_method_export_name(slot));
        }
        for (usize slot = 0;
             slot < winapi::dcomp::SCALE_TRANSFORM_METHOD_COUNT; ++slot) {
            methods.scale_transform[slot] = thunk(
                winapi::dcomp_scale_transform_method_export_name(slot));
        }
        for (usize slot = 0;
             slot < winapi::dcomp::ROTATE_TRANSFORM_METHOD_COUNT; ++slot) {
            methods.rotate_transform[slot] = thunk(
                winapi::dcomp_rotate_transform_method_export_name(slot));
        }
        for (usize slot = 0;
             slot < winapi::dcomp::SKEW_TRANSFORM_METHOD_COUNT; ++slot) {
            methods.skew_transform[slot] = thunk(
                winapi::dcomp_skew_transform_method_export_name(slot));
        }
        for (usize slot = 0;
             slot < winapi::dcomp::MATRIX_TRANSFORM_METHOD_COUNT; ++slot) {
            methods.matrix_transform[slot] = thunk(
                winapi::dcomp_matrix_transform_method_export_name(slot));
        }
        for (usize slot = 0;
             slot < winapi::dcomp::RECTANGLE_CLIP_METHOD_COUNT; ++slot) {
            methods.rectangle_clip[slot] = thunk(
                winapi::dcomp_rectangle_clip_method_export_name(slot));
        }
        ctx.dcomp_mgr.set_method_thunks(std::move(methods));
    }
}

} // namespace vitreo::cli
