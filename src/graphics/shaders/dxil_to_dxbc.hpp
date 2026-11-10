// SPDX-License-Identifier: LGPL-2.1-only
#pragma once
/// @file dxil_to_dxbc.hpp
/// @brief Lowers a DXIL (shader model 6) program to a shader model 5.1 DXBC
/// container that the DXBC translator (dxbc_to_spirv.hpp) compiles.
///
/// DXIL is scalar SSA over an unstructured, reducible CFG (DXIL.rst,
/// "Control-flow restrictions"); shader model 5.1 is register-based with
/// structured control flow and expresses the same resource model (register
/// ranges in spaces under range IDs, D3D12 descriptor tables). The lowering
///  - structures the CFG (dominator tree + reverse postorder, after Ramsey,
///    "Beyond Relooper", JFP 2022): loop headers become `loop`, merge nodes
///    get an enclosing one-pass `loop ... break endloop`, branches become
///    break / continue (a branch out of several constructs sets a target
///    register and breaks through each, which re-dispatches), conditional
///    branches and switches become if_nz chains;
///  - gives every SSA value a temp-register component and every phi a
///    register written on each incoming edge;
///  - selects SM5.1 instructions for each LLVM instruction and dx.op call;
///  - declares every resource range from dx.resources, with its range ID,
///    bounds, space, shape, element type and stride, so the frontend's
///    binding remap (d3d12_pipeline.cpp configure_descriptor_bindings) and
///    every side buffer it attaches see exactly what an SM5.1 program
///    declares;
///  - writes a DXBC container (ISG1 / OSG1 / PSG1 and RTS0 copied from the
///    DXIL container, SHEX, the container hash).
///
/// The result is a translation, not an approximation: an operation the
/// lowering cannot express exactly fails the whole shader with the
/// construct's name (DxilToDxbcResult::failure), which the caller reports as
/// UNIMPLEMENTED.

#include "shader_types.hpp"

#include <string>
#include <vector>

namespace vitreo::graphics {

struct DxilToDxbcResult {
    bool ok{false};
    /// Set when ok == false: why. `unimplemented` distinguishes a valid
    /// program using a construct the lowering does not express yet (the
    /// caller logs UNIMPLEMENTED) from malformed DXIL.
    std::string failure;
    bool unimplemented{false};
    ShaderStage stage{ShaderStage::Compute};
    u32 shader_model_major{0};
    u32 shader_model_minor{0};
    /// The wave sizes the entry runs at (HLSL SM 6.6 WaveSize, DXIL
    /// kDxilWaveSizeTag 11; SM 6.8 ranges, kDxilRangedWaveSizeTag 23, the
    /// preferred size ignored): 0 / 0 when any. "D3D12 runtime validation
    /// will fail if shaders in a pipeline state object have a required wave
    /// size that is not in the range reported by the driver": the caller
    /// checks it against WaveLaneCountMin / Max.
    u32 wave_size_min{0};
    u32 wave_size_max{0};
    /// The SM5.1 DXBC container.
    std::vector<u8> container;
};

/// Lowers the DXIL container at @p data. Mesh, amplification and library
/// programs (and DXR) parse but are refused (unimplemented).
[[nodiscard]] DxilToDxbcResult dxil_to_dxbc(const u8* data, usize size);

}  // namespace vitreo::graphics
