#pragma once
/// @file gamepad_host.hpp
/// @brief Platform-neutral host gamepad snapshots for Windows input APIs.

#include <vitreo/common.hpp>

#include <memory>

namespace vitreo::hardware {

struct GamepadState {
    bool connected{false};
    u16 buttons{0};
    u8 left_trigger{0};
    u8 right_trigger{0};
    i16 left_x{0};
    i16 left_y{0};
    i16 right_x{0};
    i16 right_y{0};

    [[nodiscard]] bool operator==(const GamepadState&) const = default;
};

class GamepadHost {
public:
    virtual ~GamepadHost() = default;

    /// Queries one of the four Windows/XInput controller slots.
    /// A successful query with no device sets connected to false.
    [[nodiscard]] virtual bool query_state(u32 index, GamepadState& out) = 0;
};

[[nodiscard]] std::shared_ptr<GamepadHost> make_platform_gamepad_host();

} // namespace vitreo::hardware
