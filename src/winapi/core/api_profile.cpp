/// @file api_profile.cpp
/// @brief See api_profile.hpp.

#include <winapi/core/api_profile.hpp>
#include <winapi/core/thunk_registry.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace vitreo::winapi::api_profile {
namespace {

struct Slot { u64 count{0}; u64 ns{0}; };
using Table = std::unordered_map<GuestAddr, Slot>;

// Per-thread tables keep the hot path lock-free; a global list of them lets
// the dump merge everything. Tables are deliberately leaked so a thread that
// exits still contributes its samples to the final dump.
std::mutex g_mu;
std::vector<Table*> g_tables;
std::atomic<const ThunkRegistry*> g_names{nullptr};
std::atomic<u64> g_total{0};
std::atomic<u64> g_next{0};

Table& local_table() {
    static thread_local Table* t = [] {
        auto* fresh = new Table();
        fresh->reserve(512);
        std::lock_guard<std::mutex> lk(g_mu);
        g_tables.push_back(fresh);
        return fresh;
    }();
    return *t;
}

std::chrono::steady_clock::time_point g_start{};
std::atomic<u64> g_next_secs{0};

double secs_interval() {
    static const double v = [] {
        const char* e = std::getenv("VITREO_API_PROFILE_SECS");
        const double d = e != nullptr ? std::atof(e) : 0.0;
        return d > 0.0 ? d : 5.0;
    }();
    return v;
}

u64 dump_every() {
    static const u64 v = [] {
        const char* e = std::getenv("VITREO_API_PROFILE_EVERY");
        const u64 n = e != nullptr ? std::strtoull(e, nullptr, 10) : 0;
        return n != 0 ? n : 2000000ull;
    }();
    return v;
}

} // namespace

bool enabled() noexcept {
    static const bool on = std::getenv("VITREO_API_PROFILE") != nullptr;
    return on;
}

void set_name_source(const ThunkRegistry* reg) noexcept {
    g_names.store(reg, std::memory_order_release);
}

void record(GuestAddr key, u64 ns) noexcept {
    auto& slot = local_table()[key];
    ++slot.count;
    slot.ns += ns;

    const u64 n = g_total.fetch_add(1, std::memory_order_relaxed) + 1;

    // Sample the clock rarely: this runs on every API call.
    if ((n & 0xFFF) != 0) return;
    const auto now = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> lk(g_mu);
        if (g_start.time_since_epoch().count() == 0) g_start = now;
    }
    const double elapsed =
        std::chrono::duration<double>(now - g_start).count();
    const u64 bucket = static_cast<u64>(elapsed / secs_interval());
    u64 seen = g_next_secs.load(std::memory_order_relaxed);
    if (bucket <= seen) return;
    if (g_next_secs.compare_exchange_strong(seen, bucket)) {
        char tag[64];
        std::snprintf(tag, sizeof(tag), "t=%.0fs", elapsed);
        dump(tag);
    }
}

void dump(const char* tag) {
    Table merged;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        for (const Table* t : g_tables) {
            for (const auto& [k, s] : *t) {
                auto& d = merged[k];
                d.count += s.count;
                d.ns += s.ns;
            }
        }
    }
    if (merged.empty()) return;

    std::vector<std::pair<GuestAddr, Slot>> rows(merged.begin(), merged.end());
    u64 all_ns = 0, all_n = 0;
    for (const auto& r : rows) { all_ns += r.second.ns; all_n += r.second.count; }

    const ThunkRegistry* reg = g_names.load(std::memory_order_acquire);
    std::fprintf(stderr,
                 "[api-profile] %s: calls=%llu host_time=%.2fs exports=%zu\n",
                 tag, (unsigned long long)all_n,
                 static_cast<double>(all_ns) / 1e9, rows.size());

    const auto show = [&](const char* what, usize limit) {
        std::fprintf(stderr, "[api-profile] top by %s:\n", what);
        for (usize i = 0; i < rows.size() && i < limit; ++i) {
            std::string name =
                reg != nullptr ? reg->debug_name_for(rows[i].first) : std::string();
            if (name.empty()) {
                char buf[32];
                std::snprintf(buf, sizeof(buf), "<0x%08x>",
                              static_cast<unsigned>(rows[i].first));
                name = buf;
            }
            std::fprintf(stderr,
                "[api-profile]   %-40s n=%-9llu total=%8.3fs avg=%8.3fus\n",
                name.c_str(), (unsigned long long)rows[i].second.count,
                static_cast<double>(rows[i].second.ns) / 1e9,
                static_cast<double>(rows[i].second.ns) / 1e3 /
                    static_cast<double>(rows[i].second.count));
        }
    };

    std::sort(rows.begin(), rows.end(),
              [](const auto& a, const auto& b) { return a.second.ns > b.second.ns; });
    show("total host time", 18);
    std::sort(rows.begin(), rows.end(),
              [](const auto& a, const auto& b) { return a.second.count > b.second.count; });
    show("call count", 18);
}

} // namespace vitreo::winapi::api_profile
