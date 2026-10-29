#include "comdlg32.hpp"
#include "winapi/core/marshaler.hpp"
#include "windowing/cocoa/cocoa_bridge.hpp"
#include "capsule/vfs/vfs.hpp"
#include <algorithm>
#include <filesystem>

namespace vitreo::winapi {
namespace {
thread_local u32 dialog_error = 0;

// Count UTF-16 code units rather than UTF-8 bytes for W buffer limits/offsets.
size_t string_units(const std::string& text, bool wide) {
    if (!wide) return text.size();
    size_t units = 0;
    for (unsigned char c : text) {
        if ((c & 0xC0) != 0x80) units += c >= 0xF0 ? 2 : 1;
    }
    return units;
}

std::optional<std::string> guest_dialog_path(capsule::Vfs* vfs, const std::string& host) {
    if (!vfs) return std::nullopt;
    const auto path = std::filesystem::path(host).lexically_normal();
    std::string best;
    size_t best_root = 0;
    for (char drive : vfs->mounted_drives()) {
        const std::string prefix = std::string(1, drive) + ":\\";
        auto root = vfs->resolve_host_path(prefix);
        if (root.is_err()) continue;
        const auto root_path = std::filesystem::path(root.value()).lexically_normal();
        const auto relative = path.lexically_relative(root_path);
        if (relative.empty() || (!relative.empty() && *relative.begin() == "..")) continue;
        if (root_path.string().size() < best_root) continue;
        best_root = root_path.string().size();
        best = prefix + (relative == "." ? "" : relative.generic_string());
        std::replace(best.begin(), best.end(), '/', '\\');
    }
    if (best.empty()) return std::nullopt;
    return best;
}
}

std::optional<std::string> guest_path_for_host(capsule::Vfs* vfs, const std::string& host) {
    return guest_dialog_path(vfs, host);
}

DllExports create_comdlg32_thunks(windowing::CocoaBridge* bridge, capsule::Vfs* vfs,
                                  FileDialogPresenter presenter) {
    DllExports exports;
    auto file_dialog = [bridge, vfs, presenter](Marshaler& m, bool save, bool wide) {
        dialog_error = 0;
        const GuestAddr ofn = m.arg_ptr(0);
        if (!ofn || (!bridge && !presenter)) { m.set_return(0u); return; }
        const bool x64 = m.cpu_is_64bit();
        const auto pointer = [&](u32 off32, u32 off64) {
            return m.read_ptr(ofn + (x64 ? off64 : off32)).value_or(0);
        };
        const auto read_string = [&](GuestAddr ptr) {
            if (!ptr) return std::string{};
            auto r = wide ? m.read_wide_string(ptr) : m.read_ansi_string(ptr);
            return r.is_ok() ? r.value() : std::string{};
        };
        const GuestAddr file = pointer(28, 48);
        const u32 capacity = m.read_u32(ofn + (x64 ? 56 : 32)).value_or(0);
        if (!file || !capacity) { dialog_error = 2; m.set_return(0u); return; }
        FileDialogRequest request{save, read_string(pointer(44, 80)),
            read_string(file), {}, read_string(pointer(48, 88))};
        // Filter entries are description/pattern pairs separated by NULs.
        GuestAddr filter = pointer(12, 24);
        const u32 index_offset = x64 ? 44 : 24;
        u32 index = std::max(1u, m.read_u32(ofn + index_offset).value_or(1));
        for (u32 i = 1; filter && i <= index && i <= 256; ++i) {
            const auto label = read_string(filter);
            if (label.empty()) break;
            filter += (string_units(label, wide) + 1) * (wide ? 2 : 1);
            const auto pattern = read_string(filter);
            if (i == index) request.filter = pattern;
            filter += (string_units(pattern, wide) + 1) * (wide ? 2 : 1);
        }
        if (vfs) {
            auto dir = vfs->resolve_host_path(request.initial_directory.empty()
                ? vfs->cwd() : request.initial_directory);
            if (dir.is_ok()) request.initial_directory = dir.value();
            else request.initial_directory.clear();
        }
        const auto separator = request.default_name.find_last_of("\\/");
        if (separator != std::string::npos) request.default_name.erase(0, separator + 1);
        std::optional<std::string> selected;
        if (presenter) selected = presenter(request);
        else {
            auto result = save
                ? bridge->show_save_file_dialog(request.initial_directory, request.default_name,
                                                request.filter, request.title)
                : bridge->show_open_file_dialog(request.initial_directory, request.filter, request.title);
            if (result.ok) selected = result.selected_path;
        }
        if (!selected) { m.set_return(0u); return; }
        auto guest = guest_dialog_path(vfs, *selected);
        if (!guest) { dialog_error = 0x3002; m.set_return(0u); return; }
        const auto units = string_units(*guest, wide);
        if (units + 1 > capacity) {
            dialog_error = 0x3003; // FNERR_BUFFERTOOSMALL
            (void)m.write_u16(file, static_cast<u16>(units + 1));
            m.set_return(0u);
            return;
        }
        auto written = wide ? m.write_wide_string(file, *guest) : m.write_ansi_string(file, *guest);
        if (written.is_err()) { dialog_error = 0xFFFF; m.set_return(0u); return; }
        const auto last_sep = guest->find_last_of("\\/");
        const auto filename = last_sep == std::string::npos ? 0 : last_sep + 1;
        const auto dot = guest->find_last_of('.');
        const auto extension = dot != std::string::npos && dot >= filename ? dot + 1 : 0;
        (void)m.write_u16(ofn + (x64 ? 100 : 56),
            static_cast<u16>(string_units(guest->substr(0, filename), wide)));
        (void)m.write_u16(ofn + (x64 ? 102 : 58),
            static_cast<u16>(string_units(guest->substr(0, extension), wide)));
        (void)m.write_u32(ofn + index_offset, index);
        m.set_return(1u);
    };
    exports.entries.push_back({"GetOpenFileNameW", 0,
        [file_dialog](Marshaler& m) { file_dialog(m, false, true); }, 4});
    exports.entries.push_back({"GetSaveFileNameW", 0,
        [file_dialog](Marshaler& m) { file_dialog(m, true, true); }, 4});
    exports.entries.push_back({"GetSaveFileNameA", 0,
        [file_dialog](Marshaler& m) { file_dialog(m, true, false); }, 4});
    exports.entries.push_back({"CommDlgExtendedError", 0,
        [](Marshaler& m) { m.set_return(dialog_error); }, 0});

    // short GetFileTitleW(LPCWSTR lpszFile, LPWSTR Buf, WORD cchSize)
    exports.entries.push_back(ExportEntry{
        "GetFileTitleW", 0,
        [](Marshaler& m) {
            GuestAddr file_ptr = m.arg_ptr(0);
            GuestAddr buf_ptr = m.arg_ptr(1);
            u32 cch = static_cast<u32>(m.arg(2) & 0xFFFF);
            if (file_ptr == 0) {
                m.set_return(static_cast<u64>(static_cast<u32>(-1)));
                return;
            }
            auto path = m.read_wide_string(file_ptr, 4096);
            if (path.is_err()) {
                m.set_return(static_cast<u64>(static_cast<u32>(-1)));
                return;
            }
            std::string s = path.value();
            auto pos = s.find_last_of("\\/");
            std::string filename = (pos != std::string::npos) ? s.substr(pos + 1) : s;
            if (buf_ptr != 0 && cch > 0) {
                if (filename.size() + 1 > cch) {
                    m.set_return(static_cast<u64>(static_cast<u32>(filename.size() + 1)));
                    return;
                }
                (void)m.write_wide_string(buf_ptr, filename);
            }
            VITREO_TRACE("[comdlg32] GetFileTitleW('%s') -> '%s'\n", s.c_str(), filename.c_str());
            m.set_return(0);
        },
        12
    });

    // short GetFileTitleA(LPCSTR lpszFile, LPSTR Buf, WORD cchSize)
    exports.entries.push_back(ExportEntry{
        "GetFileTitleA", 0,
        [](Marshaler& m) {
            GuestAddr file_ptr = m.arg_ptr(0);
            GuestAddr buf_ptr = m.arg_ptr(1);
            u32 cch = static_cast<u32>(m.arg(2) & 0xFFFF);
            if (file_ptr == 0) {
                m.set_return(static_cast<u64>(static_cast<u32>(-1)));
                return;
            }
            auto path = m.read_ansi_string(file_ptr, 4096);
            if (path.is_err()) {
                m.set_return(static_cast<u64>(static_cast<u32>(-1)));
                return;
            }
            std::string s = path.value();
            auto pos = s.find_last_of("\\/");
            std::string filename = (pos != std::string::npos) ? s.substr(pos + 1) : s;
            if (buf_ptr != 0 && cch > 0) {
                if (filename.size() + 1 > cch) {
                    m.set_return(static_cast<u64>(static_cast<u32>(filename.size() + 1)));
                    return;
                }
                (void)m.write_ansi_string(buf_ptr, filename);
            }
            VITREO_TRACE("[comdlg32] GetFileTitleA('%s') -> '%s'\n", s.c_str(), filename.c_str());
            m.set_return(0);
        },
        12
    });

    return exports;
}

} // namespace vitreo::winapi
