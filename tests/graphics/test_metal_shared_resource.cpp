// SPDX-License-Identifier: LGPL-2.1-only
/// @file test_metal_shared_resource.cpp
/// @brief Shared resources of every shape on the real Metal backend: the
///        canonical-store layout, same-process openings, and two processes
///        exchanging contents in both directions (metal_shared_resource.hpp).
///
/// The contract is D3D11's (Learn, ID3D11Device::OpenSharedResource: "If a
/// shared texture is updated on one device ID3D11DeviceContext::Flush must be
/// called on that device"; measured by tools/oracle_probe/
/// d3d11_share_ers_probe: every subresource of mipmapped, array, cube,
/// multisampled, depth, block-compressed, planar-video, volume, buffer and
/// STAGING resources round-trips between two processes). The producer's
/// synchronization point here is publish_shared_writes(), the consumer's
/// acquire_shared_writes() -- what D3D11 Flush / Present / ReleaseSync and
/// AcquireSync / an opening call.
///
/// The second process is this test binary re-executed with a filter that
/// selects MetalSharedResourceChild.* (a fork alone cannot use Metal: the
/// framework is not fork-safe once initialised in the parent).

#include <gtest/gtest.h>

#include "graphics/contracts/backend_device.hpp"
#include "graphics/contracts/format_converter.hpp"
#include "graphics/metal/metal_device.hpp"
#include "graphics/metal/metal_shared_resource.hpp"

#include <mach-o/dyld.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

extern char** environ;

using namespace vitreo;
using namespace vitreo::graphics;

