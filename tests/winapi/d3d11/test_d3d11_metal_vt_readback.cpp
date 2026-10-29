// SPDX-License-Identifier: LGPL-2.1-only

/// @file test_d3d11_metal_vt_readback.cpp
/// @brief VtFeedbackTest, readback: Map READ of STAGING resources after the
///        GPU writes that precede the copy.
///
/// Split from the former test_d3d11_metal.cpp; the fixture is
/// d3d11_test_metal_vt_feedback.hpp.

#include <gtest/gtest.h>

#include "graphics/metal/metal_submit_queue.hpp"

#include "d3d11_test_metal_vt_feedback.hpp"
#include "vt_feedback_shaders.h"

#include <algorithm>
#include <cstring>
#include <thread>
#include <utility>

namespace vitreo::winapi::d3d11_test {
namespace {

/// MetalSubmitQueue::set_append_hook_for_test: right after the first op of
/// `kind` joins the immediate stream, a second thread records a copy of its
/// own and submits the recording. The op is then in a list that is already
/// submitted, and the recording open when its recorder takes a completion
/// generation is the next one, which holds nothing. The recording thread
/// waits for it (join), so the interleaving is the same on every run.
struct SubmitAfterAppend {
    graphics::MetalSubmitQueue::TestAppendKind kind{};
    graphics::BackendBuffer* source{nullptr};
    graphics::BackendBuffer* destination{nullptr};
    bool fired{false};

