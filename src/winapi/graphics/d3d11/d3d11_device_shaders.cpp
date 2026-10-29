// SPDX-License-Identifier: LGPL-2.1-only

/// @file d3d11_device_shaders.cpp
/// @brief ID3D11Device shader creators: CreateInputLayout, the
///        Create*Shader methods and CreateClassLinkage (vtable slots
///        11-19), plus the bytecode checks they share.
///
/// Split out of d3d11_device_methods.cpp; impl_DeviceMethod forwards the
/// slots here once it has found the device. The geometry, stream-out and
/// tessellation creators (d3d11_geometry.cpp, d3d11_stream_out.cpp,
/// d3d11_tessellation.cpp) call the bytecode checks too.

#include "core/memory/host_memory_floor.hpp"
#include "winapi/graphics/d3d11/d3d11.hpp"
#include "winapi/graphics/d3d11/d3d11_caps_log.hpp"
#include "winapi/graphics/common/gpu_event_signaler.hpp"
#include "winapi/graphics/d3d11/d3d11_internal.hpp"
#include "winapi/graphics/d3d11/d3d11_vertex_formats.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/error.hpp"
#include "graphics/contracts/backend_device.hpp"
#include "graphics/contracts/gpu_resource.hpp"
#include "graphics/shaders/dxbc_parser.hpp"
#include "core/memory/guest_memory.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <array>
#include <cstring>
#include <cstdio>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace vitreo::winapi {

