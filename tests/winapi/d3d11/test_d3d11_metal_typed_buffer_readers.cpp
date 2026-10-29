// SPDX-License-Identifier: LGPL-2.1-only

/// @file test_d3d11_metal_typed_buffer_readers.cpp
/// @brief D3D11TypedBufferReaderTest: typed buffer views read by a pixel or
///        vertex shader, flat or through a descriptor table.
///
/// Split from the former test_d3d11_metal.cpp; the fixture is
/// d3d11_test_metal_typed_buffer.hpp.

#include <gtest/gtest.h>

#include "graphics/metal/metal_submit_queue.hpp"

#include "d3d11_test_metal_typed_buffer.hpp"
#include "../../graphics/typed_buffer_offset_shaders.h"

#include <array>
#include <string>

namespace vitreo::winapi::d3d11_test {
namespace {

// Which shader stage reads the view, and through which binding model: t0
// is inside the flat window, t9 past it (BACKEND_MAX_TEXTURE_SLOTS), so the
// stage binds its views through a descriptor table -- UE5's shaders bind
// that many. The vertex readers are UE's landscape vertex factory: the
// per-section arrays are read in the vertex shader.
enum class Reader { PixelFlat, PixelTable, VertexFlat, VertexTable };

class D3D11TypedBufferReaderTest
    : public D3D11TypedBufferOffsetTest,
      public ::testing::WithParamInterface<Reader> {
protected:
    [[nodiscard]] bool vertex() const {
        return GetParam() == Reader::VertexFlat ||
               GetParam() == Reader::VertexTable;
    }
    [[nodiscard]] u32 slot() const {
        return GetParam() == Reader::PixelTable ||
                       GetParam() == Reader::VertexTable
                   ? 9u
                   : 0u;
    }
    /// The target, and the reader's shaders: a full-target triangle whose
    /// pixel x reads element x % 8 (pixel readers), or eight one-column
    /// quads whose instance k carries element k (vertex readers).
    void bind_reader() {
        GuestAddr ps = 0;
        GuestAddr reader_vs = 0;
        switch (GetParam()) {
        case Reader::PixelFlat:
            ps = shader(15, tbo_ps_read_view, sizeof(tbo_ps_read_view));
            break;
        case Reader::PixelTable:
            ps = shader(15, tbo_ps_read_view_t9, sizeof(tbo_ps_read_view_t9));
            break;
        case Reader::VertexFlat:
            reader_vs = shader(12, tbo_vs_read_view, sizeof(tbo_vs_read_view));
            ps = shader(15, tbo_ps_value, sizeof(tbo_ps_value));
            break;
        case Reader::VertexTable:
            reader_vs =
                shader(12, tbo_vs_read_view_t9, sizeof(tbo_vs_read_view_t9));
            ps = shader(15, tbo_ps_value, sizeof(tbo_ps_value));
            break;
        }
        ASSERT_NE(ps, 0u);
        bind_target(ps, reader_vs, vertex() ? 5u /* TRIANGLESTRIP */ : 4u);
    }
    /// {VS,PS}SetShaderResources(slot(), 1, {view}).
    void read_view(GuestAddr view) {
        const std::array<u64, 1> list{view};
        context_call(vertex() ? 25 : 8, {slot(), 1, put(list)});
    }
    void read_draw() {
        if (vertex()) {
            context_call(21, {4, 8, 0, 0});  // DrawInstanced(4, 8, 0, 0)
        } else {
            draw();
        }
    }
};

INSTANTIATE_TEST_SUITE_P(
    Readers, D3D11TypedBufferReaderTest,
    ::testing::Values(Reader::PixelFlat, Reader::PixelTable,
                      Reader::VertexFlat, Reader::VertexTable),
    [](const ::testing::TestParamInfo<Reader>& info) {
        switch (info.param) {
        case Reader::PixelFlat: return std::string("PixelFlat");
        case Reader::PixelTable: return std::string("PixelTable");
        case Reader::VertexFlat: return std::string("VertexFlat");
        case Reader::VertexTable: return std::string("VertexTable");
        }
        return std::string("Unknown");
    });

// Draw with contents A, Map(DISCARD) to B, draw again: the two draws share
// one render pass (nothing between them ends it), and each sees its own
// contents -- A in the left eight columns, B in the right.
TEST_P(D3D11TypedBufferReaderTest, MapDiscardBetweenDrawsInOnePass) {
    const u32 bytes = kElements * 4;
    const GuestAddr data =
        buffer(bytes, D3D11_USAGE_DYNAMIC, D3D11_BIND_SHADER_RESOURCE,
               D3D11_CPU_ACCESS_WRITE, 0, nullptr);
    const GuestAddr view = srv(data, kR32Float, kSection, kSection);
    bind_reader();
    read_view(view);
    auto& queue = graphics::MetalSubmitQueue::instance();
    const u64 folded = queue.passes_folded();
    const u64 skips = d3d11_skip_events();

    const auto a = ramp(100000.0f);
    map_write(data, D3D11_MAP_WRITE_DISCARD, 0, a.data(), bytes);
    columns(0, 8);
    read_draw();
    const auto b = ramp(200000.0f);
    map_write(data, D3D11_MAP_WRITE_DISCARD, 0, b.data(), bytes);
    columns(8, 8);
    read_draw();

    const auto texels = read_target();
    expect_columns(texels, 0, 100000.0f, kSection, "A");
    expect_columns(texels, 8, 200000.0f, kSection, "B");
    EXPECT_GE(queue.passes_folded() - folded, 1u)
        << "the second draw did not fold into the first's pass";
    EXPECT_EQ(d3d11_skip_events(), skips);
}

// The same with UpdateSubresource on a DEFAULT buffer: a box over the view's
// bytes replaces A with B between the two draws.
TEST_P(D3D11TypedBufferReaderTest, UpdateSubresourceBetweenDrawsInOnePass) {
    const u32 bytes = kElements * 4;
    const auto a = ramp(100000.0f);
    const GuestAddr data = buffer(bytes, D3D11_USAGE_DEFAULT,
                                  D3D11_BIND_SHADER_RESOURCE, 0, 0, a.data());
    const GuestAddr view = srv(data, kR32Float, kSection, kSection);
    bind_reader();
    read_view(view);
    columns(0, 8);
    read_draw();
    const auto b = ramp(200000.0f);
    // D3D11_BOX {left, top, front, right, bottom, back}: the view's bytes.
    const std::array<u32, 6> box{kSection * 4, 0, 0, 2 * kSection * 4, 1, 1};
    context_call(48, {data, 0, put(box), put(b.data() + kSection,
                                             kSection * 4), 0, 0});
    columns(8, 8);
    read_draw();

    const auto texels = read_target();
    expect_columns(texels, 0, 100000.0f, kSection, "A");
    expect_columns(texels, 8, 200000.0f, kSection, "B");
}

// ID3D11DeviceContext1::UpdateSubresource1 with D3D11_COPY_DISCARD (2) and
// then D3D11_COPY_NO_OVERWRITE (1) between three draws: A, B, then a third
// write to elements the first two draws never read.
TEST_P(D3D11TypedBufferReaderTest, UpdateSubresource1CopyFlagsBetweenDraws) {
    const u32 bytes = kElements * 4;
    const auto a = ramp(100000.0f);
    const GuestAddr data = buffer(bytes, D3D11_USAGE_DEFAULT,
                                  D3D11_BIND_SHADER_RESOURCE, 0, 0, a.data());
    const GuestAddr view = srv(data, kR32Float, kSection, kSection);
    const GuestAddr later = srv(data, kR32Float, 2 * kSection + 1, 8);
    bind_reader();
    read_view(view);
    columns(0, 8);
    read_draw();
    const auto b = ramp(200000.0f);
    context_call(116, {data, 0, 0, put(b.data(), bytes), 0, 0,
                       2 /* D3D11_COPY_DISCARD */});
    columns(8, 8);
    read_draw();
    auto texels = read_target();
    expect_columns(texels, 0, 100000.0f, kSection, "A");
    expect_columns(texels, 8, 200000.0f, kSection, "B");

    // NO_OVERWRITE into [2 * 270 + 1, +8), which no earlier draw read.
    const auto c = ramp(300000.0f);
    const std::array<u32, 6> box{(2 * kSection + 1) * 4, 0, 0,
                                 (2 * kSection + 9) * 4, 1, 1};
    context_call(116, {data, 0, put(box),
                       put(c.data() + 2 * kSection + 1, 32), 0, 0,
                       1 /* D3D11_COPY_NO_OVERWRITE */});
    read_view(later);
    columns(0, 8);
    read_draw();
    texels = read_target();
    expect_columns(texels, 0, 300000.0f, 2 * kSection + 1, "C");
    expect_columns(texels, 8, 200000.0f, kSection, "B kept");
}

// Map(NO_OVERWRITE) of a disjoint region between two draws of one pass: the
// first draw reads sub-array 1 as Map(DISCARD) left it, the second reads
// sub-array 2 as NO_OVERWRITE rewrote it.
TEST_P(D3D11TypedBufferReaderTest, MapNoOverwriteDisjointRegionInOnePass) {
    const u32 bytes = kElements * 4;
    const GuestAddr data =
        buffer(bytes, D3D11_USAGE_DYNAMIC, D3D11_BIND_SHADER_RESOURCE,
               D3D11_CPU_ACCESS_WRITE, 0, nullptr);
    const GuestAddr first = srv(data, kR32Float, kSection, kSection);
    const GuestAddr second = srv(data, kR32Float, 2 * kSection, kSection);
    bind_reader();
    const auto a = ramp(100000.0f);
    map_write(data, D3D11_MAP_WRITE_DISCARD, 0, a.data(), bytes);
    read_view(first);
    columns(0, 8);
    read_draw();
    const auto b = ramp(200000.0f);
    map_write(data, D3D11_MAP_WRITE_NO_OVERWRITE, 2 * kSection * 4,
              b.data() + 2 * kSection, kSection * 4);
    read_view(second);
    columns(8, 8);
    read_draw();

    const auto texels = read_target();
    expect_columns(texels, 0, 100000.0f, kSection, "A, sub-array 1");
    expect_columns(texels, 8, 200000.0f, 2 * kSection, "B, sub-array 2");
}

// A compute shader writes the buffer through a whole-buffer RWBuffer<float>
// (CSFill: element e = 5000 + e) between two draws reading it through the
// unaligned view: the first sees the initial contents, the second the
// GPU-written ones. Then CopySubresourceRegion brings in a third array's
// view bytes before a third draw.
TEST_P(D3D11TypedBufferReaderTest, GpuWritesBeforeAnUnalignedRead) {
    const GuestAddr cs = shader(18, tbo_cs_fill, sizeof(tbo_cs_fill));
    ASSERT_NE(cs, 0u);
    const u32 bytes = kElements * 4;
    const auto a = ramp(100000.0f);
    const GuestAddr data = buffer(
        bytes, D3D11_USAGE_DEFAULT,
        D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS, 0, 0,
        a.data());
    const GuestAddr view = srv(data, kR32Float, kSection, kSection);
    const GuestAddr whole = uav(data, kR32Float, 0, kElements);
    bind_reader();
    read_view(view);
    columns(0, 8);
    read_draw();

    context_call(69, {cs, 0, 0});  // CSSetShader
    const std::array<u64, 2> uavs{0, whole};
    context_call(68, {0, 2, put(uavs), 0});  // CSSetUnorderedAccessViews
    context_call(41, {(kElements + 63) / 64, 1, 1});  // Dispatch
    const std::array<u64, 2> none{0, 0};
    context_call(68, {0, 2, put(none), 0});
    read_view(view);
    columns(8, 8);
    read_draw();
    auto texels = read_target();
    expect_columns(texels, 0, 100000.0f, kSection, "before the dispatch");
    expect_columns(texels, 8, 5000.0f, kSection, "after the dispatch");

    // CopySubresourceRegion of the view's 270 elements from another buffer.
    const auto c = ramp(300000.0f);
    const GuestAddr source = buffer(bytes, D3D11_USAGE_DEFAULT,
                                    D3D11_BIND_SHADER_RESOURCE, 0, 0, c.data());
    const std::array<u32, 6> box{kSection * 4, 0, 0, 2 * kSection * 4, 1, 1};
    context_call(46, {data, 0, kSection * 4, 0, 0, source, 0, put(box)});
    columns(0, 8);
    read_draw();
    texels = read_target();
    expect_columns(texels, 0, 300000.0f, kSection, "after the copy");
    expect_columns(texels, 8, 5000.0f, kSection, "after the dispatch, kept");
}

}  // namespace
}  // namespace vitreo::winapi::d3d11_test