namespace {

u8 pattern(u32 salt, usize i) {
    return static_cast<u8>((i * 7u + salt * 13u + 3u) % 251u);
}

std::vector<u8> pattern_bytes(u32 salt, usize size) {
    std::vector<u8> bytes(size);
    for (usize i = 0; i < size; ++i) bytes[i] = pattern(salt, i);
    return bytes;
}

// The shapes both processes agree on.
constexpr unsigned kW = 16;
constexpr unsigned kH = 8;

BackendTexture2DDesc mip_array_desc() {
    BackendTexture2DDesc d{};
    d.width = kW;
    d.height = kH;
    d.mip_levels = 3;
    d.array_size = 2;
    d.pixel_format = PixelFormat::RGBA8Unorm;
    d.render_target = true;
    d.shader_resource = true;
    d.shareable = true;
    return d;
}

BackendTexture2DDesc msaa_desc() {
    BackendTexture2DDesc d{};
    d.width = kW;
    d.height = kH;
    d.pixel_format = PixelFormat::BGRA8Unorm;
    d.render_target = true;
    d.shader_resource = true;
    d.sample_count = 4;
    d.shareable = true;
    return d;
}

BackendTexture2DDesc depth_desc() {
    BackendTexture2DDesc d{};
    d.width = kW;
    d.height = kH;
    d.array_size = 2;
    d.pixel_format = PixelFormat::D24Unorm_S8Uint;
    d.depth_stencil = true;
    d.shareable = true;
    return d;
}

BackendTexture2DDesc nv12_desc() {
    BackendTexture2DDesc d{};
    d.width = kW;
    d.height = kH;
    d.pixel_format = PixelFormat::NV12;
    d.shader_resource = true;
    d.render_target = true;
    d.shareable = true;
    return d;
}

BackendTexture3DDesc volume_desc() {
    BackendTexture3DDesc d{};
    d.width = kW;
    d.height = kH;
    d.depth = 4;
    d.mip_levels = 2;
    d.pixel_format = PixelFormat::RGBA8Unorm;
    d.shader_resource = true;
    d.shareable = true;
    return d;
}

constexpr usize kBufferBytes = 1024;
constexpr usize kMemoryBytes = 4096;

// Fill / check every subresource of the mip-array texture with salt.
bool fill_mip_array(BackendTexture2D& t, u32 salt) {
    for (unsigned slice = 0; slice < 2; ++slice) {
        for (unsigned mip = 0; mip < 3; ++mip) {
            const unsigned w = std::max(1u, kW >> mip);
            const unsigned h = std::max(1u, kH >> mip);
            const auto bytes = pattern_bytes(salt + slice * 3 + mip, w * h * 4);
            if (!t.update_slice(mip, slice, bytes.data(), w * 4)) return false;
        }
    }
    return true;
}

bool check_mip_array(BackendTexture2D& t, u32 salt) {
    for (unsigned slice = 0; slice < 2; ++slice) {
        for (unsigned mip = 0; mip < 3; ++mip) {
            const unsigned w = std::max(1u, kW >> mip);
            const unsigned h = std::max(1u, kH >> mip);
            std::vector<u8> got(w * h * 4);
            if (!t.readback_slice(mip, slice, got.data(), w * 4)) return false;
            if (got != pattern_bytes(salt + slice * 3 + mip, w * h * 4)) {
                return false;
            }
        }
    }
    return true;
}

// NV12: 8 luma rows then 4 chroma rows at a 16-byte pitch.
bool fill_nv12(BackendTexture2D& t, u32 salt) {
    const auto bytes = pattern_bytes(salt, kW * (kH + kH / 2));
    return t.update_slice(0, 0, bytes.data(), kW);
}

bool check_nv12(BackendTexture2D& t, u32 salt) {
    std::vector<u8> got(kW * (kH + kH / 2));
    return t.readback_slice(0, 0, got.data(), kW) &&
           got == pattern_bytes(salt, got.size());
}

// MSAA: a clear (a render pass), read through a resolve.
std::array<float, 4> msaa_color(u32 salt) {
    return {0.125f * static_cast<float>(salt % 7), 0.5f, 0.25f, 1.0f};
}

bool check_msaa(GraphicsBackendDevice& device, BackendTexture2D& t, u32 salt) {
    BackendTexture2DDesc one{};
    one.width = kW;
    one.height = kH;
    one.pixel_format = PixelFormat::BGRA8Unorm;
    one.render_target = true;
    one.shader_resource = true;
    auto resolved = device.create_texture_2d(one, nullptr, 0);
    if (!resolved ||
        !device.resolve_texture_2d(resolved.get(), 0, 0, &t, 0, 0,
                                   PixelFormat::BGRA8Unorm)) {
        return false;
    }
    std::vector<u8> got(kW * kH * 4);
    if (!resolved->readback(0, got.data(), kW * 4)) return false;
    const auto c = msaa_color(salt);
    const u8 b = static_cast<u8>(c[2] * 255.0f + 0.5f);
    const u8 g = static_cast<u8>(c[1] * 255.0f + 0.5f);
    const u8 r = static_cast<u8>(c[0] * 255.0f + 0.5f);
    for (usize i = 0; i < got.size(); i += 4) {
        if (std::abs(got[i] - b) > 1 || std::abs(got[i + 1] - g) > 1 ||
            std::abs(got[i + 2] - r) > 1 || got[i + 3] != 255) {
            return false;
        }
    }
    return true;
}

// D24S8 array: each slice cleared to its own depth and stencil.
bool clear_depth(BackendTexture2D& t, u32 salt) {
    for (unsigned slice = 0; slice < 2; ++slice) {
        if (!t.clear_depth_stencil_level(0.125f * static_cast<float>(salt + slice),
                                         static_cast<u8>(0x50 + salt + slice),
                                         0, slice, 1)) {
            return false;
        }
    }
    return true;
}

bool check_depth(BackendTexture2D& t, u32 salt) {
    for (unsigned slice = 0; slice < 2; ++slice) {
        std::vector<float> depth(kW * kH);
        std::vector<u8> stencil(kW * kH);
        if (!t.readback_depth_stencil_plane(0, slice, false, depth.data(),
                                            kW * sizeof(float)) ||
            !t.readback_depth_stencil_plane(0, slice, true, stencil.data(),
                                            kW)) {
            return false;
        }
        for (usize i = 0; i < depth.size(); ++i) {
            if (depth[i] != 0.125f * static_cast<float>(salt + slice) ||
                stencil[i] != static_cast<u8>(0x50 + salt + slice)) {
                return false;
            }
        }
    }
    return true;
}

bool fill_volume(BackendTexture3D& t, u32 salt) {
    for (unsigned mip = 0; mip < 2; ++mip) {
        const unsigned w = kW >> mip, h = kH >> mip, d = 4u >> mip;
        const auto bytes = pattern_bytes(salt + mip, w * h * d * 4);
        if (!t.update(mip, bytes.data(), w * 4, w * h * 4)) return false;
    }
    return true;
}

bool check_volume(BackendTexture3D& t, u32 salt) {
    for (unsigned mip = 0; mip < 2; ++mip) {
        const unsigned w = kW >> mip, h = kH >> mip, d = 4u >> mip;
        const auto expected = pattern_bytes(salt + mip, w * h * d * 4);
        for (unsigned z = 0; z < d; ++z) {
            std::vector<u8> got(w * h * 4);
            if (!t.readback_w_slice(mip, z, got.data(), w * 4) ||
                std::memcmp(got.data(), expected.data() + z * w * h * 4,
                            got.size()) != 0) {
                return false;
            }
        }
    }
    return true;
}

bool check_buffer(BackendBuffer& b, u32 salt) {
    std::vector<u8> got(kBufferBytes);
    return b.read(0, got.data(), got.size()) &&
           got == pattern_bytes(salt, kBufferBytes);
}

bool check_memory(BackendSharedMemory& m, u32 salt) {
    return m.acquire() && m.size() >= kMemoryBytes &&
           std::memcmp(m.data(), pattern_bytes(salt, kMemoryBytes).data(),
                       kMemoryBytes) == 0;
}

void finish(GraphicsBackendDevice& device) {
    auto completion = device.publish_shared_writes();
    if (completion) (void)completion->wait();
    (void)device.wait_idle();
}

std::string self_path() {
    char buffer[4096];
    uint32_t size = sizeof(buffer);
    return _NSGetExecutablePath(buffer, &size) == 0 ? std::string(buffer)
                                                    : std::string();
}

}  // namespace