    static void hook(graphics::MetalSubmitQueue::TestAppendKind kind,
                     void* context) {
        auto& self = *static_cast<SubmitAfterAppend*>(context);
        if (kind != self.kind || self.fired) return;
        self.fired = true;
        std::thread other([&self] {
            auto& device = graphics::MetalDevice::instance();
            EXPECT_TRUE(device.copy_buffer_region(self.destination, 0,
                                                  self.source, 0, 16));
            device.flush();
        });
        other.join();
    }
};

// -- 1. Readback ------------------------------------------------------------

// Learn (Map): Map READ of a STAGING resource returns its data after every
// GPU write submitted before the copy into it -- here a dispatch's UAV
// writes, copied through CopyResource.
TEST_F(VtFeedbackTest, MapReadSeesTheDispatchItsCopyFollows) {
    const GuestAddr cs = shader(18, vtfb_cs_fill, sizeof(vtfb_cs_fill));
    ASSERT_NE(cs, 0u);
    constexpr u32 kElements = 256;
    const GuestAddr out =
        buffer(kElements * 4, D3D11_USAGE_DEFAULT, D3D11_BIND_UNORDERED_ACCESS,
               0, D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, 4, nullptr);
    const GuestAddr uav = buffer_uav(out, 0, kElements);
    for (const u32 seed : {7u, 1000u, 77777u}) {
        constants({seed, 0, 0, 0});
        const std::array<u64, 1> uavs{uav};
        context_call(68, {0, 1, put(uavs), 0});  // CSSetUnorderedAccessViews
        context_call(69, {cs, 0, 0});            // CSSetShader
        context_call(41, {kElements / 64, 1, 1});  // Dispatch
        const auto words = read_buffer(out, kElements * 4);
        ASSERT_EQ(words.size(), kElements);
        for (u32 i = 0; i < kElements; ++i) {
            ASSERT_EQ(words[i], i * 3 + seed) << "seed " << seed << " i " << i;
        }
    }
}

// The same through a draw into a UINT target, a GPU copy chain
// (CopySubresourceRegion between DEFAULT textures and between DEFAULT
// buffers), and a CopySubresourceRegion into a STAGING sub-box.
TEST_F(VtFeedbackTest, MapReadSeesDrawsAndCopiesBeforeTheCopy) {
    const GuestAddr ps = shader(15, vtfb_ps_uint, sizeof(vtfb_ps_uint));
    const GuestAddr a = texture(16, 16, 1, 1, kR32Uint, D3D11_USAGE_DEFAULT,
                                D3D11_BIND_RENDER_TARGET, 0);
    const GuestAddr b = texture(16, 16, 1, 1, kR32Uint, D3D11_USAGE_DEFAULT,
                                D3D11_BIND_SHADER_RESOURCE, 0);
    const GuestAddr rtv = render_target_view(a, kR32Uint, 0);
    pipeline(ps);
    viewport(0, 0, 16, 16);
    const std::array<u64, 1> rtvs{rtv};
    context_call(33, {1, put(rtvs), 0});
    constants({0xC0FFEE01u, 0, 0, 0});
    draw();
    // b <- a, then a STAGING sub-box at (3, 5) <- b's (2,1)-(9,4).
    context_call(46, {b, 0, 0, 0, 0, a, 0, 0});  // CopySubresourceRegion
    const GuestAddr staging = texture(16, 16, 1, 1, kR32Uint,
                                      D3D11_USAGE_STAGING, 0,
                                      D3D11_CPU_ACCESS_READ);
    const std::array<u32, 6> box{2, 1, 0, 9, 4, 1};
    context_call(46, {staging, 0, 3, 5, 0, b, 0, put(box)});
    Mapped m = map(staging, 0, D3D11_MAP_READ);
    ASSERT_EQ(m.hr, d3d11::S_OK);
    usize copied = 0;
    for (u32 y = 5; y < 8; ++y) {
        for (u32 x = 3; x < 10; ++x) {
            const u32 v = memory.read_u32(m.data + y * m.row_pitch + x * 4)
                              .value();
            copied += v == 0xC0FFEE01u ? 1 : 0;
        }
    }
    EXPECT_EQ(copied, 21u) << "the 7x3 box carries the draw's value";
    unmap(staging, 0);

    // Buffers: UpdateSubresource, a GPU copy at an offset, then the
    // readback copy.
    std::array<u32, 16> words{};
    for (u32 i = 0; i < words.size(); ++i) words[i] = 0x5000 + i;
    const GuestAddr src = buffer(64, D3D11_USAGE_DEFAULT,
                                 D3D11_BIND_SHADER_RESOURCE, 0, 0, 0, nullptr);
    const GuestAddr dst = buffer(64, D3D11_USAGE_DEFAULT,
                                 D3D11_BIND_SHADER_RESOURCE, 0, 0, 0, nullptr);
    context_call(48, {src, 0, 0, put(words), 0, 0});  // UpdateSubresource
    const std::array<u32, 6> range{0, 0, 0, 32, 1, 1};
    context_call(46, {dst, 0, 16, 0, 0, src, 0, put(range)});
    const auto got = read_buffer(dst, 64);
    ASSERT_EQ(got.size(), 16u);
    for (u32 i = 0; i < 8; ++i) {
        EXPECT_EQ(got[4 + i], 0x5000 + i) << "word " << 4 + i;
    }
}

// Learn (D3D11_MAP_FLAG_DO_NOT_WAIT): "If the GPU is busy ... Map returns
// DXGI_ERROR_WAS_STILL_DRAWING" -- here while the copy into the STAGING
// resource has not run, because the GPU queue is held on a fence ahead of
// it. A poll never answers S_OK early, and answers it once the GPU has run
// the copy, with the copied data.
TEST_F(VtFeedbackTest, DoNotWaitAnswersStillDrawingUntilTheCopyHasRun) {
    auto& backend = graphics::MetalDevice::instance();
    if (!backend.supports_fences()) GTEST_SKIP() << "no timeline fences";
    const GuestAddr cs = shader(18, vtfb_cs_fill, sizeof(vtfb_cs_fill));
    ASSERT_NE(cs, 0u);
    const GuestAddr out =
        buffer(256, D3D11_USAGE_DEFAULT, D3D11_BIND_UNORDERED_ACCESS, 0,
               D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, 4, nullptr);
    const GuestAddr uav = buffer_uav(out, 0, 64);
    std::vector<u32> init(16 * 16, 0x11111111u);
    const GuestAddr tex = texture(16, 16, 1, 1, kR32Uint, D3D11_USAGE_DEFAULT,
                                  D3D11_BIND_SHADER_RESOURCE, 0,
                                  {{put(init.data(), init.size() * 4), 64}});
    const GuestAddr staging_buffer = buffer(256, D3D11_USAGE_STAGING, 0,
                                            D3D11_CPU_ACCESS_READ, 0, 0,
                                            nullptr);
    const GuestAddr staging_texture = texture(16, 16, 1, 1, kR32Uint,
                                              D3D11_USAGE_STAGING, 0,
                                              D3D11_CPU_ACCESS_READ);
    for (int round = 0; round < 2; ++round) {
        SCOPED_TRACE(round == 0 ? "buffer" : "texture");
        auto hold = backend.create_fence(0);
        ASSERT_TRUE(hold && hold->valid());
        ASSERT_TRUE(hold->wait_gpu(1));  // everything after waits
        GuestAddr staging = 0;
        if (round == 0) {
            constants({41, 0, 0, 0});
            const std::array<u64, 1> uavs{uav};
            context_call(68, {0, 1, put(uavs), 0});
            context_call(69, {cs, 0, 0});
            context_call(41, {1, 1, 1});
            context_call(47, {staging_buffer, out});
            staging = staging_buffer;
        } else {
            const std::array<u32, 6> box{0, 0, 0, 16, 16, 1};
            std::vector<u32> rows(16 * 16, 0x22222222u);
            context_call(48, {tex, 0, put(box),
                              put(rows.data(), rows.size() * 4), 64, 64 * 16});
            context_call(47, {staging_texture, tex});
            staging = staging_texture;
        }
        for (int poll = 0; poll < 20; ++poll) {
            const Mapped early = map(staging, 0, D3D11_MAP_READ,
                                     D3D11_MAP_FLAG_DO_NOT_WAIT);
            ASSERT_EQ(early.hr, kDxgiErrorWasStillDrawing)
                << "poll " << poll << ": the copy is held behind the fence";
        }
        ASSERT_TRUE(hold->signal_cpu(1));
        const Mapped m = poll_map(staging, 0);
        ASSERT_EQ(m.hr, d3d11::S_OK) << "the copy has run: the poll completes";
        if (round == 0) {
            const auto words = words_at(m.data, 64);
            for (u32 i = 0; i < 64; ++i) EXPECT_EQ(words[i], i * 3 + 41) << i;
        } else {
            for (u32 y = 0; y < 16; ++y) {
                const auto row = words_at(m.data + y * m.row_pitch, 16);
                for (u32 x = 0; x < 16; ++x) {
                    EXPECT_EQ(row[x], 0x22222222u) << x << "," << y;
                }
            }
        }
        unmap(staging, 0);
    }
}

// Another thread's submit right after the copy into STAGING joins the
// recording (UE's RHI thread and the readback poll are different threads;
// every host thread records onto one queue): the copy is in the submitted
// list, the open recording holds nothing. Map READ must still wait for
// exactly that copy, DO_NOT_WAIT must stop answering WAS_STILL_DRAWING once
// it has run -- not wait for work nobody records -- and the data is the
// copy's.
TEST_F(VtFeedbackTest, ReadbackWaitsForTheGenerationItsCopyWasRecordedIn) {
    auto& backend = graphics::MetalDevice::instance();
    graphics::BackendBufferDesc bd{};
    bd.byte_width = 16;
    bd.shader_resource = true;
    const std::array<u32, 4> side{1, 2, 3, 4};
    auto side_src = backend.create_buffer(bd, side.data());
    auto side_dst = backend.create_buffer(bd, nullptr);
    ASSERT_TRUE(side_src && side_dst);

    std::vector<u32> init(16 * 16, 0x33333333u);
    const GuestAddr tex = texture(16, 16, 1, 1, kR32Uint, D3D11_USAGE_DEFAULT,
                                  D3D11_BIND_SHADER_RESOURCE, 0,
                                  {{put(init.data(), init.size() * 4), 64}});
    std::vector<u32> buf_init(64);
    for (u32 i = 0; i < 64; ++i) buf_init[i] = 0x4400 + i;
    const GuestAddr buf = buffer(256, D3D11_USAGE_DEFAULT,
                                 D3D11_BIND_SHADER_RESOURCE, 0, 0, 0,
                                 buf_init.data());
    for (const bool blocking : {false, true}) {
        SCOPED_TRACE(blocking ? "Map READ" : "Map READ | DO_NOT_WAIT");
        // Texture: CopyResource records a DownloadTexture.
        {
            const GuestAddr staging = texture(16, 16, 1, 1, kR32Uint,
                                              D3D11_USAGE_STAGING, 0,
                                              D3D11_CPU_ACCESS_READ);
            SubmitAfterAppend race{
                graphics::MetalSubmitQueue::TestAppendKind::DownloadTexture,
                side_src.get(), side_dst.get()};
            graphics::MetalSubmitQueue::set_append_hook_for_test(
                &SubmitAfterAppend::hook, &race);
            context_call(47, {staging, tex});
            graphics::MetalSubmitQueue::set_append_hook_for_test(nullptr,
                                                                 nullptr);
            ASSERT_TRUE(race.fired);
            const Mapped m = blocking ? map(staging, 0, D3D11_MAP_READ)
                                      : poll_map(staging, 0);
            EXPECT_EQ(m.hr, d3d11::S_OK)
                << "texture: the copy ran in the other thread's list";
            if (m.hr == d3d11::S_OK) {
                usize right = 0;
                for (u32 y = 0; y < 16; ++y) {
                    for (const u32 v :
                         words_at(m.data + y * m.row_pitch, 16)) {
                        right += v == 0x33333333u ? 1 : 0;
                    }
                }
                EXPECT_EQ(right, 256u) << "texture: the data is the copy's";
                unmap(staging, 0);
            }
        }
        // Buffer: CopyResource records a CopyBuffer.
        {
            const GuestAddr staging = buffer(256, D3D11_USAGE_STAGING, 0,
                                             D3D11_CPU_ACCESS_READ, 0, 0,
                                             nullptr);
            SubmitAfterAppend race{
                graphics::MetalSubmitQueue::TestAppendKind::CopyBuffer,
                side_src.get(), side_dst.get()};
            graphics::MetalSubmitQueue::set_append_hook_for_test(
                &SubmitAfterAppend::hook, &race);
            context_call(47, {staging, buf});
            graphics::MetalSubmitQueue::set_append_hook_for_test(nullptr,
                                                                 nullptr);
            ASSERT_TRUE(race.fired);
            const Mapped m = blocking ? map(staging, 0, D3D11_MAP_READ)
                                      : poll_map(staging, 0);
            EXPECT_EQ(m.hr, d3d11::S_OK)
                << "buffer: the copy ran in the other thread's list";
            if (m.hr == d3d11::S_OK) {
                EXPECT_EQ(words_at(m.data, 64), buf_init)
                    << "buffer: the data is the copy's";
                unmap(staging, 0);
            }
        }
    }
}

// Learn (D3D11_MAP): READ_WRITE on a STAGING resource with both CPU access
// flags shows the GPU's copy, keeps the CPU's writes across Unmap, and the
// GPU then copies the written contents out.
TEST_F(VtFeedbackTest, MapReadWriteOnStagingKeepsBothSides) {
    std::vector<u32> init(64);
    for (u32 i = 0; i < 64; ++i) init[i] = 0x6600 + i;
    const GuestAddr gpu = buffer(256, D3D11_USAGE_DEFAULT,
                                 D3D11_BIND_SHADER_RESOURCE, 0, 0, 0,
                                 init.data());
    const GuestAddr staging =
        buffer(256, D3D11_USAGE_STAGING, 0,
               D3D11_CPU_ACCESS_READ | D3D11_CPU_ACCESS_WRITE, 0, 0, nullptr);
    context_call(47, {staging, gpu});
    Mapped m = map(staging, 0, D3D11_MAP_READ_WRITE);
    ASSERT_EQ(m.hr, d3d11::S_OK);
    EXPECT_EQ(words_at(m.data, 64), init) << "READ_WRITE shows the GPU copy";
    for (u32 i = 0; i < 64; i += 2) {
        ASSERT_TRUE(memory.write_u32(m.data + i * 4, 0x7700 + i).is_ok());
    }
    unmap(staging, 0);
    std::vector<u32> want = init;
    for (u32 i = 0; i < 64; i += 2) want[i] = 0x7700 + i;
    m = map(staging, 0, D3D11_MAP_READ);
    ASSERT_EQ(m.hr, d3d11::S_OK);
    EXPECT_EQ(words_at(m.data, 64), want) << "the CPU writes stay";
    unmap(staging, 0);
    const GuestAddr back = buffer(256, D3D11_USAGE_DEFAULT,
                                  D3D11_BIND_SHADER_RESOURCE, 0, 0, 0,
                                  nullptr);
    context_call(47, {back, staging});
    EXPECT_EQ(read_buffer(back, 256), want) << "and reach the GPU";

    // A STAGING texture the same way.
    std::vector<u32> texels(8 * 8, 0x01010101u);
    const GuestAddr tex = texture(8, 8, 1, 1, kR32Uint, D3D11_USAGE_DEFAULT,
                                  D3D11_BIND_SHADER_RESOURCE, 0,
                                  {{put(texels.data(), texels.size() * 4), 32}});
    const GuestAddr staging_tex =
        texture(8, 8, 1, 1, kR32Uint, D3D11_USAGE_STAGING, 0,
                D3D11_CPU_ACCESS_READ | D3D11_CPU_ACCESS_WRITE);
    context_call(47, {staging_tex, tex});
    m = map(staging_tex, 0, D3D11_MAP_READ_WRITE);
    ASSERT_EQ(m.hr, d3d11::S_OK);
    EXPECT_EQ(memory.read_u32(m.data + 3 * m.row_pitch + 4 * 4).value(),
              0x01010101u);
    ASSERT_TRUE(memory.write_u32(m.data + 3 * m.row_pitch + 4 * 4, 0xABCD)
                    .is_ok());
    unmap(staging_tex, 0);
    context_call(47, {tex, staging_tex});
    const auto level = read_uint_level(tex, kR32Uint, 8, 8, 1, 0);
    ASSERT_EQ(level.size(), 64u);
    EXPECT_EQ(level[3 * 8 + 4], 0xABCDu);
    EXPECT_EQ(level[3 * 8 + 5], 0x01010101u);
}

// Learn (D3D11_MAPPED_SUBRESOURCE): each subresource of a STAGING Texture2D
// maps with its own RowPitch (at least a row of texels) and DepthPitch (at
// least its rows), and its texel (x, y) sits at y * RowPitch + x * size --
// for every mip and array slice, at widths that are no multiple of 16.
TEST_F(VtFeedbackTest, StagingTextureSubresourcesMapWithTheirPitches) {
    constexpr u32 kW = 13, kH = 7, kMips = 3, kSlices = 2;
    std::vector<std::pair<GuestAddr, u32>> inits;
    for (u32 s = 0; s < kSlices; ++s) {
        for (u32 k = 0; k < kMips; ++k) {
            const u32 w = std::max(1u, kW >> k), h = std::max(1u, kH >> k);
            const u32 pitch = w * 4 + 12;  // a padded source pitch
            std::vector<u8> rows(static_cast<usize>(pitch) * h, 0xCC);
            for (u32 y = 0; y < h; ++y) {
                for (u32 x = 0; x < w; ++x) {
                    const u32 v = (s << 24) | (k << 16) | (y << 8) | x;
                    std::memcpy(rows.data() + y * pitch + x * 4, &v, 4);
                }
            }
            inits.emplace_back(put(rows.data(), rows.size()), pitch);
        }
    }
    const GuestAddr tex = texture(kW, kH, kMips, kSlices, kR32Uint,
                                  D3D11_USAGE_DEFAULT,
                                  D3D11_BIND_SHADER_RESOURCE, 0, inits);
    const GuestAddr staging = texture(kW, kH, kMips, kSlices, kR32Uint,
                                      D3D11_USAGE_STAGING, 0,
                                      D3D11_CPU_ACCESS_READ);
    context_call(47, {staging, tex});
    for (u32 s = 0; s < kSlices; ++s) {
        for (u32 k = 0; k < kMips; ++k) {
            const u32 sub = s * kMips + k;
            const u32 w = std::max(1u, kW >> k), h = std::max(1u, kH >> k);
            const Mapped m = map(staging, sub, D3D11_MAP_READ);
            ASSERT_EQ(m.hr, d3d11::S_OK) << "subresource " << sub;
            EXPECT_GE(m.row_pitch, w * 4) << "subresource " << sub;
            EXPECT_GE(m.depth_pitch, m.row_pitch * (h - 1) + w * 4)
                << "subresource " << sub;
            usize wrong = 0;
            for (u32 y = 0; y < h; ++y) {
                const auto row = words_at(m.data + y * m.row_pitch, w);
                for (u32 x = 0; x < w; ++x) {
                    wrong += row[x] != ((s << 24) | (k << 16) | (y << 8) | x);
                }
            }
            EXPECT_EQ(wrong, 0u) << "subresource " << sub;
            unmap(staging, sub);
        }
    }
}

}  // namespace
}  // namespace vitreo::winapi::d3d11_test
