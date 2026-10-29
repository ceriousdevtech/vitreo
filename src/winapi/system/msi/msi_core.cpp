// SPDX-License-Identifier: LGPL-2.1-only
/// @file msi_core.cpp
/// @brief msi.dll export table and the shared engine framework (msi_core.hpp).

#include "winapi/system/msi/msi_core.hpp"
#include "winapi/system/msi/msi.hpp"
#include "winapi/system/msi/msi_remote.hpp"
#include "winapi/core/marshaler.hpp"

#include <cstdio>
#include <set>
#include <thread>

namespace vitreo::winapi::msi {

u64 HandleTable::current_thread_key() {
    return static_cast<u64>(std::hash<std::thread::id>{}(std::this_thread::get_id()));
}

void log_unimplemented(std::string_view api, std::string_view what) {
    static std::mutex mutex;
    static std::set<std::string> seen;
    std::string key = std::string(api) + ":" + std::string(what);
    std::lock_guard<std::mutex> lock(mutex);
    if (!seen.insert(key).second) return;
    std::fprintf(stderr, "[vitreo] UNIMPLEMENTED msi!%.*s: %.*s\n", static_cast<int>(api.size()),
                 api.data(), static_cast<int>(what.size()), what.data());
}

}  // namespace vitreo::winapi::msi

namespace vitreo::winapi {

namespace {

struct ExportRow {
    const char* name;
    u16 ordinal;
    u16 stdcall_bytes;
};

constexpr ExportRow kExports[] = {
#define MSI_EXPORT(name, ord, bytes) {#name, ord, bytes},
#include "winapi/system/msi/msi_export_table.inc"
#undef MSI_EXPORT
};

}  // namespace

std::vector<std::string> msi_export_names() {
    std::vector<std::string> names;
    for (const ExportRow& row : kExports) names.emplace_back(row.name);
    return names;
}

msi::HandlerMap msi_handlers(MsiManager& mgr) {
    msi::HandlerMap map;
    msi::register_legacy(mgr, map);
    msi::register_database_api(mgr, map);
    msi::register_session_api(mgr, map);
    msi::register_feature_api(mgr, map);
    msi::register_product_api(mgr, map);
    msi::register_product_ops_api(mgr, map);
    msi::register_ui_api(mgr, map);
    // Last: wraps the handlers above for custom-action server proxies.
    msi::register_remote_session_api(mgr, map);
    return map;
}

DllExports create_msi_thunks(MsiManager& mgr) {
    DllExports exports;
    msi::HandlerMap handlers = msi_handlers(mgr);
    for (const ExportRow& row : kExports) {
        msi::Handler fn;
        if (auto it = handlers.find(row.name); it != handlers.end()) {
            fn = std::move(it->second);
            handlers.erase(it);
        } else {
            const std::string name = row.name;
            fn = [name](Marshaler& m) {
                msi::log_unimplemented(name, "export has no implementation");
                m.set_return(MSI_ERROR_CALL_NOT_IMPLEMENTED);
            };
        }
        exports.entries.push_back(ExportEntry{row.name, row.ordinal, std::move(fn), row.stdcall_bytes});
    }
    for (const auto& [name, fn] : handlers) {
        std::fprintf(stderr, "[msi] registrar supplied non-export '%s'\n", name.c_str());
    }
    return exports;
}

}  // namespace vitreo::winapi