TEST(SharedStoreLayout, EverySubresourceSampleAspectAndPlane) {
    usize bytes = 0;
    SharedStoreRecord r{};
    r.kind = SharedStoreKind::Texture2D;
    r.pixel_format = static_cast<u32>(PixelFormat::RGBA8Unorm);
    r.width = kW;
    r.height = kH;
    r.mip_levels = 3;
    r.array_size = 2;
    auto regions = shared_store_layout(r, bytes);
    ASSERT_EQ(regions.size(), 6u);  // D3D subresource order
    usize end = 0;
    for (usize i = 0; i < regions.size(); ++i) {
        EXPECT_EQ(regions[i].slice, i / 3);
        EXPECT_EQ(regions[i].level, i % 3);
        EXPECT_EQ(regions[i].offset % 256u, 0u);
        EXPECT_GE(regions[i].offset, end);
        EXPECT_EQ(regions[i].bytes_per_row, (kW >> (i % 3)) * 4u);
        end = regions[i].offset + regions[i].bytes_per_image;
    }
    EXPECT_GE(bytes, end);
    // A combined depth+stencil format: two aspects per subresource.
    r.pixel_format = static_cast<u32>(PixelFormat::D24Unorm_S8Uint);
    r.mip_levels = 1;
    regions = shared_store_layout(r, bytes);
    ASSERT_EQ(regions.size(), 4u);
    EXPECT_EQ(regions[0].bytes_per_row, kW * 4u);
    EXPECT_EQ(regions[1].bytes_per_row, kW * 1u);
    EXPECT_NE(regions[0].blit_options, regions[1].blit_options);
    // Multisampled: every sample of every subresource.
    r.pixel_format = static_cast<u32>(PixelFormat::BGRA8Unorm);
    r.array_size = 1;
    r.sample_count = 4;
    regions = shared_store_layout(r, bytes);
    ASSERT_EQ(regions.size(), 4u);
    for (u32 s = 0; s < 4; ++s) EXPECT_EQ(regions[s].sample, s);
    // NV12: the luma plane, then the half-size chroma plane whose rows hold
    // as many bytes (Learn DXGI_FORMAT NV12).
    r.pixel_format = static_cast<u32>(PixelFormat::NV12);
    r.sample_count = 1;
    regions = shared_store_layout(r, bytes);
    ASSERT_EQ(regions.size(), 2u);
    EXPECT_EQ(regions[0].plane, 0u);
    EXPECT_EQ(regions[1].plane, 1u);
    EXPECT_EQ(regions[1].width, kW / 2);
    EXPECT_EQ(regions[1].height, kH / 2);
    EXPECT_EQ(regions[1].bytes_per_row, kW);
    // Block-compressed: rows of 4x4 blocks.
    r.pixel_format = static_cast<u32>(PixelFormat::BC1_RGBA);
    regions = shared_store_layout(r, bytes);
    ASSERT_EQ(regions.size(), 1u);
    EXPECT_EQ(regions[0].bytes_per_row, (kW / 4) * 8u);
    EXPECT_EQ(regions[0].bytes_per_image, (kW / 4) * 8u * (kH / 4));
}

