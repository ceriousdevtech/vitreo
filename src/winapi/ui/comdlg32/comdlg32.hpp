#pragma once

#include "winapi/core/thunk_registry.hpp"
#include <functional>
#include <optional>
#include <string>

namespace vitreo::capsule { class Vfs; }

namespace vitreo::windowing {
class CocoaBridge;
}

namespace vitreo::winapi {

/// Minimal COMDLG32 exports. Installer plugins open these dialogs.
///
/// GetOpenFileNameW / GetSaveFileNameW delegate to NSOpenPanel / NSSavePanel
/// via CocoaBridge when a bridge is provided.
struct FileDialogRequest {
    bool save;
    std::string initial_directory;
    std::string default_name;
    std::string filter;
    std::string title;
};
using FileDialogPresenter = std::function<std::optional<std::string>(const FileDialogRequest&)>;

/// The guest path (drive-letter form) of a host path inside a mounted capsule
/// drive, or nullopt when it lies outside every drive. Shared by the
/// GetOpenFileName family and the Common Item Dialog.
[[nodiscard]] std::optional<std::string> guest_path_for_host(capsule::Vfs* vfs,
                                                           const std::string& host);

[[nodiscard]] DllExports create_comdlg32_thunks(
    windowing::CocoaBridge* bridge = nullptr, capsule::Vfs* vfs = nullptr,
    FileDialogPresenter presenter = {});

} // namespace vitreo::winapi
