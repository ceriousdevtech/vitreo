// SPDX-License-Identifier: LGPL-2.1-only
#pragma once
#include <memory>

namespace vitreo::hardware {
// A consumer owns native delivery independent of application activation.
// The lease controls event monitoring, not a promise that a new event arrived.
class GamepadBackgroundInputLease {
public:
    virtual ~GamepadBackgroundInputLease()=default;
};
}