void D3D11Manager::device_shader_method(Marshaler& m, usize slot,
                                        GuestAddr self) {
    switch (slot) {
    case 11: {
        // CreateInputLayout — Microsoft Learn: validates element descs against
        // a shader signature; ppInputLayout NULL returns S_FALSE on valid params.
        const GuestAddr elements_addr = m.arg_ptr(1);
        const u32 num_elements = static_cast<u32>(m.arg(2));
        const GuestAddr bytecode = m.arg_ptr(3);
        const u64 bytecode_length = m.arg(4);
        const GuestAddr layout_out = m.arg_ptr(5);
        if (layout_out != 0) (void)m.write_ptr(layout_out, 0);
        // More than D3D11_IA_VERTEX_INPUT_STRUCTURE_ELEMENT_COUNT (32) is
        // E_INVALIDARG. A vertex shader that consumes only system values
        // (SV_VertexID and the like) is matched by zero elements, but the
        // array itself must still be given: NumElements 0 with a NULL
        // pInputElementDescs is E_INVALIDARG, with a non-NULL one S_OK
        // (measured, d3d11_a05_context_state_probe "vs_id: 0 elements").
        if (num_elements > 32 || bytecode == 0 || bytecode_length == 0 ||
            elements_addr == 0) {
            m.set_return(d3d11::E_INVALIDARG);
            return;
        }
        // Learn: "The compiled shader code contains a input signature which
        // is validated against the array of elements." Any DXBC container
        // with an ISGN or ISG1 chunk serves -- a vertex shader, a
        // D3DGetInputSignatureBlob blob with no code at all, even a pixel
        // shader -- but the container must be intact: a bad hash, a length
        // short of its TotalSize, or no input signature is E_INVALIDARG
        // (all measured, d3d11_a05b_bind_rules_probe).
        std::vector<graphics::DxbcSignatureElement> signature;
        {
            constexpr u64 kMaxBytecode = 1u << 24;
            std::vector<u8> blob(static_cast<usize>(
                std::min<u64>(bytecode_length, kMaxBytecode)));
            if (bytecode_length > kMaxBytecode ||
                m_memory.read_bytes(bytecode, blob.data(), blob.size())
                    .is_err() ||
                !graphics::dxbc_container_verified(blob.data(), blob.size())) {
                m.set_return(d3d11::E_INVALIDARG);
                return;
            }
            auto container = graphics::parse_dxbc(blob.data(), blob.size());
            if (!container.has_input_signature) {
                m.set_return(d3d11::E_INVALIDARG);
                return;
            }
            signature = std::move(container.input_signature);
        }
        const usize elem_stride = m_is_64bit ? 32u : 28u;
        std::vector<InputLayoutElement> elements;
        elements.reserve(num_elements);
        for (u32 i = 0; i < num_elements; ++i) {
            const GuestAddr base = elements_addr + i * elem_stride;
            InputLayoutElement el{};
            // Learn D3D11_INPUT_ELEMENT_DESC.SemanticName is the HLSL
            // semantic matched against the shader input signature — not
            // the element-array index.
            GuestAddr name_addr = 0;
            if (m_is_64bit) {
                const auto name = m_memory.read_u64(base);
                if (name.is_err()) {
                    m.set_return(d3d11::E_INVALIDARG);
                    return;
                }
                name_addr = name.value();
            } else {
                const auto name = m_memory.read_u32(base);
                if (name.is_err()) {
                    m.set_return(d3d11::E_INVALIDARG);
                    return;
                }
                name_addr = name.value();
            }
            if (name_addr == 0) {
                m.set_return(d3d11::E_INVALIDARG);
                return;
            }
            std::array<char, 65> name_bytes{};
            usize name_len = 0;
            for (; name_len < name_bytes.size() - 1; ++name_len) {
                const auto ch = m_memory.read_u8(name_addr + name_len);
                if (ch.is_err()) {
                    m.set_return(d3d11::E_INVALIDARG);
                    return;
                }
                if (ch.value() == 0) break;
                name_bytes[name_len] = static_cast<char>(ch.value());
            }
            if (name_len == 0) {
                m.set_return(d3d11::E_INVALIDARG);
                return;
            }
            el.semantic_name.assign(name_bytes.data(), name_len);
            const GuestAddr fields = base + (m_is_64bit ? 8u : 4u);
            const auto sem_index = m_memory.read_u32(fields);
            const auto format = m_memory.read_u32(fields + 4);
            const auto slot = m_memory.read_u32(fields + 8);
            const auto offset = m_memory.read_u32(fields + 12);
            const auto slot_class = m_memory.read_u32(fields + 16);
            const auto step = m_memory.read_u32(fields + 20);
            if (sem_index.is_err() || format.is_err() || slot.is_err() ||
                offset.is_err() || slot_class.is_err() || step.is_err()) {
                m.set_return(d3d11::E_INVALIDARG);
                return;
            }
            el.semantic_index = sem_index.value();
            el.format = format.value();
            el.input_slot = slot.value();
            el.aligned_byte_offset = offset.value();
            el.input_slot_class = slot_class.value();
            el.instance_data_step_rate = step.value();
            // D3D11_INPUT_CLASSIFICATION is PER_VERTEX_DATA (0) or
            // PER_INSTANCE_DATA (1); Learn (D3D11_INPUT_ELEMENT_DESC):
            // "This value must be 0 for an element that contains per-vertex
            // data". Both measured E_INVALIDARG (class 2; per-vertex step 3).
            if (el.input_slot_class > 1 ||
                (el.input_slot_class == 0 && el.instance_data_step_rate != 0)) {
                m.set_return(d3d11::E_INVALIDARG);
                return;
            }
            // An explicit offset must be aligned to the format's size, capped
            // at 4 (measured: R8 at 3 and R32G32B32A32 at 4 are accepted;
            // R16 at 1, R8G8B8A8 at 1, R16G16 at 2, R32 at 2 and
            // R16G16B16A16 at 2 or 6 are E_INVALIDARG).
            const u32 element_bytes =
                d3d11::vertex_format_stride_from_dxgi(el.format);
            if (el.aligned_byte_offset != d3d11::kAppendAlignedElement &&
                element_bytes != 0 &&
                el.aligned_byte_offset % std::min(4u, element_bytes) != 0) {
                m.set_return(d3d11::E_INVALIDARG);
                return;
            }
            elements.push_back(el);
        }
        // Semantic names compare case-insensitively (measured: "position"
        // matches POSITION, and POSITION + position is a duplicate).
        const auto same_semantic = [](const std::string& a, u32 a_index,
                                      const std::string& b, u32 b_index) {
            if (a_index != b_index || a.size() != b.size()) return false;
            for (usize i = 0; i < a.size(); ++i) {
                if (std::tolower(static_cast<unsigned char>(a[i])) !=
                    std::tolower(static_cast<unsigned char>(b[i]))) {
                    return false;
                }
            }
            return true;
        };
        // One (name, index) twice is E_INVALIDARG even when the shader never
        // reads it (measured: duplicate POSITION0, an unused COLOR0 twice).
        for (usize a = 0; a < elements.size(); ++a) {
            for (usize b = a + 1; b < elements.size(); ++b) {
                if (same_semantic(elements[a].semantic_name,
                                  elements[a].semantic_index,
                                  elements[b].semantic_name,
                                  elements[b].semantic_index)) {
                    m.set_return(d3d11::E_INVALIDARG);
                    return;
                }
            }
        }
        // Every signature element needs a layout element, read by the shader
        // or not (measured: an unread TEXCOORD0 in the ISGN still has to be
        // supplied), except the values the input assembler generates itself:
        // D3D_NAME_VERTEX_ID (6), PRIMITIVE_ID (7) and INSTANCE_ID (8).
        // A pixel shader's SV_Position is D3D_NAME_POSITION, so it IS
        // required (measured). Extra layout elements, and width or
        // component-type mismatches, are accepted.
        for (const auto& input : signature) {
            if (input.system_value_type >= 6 && input.system_value_type <= 8) {
                continue;
            }
            bool supplied = false;
            for (const auto& el : elements) {
                if (same_semantic(input.semantic_name, input.semantic_index,
                                  el.semantic_name, el.semantic_index)) {
                    supplied = true;
                    break;
                }
            }
            if (!supplied) {
                d3d11_diag("ID3D11Device::CreateInputLayout: signature input "
                           "%s%u has no element -> E_INVALIDARG",
                           input.semantic_name.c_str(), input.semantic_index);
                m.set_return(d3d11::E_INVALIDARG);
                return;
            }
        }
        // Learn D3D11_INPUT_ELEMENT_DESC.AlignedByteOffset may be
        // D3D11_APPEND_ALIGNED_ELEMENT (0xffffffff): pack after the previous
        // element in the same input slot. Metal vertex descriptors need real
        // byte offsets — leaving 0xffffffff makes attribute N look missing.
        {
            d3d11::VertexLayoutPackingState packing{};
            for (auto& el : elements) {
                const u32 stride =
                    d3d11::vertex_format_stride_from_dxgi(el.format);
                u32 resolved_offset = 0;
                if (!d3d11::resolve_vertex_element_offset(
                        el.input_slot, el.aligned_byte_offset, stride,
                        packing, resolved_offset)) {
                    m.set_return(d3d11::E_INVALIDARG);
                    return;
                }
                el.aligned_byte_offset = resolved_offset;
            }
        }
        if (layout_out == 0) {
            m.set_return(1u); // S_FALSE — validation only
            return;
        }
        const GuestAddr owner = m_interfaces.find(self)->second.owner;
        const GuestAddr layout = create_input_layout(owner, elements);
        if (layout == 0 || m.write_ptr(layout_out, layout).is_err()) {
            if (layout != 0) (void)release_object(layout);
            m.set_return(d3d11::E_FAIL);
            return;
        }
        d3d11_diag("ID3D11Device::CreateInputLayout elements=%u -> 0x%llx",
                   num_elements, static_cast<unsigned long long>(layout));
        for (u32 i = 0; i < elements.size() && i < 8; ++i) {
            d3d11_diag(
                "  elem[%u] semantic=%s%u fmt=%u slot=%u offset=%u class=%u "
                "step=%u",
                i, elements[i].semantic_name.c_str(),
                elements[i].semantic_index, elements[i].format,
                elements[i].input_slot, elements[i].aligned_byte_offset,
                elements[i].input_slot_class,
                elements[i].instance_data_step_rate);
        }
        m.set_return(d3d11::S_OK);
        return;
    }
    case 12: {
        // CreateVertexShader
        const GuestAddr bytecode = m.arg_ptr(1);
        const u64 length = m.arg(2);
        const GuestAddr linkage = m.arg_ptr(3);
        const GuestAddr shader_out = m.arg_ptr(4);
        if (shader_out != 0) (void)m.write_ptr(shader_out, 0);
        // pClassLinkage: NULL or a linkage of this device.
        if (bytecode == 0 || length == 0 || length > (1u << 24) ||
            !valid_class_linkage(m_interfaces.find(self)->second.owner,
                                 linkage)) {
            m.set_return(d3d11::E_INVALIDARG);
            return;
        }
        std::vector<u8> bytes(static_cast<usize>(length));
        if (m_memory.read_bytes(bytecode, bytes.data(), bytes.size()).is_err()) {
            m.set_return(d3d11::E_INVALIDARG);
            return;
        }
        if (const char* why = shader_bytecode_refused(
                self, bytes.data(), bytes.size(),
                {graphics::ShaderStage::Vertex})) {
            d3d11_diag("ID3D11Device::CreateVertexShader -> E_INVALIDARG "
                       "(%s)", why);
            m.set_return(d3d11::E_INVALIDARG);
            return;
        }
        if (uav_requirements_unavailable(self, bytes.data(),
                                            bytes.size())) {
            d3d11_diag("ID3D11Device::CreateVertexShader -> E_INVALIDARG "
                       "(declares a UAV below feature level 11_1)");
            m.set_return(d3d11::E_INVALIDARG);
            return;
        }
        // A shader that declares interfaces needs a class linkage
        // (E_INVALIDARG without one; d3d11_classlink_probe [create]).
        if (requires_class_linkage(bytes) &&
            refuse_interface_shader(linkage, "CreateVertexShader") != d3d11::S_OK) {
            m.set_return(d3d11::E_INVALIDARG);
            return;
        }
        ShaderTranslation translation{};
        {
            const PureWorkUnlock unlocked(*this);
            translation = translate_shader(bytes.data(), bytes.size(),
                                           graphics::ShaderStage::Vertex);
        }
        if (shader_out == 0) {
            m.set_return(translation.translated ? 1u : d3d11::E_FAIL);
            return;
        }
        const GuestAddr owner = m_interfaces.find(self)->second.owner;
        const GuestAddr shader = create_vertex_shader(
            owner, bytes.data(), bytes.size(), false, &translation);
        if (shader != 0) {
            const auto container =
                graphics::parse_dxbc(bytes.data(), bytes.size());
            attach_shader_linkage(m_vertex_shaders.at(shader).linkage, bytes, container);
            register_class_linkage(linkage, container);
        }
        if (shader == 0) {
            d3d11_diag("ID3D11Device::CreateVertexShader length=%llu -> E_FAIL",
                       static_cast<unsigned long long>(length));
            m.set_return(d3d11::E_FAIL);
            return;
        }
        if (m.write_ptr(shader_out, shader).is_err()) {
            (void)release_object(shader);
            m.set_return(d3d11::E_POINTER);
            return;
        }
        d3d11_diag("ID3D11Device::CreateVertexShader -> 0x%llx length=%llu",
                   static_cast<unsigned long long>(shader),
                   static_cast<unsigned long long>(length));
        m.set_return(d3d11::S_OK);
        return;
    }
    case 13:
        // CreateGeometryShader(pBytecode, Length, pClassLinkage,
        //   ppGeometryShader). Metal has no geometry stage; the program runs
        // through the compute emulation (d3d11_geometry.cpp, 11.16).
        create_geometry_shader(m, self);
        return;
    case 14:
        // CreateGeometryShaderWithStreamOutput. Nine arguments after `this`;
        // the out-parameter is arg(9), not arg(8), which is pClassLinkage.
        create_geometry_shader_with_stream_output(m, self);
        return;
    case 15: {
        // CreatePixelShader
        const GuestAddr bytecode = m.arg_ptr(1);
        const u64 length = m.arg(2);
        const GuestAddr linkage = m.arg_ptr(3);
        const GuestAddr shader_out = m.arg_ptr(4);
        if (shader_out != 0) (void)m.write_ptr(shader_out, 0);
        // pClassLinkage: NULL or a linkage of this device.
        if (bytecode == 0 || length == 0 || length > (1u << 24) ||
            !valid_class_linkage(m_interfaces.find(self)->second.owner,
                                 linkage)) {
            m.set_return(d3d11::E_INVALIDARG);
            return;
        }
        std::vector<u8> bytes(static_cast<usize>(length));
        if (m_memory.read_bytes(bytecode, bytes.data(), bytes.size()).is_err()) {
            m.set_return(d3d11::E_INVALIDARG);
            return;
        }
        if (const char* why = shader_bytecode_refused(
                self, bytes.data(), bytes.size(),
                {graphics::ShaderStage::Fragment})) {
            d3d11_diag("ID3D11Device::CreatePixelShader -> E_INVALIDARG (%s)",
                       why);
            m.set_return(d3d11::E_INVALIDARG);
            return;
        }
        if (uav_requirements_unavailable(self, bytes.data(), bytes.size())) {
            d3d11_diag("ID3D11Device::CreatePixelShader -> E_INVALIDARG "
                       "(needs the 64 UAV slots of feature level 11_1)");
            m.set_return(d3d11::E_INVALIDARG);
            return;
        }
        // A shader that declares interfaces needs a class linkage
        // (E_INVALIDARG without one; d3d11_classlink_probe [create]).
        if (requires_class_linkage(bytes) &&
            refuse_interface_shader(linkage, "CreatePixelShader") != d3d11::S_OK) {
            m.set_return(d3d11::E_INVALIDARG);
            return;
        }
        // Learn D3D11_FEATURE_DATA_D3D11_OPTIONS2: SV_StencilRef needs
        // PSSpecifiedStencilRefSupported (feature 14 above); a device whose
        // cap is FALSE refuses such a shader with E_INVALIDARG, as WARP does
        // at exact 11_0 (d3d11_a09b [ps_stencil_ref]).
        {
            const auto container =
                graphics::parse_dxbc(bytes.data(), bytes.size());
            const auto device_interface = m_interfaces.find(self);
            const auto device_it =
                device_interface != m_interfaces.end()
                    ? m_devices.find(device_interface->second.owner)
                    : m_devices.end();
            const bool cap =
                device_it != m_devices.end() &&
                ps_specified_stencil_ref_supported(
                    device_it->second.feature_level);
            if (container.valid() &&
                graphics::dxbc_writes_stencil_ref(container) && !cap) {
                d3d11_diag("ID3D11Device::CreatePixelShader -> E_INVALIDARG "
                           "(SV_StencilRef; PSSpecifiedStencilRefSupported "
                           "is FALSE)");
                m.set_return(d3d11::E_INVALIDARG);
                return;
            }
        }
        ShaderTranslation translation{};
        {
            const PureWorkUnlock unlocked(*this);
            translation = translate_shader(bytes.data(), bytes.size(),
                                           graphics::ShaderStage::Fragment);
        }
        if (shader_out == 0) {
            m.set_return(translation.translated ? 1u : d3d11::E_FAIL);
            return;
        }
        const GuestAddr owner = m_interfaces.find(self)->second.owner;
        const GuestAddr shader = create_pixel_shader(
            owner, bytes.data(), bytes.size(), false, &translation);
        if (shader != 0) {
            const auto container =
                graphics::parse_dxbc(bytes.data(), bytes.size());
            attach_shader_linkage(m_pixel_shaders.at(shader).linkage, bytes, container);
            register_class_linkage(linkage, container);
        }
        if (shader == 0) {
            d3d11_diag("ID3D11Device::CreatePixelShader length=%llu -> E_FAIL",
                       static_cast<unsigned long long>(length));
            m.set_return(d3d11::E_FAIL);
            return;
        }
        if (m.write_ptr(shader_out, shader).is_err()) {
            (void)release_object(shader);
            m.set_return(d3d11::E_POINTER);
            return;
        }
        d3d11_diag("ID3D11Device::CreatePixelShader -> 0x%llx length=%llu",
                   static_cast<unsigned long long>(shader),
                   static_cast<unsigned long long>(length));
        m.set_return(d3d11::S_OK);
        return;
    }
    case 16:
    case 17:
        // CreateHullShader / CreateDomainShader (11.17).
        create_tessellation_shader(m, self, slot == 16);
        return;
    case 18: {
        // CreateComputeShader. This tier creates and owns the CS object; the
        // execution/UAV path remains explicitly unsupported by Dispatch.
        const GuestAddr bytecode = m.arg_ptr(1);
        const u64 length = m.arg(2);
        const GuestAddr linkage = m.arg_ptr(3);
        const GuestAddr shader_out = m.arg_ptr(4);
        if (shader_out != 0) (void)m.write_ptr(shader_out, 0);
        // pClassLinkage: NULL or a linkage of this device.
        if (bytecode == 0 || length == 0 || length > (1u << 24) ||
            !valid_class_linkage(m_interfaces.find(self)->second.owner,
                                 linkage)) {
            m.set_return(d3d11::E_INVALIDARG);
            return;
        }
        std::vector<u8> bytes(static_cast<usize>(length));
        if (m_memory.read_bytes(bytecode, bytes.data(), bytes.size()).is_err()) {
            m.set_return(d3d11::E_INVALIDARG);
            return;
        }
        // As for every other stage: compute bytecode of a shader model the
        // feature level runs. Shader model 5.1 is D3D12's (Learn, "Shader
        // Model 5.1"): cs_5_1 -> E_INVALIDARG at 11_0 and 11_1, x64 and x86
        // (tools/oracle_probe/d3d11_sm51_rejected_probe.expected).
        if (const char* why = shader_bytecode_refused(
                self, bytes.data(), bytes.size(),
                {graphics::ShaderStage::Compute})) {
            d3d11_diag("ID3D11Device::CreateComputeShader -> E_INVALIDARG "
                       "(%s)", why);
            m.set_return(d3d11::E_INVALIDARG);
            return;
        }
        if (uav_requirements_unavailable(self, bytes.data(), bytes.size())) {
            d3d11_diag("ID3D11Device::CreateComputeShader -> E_INVALIDARG "
                       "(needs the 64 UAV slots of feature level 11_1)");
            m.set_return(d3d11::E_INVALIDARG);
            return;
        }
        // A shader that declares interfaces needs a class linkage
        // (E_INVALIDARG without one; d3d11_classlink_probe [create]).
        if (requires_class_linkage(bytes) &&
            refuse_interface_shader(linkage, "CreateComputeShader") != d3d11::S_OK) {
            m.set_return(d3d11::E_INVALIDARG);
            return;
        }
        ShaderTranslation translation{};
        {
            const PureWorkUnlock unlocked(*this);
            translation = translate_shader(bytes.data(), bytes.size(),
                                           graphics::ShaderStage::Compute);
        }
        const GuestAddr owner = m_interfaces.find(self)->second.owner;
        const GuestAddr shader = create_compute_shader(
            owner, bytes.data(), bytes.size(), false, &translation);
        if (shader == 0) {
            m.set_return(d3d11::E_FAIL);
            return;
        }
        if (shader_out != 0) {
            const auto container =
                graphics::parse_dxbc(bytes.data(), bytes.size());
            attach_shader_linkage(m_compute_shaders.at(shader).linkage, bytes,
                                  container);
            register_class_linkage(linkage, container);
        }
        if (shader_out == 0) {
            (void)release_object(shader);
            m.set_return(1u); // S_FALSE: validation-only success.
            return;
        }
        if (m.write_ptr(shader_out, shader).is_err()) {
            (void)release_object(shader);
            m.set_return(d3d11::E_POINTER);
            return;
        }
        m.set_return(d3d11::S_OK);
        return;
    }
    case 19:
        // CreateClassLinkage(ppLinkage): d3d11_class_linkage.cpp.
        create_class_linkage(m, self);
        return;
    }
}

