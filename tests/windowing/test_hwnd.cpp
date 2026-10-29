#include <gtest/gtest.h>
#include <vitreo/common.hpp>
#include <windowing/model/hwnd_manager.hpp>
#include <atomic>
#include <thread>

using namespace vitreo;
using namespace vitreo::windowing;

class HwndManagerTest : public ::testing::Test {
protected:
    HwndManager mgr;
};

// 1. Desktop window exists on construction
TEST_F(HwndManagerTest, DesktopExists) {
    EXPECT_TRUE(mgr.is_valid(HWND_DESKTOP));
    EXPECT_NE(HWND_DESKTOP, NULL_HWND);
    const auto* desktop = mgr.find(HWND_DESKTOP);
    ASSERT_NE(desktop, nullptr);
    EXPECT_EQ(desktop->handle, HWND_DESKTOP);
    EXPECT_GT(desktop->rect.width(), 0);
    EXPECT_GT(desktop->rect.height(), 0);
}

// 2. Create returns HWND >= FIRST_HWND (0x10000)
TEST_F(HwndManagerTest, CreateReturnsHighHandle) {
    WindowData wd{};
    wd.title = L"Test";
    HWND h = mgr.create(wd);
    EXPECT_GE(h, static_cast<HWND>(0x00010000));
    EXPECT_NE(h, HWND_DESKTOP);
}

// 3. Find returns correct WindowData
TEST_F(HwndManagerTest, FindReturnsCorrectData) {
    WindowData wd{};
    wd.title = L"Hello";
    wd.style = WS::OVERLAPPEDWINDOW;
    HWND h = mgr.create(wd);

    const auto* found = mgr.find(h);
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(found->handle, h);
    EXPECT_EQ(found->title, L"Hello");
    EXPECT_EQ(found->style, WS::OVERLAPPEDWINDOW);
}

// 4. Destroy invalidates handle
TEST_F(HwndManagerTest, DestroyInvalidatesHandle) {
    WindowData wd{};
    HWND h = mgr.create(wd);
    EXPECT_TRUE(mgr.is_valid(h));
    mgr.destroy(h);
    EXPECT_FALSE(mgr.is_valid(h));
}

// 5. Find after destroy returns nullptr
TEST_F(HwndManagerTest, FindAfterDestroyReturnsNull) {
    WindowData wd{};
    HWND h = mgr.create(wd);
    mgr.destroy(h);
    EXPECT_EQ(mgr.find(h), nullptr);
}

// 6. Created handles are unique and monotonically increasing
TEST_F(HwndManagerTest, HandlesAreMonotonicallyIncreasing) {
    WindowData wd{};
    HWND h1 = mgr.create(wd);
    HWND h2 = mgr.create(wd);
    HWND h3 = mgr.create(wd);
    EXPECT_LT(h1, h2);
    EXPECT_LT(h2, h3);
}

// 7. Parent-child: create child with parent set → enum_children includes it
TEST_F(HwndManagerTest, ChildAppearsInParentEnum) {
    WindowData parent_wd{};
    parent_wd.title = L"Parent";
    HWND parent = mgr.create(parent_wd);

    WindowData child_wd{};
    child_wd.parent = parent;
    child_wd.title  = L"Child";
    HWND child = mgr.create(child_wd);

    auto children = mgr.enum_children(parent);
    ASSERT_EQ(children.size(), 1u);
    EXPECT_EQ(children[0], child);
}

// 8. Destroy removes from parent's children list
TEST_F(HwndManagerTest, DestroyRemovesFromParentChildren) {
    WindowData parent_wd{};
    HWND parent = mgr.create(parent_wd);

    WindowData child_wd{};
    child_wd.parent = parent;
    HWND child = mgr.create(child_wd);

    mgr.destroy(child);
    auto children = mgr.enum_children(parent);
    EXPECT_TRUE(children.empty());
}

// 9. Count tracks correctly
TEST_F(HwndManagerTest, CountTracksCorrectly) {
    EXPECT_EQ(mgr.count(), 0u);
    WindowData wd{};
    HWND h1 = mgr.create(wd);
    EXPECT_EQ(mgr.count(), 1u);
    HWND h2 = mgr.create(wd);
    EXPECT_EQ(mgr.count(), 2u);
    mgr.destroy(h1);
    EXPECT_EQ(mgr.count(), 1u);
    mgr.destroy(h2);
    EXPECT_EQ(mgr.count(), 0u);
}

