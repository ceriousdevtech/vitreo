// SPDX-License-Identifier: LGPL-2.1-only
/// @file process.hpp
/// @brief Capsule-scoped Vitreo process discovery and control.

#pragma once

#include "command.hpp"

#include <filesystem>
#include <utility>

namespace vitreo::cli {

class ProcessCommand final : public Command {
public:
    explicit ProcessCommand(std::filesystem::path capsule_base)
        : capsule_base_(std::move(capsule_base)) {}

    CommandResult execute(const ParsedArgs& args) override;
    std::string_view name() const override { return "process"; }
    std::string_view help() const override {
        return "List or stop registered guest processes";
    }

private:
    std::filesystem::path capsule_base_;
};

} // namespace vitreo::cli
