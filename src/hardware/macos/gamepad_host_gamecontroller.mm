/// @file gamepad_host_gamecontroller.mm
/// @brief macOS GameController-framework implementation of GamepadHost.

#include "hardware/contracts/gamepad_host.hpp"

#include <algorithm>
#include <cmath>

#import <Foundation/Foundation.h>
#import <GameController/GameController.h>

namespace vitreo::hardware {
namespace {

constexpr u16 kDpadUp = 0x0001;
constexpr u16 kDpadDown = 0x0002;
constexpr u16 kDpadLeft = 0x0004;
constexpr u16 kDpadRight = 0x0008;
constexpr u16 kStart = 0x0010;
constexpr u16 kBack = 0x0020;
constexpr u16 kLeftThumb = 0x0040;
constexpr u16 kRightThumb = 0x0080;
constexpr u16 kLeftShoulder = 0x0100;
constexpr u16 kRightShoulder = 0x0200;
constexpr u16 kA = 0x1000;
constexpr u16 kB = 0x2000;
constexpr u16 kX = 0x4000;
constexpr u16 kY = 0x8000;

[[nodiscard]] i16 thumb_axis(float value) {
    const float clamped = std::clamp(value, -1.0f, 1.0f);
    const float scaled = clamped < 0.0f ? clamped * 32768.0f
                                       : clamped * 32767.0f;
    return static_cast<i16>(std::lround(scaled));
}

[[nodiscard]] u8 trigger_axis(float value) {
    return static_cast<u8>(std::lround(
        std::clamp(value, 0.0f, 1.0f) * 255.0f));
}

void add_button(u16& buttons, GCControllerButtonInput* input, u16 mask) {
    if (input != nil && input.isPressed) buttons |= mask;
}

class GameControllerHost final : public GamepadHost {
public:
    GameControllerHost() {
        // Start the framework during platform-host construction, before the
        // first guest window finishes NSApplication launch. Apple's Game
        // Controller guide specifies that the initial connected-device list
        // is populated after application launch. Deferring framework activation
        // to the first XInput query can return its still-empty startup list.
        // This starts normal asynchronous enumeration; it neither waits for a
        // controller nor treats an empty snapshot as a connected device.
        // https://developer.apple.com/library/archive/documentation/ServicesDiscovery/Conceptual/GameControllerPG/DiscoveringControllers/DiscoveringControllers.html
        @autoreleasepool {
            (void)[GCController controllers];
        }
    }

    [[nodiscard]] bool query_state(u32 index, GamepadState& out) override {
        @autoreleasepool {
            out = GamepadState{};
            NSArray<GCController*>* controllers = [GCController controllers];
            if (index >= controllers.count) return true;
            GCExtendedGamepad* pad = controllers[index].extendedGamepad;
            if (pad == nil) return true;

            out.connected = true;
            add_button(out.buttons, pad.dpad.up, kDpadUp);
            add_button(out.buttons, pad.dpad.down, kDpadDown);
            add_button(out.buttons, pad.dpad.left, kDpadLeft);
            add_button(out.buttons, pad.dpad.right, kDpadRight);
            add_button(out.buttons, pad.buttonMenu, kStart);
            add_button(out.buttons, pad.buttonOptions, kBack);
            add_button(out.buttons, pad.leftThumbstickButton, kLeftThumb);
            add_button(out.buttons, pad.rightThumbstickButton, kRightThumb);
            add_button(out.buttons, pad.leftShoulder, kLeftShoulder);
            add_button(out.buttons, pad.rightShoulder, kRightShoulder);
            add_button(out.buttons, pad.buttonA, kA);
            add_button(out.buttons, pad.buttonB, kB);
            add_button(out.buttons, pad.buttonX, kX);
            add_button(out.buttons, pad.buttonY, kY);

            out.left_trigger = trigger_axis(pad.leftTrigger.value);
            out.right_trigger = trigger_axis(pad.rightTrigger.value);
            out.left_x = thumb_axis(pad.leftThumbstick.xAxis.value);
            out.left_y = thumb_axis(pad.leftThumbstick.yAxis.value);
            out.right_x = thumb_axis(pad.rightThumbstick.xAxis.value);
            out.right_y = thumb_axis(pad.rightThumbstick.yAxis.value);
            return true;
        }
    }
};

} // namespace

std::shared_ptr<GamepadHost> make_platform_gamepad_host() {
    return std::make_shared<GameControllerHost>();
}

} // namespace vitreo::hardware
