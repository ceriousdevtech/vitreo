// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
/// @file mscoree_shim.cpp
/// @brief mscoree.dll shim: locating and binding mscoreei.dll (see header).

#include "winapi/system/mscoree/mscoree_shim.hpp"

#include <array>
#include <cstdio>
#include <cstring>

namespace vitreo::winapi::mscoree {

namespace {

#define MSCOREE_EXPORT(ord, name, bytes) ShimExport{ord, name, bytes},
constexpr ShimExport kExportsX86[] = {
#define MSCOREE_EXPORTS_X86
#include "winapi/system/mscoree/mscoree_exports.inc"
#undef MSCOREE_EXPORTS_X86
};
constexpr ShimExport kExportsX64[] = {
#define MSCOREE_EXPORTS_X64
#include "winapi/system/mscoree/mscoree_exports.inc"
#undef MSCOREE_EXPORTS_X64
};
#undef MSCOREE_EXPORT

std::string narrow(const std::u16string& s) {
    std::string out;
    for (char16_t c : s) out.push_back(c < 0x80 ? static_cast<char>(c) : '?');
    return out;
}

/// A Policy\v4.0 value name is a decimal build number.
std::optional<u32> parse_build(const std::u16string& s) {
    if (s.empty() || s.size() > 9) return std::nullopt;
    u32 v = 0;
    for (char16_t c : s) {
        if (c < u'0' || c > u'9') return std::nullopt;
        v = v * 10 + static_cast<u32>(c - u'0');
    }
    return v;
}

}  // namespace

std::span<const ShimExport> shim_exports(bool is_64bit) {
    if (is_64bit) return {kExportsX64, std::size(kExportsX64)};
    return {kExportsX86, std::size(kExportsX86)};
}

ExportRole export_role(const ShimExport& e) {
    if (e.name[0] == '\0') {
        return e.ordinal == 142 ? ExportRole::GetVersionExW : ExportRole::ForwardByOrdinal;
    }
    if (std::strcmp(e.name, "_CorImageUnloading") == 0) return ExportRole::ImageUnloading;
    if (std::strcmp(e.name, "_CorValidateImage") == 0) return ExportRole::ValidateImage;
    return ExportRole::ForwardByName;
}

std::optional<std::u16string> implementation_path(ShimHost& host) {
    std::u16string root;
    if (auto env = host.environment_variable(u"COMPlus_InstallRoot"); env && !env->empty()) {
        root = *env;
    } else {
        root = host.windows_directory() +
               (host.is_64bit() ? u"\\Microsoft.NET\\Framework64\\" : u"\\Microsoft.NET\\Framework\\");
    }
    if (root.back() != u'\\') root.push_back(u'\\');

    std::optional<u32> best;
    for (const auto& name : host.policy_v4_builds()) {
        if (auto b = parse_build(name); b && (!best || *b > *best)) best = b;
    }
    if (!best) return std::nullopt;
    std::u16string build;
    for (char c : std::to_string(*best)) build.push_back(static_cast<char16_t>(c));
    return root + u"v4.0." + build + u"\\mscoreei.dll";
}

MscoreeShim::State MscoreeShim::state() const {
    std::lock_guard lock(mutex_);
    return state_;
}

void MscoreeShim::on_cleanup() {
    std::lock_guard lock(mutex_);
    state_ = State::CleanedUp;
    targets_.clear();
}

void MscoreeShim::on_runtime_string(std::u16string value) {
    std::lock_guard lock(mutex_);
    runtime_string_ = std::move(value);
}

void MscoreeShim::bind(ShimHost& host) {
    const auto path = implementation_path(host);
    if (!path) {
        std::fprintf(stderr,
                     "[vitreo] mscoree: no .NET Framework 4 is installed in this capsule "
                     "(HKLM\\SOFTWARE\\Microsoft\\.NETFramework\\Policy\\v4.0 names no build); "
                     "managed code cannot run (CLR_E_SHIM_RUNTIMEEXPORT)\n");
        state_ = State::Failed;
        return;
    }
    const u64 impl = host.load_library(*path);
    if (impl == 0) {
        std::fprintf(stderr, "[vitreo] mscoree: cannot load the shim implementation %s; "
                     "managed code cannot run (CLR_E_SHIM_RUNTIMEEXPORT)\n", narrow(*path).c_str());
        state_ = State::Failed;
        return;
    }
    const u64 register_cb = host.get_proc_address(impl, "RegisterShimImplCallback");
    const u64 register_cleanup = host.get_proc_address(impl, "RegisterShimImplCleanupCallback");
    const u64 set_instance = host.get_proc_address(impl, "SetShellShimInstance");
    if (register_cb == 0 && register_cleanup == 0) {
        std::fprintf(stderr, "[vitreo] mscoree: %s exports neither RegisterShimImplCallback nor "
                     "RegisterShimImplCleanupCallback; unloading it\n", narrow(*path).c_str());
        host.free_library(impl);
        state_ = State::Failed;
        return;
    }
    if (set_instance != 0) (void)host.call(set_instance, {host.shim_instance()});
    // Bound before the callbacks are registered: mscoreei may call back.
    impl_ = impl;
    state_ = State::Bound;
    if (register_cb != 0) {
        (void)host.call(register_cb, {cleanup_cb_, runtime_string_cb_});
    } else {
        (void)host.call(register_cleanup, {cleanup_cb_});
    }
}

u64 MscoreeShim::forward_target(ShimHost& host, const ShimExport& e) {
    std::lock_guard lock(mutex_);
    if (state_ == State::Unbound) bind(host);
    if (state_ != State::Bound) return 0;
    if (auto it = targets_.find(e.ordinal); it != targets_.end()) return it->second;
    u64 target = 0;
    switch (export_role(e)) {
        case ExportRole::ForwardByName:
            target = host.get_proc_address(impl_, e.name);
            break;
        case ExportRole::ForwardByOrdinal:
            target = host.get_proc_address_ordinal(impl_, e.ordinal);
            break;
        default:
            return 0;
    }
    targets_[e.ordinal] = target;
    return target;
}

}  // namespace vitreo::winapi::mscoree
