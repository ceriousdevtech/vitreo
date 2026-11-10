#include "windowing/model/cursor_confinement.hpp"
#include <gtest/gtest.h>
#include "windowing/cocoa/cursor_warp_deltas.hpp"
#include <array>

using namespace vitreo;
using namespace vitreo::windowing;

// Windows x86/x64 run3 cursor_clip_contract_probe, not host-derived values.
TEST(CursorConfinement, ZeroExtentRetainsDirectionalClamp) {
    const Rect screen{0, 0, 1920, 1080};
    const Rect clip{960, 540, 960, 540};
    const auto high = confine_cursor_point({1088, 668}, screen, &clip);
    EXPECT_EQ(high.x, 959); EXPECT_EQ(high.y, 539);
    const auto low = confine_cursor_point({832, 412}, screen, &clip);
    EXPECT_EQ(low.x, 960); EXPECT_EQ(low.y, 540);
    const auto equal = confine_cursor_point({960, 540}, screen, &clip);
    EXPECT_EQ(equal.x, 959); EXPECT_EQ(equal.y, 539);
}

TEST(CursorConfinement, OnePixelRectanglePinsBothDirections) {
    const Rect screen{0, 0, 1920, 1080};
    const Rect clip{960, 540, 961, 541};
    for (Point point : {Point{1088, 668}, Point{832, 412}, Point{960, 540}}) {
        const auto actual = confine_cursor_point(point, screen, &clip);
        EXPECT_EQ(actual.x, 960); EXPECT_EQ(actual.y, 540);
    }
}

TEST(CursorConfinement, SingleDegenerateAxisPreservesOtherExtent) {
    const Rect screen{0, 0, 1920, 1080};
    const Rect vertical{960, 540, 960, 548};
    const auto v = confine_cursor_point({1088, 668}, screen, &vertical);
    EXPECT_EQ(v.x, 959); EXPECT_EQ(v.y, 547);
    const Rect horizontal{960, 540, 968, 540};
    const auto h = confine_cursor_point({1088, 668}, screen, &horizontal);
    EXPECT_EQ(h.x, 967); EXPECT_EQ(h.y, 539);
}

TEST(CursorConfinement, ZeroAtVirtualOriginCannotWarpOutsideDesktop) {
    const Rect screen{0, 0, 1920, 1080};
    const Rect clip{0, 0, 0, 0};
    const auto actual = confine_cursor_point({0, 0}, screen, &clip);
    EXPECT_EQ(actual.x, 0); EXPECT_EQ(actual.y, 0);
}

TEST(CursorConfinement, EffectiveRectangleMatchesWindowsIntersection) {
    const Rect screen{0, 0, 1920, 1080};
    for (const Rect input : {Rect{962, 540, 960, 548},
                             Rect{1940, 20, 1960, 40}})
        EXPECT_FALSE(normalize_cursor_clip(input, screen));
    const auto partial = normalize_cursor_clip({-10, -10, 10, 10}, screen);
    ASSERT_TRUE(partial);
    EXPECT_EQ(partial->left, 0); EXPECT_EQ(partial->top, 0);
    EXPECT_EQ(partial->right, 10); EXPECT_EQ(partial->bottom, 10);
    const auto edge = normalize_cursor_clip({-8, 540, 0, 548}, screen);
    ASSERT_TRUE(edge);
    EXPECT_EQ(edge->left, 0); EXPECT_EQ(edge->right, 0);
    const auto point = normalize_cursor_clip({1920, 1080, 1920, 1080}, screen);
    ASSERT_TRUE(point);
    EXPECT_EQ(point->left, 1920); EXPECT_EQ(point->bottom, 1080);
}

TEST(CursorConfinement, NativePointOwnershipChangesOnlyAfterSuccess) {
    CursorClipNativeState state;
    EXPECT_FALSE(state.acquire_point_lock([](bool connect) {
        EXPECT_FALSE(connect); return false;
    }));
    EXPECT_FALSE(state.owns_point_lock());
    ASSERT_TRUE(state.acquire_point_lock([](bool connect) {
        EXPECT_FALSE(connect); return true;
    }));
    EXPECT_TRUE(state.owns_point_lock());
    EXPECT_FALSE(state.release_point_lock([](bool connect) {
        EXPECT_TRUE(connect); return false;
    }));
    EXPECT_TRUE(state.owns_point_lock());
    EXPECT_TRUE(state.release_point_lock([](bool connect) {
        EXPECT_TRUE(connect); return true;
    }));
    EXPECT_FALSE(state.owns_point_lock());
    EXPECT_TRUE(state.release_point_lock([](bool) {
        ADD_FAILURE() << "Unowned native association must not be changed";
        return false;
    }));
}

