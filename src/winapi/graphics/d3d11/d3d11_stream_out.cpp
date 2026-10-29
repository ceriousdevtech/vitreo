// SPDX-License-Identifier: LGPL-2.1-only
/// @file d3d11_stream_out.cpp
/// @brief D3D11 stream output: CreateGeometryShaderWithStreamOutput over a
///        vertex shader, the GS/SO binding state, the capture-and-repack
///        draw, and DrawAuto.
///
/// Metal has neither a geometry stage nor stream output. A streaming draw runs
/// as GPU passes recorded in command order:
///
///  1. a CAPTURE draw -- the bound vertex shader, translated to also write
///     every output register into a buffer keyed by vertex id, with
///     rasterization off (execute_draw with a capture DrawVariant); a
///     geometry program's expansion or a tessellated draw's domain records
///     take its place for GS / DS stream output;
///  2. vitreo_so_prepare -- how many whole primitives fit, the offsets to
///     write at, the advanced filled sizes and the query counts, all on the
///     GPU, because the filled size (BufferFilledSize) and the draw's counts
///     can be GPU-sourced (an indirect draw, a GS's emits, the tessellator,
///     DrawAuto);
///  3. a REPACK dispatch that walks the draw's primitives in the order Windows
///     writes them and copies each declared component into the SO targets
///     (graphics/shaders/stream_out_repack.hpp).
///
/// Every counting rule here is measured on Windows by
/// tools/oracle_probe/d3d11_stream_out_probe.c and d3d11_gsso_probe.c, whose
/// output is checked in beside them. The ones Learn does not state:
///
///  - A slot a stream's declaration uses with no buffer bound means that
///    stream writes NOTHING; its needed count still grows, and the other
///    streams are unaffected.
///  - Primitives that do not fit are dropped whole; the smallest target of
///    the stream decides for all of its slots. One stream overflowing does
///    not stop the others.
///  - SOSetTargets with an explicit offset sets the buffer's filled size (in
///    command order, a GPU copy); -1 keeps it (0 for a buffer never streamed
///    to).
///  - DrawAuto draws (filled size - IA slot-0 offset) / IA slot-0 stride
///    vertices, with SV_VertexID from 0, the count computed on the GPU.
///  - A slot may be written by one stream only; an entry must name an
///    element of its own stream; streams past 3, or ones the shader does not
///    declare, are E_INVALIDARG; RasterizedStream is only range-checked.