bool D3D11Manager::requires_class_linkage(const std::vector<u8>& bytes) {
    const auto container = graphics::parse_dxbc(bytes.data(), bytes.size());
    return container.valid() && graphics::dxbc_uses_class_linkage(container);
}

const char* D3D11Manager::shader_bytecode_refused(
    GuestAddr device_interface, const u8* bytecode, usize size,
    std::initializer_list<graphics::ShaderStage> stages) const {
    // Learn (ID3D11Device::Create*Shader): the bytecode must be a compiled
    // shader of that stage, of a shader model the device's feature level
    // runs -- 5.0 from 11_0, 4.1 at 10_1, 4.0 at 10_0 (and the
    // 4_0_level_9_x profiles, shader model 4.0 too, at 9_x). Measured
    // through the D3D10 device (d3d10_contract_probe [obj]): vs_4_1 and
    // vs_5_0 at 10_0, pixel-shader bytecode, and a truncated container are
    // E_INVALIDARG.
    const auto container = graphics::parse_dxbc(bytecode, size);
    if (!container.valid()) return "not shader bytecode";
    if (stages.size() != 0 &&
        std::find(stages.begin(), stages.end(), container.stage) ==
            stages.end()) {
        return "bytecode of another stage";
    }
    const auto interface_it = m_interfaces.find(device_interface);
    if (interface_it == m_interfaces.end()) return nullptr;
    const auto device = m_devices.find(interface_it->second.owner);
    if (device == m_devices.end()) return nullptr;
    const u32 level = device->second.feature_level;
    const u32 major = level >= d3d11::FEATURE_LEVEL_11_0 ? 5u : 4u;
    const u32 minor = level >= d3d11::FEATURE_LEVEL_11_0   ? 0u
                      : level >= d3d11::FEATURE_LEVEL_10_1 ? 1u
                                                           : 0u;
    if (container.shader_model_major > major ||
        (container.shader_model_major == major &&
         container.shader_model_minor > minor)) {
        return "shader model above the feature level";
    }
    return nullptr;
}

