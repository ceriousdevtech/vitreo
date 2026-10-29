// SPDX-License-Identifier: LGPL-2.1-only

/// @file test_binding_layout.cpp
/// @brief The shared b#/t#/u#/input-assembler binding namespace.
///
/// D3D gives constant buffers, shader resources, unordered access views and the
/// input-assembler streams four independent register namespaces. Metal gives
/// one buffer index namespace and one texture index namespace per stage. The
/// windows that map the former onto the latter live in
/// graphics/contracts/backend_device.hpp and are consumed by three layers that
/// must agree: the shader translator (which tells SPIRV-Cross the MSL index),
/// the Metal backend (which binds there), and every frontend that fills a
/// BackendDrawCall or BackendDispatchCall.
///
/// A disagreement between those layers does not fail. The shader samples an
/// index nothing was bound at and the draw succeeds with wrong pixels, which is
/// why these are asserted rather than commented.

#include "graphics/contracts/backend_device.hpp"

#include <gtest/gtest.h>

namespace vitreo::graphics {
namespace {

/// Half-open [begin, end) index window.
struct Window {
    const char* name;
    unsigned begin;
    unsigned end;
};

[[nodiscard]] bool overlaps(const Window& a, const Window& b) {
    return a.begin < b.end && b.begin < a.end;
}

TEST(BackendBindingLayout, BufferWindowsDoNotOverlap) {
    // Every class that consumes a per-stage Metal buffer argument index.
    const Window windows[] = {
        {"inline constants", BACKEND_INLINE_CONSTANT_BUFFER_INDEX,
         BACKEND_INLINE_CONSTANT_BUFFER_INDEX + 1},
        {"legacy vertex data", BACKEND_LEGACY_VERTEX_DATA_INDEX,
         BACKEND_LEGACY_VERTEX_DATA_INDEX + 1},
        {"b#", BACKEND_CONSTANT_BUFFER_BASE,
         BACKEND_CONSTANT_BUFFER_BASE + BACKEND_MAX_CONSTANT_BUFFER_SLOTS},
        {"structured t#", BACKEND_SRV_BUFFER_BASE,
         BACKEND_SRV_BUFFER_BASE + BACKEND_MAX_TEXTURE_SLOTS},
        {"buffer u#", BACKEND_UAV_BUFFER_BASE,
         BACKEND_UAV_BUFFER_BASE + BACKEND_MAX_TEXTURE_SLOTS},
        {"vertex streams", BACKEND_VERTEX_BUFFER_BASE,
         BACKEND_VERTEX_BUFFER_BASE + BACKEND_MAX_VERTEX_BUFFERS},
    };

    for (const auto& a : windows) {
        for (const auto& b : windows) {
            if (&a == &b) continue;
            EXPECT_FALSE(overlaps(a, b))
                << a.name << " [" << a.begin << "," << a.end << ") overlaps "
                << b.name << " [" << b.begin << "," << b.end << ")";
        }
    }
}

// The u# window used to start at 14 and the vertex-stream window at 16, so
// u2..u7 aliased streams 0..5. That was survivable only because UAV buffers
// were bound on the compute stage and streams on the vertex stage, and Metal's
// index namespace is per-stage. A vertex-stage UAV is legal D3D11 and would
// have silently overwritten a vertex buffer binding.
TEST(BackendBindingLayout, VertexStreamsSitAboveTheUavWindow) {
    EXPECT_GE(BACKEND_VERTEX_BUFFER_BASE,
              BACKEND_UAV_BUFFER_BASE + BACKEND_MAX_TEXTURE_SLOTS);
}

TEST(BackendBindingLayout, TextureWindowsDoNotOverlap) {
    const Window srv{"t#", BACKEND_SRV_TEXTURE_BASE,
                     BACKEND_SRV_TEXTURE_BASE + BACKEND_MAX_TEXTURE_SLOTS};
    const Window uav{"u#", BACKEND_UAV_TEXTURE_BASE,
                     BACKEND_UAV_TEXTURE_BASE + BACKEND_MAX_TEXTURE_SLOTS};
    EXPECT_FALSE(overlaps(srv, uav));
}

// Exceeding Metal's per-stage argument limit is not a run-time failure: the
// encoder drops the excess binding and the draw proceeds. The static_asserts in
// the contract header are the real gate; these restate them as a test so the
// reason a capacity increase was rejected is discoverable from a test name.
TEST(BackendBindingLayout, WindowsFitMetalArgumentLimits) {
    EXPECT_LE(BACKEND_BUFFER_INDEX_LIMIT, BACKEND_METAL_BUFFER_ARGUMENT_LIMIT);
    EXPECT_LE(BACKEND_TEXTURE_INDEX_LIMIT,
              BACKEND_METAL_TEXTURE_ARGUMENT_LIMIT);
}

// The compute stage has no input-assembler streams, so its buffer array stops
// where the vertex-stream window begins. Both call structs are indexed by the
// same absolute layout, so a frontend writing at BACKEND_UAV_BUFFER_BASE + slot
// must land inside the array it was handed.
TEST(BackendBindingLayout, CallStructsCoverTheWindowsFrontendsWriteTo) {
    BackendDispatchCall dispatch{};
    EXPECT_GE(dispatch.buffers.size(),
              BACKEND_UAV_BUFFER_BASE + BACKEND_MAX_TEXTURE_SLOTS);
    EXPECT_GE(dispatch.textures.size(),
              BACKEND_UAV_TEXTURE_BASE + BACKEND_MAX_TEXTURE_SLOTS);

    BackendDrawCall draw{};
    EXPECT_GE(draw.vertex_buffers.size(), BACKEND_MAX_VERTEX_BUFFERS);
    EXPECT_GE(draw.vertex_constant_buffers.size(),
              BACKEND_MAX_CONSTANT_BUFFER_SLOTS);
    EXPECT_GE(draw.fragment_srv_buffers.size(), BACKEND_MAX_TEXTURE_SLOTS);
}

// BackendDrawCall is constructed per draw on the hot path. Raising a per-slot
// capacity multiplies several of its arrays at once, and a 4x growth here is a
// cost no behavioural test can see. This is a ratchet, not a law: raise the
// bound deliberately, in the commit that needs it, having measured the draw
// path. Do not raise it to make a build pass.
TEST(BackendBindingLayout, CallStructsStayWithinTheirSizeRatchet) {
    EXPECT_LE(sizeof(BackendDrawCall), usize{3072})
        << "BackendDrawCall grew to " << sizeof(BackendDrawCall)
        << " bytes; it is built per draw call";
    EXPECT_LE(sizeof(BackendDispatchCall), usize{2048})
        << "BackendDispatchCall grew to " << sizeof(BackendDispatchCall)
        << " bytes";
}

} // namespace
} // namespace vitreo::graphics

