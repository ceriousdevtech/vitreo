// SPDX-License-Identifier: LGPL-2.1-only
/// @file d3d8_shaders.cpp
/// @brief Handle-based D3D8 vertex and pixel shaders.
///
/// Learn (Vertex Declaration Changes): in D3D8 the vertex declaration is part
/// of the vertex shader, and SetVertexShader takes either an FVF code (fixed
/// function) or a shader handle; D3D9 split the declaration into its own
/// object and the FVF into SetFVF. A D3D8 declaration (d3d8types.h
/// D3DVSD_* tokens) binds stream data to vertex registers by number. The
/// register numbers have fixed meanings for fixed-function processing
/// (D3DVSDE_POSITION = v0, ... D3DVSDE_TEXCOORD7 = v14, POSITION2,
/// NORMAL2), so each register becomes the D3D9 usage of its D3DVSDE name,
/// and a vs.1.x function, which in D3D8 carries no dcl instructions, gets
/// one `dcl_<usage> v#` per declared register, which is how D3D9 vs_1_1
/// binds the same register to the same element.

#include "winapi/graphics/d3d8/d3d8_internal.hpp"

#include "core/memory/guest_memory.hpp"

#include <vector>

namespace vitreo::winapi {

using namespace d3d8c;

namespace {

bool failed(u32 hr) { return (hr & 0x80000000u) != 0; }

constexpr D3D9InterfaceId kDev = D3D9InterfaceId::Device;

/// Usage and usage index of the element a D3DVSDE_* register carries.
bool register_usage(u32 reg, u32& usage, u32& index) {
    using namespace d3d9h;
    index = 0;
    if (reg == D3DVSDE_POSITION) usage = D3DDECLUSAGE_POSITION;
    else if (reg == D3DVSDE_BLENDWEIGHT) usage = D3DDECLUSAGE_BLENDWEIGHT;
    else if (reg == D3DVSDE_BLENDINDICES) usage = D3DDECLUSAGE_BLENDINDICES;
    else if (reg == D3DVSDE_NORMAL) usage = D3DDECLUSAGE_NORMAL;
    else if (reg == D3DVSDE_PSIZE) usage = D3DDECLUSAGE_PSIZE;
    else if (reg == D3DVSDE_DIFFUSE) usage = D3DDECLUSAGE_COLOR;
    else if (reg == D3DVSDE_SPECULAR) {
        usage = D3DDECLUSAGE_COLOR;
        index = 1;
    } else if (reg >= D3DVSDE_TEXCOORD0 && reg <= D3DVSDE_TEXCOORD7) {
        usage = D3DDECLUSAGE_TEXCOORD;
        index = reg - D3DVSDE_TEXCOORD0;
    } else if (reg == D3DVSDE_POSITION2) {
        usage = D3DDECLUSAGE_POSITION;
        index = 1;
    } else if (reg == D3DVSDE_NORMAL2) {
        usage = D3DDECLUSAGE_NORMAL;
        index = 1;
    } else {
        return false;
    }
    return true;
}

/// Bytes of one D3DVSDT_* element; 0 for an unknown type. D3DVSDT_* and
/// D3DDECLTYPE_* share the values 0..7 (d3d8_contract.inc).
u32 element_size(u32 type) {
    static_assert(D3DVSDT_FLOAT1 == d3d9h::D3DDECLTYPE_FLOAT1 &&
                  D3DVSDT_FLOAT4 == d3d9h::D3DDECLTYPE_FLOAT4 &&
                  D3DVSDT_D3DCOLOR == d3d9h::D3DDECLTYPE_D3DCOLOR &&
                  D3DVSDT_UBYTE4 == d3d9h::D3DDECLTYPE_UBYTE4 &&
                  D3DVSDT_SHORT2 == d3d9h::D3DDECLTYPE_SHORT2 &&
                  D3DVSDT_SHORT4 == d3d9h::D3DDECLTYPE_SHORT4);
    switch (type) {
    case D3DVSDT_FLOAT1: return 4;
    case D3DVSDT_FLOAT2: return 8;
    case D3DVSDT_FLOAT3: return 12;
    case D3DVSDT_FLOAT4: return 16;
    case D3DVSDT_D3DCOLOR: return 4;
    case D3DVSDT_UBYTE4: return 4;
    case D3DVSDT_SHORT2: return 4;
    case D3DVSDT_SHORT4: return 8;
    default: return 0;
    }
}

struct Element {
    u32 stream;
    u32 offset;
    u32 type;
    u32 reg;
};

/// Read a shader token stream through its D3DSIO_END token. SM 1.x carries
/// no instruction lengths: parameter tokens are the ones with bit 31 set,
/// comments carry their own length, and `def` is followed by its register
/// and four raw float tokens.
bool read_function(GuestMemory& memory, GuestAddr function, std::vector<u32>& out) {
    constexpr usize kMaxTokens = 1u << 18;
    out.clear();
    const auto token_at = [&](usize index, u32& value) {
        const auto read = memory.read_u32(function + 4u * index);
        if (read.is_err()) return false;
        value = read.value();
        return true;
    };
    u32 token = 0;
    if (!token_at(0, token)) return false;
    out.push_back(token);
    usize index = 1;
    while (index < kMaxTokens) {
        if (!token_at(index, token)) return false;
        out.push_back(token);
        ++index;
        const u32 opcode = token & D3DSI_OPCODE_MASK;
        if (opcode == D3DSIO_END) return true;
        if (opcode == D3DSIO_COMMENT) {
            const u32 length = (token & D3DSI_COMMENTSIZE_MASK) >> D3DSI_COMMENTSIZE_SHIFT;
            for (u32 i = 0; i < length; ++i, ++index) {
                if (!token_at(index, token)) return false;
                out.push_back(token);
            }
            continue;
        }
        if (opcode == D3DSIO_DEF) {
            for (u32 i = 0; i < 5; ++i, ++index) {
                if (!token_at(index, token)) return false;
                out.push_back(token);
            }
            continue;
        }
        for (;;) {
            u32 next = 0;
            if (!token_at(index, next)) return false;
            if ((next & 0x80000000u) == 0) break;
            out.push_back(next);
            ++index;
        }
    }
    return false;
}

}  // namespace

// =============================================================================
// Vertex shaders
// =============================================================================

u32 D3D8Manager::create_vertex_shader(Object& dev, GuestAddr declaration,
                                      GuestAddr function, GuestAddr out_handle,
                                      u32 usage) {
    DeviceState& ds = *dev.device;
    if (declaration == 0 || out_handle == 0) return D3DERR_INVALIDCALL;
    VertexShader shader;
    shader.usage = usage;
    std::vector<Element> elements;
    u32 stream = 0;
    u32 offset = 0;
    bool have_stream = false;
    constexpr usize kMaxDeclarationTokens = 4096;
    for (usize i = 0;; ++i) {
        if (i >= kMaxDeclarationTokens) return D3DERR_INVALIDCALL;
        const u32 token = rd(declaration + 4u * i);
        shader.declaration.push_back(token);
        if (token == 0xFFFFFFFFu) break;  // D3DVSD_END()
        const u32 kind = (token & D3DVSD_TOKENTYPEMASK) >> D3DVSD_TOKENTYPESHIFT;
        if (kind == D3DVSD_TOKEN_NOP) {
            if (token != 0) return D3DERR_INVALIDCALL;
        } else if (kind == D3DVSD_TOKEN_STREAM) {
            if ((token & D3DVSD_STREAMTESSMASK) != 0) {
                d3d8_log_unimplemented_once("IDirect3DDevice8::CreateVertexShader",
                                            "tessellator stream (D3DVSD_STREAM_TESS)");
                return D3DERR_INVALIDCALL;
            }
            stream = token & D3DVSD_STREAMNUMBERMASK;
            offset = 0;
            have_stream = true;
        } else if (kind == D3DVSD_TOKEN_STREAMDATA) {
            if (!have_stream) return D3DERR_INVALIDCALL;
            if ((token & D3DVSD_DATALOADTYPEMASK) != 0) {
                // D3DVSD_SKIP(count): count DWORDs of the stream are unused.
                offset += 4u * ((token & D3DVSD_SKIPCOUNTMASK) >> D3DVSD_SKIPCOUNTSHIFT);
                continue;
            }
            const u32 reg = token & D3DVSD_VERTEXREGMASK;
            const u32 type = (token & D3DVSD_DATATYPEMASK) >> D3DVSD_DATATYPESHIFT;
            u32 element_usage = 0;
            u32 element_index = 0;
            if (element_size(type) == 0 ||
                !register_usage(reg, element_usage, element_index)) {
                return D3DERR_INVALIDCALL;
            }
            for (const Element& existing : elements) {
                if (existing.reg == reg) return D3DERR_INVALIDCALL;
            }
            elements.push_back({stream, offset, type, reg});
            offset += element_size(type);
        } else if (kind == D3DVSD_TOKEN_TESSELLATOR) {
            d3d8_log_unimplemented_once("IDirect3DDevice8::CreateVertexShader",
                                        "tessellator declaration token");
            return D3DERR_INVALIDCALL;
        } else if (kind == D3DVSD_TOKEN_CONSTMEM) {
            const u32 count = (token & D3DVSD_CONSTCOUNTMASK) >> D3DVSD_CONSTCOUNTSHIFT;
            const u32 address = token & D3DVSD_CONSTADDRESSMASK;
            for (u32 c = 0; c < count; ++c) {
                std::array<u32, 4> value{};
                for (u32 k = 0; k < 4; ++k) {
                    ++i;
                    value[k] = rd(declaration + 4u * i);
                    shader.declaration.push_back(value[k]);
                }
                shader.constants.push_back({address + c, value});
            }
        } else if (kind == D3DVSD_TOKEN_EXT) {
            // Extension info: skipped, as the runtime does.
            const u32 count = (token & D3DVSD_EXTCOUNTMASK) >> D3DVSD_EXTCOUNTSHIFT;
            for (u32 c = 0; c < count; ++c) {
                ++i;
                shader.declaration.push_back(rd(declaration + 4u * i));
            }
        } else {
            return D3DERR_INVALIDCALL;
        }
    }

    // D3DVERTEXELEMENT9: WORD Stream, WORD Offset, BYTE Type, Method, Usage,
    // UsageIndex; terminated by D3DDECL_END {0xFF, 0, UNUSED, 0, 0, 0}.
    if (elements.size() + 1 > d3d9h::MAXD3DDECLLENGTH + 1) return D3DERR_INVALIDCALL;
    const GuestAddr decl9 = scratch(8u * (elements.size() + 1));
    if (decl9 == 0) return d3d8::E_OUTOFMEMORY;
    for (usize e = 0; e < elements.size(); ++e) {
        u32 element_usage = 0;
        u32 element_index = 0;
        (void)register_usage(elements[e].reg, element_usage, element_index);
        wr(decl9 + 8u * e, elements[e].stream | (elements[e].offset << 16));
        wr(decl9 + 8u * e + 4, elements[e].type | (d3d9h::D3DDECLMETHOD_DEFAULT << 8) |
                                   (element_usage << 16) | (element_index << 24));
    }
    wr(decl9 + 8u * elements.size(), 0x00FFu);
    wr(decl9 + 8u * elements.size() + 4, d3d9h::D3DDECLTYPE_UNUSED);

    if (function != 0) {
        std::vector<u32> tokens;
        if (!read_function(*m_memory, function, tokens)) return D3DERR_INVALIDCALL;
        const u32 version = tokens[0];
        if (version != D3DVS_VERSION_1_0 && version != D3DVS_VERSION_1_1) {
            return D3DERR_INVALIDCALL;
        }
        shader.function = tokens;
        // vs.1.0 is a subset of vs.1.1 (1.1 added the address register).
        std::vector<u32> rewritten{D3DVS_VERSION_1_1};
        for (const Element& element : elements) {
            u32 element_usage = 0;
            u32 element_index = 0;
            (void)register_usage(element.reg, element_usage, element_index);
            if (element.reg > 15) continue;  // POSITION2/NORMAL2: fixed function only.
            rewritten.push_back(d3d9h::D3DSIO_DCL);
            rewritten.push_back(0x80000000u | (element_usage << d3d9h::D3DSP_DCL_USAGE_SHIFT) |
                                (element_index << d3d9h::D3DSP_DCL_USAGEINDEX_SHIFT));
            rewritten.push_back(0x80000000u |
                                (d3d9h::D3DSPR_INPUT << d3d9h::D3DSP_REGTYPE_SHIFT) |
                                d3d9h::D3DSP_WRITEMASK_ALL | element.reg);
        }
        rewritten.insert(rewritten.end(), tokens.begin() + 1, tokens.end());
        const GuestAddr code = scratch(4u * rewritten.size());
        if (code == 0) return d3d8::E_OUTOFMEMORY;
        for (usize t = 0; t < rewritten.size(); ++t) wr(code + 4u * t, rewritten[t]);
        const GuestAddr out9 = scratch(4);
        const u32 hr = call9(kDev, D3D8_SLOT9(Device, CreateVertexShader),
                             {dev.d3d9, static_cast<u32>(code), static_cast<u32>(out9)});
        if (failed(hr)) return hr;
        shader.shader9 = rd(out9);
    }
    const GuestAddr out_decl = scratch(4);
    const u32 hr = call9(kDev, D3D8_SLOT9(Device, CreateVertexDeclaration),
                         {dev.d3d9, static_cast<u32>(decl9), static_cast<u32>(out_decl)});
    if (failed(hr)) {
        if (shader.shader9 != 0) {
            (void)call9(D3D9InterfaceId::VertexShader, 2, {shader.shader9});
        }
        return hr;
    }
    shader.declaration9 = rd(out_decl);
    // Shader handles have bit 0 set, which no FVF code has (D3DFVF_RESERVED0
    // must be 0), so SetVertexShader can tell the two apart.
    const u32 handle = (++ds.next_vertex_shader << 1) | 1u;
    ds.vertex_shaders[handle] = std::move(shader);
    wr(out_handle, handle);
    return D3D_OK;
}

u32 D3D8Manager::set_vertex_shader(Object& dev, u32 handle) {
    DeviceState& ds = *dev.device;
    if ((handle & D3DFVF_RESERVED0) == 0) {
        // An FVF code: fixed-function processing of that vertex format.
        const u32 hr = call9(kDev, D3D8_SLOT9(Device, SetVertexShader), {dev.d3d9, 0u});
        if (failed(hr)) return hr;
        return call9(kDev, D3D8_SLOT9(Device, SetFVF), {dev.d3d9, handle});
    }
    const auto found = ds.vertex_shaders.find(handle);
    if (found == ds.vertex_shaders.end()) return D3DERR_INVALIDCALL;
    const VertexShader& shader = found->second;
    u32 hr = call9(kDev, D3D8_SLOT9(Device, SetVertexDeclaration),
                   {dev.d3d9, shader.declaration9});
    if (failed(hr)) return hr;
    hr = call9(kDev, D3D8_SLOT9(Device, SetVertexShader), {dev.d3d9, shader.shader9});
    if (failed(hr)) return hr;
    if (!shader.constants.empty()) {
        const GuestAddr value = scratch(16);
        for (const auto& [reg, data] : shader.constants) {
            for (u32 k = 0; k < 4; ++k) wr(value + 4u * k, data[k]);
            hr = call9(kDev, D3D8_SLOT9(Device, SetVertexShaderConstantF),
                       {dev.d3d9, reg, static_cast<u32>(value), 1u});
            if (failed(hr)) return hr;
        }
    }
    return D3D_OK;
}

u32 D3D8Manager::get_vertex_shader(Object& dev, GuestAddr out_handle) {
    DeviceState& ds = *dev.device;
    if (out_handle == 0) return D3DERR_INVALIDCALL;
    // The current handle is whatever the D3D9 state says, so that state
    // blocks and Reset, which change that state, are reflected.
    const GuestAddr out9 = scratch(4);
    u32 hr = call9(kDev, D3D8_SLOT9(Device, GetVertexDeclaration),
                   {dev.d3d9, static_cast<u32>(out9)});
    if (failed(hr)) return hr;
    const u32 declaration9 = rd(out9);
    if (declaration9 != 0) {
        (void)call9(D3D9InterfaceId::VertexDeclaration, 2, {declaration9});
        for (const auto& [handle, shader] : ds.vertex_shaders) {
            if (shader.declaration9 == declaration9) {
                wr(out_handle, handle);
                return D3D_OK;
            }
        }
    }
    hr = call9(kDev, D3D8_SLOT9(Device, GetFVF), {dev.d3d9, static_cast<u32>(out9)});
    if (failed(hr)) return hr;
    wr(out_handle, rd(out9));
    return D3D_OK;
}

u32 D3D8Manager::delete_vertex_shader(Object& dev, u32 handle) {
    DeviceState& ds = *dev.device;
    // Measured: deleting an FVF code succeeds and does nothing.
    if ((handle & D3DFVF_RESERVED0) == 0) return D3D_OK;
    const auto found = ds.vertex_shaders.find(handle);
    if (found == ds.vertex_shaders.end()) return D3DERR_INVALIDCALL;
    if (found->second.shader9 != 0) {
        (void)call9(D3D9InterfaceId::VertexShader, 2, {found->second.shader9});
    }
    (void)call9(D3D9InterfaceId::VertexDeclaration, 2, {found->second.declaration9});
    ds.vertex_shaders.erase(found);
    return D3D_OK;
}

u32 D3D8Manager::get_vertex_shader_blob(Object& dev, u32 handle, bool declaration,
                                        GuestAddr data, GuestAddr size) {
    DeviceState& ds = *dev.device;
    if (size == 0) return D3DERR_INVALIDCALL;
    const auto found = ds.vertex_shaders.find(handle);
    if (found == ds.vertex_shaders.end()) {
        // Measured (vs.fvf.GetDeclaration.size): an FVF code reports size 0.
        if ((handle & D3DFVF_RESERVED0) == 0) wr(size, 0);
        return D3DERR_INVALIDCALL;
    }
    const std::vector<u32>& tokens =
        declaration ? found->second.declaration : found->second.function;
    const u32 bytes = static_cast<u32>(4u * tokens.size());
    if (data == 0) {
        wr(size, bytes);
        return D3D_OK;
    }
    if (rd(size) < bytes) {
        // Measured: D3DERR_INVALIDCALL, *pSizeOfData left as it was.
        return D3DERR_INVALIDCALL;
    }
    for (usize t = 0; t < tokens.size(); ++t) wr(data + 4u * t, tokens[t]);
    wr(size, bytes);
    return D3D_OK;
}

// =============================================================================
// Pixel shaders
// =============================================================================

u32 D3D8Manager::create_pixel_shader(Object& dev, GuestAddr function,
                                     GuestAddr out_handle) {
    DeviceState& ds = *dev.device;
    if (function == 0 || out_handle == 0) return D3DERR_INVALIDCALL;
    PixelShader shader;
    if (!read_function(*m_memory, function, shader.function)) return D3DERR_INVALIDCALL;
    const u32 version = shader.function[0];
    if (version < D3DPS_VERSION_1_0 || version > D3DPS_VERSION_1_4) {
        return D3DERR_INVALIDCALL;
    }
    // The ps.1.x token stream is the one D3D9 ps_1_x accepts; ps.1.0 is
    // the subset of ps.1.1 D3D9 no longer names.
    std::vector<u32> tokens = shader.function;
    if (tokens[0] == D3DPS_VERSION_1_0) tokens[0] = D3DPS_VERSION_1_1;
    const GuestAddr code = scratch(4u * tokens.size());
    if (code == 0) return d3d8::E_OUTOFMEMORY;
    for (usize t = 0; t < tokens.size(); ++t) wr(code + 4u * t, tokens[t]);
    const GuestAddr out9 = scratch(4);
    const u32 hr = call9(kDev, D3D8_SLOT9(Device, CreatePixelShader),
                         {dev.d3d9, static_cast<u32>(code), static_cast<u32>(out9)});
    if (failed(hr)) return hr;
    shader.shader9 = rd(out9);
    const u32 handle = ++ds.next_pixel_shader;
    ds.pixel_shaders[handle] = std::move(shader);
    wr(out_handle, handle);
    return D3D_OK;
}

u32 D3D8Manager::set_pixel_shader(Object& dev, u32 handle) {
    DeviceState& ds = *dev.device;
    u32 shader9 = 0;
    if (handle != 0) {
        const auto found = ds.pixel_shaders.find(handle);
        if (found == ds.pixel_shaders.end()) return D3DERR_INVALIDCALL;
        shader9 = found->second.shader9;
    }
    return call9(kDev, D3D8_SLOT9(Device, SetPixelShader), {dev.d3d9, shader9});
}

u32 D3D8Manager::get_pixel_shader(Object& dev, GuestAddr out_handle) {
    DeviceState& ds = *dev.device;
    if (out_handle == 0) return D3DERR_INVALIDCALL;
    const GuestAddr out9 = scratch(4);
    const u32 hr = call9(kDev, D3D8_SLOT9(Device, GetPixelShader),
                         {dev.d3d9, static_cast<u32>(out9)});
    if (failed(hr)) return hr;
    const u32 shader9 = rd(out9);
    u32 handle = 0;
    if (shader9 != 0) {
        (void)call9(D3D9InterfaceId::PixelShader, 2, {shader9});
        for (const auto& [h, shader] : ds.pixel_shaders) {
            if (shader.shader9 == shader9) handle = h;
        }
    }
    wr(out_handle, handle);
    return D3D_OK;
}

u32 D3D8Manager::delete_pixel_shader(Object& dev, u32 handle) {
    DeviceState& ds = *dev.device;
    const auto found = ds.pixel_shaders.find(handle);
    if (found == ds.pixel_shaders.end()) return D3DERR_INVALIDCALL;
    (void)call9(D3D9InterfaceId::PixelShader, 2, {found->second.shader9});
    ds.pixel_shaders.erase(found);
    return D3D_OK;
}

u32 D3D8Manager::get_pixel_shader_function(Object& dev, u32 handle, GuestAddr data,
                                           GuestAddr size) {
    DeviceState& ds = *dev.device;
    const auto found = ds.pixel_shaders.find(handle);
    if (found == ds.pixel_shaders.end() || size == 0) return D3DERR_INVALIDCALL;
    const std::vector<u32>& tokens = found->second.function;
    const u32 bytes = static_cast<u32>(4u * tokens.size());
    if (data == 0) {
        wr(size, bytes);
        return D3D_OK;
    }
    if (rd(size) < bytes) {
        // Measured: D3DERR_INVALIDCALL, *pSizeOfData left as it was.
        return D3DERR_INVALIDCALL;
    }
    for (usize t = 0; t < tokens.size(); ++t) wr(data + 4u * t, tokens[t]);
    wr(size, bytes);
    return D3D_OK;
}

void D3D8Manager::release_device_handles(DeviceState& state) {
    for (auto& [handle, shader] : state.vertex_shaders) {
        if (shader.shader9 != 0) (void)call9(D3D9InterfaceId::VertexShader, 2, {shader.shader9});
        if (shader.declaration9 != 0) {
            (void)call9(D3D9InterfaceId::VertexDeclaration, 2, {shader.declaration9});
        }
    }
    for (auto& [handle, shader] : state.pixel_shaders) {
        if (shader.shader9 != 0) (void)call9(D3D9InterfaceId::PixelShader, 2, {shader.shader9});
    }
    for (auto& [token, block] : state.state_blocks) {
        if (block.block9 != 0) (void)call9(D3D9InterfaceId::StateBlock, 2, {block.block9});
    }
    state.vertex_shaders.clear();
    state.pixel_shaders.clear();
    state.state_blocks.clear();
}

}  // namespace vitreo::winapi
