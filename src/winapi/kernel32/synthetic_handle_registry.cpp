// SPDX-License-Identifier: LGPL-2.1-only

#include "winapi/kernel32/synthetic_handle_registry.hpp"

namespace vitreo::winapi::synthetic_handles {
namespace {

struct Entry {
    u32 flags{0};
};

std::mutex g_mu;
std::unordered_map<Handle, Entry> g_live;

} // namespace

void note_issued(Handle h) {
    if (!is_synthetic_range(h)) return;
    std::lock_guard lock(g_mu);
    g_live.emplace(h, Entry{});
}

void note_user_object(Handle h, u32 flags, bool fresh) {
    if (h == 0) return;
    const Entry entry{flags & (HandleFlags::Inherit |
                               HandleFlags::ProtectFromClose)};
    std::lock_guard lock(g_mu);
    if (fresh) {
        g_live[h] = entry;
    } else {
        g_live.emplace(h, entry);
    }
}

bool is_live(Handle h) {
    std::lock_guard lock(g_mu);
    return g_live.find(h) != g_live.end();
}

bool close(Handle h) {
    std::lock_guard lock(g_mu);
    return g_live.erase(h) != 0;
}

bool set_flags(Handle h, u32 mask, u32 flags) {
    std::lock_guard lock(g_mu);
    auto it = g_live.find(h);
    if (it == g_live.end()) return false;
    const u32 allowed = HandleFlags::Inherit | HandleFlags::ProtectFromClose;
    mask &= allowed;
    it->second.flags = (it->second.flags & ~mask) | (flags & mask);
    return true;
}

bool get_flags(Handle h, u32& out_flags) {
    std::lock_guard lock(g_mu);
    auto it = g_live.find(h);
    if (it == g_live.end()) return false;
    out_flags = it->second.flags &
                (HandleFlags::Inherit | HandleFlags::ProtectFromClose);
    return true;
}

} // namespace vitreo::winapi::synthetic_handles
