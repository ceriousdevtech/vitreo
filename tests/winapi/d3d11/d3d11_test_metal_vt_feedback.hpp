// SPDX-License-Identifier: LGPL-2.1-only
#pragma once
/// @file d3d11_test_metal_vt_feedback.hpp
/// @brief The fixture VtFeedbackTest: the virtual-texture feedback loop on the
///        real Metal backend.
///
/// Split from the former test_d3d11_metal.cpp; one named-namespace type, so
/// every test_d3d11_metal_vt_*.cpp registers the VtFeedbackTest suite with the
/// same fixture class.

#include "d3d11_test_metal_fixture.hpp"

#include "graphics/metal/metal_submit_queue.hpp"
#include "vt_feedback_shaders.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <thread>
#include <utility>
#include <vector>

namespace vitreo::winapi::d3d11_test {

// ---------------------------------------------------------------------------
// The virtual-texture feedback loop (.docs/rv-there-yet-bringup.md,
// 2026-10-20). A pixel shader reports the pages it needs through UAV atomics
// into a RWStructuredBuffer; the application copies the report to STAGING
// and maps it for READ, then renders the requested pages into an R16_UINT
// page table with mips and into a large B8G8R8A8 pool. Each link is a
// Windows contract tested on its own here. Expected values come from the
// D3D11.3 functional spec -- 5.2.3.1 (integer clears), 5.3.9 (UAVs), 16.13
// (discarded and helper pixels: "atomic memory updates etc. are valid but
// ignored"), 22.17 (atomics), 3.2.2 / 3.2.3 (conversions) -- and Learn
// (ID3D11DeviceContext::Map, D3D11_MAP_FLAG_DO_NOT_WAIT, CopyResource,
// CopySubresourceRegion, UpdateSubresource), not from a run. Shaders:
// vt_feedback_shaders.h (fxc, tools/oracle_probe/vt_feedback_shaders.hlsl).

class VtFeedbackTest : public D3D11MetalTest {
protected:
    static constexpr u32 kRgba8Unorm = 28;   // DXGI_FORMAT_R8G8B8A8_UNORM
    static constexpr u32 kR32Typeless = 39;  // DXGI_FORMAT_R32_TYPELESS
    static constexpr u32 kR32Uint = 42;      // DXGI_FORMAT_R32_UINT
    static constexpr u32 kR16Uint = 57;      // DXGI_FORMAT_R16_UINT
    static constexpr u32 kBgra8Unorm = 87;   // DXGI_FORMAT_B8G8R8A8_UNORM
    static constexpr u32 kSentinel = 0xEEEEEEEEu;
    /// The feedback scene's target: not a multiple of 16 either way, so the
    /// last tile column and row are partial. ceil(100/16) * ceil(61/16) =
    /// 7 * 4 = 28 tiles; ceil(100/2) * ceil(61/2) = 50 * 31 = 1550 quads.
    static constexpr u32 kWidth = 100;
    static constexpr u32 kHeight = 61;
    static constexpr u32 kFeedbackWords = 4096;

    void TearDown() override {
        graphics::MetalSubmitQueue::set_append_hook_for_test(nullptr, nullptr);
    }