#include "winapi/graphics/d3d11/d3d11.hpp"
#include "winapi/graphics/d3d11/d3d11_internal.hpp"
#include "winapi/core/marshaler.hpp"
#include "graphics/contracts/backend_device.hpp"
#include "graphics/shaders/dxbc_parser.hpp"
#include "graphics/shaders/dxbc_to_spirv.hpp"
#include "graphics/shaders/gs_passthrough.hpp"
#include "graphics/shaders/ia_resolve.hpp"
#include "graphics/shaders/stream_out_repack.hpp"
#include "core/memory/guest_memory.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace vitreo::winapi {
namespace {

// d3d11.h
constexpr u32 SO_NO_RASTERIZED_STREAM = 0xFFFFFFFFu;
constexpr u32 SO_BUFFER_SLOT_COUNT = 4;
constexpr u32 SO_STREAM_COUNT = 4;
constexpr u32 SO_OUTPUT_COMPONENT_COUNT = 128;
constexpr u32 SO_BUFFER_MAX_STRIDE_IN_BYTES = 2048;
constexpr u32 SO_APPEND_OFFSET = 0xFFFFFFFFu;
constexpr u32 BIND_STREAM_OUTPUT = 0x10u;
constexpr u32 BIND_UNORDERED_ACCESS = 0x80u;
constexpr u32 FORMAT_R16_UINT = 57;

constexpr GuidBytes IID_ID3D11GEOMETRYSHADER{
    0x96, 0x5B, 0x32, 0x38, 0xFB, 0xEF, 0x22, 0x40,
    0xBA, 0x02, 0x2E, 0x79, 0x5B, 0x70, 0x27, 0x5C};

/// Capture buffers larger than this are declined rather than allocated: the
/// size follows the largest vertex id the draw reaches, which a guest can make
/// arbitrarily large with a big BaseVertexLocation.
constexpr u64 MAX_CAPTURE_BYTES = 256ull << 20;

[[nodiscard]] bool semantic_equal(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (usize i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

struct StreamTopology {
    u32 vertices_per_primitive{0};
    bool strip{false};
};

/// D3D11_PRIMITIVE_TOPOLOGY -> what stream output writes. Zero vertices per
/// primitive means stream output is not served for this topology.
[[nodiscard]] StreamTopology stream_topology(u32 topology) {
    switch (topology) {
    case 1: return {1, false};  // POINTLIST
    case 2: return {2, false};  // LINELIST
    case 3: return {2, true};   // LINESTRIP
    case 4: return {3, false};  // TRIANGLELIST
    case 5: return {3, true};   // TRIANGLESTRIP
    default: return {};
    }
}

}  // namespace

std::string d3d11_geometry_shader_method_export_name(usize slot) {
    char name[64];
    std::snprintf(name, sizeof(name), "_D3D11_GeometryShader_%03zu", slot);
    return name;
}

// -----------------------------------------------------------------------------
// Object lifetime
// -----------------------------------------------------------------------------

// retain_object: d3d11_child_refcount.cpp (device-child reference counts).

u32 D3D11Manager::release_geometry_shader(GuestAddr address) {
    auto it = m_geometry_shaders.find(address);
    if (it == m_geometry_shaders.end() || it->second.ref_count == 0) return 0;
    const u32 count = --it->second.ref_count;
    if (count == 0) {
        ShaderLinkage linkage = std::move(it->second.linkage);
        m_geometry_shaders.erase(it);
        forget_msl_hashes();
        free_object(address);
        destroy_specialisations(LinkedStage::Geometry, linkage);
    }
    return count;
}

void D3D11Manager::impl_GeometryShaderMethod(Marshaler& m, usize slot) {
    const GuestAddr self = m.arg_ptr(0);
    auto it = m_geometry_shaders.find(self);
    if (it == m_geometry_shaders.end()) {
        m.set_return(slot == 1 || slot == 2 ? 0 : d3d11::E_INVALIDARG);
        return;
    }
    switch (slot) {
    case 0: {
        const GuestAddr object_out = m.arg_ptr(2);
        if (object_out == 0) {
            m.set_return(d3d11::E_POINTER);
            return;
        }
        (void)m.write_ptr(object_out, 0);
        GuidBytes iid{};
        if (!read_guid(m.arg_ptr(1), iid) ||
            (!guid_equal(iid, IID_IUNKNOWN) &&
             !guid_equal(iid, IID_ID3D11DEVICECHILD) &&
             !guid_equal(iid, IID_ID3D11GEOMETRYSHADER))) {
            // The object's D3D10 interfaces (d3d10_device.cpp).
            if (const GuestAddr facet = d3d10_interface_for(self, iid)) {
                (void)retain_object(self);
                (void)m.write_ptr(object_out, facet);
                m.set_return(d3d11::S_OK);
                return;
            }
            m.set_return(d3d11::E_NOINTERFACE);
            return;
        }
        (void)retain_object(self);
        if (m.write_ptr(object_out, self).is_err()) {
            (void)release_object(self);
            m.set_return(d3d11::E_POINTER);
            return;
        }
        m.set_return(d3d11::S_OK);
        return;
    }
    case 1:
        m.set_return(retain_object(self));
        return;
    case 2:
        m.set_return(release_object(self));
        return;
    case 3: {
        const GuestAddr device_out = m.arg_ptr(1);
        auto device = m_devices.find(it->second.device_owner);
        if (device_out != 0 && device != m_devices.end()) {
            (void)add_ref(device->second.device_interface);
            if (m.write_ptr(device_out,
                            device->second.device_interface).is_err()) {
                (void)release(device->second.device_interface);
            }
        }
        m.set_return(0);
        return;
    }
    case 4:
        m.set_return(get_private_data(m, it->second.private_data));
        return;
    case 5:
        m.set_return(set_private_data(m, it->second.private_data));
        return;
    case 6:
        m.set_return(set_private_data_interface(m, it->second.private_data));
        return;
    default:
        m.set_return(d3d11::E_NOTIMPL);
        return;
    }
}

// -----------------------------------------------------------------------------
// CreateGeometryShaderWithStreamOutput
// -----------------------------------------------------------------------------

void D3D11Manager::create_geometry_shader_with_stream_output(Marshaler& m,
                                                             GuestAddr self) {
    // (pShaderBytecode, BytecodeLength, pSODeclaration, NumEntries,
    //  pBufferStrides, NumStrides, RasterizedStream, pClassLinkage,
    //  ppGeometryShader)
    const GuestAddr bytecode = m.arg_ptr(1);
    const u64 length = m.arg(2);
    const GuestAddr decl = m.arg_ptr(3);
    const u32 num_entries = static_cast<u32>(m.arg(4));
    const GuestAddr strides_addr = m.arg_ptr(5);
    const u32 num_strides = static_cast<u32>(m.arg(6));
    const u32 rasterized_stream = static_cast<u32>(m.arg(7));
    const GuestAddr linkage = m.arg_ptr(8);
    const GuestAddr shader_out = m.arg_ptr(9);

    // Windows NULLs the out-parameter on every failure, measured.
    if (shader_out != 0) (void)m.write_ptr(shader_out, 0);
    const auto refuse = [&](u32 hr, const char* why) {
        d3d11_diag("ID3D11Device::CreateGeometryShaderWithStreamOutput -> "
                   "0x%08x (%s)", hr, why);
        m.set_return(hr);
    };

    if (bytecode == 0 || length == 0 || length > (1u << 24)) {
        refuse(d3d11::E_INVALIDARG, "no bytecode");
        return;
    }
    if (!valid_class_linkage(m_interfaces.find(self)->second.owner,
                             linkage)) {
        refuse(d3d11::E_INVALIDARG, "not a class linkage of this device");
        return;
    }
    if (decl == 0 || num_entries == 0 ||
        num_entries > SO_STREAM_COUNT * SO_OUTPUT_COMPONENT_COUNT) {
        refuse(d3d11::E_INVALIDARG, "declaration empty or too long");
        return;
    }
    if (num_strides > SO_BUFFER_SLOT_COUNT ||
        (num_strides != 0 && strides_addr == 0)) {
        refuse(d3d11::E_INVALIDARG, "bad stride array");
        return;
    }
    if (rasterized_stream != SO_NO_RASTERIZED_STREAM &&
        rasterized_stream >= SO_STREAM_COUNT) {
        refuse(d3d11::E_INVALIDARG, "RasterizedStream out of range");
        return;
    }

    std::vector<u8> bytes(static_cast<usize>(length));
    if (m_memory.read_bytes(bytecode, bytes.data(), bytes.size()).is_err()) {
        refuse(d3d11::E_INVALIDARG, "bytecode unreadable");
        return;
    }
    const auto container = graphics::parse_dxbc(bytes.data(), bytes.size());
    if (!container.valid()) {
        refuse(d3d11::E_INVALIDARG, "not DXBC");
        return;
    }
    if (const char* why = shader_bytecode_refused(self, bytes.data(),
                                                  bytes.size(), {})) {
        refuse(d3d11::E_INVALIDARG, why);
        return;
    }
    if (uav_requirements_unavailable(self, bytes.data(), bytes.size())) {
        refuse(d3d11::E_INVALIDARG, "declares a UAV below feature level 11_1");
        return;
    }
    // WARP: a shader that declares interfaces needs a class linkage
    // (E_INVALIDARG without one, d3d11_classlink_probe [create]).
    if (graphics::dxbc_uses_class_linkage(container) &&
        refuse_interface_shader(linkage,
                                "CreateGeometryShaderWithStreamOutput") !=
            d3d11::S_OK) {
        refuse(d3d11::E_INVALIDARG, "shader declares interfaces");
        return;
    }
    // A geometry PROGRAM streams what its expansion emits (11.29); a vertex
    // or domain shader streams its own outputs (the "NULL GS" of D3D11.3
    // 14.3), the domain shader's after tessellation.
    const bool geometry_program =
        container.stage == graphics::ShaderStage::Geometry;
    if (!geometry_program &&
        container.stage != graphics::ShaderStage::Vertex &&
        container.stage != graphics::ShaderStage::Domain) {
        refuse(d3d11::E_INVALIDARG, "bytecode is not a vertex, geometry or "
                                    "domain shader");
        return;
    }
    // The streams the shader produces: dcl_stream m0..m3 in a geometry
    // program (the OSG5 elements carry theirs), otherwise stream 0 alone.
    u32 declared_streams = 1u;
    if (geometry_program) {
        for (const auto& out : container.output_signature) {
            if (out.stream < SO_STREAM_COUNT) declared_streams |= 1u << out.stream;
        }
    }

    // Validate the declaration against the shader's output signature. Each
    // rule below is one Windows refused in d3d11_stream_out_probe or
    // d3d11_gsso_probe.
    const usize entry_size = m_is_64bit ? 24u : 16u;
    const usize name_offset = m_is_64bit ? 8u : 4u;
    const usize fields_offset = name_offset + (m_is_64bit ? 8u : 4u);
    std::vector<StreamOutDeclEntry> entries;
    entries.reserve(num_entries);
    std::array<u32, SO_BUFFER_SLOT_COUNT> slot_dwords{};
    std::array<bool, SO_BUFFER_SLOT_COUNT> slot_used{};
    // The stream each slot takes: "A Buffer cannot receive data from
    // multiple streams at once" (D3D11.3 13.5.1; measured E_INVALIDARG).
    std::array<u32, SO_BUFFER_SLOT_COUNT> slot_stream{};
    std::array<u32, SO_STREAM_COUNT> stream_slots{};
    // Components already streamed, per stream and output register: Windows
    // refuses a declaration that writes the same component twice. Streams
    // declare their outputs each in "its own clean slate view of the full
    // output register set" (D3D11.3 13.5.2), so o0 of two streams differ.
    std::array<std::array<u8, 64>, SO_STREAM_COUNT> written_components{};
    u32 highest_slot = 0;
    for (u32 i = 0; i < num_entries; ++i) {
        const GuestAddr base = decl + static_cast<GuestAddr>(i) * entry_size;
        const auto stream = m_memory.read_u32(base);
        GuestAddr name_address = 0;
        bool name_ok = true;
        if (m_is_64bit) {
            const auto p = m_memory.read_u64(base + name_offset);
            name_ok = p.is_ok();
            if (name_ok) name_address = p.value();
        } else {
            const auto p = m_memory.read_u32(base + name_offset);
            name_ok = p.is_ok();
            if (name_ok) name_address = p.value();
        }
        const auto semantic_index = m_memory.read_u32(base + fields_offset);
        const auto start = m_memory.read_u8(base + fields_offset + 4);
        const auto count = m_memory.read_u8(base + fields_offset + 5);
        const auto slot = m_memory.read_u8(base + fields_offset + 6);
        if (stream.is_err() || !name_ok || semantic_index.is_err() ||
            start.is_err() || count.is_err() || slot.is_err()) {
            refuse(d3d11::E_INVALIDARG, "declaration unreadable");
            return;
        }
        if (stream.value() >= SO_STREAM_COUNT ||
            (declared_streams & (1u << stream.value())) == 0) {
            // Measured: Stream 4, and Stream 1 of a vertex shader or of a
            // geometry shader that declares no stream 1.
            refuse(d3d11::E_INVALIDARG,
                   "the entry names a stream the shader does not produce");
            return;
        }
        StreamOutDeclEntry entry{};
        entry.stream = stream.value();
        entry.slot = slot.value();
        entry.start_component = start.value();
        entry.component_count = count.value();
        if (entry.slot >= SO_BUFFER_SLOT_COUNT) {
            refuse(d3d11::E_INVALIDARG, "OutputSlot out of range");
            return;
        }
        if (slot_used[entry.slot] && slot_stream[entry.slot] != entry.stream) {
            refuse(d3d11::E_INVALIDARG, "one slot takes two streams");
            return;
        }
        if (entry.component_count == 0 ||
            entry.start_component + entry.component_count > 4) {
            refuse(d3d11::E_INVALIDARG, "component range out of range");
            return;
        }
        if (name_address == 0) {
            entry.gap = true;
        } else {
            std::string name;
            for (usize n = 0; n < 256; ++n) {
                const auto ch = m_memory.read_u8(name_address + n);
                if (ch.is_err()) {
                    refuse(d3d11::E_INVALIDARG, "semantic unreadable");
                    return;
                }
                if (ch.value() == 0) break;
                name.push_back(static_cast<char>(ch.value()));
            }
            // Only the entry's own stream's outputs (measured: stream 1
            // naming stream 0's TEXCOORD0 is refused).
            const graphics::DxbcSignatureElement* element = nullptr;
            for (const auto& out : container.output_signature) {
                if (out.stream == entry.stream &&
                    out.semantic_index == semantic_index.value() &&
                    semantic_equal(out.semantic_name, name)) {
                    element = &out;
                    break;
                }
            }
            if (element == nullptr) {
                refuse(d3d11::E_INVALIDARG,
                       "semantic is not in the stream's output signature");
                return;
            }
            const u32 wanted = ((1u << entry.component_count) - 1u)
                               << entry.start_component;
            if ((element->component_mask & wanted) != wanted) {
                refuse(d3d11::E_INVALIDARG,
                       "components past the element's mask");
                return;
            }
            if (element->register_num >= written_components[0].size()) {
                refuse(d3d11::E_INVALIDARG, "output register out of range");
                return;
            }
            u8& used =
                written_components[entry.stream][element->register_num];
            if ((used & wanted) != 0) {
                refuse(d3d11::E_INVALIDARG, "component streamed twice");
                return;
            }
            used = static_cast<u8>(used | wanted);
            entry.output_register = element->register_num;
        }
        entry.dst_dword = slot_dwords[entry.slot];
        slot_dwords[entry.slot] += entry.component_count;
        slot_used[entry.slot] = true;
        slot_stream[entry.slot] = entry.stream;
        stream_slots[entry.stream] |= 1u << entry.slot;
        highest_slot = std::max(highest_slot, entry.slot);
        entries.push_back(entry);
    }

    std::array<u32, SO_BUFFER_SLOT_COUNT> strides{};
    if (num_strides == 0) {
        // Windows accepts no strides and packs each slot tightly.
        for (u32 s = 0; s < SO_BUFFER_SLOT_COUNT; ++s) {
            strides[s] = slot_dwords[s] * 4u;
        }
    } else {
        if (highest_slot >= num_strides) {
            refuse(d3d11::E_INVALIDARG, "a used slot has no stride");
            return;
        }
        for (u32 s = 0; s < num_strides; ++s) {
            const auto stride = m_memory.read_u32(strides_addr + s * 4u);
            if (stride.is_err()) {
                refuse(d3d11::E_INVALIDARG, "strides unreadable");
                return;
            }
            strides[s] = stride.value();
            if (strides[s] % 4u != 0 ||
                strides[s] > SO_BUFFER_MAX_STRIDE_IN_BYTES ||
                (slot_used[s] && strides[s] < slot_dwords[s] * 4u)) {
                refuse(d3d11::E_INVALIDARG, "stride not a multiple of 4, too "
                                            "large, or smaller than the "
                                            "declared vertex");
                return;
            }
        }
    }

    // The declaration is valid. Before reporting success, make sure this
    // shader can actually be captured (or, for a geometry program, lowered):
    // returning an object whose draws then write nothing is the silent
    // failure this whole path exists to avoid.
    GeometryShader shader{};
    if (geometry_program) {
        std::string why;
        u32 built = d3d11::S_OK;
        {
            const PureWorkUnlock unlocked(*this);
            built = build_geometry_program(bytes, container,
                                           rasterized_stream, shader, why);
        }
        if (const u32 hr = built; hr != d3d11::S_OK) {
            d3d11_diag("CreateGeometryShaderWithStreamOutput: %s",
                       why.c_str());
            refuse(hr, "the geometry program cannot be emulated");
            return;
        }
    } else if (container.stage == graphics::ShaderStage::Domain) {
        // The domain shader runs as the tessellation emulation's compute
        // pass, whose records the repack reads.
        graphics::DxbcTranslationOptions options{};
        options.emulate_tessellation = true;
        options.domain_to_compute = true;
        const auto spirv = graphics::translate_dxbc_to_spirv(container, options);
        if (!spirv.valid() || spirv.capture_registers.empty()) {
            d3d11_diag("CreateGeometryShaderWithStreamOutput: domain "
                       "translation failed: %s", spirv.failure.c_str());
            refuse(d3d11::E_NOTIMPL, "this domain shader cannot be run by "
                                     "the tessellation emulation");
            return;
        }
    } else {
        graphics::DxbcTranslationOptions options{};
        options.capture_vertex_outputs_to_buffer = true;
        const auto spirv = graphics::translate_dxbc_to_spirv(container, options);
        if (!spirv.valid() || !spirv.captures_vertex_outputs) {
            d3d11_diag("CreateGeometryShaderWithStreamOutput: capture "
                       "translation failed: %s", spirv.failure.c_str());
            refuse(d3d11::E_NOTIMPL, "this vertex shader cannot be captured");
            return;
        }
    }

    if (shader_out == 0) {
        // Validation-only call: S_FALSE, measured.
        m.set_return(1u);
        return;
    }
    if (!ensure_storage()) {
        refuse(d3d11::E_FAIL, "no object storage");
        return;
    }
    const GuestAddr address = allocate_object(m_geometry_shader_vtable);
    if (address == 0) {
        refuse(d3d11::E_FAIL, "object allocation failed");
        return;
    }
    shader.interface_address = address;
    const auto owner = m_interfaces.find(self);
    shader.device_owner = owner != m_interfaces.end() ? owner->second.owner : 0;
    shader.stream_out = std::move(entries);
    shader.stream_out_strides = strides;
    shader.stream_out_slots = stream_slots;
    shader.rasterized_stream = rasterized_stream;
    attach_shader_linkage(shader.linkage, bytes, container);
    register_class_linkage(linkage, container);
    const GuestAddr device_owner =
        owner != m_interfaces.end() ? owner->second.owner : 0;
    m_geometry_shaders.emplace(address, std::move(shader));
    acquire_parent(device_owner);
    if (m.write_ptr(shader_out, address).is_err()) {
        (void)release_object(address);
        refuse(d3d11::E_INVALIDARG, "out-parameter unwritable");
        return;
    }
    d3d11_diag("ID3D11Device::CreateGeometryShaderWithStreamOutput -> 0x%llx "
               "entries=%u raster=%u",
               static_cast<unsigned long long>(address), num_entries,
               rasterized_stream);
    m.set_return(d3d11::S_OK);
}

// -----------------------------------------------------------------------------
// Context state
// -----------------------------------------------------------------------------

void D3D11Manager::context_gs_set_shader(Marshaler& m, GuestAddr self) {
    // GSSetShader(pShader, ppClassInstances, NumClassInstances)
    set_stage_shader(self, LinkedStage::Geometry, m.arg_ptr(1), m.arg_ptr(2),
                     static_cast<u32>(m.arg(3)));
    m.set_return(0);
}

void D3D11Manager::context_so_set_targets(Marshaler& m, GuestAddr self) {
    // SOSetTargets(NumBuffers, ppSOTargets, pOffsets). Windows unbinds every
    // slot past NumBuffers (measured: set 2 then set 1 leaves slot 1 NULL).
    const u32 count = std::min(static_cast<u32>(m.arg(1)), SO_BUFFER_SLOT_COUNT);
    const GuestAddr targets = m.arg_ptr(2);
    const GuestAddr offsets = m.arg_ptr(3);
    const usize ptr_size = m_is_64bit ? 8u : 4u;
    std::array<GuestAddr, SO_BUFFER_SLOT_COUNT> next{};
    std::array<u32, SO_BUFFER_SLOT_COUNT> next_offsets{};
    for (u32 i = 0; i < count && targets != 0; ++i) {
        GuestAddr value = 0;
        if (!read_pointer(targets + static_cast<GuestAddr>(i) * ptr_size,
                          value) ||
            value == 0 || m_buffers.find(value) == m_buffers.end()) {
            continue;
        }
        // One buffer in two slots is dropped whole: the previous targets stay
        // (measured: "so set {bSoA,bSoA} duplicate get(2): bSoB -").
        for (u32 j = 0; j < i; ++j) {
            if (next[j] == value) {
                d3d11_diag("ID3D11DeviceContext::SOSetTargets: buffer 0x%llx "
                           "in two slots -> call dropped",
                           static_cast<unsigned long long>(value));
                m.set_return(0);
                return;
            }
        }
        next[i] = value;
        next_offsets[i] = 0;
        if (offsets != 0) {
            const auto read = m_memory.read_u32(offsets + i * 4u);
            if (read.is_ok()) next_offsets[i] = read.value();
        }
    }
    auto& state = m_context_states[self];
    std::array<BoundSubresources, SO_BUFFER_SLOT_COUNT> written{};
    usize written_count = 0;
    for (u32 i = 0; i < next.size(); ++i) {
        if (next[i] == 0) continue;
        auto& buffer = m_buffers.at(next[i]);
        // An explicit offset IS the new filled size, before anything is
        // written; -1 appends to whatever the buffer already holds. The size
        // lives on the GPU, so the new one is a copy recorded in command
        // order: draws before this call still append at the old size.
        // A deferred context sets it when its list executes (the replayed
        // call), not while recording.
        if (next_offsets[i] != SO_APPEND_OFFSET &&
            !is_deferred_context(self)) {
            graphics::BackendBuffer* word = stream_out_filled_word(buffer);
            graphics::BackendBufferDesc desc{};
            desc.byte_width = 16;
            desc.shader_resource = true;
            const std::array<u32, 4> value{next_offsets[i], 0, 0, 0};
            const auto source = m_backend.create_buffer(desc, value.data());
            if (word == nullptr || !source ||
                !m_backend.copy_buffer_region(word, 0, source.get(), 0, 4)) {
                d3d11_unimplemented_once(
                    "SOSetTargets", "the stream-output offset could not be "
                                    "recorded on the GPU timeline");
            }
        }
        ++buffer.ref_count;
        written[written_count++] = buffer_subresources(next[i]);
    }
    const auto previous = state.stream_out_targets;
    state.stream_out_targets = next;
    for (const GuestAddr old : previous) {
        if (old != 0) (void)release_buffer(old);
    }
    // An SO target is an output: it unbinds the same buffer's vertex/index
    // buffer and SRVs on every stage (measured, hazard rows n/n2 and the
    // follow-up "so bSI after ib bSI"), and any UAV of it.
    for (usize i = 0; i < written_count; ++i) {
        unbind_outputs_overlapping(state, written[i],
                                   kOutputMergerUavs | kOutputComputeUavs);
    }
    unbind_inputs_overlapping(
        state, std::span<const BoundSubresources>(written.data(), written_count));
    m.set_return(0);
}

void D3D11Manager::context_so_get_targets(Marshaler& m, GuestAddr self) {
    // SOGetTargets(NumBuffers, ppSOTargets)
    const u32 count = static_cast<u32>(m.arg(1));
    const GuestAddr out = m.arg_ptr(2);
    const usize ptr_size = m_is_64bit ? 8u : 4u;
    const auto& targets = m_context_states[self].stream_out_targets;
    for (u32 i = 0; i < count && out != 0; ++i) {
        const GuestAddr value = i < targets.size() ? targets[i] : 0;
        if (value != 0) (void)retain_object(value);
        (void)m.write_ptr(out + static_cast<GuestAddr>(i) * ptr_size, value);
    }
    m.set_return(0);
}

void D3D11Manager::context_draw_auto(GuestAddr self) {
    auto& state = m_context_states[self];
    const GuestAddr source = state.vertex_buffers[0];
    const auto buffer = m_buffers.find(source);
    if (source == 0 || buffer == m_buffers.end()) {
        d3d11_diag("ID3D11DeviceContext::DrawAuto: no vertex buffer in IA "
                   "slot 0 -> nothing drawn");
        return;
    }
    // Windows: (filled - offset) / stride vertices, 0 for a buffer never
    // streamed to, SV_VertexID from 0 (d3d11_stream_out_probe). The filled
    // size is on the GPU, so the count is too: a kernel turns it into the
    // argument block of a non-indexed indirect draw, in command order.
    graphics::BackendBuffer* filled = stream_out_filled_word(buffer->second);
    if (filled == nullptr || !ensure_stream_out_pipelines()) {
        d3d11_unimplemented_once("DrawAuto", "the filled-size word or the "
                                             "DrawAuto kernel is unavailable");
        return;
    }
    graphics::BackendBufferDesc desc{};
    desc.byte_width = 16;
    desc.shader_resource = true;
    desc.unordered_access = true;
    const std::shared_ptr<graphics::BackendBuffer> args =
        m_backend.create_buffer(desc, nullptr);
    if (!args) {
        d3d11_unimplemented_once("DrawAuto", "no argument buffer");
        return;
    }
    const std::array<u32, 2> constants{state.vertex_buffer_offsets[0],
                                       state.vertex_buffer_strides[0]};
    graphics::BackendDispatchCall call{};
    call.pipeline = m_stream_out_drawauto.get();
    call.group_count_x = call.group_count_y = call.group_count_z = 1;
    call.constants = constants.data();
    call.constants_size = sizeof(constants);
    call.buffers[graphics::SO_DRAWAUTO_FILLED_INDEX] = filled;
    call.buffers[graphics::SO_DRAWAUTO_ARGS_INDEX] = args.get();
    if (!m_backend.dispatch(call)) {
        d3d11_unimplemented_once("DrawAuto", "the DrawAuto kernel could not "
                                             "be dispatched");
        return;
    }
    note_gpu_use(buffer->second);
    d3d11_diag("ID3D11DeviceContext::DrawAuto offset=%u stride=%u -> "
               "GPU-counted vertices",
               state.vertex_buffer_offsets[0], state.vertex_buffer_strides[0]);
    // The argument buffer stays alive until the GPU is done with it: the
    // backend retains what a recorded draw references.
    execute_draw(self, false, 0, 0, 0, 1, 0, 0, 0, nullptr,
                 IndirectDraw{args.get(), 0});
}

// -----------------------------------------------------------------------------
// The GPU accounting
// -----------------------------------------------------------------------------

graphics::BackendBuffer* D3D11Manager::stream_out_filled_word(Buffer& buffer) {
    if (!buffer.so_filled) {
        graphics::BackendBufferDesc desc{};
        desc.byte_width = 16;
        desc.shader_resource = true;
        desc.unordered_access = true;
        const std::array<u32, 4> zero{};
        buffer.so_filled = m_backend.create_buffer(desc, zero.data());
    }
    return buffer.so_filled.get();
}

bool D3D11Manager::ensure_stream_out_pipelines() {
    const auto build = [&](std::unique_ptr<graphics::BackendComputePipeline>& out,
                           const char* entry, u64 key) {
        if (out) return out->valid();
        graphics::BackendComputePipelineDesc pd{};
        pd.compute_entry_point = entry;
        pd.compute_shader_msl = graphics::stream_out_repack_msl();
        pd.cache_key = key;
        out = m_backend.create_compute_pipeline(pd);
        return out && out->valid();
    };
    return build(m_stream_out_repack, graphics::kStreamOutRepackEntryPoint,
                 0x50AE9ACC02ull) &&
           build(m_stream_out_prepare, graphics::kStreamOutPrepareEntryPoint,
                 0x50AE9ACC03ull) &&
           build(m_stream_out_compact, graphics::kStreamOutCompactEntryPoint,
                 0x50AE9ACC04ull) &&
           build(m_stream_out_drawauto, graphics::kStreamOutDrawAutoEntryPoint,
                 0x50AE9ACC05ull);
}

std::shared_ptr<graphics::BackendBuffer> D3D11Manager::stream_out_stats_record(
    GuestAddr context) {
    graphics::BackendBufferDesc desc{};
    desc.byte_width = graphics::SO_STATS_WORDS * sizeof(u32);
    desc.shader_resource = true;
    desc.unordered_access = true;
    const std::array<u32, graphics::SO_STATS_WORDS> zero{};
    if (!stream_out_queries_building(context)) {
        // Nobody reads it: the kernel still needs somewhere to add to.
        if (!m_stream_out_scratch_stats) {
            m_stream_out_scratch_stats = m_backend.create_buffer(desc, zero.data());
        }
        return m_stream_out_scratch_stats;
    }
    return m_backend.create_buffer(desc, zero.data());
}

bool D3D11Manager::stream_out_pass(
    GuestAddr context, const StreamOutPass& pass,
    const std::shared_ptr<graphics::BackendBuffer>& stats) {
    auto& state = m_context_states[context];
    if (!stats || !ensure_stream_out_pipelines()) return false;
    // A slot of this stream's declaration with no buffer bound -- or no
    // target bound at all -- writes nothing for the stream and still counts
    // the storage it needed (measured: d3d11_stream_out_probe 0 of 3;
    // d3d11_gsso_probe, the other streams unaffected).
    bool any_target = false;
    for (const GuestAddr target : state.stream_out_targets) {
        any_target |= target != 0;
    }
    bool write_nothing = !any_target;
    for (u32 slot = 0; slot < SO_BUFFER_SLOT_COUNT; ++slot) {
        if ((pass.slot_mask & (1u << slot)) != 0 &&
            m_buffers.find(state.stream_out_targets[slot]) == m_buffers.end()) {
            write_nothing = true;
        }
    }
    const u32 slot_mask = write_nothing ? 0u : pass.slot_mask;

    graphics::SoPrepareParams params{};
    params.primitives = pass.primitives;
    params.count_mode = pass.count_mode;
    params.vertices_per_primitive = pass.vertices_per_primitive;
    params.stream = pass.stream;
    params.slot_mask = slot_mask;
    params.write_nothing = write_nothing ? 1u : 0u;
    params.strides = pass.strides;
    graphics::BackendBufferDesc op_desc{};
    op_desc.byte_width = graphics::SO_OP_WORDS * sizeof(u32);
    op_desc.shader_resource = true;
    op_desc.unordered_access = true;
    const auto op = m_backend.create_buffer(op_desc, nullptr);
    if (!op) return false;

    graphics::BackendDispatchCall prepare{};
    prepare.pipeline = m_stream_out_prepare.get();
    prepare.group_count_x = prepare.group_count_y = prepare.group_count_z = 1;
    // Unused arguments get a real buffer: the kernel does not read them, but
    // an unbound Metal argument is not something to rely on.
    prepare.buffers[graphics::SO_PREPARE_COUNT_INDEX] =
        pass.count != nullptr ? pass.count : op.get();
    prepare.buffer_offsets[graphics::SO_PREPARE_COUNT_INDEX] =
        pass.count != nullptr ? pass.count_offset : 0u;
    for (u32 slot = 0; slot < SO_BUFFER_SLOT_COUNT; ++slot) {
        graphics::BackendBuffer* word = op.get();
        if ((slot_mask & (1u << slot)) != 0) {
            auto& target = m_buffers.at(state.stream_out_targets[slot]);
            params.sizes[slot] = target.desc.byte_width;
            word = stream_out_filled_word(target);
            if (word == nullptr) return false;
        }
        prepare.buffers[graphics::SO_PREPARE_FILLED_BASE + slot] = word;
    }
    prepare.buffers[graphics::SO_PREPARE_OP_INDEX] = op.get();
    prepare.buffers[graphics::SO_PREPARE_STATS_INDEX] = stats.get();
    prepare.constants = &params;
    prepare.constants_size = sizeof(params);
    if (!m_backend.dispatch(prepare)) return false;
    if (write_nothing || pass.max_vertices == 0) return true;

    graphics::StreamOutRepackDesc desc{};
    desc.index_mode = pass.index_mode;
    desc.first = pass.first;
    desc.base_vertex = pass.base_vertex;
    desc.vertices_per_primitive = pass.vertices_per_primitive;
    desc.strip = pass.strip;
    desc.record_vec4s = pass.record_vec4s;
    desc.entries = pass.entries;
    desc.strides = pass.strides;
    const auto words = graphics::build_stream_out_control(desc);
    graphics::BackendBufferDesc control_desc{};
    control_desc.byte_width = words.size() * sizeof(u32);
    control_desc.shader_resource = true;
    const auto control = m_backend.create_buffer(control_desc, words.data());
    if (!control) return false;

    graphics::BackendDispatchCall call{};
    call.pipeline = m_stream_out_repack.get();
    call.threads_per_group_x = graphics::SO_REPACK_THREADS_PER_GROUP;
    call.group_count_x =
        (pass.max_vertices + graphics::SO_REPACK_THREADS_PER_GROUP - 1) /
        graphics::SO_REPACK_THREADS_PER_GROUP;
    call.group_count_y = call.group_count_z = 1;
    call.buffers[graphics::SO_REPACK_CAPTURE_INDEX] = pass.records;
    call.buffer_offsets[graphics::SO_REPACK_CAPTURE_INDEX] = pass.records_offset;
    call.buffers[graphics::SO_REPACK_CONTROL_INDEX] = control.get();
    call.buffers[graphics::SO_REPACK_INDICES_INDEX] =
        pass.indices != nullptr ? pass.indices : control.get();
    call.buffer_offsets[graphics::SO_REPACK_INDICES_INDEX] =
        pass.indices != nullptr ? pass.indices_offset : 0u;
    call.buffers[graphics::SO_REPACK_OP_INDEX] = op.get();
    for (u32 slot = 0; slot < SO_BUFFER_SLOT_COUNT; ++slot) {
        graphics::BackendBuffer* target = control.get();
        if ((slot_mask & (1u << slot)) != 0) {
            auto& bound = m_buffers.at(state.stream_out_targets[slot]);
            if (bound.backend) target = bound.backend.get();
        }
        call.buffers[graphics::SO_REPACK_TARGET_BASE + slot] = target;
    }
    if (!m_backend.dispatch(call)) return false;
    for (u32 slot = 0; slot < SO_BUFFER_SLOT_COUNT; ++slot) {
        if ((slot_mask & (1u << slot)) == 0) continue;
        auto& target = m_buffers.at(state.stream_out_targets[slot]);
        target.gpu_newer = true;
        note_gpu_use(target);
    }
    return true;
}

// -----------------------------------------------------------------------------
// The streaming draw
// -----------------------------------------------------------------------------

bool D3D11Manager::execute_stream_out(GuestAddr context, bool indexed,
                                      u32 count, u32 start_location,
                                      i32 base_vertex, u32 instance_count,
                                      u32 first_instance,
                                      const IndirectDraw& indirect) {
    auto& state = m_context_states[context];
    const auto gs_it = m_geometry_shaders.find(state.geometry_shader);
    if (gs_it == m_geometry_shaders.end()) return true;
    // Copied: execute_draw below must not be able to invalidate what the loop
    // reads through an iterator into the shader map.
    const std::vector<StreamOutDeclEntry> declaration = gs_it->second.stream_out;
    const std::array<u32, 4> strides = gs_it->second.stream_out_strides;
    const u32 slot_mask = gs_it->second.stream_out_slots[0];
    // A vertex shader produces stream 0 only.
    const bool rasterize = gs_it->second.rasterized_stream == 0;

    const auto decline = [&](const char* why) {
        d3d11_diag("stream output declined, nothing written: %s", why);
        return rasterize;
    };
    if (indirect.active()) {
        return execute_stream_out_indirect(context, indexed, indirect) &&
               rasterize;
    }
    const StreamTopology topology = stream_topology(state.primitive_topology);
    if (topology.vertices_per_primitive == 0) {
        return decline("topology is adjacency, patch or undefined");
    }
    u32 primitives = graphics::stream_out_primitive_count(
        topology.vertices_per_primitive, topology.strip, count);
    // An indexed draw is assembled on the CPU: strip cuts restart a strip and
    // a list primitive holding the cut value is dropped (measured,
    // d3d11_strip_order_probe), so the index data decides the count.
    std::vector<u32> assembled;
    std::vector<u32> capture_indices;
    if (indexed) {
        CaptureExtent early{};
        std::vector<u32> indices;
        if (capture_extent(state, true, count, start_location, base_vertex,
                           early) != nullptr ||
            !draw_indices(state, true, count, early, indices)) {
            return decline("the index data is not readable on the CPU");
        }
        const u32 cut =
            state.index_buffer_format == FORMAT_R16_UINT ? 0xFFFFu
                                                         : 0xFFFFFFFFu;
        assembled = graphics::assemble_primitive_vertices(
            indices, true, cut, base_vertex, topology.vertices_per_primitive,
            topology.strip ? graphics::PrimitiveAssembly::Strip
                           : graphics::PrimitiveAssembly::List);
        primitives = static_cast<u32>(assembled.size() /
                                      topology.vertices_per_primitive);
        for (const u32 index : indices) {
            if (index != cut) capture_indices.push_back(index);
        }
    }
    const auto stats = stream_out_stats_record(context);
    if (!stats) return decline("no stats record");
    StreamOutPass pass{};
    pass.stream = 0;
    pass.strides = strides;
    pass.slot_mask = slot_mask;
    pass.vertices_per_primitive = topology.vertices_per_primitive;
    pass.primitives = primitives;

    // With nothing to write to, only the storage the draw needed counts.
    bool any_target = false;
    for (const GuestAddr target : state.stream_out_targets) {
        any_target |= target != 0;
    }
    bool slots_bound = true;
    for (u32 slot = 0; slot < SO_BUFFER_SLOT_COUNT; ++slot) {
        if ((slot_mask & (1u << slot)) != 0 &&
            m_buffers.find(state.stream_out_targets[slot]) == m_buffers.end()) {
            slots_bound = false;
        }
    }
    if (!any_target || !slots_bound || primitives == 0) {
        for (u32 instance = 0; instance < instance_count; ++instance) {
            if (!stream_out_pass(context, pass, stats)) {
                return decline("the stream-output accounting could not be "
                               "recorded");
            }
        }
        note_stream_out_stats(context, stats);
        return rasterize;
    }

    auto vs = m_vertex_shaders.find(state.vertex_shader);
    if (vs == m_vertex_shaders.end()) return decline("no vertex shader");
    if (!ensure_vertex_capture(vs->second)) {
        return decline("the vertex shader cannot be captured");
    }
    const std::vector<u32> registers = vs->second.capture_registers;
    const u32 record_vec4s = static_cast<u32>(registers.size());

    for (const auto& e : declaration) {
        graphics::StreamOutRepackEntry out{};
        out.slot = e.slot;
        out.dst_dword = e.dst_dword;
        out.start_component = e.start_component;
        out.component_count = e.component_count;
        out.src_record = graphics::STREAM_OUT_GAP;
        if (!e.gap) {
            const auto found =
                std::find(registers.begin(), registers.end(), e.output_register);
            if (found == registers.end()) {
                d3d11_diag("stream output: o%u is declared but the bound vertex "
                           "shader does not write it; skipped",
                           e.output_register);
                continue;
            }
            out.src_record = static_cast<u32>(found - registers.begin());
        }
        pass.entries.push_back(out);
    }

    CaptureExtent extent{};
    if (const char* why = capture_extent(state, indexed, count, start_location,
                                         base_vertex, extent)) {
        return decline(why);
    }
    const auto index_mode =
        static_cast<graphics::StreamOutIndexMode>(extent.index_mode);
    graphics::BackendBuffer* const index_backend = extent.index_backend;
    const u32 first = extent.first;
    // Every vertex the capture runs writes a record, trailing ones no
    // primitive uses included, so the draw's whole range sizes the buffer.
    const u64 max_vertex =
        assembled.empty()
            ? extent.max_vertex
            : std::max<u64>(extent.max_vertex,
                            *std::max_element(assembled.begin(),
                                              assembled.end()));
    const u64 record_bytes = (max_vertex + 1) * record_vec4s * 16u;
    if (record_bytes * instance_count > MAX_CAPTURE_BYTES) {
        return decline("capture would exceed the size limit");
    }

    const u32 vpp = topology.vertices_per_primitive;
    if (!m_stream_out_target) {
        graphics::BackendTexture2DDesc td{};
        td.width = td.height = 1;
        td.mip_levels = td.array_size = 1;
        td.pixel_format = graphics::PixelFormat::RGBA8Unorm;
        td.render_target = true;
        m_stream_out_target = m_backend.create_texture_2d(td, nullptr, 0);
    }
    if (!m_stream_out_target || !ensure_stream_out_pipelines()) {
        return decline("stream-out pipeline unavailable");
    }

    graphics::BackendBufferDesc capture_desc{};
    capture_desc.byte_width = static_cast<usize>(record_bytes * instance_count);
    capture_desc.shader_resource = true;
    capture_desc.unordered_access = true;
    auto capture = m_backend.create_buffer(capture_desc, nullptr);
    if (!capture) return decline("capture buffer allocation failed");
    // Indexed: the capture runs through the draw's index values minus cuts,
    // and the repack reads the assembled vertex ids as 32-bit "indices".
    std::unique_ptr<graphics::BackendBuffer> assembled_ids;
    std::unique_ptr<graphics::BackendBuffer> capture_index_buffer;
    if (indexed) {
        graphics::BackendBufferDesc ids_desc{};
        ids_desc.shader_resource = true;
        ids_desc.byte_width = assembled.size() * sizeof(u32);
        assembled_ids = m_backend.create_buffer(ids_desc, assembled.data());
        ids_desc.index_buffer = true;
        ids_desc.byte_width = capture_indices.size() * sizeof(u32);
        capture_index_buffer =
            m_backend.create_buffer(ids_desc, capture_indices.data());
        if (!assembled_ids || !capture_index_buffer) {
            return decline("primitive assembly buffers could not be allocated");
        }
    }

    pass.records = capture.get();
    pass.record_vec4s = record_vec4s;
    pass.index_mode = indexed ? graphics::StreamOutIndexMode::Uint32 : index_mode;
    pass.first = indexed ? 0u : first;
    pass.base_vertex = 0;
    // Assembled ids are already a list in stream-output order.
    pass.strip = indexed ? false : topology.strip;
    pass.indices = assembled_ids ? assembled_ids.get() : index_backend;
    pass.max_vertices = primitives * vpp;
    for (u32 instance = 0; instance < instance_count; ++instance) {
        DrawVariant request{};
        request.kind = DrawVariant::Kind::Capture;
        request.buffer = capture.get();
        request.offset = static_cast<unsigned>(record_bytes * instance);
        request.target = m_stream_out_target.get();
        request.capture_indices = capture_index_buffer.get();
        request.capture_index_count = static_cast<u32>(capture_indices.size());
        // SV_InstanceID = instance; execute_draw places the per-instance
        // data (StartInstanceLocation + instance / step) from the bias.
        request.instance_id_bias = instance;
        execute_draw(context, indexed, count, start_location, base_vertex, 1,
                     first_instance, 0, 0, &request);
        if (!request.submitted) {
            d3d11_diag("stream output: the capture draw was not submitted; "
                       "nothing written");
            break;
        }
        pass.records_offset = request.offset;
        if (!stream_out_pass(context, pass, stats)) {
            d3d11_diag("stream output: the accounting or repack could not be "
                       "recorded");
            break;
        }
    }
    note_stream_out_stats(context, stats);
    return rasterize;
}

void D3D11Manager::stream_out_geometry_expansion(
    GuestAddr context, const GeometryStreamOutput& so,
    graphics::BackendBuffer* expanded, graphics::BackendBuffer* counter,
    const std::shared_ptr<graphics::BackendBuffer>& stats) {
    const auto decline = [](const char* why) {
        d3d11_diag("geometry stream output declined, nothing written: %s",
                   why);
    };
    if (expanded == nullptr || counter == nullptr || so.invocations == 0 ||
        so.vertices_per_invocation == 0 || !stats) {
        return decline("no expansion");
    }
    if (!ensure_stream_out_pipelines()) {
        return decline("stream-out pipeline unavailable");
    }
    // Each stream separately: its vertices in D3D's order -- by invocation
    // (input primitive, then instance), then emission order -- come from
    // the per-invocation counts pass B published, compacted on the GPU;
    // how many fit is the accounting's (D3D11.3 20.4.10: a full stream
    // does not stop the others).
    for (u32 stream = 0; stream < 4; ++stream) {
        const u32 slots = so.stream_slots[stream];
        bool declared = false;
        for (const auto& e : so.entries) declared |= e.stream == stream;
        if (!declared) continue;
        StreamOutPass pass{};
        pass.stream = stream;
        pass.strides = so.strides;
        pass.slot_mask = slots;
        pass.vertices_per_primitive = so.vertices_per_primitive;
        for (const auto& e : so.entries) {
            if (e.stream != stream) continue;
            graphics::StreamOutRepackEntry out{};
            out.slot = e.slot;
            out.dst_dword = e.dst_dword;
            out.start_component = e.start_component;
            out.component_count = e.component_count;
            out.src_record = graphics::STREAM_OUT_GAP;
            if (!e.gap) {
                const auto found = std::find(so.record_registers.begin(),
                                             so.record_registers.end(),
                                             e.output_register);
                if (found == so.record_registers.end()) continue;
                out.src_record =
                    static_cast<u32>(found - so.record_registers.begin());
            }
            pass.entries.push_back(out);
        }
        // The order list: every written vertex slot of this stream.
        const u64 max_vertices = so.invocations * so.vertices_per_invocation;
        graphics::BackendBufferDesc desc{};
        desc.shader_resource = true;
        desc.unordered_access = true;
        desc.byte_width = static_cast<usize>(std::max<u64>(max_vertices, 1) * 4u);
        const auto order = m_backend.create_buffer(desc, nullptr);
        desc.byte_width = 16;
        const auto total = m_backend.create_buffer(desc, nullptr);
        if (!order || !total) return decline("compaction buffers");
        graphics::SoCompactParams compact{};
        compact.invocations = static_cast<u32>(so.invocations);
        compact.count_mode = so.invocation_count != nullptr ? 1u : 0u;
        compact.capacity = so.vertices_per_invocation;
        compact.region_base = stream * so.invocation_stride *
                              so.vertices_per_invocation;
        graphics::BackendDispatchCall call{};
        call.pipeline = m_stream_out_compact.get();
        call.group_count_x = call.group_count_y = call.group_count_z = 1;
        call.threads_per_group_x = graphics::SO_COMPACT_THREADS;
        call.constants = &compact;
        call.constants_size = sizeof(compact);
        call.buffers[graphics::SO_COMPACT_COUNT_INDEX] =
            so.invocation_count != nullptr
                ? const_cast<graphics::BackendBuffer*>(so.invocation_count)
                : total.get();
        call.buffer_offsets[graphics::SO_COMPACT_COUNT_INDEX] =
            so.invocation_count != nullptr ? so.invocation_count_offset : 0u;
        call.buffers[graphics::SO_COMPACT_COUNTS_INDEX] = counter;
        call.buffer_offsets[graphics::SO_COMPACT_COUNTS_INDEX] =
            (graphics::kGsInvocationCountBase +
             stream * so.invocation_stride) * sizeof(u32);
        call.buffers[graphics::SO_COMPACT_ORDER_INDEX] = order.get();
        call.buffers[graphics::SO_COMPACT_TOTAL_INDEX] = total.get();
        if (!m_backend.dispatch(call)) return decline("compaction dispatch");

        pass.records = expanded;
        pass.record_vec4s = static_cast<u32>(so.record_registers.size());
        pass.index_mode = graphics::StreamOutIndexMode::Uint32;
        pass.indices = order.get();
        pass.count_mode = 2;  // the compaction total is in vertices
        pass.count = total.get();
        pass.max_vertices = static_cast<u32>(max_vertices);
        if (!stream_out_pass(context, pass, stats)) {
            return decline("the accounting or repack could not be recorded");
        }
    }
}

bool D3D11Manager::execute_stream_out_indirect(GuestAddr context,
                                               bool indexed,
                                               const IndirectDraw& indirect) {
    auto& state = m_context_states[context];
    const auto decline = [](const char* why) {
        d3d11_diag("stream output of an indirect draw declined, nothing "
                   "written: %s", why);
        return false;
    };
    const auto gs_it = m_geometry_shaders.find(state.geometry_shader);
    if (gs_it == m_geometry_shaders.end()) return false;
    const std::vector<StreamOutDeclEntry> declaration = gs_it->second.stream_out;
    const std::array<u32, 4> strides = gs_it->second.stream_out_strides;
    const u32 slot_mask = gs_it->second.stream_out_slots[0];
    const StreamTopology topology = stream_topology(state.primitive_topology);
    if (topology.vertices_per_primitive == 0) {
        return decline("topology is adjacency, patch or undefined");
    }
    auto vs = m_vertex_shaders.find(state.vertex_shader);
    if (vs == m_vertex_shaders.end()) return decline("no vertex shader");
    if (!ensure_vertex_capture(vs->second)) {
        return decline("the vertex shader cannot be captured");
    }
    const std::vector<u32> registers = vs->second.capture_registers;
    const u32 vpp = topology.vertices_per_primitive;
    IndirectCapture captured{};
    if (!capture_indirect(context, indexed, indirect, vpp,
                          topology.strip ? 1u : 0u,
                          static_cast<u32>(registers.size()),
                          ~u64{0}, captured)) {
        return false;
    }
    StreamOutPass pass{};
    pass.stream = 0;
    pass.strides = strides;
    pass.slot_mask = slot_mask;
    pass.vertices_per_primitive = vpp;
    for (const auto& e : declaration) {
        graphics::StreamOutRepackEntry out{};
        out.slot = e.slot;
        out.dst_dword = e.dst_dword;
        out.start_component = e.start_component;
        out.component_count = e.component_count;
        out.src_record = graphics::STREAM_OUT_GAP;
        if (!e.gap) {
            const auto found =
                std::find(registers.begin(), registers.end(), e.output_register);
            if (found == registers.end()) continue;
            out.src_record = static_cast<u32>(found - registers.begin());
        }
        pass.entries.push_back(out);
    }
    // The assembled keys are already a list in stream-output order,
    // instance-major (D3D writes instance 0's primitives first).
    pass.records = captured.capture.get();
    pass.records_offset = graphics::CAPTURE_HEADER_BYTES;
    pass.record_vec4s = static_cast<u32>(registers.size());
    pass.index_mode = graphics::StreamOutIndexMode::Uint32;
    pass.indices = captured.ids.get();
    pass.count_mode = 1;
    pass.count = captured.resolved.get();
    pass.count_offset = graphics::IA_RESOLVED_PRIMITIVES * sizeof(u32);
    pass.max_vertices = captured.primitive_capacity * vpp;
    const auto stats = stream_out_stats_record(context);
    if (!stats || !stream_out_pass(context, pass, stats)) {
        return decline("the accounting or repack could not be recorded");
    }
    note_stream_out_stats(context, stats);
    return true;
}

// -----------------------------------------------------------------------------
// Shared by stream output and the geometry emulation
// -----------------------------------------------------------------------------

bool D3D11Manager::ensure_vertex_capture(VertexShader& vs) {
    if (vs.capture_state != 0) return vs.capture_state == 1;
    vs.capture_state = 2;
    if (vs.binding_layout.uses_descriptor_tables) {
        d3d11_diag("vertex capture: the vertex shader binds through "
                   "descriptor tables, which the capture variant cannot");
        return false;
    }
    const auto container =
        graphics::parse_dxbc(vs.bytecode.data(), vs.bytecode.size());
    if (!container.valid()) return false;
    graphics::DxbcTranslationOptions options{};
    options.capture_vertex_outputs_to_buffer = true;
    const auto spirv = graphics::translate_dxbc_to_spirv(container, options);
    if (!spirv.valid() || spirv.capture_registers.empty()) {
        d3d11_diag("vertex capture: translation failed: %s",
                   spirv.failure.c_str());
        return false;
    }
    vs.capture_registers = spirv.capture_registers;
    vs.stats_clip = stats_clip_layout(container.output_signature,
                                      vs.capture_registers, 0);
    vs.capture_state = 1;
    return true;
}

void D3D11Manager::poll_indirect_feedback() {
    auto& list = m_indirect_feedback;
    for (usize i = 0; i < list.size();) {
        const auto& entry = list[i];
        if (entry.generation != 0 &&
            !m_backend.generation_complete(entry.generation)) {
            ++i;
            continue;
        }
        std::array<u32, graphics::IA_RESOLVED_WORDS> words{};
        const bool read = entry.resolved &&
                          entry.resolved->read(0, words.data(), sizeof(words));
        const bool dropped = read && words[graphics::IA_RESOLVED_OVERFLOW] != 0;
        const bool at_limit = entry.record_capacity < m_indirect_record_capacity ||
                              m_indirect_record_capacity >= (1u << 26);
        if (dropped && at_limit) {
            // Already as large as the size limit allows: such draws cannot
            // be served (the capture would exceed MAX_CAPTURE_BYTES).
            d3d11_unimplemented_once(
                "Draw*Indirect",
                "a draw through the geometry / stream-output / tessellation "
                "emulation needs more captured vertices than the size limit "
                "allows; it is dropped");
        } else if (dropped &&
                   m_indirect_record_capacity <= entry.record_capacity) {
            // The GPU-written counts needed more than the passes were sized
            // for: that draw was dropped whole. Size the next ones larger.
            const u32 next = m_indirect_record_capacity * 4u;
            std::fprintf(stderr,
                         "[vitreo] d3d11: an indirect draw through the "
                         "geometry / stream-output / tessellation emulation "
                         "needed more than %u captured vertices; that draw "
                         "was dropped, capacity now %u\n",
                         m_indirect_record_capacity, next);
            m_indirect_record_capacity = next;
        }
        list.erase(list.begin() + static_cast<std::ptrdiff_t>(i));
    }
}

bool D3D11Manager::capture_indirect(GuestAddr context, bool indexed,
                                    const IndirectDraw& indirect, u32 vpp,
                                    u32 assembly, u32 record_vec4s,
                                    u64 max_primitives, IndirectCapture& out) {
    auto& state = m_context_states[context];
    const auto decline = [](const char* why) {
        d3d11_diag("indirect draw through an emulation: nothing drawn: %s",
                   why);
        return false;
    };
    poll_indirect_feedback();
    if (record_vec4s == 0) return decline("the vertex shader writes nothing");
    const u64 record_bytes = static_cast<u64>(record_vec4s) * 16u;
    const u32 record_capacity = static_cast<u32>(std::min<u64>(
        m_indirect_record_capacity, MAX_CAPTURE_BYTES / record_bytes));
    const u32 primitive_capacity = static_cast<u32>(
        std::min<u64>({max_primitives, record_capacity,
                       MAX_CAPTURE_BYTES / (4u * std::max(vpp, 1u))}));
    if (record_capacity == 0 || primitive_capacity == 0) {
        return decline("the capture cannot hold one vertex");
    }
    if (!m_ia_resolve) {
        graphics::BackendComputePipelineDesc pd{};
        pd.compute_entry_point = graphics::kIaResolveEntryPoint;
        pd.compute_shader_msl = graphics::ia_resolve_msl();
        pd.cache_key = 0x1A7E501Full;
        m_ia_resolve = m_backend.create_compute_pipeline(pd);
    }
    if (!m_ia_resolve || !m_ia_resolve->valid()) {
        return decline("the resolve kernel is unavailable");
    }
    if (!m_stream_out_target) {
        graphics::BackendTexture2DDesc td{};
        td.width = td.height = 1;
        td.mip_levels = td.array_size = 1;
        td.pixel_format = graphics::PixelFormat::RGBA8Unorm;
        td.render_target = true;
        m_stream_out_target = m_backend.create_texture_2d(td, nullptr, 0);
    }
    if (!m_stream_out_target) return decline("capture target unavailable");

    graphics::BackendBufferDesc desc{};
    desc.shader_resource = true;
    desc.unordered_access = true;
    desc.byte_width = graphics::IA_RESOLVED_WORDS * sizeof(u32);
    out.resolved = m_backend.create_buffer(desc, nullptr);
    desc.byte_width = static_cast<usize>(primitive_capacity) *
                      std::max(vpp, 1u) * sizeof(u32);
    out.ids = m_backend.create_buffer(desc, nullptr);
    desc.byte_width = graphics::CAPTURE_HEADER_BYTES +
                      static_cast<usize>(record_capacity) * record_bytes;
    out.capture = m_backend.create_buffer(desc, nullptr);
    if (!out.resolved || !out.ids || !out.capture) {
        return decline("resolve buffers could not be allocated");
    }
    out.record_capacity = record_capacity;
    out.primitive_capacity = primitive_capacity;

    graphics::IaResolveParams params{};
    params.indexed = indexed ? 1u : 0u;
    params.vertices_per_primitive = vpp;
    params.assembly = assembly;
    params.record_capacity = record_capacity;
    params.primitive_capacity = primitive_capacity;
    graphics::BackendBuffer* index_backend = nullptr;
    if (indexed) {
        const auto ib = m_buffers.find(state.index_buffer);
        if (ib == m_buffers.end() || !ib->second.backend) {
            return decline("no index buffer");
        }
        const bool wide = state.index_buffer_format != FORMAT_R16_UINT;
        const u32 index_size = wide ? 4u : 2u;
        params.index_32bit = wide ? 1u : 0u;
        params.index_first = state.index_buffer_offset / index_size;
        const u32 bytes = ib->second.desc.byte_width;
        params.index_elements =
            bytes > state.index_buffer_offset
                ? (bytes - state.index_buffer_offset) / index_size
                : 0u;
        index_backend = ib->second.backend.get();
    }
    graphics::BackendDispatchCall call{};
    call.pipeline = m_ia_resolve.get();
    call.group_count_x = call.group_count_y = call.group_count_z = 1;
    call.threads_per_group_x = graphics::IA_RESOLVE_THREADS;
    call.constants = &params;
    call.constants_size = sizeof(params);
    call.buffers[graphics::IA_RESOLVE_ARGS_INDEX] =
        const_cast<graphics::BackendBuffer*>(indirect.args);
    call.buffer_offsets[graphics::IA_RESOLVE_ARGS_INDEX] = indirect.offset;
    call.buffers[graphics::IA_RESOLVE_INDICES_INDEX] =
        index_backend != nullptr ? index_backend : out.resolved.get();
    call.buffers[graphics::IA_RESOLVE_IDS_INDEX] = out.ids.get();
    call.buffers[graphics::IA_RESOLVE_OUT_INDEX] = out.resolved.get();
    call.buffers[graphics::IA_RESOLVE_CAPTURE_INDEX] = out.capture.get();
    if (!m_backend.dispatch(call)) return decline("resolve dispatch failed");
    m_indirect_feedback.push_back(
        {out.resolved, m_backend.recording_generation(), record_capacity});

    // A: the capture, every instance in one draw keyed draw-relative, with
    // the argument block the resolve copied (instances zero when dropped).
    DrawVariant capture_draw{};
    capture_draw.kind = DrawVariant::Kind::Capture;
    capture_draw.buffer = out.capture.get();
    capture_draw.offset = 0;
    capture_draw.target = m_stream_out_target.get();
    capture_draw.capture_relative = true;
    execute_draw(context, indexed, 0, 0, 0, 1, 0, 0, 0, &capture_draw,
                 IndirectDraw{out.resolved.get(),
                              graphics::IA_RESOLVED_CAPTURE_ARGS *
                                  static_cast<u32>(sizeof(u32))});
    if (!capture_draw.submitted) {
        return decline("the capture draw was not submitted");
    }
    return true;
}

// The capture is keyed by the vertex id Metal generates, which includes
// StartVertexLocation / BaseVertexLocation, so it is sized by the largest one
// the draw reaches. For an indexed draw that is read from the index buffer's
// CPU copy -- which is only trustworthy when the GPU cannot write the buffer.
bool D3D11Manager::draw_indices(const ContextState& state, bool indexed,
                                u32 count, const CaptureExtent& extent,
                                std::vector<u32>& out) {
    out.clear();
    out.reserve(count);
    if (!indexed) {
        for (u32 i = 0; i < count; ++i) out.push_back(extent.first + i);
        return true;
    }
    const auto ib = m_buffers.find(state.index_buffer);
    if (ib == m_buffers.end()) return false;
    const u32 index_size = extent.index_mode == 2u ? 4u : 2u;
    const auto& shadow = ib->second.shadow;
    for (u32 i = 0; i < count; ++i) {
        const usize at = (static_cast<usize>(extent.first) + i) * index_size;
        if (at + index_size > shadow.size()) return false;
        u32 value = 0;
        std::memcpy(&value, shadow.data() + at, index_size);
        out.push_back(value);
    }
    return true;
}

const char* D3D11Manager::capture_extent(const ContextState& state,
                                         bool indexed, u32 count,
                                         u32 start_location, i32 base_vertex,
                                         CaptureExtent& out) {
    out = CaptureExtent{};
    out.first = start_location;
    if (!indexed) {
        out.max_vertex = static_cast<u64>(start_location) + count - 1;
        return nullptr;
    }
    const auto ib = m_buffers.find(state.index_buffer);
    if (ib == m_buffers.end() || !ib->second.backend) return "no index buffer";
    if ((ib->second.desc.bind_flags &
         (BIND_UNORDERED_ACCESS | BIND_STREAM_OUTPUT)) != 0) {
        return "the index buffer is GPU-writable, so its CPU copy cannot size "
               "the capture";
    }
    const bool wide = state.index_buffer_format != FORMAT_R16_UINT;
    const u32 index_size = wide ? 4u : 2u;
    out.index_mode = wide ? 2u : 1u;
    out.index_backend = ib->second.backend.get();
    out.first = state.index_buffer_offset / index_size + start_location;
    const auto& shadow = ib->second.shadow;
    // The strip-cut value (0xFFFF / 0xFFFFFFFF, D3D11.3 8.12) names no
    // vertex: the capture runs the other indices only, so it sizes nothing.
    const u32 cut = wide ? 0xFFFFFFFFu : 0xFFFFu;
    u32 max_index = 0;
    for (u32 i = 0; i < count; ++i) {
        const usize at = (static_cast<usize>(out.first) + i) * index_size;
        if (at + index_size > shadow.size()) break;
        u32 value = 0;
        std::memcpy(&value, shadow.data() + at, index_size);
        if (value != cut) max_index = std::max(max_index, value);
    }
    const i64 max_vertex = static_cast<i64>(max_index) + base_vertex;
    out.max_vertex = max_vertex < 0 ? 0u : static_cast<u64>(max_vertex);
    return nullptr;
}

}  // namespace vitreo::winapi
