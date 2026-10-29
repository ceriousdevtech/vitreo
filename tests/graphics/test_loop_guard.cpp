// SPDX-License-Identifier: LGPL-2.1-only

/// @file test_loop_guard.cpp
/// @brief A runaway shader loop ends at the iteration guard instead of
///        hanging the GPU.
///
/// Windows stops GPU work that runs past ~2 s (Timeout Detection and
/// Recovery). macOS does not stop ours: a shader looping on a runaway count
/// pinned the GPU until the whole Mac froze. Translated loops therefore carry
/// an iteration guard. The shader here loops until a 32-bit counter wraps --
/// four billion iterations -- and must stop after exactly the configured cap.
/// It is safe even if the guard were broken: one thread, finite loop.

#include <gtest/gtest.h>

#include "graphics/contracts/backend_device.hpp"
#include "graphics/metal/metal_device.hpp"
#include "graphics/shaders/dxbc_to_spirv.hpp"
#include "graphics/shaders/spirv_to_msl.hpp"

#include <cstring>
#include <regex>
#include <string>
#include <vector>

using namespace vitreo;
using namespace vitreo::graphics;

TEST(LoopGuard, RunawayLoopStopsAtTheCapOnTheGpu) {
    auto& device = MetalDevice::instance();
    if (!device.available()) GTEST_SKIP() << "no Metal device";
    const u32 cap = shader_loop_iteration_cap();
    ASSERT_NE(cap, 0u) << "guards disabled";

    DxbcContainer container{};
    container.shader_model_major = 5;
    container.stage = ShaderStage::Compute;
    const std::vector<u32> words = {
        0x00050050u, 0u,                             // cs_5_0, length below
        0x0100086Au,
        0x02000068u, 2u,                             // dcl_temps 2
        0x0400009Bu, 1u, 1u, 1u,                     // dcl_thread_group 1,1,1
        0x0300009Du, 0x0011E000u, 0u,                // dcl_uav_raw u0
        0x05000036u, 0x00100012u, 0u, 0x00004001u, 0u,  // mov r0.x, l(0)
        0x01000030u,                                 // loop
        0x0700001Eu, 0x00100012u, 0u, 0x0010000Au, 0u,
            0x00004001u, 1u,                         //   iadd r0.x, r0.x, l(1)
        0x07000020u, 0x00100012u, 1u, 0x0010000Au, 0u,
            0x00004001u, 0u,                         //   ieq r1.x, r0.x, l(0)
        0x03040003u, 0x0010000Au, 1u,                //   breakc_nz r1.x
        0x01000016u,                                 // endloop
        0x070000A6u, 0x0011E012u, 0u, 0x00004001u, 0u,
            0x00100006u, 0u,                         // store_raw u0.x, l(0), r0.x
        0x0100003Eu,                                 // ret
    };
    for (u32 word : words) {
        const auto* bytes = reinterpret_cast<const u8*>(&word);
        container.bytecode.insert(container.bytecode.end(), bytes, bytes + 4);
    }
    const u32 total = static_cast<u32>(words.size());
    std::memcpy(container.bytecode.data() + 4, &total, sizeof(total));

    const auto spirv = translate_dxbc_to_spirv(container);
    ASSERT_TRUE(spirv.valid()) << spirv.failure;
    MslCompileOptions options{};
    options.argument_buffers = false;
    const auto msl = compile_spirv_to_msl(spirv.spirv.data(),
                                          spirv.spirv.size(),
                                          ShaderStage::Compute, options);
    ASSERT_TRUE(msl.valid());
    const std::string text = msl.msl_text();
    std::smatch slot;
    ASSERT_TRUE(std::regex_search(text, slot,
                                  std::regex(R"(u0\w*\s*\[\[buffer\((\d+)\)\]\])")))
        << text;
    const unsigned uav_slot = static_cast<unsigned>(std::stoul(slot[1]));

    BackendComputePipelineDesc pd{};
    pd.compute_entry_point = "main0";
    pd.compute_shader_msl = text;
    auto pipeline = device.create_compute_pipeline(pd);
    ASSERT_TRUE(pipeline && pipeline->valid()) << text;

    BackendBufferDesc bd{};
    bd.byte_width = 16;
    bd.unordered_access = true;
    bd.shader_resource = true;
    const u32 sentinel[4] = {0xEEEEEEEEu, 0, 0, 0};
    auto buffer = device.create_buffer(bd, sentinel);
    ASSERT_TRUE(buffer);

    BackendDispatchCall call{};
    call.pipeline = pipeline.get();
    call.group_count_x = call.group_count_y = call.group_count_z = 1;
    call.threads_per_group_x = call.threads_per_group_y =
        call.threads_per_group_z = 1;
    call.buffers[uav_slot] = buffer.get();
    ASSERT_TRUE(device.dispatch(call));
    ASSERT_TRUE(device.wait_idle());

    u32 result = 0;
    ASSERT_TRUE(buffer->read(0, &result, sizeof(result)));
    EXPECT_EQ(result, cap)
        << "the loop body runs exactly `cap` times, then the guard breaks";
}
