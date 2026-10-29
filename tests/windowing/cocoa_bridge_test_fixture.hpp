/// @file cocoa_bridge_test_fixture.hpp
/// @brief Shared include set and CocoaBridgeTest fixture for the CocoaBridge tests (WINDOW-003).
///
/// Included by every test_cocoa_bridge*.cpp. CocoaBridgeTest is a global-namespace
/// class defined identically in each including translation unit, because every
/// TEST_F of a suite must name the same fixture type; all its members are defined
/// in-class.

#pragma once

#include <gtest/gtest.h>
#include <algorithm>
#include <chrono>
#include <future>
#include <thread>
#include <cstdlib>
#include <optional>
#include <string>
#include <vitreo/common.hpp>
#include <graphics/common/surface_presenter.hpp>
#include <windowing/cocoa/cocoa_bridge.hpp>
#include <windowing/cocoa/window_presentation.hpp>
#include <windowing/messages/message_queue.hpp>

using namespace vitreo;
using namespace vitreo::windowing;

class CocoaBridgeTest : public ::testing::Test {
protected:
    CocoaBridge bridge;
};
