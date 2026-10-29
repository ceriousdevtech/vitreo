// SPDX-License-Identifier: LGPL-2.1-only

#include "process.hpp"

#include "ipc/process_registry.hpp"

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <limits>
#include <optional>
#include <sstream>

namespace vitreo::cli {
namespace {

std::string json_escape(std::string_view value) {
    std::string result;
    result.reserve(value.size() + 8);
    for (const char raw_ch : value) {
        const auto ch = static_cast<unsigned char>(raw_ch);
        switch (ch) {
            case '"': result += "\\\""; break;
            case '\\': result += "\\\\"; break;
            case '\n': result += "\\n"; break;
            case '\r': result += "\\r"; break;
            case '\t': result += "\\t"; break;
            default: result.push_back(static_cast<char>(ch)); break;
        }
    }
    return result;
}

std::optional<u32> parse_pid(std::string_view value) {
    if (value.empty()) return std::nullopt;
    const std::string text(value);
    char* end = nullptr;
    const unsigned long parsed = std::strtoul(text.c_str(), &end, 10);
    if (end == nullptr || *end != '\0' || parsed == 0 ||
        parsed > std::numeric_limits<u32>::max()) {
        return std::nullopt;
    }
    return static_cast<u32>(parsed);
}

} // namespace

CommandResult ProcessCommand::execute(const ParsedArgs& args) {
    const std::string capsule = args.capsule_name.value_or("default");
    const auto root = (capsule_base_ / capsule).string();
    const auto processes = ipc::list_processes(root);

    if (args.subcommand.empty() || args.subcommand == "list") {
        if (args.json) {
            std::ostringstream out;
            out << "[";
            for (size_t i = 0; i < processes.size(); ++i) {
                if (i != 0) out << ",";
                const auto& process = processes[i];
                out << "{\"pid\":" << process.pid
                    << ",\"parent_pid\":" << process.parent_pid
                    << ",\"image_name\":\"" << json_escape(process.image_name)
                    << "\",\"image_path\":\"" << json_escape(process.image_path)
                    << "\",\"capsule\":\"" << json_escape(capsule) << "\"}";
            }
            out << "]";
            return CommandResult::success(out.str());
        }
        if (processes.empty()) {
            return CommandResult::success(
                "No guest processes running in capsule '" + capsule + "'.");
        }
        std::ostringstream out;
        for (const auto& process : processes) {
            out << process.pid << "  " << process.parent_pid << "  "
                << process.image_name << "  " << process.image_path << "\n";
        }
        return CommandResult::success(out.str());
    }

    if (args.subcommand == "stop") {
        if (args.positional_args.size() != 1) {
            return CommandResult::failure(
                "Usage: vitreo process stop <pid> --capsule <name>");
        }
        const auto pid = parse_pid(args.positional_args[0]);
        if (!pid.has_value()) return CommandResult::failure("Invalid process id");
        const auto found = std::find_if(
            processes.begin(), processes.end(),
            [&](const ipc::ProcessSnapshotEntry& process) {
                return process.pid == *pid;
            });
        if (found == processes.end()) {
            return CommandResult::failure(
                "Process is not registered in capsule '" + capsule + "'");
        }
        if (::kill(static_cast<pid_t>(*pid), SIGTERM) != 0) {
            return CommandResult::failure(
                "Failed to stop process " + std::to_string(*pid) +
                ": errno=" + std::to_string(errno));
        }
        return CommandResult::success(
            "Stop requested for process " + std::to_string(*pid));
    }

    if (args.subcommand == "stop-all") {
        size_t stopped = 0;
        size_t failed = 0;
        for (auto it = processes.rbegin(); it != processes.rend(); ++it) {
            if (::kill(static_cast<pid_t>(it->pid), SIGTERM) == 0) {
                ++stopped;
            } else {
                ++failed;
            }
        }
        if (failed != 0) {
            return CommandResult::failure(
                "Stop requested for " + std::to_string(stopped) +
                " process(es); failed to signal " + std::to_string(failed));
        }
        return CommandResult::success(
            "Stop requested for " + std::to_string(stopped) + " process(es)");
    }

    return CommandResult::failure(
        "Unknown process subcommand. Use 'list', 'stop', or 'stop-all'.");
}

} // namespace vitreo::cli
