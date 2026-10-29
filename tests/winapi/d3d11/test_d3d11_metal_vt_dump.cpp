// SPDX-License-Identifier: LGPL-2.1-only

/// @file test_d3d11_metal_vt_dump.cpp
/// @brief VtFeedbackTest, diagnostics: the render-target dump and the readback
///        trace.
///
/// Split from the former test_d3d11_metal.cpp; the fixture is
/// d3d11_test_metal_vt_feedback.hpp.

#include <gtest/gtest.h>

#include "d3d11_test_metal_vt_feedback.hpp"
#include "vt_feedback_shaders.h"

#include <unistd.h>

#include <array>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace vitreo::winapi::d3d11_test {
namespace {

// -- Diagnostics: the render-target dump and the readback trace -------------

namespace {

std::string read_text_file(const std::string& path) {
    std::ifstream in(path);
    std::stringstream text;
    text << in.rdbuf();
    return text.str();
}

/// A fresh directory under the system temporary directory.
std::string fresh_directory(const char* tag) {
    const auto dir = std::filesystem::temp_directory_path() /
                     (std::string(tag) + "-" + std::to_string(::getpid()) +
                      "-" + std::to_string(std::chrono::steady_clock::now()
                                               .time_since_epoch()
                                               .count()));
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    return dir.string();
}

}  // namespace

// The dump opens its transfer log FIRST, before any image (2026-10-20: the
// log was opened last, and in all three game dumps every image after the
// second 9288^2 pool -- and the log -- failed to open, the error unrecorded).
// Images are at most 4096 on a side, a failed one names its error, and the
// trace lines follow the readback loop event by event in the documented
// "[d3d11-xfer] frame=<n> <event> ..." format.
TEST_F(VtFeedbackTest, DumpOpensTheTransferLogFirstAndTracesTheReadbackLoop) {
    const std::string dir = fresh_directory("vitreo-vtfb-dump");
    // A target, a 9288^2 SRV|UAV pool, an R16_UINT page table with mips.
    const GuestAddr target = texture(64, 64, 1, 1, kRgba8Unorm,
                                     D3D11_USAGE_DEFAULT,
                                     D3D11_BIND_RENDER_TARGET, 0);
    const GuestAddr pool =
        texture(9288, 9288, 1, 1, kBgra8Unorm, D3D11_USAGE_DEFAULT,
                D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS, 0);
    const GuestAddr table =
        texture(16, 16, 5, 1, kR16Uint, D3D11_USAGE_DEFAULT,
                D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, 0);
    ASSERT_TRUE(target != 0 && pool != 0 && table != 0);
    for (u32 k = 0; k < 5; ++k) {
        const std::array<float, 4> c{static_cast<float>(k + 1), 0, 0, 0};
        context_call(50, {render_target_view(table, kR16Uint, k), put(c)});
    }
    ASSERT_TRUE(d3d11.dump_render_targets_to(dir));
    const std::string stats = read_text_file(dir + "/stats.txt");
    EXPECT_NE(stats.find("transfers: logging to " + dir + "/transfers.txt"),
              std::string::npos)
        << stats.substr(0, 2000);
    EXPECT_EQ(stats.find("not written"), std::string::npos);
    EXPECT_EQ(stats.find("incomplete"), std::string::npos);
    EXPECT_NE(stats.find("    mip 1 8x8 nan=0 | c0 min=2 max=2 mean=2 "
                         "distinct=1"),
              std::string::npos)
        << "the page table's mips, as integers";
    bool pool_image = false;
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        const std::string name = entry.path().filename().string();
        if (name.find("_fmt87_9288x9288_every3.ppm") != std::string::npos) {
            pool_image = true;
            EXPECT_EQ(entry.file_size(), 17u + 3096u * 3096u * 3u)
                << "3096 x 3096, every third texel";
        }
    }
    EXPECT_TRUE(pool_image) << "the pool's image";

