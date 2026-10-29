// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// CLI-002: launch policy decided before the guest process exists - elevation and exe path

#include "run_internal.hpp"
#include "external_app_mapping.hpp"
#include "capsule/vfs/image_mount_store.hpp"
#include "core/pe/parser/manifest_info.hpp"
#include "core/pe/parser/pe_parser.hpp"
#include "ipc/process_session.hpp"
#include "platform/macos/elevation_authorization.hpp"
#include "platform/scoped_environment.hpp"
#include "winapi/kernel32/error.hpp"
#include <vitreo/diagnostics.hpp>

#include <mach-o/dyld.h>

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace vitreo::cli {

namespace {

// Minimal quoting suitable for simple cases like "/S": an argument holding
// whitespace or a quote is wrapped in quotes with its quotes escaped.
[[nodiscard]] std::string quote_win_arg(std::string_view arg) {
    bool needs_quotes = false;
    for (char ch : arg) {
        if (ch == ' ' || ch == '\t' || ch == '"') {
            needs_quotes = true;
            break;
        }
    }

    if (!needs_quotes) {
        return std::string(arg);
    }

    std::string out;
    out.reserve(arg.size() + 2);
    out.push_back('"');
    for (char ch : arg) {
        if (ch == '"') {
            out.push_back('\\');
        }
        out.push_back(ch);
    }
    out.push_back('"');
    return out;
}

[[nodiscard]] std::string build_guest_cmdline(
    const std::string& win_exe_path,
    const std::vector<std::string>& guest_args) {
    std::string cmdline;
    cmdline.reserve(win_exe_path.size() + 2 + 1 + 32 * guest_args.size());

    cmdline += "\"";
    cmdline += win_exe_path;
    cmdline += "\"";

    for (const auto& arg : guest_args) {
        cmdline.push_back(' ');
        cmdline += quote_win_arg(arg);
    }

    return cmdline;
}

}  // namespace

std::string capsule_root_for(std::string_view capsule_name) {
    const std::string name = capsule_name.empty()
                                 ? std::string("default")
                                 : std::string(capsule_name);
    const char* home = std::getenv("HOME");
    return home ? std::string(home) + "/.vitreo/capsules/" + name
                : ".vitreo/capsules/" + name;
}

std::string resolve_pe_host_path(const std::string& pe_path,
                                 const std::string& capsule_root,
                                 bool& pe_from_image) {
    std::string resolved_path = pe_path;
    pe_from_image = false;
    if (resolved_path.size() >= 3 &&
        std::isalpha(static_cast<unsigned char>(resolved_path[0])) &&
        resolved_path[1] == ':' &&
        (resolved_path[2] == '\\' || resolved_path[2] == '/')) {
        const char drive = static_cast<char>(std::toupper(
            static_cast<unsigned char>(resolved_path[0])));
        capsule::ImageMountStore mounts(capsule_root);
        for (const auto& record : mounts.list()) {
            if (record.drive == drive) {
                pe_from_image = true;
                break;
            }
        }
        if (!pe_from_image) {
            const char drive_lower = static_cast<char>(std::tolower(
                static_cast<unsigned char>(resolved_path[0])));
            std::string rel = resolved_path.substr(2); // strip "C:"
            for (auto& ch : rel) {
                if (ch == '\\') ch = '/';
            }
            resolved_path = capsule_root + "/drives/" + drive_lower + rel;
        }
    }
    return resolved_path;
}

std::optional<std::string> known_windows_exe_path(
    const RunConfig& config, const std::string& capsule_root,
    const std::string& resolved_path, bool pe_from_image) {
    // If we were relaunched by ProcessSession, the private /tmp snapshot will
    // not match any capsule path. The launcher passes `--relaunched-as
    // <winpath>` carrying the original install path so we can present
    // GetCommandLineW / GetModuleFileNameW / CWD as if we were still running
    // from the install directory.
    if (!config.relaunched_as.empty()) return config.relaunched_as;
    if (pe_from_image) return config.pe_path;

    std::error_code ec;
    const std::string abs_str =
        std::filesystem::absolute(resolved_path, ec).string();
    if (ec) return std::nullopt;
    const std::string drives_root = capsule_root + "/drives/";
    for (char drive = 'a'; drive <= 'z'; ++drive) {
        const std::string drive_root = drives_root + drive;
        if (abs_str == drive_root ||
            (abs_str.rfind(drive_root, 0) == 0 &&
             abs_str.size() > drive_root.size() &&
             abs_str[drive_root.size()] == '/')) {
            std::string rel = abs_str.substr(drive_root.size());
            for (char& ch : rel) {
                if (ch == '/') ch = '\\';
            }
            return std::string(1, static_cast<char>(std::toupper(drive))) +
                   ":" + rel;
        }
    }
    return std::nullopt;
}

std::string guest_command_line(const RunConfig& config,
                               const std::string& win_exe_path) {
    // A CreateProcess-spawned child carries the full Windows command line
    // verbatim, because a browser subprocess needs its exact argv. Otherwise
    // reconstruct an "exe-path [args]" line from the resolved Windows exe path.
    return !config.guest_cmdline.empty()
               ? config.guest_cmdline
               : build_guest_cmdline(win_exe_path, config.guest_args);
}

std::optional<std::string> known_guest_command_line(const RunConfig& config) {
    if (!config.guest_cmdline.empty()) return config.guest_cmdline;
    if (config.pe_path.empty()) return std::nullopt;

    const std::string capsule_root = capsule_root_for(config.capsule_name);
    bool pe_from_image = false;
    const std::string resolved_path =
        resolve_pe_host_path(config.pe_path, capsule_root, pe_from_image);
    const auto win_exe_path = known_windows_exe_path(
        config, capsule_root, resolved_path, pe_from_image);
    if (!win_exe_path) return std::nullopt;
    return guest_command_line(config, *win_exe_path);
}

void reexec_for_scoped_environment(int argc, char* argv[]) {
    // The variable is unset in nearly every run: one getenv.
    if (argc < 2) return;
    if (const char* scoped = std::getenv(platform::kScopedEnvironmentVariable);
        scoped == nullptr || scoped[0] == '\0') {
        return;
    }
    const ParseResult parsed = parse_args(argc, const_cast<const char* const*>(argv));
    if (!parsed.success) return;
    const ParsedArgs& args = parsed.args;
    if (args.command != "run" || args.positional_args.empty()) return;

    RunConfig config;
    assign_launch_identity(config, args);
    // A guest whose command line only exists once its executable has been
    // published into the capsule (a loose executable outside it) cannot be
    // matched yet; deliver_scoped_environment_for_guest reports the values it
    // then applies late.
    const auto command_line = known_guest_command_line(config);
    if (!command_line) return;

    // The path this image was started by, not its canonical target: a guest
    // started through the app-bundle link keeps its bundle identity.
    uint32_t size = 1024;
    std::vector<char> path(size);
    if (_NSGetExecutablePath(path.data(), &size) != 0) {
        path.resize(size);
        if (_NSGetExecutablePath(path.data(), &size) != 0) path.clear();
    }
    platform::reexec_for_scoped_environment(
        *command_line, path.empty() ? nullptr : path.data(), argv, stderr);
}

std::optional<RunResult> resolve_launch_elevation(
    const RunConfig& config, const pe::ParsedPe& pe,
    const std::vector<u8>& pe_data, const std::string& resolved_path,
    std::string& elevation_credential, bool& process_elevated) {
    const bool service_managed =
        config.token_context != ipc::WorkerTokenContext::User;
    if (service_managed && !elevation_credential.empty()) {
        return RunResult{
            static_cast<int>(winapi::error_codes::ERROR_ACCESS_DENIED),
            0,
            "Conflicting guest service and authorization credentials",
            false};
    }
    if (!elevation_credential.empty()) {
        if (config.elevation_authorizer == nullptr ||
            !config.elevation_authorizer->validate_credential(
                elevation_credential)) {
            return RunResult{
                static_cast<int>(
                    winapi::error_codes::ERROR_ACCESS_DENIED),
                0,
                "Invalid guest elevation authorization credential",
                false};
        }
        process_elevated = true;
    }
    const auto requested_level = pe::pe_requested_execution_level(
        std::span<const u8>(pe_data), pe);
    const bool legacy_installer =
        pe::is_legacy_installer_candidate(
            requested_level, pe, config.pe_path);
    if (!service_managed && !process_elevated &&
        (pe::requests_elevation(requested_level) || legacy_installer)) {
        if (config.elevation_authorizer == nullptr) {
            return RunResult{
                static_cast<int>(
                    winapi::error_codes::ERROR_ELEVATION_REQUIRED),
                0,
                "The application requires administrator authorization",
                false};
        }

        std::string requested_name = config.app_name;
        if (requested_name.empty()) {
            if (const char* inherited_name =
                    std::getenv("VITREO_MACOS_APP_NAME")) {
                requested_name = inherited_name;
            }
        }
        const std::string app_name = select_application_display_name(
            requested_name, {}, config.pe_path);
        VITREO_TRACE(
            "[elevation] authorization requested source=%s image='%s'\n",
            legacy_installer ? "legacy-installer-detection" : "manifest",
            config.pe_path.c_str());
        const auto authorization = config.elevation_authorizer->authorize(
            {
                app_name,
                resolved_path,
            },
            elevation_credential);
        if (authorization != platform::ElevationAuthorizationResult::Authorized ||
            elevation_credential.empty()) {
            const bool cancelled =
                authorization ==
                platform::ElevationAuthorizationResult::Cancelled;
            return RunResult{
                static_cast<int>(
                    cancelled ? winapi::error_codes::ERROR_CANCELLED
                              : winapi::error_codes::ERROR_ACCESS_DENIED),
                0,
                cancelled
                    ? "Administrator authorization was cancelled"
                    : "Administrator authorization was denied",
                false};
        }
        process_elevated = true;
    }

    return std::nullopt;
}

std::optional<RunResult> resolve_windows_exe_path(
    const RunConfig& config, const std::string& capsule_root,
    const std::string& capsule_name, const std::string& resolved_path,
    bool pe_from_image, std::string& win_exe_path,
    ExternalAppMapping& app_mapping) {
    if (auto known = known_windows_exe_path(config, capsule_root,
                                            resolved_path, pe_from_image)) {
        // Relaunched from an install path, on a mounted image, or already
        // inside the capsule: there is nothing to publish (and no
        // C:\Vitreo\Apps symlink to create).
        win_exe_path = std::move(*known);
        return std::nullopt;
    }

    // Publish the exe's host directory under C:\Vitreo\Apps so every
    // Win32 API (CreateFileW, GetFileAttributesW, FindFirstFileW,
    // LoadLibraryW, …) can resolve the exe and its siblings — e.g.
    // cards.dll next to sol.exe — through the VFS.
    //
    // The mapping claims its own name: two host folders that share a
    // basename get distinct guest directories, and an unrelated entry
    // already sitting at a candidate name is never removed. That name is
    // decided here, so the guest command line of such a launch cannot be
    // known earlier (known_windows_exe_path).
    const std::filesystem::path native_abs =
        std::filesystem::absolute(resolved_path);
    ExternalAppMappingError mapping_error = ExternalAppMappingError::None;
    app_mapping = ExternalAppMapping::claim(
        capsule_root, native_abs.parent_path().string(), mapping_error);
    if (!app_mapping.is_owned()) {
        return RunResult{
            1, 0,
            std::string("Cannot map the application directory into "
                        "capsule '") + capsule_name + "': " +
                external_app_mapping_error_message(mapping_error),
            false};
    }
    std::filesystem::path native_path(resolved_path);
    const std::string win_exe_name = native_path.filename().string();
    win_exe_path = "C:\\Vitreo\\Apps\\" +
                   app_mapping.guest_directory_name() + "\\" +
                   win_exe_name;

    return std::nullopt;
}

} // namespace vitreo::cli