    /// ID3D11Device::CreateBuffer (D3D11_BUFFER_DESC, optional pSysMem).
    GuestAddr buffer(u32 bytes, u32 usage, u32 bind, u32 cpu, u32 misc,
                     u32 stride, const void* init) {
        const std::array<u32, 6> bd{bytes, usage, bind, cpu, misc, stride};
        GuestAddr data = 0;
        if (init != nullptr) {
            data = put(std::array<u64, 2>{put(init, bytes), 0});
        }
        const GuestAddr out = out_slot();
        EXPECT_EQ(device_call(3, {put(bd), data, out}), d3d11::S_OK);
        return memory.read_u64(out).value();
    }
    /// ID3D11Device::CreateTexture2D; @p init is level 0 of slice 0 (one
    /// D3D11_SUBRESOURCE_DATA per subresource otherwise: @p inits).
    GuestAddr texture(u32 width, u32 height, u32 mips, u32 array, u32 format,
                      u32 usage, u32 bind, u32 cpu,
                      const std::vector<std::pair<GuestAddr, u32>>& inits = {},
                      u32 samples = 1) {
        const std::array<u32, 11> td{width, height, mips, array, format,
                                     samples, 0, usage, bind, cpu, 0};
        GuestAddr data = 0;
        if (!inits.empty()) {
            std::vector<u64> words;
            for (const auto& [address, pitch] : inits) {
                words.push_back(address);
                words.push_back(pitch);  // SysMemPitch, SysMemSlicePitch 0
            }
            data = put(words.data(), words.size() * 8);
        }
        const GuestAddr out = out_slot();
        EXPECT_EQ(device_call(5, {put(td), data, out}), d3d11::S_OK);
        return memory.read_u64(out).value();
    }
    /// D3D11_RTV_DIMENSION_TEXTURE2D at @p mip.
    GuestAddr render_target_view(GuestAddr resource, u32 format, u32 mip) {
        const std::array<u32, 5> desc{format, D3D11_RTV_DIMENSION_TEXTURE2D,
                                      mip, 0, 0};
        const GuestAddr out = out_slot();
        EXPECT_EQ(device_call(9, {resource, put(desc), out}), d3d11::S_OK);
        return memory.read_u64(out).value();
    }
    /// D3D11_SRV_DIMENSION_TEXTURE2D over mips [0, @p mips).
    GuestAddr texture_srv(GuestAddr resource, u32 format, u32 mips) {
        const std::array<u32, 6> desc{format, D3D11_SRV_DIMENSION_TEXTURE2D, 0,
                                      mips, 0, 0};
        const GuestAddr out = out_slot();
        EXPECT_EQ(device_call(7, {resource, put(desc), out}), d3d11::S_OK);
        return memory.read_u64(out).value();
    }
    /// D3D11_UAV_DIMENSION_BUFFER over elements [0, @p count).
    GuestAddr buffer_uav(GuestAddr resource, u32 format, u32 count,
                         u32 flags = 0) {
        const std::array<u32, 5> desc{format, D3D11_UAV_DIMENSION_BUFFER, 0,
                                      count, flags};
        const GuestAddr out = out_slot();
        EXPECT_EQ(device_call(8, {resource, put(desc), out}), d3d11::S_OK);
        return memory.read_u64(out).value();
    }
    /// The 16-byte b0 of every entry (Params p0..p3), bound to PS and CS.
    void constants(const std::array<u32, 4>& words) {
        if (cb == 0) {
            cb = buffer(16, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0,
                        0, 0, words.data());
            const std::array<u64, 1> list{cb};
            context_call(16, {0, 1, put(list)});  // PSSetConstantBuffers
            context_call(71, {0, 1, put(list)});  // CSSetConstantBuffers
            return;
        }
        context_call(48, {cb, 0, 0, put(words), 16, 16});  // UpdateSubresource
    }
    /// The full-screen triangle with @p ps: no input layout, no culling,
    /// the scissor test @p scissor_enable.
    void pipeline(GuestAddr ps, bool scissor_enable = false) {
        if (vs == 0) {
            vs = shader(12, vtfb_vs_fullscreen, sizeof(vtfb_vs_fullscreen));
        }
        ASSERT_NE(vs, 0u);
        ASSERT_NE(ps, 0u);
        const std::array<u32, 10> rs{3, 1, 0, 0, 0, 0, 1,
                                     scissor_enable ? 1u : 0u, 0, 0};
        const GuestAddr out = out_slot();
        ASSERT_EQ(device_call(22, {put(rs), out}), d3d11::S_OK);
        context_call(43, {memory.read_u64(out).value()});  // RSSetState
        context_call(11, {vs, 0, 0});                       // VSSetShader
        context_call(9, {ps, 0, 0});                        // PSSetShader
        context_call(17, {0});                              // IASetInputLayout
        context_call(24, {4});  // IASetPrimitiveTopology TRIANGLELIST
    }
    void viewport(float x, float y, float width, float height) {
        const std::array<float, 6> vp{x, y, width, height, 0, 1};
        context_call(44, {1, put(vp)});  // RSSetViewports
    }
    void scissor(i32 left, i32 top, i32 right, i32 bottom) {
        const std::array<i32, 4> rect{left, top, right, bottom};
        context_call(45, {1, put(rect)});  // RSSetScissorRects
    }
    void draw() { context_call(13, {3, 0}); }  // Draw(3, 0)

