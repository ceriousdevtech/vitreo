// SPDX-License-Identifier: LGPL-2.1-only

/// @file ff_vertex_layout.cpp
/// @brief Flexible Vertex Format → backend vertex layout.

#include "graphics/shaders/ff_vertex_layout.hpp"
#include "graphics/shaders/ff_emulation.hpp"
#include "graphics/shaders/ff_uniforms.hpp"  // FF_MAX_TEXTURE_STAGES

namespace vitreo::graphics {

namespace {

/// D3DFVF_TEXCOORDSIZE{1,2,3,4}(i) pack two bits per coordinate set starting
/// at bit 16. The encoding is deliberately not 1..4: MSDN defines
/// SIZE1 == 3, SIZE2 == 0, SIZE3 == 1, SIZE4 == 2, so a set that does not
/// name a size defaults to the two-component case.
constexpr u32 kTexCoordSizeShift = 16;

} // namespace

u32 ff_fvf_texcoord_components(u32 fvf, u32 index) {
    if (index >= FF_MAX_TEXTURE_STAGES) return 0;
    const u32 bits = (fvf >> (kTexCoordSizeShift + index * 2)) & 0x3u;
    switch (bits) {
        case 0: return 2;  // D3DFVF_TEXCOORDSIZE2
        case 1: return 3;  // D3DFVF_TEXCOORDSIZE3
        case 2: return 4;  // D3DFVF_TEXCOORDSIZE4
        default: return 1; // D3DFVF_TEXCOORDSIZE1
    }
}

bool ff_enumerate_fvf_elements(u32 fvf, std::vector<FFVertexElement>& elements,
                               u32& stride, const char*& rejection) {
    elements.clear();
    stride = 0;
    rejection = nullptr;

    u32 offset = 0;
    const auto add = [&](u32 usage, u32 usage_index, BackendVertexFormat format,
                         u32 size) {
        FFVertexElement element;
        element.usage = usage;
        element.usage_index = usage_index;
        element.offset = offset;
        element.format = format;
        elements.push_back(element);
        offset += size;
    };

    // Position. XYZRHW is already in screen space and is mutually exclusive
    // with the blended forms; the generated vertex shader maps it through the
    // viewport rather than the world/view/projection chain.
    const u32 position = fvf & fvf::POSITION_MASK;
    switch (position) {
        case fvf::XYZ:
            add(ff_usage::POSITION, 0, BackendVertexFormat::Float3, 12);
            break;
        case fvf::XYZRHW:
            add(ff_usage::POSITIONT, 0, BackendVertexFormat::Float4, 16);
            break;
        case fvf::XYZB1:
        case fvf::XYZB2:
        case fvf::XYZB3:
        case fvf::XYZB4:
        case fvf::XYZB5: {
            add(ff_usage::POSITION, 0, BackendVertexFormat::Float3, 12);
            // Each beta is a float weight, except that LASTBETA_UBYTE4 turns
            // the final one into packed matrix indices.
            const u32 beta_count = (position - fvf::XYZB1) / 2 + 1;
            const bool last_is_indices = (fvf & fvf::LASTBETA_UBYTE4) != 0;
            const u32 weight_count =
                last_is_indices ? (beta_count > 0 ? beta_count - 1 : 0)
                                : beta_count;
            for (u32 i = 0; i < weight_count; ++i) {
                add(ff_usage::BLENDWEIGHT, i, BackendVertexFormat::Float1, 4);
            }
            if (last_is_indices) {
                add(ff_usage::BLENDINDICES, 0, BackendVertexFormat::UByte4, 4);
            }
            break;
        }
        default:
            rejection = "unsupported D3DFVF position mask";
            elements.clear();
            return false;
    }

    if ((fvf & fvf::NORMAL) != 0) {
        // A pre-transformed vertex carries no normal; Direct3D rejects the
        // combination rather than lighting a screen-space position.
        if (position == fvf::XYZRHW) {
            rejection = "D3DFVF_XYZRHW combined with D3DFVF_NORMAL";
            elements.clear();
            return false;
        }
        add(ff_usage::NORMAL, 0, BackendVertexFormat::Float3, 12);
    }
    if ((fvf & fvf::PSIZE) != 0) {
        add(ff_usage::PSIZE, 0, BackendVertexFormat::Float1, 4);
    }
    // D3DCOLOR is BGRA in memory; the backend format names that explicitly so
    // the shader receives RGBA without a swizzle in generated code.
    if ((fvf & fvf::DIFFUSE) != 0) {
        add(ff_usage::COLOR, 0, BackendVertexFormat::UByte4NormalizedBGRA, 4);
    }
    if ((fvf & fvf::SPECULAR) != 0) {
        add(ff_usage::COLOR, 1, BackendVertexFormat::UByte4NormalizedBGRA, 4);
    }

    const u32 texcoord_sets =
        (fvf & fvf::TEXCOUNT_MASK) >> fvf::TEXCOUNT_SHIFT;
    if (texcoord_sets > FF_MAX_TEXTURE_STAGES) {
        rejection = "D3DFVF texture coordinate count above 8";
        elements.clear();
        return false;
    }
    for (u32 i = 0; i < texcoord_sets; ++i) {
        const u32 components = ff_fvf_texcoord_components(fvf, i);
        BackendVertexFormat format = BackendVertexFormat::Float2;
        switch (components) {
            case 1: format = BackendVertexFormat::Float1; break;
            case 2: format = BackendVertexFormat::Float2; break;
            case 3: format = BackendVertexFormat::Float3; break;
            default: format = BackendVertexFormat::Float4; break;
        }
        add(ff_usage::TEXCOORD, i, format, components * 4);
    }

    stride = offset;
    return true;
}

bool ff_build_vertex_layout(u32 fvf,
                            std::vector<BackendVertexAttribute>& attributes,
                            u32& stride, const char*& rejection) {
    attributes.clear();
    std::vector<FFVertexElement> elements;
    if (!ff_enumerate_fvf_elements(fvf, elements, stride, rejection)) {
        stride = 0;
        return false;
    }
    // generate_ff_shaders() numbers [[attribute(n)]] in FVF memory order, so
    // the shader location is the element's index rather than its usage.
    attributes.reserve(elements.size());
    for (usize i = 0; i < elements.size(); ++i) {
        BackendVertexAttribute attribute;
        attribute.shader_location = static_cast<unsigned>(i);
        attribute.offset = elements[i].offset;
        attribute.format = elements[i].format;
        attribute.buffer_index = elements[i].stream;
        attributes.push_back(attribute);
    }
    return true;
}

} // namespace vitreo::graphics
