// A compute write through a typed UAV (RWBuffer<uint>), then a buffer copy of
// the same buffer: D3D11 orders the copy after the dispatch (Learn,
// ID3D11DeviceContext::CopyResource; the immediate context executes in call
// order). Windows reads the expected running total on every one of 20
// iterations (tools/oracle_probe/d3d11_a10_dispatch_readback_probe.expected).
//
// Vitreo lost it three ways at once. Metal tracks hazards per MTLResource, and
// the typed view is a buffer-backed MTLTexture, not the buffer, so the blit
// reading the buffer was not ordered after the dispatch writing the texture;
// copy_buffer ignored a pool slice's offset; and an update that renamed a
// GPU-written buffer into the pool rebuilt it later from the CPU mirror,
// discarding what the GPU had written. Indirect dispatches lost the race most
// often (3 of 20 reads right), direct ones occasionally.

#include <gtest/gtest.h>

#include "graphics/contracts/backend_device.hpp"
#include "graphics/metal/metal_device.hpp"
#include "graphics/shaders/dxbc_parser.hpp"
#include "graphics/shaders/dxbc_to_spirv.hpp"
#include "graphics/shaders/spirv_to_msl.hpp"
#include "typed_uav_count_shader.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <vector>

using namespace vitreo;
using namespace vitreo::graphics;

namespace {

struct CountRig {
    std::unique_ptr<BackendComputePipeline> pipeline;
    std::unique_ptr<BackendBuffer> counter;
    std::unique_ptr<BackendTextureBuffer> view;
    std::unique_ptr<BackendBuffer> args;
    std::unique_ptr<BackendBuffer> staging;
    std::array<u32, kPushBlockBytes / 4> push{};
};

bool build(MetalDevice& device, CountRig& rig, std::string& why) {
    const auto container =
        parse_dxbc(typed_uav_count_cs, sizeof(typed_uav_count_cs));
    if (!container.valid()) { why = "dxbc"; return false; }
    const auto spirv = translate_dxbc_to_spirv(container);
    if (!spirv.valid()) { why = spirv.failure; return false; }
    MslCompileOptions flat{};
    flat.argument_buffers = false;
    // An atomic on a typed UAV needs MSL 3.1's native texture atomics, as the
    // D3D11 frontend requests (raise_msl_version_for_typed_uav_atomics).
    if (spirv.uses_typed_uav_atomics) flat.msl_version = 30100u;
    const auto msl = compile_spirv_to_msl(
        spirv.spirv.data(), spirv.spirv.size(), ShaderStage::Compute, flat);
    if (!msl.valid()) { why = "msl"; return false; }
    BackendComputePipelineDesc pd{};
    pd.compute_entry_point = "main0";
    pd.compute_shader_msl = msl.msl_text();
    pd.cache_key = 0xC0047ED0u;
    rig.pipeline = device.create_compute_pipeline(pd);
    if (!rig.pipeline || !rig.pipeline->valid()) {
        why = msl.msl_text();
        return false;
    }

    const std::array<u32, 4> zeros{};
    BackendBufferDesc cd{};
    cd.byte_width = sizeof(zeros);
    cd.unordered_access = true;
    rig.counter = device.create_buffer(cd, zeros.data());
    BackendBufferDesc sd{};
    sd.byte_width = sizeof(zeros);
    sd.cpu_writable = true;
    rig.staging = device.create_buffer(sd, zeros.data());
    const std::array<u32, 3> groups{1, 1, 1};
    BackendBufferDesc ad{};
    ad.byte_width = sizeof(groups);
    rig.args = device.create_buffer(ad, groups.data());
    if (!rig.counter || !rig.staging || !rig.args) { why = "buffers"; return false; }
    rig.view = device.create_texture_buffer(*rig.counter, PixelFormat::R32Uint,
                                            0, 4, /*shader_write=*/true);
    if (!rig.view || !rig.view->valid()) { why = "typed view"; return false; }
    // The translator's out-of-bounds guard reads each view's size from the
    // push block; give every slot a size well past what the shader touches.
    for (u32 slot = 0; slot < kGuardSizeSrvSlots + kGuardSizeUavSlots; ++slot) {
        rig.push[kGuardSizeTableOffset / 4 + slot] = 0xFFFFFFFFu;
    }
    return true;
}

// Runs 20 dispatch+copy iterations without resetting the counter and returns
// how many staging reads saw the running total (60 threads each).
int run(MetalDevice& device, CountRig& rig, bool indirect) {
    int good = 0;
    for (u32 iteration = 0; iteration < 20; ++iteration) {
        BackendDispatchCall call{};
        call.pipeline = rig.pipeline.get();
        call.group_count_x = call.group_count_y = call.group_count_z = 1;
        call.threads_per_group_x = 3;
        call.threads_per_group_y = 4;
        call.threads_per_group_z = 5;
        call.texture_buffers[BACKEND_UAV_TEXTURE_BASE] = rig.view.get();
        call.constants = rig.push.data();
        call.constants_size = sizeof(rig.push);
        if (indirect) call.indirect.buffer = rig.args.get();
        if (!device.dispatch(call)) return -1;
        if (!device.copy_buffer(rig.staging.get(), rig.counter.get())) {
            return -1;
        }
        // Only the copy is waited for, as Map(READ) of the staging buffer is.
        device.wait_idle();
        u32 value = 0;
        if (!rig.staging->read(0, &value, sizeof(value))) return -1;
        if (value == 60u * (iteration + 1)) ++good;
        if (std::getenv("TYPED_UAV_DEBUG")) {
            u32 direct = 0;
            (void)rig.counter->read(0, &direct, sizeof(direct));
            std::fprintf(stderr, "iter %u staging=%u counter=%u\n", iteration,
                         value, direct);
        }
    }
    return good;
}

}  // namespace