// The stream-out block owns the one buffer index that was left, and nothing else
// may take it.
//
// The capture draw of the geometry emulation is a real draw with the guest's
// vertex buffers bound, so its output buffer must not share an index with any
// of them. The failure mode is invisible: two bindings at one index lose one,
// and a draw that fetched its own output looks like a shader that wrote zeros.
TEST(BackendBindingLayout, VertexCaptureBufferOwnsTheLastFreeBufferIndex) {
    using namespace vitreo::graphics;

    // It sits above every other buffer window, so it cannot alias one.
    EXPECT_GE(BACKEND_VERTEX_CAPTURE_BUFFER_INDEX,
              BACKEND_VERTEX_BUFFER_BASE + BACKEND_MAX_VERTEX_BUFFERS);
    EXPECT_GE(BACKEND_VERTEX_CAPTURE_BUFFER_INDEX,
              BACKEND_UAV_BUFFER_BASE + BACKEND_MAX_TEXTURE_SLOTS);
    EXPECT_GE(BACKEND_VERTEX_CAPTURE_BUFFER_INDEX,
              BACKEND_SRV_BUFFER_BASE + BACKEND_MAX_TEXTURE_SLOTS);
    EXPECT_GE(BACKEND_VERTEX_CAPTURE_BUFFER_INDEX,
              BACKEND_CONSTANT_BUFFER_BASE + BACKEND_MAX_CONSTANT_BUFFER_SLOTS);
    EXPECT_NE(BACKEND_VERTEX_CAPTURE_BUFFER_INDEX,
              BACKEND_INLINE_CONSTANT_BUFFER_INDEX);
    EXPECT_NE(BACKEND_VERTEX_CAPTURE_BUFFER_INDEX,
              BACKEND_LEGACY_VERTEX_DATA_INDEX);

    // And it fits, with nothing to spare. That is why stream-out targets are
    // written by a compute repack, not by the vertex stage: four targets and a
    // filled-size counter would need five more vertex-stage indices.
    EXPECT_LT(BACKEND_VERTEX_CAPTURE_BUFFER_INDEX,
              BACKEND_METAL_BUFFER_ARGUMENT_LIMIT);
    EXPECT_EQ(BACKEND_BUFFER_INDEX_LIMIT,
              BACKEND_VERTEX_CAPTURE_BUFFER_INDEX + 1);
    EXPECT_LE(BACKEND_BUFFER_INDEX_LIMIT, BACKEND_METAL_BUFFER_ARGUMENT_LIMIT);
    EXPECT_GT(BACKEND_VERTEX_CAPTURE_BUFFER_INDEX +
                  BACKEND_MAX_STREAM_OUT_TARGETS + 1,
              BACKEND_METAL_BUFFER_ARGUMENT_LIMIT)
        << "if this ever fits, the vertex stage could write SO targets directly";
    EXPECT_EQ(BACKEND_MAX_STREAM_OUT_TARGETS, 4u)
        << "D3D11_SO_BUFFER_SLOT_COUNT";
}
