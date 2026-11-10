// SPDX-License-Identifier: LGPL-2.1-only
/// @file test_metal_swapchain_layers.mm
/// @brief Several composition swapchains on one view each keep a CAMetalLayer.
///        A window flip-model swapchain still replaces every leftover layer.

#include <gtest/gtest.h>

#include "graphics/contracts/backend_swap_chain.hpp"
#include "graphics/metal/metal_device.hpp"

#import <AppKit/NSView.h>
#import <QuartzCore/CAMetalLayer.h>

#include <memory>
#include <vector>

using namespace vitreo::graphics;

namespace {

NSView* make_host_view() {
    NSView* view = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 320, 240)];
    [view setWantsLayer:YES];
    [view setLayer:[CALayer layer]];
    return view;
}

std::vector<CALayer*> metal_sublayers(NSView* view) {
    std::vector<CALayer*> layers;
    for (CALayer* sub in view.layer.sublayers) {
        if ([sub isKindOfClass:[CAMetalLayer class]]) layers.push_back(sub);
    }
    return layers;
}

BackendSwapChainDesc swap_desc(NSView* view, bool preserve_siblings) {
    BackendSwapChainDesc desc{};
    desc.native_view = reinterpret_cast<void*>(view);
    desc.width = 64;
    desc.height = 48;
    desc.pixel_format = PixelFormat::BGRA8Unorm;
    desc.buffer_count = 2;
    desc.vsync = false;
    desc.preserve_sibling_metal_layers = preserve_siblings;
    return desc;
}

}  // namespace

TEST(MetalSwapchainLayers, CompositionLayersKeepSiblingMetalLayers) {
    auto& device = MetalDevice::instance();
    if (!device.available()) GTEST_SKIP() << "no Metal device";

    @autoreleasepool {
        NSView* view = make_host_view();
        auto first = device.create_swap_chain(swap_desc(view, true));
        auto second = device.create_swap_chain(swap_desc(view, true));
        ASSERT_NE(first, nullptr);
        ASSERT_TRUE(first->valid());
        ASSERT_NE(second, nullptr);
        ASSERT_TRUE(second->valid());

        const std::vector<CALayer*> both = metal_sublayers(view);
        ASSERT_EQ(both.size(), 2u);
        EXPECT_NE(both[0], both[1]);

        first.reset();
        const std::vector<CALayer*> remaining = metal_sublayers(view);
        ASSERT_EQ(remaining.size(), 1u);
        EXPECT_EQ(remaining[0], both[1]);

        second.reset();
        EXPECT_TRUE(metal_sublayers(view).empty());
        [view release];
    }
}

TEST(MetalSwapchainLayers, OrdinarySwapchainReplacesPreviousOrdinaryLayer) {
    auto& device = MetalDevice::instance();
    if (!device.available()) GTEST_SKIP() << "no Metal device";

    @autoreleasepool {
        NSView* view = make_host_view();
        auto first = device.create_swap_chain(swap_desc(view, false));
        ASSERT_NE(first, nullptr);
        ASSERT_TRUE(first->valid());
        const std::vector<CALayer*> before = metal_sublayers(view);
        ASSERT_EQ(before.size(), 1u);

        auto second = device.create_swap_chain(swap_desc(view, false));
        ASSERT_NE(second, nullptr);
        ASSERT_TRUE(second->valid());
        const std::vector<CALayer*> after = metal_sublayers(view);
        ASSERT_EQ(after.size(), 1u);
        EXPECT_NE(after[0], before[0]);

        // A later window swapchain still clears composition layers too.
        auto composition = device.create_swap_chain(swap_desc(view, true));
        ASSERT_NE(composition, nullptr);
        ASSERT_EQ(metal_sublayers(view).size(), 2u);
        auto window = device.create_swap_chain(swap_desc(view, false));
        ASSERT_NE(window, nullptr);
        ASSERT_TRUE(window->valid());
        EXPECT_EQ(metal_sublayers(view).size(), 1u);

        first.reset();
        second.reset();
        composition.reset();
        window.reset();
        [view release];
    }
}
