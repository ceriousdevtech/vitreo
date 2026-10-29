// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS

#include "winapi/graphics/d3d9/d3d9_frame_dump.hpp"

#include "graphics/contracts/backend_device.hpp"

#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace vitreo::winapi {

namespace {

std::atomic<bool> g_pending{false};
std::string g_path;

void request_dump(int) noexcept {
    // Async-signal-safe: one relaxed store. The work happens on the render
    // thread at the next Present, where a readback is legal.
    g_pending.store(true, std::memory_order_relaxed);
}

void put_u32(std::vector<u8>& out, u32 v) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<u8>((v >> (i * 8)) & 0xFFu));
}
void put_u16(std::vector<u8>& out, u16 v) {
    out.push_back(static_cast<u8>(v & 0xFFu));
    out.push_back(static_cast<u8>(v >> 8));
}

/// 32-bit BGRA BMP. Chosen over raw pixels so the file opens anywhere without
/// a sidecar describing its dimensions.
bool write_bmp(const std::string& path, const std::vector<u8>& rgba,
               u32 width, u32 height) {
    const usize expected = static_cast<usize>(width) * height * 4u;
    if (width == 0 || height == 0 || rgba.size() < expected) return false;

    std::vector<u8> header;
    header.reserve(122);
    header.push_back('B');
    header.push_back('M');
    put_u32(header, static_cast<u32>(122 + expected));  // file size
    put_u16(header, 0);
    put_u16(header, 0);
    put_u32(header, 122);                               // pixel data offset
    put_u32(header, 108);                               // BITMAPV4HEADER
    put_u32(header, width);
    // Negative height: rows top-down, matching the readback order.
    put_u32(header, static_cast<u32>(-static_cast<i32>(height)));
    put_u16(header, 1);
    put_u16(header, 32);
    put_u32(header, 3);                                 // BI_BITFIELDS
    put_u32(header, static_cast<u32>(expected));
    put_u32(header, 2835);
    put_u32(header, 2835);
    put_u32(header, 0);
    put_u32(header, 0);
    put_u32(header, 0x00FF0000u);                       // red mask
    put_u32(header, 0x0000FF00u);                       // green mask
    put_u32(header, 0x000000FFu);                       // blue mask
    put_u32(header, 0xFF000000u);                       // alpha mask
    put_u32(header, 0x73524742u);                       // 'sRGB'
    while (header.size() < 122) header.push_back(0);

    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (f == nullptr) return false;
    bool ok = std::fwrite(header.data(), 1, header.size(), f) == header.size();
    // Readback is RGBA; BMP masks above declare BGRA byte order.
    std::vector<u8> row(static_cast<usize>(width) * 4u);
    for (u32 y = 0; ok && y < height; ++y) {
        const u8* src = rgba.data() + static_cast<usize>(y) * width * 4u;
        for (u32 x = 0; x < width; ++x) {
            row[x * 4 + 0] = src[x * 4 + 2];
            row[x * 4 + 1] = src[x * 4 + 1];
            row[x * 4 + 2] = src[x * 4 + 0];
            row[x * 4 + 3] = src[x * 4 + 3];
        }
        ok = std::fwrite(row.data(), 1, row.size(), f) == row.size();
    }
    std::fclose(f);
    return ok;
}

} // namespace

void d3d9_frame_dump_init() {
    static bool armed = false;
    if (armed) return;
    armed = true;
    const char* path = std::getenv("VITREO_FRAME_DUMP");
    if (path == nullptr || *path == '\0') return;
    g_path = path;
    struct sigaction sa {};
    sa.sa_handler = request_dump;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sigaction(SIGUSR2, &sa, nullptr);
    std::fprintf(stderr,
                 "[frame-dump] armed: kill -USR2 <guest pid> writes %s\n",
                 g_path.c_str());
}

bool d3d9_frame_dump_pending() noexcept {
    return g_pending.load(std::memory_order_relaxed);
}

void d3d9_frame_dump_write(graphics::BackendSwapChain& chain) {
    // Clear first: a readback that fails must not retry on every frame.
    g_pending.store(false, std::memory_order_relaxed);
    if (g_path.empty()) return;
    std::vector<u8> rgba;
    if (!chain.readback_texture_rgba(nullptr, rgba) || rgba.empty()) {
        std::fprintf(stderr, "[frame-dump] readback failed\n");
        return;
    }
    const auto& desc = chain.desc();
    if (write_bmp(g_path, rgba, desc.width, desc.height)) {
        std::fprintf(stderr, "[frame-dump] wrote %s (%ux%u)\n", g_path.c_str(),
                     desc.width, desc.height);
    } else {
        std::fprintf(stderr, "[frame-dump] write failed (%ux%u, %zu bytes)\n",
                     desc.width, desc.height, rgba.size());
    }
}

} // namespace vitreo::winapi
