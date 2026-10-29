// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// DLL registry part: dinput / xinput, ddraw, dsound, d3d9, d3dx9, usp10 and vulkan-1

#include "dll_registry_internal.hpp"

#include "windowing/cocoa/cocoa_bridge.hpp"
#include "winapi/graphics/dwrite/font_backend.hpp"
#include "winapi/media/dinput8/dinput8.hpp"
#include "winapi/media/dinput/dinput.hpp"
#include "winapi/media/xinput/xinput.hpp"

#include <algorithm>
#include <cmath>
#include <optional>
#include <vector>

namespace vitreo::cli {

void register_input_and_legacy_graphics_dlls(RuntimeContext& ctx, GuestMemory& memory) {
    auto& reg = ctx.thunk_registry;

    // -- dinput.dll (DirectInput 3-7) + dinput8.dll --
    //
    // A DirectDraw client that probes for both ddraw.dll and dinput.dll
    // refuses its hardware video driver when either module is missing and
    // falls back to a GDI path, which denies the application DirectDraw
    // output.
    //
    // DirectInput8Create shares the same manager; COM method thunks live on
    // dinput.dll so register that first.
    {
        ctx.dinput_mgr.set_memory(&memory);
        ctx.dinput_mgr.set_thunk_registry(&ctx.thunk_registry);
        ctx.dinput_mgr.set_cocoa_bridge(&ctx.cocoa_bridge);
        ctx.dinput_mgr.set_sync_manager(&ctx.sync_mgr);
        ctx.cocoa_bridge.set_native_input_callbacks(
            [&ctx](u32 vk_code, bool down) {
                ctx.dinput_mgr.note_key_transition(vk_code, down);
                ctx.u32_ext_mgr.note_raw_keyboard_transition(vk_code, down);
            },
        [&ctx](u32 button, bool down, windowing::HWND hwnd) {
            ctx.dinput_mgr.note_mouse_button(button, down);
            ctx.u32_ext_mgr.note_raw_mouse_button(button, down, hwnd);
        },
        [&ctx](i32 x, i32 y, windowing::HWND hwnd, double device_dx,
               double device_dy) {
            (void)x;
            (void)y;
            ctx.dinput_mgr.note_mouse_motion(device_dx, device_dy);
            ctx.u32_ext_mgr.note_raw_mouse_motion(device_dx, device_dy, hwnd);
        });
        ctx.cocoa_bridge.set_native_wheel_callback(
            [&ctx](i32 delta, bool horizontal, windowing::HWND hwnd) {
                // DIMOUSESTATE has one wheel axis (lZ): vertical only.
                if (!horizontal) ctx.dinput_mgr.note_mouse_wheel(delta);
                ctx.u32_ext_mgr.note_raw_mouse_wheel(delta, horizontal, hwnd);
            });
        auto dinput_exports = winapi::create_dinput_thunks(ctx.dinput_mgr);
        (void)reg.register_dll("dinput.dll", std::move(dinput_exports));
        auto dinput8_exports = winapi::create_dinput8_thunks(ctx.dinput_mgr);
        (void)reg.register_dll("dinput8.dll", std::move(dinput8_exports));
    }

    // -- xinput1_4.dll / xinput1_3.dll / xinput9_1_0.dll --
    // Launchers and gamepad clients probe these in order. No attached pad:
    // documented ERROR_DEVICE_NOT_CONNECTED, not a missing module.
    {
        using winapi::XInputDll;
        (void)reg.register_dll("xinput1_4.dll",
                               winapi::create_xinput_thunks(XInputDll::V1_4));
        (void)reg.register_dll("xinput1_3.dll",
                               winapi::create_xinput_thunks(XInputDll::V1_3));
        (void)reg.register_dll("xinput9_1_0.dll",
                               winapi::create_xinput_thunks(XInputDll::V9_1_0));
    }

    // -- ddraw.dll --
    {
        ctx.ddraw_mgr.set_thunk_registry(&ctx.thunk_registry);
        auto ddraw_exports = winapi::create_ddraw_thunks(ctx.ddraw_mgr);
        (void)reg.register_dll("ddraw.dll", std::move(ddraw_exports));
    }

    // -- dsound.dll --
    {
        ctx.dsound_mgr.set_thunk_registry(&ctx.thunk_registry);
        auto dsound_exports = winapi::create_dsound_thunks(ctx.dsound_mgr);
        (void)reg.register_dll("dsound.dll", std::move(dsound_exports));
    }

    // -- d3d9.dll --
    {
        auto d3d9_exports = winapi::create_d3d9_thunks(ctx.d3d9_mgr);
        (void)reg.register_dll("d3d9.dll", std::move(d3d9_exports));
        // Publish the guest-callable address of every COM vtable slot, for
        // every D3D9 interface the frontend can map.
        for (usize index = 0; index < winapi::D3D9_INTERFACE_COUNT; ++index) {
            const auto iface = static_cast<winapi::D3D9InterfaceId>(index);
            const usize count = winapi::d3d9_interface_method_count(iface);
            winapi::D3D9Manager::InterfaceThunks thunks(count, GuestAddr{0});
            for (usize slot = 0; slot < count; ++slot) {
                const auto address = reg.get_thunk_address(
                    "d3d9.dll", winapi::d3d9_method_export_name(iface, slot));
                thunks[slot] = address.is_ok() ? address.value() : GuestAddr{0};
            }
            ctx.d3d9_mgr.set_interface_thunks(iface, std::move(thunks));
        }
    }

    // -- d3dx9_31.dll (and common version aliases) --
    // D3D9 titles import a few helpers. Register with correct stdcall so a
    // missing-export stub cannot smash the x86 stack.
    {
        auto make_d3dx = []() {
            winapi::DllExports dx;
            // UINT D3DXGetShaderSize(CONST DWORD*) — stdcall=4.
            // Walk SM1–3 tokens until END (0xFFFF), matching D3DX9Shader.h.
            dx.entries.push_back({"D3DXGetShaderSize", 0,
                [](winapi::Marshaler& m) {
                    const vitreo::GuestAddr func = m.arg_ptr(0);
                    if (func == 0) {
                        m.set_return(0);
                        return;
                    }
                    constexpr u32 OPCODE_MASK = 0xFFFFu;
                    constexpr u32 OPCODE_END = 0xFFFFu;
                    constexpr u32 OPCODE_COMMENT = 0xFFFE;
                    constexpr u32 LENGTH_SHIFT = 24;
                    constexpr u32 LENGTH_MASK = 0xFu;
                    constexpr u32 COMMENT_LENGTH_SHIFT = 16;
                    constexpr u32 COMMENT_LENGTH_MASK = 0x7FFFu;
                    constexpr u32 kMaxTokens = 1u << 20;
                    u32 tokens = 0;
                    vitreo::GuestAddr cursor = func;
                    while (tokens < kMaxTokens) {
                        const auto word = m.memory().read_u32(cursor);
                        if (word.is_err()) {
                            m.set_return(0);
                            return;
                        }
                        const u32 token = word.value();
                        ++tokens;
                        cursor += 4;
                        const u32 opcode = token & OPCODE_MASK;
                        if (opcode == OPCODE_END) {
                            m.set_return(tokens * 4u);
                            return;
                        }
                        u32 extra = 0;
                        if (opcode == OPCODE_COMMENT) {
                            extra = (token >> COMMENT_LENGTH_SHIFT) &
                                    COMMENT_LENGTH_MASK;
                        } else {
                            extra = (token >> LENGTH_SHIFT) & LENGTH_MASK;
                        }
                        if (tokens + extra > kMaxTokens) {
                            m.set_return(0);
                            return;
                        }
                        tokens += extra;
                        cursor += static_cast<vitreo::GuestAddr>(extra) * 4;
                    }
                    m.set_return(0);
                }, 4});
            // HRESULT D3DXFillVolumeTexture(...) — stdcall=12. Not implemented.
            dx.entries.push_back({"D3DXFillVolumeTexture", 0,
                [](winapi::Marshaler& m) {
                    (void)m.arg(0); (void)m.arg(1); (void)m.arg(2);
                    VITREO_TRACE("[d3dx9] D3DXFillVolumeTexture -> E_NOTIMPL\n");
                    m.set_return(0x80004001u);
                }, 12});
            return dx;
        };
        (void)reg.register_dll("d3dx9_31.dll", make_d3dx());
    }

    // -- usp10.dll (Uniscribe) --
    // Minimal LTR Uniscribe surface for callers that only need simple Latin
    // shaping. Unsupported complex runs fail honestly rather than publishing
    // fabricated glyph data.
    {
        winapi::DllExports usp;
        // HRESULT ScriptItemize(WCHAR*, int, int, SCRIPT_CONTROL*,
        //   SCRIPT_STATE*, SCRIPT_ITEM*, int*) — stdcall=28.
        usp.entries.push_back({"ScriptItemize", 0,
            [](winapi::Marshaler& m) {
                constexpr u32 E_INVALIDARG = 0x80070057u;
                constexpr u32 E_OUTOFMEMORY = 0x8007000Eu;
                const vitreo::GuestAddr chars = m.arg_ptr(0);
                const i32 c_chars = static_cast<i32>(m.arg(1));
                const i32 c_max = static_cast<i32>(m.arg(2));
                (void)m.arg(3); // psControl
                (void)m.arg(4); // psState
                const vitreo::GuestAddr items = m.arg_ptr(5);
                const vitreo::GuestAddr pc_items = m.arg_ptr(6);
                if (chars == 0 || c_chars <= 0 || items == 0 || c_max < 2) {
                    m.set_return(E_INVALIDARG);
                    return;
                }
                // SCRIPT_ITEM is 8 bytes on x86 (iCharPos + SCRIPT_ANALYSIS).
                // Emit one LTR run covering the whole string plus the required
                // terminal sentinel (Learn).
                if (m.write_u32(items + 0, 0).is_err() ||
                    m.write_u32(items + 4, 0).is_err() ||
                    m.write_u32(items + 8, static_cast<u32>(c_chars)).is_err() ||
                    m.write_u32(items + 12, 0).is_err()) {
                    m.set_return(E_OUTOFMEMORY);
                    return;
                }
                if (pc_items != 0) (void)m.write_u32(pc_items, 1);
                VITREO_TRACE("[usp10] ScriptItemize(chars=%d) -> S_OK items=1\n",
                             c_chars);
                m.set_return(0);
            }, 28});
        // HRESULT ScriptBreak(WCHAR*, int, SCRIPT_ANALYSIS*, SCRIPT_LOGATTR*)
        // — stdcall=16. Mark every character as soft-breakable (fCharStop).
        usp.entries.push_back({"ScriptBreak", 0,
            [](winapi::Marshaler& m) {
                constexpr u32 E_INVALIDARG = 0x80070057u;
                const vitreo::GuestAddr chars = m.arg_ptr(0);
                const i32 c_chars = static_cast<i32>(m.arg(1));
                (void)m.arg(2); // psa
                const vitreo::GuestAddr attrs = m.arg_ptr(3);
                if (chars == 0 || c_chars <= 0 || attrs == 0) {
                    m.set_return(E_INVALIDARG);
                    return;
                }
                for (i32 i = 0; i < c_chars; ++i) {
                    // SCRIPT_LOGATTR is one byte; fCharStop is bit 0.
                    if (m.write_u8(attrs + static_cast<u32>(i), 0x01).is_err()) {
                        m.set_return(E_INVALIDARG);
                        return;
                    }
                }
                m.set_return(0);
            }, 16});
        // HRESULT ScriptShape(HDC, SCRIPT_CACHE*, WCHAR*, int, int,
        //   SCRIPT_ANALYSIS*, WORD*, WORD*, SCRIPT_VISATTR*, int*)
        // — stdcall=40. For the simple Latin run emitted by ScriptItemize,
        // resolve real glyph IDs from the DC's selected font. Complex shaping
        // remains unsupported and returns E_NOTIMPL.
        usp.entries.push_back({"ScriptShape", 0,
            [&ctx](winapi::Marshaler& m) {
                constexpr u32 S_OK = 0;
                constexpr u32 E_PENDING = 0x8000000Au;
                constexpr u32 E_NOTIMPL = 0x80004001u;
                constexpr u32 E_INVALIDARG = 0x80070057u;
                constexpr u32 E_OUTOFMEMORY = 0x8007000Eu;
                const auto hdc = static_cast<winapi::Handle>(m.arg(0));
                const vitreo::GuestAddr cache = m.arg_ptr(1);
                const vitreo::GuestAddr chars = m.arg_ptr(2);
                const i32 c_chars = static_cast<i32>(m.arg(3));
                const i32 c_max_glyphs = static_cast<i32>(m.arg(4));
                const vitreo::GuestAddr analysis = m.arg_ptr(5);
                const vitreo::GuestAddr glyphs_out = m.arg_ptr(6);
                const vitreo::GuestAddr clusters_out = m.arg_ptr(7);
                const vitreo::GuestAddr attrs_out = m.arg_ptr(8);
                const vitreo::GuestAddr glyph_count_out = m.arg_ptr(9);
                if (cache == 0 || chars == 0 || c_chars < 0 ||
                    c_max_glyphs < 0 || analysis == 0 || glyphs_out == 0 ||
                    clusters_out == 0 || attrs_out == 0 ||
                    glyph_count_out == 0) {
                    m.set_return(E_INVALIDARG);
                    return;
                }
                if (c_chars == 0) {
                    (void)m.write_u32(glyph_count_out, 0);
                    m.set_return(S_OK);
                    return;
                }
                if (c_max_glyphs < c_chars) {
                    m.set_return(E_OUTOFMEMORY);
                    return;
                }

                const auto analysis_bits = m.read_u16(analysis);
                if (analysis_bits.is_err()) {
                    m.set_return(E_INVALIDARG);
                    return;
                }
                // SCRIPT_ANALYSIS bit 10 is fRTL. Contextual/RTL shaping is
                // not equivalent to one-codepoint/one-glyph mapping.
                if ((analysis_bits.value() & (1u << 10)) != 0) {
                    m.set_return(E_NOTIMPL);
                    return;
                }

                std::vector<u32> codepoints;
                codepoints.reserve(static_cast<usize>(c_chars));
                for (i32 index = 0; index < c_chars; ++index) {
                    const auto unit = m.read_u16(
                        chars + static_cast<u32>(index) * 2u);
                    if (unit.is_err()) {
                        m.set_return(E_INVALIDARG);
                        return;
                    }
                    // ASCII is context-free under Uniscribe. Reject surrogate,
                    // combining, and non-Latin runs until the backend exposes
                    // full shaping rather than silently returning wrong data.
                    if (unit.value() > 0x007Fu) {
                        m.set_return(E_NOTIMPL);
                        return;
                    }
                    codepoints.push_back(unit.value());
                }

                const auto cached = m.read_ptr(cache);
                if (hdc == 0 && (cached.is_err() || cached.value() == 0)) {
                    m.set_return(E_PENDING);
                    return;
                }
                auto font = ctx.gdi_mgr.resolve_dc_font(hdc);
                if (!font) {
                    m.set_return(hdc == 0 ? E_PENDING : E_NOTIMPL);
                    return;
                }
                auto glyphs = winapi::dwrite::glyph_indices(*font, codepoints);
                if (!glyphs || glyphs->size() != codepoints.size()) {
                    m.set_return(E_NOTIMPL);
                    return;
                }
                for (usize index = 0; index < glyphs->size(); ++index) {
                    if (m.write_u16(
                            glyphs_out + static_cast<u32>(index) * 2u,
                            (*glyphs)[index]).is_err() ||
                        m.write_u16(
                            clusters_out + static_cast<u32>(index) * 2u,
                            static_cast<u16>(index)).is_err() ||
                        // SCRIPT_VISATTR.fClusterStart is bit 4.
                        m.write_u16(
                            attrs_out + static_cast<u32>(index) * 2u,
                            0x0010u).is_err()) {
                        m.set_return(E_INVALIDARG);
                        return;
                    }
                }
                if (m.write_u32(
                        glyph_count_out,
                        static_cast<u32>(glyphs->size())).is_err() ||
                    m.write_ptr(cache, 1).is_err()) {
                    m.set_return(E_INVALIDARG);
                    return;
                }
                m.set_return(S_OK);
            }, 40});
        // HRESULT ScriptPlace(HDC, SCRIPT_CACHE*, const WORD* pwGlyphs,
        //   int cGlyphs, const SCRIPT_VISATTR*, SCRIPT_ANALYSIS*,
        //   int* piAdvance, GOFFSET* pGoffset, ABC* pABC) — stdcall=36.
        // Learn (ScriptPlace): advances come from the DC's font; with no DC
        // and nothing cached the answer is E_PENDING. The simple Latin path
        // positions every glyph at its own advance (no GPOS adjustment), so
        // each GOFFSET is zero; pABC totals the run: A of the first glyph,
        // C of the last, B the rest of the advance.
        usp.entries.push_back({"ScriptPlace", 0,
            [&ctx](winapi::Marshaler& m) {
                constexpr u32 S_OK = 0;
                constexpr u32 E_PENDING = 0x8000000Au;
                constexpr u32 E_NOTIMPL = 0x80004001u;
                constexpr u32 E_INVALIDARG = 0x80070057u;
                const auto hdc = static_cast<winapi::Handle>(m.arg(0));
                const vitreo::GuestAddr cache = m.arg_ptr(1);
                const vitreo::GuestAddr glyphs_in = m.arg_ptr(2);
                const i32 count = static_cast<i32>(m.arg32(3));
                (void)m.arg_ptr(4);  // psva: every Latin glyph is a cluster start
                const vitreo::GuestAddr analysis = m.arg_ptr(5);
                const vitreo::GuestAddr advances = m.arg_ptr(6);
                const vitreo::GuestAddr offsets = m.arg_ptr(7);
                const vitreo::GuestAddr abc = m.arg_ptr(8);
                if (cache == 0 || glyphs_in == 0 || count < 0 || analysis == 0 ||
                    advances == 0 || offsets == 0) {
                    m.set_return(E_INVALIDARG);
                    return;
                }
                if (hdc == 0) {
                    // The cache ScriptShape leaves holds no font, so metrics
                    // need the DC (callers retry with it on E_PENDING).
                    m.set_return(E_PENDING);
                    return;
                }
                auto font = ctx.gdi_mgr.resolve_dc_font(hdc);
                if (!font || font->metrics.design_units_per_em == 0) {
                    m.set_return(E_NOTIMPL);
                    return;
                }
                std::vector<u16> glyphs(static_cast<usize>(count));
                for (i32 i = 0; i < count; ++i) {
                    const auto g = m.read_u16(glyphs_in + static_cast<u32>(i) * 2u);
                    if (g.is_err()) {
                        m.set_return(E_INVALIDARG);
                        return;
                    }
                    glyphs[static_cast<usize>(i)] = g.value();
                }
                auto metrics = winapi::dwrite::glyph_metrics(*font, glyphs);
                if (!metrics || metrics->size() != glyphs.size()) {
                    m.set_return(E_NOTIMPL);
                    return;
                }
                const double scale = static_cast<double>(ctx.gdi_mgr.dc_font_em_pixels(hdc)) /
                                     static_cast<double>(font->metrics.design_units_per_em);
                i32 total = 0;
                for (i32 i = 0; i < count; ++i) {
                    const auto& g = (*metrics)[static_cast<usize>(i)];
                    const i32 advance = static_cast<i32>(std::lround(g.advance_width * scale));
                    total += advance;
                    if (m.write_u32(advances + static_cast<u32>(i) * 4u, static_cast<u32>(advance)).is_err() ||
                        m.write_u32(offsets + static_cast<u32>(i) * 8u, 0).is_err() ||
                        m.write_u32(offsets + static_cast<u32>(i) * 8u + 4u, 0).is_err()) {
                        m.set_return(E_INVALIDARG);
                        return;
                    }
                }
                if (abc != 0) {
                    const i32 a = count > 0 ? static_cast<i32>(std::lround((*metrics)[0].left_side_bearing * scale)) : 0;
                    const i32 c = count > 0 ? static_cast<i32>(std::lround(metrics->back().right_side_bearing * scale)) : 0;
                    (void)m.write_u32(abc + 0, static_cast<u32>(a));
                    (void)m.write_u32(abc + 4, static_cast<u32>(std::max(0, total - a - c)));
                    (void)m.write_u32(abc + 8, static_cast<u32>(c));
                }
                m.set_return(S_OK);
            }, 36});
        // HRESULT ScriptFreeCache(SCRIPT_CACHE*) — stdcall=4. Windows always
        // nulls the caller's cache slot, even though Vitreo's simple Latin
        // path does not allocate host cache state.
        usp.entries.push_back({"ScriptFreeCache", 0,
            [](winapi::Marshaler& m) {
                constexpr u32 E_INVALIDARG = 0x80070057u;
                const vitreo::GuestAddr cache = m.arg_ptr(0);
                if (cache == 0 || m.write_ptr(cache, 0).is_err()) {
                    m.set_return(E_INVALIDARG);
                    return;
                }
                m.set_return(0);
            }, 4});
        (void)reg.register_dll("usp10.dll", std::move(usp));
    }

    // -- vulkan-1.dll --
    // On Windows this ships with the GPU driver rather than the application,
    // so it is absent from every capsule and has to come from Vitreo. Loaders
    // like volk resolve everything through vkGetInstanceProcAddr, which needs
    // the registry to hand back guest-callable thunk addresses.
    {
        ctx.vulkan_mgr.set_thunk_registry(&ctx.thunk_registry);
        auto vulkan_exports = winapi::create_vulkan_thunks(ctx.vulkan_mgr);
        (void)reg.register_dll("vulkan-1.dll", std::move(vulkan_exports));
    }
}

} // namespace vitreo::cli
