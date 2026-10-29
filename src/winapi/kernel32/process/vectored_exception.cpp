// SPDX-License-Identifier: LGPL-2.1-only

/// @file vectored_exception.cpp
/// @brief Process-wide vectored exception and continue handler lists.

#include "winapi/kernel32/process/vectored_exception.hpp"

#include <mutex>
#include <vector>

namespace vitreo::winapi {
namespace {

struct VehEntry {
    u64 handle = 0;
    u64 handler = 0;
};

std::mutex g_veh_mu;
std::vector<VehEntry> g_veh_list;
std::vector<VehEntry> g_vch_list;
// One handle space for both lists, so a handle names one registration and
// the other list's Remove refuses it.
u64 g_veh_next_handle = 1;

u64 add_to(std::vector<VehEntry>& list, u32 first, u64 handler) noexcept {
    std::lock_guard<std::mutex> lock(g_veh_mu);
    const u64 handle = g_veh_next_handle++;
    if (g_veh_next_handle == 0) {
        g_veh_next_handle = 1;
    }
    const VehEntry entry{handle, handler};
    if (first != 0) {
        list.insert(list.begin(), entry);
    } else {
        list.push_back(entry);
    }
    return entry.handle;
}

bool remove_from(std::vector<VehEntry>& list, u64 handle) noexcept {
    if (handle == 0) {
        return false;
    }
    std::lock_guard<std::mutex> lock(g_veh_mu);
    for (auto it = list.begin(); it != list.end(); ++it) {
        if (it->handle == handle) {
            list.erase(it);
            return true;
        }
    }
    return false;
}

std::vector<u64> snapshot_of(const std::vector<VehEntry>& list) {
    std::lock_guard<std::mutex> lock(g_veh_mu);
    std::vector<u64> handlers;
    handlers.reserve(list.size());
    for (const auto& entry : list) {
        handlers.push_back(entry.handler);
    }
    return handlers;
}

} // namespace

u64 add_vectored_exception_handler(u32 first, u64 handler) noexcept {
    return add_to(g_veh_list, first, handler);
}

bool remove_vectored_exception_handler(u64 handle) noexcept {
    return remove_from(g_veh_list, handle);
}

std::vector<u64> snapshot_vectored_exception_handlers() {
    return snapshot_of(g_veh_list);
}

u64 add_vectored_continue_handler(u32 first, u64 handler) noexcept {
    return add_to(g_vch_list, first, handler);
}

bool remove_vectored_continue_handler(u64 handle) noexcept {
    return remove_from(g_vch_list, handle);
}

std::vector<u64> snapshot_vectored_continue_handlers() {
    return snapshot_of(g_vch_list);
}

void clear_vectored_exception_handlers_for_test() noexcept {
    std::lock_guard<std::mutex> lock(g_veh_mu);
    g_veh_list.clear();
    g_vch_list.clear();
    g_veh_next_handle = 1;
}

} // namespace vitreo::winapi