TEST(CursorConfinement, AreaClipRepositionsOnlyWhenHostCursorIsOutside) {
    // Learn (ClipCursor): adjustment happens only when the position lies
    // outside the rectangle. A point clip repositions every sample.
    EXPECT_FALSE(cursor_clip_should_reposition_host(
        100.4, 80.2, 0.5, 0.5, 1919.5, 1079.5, false));
    EXPECT_FALSE(cursor_clip_should_reposition_host(
        1919.5, 1079.5, 0.5, 0.5, 1919.5, 1079.5, false));
    EXPECT_TRUE(cursor_clip_should_reposition_host(
        1920.0, 40.0, 0.5, 0.5, 1919.5, 1079.5, false));
    EXPECT_TRUE(cursor_clip_should_reposition_host(
        -1.0, 40.0, 0.5, 0.5, 1919.5, 1079.5, false));
    EXPECT_TRUE(cursor_clip_should_reposition_host(
        960.5, 540.5, 960.5, 540.5, 960.5, 540.5, true));
}

TEST(CursorConfinement, WarpCacheDoesNotReclampItsOwnZeroExtentResult) {
    CursorClipNativeState state;
    EXPECT_FALSE(state.is_last_warp(959.5, 540.5));
    state.note_warp(959.5, 540.5);
    EXPECT_TRUE(state.is_last_warp(959.5, 540.5));
    EXPECT_FALSE(state.is_last_warp(960.5, 540.5));
    state.invalidate_position();
    EXPECT_FALSE(state.is_last_warp(959.5, 540.5));
    EXPECT_TRUE(cursor_clip_is_point({960, 540, 960, 540}));
    EXPECT_TRUE(cursor_clip_is_point({960, 540, 961, 541}));
    EXPECT_FALSE(cursor_clip_is_point({960, 540, 960, 548}));
}

TEST(CursorConfinement, RelativeMotionAndIdleMatchWindowsEndpointFixtures) {
    // cursor_clip_motion_{x86,x64}.expected: synthetic relative input with
    // verified active point clip; no buttons/keys. Both bitnesses identical.
    const Rect screen{0, 0, 1920, 1080}, clip{960, 540, 960, 540};
    const std::array<Point, 8> deltas{{{-12,0},{12,0},{0,-12},{0,12},
                                     {-12,-12},{12,12},{0,0},{-12,0}}};
    const std::array<std::array<Point, 8>, 2> expected{{
        {{{960,540},{959,539},{960,540},{959,539},{960,540},{959,539},{959,539},{960,540}}},
        {{{960,539},{959,540},{960,540},{959,539},{960,540},{959,539},{959,539},{960,540}}}
    }};
    for (usize seed = 0; seed < expected.size(); ++seed) {
        Point position = seed == 0 ? Point{959,539} : Point{960,540};
        for (usize i = 0; i < deltas.size(); ++i) {
            position = move_point_cursor_clip(position, deltas[i].x, deltas[i].y, screen, clip);
            EXPECT_EQ(position.x, expected[seed][i].x);
            EXPECT_EQ(position.y, expected[seed][i].y);
            const Point idle = move_point_cursor_clip(position, 0, 0, screen, clip);
            EXPECT_EQ(idle.x, position.x); EXPECT_EQ(idle.y, position.y);
        }
    }
}

TEST(CursorConfinement, AssociationBarrierKeepsQueuedPretransitionCorrection) {
    CursorWarpDeltas warps;
    warps.note_warp(10, 100, 50);
    warps.note_disassociation(11);
    double dx = 103, dy = 48;
    warps.remove_warps(10.5, dx, dy);
    EXPECT_DOUBLE_EQ(dx, 3); EXPECT_DOUBLE_EQ(dy, -2);
}

TEST(CursorConfinement, AssociationBarrierDoesNotSubtractGenuineDeviceMotion) {
    CursorWarpDeltas warps;
    warps.note_warp(10, 100, 50);
    warps.note_disassociation(11);
    double dx = 3, dy = -2;
    warps.remove_warps(12, dx, dy);
    EXPECT_DOUBLE_EQ(dx, 3); EXPECT_DOUBLE_EQ(dy, -2);
    // Reassociation allows new cursor warp corrections, not old ones.
    warps.note_warp(13, 20, 30);
    dx = 24; dy = 29;
    warps.remove_warps(14, dx, dy);
    EXPECT_DOUBLE_EQ(dx, 4); EXPECT_DOUBLE_EQ(dy, -1);
}
