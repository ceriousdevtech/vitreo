// Executes fxc bytecode on Metal and compares with Windows.
//
// D3D11.3 22.11.5: firstbit_hi / firstbit_shi count from the MSB
// ("firstbit_hi on 0x10000000 returns 3") -- fxc adds the `31 - n` that
// turns them into HLSL firstbithigh() -- and 22.13.5: itof's negate modifier
// is a 2's complement negate of the integer source. The translator mapped
// both scans to LSB-based Find*Msb and dropped itof's negate; UE's base-pass
// pixel shaders turn a boolean into 1.0 with `itof r, -r`.

#include <gtest/gtest.h>

#include "graphics/contracts/backend_device.hpp"
#include "graphics/metal/metal_device.hpp"
#include "graphics/shaders/dxbc_parser.hpp"
#include "graphics/shaders/dxbc_to_spirv.hpp"
#include "graphics/shaders/spirv_to_msl.hpp"
#include "bit_scan_itof_shader.h"

#include <array>
#include <cstdint>
#include <vector>

using namespace vitreo;
using namespace vitreo::graphics;

TEST(IntegerConversionGpu, BitScansAndNegatedItofMatchWinoracle) {
    auto& device = MetalDevice::instance();
    if (!device.available()) GTEST_SKIP() << "no Metal device";

    const auto container =
        parse_dxbc(bit_scan_itof_cs, sizeof(bit_scan_itof_cs));
    ASSERT_TRUE(container.valid());
    const auto spirv = translate_dxbc_to_spirv(container);
    ASSERT_TRUE(spirv.valid()) << spirv.failure;
    MslCompileOptions flat{};
    flat.argument_buffers = false;
    const auto msl = compile_spirv_to_msl(
        spirv.spirv.data(), spirv.spirv.size(), ShaderStage::Compute, flat);
    ASSERT_TRUE(msl.valid());

    BackendComputePipelineDesc pd{};
    pd.compute_entry_point = "main0";
    pd.compute_shader_msl = msl.msl_text();
    pd.cache_key = 0xB175CA11u;
    auto pipeline = device.create_compute_pipeline(pd);
    ASSERT_TRUE(pipeline && pipeline->valid()) << msl.msl_text();

    constexpr usize kBytes = 16 * 16;
    BackendBufferDesc bd{};
    bd.byte_width = kBytes;
    bd.unordered_access = true;
    bd.shader_resource = true;
    const std::vector<u8> zeros(kBytes, 0);
    auto out = device.create_buffer(bd, zeros.data());
    ASSERT_NE(out, nullptr);

    BackendDispatchCall call{};
    call.pipeline = pipeline.get();
    call.group_count_x = call.group_count_y = call.group_count_z = 1;
    call.threads_per_group_x = 16;
    call.threads_per_group_y = call.threads_per_group_z = 1;
    call.buffers[BACKEND_UAV_BUFFER_BASE] = out.get();
    ASSERT_TRUE(device.dispatch(call));
    device.wait_idle();

    std::array<std::array<std::uint32_t, 4>, 16> got{};
    ASSERT_TRUE(out->read(0, got.data(), kBytes));
    for (usize thread = 0; thread < got.size(); ++thread) {
        for (usize c = 0; c < 4; ++c) {
            EXPECT_EQ(got[thread][c], bit_scan_itof_expected[thread][c])
                << "thread " << thread << " component " << c;
        }
    }
}
