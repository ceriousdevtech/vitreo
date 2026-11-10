// SPDX-License-Identifier: LGPL-2.1-only

/// @file d3d12_pipeline_mesh.cpp
/// @brief Mesh shader pipelines (D3D12_PIPELINE_STATE_STREAM AS / MS
/// subobjects; DirectX-Specs "Mesh Shader", CreatePipelineState) on Metal
/// object + mesh pipelines (graphics/shaders/mesh_stage.hpp).
///
/// Measured (tools/oracle_probe/d3d12_a40_mesh_probe "pso" rows, RTX 4080
/// SUPER and WARP alike): a mesh pipeline takes a mesh shader and optionally
/// an amplification shader, no vertex shader, no input layout and no stream
/// output; an amplification shader without a mesh shader, a shader of
/// another stage in either slot, an amplification payload whose size is not
/// the mesh shader's, and a pixel input no mesh output provides are
/// E_INVALIDARG. A mesh shader that declares a payload runs without an
/// amplification shader (S_OK). Every primitive topology type but PATCH is
/// accepted, UNDEFINED included (finish_graphics_pipeline).

#include "winapi/graphics/d3d12/d3d12_internal.hpp"
#include "graphics/shaders/dxbc_parser.hpp"
#include "graphics/shaders/dxil_container.hpp"
#include "core/memory/guest_memory.hpp"
#include "graphics/shaders/mesh_stage.hpp"

