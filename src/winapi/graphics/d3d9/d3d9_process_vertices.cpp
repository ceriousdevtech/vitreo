// SPDX-License-Identifier: LGPL-2.1-only
/// @file d3d9_process_vertices.cpp
/// @brief IDirect3DDevice9::ProcessVertices: fixed-function transform or the
///        CPU vertex-shader subset, written into a destination vertex buffer.

#include "winapi/graphics/d3d9/d3d9_draw_internal.hpp"
#include "winapi/graphics/d3d9/d3d9_pv_cpu.hpp"
#include "winapi/graphics/d3d9/d3d9_state.hpp"

#include "core/memory/guest_memory.hpp"
#include "winapi/core/marshaler.hpp"
#include <vitreo/diagnostics.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace vitreo::winapi {

using namespace d3d9_draw_detail;
using namespace d3d9_pv;

namespace {

[[nodiscard]] u32 vertex_format_bytes(graphics::BackendVertexFormat format) {
    switch (format) {
    case graphics::BackendVertexFormat::Float1:
    case graphics::BackendVertexFormat::UByte4NormalizedBGRA:
    case graphics::BackendVertexFormat::UByte4Normalized:
    case graphics::BackendVertexFormat::UByte4:
    case graphics::BackendVertexFormat::Dec3N:
        return 4;
    case graphics::BackendVertexFormat::Float2:
    case graphics::BackendVertexFormat::Short2:
    case graphics::BackendVertexFormat::Short2Normalized:
    case graphics::BackendVertexFormat::UShort2Normalized:
    case graphics::BackendVertexFormat::Half2:
        return 8;
    case graphics::BackendVertexFormat::Float3:
        return 12;
    case graphics::BackendVertexFormat::Float4:
    case graphics::BackendVertexFormat::Short4:
    case graphics::BackendVertexFormat::Short4Normalized:
    case graphics::BackendVertexFormat::UShort4Normalized:
    case graphics::BackendVertexFormat::Half4:
        return 16;
    }
    return 0;
}

[[nodiscard]] float bits_to_float(u32 bits) {
    float value = 0.0f;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

/// Row-vector transform: out = (x,y,z,w) * M for a D3D row-major matrix.
[[nodiscard]] std::array<float, 4> transform_position(const Matrix& m, float x,
                                                      float y, float z,
                                                      float w) {
    return {x * m[0] + y * m[4] + z * m[8] + w * m[12],
            x * m[1] + y * m[5] + z * m[9] + w * m[13],
            x * m[2] + y * m[6] + z * m[10] + w * m[14],
            x * m[3] + y * m[7] + z * m[11] + w * m[15]};
}

[[nodiscard]] Matrix multiply(const Matrix& a, const Matrix& b) {
    return graphics::ff_matrix_multiply(a, b);
}

[[nodiscard]] std::array<float, 4> pv_load_attribute(
    const u8* vertex, const D3D9Manager::FvfElement& element) {
    std::array<float, 4> value{0.0f, 0.0f, 0.0f, 1.0f};
    const u8* src = vertex + element.offset;
    switch (element.format) {
    case graphics::BackendVertexFormat::Float1:
        std::memcpy(&value[0], src, sizeof(float));
        break;
    case graphics::BackendVertexFormat::Float2:
        std::memcpy(value.data(), src, sizeof(float) * 2);
        value[2] = 0.0f;
        value[3] = 1.0f;
        break;
    case graphics::BackendVertexFormat::Float3:
        std::memcpy(value.data(), src, sizeof(float) * 3);
        value[3] = 1.0f;
        break;
    case graphics::BackendVertexFormat::Float4:
        std::memcpy(value.data(), src, sizeof(float) * 4);
        break;
    case graphics::BackendVertexFormat::UByte4NormalizedBGRA: {
        const u32 packed = static_cast<u32>(src[0]) |
                           (static_cast<u32>(src[1]) << 8) |
                           (static_cast<u32>(src[2]) << 16) |
                           (static_cast<u32>(src[3]) << 24);
        // Memory is BGRA; D3DCOLOR channels are RGBA floats.
        value[0] = static_cast<float>((packed >> 16) & 0xFFu) / 255.0f;
        value[1] = static_cast<float>((packed >> 8) & 0xFFu) / 255.0f;
        value[2] = static_cast<float>(packed & 0xFFu) / 255.0f;
        value[3] = static_cast<float>((packed >> 24) & 0xFFu) / 255.0f;
        break;
    }
    default:
        break;
    }
    return value;
}

void pv_store_float_elements(u8* dest, const D3D9Manager::FvfElement& element,
                             const std::array<float, 4>& value) {
    const u32 bytes = vertex_format_bytes(element.format);
    if (bytes == 0 || bytes > 16) return;
    if (element.format == graphics::BackendVertexFormat::UByte4NormalizedBGRA) {
        const auto channel = [](float v) -> u8 {
            if (!std::isfinite(v)) return 0;
            const float scaled = std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f;
            return static_cast<u8>(scaled);
        };
        dest[element.offset + 0] = channel(value[2]); // B
        dest[element.offset + 1] = channel(value[1]); // G
        dest[element.offset + 2] = channel(value[0]); // R
        dest[element.offset + 3] = channel(value[3]); // A
        return;
    }
    std::memcpy(dest + element.offset, value.data(), bytes);
}

} // namespace

void D3D9Manager::process_vertices(Marshaler& m, u32 self) {
    // Learn: IDirect3DDevice9::ProcessVertices — run the vertex pipeline into
    // a destination VB. Fixed-function: world*view*proj + viewport → XYZRHW.
    // Programmable: CPU-interpret mov/add/sub/mul/mad/lrp/cmp/rcp/rsq/min/
    // max/dp3/dp4/m4x4/m4x3/m3x4/m3x3/m3x2/pow/abs/frc/nrm/sgn/slt/sge/exp/
    // expp/log/logp/lit/dst/sincos/crs/mova/setp/loop/rep/break/breakc/
    // breakp/call/callnz/label/ret/if/ifc/else/endif (oPos → viewport),
    // optional oD#/oT#, relative c[aL+n]/c[a0+n], source mods.
    // Unsupported VS (e.g. texkill) / FVF → E_NOTIMPL.
    ComObject* device_entry = find(self, D3D9InterfaceId::Device);
    if (device_entry == nullptr || !device_entry->device_state) {
        m.set_return(D3DERR_INVALIDCALL);
        return;
    }
    DeviceState& state = *device_entry->device_state;

    const u32 src_start = static_cast<u32>(m.arg(1));
    const u32 dest_index = static_cast<u32>(m.arg(2));
    const u32 vertex_count = static_cast<u32>(m.arg(3));
    const u32 dest_buffer_obj = static_cast<u32>(m.arg(4));
    const u32 vertex_decl_obj = static_cast<u32>(m.arg(5));
    const u32 flags = static_cast<u32>(m.arg(6));
    (void)vertex_decl_obj;

    if (dest_buffer_obj == 0) {
        m.set_return(D3DERR_INVALIDCALL);
        return;
    }
    if ((flags & D3DLOCK_READONLY) != 0) {
        m.set_return(D3DERR_INVALIDCALL);
        return;
    }
    if (vertex_count == 0) {
        m.set_return(D3D_OK);
        return;
    }

    ComObject* dest_entry =
        find(dest_buffer_obj, D3D9InterfaceId::VertexBuffer);
    if (dest_entry == nullptr || !dest_entry->buffer) {
        m.set_return(D3DERR_INVALIDCALL);
        return;
    }
    BufferState& dest = *dest_entry->buffer;
    if (dest.fvf_or_format == 0) {
        // Learn: dest VB must be created with a nonzero FVF.
        m.set_return(D3DERR_INVALIDCALL);
        return;
    }
    if (dest.locked) {
        m.set_return(D3DERR_INVALIDCALL);
        return;
    }

    const ShaderState* vertex_shader = nullptr;
    PvCpuPlan vs_plan{};
    if (state.vertex_shader != 0) {
        ComObject* vs_entry =
            find(state.vertex_shader, D3D9InterfaceId::VertexShader);
        if (vs_entry == nullptr || !vs_entry->shader) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        vertex_shader = vs_entry->shader.get();
        if (!analyze_vs_for_process_vertices(vertex_shader->bytecode.data(),
                                             vertex_shader->bytecode.size(),
                                             vs_plan)) {
            VITREO_TRACE("[d3d9] ProcessVertices -> E_NOTIMPL "
                         "(CPU VS subset: %s)\n",
                         vs_plan.rejection);
            m.set_return(E_NOTIMPL_D3D);
            return;
        }
    } else if (state.fvf == 0 && state.vertex_declaration == 0) {
        VITREO_TRACE("[d3d9] ProcessVertices -> E_NOTIMPL "
                     "(fixed-function ProcessVertices requires SetFVF)\n");
        m.set_return(E_NOTIMPL_D3D);
        return;
    }

    const DeviceState::StreamSource& stream = state.streams[0];
    if (stream.buffer == 0 || stream.stride == 0) {
        m.set_return(D3DERR_INVALIDCALL);
        return;
    }
    ComObject* src_entry =
        find(stream.buffer, D3D9InterfaceId::VertexBuffer);
    if (src_entry == nullptr || !src_entry->buffer) {
        m.set_return(D3DERR_INVALIDCALL);
        return;
    }
    BufferState& src = *src_entry->buffer;

    constexpr u32 USAGE_POSITION = 0;
    constexpr u32 USAGE_NORMAL = 3;
    constexpr u32 USAGE_TEXCOORD = 5;
    constexpr u32 USAGE_POSITIONT = 9;
    constexpr u32 USAGE_COLOR = 10;

    const char* rejection = "an unsupported ProcessVertices FVF";
    std::vector<FvfElement> src_elements;
    std::vector<FvfElement> dest_elements;
    u32 src_layout_stride = 0;
    u32 dest_stride = 0;

    if (state.vertex_declaration != 0) {
        ComObject* decl_entry =
            find(state.vertex_declaration, D3D9InterfaceId::VertexDeclaration);
        if (decl_entry == nullptr || !decl_entry->vertex_declaration) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        // ProcessVertices still reads only stream 0 (single source stream).
        for (const FvfElement& element :
             decl_entry->vertex_declaration->elements) {
            if (element.stream == 0) {
                src_elements.push_back(element);
            }
        }
        src_layout_stride =
            decl_entry->vertex_declaration->stream_strides[0];
        if (src_elements.empty() || src_layout_stride == 0) {
            VITREO_TRACE("[d3d9] ProcessVertices -> E_NOTIMPL "
                         "(vertex declaration has no stream-0 layout)\n");
            m.set_return(E_NOTIMPL_D3D);
            return;
        }
    } else if (!enumerate_fvf_elements(state.fvf, src_elements,
                                       src_layout_stride, rejection)) {
        VITREO_TRACE("[d3d9] ProcessVertices -> E_NOTIMPL (%s)\n", rejection);
        m.set_return(E_NOTIMPL_D3D);
        return;
    }
    if (!enumerate_fvf_elements(dest.fvf_or_format, dest_elements, dest_stride,
                                rejection)) {
        VITREO_TRACE("[d3d9] ProcessVertices -> E_NOTIMPL (%s)\n", rejection);
        m.set_return(E_NOTIMPL_D3D);
        return;
    }
    if (stream.stride < src_layout_stride) {
        m.set_return(D3DERR_INVALIDCALL);
        return;
    }

    const FvfElement* src_pos =
        find_fvf_element(src_elements, USAGE_POSITION, 0);
    const FvfElement* src_post =
        find_fvf_element(src_elements, USAGE_POSITIONT, 0);
    const FvfElement* dest_post =
        find_fvf_element(dest_elements, USAGE_POSITIONT, 0);
    const FvfElement* dest_pos =
        find_fvf_element(dest_elements, USAGE_POSITION, 0);

    const bool src_xyz = src_pos != nullptr && src_post == nullptr;
    const bool src_xyzrhw = src_post != nullptr && src_pos == nullptr;
    const bool dest_xyzrhw = dest_post != nullptr && dest_pos == nullptr;
    if (!dest_xyzrhw) {
        VITREO_TRACE("[d3d9] ProcessVertices -> E_NOTIMPL "
                     "(destination FVF must include D3DFVF_XYZRHW)\n");
        m.set_return(E_NOTIMPL_D3D);
        return;
    }
    if (vertex_shader == nullptr && !src_xyz && !src_xyzrhw) {
        VITREO_TRACE("[d3d9] ProcessVertices -> E_NOTIMPL "
                     "(source FVF needs D3DFVF_XYZ or D3DFVF_XYZRHW)\n");
        m.set_return(E_NOTIMPL_D3D);
        return;
    }
    // Lighting / normal processing is out of scope for this path.
    if (find_fvf_element(dest_elements, USAGE_NORMAL, 0) != nullptr) {
        VITREO_TRACE("[d3d9] ProcessVertices -> E_NOTIMPL "
                     "(no ProcessVertices path into destination normals)\n");
        m.set_return(E_NOTIMPL_D3D);
        return;
    }

    if (vertex_shader != nullptr) {
        for (const graphics::Sm3InputElement& input : vs_plan.inputs) {
            if (find_fvf_element(src_elements, static_cast<u32>(input.usage),
                                 input.usage_index) == nullptr) {
                VITREO_TRACE("[d3d9] ProcessVertices -> D3DERR_INVALIDCALL "
                             "(VS input usage %u index %u missing from "
                             "source layout)\n",
                             static_cast<u32>(input.usage),
                             input.usage_index);
                m.set_return(D3DERR_INVALIDCALL);
                return;
            }
        }
    }

    const u64 src_bytes_needed =
        static_cast<u64>(stream.offset) +
        static_cast<u64>(src_start + vertex_count) * stream.stride;
    const u64 dest_bytes_needed =
        static_cast<u64>(dest_index + vertex_count) * dest_stride;
    if (src_bytes_needed > src.length || dest_bytes_needed > dest.length) {
        m.set_return(D3DERR_INVALIDCALL);
        return;
    }

    const auto transform_of = [&](u32 which) -> Matrix {
        const auto found = state.transforms.find(which);
        return found != state.transforms.end() ? found->second : IDENTITY;
    };
    const Matrix world_view_proj = multiply(
        multiply(transform_of(D3DTS_WORLD), transform_of(D3DTS_VIEW)),
        transform_of(D3DTS_PROJECTION));

    const float vp_x = static_cast<float>(state.viewport[0]);
    const float vp_y = static_cast<float>(state.viewport[1]);
    const float vp_w = static_cast<float>(state.viewport[2]);
    const float vp_h = static_cast<float>(state.viewport[3]);
    const float vp_min_z = bits_to_float(state.viewport[4]);
    const float vp_max_z = bits_to_float(state.viewport[5]);
    const bool copy_unaffected = (flags & D3DPV_DONOTCOPYDATA) == 0;

    std::vector<u8> src_vertex(stream.stride);
    std::vector<u8> dest_vertex(dest_stride);

    for (u32 i = 0; i < vertex_count; ++i) {
        const GuestAddr src_addr = src.storage + stream.offset +
                                   static_cast<u64>(src_start + i) *
                                       stream.stride;
        const GuestAddr dest_addr =
            dest.storage + static_cast<u64>(dest_index + i) * dest_stride;

        if (m.memory()
                .read_bytes(src_addr, src_vertex.data(), src_vertex.size())
                .is_err()) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        // Preserve existing destination contents so DONOTCOPYDATA and missing
        // source elements leave prior bytes alone (Learn).
        if (m.memory()
                .read_bytes(dest_addr, dest_vertex.data(), dest_vertex.size())
                .is_err()) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }

        float out_x = 0.0f;
        float out_y = 0.0f;
        float out_z = 0.0f;
        float out_rhw = 1.0f;

        if (vertex_shader != nullptr) {
            std::array<std::array<float, 4>, PV_MAX_INPUTS> vs_inputs{};
            for (auto& v : vs_inputs) v = {0, 0, 0, 1};
            for (const graphics::Sm3InputElement& input : vs_plan.inputs) {
                const FvfElement* el = find_fvf_element(
                    src_elements, static_cast<u32>(input.usage),
                    input.usage_index);
                if (el == nullptr || input.register_index >= PV_MAX_INPUTS) {
                    m.set_return(D3DERR_INVALIDCALL);
                    return;
                }
                vs_inputs[input.register_index] =
                    pv_load_attribute(src_vertex.data(), *el);
            }

            std::array<float, 4> opos{};
            std::array<std::array<float, 4>, PV_MAX_COLORS> ocolor{};
            std::array<std::array<float, 4>, PV_MAX_TEXCOORDS> otexcoord{};
            if (!run_vs_cpu_process_vertices(
                    vs_plan, vs_inputs.data(), state.vs_const_f.data(),
                    state.vs_const_i.data(), state.vs_const_b.data(), opos,
                    ocolor, otexcoord)) {
                m.set_return(D3DERR_INVALIDCALL);
                return;
            }
            if (opos[3] == 0.0f || !std::isfinite(opos[3])) {
                m.set_return(D3DERR_INVALIDCALL);
                return;
            }
            const float inv_w = 1.0f / opos[3];
            const float ndc_x = opos[0] * inv_w;
            const float ndc_y = opos[1] * inv_w;
            const float ndc_z = opos[2] * inv_w;
            out_x = (ndc_x + 1.0f) * vp_w * 0.5f + vp_x;
            out_y = (1.0f - ndc_y) * vp_h * 0.5f + vp_y;
            out_z = vp_min_z + ndc_z * (vp_max_z - vp_min_z);
            out_rhw = inv_w;

            std::memcpy(dest_vertex.data() + dest_post->offset, &out_x,
                        sizeof(out_x));
            std::memcpy(dest_vertex.data() + dest_post->offset + 4, &out_y,
                        sizeof(out_y));
            std::memcpy(dest_vertex.data() + dest_post->offset + 8, &out_z,
                        sizeof(out_z));
            std::memcpy(dest_vertex.data() + dest_post->offset + 12, &out_rhw,
                        sizeof(out_rhw));

            for (u32 ci = 0; ci < PV_MAX_COLORS; ++ci) {
                if (!vs_plan.writes_color[ci]) continue;
                const FvfElement* dest_el =
                    find_fvf_element(dest_elements, USAGE_COLOR, ci);
                if (dest_el == nullptr) continue;
                pv_store_float_elements(dest_vertex.data(), *dest_el,
                                        ocolor[ci]);
            }
            for (u32 ti = 0; ti < PV_MAX_TEXCOORDS; ++ti) {
                if (!vs_plan.writes_texcoord[ti]) continue;
                const FvfElement* dest_el =
                    find_fvf_element(dest_elements, USAGE_TEXCOORD, ti);
                if (dest_el == nullptr) continue;
                pv_store_float_elements(dest_vertex.data(), *dest_el,
                                        otexcoord[ti]);
            }

            if (copy_unaffected) {
                for (const FvfElement& dest_el : dest_elements) {
                    if (dest_el.usage == USAGE_POSITIONT) continue;
                    if (dest_el.usage == USAGE_COLOR &&
                        dest_el.usage_index < PV_MAX_COLORS &&
                        vs_plan.writes_color[dest_el.usage_index]) {
                        continue;
                    }
                    if (dest_el.usage == USAGE_TEXCOORD &&
                        dest_el.usage_index < PV_MAX_TEXCOORDS &&
                        vs_plan.writes_texcoord[dest_el.usage_index]) {
                        continue;
                    }
                    const FvfElement* src_el = find_fvf_element(
                        src_elements, dest_el.usage, dest_el.usage_index);
                    if (src_el == nullptr ||
                        src_el->format != dest_el.format) {
                        continue;
                    }
                    const u32 bytes = vertex_format_bytes(dest_el.format);
                    if (bytes == 0) continue;
                    std::memcpy(dest_vertex.data() + dest_el.offset,
                                src_vertex.data() + src_el->offset, bytes);
                }
            }
        } else if (src_xyz) {
            float x = 0.0f;
            float y = 0.0f;
            float z = 0.0f;
            std::memcpy(&x, src_vertex.data() + src_pos->offset, sizeof(x));
            std::memcpy(&y, src_vertex.data() + src_pos->offset + 4, sizeof(y));
            std::memcpy(&z, src_vertex.data() + src_pos->offset + 8, sizeof(z));
            const auto clip =
                transform_position(world_view_proj, x, y, z, 1.0f);
            if (clip[3] == 0.0f || !std::isfinite(clip[3])) {
                m.set_return(D3DERR_INVALIDCALL);
                return;
            }
            const float inv_w = 1.0f / clip[3];
            const float ndc_x = clip[0] * inv_w;
            const float ndc_y = clip[1] * inv_w;
            const float ndc_z = clip[2] * inv_w;
            out_x = (ndc_x + 1.0f) * vp_w * 0.5f + vp_x;
            out_y = (1.0f - ndc_y) * vp_h * 0.5f + vp_y;
            out_z = vp_min_z + ndc_z * (vp_max_z - vp_min_z);
            out_rhw = inv_w;

            std::memcpy(dest_vertex.data() + dest_post->offset, &out_x,
                        sizeof(out_x));
            std::memcpy(dest_vertex.data() + dest_post->offset + 4, &out_y,
                        sizeof(out_y));
            std::memcpy(dest_vertex.data() + dest_post->offset + 8, &out_z,
                        sizeof(out_z));
            std::memcpy(dest_vertex.data() + dest_post->offset + 12, &out_rhw,
                        sizeof(out_rhw));

            if (copy_unaffected) {
                for (const FvfElement& dest_el : dest_elements) {
                    if (dest_el.usage == USAGE_POSITIONT) continue;
                    const FvfElement* src_el = find_fvf_element(
                        src_elements, dest_el.usage, dest_el.usage_index);
                    if (src_el == nullptr ||
                        src_el->format != dest_el.format) {
                        continue;
                    }
                    const u32 bytes = vertex_format_bytes(dest_el.format);
                    if (bytes == 0) continue;
                    std::memcpy(dest_vertex.data() + dest_el.offset,
                                src_vertex.data() + src_el->offset, bytes);
                }
            }
        } else {
            std::memcpy(&out_x, src_vertex.data() + src_post->offset,
                        sizeof(out_x));
            std::memcpy(&out_y, src_vertex.data() + src_post->offset + 4,
                        sizeof(out_y));
            std::memcpy(&out_z, src_vertex.data() + src_post->offset + 8,
                        sizeof(out_z));
            std::memcpy(&out_rhw, src_vertex.data() + src_post->offset + 12,
                        sizeof(out_rhw));

            std::memcpy(dest_vertex.data() + dest_post->offset, &out_x,
                        sizeof(out_x));
            std::memcpy(dest_vertex.data() + dest_post->offset + 4, &out_y,
                        sizeof(out_y));
            std::memcpy(dest_vertex.data() + dest_post->offset + 8, &out_z,
                        sizeof(out_z));
            std::memcpy(dest_vertex.data() + dest_post->offset + 12, &out_rhw,
                        sizeof(out_rhw));

            if (copy_unaffected) {
                for (const FvfElement& dest_el : dest_elements) {
                    if (dest_el.usage == USAGE_POSITIONT) continue;
                    const FvfElement* src_el = find_fvf_element(
                        src_elements, dest_el.usage, dest_el.usage_index);
                    if (src_el == nullptr ||
                        src_el->format != dest_el.format) {
                        continue;
                    }
                    const u32 bytes = vertex_format_bytes(dest_el.format);
                    if (bytes == 0) continue;
                    std::memcpy(dest_vertex.data() + dest_el.offset,
                                src_vertex.data() + src_el->offset, bytes);
                }
            }
        }

        if (m.memory()
                .write_bytes(dest_addr, dest_vertex.data(), dest_vertex.size())
                .is_err()) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
    }

    const u32 upload_offset = dest_index * dest_stride;
    const u32 upload_size = vertex_count * dest_stride;
    if (dest.backend && dest.backend->valid() && upload_size != 0) {
        // An ordered write (it may rename): the worker must have recorded
        // the draws queued before it, as for Unlock.
        d3d9_cs_drain();
        std::vector<u8> bytes(upload_size);
        if (m.memory()
                .read_bytes(dest.storage + upload_offset, bytes.data(),
                            bytes.size())
                .is_ok()) {
            if (!dest.backend->update(upload_offset, bytes.data(),
                                      bytes.size())) {
                VITREO_TRACE("[d3d9] ProcessVertices: backend rejected %u "
                             "bytes at offset %u\n",
                             upload_size, upload_offset);
                m.set_return(D3DERR_INVALIDCALL);
                return;
            }
        }
    }

    VITREO_TRACE("[d3d9] ProcessVertices ok count=%u src_fvf=0x%x dest_fvf=0x%x "
                 "flags=0x%x vs=%s\n",
                 vertex_count, state.fvf, dest.fvf_or_format, flags,
                 vertex_shader != nullptr ? "cpu-subset" : "ff");
    m.set_return(D3D_OK);
}

} // namespace vitreo::winapi
