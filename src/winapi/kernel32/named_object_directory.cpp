// SPDX-License-Identifier: LGPL-2.1-only

/// @file named_object_directory.cpp
/// @brief Process-wide named kernel-object kind registry.

#include "named_object_directory.hpp"

#include <mutex>
#include <unordered_map>

namespace vitreo::winapi {
namespace {

std::mutex g_mu;
std::unordered_map<std::string, NamedObjectKind> g_kinds;

}  // namespace

std::string normalize_named_object_name(std::string name) {
    constexpr std::string_view kLocal = "Local\\";
    if (name.size() >= kLocal.size() &&
        name.compare(0, kLocal.size(), kLocal) == 0) {
        name.erase(0, kLocal.size());
    }
    return name;
}

bool named_object_kind_conflict(std::string_view name,
                                NamedObjectKind want) noexcept {
    if (name.empty()) return false;
    std::lock_guard lock(g_mu);
    auto it = g_kinds.find(std::string(name));
    if (it == g_kinds.end()) return false;
    return it->second != want;
}

bool named_object_register(std::string_view name,
                           NamedObjectKind kind) noexcept {
    if (name.empty()) return true;
    std::lock_guard lock(g_mu);
    auto [it, inserted] = g_kinds.emplace(std::string(name), kind);
    if (inserted) return true;
    return it->second == kind;
}

void named_object_unregister(std::string_view name,
                             NamedObjectKind kind) noexcept {
    if (name.empty()) return;
    std::lock_guard lock(g_mu);
    auto it = g_kinds.find(std::string(name));
    if (it != g_kinds.end() && it->second == kind) {
        g_kinds.erase(it);
    }
}

}  // namespace vitreo::winapi