namespace vitreo::winapi::d3d12 {

namespace {

// Metal Feature Set Tables (Apple7+): 32 KB of threadgroup memory per
// threadgroup, measured on the M4 Pro (maxThreadgroupMemoryLength 32768).
constexpr u32 kThreadgroupMemoryBytes = 32768;

}  // namespace

u32 Frontend::compile_mesh_stages(const PipelineSource& src, PipelineState& pso,
                                  std::vector<u8> ms_bytes,
                                  const std::vector<u8>& ps_bytes,
                                  const char* api) {
    if (ms_bytes.empty()) {
        return refuse(E_INVALIDARG, "%s: an amplification shader without a mesh shader", api);
    }
    for (const GuestAddr stage : {src.vs, src.gs, src.hs, src.ds}) {
        if (stage != 0 && shader_present(stage)) {
            return refuse(E_INVALIDARG, "%s: a mesh pipeline with a VS, GS, HS or DS", api);
        }
    }
    if (src.stream_output_entries != 0 || !pso.graphics.input_layout.empty()) {
        return refuse(E_INVALIDARG, "%s: a mesh pipeline with stream output or an input layout",
                      api);
    }
    if (!m_backend.supports_mesh_pipelines()) {
        unimplemented(api, "mesh shaders (this GPU has no Metal mesh pipelines)");
        return E_INVALIDARG;
    }
    // "must use DXIL bytecode for all attached shaders": the pixel shader too.
    if (src.ps != 0 && shader_present(src.ps)) {
        const auto& l = layout::D3D12_SHADER_BYTECODE[m_is_64bit];
        GuestAddr pointer = 0, length = 0;
        std::vector<u8> raw;
        if (read_pointer(src.ps + l.pShaderBytecode, pointer) &&
            read_pointer(src.ps + l.BytecodeLength, length) && length <= 0x4000000ull) {
            raw.resize(static_cast<usize>(length));
            if (m_memory.read_bytes(pointer, raw.data(), raw.size()).is_err()) raw.clear();
        }
        if (!graphics::dxil_container_present(raw.data(), raw.size())) {
            return refuse(E_INVALIDARG, "%s: a mesh or amplification shader that is not DXIL",
                          api);
        }
    }
    std::vector<u8> as_bytes;
    if (src.as != 0) {
        if (u32 error = read_shader_bytes(src.as, as_bytes); error != S_OK) {
            return error;
        }
    }
    const auto ms = graphics::parse_dxbc(ms_bytes.data(), ms_bytes.size());
    if (!ms.valid() || !ms.mesh.is_mesh()) {
        return refuse(E_INVALIDARG, "%s: the MS bytecode is not a mesh shader", api);
    }
    graphics::DxbcContainer as{};
    if (!as_bytes.empty()) {
        as = graphics::parse_dxbc(as_bytes.data(), as_bytes.size());
        if (!as.valid() || !as.mesh.is_amplification()) {
            return refuse(E_INVALIDARG, "%s: the AS bytecode is not an amplification shader",
                          api);
        }
        // "Runtime will validate that the structure size reported by the
        // compiler matches between the Amplification shader and the Mesh
        // shader."
        if (as.mesh.payload_bytes != ms.mesh.payload_bytes) {
            return refuse(E_INVALIDARG, "%s: AS payload %u bytes, MS payload %u bytes", api,
                          as.mesh.payload_bytes, ms.mesh.payload_bytes);
        }
        if (as.mesh.payload_words != ms.mesh.payload_words &&
            ms.mesh.payload_words != 0) {
            const std::string why =
                "an amplification and a mesh payload of one size (" +
                std::to_string(ms.mesh.payload_bytes) + " bytes) moved as " +
                std::to_string(as.mesh.payload_words) + " and " +
                std::to_string(ms.mesh.payload_words) + " 32-bit words";
            unimplemented(api, why.c_str());
            return E_INVALIDARG;
        }
    }
    graphics::DxbcContainer ps{};
    if (!ps_bytes.empty()) {
        ps = graphics::parse_dxbc(ps_bytes.data(), ps_bytes.size());
        if (!ps.valid()) {
            return refuse(E_INVALIDARG, "%s: the PS bytecode does not parse", api);
        }
    }
    std::string why;
    bool invalid = false;
    if (!graphics::build_mesh_stage_layout(
            ms, ps_bytes.empty() ? nullptr : &ps.input_signature,
            pso.mesh.ms_layout, why, invalid)) {
        if (invalid) {
            d3d12_diag("%s: %s", api, why.c_str());
        } else {
            unimplemented(api, why.c_str());
        }
        return E_INVALIDARG;
    }
    // Target-independent / conservative rasterization: the mesh stage
    // appends its surviving primitives to a capture that the raster pass C
    // draws (finish_mesh_pipeline, Replayer::dispatch_mesh_view).
    if (pso.raster.active &&
        !graphics::mesh_stage_capture_layout(pso.mesh.ms_layout, why)) {
        unimplemented(api, why.c_str());
        return E_INVALIDARG;
    }
    // The staged outputs share Metal's threadgroup memory with the shader's
    // own groupshared memory (measured: a mesh function past 32768 bytes is
    // refused, "exceeds the maximum threadgroup memory allowed"). An
    // amplification shader's payload is read where Metal keeps it
    // (mesh_stage_direct_payload); without one the declared payload is a
    // zeroed threadgroup copy. When they do not fit, the vertices,
    // primitives and indices are staged in device memory, one scratch slot
    // per threadgroup (MeshStageLayout::device_staging); what stays in
    // threadgroup memory then fits every legal shader (groupshared plus
    // payload <= 28 KB).
    if (graphics::mesh_stage_threadgroup_bytes(pso.mesh.ms_layout, !as_bytes.empty()) +
            ms.mesh.groupshared_bytes >
        kThreadgroupMemoryBytes) {
        pso.mesh.ms_layout.device_staging = true;
        if (graphics::mesh_stage_threadgroup_bytes(pso.mesh.ms_layout, !as_bytes.empty()) +
                ms.mesh.groupshared_bytes >
            kThreadgroupMemoryBytes) {
            unimplemented(api, "a mesh shader whose groupshared memory alone exceeds 32 KB of "
                               "Metal threadgroup memory");
            return E_INVALIDARG;
        }
    }
    // The payload the object function writes; without an amplification
    // shader a declared payload is undefined and reads 0.
    const u32 payload_words = as_bytes.empty() ? 0u : as.mesh.payload_words;
    if (u32 error = compile_shader(std::move(ms_bytes), graphics::ShaderStage::Mesh,
                                   pso.mesh.ms, api, pso.root.get(),
                                   D3D12_SHADER_VISIBILITY_MESH, nullptr, nullptr,
                                   nullptr, &pso.mesh.ms_layout, payload_words);
        error != S_OK) {
        return error;
    }
    if (!as_bytes.empty()) {
        pso.mesh.as_layout = graphics::amplification_stage_layout(as);
        pso.mesh.as_layout.device_staging = pso.mesh.ms_layout.device_staging;
        if (u32 error = compile_shader(std::move(as_bytes),
                                       graphics::ShaderStage::Amplification,
                                       pso.mesh.as, api, pso.root.get(),
                                       D3D12_SHADER_VISIBILITY_AMPLIFICATION,
                                       nullptr, nullptr, nullptr,
                                       &pso.mesh.as_layout,
                                       payload_words);
            error != S_OK) {
            return error;
        }
    }
    pso.mesh.payload_words = payload_words;
    pso.mesh.active = true;
    return S_OK;
}

graphics::BackendPipeline* Frontend::mesh_view_pipeline(PipelineState& pso, u32 kind,
                                                       bool statistics) {
    if (kind > kMeshViewReplay) return nullptr;
    auto& slot = pso.mesh.view_pipelines[kind][statistics ? 1 : 0];
    bool& tried = pso.mesh.view_tried[kind][statistics ? 1 : 0];
    if (tried) return slot.get();
    tried = true;
    constexpr const char* api = "ExecuteCommandLists";
    // Translated at replay: with this pipeline's SV_ViewID.
    struct ViewIdScope {
        u32& value;
        u32 saved;
        ~ViewIdScope() { value = saved; }
    } scope{build_scratch().view_id, build_scratch().view_id};
    build_scratch().view_id = pso.view_id;
    const u32 words = pso.mesh.payload_words;
    const bool base = kind == kMeshViewStubCapture;
    if (base && (words + 1u) * 4u > 16384u) {
        // DirectX-Specs "Mesh Shader": the payload is at most 16 KB, as is
        // Metal's; the record base takes one word after it.
        unimplemented(api, "view instancing (tier 3) of a mesh shader behind a full "
                           "16 KB amplification payload");
        return nullptr;
    }
    graphics::MeshStageLayout ms_layout = pso.mesh.ms_layout;
    graphics::MeshStageLayout as_layout = pso.mesh.as_layout;
    // The run-once passes count the stages they run; replays count none
    // (the pixel shader counts its own).
    ms_layout.statistics = statistics && kind != kMeshViewReplay;
    as_layout.statistics = statistics && kind == kMeshViewAsCapture;
    std::string ms_msl;
    std::string as_msl;
    int object = -1;
    u32 payload_bytes = 0;
    const auto translate_ms = [&](u32 payload) -> bool {
        CompiledShader ms;
        if (compile_shader(pso.mesh.ms.bytecode, graphics::ShaderStage::Mesh, ms, api,
                           pso.root.get(), D3D12_SHADER_VISIBILITY_MESH, nullptr, nullptr,
                           nullptr, &ms_layout, payload) != S_OK) {
            return false;
        }
        ms_msl = std::move(ms.msl);
        return true;
    };
    switch (kind) {
    case kMeshViewAsCapture: {
        as_layout.vi_capture = graphics::kMeshViCaptureAmplification;
        CompiledShader as;
        if (!pso.mesh.as.present ||
            compile_shader(pso.mesh.as.bytecode, graphics::ShaderStage::Amplification, as,
                           api, pso.root.get(), D3D12_SHADER_VISIBILITY_AMPLIFICATION,
                           nullptr, nullptr, nullptr, &as_layout, words) != S_OK) {
            return nullptr;
        }
        as_msl = std::move(as.msl);
        ms_layout.statistics = false;  // launched by no group
        if (!translate_ms(words)) return nullptr;
        break;
    }
    case kMeshViewStub:
    case kMeshViewStubCapture:
        if (!pso.mesh.as.present) return nullptr;
        as_msl = graphics::mesh_vi_object_replay_msl(pso.mesh.as_layout, words, base);
        if (base) {
            ms_layout.vi_capture = graphics::kMeshViCaptureMesh;
            ms_layout.vi_payload_base = true;
            ms_layout.vi_base_word = words;
            payload_bytes = (words + 1u) * 4u;
            if (!translate_ms(words + 1u)) return nullptr;
        } else if (!translate_ms(words)) {
            return nullptr;
        }
        object = 1;
        break;
    case kMeshViewMsCapture:
        ms_layout.vi_capture = graphics::kMeshViCaptureMesh;
        if (!translate_ms(0)) return nullptr;
        object = 0;
        break;
    case kMeshViewReplay:
        ms_msl = graphics::mesh_vi_mesh_replay_msl(pso.mesh.ms_layout);
        object = 0;
        break;
    default:
        return nullptr;
    }
    const std::string fragment =
        !pso.ps.present ? std::string() : statistics ? statistics_fragment(pso) : pso.ps.msl;
    slot = make_mesh_pipeline(pso, ms_msl, as_msl, fragment,
                              pso.serial ^ (0x71E30000ull + kind * 2u + (statistics ? 1u : 0u)),
                              object, payload_bytes);
    if (!slot) d3d12_diag("view instancing: the backend refused a mesh pipeline variant");
    return slot.get();
}

graphics::BackendPipeline* Frontend::mesh_statistics_pipeline(PipelineState& pso) {
    if (pso.mesh.statistics_tried) return pso.mesh.statistics_pipeline.get();
    pso.mesh.statistics_tried = true;
    // Translated at replay: with this pipeline's SV_ViewID.
    struct ViewIdScope {
        u32& value;
        u32 saved;
        ~ViewIdScope() { value = saved; }
    } scope{build_scratch().view_id, build_scratch().view_id};
    build_scratch().view_id = pso.view_id;
    constexpr const char* api = "ExecuteCommandLists";
    graphics::MeshStageLayout ms_layout = pso.mesh.ms_layout;
    graphics::MeshStageLayout as_layout = pso.mesh.as_layout;
    ms_layout.statistics = true;
    as_layout.statistics = true;
    ms_layout.depth_clip = pso.graphics.depth_clip;
    CompiledShader ms;
    CompiledShader as;
    if (compile_shader(pso.mesh.ms.bytecode, graphics::ShaderStage::Mesh, ms, api,
                       pso.root.get(), D3D12_SHADER_VISIBILITY_MESH, nullptr, nullptr,
                       nullptr, &ms_layout, pso.mesh.payload_words) != S_OK) {
        return nullptr;
    }
    if (pso.mesh.as.present &&
        compile_shader(pso.mesh.as.bytecode, graphics::ShaderStage::Amplification, as,
                       api, pso.root.get(), D3D12_SHADER_VISIBILITY_AMPLIFICATION,
                       nullptr, nullptr, nullptr, &as_layout,
                       pso.mesh.payload_words) != S_OK) {
        return nullptr;
    }
    const std::string fragment = pso.ps.present ? statistics_fragment(pso) : std::string();
    if (pso.ps.present && fragment.empty()) return nullptr;
    pso.mesh.statistics_pipeline =
        make_mesh_pipeline(pso, ms.msl, as.msl, fragment, pso.serial ^ 0x57A7C0ull);
    return pso.mesh.statistics_pipeline.get();
}

}  // namespace vitreo::winapi::d3d12