TEST(MetalSharedResource, SameProcessOpeningSharesTheLocalTexture) {
    auto& device = MetalDevice::instance();
    if (!device.available()) GTEST_SKIP() << "no Metal device";
    EXPECT_TRUE(device.supports_extended_sharing());
    EXPECT_TRUE(device.supports_shared_texture(PixelFormat::BC1_RGBA));
    EXPECT_TRUE(device.supports_shared_texture(PixelFormat::D32Float));
    EXPECT_TRUE(device.supports_shared_texture(PixelFormat::NV12));
    auto texture = device.create_texture_2d(mip_array_desc(), nullptr, 0);
    ASSERT_NE(texture, nullptr);
    ASSERT_TRUE(texture->desc().shareable);
    ASSERT_TRUE(fill_mip_array(*texture, 1));
    // A mipmapped array has no IOSurface form: its store exists once
    // exported.
    EXPECT_EQ(texture->shared_handle(), 0u);
    const u64 id = texture->export_shared();
    ASSERT_NE(id, 0u);
    EXPECT_EQ(texture->shared_handle(), id);
    EXPECT_EQ(texture->export_shared(), id);
    auto opening = device.open_shared_resource(id);
    ASSERT_EQ(opening.kind, GraphicsBackendDevice::SharedOpening::Kind::Texture2D);
    ASSERT_NE(opening.texture_2d, nullptr);
    EXPECT_EQ(opening.texture_2d->desc().mip_levels, 3u);
    EXPECT_EQ(opening.texture_2d->desc().array_size, 2u);
    EXPECT_TRUE(check_mip_array(*opening.texture_2d, 1));
    // Writes through the opening are the creator's (one queue, one texture).
    ASSERT_TRUE(fill_mip_array(*opening.texture_2d, 5));
    EXPECT_TRUE(check_mip_array(*texture, 5));
}

// The consumer half, run in a second process by the test below.
TEST(MetalSharedResourceChild, OpensChecksAndWritesBack) {
    const char* ids = std::getenv("VITREO_SHARED_RESOURCE_CHILD");
    if (ids == nullptr) GTEST_SKIP() << "runs as the second process only";
    auto& device = MetalDevice::instance();
    if (!device.available()) std::_Exit(90);
    u64 id[7] = {};
    if (std::sscanf(ids, "%llx,%llx,%llx,%llx,%llx,%llx,%llx",
                    reinterpret_cast<unsigned long long*>(&id[0]),
                    reinterpret_cast<unsigned long long*>(&id[1]),
                    reinterpret_cast<unsigned long long*>(&id[2]),
                    reinterpret_cast<unsigned long long*>(&id[3]),
                    reinterpret_cast<unsigned long long*>(&id[4]),
                    reinterpret_cast<unsigned long long*>(&id[5]),
                    reinterpret_cast<unsigned long long*>(&id[6])) != 7) {
        std::_Exit(91);
    }
    using Kind = GraphicsBackendDevice::SharedOpening::Kind;
    auto mips = device.open_shared_resource(id[0]);
    auto buffer = device.open_shared_resource(id[1]);
    auto msaa = device.open_shared_resource(id[2]);
    auto depth = device.open_shared_resource(id[3]);
    auto nv12 = device.open_shared_resource(id[4]);
    auto volume = device.open_shared_resource(id[5]);
    auto memory = device.open_shared_resource(id[6]);
    if (mips.kind != Kind::Texture2D || buffer.kind != Kind::Buffer ||
        msaa.kind != Kind::Texture2D || depth.kind != Kind::Texture2D ||
        nv12.kind != Kind::Texture2D || volume.kind != Kind::Texture3D ||
        memory.kind != Kind::Memory) {
        std::_Exit(92);
    }
    // The consumer's synchronization point.
    device.acquire_shared_writes();
    int code = 0;
    if (!check_mip_array(*mips.texture_2d, 1)) code |= 1;
    if (!check_buffer(*buffer.buffer, 2)) code |= 2;
    if (!check_msaa(device, *msaa.texture_2d, 3)) code |= 4;
    if (!check_depth(*depth.texture_2d, 1)) code |= 8;
    if (!check_nv12(*nv12.texture_2d, 4)) code |= 16;
    if (!check_volume(*volume.texture_3d, 6)) code |= 32;
    if (!check_memory(*memory.memory, 7)) code |= 64;
    // Write everything back with new contents, then the producer's
    // synchronization point.
    const auto buffer_bytes = pattern_bytes(12, kBufferBytes);
    if (!fill_mip_array(*mips.texture_2d, 11) ||
        !buffer.buffer->update(0, buffer_bytes.data(), buffer_bytes.size()) ||
        !msaa.texture_2d->clear_render_target(msaa_color(13)) ||
        !clear_depth(*depth.texture_2d, 3) || !fill_nv12(*nv12.texture_2d, 14) ||
        !fill_volume(*volume.texture_3d, 16)) {
        code |= 128;
    }
    std::memcpy(memory.memory->data(), pattern_bytes(17, kMemoryBytes).data(),
                kMemoryBytes);
    memory.memory->note_written();
    finish(device);
    std::_Exit(code);
}

