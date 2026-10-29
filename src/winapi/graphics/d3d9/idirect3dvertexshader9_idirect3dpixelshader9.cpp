// SPDX-License-Identifier: LGPL-2.1-only
/// @file d3d9_resources.cpp
/// @brief IDirect3DSwapChain9, IDirect3DSurface9, IDirect3DTexture9,
///        IDirect3DVertexBuffer9, and IDirect3DIndexBuffer9.
///
/// Locking is the contract that matters most here: D3D9 hands the application
/// a raw pointer into resource memory. Vitreo maps guest-visible memory for
/// the resource's whole lifetime and pushes the locked range into the shared
/// backend on unlock, so pixels and vertices the guest writes are the ones the
/// GPU receives.

#include "winapi/graphics/d3d9/d3d9_state.hpp"

#include "core/memory/guest_memory.hpp"
#include "winapi/core/host_gui_pump.hpp"
#include "winapi/core/marshaler.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

/// @file idirect3dvertexshader9_idirect3dpixelshader9.cpp
/// @brief IDirect3DVertexShader9 / IDirect3DPixelShader9
///
/// Closed cut from d3d9_resources.cpp — verified with plan_extraction.py.

namespace vitreo::winapi {

// =============================================================================
// IDirect3DVertexShader9 / IDirect3DPixelShader9
// =============================================================================

void D3D9Manager::create_shader(Marshaler& m, u32 device, bool pixel) {
    const GuestAddr function = m.arg_ptr(1);
    const GuestAddr out = m.arg_ptr(2);
    if (out != 0) (void)m.write_u32(out, 0);
    if (function == 0 || out == 0) {
        m.set_return(D3DERR_INVALIDCALL);
        return;
    }

    // The token stream has no length prefix: it runs until the END token.
    constexpr u32 END_TOKEN = 0x0000FFFF;
    constexpr u32 MAX_TOKENS = 65536;
    std::vector<u32> bytecode;
    bytecode.reserve(64);
    for (u32 index = 0; index < MAX_TOKENS; ++index) {
        auto token = m.read_u32(function + index * sizeof(u32));
        if (token.is_err()) {
            VITREO_TRACE("[d3d9] Create%sShader -> D3DERR_INVALIDCALL "
                         "(unreadable token at index %u)\n",
                         pixel ? "Pixel" : "Vertex", index);
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        bytecode.push_back(token.value());
        if (token.value() == END_TOKEN) break;
    }
    if (bytecode.empty() || bytecode.back() != END_TOKEN) {
        VITREO_TRACE("[d3d9] Create%sShader -> D3DERR_INVALIDCALL "
                     "(no END token within %u tokens)\n",
                     pixel ? "Pixel" : "Vertex", MAX_TOKENS);
        m.set_return(D3DERR_INVALIDCALL);
        return;
    }

    graphics::Sm3Shader translated =
        graphics::translate_sm3_to_msl(bytecode.data(), bytecode.size());
    const bool expected_pixel = translated.stage == graphics::ShaderStage::Fragment;
    if (translated.ok && expected_pixel != pixel) {
        // Windows rejects a pixel shader passed to CreateVertexShader.
        VITREO_TRACE("[d3d9] Create%sShader -> D3DERR_INVALIDCALL "
                     "(the bytecode is a %s shader)\n",
                     pixel ? "Pixel" : "Vertex",
                     expected_pixel ? "pixel" : "vertex");
        m.set_return(D3DERR_INVALIDCALL);
        return;
    }
    if (!translated.ok) {
        // The shader is well-formed D3D9 bytecode Vitreo cannot express.
        // Returning a shader object that renders something else would be
        // worse than refusing it.
        VITREO_TRACE("[d3d9] Create%sShader -> E_NOTIMPL "
                     "(no translation for %s)\n",
                     pixel ? "Pixel" : "Vertex", translated.failure.c_str());
        if (translated.failure.find("source register type") !=
                std::string::npos ||
            translated.failure.find("destination register type") !=
                std::string::npos ||
            translated.failure.find("source modifier") !=
                std::string::npos ||
            translated.failure.find("predicated") != std::string::npos ||
            translated.failure.find("opcode") != std::string::npos) {
            std::string hex;
            hex.reserve(bytecode.size() * 9);
            for (usize i = 0; i < bytecode.size() && i < 64; ++i) {
                char buf[16];
                std::snprintf(buf, sizeof(buf), "%08x ", bytecode[i]);
                hex += buf;
            }
            VITREO_TRACE("[d3d9] failing shader tokens (%zu): %s\n",
                         bytecode.size(), hex.c_str());
        }
        m.set_return(E_NOTIMPL_D3D);
        return;
    }

    const u32 object = alloc_com_object(
        pixel ? D3D9InterfaceId::PixelShader : D3D9InterfaceId::VertexShader,
        device);
    if (object == 0) {
        m.set_return(E_OUTOFMEMORY_D3D);
        return;
    }
    ComObject* entry = find_any(object);
    entry->shader = std::make_unique<ShaderState>();
    entry->shader->pixel = pixel;
    entry->shader->bytecode = std::move(bytecode);
    entry->shader->translated = std::move(translated);

    // Dump the translated MSL so the generated shader can be inspected
    // against the source bytecode; the on-screen result cannot distinguish
    // a mistranslated transform from bad vertex data.
    if (const char* dir = std::getenv("VITREO_D3D9_SHADER_DUMP")) {
        char path[512];
        std::snprintf(path, sizeof(path), "%s/%s_%08x_sm%u_%u.msl", dir,
                      pixel ? "ps" : "vs", object,
                      entry->shader->translated.major,
                      entry->shader->translated.minor);
        if (std::FILE* f = std::fopen(path, "w")) {
            std::fprintf(f, "// tokens=%zu constants=%u ok=%u\n",
                         entry->shader->bytecode.size(),
                         entry->shader->translated.float_constant_count,
                         entry->shader->translated.ok ? 1u : 0u);
            for (usize i = 0; i < entry->shader->bytecode.size(); ++i) {
                std::fprintf(f, "// [%03zu] %08x\n", i,
                             entry->shader->bytecode[i]);
            }
            std::fputs(entry->shader->translated.msl.c_str(), f);
            std::fclose(f);
        }
    }

    if (m.write_u32(out, object).is_err()) {
        destroy_object(object);
        m.set_return(E_POINTER_D3D);
        return;
    }
    VITREO_TRACE("[d3d9] Create%sShader -> shader=0x%08x sm%u.%u "
                 "constants=%u\n",
                 pixel ? "Pixel" : "Vertex", object,
                 entry->shader->translated.major,
                 entry->shader->translated.minor,
                 entry->shader->translated.float_constant_count);
    m.set_return(D3D_OK);
}

void D3D9Manager::dispatch_shader(Marshaler& m, usize slot, u32 self,
                                  bool pixel) {
    const D3D9InterfaceId iface = pixel ? D3D9InterfaceId::PixelShader
                                        : D3D9InterfaceId::VertexShader;
    if (dispatch_unknown(m, slot, self, iface)) return;

    ComObject* entry = find(self, iface);
    if (entry == nullptr || !entry->shader) {
        m.set_return(D3DERR_INVALIDCALL);
        return;
    }

    switch (slot) {
    case 3: { // GetDevice
        const GuestAddr out = m.arg_ptr(1);
        if (out == 0 || entry->device == 0) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        (void)add_ref(entry->device);
        if (m.write_u32(out, entry->device).is_err()) {
            (void)release(entry->device);
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        m.set_return(D3D_OK);
        return;
    }
    case 4: { // GetFunction
        const GuestAddr data = m.arg_ptr(1);
        const GuestAddr size_out = m.arg_ptr(2);
        const u32 byte_size = static_cast<u32>(
            entry->shader->bytecode.size() * sizeof(u32));
        if (size_out == 0) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        // A null data pointer is the documented size query.
        if (data == 0) {
            if (m.write_u32(size_out, byte_size).is_err()) {
                m.set_return(D3DERR_INVALIDCALL);
                return;
            }
            m.set_return(D3D_OK);
            return;
        }
        auto available = m.read_u32(size_out);
        if (available.is_err() || available.value() < byte_size) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        if (m.memory().write_bytes(
                data,
                reinterpret_cast<const Byte*>(entry->shader->bytecode.data()),
                byte_size).is_err() ||
            m.write_u32(size_out, byte_size).is_err()) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        m.set_return(D3D_OK);
        return;
    }
    default:
        m.set_return(E_NOTIMPL_D3D);
        return;
    }
}

} // namespace vitreo::winapi