    struct Mapped {
        u32 hr{0};
        GuestAddr data{0};
        u32 row_pitch{0};
        u32 depth_pitch{0};
    };
    Mapped map(GuestAddr resource, u32 sub, u32 type, u32 flags = 0) {
        if (m_mapped_out == 0) m_mapped_out = put(std::array<u64, 2>{0, 0});
        EXPECT_TRUE(memory.write_u64(m_mapped_out, 0).is_ok());
        EXPECT_TRUE(memory.write_u64(m_mapped_out + 8, 0).is_ok());
        Mapped m{};
        m.hr = static_cast<u32>(
            context_call(14, {resource, sub, type, flags, m_mapped_out}));
        m.data = memory.read_u64(m_mapped_out).value();
        m.row_pitch = memory.read_u32(m_mapped_out + 8).value();
        m.depth_pitch = memory.read_u32(m_mapped_out + 12).value();
        return m;
    }
    void unmap(GuestAddr resource, u32 sub) { context_call(15, {resource, sub}); }
    /// Map(READ, DO_NOT_WAIT) until it stops answering WAS_STILL_DRAWING, for
    /// at most @p seconds; the last answer. Learn (D3D11_MAP_FLAG_DO_NOT_WAIT):
    /// a caller polls this way, and every poll must make progress.
    Mapped poll_map(GuestAddr resource, u32 sub, double seconds = 5.0) {
        const auto deadline =
            std::chrono::steady_clock::now() +
            std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                std::chrono::duration<double>(seconds));
        Mapped m{};
        for (;;) {
            m = map(resource, sub, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT);
            if (m.hr != kDxgiErrorWasStillDrawing ||
                std::chrono::steady_clock::now() >= deadline) {
                return m;
            }
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
    }
    std::vector<u32> words_at(GuestAddr address, usize count) {
        std::vector<u32> words(count, 0xDDDDDDDDu);
        EXPECT_TRUE(memory.read_bytes(address,
                                      reinterpret_cast<Byte*>(words.data()),
                                      count * 4).is_ok());
        return words;
    }
    /// Copy @p resource (a buffer of @p bytes) to a STAGING twin and Map it.
    std::vector<u32> read_buffer(GuestAddr resource, u32 bytes) {
        const GuestAddr staging = buffer(bytes, D3D11_USAGE_STAGING, 0,
                                         D3D11_CPU_ACCESS_READ, 0, 0, nullptr);
        context_call(47, {staging, resource});  // CopyResource
        const Mapped m = map(staging, 0, D3D11_MAP_READ);
        EXPECT_EQ(m.hr, d3d11::S_OK);
        if (m.hr != d3d11::S_OK) return {};
        auto words = words_at(m.data, bytes / 4);
        unmap(staging, 0);
        return words;
    }
    /// Level @p mip of a @p width x @p height x @p mips UINT texture,
    /// row-major, through a STAGING copy of the whole resource.
    std::vector<u32> read_uint_level(GuestAddr resource, u32 format, u32 width,
                                     u32 height, u32 mips, u32 mip) {
        const GuestAddr staging =
            texture(width, height, mips, 1, format, D3D11_USAGE_STAGING, 0,
                    D3D11_CPU_ACCESS_READ);
        context_call(47, {staging, resource});  // CopyResource
        const Mapped m = map(staging, mip, D3D11_MAP_READ);
        EXPECT_EQ(m.hr, d3d11::S_OK);
        if (m.hr != d3d11::S_OK) return {};
        const u32 w = std::max(1u, width >> mip);
        const u32 h = std::max(1u, height >> mip);
        const u32 texel = format == kR16Uint ? 2u : 4u;
        EXPECT_GE(m.row_pitch, w * texel);
        std::vector<u32> out(static_cast<usize>(w) * h);
        for (u32 y = 0; y < h; ++y) {
            for (u32 x = 0; x < w; ++x) {
                const GuestAddr at = m.data + y * m.row_pitch + x * texel;
                out[y * w + x] = texel == 2
                    ? memory.read_u16(at).value()
                    : memory.read_u32(at).value();
            }
        }
        unmap(staging, 0 + mip);
        return out;
    }

