// SPDX-License-Identifier: LGPL-2.1-only

/// @file test_texture_zero_init.cpp
/// @brief A texture created without initial data reads as zero, as on
///        Windows.
///
/// WDDM zero-fills every new GPU allocation, so a D3D resource that has not
/// been written yet reads as zero, and games sample such targets (UE's
/// pooled render targets). Metal leaves fresh private textures as garbage --
/// Apple GPUs read them as magenta / NaN -- which painted Satisfactory's
/// scene magenta. Every kind of texture must come back zeroed.

#include <gtest/gtest.h>

#include "graphics/contracts/backend_device.hpp"
#include "graphics/metal/metal_device.hpp"

#include <vector>

using namespace vitreo;
using namespace vitreo::graphics;

namespace {

std::unique_ptr<BackendTexture2D> make(PixelFormat format, bool render_target,
                                       bool unordered_access, bool depth) {
    BackendTexture2DDesc td{};
    td.width = td.height = 64;
    td.mip_levels = 2;
    td.array_size = 1;
    td.pixel_format = format;
    td.shader_resource = true;
    td.render_target = render_target;
    td.unordered_access = unordered_access;
    td.depth_stencil = depth;
    return MetalDevice::instance().create_texture_2d(td, nullptr, 0);
}

}  // namespace

TEST(TextureZeroInit, FreshTexturesReadAsZero) {
    if (!MetalDevice::instance().available()) GTEST_SKIP() << "no Metal";
    struct Case {
        const char* name;
        PixelFormat format;
        bool rt, uav;
        unsigned bytes_per_pixel;
    };
    const Case cases[] = {
        {"RGBA16F render target", PixelFormat::RGBA16Float, true, false, 8},
        {"RGBA8 render target", PixelFormat::RGBA8Unorm, true, false, 4},
        {"RGBA8 UAV", PixelFormat::RGBA8Unorm, false, true, 4},
        {"RGBA8 sampled only", PixelFormat::RGBA8Unorm, false, false, 4},
        {"R32F sampled only", PixelFormat::R32Float, false, false, 4},
    };
    for (const Case& c : cases) {
        SCOPED_TRACE(c.name);
        auto texture = make(c.format, c.rt, c.uav, false);
        ASSERT_TRUE(texture);
        for (unsigned mip = 0; mip < 2; ++mip) {
            const unsigned side = 64u >> mip;
            std::vector<u8> pixels(side * side * c.bytes_per_pixel, 0xEE);
            ASSERT_TRUE(texture->readback(mip, pixels.data(),
                                          side * c.bytes_per_pixel));
            for (u8 byte : pixels) {
                ASSERT_EQ(byte, 0u) << "mip " << mip << " not zeroed";
            }
        }
    }

    auto depth = make(PixelFormat::D32Float_S8Uint, false, false, true);
    ASSERT_TRUE(depth);
    std::vector<float> plane(64 * 64, 1234.0f);
    ASSERT_TRUE(depth->readback_depth_stencil_plane(0, 0, false, plane.data(),
                                                    64 * sizeof(float)));
    for (float d : plane) ASSERT_EQ(d, 0.0f) << "depth not zeroed";
}

TEST(TextureZeroInit, FreshVolumeTexturesReadAsZero) {
    if (!MetalDevice::instance().available()) GTEST_SKIP() << "no Metal";
    // UE's tonemapper LUT is a 32^3 volume written by compute through a UAV;
    // it must read as zero before that, like every WDDM allocation.
    for (const bool uav : {true, false}) {
        SCOPED_TRACE(uav ? "UAV volume" : "sampled volume");
        BackendTexture3DDesc vd{};
        vd.width = vd.height = vd.depth = 32;
        vd.mip_levels = 2;
        vd.pixel_format = PixelFormat::RGBA16Float;
        vd.shader_resource = true;
        vd.unordered_access = uav;
        auto volume =
            MetalDevice::instance().create_texture_3d(vd, nullptr, 0, 0);
        ASSERT_TRUE(volume);
        for (unsigned mip = 0; mip < 2; ++mip) {
            const unsigned side = 32u >> mip;
            for (unsigned w : {0u, side / 2, side - 1}) {
                std::vector<u8> slice(side * side * 8, 0xEE);
                ASSERT_TRUE(
                    volume->readback_w_slice(mip, w, slice.data(), side * 8));
                for (u8 byte : slice) {
                    ASSERT_EQ(byte, 0u)
                        << "mip " << mip << " slice " << w << " not zeroed";
                }
            }
        }
    }
}
