// SPDX-License-Identifier: LGPL-2.1-only

/// @file test_compute_fence.cpp
/// @brief D3D sync without _t inside divergent control flow reaches Metal as
///        a fence that Metal actually compiles.
///
/// HLSL GroupMemoryBarrier()/DeviceMemoryBarrier() are fences, legal in
/// divergent flow. SPIRV-Cross would emit threadgroup_barrier (a rendezvous,
/// undefined when threads diverge); spirv_to_msl emits the MSL 3.2
/// atomic_thread_fence instead. The DXBC tests check the text; this checks
/// the Metal compiler accepts it through the backend's own pipeline path.

#include <gtest/gtest.h>

#include "graphics/contracts/backend_device.hpp"
#include "graphics/metal/metal_device.hpp"
#include "graphics/shaders/dxbc_to_spirv.hpp"
#include "graphics/shaders/spirv_to_msl.hpp"

#include <cstring>
#include <string>
#include <vector>

using namespace vitreo;
using namespace vitreo::graphics;

TEST(ComputeFence, DivergentSyncWithoutRendezvousCompilesOnMetal) {
    auto& device = MetalDevice::instance();
    if (!device.available()) GTEST_SKIP() << "no Metal device";
    if (!msl_memory_fence_available()) GTEST_SKIP() << "needs MSL 3.2";

    DxbcContainer container{};
    container.shader_model_major = 5;
    container.stage = ShaderStage::Compute;
    const std::vector<u32> words = {
        0x00050050u, 0u,                          // cs_5_0, length patched
        0x0400009Bu, 2u, 1u, 1u,                  // dcl_thread_group 2,1,1
        0x0304001Fu, 0x00004001u, 1u,             // if_nz l(1)
        0x010010BEu,                              //   sync_g
        0x01000015u,                              // endif
        0x0304001Fu, 0x00004001u, 1u,             // if_nz l(1)
        0x010050BEu,                              //   sync_uglobal_g
        0x01000015u,                              // endif
        0x0100003Eu,                              // ret
    };
    for (u32 word : words) {
        const auto* bytes = reinterpret_cast<const u8*>(&word);
        container.bytecode.insert(container.bytecode.end(), bytes, bytes + 4);
    }
    const u32 total = static_cast<u32>(words.size());
    std::memcpy(container.bytecode.data() + 4, &total, sizeof(total));

    const auto spirv = translate_dxbc_to_spirv(container);
    ASSERT_TRUE(spirv.valid()) << spirv.failure;
    const auto msl = compile_spirv_to_msl(
        spirv.spirv.data(), spirv.spirv.size(), ShaderStage::Compute);
    ASSERT_TRUE(msl.valid());
    const std::string text = msl.msl_text();
    EXPECT_NE(text.find("thread_scope_threadgroup"), std::string::npos) << text;
    EXPECT_NE(text.find("thread_scope_device"), std::string::npos) << text;
    EXPECT_EQ(text.find("threadgroup_barrier"), std::string::npos) << text;

    BackendComputePipelineDesc pd{};
    pd.compute_entry_point = "main0";  // SPIRV-Cross renames `main`
    pd.compute_shader_msl = text;
    auto pipeline = device.create_compute_pipeline(pd);
    ASSERT_TRUE(pipeline && pipeline->valid()) << text;
}