    /// The feedback scene: @p targets kWidth x kHeight R8G8B8A8 targets at
    /// RTV 0.. and @p report's UAV at u4 (UAVStartSlot 4, as the landscape
    /// shader), with @p dsv.
    void bind_feedback(GuestAddr ps, GuestAddr report_uav,
                       bool scissor_enable = false, u32 targets = 1,
                       GuestAddr dsv = 0, u32 samples = 1) {
        std::array<u64, 4> rtvs{};
        for (u32 i = 0; i < targets; ++i) {
            const GuestAddr target =
                texture(kWidth, kHeight, 1, 1, kRgba8Unorm,
                        D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET, 0, {},
                        samples);
            if (samples > 1) {
                const GuestAddr out = out_slot();  // the default view
                EXPECT_EQ(device_call(9, {target, 0, out}), d3d11::S_OK);
                rtvs[i] = memory.read_u64(out).value();
            } else {
                rtvs[i] = render_target_view(target, kRgba8Unorm, 0);
            }
        }
        pipeline(ps, scissor_enable);
        if (HasFatalFailure()) return;
        viewport(0, 0, kWidth, kHeight);
        const std::array<u64, 1> uavs{report_uav};
        const std::array<u32, 1> counts{0xFFFFFFFFu};
        // OMSetRenderTargetsAndUnorderedAccessViews(targets, rtvs, dsv, 4, 1,
        // uavs, counts).
        context_call(34, {targets, put(rtvs), dsv, 4, 1, put(uavs),
                          put(counts)});
    }
    /// The page-feedback report: element 0 a zero counter, the rest the
    /// sentinel.
    GuestAddr feedback_buffer() {
        std::vector<u32> init(kFeedbackWords, kSentinel);
        init[0] = 0;
        return buffer(kFeedbackWords * 4, D3D11_USAGE_DEFAULT,
                      D3D11_BIND_UNORDERED_ACCESS,
                      0, D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, 4, init.data());
    }
    /// The keys (y << 16 | x) of the reporting pixels: ((x | y) & mask) == 0,
    /// inside [x0, x1) x [y0, y1), and not discarded ((x & discard) == 0).
    static std::vector<u32> expected_keys(u32 mask, u32 x0, u32 y0, u32 x1,
                                          u32 y1, u32 discard = 0) {
        std::vector<u32> keys;
        for (u32 y = y0; y < y1; ++y) {
            for (u32 x = x0; x < x1; ++x) {
                if (((x | y) & mask) == 0 && (x & discard) == 0) {
                    keys.push_back((y << 16) | x);
                }
            }
        }
        return keys;
    }
    /// Entries [1, 1 + reported) of @p words must be @p keys, each exactly
    /// once and in any order; with @p sentinels, that many of them are still
    /// the sentinel instead (stores after a discard). Past them, untouched.
    static void expect_entries(const std::vector<u32>& words,
                               std::vector<u32> keys, u32 reported,
                               u32 sentinels = 0) {
        ASSERT_GE(words.size(), 1u + reported + 1u);
        std::vector<u32> got(words.begin() + 1, words.begin() + 1 + reported);
        const auto unwritten = static_cast<u32>(
            std::count(got.begin(), got.end(), kSentinel));
        EXPECT_EQ(unwritten, sentinels)
            << "entries of the reported slots left unwritten";
        got.erase(std::remove(got.begin(), got.end(), kSentinel), got.end());
        std::sort(got.begin(), got.end());
        std::sort(keys.begin(), keys.end());
        EXPECT_EQ(got, keys) << "each reporting pixel writes its slot once";
        for (usize i = 1 + reported; i < words.size(); ++i) {
            if (words[i] != kSentinel) {
                ADD_FAILURE() << "element " << i << " past the reported slots "
                              << "was written: 0x" << std::hex << words[i];
                break;
            }
        }
    }

    GuestAddr vs{0};
    GuestAddr cb{0};
    GuestAddr m_mapped_out{0};
};

}  // namespace vitreo::winapi::d3d11_test
