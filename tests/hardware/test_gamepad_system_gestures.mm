// SPDX-License-Identifier: LGPL-2.1-only
#import <Foundation/Foundation.h>
#import <GameController/GameController.h>
#include "hardware/contracts/gamepad_host.hpp"
#include <gtest/gtest.h>
#include <thread>

namespace {
void drain_main_queue() {
    [[NSRunLoop mainRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.02]];
}

// GCController.h:160-169: monitoring is a process-global class property.
// Multiple provider instances must preserve one another's active consumers.
TEST(GamepadHost, BackgroundMonitoringIsConsumerOwnedAcrossProviders) {
    @autoreleasepool {
        const BOOL original=GCController.shouldMonitorBackgroundEvents;
        auto first=vitreo::hardware::make_platform_gamepad_host();
        auto second=vitreo::hardware::make_platform_gamepad_host();
        for(BOOL initial : {NO,YES}) {
            GCController.shouldMonitorBackgroundEvents=initial;
            auto a=first->acquire_background_input();ASSERT_TRUE(a);
            EXPECT_EQ(GCController.shouldMonitorBackgroundEvents,YES);
            auto b=second->acquire_background_input();ASSERT_TRUE(b);
            a.reset();EXPECT_EQ(GCController.shouldMonitorBackgroundEvents,YES);
            b.reset();EXPECT_EQ(GCController.shouldMonitorBackgroundEvents,initial);
        }
        GCController.shouldMonitorBackgroundEvents=NO;
        std::shared_ptr<vitreo::hardware::GamepadBackgroundInputLease> worker_lease;
        std::thread worker([&]{worker_lease=first->acquire_background_input();});worker.join();
        ASSERT_TRUE(worker_lease);EXPECT_EQ(GCController.shouldMonitorBackgroundEvents,YES);
        first.reset();second.reset(); // the consumer pins the property owner
        EXPECT_EQ(GCController.shouldMonitorBackgroundEvents,YES);
        worker_lease.reset();EXPECT_EQ(GCController.shouldMonitorBackgroundEvents,NO);
        GCController.shouldMonitorBackgroundEvents=original;
        drain_main_queue();
    }
}
}

// GCControllerElement.h: Enabled can consume a button for a system gesture;
// Disabled passes it directly to this application. No global shortcut changes.
TEST(GamepadHost, ConnectedMappedButtonsBelongToGuestUntilHostDestruction) {
    @autoreleasepool {
        ASSERT_TRUE([NSThread isMainThread]);
        GCController* controller = [GCController controllerWithExtendedGamepad];
        GCExtendedGamepad* pad = controller.extendedGamepad;
        ASSERT_NE(pad, nil);
        ASSERT_NE(pad.buttonMenu, nil);
        ASSERT_NE(pad.buttonOptions, nil);
        pad.buttonMenu.preferredSystemGestureState = GCSystemGestureStateEnabled;
        pad.buttonOptions.preferredSystemGestureState = GCSystemGestureStateEnabled;
        pad.buttonA.preferredSystemGestureState = GCSystemGestureStateEnabled;
        const auto home_preference = pad.buttonHome.preferredSystemGestureState;
        auto host = vitreo::hardware::make_platform_gamepad_host();
        [[NSNotificationCenter defaultCenter]
            postNotificationName:GCControllerDidConnectNotification object:controller];
        drain_main_queue();
        EXPECT_EQ(pad.buttonMenu.preferredSystemGestureState, GCSystemGestureStateDisabled);
        EXPECT_EQ(pad.buttonOptions.preferredSystemGestureState, GCSystemGestureStateDisabled);
        EXPECT_EQ(pad.buttonA.preferredSystemGestureState, GCSystemGestureStateDisabled);
        if (pad.buttonHome)
            EXPECT_EQ(pad.buttonHome.preferredSystemGestureState, home_preference);

        host.reset();
        pad.buttonMenu.preferredSystemGestureState = GCSystemGestureStateEnabled;
        [[NSNotificationCenter defaultCenter]
            postNotificationName:GCControllerDidConnectNotification object:controller];
        drain_main_queue();
        EXPECT_EQ(pad.buttonMenu.preferredSystemGestureState, GCSystemGestureStateEnabled);
    }
}