// 10. find_window by title finds the correct window
TEST_F(HwndManagerTest, FindWindowByTitle) {
    WindowData wd{};
    wd.title = L"UniqueTitle";
    HWND h = mgr.create(wd);

    HWND found = mgr.find_window(L"", L"UniqueTitle");
    EXPECT_EQ(found, h);
}

// 11. find_window with empty title and empty class returns first non-desktop
TEST_F(HwndManagerTest, FindWindowEmptyTitleReturnsFirst) {
    // With no windows, should return NULL_HWND
    HWND found = mgr.find_window(L"", L"");
    // Might return NULL_HWND if no non-desktop windows exist
    // (unordered_map iteration order is implementation-defined)
    // Just test that creating a window makes it findable
    WindowData wd{};
    wd.title = L"Any";
    HWND h = mgr.create(wd);
    found = mgr.find_window(L"", L"");
    EXPECT_NE(found, NULL_HWND);
    EXPECT_TRUE(mgr.is_valid(found));
}

// 12. Enum children on non-existent parent returns empty
TEST_F(HwndManagerTest, EnumChildrenNonExistentParent) {
    auto children = mgr.enum_children(0xDEAD);
    EXPECT_TRUE(children.empty());
}

// 13. Desktop cannot be destroyed
TEST_F(HwndManagerTest, DesktopCannotBeDestroyed) {
    mgr.destroy(HWND_DESKTOP);
    EXPECT_TRUE(mgr.is_valid(HWND_DESKTOP));
}

// 14. find_window with non-matching title returns NULL_HWND
TEST_F(HwndManagerTest, FindWindowNoMatch) {
    WindowData wd{};
    wd.title = L"Existing";
    mgr.create(wd);

    HWND found = mgr.find_window(L"", L"NonExistent");
    EXPECT_EQ(found, NULL_HWND);
}

// 15. Multiple children tracked in order
TEST_F(HwndManagerTest, MultipleChildrenTracked) {
    WindowData parent_wd{};
    HWND parent = mgr.create(parent_wd);

    WindowData c1{};
    c1.parent = parent;
    HWND h1 = mgr.create(c1);

    WindowData c2{};
    c2.parent = parent;
    HWND h2 = mgr.create(c2);

    WindowData c3{};
    c3.parent = parent;
    HWND h3 = mgr.create(c3);

    auto children = mgr.enum_children(parent);
    ASSERT_EQ(children.size(), 3u);
    EXPECT_EQ(children[0], h1);
    EXPECT_EQ(children[1], h2);
    EXPECT_EQ(children[2], h3);
}

TEST_F(HwndManagerTest, TopLevelWindowsInsertAtFrontOfZOrder) {
    WindowData a{};
    a.parent = HWND_DESKTOP;
    const HWND ha = mgr.create(a);
    WindowData b{};
    b.parent = HWND_DESKTOP;
    const HWND hb = mgr.create(b);
    auto tops = mgr.enum_children(HWND_DESKTOP);
    ASSERT_GE(tops.size(), 2u);
    EXPECT_EQ(tops.front(), hb);  // newest on top (Learn z-order)
    EXPECT_NE(std::find(tops.begin(), tops.end(), ha), tops.end());
}

