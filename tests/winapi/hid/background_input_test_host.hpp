// SPDX-License-Identifier: LGPL-2.1-only
#pragma once
#include "hardware/contracts/gamepad_host.hpp"
#include <atomic>

class TestHidHost : public vitreo::hardware::GamepadHost {
public:
    struct Lease final : vitreo::hardware::GamepadBackgroundInputLease {
        explicit Lease(std::shared_ptr<std::atomic<unsigned>> count) : count(std::move(count)) {++*this->count;}
        ~Lease() override {--*count;}
        std::shared_ptr<std::atomic<unsigned>> count;
    };
    std::shared_ptr<std::atomic<unsigned>> monitors=std::make_shared<std::atomic<unsigned>>(0);
    bool monitoring_supported{true};
    std::shared_ptr<vitreo::hardware::GamepadBackgroundInputLease> acquire_background_input() override {
        return monitoring_supported ? std::make_shared<Lease>(monitors) : nullptr;
    }
};
