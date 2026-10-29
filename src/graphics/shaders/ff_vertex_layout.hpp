// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file ff_vertex_layout.hpp
/// @brief Flexible Vertex Format → backend vertex layout.
///
/// D3DFVF_* is one contract shared by every fixed-function Direct3D version:
/// DirectX 7's `dwVertexTypeDesc`, D3D8's and D3D9's `SetFVF` all describe the
/// same packed element order. The shaders `generate_ff_shaders()` emits number
/// their `[[attribute(n)]]` slots in that order, so the enumeration below is
/// the single definition of both the memory layout and the attribute numbering.

#include "graphics/contracts/backend_device.hpp"

#include <vector>

namespace vitreo::graphics {

/// D3DDECLUSAGE values, which are how a vertex shader names the elements an
/// FVF describes.
namespace ff_usage {
constexpr u32 POSITION = 0;
constexpr u32 BLENDWEIGHT = 1;
constexpr u32 BLENDINDICES = 2;
constexpr u32 NORMAL = 3;
constexpr u32 PSIZE = 4;
constexpr u32 TEXCOORD = 5;
constexpr u32 POSITIONT = 9;
constexpr u32 COLOR = 10;
} // namespace ff_usage

struct FFVertexElement {
    u32 usage{0};
    u32 usage_index{0};
    u32 offset{0};
    BackendVertexFormat format{BackendVertexFormat::Float4};
    /// D3DVERTEXELEMENT9.Stream; FVF layouts always use stream 0.
    u32 stream{0};
};

/// D3DFVF_TEXCOORDSIZE* — two bits per set starting at bit 16.
[[nodiscard]] u32 ff_fvf_texcoord_components(u32 fvf, u32 index);

/// Enumerate an FVF's elements in memory order. On failure, @p rejection names
/// the part that cannot be expressed and the outputs are left cleared.
[[nodiscard]] bool ff_enumerate_fvf_elements(u32 fvf,
                                             std::vector<FFVertexElement>& elements,
                                             u32& stride,
                                             const char*& rejection);

/// Vertex layout for the fixed-function pipeline, whose generated shader
/// numbers attributes in FVF order.
[[nodiscard]] bool ff_build_vertex_layout(
    u32 fvf, std::vector<BackendVertexAttribute>& attributes, u32& stride,
    const char*& rejection);

} // namespace vitreo::graphics