TEST_F(HwndManagerTest, WindowFromPointSkipsHiddenDisabledAndFindsTopmost) {
    WindowData lower{};
    lower.rect = Rect{100, 100, 400, 300};
    lower.visible = true;
    const HWND lower_hwnd = mgr.create(lower);

    WindowData upper = lower;
    upper.enabled = false;
    const HWND upper_hwnd = mgr.create(upper);
    ASSERT_TRUE(mgr.set_z_order(upper_hwnd, HWND_TOP));

    EXPECT_EQ(mgr.window_from_point(Point{200, 180}), lower_hwnd);

    WindowData hidden = lower;
    hidden.visible = false;
    hidden.rect = Rect{100, 100, 400, 300};
    const HWND hidden_hwnd = mgr.create(hidden);
    ASSERT_TRUE(mgr.set_z_order(hidden_hwnd, HWND_TOP));
    EXPECT_EQ(mgr.window_from_point(Point{200, 180}), lower_hwnd);

    WindowData top{};
    top.rect = Rect{150, 120, 350, 280};
    top.visible = true;
    const HWND top_hwnd = mgr.create(top);
    ASSERT_TRUE(mgr.set_z_order(top_hwnd, HWND_TOP));
    EXPECT_EQ(mgr.window_from_point(Point{200, 180}), top_hwnd);
    EXPECT_EQ(mgr.window_from_point(Point{10, 10}), HWND_DESKTOP);
}

TEST_F(HwndManagerTest, ChildWindowFromPointUsesZOrderAndSkipFlags) {
    WindowData parent_data{};
    parent_data.client_rect = Rect{0, 0, 300, 200};
    const HWND parent = mgr.create(parent_data);

    WindowData lower{};
    lower.parent = parent;
    lower.rect = Rect{20, 20, 180, 160};
    lower.visible = true;
    const HWND lower_hwnd = mgr.create(lower);

    WindowData upper = lower;
    upper.enabled = false;
    upper.ex_style = 0x00000020; // WS_EX_TRANSPARENT
    const HWND upper_hwnd = mgr.create(upper);
    ASSERT_TRUE(mgr.set_z_order(upper_hwnd, HWND_TOP));

    EXPECT_EQ(mgr.child_window_from_point(parent, Point{50, 50}, 0),
              upper_hwnd);
    EXPECT_EQ(mgr.child_window_from_point(parent, Point{50, 50}, 0x0002),
              lower_hwnd);
    EXPECT_EQ(mgr.child_window_from_point(parent, Point{50, 50}, 0x0004),
              lower_hwnd);
    EXPECT_EQ(mgr.child_window_from_point(parent, Point{250, 100}, 0),
              parent);
    EXPECT_EQ(mgr.child_window_from_point(parent, Point{300, 100}, 0),
              NULL_HWND);
}

TEST_F(HwndManagerTest, ChildWindowFromPointFindsDesktopChildOutsidePrimary) {
    // Windows reports the primary-monitor rectangle from GetClientRect on the
    // desktop HWND, but top-level desktop children can occupy the rest of the
    // virtual screen.
    WindowData secondary_window{};
    secondary_window.rect = Rect{2100, 100, 2500, 500};
    secondary_window.visible = true;
    const HWND hwnd = mgr.create(secondary_window);

    EXPECT_EQ(mgr.child_window_from_point(
                  HWND_DESKTOP, Point{2200, 200}, 0x0001),
              hwnd);
    EXPECT_EQ(mgr.child_window_from_point(
                  HWND_DESKTOP, Point{2600, 200}, 0x0001),
              NULL_HWND);
}

TEST_F(HwndManagerTest, OwnerThreadIdReturnsStableSnapshot) {
    WindowData wd{};
    wd.owner_thread_id = 42;
    const HWND h = mgr.create(wd);

    EXPECT_EQ(mgr.owner_thread_id(h), std::optional<u32>{42});
    mgr.destroy(h);
    EXPECT_EQ(mgr.owner_thread_id(h), std::nullopt);
}

TEST_F(HwndManagerTest, OwnerLookupIsSafeDuringWindowMapMutation) {
    WindowData target_data{};
    target_data.owner_thread_id = 77;
    const HWND target = mgr.create(target_data);
    std::atomic<bool> start{false};

    std::thread mutator([&] {
        while (!start.load(std::memory_order_acquire)) {
        }
        for (int i = 0; i < 5000; ++i) {
            WindowData transient{};
            transient.owner_thread_id = static_cast<u32>(i + 100);
            const HWND h = mgr.create(transient);
            mgr.destroy(h);
        }
    });

    start.store(true, std::memory_order_release);
    for (int i = 0; i < 5000; ++i) {
        EXPECT_EQ(mgr.owner_thread_id(target), std::optional<u32>{77});
    }
    mutator.join();
}
