// SPDX-License-Identifier: LGPL-2.1-only

#include "installer_packages.hpp"

#include "external_app_mapping.hpp"
#include "run_internal.hpp"

#include "run.hpp"
#include "winapi/kernel32/module/system_programs.hpp"

#include <unistd.h>

#include <cctype>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

namespace vitreo::cli {
namespace {

std::string extension_of(std::string_view path) {
    const auto slash = path.find_last_of("/\\");
    const auto dot = path.find_last_of('.');
    if (dot == std::string_view::npos) return {};
    if (slash != std::string_view::npos && dot < slash) return {};
    std::string ext(path.substr(dot + 1));
    for (char& ch : ext) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return ext;
}

} // namespace

InstallerPackagePlan classify_installer_package(std::string_view path) {
    const std::string ext = extension_of(path);
    if (ext == "msi") return {InstallerPackageAction::Install, ""};
    if (ext == "msp") return {InstallerPackageAction::Patch, ""};
    if (ext == "mst") {
        return {InstallerPackageAction::Unsupported,
                "[vitreo] UNIMPLEMENTED installer: a .mst transform is applied "
                "with TRANSFORMS= during an .msi install and is not a program"};
    }
    if (ext == "msm") {
        return {InstallerPackageAction::Unsupported,
                "[vitreo] UNIMPLEMENTED installer: a .msm merge module is used "
                "while building a package and is not a program"};
    }
    if (ext == "msix" || ext == "msixbundle" || ext == "appx" ||
        ext == "appxbundle" || ext == "appinstaller") {
        return {InstallerPackageAction::Unsupported,
                "[vitreo] UNIMPLEMENTED installer: MSIX and AppX packages "
                "(.msix, .msixbundle, .appx, .appxbundle, .appinstaller) "
                "are not installed by this runtime"};
    }
    if (ext == "msu") {
        return {InstallerPackageAction::Unsupported,
                "[vitreo] UNIMPLEMENTED installer: Windows Update packages "
                "(.msu) are not installed by this runtime"};
    }
    return {};
}

std::optional<RunResult> run_installer_package(
    const RunConfig& config, const std::string& capsule_root,
    const std::string& capsule_name, const std::string& resolved_path,
    bool pe_from_image) {
    const InstallerPackagePlan plan = classify_installer_package(
        config.pe_path.empty() ? resolved_path : config.pe_path);
    if (plan.action == InstallerPackageAction::None) return std::nullopt;
    if (plan.action == InstallerPackageAction::Unsupported) {
        return RunResult{1, 0, plan.detail, false};
    }

    std::string win_path;
    ExternalAppMapping mapping;
    if (auto failed = resolve_windows_exe_path(
            config, capsule_root, capsule_name, resolved_path, pe_from_image,
            win_path, mapping)) {
        return failed;
    }

    // Explorer runs `msiexec /i` (`/p` for a patch): the runtime's
    // System32\msiexec.exe system program, a real guest process that runs
    // the engine and loads the package's custom actions. Its image is
    // generated (module/system_programs.hpp); it runs from a private
    // snapshot that execute_pe deletes once read, presented to the guest as
    // its System32 path.
    const std::vector<u8> image = winapi::build_system_program_image(true);
    const char* temp_dir = std::getenv("TMPDIR");
    const std::string snapshot =
        std::string(temp_dir != nullptr && *temp_dir != '\0' ? temp_dir : "/tmp") +
        "/vitreo_launch_msiexec_" + std::to_string(::getpid()) + ".exe";
    {
        std::ofstream out(snapshot, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(image.data()),
                  static_cast<std::streamsize>(image.size()));
        if (!out) {
            return RunResult{1, 0, "Cannot stage msiexec.exe to run " + win_path, false};
        }
    }
    RunConfig msiexec = config;
    msiexec.pe_path = snapshot;
    msiexec.relaunched_as = "C:\\Windows\\System32\\msiexec.exe";
    msiexec.guest_cmdline.clear();
    msiexec.guest_args = {plan.action == InstallerPackageAction::Patch ? "/p" : "/i", win_path};
    msiexec.guest_args.insert(msiexec.guest_args.end(), config.guest_args.begin(),
                              config.guest_args.end());
    return execute_pe(msiexec);
}

} // namespace vitreo::cli