TEST(TypedUavOrderingGpu, CopyOfTypedUavBufferFollowsTheDispatchThatWroteIt) {
    auto& device = MetalDevice::instance();
    if (!device.available()) GTEST_SKIP() << "no Metal device";
    for (const bool indirect : {false, true}) {
        CountRig rig;
        std::string why;
        ASSERT_TRUE(build(device, rig, why)) << why;
        EXPECT_EQ(run(device, rig, indirect), 20)
            << (indirect ? "DispatchIndirect" : "Dispatch")
            << ": Windows reads the running total 20 of 20 times";
    }
}

TEST(TypedUavOrderingGpu, UpdateOfGpuWrittenBufferIsOrderedAgainstDispatches) {
    auto& device = MetalDevice::instance();
    if (!device.available()) GTEST_SKIP() << "no Metal device";
    CountRig rig;
    std::string why;
    ASSERT_TRUE(build(device, rig, why)) << why;
    // UpdateSubresource(0) + DispatchIndirect, 20 times: each read must be
    // exactly one dispatch (60). A rename rebuilt from the CPU mirror read 0.
    int good = 0;
    const std::array<u32, 4> zeros{};
    for (int iteration = 0; iteration < 20; ++iteration) {
        ASSERT_TRUE(rig.counter->update(0, zeros.data(), sizeof(zeros),
                                        BufferWrite::Ordered));
        BackendDispatchCall call{};
        call.pipeline = rig.pipeline.get();
        call.threads_per_group_x = 3;
        call.threads_per_group_y = 4;
        call.threads_per_group_z = 5;
        call.texture_buffers[BACKEND_UAV_TEXTURE_BASE] = rig.view.get();
        call.constants = rig.push.data();
        call.constants_size = sizeof(rig.push);
        call.indirect.buffer = rig.args.get();
        ASSERT_TRUE(device.dispatch(call));
        ASSERT_TRUE(device.copy_buffer(rig.staging.get(), rig.counter.get()));
        device.wait_idle();
        u32 value = 0;
        ASSERT_TRUE(rig.staging->read(0, &value, sizeof(value)));
        if (value == 60u) ++good;
    }
    EXPECT_EQ(good, 20) << "Windows: 20 of 20";
}