TEST(MetalSharedResource, TwoProcessesExchangeEveryShapeBothWays) {
    auto& device = MetalDevice::instance();
    if (!device.available()) GTEST_SKIP() << "no Metal device";
    auto mips = device.create_texture_2d(mip_array_desc(), nullptr, 0);
    BackendBufferDesc buffer_desc{};
    buffer_desc.byte_width = kBufferBytes;
    buffer_desc.shader_resource = true;
    buffer_desc.unordered_access = true;
    buffer_desc.shareable = true;
    const auto buffer_initial = pattern_bytes(2, kBufferBytes);
    auto buffer = device.create_buffer(buffer_desc, buffer_initial.data());
    auto msaa = device.create_texture_2d(msaa_desc(), nullptr, 0);
    auto depth = device.create_texture_2d(depth_desc(), nullptr, 0);
    auto nv12 = device.create_texture_2d(nv12_desc(), nullptr, 0);
    auto volume = device.create_texture_3d(volume_desc(), nullptr, 0, 0);
    auto memory = device.create_shared_memory(kMemoryBytes, {});
    ASSERT_NE(mips, nullptr);
    ASSERT_NE(buffer, nullptr);
    ASSERT_NE(msaa, nullptr);
    ASSERT_NE(depth, nullptr);
    ASSERT_NE(nv12, nullptr);
    ASSERT_NE(volume, nullptr);
    ASSERT_NE(memory, nullptr);
    ASSERT_TRUE(fill_mip_array(*mips, 1));
    ASSERT_TRUE(msaa->clear_render_target(msaa_color(3)));
    ASSERT_TRUE(clear_depth(*depth, 1));
    ASSERT_TRUE(fill_nv12(*nv12, 4));
    ASSERT_TRUE(fill_volume(*volume, 6));
    std::memcpy(memory->data(), pattern_bytes(7, kMemoryBytes).data(),
                kMemoryBytes);
    memory->note_written();
    const u64 ids[7] = {mips->export_shared(), buffer->shared_handle(),
                        msaa->export_shared(), depth->export_shared(),
                        nv12->export_shared(), volume->export_shared(),
                        memory->shared_handle()};
    for (const u64 id : ids) ASSERT_NE(id, 0u);
    finish(device);

    char env_ids[256];
    std::snprintf(env_ids, sizeof(env_ids),
                  "VITREO_SHARED_RESOURCE_CHILD=%llx,%llx,%llx,%llx,%llx,%llx,"
                  "%llx",
                  static_cast<unsigned long long>(ids[0]),
                  static_cast<unsigned long long>(ids[1]),
                  static_cast<unsigned long long>(ids[2]),
                  static_cast<unsigned long long>(ids[3]),
                  static_cast<unsigned long long>(ids[4]),
                  static_cast<unsigned long long>(ids[5]),
                  static_cast<unsigned long long>(ids[6]));
    std::vector<std::string> env_strings;
    for (char** e = environ; *e != nullptr; ++e) env_strings.emplace_back(*e);
    env_strings.emplace_back(env_ids);
    std::vector<char*> envp;
    for (auto& e : env_strings) envp.push_back(e.data());
    envp.push_back(nullptr);
    const std::string self = self_path();
    ASSERT_FALSE(self.empty());
    std::string filter =
        "--gtest_filter=MetalSharedResourceChild.OpensChecksAndWritesBack";
    char* argv[] = {const_cast<char*>(self.c_str()), filter.data(), nullptr};
    pid_t child = 0;
    ASSERT_EQ(::posix_spawn(&child, self.c_str(), nullptr, nullptr, argv,
                            envp.data()),
              0);
    int status = 0;
    ASSERT_EQ(::waitpid(child, &status, 0), child);
    ASSERT_TRUE(WIFEXITED(status));
    // Bits: 1 mips, 2 buffer, 4 msaa, 8 depth, 16 nv12, 32 volume,
    // 64 memory -- the child read the parent's contents; 128 its writes.
    EXPECT_EQ(WEXITSTATUS(status), 0);

    // The child's writes, published before it exited.
    device.acquire_shared_writes();
    EXPECT_TRUE(check_mip_array(*mips, 11));
    EXPECT_TRUE(check_buffer(*buffer, 12));
    EXPECT_TRUE(check_msaa(device, *msaa, 13));
    EXPECT_TRUE(check_depth(*depth, 3));
    EXPECT_TRUE(check_nv12(*nv12, 14));
    EXPECT_TRUE(check_volume(*volume, 16));
    EXPECT_TRUE(check_memory(*memory, 17));
}