    // The loop, now traced.
    const GuestAddr ps = shader(15, vtfb_ps_feedback, sizeof(vtfb_ps_feedback));
    const GuestAddr report = feedback_buffer();
    const GuestAddr report_uav = buffer_uav(report, 0, kFeedbackWords);
    bind_feedback(ps, report_uav);
    if (HasFatalFailure()) return;
    context_call(51, {report_uav, put(std::array<u32, 4>{0, 0, 0, 0})});
    constants({15, 0, 0, 0});
    draw();
    const GuestAddr staging = buffer(kFeedbackWords * 4, D3D11_USAGE_STAGING,
                                     0, D3D11_CPU_ACCESS_READ, 0, 0, nullptr);
    context_call(47, {staging, report});  // CopyResource
    const std::array<u32, 2> qd{D3D11_QUERY_EVENT, 0};
    const GuestAddr out = out_slot();
    ASSERT_EQ(device_call(24, {put(qd), out}), d3d11::S_OK);
    const GuestAddr query = memory.read_u64(out).value();
    context_call(28, {query});  // End
    u64 hr = 1;
    while (hr != d3d11::S_OK) hr = context_call(29, {query, 0, 0, 0});
    const Mapped m = map(staging, 0, D3D11_MAP_READ);
    ASSERT_EQ(m.hr, d3d11::S_OK);
    unmap(staging, 0);
    const std::array<u32, 6> box{9000, 9100, 0, 9037, 9123, 1};
    std::vector<u32> rows(37 * 23, 0x11223344u);
    context_call(48, {pool, 0, put(box), put(rows.data(), rows.size() * 4),
                      37 * 4, 37 * 4 * 23});
    const std::string log = read_text_file(dir + "/transfers.txt");
    const auto has = [&](const std::string& text) {
        return log.find(text) != std::string::npos;
    };
    char hex[32];
    std::snprintf(hex, sizeof(hex), "0x%llx",
                  static_cast<unsigned long long>(report_uav));
    EXPECT_TRUE(has("[d3d11-xfer] frame=0 om-uav rtvs=1 dsv=0x0 start=4 "
                    "uavs=1 u4=" + std::string(hex)))
        << log;
    EXPECT_TRUE(has("initial=keep")) << log;
    EXPECT_TRUE(has("[d3d11-xfer] frame=0 clear-uav kind=Uint view=" +
                    std::string(hex)))
        << log;
    std::snprintf(hex, sizeof(hex), "0x%llx",
                  static_cast<unsigned long long>(staging));
    EXPECT_TRUE(has("[d3d11-xfer] frame=0 copy-to-staging op=CopyResource "
                    "dst=" + std::string(hex) + " buffer bytes=16384 sub=0"))
        << log;
    std::snprintf(hex, sizeof(hex), "0x%llx",
                  static_cast<unsigned long long>(query));
    EXPECT_TRUE(has("[d3d11-xfer] frame=0 event-end query=" +
                    std::string(hex)))
        << log;
    EXPECT_TRUE(has("event-getdata query=" + std::string(hex) +
                    " flags=0x0 size=0 -> 0x00000000 polls="))
        << log;
    std::snprintf(hex, sizeof(hex), "0x%llx",
                  static_cast<unsigned long long>(staging));
    EXPECT_TRUE(has("[d3d11-xfer] frame=0 map-staging buffer=" +
                    std::string(hex) + " sub=0 type=READ flags=0x0 -> "
                    "0x00000000 since_copy=0 pitch=16384 first64: 0000001c"))
        << "the report's counter, 28, reaches the CPU\n" << log;
    EXPECT_TRUE(has("large-write op=UpdateSubresource dst=")) << log;
    EXPECT_TRUE(has("box=(9000,9100,0)-(9037,9123,1) pitch=148/3404 -> "
                    "called"))
        << log;

    // A log that cannot be opened (its name taken by a directory) goes to
    // stderr, and stats.txt says why.
    D3D11Manager other{memory, true, graphics::MetalDevice::instance(), dxgi};
    const std::string blocked = fresh_directory("vitreo-vtfb-dump-blocked");
    std::filesystem::create_directories(blocked + "/transfers.txt");
    ::testing::internal::CaptureStderr();
    EXPECT_TRUE(other.dump_render_targets_to(blocked));
    const std::string err = ::testing::internal::GetCapturedStderr();
    const std::string blocked_stats = read_text_file(blocked + "/stats.txt");
    EXPECT_NE(blocked_stats.find("transfers: " + blocked +
                                 "/transfers.txt not opened: Is a directory"),
              std::string::npos)
        << blocked_stats;
    EXPECT_NE(err.find("[d3d11-xfer] frame=0 log " + blocked +
                       "/transfers.txt not opened: Is a directory"),
              std::string::npos)
        << err;
    std::filesystem::remove_all(dir);
    std::filesystem::remove_all(blocked);
}

}  // namespace
}  // namespace vitreo::winapi::d3d11_test
