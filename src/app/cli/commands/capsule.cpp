#include "capsule.hpp"
#include "ipc/process_registry.hpp"
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace vitreo::cli {

namespace fs = std::filesystem;

namespace {

std::string json_escape(std::string_view value) {
    std::string result;
    result.reserve(value.size() + 8);
    for (const char raw_ch : value) {
        const auto ch = static_cast<unsigned char>(raw_ch);
        switch (ch) {
            case '"': result += "\\\""; break;
            case '\\': result += "\\\\"; break;
            case '\b': result += "\\b"; break;
            case '\f': result += "\\f"; break;
            case '\n': result += "\\n"; break;
            case '\r': result += "\\r"; break;
            case '\t': result += "\\t"; break;
            default:
                if (ch < 0x20) {
                    constexpr char hex[] = "0123456789abcdef";
                    result += "\\u00";
                    result.push_back(hex[ch >> 4]);
                    result.push_back(hex[ch & 0xf]);
                } else {
                    result.push_back(static_cast<char>(ch));
                }
        }
    }
    return result;
}

bool valid_capsule_name(std::string_view name) {
    return !name.empty() && name != "." && name != ".." &&
           name.find('/') == std::string_view::npos &&
           name.find('\\') == std::string_view::npos &&
           name.find("..") == std::string_view::npos;
}

uintmax_t directory_size(const fs::path& path) {
    uintmax_t total = 0;
    std::error_code ec;
    fs::recursive_directory_iterator it(
        path, fs::directory_options::skip_permission_denied, ec);
    const fs::recursive_directory_iterator end;
    while (!ec && it != end) {
        if (it->is_regular_file(ec) && !ec) {
            total += it->file_size(ec);
            ec.clear();
        }
        it.increment(ec);
        if (ec) ec.clear();
    }
    return total;
}

bool capsule_has_processes(const fs::path& path) {
    return !ipc::list_processes(path.string()).empty();
}

bool contains_symlink(const fs::path& root) {
    std::error_code ec;
    fs::recursive_directory_iterator it(
        root, fs::directory_options::skip_permission_denied, ec);
    const fs::recursive_directory_iterator end;
    while (!ec && it != end) {
        if (it->is_symlink(ec) && !ec) return true;
        ec.clear();
        it.increment(ec);
    }
    return false;
}

CommandResult copy_capsule(
    const fs::path& source, const fs::path& destination,
    std::string_view operation) {
    std::error_code ec;
    fs::copy(source, destination, fs::copy_options::recursive |
                                  fs::copy_options::copy_symlinks, ec);
    if (ec) {
        std::error_code cleanup_ec;
        fs::remove_all(destination, cleanup_ec);
        return CommandResult::failure(
            "Failed to " + std::string(operation) + " capsule: " + ec.message());
    }

    // Runtime state describes live host processes and is never portable.
    fs::remove_all(destination / "runtime" / "processes", ec);
    ec.clear();
    fs::create_directories(destination / "runtime" / "processes", ec);
    if (ec) {
        std::error_code cleanup_ec;
        fs::remove_all(destination, cleanup_ec);
        return CommandResult::failure(
            "Failed to initialize imported runtime state: " + ec.message());
    }
    return CommandResult::success();
}

} // namespace

// =============================================================================
// CapsuleManager
// =============================================================================

CapsuleManager::CapsuleManager(const fs::path& base_dir)
    : m_base_dir(base_dir) {}

CommandResult CapsuleManager::create(const std::string& name) {
    if (!valid_capsule_name(name)) {
        return CommandResult::failure("Invalid capsule name");
    }
    auto path = capsule_path(name);

    std::error_code ec;
    if (fs::exists(path, ec)) {
        return CommandResult::failure("Capsule '" + name + "' already exists");
    }

    auto drive_c = path / "drives" / "c";
    if (!fs::create_directories(drive_c, ec) || ec) {
        return CommandResult::failure(
            "Failed to create capsule directory: " + ec.message());
    }

    // Create standard Windows directory structure
    std::vector<fs::path> standard_dirs = {
        // Windows system directories
        drive_c / "Windows",
        drive_c / "Windows" / "System32",
        drive_c / "Windows" / "System32" / "drivers" / "etc",
        drive_c / "Windows" / "SysWOW64",
        drive_c / "Windows" / "Fonts",
        drive_c / "Windows" / "Temp",
        // The Windows Installer's cache (installed packages, icons) and the
        // folder its custom-action DLLs are written to.
        drive_c / "Windows" / "Installer",

        // Program directories. Common Files must exist: an installer probes
        // CSIDL_PROGRAM_FILES_COMMON (0x2b) with GetFileAttributes after
        // SHGetSpecialFolderLocation, and a missing directory fails that
        // substitution with E_FAIL.
        drive_c / "Program Files",
        drive_c / "Program Files" / "Common Files",
        drive_c / "Program Files (x86)",
        drive_c / "Program Files (x86)" / "Common Files",
        drive_c / "ProgramData",
        drive_c / "ProgramData" / "Microsoft" / "Windows" / "Start Menu",
        drive_c / "ProgramData" / "Microsoft" / "Windows" / "Start Menu" / "Programs",
        drive_c / "ProgramData" / "Microsoft" / "Windows" / "Start Menu" / "Programs" / "Startup",

        // Shell folders — installers place shortcuts here; must match the guest
        // paths seeded into HKCU\...\Explorer\Shell Folders (Registry::init_defaults).
        drive_c / "Windows" / "Desktop",
        drive_c / "Windows" / "Start Menu",
        drive_c / "Windows" / "Start Menu" / "Programs",
        drive_c / "Windows" / "Start Menu" / "Programs" / "Startup",

        // User profile directories
        drive_c / "Users",
        drive_c / "Users" / "Public",
        drive_c / "Users" / "Public" / "Desktop",
        drive_c / "Users" / "Default",
        drive_c / "Users" / "Default" / "Desktop",
        drive_c / "Users" / "Default" / "Documents",
        drive_c / "Users" / "Default" / "Downloads",
        drive_c / "Users" / "Default" / "Start Menu",
        drive_c / "Users" / "Default" / "Start Menu" / "Programs",
        drive_c / "Users" / "Default" / "Start Menu" / "Programs" / "Startup",
        drive_c / "Users" / "Default" / "AppData",
        drive_c / "Users" / "Default" / "AppData" / "Local",
        drive_c / "Users" / "Default" / "AppData" / "Local" / "Temp",
        drive_c / "Users" / "Default" / "AppData" / "Roaming",
        drive_c / "Users" / "Default" / "AppData" / "LocalLow",
    };

    for (const auto& dir : standard_dirs) {
        fs::create_directories(dir, ec);
        // Ignore errors - some may already exist or be optional
    }
    // Minimal hosts file. The DNS configuration parser requires
    // this file.
    {
        const auto hosts_path =
            drive_c / "Windows" / "System32" / "drivers" / "etc" / "hosts";
        if (!fs::exists(hosts_path, ec)) {
            std::ofstream hosts(hosts_path);
            if (hosts) {
                hosts << "# Vitreo capsule hosts file.\n"
                         "#\n"
                         "127.0.0.1       localhost\n"
                         "::1             localhost\n";
            }
        }
    }
    fs::create_directories(path / "registry", ec);
    if (ec) {
        return CommandResult::failure(
            "Failed to create capsule registry directory: " + ec.message());
    }
    fs::create_directories(path / "runtime" / "processes", ec);
    if (ec) {
        return CommandResult::failure(
            "Failed to create capsule runtime directory: " + ec.message());
    }

    return CommandResult::success("Created capsule '" + name + "'");
}

std::vector<CapsuleInfo> CapsuleManager::list() const {
    std::vector<CapsuleInfo> result;

    std::error_code ec;
    if (!fs::exists(m_base_dir, ec) || ec) {
        return result;
    }

    for (const auto& entry : fs::directory_iterator(m_base_dir, ec)) {
        if (ec) break;
        if (!entry.is_directory(ec)) continue;

        CapsuleInfo info;
        info.name = entry.path().filename().string();
        info.root_path = entry.path();
        info.has_c_drive = fs::exists(entry.path() / "drives" / "c", ec);
        info.has_registry = fs::exists(entry.path() / "registry", ec);
        result.push_back(std::move(info));
    }

    return result;
}

CommandResult CapsuleManager::destroy(const std::string& name) {
    if (!valid_capsule_name(name)) {
        return CommandResult::failure("Invalid capsule name");
    }
    auto path = capsule_path(name);

    std::error_code ec;
    if (!fs::exists(path, ec) || ec) {
        return CommandResult::failure("Capsule '" + name + "' does not exist");
    }
    if (capsule_has_processes(path)) {
        return CommandResult::failure(
            "Capsule '" + name + "' has running processes");
    }

    fs::remove_all(path, ec);
    if (ec) {
        return CommandResult::failure(
            "Failed to destroy capsule: " + ec.message());
    }

    return CommandResult::success("Destroyed capsule '" + name + "'");
}

CommandResult CapsuleManager::rename(
    const std::string& source, const std::string& destination) {
    if (!valid_capsule_name(source) || !valid_capsule_name(destination)) {
        return CommandResult::failure("Invalid capsule name");
    }
    const auto from = capsule_path(source);
    const auto to = capsule_path(destination);
    std::error_code ec;
    if (!fs::is_directory(from, ec) || ec) {
        return CommandResult::failure("Capsule '" + source + "' does not exist");
    }
    if (capsule_has_processes(from)) {
        return CommandResult::failure(
            "Capsule '" + source + "' has running processes");
    }
    if (fs::exists(to, ec)) {
        return CommandResult::failure("Capsule '" + destination + "' already exists");
    }
    fs::rename(from, to, ec);
    if (ec) {
        return CommandResult::failure("Failed to rename capsule: " + ec.message());
    }
    return CommandResult::success(
        "Renamed capsule '" + source + "' to '" + destination + "'");
}

CommandResult CapsuleManager::clone(
    const std::string& source, const std::string& destination) {
    if (!valid_capsule_name(source) || !valid_capsule_name(destination)) {
        return CommandResult::failure("Invalid capsule name");
    }
    const auto from = capsule_path(source);
    const auto to = capsule_path(destination);
    std::error_code ec;
    if (!fs::is_directory(from, ec) || ec) {
        return CommandResult::failure("Capsule '" + source + "' does not exist");
    }
    if (capsule_has_processes(from)) {
        return CommandResult::failure(
            "Capsule '" + source + "' has running processes");
    }
    if (fs::exists(to, ec)) {
        return CommandResult::failure("Capsule '" + destination + "' already exists");
    }
    const auto copied = copy_capsule(from, to, "clone");
    if (copied.exit_code != 0) return copied;
    return CommandResult::success(
        "Cloned capsule '" + source + "' to '" + destination + "'");
}

CommandResult CapsuleManager::trash(const std::string& name) {
    if (!valid_capsule_name(name)) {
        return CommandResult::failure("Invalid capsule name");
    }
    const auto source = capsule_path(name);
    std::error_code ec;
    if (!fs::is_directory(source, ec) || ec) {
        return CommandResult::failure("Capsule '" + name + "' does not exist");
    }
    if (capsule_has_processes(source)) {
        return CommandResult::failure(
            "Capsule '" + name + "' has running processes");
    }
    const char* home = std::getenv("HOME");
    if (home == nullptr || *home == '\0') {
        return CommandResult::failure("Cannot locate the current user's Trash");
    }
    const auto trash_root = fs::path(home) / ".Trash";
    if (!fs::is_directory(trash_root, ec) || ec) {
        return CommandResult::failure("Cannot access the current user's Trash");
    }
    auto destination = trash_root / ("Vitreo-" + name + ".vitreocapsule");
    for (unsigned suffix = 2; fs::exists(destination, ec) && !ec; ++suffix) {
        destination = trash_root /
            ("Vitreo-" + name + "-" + std::to_string(suffix) + ".vitreocapsule");
    }
    fs::rename(source, destination, ec);
    if (ec) {
        return CommandResult::failure(
            "Failed to move capsule to Trash: " + ec.message());
    }
    return CommandResult::success(
        "Moved capsule '" + name + "' to Trash at " + destination.string());
}

CommandResult CapsuleManager::export_to(
    const std::string& name, const fs::path& destination) {
    if (!valid_capsule_name(name)) {
        return CommandResult::failure("Invalid capsule name");
    }
    const auto source = capsule_path(name);
    std::error_code ec;
    if (!fs::is_directory(source, ec) || ec) {
        return CommandResult::failure("Capsule '" + name + "' does not exist");
    }
    if (capsule_has_processes(source)) {
        return CommandResult::failure(
            "Capsule '" + name + "' has running processes");
    }
    if (contains_symlink(source)) {
        return CommandResult::failure(
            "Capsule contains symbolic links and cannot be safely exported");
    }
    if (destination.empty() || fs::exists(destination, ec)) {
        return CommandResult::failure("Export destination already exists or is invalid");
    }
    const auto copied = copy_capsule(source, destination, "export");
    if (copied.exit_code != 0) return copied;
    return CommandResult::success(
        "Exported capsule '" + name + "' to " + destination.string());
}

CommandResult CapsuleManager::import_from(
    const fs::path& source, const std::string& name) {
    if (!valid_capsule_name(name)) {
        return CommandResult::failure("Invalid capsule name");
    }
    std::error_code ec;
    if (!fs::is_directory(source, ec) || ec ||
        !fs::is_directory(source / "drives" / "c", ec) || ec ||
        !fs::is_directory(source / "registry", ec) || ec) {
        return CommandResult::failure(
            "Import source is not a valid Vitreo capsule");
    }
    if (contains_symlink(source)) {
        return CommandResult::failure(
            "Import source contains symbolic links, which are not allowed");
    }
    const auto destination = capsule_path(name);
    if (fs::exists(destination, ec)) {
        return CommandResult::failure("Capsule '" + name + "' already exists");
    }
    const auto copied = copy_capsule(source, destination, "import");
    if (copied.exit_code != 0) return copied;
    return CommandResult::success(
        "Imported capsule '" + name + "' from " + source.string());
}

CommandResult CapsuleManager::validate(const std::string& name, bool json) const {
    if (!valid_capsule_name(name)) {
        return CommandResult::failure("Invalid capsule name");
    }
    const auto root = capsule_path(name);
    std::error_code ec;
    const bool exists = fs::is_directory(root, ec) && !ec;
    const bool has_c = exists && fs::is_directory(root / "drives" / "c", ec) && !ec;
    const bool has_registry = exists && fs::is_directory(root / "registry", ec) && !ec;
    const bool valid = exists && has_c && has_registry;
    if (json) {
        std::ostringstream out;
        out << "{\"name\":\"" << json_escape(name)
            << "\",\"valid\":" << (valid ? "true" : "false")
            << ",\"has_c_drive\":" << (has_c ? "true" : "false")
            << ",\"has_registry\":" << (has_registry ? "true" : "false")
            << "}";
        return valid ? CommandResult::success(out.str())
                     : CommandResult{1, out.str()};
    }
    if (valid) return CommandResult::success("Capsule '" + name + "' is healthy");
    std::string missing;
    if (!exists) missing = "capsule directory";
    else {
        if (!has_c) missing += "C: drive";
        if (!has_registry) {
            if (!missing.empty()) missing += ", ";
            missing += "registry";
        }
    }
    return CommandResult::failure(
        "Capsule '" + name + "' is invalid; missing " + missing);
}

CommandResult CapsuleManager::info(const std::string& name, bool json) const {
    if (!valid_capsule_name(name)) {
        return CommandResult::failure("Invalid capsule name");
    }
    const auto root = capsule_path(name);
    std::error_code ec;
    if (!fs::is_directory(root, ec) || ec) {
        return CommandResult::failure("Capsule '" + name + "' does not exist");
    }
    const bool has_c = fs::is_directory(root / "drives" / "c", ec) && !ec;
    const bool has_registry = fs::is_directory(root / "registry", ec) && !ec;
    const auto size = directory_size(root);
    if (json) {
        std::ostringstream out;
        out << "{\"name\":\"" << json_escape(name)
            << "\",\"root_path\":\"" << json_escape(root.string())
            << "\",\"has_c_drive\":" << (has_c ? "true" : "false")
            << ",\"has_registry\":" << (has_registry ? "true" : "false")
            << ",\"size_bytes\":" << size
            << ",\"valid\":" << (has_c && has_registry ? "true" : "false")
            << "}";
        return CommandResult::success(out.str());
    }
    std::ostringstream out;
    out << "Name: " << name << "\nPath: " << root.string()
        << "\nC: drive: " << (has_c ? "yes" : "no")
        << "\nRegistry: " << (has_registry ? "yes" : "no")
        << "\nSize: " << size << " bytes";
    return CommandResult::success(out.str());
}

bool CapsuleManager::exists(const std::string& name) const {
    std::error_code ec;
    return fs::exists(capsule_path(name), ec) && !ec;
}

fs::path CapsuleManager::capsule_path(const std::string& name) const {
    return m_base_dir / name;
}

// =============================================================================
// CapsuleCommand
// =============================================================================

CapsuleCommand::CapsuleCommand(CapsuleManager& manager)
    : m_manager(manager) {}

CommandResult CapsuleCommand::execute(const ParsedArgs& args) {
    const auto& sub = args.subcommand;

    if (sub == "create") {
        if (args.positional_args.empty()) {
            return CommandResult::failure("Usage: vitreo capsule create <name>");
        }
        return m_manager.create(args.positional_args[0]);
    }

    if (sub == "list") {
        auto capsules = m_manager.list();
        std::sort(capsules.begin(), capsules.end(),
                  [](const CapsuleInfo& lhs, const CapsuleInfo& rhs) {
                      return lhs.name < rhs.name;
                  });
        if (args.json) {
            std::ostringstream out;
            out << "[";
            for (size_t i = 0; i < capsules.size(); ++i) {
                if (i != 0) out << ",";
                const auto& c = capsules[i];
                out << "{\"name\":\"" << json_escape(c.name)
                    << "\",\"root_path\":\"" << json_escape(c.root_path.string())
                    << "\",\"has_c_drive\":" << (c.has_c_drive ? "true" : "false")
                    << ",\"has_registry\":" << (c.has_registry ? "true" : "false")
                    << "}";
            }
            out << "]";
            return CommandResult::success(out.str());
        }
        if (capsules.empty()) {
            return CommandResult::success("No capsules found.");
        }
        std::string msg;
        for (const auto& c : capsules) {
            msg += c.name;
            if (c.has_c_drive) msg += " [C:]";
            msg += '\n';
        }
        return CommandResult::success(std::move(msg));
    }

    if (sub == "info") {
        if (args.positional_args.empty()) {
            return CommandResult::failure("Usage: vitreo capsule info <name> [--json]");
        }
        return m_manager.info(args.positional_args[0], args.json);
    }

    if (sub == "validate") {
        if (args.positional_args.empty()) {
            return CommandResult::failure("Usage: vitreo capsule validate <name> [--json]");
        }
        return m_manager.validate(args.positional_args[0], args.json);
    }

    if (sub == "rename") {
        if (args.positional_args.size() != 2) {
            return CommandResult::failure(
                "Usage: vitreo capsule rename <source> <destination>");
        }
        return m_manager.rename(args.positional_args[0], args.positional_args[1]);
    }

    if (sub == "clone") {
        if (args.positional_args.size() != 2) {
            return CommandResult::failure(
                "Usage: vitreo capsule clone <source> <destination>");
        }
        return m_manager.clone(args.positional_args[0], args.positional_args[1]);
    }

    if (sub == "trash") {
        if (args.positional_args.size() != 1) {
            return CommandResult::failure("Usage: vitreo capsule trash <name>");
        }
        return m_manager.trash(args.positional_args[0]);
    }

    if (sub == "export") {
        if (args.positional_args.size() != 2) {
            return CommandResult::failure(
                "Usage: vitreo capsule export <name> <destination>");
        }
        return m_manager.export_to(
            args.positional_args[0], args.positional_args[1]);
    }

    if (sub == "import") {
        if (args.positional_args.size() != 2) {
            return CommandResult::failure(
                "Usage: vitreo capsule import <source> <name>");
        }
        return m_manager.import_from(
            args.positional_args[0], args.positional_args[1]);
    }

    if (sub == "destroy") {
        if (args.positional_args.empty()) {
            return CommandResult::failure("Usage: vitreo capsule destroy <name>");
        }
        return m_manager.destroy(args.positional_args[0]);
    }

    return CommandResult::failure(
        "Unknown capsule subcommand: '" + sub +
        "'. Use 'create', 'list', 'info', 'validate', 'rename', 'clone', "
        "'trash', 'export', 'import', or 'destroy'.");
}

} // namespace vitreo::cli