bool D3D11Manager::ps_specified_stencil_ref_supported(
    u32 feature_level) const {
    // D3D11_FEATURE_DATA_D3D11_OPTIONS2.PSSpecifiedStencilRefSupported: "the
    // hardware and driver support PS-specified stencil reference value"
    // (Learn). The output (oStencilRef, fxc: "PS Output Stencil Ref") is a
    // shader model 5 feature, so a device below FL 11_0 never has it. The
    // translator lowers SV_StencilRef to FragStencilRefEXT and the backend
    // exports it from the fragment function (supports_shader_stencil_export),
    // tested end to end by StencilRefPixelShaderWritesTheStencilBuffer
    // against d3d11_a09b_shader_probe.expected [SV_StencilRef].
    return feature_level >= d3d11::FEATURE_LEVEL_11_0 &&
           m_backend.supports_shader_stencil_export();
}

bool D3D11Manager::uav_requirements_unavailable(
    GuestAddr device_interface, const u8* bytecode, usize size) const {
    const auto interface_it = m_interfaces.find(device_interface);
    if (interface_it == m_interfaces.end()) return false;
    const auto device = m_devices.find(interface_it->second.owner);
    if (device == m_devices.end() ||
        device->second.feature_level >= d3d11::FEATURE_LEVEL_11_1) {
        return false;
    }
    const auto container = graphics::parse_dxbc(bytecode, size);
    if (!container.valid()) return false;
    if ((container.required_features &
         (graphics::kShaderRequiresUavsAtEveryStage |
          graphics::kShaderRequires64Uavs)) != 0) {
        return true;
    }
    switch (container.stage) {
    case graphics::ShaderStage::Vertex:
    case graphics::ShaderStage::Hull:
    case graphics::ShaderStage::Domain:
    case graphics::ShaderStage::Geometry:
        return graphics::dxbc_declares_uavs(container);
    default:
        return false;
    }
}

} // namespace vitreo::winapi
