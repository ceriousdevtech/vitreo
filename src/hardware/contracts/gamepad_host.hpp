#pragma once
/// @file gamepad_host.hpp
/// @brief Platform-neutral host gamepad snapshots for Windows input APIs.

#include <vitreo/common.hpp>
#include "hardware/contracts/gamepad_background_input.hpp"

#include <memory>

namespace vitreo::hardware {

struct GamepadState {
    bool connected{false};
    // Opaque host connection identity, never a guest device identifier.
    u64 connection_id{0};
    u16 buttons{0};
    u8 left_trigger{0};
    u8 right_trigger{0};
    i16 left_x{0};
    i16 left_y{0};
    i16 right_x{0};
    i16 right_y{0};

    [[nodiscard]] bool operator==(const GamepadState& other) const {
        // XInput packet numbers track observable control state, not native
        // connection bookkeeping. HID consumes connection_id separately.
        return connected==other.connected && buttons==other.buttons &&
            left_trigger==other.left_trigger && right_trigger==other.right_trigger &&
            left_x==other.left_x && left_y==other.left_y && right_x==other.right_x && right_y==other.right_y;
    }
};

class GamepadHost {
public:
    virtual ~GamepadHost() = default;

    /// Queries one of the four Windows/XInput controller slots.
    /// A successful query with no device sets connected to false.
    [[nodiscard]] virtual bool query_state(u32 index, GamepadState& out) = 0;
    /// Optional native event monitoring required by focus-independent HID I/O.
    /// Unsupported hosts return no lease; callers must not use stale snapshots.
    [[nodiscard]] virtual std::shared_ptr<GamepadBackgroundInputLease> acquire_background_input() {return {};}
};

[[nodiscard]] std::shared_ptr<GamepadHost> make_platform_gamepad_host();

} // namespace vitreo::hardware
